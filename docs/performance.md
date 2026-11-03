# Producer performance

Instructions retired by the producer for each kind of trace event, on RV32
(`rv32im_zce_zbb`) and on Cortex-M4. The numbers come from
[`tests/perf`](../tests/perf), which measures them on emulators and can be rerun
on any tree.

## Summary

Cost of one event, from the trace point to the return, clang `-O2`, library
defaults:

| Event | Record bytes | RV32 | Cortex-M4 |
|---|---|---|---|
| `EXTRACE_MARK` | 2 | 56 | 64 |
| `EXTRACE` (NONE) | 3 | 74 | 81 |
| `EXTRACE_INTS`, 1 argument | 6 | 108 | 118 |
| FTRACE MARK (address) | 3 | 90 | 81 |
| FTRACE NONE (timestamp, address) | 4 | 109 | 98 |
| FTRACE ARGS(1) | 7 | 180 | 188 |
| FTRACE ARGS(4) | 17 | 284 | 275 |
| Event switched off | 0 | 4 | 5 |
| Traced function, not patched | 0 | 2 | 3 |
| Traced function, patched, FTRACE switch off | 0 | 6 (8 for ARGS) | 9 (11 for ARGS) |

Recording the caller (a `CALLER` flavor, `EXTRACE_CFG_FTRACE_CALLER=1`) costs
20 instructions more per record on RV32, 20–33 on Cortex-M4, and 2 bytes; see
[Caller recording](#caller-recording).

Publishing costs 56 instructions on RV32 (63 on Cortex-M4) and runs once per
`publish_thr` bytes, a quarter of the ring by default: well under one instruction
per event. The whole producer (`extrace_prod.c`) is 20.8 KB of code on RV32 and
24.4 KB on Cortex-M4 at `-O2`, about 20 KB at `-Os`.

## Setup

| | RV32 | Cortex-M4 |
|---|---|---|
| Core | `rv32im_zce_zbb` (Zca, Zcb, Zcmp, Zcmt, Zbb) | ARMv7E-M with FPv4-SP, hard-float ABI |
| Emulator | `qemu-system-riscv32 -M virt -icount shift=0`, M-mode | unicorn, Cortex-M4 model |
| Counter | `minstret` | instructions counted by the emulator, read through MMIO |
| Timestamp | CLINT `mtime`, one load (`lui` + `lw`) | a memory-mapped timer, one load |
| Critical section | `csrrci mstatus, 8` / `csrw mstatus` | `mrs primask; cpsid i` / `msr primask` |
| Compiler | clang 18; GCC 13 for comparison | clang 18 |

- **What a row counts:** every instruction retired from the trace point to the
  return. For the `EXTRACE_*` rows that is the inline enable test, the call, the
  emitter and the jump back. For function entry tracing it is everything added to
  the traced function: the patched entry instruction, the trampoline entry and
  body, and the shape's emitter.
- **Conditions:** the steady state. The ring is not full, no drops are pending,
  and the event does not trigger a publish.
- **Library configuration:** the defaults (`EXTRACE_CFG_STATS=1`,
  `EXTRACE_CFG_WRITE_IN_CS_MAX=0`). User events have IDs from 16, so a 2-byte
  header.
- **ftrace:** `EXTRACE_CFG_FTRACE_BASE` is the image base. The traced function
  takes four arguments: 7, 300, 70000 and 0x80001234, which are varints of 1, 2,
  3 and 5 bytes.

## RV32, clang -O2

| Event | Bytes | Instructions | Loads / stores | Breakdown |
|---|---|---|---|---|
| MARK | 2 | 56 | 17 / 8 | call site 8, `extrace_emit_mark` 48 |
| NONE | 3 | 74 | 21 / 12 | call site 8, `extrace_emit_none` 66 |
| INTS, 1 argument | 6 | 108 | 28 / 22 | call site 10, `extrace_emit_ints32_1` 98 |
| FTRACE, not patched | 0 | 2 | 0 / 0 | two `c.nop` |
| FTRACE MARK | 3 | 90 | 29 / 21 | `jal t0` 1, body 29, `extrace_ftrace_mark` 60 |
| FTRACE NONE | 4 | 109 | 31 / 23 | `jal t0` 1, body 29, `extrace_ftrace_none` 79 |
| FTRACE ARGS(1) | 7 | 180 | 39 / 32 | `jal t0` 1, entry 2, body 31, `extrace_ftrace_args` 146 |
| FTRACE ARGS(4) | 17 | 284 | 48 / 42 | `jal t0` 1, entry 2, body 31, `extrace_ftrace_args` 250 |
| FTRACE, patched, switch off | 0 | 6 | 1 / 0 | `jal t0` 1, body 5 (ARGS: entry 2 more) |
| MARK, event off | 0 | 4 | 1 / 0 | call site 4 |
| publish | 0 | 56 | 15 / 11 | `publish` 54 |
| publish, nothing new | 0 | 39 | 9 / 6 | `publish` 37 |

The loads and stores include every register that `cm.push` and `cm.popret` move.
Each counts as one instruction but takes several cycles, so on most cores the
cycle count is noticeably higher than the instruction count.

## Cortex-M4, clang -O2

| Event | Bytes | Instructions | Loads / stores | Breakdown |
|---|---|---|---|---|
| MARK | 2 | 64 | 20 / 11 | call site 9, `extrace_emit_mark` 55 |
| NONE | 3 | 81 | 21 / 12 | call site 9, `extrace_emit_none` 72 |
| INTS, 1 argument | 6 | 118 | 26 / 19 | call site 12, `extrace_emit_ints32_1` 106 |
| FTRACE, not patched | 0 | 3 | 0 / 0 | three `nop` |
| FTRACE MARK | 3 | 81 | 37 / 27 | `mov ip, lr` + `bl` 2, body 11, `extrace_ftrace_mark` 68 |
| FTRACE NONE | 4 | 98 | 39 / 29 | patch 2, body 11, `extrace_ftrace_none` 85 |
| FTRACE ARGS(1) | 7 | 188 | 48 / 39 | patch 2, entry 3, body 11, `extrace_ftrace_args` 172 |
| FTRACE ARGS(4) | 17 | 275 | 57 / 49 | patch 2, entry 2, body 11, `extrace_ftrace_args` 260 |
| FTRACE, patched, switch off | 0 | 9 | 3 / 0 | patch 2, body 7 (ARGS: entry 2 more) |
| MARK, event off | 0 | 5 | 1 / 0 | call site 5 |
| publish | 0 | 63 | 19 / 15 | `publish` 60 |
| publish, nothing new | 0 | 44 | 13 / 10 | `publish` 41 |

The Cortex-M4 trampoline is shorter in instructions than the RV32 one because
`push` and `pop` save the argument registers in one instruction each. The
trampoline also saves `d0`–`d7` under the hard-float ABI, which is why its loads
and stores are higher. On both, the body calls the shape's emitter directly: the
flavor is the entry the function was patched to call, so no instruction decodes
it.

## Caller recording

A function patched with a `CALLER` flavor (`EXTRACE_CFG_FTRACE_CALLER=1`)
records the caller's return address as well (`FTRACE_CALLER`, 2 more bytes
here). Clang `-O2`:

| Event | Bytes | RV32 | Cortex-M4 |
|---|---|---|---|
| FTRACE MARK, caller | 5 | 110 | 101 |
| FTRACE NONE, caller | 6 | 129 | 126 |
| FTRACE ARGS(1), caller | 9 | 200 | 221 |

The `CALLER` bodies take one instruction more than the others, to pass the
caller in a register; the rest is the emitter encoding the second address
(`extrace_ftrace_mark_c` 79, `_none_c` 98, `_args_c` 165 on RV32; 87, 112 and
204 on Cortex-M4). Building the `CALLER` flavors in costs the other flavors
nothing: their rows are the same in both builds. Their emitters add 1966 bytes
of code on RV32 and 3002 bytes on Cortex-M4.

## Compilers and optimization levels

Instructions per event, library defaults. GCC 13's assembler has no Zcmp, so the
GCC builds use `rv32imc_zicsr_zbb` and save registers one by one. The trampoline
is assembly and the same in every column; only the emitters change.

| Event | RV32 clang -O2 | RV32 clang -Os | RV32 GCC -O2 | RV32 GCC -Os | M4 clang -O2 | M4 clang -Os |
|---|---|---|---|---|---|---|
| MARK | 56 | 57 | 64 | 65 | 64 | 65 |
| NONE | 74 | 75 | 86 | 88 | 81 | 82 |
| INTS, 1 argument | 108 | 110 | 128 | 128 | 118 | 117 |
| FTRACE MARK | 90 | 91 | 99 | 100 | 81 | 82 |
| FTRACE NONE | 109 | 110 | 117 | 118 | 98 | 99 |
| FTRACE ARGS(1) | 180 | 180 | 194 | 201 | 188 | 177 |
| FTRACE ARGS(4) | 284 | 284 | 297 | 326 | 275 | 268 |
| Event switched off | 4 | 4 | 4 | 4 | 5 | 5 |
| publish | 56 | 56 | 52 | 54 | 63 | 63 |
| Producer `.text`, bytes | 20802 | 20022 | 23110 | 22012 | 24414 | 20066 |

