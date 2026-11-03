/* SPDX-License-Identifier: MIT */
/*
 * extrace_args.h - compile-time argument classification and counting (DESIGN §8.12).
 *
 * EXTRACE_AC(x) is an integer constant expression and never evaluates x. The value
 * converters evaluate their argument once.
 */
#ifndef EXTRACE_ARGS_H
#define EXTRACE_ARGS_H

#include <stdint.h>
#include "extrace_types.h"

#define EXTRACE_CAT_(a, b) a##b
#define EXTRACE_CAT(a, b) EXTRACE_CAT_(a, b)

/* ---- argument counting and mapping, 1..16 arguments ---- */
#define EXTRACE_NARGS(...) \
    EXTRACE_NARGS_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define EXTRACE_NARGS_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, \
                       _16, n, ...)                                                     \
    n
#define EXTRACE_MAP_1(M, J, a0) M(0, a0)
#define EXTRACE_MAP_2(M, J, a0, a1) M(0, a0) J() M(1, a1)
#define EXTRACE_MAP_3(M, J, a0, a1, a2) M(0, a0) J() M(1, a1) J() M(2, a2)
#define EXTRACE_MAP_4(M, J, a0, a1, a2, a3) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3)
#define EXTRACE_MAP_5(M, J, a0, a1, a2, a3, a4) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4)
#define EXTRACE_MAP_6(M, J, a0, a1, a2, a3, a4, a5) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5)
#define EXTRACE_MAP_7(M, J, a0, a1, a2, a3, a4, a5, a6) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5) J() M(6, a6)
#define EXTRACE_MAP_8(M, J, a0, a1, a2, a3, a4, a5, a6, a7) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5) J() M(6, a6) J() M(7, a7)
#define EXTRACE_MAP_9(M, J, a0, a1, a2, a3, a4, a5, a6, a7, a8) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5) J() M(6, a6) J() M(7, a7) J() \
    M(8, a8)
#define EXTRACE_MAP_10(M, J, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5) J() M(6, a6) J() M(7, a7) J() \
    M(8, a8) J() M(9, a9)
#define EXTRACE_MAP_11(M, J, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5) J() M(6, a6) J() M(7, a7) J() \
    M(8, a8) J() M(9, a9) J() M(10, a10)
#define EXTRACE_MAP_12(M, J, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5) J() M(6, a6) J() M(7, a7) J() \
    M(8, a8) J() M(9, a9) J() M(10, a10) J() M(11, a11)
#define EXTRACE_MAP_13(M, J, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5) J() M(6, a6) J() M(7, a7) J() \
    M(8, a8) J() M(9, a9) J() M(10, a10) J() M(11, a11) J() \
    M(12, a12)
#define EXTRACE_MAP_14(M, J, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5) J() M(6, a6) J() M(7, a7) J() \
    M(8, a8) J() M(9, a9) J() M(10, a10) J() M(11, a11) J() \
    M(12, a12) J() M(13, a13)
#define EXTRACE_MAP_15(M, J, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5) J() M(6, a6) J() M(7, a7) J() \
    M(8, a8) J() M(9, a9) J() M(10, a10) J() M(11, a11) J() \
    M(12, a12) J() M(13, a13) J() M(14, a14)
#define EXTRACE_MAP_16(M, J, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15) \
    M(0, a0) J() M(1, a1) J() M(2, a2) J() M(3, a3) J() \
    M(4, a4) J() M(5, a5) J() M(6, a6) J() M(7, a7) J() \
    M(8, a8) J() M(9, a9) J() M(10, a10) J() M(11, a11) J() \
    M(12, a12) J() M(13, a13) J() M(14, a14) J() M(15, a15)

#define EXTRACE_MAP(M, J, ...) \
    EXTRACE_CAT(EXTRACE_MAP_, EXTRACE_NARGS(__VA_ARGS__))(M, J, __VA_ARGS__)
