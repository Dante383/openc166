/* SPDX-License-Identifier: MIT */
/* <assert.h> of openc166's C library. Included again, it follows NDEBUG
   anew, as C requires. */
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
_Noreturn void __assert(const char *expr, const char *file, int line);
#define assert(e) ((e) ? (void)0 : __assert(#e, __FILE__, __LINE__))
#endif

#if __STDC_VERSION__ >= 201112L && !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
