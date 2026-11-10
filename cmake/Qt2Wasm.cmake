# Qt2Wasm.cmake: qt2wasm_add_component() for Qt projects built for WASI.
#
# `qt2wasm build` configures the project with the Qt WASI toolchain file
# (wasm/qt/cmake/wasi-qt.cmake) and these cache variables:
#
#   QT2WASM_ROOT       tools/qt2wasm (this file's parent)
#   QT2WASM_QT_PREFIX  the Qt-for-WASI install prefix (static qtbase)
#   QT2WASM_GEN_DIR    generated glue, moc output and wit-bindgen bindings
#   QT2WASM_COMPAT_DIR compat/ from the Qt WASI toolchain (wasi_compat.c)
#
# It does not use find_package(Qt6): a cross find_package needs a host Qt of
# the same version (QT_HOST_PATH), and qt2wasm runs moc itself. Instead it
# links the static archives directly, in the order Qt's .prl files give.

cmake_minimum_required(VERSION 3.21)

get_filename_component(_qt2wasm_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(QT2WASM_ROOT "${_qt2wasm_root}" CACHE PATH "qt2wasm source dir")
set(QT2WASM_QT_PREFIX "" CACHE PATH "Qt for WASI install prefix")
set(QT2WASM_GEN_DIR "" CACHE PATH "qt2wasm generated sources")
set(QT2WASM_COMPAT_DIR "" CACHE PATH "Qt WASI compat dir (wasi_compat.c)")
set(QT2WASM_QPA_PLATFORM "offscreen" CACHE STRING "static QPA plugin for GUI components")

function(qt2wasm_add_component target)
    cmake_parse_arguments(A "GUI" "" "SOURCES;INCLUDE_DIRS" ${ARGN})
    if(NOT QT2WASM_QT_PREFIX OR NOT EXISTS "${QT2WASM_QT_PREFIX}/lib/libQt6Core.a")
        message(FATAL_ERROR "QT2WASM_QT_PREFIX='${QT2WASM_QT_PREFIX}' has no lib/libQt6Core.a")
    endif()
    if(NOT QT2WASM_GEN_DIR)
        message(FATAL_ERROR "QT2WASM_GEN_DIR is not set; run through `qt2wasm build`")
    endif()

    set(P "${QT2WASM_QT_PREFIX}")
    file(GLOB _gen_cpp "${QT2WASM_GEN_DIR}/*.cpp")
    file(GLOB _gen_c "${QT2WASM_GEN_DIR}/bindings/*.c")
    file(GLOB _gen_obj "${QT2WASM_GEN_DIR}/bindings/*_component_type.o")

    add_executable(${target}
        ${A_SOURCES} ${_gen_cpp} ${_gen_c}
        "${QT2WASM_ROOT}/runtime/runtime.cpp"
        "${QT2WASM_ROOT}/runtime/wasieventdispatcher.cpp"
        "${QT2WASM_GEN_DIR}/runtime/moc_wasieventdispatcher.cpp")
    if(QT2WASM_COMPAT_DIR AND EXISTS "${QT2WASM_COMPAT_DIR}/wasi_compat.c")
        target_sources(${target} PRIVATE "${QT2WASM_COMPAT_DIR}/wasi_compat.c")
    endif()
    set_target_properties(${target} PROPERTIES SUFFIX ".core.wasm" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

    target_include_directories(${target} PRIVATE
        ${A_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}"
        "${QT2WASM_GEN_DIR}" "${QT2WASM_GEN_DIR}/bindings"
        "${QT2WASM_ROOT}/include" "${QT2WASM_ROOT}/runtime"
        "${P}/include" "${P}/include/QtCore" "${P}/mkspecs/wasi-clang")
    target_compile_definitions(${target} PRIVATE QT_CORE_LIB QT_NO_DEBUG QT_STATIC)

    set(_libs "${P}/lib/libQt6Core.a" "${P}/lib/libQt6BundledZLIB.a" "${P}/lib/libQt6BundledPcre2.a")
    if(A_GUI)
        target_include_directories(${target} PRIVATE "${P}/include/QtGui")
        target_compile_definitions(${target} PRIVATE QT_GUI_LIB QT2WASM_GUI
            QT2WASM_QPA_PLATFORM="${QT2WASM_QPA_PLATFORM}")
        string(TOLOWER "${QT2WASM_QPA_PLATFORM}" _qpa)
        string(SUBSTRING "${_qpa}" 0 1 _first)
        string(TOUPPER "${_first}" _first)
        string(SUBSTRING "${_qpa}" 1 -1 _rest)
        set(_plugin "Q${_first}${_rest}IntegrationPlugin")
        set(_libs
            "${P}/plugins/platforms/objects-Release/${_plugin}_init/${_plugin}_init.cpp.o"
            "${P}/lib/objects-Release/Gui_resources_1/.qt/rcc/qrc_qpdf_init.cpp.o"
            "${P}/lib/objects-Release/Gui_resources_2/.qt/rcc/qrc_gui_shaders_init.cpp.o"
            "${P}/plugins/platforms/libq${_qpa}.a"
            "${P}/lib/libQt6Gui.a" "${P}/lib/libQt6BundledHarfbuzz.a"
            "${P}/lib/libQt6BundledFreetype.a" "${P}/lib/libQt6BundledLibpng.a"
            ${_libs})
    endif()

    target_link_libraries(${target} PRIVATE ${_gen_obj} ${_libs} m)
    # A reactor: no main(), the host calls exports. Keep only what exports reach.
    target_link_options(${target} PRIVATE -mexec-model=reactor -Wl,--gc-sections)
endfunction()
