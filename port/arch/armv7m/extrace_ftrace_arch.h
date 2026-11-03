/* SPDX-License-Identifier: MIT */
/*
 * extrace_ftrace_arch.h - function entry tracing on ARMv7-M, Thumb-2 (docs/ftrace.md).
 *
 * Build traced code with -fpatchable-function-entry=3 (GCC): three 16-bit NOPs at
 * the entry E. Enabled, they read
 *
 *     E+0  mov  ip, lr                  @ ip: free at a function entry (AAPCS)
 *     E+2  bl   <entry>                 @ 32-bit, +-16 MiB; lr = E+6 | 1
 *
 * so the trampoline gets ret = E + 7 and the caller's return address in ip. The
 * BL goes to the entry of the function's flavor (extrace_ftrace_entries).
 * Assembler-safe.
 */
#ifndef EXTRACE_FTRACE_ARCH_H
#define EXTRACE_FTRACE_ARCH_H

#define EXTRACE_FTRACE_ARG_REGS 4    /* r0-r3 */
#define EXTRACE_FTRACE_PATCH_HW 3    /* halfwords patched at E */
#define EXTRACE_FTRACE_RET_OFFSET 7  /* E = ret - 7 */
#define EXTRACE_FTRACE_HW_NOP 0xBF00 /* nop */
#define EXTRACE_FTRACE_HW_SKIP 0xE001 /* b.n E+6: patch in progress, or disabled */

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>

/*
 * The halfwords of the enabled patch at entry E for trampoline tramp (Thumb bit
 * ignored). false: tramp is out of BL range of E.
 */
static inline bool extrace_ftrace_insn(uint32_t e, uint32_t tramp,
                                       uint16_t hw[EXTRACE_FTRACE_PATCH_HW])
{
    int32_t off = (int32_t)((tramp & ~1u) - (e + 6u)); /* BL at E+2 reads PC = E+6 */
    uint32_t u = (uint32_t)off, s = (u >> 24) & 1u;
    uint32_t j1 = ((u >> 23) & 1u) ^ 1u ^ s, j2 = ((u >> 22) & 1u) ^ 1u ^ s;

    if (off < -(1 << 24) || off >= (1 << 24))
        return false;
    hw[0] = 0x46F4u; /* mov ip, lr */
    hw[1] = (uint16_t)(0xF000u | s << 10 | ((u >> 12) & 0x3FFu));
    hw[2] = (uint16_t)(0xD000u | j1 << 13 | j2 << 11 | ((u >> 1) & 0x7FFu));
    return true;
}

/* Make a patched halfword visible to instruction fetch (default code write). */
static inline void extrace_ftrace_sync(void)
{
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}
#endif

#endif /* EXTRACE_FTRACE_ARCH_H */
