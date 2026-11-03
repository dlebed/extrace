// SPDX-License-Identifier: MIT
/*
 * test_ftrace.c - function entry tracing, the generic part: the switch, the
 * flavors and their record shapes on the wire, and the decoder (docs/ftrace.md).
 * extrace_ftrace_hit() reaches the trampolines' targets; the trampolines run
 * under an emulator (tests/ftrace), the patcher in test_ftrace_patch.h.
 */
#include "xt_util.h"
#include "extrace_ftrace.h"

#if EXTRACE_CFG_FTRACE
static struct xt_ring R;

#define F_CALLER EXTRACE_FTRACE_CALLER
#define F_ARGS EXTRACE_FTRACE_ARGS

/* The saved argument registers, and the caller's return address. */
static uint32_t xt_args[EXTRACE_FTRACE_ARG_REGS];
static const uint32_t xt_regs[8] = { 5, 300, 0, 0xFFFFFFFFu, 4, 5, 6, 7 };
#define XT_CALLER 0x1A47u

/* Start and drain the stream start; the switch on as a consumer turns it on. */
static void ft_setup(bool on)
{
    for (unsigned int i = 0; i < EXTRACE_FTRACE_ARG_REGS; i++)
        xt_args[i] = xt_regs[i];
    xt_setup(&R, XT_DATA, 0);
    if (on) {
        EXPECT_EQ(extrace_cons_ev_set(&R.c, EXTRACE_SYS_FTRACE, EXTRACE_EVS_ON), 0);
        extrace_prod_poll(); /* applies the mask and writes CFG */
    }
    xt_drain_user(&R);
}

/* Records published since the last drain, system ones included. */
static int ft_count(void)
{
    struct extrace_dec d;

    extrace_prod_flush();
    extrace_dec_reset(&d, R.c.ring_id, 0);
    return xt_drain(&R, &d, true);
}

/* One hit of flavor f 12 ticks after the previous record; compares the published bytes. */
static void golden(uintptr_t ret, unsigned int f, const char *bytes, uint32_t n)
{
    struct extrace_window w;

    xt_step = 12;
    extrace_producer.last_ts = xt_now;
    extrace_ftrace_hit(ret, XT_CALLER, xt_args, f);
    extrace_prod_flush();
    ASSERT_EQ(extrace_cons_peek(&R.c, &w, 1u << 20), (int)n);
    EXPECT_MEM(w.span[0].p, bytes, n);
    extrace_cons_release(&R.c, &w, n);
}

TEST(ftrace_switch_off_by_default)
{
    ft_setup(false);
    extrace_ftrace_hit(0x1234, XT_CALLER, xt_args, EXTRACE_FTRACE_NONE);
    extrace_ftrace_hit(0x1234, XT_CALLER, xt_args, F_ARGS(1));
    EXPECT_EQ(ft_count(), 0);
}

TEST(ftrace_golden_shapes)
{
    ft_setup(true);
    golden(0x1234, EXTRACE_FTRACE_MARK, "\x21\xB4\x24", 3);     /* header, address */
    golden(0x1234, EXTRACE_FTRACE_NONE, "\x20\x0C\xB4\x24", 4); /* header, tsd, address */
    golden(0x1234, F_ARGS(2), "\x22\x0C\x06\x00\xB4\x24\x05\xAC\x02", 9); /* INTS */
    golden(0x7F, F_ARGS(1), "\x22\x0C\x03\x00\x7F\x05", 6);
    golden(0x7F, F_ARGS(4), "\x22\x0C\x0B\x00\x7F\x05\xAC\x02\x00\xFF\xFF\xFF\xFF\x0F", 14);
}

