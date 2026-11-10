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


def _have(mod: str) -> bool:
    try:
        __import__(mod)
    except ImportError:
        return False
    return True


def anchor(name: str) -> str:
    """wit-bindgen markdown's anchor id for an interface or world name."""
    return re.sub(r"[^A-Za-z0-9]", "_", name)


def _docs_before(lines: list[str], i: int) -> str:
    """The `///` doc comment right above lines[i], as text."""
    doc: list[str] = []
    j = i - 1
    while j >= 0 and lines[j].strip().startswith("///"):
        doc.insert(0, lines[j].strip()[3:].strip())
        j -= 1
    return "\n".join(doc).strip()


def wit_outline(wit: Path) -> dict:
    """Package, world, imports/exports and each interface's doc from WIT text
    (a .wit file or a directory of them). Empty when there is no text (.wasm)."""
    wit = Path(wit)
    files = sorted(wit.glob("*.wit")) if wit.is_dir() else [wit]
    files = [f for f in files if f.suffix == ".wit"]
    out = {"package": None, "world": None, "interfaces": [], "imports": [], "exports": []}
    for f in files:
        lines = f.read_text().splitlines()
        world = False
        for i, line in enumerate(lines):
            t = line.strip()
            if m := re.match(r"package\s+([^;\s]+)\s*;", t):
                out["package"] = out["package"] or m.group(1)
            elif m := re.match(r"interface\s+([\w-]+)\s*\{", t):
                out["interfaces"].append((m.group(1), _docs_before(lines, i)))
            elif m := re.match(r"world\s+([\w-]+)\s*\{", t):
                out["world"] = out["world"] or m.group(1)
                world = True
            elif world and (m := re.match(r"(import|export)\s+([^;]+);", t)):
                out[m.group(1) + "s"].append(m.group(2).strip())
            elif world and t == "}":
                world = False
    return out


def _brief(doc: str) -> str:
    """First paragraph of a doc comment, joined to one line."""
    return " ".join(doc.split("\n\n")[0].split())


def _overview(title: str, comps: list[dict]) -> str:
    lines = [f"# {title}", ""]
    if len(comps) == 1:
        c = comps[0]
        main = [d for n, d in c["outline"]["interfaces"] if n in c["outline"]["exports"]]
        if main and main[0]:
            lines += [_brief(main[0]), ""]
    lines += [
        '!!! info "Generated API reference"',
        "",
        "    qt2wasm generated these pages from the component's WIT with",
        "    `wit-bindgen markdown`. The prose is the Qt documentation (qdoc",
        "    `/*! */` comments) that qt2wasm carries into the WIT as `///` docs.",
        "",
    ]
    for c in comps:
        o = c["outline"]
        if len(comps) > 1:
            lines += [f"## {c['title']}", ""]
        if o["package"]:
            lines += [f"WIT package `{o['package']}`" + (
                f", world `{o['world']}`." if o["world"] else "."), ""]
        ifaces = dict(o["interfaces"])
        rows = []
        for kind, names in (("export", o["exports"]), ("import", o["imports"])):
            for n in names:
                short = n.split("/")[-1].split("@")[0]
                full = n if ":" in n else (
                    f"{o['package'].split('@')[0]}/{n}@{o['package'].split('@')[1]}"
                    if o["package"] and "@" in o["package"] else n
                )
                rows.append((kind, short, full, _brief(ifaces.get(short, ""))))
        if rows:
            lines += ['<div class="grid cards" markdown>', ""]
            for kind, short, full, brief in rows:
                link = f"{c['api']}#{anchor(full)}"
                lines += [
                    f"-   **[`{short}`]({link})** &nbsp; _{kind}_",
                    "",
                    "    ---",
                    "",
                    f"    {brief or full}",
                    "",
                ]
            lines += ["</div>", ""]
        lines += [
            f"[API reference]({c['api']}){{ .md-button .md-button--primary }}"
            + (f" [WIT source]({c['source']}){{ .md-button }}" if c["source"] else ""),
            "",
        ]
    return "\n".join(lines)


def _theme_yaml(theme: str, builder: str | None) -> list[str]:
    if theme != "material":
        return ["theme:", f"  name: {theme}"]
    # Zensical's default (modern) variant uses Lucide icons; mkdocs-material
    # only ships its own set.
    ic = (
        {"logo": "lucide/blocks", "auto": "lucide/sun-moon", "light": "lucide/sun",
         "dark": "lucide/moon"}
        if builder != "mkdocs"
        else {"logo": "material/puzzle", "auto": "material/brightness-auto",
              "light": "material/weather-sunny", "dark": "material/weather-night"}
    )
    return [
        "theme:",
        "  name: material",
        "  icon:",
        f"    logo: {ic['logo']}",
        "  palette:",
        "    - media: \"(prefers-color-scheme)\"",
        "      toggle:",
        f"        icon: {ic['auto']}",
        "        name: Switch to light mode",
        "    - media: \"(prefers-color-scheme: light)\"",
        "      scheme: default",
        "      primary: indigo",
        "      accent: indigo",
        "      toggle:",
        f"        icon: {ic['light']}",
        "        name: Switch to dark mode",
        "    - media: \"(prefers-color-scheme: dark)\"",
        "      scheme: slate",
        "      primary: indigo",
        "      accent: indigo",
        "      toggle:",
        f"        icon: {ic['dark']}",
        "        name: Switch to system preference",
        "  features:",
        "    - navigation.sections",
        "    - navigation.top",
        "    - navigation.footer",
        "    - navigation.tracking",
        "    - toc.follow",
        "    - search.highlight",
        "    - search.suggest",
        "    - content.code.copy",
    ]


