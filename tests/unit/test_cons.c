// SPDX-License-Identifier: MIT
/*
 * test_cons.c - consumer: attach validation, resume, image change, peek/release,
 * budget clamp, copy mode, head errors, epoch changes and event control
 * (DESIGN §7.5-§7.7, §9.3, §11, §17).
 */
#include "xt_util.h"

static struct xt_ring R;

TEST(attach_before_producer_and_garbage)
{
    xt_reset_world();
    memset(&R, 0, sizeof(R));
    R.block_size = XT_BLOCK_SIZE;
    EXPECT_EQ(xt_cons_attach(&R), -EXTRACE_ENOTREADY);
    /* garbage: random bytes, magic included */
    for (unsigned int i = 0; i < XT_BLOCK_SIZE / 8; i++)
        R.blk[i] = 0x9E3779B97F4A7C15ull * (i + 1);
    extrace_blk_pinfo(R.blk)->magic = EXTRACE_SHM_MAGIC;
    EXPECT_TRUE(xt_cons_attach(&R) < 0);
    EXPECT_EQ(extrace_cons_attached(&R.c), false);
    EXPECT_EQ(xt_prod_start(&R, XT_DATA, 0), 0);
    EXPECT_EQ(extrace_cons_poll_state(&R.c), 1);
    EXPECT_TRUE(extrace_cons_attached(&R.c));
    EXPECT_EQ(extrace_cons_poll_state(&R.c), 0);
    EXPECT_EQ(xt_drain_user(&R), 0);
}

TEST(attach_validation)
{
    struct extrace_shm_pinfo *pi = extrace_blk_pinfo(R.blk);
    uint16_t lv;

    xt_setup(&R, XT_DATA, 0);
    EXPECT_EQ(extrace_cons_attach(&R.c, R.blk, R.block_size, R.mirror, 1, 0), -EXTRACE_ENOSPC);
    EXPECT_EQ(extrace_cons_attach(&R.c, R.blk, R.block_size - 1, R.mirror, 64, 0),
              -EXTRACE_ECORRUPT);
    EXPECT_EQ(extrace_cons_attach(&R.c, (uint8_t *)R.blk + 1, R.block_size, R.mirror, 64, 0),
              -EXTRACE_EINVAL);
    lv = pi->layout_ver;
    pi->layout_ver = 0x0100; /* another layout major version */
    EXPECT_EQ(xt_cons_attach(&R), -EXTRACE_EVERSION);
    pi->layout_ver = lv;
    pi->data_size = 3000;
    EXPECT_EQ(xt_cons_attach(&R), -EXTRACE_ECORRUPT);
    pi->data_size = XT_DATA;
    pi->max_record = XT_DATA;
    EXPECT_EQ(xt_cons_attach(&R), -EXTRACE_ECORRUPT); /* 4 x max_record > data_size */
    pi->max_record = EXTRACE_MAX_RECORD;
    pi->mask_words = 2;
    EXPECT_EQ(xt_cons_attach(&R), -EXTRACE_ECORRUPT);
    pi->mask_words = 1;
    pi->ring_id = 255;
    EXPECT_EQ(xt_cons_attach(&R), -EXTRACE_ECORRUPT);
    pi->ring_id = 7;
    EXPECT_EQ(xt_cons_attach(&R), 0);
    EXPECT_EQ(R.c.stats.attach_errors, 7);
}

TEST(thresholds_in_one_command)
{
    struct extrace_shm_cstate *cs = extrace_blk_cstate(R.blk);
    uint32_t seq;

    xt_setup(&R, XT_DATA, 0);
    extrace_prod_poll(); /* the producer saw the attach */
    seq = R.c.cmd_seq;
    EXPECT_EQ(extrace_cons_set_thr(&R.c, 48, 99), 0);
    EXPECT_EQ(cs->publish_thr, 48);
    EXPECT_EQ(cs->notify_thr, 99);
    EXPECT_EQ(R.c.publish_thr, 48);
    EXPECT_EQ(R.c.notify_thr, 99);
    EXPECT_EQ(R.c.cmd_seq, seq + 1); /* one THR command for both */
    EXPECT_TRUE(cs->cmd_bits & EXTRACE_CMD_THR);
    extrace_prod_poll();
    EXPECT_EQ(extrace_producer.publish_thr, 48);
    EXPECT_EQ(extrace_producer.notify_thr, 99);
    /* detached: nothing of the consumer's state or cstate changes */
    R.c.flags &= (uint8_t)~EXTRACE_CF_ATTACHED;
    EXPECT_EQ(extrace_cons_set_thr(&R.c, 7, 8), -EXTRACE_ENOTREADY);
    EXPECT_EQ(cs->publish_thr, 48);
    EXPECT_EQ(cs->notify_thr, 99);
    EXPECT_EQ(R.c.publish_thr, 48);
    EXPECT_EQ(R.c.cmd_seq, seq + 1);
}

