/* SPDX-License-Identifier: MIT */
/*
 * extrace_meta.h - metadata descriptors placed in extrace_meta_* sections (DESIGN §10).
 *
 * The descriptors live only in the ELF (mode INFO) and are read by the host
 * extractor. The device needs nothing from them at runtime except the site ID,
 * which is derived from a descriptor's address.
 */
#ifndef EXTRACE_META_H
#define EXTRACE_META_H

#include <stdint.h>
#include "extrace_cfg.h"

#define EXTRACE_SITE_ALIGN 32
#define EXTRACE_SITE_SHIFT 5
#define EXTRACE_SITE_ID_MASK \
    ((uint32_t)(EXTRACE_CFG_SITE_ID_BITS >= 32 ? 0xFFFFFFFFu : (1u << EXTRACE_CFG_SITE_ID_BITS) - 1u))

struct extrace_site {               /* lives only in the ELF (§10.5) */
    const char *fmt;
    const char *file;
    uint32_t line;
    uint16_t event;
    uint8_t nargs;
    uint8_t flags;
    uint32_t sig_lo;                /* 4-bit class per argument, arg 0 in bits 3:0 */
    uint32_t sig_hi;                /* arguments 8..15 */
} __aligned(EXTRACE_SITE_ALIGN);

struct extrace_meta_ev {            /* one per catalog event */
    const char *name;
    const char *group;
    const char *spec;
    const char *ctype;              /* STRUCT events: C type name */
    uint16_t id;
    uint8_t kind;
    uint8_t def_on;
    uint8_t compiled_in;
    uint8_t reserved[3];
};

struct extrace_meta_img {
    uint32_t magic;                 /* "EXMI" */
    uint16_t layout_ver;
    uint8_t wire_ver;
    uint8_t site_id_bits;
    uint16_t ev_count;
    uint16_t site_shift;
    uint32_t max_payload;
    uint32_t meta_mode;
};

#define EXTRACE_META_IMG_MAGIC 0x494D5845u /* "EXMI" */

#if EXTRACE_CFG_META_MODE == EXTRACE_META_INFO
#define EXTRACE_META_SITE_BASE ((uintptr_t)0)
#else
extern const struct extrace_site __start_extrace_meta_site[];
#define EXTRACE_META_SITE_BASE ((uintptr_t)__start_extrace_meta_site)
#endif

/* Site ID: the descriptor's offset, scaled by its alignment (§10.2). */
static __always_inline uint32_t extrace_site_id(const struct extrace_site *site,
                                                const char *fmt)
{
#if EXTRACE_CFG_SITE_ID_MODE == EXTRACE_SITE_FMT_ADDR
    (void)site;
    return (uint32_t)(uintptr_t)fmt & EXTRACE_SITE_ID_MASK;
#else
    uintptr_t off = (uintptr_t)site - EXTRACE_META_SITE_BASE;

    (void)fmt;
    return (uint32_t)(off >> EXTRACE_SITE_SHIFT) & EXTRACE_SITE_ID_MASK;
#endif
}

#endif /* EXTRACE_META_H */
