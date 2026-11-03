// SPDX-License-Identifier: MIT
/*
 * ex_host.c - ring, viewer and record printer shared by the examples.
 *
 * The viewer is an ordinary extrace consumer: a dispatcher with one record sink.
 * The sink formats records the way a host tool does, from the image metadata:
 * event names and INTS field names from the catalog descriptors, PRINTF strings
 * from the call-site descriptors (format, argument classes).
 */
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ex_host.h"
#include "extrace_meta.h"
#include "extrace_rec.h"

#define EX_BLOCK_SIZE EXTRACE_RING_BLOCK_SIZE(EX_RING_DATA, EXTRACE_MASK_WORDS)

uint64_t ex_ring_block[(EX_BLOCK_SIZE + 7u) / 8u];
const uint32_t ex_ring_block_size = sizeof(ex_ring_block);

const struct extrace_prod_cfg ex_prod_cfg = {
    .block = ex_ring_block,
    .block_size = sizeof(ex_ring_block),
    .ring_id = EX_RING_ID,
    .name = "app",
};

struct extrace_cons ex_cons;
struct extrace_disp ex_disp;
static uint32_t ex_mirror[EXTRACE_CONS_MIRROR_WORDS(EXTRACE_CFG_MAX_EVENTS)];

/* The catalog descriptors (EXTRACE_META_RODATA keeps them in memory). */
extern const struct extrace_meta_ev __start_extrace_meta_ev[];

/* Keeps the site section, and its __start_ symbol, in an image without PRINTF. */
static const struct extrace_site ex_site_anchor __used __section("extrace_meta_site") = {
    .fmt = "",
    .file = "",
};

/* ---- deterministic clock: 1 MHz, every read advances it by 5 us ---- */

static uint32_t ex_clock = 1000;

static uint32_t ex_ts(void)
{
    return ex_clock += 5;
}

/* ---- symbols and struct printers registered by the examples ---- */

static struct {
    uint32_t addr;
    const char *name;
} ex_syms[16];
static unsigned int ex_nsyms;

void ex_symbol(const void *fn, const char *name)
{
    if (ex_nsyms < 16)
        ex_syms[ex_nsyms++] = (typeof(ex_syms[0])){ (uint32_t)(uintptr_t)fn, name };
}

static struct {
    uint16_t event;
    ex_struct_fmt fn;
} ex_sfmt[8];
static unsigned int ex_nsfmt;

void ex_struct_printer(uint16_t event, ex_struct_fmt fn)
{
    if (ex_nsfmt < 8)
        ex_sfmt[ex_nsfmt++] = (typeof(ex_sfmt[0])){ event, fn };
}

/* ---- metadata lookups ---- */

static const struct extrace_meta_ev *ev_meta(uint16_t id)
{
    for (const struct extrace_meta_ev *e = __start_extrace_meta_ev; e->name; e++)
        if (e->id == id)
            return e;
    return nullptr;
}

static const char *ev_name(uint16_t id)
{
    static const char *const sys[] = { "PAD", "SYNC", "LOST", "INFO", "FTRACE", "CFG",
                                       "PANIC", "FRAG", "FTRACE_CALLER", "SKIP" };
    const struct extrace_meta_ev *e;

    if (id < EXTRACE_SYS_FIRST_USER)
        return id < 10 ? sys[id] : "SYS?";
    e = ev_meta(id);
    return e ? e->name : "?";
}

static const char *kind_name(uint8_t k)
{
    static const char *const names[] = { "NONE", "MARK", "INTS", "PRINTF",
                                         "BUF",  "STRUCT", "STR", "EXT" };
    return k < 8 ? names[k] : "?";
}

/* "a:u b:x" -> the name and display hint of field i */
static bool spec_field(const char *spec, unsigned int i, char *name, size_t cap, char *hint)
{
    const char *p = spec;

    while (p && *p) {
        const char *end = strchr(p, ' ');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *colon = memchr(p, ':', n);

        if (i-- == 0) {
            size_t nl = colon ? (size_t)(colon - p) : n;

            snprintf(name, cap, "%.*s", (int)nl, p);
            *hint = colon && colon + 1 < p + n ? colon[1] : 'u';
            return true;
        }
        p = end ? end + 1 : nullptr;
    }
    return false;
}

