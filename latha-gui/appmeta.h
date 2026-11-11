// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// What an app says about itself, in custom sections at the top level of its
// .wasm (a component or a core module), read before the app is loaded:
//
//   latha:app    UTF-8 lines `key=value`:
//                  id=   the app id: the Wayland app id / X11 WM_CLASS and the
//                        desktop file name (id.desktop), e.g. `kwi`
//                  name= the name shown until the app sets a title
//   latha:icon   one image (PNG, or anything Qt reads: SVG with Qt Svg);
//                repeat the section for several sizes
//
// Both are optional. Without an icon the window gets latha-gui's own.
#pragma once

#include <QByteArray>
#include <QIcon>
#include <QList>
#include <QString>

struct AppMeta {
    QString id;
    QString name;
    QList<QByteArray> icons; // image files, as embedded
    bool valid = false;      // the file is wasm

    // The embedded icons as one QIcon (null when none decodes).
    QIcon icon() const;
};

// Reads the sections of PATH; errors leave the fields empty.
AppMeta readAppMeta(const QString &path);

// Plasma's task manager takes a window's icon from the desktop file that
// matches its app id, and when there is none it matches the process
// (latha-gui) to latha-gui.desktop, so the app would show latha-gui's icon.
// For an app with an id this writes ~/.local/share/applications/<id>.desktop
// (NoDisplay, Exec = latha-gui on this wasm) and its icon to
// ~/.local/share/latha-gui/icons/<id>.png, when they are missing or changed,
// and then refreshes Plasma's service cache. Returns true if it wrote.
bool installDesktopEntry(const AppMeta &meta, const QString &wasmPath);

// latha-gui's own icon: the theme's latha-gui icon, else the built-in one.
QIcon lathaGuiIcon();
