# Consumer guide

The consumer reads one or more producer rings, usually on another core: a
supervisor MCU, an application processor, or a host behind a debugger. This
guide covers how to integrate it and the API it exposes.
[DESIGN.md](../DESIGN.md) has the full specification. Section numbers (§) below
refer to it.

## Building blocks

| Component | Header | Role |
|---|---|---|
| `cons` | `extrace_cons.h` | One ring: attach, zero-copy peek/release, copy-mode read, event control, commands |
| `rec` | `extrace_rec.h` | Record decoder with a per-ring 64-bit timeline |
| `disp` | `extrace_disp.h` | Dispatcher: polls many rings and feeds sinks (record or bulk), with filtering and back-pressure |
| `sink_mem`, `sink_stats`, `sink_text` | `extrace_sink_*.h` | Ready-made sinks: capture to RAM, per-event counters, one line per record to a UART |

Most integrations use the dispatcher with sinks. Use `cons` on its own when the
records only need to be moved somewhere else, such as a DMA stream to the
host.

The consumer never blocks the producer. It never allocates: all memory is
supplied by the caller.

## Integration

### 1. Port header

The consumer image uses the same port header as the producer side
([port/template/extrace_port.h](../port/template/extrace_port.h)). It needs:

* the barriers `extrace_port_wmb` / `_rmb` / `_mb`;
* `extrace_port_ts`, the same SoC-global timer, used for the dispatcher
  timeline.

Optionally it also provides:

* `extrace_port_notify_producer(ring_id)`, a doorbell telling a producer that
  its configuration changed;
* `extrace_port_memcpy`, which must copy aligned-only for Device memory.

Map the ring blocks non-cacheable.

### 2. Build

```cmake
extrace_add_image(supervisor
    PORT_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/port
    CFG_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/cfg
    COMPONENTS cons rec disp sink_mem sink_stats)
target_link_libraries(supervisor.elf PRIVATE supervisor::extrace)
```

The consumer does not need the producers' event catalogs. Event IDs, kinds and
the INFO data arrive at runtime.

Relevant options:

| Option | Default | Purpose |
|---|---|---|
| `EXTRACE_CFG_DISP_MAX_RINGS` | 16 | Rings per dispatcher |
| `EXTRACE_CFG_DISP_STAGE` | 1024 | Staging buffer for records that wrap the ring end. Rings with a larger `max_record` attach as `NODECODE` (bulk only) |
| `EXTRACE_CFG_MAX_EVENTS` | 1024 | Sizes event-control mirrors |

### 3. Attach and dispatch

```c
#define NRINGS 2
static struct extrace_cons cons[NRINGS];          /* zero-initialized */
static uint32_t mirror[NRINGS][EXTRACE_CONS_MIRROR_WORDS(EXTRACE_CFG_MAX_EVENTS)];
static struct extrace_disp disp;
static struct extrace_stats_sink stats;
static struct extrace_stats_sink_ring stats_rings[NRINGS];
static uint32_t stats_cnt[NRINGS * 256];

void trace_init(void)
{
    extrace_disp_init(&disp);
    extrace_stats_sink_init(&stats, stats_rings, stats_cnt, NRINGS, 256);
    extrace_disp_add_sink(&disp, &stats.sink);
    for (int i = 0; i < NRINGS; i++) {
        /* the ring block as mapped on this core; the producer may not be up yet */
        extrace_cons_attach(&cons[i], ring_block[i], ring_block_size[i],
                            mirror[i], EXTRACE_CONS_MIRROR_WORDS(EXTRACE_CFG_MAX_EVENTS), 0);
        extrace_disp_add_ring(&disp, &cons[i]);
    }
}

void trace_task(void)          /* on the doorbell or a timer */
{
    while (extrace_disp_poll(&disp, 4096) > 0)
        ;
}
```

`extrace_disp_poll(d, budget)` serves the rings round-robin, up to `budget`
bytes. It returns the number of bytes processed, so 0 means idle. Each poll
handles the ring lifecycle by itself:

