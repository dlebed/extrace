// SPDX-License-Identifier: MIT
/*
 * test_lifecycle.c - randomized lifecycle and memory-ordering runs on the
 * write-buffer simulator (DESIGN §18.3, §18.4).
 *
 * Core 0 runs the producer: thread P1 (emits, polls, re-initializes, reboots),
 * thread P2 (emits, holds direct reservations) and an ISR (emits). Core 1 runs
 * the consumer under the dispatcher, with a validating record sink and a bulk
 * sink, and restarts, reconfigures and commands the producer at random. Shared
 * memory starts random or zero.
 *
 * Every delivered record is checked: sequence numbers per producer context only
 * grow (no duplicate, no reordering, no stale data), payloads are intact, an
 * event the consumer never enabled never appears (no garbage configuration), the
 * decoder never fails, and the bulk stream positions are contiguous. At the end
 * the final epoch is accounted exactly: delivered records and LOST counts equal
 * the producer's reservations and drops.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extrace_test.h"
#include "extrace.h"
#include "extrace_cons.h"
#include "extrace_disp.h"
#include "extrace_test_hooks.h"
#include "wbsim.h"

#define DATA_MAX 1024u /* inits alternate 512 and 1024: pinfo changes between epochs */
#define BLOCK_SIZE EXTRACE_RING_BLOCK_SIZE(DATA_MAX, EXTRACE_MASK_WORDS)
#define HIST_MAX 4096
#define NCTX_SEQ 3 /* P1, P2, ISR */

struct world {
    int blk;
    uint8_t *pview, *cview;
    unsigned int ops;
    bool random_mem;
    bool reboots;
    /* producer side */
    uint32_t seq[NCTX_SEQ];
    bool stop, p2_done, isr_done, prod_done;
    uint32_t final_epoch, final_head;
    struct extrace_prod_stats final_stats;
    uint32_t inits, busy, reboots_done;
    uint32_t cancels;   /* cancelled reservations of cancel_epoch */
    uint32_t cancel_epoch;
    /* consumer side */
    struct extrace_cons cons;
    uint32_t mirror[EXTRACE_CONS_MIRROR_WORDS(64)];
    struct extrace_disp disp;
    struct extrace_sink rsink, bsink;
    uint32_t last_seq[NCTX_SEQ];
    bool seen[NCTX_SEQ];
    uint32_t epoch;
    uint64_t recs, lost;
    uint64_t bulk_end;
    bool bulk_valid;
    uint32_t restarts;
    /* consumer mask transactions: (generation, set, clr) */
    uint32_t hist[HIST_MAX][3];
    unsigned int nhist;
    /* failures */
    uint32_t bad_order, bad_payload, bad_config, bad_pos, bad_tail, bad_mask;
    uint64_t delivered;
};

static struct world W;

static void hist_put(uint32_t gen, uint32_t set, uint32_t clr)
{
    if (W.nhist < HIST_MAX) {
        W.hist[W.nhist][0] = gen;
        W.hist[W.nhist][1] = set;
        W.hist[W.nhist][2] = clr;
        W.nhist++;
    }
}

/* the current bitmaps (after an attach) */
static void hist_add(void)
{
    if (extrace_cons_attached(&W.cons) && !(W.cons.mask_gen & 1u))
        hist_put(W.cons.mask_gen, W.mirror[0], W.mirror[W.cons.mask_words]);
}

/*
 * The bitmaps of the transaction about to run, recorded before it starts: the
 * producer may apply it while the consumer is still inside the call.
 */
static void hist_pre(uint32_t set, uint32_t clr)
{
    const uint32_t valid = (1u << EXTRACE_EV_COUNT) - 1u;

    if (extrace_cons_attached(&W.cons))
        hist_put(W.cons.mask_gen + 2u, set & valid, clr & valid & ~set);
}

/*
 * Invariants over private state of both agents, checked at producer-side op
 * boundaries. The consumer's own view holds its latest stores.
 */
