// SPDX-License-Identifier: MIT
#include "extrace_test.h"
#include "extrace.h"
#include "extrace_cons.h"
#include "extrace_rec.h"

static uint64_t blk_mem[(EXTRACE_RING_BLOCK_SIZE(4096, EXTRACE_MASK_WORDS) + 7) / 8];

TEST(smoke)
{
    struct extrace_prod_cfg cfg = { .block = blk_mem, .block_size = sizeof(blk_mem), .name = "t0" };
    static struct extrace_cons c;
    static uint32_t mirror[EXTRACE_CONS_MIRROR_WORDS(1024)];
    uint8_t buf[4096];
    int n;

    ASSERT_EQ(extrace_prod_init(&cfg), 0);
    EXTRACE_INTS(T_INTS, 5u, -3);
    extrace_prod_flush();
    ASSERT_EQ(extrace_cons_attach(&c, blk_mem, sizeof(blk_mem), mirror, 64, 0), 0);
    n = extrace_cons_read(&c, buf, sizeof(buf));
    EXPECT_TRUE(n > 0);
}
