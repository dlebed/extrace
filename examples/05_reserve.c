// SPDX-License-Identifier: MIT
/*
 * 05_reserve - direct reservation: build the payload in the ring, no staging copy.
 *
 *   struct extrace_resv r;
 *   if (EXTRACE_RESERVE(EV, len, align, &r)) {     // false: disabled or ring full
 *       fill(r.payload, len);                      // contiguous, aligned as asked
 *       extrace_commit(&r);                        // or _commit_urgent / _cancel
 *   }
 *
 * The event must be BUF, STRUCT, STR or ANY, and align a power of two up to
 * EXTRACE_CFG_RESV_ALIGN_MAX; both are checked at compile time.
 * EXTRACE_RESERVE_STRUCT(EV, &r) sizes and aligns for the event's C type.
 * Every successful reservation must be committed or cancelled, soon: records
 * committed while one is open are published only after it closes.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "ex_host.h"

struct dma_desc {
    uint32_t src, dst;
    uint16_t len, flags;
};

static void print_motor(char *buf, size_t cap, const void *payload, uint32_t len)
{
    struct motor_state m;

    memcpy(&m, payload, sizeof(m) < len ? sizeof(m) : len);
    snprintf(buf, cap, "{rpm=%" PRIu32 " current=%d mA phase=%u fault=%u revs=%" PRIu64 "}",
             m.rpm, m.current_ma, m.phase, m.fault, m.revolutions);
}

/* A driver that logs its descriptor ring straight into the trace ring. */
static void log_dma_ring(unsigned int n)
{
    struct extrace_resv r;

    if (!EXTRACE_RESERVE(DMA_DESC, n * (uint32_t)sizeof(struct dma_desc), 4, &r))
        return;                                     /* disabled or no room: no work at all */
    struct dma_desc *d = r.payload;                 /* 4-byte aligned, as requested */
    for (unsigned int i = 0; i < n; i++)
        d[i] = (struct dma_desc){ 0x20000000u + 64u * i, 0x40013000u, 64, (uint16_t)(1u << i) };
    extrace_commit(&r);
}

int main(void)
{
    struct extrace_resv r;

    ex_start("05_reserve: zero-copy records");
    ex_struct_printer(EXTRACE_EV_MOTOR, print_motor);

    ex_section("fill an array of descriptors in place");
    log_dma_ring(3);
    ex_show();

    ex_section("EXTRACE_RESERVE_STRUCT: the size and alignment of the catalog type");
    if (EXTRACE_RESERVE_STRUCT(MOTOR, &r)) {
        struct motor_state *m = r.payload;

        *m = (struct motor_state){ .rpm = 1200, .current_ma = 80, .phase = 1 };
        m->revolutions = 42;
        extrace_commit_urgent(&r);                  /* publish now */
    }
    ex_drain();

    ex_section("cancel: decide after reserving; the space becomes a skipped record");
    if (EXTRACE_RESERVE(PKT_DUMP, 16, 1, &r)) {
        memset(r.payload, 0xEE, 16);
        bool crc_ok = false;                        /* e.g. found out while copying */
        if (crc_ok)
            extrace_commit(&r);
        else
            extrace_cancel(&r);
    }
    EXTRACE_PRINTF(WARN, "dropped a frame with a bad CRC");
    ex_show();

    ex_section("records committed while a reservation is open wait for it");
    if (EXTRACE_RESERVE(PKT_DUMP, 4, 1, &r)) {
        EXTRACE_INTS(RX_PKT, 60u, 1u);              /* e.g. from an ISR meanwhile */
        extrace_prod_flush();
        ex_drain();                                 /* nothing yet */
        memcpy(r.payload, "\xde\xad\xbe\xef", 4);
        extrace_commit(&r);
    }
    ex_show();                                      /* both, in reservation order */

    return ex_finish();
}