/* The function that contains addr: the nearest symbol at or below it. */
static const char *sym_name(uint32_t addr, char *buf, size_t cap)
{
    unsigned int best = ex_nsyms;

    for (unsigned int i = 0; i < ex_nsyms; i++)
        if (addr >= ex_syms[i].addr && addr - ex_syms[i].addr < 4096u &&
            (best == ex_nsyms || ex_syms[i].addr > ex_syms[best].addr))
            best = i;
    if (best < ex_nsyms)
        snprintf(buf, cap, "%s", ex_syms[best].name);
    else
        snprintf(buf, cap, "0x%08" PRIx32, addr);
    return buf;
}

/* ---- varint reader for PRINTF and FRAG payloads ---- */

struct rd {
    const uint8_t *p, *end;
    bool bad;
};

static uint64_t rd_uv(struct rd *r)
{
    uint64_t v = 0;

    for (unsigned int s = 0; s < 70; s += 7) {
        if (r->p >= r->end) {
            r->bad = true;
            return 0;
        }
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7Fu) << s;
        if (!(b & 0x80u))
            return v;
    }
    r->bad = true;
    return v;
}

static int64_t rd_sv(struct rd *r)
{
    uint64_t u = rd_uv(r);

    return (int64_t)((u >> 1) ^ (0 - (u & 1u)));
}

static uint64_t rd_le(struct rd *r, unsigned int n)
{
    uint64_t v = 0;

    if (r->end - r->p < (ptrdiff_t)n) {
        r->bad = true;
        return 0;
    }
    for (unsigned int i = 0; i < n; i++)
        v |= (uint64_t)r->p[i] << (8 * i);
    r->p += n;
    return v;
}

/* ---- deferred printf: the host side of EXTRACE_PRINTF ---- */

struct out {
    char *buf;
    size_t cap, len;
};

static void out_add(struct out *o, const char *fmt, ...) __printf(2, 3);
static void out_add(struct out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->len >= o->cap)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        o->len = o->len + (size_t)n < o->cap ? o->len + (size_t)n : o->cap - 1;
}

/*
 * Formats a PRINTF record: the site descriptor gives the format string and the
 * class of every argument, which says how to read it from the payload.
 */