/* Each call has its own flavor: one function per flavor, interleaved. */
TEST(ftrace_flavor_per_call)
{
    struct extrace_dec d;
    uint32_t a, c, args[8];

    ft_setup(true);
    for (unsigned int n = 1; n <= EXTRACE_FTRACE_ARG_REGS; n++) {
        extrace_ftrace_hit(0x100u + n, XT_CALLER, xt_args, EXTRACE_FTRACE_NONE);
        extrace_ftrace_hit(0x200u + n, XT_CALLER, xt_args, F_ARGS(n));
        extrace_ftrace_hit(0x300u + n, XT_CALLER, xt_args, EXTRACE_FTRACE_MARK);
    }
    extrace_dec_reset(&d, R.c.ring_id, 0);
    extrace_prod_flush();
    ASSERT_EQ(xt_drain(&R, &d, true), 3 * EXTRACE_FTRACE_ARG_REGS);
    for (unsigned int n = 1; n <= EXTRACE_FTRACE_ARG_REGS; n++) {
        const struct extrace_rec *r0 = &xt_recs[3 * n - 3].r, *r1 = &xt_recs[3 * n - 2].r,
                                 *r2 = &xt_recs[3 * n - 1].r;

        EXPECT_EQ(r0->kind, EXTRACE_K_NONE);
        EXPECT_EQ(extrace_rec_ftrace(r0, &a, nullptr, args, 8), 0);
        EXPECT_EQ(a, 0x100u + n);
        EXPECT_EQ(r1->kind, EXTRACE_K_INTS);
        ASSERT_EQ(extrace_rec_ftrace(r1, &a, &c, args, 8), (int)n);
        EXPECT_EQ(a, 0x200u + n);
        EXPECT_EQ(c, 0);
        EXPECT_MEM(args, xt_regs, sizeof(uint32_t) * n);
        EXPECT_EQ(r2->kind, EXTRACE_K_MARK);
        EXPECT_EQ(extrace_rec_ftrace(r2, &a, nullptr, args, 8), 0);
        EXPECT_EQ(a, 0x300u + n);
    }
}

