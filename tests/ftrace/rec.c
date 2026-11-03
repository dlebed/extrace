// SPDX-License-Identifier: MIT
/*
 * rec.c - the real trampoline, patcher and producer end to end, on QEMU
 * (run_emu.py --emulator qemu). Every entry of the trampoline table is patched
 * in turn into traced(), which is called with eight distinct arguments; the
 * record it leaves in the ring must have the exact bytes of its flavor, and the
 * function must return its usual result. A second function keeps its own
 * flavor meanwhile. Built with and without EXTRACE_CFG_FTRACE_CALLER: without
 * it, the CALLER flavors must be refused and record nothing new.
 *
 * start() returns a bitmask of failed global checks; entry_fail[k] holds the
 * failed checks of table entry k.
 */
#include "extrace_prod.h"
#include "extrace_ftrace.h"

#define NREG EXTRACE_FTRACE_ARG_REGS

/* varints of 1, 2, 3, 5, 1, 4, 5 and 1 bytes */
static const uint32_t A[8] = {7, 300, 70000, 0x80001234u, 5, 0x12345678u, 0xFFFFFFFFu, 0x40};

volatile uint32_t caller_seen, other_seen;
volatile uint32_t entry_fail[EXTRACE_FTRACE_ENTRIES];

#if defined(__riscv)
#define BASE 0x80000000u
#define TRACED(name, seen)                                                                         \
    __attribute__((patchable_function_entry(2), noinline)) uint32_t name(                          \
        uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,        \
        uint32_t h)                                                                                \
    {                                                                                              \
        seen = (uint32_t)(uintptr_t)__builtin_return_address(0);                                   \
        return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;                          \
    }
