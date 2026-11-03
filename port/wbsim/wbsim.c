// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
/*
 * wbsim.c - write-buffer simulator and step scheduler (DESIGN §18.3, §18.4).
 *
 * Memory model. Shared memory is an array of 32-bit words. Every core that uses
 * a block has a private view of it. A core's stores land in its view; the words
 * where its view differs from memory form its store buffer, and they drain to
 * memory in random order and random subsets. Words owned by another core are
 * refreshed in the view from memory, also in random subsets, so a load may return
 * an old value. wmb drains everything, rmb refreshes everything, mb does both,
 * and a doorbell drains everything. Word stores are single-copy atomic.
 *
 * Scheduling. Contexts are threads that run one at a time; a baton passes at
 * hooks. The choice of the next context, the drains and the refreshes all come
 * from one seeded generator, so a failing run is reproduced by its seed.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbsim.h"

#define P16(x) ((uint32_t)(x))

struct blk {
    uint32_t size, words, ctl_words;
    uint32_t *mem;
    uint32_t *view[WBSIM_MAX_CORES];
    uint32_t *seen[WBSIM_MAX_CORES]; /* foreign words: value last put into the view */
    uint32_t *ctl[WBSIM_MAX_CORES];  /* own control words: last value stored by st32 */
    uint64_t *sseq;                  /* st32 order of each word, for newest-first drains */
    uint8_t *own;                    /* owner core of each word */
    int prod, cons;
};

enum { CTX_NEW, CTX_RUN, CTX_DONE };

struct ctx {
    pthread_t th;
    pthread_cond_t cv;
    int core;
    enum wbsim_ctx_kind kind;
    void (*fn)(void *);
    void *arg;
    int state;
    bool in_handler;
    bool wants_enter;
};

struct core {
    int irq_owner;  /* context with interrupts disabled, or -1 */
    int isr_active; /* ISR context inside its handler, or -1 */
};

static struct blk blks[WBSIM_MAX_BLOCKS];
static int nblks;
static struct ctx ctxs[WBSIM_MAX_CTX];
static int nctx;
static struct core cores[WBSIM_MAX_CORES];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t main_cv = PTHREAD_COND_INITIALIZER;
static int current = -1;
static bool finished, deadlock;
static uint64_t rng;
static struct wbsim_params prm;
static struct wbsim_stats st;
static uint32_t ts_now;
static uint64_t store_seq;
static const struct wbsim_seg *script;
static unsigned int script_n, script_i;
static uint32_t script_steps;
static thread_local int tl_ctx = -1;
uint32_t wbsim_notify_count;

static const struct wbsim_params dflt = {
    .p_switch = P16(65536 / 8),
    .p_drain = P16(65536 / 4),
    .p_refresh = P16(65536 / 4),
    .p_ld_fresh = P16(65536 / 2),
    .ts_start = 1000,
    .ts_step_max = 3,
};

const struct wbsim_params *wbsim_default_params(void)
{
    return &dflt;
}

static uint64_t next_rand(void)
{
    uint64_t x = rng;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}

uint32_t wbsim_rand(uint32_t n)
{
    return n ? (uint32_t)(next_rand() % n) : 0;
}

static bool chance(uint32_t p16)
{
    return (uint32_t)(next_rand() >> 48) < p16;
}

void wbsim_reset(uint64_t seed, const struct wbsim_params *p)
{
    for (int b = 0; b < nblks; b++) {
        free(blks[b].mem);
        free(blks[b].own);
        free(blks[b].sseq);
        for (int c = 0; c < WBSIM_MAX_CORES; c++) {
            free(blks[b].view[c]);
            free(blks[b].seen[c]);
            free(blks[b].ctl[c]);
        }
    }
    memset(blks, 0, sizeof(blks));
    nblks = 0;
    memset(ctxs, 0, sizeof(ctxs));
    nctx = 0;
    for (int c = 0; c < WBSIM_MAX_CORES; c++)
        cores[c] = (struct core){ .irq_owner = -1, .isr_active = -1 };
    memset(&st, 0, sizeof(st));
    prm = p ? *p : dflt;
    rng = seed | 1u;
    ts_now = prm.ts_start;
    store_seq = 0;
    script = nullptr;
    script_n = script_i = 0;
    script_steps = 0;
    current = -1;
    finished = deadlock = false;
    wbsim_notify_count = 0;
}

