"""`qt2wasm build`: Qt CMake project in, WASI component out.

Steps (each logged with its time):
  1. find tools          wasi-sdk, Qt-for-WASI prefix + toolchain, host moc,
                         wit-bindgen, wasm-tools, wasm-opt, p1 reactor adapter
  2. moc                 every header with Q_OBJECT/Q_GADGET: C++ for the
                         wasm build and --output-json metatypes
  3. codegen             Qt-first: WIT + C++ glue from the metatypes.
                         WIT-first (--wit): bindings only, you implement them.
  4. cmake               configure + build with the Qt WASI toolchain file
  5. optimise            wasm-opt on the core module (keeps the WIT section)
  6. componentise        wasm-tools component new --adapt (preview1 reactor)
  7. check               validate, print the world and the WASI imports
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from qt2wasm import gluegen, model, witgen

ROOT = Path(__file__).resolve().parent.parent  # tools/qt2wasm
REPO = ROOT.parent.parent  # the ditherterm checkout, when vendored there


class BuildError(Exception):
    pass


def log(msg: str) -> None:
    print(f"qt2wasm: {msg}", file=sys.stderr, flush=True)


def run(cmd: list, **kw) -> subprocess.CompletedProcess:
    cmd = [str(c) for c in cmd]
    r = subprocess.run(cmd, capture_output=True, text=True, check=False, **kw)
    if r.returncode != 0:
        raise BuildError(
            f"command failed ({r.returncode}): {' '.join(cmd)}\n{r.stdout[-4000:]}{r.stderr[-4000:]}"
        )
    return r


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
    missing = []

    def need(label: str, value: Path | None, hint: str) -> Path:
        if value is None or not Path(value).exists():
            missing.append(f"  {label}: not found ({hint})")
            return Path("/nonexistent")
        return Path(value)

    qtdir = REPO / "wasm" / "qt"
    wasi_sdk = need(
        "wasi-sdk",
        Path(
            args.wasi_sdk
            or os.environ.get("WASI_SDK_PREFIX")
            or qtdir / ".build" / "wasi-sdk"
        ),
        "--wasi-sdk or WASI_SDK_PREFIX; wasi-sdk 27 for Qt 6.8",
    )
    qt_prefix = need(
        "Qt for WASI",
        Path(
            args.qt_prefix
            or os.environ.get("QT2WASM_QT_PREFIX")
            or qtdir / ".build" / "wasi"
        ),
        "--qt-prefix or QT2WASM_QT_PREFIX; `just wasm-qt-build` makes wasm/qt/.build/wasi",
    )
    qt_toolchain = need(
        "Qt WASI toolchain file",
        Path(
            args.qt_toolchain
            or os.environ.get("QT2WASM_QT_TOOLCHAIN")
            or qtdir / "cmake" / "wasi-qt.cmake"
        ),
        "--qt-toolchain or QT2WASM_QT_TOOLCHAIN (wasm/qt/cmake/wasi-qt.cmake)",
    )
    compat = qt_toolchain.parent.parent / "compat"
    moc = need(
        "host moc",
        _which(
            "moc",
            "QT2WASM_MOC",
            Path("/usr/lib/qt6/libexec/moc"),
            qtdir / ".build" / "host" / "libexec" / "moc",
        ),
        "QT2WASM_MOC; any Qt 6 moc with output revision 68 works",
    )
    wit_bindgen = need(
        "wit-bindgen",
        _which("wit-bindgen", "QT2WASM_WIT_BINDGEN"),
        "QT2WASM_WIT_BINDGEN; 0.46 tested",
    )
    wasm_tools = need(
        "wasm-tools",
        _which("wasm-tools", "QT2WASM_WASM_TOOLS"),
        "QT2WASM_WASM_TOOLS; 1.239 tested",
    )
    wasm_opt = _which("wasm-opt", "QT2WASM_WASM_OPT")
    adapter = _which("wasi_snapshot_preview1.reactor.wasm", "QT2WASM_ADAPTER")
    if adapter is None and qtdir.exists():
        found = sorted(qtdir.glob("**/wasi_snapshot_preview1.reactor*.wasm"))
        adapter = found[0] if found else None
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
    out = []
    for h in sorted(project.rglob("*.h")):
        if build in h.parents or any(
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


def subset_font(src: Path, unicodes: str, dst: Path) -> Path:
    try:
        from fontTools import subset
    except ImportError:
        log(f"fontTools not installed; embedding {src.name} whole")
        return src
    subset.main(
        [
            str(src),
            f"--unicodes={unicodes}",
            f"--output-file={dst}",
            "--layout-features=*",
            "--no-hinting",
        ]
    )
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


def build(args) -> Path:
    t0 = time.time()
    project = Path(args.project).resolve()
    if not (project / "CMakeLists.txt").exists():
        raise BuildError(f"{project} has no CMakeLists.txt")
    bdir = (
        Path(args.build_dir).resolve()
        if args.build_dir
        else project / "build" / "qt2wasm"
    )
    gen = bdir / "gen"
    if gen.exists():
        shutil.rmtree(gen)
    (gen / "bindings").mkdir(parents=True)
    (gen / "runtime").mkdir()
    out = Path(args.output).resolve() if args.output else bdir / f"{project.name}.wasm"

    tools = find_tools(args)
    log(f"Qt {tools.qt_prefix}, wasi-sdk {tools.wasi_sdk}")

    # 2. moc
    headers = find_headers(project, bdir)
    if not headers and not args.wit:
        raise BuildError("no headers with Q_OBJECT or Q_GADGET found")
    incs = [f"-I{ROOT / 'include'}", f"-I{project}"]
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
    log(f"moc: {len(headers)} header(s)")

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
        wit_dir = gen / "wit"
        wit_dir.mkdir()
        (wit_dir / "component.wit").write_text(witgen.generate(m))
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
        log(f"codegen: {len(m.classes)} class(es) -> {wit_dir / 'component.wit'}")
    if args.font:
        fonts = [Path(f).resolve() for f in args.font]
        if args.font_subset != "none":
            ranges = SUBSETS.get(args.font_subset, args.font_subset)
            fonts = [subset_font(f, ranges, gen / f"subset-{f.name}") for f in fonts]
        write_fonts(fonts, gen / "fonts.cpp")

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
    ]
    if tools.compat:
        cfg.append(f"-DQT2WASM_COMPAT_DIR={tools.compat}")
    run(cfg)
    run(["cmake", "--build", cmk])
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
        run(
            [
                tools.wasm_opt,
                opt,
                "--strip-debug",
                "--strip-producers",
                core,
                "-o",
                opt_core,
            ]
        )
        core = opt_core
        sizes.append((f"wasm-opt {opt}", core.stat().st_size))
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
    run(cmd)
    sizes.append(("component", out.stat().st_size))

    # 7. check
    run([tools.wasm_tools, "validate", out])
    imports = imports_of(tools.wasm_tools, out)
    log(f"done in {time.time() - t0:.1f}s: {out}")
    for label, n in sizes:
        log(f"  {label:<20} {n / 1e6:8.2f} MB")
    log("  imports: " + (", ".join(imports) or "none"))
    return out
