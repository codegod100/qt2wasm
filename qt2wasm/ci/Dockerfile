# Built on Depot by ci/publish_conda.py: `pixi build` the qt2wasm package and
# export the .conda file(s). Context: tools/qt2wasm.
FROM ghcr.io/prefix-dev/pixi:latest AS build
WORKDIR /src
COPY . .
RUN pixi --version && pixi build --path pixi.toml --output-dir /out && ls -la /out
FROM scratch
COPY --from=build /out/ /
