// SPDX-License-Identifier: MIT
/*
 * varint_variants.c - the varint module compiled twice: with a CLZ-based length
 * (as on ARMv7-M, RISC-V Zbb) and with the unrolled shift chain (ARMv6-M).
 */
#include <stdint.h>
#include "varint_variants.h"

#define EXTRACE_VARINT_CLZ32(v) ((unsigned int)__builtin_clz(v))
#include "extrace_varint.h"

unsigned int vt_len32_clz(uint32_t v)
{
    return extrace_uv_len32(v);
}

unsigned int vt_len64_clz(uint32_t lo, uint32_t hi)
{
    return extrace_uv_len64(lo, hi);
}

uint8_t *vt_put32(uint8_t *p, uint32_t v)
{
    return extrace_put_uv32(p, v);
}

uint8_t *vt_put32_any(uint8_t *p, uint32_t v)
{
    return extrace_put_uv32_any(p, v);
}

uint8_t *vt_put64(uint8_t *p, uint32_t lo, uint32_t hi)
{
    return extrace_put_uv64(p, lo, hi);
}

unsigned int vt_get32(const uint8_t *p, const uint8_t *end, uint32_t *out)
{
    return extrace_get_uv32(p, end, out);
}

unsigned int vt_get64(const uint8_t *p, const uint8_t *end, uint64_t *out)
{
    return extrace_get_uv64(p, end, out);
}

uint32_t vt_zz32(int32_t s)
{
    return extrace_zz32(s);
}

uint64_t vt_zz64(int64_t s)
{
    return extrace_zz64(s);
}

int32_t vt_unzz32(uint32_t u)
{
    return extrace_unzz32(u);
}

int64_t vt_unzz64(uint64_t u)
{
    return extrace_unzz64(u);
}
