# Wire format

This document specifies the byte stream that a producer writes into its ring, and
how the producer lays out each record. It is the reference for anyone who writes a
decoder: host tools, post-mortem parsers, or a consumer that does not use
`extrace_rec.h`. [DESIGN.md](../DESIGN.md) §5 is the normative source. Section
numbers (§) refer to this document unless marked "of DESIGN".

Every hex dump in this document is real output of the producer (POSIX port, test
catalog of `tests/common/extrace_events.def`), not a hand-made example.

[wire-encodings.html](wire-encodings.html) shows every encoding below interactively, byte by byte: a record builder, a decoder for pasted hex, and a walk through a 128-byte ring. Open it in a browser.

## 1. Overview

* A ring carries a **byte stream of records**. Records are packed with no
  alignment and no padding between them, except for the explicit padding
  records of §7.
* **A record never spans the end of the ring.** When it does not fit, the
  producer fills the rest of the ring with padding and puts the record at
  offset 0 (§7.2).
* All integers are **unsigned LEB128 varints** (§2). Signed integers are
  **zigzag**-encoded first.
* Every record can be skipped without metadata: the header gives the kind, and
  the kind says whether a length follows.
* The stream is endian-neutral, with these exceptions:
  * the bytes of `BUF`/`STRUCT` payloads are copied in the producer's native
    order, which the `INFO` record states;
  * `F32`/`F64` printf arguments are little-endian.
* Zero-filled memory parses as a run of 1-byte padding records, so raw memory
  dumps stay decodable.

## 2. Varint and zigzag

```
uvarint(v): repeat { b = v & 0x7F; v >>= 7; emit(b | (v ? 0x80 : 0)); } until v == 0
zigzag32(s) = (s << 1) ^ (s >> 31)        ; arithmetic shift
zigzag64(s) = (s << 1) ^ (s >> 63)
unzigzag(u) = (u >> 1) ^ -(u & 1)
```

| Value | Bytes | Examples |
|---|---|---|
| 0 – 127 | 1 | `5` → `05`, `127` → `7F` |
| 128 – 16 383 | 2 | `300` → `AC 02`, `2000` → `D0 0F` |
| 16 384 – 2 097 151 | 3 | `70000` → `F0 A2 04`, `0x12345` → `C5 C6 04` |
| up to 2^28 − 1 | 4 | |
| up to 2^32 − 1 | 5 | `0x20001000` → `80 A0 80 80 02` |
| up to 2^64 − 1 | 10 | `1 << 40` → `80 80 80 80 80 20` (6 bytes) |

| Signed | zigzag | Encoded |
|---|---|---|
| 0, −1, 1, −2 | 0, 1, 2, 3 | `00`, `01`, `02`, `03` |
| −3 | 5 | `05` |
| −5 | 9 | `09` |
| −128 | 255 | `FF 01` |

Decoder rules:

* Reject a varint longer than 10 bytes, or longer than 5 bytes for a field that
  is defined as 32-bit.
* Reject a varint that runs past the end of its record.
* Accept over-long encodings (redundant `0x80` continuation bytes). The producer
  writes one itself for two padding sizes (§7.1).

## 3. Record grammar

```
record       = pad-record / event-record
pad-record   = %x00                              ; PAD1: one filler byte
             / %x04 uvarint(n) n*OCTET           ; PADN: skip n more bytes
event-record = hdr [tsd] [len payload]
             / ftrace-short
hdr          = uvarint((event_id << 3) | kind)   ; event_id >= 1
tsd          = uvarint(ts - prev_ts)             ; absent for MARK
len          = uvarint(payload length)           ; absent for NONE and MARK
payload      = len*OCTET
ftrace-short = %x20 tsd uvarint(addr)            ; FTRACE, NONE shape
             / %x21 uvarint(addr)                ; FTRACE, MARK shape
             / %x40 tsd uvarint(addr) uvarint(caller) ; FTRACE_CALLER, NONE shape
             / %x41 uvarint(addr) uvarint(caller)     ; FTRACE_CALLER, MARK shape
```

`ftrace-short` is the one exception to "the kind alone gives the shape". Headers
`20` and `21` (system event 4 `FTRACE` with `NONE` and `MARK`) are followed by
one uvarint with no length field, and headers `40` and `41` (system event 8
`FTRACE_CALLER`) by two (§6.6). A decoder must know this rule to find the next
record.

