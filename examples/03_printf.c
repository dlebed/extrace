// SPDX-License-Identifier: MIT
/*
 * 03_printf - deferred printf: the device never formats.
 *
 * EXTRACE_PRINTF(EV, "fmt", ...) stores the format string, file, line and one
 * 4-bit class per argument in a call-site descriptor that stays in the ELF. The
 * record carries the site ID and the raw argument values; the host formats the
 * line from the descriptor. The call costs about as much as EXTRACE_INTS.
 *
 * The macro is variadic over 0 to 16 arguments of any of these classes, chosen
 * by _Generic from the C type:
 *   integers up to 32 or 64 bits, signed or not  -> varint (zigzag if signed)
 *   float / double                                -> 4 / 8 raw bytes
 *   char * / const char *                         -> the bytes are copied
 *   EXTRACE_CSTR(s): a constant string            -> its address only
 *   any other pointer                             -> its address
 * Up to three 32-bit integers go in registers; anything else through an array.
 * The format must be a string literal (a macro that expands to one is fine).
 */
#include <inttypes.h>
#include <stdint.h>
#include "ex_host.h"

static const char *const state_names[] = { "IDLE", "CONNECTING", "ONLINE", "ERROR" };

int main(void)
{
    ex_start("03_printf: deferred formatting, any argument types");

    ex_section("no arguments, then 1-3 integers: the register emitters");
    EXTRACE_PRINTF(LOG, "scheduler started");
    EXTRACE_PRINTF(LOG, "task %u ready", 4u);
    EXTRACE_PRINTF(LOG, "task %u prio %d stack %#x", 5u, -2, 0x400u);
    ex_show();

    ex_section("every argument class in one call");
    int64_t offset_ns = -1234567890123;
    uint64_t bytes = UINT64_C(5000000000);
    float volts = 3.3f;
    double ratio = 0.618034;
    char ssid[] = "lab-net";                        /* a buffer: its bytes are copied */
    void *buf = (void *)0x20001000;
#define NET_FMT "ofs=%" PRId64 " ns tx=%" PRIu64 " B v=%.2f r=%.4f ssid=%s p=%p"
    EXTRACE_PRINTF(LOG, NET_FMT, offset_ns, bytes, volts, ratio, ssid, buf); /* a literal too */
    ex_show();

    ex_section("strings: copied (up to 64 bytes) or, for constants, logged by address");
    unsigned int st = 2;
    EXTRACE_PRINTF(LOG, "copied: %s", state_names[st]);
    EXTRACE_PRINTF(LOG, "by address: %s", EXTRACE_CSTR(state_names[st]));
    const char *banner = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                         "-this-part-is-cut";
    EXTRACE_PRINTF(LOG, "long: %s", banner);
    ex_show();

    ex_section("width, precision and flags are applied on the host");
    EXTRACE_PRINTF(WARN, "[%-6s] %08.3f %+d %5c|", "temp", 36.6, 42, 'x');
    ex_show();

    ex_section("up to 16 arguments");
#define FMT16 "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d"
    EXTRACE_PRINTF(LOG, FMT16, 1, -2, 3, -4, 5, -6, 7, -8, 9, -10, 11, -12, 13, -14, 15, -16);
    ex_show();

    ex_section("EXTRACE_TEXT: formatted on the device instead, into a STR record");
    EXTRACE_TEXT(CONSOLE, "fw %d.%d.%d (%s)", 1, 4, 2, "release");
    ex_show();

    return ex_finish();
}
