"""Qt doc comments -> WIT ``///`` docs, through qdoc.

qdoc reads the ``/*! ... */`` comments in a project's headers and sources
(``\\class``, ``\\fn``, ``\\property``, ``\\enum``/``\\value``, ``\\a``, ``\\l``,
``\\sa``, ...) with libclang and resolves each one to the declaration it
documents. With ``outputformats = WebXML`` it writes that as XML, keyed by the
C++ names. This module runs qdoc, reads the WebXML into :class:`Docs`, and
renders each description as plain Markdown-ish text that witgen writes as
``///`` lines above the matching WIT item.

Mapping (C++ -> WIT item that gets the text):

* ``\\class X`` -> the interface and resource exported from X, or the record
  made from a Q_GADGET X
* constructor, ``Q_INVOKABLE`` method, slot -> the resource's constructor/method
* ``\\property X::p`` -> the getter ``p`` and setter ``set-p``; a Q_GADGET
  property -> the record field
* signal -> the function in ``<iface>-events``
* ``\\enum X::E`` -> the enum/flags; each ``\\value`` -> that case
* ``\\a name`` -> `` `name` `` in WIT spelling; ``\\l`` and ``\\sa`` targets
  -> WIT names where they are members of the same class

qdoc is optional: without it the WIT is generated exactly as before.

Qt Help: :func:`run_qch` runs qdoc a second time with HTML output and a
``qhp.*`` project, which makes qdoc write a ``.qhp`` next to the pages, then
qhelpgenerator packs them into a ``.qch`` for Qt Assistant / Qt Creator.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import textwrap
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

from qt2wasm.names import kebab

QDOC_CANDIDATES = (
    "/usr/lib/qt6/bin/qdoc",  # Debian/Ubuntu qt6-documentation-tools
    "/usr/lib/qt6/libexec/qdoc",
    "/usr/lib64/qt6/bin/qdoc",
)


QHELPGENERATOR_CANDIDATES = (
    "/usr/lib/qt6/libexec/qhelpgenerator",  # Debian/Ubuntu qt6-tools-dev-tools
    "/usr/lib/qt6/bin/qhelpgenerator",
    "/usr/lib64/qt6/libexec/qhelpgenerator",
)


def _find(env: str, names, candidates) -> Path | None:
    v = os.environ.get(env)
    if v:
        return Path(v) if Path(v).exists() else None
    for name in names:
        p = shutil.which(name)
        if p:
            return Path(p)
    prefix = os.environ.get("CONDA_PREFIX")
    extra = (
        [Path(prefix) / d / n for d in ("bin", "libexec") for n in names]
        if prefix
        else []
    )
    for c in [*extra, *map(Path, candidates)]:
        if c.exists():
            return c
    return None


def find_qhelpgenerator() -> Path | None:
    """QT2WASM_QHELPGENERATOR, then qhelpgenerator on PATH, then distro paths."""
    return _find(
        "QT2WASM_QHELPGENERATOR",
        ("qhelpgenerator", "qhelpgenerator6"),
        QHELPGENERATOR_CANDIDATES,
    )


def find_qdoc() -> Path | None:
    """QT2WASM_QDOC, then qdoc/qdoc6 on PATH, then the usual distro paths."""
    return _find("QT2WASM_QDOC", ("qdoc", "qdoc6"), QDOC_CANDIDATES)


def qt_includes(qt_prefix: Path | str | None = None) -> list[Path]:
    """Qt's include dirs, so qdoc's clang can parse QObject classes. Without
    them qdoc cannot tie a comment to a member and drops it. ``qt_prefix``
    defaults to QT2WASM_QT_PREFIX (set by the qt6-wasi package)."""
    qt_prefix = qt_prefix or os.environ.get("QT2WASM_QT_PREFIX")
    if not qt_prefix:
        return []
    inc = Path(qt_prefix) / "include"
    return [p for p in (inc, inc / "QtCore", inc / "QtGui") if p.is_dir()]


@dataclass
class Docs:
    """Rendered doc text, keyed by C++ names as moc spells them."""

    classes: dict[str, str] = field(default_factory=dict)  # "ns::X"
    functions: dict[tuple[str, str], str] = field(default_factory=dict)  # (cls, name)
    properties: dict[tuple[str, str], str] = field(default_factory=dict)
    enums: dict[str, str] = field(default_factory=dict)  # "ns::X::E"
    values: dict[tuple[str, str], str] = field(default_factory=dict)  # (enum, value)

    def __bool__(self) -> bool:
        return any(
            (self.classes, self.functions, self.properties, self.enums, self.values)
        )

    def cls(self, cpp: str) -> str:
        return self.classes.get(cpp, "")

    def fn(self, cls: str, name: str) -> str:
        return self.functions.get((cls, name), "")

    def prop(self, cls: str, name: str) -> str:
        return self.properties.get((cls, name), "")

    def enum(self, cpp: str) -> str:
        return self.enums.get(cpp, "")

    def value(self, enum: str, name: str) -> str:
        return self.values.get((enum, name), "")


# --------------------------------------------------------------------------
# Running qdoc


def qdocconf(
    project: Path,
    out: Path,
    includes: list[Path],
    exclude: list[Path],
    fmt: str = "WebXML",
    extra: list[str] = (),
) -> str:
    def q(p: Path) -> str:
        # qdocconf values are space-separated lists: escape the spaces.
        return str(Path(p).resolve()).replace(" ", "\\ ")

    lines = [
        f"project = {project.name or 'component'}",
        f"headerdirs = {q(project)}",
        f"sourcedirs = {q(project)}",
        "headers.fileextensions = *.h *.hpp *.hxx *.hh",
        "sources.fileextensions = *.cpp *.cxx *.cc *.qdoc",
        f"outputdir = {q(out)}",
        f"outputformats = {fmt}",
        "WebXML.quotinginformation = false",
    ]
    if exclude:
        lines.append("excludedirs = " + " ".join(q(e) for e in exclude))
    if includes:
        lines.append("includepaths = " + " ".join(f"-I{q(i)}" for i in includes))
    lines.extend(extra)
    return "\n".join(lines) + "\n"


def run_qdoc(
    qdoc: Path,
    project: Path,
    work: Path,
    includes: list[Path] = (),
    exclude: list[Path] = (),
) -> tuple[Docs, str]:
    """Run qdoc over ``project`` into ``work``; return the docs and its log."""
    out = work / "webxml"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    conf = work / "qt2wasm.qdocconf"
    conf.write_text(qdocconf(project, out, list(includes), list(exclude)))
    r = subprocess.run(
        [str(qdoc), "-outputdir", str(out), str(conf)],
        capture_output=True,
        text=True,
        check=False,
        cwd=work,
    )
    log = (r.stdout + r.stderr).strip()
    if r.returncode != 0:
        raise RuntimeError(f"qdoc failed ({r.returncode}):\n{log[-4000:]}")
    return load_webxml(sorted(out.rglob("*.webxml"))), log


# --------------------------------------------------------------------------
# Qt Help (.qch)


def help_id(name: str) -> str:
    """A name safe as a qhp project id and namespace part."""
    return re.sub(r"[^A-Za-z0-9_]+", "_", name).strip("_") or "component"


def qhp_conf(name: str, version: str = "") -> list[str]:
    """qdocconf lines that make qdoc's HTML output also write ``<name>.qhp``."""
    pid = help_id(name)
    ns = f"org.qt2wasm.{pid}" + (f".{help_id(version)}" if version else "")
    k = f"qhp.{pid}"
    lines = [
        f"qhp.projects = {pid}",
        f"{k}.file = {pid}.qhp",
        f"{k}.namespace = {ns}",
        f"{k}.virtualFolder = {pid}",
        f"{k}.indexTitle = {name}",
        f"{k}.subprojects = classes",
        f"{k}.subprojects.classes.title = Classes",
        f"{k}.subprojects.classes.indexTitle = {name}",
        f"{k}.subprojects.classes.selectors = class",
        f"{k}.subprojects.classes.sortPages = true",
    ]
    if version:
        lines.insert(0, f"version = {version}")
    return lines


