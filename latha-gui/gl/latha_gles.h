/*
 * GLES 3 and EGL for a wasm32-wasi app on latha:gui: the calls run on the host's GPU through its `gl` interface
 * (qt2wasm/gui/wit/gl.wit). latha_gles.c is generated (gen.py); the calls that take or return what its JSON cannot
 * describe are in latha_gles_custom.c, and a small EGL (contexts with no window) in latha_egl.c. Compile them with
 * wit-bindgen's C bindings for the gl-guest world (gl_guest.h) and link its gl_guest_component_type.o.
 */

#pragma once

/* No native window system types on WASI: EGL's are plain pointers. */
#ifndef EGL_NO_PLATFORM_SPECIFIC_TYPES
#    define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#endif
#define GL_GLEXT_PROTOTYPES 1
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <GLES2/gl2ext_angle.h>

/* Nonzero when the host has latha:gui's gl (it sets LATHA_GUI_GL in the app's environment). A host without it
   traps on any gl call, so every entry point checks this first. */
int latha_gles_ready(void);

#ifdef __cplusplus
}
#endif
