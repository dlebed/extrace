/* SPDX-License-Identifier: MIT */
/*
 * extrace_varint.h - standalone varint (unsigned LEB128) and zigzag module
 * (DESIGN §5.2, §8.13, Appendix 22.A).
 *
 * Needs only <stdint.h> and the compiler macros (likely, unlikely,
 * __always_inline). It knows nothing about records, rings or ports. The
 * producer-side encoders are 32-bit only: a 64-bit value is passed as two words.
 */
#ifndef EXTRACE_VARINT_H
#define EXTRACE_VARINT_H

#include <stdint.h>

#if !defined(likely) || !defined(unlikely) || !defined(__always_inline)
#include "extrace_compiler.h"
#endif

/* Longest encodings accepted by the decoders. */
#define EXTRACE_UV32_MAX 5u
#define EXTRACE_UV64_MAX 10u

#ifndef EXTRACE_VARINT_CLZ32 /* the port may define it (extrace_port_clz32) */
#if defined(__ARM_FEATURE_CLZ) || defined(__riscv_zbb)
#define EXTRACE_VARINT_CLZ32(v) ((unsigned int)EXTRACE_BUILTIN_CLZ32(v))
#endif
#endif

#ifdef EXTRACE_VARINT_CLZ32
static __always_inline unsigned int extrace_uv_len32(uint32_t v)
{
    /* one clz; 9 * c is a shift-add */
    return (352u - 9u * EXTRACE_VARINT_CLZ32(v | 1u)) >> 6;
}
#else /* ARMv6-M, ARMv8-M Baseline, hosts without a port CLZ */
static __always_inline unsigned int extrace_uv_len32(uint32_t v)
{
    if (likely(!(v >> 7)))
        return 1;
    if (!(v >> 14))
        return 2;
    if (!(v >> 21))
        return 3;
    return (v >> 28) ? 5 : 4;
}
#endif

/* Length of the 64-bit value hi:lo, using 32-bit operations only (§8.13 rule 9). */
static __always_inline unsigned int extrace_uv_len64(uint32_t lo, uint32_t hi)
{
    if (likely(!hi))
        return extrace_uv_len32(lo);
    return (hi >> 3) ? 5u + extrace_uv_len32(hi >> 3) : 5u;
}

/* Fully unrolled shift-test chain: one shift and one branch per byte. */
static __always_inline uint8_t *extrace_put_uv32(uint8_t *restrict p, uint32_t v)
{
    uint32_t t = v >> 7;

    if (likely(!t)) {
        p[0] = (uint8_t)v;
        return p + 1;
    }
    p[0] = (uint8_t)(v | 0x80u);
    v = t >> 7;
    if (likely(!v)) {
        p[1] = (uint8_t)t;
        return p + 2;
    }
    p[1] = (uint8_t)(t | 0x80u);
    t = v >> 7;
    if (!t) {
        p[2] = (uint8_t)v;
        return p + 3;
    }
    p[2] = (uint8_t)(v | 0x80u);
    v = t >> 7;
    if (!v) {
        p[3] = (uint8_t)t;
        return p + 4;
    }
    p[3] = (uint8_t)(t | 0x80u);
    p[4] = (uint8_t)v;
    return p + 5;
}

/*
 * The same chain without size hints, for values with no typical length, such as
 * timestamp deltas (1-3 bytes depending on the timer rate and event density).
 */
static __always_inline uint8_t *extrace_put_uv32_any(uint8_t *restrict p, uint32_t v)
{
    uint32_t t = v >> 7;

    if (!t) {
        p[0] = (uint8_t)v;
        return p + 1;
    }
    p[0] = (uint8_t)(v | 0x80u);
    v = t >> 7;
    if (!v) {
        p[1] = (uint8_t)t;
        return p + 2;
    }
    p[1] = (uint8_t)(t | 0x80u);
    t = v >> 7;
    if (!t) {
        p[2] = (uint8_t)v;
        return p + 3;
    }
    p[2] = (uint8_t)(v | 0x80u);
    v = t >> 7;
    if (!v) {
        p[3] = (uint8_t)t;
        return p + 4;
    }
    p[3] = (uint8_t)(t | 0x80u);
    p[4] = (uint8_t)v;
    return p + 5;
}

