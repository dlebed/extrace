// SPDX-License-Identifier: MIT
/*
 * 04_payloads - bytes, strings and typed structs.
 *
 *   EXTRACE_STR(EV, s)          NUL-terminated string, copied
 *   EXTRACE_BUF(EV, p, n)       raw bytes, truncated to EXTRACE_CFG_MAX_PAYLOAD
 *   EXTRACE_BUF_LARGE(EV, p, n) any length, split into FRAG records the host joins
 *   EXTRACE_STRUCT(EV, ptr)     the C type declared in the catalog, checked at
 *                               compile time; sizeof(*ptr) bytes, native order
 *   EXTRACE_STRUCT_N(EV, ptr, n) the first n bytes of it
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "ex_host.h"

/* The host decodes a STRUCT payload with the same C type, from the ELF's DWARF. */
static void print_motor(char *buf, size_t cap, const void *payload, uint32_t len)
{
    struct motor_state m = { 0 };

    memcpy(&m, payload, len < sizeof(m) ? len : sizeof(m));
    if (len < sizeof(m))
        snprintf(buf, cap, "{rpm=%" PRIu32 " current=%d mA phase=%u} first %" PRIu32 " bytes",
                 m.rpm, m.current_ma, m.phase, len);
    else
        snprintf(buf, cap, "{rpm=%" PRIu32 " current=%d mA phase=%u fault=%u revs=%" PRIu64 "}",
                 m.rpm, m.current_ma, m.phase, m.fault, m.revolutions);
}

int main(void)
{
    static uint8_t frame[600];
    static uint8_t image[1500];

    ex_start("04_payloads: strings, buffers and structs");
    ex_struct_printer(EXTRACE_EV_MOTOR, print_motor);
    for (unsigned int i = 0; i < sizeof(frame); i++)
        frame[i] = (uint8_t)i;
    for (unsigned int i = 0; i < sizeof(image); i++)
        image[i] = (uint8_t)(0xA0u + i);

    ex_section("EXTRACE_STR");
    EXTRACE_STR(CONSOLE, "AT+CWJAP=\"lab-net\"");
    ex_show();

    ex_section("EXTRACE_BUF: a 42-byte header, then a frame cut to MAX_PAYLOAD (512)");
    EXTRACE_BUF(PKT_DUMP, frame, 42);
    EXTRACE_BUF(PKT_DUMP, frame, sizeof(frame));
    ex_show();

    ex_section("EXTRACE_BUF_LARGE: 1500 bytes as fragments, other records may interleave");
    EXTRACE_BUF_LARGE(FW_CHUNK, image, sizeof(image));
    ex_show();

    ex_section("EXTRACE_STRUCT: type-checked against the catalog's struct motor_state");
    struct motor_state m = {
        .rpm = 2400, .current_ma = -350, .phase = 2, .fault = 0, .revolutions = 123456789,
    };
    EXTRACE_STRUCT(MOTOR, &m);
    EXTRACE_STRUCT_N(MOTOR, &m, offsetof(struct motor_state, fault));
    m.fault = 3;
    EXTRACE_STRUCT_URGENT(MOTOR, &m);               /* a fault: publish now */
    ex_show();

    ex_section("an ANY event takes whichever macro fits the call site");
    struct { uint32_t addr, val; } wr = { 0x40021000u, 0x81u };
    EXTRACE_INTS(REG_WR, wr.addr, wr.val);
    EXTRACE_BUF(REG_WR, &wr, sizeof(wr));
    ex_show();

    return ex_finish();
}
