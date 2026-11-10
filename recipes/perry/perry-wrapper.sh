#!/usr/bin/env bash
# perry from the conda package (recipes/perry): runs the prebuilt native
# compiler with the conda C toolchain and this env's libraries.
#  - Perry links with `cc` from PATH, so a private bin dir maps cc to $CC (set
#    by conda compiler activation) or x86_64-conda-linux-gnu-cc.
#  - pkg-config finds GTK4/GStreamer/libshumate/libsoup in this env, plus the
#    WebKitGTK stub .pc.
#  - PERRY_EXTRA_LINK_ARGS adds this env's lib dir and the WebKitGTK stubs to
#    the link and an rpath to this env, so the app runs without activation.
set -euo pipefail
prefix=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)
native=$prefix/@NATIVE@/bin/perry
stubs=$prefix/lib/perry/stubs

cc=${CC:-}
if [ -z "$cc" ] || ! command -v "${cc%% *}" >/dev/null 2>&1; then
    if [ -x "$prefix/bin/x86_64-conda-linux-gnu-cc" ]; then
        cc=$prefix/bin/x86_64-conda-linux-gnu-cc
    else
        cc=cc
    fi
fi
if [ "$cc" != cc ]; then
    ccdir=${TMPDIR:-/tmp}/perry-conda-cc-$(id -u)
    mkdir -p "$ccdir"
    printf '#!/bin/sh\nexec %s "$@"\n' "$cc" > "$ccdir/cc.$$"
    chmod 0755 "$ccdir/cc.$$"
    mv -f "$ccdir/cc.$$" "$ccdir/cc"
    export PATH="$ccdir:$PATH"
fi

export PKG_CONFIG_PATH="$stubs/pkgconfig:$prefix/lib/pkgconfig:$prefix/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PERRY_EXTRA_LINK_ARGS="-L$stubs -L$prefix/lib -Wl,-rpath,$prefix/lib -Wl,-rpath-link,$prefix/lib${PERRY_EXTRA_LINK_ARGS:+ $PERRY_EXTRA_LINK_ARGS}"
exec "$native" "$@"
