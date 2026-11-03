// SPDX-License-Identifier: MIT
// expect: pointer type does not match the type declared for MOTOR
/* MOTOR is declared with struct motor_state: any other pointer is rejected. */
#include "extrace.h"

struct pump_state {
    int rpm;
};

void f(const struct pump_state *p)
{
    EXTRACE_STRUCT(MOTOR, p);
}
