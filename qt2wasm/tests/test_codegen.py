"""Codegen tests. Pure-Python ones always run; the rest need tools and skip
without them:
  moc              host Qt 6 moc (QT2WASM_MOC, PATH, or /usr/lib/qt6/libexec/moc)
  wasm-tools       to validate generated WIT
  wit-bindgen      + host Qt6Core (pkg-config) for the native glue test
  qdoc             (QT2WASM_QDOC, PATH, /usr/lib/qt6/bin/qdoc) for the docs test
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

from qt2wasm import docs, gluegen, model, witgen
from qt2wasm.names import kebab, snake, wit_ident

HERE = Path(__file__).parent
ROOT = HERE.parent


def tool(name, env, *extra):
    p = os.environ.get(env) or shutil.which(name)
    if p:
        return p
    for e in extra:
        if Path(e).exists():
            return str(e)
    return None


MOC = tool("moc", "QT2WASM_MOC", "/usr/lib/qt6/libexec/moc")
WASM_TOOLS = tool("wasm-tools", "QT2WASM_WASM_TOOLS")
WIT_BINDGEN = tool("wit-bindgen", "QT2WASM_WIT_BINDGEN")


@pytest.mark.parametrize(
    "cpp,wit",
    [
        ("setDitherMethod", "set-dither-method"),
        ("toRGB", "to-rgb"),
        ("RGB888", "rgb888"),
        ("value_changed", "value-changed"),
        ("parseHTTPHeader", "parse-http-header"),
        ("x", "x"),
    ],
)
def test_kebab(cpp, wit):
    assert kebab(cpp) == wit


def test_keywords_and_snake():
    assert wit_ident("type") == "%type"
    assert wit_ident("list") == "%list"
    assert snake("%type") == "type"
    assert snake("set-value") == "set_value"


def _cls(**kw):
    base = {
        "className": "Thing",
        "qualifiedClassName": "Thing",
        "object": True,
        "classInfos": [
            {"name": "qt2wasm-interface", "value": "acme:things/thing@1.2.3"}
        ],
        "constructors": [
            {"name": "Thing", "arguments": [{"name": "parent", "type": "QObject*"}]}
        ],
        "_header": "thing.h",
    }
    base.update(kw)
    return base


def test_type_mapping():
    m = model.build_model(
        [
            _cls(
                methods=[
                    {"name": "names", "returnType": "QStringList", "arguments": []},
                    {
                        "name": "bytes",
                        "returnType": "QByteArray",
                        "arguments": [{"name": "n", "type": "quint16"}],
                    },
                    {
                        "name": "maybe",
                        "returnType": "std::optional<QList<qint64>>",
                        "arguments": [],
                    },
                    {
                        "name": "area",
                        "returnType": "double",
                        "arguments": [{"name": "r", "type": "const QRectF &"}],
                    },
                ]
            )
        ]
    )
    text = witgen.generate(m)
    assert "package acme:things@1.2.3;" in text
    assert "names: func() -> list<string>;" in text
    assert "bytes: func(n: u16) -> list<u8>;" in text
    assert "maybe: func() -> option<list<s64>>;" in text
    assert "area: func(r: rectf) -> f64;" in text
    assert "record rectf {" in text


def test_refusals_name_the_member():
    with pytest.raises(model.Qt2WasmError, match=r"Thing::get.*QVariant"):
        model.build_model(
            [_cls(methods=[{"name": "get", "returnType": "QVariant", "arguments": []}])]
        )
    with pytest.raises(model.Qt2WasmError, match=r"pointer"):
        model.build_model(
            [_cls(methods=[{"name": "p", "returnType": "QObject*", "arguments": []}])]
        )
    with pytest.raises(model.Qt2WasmError, match=r"WIT has no overloads"):
        model.build_model(
            [
                _cls(
                    methods=[
                        {"name": "f", "returnType": "void", "arguments": []},
                        {
                            "name": "f",
                            "returnType": "void",
                            "arguments": [{"name": "a", "type": "int"}],
                        },
                    ]
                )
            ]
        )
    with pytest.raises(model.Qt2WasmError, match=r"Q_INVOKABLE"):
        model.build_model([_cls(constructors=[])])
    with pytest.raises(model.Qt2WasmError, match=r"no exported classes"):
        model.build_model(
            [{"className": "X", "qualifiedClassName": "X", "object": True}]
        )


def test_cloned_default_arg_methods_are_skipped():
    m = model.build_model(
        [
            _cls(
                methods=[
                    {
                        "name": "f",
                        "returnType": "void",
                        "arguments": [{"name": "a", "type": "int"}],
                    },
                    {
                        "name": "f",
                        "returnType": "void",
                        "arguments": [],
                        "isCloned": True,
                    },
                ]
            )
        ]
    )
    assert [x.wit_name for x in m.classes[0].methods] == ["f"]


def test_parse_header():
    protos = gluegen.parse_header(
        "extern void a_b(uint32_t source, w_string_t *why, int64_t at);\n"
        "bool x_method_c_pixel(x_borrow_c_t self, x_point_t *at, x_rgba_t *ret);\n"
        "x_own_c_t x_constructor_c(void);\n"
    )
    assert [p.name for p in protos["a_b"].params] == ["source", "why", "at"]
    assert (
        protos["x_method_c_pixel"].ret == "bool"
        and protos["x_method_c_pixel"].params[-1].is_ptr
    )
    assert protos["x_constructor_c"].params == []


def _moc_json(header: Path, tmp: Path) -> Path:
    out = tmp / f"moc_{header.stem}.cpp"
    subprocess.run(
        [MOC, f"-I{ROOT / 'include'}", "--output-json", str(header), "-o", str(out)],
        check=True,
    )
    return Path(str(out) + ".json")


needs_moc = pytest.mark.skipif(not MOC, reason="no host moc")
needs_wasm_tools = pytest.mark.skipif(not WASM_TOOLS, reason="no wasm-tools")


@needs_moc
@needs_wasm_tools
@pytest.mark.parametrize("example", ["counter", "canvas"])
def test_examples_generate_valid_wit(example, tmp_path):
    hdr = ROOT / "examples" / example / f"{example}.h"
    m = model.build_model(model.load_moc_json([_moc_json(hdr, tmp_path)]))
    (tmp_path / "wit").mkdir()
    (tmp_path / "wit" / "component.wit").write_text(witgen.generate(m))
    r = subprocess.run(
        [WASM_TOOLS, "component", "wit", str(tmp_path / "wit")],
        capture_output=True,
        text=True,
        check=False,
    )
    assert r.returncode == 0, r.stderr
    if example == "canvas":
        assert "flags text-options" in r.stdout and "record stroke" in r.stdout
        assert "pixel: func(at: point) -> option<rgba>;" in r.stdout


# What qdoc 6.4+ writes for a documented class (WebXML, trimmed).
WEBXML = """<?xml version="1.0" encoding="UTF-8"?>
<WebXML><document>
<class name="Thing" fullname="Thing" documented="true">
  <description><brief>A thing.</brief><para>Second paragraph.</para></description>
  <function name="Thing" meta="constructor">
    <description><para>Makes a thing.</para></description></function>
  <function name="paint" meta="plain">
    <description><para>Paints <argument>fillColor</argument> using
      <link raw="kind" type="property">kind</link>.</para>
      <see-also>size(), QWidget</see-also></description></function>
  <function name="changed" meta="signal">
    <description><para>Emitted when it changes; <argument>why</argument>.</para>
    </description></function>
  <property name="kind" documented="true">
    <description><brief>This property holds The kind of thing..</brief></description>
  </property>
  <enum name="Kind" fullname="Thing::Kind">
    <description><para>Kinds.</para>
      <list type="enum">
        <definition><term>Thing::Kind::Round</term>Round</definition>
        <item><para>Has no corners.</para></item>
        <definition><term>Thing::Kind::Square</term>Square</definition>
        <item><para>Has four.</para></item>
      </list></description></enum>
