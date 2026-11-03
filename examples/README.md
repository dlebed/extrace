# Producer API examples

Small, runnable programs, one per producer feature. They run on the host: the
POSIX port stands in for the SoC, and a viewer in the same process reads the
ring as a consumer core would and prints every record decoded and formatted.
The example code itself uses only the producer API, exactly as firmware does.

```bash
cmake -S examples -B build-ex && cmake --build build-ex
./build-ex/03_printf
ctest --test-dir build-ex      # every output against expected/, and the compile_fail/ cases
```

They are also part of the main build (`EXTRACE_BUILD_EXAMPLES`, on for a
top-level build), so `ctest --preset test` runs them.

| Example | Shows |
|---|---|
| [01_quickstart](01_quickstart.c) | `extrace_prod_init`, `EXTRACE_MARK`, `EXTRACE`, `EXTRACE_INTS`, `EXTRACE_PRINTF`, `EXTRACE_STR`, `extrace_prod_poll` |
| [02_ints](02_ints.c) | `EXTRACE_INTS` with 1 to 16 arguments of any integer type: sign from the C type, 64-bit detection, register emitters for 1 to 3 arguments, single evaluation, `_URGENT` |
| [03_printf](03_printf.c) | Deferred `EXTRACE_PRINTF` with 0 to 16 arguments: integers, 64-bit, `float`/`double`, copied strings, `EXTRACE_CSTR`, pointers, host-side width and flags; `EXTRACE_TEXT` |
| [04_payloads](04_payloads.c) | `EXTRACE_STR`, `EXTRACE_BUF` (with truncation), `EXTRACE_BUF_LARGE` fragments, type-checked `EXTRACE_STRUCT` / `_N` / `_URGENT`, `ANY` events |
| [05_reserve](05_reserve.c) | Zero-copy `EXTRACE_RESERVE` with alignment, `EXTRACE_RESERVE_STRUCT`, `extrace_commit_urgent`, `extrace_cancel`, records behind an open reservation |
| [06_control](06_control.c) | Compiled-out groups, `EXTRACE_ON` guards, catalog defaults, consumer switching events at run time (`CFG`), runtime-ID emitters |
| [07_contexts](07_contexts.c) | Tracing from an ISR, publication thresholds and the doorbell, overflow and `LOST`, re-init to a new epoch, `extrace_prod_panic` |
| [08_ftrace](08_ftrace.c) | Function entry tracing with per-function flavors (`MARK`, `NONE`, `ARGS(n)`, `\| CALLER`) and the `FTRACE` switch |

## The variadic macros

The record macros take their arguments the way `printf` does, and resolve
everything about them at compile time with `_Generic`:

```c
EXTRACE_INTS(RX_PKT, len, port);                    /* 1-3 integers: passed in registers */
EXTRACE_INTS(ADC_BLOCK, s[0], s[1], /* ... */ s[15]); /* up to 16, through one array */
EXTRACE_INTS(UPTIME, UINT64_C(1) << 40);            /* a 64-bit argument picks the 64-bit emitter */
EXTRACE_INTS(TX_DONE, 1500u, -110);                 /* -110 is signed: zigzag, 2 bytes */

EXTRACE_PRINTF(LOG, "scheduler started");           /* no arguments */
EXTRACE_PRINTF(LOG, "v=%.2f ssid=%s p=%p", volts, ssid, buf); /* float, copied string, pointer */
EXTRACE_PRINTF(LOG, "state %s", EXTRACE_CSTR(state_names[st])); /* constant string: address only */
```

The output of `02_ints` and `03_printf` (the clock is simulated, so it is
reproducible):

```
-- 1 to 3 arguments: register emitters, no array on the stack
       20 us  UPTIME        INTS    us=123456
       25 us  RX_PKT        INTS    len=60 port=7
       30 us  SENSOR        INTS    ch=3 raw=0xbeef mC=-1250
     (3 records, 26 bytes in the ring)

-- every argument class in one call
       55 us  LOG           PRINTF  "ofs=-1234567890123 ns tx=5000000000 B v=3.30 r=0.6180 ssid=lab-net p=0x20001000"  @03_printf.c:44
     (1 record, 41 bytes in the ring)
```

The field names (`len`, `port`) and display hints (`raw:x`) come from the
`spec` column of the catalog, and the formatted string from the call-site
descriptor: the record itself holds only the event ID, a time delta and the
values.

## Mistakes the compiler catches

Each file in [compile_fail/](compile_fail) must be rejected with the diagnostic
named on its second line; CTest checks that it is.

| File | Mistake |
|---|---|
| `unknown_event.c` | A misspelled event name |
| `kind_mismatch.c` | `EXTRACE_INTS` on a `PRINTF` event |
| `ints_float.c` | A `float` in `EXTRACE_INTS` |
| `too_many_args.c` | More values than `EXTRACE_CFG_MAX_ARGS` |
| `struct_type.c` | `EXTRACE_STRUCT` with a pointer to another type |
| `reserve_kind.c` | `EXTRACE_RESERVE` on an `INTS` event |
| `reserve_align.c` | An alignment that is not a power of two |

## Files

| Path | Content |
|---|---|
| [config/](config) | The image's catalog (`extrace_events.def`), its STRUCT types and configuration |
| [common/ex_host.c](common/ex_host.c) | Ring block, producer setup, and the viewer: a dispatcher with a record sink that formats records from the image metadata |
| `expected/` | The output of each example, compared by CTest |

What differs from firmware:

* The examples keep the metadata in memory (`EXTRACE_META_RODATA`) so that the
  viewer can read it in the same process. Firmware keeps the default
  `EXTRACE_META_INFO`, which leaves it in the ELF only, for the host.
* The host has no ftrace trampoline. `08_ftrace` calls `extrace_ftrace_hit()`,
  the C entry to the same emitters, from a macro at the top of each function.
  On the target the functions are unchanged and are patched at run time with
  `extrace_ftrace_patch()`; see [docs/ftrace.md](../docs/ftrace.md).
