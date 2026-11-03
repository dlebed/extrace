// SPDX-License-Identifier: MIT
/*
 * test_prod.c - producer: init, every emitter and wire kind, reservation, wrap,
 * overflow accounting, publication, direct reservations, poll, panic, index and
 * timer wrap (DESIGN §7, §8, §18.1).
 */
#include <stdio.h>
#include "xt_util.h"
#include "extrace_ftrace.h"

static struct xt_ring R;

static const struct xt_rec *find(int n, uint16_t ev, int nth)
{
    for (int i = 0; i < n; i++)
        if (xt_recs[i].r.event == ev && nth-- == 0)
            return &xt_recs[i];
    return nullptr;
}

/* ---- init ---- */

TEST(init_rejects_bad_config)
{
    struct extrace_prod_cfg cfg = { .block = R.blk, .block_size = sizeof(R.blk), .data_size = 4096 };

    xt_reset_world();
    EXPECT_EQ(extrace_prod_init(nullptr), -EXTRACE_EINVAL);
    cfg.block = (uint8_t *)R.blk + 4;
    EXPECT_EQ(extrace_prod_init(&cfg), -EXTRACE_EINVAL); /* not 8-byte aligned */
    cfg.block = R.blk;
    cfg.data_size = 3000;
    EXPECT_EQ(extrace_prod_init(&cfg), -EXTRACE_EINVAL); /* not a power of two */
    cfg.data_size = 64;
    while (cfg.data_size * 2 < 4 * EXTRACE_MAX_RESV)
        cfg.data_size *= 2;
    EXPECT_EQ(extrace_prod_init(&cfg), -EXTRACE_EINVAL); /* < 4 x EXTRACE_MAX_RESV */
    cfg.data_size = 4096;
    cfg.block_size = 4096;
    EXPECT_EQ(extrace_prod_init(&cfg), -EXTRACE_EINVAL); /* no room for the control part */
    cfg.block_size = sizeof(R.blk);
    cfg.ring_id = 255;
    EXPECT_EQ(extrace_prod_init(&cfg), -EXTRACE_EINVAL);
    cfg.ring_id = 1;
    EXPECT_EQ(extrace_prod_init(&cfg), 0);
    EXPECT_EQ(extrace_producer.size, 4096);
}

TEST(init_largest_data_size_fits)
{
    struct extrace_prod_cfg cfg = { .block = R.blk, .block_size = 2 * XT_DATA + 1000 };

    xt_reset_world();
    EXPECT_EQ(extrace_prod_init(&cfg), 0);
    EXPECT_EQ(extrace_producer.size, 2 * XT_DATA);
    /* a block sized with EXTRACE_RING_BLOCK_SIZE_MAX gets exactly the data size */
    cfg.block_size = EXTRACE_RING_BLOCK_SIZE_MAX(XT_DATA);
    EXPECT_EQ(extrace_prod_init(&cfg), 0);
    EXPECT_EQ(extrace_producer.size, XT_DATA);
}

TEST(init_writes_pinfo_and_first_records)
{
    struct extrace_shm_pinfo *pi = extrace_blk_pinfo(R.blk);
    struct extrace_shm_pstate *ps = extrace_blk_pstate(R.blk);
    struct extrace_dec d;
    const uint8_t *q;
    uint32_t v = 0, nf = 0;
    int n;

    xt_setup(&R, XT_DATA, 0);
    EXPECT_EQ(pi->magic, EXTRACE_SHM_MAGIC);
    EXPECT_EQ(pi->layout_ver, EXTRACE_LAYOUT_VERSION);
    EXPECT_EQ(pi->data_off, EXTRACE_CTL_SIZE(EXTRACE_MASK_WORDS));
    EXPECT_EQ(pi->data_size, XT_DATA);
    EXPECT_EQ(pi->ev_count, EXTRACE_EV_COUNT);
    EXPECT_EQ(pi->max_record, EXTRACE_MAX_RECORD);
    EXPECT_EQ(pi->ring_id, 7);
    EXPECT_MEM(pi->name, "xt0", 4);
    EXPECT_EQ(ps->status, EXTRACE_ST_RUNNING);
    EXPECT_NE(ps->epoch, 0);
    EXPECT_EQ(R.c.pinfo.data_size, XT_DATA);
    EXPECT_EQ(xt_notifies, 1); /* the doorbell at I6 */

    extrace_dec_reset(&d, 7, 0);
    n = xt_drain(&R, &d, true);
    ASSERT_TRUE(n >= 2);
    /* INFO then SYNC open every epoch */
    EXPECT_EQ(xt_recs[0].r.event, EXTRACE_SYS_INFO);
    EXPECT_EQ(xt_recs[0].r.kind, EXTRACE_K_BUF);
    EXPECT_FALSE(xt_recs[0].r.flags & EXTRACE_REC_HAS_TS);
    EXPECT_EQ(xt_recs[1].r.event, EXTRACE_SYS_SYNC);
    EXPECT_TRUE(xt_recs[1].r.flags & EXTRACE_REC_HAS_TS);
    q = xt_recs[0].payload;
    q += extrace_get_uv32(q, q + 8, &nf);
    EXPECT_EQ(nf, 13);
    q += extrace_get_uv32(q, q + 8, &v);
    EXPECT_EQ(v, EXTRACE_WIRE_VERSION);
    q += extrace_get_uv32(q, q + 8, &v);
    EXPECT_EQ(v, EXTRACE_LAYOUT_VERSION);
    q += extrace_get_uv32(q, q + 8, &v);
    EXPECT_EQ(v, EXTRACE_PI_TS_GLOBAL);
    q += extrace_get_uv32(q, q + 8, &v);
    EXPECT_EQ(v, 1000000);
    q += extrace_get_uv32(q, q + 8, &v);
    EXPECT_EQ(v, 7);
    q += extrace_get_uv32(q, q + 8, &v);
    EXPECT_EQ(v, extrace_producer.epoch);
    for (int i = 0; i < 4; i++) /* ring_size, ev_count, ptr_size, site_id_bits */
        q += extrace_get_uv32(q, q + 8, &v);
    for (int i = 0; i < 2; i++) { /* build_id, name */
        q += extrace_get_uv32(q, q + 8, &v);
        q += v;
    }
    q += extrace_get_uv32(q, q + 8, &v);
    EXPECT_EQ(v, (uint32_t)EXTRACE_CFG_FTRACE_BASE);
    EXPECT_EQ(q, xt_recs[0].payload + xt_recs[0].r.len);
}

