/* wasi shim: no setjmp/longjmp without the wasm exception-handling proposal.
   setjmp always returns 0; longjmp traps. Errors in libpng/freetype abort. */
#pragma once
typedef long jmp_buf[1];
typedef long sigjmp_buf[1];
#define setjmp(b) (0)
#define _setjmp(b) (0)
#define sigsetjmp(b, s) (0)
#ifdef __cplusplus
extern "C" {
#endif
_Noreturn static inline void longjmp(jmp_buf b, int v) { (void)b; (void)v; __builtin_trap(); }
_Noreturn static inline void _longjmp(jmp_buf b, int v) { (void)b; (void)v; __builtin_trap(); }
_Noreturn static inline void siglongjmp(sigjmp_buf b, int v) { (void)b; (void)v; __builtin_trap(); }
#ifdef __cplusplus
}
#endif
