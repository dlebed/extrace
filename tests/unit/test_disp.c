// SPDX-License-Identifier: MIT
/*
 * test_disp.c - dispatcher: bulk and record sinks, filters, admission and
 * back-pressure, lossy gaps, staged decoding, epoch changes, fairness and the
 * control API (DESIGN §12).
 */
#include "xt_util.h"
#include "extrace_disp.h"
#include "extrace_sink_mem.h"

static struct xt_ring A, B;
static struct extrace_disp D;
static struct extrace_sink_mem M;
static uint8_t mem_buf[1 << 16];

/* record collector */
struct coll {
    struct extrace_sink s;
    unsigned int n, events[64];
    uint32_t ev_epoch[64];          /* info->epoch of each event */
    uint16_t ev[2048];
    uint8_t ring[2048];
    uint64_t ts[2048];
};
static struct coll C;

static void coll_record(struct extrace_sink *s, const struct extrace_rec *r)
{
    struct coll *c = (struct coll *)s;

    if (c->n < 2048) {
        c->ev[c->n] = r->event;
        c->ring[c->n] = r->ring_id;
        c->ts[c->n] = r->ts;
    }
    c->n++;
}

static void coll_event(struct extrace_sink *s, uint8_t ring_id, enum extrace_ring_evt e,
                       const struct extrace_ring_evt_info *info)
{
    struct coll *c = (struct coll *)s;

    unsigned int i = c->events[0] + 1 < 64 ? ++c->events[0] : 0;

    (void)ring_id;
    c->events[i] = (unsigned int)e;
    c->ev_epoch[i] = info->epoch;
}

static const struct extrace_sink_ops coll_ops = { .record = coll_record, .ring_event = coll_event };

static void coll_init(void)
{
    memset(&C, 0, sizeof(C));
    C.s.ops = &coll_ops;
    C.s.cls = EXTRACE_SINK_RECORD;
    C.s.kind_mask = 0xFF;
    extrace_sink_ring_all(&C.s);
}

static unsigned int count_ev(uint16_t ev)
{
    unsigned int k = 0;

    for (unsigned int i = 0; i < C.n && i < 2048; i++)
        k += C.ev[i] == ev;
    return k;
}

/* Ring B: written once, then the producer moves on to ring A. */
static void setup_two_rings(void)
{
    xt_reset_world();
    memset(&A, 0, sizeof(A));
    memset(&B, 0, sizeof(B));
    {
        struct extrace_prod_cfg cfg = {
            .block = B.blk, .block_size = EXTRACE_RING_BLOCK_SIZE(2 * XT_DATA, EXTRACE_MASK_WORDS),
            .data_size = 2 * XT_DATA, .ring_id = 8, .name = "b",
        };

        B.block_size = cfg.block_size;
        EXPECT_EQ(extrace_prod_init(&cfg), 0);
        for (uint32_t i = 0; i < 10; i++)
            EXTRACE_INTS(T_SEQ, i, 8u);
        extrace_prod_flush();
    }
    EXPECT_EQ(xt_prod_start(&A, XT_DATA, 0), 0);
    extrace_disp_init(&D);
    extrace_cons_attach(&A.c, A.blk, A.block_size, A.mirror, XT_MIRROR_WORDS, 0);
    extrace_cons_attach(&B.c, B.blk, B.block_size, B.mirror, XT_MIRROR_WORDS, 0);
    EXPECT_EQ(extrace_disp_add_ring(&D, &A.c), 0);
    EXPECT_EQ(extrace_disp_add_ring(&D, &B.c), 1);
    EXPECT_EQ(extrace_disp_add_ring(&D, &B.c), -EXTRACE_EINVAL);
    extrace_sink_mem_init(&M, mem_buf, sizeof(mem_buf), false);
    coll_init();
    EXPECT_EQ(extrace_disp_add_sink(&D, &M.sink), 0);
    EXPECT_EQ(extrace_disp_add_sink(&D, &C.s), 0);
    EXPECT_EQ(extrace_disp_add_sink(&D, &C.s), -EXTRACE_EINVAL);
}

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint64_t le64(const uint8_t *b)
{
    return le32(b) | (uint64_t)le32(b + 4) << 32;
}

