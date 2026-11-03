// SPDX-License-Identifier: MIT
/*
 * test_interleave.c - directed interleavings on the write-buffer simulator
 * (DESIGN §7.6.1, §18.4).
 *
 * Scripted schedules stop one side after every number of shared-memory steps
 * and run the other side for every number of steps, under several seeds for the
 * drain and refresh decisions:
 *
 *  - producer re-init (I0-I6) against consumer attach (A1-A4): an attach must
 *    see a pinfo snapshot that belongs to the epoch it attached to, never a torn
 *    mix of two inits;
 *  - producer restart against consumer peek/release (C1-C7): no record of the
 *    new epoch may reach a record sink as one of the old epoch, and no window is
 *    released into the new epoch;
 *  - producer re-init against a reservation in the slow path and against a
 *    preempted publish: init refuses (-EXTRACE_EBUSY) or the reservation fails,
 *    and no tail or head of the old epoch enters the new one.
 */
#include <stdio.h>
#include <string.h>
#include "extrace_test.h"
#include "extrace.h"
#include "extrace_cons.h"
#include "extrace_disp.h"
#include "extrace_test_hooks.h"
#include "wbsim.h"

#define BLOCK EXTRACE_RING_BLOCK_SIZE(1024u, EXTRACE_MASK_WORDS)

static struct {
    uint8_t *pv, *cv;
    struct extrace_cons c;
    uint32_t mirror[4];
    struct extrace_disp d;
    struct extrace_sink s;
    uint32_t e_old, e_new;
    int init_ret;
    bool cons_done, prod_done;
    uint32_t bad;
    uint32_t seq_seen, recs;
} X;

static int prod_init(uint32_t size, const char *name)
{
    struct extrace_prod_cfg cfg = { .block = X.pv, .block_size = BLOCK, .data_size = size,
                                    .ring_id = 5, .name = name };

    return extrace_prod_init(&cfg);
}

static void world(uint64_t seed, bool random_mem)
{
    memset(&X, 0, sizeof(X));
    memset(&extrace_producer, 0, sizeof(extrace_producer));
    extrace_test_index_base = 0;
    extrace_test_resv_hook = nullptr;
    wbsim_reset(seed, nullptr);
    wbsim_block_add(BLOCK, EXTRACE_CTL_SIZE(EXTRACE_MASK_WORDS), 0, 1, random_mem);
    X.pv = wbsim_block_view(0, 0);
    X.cv = wbsim_block_view(0, 1);
}

/* ---- init against attach ---- */

static void ia_prod(void *arg)
{
    (void)arg;
    X.init_ret = prod_init(1024, "new-ring");
    X.e_new = extrace_producer.epoch;
    X.prod_done = true;
}

static void ia_cons(void *arg)
{
    (void)arg;
    for (unsigned int i = 0; i < 400; i++) {
        if (extrace_cons_attach(&X.c, X.cv, BLOCK, X.mirror, 4, 0) == 0) {
            /* the snapshot must belong to the epoch it was attached with */
            bool is_new = X.c.epoch == X.e_new || (!X.prod_done && X.c.epoch != X.e_old);
            uint32_t want = is_new ? 1024u : 512u;

            if (X.c.pinfo.data_size != want ||
                memcmp(X.c.pinfo.name, is_new ? "new-ring" : "old", is_new ? 8 : 4) != 0) {
                X.bad++;
                xt_log("torn: epoch %08x (old %08x) size %u name %.8s\n", X.c.epoch, X.e_old,
                       X.c.pinfo.data_size, X.c.pinfo.name);
            }
            if (X.prod_done && X.c.epoch == X.e_new)
                break;
            memset(&X.c, 0, sizeof(X.c)); /* attach again later */
        }
        wbsim_yield();
    }
    X.cons_done = true;
}

static void ia_setup_old(void *arg)
{
    (void)arg;
    if (prod_init(512, "old") == 0)
        X.e_old = extrace_producer.epoch;
}

