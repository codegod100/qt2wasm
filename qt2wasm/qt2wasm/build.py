"""`qt2wasm build`: Qt CMake project in, WASI component out.

Steps (their times are logged at the end; unchanged steps are skipped, see
Stamps below):
  1. find tools          wasi-sdk, Qt-for-WASI prefix + toolchain, host moc,
                         wit-bindgen, wasm-tools, wasm-opt, p1 reactor adapter
  2. moc                 every header with Q_OBJECT/Q_GADGET: C++ for the
                         wasm build and --output-json metatypes
  3. codegen             Qt-first: WIT + C++ glue from the metatypes, with
                         the Qt doc comments as WIT /// docs when qdoc is
                         found (--docs). The WIT is also written next to
                         the component as <name>.wit, as Markdown
                         (wit-bindgen markdown) as <name>.md, a docs site
                         <name>-docs/site/ (Zensical or MkDocs, --site),
                         and, when
                         qdoc and qhelpgenerator are found, the Qt Help
                         file <name>.qch (--qch).
                         WIT-first (--wit): bindings only, you implement them.
  4. cmake               configure + build with the Qt WASI toolchain file
  5. optimise            wasm-opt on the core module (keeps the WIT section)
  6. componentise        wasm-tools component new --adapt (preview1 reactor)
  7. check               validate, print the world and the WASI imports
  8. js                  jco transpile -> <name>-js/: ES module + .d.ts and a
                         load() helper with the WASI preview2 shim, runnable
                         with plain node (--js; jco is a qt2wasm dependency)
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from qt2wasm import docs as qdocs
from qt2wasm import gluegen, model, witgen

_PKG = Path(__file__).resolve().parent
# Installed (conda/pip): cmake/, include/ and runtime/ ship inside the package
# as _data/. From a checkout: the qt2wasm/ directory itself.
ROOT = _PKG / "_data" if (_PKG / "_data" / "cmake").is_dir() else _PKG.parent


class BuildError(Exception):
    pass


def log(msg: str) -> None:
    print(f"qt2wasm: {msg}", file=sys.stderr, flush=True)


# Host compiler settings that a conda env with compilers exports on activation
# (e.g. CFLAGS=-march=nocona -mtune=haswell, CC=x86_64-conda-linux-gnu-cc).
# CMake folds them into the wasm32-wasip1 build, where clang rejects them.
HOST_BUILD_VARS = ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "DEBUG_CFLAGS", "DEBUG_CXXFLAGS",
                   "DEBUG_CPPFLAGS", "CC", "CXX", "CPP", "LD", "AR", "NM", "RANLIB")


def wasm_env() -> dict:
    """os.environ without the host compiler settings, for the WASI CMake build."""
    return {k: v for k, v in os.environ.items() if k not in HOST_BUILD_VARS}


def run(cmd: list, **kw) -> subprocess.CompletedProcess:
    cmd = [str(c) for c in cmd]
    r = subprocess.run(cmd, capture_output=True, text=True, check=False, **kw)
    if r.returncode != 0:
        raise BuildError(
            f"command failed ({r.returncode}): {' '.join(cmd)}\n{r.stdout[-4000:]}{r.stderr[-4000:]}"
        )
    return r


# Incremental builds. A rebuild with nothing changed should take seconds:
#  - generated sources are made in gen.new/ and only files whose bytes differ
#    are copied into gen/, so their mtimes (and ninja's view) stay put;
#  - font subsets, the CMake configure, wasm-opt, the component wrap, the docs
#    site, the .qch and the JS bindings each record a digest of their inputs
#    in <build-dir>/stamps/ and are skipped while it and their outputs match.
STAMP_VERSION = "1"


def digest(*parts) -> str:
    """sha256 over strings, bytes and (the contents of) files."""
    h = hashlib.sha256(STAMP_VERSION.encode())
    for p in parts:
        if isinstance(p, Path):
            h.update(b"F" + str(p).encode() + b"\0")
            if p.is_file():
                with open(p, "rb") as f:
                    for chunk in iter(lambda: f.read(1 << 20), b""):
                        h.update(chunk)
            else:
                h.update(b"<missing>")
        elif isinstance(p, bytes):
            h.update(b"B" + p)
        else:
            h.update(b"S" + str(p).encode() + b"\0")
    return h.hexdigest()


def file_digest(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Stamps:
    """<build-dir>/stamps/<step>.json: {"in": inputs digest, "out": {path:
    sha256 or "dir"}}. fresh() is true when the inputs digest matches and every
    output is still there unchanged (a directory only has to exist)."""

    def __init__(self, bdir: Path, enabled: bool = True):
        self.dir = bdir / "stamps"
        self.enabled = enabled

    def _path(self, step: str) -> Path:
        return self.dir / f"{step}.json"

    def fresh(self, step: str, key: str) -> bool:
        if not self.enabled:
            return False
        try:
            st = json.loads(self._path(step).read_text())
        except (OSError, ValueError):
            return False
        if st.get("in") != key:
            return False
        for path, want in st.get("out", {}).items():
            p = Path(path)
            if want == "dir":
                if not p.is_dir():
                    return False
            elif not p.is_file() or file_digest(p) != want:
                return False
        return True

    def record(self, step: str, key: str, outputs: list) -> None:
        self.dir.mkdir(parents=True, exist_ok=True)
        out = {}
        for o in outputs:
            if o is None:
                continue
            o = Path(o)
            out[str(o)] = "dir" if o.is_dir() else file_digest(o)
        self._path(step).write_text(json.dumps({"in": key, "out": out}, indent=1))

    def drop(self, step: str) -> None:
        self._path(step).unlink(missing_ok=True)


def sync_tree(src: Path, dst: Path) -> int:
    """Make dst a copy of src, rewriting only files whose bytes differ (so an
    unchanged generated file keeps its mtime). Returns how many changed."""
    changed = 0
    want = set()
    for f in sorted(src.rglob("*")):
        if f.is_dir():
            continue
        rel = f.relative_to(src)
        want.add(rel)
        d = dst / rel
        if d.is_file() and d.stat().st_size == f.stat().st_size and d.read_bytes() == f.read_bytes():
            continue
        d.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(f, d)
        changed += 1
    if dst.exists():
        for f in sorted(dst.rglob("*"), reverse=True):
            rel = f.relative_to(dst)
            if f.is_dir():
                if not any(f.iterdir()):
                    f.rmdir()
            elif rel not in want:
                f.unlink()
                changed += 1
    return changed


class Timer:
    """Wall time per build step, printed at the end of the build."""

    def __init__(self):
        self.steps: list[tuple[str, float]] = []
        self.t = time.time()

    def lap(self, label: str) -> None:
        now = time.time()
        self.steps.append((label, now - self.t))
        self.t = now

    def summary(self) -> str:
        return ", ".join(f"{n} {s:.1f}s" for n, s in self.steps if s >= 0.05)


@dataclass
class Tools:
    wasi_sdk: Path
    qt_prefix: Path
    qt_toolchain: Path
    compat: Path | None
    moc: Path
    wit_bindgen: Path
    wasm_tools: Path
    wasm_opt: Path | None
    adapter: Path | None


def _which(name: str, env: str, *extra: Path) -> Path | None:
    if os.environ.get(env):
        return Path(os.environ[env])
    found = shutil.which(name)
    if found:
        return Path(found)
    for e in extra:
        if e.exists():
            return e
    return None


def find_tools(args) -> Tools:
    """The toolchain comes from conda packages on prefix.dev/nandi-testing:
    qt6-wasi (Qt + host moc + toolchain file, pulls in wasi-sdk), wit-bindgen,
    wasm-tools and wasi-preview1-adapter. Their activation scripts export the
    env vars below; without activation, look under this Python's prefix."""
    missing = []

    def need(label: str, value: Path | None, hint: str) -> Path:
        if value is None or not Path(value).exists():
            missing.append(f"  {label}: not found ({hint})")
            return Path("/nonexistent")
        return Path(value)

    prefix = Path(os.environ.get("CONDA_PREFIX") or sys.prefix)
    qtpkg = prefix / "opt" / "qt6-wasi"
    pkg_hint = "install qt6-wasi from https://prefix.dev/nandi-testing"
    wasi_sdk = need(
        "wasi-sdk",
        Path(
            args.wasi_sdk
            or os.environ.get("WASI_SDK_PREFIX")
            or prefix / "opt" / "wasi-sdk"
        ),
        f"--wasi-sdk or WASI_SDK_PREFIX; {pkg_hint} (it depends on wasi-sdk 27)",
    )
    qt_prefix = need(
        "Qt for WASI",
        Path(args.qt_prefix or os.environ.get("QT2WASM_QT_PREFIX") or qtpkg / "wasi"),
        f"--qt-prefix or QT2WASM_QT_PREFIX; {pkg_hint}",
    )
    qt_toolchain = need(
        "Qt WASI toolchain file",
        Path(
            args.qt_toolchain
            or os.environ.get("QT2WASM_QT_TOOLCHAIN")
            or qtpkg / "cmake" / "wasi-qt.cmake"
        ),
        f"--qt-toolchain or QT2WASM_QT_TOOLCHAIN; {pkg_hint}",
    )
    compat = qt_toolchain.parent.parent / "compat"
    moc = need(
        "host moc",
        _which(
            "moc",
            "QT2WASM_MOC",
            qtpkg / "host" / "libexec" / "moc",
            Path("/usr/lib/qt6/libexec/moc"),
        ),
        f"QT2WASM_MOC; {pkg_hint}, or any Qt 6 moc with output revision 68",
    )
    wit_bindgen = need(
        "wit-bindgen",
        _which("wit-bindgen", "QT2WASM_WIT_BINDGEN", prefix / "bin" / "wit-bindgen"),
        "QT2WASM_WIT_BINDGEN; install wit-bindgen from nandi-testing (0.46 tested)",
    )
    wasm_tools = need(
        "wasm-tools",
        _which("wasm-tools", "QT2WASM_WASM_TOOLS", prefix / "bin" / "wasm-tools"),
        "QT2WASM_WASM_TOOLS; install wasm-tools from nandi-testing (1.239 tested)",
    )
    wasm_opt = _which("wasm-opt", "QT2WASM_WASM_OPT")
    adapter = _which(
        "wasi_snapshot_preview1.reactor.wasm",
        "QT2WASM_ADAPTER",
        prefix
        / "share"
        / "wasi-preview1-adapter"
        / "wasi_snapshot_preview1.reactor.wasm",
    )
    if missing:
        raise BuildError("missing tools:\n" + "\n".join(missing))
    return Tools(
        wasi_sdk,
        qt_prefix,
        qt_toolchain,
        compat if compat.exists() else None,
        moc,
        wit_bindgen,
        wasm_tools,
        wasm_opt,
        adapter,
    )