TEST(disp_bulk_and_record_sinks)
{
    const uint8_t *q;
    unsigned int frames = 0, info_frames = 0;
    uint64_t pos_a = 0;
    int n;

    setup_two_rings();
    for (uint32_t i = 0; i < 5; i++)
        EXTRACE_INTS(T_SEQ, i, 7u);
    extrace_prod_flush();
    n = extrace_disp_poll(&D, 1u << 20);
    EXPECT_TRUE(n > 0);
    EXPECT_EQ(count_ev(EXTRACE_EV_T_SEQ), 15);
    EXPECT_EQ(count_ev(EXTRACE_SYS_INFO), 2);
    EXPECT_EQ(C.events[0], 2); /* two ATTACHED */
    EXPECT_EQ(C.events[1], EXTRACE_RING_ATTACHED);
    /* frames: INFO (EPOCH_NEW) per ring, then data frames with BOUNDARY */
    for (q = mem_buf; q < mem_buf + M.used;) {
        uint16_t flags = (uint16_t)(q[6] | q[7] << 8);
        uint32_t len = le32(q + 12);

        EXPECT_EQ(le32(q), EXTRACE_BULK_MAGIC);
        EXPECT_EQ(q[4], EXTRACE_BULK_VERSION);
        if (flags & EXTRACE_BF_INFO) {
            info_frames++;
            EXPECT_TRUE(flags & EXTRACE_BF_EPOCH_NEW);
        } else {
            EXPECT_TRUE(flags & EXTRACE_BF_BOUNDARY);
            if (q[5] == 7) {
                EXPECT_EQ(le64(q + 16), pos_a);
                pos_a += len;
                EXPECT_EQ(le32(q + 8), A.c.epoch);
            }
        }
        frames++;
        q += EXTRACE_BULK_HDR_SIZE + len;
    }
    EXPECT_EQ(info_frames, 2);
    EXPECT_TRUE(frames >= 4);
    EXPECT_EQ(pos_a, A.c.tail);
    EXPECT_EQ(M.sink.stats.windows, frames - info_frames);
    EXPECT_EQ(extrace_disp_poll(&D, 1u << 20), 0);
    {
        struct extrace_disp_stats st;

        EXPECT_EQ(extrace_ctl_stats(&D, &st), 0);
        EXPECT_EQ(st.records, C.n);
        EXPECT_EQ(st.bytes, (uint64_t)A.c.tail + B.c.tail);
    }
}

TEST(disp_filters)
{
    uint32_t only_seq[1] = { 1u << EXTRACE_EV_T_SEQ };
    struct extrace_ev_map map = { .map = only_seq, .words = 1 };

    setup_two_rings();
    extrace_disp_poll(&D, 1u << 20); /* attach both */
    C.n = 0;
    EXPECT_EQ(extrace_ev_map_words(&D, 7), 1);
    EXPECT_EQ(extrace_ev_map_words(&D, 99), 0);
    EXPECT_EQ(extrace_ctl_sink_filter(&D, &C.s, 7, 1u << EXTRACE_K_INTS, &map), 0);
    EXPECT_EQ(extrace_ctl_sink_filter(&D, &C.s, 99, 0xFF, nullptr), -EXTRACE_ENOTREADY);
    EXPECT_EQ(extrace_ctl_sink_filter(&D, &C.s, EXTRACE_RING_ALL, 0xFF, nullptr), -EXTRACE_EINVAL);
    extrace_sink_ring_set(&C.s, 8, false);
    EXTRACE_INTS(T_SEQ, 1u, 7u);
    EXTRACE_INTS(T_INTS, 1u);
    EXTRACE_MARK(T_MARK);
    EXTRACE_BUF(T_BUF, "x", 1);
    extrace_prod_flush();
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(C.n, 1);
    EXPECT_EQ(C.ev[0], EXTRACE_EV_T_SEQ);
    EXPECT_EQ(C.ring[0], 7);
    /* a disabled sink sees nothing */
    EXPECT_EQ(extrace_ctl_sink_enable(&D, &C.s, false), 0);
    EXTRACE_INTS(T_SEQ, 2u, 7u);
    extrace_prod_flush();
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(C.n, 1);
    EXPECT_EQ(extrace_ctl_sink_enable(&D, &C.s, true), 0);
    EXPECT_EQ(extrace_disp_del_sink(&D, &C.s), 0);
    EXPECT_EQ(extrace_disp_del_sink(&D, &C.s), -EXTRACE_EINVAL);
}

