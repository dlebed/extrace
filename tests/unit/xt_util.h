/* SPDX-License-Identifier: MIT */
/*
 * xt_util.h - helpers shared by the unit tests on the POSIX port: a controllable
 * timer, ring blocks, and a reader that returns every record of a ring decoded.
 */
#ifndef XT_UTIL_H
#define XT_UTIL_H

#include <string.h>
#include "extrace_test.h"
#include "extrace.h"
#include "extrace_cons.h"
#include "extrace_rec.h"
#include "extrace_test_hooks.h"

#define XT_DATA 4096u
#define XT_BLOCK_SIZE EXTRACE_RING_BLOCK_SIZE(XT_DATA, EXTRACE_MASK_WORDS)
#define XT_MIRROR_WORDS 64u

/* ---- timer ---- */

static uint32_t xt_now = 1000;
static uint32_t xt_step = 1;

static inline uint32_t xt_ts(void)
{
    xt_now += xt_step;
    return xt_now;
}

/* ---- doorbells ---- */

static unsigned int xt_notifies, xt_notifies_producer;

static inline void xt_notify(uint8_t ring_id)
{
    (void)ring_id;
    xt_notifies++;
}

static inline void xt_notify_producer(uint8_t ring_id)
{
    (void)ring_id;
    xt_notifies_producer++;
}

/* ---- a ring block and its consumer ---- */

struct xt_ring {
    uint64_t blk[(XT_BLOCK_SIZE + 4u * XT_DATA + 7u) / 8u]; /* room for larger rings */
    uint32_t block_size;
    struct extrace_cons c;
    uint32_t mirror[XT_MIRROR_WORDS];
};

/* Cold start of the test producer: private state zeroed as after a reset. */
static inline void xt_reset_world(void)
{
    memset(&extrace_producer, 0, sizeof(extrace_producer));
    extrace_posix_ts_hook = xt_ts;
    extrace_posix_notify_hook = xt_notify;
    extrace_posix_notify_producer_hook = xt_notify_producer;
    extrace_test_index_base = 0;
    extrace_test_resv_hook = nullptr;
    xt_now = 1000;
    xt_step = 1;
    xt_notifies = xt_notifies_producer = 0;
}

static inline int xt_prod_start(struct xt_ring *r, uint32_t data_size, uint32_t publish_thr)
{
    struct extrace_prod_cfg cfg = {
        .block = r->blk,
        .block_size = EXTRACE_RING_BLOCK_SIZE(data_size, EXTRACE_MASK_WORDS),
        .data_size = data_size,
        .ring_id = 7,
        .name = "xt0",
        .publish_thr = publish_thr,
    };

    r->block_size = cfg.block_size;
    return extrace_prod_init(&cfg);
}

static inline int xt_cons_attach(struct xt_ring *r)
{
    return extrace_cons_attach(&r->c, r->blk, r->block_size, r->mirror, XT_MIRROR_WORDS, 0);
}

/* A fresh world: zeroed block, producer started, consumer attached. */
static inline void xt_setup(struct xt_ring *r, uint32_t data_size, uint32_t publish_thr)
{
    xt_reset_world();
    memset(r, 0, sizeof(*r));
    EXPECT_EQ(xt_prod_start(r, data_size, publish_thr), 0);
    EXPECT_EQ(xt_cons_attach(r), 0);
}

/* ---- reading back ---- */

struct xt_rec {
    struct extrace_rec r;
    uint8_t payload[1024];
};

#define XT_MAX_RECS 4096
static struct xt_rec xt_recs[XT_MAX_RECS];
static uint8_t xt_buf[65536];

/*
 * Drain everything published: copy mode, then decode. Returns the number of
 * event records (padding skipped), or -1 on a decoder error.
 */
static inline int xt_drain(struct xt_ring *r, struct extrace_dec *d, bool include_sys)
{
    int n = 0;

    for (;;) {
        int got = extrace_cons_read(&r->c, xt_buf, sizeof(xt_buf));
        const uint8_t *p = xt_buf;

        if (got <= 0)
            return got < 0 && got != -EXTRACE_EEPOCH ? -1 : n;
        while (p < xt_buf + got) {
            struct extrace_rec rec;
            int ret = extrace_dec_next(d, &p, xt_buf + got, &rec);

            if (ret == 0)
                break;
            if (ret < 0)
                return -1;
            if (!include_sys && rec.event < EXTRACE_SYS_FIRST_USER)
                continue;
            if (n < XT_MAX_RECS) {
                xt_recs[n].r = rec;
                memcpy(xt_recs[n].payload, rec.payload,
                       rec.len < sizeof(xt_recs[n].payload) ? rec.len : sizeof(xt_recs[n].payload));
                xt_recs[n].r.payload = xt_recs[n].payload;
            }
            n++;
        }
    }
}

static inline int xt_drain_user(struct xt_ring *r)
{
    struct extrace_dec d;

    extrace_dec_reset(&d, r->c.ring_id, 0);
    extrace_prod_flush();
    return xt_drain(r, &d, false);
}

static inline uint64_t xt_int(const struct xt_rec *x, unsigned int i)
{
    uint64_t v[16] = { 0 };

    extrace_rec_ints(&x->r, v, 16, nullptr);
    return v[i];
}

static inline int xt_nints(const struct xt_rec *x)
{
    uint64_t v[16];

    return extrace_rec_ints(&x->r, v, 16, nullptr);
}

#endif /* XT_UTIL_H */
