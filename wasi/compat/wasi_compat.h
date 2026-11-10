/* Force-included shims for building Qt against wasi-libc. */
#pragma once
#ifdef __wasi__
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline uid_t geteuid(void) { return 0; }
static inline uid_t getuid(void) { return 0; }
static inline gid_t getegid(void) { return 0; }
static inline gid_t getgid(void) { return 0; }
static inline void tzset(void) {}
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#ifndef F_DUPFD
#define F_DUPFD 0
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef POLLPRI
#define POLLPRI 0x002
#endif
static inline int dup(int fd) { (void)fd; errno = ENOSYS; return -1; }
static inline int dup2(int a, int b) { (void)a; (void)b; errno = ENOSYS; return -1; }
static inline int pipe(int fds[2]) { (void)fds; errno = ENOSYS; return -1; }
static inline int kill(pid_t p, int s) { (void)p; (void)s; errno = ENOSYS; return -1; }
#ifndef SA_RESTART
struct sigaction { void (*sa_handler)(int); void (*sa_sigaction)(int, void *, void *); sigset_t sa_mask; int sa_flags; };
#define SA_RESTART 0x10000000
#define SA_SIGINFO 4
#define SA_NOCLDSTOP 1
static inline int sigaction(int s, const struct sigaction *a, struct sigaction *o) { (void)s; (void)a; if (o) { o->sa_handler = 0; o->sa_flags = 0; } return 0; }
static inline int sigemptyset(sigset_t *set) { __builtin_memset(set, 0, sizeof *set); return 0; }
static inline int sigaddset(sigset_t *set, int s) { (void)set; (void)s; return 0; }
#endif
extern char *tzname[2];
#ifdef __cplusplus
}
#endif
#endif
