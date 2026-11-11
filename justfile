# qt2wasm: every command is a pixi task (pixi.toml [tasks]); this justfile only
# aliases them, so `just test` == `pixi run --frozen test`. The tasks chain their
# build steps with `depends-on`, so any recipe builds whatever it needs first.
# Heavy builds (Qt itself, qt6-wasi) happen in CI on Depot, not here.

set dotenv-load := false

root := justfile_directory()
# --frozen: use pixi.lock as it is, so a run never re-solves the env. The explicit
# manifest keeps another project's pixi env out of it.
pixi := "pixi run --frozen --manifest-path " + root / "pixi.toml"

default:
    @just --list

# --- pixi environment --------------------------------------------------------

# Install the dev env (prebuilt toolchain packages, Python, pytest, rattler-build)
setup *args="":
    pixi install --frozen --manifest-path {{root}}/pixi.toml {{args}}

# Show how pixi sees this workspace (envs, deps, tasks)
info:
    pixi info --manifest-path {{root}}/pixi.toml
    pixi task list --manifest-path {{root}}/pixi.toml

# Re-solve the env into pixi.lock (after a dependency or channel change)
lock:
    pixi lock --manifest-path {{root}}/pixi.toml

# Add a dependency, e.g. `just add rich`
add *args:
    pixi add --manifest-path {{root}}/pixi.toml {{args}}

# --- qt2wasm (from this checkout) --------------------------------------------

# Run qt2wasm, e.g. `just qt2wasm build path/to/project -o app.wasm`
qt2wasm *args:
    {{pixi}} qt2wasm {{args}}

# Build the counter and canvas examples into qt2wasm/build/ (cached)
examples:
    {{pixi}} examples

# Run both examples under wasmtime; saves qt2wasm/build/canvas.png
run:
    {{pixi}} run

# qt2wasm tests: codegen, WIT validation and the built examples, e.g. `just test -k wit`
test *args:
    {{pixi}} test {{args}}

# --- example/ (plain Qt for WASI, no qt2wasm) --------------------------------

# Build example/ against the published qt6-wasi and run it with wasmtime
example:
    {{pixi}} example

# --- packages (into out/ and qt2wasm/dist/) ----------------------------------

# rattler-build one recipe from recipes/, e.g. `just build wasm-tools`
build recipe:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{recipe}}" = qt6-wasi ] && [ "${QT_LOCAL_BUILD:-}" != 1 ]; then
        echo "qt6-wasi compiles Qt (30+ min); CI on Depot builds it on push to main." >&2
        echo "To build it here anyway: QT_LOCAL_BUILD=1 just build qt6-wasi" >&2
        exit 1
    fi
    {{pixi}} build-pkg {{recipe}}

# Build the light toolchain packages (all of recipes/ but qt6-wasi)
build-toolchain:
    {{pixi}} build-toolchain

# Build the qt2wasm conda package with pixi-build
build-qt2wasm:
    {{pixi}} build-qt2wasm

# Build every package except qt6-wasi
build-all:
    {{pixi}} build-all

# --- installing onto your system ---------------------------------------------

# Put a prebuilt package from nandi-testing on your PATH (~/.pixi/bin, pixi global), e.g. `just install latha-gui`; rerun to update
install pkg:
    pixi global install --force-reinstall -c conda-forge -c https://prefix.dev/nandi-testing {{pkg}}

# --- publishing (prefix.dev/nandi-testing) -----------------------------------
# CI publishes on every push to main; these run the same scripts locally and
# need PREFIX_TOKEN (prefix.dev API key with write access to nandi-testing).

# Which packages are already published (no token needed)
status:
    {{pixi}} publish-status

# Build + upload unpublished toolchain packages (needs PREFIX_TOKEN)
publish-toolchain:
    {{pixi}} publish-toolchain

# Build + upload qt2wasm if its version is new (needs PREFIX_TOKEN)
publish-qt2wasm:
    {{pixi}} publish-qt2wasm

# Both, toolchain first (needs PREFIX_TOKEN)
publish:
    {{pixi}} publish

# Remove build output
clean:
    {{pixi}} clean
    find {{root}} -name __pycache__ -type d -prune -not -path '*/.pixi/*' -exec rm -rf {} +
