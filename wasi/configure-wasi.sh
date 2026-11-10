#!/bin/sh
# Cross-configure qtbase 6.8.3 for wasm32-wasip1 with wasi-sdk 27.
#   configure-wasi.sh <qtbase-src> <build-dir> [extra cmake args]
# Env: QT_HOST_PATH (native Qt with moc/rcc), QT_WASI_PREFIX (install prefix),
#      WASI_SDK_PREFIX (read by cmake/wasi-qt.cmake).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$1; bld=$2; shift 2
: "${QT_HOST_PATH:?set QT_HOST_PATH}" "${QT_WASI_PREFIX:?set QT_WASI_PREFIX}"
cmake -G Ninja -S "$src" -B "$bld" --fresh \
 -DCMAKE_TOOLCHAIN_FILE="$here/cmake/wasi-qt.cmake" -DQT_HOST_PATH="$QT_HOST_PATH" \
 -DQT_QMAKE_TARGET_MKSPEC=wasi-clang -DCMAKE_INSTALL_PREFIX="$QT_WASI_PREFIX" \
 -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
 -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DQT_BUILD_TOOLS_WHEN_CROSSCOMPILING=OFF \
 -DFEATURE_thread=OFF -DFEATURE_xcb=OFF -DFEATURE_opengl=OFF -DINPUT_opengl=no -DFEATURE_widgets=OFF \
 -DFEATURE_dbus=OFF -DFEATURE_network=OFF -DFEATURE_sql=OFF -DFEATURE_testlib=OFF -DFEATURE_printsupport=OFF \
 -DFEATURE_concurrent=OFF -DFEATURE_xml=OFF -DFEATURE_vulkan=OFF -DFEATURE_glib=OFF -DFEATURE_icu=OFF \
 -DFEATURE_system_pcre2=OFF -DFEATURE_system_zlib=OFF -DFEATURE_system_freetype=OFF -DFEATURE_system_harfbuzz=OFF \
 -DFEATURE_system_png=OFF -DFEATURE_system_jpeg=OFF -DFEATURE_jpeg=OFF -DFEATURE_gif=OFF -DFEATURE_freetype=ON \
 -DFEATURE_fontconfig=OFF -DFEATURE_process=OFF -DFEATURE_sharedmemory=OFF -DFEATURE_systemsemaphore=OFF \
 -DFEATURE_library=OFF -DFEATURE_filesystemwatcher=OFF -DFEATURE_libudev=OFF -DFEATURE_evdev=OFF \
 -DFEATURE_libinput=OFF -DFEATURE_tslib=OFF -DFEATURE_mtdev=OFF -DFEATURE_linuxfb=OFF -DFEATURE_vnc=OFF \
 -DFEATURE_eglfs=OFF -DFEATURE_xkbcommon=OFF -DFEATURE_glibc=OFF -DFEATURE_backtrace=OFF -DFEATURE_journald=OFF \
 -DFEATURE_syslog=OFF -DFEATURE_slog2=OFF -DFEATURE_inotify=OFF -DFEATURE_eventfd=OFF -DFEATURE_poll_ppoll=OFF \
 -DFEATURE_poll_pollts=OFF -DFEATURE_poll_poll=ON -DFEATURE_futimens=OFF -DFEATURE_getentropy=ON \
 -DFEATURE_cxx11_future=OFF -DFEATURE_std_atomic64=ON -DFEATURE_forkfd_pidfd=OFF -DFEATURE_openssl=OFF \
 -DFEATURE_zstd=OFF -DFEATURE_brotli=OFF -DFEATURE_sessionmanager=OFF -DFEATURE_accessibility=OFF \
 -DFEATURE_draganddrop=OFF -DFEATURE_clipboard=OFF -DFEATURE_desktopservices=OFF -DFEATURE_permissions=OFF \
 -DFEATURE_ssl=OFF -DFEATURE_dladdr=OFF -DFEATURE_dlopen=OFF -DFEATURE_reduce_relocations=OFF \
 -DFEATURE_stack_protector=OFF -DFEATURE_stack_clash_protection=OFF -DFEATURE_intelcet=OFF -DFEATURE_pkg_config=OFF \
 "$@"
