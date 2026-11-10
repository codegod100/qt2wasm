#!/usr/bin/env bash
# Forge CI: install the dev env (pixi.toml: the published toolchain packages,
# nothing compiled but our own code), run the qt2wasm tests and examples from
# this checkout, then build and run both examples with the published qt2wasm
# package on its own, in a throwaway env that has nothing from this checkout.
set -euo pipefail
curl -fsSL https://github.com/prefix-dev/pixi/releases/download/v0.84.1/pixi-x86_64-unknown-linux-musl.tar.gz | tar -xz -C /usr/local/bin pixi
pixi --version
pixi install
pixi run test
pixi run run

v=$(sed -n 's/^version = "\(.*\)"/\1/p' qt2wasm/pixi.toml)
font=$(ls .pixi/envs/default/fonts/DejaVuSans*.ttf | head -n 1)
pub="pixi exec -c https://prefix.dev/nandi-testing -c conda-forge -s qt2wasm==$v -- qt2wasm"
$pub build qt2wasm/examples/counter -o /tmp/pub-counter.wasm
$pub build qt2wasm/examples/canvas --font "$font" -o /tmp/pub-canvas.wasm
pixi run python qt2wasm/tests/run_counter.py /tmp/pub-counter.wasm
pixi run python qt2wasm/tests/run_canvas.py /tmp/pub-canvas.wasm /tmp/pub-canvas.png
echo "qt2wasm $v: checkout and published package both OK"