TEST(disp_backpressure_and_gaps)
{
    static uint8_t small[256];
    struct extrace_sink_mem lossy;
    int n;

    setup_two_rings();
    extrace_disp_poll(&D, 1u << 20);
    /* lossless sink without room: nothing offered, nothing released */
    M.used = M.size - 40;
    EXTRACE_BUF(T_BUF, mem_buf, 100);
    extrace_prod_flush();
    n = extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(n, 0);
    EXPECT_EQ(D.stats.backpressure, 1);
    EXPECT_NE(A.c.tail, extrace_blk_pstate(A.blk)->head);
    extrace_sink_mem_reset(&M);
    EXPECT_TRUE(extrace_disp_poll(&D, 1u << 20) > 0);
    EXPECT_EQ(A.c.tail, extrace_blk_pstate(A.blk)->head);
    /* lossy sink: loses the window, flags GAP on the next frame */
    EXPECT_EQ(extrace_disp_del_sink(&D, &M.sink), 0);
    extrace_sink_mem_init(&lossy, small, sizeof(small), true);
    EXPECT_EQ(extrace_disp_add_sink(&D, &lossy.sink), 0);
    EXTRACE_BUF(T_BUF, mem_buf, 300);
    extrace_prod_flush();
    EXPECT_TRUE(extrace_disp_poll(&D, 1u << 20) > 0);
    EXPECT_EQ(lossy.sink.stats.gaps, 1);
    EXPECT_EQ(lossy.used, 0);
    EXTRACE_INTS(T_INTS, 1u);
    extrace_prod_flush();
    EXPECT_TRUE(extrace_disp_poll(&D, 1u << 20) > 0);
    ASSERT_TRUE(lossy.used > EXTRACE_BULK_HDR_SIZE);
    EXPECT_TRUE(small[6] & EXTRACE_BF_GAP);
}

TEST(disp_staged_decoding_large_windows)
{
    uint8_t b[300];
    unsigned int k = 0;

    setup_two_rings();
    extrace_disp_poll(&D, 1u << 20);
    C.n = 0;
    memset(b, 0x5A, sizeof(b));
    /* ~3.5 KiB in one window: several staging pieces, records carried over */
    for (uint32_t i = 0; i < 12; i++) {
        EXTRACE_BUF(T_BUF, b, 280);
        EXTRACE_INTS(T_SEQ, i, 7u);
        k += 2;
    }
    extrace_prod_flush();
    /* a window of up to half the ring per poll: about 2 KiB, two staging pieces */
    EXPECT_TRUE(extrace_disp_poll(&D, 1u << 20) > EXTRACE_CFG_DISP_STAGE);
    while (extrace_disp_poll(&D, 1u << 20) > 0)
        ;
    EXPECT_EQ(C.n, k);
    EXPECT_EQ(D.ring[0].dec.errors, 0);
}

