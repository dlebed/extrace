// SPDX-License-Identifier: MIT
/*
 * extrace_cons.c - ring reader (DESIGN §7.3-§7.7, §9.3, §11).
 *
 * The consumer writes only cstate and the override bitmaps, always from local
 * mirrors (§6.3 rule 8), and reads producer words with the ordering of §7.
 */
#include "extrace_cons.h"
#include "extrace_wire.h"

#if EXTRACE_CFG_TEST_HOOKS
#include "extrace_test_hooks.h"
#endif

/* Producer-owned regions: read-only for the consumer (§6.3, one writer per word). */
static inline const struct extrace_shm_pinfo *cpi(const struct extrace_cons *c)
{
    return extrace_blk_pinfo(c->block);
}

static inline const struct extrace_shm_pstate *cps(const struct extrace_cons *c)
{
    return extrace_blk_pstate(c->block);
}

static inline struct extrace_shm_cstate *ccs(const struct extrace_cons *c)
{
    return extrace_blk_cstate(c->block);
}

static inline uint32_t *mset_sh(const struct extrace_cons *c)
{
    return extrace_blk_mask_set(c->block);
}

static inline uint32_t *mclr_sh(const struct extrace_cons *c)
{
    return extrace_blk_mask_clr(c->block, c->mask_words);
}

static uint32_t index_base(void)
{
#if EXTRACE_CFG_TEST_HOOKS
    return extrace_test_index_base;
#else
    return 0;
#endif
}

/*
 * After an index that looked invalid: did the producer (re)initialize? I2 stores
 * magic = 0 before I3 resets head, and I3 stores the epoch before I4 restores
 * magic, so magic first, then the epoch, each after rmb (§17).
 */
static bool producer_moved(const struct extrace_cons *c, uint32_t e)
{
    uint32_t m;

    extrace_port_rmb();
    m = extrace_shm_ld32(&cpi(c)->magic);
    extrace_port_rmb();
    return m != EXTRACE_SHM_MAGIC || extrace_shm_ld32(&cps(c)->epoch) != e;
}

static int validate(const struct extrace_cons *c, const struct extrace_shm_pinfo *s)
{
    if ((s->layout_ver >> 8) != EXTRACE_LAYOUT_MAJOR || s->wire_ver != EXTRACE_WIRE_VERSION)
        return -EXTRACE_EVERSION;
    if (s->ev_count < EXTRACE_SYS_FIRST_USER || s->mask_words != (s->ev_count + 31u) / 32u ||
        s->data_off != EXTRACE_CTL_SIZE(s->mask_words) || !extrace_is_pow2(s->data_size) ||
        s->data_size < EXTRACE_MIN_RING_SIZE || s->data_size > 0x80000000u ||
        s->data_off > c->block_size || s->data_size > c->block_size - s->data_off ||
        s->max_record > s->data_size / 4u || s->ring_id == EXTRACE_RING_ALL)
        return -EXTRACE_ECORRUPT;
    if (2u * s->mask_words > c->mirror_words)
        return -EXTRACE_ENOSPC;
    return 0;
}

static bool same_image(const struct extrace_cons *c, const struct extrace_shm_pinfo *s)
{
    if (!(c->flags & EXTRACE_CF_HAVE_SNAP) || c->pinfo.ev_count != s->ev_count)
        return false;
    for (unsigned int i = 0; i < sizeof(s->build_id); i++)
        if (c->pinfo.build_id[i] != s->build_id[i])
            return false;
    return true;
}

