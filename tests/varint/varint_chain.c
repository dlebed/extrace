// SPDX-License-Identifier: MIT
/* varint_chain.c - the varint lengths without a CLZ instruction. */
#include <stdint.h>
#include "varint_variants.h"

#undef EXTRACE_VARINT_CLZ32
#include "extrace_varint.h"

#ifdef EXTRACE_VARINT_CLZ32
#error "the chain variant must not use CLZ"
#endif

unsigned int vt_len32_chain(uint32_t v)
{
    return extrace_uv_len32(v);
}

unsigned int vt_len64_chain(uint32_t lo, uint32_t hi)
{
    return extrace_uv_len64(lo, hi);
}