int wbsim_block_add(uint32_t size, uint32_t data_off, int prod_core, int cons_core,
                    bool random_init)
{
    struct blk *b;
    uint32_t words = (size + 3u) / 4u;

    if (nblks >= WBSIM_MAX_BLOCKS)
        return -1;
    b = &blks[nblks];
    b->size = words * 4u;
    b->words = words;
    b->ctl_words = data_off / 4u;
    b->prod = prod_core;
    b->cons = cons_core;
    b->mem = aligned_alloc(64, (b->size + 63u) & ~63u);
    b->own = malloc(words);
    b->sseq = calloc(words, sizeof(uint64_t));
    for (uint32_t w = 0; w < words; w++) {
        /* pinfo + pstate and the data: producer; cstate + bitmaps: consumer (§6.1) */
        b->own[w] = (uint8_t)((w >= 24 && w < b->ctl_words) ? cons_core : prod_core);
        b->mem[w] = random_init ? (uint32_t)next_rand() : 0u;
    }
    for (int c = 0; c < WBSIM_MAX_CORES; c++) {
        if (c != prod_core && c != cons_core)
            continue;
        b->view[c] = aligned_alloc(64, (b->size + 63u) & ~63u);
        b->seen[c] = malloc(b->size);
        b->ctl[c] = malloc(b->size);
        memcpy(b->view[c], b->mem, b->size);
        memcpy(b->seen[c], b->mem, b->size);
        memcpy(b->ctl[c], b->mem, b->size);
    }
    return nblks++;
}

void *wbsim_block_view(int block, int core)
{
    return blks[block].view[core];
}

void *wbsim_block_mem(int block)
{
    return blks[block].mem;
}

const struct wbsim_stats *wbsim_get_stats(void)
{
    return &st;
}

/* ---- memory model ---- */

static bool locate(const void *p, struct blk **bp, int *core, uint32_t *w)
{
    uintptr_t a = (uintptr_t)p;

    for (int i = 0; i < nblks; i++) {
        for (int c = 0; c < WBSIM_MAX_CORES; c++) {
            uintptr_t v = (uintptr_t)blks[i].view[c];

            if (v && a >= v && a < v + blks[i].size) {
                *bp = &blks[i];
                *core = c;
                *w = (uint32_t)((a - v) / 4u);
                return true;
            }
        }
    }
    return false;
}

static int cur_core(void)
{
    return tl_ctx >= 0 ? ctxs[tl_ctx].core : -1;
}

/*
 * Drain the newest pending control stores of core c, leaving the older ones (and
 * the data) behind: exactly the reordering that a write barrier forbids.
 */
static void drain_newest(int c)
{
    for (int i = 0; i < nblks; i++) {
        struct blk *b = &blks[i];
        uint32_t *v = b->view[c], best = UINT32_MAX;

        if (!v)
            continue;
        for (uint32_t w = 0; w < b->ctl_words; w++)
            if (b->own[w] == c && v[w] != b->mem[w] && v[w] == b->ctl[c][w] &&
                (best == UINT32_MAX || b->sseq[w] > b->sseq[best]))
                best = w;
        if (best != UINT32_MAX) {
            b->mem[best] = v[best];
            st.drains++;
        }
    }
}

