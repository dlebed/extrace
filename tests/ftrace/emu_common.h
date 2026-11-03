/* SPDX-License-Identifier: MIT */
/*
 * emu_common.h - bare-metal image that tests a real ftrace trampoline and the
 * generic patcher under an emulator (run_emu.py). The trampoline's targets are
 * mocks that log their arguments and the flavor they stand for, and clobber
 * every caller-saved register. start() returns a bitmask of failed checks;
 * entry_fail[k] holds the failed checks of table entry k. Built with and without
 * EXTRACE_CFG_FTRACE_CALLER: without it, the CALLER flavors must be refused.
 */
#include <stdint.h>
#include "extrace_ftrace.h"

#ifndef EXTRACE_CFG_FTRACE_CALLER
#define EXTRACE_CFG_FTRACE_CALLER 0
#endif

struct {
    uint32_t mask0; /* the trampolines test bit FTRACE of the first word */
} extrace_producer;

volatile uint32_t hits, log_ret, log_f, log_args[EXTRACE_FTRACE_ARG_REGS], log_caller;
volatile uint32_t caller_seen; /* the traced function's own return address */
volatile uint32_t entry_fail[EXTRACE_FTRACE_ENTRIES];

void emu_clobber(void); /* arch: trash caller-saved registers */
uint32_t start(void);
uint32_t emu_checks(uintptr_t pc, uint32_t (*call)(void), uint32_t want, uintptr_t far);

/* The trampoline's targets: log the flavor they stand for, then clobber. */
static void hit(uintptr_t ret, unsigned int f, uintptr_t caller, const uint32_t *args)
{
    hits++;
    log_ret = (uint32_t)ret;
    log_f = f;
    log_caller = (uint32_t)caller;
    for (int i = 0; i < EXTRACE_FTRACE_ARG_REGS; i++)
        log_args[i] = args ? args[i] : 0;
    emu_clobber();
}

void extrace_ftrace_mark(uintptr_t ret)
{
    hit(ret, EXTRACE_FTRACE_MARK, 0, 0);
}

void extrace_ftrace_none(uintptr_t ret)
{
    hit(ret, EXTRACE_FTRACE_NONE, 0, 0);
}

void extrace_ftrace_args(uintptr_t ret, const uint32_t *args, unsigned int n)
{
    hit(ret, EXTRACE_FTRACE_ARGS(n), 0, args);
}

void extrace_ftrace_mark_c(uintptr_t ret, uintptr_t caller)
{
    hit(ret, EXTRACE_FTRACE_MARK | EXTRACE_FTRACE_CALLER, caller, 0);
}

void extrace_ftrace_none_c(uintptr_t ret, uintptr_t caller)
{
    hit(ret, EXTRACE_FTRACE_NONE | EXTRACE_FTRACE_CALLER, caller, 0);
}

void extrace_ftrace_args_c(uintptr_t ret, uintptr_t caller, const uint32_t *args, unsigned int n)
{
    hit(ret, EXTRACE_FTRACE_ARGS(n) | EXTRACE_FTRACE_CALLER, caller, args);
}

/* The flavor of entry k of the table. */
static unsigned int flavor_of(int k)
{
    return k < EXTRACE_FTRACE_SHAPES
               ? EXTRACE_FTRACE_MARK + (unsigned int)k
               : (EXTRACE_FTRACE_MARK + (unsigned int)(k - EXTRACE_FTRACE_SHAPES)) |
                     EXTRACE_FTRACE_CALLER;
}

/* The pad at pc holds the patch of flavor f, or the NOPs. */
static int pad_is(uintptr_t pc, unsigned int f)
{
    const volatile uint16_t *e = (const volatile uint16_t *)(pc & ~(uintptr_t)1);
    uint16_t hw[EXTRACE_FTRACE_PATCH_HW];
    int k = extrace_ftrace_entry_index(f);

    for (unsigned int i = 0; i < EXTRACE_FTRACE_PATCH_HW; i++)
        hw[i] = EXTRACE_FTRACE_HW_NOP;
    if (k >= 0)
        extrace_ftrace_insn((uint32_t)(uintptr_t)e, extrace_ftrace_entries[k], hw);
    for (unsigned int i = 0; i < EXTRACE_FTRACE_PATCH_HW; i++)
        if (e[i] != hw[i])
            return 0;
    return 1;
}