_QT_CLASS = re.compile(r"\bQ_(OBJECT|GADGET)\b")


def find_headers(project: Path, build: Path) -> list[Path]:
    """Headers with Q_OBJECT/Q_GADGET, skipping the build dir, the default
    one (project/build, left over from an earlier build without --build-dir)
    and dot dirs."""
    out = []
    skip = {build, project / "build"}
    for h in sorted(project.rglob("*.h")):
        if skip & set(h.parents) or any(
            p.name.startswith(".") for p in h.relative_to(project).parents if p.name
        ):
            continue
        if _QT_CLASS.search(h.read_text(errors="replace")):
            out.append(h)
    return out


# Unicode ranges for --font-subset. "latin" also keeps the block and braille
# characters terminal renderers draw with.
SUBSETS = {
    "latin": "U+0020-007E,U+00A0-00FF,U+2010-2027,U+2500-259F,U+2800-28FF",
    "ascii": "U+0020-007E",
}


def subset_font(src: Path, unicodes: str, cache: Path) -> Path:
    """The subset of src, made once per (font bytes, ranges, fontTools
    version) and kept in cache/ for later builds."""
    try:
        import fontTools
        from fontTools import subset
    except ImportError:
        log(f"fontTools not installed; embedding {src.name} whole")
        return src
    key = digest(src.read_bytes(), unicodes, fontTools.version)[:16]
    dst = cache / f"{key}-{src.name}"
    if dst.is_file():
        return dst
    cache.mkdir(parents=True, exist_ok=True)
    tmp = cache / f".{dst.name}.tmp"
    subset.main(
        [
            str(src),
            f"--unicodes={unicodes}",
            f"--output-file={tmp}",
            "--layout-features=*",
            "--no-hinting",
        ]
    )
    tmp.replace(dst)
    log(
        f"font {src.name}: {src.stat().st_size // 1024} KB -> {dst.stat().st_size // 1024} KB"
    )
    return dst