/* Drain stores of core c: everything, or a random subset. */
static void drain(int c, bool all)
{
    for (int i = 0; i < nblks; i++) {
        struct blk *b = &blks[i];
        uint32_t *v = b->view[c];

        if (!v)
            continue;
        for (uint32_t w = 0; w < b->words; w++) {
            if (b->own[w] != c || v[w] == b->mem[w])
                continue;
            if (w < b->ctl_words && v[w] != b->ctl[c][w]) {
                st.viol_plain_ctl++;
                fprintf(stderr, "wbsim: core %d changed control word %u without st32\n", c, w);
                b->ctl[c][w] = v[w];
            }
            if (all || (next_rand() & 1u)) {
                b->mem[w] = v[w];
                st.drains++;
            }
        }
    }
}

/* Refresh foreign words of core c: everything, or a random subset. */
static void refresh(int c, bool all)
{
    for (int i = 0; i < nblks; i++) {
        struct blk *b = &blks[i];
        uint32_t *v = b->view[c], *s = b->seen[c];

        if (!v)
            continue;
        for (uint32_t w = 0; w < b->words; w++) {
            if (b->own[w] == c)
                continue;
            if (v[w] != s[w]) {
                st.viol_foreign_store++;
                fprintf(stderr, "wbsim: core %d stored to word %u it does not own\n", c, w);
                v[w] = s[w];
            }
            if (v[w] != b->mem[w] && (all || (next_rand() & 1u))) {
                v[w] = s[w] = b->mem[w];
                st.refreshes++;
            }
        }
    }
}

/* ---- scheduler ---- */

static bool runnable(int i)
{
    const struct ctx *x = &ctxs[i];
    const struct core *k = &cores[x->core];

    if (x->state != CTX_RUN)
        return false;
    if (x->kind == WBSIM_THREAD)
        return k->isr_active < 0 && (k->irq_owner < 0 || k->irq_owner == i);
    if (x->in_handler)
        return true;
    if (x->wants_enter)
        return k->irq_owner < 0 && k->isr_active < 0;
    return true;
}

static int pick(int cur, bool force_other)
{
    int cand[WBSIM_MAX_CTX], n = 0;

    while (script && script_i < script_n) {
        const struct wbsim_seg *s = &script[script_i];

        if (script_steps < s->steps && runnable(s->ctx)) {
            script_steps++;
            return s->ctx;
        }
        script_i++;
        script_steps = 0;
    }
    for (int i = 0; i < nctx; i++)
        if (runnable(i) && !(force_other && i == cur))
            cand[n++] = i;
    if (!n)
        return (cur >= 0 && runnable(cur)) ? cur : -1;
    if (cur >= 0 && runnable(cur) && !force_other && !chance(prm.p_switch))
        return cur;
    return cand[wbsim_rand((uint32_t)n)];
}

/* Hand the baton to next and wait until it comes back; mtx held. */
static void switch_to(int next)
{
    int me = tl_ctx;

    if (next == me)
        return;
    st.switches++;
    current = next;
    pthread_cond_signal(&ctxs[next].cv);
    while (current != me)
        pthread_cond_wait(&ctxs[me].cv, &mtx);
}

static void sched_point(bool force_other)
{
    int next;

    if (tl_ctx < 0)
        return;
    pthread_mutex_lock(&mtx);
    next = pick(tl_ctx, force_other);
    if (next < 0) {
        deadlock = true;
        fprintf(stderr, "wbsim: deadlock\n");
        abort();
    }
    switch_to(next);
    pthread_mutex_unlock(&mtx);
}

/*
 * Every hook: random drains of any core (write buffers drain on their own, also
 * while their core is idle), random refreshes of the calling core, then maybe a
 * context switch.
 */
static void mem_step(int c)
{
    st.hooks++;
    if (chance(prm.p_drain)) {
        int k = (int)wbsim_rand(WBSIM_MAX_CORES);

        if (wbsim_rand(2))
            drain_newest(k);
        else
            drain(k, false);
    }
    if (chance(prm.p_refresh))
        refresh(c, false);
}

static void hook(void)
{
    int c = cur_core();

    if (c < 0)
        return;
    mem_step(c);
    sched_point(false);
}

void wbsim_yield(void)
{
    int c = cur_core();

    if (c < 0)
        return;
    mem_step(c);
    sched_point(true);
}

