# qt2wasm

Build Qt C++ into WASI WebAssembly components, plus the prebuilt toolchain that
needs: Qt for WASI and the wasm tools as conda packages on
[prefix.dev/nandi-testing](https://prefix.dev/channels/nandi-testing). Everything
heavy is built once in CI on Depot; installing only downloads packages.

## qt6-wasi vs qt2wasm

They live together here, but they are separate conda packages at different layers:

- **qt6-wasi** (`recipes/qt6-wasi`) is **Qt itself**: qtbase 6.8.3 cross-compiled
  for `wasm32-wasip1` (static QtCore + QtGui, offscreen platform), the native
  host `moc`/`rcc`, a CMake toolchain file and POSIX shims. It is a library
  package, like `qt6-main` on conda-forge, just for WASI. You use it with plain
  CMake (`qt-cmake`) and get an ordinary WASI module (a command with `main`),
  as in [`example/`](example/). It knows nothing about components or WIT. Its
  siblings in `recipes/` (wasi-sdk, wit-bindgen, wasm-tools, the preview1
  adapter, the wasmtime CLI) are repackaged upstream tools.
- **qt2wasm** (`qt2wasm/`) is **the tool on top**: a Python command that takes a
  Qt CMake project and produces a WebAssembly **component**. It reads your
  classes with moc, generates the WIT interface and the C++ glue from
  `Q_INVOKABLE`s, properties, signals and enums, runs `wit-bindgen`, links
  against qt6-wasi, shrinks with `wasm-opt` and wraps the result with
  `wasm-tools` and the preview1 adapter. Its runtime gives each component a
  non-blocking Qt event loop. The qt2wasm package depends on qt6-wasi and the
  tools above, so installing it brings the whole toolchain.

In short: use **qt6-wasi** to compile Qt code for WASI yourself, and **qt2wasm**
to turn a Qt class into a component other languages can call. The Qt package is
slow to build (~30 min) and rarely changes; the tool is pure Python and changes
often. That is why they publish independently: a qt2wasm change never rebuilds Qt.

| Package | Version | What | Installs |
| --- | --- | --- | --- |
| `qt2wasm` | 0.3.0 | the qt2wasm command, its CMake module, headers and runtime (noarch) | `$PREFIX/bin/qt2wasm`, Python package |
| `qt6-wasi` | 6.8.3 | static QtCore + QtGui (offscreen QPA, bundled FreeType/HarfBuzz/libpng/zlib) for wasm32-wasip1, with the WASI patches; native host Qt tools (moc, rcc); the `wasi-qt.cmake` toolchain and POSIX compat shims | `$PREFIX/opt/qt6-wasi/{wasi,host,cmake,compat}` |
| `wasi-sdk` | 27.0 | upstream wasi-sdk (clang 20, lld, wasi-libc, libc++) | `$PREFIX/opt/wasi-sdk` |
| `wit-bindgen` | 0.46.0 | upstream release binary | `$PREFIX/bin` |
| `wasm-tools` | 1.239.0 | upstream release binary | `$PREFIX/bin` |
| `wasi-preview1-adapter` | 36.0.0 | wasmtime's `wasi_snapshot_preview1.reactor.wasm` | `$PREFIX/share/wasi-preview1-adapter/` |
| `wasmtime` | 36.0.0 | the wasmtime CLI, upstream release binary | `$PREFIX/bin` |

`wac-cli` 0.12.0 comes from the same channel (built by nandi.uk/qtdev). All linux-64.


## Layout

| Path | What |
| --- | --- |
| [`qt2wasm/`](qt2wasm/) | the qt2wasm tool, its examples and tests ([README](qt2wasm/README.md)); `qt2wasm/pixi.toml` is its pixi-build package |
| `recipes/` | rattler-build recipes for qt6-wasi, wasi-sdk, wit-bindgen, wasm-tools, wasi-preview1-adapter, wasmtime |
| `wasi/` | the WASI patch set and toolchain files the qt6-wasi recipe builds with |
| [`example/`](example/) | a plain Qt-for-WASI program (no qt2wasm) built against the published qt6-wasi |
| `pixi.toml` | dev env for qt2wasm: `pixi run test`, `pixi run run`, `pixi run qt2wasm ...` |
| `ci/`, `.forge/jobs.toml` | CI on Depot: publish the packages, build the example, test qt2wasm |

History: qt6-wasi came from nandi.uk/qt6-wasi and qt2wasm from ditherterm's
`tools/qt2wasm`; both histories are kept here.

## Example (qt6-wasi without qt2wasm)

[`example/`](example/) is a small QtGui program (QPainter gradients and paths into a
`QImage`, saved as PNG, with a truecolor preview printed to the terminal), built
against the published packages. It is a plain WASI command (`main`), so it runs
with the wasmtime CLI (the `wasmtime` package from `recipes/`, since conda-forge
only has wasmtime-py):

```sh
cd example
pixi run example    # installs qt6-wasi + wasmtime from nandi-testing, builds, runs:
                    # wasmtime run --dir . build/qt6-wasi-example.wasm qt6-wasi-example.png
```

`--dir .` preopens the current directory so the program can write the PNG.
Raster painting needs no `QGuiApplication` or QPA plugin. The `example` forge job
builds and runs it on every push.

## Use qt6-wasi directly

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

## CI: build and publish

`.forge/jobs.toml` runs four jobs on Depot on every push to main:

| Job | What | Skips when |
| --- | --- | --- |
| `publish-toolchain` | `ci/publish.py`: rattler-build each recipe in `recipes/`, upload to nandi-testing | that name + version + build number is already published |
| `publish-qt2wasm` | `qt2wasm/ci/publish_conda.py`: pixi-build the qt2wasm package, upload (after the toolchain) | that qt2wasm version is already published |
| `example` | `ci/example.sh`: build `example/` and run it with the wasmtime CLI | never (about a minute) |
| `qt2wasm-check` | `ci/check_qt2wasm.sh`: qt2wasm tests and examples from the checkout, then with the published package | never |

The forge has no path filters and checks out without `.git`, so "only build
what changed" works by version: bump `build.number` (or `version`) in a recipe,
or the version in `qt2wasm/pixi.toml` + `qt2wasm/pyproject.toml`, to publish
again. Everything else is skipped.

**Incremental builds.** The publish jobs run their builds with `depot build`
(ci/build.Dockerfile, qt2wasm/ci/Dockerfile) and BuildKit cache mounts, which
Depot keeps between runs: the conda package caches, rattler-build's source
cache (the qtbase tarball) and **ccache** for the Qt build (`recipes/qt6-wasi/build.sh`
turns it on when the cache is mounted, with paths made relative so each build's
work dir still hits). The first Qt build after this was set up fills the cache;
from then on a rebuild after a small patch change mostly comes from ccache instead
of a full ~30 min compile. The job log ends with ccache's
hit/miss counts. Without the Depot CLI, `ci/publish.py` builds in place, uncached.

Locally (needs the qtbase tarball to be reachable):

```sh
rattler-build build -r recipes/qt6-wasi -c https://prefix.dev/nandi-testing -c conda-forge
```

Build notes: the static Qt bakes its install prefix into a 256-byte buffer, which
conda's placeholder prefix overflows, so the wasm Qt is installed to a short staging
path and moved into `$PREFIX`; conda drops empty directories, so the wasi-sdk recipe
keeps them (clang only finds libc++ when `include/c++/v1` exists).
