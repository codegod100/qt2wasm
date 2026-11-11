#!/usr/bin/env bash
# Build tests/proc-test.c for wasm32-wasip1 and run it with latha-run
# (latha:proc processes and channels, wasm exceptions, tail calls).
#   WASI_SDK=/path/to/wasi-sdk LATHA_RUN=build/cargo/release/latha-run bash latha-gui/tests/proc-test.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
sdk=${WASI_SDK:-${WASI_SDK_PREFIX:?set WASI_SDK to a wasi-sdk (27 or later)}}
run=${LATHA_RUN:-$here/../build/cargo/release/latha-run}
out=${TMPDIR:-/tmp}/latha-proc-test.wasm
"$sdk/bin/clang" --target=wasm32-wasip1 -O2 -mtail-call -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false \
    "$here/proc-test.c" -lsetjmp -o "$out"
"$run" "$out"
