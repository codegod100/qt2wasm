#!/usr/bin/env bash
# Forge CI: perry/counter, a Perry app that loads the qt2wasm counter component
# with wasmtime (perry-component-host) and draws a GTK4 UI, all from the
# published nandi-testing packages. Runs the CLI smoke test against the real
# component and links the GTK4 app (no display here, so it isn't opened).
set -euo pipefail
command -v pixi >/dev/null || curl -fsSL https://github.com/prefix-dev/pixi/releases/download/v0.84.1/pixi-x86_64-unknown-linux-musl.tar.gz | tar -xz -C /usr/local/bin pixi
cd perry/counter
pixi run build
out=$(pixi run ./counter-cli counter.wasm)
echo "$out"
grep -qx 'n=8 (doubling)' <<<"$out"
grep -q '^history \[' <<<"$out"
grep -q '^signals [0-9]*:1,' <<<"$out"
test -x counter-app
echo "perry/counter: Qt component on wasmtime from a native Perry binary OK"
