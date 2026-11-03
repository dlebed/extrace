// SPDX-License-Identifier: MIT
// expect: EXTRACE_RESERVE needs a BUF, STRUCT, STR or ANY event
/* A reservation holds bytes: an INTS event cannot be reserved. */
#include "extrace.h"

void f(void)
{
    struct extrace_resv r;

    if (EXTRACE_RESERVE(RX_PKT, 8, 1, &r))
        extrace_commit(&r);
}
