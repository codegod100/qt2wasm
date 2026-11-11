// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
#include "appmeta.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>

namespace {

// Unsigned LEB128 at p (< end); advances p, false when truncated or too long.
bool leb(const uchar *&p, const uchar *end, quint64 &v)
{
    v = 0;
    for (int shift = 0; shift < 64 && p < end; shift += 7) {
        const uchar b = *p++;
        v |= quint64(b & 0x7f) << shift;
        if (!(b & 0x80))
            return true;
    }
    return false;
}

} // namespace

AppMeta readAppMeta(const QString &path)
{
    AppMeta m;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return m;
    const qint64 size = f.size();
    const uchar *data = size >= 8 ? f.map(0, size) : nullptr;
    QByteArray copy;
    if (!data && size >= 8) { // not mappable (a pipe, say): read it
        copy = f.readAll();
        data = reinterpret_cast<const uchar *>(copy.constData());
    }
    if (!data || size < 8 || memcmp(data, "\0asm", 4) != 0)
        return m;
    m.valid = true;
    // Only the top level: sections of nested modules/components are theirs.
    const uchar *p = data + 8, *end = data + size;
    while (p < end) {
        const uchar id = *p++;
        quint64 len = 0;
        if (!leb(p, end, len) || len > quint64(end - p))
            break;
        const uchar *body = p, *next = p + len;
        p = next;
        if (id != 0)
            continue;
        quint64 nameLen = 0;
        if (!leb(body, next, nameLen) || nameLen > quint64(next - body))
            continue;
        const QByteArray name(reinterpret_cast<const char *>(body), qsizetype(nameLen));
        body += nameLen;
        const QByteArray payload(reinterpret_cast<const char *>(body), qsizetype(next - body));
        if (name == "latha:icon") {
            if (!payload.isEmpty())
                m.icons << payload;
        } else if (name == "latha:app") {
            for (const QByteArray &line : payload.split('\n')) {
                const qsizetype eq = line.indexOf('=');
                if (eq <= 0)
                    continue;
                const QByteArray key = line.left(eq).trimmed();
                const QString value = QString::fromUtf8(line.mid(eq + 1).trimmed());
                if (key == "id")
                    m.id = value;
                else if (key == "name")
                    m.name = value;
            }
        }
    }
    // An id names a desktop file and a Wayland app id: keep it to the
    // characters those allow (reverse-DNS style), or drop it.
    for (const QChar c : std::as_const(m.id))
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != u'.' && c != u'-' && c != u'_') {
            m.id.clear();
            break;
        }
    return m;
}

QIcon AppMeta::icon() const
{
    QIcon icon;
    for (const QByteArray &bytes : icons) {
        QImage img;
        if (img.loadFromData(bytes) && !img.isNull())
            icon.addPixmap(QPixmap::fromImage(img));
    }
    return icon;
}

QIcon lathaGuiIcon()
{
    QIcon icon;
    for (int s : {16, 32, 48, 256})
        icon.addFile(QStringLiteral(":/latha-gui/latha-gui-%1.png").arg(s), QSize(s, s));
    return QIcon::fromTheme(QStringLiteral("latha-gui"), icon);
}

namespace {

// Writes BYTES to PATH unless it already holds them; true if it wrote.
bool writeIfChanged(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly) && f.readAll() == bytes)
        return false;
    f.close();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path + QStringLiteral(".tmp"));
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size())
        return false;
    out.close();
    QFile::remove(path);
    return out.rename(path);
}

// Desktop Entry Exec quoting: the argument in double quotes, with ", `, $
// and \ escaped (and % doubled, Exec's own escape).
QString execQuote(const QString &arg)
{
    QString q;
    for (const QChar c : arg) {
        if (c == u'"' || c == u'`' || c == u'$' || c == u'\\')
            q += u'\\';
        q += c;
        if (c == u'%')
            q += u'%';
    }
    return u'"' + q + u'"';
}

} // namespace

bool installDesktopEntry(const AppMeta &meta, const QString &wasmPath)
{
    if (meta.id.isEmpty() || meta.id == QLatin1String("latha-gui"))
        return false;
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (data.isEmpty())
        return false;
    // The largest icon, as PNG (whatever format the app embedded).
    QImage best;
    for (const QByteArray &bytes : meta.icons) {
        QImage img;
        if (img.loadFromData(bytes) && img.width() * img.height() > best.width() * best.height())
            best = img;
    }
    bool wrote = false;
    QString iconPath;
    if (!best.isNull()) {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        best.save(&buf, "PNG");
        iconPath = data + QStringLiteral("/latha-gui/icons/") + meta.id + QStringLiteral(".png");
        wrote |= writeIfChanged(iconPath, png);
    }
    const QString name = meta.name.isEmpty() ? meta.id : meta.name;
    // latha-gui from PATH when it is there (the /usr/bin link of `just install`),
    // so runs from different pixi envs do not rewrite the entry each time.
    QString exe = QStandardPaths::findExecutable(QStringLiteral("latha-gui"));
    if (exe.isEmpty())
        exe = QCoreApplication::applicationFilePath();
    const QString entry = QStringLiteral("[Desktop Entry]\n"
                                         "# Written by latha-gui for the latha:gui app %1, so the desktop shows the\n"
                                         "# app's own name and icon for its windows. latha-gui rewrites it when the\n"
                                         "# app changes; delete it freely.\n"
                                         "Type=Application\n"
                                         "Name=%2\n"
                                         "Comment=%3 (a WebAssembly app, run by latha-gui)\n"
                                         "Exec=%4 %5\n"
                                         "Icon=%6\n"
                                         "Terminal=false\n"
                                         "NoDisplay=true\n"
                                         "StartupWMClass=%7\n")
                              .arg(QFileInfo(wasmPath).absoluteFilePath(), name, name,
                                   execQuote(exe), execQuote(QFileInfo(wasmPath).absoluteFilePath()),
                                   iconPath.isEmpty() ? QStringLiteral("latha-gui") : iconPath, meta.id);
    wrote |= writeIfChanged(data + QStringLiteral("/applications/") + meta.id + QStringLiteral(".desktop"),
                            entry.toUtf8());
    if (wrote) {
        // Plasma resolves desktop files through its service cache (ksycoca);
        // kded would notice the new file too, but maybe after our window
        // shows. Elsewhere update-desktop-database is optional.
        for (const QString &tool : {QStringLiteral("kbuildsycoca6"), QStringLiteral("kbuildsycoca5")}) {
            const QString path = QStandardPaths::findExecutable(tool);
            if (path.isEmpty())
                continue;
            QProcess p;
            p.setProcessChannelMode(QProcess::ForwardedErrorChannel);
            p.setStandardOutputFile(QProcess::nullDevice());
            p.start(path, {});
            p.waitForFinished(5000);
            break;
        }
    }
    return wrote;
}
