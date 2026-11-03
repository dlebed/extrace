/* SPDX-License-Identifier: MIT */
/*
 * extrace_prod.h - producer API and macros (DESIGN §8).
 *
 * Every EXTRACE_* call site costs one load, a bit test and a branch while the
 * event is disabled, and its arguments are evaluated only when it is enabled.
 */
#ifndef EXTRACE_PROD_H
#define EXTRACE_PROD_H

#include <stdint.h>
#include <stddef.h>
#include "extrace_cfg.h"
#include "extrace_types.h"
#include "extrace_wire.h"
#include "extrace_shm.h"
#include "extrace_events.h"
#include "extrace_args.h"
#include "extrace_meta.h"

/* struct extrace_prod.flags */
#define EXTRACE_PF_READY 0x01u
#define EXTRACE_PF_PANIC 0x02u

struct extrace_prod_stats {
    uint32_t epoch;
    uint32_t lost_total;      /* records dropped in this epoch */
    uint32_t hwm;             /* high-water mark of ring usage, bytes */
    uint32_t records;         /* successful reservations, incl. system records */
    uint32_t publishes;
    uint32_t tail_refreshes;  /* slow-path and poll reads of cstate.tail */
    uint32_t tail_rejected;   /* tail values rejected as stale or invalid */
    uint32_t resv_open_max;   /* deepest nesting of open reservations */
    uint32_t init_busy;       /* init refused with -EXTRACE_EBUSY */
};

struct extrace_prod {
    /* ---- hot: read by every EXTRACE_* call ---- */
    uint32_t mask[EXTRACE_MASK_WORDS];

    /* ---- reservation state: modified only inside the critical section ---- */
    uint32_t head;            /* next reservation index (free-running) */
    uint32_t committed;       /* index: end of contiguous fully-written prefix */
    uint32_t tail_cache;      /* last accepted consumer tail index */
    uint32_t lost_pending;    /* drops not yet reported in-band */
    uint32_t lost_bytes;      /* bytes of those drops (EXTRACE_CFG_LOST_BYTES) */
    uint32_t last_ts;         /* raw 32-bit timer of last ts-carrying record */
    uint8_t inflight;         /* reservations not yet committed */
    uint8_t publishing;       /* a publish is running (any context) */
    uint8_t publish_again;    /* a context asked for publish while busy */
    uint8_t flags;            /* READY, PANIC */

    /* ---- warm/cold ---- */
    uint8_t notify_force;     /* ring the doorbell at the next publish */
    uint8_t attached_seen;    /* poll saw a consumer attached to this epoch */
    uint8_t polling;          /* debug: poll is running */
    uint8_t ring_id;
    char info_name[8];        /* instance name for INFO records */
    uint32_t published;       /* last index written to pstate.head */
    uint32_t notified;        /* published index at the last doorbell */
    uint32_t publish_thr, notify_thr, publish_thr_dflt;
    uint32_t lost_total, lost_total_st, epoch, mask_gen_seen, cmd_seq_seen, hwm;
    uint32_t publish_cnt;     /* mirrors of pstate words: stored, never read back */
    uint32_t frag_seq;        /* xfer_id of the last EXTRACE_BUF_LARGE (§5.9.6) */
    uint32_t last_sync_ts;    /* raw 32-bit timer of the last SYNC */
    uint32_t sync_period;     /* ticks, computed at init (§8.7) */
    uint8_t *data;            /* producer view of data[] */
    uint32_t size, idx_mask;  /* power of two; idx_mask = size - 1 (§6.6) */
    struct extrace_shm_pinfo *pi;
    struct extrace_shm_pstate *ps;
    const struct extrace_shm_cstate *cs;
    const uint32_t *mset, *mclr;
    struct extrace_prod_stats stats;
};

extern struct extrace_prod extrace_producer; /* the one producer of this instance */

static __always_inline bool extrace_ev_on(uint32_t id)
{
    return (extrace_producer.mask[id >> 5] >> (id & 31)) & 1u;
}

/* ---- lifecycle (§8.10, §8.11) ---- */

