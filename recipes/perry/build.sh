#!/usr/bin/env bash
# Install Perry and its extension pack from npm, rebuild the compiler binary
# from source with strip-dedup-extract.patch, compile the WebKitGTK/libshumate stubs,
# install the wrapper. See recipe.yaml.
set -euxo pipefail
V=$PKG_VERSION
TRIPLE=x86_64-unknown-linux-gnu
npm install --global --no-audit --no-fund --prefix "$PREFIX" \
    "@perryts/perry@$V" "@perryts/perry-ext-$TRIPLE@$V"
mods=$PREFIX/lib/node_modules/@perryts
native=$mods/perry/node_modules/@perryts/perry-linux-x64
[ -x "$native/bin/perry" ] || native=$mods/perry-linux-x64
test -x "$native/bin/perry"
test -f "$mods/perry-ext-$TRIPLE/target.json"

# The compiler binary itself, from source at the npm release's revision with
# strip-dedup-extract.patch (see recipe.yaml). Pinned nightly via rustup,
# private to this build; LLVM 22.1 from the host env, linked statically.
grep -q "\"source_revision\": \"$PERRY_REV\"" "$mods/perry-ext-$TRIPLE/target.json"
src=$SRC_DIR/perry-src
git init -q "$src"
git -C "$src" fetch -q --depth 1 https://github.com/PerryTS/perry.git "$PERRY_REV"
git -C "$src" checkout -q FETCH_HEAD
git -C "$src" apply "$RECIPE_DIR/strip-dedup-extract.patch"
export RUSTUP_HOME=$SRC_DIR/.rustup CARGO_HOME=${CARGO_HOME_CACHE:-$SRC_DIR/.cargo}
curl -fsSL https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain "$PERRY_RUST"
export PATH=$CARGO_HOME/bin:$PATH
export LLVM_SYS_221_PREFIX=$PREFIX LIBRARY_PATH=$PREFIX/lib${LIBRARY_PATH:+:$LIBRARY_PATH}
export CC_x86_64_unknown_linux_gnu=$CC CXX_x86_64_unknown_linux_gnu=$CXX AR_x86_64_unknown_linux_gnu=$AR
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=$CC
# RUSTFLAGS would replace .cargo/config.toml's (required) unwind flags.
unset RUSTFLAGS CARGO_ENCODED_RUSTFLAGS
(cd "$src" && cargo build --locked --profile prod --target $TRIPLE -p perry)
install -m755 "$src/target/$TRIPLE/prod/perry" "$native/bin/perry"
"$native/bin/perry" --version | grep -qx "perry $V"
rm -rf "$src/target"

# The wrapper execs the native compiler directly (no node at run time), at a
# path relative to $PREFIX.
rel=${native#"$PREFIX"/}
rm -f "$PREFIX/bin/perry"
sed "s|@NATIVE@|$rel|" "$RECIPE_DIR/perry-wrapper.sh" > "$PREFIX/bin/perry"
chmod 0755 "$PREFIX/bin/perry"

# WebKitGTK 6 / JavaScriptCoreGTK 6 / libshumate stubs: every webkit_*/jsc_*/
# shumate_* symbol the GTK4 UI archive references, as a function that aborts
# with a message. (WebKitGTK 6 is not on conda-forge; libshumate is, but it
# drags in protobuf-c -> libabseil, which nandi-testing shadows, so the env
# would not solve with nandi-testing ahead of conda-forge.)
stubs=$PREFIX/lib/perry/stubs
mkdir -p "$stubs/pkgconfig"
work=$(mktemp -d)
zstd -dq "$native/lib/libperry_ui_gtk4.a.zst" -o "$work/ui.a"
"${NM:-nm}" "$work/ui.a" 2>/dev/null | awk '$1=="U"{print $2}' | grep -E '^(webkit_|jsc_|shumate_)' | sort -u > "$work/syms"
test -s "$work/syms"
{
    echo '#include <stdio.h>'
    echo '#include <stdlib.h>'
    echo 'static void perry_stubbed(const char *f) {'
    echo '    fprintf(stderr, "perry: %s: not available in the conda perry package (WebView and MapView are stubbed out)\n", f);'
    echo '    abort();'
    echo '}'
    while read -r s; do echo "void $s(void) { perry_stubbed(\"$s\"); }"; done < "$work/syms"
} > "$work/stub.c"
$CC -O1 -fPIC -c "$work/stub.c" -o "$work/stub.o"
for lib in webkitgtk-6.0 javascriptcoregtk-6.0 shumate-1.0; do
    "${AR:-ar}" rcs "$stubs/lib$lib.a" "$work/stub.o"
done
# So `pkg-config webkitgtk-6.0` / `shumate-1.0` succeed (Perry warns otherwise).
cat > "$stubs/pkgconfig/webkitgtk-6.0.pc" <<PC
Name: webkitgtk-6.0
Description: stub (conda perry package; WebView aborts)
Version: 2.0.0
Libs: -lwebkitgtk-6.0 -ljavascriptcoregtk-6.0
Cflags:
PC
cat > "$stubs/pkgconfig/shumate-1.0.pc" <<PC
Name: shumate-1.0
Description: stub (conda perry package; MapView aborts)
Version: 1.0.0
Libs: -lshumate-1.0
Cflags:
PC
rm -rf "$work"