def write_fonts(fonts: list[Path], out: Path) -> None:
    L = [
        "// Generated by qt2wasm: fonts compiled into the component.",
        "#include <cstddef>",
        "",
    ]
    names = []
    for i, f in enumerate(fonts):
        data = Path(f).read_bytes()
        names.append((f"font{i}", len(data)))
        L.append(f"// {Path(f).name}, {len(data)} bytes")
        L.append(f"static const unsigned char font{i}[] = {{")
        for j in range(0, len(data), 24):
            L.append("    " + ",".join(str(b) for b in data[j : j + 24]) + ",")
        L.append("};")
    L.append(
        'extern "C" void qt2wasm_bundled_fonts(void (*add)(const unsigned char *, size_t)) {'
    )
    for n, size in names:
        L.append(f"    add({n}, {size});")
    L.append("}")
    out.write_text("\n".join(L) + "\n")


def imports_of(wasm_tools: Path, component: Path) -> list[str]:
    wit = run([wasm_tools, "component", "wit", component]).stdout
    return re.findall(r"^\s*import ([\w:/.@-]+);", wit, re.MULTILINE)


def qdoc_includes(tools: Tools, project: Path) -> list[Path]:
    """What qdoc's clang needs to parse the project's headers."""
    return [ROOT / "include", project, *qdocs.qt_includes(tools.qt_prefix)]