/*
 * The common sequence; call() runs the traced function with arguments 1, 2, 3 ...
 * and returns its result, which must be want. Every entry of the table is
 * patched in turn, in place, and must deliver its flavor.
 */
uint32_t emu_checks(uintptr_t pc, uint32_t (*call)(void), uint32_t want, uintptr_t far)
{
    volatile uint16_t *e = (volatile uint16_t *)(pc & ~(uintptr_t)1);
    uint32_t fail = 0, n = 0;

    fail |= (call() != want || hits) << 0;                             /* NOPs */
    fail |= (extrace_ftrace_patch((uintptr_t)&emu_checks, EXTRACE_FTRACE_NONE) !=
             -EXTRACE_EINVAL) << 1;
    fail |= (extrace_ftrace_patch(far, EXTRACE_FTRACE_NONE) != -EXTRACE_ERANGE) << 2;
    e[0] = EXTRACE_FTRACE_HW_SKIP;                                      /* left halfway */
    fail |= (call() != want || hits) << 3;
    fail |= (extrace_ftrace_patch(pc, EXTRACE_FTRACE_NONE) != 0 ||
             !pad_is(pc, EXTRACE_FTRACE_NONE)) << 4;
    fail |= (call() != want || hits) << 5;                             /* switch off */
    extrace_producer.mask0 = 1u << EXTRACE_FTRACE_SYS_ID;
    unsigned int cur = EXTRACE_FTRACE_NONE, want_n = 0;

    for (int k = 0; k < EXTRACE_FTRACE_ENTRIES; k++) {
        unsigned int f = flavor_of(k);
        uint32_t ef = 0;

        if ((f & EXTRACE_FTRACE_CALLER) && !EXTRACE_CFG_FTRACE_CALLER) {
            /* not a flavor of this build: refused, the pad and the calls unchanged */
            ef |= (extrace_ftrace_patch(pc, f) != -EXTRACE_EINVAL || !pad_is(pc, cur)) << 17;
            ef |= (call() != want || hits != ++n || log_f != cur) << 18;
            entry_fail[k] = ef;
            fail |= ef;
            continue;
        }
        want_n++;
        cur = f;
        ef |= (extrace_ftrace_patch(pc, f) != 0 || !pad_is(pc, f)) << 6;
        ef |= (call() != want || hits != ++n) << 7;
        ef |= (log_f != f) << 8;
        ef |= (log_ret != (uint32_t)(pc & ~(uintptr_t)1) + EXTRACE_FTRACE_RET_OFFSET) << 9;
        for (uint32_t i = 0; i < EXTRACE_FTRACE_ARG_REGS; i++) /* all saved, n recorded */
            ef |= ((f & 15u) > EXTRACE_FTRACE_NONE &&
                   log_args[i] != i + 1) << 10;
        ef |= ((f & EXTRACE_FTRACE_CALLER) ? !caller_seen || log_caller != caller_seen
                                           : log_caller != 0) << 11;
        entry_fail[k] = ef;
        fail |= ef;
    }
    fail |= (extrace_ftrace_patch(pc, EXTRACE_FTRACE_OFF) != 0) << 12; /* back to NOPs */
    fail |= !pad_is(pc, EXTRACE_FTRACE_OFF) << 13;
    fail |= (call() != want || hits != n) << 14;
    fail |= (extrace_ftrace_patch(pc, EXTRACE_FTRACE_OFF) != 0) << 15;
    fail |= (want_n != (EXTRACE_CFG_FTRACE_CALLER ? EXTRACE_FTRACE_ENTRIES
                                                  : EXTRACE_FTRACE_SHAPES)) << 16;
    return fail;
}
