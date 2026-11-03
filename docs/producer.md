# Producer guide

The producer is the traced image: firmware on a core that writes trace records
into a shared-memory ring. This guide covers how to integrate it and the API it
exposes. [wire-format.md](wire-format.md) shows the bytes each macro writes. [DESIGN.md](../DESIGN.md) has the full specification. Section numbers
(§) below refer to it.

## How it works

* An `EXTRACE_*` call site costs one load, a bit test and a branch while its
  event is disabled. Arguments are evaluated only when the event is enabled.
* An enabled call goes to a shared, out-of-line emitter. The emitter reserves
  space inside a short interrupt-masked section, writes the record, and commits
  it. Any context may trace, including nested interrupts.
* Committed records are published to the consumer in batches (`publish_thr`),
  and optionally announced through a doorbell.
* When the ring is full, records are dropped, never blocked. The drops are
  counted and reported in-band as a `LOST` record.

## Integration

### 1. Port header

Copy [port/template/extrace_port.h](../port/template/extrace_port.h) next to
your image and fill it in. Every hook is `static inline`.

| Hook | Required | Purpose |
|---|---|---|
| `extrace_port_ts()`, `extrace_port_ts_freq()` | yes | SoC-global free-running 32-bit timer, the same counter on every core |
| `extrace_port_irq_save()` / `_restore()` | yes | Mask every interrupt that may trace; restore the previous state |
| `extrace_port_wmb()` / `_rmb()` / `_mb()` | yes | Barriers as seen by the other agents (ARM `dmb`; RISC-V `fence w,w` / `r,r` / `rw,rw`) |
| `extrace_port_notify(ring_id)` | optional | Doorbell to the consumer. Starts with a completion barrier (`dsb` / `fence w,o`) |
| `extrace_port_in_isr()` | with `PUBLISH_IN_ISR=0` | Interrupt-context query |
| `extrace_port_build_id()` | recommended | GNU build ID, sent in the `INFO` record |
| `extrace_port_vsnprintf()` | for `EXTRACE_TEXT` / `PRINTF_LOCAL` | Reentrant formatter |
| `EXTRACE_PORT_HOT_TEXT` / `_HOT_DATA` | optional | Section attributes that place the emitters and the producer state in TCM |

Map the trace memory non-cacheable on every agent.

### 2. Event catalog: `extrace_events.def`

```c
/*            group  compiled-in */
EXTRACE_GROUP(CORE,  1)
EXTRACE_GROUP(DEBUG, 0)          /* compiled out; -DEXTRACE_GRP_DEBUG_ON=1 turns it on */

/*         name       kind    default group  spec */
EXTRACE_EV(RX_PKT,    INTS,   ON,     CORE,  "len:u port:u")
EXTRACE_EV(LOG,       PRINTF, ON,     CORE,  "")
EXTRACE_EV(DUMP,      BUF,    OFF,    DEBUG, "")

/*                name    default group C type */
EXTRACE_EV_STRUCT(STATE,  ON,     CORE, struct fsm_state)
```

* Kinds are `MARK`, `NONE` (timestamp only), `INTS`, `PRINTF`, `BUF`, `STR`,
  `STRUCT` and `ANY`.
* Each macro checks at compile time that it matches its event's kind.
* `default` is the event's runtime state at init; the consumer can change it
  later.
* Types used by `EXTRACE_EV_STRUCT` are declared in an optional
  `extrace_events_types.h` in the same directory.

### 3. Configuration: `extrace_user_cfg.h` (optional)

Defaults live in [extrace_cfg.h](../include/extrace/extrace_cfg.h). These are
the options that matter most:

