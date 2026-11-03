/* SPDX-License-Identifier: MIT */
/*
 * extrace_cfg.h - configuration defaults and consistency checks (DESIGN §14).
 *
 * Configuration comes from an optional per-image extrace_user_cfg.h and from -D
 * flags. This is the only library header that includes extrace_port.h.
 */
#ifndef EXTRACE_CFG_H
#define EXTRACE_CFG_H

#include <stdint.h>
#include <stddef.h>

#if __has_include("extrace_user_cfg.h")
#include "extrace_user_cfg.h"
#endif

#include "extrace_port.h"

/* Values of EXTRACE_CFG_META_MODE and EXTRACE_CFG_SITE_ID_MODE. */
#define EXTRACE_META_INFO 0
#define EXTRACE_META_PHANTOM 1
#define EXTRACE_META_RODATA 2
#define EXTRACE_SITE_DESC 0
#define EXTRACE_SITE_FMT_ADDR 1

#ifndef EXTRACE_CFG_ENABLE
#define EXTRACE_CFG_ENABLE 1
#endif
#ifndef EXTRACE_CFG_MAX_EVENTS
#define EXTRACE_CFG_MAX_EVENTS 1024
#endif
#ifndef EXTRACE_CFG_MAX_PAYLOAD
#define EXTRACE_CFG_MAX_PAYLOAD 512
#endif
#ifndef EXTRACE_CFG_MAX_ARGS
#define EXTRACE_CFG_MAX_ARGS 16
#endif
#ifndef EXTRACE_CFG_STR_ARG_MAX
#define EXTRACE_CFG_STR_ARG_MAX 64
#endif
#ifndef EXTRACE_CFG_PUBLISH_IN_ISR
#define EXTRACE_CFG_PUBLISH_IN_ISR 1
#endif
#ifndef EXTRACE_CFG_WRITE_IN_CS_MAX
#define EXTRACE_CFG_WRITE_IN_CS_MAX 0
#endif
#ifndef EXTRACE_CFG_RESV_ALIGN_MAX
#define EXTRACE_CFG_RESV_ALIGN_MAX 8
#endif
#ifndef EXTRACE_CFG_EMIT_SPECIALIZE
#define EXTRACE_CFG_EMIT_SPECIALIZE 1
#endif
#ifndef EXTRACE_CFG_DISP_STAGE
#define EXTRACE_CFG_DISP_STAGE 1024
#endif
#ifndef EXTRACE_CFG_SYNC_PERIOD_TICKS
#define EXTRACE_CFG_SYNC_PERIOD_TICKS 0
#endif
#ifndef EXTRACE_CFG_LOST_BYTES
#define EXTRACE_CFG_LOST_BYTES 0
#endif
#ifndef EXTRACE_CFG_PRINTF_LOCAL
#define EXTRACE_CFG_PRINTF_LOCAL 0
#endif
#ifndef EXTRACE_CFG_TEXT_MAX
#define EXTRACE_CFG_TEXT_MAX 128
#endif
#ifndef EXTRACE_CFG_SITE_ID_BITS
#define EXTRACE_CFG_SITE_ID_BITS 16
#endif
#ifndef EXTRACE_CFG_SITE_ID_MODE
#define EXTRACE_CFG_SITE_ID_MODE EXTRACE_SITE_DESC
#endif
#ifndef EXTRACE_CFG_META_MODE
#define EXTRACE_CFG_META_MODE EXTRACE_META_INFO
#endif
#ifndef EXTRACE_CFG_DISP_MAX_RINGS
#define EXTRACE_CFG_DISP_MAX_RINGS 16
#endif
#ifndef EXTRACE_CFG_FTRACE
#define EXTRACE_CFG_FTRACE 0          /* function entry tracing (docs/ftrace.md) */
#endif
#ifndef EXTRACE_CFG_FTRACE_ON
#define EXTRACE_CFG_FTRACE_ON 0       /* default state of the FTRACE switch */
#endif
#ifndef EXTRACE_CFG_FTRACE_CALLER
#define EXTRACE_CFG_FTRACE_CALLER 0   /* builds the CALLER flavors (FTRACE_CALLER records) */
#endif
#ifndef EXTRACE_CFG_FTRACE_BASE
#define EXTRACE_CFG_FTRACE_BASE 0u    /* subtracted from every recorded address */
#endif
#ifndef EXTRACE_CFG_STATS
#define EXTRACE_CFG_STATS 1
#endif
#ifndef EXTRACE_CFG_DEBUG
#define EXTRACE_CFG_DEBUG 0
#endif
#ifndef EXTRACE_CFG_TEST_HOOKS
#define EXTRACE_CFG_TEST_HOOKS 0
#endif