def module_page(name: str) -> str:
    """A \\module page for the help's front page, when the project has none.
    Classes without \\inmodule default to the project name, so they list here."""
    return (
        "/*!\n"
        f"    \\module {name}\n"
        f"    \\title {name}\n"
        f"    \\brief The {name} WASI component.\n\n"
        "    The C++ classes this component exports, with their Qt documentation.\n"
        "*/\n"
    )


def _has_module(project: Path, exclude) -> bool:
    ex = [Path(e).resolve() for e in exclude]
    for p in project.rglob("*"):
        if p.suffix not in (".cpp", ".cxx", ".cc", ".qdoc", ".h", ".hpp"):
            continue
        if any(p.resolve().is_relative_to(e) for e in ex):
            continue
        try:
            if "\\module " in p.read_text(errors="replace"):
                return True
        except OSError:
            pass
    return False


def run_qch(
    qdoc: Path,
    qhelpgenerator: Path,
    project: Path,
    work: Path,
    out: Path,
    includes: list[Path] = (),
    exclude: list[Path] = (),
    name: str | None = None,
    version: str = "",
) -> tuple[Path, str]:
    """qdoc HTML + .qhp over ``project`` in ``work``, then qhelpgenerator
    -> ``out`` (a .qch). Returns ``out`` and the combined log."""
    name = name or project.name or "component"
    html = work / "html"
    if html.exists():
        shutil.rmtree(html)
    html.mkdir(parents=True)
    extra = qhp_conf(name, version)
    if not _has_module(project, exclude):
        page = work / "qt2wasm-module.qdoc"
        page.write_text(module_page(name))
        extra.append(f"sources += {str(page.resolve()).replace(' ', chr(92) + ' ')}")
    conf = work / "qt2wasm-html.qdocconf"
    conf.write_text(
        qdocconf(project, html, list(includes), list(exclude), "HTML", extra)
    )
    r = subprocess.run(
        [str(qdoc), "-outputdir", str(html), str(conf)],
        capture_output=True,
        text=True,
        check=False,
        cwd=work,
    )
    log = (r.stdout + r.stderr).strip()
    if r.returncode != 0:
        raise RuntimeError(f"qdoc (HTML) failed ({r.returncode}):\n{log[-4000:]}")
    qhp = html / f"{help_id(name)}.qhp"
    if not qhp.exists():
        raise RuntimeError(f"qdoc wrote no {qhp.name}:\n{log[-4000:]}")
    out.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(
        [str(qhelpgenerator), str(qhp), "-o", str(out)],
        capture_output=True,
        text=True,
        check=False,
        cwd=work,
    )
    log += "\n" + (r.stdout + r.stderr).strip()
    if r.returncode != 0 or not out.exists():
        raise RuntimeError(f"qhelpgenerator failed ({r.returncode}):\n{log[-4000:]}")
    return out, log