struct extrace_prod_cfg {
    void *block;              /* producer view of the ring block */
    uint32_t block_size;      /* whole block, incl. the control part */
    uint32_t data_size;       /* 0 = largest power of two after the control part */
    uint8_t ring_id;          /* system-wide unique, 0..254; per instance */
    uint16_t flags;           /* reserved, 0 */
    const char *name;         /* instance name, up to 8 chars, e.g. "dsp1" */
    uint32_t publish_thr;     /* 0 = default (size / 4) */
};

int extrace_prod_init(const struct extrace_prod_cfg *cfg);
void extrace_prod_poll(void);
void extrace_prod_flush(void);
void extrace_prod_panic(uint32_t reason, uintptr_t pc);
void extrace_prod_stats(struct extrace_prod_stats *out);

/* ---- emitters (out of line, shared by all call sites) ---- */

void extrace_emit_mark(uint32_t henc);
void extrace_emit_none(uint32_t henc);
void extrace_emit_ints32(uint32_t henc, uint32_t sign_mask, const uint32_t *v, uint32_t n);
void extrace_emit_ints64(uint32_t henc, uint32_t sign_mask, const uint64_t *v, uint32_t n);
void extrace_emit_ints32_1(uint32_t desc, uint32_t a0);
void extrace_emit_ints32_2(uint32_t desc, uint32_t a0, uint32_t a1);
void extrace_emit_ints32_3(uint32_t desc, uint32_t a0, uint32_t a1, uint32_t a2);
void extrace_emit_printf32(uint32_t henc, uint32_t site, uint32_t sig_lo, const uint32_t *v,
                           uint32_t n);
void extrace_emit_printf32_0(uint32_t henc, uint32_t site);
void extrace_emit_printf32_1(uint32_t desc, uint32_t site, uint32_t a0);
void extrace_emit_printf32_2(uint32_t desc, uint32_t site, uint32_t a0, uint32_t a1);
void extrace_emit_printf32_3(uint32_t desc, uint32_t site, uint32_t a0, uint32_t a1, uint32_t a2);
void extrace_emit_printf64(uint32_t henc, uint32_t site, uint32_t sig_lo, uint32_t sig_hi,
                           const uint64_t *v, uint32_t n);
void extrace_emit_buf(uint32_t henc, const void *p, size_t n);
void extrace_emit_str(uint32_t henc, const char *s);
void extrace_emit_buf_large(uint32_t id, const void *p, size_t n, uint32_t urgent);
#ifdef EXTRACE_PORT_HAS_VSNPRINTF
void extrace_emit_text(uint32_t henc, const char *fmt, ...) __printf(2, 3);
#endif

/* ---- runtime-ID variants: mask check only (§8.11) ---- */

bool extrace_emit_dyn_none(uint16_t id);
bool extrace_emit_dyn_ints(uint16_t id, const uint32_t *v, unsigned int n, uint32_t sign_mask);
bool extrace_emit_dyn_ints64(uint16_t id, const uint64_t *v, unsigned int n, uint32_t sign_mask);
bool extrace_emit_dyn_buf(uint16_t id, uint8_t kind, const void *p, uint32_t n);

/* ---- direct reservation (§8.14) ---- */

struct extrace_resv {
    void *payload;            /* len bytes, contiguous in the ring: filled by the caller */
    uint32_t len;
    uint8_t *rec;             /* private: first header byte, for cancel */
    uint8_t hdr_len;          /* private: header bytes, for cancel */
};

bool extrace_reserve(uint32_t henc, uint32_t len, struct extrace_resv *r);
bool extrace_reserve_aligned(uint32_t henc, uint32_t len, uint32_t align, struct extrace_resv *r);
void extrace_commit(struct extrace_resv *r);
void extrace_commit_urgent(struct extrace_resv *r);
void extrace_cancel(struct extrace_resv *r);

/* ---- macros (§8.2, §8.11) ---- */

