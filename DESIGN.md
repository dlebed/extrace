# extrace: Design Document

**extrace (embedded eXecution trace): a portable, lock-free tracing framework for heterogeneous, non-coherent multi-core systems**

| | |
|---|---|
| Status | Draft, v0.5 |
| Date | 2026-10-29 |
| Targets | ARM (Cortex-M, Cortex-R, Cortex-A) and RISC-V (RV32/RV64 with Zbb and Zce) |
| Language | C23 (gcc, clang) for the target library; Python 3.11+ for host tooling |
| Build | CMake (default and only supported build system); CTest for unit tests |
| Scope | Architecture, wire format, shared-memory protocol, producer, consumer/dispatcher, port layer, host tooling, verification |
| Memory model | Every agent maps the shared trace memory **non-cacheable** (§1.7). |

---

## Table of contents

1. [Introduction](#1-introduction)
2. [Architecture overview](#2-architecture-overview)
3. [Key design decisions](#3-key-design-decisions)
4. [Event model](#4-event-model)
5. [Wire format](#5-wire-format)
6. [Shared memory layout and ownership](#6-shared-memory-layout-and-ownership)
7. [Coherency, ordering and lifecycle protocol](#7-coherency-ordering-and-lifecycle-protocol)
8. [Producer design](#8-producer-design)
9. [Event enable control](#9-event-enable-control)
10. [Deferred printf and image metadata](#10-deferred-printf-and-image-metadata)
11. [Consumer (ring reader) design](#11-consumer-ring-reader-design)
12. [Dispatcher and sinks](#12-dispatcher-and-sinks)
13. [Port layer](#13-port-layer)
14. [Configuration reference](#14-configuration-reference)
15. [Host-side tooling](#15-host-side-tooling)
16. [Performance and memory budget](#16-performance-and-memory-budget)
17. [Robustness and error handling](#17-robustness-and-error-handling)
18. [Verification strategy](#18-verification-strategy)
19. [Source tree, build and coding style](#19-source-tree-build-and-coding-style)
20. [Implementation roadmap](#20-implementation-roadmap)
21. [Risks and open questions](#21-risks-and-open-questions)
22. [Appendices](#22-appendices)

---

## 1. Introduction

### 1.1 Problem statement

An SoC contains several subsystems (application cores, MAC/PHY controllers, DSPs,
accelerators). Each one runs its own firmware image and they share some memory, but
that memory is **not hardware-coherent**. Every agent maps the
shared trace memory **non-cacheable** (§1.7). Engineers need one tracing facility
that:

* records events from all subsystems with very low overhead, from both thread and
  interrupt context;
* moves the data through shared memory to one subsystem (the *dispatcher*). The
  dispatcher forwards the data to the outside world (PCIe/USB/Ethernet to a host,
  UART) or processes it locally;
* uses as little device memory, CPU time and code space as possible;
* can be decoded offline using metadata extracted from the firmware images.

### 1.2 Goals

| ID | Goal |
|----|------|
| G1 | **Portable library**, freestanding C23, no external dependencies. All platform specifics live in a single `extrace_port.h` with documented, inlinable hooks. |
| G2 | **Multiple producers, single consumer per ring.** Shared memory is non-coherent and mapped non-cacheable by every agent; no cache maintenance anywhere (§1.7). |
| G3 | **Dispatcher with multiple sinks.** *Bulk* sinks take unparsed chunks with zero copy. *Record* sinks get parsed events filtered by source, event and payload kind. |
| G4 | **Per-event runtime enable/disable** with a compile-time default configuration. A disabled event costs a few instructions. |
| G5 | **Event kinds**: minimal marker, no-payload event, variable integer list, deferred printf, buffer, structure, string. |
| G6 | **Compact encoding**: base-128 varints for all integers, with a fast branch-light encoder. |
| G7 | **Offline decoding**: format strings, event names, struct layouts and enum names come from post-processing the firmware ELF. They are not stored on the device. |
| G8 | **Modular**: producer and consumer are separate link units. An image can contain either one or both. |
| G9 | **Drop-newest on overflow**, with exact accounting of lost events reported in-band. |
| G10 | **Reentrant producer** callable from threads and ISRs. Critical sections come from the port and stay very short. |
| G11 | **No locks between agents.** Each ring is single-producer/single-consumer and lock-free across subsystems. |

### 1.3 Non-goals

* Hardware trace (ETM/PTM/Nexus) integration. extrace is software instrumentation.
* An "overwrite-oldest" flight-recorder mode. The rings always drop the newest data.
* Tracing from non-maskable contexts (NMI, or FIQ used as a non-maskable source).
  Only thread context and regular (maskable, possibly nested) interrupts are
  supported (§8.9).
* SMP subsystems. A firmware image runs on exactly one core (AMP). Several cores
  may run instances of the same image or different images. Each instance is an
  independent producer with its own ring (§8.1).
* A host transport protocol stack. extrace defines the bulk *frame* format, and the
  transport (PCIe DMA, USB bulk, sockets) belongs to the platform.

### 1.4 Requirements traceability

| Requirement | Where addressed |
|---|---|
| 1. Standalone library + single port header with inlined hooks | §2.3, §13, §19 |
| 2. Multi-producer / single consumer, non-coherent memory (mapped non-cacheable) | §1.7, §3 (D1, D8), §6, §7, §13.3 |
| 3. Dispatcher with multiple sinks and configurable filters | §12 |
| 4. Per-event enable/disable with defaults, minimal disabled cost | §4.5, §9, §16.1 |
| 5. Event kinds incl. deferred printf with 16-bit string IDs | §4.3, §5.7, §10 |
| 6. Base-128 integer encoding, fast | §5.2, §22.A |
| 7. Host-side metadata, struct layouts, post-processing | §10, §15 |
| 8. Producer and consumer as separate modules | §2.3, §19 |
| 9. Bulk and fine-grained sinks | §12.4–§12.8 |
| 10. Overflow handling: no overwrite, lost counter, in-band report | §8.6, §5.9 |
| 11. Reentrant producer, thread + ISR, minimal critical section | §8.3–§8.5 |
| 12. Minimal locks, lockless SPSC between agents | §3 (D1, D6), §7 |
| 13. gcc/clang, C23, C library, Python scripts | §13, §15, §19 |
| 14. CMake build, unit tests covering all functionality, sanitizers in test builds | §18, §19 |
| 15. Linux kernel-like coding style, 4-space indentation | §19.3 |
| 16. Per-instance event control (each subsystem instance has its own enable and control bitmaps) | §6.1, §9 |

### 1.5 Terminology

| Term | Meaning |
|---|---|
| **Subsystem / agent** | A CPU (or cluster) running its own firmware image. "Agent" also covers a DMA engine that a bulk sink uses to read ring data (§12.6), the only DMA access to the rings. |
| **Producer** | Code on an agent that generates trace records into a ring. |
| **Ring** | A single-producer/single-consumer byte ring buffer in shared memory, plus its control block. |
| **Consumer** | The code that reads one ring. One consumer per ring. |
| **Dispatcher** | Consumer-side module that drains many rings and fans the data out to sinks. |
| **Sink** | A destination for trace data: *bulk* (raw chunks) or *record* (parsed, filtered events). |
| **Host** | The machine running offline decoding with metadata. |
| **Record** | One encoded trace event in the ring. |
| **Reserve / commit / publish** | Producer phases: claim space, finish writing, make data visible to the consumer. |
| **Non-cacheable** | Mapped so that loads and stores go to memory, not to a data cache: ARM *Normal Non-cacheable*, RISC-V main memory with a non-cacheable PMA or Svpbmt `NC`. Stores may still be buffered and reordered by write buffers, so barriers remain (§6.3). |
| **Epoch** | A per-ring counter incremented every time the producer (re)initializes the ring. |
| **Site** | One static call site of `EXTRACE_PRINTF`, described by a descriptor in the ELF. |
| **Instance** | One running copy of a firmware image on one core. Two cores running the same image are two instances, with two rings and two independent event masks. |
| **Index** | A free-running 32-bit byte counter (`head`, `tail`) that starts at 0 in every epoch and wraps naturally at 2^32. The ring size is a power of two, so the physical ring offset is `index & (size − 1)` (§6.6). |

### 1.6 Lessons from etrace

extrace is a new implementation. No code is taken over from the earlier `etrace`
prototype, but its experience shaped several decisions:

| etrace | Lesson for extrace |
|---|---|
| Free-running read/write indices; `used = w - r` | **Adopted** (§6.6): full and empty are unambiguous, there is no "one byte free" rule and no special case for `tail == 0`, and post-mortem decoding needs nothing but the two indices. The power-of-two ring size is kept as well: every index operation is one instruction. |
| `write_idx` and `read_idx` in the same 32-byte control block | Pitfall: on cached non-coherent memory each side's cache clean overwrites the other side's index. extrace maps shared memory non-cacheable and keeps one writer per 32-bit word (D8). |
| `drop_count` incremented by the producer and reset by the consumer | Pitfall: two writers of one word. extrace: drops are counted by the producer only and reported in-band (`LOST`). |
| Whole record written without a critical section | Pitfall: not reentrant from ISRs. extrace: reserve/commit with an in-flight counter (D6). |
| Consumer attaches by casting a pointer | Pitfall: garbage or restarted producers are not detected. extrace: magic plus epoch handshake (§7.6). |
| 12-byte aligned header, `MAX_RECORD` of slack held back on every reservation | Pitfall: up to half of the memory wasted. extrace: varint records, exact fit test, the whole ring usable. |
| Batch read that does not advance the index, explicit release | **Kept as an idea**: zero-copy peek/release (§11). |
| Inline enabled check, out-of-line writers | **Kept**: the same fast path shape (§8.2). |
| Host-only unit tests with sanitizers | **Kept and extended** (§18). |

### 1.7 Non-cacheable shared memory

**Every agent maps the shared trace memory non-cacheable.** That covers producers, the dispatcher and
any DMA engine reading for a bulk sink. As a consequence:

* There is **no cache maintenance** anywhere: no clean, no invalidate, no data-cache
  hooks in the port, and no compile-time cache option per image.
* There is **no coherency granule**. Ring blocks need no cache-line alignment and no
  line padding, and there is no system-wide line size to agree on. One writer per
  32-bit word is the only ownership rule (D8).
* **Barriers remain.** Write buffers may still reorder stores to non-cacheable
  memory, and the epoch and seqlock protocol (§7) still covers torn multi-word
  reads and the init/restart races. These have nothing to do with caches.
* **Speed depends on the bus.** Every shared-memory access goes to the
  interconnect. Producers write records directly to the ring, store-only, with no
  read-modify-write (§8.8.1). Consumers read windows in word-sized chunks into
  local memory before parsing (§11.5, §12.5).

The memory type affects nothing else: the wire format, the varint module, the
event model, the producer API (including direct reservations), the lifecycle
protocol, the dispatcher and sinks, and the host tooling are independent of it.

---

## 2. Architecture overview

### 2.1 System view

```
 Subsystem A (FW-A)                 Subsystem B (FW-B)                Subsystem D (FW-D: dispatcher)  
+------------------------------+   +------------------------------+   +--------------------------------------+
| thread / ISR code            |   | thread / ISR code            |   |  extrace_cons  (one reader per ring)  |
|   EXTRACE_*(EV, ...) macros   |   |   EXTRACE_*(EV, ...) macros   |   |        |                             |
|   extrace_prod  (ring A0)     |   |   extrace_prod  (ring B0)     |   |  extrace_disp                         |
+--------------|---------------+   +--------------|---------------+   |   +-- bulk sinks ---> PCIe DMA ----+  |
               | uncached writes                  | uncached writes   |   +-- record sinks -> UART, stats  |  |
               v                                  v                   |  (optional local extrace_prod D0)    |
 =============================== shared memory, mapped non-cacheable everywhere ===========================
   [ctl A0 | data A0]      [ctl A1 | data A1]      [ctl B0 | data B0]      [ctl D0 | data D0]
 ===========================================================================================================
               |  doorbell IRQ (optional, rate-limited) ---------------------------------> D
                                                                                                     |
                                                     Host: extrace_extract.py (ELF -> metadata)       v
                                                           extrace_decode.py (frames + metadata -> text/JSON/timeline)
```

* Each producing **core** owns one ring. Every core runs exactly one firmware
  instance (AMP). Two cores running the same image are two producers with two rings,
  two `ring_id`s and two independent event masks.
* A ring is written by exactly one agent and read by exactly one agent. No location
  in shared memory has two concurrent writers (§6.3).
* The dispatcher polls rings or is woken by a doorbell. It reads published windows
  and hands them to sinks.
* The host receives framed raw ring data plus per-image metadata. All formatting
  happens there.

### 2.2 Life of an event

1. Code calls `EXTRACE_INTS(TX_DONE, q, len)`.
2. The macro checks the compile-time switch for `TX_DONE`. If the event is compiled
   out, no code is generated.
3. It tests one bit of the producer-local enable mask. If the bit is clear, the
   arguments are **not evaluated** and execution continues (§16.1).
4. If enabled, it computes the payload size outside any critical section and calls
   the out-of-line emitter.
5. **Reserve** (critical section, about 20–40 instructions): read the timestamp,
   compute the delta, find contiguous space, account any pending lost-event report,
   bump `head` and `inflight`.
6. **Write** (interrupts enabled): varint-encode the header, timestamp delta,
   length and payload directly into the ring. Records never wrap, so there are no
   per-byte modulo checks.
7. **Commit** (critical section, about 5 instructions): `--inflight`. When it
   reaches zero, `committed = head`.
8. **Publish** (batched by threshold, poll, urgency or explicit flush): write
   barrier, store `head` into the producer-owned control word, and optionally ring
   the doorbell.
9. The dispatcher observes the new `head` and offers the window to bulk sinks (zero
   copy). For record sinks it copies the window to local memory in word-sized chunks
   and decodes the records there (§12.5).
10. Once all lossless sinks are done with the window, the dispatcher stores `tail`
    into the consumer-owned control word, and the producer can reuse the space.
11. On the host, frames are demultiplexed per ring, decoded and annotated with ELF
    metadata, mapped onto a common timeline and rendered.

### 2.3 Module decomposition

| Module | Files | Linked into | Depends on |
|---|---|---|---|
| **varint** (header-only) | `extrace_varint.h` | every image, host tests, fuzzers | `<stdint.h>` and the compiler macros only (§8.13) |
| **common** (header-only) | `extrace_cfg.h`, `extrace_types.h`, `extrace_wire.h`, `extrace_shm.h`, `extrace_events.h` | every image | varint, `extrace_port.h`, user `extrace_events.def`, optional `extrace_user_cfg.h` |
| **producer** | `extrace_prod.h`, `extrace_prod.c`, `extrace_args.h` | producer images | common |
| **consumer** | `extrace_cons.h`, `extrace_cons.c` | dispatcher images | common |
| **record decoder** | `extrace_rec.h`, `extrace_rec.c` | dispatcher images (record sinks), unit tests | common |
| **dispatcher** | `extrace_disp.h`, `extrace_disp.c` | dispatcher images | consumer, record decoder |
| **reference sinks** | `sinks/extrace_sink_bulk.c`, `sinks/extrace_sink_text.c`, `sinks/extrace_sink_stats.c` | optional | dispatcher |
| **port** | `extrace_port.h` (one per image/platform), `extrace_compiler.h` (§13.7) | every image | platform BSP only |
| **host tools** | `tools/extrace/*.py` | host | Python stdlib; `pyelftools` optional (DWARF) |

A producer-only image links `extrace_prod.c` and nothing else. An image that
dispatches and also traces links everything. The producer never references consumer
symbols and the reverse also holds; this is checked in CI by linking minimal test
images (§18).

---

## 3. Key design decisions

| # | Decision | Rationale | Rejected alternatives |
|---|---|---|---|
| D1 | **One SPSC ring per producing core.** The consumer side reads many rings. | SPSC needs no atomic read-modify-write across agents. That matters because atomics do not work across non-coherent agents. Per-core rings mean no two cores ever write the same ring. | A shared MPSC ring needs cross-agent atomics or a hardware mutex, so it contends and fails on non-coherent memory. |
| D2 | **Drop-newest on overflow** with a lost-event counter, reported in-band before the next successful record. | Required. Old data is never corrupted. Producer and consumer stay decoupled because the producer never has to move the consumer's tail. | Overwrite-oldest requires the producer to modify reader state, which makes two writers. |
| D3 | **Records are contiguous in the ring.** The tail gap at wrap is filled with a PAD record. | The encoder writes linearly with no wrap checks, which is the main enabler of fast varint encoding. The consumer can parse and hand out zero-copy pointers to every record. Wasted space is at most one maximum record per wrap. | Splitting records across the wrap point needs a wrap check per write, copying on the consumer side, and complex decoders. |
| D4 | **Byte-oriented varint wire format, no alignment.** A typical record is 2–8 bytes. | Memory efficiency is the dominant constraint. Varints are also endian-neutral. | Fixed 8-byte aligned header plus typed payload header: 2–4× larger for common events. |
| D5 | **32-bit delta timestamps**, taken inside the reservation critical section. The producer never handles 64-bit time. | Buffer order then equals timestamp order. A delta is usually 1–3 bytes instead of 4–8. All producer time arithmetic is single 32-bit operations, which suits 32-bit cores. Periodic `SYNC` and `LOST` records carry the absolute 32-bit value, and the dispatcher and host build the 64-bit timeline. | Absolute 32/64-bit timestamps in every record. A 64-bit extended timestamp per producer (v0.2), which costs 64-bit arithmetic in every critical section. |
| D6 | **Reserve / write / commit with an in-flight counter.** Only the fully committed prefix is published. The mechanism is also public (§8.14): reserve, fill in place, commit. | Critical sections cover only pointer arithmetic. A preempted writer can never cause a partially written record to be published. | Writing the whole record under the critical section gives long IRQ latency. Publishing the reserved head would expose half-written records. |
| D7 | **Batched publication** (threshold / poll / urgent / explicit). | A publish is only a barrier and one store, but barriers can stall until the write buffer drains, and doorbells interrupt the dispatcher, so their cost is amortized over many records. | Publishing per record. |
| D8 | **Single writer per 32-bit word**, written with whole-word stores. Producer- and consumer-owned fields are grouped by owner but need no line padding. Shared memory stores **offsets and indices, never pointers**. | With non-cacheable mappings no agent writes back a whole line, so fields of different writers may share a line. Aligned word stores are single-copy atomic, so a word with one writer is never torn. Agents map shared memory at different addresses. | Line-padded regions per owner, which cost up to about 110 B more per ring. Sub-word fields written by different agents. Pointers in shared memory. |
| D9 | **Producer-local effective event mask**, recomputed from compile-time defaults plus consumer-owned *set*/*clear* override bitmaps, guarded by a seqlock generation. | The enabled check never touches shared memory. Defaults live in the producer image, and the consumer needs no knowledge of them to flip one event. Single writer per word. | A shared mask read on every event (uncached loads, slow). A two-writer "updated" flag. |
| D10 | **Deferred printf**: the record carries a 16-bit *site ID* plus binary arguments. Format strings and site descriptors live in an ELF section that is not loaded. | The device stores no format strings, does no formatting and stays reentrant. The host does all formatting. | Formatting on the device: slow, large, and needs a shared print buffer. |
| D11 | **Payload kind is encoded in the record header** (3 bits). | On-device record sinks can skip, filter and decode basic kinds without metadata. | Kind known only from metadata. |
| D12 | **X-macro event catalog** as single source of truth. Metadata is emitted into the ELF and extracted after the build. | No build-time code generator is needed for firmware. The compiler checks kinds and struct types at each call site. IDs need not be stable across builds. | Hand-numbered enums plus a regex-based generator script. |
| D13 | **Port header of `static inline` hooks**: timer, critical section, barriers, doorbells, compiler macros. No cache hooks. | Zero call overhead. The port is small: the memory-type decision is made once, in the platform's MPU/MMU/PMA setup, not in the library. | `#ifdef PLATFORM_X` ladders spread over the library. Per-image cache maintenance options. |
| D14 | **Dispatcher works on zero-copy windows** with two sink classes. Every sink callback is **synchronous**: a window is released only after all sinks have returned. | A bulk sink can DMA straight from the ring inside its callback. Parsing happens once per record for all record sinks. No reference to ring memory outlives a callback, so release needs no completion tracking. | Copying every window to a staging buffer (still possible inside a sink). Asynchronous bulk sinks with deferred completion (v0.3): a pending-window queue, a separate read cursor and epoch checks on late completions, for no gain over a sink that waits for its own DMA. |
| D15 | **Critical section = local interrupt disable** through a flags-returning port API: `flags = extrace_port_irq_save(); ... extrace_port_irq_restore(flags);` | Simplest, bounded and portable. Producer state belongs to one core (AMP, no SMP), so local IRQ masking is enough. Returning the previous state as `flags` makes nesting free, and it works whether the caller runs with interrupts enabled or already disabled. | A CAS-based reservation loop, which is harder to make correct together with commit tracking and offers little benefit on single-issue embedded cores. A global enable/disable pair, which breaks when called with IRQs already off. |
| D16 | **Free-running 32-bit indices** for `head` and `tail`, and a **power-of-two ring size** (§6.6). | `used = head − tail` is exact, so full and empty are unambiguous and the whole ring is usable. A stale or corrupt index is detectable with one compare. Indices wrap naturally at 2^32, every index operation is one add, subtract or AND, and every shared index is one torn-free word. | Offsets in `[0, size)` with a "one byte free" rule (v0.2). Any line-multiple size with indices wrapping at a multiple of the size (v0.4 draft): no per-record cost, but a second number space in the producer, an init-time division and more test cases, for sizing flexibility that is not needed. |
| D17 | **Per-instance event control.** Each ring block carries its own set/clr override bitmaps, sized by that image's catalog, and each producer instance keeps its own effective mask. | Subsystems have different event IDs, names and format strings, and two instances of the same image may need different settings. | A global mask per image, or per system. |
| D18 | **Little-endian shared memory.** Every shared control word, the bulk frame header and the fixed-width printf float arguments are little-endian. | Nearly all target systems are little-endian, so every shared access is a plain load or store with no conversion. The record stream is varint-based and byte-order-neutral anyway. Big-endian agents stay correct: their shared-word helpers add one byte swap, on control paths only. | Native byte order per agent: mixed-endian systems could not attach. A byte-order flag per ring: a branch on every control access for a case that barely exists. |

---

## 4. Event model

### 4.1 Event catalog

Each firmware image provides `extrace_events.def`, an X-macro list that is the single
source of truth for its events:

```c
/* extrace_events.def: event catalog of FW-A (example) */

/*           group     compiled-in */
EXTRACE_GROUP(SCHED,    1)
EXTRACE_GROUP(TX,       1)
EXTRACE_GROUP(RX,       1)
EXTRACE_GROUP(LOG,      1)
EXTRACE_GROUP(DEBUG,    0)          /* whole group removed at compile time */

/*        name             kind     default  group   spec (host-only, free text) */
EXTRACE_EV(SCHED_SWITCH,    INTS,    ON,      SCHED,  "prev:u next:u prio:s")
EXTRACE_EV(IRQ_ENTER,       NONE,    OFF,     SCHED,  "")
EXTRACE_EV(IRQ_TICK,        MARK,    OFF,     SCHED,  "")
EXTRACE_EV(TXQ_STATE,       INTS,    ON,      TX,     "q:u state:enum txq_state depth:u")
EXTRACE_EV(RX_DUMP,         BUF,     OFF,     RX,     "")
EXTRACE_EV(LOG_INFO,        PRINTF,  ON,      LOG,    "")
EXTRACE_EV(LOG_DBG,         PRINTF,  OFF,     LOG,    "")
EXTRACE_EV(FW_NAME,         STR,     ON,      LOG,    "")
EXTRACE_EV(DBG_PROBE,       ANY,     OFF,     DEBUG,  "")

/*               name       default  group  C type */
EXTRACE_EV_STRUCT(TX_DONE,   ON,      TX,    struct tx_done_info)
EXTRACE_EV_STRUCT(RX_STATS,  OFF,     RX,    struct rx_stats)
```

`extrace_events.h` includes the `.def` several times with different definitions of
`EXTRACE_GROUP`, `EXTRACE_EV` and `EXTRACE_EV_STRUCT` to generate:

| Artifact | Example | Used for |
|---|---|---|
| Event ID enum | `EXTRACE_EV_TX_DONE = 25` | wire header |
| Compile-time switch | `EXTRACE_CT_TX_DONE = EXTRACE_GRP_TX_ON && EXTRACE_CFG_ENABLE` | dead-code elimination |
| Declared kind | `EXTRACE_KIND_TX_DONE = EXTRACE_K_STRUCT` | `static_assert` at call sites |
| Declared type | `typedef struct tx_done_info extrace_type_TX_DONE;` | `_Generic` type check in `EXTRACE_STRUCT` |
| Defaults bitmap | `extrace_ev_defaults.w[EXTRACE_MASK_WORDS]` (const, in flash, §4.5) | producer init, mask recompute (§7.5 M3) |
| Metadata descriptors | `struct extrace_meta_ev` objects in the metadata section | host extraction (§10.5) |

Call sites use **event names as tokens**, not enum values: `EXTRACE(IRQ_ENTER)`.
Token pasting gives access to all compile-time attributes without runtime lookups.
For events whose ID is computed at runtime there is a separate `extrace_emit_dyn_*`
API (§8.11) that checks the mask at runtime only.

### 4.2 Event IDs

* IDs are assigned **automatically and sequentially** in catalog order. They are
  stable only within one build. The host always decodes with the metadata of the
  exact image, matched by build ID (§10.6), so cross-build stability is not needed.
  This avoids hand numbering and its merge conflicts.
* `0..15` are **reserved system events** (§4.6). User events start at 16.
* The catalog defines `EXTRACE_EV_COUNT` (highest ID + 1), a compile-time constant
  per image. **Every per-event structure is sized from it**: the producer's mask
  (`EXTRACE_MASK_WORDS = ceil(EXTRACE_EV_COUNT / 32)`), the defaults table, and the
  shared-memory bitmaps of that image's ring. An image with 200 events uses
  7-word bitmaps, not 32.
* `EXTRACE_CFG_MAX_EVENTS` (default 1024) is only an **upper bound**. The catalog is
  `static_assert`ed against it, and system memory maps use it to size ring blocks
  without knowing each image's catalog (§6.1).
* Consumers learn each ring's event count at attach time from `pinfo.ev_count`, so
  consumer-side structures are sized at runtime (§11, §12.3).
* IDs up to 2047 encode with the kind in a 2-byte header (§5.4).
* IDs are per image. Different images may reuse the same numbers. The source ring
  identifies the image.
* Shared libraries used by several images (RTOS, drivers) can provide `.def`
  fragments that each image `#include`s in its catalog.

### 4.3 Event kinds

| Kind | Code | Wire content after header | API | Typical size |
|---|---|---|---|---|
| `NONE` | 0 | ts-delta | `EXTRACE(EV)` | 2–4 B |
| `MARK` | 1 | *(nothing, no timestamp)* | `EXTRACE_MARK(EV)` | 2 B (user IDs 16–2047) |
| `INTS` | 2 | ts-delta, len, sign-mask, N varints | `EXTRACE_INTS(EV, a, b, ...)` (1–16 args) | 5–6 B + 1–5 B/arg |
| `PRINTF` | 3 | ts-delta, len, site-ID, args | `EXTRACE_PRINTF(EV, "fmt", ...)` | 5–8 B + args |
| `BUF` | 4 | ts-delta, len, raw bytes | `EXTRACE_BUF(EV, ptr, n)` | 4–6 B + n |
| `STRUCT` | 5 | ts-delta, len, raw bytes of declared type | `EXTRACE_STRUCT(EV, ptr)` | 4–6 B + sizeof |
| `STR` | 6 | ts-delta, len, characters (no NUL) | `EXTRACE_STR(EV, s)` | 4–6 B + strlen |
| `EXT` | 7 | ts-delta, len, ext-type, ext body | internal (fragments, future) | – |

`ANY` in the catalog means "kind not fixed": any API may be used with that event. The
kind is still always present on the wire.

`MARK` is the absolute minimum. It records only that the event happened, in buffer
order, with no timestamp and no timer read. The host gives it the time of the
neighbouring records. It is meant for very hot paths such as counting, coverage and
"was this branch taken".

`STRUCT` and `BUF` have the same encoding. The difference is semantic: a `STRUCT`
event has a declared C type and the host decodes it field by field using the layout
extracted from the ELF.

### 4.4 Groups and compile-time removal

* Every event belongs to one **group**. `EXTRACE_GROUP(name, compiled_in)` removes all
  events of a group from the build when `compiled_in` is 0. Their call sites become
  `if (0) { ... }` blocks, which still type-check the arguments but generate no code.
* `EXTRACE_CFG_ENABLE=0` removes all tracing from an image the same way.
* Group overrides from the build system: `-DEXTRACE_GRP_DEBUG_ON=1` overrides the
  catalog value, so debug builds can include groups that release builds drop.
* At runtime there is only a per-event mask. Group operations ("enable all TX") are
  carried out by the consumer or host, which expand groups to event sets using
  metadata. This keeps the producer fast path to a single bit test.

### 4.5 Default enable state

* Column `default` (`ON`/`OFF`) defines the state after producer init, before any
  consumer configuration exists.
* The producer-local mask starts **all-zero in `.bss`**. Calls made before
  `extrace_prod_init()` are therefore cheap no-ops and never touch unconfigured
  memory.
* At init the producer copies the **defaults bitmap** into its mask. Later consumer
  overrides apply on top of the defaults word by word as
  `effective[w] = (defaults[w] & ~clr[w]) | set[w]` (§7.5 M3, §9).

**Defaults bitmap.** The bitmap is a constant of exactly `EXTRACE_MASK_WORDS` words
in read-only memory, with bit `i` of word `w` set when event `32w + i` is default-on.
It is the same layout as the mask and the shared override bitmaps. `extrace_events.h`
generates it from the catalog at compile time, with no generator script, by
expanding the catalog into a struct with one 1-bit field per event, in ID order,
overlaid on the word array:

```c
#define EXTRACE_DEF_ON  1u
#define EXTRACE_DEF_OFF 0u

struct extrace_ev_bits {
    unsigned int : 16;                          /* IDs 0..15: system events */
#define EXTRACE_EV(name, kind, def, grp, spec)  unsigned int ev_##name : 1;
#define EXTRACE_EV_STRUCT(name, def, grp, type) unsigned int ev_##name : 1;
#include "extrace_events.def"
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT
};

union extrace_ev_defaults {
    struct extrace_ev_bits b;
    uint32_t w[EXTRACE_MASK_WORDS];
};
static_assert(sizeof(union extrace_ev_defaults) == 4u * EXTRACE_MASK_WORDS);

/* in extrace_prod.c: one initializer per event, ID order */
const union extrace_ev_defaults extrace_ev_defaults = { .b = {
#define EXTRACE_EV(name, kind, def, grp, spec)  .ev_##name = EXTRACE_DEF_##def,
#define EXTRACE_EV_STRUCT(name, def, grp, type) .ev_##name = EXTRACE_DEF_##def,
#include "extrace_events.def"
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT
} };
```

* **Size and cost.** The bitmap takes `4 × EXTRACE_MASK_WORDS` bytes of flash (8 B for
  a 60-event catalog, 128 B at the 1024-event cap), and no RAM or init code. The v0.3
  list of default-on IDs took 2 bytes per default-on event and could not be indexed
  by word.
* **Robustness.** The fields are named `ev_<name>`. Arguments of `##` are not
  macro-expanded, so a user macro with the same name as an event cannot break the
  expansion. The fields are `unsigned int`, the bit-field type that every C
  implementation supports. `uint32_t` is `unsigned long` on some ARM toolchains.
  Events of groups that are compiled out keep their ID and their bit.
* **Bit order.** The psABIs of the targets fix the bit-field allocation order. On
  little-endian ARM and RISC-V, the first field is the least significant bit of its
  32-bit unit, so the words are the bitmap directly. On big-endian ARM, allocation
  starts at the most significant bit, so every word is bit-reversed. The producer
  then applies one `rbit` per word when it reads the bitmap, at init and in M3, both
  cold paths. The fast-path bit test is unchanged. Verified with clang 18 for
  Cortex-M4, Cortex-M0+, RV32 Zbb+Zce, big-endian Cortex-M and Cortex-R, and with
  gcc on the host. Every target produces the expected words.

### 4.6 System events

IDs 0–15 are defined by extrace and have the same meaning in every image:

| ID | Name | Kind | Payload | Emitted |
|---|---|---|---|---|
| 0 | `PAD` | special | none or skipped bytes; **no timestamp** | at wrap (§5.8) |
| 1 | `SYNC` | INTS | `ts_abs` | after init, periodically from poll, on command |
| 2 | `LOST` | INTS | `count` [, `bytes`] | immediately before the first successful record after drops |
| 3 | `INFO` | BUF | producer info, varint-serialized (§5.9.3) | first record after init, on command |
| 4 | `FTRACE` | NONE / MARK / INTS | function entry: address [, arguments] (§5.9.7) | the ftrace trampoline; its mask bit is the global switch |
| 5 | `CFG` | INTS | `mask_gen` | after applying a new mask generation |
| 6 | `PANIC` | INTS | `reason`, `pc` (optional) | from the panic hook before the final flush |
| 7 | `FRAG` | EXT | fragment of a large buffer (§5.9.6) | `EXTRACE_BUF_LARGE()` |
| 8 | `FTRACE_CALLER` | NONE / MARK / INTS | `FTRACE` with the caller's return address (§5.9.7) | the ftrace trampoline, for functions patched with a CALLER flavor; its mask bit has no effect |
| 9 | `SKIP` | BUF | none: the bytes of a cancelled direct reservation, skipped; its `tsd` stays on the timeline | `extrace_cancel()` (§8.14) |
| 10–15 | reserved | – | – | – |

---

## 5. Wire format

### 5.1 Principles

* A ring carries a **byte stream of records**. No alignment, no padding inside records.
* All integers are **unsigned LEB128 varints**. Signed values use **zigzag** first.
* Every record can be skipped without metadata: the header gives the kind and the
  kind determines whether a length follows.
* Records never span the end of the ring (§5.8).
* The stream is endian-neutral except for the raw bytes of `BUF`/`STRUCT` payloads.
  Those are in the producer's native byte order, which the `INFO` record states.
* Every fixed-width multi-byte value that extrace itself defines is **little-endian**
  (D18): shared control words (§6.2), `F32`/`F64` printf arguments (§5.7.2) and the
  bulk frame header (§12.7).

### 5.2 Varint and zigzag

```
uvarint(v): repeat { b = v & 0x7F; v >>= 7; emit(b | (v ? 0x80 : 0)); } until v == 0
zigzag(s)  = (s << 1) ^ (s >> (N-1))         ; N = 32 or 64, arithmetic shift
unzigzag(u)= (u >> 1) ^ -(u & 1)
```

| Value range | Bytes |
|---|---|
| 0 – 127 | 1 |
| 128 – 16 383 | 2 |
| 16 384 – 2 097 151 | 3 |
| up to 2^28 − 1 | 4 |
| up to 2^32 − 1 | 5 |
| up to 2^64 − 1 | 10 |

**Encoded length without a loop** (used to size reservations before writing, on
cores with a count-leading-zeros instruction):

```
bits(v) = W - clz(v | 1)            ; W = 32 or 64
len(v)  = (bits(v) * 9 + 64) >> 6   ; exact for bits = 1..64
        = (352 - 9 * clz(v | 1)) >> 6   ; W = 32 form used by the producer
```

**Encoder.** Every header, delta, length and integer argument is a varint, so the
encoder is the producer's hottest code after the enabled check. Its implementation
trades code size for speed: constants are pre-encoded at compile time, and the
runtime encoder is a fully unrolled shift-test chain, inlined into every emitter,
with an "all fields one byte" fast path for argument lists. The destination is
guaranteed contiguous and in bounds, so there are no bound or wrap checks while
encoding. §8.13 specifies the techniques and their measured cost on ARM and
RISC-V; Appendix 22.A has the routines.

**Decoder rules.** A decoder must reject varints longer than 10 bytes (5 for fields
defined as 32-bit). It must also reject a varint that runs past the end of the
record or window. Over-long encodings (redundant `0x80` bytes) are tolerated; the
producer itself writes one for two PADN gap sizes (§5.8).

### 5.3 Record grammar

```
record      = pad-record / event-record
pad-record  = %x00                                 ; PAD1: one filler byte
            / %x04 uvarint(n) n*OCTET              ; PADN: header(id 0, BUF), skip n bytes
event-record= hdr [tsd] [len payload]
hdr         = uvarint( (event_id << 3) | kind )    ; event_id >= 1
tsd         = uvarint( ts - prev_ts )              ; present unless kind == MARK
len         = uvarint( length of payload )         ; present unless kind in {NONE, MARK}
payload     = len*OCTET                            ; interpretation per kind (5.7)
            / %x20 tsd uvarint(addr)               ; FTRACE short shapes (5.9.7):
            / %x21 uvarint(addr)                   ; no len field
            / %x40 tsd uvarint(addr) uvarint(caller) ; FTRACE_CALLER short shapes
            / %x41 uvarint(addr) uvarint(caller)
```

### 5.4 Header

`hdr = (event_id << 3) | kind`, varint encoded:

| Event ID range | Header bytes |
|---|---|
| 0 – 15 (system) | 1 |
| 16 – 2047 | 2 |
| 2048 – 262 143 | 3 |

Byte `0x00` (ID 0, kind NONE) is the 1-byte PAD. Zero-filled memory therefore parses
as padding, which helps raw post-mortem dumps.

### 5.5 Timestamp field

* `tsd` is the difference, **modulo 2^32**, in **ticks** between this record's
  timestamp and the previous timestamp-carrying record **in the same ring**:
  `tsd = (uint32_t)(ts - prev_ts)`. The decoder keeps `prev_ts` per ring.
* The timestamp is read **inside the reservation critical section**, so ring order
  and timestamp order agree, and `tsd` is the true elapsed time as long as
  consecutive timestamped records are less than one timer wrap apart. The producer
  guarantees this with periodic `SYNC` records (§8.7).
* The time base is the **SoC-global free-running 32-bit timer**, which every
  subsystem reads through `extrace_port_ts()` (§13.2). **The producer works only
  with 32-bit time**: it never extends the timer, keeps a 32-bit `last_ts`, and
  encodes 32-bit deltas, which are 1–3 bytes on the wire for typical event rates.
  Extension to a 64-bit timeline happens only in the dispatcher and on the host
  (§12.5, §15.5).
* `SYNC` and `LOST` records carry the **absolute** 32-bit timer value in their
  payload. Their `tsd` is 0, and the decoder sets `prev_ts := ts_abs` after reading
  one. The first records after init are `INFO` and `SYNC`, so every stream starts
  anchored, and every loss episode re-anchors the stream.
* `MARK` and `PAD` have no `tsd` and do not change `prev_ts`.
* The record whose reservation carried a `LOST` record uses `tsd = 0` (§8.6).

### 5.6 Length field

Present for kinds `INTS`, `PRINTF`, `BUF`, `STRUCT`, `STR`, `EXT`. It is the byte
count of the payload that follows, at most `EXTRACE_CFG_MAX_PAYLOAD` (default 512,
hard limit 65 535, lower limit 64 so that `INFO` always fits). Two derived bounds are
used throughout:

* `EXTRACE_MAX_RECORD`: the largest single record, `MAX_PAYLOAD` plus the worst-case
  header (3), delta (5) and length (3) bytes, so 523 bytes by default. It sizes the
  dispatcher's staging buffer (§12.5) and is reported in `pinfo.max_record`.
* `EXTRACE_MAX_RESV`: the largest reservation, `MAX_RECORD` plus a worst-case `LOST`
  record (19 bytes) plus `EXTRACE_CFG_RESV_ALIGN_MAX − 1` alignment bytes (§8.14). A
  wrap gap is always shorter than it (§5.8), and the minimum ring size is a multiple
  of it (§5.10).

### 5.7 Payload per kind

#### 5.7.1 `INTS`

```
payload = uvarint(sign_mask) 1*16( uvarint(value_i) )
```

* `sign_mask` bit *i* = 1 means argument *i* is signed and zigzag-encoded.
* The argument count is implicit: the number of varints after `sign_mask` within `len`.
* Values may be up to 64 bits wide.
* `sign_mask` is computed **at compile time** from the C argument types using
  `_Generic`. At runtime it is an immediate constant.

#### 5.7.2 `PRINTF`

```
payload = uvarint(site_id) *( arg )
arg     = uvarint(u)                 ; class U32/U64/PTR/CSTR
        / uvarint(zigzag(s))         ; class S32/S64
        / 4OCTET                     ; class F32, little-endian IEEE-754 binary32
        / 8OCTET                     ; class F64, little-endian IEEE-754 binary64
        / uvarint(n) n*OCTET         ; class STR, n <= EXTRACE_CFG_STR_ARG_MAX
```

* `site_id` identifies the call-site descriptor that holds the format string, file,
  line and **argument class signature** (§10.2). The record itself carries no type
  tags. The host gets types from the descriptor.
* Argument classes are determined per argument at compile time with `_Generic`
  (§10.3).

#### 5.7.3 `BUF`, `STRUCT`

`payload = len*OCTET`, raw memory copied as is. For `STRUCT` the host decodes with the
declared type's layout. If `len < sizeof(type)` the struct was truncated. If
`len > sizeof(type)` the struct ends in a flexible array member, and the host
computes the element count from the extra bytes.

#### 5.7.4 `STR`

`payload = len*OCTET`, bytes of a string without the terminating NUL, truncated to
`EXTRACE_CFG_MAX_PAYLOAD`.

#### 5.7.5 `EXT`

`payload = uvarint(ext_type) ext-body`. `ext_type` 1 = `FRAG` (§5.9.6). Other values
are reserved, and unknown `ext_type` values must be skipped using `len`.

### 5.8 Padding and wrap

When a record does not fit between the current physical offset and the end of the
ring, the producer reserves the **gap plus the record** and places the record at
physical offset 0. Both advance the free-running `head` index (§6.6). The writer
fills the start of the gap with:

* gap = 1 byte: `0x00` (PAD1);
* gap ≥ 2 bytes: `0x04`, then `n` written as a varint of exactly `k` bytes, with
  `1 + k + n == gap` (PADN). `k` is the smallest length for which
  `len(uvarint(gap − 1 − k)) ≤ k`, and `n = gap − 1 − k`. The skipped bytes are not
  written, so a large gap costs at most 4 byte stores.

**Over-long PADN length.** Usually `k` is the minimal length of `n`. Two gap sizes
have no minimal encoding, because `n` would have to cross a varint length boundary:
130 bytes (`n = 127` written as `FF 00`) and 16 387 bytes (`n = 16 383` written as
`FF FF 00`). For these the writer emits `n` one byte longer than minimal, padded with
a redundant continuation byte. Decoders already accept over-long varints (§5.2), so
no special case exists on the reading side. Larger gaps are impossible because a gap
is always shorter than `EXTRACE_MAX_RESV` (§5.6).

A consumer reading from its physical offset up to the ring end therefore always sees
a well-formed record stream. Bulk dumps stay parseable without knowing the ring
geometry.

Event ID 0 is valid only as PAD1 (kind `NONE`, byte `0x00`) or PADN (kind `BUF`,
byte `0x04`). A decoder treats any other header with ID 0 as corruption.

**Padding at any record boundary.** Padding is not limited to the ring end. Three
producers of padding exist:

| Source | Form | Position |
|---|---|---|
| Wrap gap (above) | one `PAD1` or `PADN` | at the physical ring end |
| Payload alignment of a direct reservation (§8.14) | 1–7 `PAD1` bytes | immediately before the record header, after any `LOST` prefix |
| Cancelled direct reservation (§8.14) | the header becomes `PAD1` bytes and a 1-byte `SKIP` header; `tsd`, `len` and payload stay, so the record is skipped but its time delta is kept | where the record was |

Every decoder therefore follows one rule wherever a record may start: **skip any
run of `PAD1` and `PADN` records, then decode the next header.** Padding has no
timestamp, does not change `prev_ts` and is never delivered to sinks or counted as
an event. The bytes after a padding run are an ordinary record whose `len` gives the
payload exactly. A decoder never needs to know why the padding is there or how the
payload was aligned. The C record decoder (§12.5), the dispatcher's budget clamp
(§11.3), the host parser (§15.2) and the post-mortem tool (§15.3) all implement this
rule.

### 5.9 System event payloads

#### 5.9.1 `SYNC` (ID 1, INTS)

`ts_abs` (u32): the raw global timer value at reservation. `tsd` is always 0. The
dispatcher and the host place it on the 64-bit timeline using their own extended
reading of the same timer (§12.5, §15.5).

#### 5.9.2 `LOST` (ID 2, INTS)

`count` (u32): number of records dropped since the previous `LOST` report.
`ts_abs` (u32): raw global timer value at recovery; `tsd` of the `LOST` record is 0.
Optionally `bytes` (u32), the number of bytes those records would have occupied, when
`EXTRACE_CFG_LOST_BYTES=1`.

`LOST` carries an absolute time because a drop episode can last longer than a timer
wrap (the periodic `SYNC` records of that period were dropped too), and a delta
would then be ambiguous.

#### 5.9.3 `INFO` (ID 3, BUF)

The payload is a sequence of varint fields with an explicit field-count prefix, so
the format can grow:

```
uvarint(nfields)
uvarint(wire_version)       ; 1
uvarint(layout_version)     ; shared-memory layout version
uvarint(flags)              ; bit0 big-endian, bit1, bit2 reserved (0), bit3 ts_global
uvarint(ts_freq_hz)
uvarint(ring_id)
uvarint(epoch)
uvarint(ring_size)
uvarint(ev_count)           ; highest event ID + 1
uvarint(ptr_size)           ; bytes
uvarint(site_id_bits)       ; 16 by default
uvarint(build_id_len) build_id_len*OCTET
uvarint(name_len)     name_len*OCTET
uvarint(ftrace_base)        ; EXTRACE_CFG_FTRACE_BASE (5.9.7)
```

Decoders ignore fields beyond those they know.

#### 5.9.4 `CFG` (ID 5, INTS)

`mask_gen` (u32): the mask generation that is now in effect. The host uses it to
mark the exact point at which a configuration change took effect.

#### 5.9.5 `PANIC` (ID 6, INTS)

`reason` (u32), `pc` (uintptr, optional). Emitted only if space and state allow
(§8.10).

#### 5.9.6 `FRAG` (ID 7, EXT ext_type 1)

```
ext-body = uvarint(xfer_id) uvarint(orig_event_id) uvarint(total_len) uvarint(offset) bytes
```

Used by `EXTRACE_BUF_LARGE(EV, p, n)` for buffers larger than `MAX_PAYLOAD`. Each
fragment is an independent reservation, so other records may interleave. `xfer_id`
comes from a producer-local 32-bit counter (`frag_seq`), incremented under one short
critical section before the first fragment, so two transfers in flight at once, for
example of the same event and size from a thread and an interrupt, never share it.
The host reassembles fragments per (ring, epoch, `xfer_id`) in offset order.
Missing fragments, for example dropped on overflow, mark the buffer incomplete.

#### 5.9.7 `FTRACE` (ID 4)

Function entry tracing ([docs/ftrace.md](docs/ftrace.md)). The record carries
`addr = return address - ftrace_base`, where the return address is the one the
trampoline was called with. The host maps it to the function through the ELF.
Each patched function has its own flavor, which picks the shape:

```
%x21 uvarint(addr)                               ; MARK
%x20 tsd uvarint(addr)                           ; NONE
%x22 tsd len uvarint(0) uvarint(addr) a0..a(n-1) ; ARGS(n): INTS, unsigned
```

The short shapes are the only records whose layout depends on the ID: the
uvarint follows the kind's normal fields without a length field. This keeps a
call record at 3 bytes, against 6 for an `INTS` with the address. The mask bit
of ID 4 is the switch for every function, whatever its flavor.

`FTRACE_CALLER` (ID 8) has the same three shapes with a second uvarint after the
address, `caller = the caller's return address - ftrace_base`:

```
%x41 uvarint(addr) uvarint(caller)                     ; MARK
%x40 tsd uvarint(addr) uvarint(caller)                 ; NONE
%x42 tsd len uvarint(0) uvarint(addr) uvarint(caller) a0..a(n-1) ; ARGS(n)
```

Functions patched with a `CALLER` flavor write these. The producer builds them
only with `EXTRACE_CFG_FTRACE_CALLER=1`. A separate ID keeps every record
self-describing, whatever the flavor of the function that wrote it.

The flavor is in the patch: the patched entry calls the trampoline entry of the
flavor, and the trampoline has one body per shape that calls that shape's
emitter. No flavor is stored on the device, and the record path has no dispatch
([ftrace.md](docs/ftrace.md#trampoline-contract)).

### 5.10 Limits

| Item | Default | Limit |
|---|---|---|
| Events per image | catalog size, cap 1024 (`EXTRACE_CFG_MAX_EVENTS`) | `ev_count` ≤ 65 535, so IDs ≤ 65 534 (bitmaps are sized by the actual count) |
| Arguments per `INTS`/`PRINTF` | 16 | 16 (signature is 64 bits, 4 bits per arg) |
| Payload per record | 512 B | 64–65 535 B, and ≤ ring size / 4 |
| String argument | 64 B | `MAX_PAYLOAD` |
| Ring size | set per ring at init | **power of two**, ≥ 64, ≥ 4 × `EXTRACE_MAX_RESV`, ≤ 2^31 (§6.6) |
| Rings per system | – | 255: `ring_id` is 8 bits system-wide, 0–254; 255 is `EXTRACE_RING_ALL` |
| Rings per dispatcher | 16 (`EXTRACE_CFG_DISP_MAX_RINGS`) | 255 |
| Printf sites per image | 65 536 | widen `EXTRACE_CFG_SITE_ID_BITS` |

### 5.11 Worked examples

**NONE event**, ID 40, 300 ticks after the previous record:

```
hdr  = 40<<3 | 0 = 320  -> C0 02
tsd  = 300              -> AC 02
total: 4 bytes          (C0 02 AC 02)
```

**MARK event**, ID 40:

```
hdr  = 40<<3 | 1 = 321  -> C1 02
total: 2 bytes          (no timestamp, no length)
```

Every user event header takes 2 bytes, because user IDs start at 16. A 1-byte header
is only possible for system IDs 0–15.

**INTS event**, ID 20, args `(uint32_t)5, (int)-3`, delta 12:

```
hdr       = 20<<3 | 2 = 162     -> A2 01
tsd       = 12                  -> 0C
sign_mask = 0b10                -> 02
v0        = 5                   -> 05
v1        = zigzag(-3) = 5      -> 05
len       = 3                   -> 03
record: A2 01 0C 03 02 05 05    = 7 bytes
```

**PRINTF event**, ID 30, `"q=%u st=%d"` at site 0x01A3, args `(7u, -1)`, delta 2000:

```
hdr  = 30<<3 | 3 = 243          -> F3 01
tsd  = 2000                     -> D0 0F
site = 0x01A3 = 419             -> A3 03
a0   = 7                        -> 07
a1   = zigzag(-1) = 1           -> 01
len  = 4                        -> 04
record: F3 01 D0 0F 04 A3 03 07 01 = 9 bytes
```

### 5.12 Size comparison

Compared with a typical fixed design (8-byte header with 16-bit ID, 16-bit size and
32-bit timestamp; a 4-byte typed payload header; 4 bytes per integer; 8-byte record
alignment):

| Event | Fixed design | extrace (typical) | Saving |
|---|---|---|---|
| no payload | 8 B | 3–4 B | ~55% |
| marker | 8 B | 2 B | 75% |
| one u32 (small) | 16 B | 6–7 B | ~60% |
| four small ints | 32 B | 9–10 B | ~70% |
| printf with 2 small ints (formatted on device: ~30 chars) | ~48 B | 9 B | ~80% |
| 64-byte struct | 80 B | 68–69 B | ~14% |

### 5.13 Versioning

* `wire_version` (in `INFO` and bulk frames) increments on incompatible changes.
  Compatible additions use reserved kinds or EXT types, `INFO` fields and system
  IDs 10–15, which old decoders skip (`SKIP`, ID 9, was added this way: an old
  decoder skips it as an unknown ID and loses only its `tsd`). The `FTRACE` and `FTRACE_CALLER` short
  shapes (§5.9.7) belong to version 1: a decoder must know them to find the
  next record.
* `layout_version` (in shared-memory `pinfo`) covers the control block layout. A
  consumer refuses to attach to an unknown major layout and reports this.

---

## 6. Shared memory layout and ownership

### 6.1 Ring block

Each ring is one contiguous **ring block** in shared memory, aligned to 8 bytes:

```
 offset              writer  readers   content
 +0                  P       C, host   pinfo     (64 B, static after init)
 +64                 P       C         pstate    (32 B, dynamic)
 +96                 C       P         cstate    (32 B, dynamic)
 +128                C       P         mask_set  (4W B)
 +128 + 4W           C       P         mask_clr  (4W B)
 +data_off           P       C, DMA    data      (size B, power of two)
```

The control part is packed without padding: `data_off = 128 + 8W`, a multiple of
8. Fields of producer and consumer may lie next to each other, because no agent
caches the memory (§1.7). Correctness needs only one writer per 32-bit word
(§6.3 rule 1). The data area starts 8-byte aligned, which is what the direct
reservation API needs for payload alignment (§8.14, `EXTRACE_CFG_RESV_ALIGN_MAX ≤ 8`).

`W = pinfo.mask_words = ceil(ev_count / 32)`, taken from the **producer's** catalog.
Offsets are relative to the block base. Each side computes its own pointers from its
own mapping of the block base. **No pointers are ever stored in shared memory.**

The mask bitmaps are part of every ring block, so **each producer instance has its
own control bitmaps** (D17). Two cores running the same image use two ring blocks
and can be configured independently.

The block layout is therefore dynamic:

```c
#define EXTRACE_PINFO_OFF                0u
#define EXTRACE_PSTATE_OFF               64u
#define EXTRACE_CSTATE_OFF               96u
#define EXTRACE_MASK_SET_OFF             128u
#define EXTRACE_MASK_CLR_OFF(w)          (EXTRACE_MASK_SET_OFF + 4u * (w))
#define EXTRACE_CTL_SIZE(w)              (EXTRACE_MASK_SET_OFF + 8u * (w))
#define EXTRACE_RING_BLOCK_SIZE(data, w) (EXTRACE_CTL_SIZE(w) + (data))
/* sizing in a system memory map that does not know the producer's catalog: */
#define EXTRACE_RING_BLOCK_SIZE_MAX(data) \
    EXTRACE_RING_BLOCK_SIZE(data, (EXTRACE_CFG_MAX_EVENTS + 31u) / 32u)
```

The producer computes `data_off = EXTRACE_CTL_SIZE(EXTRACE_MASK_WORDS)` and, if the
configured data size is 0, uses the largest power of two that fits after the control
part. A block sized with `EXTRACE_RING_BLOCK_SIZE_MAX(data)` for a power-of-two
`data` therefore gets exactly `data`; the bitmap space a smaller catalog does not
need stays unused (at most `8 × (32 − W)` bytes). The block itself needs no
power-of-two size, only its data area. The producer writes `data_off`, `data_size`,
`mask_words` and `ev_count` into `pinfo`. The consumer derives every pointer from
those fields and checks them against the block size it was given.

### 6.2 Control structures

Every field that more than one agent touches after init is a naturally aligned
32-bit word, read and written with single 32-bit `volatile` accesses, so it is
single-copy atomic. `pinfo` also has byte and halfword fields. It is written once
per epoch before `magic` and read only as a seqlock snapshot (§7.6 A1), so those
fields are never accessed concurrently with their writer.

```c
#define EXTRACE_SHM_MAGIC   0x52545845u   /* "EXTR" little-endian */

struct extrace_shm_pinfo {                 /* producer-owned, written once per epoch */
    uint32_t magic;          /* written last during init                      */
    uint16_t layout_ver;     /* major << 8 | minor                            */
    uint8_t  wire_ver;
    uint8_t  flags;          /* EXTRACE_PI_BIG_ENDIAN, _TS_GLOBAL             */
    uint8_t  ring_id;        /* system-wide unique, 0..254                    */
    uint8_t  reserved0;
    uint16_t reserved2;      /* 0                                             */
    uint32_t data_off;       /* from block base                               */
    uint32_t data_size;
    uint16_t mask_words;     /* ceil(ev_count / 32), sizes mask_set/mask_clr  */
    uint16_t ev_count;       /* highest event ID + 1 of the producer image    */
    uint32_t max_record;     /* EXTRACE_MAX_RECORD of the producer image (§5.6) */
    uint32_t ts_freq;        /* global timer ticks per second                 */
    uint32_t reserved1;
    uint8_t  build_id[20];   /* GNU build-id (truncated/zero-padded)          */
    char     name[8];        /* instance name, e.g. "dsp0"; no NUL if 8 chars */
};

struct extrace_shm_pstate {                /* producer-owned, dynamic */
    uint32_t epoch;          /* incremented on every producer init            */
    uint32_t head;           /* published free-running index, record boundary */
    uint32_t lost_total;     /* cumulative dropped records (diagnostics)      */
    uint32_t mask_gen_ack;   /* last applied mask generation                  */
    uint32_t cmd_seq_ack;    /* last executed command sequence                */
    uint32_t hwm;            /* high-water mark of ring usage, bytes          */
    uint32_t status;         /* RUNNING, PANIC, ...                           */
    uint32_t publish_cnt;    /* diagnostics                                   */
};

struct extrace_shm_cstate {                /* consumer-owned, dynamic */
    uint32_t magic;          /* EXTRACE_SHM_MAGIC once the consumer has attached */
    uint32_t attached_epoch; /* epoch the consumer acknowledged                 */
    uint32_t tail;           /* consumed free-running index, record boundary    */
    uint32_t mask_gen;       /* seqlock generation of mask_set/mask_clr         */
    uint32_t notify_thr;     /* doorbell after this many newly published bytes; 0 = off */
    uint32_t publish_thr;    /* producer publish threshold hint; 0 = default    */
    uint32_t cmd_seq;        /* incremented after cmd_bits is updated (§7.7)    */
    uint32_t cmd_bits;       /* pending EXTRACE_CMD_FLUSH | _SYNC | _INFO | _THR */
};
```

`static_assert`s pin `sizeof` of `pinfo` to 64 bytes and of `pstate`/`cstate` to 32
bytes, and every field offset to its documented value. The structures carry no
`alignas`; their placement comes only from the `*_OFF` macros in §6.1. Both sides
compile the same `extrace_shm.h`, so the layout is part of the build contract.
`layout_ver` is 2.0. A consumer refuses to attach to a ring whose layout has a
different major version, and the reverse also holds (§5.13).

**Byte order (D18).** Shared memory is little-endian. `extrace_shm_ld32()` and
`extrace_shm_st32()` are plain `volatile` word accesses on little-endian agents and
add one byte swap (`EXTRACE_BUILTIN_BSWAP32`, a single `rev` on ARM) on big-endian
ones. The structures above describe the little-endian byte image. The producer
assembles `pinfo` in local memory as that image and copies it into the block with
aligned word stores, so no byte or halfword store ever reaches shared memory (§6.3
rule 1). The consumer copies it out the same way at A1. On a little-endian agent the
local image simply is the C structure. A big-endian agent packs and unpacks the byte
and halfword fields with shifts, in cold init and attach code. Ring data needs no
conversion: the record stream is bytes (§5.1).

### 6.3 Ownership rules

These rules are the foundation of correctness on non-coherent memory.

1. **Single writer per 32-bit word.** Every shared word has exactly one writer
   (the table in §6.1). Control fields are written with aligned 32-bit stores,
   never with byte or halfword stores, so no write can disturb a neighbouring field
   of the other agent, even on interconnects or ECC memories that turn sub-word
   writes into read-modify-write cycles.
2. **Writers barrier before publishing.** A writer issues the port's write barrier
   (`wmb`) between the stores that prepare data and the single store that publishes
   it (`head`, `magic`, `epoch`, `mask_gen`, `cmd_seq`, `attached_epoch`). Write
   buffers may otherwise make the publishing store visible first.
3. **Readers barrier after the publishing word.** A reader issues `rmb` between
   reading a publishing word and reading the data it guards.
4. **Readers never write words they do not own**, not even zero to "clear". The
   consumer never writes ring data.
5. **Ownership transfers only by protocol**. Example: during init the producer
   writes `pinfo` before publishing `magic`; the consumer reads it only after
   `magic` is valid.
6. **All shared control accesses are `volatile`**, so the compiler neither caches a
   value in a register across a poll nor splits or merges the access. Ring data is
   written with ordinary stores, which the barriers order (they are compiler barriers
   too, §13.2).
7. **Mapping.** Every agent maps the trace memory as normal, non-cacheable memory,
   not as device memory (§13.6). Device memory also works, but only with an
   aligned-only port `memcpy` (§13.3), and it forbids write merging and early write
   acknowledgement, which makes every byte store slower.
8. **Own words are stored from local mirrors.** An agent never reads back a shared
   word it writes, and never increments one in place. It keeps the value in local
   memory and stores the new value. Examples: `head`, `lost_total`, `hwm` and
   `publish_cnt` in the producer; `tail`, `mask_gen`, `cmd_seq` and the
   `mask_set`/`mask_clr` words in the consumer (§11.1). A read-back would be an
   unbuffered bus read. The only exceptions are deliberate and happen once per
   (re)start: a restarted consumer loads its mirrors from `cstate` when it resumes an
   epoch (§7.6 A2), and a re-initializing producer reads `pinfo.magic` and
   `pstate.epoch` to choose the next epoch (§7.6 I1).

### 6.4 Sizing example

| Producer catalog | `W` | Control overhead per ring |
|---|---|---|
| 1024 events (the cap) | 32 | 384 B (128 + 256) |
| 300 events | 10 | 208 B (128 + 80) |
| 60 events | 2 | 144 B (128 + 16) |

The data area is chosen per ring, for example 4–64 KiB. The overhead does not
depend on any cache-line size.

### 6.5 Directory (optional)

A system may place a **ring directory** at a well-known shared address so the
dispatcher and post-mortem tools can discover rings without platform-specific code:

```c
struct extrace_shm_dir {                   /* 8-byte aligned */
    uint32_t magic;          /* "EXDR" */
    uint16_t layout_ver;
    uint16_t nrings;
    uint32_t ring_off[EXTRACE_CFG_DIR_MAX_RINGS];   /* block offsets from dir base */
    uint32_t ring_size[EXTRACE_CFG_DIR_MAX_RINGS];  /* whole block sizes           */
};
```

The directory is static and written by whoever owns the system memory map: the boot
loader, the dispatcher, or a build-time constant image. Without a directory the
platform passes block addresses to `extrace_prod_init()` and `extrace_cons_attach()`
directly. An image that runs on several cores selects its block and `ring_id` at
runtime, for example from a core-ID register (§8.1).

### 6.6 Free-running indices

`head` (producer) and `tail` (consumer) are **free-running 32-bit byte indices**.
They start at 0 when an epoch starts, only move forward, and wrap naturally at 2^32.
Padding bytes at the ring end are counted like record bytes. The ring size is a
**power of two** (at least 64 and `4 × EXTRACE_MAX_RESV`, at most 2^31), so 2^32
is a multiple of it and every index maps to its physical offset with one AND:

```
mask     = size - 1
used     = head - tail                      (uint32_t arithmetic, correct across the wrap)
free     = size - used
empty    : used == 0
full     : used == size                     (the whole ring is usable)
valid    : used <= size                     (anything else is corruption or a stale epoch)
phys(i)  = i & mask
```

* **Cost.** Every index operation is a single add, subtract or AND. There are no
  locally tracked physical offsets and no division anywhere, including init.
  Arbitrary line-multiple sizes were evaluated in review round 3: they can be kept
  free on the per-record path, but only with a second number space in the producer
  (counters next to indices), an init-time division and extra test dimensions,
  which is not worth the sizing flexibility (§21.2, 3.6).
* **Sizing granularity** is the trade-off: data areas are 4, 8, 16, … KiB. The
  control part lies outside the data area, so the block size
  (`EXTRACE_CTL_SIZE + size`) needs no rounding.
* `size ≤ 2^31` keeps `used` unambiguous.
* Every index is a single 32-bit word, so a reader can never see a torn
  index/offset pair. Indices are compared only through `used`-style differences,
  never with `<`.
* **Stale-value filtering.** Indices are ordered, so the producer can reject a `tail`
  that is older than the one it has cached, or beyond `head`
  (`t − tail_cache > head − tail_cache`). Concurrent refreshes therefore never move
  the cached tail backwards (§8.4).
* **Validation at attach.** The consumer refuses a `pinfo.data_size` that is not a
  power of two (§7.6 A1).
* **Post-mortem.** A dump of the ring block is decoded from `tail & mask` to
  `head & mask` with no other state.
* **Stream position.** The consumer's 64-bit stream position (§11, bulk frame `pos`)
  is `tail` extended to 64 bits. It starts at `tail` on every attach, including a
  resume (§7.6 A3), and every release advances both by the same amount, so its low
  32 bits always equal `tail`. It is consumer-side only and never stored in shared
  memory. After a dispatcher restart the host re-extends it from the last `pos` it
  saw for that ring and epoch (§12.7).

The PAD rule (§5.8) is unchanged: a record never straddles the physical ring end.

---

## 7. Coherency, ordering and lifecycle protocol

### 7.1 Port primitives used

| Primitive | Semantics required (§13 has details) |
|---|---|
| `extrace_port_wmb()` | Prior stores are ordered before later stores, as observed by every other agent. Ordering is enough; completion is not required. ARM: `DMB` (full system on Cortex-M; `DMB OSHST` or `DMB ST` on R/A). RISC-V: `fence w,w`. |
| `extrace_port_rmb()` | Prior loads are ordered before later loads, and later loads are not satisfied speculatively from before the barrier. ARM: `DMB` (`DMB LD` on v8). RISC-V: `fence r,r`. |
| `extrace_port_mb()` | Full ordering of prior loads and stores before later loads and stores. ARM: `DMB`. RISC-V: `fence rw,rw`. |
| `extrace_shm_ld32(p)` / `extrace_shm_st32(p, v)` | Library helpers: aligned 32-bit `volatile` load and store of a shared control word, little-endian (§6.2). A plain access on little-endian agents, plus one byte swap on big-endian ones. |

All barriers are also compiler barriers. With non-cacheable memory no barrier has
to wait for cache maintenance, so ordering barriers (`DMB`, `fence`) are sufficient
everywhere except before a doorbell, where the notify hook supplies a completion
barrier (§7.2).

### 7.2 Producer publish sequence

```
P1  write records for indices [from, to)             (ordinary byte/word stores)
P2  wmb()                                             (data ordered before the index store)
P3  pstate.head = to ; pstate.hwm/... updated         (one 32-bit index store)
P4  if notify condition: extrace_port_notify(ring_id)  (doorbell; hook completes P3 first)
```

P2 is the critical ordering point. The consumer must never be able to see the new
`head` before the data. A doorbell is often a write to a device register in another
part of the interconnect, so an ordering barrier is not always enough there.
`extrace_port_notify()` therefore starts with a completion barrier (`DSB` on ARM,
`fence w,o` or `fence rw,rw` plus the platform rule on RISC-V), so the consumer that
reacts to the doorbell sees the new `head`. Doorbells are rate-limited, so the
heavier barrier is rare.

### 7.3 Consumer read sequence

```
C1  e = pstate.epoch ; rmb() ; h = pstate.head        (epoch first, §7.6)
C2  rmb()                                             (index read before data reads)
C3  validate: e == attached_epoch, h - tail <= size, ... (§17)
C4  read / parse / DMA the window [tail, h)           (record path: word-wise copy first, §12.5)
C5  mb()                                              (all reads of the window complete)
C6  re-check pstate.epoch == e   (if changed: discard results, re-attach, §7.6)
C7  cstate.tail = t' (index at the end of the released part) ; wmb()
```

C5 ensures the producer cannot overwrite data the consumer is still reading. Bulk
sinks return only after any DMA of the window has completed (§12.6), so C5–C7 also
order DMA reads. The DMA engine reads memory directly, and memory always holds
the published data, so no step is needed to make the data visible to DMA.

The record path also re-checks the epoch after each staged copy, before decoding it
(§12.5), so bytes that a restarted producer has overwritten never reach a record
sink. The check is sound because the producer stores the new `epoch` (I3) before
any data of the new epoch: a copy that contains even one new byte is followed, after
`rmb`, by an epoch load that returns the new value.

### 7.4 Producer reading the consumer tail

```
R1  if cstate.magic != MAGIC or cstate.attached_epoch != my_epoch:  t = 0 (see 7.6)
    else: rmb(); t = cstate.tail
R2  mb()     (the tail read completes before stores into freed space)
R3  in CS: if t - tail_cache <= head - tail_cache: tail_cache = t   (else stale/invalid: ignore)
```

This runs only on the reservation slow path (§8.4) and in `poll`. It never runs on
the fast path. R3 uses the ordering of free-running indices (§6.6): a value older
than the cached tail, or one beyond `head`, is never accepted. An invalid value is
also counted in the producer statistics.

### 7.5 Mask update (seqlock across agents)

Consumer (writer):

```
W1  c->mask_gen += 1 (odd) ; cstate.mask_gen = c->mask_gen ; wmb     (local mirror, §6.3 rule 8)
W2  update the local mirrors of mask_set / mask_clr ; store the changed words ; wmb   (§11.1)
W3  c->mask_gen += 1 (even) ; cstate.mask_gen = c->mask_gen ; wmb
W4  optional: extrace_port_notify_producer(ring_id)
```

Producer (reader, in `extrace_prod_poll()`, which runs in one context only, §8.10):

```
M0  if cstate.magic != MAGIC or cstate.attached_epoch != my_epoch: return
M1  g1 = ld(cstate.mask_gen) ; if g1 odd or g1 == mask_gen_seen: return
M2  rmb
M3  for each word w: local.mask[w] = (defaults.w[w] & ~clr[w]) | set[w]     (§4.5)
M4  rmb ; g2 = ld(cstate.mask_gen)
M5  if g2 == g1: mask_gen_seen = g1 ; pstate.mask_gen_ack = g1 ; emit CFG(g1)
    else: leave mask_gen_seen unchanged, so the next poll recomputes
```

M3 writes word by word into the live local mask while trace calls may run
concurrently. That is harmless. A concurrent caller sees either the old or the new
state of each event's bit, and the enable state is advisory by nature. No temporary
buffer is needed.

M0 is essential. Before the consumer has attached to the current epoch, `cstate`
and the bitmaps may hold garbage (first boot) or the settings of a previous image
(§7.6). The producer runs on its catalog defaults until a consumer has attached.
The same guard protects command processing (§7.7).

### 7.6 Lifecycle and epochs

**Requirement.** The producer and the consumer initialize, re-initialize and run
completely asynchronously, on different cores, in any order, at any time and at any
relative speed, including both at once. Re-initialization of one side may happen
while the other side is in any step of its own init, attach, publish or read
sequence. No sequence of these events may make either side block, deliver a corrupt
record, apply garbage configuration, or overwrite memory it does not own.

**Memory assumption.** At the moment either side starts its (re)initialization,
each ring block is in one of two states:

* **random**: contents after power-up, arbitrary bit patterns in every field; or
* **consistent**: the state left by the protocol itself, with every completed
  sequence fully visible and no half-written structure. An init or attach is not
  abandoned halfway. A side re-initializes only from a point between its own
  protocol sequences.

Anything else, such as a core reset in the middle of a shared-memory sequence, is
out of scope. The protocol still never hangs in that case, but recovery is not
guaranteed until the next init. The protocol achieves this without locks or shared atomics. It relies on
three tools only:

1. **Single writer per word** (§6.3), so no store of one side can be lost or torn by
   the other side.
2. **Single-word fields.** Every value that is read across agents is one aligned
   32-bit word and therefore never torn. Multi-word data (`pinfo`, the bitmaps) is
   protected by a seqlock-style read: a generation word is read before and after.
3. **Epochs.** A producer-owned 32-bit `epoch` changes on every producer init and
   never repeats a value the consumer has acknowledged. Everything the consumer
   writes is tagged with the epoch it belongs to, and the producer ignores
   anything tagged with another epoch.

**Producer init** (`extrace_prod_init`, thread context):

```
I0  flags = irq_save()
    if inflight != 0 or publishing:                              (a preempted emit or publish)
        irq_restore(flags); return -EXTRACE_EBUSY
    READY = 0 ; local mask = 0                                   (new emits drop silently)
    irq_restore(flags)
I1  old = pstate.epoch ; if pinfo.magic != MAGIC: old ^= extrace_port_ts()
    new = old + 1 ; while new == 0 or new == cstate.attached_epoch: new++
    s0 = ld(cstate.cmd_seq)
I2  pinfo.magic = 0 ; wmb                                   (consumers stop trusting the block)
I3  store the local pinfo image word by word (§6.2) ; wmb
    pstate = { head: 0, lost_total: 0, cmd_seq_ack: s0, ..., status: RUNNING } ; wmb
    pstate.epoch = new ; wmb                               (epoch last: §7.3 C1 reads it first)
I4  pinfo.magic = MAGIC ; wmb
I5  local: head = committed = published = 0 ; tail_cache = 0 ; epoch = new
           mask_gen_seen = 0 ; cmd_seq_seen = s0 ; attached_seen = 0 ; lost_pending = 0
I6  flags = irq_save() ; local mask = defaults.w (§4.5) ; READY = 1 ; irq_restore(flags)
    emit INFO, SYNC ; publish ; extrace_port_notify(ring_id) once, if the port has it
```

Until the consumer acknowledges the new epoch, the producer assumes `tail == 0`. It
can fill the ring once and then drops new records with exact accounting. It never
overwrites anything the consumer could be reading in the new epoch.

`reserve()` checks `READY` inside its critical section and fails without counting
a loss while it is clear. I0 refuses to re-initialize while a reservation is in
flight (a preempted thread between reserve and commit) or a publish is running, so
no stale writer and no stale `head` store can land in the new epoch. The one
remaining window, a reservation slow path that has left its critical section to read
`tail`, re-checks `READY` and the epoch when it re-enters and gives up (§8.4).
`cmd_seq_ack` starts at the current `cmd_seq`, so commands posted before the restart
count as done; the new epoch emits `INFO` and `SYNC` anyway, and the consumer's
thresholds are reloaded after attach (§8.10). The doorbell at I6 lets a dispatcher
that sleeps until notified discover the new epoch. I1 picks an epoch that is never 0 and never equal to the epoch
the consumer last acknowledged, so a stale `cstate` can never look attached to the
new epoch. After a cold boot with garbage memory the timer value mixed into
`old` makes the result unpredictable as well.

**Consumer attach / re-attach** (`extrace_cons_attach`, and also on epoch change;
the consumer of a ring runs in one context, normally the dispatcher loop):

```
A1  seqlock read of the producer's identity:
        e1 = ld(pstate.epoch) ; rmb ; m1 = ld(pinfo.magic) ; rmb
        snap = word copy of pinfo (§6.2) ; rmb
        m2 = ld(pinfo.magic) ; rmb ; e2 = ld(pstate.epoch)
    require m1 == m2 == MAGIC and e1 == e2, else return -EXTRACE_ENOTREADY (retry later)
    validate snap: layout/wire versions (layout major 2, §6.2), mask_words == ceil(ev_count/32),
        data_off == EXTRACE_CTL_SIZE(mask_words), data_size a power of two ≥ 64,
        data_off + data_size <= block_size, 4 × max_record <= data_size, ring_id < 255
        ; else -EXTRACE_EVERSION / _ECORRUPT
    (max_record > EXTRACE_CFG_DISP_STAGE: attach for bulk sinks only, no record decoding, §12.5)
    e = e1
A2  if cstate.magic == MAGIC and cstate.attached_epoch == e:      -- resume (consumer restarted)
          tail = cstate.tail ; mask_gen, cmd_seq, cmd_pending, mask_set/mask_clr mirrors
                = from cstate   (loaded once)
          rmb ; h = ld(pstate.head)                               (no older than e1)
          if h - tail > size: if the producer moved (§17 re-check): -EXTRACE_ENOTREADY (retry)
                              else tail = h (count error)
    else:                                                         -- new epoch: indices start at 0
          tail = 0
          if cstate.magic != MAGIC or the image changed (build ID or ev_count differs
             from the consumer's previous snapshot of this block, or there is none):
                mask_gen = 2 (mirror and cstate) ; clear mask_set/mask_clr (mirrors and shared)
                notify_thr, publish_thr = defaults
                cmd_bits = 0 ; wmb                               (all before A3)
A3  cstate.tail = tail ; wmb
    cstate.attached_epoch = e ; cstate.magic = MAGIC ; wmb     (publishes the attach)
    local: epoch = e ; stream_pos = tail ; pinfo = snap
A4  report "ring attached (epoch e)" to the dispatcher, which emits an INFO frame to bulk sinks
```

A1 is a seqlock with `pinfo.magic` as the "write in progress" flag and
`pstate.epoch` as the sequence. The producer clears `magic` (I2) before it touches
`pinfo` and changes `epoch` (I3) before it sets `magic` again (I4). A copy that
overlaps any part of I2–I4 therefore sees either `magic == 0` or two different
epochs, and is retried.

#### 7.6.1 Interleaving analysis

Every row is safe for every relative timing of the steps involved.

| # | Interleaving | Outcome |
|---|---|---|
| 1 | Producer up first, consumer later | Producer runs with defaults and `tail = 0`, fills the ring at most once, counts drops. Consumer attaches with `tail = 0` and reads from `INFO`/`SYNC` onwards. Overrides apply once A3 is visible (§7.5 M0), and the producer then loads the consumer's thresholds (§8.10). |
| 2 | Consumer up first | A1 fails (`magic` invalid, or garbage that fails validation). The consumer retries on each poll or doorbell and attaches once I4 is visible. The consumer writes nothing to `cstate` before a successful A1. |
| 3 | Both start at once | A1 overlaps I2–I4: the seqlock read fails and is retried. A1 completes before I2 (old garbage that passes validation, practically impossible): the consumer attaches to a stale epoch, the first C1 sees the new epoch, and it re-attaches. |
| 4 | Consumer attach (A2–A3) races producer reads (R1–R2, M0) | The consumer writes `tail` and the bitmaps before `attached_epoch` (A2 → A3), and the producer reads `attached_epoch` before `tail`/bitmaps with `rmb`. The producer sees either "not attached" (uses `tail = 0`, defaults) or a fully initialized `cstate`. |
| 5 | Producer restarts while the consumer is attached | I2 → I4 run while the consumer peeks: C1 reads `epoch` before `head` and the producer writes `head = 0` before `epoch` (I3), so the consumer sees either the old epoch (validation of `h − tail` fails, or the window is empty) or the new epoch (re-attach). C6 discards a window whose epoch changed during processing. Stale `cstate` contents are ignored by the producer because `attached_epoch` cannot equal the new epoch (I1). |
| 6 | Producer restarts while the consumer is in A1–A3 | The consumer either fails A1 (retry) or attaches to the old epoch, then detects the new one at the next C1 and re-attaches. Producer reads during this time see a `cstate` with a non-matching epoch and are ignored. |
| 7 | Consumer re-initializes, producer keeps running | `cstate` still says "attached to the current epoch": resume at `cstate.tail` without loss or duplication; overrides survive. |
| 8 | Producer restarts with a different image | New build ID or `ev_count`: the consumer clears the overrides before attaching, so IDs from the old image never configure the new one. `mask_words` and the bitmap layout come from the new `pinfo`. |
| 9 | Producer stops (or is reset) and stays down, then re-initializes much later | The consumer keeps the attached state and sees no new data. `head` is only ever published after a write barrier behind the data (P1–P3), so everything up to the visible `head` is valid. A later producer init is case 5. While I2–I4 of that init run, the consumer's error path re-reads `pinfo.magic` (§17) and waits. |
| 10 | Consumer stops forever | The producer fills the ring once and counts drops. It never blocks and never writes consumer-owned words. |
| 11 | Producer restarts while a bulk sink transmits a window | The sink returns only when its transfer is complete (§12.6). C6 then sees the new epoch, the window is not released into the new epoch, and the ring re-attaches. The next frame carries `EPOCH_NEW` (the one unavoidable loss, below). |
| 12 | Producer re-init while one of its own threads is preempted inside an emit or a publish | I0 returns `-EXTRACE_EBUSY`. A thread preempted in the reservation slow path, outside the critical section, fails its reservation when it re-enters (§8.4). The caller retries after that thread runs, or re-initializes only from boot or from a quiesced state. |
| 13 | Random memory on both sides (power-up) | Producer: `old` is randomized by the timer, `new ≠ cstate.attached_epoch`, and garbage `cstate` is never applied, because M0 requires the current epoch. Consumer: before the producer's I4, A1 sees garbage `magic`/fields and fails validation. In the practically impossible case that garbage passes, the consumer attaches to a garbage epoch and re-attaches at its first C1 after the producer's init (case 3). |
| 14 | Repeated re-initialization of either side, in any order, back to back | Each producer init creates a fresh epoch distinct from the last acknowledged one. Each consumer attach either resumes the current epoch or starts it at 0. Any interleaving reduces to cases 3–7. |

**The one unavoidable loss.** When the producer restarts while a window of the old
epoch is still being read, the new epoch may overwrite those bytes. The epoch
re-check after each staged copy (§7.3, §12.5) keeps them from reaching record sinks,
and C6 keeps the window from being released into the new epoch. A bulk sink that has already
transmitted the window may have sent partly overwritten bytes. The next frame of that
ring then carries `EPOCH_NEW`, and the host treats the tail of the previous epoch
as untrusted: it parses up to the first malformed record (§15.2).

Mask overrides in `cstate`/`mask_*` persist across restarts of the **same image**,
because they describe the host's intent for that instance. The producer applies them
as soon as the consumer has acknowledged the new epoch: `mask_gen_seen` resets to 0
at init, and any valid even generation is then applied (§7.5, M0). If the core is
restarted with a **different image** (new build ID or event count), event IDs no
longer mean the same thing, so the consumer clears the overrides at A2 and the new
image starts from its own defaults. The consumer keeps the last `pinfo` snapshot per
block for this comparison. After a consumer restart there is no snapshot, and the
overrides are kept only if `cstate` was already attached to the current epoch.

### 7.7 Commands

`cstate.cmd_bits` is a bitmask of **pending** requests. Bits accumulate until the
producer has acknowledged them, so a request is never lost when several are posted
before the producer polls.

Consumer (`extrace_cons_cmd(c, bits)`):

```
K1  if pstate.cmd_seq_ack == c->cmd_seq: pending = 0         (everything posted is done)
K2  pending |= bits ; cstate.cmd_bits = pending ; wmb
K3  c->cmd_seq += 1 ; cstate.cmd_seq = c->cmd_seq ; wmb ; optional notify_producer
```

Producer (in `extrace_prod_poll()`, after the M0 guard):

```
X1  s = ld(cstate.cmd_seq) ; if s == cmd_seq_seen: return
X2  rmb ; b = ld(cstate.cmd_bits)
X3  execute every action in b
X4  cmd_seq_seen = s ; pstate.cmd_seq_ack = s
```

The producer reads `cmd_seq` before `cmd_bits`, so the bits it reads include every
request posted up to `s`. A bit posted after `s` may be executed early and executed
again at the next poll. Duplicates are harmless because every action is idempotent:

| Command | Producer action |
|---|---|
| `FLUSH` | publish all committed data now |
| `SYNC` | emit `SYNC` |
| `INFO` | emit `INFO` (e.g. the host connected late) |
| `THR` | reload `publish_thr` and `notify_thr` from `cstate`; the producer also does this on its own at the first `poll` after a consumer attaches to its epoch (§8.10) |

---

## 8. Producer design

### 8.1 Producer-local state

Producer state lives in **private** memory, ideally TCM or another fast local RAM
(`EXTRACE_PORT_HOT_DATA` attribute). It is never shared.

```c
struct extrace_prod {
    /* ---- hot: read by every EXTRACE_* call ---- */
    uint32_t mask[EXTRACE_MASK_WORDS];     /* sized by the catalog (EV_COUNT) */

    /* ---- reservation state: modified only inside the critical section ---- */
    uint32_t head;            /* next reservation index (free-running)         */
    uint32_t committed;       /* index: end of contiguous fully-written prefix */
    uint32_t tail_cache;      /* last accepted consumer tail index             */
    uint32_t lost_pending;    /* drops not yet reported in-band                */
    uint32_t last_ts;         /* raw 32-bit timer of last ts-carrying record   */
    uint8_t  inflight;        /* reservations not yet committed                */
    uint8_t  publishing;      /* a publish is running (any context)            */
    uint8_t  publish_again;   /* a context asked for publish while busy        */
    uint8_t  flags;           /* READY, PANIC                                  */

    /* ---- warm/cold ---- */
    uint32_t published;       /* last index written to pstate.head             */
    uint32_t notified;        /* published index at the last doorbell          */
    uint32_t publish_thr, notify_thr;
    uint32_t lost_total, epoch, mask_gen_seen, cmd_seq_seen, hwm;
    uint32_t publish_cnt;     /* mirrors of pstate words: stored, never read back */
    uint32_t frag_seq;        /* xfer_id of the last EXTRACE_BUF_LARGE (§5.9.6) */
    bool     attached_seen;   /* poll saw a consumer attached to this epoch    */
    uint32_t last_sync_ts;    /* raw 32-bit timer of the last SYNC             */
    uint32_t sync_period;     /* ticks, computed at init (§8.7)                */
    uint8_t *data;            /* producer view of data[]                       */
    uint32_t size, mask;      /* power of two; mask = size - 1 (§6.6)          */
    struct extrace_shm_pinfo  *pi;
    struct extrace_shm_pstate *ps;
    const struct extrace_shm_cstate *cs;
    const uint32_t *mset, *mclr;
};

extern struct extrace_prod extrace_producer;       /* the one producer of this instance */
```

**One producer per firmware instance.** extrace targets AMP systems: each core runs
one firmware instance, and SMP subsystems are not supported (§1.3). An image
therefore has exactly one statically allocated `extrace_producer`. Its address is a
link-time constant, so the fast path does no pointer chasing and needs no CPU index.

**Several instances of the same image.** When two or more cores run the same image,
each core has its own private copy of `extrace_producer` (separate data memory,
TCM, or a per-core data load address, as the platform arranges for any global).
Each instance passes its own ring block, `ring_id` and name to `extrace_prod_init()`,
typically chosen at runtime from a core-ID register. Each ring block carries its own
control bitmaps, and each instance keeps its own effective mask, so instances are
configured independently (D17, §9). They share one metadata file because the build
ID is the same, and the host tells them apart by `ring_id` and name.

### 8.2 Fast path: enabled check

```c
static inline bool extrace_ev_on(uint32_t id)
{
    return (extrace_producer.mask[id >> 5] >> (id & 31)) & 1u;
}

#define EXTRACE_ON(EV) \
    (EXTRACE_CT_##EV && unlikely(extrace_ev_on(EXTRACE_EV_##EV)))
```

`id` is a compile-time constant, so this compiles to one load from a fixed address,
one bit test and one conditional branch. On most targets that is 2–4 instructions and
6–14 bytes of code. When `EXTRACE_CT_##EV == 0` the whole statement disappears.

Every API macro has the same shape:

```c
#define EXTRACE_INTS(EV, ...)                                               \
    do {                                                                    \
        if (EXTRACE_ON(EV)) {                                               \
            EXTRACE_CHECK_KIND(EV, EXTRACE_K_INTS); /* static_assert */     \
            /* all-32-bit signature: uint32_t[] + 32-bit emitter */      \
            extrace_emit_ints32(EXTRACE_HDR(EV, EXTRACE_K_INTS),            \
                                EXTRACE_SIGN_MASK(__VA_ARGS__),             \
                                (const uint32_t[]){                         \
                                    EXTRACE_AV32_LIST(__VA_ARGS__) },       \
                                EXTRACE_NARGS(__VA_ARGS__));                \
        }                                                                   \
    } while (0)
```

(The 64-bit selection of §8.12 is omitted here for brevity.)

* **Arguments are evaluated only when the event is enabled.** Side effects in trace
  arguments are therefore conditional. This is documented, and it is the reason for
  `EXTRACE_ENABLED(EV)` (an alias of `EXTRACE_ON`), which guards expensive preparation:

  ```c
  if (EXTRACE_ENABLED(TX_DONE)) { struct tx_done_info i; fill(&i); EXTRACE_STRUCT(TX_DONE, &i); }
  ```

* `EXTRACE_HDR(EV, K)` is a compile-time constant `(id << 3) | K`, and so are its
  varint length and bytes.
* The emitters are out-of-line (`__noinline`, §13.7) and shared by all call sites.
  This keeps code size per site small. Specialized fast emitters exist for the most
  common shapes: `extrace_emit_none`, `extrace_emit_mark`, `extrace_emit_u32x1`,
  `extrace_emit_u32x2`. The macros pick them at compile time based on argument count
  and types.

### 8.3 Reserve / write / commit

```
emit(hdr, kind, payload-descriptor):
    plen   = payload_size(desc)                        -- outside CS
    fixed  = len(hdr) + (kind has len ? len(plen) : 0) + plen
    r      = reserve(fixed, kind != MARK)               -- CS #1
    if !r.ptr: return
    if r.pad:   write_pad(r.pad_ptr, r.pad)             -- outside CS, at the old ring end
    p = r.ptr                                           -- record start (offset 0 after a wrap)
    if r.lost:  p = write_lost(p, r.lost, r.ts); r.tsd = 0   -- LOST(count, ts_abs), tsd 0
    p = put_uv(p, hdr)
    if kind != MARK: p = put_uv(p, r.tsd)
    if kind has len: p = put_uv(p, plen)
    p = encode_payload(p, desc)                         -- direct into ring, no wrap checks
    commit(urgent)                                      -- CS #2
```

All critical sections in this document use the port's flags-returning API (§13.2):

```c
unsigned long flags;

flags = extrace_port_irq_save();    /* mask IRQs, return the previous state */
/* ... */
extrace_port_irq_restore(flags);    /* restore exactly that state */
```

The caller owns `flags`, so sections nest naturally and work from contexts that
already run with interrupts disabled (ISRs, panic handlers, code under an outer
lock): restoring never re-enables interrupts that were off on entry.

### 8.4 Reservation algorithm

```c
/* head, tail_cache: free-running indices; off = head & mask (§6.6). */
reserve(fixed, want_ts):
    flags = extrace_port_irq_save()
    if !READY: extrace_port_irq_restore(flags); return FAIL   /* init/panic, not counted */
    if inflight == 255: lost_pending++; extrace_port_irq_restore(flags); return FAIL
    read_ts = want_ts || lost_pending                      /* LOST always gets a time */
    if read_ts: ts = extrace_port_ts(); tsd = ts - last_ts  /* uint32_t, §8.7 */
    else:       tsd = 0
    if lost_pending:                                       /* §8.6: LOST carries ts,  */
        need = lost_record_len(lost_pending, ts)           /* the record gets tsd = 0 */
             + fixed + (want_ts ? 1 : 0)
    else:
        need = fixed + (want_ts ? uv_len32(tsd) : 0)
    pad = fit(need)                  /* -> pad bytes (0 = no wrap) or FAIL; see below */
    if pad == FAIL:
        ep = epoch
        extrace_port_irq_restore(flags)
        t = read_tail(ep)                                  /* §7.4 R1–R2, IRQs on */
        flags = extrace_port_irq_save()
        if !READY or epoch != ep:                          /* re-initialized meanwhile */
            extrace_port_irq_restore(flags); return FAIL    /* not counted */
        accept t into tail_cache (§7.4 R3) ; recompute ts/need ; pad = fit(need)
        if pad == FAIL: lost_pending++; extrace_port_irq_restore(flags); return FAIL
    pad_off   = head & mask
    start_off = pad ? 0 : pad_off
    head     += pad + need
    last_ts   = read_ts ? ts : last_ts
    lost      = lost_pending; lost_pending = 0
    inflight++
    extrace_port_irq_restore(flags)
    return { ptr: data + start_off, pad_ptr: data + pad_off, pad, ts, tsd, lost }
```

`fit(need)` uses the free-running indices directly. The whole ring is usable, and
there is no special case for an empty ring or a tail at offset 0:

```
used     = head - tail_cache                     (≤ size by invariant)
end_room = size - (head & mask)                   (≥ 1)
pad      = need <= end_room ? 0 : end_room        (wrap: skip to physical offset 0)
if used + pad + need > size: FAIL
return pad
```

When a wrap happens, the reserving context owns the gap `[head & mask, size)` as well as
its record at offset 0, and writes the PAD into the gap outside the critical
section. The gap is committed and published together with the record, because both
lie in the same index range.

**Critical-section length**: timestamp read, a subtraction, the varint length of
the delta (`extrace_uv_len32`, §8.13), the fit test (two compares and an add), and
about five stores. That is about 20–40 instructions, and constant time on the fast
path. `MARK` and `NONE` records are also written inside this section (§8.13 rule 7),
which adds 3–10 instructions and removes their commit section. The slow path
re-enters the critical section once, after reading shared memory with interrupts
enabled.

**Code layout**: the emitters inline only the common case: `READY`, no pending
drops, fewer than 255 open reservations, and a record that fits without a wrap at
the cached tail. One out-of-line function runs everything else, the full algorithm
above, and also writes the wrap gap, the `LOST` record and the alignment bytes. The
fast path hands it the timestamp it has read, so both paths read the timer as the
algorithm does.

**Concurrent `tail_cache` refresh**: two contexts may refresh in either order. R3
(§7.4) accepts a value only if it is not older than the cached one and not beyond
`head`, so the cached tail never moves backwards, and a stale read just leaves it
unchanged. If the producer was re-initialized while the slow path was outside its
critical section (`READY` clear, or a new epoch), the reservation fails without
counting a loss, so neither the reservation nor a `tail` of the old epoch enters the
new one.

### 8.5 Commit and nesting

```c
commit(urgent):
    flags = extrace_port_irq_save()
    if --inflight == 0: committed = head
    pend = committed - published
    extrace_port_irq_restore(flags)
    if pend >= publish_thr or urgent: publish()
```

Thread `T` reserves A. ISR `I` interrupts, reserves B, writes B and commits B. At that
point `inflight` is back to 1, so `committed` does not advance. When `T` commits A,
`inflight` reaches 0 and `committed` jumps past both A and B. The consumer can never
see B without a fully written A in front of it.

```
 ring:  ... | A (T, in flight) | B (ISR, written+committed) | free ...
             ^committed          (published stays here until T commits A)
```

**Preemption.** On an RTOS a thread can be preempted between reserve and commit, and
that delays publication of everything reserved after it. It cannot corrupt anything.
The write phase is short and bounded (encoding at most `MAX_RECORD` bytes), so the
delay equals the scheduling delay of that thread. Mitigations, in order of preference:

1. The default. Accept the delay: data is published once the thread runs again.
2. `EXTRACE_CFG_WRITE_IN_CS_MAX = n`: records up to `n` bytes are encoded inside the
   critical section. This removes in-flight windows for the hottest small records at
   the cost of a slightly longer critical section (~10–30 extra instructions).
   `MARK` and `NONE` records are always written this way (§8.13 rule 7).
3. A port `extrace_port_irq_save()` that also disables preemption (scheduler lock)
   for thread context, with `extrace_port_irq_restore(flags)` undoing both from the
   saved flags. RTOS-specific.

`inflight` is a `uint8_t`. For emitters the depth equals the interrupt nesting depth
plus one. Direct reservations (§8.14) can stay open longer, so a reservation fails,
counted as lost, while 255 are open. The counter cannot overflow in any build.

### 8.6 Overflow and lost-event accounting

* When `fit` fails twice (before and after refreshing the tail), the record is
  dropped and `lost_pending++` is incremented **inside** the critical section. This
  is exact under nesting.
* The next successful reservation adds a `LOST` record before its own record **in
  the same reservation**. The `LOST` record carries the absolute 32-bit timer value
  (`tsd = 0`, §5.9.2), and the original record follows with `tsd = 0`. The report
  is therefore atomic with respect to other contexts, positioned exactly where the
  gap occurred, timestamped at the moment of first recovery, and it re-anchors time
  however long the drop episode lasted.
* `lost_total` is also accumulated and written to `pstate.lost_total` at publish time
  for diagnostics.
* Records that are too large (`plen > MAX_PAYLOAD`) are **truncated** to
  `MAX_PAYLOAD` for BUF/STR/STRUCT. The record's `len` reflects the truncated size,
  so the stream stays consistent and the host can detect truncation from the length.
  `INTS`/`PRINTF` are bounded by the argument count and the string length. When
  a small `MAX_PAYLOAD` cannot hold that bound, an oversized `INTS`/`PRINTF`
  record is dropped and counted like an overflow; otherwise the check compiles
  away. `EXTRACE_BUF_LARGE` fragments buffers instead.
* `MARK` events are also counted as lost when dropped.
* On overflow the producer calls `publish()` (when allowed in this context) and
  rings the doorbell regardless of the threshold, at most once per overflow episode,
  so a sleeping consumer wakes up.

### 8.7 Timestamps

* `extrace_port_ts()` reads the **SoC-global 32-bit free-running timer**, which all
  subsystems can access. The platform port supplies it. If the hardware counts down,
  the port inverts it. All rings therefore share one clock and one frequency.
* **32-bit only on the producer.** `extrace_port_ts()` returns `uint32_t`. A
  platform whose global counter is 64 bits wide returns its low word. The producer
  stores `last_ts` and `last_sync_ts` as 32-bit values and computes
  `tsd = ts - last_ts` with one 32-bit subtraction. There is no 64-bit arithmetic,
  no carry handling and no wrap counter in any critical section, which keeps the
  critical section short on 32-bit cores (Cortex-M/R, RV32).
* **Delta validity.** A 32-bit delta is exact while consecutive timestamped records
  of a ring are less than one wrap period apart: about 71.6 minutes with the
  typical 1 MHz (1 µs) global timer, 4.3 minutes at 16 MHz, 43 s at 100 MHz. The
  producer guarantees this:
  * **Periodic SYNC**: `poll` emits `SYNC(ts_abs)` when
    `ts - last_sync_ts >= sync_period`. `extrace_prod_init()` computes
    `sync_period = min(2^30, extrace_port_ts_freq() / 10)` ticks: at most a quarter
    of the wrap period, capped at 100 ms. A non-zero `EXTRACE_CFG_SYNC_PERIOD_TICKS`
    overrides it at compile time. At 1 MHz that is 100 000 ticks (100 ms). The
    integration guide requires calling `poll` at least once per
    `wrap period − sync_period`, which is trivially met by a 1–10 ms tick hook.
    At 1 MHz a delta between records 128 µs to 16 ms apart takes 2 bytes, and one
    up to 2.1 s takes 3 bytes.
  * **Loss**: if records, possibly including periodic `SYNC`s, were dropped, the
    next `LOST` record carries an absolute timestamp (§5.9.2), so no ambiguous delta
    is ever produced across a drop episode.
* **Common clock.** The low 32 bits are the same clock in every ring. The dispatcher
  and the host place each ring's absolute anchors (`SYNC`, `LOST`) onto one 64-bit
  timeline using their own 64-bit extended reading of the same timer (§12.5,
  §15.5), then accumulate deltas. Producers never coordinate and never see 64-bit
  time.

### 8.8 Publication policy

`publish()` makes `[published, committed)` visible (§7.2). Triggers:

| Trigger | Default |
|---|---|
| unpublished bytes ≥ `publish_thr` after a commit | `size / 4` (overridable by `cstate.publish_thr`) |
| `extrace_prod_poll()` with unpublished data | always |
| urgent record (`EXTRACE_*_URGENT` variants, `PANIC`, `LOST` episode start) | immediate |
| explicit `extrace_prod_flush()` | immediate |
| consumer `FLUSH` command | next poll |

Concurrency (publish runs with interrupts enabled):

```c
publish():
    flags = extrace_port_irq_save()
    if publishing: publish_again = 1; extrace_port_irq_restore(flags); return
    publishing = 1; from = published; to = committed
    extrace_port_irq_restore(flags)
    loop:
        if from != to:
            wmb ; st(pstate.head, to) ; update hwm/lost_total
            maybe_notify(to)                           /* completion barrier inside, §7.2 */
        flags = extrace_port_irq_save()
        published = to
        if publish_again and committed != published:
            publish_again = 0; from = published; to = committed
            extrace_port_irq_restore(flags); goto loop
        publishing = 0; publish_again = 0
        extrace_port_irq_restore(flags)
```

* A publish costs one write barrier and a few word stores, independent of how many
  bytes it publishes. There is no per-byte work.
* `EXTRACE_CFG_PUBLISH_IN_ISR=0` makes ISR-context commits skip the threshold publish.
  The next thread-context commit or `poll` performs it. With no cache maintenance a
  publish is short, so this option matters only when the barrier itself stalls for
  long on a slow interconnect. The decision uses `extrace_port_in_isr()`.
* **Doorbell**: `maybe_notify(to)` rings when `notify_thr != 0` and
  `to - notified >= notify_thr`. The consumer sets `notify_thr` to 0 when it
  polls on its own, which avoids interrupt storms, and to a byte count when it wants
  to sleep.

#### 8.8.1 Writing records to non-cacheable memory

Every producer store to the ring is a bus write, and every load from it is an
unbuffered bus read. The producer therefore writes **directly and store-only**.
The emitters encode straight into the ring, one byte store per encoded byte (§8.13).
Every byte is computed in registers and stored once, and nothing is read back.

**No read-modify-write anywhere.** Audit of every producer write path:

| Write | How it is written |
|---|---|
| Record header | Compile-time constant, 2–3 byte stores (§8.13 rule 1) |
| Delta, length, varint arguments | Unrolled encoder: each byte built in a register, stored once. The length is known from the sizing pass before the reservation and is never back-patched. |
| 64-bit varint | The continuation bit of byte 5 is set in a register (Appendix 22.A) |
| `BUF`/`STRUCT`/`STR` payloads | `extrace_port_memcpy` (§13.2), which never reads its destination |
| Wrap `PAD`, `LOST` prefix, alignment `PAD1` bytes | Plain stores (§5.8, §8.14) |
| Cancel of a direct reservation | `PAD1` bytes and a `SKIP` header written over the record header: stores only, no read |
| Direct reservation payload | The caller's stores. The API rules forbid reading back or modifying in place (§8.14). |
| Control words (`pstate`) | Stored from producer-local mirrors, never incremented in place (§6.3 rule 8) |
| `pinfo` (init only) | Assembled locally as the little-endian image, copied with aligned word stores (§6.2) |

The encoders follow the rule "write once, never read back" (§8.13 rule 10), and
the release-check build verifies it: the encoder probes must contain no load from
the destination. The wbsim port can trap loads from the ring in producer context
(§18.1).

**Staging (documented, not implemented).** An alternative was evaluated: encode the
record into a fixed-size buffer on the stack, then copy it to the ring with aligned
halfword and word stores.

**Evaluation** (review round 4). A model of the number of shared-memory stores
needed to write `r` bytes at a random byte offset, with aligned byte, halfword and
word pieces when staged:

| Record bytes | Direct stores | Staged stores (average) | Saved |
|---|---|---|---|
| 2 (`MARK`) | 2 | 1.5 | 25 % |
| 4 (`NONE`) | 4 | 2.25 | 44 % |
| 7 (`INTS`, 2 small args) | 7 | 3.0 | 57 % |
| 12 | 12 | 4.25 | 65 % |
| 32 | 32 | 9.25 | 71 % |

Measured cost (clang 18, `-O2`, Cortex-M4 and RV32 with Zbb and Zce) for the
7-byte record:

* Encoding into the stack buffer costs the same as encoding into the ring, about
  20 instructions.
* The aligned copy adds 25–40 instructions, depending on the offset.
* Code grows from 276 B to 740 B on Cortex-M4 and from 304 B to 474 B on RISC-V for
  this emitter.

Saving four bus stores therefore costs about 30 extra instructions. Staging pays
off only when each saved store **stalls the core** for more than roughly 8–10 cycles:

* **Posted writes absorb direct stores.** Cores whose write buffer accepts a burst of
  stores without stalling see direct writes almost free. Examples: Cortex-M7 and
  Cortex-R with store buffers and merging of Normal Non-cacheable writes, and most
  application-class RISC-V cores. Direct is faster there.
* **Shallow buffer, slow interconnect.** A core with a one-entry write buffer
  (Cortex-M4) writing to shared memory behind bridges, at 10 or more cycles per
  write, stalls on every byte store after the first. Staging is faster there for
  records of 4 bytes and more.
* **Header-only staging is not worth it.** A header is 4–6 bytes (2-byte
  pre-encoded `hdr`, 1–3-byte delta, 1-byte length). It saves 2–3 stores for a
  copy that costs about as much as the record copy above. For large payloads it
  saves nothing, because the payload copy uses `memcpy`, which already writes words
  after aligning the destination. The header involves no bit manipulation in shared
  memory that staging could avoid.
* **Payload-heavy records** (`BUF`, `STRUCT`, `STR`, direct reservations) already go
  through `memcpy`, or through the caller's own stores into the reservation, so
  they gain nothing from staging.

**Decision.** Staging is **not implemented**, not even as a compile-time option.
Direct, store-only writes are the only producer path. The evaluation is kept here
for the case that measurements on a real platform show expensive shared stores. The
port conformance test reports the stall per shared byte store for that purpose
(§13.5). Should staging ever be added, these are the constraints:

* stage only emitter records of up to about 16 bytes;
* assemble records of up to 8 bytes in two registers rather than a stack buffer, so
  the copy is 1–3 stores with no loads;
* write larger records and direct reservations directly;
* never stage just the header.

### 8.9 Supported execution contexts

* **Thread context and regular interrupts**, including nested interrupts of any
  maskable priority. The port's critical section must mask every interrupt source
  whose handler may call extrace (§13.5).
* **Other cores** never share a ring: one ring and one producer per core (D1). A
  firmware instance runs on exactly one core; thread migration between cores does
  not exist in the supported AMP model.
* **Non-maskable contexts are not supported**: NMI, and FIQ when it is used as a
  non-maskable source. Such handlers must not call extrace. Debug builds can assert
  this when the port provides a context query.

### 8.10 Poll, flush, panic

```c
void extrace_prod_poll(void);        /* one context only: e.g. a bottom-half or work thread */
void extrace_prod_flush(void);       /* publish everything committed now */
void extrace_prod_panic(uint32_t reason, uintptr_t pc);
```

`poll`:
1. read `cstate` (a few word loads) and apply the M0 attach guard (§7.5). The first
   time the guard passes in an epoch (`attached_seen`), reload `publish_thr` and
   `notify_thr` from `cstate` as for a `THR` command (§7.7), so the consumer's
   thresholds also reach a producer that restarted after they were set;
2. apply a new mask generation (§7.5);
3. execute commands (§7.7);
4. read the timer and emit a periodic `SYNC` when `sync_period` has passed (§8.7);
5. publish if anything is unpublished.

When nothing has changed, `poll` costs about 6 shared-memory word loads and a
compare.

**Single calling context.** `poll` is **not reentrant**: the image calls it from
exactly one context, typically a bottom half, a work thread or the idle loop, that
also handles the periodic tick. The mask recompute (M3), the command sequence
(X1–X4) and the periodic `SYNC` keep state outside the critical section
(`mask_gen_seen`, `cmd_seq_seen`, `last_sync_ts`). Overlapping calls could leave a
partly applied mask that is marked as current. The consumer→producer doorbell
therefore does not call `poll` from its interrupt handler: the handler only wakes
the poll context. This keeps the interrupt short and needs no lock.

Re-initialization must not overlap `poll` either: `extrace_prod_init()` runs from the
poll context, or while that context is quiesced. Otherwise a `poll` preempted
between M3 and M5 could mark the old overrides as applied after I6 has reset the
mask to defaults. Debug builds assert this through the same `polling` flag.

* Emit calls and `extrace_prod_flush()` stay callable from any context and may
  interrupt `poll` at any point. They touch only critical-section state and
  `publish()`, which has its own re-entry handling (§8.8).
* With `EXTRACE_CFG_DEBUG=1`, `poll` sets and clears a `polling` flag and asserts
  that it is clear on entry, which catches a second calling context during testing.
* `extrace_prod_panic()` does not call `poll`, so it is unaffected.

`panic`: called from fatal-error handlers with interrupts disabled. It tries to emit
`PANIC(reason, pc)`, but only if `inflight == 0`. Otherwise the ring prefix is not
consistent beyond `committed`. It then publishes `committed` (not `head`) and sets
`pstate.status = PANIC`. In-flight records from the interrupted contexts are lost;
everything before them survives in shared memory for a post-mortem dump
(§15.3).

The interrupted code may itself have been inside `publish()`. Panic ignores the
`publishing` flag and runs the publish sequence (§7.2) directly from `published` to
`committed`, because the interrupted publisher will never resume. Storing the same
`head` twice is harmless. After panic the producer
stays in the `PANIC` state with `READY` cleared, so later emit calls are dropped without counting.

### 8.11 Producer API summary

```c
/* ---- compile-time macros (EV = event name token from the catalog) ---- */
EXTRACE_ENABLED(EV)                        /* bool, for guarding expensive prep          */
EXTRACE_MARK(EV)                           /* MARK: no timestamp, no payload             */
EXTRACE(EV)                                /* NONE: timestamp only                       */
EXTRACE_INTS(EV, ...)                      /* INTS: 1..16 integer args                   */
EXTRACE_PRINTF(EV, "fmt", ...)             /* PRINTF: deferred formatting (§10)          */
EXTRACE_BUF(EV, ptr, len)                  /* BUF                                        */
EXTRACE_BUF_LARGE(EV, ptr, len)            /* FRAG sequence for len > MAX_PAYLOAD         */
EXTRACE_STRUCT(EV, ptr)                    /* STRUCT: sizeof(*ptr), type-checked         */
EXTRACE_STRUCT_N(EV, ptr, len)             /* STRUCT with flexible array tail            */
EXTRACE_STR(EV, str)                       /* STR: bounded strlen                        */
EXTRACE_TEXT(EV, "fmt", ...)               /* STR formatted on device (optional mode)    */
/* _URGENT variants of each: publish immediately after commit                           */

/* ---- direct reservation: fill the payload in place (§8.14) ---- */
EXTRACE_RESERVE(EV, len, align, &r)        /* bool; align: constant, 1 = no alignment     */
EXTRACE_RESERVE_STRUCT(EV, &r)             /* sizeof, min(alignof, RESV_ALIGN_MAX)       */
void extrace_commit(struct extrace_resv *r);
void extrace_commit_urgent(struct extrace_resv *r);
void extrace_cancel(struct extrace_resv *r);

/* ---- runtime-ID variants (mask check only, no compile-time checks) ---- */
bool extrace_emit_dyn_none(uint16_t id);
bool extrace_emit_dyn_ints(uint16_t id, const uint32_t *v, unsigned int n,
                           uint32_t sign_mask);         /* 32-bit values */
bool extrace_emit_dyn_ints64(uint16_t id, const uint64_t *v, unsigned int n,
                             uint32_t sign_mask);       /* 64-bit values */
bool extrace_emit_dyn_buf (uint16_t id, uint8_t kind, const void *p, uint32_t n);

/* ---- lifecycle ---- */
struct extrace_prod_cfg {
    void       *block;        /* producer view of the ring block              */
    uint32_t    block_size;   /* whole block, incl. the control part          */
    uint32_t    data_size;    /* 0 = largest power of two after the control   */
    uint8_t     ring_id;      /* system-wide unique, 0..254; per instance     */
    uint16_t    flags;        /* reserved, 0                                  */
    const char *name;         /* instance name, up to 8 chars, e.g. "dsp1"    */
    uint32_t    publish_thr;  /* 0 = default                                  */
};
int  extrace_prod_init(const struct extrace_prod_cfg *cfg);
void extrace_prod_poll(void);
void extrace_prod_flush(void);
void extrace_prod_panic(uint32_t reason, uintptr_t pc);
void extrace_prod_stats(struct extrace_prod_stats *out);   /* lost, hwm, publishes, ... */
```

All emit functions are reentrant: they use only the stack, the critical-section
protected fields of `extrace_producer`, and the bytes they reserved. There is no shared
scratch buffer anywhere in the producer.

### 8.12 Argument marshalling

`EXTRACE_INTS` and `EXTRACE_PRINTF` convert arguments with `_Generic`:

```c
#define EXTRACE_AC(x) _Generic((x),                                                 \
    bool: EXTRACE_AC_U32, char: EXTRACE_AC_S32, signed char: EXTRACE_AC_S32,        \
    short: EXTRACE_AC_S32, int: EXTRACE_AC_S32,                                    \
    long: EXTRACE_AC_SLONG, long long: EXTRACE_AC_S64,                             \
    unsigned char: EXTRACE_AC_U32, unsigned short: EXTRACE_AC_U32,                 \
    unsigned int: EXTRACE_AC_U32, unsigned long: EXTRACE_AC_ULONG,                 \
    unsigned long long: EXTRACE_AC_U64,                                           \
    float: EXTRACE_AC_F32, double: EXTRACE_AC_F64,                                 \
    char *: EXTRACE_AC_STR, const char *: EXTRACE_AC_STR,                          \
    default: EXTRACE_AC_PTR)
```

* `EXTRACE_AC(x)` is an integer constant expression. The per-call **signature**
  `Σ AC(arg_i) << 4i` and the INTS **sign mask** are therefore compile-time
  constants, embedded as immediates (INTS) or stored in the site descriptor (PRINTF).
  The signature is handled as two 32-bit words, `sig_lo` (arguments 0–7) and
  `sig_hi` (8–15). Calls with at most 8 arguments, almost all of them, select
  emitter variants that take only `sig_lo`, so no 64-bit value is passed on 32-bit
  targets.
* **32-bit values by default.** Values travel in a compound-literal array built on
  the caller's stack. When every argument class of a call is 32 bits wide, which is
  the overwhelmingly common case on 32-bit targets, the array is `uint32_t[]`
  and the call goes to a 32-bit emitter (`extrace_emit_ints32`,
  `extrace_emit_printf32`) that uses only 32-bit varint encoding. Only a call with
  a 64-bit class (`U64`/`S64`/`F64`, or `PTR` on a 64-bit target) uses
  `uint64_t[]` and the 64-bit emitter. The choice is a constant-condition `?:` on
  the compile-time signature, so the unused branch is never emitted. Integers are
  sign- or zero-extended, floats bit-cast (`F32` stays 32 bits), and pointers are
  converted via `uintptr_t`.
* Enumerations are compatible with an integer type and are classified as that type.
  The host shows the enum name when the event spec or format names the enum (§10.4).
* Non-scalar arguments (structs) fail to compile. `EXTRACE_INTS` additionally
  `static_assert`s that every class is an integer class.
* `EXTRACE_CSTR(s)` wraps a pointer to a string **constant in the image's read-only
  data**, such as `__func__` or a literal. It is logged as class `CSTR` (address only)
  and the host reads the string from the ELF. This costs 2–4 bytes instead of the
  string length.

### 8.13 Varint serialization on ARM and RISC-V

All header fields (`hdr`, `tsd`, `len`) and all `INTS`/`PRINTF` integers are varints,
so serialization runs for every record. It is designed for speed first and **pays
with code size where that buys cycles**. Varint code exists only inside the shared,
out-of-line emitters (§8.2), never at call sites, so unrolling costs a fixed amount
per emitter variant, not per traced event.

**Module boundary.** All varint and zigzag code lives in one standalone header,
`include/extrace/extrace_varint.h` (Appendix 22.A). It depends only on `<stdint.h>`
and the compiler macros of §13.7, and it knows nothing about records, rings, events
or ports. The CLZ comes from `EXTRACE_VARINT_CLZ32`, which the port may define
(normally to `extrace_port_clz32`); otherwise the module uses the builtin where the
ISA has the instruction. `extrace_wire.h` builds the record-level helpers on top of
it (pre-encoded headers, kinds, PADN), and the emitters and the record decoder call
only those two headers. The module has its own test directory (`tests/varint/`),
fuzz target and cost probes, and it can be reused outside extrace. `varint.py`
mirrors it on the host.

The rules below were chosen by comparing the code that clang 18 generates at `-O2`
for Cortex-M4 (Thumb-2, ARMv7E-M), Cortex-M0+ (ARMv6-M) and RV32IM with Zbb and Zce,
the RISC-V baseline (§13.6). Instruction counts include the branch and the pointer
update. M6 (§20) replaces them with cycle measurements on target.

**1. Constants are encoded at compile time.** `EXTRACE_HDR_ENC(EV, K)` is an integer
constant holding the encoded header bytes in bits 0–23 and their count in bits 30–31.
The specialized emitters (rule 4) also receive the sign mask in bits 24–26, so one
32-bit immediate describes the record. The emitter stores the header bytes with no
encoding logic: every user header is exactly 2 bytes (IDs 16–2047), so this is two
byte stores and a shift, with a third store only for IDs ≥ 2048. The same applies to
every other compile-time value: the `STRUCT` length (`sizeof`), the `INTS` sign mask
and the fixed sizes of `MARK` and `NONE`. System records, which have 1-byte headers,
use internal emitters with their own constants.

**2. Runtime values use a fully unrolled shift-test chain** (Appendix 22.A). Each of
the five stages computes `t = v >> 7` and branches on `t == 0`. On ARM the shift sets
the flags (`lsrs` + `beq`). On RISC-V the result feeds `beqz` directly, so no
comparison constant is materialized. The shifted value is reused as the next byte.
All stores use constant offsets from one base pointer, which is advanced once.

| Encoded length | Cortex-M4 | Cortex-M0+ | RV32 Zbb+Zce |
|---|---|---|---|
| 1 byte | 4 | 4 | 4 |
| 2 bytes | 8 | 10 | 8 |
| each further byte | +4 | +4 | +4 |

Alternatives that were measured and rejected:

| Alternative | Why not |
|---|---|
| Byte loop (v0.3 appendix) | clang unrolls it anyway, but keeps loop-carried moves and a stack frame on the multi-byte path (Thumb-2: 39 instructions in total versus 34, plus push/pop). |
| Straight-line stores dispatched on a known length (`switch (n)`) | 2 instructions per byte on Thumb-2 (`orr` with a shifted operand, then `strb`), but the table branch costs 5–6 instructions up front. That is slower for the 1–3-byte values that dominate. |
| Assembling the bytes in a register and doing one word store | Unaligned word stores trap on ARMv6-M, ARMv8-M Baseline and most RISC-V cores, or are emulated in M-mode at 100+ cycles. The store also writes past the varint, which at the end of a record lands in another context's reservation. Spreading the 7-bit groups alone takes about 9 instructions. |

**3. Fast path when every argument fits in one byte.** An `INTS` or `PRINTF` emitter
ORs all argument values, the sign mask and the payload length together. One test,
`((a0 | a1 | ... | sm | plen) >> 7) == 0`, proves that every field is one byte.
Sizing then needs no length computation (`plen = nargs + 1`), and writing is one
store per field with no per-value test. The test result is computed once, before the
reservation, and reused for writing. Measured: the complete write phase of an `INTS`
record with two small arguments (2-byte header, 1-byte delta, length, sign mask, two
values, 7 bytes in total) is 19 instructions on RISC-V and about 20 on Cortex-M4.
When the test fails, the emitter falls back to the per-value chain.

**4. Specialized emitters with arguments in registers.**
`extrace_emit_ints32_{1,2,3}(desc, a0[, a1[, a2]])` and
`extrace_emit_printf32_{0,1,2,3}` take the descriptor of rule 1 and the values in
argument registers (AAPCS r0–r3, RISC-V a0–a7). No array is built on the caller's
stack, and every encoder inside is inlined and unrolled. The array emitters
(§8.12) remain for calls with 4–16 arguments or any 64-bit class. Each specialized
emitter costs an estimated 150–250 B of code. Unused ones are removed by
`-ffunction-sections`/`--gc-sections`, and `EXTRACE_CFG_EMIT_SPECIALIZE=0` maps all
calls to the array emitters for images that need minimal code size.

**5. Varint length (`extrace_uv_len32`).** The reservation needs the length of `tsd`
inside the critical section, and the sizing pass needs the lengths of non-small
arguments. The implementation is chosen at compile time from `__ARM_FEATURE_CLZ` /
`__riscv_zbb`, and the port can override it with `extrace_port_clz32`:

* **With a CLZ instruction** (ARMv7-M mainline and later, ARMv7-R/A, ARMv8, and
  every RISC-V target, because Zbb is part of the baseline):
  `(352 − 9 × clz(v | 1)) >> 6`. `__builtin_clz` maps to the single `clz`
  instruction. The helper is 6 branch-free instructions on Thumb-2 and 7 on RISC-V
  (26 B). The factor 9 becomes a shift-add, so no multiplier is needed.
* **Without** (ARMv6-M, ARMv8-M Baseline): an unrolled shift-test chain, which takes
  3 instructions for a 1-byte value. `__builtin_clz` is never used on these cores,
  because it becomes a libgcc call or a long bit-twiddling sequence. For reference,
  on a RISC-V core without Zbb clang expands it into about 30 instructions plus a
  multiply (118 B), which is one reason Zbb is required.

**Zce** shrinks the unrolled code without changing instruction counts. Compressed
byte stores (`c.sb`, Zcb) and push/pop (Zcmp) reduce the encoder from 104 B to 88 B
and the write phase of rule 3 from 528 B to 452 B (about −15 %). Emitters keep the
destination pointer and the bytes in `x8`–`x15` where possible, because `c.sb` can
address only those registers, with offsets 0–3.

**6. The delta write reuses the chain.** The length of `tsd` is already known from the
reservation, but dispatching on it costs more than the chain for the usual 1–3-byte
deltas (rule 2).

**7. Tiny records are written inside the reservation critical section.** A `MARK`
(2 bytes) or `NONE` (2 bytes plus a 1–5-byte delta) is cheaper to write than to track.
Writing it inside critical section #1 (§8.4) removes the `inflight` increment, the
whole commit section (an `irq_save`/`irq_restore` pair) and the commit bookkeeping.
If `inflight` is 0, `committed` advances together with `head` in the same section.
The section grows by 3–10 instructions. A wrap `PAD` and a pending `LOST` prefix for
these records are written there too; both are rare and bounded (at most 4 and 19
bytes). This applies regardless of `EXTRACE_CFG_WRITE_IN_CS_MAX`, which extends the
same treatment to larger records.

**8. Zigzag** uses `((uint32_t)s << 1) ^ (0u − ((uint32_t)s >> 31))`. The form has no
implementation-defined right shift of a negative value, and it compiles to the same
code as the arithmetic-shift form: 2 instructions on Thumb-2 (`lsls`, then
`eor …, asr #31`) and 3 on RV32.

**9. 64-bit values on 32-bit cores** use only 32-bit operations. If `hi == 0`, the value
takes the 32-bit path. Otherwise the emitter stores four continuation bytes from
`lo`, then a fifth byte `(lo >> 28) | ((hi & 7) << 4)`, then runs the 32-bit chain
on `hi >> 3`. The length is `hi == 0 ? len32(lo) : 5 + (hi >> 3 ? len32(hi >> 3) : 0)`.

**10. Write once, never read back.** Encoders only store to the destination. They
never load from it or modify a byte in place (`|=`), because in non-cacheable
memory a read-back is an unbuffered bus read (§8.8.1). Bytes that depend on later
results, such as a continuation bit, are completed in a register first (Appendix
22.A, `extrace_put_uv64`).

**10a. Aliasing discipline.** Encoders take `uint8_t *restrict` destinations and work
on local copies. A store through `uint8_t *` may alias any object, so any producer
state that is needed after the write phase is loaded into locals first. Otherwise the
compiler reloads it after every byte store.

**11. Decoder** (dispatcher, host-test C decoder). The decoder has a 1-byte fast path.
When at least 5 bytes remain before `end`, it runs an unrolled path with no bounds
checks; otherwise it uses the checked loop. For 32-bit fields it rejects a fifth byte
above `0x0F`, because that value does not fit in 32 bits. This is a corruption check
and does not affect the tolerance for over-long encodings.

**12. Guarding the numbers.** The release-check build (§18.6) disassembles probe
functions for each target and asserts instruction budgets for the 1- and 2-byte
encoder paths, the length helpers and the write phase of rule 3. A codegen regression
therefore fails CI in the same way as a regression in the disabled-event path.

### 8.14 Direct reservation API

The reserve / write / commit mechanism of §8.3–§8.5 is also a **public API**. A
caller reserves a record with an exact payload length and gets a pointer into the
ring. It fills the payload in place, with interrupts enabled, and then commits. All
single-shot macros (`EXTRACE_BUF`, `EXTRACE_STRUCT`, `EXTRACE_STR`, and the varint
emitters of §8.13) are thin wrappers over the same internals: reserve, then copy
or encode, then commit. Most code should use the wrappers. The direct API is for
payloads produced in place, where a stack copy would double the work, for example a
driver filling a descriptor or a protocol stack serializing a header.

```c
struct extrace_resv {
    void    *payload;        /* len bytes, contiguous in the ring: filled by the caller */
    uint32_t len;
    uint8_t *rec;            /* private: first header byte, for cancel               */
    uint8_t  hdr_len;        /* private: header bytes, for cancel                    */
};

/*
 * EV must be declared BUF, STRUCT, STR or ANY. The result is false, with nothing
 * reserved, if the event is disabled or compiled out (len is not evaluated), if
 * len > EXTRACE_CFG_MAX_PAYLOAD, or if the ring is full (counted as lost, §8.6).
 * align: integer constant expression, 1 (none) or a power of two up to
 * EXTRACE_CFG_RESV_ALIGN_MAX; aligns r->payload in memory.
 */
bool EXTRACE_RESERVE(EV, len, align, struct extrace_resv *r);
/* STRUCT events: len = sizeof, align = min(alignof, EXTRACE_CFG_RESV_ALIGN_MAX) */
bool EXTRACE_RESERVE_STRUCT(EV, struct extrace_resv *r);

void extrace_commit(struct extrace_resv *r);         /* payload complete            */
void extrace_commit_urgent(struct extrace_resv *r);  /* ... and publish now (§8.8)  */
void extrace_cancel(struct extrace_resv *r);         /* abandon: becomes SKIP       */
```

Example of filling a struct in place:

```c
struct extrace_resv r;

if (EXTRACE_RESERVE_STRUCT(TX_DONE, &r)) {
    struct tx_done_info *ti = r.payload;     /* aligned for the type */

    ti->status = desc->status;
    ti->rate = desc->rate;
    /* ... every byte of *ti, including padding, is written ... */
    extrace_commit(&r);
}
```

**Alignment is a compile-time choice.** Some platforms never need it, so it costs
nothing unless it is used:

* `align` is an integer constant expression, checked by `static_assert` to be a
  power of two no larger than `EXTRACE_CFG_RESV_ALIGN_MAX`. The macro selects the
  reserve variant at compile time. `align == 1` calls `extrace_reserve()`, which has
  no alignment code at all, not even a compare. `align > 1` calls
  `extrace_reserve_aligned()`, a separate function that is linked only if some call
  site uses it (`--gc-sections`).
* `EXTRACE_CFG_RESV_ALIGN_MAX` (default 8) caps alignment per image. Setting it to 1
  removes alignment support entirely. `EXTRACE_RESERVE_STRUCT` then reserves
  unaligned payloads, and the caller fills them with `memcpy` (see the rules below).
  This suits images that fill payloads only with `memcpy` or byte stores, or that
  use only the single-shot macros.
* The single-shot macros never align. Their payload is copied with `memcpy`, which
  has no alignment requirement.

**Sequence.**

1. **Reserve: critical section #1, unchanged (§8.4).** It reads the timestamp,
   computes the delta, runs the fit test and bumps `head` and `inflight`. With
   `align > 1` it also computes `k`, the number of alignment bytes that puts the
   payload start (record start + header + delta + length bytes) on an `align`
   boundary at the record's final position, after any wrap. `k` is included in the
   reservation. The data area starts 8-byte aligned (§6.1), so an aligned
   offset is an aligned address. This costs a few instructions in
   `extrace_reserve_aligned()` only.
2. **Header: interrupts enabled.** Still inside `reserve`, the producer writes the
   wrap `PAD`, a pending `LOST` prefix, `k` `PAD1` bytes (`0x00`), the header, the
   delta and the length. The `PAD1` bytes lie at a record boundary, so every decoder
   skips them as padding records and finds the header right after them (§5.8). It
   then returns
   `r->payload`.
3. **Fill: interrupts enabled.** The caller writes `[payload, payload + len)`. Any
   other context, an interrupt or a preempting thread, can meanwhile reserve, write
   and commit its own records. Those records lie after this one in the ring and are
   published only after this one is committed, because `committed` advances only
   when `inflight` returns to 0 (§8.5).
4. **Commit: critical section #2 (§8.5).** About five instructions decrement
   `inflight`, followed by the threshold publish.

**Cancel.** `extrace_cancel()` overwrites the record header (2 or 3 bytes) with
`PAD1` bytes and the 1-byte header of a `SKIP` record (§4.6), then commits. `tsd`,
`len` and the payload stay where they are: decoders skip the record but still add
its `tsd`. The reservation advanced the producer's `last_ts`, and the deltas of all
later records, possibly reserved already by other contexts, build on it, so a
padding record without a `tsd` would shift every later timestamp until the next
anchor. At most 3 byte stores, no reads. The record never reaches record sinks and
is not counted as lost. A `LOST` prefix written in the same
reservation stays valid. A reservation cannot be returned to the ring, because
other contexts may already hold space after it.

**Rules for callers.**

* Every successful reserve is followed by **exactly one** `commit` or `cancel`. It
  normally comes from the same context; `inflight` is a plain counter, so any
  context is allowed. At most 255 reservations can be open at once (`inflight` is
  8 bits wide); a further reserve fails and counts as lost (§8.5). Open
  reservations can be committed in any order.
* **Keep the fill short and never block.** Until the commit, the publication of
  every later record waits, including records from interrupts (§8.5,
  "Preemption"). A reservation held across a blocking call can fill the ring and
  cause drops in every context. `extrace_prod_stats()` reports the longest
  reserve-to-commit time in debug builds.
* Write **only** inside `[payload, payload + len)` and write **every** byte.
  Unwritten bytes would carry stale ring data to the host, and for `STRUCT` that
  includes padding inside the type.
* Without alignment the payload is byte-aligned (records are packed, D4), so it is
  filled with `memcpy` or byte stores. Only an aligned reservation may be filled
  through a typed pointer. This holds even on cores that tolerate unaligned
  accesses: the access is undefined behaviour in C, and compilers may emit
  multi-word stores (`STRD`, `STM`) that fault when unaligned even on Cortex-M4. With
  `EXTRACE_CFG_RESV_ALIGN_MAX = 1`, `EXTRACE_RESERVE_STRUCT` therefore always needs
  `memcpy`. Alignment costs at most `align − 1` bytes per record.
* The size is exact. There is no truncation: a `len` above
  `EXTRACE_CFG_MAX_PAYLOAD` fails the reservation. Larger data uses
  `EXTRACE_BUF_LARGE` (§5.9.6).
* The payload lives in non-cacheable memory: every store is a bus write and every
  load is an unbuffered bus read. Build values in registers or on the stack and
  store them once. Never read back or modify payload bytes in place (no `|=`, no
  incrementing a counter in the reservation). For larger payloads prefer an aligned
  reservation (`align = 4`) with word stores, or one `memcpy` of a prepared object.
* Lifecycle: `extrace_prod_init()` returns `-EXTRACE_EBUSY` while any reservation is
  open (§7.6 I0). `extrace_prod_panic()` publishes only `committed`, so open
  reservations are lost (§8.10).

**Interrupt responsiveness.** The critical sections cover only the reservation
arithmetic (§8.4) and the commit counter (§8.5). Payload bytes, whether written by
the caller or copied by a wrapper, are always written with interrupts enabled. This
holds for every record with a payload. Two deliberate exceptions concern only
header bytes:

* `MARK` and `NONE` records have no payload. Their 2–7 header bytes are written
  inside critical section #1 (§8.13 rule 7), which is shorter than the commit
  section it replaces.
* The opt-in `EXTRACE_CFG_WRITE_IN_CS_MAX` (default 0, §8.5) is an RTOS preemption
  mitigation.

---

## 9. Event enable control

### 9.1 Layers

| Layer | Mechanism | Cost when off | Changeable |
|---|---|---|---|
| Image | `EXTRACE_CFG_ENABLE=0` | 0 (no code) | rebuild |
| Group | `EXTRACE_GROUP(name, 0)` / `-DEXTRACE_GRP_<name>_ON=0` | 0 (no code) | rebuild |
| Event default | catalog column `ON`/`OFF` | 2–4 instructions | rebuild |
| Event runtime | consumer-owned `mask_set`/`mask_clr` → producer-local `mask` | 2–4 instructions | runtime, any time |

### 9.2 Effective mask

```
effective[w] = (defaults[w] & ~mask_clr[w]) | mask_set[w]
```

| Consumer operation | `mask_set` bit | `mask_clr` bit |
|---|---|---|
| enable(id) | 1 | 0 |
| disable(id) | 0 | 1 |
| reset(id) to default | 0 | 0 |
| enable all / disable all | all 1 / all 0 | all 0 / all 1 |
| reset all to defaults | all 0 | all 0 |

With this scheme the consumer (and the host behind it) never needs to know the
producer's defaults in order to change an event. Defaults stay owned by the
producer image, and "reset to defaults" is trivial.

### 9.3 Consumer control API

```c
/* st: ON / OFF / DEFAULT */
int extrace_cons_ev_set(struct extrace_cons *c, uint16_t id,
                        enum extrace_ev_state st);
/* one seqlock transaction */
int extrace_cons_ev_set_many(struct extrace_cons *c, const uint32_t *set,
                             const uint32_t *clr, uint16_t words);
int extrace_cons_ev_reset_all(struct extrace_cons *c);
/* to match CFG records and the producer's ack */
uint32_t extrace_cons_mask_gen(const struct extrace_cons *c);
/* pstate.mask_gen_ack == gen */
bool extrace_cons_mask_applied(const struct extrace_cons *c);
```

IDs are validated against the ring's `pinfo.ev_count`, and `words` against
`pinfo.mask_words`. Every call is one seqlock write transaction (§7.5). Batch changes
with `set_many` so the producer sees them atomically and the host gets one `CFG`
record.
Consumer-side callers must serialize among themselves, for example with a local
mutex in the dispatcher image. That lock is local to the consumer and is never
shared with producers.

### 9.4 Latency of configuration changes

The producer applies changes at its next `extrace_prod_poll()`. If the port provides a
consumer→producer doorbell (`extrace_port_notify_producer`), the producer's handler
wakes the poll context (§8.10), and the change takes effect after one scheduling
latency, typically microseconds. Otherwise the poll
period bounds it (typically 1–10 ms). The `CFG` record in the stream marks the exact
switch point.

### 9.5 Per-instance granularity

Event control is **per subsystem instance**, never global:

* Every ring block carries its own `mask_set`/`mask_clr` bitmaps, sized by **that
  producer's** catalog (`pinfo.mask_words`), and every producer instance keeps its
  own effective `mask` (§8.1).
* Event IDs, names, groups and format strings differ between images. An ID means
  something only together with the ring it came from, so every control call names
  a `ring_id` (§12.10). `EXTRACE_RING_ALL` is allowed only for operations that do
  not depend on IDs: reset to defaults, commands and thresholds.
* Two instances of the same image (same build ID, different `ring_id`) share
  metadata but **not** settings. For example, `LOG_DBG` can be on for `dsp0` and off
  for `dsp1`.
* The host resolves names and groups with the metadata bound to each ring (build ID
  from `pinfo`/`INFO`) and sends bitmaps sized for that ring (§15.4).
* The dispatcher's record-sink filters are also per ring (§12.3).

---

## 10. Deferred printf and image metadata

### 10.1 Overview

```c
EXTRACE_PRINTF(LOG_INFO, "tx q=%u len=%u st=%d", q, len, st);
```

expands to (simplified):

```c
do {
    if (EXTRACE_ON(LOG_INFO)) {
        static const char xt_fmt[] __section("extrace_meta_str") =
            "tx q=%u len=%u st=%d";
        static const struct extrace_site xt_site
            __section("extrace_meta_site") __aligned(EXTRACE_SITE_ALIGN)
            __used = {
            .fmt = xt_fmt,
            .file = EXTRACE_FILE_STR,
            .line = __LINE__,
            .sig_lo = EXTRACE_SIG_LO(q, len, st), /* compile-time constant */
            .event = EXTRACE_EV_LOG_INFO,
        };
        /* all three classes are 32-bit, so the 32-bit path is selected */
        extrace_emit_printf32(EXTRACE_HDR(LOG_INFO, EXTRACE_K_PRINTF),
                              extrace_site_id(&xt_site),
                              EXTRACE_SIG_LO(q, len, st),
                              (const uint32_t[]){ EXTRACE_AV32(q),
                                                  EXTRACE_AV32(len),
                                                  EXTRACE_AV32(st) },
                              3);
    }
} while (0)
```

* The device stores **neither the format string nor the descriptor** in loaded
  memory (§10.5). It does not parse formats and does not format text.
* The emitter uses `sig_lo` (an immediate) to encode each argument according to its
  class. It does not need the descriptor at runtime.
* There is no shared buffer, so the call is fully reentrant.

### 10.2 Site descriptor and site ID

```c
struct extrace_site {                /* lives only in the ELF (§10.5) */
    const char *fmt;
    const char *file;
    uint32_t line;
    uint16_t event;
    uint8_t nargs;
    uint8_t flags;
    uint32_t sig_lo;                /* 4-bit class per argument, arg 0 in bits 3:0 */
    uint32_t sig_hi;                /* arguments 8..15                            */
} __aligned(EXTRACE_SITE_ALIGN);    /* EXTRACE_SITE_ALIGN = 32 */
```

**Site ID = low 16 bits of the descriptor's address, scaled by its alignment:**

```c
static __always_inline uint32_t extrace_site_id(const struct extrace_site *site)
{
    uintptr_t off = (uintptr_t)site - (uintptr_t)EXTRACE_META_SITE_BASE;

    return (uint32_t)(off >> EXTRACE_SITE_SHIFT) & EXTRACE_SITE_ID_MASK;
}
```

* All descriptors are placed in their own input section, which the linker script
  puts **first** in the metadata output section. Their addresses are therefore
  small, dense and aligned. Shifting out the 5 always-zero low bits makes the 16-bit
  ID cover 65 536 call sites. Without the shift, raw 16 address bits would cover only
  2 048 descriptors.
* `EXTRACE_META_SITE_BASE` is `__start_extrace_meta_site`, which GNU ld and lld
  generate automatically for sections whose names are C identifiers. In INFO mode
  (§10.5) the base is 0 and the subtraction folds away.
* The ID is varint-encoded on the wire: 1 byte for the first 128 sites, 2 bytes up
  to 16 383, 3 bytes above that.
* `EXTRACE_CFG_SITE_ID_BITS` (default 16) widens the mask for very large images. The
  value is reported in `INFO`.
* **Strict compatibility mode** `EXTRACE_CFG_SITE_ID_MODE=FMT_ADDR`: the ID is the low
  16 bits of the **format string's** address. Use it when the linker script cannot be
  changed and the strings stay in normal `.rodata`. The extractor detects ID
  collisions and fails the build, and for this mode it also reports the per-site
  argument signatures, which then have to be stored alongside the strings.

### 10.3 Argument classes

| Class | Code | Wire | Host formatting |
|---|---|---|---|
| `U32` | 1 | uvarint | `%u %x %X %o %c` |
| `S32` | 2 | zigzag uvarint | `%d %i` |
| `U64` | 3 | uvarint | `%llu %llx` |
| `S64` | 4 | zigzag uvarint | `%lld` |
| `F32` | 5 | 4 bytes LE | `%f %e %g %a` |
| `F64` | 6 | 8 bytes LE | `%f %e %g %a` |
| `STR` | 7 | uvarint n + n bytes (≤ `STR_ARG_MAX`) | `%s` |
| `PTR` | 8 | uvarint | `%p` |
| `CSTR` | 9 | uvarint address | `%s` (string read from the ELF) |
| `ULONG`/`SLONG` | – | resolved at compile time to U32/S32 or U64/S64 by `sizeof(long)` | |

The host formats each argument **according to its recorded class**, with the
format's flags, width and precision. Length modifiers in the format are ignored. A
mismatched specifier such as `%d` with an unsigned argument still prints correctly,
and the extractor's lint reports the mismatch as a warning.

### 10.4 Host-only format extensions

These are allowed in deferred mode only. The device never parses formats, so they
cost nothing:

| Extension | Meaning |
|---|---|
| `%{enum NAME}d` | print the enumerator name of `enum NAME` (from DWARF) |
| `%{flags NAME}x` | decompose a bitmask using `enum NAME` bit values |
| `%{ip4}u`, `%{mac}s` | well-known pretty printers (extensible in Python) |

With `EXTRACE_CFG_PRINTF_LOCAL=1` (formatting on the device, §10.7) these extensions
are stripped to plain specifiers by a macro-time check, or reported as an error.

### 10.5 Metadata placement in the ELF

Everything the host needs is emitted by the compiler into sections whose names start
with `extrace_meta_`:

| Input section | Content |
|---|---|
| `extrace_meta_site` | `struct extrace_site` descriptors (must come first) |
| `extrace_meta_ev` | `struct extrace_meta_ev` per catalog event: id, kind, default, group, name, spec, C type name |
| `extrace_meta_grp` | group names and compile-time state |
| `extrace_meta_img` | one `struct extrace_meta_img`: library version, config values (`EV_COUNT`, `SITE_ID_BITS`, ...), layout version |
| `extrace_meta_str` | all strings referenced by the above |

Three placement modes, selected per image:

**Mode A, INFO (default).** The output section is **not allocated** and starts at
address 0:

```ld
/* extrace metadata: kept in the ELF, never loaded, never occupies target memory */
.extrace_meta 0 (INFO) :
{
    KEEP(*(extrace_meta_site))
    KEEP(*(extrace_meta_ev))
    KEEP(*(extrace_meta_grp))
    KEEP(*(extrace_meta_img))
    KEEP(*(extrace_meta_str))
}
```

The device pays nothing in memory. A site ID is simply `address >> 5`. Requirement:
the code model must be able to materialize the absolute address of a symbol in this
section, through absolute or literal-pool addressing. That holds for typical
embedded code models: ARM Thumb/A32 literal pools, AArch64 `ADRP` within ±4 GiB of
address 0, and RISC-V `medlow`. RISC-V `medany` works only while the code stays
within ±2 GiB of address 0; otherwise use mode B (§13.6).

**Mode B, phantom region.** For PC-relative code models: the section is allocated in
a `MEMORY` region that does not exist physically, placed within addressing range of
the code. It is excluded from the load image with `objcopy -R .extrace_meta` when
producing `.bin`/`.hex`, or because the region has no load segment in the loader's
view. Site IDs are base-relative.

**Mode C, rodata.** No linker script control at all: the sections are ordinary
read-only data and cost memory. IDs are base-relative.

Section names without dots let the linker provide `__start_`/`__stop_` symbols in
modes B and C. `KEEP` and `gnu::used` protect the descriptors against
`--gc-sections`. Because of `gnu::used`, a descriptor whose call site is compiled
out through a constant-false `if` (group or image disabled) **may still be emitted**.
That costs no target memory in mode A. The extractor marks such sites as
compiled out, using the group state in `extrace_meta_grp` and the event's
compile-time switch. The M1 conformance test records the actual gcc and clang
behaviour. If a site must vanish completely, the macros can be built to emit the
descriptor only through a preprocessor-level switch (`EXTRACE_CFG_ENABLE=0`).

Milestone M1 (§20) includes a **toolchain conformance test**. It builds a sample
image for every supported architecture and code model with gcc+ld and clang+lld,
then checks with the extractor that site IDs computed on the target match the
descriptor addresses in the ELF.

### 10.6 Image identity

* Images are linked with `--build-id` (GNU note). The port hook
  `extrace_port_build_id()` returns a pointer to the note's descriptor, usually via a
  linker symbol. The producer copies it into `pinfo.build_id` and the `INFO` record.
* The host indexes metadata files by build ID. A stream from an unknown build ID is
  still parsed structurally (kinds, ints, buffers, raw printf args), but it is not
  named or formatted.
* Images without build-id support may instead let the extractor patch a 20-byte
  `extrace_image_id` variable with a hash of the metadata section.

### 10.7 On-device formatting mode (optional)

`EXTRACE_CFG_PRINTF_LOCAL=1` makes `EXTRACE_PRINTF` format on the device into a stack
buffer of `EXTRACE_CFG_TEXT_MAX` bytes (default 128) using the port's `vsnprintf`
hook, and emit a `STR` record with the same event ID. Use it for images that have no
metadata pipeline, or when a UART sink must print readable text with no host at all.
`EXTRACE_TEXT(EV, ...)` always formats locally, regardless of the mode. Local
formatting is reentrant (stack buffer) but slow and uses stack. It is never the
default.

### 10.8 Struct layouts and enums

* `EXTRACE_EV_STRUCT(name, def, grp, type)` records the **C type name** (`#type`) in
  the event metadata. The host resolves the layout from **DWARF** in the same ELF,
  including field names, offsets, sizes, bitfields, arrays, nested structs, enums
  and endianness. Images are built with `-g`; debug info is not loaded on the
  target.
* Enum names for `%{enum X}` and for `INTS` spec fields like `state:enum txq_state`
  also come from DWARF.
* **Without DWARF** (stripped builds, strict toolchains), an image can annotate types
  explicitly:

  ```c
  EXTRACE_TYPE_DESC(struct tx_done_info,
    EXTRACE_FIELD(status), EXTRACE_FIELD(rate), EXTRACE_FIELD_ARR(seq_mask),
    EXTRACE_FIELD_ENUM(state, txq_state));
  ```

  This emits `offsetof`/`sizeof`/type-class descriptors (via `_Generic`) into
  `extrace_meta_type`, which the extractor reads with no DWARF parser. DWARF remains
  the recommended path because it needs no annotations and cannot drift from the
  code.

---

## 11. Consumer (ring reader) design

### 11.1 State

```c
struct extrace_cons {
    uint8_t *block;                 /* consumer view of the ring block            */
    const uint8_t *data;
    uint32_t size;
    uint32_t tail;                  /* free-running index, local copy of cstate.tail */
    uint32_t epoch;                 /* attached epoch                             */
    uint64_t stream_pos;            /* tail extended to 64 bits (§6.6)            */
    uint32_t cmd_pending;           /* command bits not yet acknowledged (§7.7)   */
    uint32_t mask_gen, cmd_seq;     /* mirrors of cstate words (§6.3 rule 8)      */
    uint32_t *mask_mirror;          /* caller-provided: mask_set words, then mask_clr */
    uint16_t mirror_words;          /* capacity, at least 2 × mask_words          */
    uint8_t ring_id;
    uint8_t flags;                  /* ATTACHED, ERROR                            */
    uint16_t ev_count;              /* from pinfo: valid event IDs are < ev_count */
    uint16_t mask_words;            /* from pinfo: sizes the shared bitmaps       */
    struct extrace_shm_pinfo pinfo; /* validated snapshot, also used in §7.6 A2   */
    struct extrace_cons_stats stats; /* windows, bytes, epoch changes, errors     */
};
```

### 11.2 API

```c
/* mirror: EXTRACE_CONS_MIRROR_WORDS(n) words for rings of up to n events */
#define EXTRACE_CONS_MIRROR_WORDS(n) (2u * (((n) + 31u) / 32u))
int extrace_cons_attach(struct extrace_cons *c, void *block, uint32_t block_size,
                        uint32_t *mirror, uint16_t mirror_words, uint16_t flags);
/* detects epoch change and readiness */
int extrace_cons_poll_state(struct extrace_cons *c);

/* Zero-copy */
struct extrace_span {
    const uint8_t *p;
    uint32_t len;
};

struct extrace_window {
    struct extrace_span span[2];    /* from tail & mask to the ring end, then from 0 */
    uint8_t nspan;
    uint32_t start;                 /* free-running index of the first byte (== tail) */
    uint32_t end;                   /* free-running index after the last byte     */
    uint64_t pos;                   /* 64-bit stream position of span[0].p       */
    uint32_t epoch;
};

int extrace_cons_peek(struct extrace_cons *c, struct extrace_window *w,
                      uint32_t max_bytes);
int extrace_cons_release(struct extrace_cons *c, struct extrace_window *w,
                         uint32_t bytes);

/* Copy mode: whole records only; bytes copied, or -EXTRACE_ENOSPC (§11.5) */
int extrace_cons_read(struct extrace_cons *c, void *dst, uint32_t dst_size);

/* Control (see §9.3) and commands */
int extrace_cons_cmd(struct extrace_cons *c, uint32_t cmd_bits);
int extrace_cons_set_thr(struct extrace_cons *c, uint32_t publish_thr,
                         uint32_t notify_thr);   /* both, one THR command */
int extrace_cons_set_notify(struct extrace_cons *c, uint32_t notify_thr);
int extrace_cons_set_publish_thr(struct extrace_cons *c, uint32_t thr);

/* Producer diagnostics of the attached epoch: lost_total, hwm, status */
int extrace_cons_diag(const struct extrace_cons *c, struct extrace_cons_diag *out);
```

The consumer module is the only code on the consumer side that touches the ring
block. The dispatcher uses these calls for every shared-memory access, so the
consumer-side word ownership and ordering rules (§6.3, §7) live in one file.
Pointers to producer-owned regions (`pinfo`, `pstate`) are `const` in the
consumer, as pointers to consumer-owned regions are in the producer.

The consumer keeps local mirrors of the override bitmaps it owns, so a single-event
change, `extrace_ctl_ev_get()` and a resume never read shared memory back (§6.3
rule 8). The caller provides them at attach, like all consumer memory. A ring whose
`mask_words` does not fit refuses to attach with `-EXTRACE_ENOSPC`.

### 11.3 Peek

1. Load `head` and `epoch` (§7.3 C1–C3). If the epoch changed, re-attach and return
   `-EXTRACE_EEPOCH`. If `head − tail > size`, mark the ring `ERROR` (§17).
2. `avail = head − tail`. If it is 0, return 0.
3. Build the spans from `off = tail & mask`:
   `len0 = min(avail, size - off)`; `span[0] = [off, off + len0)`; if
   `avail > len0`, `span[1] = [0, avail - len0)`.
4. **Budget clamp.** If `max_bytes` is smaller than `avail`, walk record headers
   from `tail` (decode `hdr`, `tsd`, `len` only; a `PAD1` advances one byte and a
   `PADN` skips its length, §5.8) to the last record boundary within
   budget. A record is never split, because windows always start and end on record
   boundaries. The published `head` is always a record boundary, and so is every
   release. The walk reads only the few header bytes of each record from shared
   memory. If the first record alone exceeds `max_bytes`, the window holds exactly
   that record: the budget is a soft limit, so a small budget can delay a ring but
   never starve it.

### 11.4 Release

`release(w, bytes)` with `bytes ≤ w->end − w->start`, at a record boundary: `mb()`,
re-check the epoch, `tail += bytes`, `stream_pos += bytes`, store `cstate.tail`, `wmb()`. The
window is then trimmed, or zeroed when it is fully released, so releasing the same
window twice is a harmless no-op. Releasing the first span and part of the second
is fine.

### 11.5 Copy mode

`extrace_cons_read` peeks with `max_bytes = dst_size`, copies the spans with word
loads (`extrace_port_memcpy` aligns the source and uses word or burst loads), and
releases immediately. If the next record is larger than `dst_size`, it returns
`-EXTRACE_ENOSPC` and releases nothing, so the caller can retry with a larger buffer. The copy output is a
valid record stream, including PAD records at former wrap points.

---

## 12. Dispatcher and sinks

### 12.1 Responsibilities

* Own the `extrace_cons` of every attached ring and track attach state, epoch changes
  and errors.
* Drain rings fairly, within a per-call budget.
* Hand raw windows to **bulk sinks** without copying or parsing.
* Decode records **once** and deliver them to every matching **record sink**.
* Release ring space when all lossless sinks are done with it.
* Report its own health (per-ring and per-sink counters). Optionally it emits these
  as trace events into its own local ring, so they reach the host in-band.

### 12.2 Sink interface

```c
enum extrace_sink_class { EXTRACE_SINK_BULK, EXTRACE_SINK_RECORD };

enum extrace_sink_ret {
    EXTRACE_SINK_DONE,      /* finished with the window; no reference remains     */
    EXTRACE_SINK_BUSY,      /* lossy sinks only: cannot accept now                */
};

struct extrace_rec {                /* decoded record view (zero-copy) */
    uint8_t ring_id;
    uint8_t kind;
    uint8_t flags;                  /* HAS_TS, AFTER_LOST, ...                    */
    uint16_t event;
    uint64_t ts;                    /* global-timer ticks re-based onto the
                                       dispatcher timeline (§12.5); MARK: last known */
    const uint8_t *payload;         /* points into the ring; valid during callback */
    uint32_t len;
};

struct extrace_sink;

struct extrace_sink_ops {
    /* BULK; can_accept == NULL means always */
    bool (*can_accept)(struct extrace_sink *s, uint8_t ring_id, uint32_t len);
    /* BULK: called when new data is available; synchronous (§12.6) */
    enum extrace_sink_ret (*window)(struct extrace_sink *s, uint8_t ring_id,
                                    const struct extrace_window *w);
    /* RECORD */
    void (*record)(struct extrace_sink *s, const struct extrace_rec *r);
    /* both: ring attached / epoch changed / detached, with validated pinfo */
    void (*ring_event)(struct extrace_sink *s, uint8_t ring_id,
                       enum extrace_ring_evt e,
                       const struct extrace_shm_pinfo *pi);
    /* optional, end of a dispatch round */
    void (*flush)(struct extrace_sink *s);
};

struct extrace_sink {
    const struct extrace_sink_ops *ops;
    uint8_t cls;                    /* BULK / RECORD                              */
    uint8_t flags;                  /* EXTRACE_SINK_F_LOSSY                       */
    uint8_t kind_mask;              /* RECORD: which kinds (bit per kind)         */
    uint32_t ring_map[8];           /* which rings: bit per ring_id (0..254)      */
    /* RECORD: per attached ring (dispatcher slot); map == NULL means all events */
    struct extrace_ev_map ev_filter[EXTRACE_CFG_DISP_MAX_RINGS];
    struct extrace_sink_stats stats; /* windows, bytes, records, busy, gaps       */
    struct extrace_sink *next;      /* intrusive list, no allocation              */
};

void extrace_disp_init(struct extrace_disp *d);
int extrace_disp_add_ring(struct extrace_disp *d, struct extrace_cons *c);
int extrace_disp_add_sink(struct extrace_disp *d, struct extrace_sink *s);
int extrace_disp_del_sink(struct extrace_disp *d, struct extrace_sink *s);
/* returns bytes processed or a negative error */
int extrace_disp_poll(struct extrace_disp *d, uint32_t budget_bytes);
```

**Ring identity.** Every public API, sink callback, decoded record and bulk frame
identifies a ring by its system-wide `ring_id` from `pinfo` (0–254). The dispatcher
maps `ring_id` to an internal slot (at most `EXTRACE_CFG_DISP_MAX_RINGS`) once, at
attach time. Slots never appear in an interface. `extrace_disp_add_ring()` rejects a
second ring with the same `ring_id`.

```c
struct extrace_ev_map {          /* caller-owned bitmap, sized for the ring's ev_count */
    const uint32_t *map;
    uint16_t        words;       /* IDs at or beyond words * 32 do not match */
};
```

All memory is caller-provided (static structs). The dispatcher never allocates.
Per-event structures on the consumer side are sized at runtime from each ring's
`pinfo.ev_count`. `extrace_ev_map_words(ring)` tells the caller how many words to
provide.

### 12.3 Filters

A record reaches record sink `s` iff:

```
bit(s->ring_map, ring_id)
&& (s->kind_mask >> kind) & 1
&& (s->ev_filter[slot].map == NULL
    || (event < 32 * s->ev_filter[slot].words && bit(s->ev_filter[slot].map, event)))
```

`slot` is the dispatcher's internal slot of `ring_id`.

* Filters are per ring because event IDs are per image.
* The dispatcher maintains a per-ring **union** of all record sinks' interests. A
  record outside the union is skipped right after decoding its header, without
  building a `extrace_rec` or computing the timestamp beyond the delta accumulation.
  If no record sink is interested in a ring at all, its windows are not parsed: the
  dispatcher releases them after the bulk sinks, without walking a single header.
* Bulk sinks filter by `ring_map` only. They exist to move raw data, and finer
  filtering of what the producer emits belongs in the producer's event mask, which
  saves ring space and bandwidth too.
* System records (`SYNC`, `LOST`, `INFO`, `CFG`, `PANIC`) are always decoded when the
  ring is parsed, because they maintain decoder state. They are delivered to sinks
  that ask for them like any other event (IDs 1–7).

### 12.4 Dispatch round

```
extrace_disp_poll(d, budget):
    for each ring r in round-robin order starting after the last served ring:
        w = cons_peek(r, min(budget_left, per_ring_quantum))
        if w empty: continue
        -- admission: all lossless bulk sinks must be able to take the window
        if any lossless bulk sink s on r has s->can_accept(r, w.len) == false:
            continue                       (back-pressure: nothing offered, nothing released)
        -- bulk sinks
        for s in bulk sinks with ring r:
            ret = s->window(r, w)
            BUSY & LOSSY   -> s.gap += w.len ; mark s gap-pending for ring r
            BUSY & !LOSSY  -> contract violation: counted as error + gap (never re-offered)
        -- record sinks
        if union(r) not empty: stage, re-check epoch, decode, deliver matching records (12.5)
        -- release: every sink has returned, nothing references w any more
        cons_release(r, w)                  (C5–C7)
        budget_left -= w.len
    call flush() on sinks
```

* **Lockstep frontier.** Every sink sees the same window sequence, and each window is
  offered exactly once. Admission is checked **before** any sink sees the window, so
  no window is ever delivered twice. A lossless sink that cannot accept holds the
  ring for everyone, and the producer sees the backpressure as a full ring and starts
  counting drops. Drops are therefore accounted at the source.
* **Lossy sinks** (for example a UART text sink) never hold the ring. When they
  cannot keep up they lose windows and count them. The bulk frame they emit next
  carries the `GAP` flag.
* **Fairness**: a per-ring quantum (default `size / 2`) and round-robin start mean
  one chatty ring cannot starve the others. The overall budget bounds dispatcher
  latency per call.

### 12.5 Record decoding

`extrace_rec.c` implements the wire grammar (§5.3) with full bounds checking:

```c
struct extrace_dec {
    uint64_t ts;
    uint8_t ring_id;
    uint32_t errors;
};

int extrace_dec_next(struct extrace_dec *d, const uint8_t **p,
                     const uint8_t *end, struct extrace_rec *out);

/* Helpers for record sinks */
int extrace_rec_ints(const struct extrace_rec *r, uint64_t *out,
                     unsigned int max, uint32_t *sign_mask);
int extrace_rec_printf_site(const struct extrace_rec *r, uint32_t *site,
                            const uint8_t **args, uint32_t *args_len);
```

* Spans are decoded independently. Records never cross a span boundary because of
  PAD (§5.8).
* **Staged decoding.** Loads from non-cacheable memory are not buffered: every load
  stalls for the full bus latency, and a varint decoder touches every byte. The
  record path therefore never parses shared memory in place. It copies each span,
  in pieces of at most `EXTRACE_CFG_DISP_STAGE` bytes (default 1024), into a
  dispatcher-local buffer with word or burst loads. After each copy it issues `rmb`
  and re-checks `pstate.epoch`. If the epoch changed, the copy is discarded and
  nothing more of the window is decoded (§7.3). Otherwise it decodes the complete
  records there and carries an incomplete trailing record over to the next piece.
  The buffer must hold one whole record: a ring whose `pinfo.max_record` exceeds
  `EXTRACE_CFG_DISP_STAGE` attaches for bulk sinks only, its record decoding stays
  off, and the attach reports it (`ring_event`, stats). This needs about 4 times fewer stalled loads than a byte-wise
  parse, and then decodes from fast memory. `extrace_rec.payload` points into the
  local buffer. Bulk sinks are unaffected: they read windows by DMA or `memcpy`
  (§12.6).
* `extrace_dec_next()` skips padding internally: wrap gaps, alignment bytes and
  cancelled (`SKIP`) records (§5.8), adding the `tsd` of a `SKIP` to the timeline. It returns only event records, with `payload` pointing at
  the first payload byte, so alignment is invisible to record sinks. Padding bytes
  are counted in the decoder statistics.
* Decoder state (`ts`) is per ring and persists across windows. It resets on epoch
  change and is reset again by the first `SYNC`. Until the first anchor after an
  attach (`SYNC` or `LOST`) the decoder has no absolute time, and records are
  delivered with `HAS_TS` clear. A new epoch starts with `INFO` and `SYNC` (§7.6 I6),
  so only a consumer that resumes mid-epoch sees such records, for at most one
  `sync_period`.
* **Timeline re-basing.** At each peek, after the peek has read `head`, the
  dispatcher reads the global timer itself and extends it to 64 bits (`disp_ts`).
  Every record of the window, anchors included, was timestamped before its
  publication, so `disp_ts` is never older than an anchor in the window, which the
  formula below requires. A value sampled once per poll would not be: a producer
  can publish an anchor after that sample and before its ring is peeked. This is one 64-bit extension per peek on the
  dispatcher, never per record on a producer. On every absolute anchor (`SYNC` or
  `LOST` with 32-bit value `S`) it sets the ring's 64-bit decoder time:
  `ts = disp_ts − (uint32_t)(low32(disp_ts) − S)`. Each following record adds its
  32-bit `tsd`. Record sinks therefore see one common 64-bit timeline across all
  rings. This is valid while an anchor reaches the dispatcher within one timer wrap
  period of being produced (71 minutes at 1 MHz), which periodic `SYNC` (§8.7) and
  normal delivery latency guarantee by a wide margin.
* Payload pointers point into the dispatcher's staging buffer and stay valid only
  during the callback.
  A sink that needs data later copies it. Payload bytes are unaligned, so sinks read
  struct fields with `memcpy` into a local object, never by casting.

### 12.6 Synchronous bulk sinks and DMA

A bulk sink is a **callback that runs when new data is available**: the dispatcher
calls `window()` with each new window and releases it once every sink has returned.
The contract is synchronous. When `window()` returns, the sink holds no reference to
ring memory, and every read of the window, by the CPU or by a DMA engine, is
complete.

A host interface that moves data by DMA (PCIe, USB) is the only place where DMA
reads a ring. The sink handles it inside the callback in one of two ways:

* **Zero copy.** Build the frame header (§12.7) in local memory, submit header and
  spans as one scatter-gather transfer, and wait for its completion before
  returning. The dispatcher is busy for the duration of the transfer, which bounds
  how much can be in flight. The per-ring quantum and the budget (§12.4) keep that
  wait bounded.
* **Staging.** Copy the window into the sink's own transfer buffer and return. DMA
  then runs from that buffer at its own pace, which is the sink's private business.
  This costs one copy and decouples the dispatcher from the link.

Memory and DMA:

* The trace memory is non-cacheable for every agent, the DMA engine included, so
  memory always holds the published window. A DMA read needs no cache maintenance,
  and a sink that only DMAs a window never makes the dispatcher CPU read it.
* The epoch re-check C6 runs after the sinks have returned, before release. A
  producer restart during a transfer is therefore detected, and the window is never
  released into the new epoch (§7.6.1, row 11).

There is no pending-window queue, no deferred completion call and no second read
cursor in the consumer. `extrace_cons_peek()` always starts at `tail`, because
every offered window is released before the next peek.

### 12.7 Bulk frame format (to host)

Bulk sinks prefix every window, or window piece, with a frame header:

```c
struct extrace_bulk_hdr {           /* 32 bytes, little-endian, naturally aligned */
    uint32_t magic;                 /* "EXBF" */
    uint8_t version;                /* 1 */
    uint8_t ring_id;                /* system-wide ring ID from pinfo */
    uint16_t flags;                 /* BOUNDARY, ATTACH, EPOCH_NEW, GAP, INFO, DISP_STATS */
    uint32_t epoch;
    uint32_t len;                   /* payload bytes following */
    uint64_t pos;                   /* stream position of first payload byte */
    uint64_t disp_ts;               /* dispatcher's 64-bit extended timer at peek */
};
```

* `pos` is the ring's free-running index extended to 64 bits (§6.6); its low 32 bits
  always equal the index. The host link delivers
  frames in order, so a `pos` that does not continue from the previous frame of the
  same ring and epoch always means a gap (lossy sink, dropped transfer). No reorder
  stage is needed on the host.
* `disp_ts` lets the host re-base each ring's timestamps onto one timeline, exactly
  as the dispatcher does on the device (§12.5, §15.5).
* `BOUNDARY` means the payload starts at a record boundary. Windows always do. If the
  transport has to split a window by MTU, only the first piece has `BOUNDARY` set.
  After a gap the host resumes parsing at the next `BOUNDARY` frame and re-anchors
  time at the next `SYNC` (periodic, §8.7).
* An `INFO` frame (payload = `pinfo` snapshot, varint-serialized as in §5.9.3) is
  sent whenever a ring attaches, so the host can bind ring → image (build ID) even
  before the in-band `INFO` record arrives. It carries `EPOCH_NEW` when the attach
  starts a new epoch, and `ATTACH` alone when the consumer resumed the current epoch
  after a restart (§7.6 A2). After `ATTACH` the stream simply continues: the host
  re-extends `pos` from its low 32 bits and the last `pos` of that ring and epoch,
  and reports no gap.
* **Zero copy**: the header is built in local memory, and the payload is described by
  1–2 spans that point directly into shared memory. Inside the window callback a
  scatter-gather DMA sends header + spans as one transfer (§12.6).

### 12.8 Reference sinks

| Sink | Class | Behaviour |
|---|---|---|
| `extrace_sink_bulk` | BULK, lossless, synchronous | Generic frame builder on top of a platform "transport" ops table: `xfer(sg_list, n)`, which returns when the transfer is complete, or a `copy`-mode transport that stages the frame in the sink's own buffer (§12.6). Examples: PCIe DMA into a host ring, USB bulk, memory dump buffer. |
| `extrace_sink_text` | RECORD, lossy | Prints `[ring] t=<us> <ev-id> <kind> <decoded ints / hex / raw printf args>` through a non-blocking `putc`/`write` hook with its own small text FIFO. Uses an optional on-device name table (`extrace_names.c`, generated by the extractor) when it fits in memory. Otherwise it prints IDs, and the host tool can post-decode the UART log with the metadata. |
| `extrace_sink_stats` | RECORD, lossless (cheap) | Per (ring, event) counters, plus optional per-event integer-argument aggregates (min/max/sum/count of argument *k*) configured by a static table. Results are reported periodically: printed, exported through a callback, or emitted as dispatcher trace events. |
| `extrace_sink_mem` | BULK, sync | Copies windows into a local capture buffer (linear or circular) for retrieval over a debugger or later upload. |

### 12.9 Dispatcher without its own tracing

If the dispatcher image does not link the producer module, dispatcher statistics are
available through `extrace_disp_stats()` only, and sink gaps are reported through
bulk-frame flags. The dispatcher does not depend on the producer.

### 12.10 Control API for host commands

The host reaches the device through a platform-specific command channel, which
extrace does not define. The platform's command handler calls the following
dispatcher-level API. It wraps the per-ring consumer control (§9.3) and sink
management, so platform code never touches shared memory directly.

```c
/* broadcast, only for ID-independent operations (§9.5) */
#define EXTRACE_RING_ALL 0xFFu

/* Event enable control: always one ring, because event IDs are per image (§9.5) */
int extrace_ctl_ev_set(struct extrace_disp *d, uint8_t ring_id, uint16_t id,
                       enum extrace_ev_state st);       /* ON / OFF / DEFAULT */
int extrace_ctl_ev_set_range(struct extrace_disp *d, uint8_t ring_id,
                             uint16_t first, uint16_t last,
                             enum extrace_ev_state st);
int extrace_ctl_ev_set_map(struct extrace_disp *d, uint8_t ring_id,
                           const uint32_t *set, const uint32_t *clr,
                           uint16_t words);             /* atomic batch */
int extrace_ctl_ev_get(struct extrace_disp *d, uint8_t ring_id, uint16_t id,
                       enum extrace_ev_state *st);      /* override state */
/* ID-independent: ring_id may be EXTRACE_RING_ALL */
int extrace_ctl_ev_reset(struct extrace_disp *d, uint8_t ring_id); /* to defaults */
int extrace_ctl_ev_applied(struct extrace_disp *d, uint8_t ring_id); /* 1 once acked */

/* Producer commands and thresholds (ring_id may be EXTRACE_RING_ALL) */
int extrace_ctl_ring_cmd(struct extrace_disp *d, uint8_t ring_id,
                         uint32_t cmd_bits);            /* FLUSH | SYNC | INFO */
int extrace_ctl_ring_thr(struct extrace_disp *d, uint8_t ring_id,
                         uint32_t publish_thr, uint32_t notify_thr);

/* Sinks */
int extrace_ctl_sink_enable(struct extrace_disp *d, struct extrace_sink *s,
                            bool on);
int extrace_ctl_sink_filter(struct extrace_disp *d, struct extrace_sink *s,
                            uint8_t ring_id, uint8_t kind_mask,
                            const struct extrace_ev_map *ev);

/*
 * Introspection: attached, epoch, name, build ID, ev_count, mask gen applied,
 * lost_total, hwm, ...
 */
int extrace_ctl_ring_info(struct extrace_disp *d, uint8_t ring_id,
                          struct extrace_ring_info *out);
int extrace_ctl_stats(struct extrace_disp *d, struct extrace_disp_stats *out);
```

* **Group operations** ("enable group TX") are expanded on the host into bitmaps
  using the image metadata, then sent as one `extrace_ctl_ev_set_map` call. The
  device needs no group names.
* `extrace_ctl_ev_get` returns the **override** state (ON/OFF/DEFAULT). The defaults
  belong to the producer image and are known to the host from metadata.
* Concurrency: these calls change consumer-owned shared state and dispatcher lists.
  They must run in the dispatcher's context (for example a command handler invoked
  from the same loop as `extrace_disp_poll`), or the platform must serialize them
  with it. This is a local serialization only; producers are never locked.
* Every call returns 0 or a negative error code (`-EXTRACE_E*`, §17). A ring that
  is not attached yet returns `-EXTRACE_ENOTREADY`. An ID-dependent call with
  `EXTRACE_RING_ALL` returns `-EXTRACE_EINVAL`.

---

## 13. Port layer

### 13.1 Contract

* Every image provides exactly one `extrace_port.h` on its include path. The library
  includes it from `extrace_cfg.h` and from nowhere else.
* The port also supplies the **compiler abstraction**: kernel-style macros such as
  `__used`, `__maybe_unused`, `__aligned()` and `likely()` (§13.7). The library
  never spells a compiler extension directly.
* All hooks are `static inline` functions or macros. **There is no runtime
  indirection.** The hooks on the producer fast path are `extrace_port_ts`,
  `extrace_port_irq_save` and `extrace_port_irq_restore`. They must be a handful
  of instructions each.
* Hooks must not call back into extrace, and must not block or allocate.
* A **template** (`port/template/extrace_port.h`) documents every hook. Reference ports
  cover the two target architectures, ARM (Cortex-M, Cortex-R, Cortex-A) and RISC-V,
  plus POSIX and a write-buffer reordering simulator for host testing (§18.3).
  Architecture notes are in §13.6.
* **There are no cache hooks.** The port does not know about caches. Mapping the
  trace memory non-cacheable is the platform's job, done once in its MPU, MMU or PMA
  setup (§6.3 rule 7, §13.6).

### 13.2 Hook reference

| Hook | Required | Context | Semantics and expectations |
|---|---|---|---|
| `uint32_t extrace_port_ts(void)` | yes | any, incl. ISR, IRQs off | Read the **SoC-global free-running timer** (typically 1 MHz or faster), the same counter in every image and on the dispatcher. Monotonic, **up-counting**, wrapping at 2^32, cheap (one MMIO or sysreg load). A 64-bit counter returns its low word. Down-counters must be inverted (`~x` or `0 - x`). |
| `uint32_t extrace_port_ts_freq(void)` | yes | init | Global timer ticks per second, reported in `INFO`. |
| `unsigned long extrace_port_irq_save(void)` | yes | any | Disable local interrupts (all that may call extrace) and **return the previous interrupt state as `flags`**. Works when interrupts are already disabled. **Must act as a compiler barrier** (`barrier()` semantics). Nests by construction. |
| `void extrace_port_irq_restore(unsigned long flags)` | yes | any | Restore exactly the state saved in `flags`; never unconditionally enable interrupts. Compiler barrier. |
| `void extrace_port_wmb(void)` | yes | any | Order prior stores before subsequent stores as observed by every other agent. Ordering is enough. ARM: `DMB` (Cortex-M), `DMB OSHST` or `DMB ST` (R, A). RISC-V: `fence w,w`. Compiler barrier. |
| `void extrace_port_rmb(void)` | yes | any | Order prior loads before subsequent loads, with no speculative early satisfaction. ARM: `DMB` (`DMB LD` on v8). RISC-V: `fence r,r`. Compiler barrier. |
| `void extrace_port_mb(void)` | yes | any | Order all prior loads and stores before all subsequent ones. ARM: `DMB`. RISC-V: `fence rw,rw`. Compiler barrier. |
| `void extrace_port_notify(uint8_t ring_id)` | optional | any | Producer→consumer doorbell (mailbox write, IPI). **Starts with a completion barrier** so the preceding `head` store is visible before the doorbell (ARM `DSB`; RISC-V `fence w,o` for an I/O-mapped mailbox, plus any platform rule). Must be non-blocking. Called at most once per publish. |
| `void extrace_port_notify_producer(uint8_t ring_id)` | optional | consumer | Consumer→producer "configuration changed" doorbell. The producer's handler wakes the context that runs `extrace_prod_poll()`; it never calls `poll` itself (§8.10). |
| `bool extrace_port_in_isr(void)` | optional | any | For `EXTRACE_CFG_PUBLISH_IN_ISR=0`. Defaults to `false`. |
| `const uint8_t *extrace_port_build_id(size_t *len)` | recommended | init | Pointer to the GNU build-ID bytes. |
| `int extrace_port_vsnprintf(char*, size_t, const char*, va_list)` | only if `PRINTF_LOCAL` or `EXTRACE_TEXT` used | thread/ISR | Reentrant formatter. |
| `EXTRACE_PORT_HOT_DATA` / `EXTRACE_PORT_HOT_TEXT` | optional | – | Section attributes that place producer state and emitters in TCM or fast RAM. |
| `EXTRACE_PORT_ASSERT(x)` | optional | – | Debug assertions. Defaults to nothing. |
| `unsigned int extrace_port_clz32(uint32_t v)` | optional | any | Count leading zeros, `v != 0`. The port hands it to the varint module as `EXTRACE_VARINT_CLZ32`. Without it, the module uses the compiler builtin where the ISA has a CLZ instruction (always on RISC-V, Zbb), otherwise an unrolled shift chain (§8.13). |
| `extrace_port_memcpy` / `extrace_port_memset` | optional | any | Default to the compiler's builtin memcpy/memset (§13.7). Every copy into or out of a ring goes through `extrace_port_memcpy`. A port that maps the trace memory as Device memory must supply one that uses only aligned accesses (§13.3). Freestanding images must still provide `memcpy`/`memset` symbols, because compilers may emit calls to them. |

### 13.3 Memory types

| Mapping of the trace memory | Use | Notes |
|---|---|---|
| ARM Normal Non-cacheable (MPU `TEX=1, C=0, B=0` on ARMv7-M; MAIR `0x44` on ARMv8) | **recommended** | Stores are buffered, can be merged and are acknowledged early, so byte stores are cheap. Barriers: `DMB`. |
| RISC-V main memory with a non-cacheable PMA, or Svpbmt `NC` | **recommended** | The same properties. `fence` with the `r`/`w` bits orders it. |
| ARM Device (nGnRE and stricter), RISC-V I/O PMA | works, slower; needs an aligned-only `extrace_port_memcpy` | No merging and no early acknowledgement for the stricter types, so every byte store is a full bus transaction. Unaligned accesses to Device memory are UNPREDICTABLE or fault, and the compiler's `memcpy` uses them on ARMv7-M and later, so the port supplies an aligned-only copy (§13.2). Use the `o`/`i` fence bits on RISC-V. |
| Any cacheable mapping | **not supported** | Map the trace memory non-cacheable for every agent (§1.7). Cache maintenance support is a possible future extension (§21.3). |

A core that has a data cache maps the trace memory through a non-cacheable region
or alias. A local ring of the dispatcher, written and read by the same core, may
live in ordinary cacheable RAM only if no bulk sink reads it by DMA: a zero-copy
transfer (§12.6) would read memory that the cache has not written back. With such a
sink, the local ring is mapped non-cacheable like every other ring.

### 13.4 Template excerpt

```c
/* extrace_port.h: port for <platform>. Every hook here is on a hot path unless noted. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "soc_memmap.h"                /* provides timer address, mailbox, ... */

#include "extrace_compiler.h"         /* default __used, likely(), ... (§13.7) */

/* trace memory is mapped Normal Non-cacheable by the MPU setup (§13.3) */

static __always_inline uint32_t extrace_port_ts(void)
{
    /* 1 MHz global up-counter */
    return *(const volatile uint32_t *)SOC_FREE_RUNNING_TIMER;
}

static inline uint32_t extrace_port_ts_freq(void)
{
    return SOC_TIMER_HZ;
}

static __always_inline unsigned long extrace_port_irq_save(void)
{
    unsigned long flags;

    __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(flags) : : "memory");
    return flags;
}

static __always_inline void extrace_port_irq_restore(unsigned long flags)
{
    __asm__ volatile("msr primask, %0" : : "r"(flags) : "memory");
}

static inline void extrace_port_wmb(void)
{
    __asm__ volatile("dmb" : : : "memory");
}

static inline void extrace_port_rmb(void)
{
    __asm__ volatile("dmb" : : : "memory");
}

static inline void extrace_port_mb(void)
{
    __asm__ volatile("dmb" : : : "memory");
}

static inline void extrace_port_notify(uint8_t ring_id)
{
    __asm__ volatile("dsb" : : : "memory");   /* head store complete before the doorbell */
    soc_mailbox_ring(SOC_MBOX_TRACE, ring_id);
}
```

### 13.5 Port bring-up checklist

1. Every agent maps the trace memory non-cacheable, preferably as normal memory
   (§13.3). The DMA engine of a bulk sink reads it without a cache as well.
2. The ring blocks are 8-byte aligned.
3. `extrace_port_ts()` reads the SoC-global timer in every image (including the
   dispatcher), counts up, and does not stall in low-power states that tracing
   cares about.
4. The critical section masks every interrupt priority that can call extrace. On
   ARMv7-M with BASEPRI, that means `BASEPRI` covering those priorities.
5. `wmb`/`rmb` order accesses to the trace memory as seen by the other agents, and
   `extrace_port_notify` completes prior stores before ringing (verify with the
   architecture manual for the specific core and interconnect).
6. Run the port conformance test (`tests/port_conformance.c`) on target. It
   exercises ordering with a peer agent and reports the measured cycle cost of
   every hook. It also reports the stall per byte store and per word load on the
   trace memory, as data for the staging evaluation (§8.8.1). Where the core
   exposes the memory attributes (MPU/PMP readback), it checks that the region is
   non-cacheable.

### 13.6 Architecture notes: ARM and RISC-V

| Topic | ARM | RISC-V |
|---|---|---|
| Critical section (`irq_save`/`irq_restore`) | Cortex-M: `mrs primask` + `cpsid i`, restore with `msr primask`. Use `BASEPRI` only if every interrupt that traces is at or below the masked priority. Cortex-R and AArch32 A-profile: `mrs cpsr` + `cpsid i`. If FIQ handlers trace (as maskable interrupts), use `cpsid if`. AArch64: `mrs daif` + `msr daifset, #2`. | M-mode: `csrrci flags, mstatus, 8` (clear MIE), restore by `csrs mstatus, flags & 8`. S-mode: the same with `sstatus.SIE` (bit 1). |
| Write / read / full barrier | `DMB` on Cortex-M (full system). On R and A profiles use outer-shareable or full-system scope (`DMB OSHST`/`DMB SY`), because the other agents are outside the inner-shareable domain. | `fence w,w` / `fence r,r` / `fence rw,rw` for main memory. For an I/O PMA region add the `o`/`i` bits (`fence ow,ow`, ...). |
| Doorbell completion | `DSB` before the mailbox write, inside `extrace_port_notify`. | `fence w,o` (or `fence rw,rw`) before the mailbox write, plus any platform-specific completion rule. |
| Mapping the trace memory | MPU region or MMU page as Normal Non-cacheable (§13.3). On cores without a data cache (Cortex-M0/M3/M4 without system cache) the default map already is uncached. | PMA attribute of the shared SRAM, or Svpbmt `NC`. Cores without data caches need nothing. |
| Global timer | MMIO system counter or a SoC timer block via `extrace_port_ts()`. | MMIO SoC timer (CLINT `mtime` low word, or a vendor timer) via `extrace_port_ts()`. |
| ISA baseline | ARMv6-M and later (Cortex-M), ARMv7-R, ARMv7-A/ARMv8-A. | **RV32 with Zbb and Zce** (Zca, Zcb, Zcmp, Zcmt), plus M where available. Zbb provides `clz`; Zce provides compressed byte stores and push/pop, which keep the unrolled encoders small (§8.13). |
| Count leading zeros | `CLZ` is available from ARMv5T/ARMv7-M. ARMv6-M and ARMv8-M Baseline lack it, so the library uses an unrolled shift-test chain (`__ARM_FEATURE_CLZ` undefined, §8.13 rule 5). | `clz` (Zbb), one instruction via `__builtin_clz`. The build fails with a clear error if `__riscv_zbb` is undefined, unless the port overrides `extrace_port_clz32`. |
| Unaligned access | The library itself never performs unaligned word accesses: records are written bytewise, payloads go through `extrace_port_memcpy`. The compiler's `memcpy` may use unaligned accesses where the core supports them on Normal memory (ARMv7-M and later). That is legal there, and it is why Device mappings need an aligned-only copy (§13.3). Safe on ARMv6-M and ARMv8-M Baseline. | The library itself behaves the same. Compilers emit no misaligned accesses unless told that the core handles them fast, so cores that trap or emulate them are safe. |
| Metadata mode (§10.5) | Mode A works for Thumb/A32 literal pools and AArch64 `ADRP`. | Mode A with `-mcmodel=medlow`, or with `medany` while code stays within ±2 GiB of address 0. Otherwise mode B. |
| Compiler macros (§13.7) | gcc/clang defaults from `port/common/extrace_compiler.h`. | The same. |
| Byte order | Little-endian is the tuned case: shared words are plain `ldr`/`str` (D18). A big-endian image (BE-8, for example on some Cortex-R) adds one `rev` per shared control-word access, on control paths only, and one `rbit` per defaults word at init (§4.5). Nothing on the record path depends on byte order. | Little-endian; nothing to do. |
| 64-bit arithmetic | None on the producer: timestamps, deltas, indices and the usual argument path are 32-bit (D5, §8.12). 64-bit values appear only in calls with 64-bit arguments and on the consumer side (stream position, timeline). | The same on RV32. |


### 13.7 Compiler abstraction

The library sources and public headers never use a compiler extension directly: no
`__attribute__`, no `[[gnu::...]]`, no `__builtin_*`. They use **Linux kernel-style
macros** that the port layer provides. `port/common/extrace_compiler.h` holds the
gcc/clang definitions and is included by every reference port. A port for another
toolchain replaces it or overrides single macros.

| Macro | gcc/clang definition | Used for |
|---|---|---|
| `__always_inline` | `inline __attribute__((__always_inline__))` | fast-path helpers, port hooks |
| `__noinline` | `__attribute__((__noinline__))` | shared out-of-line emitters |
| `__cold` | `__attribute__((__cold__))` | slow reservation, drop accounting, publish |
| `__used` | `__attribute__((__used__))` | metadata descriptors (§10.5) |
| `__maybe_unused` | `__attribute__((__unused__))` | config-dependent helpers and variables |
| `__aligned(x)` | `__attribute__((__aligned__(x)))` | site descriptors, ring blocks |
| `__packed` | `__attribute__((__packed__))` | (host-shared frame structs only if ever needed) |
| `__section(s)` | `__attribute__((__section__(s)))` | `extrace_meta_*` sections, hot data/text |
| `__printf(a, b)` | `__attribute__((__format__(__printf__, a, b)))` | `EXTRACE_TEXT` and the local-format path |
| `__noreturn` | `__attribute__((__noreturn__))` | panic helpers in tests and examples |
| `likely(x)` / `unlikely(x)` | `__builtin_expect(!!(x), 1)` / `(…, 0)` | branch hints, `EXTRACE_ON` |
| `barrier()` | `__asm__ __volatile__("" : : : "memory")` | compiler barrier in ports |
| `EXTRACE_BUILTIN_CLZ32(v)` | `__builtin_clz(v)` | default of `extrace_port_clz32` (§13.2) |
| `EXTRACE_BUILTIN_BSWAP32(v)` | `__builtin_bswap32(v)` | shared-word helpers, on big-endian agents only (§6.2) |
| `EXTRACE_BUILTIN_MEMCPY` / `_MEMSET` | `__builtin_memcpy` / `__builtin_memset` | defaults of `extrace_port_memcpy`/`_memset` |

Rules:

* Every definition is wrapped in `#ifndef NAME`. Toolchain headers often define some
  of these names already: newlib's `<sys/cdefs.h>` defines `__used`, `__aligned`,
  `__packed` and `__section`, and RTOS headers often define `likely`. The first
  definition wins, and the conformance test (§18.6) checks that each one has the
  expected effect.
* Standard C23 features are used directly and are not wrapped: `static_assert`,
  `bool`, `nullptr`, `_Generic`, `typeof`. `alignas` is not used in shared-memory
  structures at all (§6.2).
* Public headers include `extrace_compiler.h` through `extrace_port.h` only, so an
  application sees exactly the macros its port chose.

---

## 14. Configuration reference

Configuration comes from `extrace_user_cfg.h` (optional, per image) and `-D` flags.
`extrace_cfg.h` supplies the defaults and `static_assert`s the consistency rules.

| Option | Default | Meaning |
|---|---|---|
| `EXTRACE_CFG_ENABLE` | 1 | Master switch. 0 makes all macros compile to type-checked no-ops. |
| `EXTRACE_CFG_MAX_EVENTS` | 1024 | Upper bound for any image's catalog (`static_assert`). Also sizes ring blocks in system memory maps (`EXTRACE_RING_BLOCK_SIZE_MAX`). Actual bitmaps are sized from `EXTRACE_EV_COUNT`. |
| `EXTRACE_CFG_MAX_PAYLOAD` | 512 | Max payload bytes per record, 64–65 535 (§5.6). |
| `EXTRACE_CFG_MAX_ARGS` | 16 | Max args for INTS/PRINTF (≤ 16). |
| `EXTRACE_CFG_STR_ARG_MAX` | 64 | Max bytes of a `%s` argument. |
| `EXTRACE_CFG_PUBLISH_IN_ISR` | 1 | Allow threshold publish from ISR context. |
| `publish_thr` in `struct extrace_prod_cfg` (run time, not a macro) | 0 = size/4 | Default publish threshold. |
| `EXTRACE_CFG_WRITE_IN_CS_MAX` | 0 | Encode records ≤ n bytes inside the critical section (§8.5). `MARK` and `NONE` are always written there (§8.13). |
| `EXTRACE_CFG_RESV_ALIGN_MAX` | 8 | Largest payload alignment of the direct reservation API (§8.14). 1 removes alignment support; `align` must be a constant ≤ this value. |
| `EXTRACE_CFG_EMIT_SPECIALIZE` | 1 | Register-argument emitters for 1–3 arguments (§8.13 rule 4). 0 = array emitters only, minimal code size. |
| `EXTRACE_CFG_DISP_STAGE` | 1024 | Dispatcher staging buffer for record decoding (§12.5). Rings whose `pinfo.max_record` exceeds it are not decoded (bulk sinks only). |
| `EXTRACE_CFG_SYNC_PERIOD_TICKS` | 0 (= runtime `min(2^30, freq / 10)`, §8.7) | Periodic `SYNC` from poll. A non-zero value must stay below a quarter of the 32-bit wrap period. |
| `EXTRACE_CFG_LOST_BYTES` | 0 | Include dropped byte count in `LOST`. |
| `EXTRACE_CFG_PRINTF_LOCAL` | 0 | Format on device (§10.7). |
| `EXTRACE_CFG_TEXT_MAX` | 128 | Stack buffer for on-device formatting. |
| `EXTRACE_CFG_SITE_ID_BITS` | 16 | Site ID width. |
| `EXTRACE_CFG_SITE_ID_MODE` | `DESC` | `DESC` (descriptor address >> 5) or `FMT_ADDR` (low bits of format address). |
| `EXTRACE_CFG_META_MODE` | `INFO` | `INFO`, `PHANTOM` or `RODATA` (§10.5). |
| `EXTRACE_CFG_DISP_MAX_RINGS` | 16 | Dispatcher ring slots. |
| `EXTRACE_CFG_FTRACE` | 0 | Function entry tracing emitters ([docs/ftrace.md](docs/ftrace.md)); `extrace_add_image(... FTRACE_ARCH <arch>)` sets it. |
| `EXTRACE_CFG_FTRACE_ON` | 0 | Default state of the `FTRACE` switch (mask bit of system event 4). |
| `EXTRACE_CFG_FTRACE_CALLER` | 0 | Builds the `CALLER` flavors (`FTRACE_CALLER` records, §5.9.7). |
| `EXTRACE_CFG_FTRACE_BASE` | 0 | Subtracted from every recorded address; sent as `INFO.ftrace_base`. |
| `EXTRACE_CFG_STATS` | 1 | Maintain diagnostic counters. |
| `EXTRACE_CFG_DEBUG` | 0 | Internal assertions, canaries, extra validation. |
| `EXTRACE_CFG_TEST_HOOKS` | 0 | Unit-test builds only: lets tests start an epoch at an arbitrary index (for example just below 2^32, §6.6) and inject timer values, so index and timer wrap paths are exercised without moving 4 GiB of data. Never enabled in firmware. |

---

## 15. Host-side tooling

All tools are Python 3.11+, in the `tools/extrace/` package, and use only the standard
library. `pyelftools` is an **optional** dependency, used only for DWARF struct and
enum extraction. It is pure Python and may be vendored.

### 15.1 `extrace-extract`: ELF → metadata

```
extrace-extract fw_a.elf -o fw_a.exmeta.json [--names-c extrace_names.c] [--strict]
```

1. Parse the ELF header (class, endianness, machine) and section headers, and read
   the GNU build-ID note.
2. Locate the `extrace_meta_*` content (mode A: the `.extrace_meta` output section;
   modes B/C: the `__start_`/`__stop_` ranges). Parse the descriptors with the
   target's pointer size and byte order. Resolve string pointers against the section
   contents.
3. Compute site IDs exactly as the target does. **Fail** on duplicate IDs, which can
   only happen in `FMT_ADDR` mode or if `SITE_ID_BITS` is too small.
4. Resolve every `STRUCT` event type and every `enum` referenced in specs or `%{enum}`
   formats via DWARF, or via `extrace_meta_type` descriptors.
5. **Lint**: format specifier versus argument class mismatches, argument count
   mismatches, missing struct types, events never referenced (informational).
6. Emit metadata JSON. Optionally emit `extrace_names.c`, a compact on-device
   event-name table for the text sink.

Metadata JSON (abbreviated):

```json
{
  "extrace_meta": 1,
  "image": { "build_id": "5b1f…", "elf": "fw_a.elf", "machine": "ARM", "endian": "little",
             "ptr_size": 4, "site_id_bits": 16, "layout": "uncached", "ev_count": 212 },
  "groups": { "SCHED": true, "DEBUG": false },
  "events": [
    { "id": 16, "name": "SCHED_SWITCH", "kind": "INTS", "group": "SCHED", "default": true,
      "spec": [ {"name": "prev", "type": "u"}, {"name": "next", "type": "u"}, {"name": "prio", "type": "s"} ] },
    { "id": 25, "name": "TX_DONE", "kind": "STRUCT", "group": "TX", "default": true,
      "ctype": "struct tx_done_info" }
  ],
  "sites": {
    "419": { "event": "LOG_INFO", "fmt": "tx q=%u len=%u st=%{enum tx_status}d",
             "file": "drv/tx.c", "line": 120, "args": ["U32", "U32", "S32"] }
  },
  "types": {
    "struct tx_done_info": { "size": 24, "fields": [
      { "name": "status", "offset": 0, "size": 4, "type": "uint32_t" },
      { "name": "state",  "offset": 4, "size": 1, "type": "enum txq_state" } ] }
  },
  "enums": { "enum txq_state": { "0": "IDLE", "1": "ACTIVE", "2": "STALLED" } }
}
```

### 15.2 `extrace-decode`: stream → events

```
extrace-decode --meta fw_a.exmeta.json --meta fw_b.exmeta.json \
              --in capture.exbf [--format text|jsonl|chrome] [--ring 0,2] [--event 'TX_*']
```

Pipeline:

```
input adapter ──> demux (ring, epoch) ──> record parser ──> metadata binder ──> clock model ──> k-way merge ──> renderer
 (bulk frames,     (gap detection,         (wire grammar,    (by build-id from   (per-ring       (by global      (text, JSON lines,
  raw ring dump,    BOUNDARY resync)        same rules as     INFO / frame)       freq + SYNC)    time)           Chrome/Perfetto trace)
  UART text log)                            the C decoder)
```

* The **record parser** is a direct transcription of §5, including the padding rule
  of §5.8: padding runs are skipped at every record boundary. Golden test vectors
  shared with the C encoder and decoder keep them in lockstep (§18.1).
* The **printf formatter** implements C conversion semantics (flags, width,
  precision, `diouxXcspfFeEgGaA%`) driven by the recorded argument classes, plus the
  extensions from §10.4.
* `--format chrome` emits Trace Event Format JSON, viewable in Perfetto or
  `chrome://tracing`. Rings become tracks. Paired events (`*_ENTER`/`*_EXIT`, or a
  `pair` annotation in the spec) become duration slices, and INTS fields become
  arguments and counter tracks.

### 15.3 Other inputs

* **Raw ring dump** (post-mortem, JTAG or crash dump): `extrace-decode --raw-block
  blk.bin`. The tool reads `pinfo`, `pstate.head` and `cstate.tail` (little-endian, §6.2) from the dump
  and parses `[tail, head)`. With `--all` it parses from `head` forward around the
  ring and recovers older records heuristically: it resyncs at plausible PAD/`SYNC`
  boundaries and labels the recovered records as such.
* **UART text log** from `extrace_sink_text`: lines are parsed back into records
  (IDs, raw args) and then rendered with metadata.

### 15.4 `extrace-ctl` (optional helper)

Translates human-level requests ("enable group TX on ring 2", "disable LOG_DBG",
"reset ring 0 to defaults") into calls of the device control API (§12.10): ring
index, event IDs or `set`/`clr` bitmaps sized for that ring's `ev_count`. It emits
them as JSON request objects, which the platform's host command layer carries to the
device and turns into `extrace_ctl_*` calls. extrace defines no wire protocol of its
own for this channel.

### 15.5 Clock correlation

All producers and the dispatcher read the same SoC-global 32-bit timer, so there is
no drift or rate correlation to compute. Producers record only 32-bit time (§8.7).
The only unknown is which 2^32-tick period each absolute anchor belongs to:

1. Every bulk frame carries `disp_ts`, the dispatcher's extended timer value at the
   time it peeked the window.
2. For each absolute anchor (`SYNC` or `LOST`, 32-bit value `S`) in a frame:
   `ts64 = disp_ts − (uint32_t)(low32(disp_ts) − S)`.
3. Later records of that ring add their 32-bit `tsd` to `ts64`. Every further
   anchor re-checks the result: a mismatch means corruption or lost frames and is
   flagged.

The result is one global 64-bit timeline. `ts / freq` is directly comparable across
all rings, and the k-way merge is exact. The method requires that an anchor reach the
dispatcher within one wrap period of being produced (about 71 minutes at 1 MHz). The periodic `SYNC` and normal
delivery latency satisfy this. If the ring was stalled for longer, the host flags the
affected interval as ambiguous.

Raw post-mortem dumps without frames contain no `disp_ts`. Their rings are aligned
by their low 32 bits relative to the dump's newest record, or by the time of the dump
when the tool is given it.

---

## 16. Performance and memory budget

### 16.1 Fast path costs (estimates for an in-order 32-bit core, to be measured in M6)

| Situation | Cost |
|---|---|
| Event compiled out | 0 instructions, 0 bytes |
| Event disabled at runtime | 1 load + bit test + branch ≈ 2–4 instr, 6–14 B code per site |
| `EXTRACE_MARK` enabled | ≈ 25–35 instr (critical section, no timer read) |
| `EXTRACE` (NONE) enabled | ≈ 35–50 instr + timer read |
| `EXTRACE_INTS` 2 small args | ≈ 50–70 instr + timer read |
| `EXTRACE_PRINTF` 3 small args | ≈ 70–100 instr + timer read |
| `EXTRACE_STRUCT` 64 B | ≈ 50 instr + `memcpy(64)` |
| Critical section length | ≈ 20–40 instr (constant, fast path) |
| Publish (amortized) | 1 write barrier + index store per threshold, independent of size; doorbell adds a completion barrier |

Code size, estimated: producer core ≈ 1.5–2.5 KiB. Dispatcher + decoder ≈ 3–4 KiB.
Reference sinks ≈ 1–2 KiB each.

### 16.2 Memory per producer

| Item | Bytes |
|---|---|
| `struct extrace_prod` (private) | ≈ 90 + 4 × `EXTRACE_MASK_WORDS` (≈ 220 at 1024 events, ≈ 120 at 200) |
| Defaults bitmap (rodata, §4.5) | 4 × `EXTRACE_MASK_WORDS` (8 at 60 events, 128 at 1024) |
| Ring control block in shared memory | 144–384, depending on catalog size (§6.4) |
| Ring data | per ring, user-chosen |
| Format strings, descriptors, names | **0** (INFO mode) |

### 16.3 Ring sizing guidance

`size ≥ peak_rate_bytes_per_s × worst_consumer_latency_s × 2`, rounded up to a power
of two, and at least
`4 × EXTRACE_MAX_RESV`. `pstate.hwm` (high-water mark) and `lost_total` let you
check the sizing on real workloads, and the dispatcher reports both.

### 16.4 Speed techniques used

* Contiguous reservations, so the encoders never check for wrap.
* Compile-time header bytes, sign masks and argument signatures.
* Fully unrolled varint encoder, compile-time pre-encoded headers, the all-small
  argument fast path, register-argument emitters, and in-section writes of `MARK` and
  `NONE` (§8.13).
* Varint lengths from one `clz` where the ISA has it (all RISC-V targets via Zbb);
  an unrolled shift chain otherwise.
* Out-of-line cold paths (`__cold`, `unlikely()`): slow reservation,
  publish, drop accounting.
* No shared-memory access on the fast path except the record bytes themselves.
* Power-of-two rings: every index operation is one add, subtract or AND (§6.6).
* Batched publication and doorbells.
* Hot state and emitters in TCM via port attributes.

---

## 17. Robustness and error handling

| Hazard | Handling |
|---|---|
| Corrupt or inconsistent `head` (`head − tail > size`) | The consumer re-reads `pinfo.magic`, then `pstate.epoch`, each after `rmb`. I2 orders `magic = 0` before I3 resets `head`, and I3 orders the new epoch before I4 restores `magic`, so a `head` of a restarting producer always shows up as an invalid `magic` or a new epoch. If `magic` is invalid, the producer is (re)initializing or dead: detach and wait for A1 to succeed again. If the epoch changed: re-attach. Otherwise mark the ring `ERROR`, skip to `head` (discard the window), count the event and continue. The resume check of A2 uses the same re-check. |
| Malformed record inside a window | The decoder stops at the bad byte and counts an error. The rest of the window is still delivered to bulk sinks, raw, and released. Record sinks resume at the next window, which starts at a record boundary. |
| Producer restarted mid-read | Detected by the epoch re-check after each staged copy (§12.5) and before release (C6). Results are discarded and the ring re-attaches. |
| Version or layout mismatch | Attach refused. Reported via `ring_event(ATTACH_FAILED)` and stats. |
| Consumer stalled | The producer drops and counts. The `LOST` record appears when space returns, and `pstate.lost_total` is visible live. |
| Host stalled | Lossless bulk sinks backpressure into producer drops. Lossy sinks report gaps. Nothing is silently lost: every loss is counted at exactly one place. |
| Clock wrap (32-bit) without poll | Documented requirement. In debug builds, poll measures the interval and asserts. |
| Deep nesting or stuck writer | A reservation fails, counted as lost, while 255 are open (§8.5). A stuck writer only delays publication, and `hwm` and the publish counter in `extrace_prod_stats()` reveal it. |
| Panic | §8.10: committed data is published, status is set, and post-mortem decoding is supported. |

Error reporting follows the kernel convention: every `int`-returning API returns 0
(or a non-negative count) on success and a **negated** error code on failure,
`return -EXTRACE_EINVAL;`. The codes are positive constants of `enum extrace_err` in
`extrace_types.h`: `EXTRACE_EINVAL`, `EXTRACE_ENOTREADY`, `EXTRACE_EEPOCH`,
`EXTRACE_EVERSION`, `EXTRACE_ECORRUPT`, `EXTRACE_EBUSY`, `EXTRACE_ENOSPC`. Emit
functions return nothing on the macro path. Drops are recorded via `LOST`.

---

## 18. Verification strategy

### 18.0 Test infrastructure and coverage requirement

* **CMake + CTest.** Every test in this chapter that runs on the host is a CTest
  test, registered by the top-level `CMakeLists.txt` when `EXTRACE_BUILD_TESTS=ON`
  (the default for a top-level build). `ctest --test-dir build` runs everything.
* **Sanitizers are on in unit-test builds.** The `test` CMake preset builds with
  `-fsanitize=address,undefined -fno-omit-frame-pointer
  -fno-sanitize-recover=all`, so any undefined behaviour fails the test. A separate
  `tsan` preset builds the concurrency suite (§18.2) with ThreadSanitizer, because
  ASan and TSan cannot be combined. Tests use the POSIX and wbsim ports.
* **Unit tests cover the entire functionality.** Every public function, every macro
  of the producer API, every wire kind and system record, every configuration
  option of §14 that changes code paths, and every row of the interleaving table
  §7.6.1 has at least one test. A `coverage` preset (gcc `--coverage` or clang
  source-based coverage) reports line and branch coverage of `src/` and
  `include/`. CI requires 100 % function coverage and ≥ 95 % branch coverage.
  Uncovered branches must be justified in a reviewed exclusion list (for example
  `EXTRACE_PORT_ASSERT` failure arms).
* **Test framework.** A small, dependency-free in-tree harness
  (`tests/harness/extrace_test.h`, auto-registering tests through a linker section
  or constructor, with `TEST()` and `EXPECT_*`/`ASSERT_*` macros). Python tooling
  uses `pytest`.
* **Configuration matrix.** The unit suite is built several times with different
  `extrace_user_cfg.h` variants (deferred and local printf,
  `WRITE_IN_CS_MAX`, `PUBLISH_IN_ISR=0`, `LOST_BYTES`, `CFG_ENABLE=0`, smallest and
  largest ring sizes).

### 18.1 Unit tests (host, POSIX port)

* **Varint/zigzag** (`tests/varint/`, the module alone): exhaustive over 0..2^24 and boundary values up to 2^64−1 per
  commit, and exhaustive over the whole 32-bit range in the nightly job. Every
  encoder and length variant of §8.13 (CLZ and shift-chain lengths, the unrolled
  encoder, the all-small path, the 64-bit split) is checked against a reference
  encoder. Decoder: rejection of over-long input, of a fifth byte above `0x0F` in
  32-bit fields, and agreement of the unchecked and checked paths.
* **Index arithmetic** (§6.6): epochs started (via `EXTRACE_CFG_TEST_HOOKS`) with the
  index just below 2^32, so reservation, publish, peek, release and tail refresh all
  cross the index wrap. Attach rejects data sizes that are not a power of two.
* **32-bit timer wrap**: injected timer values across 2^32; deltas, periodic
  `SYNC`, `LOST` anchors and the dispatcher's 64-bit re-basing.
* **Golden vectors**: a shared JSON file of (API call → expected bytes) checked by
  both the C encoder tests and the Python parser tests, so C and Python decoding
  never diverge. It includes PADN records for every gap length class (§5.8).
* **Block layout**: `*_OFF` values and `EXTRACE_CTL_SIZE` for catalogs of 1, 32, 33
  and 1024 events checked against §6.4; every shared word has exactly one writer;
  the data area is 8-byte aligned; a big-endian cross build writes `pinfo`, `pstate`
  and `cstate` byte-identical to a little-endian one.
* **Store-only producer** (§8.8.1): with wbsim trapping loads from ring memory in
  producer context, every emitter, `PAD`/`LOST`/alignment path, cancel and publish
  runs without a single ring load, and no control word is read back by its writer.
* **Reservation model test**: a randomized sequence of reserve/commit/release with
  random sizes, checked against a reference model in Python. Covers wrap with every
  gap size 0..max, including the over-long PADN gaps of 130 and 16 387 bytes
  (§5.8), the exactly-full and empty boundaries, and `LOST` insertion.
* **Padding recovery** (§5.8): a reference producer model with aligned
  reservations (`align` 1–8), wrap gaps, `LOST` prefixes and cancels, started at
  random indices so it wraps the ring. Its windows are decoded by the C decoder, the
  budget-clamp walk and the Python parser, and every committed payload must come
  back byte-exact, with no padding delivered as an event. The model was prototyped
  in Python during review round 3: 12 000 trials over 64–1024-byte rings
  (3 028 wraps, 14 581 aligned records, 4 153 cancels, 6 931 `LOST` prefixes)
  recovered every payload. Golden vectors include aligned and cancelled records.
* **Direct reservation** (§8.14): alignment for every `align` at every ring offset,
  including across the wrap, built with `EXTRACE_CFG_RESV_ALIGN_MAX` = 1, 4 and 8
  (with 1, a non-constant or too-large `align` must fail to compile, and
  `extrace_reserve_aligned` must not be linked); payload pointers aligned in memory; `PAD1` alignment
  bytes parse as padding and cancels as `SKIP`, keeping later timestamps; `len` above the limit fails without
  side effects; disabled events do not evaluate `len`.
* **Mask seqlock**: interleaved updates and producer recomputation, including torn
  generations. Emits from a simulated ISR at every step of `poll`, and the debug
  assertion against a second `poll` context.
* **Defaults bitmap** (§4.5): for test catalogs whose sizes cross word boundaries
  (31, 32, 33, 64 and 1024 events), the generated words equal a bitmap built from
  the catalog's ID list, on the host and in the cross-compiled conformance build
  (little- and big-endian). A user macro named like an event must not change the
  result.

### 18.2 Concurrency tests (host)

* Producer thread plus a high-rate **signal handler acting as the ISR**, which calls
  extrace from inside the reserve→commit window. The same runs with the thread
  holding a direct reservation (§8.14) for a long fill, with commits in reverse and
  random order, and with cancels. The consumer must see every committed record
  exactly once and no cancelled one. Port critical section = signal
  masking. A consumer thread validates every record (sequence numbers in the
  payload, strictly increasing timestamps, exact `LOST` counts versus injected
  drops).
* Same scenario under ThreadSanitizer and UBSan, with the POSIX port (shared memory
  as ordinary memory). ThreadSanitizer does not model standalone fences, so in the
  `tsan` preset the port implements `extrace_shm_st32` as a release store and
  `extrace_shm_ld32` as an acquire load (`__atomic`), and the barriers as compiler
  barriers. That is stronger than the fence form and gives TSan the happens-before
  edges for ring data. The other presets keep C11 fences.

### 18.3 Write-buffer simulator port (wbsim)

A port in which each simulated agent has a **private store buffer** in front of the
shared buffer, modelling non-cacheable memory behind posted writes:

* stores enter the agent's buffer and drain to shared memory later, possibly out of
  program order and coalesced, at random points;
* loads return the newest value from the agent's own buffer, otherwise shared
  memory;
* `wmb` drains or orders the buffer as the port contract requires (§7.1), `rmb` and
  `mb` restrict load reordering, and `extrace_port_notify` drains completely;
* control-word stores are 32-bit and single-copy atomic. A sub-word store to a
  control field, or a second writer of a word, is detected and fails the test.

Running the full producer/dispatcher test suite on top of it detects missing or
misplaced barriers and violations of the single-writer-per-word rule (§6.3). This is
the most important test for requirement 2. It runs in CI on every change.

### 18.4 Lifecycle tests

Producer and consumer run as independent agents (threads on the POSIX port, or
simulated agents on wbsim) with a **step scheduler** that can stop either side
between any two shared-memory accesses of §7.6. Tests:

* every row of §7.6.1, as a directed test;
* producer re-initialization with another thread preempted at every point of
  `publish()` and of the reservation slow path: init returns `-EXTRACE_EBUSY` or the
  reservation fails, and nothing of the old epoch is stored in the new one;
* exhaustive interleaving of producer init (I0–I6) with consumer attach (A1–A4) at
  shared-access granularity, and with a producer restart inserted at every point
  of a consumer peek/release (C1–C7);
* random start orders, re-initializations and permanent stops of either side,
  with each side stopping or re-initializing only between its own protocol
  sequences (the memory assumption of §7.6), while the other side is at any step;
  initial memory either random or left consistent by an earlier run; long
  randomized runs.

Expected outcome: no hang, no crash, no sanitizer report, no corrupt record
delivered to a record sink, no garbage configuration applied, and loss limited to
the documented cases (§7.6, "the one unavoidable loss").

### 18.5 Fuzzing

libFuzzer/AFL on `extrace_dec_next` and `extrace_cons_peek` with arbitrary
`pinfo`/`pstate`/data contents. Hypothesis-based fuzzing of the Python decoder.

### 18.6 Toolchain conformance

* Build matrix: gcc and clang × `-O0/-O2/-Os` × LTO on/off × the supported
  architectures and code models.
* Metadata mode test (§10.5): site IDs computed on the target equal the extractor's
  IDs.
* **Disabled-event cost check**: a script disassembles a probe function that
  contains one disabled `EXTRACE_INTS` and asserts an instruction-count budget, so
  regressions in the fast path fail CI.
* **Encoder cost check**: the same script asserts instruction budgets for the varint
  paths of §8.13 (1- and 2-byte encodes, the length helpers, and the `INTS` two-argument
  write phase) for Cortex-M0+, Cortex-M4 and RV32 with Zbb and Zce.
* Link-isolation check: a producer-only image must not pull in consumer symbols, and
  the reverse.

### 18.7 On-target

Port conformance test (§13.5), cycle measurements for §16.1, a long-duration soak
with all rings at high rate and an artificially slow host, and post-mortem decode
after an injected crash.

---

## 19. Source tree, build and coding style

### 19.1 Source tree

```
extrace/
├── CMakeLists.txt              # top-level build; options in §19.2
├── CMakePresets.json           # dev, test (ASan+UBSan), tsan, coverage, cross-* presets
├── DESIGN.md
├── include/extrace/
│   ├── extrace.h               # umbrella for producers: cfg + events + prod API
│   ├── extrace_cfg.h           # defaults, validation static_asserts
│   ├── extrace_types.h         # kinds, arg classes, enum extrace_err
│   ├── extrace_varint.h        # standalone varint/zigzag module: lengths, encode, decode (§8.13)
│   ├── extrace_wire.h          # record grammar helpers on top of extrace_varint.h (headers, kinds)
│   ├── extrace_shm.h           # shared-memory layout (pinfo/pstate/cstate), adv/dist
│   ├── extrace_events.h        # X-macro expansion of user extrace_events.def
│   ├── extrace_args.h          # _Generic argument classification, arg-count macros
│   ├── extrace_meta.h          # metadata section descriptors
│   ├── extrace_prod.h          # producer API + macros
│   ├── extrace_cons.h          # ring reader API
│   ├── extrace_rec.h           # record decoder API
│   ├── extrace_disp.h          # dispatcher + sink interface
│   └── extrace_ftrace.h        # function entry tracing: flavors, patcher, emitters
├── src/
│   ├── extrace_prod.c
│   ├── extrace_cons.c
│   ├── extrace_rec.c
│   ├── extrace_disp.c
│   ├── extrace_ftrace.c                     # patcher of function entry pads
│   └── sinks/{extrace_sink_bulk.c, extrace_sink_text.c, extrace_sink_stats.c, extrace_sink_mem.c}
├── port/
│   ├── common/extrace_compiler.h            # kernel-style compiler macros (§13.7)
│   ├── template/extrace_port.h
│   ├── posix/extrace_port.h
│   ├── wbsim/{extrace_port.h, wbsim.c}      # write-buffer reordering simulator
│   ├── arch/{armv7m,rv32}/                  # ftrace trampolines and patch encoders
│   ├── arm-m/ arm-r/ arm-a/                 # ARM reference ports
│   └── riscv/                               # RISC-V reference port (Zbb, Zce)
├── linker/extrace_meta.ld                   # INCLUDE-able script fragment (modes A/B)
├── linker/extrace_ftrace.ld                 # keeps the ftrace site table in the ELF only
├── cmake/
│   ├── extrace.cmake                        # extrace_add_image() (extrace_extract_meta() to come, §15)
│   └── toolchains/{arm-none-eabi-gcc,arm-none-eabi-clang,riscv32-gcc,riscv32-clang}.cmake
├── tools/extrace/
│   ├── elf.py  dwarf.py  meta.py            # stdlib ELF reader, optional DWARF via pyelftools
│   ├── varint.py  wire.py                   # varint module (mirrors extrace_varint.h), record parser
│   ├── printf.py  clock.py                  # C-printf formatter, clock model
│   ├── extract.py  decode.py  ctl.py        # CLI entry points
│   └── tests/                               # pytest
├── tests/
│   ├── harness/                             # minimal in-tree test framework
│   ├── unit/ concurrency/ wbsim/ lifecycle/ fuzz/ golden/
│   ├── varint/                              # varint module tests and encoder cost probes
│   ├── ftrace/                              # trampolines and patcher on unicorn and QEMU
│   ├── perf/                                # producer instruction counts on emulators
│   └── port_conformance.c
└── examples/
    ├── two_producers_one_dispatcher/        # host-simulated multi-agent demo
    └── events/extrace_events.def
```

### 19.2 Build system: CMake

CMake (≥ 3.25) is the default and only supported build system. The library is
source-distributed and compiled **per image**, so that each image's port header,
event catalog and configuration apply and port hooks inline.

* **Targets.** `extrace::prod`, `extrace::cons`, `extrace::rec`, `extrace::disp` and
  one target per reference sink, as `OBJECT` or `STATIC` libraries. Each image links
  only what it needs, and the link-isolation check (§18.6) builds producer-only and
  consumer-only images.
* **Per-image integration** through `cmake/extrace.cmake`:

  ```cmake
  extrace_add_image(fw_a
      PORT_DIR      ${CMAKE_CURRENT_SOURCE_DIR}/port         # extrace_port.h
      EVENTS_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/events       # extrace_events.def
      CFG_DIR       ${CMAKE_CURRENT_SOURCE_DIR}/cfg          # extrace_user_cfg.h, optional
      COMPONENTS    prod                                     # prod cons rec disp sink_*
      DEFINES       EXTRACE_CFG_META_MODE=EXTRACE_META_INFO  # optional, §10.5, §14
      FTRACE_ARCH   armv7m                                   # optional: armv7m or rv32
      SOURCES       port.c)                                  # optional extra sources
  target_link_libraries(fw_a.elf PRIVATE fw_a::extrace)
  ```

  The helper adds the include paths, `-std=c23`, the warning set below and
  `-fpatchable-function-entry=0` (where the compiler has it). The image's own
  link adds the linker fragments (`INCLUDE extrace_meta.ld` for modes A and B,
  `extrace_ftrace.ld`) and `--build-id`. The post-link step
  `extrace_extract_meta(fw_a.elf)` (fw_a.exmeta.json, keyed by build ID) comes
  with the host tools (§15) and is not in `extrace.cmake` yet.
* **Options.** `EXTRACE_BUILD_TESTS` (default ON for a top-level build),
  `EXTRACE_SANITIZE` (`address,undefined` by default in the `test` preset; `thread`
  in the `tsan` preset), `EXTRACE_COVERAGE`, `EXTRACE_BUILD_TOOLS` (Python tool
  tests through CTest), `EXTRACE_WERROR` (ON in CI).
* **Presets.** `dev` (host, POSIX port, `-O0 -g`), `test` (ASan+UBSan, all unit
  tests), `tsan`, `coverage`, `release-check` (`-O2`/`-Os` and LTO, disabled-event
  cost check), and cross presets for ARM and RISC-V with gcc and clang toolchain
  files.
* **Compiler flags.** `-std=c23` (or `-std=gnu23`), `-ffreestanding` for firmware,
  `-g` for DWARF struct layouts. The library compiles warning-free with
  `-Wall -Wextra -Wconversion -Wshadow -Wcast-align -Wstrict-prototypes` on
  gcc ≥ 13 and clang ≥ 17, and with `-Werror` in CI.

### 19.3 Coding style

The C code follows the **Linux kernel coding style** (`Documentation/process/coding-style.rst`),
with one deliberate change: **indentation is 4 spaces, never tabs**.

* **Naming.** `lower_snake_case` for functions, variables, struct tags and enum
  constants' prefixes; `UPPER_SNAKE_CASE` for macros and enum constants. Every
  public identifier starts with `extrace_` / `EXTRACE_`. Internal helpers in a `.c`
  file are `static` and may drop the prefix. Short local names (`i`, `n`, `p`,
  `flags`) are fine.
* **No typedefs** for structs or enums: `struct extrace_prod`, `enum extrace_err`.
  Fixed-width integer types come from `<stdint.h>` (`uint32_t`, not `u32`), because
  the library must build against any freestanding C library.
* **Layout.** Opening brace of a function on its own line, other opening braces on
  the same line; `else` on the closing-brace line; one statement per line; spaces
  around binary operators; line length 100 columns as a soft limit (kernel default).
  A `.clang-format` file in the repository encodes these rules (kernel settings with
  `UseTab: Never`, `IndentWidth: 4`, `ContinuationIndentWidth: 4`), and CI checks it.
* **Comments.** `/* ... */` only, kernel-style multi-line blocks. Public functions get
  a kernel-doc comment (`/** extrace_prod_init() - ... */`).
* **Errors.** Return `0` or a negated `EXTRACE_E*` code (§17). Use `goto`-based
  cleanup where a function has several exit paths.
* **Compiler extensions** only through the macros of §13.7 (`__used`,
  `__maybe_unused`, `__aligned()`, `likely()`, ...), never spelled directly.
* **Inline functions before macros.** Use `static inline` (or `static __always_inline`
  on hot paths) for all helpers: they type-check their arguments, evaluate them
  once, and are debuggable. A macro is used **only** where a function cannot do the
  job or would be measurably less efficient:
  * token pasting on event names (`EXTRACE_ON(EV)`, `EXTRACE_CT_##EV`), and the
    X-macro catalog expansion;
  * argument-type dispatch with `_Generic` and argument counting with `__VA_ARGS__`
    (`EXTRACE_AC`, `EXTRACE_SIG_LO`, `EXTRACE_NARGS`);
  * code that must not evaluate its arguments when an event is disabled, and must
    disappear entirely when it is compiled out (the `EXTRACE_*` producer API);
  * a per-call-site `static` object (printf site descriptors) or `__FILE__`/`__LINE__`;
  * integer constant expressions needed for array sizes, `static_assert` and static
    initializers (`EXTRACE_CTL_SIZE`, `EXTRACE_RING_BLOCK_SIZE`, `EXTRACE_HDR`).

  Everything else is an inline function: the enabled-bit test (`extrace_ev_on()`),
  index helpers (`extrace_idx_used()`, `extrace_idx_off()`), varint and zigzag
  helpers, `extrace_site_id()`, shared-memory load/store helpers, and every port hook
  (§13.1). Each remaining macro is kept as thin as possible and forwards to an inline
  function where it can.
* **Python** tooling follows PEP 8 (4 spaces, `ruff` in CI).

## 20. Implementation roadmap

| Milestone | Content | Exit criteria |
|---|---|---|
| **M0** Skeleton + wire core | CMake project, presets (incl. sanitizer `test` preset), test harness, `.clang-format`, `port/common/extrace_compiler.h`, POSIX port; `extrace_varint.h` with its tests and cost probes, `extrace_wire.h`, index helpers (§6.6), `varint.py`, `wire.py`, golden vectors | CI builds with gcc and clang; varint/zigzag/index/record tests pass in C (under ASan+UBSan) and Python |
| **M1** Producer + metadata | catalog X-macros, macros, emitters, reserve/commit/publish, POSIX port, metadata sections + linker fragment, toolchain conformance test | unit + model tests pass; site IDs verified for gcc/ld and clang/lld |
| **M2** Consumer + lifecycle | `extrace_cons`, attach/epoch protocol, mask/command protocol, step-scheduler lifecycle tests | every row of §7.6.1 covered; exhaustive init/attach interleavings pass |
| **M3** Memory ordering | wbsim port, full suite on wbsim | zero failures over long randomized runs |
| **M4** Dispatcher + sinks | dispatcher, decoder, bulk/text/stats/mem sinks, bulk frames | multi-ring demo with simulated host transport |
| **M5** Host tools | extract (ELF + DWARF), decode (text/jsonl/chrome), clock model, ctl | end-to-end decode of the demo; lint catches seeded errors |
| **M6** Target ports | template, ARM (Cortex-M, Cortex-R, Cortex-A), RISC-V (Zbb, Zce); port conformance + cycle and shared-memory stall measurements | §16.1 table filled with measured numbers |
| **M7** Hardening | fuzzing, soak tests, documentation (integration guide, porting guide) | coverage gate of §18.0 met; release 1.0 |

---

## 21. Risks and open questions

### 21.1 Risks

| Risk | Impact | Mitigation |
|---|---|---|
| INFO-section symbol addressing not supported by some code model or linker | site IDs unusable in mode A | Mode B/C fallback; conformance test in M1 |
| Writer preemption on RTOS delays publication | latency spikes, possible drops | `WRITE_IN_CS_MAX`, scheduler-lock option in `irq_save`, sizing guidance |
| Toolchain headers predefine `__used`, `likely` etc. with different meaning | wrong attributes | `#ifndef` guards, conformance test checks each macro's effect (§13.7) |
| A core is reset in the middle of a shared-memory sequence | state outside the memory assumption of §7.6 | out of scope by assumption; the protocol never hangs and the next init of that side recovers |
| An agent maps the trace memory cacheable by mistake | stale or lost data, intermittent | memory type set once in the platform's MPU/MMU/PMA setup; the port conformance test (§13.5) checks the attribute where the core exposes it; `layout_ver` keeps images with a different shared-memory layout from attaching |
| Poll interval or anchor delivery longer than the 32-bit wrap period (71 min at 1 MHz, 43 s at 100 MHz) | wrong timestamps or ambiguous cross-ring alignment | periodic `SYNC`, absolute time in `LOST`; documented poll requirement; debug assertion in `poll`; host flags ambiguous intervals |
| DWARF variance between compilers | struct decode issues | pyelftools handles DWARF 4/5 from gcc and clang; `EXTRACE_TYPE_DESC` fallback |
| Host-side metadata lost or mismatched | undecodable traces | build-ID binding; archive metadata with every build; structural decode still possible |

### 21.2 Resolved decisions

Review round 1:

| # | Question | Decision | Applied in |
|---|---|---|---|
| 1 | Global timer | A shared 32-bit SoC-global timer exists, read via `extrace_port_ts()`. The dispatcher and host re-base the rings onto one 64-bit timeline. (Round 2 removed the producer-side 64-bit extension, see 2.7.) | §5.5, §8.7, §12.5, §12.7, §13.2, §15.5 |
| 2 | Event count | Cap of 1024 events per image. All per-event structures are sized from the catalog's `EXTRACE_EV_COUNT` (producer) or `pinfo.ev_count` (consumer). | §4.2, §6.1, §6.4, §11.1, §12.2, §14 |
| 3 | NMI/FIQ | Not supported. Thread context and regular interrupts only. Target architectures are ARM and RISC-V. | §1.3, §8.9, §13.6 |
| 4 | Overwrite-oldest mode | Not needed. | §1.3 |
| 5 | Host link ordering | Globally in order. No reorder stage; a `pos` discontinuity is always a gap. | §12.7 |
| 6 | Host→dispatcher control | extrace provides a control API. The platform's host command layer calls it. | §12.10, §15.4 |

Review round 2:

| # | Question | Decision | Applied in |
|---|---|---|---|
| 2.1 | Reuse of the etrace prototype | Nothing is ported. Its pitfalls and strong points are recorded. | §1.6 |
| 2.2 | Build and tests | CMake is the build system. Unit tests cover the entire functionality, with a coverage gate; unit-test builds run under ASan+UBSan (TSan separately). | §18.0, §19.2 |
| 2.3 | Ring indices | Free-running 32-bit indices (from etrace), wrapping at a multiple of the ring size so any line-multiple size works and `index % size` is exact. Superseded by 3.6 (power-of-two sizes). | D16, §6.6, §7, §8.4, §11 |
| 2.4 | SMP | Not supported. AMP only: one firmware instance per core, several instances of one image allowed. | §1.3, §8.1, §8.9 |
| 2.5 | Critical section API | `flags = extrace_port_irq_save(); ... extrace_port_irq_restore(flags);` | D15, §8.3, §13.2 |
| 2.6 | Event control granularity | Enable and control bitmaps per subsystem instance (ring); ID-dependent control always names one ring. | D17, §6.1, §9.5, §12.10 |
| 2.7 | 32-bit targets | No 64-bit time on producers: 32-bit timer, 32-bit deltas, absolute 32-bit anchors in `SYNC`/`LOST`; 32-bit argument arrays and signatures in the common case. Typical global timer: 1 MHz or faster. | D5, §5.5, §5.9, §8.7, §8.12 |
| 2.8 | Initialization robustness | Any order and timing of init/re-init on either side, with shared memory either random or consistent. Seqlock attach, epoch selection that avoids the acknowledged epoch, init refused while an emit is in flight. | §7.6, §7.6.1, §18.4 |
| 2.9 | Coding style | Linux kernel style with 4-space indentation; kernel-style compiler macros from the port layer; inline functions preferred over macros. | §13.7, §19.3 |
| 2.10 | Review findings | `pinfo` sized in lines for any L; M0 attach guard for masks and commands; accumulating command bits; TEXT system ID removed; one `ring_id` type (u8) everywhere; MARK size claims corrected; panic vs. in-progress publish defined. | §6.1, §7.5, §7.7, §4.6, §12, §5.11, §8.10 |

Review round 3:

| # | Question | Decision | Applied in |
|---|---|---|---|
| 3.1 | PADN gaps without a minimal encoding (130 and 16 387 bytes) | The writer encodes the PADN length one byte over-long; decoders already accept over-long varints. | §5.2, §5.8, §18.1 |
| 3.2 | Control block layout | Two regions split by owner: producer-owned `pctl` (`pinfo` + `pstate`) and consumer-owned `cctl` (`cstate` + `mask_set` + `mask_clr`), each rounded to whole lines. Saves up to 384 B per ring (128 B for a 60-event catalog at `L = 64`). Superseded by 4.1 (packed, no lines). | §6.1–§6.4, §7.2–§7.4, §16.2 |
| 3.3 | Varint serialization speed | Trade code size for speed: compile-time pre-encoded headers and constants, a fully unrolled shift-test encoder, an all-small fast path for argument lists, register-argument emitters for 1–3 arguments, `MARK`/`NONE` written inside the reservation section, a CLZ-based length, and a decoder with an unchecked fast path. Choices backed by clang codegen measurements and guarded by CI instruction budgets. | §5.2, §8.4, §8.5, §8.13, §14, §16.4, §18.1, §18.6, §22.A |
| 3.4 | RISC-V baseline | RV32 with Zbb and Zce. `__builtin_clz` is one `clz` instruction, and Zce keeps the unrolled encoders about 15 % smaller. | §8.13, §13.6 |
| 3.5 | Varint module | Varint and zigzag code is a standalone header, `extrace_varint.h`, with its own tests and a Python mirror. Record helpers in `extrace_wire.h` build on it. | §2.3, §8.13, §19.1, §20, §22.A |
| 3.6 | Ring size | Power of two required. Indices wrap naturally at 2^32 and every index operation is one add, subtract or AND. Arbitrary sizes were evaluated: they can be made free per record, but need a second number space in the producer, an init-time division and more tests, for flexibility that is not needed. The plain index subtractions of v0.3 are correct under this rule. | D16, §5.10, §6.1, §6.6, §7.3, §7.4, §7.6, §8.1, §8.4, §8.8, §11, §18, §22.B |
| 3.7 | Bulk sinks and DMA | Bulk sinks are synchronous callbacks invoked when new data arrives. A host-interface sink that uses DMA waits for its transfer (or stages the data) inside the callback; this is the only DMA access to the rings. Removes the pending-window queue, `extrace_disp_bulk_done()`, `DISP_MAX_PENDING`, and the double-peek problem of asynchronous windows. DMA-only sinks let the dispatcher skip cache invalidation (there is no cache maintenance at all, 4.1). | D14, §1.5, §7.3, §7.6.1, §12.2, §12.4, §12.6–§12.8, §14 |
| 3.8 | Default-on events | A constant bitmap of `EXTRACE_MASK_WORDS` words in flash, generated at compile time from the catalog through a bit-field union, replaces the list of default-on IDs. It is indexable by word for M3, needs no RAM or init code, and is verified on little- and big-endian targets. | §4.1, §4.5, §7.5, §7.6, §16.2, §18.1 |
| 3.9 | `poll` calling context | `extrace_prod_poll()` runs in one context only (a bottom half or work thread); the configuration doorbell wakes that context instead of calling `poll`. Debug builds assert against overlapping calls. No runtime guard. | §7.5, §8.10, §9.4, §13.2, §18.1 |
| 3.10 | Direct reservation API | Reserve / fill in place / commit is public (`EXTRACE_RESERVE`, `EXTRACE_RESERVE_STRUCT`, `extrace_commit`, `extrace_cancel`), with optional payload alignment through `PAD1` bytes and cancel through `SKIP`. Alignment is a compile-time constant per call and capped per image by `EXTRACE_CFG_RESV_ALIGN_MAX`; unaligned reservations contain no alignment code. All single-shot macros are wrappers over it. Payload bytes are always written with interrupts enabled. | D6, §8.11, §8.14, §14, §18.1, §18.2 |

Review round 4 (non-cacheable shared memory):

| # | Question | Decision | Applied in |
|---|---|---|---|
| 4.1 | Memory type of the trace memory | Every agent maps it non-cacheable (preferably normal memory). No cache maintenance, no cache hooks in the port, no system-wide line size, no line padding; barriers and the epoch/seqlock protocol stay. `layout_ver` is 2.0. | §1.7, D7, D8, D13, §6, §7, §13, §18.3 |
| 4.2 | Producer record staging | Evaluated and documented, **not implemented**, not even as an option: staging saves 25–73 % of bus stores but costs about 30 instructions per small record, and header-only staging gains almost nothing. The producer writes directly and store-only; every write path was audited for read-modify-write, and own control words are stored from local mirrors. | §6.3, §7.5, §7.7, §8.8.1, §8.13, §13.5, §14, §22.A |
| 4.3 | Consumer reads | The record path copies windows into a local staging buffer with word loads before decoding. Bulk sinks read by DMA or `memcpy`. | §11.5, §12.5, §14 |
| 4.4 | Byte order | Tuned for little-endian: shared control words, the bulk frame header and float printf arguments are little-endian, so little-endian agents use plain accesses. Big-endian agents stay correct with one byte swap per control-word access; `pinfo` is a little-endian byte image stored with word stores. | D18, §5.1, §6.2, §6.3, §7.1, §13.6, §13.7, §15.3, §18.1 |

Review round 5 (final review before implementation):

| # | Finding | Fix | Applied in |
|---|---|---|---|
| 5.1 | `extrace_put_hdr` tested bit 31, which count 2 also sets, and stored a stray byte after every 2-byte header (outside the reservation for `MARK`) | Test bit 30 | §22.A |
| 5.2 | Re-init raced with a preempted `publish()` and with the reservation slow path | I0 also refuses while `publishing`; the slow path re-checks `READY` and the epoch; `init` never overlaps `poll` | §7.6, §8.4, §8.10, §18.4 |
| 5.3 | Record sinks could see bytes of a restarted producer before C6 | Epoch re-check after each staged copy | §7.3, §7.6, §12.4, §12.5, §17 |
| 5.4 | Default staging buffer smaller than the default record; `pinfo.max_record` overflowed 16 bits | `DISP_STAGE` 1024; 32-bit `max_record`; rings with larger records are not decoded; `EXTRACE_MAX_RESV` bounds wrap gaps and ring size | §5.6, §5.8, §5.10, §6.2, §7.6, §12.5, §14, §16.3 |
| 5.5 | Consumer thresholds never reached a (re)started producer | Implicit `THR` at the first poll after attach; doorbell after init | §7.6, §7.7, §8.1, §8.10 |
| 5.6 | Consumer had no storage for its bitmap mirrors | Caller-provided mirror array at attach | §6.3, §7.5, §7.6, §11.1, §11.2 |
| 5.7 | A budget below one record could starve a ring | A window always holds at least one record; copy mode returns `-EXTRACE_ENOSPC` | §11.2, §11.3, §11.5 |
| 5.8 | `pos` restarted at 0 when a consumer resumed an epoch | `stream_pos` starts at `tail`; `ATTACH` frame flag | §6.6, §7.6, §12.7 |
| 5.9 | Cacheable local ring vs DMA bulk sinks; Device memory vs unaligned `memcpy` | Local ring cacheable only without DMA sinks; Device mappings need an aligned-only port `memcpy` | §6.3, §8.8.1, §13.2, §13.3, §13.6 |
| 5.10 | Concurrent `FRAG` transfers of the same event and size were ambiguous | `xfer_id` in every fragment | §5.9.6, §8.1 |
| 5.11 | `inflight` could overflow with open direct reservations | A reservation fails, counted as lost, while 255 are open | §8.4, §8.5, §8.14, §17 |
| 5.12 | ThreadSanitizer does not model standalone fences | Acquire/release helpers in the `tsan` preset | §18.2 |
| 5.13 | Smaller items | `cmd_seq_ack` starts at the current `cmd_seq`; rule 8 lists the init read-back; records before the first anchor flagged; stale text removed | §6.3, §7.6, §12.5, §16, §21.2, §22.D, D9 |

### 21.3 Open questions

1. **Security**: should ring blocks be protected so that a faulty subsystem cannot
   corrupt other rings? The design already isolates rings per block. An MPU, SMMU or
   PMP configuration could enforce it.
2. **1-byte markers**: user event headers are always at least 2 bytes because IDs
   0–15 are reserved. Should a small range of 1-byte user IDs (for example by
   shrinking the system range to 0–7) be offered for the hottest `MARK` events?
3. **Pre-arming**: should the consumer be able to configure overrides before the
   producer has ever started, so that events from the first instructions after
   boot follow the host's settings? Today a producer applies overrides only after
   the consumer has attached to its current epoch (§7.5 M0).
4. **Cacheable trace memory**: should agents be allowed to map the trace memory
   cacheable, with cache maintenance hooks in the port? Today every agent maps it
   non-cacheable (§1.7), which keeps the port free of cache hooks.

---

## 22. Appendices

### 22.A Reference varint routines

The routines below are the reference implementation of §8.13. They were compiled
warning-free with `-Wall -Wextra -Wconversion` for Cortex-M4, Cortex-M0+ and RV32
with Zbb and Zce, and checked against a reference encoder under ASan and UBSan.

```c
/*
 * extrace_varint.h: standalone varint module (§8.13). Needs only <stdint.h> and the
 * compiler macros (likely, unlikely, __always_inline). It knows nothing about
 * records, rings or ports. Producer-side routines are 32-bit only.
 */
#include <stdint.h>

#ifndef EXTRACE_VARINT_CLZ32           /* the port may define it (extrace_port_clz32) */
#if defined(__ARM_FEATURE_CLZ) || defined(__riscv_zbb)
#define EXTRACE_VARINT_CLZ32(v)     ((unsigned int)EXTRACE_BUILTIN_CLZ32(v))
#endif
#endif

#ifdef EXTRACE_VARINT_CLZ32
static __always_inline unsigned int extrace_uv_len32(uint32_t v)
{
    /* one clz; 9 * c is a shift-add */
    return (352u - 9u * EXTRACE_VARINT_CLZ32(v | 1u)) >> 6;
}
#else /* ARMv6-M, ARMv8-M Baseline */
static __always_inline unsigned int extrace_uv_len32(uint32_t v)
{
    if (likely(!(v >> 7)))
        return 1;
    if (!(v >> 14))
        return 2;
    if (!(v >> 21))
        return 3;
    return (v >> 28) ? 5 : 4;
}
#endif

/* Fully unrolled shift-test chain: one shift and one branch per byte. */
static __always_inline uint8_t *extrace_put_uv32(uint8_t *restrict p, uint32_t v)
{
    uint32_t t = v >> 7;

    if (likely(!t)) {
        p[0] = (uint8_t)v;
        return p + 1;
    }
    p[0] = (uint8_t)(v | 0x80u);
    v = t >> 7;
    if (likely(!v)) {
        p[1] = (uint8_t)t;
        return p + 2;
    }
    p[1] = (uint8_t)(t | 0x80u);
    t = v >> 7;
    if (!t) {
        p[2] = (uint8_t)v;
        return p + 3;
    }
    p[2] = (uint8_t)(v | 0x80u);
    v = t >> 7;
    if (!v) {
        p[3] = (uint8_t)t;
        return p + 4;
    }
    p[3] = (uint8_t)(t | 0x80u);
    p[4] = (uint8_t)v;
    return p + 5;
}

static __always_inline uint32_t extrace_zz32(int32_t s)
{
    return ((uint32_t)s << 1) ^ (0u - ((uint32_t)s >> 31));
}

/*
 * extrace_uv_len64, extrace_zz64, extrace_unzz32/64 and extrace_get_uv64 follow the
 * same patterns. 64-bit value on a 32-bit core: only 32-bit shifts.
 */
static inline uint8_t *extrace_put_uv64(uint8_t *restrict p, uint32_t lo, uint32_t hi)
{
    uint32_t b4, rest;

    if (likely(!hi))
        return extrace_put_uv32(p, lo);
    p[0] = (uint8_t)(lo | 0x80u);
    p[1] = (uint8_t)((lo >> 7) | 0x80u);
    p[2] = (uint8_t)((lo >> 14) | 0x80u);
    p[3] = (uint8_t)((lo >> 21) | 0x80u);
    b4 = (lo >> 28) | ((hi & 7u) << 4);
    rest = hi >> 3;
    if (!rest) {
        p[4] = (uint8_t)b4;
        return p + 5;
    }
    p[4] = (uint8_t)(b4 | 0x80u);           /* built in a register: no read-back */
    return extrace_put_uv32(p + 5, rest);
}

/*
 * Consumer-side decoder: rejects encodings longer than 5 bytes and a fifth byte
 * that does not fit in 32 bits. Returns the bytes consumed, or 0 on error.
 */
static inline unsigned int extrace_get_uv32(const uint8_t *p, const uint8_t *end,
                                            uint32_t *out)
{
    uint32_t v = 0;
    unsigned int shift = 0, n = 0;

    if (likely(p < end && !(p[0] & 0x80u))) {       /* 1-byte fast path */
        *out = p[0];
        return 1;
    }
    if (likely(end - p >= 5)) {                     /* unrolled, no bounds checks */
        v = p[0] & 0x7Fu;
        if (!(p[1] & 0x80u)) { *out = v | (uint32_t)p[1] << 7; return 2; }
        v |= (uint32_t)(p[1] & 0x7Fu) << 7;
        if (!(p[2] & 0x80u)) { *out = v | (uint32_t)p[2] << 14; return 3; }
        v |= (uint32_t)(p[2] & 0x7Fu) << 14;
        if (!(p[3] & 0x80u)) { *out = v | (uint32_t)p[3] << 21; return 4; }
        v |= (uint32_t)(p[3] & 0x7Fu) << 21;
        if (p[4] > 0x0Fu)                           /* continuation or > 32 bits */
            return 0;
        *out = v | (uint32_t)p[4] << 28;
        return 5;
    }
    while (p + n < end && n < 5) {                  /* checked tail of a window */
        uint8_t b = p[n++];

        if (n == 5 && b > 0x0Fu)
            return 0;
        v |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) {
            *out = v;
            return n;
        }
        shift += 7;
    }
    return 0;
}
```

The record-level helper that stores a pre-encoded header lives in `extrace_wire.h`,
which includes `extrace_varint.h`:

```c
/* extrace_wire.h (excerpt) */
/* Header pre-encoded at compile time: bytes in bits 0..23, count in bits 30..31. */
static __always_inline uint8_t *extrace_put_hdr(uint8_t *restrict p, uint32_t henc)
{
    p[0] = (uint8_t)henc;
    p[1] = (uint8_t)(henc >> 8);            /* user headers: always 2 bytes */
    if (unlikely(henc & (1u << 30)))        /* count 3 (0b11): IDs >= 2048; count 2 is 0b10 */
        p[2] = (uint8_t)(henc >> 16);
    return p + (henc >> 30);
}
```

### 22.B Worked ring example (size 64, wrap)

```
state:  size = 64, mask = 63
        tail = 104 (phys 40), head = 122 (phys 58). Next record needs 9 bytes.
fit(9): used     = 122 - 104 = 18
        end_room = 64 - 58 = 6           -> 9 does not fit: pad = 6
        used + pad + need = 18 + 6 + 9 = 33 <= 64   -> OK
reserve: pad_off = 58, start_off = 0
         head = 122 + 15 = 137 (phys 9)
writer:  data[58] = 0x04, data[59] = 0x04 (PADN, n = 4) ; data[60..63] untouched
         record encoded at data[0..8]
consumer window after publish: avail = 137 - 104 = 33, off = 104 & 63 = 40
         span0 = [40, 64) (24 B), span1 = [0, 9) (9 B)
         span0 ends with the PADN record ; parser skips it ; span1 parsed normally
```

PADN check: `1 (hdr) + 1 (uvarint 4) + 4 == 6` ✓. Across the 32-bit wrap nothing
changes: with `tail = 0xFFFFFFF0` and `head = 0x00000010`, `used = 0x20` and the
physical offsets are `0x30` and `0x10`.

### 22.C Nested reservation timeline

```
t0  T: reserve A (8 B)  head 100->108  inflight 1   committed 100
t1      ISR: reserve B (5 B)  head 108->113  inflight 2
t2      ISR: write B ; commit -> inflight 1 (committed stays 100)
t3      ISR: commit sees pend < thr, no publish ; return
t4  T: write A ; commit -> inflight 0, committed = 113 ; pend 13 B
t5  T: (threshold reached) publish [100, 113): wmb, pstate.head = 113
```

### 22.D Glossary of on-wire constants

| Constant | Value |
|---|---|
| `EXTRACE_SHM_MAGIC` | `0x52545845` ("EXTR") |
| Bulk frame magic | `0x46425845` ("EXBF") |
| Wire version | 1 |
| Layout version | 2.0 |
| Kinds | NONE 0, MARK 1, INTS 2, PRINTF 3, BUF 4, STRUCT 5, STR 6, EXT 7 |
| System IDs | PAD 0, SYNC 1, LOST 2, INFO 3, (4 reserved), CFG 5, PANIC 6, FRAG 7 |
| Arg classes | U32 1, S32 2, U64 3, S64 4, F32 5, F64 6, STR 7, PTR 8, CSTR 9 |
| EXT types | FRAG 1 |