static void check_tail(void)
{
    const struct extrace_shm_cstate *cs = extrace_blk_cstate(W.cview);
    const struct extrace_prod *p = &extrace_producer;
    bool att;

    if (!(p->flags & EXTRACE_PF_READY))
        return;
    att = extrace_le32(cs->magic) == EXTRACE_SHM_MAGIC &&
          extrace_le32(cs->attached_epoch) == p->epoch;
    /* the cached tail is never ahead of what the consumer released in this epoch */
    if (att ? extrace_le32(cs->tail) - p->tail_cache > p->size
            : p->tail_cache != extrace_test_index_base) {
        W.bad_tail++;
        xt_log("tail: att %d cs.tail %u tail_cache %u\n", att, extrace_le32(cs->tail),
               p->tail_cache);
    }
}

/* Highest window end offered per epoch: the consumer releases at most that far. */
#define ENDS_MAX 64
static struct {
    uint32_t epoch, end;
} ends[ENDS_MAX];
static unsigned int nends;

static uint32_t *end_of(uint32_t epoch)
{
    for (unsigned int i = 0; i < nends; i++)
        if (ends[i].epoch == epoch)
            return &ends[i].end;
    if (nends == ENDS_MAX)
        nends = 0; /* forget the oldest epochs */
    ends[nends].epoch = epoch;
    ends[nends].end = extrace_test_index_base;
    return &ends[nends++].end;
}

/*
 * Every tail the producer reads for an epoch is one the consumer stored for that
 * epoch: never beyond what it was offered (a stale tail of an older epoch is).
 */
static void tail_read(uint32_t epoch, uint32_t t)
{
    uint32_t base = extrace_test_index_base, end = *end_of(epoch);

    if (t - base > end - base) {
        W.bad_tail++;
        xt_log("tail read: epoch %08x read %u, offered up to %u\n", epoch, t, end);
    }
}

static void check_mask(void)
{
    const struct extrace_shm_cstate *cs = extrace_blk_cstate(W.cview);
    const struct extrace_prod *p = &extrace_producer;
    uint32_t def = extrace_ev_default_word(0), m = p->mask[0];
    bool att;

    if (!(p->flags & EXTRACE_PF_READY))
        return;
    if (m & (1u << EXTRACE_EV_T_OFF)) {
        W.bad_mask++; /* in no generation the consumer ever wrote */
        xt_log("mask: T_OFF on, gen %u mask %08x\n", p->mask_gen_seen, m);
        return;
    }
    att = extrace_le32(cs->magic) == EXTRACE_SHM_MAGIC &&
          extrace_le32(cs->attached_epoch) == p->epoch;
    if (!att) {
        /* M0: nothing applied before the consumer attached to this epoch */
        if (m != def || p->mask_gen_seen != 0) {
            W.bad_mask++;
            xt_log("mask: applied before attach: gen %u mask %08x\n", p->mask_gen_seen, m);
        }
        return;
    }
    /*
     * A torn read leaves a mix in the live mask without marking it (M5); only a
     * producer that caught up with the consumer's generation must match it.
     */
    if (p->mask_gen_seen == 0 || extrace_le32(cs->mask_gen) != p->mask_gen_seen)
        return;
    for (unsigned int i = 0; i < W.nhist; i++)
        if (W.hist[i][0] == p->mask_gen_seen && ((def & ~W.hist[i][2]) | W.hist[i][1]) == m)
            return;
    W.bad_mask++;
    xt_log("mask: gen %u mask %08x\n", p->mask_gen_seen, m);
}

/* debug: reservations of the current epoch vs record starts seen in bulk windows */
#define DBG_MAX 4096
static uint32_t dbg_res[DBG_MAX], dbg_seen[DBG_MAX];
static unsigned int dbg_nres, dbg_nseen;
static uint32_t dbg_epoch;

static void dbg_resv(uint32_t epoch, uint32_t idx)
{
    if (epoch != dbg_epoch) {
        dbg_epoch = epoch;
        dbg_nres = 0;
    }
    if (dbg_nres < DBG_MAX)
        dbg_res[dbg_nres++] = idx;
}