void wbsim_sync_core(void)
{
    int c = cur_core();

    if (c < 0)
        return;
    drain(c, true);
    refresh(c, true);
}

static void *ctx_main(void *arg)
{
    int me = (int)(intptr_t)arg, next;

    tl_ctx = me;
    pthread_mutex_lock(&mtx);
    while (current != me)
        pthread_cond_wait(&ctxs[me].cv, &mtx);
    pthread_mutex_unlock(&mtx);

    ctxs[me].fn(ctxs[me].arg);

    pthread_mutex_lock(&mtx);
    if (cores[ctxs[me].core].irq_owner == me)
        cores[ctxs[me].core].irq_owner = -1;
    ctxs[me].state = CTX_DONE;
    next = pick(me, true);
    if (next < 0) {
        bool all = true;

        for (int i = 0; i < nctx; i++)
            if (ctxs[i].state != CTX_DONE)
                all = false;
        if (!all) {
            deadlock = true;
            fprintf(stderr, "wbsim: deadlock at the end of context %d\n", me);
        }
        finished = true;
        current = -1;
        pthread_cond_signal(&main_cv);
    } else {
        current = next;
        pthread_cond_signal(&ctxs[next].cv);
    }
    pthread_mutex_unlock(&mtx);
    return nullptr;
}

int wbsim_ctx_add(int core, enum wbsim_ctx_kind kind, void (*fn)(void *), void *arg)
{
    struct ctx *x;

    if (nctx >= WBSIM_MAX_CTX || core < 0 || core >= WBSIM_MAX_CORES)
        return -1;
    x = &ctxs[nctx];
    x->core = core;
    x->kind = kind;
    x->fn = fn;
    x->arg = arg;
    x->state = CTX_NEW;
    pthread_cond_init(&x->cv, nullptr);
    return nctx++;
}

void wbsim_ctx_clear(void)
{
    memset(ctxs, 0, sizeof(ctxs));
    nctx = 0;
    for (int c = 0; c < WBSIM_MAX_CORES; c++)
        cores[c] = (struct core){ .irq_owner = -1, .isr_active = -1 };
    script = nullptr;
    script_n = script_i = 0;
    script_steps = 0;
    finished = deadlock = false;
}

void wbsim_script(const struct wbsim_seg *segs, unsigned int n)
{
    script = segs;
    script_n = n;
    script_i = 0;
    script_steps = 0;
}

int wbsim_run(void)
{
    int first;

    for (int i = 0; i < nctx; i++)
        ctxs[i].state = CTX_RUN;
    for (int i = 0; i < nctx; i++)
        pthread_create(&ctxs[i].th, nullptr, ctx_main, (void *)(intptr_t)i);
    pthread_mutex_lock(&mtx);
    first = pick(-1, false);
    current = first;
    if (first >= 0)
        pthread_cond_signal(&ctxs[first].cv);
    else
        finished = true;
    while (!finished)
        pthread_cond_wait(&main_cv, &mtx);
    pthread_mutex_unlock(&mtx);
    for (int i = 0; i < nctx; i++)
        pthread_join(ctxs[i].th, nullptr);
    for (int i = 0; i < nctx; i++)
        pthread_cond_destroy(&ctxs[i].cv);
    return deadlock ? -1 : 0;
}

int wbsim_cur_ctx(void)
{
    return tl_ctx;
}

int wbsim_cur_core(void)
{
    return cur_core();
}

void wbsim_isr_enter(void)
{
    struct ctx *x = &ctxs[tl_ctx];

    x->wants_enter = true;
    pthread_mutex_lock(&mtx);
    while (!runnable(tl_ctx) || current != tl_ctx) {
        int next = pick(tl_ctx, true);

        if (next < 0) {
            fprintf(stderr, "wbsim: deadlock entering an ISR\n");
            abort();
        }
        switch_to(next);
    }
    x->wants_enter = false;
    x->in_handler = true;
    cores[x->core].isr_active = tl_ctx;
    pthread_mutex_unlock(&mtx);
}