## Where the instructions go: MARK on RV32

The trace point is 8 instructions:
- the enable test, `lui` + `lbu` + `andi` + `bnez`;
- the pre-encoded header word, `lui` + `addi`;
- the call (`jal`) and the jump back from the out-of-line block (`j`).

With the event switched off, only the first four run.

The 48 instructions of `extrace_emit_mark`:

| Part | Instructions |
|---|---|
| `cm.push` / `cm.popret` | 2 |
| Producer address, loaded once into `s0` | 2 |
| Critical section enter and exit | 2 |
| Fast-path test: ready, no pending drops, fewer than 255 open reservations | 8 |
| Header length from the header word | 1 |
| Fit test: end of ring and free space at the cached tail | 11 |
| Reservation: head update, records counter | 6 |
| Header bytes, written inside the critical section | 5 |
| Commit and publish-threshold test, urgent flag | 11 |

The common case runs inline in each emitter. Anything unusual goes to one
out-of-line function, the reservation slow path (DESIGN §8.4): not ready, drops
to report as a `LOST` record, a wrap at the ring end, a full ring at the cached
tail, or 255 open reservations.

## What changes the numbers

- **Value sizes.** Every varint byte beyond the first adds a few instructions. An
  `INTS` or FTRACE ARGS record whose values all fit in 7 bits takes a shorter
  path that skips the per-value length computation. A function address is
  usually two bytes or more after `EXTRACE_CFG_FTRACE_BASE`, so ARGS takes the
  general path, as measured above.
- **Header length.** User event IDs from 16 have a 2-byte header; system records
  have 1 byte.
- **`EXTRACE_CFG_STATS=0`** removes the records counter: 3–6 instructions fewer
  per event (MARK 53, NONE 71, INTS 102 on RV32).
- **`EXTRACE_CFG_WRITE_IN_CS_MAX`** writes small `INTS` and `PRINTF` records
  inside the reservation's critical section instead of in a second one. With 16,
  INTS with 1 argument is 99 instructions on RV32, for a longer section with
  interrupts masked. `MARK`, `NONE` and the FTRACE MARK and NONE records are
  always written that way.
- **Wrap, drops, ring full.** These take the slow path once and then return to
  the fast path. A wrap happens once per pass over the ring.
- **Global pointer.** The images are linked without `gp` relaxation. With it, the
  address loads of the trace point can become one `gp`-relative instruction.

## Reproducing

```bash
python3 tests/perf/run_perf.py
```

The runner needs clang, `ld.lld` and the LLVM binutils. For RV32 it also needs
`qemu-system-riscv32`, version 8.1 or later for Zcmp, and for Cortex-M4 the
Python `unicorn` module.

```bash
python3 tests/perf/run_perf.py rv32 --cc gcc --opt Os -D EXTRACE_CFG_STATS=0
```

That builds one target with another compiler, optimization level or library
configuration; GCC builds need `riscv64-unknown-elf-gcc`. The caller recording
rows appear with `-D EXTRACE_CFG_FTRACE_CALLER=1`.

```bash
python3 tests/perf/run_perf.py rv32 --trace MARK
```

That also prints the executed instructions of one row. Each count in the tables
is the image's own counter, checked against an instruction trace, which also gives
the breakdown and the loads and stores.

A producer change that is meant only as an optimization can be checked against
the previous revision:

```bash
python3 tests/perf/run_diff.py --base HEAD
```

It runs a random workload on the POSIX port with the library (producer, consumer
and decoder) of the base and of the working tree, in eight library
configurations, one of them with the `CALLER` flavors. It fails on any difference in ring bytes, producer state,
statistics, reservation positions or timer reads.
