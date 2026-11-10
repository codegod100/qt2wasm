#pragma once
#include <sys/types.h>
#define WNOHANG 1
#define WUNTRACED 2
#define WIFEXITED(s) 1
#define WEXITSTATUS(s) ((s) & 0xff)
#define WIFSIGNALED(s) 0
#define WTERMSIG(s) 0
#ifdef __cplusplus
extern "C" {
#endif
static inline pid_t waitpid(pid_t p, int *s, int o) { (void)p; (void)s; (void)o; return -1; }
#ifdef __cplusplus
}
#endif
