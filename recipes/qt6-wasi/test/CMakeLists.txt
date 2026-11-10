# Smoke test: a QtGui program (QImage + QPainter, offscreen-free) for wasm32-wasip1.
cmake_minimum_required(VERSION 3.21)
project(qtwasitest LANGUAGES C CXX)
find_package(Qt6 REQUIRED COMPONENTS Core Gui)
qt_add_executable(qtwasitest main.cpp "${WASI_QT_COMPAT_C}")
target_link_libraries(qtwasitest PRIVATE Qt6::Gui)
