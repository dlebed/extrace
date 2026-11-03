# Function entry tracing

Function entry tracing records each call to an instrumented function, much like Linux ftrace.

The compiler leaves a few NOPs at the entry of each function. The host picks the
functions to trace from the ELF, and a **flavor** for each, and sends their entry
PCs to the device. There, `extrace_ftrace_patch()` checks each pad and patches it
into a call of the trampoline entry of that flavor. The trampoline hands the
return address of that call to the producer's emitter for the flavor's record
shape, which writes an `FTRACE` record. The host maps the address back to the
function using the ELF and its debug data.

The flavor is per function, so one image can record the functions under
investigation with their arguments and the caller, and the rest with a
timestamp only, or without:

| Flavor | Records |
|---|---|
| `EXTRACE_FTRACE_MARK` | The address: that the function was called |
| `EXTRACE_FTRACE_NONE` | The timestamp and the address |
| `EXTRACE_FTRACE_ARGS(n)` | Also the first n argument registers, n = 1 to 4 on ARM, 1 to 8 on RV32 |
| any of these `\| EXTRACE_FTRACE_CALLER` | Also the caller (`EXTRACE_CFG_FTRACE_CALLER=1`) |

This guide covers:

* what lives where;
* the flavors and their records;
* how to build and control function tracing;
* the trampoline contract per architecture;
* patching, and the site table.

[producer.md](producer.md) and [wire-format.md](wire-format.md) describe the rest
of the producer and the byte format.

## Split of responsibilities

| Part | Where | What |
|---|---|---|
| NOP pad | compiler | `-fpatchable-function-entry=N` on the traced code |
| Site list | host | Entry PCs of the instrumented functions, from the ELF; the device keeps no table |
| Command channel | **platform** | Brings the PCs and flavors to the device and calls `extrace_ftrace_patch(pc, flavor)` |
| Patching | generic, `extrace_ftrace.c` | Checks the pad at `pc`, then writes the patch of the flavor's entry, or the NOPs, in a safe order |
| Code store | port, `extrace_port_code_write16()` | Optional: stores a halfword and makes it visible to instruction fetch. The default is a store followed by `DSB`/`ISB` or `fence(.i)` |
| Encoder | port, `port/arch/<arch>/extrace_ftrace_arch.h` | `extrace_ftrace_insn()` returns the patch halfwords, plus constants |
| Trampoline | port, `port/arch/<arch>/extrace_ftrace_<arch>.S` | One entry per flavor, listed in `extrace_ftrace_entries`; one body per record shape tests the switch, saves the argument registers, calls the shape's emitter and returns to the function |
| Emitters | generic, `extrace_prod.c` | `extrace_ftrace_mark()`, `_none()`, `_args()` and the `_c` ones with the caller: write the record. `extrace_ftrace_hit()` reaches them from C |
| Decoding | generic, `extrace_rec.c` | `extrace_rec_ftrace()`, the text sink, `INFO.ftrace_base` |
| Symbols | host | address → function, from the ELF |

Two ports ship with extrace:

* `armv7m`: Cortex-M3/M4/M7, Thumb-2, soft-float or hard-float ABI.
* `rv32`: RV32I with C/Zca, so `rv32im_Zce_Zbb_Zicsr` is covered. It has no F/D and is not RV32E.

## Records

All function records use system event 4, `FTRACE`. The record carries no event ID
per function: the address identifies the function.

Each traced function has its own flavor, set when it is patched; the flavor
chooses the record shape:

| Flavor | Kind | Bytes | Records |
|---|---|---|---|
| `EXTRACE_FTRACE_MARK` | `MARK` | `21`, addr | That the function was called |
| `EXTRACE_FTRACE_NONE` | `NONE` | `20`, tsd, addr | The call and when it happened |
| `EXTRACE_FTRACE_ARGS(n)` | `INTS` | `22`, tsd, len, `00`, addr, a0 … a(n-1) | The call, when, and the first n argument registers |

