/* SPDX-License-Identifier: MIT */
/*
 * extrace_ftrace_armv7m.S - function entry trampoline for ARMv7-M (docs/ftrace.md).
 *
 * Entered from a patched entry E into the entry of the function's flavor, with
 * lr = E+7 and ip = the caller's return address; r0-r3 hold the arguments. One
 * body per record shape saves r0-r3, ip and lr (24 bytes: sp stays 8-byte
 * aligned), tests the switch, calls the shape's emitter in extrace_prod.c
 * directly and returns to E+6 with every register the function may read at its
 * entry restored (r0-r3, sp, lr; s0-s15 with the hard-float ABI). An ARGS(n)
 * entry saves, loads n and joins the ARGS body; the last one falls into it.
 * Disabled cost: 7 instructions (9 for ARGS), no call.
 */
#include "extrace_ftrace.h"

    .syntax unified
    .thumb

    @ fn(ret = lr [, caller = ip] [, args = the saved r0-r3 in r<args>, n set by
    @ the entry]) with the switch on; save: the body saves r0-r3, ip and lr itself
    .macro FT_BODY fn, caller, args, save
    .if \save
    push.w  {r0-r3, ip, lr}
    .endif
    ldr     r0, =extrace_producer           @ one instruction: from the pool below
    ldr     r0, [r0]                        @ mask word 0
    lsls    r0, r0, #(31 - EXTRACE_FTRACE_SYS_ID)
    bpl     .Loff
#ifdef __ARM_PCS_VFP
    vpush   {d0-d7}                         @ argument registers s0-s15
    .if \args
    add     r\args, sp, #64
    .endif
#else
    .if \args
    mov     r\args, sp
    .endif
#endif
    .if \caller
    mov     r1, ip
    .endif
    mov     r0, lr                          @ ret = E+7
    bl      \fn
#ifdef __ARM_PCS_VFP
    vpop    {d0-d7}
#endif
    pop     {r0-r3, lr}                     @ lr = saved ip (LR and PC in one POP
    pop     {pc}                            @ are UNPREDICTABLE); pc = E+7
    .endm

    .section .text.extrace_ftrace_entry, "ax", %progbits
    .p2align 2
    .thumb_func
.Lmark:
    FT_BODY extrace_ftrace_mark, 0, 0, 1
    .thumb_func
.Lnone:
    FT_BODY extrace_ftrace_none, 0, 0, 1
    .irp n, 1, 2, 3
    .thumb_func
.Largs\n:
    push.w  {r0-r3, ip, lr}
    movs    r2, #\n
    b       .Largs
    .endr
    .thumb_func
.Largs4:
    push.w  {r0-r3, ip, lr}
    movs    r2, #4
.Largs:
    FT_BODY extrace_ftrace_args, 0, 1, 0
    .thumb_func
.Lmark_c:
    FT_BODY extrace_ftrace_mark_c, 1, 0, 1
    .thumb_func
.Lnone_c:
    FT_BODY extrace_ftrace_none_c, 1, 0, 1
    .irp n, 1, 2, 3
    .thumb_func
.Largs\n\()_c:
    push.w  {r0-r3, ip, lr}
    movs    r3, #\n
    b       .Largs_c
    .endr
    .thumb_func
.Largs4_c:
    push.w  {r0-r3, ip, lr}
    movs    r3, #4
.Largs_c:
    FT_BODY extrace_ftrace_args_c, 1, 2, 0
.Loff:
    pop     {r0-r3, lr}                     @ switch off
    pop     {pc}
    .ltorg

    @ by extrace_ftrace_entry_index(): MARK, NONE, ARGS 1-4, then with the caller
    .section .rodata.extrace_ftrace_entries, "a", %progbits
    .p2align 2
    .global extrace_ftrace_entries
    .type   extrace_ftrace_entries, %object
extrace_ftrace_entries:
    .word   .Lmark, .Lnone
    .irp n, 1, 2, 3, 4
    .word   .Largs\n
    .endr
    .word   .Lmark_c, .Lnone_c
    .irp n, 1, 2, 3, 4
    .word   .Largs\n\()_c
    .endr
    .if EXTRACE_FTRACE_ARG_REGS != 4 || . - extrace_ftrace_entries != 4 * EXTRACE_FTRACE_ENTRIES
    .error "extrace ftrace: the entry table does not match extrace_ftrace.h"
    .endif
    .size   extrace_ftrace_entries, . - extrace_ftrace_entries