### 3.1 Kinds

| Kind | Value | `tsd` | `len` + payload | Payload |
|---|---|---|---|---|
| `NONE` | 0 | yes | no | – |
| `MARK` | 1 | no | no | – |
| `INTS` | 2 | yes | yes | sign mask and 1–16 integers (§5.1) |
| `PRINTF` | 3 | yes | yes | site ID and typed arguments (§5.2) |
| `BUF` | 4 | yes | yes | raw bytes |
| `STRUCT` | 5 | yes | yes | raw bytes of a declared C type |
| `STR` | 6 | yes | yes | string bytes without the NUL |
| `EXT` | 7 | yes | yes | `uvarint(ext_type)`, then the body (§6.5) |

`ANY` (8) exists only in the event catalog and never on the wire. A record of an
`ANY` event carries the kind of the macro that emitted it, and `BUF` for a
direct reservation.

### 3.2 Event IDs and header size

| IDs | Use | Header bytes | Example |
|---|---|---|---|
| 0 | padding only: `00` (PAD1) and `04` (PADN) | 1 | |
| 1–15 | system events (§6) | 1 | `SYNC` = 1<<3 \| 2 → `0A` |
| 16–2047 | user events | 2 | ID 18 `INTS` = 146 → `92 01` |
| 2048–262 143 | user events | 3 | ID 3000 `INTS` = 24002 → `C2 BB 01` |

Every user header is at least 2 bytes. A header with ID 0 is valid only as `00`
or `04`. Any other header with ID 0 is corruption.

## 4. How the producer writes a record

### 4.1 One reservation per record

An emitter computes the size of its record before it reserves:

* **`fixed`**: header bytes + `len` bytes + payload bytes. The header length is
  a compile-time constant (§4.4). Payload lengths are computed with a
  branch-free varint length function.
* **The timestamp delta** is not known yet. It is computed inside the critical
  section.

Inside one interrupt-masked critical section the producer then:

1. Reads the timer, if the record carries time, and computes
   `tsd = ts - last_ts` in 32 bits.
2. Adds the size of a pending `LOST` record, if records were dropped since the
   last report (§6.2).
3. Adds the alignment filler `k`, for aligned direct reservations only (§7.3).
4. Checks whether the record fits contiguously before the ring end. If not, it
   reserves the remaining gap as well and places the record at offset 0 (§7.2).
5. Checks the free space against its cached copy of the consumer tail. When
   that fails it refreshes the tail once, and then drops the record and counts
   it as lost.
6. Advances `head` by the whole reservation, and records `last_ts`.

One reservation can therefore cover up to four parts, always in this order:

```
 old head                                          new head
    | wrap gap      | LOST record | k x PAD1 | hdr tsd len payload |
    | (ring end)    | (offset 0, or right here if no wrap)          |
```

### 4.2 Writing

* Small records are written inside the critical section, before interrupts are
  re-enabled:
  * `MARK` and `NONE` always;
  * `INTS`/`PRINTF`/`BUF` when `EXTRACE_CFG_WRITE_IN_CS_MAX` allows.
* All other records are written after interrupts are re-enabled. An `inflight`
  counter tracks the open reservations.
* Writes are **stores only**. The producer never reads back ring memory, which is
  non-cacheable. Each varint is completed in a register before it is stored.
* Byte order inside a reservation: wrap padding at the old position, the `LOST`
  record, alignment `PAD1`s, then the header, `tsd`, `len` and payload.

### 4.3 Commit and publication

A record becomes visible to the consumer only after all of the following:

1. It is committed: the last open reservation closed, so `committed = head`.
2. A write barrier executes.
3. The producer stores the new `head` in shared memory (§7.2 of DESIGN).

A consumer reads only up to the published `head`, so it never sees a partly
written record. Publication happens in batches (`publish_thr`), on urgent
records, or on `extrace_prod_flush()`.

### 4.4 Pre-encoded headers

A header is encoded at compile time into a 32-bit constant (`EXTRACE_HENC`):

| Bits | Content |
|---|---|
| 0–23 | the 1–3 varint bytes of `(id << 3) \| kind` |
| 24–26 | sign mask, only for the register-argument emitters |
| 29 | urgent flag: publish right after commit |
| 30–31 | header length in bytes |