TEST(disp_epoch_change_and_detach)
{
    unsigned int att;

    uint32_t ep;

    setup_two_rings();
    extrace_disp_poll(&D, 1u << 20);
    att = C.events[0];
    ep = A.c.epoch;
    EXPECT_EQ(xt_prod_start(&A, XT_DATA, 0), 0); /* producer restart */
    EXTRACE_INTS(T_SEQ, 1u, 7u);
    extrace_prod_flush();
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(C.events[0], att + 2);
    EXPECT_EQ(C.events[att + 1], EXTRACE_RING_DETACHED);
    EXPECT_EQ(C.ev_epoch[att + 1], ep); /* the identity that went away */
    EXPECT_EQ(C.events[att + 2], EXTRACE_RING_ATTACHED);
    EXPECT_EQ(C.ev_epoch[att + 2], A.c.epoch);
    EXPECT_NE(A.c.epoch, ep);
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(A.c.tail, extrace_blk_pstate(A.blk)->head);
    /* producer re-initializing: magic invalid -> detached */
    extrace_blk_pinfo(A.blk)->magic = 0;
    extrace_blk_pstate(A.blk)->head = A.c.tail + 3 * XT_DATA;
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(C.events[C.events[0]], EXTRACE_RING_DETACHED);
    EXPECT_EQ(extrace_ctl_ev_set(&D, 7, EXTRACE_EV_T_SEQ, EXTRACE_EVS_ON), -EXTRACE_ENOTREADY);
    EXPECT_EQ(xt_prod_start(&A, XT_DATA, 0), 0);
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(C.events[C.events[0]], EXTRACE_RING_ATTACHED);
}

/* A producer restart seen by poll_state() (ring not peeked yet) reports the same. */
TEST(disp_epoch_change_seen_at_poll_state)
{
    unsigned int att;

    setup_two_rings();
    extrace_disp_poll(&D, 1u << 20);
    D.ring[0].attached = 1;
    A.c.flags &= (uint8_t)~EXTRACE_CF_ATTACHED; /* forces the poll_state() path */
    att = C.events[0];
    EXPECT_EQ(xt_prod_start(&A, XT_DATA, 0), 0);
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(C.events[0], att + 2);
    EXPECT_EQ(C.events[att + 1], EXTRACE_RING_DETACHED);
    EXPECT_EQ(C.events[att + 2], EXTRACE_RING_ATTACHED);
}

TEST(disp_attach_failed_reported_once)
{
    unsigned int att;

    setup_two_rings();
    extrace_disp_poll(&D, 1u << 20);
    att = C.events[0];
    /* a new epoch whose INFO the consumer refuses */
    extrace_blk_pinfo(A.blk)->wire_ver = EXTRACE_WIRE_VERSION + 1;
    extrace_blk_pstate(A.blk)->epoch++;
    for (int i = 0; i < 4; i++)
        extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(C.events[0], att + 2);
    EXPECT_EQ(C.events[att + 1], EXTRACE_RING_DETACHED);
    EXPECT_EQ(C.events[att + 2], EXTRACE_RING_ATTACH_FAILED);
    EXPECT_FALSE(D.ring[0].attached);
    /* a good producer again: attached, and a later failure is reported again */
    EXPECT_EQ(xt_prod_start(&A, XT_DATA, 0), 0);
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(C.events[0], att + 3);
    EXPECT_EQ(C.events[att + 3], EXTRACE_RING_ATTACHED);
    extrace_blk_pinfo(A.blk)->wire_ver = EXTRACE_WIRE_VERSION + 1;
    extrace_blk_pstate(A.blk)->epoch++;
    extrace_disp_poll(&D, 1u << 20);
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(C.events[0], att + 5);
    EXPECT_EQ(C.events[att + 5], EXTRACE_RING_ATTACH_FAILED);
}

/*
 * The dispatcher's timer is read after the window is peeked: an anchor published
 * while the poll is under way must not be placed one timer wrap back (§12.5).
 */
static bool inj_armed;

static uint32_t inj_ts(void)
{
    uint32_t t = xt_ts();

    if (inj_armed) { /* first timer read of the poll: the producer emits a SYNC now */
        inj_armed = false;
        extrace_producer.sync_period = 1;
        extrace_prod_poll();
    }
    return t;
}

TEST(disp_timer_read_after_peek)
{
    unsigned int k = 0;

    setup_two_rings();
    while (extrace_disp_poll(&D, 1u << 20) > 0)
        ;
    C.n = 0;
    EXTRACE_INTS(T_SEQ, 1u, 7u); /* something to peek on ring A */
    extrace_prod_flush();
    extrace_posix_ts_hook = inj_ts;
    inj_armed = true;
    while (extrace_disp_poll(&D, 1u << 20) > 0)
        ;
    extrace_posix_ts_hook = xt_ts;
    for (unsigned int i = 0; i < C.n && i < 2048; i++) {
        if (C.ev[i] != EXTRACE_SYS_SYNC || C.ring[i] != 7)
            continue;
        k++;
        EXPECT_TRUE(C.ts[i] <= D.ts64); /* not on an earlier timer period */
        EXPECT_TRUE(C.ts[i] > 1000);
    }
    EXPECT_EQ(k, 1);
}

