/* SPDX-License-Identifier: MIT */
#ifndef VARINT_VARIANTS_H
#define VARINT_VARIANTS_H

#include <stdint.h>

unsigned int vt_len32_clz(uint32_t v);
unsigned int vt_len64_clz(uint32_t lo, uint32_t hi);
unsigned int vt_len32_chain(uint32_t v);
unsigned int vt_len64_chain(uint32_t lo, uint32_t hi);
uint8_t *vt_put32(uint8_t *p, uint32_t v);
uint8_t *vt_put32_any(uint8_t *p, uint32_t v);
uint8_t *vt_put64(uint8_t *p, uint32_t lo, uint32_t hi);
unsigned int vt_get32(const uint8_t *p, const uint8_t *end, uint32_t *out);
unsigned int vt_get64(const uint8_t *p, const uint8_t *end, uint64_t *out);
uint32_t vt_zz32(int32_t s);
uint64_t vt_zz64(int64_t s);
int32_t vt_unzz32(uint32_t u);
int64_t vt_unzz64(uint64_t u);

#endif
