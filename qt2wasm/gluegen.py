"""Model + wit-bindgen C header -> C++ glue.

The glue defines every export function wit-bindgen declared, calling into
the Qt class, and connects every signal to its import. Signatures are taken
verbatim from the generated header rather than re-derived, so the glue keeps
matching whatever naming wit-bindgen uses; the model says which Qt type each
parameter has, and ``qt2wasm::Conv`` (convert.h) converts it.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from qt2wasm.model import ExportClass, Model, Qt2WasmError, WitType
from qt2wasm.names import snake


@dataclass
class CParam:
    ctype: str
    name: str

    @property
    def is_ptr(self) -> bool:
        return self.ctype.rstrip().endswith("*")


@dataclass
class CProto:
    ret: str
    name: str
    params: list[CParam]

    def decl(self) -> str:
        ps = ", ".join(f"{p.ctype} {p.name}" for p in self.params) or "void"
        return f"{self.ret} {self.name}({ps})"


_PROTO = re.compile(
    r"^(?:extern\s+)?([\w\s\*]+?)\s*\b(\w+)\s*\(([^;]*)\)\s*;", re.MULTILINE
)


def parse_header(text: str) -> dict[str, CProto]:
    protos = {}
    for ret, name, params in _PROTO.findall(text):
        ps = []
        params = params.strip()
        if params and params != "void":
            for raw in params.split(","):
                raw = raw.strip()
                m = re.fullmatch(r"(.+?[\s\*])(\w+)", raw)
                if not m:
                    raise Qt2WasmError(f"cannot parse parameter {raw!r} of {name}")
                ps.append(CParam(m.group(1).strip(), m.group(2)))
        protos[name] = CProto(ret.strip(), name, ps)
    return protos


def _qt_type(t: WitType) -> str:
    return t.cpp


def _lookup(protos: dict[str, CProto], name: str) -> CProto:
    if name not in protos:
        near = [n for n in protos if n.split("_")[-1] == name.split("_")[-1]]
        raise Qt2WasmError(
            f"wit-bindgen header has no {name}; nearest: {near[:3]}. "
            "Is the wit-bindgen version supported?"
        )
    return protos[name]


def _arg_expr(p: CParam, t: WitType) -> str:
    ref = p.name if p.is_ptr else f"&{p.name}"
    return f"qt2wasm::Conv<{_qt_type(t)}>::in({ref})"


def _emit_return(
    lines: list[str], proto: CProto, result: WitType | None, call: str
) -> None:
    has_ret_ptr = bool(proto.params) and proto.params[-1].name == "ret"
    if result is None:
        lines.append(f"    {call};")
        return
    q = _qt_type(result)
    if result.kind == "option" and has_ret_ptr and proto.ret == "bool":
        inner = _qt_type(result.inner[0])
        lines.append(f"    const {q} r = {call};")
        lines.append("    if (!r) return false;")
        lines.append(f"    qt2wasm::Conv<{inner}>::out(*r, ret);")
        lines.append("    return true;")
    elif has_ret_ptr and proto.ret == "void":
        lines.append(f"    qt2wasm::Conv<{q}>::out({call}, ret);")
    elif proto.ret != "void":
        lines.append(f"    {proto.ret} out_;")
        lines.append(f"    qt2wasm::Conv<{q}>::out({call}, &out_);")
        lines.append("    return out_;")
    else:
        raise Qt2WasmError(f"{proto.name}: unexpected C return shape for {result.wit}")


def _gadget_convs(model: Model) -> list[str]:
    """Conv<> specialisations for Q_GADGET records, field by field."""
    out = []
    for g in model.gadgets.values():
        out.append(f"template <> struct qt2wasm::Conv<{g.cpp}> {{")
        out.append(f"    template <class C> static {g.cpp} in(C *c) {{")
        out.append(f"        {g.cpp} v{{}};")
        for f in g.fields:
            cf = snake(f.wit_name)
            val = f"qt2wasm::Conv<{f.type.cpp}>::in(&c->{cf})"
            if f.member:
                out.append(f"        v.{f.member} = {val};")
            elif f.write:
                out.append(f"        v.{f.write}({val});")
            else:
                raise Qt2WasmError(
                    f"{g.cpp}::{f.wit_name}: a record field needs MEMBER or WRITE"
                )
        out.append("        return v;")
        out.append("    }")
        out.append(f"    template <class C> static void out(const {g.cpp} &v, C *c) {{")
        for f in g.fields:
            cf = snake(f.wit_name)
            src = f"v.{f.member}" if f.member else f"v.{f.read}()"
            out.append(f"        qt2wasm::Conv<{f.type.cpp}>::out({src}, &c->{cf});")
        out.append("    }")
        out.append("    template <class C> static void drop(C *c) {")
        for f in g.fields:
            out.append(
                f"        qt2wasm::Conv<{f.type.cpp}>::drop(&c->{snake(f.wit_name)});"
            )
        out.append("    }")
        out.append("};")
        out.append("")
    return out


def _class_glue(model: Model, c: ExportClass, protos: dict[str, CProto]) -> list[str]:
    ns, pkg = snake(model.namespace), snake(model.pkg_name)
    exp = f"exports_{ns}_{pkg}_{snake(c.interface)}"
    res = snake(c.resource)
    rep = f"{exp}_{res}_t"
    L: list[str] = []
    w = L.append

    w(f"// ---- {c.cpp} -> {model.namespace}:{model.pkg_name}/{c.interface}")
    w(f"struct {rep} {{")
    w(f"    {c.cpp} *obj;")
    w("    uint32_t id;")
    w("};")
    w("")

    # Constructor
    ctor = _lookup(protos, f"{exp}_constructor_{res}")
    w(f"{ctor.decl()} {{")
    w("    qt2wasm::Call call_;")
    args = ", ".join(_arg_expr(p, t.type) for p, t in zip(ctor.params, c.ctor_params))
    w(f"    auto *rep = new {rep}{{new {c.cpp}({args}), qt2wasm_next_id()}};")
    imp = f"{ns}_{pkg}_{snake(c.events_interface)}"
    for s in c.signals:
        iproto = _lookup(protos, f"{imp}_{snake(s.wit_name)}")
        lam_params = ", ".join(f"{p.type.cpp} a{i}" for i, p in enumerate(s.params))
        w(
            f"    QObject::connect(rep->obj, &{c.cpp}::{s.cpp_name}, rep->obj, [id = rep->id]({lam_params}) {{"
        )
        call_args = ["id"]
        for i, (p, cp) in enumerate(zip(s.params, iproto.params[1:])):
            base = cp.ctype.rstrip("*").strip() if cp.is_ptr else cp.ctype
            w(f"        {base} c{i};")
            w(f"        qt2wasm::Conv<{p.type.cpp}>::out(a{i}, &c{i});")
            call_args.append(f"&c{i}" if cp.is_ptr else f"c{i}")
        w(f"        {iproto.name}({', '.join(call_args)});")
        for i, p in enumerate(s.params):
            w(f"        qt2wasm::Conv<{p.type.cpp}>::drop(&c{i});")
        w("    });")
    w(f"    return {exp}_{res}_new(rep);")
    w("}")
    w("")

    # Destructor
    dtor = _lookup(protos, f"{exp}_{res}_destructor")
    w(f"{dtor.decl()} {{")
    w("    qt2wasm::Call call_;")
    w(f"    delete {dtor.params[0].name}->obj;")
    w(f"    delete {dtor.params[0].name};")
    w("}")
    w("")

    # id
    idp = _lookup(protos, f"{exp}_method_{res}_id")
    w(f"{idp.decl()} {{ return {idp.params[0].name}->id; }}")
    w("")

    for m in c.methods:
        proto = _lookup(protos, f"{exp}_method_{res}_{snake(m.wit_name)}")
        self_name = proto.params[0].name
        cparams = proto.params[1 : 1 + len(m.params)]
        args = ", ".join(_arg_expr(p, t.type) for p, t in zip(cparams, m.params))
        w(f"{proto.decl()} {{")
        w("    qt2wasm::Call call_;")
        _emit_return(L, proto, m.result, f"{self_name}->obj->{m.cpp_name}({args})")
        w("}")
        w("")
    return L


def generate(
    model: Model, header: Path, bindings_header_name: str | None = None
) -> str:
    text = Path(header).read_text()
    protos = parse_header(text)
    inc = bindings_header_name or Path(header).name
    L = [
        "// Generated by qt2wasm. Do not edit; regenerate.",
        "#include <qt2wasm/convert.h>",
        "#include <qt2wasm/runtime.h>",
        "",
    ]
    for h in dict.fromkeys(c.header for c in model.classes if c.header):
        L.append(f'#include "{Path(h).name}"')
    L += [
        "",
        f'#include "{inc}"',
        "",
        "#include <QObject>",
        "",
        "static uint32_t qt2wasm_next_id() {",
        "    static uint32_t next = 0;",
        "    return ++next;",
        "}",
        "",
    ]
    L += _gadget_convs(model)
    L.append('extern "C" {')
    L.append("")
    for c in model.classes:
        L += _class_glue(model, c, protos)
    L.append('}  // extern "C"')
    return "\n".join(L) + "\n"
