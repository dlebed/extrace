// SPDX-License-Identifier: MIT
/*
 * extrace_disp.c - dispatcher: fair draining of many rings, zero-copy bulk sinks,
 * staged record decoding for record sinks, and the control API (DESIGN §12).
 */
#include "extrace_disp.h"
#include "extrace_wire.h"

void extrace_disp_init(struct extrace_disp *d)
{
    extrace_port_memset(d, 0, sizeof(*d));
}

int extrace_disp_add_ring(struct extrace_disp *d, struct extrace_cons *c)
{
    if (!c || d->nrings >= EXTRACE_CFG_DISP_MAX_RINGS)
        return -EXTRACE_ENOSPC;
    for (unsigned int i = 0; i < d->nrings; i++)
        if (d->ring[i].c == c)
            return -EXTRACE_EINVAL;
    d->ring[d->nrings].c = c;
    d->ring[d->nrings].attached = 0;
    d->nrings++;
    return (int)d->nrings - 1;
}

int extrace_disp_add_sink(struct extrace_disp *d, struct extrace_sink *s)
{
    for (struct extrace_sink *i = d->sinks; i; i = i->next)
        if (i == s)
            return -EXTRACE_EINVAL;
    if (!s->ops || (s->cls == EXTRACE_SINK_BULK && !s->ops->window) ||
        (s->cls == EXTRACE_SINK_RECORD && !s->ops->record))
        return -EXTRACE_EINVAL;
    s->next = d->sinks;
    d->sinks = s;
    return 0;
}

int extrace_disp_del_sink(struct extrace_disp *d, struct extrace_sink *s)
{
    for (struct extrace_sink **i = &d->sinks; *i; i = &(*i)->next) {
        if (*i == s) {
            *i = s->next;
            s->next = nullptr;
            return 0;
        }
    }
    return -EXTRACE_EINVAL;
}

uint64_t extrace_disp_now(struct extrace_disp *d)
{
    d->ts64 += (uint32_t)(extrace_port_ts() - (uint32_t)d->ts64);
    return d->ts64;
}

static bool on_ring(const struct extrace_sink *s, uint8_t ring_id)
{
    return !(s->flags & EXTRACE_SINK_F_DISABLED) &&
           ((s->ring_map[ring_id / 32u] >> (ring_id % 32u)) & 1u);
}

static int slot_of(const struct extrace_disp *d, uint8_t ring_id)
{
    for (unsigned int i = 0; i < d->nrings; i++)
        if (d->ring[i].attached && d->ring[i].c->ring_id == ring_id)
            return (int)i;
    return -1;
}

static void ring_evt(struct extrace_disp *d, unsigned int slot, enum extrace_ring_evt e)
{
    const struct extrace_disp_ring *r = &d->ring[slot];
    const struct extrace_cons *c = r->c;
    struct extrace_ring_evt_info info = {
        .pi = &c->pinfo,
        .epoch = c->epoch,
        .pos = c->stream_pos,
        .disp_ts = d->ts64,
    };

    if (e == EXTRACE_RING_DETACHED) { /* the consumer may already hold the new epoch */
        info.epoch = r->epoch;
        info.pos = r->pos;
    }

    for (struct extrace_sink *s = d->sinks; s; s = s->next)
        if (s->ops->ring_event && on_ring(s, c->ring_id))
            s->ops->ring_event(s, c->ring_id, e, &info);
}

static void on_detach(struct extrace_disp *d, unsigned int slot)
{
    struct extrace_disp_ring *r = &d->ring[slot];

    if (r->attached) {
        ring_evt(d, slot, EXTRACE_RING_DETACHED);
        r->attached = 0;
    }
}

/* Reported once until the ring attaches again. */
static void on_attach_failed(struct extrace_disp *d, unsigned int slot)
{
    struct extrace_disp_ring *r = &d->ring[slot];

    on_detach(d, slot);
    if (!r->failed) {
        r->failed = 1;
        ring_evt(d, slot, EXTRACE_RING_ATTACH_FAILED);
    }
}

