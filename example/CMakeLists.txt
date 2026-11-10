# Configure with the WASI Qt's qt-cmake: `pixi run build` (see pixi.toml).
cmake_minimum_required(VERSION 3.21)
project(qt6wasiexample LANGUAGES C CXX)
find_package(Qt6 REQUIRED COMPONENTS Core Gui)
# WASI_QT_COMPAT_C comes from the wasi-qt.cmake toolchain: stubs for what wasi-libc lacks.
qt_add_executable(qt6-wasi-example main.cpp "${WASI_QT_COMPAT_C}")
target_link_libraries(qt6-wasi-example PRIVATE Qt6::Gui)