#define EXTRACE_ON(EV) (EXTRACE_CT_##EV && unlikely(extrace_ev_on(EXTRACE_EV_##EV)))
#define EXTRACE_ENABLED(EV) EXTRACE_ON(EV)
#define EXTRACE_HDR(EV, K) EXTRACE_HENC(EXTRACE_EV_##EV, K)
#define EXTRACE_CHECK_KIND(EV, K)                                                     \
    static_assert((int)EXTRACE_KIND_##EV == (int)(K) ||                                \
                      (int)EXTRACE_KIND_##EV == (int)EXTRACE_K_ANY,                    \
                  "event " #EV " is declared with another kind")

#define EXTRACE_MARK_(EV, U)                                                          \
    do {                                                                              \
        if (EXTRACE_ON(EV)) {                                                         \
            EXTRACE_CHECK_KIND(EV, EXTRACE_K_MARK);                                   \
            extrace_emit_mark(EXTRACE_HDR(EV, EXTRACE_K_MARK) | (U));                 \
        }                                                                             \
    } while (0)
#define EXTRACE_MARK(EV) EXTRACE_MARK_(EV, 0u)
#define EXTRACE_MARK_URGENT(EV) EXTRACE_MARK_(EV, EXTRACE_HENC_URGENT)

#define EXTRACE_NONE_(EV, U)                                                          \
    do {                                                                              \
        if (EXTRACE_ON(EV)) {                                                         \
            EXTRACE_CHECK_KIND(EV, EXTRACE_K_NONE);                                   \
            extrace_emit_none(EXTRACE_HDR(EV, EXTRACE_K_NONE) | (U));                 \
        }                                                                             \
    } while (0)
#define EXTRACE(EV) EXTRACE_NONE_(EV, 0u)
#define EXTRACE_URGENT(EV) EXTRACE_NONE_(EV, EXTRACE_HENC_URGENT)

/* INTS: 1..16 integer arguments; the 64-bit array emitter if any argument is 64-bit. */
#define EXTRACE_INTS_NA(h, ...)                                                       \
    (EXTRACE_ANY64(__VA_ARGS__)                                                       \
         ? extrace_emit_ints64((h), EXTRACE_SIGN_MASK(__VA_ARGS__),                    \
                               (const uint64_t[]){ EXTRACE_AV64_LIST(__VA_ARGS__) },   \
                               EXTRACE_NARGS(__VA_ARGS__))                            \
         : extrace_emit_ints32((h), EXTRACE_SIGN_MASK(__VA_ARGS__),                    \
                               (const uint32_t[]){ EXTRACE_AV32_LIST(__VA_ARGS__) },   \
                               EXTRACE_NARGS(__VA_ARGS__)))
#define EXTRACE_INTS_DESC_(h, ...) ((h) | EXTRACE_SIGN_MASK(__VA_ARGS__) << EXTRACE_HENC_SM_SHIFT)
#define EXTRACE_INTS_N1(h, a0)                                                        \
    (EXTRACE_ANY64(a0) ? EXTRACE_INTS_NA(h, a0)                                       \
                       : extrace_emit_ints32_1(EXTRACE_INTS_DESC_(h, a0), EXTRACE_AV32(a0)))
#define EXTRACE_INTS_N2(h, a0, a1)                                                    \
    (EXTRACE_ANY64(a0, a1) ? EXTRACE_INTS_NA(h, a0, a1)                               \
                           : extrace_emit_ints32_2(EXTRACE_INTS_DESC_(h, a0, a1),     \
                                                   EXTRACE_AV32(a0), EXTRACE_AV32(a1)))
#define EXTRACE_INTS_N3(h, a0, a1, a2)                                                \
    (EXTRACE_ANY64(a0, a1, a2)                                                        \
         ? EXTRACE_INTS_NA(h, a0, a1, a2)                                             \
         : extrace_emit_ints32_3(EXTRACE_INTS_DESC_(h, a0, a1, a2), EXTRACE_AV32(a0), \
                                 EXTRACE_AV32(a1), EXTRACE_AV32(a2)))
#define EXTRACE_INTS_(EV, U, ...)                                                     \
    do {                                                                              \
        if (EXTRACE_ON(EV)) {                                                         \
            EXTRACE_CHECK_KIND(EV, EXTRACE_K_INTS);                                   \
            static_assert(EXTRACE_NARGS(__VA_ARGS__) <= EXTRACE_CFG_MAX_ARGS,         \
                          "too many arguments");                                      \
            static_assert(EXTRACE_ALL_INT(__VA_ARGS__), "EXTRACE_INTS takes integers"); \
            EXTRACE_CAT(EXTRACE_INTS_N, EXTRACE_SPEC_N(__VA_ARGS__))(                 \
                EXTRACE_HDR(EV, EXTRACE_K_INTS) | (U), __VA_ARGS__);                  \
        }                                                                             \
    } while (0)
#define EXTRACE_INTS(EV, ...) EXTRACE_INTS_(EV, 0u, __VA_ARGS__)
#define EXTRACE_INTS_URGENT(EV, ...) EXTRACE_INTS_(EV, EXTRACE_HENC_URGENT, __VA_ARGS__)

/*
 * PRINTF: deferred formatting through a site descriptor in the ELF (§10). Up to
 * three 32-bit integer arguments go in registers (§8.13 rule 4), anything else
 * through an array.
 */
#define EXTRACE_PRINTF_NA(h, site, ...)                                               \
    ((EXTRACE_NARGS(__VA_ARGS__) > 8 || EXTRACE_ANY64(__VA_ARGS__))                   \
         ? extrace_emit_printf64((h), (site), EXTRACE_SIG_LO(__VA_ARGS__),            \
                                 EXTRACE_SIG_HI(__VA_ARGS__),                         \
                                 (const uint64_t[]){ EXTRACE_AV64_LIST(__VA_ARGS__) }, \
                                 EXTRACE_NARGS(__VA_ARGS__))                          \
         : extrace_emit_printf32((h), (site), EXTRACE_SIG_LO(__VA_ARGS__),            \
                                 (const uint32_t[]){ EXTRACE_AV32_LIST(__VA_ARGS__) }, \
                                 EXTRACE_NARGS(__VA_ARGS__)))
#define EXTRACE_PRINTF_V32(...) (EXTRACE_ALL_INT(__VA_ARGS__) && !EXTRACE_ANY64(__VA_ARGS__))
#define EXTRACE_PRINTF_N1(h, site, a0)                                                \
    (EXTRACE_PRINTF_V32(a0)                                                           \
         ? extrace_emit_printf32_1(EXTRACE_INTS_DESC_(h, a0), (site), EXTRACE_AV32(a0)) \
         : EXTRACE_PRINTF_NA(h, site, a0))
#define EXTRACE_PRINTF_N2(h, site, a0, a1)                                            \
    (EXTRACE_PRINTF_V32(a0, a1)                                                       \
         ? extrace_emit_printf32_2(EXTRACE_INTS_DESC_(h, a0, a1), (site), EXTRACE_AV32(a0), \
                                   EXTRACE_AV32(a1))                                  \
         : EXTRACE_PRINTF_NA(h, site, a0, a1))
#define EXTRACE_PRINTF_N3(h, site, a0, a1, a2)                                        \
    (EXTRACE_PRINTF_V32(a0, a1, a2)                                                   \
         ? extrace_emit_printf32_3(EXTRACE_INTS_DESC_(h, a0, a1, a2), (site),         \
                                   EXTRACE_AV32(a0), EXTRACE_AV32(a1), EXTRACE_AV32(a2)) \
         : EXTRACE_PRINTF_NA(h, site, a0, a1, a2))
#if EXTRACE_CFG_EMIT_SPECIALIZE
#define EXTRACE_PRINTF_CALL(h, site, ...) extrace_emit_printf32_0((h), (site))
#else
#define EXTRACE_PRINTF_CALL(h, site, ...) extrace_emit_printf32((h), (site), 0u, nullptr, 0)
#endif
#define EXTRACE_PRINTF_CALLN(h, site, ...) \
    EXTRACE_CAT(EXTRACE_PRINTF_N, EXTRACE_SPEC_N(__VA_ARGS__))(h, site, __VA_ARGS__)
#define EXTRACE_PRINTF_DEFERRED_(EV, U, fmt_, ...)                                     \
    do {                                                                              \
        if (EXTRACE_ON(EV)) {                                                         \
            EXTRACE_CHECK_KIND(EV, EXTRACE_K_PRINTF);                                 \
            static_assert(0 __VA_OPT__(+EXTRACE_NARGS(__VA_ARGS__)) <= EXTRACE_CFG_MAX_ARGS, \
                          "too many arguments");                                      \
            static const char xt_fmt_[] __section("extrace_meta_str") = fmt_;          \
            static const char xt_file_[] __section("extrace_meta_str") = __FILE__;    \
            static const struct extrace_site xt_site_ __used                          \
                __section("extrace_meta_site") = {                                    \
                    .fmt = xt_fmt_,                                                   \
                    .file = xt_file_,                                                 \
                    .line = __LINE__,                                                 \
                    .event = EXTRACE_EV_##EV,                                         \
                    .nargs = 0 __VA_OPT__(+EXTRACE_NARGS(__VA_ARGS__)),               \
                    .sig_lo = 0u __VA_OPT__(| EXTRACE_SIG_LO(__VA_ARGS__)),           \
                    .sig_hi = 0u __VA_OPT__(| EXTRACE_SIG_HI(__VA_ARGS__)),           \
                };                                                                    \
            EXTRACE_CAT(EXTRACE_PRINTF_CALL, __VA_OPT__(N))(                          \
                EXTRACE_HDR(EV, EXTRACE_K_PRINTF) | (U),                              \
                extrace_site_id(&xt_site_, xt_fmt_) __VA_OPT__(, ) __VA_ARGS__);      \
        }                                                                             \
    } while (0)

#ifdef EXTRACE_PORT_HAS_VSNPRINTF
/* STR formatted on the device (§10.7). */
#define EXTRACE_TEXT_(EV, K, U, fmt, ...)                                             \
    do {                                                                              \
        if (EXTRACE_ON(EV)) {                                                         \
            EXTRACE_CHECK_KIND(EV, K);                                                \
            extrace_emit_text(EXTRACE_HDR(EV, EXTRACE_K_STR) | (U),                   \
                              fmt __VA_OPT__(, ) __VA_ARGS__);                        \
        }                                                                             \
    } while (0)
#define EXTRACE_TEXT(EV, fmt, ...) \
    EXTRACE_TEXT_(EV, EXTRACE_K_STR, 0u, fmt __VA_OPT__(, ) __VA_ARGS__)
#endif

#if EXTRACE_CFG_PRINTF_LOCAL
/* the event keeps its PRINTF declaration; the record is a formatted STR */
#define EXTRACE_PRINTF_(EV, U, fmt, ...) \
    EXTRACE_TEXT_(EV, EXTRACE_K_PRINTF, U, fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define EXTRACE_PRINTF_(EV, U, fmt, ...) \
    EXTRACE_PRINTF_DEFERRED_(EV, U, fmt __VA_OPT__(, ) __VA_ARGS__)
#endif
#define EXTRACE_PRINTF(EV, fmt, ...) EXTRACE_PRINTF_(EV, 0u, fmt __VA_OPT__(, ) __VA_ARGS__)
#define EXTRACE_PRINTF_URGENT(EV, fmt, ...) \
    EXTRACE_PRINTF_(EV, EXTRACE_HENC_URGENT, fmt __VA_OPT__(, ) __VA_ARGS__)

/* BUF, STRUCT, STR: payload copied with extrace_port_memcpy, truncated to MAX_PAYLOAD. */
#define EXTRACE_BUF_(EV, U, p, n)                                                     \
    do {                                                                              \
        if (EXTRACE_ON(EV)) {                                                         \
            EXTRACE_CHECK_KIND(EV, EXTRACE_K_BUF);                                    \
            extrace_emit_buf(EXTRACE_HDR(EV, EXTRACE_K_BUF) | (U), (p), (n));         \
        }                                                                             \
    } while (0)
#define EXTRACE_BUF(EV, p, n) EXTRACE_BUF_(EV, 0u, p, n)
#define EXTRACE_BUF_URGENT(EV, p, n) EXTRACE_BUF_(EV, EXTRACE_HENC_URGENT, p, n)

#define EXTRACE_BUF_LARGE(EV, p, n)                                                   \
    do {                                                                              \
        if (EXTRACE_ON(EV)) {                                                         \
            EXTRACE_CHECK_KIND(EV, EXTRACE_K_BUF);                                    \
            extrace_emit_buf_large(EXTRACE_EV_##EV, (p), (n), 0u);                    \
        }                                                                             \
    } while (0)

#define EXTRACE_STRUCT_TYPECHECK_(EV, ptr)                                            \
    static_assert(_Generic((ptr), extrace_type_##EV *: 1, const extrace_type_##EV *: 1, \
                      default: (int)EXTRACE_KIND_##EV == (int)EXTRACE_K_ANY),         \
                  "pointer type does not match the type declared for " #EV)
#define EXTRACE_STRUCT_N_(EV, U, ptr, n)                                              \
    do {                                                                              \
        if (EXTRACE_ON(EV)) {                                                         \
            EXTRACE_CHECK_KIND(EV, EXTRACE_K_STRUCT);                                 \
            EXTRACE_STRUCT_TYPECHECK_(EV, ptr);                                       \
            extrace_emit_buf(EXTRACE_HDR(EV, EXTRACE_K_STRUCT) | (U), (ptr), (n));    \
        }                                                                             \
    } while (0)
#define EXTRACE_STRUCT(EV, ptr) EXTRACE_STRUCT_N_(EV, 0u, ptr, sizeof(*(ptr)))
#define EXTRACE_STRUCT_URGENT(EV, ptr) \
    EXTRACE_STRUCT_N_(EV, EXTRACE_HENC_URGENT, ptr, sizeof(*(ptr)))
#define EXTRACE_STRUCT_N(EV, ptr, n) EXTRACE_STRUCT_N_(EV, 0u, ptr, n)

#define EXTRACE_STR_(EV, U, s)                                                        \
    do {                                                                              \
        if (EXTRACE_ON(EV)) {                                                         \
            EXTRACE_CHECK_KIND(EV, EXTRACE_K_STR);                                    \
            extrace_emit_str(EXTRACE_HDR(EV, EXTRACE_K_STR) | (U), (s));              \
        }                                                                             \
    } while (0)
#define EXTRACE_STR(EV, s) EXTRACE_STR_(EV, 0u, s)
#define EXTRACE_STR_URGENT(EV, s) EXTRACE_STR_(EV, EXTRACE_HENC_URGENT, s)

/* Direct reservation (§8.14). align: integer constant, 1 or a power of two. */
#define EXTRACE_RESV_KIND_(EV) \
    ((int)EXTRACE_KIND_##EV == (int)EXTRACE_K_ANY ? (int)EXTRACE_K_BUF : (int)EXTRACE_KIND_##EV)
#define EXTRACE_RESV_CHECK_(EV, align)                                                \
    ((void)sizeof(struct {                                                            \
        static_assert((int)EXTRACE_KIND_##EV == (int)EXTRACE_K_BUF ||                  \
                          (int)EXTRACE_KIND_##EV == (int)EXTRACE_K_STRUCT ||           \
                          (int)EXTRACE_KIND_##EV == (int)EXTRACE_K_STR ||              \
                          (int)EXTRACE_KIND_##EV == (int)EXTRACE_K_ANY,                \
                      "EXTRACE_RESERVE needs a BUF, STRUCT, STR or ANY event");        \
        static_assert((align) >= 1 && ((align) & ((align) - 1)) == 0 &&               \
                          (align) <= EXTRACE_CFG_RESV_ALIGN_MAX,                      \
                      "align must be a power of two <= EXTRACE_CFG_RESV_ALIGN_MAX");  \
        int x_;                                                                       \
    }))
#if EXTRACE_CFG_RESV_ALIGN_MAX > 1
#define EXTRACE_RESERVE_CALL_(h, len, align, r)                                       \
    ((align) > 1 ? extrace_reserve_aligned((h), (len), (align), (r))                  \
                 : extrace_reserve((h), (len), (r)))
#else
#define EXTRACE_RESERVE_CALL_(h, len, align, r) extrace_reserve((h), (len), (r))
#endif
#define EXTRACE_RESERVE(EV, len, align, r)                                            \
    (EXTRACE_RESV_CHECK_(EV, align),                                                  \
     EXTRACE_ON(EV) &&                                                                \
         EXTRACE_RESERVE_CALL_(EXTRACE_HENC(EXTRACE_EV_##EV, EXTRACE_RESV_KIND_(EV)), \
                               (len), (align), (r)))
#define EXTRACE_RESV_ALIGNOF_(T) \
    (alignof(T) < EXTRACE_CFG_RESV_ALIGN_MAX ? alignof(T) : EXTRACE_CFG_RESV_ALIGN_MAX)
#define EXTRACE_RESERVE_STRUCT(EV, r)                                                 \
    EXTRACE_RESERVE(EV, (uint32_t)sizeof(extrace_type_##EV),                          \
                    EXTRACE_RESV_ALIGNOF_(extrace_type_##EV), r)

#endif /* EXTRACE_PROD_H */
