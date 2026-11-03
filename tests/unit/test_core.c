// SPDX-License-Identifier: MIT
/*
 * test_core.c - wire helpers, block layout, catalog expansion and argument
 * classification (DESIGN §4, §5, §6, §8.12, §18.1).
 */
#include "xt_util.h"

/* ---- wire ---- */

TEST(wire_header_encoding)
{
    uint8_t b[8];
    uint32_t h;

    /* 1-byte system headers */
    EXPECT_EQ(EXTRACE_HDR_SYNC, 0x0A);
    EXPECT_EQ(EXTRACE_HDR_LOST, 0x12);
    EXPECT_EQ(EXTRACE_HDR_INFO, 0x1C);
    EXPECT_EQ(EXTRACE_HDR_FRAG, 0x3F);
    /* §5.11: ID 40 NONE -> C0 02, ID 40 MARK -> C1 02 */
    h = EXTRACE_HENC(40, EXTRACE_K_NONE);
    EXPECT_EQ(extrace_henc_len(h), 2);
    EXPECT_EQ(h & 0xFFFF, 0x02C0);
    h = EXTRACE_HENC(40, EXTRACE_K_MARK);
    EXPECT_EQ(h & 0xFFFF, 0x02C1);
    EXPECT_EQ(extrace_henc_len(EXTRACE_HENC(2047, 7)), 2);
    EXPECT_EQ(extrace_henc_len(EXTRACE_HENC(2048, 0)), 3);
    EXPECT_EQ(extrace_henc_len(EXTRACE_HENC(65534, 7)), 3);
    EXPECT_EQ(extrace_henc(2048, 2), EXTRACE_HENC(2048, 2));

    /* 2-byte header: no third store (review finding 5.1) */
    memset(b, 0xEE, sizeof(b));
    EXPECT_EQ(extrace_put_hdr(b, EXTRACE_HENC(20, EXTRACE_K_INTS)) - b, 2);
    EXPECT_EQ(b[0], 0xA2);
    EXPECT_EQ(b[1], 0x01);
    EXPECT_EQ(b[2], 0xEE);
    /* the urgent flag and a sign mask do not leak into the bytes */
    memset(b, 0xEE, sizeof(b));
    EXPECT_EQ(extrace_put_hdr(b, EXTRACE_HENC(20, 2) | EXTRACE_HENC_URGENT | (5u << 24)) - b, 2);
    EXPECT_EQ(b[2], 0xEE);
    /* 3-byte header */
    memset(b, 0xEE, sizeof(b));
    EXPECT_EQ(extrace_put_hdr(b, EXTRACE_HENC(5000, EXTRACE_K_BUF)) - b, 3);
    {
        uint32_t v = 0;

        EXPECT_EQ(extrace_get_uv32(b, b + 3, &v), 3);
        EXPECT_EQ(v, (5000u << 3) | EXTRACE_K_BUF);
        EXPECT_EQ(b[3], 0xEE);
    }
}

TEST(wire_padding_every_gap)
{
    static uint8_t b[70000];

    for (uint32_t gap = 1; gap < 66000; gap += (gap < 20000 ? 1 : 97)) {
        struct extrace_dec d;
        struct extrace_rec rec;
        const uint8_t *p = b;
        uint32_t stored = 0;

        memset(b, 0xEE, gap + 8);
        extrace_put_pad(b, gap);
        for (uint32_t i = 0; i < gap; i++)
            stored += b[i] != 0xEE;
        if (!EXPECT_TRUE(stored <= 5) || !EXPECT_EQ(extrace_rec_span_len(b, b + gap), gap))
            return;
        extrace_dec_reset(&d, 0, 0);
        if (!EXPECT_EQ(extrace_dec_next(&d, &p, b + gap, &rec), 0) || !EXPECT_EQ(p - b, gap))
            return;
    }
    /* gaps without a minimal encoding are written over-long (§5.8) */
    extrace_put_pad(b, 130);
    EXPECT_MEM(b, "\x04\xFF\x00", 3);
    extrace_put_pad(b, 16387);
    EXPECT_MEM(b, "\x04\xFF\xFF\x00", 4);
    extrace_put_pad(b, 2);
    EXPECT_MEM(b, "\x04\x00", 2);
    extrace_put_pad(b, 1);
    EXPECT_EQ(b[0], 0x00);
}