TEST(producer_diagnostics)
{
    struct extrace_shm_pstate *ps = extrace_blk_pstate(R.blk);
    struct extrace_cons_diag dg;

    xt_setup(&R, XT_DATA, 0);
    for (uint32_t i = 0; i < 20; i++)
        EXTRACE_INTS(T_SEQ, i, 0u);
    extrace_prod_flush();
    ps->lost_total = 5; /* as a producer that dropped records would publish it */
    EXPECT_EQ(extrace_cons_diag(&R.c, &dg), 0);
    EXPECT_EQ(dg.lost_total, 5);
    EXPECT_EQ(dg.hwm, ps->hwm);
    EXPECT_TRUE(dg.hwm > 0);
    EXPECT_EQ(dg.status, EXTRACE_ST_RUNNING);
    R.c.flags &= (uint8_t)~EXTRACE_CF_ATTACHED;
    EXPECT_EQ(extrace_cons_diag(&R.c, &dg), -EXTRACE_ENOTREADY);
}

TEST(resume_after_consumer_restart)
{
    struct extrace_shm_cstate *cs = extrace_blk_cstate(R.blk);
    uint32_t tail, gen;
    int n;

    xt_setup(&R, XT_DATA, 0);
    EXTRACE_INTS(T_SEQ, 1u, 0u);
    EXPECT_EQ(xt_drain_user(&R), 1);
    EXPECT_EQ(extrace_cons_ev_set(&R.c, EXTRACE_EV_T_OFF, EXTRACE_EVS_ON), 0);
    EXPECT_EQ(extrace_cons_set_notify(&R.c, 99), 0);
    tail = R.c.tail;
    gen = R.c.mask_gen;
    EXTRACE_INTS(T_SEQ, 2u, 0u);
    extrace_prod_flush();
    /* the consumer restarts: private state lost, cstate attached to this epoch */
    memset(&R.c, 0, sizeof(R.c));
    memset(R.mirror, 0, sizeof(R.mirror));
    EXPECT_EQ(xt_cons_attach(&R), 0);
    EXPECT_TRUE(R.c.flags & EXTRACE_CF_RESUMED);
    EXPECT_EQ(R.c.tail, tail);
    EXPECT_EQ(R.c.stream_pos, tail);
    EXPECT_EQ(R.c.mask_gen, gen);
    EXPECT_EQ(R.c.notify_thr, 99);
    {
        enum extrace_ev_state st;

        EXPECT_EQ(extrace_cons_ev_get(&R.c, EXTRACE_EV_T_OFF, &st), 0);
        EXPECT_EQ(st, EXTRACE_EVS_ON);
    }
    n = xt_drain_user(&R);
    ASSERT_EQ(n, 1); /* no loss, no duplicate */
    EXPECT_EQ(xt_int(&xt_recs[0], 0), 2);
    /* stopped inside a mask transaction: the resume closes it */
    cs->mask_gen = gen + 1;
    memset(&R.c, 0, sizeof(R.c));
    EXPECT_EQ(xt_cons_attach(&R), 0);
    EXPECT_EQ(R.c.mask_gen, gen + 2);
    EXPECT_EQ(cs->mask_gen, gen + 2);
}

TEST(resume_with_invalid_tail)
{
    xt_setup(&R, XT_DATA, 0);
    extrace_prod_flush();
    extrace_blk_cstate(R.blk)->tail = 0x80000000u;
    memset(&R.c, 0, sizeof(R.c));
    EXPECT_EQ(xt_cons_attach(&R), 0);
    EXPECT_EQ(R.c.tail, extrace_blk_pstate(R.blk)->head);
    EXPECT_EQ(R.c.stats.head_errors, 1);
}

