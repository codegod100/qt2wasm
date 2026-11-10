"""Name conversions between C++ (camelCase), WIT (kebab-case) and C (snake_case)."""

from __future__ import annotations

import re

# Words WIT reserves; a WIT identifier that collides takes a leading %.
WIT_KEYWORDS = {
    "as",
    "async",
    "bool",
    "borrow",
    "char",
    "constructor",
    "enum",
    "export",
    "f32",
    "f64",
    "flags",
    "from",
    "func",
    "future",
    "import",
    "include",
    "interface",
    "list",
    "option",
    "own",
    "package",
    "record",
    "resource",
    "result",
    "s16",
    "s32",
    "s64",
    "s8",
    "static",
    "stream",
    "string",
    "tuple",
    "type",
    "u16",
    "u32",
    "u64",
    "u8",
    "use",
    "variant",
    "with",
    "world",
}


def kebab(name: str) -> str:
    """``setDitherMethod`` -> ``set-dither-method``, ``toRGB`` -> ``to-rgb``,
    ``RGB888`` -> ``rgb888``, ``value_changed`` -> ``value-changed``."""
    s = name.replace("_", "-")
    # aB -> a-B ; ABc -> A-Bc (acronym followed by a word)
    s = re.sub(r"([a-z0-9])([A-Z])", r"\1-\2", s)
    s = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1-\2", s)
    s = re.sub(r"-+", "-", s).strip("-").lower()
    # WIT words may not start with a digit after a dash: "rgb-888" is invalid
    # only when a part starts with a digit; join such parts to the previous one.
    parts = s.split("-")
    out: list[str] = []
    for p in parts:
        if out and p[:1].isdigit():
            out[-1] += p
        else:
            out.append(p)
    return "-".join(out)


def wit_ident(name: str) -> str:
    k = kebab(name)
    return f"%{k}" if k in WIT_KEYWORDS else k


def snake(wit_name: str) -> str:
    """WIT name -> the identifier wit-bindgen's C generator uses."""
    return wit_name.lstrip("%").replace("-", "_").replace(":", "_").replace("/", "_")
