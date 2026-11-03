/* SPDX-License-Identifier: MIT */
/*
 * extrace_test_hooks.h - unit-test hooks (EXTRACE_CFG_TEST_HOOKS, DESIGN §14).
 * Never enabled in firmware.
 */
#ifndef EXTRACE_TEST_HOOKS_H
#define EXTRACE_TEST_HOOKS_H

#include <stdint.h>

/*
 * Index at which every new epoch starts, on both sides. Lets tests cross the
 * 2^32 index wrap without moving 4 GiB of data (§6.6, §18.1).
 */
extern uint32_t extrace_test_index_base;

/* Called inside the reservation critical section with the index of the record's
 * first header byte (after any wrap gap, LOST prefix and alignment). */
extern void (*extrace_test_resv_hook)(uint32_t epoch, uint32_t rec_index);

/* Called with every consumer tail the producer reads for its epoch (R1, poll). */
extern void (*extrace_test_tail_hook)(uint32_t epoch, uint32_t tail);

#endif /* EXTRACE_TEST_HOOKS_H */