Writing a user header is two byte stores, plus a third for IDs ≥ 2048. Bits
24–29 never reach the wire: the urgent flag only controls publication, and the
sign mask is written as its own payload field.

### 4.5 Timestamps

* `ts` is the SoC-global free-running 32-bit timer, read inside the reservation
  critical section, so ring order and time order agree.
* `tsd` is `(ts - prev_ts) mod 2^32`, where `prev_ts` is the timestamp of the
  previous record in the same ring that carried time.
* `MARK` and padding carry no time and leave `prev_ts` unchanged.
* `SYNC` and `LOST` carry the absolute 32-bit timer value in their payload, with
  `tsd = 0`. A decoder sets `prev_ts = ts_abs` after either one.
* The record that follows a `LOST` record in the same reservation has
  `tsd = 0`, because it was stamped at the same instant.
* The producer emits `SYNC` periodically (default every 100 ms or a quarter of
  the timer wrap period, whichever is shorter), so deltas are never ambiguous.
* A decoder extends time to 64 bits from the absolute anchors. Records before
  the first anchor have no usable time.

## 5. Payloads of user events

### 5.1 `INTS`

```
payload = uvarint(sign_mask) 1*16( uvarint(value_i) )
```

* Bit *i* of `sign_mask` is set when argument *i* has a signed C type. That
  value is zigzag-encoded.
* The mask is a compile-time constant from `_Generic`.
* The argument count is implicit: as many varints as fit in `len`.
* Values are up to 64 bits wide. A 32-bit argument is zero- or sign-extended
  first.
* Fast path: when every value and the mask are below 128, each field is one
  byte, and `len = nargs + 1`.

**`EXTRACE_INTS(T_INTS, 5u, -3)`**, ID 18, 12 ticks after the previous record:

```
92 01  0C  03  02  05  05                              7 bytes
hdr    tsd len sm  v0  v1      sm = 0b10: v1 is signed, zigzag(-3) = 5
```

**`EXTRACE_INTS(T_INTS, 0x12345u)`**, delta 1:

```
92 01  01  04  00  C5 C6 04                            8 bytes
hdr    tsd len sm  v0 = 0x12345
```

**`EXTRACE_INTS(T_INTS, 1ull << 40, -5ll, (int8_t)-128)`**, delta 70 000:

```
92 01  F0 A2 04  0A  06  80 80 80 80 80 20  09  FF 01   16 bytes
hdr    tsd       len sm  v0 = 1<<40         v1  v2
                     sm = 0b110: v1 = zigzag(-5) = 9, v2 = zigzag(-128) = 255
```

### 5.2 `PRINTF`

```
payload = uvarint(site_id) *( arg )
arg     = uvarint(u)              ; U32, U64, PTR, CSTR
        / uvarint(zigzag(s))      ; S32, S64
        / 4OCTET                  ; F32, little-endian IEEE 754 binary32
        / 8OCTET                  ; F64, little-endian IEEE 754 binary64
        / uvarint(n) n*OCTET      ; STR, n <= EXTRACE_CFG_STR_ARG_MAX
```

* The record has **no type tags**. `site_id` identifies a call-site descriptor
  in the ELF, which holds the format string, file, line, and one 4-bit class
  per argument (§10.2–10.3 of DESIGN).
* The host reads the classes from the descriptor to split the arguments. The
  argument classes are:

| Class | Value | C types |
|---|---|---|
| `U32` | 1 | `bool`, unsigned integers up to 32 bits |
| `S32` | 2 | `char`, signed integers up to 32 bits |
| `U64` / `S64` | 3 / 4 | 64-bit integers, and `long` on LP64 |
| `F32` / `F64` | 5 / 6 | `float` / `double` |
| `STR` | 7 | `char *`, `const char *`: the bytes are copied |
| `PTR` | 8 | any other pointer: its address |
| `CSTR` | 9 | `EXTRACE_CSTR(s)`: a constant string logged by address |

* String arguments are truncated to `STR_ARG_MAX` (64) bytes each. If the
  payload would still exceed `MAX_PAYLOAD`, strings are shortened, starting
  from the last one.
* Site IDs depend on where the descriptors are linked. In the examples below
  they are 0, 1, 2 and 3.

