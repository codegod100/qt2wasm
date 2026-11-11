// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// The desktop's accent color, for latha:gui's host.accent-color.
//
// latha-gui runs on its own (conda) Qt, which cannot load the desktop's Qt
// platform theme (KDE's plasma-integration), so QPalette knows nothing about
// the accent. We ask the desktop directly, first source that answers wins:
//   1. $LATHA_GUI_ACCENT: #rrggbb, or "none";
//   2. the XDG desktop portal: org.freedesktop.appearance accent-color
//      (Plasma 6, GNOME 47+, ...), followed through SettingChanged;
//   3. KDE's kdeglobals: [General] AccentColor, else the selection color
//      ([Colors:Selection] BackgroundNormal), followed with a file watcher.
#pragma once

#include <QColor>
#include <QDBusVariant>
#include <QFileSystemWatcher>
#include <QObject>

class Accent : public QObject
{
    Q_OBJECT
public:
    static Accent *instance();
    /** Invalid when the desktop has no accent color. */
    QColor color() const { return m_color; }

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void portalChanged(const QString &ns, const QString &key, const QDBusVariant &value);

private:
    Accent();
    void reload();
    QColor fromPortal(bool *answered) const;
    QColor fromKdeglobals() const;
    void watchKdeglobals();

    QColor m_color;
    QColor m_portal;
    bool m_portalAnswered = false;
    bool m_override = false;
    QString m_kdeglobals;
    QFileSystemWatcher m_watcher;
};
