// SPDX-License-Identifier: MIT
/*
 * 07_contexts - interrupts, publication, overflow and the producer lifecycle.
 *
 *   - Any context may trace: thread, ISR, nested ISR, fault handler. The
 *     reservation is a short interrupt-masked section; nothing blocks.
 *   - Committed records become visible in batches: at publish_thr bytes, at
 *     extrace_prod_poll(), extrace_prod_flush(), or right away for _URGENT.
 *   - A full ring drops new records instead of overwriting old ones; the next
 *     record that fits is preceded by a LOST record with the count.
 *   - extrace_prod_init() may run again at any time: a new epoch, which the
 *     consumer follows by itself. extrace_prod_panic() seals the ring.
 */
#include <inttypes.h>
#include <stdio.h>
#include "ex_host.h"

static unsigned int irq_seq;
static unsigned int doorbells;

static void uart_isr(void)                      /* runs as an interrupt handler */
{
    EXTRACE_INTS(IRQ, ++irq_seq, 37u);
}

static void on_doorbell(uint8_t ring_id)        /* the consumer's interrupt, on its core */
{
    (void)ring_id;
    doorbells++;
}

int main(void)
{
    struct extrace_prod_stats st;
    struct extrace_cons_diag diag;

    ex_start("07_contexts: ISRs, publication, overflow, re-init, panic");

    ex_section("an interrupt arrives between two thread records");
    EXTRACE_PRINTF(LOG, "tx start");
    ex_raise_irq(uart_isr);
    EXTRACE_PRINTF(LOG, "tx end");
    ex_show();

    ex_section("publication: committed records wait for publish_thr, a poll or a flush");
    EXTRACE_INTS(RX_PKT, 60u, 1u);
    ex_drain();                                 /* not published yet */
    extrace_prod_poll();                        /* also applies the consumer's first mask: CFG */
    ex_drain();
    EXTRACE_URGENT(IDLE);                       /* _URGENT: published at once */
    ex_drain();

    ex_section("the doorbell: the consumer asks for one per 256 bytes");
    extrace_posix_notify_hook = on_doorbell;
    extrace_cons_set_thr(&ex_cons, 256, 256);   /* publish and notify thresholds */
    extrace_prod_poll();                        /* applies them */
    for (unsigned int i = 0; i < 100; i++)
        EXTRACE_INTS(RX_PKT, 60u + i, 1u);
    printf("     %u doorbells for 100 records\n", doorbells);
    ex_quiet_records(true);
    ex_show();

    ex_section("overflow: the consumer stalls, the producer drops and counts");
    for (unsigned int i = 0; i < 1000; i++)
        EXTRACE_INTS(SENSOR, i % 4u, 0x8000u + i, -(int)i);
    extrace_prod_stats(&st);
    printf("     producer: %" PRIu32 " lost so far, ring high-water mark %" PRIu32 " bytes\n",
           st.lost_total, st.hwm);
    ex_show();                                  /* the consumer catches up */
    ex_quiet_records(false);
    EXTRACE_PRINTF(LOG, "back to normal");      /* LOST goes in first */
    ex_show();

    ex_section("re-init: a new epoch; the consumer re-attaches by itself");
    extrace_prod_init(&ex_prod_cfg);
    EXTRACE_MARK(BOOT);
    ex_show();

    ex_section("panic: publish what is committed, seal the ring");
    EXTRACE_PRINTF(WARN, "bus fault at %p", (void *)0x40080000);
    extrace_prod_panic(0xBADu, 0x08001234u);    /* from the fault handler */
    EXTRACE_PRINTF(LOG, "not recorded after a panic");
    ex_drain();
    extrace_cons_diag(&ex_cons, &diag);
    printf("     consumer sees status %s\n", diag.status == EXTRACE_ST_PANIC ? "PANIC" : "RUNNING");

    return ex_finish();
}
