"""Drive the Counter component under wasmtime: exports, signals as imports,
and the queued QTimer the reactor event loop must deliver."""

import sys

from wasmtime import Engine, Store, WasiConfig
from wasmtime.component import Component, Linker

PKG = "demo:counter"
VER = "0.1.0"


def main(path: str) -> int:
    engine = Engine()
    store = Store(engine)
    wasi = WasiConfig()
    wasi.inherit_stderr()
    store.set_wasi(wasi)
    linker = Linker(engine)
    linker.add_wasip2()
    events = []
    with (
        linker.root() as root,
        root.add_instance(f"{PKG}/counter-events@{VER}") as inst,
    ):
        inst.add_func(
            "value-changed",
            lambda _s, source, value: events.append(("value-changed", source, value)),
        )
        inst.add_func(
            "overflowed",
            lambda _s, source, why, at: events.append(("overflowed", source, why, at)),
        )
    comp = Component.from_file(engine, path)
    instance = linker.instantiate(store, comp)
    iface = instance.get_export_index(store, f"{PKG}/counter@{VER}")

    def f(name):
        return instance.get_func(store, instance.get_export_index(store, name, iface))

    c = f("[constructor]counter")(store)
    m = lambda n, *a: f(f"[method]counter.{n}")(store, c, *a)
    assert m("id") == 1
    m("set-value", 5)
    m("step")
    m("set-mode", "doubling")
    m("step")
    assert m("value") == 12, m("value")
    assert m("describe", "n=") == "n=12 (doubling)", m("describe", "n=")
    assert list(m("history", 2)) == [6.0, 12.0]
    m("set-value", 0x40000000)
    m("step")
    assert events == [
        ("value-changed", 1, 5),
        ("value-changed", 1, 6),
        ("value-changed", 1, 12),
        ("value-changed", 1, 1073741824),
        ("overflowed", 1, "past INT_MAX", 2147483648),
    ], events
    c.drop(store)
    print("counter component OK:", len(events), "signals received")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