TEST(interleave_init_vs_attach)
{
    unsigned int runs = 0;

    for (uint32_t k = 0; k < 120; k += 1) {
        for (uint32_t j = 1; j < 60; j += 7) {
            for (uint64_t s = 1; s <= 3; s++) {
                const struct wbsim_seg script[] = { { 0, k }, { 1, j }, { 0, 100000 } };

                world(xt_seed() + k * 1000 + j * 10 + s, s == 3);
                wbsim_ctx_add(0, WBSIM_THREAD, ia_setup_old, nullptr);
                if (wbsim_run() != 0)
                    return;
                /* second run: the old epoch exists, now race init and attach */
                wbsim_ctx_clear();
                wbsim_ctx_add(0, WBSIM_THREAD, ia_prod, nullptr);
                wbsim_ctx_add(1, WBSIM_THREAD, ia_cons, nullptr);
                wbsim_script(script, 3);
                ASSERT_EQ(wbsim_run(), 0);
                runs++;
                if (!EXPECT_EQ(X.bad, 0) || !EXPECT_EQ(X.init_ret, 0) ||
                    !EXPECT_EQ(X.c.epoch, X.e_new) ||
                    !EXPECT_EQ(wbsim_get_stats()->viol_foreign_store +
                                   wbsim_get_stats()->viol_plain_ctl +
                                   wbsim_get_stats()->viol_own_load,
                               0)) {
                    fprintf(stderr, "  k %u j %u seed %llu\n", k, j, (unsigned long long)s);
                    return;
                }
            }
        }
    }
    xt_log("init vs attach: %u schedules\n", runs);
}

/* ---- restart against peek/release ---- */

static void pr_record(struct extrace_sink *s, const struct extrace_rec *r)
{
    uint64_t v[4];

    (void)s;
    if (r->event != EXTRACE_EV_T_SEQ)
        return;
    X.recs++;
    if (extrace_rec_ints(r, v, 4, nullptr) != 2 || v[1] != 0) {
        X.bad++;
        return;
    }
    /* sequence numbers of the old epoch are below 1000, of the new one above */
    if (X.c.epoch == X.e_old && v[0] >= 1000) {
        X.bad++;
        xt_log("new-epoch record delivered as old: seq %llu\n", (unsigned long long)v[0]);
    }
    if (X.seq_seen && v[0] < 1000 && v[0] <= X.seq_seen) {
        X.bad++; /* duplicate or reordered */
    }
    if (v[0] < 1000)
        X.seq_seen = (uint32_t)v[0];
}

static const struct extrace_sink_ops pr_ops = { .record = pr_record };

static void pr_prod(void *arg)
{
    (void)arg;
    if (prod_init(512, "old") != 0)
        return;
    X.e_old = extrace_producer.epoch;
    for (uint32_t i = 1; i < 60; i++)
        EXTRACE_INTS(T_SEQ, i, 0u);
    extrace_prod_flush();
    /* restart: the new epoch overwrites the ring from offset 0 */
    while ((X.init_ret = prod_init(512, "old")) != 0)
        wbsim_yield();
    X.e_new = extrace_producer.epoch;
    for (uint32_t i = 1000; i < 1070; i++)
        EXTRACE_INTS(T_SEQ, i, 0u);
    extrace_prod_flush();
    X.prod_done = true;
}

static void pr_cons(void *arg)
{
    (void)arg;
    extrace_disp_init(&X.d);
    X.s.ops = &pr_ops;
    X.s.cls = EXTRACE_SINK_RECORD;
    X.s.kind_mask = 0xFF;
    extrace_sink_ring_all(&X.s);
    extrace_disp_add_sink(&X.d, &X.s);
    extrace_cons_attach(&X.c, X.cv, BLOCK, X.mirror, 4, 0);
    extrace_disp_add_ring(&X.d, &X.c);
    for (unsigned int i = 0; i < 200000; i++) {
        extrace_disp_poll(&X.d, 40);
        if (X.prod_done && extrace_cons_attached(&X.c) && X.c.epoch == X.e_new &&
            X.c.tail == extrace_producer.published)
            break;
        wbsim_yield();
    }
    /* the released tail of the new epoch never covers unpublished bytes */
    if (X.c.epoch == X.e_new && X.c.tail - 0u > extrace_producer.published)
        X.bad++;
    X.cons_done = true;
}

