// SPDX-License-Identifier: MIT
/*
 * test_rec.c - record decoder: grammar, padding, timeline anchors, errors and
 * random input (DESIGN §5, §12.5, §18.5).
 */
#include "xt_util.h"
#include "extrace_wire.h"

static int dec_all(struct extrace_dec *d, const uint8_t *b, size_t n, struct extrace_rec *out,
                   int max, int *err)
{
    const uint8_t *p = b;
    int k = 0;

    *err = 0;
    while (k < max) {
        int r = extrace_dec_next(d, &p, b + n, &out[k]);

        if (r == 1) {
            k++;
            continue;
        }
        *err = r;
        break;
    }
    return k;
}

TEST(decode_stream_with_anchors)
{
    /* PAD1, SYNC(ts 1000), NONE(+5), MARK, PADN(3), LOST(count 4, ts 2000), INTS(+0) */
    const uint8_t s[] = {
        0x00,
        0x0A, 0x00, 0x03, 0x00, 0xE8, 0x07,
        0x80, 0x01, 0x05,
        0x81, 0x01,
        0x04, 0x01, 0xEE,
        0x12, 0x00, 0x04, 0x00, 0x04, 0xD0, 0x0F,
        0x92, 0x01, 0x00, 0x02, 0x00, 0x07,
    };
    struct extrace_rec r[8];
    struct extrace_dec d;
    uint64_t v[4];
    int n, err;

    extrace_dec_reset(&d, 3, 0);
    d.disp_ts = 0x500000000ull + 3000; /* dispatcher timeline: high word 5 */
    n = dec_all(&d, s, sizeof(s), r, 8, &err);
    ASSERT_EQ(n, 5);
    EXPECT_EQ(err, 0);
    EXPECT_EQ(r[0].event, EXTRACE_SYS_SYNC);
    EXPECT_EQ(r[0].ts, 0x500000000ull + 1000);
    EXPECT_TRUE(r[0].flags & EXTRACE_REC_HAS_TS);
    EXPECT_EQ(r[1].event, 16);
    EXPECT_EQ(r[1].kind, EXTRACE_K_NONE);
    EXPECT_EQ(r[1].ts, 0x500000000ull + 1005);
    EXPECT_EQ(r[2].kind, EXTRACE_K_MARK);
    EXPECT_EQ(r[2].ts, r[1].ts); /* MARK: last known time */
    EXPECT_EQ(r[3].event, EXTRACE_SYS_LOST);
    EXPECT_EQ(extrace_rec_ints(&r[3], v, 4, nullptr), 2);
    EXPECT_EQ(v[0], 4);
    EXPECT_EQ(r[3].ts, 0x500000000ull + 2000);
    EXPECT_TRUE(r[4].flags & EXTRACE_REC_AFTER_LOST);
    EXPECT_EQ(r[4].ts, r[3].ts);
    EXPECT_EQ(d.pad_bytes, 4);
    EXPECT_EQ(d.records, 5);
    ASSERT_EQ(extrace_rec_ints(&r[4], v, 4, nullptr), 1);
    EXPECT_EQ(v[0], 7);
}

TEST(decode_skip_keeps_time)
{
    /* SYNC(ts 1000), PAD1 + SKIP(+7, 2 bytes), NONE(+5): a cancelled reservation */
    const uint8_t s[] = {
        0x0A, 0x00, 0x03, 0x00, 0xE8, 0x07,
        0x00, EXTRACE_HDR_SKIP, 0x07, 0x02, 0xAA, 0xBB,
        0x80, 0x01, 0x05,
    };
    struct extrace_rec r[4];
    struct extrace_dec d;
    int err;

    extrace_dec_reset(&d, 0, 0);
    d.disp_ts = 3000;
    ASSERT_EQ(dec_all(&d, s, sizeof(s), r, 4, &err), 2); /* SKIP is not delivered */
    EXPECT_EQ(err, 0);
    EXPECT_EQ(r[0].event, EXTRACE_SYS_SYNC);
    EXPECT_EQ(r[1].event, 16);
    EXPECT_EQ(r[1].ts, 1012);
    EXPECT_EQ(d.records, 2);
}

TEST(decode_no_time_before_anchor)
{
    const uint8_t s[] = { 0x80, 0x01, 0x05, 0x80, 0x01, 0x05 };
    struct extrace_rec r[2];
    struct extrace_dec d;
    int err;

    extrace_dec_reset(&d, 0, 0);
    ASSERT_EQ(dec_all(&d, s, sizeof(s), r, 2, &err), 2);
    EXPECT_FALSE(r[0].flags & EXTRACE_REC_HAS_TS);
    EXPECT_FALSE(r[1].flags & EXTRACE_REC_HAS_TS);
}

TEST(decode_rebase_across_timer_wrap)
{
    /* the anchor was produced just before the 32-bit timer wrapped */
    EXPECT_EQ(extrace_rebase(0x100000010ull, 0xFFFFFFF0u), 0xFFFFFFF0ull);
    EXPECT_EQ(extrace_rebase(0x100000010ull, 0x00000005u), 0x100000005ull);
    EXPECT_EQ(extrace_rebase(0x7FFFFFFFFull, 0xFFFFFFFFu), 0x7FFFFFFFFull);
}

