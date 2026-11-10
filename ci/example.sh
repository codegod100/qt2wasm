#!/usr/bin/env bash
# Forge CI: install example/ (the published qt6-wasi and wasmtime from
# nandi-testing), build it and run it with the wasmtime CLI.
# No lockfile yet: pixi solves here. Lock it once wasmtime is published.
set -euo pipefail
curl -fsSL https://github.com/prefix-dev/pixi/releases/download/v0.84.1/pixi-x86_64-unknown-linux-musl.tar.gz | tar -xz -C /usr/local/bin pixi
cd example
pixi run example
test -s qt6-wasi-example.png
