// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
/* extrace_port_posix.c - state and helpers of the POSIX test port. */
#include <string.h>
#include <time.h>
#include "extrace_port.h"

uint32_t (*extrace_posix_ts_hook)(void);
void (*extrace_posix_notify_hook)(uint8_t ring_id);
void (*extrace_posix_notify_producer_hook)(uint8_t ring_id);
const uint8_t *extrace_posix_build_id;
size_t extrace_posix_build_id_len;

thread_local volatile sig_atomic_t extrace_posix_irq_off;
thread_local volatile sig_atomic_t extrace_posix_irq_pending;
thread_local volatile sig_atomic_t extrace_posix_isr_depth;

static void (*volatile isr_fn)(void);

uint32_t extrace_posix_clock_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u);
}

static void run_isr(void)
{
    void (*fn)(void) = isr_fn;

    if (!fn)
        return;
    extrace_posix_isr_depth++;
    fn();
    extrace_posix_isr_depth--;
}

/* A masked interrupt fires when the section ends. */
void extrace_posix_run_pending(void)
{
    while (extrace_posix_irq_pending && !extrace_posix_irq_off) {
        extrace_posix_irq_pending = 0;
        extrace_posix_irq_off = 1;   /* the hardware masks the line while it runs */
        run_isr();
        extrace_posix_irq_off = 0;
    }
}

static void on_signal(int sig)
{
    (void)sig;
    if (extrace_posix_irq_off) {
        extrace_posix_irq_pending = 1;
        return;
    }
    run_isr();
}

int extrace_posix_set_isr(int sig, void (*fn)(void))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    isr_fn = fn;
    sa.sa_handler = fn ? on_signal : SIG_IGN;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, nullptr);
}