**`EXTRACE_PRINTF(T_PRINTF, "q=%u st=%d", 7u, -1)`**, ID 19, delta 2000:

```
9B 01  D0 0F  03  00    07  01                         8 bytes
hdr    tsd    len site  a0  a1 = zigzag(-1)
```

**`EXTRACE_PRINTF(T_PRINTF, "s=%s f=%f g=%g", "abc", 1.5f, -2.25)`**, delta 3:

```
9B 01  03  11  01    03 61 62 63   00 00 C0 3F   00 00 00 00 00 00 02 C0   21 bytes
hdr    tsd len site  STR "abc"     F32 1.5       F64 -2.25
```

**`EXTRACE_PRINTF(T_PRINTF, "tick")`**, no arguments, delta 6:

```
9B 01  06  01  02                                      5 bytes
hdr    tsd len site
```

**`EXTRACE_PRINTF(T_PRINTF, "p=%p", (void *)0x20001000)`**, delta 6:

```
9B 01  06  06  03    80 A0 80 80 02                    10 bytes
hdr    tsd len site  PTR 0x20001000
```

With `EXTRACE_CFG_PRINTF_LOCAL=1`, `EXTRACE_PRINTF` formats on the device and
writes a `STR` record instead.

### 5.3 `BUF`

The payload is raw bytes, copied as is and truncated to `MAX_PAYLOAD`. `len`
gives the stored size, so the host can see a truncation.

**`EXTRACE_BUF(T_BUF, "\x01\x02\x03\x04\x05", 5)`**, ID 20, delta 4:

```
A4 01  04  05  01 02 03 04 05                          9 bytes
```

### 5.4 `STR`

The bytes of the string without the terminating NUL, truncated to `MAX_PAYLOAD`.
`EXTRACE_TEXT` writes the same kind, formatted on the device and truncated to
`EXTRACE_CFG_TEXT_MAX − 1` bytes.

**`EXTRACE_STR(T_STR, "hello")`**, ID 21, delta 4:

```
AE 01  04  05  68 65 6C 6C 6F                          9 bytes
```

### 5.5 `STRUCT`

The bytes of the declared C type in the producer's native layout and byte
order, including compiler padding. The host decodes them with the type
information from the ELF. Its interpretation depends on `len`:

* `len < sizeof(type)`: the struct was truncated (`EXTRACE_STRUCT_N`);
* `len > sizeof(type)`: the struct ends in a flexible array member.

**`EXTRACE_STRUCT(T_STRUCT, &ts)`**, ID 29, with
`struct test_struct { uint32_t a; uint16_t b; uint8_t c; uint64_t d; }` (16 bytes
on a little-endian host):

```
ED 01  04  10  44 33 22 11  66 55  77  00  08 07 06 05 04 03 02 01    20 bytes
hdr    tsd len a            b      c   pad d
```

### 5.6 `NONE` and `MARK`

**`EXTRACE(T_NONE)`**, ID 17, delta 300: a timestamp and nothing else.

```
88 01  AC 02                                           4 bytes
```

**`EXTRACE_MARK(T_MARK)`**, ID 16: an event without time, the smallest record.

```
81 01                                                  2 bytes
```

## 6. System records

System records use IDs 1–15 and 1-byte headers. They are written by the
producer itself and delivered to record sinks like user events. `FTRACE` (4)
and `FTRACE_CALLER` (8) records are written by the function entry trampoline
(§6.6). `SKIP` (9) is a cancelled direct reservation (§7.4). IDs 10–15 are
reserved. A decoder parses them like any other record and ignores IDs it does
not know.

| ID | Name | Header | Kind |
|---|---|---|---|
| 1 | `SYNC` | `0A` | `INTS` |
| 2 | `LOST` | `12` | `INTS` |
| 3 | `INFO` | `1C` | `BUF` |
| 4 | `FTRACE` | `20` / `21` / `22` | `NONE` / `MARK` / `INTS` |
| 5 | `CFG` | `2A` | `INTS` |
| 6 | `PANIC` | `32` | `INTS` |
| 7 | `FRAG` | `3F` | `EXT` |
| 8 | `FTRACE_CALLER` | `40` / `41` / `42` | `NONE` / `MARK` / `INTS` |
| 9 | `SKIP` | `4C` | `BUF` |

### 6.1 Stream start: `INFO`, then `SYNC`