TEST(disp_nodecode_and_duplicates)
{
    struct extrace_cons dup;
    static uint64_t dup_blk[(XT_BLOCK_SIZE + 7) / 8];
    uint32_t dup_mirror[8];

    setup_two_rings();
    /* ring B announces records larger than the staging buffer */
    extrace_blk_pinfo(B.blk)->max_record = EXTRACE_CFG_DISP_STAGE + 4;
    extrace_cons_attach(&B.c, B.blk, B.block_size, B.mirror, XT_MIRROR_WORDS, 0);
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_FALSE(D.ring[1].decode); /* the dispatcher's decision, from its staging size */
    EXPECT_TRUE(D.ring[0].decode);
    EXPECT_EQ(count_ev(EXTRACE_EV_T_SEQ), 0); /* B's records are not decoded */
    EXPECT_EQ(B.c.tail, extrace_blk_pstate(B.blk)->head); /* but released */
    /* a second ring with ring_id 7 */
    memcpy(dup_blk, A.blk, sizeof(dup_blk));
    memset(&dup, 0, sizeof(dup));
    extrace_blk_cstate(dup_blk)->magic = 0;
    extrace_cons_attach(&dup, dup_blk, XT_BLOCK_SIZE, dup_mirror, 8, 0);
    EXPECT_EQ(extrace_disp_add_ring(&D, &dup), 2);
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_FALSE(D.ring[2].attached);
    EXPECT_EQ(C.events[C.events[0]], EXTRACE_RING_ATTACH_FAILED);
}

TEST(disp_fairness_and_budget)
{
    uint32_t a0, b0;

    setup_two_rings();
    extrace_disp_poll(&D, 1u << 20);
    for (uint32_t i = 0; i < 200; i++)
        EXTRACE_INTS(T_SEQ, i, 7u);
    extrace_prod_flush();
    a0 = A.c.tail;
    b0 = B.c.tail;
    EXPECT_EQ(extrace_disp_poll(&D, 64), 63); /* whole records within the budget */
    EXPECT_EQ(A.c.tail - a0, 63);
    EXPECT_EQ(B.c.tail, b0);
    EXPECT_TRUE(extrace_disp_poll(&D, 1u << 20) > 0);
    EXPECT_EQ(A.c.tail, extrace_blk_pstate(A.blk)->head);
}

