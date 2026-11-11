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

# Install a prebuilt nandi-testing package (pixi global) and link its programs into /usr/bin (sudo) plus its desktop entry if the checkout has one, e.g. `just install latha-gui`; rerun to update
install pkg:
    #!/usr/bin/env bash
    set -euo pipefail
    pixi global install --force-reinstall -c conda-forge -c https://prefix.dev/nandi-testing "{{pkg}}"
    bin=${PIXI_HOME:-$HOME/.pixi}/bin
    # The programs pixi exposed for this env (its trampolines need their env,
    # so /usr/bin gets symlinks to them, not copies).
    exes=()
    for cfg in "$bin"/trampoline_configuration/*.json; do
        [ -e "$cfg" ] || continue
        grep -q "/envs/{{pkg}}/" "$cfg" && exes+=("$(basename "$cfg" .json)")
    done
    [ ${#exes[@]} -gt 0 ] || { [ -e "$bin/{{pkg}}" ] && exes=("{{pkg}}"); }
    [ ${#exes[@]} -gt 0 ] || { echo "no programs found for {{pkg}} in $bin" >&2; exit 1; }
    echo "linking ${exes[*]} into /usr/bin (needs sudo)"
    for exe in "${exes[@]}"; do
        sudo ln -sfn "$bin/$exe" "/usr/bin/$exe"
        echo "/usr/bin/$exe -> $bin/$exe"
    done
    # A desktop entry in the checkout (e.g. latha-gui/latha-gui.desktop) makes the
    # program a file handler: it shows up in Open With, and becomes the default for
    # its MIME types unless you already picked one.
    desktop="{{root}}/{{pkg}}/{{pkg}}.desktop"
    [ -e "$desktop" ] || exit 0
    sudo install -Dm644 "$desktop" "/usr/share/applications/{{pkg}}.desktop"
    echo "installed /usr/share/applications/{{pkg}}.desktop"
    mime="{{root}}/{{pkg}}/{{pkg}}-mime.xml"
    if [ -e "$mime" ]; then
        sudo install -Dm644 "$mime" "/usr/share/mime/packages/{{pkg}}.xml"
        command -v update-mime-database >/dev/null && sudo update-mime-database /usr/share/mime
    fi
    command -v update-desktop-database >/dev/null && sudo update-desktop-database -q /usr/share/applications
    command -v xdg-mime >/dev/null && for t in $(sed -n 's/^MimeType=//p' "$desktop" | tr ';' ' '); do
        cur=$(xdg-mime query default "$t" 2>/dev/null || true)
        if [ -z "$cur" ]; then
            xdg-mime default "{{pkg}}.desktop" "$t" && echo "default for $t: {{pkg}}.desktop"
        fi
    done
    # Plasma reads its own service cache; rebuild it so Open With sees the entry now.
    for k in kbuildsycoca6 kbuildsycoca5; do
        command -v "$k" >/dev/null && { "$k" >/dev/null 2>&1 || true; break; }
    done
    true

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