def project_docs(args, tools: Tools, project: Path, bdir: Path, gen: Path):
    """Qt doc comments via qdoc, or None. --docs auto (default) skips quietly
    without qdoc and warns if it fails; --docs qdoc makes either an error."""
    mode = getattr(args, "docs", "auto") or "auto"
    if mode == "none":
        return None
    qdoc = qdocs.find_qdoc()
    if qdoc is None:
        if mode == "qdoc":
            raise BuildError(
                "--docs qdoc: qdoc not found (QT2WASM_QDOC, PATH, or "
                "/usr/lib/qt6/bin/qdoc from qt6-documentation-tools)"
            )
        log("docs: no qdoc found, WIT without Qt docs (set QT2WASM_QDOC)")
        return None
    try:
        docs, qlog = qdocs.run_qdoc(
            qdoc,
            project,
            gen / "qdoc",
            qdoc_includes(tools, project),
            exclude=[bdir, project / "build"],
        )
    except Exception as e:
        if mode == "qdoc":
            raise BuildError(str(e)) from e
        log(f"docs: qdoc failed, WIT without Qt docs: {str(e).splitlines()[0]}")
        return None
    n = len(docs.classes) + len(docs.functions) + len(docs.properties) + len(docs.enums)
    log(f"docs: qdoc {qdoc}: {n} documented item(s)")
    lost = qlog.count("Cannot tie this documentation")
    if lost:
        log(f"docs: qdoc could not place {lost} comment(s); see {gen / 'qdoc'}")
        (gen / "qdoc" / "qdoc.log").write_text(qlog)
    return docs


def wit_digest(wit: Path) -> str:
    files = sorted(wit.rglob("*.wit")) if wit.is_dir() else [wit]
    return digest(*files)


