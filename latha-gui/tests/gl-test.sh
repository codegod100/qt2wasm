#!/usr/bin/env bash
# Build tests/gl-test.c (a latha:gui app that draws with the host's GL through latha-gui/gl) and run it in
# latha-gui offscreen: it prints "gl-test: ... ok" and the screenshot shows a red triangle on blue.
#   WASI_SDK=/path/to/wasi-sdk LATHA_GUI=build/latha-gui ADAPTER=wasi_snapshot_preview1.reactor.wasm \
#     bash latha-gui/tests/gl-test.sh [screenshot.png]
# Needs wit-bindgen and wasm-tools on PATH, Khronos' EGL/GLES headers (GL_HEADERS: a directory with EGL/, GLES2/,
# GLES3/ and KHR/, such as ANGLE's include/), and a GL driver on the host (EGL + GLES; Mesa's llvmpipe will do).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
sdk=${WASI_SDK:-${WASI_SDK_PREFIX:?set WASI_SDK to a wasi-sdk (27 or later)}}
gui=${LATHA_GUI:-$here/../build/latha-gui}
adapter=${ADAPTER:?set ADAPTER to wasi_snapshot_preview1.reactor.wasm}
headers=${GL_HEADERS:?set GL_HEADERS to a directory with the EGL and GLES headers}
wit=$here/../../qt2wasm/gui/wit
gl=$here/../gl
out=$(mktemp -d)
trap 'rm -r "$out"' EXIT
python3 "$gl/gen.py" "$out" >/dev/null
wit-bindgen c "$wit" --world gui --out-dir "$out" >/dev/null
wit-bindgen c "$wit" --world gl-guest --out-dir "$out" >/dev/null
"$sdk/bin/clang" --target=wasm32-wasip1 -O1 -mexec-model=reactor -I"$out" -I"$gl" -I"$headers" \
    "$here/gl-test.c" "$out/gui.c" "$out/gl_guest.c" "$out/latha_gles.c" "$gl/latha_gles_custom.c" "$gl/latha_egl.c" \
    "$out/gui_component_type.o" "$out/gl_guest_component_type.o" -o "$out/gl-test.core.wasm"
wasm-tools component new "$out/gl-test.core.wasm" --adapt "wasi_snapshot_preview1=$adapter" -o "$out/gl-test.wasm"
shot=${1:-$out/gl-test.png}
QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-offscreen} "$gui" --no-cache --screenshot "$shot" --quit-after 3000 "$out/gl-test.wasm" 2>&1 \
    | tee "$out/log"
grep -q "gl-test: .*: ok" "$out/log"
echo "gl-test passed${1:+ ($shot)}"