#define EXTRACE_J_OR() |
#define EXTRACE_J_AND() &&
#define EXTRACE_J_COMMA() ,

/* 1, 2, 3 for the register-argument emitters, A for the array emitters. */
#if EXTRACE_CFG_EMIT_SPECIALIZE
#define EXTRACE_SPEC_N(...) \
    EXTRACE_NARGS_(__VA_ARGS__, A, A, A, A, A, A, A, A, A, A, A, A, A, 3, 2, 1, 0)
#else
#define EXTRACE_SPEC_N(...) A
#endif

/* ---- classification (§8.12, §10.3) ---- */

/* A pointer to a string constant in read-only data, logged by address (§8.12). */
struct extrace_cstr {
    const char *s;
};
#define EXTRACE_CSTR(str) ((struct extrace_cstr){ (str) })

#define EXTRACE_PTR64 (sizeof(void *) == 8)
#define EXTRACE_AC_SLONG (sizeof(long) == 8 ? EXTRACE_AC_S64 : EXTRACE_AC_S32)
#define EXTRACE_AC_ULONG (sizeof(long) == 8 ? EXTRACE_AC_U64 : EXTRACE_AC_U32)

#define EXTRACE_AC(x)                                                                 \
    _Generic((x),                                                                     \
        bool: EXTRACE_AC_U32, char: EXTRACE_AC_S32, signed char: EXTRACE_AC_S32,      \
        short: EXTRACE_AC_S32, int: EXTRACE_AC_S32, long: EXTRACE_AC_SLONG,           \
        long long: EXTRACE_AC_S64, unsigned char: EXTRACE_AC_U32,                     \
        unsigned short: EXTRACE_AC_U32, unsigned int: EXTRACE_AC_U32,                 \
        unsigned long: EXTRACE_AC_ULONG, unsigned long long: EXTRACE_AC_U64,          \
        float: EXTRACE_AC_F32, double: EXTRACE_AC_F64, char *: EXTRACE_AC_STR,        \
        const char *: EXTRACE_AC_STR, struct extrace_cstr: EXTRACE_AC_CSTR,           \
        default: EXTRACE_AC_PTR)

#define EXTRACE_AC_SIGNED(c) ((c) == EXTRACE_AC_S32 || (c) == EXTRACE_AC_S64)
#define EXTRACE_AC_ISINT(c) \
    ((c) == EXTRACE_AC_U32 || (c) == EXTRACE_AC_S32 || (c) == EXTRACE_AC_U64 || (c) == EXTRACE_AC_S64)
#define EXTRACE_AC_IS64(c)                                                            \
    ((c) == EXTRACE_AC_U64 || (c) == EXTRACE_AC_S64 || (c) == EXTRACE_AC_F64 ||       \
     (EXTRACE_PTR64 &&                                                                \
      ((c) == EXTRACE_AC_STR || (c) == EXTRACE_AC_PTR || (c) == EXTRACE_AC_CSTR)))

#define EXTRACE_M_SIGLO_(i, x) ((i) < 8 ? (uint32_t)EXTRACE_AC(x) << (4 * ((i) & 7)) : 0u)
#define EXTRACE_M_SIGHI_(i, x) ((i) >= 8 ? (uint32_t)EXTRACE_AC(x) << (4 * ((i) & 7)) : 0u)
#define EXTRACE_M_SIGN_(i, x) ((uint32_t)EXTRACE_AC_SIGNED(EXTRACE_AC(x)) << (i))
#define EXTRACE_M_IS64_(i, x) EXTRACE_AC_IS64(EXTRACE_AC(x))
#define EXTRACE_M_ISINT_(i, x) EXTRACE_AC_ISINT(EXTRACE_AC(x))
#define EXTRACE_M_AV32_(i, x) EXTRACE_AV32(x)
#define EXTRACE_M_AV64_(i, x) EXTRACE_AV64(x)

