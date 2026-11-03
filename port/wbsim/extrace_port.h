/* SPDX-License-Identifier: MIT */
/*
 * extrace_port.h - write-buffer simulator port (DESIGN §18.3). Every hook is a
 * scheduling point of the simulator.
 */
#ifndef EXTRACE_PORT_H
#define EXTRACE_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "extrace_compiler.h"
#include "wbsim.h"

static __always_inline uint32_t extrace_port_ts(void)
{
    return wbsim_ts();
}

static inline uint32_t extrace_port_ts_freq(void)
{
    return 1000000u;
}

static __always_inline unsigned long extrace_port_irq_save(void)
{
    unsigned long f = wbsim_irq_save();

    barrier();
    return f;
}

static __always_inline void extrace_port_irq_restore(unsigned long flags)
{
    barrier();
    wbsim_irq_restore(flags);
}

static inline void extrace_port_wmb(void)
{
    barrier();
    wbsim_wmb();
    barrier();
}

static inline void extrace_port_rmb(void)
{
    barrier();
    wbsim_rmb();
    barrier();
}

static inline void extrace_port_mb(void)
{
    barrier();
    wbsim_mb();
    barrier();
}

#define EXTRACE_PORT_SHM_LD32(p) wbsim_ld32(p)
#define EXTRACE_PORT_SHM_LD32_OWN(p) wbsim_ld32_own(p)
#define EXTRACE_PORT_SHM_ST32(p, v) wbsim_st32((p), (v))

#define EXTRACE_PORT_HAS_MEMCPY
#define extrace_port_memcpy wbsim_memcpy

#define EXTRACE_PORT_HAS_NOTIFY
static inline void extrace_port_notify(uint8_t ring_id)
{
    wbsim_notify(ring_id);
}

#define EXTRACE_PORT_HAS_IN_ISR
static inline bool extrace_port_in_isr(void)
{
    return wbsim_in_isr();
}

#define EXTRACE_PORT_ASSERT(x)                                                          \
    do {                                                                                \
        if (!(x)) {                                                                     \
            fprintf(stderr, "%s:%d: extrace assertion '%s' failed\n", __FILE__, __LINE__, \
                    #x);                                                                \
            abort();                                                                    \
        }                                                                               \
    } while (0)

#endif /* EXTRACE_PORT_H */
