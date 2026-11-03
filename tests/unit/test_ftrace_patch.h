/* SPDX-License-Identifier: MIT */
/*
 * test_ftrace_patch.h - the generic patcher (src/extrace_ftrace.c) on a pad in
 * memory, with the encoder of one architecture: flavors, changing flavor in
 * place, halfway states, refusals. Included by test_ftrace_patch_<arch>.c after
 * the arch header, with XT_ARCH set. The entry table holds fake addresses near
 * the pad, the code store logs.
 */
#include <string.h>
#include "extrace_ftrace.h"

#define XT_CAT2(a, b) a##b
#define XT_CAT(a, b) XT_CAT2(a, b)
#define XT_NAME(n) XT_CAT(n, XT_CAT(_, XT_ARCH))
#define XT_TEST(n) TEST(n) /* expands the name first */

#define FT_HW EXTRACE_FTRACE_PATCH_HW

static uint16_t XT_NAME(pad)[16] __attribute__((aligned(4)));
static uint16_t XT_NAME(wlog)[64];
static unsigned int XT_NAME(nw);
static uint32_t XT_NAME(entries)[EXTRACE_FTRACE_ENTRIES];

#define EXTRACE_PORT_HAS_CODE_WRITE 1
static inline void extrace_port_code_write16(uint16_t *p, uint16_t v)
{
    *p = v;
    if (XT_NAME(nw) < 64)
        XT_NAME(wlog)[XT_NAME(nw)] = v;
    XT_NAME(nw)++;
}

/* the patcher reads the fake table */
#define extrace_ftrace_entries XT_NAME(entries)
#define extrace_ftrace_patch XT_NAME(xt_patch)
#include "../../src/extrace_ftrace.c"
#undef extrace_ftrace_entries
#undef extrace_ftrace_patch

#define PAD XT_NAME(pad)
#define NW XT_NAME(nw)
#define WLOG XT_NAME(wlog)
#define PATCH XT_NAME(xt_patch)

/*
 * The trampoline at pad + off: entries of uneven sizes, as the real ones (an ARGS
 * entry is a few bytes, a body tens), with the Thumb bit on ARM.
 */
#define XT_ENTRY(t, i) ((t) + 6u * (uint32_t)(i) + ((uint32_t)(i) % 3u) * 40u)

static uint32_t XT_NAME(set_tramp)(uint32_t off)
{
    uint32_t t = (uint32_t)(uintptr_t)PAD + off + (EXTRACE_FTRACE_RET_OFFSET & 1u);

    for (int i = 0; i < EXTRACE_FTRACE_ENTRIES; i++)
        XT_NAME(entries)[i] = XT_ENTRY(t, i);
    return t;
}

/* The pad holds the patch for entry i of the table at tramp. */
static bool XT_NAME(pad_is)(uint32_t tramp, int i)
{
    uint16_t hw[FT_HW];

    for (unsigned int k = 0; k < FT_HW; k++)
        hw[k] = EXTRACE_FTRACE_HW_NOP;
    if (i >= 0 && !extrace_ftrace_insn((uint32_t)(uintptr_t)PAD,
                                       XT_ENTRY(tramp, i), hw))
        return false;
    for (unsigned int k = 0; k < FT_HW; k++)
        if (PAD[k] != hw[k])
            return false;
    return true;
}

static void XT_NAME(nops)(void)
{
    for (unsigned int k = 0; k < FT_HW; k++)
        PAD[k] = EXTRACE_FTRACE_HW_NOP;
    NW = 0;
}

