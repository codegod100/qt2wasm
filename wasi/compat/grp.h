#pragma once
#include <sys/types.h>
struct group { char *gr_name; char *gr_passwd; gid_t gr_gid; char **gr_mem; };
static inline struct group *getgrgid(gid_t g) { (void)g; return 0; }
static inline int getgrgid_r(gid_t g, struct group *gr, char *b, size_t n, struct group **r) { (void)g;(void)gr;(void)b;(void)n; *r = 0; return 0; }