# --------------------------------------------------------------------------
# WebXML -> Docs


def load_webxml(paths) -> Docs:
    docs = Docs()
    for p in paths:
        root = ET.parse(p).getroot()
        for cls in root.iter("class"):
            _class(cls, docs)
    return docs


def _class(cls: ET.Element, docs: Docs) -> None:
    cpp = cls.get("fullname") or cls.get("name", "")
    if not cpp:
        return
    members = {
        c.get("name"): c.tag
        for c in cls
        if c.tag in ("function", "property", "enum") and c.get("name")
    }
    r = _Renderer(members)
    text = r.render(cls.find("description"))
    if text:
        docs.classes[cpp] = text
    for c in cls:
        name = c.get("name")
        if not name:
            continue
        desc = c.find("description")
        if c.tag == "function":
            text = r.render(desc)
            if text and (cpp, name) not in docs.functions:
                docs.functions[(cpp, name)] = text
        elif c.tag == "property":
            text = r.render(desc)
            if text:
                docs.properties[(cpp, name)] = _property_text(text)
        elif c.tag == "enum":
            enum = f"{cpp}::{name}"
            text = r.render(desc, skip_enum_list=True)
            if text:
                docs.enums[enum] = text
            for vname, vtext in r.enum_values(desc):
                docs.values[(enum, vname)] = vtext


def _property_text(text: str) -> str:
    # qdoc turns "\brief the current count" into "This property holds the
    # current count."; a brief that ends in a period gets a second one.
    text = text.replace("..", ".")
    head = "This property holds "
    rest = text[len(head) :]
    # "The current count" -> "the current count", but leave "URL ..." alone.
    if text.startswith(head) and rest[:1].isupper() and not rest[1:2].isupper():
        text = head + rest[0].lower() + rest[1:]
    return text