/* Every flavor has an entry of its own, and nothing else has one. */
XT_TEST(XT_NAME(ftrace_entry_index))
{
    unsigned int seen = 0, n = 0;

    for (unsigned int f = 0; f < 0x40; f++) {
        int i = extrace_ftrace_entry_index(f);
        unsigned int s = f & ~(unsigned int)EXTRACE_FTRACE_CALLER;
        bool flavor = f < 0x20 && s >= EXTRACE_FTRACE_MARK &&
                      s <= EXTRACE_FTRACE_ARGS(EXTRACE_FTRACE_ARG_REGS);

        EXPECT_EQ(i >= 0, flavor);
        if (i < 0)
            continue;
        ASSERT_TRUE(i < EXTRACE_FTRACE_ENTRIES);
        EXPECT_FALSE(seen >> i & 1u);
        seen |= 1u << i;
        n++;
    }
    EXPECT_EQ(n, EXTRACE_FTRACE_ENTRIES);
    EXPECT_EQ(extrace_ftrace_entry_index(EXTRACE_FTRACE_MARK), 0);
    EXPECT_EQ(extrace_ftrace_entry_index(EXTRACE_FTRACE_ARGS(1)), 2);
    EXPECT_EQ(extrace_ftrace_entry_index(EXTRACE_FTRACE_MARK | EXTRACE_FTRACE_CALLER),
              EXTRACE_FTRACE_SHAPES);
}

XT_TEST(XT_NAME(ftrace_patch_flavors))
{
    uint32_t t = XT_NAME(set_tramp)(0x1000);
    const unsigned int caller = EXTRACE_CFG_FTRACE_CALLER ? EXTRACE_FTRACE_CALLER : 0u;
    const unsigned int fl[] = { EXTRACE_FTRACE_NONE, EXTRACE_FTRACE_MARK, EXTRACE_FTRACE_ARGS(1),
                                EXTRACE_FTRACE_ARGS(EXTRACE_FTRACE_ARG_REGS),
                                EXTRACE_FTRACE_NONE | caller,
                                EXTRACE_FTRACE_ARGS(2) | caller };

    XT_NAME(nops)();
    EXPECT_EQ(PATCH((uintptr_t)PAD, EXTRACE_FTRACE_OFF), 0); /* already NOPs: no write */
    EXPECT_EQ(NW, 0);
    for (unsigned int k = 0; k < sizeof(fl) / sizeof(fl[0]); k++) {
        int i = extrace_ftrace_entry_index(fl[k]);

        NW = 0;
        ASSERT_EQ(PATCH((uintptr_t)PAD | 1u, fl[k]), 0); /* Thumb bit ignored */
        EXPECT_TRUE(XT_NAME(pad_is)(t, i));
        /* the skip branch first, its final value last */
        ASSERT_EQ(NW, FT_HW + 1);
        EXPECT_EQ(WLOG[0], EXTRACE_FTRACE_HW_SKIP);
        EXPECT_EQ(WLOG[FT_HW], PAD[0]);
        NW = 0;
        EXPECT_EQ(PATCH((uintptr_t)PAD, fl[k]), 0); /* the same again: no write */
        EXPECT_EQ(NW, 0);
    }
    EXPECT_EQ(PATCH((uintptr_t)PAD, EXTRACE_FTRACE_OFF), 0);
    EXPECT_TRUE(XT_NAME(pad_is)(t, -1));
}