TEST(new_epoch_keeps_overrides_of_the_same_image)
{
    xt_setup(&R, XT_DATA, 0);
    EXPECT_EQ(extrace_cons_ev_set(&R.c, EXTRACE_EV_T_INTS, EXTRACE_EVS_OFF), 0);
    extrace_prod_poll();
    EXPECT_FALSE(EXTRACE_ENABLED(T_INTS));
    EXPECT_EQ(xt_prod_start(&R, XT_DATA, 0), 0); /* producer restart, same image */
    EXPECT_TRUE(EXTRACE_ENABLED(T_INTS));        /* defaults until the consumer attached */
    extrace_prod_poll();
    EXPECT_TRUE(EXTRACE_ENABLED(T_INTS));
    EXPECT_EQ(extrace_cons_poll_state(&R.c), 1);
    EXPECT_FALSE(R.c.flags & EXTRACE_CF_RESUMED);
    extrace_prod_poll();
    EXPECT_FALSE(EXTRACE_ENABLED(T_INTS)); /* the overrides survive */
}

TEST(new_image_clears_overrides)
{
    static const uint8_t id_a[20] = { 1, 2, 3 }, id_b[20] = { 9, 9, 9 };
    enum extrace_ev_state st;

    xt_reset_world();
    memset(&R, 0, sizeof(R));
    extrace_posix_build_id = id_a;
    extrace_posix_build_id_len = sizeof(id_a);
    ASSERT_EQ(xt_prod_start(&R, XT_DATA, 0), 0);
    ASSERT_EQ(xt_cons_attach(&R), 0);
    EXPECT_MEM(R.c.pinfo.build_id, id_a, 20);
    EXPECT_EQ(extrace_cons_ev_set(&R.c, EXTRACE_EV_T_INTS, EXTRACE_EVS_OFF), 0);
    extrace_posix_build_id = id_b; /* the core restarts with another image */
    ASSERT_EQ(xt_prod_start(&R, XT_DATA, 0), 0);
    EXPECT_EQ(extrace_cons_poll_state(&R.c), 1);
    EXPECT_EQ(extrace_cons_ev_get(&R.c, EXTRACE_EV_T_INTS, &st), 0);
    EXPECT_EQ(st, EXTRACE_EVS_DEFAULT);
    EXPECT_EQ(extrace_blk_mask_clr(R.blk, 1)[0], 0);
    extrace_prod_poll();
    EXPECT_TRUE(EXTRACE_ENABLED(T_INTS));
    extrace_posix_build_id = nullptr;
    extrace_posix_build_id_len = 0;
}

TEST(peek_spans_and_budget_clamp)
{
    struct extrace_window w;
    uint32_t start;
    int n;

    xt_setup(&R, XT_DATA, 0);
    xt_drain_user(&R);
    /* push the tail close to the ring end so the next records wrap */
    while (((extrace_producer.head + 40) & (XT_DATA - 1)) > 40) {
        EXTRACE_INTS(T_SEQ, 0u, 0u);
        xt_drain_user(&R);
    }
    for (uint32_t i = 0; i < 20; i++)
        EXTRACE_INTS(T_SEQ, i, 0u); /* 7 bytes each */
    extrace_prod_flush();
    n = extrace_cons_peek(&R.c, &w, 1u << 20);
    ASSERT_TRUE(n > 0);
    EXPECT_EQ(w.nspan, 2);
    EXPECT_EQ(w.span[0].len + w.span[1].len, (uint32_t)n);
    EXPECT_EQ(w.span[1].p, R.c.data);
    EXPECT_EQ(w.start, R.c.tail);
    start = w.start;
    /* budget below one record: still one whole record */
    n = extrace_cons_peek(&R.c, &w, 1);
    EXPECT_EQ(n, 7);
    /* budget in the middle: whole records only */
    n = extrace_cons_peek(&R.c, &w, 30);
    EXPECT_EQ(n, 28);
    EXPECT_EQ(extrace_cons_release(&R.c, &w, 14), 0);
    EXPECT_EQ(R.c.tail, start + 14);
    EXPECT_EQ(w.start, start + 14); /* trimmed */
    EXPECT_EQ(extrace_window_len(&w), 14);
    EXPECT_EQ(extrace_cons_release(&R.c, &w, 15), -EXTRACE_EINVAL);
    EXPECT_EQ(extrace_cons_release(&R.c, &w, 14), 0);
    EXPECT_EQ(extrace_cons_release(&R.c, &w, 14), 0); /* empty window: no-op */
    EXPECT_EQ(R.c.tail, start + 28);
    EXPECT_EQ(extrace_blk_cstate(R.blk)->tail, R.c.tail);
    /* release across the wrap point */
    n = extrace_cons_peek(&R.c, &w, 1u << 20);
    ASSERT_TRUE(n > 0);
    EXPECT_EQ(extrace_cons_release(&R.c, &w, (uint32_t)n), 0);
    EXPECT_EQ(extrace_cons_peek(&R.c, &w, 100), 0);
}

