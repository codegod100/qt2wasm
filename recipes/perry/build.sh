#!/usr/bin/env bash
# Install Perry and its extension pack from npm, compile the WebKitGTK stubs,
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

# The wrapper execs the native compiler directly (no node at run time), at a
# path relative to $PREFIX.
rel=${native#"$PREFIX"/}
rm -f "$PREFIX/bin/perry"
sed "s|@NATIVE@|$rel|" "$RECIPE_DIR/perry-wrapper.sh" > "$PREFIX/bin/perry"
chmod 0755 "$PREFIX/bin/perry"

# WebKitGTK 6 / JavaScriptCoreGTK 6 stubs: every webkit_*/jsc_* symbol the
# GTK4 UI archive references, as a function that aborts with a message.
stubs=$PREFIX/lib/perry/stubs
mkdir -p "$stubs/pkgconfig"
work=$(mktemp -d)
zstd -dq "$native/lib/libperry_ui_gtk4.a.zst" -o "$work/ui.a"
"${NM:-nm}" "$work/ui.a" 2>/dev/null | awk '$1=="U"{print $2}' | grep -E '^(webkit_|jsc_)' | sort -u > "$work/syms"
test -s "$work/syms"
{
    echo '#include <stdio.h>'
    echo '#include <stdlib.h>'
    echo 'static void perry_no_webkit(const char *f) {'
    echo '    fprintf(stderr, "perry: %s: WebKitGTK is not available in the conda perry package (WebView is stubbed out)\n", f);'
    echo '    abort();'
    echo '}'
    while read -r s; do echo "void $s(void) { perry_no_webkit(\"$s\"); }"; done < "$work/syms"
} > "$work/webkit_stub.c"
$CC -O1 -fPIC -c "$work/webkit_stub.c" -o "$work/webkit_stub.o"
for lib in webkitgtk-6.0 javascriptcoregtk-6.0; do
    "${AR:-ar}" rcs "$stubs/lib$lib.a" "$work/webkit_stub.o"
done
# So `pkg-config webkitgtk-6.0` succeeds (Perry warns otherwise).
cat > "$stubs/pkgconfig/webkitgtk-6.0.pc" <<PC
Name: webkitgtk-6.0
Description: stub (conda perry package; WebView aborts)
Version: 2.0.0
Libs: -lwebkitgtk-6.0 -ljavascriptcoregtk-6.0
Cflags:
PC
rm -rf "$work"
