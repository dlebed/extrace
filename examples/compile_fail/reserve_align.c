// SPDX-License-Identifier: MIT
// expect: align must be a power of two
#include "extrace.h"

void f(void)
{
    struct extrace_resv r;

    if (EXTRACE_RESERVE(DMA_DESC, 12, 3, &r))
        extrace_commit(&r);
}
