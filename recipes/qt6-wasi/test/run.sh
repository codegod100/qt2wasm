# Configure + build test/ with the packaged qt-cmake; checks the relocated
# toolchain, QT_HOST_PATH and moc. (No wasm runtime here, so it isn't run.)
set -euxo pipefail
test -x "$QT2WASM_MOC"
"$QT2WASM_MOC" --version
test -f "$QT_WASI_PREFIX/lib/libQt6Gui.a"
"$QT_WASI_PREFIX/bin/qt-cmake" -S test -B build-test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-test
test -s build-test/qtwasitest.wasm
