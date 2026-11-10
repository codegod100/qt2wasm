#!/usr/bin/env bash
# recipes/perry-component-host test: generate bindings with perry-wit, compile a
# perry/component program against the archive (offline) and run a WAT
# component on wasmtime through it, with a TypeScript host import.
set -euxo pipefail
export PERRY_NO_AUTO_FETCH=1 PERRY_NO_TELEMETRY=1
: "${PERRY_LIB_DIR:?activation should set PERRY_LIB_DIR}"
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
cp "$here"/calc.* "$work/"
cd "$work"
wasm-tools parse calc.wat -o calc.wasm
wasm-tools validate calc.wasm
perry-wit calc.wit gen calc-world
test -f gen/bindings.ts
perry compile calc.ts -o calc
test "$(./calc calc.wasm)" = "add: 42"
