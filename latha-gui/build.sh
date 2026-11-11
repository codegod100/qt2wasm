#!/usr/bin/env bash
# Build latha-gui: the wasmtime host (host/, cargo) then the Qt window (CMake).
# Output: latha-gui/build/latha-gui (and latha-run, the same host without Qt). Run in the pixi `latha` env
# (pixi run -e latha latha-build), which brings Qt 6, Rust and CMake.
#   LATHA_GUI_BUILD_TYPE=Debug   a debug build of both halves
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build=$here/build
type=${LATHA_GUI_BUILD_TYPE:-Release}
profile=release
[ "$type" = Debug ] && profile=dev
mkdir -p "$build"
export CARGO_TARGET_DIR=$build/cargo
cargo build --manifest-path "$here/host/Cargo.toml" --locked --profile "$profile"
outdir=release
[ "$profile" = dev ] && outdir=debug
lib=$CARGO_TARGET_DIR/$outdir/liblatha_host.a
# A CMake cache from another env (its compiler gone) cannot be reused.
cache=$build/qt/CMakeCache.txt
if [ -f "$cache" ]; then
    cxx=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$cache")
    [ -n "$cxx" ] && [ ! -x "$cxx" ] && rm -rf "$build/qt"
fi
gen=()
command -v ninja >/dev/null && gen=(-G Ninja)
cmake -S "$here" -B "$build/qt" "${gen[@]}" -DCMAKE_BUILD_TYPE="$type" -DLATHA_HOST_LIB="$lib" \
    ${CMAKE_PREFIX_PATH:+-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH"} >/dev/null
cmake --build "$build/qt"
cp "$build/qt/latha-gui" "$build/latha-gui"
cp "$CARGO_TARGET_DIR/$outdir/latha-run" "$build/latha-run"
echo "built $build/latha-gui"
