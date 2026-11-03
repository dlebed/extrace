/* SPDX-License-Identifier: MIT */
/*
 * extrace_disp.h - dispatcher, sink interface, bulk frames and control API
 * (DESIGN §12).
 *
 * All memory is caller-provided; the dispatcher never allocates. Sink callbacks
 * are synchronous: a window is released only after every sink has returned.
 */
#ifndef EXTRACE_DISP_H
#define EXTRACE_DISP_H

#include <stdint.h>
#include <stdbool.h>
#include "extrace_cfg.h"
#include "extrace_cons.h"
#include "extrace_rec.h"

enum extrace_sink_class { EXTRACE_SINK_BULK, EXTRACE_SINK_RECORD };

enum extrace_sink_ret {
    EXTRACE_SINK_DONE,      /* finished with the window; no reference remains */
    EXTRACE_SINK_BUSY,      /* lossy sinks only: cannot accept now */
};

enum extrace_ring_evt {
    EXTRACE_RING_ATTACHED,      /* attached to a new epoch */
    EXTRACE_RING_RESUMED,       /* consumer restart: resumed the current epoch */
    EXTRACE_RING_DETACHED,      /* producer (re)initializing or gone */
    EXTRACE_RING_ATTACH_FAILED, /* version, layout or validation failure */
    EXTRACE_RING_NODECODE,      /* attached, records too large to decode (§12.5) */
};

#define EXTRACE_SINK_F_LOSSY 0x01u
#define EXTRACE_SINK_F_DISABLED 0x02u

struct extrace_ev_map {          /* caller-owned bitmap, sized for the ring's ev_count */
    const uint32_t *map;
    uint16_t words;              /* IDs at or beyond words * 32 do not match */
};

struct extrace_sink_stats {
    uint64_t bytes;
    uint32_t windows;
    uint32_t records;
    uint32_t busy;
    uint32_t gaps;
    uint32_t errors;
};

struct extrace_sink;

struct extrace_ring_evt_info {
    const struct extrace_shm_pinfo *pi; /* validated snapshot */
    uint32_t epoch;
    uint64_t pos;                   /* stream position the ring continues at */
    uint64_t disp_ts;
};

struct extrace_sink_ops {
    /* BULK; can_accept == NULL means always */
    bool (*can_accept)(struct extrace_sink *s, uint8_t ring_id, uint32_t len);
    /* BULK: called when new data is available; synchronous (§12.6) */
    enum extrace_sink_ret (*window)(struct extrace_sink *s, uint8_t ring_id,
                                    const struct extrace_window *w);
    /* RECORD */
    void (*record)(struct extrace_sink *s, const struct extrace_rec *r);
    /* both: ring attached / resumed / detached, with validated pinfo */
    void (*ring_event)(struct extrace_sink *s, uint8_t ring_id, enum extrace_ring_evt e,
                       const struct extrace_ring_evt_info *info);
    /* optional, end of a dispatch round */
    void (*flush)(struct extrace_sink *s);
};

struct extrace_sink {
    const struct extrace_sink_ops *ops;
    uint8_t cls;                    /* BULK / RECORD */
    uint8_t flags;                  /* EXTRACE_SINK_F_* */
    uint8_t kind_mask;              /* RECORD: which kinds (bit per kind) */
    uint32_t ring_map[8];           /* which rings: bit per ring_id (0..254) */
    /* RECORD: per dispatcher slot; map == NULL means all events */
    struct extrace_ev_map ev_filter[EXTRACE_CFG_DISP_MAX_RINGS];
    uint32_t gap_map[8];            /* BULK: next frame of the ring carries GAP */
    struct extrace_sink_stats stats;
    struct extrace_sink *next;      /* intrusive list, no allocation */
};

struct extrace_disp_stats {
    uint64_t bytes;
    uint32_t polls;
    uint32_t windows;
    uint32_t records;
    uint32_t backpressure;
    uint32_t epoch_aborts;          /* staged copies or releases that saw a new epoch */
    uint32_t decode_errors;
    uint32_t attach_errors;         /* version, validation or duplicate ring_id */
};

struct extrace_disp_ring {
    struct extrace_cons *c;
    struct extrace_dec dec;
    uint32_t epoch;                 /* reported identity, for DETACHED */
    uint64_t pos;
    uint8_t attached;               /* reported to the sinks as attached */
    uint8_t decode;                 /* records of this ring are decoded */
    uint8_t failed;                 /* ATTACH_FAILED reported, not attached since */
};

struct extrace_disp {
    struct extrace_disp_ring ring[EXTRACE_CFG_DISP_MAX_RINGS];
    unsigned int nrings;
    unsigned int rr;                /* last served slot */
    struct extrace_sink *sinks;
    uint64_t ts64;                  /* dispatcher's extended global timer */
    struct extrace_disp_stats stats;
    uint8_t stage[EXTRACE_CFG_DISP_STAGE] __aligned(8);
};