/* ---- layout ---- */

TEST(layout_offsets)
{
    EXPECT_EQ(EXTRACE_CTL_SIZE(1), 136);
    EXPECT_EQ(EXTRACE_CTL_SIZE(2), 144);
    EXPECT_EQ(EXTRACE_CTL_SIZE(10), 208);
    EXPECT_EQ(EXTRACE_CTL_SIZE(32), 384);
    EXPECT_EQ(EXTRACE_MASK_CLR_OFF(2), 136);
    EXPECT_EQ(EXTRACE_RING_BLOCK_SIZE(4096, 2), 4096 + 144);
    EXPECT_EQ(EXTRACE_RING_BLOCK_SIZE_MAX(4096), 4096 + 128 + 8 * ((EXTRACE_CFG_MAX_EVENTS + 31) / 32));
    for (uint32_t w = 1; w <= 32; w++)
        EXPECT_EQ(EXTRACE_CTL_SIZE(w) % 8, 0); /* the data area is 8-byte aligned */
}

TEST(layout_pinfo_image)
{
    struct extrace_shm_pinfo a = {
        .magic = EXTRACE_SHM_MAGIC, .layout_ver = 0x0200, .wire_ver = 1, .flags = 0x08,
        .ring_id = 0x42, .reserved0 = 0, .reserved2 = 0x1234, .data_off = 144,
        .data_size = 4096, .mask_words = 2, .ev_count = 60, .max_record = 523,
        .ts_freq = 1000000, .reserved1 = 0xDEADBEEF, .name = "dsp0",
    }, b;
    uint32_t w[EXTRACE_PINFO_WORDS];
    uint8_t bytes[64];

    for (unsigned int i = 0; i < 20; i++)
        a.build_id[i] = (uint8_t)(i * 13 + 1);
    extrace_pinfo_pack(&a, w);
    /* the words are the little-endian byte image of the documented layout */
    for (unsigned int i = 0; i < EXTRACE_PINFO_WORDS; i++)
        extrace_le32_bytes(&bytes[4 * i], w[i]);
    EXPECT_EQ(bytes[0], 0x45); /* "EXTR" */
    EXPECT_EQ(bytes[3], 0x52);
    EXPECT_EQ(bytes[4], 0x00); /* layout_ver 0x0200, LE */
    EXPECT_EQ(bytes[5], 0x02);
    EXPECT_EQ(bytes[6], 1);
    EXPECT_EQ(bytes[7], 0x08);
    EXPECT_EQ(bytes[8], 0x42);
    EXPECT_EQ(bytes[10], 0x34);
    EXPECT_EQ(bytes[11], 0x12);
    EXPECT_EQ(bytes[22], 60);
    EXPECT_EQ(bytes[36], 1);
    EXPECT_MEM(&bytes[56], "dsp0", 4);
#if !EXTRACE_BIG_ENDIAN
    EXPECT_MEM(bytes, &a, sizeof(a)); /* on little-endian agents the image is the struct */
#endif
    extrace_pinfo_unpack(&b, w);
    EXPECT_MEM(&b, &a, sizeof(a));
}

TEST(layout_shm_words_little_endian)
{
    uint32_t word;
    uint8_t *b = (uint8_t *)&word;

    extrace_shm_st32(&word, 0x11223344u);
    EXPECT_EQ(b[0], 0x44);
    EXPECT_EQ(b[3], 0x11);
    EXPECT_EQ(extrace_shm_ld32(&word), 0x11223344u);
    EXPECT_EQ(extrace_rbit32(0x00000001u), 0x80000000u);
    EXPECT_EQ(extrace_rbit32(0x12345678u), 0x1E6A2C48u);
}

