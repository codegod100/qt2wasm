// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// latha:proc check (tests/proc-test.sh builds and runs it): a wasm32-wasip1
// command that starts copies of itself as processes and talks to them over
// channels, passes an endpoint along a message, kills a busy child; plus
// setjmp/longjmp (wasm exceptions) and musttail (wasm tail calls), the two
// engine features webby's wasm build needs. Shared memory (latha:proc memory)
// between two processes, and spawn_env.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../qt2wasm/proc/latha_proc.h"

#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            fprintf(stderr, "proc-test: %s:%d: check failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

static jmp_buf jb;
static __attribute__((noinline)) void jump(int v) { longjmp(jb, v); }

static __attribute__((noinline)) long count(long n, long acc)
{
    if (n == 0)
        return acc;
    __attribute__((musttail)) return count(n - 1, acc + 1);
}

static int child_echo(void)
{
    uint32_t p;
    CHECK(latha_parent(&p) == 0);
    CHECK(latha_parent(&p) == LATHA_E_BAD_HANDLE); // only once
    for (;;) {
        char buf[64];
        uint32_t hs[4], len, nh;
        int r = latha_recv(p, buf, sizeof buf - 1, hs, 4, &len, &nh, -1);
        if (r == LATHA_E_CLOSED)
            return 7; // the parent closed our channel: done
        CHECK(r == 0);
        buf[len] = 0;
        char out[96];
        int n = snprintf(out, sizeof out, "pong:%s", buf);
        // Reply on the endpoint that came with the message, if any.
        uint32_t to = nh ? hs[0] : p;
        CHECK(latha_send(to, out, n, NULL, 0) == 0);
        if (nh)
            CHECK(latha_close(hs[0]) == 0);
    }
}

static int child_spin(void)
{
    uint32_t p;
    CHECK(latha_parent(&p) == 0);
    CHECK(latha_send(p, "spinning", 8, NULL, 0) == 0);
    volatile unsigned long x = 0;
    for (;;)
        ++x; // until killed
}

// Shared memory: a memory object comes in a message; check the parent's bytes, write ours, answer.
static int child_mem(void)
{
    uint32_t p, hs[1], len, nh;
    char buf[8];
    CHECK(latha_parent(&p) == 0);
    CHECK(getenv("PROC_TEST") && !strcmp(getenv("PROC_TEST"), "mem")); // spawn_env's environment
    CHECK(latha_recv(p, buf, sizeof buf, hs, 1, &len, &nh, 5000) == 0 && nh == 1);
    CHECK(latha_is_memory(hs[0]) == 1 && latha_is_memory(p) == 0);
    uint64_t size;
    CHECK(latha_mem_size(hs[0], &size) == 0 && size == 8192);
    size_t page = (size_t)latha_page_size();
    char *m = aligned_alloc(page, 8192);
    CHECK(latha_mem_map(hs[0], 0, m, 8192) == 0);
    CHECK(!strcmp(m, "parent"));
    strcpy(m + 4096, "child");
    CHECK(latha_mem_unmap(m, 8192) == 0);
    CHECK(m[0] == 0); // our own (zeroed) pages again
    free(m);
    CHECK(latha_close(hs[0]) == 0);
    CHECK(latha_send(p, "done", 4, NULL, 0) == 0);
    return 0;
}

