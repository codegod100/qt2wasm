char *tzname[2] = { (char*)"UTC", (char*)"UTC" };
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
/* Linker shims for Qt on wasm32-wasip1 (no eventfd, flock, mremap, hostname, C++ EH). */
int eventfd(unsigned int v, int f) { (void)v; (void)f; errno = ENOSYS; return -1; }
int eventfd_read(int fd, uint64_t *v) { (void)fd; (void)v; errno = ENOSYS; return -1; }
int eventfd_write(int fd, uint64_t v) { (void)fd; (void)v; errno = ENOSYS; return -1; }
int flock(int fd, int op) { (void)fd; (void)op; return 0; }
void *mremap(void *o, size_t os, size_t ns, int f, ...) { (void)o; (void)os; (void)ns; (void)f; errno = ENOSYS; return (void *)-1; }
int gethostname(char *n, size_t l) { if (l) { strncpy(n, "wasi", l); n[l - 1] = 0; } return 0; }
void *__cxa_allocate_exception(size_t n) { return malloc(n); }
_Noreturn void __cxa_throw(void *e, void *t, void (*d)(void *)) { (void)e; (void)t; (void)d; abort(); }
