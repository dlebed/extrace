/* SPDX-License-Identifier: MIT */
/*
 * extrace_port.h - port template (DESIGN §13). Copy it next to an image and fill
 * in every hook. All hooks are static inline: there is no runtime indirection.
 *
 * Required: extrace_port_ts, extrace_port_ts_freq, extrace_port_irq_save,
 * extrace_port_irq_restore, extrace_port_wmb, extrace_port_rmb, extrace_port_mb.
 * Optional hooks are announced with an EXTRACE_PORT_HAS_* define; the library
 * supplies a default for every optional hook that is not announced.
 *
 * The trace memory is mapped non-cacheable by every agent, preferably as normal
 * memory (§13.3). The port does not know about caches.
 */
#ifndef EXTRACE_PORT_H
#define EXTRACE_PORT_H

#include <stdint.h>
#include <stddef.h>
#include "extrace_compiler.h"   /* default __used, likely(), ... (§13.7) */
#include "soc_memmap.h"         /* platform: timer address, mailbox, ... */

/*
 * SoC-global free-running timer, the same counter in every image (§8.7):
 * monotonic, up-counting, wrapping at 2^32. A 64-bit counter returns its low
 * word; a down-counter is inverted here.
 */
static __always_inline uint32_t extrace_port_ts(void)
{
    return *(const volatile uint32_t *)SOC_FREE_RUNNING_TIMER;
}

static inline uint32_t extrace_port_ts_freq(void)
{
    return SOC_TIMER_HZ;
}

/*
 * Critical section: mask every interrupt that may call extrace and return the
 * previous state; restore exactly that state. Both are compiler barriers.
 * Cortex-M: primask (or BASEPRI covering every tracing priority).
 * RISC-V M-mode: csrrci mstatus.MIE, restore with csrs.
 */
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

/*
 * Ordering barriers as observed by the other agents (§7.1, §13.6).
 * ARM: DMB (full system on Cortex-M; DMB OSHST / DMB OSH on R and A profiles).
 * RISC-V: fence w,w / fence r,r / fence rw,rw (add o/i for an I/O PMA region).
 */
static inline void extrace_port_wmb(void)
{
    __asm__ volatile("dmb" : : : "memory");
}

static inline void extrace_port_rmb(void)
{
    __asm__ volatile("dmb" : : : "memory");
}

static inline void extrace_port_mb(void)
{
    __asm__ volatile("dmb" : : : "memory");
}

/*
 * Optional: producer -> consumer doorbell. Starts with a completion barrier, so
 * the head store is visible before the doorbell (ARM DSB, RISC-V fence w,o).
 */
#define EXTRACE_PORT_HAS_NOTIFY
static inline void extrace_port_notify(uint8_t ring_id)
{
    __asm__ volatile("dsb" : : : "memory");
    soc_mailbox_ring(SOC_MBOX_TRACE, ring_id);
}

/*
 * Optional: consumer -> producer "configuration changed" doorbell. The
 * producer's handler wakes the context that runs extrace_prod_poll() and never
 * calls poll itself (§8.10).
 */
/* #define EXTRACE_PORT_HAS_NOTIFY_PRODUCER */
/* static inline void extrace_port_notify_producer(uint8_t ring_id) { ... } */

/* Optional: interrupt context query, for EXTRACE_CFG_PUBLISH_IN_ISR=0. */
/* #define EXTRACE_PORT_HAS_IN_ISR */
/* static inline bool extrace_port_in_isr(void) { ... } */

/* Recommended: the GNU build ID of the image (§10.6). */
/* #define EXTRACE_PORT_HAS_BUILD_ID */
/* static inline const uint8_t *extrace_port_build_id(size_t *len) { ... } */

/* Only for EXTRACE_CFG_PRINTF_LOCAL or EXTRACE_TEXT: a reentrant formatter. */
/* #define EXTRACE_PORT_HAS_VSNPRINTF */
/* static inline int extrace_port_vsnprintf(char *b, size_t n, const char *f, va_list ap); */

/*
 * Optional: memcpy/memset for ring copies. A Device-memory mapping needs an
 * aligned-only copy (§13.3).
 */
/* #define EXTRACE_PORT_HAS_MEMCPY */
/* #define extrace_port_memcpy my_aligned_memcpy */

/*
 * Optional, function entry tracing: store one halfword of code and make it visible
 * to instruction fetch, e.g. cache maintenance on a Cortex-M7 running from
 * cacheable memory. Default: plain store, then DSB/ISB or fence(.i).
 */
/* #define EXTRACE_PORT_HAS_CODE_WRITE */
/* static inline void extrace_port_code_write16(uint16_t *p, uint16_t v) { ... } */

/* Optional: count leading zeros for the varint length, v != 0 (§8.13 rule 5). */
/* #define EXTRACE_VARINT_CLZ32(v) ((unsigned int)__builtin_clz(v)) */

/* Optional: section attributes for TCM placement, and debug assertions. */
/* #define EXTRACE_PORT_HOT_DATA __section(".dtcm") */
/* #define EXTRACE_PORT_HOT_TEXT __section(".itcm") */
/* #define EXTRACE_PORT_ASSERT(x) ... */

#endif /* EXTRACE_PORT_H */
