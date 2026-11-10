#!/usr/bin/env bash
# Regenerate the checked-in example docs (`pixi run docs`). For each example,
# `qt2wasm build --docs-only` runs moc, qdoc and codegen (no compile or link)
# into qt2wasm/build/docs/, then:
#   examples/<name>/<name>.wit  the fresh WIT, when qdoc is installed (without
#                               qdoc it has no Qt docs, so the checked-in WIT
#                               is kept, with a warning)
#   examples/<name>/<name>.qch  the fresh Qt Help file, when one was made
#   examples/<name>/<name>.md   wit-bindgen markdown of that WIT
#   examples/<name>/<name>-docs/  the Zensical site (open site/index.html)
# Run from the repo root.
set -euo pipefail
out=qt2wasm/build/docs
mkdir -p "$out"
have_qdoc=$(python -c 'from qt2wasm import docs; print(1 if docs.find_qdoc() else "")')
for ex in counter canvas; do
  d=qt2wasm/examples/$ex
  if [ -n "$have_qdoc" ]; then
    python -m qt2wasm build "$d" --docs-only --site none -o "$out/$ex.wasm" \
      --build-dir "$out/$ex-build"
    cp "$out/$ex.wit" "$d/$ex.wit"
    [ -f "$out/$ex.qch" ] && cp "$out/$ex.qch" "$d/$ex.qch"
  else
    echo "docs: no qdoc (QT2WASM_QDOC or qt6-documentation-tools): keeping the checked-in $d/$ex.wit" >&2
  fi
  python -m qt2wasm markdown "$d/$ex.wit" -o "$d"
  python -m qt2wasm mkdocs "$d/$ex.wit" -o "$d/$ex-docs" --build --builder zensical
done
