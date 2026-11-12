# latha-android: run a latha:gui wasm app on Android

The Android counterpart of [latha-gui](../latha-gui/README.md): a separate,
small runner, not a port. One NativeActivity written in Rust (no Java, no Qt);
the app is any component of the **latha:gui** world
([`../qt2wasm/gui/wit/gui.wit`](../qt2wasm/gui/wit/gui.wit)), run on wasmtime
(Cranelift) by the same host crate latha-gui uses
([`../latha-gui/host`](../latha-gui/host), through its C API).

| latha:gui | on Android |
| --- | --- |
| `paint` / `frame-pixels` | damage copied into our frame (moves are `memmove`s), then into the `ANativeWindow` as RGBX |
| `resize` | the activity's content rect (smaller while the soft keyboard is up), scale = density / 160 |
| `pointer` | one finger = left button (enter, move, press, ...; taps within 400 ms count as double clicks); a mouse hovers |
| `wheel` | two-finger drag (pixels + notches), or a mouse wheel |
| `key` | hardware and soft-keyboard keys, X keysyms, text from the key character map; Ctrl+letter gives the control character |
| `text` | dead-key compositions |
| `close` | the Back button (the app may refuse) |
| `prefers-dark` / `color-scheme` | the system's night mode |
| `request-frame` / `request-tick` / `tick` | the loop, as in latha-gui: tick when due, paint at most every 16 ms |
| latha:proc | as latha-gui (the host crate) |

The soft keyboard comes up after a tap that leaves the app showing the `text`
cursor (it does over editable text) and goes away after other taps; the Menu
key also shows it, and a quick three-finger tap toggles it in any app (for
apps that take keys without a text cursor, like the sketch).

Not there yet: `pty` (spawn fails), the system clipboard (the clipboard is
in-process), `open-url`, `accent-color`, IME composition beyond dead keys
(NativeActivity sends keys, not committed text: GameActivity would), and
per-build signing keys (each build makes a new one, so uninstall before
installing a newer APK).

## Which app

`/sdcard/Android/data/uk.nandi.latha/files/app.wasm` if it is there
(`adb push my-app.wasm /sdcard/Android/data/uk.nandi.latha/files/app.wasm`),
else the APK's `assets/app.wasm` (the published APK bundles qt2wasm's sketch
example), with `assets/app.args` (one per line, the name first). The app sees
the activity's files dir as `/data`. Compiled code is cached in the files dir,
so only the first start compiles (a gray screen meanwhile). Logs:
`adb logcat -s latha`; a dark red screen means it could not start.

## Get it

```sh
pixi exec -c https://prefix.dev/nandi-testing -s latha-android -- \
    sh -c 'cp $CONDA_PREFIX/share/latha-android/latha.apk .'
adb install latha.apk
```

arm64-v8a and x86_64 (emulators), Android 11 (API 30) or newer: latha:proc
shared memory needs `memfd_create`.

## Build

CI builds it ([`../recipes/latha-android`](../recipes/latha-android), job
`publish-latha-android`): rustup, `cargo-apk`, the NDK and the SDK parts come
from upstream in `build.sh`. By hand, with `ANDROID_HOME` and
`ANDROID_NDK_ROOT` set and an app in `assets/app.wasm`:

```sh
cargo install cargo-apk --version 0.10.0 --locked
cargo apk build --release      # target/release/apk/latha.apk
```
