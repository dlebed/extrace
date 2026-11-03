/* SPDX-License-Identifier: MIT */
/* extrace_port.h - RV32 M-mode on qemu virt: CLINT mtime, mstatus.MIE critical section. */
#ifndef EXTRACE_PORT_H
#define EXTRACE_PORT_H

#include <stdint.h>
#include <stddef.h>
#include "extrace_compiler.h"

#define PERF_MTIME 0x0200BFF8u

static __always_inline uint32_t extrace_port_ts(void)
{
    return *(const volatile uint32_t *)PERF_MTIME;
}

static inline uint32_t extrace_port_ts_freq(void)
{
    return 10000000u;
}

static __always_inline unsigned long extrace_port_irq_save(void)
{
    unsigned long flags;

    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(flags) : : "memory");
    return flags;
}

static __always_inline void extrace_port_irq_restore(unsigned long flags)
{
    __asm__ volatile("csrw mstatus, %0" : : "r"(flags) : "memory");
}

static inline void extrace_port_wmb(void) { __asm__ volatile("fence w,w" : : : "memory"); }
static inline void extrace_port_rmb(void) { __asm__ volatile("fence r,r" : : : "memory"); }
static inline void extrace_port_mb(void) { __asm__ volatile("fence rw,rw" : : : "memory"); }

#endif /* EXTRACE_PORT_H */
