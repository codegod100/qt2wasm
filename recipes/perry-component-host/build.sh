#!/usr/bin/env bash
# Build libperry_component_host.a and perry-wit from Perry's source at $PERRY_REV
# with Perry's pinned nightly (rustup, private to this build). See recipe.yaml.
set -euxo pipefail
src=$SRC_DIR/perry
git init -q "$src"
git -C "$src" fetch -q --depth 1 https://github.com/PerryTS/perry.git "$PERRY_REV"
git -C "$src" checkout -q FETCH_HEAD
# Load-time patch: parallel Cranelift compilation, an on-disk compiled-code
# cache ($XDG_CACHE_HOME/perry-component-host; PERRY_COMPONENT_CACHE=0 turns it
# off, PERRY_COMPONENT_CACHE_DIR moves it) and one compile per load instead of
# two when the task-exit rewrite applies. Also: Options.fuel == 0 turns fuel
# metering off (consume_fuel(false); refills become no-ops).
git -C "$src" apply "$RECIPE_DIR/load-speed.patch"

export RUSTUP_HOME=$SRC_DIR/.rustup CARGO_HOME=${CARGO_HOME_CACHE:-$SRC_DIR/.cargo}
curl -fsSL https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain "$PERRY_RUST"
export PATH=$CARGO_HOME/bin:$PATH
rustc --version

triple=x86_64-unknown-linux-gnu
# ring / zstd-sys compile C with the conda compiler; link with it too.
export CC_x86_64_unknown_linux_gnu=$CC AR_x86_64_unknown_linux_gnu=$AR
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=$CC
# The staticlib is never LTO'd (see Perry's release-packages.yml), so skip the bitcode.
export CARGO_PROFILE_PROD_LTO=false
# conda's activation sets CFLAGS/LDFLAGS for C; cargo reads RUSTFLAGS only if
# set, which would replace .cargo/config.toml's (required) unwind flags.
unset RUSTFLAGS CARGO_ENCODED_RUSTFLAGS
cd "$src"
cargo build --locked --profile prod --target $triple -p perry-component-host -p perry-wit

out=target/$triple/prod
install -Dm644 "$out/libperry_component_host.a" "$PREFIX/lib/perry/component/libperry_component_host.a"
install -Dm755 "$out/perry-wit" "$PREFIX/bin/perry-wit"
# perry's own lib lookup checks PERRY_LIB_DIR first and falls through to its
# bundled archives for everything else.
for d in activate deactivate; do mkdir -p "$PREFIX/etc/conda/$d.d"; done
cat > "$PREFIX/etc/conda/activate.d/perry-component-host.sh" <<'SH'
export PERRY_LIB_DIR="${CONDA_PREFIX}/lib/perry/component"
SH
cat > "$PREFIX/etc/conda/deactivate.d/perry-component-host.sh" <<'SH'
unset PERRY_LIB_DIR
SH