/*
 * A4: the consumer of slot attached (new epoch or resume). A ring still reported
 * as attached is detached first, so sinks see DETACHED before the new ATTACHED.
 */
static void on_attach(struct extrace_disp *d, unsigned int slot)
{
    struct extrace_disp_ring *r = &d->ring[slot];
    const struct extrace_cons *c = r->c;

    on_detach(d, slot);
    for (unsigned int i = 0; i < d->nrings; i++) {
        if (i != slot && d->ring[i].attached && d->ring[i].c->ring_id == c->ring_id) {
            on_attach_failed(d, slot); /* a second ring with the same ring_id */
            return;
        }
    }
    r->attached = 1;
    r->failed = 0;
    r->epoch = c->epoch;
    r->pos = c->stream_pos;
    r->decode = c->pinfo.max_record <= EXTRACE_CFG_DISP_STAGE; /* §12.5 */
    extrace_dec_reset(&r->dec, c->ring_id, c->pinfo.max_record);
    ring_evt(d, slot, (c->flags & EXTRACE_CF_RESUMED) ? EXTRACE_RING_RESUMED
                                                      : EXTRACE_RING_ATTACHED);
    if (!r->decode)
        ring_evt(d, slot, EXTRACE_RING_NODECODE);
}

/*
 * The consumer is not attached: -EXTRACE_ENOTREADY while the producer is not up
 * (or re-initializing); any other error refused the producer's identity (§17).
 */
static void on_lost_attach(struct extrace_disp *d, unsigned int slot, int err)
{
    if (err == -EXTRACE_ENOTREADY || err == -EXTRACE_EEPOCH)
        on_detach(d, slot);
    else
        on_attach_failed(d, slot);
}

/* After a peek or release saw a new epoch: report what the consumer did. */
static void on_epoch(struct extrace_disp *d, unsigned int slot, int err)
{
    if (extrace_cons_attached(d->ring[slot].c))
        on_attach(d, slot);
    else
        on_lost_attach(d, slot, err);
}

static bool want_records(const struct extrace_disp *d, uint8_t ring_id)
{
    for (const struct extrace_sink *s = d->sinks; s; s = s->next)
        if (s->cls == EXTRACE_SINK_RECORD && on_ring(s, ring_id))
            return true;
    return false;
}

static void deliver(struct extrace_disp *d, unsigned int slot, const struct extrace_rec *rec)
{
    d->stats.records++;
    for (struct extrace_sink *s = d->sinks; s; s = s->next) {
        const struct extrace_ev_map *f = &s->ev_filter[slot];

        if (s->cls != EXTRACE_SINK_RECORD || !on_ring(s, rec->ring_id) ||
            !(((uint32_t)s->kind_mask >> rec->kind) & 1u))
            continue;
        if (f->map && (rec->event >= 32u * f->words ||
                       !((f->map[rec->event / 32u] >> (rec->event % 32u)) & 1u)))
            continue;
        s->stats.records++;
        s->ops->record(s, rec);
    }
}

/*
 * Staged decoding (§12.5): copy each span in pieces into local memory, re-check
 * the epoch after every copy, then decode from the local buffer.
 */