* **Producer not up yet.** `-EXTRACE_ENOTREADY` is retried on every poll. Start
  the cores in any order.
* **Producer re-initialized** (new epoch). In-flight windows are dropped and the
  ring re-attaches. Sinks receive `EXTRACE_RING_DETACHED` with the old epoch and
  stream position, then `EXTRACE_RING_ATTACHED` with the new `pinfo`.
* **Consumer restarted** (same epoch). The ring resumes where the stored tail
  left off, and sinks receive `EXTRACE_RING_RESUMED`. A different image in the
  same block is detected and treated as a new epoch.
* **Version, layout or validation failure.** Sinks receive
  `EXTRACE_RING_DETACHED` if the ring was attached, then
  `EXTRACE_RING_ATTACH_FAILED`, once until the ring attaches again.
  `stats.attach_errors` counts every failed attempt. The ring is retried on later
  polls. A second ring with the same `ring_id` is reported the same way.

Doorbells: set a notify threshold with `extrace_cons_set_notify(c, bytes)`, or
both thresholds at once with `extrace_cons_set_thr(c, publish, notify)`.
With the default of 0, the producer rings only when it starts dropping records.

### 4. Sinks

```c
static void on_record(struct extrace_sink *s, const struct extrace_rec *r)
{
    /* r->ring_id, r->event, r->kind, r->ts (64-bit ticks), r->payload/len */
    if (r->kind == EXTRACE_K_INTS) {
        uint64_t v[16];
        int n = extrace_rec_ints(r, v, 16, nullptr);
        ...
    }
}

static const struct extrace_sink_ops my_ops = { .record = on_record };
static struct extrace_sink my_sink = {
    .ops = &my_ops, .cls = EXTRACE_SINK_RECORD, .kind_mask = 0xFF,
};

extrace_sink_ring_all(&my_sink);        /* or extrace_sink_ring_set(&s, id, true) */
extrace_disp_add_sink(&disp, &my_sink);
```

* **Record sinks** (`record`) receive decoded records. These include system
  records: `LOST` (drop count), `SYNC`, `INFO` and `PANIC`.
  * `r->payload` is valid only during the callback.
  * Filter with `kind_mask` and with per-ring `ev_filter` bitmaps
    (`extrace_ctl_sink_filter`).
* **Bulk sinks** (`window`) receive raw ring bytes for transport to the host.
  * Wrap them in 32-byte frame headers (`extrace_bulk_hdr_put`; `pos` and
    `epoch` let the host detect gaps).
  * Return `EXTRACE_SINK_DONE` when finished.
  * A sink with `EXTRACE_SINK_F_LOSSY` may return `BUSY`. That window is then
    lost to it, and `extrace_sink_take_gap` tells it to set `GAP` on its next
    frame.
  * A lossless sink applies back-pressure through `can_accept`, and the data
    stays in the ring.
* `ring_event` reports attach, resume and detach together with the validated
  `pinfo`. `flush` runs at the end of each poll.

All callbacks are synchronous. A window is released only after every sink has
returned, so a sink must copy anything it needs to keep.

### 5. Control

The `extrace_ctl_*` functions address a ring by `ring_id` through the
dispatcher. They can be wired to a host command channel:

| Call | Effect |
|---|---|
| `extrace_ctl_ev_set(d, ring, id, EXTRACE_EVS_ON / OFF / DEFAULT)` | Enable or disable one event. Also `_set_range`, `_set_map` and `_reset` |
| `extrace_ctl_ev_applied(d, ring)` | 1 once the producer has applied the last change (at its next poll) |
| `extrace_ctl_ring_cmd(d, ring, EXTRACE_CMD_FLUSH / SYNC / INFO)` | Ask the producer to publish, emit a `SYNC` anchor, or re-send `INFO` |
| `extrace_ctl_ring_thr(d, ring, publish_thr, notify_thr)` | Publication and doorbell thresholds in bytes |
| `extrace_ctl_ring_info(d, ring, &info)` | Epoch, event count, drops, high-water mark and status (`PANIC`) |
| `extrace_ctl_sink_enable` / `_sink_filter` / `extrace_ctl_stats` | Sink on/off, filters, dispatcher counters |

