/* SPDX-License-Identifier: MIT */
/*
 * extrace_test.h - minimal, dependency-free test harness (DESIGN §18.0).
 *
 * TEST(name) { ... } registers a test through a constructor. EXPECT_* report and
 * continue, ASSERT_* report and leave the test.
 */
#ifndef EXTRACE_TEST_H
#define EXTRACE_TEST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct xt_test {
    const char *name;
    void (*fn)(void);
    struct xt_test *next;
};

void xt_register(struct xt_test *t);
bool xt_check(bool ok, const char *expr, const char *file, int line);
bool xt_check_eq(unsigned long long a, unsigned long long b, bool eq, const char *ea,
                 const char *eb, const char *file, int line);
bool xt_check_mem(const void *a, const void *b, size_t n, const char *ea, const char *eb,
                  const char *file, int line);
void xt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* seed for randomized tests: XT_SEED from the environment, else a fixed value */
uint64_t xt_seed(void);
/* number of iterations for randomized tests, scaled by XT_SCALE */
unsigned long xt_iters(unsigned long base);

#define TEST(name)                                                                  \
    static void xt_fn_##name(void);                                                 \
    static struct xt_test xt_t_##name = { #name, xt_fn_##name, nullptr };           \
    __attribute__((constructor)) static void xt_reg_##name(void)                    \
    {                                                                               \
        xt_register(&xt_t_##name);                                                  \
    }                                                                               \
    static void xt_fn_##name(void)

#define EXPECT_TRUE(x) xt_check(!!(x), #x, __FILE__, __LINE__)
#define EXPECT_FALSE(x) xt_check(!(x), "!(" #x ")", __FILE__, __LINE__)
#define EXPECT_EQ(a, b) \
    xt_check_eq((unsigned long long)(a), (unsigned long long)(b), true, #a, #b, __FILE__, __LINE__)
#define EXPECT_NE(a, b) \
    xt_check_eq((unsigned long long)(a), (unsigned long long)(b), false, #a, #b, __FILE__, __LINE__)
#define EXPECT_MEM(a, b, n) xt_check_mem((a), (b), (n), #a, #b, __FILE__, __LINE__)
#define ASSERT_TRUE(x)        \
    do {                      \
        if (!EXPECT_TRUE(x))  \
            return;           \
    } while (0)
#define ASSERT_EQ(a, b)        \
    do {                       \
        if (!EXPECT_EQ(a, b))  \
            return;            \
    } while (0)

/* xorshift64* for reproducible randomized tests */
static inline uint64_t xt_rand(uint64_t *s)
{
    uint64_t x = *s;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static inline uint32_t xt_rand_below(uint64_t *s, uint32_t n)
{
    return n ? (uint32_t)(xt_rand(s) % n) : 0;
}

#endif /* EXTRACE_TEST_H */
