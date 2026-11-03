// SPDX-License-Identifier: MIT
/*
 * diff.c SEED OPS - a random producer workload on the POSIX port (run_diff.py).
 *
 * After every operation it prints a digest of all the state another agent or
 * the application can observe: the whole ring block, the producer's fields and
 * statistics, the reservation positions (test hook) and the number of timer
 * reads. Two producer builds that print the same lines behave the same.
 *
 * The workload covers every reservation path: drops and LOST reports, wraps,
 * tail refreshes, aligned and nested direct reservations up to 255 open, cancel,
 * urgent records, panic and re-init, and the three function tracing modes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extrace.h"
#include "extrace_cons.h"
#include "extrace_ftrace.h"
#include "extrace_test_hooks.h"

#define DATA 1024u
static uint64_t blk[(EXTRACE_RING_BLOCK_SIZE(4 * DATA, EXTRACE_MASK_WORDS) + 7) / 8];
static uint32_t mirror[64];
static struct extrace_cons C;
static uint64_t rng;
static uint32_t now = 1000, ts_reads, step = 1;
static uint64_t hk;

static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return n ? (uint32_t)(rng % n) : 0;
}

static uint32_t ts(void)
{
    ts_reads++;
    now += step;
    return now;
}

static void fnv(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = p;

    while (n--)
        *h = (*h ^ *b++) * 0x100000001b3ull;
}

static void resv_hook(uint32_t ep, uint32_t idx)
{
    fnv(&hk, &ep, 4);
    fnv(&hk, &idx, 4);
}

static void start(uint32_t size)
{
    struct extrace_prod_cfg cfg = {
        .block = blk,
        .block_size = EXTRACE_RING_BLOCK_SIZE(size, EXTRACE_MASK_WORDS),
        .data_size = size,
        .ring_id = 3,
        .name = "df",
        .publish_thr = rnd(3) ? 0 : 1 + rnd(size),
    };
    int e = extrace_prod_init(&cfg);

    if (e != 0 || extrace_cons_attach(&C, blk, cfg.block_size, mirror, 64, 0) != 0) {
        printf("start failed %d\n", e);
        exit(1);
    }
}

#define H(f) fnv(&h, &p->f, sizeof p->f)

static void digest(int op)
{
    const struct extrace_prod *p = &extrace_producer;
    uint32_t lost_bytes = EXTRACE_CFG_LOST_BYTES ? p->lost_bytes : 0; /* reported only then */
    uint64_t h = 0xcbf29ce484222325ull;

    fnv(&h, blk, sizeof blk);
    fnv(&h, &lost_bytes, 4);
    fnv(&h, &ts_reads, 4);
    H(mask);
    H(head);
    H(committed);
    H(tail_cache);
    H(lost_pending);
    H(last_ts);
    H(inflight);
    H(publishing);
    H(publish_again);
    H(flags);
    H(notify_force);
    H(attached_seen);
    H(published);
    H(notified);
    H(publish_thr);
    H(notify_thr);
    H(lost_total);
    H(lost_total_st);
    H(epoch);
    H(hwm);
    H(publish_cnt);
    H(frag_seq);
    H(last_sync_ts);
    H(size);
    H(stats);
    printf("%d %016llx %016llx\n", op, (unsigned long long)h, (unsigned long long)hk);
}

static uint8_t buf[4096];
static uint32_t ft_args[9] = { 1, 200, 70000, 0xffffffffu, 5, 6, 7, 8, 0x1234 }; /* + caller */

/* m: 1 MARK, 2 NONE, 3 ARGS(n) */
static void ft_hit(uint32_t ret, uint32_t m, uint32_t n, bool caller)
{
    unsigned int f = m == 1 ? EXTRACE_FTRACE_MARK : m == 2 ? EXTRACE_FTRACE_NONE : EXTRACE_FTRACE_ARGS(n);

    extrace_ftrace_hit(ret, ft_args[8], ft_args, f | (caller ? EXTRACE_FTRACE_CALLER : 0u));
}