void wbsim_isr_exit(void)
{
    struct ctx *x = &ctxs[tl_ctx];

    x->in_handler = false;
    cores[x->core].isr_active = -1;
    hook();
}

bool wbsim_in_isr(void)
{
    return tl_ctx >= 0 && ctxs[tl_ctx].in_handler;
}

/* ---- port hooks ---- */

uint32_t wbsim_ts(void)
{
    hook();
    ts_now += 1u + wbsim_rand(prm.ts_step_max);
    return ts_now;
}

unsigned long wbsim_irq_save(void)
{
    int me = tl_ctx;
    struct core *k;
    unsigned long f;

    if (me < 0)
        return 0;
    k = &cores[ctxs[me].core];
    f = k->irq_owner == me ? 1u : 0u;
    k->irq_owner = me;
    hook();
    return f;
}

void wbsim_irq_restore(unsigned long flags)
{
    int me = tl_ctx;

    if (me < 0)
        return;
    if (!flags)
        cores[ctxs[me].core].irq_owner = -1;
    hook();
}

void wbsim_wmb(void)
{
    int c = cur_core();

    if (c >= 0)
        drain(c, true);
    hook();
}

void wbsim_rmb(void)
{
    int c = cur_core();

    if (c >= 0)
        refresh(c, true);
    hook();
}

void wbsim_mb(void)
{
    int c = cur_core();

    if (c >= 0) {
        drain(c, true);
        refresh(c, true);
    }
    hook();
}

static uint32_t load(const void *p, bool own_ok)
{
    struct blk *b;
    int vc, c;
    uint32_t w;

    hook();
    if (!locate(p, &b, &vc, &w))
        return *(const volatile uint32_t *)p;
    c = cur_core();
    if (c >= 0 && c != vc)
        fprintf(stderr, "wbsim: core %d loads through the view of core %d\n", c, vc);
    if (b->own[w] == vc) {
        if (!own_ok && w < b->ctl_words) {
            st.viol_own_load++;
            fprintf(stderr, "wbsim: core %d reads back its own control word %u\n", vc, w);
        }
    } else if (chance(prm.p_ld_fresh) && b->view[vc][w] == b->seen[vc][w]) {
        b->view[vc][w] = b->seen[vc][w] = b->mem[w];
    }
    return b->view[vc][w];
}

uint32_t wbsim_ld32(const void *p)
{
    return load(p, false);
}

uint32_t wbsim_ld32_own(const void *p)
{
    return load(p, true);
}

void wbsim_st32(void *p, uint32_t v)
{
    struct blk *b;
    int vc;
    uint32_t w;

    if (!locate(p, &b, &vc, &w)) {
        *(volatile uint32_t *)p = v;
        hook();
        return;
    }
    if (b->own[w] != vc) {
        st.viol_foreign_store++;
        fprintf(stderr, "wbsim: core %d st32 to word %u it does not own\n", vc, w);
    }
    b->view[vc][w] = v;
    b->sseq[w] = ++store_seq;
    if (w < b->ctl_words)
        b->ctl[vc][w] = v;
    hook();
}

void *wbsim_memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dd = d;
    const uint8_t *ss = s;
    struct blk *b;
    int vc;
    uint32_t w;
    bool shared = locate(d, &b, &vc, &w) || locate(s, &b, &vc, &w);

    if (!shared || tl_ctx < 0) {
        memcpy(d, s, n);
        return d;
    }
    while (n) { /* piecewise: other agents may run between the pieces */
        size_t k = 1u + wbsim_rand(16);

        if (k > n)
            k = n;
        hook();
        memcpy(dd, ss, k);
        dd += k;
        ss += k;
        n -= k;
    }
    return d;
}

void wbsim_notify(uint8_t ring_id)
{
    int c = cur_core();

    (void)ring_id;
    if (c >= 0)
        drain(c, true); /* completion barrier */
    st.notifies++;
    wbsim_notify_count++;
    hook();
}
