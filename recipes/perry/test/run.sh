#!/usr/bin/env bash
# recipes/perry test: a CLI program, wasm through the embedded wasmi engine,
# node:http from the bundled extension pack, and a GTK4 perry/ui app (linked,
# not run: there is no display). Compiles offline.
set -euxo pipefail
export PERRY_NO_AUTO_FETCH=1 PERRY_NO_TELEMETRY=1
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
cp "$here"/*.ts "$work/"
cd "$work"
# CI keeps only the tail of the log: on failure show the compiler's errors.
pc() {
    if ! perry compile "$@" > perry.log 2>&1; then
        # the first real errors, short enough to survive the log tail
        grep -E -i 'error|panick|unsupported|not supported|failed' perry.log \
            | grep -v -E 'strip-dedup|<unknown>|search for|failed to compile \(see' | head -15 | cut -c1-300
        exit 1
    fi
}
pc hello.ts -o hello
test "$(./hello)" = "hello from perry"
pc wasm.ts -o wasm
test "$(./wasm)" = "add: 42"
pc http.ts -o http
test "$(./http)" = "function"
pc ui.ts -o ui --target linux
test -x ui
# The UI binary resolves GTK4 from this env through its rpath.
if ldd ui | grep 'not found'; then exit 1; fi
ldd ui | grep -q "$CONDA_PREFIX/lib/libgtk-4.so"
