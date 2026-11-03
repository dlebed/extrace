/* SPDX-License-Identifier: MIT */
/*
 * extrace_rec.h - record decoder (DESIGN §5, §12.5): the wire grammar with full
 * bounds checking, padding skipped at every record boundary, and the per-ring
 * 64-bit timeline.
 */
#ifndef EXTRACE_REC_H
#define EXTRACE_REC_H

#include <stdint.h>
#include <stdbool.h>
#include "extrace_cfg.h"
#include "extrace_types.h"

/* struct extrace_rec.flags */
#define EXTRACE_REC_HAS_TS 0x01u     /* ts is on the 64-bit timeline */
#define EXTRACE_REC_AFTER_LOST 0x02u /* the first record after a LOST report */

struct extrace_rec {                 /* decoded record view (zero-copy) */
    uint8_t ring_id;
    uint8_t kind;
    uint8_t flags;
    uint16_t event;
    uint64_t ts;                     /* global-timer ticks on the dispatcher timeline */
    const uint8_t *payload;          /* valid during the callback only */
    uint32_t len;
};

struct extrace_dec {
    uint64_t ts;                     /* time of the last timestamped record */
    uint64_t disp_ts;                /* dispatcher's 64-bit timer at peek */
    uint32_t max_record;             /* longer records are corrupt; 0 = no limit */
    uint32_t pad_skip;               /* padding that continues beyond the decoded piece */
    uint32_t errors;
    uint32_t pad_bytes;              /* padding and cancelled (SKIP) records */
    uint32_t records;
    uint8_t ring_id;
    uint8_t anchored;                /* an absolute anchor (SYNC, LOST) was seen */
    uint8_t after_lost;
};

void extrace_dec_reset(struct extrace_dec *d, uint8_t ring_id, uint32_t max_record);

/*
 * Decode the next event record at *p, skipping padding and SKIP records (whose
 * tsd still advances the timeline). Returns 1 with *out set and *p advanced, 0
 * when only padding remained (*p == end; a PADN that runs past end leaves the
 * rest in d->pad_skip), -EXTRACE_ENOSPC when the record continues past end (*p
 * unchanged), or -EXTRACE_ECORRUPT (*p at the bad record).
 */
int extrace_dec_next(struct extrace_dec *d, const uint8_t **p, const uint8_t *end,
                     struct extrace_rec *out);

/* INTS payload: values (zigzag undone), count returned, or -EXTRACE_ECORRUPT. */
int extrace_rec_ints(const struct extrace_rec *r, uint64_t *out, unsigned int max,
                     uint32_t *sign_mask);
/* PRINTF payload: site ID and the raw argument bytes. */
int extrace_rec_printf_site(const struct extrace_rec *r, uint32_t *site, const uint8_t **args,
                            uint32_t *args_len);

/*
 * FTRACE or FTRACE_CALLER record, any of its three shapes (§5.9.7): the address
 * field (return address minus the INFO ftrace_base), the caller field (the
 * caller's return address minus ftrace_base; 0 for FTRACE, which has none) and up
 * to max argument registers. caller may be null. Returns the number of arguments
 * in the record, or -EXTRACE_ECORRUPT.
 */
int extrace_rec_ftrace(const struct extrace_rec *r, uint32_t *addr, uint32_t *caller,
                       uint32_t *args, unsigned int max);

/* Absolute 32-bit timer value S placed on the timeline of disp_ts (§12.5). */
static inline uint64_t extrace_rebase(uint64_t disp_ts, uint32_t s)
{
    return disp_ts - (uint32_t)((uint32_t)disp_ts - s);
}

#endif /* EXTRACE_REC_H */
