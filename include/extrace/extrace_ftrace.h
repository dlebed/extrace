/* SPDX-License-Identifier: MIT */
/*
 * extrace_ftrace.h - function entry tracing (docs/ftrace.md).
 *
 * Functions built with -fpatchable-function-entry get a few NOPs at their entry.
 * The platform patches them into a call of one entry of the port's trampoline,
 * so each function has its own flavor: MARK, NONE or ARGS(n), each optionally
 * with the caller. The trampoline has one body per record shape; an ARGS(n)
 * entry loads n and joins the ARGS body. A body tests the switch (the mask bit
 * of system event FTRACE), saves the argument registers and calls the shape's
 * emitter with the return address of the patch, which goes into the record as
 * is (minus EXTRACE_CFG_FTRACE_BASE); the host maps it to the function.
 *
 * The first part is assembler-safe: the trampolines include this header.
 */
#ifndef EXTRACE_FTRACE_H
#define EXTRACE_FTRACE_H

#define EXTRACE_FTRACE_SYS_ID 4 /* EXTRACE_SYS_FTRACE, in mask word 0 */

/*
 * Flavors: what a patched function records. MARK, NONE and ARGS(n) are the
 * shapes of the FTRACE record; with EXTRACE_FTRACE_CALLER or'ed in, the same
 * shapes of FTRACE_CALLER, which add the caller's return address.
 */
#define EXTRACE_FTRACE_OFF 0               /* the NOPs: not traced */
#define EXTRACE_FTRACE_MARK 1              /* address (MARK shape, no timestamp) */
#define EXTRACE_FTRACE_NONE 2              /* timestamp and address (NONE shape) */
#define EXTRACE_FTRACE_ARGS(n) (2 + (n))   /* and the first n argument registers (INTS) */
#define EXTRACE_FTRACE_CALLER 0x10         /* or'ed: FTRACE_CALLER records */

/* Architecture constants: argument registers, entry size, patch encodings. */
#if __has_include("extrace_ftrace_arch.h")
#include "extrace_ftrace_arch.h"
#endif
#ifndef EXTRACE_FTRACE_ARG_REGS
#define EXTRACE_FTRACE_ARG_REGS 8 /* no trampoline (host builds): a0..a7 */
#endif
/* ARGS(n) records n = 1..EXTRACE_FTRACE_ARG_REGS: 4 on ARMv7-M (r0-r3), 8 on RV32 */
#define EXTRACE_FTRACE_SHAPES (2 + EXTRACE_FTRACE_ARG_REGS) /* MARK, NONE, ARGS(1..) */
#define EXTRACE_FTRACE_ENTRIES (2 * EXTRACE_FTRACE_SHAPES)  /* without and with the caller */

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>
#include "extrace_types.h"

static_assert(EXTRACE_FTRACE_SYS_ID == EXTRACE_SYS_FTRACE);

/*
 * The index of flavor f in extrace_ftrace_entries: MARK, NONE, ARGS(1..ARG_REGS),
 * then the same with the caller. -1: f is not a flavor (OFF included).
 */
static inline int extrace_ftrace_entry_index(unsigned int f)
{
    unsigned int s = f & ~(unsigned int)EXTRACE_FTRACE_CALLER;

    if (s < EXTRACE_FTRACE_MARK || s > EXTRACE_FTRACE_ARGS(EXTRACE_FTRACE_ARG_REGS))
        return -1;
    return (int)(s - EXTRACE_FTRACE_MARK) + ((f & EXTRACE_FTRACE_CALLER) ? EXTRACE_FTRACE_SHAPES : 0);
}

/*
 * Patch the NOP pad at function entry pc into a call of the entry of flavor f, or
 * restore its NOPs (EXTRACE_FTRACE_OFF). The pad is checked first: it must hold
 * the NOPs, the patch of any flavor, or a patch left halfway, so a wrong address
 * is refused before anything is written; a patched function changes flavor in
 * place. Every intermediate state is a valid program, so the function may run
 * meanwhile, but calls for the same pc must not overlap. Returns 0,
 * -EXTRACE_EINVAL (no patch site at pc, or f is not a flavor of this build: the
 * CALLER flavors need EXTRACE_CFG_FTRACE_CALLER) or -EXTRACE_ERANGE (the
 * trampoline is out of branch range of pc).
 */
int extrace_ftrace_patch(uintptr_t pc, unsigned int f);

/*
 * Record one call of flavor f from C, for a caller without a trampoline: ret as a
 * trampoline delivers it, the caller's return address (CALLER flavors only) and
 * the arguments (ARGS(n) only, n words). Records nothing while the switch is off
 * or for a flavor that is not one of this build.
 */
void extrace_ftrace_hit(uintptr_t ret, uintptr_t caller, const uint32_t *args, unsigned int f);

/*
 * The trampoline's targets, one per record shape: the switch is tested, ret and
 * caller are as delivered, args the saved argument registers. Not for C callers,
 * which use extrace_ftrace_hit(). Without EXTRACE_CFG_FTRACE_CALLER the _c ones
 * record nothing (the patcher refuses their flavors).
 */
void extrace_ftrace_mark(uintptr_t ret);
void extrace_ftrace_none(uintptr_t ret);
void extrace_ftrace_args(uintptr_t ret, const uint32_t *args, unsigned int n);
void extrace_ftrace_mark_c(uintptr_t ret, uintptr_t caller);
void extrace_ftrace_none_c(uintptr_t ret, uintptr_t caller);
void extrace_ftrace_args_c(uintptr_t ret, uintptr_t caller, const uint32_t *args, unsigned int n);

/* The trampoline (port, assembly): the address of each entry, by extrace_ftrace_entry_index(). */
extern const uint32_t extrace_ftrace_entries[EXTRACE_FTRACE_ENTRIES];
#endif

#endif /* EXTRACE_FTRACE_H */