TEST(index_arithmetic)
{
    EXPECT_EQ(extrace_idx_used(0x00000010u, 0xFFFFFFF0u), 0x20);
    EXPECT_EQ(extrace_idx_off(0xFFFFFFF0u, 63), 0x30);
    EXPECT_EQ(extrace_idx_off(0x00000010u, 63), 0x10);
    EXPECT_TRUE(extrace_is_pow2(64));
    EXPECT_FALSE(extrace_is_pow2(96));
    EXPECT_FALSE(extrace_is_pow2(0));
}

/* ---- catalog ---- */

TEST(catalog_ids_and_switches)
{
    EXPECT_EQ(EXTRACE_EV_T_MARK, 16);
    EXPECT_EQ(EXTRACE_EV_T_STRUCT, 29);
    EXPECT_EQ(EXTRACE_EV_COUNT, 30);
    EXPECT_EQ(EXTRACE_MASK_WORDS, 1);
    EXPECT_EQ(EXTRACE_KIND_T_ANY, EXTRACE_K_ANY);
    EXPECT_EQ(EXTRACE_KIND_T_STRUCT, EXTRACE_K_STRUCT);
    EXPECT_EQ(EXTRACE_CT_T_MARK, 1);
    EXPECT_EQ(EXTRACE_CT_T_DEBUG, 0);  /* group compiled out */
    EXPECT_EQ(EXTRACE_CT_T_FORCED, 1); /* -DEXTRACE_GRP_FORCED_ON=1 overrides the catalog */
    EXPECT_EQ(sizeof(extrace_type_T_STRUCT), sizeof(struct test_struct));
}

TEST(catalog_defaults_bitmap)
{
    uint32_t want = 0;

    want |= 1u << EXTRACE_EV_T_MARK;
    want |= 1u << EXTRACE_EV_T_NONE;
    want |= 1u << EXTRACE_EV_T_INTS;
    want |= 1u << EXTRACE_EV_T_PRINTF;
    want |= 1u << EXTRACE_EV_T_BUF;
    want |= 1u << EXTRACE_EV_T_STR;
    want |= 1u << EXTRACE_EV_T_ANY;
    want |= 1u << EXTRACE_EV_T_DEBUG; /* compiled out, still default-on */
    want |= 1u << EXTRACE_EV_T_FORCED;
    want |= 1u << EXTRACE_EV_T_SEQ;
    want |= 1u << EXTRACE_EV_T_ISR;
    want |= 1u << EXTRACE_EV_T_STRUCT;
    EXPECT_EQ(extrace_ev_default_word(0), want);
}

static int side_effects;

static uint32_t touch(void)
{
    side_effects++;
    return 1;
}

TEST(disabled_events_do_not_evaluate_arguments)
{
    static struct xt_ring r;
    struct extrace_resv rv;

    xt_setup(&r, XT_DATA, 0);
    side_effects = 0;
    EXTRACE_INTS(T_OFF, touch());     /* default off */
    EXTRACE_INTS(T_DEBUG, touch());   /* compiled out */
    extrace_producer.mask[0] &= ~(1u << EXTRACE_EV_T_BUF);
    EXPECT_FALSE(EXTRACE_RESERVE(T_BUF, touch(), 1, &rv));
    extrace_producer.mask[0] |= 1u << EXTRACE_EV_T_BUF;
    EXPECT_EQ(side_effects, 0);
    EXTRACE_INTS(T_INTS, touch());
    EXPECT_EQ(side_effects, 1);
    EXPECT_FALSE(EXTRACE_ENABLED(T_OFF));
    EXPECT_TRUE(EXTRACE_ENABLED(T_INTS));
    EXPECT_FALSE(EXTRACE_ENABLED(T_DEBUG));
}