def project_site(args, tools: Tools, wit: Path, out: Path, stamps: "Stamps | None" = None):
    """<out>.md (wit-bindgen markdown) and the docs site <out>-docs/ (mkdocs.yml,
    docs/, site/ built with Zensical or MkDocs). --site auto (default) skips
    the site quietly when neither is installed and warns if the build fails;
    --site always makes either an error; --site none writes only the .md."""
    from qt2wasm import mdsite

    world = getattr(args, "world", None)
    mode = getattr(args, "site", "auto") or "auto"
    builder = mdsite.find_builder() if mode != "none" else None
    sdir = out.parent / f"{out.stem}-docs"
    key = digest("site", wit_digest(wit), world, out.stem, mode, builder, tools.wit_bindgen,
                 *sorted(Path(mdsite.__file__).parent.glob("mdsite.py")))
    if stamps and stamps.fresh("site", key):
        site = sdir / "site" if builder else None
        log(f"site: {site / 'index.html' if site else out.with_suffix('.md')} (unchanged)")
        return site
    if stamps:
        stamps.drop("site")
    try:
        md = mdsite.markdown(
            wit, out.parent, name=out.stem, world=world, wit_bindgen=tools.wit_bindgen
        )
    except Exception as e:
        raise BuildError(str(e)) from e
    if mode == "none":
        if stamps:
            stamps.record("site", key, [md])
        return None
    if builder is None:
        if mode == "always":
            raise BuildError(
                "--site always: neither zensical nor mkdocs found (install "
                "zensical from conda-forge or PyPI, or set QT2WASM_SITE_BUILDER)"
            )
        log(f"site: no zensical or mkdocs found, only {md.name}")
        if stamps:
            stamps.record("site", key, [md])
        return None
    try:
        cfg = mdsite.mkdocs_project(
            [wit], sdir, world=world, wit_bindgen=tools.wit_bindgen
        )
        site = mdsite.build_site(cfg)
    except Exception as e:
        if mode == "always":
            raise BuildError(str(e)) from e
        log(f"site: failed, only {md.name}: {str(e).splitlines()[0]}")
        return None
    if stamps:
        stamps.record("site", key, [md, site / "index.html"])
    log(f"site: {site / 'index.html'}")
    return site


def project_js(args, out: Path, stamps: "Stamps | None" = None):
    """<out>-js/: jco transpile + loader (qt2wasm.jsgen), or None. --js auto
    (default) skips quietly without jco and warns if it fails; --js always
    makes either an error; --js none turns it off."""
    from qt2wasm import jsgen

    mode = getattr(args, "js", "auto") or "auto"
    if mode == "none":
        return None
    jco = jsgen.find_jco()
    if jco is None:
        if mode == "always":
            raise BuildError("--js always: jco not found (install jco from "
                             "https://prefix.dev/nandi-testing, or set QT2WASM_JCO)")
        log("js: no jco found, no JS bindings (set QT2WASM_JCO)")
        return None
    key = digest("js", out, jco, Path(jsgen.__file__))
    dst = out.parent / f"{out.stem}-js"
    if stamps and stamps.fresh("js", key):
        log(f"js: {dst / 'index.js'} (unchanged)")
        return dst
    try:
        dst = jsgen.transpile(out, dst, out.stem, jco)
    except jsgen.JsError as e:
        if mode == "always":
            raise BuildError(str(e)) from e
        log(f"js: failed, no JS bindings: {str(e).splitlines()[0]}")
        return None
    if stamps:
        stamps.record("js", key, [dst / "index.js"])
    log(f"js: {dst / 'index.js'} (jco {jco})")
    return dst


def source_digest(*dirs: Path, skip: tuple = ()) -> str:
    """Digest of the C++/qdoc sources under dirs (what qdoc reads)."""
    files = []
    for d in dirs:
        for f in sorted(d.rglob("*")):
            if f.suffix not in (".h", ".hpp", ".cpp", ".qdoc", ".qdocconf") or not f.is_file():
                continue
            if any(s in f.parents for s in skip) or any(
                p.name.startswith(".") for p in f.relative_to(d).parents if p.name
            ):
                continue
            files.append(f)
    return digest(*files)


