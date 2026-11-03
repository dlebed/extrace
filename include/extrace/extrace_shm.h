/* SPDX-License-Identifier: MIT */
/*
 * extrace_shm.h - shared-memory layout, ownership and access helpers (DESIGN §6, §7.1).
 *
 * Every field touched by more than one agent after init is an aligned 32-bit word
 * with one writer, accessed only through extrace_shm_ld32()/extrace_shm_st32().
 * Shared memory is little-endian (D18). No pointers are ever stored in it.
 */
#ifndef EXTRACE_SHM_H
#define EXTRACE_SHM_H

#include <stdint.h>
#include <stddef.h>
#include "extrace_cfg.h"

#define EXTRACE_SHM_MAGIC 0x52545845u /* "EXTR" little-endian */
#define EXTRACE_LAYOUT_MAJOR 2u       /* §6.2 */
#define EXTRACE_LAYOUT_MINOR 0u
#define EXTRACE_LAYOUT_VERSION ((EXTRACE_LAYOUT_MAJOR << 8) | EXTRACE_LAYOUT_MINOR)

/* pinfo.flags */
#define EXTRACE_PI_BIG_ENDIAN 0x01u
#define EXTRACE_PI_TS_GLOBAL 0x08u

/* pstate.status */
#define EXTRACE_ST_RUNNING 1u
#define EXTRACE_ST_PANIC 2u

/* Ring block layout (§6.1). */
#define EXTRACE_PINFO_OFF 0u
#define EXTRACE_PSTATE_OFF 64u
#define EXTRACE_CSTATE_OFF 96u
#define EXTRACE_MASK_SET_OFF 128u
#define EXTRACE_MASK_CLR_OFF(w) (EXTRACE_MASK_SET_OFF + 4u * (w))
#define EXTRACE_CTL_SIZE(w) (EXTRACE_MASK_SET_OFF + 8u * (w))
#define EXTRACE_RING_BLOCK_SIZE(data, w) (EXTRACE_CTL_SIZE(w) + (data))
#define EXTRACE_RING_BLOCK_SIZE_MAX(data) \
    EXTRACE_RING_BLOCK_SIZE(data, (EXTRACE_CFG_MAX_EVENTS + 31u) / 32u)
#define EXTRACE_MIN_RING_SIZE 64u

struct extrace_shm_pinfo {   /* producer-owned, written once per epoch */
    uint32_t magic;          /* written last during init */
    uint16_t layout_ver;     /* major << 8 | minor */
    uint8_t wire_ver;
    uint8_t flags;           /* EXTRACE_PI_BIG_ENDIAN, _TS_GLOBAL */
    uint8_t ring_id;         /* system-wide unique, 0..254 */
    uint8_t reserved0;
    uint16_t reserved2;      /* 0 */
    uint32_t data_off;       /* from block base */
    uint32_t data_size;
    uint16_t mask_words;     /* ceil(ev_count / 32), sizes mask_set/mask_clr */
    uint16_t ev_count;       /* highest event ID + 1 of the producer image */
    uint32_t max_record;     /* EXTRACE_MAX_RECORD of the producer image */
    uint32_t ts_freq;        /* global timer ticks per second */
    uint32_t reserved1;
    uint8_t build_id[20];    /* GNU build-id (truncated/zero-padded) */
    char name[8];            /* instance name; no NUL if 8 chars */
};

struct extrace_shm_pstate {  /* producer-owned, dynamic */
    uint32_t epoch;          /* incremented on every producer init */
    uint32_t head;           /* published free-running index, record boundary */
    uint32_t lost_total;     /* cumulative dropped records (diagnostics) */
    uint32_t mask_gen_ack;   /* last applied mask generation */
    uint32_t cmd_seq_ack;    /* last executed command sequence */
    uint32_t hwm;            /* high-water mark of ring usage, bytes */
    uint32_t status;         /* RUNNING, PANIC */
    uint32_t publish_cnt;    /* diagnostics */
};