static void format_printf(const struct extrace_site *site, struct rd *r, struct out *o)
{
    const char *f = site->fmt;
    unsigned int i = 0;

    while (*f) {
        char spec[32] = "%", conv;
        size_t sl = 1;

        if (*f != '%' || f[1] == '%') {
            out_add(o, "%c", *f);
            f += *f == '%' ? 2 : 1;
            continue;
        }
        f++;
        while (*f && strchr("-+ #0123456789.", *f) && sl < 20)
            spec[sl++] = *f++;
        while (*f && strchr("hljztL", *f))      /* the class decides the width */
            f++;
        conv = *f ? *f++ : 'd';

        unsigned int cls = i < site->nargs
                               ? ((i < 8 ? site->sig_lo >> (4 * i) : site->sig_hi >> (4 * (i - 8))) & 15u)
                               : EXTRACE_AC_NONE;
        i++;
        switch (cls) {
        case EXTRACE_AC_U32:
        case EXTRACE_AC_U64:
        case EXTRACE_AC_S32:
        case EXTRACE_AC_S64: {
            bool sgn = cls == EXTRACE_AC_S32 || cls == EXTRACE_AC_S64;
            uint64_t v = sgn ? (uint64_t)rd_sv(r) : rd_uv(r);

            if (conv == 'c') {
                strcpy(spec + sl, "c");
                out_add(o, spec, (int)v);
            } else {
                spec[sl] = 'l', spec[sl + 1] = 'l', spec[sl + 2] = conv, spec[sl + 3] = 0;
                if (sgn && (conv == 'd' || conv == 'i'))
                    out_add(o, spec, (long long)v);
                else
                    out_add(o, spec, (unsigned long long)v);
            }
            break;
        }
        case EXTRACE_AC_F32:
        case EXTRACE_AC_F64: {
            double d;

            if (cls == EXTRACE_AC_F32) {
                uint32_t u = (uint32_t)rd_le(r, 4);
                float fl;

                memcpy(&fl, &u, sizeof(fl));
                d = fl;
            } else {
                uint64_t u = rd_le(r, 8);

                memcpy(&d, &u, sizeof(d));
            }
            spec[sl] = conv, spec[sl + 1] = 0;
            out_add(o, spec, d);
            break;
        }
        case EXTRACE_AC_STR: {                  /* the bytes were copied into the record */
            uint64_t n = rd_uv(r);
            char s[EXTRACE_CFG_STR_ARG_MAX + 1];

            if (n > EXTRACE_CFG_STR_ARG_MAX || r->end - r->p < (ptrdiff_t)n) {
                r->bad = true;
                return;
            }
            memcpy(s, r->p, n);
            s[n] = 0;
            r->p += n;
            strcpy(spec + sl, "s");
            out_add(o, spec, s);
            break;
        }
        case EXTRACE_AC_CSTR: {                 /* an address in .rodata: the host reads the ELF */
            uint64_t a = rd_uv(r);

            strcpy(spec + sl, "s");
            out_add(o, spec, (const char *)(uintptr_t)a);
            break;
        }
        case EXTRACE_AC_PTR:
            out_add(o, "0x%" PRIx64, rd_uv(r));
            break;
        default:
            out_add(o, "<?>");
            break;
        }
        if (r->bad)
            return;
    }
}

/* ---- the record sink ---- */

static void emit_line(const char *ts, const char *name, const char *kind, const char *text)
{
    char l[640];
    size_t n = (size_t)snprintf(l, sizeof(l), "  %s  %-13s %-6s  %s", ts, name, kind, text);

    if (n >= sizeof(l))
        n = sizeof(l) - 1;
    while (n && l[n - 1] == ' ')
        l[--n] = 0;
    puts(l);
}

static uint64_t ex_t0;
static bool ex_t0_set;
static bool ex_quiet;
static uint32_t ex_frag_got;

static void hexdump(struct out *o, const uint8_t *p, uint32_t n, uint32_t max)
{
    for (uint32_t i = 0; i < n && i < max; i++)
        out_add(o, "%s%02x", i ? " " : "", p[i]);
    if (n > max)
        out_add(o, " ...");
}

static void print_ints(const struct extrace_rec *r, struct out *o)
{
    const struct extrace_meta_ev *e = ev_meta(r->event);
    uint64_t v[16];
    uint32_t sm;
    int n = extrace_rec_ints(r, v, 16, &sm);

    for (int i = 0; i < n; i++) {
        char name[24], hint = 'u';
        bool named = e && e->spec && spec_field(e->spec, (unsigned int)i, name, sizeof(name), &hint);

        if (named)
            out_add(o, "%s%s=", i ? " " : "", name);
        else
            out_add(o, "%s", i ? " " : "");
        if (hint == 'x')
            out_add(o, "0x%" PRIx64, v[i]);
        else if ((sm >> i) & 1u)
            out_add(o, "%" PRId64, (int64_t)v[i]);
        else
            out_add(o, "%" PRIu64, v[i]);
    }
}

