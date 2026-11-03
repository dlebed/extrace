/* SPDX-License-Identifier: MIT */
/*
 * ex_host.h - the scaffolding the examples share: one producer ring in "shared
 * memory" and, in the same process, a consumer with a dispatcher and a record
 * sink that prints each record decoded and formatted, as a host tool would.
 *
 * On a real SoC the producer runs on one core and the viewer on another (or on
 * the host behind a debugger); only the ring block is shared. The examples
 * themselves use the producer API only.
 */
#ifndef EX_HOST_H
#define EX_HOST_H

#include <stddef.h>
#include <stdint.h>
#include "extrace.h"
#include "extrace_disp.h"

#define EX_RING_DATA 4096u                       /* power of two */
#define EX_RING_ID 1u

extern uint64_t ex_ring_block[];                 /* the shared ring block */
extern const uint32_t ex_ring_block_size;
extern const struct extrace_prod_cfg ex_prod_cfg;
extern struct extrace_cons ex_cons;              /* the consumer side of the ring */
extern struct extrace_disp ex_disp;

/* Starts the viewer and the producer, with a deterministic clock. */
void ex_start(const char *title);
/* Prints a section heading. */
void ex_section(const char *title);
/* Publishes what the producer committed and prints every record the viewer reads. */
void ex_show(void);
/* Runs the viewer only: prints what the producer has published so far. */
void ex_drain(void);
/* Quiet: print only LOST and PANIC records, and the counts. */
void ex_quiet_records(bool on);
/* Producer statistics, then the end of the example. Returns the exit status. */
int ex_finish(void);

/* Names for function addresses in FTRACE records, as a host reads them from the ELF. */
void ex_symbol(const void *fn, const char *name);

/* Pretty-printer for the payload of a STRUCT event, as a host would with the C type. */
typedef void (*ex_struct_fmt)(char *buf, size_t cap, const void *payload, uint32_t len);
void ex_struct_printer(uint16_t event, ex_struct_fmt fn);

/* Calls fn as an interrupt handler (the POSIX port simulates ISRs with signals). */
void ex_raise_irq(void (*fn)(void));

#endif /* EX_HOST_H */