/* A1-A4 (§7.6). */
static int attach_once(struct extrace_cons *c)
{
    const struct extrace_shm_pinfo *pi = cpi(c);
    const struct extrace_shm_pstate *ps = cps(c);
    struct extrace_shm_pinfo snap;
    struct extrace_shm_cstate *cs = ccs(c);
    uint32_t w[EXTRACE_PINFO_WORDS], e1, e2, m1, m2, tail, words;
    bool resume;
    int ret;

    c->flags &= (uint8_t)~(EXTRACE_CF_ATTACHED | EXTRACE_CF_RESUMED | EXTRACE_CF_ERROR);

    /* A1: seqlock read of the producer's identity */
    e1 = extrace_shm_ld32(&ps->epoch);
    extrace_port_rmb();
    m1 = extrace_shm_ld32(&pi->magic);
    extrace_port_rmb();
    for (unsigned int i = 0; i < EXTRACE_PINFO_WORDS; i++)
        w[i] = extrace_shm_ld32((const uint32_t *)pi + i);
    extrace_port_rmb();
    m2 = extrace_shm_ld32(&pi->magic);
    extrace_port_rmb();
    e2 = extrace_shm_ld32(&ps->epoch);
    if (m1 != EXTRACE_SHM_MAGIC || m2 != EXTRACE_SHM_MAGIC || e1 != e2)
        return c->last_error = -EXTRACE_ENOTREADY;
    extrace_pinfo_unpack(&snap, w);
    ret = validate(c, &snap);
    if (ret) {
        c->stats.attach_errors++;
        return c->last_error = ret;
    }
    words = snap.mask_words;

    /* A2 */
    resume = extrace_shm_ld32_own(&cs->magic) == EXTRACE_SHM_MAGIC &&
             extrace_shm_ld32_own(&cs->attached_epoch) == e1;
    c->mask_words = (uint16_t)words;
    if (resume) {
        /* consumer restarted: load the mirrors once (§6.3 rule 8 exception) */
        uint32_t h;

        tail = extrace_shm_ld32_own(&cs->tail);
        c->mask_gen = extrace_shm_ld32_own(&cs->mask_gen);
        c->cmd_seq = extrace_shm_ld32_own(&cs->cmd_seq);
        c->cmd_pending = extrace_shm_ld32_own(&cs->cmd_bits);
        c->notify_thr = extrace_shm_ld32_own(&cs->notify_thr);
        c->publish_thr = extrace_shm_ld32_own(&cs->publish_thr);
        for (uint32_t i = 0; i < words; i++) {
            c->mask_mirror[i] = extrace_shm_ld32_own(&mset_sh(c)[i]);
            c->mask_mirror[words + i] = extrace_shm_ld32_own(&mclr_sh(c)[i]);
        }
        if (c->mask_gen & 1u) { /* stopped inside W1-W3: close the transaction */
            c->mask_gen++;
            extrace_shm_st32(&cs->mask_gen, c->mask_gen);
        }
        extrace_port_rmb(); /* head no older than the epoch read in A1 */
        h = extrace_shm_ld32(&ps->head);
        if (h - tail > snap.data_size) {
            if (producer_moved(c, e1))
                return c->last_error = -EXTRACE_ENOTREADY; /* attach to the new epoch later */
            c->stats.head_errors++;
            tail = h;
        }
    } else {
        tail = index_base(); /* a new epoch: indices start at 0 */
        if (extrace_shm_ld32_own(&cs->magic) != EXTRACE_SHM_MAGIC || !same_image(c, &snap)) {
            /* new image or garbage: drop the overrides, nothing old configures it */
            if (!(c->flags & EXTRACE_CF_HAVE_SNAP))
                c->cmd_seq = 0;
            c->mask_gen = 2;
            c->cmd_pending = 0;
            for (uint32_t i = 0; i < 2u * words; i++)
                c->mask_mirror[i] = 0;
            for (uint32_t i = 0; i < words; i++) {
                extrace_shm_st32(&mset_sh(c)[i], 0);
                extrace_shm_st32(&mclr_sh(c)[i], 0);
            }
            extrace_shm_st32(&cs->mask_gen, c->mask_gen);
            extrace_shm_st32(&cs->notify_thr, c->notify_thr);
            extrace_shm_st32(&cs->publish_thr, c->publish_thr);
            extrace_shm_st32(&cs->cmd_bits, 0);
            extrace_shm_st32(&cs->cmd_seq, c->cmd_seq);
            extrace_port_wmb(); /* all before A3 */
        }
    }

    /* A3: tail before attached_epoch; attached_epoch publishes the attach */
    extrace_shm_st32(&cs->tail, tail);
    extrace_port_wmb();
    extrace_shm_st32(&cs->attached_epoch, e1);
    extrace_shm_st32(&cs->magic, EXTRACE_SHM_MAGIC);
    extrace_port_wmb();

    c->epoch = e1;
    c->tail = tail;
    c->stream_pos = tail;
    c->pinfo = snap;
    c->size = snap.data_size;
    c->data = c->block + snap.data_off;
    c->ring_id = snap.ring_id;
    c->ev_count = snap.ev_count;
    c->flags |= EXTRACE_CF_ATTACHED | EXTRACE_CF_HAVE_SNAP;
    if (resume)
        c->flags |= EXTRACE_CF_RESUMED;
    if (resume)
        c->stats.resumes++;
    else
        c->stats.attaches++;
    (void)e2;
    return c->last_error = 0;
}

