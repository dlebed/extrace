// SPDX-License-Identifier: MIT
/*
 * 08_ftrace - function entry tracing, per-function flavors.
 *
 * On the target nothing in the source changes: the traced code is built with
 * -fpatchable-function-entry, and a host command makes the platform call
 *
 *     extrace_ftrace_patch(pc_of_fn, EXTRACE_FTRACE_ARGS(2) | EXTRACE_FTRACE_CALLER);
 *
 * which turns the NOP pad at the function's entry into a call of the flavor's
 * trampoline entry (port/arch/armv7m, port/arch/rv32; see docs/ftrace.md). The
 * trampoline passes the return address of that call, the caller's return
 * address and the argument registers to the emitter of the flavor's record.
 *
 * The host has no trampoline, so here a macro at the top of each function plays
 * the patched pad: it calls extrace_ftrace_hit(), the C entry to the same
 * emitters, with the flavor that a patch table assigns to the function.
 */
#include <stdint.h>
#include "ex_host.h"
#include "extrace_ftrace.h"

static void spi_xfer(uint32_t dev, uint32_t len);
static int crc16(const uint8_t *p, uint32_t len);
static void sched_tick(void);
static void idle_hook(void);

/* The "patch table" the host would send: function and flavor. */
static const struct {
    const void *fn;
    const char *name;
    unsigned int flavor;
} plan[] = {
    { (const void *)spi_xfer,   "spi_xfer",   EXTRACE_FTRACE_ARGS(2) | EXTRACE_FTRACE_CALLER },
    { (const void *)crc16,      "crc16",      EXTRACE_FTRACE_ARGS(2) },
    { (const void *)sched_tick, "sched_tick", EXTRACE_FTRACE_NONE },
    { (const void *)idle_hook,  "idle_hook",  EXTRACE_FTRACE_MARK },
};

static void app_task(void);

/*
 * Host code addresses moved to where they would be in an MCU's flash, so that
 * the records have the size they have on the target (and ASLR does not matter).
 */
static uintptr_t flash_addr(const void *p)
{
    return 0x08100000u + (uintptr_t)p - (uintptr_t)(const void *)app_task;
}

static void entry_hit(const void *fn, const void *caller, const uint32_t *args)
{
    for (unsigned int i = 0; i < sizeof(plan) / sizeof(plan[0]); i++)
        if (plan[i].fn == fn)
            extrace_ftrace_hit(flash_addr(fn), flash_addr(caller), args, plan[i].flavor);
}

/* The patched pad, by hand: up to 8 argument registers, as uint32_t. */
#define FTRACE_ENTRY(fn, ...)                                                  \
    entry_hit((const void *)(fn), __builtin_return_address(0),                 \
              (const uint32_t[]){ 0 __VA_OPT__(, ) __VA_ARGS__ } __VA_OPT__(+1))

static __noinline void spi_xfer(uint32_t dev, uint32_t len)
{
    FTRACE_ENTRY(spi_xfer, dev, len);
    /* ... */
}

static __noinline int crc16(const uint8_t *p, uint32_t len)
{
    FTRACE_ENTRY(crc16, (uint32_t)(uintptr_t)p, len);
    return (int)len;
}

static __noinline void sched_tick(void)
{
    FTRACE_ENTRY(sched_tick);
}

static __noinline void idle_hook(void)
{
    FTRACE_ENTRY(idle_hook);
}

static __noinline void app_task(void)
{
    sched_tick();
    spi_xfer(2, 16);
    crc16((const uint8_t *)0x20003000u, 16);
    idle_hook();
}

int main(void)
{
    ex_start("08_ftrace: function entry tracing");
    for (unsigned int i = 0; i < sizeof(plan) / sizeof(plan[0]); i++)
        ex_symbol((const void *)flash_addr(plan[i].fn), plan[i].name); /* from the ELF */
    ex_symbol((const void *)flash_addr((const void *)app_task), "app_task");

    ex_section("four functions, four flavors: MARK, NONE, ARGS(2), ARGS(2) | CALLER");
    app_task();
    ex_show();

    ex_section("the consumer turns the FTRACE switch off: patched functions cost a test");
    extrace_cons_ev_set(&ex_cons, EXTRACE_SYS_FTRACE, EXTRACE_EVS_OFF);
    extrace_prod_poll();
    app_task();
    ex_show();

    return ex_finish();
}
