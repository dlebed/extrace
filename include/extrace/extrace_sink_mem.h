/* SPDX-License-Identifier: MIT */
/*
 * extrace_sink_mem.h - bulk sink that captures frames into a linear memory buffer
 * (DESIGN §12.8), for retrieval over a debugger or a later upload.
 */
#ifndef EXTRACE_SINK_MEM_H
#define EXTRACE_SINK_MEM_H

#include "extrace_disp.h"

struct extrace_sink_mem {
    struct extrace_sink sink;
    uint8_t *buf;
    uint32_t size;
    uint32_t used;
    uint32_t frames;
};

/* lossy: drop windows when full (GAP); otherwise apply back-pressure */
void extrace_sink_mem_init(struct extrace_sink_mem *m, void *buf, uint32_t size, bool lossy);
void extrace_sink_mem_reset(struct extrace_sink_mem *m);

#endif /* EXTRACE_SINK_MEM_H */
