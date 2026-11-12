#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
"""Set keys of an app's latha:app custom section (id=, name=, version=, update=; see ../README.md).

    set-app-info.py APP.wasm [OUT.wasm] key=value...

Keeps the keys it is not given (key= with nothing drops one), drops the old top-level latha:app section and
appends the new one last (custom sections may go anywhere; latha-gui reads the top level only). Writes OUT.wasm,
or APP.wasm in place. Standard library only, so a build can copy it."""
import os
import sys


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def read_leb(data, i):
    v = shift = 0
    while True:
        b = data[i]
        i += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return v, i


def main(argv):
    files = [a for a in argv if "=" not in a]
    pairs = [a.split("=", 1) for a in argv if "=" in a]
    if not files or len(files) > 2 or not pairs:
        sys.exit(__doc__)
    src, dst = files[0], files[-1]
    data = open(src, "rb").read()
    if data[:4] != b"\0asm":
        sys.exit(f"{src}: not a wasm file")
    keys, order, keep = {}, [], [data[:8]]
    i = 8
    while i < len(data):
        start = i
        sid = data[i]
        size, body = read_leb(data, i + 1)
        end = body + size
        if sid == 0:
            nlen, n0 = read_leb(data, body)
            if data[n0:n0 + nlen] == b"latha:app":
                for line in data[n0 + nlen:end].decode("utf-8", "replace").splitlines():
                    if "=" in line:
                        k, v = line.split("=", 1)
                        if k.strip() not in keys:
                            order.append(k.strip())
                        keys[k.strip()] = v.strip()
                i = end
                continue
        keep.append(data[start:end])
        i = end
    for k, v in pairs:
        if "\n" in v:
            sys.exit(f"{k}: values are one line")
        if k not in keys:
            order.append(k)
        keys[k] = v
    payload = "".join(f"{k}={keys[k]}\n" for k in order if keys[k] != "").encode()
    name = b"latha:app"
    body = leb(len(name)) + name + payload
    tmp = dst + ".tmp"
    with open(tmp, "wb") as f:
        for part in keep:
            f.write(part)
        f.write(b"\0" + leb(len(body)) + body)
    os.replace(tmp, dst)


if __name__ == "__main__":
    main(sys.argv[1:])