def project_qch(args, tools: Tools, project: Path, bdir: Path, gen: Path, out: Path,
                stamps: "Stamps | None" = None):
    """qdoc HTML + qhelpgenerator -> <out>.qch, or None. --qch auto (default)
    skips quietly when either tool is missing and warns if one fails;
    --qch always makes either an error. --docs none turns it off too."""
    mode = getattr(args, "qch", "auto") or "auto"
    if mode == "none" or getattr(args, "docs", "auto") == "none":
        return None
    qdoc, qhg = qdocs.find_qdoc(), qdocs.find_qhelpgenerator()
    if qdoc is None or qhg is None:
        missing = " and ".join(
            n for n, t in (("qdoc", qdoc), ("qhelpgenerator", qhg)) if t is None
        )
        if mode == "always":
            raise BuildError(
                f"--qch always: {missing} not found (QT2WASM_QDOC / "
                "QT2WASM_QHELPGENERATOR, PATH, or Debian's qt6-documentation-tools "
                "and qt6-tools-dev-tools)"
            )
        log(f"qch: no {missing} found, no Qt Help file")
        return None
    qch = out.with_suffix(".qch")
    key = digest("qch", qdoc, qhg, out.stem, source_digest(project, ROOT / "include", skip=(bdir, project / "build")))
    if stamps and stamps.fresh("qch", key):
        log(f"qch: {qch} (unchanged)")
        return qch
    try:
        qch, qlog = qdocs.run_qch(
            qdoc,
            qhg,
            project,
            gen / "qch",
            qch,
            qdoc_includes(tools, project),
            exclude=[bdir, project / "build"],
            name=out.stem,
        )
    except Exception as e:
        if mode == "always":
            raise BuildError(str(e)) from e
        log(f"qch: failed, no Qt Help file: {str(e).splitlines()[0]}")
        return None
    (gen / "qch" / "qch.log").write_text(qlog)
    if stamps:
        stamps.record("qch", key, [qch])
    log(f"qch: {qch}")
    return qch


