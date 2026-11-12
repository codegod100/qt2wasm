// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// C API of latha-host (src/lib.rs): one latha:gui component on wasmtime.
// All calls on one thread; callbacks run inside the call that caused them,
// except `wake`. The engine has wasm exceptions, tail calls and SIMD on;
// programs get wasi:sockets and latha:proc (../../qt2wasm/proc/wit/proc.wit).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LathaApp LathaApp;
typedef struct LathaOut LathaOut;

typedef struct { const char *ptr; size_t len; } LathaStr;

typedef struct {
    void *user;
    void (*request_frame)(void *user);
    void (*request_tick)(void *user, uint32_t delay_ms);
    void (*set_title)(void *user, LathaStr title);
    void (*set_cursor)(void *user, uint32_t shape);          // types.cursor, in WIT order
    void (*set_min_size)(void *user, uint32_t w, uint32_t h);
    void (*clipboard_text)(void *user, int selection, LathaOut *out); // 0 clipboard, 1 primary
    void (*set_clipboard_text)(void *user, int selection, LathaStr text);
    int (*prefers_dark)(void *user);
    int (*accent_color)(void *user, uint32_t *rgb);  // 1 and *rgb = 0xRRGGBB, or 0: none
    void (*bell)(void *user);
    int (*open_url)(void *user, LathaStr url);  // 1: opened (handed to the desktop), 0: not
    void (*quit)(void *user, int32_t code);
    // env: envc (name, value) pairs; cwd.ptr NULL: none. >= 0 id, -1 error in err.
    int64_t (*pty_spawn)(void *user, const LathaStr *argv, size_t argc, const LathaStr *env, size_t envc,
                         LathaStr cwd, uint16_t cols, uint16_t rows, LathaOut *err);
    void (*pty_write)(void *user, uint32_t id, const uint8_t *data, size_t len);
    void (*pty_read)(void *user, uint32_t id, LathaOut *out);   // raw bytes
    void (*pty_resize)(void *user, uint32_t id, uint16_t cols, uint16_t rows);
    int (*pty_exit_code)(void *user, uint32_t id, int32_t *code);
    void (*pty_close)(void *user, uint32_t id);
    // Called from ANY thread when a latha:proc message (or a close) arrived for
    // the app; call latha_tick soon on the GUI thread. NULL: not called.
    void (*wake)(void *user);
} LathaCallbacks;

typedef struct { int32_t x, y; uint32_t width, height; } LathaRect;
typedef struct { LathaRect area; int32_t dx, dy; } LathaMove;

enum { LATHA_SHIFT = 1, LATHA_CTRL = 2, LATHA_ALT = 4, LATHA_META = 8, LATHA_KEYPAD = 16 };
enum { LATHA_BTN_LEFT = 1, LATHA_BTN_RIGHT = 2, LATHA_BTN_MIDDLE = 4, LATHA_BTN_BACK = 8, LATHA_BTN_FORWARD = 16 };
enum { LATHA_BUTTON_NONE, LATHA_BUTTON_LEFT, LATHA_BUTTON_RIGHT, LATHA_BUTTON_MIDDLE, LATHA_BUTTON_BACK, LATHA_BUTTON_FORWARD };
enum { LATHA_PRESS, LATHA_RELEASE, LATHA_MOVE, LATHA_ENTER, LATHA_LEAVE };

typedef struct {
    uint32_t kind, button, buttons, modifiers;
    double x, y;
    uint32_t clicks;
} LathaPointer;

typedef struct {
    double x, y, steps_x, steps_y, pixels_x, pixels_y;
    uint32_t modifiers;
} LathaWheel;

typedef struct {
    const LathaStr *dirs;       // host directories ...
    const LathaStr *guest_dirs; // ... and where the app sees them
    size_t ndirs;
    int no_cache;
} LathaOptions;

// What PATH is: 0 not wasm, 1 a core module, 2 a WASI command component
// (exports wasi:cli/run, not latha:gui/app), 3 another component (a latha:gui app).
int latha_wasm_kind(const char *path);
// Run a WASI command (kind 1 or 2) to its end without a window, on its own
// big-stack thread, with latha:proc; returns its exit code, or -1 (and *err)
// if it could not start.
int latha_run(const char *path, const LathaOptions *opts, const LathaStr *args, size_t nargs, char **err);

// 1 when loading PATH would compile it first (seconds; no compiled code cached yet), else 0.
int latha_needs_prepare(const char *path, int no_cache);
// Compile PATH into the cache, or fetch it precompiled (LATHA_GUI_PREBUILT_URL), so latha_load is quick; any
// thread. PROGRESS (may be NULL) runs on other threads: stage 1 downloading (bytes), 2 compiling (functions).
typedef void (*LathaProgress)(void *user, int stage, uint64_t done, uint64_t total);
int latha_prepare(const char *path, int no_cache, LathaProgress progress, void *user, char **err);
// Compile PATH for a portable CPU baseline into DIR/<key>.cwasm.zst (a prebuilt store's file); returns that
// path (latha_string_free), or NULL and *err.
char *latha_precompile(const char *path, const char *dir, char **err);

LathaApp *latha_load(const char *path, const LathaOptions *opts, const LathaCallbacks *cb,
                     const LathaStr *args, size_t nargs, char **err);
void latha_free(LathaApp *app);
void latha_string_free(char *s);
void latha_out_set(LathaOut *out, const void *data, size_t len);
int latha_has_pty(const LathaApp *app);
const char *latha_error(const LathaApp *app);

// Every call below returns < 0 once the app trapped (see latha_error).
int latha_start(LathaApp *app, const LathaStr *args, size_t nargs, uint32_t *w, uint32_t *h, char **err);
int latha_resize(LathaApp *app, uint32_t w, uint32_t h, double scale);
int latha_key(LathaApp *app, int pressed, int repeat, uint32_t keysym, uint32_t scancode, uint32_t modifiers,
              LathaStr text);
int latha_text(LathaApp *app, LathaStr text);
int latha_pointer(LathaApp *app, const LathaPointer *ev);
int latha_wheel(LathaApp *app, const LathaWheel *ev);
int latha_focus(LathaApp *app, int focused);
int latha_color_scheme(LathaApp *app, int dark);
int latha_close(LathaApp *app);   // 1 may close, 0 not now
int64_t latha_tick(LathaApp *app); // ms, -1 none, -2 trap
int latha_paint(LathaApp *app);
void latha_damage(const LathaApp *app, const LathaMove **moves, size_t *nmoves, const LathaRect **rects,
                  size_t *nrects);
int latha_frame_size(LathaApp *app, uint32_t *w, uint32_t *h);
int latha_frame_pixels(LathaApp *app, LathaRect r, uint8_t *dst, size_t stride);

#ifdef __cplusplus
}
#endif
