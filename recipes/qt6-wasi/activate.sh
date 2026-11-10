# qt6-wasi: where the WASI Qt, its host tools and toolchain file live.
export QT_WASI_PREFIX="$CONDA_PREFIX/opt/qt6-wasi/wasi"
export QT_WASI_HOST_PATH="$CONDA_PREFIX/opt/qt6-wasi/host"
export QT_WASI_TOOLCHAIN="$CONDA_PREFIX/opt/qt6-wasi/cmake/wasi-qt.cmake"
export QT2WASM_QT_PREFIX="$QT_WASI_PREFIX"
export QT2WASM_QT_TOOLCHAIN="$QT_WASI_TOOLCHAIN"
export QT2WASM_MOC="$QT_WASI_HOST_PATH/libexec/moc"
