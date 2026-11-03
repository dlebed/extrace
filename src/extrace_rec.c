// SPDX-License-Identifier: MIT
/* extrace_rec.c - record decoder (DESIGN §5, §12.5). */
#include "extrace_rec.h"
#include "extrace_wire.h"

void extrace_dec_reset(struct extrace_dec *d, uint8_t ring_id, uint32_t max_record)
{
    *d = (struct extrace_dec){ .ring_id = ring_id, .max_record = max_record };
}

/* 1: value, 0: runs past end (truncated), -1: malformed. */
static int get_uv(const uint8_t **q, const uint8_t *end, uint32_t *v)
{
    unsigned int n = extrace_get_uv32(*q, end, v);

    if (n) {
        *q += n;
        return 1;
    }
    /* cut off: every byte up to end continues, and fewer than 5 of them */
    if (end - *q >= 5)
        return -1;
    for (const uint8_t *b = *q; b < end; b++)
        if (!(*b & 0x80u))
            return -1;
    return 0;
}

static int anchor_ts(const uint8_t *pl, uint32_t len, bool lost, uint32_t *s)
{
    const uint8_t *q = pl, *end = pl + len;
    uint32_t sm, v;

    if (get_uv(&q, end, &sm) != 1 || sm != 0)
        return -1;
    if (lost && get_uv(&q, end, &v) != 1) /* count */
        return -1;
    if (get_uv(&q, end, s) != 1)
        return -1;
    return 0;
}

int extrace_dec_next(struct extrace_dec *d, const uint8_t **p, const uint8_t *end,
                     struct extrace_rec *out)
{
    const uint8_t *q = *p, *start;
    uint32_t h, tsd, len, id, kind, s;
    int r;

next:
    h = tsd = len = 0;
    for (;;) { /* skip any run of PAD1 and PADN (§5.8) */
        uint32_t n;

        if (q >= end) {
            *p = q;
            return 0;
        }
        if (*q == EXTRACE_HDR_PAD1) {
            q++;
            d->pad_bytes++;
            continue;
        }
        if (*q != EXTRACE_HDR_PADN)
            break;
        start = q++;
        r = get_uv(&q, end, &n);
        if (r <= 0) {
            *p = start;
            if (r == 0)
                return -EXTRACE_ENOSPC;
            d->errors++;
            return -EXTRACE_ECORRUPT;
        }
        if (n > (uint32_t)(end - q)) {
            d->pad_skip = n - (uint32_t)(end - q);
            d->pad_bytes += (uint32_t)(end - start);
            *p = end;
            return 0;
        }
        q += n;
        d->pad_bytes += (uint32_t)(q - start);
    }
    start = q;
    r = get_uv(&q, end, &h);
    kind = h & 7u;
    id = h >> 3;
    if (r == 1 && (id == 0 || id > 0xFFFFu))
        r = -1; /* ID 0 is valid only as PAD1 and PADN; IDs are 16-bit in every kind */
    if (r == 1 && kind != EXTRACE_K_MARK)
        r = get_uv(&q, end, &tsd);
    if (r == 1 && extrace_kind_has_len(kind)) {
        r = get_uv(&q, end, &len);
        if (r == 1 && len > 65535u)
            r = -1;
    } else if (r == 1 && extrace_hdr_ftrace_short(h)) { /* payload: 1 or 2 uvarints */
        const uint8_t *a = q;

        for (unsigned int i = extrace_hdr_ftrace_short(h); r == 1 && i; i--)
            r = get_uv(&q, end, &s);
        len = (uint32_t)(q - a);
        q = a;
    }
    if (r == 1 && d->max_record && (uint32_t)(q - start) + len > d->max_record)
        r = -1;
    if (r == 1 && len > (uint32_t)(end - q))
        r = 0;
    if (r <= 0) {
        *p = start;
        if (r == 0)
            return -EXTRACE_ENOSPC;
        d->errors++;
        return -EXTRACE_ECORRUPT;
    }
    if (h == EXTRACE_HDR_SKIP) { /* a cancelled reservation: its time only (§8.8.1) */
        d->ts += tsd;
        q += len;
        d->pad_bytes += (uint32_t)(q - start);
        goto next;
    }
    out->ring_id = d->ring_id;
    out->kind = (uint8_t)kind;
    out->event = (uint16_t)id;
    out->payload = q;
    out->len = len;
    out->flags = 0;
    if (kind == EXTRACE_K_INTS && (id == EXTRACE_SYS_SYNC || id == EXTRACE_SYS_LOST)) {
        if (anchor_ts(q, len, id == EXTRACE_SYS_LOST, &s)) {
            *p = start;
            d->errors++;
            return -EXTRACE_ECORRUPT;
        }
        d->ts = extrace_rebase(d->disp_ts, s);
        d->anchored = 1;
    } else if (kind != EXTRACE_K_MARK) {
        d->ts += tsd;
    }
    if (d->after_lost && id != EXTRACE_SYS_LOST) {
        out->flags |= EXTRACE_REC_AFTER_LOST;
        d->after_lost = 0;
    }
    if (id == EXTRACE_SYS_LOST)
        d->after_lost = 1;
    if (d->anchored)
        out->flags |= EXTRACE_REC_HAS_TS;
    out->ts = d->ts;
    d->records++;
    *p = q + len;
    return 1;
}