TEST(read_copy_mode)
{
    uint8_t small[8];
    uint8_t big[64];
    int n;

    xt_setup(&R, XT_DATA, 0);
    xt_drain_user(&R);
    EXTRACE_BUF(T_BUF, "0123456789abcdef", 16); /* 19-byte record */
    extrace_prod_flush();
    EXPECT_EQ(extrace_cons_read(&R.c, small, sizeof(small)), -EXTRACE_ENOSPC);
    n = extrace_cons_read(&R.c, big, sizeof(big));
    EXPECT_TRUE(n >= 20 && n <= 24); /* header, delta, length, payload */
    EXPECT_MEM(big + n - 16, "0123456789abcdef", 16);
    EXPECT_EQ(extrace_cons_read(&R.c, big, sizeof(big)), 0);
}

TEST(head_errors)
{
    struct extrace_shm_pstate *ps = extrace_blk_pstate(R.blk);
    struct extrace_window w;

    xt_setup(&R, XT_DATA, 0);
    extrace_prod_flush();
    ps->head = R.c.tail + XT_DATA + 1; /* corrupt, epoch stable */
    EXPECT_EQ(extrace_cons_peek(&R.c, &w, 100), -EXTRACE_ECORRUPT);
    EXPECT_TRUE(R.c.flags & EXTRACE_CF_ERROR);
    EXPECT_EQ(R.c.tail, ps->head);
    EXPECT_EQ(R.c.stats.head_errors, 1);
    ps->head += XT_DATA + 1;
    extrace_blk_pinfo(R.blk)->magic = 0; /* producer re-initializing */
    EXPECT_EQ(extrace_cons_peek(&R.c, &w, 100), -EXTRACE_ENOTREADY);
    EXPECT_FALSE(extrace_cons_attached(&R.c));
    EXPECT_EQ(extrace_cons_peek(&R.c, &w, 100), -EXTRACE_ENOTREADY);
    EXPECT_EQ(xt_prod_start(&R, XT_DATA, 0), 0);
    EXPECT_EQ(extrace_cons_poll_state(&R.c), 1);
    EXPECT_TRUE(extrace_cons_peek(&R.c, &w, 1u << 20) > 0);
}

TEST(epoch_change_during_peek_and_release)
{
    struct extrace_window w;
    uint32_t e0;
    int n;

    xt_setup(&R, XT_DATA, 0);
    extrace_prod_flush();
    e0 = R.c.epoch;
    n = extrace_cons_peek(&R.c, &w, 1u << 20);
    ASSERT_TRUE(n > 0);
    EXPECT_EQ(xt_prod_start(&R, XT_DATA, 0), 0); /* restart while the window is held */
    EXPECT_EQ(extrace_cons_release(&R.c, &w, (uint32_t)n), -EXTRACE_EEPOCH); /* C6 */
    EXPECT_NE(R.c.epoch, e0);
    EXPECT_TRUE(extrace_cons_attached(&R.c)); /* re-attached */
    EXPECT_EQ(R.c.tail, 0);
    EXPECT_EQ(xt_prod_start(&R, XT_DATA, 0), 0);
    EXPECT_EQ(extrace_cons_peek(&R.c, &w, 100), -EXTRACE_EEPOCH);
    EXPECT_EQ(R.c.epoch, extrace_producer.epoch);
    EXPECT_EQ(R.c.stats.epoch_changes, 2);
}