The address field `addr` is the return address the trampoline received, minus
`EXTRACE_CFG_FTRACE_BASE`. It is a uvarint. Argument registers are recorded as
unsigned 32-bit values.

### Caller

A flavor with `EXTRACE_FTRACE_CALLER` or'ed in also records the caller: the
return address of the call into the traced function, minus
`EXTRACE_CFG_FTRACE_BASE`. The trampoline passes it from where it already is,
`ra` on RV32 and `ip` on ARM. The records then use system event 8,
`FTRACE_CALLER`, in the same three shapes, with the caller as a uvarint right
after the address:

| Flavor | Kind | Bytes | Records |
|---|---|---|---|
| `EXTRACE_FTRACE_MARK \| EXTRACE_FTRACE_CALLER` | `MARK` | `41`, addr, caller | That the function was called, and from where |
| `EXTRACE_FTRACE_NONE \| EXTRACE_FTRACE_CALLER` | `NONE` | `40`, tsd, addr, caller | Also when |
| `EXTRACE_FTRACE_ARGS(n) \| EXTRACE_FTRACE_CALLER` | `INTS` | `42`, tsd, len, `00`, addr, caller, a0 … a(n-1) | Also the first n argument registers |

```
caller's return address 0x20000C13 (caller 0xC13 = 93 18)

41  C7 34  93 18                                  MARK    5 bytes
40  1D  C7 34  93 18                              NONE    6 bytes
42  19  06  00  C7 34  93 18  07                  ARGS 1  9 bytes: 7
hdr tsd len sm  addr   caller a0
```

The caller's return address points just past the call instruction in the
caller; on ARM its Thumb bit is set. The host maps `ftrace_base + caller - 1` to
the calling function and line. Two cases need care:

* A function entered by a tail call reports the caller of the function that
  made the tail call.
* A Cortex-M exception handler reports its `EXC_RETURN` value, 0xFFFFFFxx,
  since that is what `lr` holds at its entry.

### Shapes

The MARK and NONE shapes reuse the kinds `MARK` (no timestamp) and `NONE`
(timestamp), followed by one uvarint (two with the caller) and no length field.
These are the only records whose size depends on their ID. Every decoder in
extrace handles them (wire format §3).

With the base set to the start of the code region, a 64 KiB to 2 MiB image
needs 2 to 3 bytes for `addr`:

```
EXTRACE_CFG_FTRACE_BASE = 0x20000000, return address 0x20001A47 (addr 0x1A47 = C7 34)

21  C7 34                                         MARK    3 bytes
20  1D  C7 34                                     NONE    4 bytes (tsd 29)
22  19  0B  00  C7 34  07  AC 02  80 80 81 80 02  ARGS 3  14 bytes: 7, 300, 0x20004000
hdr tsd len sm  addr   a0  a1     a2
```

With base 0, the same address takes 5 bytes (`C7 B4 80 80 02`).

The host recovers the function entry E from the address:

* It reads the base from the 13th `INFO` field, `ftrace_base`. The field is 0 if
  unset, and older decoders skip it.
* It computes `ret = ftrace_base + addr` and `E = ret - RET_OFFSET`.

| Port | `ret` | `RET_OFFSET` |
|---|---|---|
| `armv7m` | E + 6, with the Thumb bit set | 7 |
| `rv32` | E + 4 | 4 |

Code that runs with no trampoline, or wants a record of its own at one call
site, calls `extrace_ftrace_hit(ret, caller, args, flavor)` directly. It tests
the switch and the flavor itself: a flavor this build does not record writes
nothing.

### Why the address and not an event ID

The trampoline only learns the return address. Mapping it to an ID would cost a
table search on every call: 4–8k functions means 12–13 steps of a binary search.
Alternatively, each site would need per-site state written by the patcher.

The address costs nothing on the device and needs no tables. It works for any
number of functions and any code layout. The host already has the symbols.

## Control