int extrace_cons_attach(struct extrace_cons *c, void *block, uint32_t block_size,
                        uint32_t *mirror, uint16_t mirror_words, uint16_t flags)
{
    if (!c || !block || ((uintptr_t)block & 7u) || !mirror)
        return -EXTRACE_EINVAL;
    if (c->block != block)
        c->flags = 0; /* another block: no snapshot */
    c->flags &= (uint8_t)~EXTRACE_CF_ATTACHED;
    c->block = block;
    c->block_size = block_size;
    c->mask_mirror = mirror;
    c->mirror_words = mirror_words;
    c->attach_flags = flags;
    return attach_once(c);
}

int extrace_cons_poll_state(struct extrace_cons *c)
{
    int ret;

    if (!c->block)
        return -EXTRACE_EINVAL;
    if (c->flags & EXTRACE_CF_ATTACHED) {
        if (extrace_shm_ld32(&cps(c)->epoch) == c->epoch)
            return 0;
        c->stats.epoch_changes++;
    }
    ret = attach_once(c);
    return ret ? ret : 1;
}

bool extrace_cons_attached(const struct extrace_cons *c)
{
    return c->flags & EXTRACE_CF_ATTACHED;
}

bool extrace_cons_epoch_ok(const struct extrace_cons *c)
{
    extrace_port_rmb();
    return extrace_shm_ld32(&cps(c)->epoch) == c->epoch;
}

/* The producer restarted: re-attach, report -EXTRACE_EEPOCH (or why not). */
static int reattach(struct extrace_cons *c)
{
    int ret;

    c->stats.epoch_changes++;
    ret = attach_once(c);
    return ret ? ret : -EXTRACE_EEPOCH;
}

/* §17: head - tail > size */
static int head_error(struct extrace_cons *c, uint32_t h)
{
    if (producer_moved(c, c->epoch)) {
        if (extrace_shm_ld32(&cpi(c)->magic) != EXTRACE_SHM_MAGIC) {
            /* (re)initializing or dead: wait for A1 to succeed again */
            c->flags &= (uint8_t)~EXTRACE_CF_ATTACHED;
            return c->last_error = -EXTRACE_ENOTREADY;
        }
        return reattach(c);
    }
    c->flags |= EXTRACE_CF_ERROR;
    c->stats.head_errors++;

    c->stream_pos += h - c->tail;
    c->tail = h;
    extrace_shm_st32(&ccs(c)->tail, h);
    extrace_port_wmb();
    return -EXTRACE_ECORRUPT;
}

/* Header walk of the budget clamp (§11.3): padding counts as a record. */
uint32_t extrace_rec_span_len(const uint8_t *p, const uint8_t *end)
{
    const uint8_t *q = p;
    uint32_t h, v, n, kind;

    if (q >= end)
        return 0;
    if (*q == EXTRACE_HDR_PAD1)
        return 1;
    if (*q == EXTRACE_HDR_PADN) {
        n = extrace_get_uv32(q + 1, end, &v);
        if (!n || v > (uint32_t)(end - q) - 1u - n)
            return 0;
        return 1u + n + v;
    }
    n = extrace_get_uv32(q, end, &h);
    if (!n || (h >> 3) == 0)
        return 0;
    q += n;
    kind = h & 7u;
    if (kind != EXTRACE_K_MARK) {
        n = extrace_get_uv32(q, end, &v);
        if (!n)
            return 0;
        q += n;
    }
    if (extrace_kind_has_len(kind)) {
        n = extrace_get_uv32(q, end, &v);
        if (!n || v > (uint32_t)(end - q) - n)
            return 0;
        q += n + v;
    } else {
        for (unsigned int i = extrace_hdr_ftrace_short(h); i; i--) {
            n = extrace_get_uv32(q, end, &v);
            if (!n)
                return 0;
            q += n;
        }
    }
    return (uint32_t)(q - p);
}

