# qt6-wasi: Qt for WASI as conda packages

Prebuilt **qtbase 6.8.3 for `wasm32-wasip1`** plus the tools to use it, published to
[prefix.dev/nandi-testing](https://prefix.dev/channels/nandi-testing). conda-forge only
ships qtbase (`qt6-main`) for native platforms, and building Qt for WASI takes ~30 min,
so it happens once here, in CI, instead of on every machine.

| Package | Version | What | Installs |
| --- | --- | --- | --- |
| `qt6-wasi` | 6.8.3 | static QtCore + QtGui (offscreen QPA, bundled FreeType/HarfBuzz/libpng/zlib) for wasm32-wasip1, with the WASI patches; native host Qt tools (moc, rcc); the `wasi-qt.cmake` toolchain and POSIX compat shims | `$PREFIX/opt/qt6-wasi/{wasi,host,cmake,compat}` |
| `wasi-sdk` | 27.0 | upstream wasi-sdk (clang 20, lld, wasi-libc, libc++) | `$PREFIX/opt/wasi-sdk` |
| `wit-bindgen` | 0.46.0 | upstream release binary | `$PREFIX/bin` |
| `wasm-tools` | 1.239.0 | upstream release binary | `$PREFIX/bin` |
| `wasi-preview1-adapter` | 36.0.0 | wasmtime's `wasi_snapshot_preview1.reactor.wasm` | `$PREFIX/share/wasi-preview1-adapter/` |

`wac-cli` 0.12.0 comes from the same channel (built by nandi.uk/qtdev). All linux-64.

## Use

```toml
[workspace]
channels = ["https://prefix.dev/nandi-testing", "conda-forge"]
platforms = ["linux-64"]

[dependencies]
qt6-wasi = "6.8.3.*"        # pulls in wasi-sdk, cmake, ninja
wit-bindgen = "*"
wasm-tools = "*"
wasi-preview1-adapter = "*"
```

Activation sets `WASI_SDK_PREFIX`, `QT_WASI_PREFIX`, `QT_WASI_HOST_PATH`, `QT_WASI_TOOLCHAIN`,
and the `QT2WASM_*` variables qt2wasm reads (`QT2WASM_QT_PREFIX`, `QT2WASM_QT_TOOLCHAIN`,
`QT2WASM_MOC`, `QT2WASM_ADAPTER`). Configure a Qt project with

```sh
$QT_WASI_PREFIX/bin/qt-cmake -S . -B build -G Ninja
```

and add `${WASI_QT_COMPAT_C}` (set by the toolchain) to the final module's sources. See
`recipes/qt6-wasi/test/` for the smallest working project, and ditherterm's
`wasm/qt/guest` or qt2wasm for components.

## What it takes to run Qt on WASI

`wasi/` holds everything the cross build needs:

- `cmake/wasi-qt.cmake`: wraps wasi-sdk's `wasi-sdk-p1.cmake`, forces `UNIX=1` (our
  `Platform/WASI.cmake`, plus `wasi-unix.cmake` for CMake 4, which ships its own WASI
  platform file), force-includes `compat/wasi_compat.h`, links wasi-libc's emulated
  signal/mman/process-clock/getpid libraries.
- `mkspecs/wasi-clang`: a minimal POSIX `qplatformdefs.h`.
- `patches/qtbase-6.8.3-wasi.patch`: `Q_OS_WASI` detection, no thread wake-up pipe in the
  unix event dispatcher, and setjmp-free overflow handling in `qgrayraster.c` / FreeType
  `ftgrays.c` (wasi-libc has no `setjmp`/`longjmp` without the EH proposal).
- `compat/`: stubs and headers wasi-libc lacks (`geteuid`, `dup2`, `pipe`, `sys/wait.h`,
  `pwd.h`, a trapping `setjmp.h`, ...); `wasi_compat.c` is linked into the final module.
- `configure-wasi.sh`: the qtbase feature set (static, no threads, no processes,
  dlopen, sockets or display server).

Limits: single-threaded; no `QProcess`, plugins at runtime, network or filesystem
preopens; PNG decode errors trap; C++ exceptions abort.

## Build and publish

Every push to main runs `ci/publish.py` on Depot (`.forge/jobs.toml`): each recipe whose
version + build number is not on nandi-testing yet is built with rattler-build and
uploaded right away. To publish a change, bump `build.number` (or `version`) in the
recipe. Locally (needs the qtbase tarball to be reachable):

```sh
rattler-build build -r recipes/qt6-wasi -c https://prefix.dev/nandi-testing -c conda-forge
```

Build notes: the static Qt bakes its install prefix into a 256-byte buffer, which
conda's placeholder prefix overflows, so the wasm Qt is installed to a short staging
path and moved into `$PREFIX`; conda drops empty directories, so the wasi-sdk recipe
keeps them (clang only finds libc++ when `include/c++/v1` exists).
