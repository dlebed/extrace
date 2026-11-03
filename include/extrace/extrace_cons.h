/* SPDX-License-Identifier: MIT */
/*
 * extrace_cons.h - ring reader: attach, zero-copy peek/release, copy mode and
 * consumer-side control (DESIGN §7.3, §7.5-§7.7, §9.3, §11).
 *
 * A consumer runs in one context. A struct extrace_cons must be zero-initialized
 * before its first attach (static storage is); the attach then keeps its pinfo
 * snapshot to detect a producer restarted with a different image (§7.6).
 */
#ifndef EXTRACE_CONS_H
#define EXTRACE_CONS_H

#include <stdint.h>
#include <stdbool.h>
#include "extrace_cfg.h"
#include "extrace_types.h"
#include "extrace_shm.h"

/* struct extrace_cons.flags */
#define EXTRACE_CF_ATTACHED 0x01u
#define EXTRACE_CF_ERROR 0x02u
#define EXTRACE_CF_HAVE_SNAP 0x04u
#define EXTRACE_CF_RESUMED 0x10u   /* the last attach resumed the current epoch */

/* mirror: EXTRACE_CONS_MIRROR_WORDS(n) words for rings of up to n events */
#define EXTRACE_CONS_MIRROR_WORDS(n) (2u * (((n) + 31u) / 32u))

struct extrace_cons_stats {
    uint32_t attaches;       /* attaches to a new epoch */
    uint32_t resumes;        /* attaches that resumed the current epoch */
    uint32_t epoch_changes;
    uint32_t attach_errors;  /* validation failures (version, corrupt pinfo, mirror) */
    uint32_t head_errors;    /* head - tail > size with a stable epoch (§17) */
    uint32_t windows;
    uint64_t bytes;
};

/* Producer diagnostics of the attached epoch (pstate, §6.2). */
struct extrace_cons_diag {
    uint32_t lost_total;     /* records dropped in this epoch */
    uint32_t hwm;            /* high-water mark of ring usage, bytes */
    uint32_t status;         /* EXTRACE_ST_RUNNING, EXTRACE_ST_PANIC */
};

struct extrace_cons {
    uint8_t *block;                 /* consumer view of the ring block */
    uint32_t block_size;
    const uint8_t *data;
    uint32_t size;
    uint32_t tail;                  /* free-running index, local copy of cstate.tail */
    uint32_t epoch;                 /* attached epoch */
    uint64_t stream_pos;            /* tail extended to 64 bits (§6.6) */
    uint32_t cmd_pending;           /* command bits not yet acknowledged (§7.7) */
    uint32_t mask_gen, cmd_seq;     /* mirrors of cstate words (§6.3 rule 8) */
    uint32_t notify_thr, publish_thr;
    uint32_t *mask_mirror;          /* caller-provided: mask_set words, then mask_clr */
    uint16_t mirror_words;          /* capacity, at least 2 x mask_words */
    uint16_t attach_flags;
    uint8_t ring_id;
    uint8_t flags;                  /* EXTRACE_CF_* */
    uint16_t ev_count;              /* from pinfo: valid event IDs are < ev_count */
    uint16_t mask_words;            /* from pinfo: sizes the shared bitmaps */
    int last_error;                 /* last attach result */
    struct extrace_shm_pinfo pinfo; /* validated snapshot, also used in §7.6 A2 */
    struct extrace_cons_stats stats;
};

struct extrace_span {
    const uint8_t *p;
    uint32_t len;
};

struct extrace_window {
    struct extrace_span span[2];    /* from tail & mask to the ring end, then from 0 */
    uint8_t nspan;
    uint32_t start;                 /* free-running index of the first byte (== tail) */
    uint32_t end;                   /* free-running index after the last byte */
    uint64_t pos;                   /* 64-bit stream position of span[0].p */
    uint64_t disp_ts;               /* set by the dispatcher: its timer at peek */
    uint32_t epoch;
};

static inline uint32_t extrace_window_len(const struct extrace_window *w)
{
    return w->end - w->start;
}

int extrace_cons_attach(struct extrace_cons *c, void *block, uint32_t block_size,
                        uint32_t *mirror, uint16_t mirror_words, uint16_t flags);
/* 1: (re)attached in this call, 0: attached and unchanged, < 0: not attached */
int extrace_cons_poll_state(struct extrace_cons *c);
bool extrace_cons_attached(const struct extrace_cons *c);
/* true while the producer's epoch still equals the attached one (rmb first) */
bool extrace_cons_epoch_ok(const struct extrace_cons *c);

/* Zero-copy: window length, 0 when empty, or -EXTRACE_E* (§11.3) */
int extrace_cons_peek(struct extrace_cons *c, struct extrace_window *w, uint32_t max_bytes);
int extrace_cons_release(struct extrace_cons *c, struct extrace_window *w, uint32_t bytes);

/* Copy mode: whole records only; bytes copied, or -EXTRACE_ENOSPC (§11.5) */
int extrace_cons_read(struct extrace_cons *c, void *dst, uint32_t dst_size);

/* Commands and thresholds (§7.7) */
int extrace_cons_cmd(struct extrace_cons *c, uint32_t cmd_bits);
/* Both thresholds, applied by the producer with one THR command */
int extrace_cons_set_thr(struct extrace_cons *c, uint32_t publish_thr, uint32_t notify_thr);
int extrace_cons_set_notify(struct extrace_cons *c, uint32_t notify_thr);
int extrace_cons_set_publish_thr(struct extrace_cons *c, uint32_t thr);
bool extrace_cons_cmd_done(const struct extrace_cons *c);

/* Producer diagnostics: 0, or -EXTRACE_ENOTREADY when not attached */
int extrace_cons_diag(const struct extrace_cons *c, struct extrace_cons_diag *out);

/* Event control (§9.3) */
int extrace_cons_ev_set(struct extrace_cons *c, uint16_t id, enum extrace_ev_state st);
int extrace_cons_ev_get(const struct extrace_cons *c, uint16_t id, enum extrace_ev_state *st);
int extrace_cons_ev_set_many(struct extrace_cons *c, const uint32_t *set, const uint32_t *clr,
                             uint16_t words);
int extrace_cons_ev_reset_all(struct extrace_cons *c);
uint32_t extrace_cons_mask_gen(const struct extrace_cons *c);
bool extrace_cons_mask_applied(const struct extrace_cons *c);

/* Length of the record (or padding record) at p, 0 if malformed or cut off. */
uint32_t extrace_rec_span_len(const uint8_t *p, const uint8_t *end);

#endif /* EXTRACE_CONS_H */