/* Compile-time signature words, sign mask and properties of an argument list. */
#define EXTRACE_SIG_LO(...) (0u | EXTRACE_MAP(EXTRACE_M_SIGLO_, EXTRACE_J_OR, __VA_ARGS__))
#define EXTRACE_SIG_HI(...) (0u | EXTRACE_MAP(EXTRACE_M_SIGHI_, EXTRACE_J_OR, __VA_ARGS__))
#define EXTRACE_SIGN_MASK(...) (0u | EXTRACE_MAP(EXTRACE_M_SIGN_, EXTRACE_J_OR, __VA_ARGS__))
#define EXTRACE_ANY64(...) (0 || EXTRACE_MAP(EXTRACE_M_IS64_, EXTRACE_J_OR, __VA_ARGS__))
#define EXTRACE_ALL_INT(...) (1 && EXTRACE_MAP(EXTRACE_M_ISINT_, EXTRACE_J_AND, __VA_ARGS__))
#define EXTRACE_AV32_LIST(...) EXTRACE_MAP(EXTRACE_M_AV32_, EXTRACE_J_COMMA, __VA_ARGS__)
#define EXTRACE_AV64_LIST(...) EXTRACE_MAP(EXTRACE_M_AV64_, EXTRACE_J_COMMA, __VA_ARGS__)

/* ---- value conversion: integers extended, floats bit-cast, pointers via uintptr_t ---- */

static __always_inline uint32_t extrace_cv32_s(int64_t v)
{
    return (uint32_t)v;
}

static __always_inline uint32_t extrace_cv32_u(uint64_t v)
{
    return (uint32_t)v;
}

static __always_inline uint32_t extrace_cv32_f(float v)
{
    uint32_t u;

    EXTRACE_BUILTIN_MEMCPY(&u, &v, sizeof(u));
    return u;
}

static __always_inline uint32_t extrace_cv32_d(double v)
{
    return (uint32_t)v; /* never used: F64 selects the 64-bit path */
}

static __always_inline uint32_t extrace_cv32_p(const volatile void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static __always_inline uint32_t extrace_cv32_cstr(struct extrace_cstr c)
{
    return (uint32_t)(uintptr_t)c.s;
}

static __always_inline uint64_t extrace_cv64_s(int64_t v)
{
    return (uint64_t)v;
}

static __always_inline uint64_t extrace_cv64_u(uint64_t v)
{
    return v;
}

static __always_inline uint64_t extrace_cv64_f(float v)
{
    return extrace_cv32_f(v);
}

static __always_inline uint64_t extrace_cv64_d(double v)
{
    uint64_t u;

    EXTRACE_BUILTIN_MEMCPY(&u, &v, sizeof(u));
    return u;
}

static __always_inline uint64_t extrace_cv64_p(const volatile void *p)
{
    return (uint64_t)(uintptr_t)p;
}

static __always_inline uint64_t extrace_cv64_cstr(struct extrace_cstr c)
{
    return (uint64_t)(uintptr_t)c.s;
}

#define EXTRACE_CV_(x, w)                                                             \
    _Generic((x),                                                                     \
        bool: extrace_cv##w##_u, char: extrace_cv##w##_s, signed char: extrace_cv##w##_s, \
        short: extrace_cv##w##_s, int: extrace_cv##w##_s, long: extrace_cv##w##_s,   \
        long long: extrace_cv##w##_s, unsigned char: extrace_cv##w##_u,               \
        unsigned short: extrace_cv##w##_u, unsigned int: extrace_cv##w##_u,           \
        unsigned long: extrace_cv##w##_u, unsigned long long: extrace_cv##w##_u,      \
        float: extrace_cv##w##_f, double: extrace_cv##w##_d,                          \
        struct extrace_cstr: extrace_cv##w##_cstr, default: extrace_cv##w##_p)(x)
#define EXTRACE_AV32(x) EXTRACE_CV_(x, 32)
#define EXTRACE_AV64(x) EXTRACE_CV_(x, 64)

#endif /* EXTRACE_ARGS_H */