EXTRA_CSS = """\
/* qt2wasm: tidy wit-bindgen markdown in the Material/Zensical theme. */
.md-typeset h4 code { font-size: 0.95em; padding: 0.15em 0.45em; }
.md-typeset h4 { margin-top: 2.2em; padding-top: 0.6em;
  border-top: 1px solid var(--md-default-fg-color--lightest); }
.md-typeset h5 { margin-top: 1em; text-transform: uppercase;
  letter-spacing: 0.04em; color: var(--md-default-fg-color--light); }
.md-typeset h2 { margin-top: 2.5em; }
.md-typeset .grid.cards > ul > li { display: flex; flex-direction: column; }
"""


def mkdocs_project(
    wits: list[Path],
    out: Path,
    title: str | None = None,
    theme: str | None = None,
    world: str | None = None,
    wit_bindgen: Path | None = None,
    builder: str | None = None,
) -> Path:
    """Write a docs site project for wits into out/: mkdocs.yml and docs/
    with an overview (index.md), and per WIT the API reference <name>.md and
    the WIT source <name>-wit.md. Zensical reads it as is; so does MkDocs.
    Returns the mkdocs.yml path.

    docs/ is regenerated; mkdocs.yml is rewritten too (it is generated)."""
    if not wits:
        raise Qt2WasmError("mkdocs: give at least one WIT file")
    out = Path(out)
    docs = out / "docs"
    if docs.exists():
        shutil.rmtree(docs)
    docs.mkdir(parents=True)
    comps: list[dict] = []
    for w in wits:
        w = Path(w)
        md = markdown(w, docs, world=world, wit_bindgen=wit_bindgen)
        outline = wit_outline(w)
        # wit-bindgen titles the page "World <name>"; name the package.
        text = md.read_text()
        text = re.sub(
            r"^# (<a id=\"[^\"]*\"></a>)World ([\w-]+)",
            lambda m: f"# {m.group(1)}{outline['package'] or m.group(2)} API reference",
            text,
            count=1,
        )
        md.write_text(text)
        source = None
        if w.is_file() and w.suffix == ".wit":
            source = f"{md.stem}-wit.md"
            (docs / source).write_text(
                f"# {outline['package'] or md.stem}: WIT\n\n"
                f"The WIT qt2wasm generated for this component (`{w.name}`).\n\n"
                # Pygments has no WIT lexer; WIT's syntax is close enough to
                # Rust's (keywords, `///` docs, generics) to read well.
                '```rust title="' + w.name + '"\n' + w.read_text().rstrip() + "\n```\n"
            )
        comps.append({
            "title": outline["package"] or md.stem,
            "api": md.name,
            "source": source,
            "outline": outline,
        })

    if title is None:
        title = comps[0]["title"] if len(comps) == 1 else "Component API"
    (docs / "index.md").write_text(_overview(title, comps))

    found = find_builder(builder)
    bname = found[0] if found else builder
    if theme is None:
        # Zensical renders the Material look itself; plain MkDocs needs
        # mkdocs-material installed for it.
        theme = "material" if bname == "zensical" or have_material() else "mkdocs"
    if theme == "material":
        (docs / "stylesheets").mkdir()
        (docs / "stylesheets" / "qt2wasm.css").write_text(EXTRA_CSS)

    yml = [
        "# Generated by qt2wasm. Regenerate rather than edit.",
        "# Build with `zensical build` (or `mkdocs build`).",
        f"site_name: {_yaml_str(title)}",
        f"site_description: {_yaml_str('API reference for ' + title + ', generated by qt2wasm')}",
        "docs_dir: docs",
        "site_dir: site",
        "use_directory_urls: false",
        *_theme_yaml(theme, bname),
    ]
    if theme == "material":
        yml += ["extra_css:", "  - stylesheets/qt2wasm.css"]
    yml += ["markdown_extensions:", "  - admonition", "  - attr_list", "  - md_in_html",
            "  - toc:", "      permalink: true"]
    if _have("pymdownx"):
        yml += ["  - pymdownx.highlight", "  - pymdownx.inlinehilite",
                "  - pymdownx.superfences", "  - pymdownx.details"]
    yml += ["nav:", "  - Overview: index.md"]
    for c in comps:
        sub = [f"API reference: {c['api']}"] + (
            [f"WIT source: {c['source']}"] if c["source"] else [])
        if len(comps) == 1:
            yml += [f"  - {e}" for e in sub]
        else:
            yml += [f"  - {_yaml_str(c['title'])}:"] + [f"    - {e}" for e in sub]
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