TRACED(traced, caller_seen)
TRACED(other, other_seen)
#elif defined(__arm__)
#define BASE 0x20000000u
/* the entry -fpatchable-function-entry=3 gives (clang lacks it for Thumb) */
#define TRACED(name, seen)                                                                         \
    uint32_t name(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t); \
    __attribute__((noinline)) uint32_t name##_body(uint32_t a, uint32_t b, uint32_t c, uint32_t d, \
                                                   uint32_t e, uint32_t f, uint32_t g, uint32_t h) \
    {                                                                                              \
        seen = (uint32_t)(uintptr_t)__builtin_return_address(0);                                   \
        return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;                          \
    }                                                                                              \
    __asm__(".syntax unified\n.thumb\n.global " #name "\n.type " #name ", %function\n"             \
            ".p2align 2\n.thumb_func\n" #name ": nop\n nop\n nop\n b.w " #name "_body\n");
TRACED(traced, caller_seen)
TRACED(other, other_seen)
#endif

#if EXTRACE_CFG_FTRACE_BASE != BASE
#error "build with EXTRACE_CFG_FTRACE_BASE at the image base"
#endif

typedef uint32_t fn8(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                     uint32_t);
static fn8 *volatile traced_p = traced, *volatile other_p = other;

static uint32_t want_ret(void)
{
    uint32_t r = 0;

    for (int i = 0; i < 8; i++)
        r += (uint32_t)(i + 1) * A[i];
    return r;
}

static uint8_t block[16384] __attribute__((aligned(64)));
static uint8_t got[96], exp_[96];

static unsigned int put_uv(uint8_t *p, uint32_t v)
{
    unsigned int n = 0;

    while (v >= 0x80) {
        p[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

static unsigned int get_uv(const uint8_t *p, unsigned int lim, uint32_t *v)
{
    unsigned int n = 0, sh = 0;

    *v = 0;
    while (n < lim && n < 5) {
        *v |= (uint32_t)(p[n] & 0x7F) << sh;
        sh += 7;
        if (!(p[n++] & 0x80))
            return n;
    }
    return 0;
}

/* the bytes written since index h0 */
static unsigned int take(uint32_t h0)
{
    uint32_t n = extrace_producer.head - h0;

    if (n > sizeof got)
        return sizeof got + 1;
    for (uint32_t i = 0; i < n; i++)
        got[i] = extrace_producer.data[(h0 + i) & extrace_producer.idx_mask];
    return n;
}

/*
 * The record of one call of a function at pc with flavor f, from caller's return
 * address cr: 0 if got[0..n) is exactly that record (any tsd), else failed checks.
 */
static uint32_t check_rec(unsigned int n, uintptr_t pc, unsigned int f, uint32_t cr)
{
    unsigned int s = f & ~(unsigned int)EXTRACE_FTRACE_CALLER, i = 1, e = 0, k;
    bool c = f & EXTRACE_FTRACE_CALLER;
    uint32_t kind = s == EXTRACE_FTRACE_MARK   ? EXTRACE_K_MARK
                    : s == EXTRACE_FTRACE_NONE ? EXTRACE_K_NONE
                                               : EXTRACE_K_INTS;
    uint32_t hdr = (uint32_t)(c ? EXTRACE_SYS_FTRACE_CALLER : EXTRACE_SYS_FTRACE) << 3 | kind;
    uint32_t v, len = 0, ret = (uint32_t)(pc & ~(uintptr_t)1) + EXTRACE_FTRACE_RET_OFFSET;

    if (n < 2 || n > sizeof got)
        return 1u << 0;
    if (got[0] != hdr)
        return 1u << 1;
    if (kind != EXTRACE_K_MARK) { /* tsd: any value */
        k = get_uv(got + i, n - i, &v);
        if (!k)
            return 1u << 2;
        i += k;
    }
    if (kind == EXTRACE_K_INTS) {
        k = get_uv(got + i, n - i, &len);
        if (!k || len != n - i - k)
            return 1u << 3;
        i += k;
        exp_[e++] = 0; /* sign mask */
    }
    e += put_uv(exp_ + e, ret - EXTRACE_CFG_FTRACE_BASE);
    if (c)
        e += put_uv(exp_ + e, cr - EXTRACE_CFG_FTRACE_BASE);
    if (kind == EXTRACE_K_INTS)
        for (unsigned int a = 0; a < s - EXTRACE_FTRACE_NONE; a++)
            e += put_uv(exp_ + e, A[a]);
    if (n - i != e)
        return 1u << 4;
    for (k = 0; k < e; k++)
        if (got[i + k] != exp_[k])
            return 1u << 5;
    return 0;
}

static uint32_t call(fn8 *volatile *p)
{
    return (*p)(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7]);
}

static unsigned int flavor_of(int k)
{
    return k < EXTRACE_FTRACE_SHAPES
               ? EXTRACE_FTRACE_MARK + (unsigned int)k
               : (EXTRACE_FTRACE_MARK + (unsigned int)(k - EXTRACE_FTRACE_SHAPES)) |
                     EXTRACE_FTRACE_CALLER;
}

uint32_t start(void)
{
    struct extrace_prod_cfg cfg = {.block = block, .block_size = sizeof block, .name = "rec"};
    uintptr_t pc = (uintptr_t)&traced, po = (uintptr_t)&other;
    const uint32_t want = want_ret();
    const unsigned int of =
        EXTRACE_FTRACE_NONE | (EXTRACE_CFG_FTRACE_CALLER ? EXTRACE_FTRACE_CALLER : 0);
    uint32_t fail = 0, h0;

    if (extrace_prod_init(&cfg) != 0)
        return 1u << 31;
    /* not patched: no record */
    h0 = extrace_producer.head;
    fail |= (call(&traced_p) != want || take(h0) != 0) << 0;
    /* patched, switch off: no record */
    fail |= (extrace_ftrace_patch(pc, EXTRACE_FTRACE_MARK) != 0) << 1;
    extrace_producer.mask[0] &= ~(1u << EXTRACE_SYS_FTRACE);
    h0 = extrace_producer.head;
    fail |= (call(&traced_p) != want || take(h0) != 0) << 2;
    extrace_producer.mask[0] |= 1u << EXTRACE_SYS_FTRACE;
    /* a second function keeps its own flavor */
    fail |= (extrace_ftrace_patch(po, of) != 0) << 3;

    unsigned int cur = EXTRACE_FTRACE_MARK;

    for (int k = 0; k < EXTRACE_FTRACE_ENTRIES; k++) {
        unsigned int f = flavor_of(k), n;
        uint32_t ef = 0;

        if ((f & EXTRACE_FTRACE_CALLER) && !EXTRACE_CFG_FTRACE_CALLER) {
            /* refused; traced() keeps recording its previous flavor */
            ef |= (extrace_ftrace_patch(pc, f) != -EXTRACE_EINVAL) << 8;
            f = cur;
        } else {
            ef |= (extrace_ftrace_patch(pc, f) != 0) << 9;
            cur = f;
        }
        h0 = extrace_producer.head;
        caller_seen = 0;
        ef |= (call(&traced_p) != want) << 10;
        ef |= (caller_seen == 0) << 11;
        n = take(h0);
        ef |= check_rec(n, pc, f, caller_seen) << 12;
        h0 = extrace_producer.head;
        other_seen = 0;
        ef |= (call(&other_p) != want) << 20;
        n = take(h0);
        ef |= check_rec(n, po, of, other_seen) << 21;
        entry_fail[k] = ef;
        fail |= ef ? 1u << 4 : 0;
    }
    /* back to the NOPs: no record */
    fail |= (extrace_ftrace_patch(pc, EXTRACE_FTRACE_OFF) != 0) << 5;
    h0 = extrace_producer.head;
    fail |= (call(&traced_p) != want || take(h0) != 0) << 6;
    fail |= (extrace_producer.lost_pending != 0 || extrace_producer.stats.lost_total != 0) << 7;
    return fail;
}
