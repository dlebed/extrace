/* SPDX-License-Identifier: MIT */
/*
 * extrace_compiler.h - kernel-style compiler macros for gcc and clang (DESIGN §13.7).
 *
 * Every definition is guarded, so toolchain headers that define some of these
 * names first (newlib <sys/cdefs.h>, RTOS headers) win.
 */
#ifndef EXTRACE_COMPILER_H
#define EXTRACE_COMPILER_H

#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif
#ifndef __noinline
#define __noinline __attribute__((__noinline__))
#endif
#ifndef __cold
#define __cold __attribute__((__cold__))
#endif
#ifndef __used
#define __used __attribute__((__used__))
#endif
#ifndef __maybe_unused
#define __maybe_unused __attribute__((__unused__))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((__aligned__(x)))
#endif
#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __section
#define __section(s) __attribute__((__section__(s)))
#endif
#ifndef __printf
#define __printf(a, b) __attribute__((__format__(__printf__, a, b)))
#endif
#ifndef __noreturn
#define __noreturn __attribute__((__noreturn__))
#endif
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#ifndef barrier
#define barrier() __asm__ __volatile__("" : : : "memory")
#endif
/* Make var opaque to the optimizer, which then neither folds nor recomputes it. */
#ifndef OPTIMIZER_HIDE_VAR
#define OPTIMIZER_HIDE_VAR(var) __asm__("" : "=r"(var) : "0"(var))
#endif
#ifndef EXTRACE_BUILTIN_CLZ32
#define EXTRACE_BUILTIN_CLZ32(v) __builtin_clz(v)
#endif
#ifndef EXTRACE_BUILTIN_BSWAP32
#define EXTRACE_BUILTIN_BSWAP32(v) __builtin_bswap32(v)
#endif
#ifndef EXTRACE_BUILTIN_MEMCPY
#define EXTRACE_BUILTIN_MEMCPY __builtin_memcpy
#endif
#ifndef EXTRACE_BUILTIN_MEMMOVE
#define EXTRACE_BUILTIN_MEMMOVE __builtin_memmove
#endif
#ifndef EXTRACE_BUILTIN_MEMSET
#define EXTRACE_BUILTIN_MEMSET __builtin_memset
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define EXTRACE_BIG_ENDIAN 1
#else
#define EXTRACE_BIG_ENDIAN 0
#endif

#endif /* EXTRACE_COMPILER_H */
