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
`wasm-tools` and `wasi-preview1-adapter` from nandi-testing (built from
`recipes/` in this repo, linux-64), plus cmake, ninja, binaryen, fontTools and Zensical
from conda-forge. Nothing is compiled at install time. The `publish-qt2wasm`
job in `.forge/jobs.toml` builds it with pixi-build on Depot and publishes it
when the version in `pixi.toml` and `pyproject.toml` is new.

In a pixi project:

```toml
[workspace]
channels = ["conda-forge", "https://prefix.dev/nandi-testing"]
platforms = ["linux-64"]

[dependencies]
qt2wasm = { version = "0.7.*", channel = "https://prefix.dev/nandi-testing" }
```

then `pixi run qt2wasm build path/to/project -o app.wasm` (add `--font some.ttf`
for QtGui). It finds wasi-sdk, Qt, moc, wit-bindgen, wasm-tools and the adapter
in the active env (see "Tools it looks for").

## Quick start: the examples

From a checkout of this repo, with [pixi](https://pixi.sh), at the repo root
(its `pixi.toml` installs the published toolchain, Python and wasmtime-py, and
runs qt2wasm from this directory):

```sh
pixi run run     # builds the examples, then runs both under wasmtime;
                 # writes qt2wasm/build/canvas.png
pixi run test    # codegen tests + the built components
pixi run qt2wasm build path/to/project -o app.wasm
```

`run` and `test` depend on the `examples` task, which builds
`qt2wasm/build/counter.wasm` and `canvas.wasm` and is cached on its inputs.
The canvas example compiles in a font: DejaVu Sans (Mono) from the pixi env or
the system, or `QT2WASM_FONT=/path/to.ttf`. `wasm-opt` (binaryen) is in the env
and gives the smaller sizes below.

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

`qt2wasm build` writes the WIT next to the component (`app.wasm` ->
`app.wit`). `qt2wasm wit moc_x.cpp.json` prints the WIT without building, and
`qt2wasm glue` writes only the glue, for projects with their own build.

### Docs: qdoc comments become WIT `///`

Document the class the Qt way, with qdoc `/*! ... */` comments in the header
or the `.cpp`, and `qt2wasm build` runs qdoc over the project and writes the
text into the WIT as `///` docs, which wit-bindgen and other WIT tools carry
into the bindings they generate:

```cpp
/*!
    \fn QList<double> Counter::history(int n) const
    Returns the last \a n values the counter held, oldest first.
*/
```

```wit
/// Returns the last `n` values the counter held, oldest first.
history: func(n: s32) -> list<f64>;
```

| qdoc | WIT item |
| --- | --- |
| `\class X` | the interface (brief) and resource (full text), or a Q_GADGET's record |
| constructor, method, slot | the resource's constructor or method |
| `\property X::p` | getter `p` and setter `set-p`; on a Q_GADGET, the record field |
| signal | its function in `iface-events` |
| `\enum`, `\value` | the enum or flags, and each case |
| `\a arg`, `\c code`, `\l`/`\sa` to a member | `` `arg` ``, `` `code` ``, the member's WIT name |

[`examples/counter/counter.wit`](examples/counter/counter.wit) is the
WIT generated from the documented [`counter.cpp`](examples/counter/counter.cpp),
checked in as a sample (a test keeps it current when qdoc is installed).

`--docs auto` (the default) uses qdoc when it is found and skips it quietly
otherwise. `--docs qdoc` makes a missing or failing qdoc an error, and
`--docs none` turns it off. `qt2wasm wit ... --docs-from DIR` does the same
for the `wit` command, and `--webxml PATH` reuses qdoc WebXML you already
have. qdoc's own log, and anything it could not place, go to
`build/qt2wasm/gen/qdoc/`. The docs are in the `.wit`, not inside the
`.wasm`: a component's type section has nowhere to keep them.

qdoc comes from qttools, which no conda channel ships, so it is not a
dependency of the package yet. On Debian or Ubuntu, `apt install
qt6-documentation-tools` gives `/usr/lib/qt6/bin/qdoc` (6.4 tested), or set
`QT2WASM_QDOC`.

### Qt Help: a `.qch` next to the component

When qhelpgenerator is found too, `qt2wasm build` also writes `<name>.qch`
next to the `.wasm` and `.wit`: qdoc renders the same comments as HTML with a
Qt Help project (`.qhp`), and qhelpgenerator packs it. Open it in Qt Assistant
or add it in Qt Creator (Preferences, Help, Documentation). The front page is a
`\module <name>` page listing the classes; write your own `\module` comment to
replace it. The help namespace is `org.qt2wasm.<name>`.

`--qch auto` (the default) makes it when qdoc and qhelpgenerator are both found
and skips it quietly otherwise, `--qch always` makes a missing tool or a failure
an error, and `--qch none` (or `--docs none`) turns it off. Without a build,
`qt2wasm qch DIR [-o name.qch] [--version 1.0]` makes one from a directory of
sources. The pages and `.qhp` stay in `build/qt2wasm/gen/qch/`.
`examples/counter/counter.qch` is a checked-in sample (made with qdoc and
qhelpgenerator 6.4; regenerate with `qt2wasm qch examples/counter -o
examples/counter/counter.qch`).

qhelpgenerator comes from qttools too: on Debian or Ubuntu, `apt install
qt6-tools-dev-tools` gives `/usr/lib/qt6/libexec/qhelpgenerator` (6.4 tested),
or set `QT2WASM_QHELPGENERATOR`.

### Docs site: WIT -> Markdown -> Zensical

Every `qt2wasm build` also writes the component's API docs:

- `<name>.md` next to the `.wasm` and `.wit`: the WIT as Markdown, from
  `wit-bindgen markdown`, so the qdoc prose is in it.
- `<name>-docs/`: a docs site project (`mkdocs.yml`, `docs/`) and the built
  site in `<name>-docs/site/` (open `index.html`). It is built with
  [Zensical](https://zensical.org), the successor of Material for MkDocs,
  which reads `mkdocs.yml` as is. Plain MkDocs is the fallback when Zensical
  is not installed.

`--site auto` (the default) builds the site when Zensical or MkDocs is found
and otherwise writes only the `.md`, quietly; a failed site build is a
warning. `--site always` makes either an error, and `--site none` writes only
the `.md`. `QT2WASM_SITE_BUILDER=mkdocs` forces MkDocs. Zensical is a
dependency of the conda package (from conda-forge), so with the package the
site is always made.

For WITs you have already, or several components on one site:

```sh
qt2wasm markdown app.wit -o docs/           # just the Markdown page
qt2wasm mkdocs a.wit b.wit -o api --build   # api/mkdocs.yml + docs/ + site/
```

`mkdocs` writes one page per WIT plus an overview, named after the WIT
package; `--build` builds it (`--builder zensical|mkdocs` to pick), or leave
it out and run `zensical serve -f api/mkdocs.yml`. The theme is Material
with Zensical or mkdocs-material, else plain `mkdocs` (`--theme` picks one).
In this repo `pixi run docs` builds the counter example's site into
`qt2wasm/build/counter-docs/site/` and `pixi run docs-serve` serves it.
[`examples/counter/counter.md`](examples/counter/counter.md) is the checked-in
Markdown for the sample WIT.

qt2wasm tidies wit-bindgen's Markdown for Python-Markdown: wit-bindgen puts
enum-case and field docs after a bare `<p>`, which would be read as a raw
HTML block that swallows the rest of the page.

## Writing a component (WIT-first)

`qt2wasm build PROJECT --wit path/to/wit --world my-world` skips the WIT and
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
| qdoc (optional, WIT docs) | `QT2WASM_QDOC`, `PATH`, `/usr/lib/qt6/bin/qdoc` | 6.4 |
| qhelpgenerator (optional, `.qch`) | `QT2WASM_QHELPGENERATOR`, `PATH`, `/usr/lib/qt6/libexec/qhelpgenerator` | 6.4 |
| zensical, or mkdocs (docs site) | `QT2WASM_SITE_BUILDER`, `PATH`, Python | zensical 0.1, mkdocs 1.6 |

## Measured (2026-11-09)

| Example | Core module | After `wasm-opt -Oz` | Component |
| --- | --- | --- | --- |
| counter (QtCore only) | 4.83 MB | 2.31 MB | 2.36 MB |
| canvas (QtGui, QPainter, DejaVu Sans subset 45 KB) | 9.06 MB | 6.45 MB | 6.50 MB |

Both run under wasmtime-py (`pixi run run`, `pixi run test`).

## Not done yet

- WIT-first C++ interface generation (Qt types in an abstract class per export).
- Denying unused WASI imports (filesystem, cli) with WASI-Virt.
- Wizer pre-initialisation, LTO-built Qt, a wasip2-native link.
