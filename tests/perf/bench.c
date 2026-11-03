// SPDX-License-Identifier: MIT
/*
 * bench.c - instructions retired by each producer fast path (run_perf.py).
 *
 * Bare-metal image: each row is measured as the difference of two reads of the
 * retired-instruction counter around the statement (RV32: minstret under qemu
 * -icount; ARMv7-M: a counter mapped by the unicorn runner). Function entry
 * tracing rows are the cost added to a traced function, against the same body
 * without NOPs. Output: "name,instructions,bytes" lines; pass 2 is the steady
 * state.
 */
#include "extrace_prod.h"
#include "extrace_ftrace.h"

#if defined(__riscv)
#define UART ((volatile uint8_t *)0x10000000u) /* qemu virt 16550 */
#define STUB_INSNS 0u                           /* traced() is plain() with NOPs */

static __always_inline uint32_t instret(void)
{
    uint32_t v;

    __asm__ volatile("csrr %0, minstret" : "=r"(v) : : "memory");
    return v;
}

__attribute__((patchable_function_entry(2), noinline)) uint32_t
traced(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return a + b + c + d;
}
#elif defined(__arm__)
#define UART ((volatile uint8_t *)0x40000008u)
#define STUB_INSNS 1u /* the b.w into plain() */

static __always_inline uint32_t instret(void)
{
    uint32_t v;

    barrier();
    v = *(volatile uint32_t *)0x40000000u;
    barrier();
    return v;
}

/* the entry -fpatchable-function-entry=3 gives (clang lacks it for Thumb) */
uint32_t traced(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
__asm__(".syntax unified\n.thumb\n.global traced\n.type traced, %function\n.p2align 2\n"
        ".thumb_func\ntraced: nop\n nop\n nop\n b.w plain\n");
#endif

__attribute__((noinline)) uint32_t plain(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return a + b + c + d;
}

static void puts_(const char *s)
{
    while (*s)
        *UART = (uint8_t)*s++;
}

static void putu(uint32_t v)
{
    char b[11];
    int i = 10;

    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    puts_(&b[i]);
}

static uint32_t cal;

#define MEASURE(stmt)                 \
    ({                                \
        uint32_t t0_ = instret();     \
        stmt;                         \
        uint32_t t1_ = instret();     \
        t1_ - t0_ - cal;              \
    })

static void row(const char *name, uint32_t n, uint32_t bytes)
{
    puts_(name);
    puts_(",");
    putu(n);
    puts_(",");
    putu(bytes);
    puts_("\n");
}

/* through volatile pointers: same call sequence for both, no inlining */
typedef uint32_t fn4(uint32_t, uint32_t, uint32_t, uint32_t);
static fn4 *volatile traced_p = traced;
static fn4 *volatile plain_p = plain;
/* the arguments of the traced call: varints of 1, 2, 3 and 5 bytes */
static volatile uint32_t sink, arg = 7, arg2 = 300, arg3 = 70000, arg4 = 0x80001234u;

static uint8_t block[16384] __attribute__((aligned(64)));

#define HEAD() (extrace_producer.head)

/* each measurement in a function of its own, as a trace point in ordinary code */
#define BENCH(fn, stmt)                                       \
    static __attribute__((noinline)) uint32_t fn(void)        \
    {                                                         \
        return MEASURE(stmt);                                 \
    }

BENCH(b_mark, EXTRACE_MARK(P_MARK))
BENCH(b_none, EXTRACE(P_NONE))
BENCH(b_ints, EXTRACE_INTS(P_INTS, arg))
BENCH(b_flush, extrace_prod_flush())

#define RUN(name, fn)                                    \
    do {                                                 \
        uint32_t h0 = HEAD(), n = fn();                  \
        row(name, n, HEAD() - h0);                       \
    } while (0)

static __attribute__((noinline)) uint32_t call_plain(void)
{
    return MEASURE(sink = plain_p(arg, arg2, arg3, arg4));
}

static __attribute__((noinline)) uint32_t call_traced(void)
{
    return MEASURE(sink = traced_p(arg, arg2, arg3, arg4)) - STUB_INSNS;
}

static void ft_row(const char *name)
{
    uint32_t h0 = HEAD(), n = call_traced();

    row(name, n - call_plain(), HEAD() - h0);
}

/* patch traced() with flavor f, then measure */
static void ft_flavor(const char *name, unsigned int f)
{
    extrace_ftrace_patch((uintptr_t)&traced, f);
    ft_row(name);
}

int main(void)
{
    struct extrace_prod_cfg c = { .block = block, .block_size = sizeof block, .name = "perf" };
    uint32_t bit = 1u << (EXTRACE_EV_P_MARK & 31);

    cal = 0;
    cal = MEASURE((void)0);
    if (extrace_prod_init(&c) != 0) {
        puts_("init failed\n");
        return 1;
    }
    extrace_prod_flush();
    for (int pass = 1; pass <= 2; pass++) {
        puts_(pass == 1 ? "# pass 1\n" : "# pass 2\n");
        RUN("MARK", b_mark);
        RUN("NONE", b_none);
        RUN("INTS 1 arg", b_ints);
        extrace_ftrace_patch((uintptr_t)&traced, EXTRACE_FTRACE_OFF);
        row("FTRACE not patched", call_traced() - call_plain(), 0);
        ft_flavor("FTRACE MARK", EXTRACE_FTRACE_MARK);
        ft_flavor("FTRACE NONE", EXTRACE_FTRACE_NONE);
        ft_flavor("FTRACE ARGS 1 arg", EXTRACE_FTRACE_ARGS(1));
        ft_flavor("FTRACE ARGS 4 args", EXTRACE_FTRACE_ARGS(4));
#if EXTRACE_CFG_FTRACE_CALLER
        ft_flavor("FTRACE MARK, caller", EXTRACE_FTRACE_MARK | EXTRACE_FTRACE_CALLER);
        ft_flavor("FTRACE NONE, caller", EXTRACE_FTRACE_NONE | EXTRACE_FTRACE_CALLER);
        ft_flavor("FTRACE ARGS 1 arg, caller", EXTRACE_FTRACE_ARGS(1) | EXTRACE_FTRACE_CALLER);
#endif
        extrace_ftrace_patch((uintptr_t)&traced, EXTRACE_FTRACE_NONE);
        extrace_producer.mask[0] &= ~(1u << EXTRACE_SYS_FTRACE);
        ft_row("FTRACE patched, switch off");
        extrace_producer.mask[0] |= 1u << EXTRACE_SYS_FTRACE;
        extrace_producer.mask[EXTRACE_EV_P_MARK >> 5] &= ~bit;
        RUN("MARK, event off", b_mark);
        extrace_producer.mask[EXTRACE_EV_P_MARK >> 5] |= bit;
        RUN("publish", b_flush);
        RUN("publish, nothing new", b_flush);
    }
    puts_("publish_thr,");
    putu(extrace_producer.publish_thr);
    puts_(",0\n");
    return 0;
}