TEST(init_epochs_never_repeat_the_acknowledged_one)
{
    uint32_t e1, e2;

    xt_setup(&R, XT_DATA, 0);
    e1 = extrace_producer.epoch;
    /* force the next candidate to collide with the acknowledged epoch */
    extrace_shm_st32(&extrace_blk_cstate(R.blk)->attached_epoch, e1 + 1);
    EXPECT_EQ(xt_prod_start(&R, XT_DATA, 0), 0);
    e2 = extrace_producer.epoch;
    EXPECT_NE(e2, e1 + 1);
    EXPECT_NE(e2, 0);
    EXPECT_NE(e2, e1);
}

TEST(init_refuses_while_busy)
{
    struct extrace_resv rv;

    xt_setup(&R, XT_DATA, 0);
    ASSERT_TRUE(EXTRACE_RESERVE(T_BUF, 8, 1, &rv));
    EXPECT_EQ(xt_prod_start(&R, XT_DATA, 0), -EXTRACE_EBUSY);
    memset(rv.payload, 1, 8);
    extrace_commit(&rv);
    extrace_producer.publishing = 1; /* a publish preempted by the re-init */
    EXPECT_EQ(xt_prod_start(&R, XT_DATA, 0), -EXTRACE_EBUSY);
    extrace_producer.publishing = 0;
    EXPECT_EQ(xt_prod_start(&R, XT_DATA, 0), 0);
    EXPECT_EQ(extrace_producer.stats.init_busy, 2);
}

/* ---- emitters and wire kinds ---- */

TEST(emit_golden_ints)
{
    struct extrace_window w;

    xt_setup(&R, XT_DATA, 0);
    xt_drain_user(&R); /* skip INFO and SYNC */
    xt_step = 12;
    extrace_producer.last_ts = xt_now; /* the next record is 12 ticks later */
    EXTRACE_INTS(T_INTS, 5u, -3);
    extrace_prod_flush();
    ASSERT_EQ(extrace_cons_peek(&R.c, &w, 1u << 20), 7);
    /* §5.11 with ID 18: A2 -> 92 */
    EXPECT_MEM(w.span[0].p, "\x92\x01\x0C\x03\x02\x05\x05", 7);
}

TEST(emit_every_kind)
{
    struct test_struct ts = { .a = 1, .b = 2, .c = 3, .d = 0x1122334455667788ull };
    const struct xt_rec *x;
    uint8_t big[700];
    int n;

    xt_setup(&R, XT_DATA, 0);
    for (unsigned int i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)i;
    EXTRACE_MARK(T_MARK);
    EXTRACE(T_NONE);
    EXTRACE_INTS(T_INTS, 7u);
    EXTRACE_INTS(T_INTS, 1u, 2u);
    EXTRACE_INTS(T_INTS, 1u, -2, 3u);
    EXTRACE_INTS(T_INTS, 1u, 2u, 3u, -4);
    EXTRACE_INTS(T_INTS, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, -15);
    EXTRACE_INTS(T_INTS, 0xFFFFFFFFu, 300u, 1ull << 40, -5ll);
    EXTRACE_BUF(T_BUF, big, 10);
    EXTRACE_BUF(T_BUF, big, sizeof(big)); /* truncated to MAX_PAYLOAD */
    EXTRACE_STRUCT(T_STRUCT, &ts);
    EXTRACE_STRUCT_N(T_STRUCT, &ts, 4);
    EXTRACE_STR(T_STR, "hello");
    EXTRACE_BUF(T_ANY, big, 3);
    n = xt_drain_user(&R);
    ASSERT_EQ(n, 14);

    EXPECT_EQ(xt_recs[0].r.kind, EXTRACE_K_MARK);
    EXPECT_EQ(xt_recs[0].r.event, EXTRACE_EV_T_MARK);
    EXPECT_EQ(xt_recs[1].r.kind, EXTRACE_K_NONE);
    x = &xt_recs[2];
    EXPECT_EQ(xt_nints(x), 1);
    EXPECT_EQ(xt_int(x, 0), 7);
    EXPECT_EQ(xt_nints(&xt_recs[3]), 2);
    x = &xt_recs[4];
    EXPECT_EQ(xt_int(x, 1), (uint64_t)-2);
    EXPECT_EQ(xt_int(x, 2), 3);
    EXPECT_EQ(xt_int(&xt_recs[5], 3), (uint64_t)-4);
    x = &xt_recs[6];
    EXPECT_EQ(xt_nints(x), 16);
    EXPECT_EQ(xt_int(x, 14), 14);
    EXPECT_EQ(xt_int(x, 15), (uint64_t)-15);
    x = &xt_recs[7];
    EXPECT_EQ(xt_int(x, 0), 0xFFFFFFFFu);
    EXPECT_EQ(xt_int(x, 1), 300);
    EXPECT_EQ(xt_int(x, 2), 1ull << 40);
    EXPECT_EQ(xt_int(x, 3), (uint64_t)-5ll);
    EXPECT_EQ(xt_recs[8].r.len, 10);
    EXPECT_MEM(xt_recs[8].payload, big, 10);
    EXPECT_EQ(xt_recs[9].r.len, EXTRACE_CFG_MAX_PAYLOAD);
    EXPECT_MEM(xt_recs[9].payload, big, EXTRACE_CFG_MAX_PAYLOAD);
    EXPECT_EQ(xt_recs[10].r.kind, EXTRACE_K_STRUCT);
    EXPECT_EQ(xt_recs[10].r.len, sizeof(ts));
    EXPECT_MEM(xt_recs[10].payload, &ts, sizeof(ts));
    EXPECT_EQ(xt_recs[11].r.len, 4);
    EXPECT_EQ(xt_recs[12].r.kind, EXTRACE_K_STR);
    EXPECT_EQ(xt_recs[12].r.len, 5);
    EXPECT_MEM(xt_recs[12].payload, "hello", 5);
    EXPECT_EQ(xt_recs[13].r.kind, EXTRACE_K_BUF);
    EXPECT_EQ(xt_recs[13].r.event, EXTRACE_EV_T_ANY);
    /* timestamps never go backwards */
    for (int i = 1; i < n; i++)
        EXPECT_TRUE(xt_recs[i].r.ts >= xt_recs[i - 1].r.ts);
}

