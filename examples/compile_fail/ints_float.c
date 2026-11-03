// SPDX-License-Identifier: MIT
// expect: EXTRACE_INTS takes integers
/* INTS records integers only; a float belongs in EXTRACE_PRINTF. */
#include "extrace.h"

void f(float volts)
{
    EXTRACE_INTS(SENSOR, 1u, volts);
}
