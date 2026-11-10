#!/usr/bin/env bash
# rattler-build script for qt6-wasi: native host qtbase, then qtbase for
# wasm32-wasip1 with wasi/configure-wasi.sh, both into $PREFIX/opt/qt6-wasi.
set -euxo pipefail

Q=$PREFIX/opt/qt6-wasi
jobs=${CPU_COUNT:-$(nproc)}
mkdir -p "$Q"
cp -r wasiqt/cmake wasiqt/compat "$Q/"
cp -r wasiqt/mkspecs/wasi-clang qtbase/mkspecs/

# 1. Native host Qt (moc, rcc, cmake tooling for QT_HOST_PATH), minimal features.
cmake -S qtbase -B build-host -G Ninja ${CMAKE_ARGS:-} \
    -DCMAKE_INSTALL_PREFIX="$Q/host" -DCMAKE_BUILD_TYPE=Release \
    -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF \
    -DFEATURE_xcb=OFF -DFEATURE_opengl=OFF -DINPUT_opengl=no -DFEATURE_widgets=OFF -DFEATURE_dbus=OFF \
    -DFEATURE_network=OFF -DFEATURE_sql=OFF -DFEATURE_testlib=OFF -DFEATURE_printsupport=OFF \
    -DFEATURE_concurrent=OFF -DFEATURE_xml=OFF -DFEATURE_vulkan=OFF -DFEATURE_glib=OFF -DFEATURE_icu=OFF \
    -DFEATURE_system_pcre2=OFF -DFEATURE_system_zlib=OFF -DFEATURE_system_freetype=OFF \
    -DFEATURE_system_harfbuzz=OFF -DFEATURE_system_png=OFF -DFEATURE_fontconfig=OFF \
    -DFEATURE_xkbcommon=OFF -DFEATURE_libudev=OFF -DFEATURE_evdev=OFF -DFEATURE_libinput=OFF \
    -DFEATURE_wayland=OFF -DFEATURE_eglfs=OFF -DFEATURE_linuxfb=OFF -DFEATURE_vnc=OFF \
    -DFEATURE_system_doubleconversion=OFF -DFEATURE_system_libb2=OFF -DFEATURE_zstd=OFF
cmake --build build-host --parallel "$jobs"
cmake --install build-host

# 2. qtbase for wasm32-wasip1. Drop conda's native compiler/flag variables so
#    they don't leak into the wasm cross build; record the installed toolchain
#    file (not the one in the work dir) in qt-cmake. A static Qt bakes its
#    install prefix into a 256-byte buffer (qconfig.cpp), which conda's long
#    placeholder $PREFIX overflows, so install to a short staging prefix and
#    move it into place, rewriting the staging path in text files.
stage=/tmp/qt6-wasi-stage-$$
rm -rf "$stage"
env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CC -u CXX -u CPP -u AR -u RANLIB -u NM -u LD \
    -u STRIP -u OBJCOPY -u OBJDUMP -u CMAKE_ARGS -u CMAKE_PREFIX_PATH -u PKG_CONFIG_PATH \
    WASI_SDK_PREFIX="$PREFIX/opt/wasi-sdk" QT_HOST_PATH="$Q/host" QT_WASI_PREFIX="$stage" \
    bash wasiqt/configure-wasi.sh "$PWD/qtbase" "$PWD/build-wasi" \
        -DCMAKE_TOOLCHAIN_FILE="$Q/cmake/wasi-qt.cmake" -DWASI_SDK_PREFIX="$PREFIX/opt/wasi-sdk"
env -u CFLAGS -u CXXFLAGS -u LDFLAGS cmake --build build-wasi --parallel "$jobs"
cmake --install build-wasi
mv "$stage" "$Q/wasi"
grep -rlI --null -F "$stage" "$Q/wasi" | xargs -0 -r sed -i "s|$stage|$Q/wasi|g"

# Docs and the host Qt's static helper sources aren't needed.
rm -rf "$Q/host/doc" "$Q/wasi/doc"

for s in activate deactivate; do
    mkdir -p "$PREFIX/etc/conda/$s.d"
    cp "$RECIPE_DIR/$s.sh" "$PREFIX/etc/conda/$s.d/qt6-wasi.sh"
done