int main(int argc, char **argv)
{
    unsigned long seed;
    int ops;
    struct extrace_resv open[300];
    int nopen = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: diff SEED OPS\n");
        return 2;
    }
    seed = strtoul(argv[1], 0, 0);
    ops = atoi(argv[2]);
    rng = seed * 0x9E3779B97F4A7C15ull + 1;
    extrace_posix_ts_hook = ts;
    extrace_test_resv_hook = resv_hook;
    for (uint32_t i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 7 + 3);
    EXTRACE_MARK(T_MARK); /* before init: not ready */
    start(2 * DATA);
    for (int op = 0; op < ops; op++) {
        uint32_t k = rnd(30), a = rnd(4) == 0 ? rnd(1u << rnd(32)) : rnd(300);

        step = rnd(4) == 0 ? rnd(1u << rnd(28)) : rnd(20);
        switch (k) {
        case 0: case 1: EXTRACE_MARK(T_MARK); break;
        case 2: EXTRACE_MARK_URGENT(T_MARK); break;
        case 3: case 4: EXTRACE(T_NONE); break;
        case 5: EXTRACE_URGENT(T_NONE); break;
        case 6: EXTRACE_INTS(T_INTS, a, (int32_t)-a); break;
        case 7: EXTRACE_INTS(T_SEQ, a, op); break;
        case 8: EXTRACE_INTS(T_ISR, a); break;
        case 9: EXTRACE_PRINTF(T_PRINTF, "x=%u y=%d s=%s", a, -(int)a, "ab"); break;
        case 10: EXTRACE_BUF(T_BUF, buf, rnd(rnd(4) ? 40 : 200)); break;
        case 11: EXTRACE_STR(T_STR, "hello world"); break;
        case 12: case 13: {
            uint32_t m = 1 + rnd(3), n = m == 3 ? 1 + rnd(EXTRACE_FTRACE_ARG_REGS) : 0;

            ft_hit(0x1000 + a * 4, m, n, EXTRACE_CFG_FTRACE_CALLER && rnd(2));
            break;
        }
        case 14: case 15: { /* direct reservations, possibly nested deep */
            uint32_t n = rnd(8) == 0 ? 250 + rnd(10) : 1 + rnd(3), al = 1u << rnd(4);
            if (n > 8)
                while (extrace_cons_read(&C, buf + 2048, 2048) > 0)
                    ;
            for (uint32_t i = 0; i < n && nopen < 300; i++) {
#if EXTRACE_CFG_RESV_ALIGN_MAX >= 8
                bool ok = al > 1 ? EXTRACE_RESERVE(T_ANY, (n > 8 ? 0 : rnd(20)), 8, &open[nopen])
                                 : EXTRACE_RESERVE(T_ANY, (n > 8 ? 0 : rnd(20)), 1, &open[nopen]);
#else
                bool ok = EXTRACE_RESERVE(T_ANY, (n > 8 ? 0 : rnd(20)), 1, &open[nopen]);
#endif
                if (ok) {
                    memset(open[nopen].payload, 0x5a, open[nopen].len);
                    nopen++;
                }
            }
            break;
        }
        case 16: case 17:
            while (nopen) {
                nopen--;
                if (rnd(3) == 0)
                    extrace_cancel(&open[nopen]);
                else if (rnd(5) == 0)
                    extrace_commit_urgent(&open[nopen]);
                else
                    extrace_commit(&open[nopen]);
            }
            break;
        case 18: case 19: case 20: extrace_cons_read(&C, buf + 2048, 1 + rnd(1024)); break;
        case 21: extrace_prod_flush(); break;
        case 22: extrace_prod_poll(); break;
        case 23:
            if (rnd(20) == 0 && nopen == 0) { /* panic, write while not ready, restart */
                extrace_prod_panic(1, 0x1234);
                EXTRACE_MARK(T_MARK);
                EXTRACE_INTS(T_INTS, 1u, 2);
                start(DATA << rnd(3));
            }
            break;
        case 24: EXTRACE_BUF_LARGE(T_BUF, buf, rnd(700)); break;
        default:
            EXTRACE_INTS(T_INTS, a, 3); break;
        }
        digest(op);
    }
    fprintf(stderr, "records %u lost %u publishes %u refresh %u open_max %u ts_reads %u\n",
            extrace_producer.stats.records, extrace_producer.lost_total, extrace_producer.stats.publishes,
            extrace_producer.stats.tail_refreshes, extrace_producer.stats.resv_open_max, ts_reads);
    return 0;
}
