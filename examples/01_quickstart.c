// SPDX-License-Identifier: MIT
/*
 * 01_quickstart - the producer in a few lines.
 *
 *   1. The image declares its events in extrace_events.def (config/).
 *   2. extrace_prod_init() takes the shared ring block; any time, any order.
 *   3. EXTRACE_* macros trace from any context. A disabled event costs a load,
 *      a bit test and a branch; its arguments are not evaluated.
 *   4. extrace_prod_poll() runs periodically from one task: it publishes, applies
 *      the consumer's event switches and runs its commands.
 */
#include "ex_host.h"

static unsigned int rx_one(unsigned int port)
{
    unsigned int len = 64u + 100u * port;

    EXTRACE_INTS(RX_PKT, len, port);               /* integers: 1 to 16 of them */
    return len;
}

int main(void)
{
    ex_start("01_quickstart: init, the basic macros, poll");

    ex_section("lifecycle: init writes INFO and SYNC, then the image traces");
    EXTRACE_MARK(BOOT);                             /* event only, 2 bytes, no time */
    EXTRACE(IDLE);                                  /* event and timestamp */
    for (unsigned int port = 0; port < 3; port++)
        rx_one(port);
    EXTRACE_PRINTF(LOG, "link up: %u Mbit/s, %s duplex", 1000u, "full");
    EXTRACE_STR(CONSOLE, "hello from the producer");

    /* published at the threshold, at a poll, on a flush or for an _URGENT record */
    extrace_prod_poll();
    ex_show();

    ex_section("signedness comes from the argument types");
    EXTRACE_INTS(TX_DONE, 1500u, -110);             /* unsigned, then signed (zigzag) */
    ex_show();

    return ex_finish();
}
