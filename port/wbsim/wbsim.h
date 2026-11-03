/* SPDX-License-Identifier: MIT */
/*
 * wbsim.h - write-buffer simulator and step scheduler (DESIGN §18.3, §18.4).
 *
 * Each simulated core has a private view of every registered block. Stores go to
 * the view and drain to shared memory word by word, in random order and at
 * random points; foreign words of a view are refreshed from shared memory at
 * random points. Barriers drain (wmb), refresh (rmb) or both (mb), and a
 * doorbell drains completely. Missing or misplaced barriers therefore show up
 * as stale or reordered values. The simulator also flags a store to a word the
 * core does not own, a plain (non-st32) store to a control word, and a load of
 * an own control word outside the declared exceptions (§6.3).
 *
 * Contexts (threads, ISRs) run one at a time under a seeded scheduler that may
 * switch at every shared-memory access, so runs are reproducible from the seed.
 */
#ifndef WBSIM_H
#define WBSIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WBSIM_MAX_CTX 8
#define WBSIM_MAX_CORES 4
#define WBSIM_MAX_BLOCKS 4

enum wbsim_ctx_kind { WBSIM_THREAD, WBSIM_ISR };

struct wbsim_params {
    uint32_t p_switch;      /* 1/65536 units: chance to switch context at a hook */
    uint32_t p_drain;       /* chance that a hook drains some pending stores */
    uint32_t p_refresh;     /* chance that a hook refreshes some foreign words */
    uint32_t p_ld_fresh;    /* chance that a foreign control-word load sees memory */
    uint32_t ts_start;      /* first timer value */
    uint32_t ts_step_max;   /* the timer advances 1..ts_step_max per read */
};

struct wbsim_stats {
    uint64_t hooks;
    uint64_t switches;
    uint64_t drains;
    uint64_t refreshes;
    uint64_t notifies;
    uint32_t viol_foreign_store; /* store to a word owned by another core */
    uint32_t viol_plain_ctl;     /* control word changed without st32 */
    uint32_t viol_own_load;      /* load of an own control word (rule 8) */
};

/* Scripted schedule: run ctx for steps hooks, then the next segment. */
struct wbsim_seg {
    int ctx;
    uint32_t steps;
};

void wbsim_reset(uint64_t seed, const struct wbsim_params *p);
const struct wbsim_params *wbsim_default_params(void);

/*
 * Register a ring block of size bytes whose producer runs on prod_core and whose
 * consumer runs on cons_core. data_off is where the producer-owned data starts.
 * Memory and views start random (random_init) or zero.
 */
int wbsim_block_add(uint32_t size, uint32_t data_off, int prod_core, int cons_core,
                    bool random_init);
void *wbsim_block_view(int block, int core);
void *wbsim_block_mem(int block);

int wbsim_ctx_add(int core, enum wbsim_ctx_kind kind, void (*fn)(void *), void *arg);
/* forget all contexts, keep memory: the next run starts a new phase */
void wbsim_ctx_clear(void);
/* optional script, then random scheduling */
void wbsim_script(const struct wbsim_seg *segs, unsigned int n);
/* run all contexts to completion; returns 0, or -1 on deadlock */
int wbsim_run(void);

/* Called by context code */
void wbsim_isr_enter(void);
void wbsim_isr_exit(void);
void wbsim_yield(void);
int wbsim_cur_ctx(void);
int wbsim_cur_core(void);
uint32_t wbsim_rand(uint32_t n);    /* scheduler RNG, for context code */
/* drain or refresh everything of the calling core (outside the port contract) */
void wbsim_sync_core(void);

const struct wbsim_stats *wbsim_get_stats(void);

/* Port hooks */
uint32_t wbsim_ts(void);
unsigned long wbsim_irq_save(void);
void wbsim_irq_restore(unsigned long flags);
void wbsim_wmb(void);
void wbsim_rmb(void);
void wbsim_mb(void);
uint32_t wbsim_ld32(const void *p);
uint32_t wbsim_ld32_own(const void *p);
void wbsim_st32(void *p, uint32_t v);
void *wbsim_memcpy(void *d, const void *s, size_t n);
void wbsim_notify(uint8_t ring_id);
bool wbsim_in_isr(void);
extern uint32_t wbsim_notify_count;

#endif /* WBSIM_H */