`extrace_prod_init()` writes `INFO` and `SYNC` as the first two records of every
epoch, inside the critical section that makes the producer ready. A consumer
attaching to a new epoch therefore always starts at an anchor.

`INFO` is a `BUF` whose payload is a counted list of varint fields, so it can grow
(decoders ignore fields they do not know):

```
1C  01  16   0D       01    80 04   08     C0 84 3D   07    EA 07  80 20  1E      08   10    00    03  78 74 30  00
hdr tsd len  nfields  wire  layout  flags  ts_freq    ring  epoch  size   ev_cnt  ptr  site  bid   name "xt0"   fbase
                      1     0x200   TS_GL  1 000 000  7     1002   4096   30      8    16    len 0               0
```

| Field | Meaning |
|---|---|
| `nfields` | number of fields that follow: 13 |
| `wire_version` | 1 |
| `layout_version` | shared-memory layout, major << 8 \| minor |
| `flags` | bit 0 big-endian producer, bit 3 global timer (bits 1–2 reserved) |
| `ts_freq_hz` | timer frequency |
| `ring_id`, `epoch`, `ring_size` | identity of this ring and epoch |
| `ev_count` | highest event ID + 1 |
| `ptr_size` | pointer size in bytes |
| `site_id_bits` | width of PRINTF site IDs |
| `build_id` | `uvarint(len)` and the GNU build ID bytes, so the host can find the ELF |
| `name` | `uvarint(len)` and the instance name, up to 8 bytes |
| `ftrace_base` | `EXTRACE_CFG_FTRACE_BASE`, added to the `addr` and `caller` fields of `FTRACE` and `FTRACE_CALLER` (§6.6) |

`INFO` precedes the first anchor, so its `tsd` carries no usable time.

`SYNC` carries the absolute timer value. Its `tsd` is always 0:

```
0A  00   03   00  EC 07                                6 bytes
hdr tsd  len  sm  ts_abs = 1004
```

`SYNC` is emitted at init, periodically by `extrace_prod_poll()`, and on the
consumer command `EXTRACE_CMD_SYNC`.

### 6.2 `LOST`

When a record does not fit, it is dropped and counted inside the critical
section. The **next successful reservation** first writes a `LOST` record, then
its own record, both in the same reservation:

```
12  00  05  00  05     C0 BE 04       92 01  00  02  00  09
hdr tsd len sm  count  ts_abs         INTS (9u) with tsd = 0
    = 0         = 5    = 73 536
```

| Field | Meaning |
|---|---|
| `count` | records dropped since the previous report |
| `ts_abs` | absolute timer value at recovery; it re-anchors time |
| `bytes` | optional third field, with `EXTRACE_CFG_LOST_BYTES=1`: bytes those records would have used |

* The report sits exactly where the gap occurred and is atomic with respect to
  other contexts.
* A decoder places the `LOST` record at `ts_abs`, and the record after it at the
  same time.
* `INTS`/`PRINTF` records that would exceed `MAX_PAYLOAD` (possible only with a
  small `MAX_PAYLOAD`) are also dropped and counted here.

### 6.3 `CFG`

Written by `extrace_prod_poll()` when it applies a new event mask. Its single
value is the mask generation that is now in effect, which marks the exact point
of a configuration change:

```
2A  04  02  00  04                                     5 bytes
hdr tsd len sm  mask_gen = 4
```

### 6.4 `PANIC`

Written by `extrace_prod_panic(reason, pc)` when no reservation is open:

```
32  08  07  00  AD 17          B4 A4 80 40             10 bytes
hdr tsd len sm  reason = 0xBAD pc = 0x08001234
```

### 6.5 `FRAG` (`EXT` type 1)

`EXTRACE_BUF_LARGE` splits a buffer larger than `MAX_PAYLOAD` into fragments.
Each fragment is a separate reservation, so other records may sit between them:

```
payload = uvarint(ext_type = 1) uvarint(xfer_id) uvarint(orig_event_id)
          uvarint(total_len) uvarint(offset) bytes
```

`EXTRACE_BUF_LARGE(T_BUF, big, 1200)` writes three fragments. The first
16 bytes of each are shown:

```
3F  08  80 04  01   01    14    B0 09  00      00 01 02 03 04 05 ...  516 bytes
hdr tsd len    ext  xfer  id 20 total  offset  data (506 bytes)
3F  04  80 04  01   01    14    B0 09  FA 03   FA FB FC FD FE ...     516 bytes, offset 506
3F  04  C4 01  01   01    14    B0 09  F3 07   F3 F4 F5 F6 F7 ...     200 bytes, offset 1011
```

The host reassembles fragments per (ring, epoch, `xfer_id`) in offset order. A
missing fragment, for example one dropped on overflow, marks the buffer
incomplete. Decoders skip unknown `ext_type` values by `len`.

### 6.6 `FTRACE` and `FTRACE_CALLER`

Function entry tracing ([ftrace.md](ftrace.md)) records the return address of
the call from an instrumented function's entry into the trampoline. The address
field is `addr = return address − ftrace_base`, as a uvarint.

The flavor the function was patched with picks one of three shapes. These examples use
`ftrace_base` = 0x20000000 and the return address 0x20001A47, so `addr` = 0x1A47:

```
21  C7 34                                         MARK: no timestamp           3 bytes
20  1D  C7 34                                     NONE: tsd 29                 4 bytes
22  19  0B  00  C7 34  07  AC 02  80 80 81 80 02  ARGS, 3 registers           14 bytes
hdr tsd len sm  addr   a0  a1     a2 = 0x20004000
```

* `21` and `20` are the `ftrace-short` forms (§3). `21` uses the `MARK` kind and
  carries no time, so a decoder gives it the time of the last timed record.
* `22` is an ordinary `INTS` record. The sign mask is 0, `addr` comes first,
  and the argument registers follow as unsigned 32-bit values.
* With base 0 the same address takes 5 bytes: `C7 B4 80 80 02`.

The function entry is `ftrace_base + addr − RET_OFFSET`. `RET_OFFSET` is 7 on
ARMv7-M (Thumb bit included) and 4 on RV32.

With a `CALLER` flavor, the same three shapes use system event 8,
`FTRACE_CALLER`, and carry a second uvarint after `addr`:
`caller = caller's return address − ftrace_base`. With the caller's return
address 0x20000C13 (`caller` = 0xC13):

```
41  C7 34  93 18                                  MARK: no timestamp           5 bytes
40  1D  C7 34  93 18                              NONE: tsd 29                 6 bytes
42  19  06  00  C7 34  93 18  07                  ARGS, 1 register             9 bytes
hdr tsd len sm  addr   caller a0
```

The caller's return address is the one the traced function returns to, so it
points just past the call instruction in the caller (the Thumb bit is set on
ARMv7-M). `extrace_rec_ftrace()` decodes all six shapes.

## 7. Padding

Padding can appear wherever a record may start. A decoder skips any run of
`PAD1`/`PADN` before reading a header. Padding has no time and is never
delivered as an event.

### 7.1 Encoding a gap

A gap of `g` bytes is filled as follows:

* `g = 1`: `PAD1` (`00`).
* `g ≥ 2`: `PADN` (`04`), followed by `n = g − 1 − k` written in exactly `k`
  bytes, where `k` is the smallest length for which `n` fits.

The skipped bytes are not written, so a gap costs at most a few stores.

| Gap | Bytes written | Notes |
|---|---|---|
| 1 | `00` | PAD1 |
| 2 | `04 00` | n = 0 |
| 3 | `04 01` | n = 1, one byte skipped |
| 129 | `04 7F` | n = 127 |
| 130 | `04 FF 00` | n = 127 in 2 bytes: no minimal encoding fits exactly |
| 131 | `04 80 01` | n = 128 |
| 300 | `04 A9 02` | n = 297 |

Two gap sizes, 130 and 16 387, have no minimal encoding. The writer then adds a
redundant continuation byte, which decoders accept (§2).

### 7.2 Wrap

When a record does not fit before the ring end, the rest of the ring becomes
padding and the record starts at offset 0. Here a 44-byte `BUF` record met
39 bytes of room:

```
offset 4057:  04 25 .. (37 bytes not written) ..       PADN, n = 37: 39 bytes
offset 0:     A4 01 0A 28 AB AB ... AB                 BUF, 40 bytes of payload
```

Reading from any record boundary up to the ring end always yields well-formed
records. The consumer gets the two parts as two spans of one window.

### 7.3 Aligned direct reservation