static void print_sys(const struct extrace_rec *r, struct out *o)
{
    uint64_t v[16];
    uint32_t addr, caller, args[16];
    char s1[48], s2[48];
    int n;

    switch (r->event) {
    case EXTRACE_SYS_LOST:
        extrace_rec_ints(r, v, 16, nullptr);
        out_add(o, "%" PRIu64 " records dropped (ring full)", v[0]);
        break;
    case EXTRACE_SYS_CFG:
        extrace_rec_ints(r, v, 16, nullptr);
        out_add(o, "event mask generation %" PRIu64 " applied", v[0]);
        break;
    case EXTRACE_SYS_PANIC:
        extrace_rec_ints(r, v, 16, nullptr);
        out_add(o, "reason=0x%" PRIx64 " pc=0x%" PRIx64, v[0], v[1]);
        break;
    case EXTRACE_SYS_SYNC:
        out_add(o, "time anchor");
        break;
    case EXTRACE_SYS_INFO:
        out_add(o, "image description, %" PRIu32 " bytes", r->len);
        break;
    case EXTRACE_SYS_FTRACE:
    case EXTRACE_SYS_FTRACE_CALLER:
        n = extrace_rec_ftrace(r, &addr, &caller, args, 16);
        out_add(o, "%s", sym_name(addr, s1, sizeof(s1)));
        if (r->event == EXTRACE_SYS_FTRACE_CALLER)
            out_add(o, " from %s", sym_name(caller, s2, sizeof(s2)));
        for (int i = 0; i < n; i++)
            out_add(o, "%s0x%" PRIx32, i ? ", " : " (", args[i]);
        if (n > 0)
            out_add(o, ")");
        break;
    case EXTRACE_SYS_FRAG: {
        struct rd rd = { r->payload, r->payload + r->len, false };
        uint64_t type = rd_uv(&rd), xfer = rd_uv(&rd), id = rd_uv(&rd);
        uint64_t total = rd_uv(&rd), off = rd_uv(&rd);
        uint32_t len = (uint32_t)(rd.end - rd.p);

        (void)type;
        ex_frag_got = off == 0 ? len : ex_frag_got + len;
        out_add(o, "%s xfer %" PRIu64 ": bytes %" PRIu64 "..%" PRIu64 " of %" PRIu64,
                ev_name((uint16_t)id), xfer, off, off + len - 1, total);
        if (ex_frag_got == total)
            out_add(o, ", complete");
        break;
    }
    default:
        hexdump(o, r->payload, r->len, 16);
        break;
    }
}

static void on_record(struct extrace_sink *s, const struct extrace_rec *r)
{
    char line[512] = "", ts[24];
    struct out o = { line, sizeof(line), 0 };

    (void)s;
    if (!ex_t0_set && (r->flags & EXTRACE_REC_HAS_TS)) {
        ex_t0 = r->ts;
        ex_t0_set = true;
    }
    if (r->kind == EXTRACE_K_MARK || !(r->flags & EXTRACE_REC_HAS_TS))
        snprintf(ts, sizeof(ts), "%10s", "-");     /* MARK records carry no time */
    else
        snprintf(ts, sizeof(ts), "%7" PRIu64 " us", r->ts - ex_t0);

    if (ex_quiet && r->event != EXTRACE_SYS_LOST && r->event != EXTRACE_SYS_PANIC)
        return;
    if (r->event < EXTRACE_SYS_FIRST_USER) {
        print_sys(r, &o);
        emit_line(ts, ev_name(r->event),
                  r->event == EXTRACE_SYS_FTRACE || r->event == EXTRACE_SYS_FTRACE_CALLER
                      ? kind_name(r->kind) : "sys", line);
        return;
    }
    switch (r->kind) {
    case EXTRACE_K_INTS:
        print_ints(r, &o);
        break;
    case EXTRACE_K_PRINTF: {
        uint32_t site_id, alen;
        const uint8_t *args;

        if (extrace_rec_printf_site(r, &site_id, &args, &alen) == 0) {
            const struct extrace_site *site = (const struct extrace_site *)(
                EXTRACE_META_SITE_BASE + ((uintptr_t)site_id << EXTRACE_SITE_SHIFT));
            struct rd rd = { args, args + alen, false };
            const char *file = strrchr(site->file, '/');

            out_add(&o, "\"");
            format_printf(site, &rd, &o);
            out_add(&o, "\"  @%s:%" PRIu32, file ? file + 1 : site->file, site->line);
        }
        break;
    }
    case EXTRACE_K_STR:
        out_add(&o, "\"%.*s\"", (int)r->len, (const char *)r->payload);
        break;
    case EXTRACE_K_STRUCT:
        for (unsigned int i = 0; i < ex_nsfmt; i++)
            if (ex_sfmt[i].event == r->event) {
                ex_sfmt[i].fn(line, sizeof(line), r->payload, r->len);
                goto print;
            }
        [[fallthrough]];
    case EXTRACE_K_BUF:
        out_add(&o, "[%" PRIu32 " bytes] ", r->len);
        hexdump(&o, r->payload, r->len, 12);
        break;
    default:
        break;
    }
print:
    emit_line(ts, ev_name(r->event), kind_name(r->kind), line);
}

