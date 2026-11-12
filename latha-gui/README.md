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
| `latha:app` | UTF-8 `key=value` lines. `id=` is the Wayland app id / X11 `WM_CLASS` and the desktop file name (`id.desktop`), letters, digits, `.`, `-`, `_` (e.g. `kwi`); `name=` is the window title until the app sets one; `version=` and `update=` are for [updates](#app-updates). |
| `latha:icon` | one image file (PNG; SVG works where Qt Svg is installed). Repeat the section for several sizes. |

The icon becomes the window icon (xdg-toplevel-icon on Wayland, which Plasma
uses for the title bar and task manager; `_NET_WM_ICON` on X11). An app
without one is shown with latha-gui's own icon ([`icons/`](icons/latha-gui.svg))
under the `latha-gui` app id. Custom sections can be appended anywhere in a
module or component, so a build adds them last, after `wac plug` (kwi-wasm's
`scripts/embed-app-meta.py` does). `latha-gui --app-info APP.wasm` prints what
an app carries.

Plasma's task manager does not use the window icon when it finds a desktop
file for the window, and with none for the app id it falls back to the
process, latha-gui, and so to `latha-gui.desktop`. So for an app with an id,
latha-gui writes `~/.local/share/applications/<id>.desktop` (hidden from
menus, `Exec` = latha-gui on that .wasm, `Icon` = the app's icon saved as
`~/.local/share/latha-gui/icons/<id>.png`) when it is missing or out of date,
and runs `kbuildsycoca6` so Plasma sees it before the window opens.
`LATHA_GUI_DESKTOP_ENTRY=0` turns that off; test runs (`--screenshot`,
offscreen) never write it.

## App updates

An app can update itself through latha-gui (and latha-android), with nothing
in its WIT: two more `latha:app` keys, `version=` (dot or dash separated
parts, numbers compared as numbers: `2026.11.12.1730`, `1.4.0-2`) and
`update=` (the https URL of its update manifest). The manifest is `key=value`
lines too:

```
version=2026.11.12.1730
url=webby.wasm.zst
sha256=<SHA-256 of the unpacked .wasm>
```

`url` is absolute or relative to the manifest; a name ending `.zst` is
zstd and unpacked on the way. Once the window is up, latha-gui checks the
manifest on a background thread (at most every `LATHA_GUI_UPDATE_INTERVAL`
seconds, default 6 hours, per app). A newer version is downloaded into
`~/.local/share/latha-gui/updates/<id>/<version>.wasm` and kept only if its
SHA-256 matches, it is wasm, and its own `latha:app` has the same `id=` and the
manifest's `version=`; then its precompiled code is fetched from the prebuilt
stores when they have it (never compiled in the background). From the next
start, `latha-gui APP.wasm` runs that copy when it is newer than `APP.wasm`
(the file you give stays as installed, and a newer package wins over an older
download); `--app-info` says which it runs. Updates come over https only
(http to localhost, for tests). `LATHA_GUI_UPDATE=0` turns checking and
running updates off, `LATHA_GUI_UPDATE_DIR` moves the store, and `latha-run
--update APP.wasm` checks now. [`tools/set-app-info.py`](tools/set-app-info.py)
sets the keys in a built `.wasm` (standard-library Python, so a build can copy
it); [`tests/update.sh`](tests/update.sh) tests it all against a local server.
The code is [`host/src/update.rs`](host/src/update.rs).

`just install latha-gui` also puts the latha-gui icon into the hicolor theme;
the desktop entry and `.wasm` files in the file manager use it.

## How it is built

| Part | What |
| --- | --- |
| `host/` (Rust) | wasmtime 50 + wasmtime-wasi (preview 2, and preview 1 for commands): loads and instantiates the component (`bindgen!` on `gui-pty`, so plain `gui` apps load too), caches compiled code in `~/.cache/latha-gui` (whole components, and Cranelift's per-function cache in `functions/`, so a component composed again with one part changed, e.g. a UI plugged into an unchanged engine with `wac plug`, only compiles that part: 197 MB webby composition 35 s cold, 13 s after a UI change), and has a small C API ([`host/latha_host.h`](host/latha_host.h)). Imports are function pointers into the Qt side. |
| `appmeta.cpp` | the app's id, name and icon (custom sections) |
| `main.cpp` | arguments, the import callbacks (clipboard, primary selection, title, cursor, bell, quit) |
| `window.cpp` | the window (a `QWidget`): event loop pump, input mapping, frame copy |
| `host/src/procs.rs` | latha:proc (processes on threads, channels), WASI commands |
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
  --app-info            print the app's id, name, version, update URL and icons (latha:app, latha:icon) and exit
  --precompile DIR      compile APP.wasm for a prebuilt store into DIR/<key>.cwasm.zst and exit
```

### First run of a big app

Compiling a big component takes Cranelift a while the first time (webby.wasm, 200 MB: 165 s of CPU, 35-50 s
on 4-8 cores; Winch can't do exceptions or GC, and `opt-level=none` only saves 10%). So the first run:

- shows a small window at once ("Getting webby ready", a progress bar; closing it quits), and
- asks the prebuilt stores for the app compiled ahead of time: `<key>.cwasm.zst` (webby: 84 MB, about a second
  on a fast link, then it loads from `~/.cache/latha-gui` in 0.02 s like any cached app). The key is
  (latha-host's version, the portable engine's settings, the app's SHA-256), so a file only ever matches those
  exact bytes and a latha-gui with the same wasmtime and settings. It is compiled for x86-64-v3 (AVX2, BMI2,
  FMA; Haswell and later); a CPU without those fails wasmtime's check and the app is compiled here instead.
- With no store having it, it compiles here as before, with the bar following Cranelift's functions.

A precompiled file is native code, not sandboxed wasm, so latha-gui only fetches from the stores in
`LATHA_GUI_PREBUILT_URL` (space separated, default: webby's `wasm-engine` release, then this repo's `prebuilt`
release), never from anything the app names. `LATHA_GUI_PREBUILT=0` turns fetching off. A release makes the
file with `latha-run --precompile DIR APP.wasm` (or `latha-gui --precompile DIR APP.wasm`) and uploads it;
`latha-run --prebuilt-name APP.wasm` prints the name a store needs. `host/examples/prepare.rs` times a first
run (`LATHA_GUI_CACHE_DIR=$(mktemp -d) cargo run --release --example prepare APP.wasm`).

Environment: `LATHA_GUI_PREBUILT=0`, `LATHA_GUI_PREBUILT_URL` (above), `LATHA_GUI_THEME=dark|light` (else the desktop's color scheme,
followed live), `LATHA_GUI_ACCENT=#rrggbb|none` (else the desktop's accent
color for `host.accent-color`: the XDG portal's `org.freedesktop.appearance
accent-color`, then KDE's `kdeglobals` AccentColor / selection color, both
followed live; our own Qt cannot load KDE's platform theme, so QPalette does
not know it), `LATHA_GUI_OPEN_URL=COMMAND` (runs COMMAND URL for
`host.open-url`, e.g. a particular browser; else QDesktopServices: the XDG
portal in a sandbox, otherwise xdg-open / the desktop's opener),
`LATHA_GUI_WINDOW_SIZE=WxH`, `LATHA_GUI_WASM_STACK_MB=N` (default 256), `LATHA_GUI_CACHE=0`,
`LATHA_GUI_CACHE_DIR`. The app gets our environment minus `QT_*` / `QML*`
(those are for this window's Qt, not the app's). Programs on a pty get our
environment plus what the app passes, `TERM=xterm-256color` unless it sets
one.

Keys are X11 keysyms (Qt's `nativeVirtualKey` on xcb and Wayland), the
scancode is evdev + 8, coordinates are device pixels (`devicePixelRatio`
applied, fractional scales too).

## WASI commands, engine features, network

`latha-gui APP.wasm` also runs a plain WASI command: a core module with
preview 1 (what wasi-sdk's `--target=wasm32-wasip1` links) or a component
that exports `wasi:cli/run`. It runs in the terminal, without Qt or a window,
on its own thread with a 256 MiB wasm stack, with the current directory open
as `.` as well as `/`; its exit code is ours. `latha-run` (built next to
latha-gui, no Qt needed) does only that:

```sh
latha-run [--dir HOST[::GUEST]]... [--no-root] [--no-cache] [--no-network] APP.wasm [ARGS...]
latha-gui js.wasm -c 'print(6 * 7)'      # webby's wasm/ build of Ladybird's js
```

The window's app gets the same 256 MiB wasm stack as commands
(`LATHA_GUI_WASM_STACK_MB=N` to change it): its export calls run on a fiber
with that stack (wasmtime's async call, mapped lazily), still on the GUI
thread, so imports (clipboard, title, ...) are plain Qt calls. LibWeb layout
and LibJS in a window app recurse as deeply as natively (`tests/deep-stack.sh`).

Every engine (app and commands) has wasm exception handling (exnref, which
wasi-sdk's `-mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false
-lsetjmp` emits for setjmp/longjmp), tail calls (`-mtail-call`, clang's
`musttail`), SIMD and GC types on. Programs get `wasi:sockets` (TCP, UDP,
name lookup) as a native program would; `LATHA_GUI_NETWORK=0` turns that off.

## GL: latha:gui/gl

An app can draw with the host's GPU (0.5.0): OpenGL ES 3 calls through the
`gl` interface, [`../qt2wasm/gui/wit/gl.wit`](../qt2wasm/gui/wit/gl.wit)
(latha:gui 0.2.3). The host makes windowless contexts (`create-context`,
`make-current`) and runs each call on the current one; names (buffers,
textures, ...) are the host's, pointers become lists both ways, a pointer that
is an offset into a bound buffer stays a number. webby uses it for WebGL.

- The host loads EGL and GLES when the app makes its first context:
  `LATHA_GUI_GL_EGL=/path/libEGL.so` (and `LATHA_GUI_GL_GLES`) for another
  one, such as ANGLE's; else the system's `libEGL.so.1` / `libGLESv2.so.2`
  (glvnd, Mesa, NVIDIA), surfaceless where EGL can, else on a 1x1 pbuffer.
  `LATHA_GUI_GL=0` turns GL off.
- With ANGLE the host calls its robust entry points (`glTexImage2DRobustANGLE`,
  ...) and passes the app's context attributes, WebGL validation included.
  On another driver it calls the plain ones, pads uploads to what the unpack
  state reads and sizes readbacks and getters itself; shaders then reach the
  driver as the app wrote them (no WebGL shader validation).
- The host sets `LATHA_GUI_GL=0.2.3` in the app's environment. An older
  latha-gui loads such an app but traps on any gl call, so check that first.
- [`gl/`](gl/): `gen.py` makes gl.wit, the host's `host/src/gl_gen.rs` and
  the guest's `latha_gles.c` (the GLES C API over wit-bindgen's bindings for
  the `gl-guest` world) from `gl-functions.json` (Ladybird's WebGL call list
  plus a few). `latha_gles_custom.c` and `latha_egl.c` are the hand-written
  rest of the guest side, `host/src/gl.rs` of the host's.
  [`tests/gl-test.sh`](tests/gl-test.sh) draws a triangle and checks it.

## Processes and channels: latha:proc

For multi-process programs (webby: Ladybird's WebContent, RequestServer and
ImageDecoder), any program can start other wasm programs as processes and talk
to them over channels, [`../qt2wasm/proc/wit/proc.wit`](../qt2wasm/proc/wit/proc.wit)
(components) or the same calls as core imports from the module `latha:proc`,
[`../qt2wasm/proc/latha_proc.h`](../qt2wasm/proc/latha_proc.h) (wasip1 modules):

- `spawn(path, args)`: a new instance of a module or command component (""
  = the caller's own .wasm, relative paths from its directory) on its own
  native thread with a big stack; returns a pid and our end of a channel whose
  other end is the child's `parent()`. `spawn-with-env(path, args, env)` gives
  the child that environment instead of ours (each process passes on the one
  it was started with). `wait-exit`, `kill` (the child traps at its next loop
  or call; checked every 10 ms by the engine's epoch).
- channels: `pair`, `send(h, bytes, handles)` (never blocks; the handles move
  to the receiver, like SCM_RIGHTS), `recv(h, timeout)`, `wait(handles,
  timeout)`, `close`. Closing an end (or the process ending) shows up on the
  other end as `closed` once it has read what was queued.

- shared memory (0.4.0): `memory.create(size)` makes an object (a memfd),
  `map-at(h, offset, addr, len)` makes a page-aligned range of the caller's own
  linear memory the object's bytes (mmap `MAP_FIXED` over it), `unmap-at` gives
  the range its own zeroed pages back. Every process that maps one object sees
  the same bytes, which is how Ladybird's `AnonymousBuffer` (page bitmaps
  between WebContent and the UI) works across instances. Memory handles share
  the endpoints' table: they move in messages, `dup` keeps one of your own,
  `close` closes them, `is-memory` tells them apart. Objects only grow
  (`resize` refuses smaller), so a mapping never runs past the end. To find a
  component's linear memory (the component model does not show the host its
  core memories), every engine makes its memories through a `MemoryCreator`
  that notes their base addresses per instance (`procs::capture_memories`),
  with copy-on-write memory images off.

Each process has its own store and memory; nothing else is shared,
and messages are copied (16 MiB, 256 handles at most). All processes of a run share
one engine and its compiled-code cache. In latha-gui, the window's app may use
latha:proc too: its `tick` runs soon after a message arrives for it (the host's
thread-safe `wake` callback), so it reads its channels with timeout 0 there.
`tests/proc-test.sh` (wasi-sdk 27) checks all of it (shared memory with a
child too) and the two engine features.

Limits: one window per app, no drag and drop, core modules are not wrapped
into components (perry-gui does that with wasm-tools; build a component).