TEST(interleave_restart_vs_read)
{
    for (uint32_t k = 0; k < 400; k += 3) {
        for (uint64_t s = 1; s <= 4; s++) {
            const struct wbsim_seg script[] = { { 1, 2 + (uint32_t)s * 5 }, { 0, k },
                                                { 1, 40u * (uint32_t)s } };

            world(xt_seed() ^ (k * 77 + s), s == 4);
            wbsim_ctx_add(0, WBSIM_THREAD, pr_prod, nullptr);
            wbsim_ctx_add(1, WBSIM_THREAD, pr_cons, nullptr);
            wbsim_script(script, 3);
            ASSERT_EQ(wbsim_run(), 0);
            if (!EXPECT_EQ(X.bad, 0) || !EXPECT_EQ(X.c.epoch, X.e_new) ||
                !EXPECT_EQ(X.d.ring[0].dec.errors + X.d.stats.decode_errors, 0)) {
                fprintf(stderr, "  k %u seed %llu\n", k, (unsigned long long)s);
                return;
            }
        }
    }
}

/* ---- re-init against a slow-path reservation and a preempted publish ---- */

static void sp_filler(void *arg)
{
    (void)arg;
    /* fill the ring: the reservations take the slow path and read the tail */
    for (uint32_t i = 0; i < 200; i++)
        EXTRACE_INTS(T_SEQ, i, 1u);
    extrace_prod_flush();
}

static void sp_reinit(void *arg)
{
    (void)arg;
    X.init_ret = prod_init(512, "old");
    X.e_new = extrace_producer.epoch;
    /* move head past the old consumer tail, so a stale tail would pass R3 */
    for (uint32_t i = 0; i < 40; i++)
        EXTRACE_INTS(T_SEQ, 2000 + i, 2u);
    X.prod_done = true;
}

static void sp_setup_prod(void *arg)
{
    (void)arg;
    if (prod_init(512, "old") != 0)
        return;
    X.e_old = extrace_producer.epoch;
    for (uint32_t i = 0; i < 15; i++)
        EXTRACE_INTS(T_SEQ, i, 0u);
    extrace_prod_flush();
    X.prod_done = true;
}

static void sp_setup_cons(void *arg)
{
    (void)arg;
    while (!X.prod_done || extrace_cons_attach(&X.c, X.cv, BLOCK, X.mirror, 4, 0) != 0)
        wbsim_yield();
    while (X.c.tail != extrace_producer.published) { /* consume the old epoch */
        uint8_t b[256];

        extrace_cons_read(&X.c, b, sizeof(b));
        wbsim_yield();
    }
}

TEST(interleave_reinit_vs_slow_path_and_publish)
{
    for (uint32_t k = 0; k < 900; k += 4) {
        for (uint64_t s = 1; s <= 2; s++) {
            const struct wbsim_seg script[] = { { 0, k }, { 1, 100000 } };
            const struct extrace_shm_cstate *cs;
            bool att;

            world(xt_seed() ^ (k * 131 + s), false);
            wbsim_ctx_add(0, WBSIM_THREAD, sp_setup_prod, nullptr);
            wbsim_ctx_add(1, WBSIM_THREAD, sp_setup_cons, nullptr);
            ASSERT_EQ(wbsim_run(), 0);
            ASSERT_TRUE(X.c.tail > 64);
            X.prod_done = false;
            /* the consumer stays attached to the old epoch: no consumer context */
            wbsim_ctx_clear();
            wbsim_ctx_add(0, WBSIM_THREAD, sp_filler, nullptr);
            wbsim_ctx_add(0, WBSIM_THREAD, sp_reinit, nullptr);
            wbsim_script(script, 2);
            ASSERT_EQ(wbsim_run(), 0);
            /*
             * Re-init either refused (a reservation or publish was open) or
             * produced a new epoch; in the new epoch the producer never trusts a
             * tail the consumer did not store for it.
             */
            EXPECT_TRUE(X.init_ret == 0 || X.init_ret == -EXTRACE_EBUSY);
            cs = extrace_blk_cstate(X.cv);
            att = cs->magic == EXTRACE_SHM_MAGIC && cs->attached_epoch == extrace_producer.epoch;
            if (!att && !EXPECT_EQ(extrace_producer.tail_cache, 0)) {
                fprintf(stderr, "  k %u seed %llu\n", k, (unsigned long long)s);
                return;
            }
            if (att && !EXPECT_TRUE(cs->tail - extrace_producer.tail_cache <= 512)) {
                fprintf(stderr, "  k %u seed %llu\n", k, (unsigned long long)s);
                return;
            }
            EXPECT_EQ(extrace_producer.inflight, 0);
            EXPECT_EQ(extrace_producer.publishing, 0);
        }
    }
}

