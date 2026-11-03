/* SPDX-License-Identifier: MIT */
/* extrace_events_types.h - C types of the STRUCT events of the example catalog. */
#ifndef EXTRACE_EVENTS_TYPES_H
#define EXTRACE_EVENTS_TYPES_H

#include <stdint.h>

struct motor_state {
    uint32_t rpm;
    int16_t current_ma;
    uint8_t phase;
    uint8_t fault;
    uint64_t revolutions;
};

#endif /* EXTRACE_EVENTS_TYPES_H */