TEST(event_control_api)
{
    uint32_t set[1] = { 1u << EXTRACE_EV_T_OFF }, clr[1] = { 1u << EXTRACE_EV_T_INTS };
    enum extrace_ev_state st;
    uint32_t gen;

    xt_setup(&R, XT_DATA, 0);
    gen = R.c.mask_gen;
    EXPECT_EQ(gen % 2, 0);
    EXPECT_EQ(extrace_cons_ev_set(&R.c, EXTRACE_EV_COUNT, EXTRACE_EVS_ON), -EXTRACE_EINVAL);
    EXPECT_EQ(extrace_cons_ev_set(&R.c, EXTRACE_EV_T_INTS, (enum extrace_ev_state)7),
              -EXTRACE_EINVAL);
    EXPECT_EQ(extrace_cons_ev_set(&R.c, EXTRACE_EV_T_INTS, EXTRACE_EVS_DEFAULT), 0);
    EXPECT_EQ(R.c.mask_gen, gen); /* no change, no transaction */
    EXPECT_EQ(extrace_cons_ev_set_many(&R.c, set, clr, 1), 0);
    EXPECT_EQ(R.c.mask_gen, gen + 2);
    EXPECT_EQ(extrace_cons_ev_set_many(&R.c, set, clr, 2), -EXTRACE_EINVAL);
    EXPECT_EQ(extrace_cons_ev_get(&R.c, EXTRACE_EV_T_OFF, &st), 0);
    EXPECT_EQ(st, EXTRACE_EVS_ON);
    EXPECT_EQ(extrace_cons_ev_get(&R.c, EXTRACE_EV_T_INTS, &st), 0);
    EXPECT_EQ(st, EXTRACE_EVS_OFF);
    EXPECT_EQ(extrace_cons_ev_get(&R.c, EXTRACE_EV_COUNT, &st), -EXTRACE_EINVAL);
    EXPECT_FALSE(extrace_cons_mask_applied(&R.c));
    extrace_prod_poll();
    EXPECT_TRUE(extrace_cons_mask_applied(&R.c));
    EXPECT_TRUE(EXTRACE_ENABLED(T_OFF));
    EXPECT_FALSE(EXTRACE_ENABLED(T_INTS));
    /* bits beyond ev_count are never stored */
    set[0] = 0xFFFFFFFFu;
    EXPECT_EQ(extrace_cons_ev_set_many(&R.c, set, nullptr, 1), 0);
    EXPECT_EQ(extrace_blk_mask_set(R.blk)[0], (1u << EXTRACE_EV_COUNT) - 1u);
    EXPECT_EQ(extrace_cons_ev_reset_all(&R.c), 0);
    EXPECT_EQ(extrace_blk_mask_set(R.blk)[0], 0);
    /* not attached */
    memset(&R.c, 0, sizeof(R.c));
    EXPECT_EQ(extrace_cons_ev_set(&R.c, 16, EXTRACE_EVS_ON), -EXTRACE_ENOTREADY);
    EXPECT_EQ(extrace_cons_cmd(&R.c, EXTRACE_CMD_SYNC), -EXTRACE_ENOTREADY);
    EXPECT_EQ(extrace_cons_set_notify(&R.c, 1), -EXTRACE_ENOTREADY);
    EXPECT_EQ(extrace_cons_set_publish_thr(&R.c, 1), -EXTRACE_ENOTREADY);
    EXPECT_EQ(extrace_cons_set_thr(&R.c, 1, 1), -EXTRACE_ENOTREADY);
    EXPECT_EQ(extrace_cons_ev_get(&R.c, 16, &st), -EXTRACE_ENOTREADY);
    EXPECT_EQ(extrace_cons_ev_set_many(&R.c, set, clr, 1), -EXTRACE_ENOTREADY);
    EXPECT_FALSE(extrace_cons_mask_applied(&R.c));
    {
        struct extrace_window w;

        EXPECT_EQ(extrace_cons_peek(&R.c, &w, 10), -EXTRACE_ENOTREADY);
        EXPECT_EQ(extrace_cons_poll_state(&R.c), -EXTRACE_EINVAL);
    }
}

TEST(rec_span_len_walk)
{
    const uint8_t pad1[] = { 0x00 };
    const uint8_t padn[] = { 0x04, 0x02, 0xEE, 0xEE };
    const uint8_t mark[] = { 0x81, 0x01 };
    const uint8_t none[] = { 0x80, 0x01, 0x85, 0x01 };
    const uint8_t ints[] = { 0x92, 0x01, 0x0C, 0x03, 0x02, 0x05, 0x05 };
    const uint8_t bad[] = { 0x01 };          /* ID 0, kind MARK */
    const uint8_t cut[] = { 0x92, 0x01, 0x0C, 0x03, 0x02 };

    EXPECT_EQ(extrace_rec_span_len(pad1, pad1 + 1), 1);
    EXPECT_EQ(extrace_rec_span_len(padn, padn + 4), 4);
    EXPECT_EQ(extrace_rec_span_len(padn, padn + 3), 0);
    EXPECT_EQ(extrace_rec_span_len(mark, mark + 2), 2);
    EXPECT_EQ(extrace_rec_span_len(none, none + 4), 4);
    EXPECT_EQ(extrace_rec_span_len(ints, ints + 7), 7);
    EXPECT_EQ(extrace_rec_span_len(bad, bad + 1), 0);
    EXPECT_EQ(extrace_rec_span_len(cut, cut + 5), 0);
    EXPECT_EQ(extrace_rec_span_len(ints, ints), 0);
}