static void rsink_record(struct extrace_sink *s, const struct extrace_rec *r)
{
    uint64_t v[16];
    int n;

    (void)s;
    W.delivered++;
    if (r->event == EXTRACE_SYS_LOST) {
        n = extrace_rec_ints(r, v, 16, nullptr);
        if (n >= 1)
            W.lost += v[0];
        return;
    }
    W.recs++;
    if (r->event == EXTRACE_EV_T_OFF) {
        W.bad_config++;
        return;
    }
    if (r->event == EXTRACE_EV_T_SEQ || r->event == EXTRACE_EV_T_ISR) {
        unsigned int ctx;

        n = extrace_rec_ints(r, v, 16, nullptr);
        ctx = r->event == EXTRACE_EV_T_ISR ? 2u : (n >= 2 ? (unsigned int)v[1] : 99u);
        if (n < 1 || ctx >= NCTX_SEQ) {
            W.bad_payload++;
            return;
        }
        if (W.seen[ctx] && (uint32_t)v[0] <= W.last_seq[ctx]) {
            W.bad_order++;
            xt_log("order: ctx %u seq %u after %u\n", ctx, (unsigned)v[0], W.last_seq[ctx]);
        }
        W.seen[ctx] = true;
        W.last_seq[ctx] = (uint32_t)v[0];
        return;
    }
    if (r->event == EXTRACE_EV_T_BUF || r->event == EXTRACE_EV_T_ANY) {
        uint32_t seq;

        if (r->len < 4) {
            W.bad_payload++;
            return;
        }
        memcpy(&seq, r->payload, 4);
        for (uint32_t i = 4; i < r->len; i++)
            if (r->payload[i] != (uint8_t)(seq + i))
                W.bad_payload++;
    }
}

static void rsink_event(struct extrace_sink *s, uint8_t ring_id, enum extrace_ring_evt e,
                        const struct extrace_ring_evt_info *info)
{
    (void)s;
    (void)ring_id;
    hist_add();
    if (e == EXTRACE_RING_ATTACHED || (e == EXTRACE_RING_RESUMED && info->epoch != W.epoch)) {
        W.epoch = info->epoch;
        W.recs = 0;
        W.lost = 0;
    }
}

static enum extrace_sink_ret bsink_window(struct extrace_sink *s, uint8_t ring_id,
                                          const struct extrace_window *w)
{
    (void)s;
    (void)ring_id;
    if (W.bulk_valid && w->pos != W.bulk_end)
        W.bad_pos++;
    if (w->epoch == dbg_epoch) {
        uint32_t idx = w->start;

        for (unsigned int i = 0; i < w->nspan; i++) {
            const uint8_t *q = w->span[i].p, *end = q + w->span[i].len;

            while (q < end) {
                uint32_t l = extrace_rec_span_len(q, end);

                if (!l)
                    break;
                if (*q != 0x00 && *q != 0x04 && *q != EXTRACE_HDR_LOST && dbg_nseen < DBG_MAX)
                    dbg_seen[dbg_nseen++] = idx;
                q += l;
                idx += l;
            }
        }
    }
    if ((uint32_t)w->pos != w->start)
        W.bad_pos++;
    W.bulk_end = w->pos + extrace_window_len(w);
    W.bulk_valid = true;
    {
        uint32_t *e = end_of(w->epoch), base = extrace_test_index_base;

        if (w->end - base > *e - base)
            *e = w->end;
    }
    return EXTRACE_SINK_DONE;
}

static void bsink_event(struct extrace_sink *s, uint8_t ring_id, enum extrace_ring_evt e,
                        const struct extrace_ring_evt_info *info)
{
    (void)s;
    (void)ring_id;
    if (e == EXTRACE_RING_ATTACHED)
        dbg_nseen = 0;
    if (e == EXTRACE_RING_ATTACHED || e == EXTRACE_RING_RESUMED) {
        W.bulk_end = info->pos;
        W.bulk_valid = true;
    }
}

