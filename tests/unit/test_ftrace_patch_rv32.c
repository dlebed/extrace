// SPDX-License-Identifier: MIT
/* test_ftrace_patch_rv32.c - the patcher with the RV32 encoder (test_ftrace_patch.h). */
#include "extrace_test.h"
#include "../../port/arch/rv32/extrace_ftrace_arch.h"

#define XT_ARCH rv32
#include "test_ftrace_patch.h"
