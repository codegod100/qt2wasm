"""qt2wasm command line.

qt2wasm build PROJECT [-o out.wasm]   Qt CMake project -> WASI component
qt2wasm wit HEADER.json... [-o x.wit] moc metatypes -> WIT (Qt-first);
                                      --docs-from DIR adds the Qt docs (qdoc)
qt2wasm glue HEADER.json... --bindings component.h -o glue.cpp
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from qt2wasm import gluegen, model, witgen


def _model(paths):
    return model.build_model(model.load_moc_json([Path(p) for p in paths]))


def _docs(a):
    from qt2wasm import docs

    if a.webxml:
        p = Path(a.webxml)
        return docs.load_webxml(sorted(p.rglob("*.webxml")) if p.is_dir() else [p])
    if not a.docs_from:
        return None
    qdoc = docs.find_qdoc()
    if qdoc is None:
        raise model.Qt2WasmError(
            "--docs-from: qdoc not found (QT2WASM_QDOC, PATH, or /usr/lib/qt6/bin/qdoc)"
        )
    import tempfile

    from qt2wasm.build import ROOT

    incs = [Path(i) for i in a.includes] + [ROOT / "include", Path(a.docs_from)]
    incs += docs.qt_includes()
    with tempfile.TemporaryDirectory(prefix="qt2wasm-qdoc-") as tmp:
        d, _ = docs.run_qdoc(
            qdoc,
            Path(a.docs_from).resolve(),
            Path(tmp),
            [i for i in incs if i.is_dir()],
        )
    return d


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="qt2wasm",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="build a Qt project into a WASI component")
    b.add_argument(
        "project",
        help="directory with a CMakeLists.txt calling qt2wasm_add_component()",
    )
    b.add_argument(
        "-o", "--output", help="component path (default build/qt2wasm/<name>.wasm)"
    )
    b.add_argument("--build-dir")
    b.add_argument(
        "--wit",
        help="WIT-first: implement this existing WIT package instead of generating one",
    )
    b.add_argument("--world", help="world in --wit to target")
    b.add_argument(
        "--font",
        action="append",
        default=[],
        help="font file to compile in (repeatable)",
    )
    b.add_argument(
        "--font-subset",
        default="latin",
        help="latin (default), ascii, none, or fontTools --unicodes ranges",
    )
    b.add_argument(
        "--docs",
        choices=["auto", "qdoc", "none"],
        default="auto",
        help="Qt doc comments -> WIT /// docs: auto (qdoc if found, the default), "
        "qdoc (required), none",
    )
    b.add_argument("--opt", choices=["size", "speed"], default="size")
    b.add_argument(
        "--lto",
        action="store_true",
        help="thin LTO for the project's code (Qt itself is not LTO-built yet)",
    )
    b.add_argument("--no-wasm-opt", action="store_true")
    b.add_argument(
        "--qpa", default="offscreen", help="static QPA plugin (offscreen or minimal)"
    )
    b.add_argument("--wasi-sdk")
    b.add_argument("--qt-prefix")
    b.add_argument("--qt-toolchain")

    w = sub.add_parser("wit", help="moc --output-json metatypes -> WIT")
    w.add_argument("json", nargs="+")
    w.add_argument("-o", "--output")
    w.add_argument(
        "--docs-from",
        metavar="DIR",
        help="run qdoc over DIR's /*! */ comments and add them as /// docs",
    )
    w.add_argument(
        "--webxml",
        metavar="PATH",
        help="use qdoc WebXML output already made (a .webxml file or a directory)",
    )
    w.add_argument(
        "-I",
        dest="includes",
        action="append",
        default=[],
        help="include dir for qdoc's clang (repeatable; Qt's include dirs and "
        "qt2wasm's are added from QT2WASM_QT_PREFIX / the checkout)",
    )

    g = sub.add_parser("glue", help="metatypes + wit-bindgen C header -> C++ glue")
    g.add_argument("json", nargs="+")
    g.add_argument("--bindings", required=True, help="the header `wit-bindgen c` wrote")
    g.add_argument("-o", "--output", required=True)

    a = ap.parse_args(argv)
    try:
        if a.cmd == "build":
            from qt2wasm.build import build

            build(a)
        elif a.cmd == "wit":
            text = witgen.generate(_model(a.json), _docs(a))
            if a.output:
                Path(a.output).write_text(text)
            else:
                sys.stdout.write(text)
        elif a.cmd == "glue":
            Path(a.output).write_text(
                gluegen.generate(_model(a.json), Path(a.bindings))
            )
    except model.Qt2WasmError as e:
        print(f"qt2wasm: error: {e}", file=sys.stderr)
        return 2
    except Exception as e:  # BuildError and friends: message is the report
        if type(e).__name__ == "BuildError":
            print(f"qt2wasm: error: {e}", file=sys.stderr)
            return 1
        raise
    return 0
