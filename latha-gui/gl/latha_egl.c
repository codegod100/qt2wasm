/*
 * The EGL an app needs for offscreen GLES (what Ladybird's OpenGLContext calls), over latha:gui's gl: one display,
 * one config, contexts with no window (the host's, by number), and no surfaces (an app draws into framebuffers of
 * its own and reads them back). What the host cannot do here (images, client buffers, pbuffers) fails as EGL says it may.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gl_guest.h"
#include "latha_gles.h"

#define LATHA_DISPLAY ((EGLDisplay)(uintptr_t)1)
#define LATHA_CONFIG ((EGLConfig)(uintptr_t)1)

static EGLint s_error = EGL_SUCCESS;
static uint32_t s_current;

static EGLBoolean fail(EGLint error)
{
    s_error = error;
    return EGL_FALSE;
}

static EGLBoolean ok(void)
{
    s_error = EGL_SUCCESS;
    return EGL_TRUE;
}

EGLAPI EGLint EGLAPIENTRY eglGetError(void)
{
    EGLint e = s_error;
    s_error = EGL_SUCCESS;
    return e;
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetPlatformDisplay(EGLenum platform, void* native_display, EGLAttrib const* attrib_list)
{
    (void)platform;
    (void)native_display;
    (void)attrib_list;
    if (!latha_gles_ready()) {
        s_error = EGL_NOT_INITIALIZED;
        return EGL_NO_DISPLAY;
    }
    return LATHA_DISPLAY;
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType display_id)
{
    return eglGetPlatformDisplay(0, (void*)(uintptr_t)display_id, NULL);
}

EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor)
{
    if (dpy != LATHA_DISPLAY)
        return fail(EGL_BAD_DISPLAY);
    if (major)
        *major = 1;
    if (minor)
        *minor = 5;
    return ok();
}

EGLAPI EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay dpy)
{
    return dpy == LATHA_DISPLAY ? ok() : fail(EGL_BAD_DISPLAY);
}

EGLAPI EGLBoolean EGLAPIENTRY eglReleaseThread(void)
{
    return ok();
}

EGLAPI char const* EGLAPIENTRY eglQueryString(EGLDisplay dpy, EGLint name)
{
    (void)dpy;
    switch (name) {
    case EGL_VENDOR:
        return "latha:gui";
    case EGL_VERSION:
        return "1.5 latha:gui";
    case EGL_CLIENT_APIS:
        return "OpenGL_ES";
    default:
        return "";
    }
}

EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay dpy, EGLint const* attrib_list, EGLConfig* configs, EGLint config_size, EGLint* num_config)
{
    (void)attrib_list;
    if (dpy != LATHA_DISPLAY)
        return fail(EGL_BAD_DISPLAY);
    if (!num_config)
        return fail(EGL_BAD_PARAMETER);
    *num_config = 1;
    if (configs) {
        if (config_size < 1) {
            *num_config = 0;
            return ok();
        }
        configs[0] = LATHA_CONFIG;
    }
    return ok();
}

EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value)
{
    if (dpy != LATHA_DISPLAY)
        return fail(EGL_BAD_DISPLAY);
    if (config != LATHA_CONFIG)
        return fail(EGL_BAD_CONFIG);
    switch (attribute) {
    case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: case EGL_STENCIL_SIZE:
        *value = 8;
        break;
    case EGL_DEPTH_SIZE:
        *value = 24;
        break;
    case EGL_BIND_TO_TEXTURE_TARGET_ANGLE:
        *value = EGL_TEXTURE_2D;
        break;
    default:
        *value = 0;
        break;
    }
    return ok();
}

EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, EGLint const* attrib_list)
{
    if (dpy != LATHA_DISPLAY) {
        s_error = EGL_BAD_DISPLAY;
        return EGL_NO_CONTEXT;
    }
    if (share_context != EGL_NO_CONTEXT) {
        s_error = EGL_BAD_MATCH;
        return EGL_NO_CONTEXT;
    }
    (void)config;
    uint32_t client_version = 1;
    size_t count = 0;
    if (attrib_list) {
        while (attrib_list[count] != EGL_NONE)
            count += 2;
        for (size_t i = 0; i < count; i += 2) {
            if (attrib_list[i] == EGL_CONTEXT_CLIENT_VERSION)
                client_version = (uint32_t)attrib_list[i + 1];
        }
    }
    gl_guest_list_s32_t attribs = { (int32_t*)attrib_list, count };
    uint32_t context = latha_gui_gl_create_context(client_version, &attribs);
    if (!context) {
        s_error = EGL_BAD_ALLOC;
        return EGL_NO_CONTEXT;
    }
    s_error = EGL_SUCCESS;
    return (EGLContext)(uintptr_t)context;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    if (dpy != LATHA_DISPLAY)
        return fail(EGL_BAD_DISPLAY);
    uint32_t context = (uint32_t)(uintptr_t)ctx;
    if (!context)
        return fail(EGL_BAD_CONTEXT);
    latha_gui_gl_destroy_context(context);
    if (s_current == context)
        s_current = 0;
    return ok();
}

EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    (void)draw;
    (void)read;
    if (dpy != LATHA_DISPLAY)
        return fail(EGL_BAD_DISPLAY);
    uint32_t context = (uint32_t)(uintptr_t)ctx;
    // The engine makes its context current before nearly every call: only a change goes to the host.
    if (context == s_current)
        return ok();
    if (!latha_gui_gl_make_current(context))
        return fail(EGL_BAD_CONTEXT);
    s_current = context;
    static int logged;
    if (context && !logged) {
        logged = 1;
        gl_guest_string_t renderer;
        latha_gui_gl_renderer(&renderer);
        fprintf(stderr, "latha-gles: GL runs on the host's %.*s\n", (int)renderer.len, (char const*)renderer.ptr);
        gl_guest_string_free(&renderer);
    }
    return ok();
}

EGLAPI EGLContext EGLAPIENTRY eglGetCurrentContext(void)
{
    return (EGLContext)(uintptr_t)s_current;
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetCurrentDisplay(void)
{
    return s_current ? LATHA_DISPLAY : EGL_NO_DISPLAY;
}

EGLAPI EGLSurface EGLAPIENTRY eglGetCurrentSurface(EGLint readdraw)
{
    (void)readdraw;
    return EGL_NO_SURFACE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglBindAPI(EGLenum api)
{
    return api == EGL_OPENGL_ES_API ? ok() : fail(EGL_BAD_PARAMETER);
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, EGLint const* attrib_list)
{
    (void)dpy;
    (void)config;
    (void)attrib_list;
    s_error = EGL_BAD_MATCH;
    return EGL_NO_SURFACE;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer, EGLConfig config, EGLint const* attrib_list)
{
    (void)dpy;
    (void)buftype;
    (void)buffer;
    (void)config;
    (void)attrib_list;
    s_error = EGL_BAD_PARAMETER;
    return EGL_NO_SURFACE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    (void)dpy;
    return surface == EGL_NO_SURFACE ? fail(EGL_BAD_SURFACE) : ok();
}

EGLAPI EGLBoolean EGLAPIENTRY eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
    (void)dpy;
    (void)surface;
    (void)buffer;
    return fail(EGL_BAD_SURFACE);
}

EGLAPI EGLBoolean EGLAPIENTRY eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
    (void)dpy;
    (void)surface;
    (void)buffer;
    return fail(EGL_BAD_SURFACE);
}

EGLAPI EGLImage EGLAPIENTRY eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, EGLAttrib const* attrib_list)
{
    (void)dpy;
    (void)ctx;
    (void)target;
    (void)buffer;
    (void)attrib_list;
    s_error = EGL_BAD_PARAMETER;
    return EGL_NO_IMAGE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroyImage(EGLDisplay dpy, EGLImage image)
{
    (void)dpy;
    (void)image;
    return fail(EGL_BAD_PARAMETER);
}

EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(char const* procname)
{
    (void)procname;
    return NULL;
}

EGLAPI EGLBoolean EGLAPIENTRY eglQueryDisplayAttribEXT(EGLDisplay dpy, EGLint attribute, EGLAttrib* value)
{
    (void)dpy;
    (void)attribute;
    (void)value;
    return fail(EGL_BAD_ATTRIBUTE);
}

EGLAPI EGLBoolean EGLAPIENTRY eglQueryDeviceAttribEXT(EGLDeviceEXT device, EGLint attribute, EGLAttrib* value)
{
    (void)device;
    (void)attribute;
    (void)value;
    return fail(EGL_BAD_ATTRIBUTE);
}

EGLAPI void EGLAPIENTRY eglWaitUntilWorkScheduledANGLE(EGLDisplay dpy)
{
    (void)dpy;
}
