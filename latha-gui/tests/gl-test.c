// A latha:gui app that draws with the host's GL (gl.wit, through latha-gui/gl's GLES and EGL): a red triangle on
// blue, drawn into a framebuffer, read back and shown as the window. It checks a few pixels and prints
// "gl-test: ok" (or what was wrong) to stderr. Built and run by gl-test.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"
#include "latha_gles.h"

enum { W = 96, H = 64 };
static uint8_t s_frame[W * H * 4];
static int s_drawn;

static GLuint compile(GLenum type, char const* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = { 0 };
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        fprintf(stderr, "gl-test: shader: %s\n", log);
    }
    return shader;
}

static int draw(void)
{
    if (!latha_gles_ready()) {
        fprintf(stderr, "gl-test: no LATHA_GUI_GL: this host has no gl\n");
        return 0;
    }
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, (void*)EGL_DEFAULT_DISPLAY, NULL);
    EGLint major, minor, n;
    EGLConfig config;
    eglInitialize(display, &major, &minor);
    eglChooseConfig(display, (EGLint[]) { EGL_NONE }, &config, 1, &n);
    EGLint attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, attribs);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        fprintf(stderr, "gl-test: no context (EGL error %#x)\n", eglGetError());
        return 0;
    }
    fprintf(stderr, "gl-test: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    GLuint rb, fb;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl-test: framebuffer incomplete\n");
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER,
        "#version 300 es\nin vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n"));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER,
        "#version 300 es\nprecision mediump float;\nuniform vec4 color;\nout vec4 o;\nvoid main() { o = color; }\n"));
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    glUseProgram(program);
    glUniform4f(glGetUniformLocation(program, "color"), 1, 0, 0, 1);

    float const triangle[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
    GLuint buffer, vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof triangle, triangle, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    glViewport(0, 0, W, H);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    static uint8_t rgba[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        fprintf(stderr, "gl-test: GL error %#x\n", error);

    // GL's rows go bottom up; the frame is top down, B G R A.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t const* s = &rgba[((H - 1 - y) * W + x) * 4];
            uint8_t* d = &s_frame[(y * W + x) * 4];
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
        }
    }
    uint8_t const* corner = &rgba[0];
    uint8_t const* middle = &rgba[((H / 2) * W + W / 2) * 4];
    int ok = corner[2] == 255 && corner[0] == 0 && middle[0] == 255 && middle[2] == 0 && error == GL_NO_ERROR;
    fprintf(stderr, "gl-test: corner %d,%d,%d middle %d,%d,%d: %s\n", corner[0], corner[1], corner[2], middle[0],
        middle[1], middle[2], ok ? "ok" : "WRONG");
    return ok;
}

bool exports_latha_gui_app_start(gui_list_string_t* args, exports_latha_gui_app_size_t* ret, gui_string_t* err)
{
    (void)args;
    (void)err;
    ret->width = W;
    ret->height = H;
    return true;
}

void exports_latha_gui_app_resize(uint32_t width, uint32_t height, double scale) { (void)width, (void)height, (void)scale; }
void exports_latha_gui_app_key(exports_latha_gui_app_key_event_t* event) { (void)event; }
void exports_latha_gui_app_text(gui_string_t* text) { (void)text; }
void exports_latha_gui_app_pointer(exports_latha_gui_app_pointer_event_t* event) { (void)event; }
void exports_latha_gui_app_wheel(exports_latha_gui_app_wheel_event_t* event) { (void)event; }
void exports_latha_gui_app_focus(bool focused) { (void)focused; }
void exports_latha_gui_app_color_scheme(bool dark) { (void)dark; }
bool exports_latha_gui_app_close(void) { return true; }
bool exports_latha_gui_app_tick(uint32_t* ret) { (void)ret; return false; }

void exports_latha_gui_app_paint(exports_latha_gui_app_damage_t* ret)
{
    memset(ret, 0, sizeof *ret);
    if (s_drawn)
        return;
    s_drawn = 1;
    draw();
    latha_gui_types_rect_t* r = malloc(sizeof *r);
    *r = (latha_gui_types_rect_t) { 0, 0, W, H };
    ret->rects.ptr = r;
    ret->rects.len = 1;
}

void exports_latha_gui_app_frame_size(exports_latha_gui_app_size_t* ret)
{
    ret->width = W;
    ret->height = H;
}

static char const B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void exports_latha_gui_app_frame_pixels(exports_latha_gui_app_rect_t* r, gui_string_t* ret)
{
    size_t n = 0;
    size_t bytes = (size_t)r->width * r->height * 4;
    uint8_t* row = malloc(bytes);
    for (uint32_t y = 0; y < r->height; ++y)
        memcpy(row + (size_t)y * r->width * 4, &s_frame[((r->y + y) * W + r->x) * 4], (size_t)r->width * 4);
    char* out = malloc((bytes + 2) / 3 * 4);
    for (size_t i = 0; i < bytes; i += 3) {
        uint32_t v = row[i] << 16 | (i + 1 < bytes ? row[i + 1] << 8 : 0) | (i + 2 < bytes ? row[i + 2] : 0);
        out[n++] = B64[v >> 18 & 63];
        out[n++] = B64[v >> 12 & 63];
        out[n++] = i + 1 < bytes ? B64[v >> 6 & 63] : '=';
        out[n++] = i + 2 < bytes ? B64[v & 63] : '=';
    }
    free(row);
    ret->ptr = (uint8_t*)out;
    ret->len = n;
}
