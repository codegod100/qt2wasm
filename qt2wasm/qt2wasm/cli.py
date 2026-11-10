"""qt2wasm command line.

qt2wasm build PROJECT [-o out.wasm]   Qt CMake project -> WASI component
qt2wasm wit HEADER.json... [-o x.wit] moc metatypes -> WIT (Qt-first);
                                      --docs-from DIR adds the Qt docs (qdoc)
qt2wasm glue HEADER.json... --bindings component.h -o glue.cpp
qt2wasm qch DIR [-o name.qch]          qdoc comments in DIR -> Qt Help (.qch)
qt2wasm markdown APP.wit [-o DIR]     WIT -> Markdown (wit-bindgen markdown)
qt2wasm mkdocs APP.wit... -o DIR      docs site project (mkdocs.yml) from the
                                      WIT; --build builds it with Zensical
                                      (or MkDocs)
qt2wasm js APP.wasm [-o DIR]          component -> JS bindings (jco transpile
                                      + load() with the WASI shim); `build`
                                      does this by default as <name>-js/
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


def _qch(a):
    import tempfile

    from qt2wasm import docs
    from qt2wasm.build import ROOT

    qdoc, qhg = docs.find_qdoc(), docs.find_qhelpgenerator()
    if qdoc is None or qhg is None:
        raise model.Qt2WasmError(
            "qch: needs qdoc and qhelpgenerator (QT2WASM_QDOC / "
            "QT2WASM_QHELPGENERATOR, PATH, or /usr/lib/qt6/{bin,libexec})"
        )
    proj = Path(a.project).resolve()
    out = Path(a.output or f"{proj.name}.qch").resolve()
    incs = [Path(i) for i in a.includes] + [ROOT / "include", proj]
    incs += docs.qt_includes()
    with tempfile.TemporaryDirectory(prefix="qt2wasm-qch-") as tmp:
        docs.run_qch(
            qdoc,
            qhg,
            proj,
            Path(tmp),
            out,
            [i for i in incs if i.is_dir()],
            exclude=[proj / "build"],
            name=a.name or out.stem,
            version=a.version,
        )
    print(out)


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
    b.add_argument(
        "--qch",
        choices=["auto", "always", "none"],
        default="auto",
        help="Qt Help file <name>.qch next to the component, from qdoc HTML + "
        "qhelpgenerator: auto (when both are found, the default), always "
        "(required), none",
    )
    b.add_argument(
        "--site",
        choices=["auto", "always", "none"],
        default="auto",
        help="docs site <name>-docs/site/ from the WIT, built with Zensical "
        "(or MkDocs): auto (when one is found, the default), always (required), "
        "none (<name>.md only)",
    )
    b.add_argument(
        "--js",
        choices=["auto", "always", "none"],
        default="auto",
        help="JS bindings <name>-js/ (jco transpile + a load() helper with the "
        "WASI shim, runs under node): auto (when jco is found, the default), "
        "always (required), none",
    )
    b.add_argument(
        "--docs-only",
        action="store_true",
        help="stop after the docs: moc, qdoc and codegen write <name>.wit, .md, "
        ".qch and the site, with no compile or link (seconds, not minutes)",
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

    h = sub.add_parser("qch", help="qdoc comments in a project -> Qt Help (.qch)")
    h.add_argument("project", help="directory with the /*! */ comments")
    h.add_argument("-o", "--output", help="the .qch (default ./<dirname>.qch)")
    h.add_argument("--name", help="help project name (default: the .qch's stem)")
    h.add_argument("--version", default="", help="version for the help namespace")
    h.add_argument(
        "-I",
        dest="includes",
        action="append",
        default=[],
        help="include dir for qdoc's clang (repeatable)",
    )

    g = sub.add_parser("glue", help="metatypes + wit-bindgen C header -> C++ glue")
    g.add_argument("json", nargs="+")
    g.add_argument("--bindings", required=True, help="the header `wit-bindgen c` wrote")
    g.add_argument("-o", "--output", required=True)

    md = sub.add_parser("markdown", help="WIT -> Markdown with wit-bindgen markdown")
    md.add_argument("wit", help="a .wit file, a WIT directory or a component .wasm")
    md.add_argument("-o", "--out-dir", default=".", help="directory for <name>.md")
    md.add_argument("--world", help="world to document (if the WIT has several)")

    mk = sub.add_parser(
        "mkdocs", help="docs site project (mkdocs.yml, Zensical/MkDocs) from WIT files"
    )
    mk.add_argument("wit", nargs="+", help=".wit files (one page each)")
    mk.add_argument(
        "-o", "--output", required=True, help="project dir: mkdocs.yml + docs/"
    )
    mk.add_argument("--title", help="site_name (default: the WIT package)")
    mk.add_argument(
        "--theme",
        help="theme (default material with Zensical or mkdocs-material, else mkdocs)",
    )
    mk.add_argument(
        "--build",
        action="store_true",
        help="also build it into DIR/site (Zensical, else MkDocs)",
    )
    mk.add_argument(
        "--builder", choices=["zensical", "mkdocs"], help="force one site builder"
    )

    js = sub.add_parser("js", help="component -> JS bindings (jco transpile + loader)")
    js.add_argument("wasm", help="the component .wasm")
    js.add_argument("-o", "--output", help="directory (default <name>-js next to it)")
    js.add_argument("--name", help="module name (default: the .wasm's stem)")

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
        elif a.cmd == "qch":
            _qch(a)
        elif a.cmd == "markdown":
            from qt2wasm import mdsite

            print(mdsite.markdown(Path(a.wit), Path(a.out_dir), world=a.world))
        elif a.cmd == "mkdocs":
            from qt2wasm import mdsite

            cfg = mdsite.mkdocs_project(
                [Path(w) for w in a.wit],
                Path(a.output),
                a.title,
                a.theme,
                builder=a.builder,
            )
            print(cfg)
            if a.build:
                print(mdsite.build_site(cfg, builder=a.builder) / "index.html")
        elif a.cmd == "js":
            from qt2wasm import jsgen

            wasm = Path(a.wasm).resolve()
            jco = jsgen.find_jco()
            if jco is None:
                raise model.Qt2WasmError("js: jco not found (PATH or QT2WASM_JCO)")
            name = a.name or wasm.stem
            dst = Path(a.output).resolve() if a.output else wasm.parent / f"{name}-js"
            try:
                print(jsgen.transpile(wasm, dst, name, jco) / "index.js")
            except jsgen.JsError as e:
                raise model.Qt2WasmError(str(e)) from e
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
