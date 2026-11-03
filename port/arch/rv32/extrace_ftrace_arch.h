/* SPDX-License-Identifier: MIT */
/*
 * extrace_ftrace_arch.h - function entry tracing on RV32 with C/Zca (docs/ftrace.md).
 *
 * Build traced code with -fpatchable-function-entry=2: two c.nop at the entry E.
 * Enabled, they read
 *
 *     E+0  jal  t0, <entry>                # 32-bit, +-1 MiB; t0 = E+4
 *
 * t0 is the psABI alternate link register and free at a function entry, so ra
 * keeps the caller's return address and nothing else needs saving. The JAL goes
 * to the entry of the function's flavor (extrace_ftrace_entries).
 * Assembler-safe.
 */
#ifndef EXTRACE_FTRACE_ARCH_H
#define EXTRACE_FTRACE_ARCH_H

#define EXTRACE_FTRACE_ARG_REGS 8    /* a0-a7 */
#define EXTRACE_FTRACE_PATCH_HW 2    /* halfwords patched at E */
#define EXTRACE_FTRACE_RET_OFFSET 4  /* E = ret - 4 */
#define EXTRACE_FTRACE_HW_NOP 0x0001 /* c.nop */
#define EXTRACE_FTRACE_HW_SKIP 0xA011 /* c.j E+4: patch in progress, or disabled */

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>

/*
 * The halfwords of the enabled patch at entry E for trampoline tramp, in address
 * order. false: tramp is out of JAL range of E.
 */
static inline bool extrace_ftrace_insn(uint32_t e, uint32_t tramp,
                                       uint16_t hw[EXTRACE_FTRACE_PATCH_HW])
{
    int32_t off = (int32_t)(tramp - e);
    uint32_t u = (uint32_t)off, insn;

    if (off < -(1 << 20) || off >= (1 << 20))
        return false;
    insn = (u & 0x100000u) << 11 | (u & 0x7FEu) << 20 | (u & 0x800u) << 9 | (u & 0xFF000u) |
           5u << 7 | 0x6Fu; /* jal t0, off */
    hw[0] = (uint16_t)insn;
    hw[1] = (uint16_t)(insn >> 16);
    return true;
}

/*
 * Make a patched halfword visible to instruction fetch (default code write).
 * Without Zifencei only a data fence: enough for a core without an instruction
 * cache or prefetch buffer; other cores need EXTRACE_PORT_HAS_CODE_WRITE.
 */
static inline void extrace_ftrace_sync(void)
{
#ifdef __riscv_zifencei
    __asm__ volatile("fence.i" ::: "memory");
#else
    __asm__ volatile("fence" ::: "memory");
#endif
}
#endif

#endif /* EXTRACE_FTRACE_ARCH_H */
