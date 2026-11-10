# qt2wasm

A Qt 6 to WebAssembly toolchain for **WASI**, built on wasi-sdk. It targets
desktop and server modules run by a WASI runtime such as wasmtime, not the
browser: there is no Emscripten here.

Two layers live in this repo:

- **qt6-wasi** is Qt itself: qtbase 6.8.3 cross-compiled for `wasm32-wasip1`
  (static QtCore + QtGui, offscreen QPA), with host `moc`/`rcc`, a CMake
  toolchain file and POSIX shims. Build a Qt program with `qt-cmake` and you get
  an ordinary WASI command module.
- **qt2wasm** is the tool on top: a Python command that turns a Qt class into a
  WebAssembly **component**. It reads the class with moc, generates WIT and C++
  glue from `Q_INVOKABLE`s, properties, signals and enums, links against
  qt6-wasi and wraps the result with wasm-tools and the preview1 adapter.

## Packages

All are linux-64 conda packages on [prefix.dev/nandi-testing](https://prefix.dev/channels/nandi-testing).

| Package | Version | What |
| --- | --- | --- |
| `qt2wasm` | 0.3.0 | the qt2wasm command, CMake module, headers and runtime; depends on everything below |
| `qt6-wasi` | 6.8.3 | Qt for WASI (`$PREFIX/opt/qt6-wasi`) |
| `wasi-sdk` | 27.0 | clang 20, lld, wasi-libc, libc++ (`$PREFIX/opt/wasi-sdk`) |
| `wit-bindgen` | 0.46.0 | upstream binary |
| `wasm-tools` | 1.239.0 | upstream binary |
| `wasi-preview1-adapter` | 36.0.0 | wasmtime's `wasi_snapshot_preview1.reactor.wasm` |
| `wasmtime` | 36.0.0 | the wasmtime CLI |

Nothing is compiled when you install: Qt and the tools are prebuilt in CI.

## Install

With pixi, add the channel and the packages you need:

```toml
[workspace]
channels = ["https://prefix.dev/nandi-testing", "conda-forge"]
platforms = ["linux-64"]

[dependencies]
qt2wasm = "*"     # the whole toolchain
# or just Qt for WASI:
# qt6-wasi = "6.8.3.*"
# wasmtime = "36.*"
```

Activation sets `QT_WASI_PREFIX`, `WASI_SDK_PREFIX`, `QT_WASI_TOOLCHAIN` and the
`QT2WASM_*` variables. Configure a plain Qt project with
`$QT_WASI_PREFIX/bin/qt-cmake -S . -B build -G Ninja`. For components, see
[qt2wasm/README.md](qt2wasm/README.md).

## Working in this repo

Every command is a pixi task in [`pixi.toml`](pixi.toml), and the
[`justfile`](justfile) aliases them (`just test` is `pixi run --frozen test`).
Run `just` for the list. The main ones:

| Command | What |
| --- | --- |
| `just setup` | install the dev env (prebuilt packages only, nothing compiled) |
| `just test` / `just run` | qt2wasm tests; build and run the counter and canvas examples |
| `just example` | build and run `example/` |
| `just build <recipe>` | rattler-build one recipe into `out/` (qt6-wasi is left to CI) |
| `just build-toolchain` / `just build-qt2wasm` / `just build-all` | build the packages locally |
| `just status` | which package versions are already on nandi-testing |
| `just publish` | upload what isn't published yet (needs `PREFIX_TOKEN`); CI does this on push |

## Run the example

[`example/`](example/) is a small QPainter program built against the published
packages:

```sh
cd example
pixi run example
```

This installs qt6-wasi and wasmtime, builds `build/qt6-wasi-example.wasm`, and
runs it with `wasmtime run --dir . ...`. It prints a truecolor preview and writes
`qt6-wasi-example.png`.

## CI

`.forge/jobs.toml` runs on Depot on every push to main:

| Job | What |
| --- | --- |
| `publish-toolchain` | `ci/publish.py`: rattler-build each recipe in `recipes/` and upload it to nandi-testing |
| `publish-qt2wasm` | `qt2wasm/ci/publish_conda.py`: build and upload the qt2wasm package |
| `example` | `ci/example.sh`: build and run `example/` |
| `qt2wasm-check` | `ci/check_qt2wasm.sh`: qt2wasm tests and examples, from source and from the published package |

A package is skipped when its name, version and build number are already
published, so to rebuild one, bump its `build.number` or version. Builds run via
`depot build` with cache mounts (including ccache for Qt), so a Qt rebuild after a
small patch is incremental. Secrets: `PREFIX_TOKEN`, `DEPOT_TOKEN`, `DEPOT_PROJECT_ID`.

## Layout

| Path | What |
| --- | --- |
| `qt2wasm/` | the tool, its examples and tests |
| `recipes/` | rattler-build recipes for the packages above |
| `wasi/` | the qtbase WASI patch, toolchain files, compat shims and configure script |
| `example/` | plain Qt-for-WASI example (no qt2wasm) |
| `ci/`, `.forge/` | CI scripts and jobs |

Limits of Qt on WASI: single-threaded; no `QProcess`, runtime plugins or
network; C++ exceptions abort.
