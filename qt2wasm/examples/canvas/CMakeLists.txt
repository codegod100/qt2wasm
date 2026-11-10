cmake_minimum_required(VERSION 3.21)
project(canvas C CXX)
include(${QT2WASM_ROOT}/cmake/Qt2Wasm.cmake)
qt2wasm_add_component(canvas GUI SOURCES canvas.cpp)
