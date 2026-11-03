// SPDX-License-Identifier: MIT
/* extrace_sink_stats.c - per (ring, event) record counters (DESIGN §12.8). */
#include "extrace_sink_stats.h"

static struct extrace_stats_sink_ring *slot(struct extrace_stats_sink *st, uint8_t ring_id,
                                            bool create)
{
    for (unsigned int i = 0; i < st->nrings; i++)
        if (st->rings[i].used && st->rings[i].ring_id == ring_id)
            return &st->rings[i];
    if (!create)
        return nullptr;
    for (unsigned int i = 0; i < st->nrings; i++) {
        if (!st->rings[i].used) {
            st->rings[i].used = 1;
            st->rings[i].ring_id = ring_id;
            return &st->rings[i];
        }
    }
    return nullptr;
}

static void stats_record(struct extrace_sink *s, const struct extrace_rec *r)
{
    struct extrace_stats_sink *st = (struct extrace_stats_sink *)s;
    struct extrace_stats_sink_ring *k = slot(st, r->ring_id, true);

    if (!k) {
        s->stats.errors++; /* more rings than slots */
        return;
    }
    if (r->event == EXTRACE_SYS_LOST) {
        uint64_t v[1];

        if (extrace_rec_ints(r, v, 1, nullptr) >= 1)
            k->lost += (uint32_t)v[0];
    }
    if (r->event < st->max_events)
        k->count[r->event]++;
    else
        k->other++;
}

static const struct extrace_sink_ops stats_ops = { .record = stats_record };

void extrace_stats_sink_init(struct extrace_stats_sink *st, struct extrace_stats_sink_ring *rings,
                             uint32_t *counters, unsigned int nrings, uint16_t max_events)
{
    extrace_port_memset(st, 0, sizeof(*st));
    st->sink.ops = &stats_ops;
    st->sink.cls = EXTRACE_SINK_RECORD;
    st->sink.kind_mask = 0xFF;
    extrace_sink_ring_all(&st->sink);
    st->rings = rings;
    st->nrings = nrings;
    st->max_events = max_events;
    for (unsigned int i = 0; i < nrings; i++)
        rings[i] = (struct extrace_stats_sink_ring){ .count = counters + (size_t)i * max_events };
    extrace_stats_sink_clear(st);
}

void extrace_stats_sink_clear(struct extrace_stats_sink *st)
{
    for (unsigned int i = 0; i < st->nrings; i++) {
        st->rings[i].lost = 0;
        st->rings[i].other = 0;
        extrace_port_memset(st->rings[i].count, 0, sizeof(uint32_t) * st->max_events);
    }
}

uint32_t extrace_stats_sink_get(const struct extrace_stats_sink *st, uint8_t ring_id, uint16_t id)
{
    struct extrace_stats_sink_ring *k = slot((struct extrace_stats_sink *)st, ring_id, false);

    return k && id < st->max_events ? k->count[id] : 0;
}

uint32_t extrace_stats_sink_lost(const struct extrace_stats_sink *st, uint8_t ring_id)
{
    struct extrace_stats_sink_ring *k = slot((struct extrace_stats_sink *)st, ring_id, false);

    return k ? k->lost : 0;
}
