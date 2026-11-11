# latha-gui: run any latha:gui wasm app in a Qt window

`wasmtime run`, but with a window. Point `latha-gui` at a component that
implements the **latha:gui** world ([`../qt2wasm/gui/wit/gui.wit`](../qt2wasm/gui/wit/gui.wit),
`gui` or `gui-pty`) and it opens a native Qt 6 window, sends the app its
keyboard, mouse, wheel, focus and IME input, and shows the frames it paints.
The name says what it loads: components of the `latha:` namespace.

```sh
latha-gui path/to/app.wasm [args...]
pixi global install -c conda-forge -c https://prefix.dev/nandi-testing latha-gui   # the prebuilt
```

`just install latha-gui` (from the qt2wasm checkout) installs the prebuilt,
links it into /usr/bin, and installs [`latha-gui.desktop`](latha-gui.desktop)
(plus an `application/wasm` MIME definition for older shared-mime-info), so
`.wasm` files open with latha-gui from the file manager's Open With; it becomes
the default for `.wasm` unless you already chose one.

It ships as the conda package `latha-gui` on prefix.dev/nandi-testing
([`../recipes/latha-gui`](../recipes/latha-gui/recipe.yaml), built and
published by the `publish-toolchain` forge job like the other recipes; bump
its version to publish a new one). From this checkout: `bash latha-gui/build.sh`
(needs Qt 6, Rust, CMake and a C++ compiler, e.g. conda-forge `qt6-main rust
cmake ninja cxx-compiler`) writes `latha-gui/build/latha-gui`;
`bash latha-gui/smoke.sh APP.wasm` is a headless check (with
`LATHA_GUI=latha-gui/build/latha-gui` for the local build). kwi-wasm's
`just gui` opens its terminal app (gui/app) in it.

## App id, name and icon

An app names itself and brings its icon in two custom sections at the top
level of its `.wasm` (read by [`appmeta.cpp`](appmeta.cpp) before the app
loads; no WIT import, so any latha-gui runs any app):

| Section | Holds |
| --- | --- |
| `latha:app` | UTF-8 `key=value` lines. `id=` is the Wayland app id / X11 `WM_CLASS` and the desktop file name (`id.desktop`), letters, digits, `.`, `-`, `_` (e.g. `kwi`); `name=` is the window title until the app sets one. |
| `latha:icon` | one image file (PNG; SVG works where Qt Svg is installed). Repeat the section for several sizes. |

The icon becomes the window icon (xdg-toplevel-icon on Wayland, which Plasma
uses for the title bar and task manager; `_NET_WM_ICON` on X11). An app
without one is shown with latha-gui's own icon ([`icons/`](icons/latha-gui.svg))
under the `latha-gui` app id. Custom sections can be appended anywhere in a
module or component, so a build adds them last, after `wac plug` (kwi-wasm's
`scripts/embed-app-meta.py` does). `latha-gui --app-info APP.wasm` prints what
an app carries.

`just install latha-gui` also puts the latha-gui icon into the hicolor theme;
the desktop entry and `.wasm` files in the file manager use it.

## How it is built

| Part | What |
| --- | --- |
| `host/` (Rust) | wasmtime 50 + wasmtime-wasi (preview 2): loads and instantiates the component (`bindgen!` on `gui-pty`, so plain `gui` apps load too), caches compiled code in `~/.cache/latha-gui`, and has a small C API ([`host/latha_host.h`](host/latha_host.h)). Imports are function pointers into the Qt side. |
| `appmeta.cpp` | the app's id, name and icon (custom sections) |
| `main.cpp` | arguments, the import callbacks (clipboard, primary selection, title, cursor, bell, quit) |
| `window.cpp` | the window (a `QWidget`): event loop pump, input mapping, frame copy |
| `latha_pty.cpp` | `latha:gui/pty`: `forkpty`, output read on the Qt loop |
| `build.sh` | `cargo build` the host, then CMake the window: `build/latha-gui` |

Speed, as in perry-gui: frames cross as base64 (`frame-pixels` is a string,
which wasmtime lifts in one copy) and are decoded straight into the window's
`QImage` (premultiplied BGRA = `Format_ARGB32_Premultiplied`, so no
conversion); only the damaged rectangles are copied, and a `move` (scrolling)
shifts the pixels already shown with a `memmove` a row. No fuel metering.
At 2700x1650 with kwi (kwi-wasm's gui/app): typing ~1 ms a key, a 3-row scroll ~13 ms, a full
frame ~60-80 ms (perry-gui: ~6, ~20, ~140 ms).

## Options

```
latha-gui [options] APP.wasm [args...]
  --dir HOST[::GUEST]   let the app open HOST (as GUEST, default the same path)
                        (/ is always open as /, unless --no-root)
  --no-root             do not give the app the whole file system (/)
  --size WxH            window size in logical pixels (else what the app asks for)
  --no-cache            compile the app again
  --screenshot FILE     save the app's frame to FILE and quit (with --quit-after)
  --quit-after MS       quit after MS milliseconds
  --type TEXT           type TEXT (\n = Return) once the app runs (tests)
  --debug               paint timings on stderr (LATHA_GUI_DEBUG=1)
  --verify              check every scroll move against a full copy (LATHA_GUI_VERIFY=1)
  --app-info            print the app's id, name and icons (latha:app, latha:icon) and exit
```

Environment: `LATHA_GUI_THEME=dark|light` (else the desktop's color scheme,
followed live), `LATHA_GUI_ACCENT=#rrggbb|none` (else the desktop's accent
color for `host.accent-color`: the XDG portal's `org.freedesktop.appearance
accent-color`, then KDE's `kdeglobals` AccentColor / selection color, both
followed live; our own Qt cannot load KDE's platform theme, so QPalette does
not know it), `LATHA_GUI_WINDOW_SIZE=WxH`, `LATHA_GUI_CACHE=0`,
`LATHA_GUI_CACHE_DIR`. The app gets our environment minus `QT_*` / `QML*`
(those are for this window's Qt, not the app's). Programs on a pty get our
environment plus what the app passes, `TERM=xterm-256color` unless it sets
one.

Keys are X11 keysyms (Qt's `nativeVirtualKey` on xcb and Wayland), the
scancode is evdev + 8, coordinates are device pixels (`devicePixelRatio`
applied, fractional scales too).

Limits: one window per app, no drag and drop, core modules are not wrapped
into components (perry-gui does that with wasm-tools; build a component).