static const struct extrace_sink_ops rsink_ops = { .record = rsink_record,
                                                   .ring_event = rsink_event };
static const struct extrace_sink_ops bsink_ops = { .window = bsink_window,
                                                   .ring_event = bsink_event };

static int prod_init(void)
{
    bool big = wbsim_rand(2);
    struct extrace_prod_cfg cfg = {
        .block = W.pview, .block_size = BLOCK_SIZE, .data_size = big ? 1024u : 512u,
        .ring_id = 3, .name = big ? "big" : "small", .publish_thr = 64,
    };
    int r = extrace_prod_init(&cfg);

    if (r == 0) {
        W.inits++;
    }
    else if (r == -EXTRACE_EBUSY)
        W.busy++;
    return r;
}

static void emit_buf(uint32_t seq, bool direct)
{
    uint8_t b[40];
    uint32_t len = 4u + wbsim_rand(36);

    memcpy(b, &seq, 4);
    for (uint32_t i = 4; i < len; i++)
        b[i] = (uint8_t)(seq + i);
    if (!direct) {
        EXTRACE_BUF(T_BUF, b, len);
        return;
    }
    {
        struct extrace_resv r;
        uint32_t align = 1u << wbsim_rand(4);
        bool ok = align == 1 ? EXTRACE_RESERVE(T_ANY, len, 1, &r)
                  : align == 2 ? EXTRACE_RESERVE(T_ANY, len, 2, &r)
                  : align == 4 ? EXTRACE_RESERVE(T_ANY, len, 4, &r)
                               : EXTRACE_RESERVE(T_ANY, len, 8, &r);

        if (!ok)
            return;
        if (align > 1 && ((uintptr_t)r.payload & (align - 1u)))
            W.bad_payload++; /* misaligned payload */
        for (uint32_t i = 0; i < len; i += 8) { /* a slow fill: others run meanwhile */
            uint32_t k = len - i < 8 ? len - i : 8;

            memcpy((uint8_t *)r.payload + i, b + i, k);
            wbsim_yield();
        }
        if (wbsim_rand(8) == 0) {
            /* a reservation that never reaches the stream; the epoch cannot change
             * while it is open (I0) */
            if (extrace_producer.epoch != W.cancel_epoch) {
                W.cancel_epoch = extrace_producer.epoch;
                W.cancels = 0;
            }
            extrace_cancel(&r);
            W.cancels++;
        }
        else
            extrace_commit(&r);
    }
}

static void p1_body(void *arg)
{
    (void)arg;
    while (prod_init() != 0)
        wbsim_yield();
    for (unsigned int i = 0; i < W.ops; i++) {
        uint32_t op = wbsim_rand(100);

        if (op < 45) {
            EXTRACE_INTS(T_SEQ, W.seq[0]++, 0u);
        } else if (op < 55) {
            emit_buf(W.seq[0]++, false);
        } else if (op < 58) {
            EXTRACE_INTS(T_OFF, 1u); /* never enabled by the consumer */
        } else if (op < 62) {
            EXTRACE_MARK(T_MARK);
        } else if (op < 78) {
            extrace_prod_poll();
        } else if (op < 82) {
            extrace_prod_flush();
        } else if (op < 85) {
            emit_buf(W.seq[0]++, true);
        } else if (op < 87) {
            prod_init(); /* warm re-init; -EXTRACE_EBUSY while P2 is inside an emit */
        } else if (op < 88 && W.reboots) {
            /* core reset between protocol sequences: private state is lost */
            if (extrace_producer.inflight == 0 && !extrace_producer.publishing) {
                memset(&extrace_producer, 0, sizeof(extrace_producer));
                W.reboots_done++;
                while (prod_init() != 0)
                    wbsim_yield();
            }
        } else {
            wbsim_yield();
        }
        check_tail();
        check_mask();
    }
    W.stop = true;
    while (!W.p2_done || !W.isr_done)
        wbsim_yield();
    /* quiesce: report pending drops with one more record, then publish everything */
    for (;;) {
        extrace_prod_poll();
        if (extrace_producer.lost_pending == 0 && extrace_producer.committed == extrace_producer.head &&
            extrace_producer.published == extrace_producer.committed)
            break;
        EXTRACE_MARK(T_MARK);
        extrace_prod_flush();
        wbsim_yield();
    }
    W.final_epoch = extrace_producer.epoch;
    W.final_head = extrace_producer.published;
    extrace_prod_stats(&W.final_stats);
    W.prod_done = true;
}

