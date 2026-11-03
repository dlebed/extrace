// SPDX-License-Identifier: MIT
/*
 * 06_control - what decides whether a call site records.
 *
 *   compile time  EXTRACE_CFG_ENABLE=0 removes every call site; a group declared
 *                 with compiled-in 0 removes its events (EXTRACE_CT_<EV> is 0);
 *                 -DEXTRACE_GRP_<G>_ON=0/1 overrides a group per build.
 *   run time      one mask bit per event, set from the catalog default at init
 *                 and switched by the consumer at any time; the producer applies
 *                 a change at its next extrace_prod_poll() and records a CFG.
 *
 * EXTRACE_ON(EV) is the same test as a bool, for preparing expensive arguments.
 * The runtime-ID emitters (extrace_emit_dyn_*) take the event as data, for
 * generic layers; they test the same mask.
 */
#include "ex_host.h"

static unsigned int checksum_calls;

static uint32_t checksum(const uint8_t *p, unsigned int n)   /* expensive */
{
    uint32_t s = 0;

    checksum_calls++;
    for (unsigned int i = 0; i < n; i++)
        s = (s << 1 | s >> 31) ^ p[i];
    return s;
}

/* A generic register-access layer: the event comes from a table, not from a name. */
static const uint16_t bus_event[] = { EXTRACE_EV_REG_WR, EXTRACE_EV_HEAP };

static void bus_write(unsigned int bus, uint32_t addr, uint32_t val)
{
    uint32_t v[2] = { addr, val };

    extrace_emit_dyn_ints(bus_event[bus], v, 2, 0);
    /* ... the actual write ... */
}

int main(void)
{
    static const uint8_t blob[256] = { 1, 2, 3 };

    ex_start("06_control: compile-time groups, runtime switches, guards");

    ex_section("compile time: the VERBOSE group is not built");
    static_assert(!EXTRACE_CT_REG_DUMP, "REG_DUMP is in a compiled-out group");
    EXTRACE_BUF(REG_DUMP, blob, checksum(blob, sizeof(blob)) & 0xFFu); /* no code at all */
    EXTRACE_PRINTF(LOG, "checksum() called %u times", checksum_calls);
    ex_show();

    ex_section("EXTRACE_ON guards work that only feeds the trace");
    if (EXTRACE_ON(LOG)) {
        uint32_t sum = checksum(blob, sizeof(blob));

        EXTRACE_PRINTF(LOG, "blob: %zu bytes, sum %#x", sizeof(blob), sum);
    }
    if (EXTRACE_ON(HEAP))                           /* OFF by default */
        EXTRACE_INTS(HEAP, checksum(blob, 16), 0u, 0u);
    EXTRACE_PRINTF(LOG, "checksum() called %u times", checksum_calls);
    ex_show();

    ex_section("the consumer switches HEAP on and RX_PKT off; the producer applies at poll");
    extrace_cons_ev_set(&ex_cons, EXTRACE_EV_HEAP, EXTRACE_EVS_ON);
    extrace_cons_ev_set(&ex_cons, EXTRACE_EV_RX_PKT, EXTRACE_EVS_OFF);
    EXTRACE_INTS(HEAP, 1u, 2u, 3u);                 /* not applied yet: still off */
    extrace_prod_poll();
    EXTRACE_INTS(HEAP, 12288u, 4096u, 2048u);
    EXTRACE_INTS(RX_PKT, 60u, 1u);                  /* now off */
    ex_show();

    ex_section("back to the catalog defaults");
    extrace_cons_ev_reset_all(&ex_cons);
    extrace_prod_poll();
    EXTRACE_INTS(HEAP, 1u, 2u, 3u);                 /* off again */
    EXTRACE_INTS(RX_PKT, 60u, 1u);
    ex_show();

    ex_section("runtime IDs: the event is data; the mask still applies");
    bus_write(0, 0x40021018u, 0x4u);
    bus_write(1, 0x40021000u, 0x1u);                /* HEAP is off: returns false */
    ex_show();

    return ex_finish();
}