TEST(decode_errors)
{
    struct extrace_rec r;
    struct extrace_dec d;
    const uint8_t *p;
    static const uint8_t bad_id0[] = { 0x02, 0x00, 0x00 };          /* ID 0, kind INTS */
    static const uint8_t cut_hdr[] = { 0x92 };                      /* header continues */
    static const uint8_t cut_len[] = { 0x92, 0x01, 0x00, 0x05, 0x00 }; /* payload missing */
    static const uint8_t long_v[] = { 0x92, 0x81, 0x81, 0x81, 0x81, 0x81, 0x00 };
    static const uint8_t pad_cut[] = { 0x04, 0x80 };                /* PADN length cut */
    static const uint8_t pad_over[] = { 0x04, 0x10, 0xEE };         /* PADN beyond end */
    static const uint8_t sync_bad[] = { 0x0A, 0x00, 0x01, 0x05 };   /* sign mask must be 0 */
    static const uint8_t mark_big[] = { 0x81, 0x81, 0x20 };         /* MARK, ID 65552 */
    static const uint8_t none_big[] = { 0x80, 0x80, 0x20, 0x00 };   /* NONE, ID 65536 */

    extrace_dec_reset(&d, 0, 0);
    p = bad_id0;
    EXPECT_EQ(extrace_dec_next(&d, &p, bad_id0 + 3, &r), -EXTRACE_ECORRUPT);
    EXPECT_EQ(p, bad_id0);
    p = cut_hdr;
    EXPECT_EQ(extrace_dec_next(&d, &p, cut_hdr + 1, &r), -EXTRACE_ENOSPC);
    p = cut_len;
    EXPECT_EQ(extrace_dec_next(&d, &p, cut_len + 5, &r), -EXTRACE_ENOSPC);
    EXPECT_EQ(p, cut_len);
    p = long_v;
    EXPECT_EQ(extrace_dec_next(&d, &p, long_v + 7, &r), -EXTRACE_ECORRUPT);
    p = pad_cut;
    EXPECT_EQ(extrace_dec_next(&d, &p, pad_cut + 2, &r), -EXTRACE_ENOSPC);
    p = pad_over;
    EXPECT_EQ(extrace_dec_next(&d, &p, pad_over + 3, &r), 0);
    EXPECT_EQ(d.pad_skip, 15);
    p = sync_bad;
    EXPECT_EQ(extrace_dec_next(&d, &p, sync_bad + 4, &r), -EXTRACE_ECORRUPT);
    /* event IDs above 0xFFFF are corrupt in every kind, not only length-bearing ones */
    p = mark_big;
    EXPECT_EQ(extrace_dec_next(&d, &p, mark_big + 3, &r), -EXTRACE_ECORRUPT);
    EXPECT_EQ(p, mark_big);
    p = none_big;
    EXPECT_EQ(extrace_dec_next(&d, &p, none_big + 4, &r), -EXTRACE_ECORRUPT);
    EXPECT_EQ(d.errors, 5);
    /* max_record bounds the record length */
    extrace_dec_reset(&d, 0, 4);
    p = cut_len;
    EXPECT_EQ(extrace_dec_next(&d, &p, cut_len + 5, &r), -EXTRACE_ECORRUPT);
}

TEST(decode_helpers)
{
    const uint8_t ints[] = { 0x02, 0x05, 0x05, 0x81, 0x01 };
    const uint8_t pf[] = { 0xA3, 0x03, 0x07, 0x01 };
    struct extrace_rec r = { .kind = EXTRACE_K_INTS, .payload = ints, .len = sizeof(ints) };
    uint64_t v[2];
    uint32_t sm, site, alen;
    const uint8_t *args;

    EXPECT_EQ(extrace_rec_ints(&r, v, 2, &sm), 3); /* more values than room */
    EXPECT_EQ(sm, 2);
    EXPECT_EQ(v[0], 5);
    EXPECT_EQ(v[1], (uint64_t)-3);
    r.kind = EXTRACE_K_BUF;
    EXPECT_EQ(extrace_rec_ints(&r, v, 2, &sm), -EXTRACE_ECORRUPT);
    EXPECT_EQ(extrace_rec_printf_site(&r, &site, &args, &alen), -EXTRACE_ECORRUPT);
    r = (struct extrace_rec){ .kind = EXTRACE_K_PRINTF, .payload = pf, .len = sizeof(pf) };
    EXPECT_EQ(extrace_rec_printf_site(&r, &site, &args, &alen), 0);
    EXPECT_EQ(site, 0x1A3);
    EXPECT_EQ(alen, 2);
    EXPECT_EQ(args, pf + 2);
}

TEST(decode_random_input)
{
    uint64_t s = xt_seed() ^ 0xFEED;
    static uint8_t b[512];

    for (unsigned long it = 0; it < xt_iters(20000); it++) {
        struct extrace_dec d;
        const uint8_t *p = b;
        size_t n = 1 + xt_rand_below(&s, sizeof(b));
        unsigned int guard = 0;

        for (size_t i = 0; i < n; i++)
            b[i] = (uint8_t)(xt_rand(&s) >> (8 * (i & 3)));
        extrace_dec_reset(&d, 0, xt_rand_below(&s, 2) ? 600 : 0);
        d.disp_ts = xt_rand(&s);
        while (p < b + n && guard++ < n + 1) {
            struct extrace_rec r;
            const uint8_t *before = p;
            int ret = extrace_dec_next(&d, &p, b + n, &r);

            if (ret != 1)
                break;
            EXPECT_TRUE(p > before);
            EXPECT_TRUE(r.payload >= b && r.payload + r.len <= b + n);
            if (r.kind == EXTRACE_K_INTS) {
                uint64_t v[16];

                extrace_rec_ints(&r, v, 16, nullptr);
            }
        }
        (void)extrace_rec_span_len(b, b + n);
    }
}