* **Switch.** Tracing is on when the mask bit of system event 4 is set.
  * A consumer flips it like any other event:
    `extrace_cons_ev_set(c, EXTRACE_SYS_FTRACE, EXTRACE_EVS_ON)` or
    `extrace_ctl_ev_set(d, ring, EXTRACE_SYS_FTRACE, ...)`.
  * The default comes from `EXTRACE_CFG_FTRACE_ON`.
  * When the switch is off, a patched function costs only the trampoline's
    test (see Costs), whatever its flavor.
* **Flavor.** Set per function by `extrace_ftrace_patch(pc, flavor)`, and
  changed the same way, in place, while the function may run (see Patching).
  The values are small integers the host can send as they are:
  `EXTRACE_FTRACE_OFF` 0, `MARK` 1, `NONE` 2, `ARGS(n)` 2 + n, and
  `EXTRACE_FTRACE_CALLER` 0x10 or'ed in.
* **Caller.** The `CALLER` flavors exist with `EXTRACE_CFG_FTRACE_CALLER=1`;
  without it, the patcher refuses them. The mask bit of `FTRACE_CALLER` has no
  effect: the FTRACE switch covers both events.

| Option | Default | Effect |
|---|---|---|
| `EXTRACE_CFG_FTRACE` | 0 | Builds the emitters. `extrace_add_image(... FTRACE_ARCH <arch>)` sets it |
| `EXTRACE_CFG_FTRACE_ON` | 0 | Default state of the switch |
| `EXTRACE_CFG_FTRACE_CALLER` | 0 | Builds the `CALLER` flavors (`FTRACE_CALLER` records) |
| `EXTRACE_CFG_FTRACE_BASE` | 0 | Subtracted from every address. Set it to the start of the traced code |

## Integration

1. **Build the traced code** with the NOP pad of the port:

   | Port | Flag | NOPs |
   |---|---|---|
   | `armv7m` | `-fpatchable-function-entry=3` | 3 × `nop` (16-bit), 6 bytes |
   | `rv32` | `-fpatchable-function-entry=2` | 2 × `c.nop`, 4 bytes |

   GCC supports both. Clang supports RISC-V but not 32-bit ARM (checked with
   clang 18).

   Exclude a single function with
   `__attribute__((patchable_function_entry(0, 0)))`, or leave its file out of
   the flag. Functions that **must** be excluded:

   * everything the emitters can reach: extrace itself, the port hooks and
     anything they call;
   * RISC-V `__attribute__((interrupt))` handlers, where `t0` and `ra` are live;
   * naked functions;
   * functions that receive a GNU nested-function static chain, which uses
     `ip` on ARM and `t2` on RISC-V.

2. **Build extrace:**

   ```cmake
   enable_language(ASM)
   extrace_add_image(fw_a ... COMPONENTS prod FTRACE_ARCH armv7m)   # or rv32
   ```

   `FTRACE_ARCH` does three things:

   * adds the trampoline and the patcher, `src/extrace_ftrace.c`;
   * puts the arch header on the include path;
   * defines `EXTRACE_CFG_FTRACE=1`.

   The library is compiled with `-fpatchable-function-entry=0` whenever the
   compiler accepts the flag, so a global flag never instruments it.

3. **Place the trampoline** (section `.text.extrace_ftrace_entry`) within branch
   range of all traced code:

   * ARM `BL`: ±16 MiB. Its bodies call the emitters with `BL` too, so
     `extrace_prod.c` must be within ±16 MiB of it as well.
   * RISC-V `JAL`: ±1 MiB. Its bodies reach the emitters with `call`, from any
     distance (one `jal` once the linker relaxes it).

   The encoder returns false for a site that is out of range. TCM is a good
   place for the trampoline, provided it is in range. For example, ITCM at
   0x00000000 is out of range of flash at 0x08000000. The entry table,
   `.rodata.extrace_ftrace_entries`, is read only by the patcher and can go
   anywhere.