static void expect(uint32_t h, const char *want)
{
    char buf[64];
    uint32_t len, nh;
    // Too small a buffer first: the message stays and says how big it is.
    CHECK(latha_recv(h, buf, 1, NULL, 0, &len, &nh, 5000) == LATHA_E_TOO_BIG);
    CHECK(len == strlen(want));
    CHECK(latha_recv(h, buf, sizeof buf, NULL, 0, &len, &nh, 0) == 0);
    CHECK(len == strlen(want) && !memcmp(buf, want, len));
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "echo"))
        return child_echo();
    if (argc > 1 && !strcmp(argv[1], "spin"))
        return child_spin();
    if (argc > 1 && !strcmp(argv[1], "mem"))
        return child_mem();

    uint32_t p;
    CHECK(latha_parent(&p) == LATHA_E_BAD_HANDLE); // started by the host, no parent

    // Engine features.
    int v = setjmp(jb);
    if (v == 0)
        jump(42);
    CHECK(v == 42);
    CHECK(count(10000000, 0) == 10000000); // without tail calls this overflows the stack

    // A local channel.
    uint32_t ab[2];
    CHECK(latha_pair(ab) == 0);
    CHECK(latha_send(ab[0], "hi", 2, NULL, 0) == 0);
    uint8_t ready[2];
    CHECK(latha_wait(ab, 2, ready, 0) == 1 && !ready[0] && ready[1]);
    expect(ab[1], "hi");
    CHECK(latha_recv(ab[1], NULL, 0, NULL, 0, &(uint32_t){0}, &(uint32_t){0}, 10) == LATHA_E_TIMEOUT);

    // A child process: messages both ways.
    static const char echo_args[] = "proc-test\0echo";
    uint32_t pid, ch;
    CHECK(latha_spawn("", 0, echo_args, sizeof echo_args, &pid, &ch) == 0);
    CHECK(latha_send(ch, "one", 3, NULL, 0) == 0);
    expect(ch, "pong:one");

    // An endpoint travels with a message; the answer comes back on its peer.
    uint32_t xy[2];
    CHECK(latha_pair(xy) == 0);
    CHECK(latha_send(ch, "two", 3, &xy[1], 1) == 0);
    CHECK(latha_close(xy[1]) == LATHA_E_BAD_HANDLE); // it moved
    expect(xy[0], "pong:two");
    // The child closed its copy: we see the close.
    CHECK(latha_recv(xy[0], NULL, 0, NULL, 0, &(uint32_t){0}, &(uint32_t){0}, 5000) == LATHA_E_CLOSED);

    // Closing our end ends the child's loop.
    int32_t code;
    CHECK(latha_wait_exit(pid, 0, &code) == LATHA_E_TIMEOUT);
    CHECK(latha_close(ch) == 0);
    CHECK(latha_wait_exit(pid, 5000, &code) == 0 && code == 7);

    // A busy child, killed.
    static const char spin_args[] = "proc-test\0spin";
    uint32_t spid, sch;
    CHECK(latha_spawn("", 0, spin_args, sizeof spin_args, &spid, &sch) == 0);
    expect(sch, "spinning");
    CHECK(latha_kill(spid) == 0);
    CHECK(latha_wait_exit(spid, 5000, &code) == 0 && code == 134);
    CHECK(latha_recv(sch, NULL, 0, NULL, 0, &(uint32_t){0}, &(uint32_t){0}, 5000) == LATHA_E_CLOSED);

    // Shared memory with a child (started with its own environment).
    size_t page = (size_t)latha_page_size();
    CHECK(page >= 4096 && 8192 % page == 0);
    uint32_t mh, mh2;
    CHECK(latha_mem_create(8192, &mh) == 0 && latha_mem_dup(mh, &mh2) == 0);
    char *m = aligned_alloc(page, 8192);
    CHECK(latha_mem_map(mh, 0, m, 8192) == 0);
    CHECK(latha_mem_map(mh, 0, (char *)m + 1, page) == LATHA_E_UNALIGNED);
    CHECK(latha_mem_resize(mh, 4096) == LATHA_E_FAULT); // never smaller
    strcpy(m, "parent");
    static const char mem_args[] = "proc-test\0mem";
    static const char mem_env[] = "PROC_TEST=mem";
    uint32_t mpid, mch;
    CHECK(latha_spawn_env("", 0, mem_args, sizeof mem_args, mem_env, sizeof mem_env, &mpid, &mch) == 0);
    CHECK(latha_send(mch, "m", 1, &mh2, 1) == 0);
    expect(mch, "done");
    CHECK(latha_wait_exit(mpid, 5000, &code) == 0 && code == 0);
    CHECK(!strcmp(m + 4096, "child"));
    CHECK(latha_mem_unmap(m, 8192) == 0 && latha_close(mh) == 0);
    free(m);

    // A program that is not there.
    CHECK(latha_spawn("no-such.wasm", 12, NULL, 0, &spid, &sch) == LATHA_E_SPAWN);

    printf("proc-test: ok\n");
    return 0;
}