/* ---- consumer attach against producer tail reads (R1, poll) ---- */

static uint32_t at_bad_reads;

/* Nothing was released in the new epoch: any tail read for it but 0 is stale. */
static void at_tail_hook(uint32_t epoch, uint32_t t)
{
    if (epoch == X.e_new && t != 0) {
        at_bad_reads++;
        xt_log("stale tail %u read for the new epoch\n", t);
    }
}

static void at_prod(void *arg)
{
    (void)arg;
    /* the ring is full: every emit takes the slow path and reads the tail */
    for (uint32_t i = 0; i < 400 && !X.cons_done; i++) {
        EXTRACE_INTS(T_SEQ, i, 1u);
        if (i % 8 == 0)
            extrace_prod_poll();
    }
    X.prod_done = true;
}

static void at_cons(void *arg)
{
    (void)arg;
    while (extrace_cons_poll_state(&X.c) < 0 || X.c.epoch != X.e_new)
        wbsim_yield();
    X.cons_done = true;
}

TEST(interleave_attach_vs_tail_read)
{
    for (uint32_t k = 0; k < 600; k += 3) {
        for (uint64_t s = 1; s <= 2; s++) {
            const struct wbsim_seg script[] = { { 0, k }, { 1, 100000 } };

            world(xt_seed() ^ (k * 7919 + s), false);
            /* old epoch: consumed far enough that its tail differs from 0 */
            wbsim_ctx_add(0, WBSIM_THREAD, sp_setup_prod, nullptr);
            wbsim_ctx_add(1, WBSIM_THREAD, sp_setup_cons, nullptr);
            ASSERT_EQ(wbsim_run(), 0);
            ASSERT_TRUE(X.c.tail > 64);
            /* new epoch, filled while the consumer still sits on the old one */
            wbsim_ctx_clear();
            X.prod_done = false;
            wbsim_ctx_add(0, WBSIM_THREAD, sp_reinit, nullptr);
            ASSERT_EQ(wbsim_run(), 0);
            ASSERT_EQ(X.init_ret, 0);
            for (uint32_t i = 0; i < 80; i++)
                EXTRACE_INTS(T_SEQ, i, 3u); /* not a context: no hooks run */
            /* race: the consumer attaches at step k of the producer's slow paths */
            wbsim_ctx_clear();
            X.prod_done = X.cons_done = false;
            at_bad_reads = 0;
            extrace_test_tail_hook = at_tail_hook;
            wbsim_ctx_add(0, WBSIM_THREAD, at_prod, nullptr);
            wbsim_ctx_add(1, WBSIM_THREAD, at_cons, nullptr);
            wbsim_script(script, 2);
            ASSERT_EQ(wbsim_run(), 0);
            extrace_test_tail_hook = nullptr;
            if (!EXPECT_EQ(at_bad_reads, 0)) {
                fprintf(stderr, "  k %u seed %llu\n", k, (unsigned long long)s);
                return;
            }
        }
    }
}
