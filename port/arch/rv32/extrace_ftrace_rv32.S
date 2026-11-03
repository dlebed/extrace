/* SPDX-License-Identifier: MIT */
/*
 * extrace_ftrace_rv32.S - function entry trampoline for RV32 (docs/ftrace.md).
 *
 * Entered from a patched entry E by jal t0 into the entry of the function's
 * flavor: t0 = E+4, ra = the caller's return address, a0-a7 the arguments. t1
 * and t3-t6 are free at a function entry (t2 may be a static chain). One body
 * per record shape tests the switch, saves a0-a7, ra and t0, calls the shape's
 * emitter in extrace_prod.c directly and returns to E+4 with them restored. An
 * ARGS(n) entry loads n into t1 and joins the ARGS body; the last one falls into
 * it. Disabled cost: 5 instructions (7 for ARGS), no stack.
 */
#include "extrace_ftrace.h"

#if defined(__riscv_flen) || defined(__riscv_32e)
#error "extrace ftrace: the trampoline supports RV32I without F/D only"
#endif

    /* fn(ret = t0 [, caller = ra] [, args = sp, n = t1]) with the switch on */
    .macro FT_BODY fn, caller, args
    lui     t3, %hi(extrace_producer)
    lw      t3, %lo(extrace_producer)(t3)   # mask word 0
    andi    t3, t3, 1 << EXTRACE_FTRACE_SYS_ID
    beqz    t3, .Loff
    addi    sp, sp, -48                     # 16-byte aligned frame
    sw      a0, 0(sp)
    sw      a1, 4(sp)
    sw      a2, 8(sp)
    sw      a3, 12(sp)
    sw      a4, 16(sp)
    sw      a5, 20(sp)
    sw      a6, 24(sp)
    sw      a7, 28(sp)
    sw      ra, 32(sp)
    sw      t0, 36(sp)
    mv      a0, t0                          # ret = E+4
    .if \caller
    mv      a1, ra
    .if \args
    mv      a2, sp
    mv      a3, t1
    .endif
    .elseif \args
    mv      a1, sp
    mv      a2, t1
    .endif
    call    \fn
    lw      a0, 0(sp)
    lw      a1, 4(sp)
    lw      a2, 8(sp)
    lw      a3, 12(sp)
    lw      a4, 16(sp)
    lw      a5, 20(sp)
    lw      a6, 24(sp)
    lw      a7, 28(sp)
    lw      ra, 32(sp)
    lw      t0, 36(sp)
    addi    sp, sp, 48
    jr      t0
    .endm

    .section .text.extrace_ftrace_entry, "ax", @progbits
    .p2align 2
.Lmark:
    FT_BODY extrace_ftrace_mark, 0, 0
.Lnone:
    FT_BODY extrace_ftrace_none, 0, 0
    .irp n, 1, 2, 3, 4, 5, 6, 7
.Largs\n:
    c.li    t1, \n
    c.j     .Largs
    .endr
.Largs8:
    c.li    t1, 8
.Largs:
    FT_BODY extrace_ftrace_args, 0, 1
.Lmark_c:
    FT_BODY extrace_ftrace_mark_c, 1, 0
.Lnone_c:
    FT_BODY extrace_ftrace_none_c, 1, 0
    .irp n, 1, 2, 3, 4, 5, 6, 7
.Largs\n\()_c:
    c.li    t1, \n
    c.j     .Largs_c
    .endr
.Largs8_c:
    c.li    t1, 8
.Largs_c:
    FT_BODY extrace_ftrace_args_c, 1, 1
.Loff:
    jr      t0                              # switch off

    /* by extrace_ftrace_entry_index(): MARK, NONE, ARGS 1-8, then with the caller */
    .section .rodata.extrace_ftrace_entries, "a", @progbits
    .p2align 2
    .globl  extrace_ftrace_entries
    .type   extrace_ftrace_entries, @object
extrace_ftrace_entries:
    .word   .Lmark, .Lnone
    .irp n, 1, 2, 3, 4, 5, 6, 7, 8
    .word   .Largs\n
    .endr
    .word   .Lmark_c, .Lnone_c
    .irp n, 1, 2, 3, 4, 5, 6, 7, 8
    .word   .Largs\n\()_c
    .endr
    .if EXTRACE_FTRACE_ARG_REGS != 8 || . - extrace_ftrace_entries != 4 * EXTRACE_FTRACE_ENTRIES
    .error "extrace ftrace: the entry table does not match extrace_ftrace.h"
    .endif
    .size   extrace_ftrace_entries, . - extrace_ftrace_entries
