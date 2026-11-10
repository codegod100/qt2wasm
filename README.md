# qt2wasm

Build Qt C++ into WASI WebAssembly components: a Qt CMake project goes in,
a validated `.wasm` component comes out, in one command.

It is not a new compiler. It is a layer over upstream tools: wasi-sdk's clang
compiles, host `moc` reads your classes, qt2wasm generates the WIT and the
C++ glue, `wit-bindgen c` generates the canonical-ABI bindings, `wasm-opt`
shrinks the module and `wasm-tools` wraps it into a component. Qt itself is
the static QtCore + QtGui 6.8.3 WASI build from the `qt6-wasi` conda package
(offscreen platform, no threads, bundled FreeType and HarfBuzz).

Design doc: "qt2wasm: Qt C++ to WASI components" (Claude Doc in the project).

## Install

```sh
pixi global install -c https://prefix.dev/nandi-testing -c conda-forge qt2wasm
```

The conda package carries the `qt2wasm` command, its CMake module, headers and
runtime, and depends on the whole toolchain as prebuilt packages: `qt6-wasi`
(Qt for WASI, host moc, the toolchain file; pulls in `wasi-sdk`), `wit-bindgen`,
`wasm-tools` and `wasi-preview1-adapter` from nandi-testing (built on Depot by
nandi.uk/qt6-wasi, linux-64), plus cmake, ninja, binaryen and fontTools from
conda-forge. Nothing is compiled at install time. It is built by the `qt2wasm-conda` job in `.forge/jobs.toml` on Depot
with pixi-build and published when the version in `pixi.toml` and
`pyproject.toml` is new.

## Quick start: the examples