int extrace_rec_ints(const struct extrace_rec *r, uint64_t *out, unsigned int max,
                     uint32_t *sign_mask)
{
    const uint8_t *q = r->payload, *end = r->payload + r->len;
    unsigned int n = 0, k;
    uint32_t sm;

    if (r->kind != EXTRACE_K_INTS || get_uv(&q, end, &sm) != 1)
        return -EXTRACE_ECORRUPT;
    while (q < end) {
        uint64_t v;

        k = extrace_get_uv64(q, end, &v);
        if (!k || n >= 16)
            return -EXTRACE_ECORRUPT;
        if ((sm >> n) & 1u)
            v = (uint64_t)extrace_unzz64(v);
        if (n < max)
            out[n] = v;
        n++;
        q += k;
    }
    if (sign_mask)
        *sign_mask = sm;
    return (int)n;
}

int extrace_rec_printf_site(const struct extrace_rec *r, uint32_t *site, const uint8_t **args,
                            uint32_t *args_len)
{
    const uint8_t *q = r->payload, *end = r->payload + r->len;

    if (r->kind != EXTRACE_K_PRINTF || get_uv(&q, end, site) != 1)
        return -EXTRACE_ECORRUPT;
    *args = q;
    *args_len = (uint32_t)(end - q);
    return 0;
}

int extrace_rec_ftrace(const struct extrace_rec *r, uint32_t *addr, uint32_t *caller,
                       uint32_t *args, unsigned int max)
{
    const uint8_t *q = r->payload, *end = r->payload + r->len;
    uint64_t v[16];
    uint32_t sm, c = 0;
    int n, lead;

    if (r->event != EXTRACE_SYS_FTRACE && r->event != EXTRACE_SYS_FTRACE_CALLER)
        return -EXTRACE_ECORRUPT;
    lead = r->event == EXTRACE_SYS_FTRACE_CALLER ? 2 : 1;
    if (r->kind == EXTRACE_K_NONE || r->kind == EXTRACE_K_MARK) {
        if (get_uv(&q, end, addr) != 1 || (lead == 2 && get_uv(&q, end, &c) != 1) || q != end)
            return -EXTRACE_ECORRUPT;
        if (caller)
            *caller = c;
        return 0;
    }
    n = extrace_rec_ints(r, v, 16, &sm);
    if (n < lead || sm)
        return -EXTRACE_ECORRUPT;
    for (int i = 0; i < n; i++)
        if (v[i] > 0xFFFFFFFFu)
            return -EXTRACE_ECORRUPT;
    *addr = (uint32_t)v[0];
    if (caller)
        *caller = lead == 2 ? (uint32_t)v[1] : 0u;
    for (int i = lead; i < n && (unsigned int)(i - lead) < max; i++)
        args[i - lead] = (uint32_t)v[i];
    return n - lead;
}