static void on_ring_event(struct extrace_sink *s, uint8_t ring_id, enum extrace_ring_evt e,
                          const struct extrace_ring_evt_info *info)
{
    (void)s;
    if (e == EXTRACE_RING_ATTACHED)
        printf("  == ring %u \"%.8s\" attached, epoch %" PRIu32 "\n", ring_id,
               info->pi->name, info->epoch);
    else if (e == EXTRACE_RING_DETACHED)
        printf("  == ring %u detached from epoch %" PRIu32 " (producer re-initialized)\n",
               ring_id, info->epoch);
}

static const struct extrace_sink_ops ex_sink_ops = {
    .record = on_record,
    .ring_event = on_ring_event,
};
static struct extrace_sink ex_sink = {
    .ops = &ex_sink_ops,
    .cls = EXTRACE_SINK_RECORD,
    .kind_mask = 0xFF,
};

/* ---- example flow ---- */

void ex_start(const char *title)
{
    extrace_posix_ts_hook = ex_ts;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    printf("%s\n", title);

    extrace_disp_init(&ex_disp);
    extrace_sink_ring_all(&ex_sink);
    extrace_disp_add_sink(&ex_disp, &ex_sink);
    extrace_cons_attach(&ex_cons, ex_ring_block, ex_ring_block_size, ex_mirror,
                        EXTRACE_CONS_MIRROR_WORDS(EXTRACE_CFG_MAX_EVENTS), 0);
    extrace_disp_add_ring(&ex_disp, &ex_cons);

    if (extrace_prod_init(&ex_prod_cfg) != 0)
        printf("extrace_prod_init failed\n");
    extrace_prod_flush();                           /* the stream starts with INFO and SYNC */
    while (extrace_disp_poll(&ex_disp, 4096) > 0)
        ;
}

void ex_section(const char *title)
{
    printf("\n-- %s\n", title);
}

void ex_drain(void)
{
    uint64_t bytes = ex_cons.stats.bytes;
    uint32_t recs = ex_disp.stats.records;

    for (int idle = 0; idle < 2;)                   /* a re-attach takes a poll of its own */
        idle = extrace_disp_poll(&ex_disp, 4096) > 0 ? 0 : idle + 1;
    if (ex_disp.stats.records != recs)
        printf("     (%" PRIu32 " record%s, %" PRIu64 " bytes in the ring)\n",
               ex_disp.stats.records - recs, ex_disp.stats.records - recs == 1 ? "" : "s",
               ex_cons.stats.bytes - bytes);
    else
        printf("     (nothing published)\n");
}

void ex_quiet_records(bool on)
{
    ex_quiet = on;
}

void ex_show(void)
{
    extrace_prod_flush();
    ex_drain();
}

int ex_finish(void)
{
    struct extrace_prod_stats st;

    extrace_prod_stats(&st);
    printf("\nproducer: %" PRIu32 " records, %" PRIu32 " lost, high-water mark %" PRIu32
           " of %u bytes\n",
           st.records, st.lost_total, st.hwm, EX_RING_DATA);
    return 0;
}

void ex_raise_irq(void (*fn)(void))
{
    extrace_posix_set_isr(SIGUSR1, fn);
    raise(SIGUSR1);
    extrace_posix_set_isr(SIGUSR1, nullptr);
}
