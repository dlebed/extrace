/* SPDX-License-Identifier: MIT */
/*
 * extrace_port.h - POSIX host port for unit and concurrency tests (DESIGN §13, §18).
 *
 * Shared memory is ordinary memory. Control words use relaxed atomics and the
 * barriers are C11 fences; with EXTRACE_POSIX_TSAN the words use release stores
 * and acquire loads instead, because ThreadSanitizer does not model fences (§18.2).
 *
 * Interrupts are simulated per thread: a signal handler registered through
 * extrace_posix_set_isr() acts as the ISR, and the critical section is a
 * thread-local mask flag. A signal that arrives while the flag is set is held
 * pending and runs when the section ends, like a masked interrupt line.
 */
#ifndef EXTRACE_PORT_H
#define EXTRACE_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include "extrace_compiler.h"

#define EXTRACE_VARINT_CLZ32(v) ((unsigned int)EXTRACE_BUILTIN_CLZ32(v))

/* ---- timer: CLOCK_MONOTONIC in microseconds, or a test hook ---- */

extern uint32_t (*extrace_posix_ts_hook)(void);
uint32_t extrace_posix_clock_us(void);

static __always_inline uint32_t extrace_port_ts(void)
{
    if (extrace_posix_ts_hook)
        return extrace_posix_ts_hook();
    return extrace_posix_clock_us();
}

static inline uint32_t extrace_port_ts_freq(void)
{
    return 1000000u;
}

/* ---- simulated interrupt masking ---- */

extern thread_local volatile sig_atomic_t extrace_posix_irq_off;
extern thread_local volatile sig_atomic_t extrace_posix_irq_pending;
extern thread_local volatile sig_atomic_t extrace_posix_isr_depth;
void extrace_posix_run_pending(void);

static __always_inline unsigned long extrace_port_irq_save(void)
{
    unsigned long flags = (unsigned long)extrace_posix_irq_off;

    extrace_posix_irq_off = 1;
    barrier();
    return flags;
}

static __always_inline void extrace_port_irq_restore(unsigned long flags)
{
    barrier();
    extrace_posix_irq_off = (sig_atomic_t)flags;
    if (!flags && extrace_posix_irq_pending)
        extrace_posix_run_pending();
}

#define EXTRACE_PORT_HAS_IN_ISR
static inline bool extrace_port_in_isr(void)
{
    return extrace_posix_isr_depth != 0;
}

/* ---- barriers and shared words ---- */

#ifdef EXTRACE_POSIX_TSAN
/*
 * ThreadSanitizer does not model standalone fences (§18.2): shared words use
 * release stores and acquire loads, which is stronger, and the barriers only
 * keep the compiler from moving accesses.
 */
static inline void extrace_port_wmb(void)
{
    barrier();
}

static inline void extrace_port_rmb(void)
{
    barrier();
}

static inline void extrace_port_mb(void)
{
    barrier();
}

#define EXTRACE_PORT_SHM_LD32(p) __atomic_load_n((const uint32_t *)(p), __ATOMIC_ACQUIRE)
#define EXTRACE_PORT_SHM_ST32(p, v) __atomic_store_n((uint32_t *)(p), (v), __ATOMIC_RELEASE)
#else
static inline void extrace_port_wmb(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void extrace_port_rmb(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

static inline void extrace_port_mb(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#define EXTRACE_PORT_SHM_LD32(p) __atomic_load_n((const uint32_t *)(p), __ATOMIC_RELAXED)
#define EXTRACE_PORT_SHM_ST32(p, v) __atomic_store_n((uint32_t *)(p), (v), __ATOMIC_RELAXED)
#endif

/* ---- doorbells: counted, with an optional test callback ---- */

extern void (*extrace_posix_notify_hook)(uint8_t ring_id);
extern void (*extrace_posix_notify_producer_hook)(uint8_t ring_id);

#define EXTRACE_PORT_HAS_NOTIFY
static inline void extrace_port_notify(uint8_t ring_id)
{
    extrace_port_mb();
    if (extrace_posix_notify_hook)
        extrace_posix_notify_hook(ring_id);
}

#define EXTRACE_PORT_HAS_NOTIFY_PRODUCER
static inline void extrace_port_notify_producer(uint8_t ring_id)
{
    if (extrace_posix_notify_producer_hook)
        extrace_posix_notify_producer_hook(ring_id);
}

/* ---- misc ---- */

#define EXTRACE_PORT_HAS_BUILD_ID
extern const uint8_t *extrace_posix_build_id;
extern size_t extrace_posix_build_id_len;
static inline const uint8_t *extrace_port_build_id(size_t *len)
{
    *len = extrace_posix_build_id_len;
    return extrace_posix_build_id;
}

#define EXTRACE_PORT_HAS_VSNPRINTF
static inline int extrace_port_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return vsnprintf(buf, n, fmt, ap);
}

#define EXTRACE_PORT_ASSERT(x)                                                          \
    do {                                                                                \
        if (!(x)) {                                                                     \
            fprintf(stderr, "%s:%d: extrace assertion '%s' failed\n", __FILE__, __LINE__, \
                    #x);                                                                \
            abort();                                                                    \
        }                                                                               \
    } while (0)

/* ---- test API ---- */

/* Install fn as the ISR of the calling thread's signal sig (0 removes it). */
int extrace_posix_set_isr(int sig, void (*fn)(void));

#endif /* EXTRACE_PORT_H */
