// SPDX-License-Identifier: MIT
/*
 * extrace_ftrace.c - patching of function entry pads (docs/ftrace.md). Generic:
 * the encodings come from the arch header, the code store from the port.
 */
#include "extrace_cfg.h"
#include "extrace_ftrace.h"

#ifndef EXTRACE_PORT_HAS_CODE_WRITE
/* Default: a plain store, then the arch's instruction-fetch synchronization. */
static inline void extrace_port_code_write16(uint16_t *p, uint16_t v)
{
    *(volatile uint16_t *)p = v;
    extrace_ftrace_sync();
}
#endif

#define FT_HW EXTRACE_FTRACE_PATCH_HW
#define FT_ALL ((1u << FT_HW) - 1u)

static_assert(FT_HW >= 2 && FT_HW <= 8, "a pad of 2..8 halfwords");

/* The patch at ea for entry i of the trampoline; once, out of line (cold code). */
static __noinline bool entry_insn(uint32_t ea, unsigned int i, uint16_t hw[FT_HW])
{
    return extrace_ftrace_insn(ea, extrace_ftrace_entries[i], hw);
}

int extrace_ftrace_patch(uintptr_t pc, unsigned int f)
{
    uint16_t *e = (uint16_t *)(pc & ~(uintptr_t)1), cur[FT_HW], want[FT_HW], hw[FT_HW];
    uint32_t ea = (uint32_t)(uintptr_t)e;
    int idx = extrace_ftrace_entry_index(f);
    unsigned int known = 0; /* bit i: cur[i] is a NOP, or halfword i of some entry's patch */
    bool set = false, nop, same = true;

    if (f != EXTRACE_FTRACE_OFF &&
        (idx < 0 || ((f & EXTRACE_FTRACE_CALLER) && !EXTRACE_CFG_FTRACE_CALLER)))
        return -EXTRACE_EINVAL;
    for (unsigned int i = 0; i < FT_HW; i++)
        want[i] = EXTRACE_FTRACE_HW_NOP;
    if (idx >= 0 && !entry_insn(ea, (unsigned int)idx, want))
        return -EXTRACE_ERANGE;
    for (unsigned int i = 0; i < FT_HW; i++) {
        cur[i] = ((const volatile uint16_t *)e)[i];
        known |= (unsigned int)(cur[i] == EXTRACE_FTRACE_HW_NOP) << i;
        same = same && cur[i] == want[i];
    }
    nop = known == FT_ALL;
    /* the patch of any entry: a function changes flavor in place */
    for (unsigned int k = 0; k < EXTRACE_FTRACE_ENTRIES; k++) {
        unsigned int m = 0;

        if (!entry_insn(ea, k, hw))
            continue;
        for (unsigned int i = 0; i < FT_HW; i++)
            m |= (unsigned int)(cur[i] == hw[i]) << i;
        set = set || m == FT_ALL;
        known |= m;
    }
    /* halfway: the skip branch first, the rest NOPs or halfwords of patches, mixed */
    if (!nop && !set && !(cur[0] == EXTRACE_FTRACE_HW_SKIP && (known | 1u) == FT_ALL))
        return -EXTRACE_EINVAL;
    if (same)
        return 0;
    /* first halfword: a branch over the pad while the rest changes */
    extrace_port_code_write16(&e[0], EXTRACE_FTRACE_HW_SKIP);
    for (unsigned int i = 1; i < FT_HW; i++)
        extrace_port_code_write16(&e[i], want[i]);
    extrace_port_code_write16(&e[0], want[0]);
    return 0;
}