class _Renderer:
    """One class's WebXML description -> text. ``members`` lets \\l and \\sa
    to a member of the same class print its WIT name."""

    def __init__(self, members: dict[str, str]):
        self.members = members

    def wit_ref(self, raw: str) -> str | None:
        raw = raw.strip()
        name = raw.split("::")[-1].removesuffix("()").strip()
        # A member qdoc knows, or anything written as a call: `fooBar()`.
        if name.isidentifier() and (name in self.members or raw.endswith("()")):
            return kebab(name)
        return None

    def render(self, desc: ET.Element | None, skip_enum_list: bool = False) -> str:
        if desc is None:
            return ""
        blocks: list[str] = []
        sees: list[str] = []
        for el in desc:
            if el.tag == "see-also":
                sees.extend(self.see_also(el))
            elif el.tag == "list" and el.get("type") == "enum" and skip_enum_list:
                continue
            else:
                b = self.block(el)
                if b:
                    blocks.append(b)
        # A \brief is repeated as the description's first paragraph only when
        # qdoc says so; drop an exact duplicate.
        out: list[str] = []
        for b in blocks:
            if not out or out[-1] != b:
                out.append(b)
        if sees:
            out.append("See also: " + ", ".join(s for s in sees if s) + ".")
        return "\n\n".join(out).strip()

    def see_also(self, el: ET.Element) -> list[str]:
        """``\\sa`` targets: WIT names for members of this class, else as written."""
        if len(el):  # <link> children
            items = [self.inline_el(c) for c in el if c.tag == "link"]
        else:
            items = [x.strip() for x in (el.text or "").split(",")]
        out = []
        for x in items:
            if not x:
                continue
            ref = None if x.startswith("`") else self.wit_ref(x)
            out.append(f"`{ref}`" if ref else x)
        return out

    def block(self, el: ET.Element) -> str:
        t = el.tag
        if t in ("brief", "para"):
            return _squash(self.inline(el))
        if t in ("code", "codeline", "badcode", "qml", "js"):
            body = (el.text or "").strip("\n")
            return f"```\n{body}\n```" if body.strip() else ""
        if t == "list":
            items = []
            n = 0
            kind = el.get("type", "")
            term = None
            for it in el:
                if it.tag == "definition":
                    term = _squash(self.inline(it))
                    continue
                if it.tag != "item":
                    continue
                n += 1
                body = " ".join(self.block(c) or _squash(self.inline(c)) for c in it)
                body = _squash(body or self.inline(it))
                mark = f"{n}." if kind in ("ordered", "numeric") else "-"
                items.append(f"{mark} {term + ': ' if term else ''}{body}")
                term = None
            return "\n".join(items)
        if t in ("note", "warning"):
            return f"{t.capitalize()}: " + _squash(self.inline(el))
        if t in ("table", "quotation", "section"):
            parts = [self.block(c) or _squash(self.inline(c)) for c in el]
            return "\n\n".join(p for p in parts if p)
        if t in ("heading", "title"):
            return _squash(self.inline(el))
        return _squash(self.inline(el))

    def inline(self, el: ET.Element) -> str:
        s = [el.text or ""]
        for c in el:
            s.append(self.inline_el(c))
            s.append(c.tail or "")
        return "".join(s)

    def inline_el(self, c: ET.Element) -> str:
        t = c.tag
        text = self.inline(c)
        if t == "argument":
            return f"`{kebab(text.strip())}`"
        if t in ("teletype", "code"):
            return f"`{text.strip()}`"
        if t == "link":
            if c.get("type") in ("function", "property", "enum", "class"):
                ref = self.wit_ref(c.get("raw") or text)
                if ref:
                    return f"`{ref}`"
            return text
        if t == "bold":
            return f"**{text}**"
        if t == "italic":
            return f"*{text}*"
        if t in ("brief", "para"):
            return text + " "
        return text

    def enum_values(self, desc: ET.Element | None):
        if desc is None:
            return
        for lst in desc.iter("list"):
            if lst.get("type") != "enum":
                continue
            name = None
            for it in lst:
                if it.tag == "definition":
                    term = it.find("term")
                    raw = (term.text if term is not None else "") or ""
                    name = raw.split("::")[-1].strip() or (it.text or "").strip()
                    # <definition><term>X::E::V</term>V</definition>
                    if term is not None and term.tail and term.tail.strip():
                        name = term.tail.strip()
                elif it.tag == "item" and name:
                    text = _squash(
                        " ".join(self.block(c) or self.inline(c) for c in it)
                        or self.inline(it)
                    )
                    if text:
                        yield name, text
                    name = None


def _squash(s: str) -> str:
    return " ".join(s.split())


def doc_lines(text: str, indent: str, width: int = 80) -> list[str]:
    """``text`` as ``///`` lines at ``indent``, prose wrapped to ``width``;
    code blocks and list items keep their lines, blank lines become ``///``."""
    if not text:
        return []
    room = max(40, width - len(indent) - 4)
    lines: list[str] = []
    code = False
    for line in text.splitlines():
        if line.startswith("```"):
            code = not code
            lines.append(line)
        elif code or not line.strip():
            lines.append(line)
        else:
            lead = re.match(r"(- |\d+\. )?", line).group(0)
            wrapped = textwrap.wrap(
                line,
                room,
                subsequent_indent=" " * len(lead),
                break_long_words=False,
                break_on_hyphens=False,
            )
            lines.extend(wrapped or [line])
    return [f"{indent}///" + (f" {ln}" if ln.strip() else "") for ln in lines]
