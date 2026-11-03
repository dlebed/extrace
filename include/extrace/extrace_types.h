/* SPDX-License-Identifier: MIT */
/*
 * extrace_types.h - wire kinds, system event IDs, argument classes and error codes
 * (DESIGN §4.3, §4.6, §10.3, §17, §22.D).
 */
#ifndef EXTRACE_TYPES_H
#define EXTRACE_TYPES_H

#include <stdint.h>

/* Record kinds (3 bits of the header). */
enum extrace_kind {
    EXTRACE_K_NONE = 0,
    EXTRACE_K_MARK = 1,
    EXTRACE_K_INTS = 2,
    EXTRACE_K_PRINTF = 3,
    EXTRACE_K_BUF = 4,
    EXTRACE_K_STRUCT = 5,
    EXTRACE_K_STR = 6,
    EXTRACE_K_EXT = 7,
    EXTRACE_K_ANY = 8, /* catalog only: kind not fixed */
};

/* System event IDs 0..15 (§4.6). */
enum extrace_sys_ev {
    EXTRACE_SYS_PAD = 0,
    EXTRACE_SYS_SYNC = 1,
    EXTRACE_SYS_LOST = 2,
    EXTRACE_SYS_INFO = 3,
    EXTRACE_SYS_FTRACE = 4,
    EXTRACE_SYS_CFG = 5,
    EXTRACE_SYS_PANIC = 6,
    EXTRACE_SYS_FRAG = 7,
    EXTRACE_SYS_FTRACE_CALLER = 8,
    EXTRACE_SYS_SKIP = 9, /* cancelled reservation: keeps its tsd, not delivered */
    EXTRACE_SYS_FIRST_USER = 16,
};

/* EXT types (§5.7.5). */
enum extrace_ext_type {
    EXTRACE_EXT_FRAG = 1,
};

/* PRINTF argument classes, 4 bits each in the signature (§10.3). */
enum extrace_arg_class {
    EXTRACE_AC_NONE = 0,
    EXTRACE_AC_U32 = 1,
    EXTRACE_AC_S32 = 2,
    EXTRACE_AC_U64 = 3,
    EXTRACE_AC_S64 = 4,
    EXTRACE_AC_F32 = 5,
    EXTRACE_AC_F64 = 6,
    EXTRACE_AC_STR = 7,
    EXTRACE_AC_PTR = 8,
    EXTRACE_AC_CSTR = 9,
};

/* Error codes; APIs return 0 / a count, or the negated code (§17). */
enum extrace_err {
    EXTRACE_EINVAL = 1,
    EXTRACE_ENOTREADY = 2,
    EXTRACE_EEPOCH = 3,
    EXTRACE_EVERSION = 4,
    EXTRACE_ECORRUPT = 5,
    EXTRACE_EBUSY = 6,
    EXTRACE_ENOSPC = 7,
    EXTRACE_ERANGE = 8,
};

/* Producer commands posted through cstate.cmd_bits (§7.7). */
#define EXTRACE_CMD_FLUSH 0x1u
#define EXTRACE_CMD_SYNC 0x2u
#define EXTRACE_CMD_INFO 0x4u
#define EXTRACE_CMD_THR 0x8u
#define EXTRACE_CMD_ALL 0xFu

/* Event override state (§9.3). */
enum extrace_ev_state {
    EXTRACE_EVS_DEFAULT = 0,
    EXTRACE_EVS_ON = 1,
    EXTRACE_EVS_OFF = 2,
};

/* System-wide ring ID that addresses every ring (§12.10). */
#define EXTRACE_RING_ALL 0xFFu

#endif /* EXTRACE_TYPES_H */
