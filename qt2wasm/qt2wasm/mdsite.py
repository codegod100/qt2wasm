"""WIT -> Markdown (`wit-bindgen markdown`) -> MkDocs site.

`qt2wasm markdown app.wit -o docs/`   one <world>.md per WIT
`qt2wasm mkdocs app.wit... -o site/`  an MkDocs-format project (mkdocs.yml +
                                      docs/); `--build` builds it with
                                      Zensical, or MkDocs if that is all
                                      there is
`qt2wasm build` does the same for every project into <name>-docs/ (--site).

The Markdown is wit-bindgen's, so the WIT `///` docs (qdoc's, when qt2wasm
generated the WIT with qdoc) are the prose. qt2wasm only tidies it for
Python-Markdown (see `tidy`) and writes the MkDocs config around it.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from qt2wasm.model import Qt2WasmError


def find_wit_bindgen() -> Path:
    p = os.environ.get("QT2WASM_WIT_BINDGEN") or shutil.which("wit-bindgen")
    if not p:
        raise Qt2WasmError(
            "wit-bindgen not found (QT2WASM_WIT_BINDGEN or PATH; "
            "install wit-bindgen from nandi-testing, 0.46 tested)"
        )
    return Path(p)


def tidy(md: str) -> str:
    """Fix what in wit-bindgen's Markdown MkDocs renders wrong.

    - `<p>` before enum-case / field docs (see below).
    - `----` right under a doc paragraph (resource docs) turns the paragraph
      into a setext heading: put a blank line before it.
    - Runs of blank lines collapse to one.
    """
    out: list[str] = []
    for line in md.splitlines():
        if line.strip() == "----" and out and out[-1].strip():
            out.append("")
        p = re.match(r"^(\s*)<p>(.*)$", line)
        if p:
            # wit-bindgen puts case/field docs after a bare `<p>`, which
            # Python-Markdown reads as the start of a raw HTML block that
            # swallows the rest of the page. Make it a plain paragraph
            # instead: indented 4 under a list item, as Python-Markdown wants.
            if p.group(2).strip():
                out += ["", ("    " if p.group(1) else "") + p.group(2).rstrip()]
            continue
        out.append(line.rstrip())
    text = "\n".join(out).strip() + "\n"
    return re.sub(r"\n{3,}", "\n\n", text)


def package_name(wit: Path) -> str | None:
    """`demo:counter@0.1.0` from the WIT's `package` line, if it has one."""
    files = sorted(wit.glob("*.wit")) if wit.is_dir() else [wit]
    for f in files:
        m = re.search(r"^\s*package\s+([^;\s]+)\s*;", f.read_text(), re.MULTILINE)
        if m:
            return m.group(1)
    return None


def markdown(
    wit: Path,
    out_dir: Path,
    name: str | None = None,
    world: str | None = None,
    wit_bindgen: Path | None = None,
) -> Path:
    """Run `wit-bindgen markdown` on WIT (file, dir or .wasm) into out_dir/<name>.md.

    name defaults to the WIT file's stem (the world's name for a directory)."""
    wit = Path(wit)
    if not wit.exists():
        raise Qt2WasmError(f"{wit}: no such WIT file or directory")
    with tempfile.TemporaryDirectory(prefix="qt2wasm-md-") as tmp:
        cmd = [str(wit_bindgen or find_wit_bindgen()), "markdown", str(wit), "--out-dir", tmp]
        if world:
            cmd += ["--world", world]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise Qt2WasmError(f"wit-bindgen markdown {wit}:\n{r.stderr.strip()}")
        mds = sorted(Path(tmp).glob("*.md"))
        if len(mds) != 1:
            raise Qt2WasmError(f"wit-bindgen markdown wrote {[m.name for m in mds]}")
        if name is None:
            name = wit.stem if wit.is_file() else mds[0].stem
        out_dir.mkdir(parents=True, exist_ok=True)
        dest = out_dir / f"{name}.md"
        dest.write_text(tidy(mds[0].read_text()))
    return dest


def _yaml_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def have_material() -> bool:
    try:
        import material  # noqa: F401  (mkdocs-material)
    except ImportError:
        return False
    return True


