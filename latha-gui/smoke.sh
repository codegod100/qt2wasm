#!/usr/bin/env bash
# latha-gui's headless check (Qt offscreen) on APP.wasm: it starts, typed
# text goes in (a gui-pty app such as kwi runs it in its shell), scroll moves
# are verified, and the frame is saved ($2, default ./latha-gui-smoke.png).
#   bash latha-gui/smoke.sh APP.wasm [OUT.png]    (LATHA_GUI=path/to/latha-gui)
set -euo pipefail
app=${1:?usage: smoke.sh APP.wasm [OUT.png]}
out=$(realpath -m "${2:-latha-gui-smoke.png}")
log=${out%.png}.log
if [ ! -f "$app" ]; then
    echo "latha-smoke: no $app" >&2
    exit 1
fi
rm -f "$out"
QT_QPA_PLATFORM=offscreen LANG=${LANG:-C.UTF-8} "${LATHA_GUI:-latha-gui}" --verify --size 800x500 \
    --type 'echo latha-gui-smoke-$((6*7))\n' --type-after 1500 --screenshot "$out" --quit-after 4000 "$app" \
    2>&1 | tee "$log"
if grep -q "trapped\|VERIFY: moved frame differs" "$log"; then
    echo "latha-smoke: failed" >&2
    exit 1
fi
[ -s "$out" ] || { echo "latha-smoke: no screenshot" >&2; exit 1; }
echo "latha-smoke: ok ($out)"