Changes take effect at the producer's next `extrace_prod_poll`. Provide
`extrace_port_notify_producer` to wake that poll immediately.

## Low-level ring API (`extrace_cons.h`)

Use these without the dispatcher, for example to stream ring bytes over DMA:

```c
struct extrace_window w;
int n = extrace_cons_peek(c, &w, max_bytes);   /* > 0 bytes, 0 empty, < 0 error */

if (n > 0) {
    send(w.span[0].p, w.span[0].len);          /* nspan == 2 when the data wraps */
    if (w.nspan == 2)
        send(w.span[1].p, w.span[1].len);
    if (extrace_cons_release(c, &w, (uint32_t)n) == -EXTRACE_EEPOCH)
        discard_sent_data();                   /* the producer restarted meanwhile */
}
```

* **`peek`** returns whole published bytes, never a partial record.
  `-EXTRACE_EEPOCH` means the producer restarted, and the consumer has already
  re-attached.
* **`release`** frees space for the producer. It re-checks the epoch: data
  copied out of a window that `release` reports as `-EXTRACE_EEPOCH` may be
  torn and must be discarded.
* **`extrace_cons_read(c, dst, size)`** is copy mode. It copies whole records
  only and validates the epoch.
* **`extrace_cons_poll_state(c)`** attaches or re-attaches. It returns 1 on a
  new attach, 0 if nothing changed, and < 0 if the ring is not attached.
* **Event and command calls** without the dispatcher: `extrace_cons_ev_set`,
  `extrace_cons_cmd`, `extrace_cons_set_thr`, `extrace_cons_set_publish_thr`.
* **`extrace_cons_diag(c, &out)`** reads the producer's diagnostics of the
  attached epoch: drops, high-water mark and status.

## Decoding (`extrace_rec.h`)

The record format is specified in [wire-format.md](wire-format.md).

```c
struct extrace_dec dec;
struct extrace_rec r;
const uint8_t *p = buf;

extrace_dec_reset(&dec, ring_id, pinfo_max_record);
dec.disp_ts = now64;                          /* places 32-bit stamps on a 64-bit timeline */
while (extrace_dec_next(&dec, &p, buf + len, &r) == 1)
    handle(&r);
```

`extrace_dec_next` returns:

* 1 for a record;
* 0 at the end (only padding was left);
* `-EXTRACE_ENOSPC` when a record is cut off at the end of the buffer;
* `-EXTRACE_ECORRUPT` for a malformed record.

Helpers:

* `extrace_rec_ints` returns the values with signedness restored.
* `extrace_rec_printf_site` returns the site ID and the raw argument bytes. The
  host formats the string with the ELF metadata.
* `extrace_rec_ftrace` returns the address field, the caller field and the
  argument registers of an `FTRACE` or `FTRACE_CALLER` record, whatever its shape
  ([ftrace.md](ftrace.md)). Turn function tracing on with
  `extrace_ctl_ev_set(d, ring, EXTRACE_SYS_FTRACE, EXTRACE_EVS_ON)`. What each
  function records, and whether with its caller, is its flavor, set when the
  platform patches it ([ftrace.md](ftrace.md#control)).

## Performance notes

* Sizing the budget: about half a ring per ring per poll keeps the rings
  fair. Call `poll` until it returns 0 after each doorbell.
* Record sinks cost a decode per record. A bulk sink alone skips decoding
  completely (zero-copy).
* Lossy bulk sinks never stall the producer. Lossless sinks hold data in the
  ring, so the producer drops new records (and counts them) once the ring
  fills.