From a ditherterm checkout, with [pixi](https://pixi.sh) and `just` on PATH
(the `wasm` pixi env brings Python, wasmtime-py, cmake, ninja and Rust):

```sh
pixi run qt2wasm-run    # builds everything it needs, then runs both examples
                        # under wasmtime; writes build/canvas.png
pixi run qt2wasm-test   # codegen tests + the built components
```

These are pixi tasks chained with `depends-on`: `qt2wasm-run` and
`qt2wasm-test` depend on `qt2wasm-examples` (builds `tools/qt2wasm/build/counter.wasm`
and `canvas.wasm`, cached on its inputs). The `wasm` env installs the toolchain
prebuilt (qt6-wasi, wasi-sdk, wit-bindgen, wasm-tools, the adapter), so there
is no Qt build step. The `just qt2wasm-*` recipes are aliases of the same tasks. The canvas example compiles in a font: it uses
DejaVu Sans from the pixi env or the system, or `QT2WASM_FONT=/path/to.ttf`.
`wasm-opt` (binaryen) is optional; put it on PATH for the smaller sizes below.

Your own project: `pixi run qt2wasm build path/to/project -o app.wasm` (add
`--font some.ttf` for QtGui). Without just, run `python -m qt2wasm build ...`
with `PYTHONPATH=tools/qt2wasm`; it finds wasi-sdk, Qt, moc, wit-bindgen,
wasm-tools and the adapter in the active conda/pixi env (see "Tools it looks for").

## Writing a component (Qt-first)

Mark a `QObject` with `QT2WASM_EXPORT` and give it a `Q_INVOKABLE`
constructor. Everything moc can see becomes WIT:

```cpp
#include <qt2wasm/runtime.h>

class Counter : public QObject {
    Q_OBJECT
    QT2WASM_EXPORT("demo:counter/counter@0.1.0")
    Q_PROPERTY(int value READ value WRITE setValue NOTIFY valueChanged)
public:
    enum class Mode { Linear, Doubling };
    Q_ENUM(Mode)
    Q_INVOKABLE explicit Counter(QObject *parent = nullptr);
    Q_INVOKABLE QString describe(const QString &prefix) const;
public slots:
    void step();
signals:
    void valueChanged(int value);
};
```

```cmake
cmake_minimum_required(VERSION 3.21)
project(counter C CXX)
include(${QT2WASM_ROOT}/cmake/Qt2Wasm.cmake)
qt2wasm_add_component(counter SOURCES counter.cpp)      # add GUI for QtGui
```

| Qt declaration | WIT |
| --- | --- |
| `QObject` + `QT2WASM_EXPORT("ns:pkg/iface@ver")` | `interface iface { resource ... }` |
| `Q_INVOKABLE` constructor (a trailing `QObject *parent` is dropped) | `constructor(...)` |
| `Q_INVOKABLE` method, public slot | method (camelCase becomes kebab-case) |
| `Q_PROPERTY(T x READ x WRITE setX)` | `x: func() -> T`, `set-x: func(value: T)` |
| signal | imported `iface-events` function, first argument `source: u32` (the instance's `id()`) |
| `Q_ENUM` / `Q_FLAG` | `enum` / `flags` in the `types` interface |
| `Q_GADGET` with `Q_PROPERTY(... MEMBER ...)` | `record` in `types` |

| C++ | WIT |
| --- | --- |
| `bool`, `int`, `uint`, `qint64`, `float`, `double`, ... | `bool`, `s32`, `u32`, `s64`, `f32`, `f64`, ... |
| `QString` / `QByteArray` / `QStringList` | `string` / `list<u8>` / `list<string>` |
| `QList<T>`, `QVector<T>`, `std::vector<T>` | `list<T>` |
| `std::optional<T>` | `option<T>` |
| `QPoint(F)`, `QSize(F)`, `QRect(F)`, `QColor` | `point(f)`, `size(f)`, `rect(f)`, `rgba` records |
| `QImage` | `image { width, height, pixels: list<u8> }`, RGBA8888 |
| `QVariant`, pointers, overloads | refused, with the member named |

`qt2wasm wit moc_x.cpp.json` prints the WIT without building, and
`qt2wasm glue` writes only the glue, for projects with their own build.

## Writing a component (WIT-first)

`qt2wasm build PROJECT --wit wasm/wit --world ditherterm` skips the WIT and
glue generation and only generates `wit-bindgen c` bindings for the given
world; you define the `exports_*` functions it declares, converting with
`qt2wasm::Conv<T>` from `qt2wasm/convert.h` (strings, lists, QImage, QColor,
records). Generating a Qt-typed C++ interface for an existing WIT is the next
milestone in the design doc.

## How a call runs

A component is a reactor: no `main`, no `exec()`. Each generated export
opens a `qt2wasm::Call`. The first one creates the `QGuiApplication` (or
`QCoreApplication`) with `qt2wasm::WasiEventDispatcher`, a dispatcher that
never blocks, and registers the compiled-in fonts. When the export returns,
the dispatcher delivers posted events, deferred deletes and due timers, so
queued connections and `QTimer::singleShot` work, on that call or a later
one.

## Tools it looks for

| Tool | Where | Tested with |
| --- | --- | --- |
| wasi-sdk | `--wasi-sdk`, `WASI_SDK_PREFIX`, `$CONDA_PREFIX/opt/wasi-sdk` | 27.0 |
| Qt for WASI prefix | `--qt-prefix`, `QT2WASM_QT_PREFIX`, `$CONDA_PREFIX/opt/qt6-wasi/wasi` | 6.8.3 |
| Qt WASI toolchain file | `--qt-toolchain`, `QT2WASM_QT_TOOLCHAIN`, `$CONDA_PREFIX/opt/qt6-wasi/cmake/wasi-qt.cmake` | |
| host moc | `QT2WASM_MOC`, `PATH`, `$CONDA_PREFIX/opt/qt6-wasi/host/libexec/moc`, `/usr/lib/qt6/libexec/moc` | 6.4 and 6.8 (output revision 68) |
| wit-bindgen | `QT2WASM_WIT_BINDGEN`, `PATH`, `$CONDA_PREFIX/bin` | 0.46.0 |
| wasm-tools | `QT2WASM_WASM_TOOLS`, `PATH`, `$CONDA_PREFIX/bin` | 1.239.0 |
| wasm-opt (optional) | `QT2WASM_WASM_OPT`, `PATH` | binaryen 119 |
| preview1 reactor adapter | `QT2WASM_ADAPTER`, `$CONDA_PREFIX/share/wasi-preview1-adapter/` | wasmtime 36 |
| fontTools (optional, subsetting) | Python | 4.x |

## Measured (2026-11-09)

| Example | Core module | After `wasm-opt -Oz` | Component |
| --- | --- | --- | --- |
| counter (QtCore only) | 4.83 MB | 2.31 MB | 2.36 MB |
| canvas (QtGui, QPainter, DejaVu Sans subset 45 KB) | 9.06 MB | 6.45 MB | 6.50 MB |

Both run under wasmtime-py (`just qt2wasm-run`, `just qt2wasm-test`).

## Not done yet

- WIT-first C++ interface generation (Qt types in an abstract class per export).
- Denying unused WASI imports (filesystem, cli) with WASI-Virt.
- Wizer pre-initialisation, LTO-built Qt, a wasip2-native link.
