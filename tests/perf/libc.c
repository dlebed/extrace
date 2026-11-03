// SPDX-License-Identifier: MIT
/* libc.c - the few runtime functions the freestanding images need. */
#include <stddef.h>

void *memset(void *d, int c, size_t n);
void *memcpy(void *restrict d, const void *restrict s, size_t n);

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;

    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *restrict d, const void *restrict s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;

    while (n--)
        *p++ = *q++;
    return d;
}

#ifdef __arm__
void __aeabi_memcpy(void *d, const void *s, size_t n);
void __aeabi_memcpy4(void *d, const void *s, size_t n);
void __aeabi_memclr(void *d, size_t n);
void __aeabi_memclr4(void *d, size_t n);
void __aeabi_memset(void *d, size_t n, int c);

void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
#endif