static void p2_body(void *arg)
{
    (void)arg;
    while (!W.stop) {
        uint32_t op = wbsim_rand(10);

        if (op < 6)
            EXTRACE_INTS(T_SEQ, W.seq[1]++, 1u);
        else if (op < 7)
            emit_buf(W.seq[1]++, true);
        else if (op < 8)
            EXTRACE(T_NONE);
        else
            wbsim_yield();
        check_tail();
    }
    W.p2_done = true;
}

static void isr_body(void *arg)
{
    (void)arg;
    while (!W.stop) {
        wbsim_isr_enter();
        EXTRACE_INTS(T_ISR, W.seq[2]++);
        if (wbsim_rand(4) == 0)
            EXTRACE_MARK(T_MARK);
        check_tail();
        wbsim_isr_exit();
        for (uint32_t k = wbsim_rand(6); k; k--)
            wbsim_yield();
    }
    W.isr_done = true;
}

static void cons_setup(void)
{
    memset(&W.cons, 0, sizeof(W.cons));
    extrace_disp_init(&W.disp);
    extrace_cons_attach(&W.cons, W.cview, BLOCK_SIZE, W.mirror,
                        (uint16_t)(sizeof(W.mirror) / sizeof(W.mirror[0])), 0);
    extrace_disp_add_ring(&W.disp, &W.cons);
    memset(&W.rsink, 0, sizeof(W.rsink));
    W.rsink.ops = &rsink_ops;
    W.rsink.cls = EXTRACE_SINK_RECORD;
    W.rsink.kind_mask = 0xFF;
    extrace_sink_ring_all(&W.rsink);
    memset(&W.bsink, 0, sizeof(W.bsink));
    W.bsink.ops = &bsink_ops;
    W.bsink.cls = EXTRACE_SINK_BULK;
    extrace_sink_ring_all(&W.bsink);
    extrace_disp_add_sink(&W.disp, &W.rsink);
    extrace_disp_add_sink(&W.disp, &W.bsink);
}

static void dump_state(const char *who)
{
    fprintf(stderr,
            "%s stuck: prod epoch %08x head %u committed %u published %u tail_cache %u "
            "lost_pending %u inflight %u flags %x | cons attached %d epoch %08x tail %u "
            "final epoch %08x head %u | stop %d p2 %d isr %d done %d\n",
            who, extrace_producer.epoch, extrace_producer.head, extrace_producer.committed,
            extrace_producer.published, extrace_producer.tail_cache,
            extrace_producer.lost_pending, extrace_producer.inflight, extrace_producer.flags,
            extrace_cons_attached(&W.cons), W.cons.epoch, W.cons.tail, W.final_epoch,
            W.final_head, W.stop, W.p2_done, W.isr_done, W.prod_done);
    abort();
}