def build(args) -> Path:
    t0 = time.time()
    timer = Timer()
    project = Path(args.project).resolve()
    if not (project / "CMakeLists.txt").exists():
        raise BuildError(f"{project} has no CMakeLists.txt")
    bdir = (
        Path(args.build_dir).resolve()
        if args.build_dir
        else project / "build" / "qt2wasm"
    )
    stamps = Stamps(bdir, enabled=not getattr(args, "clean", False))
    if getattr(args, "clean", False):
        for d in ("gen", "cmake", "stamps"):
            shutil.rmtree(bdir / d, ignore_errors=True)
    # Generated sources are written to gen.new/, then synced into gen/ (only
    # changed files are rewritten), so ninja recompiles only what changed.
    gen = bdir / "gen.new"
    if gen.exists():
        shutil.rmtree(gen)
    (gen / "bindings").mkdir(parents=True)
    (gen / "runtime").mkdir()
    out = Path(args.output).resolve() if args.output else bdir / f"{project.name}.wasm"

    tools = find_tools(args)
    log(f"Qt {tools.qt_prefix}, wasi-sdk {tools.wasi_sdk}")

    gui = getattr(args, "gui", None)
    if gui:
        if args.wit:
            raise BuildError("--gui implements gui/wit/gui.wit; it does not take --wit")
        args.wit, args.world = str(ROOT / "gui" / "wit"), gui
        if args.qpa == "offscreen":
            args.qpa = "qt2wasmgui"
    dev = getattr(args, "dev", False)
    if dev:
        args.no_wasm_opt = True
        args.js = "none"
        args.site = "none"
        args.qch = "none"
        log("dev build: no wasm-opt, docs site, Qt Help file or JS bindings")

    # 2. moc
    headers = find_headers(project, bdir)
    extra = [Path(d).resolve() for d in getattr(args, "moc", None) or []]
    for d in extra:
        headers += [h for h in find_headers(d, bdir) if h not in headers]
    if not headers and not args.wit:
        raise BuildError("no headers with Q_OBJECT or Q_GADGET found")
    incs = [f"-I{ROOT / 'include'}", f"-I{project}", *(f"-I{d}" for d in extra)]
    jsons = []
    for h in headers:
        cpp = gen / f"moc_{h.stem}.cpp"
        run([tools.moc, *incs, "--output-json", h, "-o", cpp])
        jsons.append(Path(str(cpp) + ".json"))
    run(
        [
            tools.moc,
            ROOT / "runtime" / "wasieventdispatcher.h",
            "-o",
            gen / "runtime" / "moc_wasieventdispatcher.cpp",
        ]
    )
    if gui:
        run([tools.moc, ROOT / "gui" / "qt2wasmgui.h", "-o", gen / "runtime" / "moc_qt2wasmgui.cpp"])
    log(f"moc: {len(headers)} header(s)")
    timer.lap("moc")

    # 3. codegen
    if args.wit:
        wit_dir, world = Path(args.wit).resolve(), args.world
        if not world:
            raise BuildError("--wit needs --world")
        run(
            [
                tools.wit_bindgen,
                "c",
                wit_dir,
                "--world",
                world,
                "--out-dir",
                gen / "bindings",
            ]
        )
        log(
            f"WIT-first: bindings for world {world}; implement the exports_* functions it declares"
        )
    else:
        m = model.build_model(model.load_moc_json(jsons))
        docs = project_docs(args, tools, project, bdir, gen)
        wit_dir = gen / "wit"
        wit_dir.mkdir()
        (wit_dir / "component.wit").write_text(witgen.generate(m, docs))
        run([tools.wasm_tools, "component", "wit", wit_dir])  # parse check
        run(
            [
                tools.wit_bindgen,
                "c",
                wit_dir,
                "--world",
                "component",
                "--out-dir",
                gen / "bindings",
            ]
        )
        (gen / "glue.cpp").write_text(
            gluegen.generate(m, gen / "bindings" / "component.h")
        )
        log(f"codegen: {len(m.classes)} class(es) -> {bdir / 'gen' / 'wit' / 'component.wit'}")
    out.parent.mkdir(parents=True, exist_ok=True)
    if not args.wit:
        shutil.copyfile(wit_dir / "component.wit", out.with_suffix(".wit"))
    elif gui:
        shutil.copyfile(wit_dir / "gui.wit", out.with_suffix(".wit"))
    timer.lap("codegen")
    project_qch(args, tools, project, bdir, gen, out, stamps)
    timer.lap("qch")
    project_site(args, tools, wit_dir if args.wit and not gui else out.with_suffix(".wit"), out, stamps)
    timer.lap("site")
    if args.font and not getattr(args, "docs_only", False):
        fonts = [Path(f).resolve() for f in args.font]
        if args.font_subset != "none":
            ranges = SUBSETS.get(args.font_subset, args.font_subset)
            fonts = [subset_font(f, ranges, bdir / "fonts") for f in fonts]
        write_fonts(fonts, gen / "fonts.cpp")
        timer.lap("fonts")

    # gen.new -> gen: only changed files get a new mtime.
    changed = sync_tree(gen, bdir / "gen")
    shutil.rmtree(gen)
    gen = bdir / "gen"
    log(f"generated sources: {changed} file(s) changed")
    if getattr(args, "docs_only", False):
        log(f"docs only: {out.with_suffix('.wit')} and friends in {time.time() - t0:.1f}s")
        return out.with_suffix(".wit")

    # 4. cmake
    cmk = bdir / "cmake"
    opt = {"size": "-Oz", "speed": "-O2"}[args.opt]
    flags = f"{opt} -ffunction-sections -fdata-sections"
    if args.lto:
        flags += " -flto=thin"
    cfg = [
        "cmake",
        "-S",
        project,
        "-B",
        cmk,
        "-G",
        "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={tools.qt_toolchain}",
        f"-DWASI_SDK_PREFIX={tools.wasi_sdk}",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_C_FLAGS_RELEASE={flags} -DNDEBUG",
        f"-DCMAKE_CXX_FLAGS_RELEASE={flags} -DNDEBUG",
        f"-DQT2WASM_ROOT={ROOT}",
        f"-DQT2WASM_QT_PREFIX={tools.qt_prefix}",
        f"-DQT2WASM_GEN_DIR={gen}",
        f"-DQT2WASM_QPA_PLATFORM={args.qpa}",
        f"-DQT2WASM_GUI_WORLD={gui or ''}",
    ]
    if tools.compat:
        cfg.append(f"-DQT2WASM_COMPAT_DIR={tools.compat}")
    # Configure again only when the arguments, the toolchain or the set of
    # generated files (Qt2Wasm.cmake globs them) changed; ninja re-runs CMake
    # by itself when a CMakeLists.txt changes.
    gen_files = sorted(str(f.relative_to(gen)) for f in gen.rglob("*") if f.is_file())
    cfg_key = digest("cmake", *map(str, cfg), *gen_files)
    if (cmk / "build.ninja").is_file() and stamps.fresh("configure", cfg_key):
        log("cmake: configured (unchanged)")
    else:
        stamps.drop("configure")
        run(cfg, env=wasm_env())
        stamps.record("configure", cfg_key, [cmk / "build.ninja"])
        timer.lap("configure")
    r = run(["cmake", "--build", cmk], env=wasm_env())
    if "no work to do" in r.stdout:
        log("cmake: up to date")
    else:
        n = len(re.findall(r"^\[\d+/\d+\] Building", r.stdout, re.MULTILINE))
        log(f"cmake: built ({n} source(s) compiled)")
    timer.lap("compile+link")
    cores = sorted(cmk.glob("*.core.wasm"))
    if len(cores) != 1:
        raise BuildError(
            f"expected one qt2wasm_add_component target, found {[c.name for c in cores]}"
        )
    core = cores[0]
    sizes = [("linked core module", core.stat().st_size)]

    # 5. optimise
    if tools.wasm_opt and not args.no_wasm_opt:
        opt_core = bdir / "opt.core.wasm"
        cmd = [tools.wasm_opt, opt, "--strip-debug", "--strip-producers", core, "-o", opt_core]
        key = digest("wasm-opt", *map(str, cmd), core)
        if stamps.fresh("wasm-opt", key):
            log(f"wasm-opt {opt}: core unchanged, reusing {opt_core.name}")
        else:
            stamps.drop("wasm-opt")
            run(cmd)
            stamps.record("wasm-opt", key, [opt_core])
        core = opt_core
        sizes.append((f"wasm-opt {opt}", core.stat().st_size))
        timer.lap("wasm-opt")
    else:
        log("wasm-opt not found or disabled; skipping")

    # 6. componentise
    cmd = [tools.wasm_tools, "component", "new", core, "-o", out]
    if tools.adapter:
        cmd[4:4] = ["--adapt", f"wasi_snapshot_preview1={tools.adapter}"]
    else:
        raise BuildError(
            "no wasi_snapshot_preview1 reactor adapter (QT2WASM_ADAPTER); "
            "get it from a wasmtime release matching your host"
        )
    key = digest("component", *map(str, cmd), core, tools.adapter, tools.wasm_tools)
    if stamps.fresh("component", key):
        log(f"component: core unchanged, {out.name} is up to date")
    else:
        stamps.drop("component")
        run(cmd)
        # 7. check
        run([tools.wasm_tools, "validate", out])
        stamps.record("component", key, [out])
    sizes.append(("component", out.stat().st_size))
    imports = imports_of(tools.wasm_tools, out)
    timer.lap("component")
    log(f"done in {time.time() - t0:.1f}s: {out}")
    for label, n in sizes:
        log(f"  {label:<20} {n / 1e6:8.2f} MB")
    log("  imports: " + (", ".join(imports) or "none"))
    project_js(args, out, stamps)
    timer.lap("js")
    if timer.summary():
        log(f"  times: {timer.summary()}")
    return out
