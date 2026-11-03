// SPDX-License-Identifier: MIT
/*
 * test_concurrency.c - real threads on the POSIX port (DESIGN §18.2).
 *
 * A producer thread emits sequence-numbered records and holds direct
 * reservations across slow fills; a signal acting as the ISR interrupts it at
 * random points, including inside the reserve-to-commit window, and emits its own
 * records. A consumer thread runs the dispatcher with a validating record sink in
 * parallel. Every committed record must arrive exactly once and in order per
 * context, no cancelled reservation may arrive, payloads must be intact, and the
 * drop count must match the LOST reports exactly.
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "extrace_test.h"
#include "extrace.h"
#include "extrace_cons.h"
#include "extrace_disp.h"

#define DATA 4096u
#define BLOCK EXTRACE_RING_BLOCK_SIZE(DATA, EXTRACE_MASK_WORDS)

static uint64_t blk[(BLOCK + 7) / 8];
static struct extrace_cons cons;
static uint32_t mirror[8];
static struct extrace_disp disp;
static struct extrace_sink sink;

static pthread_t prod_th;
static atomic_bool prod_done, stop_irq;
static atomic_uint isr_runs;
static uint32_t isr_seq;          /* written by the ISR only */
static uint32_t final_head, final_epoch;
static struct extrace_prod_stats final_stats;
static uint32_t cancels;

/* consumer-side checks */
static uint32_t last[2];
static bool seen[2];
static uint64_t recs, lost;
static uint32_t bad_order, bad_payload;

static void isr(void)
{
    atomic_fetch_add(&isr_runs, 1);
    EXTRACE_INTS(T_ISR, isr_seq++);
    if (isr_seq % 7 == 0)
        EXTRACE_MARK(T_MARK);
}

static void *irq_thread(void *arg)
{
    struct timespec ts = { 0, 2000 };

    (void)arg;
    while (!atomic_load(&stop_irq)) {
        pthread_kill(prod_th, SIGUSR1);
        nanosleep(&ts, nullptr);
    }
    return nullptr;
}

static void *prod_thread(void *arg)
{
    struct extrace_prod_cfg cfg = { .block = blk, .block_size = BLOCK, .ring_id = 1,
                                    .name = "conc", .publish_thr = 256 };
    unsigned long n = (unsigned long)(uintptr_t)arg;
    uint8_t b[64];

    if (extrace_prod_init(&cfg) != 0)
        return nullptr;
    for (unsigned long i = 0; i < n; i++) {
        uint32_t seq = (uint32_t)i;

        if (i % 5 == 4) {
            struct extrace_resv r;

            if (EXTRACE_RESERVE(T_ANY, 32, 4, &r)) {
                memcpy(b, &seq, 4);
                for (unsigned int k = 4; k < 32; k++)
                    b[k] = (uint8_t)(seq + k);
                for (unsigned int k = 0; k < 32; k += 4) { /* slow fill: the ISR runs between */
                    memcpy((uint8_t *)r.payload + k, b + k, 4);
                    sched_yield();
                }
                if (i % 35 == 4) {
                    extrace_cancel(&r);
                    cancels++;
                } else {
                    extrace_commit(&r);
                }
            }
        } else {
            EXTRACE_INTS(T_SEQ, seq, 0u);
        }
        if (i % 64 == 0)
            extrace_prod_poll();
    }
    atomic_store(&stop_irq, true);
    extrace_posix_set_isr(SIGUSR1, nullptr); /* no more interrupts */
    {
        unsigned long flags;

        /* quiesce: report the last drops, publish everything */
        do {
            extrace_prod_poll();
            EXTRACE_MARK(T_MARK);
            extrace_prod_flush();
            sched_yield();
            flags = extrace_port_irq_save();
            n = extrace_producer.lost_pending;
            extrace_port_irq_restore(flags);
        } while (n);
    }
    extrace_prod_flush();
    final_head = extrace_producer.published;
    final_epoch = extrace_producer.epoch;
    extrace_prod_stats(&final_stats);
    atomic_store(&prod_done, true);
    return nullptr;
}

static void on_record(struct extrace_sink *s, const struct extrace_rec *r)
{
    uint64_t v[4];
    int n;

    (void)s;
    if (r->event == EXTRACE_SYS_LOST) {
        if (extrace_rec_ints(r, v, 4, nullptr) >= 1)
            lost += v[0];
        return;
    }
    recs++;
    if (r->event == EXTRACE_EV_T_SEQ || r->event == EXTRACE_EV_T_ISR) {
        unsigned int c = r->event == EXTRACE_EV_T_ISR;

        n = extrace_rec_ints(r, v, 4, nullptr);
        if (n < 1) {
            bad_payload++;
            return;
        }
        if (seen[c] && (uint32_t)v[0] <= last[c])
            bad_order++;
        seen[c] = true;
        last[c] = (uint32_t)v[0];
    } else if (r->event == EXTRACE_EV_T_ANY) {
        uint32_t seq;

        if (r->len != 32) {
            bad_payload++;
            return;
        }
        memcpy(&seq, r->payload, 4);
        for (unsigned int k = 4; k < 32; k++)
            if (r->payload[k] != (uint8_t)(seq + k))
                bad_payload++;
    }
}

static const struct extrace_sink_ops ops = { .record = on_record };

static void run(unsigned long n)
{
    pthread_t irq;

    memset(blk, 0, sizeof(blk));
    memset(&cons, 0, sizeof(cons));
    memset(&extrace_producer, 0, sizeof(extrace_producer));
    memset(last, 0, sizeof(last));
    memset(seen, 0, sizeof(seen));
    recs = lost = 0;
    bad_order = bad_payload = 0;
    cancels = 0;
    isr_seq = 0;
    atomic_store(&prod_done, false);
    atomic_store(&stop_irq, false);
    extrace_disp_init(&disp);
    memset(&sink, 0, sizeof(sink));
    sink.ops = &ops;
    sink.cls = EXTRACE_SINK_RECORD;
    sink.kind_mask = 0xFF;
    extrace_sink_ring_all(&sink);
    extrace_disp_add_sink(&disp, &sink);
    extrace_cons_attach(&cons, blk, BLOCK, mirror, 8, 0);
    extrace_disp_add_ring(&disp, &cons);

    extrace_posix_set_isr(SIGUSR1, isr); /* before the first signal */
    pthread_create(&prod_th, nullptr, prod_thread, (void *)(uintptr_t)n);
    pthread_create(&irq, nullptr, irq_thread, nullptr);
    for (;;) {
        bool done = atomic_load(&prod_done);

        extrace_disp_poll(&disp, 2048);
        if (done && extrace_cons_attached(&cons) && cons.epoch == final_epoch &&
            cons.tail == final_head && extrace_disp_poll(&disp, 2048) == 0)
            break;
    }
    pthread_join(prod_th, nullptr);
    atomic_store(&stop_irq, true);
    pthread_join(irq, nullptr);

    EXPECT_EQ(bad_order, 0);
    EXPECT_EQ(bad_payload, 0);
    EXPECT_EQ(disp.ring[0].dec.errors + disp.stats.decode_errors, 0);
    EXPECT_EQ(recs + cancels, final_stats.records);
    EXPECT_EQ(lost, final_stats.lost_total);
    xt_log("records %llu lost %llu cancels %u isr runs %u\n", (unsigned long long)recs,
           (unsigned long long)lost, cancels, atomic_load(&isr_runs));
}

TEST(concurrency_producer_isr_consumer)
{
    for (unsigned long i = 0; i < xt_iters(3); i++)
        run(200000);
}