static uint32_t clamp(const struct extrace_window *w, uint32_t max_bytes)
{
    uint32_t n = 0;

    for (unsigned int s = 0; s < w->nspan; s++) {
        const uint8_t *q = w->span[s].p, *end = q + w->span[s].len;

        while (q < end) {
            uint32_t l = extrace_rec_span_len(q, end);

            if (!l) /* malformed: the decoder reports it; offer what is known */
                return n ? n : extrace_window_len(w);
            if (n && n + l > max_bytes)
                return n;
            n += l;
            q += l;
            if (n >= max_bytes)
                return n;
        }
    }
    return n;
}

static void window_set(struct extrace_window *w, const struct extrace_cons *c, uint32_t n,
                       uint32_t e)
{
    uint32_t off = c->tail & (c->size - 1u), len0 = c->size - off;

    if (len0 > n)
        len0 = n;
    w->span[0].p = c->data + off;
    w->span[0].len = len0;
    w->span[1].p = c->data;
    w->span[1].len = n - len0;
    w->nspan = n == 0 ? 0 : (n > len0 ? 2 : 1);
    w->start = c->tail;
    w->end = c->tail + n;
    w->pos = c->stream_pos;
    w->epoch = e;
}

int extrace_cons_peek(struct extrace_cons *c, struct extrace_window *w, uint32_t max_bytes)
{
    const struct extrace_shm_pstate *ps = cps(c);
    uint32_t e, h, avail, n;

    *w = (struct extrace_window){ 0 };
    if (!(c->flags & EXTRACE_CF_ATTACHED))
        return -EXTRACE_ENOTREADY;
    /* C1-C3: epoch first; the producer stores head before epoch (I3) */
    e = extrace_shm_ld32(&ps->epoch);
    extrace_port_rmb();
    h = extrace_shm_ld32(&ps->head);
    extrace_port_rmb();
    if (e != c->epoch)
        return reattach(c);
    avail = h - c->tail;
    if (avail > c->size)
        return head_error(c, h);
    c->flags &= (uint8_t)~EXTRACE_CF_ERROR;
    if (!avail)
        return 0;
    window_set(w, c, avail, e);
    if (max_bytes < avail) {
        n = clamp(w, max_bytes);
        window_set(w, c, n, e);
    }
    c->stats.windows++;
    return (int)extrace_window_len(w);
}

static void window_trim(struct extrace_window *w, uint32_t bytes)
{
    w->start += bytes;
    w->pos += bytes;
    if (w->start == w->end) {
        *w = (struct extrace_window){ .start = w->start, .end = w->end, .pos = w->pos,
                                      .epoch = w->epoch };
        return;
    }
    if (bytes < w->span[0].len) {
        w->span[0].p += bytes;
        w->span[0].len -= bytes;
        return;
    }
    bytes -= w->span[0].len;
    w->span[0].p = w->span[1].p + bytes;
    w->span[0].len = w->span[1].len - bytes;
    w->span[1].len = 0;
    w->nspan = 1;
}

int extrace_cons_release(struct extrace_cons *c, struct extrace_window *w, uint32_t bytes)
{
    if (bytes == 0 || w->start == w->end)
        return 0;
    if (!(c->flags & EXTRACE_CF_ATTACHED) || w->epoch != c->epoch || w->start != c->tail ||
        bytes > extrace_window_len(w))
        return -EXTRACE_EINVAL;
    extrace_port_mb(); /* C5: every read of the window is complete */
    if (extrace_shm_ld32(&cps(c)->epoch) != c->epoch) { /* C6 */
        *w = (struct extrace_window){ 0 };
        return reattach(c);
    }
    c->tail += bytes; /* C7 */
    c->stream_pos += bytes;
    extrace_shm_st32(&ccs(c)->tail, c->tail);
    extrace_port_wmb();
    c->stats.bytes += bytes;
    window_trim(w, bytes);
    return 0;
}

