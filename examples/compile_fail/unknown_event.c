// SPDX-License-Identifier: MIT
// expect: EXTRACE_CT_RX_PKTS
/* A misspelled event name is a compile error, not a silent no-op. */
#include "extrace.h"

void f(void)
{
    EXTRACE_INTS(RX_PKTS, 60u, 1u);
}
