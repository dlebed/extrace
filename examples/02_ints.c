// SPDX-License-Identifier: MIT
/*
 * 02_ints - EXTRACE_INTS: one variadic macro for 1 to 16 integers.
 *
 * Everything about the arguments is resolved at compile time with _Generic:
 *   - the count picks the emitter: 1-3 arguments go in registers to a
 *     specialized emitter, 4-16 through a compound-literal array;
 *   - the signedness of each argument becomes a constant sign mask, and signed
 *     values are zigzag-encoded, so -1 takes one byte, not five;
 *   - any 64-bit argument switches the call to the 64-bit emitter;
 *   - a non-integer argument (float, pointer) is a compile error.
 * Every value is a varint on the wire: below 128 it takes a single byte.
 * Arguments are evaluated exactly once, and only when the event is enabled.
 */
#include <stdint.h>
#include "ex_host.h"

enum port { PORT_ETH0 = 1, PORT_ETH1 = 2, PORT_WIFI = 7 };

static unsigned int reads;

static unsigned int read_fifo_level(void)           /* an argument with a side effect */
{
    return ++reads * 100u;
}

int main(void)
{
    ex_start("02_ints: variadic integer records");

    ex_section("1 to 3 arguments: register emitters, no array on the stack");
    EXTRACE_INTS(UPTIME, 123456u);
    EXTRACE_INTS(RX_PKT, 60u, PORT_WIFI);           /* an enum is an integer too */
    EXTRACE_INTS(SENSOR, 3u, 0xBEEFu, -1250);       /* the spec says: print raw in hex */
    ex_show();

    ex_section("any integer type: the C type sets the sign, the value sets the size");
    int8_t i8 = -128;
    uint16_t u16 = 65535;
    char c = 'A';
    bool b = true;
    long l = -7;
    size_t sz = sizeof(struct motor_state);
    EXTRACE_INTS(ADC_BLOCK, i8, u16, c, b, l, sz);
    ex_show();

    ex_section("64-bit arguments select the 64-bit emitter automatically");
    EXTRACE_INTS(UPTIME, UINT64_C(1) << 40);
    EXTRACE_INTS(ADC_BLOCK, INT64_MIN, UINT64_MAX, 0);
    ex_show();

    ex_section("4 to 16 arguments: one call, the values go through a compound literal");
    int16_t s[16];
    for (int i = 0; i < 16; i++)
        s[i] = (int16_t)((i - 8) * 300);
    EXTRACE_INTS(ADC_BLOCK, s[0], s[1], s[2], s[3]);
    EXTRACE_INTS(ADC_BLOCK, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10],
                 s[11], s[12], s[13], s[14], s[15]);
    ex_show();

    ex_section("arguments are evaluated once, and only when the event is enabled");
    EXTRACE_INTS(RX_PKT, read_fifo_level(), PORT_ETH0);     /* enabled: one read */
    EXTRACE_INTS(HEAP, read_fifo_level(), read_fifo_level(), 0u); /* OFF by default: none */
    EXTRACE_INTS(UPTIME, reads);                    /* 1 */
    ex_show();

    ex_section("_URGENT: publish right after the commit, even from an ISR");
    EXTRACE_INTS_URGENT(TX_DONE, 64u, 0);
    ex_drain();                                     /* visible without a flush */

    return ex_finish();
}