| Option | Default | Effect |
|---|---|---|
| `EXTRACE_CFG_ENABLE` | 1 | 0 compiles every macro to nothing |
| `EXTRACE_CFG_MAX_EVENTS` | 1024 | Upper bound for catalog IDs |
| `EXTRACE_CFG_MAX_PAYLOAD` | 512 | Largest record payload. `BUF`/`STR` are truncated to it. `INTS`/`PRINTF` records that do not fit are dropped and counted |
| `EXTRACE_CFG_MAX_ARGS` | 16 | Maximum `INTS`/`PRINTF` arguments |
| `EXTRACE_CFG_WRITE_IN_CS_MAX` | 0 | Records up to this size are written inside the reservation section. This saves a lock pair per record but lengthens the section |
| `EXTRACE_CFG_EMIT_SPECIALIZE` | 1 | Register-argument emitters for 1–3 arguments. Set 0 to minimize code size |
| `EXTRACE_CFG_PUBLISH_IN_ISR` | 1 | 0 defers publication out of interrupts (urgent records excepted) |
| `EXTRACE_CFG_PRINTF_LOCAL` | 0 | 1 formats `EXTRACE_PRINTF` on the device instead of deferring it to the host |
| `EXTRACE_CFG_META_MODE` | `INFO` | Where the event and printf metadata goes in the ELF |
| `EXTRACE_CFG_STATS` | 1 | Producer statistics counters |
| `EXTRACE_CFG_FTRACE` | 0 | Function entry tracing, see [ftrace.md](ftrace.md) |

### 4. Build

```cmake
include(path/to/extrace/cmake/extrace.cmake)
extrace_add_image(fw_a
    PORT_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/port      # extrace_port.h
    EVENTS_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/events    # extrace_events.def
    CFG_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/cfg       # extrace_user_cfg.h
    COMPONENTS  prod)
target_link_libraries(fw_a.elf PRIVATE fw_a::extrace)
```

In the default `INFO` metadata mode, `INCLUDE`
[linker/extrace_meta.ld](../linker/extrace_meta.ld) from the image's linker
script. The metadata then stays in the ELF for the host and takes no target
memory.

### 5. Shared ring block

```c
#define TRACE_DATA 8192u   /* power of two */
#define TRACE_BLOCK EXTRACE_RING_BLOCK_SIZE(TRACE_DATA, EXTRACE_MASK_WORDS)
static uint64_t trace_blk[TRACE_BLOCK / 8] __section(".trace_shm");  /* 8-byte aligned */
```

The block holds the control part followed by the data ring. Give the
consumer the block's address in its own address space, using a fixed memory
map or a boot message.

### 6. Init, poll, panic

```c
static const struct extrace_prod_cfg cfg = {
    .block = trace_blk, .block_size = sizeof(trace_blk),
    .ring_id = 3,             /* unique in the system, 0..254 */
    .name = "dsp1",
    .publish_thr = 0,         /* 0 = size / 4 */
};

int main(void)
{
    extrace_prod_init(&cfg);  /* 0, -EXTRACE_EINVAL or -EXTRACE_EBUSY */
    for (;;) {
        ...
        extrace_prod_poll();  /* periodic, from one task context */
    }
}

void HardFault_Handler(void) { extrace_prod_panic(REASON_HARDFAULT, fault_pc); }
```

* **`extrace_prod_init`** may run at any time, before or after the consumer
  starts, and again later to restart tracing.
  * Each call starts a new epoch. A consumer re-attaches to it by itself.
  * Records emitted before init (or during re-init) are dropped and not
    counted.
  * It returns `-EXTRACE_EBUSY` if a reservation or publication is still
    open. Retry from a context those do not interrupt.
* **`extrace_prod_poll`** must not overlap itself or init. Call it every few
  milliseconds from one task, or when the consumer's
  `extrace_port_notify_producer` doorbell fires (the handler only wakes that
  task). It does the following:
  * applies event enables and disables requested by the consumer;
  * runs consumer commands (`FLUSH`, `SYNC`, `INFO`, `THR`);
  * refreshes the cached consumer tail;
  * emits periodic `SYNC` anchors;
  * publishes.
