// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
#include "appmeta.h"

#include <QFile>
#include <QImage>
#include <QPixmap>

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