static void cons_body(void *arg)
{
    unsigned long spins = 0;

    (void)arg;
    cons_setup();
    for (;;) {
        uint32_t op = wbsim_rand(100);

        if (++spins > 2000000ul)
            dump_state("consumer");

        extrace_disp_poll(&W.disp, 32u + wbsim_rand(400));
        if (W.prod_done && extrace_cons_attached(&W.cons) && W.cons.epoch == W.final_epoch &&
            W.cons.tail == W.final_head)
            break;
        if (W.prod_done) {
            wbsim_yield();
            continue;
        }
        if (op < 2) {
            /* consumer restart between dispatch rounds: no snapshot survives */
            W.restarts++;
            cons_setup();
        } else if (op < 5) {
            enum extrace_ev_state st = (enum extrace_ev_state)wbsim_rand(3);

            uint32_t bit = 1u << EXTRACE_EV_T_SEQ, set = W.mirror[0] & ~bit;
            uint32_t clr = W.mirror[W.cons.mask_words] & ~bit;

            if (st == EXTRACE_EVS_OFF && wbsim_rand(2))
                st = EXTRACE_EVS_DEFAULT;
            hist_pre(set | (st == EXTRACE_EVS_ON ? bit : 0u), clr | (st == EXTRACE_EVS_OFF ? bit : 0u));
            extrace_cons_ev_set(&W.cons, EXTRACE_EV_T_SEQ, st);
        } else if (op < 8) {
            /*
             * Generations that differ in both words, so a torn read (set of one,
             * clr of another) gives a mask that matches neither.
             */
            const uint32_t on = 1u << EXTRACE_EV_T_OPT, off = 1u << EXTRACE_EV_T_STR;
            uint32_t pat = wbsim_rand(3);
            uint32_t set = pat == 0 ? on : 0u, clr = pat == 0 ? off : pat == 1 ? 0u : off;

            hist_pre(set, clr);
            extrace_cons_ev_set_many(&W.cons, &set, &clr, 1);
        } else if (op < 11) {
            extrace_cons_cmd(&W.cons, EXTRACE_CMD_SYNC << wbsim_rand(3));
        } else if (op < 13) {
            extrace_cons_set_notify(&W.cons, wbsim_rand(2) ? 0u : 128u);
        } else if (op < 15) {
            extrace_cons_set_publish_thr(&W.cons, wbsim_rand(2) ? 0u : 32u);
        } else {
            wbsim_yield();
        }
    }
}

static int run_world(uint64_t seed, const struct wbsim_params *prm, unsigned int ops,
                     bool random_mem, bool reboots, uint32_t index_base)
{
    int fails = 0;

    memset(&W, 0, sizeof(W));
    memset(&extrace_producer, 0, sizeof(extrace_producer));
    extrace_test_index_base = index_base;
    wbsim_reset(seed, prm);
    extrace_test_resv_hook = dbg_resv;
    extrace_test_tail_hook = tail_read;
    hist_put(2, 0, 0); /* what every attach to a new image starts with (A2) */
    nends = 0;
    dbg_epoch = 0;
    dbg_nres = dbg_nseen = 0;
    W.ops = ops;
    W.random_mem = random_mem;
    W.reboots = reboots;
    W.blk = wbsim_block_add(BLOCK_SIZE, EXTRACE_CTL_SIZE(EXTRACE_MASK_WORDS), 0, 1, random_mem);
    W.pview = wbsim_block_view(W.blk, 0);
    W.cview = wbsim_block_view(W.blk, 1);
    wbsim_ctx_add(0, WBSIM_THREAD, p1_body, nullptr);
    wbsim_ctx_add(0, WBSIM_THREAD, p2_body, nullptr);
    wbsim_ctx_add(0, WBSIM_ISR, isr_body, nullptr);
    wbsim_ctx_add(1, WBSIM_THREAD, cons_body, nullptr);
    if (wbsim_run() != 0)
        fails++;

    {
        const struct wbsim_stats *s = wbsim_get_stats();
        uint32_t dec_err = W.disp.ring[0].dec.errors + W.disp.stats.decode_errors;

        fails += !EXPECT_EQ(s->viol_foreign_store, 0);
        fails += !EXPECT_EQ(s->viol_plain_ctl, 0);
        fails += !EXPECT_EQ(s->viol_own_load, 0);
        fails += !EXPECT_EQ(W.bad_order, 0);
        fails += !EXPECT_EQ(W.bad_payload, 0);
        fails += !EXPECT_EQ(W.bad_config, 0);
        fails += !EXPECT_EQ(W.bad_pos, 0);
        fails += !EXPECT_EQ(W.bad_tail, 0);
        fails += !EXPECT_EQ(W.bad_mask, 0);
        fails += !EXPECT_EQ(dec_err, 0);
        fails += !EXPECT_EQ(W.cons.stats.head_errors, 0);
        fails += !EXPECT_EQ(W.epoch, W.final_epoch);
        fails += !EXPECT_EQ(W.recs + (W.cancel_epoch == W.final_epoch ? W.cancels : 0u),
                            W.final_stats.records);
        fails += !EXPECT_EQ(W.lost, W.final_stats.lost_total);
        xt_log("seed %llx: inits %u busy %u reboots %u restarts %u delivered %llu lost %llu "
               "switches %llu\n",
               (unsigned long long)seed, W.inits, W.busy, W.reboots_done, W.restarts,
               (unsigned long long)W.delivered, (unsigned long long)W.lost,
               (unsigned long long)s->switches);
    }
    if (fails) {
        fprintf(stderr, "  failing seed: 0x%llx\n", (unsigned long long)seed);
        for (unsigned int i = 0, j = 0; i < dbg_nres; i++) {
            while (j < dbg_nseen && dbg_seen[j] < dbg_res[i])
                j++;
            if (j >= dbg_nseen || dbg_seen[j] != dbg_res[i])
                fprintf(stderr, "  reservation at %u (#%u of %u) not seen\n", dbg_res[i], i,
                        dbg_nres);
        }
    }
    return fails;
}

