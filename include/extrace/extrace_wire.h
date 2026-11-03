/* SPDX-License-Identifier: MIT */
/*
 * extrace_wire.h - record-level helpers on top of extrace_varint.h (DESIGN §5, §8.13):
 * pre-encoded headers, system record headers and padding.
 */
#ifndef EXTRACE_WIRE_H
#define EXTRACE_WIRE_H

#include <stdint.h>
#include <stdbool.h>
#include "extrace_varint.h"
#include "extrace_types.h"

#define EXTRACE_WIRE_VERSION 1u

/* One-byte headers of system records: (id << 3) | kind. */
#define EXTRACE_HDR_PAD1 0x00u /* PAD, NONE */
#define EXTRACE_HDR_PADN 0x04u /* PAD, BUF */
#define EXTRACE_HDR_SYNC ((EXTRACE_SYS_SYNC << 3) | EXTRACE_K_INTS)
#define EXTRACE_HDR_LOST ((EXTRACE_SYS_LOST << 3) | EXTRACE_K_INTS)
#define EXTRACE_HDR_INFO ((EXTRACE_SYS_INFO << 3) | EXTRACE_K_BUF)
#define EXTRACE_HDR_FTRACE_NONE ((EXTRACE_SYS_FTRACE << 3) | EXTRACE_K_NONE)
#define EXTRACE_HDR_FTRACE_MARK ((EXTRACE_SYS_FTRACE << 3) | EXTRACE_K_MARK)
#define EXTRACE_HDR_FTRACE_INTS ((EXTRACE_SYS_FTRACE << 3) | EXTRACE_K_INTS)
#define EXTRACE_HDR_CFG ((EXTRACE_SYS_CFG << 3) | EXTRACE_K_INTS)
#define EXTRACE_HDR_PANIC ((EXTRACE_SYS_PANIC << 3) | EXTRACE_K_INTS)
#define EXTRACE_HDR_FRAG ((EXTRACE_SYS_FRAG << 3) | EXTRACE_K_EXT)
#define EXTRACE_HDR_FTRACE_CALLER_NONE ((EXTRACE_SYS_FTRACE_CALLER << 3) | EXTRACE_K_NONE)
#define EXTRACE_HDR_FTRACE_CALLER_MARK ((EXTRACE_SYS_FTRACE_CALLER << 3) | EXTRACE_K_MARK)
#define EXTRACE_HDR_FTRACE_CALLER_INTS ((EXTRACE_SYS_FTRACE_CALLER << 3) | EXTRACE_K_INTS)
#define EXTRACE_HDR_SKIP ((EXTRACE_SYS_SKIP << 3) | EXTRACE_K_BUF)

/*
 * Header pre-encoded at compile time (§8.13 rule 1): the varint bytes of
 * (id << 3) | kind in bits 0..23 and their count in bits 30..31. Bits 24..26 may
 * carry a sign mask (specialized emitters), bit 29 the urgent flag.
 */
#define EXTRACE_HENC_RAW_(h)                                                              \
    ((h) < 0x80u ? ((h) | (1u << 30))                                                     \
     : (h) < 0x4000u                                                                      \
         ? (((h) & 0x7Fu) | 0x80u | (((h) >> 7) << 8) | (2u << 30))                       \
         : (((h) & 0x7Fu) | 0x80u | (((((h) >> 7) & 0x7Fu) | 0x80u) << 8) |               \
            (((h) >> 14) << 16) | (3u << 30)))
#define EXTRACE_HENC(id, kind) EXTRACE_HENC_RAW_((((uint32_t)(id)) << 3) | (uint32_t)(kind))
#define EXTRACE_HENC_URGENT (1u << 29)
#define EXTRACE_HENC_SM_SHIFT 24
#define EXTRACE_HENC_FLAGS_MASK (0x3Fu << 24)

static __always_inline uint32_t extrace_henc(uint32_t id, uint32_t kind)
{
    return EXTRACE_HENC(id, kind);
}

static __always_inline unsigned int extrace_henc_len(uint32_t henc)
{
    return henc >> 30;
}

static __always_inline uint32_t extrace_henc_kind(uint32_t henc)
{
    return henc & 7u;
}

static __always_inline bool extrace_kind_has_len(uint32_t kind)
{
    return kind >= EXTRACE_K_INTS;
}

/*
 * FTRACE and FTRACE_CALLER in the NONE and MARK shapes (§5.9.7): the header (and
 * the timestamp, NONE) is followed by uvarints with no length field, the address
 * (FTRACE) or the address and the caller (FTRACE_CALLER). Returns their number,
 * 0 for every other header.
 */
static __always_inline unsigned int extrace_hdr_ftrace_short(uint32_t h)
{
    return (h >> 1) == (EXTRACE_SYS_FTRACE << 2)          ? 1u
           : (h >> 1) == (EXTRACE_SYS_FTRACE_CALLER << 2) ? 2u
                                                          : 0u;
}

/* Header pre-encoded at compile time: user headers are 2 or 3 bytes. */
static __always_inline uint8_t *extrace_put_hdr(uint8_t *restrict p, uint32_t henc)
{
    p[0] = (uint8_t)henc;
    p[1] = (uint8_t)(henc >> 8);     /* user headers: always 2 bytes */
    if (unlikely(henc & (1u << 30))) /* count 3 (0b11): IDs >= 2048; count 2 is 0b10 */
        p[2] = (uint8_t)(henc >> 16);
    return p + (henc >> 30);
}

/*
 * Padding of exactly gap bytes (§5.8): PAD1 for one byte, else PADN with its
 * length written in exactly k bytes, over-long where no minimal encoding exists.
 * The skipped bytes are not written: at most five stores, all stores only.
 */
static inline void extrace_put_pad(uint8_t *restrict p, uint32_t gap)
{
    uint32_t k, n;

    if (gap == 1) {
        p[0] = EXTRACE_HDR_PAD1;
        return;
    }
    for (k = 1;; k++) {
        n = gap - 1u - k;
        if (k >= 4u || extrace_uv_len32(n) <= k)
            break;
    }
    p[0] = EXTRACE_HDR_PADN;
    for (uint32_t i = 1; i < k; i++) {
        p[i] = (uint8_t)((n & 0x7Fu) | 0x80u);
        n >>= 7;
    }
    p[k] = (uint8_t)n;
}

#endif /* EXTRACE_WIRE_H */