* **`extrace_prod_flush`** publishes everything committed so far.
* **`extrace_prod_panic(reason, pc)`** publishes what is committed, writes a
  `PANIC` record, marks the ring, and stops tracing until the next init. It is
  safe in fault handlers.
* **`extrace_prod_stats(&st)`** reports records, drops, the high-water mark,
  publications and tail refreshes.

## API

All macros take the catalog name without a prefix. Each also has an `_URGENT`
variant, which publishes right after the commit, even from an interrupt. Whether
the doorbell rings still depends on the notify threshold.

| Macro | Record |
|---|---|
| `EXTRACE_MARK(EV)` | Event only, no timestamp (2 bytes) |
| `EXTRACE(EV)` | Event and timestamp |
| `EXTRACE_INTS(EV, a, ...)` | 1–16 integers. Signedness comes from the argument types; small values take 1 byte |
| `EXTRACE_PRINTF(EV, "fmt", ...)` | Site ID and raw arguments. The host formats the string from the ELF metadata |
| `EXTRACE_TEXT(EV, "fmt", ...)` | `STR` formatted on the device (needs `vsnprintf`) |
| `EXTRACE_BUF(EV, p, n)` | Bytes, truncated to `MAX_PAYLOAD` |
| `EXTRACE_BUF_LARGE(EV, p, n)` | Bytes of any length, as a fragment sequence |
| `EXTRACE_STR(EV, s)` | NUL-terminated string |
| `EXTRACE_STRUCT(EV, ptr)` | The declared C type, type-checked |
| `EXTRACE_ON(EV)` | True if the event is compiled in and enabled. Guards expensive argument preparation |

**Direct reservation** writes the payload in place, with no staging copy:

```c
struct extrace_resv r;

if (EXTRACE_RESERVE(DUMP, len, 4, &r)) {   /* align: 1 or a power of two */
    fill(r.payload, len);
    extrace_commit(&r);                    /* or extrace_cancel(&r) */
}
```

Every successful reservation must end in `extrace_commit`,
`extrace_commit_urgent` or `extrace_cancel`. Keep the window short: records
committed after an open reservation are not published until it closes. At most
255 reservations may be open at once. `EXTRACE_RESERVE_STRUCT(EV, &r)` sizes
and aligns the reservation for the event's declared type.

**Function entry tracing** records every call of functions built with
`-fpatchable-function-entry` once the platform has patched them. Each function
is patched with its own flavor: call only, call and time, or call, time and
arguments, each optionally with the caller's return address. See
[ftrace.md](ftrace.md).

**Runtime IDs** check the mask at run time and return false when the event is
disabled or the arguments are invalid. They are for generic layers that get
the event ID as data: `extrace_emit_dyn_none`, `_ints`, `_ints64` and `_buf`.

## Rules

* Tracing is allowed from any context: thread, ISR, nested ISR, or exception.
  It never blocks and never allocates.
* `PRINTF` format strings must be literals. String arguments (`%s`) are copied,
  up to `STR_ARG_MAX` bytes each. Wrap a pointer to a constant string in
  `EXTRACE_CSTR(s)` to log it by address.
* One producer per image (`extrace_producer`). Each image has its own ring and
  `ring_id`.

## Performance notes

* A disabled event costs one load, a bit test and a not-taken branch.
  `EXTRACE_CT_*` compile-time switches remove a whole group.
* The hot path takes one interrupt-masked section to reserve and one to
  commit. `MARK`/`NONE` records, and anything up to `WRITE_IN_CS_MAX`, are
  written inside the first section, so they pay for one section only.
* `INTS` and `PRINTF` with 1–3 32-bit integer arguments pass them in
  registers. When every value is below 128, each field is a single byte store.
* Place the emitters and `extrace_producer` in TCM through
  `EXTRACE_PORT_HOT_TEXT` / `EXTRACE_PORT_HOT_DATA`.
* Raise `publish_thr` (or set it from the consumer) to publish less often. Use
  `_URGENT` only where latency matters.
