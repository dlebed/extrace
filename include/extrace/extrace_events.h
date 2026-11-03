/* SPDX-License-Identifier: MIT */
/*
 * extrace_events.h - X-macro expansion of the image's event catalog (DESIGN §4).
 *
 * The image provides extrace_events.def on its include path:
 *
 *     EXTRACE_GROUP(name, compiled_in)
 *     EXTRACE_EV(name, kind, default, group, "spec")
 *     EXTRACE_EV_STRUCT(name, default, group, c_type)
 *
 * A build may override a group with -DEXTRACE_GRP_<name>_ON=0 or =1.
 */
#ifndef EXTRACE_EVENTS_H
#define EXTRACE_EVENTS_H

#include <stdint.h>
#include "extrace_cfg.h"
#include "extrace_types.h"

/* C types named by EXTRACE_EV_STRUCT entries, declared next to the catalog. */
#if __has_include("extrace_events_types.h")
#include "extrace_events_types.h"
#endif

/* ---- group selection: -DEXTRACE_GRP_<g>_ON overrides the catalog value ---- */
#define EXTRACE_GRPPH_1 ~, 1
#define EXTRACE_GRPPH_0 ~, 0
#define EXTRACE_ARG2__(a, b, ...) b
#define EXTRACE_ARG2_(...) EXTRACE_ARG2__(__VA_ARGS__)
#define EXTRACE_GRP_PICK2_(dflt, val) EXTRACE_ARG2_(EXTRACE_GRPPH_##val, dflt, ~)
#define EXTRACE_GRP_PICK_(dflt, val) EXTRACE_GRP_PICK2_(dflt, val)
#define EXTRACE_GRP_ON_(grp) EXTRACE_GRP_PICK_(EXTRACE_GRPCAT_##grp, EXTRACE_GRP_##grp##_ON)

/* Catalog values of the groups. */
#define EXTRACE_GROUP(name, on) EXTRACE_GRPCAT_##name = (on),
#define EXTRACE_EV(name, kind, def, grp, spec)
#define EXTRACE_EV_STRUCT(name, def, grp, type)
enum extrace_grp_catalog {
#include "extrace_events.def"
    EXTRACE_GRPCAT_END_
};
#undef EXTRACE_GROUP
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT

/* Effective compile-time state of the groups. */
#define EXTRACE_GROUP(name, on) EXTRACE_GRP_STATE_##name = EXTRACE_GRP_ON_(name),
#define EXTRACE_EV(name, kind, def, grp, spec)
#define EXTRACE_EV_STRUCT(name, def, grp, type)
enum extrace_grp_state {
#include "extrace_events.def"
    EXTRACE_GRP_STATE_END_
};
#undef EXTRACE_GROUP
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT

/* Event IDs, assigned sequentially from 16 in catalog order (§4.2). */
#define EXTRACE_GROUP(name, on)
#define EXTRACE_EV(name, kind, def, grp, spec) EXTRACE_EV_##name,
#define EXTRACE_EV_STRUCT(name, def, grp, type) EXTRACE_EV_##name,
enum extrace_ev_id {
    EXTRACE_EV_SYS_LAST_ = EXTRACE_SYS_FIRST_USER - 1,
#include "extrace_events.def"
    EXTRACE_EV_COUNT
};
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT

/* Declared kinds and compile-time switches. */
#define EXTRACE_EV(name, kind, def, grp, spec)                                   \
    EXTRACE_KIND_##name = EXTRACE_K_##kind,                                    \
    EXTRACE_CT_##name = (EXTRACE_CFG_ENABLE && EXTRACE_GRP_STATE_##grp),        \
    EXTRACE_DEF_##name = EXTRACE_DEF_##def##_,
#define EXTRACE_EV_STRUCT(name, def, grp, type)                                  \
    EXTRACE_KIND_##name = EXTRACE_K_STRUCT,                                    \
    EXTRACE_CT_##name = (EXTRACE_CFG_ENABLE && EXTRACE_GRP_STATE_##grp),        \
    EXTRACE_DEF_##name = EXTRACE_DEF_##def##_,
#define EXTRACE_DEF_ON_ 1u
#define EXTRACE_DEF_OFF_ 0u
enum extrace_ev_attr {
    EXTRACE_EV_ATTR_FIRST_ = 0,
#include "extrace_events.def"
    EXTRACE_EV_ATTR_END_
};
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT

/* Declared C types of STRUCT events; other events get an incomplete type. */
struct extrace_untyped;
#define EXTRACE_EV(name, kind, def, grp, spec) typedef struct extrace_untyped extrace_type_##name;
#define EXTRACE_EV_STRUCT(name, def, grp, type) typedef type extrace_type_##name;
#include "extrace_events.def"
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT
#undef EXTRACE_GROUP

enum {
    EXTRACE_MASK_WORDS = (EXTRACE_EV_COUNT + 31) / 32,
};

static_assert(EXTRACE_EV_COUNT <= EXTRACE_CFG_MAX_EVENTS,
              "event catalog larger than EXTRACE_CFG_MAX_EVENTS");

/*
 * Defaults bitmap (§4.5): one 1-bit field per event in ID order, overlaid on the
 * mask words. Fields are named ev_<name>, so a user macro of the same name as an
 * event cannot break the expansion.
 */
#define EXTRACE_GROUP(name, on)
#define EXTRACE_EV(name, kind, def, grp, spec) unsigned int ev_##name : 1;
#define EXTRACE_EV_STRUCT(name, def, grp, type) unsigned int ev_##name : 1;
struct extrace_ev_bits {
    unsigned int : 4;  /* IDs 0..15: system events, off but the FTRACE switch */
    unsigned int sys_ftrace : 1;
    unsigned int : 11;
#include "extrace_events.def"
};
#undef EXTRACE_GROUP
#undef EXTRACE_EV
#undef EXTRACE_EV_STRUCT

union extrace_ev_defaults {
    struct extrace_ev_bits b;
    uint32_t w[EXTRACE_MASK_WORDS];
};
static_assert(sizeof(union extrace_ev_defaults) == 4u * EXTRACE_MASK_WORDS);

extern const union extrace_ev_defaults extrace_ev_defaults;

static inline uint32_t extrace_rbit32(uint32_t v)
{
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}

/* Word w of the defaults bitmap; bit-fields start at the MSB on big-endian targets. */
static inline uint32_t extrace_ev_default_word(unsigned int w)
{
#if EXTRACE_BIG_ENDIAN
    return extrace_rbit32(extrace_ev_defaults.w[w]);
#else
    return extrace_ev_defaults.w[w];
#endif
}

#endif /* EXTRACE_EVENTS_H */
