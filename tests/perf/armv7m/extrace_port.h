/* SPDX-License-Identifier: MIT */
/* extrace_port.h - Cortex-M4 under unicorn: timer at 0x40000004, PRIMASK critical section. */
#ifndef EXTRACE_PORT_H
#define EXTRACE_PORT_H

#include <stdint.h>
#include <stddef.h>
#include "extrace_compiler.h"

static __always_inline uint32_t extrace_port_ts(void)
{
    return *(const volatile uint32_t *)0x40000004u;
}

static inline uint32_t extrace_port_ts_freq(void)
{
    return 10000000u;
}

static __always_inline unsigned long extrace_port_irq_save(void)
{
    unsigned long flags;

    __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(flags) : : "memory");
    return flags;
}

static __always_inline void extrace_port_irq_restore(unsigned long flags)
{
    __asm__ volatile("msr primask, %0" : : "r"(flags) : "memory");
}

static inline void extrace_port_wmb(void) { __asm__ volatile("dmb" : : : "memory"); }
static inline void extrace_port_rmb(void) { __asm__ volatile("dmb" : : : "memory"); }
static inline void extrace_port_mb(void) { __asm__ volatile("dmb" : : : "memory"); }

#endif /* EXTRACE_PORT_H */