TEST(disp_control_api)
{
    uint32_t set[1] = { 1u << EXTRACE_EV_T_OFF }, clr[1] = { 0 };
    enum extrace_ev_state st;
    struct extrace_ring_info info;

    setup_two_rings();
    EXPECT_EQ(extrace_ctl_ev_set(&D, 7, EXTRACE_EV_T_OFF, EXTRACE_EVS_ON), -EXTRACE_ENOTREADY);
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(extrace_ctl_ev_set(&D, EXTRACE_RING_ALL, 16, EXTRACE_EVS_ON), -EXTRACE_EINVAL);
    EXPECT_EQ(extrace_ctl_ev_set(&D, 7, EXTRACE_EV_T_INTS, EXTRACE_EVS_OFF), 0);
    EXPECT_EQ(extrace_ctl_ev_get(&D, 7, EXTRACE_EV_T_INTS, &st), 0);
    EXPECT_EQ(st, EXTRACE_EVS_OFF);
    EXPECT_EQ(extrace_ctl_ev_set_range(&D, 7, EXTRACE_EV_T_MARK, EXTRACE_EV_T_NONE,
                                       EXTRACE_EVS_OFF), 0);
    EXPECT_EQ(extrace_ctl_ev_set_range(&D, 7, 20, 10, EXTRACE_EVS_OFF), -EXTRACE_EINVAL);
    EXPECT_EQ(extrace_ctl_ev_get(&D, 7, EXTRACE_EV_T_NONE, &st), 0);
    EXPECT_EQ(st, EXTRACE_EVS_OFF);
    EXPECT_EQ(extrace_ctl_ev_applied(&D, 7), 0);
    extrace_prod_poll();
    EXPECT_EQ(extrace_ctl_ev_applied(&D, 7), 1);
    EXPECT_FALSE(EXTRACE_ENABLED(T_MARK));
    EXPECT_EQ(extrace_ctl_ev_set_map(&D, 7, set, clr, 1), 0);
    extrace_prod_poll();
    EXPECT_TRUE(EXTRACE_ENABLED(T_OFF));
    EXPECT_TRUE(EXTRACE_ENABLED(T_MARK));
    EXPECT_EQ(extrace_ctl_ev_reset(&D, EXTRACE_RING_ALL), 0);
    extrace_prod_poll();
    EXPECT_FALSE(EXTRACE_ENABLED(T_OFF));
    EXPECT_EQ(extrace_ctl_ev_applied(&D, 7), 1);
    EXPECT_EQ(extrace_ctl_ring_thr(&D, 7, 48, 64), 0);
    EXPECT_EQ(extrace_ctl_ring_cmd(&D, EXTRACE_RING_ALL, EXTRACE_CMD_SYNC), 0);
    extrace_prod_poll();
    EXPECT_EQ(extrace_producer.publish_thr, 48);
    EXPECT_EQ(extrace_producer.notify_thr, 64);
    EXPECT_EQ(A.c.publish_thr, 48);
    EXPECT_EQ(extrace_ctl_ring_cmd(&D, 99, EXTRACE_CMD_SYNC), -EXTRACE_ENOTREADY);
    EXPECT_EQ(extrace_ctl_ring_info(&D, 7, &info), 0);
    EXPECT_EQ(info.attached, 1);
    EXPECT_EQ(info.epoch, extrace_producer.epoch);
    EXPECT_EQ(info.ev_count, EXTRACE_EV_COUNT);
    EXPECT_EQ(info.status, EXTRACE_ST_RUNNING);
    EXPECT_EQ(info.hwm, extrace_blk_pstate(A.blk)->hwm);
    EXPECT_EQ(info.lost_total, extrace_blk_pstate(A.blk)->lost_total);
    EXPECT_EQ(info.pinfo.ring_id, 7);
    EXPECT_EQ(extrace_ctl_ring_info(&D, 99, &info), -EXTRACE_ENOTREADY);
    EXPECT_EQ(extrace_ctl_ring_info(&D, EXTRACE_RING_ALL, &info), -EXTRACE_EINVAL);
}

TEST(bulk_frame_and_info_serialization)
{
    struct extrace_bulk_hdr h = {
        .magic = EXTRACE_BULK_MAGIC, .version = 1, .ring_id = 3, .flags = 0x0105,
        .epoch = 0x11223344, .len = 77, .pos = 0x0102030405060708ull, .disp_ts = 9,
    };
    struct extrace_shm_pinfo pi = { .wire_ver = 1, .layout_ver = 0x200, .ring_id = 3,
                                    .data_size = 4096, .ev_count = 60, .name = "abc" };
    uint8_t b[EXTRACE_BULK_HDR_SIZE], s[96];
    uint32_t n, v = 0;
    const uint8_t *q = s;

    extrace_bulk_hdr_put(b, &h);
    EXPECT_MEM(b, "EXBF", 4);
    EXPECT_EQ(b[6], 0x05);
    EXPECT_EQ(b[7], 0x01);
    EXPECT_EQ(le32(b + 8), 0x11223344);
    EXPECT_EQ(le64(b + 16), 0x0102030405060708ull);
    n = extrace_pinfo_serialize(&pi, 5, s, sizeof(s));
    EXPECT_TRUE(n > 30);
    q += extrace_get_uv32(q, s + n, &v);
    EXPECT_EQ(v, 12);
    EXPECT_MEM(s + n - 3, "abc", 3);
    EXPECT_EQ(extrace_pinfo_serialize(&pi, 5, s, 10), 0);
}
