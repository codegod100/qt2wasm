# qt2wasm

Build Qt C++ into WASI WebAssembly components: a Qt CMake project goes in,
a validated `.wasm` component comes out, in one command.

It is not a new compiler. It is a layer over upstream tools: wasi-sdk's clang
compiles, host `moc` reads your classes, qt2wasm generates the WIT and the
C++ glue, `wit-bindgen c` generates the canonical-ABI bindings, `wasm-opt`
shrinks the module and `wasm-tools` wraps it into a component. Qt itself is
the static QtCore + QtGui 6.8.3 WASI build from `wasm/qt` (offscreen
platform, no threads, bundled FreeType and HarfBuzz).

Design doc: "qt2wasm: Qt C++ to WASI components" (Claude Doc in the project).

## Install

```sh
pixi global install -c https://prefix.dev/nandi-testing -c conda-forge qt2wasm
```

The conda package carries the `qt2wasm` command, its CMake module, headers and
runtime, plus cmake, ninja, binaryen and fontTools. Bring wasi-sdk, the Qt for
WASI prefix, host moc, wit-bindgen and wasm-tools yourself (see "Tools it looks
for"). It is built by the `qt2wasm-conda` job in `.forge/jobs.toml` on Depot
with pixi-build and published when the version in `pixi.toml` and
`pyproject.toml` is new.

## Quick start

```sh
pip install -e tools/qt2wasm          # or: PYTHONPATH=tools/qt2wasm
python -m qt2wasm build tools/qt2wasm/examples/counter -o counter.wasm
python tools/qt2wasm/tests/run_counter.py counter.wasm
```

For a QtGui project, pass the fonts to compile in (subset to Latin plus the
block and braille characters by default):

```sh
python -m qt2wasm build tools/qt2wasm/examples/canvas \
    --font /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf -o canvas.wasm
python tools/qt2wasm/tests/run_canvas.py canvas.wasm canvas.png
```

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
| wasi-sdk | `--wasi-sdk`, `WASI_SDK_PREFIX`, `wasm/qt/.build/wasi-sdk` | 27.0 |
| Qt for WASI prefix | `--qt-prefix`, `QT2WASM_QT_PREFIX`, `wasm/qt/.build/wasi` | 6.8.3 |
| Qt WASI toolchain file | `--qt-toolchain`, `QT2WASM_QT_TOOLCHAIN`, `wasm/qt/cmake/wasi-qt.cmake` | |
| host moc | `QT2WASM_MOC`, `PATH`, `/usr/lib/qt6/libexec/moc` | 6.4 and 6.8 (output revision 68) |
| wit-bindgen | `QT2WASM_WIT_BINDGEN`, `PATH` | 0.46.0 |
| wasm-tools | `QT2WASM_WASM_TOOLS`, `PATH` | 1.239.0 |
| wasm-opt (optional) | `QT2WASM_WASM_OPT`, `PATH` | binaryen 119 |
| preview1 reactor adapter | `QT2WASM_ADAPTER`, `wasm/qt/` | wasmtime 36 |
| fontTools (optional, subsetting) | Python | 4.x |

## Measured (2026-11-09)

| Example | Core module | After `wasm-opt -Oz` | Component |
| --- | --- | --- | --- |
| counter (QtCore only) | 4.83 MB | 2.31 MB | 2.36 MB |
| canvas (QtGui, QPainter, DejaVu Sans subset 45 KB) | 9.06 MB | 6.45 MB | 6.50 MB |

Both run under wasmtime-py: `pytest tools/qt2wasm/tests` with
`QT2WASM_TEST_COUNTER` / `QT2WASM_TEST_CANVAS` pointing at the builds.

## Not done yet

- WIT-first C++ interface generation (Qt types in an abstract class per export).
- Denying unused WASI imports (filesystem, cli) with WASI-Virt.
- Wizer pre-initialisation, LTO-built Qt, a wasip2-native link.