XT_TEST(XT_NAME(ftrace_patch_refusals))
{
    const unsigned int bad[] = { 0x20, EXTRACE_FTRACE_CALLER,
                                 EXTRACE_FTRACE_ARGS(EXTRACE_FTRACE_ARG_REGS + 1),
                                 EXTRACE_FTRACE_ARGS(EXTRACE_FTRACE_ARG_REGS + 1) |
                                     EXTRACE_FTRACE_CALLER,
                                 EXTRACE_FTRACE_NONE | 0x20 };
    uint32_t t = XT_NAME(set_tramp)(0x1000);

    XT_NAME(nops)();
    for (unsigned int k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
        EXPECT_EQ(PATCH((uintptr_t)PAD, bad[k]), -EXTRACE_EINVAL);
    EXPECT_EQ(PATCH((uintptr_t)PAD, EXTRACE_FTRACE_MARK | EXTRACE_FTRACE_CALLER),
              EXTRACE_CFG_FTRACE_CALLER ? 0 : -EXTRACE_EINVAL);
    EXPECT_TRUE(XT_NAME(pad_is)(t, EXTRACE_CFG_FTRACE_CALLER ? EXTRACE_FTRACE_SHAPES : -1));
    XT_NAME(nops)();
    /* no patch site: refused before any write, whatever the flavor */
    PAD[0] = 0x1234;
    EXPECT_EQ(PATCH((uintptr_t)PAD, EXTRACE_FTRACE_NONE), -EXTRACE_EINVAL);
    EXPECT_EQ(PATCH((uintptr_t)PAD, EXTRACE_FTRACE_OFF), -EXTRACE_EINVAL);
    PAD[0] = EXTRACE_FTRACE_HW_SKIP; /* skip branch, then a stranger */
    PAD[FT_HW - 1] = 0x1234;
    EXPECT_EQ(PATCH((uintptr_t)PAD, EXTRACE_FTRACE_NONE), -EXTRACE_EINVAL);
    EXPECT_EQ(NW, 0);
    /* out of branch range: refused, the NOPs stay */
    XT_NAME(nops)();
    XT_NAME(set_tramp)(EXTRACE_FTRACE_PATCH_HW == 3 ? 32u << 20 : 2u << 20);
    EXPECT_EQ(PATCH((uintptr_t)PAD, EXTRACE_FTRACE_NONE), -EXTRACE_ERANGE);
    EXPECT_EQ(PATCH((uintptr_t)PAD, EXTRACE_FTRACE_OFF), 0);
    EXPECT_EQ(NW, 0);
}

/* A change of flavor left halfway: halfwords of the old and the new patch mixed. */
XT_TEST(XT_NAME(ftrace_patch_completes_a_halfway_change))
{
    uint32_t t = XT_NAME(set_tramp)(0x1000);
    int a = extrace_ftrace_entry_index(EXTRACE_FTRACE_MARK);
    int b = extrace_ftrace_entry_index(EXTRACE_FTRACE_ARGS(EXTRACE_FTRACE_ARG_REGS));
    uint16_t ha[FT_HW], hb[FT_HW];

    ASSERT_TRUE(extrace_ftrace_insn((uint32_t)(uintptr_t)PAD,
                                    XT_ENTRY(t, a), ha));
    ASSERT_TRUE(extrace_ftrace_insn((uint32_t)(uintptr_t)PAD,
                                    XT_ENTRY(t, b), hb));
    for (unsigned int cut = 1; cut < FT_HW; cut++) {
        PAD[0] = EXTRACE_FTRACE_HW_SKIP;
        for (unsigned int k = 1; k < FT_HW; k++)
            PAD[k] = k <= cut ? hb[k] : ha[k];
        NW = 0;
        EXPECT_EQ(PATCH((uintptr_t)PAD, EXTRACE_FTRACE_ARGS(EXTRACE_FTRACE_ARG_REGS)), 0);
        EXPECT_TRUE(XT_NAME(pad_is)(t, b));
        PAD[0] = EXTRACE_FTRACE_HW_SKIP; /* halfway back to the NOPs */
        for (unsigned int k = 1; k < FT_HW; k++)
            PAD[k] = k <= cut ? EXTRACE_FTRACE_HW_NOP : hb[k];
        EXPECT_EQ(PATCH((uintptr_t)PAD, EXTRACE_FTRACE_MARK), 0);
        EXPECT_TRUE(XT_NAME(pad_is)(t, a));
    }
    /* every entry has its own encoding */
    for (int i = 0; i < EXTRACE_FTRACE_ENTRIES; i++) {
        XT_NAME(nops)();
        ASSERT_TRUE(extrace_ftrace_insn((uint32_t)(uintptr_t)PAD,
                                        XT_ENTRY(t, i), ha));
        for (int j = 0; j < i; j++) {
            ASSERT_TRUE(extrace_ftrace_insn((uint32_t)(uintptr_t)PAD,
                                            XT_ENTRY(t, j), hb));
            EXPECT_TRUE(memcmp(ha, hb, sizeof(ha)) != 0);
        }
    }
}
