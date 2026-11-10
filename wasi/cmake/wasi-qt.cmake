# CMake toolchain for building Qt (and Qt apps/components) for wasm32-wasip1
# with wasi-sdk. Wraps wasi-sdk's own toolchain file and adds:
#   * Platform/WASI.cmake that sets UNIX=1, so Qt takes its unix code paths;
#   * the compat/ shims (force-included header + stub headers) for the POSIX
#     bits wasi-libc lacks; link compat/wasi_compat.c (WASI_QT_COMPAT_C) into
#     the final module;
#   * wasi-libc's emulated signal/mman/process-clock/getpid libraries.
#
# WASI_SDK_PREFIX: -D, environment, ../../wasi-sdk (conda package layout), or
# ../.build/wasi-sdk.

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

if(NOT WASI_SDK_PREFIX)
    if(DEFINED ENV{WASI_SDK_PREFIX})
        set(WASI_SDK_PREFIX "$ENV{WASI_SDK_PREFIX}")
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../wasi-sdk/share/cmake/wasi-sdk-p1.cmake")
        # the qt6-wasi conda package: $PREFIX/opt/{qt6-wasi/cmake,wasi-sdk}
        get_filename_component(WASI_SDK_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../wasi-sdk" ABSOLUTE)
    else()
        get_filename_component(WASI_SDK_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../.build/wasi-sdk" ABSOLUTE)
    endif()
endif()
if(NOT EXISTS "${WASI_SDK_PREFIX}/share/cmake/wasi-sdk-p1.cmake")
    message(FATAL_ERROR "wasi-sdk not found at ${WASI_SDK_PREFIX} (install wasi-sdk or set WASI_SDK_PREFIX)")
endif()
set(ENV{WASI_SDK_PREFIX} "${WASI_SDK_PREFIX}")  # for try_compile re-runs
include("${WASI_SDK_PREFIX}/share/cmake/wasi-sdk-p1.cmake")
# wasi-sdk appended its own Platform/ dir; keep ours (UNIX=1) first.
list(REMOVE_ITEM CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

get_filename_component(_wasi_qt_compat "${CMAKE_CURRENT_LIST_DIR}/../compat" ABSOLUTE)
set(WASI_QT_COMPAT_C "${_wasi_qt_compat}/wasi_compat.c")
set(_wasi_flags "-isystem ${_wasi_qt_compat} -include ${_wasi_qt_compat}/wasi_compat.h -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID -fno-exceptions")
set(CMAKE_C_FLAGS_INIT "${_wasi_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_wasi_flags}")
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/wasi-unix.cmake")  # CMake >= 4
set(CMAKE_EXE_LINKER_FLAGS_INIT "-lwasi-emulated-signal -lwasi-emulated-mman -lwasi-emulated-process-clocks -lwasi-emulated-getpid")