/* INTS that cannot fit a small MAX_PAYLOAD is dropped and counted, never written. */
TEST(emit_ints_oversize)
{
    int64_t big = -(1ll << 62);
    int n;

    xt_setup(&R, XT_DATA, 0);
    xt_drain_user(&R);
    EXTRACE_INTS(T_INTS, big, big, big, big, big, big, big, big, big, big, big, big, big, big,
                 big, big); /* 3 + 16 * 10 bytes */
    EXTRACE_INTS(T_INTS, 1u);
    n = xt_drain_user(&R);
    if (163 > EXTRACE_CFG_MAX_PAYLOAD) {
        EXPECT_EQ(extrace_producer.lost_total, 1);
        ASSERT_EQ(n, 1);
        EXPECT_EQ(xt_int(&xt_recs[0], 0), 1);
    } else {
        EXPECT_EQ(extrace_producer.lost_total, 0);
        ASSERT_EQ(n, 2);
        EXPECT_EQ(xt_int(&xt_recs[0], 15), (uint64_t)big);
    }
}

#if !EXTRACE_CFG_PRINTF_LOCAL
static const uint8_t *printf_arg(const uint8_t *q, uint64_t *v)
{
    return q + extrace_get_uv64(q, q + 10, v);
}

TEST(emit_printf)
{
    uint32_t site0, site1, site2;
    const uint8_t *args;
    uint32_t alen;
    uint64_t v = 0;
    float f = 1.5f;
    double d = -2.25;
    int n;

    xt_setup(&R, XT_DATA, 0);
    EXTRACE_PRINTF(T_PRINTF, "no args");
    EXTRACE_PRINTF(T_PRINTF, "u=%u d=%d", 7u, -1);
    EXTRACE_PRINTF(T_PRINTF, "s=%s f=%f g=%g p=%p c=%s", "abc", f, d, (void *)&R,
                   EXTRACE_CSTR(__func__));
    EXTRACE_PRINTF(T_PRINTF, "%d %d %d %d %d %d %d %d %d %d", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    n = xt_drain_user(&R);
    ASSERT_EQ(n, 4);

    ASSERT_EQ(extrace_rec_printf_site(&xt_recs[0].r, &site0, &args, &alen), 0);
    EXPECT_EQ(alen, 0);
    ASSERT_EQ(extrace_rec_printf_site(&xt_recs[1].r, &site1, &args, &alen), 0);
    EXPECT_NE(site0, site1);
    EXPECT_EQ(alen, 2);
    EXPECT_EQ(args[0], 7);
    EXPECT_EQ(args[1], 1); /* zigzag(-1) */
    ASSERT_EQ(extrace_rec_printf_site(&xt_recs[2].r, &site2, &args, &alen), 0);
    EXPECT_NE(site2, site1);
    EXPECT_EQ(args[0], 3);
    EXPECT_MEM(args + 1, "abc", 3);
    args += 4;
    EXPECT_MEM(args, "\x00\x00\xC0\x3F", 4); /* F32 1.5, little-endian */
    args += 4;
    EXPECT_MEM(args, "\x00\x00\x00\x00\x00\x00\x02\xC0", 8); /* F64 -2.25 */
    args += 8;
    args = printf_arg(args, &v);
    EXPECT_EQ(v, (uint64_t)(uintptr_t)&R);
    args = printf_arg(args, &v);
    EXPECT_EQ(v, (uint64_t)(uintptr_t)__func__);
    ASSERT_EQ(extrace_rec_printf_site(&xt_recs[3].r, &site2, &args, &alen), 0);
    EXPECT_EQ(alen, 10);
    EXPECT_EQ(args[9], 18); /* zigzag(9) */
}

TEST(emit_printf_long_strings_are_shortened)
{
    char s[EXTRACE_CFG_STR_ARG_MAX + 10];
    uint32_t site, alen, sl = 0;
    const uint8_t *args;
    int n;

    memset(s, 'x', sizeof(s) - 1);
    s[sizeof(s) - 1] = 0;
    xt_setup(&R, XT_DATA, 0);
    EXTRACE_PRINTF(T_PRINTF, "%s", s);
    /* 16 maximal strings exceed MAX_PAYLOAD: the last ones are cut */
    EXTRACE_PRINTF(T_PRINTF, "%s", s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s);
    n = xt_drain_user(&R);
    ASSERT_EQ(n, 2);
    extrace_rec_printf_site(&xt_recs[0].r, &site, &args, &alen);
    args += extrace_get_uv32(args, args + 4, &sl);
    EXPECT_TRUE(sl <= EXTRACE_CFG_STR_ARG_MAX);
    if (EXTRACE_CFG_MAX_PAYLOAD >= 2 * EXTRACE_CFG_STR_ARG_MAX)
        EXPECT_EQ(sl, EXTRACE_CFG_STR_ARG_MAX);
    EXPECT_TRUE(xt_recs[0].r.len <= EXTRACE_CFG_MAX_PAYLOAD);
    EXPECT_TRUE(xt_recs[1].r.len <= EXTRACE_CFG_MAX_PAYLOAD);
    EXPECT_TRUE(xt_recs[1].r.len >= EXTRACE_CFG_MAX_PAYLOAD - 4);
}

#else
TEST(emit_printf_local_mode)
{
    int n;

    xt_setup(&R, XT_DATA, 0);
    EXTRACE_PRINTF(T_PRINTF, "u=%u", 7u);
    n = xt_drain_user(&R);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(xt_recs[0].r.kind, EXTRACE_K_STR);
    EXPECT_MEM(xt_recs[0].payload, "u=7", 3);
}
#endif

TEST(emit_text_local)
{
    int n;

    xt_setup(&R, XT_DATA, 0);
    EXTRACE_TEXT(T_STR, "v=%d", 42);
    n = xt_drain_user(&R);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(xt_recs[0].r.kind, EXTRACE_K_STR);
    EXPECT_EQ(xt_recs[0].r.len, 4);
    EXPECT_MEM(xt_recs[0].payload, "v=42", 4);
    /* longer than TEXT_MAX and, in small builds, MAX_PAYLOAD: truncated */
    EXTRACE_TEXT(T_STR, "%0*d", 300, 7);
    n = xt_drain_user(&R);
    ASSERT_EQ(n, 1);
    EXPECT_TRUE(xt_recs[0].r.len <= EXTRACE_CFG_MAX_PAYLOAD);
    EXPECT_EQ(xt_recs[0].r.len, EXTRACE_CFG_TEXT_MAX <= EXTRACE_CFG_MAX_PAYLOAD
                                    ? EXTRACE_CFG_TEXT_MAX - 1
                                    : EXTRACE_CFG_MAX_PAYLOAD);
}

TEST(emit_dyn)
{
    const uint32_t v32[2] = { 1, 2 };
    const uint64_t v64[1] = { 1ull << 50 };
    int n;

    xt_setup(&R, XT_DATA, 0);
    EXPECT_TRUE(extrace_emit_dyn_none(EXTRACE_EV_T_NONE));
    EXPECT_TRUE(extrace_emit_dyn_ints(EXTRACE_EV_T_INTS, v32, 2, 0));
    EXPECT_TRUE(extrace_emit_dyn_ints64(EXTRACE_EV_T_INTS, v64, 1, 0));
    EXPECT_TRUE(extrace_emit_dyn_buf(EXTRACE_EV_T_BUF, EXTRACE_K_BUF, "ab", 2));
    EXPECT_FALSE(extrace_emit_dyn_none(EXTRACE_EV_T_OFF));       /* disabled */
    EXPECT_FALSE(extrace_emit_dyn_none(EXTRACE_SYS_SYNC));       /* system ID */
    EXPECT_FALSE(extrace_emit_dyn_none(EXTRACE_EV_COUNT));       /* out of range */
    EXPECT_FALSE(extrace_emit_dyn_ints(EXTRACE_EV_T_INTS, v32, 0, 0));
    EXPECT_FALSE(extrace_emit_dyn_buf(EXTRACE_EV_T_BUF, EXTRACE_K_INTS, "ab", 2));
    n = xt_drain_user(&R);
    ASSERT_EQ(n, 4);
    EXPECT_EQ(xt_int(&xt_recs[2], 0), 1ull << 50);
}

TEST(emit_buf_large_fragments)
{
    static uint8_t big[3000];
    uint32_t total = 0, xfer = 0;
    int n;

    for (unsigned int i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i * 7);
    xt_setup(&R, 8192, 0);
    EXTRACE_BUF_LARGE(T_BUF, big, sizeof(big));
    EXTRACE_BUF_LARGE(T_BUF, big, EXTRACE_CFG_MAX_PAYLOAD / 2); /* fits: an ordinary BUF */
    extrace_prod_flush();
    {
        struct extrace_dec d;

        extrace_dec_reset(&d, 7, 0);
        n = xt_drain(&R, &d, true);
    }
    for (int i = 0; i < n; i++) {
        const struct extrace_rec *r = &xt_recs[i].r;
        const uint8_t *q = r->payload, *end = r->payload + r->len;
        uint32_t et = 0, x = 0, id = 0, tot = 0, off = 0;

        if (r->event != EXTRACE_SYS_FRAG)
            continue;
        EXPECT_EQ(r->kind, EXTRACE_K_EXT);
        q += extrace_get_uv32(q, end, &et);
        q += extrace_get_uv32(q, end, &x);
        q += extrace_get_uv32(q, end, &id);
        q += extrace_get_uv32(q, end, &tot);
        q += extrace_get_uv32(q, end, &off);
        EXPECT_EQ(et, EXTRACE_EXT_FRAG);
        EXPECT_EQ(id, EXTRACE_EV_T_BUF);
        EXPECT_EQ(tot, sizeof(big));
        EXPECT_EQ(off, total);
        if (!xfer)
            xfer = x;
        EXPECT_EQ(x, xfer);
        EXPECT_MEM(q, big + off, (size_t)(end - q));
        total += (uint32_t)(end - q);
        EXPECT_TRUE(r->len <= EXTRACE_CFG_MAX_PAYLOAD);
    }
    EXPECT_EQ(total, sizeof(big));
    EXPECT_EQ(xt_recs[n - 1].r.event, EXTRACE_EV_T_BUF);
    EXPECT_EQ(xt_recs[n - 1].r.len, EXTRACE_CFG_MAX_PAYLOAD / 2);
    EXPECT_EQ(extrace_producer.frag_seq, xfer);
}

/* ---- publication ---- */

TEST(publish_threshold_and_urgent)
{
    struct extrace_shm_pstate *ps = extrace_blk_pstate(R.blk);
    uint32_t h0;

    xt_setup(&R, XT_DATA, 64);
    extrace_prod_flush();
    h0 = ps->head;
    EXTRACE_INTS(T_INTS, 1u); /* 6 bytes: below the threshold */
    EXPECT_EQ(ps->head, h0);
    EXPECT_NE(extrace_producer.committed, extrace_producer.published);
    EXTRACE_INTS_URGENT(T_INTS, 2u);
    EXPECT_EQ(ps->head, extrace_producer.head);
    h0 = ps->head;
    for (int i = 0; i < 20 && ps->head == h0; i++)
        EXTRACE_INTS(T_INTS, 1u);
    EXPECT_NE(ps->head, h0); /* the threshold published */
    EXPECT_TRUE(ps->head - h0 >= 64);
    EXPECT_EQ(ps->publish_cnt, extrace_producer.publish_cnt);
    EXTRACE_MARK_URGENT(T_MARK);
    EXPECT_EQ(ps->head, extrace_producer.head);
}

TEST(publish_nested_commit_order)
{
    struct extrace_resv a;
    uint32_t committed;
    int n;

    xt_setup(&R, XT_DATA, 0);
    xt_drain_user(&R);
    committed = extrace_producer.committed;
    ASSERT_TRUE(EXTRACE_RESERVE(T_BUF, 4, 1, &a)); /* thread: reservation A */
    EXTRACE_INTS(T_INTS, 9u);                      /* "ISR": B after A */
    EXTRACE_MARK(T_MARK);                          /* written in the critical section */
    EXPECT_EQ(extrace_producer.committed, committed); /* B waits for A */
    extrace_prod_flush();
    EXPECT_EQ(extrace_cons_read(&R.c, xt_buf, sizeof(xt_buf)), 0);
    memcpy(a.payload, "AAAA", 4);
    extrace_commit(&a);
    EXPECT_EQ(extrace_producer.committed, extrace_producer.head);
    n = xt_drain_user(&R);
    ASSERT_EQ(n, 3);
    EXPECT_EQ(xt_recs[0].r.event, EXTRACE_EV_T_BUF);
    EXPECT_EQ(xt_recs[1].r.event, EXTRACE_EV_T_INTS);
    EXPECT_EQ(xt_recs[2].r.event, EXTRACE_EV_T_MARK);
}

TEST(publish_in_isr_option)
{
    struct extrace_shm_pstate *ps = extrace_blk_pstate(R.blk);
    uint32_t h0;

    xt_setup(&R, XT_DATA, 8);
    extrace_prod_flush();
    h0 = ps->head;
    extrace_posix_isr_depth = 1; /* as if called from an interrupt handler */
    EXTRACE_INTS(T_INTS, 1u, 2u, 3u);
    if (EXTRACE_CFG_PUBLISH_IN_ISR)
        EXPECT_NE(ps->head, h0);
    else
        EXPECT_EQ(ps->head, h0); /* the next thread-context commit or poll publishes */
    EXTRACE_INTS_URGENT(T_INTS, 4u);
    EXPECT_EQ(ps->head, extrace_producer.head); /* urgent publishes in any context */
    extrace_posix_isr_depth = 0;
}

/* ---- ring wrap and overflow ---- */

TEST(wrap_records_never_straddle)
{
    uint64_t s = xt_seed();
    uint8_t b[200];

    for (unsigned int i = 0; i < sizeof(b); i++)
        b[i] = (uint8_t)i;
    xt_setup(&R, XT_DATA, 0);
    xt_drain_user(&R);
    for (int round = 0; round < 400; round++) {
        int k = (int)xt_rand_below(&s, 8) + 1, n;

        for (int j = 0; j < k; j++) {
            uint32_t len = xt_rand_below(&s, sizeof(b));

            EXTRACE_BUF(T_BUF, b, len);
        }
        n = xt_drain_user(&R);
        ASSERT_EQ(n, k);
        for (int j = 0; j < n; j++)
            EXPECT_MEM(xt_recs[j].payload, b, xt_recs[j].r.len);
    }
    EXPECT_TRUE(extrace_producer.head > 10 * XT_DATA);
    EXPECT_EQ(extrace_producer.lost_total, 0);
}

TEST(overflow_drops_and_reports_lost)
{
    struct extrace_shm_pstate *ps = extrace_blk_pstate(R.blk);
    unsigned int emitted = 0, notif;
    uint32_t lost;
    const struct xt_rec *lr;
    int n;

    xt_setup(&R, XT_DATA, 0);
    notif = xt_notifies;
    /* no consumer progress: fill the ring and keep going */
    for (int i = 0; i < 2000; i++, emitted++)
        EXTRACE_INTS(T_SEQ, (uint32_t)i, 0u);
    lost = extrace_producer.lost_total;
    EXPECT_TRUE(lost > 0);
    EXPECT_EQ(extrace_producer.lost_pending, lost);
    EXPECT_EQ(xt_notifies, notif + 1); /* one doorbell per overflow episode */
    EXPECT_TRUE(extrace_producer.head - extrace_producer.tail_cache <= XT_DATA);
    extrace_prod_flush(); /* lost_total is stored at publish time */
    EXPECT_EQ(ps->lost_total, lost);
    /* drain, then the next record carries the report */
    n = xt_drain_user(&R);
    EXPECT_EQ((unsigned int)n + lost, emitted);
    EXTRACE_INTS(T_SEQ, 5000u, 0u);
    {
        struct extrace_dec d;

        extrace_dec_reset(&d, 7, 0);
        extrace_prod_flush();
        n = xt_drain(&R, &d, true);
    }
    ASSERT_EQ(n, 2);
    lr = find(n, EXTRACE_SYS_LOST, 0);
    ASSERT_TRUE(lr != nullptr);
    EXPECT_EQ(xt_int(lr, 0), lost);
    EXPECT_TRUE(xt_recs[1].r.flags & EXTRACE_REC_AFTER_LOST);
    EXPECT_EQ(xt_recs[1].r.ts, lr->r.ts); /* tsd 0 after LOST */
    EXPECT_EQ(extrace_producer.lost_pending, 0);
    EXPECT_EQ(ps->hwm, XT_DATA - (XT_DATA - ps->hwm)); /* recorded */
    EXPECT_TRUE(ps->hwm > XT_DATA - 16);
}

TEST(overflow_exact_fill)
{
    uint32_t n0;

    xt_setup(&R, XT_DATA, 0);
    if (extrace_producer.head & 1u) {
        xt_step = 1;
        EXTRACE(T_NONE); /* 3 bytes: an even head */
    }
    xt_drain_user(&R);
    /* MARK records are 2 bytes: the ring holds exactly size / 2 of them */
    n0 = (XT_DATA - (extrace_producer.head & (XT_DATA - 1))) % 2;
    EXPECT_EQ(n0, 0);
    for (unsigned int i = 0; i < XT_DATA / 2; i++)
        EXTRACE_MARK(T_MARK);
    EXPECT_EQ(extrace_producer.lost_total, 0);
    EXPECT_EQ(extrace_producer.head - extrace_producer.tail_cache, XT_DATA);
    EXTRACE_MARK(T_MARK);
    EXPECT_EQ(extrace_producer.lost_total, 1);
    EXPECT_EQ(xt_drain_user(&R), (int)(XT_DATA / 2));
}

TEST(inflight_limit)
{
    static struct extrace_resv rv[256];
    unsigned int ok = 0;

    xt_setup(&R, 16384, 0);
    for (unsigned int i = 0; i < 256; i++)
        ok += EXTRACE_RESERVE(T_BUF, 1, 1, &rv[i]);
    EXPECT_EQ(ok, 255);
    EXPECT_EQ(extrace_producer.lost_pending, 1);
    EXPECT_EQ(extrace_producer.stats.resv_open_max, 255);
    for (unsigned int i = 0; i < 255; i++) {
        *(uint8_t *)rv[i].payload = (uint8_t)i;
        extrace_commit(&rv[254 - i]); /* any order */
    }
    EXPECT_EQ(extrace_producer.inflight, 0);
    EXPECT_EQ(xt_drain_user(&R), 255);
}

/* ---- direct reservation ---- */

#if EXTRACE_CFG_RESV_ALIGN_MAX >= 8
TEST(reserve_alignment_at_every_offset)
{
    for (uint32_t pre = 0; pre < 24; pre++) {
        static const uint32_t al[] = { 1, 2, 4, 8 };

        for (unsigned int a = 0; a < 4; a++) {
            struct extrace_resv rv;
            bool ok;
            int n;

            xt_setup(&R, XT_DATA, 0);
            xt_drain_user(&R);
            for (uint32_t i = 0; i < pre; i++)
                EXTRACE_MARK(T_MARK); /* shift the start by 2 bytes each */
            EXTRACE_BUF(T_BUF, "x", pre % 3);
            switch (al[a]) {
            case 1:
                ok = EXTRACE_RESERVE(T_BUF, 12, 1, &rv);
                break;
            case 2:
                ok = EXTRACE_RESERVE(T_BUF, 12, 2, &rv);
                break;
            case 4:
                ok = EXTRACE_RESERVE(T_BUF, 12, 4, &rv);
                break;
            default:
                ok = EXTRACE_RESERVE(T_BUF, 12, 8, &rv);
                break;
            }
            ASSERT_TRUE(ok);
            EXPECT_EQ((uintptr_t)rv.payload % al[a], 0);
            memcpy(rv.payload, "0123456789ab", 12);
            extrace_commit(&rv);
            n = xt_drain_user(&R);
            ASSERT_EQ(n, (int)pre + 2);
            EXPECT_EQ(xt_recs[n - 1].r.len, 12);
            EXPECT_MEM(xt_recs[n - 1].payload, "0123456789ab", 12);
        }
    }
}

#endif

TEST(reserve_struct_cancel_and_limits)
{
    struct extrace_resv rv;
    struct test_struct t = { .d = 42 };
    int n;

    xt_setup(&R, XT_DATA, 0);
    ASSERT_TRUE(EXTRACE_RESERVE_STRUCT(T_STRUCT, &rv));
    EXPECT_EQ(rv.len, sizeof(struct test_struct));
    EXPECT_EQ((uintptr_t)rv.payload % EXTRACE_RESV_ALIGNOF_(struct test_struct), 0);
    memcpy(rv.payload, &t, sizeof(t));
    extrace_commit_urgent(&rv);
    EXPECT_EQ(extrace_blk_pstate(R.blk)->head, extrace_producer.head);
    ASSERT_TRUE(EXTRACE_RESERVE(T_BUF, 50, 1, &rv));
    extrace_cancel(&rv); /* becomes padding */
    EXPECT_FALSE(EXTRACE_RESERVE(T_BUF, EXTRACE_CFG_MAX_PAYLOAD + 1, 1, &rv));
#if EXTRACE_CFG_RESV_ALIGN_MAX > 1
    EXPECT_FALSE(extrace_reserve_aligned(EXTRACE_HENC(EXTRACE_EV_T_BUF, EXTRACE_K_BUF), 4, 3, &rv));
#endif
    ASSERT_TRUE(EXTRACE_RESERVE(T_STR, 2, 1, &rv));
    memcpy(rv.payload, "ok", 2);
    extrace_commit(&rv);
    EXPECT_EQ(extrace_producer.lost_total, 0);
    n = xt_drain_user(&R);
    ASSERT_EQ(n, 2);
    EXPECT_EQ(xt_recs[0].r.event, EXTRACE_EV_T_STRUCT);
    EXPECT_EQ(xt_recs[1].r.event, EXTRACE_EV_T_STR);
    EXPECT_EQ(xt_recs[1].r.kind, EXTRACE_K_STR);
}

TEST(cancel_keeps_timestamp_chain)
{
    struct extrace_resv rv;
    struct extrace_dec d;
    int n;

    xt_setup(&R, XT_DATA, 0);
    xt_step = 0; /* each record is timestamped with xt_now as set here */
    xt_now = 2000;
    EXTRACE_INTS(T_SEQ, 1u, 0u);
    xt_now = 2020;
    ASSERT_TRUE(EXTRACE_RESERVE(T_BUF, 6, 1, &rv));
    extrace_cancel(&rv);
    xt_now = 2030;
    EXTRACE_INTS(T_SEQ, 2u, 0u);
    extrace_dec_reset(&d, R.c.ring_id, 0);
    d.disp_ts = 5000;
    extrace_prod_flush();
    n = xt_drain(&R, &d, false);
    ASSERT_EQ(n, 2);
    EXPECT_TRUE(xt_recs[1].r.flags & EXTRACE_REC_HAS_TS);
    EXPECT_EQ(xt_recs[0].r.ts, 2000);
    EXPECT_EQ(xt_recs[1].r.ts, 2030); /* the cancelled record's delta is kept */
    EXPECT_EQ(d.errors, 0);
}

/* ---- poll: masks, commands, thresholds, periodic SYNC ---- */

TEST(poll_applies_masks_after_attach)
{
    struct extrace_shm_pstate *ps = extrace_blk_pstate(R.blk);
    int n;

    xt_setup(&R, XT_DATA, 0);
    EXPECT_TRUE(EXTRACE_ENABLED(T_INTS));
    EXPECT_EQ(extrace_cons_ev_set(&R.c, EXTRACE_EV_T_INTS, EXTRACE_EVS_OFF), 0);
    EXPECT_EQ(extrace_cons_ev_set(&R.c, EXTRACE_EV_T_OFF, EXTRACE_EVS_ON), 0);
    EXPECT_EQ(xt_notifies_producer, 2);
    EXPECT_TRUE(EXTRACE_ENABLED(T_INTS)); /* not before poll */
    extrace_prod_poll();
    EXPECT_FALSE(EXTRACE_ENABLED(T_INTS));
    EXPECT_TRUE(EXTRACE_ENABLED(T_OFF));
    EXPECT_EQ(ps->mask_gen_ack, extrace_cons_mask_gen(&R.c));
    EXPECT_TRUE(extrace_cons_mask_applied(&R.c));
    {
        struct extrace_dec d;
        const struct xt_rec *cfg;

        extrace_dec_reset(&d, 7, 0);
        extrace_prod_flush();
        n = xt_drain(&R, &d, true);
        cfg = find(n, EXTRACE_SYS_CFG, 0);
        ASSERT_TRUE(cfg != nullptr);
        EXPECT_EQ(xt_int(cfg, 0), extrace_cons_mask_gen(&R.c));
    }
    EXPECT_EQ(extrace_cons_ev_reset_all(&R.c), 0);
    extrace_prod_poll();
    EXPECT_TRUE(EXTRACE_ENABLED(T_INTS));
    EXPECT_FALSE(EXTRACE_ENABLED(T_OFF));
}

TEST(poll_ignores_cstate_before_attach)
{
    struct extrace_shm_cstate *cs = extrace_blk_cstate(R.blk);

    xt_reset_world();
    memset(&R, 0, sizeof(R));
    cs->cmd_seq = 77; /* garbage from an earlier life */
    ASSERT_EQ(xt_prod_start(&R, XT_DATA, 0), 0);
    EXPECT_EQ(extrace_blk_pstate(R.blk)->cmd_seq_ack, 77); /* the s0 of I1 */
    /* garbage overrides and commands of no attached consumer */
    extrace_blk_mask_set(R.blk)[0] = 0xFFFFFFFFu;
    cs->mask_gen = 10;
    cs->cmd_seq = 78;
    cs->cmd_bits = EXTRACE_CMD_ALL;
    cs->magic = EXTRACE_SHM_MAGIC;
    cs->attached_epoch = extrace_producer.epoch + 1;
    extrace_prod_poll();
    EXPECT_FALSE(EXTRACE_ENABLED(T_OFF));
    EXPECT_EQ(extrace_blk_pstate(R.blk)->cmd_seq_ack, 77); /* nothing executed */
    EXPECT_EQ(extrace_blk_pstate(R.blk)->mask_gen_ack, 0);
}

TEST(poll_commands_and_thresholds)
{
    struct extrace_shm_pstate *ps = extrace_blk_pstate(R.blk);
    unsigned int notif;
    int n, info = 0, sync = 0;

    xt_setup(&R, XT_DATA, 0);
    extrace_prod_poll();
    xt_drain_user(&R);
    EXPECT_EQ(extrace_cons_set_publish_thr(&R.c, 16), 0);
    EXPECT_EQ(extrace_cons_set_notify(&R.c, 32), 0);
    EXPECT_FALSE(extrace_cons_cmd_done(&R.c));
    extrace_prod_poll();
    EXPECT_TRUE(extrace_cons_cmd_done(&R.c));
    EXPECT_EQ(extrace_producer.publish_thr, 16);
    EXPECT_EQ(extrace_producer.notify_thr, 32);
    EXPECT_EQ(extrace_cons_cmd(&R.c, EXTRACE_CMD_SYNC | EXTRACE_CMD_INFO), 0);
    EXPECT_EQ(extrace_cons_cmd(&R.c, EXTRACE_CMD_FLUSH), 0); /* bits accumulate */
    EXPECT_EQ(R.c.cmd_pending, EXTRACE_CMD_SYNC | EXTRACE_CMD_INFO | EXTRACE_CMD_FLUSH);
    EXPECT_EQ(extrace_cons_cmd(&R.c, 0x100), -EXTRACE_EINVAL);
    notif = xt_notifies;
    extrace_prod_poll();
    EXPECT_EQ(ps->cmd_seq_ack, R.c.cmd_seq);
    EXPECT_EQ(ps->head, extrace_producer.head); /* FLUSH */
    EXPECT_TRUE(xt_notifies > notif);              /* notify_thr reached */
    {
        struct extrace_dec d;

        extrace_dec_reset(&d, 7, 0);
        n = xt_drain(&R, &d, true);
        for (int i = 0; i < n; i++) {
            info += xt_recs[i].r.event == EXTRACE_SYS_INFO;
            sync += xt_recs[i].r.event == EXTRACE_SYS_SYNC;
        }
    }
    EXPECT_EQ(info, 1);
    EXPECT_EQ(sync, 1);
    EXPECT_EQ(extrace_cons_cmd(&R.c, EXTRACE_CMD_SYNC), 0);
    EXPECT_EQ(R.c.cmd_pending, EXTRACE_CMD_SYNC); /* acknowledged bits dropped (K1) */
}

TEST(poll_thresholds_reach_a_restarted_producer)
{
    xt_setup(&R, XT_DATA, 0);
    EXPECT_EQ(extrace_cons_set_publish_thr(&R.c, 40), 0);
    extrace_prod_poll();
    EXPECT_EQ(extrace_producer.publish_thr, 40);
    EXPECT_EQ(xt_prod_start(&R, XT_DATA, 0), 0); /* restart: defaults again */
    EXPECT_EQ(extrace_producer.publish_thr, XT_DATA / 4);
    EXPECT_EQ(extrace_cons_poll_state(&R.c), 1); /* consumer re-attaches */
    extrace_prod_poll();
    EXPECT_EQ(extrace_producer.publish_thr, 40); /* implicit THR at the first poll */
}

TEST(poll_periodic_sync)
{
    int n, syncs = 0;
    struct extrace_dec d;

    xt_setup(&R, XT_DATA, 0);
    xt_step = 30000; /* 30 ms per timer read at 1 MHz */
    for (int i = 0; i < 10; i++)
        extrace_prod_poll();
    extrace_dec_reset(&d, 7, 0);
    n = xt_drain(&R, &d, true);
    for (int i = 0; i < n; i++)
        syncs += xt_recs[i].r.event == EXTRACE_SYS_SYNC;
    EXPECT_TRUE(syncs >= 3);
    EXPECT_EQ(extrace_producer.sync_period, 100000);
}

/* ---- panic ---- */

TEST(panic_publishes_committed)
{
    struct extrace_shm_pstate *ps = extrace_blk_pstate(R.blk);
    const struct xt_rec *pr;
    struct extrace_dec d;
    int n;

    xt_setup(&R, XT_DATA, 0);
    EXTRACE_INTS(T_INTS, 1u);
    extrace_prod_panic(0xBAD, 0x1234);
    EXPECT_EQ(ps->status, EXTRACE_ST_PANIC);
    EXPECT_EQ(ps->head, extrace_producer.committed);
    EXPECT_TRUE(extrace_producer.flags & EXTRACE_PF_PANIC);
    EXTRACE_INTS(T_INTS, 2u); /* dropped without counting */
    EXPECT_EQ(extrace_producer.lost_total, 0);
    extrace_dec_reset(&d, 7, 0);
    n = xt_drain(&R, &d, true);
    pr = find(n, EXTRACE_SYS_PANIC, 0);
    ASSERT_TRUE(pr != nullptr);
    EXPECT_EQ(xt_int(pr, 0), 0xBAD);
    EXPECT_EQ(xt_int(pr, 1), 0x1234);
    EXPECT_EQ(xt_recs[n - 1].r.event, EXTRACE_SYS_PANIC);
    extrace_prod_panic(1, 2); /* idempotent */
}

TEST(panic_with_open_reservation)
{
    struct extrace_resv rv;
    struct extrace_shm_pstate *ps = extrace_blk_pstate(R.blk);
    uint32_t committed;

    xt_setup(&R, XT_DATA, 0);
    EXTRACE_INTS(T_INTS, 1u);
    ASSERT_TRUE(EXTRACE_RESERVE(T_BUF, 4, 1, &rv));
    committed = extrace_producer.committed;
    extrace_producer.publishing = 1; /* panic interrupts a publish */
    extrace_prod_panic(1, 0);
    EXPECT_EQ(ps->head, committed); /* the open reservation is not published */
    EXPECT_EQ(xt_drain_user(&R), 1);
    extrace_producer.publishing = 0;
}

/* ---- index and timer wrap ---- */

TEST(index_wrap_end_to_end)
{
    uint32_t seq = 0, got = 0;

    xt_reset_world();
    extrace_test_index_base = 0xFFFFF000u;
    memset(&R, 0, sizeof(R));
    ASSERT_EQ(xt_prod_start(&R, XT_DATA, 0), 0);
    ASSERT_EQ(xt_cons_attach(&R), 0);
    EXPECT_EQ(R.c.tail, 0xFFFFF000u);
    for (int round = 0; round < 50; round++) {
        int n;

        for (int i = 0; i < 40; i++)
            EXTRACE_INTS(T_SEQ, seq++, 0u);
        n = xt_drain_user(&R);
        for (int i = 0; i < n; i++)
            EXPECT_EQ(xt_int(&xt_recs[i], 0), got++);
    }
    EXPECT_EQ(got, seq);
    EXPECT_TRUE(extrace_producer.head < 0x10000u); /* crossed 2^32 */
    EXPECT_EQ((uint32_t)R.c.stream_pos, R.c.tail);
    extrace_test_index_base = 0;
}

TEST(timer_wrap_deltas)
{
    struct extrace_dec d;
    uint64_t prev = 0;
    int n;

    xt_reset_world();
    memset(&R, 0, sizeof(R));
    xt_now = 0xFFFFFF00u;
    xt_step = 7;
    ASSERT_EQ(xt_prod_start(&R, XT_DATA, 0), 0);
    ASSERT_EQ(xt_cons_attach(&R), 0);
    for (int i = 0; i < 100; i++)
        EXTRACE(T_NONE);
    extrace_prod_flush();
    extrace_dec_reset(&d, 7, 0);
    d.disp_ts = 0x100000000ull + xt_now; /* the dispatcher's extended timer */
    n = xt_drain(&R, &d, true);
    ASSERT_TRUE(n > 100);
    for (int i = 1; i < n; i++) {
        if (!(xt_recs[i].r.flags & EXTRACE_REC_HAS_TS))
            continue;
        EXPECT_TRUE(xt_recs[i].r.ts >= prev);
        prev = xt_recs[i].r.ts;
    }
    EXPECT_TRUE(prev >= 0x100000000ull); /* crossed into the next 2^32 period */
}

TEST(stats_report)
{
    struct extrace_prod_stats st;

    xt_setup(&R, XT_DATA, 0);
    EXTRACE_INTS(T_INTS, 1u);
    extrace_prod_flush();
    extrace_prod_stats(&st);
    EXPECT_EQ(st.epoch, extrace_producer.epoch);
    EXPECT_EQ(st.records, 3); /* INFO, SYNC, INTS */
    EXPECT_TRUE(st.publishes >= 1);
}