struct extrace_shm_cstate {  /* consumer-owned, dynamic */
    uint32_t magic;          /* EXTRACE_SHM_MAGIC once the consumer has attached */
    uint32_t attached_epoch; /* epoch the consumer acknowledged */
    uint32_t tail;           /* consumed free-running index, record boundary */
    uint32_t mask_gen;       /* seqlock generation of mask_set/mask_clr */
    uint32_t notify_thr;     /* doorbell after this many newly published bytes; 0 = off */
    uint32_t publish_thr;    /* producer publish threshold hint; 0 = default */
    uint32_t cmd_seq;        /* incremented after cmd_bits is updated */
    uint32_t cmd_bits;       /* pending EXTRACE_CMD_* */
};

static_assert(sizeof(struct extrace_shm_pinfo) == 64);
static_assert(sizeof(struct extrace_shm_pstate) == 32);
static_assert(sizeof(struct extrace_shm_cstate) == 32);
static_assert(offsetof(struct extrace_shm_pinfo, layout_ver) == 4);
static_assert(offsetof(struct extrace_shm_pinfo, ring_id) == 8);
static_assert(offsetof(struct extrace_shm_pinfo, data_off) == 12);
static_assert(offsetof(struct extrace_shm_pinfo, mask_words) == 20);
static_assert(offsetof(struct extrace_shm_pinfo, max_record) == 24);
static_assert(offsetof(struct extrace_shm_pinfo, ts_freq) == 28);
static_assert(offsetof(struct extrace_shm_pinfo, build_id) == 36);
static_assert(offsetof(struct extrace_shm_pinfo, name) == 56);
static_assert(offsetof(struct extrace_shm_pstate, head) == 4);
static_assert(offsetof(struct extrace_shm_pstate, publish_cnt) == 28);
static_assert(offsetof(struct extrace_shm_cstate, tail) == 8);
static_assert(offsetof(struct extrace_shm_cstate, cmd_bits) == 28);

#define EXTRACE_PINFO_WORDS 16u

/* ---- byte order (D18) ---- */

static __always_inline uint32_t extrace_le32(uint32_t v)
{
#if EXTRACE_BIG_ENDIAN
    return EXTRACE_BUILTIN_BSWAP32(v);
#else
    return v;
#endif
}

/*
 * Raw word access. A port may route shared-memory accesses through its own hooks
 * (EXTRACE_PORT_SHM_LD32/ST32), for example to give ThreadSanitizer acquire/release
 * edges or to simulate write buffers. The value is in memory byte order.
 */
static __always_inline uint32_t extrace_shm_ld32_raw(const void *p)
{
#ifdef EXTRACE_PORT_SHM_LD32
    return EXTRACE_PORT_SHM_LD32(p);
#else
    return *(const volatile uint32_t *)p;
#endif
}

static __always_inline void extrace_shm_st32_raw(void *p, uint32_t v)
{
#ifdef EXTRACE_PORT_SHM_ST32
    EXTRACE_PORT_SHM_ST32(p, v);
#else
    *(volatile uint32_t *)p = v;
#endif
}

/* Load a shared control word written by the other agent. */
static __always_inline uint32_t extrace_shm_ld32(const void *p)
{
    return extrace_le32(extrace_shm_ld32_raw(p));
}

/*
 * Load a shared word that this agent writes itself. Allowed only at the
 * deliberate once-per-(re)start exceptions of §6.3 rule 8.
 */
static __always_inline uint32_t extrace_shm_ld32_own(const void *p)
{
#ifdef EXTRACE_PORT_SHM_LD32_OWN
    return extrace_le32(EXTRACE_PORT_SHM_LD32_OWN(p));
#else
    return extrace_shm_ld32(p);
#endif
}

/* Store a shared control word owned by this agent. */
static __always_inline void extrace_shm_st32(void *p, uint32_t v)
{
    extrace_shm_st32_raw(p, extrace_le32(v));
}

/* ---- pinfo as a little-endian word image (§6.2) ---- */