static int decode_window(struct extrace_disp *d, unsigned int slot, const struct extrace_window *w)
{
    struct extrace_disp_ring *r = &d->ring[slot];
    struct extrace_dec *dec = &r->dec;

    dec->disp_ts = w->disp_ts;
    for (unsigned int i = 0; i < w->nspan; i++) {
        const uint8_t *src = w->span[i].p;
        uint32_t left = w->span[i].len, carry = 0;

        while (left || carry) {
            const uint8_t *q, *end;
            uint32_t chunk;

            if (dec->pad_skip) {
                uint32_t k = dec->pad_skip < left ? dec->pad_skip : left;

                src += k;
                left -= k;
                dec->pad_skip -= k;
                continue;
            }
            chunk = EXTRACE_CFG_DISP_STAGE - carry;
            if (chunk > left)
                chunk = left;
            if (!chunk) { /* a record runs past the span end */
                dec->errors++;
                d->stats.decode_errors++;
                return 0;
            }
            extrace_port_memcpy(d->stage + carry, src, chunk);
            src += chunk;
            left -= chunk;
            if (!extrace_cons_epoch_ok(r->c)) {
                d->stats.epoch_aborts++;
                return -EXTRACE_EEPOCH; /* nothing of a restarted producer is decoded */
            }
            q = d->stage;
            end = d->stage + carry + chunk;
            carry = 0;
            for (;;) {
                struct extrace_rec rec;
                int ret = extrace_dec_next(dec, &q, end, &rec);

                if (ret == 1) {
                    deliver(d, slot, &rec);
                    continue;
                }
                if (ret == -EXTRACE_ENOSPC) {
                    carry = (uint32_t)(end - q);
                    EXTRACE_BUILTIN_MEMMOVE(d->stage, q, carry);
                } else if (ret < 0) {
                    d->stats.decode_errors++;
                    return 0; /* the next window starts at a record boundary */
                }
                break;
            }
        }
    }
    dec->pad_skip = 0;
    return 0;
}

static bool admit(struct extrace_disp *d, uint8_t ring_id, uint32_t len)
{
    for (struct extrace_sink *s = d->sinks; s; s = s->next)
        if (s->cls == EXTRACE_SINK_BULK && !(s->flags & EXTRACE_SINK_F_LOSSY) &&
            on_ring(s, ring_id) && s->ops->can_accept && !s->ops->can_accept(s, ring_id, len))
            return false;
    return true;
}

static void offer_bulk(struct extrace_disp *d, uint8_t ring_id, const struct extrace_window *w)
{
    uint32_t len = extrace_window_len(w);

    for (struct extrace_sink *s = d->sinks; s; s = s->next) {
        if (s->cls != EXTRACE_SINK_BULK || !on_ring(s, ring_id))
            continue;
        if (s->ops->window(s, ring_id, w) == EXTRACE_SINK_DONE) {
            s->stats.windows++;
            s->stats.bytes += len;
            continue;
        }
        s->stats.busy++;
        s->stats.gaps++;
        if (!(s->flags & EXTRACE_SINK_F_LOSSY))
            s->stats.errors++; /* contract violation: never re-offered */
        s->gap_map[ring_id / 32u] |= 1u << (ring_id % 32u);
    }
}

