#!/usr/bin/env bash
# The window app's wasm stack: tests/deep-stack (a latha:gui app in Rust)
# recurses 1000000 calls in start. Passes with latha-gui's default stack,
# and must trap with LATHA_GUI_WASM_STACK_MB=1 (so the check means something).
#   LATHA_GUI=build/latha-gui bash latha-gui/tests/deep-stack.sh   (needs rustup target wasm32-wasip2)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
gui=${LATHA_GUI:-$here/../build/latha-gui}
export CARGO_TARGET_DIR=${CARGO_TARGET_DIR:-$here/../build/deep-stack}
# The app's own shadow stack (linear memory) must hold the recursion too.
RUSTFLAGS="-C link-arg=-zstack-size=134217728" cargo build -q --release --target wasm32-wasip2 \
    --manifest-path "$here/deep-stack/Cargo.toml"
app=$CARGO_TARGET_DIR/wasm32-wasip2/release/deep_stack.wasm
out=${TMPDIR:-/tmp}/latha-deep-stack.png
export QT_QPA_PLATFORM=offscreen LANG=${LANG:-C.UTF-8}
"$gui" --screenshot "$out" --quit-after 2000 "$app" 2>&1 | grep -q 'recursed 1000000 calls' \
    || { echo "deep-stack: failed with the default stack" >&2; exit 1; }
if LATHA_GUI_WASM_STACK_MB=1 "$gui" --screenshot "$out" --quit-after 2000 "$app" 2>&1 | grep -q 'recursed'; then
    echo "deep-stack: a 1 MiB stack should not have been enough" >&2
    exit 1
fi
echo "deep-stack: ok"