TEST(lifecycle_one)
{
    const char *e = getenv("XT_WORLD");

    if (e && getenv("XT_HEAVY")) {
        struct wbsim_params p = *wbsim_default_params();

        p.p_switch = 65536 / 3;
        p.p_drain = 65536 / 32;
        p.p_refresh = 65536 / 32;
        p.p_ld_fresh = 65536 / 8;
        run_world(strtoull(e, nullptr, 0), &p, 250, true, true, 0);
    } else if (e) {
        run_world(strtoull(e, nullptr, 0), nullptr, 300, false, true, 0);
    }
}

TEST(lifecycle_random_zero_mem)
{
    uint64_t s = xt_seed();

    for (unsigned long i = 0; i < xt_iters(40); i++)
        if (run_world(xt_rand(&s), nullptr, 300, false, true, 0))
            return;
}

TEST(lifecycle_random_garbage_mem)
{
    uint64_t s = xt_seed() ^ 0x1234;

    for (unsigned long i = 0; i < xt_iters(40); i++)
        if (run_world(xt_rand(&s), nullptr, 300, true, true, 0))
            return;
}

TEST(lifecycle_index_wrap)
{
    uint64_t s = xt_seed() ^ 0x5678;

    for (unsigned long i = 0; i < xt_iters(20); i++)
        if (run_world(xt_rand(&s), nullptr, 300, false, false, 0xFFFFFF00u))
            return;
}

TEST(lifecycle_heavy_reordering)
{
    struct wbsim_params p = *wbsim_default_params();
    uint64_t s = xt_seed() ^ 0x9ABC;

    /* stores linger, loads are stale, switches are frequent */
    p.p_switch = 65536 / 3;
    p.p_drain = 65536 / 32;
    p.p_refresh = 65536 / 32;
    p.p_ld_fresh = 65536 / 8;
    for (unsigned long i = 0; i < xt_iters(30); i++)
        if (run_world(xt_rand(&s), &p, 250, true, true, 0))
            return;
}

TEST(lifecycle_timer_wrap)
{
    struct wbsim_params p = *wbsim_default_params();
    uint64_t s = xt_seed() ^ 0xDEF0;

    p.ts_start = 0xFFFFF000u;
    p.ts_step_max = 40;
    for (unsigned long i = 0; i < xt_iters(10); i++)
        if (run_world(xt_rand(&s), &p, 300, false, false, 0))
            return;
}