`EXTRACE_RESERVE(EV, len, align, &r)` aligns the **payload address** for
in-place writes of typed data. The filler is 0–7 `PAD1` bytes written before the
header (after a `LOST` record, if any). Here a `MARK` left the head at offset
157, and an 8-byte reservation with `align = 4` needed 3 filler bytes to put the
payload at 164:

```
offset 155:  81 01                                     MARK
offset 157:  00 00 00                                  3 x PAD1
offset 160:  B4 01  04  08  A0 A1 A2 A3 A4 A5 A6 A7     BUF (event T_ANY, ID 22)
                            ^ payload at offset 164
```

The ring's data area starts 8-byte aligned in memory, so an aligned offset is an
aligned address.

### 7.4 Cancelled direct reservation

`extrace_cancel()` overwrites only the record header: all but its last byte
become `PAD1`, the last one the 1-byte header of a `SKIP` record. The `tsd`, the
`len` and the payload are left as they were. A decoder skips the record, payload
included, but adds its `tsd` to the time of the ring: the reservation advanced
the producer's last timestamp, and the deltas of the records after it build on
that. Here a 6-byte `BUF` reservation (event 20, header `A4 01`, `tsd` 4) was
cancelled:

```
00 4C  04 06 55 55 55 55 55 55                         PAD1, SKIP: tsd 4, 6 bytes skipped
^^^^^ written by cancel; the old tsd, len and payload bytes follow
81 01                                                  next record (MARK)
```

A 3-byte header becomes `00 00 4C`. A `SKIP` is never delivered to record
sinks.

## 8. Limits

| Item | Default | Limit |
|---|---|---|
| Payload per record (`MAX_PAYLOAD`) | 512 B | 64–65 535 B, and ≤ ring size / 4 |
| Largest record (`MAX_RECORD`) | 523 B | `MAX_PAYLOAD` + 3 (hdr) + 5 (tsd) + 3 (len) |
| Arguments per `INTS`/`PRINTF` | 16 | 16 |
| String argument | 64 B | `MAX_PAYLOAD` |
| `LOST` record | ≤ 19 B | |
| Alignment filler | 0–7 B | `EXTRACE_CFG_RESV_ALIGN_MAX` − 1 |
| Ring size | power of two | ≥ 64, ≥ 4 × largest reservation, ≤ 2^31 |
| Event IDs | `ev_count` from `INFO` | ≤ 65 534 |

`pinfo.max_record` in shared memory and `MAX_RECORD` tell a decoder the largest
valid record length. A longer record is corruption.

## 9. Decoding procedure

```
prev_ts = none
loop at a record boundary:
    b = next byte
    if b == 0x00: skip 1 byte; continue                     # PAD1
    if b == 0x04: n = uvarint; skip n bytes; continue       # PADN
    h = uvarint; id = h >> 3; kind = h & 7
    if id == 0: corrupt
    tsd = (kind != MARK) ? uvarint : none
    len = (kind >= INTS) ? uvarint : 0
    if h in (0x20, 0x21): len = size of the next uvarint     # FTRACE short (§6.6)
    if h in (0x40, 0x41): len = size of the next 2 uvarints  # FTRACE_CALLER short
    payload = next len bytes                                # must not pass the end
    if id in (SYNC, LOST): prev_ts = payload ts_abs         # absolute anchor
    elif tsd is not none and prev_ts is known: prev_ts += tsd
    ts = (kind == MARK) ? last known time : prev_ts
    deliver (id, kind, ts, payload)       # consumers ignore IDs they do not know
```

The C implementation is `extrace_dec_next()` in
[src/extrace_rec.c](../src/extrace_rec.c). It also places 32-bit anchors on the
dispatcher's 64-bit timeline (`extrace_rebase`) and flags the first record after
a `LOST` report.

## 10. Versioning

* `wire_version` (in `INFO` and in bulk frames) is incremented on incompatible
  changes.
* Compatible additions use reserved `EXT` types, new `INFO` fields, and the
  reserved system IDs, all of which older decoders skip. The `ftrace-short`
  shapes (§3) of `FTRACE` and `FTRACE_CALLER` are the exception: they belong to
  version 1, and a version 1 decoder must implement them to find the next
  record.
* `layout_version` covers the shared-memory control block. A consumer refuses
  an unknown major version.
