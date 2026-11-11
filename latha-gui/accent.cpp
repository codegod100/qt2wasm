// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
#include "accent.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

namespace {
const char kService[] = "org.freedesktop.portal.Desktop";
const char kPath[] = "/org/freedesktop/portal/desktop";
const char kIface[] = "org.freedesktop.portal.Settings";
const char kNs[] = "org.freedesktop.appearance";
const char kKey[] = "accent-color";

// The portal's accent-color is (ddd), sRGB in [0, 1]; out of range: unset.
QColor portalColor(const QVariant &v)
{
    QVariant inner = v;
    while (inner.canConvert<QDBusVariant>() && inner.userType() == qMetaTypeId<QDBusVariant>())
        inner = inner.value<QDBusVariant>().variant();
    if (inner.userType() != qMetaTypeId<QDBusArgument>())
        return {};
    const QDBusArgument arg = inner.value<QDBusArgument>();
    double c[3] = {-1, -1, -1};
    arg.beginStructure();
    arg >> c[0] >> c[1] >> c[2];
    arg.endStructure();
    for (double x : c)
        if (x < 0 || x > 1)
            return {};
    return QColor::fromRgbF(float(c[0]), float(c[1]), float(c[2]));
}

// "r,g,b" (kdeglobals) -> color.
QColor rgbList(const QString &s)
{
    const QStringList p = s.split(QLatin1Char(','));
    if (p.size() < 3)
        return {};
    bool ok[3];
    const int r = p[0].trimmed().toInt(&ok[0]), g = p[1].trimmed().toInt(&ok[1]), b = p[2].trimmed().toInt(&ok[2]);
    if (!ok[0] || !ok[1] || !ok[2])
        return {};
    return QColor(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
}
}  // namespace

Accent *Accent::instance()
{
    static Accent *a = new Accent;
    return a;
}

Accent::Accent()
{
    const QByteArray env = qgetenv("LATHA_GUI_ACCENT");
    if (!env.isEmpty()) {
        m_override = true;
        m_color = env == "none" ? QColor() : QColor(QString::fromLatin1(env));
        return;
    }
    m_portal = fromPortal(&m_portalAnswered);
    QDBusConnection::sessionBus().connect(QString(), QLatin1String(kPath), QLatin1String(kIface),
                                          QStringLiteral("SettingChanged"), this,
                                          SLOT(portalChanged(QString, QString, QDBusVariant)));
    m_kdeglobals = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kdeglobals");
    watchKdeglobals();
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { watchKdeglobals(); reload(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { watchKdeglobals(); reload(); });
    reload();
}

QColor Accent::fromPortal(bool *answered) const
{
    *answered = false;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return {};
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath),
                                                    QLatin1String(kIface), QStringLiteral("ReadOne"));
    m << QLatin1String(kNs) << QLatin1String(kKey);
    const QDBusMessage r = bus.call(m, QDBus::Block, 1000);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return {};
    const QColor c = portalColor(r.arguments().first());
    *answered = c.isValid();
    return c;
}

QColor Accent::fromKdeglobals() const
{
    QFile f(m_kdeglobals);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QString group;
    QColor accent, selection;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            group = line.mid(1, line.indexOf(QLatin1Char(']')) - 1);
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString key = line.left(eq).trimmed(), value = line.mid(eq + 1).trimmed();
        if (group == QLatin1String("General") && key == QLatin1String("AccentColor"))
            accent = rgbList(value);
        else if (group == QLatin1String("Colors:Selection") && key == QLatin1String("BackgroundNormal"))
            selection = rgbList(value);
    }
    return accent.isValid() ? accent : selection;
}

void Accent::watchKdeglobals()
{
    // KConfig replaces the file (rename): watch it and its directory.
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (QFileInfo::exists(m_kdeglobals))
        m_watcher.addPath(m_kdeglobals);
    const QString dir = QFileInfo(m_kdeglobals).absolutePath();
    if (m_watcher.directories().isEmpty() && QFileInfo::exists(dir))
        m_watcher.addPath(dir);
}

void Accent::portalChanged(const QString &ns, const QString &key, const QDBusVariant &value)
{
    if (ns != QLatin1String(kNs) || key != QLatin1String(kKey))
        return;
    m_portal = portalColor(QVariant::fromValue(value));
    m_portalAnswered = m_portal.isValid();
    reload();
}

void Accent::reload()
{
    if (m_override)
        return;
    const QColor c = m_portalAnswered ? m_portal : fromKdeglobals();
    if (c == m_color)
        return;
    m_color = c;
    Q_EMIT changed();
}