/* 64-bit value on a 32-bit core: only 32-bit shifts, no read-back (§8.13 rule 10). */
static inline uint8_t *extrace_put_uv64(uint8_t *restrict p, uint32_t lo, uint32_t hi)
{
    uint32_t b4, rest;

    if (likely(!hi))
        return extrace_put_uv32(p, lo);
    p[0] = (uint8_t)(lo | 0x80u);
    p[1] = (uint8_t)((lo >> 7) | 0x80u);
    p[2] = (uint8_t)((lo >> 14) | 0x80u);
    p[3] = (uint8_t)((lo >> 21) | 0x80u);
    b4 = (lo >> 28) | ((hi & 7u) << 4);
    rest = hi >> 3;
    if (!rest) {
        p[4] = (uint8_t)b4;
        return p + 5;
    }
    p[4] = (uint8_t)(b4 | 0x80u); /* built in a register: no read-back */
    return extrace_put_uv32(p + 5, rest);
}

static __always_inline uint32_t extrace_zz32(int32_t s)
{
    return ((uint32_t)s << 1) ^ (0u - ((uint32_t)s >> 31));
}

static __always_inline uint64_t extrace_zz64(int64_t s)
{
    return ((uint64_t)s << 1) ^ (0u - ((uint64_t)s >> 63));
}

static __always_inline int32_t extrace_unzz32(uint32_t u)
{
    return (int32_t)((u >> 1) ^ (0u - (u & 1u)));
}

static __always_inline int64_t extrace_unzz64(uint64_t u)
{
    return (int64_t)((u >> 1) ^ (0u - (u & 1u)));
}

/*
 * Consumer-side decoder: rejects encodings longer than 5 bytes and a fifth byte
 * that does not fit in 32 bits. Returns the bytes consumed, or 0 on error.
 */
static inline unsigned int extrace_get_uv32(const uint8_t *p, const uint8_t *end,
                                            uint32_t *out)
{
    uint32_t v = 0;
    unsigned int shift = 0, n = 0;

    if (likely(p < end && !(p[0] & 0x80u))) { /* 1-byte fast path */
        *out = p[0];
        return 1;
    }
    if (likely(end - p >= 5)) { /* unrolled, no bounds checks */
        v = p[0] & 0x7Fu;
        if (!(p[1] & 0x80u)) {
            *out = v | (uint32_t)p[1] << 7;
            return 2;
        }
        v |= (uint32_t)(p[1] & 0x7Fu) << 7;
        if (!(p[2] & 0x80u)) {
            *out = v | (uint32_t)p[2] << 14;
            return 3;
        }
        v |= (uint32_t)(p[2] & 0x7Fu) << 14;
        if (!(p[3] & 0x80u)) {
            *out = v | (uint32_t)p[3] << 21;
            return 4;
        }
        v |= (uint32_t)(p[3] & 0x7Fu) << 21;
        if (p[4] > 0x0Fu) /* continuation or > 32 bits */
            return 0;
        *out = v | (uint32_t)p[4] << 28;
        return 5;
    }
    while (p + n < end && n < 5) { /* checked tail of a window */
        uint8_t b = p[n++];

        if (n == 5 && b > 0x0Fu)
            return 0;
        v |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) {
            *out = v;
            return n;
        }
        shift += 7;
    }
    return 0;
}

/*
 * 64-bit decoder: rejects encodings longer than 10 bytes and a tenth byte that
 * does not fit in 64 bits. Returns the bytes consumed, or 0 on error.
 */
static inline unsigned int extrace_get_uv64(const uint8_t *p, const uint8_t *end,
                                            uint64_t *out)
{
    uint64_t v = 0;
    unsigned int shift = 0, n = 0;

    if (likely(p < end && !(p[0] & 0x80u))) {
        *out = p[0];
        return 1;
    }
    while (p + n < end && n < 10) {
        uint8_t b = p[n++];

        if (n == 10 && b > 0x01u)
            return 0;
        v |= (uint64_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) {
            *out = v;
            return n;
        }
        shift += 7;
    }
    return 0;
}

#endif /* EXTRACE_VARINT_H */
