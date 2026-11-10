"""Read moc's metatype JSON (``moc --output-json``) into a small model of
what qt2wasm exports, and map Qt/C++ types onto WIT types.

Only classes that carry ``QT2WASM_EXPORT("pkg:ns/iface@x.y.z")`` (a
``Q_CLASSINFO("qt2wasm-interface", ...)``) become WIT resources. ``Q_GADGET``
classes used by them become records.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

from qt2wasm.names import kebab, wit_ident

CLASSINFO_KEY = "qt2wasm-interface"


class Qt2WasmError(Exception):
    """A declaration qt2wasm cannot map; the message names it."""


# --------------------------------------------------------------------------
# Types

PRIMITIVES = {
    "bool": "bool",
    "char": "s8",
    "signed char": "s8",
    "uchar": "u8",
    "unsigned char": "u8",
    "qint8": "s8",
    "quint8": "u8",
    "int8_t": "s8",
    "uint8_t": "u8",
    "short": "s16",
    "ushort": "u16",
    "unsigned short": "u16",
    "qint16": "s16",
    "quint16": "u16",
    "int16_t": "s16",
    "uint16_t": "u16",
    "int": "s32",
    "uint": "u32",
    "unsigned": "u32",
    "unsigned int": "u32",
    "qint32": "s32",
    "quint32": "u32",
    "int32_t": "s32",
    "uint32_t": "u32",
    "qint64": "s64",
    "quint64": "u64",
    "qlonglong": "s64",
    "qulonglong": "u64",
    "long long": "s64",
    "unsigned long long": "u64",
    "int64_t": "s64",
    "uint64_t": "u64",
    "qsizetype": "s64",
    "float": "f32",
    "double": "f64",
    "qreal": "f64",
    "QChar": "char",
}

# Qt value classes that map onto records in the shared `qt-types` interface.
QT_RECORDS = {
    "QPoint": ("point", [("x", "s32"), ("y", "s32")]),
    "QPointF": ("pointf", [("x", "f64"), ("y", "f64")]),
    "QSize": ("size", [("width", "s32"), ("height", "s32")]),
    "QSizeF": ("sizef", [("width", "f64"), ("height", "f64")]),
    "QRect": (
        "rect",
        [("x", "s32"), ("y", "s32"), ("width", "s32"), ("height", "s32")],
    ),
    "QRectF": (
        "rectf",
        [("x", "f64"), ("y", "f64"), ("width", "f64"), ("height", "f64")],
    ),
    "QColor": ("rgba", [("r", "u8"), ("g", "u8"), ("b", "u8"), ("a", "u8")]),
    # RGBA8888, tightly packed rows: width * height * 4 bytes.
    "QImage": ("image", [("width", "u32"), ("height", "u32"), ("pixels", "list<u8>")]),
}

REFUSED = {
    "QVariant": "QVariant has no fixed shape; use a concrete type or a Q_GADGET",
    "QVariantMap": "QVariantMap has no fixed shape; use a Q_GADGET",
    "QVariantList": "QVariantList has no fixed shape; use QList<T>",
    "QJsonValue": "pass JSON as QString",
    "QJsonObject": "pass JSON as QString",
}

LIST_TEMPLATES = ("QList", "QVector", "std::vector")


@dataclass
class WitType:
    """A mapped type: its WIT spelling and its C++ (Qt) spelling."""

    wit: str
    cpp: str
    kind: (
        str  # prim | string | bytes | list | option | qtrecord | enum | flags | gadget
    )
    inner: list[WitType] = field(default_factory=list)


def normalize(t: str) -> str:
    t = t.strip()
    t = re.sub(r"^const\s+", "", t)
    t = re.sub(r"\s*&$", "", t)
    t = re.sub(r"\s+", " ", t)
    return t.strip()


def split_template(t: str) -> tuple[str, list[str]] | None:
    m = re.fullmatch(r"([\w:]+)\s*<(.*)>", t)
    if not m:
        return None
    args, depth, cur = [], 0, ""
    for ch in m.group(2):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    return m.group(1), args


# --------------------------------------------------------------------------
# Model


@dataclass
class EnumDecl:
    owner: str  # C++ class that declares it
    name: str  # C++ name (for flags: the QFlags alias name)
    values: list[str]
    is_flag: bool
    alias: str | None = None  # flags: the underlying enum

    @property
    def wit_name(self) -> str:
        return wit_ident(self.name)

    @property
    def cpp(self) -> str:
        return f"{self.owner}::{self.name}"


@dataclass
class Param:
    name: str
    type: WitType


@dataclass
class Method:
    cpp_name: str
    wit_name: str
    params: list[Param]
    result: WitType | None
    kind: str = "method"  # method | getter | setter
    prop: str | None = None  # getter/setter: the Q_PROPERTY's C++ name


@dataclass
class Signal:
    cpp_name: str
    wit_name: str
    params: list[Param]


@dataclass
class GadgetField:
    wit_name: str
    type: WitType
    member: str | None
    read: str | None
    write: str | None


@dataclass
class Gadget:
    cpp: str
    wit_name: str
    fields: list[GadgetField]


@dataclass
class ExportClass:
    cpp: str  # qualified C++ class name
    package: str  # e.g. "demo:counter@0.1.0"
    interface: str  # e.g. "counter"
    resource: str  # WIT resource name
    ctor_params: list[Param]
    methods: list[Method]
    signals: list[Signal]
    enums: list[EnumDecl]
    header: str  # header moc read

    @property
    def events_interface(self) -> str:
        return f"{self.interface}-events"


@dataclass
class Model:
    classes: list[ExportClass]
    gadgets: dict[str, Gadget]
    uses_qt_types: set[str]
    package: str
    enums: list[EnumDecl]

    @property
    def namespace(self) -> str:
        return self.package.split(":")[0]

    @property
    def pkg_name(self) -> str:
        return self.package.split(":")[1].split("@")[0]

    @property
    def version(self) -> str | None:
        return self.package.split("@")[1] if "@" in self.package else None


def parse_interface_ref(ref: str) -> tuple[str, str]:
    """``demo:counter/counter@0.1.0`` -> (``demo:counter@0.1.0``, ``counter``)."""
    m = re.fullmatch(
        r"([a-z][a-z0-9-]*):([a-z][a-z0-9-]*)/([a-z][a-z0-9-]*)(@[0-9][\w.+-]*)?", ref
    )
    if not m:
        raise Qt2WasmError(
            f"QT2WASM_EXPORT value {ref!r} must look like 'namespace:package/interface@1.0.0'"
        )
    ns, pkg, iface, ver = m.groups()
    return f"{ns}:{pkg}{ver or ''}", iface


class _Mapper:
    def __init__(self, enums: dict[str, EnumDecl], gadgets_json: dict[str, dict]):
        self.enums = enums  # keyed by "Class::Name" and by bare name
        self.gadgets_json = gadgets_json
        self.gadgets: dict[str, Gadget] = {}
        self.qt_types: set[str] = set()
        self.used_enums: dict[str, EnumDecl] = {}

    def map(self, raw: str, where: str, owner: str | None = None) -> WitType:
        t = normalize(raw)
        if t in REFUSED:
            raise Qt2WasmError(f"{where}: {t} is not supported: {REFUSED[t]}")
        if t.endswith("*"):
            raise Qt2WasmError(
                f"{where}: pointer type {t!r} cannot cross a component boundary; "
                "return a value, or export the class as its own resource"
            )
        if t in PRIMITIVES:
            return WitType(PRIMITIVES[t], t, "prim")
        if t == "QString":
            return WitType("string", "QString", "string")
        if t == "QByteArray":
            return WitType("list<u8>", "QByteArray", "bytes")
        if t == "QStringList":
            s = WitType("string", "QString", "string")
            return WitType("list<string>", "QStringList", "list", [s])
        if t in QT_RECORDS:
            self.qt_types.add(t)
            return WitType(QT_RECORDS[t][0], t, "qtrecord")
        tmpl = split_template(t)
        if tmpl:
            name, args = tmpl
            if name in LIST_TEMPLATES and len(args) == 1:
                inner = self.map(args[0], where, owner)
                return WitType(
                    f"list<{inner.wit}>", f"{name}<{inner.cpp}>", "list", [inner]
                )
            if name == "std::optional" and len(args) == 1:
                inner = self.map(args[0], where, owner)
                return WitType(
                    f"option<{inner.wit}>",
                    f"std::optional<{inner.cpp}>",
                    "option",
                    [inner],
                )
            if name == "QFlags" and len(args) == 1:
                for e in self.enums.values():
                    if e.is_flag and (e.alias == args[0].split("::")[-1]):
                        self.used_enums[e.cpp] = e
                        return WitType(e.wit_name, e.cpp, "flags")
        enum = self.enums.get(t) or (self.enums.get(f"{owner}::{t}") if owner else None)
        if enum:
            self.used_enums[enum.cpp] = enum
            return WitType(enum.wit_name, enum.cpp, "flags" if enum.is_flag else "enum")
        if t in self.gadgets_json:
            return WitType(self._gadget(t, where).wit_name, t, "gadget")
        raise Qt2WasmError(
            f"{where}: no WIT mapping for C++ type {t!r}. Supported: primitives, QString, "
            "QByteArray, QStringList, QList/QVector<T>, std::optional<T>, "
            f"{', '.join(QT_RECORDS)}, Q_ENUM/Q_FLAG enums and Q_GADGET structs"
        )

    def _gadget(self, cpp: str, where: str) -> Gadget:
        if cpp in self.gadgets:
            return self.gadgets[cpp]
        cls = self.gadgets_json[cpp]
        g = Gadget(cpp, wit_ident(cls["className"]), [])
        self.gadgets[cpp] = g  # before fields, so self-reference fails loudly below
        for p in cls.get("properties", []):
            ft = self.map(p["type"], f"{where} -> {cpp}::{p['name']}", cpp)
            if ft.kind == "gadget" and ft.cpp == cpp:
                raise Qt2WasmError(
                    f"{cpp}::{p['name']}: recursive records are not supported"
                )
            g.fields.append(
                GadgetField(
                    wit_ident(p["name"]),
                    ft,
                    p.get("member"),
                    p.get("read"),
                    p.get("write"),
                )
            )
        if not g.fields:
            raise Qt2WasmError(
                f"Q_GADGET {cpp} has no Q_PROPERTY fields to make a record from"
            )
        return g


def _params(args: list[dict], where: str, mapper: _Mapper, owner: str) -> list[Param]:
    out = []
    for i, a in enumerate(args):
        name = a.get("name") or f"arg{i}"
        out.append(
            Param(wit_ident(name), mapper.map(a["type"], f"{where}({name})", owner))
        )
    return out


def load_moc_json(paths: list[Path]) -> list[dict]:
    """All classes from one or more moc JSON files, tagged with their header."""
    classes = []
    for p in paths:
        data = json.loads(Path(p).read_text())
        for c in data.get("classes", []):
            c = dict(c)
            c["_header"] = data.get("inputFile", "")
            classes.append(c)
    return classes


def build_model(classes: list[dict]) -> Model:
    gadgets_json = {c["qualifiedClassName"]: c for c in classes if c.get("gadget")}
    enums: dict[str, EnumDecl] = {}
    for c in classes:
        for e in c.get("enums", []):
            d = EnumDecl(
                c["qualifiedClassName"],
                e["name"],
                e["values"],
                e.get("isFlag", False),
                e.get("alias"),
            )
            enums[d.cpp] = d
    mapper = _Mapper(enums, gadgets_json)

    exports: list[ExportClass] = []
    packages = set()
    for c in classes:
        info = {ci["name"]: ci["value"] for ci in c.get("classInfos", [])}
        if CLASSINFO_KEY not in info:
            continue
        cpp = c["qualifiedClassName"]
        if not c.get("object"):
            raise Qt2WasmError(
                f"{cpp}: QT2WASM_EXPORT needs a QObject subclass with Q_OBJECT"
            )
        package, iface = parse_interface_ref(info[CLASSINFO_KEY])
        packages.add(package)

        # Constructor: the Q_INVOKABLE one with the fewest arguments, after
        # dropping a trailing `QObject *parent`.
        ctors = []
        for k in c.get("constructors", []):
            args = [
                a
                for a in k.get("arguments", [])
                if normalize(a["type"]) not in ("QObject*", "QObject *")
            ]
            ctors.append(args)
        if not ctors:
            raise Qt2WasmError(
                f"{cpp}: mark a constructor Q_INVOKABLE so qt2wasm can create instances"
            )
        ctor_args = min(ctors, key=len)
        ctor = _params(ctor_args, f"{cpp}::{c['className']}", mapper, cpp)

        methods: list[Method] = []
        seen: dict[str, str] = {}

        def add(m: Method, origin: str, seen=seen, methods=methods, cpp=cpp) -> None:
            if m.wit_name in seen:
                raise Qt2WasmError(
                    f"{cpp}: {origin} and {seen[m.wit_name]} both become WIT method "
                    f"'{m.wit_name}'; rename one (WIT has no overloads)"
                )
            seen[m.wit_name] = origin
            methods.append(m)

        reserved = {"id"}
        for p in c.get("properties", []):
            if p.get("read"):
                t = mapper.map(p["type"], f"{cpp}::{p['name']} (property)", cpp)
                add(
                    Method(p["read"], wit_ident(p["name"]), [], t, "getter", p["name"]),
                    f"property {p['name']}",
                )
                if p.get("write"):
                    add(
                        Method(
                            p["write"],
                            wit_ident("set-" + kebab(p["name"])),
                            [Param("value", t)],
                            None,
                            "setter",
                            p["name"],
                        ),
                        f"property {p['name']} setter",
                    )
        for kind in ("methods", "slots"):
            for m in c.get(kind, []):
                if m.get("access", "public") != "public" or m.get("isCloned"):
                    continue
                where = f"{cpp}::{m['name']}"
                ret = normalize(m.get("returnType", "void"))
                res = (
                    None
                    if ret in ("void", "")
                    else mapper.map(ret, f"{where} (return)", cpp)
                )
                wname = wit_ident(m["name"])
                if wname.lstrip("%") in reserved:
                    raise Qt2WasmError(
                        f"{where}: the name 'id' is reserved for the instance id"
                    )
                add(
                    Method(
                        m["name"],
                        wname,
                        _params(m.get("arguments", []), where, mapper, cpp),
                        res,
                    ),
                    where,
                )

        signals: list[Signal] = []
        names = [s["name"] for s in c.get("signals", []) if not s.get("isCloned")]
        for s in c.get("signals", []):
            if s.get("isCloned"):
                continue
            if names.count(s["name"]) > 1:
                raise Qt2WasmError(
                    f"{cpp}::{s['name']}: overloaded signals are not supported"
                )
            where = f"{cpp}::{s['name']} (signal)"
            signals.append(
                Signal(
                    s["name"],
                    wit_ident(s["name"]),
                    _params(s.get("arguments", []), where, mapper, cpp),
                )
            )

        exports.append(
            ExportClass(
                cpp=cpp,
                package=package,
                interface=iface,
                resource=wit_ident(c["className"]),
                ctor_params=ctor,
                methods=methods,
                signals=signals,
                enums=[e for e in enums.values() if e.owner == cpp],
                header=c.get("_header", ""),
            )
        )

    if not exports:
        raise Qt2WasmError(
            'no exported classes: add QT2WASM_EXPORT("namespace:package/interface@0.1.0") '
            "to a Q_OBJECT class"
        )
    if len(packages) > 1:
        raise Qt2WasmError(
            f"all exported classes must share one WIT package, got {sorted(packages)}"
        )
    ifaces = [e.interface for e in exports]
    dup = {i for i in ifaces if ifaces.count(i) > 1}
    if dup:
        raise Qt2WasmError(f"interfaces exported by more than one class: {sorted(dup)}")
    # Every enum an exported class declares, plus any enum it uses from elsewhere.
    all_enums = dict(mapper.used_enums)
    for e in exports:
        for d in e.enums:
            all_enums.setdefault(d.cpp, d)
    names = [d.wit_name for d in all_enums.values()]
    clash = {n for n in names if names.count(n) > 1}
    if clash:
        raise Qt2WasmError(
            f"enums from different classes share a WIT name: {sorted(clash)}"
        )
    return Model(
        exports,
        mapper.gadgets,
        mapper.qt_types,
        packages.pop(),
        list(all_enums.values()),
    )
