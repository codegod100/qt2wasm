# SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
#
# latha-gui: the Qt window (this) + the wasmtime host (host/, a Rust staticlib
# that build.sh compiles with cargo first and passes as LATHA_HOST_LIB).
cmake_minimum_required(VERSION 3.24)
project(latha-gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus)
if(NOT LATHA_HOST_LIB)
    message(FATAL_ERROR "set LATHA_HOST_LIB to liblatha_host.a (build.sh does)")
endif()

add_executable(latha-gui main.cpp window.cpp window.h accent.cpp accent.h appmeta.cpp appmeta.h icons/icons.qrc latha_pty.cpp latha_pty.h host/latha_host.h)
target_include_directories(latha-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# The host links the system libzstd (precompiled apps, host/src/prebuilt.rs); a static library does not carry that.
find_library(LATHA_ZSTD_LIB zstd REQUIRED)
target_link_libraries(latha-gui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus ${LATHA_HOST_LIB} ${LATHA_ZSTD_LIB} util dl pthread m)
install(TARGETS latha-gui RUNTIME DESTINATION bin)
