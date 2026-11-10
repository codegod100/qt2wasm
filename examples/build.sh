#!/usr/bin/env bash
# Build the counter (QtCore) and canvas (QtGui) examples into tools/qt2wasm/build/
# (`pixi run qt2wasm-examples`). Needs the Qt-for-WASI build in wasm/qt/.build.
# The canvas example embeds a TTF: QT2WASM_FONT, else the first DejaVu/Arial found.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
q=$(cd "$here/.." && pwd)
out=$q/build
font=${QT2WASM_FONT:-}
if [ -z "$font" ]; then
    for f in "${CONDA_PREFIX:-}/fonts/DejaVuSans.ttf" "${CONDA_PREFIX:-}/fonts/DejaVuSansMono.ttf" \
             /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf /usr/share/fonts/TTF/DejaVuSans.ttf \
             /Library/Fonts/Arial.ttf /System/Library/Fonts/Supplemental/Arial.ttf; do
        [ -f "$f" ] && { font=$f; break; }
    done
fi
[ -n "$font" ] || { echo "no TTF found for the canvas example: set QT2WASM_FONT=/path/to/font.ttf" >&2; exit 1; }
mkdir -p "$out"
export PYTHONPATH=$q
python -m qt2wasm build "$here/counter" -o "$out/counter.wasm"
python -m qt2wasm build "$here/canvas" --font "$font" -o "$out/canvas.wasm"
