// SPDX-License-Identifier: MIT
/* test_disabled.c - EXTRACE_CFG_ENABLE=0: every macro compiles and emits nothing (§4.4). */
#include "xt_util.h"

static struct xt_ring R;
static int evaluated;

static uint32_t arg(void)
{
    evaluated++;
    return 1;
}

TEST(disabled_image_emits_nothing)
{
    struct test_struct t = { 0 };
    struct extrace_resv rv;

    xt_setup(&R, XT_DATA, 0);
    EXTRACE_MARK(T_MARK);
    EXTRACE(T_NONE);
    EXTRACE_INTS(T_INTS, arg(), arg());
    EXTRACE_PRINTF(T_PRINTF, "x=%u", arg());
    EXTRACE_BUF(T_BUF, &t, sizeof(t));
    EXTRACE_BUF_LARGE(T_BUF, &t, sizeof(t));
    EXTRACE_STRUCT(T_STRUCT, &t);
    EXTRACE_STR(T_STR, "s");
    EXPECT_FALSE(EXTRACE_RESERVE(T_BUF, arg(), 1, &rv));
    EXPECT_FALSE(EXTRACE_ENABLED(T_INTS));
    EXPECT_EQ(EXTRACE_CT_T_INTS, 0);
    EXPECT_FALSE(extrace_emit_dyn_none(EXTRACE_EV_T_NONE));
    EXPECT_EQ(evaluated, 0);
    EXPECT_EQ(xt_drain_user(&R), 0);
}