/* Derived bounds (§5.6). */
#define EXTRACE_MAX_RECORD (EXTRACE_CFG_MAX_PAYLOAD + 3u + 5u + 3u)
#define EXTRACE_LOST_REC_MAX 19u
#define EXTRACE_MAX_RESV (EXTRACE_MAX_RECORD + EXTRACE_LOST_REC_MAX + EXTRACE_CFG_RESV_ALIGN_MAX - 1u)

static_assert(EXTRACE_CFG_MAX_PAYLOAD >= 64 && EXTRACE_CFG_MAX_PAYLOAD <= 65535,
              "EXTRACE_CFG_MAX_PAYLOAD must be 64..65535");
static_assert(EXTRACE_CFG_MAX_EVENTS >= 16 && EXTRACE_CFG_MAX_EVENTS <= 65535,
              "EXTRACE_CFG_MAX_EVENTS must be 16..65535");
static_assert(EXTRACE_CFG_MAX_ARGS >= 1 && EXTRACE_CFG_MAX_ARGS <= 16,
              "EXTRACE_CFG_MAX_ARGS must be 1..16");
static_assert(EXTRACE_CFG_STR_ARG_MAX >= 1 && EXTRACE_CFG_STR_ARG_MAX <= EXTRACE_CFG_MAX_PAYLOAD,
              "EXTRACE_CFG_STR_ARG_MAX must be 1..MAX_PAYLOAD");
static_assert(EXTRACE_CFG_RESV_ALIGN_MAX == 1 || EXTRACE_CFG_RESV_ALIGN_MAX == 2 ||
                  EXTRACE_CFG_RESV_ALIGN_MAX == 4 || EXTRACE_CFG_RESV_ALIGN_MAX == 8,
              "EXTRACE_CFG_RESV_ALIGN_MAX must be 1, 2, 4 or 8");
static_assert(EXTRACE_CFG_DISP_MAX_RINGS >= 1 && EXTRACE_CFG_DISP_MAX_RINGS <= 255,
              "EXTRACE_CFG_DISP_MAX_RINGS must be 1..255");
static_assert(EXTRACE_CFG_SITE_ID_BITS >= 8 && EXTRACE_CFG_SITE_ID_BITS <= 32,
              "EXTRACE_CFG_SITE_ID_BITS must be 8..32");
static_assert(EXTRACE_CFG_WRITE_IN_CS_MAX <= EXTRACE_MAX_RECORD,
              "EXTRACE_CFG_WRITE_IN_CS_MAX too large");

/* Optional port hooks with defaults (§13.2). */
#ifndef EXTRACE_PORT_HOT_DATA
#define EXTRACE_PORT_HOT_DATA
#endif
#ifndef EXTRACE_PORT_HOT_TEXT
#define EXTRACE_PORT_HOT_TEXT
#endif
#ifndef EXTRACE_PORT_ASSERT
#define EXTRACE_PORT_ASSERT(x) ((void)0)
#endif
#if EXTRACE_CFG_DEBUG
#define EXTRACE_ASSERT(x) EXTRACE_PORT_ASSERT(x)
#else
#define EXTRACE_ASSERT(x) ((void)0)
#endif
#ifndef EXTRACE_PORT_HAS_MEMCPY
#define extrace_port_memcpy EXTRACE_BUILTIN_MEMCPY
#endif
#ifndef EXTRACE_PORT_HAS_MEMSET
#define extrace_port_memset EXTRACE_BUILTIN_MEMSET
#endif
#ifndef EXTRACE_PORT_HAS_NOTIFY
static inline void extrace_port_notify(uint8_t ring_id)
{
    (void)ring_id;
}
#endif
#ifndef EXTRACE_PORT_HAS_NOTIFY_PRODUCER
static inline void extrace_port_notify_producer(uint8_t ring_id)
{
    (void)ring_id;
}
#endif
#ifndef EXTRACE_PORT_HAS_IN_ISR
static inline bool extrace_port_in_isr(void)
{
    return false;
}
#endif
#ifndef EXTRACE_PORT_HAS_BUILD_ID
static inline const uint8_t *extrace_port_build_id(size_t *len)
{
    *len = 0;
    return nullptr;
}
#endif

#endif /* EXTRACE_CFG_H */
