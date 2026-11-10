#!/usr/bin/env bash
# Forge CI: install example/ from its lock (the published qt6-wasi from
# nandi-testing, wasmtime-py from conda-forge), build it and run it.
set -euo pipefail
curl -fsSL https://github.com/prefix-dev/pixi/releases/download/v0.84.1/pixi-x86_64-unknown-linux-musl.tar.gz | tar -xz -C /usr/local/bin pixi
cd example
pixi run --frozen example
test -s qt6-wasi-example.png
