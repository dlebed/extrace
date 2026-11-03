// SPDX-License-Identifier: MIT
/* extrace_sink_mem.c - frame capture into a linear buffer (DESIGN §12.7, §12.8). */
#include "extrace_sink_mem.h"

static struct extrace_sink_mem *to_mem(struct extrace_sink *s)
{
    return (struct extrace_sink_mem *)s; /* sink is the first member */
}

static bool mem_can_accept(struct extrace_sink *s, uint8_t ring_id, uint32_t len)
{
    struct extrace_sink_mem *m = to_mem(s);

    (void)ring_id;
    return m->size - m->used >= EXTRACE_BULK_HDR_SIZE + len;
}

static void put_frame(struct extrace_sink_mem *m, const struct extrace_bulk_hdr *h,
                      const struct extrace_span *sp, unsigned int n)
{
    uint8_t *q = m->buf + m->used;

    extrace_bulk_hdr_put(q, h);
    q += EXTRACE_BULK_HDR_SIZE;
    for (unsigned int i = 0; i < n; i++) {
        extrace_port_memcpy(q, sp[i].p, sp[i].len); /* word loads from the ring (§11.5) */
        q += sp[i].len;
    }
    m->used = (uint32_t)(q - m->buf);
    m->frames++;
}

static enum extrace_sink_ret mem_window(struct extrace_sink *s, uint8_t ring_id,
                                        const struct extrace_window *w)
{
    struct extrace_sink_mem *m = to_mem(s);
    uint32_t len = extrace_window_len(w);
    struct extrace_bulk_hdr h = {
        .magic = EXTRACE_BULK_MAGIC,
        .version = EXTRACE_BULK_VERSION,
        .ring_id = ring_id,
        .flags = EXTRACE_BF_BOUNDARY,
        .epoch = w->epoch,
        .len = len,
        .pos = w->pos,
        .disp_ts = w->disp_ts,
    };

    if (!mem_can_accept(s, ring_id, len))
        return EXTRACE_SINK_BUSY;
    if (extrace_sink_take_gap(s, ring_id))
        h.flags |= EXTRACE_BF_GAP;
    put_frame(m, &h, w->span, w->nspan);
    return EXTRACE_SINK_DONE;
}

static void mem_ring_event(struct extrace_sink *s, uint8_t ring_id, enum extrace_ring_evt e,
                           const struct extrace_ring_evt_info *info)
{
    struct extrace_sink_mem *m = to_mem(s);
    uint8_t buf[96];
    struct extrace_span sp = { .p = buf };
    struct extrace_bulk_hdr h = {
        .magic = EXTRACE_BULK_MAGIC,
        .version = EXTRACE_BULK_VERSION,
        .ring_id = ring_id,
        .epoch = info->epoch,
        .pos = info->pos,
        .disp_ts = info->disp_ts,
    };

    if (e != EXTRACE_RING_ATTACHED && e != EXTRACE_RING_RESUMED)
        return;
    /* INFO frame: EPOCH_NEW for a new epoch, ATTACH alone for a resume (§12.7) */
    h.flags = EXTRACE_BF_INFO |
              (e == EXTRACE_RING_ATTACHED ? EXTRACE_BF_EPOCH_NEW : EXTRACE_BF_ATTACH);
    sp.len = extrace_pinfo_serialize(info->pi, info->epoch, buf, sizeof(buf));
    h.len = sp.len;
    if (!sp.len || !mem_can_accept(s, ring_id, sp.len)) {
        s->stats.errors++;
        return;
    }
    put_frame(m, &h, &sp, 1);
}

static const struct extrace_sink_ops mem_ops = {
    .can_accept = mem_can_accept,
    .window = mem_window,
    .ring_event = mem_ring_event,
};

void extrace_sink_mem_init(struct extrace_sink_mem *m, void *buf, uint32_t size, bool lossy)
{
    extrace_port_memset(m, 0, sizeof(*m));
    m->sink.ops = &mem_ops;
    m->sink.cls = EXTRACE_SINK_BULK;
    m->sink.flags = lossy ? EXTRACE_SINK_F_LOSSY : 0u;
    extrace_sink_ring_all(&m->sink);
    m->buf = buf;
    m->size = size;
}

void extrace_sink_mem_reset(struct extrace_sink_mem *m)
{
    m->used = 0;
    m->frames = 0;
}
