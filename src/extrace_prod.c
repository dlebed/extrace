// SPDX-License-Identifier: MIT
/*
 * extrace_prod.c - producer: reservation, emitters, publication, lifecycle and
 * control polling (DESIGN §7, §8).
 *
 * Producer state is private. Shared memory is touched only by the record bytes,
 * by publish() and by the control paths, and only through stores of values kept
 * in local mirrors (§6.3 rule 8). Critical sections cover pointer arithmetic only.
 */
#include "extrace_prod.h"
#include "extrace_ftrace.h"

#ifdef EXTRACE_PORT_HAS_VSNPRINTF
#include <stdarg.h>
#endif

#if EXTRACE_CFG_TEST_HOOKS
#include "extrace_test_hooks.h"
#endif

EXTRACE_PORT_HOT_DATA struct extrace_prod extrace_producer;

/* ---- catalog-derived constants (§4.5, §10.5) ---- */

#define EXTRACE_GROUP(name, on)
#define EXTRACE_EV(name, kind, def, grp, spec) .ev_##name = EXTRACE_DEF_##def##_,
#define EXTRACE_EV_STRUCT(name, def, grp, type) .ev_##name = EXTRACE_DEF_##def##_,
const union extrace_ev_defaults extrace_ev_defaults = { .b = {
    .sys_ftrace = EXTRACE_CFG_FTRACE && EXTRACE_CFG_FTRACE_ON,
#include "extrace_events.def"
} };
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT

#define EXTRACE_EV(n_, k_, d_, g_, s_)                                            \
    static const char xt_n_##n_[] __section("extrace_meta_str") = #n_;          \
    static const char xt_g_##n_[] __section("extrace_meta_str") = #g_;          \
    static const char xt_s_##n_[] __section("extrace_meta_str") = s_;
#define EXTRACE_EV_STRUCT(n_, d_, g_, t_)                                         \
    static const char xt_n_##n_[] __section("extrace_meta_str") = #n_;          \
    static const char xt_g_##n_[] __section("extrace_meta_str") = #g_;          \
    static const char xt_t_##n_[] __section("extrace_meta_str") = #t_;
#include "extrace_events.def"
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT

#define EXTRACE_EV(n_, k_, d_, g_, s_)                                            \
    { .name = xt_n_##n_, .group = xt_g_##n_, .spec = xt_s_##n_,                \
      .id = EXTRACE_EV_##n_, .kind = EXTRACE_K_##k_,                            \
      .def_on = EXTRACE_DEF_##d_##_, .compiled_in = EXTRACE_CT_##n_ },
#define EXTRACE_EV_STRUCT(n_, d_, g_, t_)                                         \
    { .name = xt_n_##n_, .group = xt_g_##n_, .ctype = xt_t_##n_,               \
      .id = EXTRACE_EV_##n_, .kind = EXTRACE_K_STRUCT,                          \
      .def_on = EXTRACE_DEF_##d_##_, .compiled_in = EXTRACE_CT_##n_ },
static const struct extrace_meta_ev extrace_meta_events[] __used
    __section("extrace_meta_ev") = {
#include "extrace_events.def"
    { .name = nullptr },
};
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT
#undef EXTRACE_GROUP

static const struct extrace_meta_img extrace_meta_image __used __section("extrace_meta_img") = {
    .magic = EXTRACE_META_IMG_MAGIC,
    .layout_ver = EXTRACE_LAYOUT_VERSION,
    .wire_ver = EXTRACE_WIRE_VERSION,
    .site_id_bits = EXTRACE_CFG_SITE_ID_BITS,
    .ev_count = EXTRACE_EV_COUNT,
    .site_shift = EXTRACE_SITE_SHIFT,
    .max_payload = EXTRACE_CFG_MAX_PAYLOAD,
    .meta_mode = EXTRACE_CFG_META_MODE,
};

/* ---- reservation (§8.3, §8.4) ---- */

/* reserve() modes */
#define M_TS 0x1u     /* the record carries a delta timestamp */
#define M_ANCHOR 0x2u /* SYNC: the payload carries the absolute time, tsd is 0 */
#define M_HOLD 0x4u   /* write the record inside the critical section */

/*
 * The producer for the fast paths. Opaque, its address is computed once and kept
 * in a register: clang otherwise rebuilds it in every basic block that uses it.
 */
static __always_inline struct extrace_prod *prod(void)
{
    struct extrace_prod *p = &extrace_producer;

    OPTIMIZER_HIDE_VAR(p);
    return p;
}

struct rsv {
    struct extrace_prod *p; /* prod() of the reservation, reused by finish() */
    uint8_t *rec;         /* record start (offset 0 after a wrap) */
    uint8_t *pad_ptr;     /* wrap gap at the old ring end */
    uint32_t pad;         /* wrap gap bytes, 0 = no wrap */
    uint32_t k;           /* alignment PAD1 bytes */
    uint32_t ts, tsd;
    unsigned long flags;  /* M_HOLD: the saved interrupt state */
    bool hold;
};

static inline uint32_t lost_rec_len(uint32_t count, uint32_t ts, uint32_t bytes)
{
    /* hdr, tsd 0, len, sign mask, count, ts_abs [, bytes] */
    uint32_t n = 4u + extrace_uv_len32(count) + extrace_uv_len32(ts);

    if (EXTRACE_CFG_LOST_BYTES)
        n += extrace_uv_len32(bytes);
    return n;
}

static uint8_t *put_lost(uint8_t *restrict q, uint32_t count, uint32_t ts, uint32_t bytes)
{
    uint32_t plen = 1u + extrace_uv_len32(count) + extrace_uv_len32(ts);

    if (EXTRACE_CFG_LOST_BYTES)
        plen += extrace_uv_len32(bytes);
    q[0] = EXTRACE_HDR_LOST;
    q[1] = 0; /* tsd */
    q[2] = (uint8_t)plen;
    q[3] = 0; /* sign mask */
    q = extrace_put_uv32(q + 4, count);
    q = extrace_put_uv32(q, ts);
    if (EXTRACE_CFG_LOST_BYTES)
        q = extrace_put_uv32(q, bytes);
    return q;
}

/*
 * Size the record at the current head and run the fit test (§8.4). Called inside
 * the critical section with the timestamp, read when the record carries one or
 * a LOST prefix. need covers the LOST prefix, the record and alignment. fast:
 * fail rather than wrap.
 */
static __always_inline bool resv_fit(struct extrace_prod *p, uint32_t fixed, uint32_t plen,
                                     uint32_t mode, uint32_t align, uint32_t lost, uint32_t ts,
                                     bool fast, struct rsv *r, uint32_t *total)
{
    uint32_t tsd = 0, need = fixed, used, off, end_room, k = 0, pad = 0;

    if (lost)
        need += lost_rec_len(lost, ts, p->lost_bytes);
    else if ((mode & (M_TS | M_ANCHOR)) == M_TS)
        tsd = ts - p->last_ts;
    if (mode & M_TS)
        need += extrace_uv_len32(tsd);
    if (mode & M_ANCHOR)
        need += extrace_uv_len32(ts);

    used = p->head - p->tail_cache;
    off = p->head & p->idx_mask;
    end_room = p->size - off;
    if (align > 1)
        k = (0u - (off + need - plen)) & (align - 1u);
    if (unlikely(need + k > end_room)) { /* wrap */
        if (fast)
            return false;
        pad = end_room;
        off = 0;
        if (align > 1)
            k = (0u - (need - plen)) & (align - 1u);
    }
    r->ts = ts;
    r->tsd = tsd;
    r->pad = pad;
    r->k = k;
    r->pad_ptr = p->data + (p->head & p->idx_mask);
    r->rec = p->data + off;
    *total = pad + need + k;
    return used + pad + need + k <= p->size;
}

/* The timestamp for resv_fit(). */
static __always_inline uint32_t resv_ts(uint32_t mode, uint32_t lost)
{
    return (mode & M_TS) || lost ? extrace_port_ts() : 0u;
}

/* R1-R2 (§7.4): read the consumer tail of epoch ep, interrupts enabled. */
static __cold bool read_tail(const struct extrace_shm_cstate *cs, uint32_t ep, uint32_t *t)
{
    bool ok = false;

    if (extrace_shm_ld32(&cs->magic) == EXTRACE_SHM_MAGIC &&
        extrace_shm_ld32(&cs->attached_epoch) == ep) {
        extrace_port_rmb();
        *t = extrace_shm_ld32(&cs->tail);
        ok = true;
#if EXTRACE_CFG_TEST_HOOKS
        if (extrace_test_tail_hook)
            extrace_test_tail_hook(ep, *t);
#endif
    }
    extrace_port_mb(); /* the tail read completes before stores into freed space */
    return ok;
}

/* R3, inside the critical section: never move the cached tail backwards or past head. */
static inline void accept_tail(struct extrace_prod *p, uint32_t t)
{
    if (EXTRACE_CFG_STATS)
        p->stats.tail_refreshes++;
    if (t - p->tail_cache <= p->head - p->tail_cache)
        p->tail_cache = t;
    else if (EXTRACE_CFG_STATS)
        p->stats.tail_rejected++;
}

static void publish(void);

static __cold void publish_kick(void)
{
    if (EXTRACE_CFG_PUBLISH_IN_ISR || !extrace_port_in_isr())
        publish();
}

/* Drop accounting inside the critical section (§8.6); true at an episode start. */
static __cold bool drop(struct extrace_prod *p, uint32_t bytes)
{
    p->lost_total++;
    if (EXTRACE_CFG_LOST_BYTES)
        p->lost_bytes += bytes;
    if (p->lost_pending++ == 0) {
        p->notify_force = 1;
        return true;
    }
    return false;
}

static __cold void resv_fail_full(struct extrace_prod *p, unsigned long flags, uint32_t bytes)
{
    bool kick = drop(p, bytes);

    extrace_port_irq_restore(flags);
    if (kick)
        publish_kick();
}

/*
 * INTS and PRINTF payloads are bounded by MAX_ARGS, not by MAX_PAYLOAD. When a
 * small MAX_PAYLOAD cannot hold the worst case, an oversized record is dropped
 * and counted as lost; otherwise the check compiles away.
 */
#define V_PLEN_MAX (5u + 10u * EXTRACE_CFG_MAX_ARGS)

static __cold __noinline void drop_oversize(uint32_t bytes)
{
    struct extrace_prod *p = &extrace_producer;
    unsigned long flags = extrace_port_irq_save();

    if (p->flags & EXTRACE_PF_READY)
        resv_fail_full(p, flags, bytes);
    else
        extrace_port_irq_restore(flags);
}

static __always_inline bool oversize(uint32_t plen)
{
    if (V_PLEN_MAX <= EXTRACE_CFG_MAX_PAYLOAD || likely(plen <= EXTRACE_CFG_MAX_PAYLOAD))
        return false;
    drop_oversize(plen);
    return true;
}

/*
 * Ring full at the cached tail: refresh it with interrupts enabled (R1-R3) and
 * re-enter the section. Out of line, with no pointers into the caller's frame,
 * so the emitters keep their fast-path state in registers.
 */
static __cold __noinline unsigned long refresh_tail(struct extrace_prod *p, unsigned long flags)
{
    const struct extrace_shm_cstate *cs = p->cs; /* with ep: one snapshot */
    uint32_t ep = p->epoch, t;
    bool ok;

    extrace_port_irq_restore(flags);
    ok = read_tail(cs, ep, &t);
    flags = extrace_port_irq_save();
    if (ok && (p->flags & EXTRACE_PF_READY) && p->epoch == ep)
        accept_tail(p, t);
    return flags;
}

/*
 * Take the space sized by resv_fit() (§8.4), inside the critical section. lost:
 * the drops reported by the LOST prefix, 0 on the fast path.
 */
static __always_inline void resv_take(struct extrace_prod *p, uint32_t total, uint32_t mode,
                                      uint32_t lost, unsigned long flags, struct rsv *r)
{
    p->head += total;
    if (EXTRACE_CFG_STATS)
        p->stats.records++;
    if ((mode & M_TS) || lost)
        p->last_ts = r->ts;
    if (mode & M_ANCHOR)
        p->last_sync_ts = r->ts;
#if EXTRACE_CFG_TEST_HOOKS
    if (extrace_test_resv_hook)
        extrace_test_resv_hook(p->epoch, p->head - total + r->pad + r->k +
                                             (lost ? lost_rec_len(lost, r->ts, p->lost_bytes) : 0u));
#endif
    if (lost)
        p->lost_pending = 0;
    if (EXTRACE_CFG_LOST_BYTES)
        p->lost_bytes = 0;
    if (mode & M_HOLD) {
        r->hold = true;
        r->flags = flags;
        return;
    }
    r->hold = false;
    p->inflight++;
    if (EXTRACE_CFG_STATS && unlikely(p->inflight > p->stats.resv_open_max))
        p->stats.resv_open_max = p->inflight;
    extrace_port_irq_restore(flags);
}

/*
 * Every case of reserve(), the rare ones included: not ready, drops to report, a
 * wrap, ring full at the cached tail, too many open reservations. Entered inside
 * the critical section; fast: the fast path ran and read ts. On
 * success the wrap gap, the LOST record and the alignment bytes are written, and
 * r->rec is the first header byte.
 */
static __cold __noinline bool reserve_slow(struct extrace_prod *p, uint32_t fixed, uint32_t plen,
                                           uint32_t mode, uint32_t align, unsigned long flags,
                                           bool fast, uint32_t ts, struct rsv *r)
{
    uint32_t total, lost, lost_bytes;
    uint8_t *q;

    r->p = p;
    if (!(p->flags & EXTRACE_PF_READY)) {
        extrace_port_irq_restore(flags); /* init or panic: not counted */
        return false;
    }
    lost = p->lost_pending;
    if (!resv_fit(p, fixed, plen, mode, align, lost, fast ? ts : resv_ts(mode, lost), false, r,
                  &total)) {
        uint32_t ep = p->epoch;

        flags = refresh_tail(p, flags);
        if (!(p->flags & EXTRACE_PF_READY) || p->epoch != ep) {
            extrace_port_irq_restore(flags); /* re-initialized meanwhile: not counted */
            return false;
        }
        lost = p->lost_pending;
        if (!resv_fit(p, fixed, plen, mode, align, lost, resv_ts(mode, lost), false, r, &total)) {
            resv_fail_full(p, flags, fixed);
            return false;
        }
    }
    /* checked last: the slow path opens the section for other reservations */
    if (p->inflight == 255) {
        resv_fail_full(p, flags, fixed);
        return false;
    }
    lost_bytes = p->lost_bytes;
    resv_take(p, total, mode, lost, flags, r);
    if (r->pad)
        extrace_put_pad(r->pad_ptr, r->pad);
    q = r->rec;
    if (lost)
        q = put_lost(q, lost, r->ts, lost_bytes);
    for (uint32_t i = 0; i < r->k; i++)
        *q++ = EXTRACE_HDR_PAD1;
    r->rec = q;
    r->pad = 0;
    r->k = 0;
    return true;
}

/*
 * Reserve space for one record (§8.4). On success the record bytes are owned by
 * the caller; with M_HOLD the critical section stays open until finish(). The
 * common case is handled inline; anything unusual goes to reserve_slow().
 */
static __always_inline bool reserve(uint32_t fixed, uint32_t plen, uint32_t mode, uint32_t align,
                                    struct rsv *r)
{
    struct extrace_prod *p = prod();
    unsigned long flags;
    uint32_t total, ts = 0;
    struct rsv s;
    bool fast;

    r->p = p;
    flags = extrace_port_irq_save();
    fast = likely((p->flags & EXTRACE_PF_READY) && p->lost_pending == 0 && p->inflight != 255);
    if (fast) {
        ts = resv_ts(mode, 0);
        if (likely(resv_fit(p, fixed, plen, mode, align, 0, ts, true, r, &total))) {
            resv_take(p, total, mode, 0, flags, r);
            return true;
        }
    }
    /* through a copy, so that r stays in registers; the constants fold in finish() */
    if (!reserve_slow(p, fixed, plen, mode, align, flags, fast, ts, &s))
        return false;
    r->rec = s.rec;
    r->pad = 0;
    r->k = 0;
    r->ts = s.ts;
    r->tsd = s.tsd;
    r->flags = s.flags;
    r->hold = mode & M_HOLD;
    return true;
}

/* Wrap gap and alignment bytes; returns the first header byte. */
static __always_inline uint8_t *write_prefix(const struct rsv *r)
{
    uint8_t *q = r->rec;

    if (unlikely(r->pad))
        extrace_put_pad(r->pad_ptr, r->pad);
    for (uint32_t i = 0; i < r->k; i++)
        *q++ = EXTRACE_HDR_PAD1;
    return q;
}

static void publish_from_commit(bool urgent)
{
    if (!EXTRACE_CFG_PUBLISH_IN_ISR && !urgent && extrace_port_in_isr())
        return;
    publish();
}

/* Commit (§8.5): the record is complete. */
static __always_inline void finish(struct rsv *r, bool urgent)
{
    struct extrace_prod *p = r->p;
    unsigned long flags;
    uint32_t pend;

    if (r->hold) {
        /* written inside the reservation's critical section (§8.13 rule 7) */
        flags = r->flags;
        if (likely(p->inflight == 0))
            p->committed = p->head;
    } else {
        flags = extrace_port_irq_save();
        EXTRACE_ASSERT(p->inflight > 0);
        if (likely(--p->inflight == 0))
            p->committed = p->head;
    }
    pend = p->committed - p->published;
    extrace_port_irq_restore(flags);
    if (unlikely(urgent || pend >= p->publish_thr))
        publish_from_commit(urgent);
}

static __always_inline bool hold_small(uint32_t fixed)
{
    return EXTRACE_CFG_WRITE_IN_CS_MAX && fixed + 5u <= EXTRACE_CFG_WRITE_IN_CS_MAX;
}

/* ---- publication (§7.2, §8.8) ---- */

static void maybe_notify(struct extrace_prod *p, uint32_t to, bool force)
{
    if (force || (p->notify_thr && to - p->notified >= p->notify_thr)) {
        extrace_port_notify(p->ring_id); /* completion barrier inside (§7.2) */
        p->notified = to;
    }
}

static void store_head(struct extrace_prod *p, uint32_t to)
{
    uint32_t used = to - p->tail_cache;

    extrace_port_wmb();                    /* P2: data before the index */
    extrace_shm_st32(&p->ps->head, to);    /* P3 */
    if (unlikely(used > p->hwm) && used <= p->size) {
        p->hwm = used;
        extrace_shm_st32(&p->ps->hwm, used);
    }
    extrace_shm_st32(&p->ps->publish_cnt, ++p->publish_cnt);
}

/* Diagnostics: the drop count also changes while nothing new is published. */
static void store_lost(struct extrace_prod *p)
{
    uint32_t lt = p->lost_total;

    if (unlikely(lt != p->lost_total_st)) {
        p->lost_total_st = lt;
        extrace_shm_st32(&p->ps->lost_total, lt);
    }
}

static void publish(void)
{
    struct extrace_prod *p = &extrace_producer;
    unsigned long flags;
    uint32_t from, to;
    bool force;

    flags = extrace_port_irq_save();
    if (unlikely(!(p->flags & EXTRACE_PF_READY))) {
        extrace_port_irq_restore(flags);
        return;
    }
    if (unlikely(p->publishing)) {
        p->publish_again = 1;
        extrace_port_irq_restore(flags);
        return;
    }
    p->publishing = 1;
    from = p->published;
    to = p->committed;
    force = p->notify_force;
    p->notify_force = 0;
    extrace_port_irq_restore(flags);
    for (;;) {
        if (likely(from != to))
            store_head(p, to);
        store_lost(p);
        if (from != to || force)
            maybe_notify(p, to, force);
        flags = extrace_port_irq_save();
        p->published = to;
        if (unlikely(p->publish_again) && (p->committed != p->published || p->notify_force)) {
            p->publish_again = 0;
            from = p->published;
            to = p->committed;
            force = p->notify_force;
            p->notify_force = 0;
            extrace_port_irq_restore(flags);
            continue;
        }
        p->publishing = 0;
        p->publish_again = 0;
        p->stats.publishes++;
        extrace_port_irq_restore(flags);
        return;
    }
}

void extrace_prod_flush(void)
{
    publish();
}

/* ---- emitters (§8.13) ---- */

static __always_inline uint8_t *put_h(uint8_t *restrict q, uint32_t henc, bool sys)
{
    if (sys) {
        q[0] = (uint8_t)henc;
        return q + 1;
    }
    return extrace_put_hdr(q, henc);
}

static __always_inline uint32_t hdr_len(uint32_t henc, bool sys)
{
    return sys ? 1u : extrace_henc_len(henc);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_mark(uint32_t henc)
{
    struct rsv r;
    uint8_t *q;

    if (unlikely(!reserve(extrace_henc_len(henc), 0, M_HOLD, 1, &r)))
        return;
    q = write_prefix(&r);
    extrace_put_hdr(q, henc);
    finish(&r, henc & EXTRACE_HENC_URGENT);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_none(uint32_t henc)
{
    struct rsv r;
    uint8_t *q;

    if (unlikely(!reserve(extrace_henc_len(henc), 0, M_TS | M_HOLD, 1, &r)))
        return;
    q = write_prefix(&r);
    q = extrace_put_hdr(q, henc);
    extrace_put_uv32_any(q, r.tsd);
    finish(&r, henc & EXTRACE_HENC_URGENT);
}

static __always_inline uint32_t ints_enc32(const uint32_t *v, uint32_t i, uint32_t sm)
{
    return ((sm >> i) & 1u) ? extrace_zz32((int32_t)v[i]) : v[i];
}

/*
 * INTS record, or a PRINTF record whose arguments are all 32-bit integers: the
 * same layout with the site ID in place of the sign mask as the first field.
 * nl leading values: an INTS record starts with the unsigned values l0 (nl >= 1)
 * and l1 (nl == 2), then v[]; sm is 0.
 */
static __always_inline void emit_v32_core(uint32_t henc, uint32_t sm, uint32_t site,
                                          bool is_printf, const uint32_t *v, uint32_t n,
                                          uint32_t nl, uint32_t l0, uint32_t l1, bool sys,
                                          bool force_hold)
{
    uint32_t first = is_printf ? site : sm, acc = is_printf ? 0u : sm, plen, fixed, mode;
    bool small;
    struct rsv r;
    uint8_t *q;

    if (nl > 0)
        acc |= l0;
    if (nl > 1)
        acc |= l1;
    for (uint32_t i = 0; i < n; i++)
        acc |= ints_enc32(v, i, sm);
    small = !(acc >> 7); /* every value is one byte (§8.13 rule 3) */
    if (likely(small)) {
        plen = n + nl + (is_printf ? extrace_uv_len32(first) : 1u);
    } else {
        plen = extrace_uv_len32(first);
        if (nl > 0)
            plen += extrace_uv_len32(l0);
        if (nl > 1)
            plen += extrace_uv_len32(l1);
        for (uint32_t i = 0; i < n; i++)
            plen += extrace_uv_len32(ints_enc32(v, i, sm));
        if (oversize(plen))
            return;
    }
    fixed = hdr_len(henc, sys) + extrace_uv_len32(plen) + plen;
    mode = M_TS | ((force_hold || hold_small(fixed)) ? M_HOLD : 0u);
    if (unlikely(!reserve(fixed, plen, mode, 1, &r)))
        return;
    q = write_prefix(&r);
    q = put_h(q, henc, sys);
    q = extrace_put_uv32_any(q, r.tsd);
    q = extrace_put_uv32(q, plen);
    if (is_printf)
        q = extrace_put_uv32(q, first);
    if (likely(small)) {
        if (!is_printf)
            *q++ = (uint8_t)sm;
        if (nl > 0)
            *q++ = (uint8_t)l0;
        if (nl > 1)
            *q++ = (uint8_t)l1;
        for (uint32_t i = 0; i < n; i++)
            *q++ = (uint8_t)ints_enc32(v, i, sm);
    } else {
        if (!is_printf)
            q = extrace_put_uv32(q, sm);
        if (nl > 0)
            q = extrace_put_uv32(q, l0);
        if (nl > 1)
            q = extrace_put_uv32(q, l1);
        for (uint32_t i = 0; i < n; i++)
            q = extrace_put_uv32(q, ints_enc32(v, i, sm));
    }
    finish(&r, henc & EXTRACE_HENC_URGENT);
}

static __always_inline void emit_ints32_core(uint32_t henc, uint32_t sm, const uint32_t *v,
                                             uint32_t n, bool sys, bool force_hold)
{
    emit_v32_core(henc, sm, 0, false, v, n, 0, 0, 0, sys, force_hold);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_ints32(uint32_t henc, uint32_t sign_mask,
                                                           const uint32_t *v, uint32_t n)
{
    emit_ints32_core(henc, sign_mask, v, n, false, false);
}

#define DESC_HENC(d) ((d) & ~(7u << EXTRACE_HENC_SM_SHIFT))
#define DESC_SM(d) (((d) >> EXTRACE_HENC_SM_SHIFT) & 7u)

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_ints32_1(uint32_t desc, uint32_t a0)
{
    const uint32_t v[1] = { a0 };

    emit_ints32_core(DESC_HENC(desc), DESC_SM(desc), v, 1, false, false);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_ints32_2(uint32_t desc, uint32_t a0, uint32_t a1)
{
    const uint32_t v[2] = { a0, a1 };

    emit_ints32_core(DESC_HENC(desc), DESC_SM(desc), v, 2, false, false);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_ints32_3(uint32_t desc, uint32_t a0, uint32_t a1, uint32_t a2)
{
    const uint32_t v[3] = { a0, a1, a2 };

    emit_ints32_core(DESC_HENC(desc), DESC_SM(desc), v, 3, false, false);
}

#if EXTRACE_CFG_FTRACE
/* ---- function entry tracing (docs/ftrace.md) ---- */

static_assert(offsetof(struct extrace_prod, mask) == 0 && EXTRACE_SYS_FTRACE < 32,
              "the trampolines test bit FTRACE of the first word of extrace_producer");
static_assert(EXTRACE_FTRACE_ARG_REGS >= 1 && EXTRACE_FTRACE_ARG_REGS + 2 <= 16,
              "FTRACE_CALLER ARGS: address, caller, arguments in one INTS record");
static_assert(EXTRACE_FTRACE_ARGS(EXTRACE_FTRACE_ARG_REGS) < EXTRACE_FTRACE_CALLER,
              "the CALLER bit is above every shape");

/*
 * MARK and NONE: header, [timestamp,] address [, caller]; no length field
 * (§5.9.7). The caller shapes are those of system event FTRACE_CALLER.
 */
static __always_inline void emit_ftrace_short(uint32_t a, uint32_t c, bool ts, bool caller)
{
    uint32_t fixed = 1u + extrace_uv_len32(a) + (caller ? extrace_uv_len32(c) : 0u);
    struct rsv r;
    uint8_t *q;

    if (!EXTRACE_CFG_ENABLE || unlikely(!reserve(fixed, 0, (ts ? M_TS : 0u) | M_HOLD, 1, &r)))
        return;
    q = write_prefix(&r);
    if (caller)
        *q++ = ts ? EXTRACE_HDR_FTRACE_CALLER_NONE : EXTRACE_HDR_FTRACE_CALLER_MARK;
    else
        *q++ = ts ? EXTRACE_HDR_FTRACE_NONE : EXTRACE_HDR_FTRACE_MARK;
    if (ts)
        q = extrace_put_uv32_any(q, r.tsd);
    q = extrace_put_uv32(q, a);
    if (caller)
        extrace_put_uv32(q, c);
    finish(&r, 0);
}

#define FT_A(ret) ((uint32_t)((ret) - EXTRACE_CFG_FTRACE_BASE))

/* The trampoline's targets: the switch is tested, ret and caller as delivered. */
void EXTRACE_PORT_HOT_TEXT __noinline extrace_ftrace_mark(uintptr_t ret)
{
    emit_ftrace_short(FT_A(ret), 0, false, false);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_ftrace_none(uintptr_t ret)
{
    emit_ftrace_short(FT_A(ret), 0, true, false);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_ftrace_args(uintptr_t ret, const uint32_t *args,
                                                          unsigned int n)
{
    if (EXTRACE_CFG_ENABLE)
        emit_v32_core(EXTRACE_HDR_FTRACE_INTS, 0, 0, false, args, n, 1, FT_A(ret), 0, true,
                      false);
}

#if EXTRACE_CFG_FTRACE_CALLER
/* The CALLER flavors: FTRACE_CALLER records. */
void EXTRACE_PORT_HOT_TEXT __noinline extrace_ftrace_mark_c(uintptr_t ret, uintptr_t caller)
{
    emit_ftrace_short(FT_A(ret), FT_A(caller), false, true);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_ftrace_none_c(uintptr_t ret, uintptr_t caller)
{
    emit_ftrace_short(FT_A(ret), FT_A(caller), true, true);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_ftrace_args_c(uintptr_t ret, uintptr_t caller,
                                                            const uint32_t *args, unsigned int n)
{
    if (EXTRACE_CFG_ENABLE)
        emit_v32_core(EXTRACE_HDR_FTRACE_CALLER_INTS, 0, 0, false, args, n, 2, FT_A(ret),
                      FT_A(caller), true, false);
}
#else
/* Not built: the trampoline still links them; the patcher refuses their flavors. */
void extrace_ftrace_mark_c(uintptr_t ret, uintptr_t caller)
{
    (void)ret, (void)caller;
}

void extrace_ftrace_none_c(uintptr_t ret, uintptr_t caller)
{
    (void)ret, (void)caller;
}

void extrace_ftrace_args_c(uintptr_t ret, uintptr_t caller, const uint32_t *args, unsigned int n)
{
    (void)ret, (void)caller, (void)args, (void)n;
}
#endif

/* From C: the switch and the flavor are checked here. */
void extrace_ftrace_hit(uintptr_t ret, uintptr_t caller, const uint32_t *args, unsigned int f)
{
    unsigned int s = f & ~(unsigned int)EXTRACE_FTRACE_CALLER, n = s - EXTRACE_FTRACE_NONE;
    bool c = f & EXTRACE_FTRACE_CALLER;

    if (!EXTRACE_CFG_ENABLE || !(extrace_producer.mask[0] & (1u << EXTRACE_SYS_FTRACE)) ||
        extrace_ftrace_entry_index(f) < 0 || (c && !EXTRACE_CFG_FTRACE_CALLER))
        return;
    if (s == EXTRACE_FTRACE_MARK)
        c ? extrace_ftrace_mark_c(ret, caller) : extrace_ftrace_mark(ret);
    else if (s == EXTRACE_FTRACE_NONE)
        c ? extrace_ftrace_none_c(ret, caller) : extrace_ftrace_none(ret);
    else
        c ? extrace_ftrace_args_c(ret, caller, args, n) : extrace_ftrace_args(ret, args, n);
}
#endif

static __always_inline uint64_t ints_enc64(const uint64_t *v, uint32_t i, uint32_t sm)
{
    return ((sm >> i) & 1u) ? extrace_zz64((int64_t)v[i]) : v[i];
}

static __always_inline uint32_t uv_len64(uint64_t v)
{
    return extrace_uv_len64((uint32_t)v, (uint32_t)(v >> 32));
}

static __always_inline uint8_t *put_uv64(uint8_t *restrict q, uint64_t v)
{
    return extrace_put_uv64(q, (uint32_t)v, (uint32_t)(v >> 32));
}

static void emit_ints64_core(uint32_t henc, uint32_t sm, const uint64_t *v, uint32_t n, bool sys,
                             bool force_hold)
{
    uint32_t plen = extrace_uv_len32(sm), fixed, mode;
    struct rsv r;
    uint8_t *q;

    for (uint32_t i = 0; i < n; i++)
        plen += uv_len64(ints_enc64(v, i, sm));
    if (oversize(plen))
        return;
    fixed = hdr_len(henc, sys) + extrace_uv_len32(plen) + plen;
    mode = M_TS | ((force_hold || hold_small(fixed)) ? M_HOLD : 0u);
    if (unlikely(!reserve(fixed, plen, mode, 1, &r)))
        return;
    q = write_prefix(&r);
    q = put_h(q, henc, sys);
    q = extrace_put_uv32_any(q, r.tsd);
    q = extrace_put_uv32(q, plen);
    q = extrace_put_uv32(q, sm);
    for (uint32_t i = 0; i < n; i++)
        q = put_uv64(q, ints_enc64(v, i, sm));
    finish(&r, henc & EXTRACE_HENC_URGENT);
}

void __noinline extrace_emit_ints64(uint32_t henc, uint32_t sign_mask, const uint64_t *v,
                                    uint32_t n)
{
    emit_ints64_core(henc, sign_mask, v, n, false, false);
}

/* Bounded strlen without a C library dependency. */
static uint32_t bounded_strlen(const char *s, uint32_t max)
{
    uint32_t n = 0;

    if (unlikely(!s))
        return 0;
    while (n < max && s[n])
        n++;
    return n;
}

/*
 * PRINTF arguments (§5.7.2): v holds 64-bit values; classes come from the
 * signature. String lengths are measured once and kept, so the record matches
 * its reservation even if a string changes while it is written.
 */
static uint32_t printf_class(uint32_t sig_lo, uint32_t sig_hi, uint32_t i)
{
    return ((i < 8 ? sig_lo : sig_hi) >> (4u * (i & 7u))) & 15u;
}

static uint32_t printf_arg_len(uint32_t cls, uint64_t v, uint32_t slen)
{
    switch (cls) {
    case EXTRACE_AC_S32:
        return extrace_uv_len32(extrace_zz32((int32_t)v));
    case EXTRACE_AC_S64:
        return uv_len64(extrace_zz64((int64_t)v));
    case EXTRACE_AC_F32:
        return 4;
    case EXTRACE_AC_F64:
        return 8;
    case EXTRACE_AC_STR:
        return extrace_uv_len32(slen) + slen;
    default: /* U32, U64, PTR, CSTR */
        return uv_len64(v);
    }
}

static uint8_t *printf_arg_put(uint8_t *restrict q, uint32_t cls, uint64_t v, uint32_t slen)
{
    switch (cls) {
    case EXTRACE_AC_S32:
        return extrace_put_uv32(q, extrace_zz32((int32_t)v));
    case EXTRACE_AC_S64:
        return put_uv64(q, extrace_zz64((int64_t)v));
    case EXTRACE_AC_F32:
        extrace_le32_bytes(q, (uint32_t)v);
        return q + 4;
    case EXTRACE_AC_F64:
        extrace_le32_bytes(q, (uint32_t)v);
        extrace_le32_bytes(q + 4, (uint32_t)(v >> 32));
        return q + 8;
    case EXTRACE_AC_STR:
        q = extrace_put_uv32(q, slen);
        if (slen)
            extrace_port_memcpy(q, (const char *)(uintptr_t)v, slen);
        return q + slen;
    default:
        return put_uv64(q, v);
    }
}

static void emit_printf_core(uint32_t henc, uint32_t site, uint32_t sig_lo, uint32_t sig_hi,
                             const uint32_t *v32, const uint64_t *v64, uint32_t n)
{
    uint16_t slen[16];
    uint32_t plen = extrace_uv_len32(site), fixed;
    struct rsv r;
    uint8_t *q;

    if (unlikely(n > 16))
        n = 16;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cls = printf_class(sig_lo, sig_hi, i);
        uint64_t v = v64 ? v64[i] : v32[i];

        slen[i] = 0;
        if (cls == EXTRACE_AC_STR)
            slen[i] = (uint16_t)bounded_strlen((const char *)(uintptr_t)v,
                                               EXTRACE_CFG_STR_ARG_MAX);
        plen += printf_arg_len(cls, v, slen[i]);
    }
    /* Strings are the only unbounded part: shorten them from the last one. */
    for (uint32_t i = n; unlikely(plen > EXTRACE_CFG_MAX_PAYLOAD) && i-- > 0;) {
        uint32_t cls = printf_class(sig_lo, sig_hi, i);

        if (cls == EXTRACE_AC_STR && slen[i]) {
            uint32_t old = extrace_uv_len32(slen[i]) + slen[i];
            uint32_t cut = plen - EXTRACE_CFG_MAX_PAYLOAD;

            slen[i] = (uint16_t)(cut >= slen[i] ? 0u : slen[i] - cut);
            plen = plen - old + extrace_uv_len32(slen[i]) + slen[i];
        }
    }
    if (oversize(plen))
        return;
    fixed = extrace_henc_len(henc) + extrace_uv_len32(plen) + plen;
    if (unlikely(!reserve(fixed, plen, M_TS | (hold_small(fixed) ? M_HOLD : 0u), 1, &r)))
        return;
    q = write_prefix(&r);
    q = extrace_put_hdr(q, henc);
    q = extrace_put_uv32_any(q, r.tsd);
    q = extrace_put_uv32(q, plen);
    q = extrace_put_uv32(q, site);
    for (uint32_t i = 0; i < n; i++)
        q = printf_arg_put(q, printf_class(sig_lo, sig_hi, i), v64 ? v64[i] : v32[i], slen[i]);
    finish(&r, henc & EXTRACE_HENC_URGENT);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_printf32(uint32_t henc, uint32_t site,
                                                            uint32_t sig_lo, const uint32_t *v,
                                                            uint32_t n)
{
    emit_printf_core(henc, site, sig_lo, 0, v, nullptr, n);
}

void __noinline extrace_emit_printf64(uint32_t henc, uint32_t site, uint32_t sig_lo,
                                      uint32_t sig_hi, const uint64_t *v, uint32_t n)
{
    emit_printf_core(henc, site, sig_lo, sig_hi, nullptr, v, n);
}

/* PRINTF with 0-3 32-bit integer arguments in registers (§8.13 rule 4). */
void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_printf32_0(uint32_t henc, uint32_t site)
{
    emit_v32_core(henc, 0, site, true, nullptr, 0, 0, 0, 0, false, false);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_printf32_1(uint32_t desc, uint32_t site,
                                                              uint32_t a0)
{
    const uint32_t v[1] = { a0 };

    emit_v32_core(DESC_HENC(desc), DESC_SM(desc), site, true, v, 1, 0, 0, 0, false, false);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_printf32_2(uint32_t desc, uint32_t site,
                                                              uint32_t a0, uint32_t a1)
{
    const uint32_t v[2] = { a0, a1 };

    emit_v32_core(DESC_HENC(desc), DESC_SM(desc), site, true, v, 2, 0, 0, 0, false, false);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_printf32_3(uint32_t desc, uint32_t site,
                                                              uint32_t a0, uint32_t a1, uint32_t a2)
{
    const uint32_t v[3] = { a0, a1, a2 };

    emit_v32_core(DESC_HENC(desc), DESC_SM(desc), site, true, v, 3, 0, 0, 0, false, false);
}

static __always_inline void emit_buf_core(uint32_t henc, const void *src, uint32_t n, bool sys,
                                          bool force_hold)
{
    uint32_t fixed, mode;
    struct rsv r;
    uint8_t *q;

    fixed = hdr_len(henc, sys) + extrace_uv_len32(n) + n;
    mode = M_TS | ((force_hold || hold_small(fixed)) ? M_HOLD : 0u);
    if (unlikely(!reserve(fixed, n, mode, 1, &r)))
        return;
    q = write_prefix(&r);
    q = put_h(q, henc, sys);
    q = extrace_put_uv32_any(q, r.tsd);
    q = extrace_put_uv32(q, n);
    if (likely(n))
        extrace_port_memcpy(q, src, n);
    finish(&r, henc & EXTRACE_HENC_URGENT);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_buf(uint32_t henc, const void *p, size_t n)
{
    /* truncated; len tells the host (§8.6) */
    emit_buf_core(henc, p, n > EXTRACE_CFG_MAX_PAYLOAD ? EXTRACE_CFG_MAX_PAYLOAD : (uint32_t)n,
                  false, false);
}

void EXTRACE_PORT_HOT_TEXT __noinline extrace_emit_str(uint32_t henc, const char *s)
{
    emit_buf_core(henc, s, bounded_strlen(s, EXTRACE_CFG_MAX_PAYLOAD), false, false);
}

/* FRAG sequence for buffers larger than MAX_PAYLOAD (§5.9.6). */
void __noinline extrace_emit_buf_large(uint32_t id, const void *src, size_t n, uint32_t urgent)
{
    struct extrace_prod *p = &extrace_producer;
    const uint8_t *b = src;
    uint32_t total = n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n, off = 0, xfer;
    unsigned long flags;

    if (total <= EXTRACE_CFG_MAX_PAYLOAD) {
        emit_buf_core(extrace_henc(id, EXTRACE_K_BUF) | (urgent ? EXTRACE_HENC_URGENT : 0u), src,
                      total, false, false);
        return;
    }
    flags = extrace_port_irq_save();
    xfer = ++p->frag_seq;
    extrace_port_irq_restore(flags);
    while (off < total) {
        uint32_t hdrb = 1u + extrace_uv_len32(xfer) + extrace_uv_len32(id) +
                        extrace_uv_len32(total) + extrace_uv_len32(off);
        uint32_t chunk = total - off, plen, fixed;
        struct rsv r;
        uint8_t *q;

        if (chunk > EXTRACE_CFG_MAX_PAYLOAD - hdrb)
            chunk = EXTRACE_CFG_MAX_PAYLOAD - hdrb;
        plen = hdrb + chunk;
        fixed = 1u + extrace_uv_len32(plen) + plen;
        if (unlikely(!(p->flags & EXTRACE_PF_READY)))
            return; /* init or panic: the rest would be dropped too */
        if (likely(reserve(fixed, plen, M_TS, 1, &r))) {
            q = write_prefix(&r);
            *q++ = EXTRACE_HDR_FRAG;
            q = extrace_put_uv32_any(q, r.tsd);
            q = extrace_put_uv32(q, plen);
            *q++ = EXTRACE_EXT_FRAG;
            q = extrace_put_uv32(q, xfer);
            q = extrace_put_uv32(q, id);
            q = extrace_put_uv32(q, total);
            q = extrace_put_uv32(q, off);
            extrace_port_memcpy(q, b + off, chunk);
            finish(&r, urgent && off + chunk >= total);
        }
        off += chunk;
    }
}

#ifdef EXTRACE_PORT_HAS_VSNPRINTF
void __noinline extrace_emit_text(uint32_t henc, const char *fmt, ...)
{
    /* the formatter truncates; the text never exceeds MAX_PAYLOAD */
    char buf[EXTRACE_CFG_TEXT_MAX <= EXTRACE_CFG_MAX_PAYLOAD ? EXTRACE_CFG_TEXT_MAX
                                                             : EXTRACE_CFG_MAX_PAYLOAD + 1u];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = extrace_port_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (unlikely(n < 0))
        return;
    if (unlikely((uint32_t)n >= sizeof(buf)))
        n = (int)sizeof(buf) - 1;
    emit_buf_core(henc, buf, (uint32_t)n, false, false);
}
#endif

/* ---- runtime-ID variants (§8.11) ---- */

static inline bool dyn_on(uint16_t id)
{
    return EXTRACE_CFG_ENABLE && likely(id >= EXTRACE_SYS_FIRST_USER && id < EXTRACE_EV_COUNT) &&
           extrace_ev_on(id);
}

bool extrace_emit_dyn_none(uint16_t id)
{
    if (!dyn_on(id))
        return false;
    extrace_emit_none(extrace_henc(id, EXTRACE_K_NONE));
    return true;
}

bool extrace_emit_dyn_ints(uint16_t id, const uint32_t *v, unsigned int n, uint32_t sign_mask)
{
    if (!dyn_on(id) || unlikely(n < 1 || n > EXTRACE_CFG_MAX_ARGS))
        return false;
    extrace_emit_ints32(extrace_henc(id, EXTRACE_K_INTS), sign_mask, v, n);
    return true;
}

bool extrace_emit_dyn_ints64(uint16_t id, const uint64_t *v, unsigned int n, uint32_t sign_mask)
{
    if (!dyn_on(id) || unlikely(n < 1 || n > EXTRACE_CFG_MAX_ARGS))
        return false;
    extrace_emit_ints64(extrace_henc(id, EXTRACE_K_INTS), sign_mask, v, n);
    return true;
}

bool extrace_emit_dyn_buf(uint16_t id, uint8_t kind, const void *p, uint32_t n)
{
    if (unlikely(kind != EXTRACE_K_BUF && kind != EXTRACE_K_STRUCT && kind != EXTRACE_K_STR))
        return false;
    if (!dyn_on(id))
        return false;
    extrace_emit_buf(extrace_henc(id, kind), p, n);
    return true;
}

/* ---- direct reservation (§8.14) ---- */

static __always_inline bool reserve_direct(uint32_t henc, uint32_t len, uint32_t align,
                                           struct extrace_resv *out)
{
    struct rsv r;
    uint32_t fixed;
    uint8_t *q, *h;

    if (unlikely(len > EXTRACE_CFG_MAX_PAYLOAD))
        return false;
    henc &= ~EXTRACE_HENC_FLAGS_MASK;
    fixed = extrace_henc_len(henc) + extrace_uv_len32(len) + len;
    if (unlikely(!reserve(fixed, len, M_TS, align, &r)))
        return false;
    h = write_prefix(&r);
    q = extrace_put_hdr(h, henc);
    q = extrace_put_uv32_any(q, r.tsd);
    q = extrace_put_uv32(q, len);
    out->payload = q;
    out->len = len;
    out->rec = h;
    out->hdr_len = (uint8_t)extrace_henc_len(henc);
    return true;
}

bool __noinline extrace_reserve(uint32_t henc, uint32_t len, struct extrace_resv *r)
{
    return reserve_direct(henc, len, 1, r);
}

#if EXTRACE_CFG_RESV_ALIGN_MAX > 1
bool __noinline extrace_reserve_aligned(uint32_t henc, uint32_t len, uint32_t align,
                                        struct extrace_resv *r)
{
    if (unlikely(align < 2 || align > EXTRACE_CFG_RESV_ALIGN_MAX || (align & (align - 1u))))
        return false;
    return reserve_direct(henc, len, align, r);
}
#endif

static void commit_resv(bool urgent)
{
    struct rsv r = { .p = prod(), .hold = false };

    finish(&r, urgent);
}

void extrace_commit(struct extrace_resv *r)
{
    (void)r;
    commit_resv(false);
}

void extrace_commit_urgent(struct extrace_resv *r)
{
    (void)r;
    commit_resv(true);
}

/*
 * The header becomes PAD1 bytes and a 1-byte SKIP header; tsd, len and payload
 * stay. SKIP keeps the tsd on the timeline, which later records' deltas build on.
 * Stores only (§8.8.1).
 */
void extrace_cancel(struct extrace_resv *r)
{
    for (unsigned int i = 0; i + 1u < r->hdr_len; i++)
        r->rec[i] = EXTRACE_HDR_PAD1;
    r->rec[r->hdr_len - 1u] = EXTRACE_HDR_SKIP;
    commit_resv(false);
}

/* ---- system records ---- */

static void emit_sync(bool hold)
{
    struct rsv r;
    uint8_t *q;

    /* hdr, len, sign mask + tsd 0 + ts_abs; the payload size depends on ts */
    if (unlikely(!reserve(3u, 0, M_TS | M_ANCHOR | (hold ? M_HOLD : 0u), 1, &r)))
        return;
    q = write_prefix(&r);
    q[0] = EXTRACE_HDR_SYNC;
    q[1] = 0; /* tsd */
    q[2] = (uint8_t)(1u + extrace_uv_len32(r.ts));
    q[3] = 0; /* sign mask */
    extrace_put_uv32(q + 4, r.ts);
    finish(&r, false);
}

static void emit_info(bool hold)
{
    const struct extrace_prod *p = &extrace_producer;
    uint8_t b[64], *q = b; /* 63 at most */
    const uint8_t *bid;
    size_t bid_len;
    uint32_t name_len = 0;
    char name[8];

    bid = extrace_port_build_id(&bid_len);
    if (!bid || bid_len > 20)
        bid_len = bid ? 20 : 0;
    extrace_port_memcpy(name, p->info_name, sizeof(name));
    while (name_len < 8 && name[name_len])
        name_len++;
    q = extrace_put_uv32(q, 13);                          /* nfields */
    q = extrace_put_uv32(q, EXTRACE_WIRE_VERSION);
    q = extrace_put_uv32(q, EXTRACE_LAYOUT_VERSION);
    q = extrace_put_uv32(q, (EXTRACE_BIG_ENDIAN ? EXTRACE_PI_BIG_ENDIAN : 0u) | EXTRACE_PI_TS_GLOBAL);
    q = extrace_put_uv32(q, extrace_port_ts_freq());
    q = extrace_put_uv32(q, p->ring_id);
    q = extrace_put_uv32(q, p->epoch);
    q = extrace_put_uv32(q, p->size);
    q = extrace_put_uv32(q, EXTRACE_EV_COUNT);
    q = extrace_put_uv32(q, (uint32_t)sizeof(void *));
    q = extrace_put_uv32(q, EXTRACE_CFG_SITE_ID_BITS);
    q = extrace_put_uv32(q, (uint32_t)bid_len);
    if (bid_len)
        extrace_port_memcpy(q, bid, bid_len);
    q += bid_len;
    q = extrace_put_uv32(q, name_len);
    extrace_port_memcpy(q, name, name_len);
    q += name_len;
    q = extrace_put_uv32(q, (uint32_t)EXTRACE_CFG_FTRACE_BASE);
    emit_buf_core(EXTRACE_HDR_INFO, b, (uint32_t)(q - b), true, hold);
}

static void emit_cfg(uint32_t gen)
{
    emit_ints32_core(EXTRACE_HDR_CFG, 0, &gen, 1, true, false);
}

/* ---- lifecycle (§7.6) ---- */

static uint32_t index_base(void)
{
#if EXTRACE_CFG_TEST_HOOKS
    return extrace_test_index_base;
#else
    return 0;
#endif
}

static uint32_t pow2_floor(uint32_t v)
{
    uint32_t r = 1;

    while (r <= v / 2u)
        r <<= 1;
    return r;
}

int extrace_prod_init(const struct extrace_prod_cfg *cfg)
{
    struct extrace_prod *p = &extrace_producer;
    struct extrace_shm_pinfo info = { 0 };
    struct extrace_shm_pinfo *pi;
    struct extrace_shm_pstate *ps;
    struct extrace_shm_cstate *cs;
    uint32_t data_off = EXTRACE_CTL_SIZE(EXTRACE_MASK_WORDS), size, old, ep, ae, s0, base;
    uint32_t w[EXTRACE_PINFO_WORDS];
    unsigned long flags;
    const uint8_t *bid;
    size_t bid_len;
    uint8_t *blk;

    if (!cfg || !cfg->block || ((uintptr_t)cfg->block & 7u) || cfg->ring_id == 0xFFu ||
        cfg->block_size < data_off + EXTRACE_MIN_RING_SIZE)
        return -EXTRACE_EINVAL;
    size = cfg->data_size ? cfg->data_size : pow2_floor(cfg->block_size - data_off);
    if (size > 0x80000000u)
        size = 0x80000000u;
    if (!extrace_is_pow2(size) || size < EXTRACE_MIN_RING_SIZE || size < 4u * EXTRACE_MAX_RESV ||
        size > cfg->block_size - data_off)
        return -EXTRACE_EINVAL;
    EXTRACE_ASSERT(!p->polling); /* init never overlaps poll (§8.10) */

    /* I0: no stale writer or publisher may land in the new epoch */
    flags = extrace_port_irq_save();
    if (p->inflight != 0 || p->publishing) {
        p->stats.init_busy++;
        extrace_port_irq_restore(flags);
        return -EXTRACE_EBUSY;
    }
    p->flags &= (uint8_t)~(EXTRACE_PF_READY | EXTRACE_PF_PANIC);
    for (unsigned int i = 0; i < EXTRACE_MASK_WORDS; i++)
        p->mask[i] = 0;
    extrace_port_irq_restore(flags);

    blk = cfg->block;
    pi = extrace_blk_pinfo(blk);
    ps = extrace_blk_pstate(blk);
    cs = extrace_blk_cstate(blk);

    /* I1: choose an epoch that is never 0 and never the acknowledged one */
    old = extrace_shm_ld32_own(&ps->epoch);
    if (extrace_shm_ld32_own(&pi->magic) != EXTRACE_SHM_MAGIC)
        old ^= extrace_port_ts();
    ae = extrace_shm_ld32(&cs->attached_epoch);
    ep = old + 1u;
    while (ep == 0 || ep == ae)
        ep++;
    s0 = extrace_shm_ld32(&cs->cmd_seq);
    base = index_base();

    /* I2: consumers stop trusting the block */
    extrace_shm_st32(&pi->magic, 0);
    extrace_port_wmb();

    /* I3: pinfo, pstate, then the epoch */
    info.layout_ver = EXTRACE_LAYOUT_VERSION;
    info.wire_ver = EXTRACE_WIRE_VERSION;
    info.flags = (EXTRACE_BIG_ENDIAN ? EXTRACE_PI_BIG_ENDIAN : 0u) | EXTRACE_PI_TS_GLOBAL;
    info.ring_id = cfg->ring_id;
    info.data_off = data_off;
    info.data_size = size;
    info.mask_words = EXTRACE_MASK_WORDS;
    info.ev_count = EXTRACE_EV_COUNT;
    info.max_record = EXTRACE_MAX_RECORD;
    info.ts_freq = extrace_port_ts_freq();
    bid = extrace_port_build_id(&bid_len);
    if (bid)
        extrace_port_memcpy(info.build_id, bid, bid_len < 20 ? bid_len : 20);
    for (unsigned int i = 0; cfg->name && i < 8 && cfg->name[i]; i++)
        info.name[i] = cfg->name[i];
    extrace_pinfo_pack(&info, w);
    for (unsigned int i = 1; i < EXTRACE_PINFO_WORDS; i++)
        extrace_shm_st32((uint32_t *)pi + i, w[i]);
    extrace_port_wmb();
    extrace_shm_st32(&ps->head, base);
    extrace_shm_st32(&ps->lost_total, 0);
    extrace_shm_st32(&ps->mask_gen_ack, 0);
    extrace_shm_st32(&ps->cmd_seq_ack, s0);
    extrace_shm_st32(&ps->hwm, 0);
    extrace_shm_st32(&ps->status, EXTRACE_ST_RUNNING);
    extrace_shm_st32(&ps->publish_cnt, 0);
    extrace_port_wmb();
    extrace_shm_st32(&ps->epoch, ep); /* epoch last: §7.3 C1 reads it first */
    extrace_port_wmb();

    /* I4 */
    extrace_shm_st32(&pi->magic, EXTRACE_SHM_MAGIC);
    extrace_port_wmb();

    /* I5: local state of the new epoch */
    p->pi = pi;
    p->ps = ps;
    p->cs = cs;
    p->mset = extrace_blk_mask_set(blk);
    p->mclr = extrace_blk_mask_clr(blk, EXTRACE_MASK_WORDS);
    p->data = blk + data_off;
    p->size = size;
    p->idx_mask = size - 1u;
    p->ring_id = cfg->ring_id;
    extrace_port_memcpy(p->info_name, info.name, sizeof(p->info_name));
    p->head = p->committed = p->published = p->notified = p->tail_cache = base;
    p->epoch = ep;
    p->mask_gen_seen = 0;
    p->cmd_seq_seen = s0;
    p->attached_seen = 0;
    p->lost_pending = 0;
    p->lost_bytes = 0;
    p->lost_total = 0;
    p->lost_total_st = 0;
    p->hwm = 0;
    p->publish_cnt = 0;
    p->notify_force = 0;
    p->publish_again = 0;
    p->publish_thr_dflt = cfg->publish_thr ? cfg->publish_thr : size / 4u;
    if (p->publish_thr_dflt > size)
        p->publish_thr_dflt = size;
    p->publish_thr = p->publish_thr_dflt;
    p->notify_thr = 0;
    p->sync_period = EXTRACE_CFG_SYNC_PERIOD_TICKS ? EXTRACE_CFG_SYNC_PERIOD_TICKS
                                                    : extrace_port_ts_freq() / 10u;
    if (p->sync_period == 0 || p->sync_period > (1u << 30))
        p->sync_period = 1u << 30;
    p->last_ts = extrace_port_ts();
    p->last_sync_ts = p->last_ts;
    p->stats = (struct extrace_prod_stats){ .epoch = ep, .init_busy = p->stats.init_busy };

    /* I6: defaults, READY, and INFO + SYNC as the first records of the epoch */
    flags = extrace_port_irq_save();
    for (unsigned int i = 0; i < EXTRACE_MASK_WORDS; i++)
        p->mask[i] = extrace_ev_default_word(i);
    p->flags |= EXTRACE_PF_READY;
    emit_info(true);
    emit_sync(true);
    extrace_port_irq_restore(flags);
    publish();
    extrace_port_notify(p->ring_id);
    return 0;
}

/* ---- poll (§7.5, §7.7, §8.10) ---- */

static void load_thr(struct extrace_prod *p)
{
    uint32_t pt = extrace_shm_ld32(&p->cs->publish_thr);
    uint32_t nt = extrace_shm_ld32(&p->cs->notify_thr);

    p->publish_thr = pt ? (pt < p->size ? pt : p->size) : p->publish_thr_dflt;
    p->notify_thr = nt;
}

/* M1-M5: apply a new mask generation, seqlock read. */
static void apply_mask(struct extrace_prod *p)
{
    uint32_t g1 = extrace_shm_ld32(&p->cs->mask_gen), g2;

    if (likely((g1 & 1u) || g1 == p->mask_gen_seen))
        return;
    extrace_port_rmb();
    for (unsigned int w = 0; w < EXTRACE_MASK_WORDS; w++) {
        uint32_t s = extrace_shm_ld32(&p->mset[w]);
        uint32_t c = extrace_shm_ld32(&p->mclr[w]);

        p->mask[w] = (extrace_ev_default_word(w) & ~c) | s;
    }
    extrace_port_rmb();
    g2 = extrace_shm_ld32(&p->cs->mask_gen);
    if (unlikely(g2 != g1))
        return; /* torn: the next poll recomputes */
    p->mask_gen_seen = g1;
    extrace_shm_st32(&p->ps->mask_gen_ack, g1);
    emit_cfg(g1);
}

/* X1-X4: execute pending commands. */
static void run_cmds(struct extrace_prod *p)
{
    uint32_t s = extrace_shm_ld32(&p->cs->cmd_seq), b;

    if (likely(s == p->cmd_seq_seen))
        return;
    extrace_port_rmb();
    b = extrace_shm_ld32(&p->cs->cmd_bits);
    if (b & EXTRACE_CMD_THR)
        load_thr(p);
    if (b & EXTRACE_CMD_INFO)
        emit_info(false);
    if (b & EXTRACE_CMD_SYNC)
        emit_sync(false);
    if (b & EXTRACE_CMD_FLUSH)
        publish();
    p->cmd_seq_seen = s;
    extrace_port_wmb(); /* the effects before the acknowledgement */
    extrace_shm_st32(&p->ps->cmd_seq_ack, s);
}

void extrace_prod_poll(void)
{
    struct extrace_prod *p = &extrace_producer;
    const struct extrace_shm_cstate *cs = p->cs;

    EXTRACE_ASSERT(!p->polling);
    p->polling = 1;
    if (unlikely(!(p->flags & EXTRACE_PF_READY)))
        goto out;
    /* M0: nothing in cstate is trusted before the consumer attached to this epoch */
    if (extrace_shm_ld32(&cs->magic) == EXTRACE_SHM_MAGIC &&
        extrace_shm_ld32(&cs->attached_epoch) == p->epoch) {
        uint32_t t;
        unsigned long flags;

        extrace_port_rmb();
        t = extrace_shm_ld32(&cs->tail); /* R1-R3 */
#if EXTRACE_CFG_TEST_HOOKS
        if (extrace_test_tail_hook)
            extrace_test_tail_hook(p->epoch, t);
#endif
        extrace_port_mb();
        flags = extrace_port_irq_save();
        accept_tail(p, t);
        extrace_port_irq_restore(flags);
        if (unlikely(!p->attached_seen)) {
            p->attached_seen = 1;
            load_thr(p);
        }
        apply_mask(p);
        run_cmds(p);
    }
    if (unlikely(extrace_port_ts() - p->last_sync_ts >= p->sync_period))
        emit_sync(false);
    if (p->committed != p->published || p->notify_force)
        publish();
out:
    p->polling = 0;
}

/* ---- panic (§8.10) ---- */

void extrace_prod_panic(uint32_t reason, uintptr_t pc)
{
    struct extrace_prod *p = &extrace_producer;
    unsigned long flags;

    flags = extrace_port_irq_save();
    if (p->flags & EXTRACE_PF_READY) {
        uint32_t to;

        if (p->inflight == 0) {
            const uint64_t v[2] = { reason, (uint64_t)pc };

            emit_ints64_core(EXTRACE_HDR_PANIC, 0, v, 2, true, true);
        }
        p->flags = (uint8_t)((p->flags & ~EXTRACE_PF_READY) | EXTRACE_PF_PANIC);
        /* the interrupted publisher never resumes: publish committed directly */
        to = p->committed;
        extrace_port_wmb();
        extrace_shm_st32(&p->ps->head, to);
        store_lost(p);
        extrace_shm_st32(&p->ps->status, EXTRACE_ST_PANIC);
        p->published = to;
        extrace_port_wmb();
        extrace_port_notify(p->ring_id);
    }
    extrace_port_irq_restore(flags);
}

void extrace_prod_stats(struct extrace_prod_stats *out)
{
    const struct extrace_prod *p = &extrace_producer;
    unsigned long flags;

    flags = extrace_port_irq_save();
    *out = p->stats;
    out->epoch = p->epoch;
    out->lost_total = p->lost_total;
    out->hwm = p->hwm;
    extrace_port_irq_restore(flags);
}
