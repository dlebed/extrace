/* SPDX-License-Identifier: MIT */
/*
 * extrace_user_cfg.h - configuration of the example image.
 *
 * Firmware keeps the metadata (event names, printf formats) in the ELF only
 * (EXTRACE_META_INFO, the default) and the host reads it from there. The examples
 * run producer and viewer in one process, so they load it into memory instead:
 * the viewer then reads the very descriptors a host tool would read from the ELF.
 */
#ifndef EXTRACE_USER_CFG_H
#define EXTRACE_USER_CFG_H

#define EXTRACE_CFG_META_MODE EXTRACE_META_RODATA

/* Function entry tracing: built, switched on, with the CALLER flavors (08_ftrace). */
#define EXTRACE_CFG_FTRACE 1
#define EXTRACE_CFG_FTRACE_ON 1
#define EXTRACE_CFG_FTRACE_CALLER 1

#endif /* EXTRACE_USER_CFG_H */