TEST(before_init_calls_are_no_ops)
{
    xt_reset_world();
    EXTRACE_INTS(T_INTS, 1u); /* mask is all-zero in .bss */
    EXTRACE_MARK(T_MARK);
    extrace_prod_flush();
    extrace_prod_poll();
    EXPECT_EQ(extrace_producer.head, 0);
    EXPECT_FALSE(extrace_emit_dyn_none(EXTRACE_EV_T_NONE));
}

/* ---- argument classification (§8.12) ---- */

enum small_enum { SE_A = 1 };

TEST(argument_classes)
{
    char c = 0;
    long l = 0;
    unsigned long ul = 0;
    const char *s = "x";
    int *ip = nullptr;
    float f = 0;
    double d = 0;
    bool b = false;

    EXPECT_EQ(EXTRACE_AC(b), EXTRACE_AC_U32);
    EXPECT_EQ(EXTRACE_AC(c), EXTRACE_AC_S32);
    EXPECT_EQ(EXTRACE_AC((short)1), EXTRACE_AC_S32);
    EXPECT_EQ(EXTRACE_AC(1), EXTRACE_AC_S32);
    EXPECT_EQ(EXTRACE_AC(1u), EXTRACE_AC_U32);
    EXPECT_EQ(EXTRACE_AC((uint8_t)1), EXTRACE_AC_U32);
    EXPECT_EQ(EXTRACE_AC(1ll), EXTRACE_AC_S64);
    EXPECT_EQ(EXTRACE_AC(1ull), EXTRACE_AC_U64);
    EXPECT_EQ(EXTRACE_AC(l), sizeof(long) == 8 ? EXTRACE_AC_S64 : EXTRACE_AC_S32);
    EXPECT_EQ(EXTRACE_AC(ul), sizeof(long) == 8 ? EXTRACE_AC_U64 : EXTRACE_AC_U32);
    EXPECT_EQ(EXTRACE_AC(f), EXTRACE_AC_F32);
    EXPECT_EQ(EXTRACE_AC(d), EXTRACE_AC_F64);
    EXPECT_EQ(EXTRACE_AC(s), EXTRACE_AC_STR);
    EXPECT_EQ(EXTRACE_AC("lit"), EXTRACE_AC_STR);
    EXPECT_EQ(EXTRACE_AC(ip), EXTRACE_AC_PTR);
    EXPECT_EQ(EXTRACE_AC(EXTRACE_CSTR(__func__)), EXTRACE_AC_CSTR);
    EXPECT_EQ(EXTRACE_AC(SE_A), EXTRACE_AC_S32);
    EXPECT_EQ(EXTRACE_NARGS(a), 1);
    EXPECT_EQ(EXTRACE_NARGS(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p), 16);
    EXPECT_EQ(EXTRACE_SIG_LO(1u, -1, 2ll), 0x421u);
    EXPECT_EQ(EXTRACE_SIGN_MASK(1u, -1, 2ll), 0x6u);
    EXPECT_EQ(EXTRACE_SIG_HI(0, 1, 2, 3, 4, 5, 6, 7, 8u, 9), 0x21u);
    EXPECT_TRUE(EXTRACE_ANY64(1, 2ull));
    EXPECT_FALSE(EXTRACE_ANY64(1, 2u, f));
    EXPECT_TRUE(EXTRACE_ALL_INT(1, 2u, c));
    EXPECT_FALSE(EXTRACE_ALL_INT(1, f));
    EXPECT_EQ(EXTRACE_AV32(-1), 0xFFFFFFFFu);
    EXPECT_EQ(EXTRACE_AV64(-1), ~0ull);
    EXPECT_EQ(EXTRACE_AV32(1.5f), 0x3FC00000u);
    EXPECT_EQ(EXTRACE_AV64(1.5), 0x3FF8000000000000ull);
}
