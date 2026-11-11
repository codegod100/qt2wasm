// latha:proc for core wasm modules (wasm32-wasip1, no component model):
// processes and message channels, imported from the module "latha:proc".
// Same meaning as wit/proc.wit (read that for the details); here results come
// back through out-pointers and every call returns 0 or a negative LATHA_E_*.
//
// latha-gui (0.3.0 and later) provides these to every program it runs.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LATHA_PROC_IMPORT(name) __attribute__((import_module("latha:proc"), import_name(name)))

enum {
    LATHA_OK = 0,
    LATHA_E_CLOSED = -1,     // the peer is gone and nothing is queued
    LATHA_E_TIMEOUT = -2,    // nothing arrived in time
    LATHA_E_BAD_HANDLE = -3, // not one of our handles
    LATHA_E_TOO_BIG = -4,    // message over the limit, or (recv) bigger than the buffers: *len / *nhandles say how big
    LATHA_E_SPAWN = -5,      // spawn failed (message on stderr)
    LATHA_E_FAULT = -6,      // a pointer outside linear memory
};

// Two connected endpoints into out[0], out[1].
LATHA_PROC_IMPORT("pair") int32_t latha_pair(uint32_t out[2]);

// Queue data (len bytes) and nhandles endpoints for h's peer; never blocks.
LATHA_PROC_IMPORT("send")
int32_t latha_send(uint32_t h, const void *data, size_t len, const uint32_t *handles, size_t nhandles);

// The next message on h into buf (cap bytes) and hbuf (hcap handles); *len and
// *nhandles get its size. LATHA_E_TOO_BIG: it did not fit and is still queued.
// timeout_ms 0: do not wait, < 0: for ever.
LATHA_PROC_IMPORT("recv")
int32_t latha_recv(uint32_t h, void *buf, size_t cap, uint32_t *hbuf, size_t hcap, uint32_t *len, uint32_t *nhandles,
                   int32_t timeout_ms);

// Wait until one of hs[0..n) has a message or its peer closed; ready[i] = 1 for
// those. Returns how many are ready (0 after the timeout).
LATHA_PROC_IMPORT("wait") int32_t latha_wait(const uint32_t *hs, size_t n, uint8_t *ready, int32_t timeout_ms);

LATHA_PROC_IMPORT("close") int32_t latha_close(uint32_t h);

// The channel this process was started with into *h, once; LATHA_E_BAD_HANDLE if none.
LATHA_PROC_IMPORT("parent") int32_t latha_parent(uint32_t *h);

// Start a wasm program (see proc.wit's spawn). args: argc NUL-terminated strings
// back to back, the program name first, in args_len bytes.
LATHA_PROC_IMPORT("spawn")
int32_t latha_spawn(const char *path, size_t path_len, const char *args, size_t args_len, uint32_t *pid, uint32_t *h);

// A child's exit code into *code once it ended; LATHA_E_TIMEOUT before that.
LATHA_PROC_IMPORT("wait_exit") int32_t latha_wait_exit(uint32_t pid, int32_t timeout_ms, int32_t *code);

LATHA_PROC_IMPORT("kill") int32_t latha_kill(uint32_t pid);

#ifdef __cplusplus
}
#endif