static inline uint32_t extrace_bytes_le32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static inline void extrace_le32_bytes(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

/* Native struct -> the 16 word values of the little-endian byte image. */
static inline void extrace_pinfo_pack(const struct extrace_shm_pinfo *pi,
                                      uint32_t w[EXTRACE_PINFO_WORDS])
{
    w[0] = pi->magic;
    w[1] = (uint32_t)pi->layout_ver | (uint32_t)pi->wire_ver << 16 | (uint32_t)pi->flags << 24;
    w[2] = (uint32_t)pi->ring_id | (uint32_t)pi->reserved0 << 8 | (uint32_t)pi->reserved2 << 16;
    w[3] = pi->data_off;
    w[4] = pi->data_size;
    w[5] = (uint32_t)pi->mask_words | (uint32_t)pi->ev_count << 16;
    w[6] = pi->max_record;
    w[7] = pi->ts_freq;
    w[8] = pi->reserved1;
    for (unsigned int i = 0; i < 5; i++)
        w[9 + i] = extrace_bytes_le32(&pi->build_id[4 * i]);
    for (unsigned int i = 0; i < 2; i++)
        w[14 + i] = extrace_bytes_le32((const uint8_t *)&pi->name[4 * i]);
}

static inline void extrace_pinfo_unpack(struct extrace_shm_pinfo *pi,
                                        const uint32_t w[EXTRACE_PINFO_WORDS])
{
    pi->magic = w[0];
    pi->layout_ver = (uint16_t)w[1];
    pi->wire_ver = (uint8_t)(w[1] >> 16);
    pi->flags = (uint8_t)(w[1] >> 24);
    pi->ring_id = (uint8_t)w[2];
    pi->reserved0 = (uint8_t)(w[2] >> 8);
    pi->reserved2 = (uint16_t)(w[2] >> 16);
    pi->data_off = w[3];
    pi->data_size = w[4];
    pi->mask_words = (uint16_t)w[5];
    pi->ev_count = (uint16_t)(w[5] >> 16);
    pi->max_record = w[6];
    pi->ts_freq = w[7];
    pi->reserved1 = w[8];
    for (unsigned int i = 0; i < 5; i++)
        extrace_le32_bytes(&pi->build_id[4 * i], w[9 + i]);
    for (unsigned int i = 0; i < 2; i++)
        extrace_le32_bytes((uint8_t *)&pi->name[4 * i], w[14 + i]);
}

/* ---- free-running indices (§6.6) ---- */

static __always_inline uint32_t extrace_idx_used(uint32_t head, uint32_t tail)
{
    return head - tail;
}

static __always_inline uint32_t extrace_idx_off(uint32_t idx, uint32_t mask)
{
    return idx & mask;
}

static __always_inline bool extrace_is_pow2(uint32_t v)
{
    return v && !(v & (v - 1u));
}

/* Pointers into a ring block mapped at base (8-byte aligned, checked at init and attach). */
static __always_inline struct extrace_shm_pinfo *extrace_blk_pinfo(void *base)
{
    return (struct extrace_shm_pinfo *)(void *)((uint8_t *)base + EXTRACE_PINFO_OFF);
}

static __always_inline struct extrace_shm_pstate *extrace_blk_pstate(void *base)
{
    return (struct extrace_shm_pstate *)(void *)((uint8_t *)base + EXTRACE_PSTATE_OFF);
}

static __always_inline struct extrace_shm_cstate *extrace_blk_cstate(void *base)
{
    return (struct extrace_shm_cstate *)(void *)((uint8_t *)base + EXTRACE_CSTATE_OFF);
}

static __always_inline uint32_t *extrace_blk_mask_set(void *base)
{
    return (uint32_t *)(void *)((uint8_t *)base + EXTRACE_MASK_SET_OFF);
}

static __always_inline uint32_t *extrace_blk_mask_clr(void *base, uint32_t words)
{
    return (uint32_t *)(void *)((uint8_t *)base + EXTRACE_MASK_CLR_OFF(words));
}

#endif /* EXTRACE_SHM_H */