4. **Keep the site table off the device:** `INCLUDE extrace_ftrace.ld` from
   the image's linker script, from [linker/](../linker/extrace_ftrace.ld). See
   below.

5. **Patch** the functions to trace with `extrace_ftrace_patch()`, as described
   below. Then turn the switch on.

## Trampoline contract

The trampoline has one entry per flavor; `extrace_ftrace_entries[i]`, with
`i = extrace_ftrace_entry_index(flavor)`, is its address. Behind the entries
there is one **body** per record shape: MARK, NONE and ARGS, each with and
without the caller. A body tests the switch, saves the registers, calls the
shape's emitter directly and returns to the function. So the flavor costs
nothing at run time: no flavor code, no dispatch, and no second switch test.

* The MARK and NONE entries are their bodies.
* An ARGS(n) entry loads n and joins the ARGS body: two instructions on RV32
  (`c.li t1, n` ; `c.j`), three on ARM (the save, `movs`, `b`). The entry with
  the most arguments falls straight into the body. Only the offset of the entry
  tells how many arguments are recorded; the body is shared.

| | `armv7m` | `rv32` |
|---|---|---|
| Patch at E | `mov ip, lr` ; `bl <entry>` | `jal t0, <entry>` |
| Return address of the patch | `lr` = E+7 | `t0` = E+4 |
| Caller's return address | `ip` | `ra` (untouched) |
| Scratch at entry | `ip` (AAPCS: may be clobbered by veneers) | `t0`–`t6` (psABI temporaries); `t2` is not touched |
| Saved around the emitter | r0–r3, ip, lr; d0–d7 with `__ARM_PCS_VFP` | a0–a7, ra, t0 |
| Enabled path, MARK / NONE | 11 instructions | 29 instructions |
| Enabled path, ARGS(n) | 13 (n = 4) or 14 | 32 (n = 8) or 33 |
| Disabled path | 7 instructions (9 for ARGS), one stack frame | 5 instructions (7 for ARGS), no stack |
| Exit | `pop {r0-r3, lr}` ; `pop {pc}` | `jr t0` |
| Size | 264 bytes, and the 48-byte entry table | about 480 bytes linked, and the 80-byte entry table |

The `CALLER` bodies add one instruction, which passes the caller's return address
in a register. ARM loads the producer's address from a literal pool next to
the bodies: one instruction instead of `movw` and `movt`.

The emitters, in `extrace_prod.c`:

```c
void extrace_ftrace_mark(uintptr_t ret);
void extrace_ftrace_none(uintptr_t ret);
void extrace_ftrace_args(uintptr_t ret, const uint32_t *args, unsigned int n);
void extrace_ftrace_mark_c(uintptr_t ret, uintptr_t caller);
void extrace_ftrace_none_c(uintptr_t ret, uintptr_t caller);
void extrace_ftrace_args_c(uintptr_t ret, uintptr_t caller, const uint32_t *args, unsigned int n);
```

`ret` and `caller` are as delivered; the emitters subtract
`EXTRACE_CFG_FTRACE_BASE`. `args` points to the saved a0–a7 or r0–r3, and n is
at most `EXTRACE_FTRACE_ARG_REGS`. They do not test the switch: the body did.
Without `EXTRACE_CFG_FTRACE_CALLER` the `_c` ones are empty, for the trampoline
to link; the patcher refuses their flavors. C code uses
`extrace_ftrace_hit()`, which checks the switch and the flavor first.

Every body tests bit 4 of the first word of `extrace_producer` before saving
anything. `extrace_prod.c` asserts that this layout holds. Each `.S` checks at
assembly time that its entry table matches `extrace_ftrace.h`.

**RISC-V without the CSR.** JAL can link into `t0`, the psABI's alternate link
register. `ra` therefore survives without being swapped into a spare CSR, and the
patch is a single instruction, or 2 NOPs.