void extrace_disp_init(struct extrace_disp *d);
int extrace_disp_add_ring(struct extrace_disp *d, struct extrace_cons *c);
int extrace_disp_add_sink(struct extrace_disp *d, struct extrace_sink *s);
int extrace_disp_del_sink(struct extrace_disp *d, struct extrace_sink *s);
/* returns bytes processed or a negative error */
int extrace_disp_poll(struct extrace_disp *d, uint32_t budget_bytes);
/* the dispatcher's 64-bit timer (§12.5), extended from extrace_port_ts() */
uint64_t extrace_disp_now(struct extrace_disp *d);
/* words of an ev_map for ring_id, from its pinfo.ev_count; 0 if not attached */
uint16_t extrace_ev_map_words(struct extrace_disp *d, uint8_t ring_id);

static inline void extrace_sink_ring_set(struct extrace_sink *s, uint8_t ring_id, bool on)
{
    if (on)
        s->ring_map[ring_id / 32u] |= 1u << (ring_id % 32u);
    else
        s->ring_map[ring_id / 32u] &= ~(1u << (ring_id % 32u));
}

/* BULK sinks: true once after a lost window of ring_id; the next frame sets GAP. */
static inline bool extrace_sink_take_gap(struct extrace_sink *s, uint8_t ring_id)
{
    uint32_t bit = 1u << (ring_id % 32u);
    bool gap = s->gap_map[ring_id / 32u] & bit;

    s->gap_map[ring_id / 32u] &= ~bit;
    return gap;
}

static inline void extrace_sink_ring_all(struct extrace_sink *s)
{
    for (unsigned int i = 0; i < 8; i++)
        s->ring_map[i] = 0xFFFFFFFFu;
}

/* ---- bulk frames (§12.7) ---- */

#define EXTRACE_BULK_MAGIC 0x46425845u /* "EXBF" */
#define EXTRACE_BULK_VERSION 1u
#define EXTRACE_BF_BOUNDARY 0x01u
#define EXTRACE_BF_ATTACH 0x02u
#define EXTRACE_BF_EPOCH_NEW 0x04u
#define EXTRACE_BF_GAP 0x08u
#define EXTRACE_BF_INFO 0x10u
#define EXTRACE_BF_DISP_STATS 0x20u
#define EXTRACE_BULK_HDR_SIZE 32u

struct extrace_bulk_hdr {           /* 32 bytes, little-endian, naturally aligned */
    uint32_t magic;
    uint8_t version;
    uint8_t ring_id;
    uint16_t flags;
    uint32_t epoch;
    uint32_t len;                   /* payload bytes following */
    uint64_t pos;                   /* stream position of the first payload byte */
    uint64_t disp_ts;               /* dispatcher's 64-bit extended timer at peek */
};
static_assert(sizeof(struct extrace_bulk_hdr) == EXTRACE_BULK_HDR_SIZE);

/* Serialize a frame header as its 32 little-endian bytes. */
void extrace_bulk_hdr_put(uint8_t out[EXTRACE_BULK_HDR_SIZE], const struct extrace_bulk_hdr *h);
/* pinfo varint-serialized as in §5.9.3, for INFO frames; returns bytes written */
uint32_t extrace_pinfo_serialize(const struct extrace_shm_pinfo *pi, uint32_t epoch, uint8_t *out,
                                 uint32_t size);

/* ---- control API for host commands (§12.10) ---- */

struct extrace_ring_info {
    uint8_t attached;
    uint8_t ring_id;
    uint16_t ev_count;
    uint32_t epoch;
    uint32_t mask_gen;
    uint8_t mask_applied;
    uint32_t lost_total;
    uint32_t hwm;
    uint32_t status;
    struct extrace_shm_pinfo pinfo;
};

int extrace_ctl_ev_set(struct extrace_disp *d, uint8_t ring_id, uint16_t id,
                       enum extrace_ev_state st);
int extrace_ctl_ev_set_range(struct extrace_disp *d, uint8_t ring_id, uint16_t first,
                             uint16_t last, enum extrace_ev_state st);
int extrace_ctl_ev_set_map(struct extrace_disp *d, uint8_t ring_id, const uint32_t *set,
                           const uint32_t *clr, uint16_t words);
int extrace_ctl_ev_get(struct extrace_disp *d, uint8_t ring_id, uint16_t id,
                       enum extrace_ev_state *st);
int extrace_ctl_ev_reset(struct extrace_disp *d, uint8_t ring_id);
int extrace_ctl_ev_applied(struct extrace_disp *d, uint8_t ring_id);
int extrace_ctl_ring_cmd(struct extrace_disp *d, uint8_t ring_id, uint32_t cmd_bits);
int extrace_ctl_ring_thr(struct extrace_disp *d, uint8_t ring_id, uint32_t publish_thr,
                         uint32_t notify_thr);
int extrace_ctl_sink_enable(struct extrace_disp *d, struct extrace_sink *s, bool on);
int extrace_ctl_sink_filter(struct extrace_disp *d, struct extrace_sink *s, uint8_t ring_id,
                            uint8_t kind_mask, const struct extrace_ev_map *ev);
int extrace_ctl_ring_info(struct extrace_disp *d, uint8_t ring_id, struct extrace_ring_info *out);
int extrace_ctl_stats(struct extrace_disp *d, struct extrace_disp_stats *out);

#endif /* EXTRACE_DISP_H */
