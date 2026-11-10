#pragma once
#include <sys/types.h>
struct passwd { char *pw_name; char *pw_passwd; uid_t pw_uid; gid_t pw_gid; char *pw_gecos; char *pw_dir; char *pw_shell; };
static inline struct passwd *getpwuid(uid_t u) { (void)u; return 0; }
static inline int getpwuid_r(uid_t u, struct passwd *p, char *b, size_t n, struct passwd **r) { (void)u;(void)p;(void)b;(void)n; *r = 0; return 0; }
