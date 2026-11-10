"""Drive the Canvas component (QPainter + QFont inside wasm) and save its image."""

import sys
from pathlib import Path
from types import SimpleNamespace

from wasmtime import Engine, Store, WasiConfig
from wasmtime.component import Component, Linker

PKG, VER = "demo:canvas", "0.1.0"


def rec(**kw):
    r = SimpleNamespace()
    for k, v in kw.items():
        setattr(r, k.replace("_", "-"), v)
    return r


def rgba(r, g, b, a=255):
    return rec(r=r, g=g, b=b, a=a)


def main(path: str, out_png: str | None) -> int:
    engine = Engine()
    store = Store(engine)
    wasi = WasiConfig()
    wasi.inherit_stderr()
    store.set_wasi(wasi)
    linker = Linker(engine)
    linker.add_wasip2()
    painted = []
    with linker.root() as root, root.add_instance(f"{PKG}/canvas-events@{VER}") as inst:
        inst.add_func(
            "painted",
            lambda _s, source, rect: painted.append(
                (source, rect.x, rect.y, rect.width, rect.height)
            ),
        )
    instance = linker.instantiate(store, Component.from_file(engine, path))
    iface = instance.get_export_index(store, f"{PKG}/canvas@{VER}")

    def f(name):
        return instance.get_func(store, instance.get_export_index(store, name, iface))

    c = f("[constructor]canvas")(store, 320, 120)
    m = lambda n, *a: f(f"[method]canvas.{n}")(store, c, *a)
    size = m("size")
    assert (size.width, size.height) == (320, 120)
    families = list(m("font-families"))
    assert families, "no fonts registered"
    m("clear", rgba(18, 18, 30))
    m(
        "shape",
        "star",
        rec(x=16, y=12, width=96, height=96),
        rgba(250, 200, 40),
        rec(color=rgba(255, 255, 255), width=3.0),
    )
    m(
        "shape",
        "ellipse",
        rec(x=200, y=70, width=100, height=40),
        rgba(80, 140, 255, 200),
        rec(color=rgba(0, 0, 0, 0), width=0.0),
    )
    box = m("text", rec(x=124, y=62), "Qt in wasm", 30, rgba(255, 255, 255), {"bold"})
    assert box.width > 100 and box.height > 20, (box.width, box.height)
    centre = m("pixel", rec(x=64, y=60))
    assert centre is not None and (centre.r, centre.g) == (250, 200), vars(centre)
    assert m("pixel", rec(x=999, y=0)) is None
    img = m("image")
    assert (img.width, img.height, len(img.pixels)) == (320, 120, 320 * 120 * 4)
    assert len(painted) == 4 and all(p[0] == 1 for p in painted), painted
    if out_png:
        try:
            from PySide6.QtGui import QImage
        except ImportError:
            import struct
            import zlib

            raw = b"".join(
                b"\0" + bytes(img.pixels[y * 1280 : (y + 1) * 1280]) for y in range(120)
            )

            def chunk(t, d):
                return (
                    struct.pack(">I", len(d))
                    + t
                    + d
                    + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
                )

            png = (
                b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 120, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw))
                + chunk(b"IEND", b"")
            )
            Path(out_png).write_bytes(png)
        else:
            QImage(bytes(img.pixels), 320, 120, QImage.Format.Format_RGBA8888).save(
                out_png
            )
    c.drop(store)
    print(
        f"canvas component OK: fonts={families}, text box={box.width}x{box.height}, {len(painted)} painted signals"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None))