int extrace_disp_poll(struct extrace_disp *d, uint32_t budget_bytes)
{
    uint32_t left = budget_bytes, done = 0;
    unsigned int first = d->rr + 1u;

    extrace_disp_now(d); /* every poll: keeps the 64-bit extension across idle periods */
    d->stats.polls++;
    for (unsigned int i = 0; i < d->nrings && left; i++) {
        unsigned int slot = (first + i) % d->nrings;
        struct extrace_disp_ring *r = &d->ring[slot];
        struct extrace_cons *c = r->c;
        struct extrace_window w;
        uint32_t quantum;
        int n, ret;

        if (!extrace_cons_attached(c) || !r->attached) {
            ret = extrace_cons_poll_state(c);
            if (ret < 0) {
                if (ret != -EXTRACE_ENOTREADY)
                    d->stats.attach_errors++;
                on_lost_attach(d, slot, ret);
                continue;
            }
            if (ret == 1 || !r->attached)
                on_attach(d, slot);
            if (!r->attached)
                continue;
        }
        quantum = c->size / 2u < left ? c->size / 2u : left;
        n = extrace_cons_peek(c, &w, quantum);
        if (n == -EXTRACE_EEPOCH) {
            on_epoch(d, slot, n);
            continue;
        }
        if (n < 0) {
            if (!extrace_cons_attached(c)) {
                if (n != -EXTRACE_ENOTREADY)
                    d->stats.attach_errors++;
                on_lost_attach(d, slot, n);
            }
            continue;
        }
        if (n == 0)
            continue;
        /*
         * After the peek read head: every record of the window, anchors included,
         * was timestamped before this, as extrace_rebase() requires (§12.5).
         */
        w.disp_ts = extrace_disp_now(d);
        if (!admit(d, c->ring_id, (uint32_t)n)) {
            d->stats.backpressure++; /* nothing offered, nothing released */
            continue;
        }
        offer_bulk(d, c->ring_id, &w);
        if (r->decode && want_records(d, c->ring_id) && decode_window(d, slot, &w) < 0) {
            /* the producer restarted: release fails below and re-attaches */
        }
        ret = extrace_cons_release(c, &w, (uint32_t)n);
        if (ret == -EXTRACE_EEPOCH || (ret < 0 && !extrace_cons_attached(c))) {
            d->stats.epoch_aborts++;
            on_epoch(d, slot, ret);
            continue;
        }
        d->stats.windows++;
        d->stats.bytes += (uint32_t)n;
        done += (uint32_t)n;
        left -= (uint32_t)n < left ? (uint32_t)n : left;
        d->rr = slot;
    }
    for (struct extrace_sink *s = d->sinks; s; s = s->next)
        if (s->ops->flush && !(s->flags & EXTRACE_SINK_F_DISABLED))
            s->ops->flush(s);
    return (int)done;
}

uint16_t extrace_ev_map_words(struct extrace_disp *d, uint8_t ring_id)
{
    int slot = slot_of(d, ring_id);

    return slot < 0 ? 0 : d->ring[slot].c->mask_words;
}

/* ---- bulk frames (§12.7) ---- */

static void put_le64(uint8_t *b, uint64_t v)
{
    extrace_le32_bytes(b, (uint32_t)v);
    extrace_le32_bytes(b + 4, (uint32_t)(v >> 32));
}

void extrace_bulk_hdr_put(uint8_t out[EXTRACE_BULK_HDR_SIZE], const struct extrace_bulk_hdr *h)
{
    extrace_le32_bytes(out, h->magic);
    out[4] = h->version;
    out[5] = h->ring_id;
    out[6] = (uint8_t)h->flags;
    out[7] = (uint8_t)(h->flags >> 8);
    extrace_le32_bytes(out + 8, h->epoch);
    extrace_le32_bytes(out + 12, h->len);
    put_le64(out + 16, h->pos);
    put_le64(out + 24, h->disp_ts);
}

uint32_t extrace_pinfo_serialize(const struct extrace_shm_pinfo *pi, uint32_t epoch, uint8_t *out,
                                 uint32_t size)
{
    uint8_t b[96], *q = b;
    uint32_t name_len = 0;

    while (name_len < 8 && pi->name[name_len])
        name_len++;
    q = extrace_put_uv32(q, 12);
    q = extrace_put_uv32(q, pi->wire_ver);
    q = extrace_put_uv32(q, pi->layout_ver);
    q = extrace_put_uv32(q, pi->flags);
    q = extrace_put_uv32(q, pi->ts_freq);
    q = extrace_put_uv32(q, pi->ring_id);
    q = extrace_put_uv32(q, epoch);
    q = extrace_put_uv32(q, pi->data_size);
    q = extrace_put_uv32(q, pi->ev_count);
    q = extrace_put_uv32(q, 0); /* ptr_size: not in pinfo */
    q = extrace_put_uv32(q, 0); /* site_id_bits: not in pinfo */
    q = extrace_put_uv32(q, sizeof(pi->build_id));
    extrace_port_memcpy(q, pi->build_id, sizeof(pi->build_id));
    q += sizeof(pi->build_id);
    q = extrace_put_uv32(q, name_len);
    extrace_port_memcpy(q, pi->name, name_len);
    q += name_len;
    if ((uint32_t)(q - b) > size)
        return 0;
    extrace_port_memcpy(out, b, (size_t)(q - b));
    return (uint32_t)(q - b);
}

