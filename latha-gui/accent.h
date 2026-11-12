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
//
// Light or dark (host.prefers-dark) comes the same way, for the same reason: conda Qt's QStyleHints::colorScheme
// is often Unknown on KDE. The portal's org.freedesktop.appearance color-scheme (1 dark, 2 light, 0 no preference),
// else kdeglobals' [Colors:Window] BackgroundNormal (dark when its lightness is under half); neither: Qt's answer.
#pragma once

#include <optional>

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
    /** Whether the desktop is dark, if it says. */
    std::optional<bool> dark() const { return m_dark; }

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void portalChanged(const QString &ns, const QString &key, const QDBusVariant &value);

private:
    Accent();
    void reload();
    QColor fromPortal(bool *answered) const;
    QColor fromKdeglobals(std::optional<bool> *dark) const;
    std::optional<bool> schemeFromPortal() const;
    void watchKdeglobals();

    QColor m_color;
    QColor m_portal;
    std::optional<bool> m_dark;
    std::optional<bool> m_portalDark;
    bool m_portalAnswered = false;
    bool m_override = false;
    QString m_kdeglobals;
    QFileSystemWatcher m_watcher;
};