/* Not a flavor of this build: nothing is recorded, nothing is read. */
TEST(ftrace_unknown_flavor_records_nothing)
{
    const unsigned int bad[] = { EXTRACE_FTRACE_OFF, F_ARGS(EXTRACE_FTRACE_ARG_REGS + 1), 0x1F,
                                 0x20, 0xFFFFFFFFu, F_CALLER,
                                 F_ARGS(EXTRACE_FTRACE_ARG_REGS + 1) | F_CALLER };

    ft_setup(true);
    for (unsigned int k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
        extrace_ftrace_hit(0x1234, XT_CALLER, nullptr, bad[k]);
#if !EXTRACE_CFG_FTRACE_CALLER
    extrace_ftrace_hit(0x1234, XT_CALLER, nullptr, EXTRACE_FTRACE_NONE | F_CALLER); /* not built */
    extrace_ftrace_hit(0x1234, XT_CALLER, nullptr, F_ARGS(1) | F_CALLER);
#endif
    EXPECT_EQ(ft_count(), 0);
}

TEST(ftrace_decode_every_shape)
{
    struct extrace_dec d;
    uint32_t a, c = 1, args[8];
    uint64_t ts1;
    int n;

    ft_setup(true);
    xt_step = 10;
    extrace_ftrace_hit(0x20001007u, XT_CALLER, xt_args, EXTRACE_FTRACE_NONE);
    extrace_ftrace_hit(0x20002003u, XT_CALLER, xt_args, EXTRACE_FTRACE_MARK);
    extrace_ftrace_hit(0xFFFFFFFEu, XT_CALLER, xt_args, F_ARGS(EXTRACE_FTRACE_ARG_REGS));
    extrace_dec_reset(&d, R.c.ring_id, 0);
    extrace_prod_flush();
    ASSERT_EQ(xt_drain(&R, &d, true), 3);

    EXPECT_EQ(xt_recs[0].r.event, EXTRACE_SYS_FTRACE);
    EXPECT_EQ(xt_recs[0].r.kind, EXTRACE_K_NONE);
    EXPECT_EQ(extrace_rec_ftrace(&xt_recs[0].r, &a, &c, args, 8), 0);
    EXPECT_EQ(a, 0x20001007u);
    EXPECT_EQ(c, 0);
    ts1 = xt_recs[0].r.ts;
    EXPECT_EQ(xt_recs[1].r.kind, EXTRACE_K_MARK); /* no timestamp of its own */
    EXPECT_EQ(xt_recs[1].r.ts, ts1);
    EXPECT_EQ(extrace_rec_ftrace(&xt_recs[1].r, &a, nullptr, args, 8), 0);
    EXPECT_EQ(a, 0x20002003u);
    EXPECT_EQ(xt_recs[2].r.kind, EXTRACE_K_INTS);
    EXPECT_TRUE(xt_recs[2].r.ts > ts1);
    n = extrace_rec_ftrace(&xt_recs[2].r, &a, &c, args, 8);
    ASSERT_EQ(n, EXTRACE_FTRACE_ARG_REGS);
    EXPECT_EQ(a, 0xFFFFFFFEu);
    EXPECT_MEM(args, xt_regs, sizeof(uint32_t) * (size_t)n);
    EXPECT_EQ(c, 0); /* no caller field */
    EXPECT_EQ(extrace_rec_ftrace(&xt_recs[2].r, &a, &c, args, 1), n); /* max caps the copy */
}

TEST(ftrace_decoder_rejects_truncated_address)
{
    static const uint8_t cut[] = { 0x21, 0xB4 }, bad[] = { 0x21, 0x80, 0x80, 0x80, 0x80, 0x80 };
    struct extrace_dec d;
    struct extrace_rec r;
    const uint8_t *p = cut;

    extrace_dec_reset(&d, 1, 0);
    EXPECT_EQ(extrace_dec_next(&d, &p, cut + sizeof(cut), &r), -EXTRACE_ENOSPC);
    EXPECT_EQ(p, cut);
    EXPECT_EQ(extrace_rec_span_len(cut, cut + sizeof(cut)), 0);
    EXPECT_EQ(extrace_rec_span_len(cut, cut + 2), 0);
    p = bad;
    EXPECT_EQ(extrace_dec_next(&d, &p, bad + sizeof(bad), &r), -EXTRACE_ECORRUPT);
    p = (const uint8_t *)"\x20\x0C\xB4\x24\x00";
    EXPECT_EQ(extrace_rec_span_len(p, p + 5), 4);
}

#if EXTRACE_CFG_FTRACE_CALLER
TEST(ftrace_caller_golden_shapes)
{
    ft_setup(true);
    golden(0x1234, EXTRACE_FTRACE_MARK | F_CALLER, "\x41\xB4\x24\xC7\x34", 5); /* + caller */
    golden(0x1234, EXTRACE_FTRACE_NONE | F_CALLER, "\x40\x0C\xB4\x24\xC7\x34", 6);
    golden(0x1234, F_ARGS(2) | F_CALLER, "\x42\x0C\x08\x00\xB4\x24\xC7\x34\x05\xAC\x02", 11);
    golden(0x7F, F_ARGS(1) | F_CALLER, "\x42\x0C\x05\x00\x7F\xC7\x34\x05", 8);
    /* the next function without the caller: FTRACE again */
    golden(0x1234, EXTRACE_FTRACE_NONE, "\x20\x0C\xB4\x24", 4);
}

TEST(ftrace_caller_switch_off)
{
    ft_setup(false);
    extrace_ftrace_hit(0x1234, XT_CALLER, nullptr, EXTRACE_FTRACE_NONE | F_CALLER); /* the slot is not read */
    EXPECT_EQ(ft_count(), 0);
}

TEST(ftrace_caller_decode_every_shape)
{
    struct extrace_dec d;
    uint32_t a, c, args[8];
    int n;

    ft_setup(true);
    extrace_ftrace_hit(0x20001007u, XT_CALLER, xt_args, EXTRACE_FTRACE_NONE | F_CALLER);
    extrace_ftrace_hit(0x20002003u, XT_CALLER, xt_args, EXTRACE_FTRACE_MARK | F_CALLER);
    extrace_ftrace_hit(0xFFFFFFFEu, XT_CALLER, xt_args, F_ARGS(EXTRACE_FTRACE_ARG_REGS) | F_CALLER);
    extrace_dec_reset(&d, R.c.ring_id, 0);
    extrace_prod_flush();
    ASSERT_EQ(xt_drain(&R, &d, true), 3);

    for (int i = 0; i < 3; i++)
        EXPECT_EQ(xt_recs[i].r.event, EXTRACE_SYS_FTRACE_CALLER);
    EXPECT_EQ(xt_recs[0].r.kind, EXTRACE_K_NONE);
    EXPECT_EQ(extrace_rec_ftrace(&xt_recs[0].r, &a, &c, args, 8), 0);
    EXPECT_EQ(a, 0x20001007u);
    EXPECT_EQ(c, 0x1A47u);
    EXPECT_EQ(xt_recs[1].r.kind, EXTRACE_K_MARK);
    EXPECT_EQ(xt_recs[1].r.ts, xt_recs[0].r.ts);
    EXPECT_EQ(extrace_rec_ftrace(&xt_recs[1].r, &a, &c, args, 8), 0);
    EXPECT_EQ(a, 0x20002003u);
    EXPECT_EQ(c, 0x1A47u);
    EXPECT_EQ(xt_recs[2].r.kind, EXTRACE_K_INTS);
    c = 0;
    n = extrace_rec_ftrace(&xt_recs[2].r, &a, &c, args, 8);
    ASSERT_EQ(n, EXTRACE_FTRACE_ARG_REGS);
    EXPECT_EQ(a, 0xFFFFFFFEu);
    EXPECT_EQ(c, 0x1A47u);
    EXPECT_MEM(args, xt_regs, sizeof(uint32_t) * (size_t)n);
}

TEST(ftrace_caller_decoder_rejects_truncated_caller)
{
    static const uint8_t cut[] = { 0x41, 0xB4, 0x24, 0xC7 }, one[] = { 0x41, 0x24 };
    struct extrace_dec d;
    struct extrace_rec r;
    const uint8_t *p = cut;
    uint32_t a, c;

    extrace_dec_reset(&d, 1, 0);
    EXPECT_EQ(extrace_dec_next(&d, &p, cut + sizeof(cut), &r), -EXTRACE_ENOSPC);
    EXPECT_EQ(p, cut);
    EXPECT_EQ(extrace_rec_span_len(cut, cut + sizeof(cut)), 0);
    p = (const uint8_t *)"\x41\xB4\x24\xC7\x34\x00";
    EXPECT_EQ(extrace_rec_span_len(p, p + 6), 5);
    p = (const uint8_t *)"\x40\x0C\xB4\x24\xC7\x34\x00";
    EXPECT_EQ(extrace_rec_span_len(p, p + 7), 6);
    p = one; /* an FTRACE_CALLER record whose payload holds the address only */
    r = (struct extrace_rec){ .event = EXTRACE_SYS_FTRACE_CALLER, .kind = EXTRACE_K_MARK,
                              .payload = one + 1, .len = 1 };
    EXPECT_EQ(extrace_rec_ftrace(&r, &a, &c, nullptr, 0), -EXTRACE_ECORRUPT);
}
#endif

TEST(ftrace_drop_is_counted)
{
    ft_setup(true);
    for (int i = 0; i < 2000; i++) /* fill the ring: nothing is consumed */
        extrace_ftrace_hit(0x20001007u, XT_CALLER, xt_args, F_ARGS(1));
    EXPECT_NE(extrace_producer.lost_total, 0);
}
#endif
