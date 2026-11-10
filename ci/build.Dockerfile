# syntax=docker/dockerfile:1
# Built by ci/publish.py with `depot build`: rattler-build one recipe and export
# its .conda files. Context: the repo root. The cache mounts survive between
# Depot builds, so a rebuild is incremental:
#   /root/.cache/rattler   rattler's package cache (conda-forge + nandi-testing)
#   /out/src_cache         rattler-build's source cache (the qtbase tarball)
#   /root/.ccache          ccache (recipes/qt6-wasi/build.sh uses it when present)
FROM python:3.12-bookworm AS build
ARG RATTLER_BUILD_URL
RUN curl -fsSL "$RATTLER_BUILD_URL" -o /usr/local/bin/rattler-build && chmod 0755 /usr/local/bin/rattler-build \
    && rattler-build --version
COPY . /src
WORKDIR /src
ARG RECIPE
RUN --mount=type=cache,id=qt2wasm-rattler,target=/root/.cache/rattler \
    --mount=type=cache,id=qt2wasm-srccache,target=/out/src_cache \
    --mount=type=cache,id=qt2wasm-ccache,target=/root/.ccache \
    rattler-build build --recipe "recipes/$RECIPE" --output-dir /out \
        -c https://prefix.dev/nandi-testing -c conda-forge --log-style plain --color never \
    && mkdir -p /export && find /out -name '*.conda' -not -path '/out/src_cache/*' -not -path '/out/bld/*' \
        -exec cp {} /export/ \;

FROM scratch
COPY --from=build /export/ /