int extrace_cons_read(struct extrace_cons *c, void *dst, uint32_t dst_size)
{
    struct extrace_window w;
    uint8_t *d = dst;
    int n, ret;

    n = extrace_cons_peek(c, &w, dst_size);
    if (n <= 0)
        return n;
    if ((uint32_t)n > dst_size)
        return -EXTRACE_ENOSPC; /* the next record alone does not fit */
    extrace_port_memcpy(d, w.span[0].p, w.span[0].len);
    if (w.nspan > 1)
        extrace_port_memcpy(d + w.span[0].len, w.span[1].p, w.span[1].len);
    ret = extrace_cons_release(c, &w, (uint32_t)n);
    return ret ? ret : n; /* a changed epoch invalidates the copy */
}

/* ---- commands (§7.7) ---- */

int extrace_cons_cmd(struct extrace_cons *c, uint32_t cmd_bits)
{
    struct extrace_shm_cstate *cs = ccs(c);

    if (!(c->flags & EXTRACE_CF_ATTACHED))
        return -EXTRACE_ENOTREADY;
    if (cmd_bits & ~EXTRACE_CMD_ALL)
        return -EXTRACE_EINVAL;
    if (extrace_shm_ld32(&cps(c)->cmd_seq_ack) == c->cmd_seq) /* K1 */
        c->cmd_pending = 0;
    c->cmd_pending |= cmd_bits; /* K2 */
    extrace_shm_st32(&cs->cmd_bits, c->cmd_pending);
    extrace_port_wmb();
    c->cmd_seq++; /* K3 */
    extrace_shm_st32(&cs->cmd_seq, c->cmd_seq);
    extrace_port_wmb();
    extrace_port_notify_producer(c->ring_id);
    return 0;
}

bool extrace_cons_cmd_done(const struct extrace_cons *c)
{
    return extrace_shm_ld32(&cps(c)->cmd_seq_ack) == c->cmd_seq;
}

int extrace_cons_set_thr(struct extrace_cons *c, uint32_t publish_thr, uint32_t notify_thr)
{
    struct extrace_shm_cstate *cs = ccs(c);

    if (!(c->flags & EXTRACE_CF_ATTACHED))
        return -EXTRACE_ENOTREADY;
    c->publish_thr = publish_thr;
    c->notify_thr = notify_thr;
    extrace_shm_st32(&cs->publish_thr, publish_thr);
    extrace_shm_st32(&cs->notify_thr, notify_thr);
    extrace_port_wmb(); /* both before the command that makes the producer load them */
    return extrace_cons_cmd(c, EXTRACE_CMD_THR);
}

int extrace_cons_set_notify(struct extrace_cons *c, uint32_t notify_thr)
{
    return extrace_cons_set_thr(c, c->publish_thr, notify_thr);
}

int extrace_cons_set_publish_thr(struct extrace_cons *c, uint32_t thr)
{
    return extrace_cons_set_thr(c, thr, c->notify_thr);
}

int extrace_cons_diag(const struct extrace_cons *c, struct extrace_cons_diag *out)
{
    const struct extrace_shm_pstate *ps = cps(c);

    *out = (struct extrace_cons_diag){ 0 };
    if (!(c->flags & EXTRACE_CF_ATTACHED))
        return -EXTRACE_ENOTREADY;
    out->lost_total = extrace_shm_ld32(&ps->lost_total);
    out->hwm = extrace_shm_ld32(&ps->hwm);
    out->status = extrace_shm_ld32(&ps->status);
    return 0;
}

/* ---- event control: seqlock writer W1-W4 (§7.5) ---- */

static uint32_t last_word_mask(const struct extrace_cons *c, uint32_t w)
{
    uint32_t bits = c->ev_count - 32u * w;

    return bits >= 32u ? 0xFFFFFFFFu : (1u << bits) - 1u;
}

