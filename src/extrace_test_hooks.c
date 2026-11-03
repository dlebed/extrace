// SPDX-License-Identifier: MIT
/* extrace_test_hooks.c - storage of the unit-test hooks. */
#include "extrace_test_hooks.h"

uint32_t extrace_test_index_base;
void (*extrace_test_resv_hook)(uint32_t epoch, uint32_t rec_index);
void (*extrace_test_tail_hook)(uint32_t epoch, uint32_t tail);