A CSR swap would need `csrw` and `jal ra`: 8 bytes, or 4 NOPs, plus a CSR access
on every call. If the trampoline must be more than 1 MiB away, the 4-NOP
`auipc t0, hi` ; `jalr t0, lo(t0)` reaches any address without a CSR either. It
is not shipped, but the trampoline is the same.

## Patching

### Site table: host only by default

GCC and clang list every patch pad in the section `__patchable_function_entries`,
one pointer per function. Neither compiler has an option to omit it, and by
default the linker loads it like data: 4 bytes per function.

[linker/extrace_ftrace.ld](../linker/extrace_ftrace.ld) places it in a
non-allocated `(INFO)` output section, like the extrace metadata. The ELF keeps
it with final addresses, and the host reads it from there. The device image
does not contain it.

A firmware that wants to walk the sites itself leaves the fragment out. It then
iterates `__start___patchable_function_entries` to
`__stop___patchable_function_entries`, clearing bit 0 of each entry.

The host can also skip the table and offer every function symbol. A function
without a pad is refused on the device (see below).

### `extrace_ftrace_patch(pc, flavor)`

The host sends the entry PC of a function, which is its symbol value, and the
flavor; the Thumb bit of the PC is ignored. The device calls
`extrace_ftrace_patch(pc, flavor)` to trace the function or to change its
flavor, or `extrace_ftrace_patch(pc, EXTRACE_FTRACE_OFF)` to restore the NOPs.
A flavor that is not one of this build is refused with `-EXTRACE_EINVAL`:
an `ARGS(n)` beyond the argument registers, or a `CALLER` flavor without
`EXTRACE_CFG_FTRACE_CALLER`.

Before writing anything, the call reads the pad and accepts only:

* the NOPs (3 × `BF00` on ARM, 2 × `0001` on RISC-V);
* the complete patch for any entry of this trampoline;
* a patch left halfway: the skip branch first, then halfwords that are each a
  NOP or the same halfword of some entry's patch. A change of flavor left
  halfway mixes the old patch and the new one.

Anything else returns `-EXTRACE_EINVAL` without a write, for example a wrong
address or a function built without the pad. A pad out of branch range of the
trampoline returns `-EXTRACE_ERANGE`. Patching a pad that is already in the
requested state returns 0 without writing.

The write order keeps every intermediate state a valid program, because
halfword stores are single-copy atomic. An interrupt may run the function
while it is being patched:

1. The first halfword becomes `EXTRACE_FTRACE_HW_SKIP`, a 16-bit branch over
   the pad (`b.n E+6` or `c.j E+4`).
2. The remaining halfwords get the new patch, or the NOPs.
3. The first halfword gets its final value.

Each store goes through `extrace_port_code_write16()`. Its default is a plain
store followed by an instruction-fetch barrier:

* ARMv7-M: `DSB`; `ISB`.
* RISC-V: `fence.i` when Zifencei is enabled. Otherwise (`rv32im_Zce_Zbb_Zicsr`)
  only `fence`, which is enough for a core without an instruction cache or
  prefetch buffer.

A port overrides it with `EXTRACE_PORT_HAS_CODE_WRITE` when it needs more:

* Cortex-M7 code in cacheable memory: clean the D-cache line and invalidate
  the I-cache line.
* A core with an I-cache but without Zifencei: its own mechanism.
* An MPU region that must be unlocked.
* Other cores running the same code.

Some rules:

* Calls for the same `pc` must not overlap. One command handler is enough.
* The platform should check that `pc` lies in the traced code region before
  calling: the check reads the pad, so a wild address can fault.
* The traced code must be writable: RAM or TCM, or flash behind a remap.
* With the switch off, a patched function only costs the trampoline's test.
  Restoring the NOPs removes even that.
* A change of flavor goes through the same three steps. Calls that run
  meanwhile record nothing, or the old flavor, or the new one; never a mix.

## Costs

* **Unpatched.** The NOPs are the only code added: 3 × `nop` on ARM (6 bytes) or
  2 × `c.nop` on RISC-V (4 bytes) per function, executed on every call.
