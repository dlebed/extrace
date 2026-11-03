// SPDX-License-Identifier: MIT
// expect: too many arguments
/* At most EXTRACE_CFG_MAX_ARGS (here 4) values per record. */
#define EXTRACE_CFG_MAX_ARGS 4
#include "extrace.h"

void f(void)
{
    EXTRACE_INTS(ADC_BLOCK, 1, 2, 3, 4, 5);
}
