// SPDX-License-Identifier: MIT
// expect: event LOG is declared with another kind
/* LOG is a PRINTF event: EXTRACE_INTS on it does not compile. */
#include "extrace.h"

void f(void)
{
    EXTRACE_INTS(LOG, 1u);
}
