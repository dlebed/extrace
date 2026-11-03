// SPDX-License-Identifier: MIT
/*
 * test_ftrace_insn_armv7m.c - ARMv7-M patch encoder against vectors taken from the LLVM
 * assembler at the same link addresses (docs/ftrace.md).
 */
#include "extrace_test.h"
#include "../../port/arch/armv7m/extrace_ftrace_arch.h"

static const struct {
    uint32_t e, tramp;
    uint16_t hw[EXTRACE_FTRACE_PATCH_HW];
} vec[] = {
    { 0x20001000u, 0x20000200u, { 0x46F4, 0xF7FF, 0xF8FD } },
    { 0x20000200u, 0x20001000u, { 0x46F4, 0xF000, 0xFEFD } },
    { 0x08000000u, 0x08FFF000u, { 0x46F4, 0xF3FE, 0xD7FD } },
    { 0x08FFF000u, 0x08000004u, { 0x46F4, 0xF400, 0xD7FF } },
    { 0x00001000u, 0x00123457u, { 0x46F4, 0xF122, 0xFA28 } }, /* Thumb bit ignored */
};

TEST(ftrace_insn_armv7m)
{
    uint16_t hw[EXTRACE_FTRACE_PATCH_HW];

    for (unsigned int i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        ASSERT_TRUE(extrace_ftrace_insn(vec[i].e, vec[i].tramp, hw));
        EXPECT_MEM(hw, vec[i].hw, sizeof(hw));
    }
    EXPECT_FALSE(extrace_ftrace_insn(0x00000000u, 0x01000006u, hw)); /* out of range */
}
