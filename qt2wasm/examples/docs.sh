#!/usr/bin/env bash
# Regenerate the checked-in example docs from each example's checked-in WIT:
# examples/<name>/<name>.md (wit-bindgen markdown) and the docs site
# examples/<name>/<name>-docs/ (mkdocs.yml, docs/, site/ built with Zensical).
# Run from the repo root: `pixi run docs`.
set -euo pipefail
for ex in counter canvas; do
  d=qt2wasm/examples/$ex
  python -m qt2wasm markdown "$d/$ex.wit" -o "$d"
  python -m qt2wasm mkdocs "$d/$ex.wit" -o "$d/$ex-docs" --build --builder zensical
done