* **Site table.** None on the device with `extrace_ftrace.ld`. The table stays
  in the ELF for the host. Without the fragment, it costs 4 bytes per function.
* **Patcher.** `extrace_ftrace_patch()`, once per change: about 390 bytes on
  RV32, 540 on Cortex-M4. It tries the patch of every entry to recognize the
  pad.
* **Trampoline.** One body per record shape: about 480 bytes on RV32 and 264
  on Cortex-M4, and an entry table of 80 or 48 bytes.
* **Patched, switch off.** The disabled path in the contract table: no call,
  and no stack use on RISC-V.
* **Patched, switch on.** The body calls the shape's emitter directly: 29–33
  instructions on RV32 and 11–14 on Cortex-M4 around it, most of them saving
  and restoring registers ([performance.md](performance.md)).
  * MARK and NONE cost about as much as `EXTRACE_MARK` and `EXTRACE`: one
    critical section, with the record written inside it.
  * ARGS(n) is an `INTS` record with `1 + n` values.
* **Caller.** The caller's uvarint, usually 2–3 bytes, and 20 instructions per
  record on RV32, 20–33 on Cortex-M4, almost all of them encoding it. The
  `FTRACE_CALLER` emitters are about 1.9 KB of code on RV32 and 2.9 KB on
  Cortex-M4, built only with `EXTRACE_CFG_FTRACE_CALLER=1`. Building them costs
  the other flavors nothing.
* **Record size.** 3, 4–5 or 6+ bytes, as shown above, plus the caller.

## Testing

* `tests/unit/test_ftrace.c` covers the emitters on the host, through
  `extrace_ftrace_hit()`:
  * the switch;
  * the exact bytes of every shape, with and without the caller;
  * flavors mixed call by call;
  * flavors this build does not record;
  * decoding, including truncated records;
  * drop accounting.

  The `ftregs4` configuration builds it with four argument registers, as on ARM.
* `test_ftrace_patch_<arch>.c` runs the patcher on a pad in memory with each
  encoder: every flavor, changes of flavor in place, halfway states of a change,
  refusals and the write order.
* `test_ftrace_insn_<arch>.c` checks the encoders against vectors from the LLVM
  assembler.
* `tests/ftrace/run_emu.py` runs both real trampolines and the patcher under
  the Unicorn emulator, built with `EXTRACE_CFG_FTRACE_CALLER` 0 and 1. Mock
  emitters log what they get and clobber every scratch register. The test
  patches every entry of the table in turn, in place, and checks:
  * that each entry reaches the emitter of its flavor, with its argument count;
  * arguments, the return address and the caller's return address;
  * that registers are restored, including s0–s15 on hard-float ARM;
  * that the patcher refuses a function without a pad and an out-of-range
    site;
  * that it completes a half-written patch;
  * that it restores the exact NOPs;
  * without `EXTRACE_CFG_FTRACE_CALLER`, that the patcher refuses every
    `CALLER` flavor and leaves the pad as it was.

  It links with `extrace_ftrace.ld` and checks that the site table is in the
  ELF, holds `traced()`, and is not allocated.

  CTest runs it as `ftrace_emu` when clang, ld.lld and Python with `unicorn`
  are available.
* `run_emu.py --emulator qemu` runs the same images on
  `qemu-system-riscv32 -M virt` and `qemu-system-arm -M mps2-an386`
  (Cortex-M4F, hard float), and an end-to-end image, `tests/ftrace/rec.c`, with
  the real producer: every entry of the table is patched in turn into a
  function with eight arguments, and the record it leaves in the ring must have
  the exact bytes of its flavor (any `tsd`), the function its usual result. A
  second function keeps its own flavor meanwhile, and nothing is recorded
  unpatched, with the switch off, or for a refused `CALLER` flavor. Both
  `EXTRACE_CFG_FTRACE_CALLER` builds; CTest runs it as `ftrace_qemu` when both
  QEMU system emulators are installed.
