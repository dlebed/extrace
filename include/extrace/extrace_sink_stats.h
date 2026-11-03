/* SPDX-License-Identifier: MIT */
/*
 * extrace_sink_stats.h - record sink that counts records per (ring, event)
 * (DESIGN §12.8). Lossless and cheap: one increment per record.
 */
#ifndef EXTRACE_SINK_STATS_H
#define EXTRACE_SINK_STATS_H

#include "extrace_disp.h"

struct extrace_stats_sink_ring {
    uint8_t ring_id;
    uint8_t used;
    uint32_t lost;                  /* sum of LOST counts */
    uint32_t other;                 /* events at or beyond max_events */
    uint32_t *count;                /* caller-provided: max_events counters */
};

struct extrace_stats_sink {
    struct extrace_sink sink;
    struct extrace_stats_sink_ring *rings;
    unsigned int nrings;
    uint16_t max_events;
};

/*
 * rings: nrings slots, each with a counter array of max_events entries
 * (counters[i * max_events ...]); slots are bound to ring IDs on first use.
 */
void extrace_stats_sink_init(struct extrace_stats_sink *st, struct extrace_stats_sink_ring *rings,
                             uint32_t *counters, unsigned int nrings, uint16_t max_events);
/* records of event id seen on ring_id so far */
uint32_t extrace_stats_sink_get(const struct extrace_stats_sink *st, uint8_t ring_id, uint16_t id);
uint32_t extrace_stats_sink_lost(const struct extrace_stats_sink *st, uint8_t ring_id);
void extrace_stats_sink_clear(struct extrace_stats_sink *st);

#endif /* EXTRACE_SINK_STATS_H */
