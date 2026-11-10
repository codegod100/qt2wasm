#!/usr/bin/env bash
# Regenerate the checked-in example docs (`pixi run docs`, after the
# `examples` task has built both examples into qt2wasm/build/):
#   examples/<name>/<name>.wit  from the fresh build, when qdoc is installed
#                               (without qdoc the build's WIT has no Qt docs,
#                               so the checked-in WIT is kept, with a warning)
#   examples/<name>/<name>.qch  from the fresh build, when it made one
#   examples/<name>/<name>.md   wit-bindgen markdown of that WIT
#   examples/<name>/<name>-docs/  the Zensical site (open site/index.html)
# Run from the repo root.
set -euo pipefail
build=qt2wasm/build
have_qdoc=$(python -c 'from qt2wasm import docs; print(1 if docs.find_qdoc() else "")')
for ex in counter canvas; do
  d=qt2wasm/examples/$ex
  if [ -n "$have_qdoc" ] && [ -f "$build/$ex.wit" ]; then
    cp "$build/$ex.wit" "$d/$ex.wit"
    [ -f "$build/$ex.qch" ] && cp "$build/$ex.qch" "$d/$ex.qch"
  else
    echo "docs: no qdoc (QT2WASM_QDOC or qt6-documentation-tools): keeping the checked-in $d/$ex.wit" >&2
  fi
  python -m qt2wasm markdown "$d/$ex.wit" -o "$d"
  python -m qt2wasm mkdocs "$d/$ex.wit" -o "$d/$ex-docs" --build --builder zensical
done
