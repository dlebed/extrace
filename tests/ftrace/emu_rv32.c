// SPDX-License-Identifier: MIT
/* emu_rv32.c - RV32 trampoline and patcher under emulation. */
#include "emu_common.h"

__attribute__((patchable_function_entry(2), noinline)) uint32_t
traced(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
       uint32_t h)
{
    caller_seen = (uint32_t)(uintptr_t)__builtin_return_address(0);
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}

void emu_clobber(void)
{
    __asm__ volatile(".irp r,a0,a1,a2,a3,a4,a5,a6,a7,t0,t1,t2,t3,t4,t5,t6\n\t"
                     "li \\r, 0x5a5a\n\t.endr"
                     ::: "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "t0", "t1", "t2", "t3",
                       "t4", "t5", "t6");
}

/* through a volatile pointer: traced() is pure, clang would fold a direct call */
static uint32_t (*volatile traced_p)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                     uint32_t, uint32_t) = traced;

static uint32_t call(void)
{
    return traced_p(1, 2, 3, 4, 5, 6, 7, 8);
}

uint32_t start(void)
{
    return emu_checks((uintptr_t)&traced, call, 204, extrace_ftrace_entries[0] + (2u << 20));
}
