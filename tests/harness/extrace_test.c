// SPDX-License-Identifier: MIT
/* extrace_test.c - test runner: `test_x [substring]` runs the matching tests. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extrace_test.h"

static struct xt_test *head, **tailp = &head;
static int failures_cur;
static const char *cur_name;

void xt_register(struct xt_test *t)
{
    *tailp = t; /* constructors run in definition order: keep it */
    tailp = &t->next;
}

static void fail_hdr(const char *file, int line)
{
    if (!failures_cur)
        fprintf(stderr, "FAIL %s\n", cur_name);
    failures_cur++;
    fprintf(stderr, "  %s:%d: ", file, line);
}

bool xt_check(bool ok, const char *expr, const char *file, int line)
{
    if (ok)
        return true;
    fail_hdr(file, line);
    fprintf(stderr, "expected %s\n", expr);
    return false;
}

bool xt_check_eq(unsigned long long a, unsigned long long b, bool eq, const char *ea,
                 const char *eb, const char *file, int line)
{
    if ((a == b) == eq)
        return true;
    fail_hdr(file, line);
    fprintf(stderr, "%s %s %s (0x%llx vs 0x%llx, %lld vs %lld)\n", ea, eq ? "==" : "!=", eb, a,
            b, (long long)a, (long long)b);
    return false;
}

bool xt_check_mem(const void *a, const void *b, size_t n, const char *ea, const char *eb,
                  const char *file, int line)
{
    const unsigned char *x = a, *y = b;

    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            fail_hdr(file, line);
            fprintf(stderr, "%s differs from %s at byte %zu (0x%02x vs 0x%02x)\n", ea, eb, i,
                    x[i], y[i]);
            return false;
        }
    }
    return true;
}

void xt_log(const char *fmt, ...)
{
    va_list ap;

    if (!getenv("XT_VERBOSE"))
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

uint64_t xt_seed(void)
{
    const char *s = getenv("XT_SEED");

    return s ? strtoull(s, nullptr, 0) | 1u : 0x9E3779B97F4A7C15ull;
}

unsigned long xt_iters(unsigned long base)
{
    const char *s = getenv("XT_SCALE");
    unsigned long k = s ? strtoul(s, nullptr, 0) : 1;

    return base * (k ? k : 1);
}

int main(int argc, char **argv)
{
    int run = 0, failed = 0;

    for (struct xt_test *t = head; t; t = t->next) {
        if (argc > 1 && !strstr(t->name, argv[1]))
            continue;
        cur_name = t->name;
        failures_cur = 0;
        t->fn();
        run++;
        if (failures_cur)
            failed++;
        else if (getenv("XT_VERBOSE"))
            fprintf(stderr, "ok   %s\n", t->name);
    }
    printf("%d tests, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
