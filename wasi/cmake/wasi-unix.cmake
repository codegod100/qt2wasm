# CMake >= 4 ships its own Platform/WASI.cmake, which takes precedence over
# ours (CMP0017), so UNIX stays unset and Qt's unix features fail to configure.
# wasi-qt.cmake points CMAKE_USER_MAKE_RULES_OVERRIDE here; it runs after the
# platform file.
set(UNIX 1)
