"""Run a WASI command module under wasmtime-py with the current dir preopened.

conda-forge has wasmtime-py but not the wasmtime CLI, so this stands in for
`wasmtime run --dir=. module.wasm args...`. Usage: python run.py module.wasm [args...]
"""
import sys

import wasmtime


def main() -> int:
    module, *args = sys.argv[1:]
    engine = wasmtime.Engine()
    store = wasmtime.Store(engine)
    linker = wasmtime.Linker(engine)
    linker.define_wasi()
    wasi = wasmtime.WasiConfig()
    wasi.argv = [module, *args]
    wasi.preopen_dir(".", ".")
    wasi.inherit_stdout()
    wasi.inherit_stderr()
    store.set_wasi(wasi)
    instance = linker.instantiate(store, wasmtime.Module.from_file(engine, module))
    try:
        instance.exports(store)["_start"](store)
    except wasmtime.ExitTrap as e:
        return e.code
    return 0


if __name__ == "__main__":
    sys.exit(main())