/* ---- control API (§12.10) ---- */

static struct extrace_cons *ring_cons(struct extrace_disp *d, uint8_t ring_id, int *err)
{
    int slot;

    if (ring_id == EXTRACE_RING_ALL) {
        *err = -EXTRACE_EINVAL;
        return nullptr;
    }
    slot = slot_of(d, ring_id);
    if (slot < 0) {
        *err = -EXTRACE_ENOTREADY;
        return nullptr;
    }
    return d->ring[slot].c;
}

int extrace_ctl_ev_set(struct extrace_disp *d, uint8_t ring_id, uint16_t id,
                       enum extrace_ev_state st)
{
    int err = 0;
    struct extrace_cons *c = ring_cons(d, ring_id, &err);

    return c ? extrace_cons_ev_set(c, id, st) : err;
}

int extrace_ctl_ev_set_range(struct extrace_disp *d, uint8_t ring_id, uint16_t first,
                             uint16_t last, enum extrace_ev_state st)
{
    uint32_t s[EXTRACE_CFG_MAX_EVENTS / 32u + 1u], k[EXTRACE_CFG_MAX_EVENTS / 32u + 1u];
    int err = 0;
    struct extrace_cons *c = ring_cons(d, ring_id, &err);
    uint32_t words;

    if (!c)
        return err;
    words = c->mask_words;
    if (first > last || last >= c->ev_count || st > EXTRACE_EVS_OFF ||
        words > sizeof(s) / sizeof(s[0]))
        return -EXTRACE_EINVAL;
    for (uint32_t i = 0; i < words; i++) {
        s[i] = c->mask_mirror[i];
        k[i] = c->mask_mirror[words + i];
    }
    for (uint32_t id = first; id <= last; id++) {
        uint32_t bit = 1u << (id % 32u);

        s[id / 32u] &= ~bit;
        k[id / 32u] &= ~bit;
        if (st == EXTRACE_EVS_ON)
            s[id / 32u] |= bit;
        else if (st == EXTRACE_EVS_OFF)
            k[id / 32u] |= bit;
    }
    return extrace_cons_ev_set_many(c, s, k, (uint16_t)words);
}

int extrace_ctl_ev_set_map(struct extrace_disp *d, uint8_t ring_id, const uint32_t *set,
                           const uint32_t *clr, uint16_t words)
{
    int err = 0;
    struct extrace_cons *c = ring_cons(d, ring_id, &err);

    return c ? extrace_cons_ev_set_many(c, set, clr, words) : err;
}

int extrace_ctl_ev_get(struct extrace_disp *d, uint8_t ring_id, uint16_t id,
                       enum extrace_ev_state *st)
{
    int err = 0;
    struct extrace_cons *c = ring_cons(d, ring_id, &err);

    return c ? extrace_cons_ev_get(c, id, st) : err;
}

/* Apply fn to one ring, or to every attached ring for EXTRACE_RING_ALL. */
static int for_rings(struct extrace_disp *d, uint8_t ring_id,
                     int (*fn)(struct extrace_cons *c, uint32_t a, uint32_t b), uint32_t a,
                     uint32_t b)
{
    int ret = 0, any = 0;

    for (unsigned int i = 0; i < d->nrings; i++) {
        struct extrace_cons *c = d->ring[i].c;
        int r;

        if (!d->ring[i].attached || (ring_id != EXTRACE_RING_ALL && c->ring_id != ring_id))
            continue;
        any = 1;
        r = fn(c, a, b);
        if (r < 0 && !ret)
            ret = r;
        else if (r > 0 && !ret && ring_id != EXTRACE_RING_ALL)
            ret = r;
    }
    if (!any && ring_id != EXTRACE_RING_ALL)
        return -EXTRACE_ENOTREADY;
    return ret;
}