</class>
</document></WebXML>
"""


def test_qdoc_webxml_becomes_wit_docs(tmp_path):
    (tmp_path / "t.webxml").write_text(WEBXML)
    d = docs.load_webxml([tmp_path / "t.webxml"])
    assert d.prop("Thing", "kind") == "This property holds the kind of thing."
    assert d.value("Thing::Kind", "Round") == "Has no corners."
    m = model.build_model(
        [
            _cls(
                enums=[{"name": "Kind", "values": ["Round", "Square"]}],
                properties=[
                    {"name": "kind", "type": "Kind", "read": "kind", "write": "setKind"}
                ],
                methods=[
                    {
                        "name": "paint",
                        "returnType": "void",
                        "arguments": [{"name": "fillColor", "type": "QColor"}],
                    }
                ],
                signals=[
                    {
                        "name": "changed",
                        "returnType": "void",
                        "arguments": [{"name": "why", "type": "QString"}],
                    }
                ],
            )
        ]
    )
    text = witgen.generate(m, d)
    assert "/// A thing.\n///\n/// Exported from Thing" in text
    assert (
        "    /// A thing.\n    ///\n    /// Second paragraph.\n    resource thing"
        in text
    )
    assert "        /// Makes a thing.\n        constructor();" in text
    assert "/// Paints `fill-color` using `kind`." in text
    assert "/// See also: `size`, QWidget." in text
    assert "/// Sets `kind`." in text and "/// Q_PROPERTY write: Thing::setKind" in text
    assert "        /// Has no corners.\n        round," in text
    assert "    /// Emitted when it changes; `why`.\n    changed: func(" in text
    assert witgen.generate(m) == witgen.generate(m, docs.Docs())  # no docs, same WIT
    if WASM_TOOLS:
        (tmp_path / "wit").mkdir()
        (tmp_path / "wit" / "c.wit").write_text(text)
        subprocess.run(
            [WASM_TOOLS, "component", "wit", str(tmp_path / "wit")], check=True
        )


def test_doc_lines_wrap_prose_not_code():
    text = "word " * 30 + "\n\n```\n" + "x" * 100 + "\n```"
    lines = docs.doc_lines(text.strip(), "    ")
    assert all(len(ln) <= 80 for ln in lines if "xxx" not in ln)
    assert "    /// " + "x" * 100 in lines and "    ///" in lines


QDOC = docs.find_qdoc()


@needs_moc
@pytest.mark.skipif(not QDOC, reason="no qdoc")
def test_qdoc_documents_counter_example(tmp_path):
    proj = ROOT / "examples" / "counter"
    d, _ = docs.run_qdoc(
        QDOC, proj, tmp_path / "qdoc", [ROOT / "include", proj, *docs.qt_includes()]
    )
    m = model.build_model(
        model.load_moc_json([_moc_json(proj / "counter.h", tmp_path)])
    )
    text = witgen.generate(m, d)
    assert "/// A counter that steps by one or doubles." in text
    assert "/// Returns the last `n` values the counter held, oldest first." in text
    assert "/// Emitted when the count changes to `value`." in text
    assert "        /// Add one.\n        linear," in text


def _pkg(name):
    return subprocess.run(
        ["pkg-config", "--cflags", "--libs", name],
        capture_output=True,
        text=True,
        check=False,
    )


@needs_moc
@pytest.mark.skipif(
    not WIT_BINDGEN or not shutil.which("g++") or _pkg("Qt6Core").returncode != 0,
    reason="needs wit-bindgen, g++ and host Qt6Core",
)
def test_counter_glue_runs_natively(tmp_path):
    """Generated glue + runtime + Counter, compiled against host Qt, called
    the way the canonical ABI would call the exports."""
    ex = ROOT / "examples" / "counter"
    js = _moc_json(ex / "counter.h", tmp_path)
    m = model.build_model(model.load_moc_json([js]))
    (tmp_path / "wit").mkdir()
    (tmp_path / "wit" / "component.wit").write_text(witgen.generate(m))
    subprocess.run(
        [
            WIT_BINDGEN,
            "c",
            str(tmp_path / "wit"),
            "--world",
            "component",
            "--out-dir",
            str(tmp_path / "bind"),
        ],
        check=True,
        capture_output=True,
    )
    (tmp_path / "glue.cpp").write_text(
        gluegen.generate(m, tmp_path / "bind" / "component.h")
    )
    subprocess.run(
        [
            MOC,
            str(ROOT / "runtime" / "wasieventdispatcher.h"),
            "-o",
            str(tmp_path / "moc_disp.cpp"),
        ],
        check=True,
    )
    qt = _pkg("Qt6Core").stdout.split()
    exe = tmp_path / "harness"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-fPIC",
            f"-I{ROOT / 'include'}",
            f"-I{ROOT / 'runtime'}",
            f"-I{ex}",
            f"-I{tmp_path / 'bind'}",
            str(tmp_path / "glue.cpp"),
            str(tmp_path / "moc_counter.cpp"),
            str(tmp_path / "moc_disp.cpp"),
            str(ex / "counter.cpp"),
            str(ROOT / "runtime" / "runtime.cpp"),
            str(ROOT / "runtime" / "wasieventdispatcher.cpp"),
            str(HERE / "native" / "counter_harness.cpp"),
            *qt,
            "-o",
            str(exe),
        ],
        check=True,
    )
    r = subprocess.run(
        [str(exe)],
        capture_output=True,
        text=True,
        check=False,
        env={**os.environ, "QT_QPA_PLATFORM": "offscreen"},
    )
    assert r.returncode == 0, r.stderr
    assert "native glue test: OK" in r.stdout


COMPONENTS = {
    "counter": os.environ.get("QT2WASM_TEST_COUNTER"),
    "canvas": os.environ.get("QT2WASM_TEST_CANVAS"),
}


@pytest.mark.parametrize("name", ["counter", "canvas"])
def test_built_component_runs(name, tmp_path):
    """Set QT2WASM_TEST_COUNTER / QT2WASM_TEST_CANVAS to built .wasm files."""
    path = COMPONENTS[name]
    if not path:
        pytest.skip(f"QT2WASM_TEST_{name.upper()} not set")
    pytest.importorskip("wasmtime")
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        f"run_{name}", HERE / f"run_{name}.py"
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    args = (path, str(tmp_path / "out.png")) if name == "canvas" else (path,)
    assert mod.main(*args) == 0
