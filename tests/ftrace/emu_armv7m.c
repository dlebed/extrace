// SPDX-License-Identifier: MIT
/* emu_armv7m.c - ARMv7-M trampoline and patcher under emulation (hard-float ABI). */
#include "emu_common.h"

/* Patchable entry as GCC emits it with -fpatchable-function-entry=3, table included. */
float traced(uint32_t a, uint32_t b, uint32_t c, uint32_t d, float f);
float traced_body(uint32_t a, uint32_t b, uint32_t c, uint32_t d, float f);
__asm__(".syntax unified\n.thumb\n.global traced\n.type traced, %function\n"
        ".Lpfe_traced:\n.thumb_func\ntraced: nop\n nop\n nop\n b.w traced_body\n"
        ".pushsection __patchable_function_entries, \"aw\"\n.p2align 2\n"
        ".word .Lpfe_traced\n.popsection\n");

float traced_body(uint32_t a, uint32_t b, uint32_t c, uint32_t d, float f)
{
    caller_seen = (uint32_t)(uintptr_t)__builtin_return_address(0); /* lr: entered by b.w */
    return (float)(a + 2 * b + 3 * c + 4 * d) * f;
}

void emu_clobber(void)
{
    __asm__ volatile("mov r0, #0x11\n\tmov r1, #0x22\n\tmov r2, #0x33\n\tmov r3, #0x44\n\t"
                     "mov r12, #0x55\n\tvmov s0, r0\n\t"
                     ".irp n,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n\tvmov s\\n, r1\n\t.endr"
                     ::: "r0", "r1", "r2", "r3", "r12", "d0", "d1", "d2", "d3", "d4", "d5",
                       "d6", "d7");
}

/* 30 * 1.5: the float argument in s0 must survive the trampoline */
static uint32_t call(void)
{
    return (uint32_t)traced(1, 2, 3, 4, 1.5f);
}

uint32_t start(void)
{
    return emu_checks((uintptr_t)&traced, call, 45, extrace_ftrace_entries[0] + (32u << 20));
}
