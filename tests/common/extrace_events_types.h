/* SPDX-License-Identifier: MIT */
/* extrace_events_types.h - C types of the STRUCT events of the test catalog. */
#ifndef EXTRACE_EVENTS_TYPES_H
#define EXTRACE_EVENTS_TYPES_H

#include <stdint.h>

struct test_struct {
    uint32_t a;
    uint16_t b;
    uint8_t c;
    uint64_t d;
};

#endif /* EXTRACE_EVENTS_TYPES_H */