BUILDERS = ("zensical", "mkdocs")


def find_builder(name: str | None = None) -> tuple[str, list[str]] | None:
    """The site generator to run on the mkdocs.yml: Zensical (Material's
    successor; it reads mkdocs.yml as is) when found, else MkDocs.
    QT2WASM_SITE_BUILDER or `name` forces one. Returns (name, argv) or None."""
    import sys

    name = name or os.environ.get("QT2WASM_SITE_BUILDER") or None
    for b in [name] if name else BUILDERS:
        exe = shutil.which(b)
        if exe:
            return b, [exe]
        try:
            __import__(b)
        except ImportError:
            continue
        return b, [sys.executable, "-m", b]
    return None


def mkdocs_project(
    wits: list[Path],
    out: Path,
    title: str | None = None,
    theme: str | None = None,
    world: str | None = None,
    wit_bindgen: Path | None = None,
    builder: str | None = None,
) -> Path:
    """Write an MkDocs project for wits into out/: mkdocs.yml, docs/index.md
    and one docs/<name>.md per WIT. Returns the mkdocs.yml path.

    docs/ is regenerated; mkdocs.yml is rewritten too (it is generated)."""
    if not wits:
        raise Qt2WasmError("mkdocs: give at least one WIT file")
    out = Path(out)
    docs = out / "docs"
    if docs.exists():
        shutil.rmtree(docs)
    docs.mkdir(parents=True)
    pages: list[tuple[str, str, str | None]] = []  # (nav title, file, package)
    for w in wits:
        w = Path(w)
        md = markdown(w, docs, world=world, wit_bindgen=wit_bindgen)
        pkg = package_name(w)
        pages.append((pkg or md.stem, md.name, pkg))

    if title is None:
        title = pages[0][0] if len(pages) == 1 else "Component API"
    lines = [
        f"# {title}",
        "",
        "API reference for the WASI component(s) below, generated by qt2wasm",
        "from the WIT (`wit-bindgen markdown`). The prose is the Qt doc",
        "comments, carried into the WIT as `///` docs.",
        "",
    ]
    for nav, f, pkg in pages:
        lines.append(f"- [{nav}]({f})" + (f": WIT package `{pkg}`" if pkg else ""))
    (docs / "index.md").write_text("\n".join(lines) + "\n")

    if theme is None:
        # Zensical renders the Material look itself; plain MkDocs needs
        # mkdocs-material installed for it.
        b = find_builder(builder)
        theme = "material" if (b and b[0] == "zensical") or have_material() else "mkdocs"
    yml = [
        "# Generated by qt2wasm. Regenerate rather than edit.",
        "# Build with `zensical build` (or `mkdocs build`).",
        f"site_name: {_yaml_str(title)}",
        "docs_dir: docs",
        "site_dir: site",
        "use_directory_urls: false",
        "theme:",
        f"  name: {theme}",
        "markdown_extensions:",
        "  - toc:",
        "      permalink: true",
        "nav:",
        "  - Overview: index.md",
    ]
    yml += [f"  - {_yaml_str(nav)}: {f}" for nav, f, _ in pages]
    cfg = out / "mkdocs.yml"
    cfg.write_text("\n".join(yml) + "\n")
    return cfg


def build_site(cfg: Path, strict: bool = False, builder: str | None = None) -> Path:
    """Build the site for cfg with Zensical (or MkDocs); returns the site dir."""
    found = find_builder(builder)
    if found is None:
        want = builder or "zensical or mkdocs"
        raise Qt2WasmError(
            f"{want} not found: install zensical (conda-forge or PyPI), "
            "or mkdocs, or leave out --build"
        )
    name, cmd = found
    cmd = [*cmd, "build", "-f", str(cfg)]
    if name == "mkdocs":
        cmd.append("-q")
    if strict:
        cmd.append("--strict")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise Qt2WasmError(f"{name} build failed:\n{(r.stderr or r.stdout).strip()}")
    site = cfg.parent / "site"
    if not (site / "index.html").exists():
        raise Qt2WasmError(f"{name} build wrote no {site / 'index.html'}")
    return site