/* Store the new bitmaps (mirrors updated, changed words stored) in one transaction. */
static int mask_commit(struct extrace_cons *c, const uint32_t *set, const uint32_t *clr)
{
    struct extrace_shm_cstate *cs = ccs(c);
    uint32_t words = c->mask_words, *ms = c->mask_mirror, *mc = c->mask_mirror + words;

    c->mask_gen++; /* W1: odd (generations are even between transactions) */
    extrace_shm_st32(&cs->mask_gen, c->mask_gen);
    extrace_port_wmb();
    for (uint32_t i = 0; i < words; i++) { /* W2 */
        uint32_t m = last_word_mask(c, i), s = set[i] & m, k = clr[i] & m & ~s;

        if (ms[i] != s) {
            ms[i] = s;
            extrace_shm_st32(&mset_sh(c)[i], s);
        }
        if (mc[i] != k) {
            mc[i] = k;
            extrace_shm_st32(&mclr_sh(c)[i], k);
        }
    }
    extrace_port_wmb();
    c->mask_gen++; /* W3: even */
    extrace_shm_st32(&cs->mask_gen, c->mask_gen);
    extrace_port_wmb();
    extrace_port_notify_producer(c->ring_id); /* W4 */
    return 0;
}

int extrace_cons_ev_set(struct extrace_cons *c, uint16_t id, enum extrace_ev_state st)
{
    uint32_t s[EXTRACE_CFG_MAX_EVENTS / 32u + 1u], k[EXTRACE_CFG_MAX_EVENTS / 32u + 1u];
    uint32_t words = c->mask_words, w = id / 32u, bit = 1u << (id % 32u);

    if (!(c->flags & EXTRACE_CF_ATTACHED))
        return -EXTRACE_ENOTREADY;
    if (id >= c->ev_count || st > EXTRACE_EVS_OFF || words > sizeof(s) / sizeof(s[0]))
        return -EXTRACE_EINVAL;
    for (uint32_t i = 0; i < words; i++) {
        s[i] = c->mask_mirror[i];
        k[i] = c->mask_mirror[words + i];
    }
    s[w] &= ~bit;
    k[w] &= ~bit;
    if (st == EXTRACE_EVS_ON)
        s[w] |= bit;
    else if (st == EXTRACE_EVS_OFF)
        k[w] |= bit;
    if (s[w] == c->mask_mirror[w] && k[w] == c->mask_mirror[words + w])
        return 0;
    return mask_commit(c, s, k);
}

int extrace_cons_ev_get(const struct extrace_cons *c, uint16_t id, enum extrace_ev_state *st)
{
    uint32_t w = id / 32u, bit = 1u << (id % 32u);

    if (!(c->flags & EXTRACE_CF_ATTACHED))
        return -EXTRACE_ENOTREADY;
    if (id >= c->ev_count)
        return -EXTRACE_EINVAL;
    if (c->mask_mirror[w] & bit)
        *st = EXTRACE_EVS_ON;
    else if (c->mask_mirror[c->mask_words + w] & bit)
        *st = EXTRACE_EVS_OFF;
    else
        *st = EXTRACE_EVS_DEFAULT;
    return 0;
}

int extrace_cons_ev_set_many(struct extrace_cons *c, const uint32_t *set, const uint32_t *clr,
                             uint16_t words)
{
    uint32_t s[EXTRACE_CFG_MAX_EVENTS / 32u + 1u], k[EXTRACE_CFG_MAX_EVENTS / 32u + 1u];

    if (!(c->flags & EXTRACE_CF_ATTACHED))
        return -EXTRACE_ENOTREADY;
    if (words > c->mask_words || c->mask_words > sizeof(s) / sizeof(s[0]))
        return -EXTRACE_EINVAL;
    for (uint32_t i = 0; i < c->mask_words; i++) {
        s[i] = (set && i < words) ? set[i] : 0u;
        k[i] = (clr && i < words) ? clr[i] : 0u;
    }
    return mask_commit(c, s, k);
}

int extrace_cons_ev_reset_all(struct extrace_cons *c)
{
    return extrace_cons_ev_set_many(c, nullptr, nullptr, 0);
}

uint32_t extrace_cons_mask_gen(const struct extrace_cons *c)
{
    return c->mask_gen;
}

bool extrace_cons_mask_applied(const struct extrace_cons *c)
{
    return (c->flags & EXTRACE_CF_ATTACHED) &&
           extrace_shm_ld32(&cps(c)->mask_gen_ack) == c->mask_gen;
}
