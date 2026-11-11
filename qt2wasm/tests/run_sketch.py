"""Drive the sketch GUI component (`qt2wasm build --gui`) as a latha:gui host
would: start, resize, paint, input, clipboard, timers, HiDPI, quit."""

import base64
import sys
from types import SimpleNamespace

from wasmtime import Engine, Store, WasiConfig
from wasmtime.component import Component, Linker

V = "0.1.0"
HOST, APP = f"latha:gui/host@{V}", f"latha:gui/app@{V}"


def rec(**kw):
    r = SimpleNamespace()
    for k, v in kw.items():
        setattr(r, k.replace("_", "-"), v)
    return r


def main(path: str) -> int:
    engine = Engine()
    store = Store(engine)
    wasi = WasiConfig()
    wasi.inherit_stderr()
    store.set_wasi(wasi)
    linker = Linker(engine)
    linker.add_wasip2()
    log = []
    clip = {"text": "pasted"}
    funcs = {
        "request-frame": lambda _s: log.append("frame"),
        "request-tick": lambda _s, ms: log.append(("tick", ms)),
        "set-title": lambda _s, t: log.append(("title", t)),
        "set-cursor": lambda _s, c: log.append(("cursor", c)),
        "set-min-size": lambda _s, m: log.append(("min", m.width, m.height)),
        "clipboard-text": lambda _s: clip["text"],
        "set-clipboard-text": lambda _s, t: clip.__setitem__("text", t),
        "primary-text": lambda _s: "",
        "set-primary-text": lambda _s, t: None,
        "prefers-dark": lambda _s: True,
        "bell": lambda _s: None,
        "quit": lambda _s, code: log.append(("quit", code)),
    }
    with linker.root() as root, root.add_instance(HOST) as inst:
        for name, fn in funcs.items():
            inst.add_func(name, fn)
    instance = linker.instantiate(store, Component.from_file(engine, path))
    iface = instance.get_export_index(store, APP)

    def f(name):
        return lambda *a: instance.get_func(store, instance.get_export_index(store, name, iface))(store, *a)

    size = f("start")(["sketch"])
    assert (size.width, size.height) == (720, 480), vars(size)
    assert ("title", "Sketch (Qt on wasm)") in log and ("min", 320, 240) in log, log
    assert any(isinstance(e, tuple) and e[0] == "tick" for e in log), "the clock timer asks for ticks"

    def paint():
        rects = f("paint")()
        fs = f("frame-size")()
        return rects, fs

    f("resize")(640, 400, 1.0)
    rects, fs = paint()
    assert (fs.width, fs.height) == (640, 400)
    assert [(r.x, r.y, r.width, r.height) for r in rects] == [(0, 0, 640, 400)], rects
    px = base64.b64decode(f("frame-pixels")(rec(x=0, y=0, width=640, height=400)))
    assert len(px) == 640 * 400 * 4
    # dark theme background (BGRA): 0x1e1f22
    assert tuple(px[(200 * 640 + 300) * 4:][:4]) == (0x22, 0x1F, 0x1E, 0xFF), tuple(px[(200 * 640 + 300) * 4:][:4])

    key = lambda sym, text, mods=(): f("key")(rec(pressed=True, repeat=False, keysym=sym, scancode=0, modifiers=set(mods), text=text))
    key(0x68, "h")
    key(0x69, "i")
    key(0x76, "\x16", {"ctrl"})  # Ctrl+V pastes from the host clipboard
    key(0x63, "\x03", {"ctrl"})  # Ctrl+C copies the text back
    assert clip["text"] == "hipasted", clip

    ptr = lambda kind, button, buttons, x, y: f("pointer")(rec(kind=kind, button=button, buttons=set(buttons), modifiers=set(), x=x, y=y, clicks=1))
    ptr("press", "left", {"left"}, 100.0, 200.0)
    ptr("move", "none", {"left"}, 300.0, 300.0)
    ptr("release", "left", set(), 300.0, 300.0)
    rects, _ = paint()
    assert rects, "drawing repaints"
    stroke = base64.b64decode(f("frame-pixels")(rec(x=200, y=250, width=1, height=1)))
    assert stroke[2] < 0x80 and stroke[0] > 0x80, tuple(stroke)  # the blue stroke

    tick = f("tick")()
    assert tick is not None and 0 <= tick <= 1000, tick

    # HiDPI: device pixels double, the app lays out at 640x400 logical.
    f("resize")(1280, 800, 2.0)
    rects, fs = paint()
    assert (fs.width, fs.height) == (1280, 800) and rects, (vars(fs), rects)

    key(0x71, "\x11", {"ctrl"})  # Ctrl+Q quits
    assert ("quit", 0) in log, log
    log.clear()
    assert f("close")() is True  # the window manager's close: the last window goes
    assert ("quit", 0) in log, log
    print("sketch: start, paint, keys, clipboard, pointer, tick, HiDPI, quit OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
