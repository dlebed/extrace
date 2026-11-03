// SPDX-License-Identifier: MIT
/*
 * test_ftrace_insn_rv32.c - RV32 patch encoder against vectors taken from the LLVM
 * assembler at the same link addresses (docs/ftrace.md).
 */
#include "extrace_test.h"
#include "../../port/arch/rv32/extrace_ftrace_arch.h"

static const struct {
    uint32_t e, tramp;
    uint16_t hw[EXTRACE_FTRACE_PATCH_HW];
} vec[] = {
    { 0x80001000u, 0x80000200u, { 0xF2EF, 0xA00F } },
    { 0x80000200u, 0x80001000u, { 0x02EF, 0x6010 } },
    { 0x80000000u, 0x800FF000u, { 0xF2EF, 0x000F } },
    { 0x800FF000u, 0x80000004u, { 0x12EF, 0x8040 } },
    { 0x00001000u, 0x00055554u, { 0x42EF, 0x5545 } },
};

TEST(ftrace_insn_rv32)
{
    uint16_t hw[EXTRACE_FTRACE_PATCH_HW];

    for (unsigned int i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        ASSERT_TRUE(extrace_ftrace_insn(vec[i].e, vec[i].tramp, hw));
        EXPECT_MEM(hw, vec[i].hw, sizeof(hw));
    }
    EXPECT_FALSE(extrace_ftrace_insn(0x00000000u, 0x00100000u, hw)); /* out of range */
}
