# SPDX-License-Identifier: MIT
# Unit tests on the POSIX port (§18.1), built once per configuration (§18.0).
set(XT_UNIT_SRCS test_core.c test_prod.c test_cons.c test_rec.c test_disp.c test_sinks.c
                 test_ftrace.c test_ftrace_insn_armv7m.c test_ftrace_insn_rv32.c
                 test_ftrace_patch_armv7m.c test_ftrace_patch_rv32.c)

xt_variant(xt_posix PORT posix)
xt_test(test_unit xt_posix ${XT_UNIT_SRCS})

# Configuration matrix: every option of §14 that changes code paths.
set(XT_MATRIX
    "cs16:EXTRACE_CFG_WRITE_IN_CS_MAX=16"
    "lostb:EXTRACE_CFG_LOST_BYTES=1"
    "noisr:EXTRACE_CFG_PUBLISH_IN_ISR=0"
    "align1:EXTRACE_CFG_RESV_ALIGN_MAX=1"
    "nospec:EXTRACE_CFG_EMIT_SPECIALIZE=0"
    "local:EXTRACE_CFG_PRINTF_LOCAL=1"
    "small:EXTRACE_CFG_MAX_PAYLOAD=64"
    "caller:EXTRACE_CFG_FTRACE_CALLER=1"
    "ftregs4:EXTRACE_CFG_FTRACE_CALLER=1,EXTRACE_FTRACE_ARG_REGS=4")
foreach(entry IN LISTS XT_MATRIX)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 tag)
    list(GET parts 1 def)
    string(REPLACE "," ";" def "${def}")
    xt_variant(xt_posix_${tag} PORT posix DEFINES ${def})
    if(tag STREQUAL "small" OR tag STREQUAL "ftregs4") # ftregs4: the ARM block on the host
        xt_test(test_unit_${tag} xt_posix_${tag} test_core.c test_prod.c test_cons.c test_rec.c
                test_ftrace.c)
    else()
        xt_test(test_unit_${tag} xt_posix_${tag} ${XT_UNIT_SRCS})
    endif()
endforeach()

# EXTRACE_CFG_ENABLE=0: every macro compiles and does nothing.
xt_variant(xt_posix_off PORT posix DEFINES EXTRACE_CFG_ENABLE=0)
xt_test(test_unit_off xt_posix_off test_disabled.c)
