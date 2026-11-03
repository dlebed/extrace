// SPDX-License-Identifier: MIT
/* extrace_sink_text.c - one text line per record through a write hook (DESIGN §12.8). */
#include "extrace_sink_text.h"

struct line {
    char b[EXTRACE_TEXT_LINE_MAX];
    uint32_t n;
};

static void put_c(struct line *l, char c)
{
    if (l->n < sizeof(l->b))
        l->b[l->n++] = c;
}

static void put_s(struct line *l, const char *s)
{
    while (*s)
        put_c(l, *s++);
}

static void put_u(struct line *l, uint64_t v)
{
    char d[20];
    int n = 0;

    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        put_c(l, d[--n]);
}

static void put_hex8(struct line *l, uint8_t v)
{
    static const char hex[] = "0123456789abcdef";

    put_c(l, hex[v >> 4]);
    put_c(l, hex[v & 15u]);
}

static const char *const kind_name[8] = {
    "NONE", "MARK", "INTS", "PRINTF", "BUF", "STRUCT", "STR", "EXT",
};

static void format(struct line *l, const struct extrace_rec *r)
{
    put_c(l, '[');
    put_u(l, r->ring_id);
    put_s(l, "] t=");
    if (r->flags & EXTRACE_REC_HAS_TS)
        put_u(l, r->ts);
    else
        put_c(l, '?');
    put_c(l, ' ');
    put_u(l, r->event);
    put_c(l, ' ');
    put_s(l, kind_name[r->kind & 7u]);
    if (r->event == EXTRACE_SYS_FTRACE || r->event == EXTRACE_SYS_FTRACE_CALLER) {
        uint32_t a, c, v[16];
        int n = extrace_rec_ftrace(r, &a, &c, v, 16);

        put_s(l, " fn 0x");
        for (int i = 24; i >= 0; i -= 8)
            put_hex8(l, (uint8_t)(a >> i));
        if (r->event == EXTRACE_SYS_FTRACE_CALLER) {
            put_s(l, " from 0x");
            for (int i = 24; i >= 0; i -= 8)
                put_hex8(l, (uint8_t)(c >> i));
        }
        for (int i = 0; i < n; i++) {
            put_c(l, ' ');
            put_u(l, v[i]);
        }
    } else if (r->kind == EXTRACE_K_INTS) {
        uint64_t v[16];
        uint32_t sm;
        int n = extrace_rec_ints(r, v, 16, &sm);

        for (int i = 0; i < n && i < 16; i++) {
            put_c(l, ' ');
            if (((sm >> i) & 1u) && (int64_t)v[i] < 0) {
                put_c(l, '-');
                put_u(l, 0u - v[i]);
            } else {
                put_u(l, v[i]);
            }
        }
    } else if (r->kind == EXTRACE_K_STR) {
        put_s(l, " \"");
        for (uint32_t i = 0; i < r->len; i++) {
            char c = (char)r->payload[i];

            put_c(l, (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        put_c(l, '"');
    } else if (r->len) {
        uint32_t n = r->len < 16u ? r->len : 16u;
        const uint8_t *p = r->payload;

        if (r->kind == EXTRACE_K_PRINTF) {
            uint32_t site, alen;

            if (extrace_rec_printf_site(r, &site, &p, &alen) == 0) {
                put_s(l, " site=");
                put_u(l, site);
                n = alen < 16u ? alen : 16u;
            }
        }
        put_c(l, ' ');
        for (uint32_t i = 0; i < n; i++)
            put_hex8(l, p[i]);
        if (n < (uint32_t)(r->payload + r->len - p))
            put_s(l, "...");
    }
    put_c(l, '\n');
}

/* Push queued bytes to the hook, as far as it takes them. */
static void drain(struct extrace_text_sink *t)
{
    while (t->head != t->tail) {
        uint32_t off = t->tail & (t->size - 1u), n = t->head - t->tail, k;

        if (n > t->size - off)
            n = t->size - off;
        k = t->write(t->ctx, t->fifo + off, n);
        t->tail += k;
        if (k < n)
            return;
    }
}

static void text_record(struct extrace_sink *s, const struct extrace_rec *r)
{
    struct extrace_text_sink *t = (struct extrace_text_sink *)s;
    struct line l = { .n = 0 };

    format(&l, r);
    drain(t);
    if (t->size - (t->head - t->tail) < l.n) {
        t->dropped++; /* lossy: never blocks the dispatcher */
        s->stats.gaps++;
        return;
    }
    for (uint32_t i = 0; i < l.n; i++)
        t->fifo[(t->head + i) & (t->size - 1u)] = l.b[i];
    t->head += l.n;
}

static void text_flush(struct extrace_sink *s)
{
    drain((struct extrace_text_sink *)s);
}

static const struct extrace_sink_ops text_ops = { .record = text_record, .flush = text_flush };

int extrace_text_sink_init(struct extrace_text_sink *t, char *fifo, uint32_t fifo_size,
                           extrace_text_write_fn write, void *ctx)
{
    if (!t || !fifo || !write || !extrace_is_pow2(fifo_size) ||
        fifo_size < EXTRACE_TEXT_LINE_MAX)
        return -EXTRACE_EINVAL;
    extrace_port_memset(t, 0, sizeof(*t));
    t->sink.ops = &text_ops;
    t->sink.cls = EXTRACE_SINK_RECORD;
    t->sink.flags = EXTRACE_SINK_F_LOSSY;
    t->sink.kind_mask = 0xFF;
    extrace_sink_ring_all(&t->sink);
    t->write = write;
    t->ctx = ctx;
    t->fifo = fifo;
    t->size = fifo_size;
    return 0;
}
