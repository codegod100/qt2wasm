# syntax=docker/dockerfile:1
# Built on Depot by ci/publish_conda.py: `pixi build` the qt2wasm package and
# export the .conda file(s). Context: qt2wasm/. The cache mounts (pixi's and
# rattler's package caches) survive between Depot builds.
FROM ghcr.io/prefix-dev/pixi:latest AS build
WORKDIR /src
COPY . .
RUN --mount=type=cache,id=qt2wasm-pixi,target=/root/.cache/rattler \
    --mount=type=cache,id=qt2wasm-pixi-build,target=/root/.cache/pixi \
    pixi --version && pixi build --path pixi.toml --output-dir /out && ls -la /out
FROM scratch
COPY --from=build /out/ /