static int do_reset(struct extrace_cons *c, uint32_t a, uint32_t b)
{
    (void)a;
    (void)b;
    return extrace_cons_ev_reset_all(c);
}

static int do_applied(struct extrace_cons *c, uint32_t a, uint32_t b)
{
    (void)a;
    (void)b;
    return extrace_cons_mask_applied(c) ? 1 : 0;
}

static int do_cmd(struct extrace_cons *c, uint32_t a, uint32_t b)
{
    (void)b;
    return extrace_cons_cmd(c, a);
}

static int do_thr(struct extrace_cons *c, uint32_t a, uint32_t b)
{
    return extrace_cons_set_thr(c, a, b);
}

int extrace_ctl_ev_reset(struct extrace_disp *d, uint8_t ring_id)
{
    return for_rings(d, ring_id, do_reset, 0, 0);
}

int extrace_ctl_ev_applied(struct extrace_disp *d, uint8_t ring_id)
{
    int all = 1;

    if (ring_id != EXTRACE_RING_ALL)
        return for_rings(d, ring_id, do_applied, 0, 0);
    for (unsigned int i = 0; i < d->nrings; i++)
        if (d->ring[i].attached && !extrace_cons_mask_applied(d->ring[i].c))
            all = 0;
    return all;
}

int extrace_ctl_ring_cmd(struct extrace_disp *d, uint8_t ring_id, uint32_t cmd_bits)
{
    return for_rings(d, ring_id, do_cmd, cmd_bits, 0);
}

int extrace_ctl_ring_thr(struct extrace_disp *d, uint8_t ring_id, uint32_t publish_thr,
                         uint32_t notify_thr)
{
    return for_rings(d, ring_id, do_thr, publish_thr, notify_thr);
}

int extrace_ctl_sink_enable(struct extrace_disp *d, struct extrace_sink *s, bool on)
{
    (void)d;
    if (on)
        s->flags &= (uint8_t)~EXTRACE_SINK_F_DISABLED;
    else
        s->flags |= EXTRACE_SINK_F_DISABLED;
    return 0;
}

int extrace_ctl_sink_filter(struct extrace_disp *d, struct extrace_sink *s, uint8_t ring_id,
                            uint8_t kind_mask, const struct extrace_ev_map *ev)
{
    int slot = slot_of(d, ring_id);

    if (ring_id == EXTRACE_RING_ALL)
        return -EXTRACE_EINVAL;
    if (slot < 0)
        return -EXTRACE_ENOTREADY;
    s->kind_mask = kind_mask;
    s->ev_filter[slot] = ev ? *ev : (struct extrace_ev_map){ 0 };
    extrace_sink_ring_set(s, ring_id, true);
    return 0;
}

int extrace_ctl_ring_info(struct extrace_disp *d, uint8_t ring_id, struct extrace_ring_info *out)
{
    int slot = slot_of(d, ring_id);
    const struct extrace_cons *c;
    struct extrace_cons_diag dg;

    *out = (struct extrace_ring_info){ .ring_id = ring_id };
    if (ring_id == EXTRACE_RING_ALL)
        return -EXTRACE_EINVAL;
    if (slot < 0)
        return -EXTRACE_ENOTREADY;
    c = d->ring[slot].c;
    if (extrace_cons_diag(c, &dg))
        return -EXTRACE_ENOTREADY;
    out->attached = 1;
    out->ev_count = c->ev_count;
    out->epoch = c->epoch;
    out->mask_gen = c->mask_gen;
    out->mask_applied = extrace_cons_mask_applied(c);
    out->lost_total = dg.lost_total;
    out->hwm = dg.hwm;
    out->status = dg.status;
    out->pinfo = c->pinfo;
    return 0;
}

int extrace_ctl_stats(struct extrace_disp *d, struct extrace_disp_stats *out)
{
    *out = d->stats;
    return 0;
}
