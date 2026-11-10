#!/usr/bin/env bash
# Build the Qt counter component with qt2wasm, generate the perry-wit bindings
# from the component's own WIT (wasi imports included), and compile the Perry
# CLI smoke test and the GTK4 app. Run inside the pixi env (`pixi run build`).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
export PERRY_NO_AUTO_FETCH=1 PERRY_NO_TELEMETRY=1
qt2wasm build ../../qt2wasm/examples/counter -o counter.wasm
rm -rf wit gen
wasm-tools component wit counter.wasm --out-dir wit
perry-wit wit gen
perry compile cli.ts -o counter-cli
perry compile app.ts -o counter-app --target linux
ls -la counter.wasm counter-cli counter-app
