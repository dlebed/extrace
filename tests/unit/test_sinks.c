// SPDX-License-Identifier: MIT
/* test_sinks.c - the stats and text reference sinks (DESIGN §12.8). */
#include <stdio.h>
#include "xt_util.h"
#include "extrace_ftrace.h"
#include "extrace_disp.h"
#include "extrace_sink_stats.h"
#include "extrace_sink_text.h"

static struct xt_ring R;
static struct extrace_disp D;

static void setup(void)
{
    xt_setup(&R, XT_DATA, 0);
    extrace_disp_init(&D);
    EXPECT_EQ(extrace_disp_add_ring(&D, &R.c), 0);
}

TEST(stats_sink_counts)
{
    static struct extrace_stats_sink st;
    static struct extrace_stats_sink_ring rings[1];
    static uint32_t counters[1 * 64];
    int i;

    setup();
    extrace_stats_sink_init(&st, rings, counters, 1, 64);
    EXPECT_EQ(extrace_disp_add_sink(&D, &st.sink), 0);
    for (i = 0; i < 5; i++)
        EXTRACE_INTS(T_SEQ, (uint32_t)i, 0u);
    EXTRACE_MARK(T_MARK);
    extrace_prod_flush();
    extrace_disp_poll(&D, 1u << 20);
    EXPECT_EQ(extrace_stats_sink_get(&st, 7, EXTRACE_EV_T_SEQ), 5);
    EXPECT_EQ(extrace_stats_sink_get(&st, 7, EXTRACE_EV_T_MARK), 1);
    EXPECT_EQ(extrace_stats_sink_get(&st, 7, EXTRACE_SYS_INFO), 1);
    EXPECT_EQ(extrace_stats_sink_get(&st, 9, EXTRACE_EV_T_SEQ), 0);
    /* overflow: the LOST counts add up to the drops */
    for (i = 0; i < 2000; i++)
        EXTRACE_INTS(T_SEQ, (uint32_t)i, 0u);
    extrace_prod_flush();
    while (extrace_disp_poll(&D, 1u << 20) > 0) /* half a ring per poll and ring */
        ;
    EXTRACE_MARK(T_MARK); /* carries the LOST report */
    extrace_prod_flush();
    while (extrace_disp_poll(&D, 1u << 20) > 0)
        ;
    EXPECT_EQ(extrace_stats_sink_lost(&st, 7), extrace_producer.lost_total);
    EXPECT_TRUE(extrace_producer.lost_total > 0);
    extrace_stats_sink_clear(&st);
    EXPECT_EQ(extrace_stats_sink_get(&st, 7, EXTRACE_EV_T_SEQ), 0);
}

static char out[8192];
static uint32_t out_n, out_cap = sizeof(out);

static uint32_t write_hook(void *ctx, const char *b, uint32_t n)
{
    (void)ctx;
    if (n > out_cap - out_n)
        n = out_cap - out_n;
    memcpy(out + out_n, b, n);
    out_n += n;
    return n;
}

TEST(text_sink_lines)
{
    static struct extrace_text_sink t;
    static char fifo[1024];
    char want[64];

    setup();
    out_n = 0;
    out_cap = sizeof(out);
    EXPECT_EQ(extrace_text_sink_init(&t, fifo, 1000, write_hook, nullptr), -EXTRACE_EINVAL);
    EXPECT_EQ(extrace_text_sink_init(&t, fifo, sizeof(fifo), write_hook, nullptr), 0);
    EXPECT_EQ(extrace_disp_add_sink(&D, &t.sink), 0);
    extrace_disp_poll(&D, 1u << 20); /* INFO, SYNC */
    out_n = 0;
    EXTRACE_INTS(T_INTS, 7u, -3);
    EXTRACE_STR(T_STR, "hi\x01");
    EXTRACE_BUF(T_BUF, "\xAB\xCD", 2);
    EXPECT_EQ(extrace_cons_ev_set(&R.c, EXTRACE_SYS_FTRACE, EXTRACE_EVS_ON), 0);
    extrace_prod_poll();
    extrace_ftrace_hit(0x20001007u, 0, (const uint32_t[]){ 7, 300 }, EXTRACE_FTRACE_ARGS(2));
    extrace_ftrace_hit(0x80u, 0, nullptr, EXTRACE_FTRACE_MARK);
#if EXTRACE_CFG_FTRACE_CALLER
    extrace_ftrace_hit(0x81u, 0x20000123u, nullptr, EXTRACE_FTRACE_MARK | EXTRACE_FTRACE_CALLER);
#endif
    extrace_prod_flush();
    extrace_disp_poll(&D, 1u << 20);
    out[out_n] = 0;
    EXPECT_TRUE(strstr(out, " 4 INTS fn 0x20001007 7 300\n") != nullptr);
    EXPECT_TRUE(strstr(out, " 4 MARK fn 0x00000080\n") != nullptr);
#if EXTRACE_CFG_FTRACE_CALLER
    EXPECT_TRUE(strstr(out, " 8 MARK fn 0x00000081 from 0x20000123\n") != nullptr);
#endif
    snprintf(want, sizeof(want), " %u INTS 7 -3\n", EXTRACE_EV_T_INTS);
    EXPECT_TRUE(strstr(out, want) != nullptr);
    snprintf(want, sizeof(want), " %u STR \"hi.\"\n", EXTRACE_EV_T_STR);
    EXPECT_TRUE(strstr(out, want) != nullptr);
    snprintf(want, sizeof(want), " %u BUF abcd\n", EXTRACE_EV_T_BUF);
    EXPECT_TRUE(strstr(out, want) != nullptr);
    EXPECT_TRUE(strncmp(out, "[7] t=", 6) == 0);
    /* a stalled UART: the FIFO fills, lines are dropped, nothing blocks */
    out_cap = out_n;
    for (int i = 0; i < 200; i++)
        EXTRACE_INTS(T_SEQ, (uint32_t)i, 0u);
    extrace_prod_flush();
    while (extrace_disp_poll(&D, 1u << 20) > 0)
        ;
    EXPECT_TRUE(t.dropped > 0);
    EXPECT_EQ(R.c.tail, extrace_blk_pstate(R.blk)->head);
    out_cap = sizeof(out);
    extrace_disp_poll(&D, 1u << 20); /* flush drains the FIFO */
    EXPECT_EQ(t.head, t.tail);
}
