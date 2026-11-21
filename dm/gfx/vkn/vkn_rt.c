// [DOS2] VK-NATIVE stage 5.5: the backend's own recording thread (vkn_rt.h: the contract, the modes, the snapshot rules). No game
// addresses, no engine record offsets.
// The stream: chunks of 256 KiB (a bigger record gets a chunk of its own), records 16-byte aligned with an 8-byte header, never across
// a chunk (a NEXT record ends a chunk). One producer (the encoder) and one consumer (the worker): the producer publishes a record count
// (release), the consumer executes up to it and publishes its own count (release); a chunk read to its end goes back to a free list.
// Announces of constant blocks and slot copies are not published on their own: the next draw / call / frame record carries them (a
// sync publishes everything); consecutive announces grow one record.
// Waiting: the worker spins a short while (sched_yield) when it runs dry, then sleeps on a condition variable ("sleeping" flag,
// Dekker-safe with the producer's publish); the encoder waits on another one for "done" (sync) and "frame done" (backpressure).
// Ownership: after a sync the encoder owns the recording (owner_engine = 1) until it publishes its next record from outside an owned
// section; the worker never runs while nothing is published, so the flag only names the thread (vkn_rt_rec_thread_is).
// Inline execution (mode 9, an owned section): the record is built in a per-thread scratch buffer and executed right there.
#ifdef DM_GAME_DOS2
#include "vkn_rt.h"
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "vkn_buf.h"
#include "vkn_frame.h"
#include "vkn_pass.h"
#include "vkn_prof.h"
#include "vkn_query.h"
#include "vkn_state.h"
#include "vkn_ui.h"
_Static_assert(VKN_PASS_COLORS_RT == VKN_PASS_COLORS, "vkn_rt.h colour count");

#define RT_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark as a literal of its own
enum { R_FOREIGN, R_MISPREDICT, R_OOM, R_WORKER, R_RING, R_SLOTS, R_FRAME, R_N };
static uint32_t g_red[R_N];
#define RT_REDLN(r, ...) do { if (__atomic_add_fetch(&g_red[r], 1, __ATOMIC_RELAXED) <= 16) vkn_logf(RT_RED "rt: " __VA_ARGS__); } while (0)

// what vkn_draw.c / vkn_frame.c / vkn_buf.c offer this module (stage 5.5 blocks of their headers)
extern const void *vkn_draw_rt_snap; extern const vkn_draw_in *vkn_draw_rt_in;   // vkn_draw.c: the engine's indices of the draw executed now
int vkn_rec_end_due(void);                     // vkn_frame.c
uint64_t vkn_frame_gpu_done(int *any);         // vkn_frame.c: newest presented recording the GPU finished (its vkn_frame())
void vkn_frame_wait_frame(uint64_t f);         // vkn_frame.c (the recording's owner): wait until the recording of frame f finished
uint64_t vkn_ring_cum(void);                   // vkn_buf.c: ring bytes handed out so far

// ================================================================ state
static int g_mode, g_slots = VKN_RT_SLOTS_CMP;
int vkn_rt_live_;
static int g_threaded;
static pthread_t g_enc, g_wk;
static int g_wk_run;
static __thread int t_dec;          // records being executed by this thread (depth)
static __thread int t_own;          // owned sections of this thread (depth)
static __thread int t_wk;           // this is the worker
static int g_owner_engine;          // after a sync: the encoder owns the recording (atomic)
static void (*g_yield)(int);
static uint64_t g_ring_limit;

static vkn_rt_stats g_st;           // ops / bytes / slots / encode-side: the encoder; idle / busy / frames: the worker (atomics)
#define ST_ADD(f, v) __atomic_add_fetch(&g_st.f, (v), __ATOMIC_RELAXED)
static vkn_rt_site *g_sites; static pthread_mutex_t g_site_mu = PTHREAD_MUTEX_INITIALIZER;
static vkn_rt_site g_site_ring = VKN_RT_SITE("ring guard"),
                   g_site_off = VKN_RT_SITE("off-frame end"), g_site_rebase = VKN_RT_SITE("frame rebase"), g_site_oom = VKN_RT_SITE("stream full");

// ================================================================ the stream
enum { CHUNK = 256 << 10, HDR = 8, SLOT_ARR = 4 };
enum { K_NEXT = 1, K_DRAW, K_DISPATCH, K_UBO, K_SLOTS, K_CALL, K_BEGIN, K_END, K_TAIL };
typedef struct rt_hdr { uint32_t size; uint16_t kind; uint16_t n; } rt_hdr;   // size: whole record, 16-byte multiple
typedef struct rt_chunk { struct rt_chunk *next, *free_next; uint32_t cap; uint32_t pad; uint8_t data[]; } rt_chunk;
static rt_chunk *g_wc; static uint32_t g_wo;          // producer: chunk, offset
static rt_chunk *g_rc; static uint32_t g_ro;          // consumer
static uint64_t g_written, g_published, g_done_local; // producer counts / consumer's own
static uint64_t g_pushed, g_done;                     // atomics: published by the producer / executed by the consumer
static rt_chunk *g_free; static uint32_t g_nfree; static pthread_mutex_t g_free_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv_work = PTHREAD_COND_INITIALIZER, g_cv_done = PTHREAD_COND_INITIALIZER;
static int g_sleeping, g_stop;                        // atomics
static uint64_t g_wait_done;                          // atomic: the encoder waits for g_done >= this (0 = nobody)
static uint64_t g_enc_tails, g_wk_tails, g_wait_tails;   // frame tails written / executed; backpressure target (atomics)
static uint64_t g_since_sync;                         // records written since the last sync (mode 9: what a sync would have waited for)

static rt_chunk *chunk_get(uint32_t need) {
    rt_chunk *c = NULL;
    if (need <= CHUNK) {
        pthread_mutex_lock(&g_free_mu);
        if (g_free) { c = g_free; g_free = c->free_next; g_nfree--; }
        pthread_mutex_unlock(&g_free_mu);
        if (!c) { c = (rt_chunk *)malloc(sizeof *c + CHUNK); if (c) c->cap = CHUNK; }
    } else { c = (rt_chunk *)malloc(sizeof *c + need); if (c) c->cap = need; }
    if (c) { c->next = NULL; c->free_next = NULL; }
    return c;
}
static void chunk_put(rt_chunk *c) {
    if (c->cap != CHUNK) { free(c); return; }
    pthread_mutex_lock(&g_free_mu);
    if (g_nfree < 32) { c->free_next = g_free; g_free = c; g_nfree++; c = NULL; }
    pthread_mutex_unlock(&g_free_mu);
    free(c);
}

// ---- per-thread scratch for records executed at once ----
static __thread uint8_t *t_scr; static __thread uint32_t t_scr_cap;
static void *scratch(uint32_t bytes) {
    if (bytes > t_scr_cap) {
        uint32_t c = t_scr_cap ? t_scr_cap : 4096; while (c < bytes) c *= 2;
        void *p = realloc(t_scr, c); if (!p) return NULL;
        t_scr = (uint8_t *)p; t_scr_cap = c;
    }
    return t_scr;
}

// ================================================================ execution of a record
static void exec(rt_hdr *h);
static void publish(void) {
    if (g_written == g_published) return;
    g_published = g_written;
    __atomic_store_n(&g_pushed, g_written, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_sleeping, __ATOMIC_SEQ_CST)) { pthread_mutex_lock(&g_mu); pthread_cond_signal(&g_cv_work); pthread_mutex_unlock(&g_mu); }
}

// Where the next record goes: 1 = executed at once on this thread (mode 9, an owned section, another thread), 0 = the stream.
static int foreign_wait(void);
static int inline_here(void) {
    if (!g_threaded) return 1;
    if (!pthread_equal(pthread_self(), g_enc)) {   // another thread records: not expected (the backend is single-threaded); exact but racy
        ST_ADD(foreign, 1);
        RT_REDLN(R_FOREIGN, "a record from a thread that is not the engine thread (inside a hook of another thread?): executed there after the worker went idle");
        foreign_wait();
        return 1;
    }
    return t_own > 0;
}

// One record of `bytes` (header included). *inl = executed at once (built in scratch). NULL = no memory (the caller executes directly).
static int g_last_ubo_ok; static uint64_t g_last_ubo_idx; static rt_hdr *g_last_ubo;
static rt_hdr *rec_new_at(uint16_t kind, uint32_t bytes, int inl) {
    bytes = (bytes + 15u) & ~15u;
    rt_hdr *h;
    if (inl) {
        h = (rt_hdr *)scratch(bytes); if (!h) return NULL;
        if (!g_threaded && !t_own) g_since_sync++;   // mode 9: what a sync of mode 1 would wait for
    }
    else {
        if (g_wc && g_wo + bytes + 16 > g_wc->cap) {   // this chunk is full: a NEXT record, a new chunk
            rt_chunk *n = chunk_get(bytes + 16 > CHUNK ? bytes + 16 : CHUNK);
            if (!n) return NULL;
            rt_hdr *x = (rt_hdr *)(g_wc->data + g_wo); x->size = 16; x->kind = K_NEXT; x->n = 0;
            g_wc->next = n; g_wc = n; g_wo = 0; g_written++;
            ST_ADD(ops[VKN_RT_OP_NEXT], 1);
        }
        if (!g_wc) return NULL;
        h = (rt_hdr *)(g_wc->data + g_wo); g_wo += bytes;
        g_written++; g_since_sync++;
        ST_ADD(bytes, bytes);
    }
    h->size = bytes; h->kind = kind; h->n = 0;
    g_last_ubo_ok = 0;
    return h;
}
static rt_hdr *rec_new(uint16_t kind, uint32_t bytes, int *inl) { *inl = inline_here(); return rec_new_at(kind, bytes, *inl); }
// The record is complete: published (pub) or left for the next publishing record; executed now when inline.
static void rec_done(rt_hdr *h, int inl, int pub) {
    if (inl) { ST_ADD(inline_ops, 1); t_dec++; exec(h); t_dec--; return; }
    if (g_yield) g_yield(0);
    if (pub) {
        if (__atomic_load_n(&g_owner_engine, __ATOMIC_RELAXED)) __atomic_store_n(&g_owner_engine, 0, __ATOMIC_RELEASE);
        publish();
    }
}

// ================================================================ the encoder's side state
static uint64_t g_enc_frame; static int g_enc_due, g_rebase;   // vkn_frame() as the recording will have it; a mismatch seen by the worker
static uint32_t g_ubo_gen; static struct { uint32_t off, size; uint8_t valid; } g_eubo[VKN_STAGE_SLOTS][VKN_UBO_SLOTS];
static uint64_t g_cond; static int g_cond_on;
static int32_t g_eslot[SLOT_ARR][VKN_TEX_SLOTS]; static uint8_t g_eslot_mask; static int g_eslot_ok; static uint32_t g_slot_gen, g_eslot_gen;
static uint64_t g_ring_start[16];                               // vkn_ring_cum() at the begin of encoder frame f (index f & 15)
static uint64_t g_cr_cum[VKN_RT_CPU_RINGS], g_cr_start[VKN_RT_CPU_RINGS][16];   // [stage 5.2 audit] the engine's CPU-written rings (vkn_rt_cpu_ring)
static uint32_t g_enc_serial;                                   // vkn_rt_cb_serial: + 1 at every recording begin written
static uint64_t g_vf_h; static const vkn_vfmt *g_vf;            // the last vertex format looked up (formats are never destroyed)

// ---- the worker's side state ----
static int32_t g_dslot[SLOT_ARR][VKN_TEX_SLOTS]; static uint8_t g_dslot_mask;

int vkn_rt_mode(void) { return g_mode; }
int vkn_rt_threaded(void) { return g_threaded; }
int vkn_rt_decoding(void) { return t_dec > 0; }
int vkn_rt_encoder_here(void) { return vkn_rt_live_ && pthread_equal(pthread_self(), g_enc); }
int vkn_rt_tex_rec(void) { return vkn_rt_live_ && !t_dec && pthread_equal(pthread_self(), g_enc) && (!g_threaded || !t_own); }   // vkn_tex.c
int vkn_rt_rec_thread_is(void) {
    if (!g_threaded) return -1;
    const pthread_t o = __atomic_load_n(&g_owner_engine, __ATOMIC_ACQUIRE) ? g_enc : g_wk;
    return pthread_equal(pthread_self(), o) ? 1 : 0;
}
uint32_t vkn_rt_cb_serial(void) { return vkn_rt_live_ ? g_enc_serial : vkn_cb_serial(); }
uint64_t vkn_rt_frame(void) {
    if (g_threaded && !t_wk && pthread_equal(pthread_self(), g_enc)) return g_enc_frame;
    return vkn_frame();
}

// ================================================================ the worker
static void *worker_main(void *arg) {
    (void)arg;
    t_wk = 1;
#if defined(__ANDROID__) || defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "vkn-record");   // (the thread lists of the CPU placement / ADPF scans and the profilers name it)
#endif
    for (;;) {
        uint64_t avail = __atomic_load_n(&g_pushed, __ATOMIC_ACQUIRE);
        if (g_done_local == avail) {
            const uint64_t t0 = vkn_now_ns();
            for (int spin = 0; spin < 64 && g_done_local == avail; spin++) { sched_yield(); avail = __atomic_load_n(&g_pushed, __ATOMIC_ACQUIRE); }
            if (g_done_local == avail) {
                pthread_mutex_lock(&g_mu);
                for (;;) {
                    __atomic_store_n(&g_sleeping, 1, __ATOMIC_SEQ_CST);
                    avail = __atomic_load_n(&g_pushed, __ATOMIC_SEQ_CST);
                    if (g_done_local != avail || __atomic_load_n(&g_stop, __ATOMIC_SEQ_CST)) break;
                    pthread_cond_wait(&g_cv_work, &g_mu);
                }
                __atomic_store_n(&g_sleeping, 0, __ATOMIC_RELAXED);
                pthread_mutex_unlock(&g_mu);
            }
            ST_ADD(idle_ns, vkn_now_ns() - t0);
            if (g_done_local == avail && __atomic_load_n(&g_stop, __ATOMIC_SEQ_CST)) break;
            continue;
        }
        const uint64_t t1 = vkn_now_ns();
        while (g_done_local < avail) {
            rt_hdr *h = (rt_hdr *)(g_rc->data + g_ro);
            if (h->kind == K_NEXT) { rt_chunk *c = g_rc; g_rc = c->next; g_ro = 0; chunk_put(c); }
            else { if (g_yield) g_yield(1); t_dec++; exec(h); t_dec--; g_ro += h->size; }
            g_done_local++;
            __atomic_store_n(&g_done, g_done_local, __ATOMIC_SEQ_CST);
            const uint64_t w = __atomic_load_n(&g_wait_done, __ATOMIC_SEQ_CST);
            if (w && g_done_local >= w) { pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv_done); pthread_mutex_unlock(&g_mu); }
        }
        ST_ADD(busy_ns, vkn_now_ns() - t1);
    }
    return NULL;
}
static int worker_start(void) {
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);   // Vulkan pipeline compiles: as vkn_shader.c's worker
    g_stop = 0;
    const int r = pthread_create(&g_wk, &a, worker_main, NULL);
    pthread_attr_destroy(&a);
    if (r) { RT_REDLN(R_WORKER, "no recording thread (pthread_create %d): records are executed on the engine thread (mode 9 behaviour)", r); return -1; }
    g_wk_run = 1;
    return 0;
}

// ================================================================ sync
// Waits until the consumer executed everything published so far (the encoder publishes first). The caller is the encoder.
static uint64_t wait_done(void) {
    publish();
    const uint64_t target = g_written;
    if (__atomic_load_n(&g_done, __ATOMIC_SEQ_CST) >= target) return 0;
    const uint64_t t0 = vkn_now_ns();
    pthread_mutex_lock(&g_mu);
    __atomic_store_n(&g_wait_done, target, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_done, __ATOMIC_SEQ_CST) < target) pthread_cond_wait(&g_cv_done, &g_mu);
    __atomic_store_n(&g_wait_done, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_mu);
    return vkn_now_ns() - t0 + 1;
}
static void retire_pending_run(int all);
static void rebase(void) {   // the worker is idle: the frame counter as it is
    g_enc_frame = vkn_frame(); g_enc_due = vkn_rec_end_due(); g_rebase = 0;
}
static void site_reg(vkn_rt_site *s) {
    if (s->reg) return;
    pthread_mutex_lock(&g_site_mu);
    if (!s->reg) { s->next = g_sites; g_sites = s; s->reg = 1; }
    pthread_mutex_unlock(&g_site_mu);
}
void vkn_rt_sync(vkn_rt_site *s) {
    if (!vkn_rt_live_ || !pthread_equal(pthread_self(), g_enc)) return;   // loaders: no sync (they ran beside the recording before too)
    if (s) { site_reg(s); __atomic_add_fetch(&s->calls, 1, __ATOMIC_RELAXED); }
    if (!g_threaded) {   // mode 9: nothing waits; a sync counts when records were written since the last one (what mode 1 would wait for)
        if (g_since_sync) { g_since_sync = 0; if (s) __atomic_add_fetch(&s->syncs, 1, __ATOMIC_RELAXED); ST_ADD(syncs, 1); }
        return;
    }
    if (t_own > 0 && __atomic_load_n(&g_owner_engine, __ATOMIC_RELAXED)) return;   // owned already: nothing was published since
    const int had = g_since_sync != 0 || __atomic_load_n(&g_done, __ATOMIC_SEQ_CST) != g_written;
    const uint64_t w = wait_done();
    g_since_sync = 0;
    __atomic_store_n(&g_owner_engine, 1, __ATOMIC_RELEASE);
    rebase();
    retire_pending_run(1);
    if (had && s) { __atomic_add_fetch(&s->syncs, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&s->wait_ns, w, __ATOMIC_RELAXED); }
    if (had) { ST_ADD(syncs, 1); ST_ADD(sync_wait_ns, w); }
}
void vkn_rt_own_begin(vkn_rt_site *s) {
    vkn_rt_sync(s);
    t_own++;
    if (g_cond_on && vkn_rt_live_ && pthread_equal(pthread_self(), g_enc)) { g_cond_on = 0; vkn_query_cond(g_cond); }   // a condition not yet carried: in order now
}
void vkn_rt_own_end(void) { if (t_own > 0) t_own--; }
static int foreign_wait(void) {   // another thread executes at once: the worker idle first (not exact against a concurrent encoder)
    while (__atomic_load_n(&g_done, __ATOMIC_SEQ_CST) != __atomic_load_n(&g_pushed, __ATOMIC_SEQ_CST)) sched_yield();
    return 0;
}

// ================================================================ install / bind
void vkn_rt_install(int mode, int slots) {
    if (mode != VKN_RT_AHEAD1 && mode != VKN_RT_AHEAD2 && mode != VKN_RT_VERIFY) mode = VKN_RT_OFF;
    g_mode = mode; g_slots = slots >= 0 && slots <= 3 ? slots : VKN_RT_SLOTS_CMP;
}
static void bind_encoder(void) {
    g_enc = pthread_self();
    if (!g_wc) { g_wc = g_rc = chunk_get(CHUNK); g_wo = g_ro = 0; }
    if (!g_wc) { RT_REDLN(R_OOM, "no memory for the stream: DM_VK_NATIVE_RT off"); g_mode = VKN_RT_OFF; return; }
    rebase();
    g_threaded = 0;
    if (g_mode == VKN_RT_AHEAD1 || g_mode == VKN_RT_AHEAD2) g_threaded = !g_wk_run ? worker_start() == 0 : 1;
    __atomic_store_n(&g_owner_engine, 1, __ATOMIC_RELEASE);
    vkn_rt_live_ = 1;
    vkn_logf("rt: DM_VK_NATIVE_RT=%d: %s; slot arrays %s", g_mode,
             g_mode == VKN_RT_VERIFY ? "every record executed at once on the engine thread (verify)" : g_threaded ? (g_mode == 1 ? "recording thread, engine at most 1 frame ahead" : "recording thread, engine at most 2 frames ahead") : "no recording thread",
             g_slots == 0 ? "copied at every draw" : g_slots == 1 ? "copied on change (compare)" : g_slots == 2 ? "copied on change (the engine's dirty ranges)" : "dirty ranges checked by compare");
}
void vkn_rt_shutdown(void) {
    if (g_threaded) {
        vkn_rt_sync(NULL);
        pthread_mutex_lock(&g_mu); __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST); pthread_cond_signal(&g_cv_work); pthread_mutex_unlock(&g_mu);
        pthread_join(g_wk, NULL); g_wk_run = 0;
    }
    retire_pending_run(1);
    vkn_rt_live_ = 0; g_threaded = 0; g_eslot_ok = 0; g_dslot_mask = 0; g_cond_on = 0;
    __atomic_store_n(&g_owner_engine, 0, __ATOMIC_RELEASE);
}
void vkn_rt_test_yield(void (*fn)(int where)) { g_yield = fn; }
void vkn_rt_test_ring_limit(uint64_t bytes) { g_ring_limit = bytes; }

// ================================================================ records
// ---- closures ----
typedef struct op_call { rt_hdr h; uint32_t bytes, pad; void (*fn)(void *); } op_call;   // payload after (16-aligned)
#define CALL_PAY(c) ((uint8_t *)(c) + ((sizeof(op_call) + 15u) & ~15u))
void vkn_rt_call(void (*fn)(void *arg), const void *arg, uint32_t bytes) {
    if (!vkn_rt_live_) { fn((void *)arg); return; }
    int inl; op_call *c = (op_call *)rec_new(K_CALL, (uint32_t)((sizeof(op_call) + 15u) & ~15u) + bytes, &inl);
    if (!c) { ST_ADD(oom, 1); RT_REDLN(R_OOM, "stream: no memory: a call executed after a sync"); vkn_rt_own_begin(&g_site_oom); fn((void *)arg); vkn_rt_own_end(); return; }
    c->fn = fn; c->bytes = bytes; if (bytes) memcpy(CALL_PAY(c), arg, bytes);
    ST_ADD(ops[VKN_RT_OP_CALL], 1);
    rec_done(&c->h, inl, 1);
}
static void retire_block_fn(void *p) { vkn_retire_block((const vkn_block *)p); }
typedef struct { void (*fn)(void *); void *ctx; } retire_job;
static void retire_fn(void *p) { const retire_job *j = (const retire_job *)p; vkn_retire(j->fn, j->ctx); }
// Retirements from other threads while records are in flight: kept with the record count written at the call; run by the worker once
// it executed that many (at the next frame tail) or at the next sync - never earlier than at the same point of the recording.
typedef struct pend_ret { void (*fn)(void *); void *ctx; uint64_t at; struct pend_ret *next; } pend_ret;
static pend_ret *g_pret; static pthread_mutex_t g_pret_mu = PTHREAD_MUTEX_INITIALIZER; static int g_pret_n;
static void retire_pending_run(int all) {
    if (!__atomic_load_n(&g_pret_n, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&g_pret_mu);
    pend_ret **pp = &g_pret, *run = NULL;
    const uint64_t done = __atomic_load_n(&g_done, __ATOMIC_ACQUIRE);
    while (*pp) { pend_ret *p = *pp; if (all || p->at <= done) { *pp = p->next; p->next = run; run = p; g_pret_n--; } else pp = &p->next; }
    pthread_mutex_unlock(&g_pret_mu);
    while (run) { pend_ret *n = run->next; vkn_retire(run->fn, run->ctx); free(run); run = n; }
}
void vkn_rt_retire(void (*fn)(void *ctx), void *ctx) {
    if (!vkn_rt_live_ || !g_threaded || t_dec || vkn_rt_rec_thread_is() == 1) { vkn_retire(fn, ctx); return; }
    if (pthread_equal(pthread_self(), g_enc)) { const retire_job j = {fn, ctx}; vkn_rt_call(retire_fn, &j, sizeof j); return; }
    pend_ret *p = (pend_ret *)malloc(sizeof *p);
    if (!p) { vkn_retire(fn, ctx); return; }
    p->fn = fn; p->ctx = ctx; p->at = __atomic_load_n(&g_pushed, __ATOMIC_ACQUIRE);
    pthread_mutex_lock(&g_pret_mu); p->next = g_pret; g_pret = p; g_pret_n++; pthread_mutex_unlock(&g_pret_mu);
}
static void retire_blk_free(void *ctx) { vkn_block_free((vkn_block *)ctx); free(ctx); }
void vkn_rt_retire_block(const vkn_block *b) {
    if (!b || !b->size) return;
    if (!vkn_rt_live_ || !g_threaded || t_dec || vkn_rt_rec_thread_is() == 1) { vkn_retire_block(b); return; }
    if (pthread_equal(pthread_self(), g_enc)) { vkn_rt_call(retire_block_fn, b, sizeof *b); return; }
    vkn_block *c = (vkn_block *)malloc(sizeof *c);
    if (!c) { vkn_retire_block(b); return; }
    *c = *b; vkn_rt_retire(retire_blk_free, c);
}

// ---- constant blocks ----
typedef struct ubo_e { uint8_t stage, slot, pad[2]; uint32_t off, size, pad2; } ubo_e;   // 16 bytes
typedef struct op_ubo { rt_hdr h; uint32_t pad[2]; } op_ubo;                              // entries after (16-aligned)
static void ubo_bad_fn(void *p) { const int32_t *a = (const int32_t *)p; vkn_draw_ubo(a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3]); }
uint32_t vkn_rt_ubo_gen(void) { return vkn_rt_live_ ? g_ubo_gen : vkn_draw_ubo_gen(); }
void vkn_rt_ubo(int stage, uint32_t slot, uint32_t off, uint32_t size) {
    if (!vkn_rt_live_) { vkn_draw_ubo(stage, slot, off, size); return; }
    if (stage >= 0 && stage < VKN_STAGE_SLOTS && slot < VKN_UBO_SLOTS) {   // vkn_draw_ubo's generation rule (out of range: ignored there)
        if (!g_eubo[stage][slot].valid || g_eubo[stage][slot].off != off || g_eubo[stage][slot].size != size) g_ubo_gen++;
        g_eubo[stage][slot].off = off; g_eubo[stage][slot].size = size; g_eubo[stage][slot].valid = 1;
    }
    ST_ADD(ops[VKN_RT_OP_UBO], 1);
    const ubo_e e = {(uint8_t)stage, (uint8_t)(slot < 256 ? slot : 255), {0, 0}, off, size, 0};
    if (stage < 0 || stage > 255 || slot > 254) { const int32_t a[4] = {stage, (int32_t)slot, (int32_t)off, (int32_t)size}; vkn_rt_call(ubo_bad_fn, a, sizeof a); return; }   // (counted and logged there)
    const int inl0 = inline_here();
    // the previous record is an unpublished announce at the end of this chunk: one more entry
    if (!inl0 && g_last_ubo_ok && g_last_ubo && g_last_ubo_idx + 1 == g_written && g_last_ubo_idx >= g_published &&
        (uint8_t *)g_last_ubo + g_last_ubo->size == g_wc->data + g_wo && g_wo + 16 + 16 <= g_wc->cap && g_last_ubo->n < 0xffff) {
        memcpy((uint8_t *)g_last_ubo + g_last_ubo->size, &e, sizeof e);
        g_last_ubo->size += 16; g_last_ubo->n++; g_wo += 16; ST_ADD(bytes, 16);
        return;
    }
    op_ubo *u = (op_ubo *)rec_new_at(K_UBO, sizeof(op_ubo) + sizeof e, inl0);
    if (!u) { vkn_rt_own_begin(&g_site_oom); vkn_draw_ubo(stage, slot, off, size); vkn_rt_own_end(); return; }
    u->h.n = 1; memcpy(u + 1, &e, sizeof e);
    rec_done(&u->h, inl0, 0);
    if (!inl0) { g_last_ubo = &u->h; g_last_ubo_idx = g_written - 1; g_last_ubo_ok = 1; }
}

// ---- the condition of the next draw ----
void vkn_rt_query_cond(uint64_t key) {
    if (!vkn_rt_live_ || inline_here()) { vkn_query_cond(key); return; }   // (an owned section: the draw that follows is executed at once too)
    g_cond = key; g_cond_on = 1;
}

// ---- slot arrays: stages 0 / 1, textures then samplers ----
typedef struct op_slots { rt_hdr h; uint8_t mask, pad[7]; int32_t a[SLOT_ARR][VKN_TEX_SLOTS]; } op_slots;
void vkn_rt_slots_touch(void) { if (vkn_rt_live_) g_slot_gen++; }
static const int32_t *arr_of(const vkn_draw_in *in, int k) { return k < 2 ? in->tex[k] : in->smp[k - 2]; }
// Writes a slot record when what the draw names differs from the last one written; 0 = ok, -1 = no memory.
static int slots_enc(const vkn_draw_in *in) {
    uint8_t mask = 0;
    for (int k = 0; k < SLOT_ARR; k++) if (arr_of(in, k)) mask |= (uint8_t)(1u << k);
    int same = g_eslot_ok && mask == g_eslot_mask;
    if (same && g_slots != VKN_RT_SLOTS_COPY) {
        if (g_slots == VKN_RT_SLOTS_CMP || g_slots == VKN_RT_SLOTS_GEN_VERIFY) {
            int eq = 1;
            for (int k = 0; eq && k < SLOT_ARR; k++) if (mask >> k & 1) eq = !memcmp(arr_of(in, k), g_eslot[k], sizeof g_eslot[k]);
            if (g_slots == VKN_RT_SLOTS_GEN_VERIFY && !eq && g_eslot_gen == g_slot_gen) {   // (a dirty range without a change only costs a copy)
                ST_ADD(slot_gen_miss, 1);
                RT_REDLN(R_SLOTS, "slot arrays changed without a dirty range (DM_VK_NATIVE_RT_SLOTS=3): the compare wins");
            }
            same = eq;
        } else same = g_eslot_gen == g_slot_gen;   // VKN_RT_SLOTS_GEN: trust the engine's ranges
    } else if (g_slots == VKN_RT_SLOTS_COPY) same = 0;
    if (same) return 0;
    int inl; op_slots *s = (op_slots *)rec_new(K_SLOTS, sizeof(op_slots), &inl);
    if (!s) return -1;
    s->mask = mask;
    for (int k = 0; k < SLOT_ARR; k++) {
        if (mask >> k & 1) { memcpy(s->a[k], arr_of(in, k), sizeof s->a[k]); memcpy(g_eslot[k], s->a[k], sizeof s->a[k]); }
        else memset(s->a[k], 0xff, sizeof s->a[k]);
    }
    g_eslot_mask = mask; g_eslot_ok = 1; g_eslot_gen = g_slot_gen;
    ST_ADD(ops[VKN_RT_OP_SLOTS], 1); ST_ADD(slot_copies, 1);
    rec_done(&s->h, inl, 0);
    return 0;
}

// ---- draws ----
typedef struct op_draw { rt_hdr h; uint8_t indexed, cond_on, pad[2]; uint32_t first, count, instances, snap; uint32_t pad2[3]; uint64_t cond; vkn_draw_in in; } op_draw;
#define DRAW_SNAP(d) ((uint8_t *)(d) + ((sizeof(op_draw) + 15u) & ~15u))
static const vkn_vfmt *vf_of(uint64_t h) {
    if (!h) return NULL;
    if (h == g_vf_h && g_vf) return g_vf;
    const uint16_t id = vkn_vfmt_id(h); const vkn_vfmt *vf = id ? vkn_vfmt_info(id) : NULL;
    if (vf) { g_vf_h = h; g_vf = vf; }
    return vf;
}
// The buffers this draw reads, marked "used" now (vkn_buf_update's rename rule runs on this thread); the IB of an indexed draw.
static void mark(const vkn_draw_in *in, int indexed, vkn_buf_info *ib, int *ib_rc) {
    const vkn_vfmt *vf = vf_of(in->vfmt);
    uint64_t h[VKN_VFMT_STREAMS]; uint32_t n = 0;
    for (uint32_t i = 0; vf && i < vf->nbind; i++) { const uint32_t st = vf->bind[i].binding; if (st < VKN_VFMT_STREAMS && in->vb[st] && n < VKN_VFMT_STREAMS) h[n++] = in->vb[st]; }
    if (n) vkn_buf_mark(VKN_BUF_VB, h, n);
    if (indexed) *ib_rc = in->ib ? vkn_buf_lookup(VKN_BUF_IB, in->ib, ib) : -1;   // (marks it)
}
static int rw_prim(uint8_t p) { return p == 2 || p == 4 || p == 7; }
static int draw_enc(const vkn_draw_in *in, int indexed, uint32_t first, uint32_t count, uint32_t instances) {
    vkn_buf_info ib; int ib_rc = -1; memset(&ib, 0, sizeof ib);
    mark(in, indexed, &ib, &ib_rc);
    uint32_t snap = 0; const void *src = NULL;
    if (indexed && rw_prim(in->prim) && count && !ib_rc && ib.blk.size && ib.blk.buf && ib.blk.cpu) {   // the rewrite reads the engine's indices: now
        const uint32_t isz = ib.index_size == 4 ? 4 : 2;
        if (((uint64_t)first + count) * isz <= ib.size && (uint64_t)count * isz < (64u << 20)) { snap = count * isz; src = (const uint8_t *)ib.blk.cpu + (uint64_t)first * isz; }
    }
    if (slots_enc(in)) return -1;
    int inl; op_draw *d = (op_draw *)rec_new(K_DRAW, (uint32_t)((sizeof(op_draw) + 15u) & ~15u) + snap, &inl);
    if (!d) return -1;
    d->indexed = (uint8_t)indexed; d->first = first; d->count = count; d->instances = instances; d->snap = snap;
    d->cond_on = (uint8_t)g_cond_on; d->cond = g_cond; g_cond_on = 0;
    d->in = *in; for (int k = 0; k < VKN_STAGE_SLOTS; k++) { d->in.tex[k] = NULL; d->in.smp[k] = NULL; }
    if (snap) { memcpy(DRAW_SNAP(d), src, snap); ST_ADD(ops[VKN_RT_OP_SNAP], 1); }
    ST_ADD(ops[VKN_RT_OP_DRAW], 1);
    rec_done(&d->h, inl, 1);
    return 0;
}
int vkn_rt_draw_indexed(const vkn_draw_in *in, uint32_t first, uint32_t count, uint32_t instances) {
    if (!vkn_rt_live_) return vkn_draw_indexed(in, first, count, instances);
    if (!in) return 1;
    if (draw_enc(in, 1, first, count, instances)) { ST_ADD(oom, 1); vkn_rt_own_begin(&g_site_oom); const int r = vkn_draw_indexed(in, first, count, instances); vkn_rt_own_end(); return r; }
    return 0;
}
int vkn_rt_draw_arrays(const vkn_draw_in *in, uint32_t first, uint32_t count) {
    if (!vkn_rt_live_) return vkn_draw_arrays(in, first, count);
    if (!in) return 1;
    if (draw_enc(in, 0, first, count, 1)) { ST_ADD(oom, 1); vkn_rt_own_begin(&g_site_oom); const int r = vkn_draw_arrays(in, first, count); vkn_rt_own_end(); return r; }
    return 0;
}
typedef struct op_disp { rt_hdr h; uint8_t cond_on, mask, pad[2]; uint32_t x, y, z, nimg; uint32_t pad2[3]; uint64_t cond; vkn_draw_in in; int32_t a[6][VKN_TEX_SLOTS]; } op_disp;
int vkn_rt_dispatch(const vkn_draw_in *in, const vkn_img_bind *img, uint32_t nimg, uint32_t x, uint32_t y, uint32_t z) {
    if (!vkn_rt_live_) { vkn_query_dispatch(); return vkn_draw_dispatch(in, img, nimg, x, y, z); }
    if (!in) { vkn_query_dispatch(); return 1; }
    int inl; op_disp *d = (op_disp *)rec_new(K_DISPATCH, sizeof(op_disp) + nimg * (uint32_t)sizeof(vkn_img_bind), &inl);
    if (!d) { ST_ADD(oom, 1); vkn_rt_own_begin(&g_site_oom); vkn_query_dispatch(); const int r = vkn_draw_dispatch(in, img, nimg, x, y, z); vkn_rt_own_end(); return r; }
    d->cond_on = (uint8_t)g_cond_on; d->cond = g_cond; g_cond_on = 0;
    d->x = x; d->y = y; d->z = z; d->nimg = nimg; d->mask = 0;
    d->in = *in;
    for (int k = 0; k < 6; k++) {
        const int32_t *p = k < 3 ? in->tex[k] : in->smp[k - 3];
        if (p) { memcpy(d->a[k], p, sizeof d->a[k]); d->mask |= (uint8_t)(1u << k); } else memset(d->a[k], 0xff, sizeof d->a[k]);
    }
    if (nimg) memcpy(d + 1, img, nimg * sizeof *img);
    ST_ADD(ops[VKN_RT_OP_DISPATCH], 1);
    rec_done(&d->h, inl, 1);
    return 0;
}

// ---- frames ----
typedef struct op_begin { rt_hdr h; int32_t presented, pad; uint64_t predicted; } op_begin;
typedef struct op_end { rt_hdr h; int32_t present, interval, has_crop, pad; vkn_tex *win; int32_t crop[4]; } op_end;
typedef struct op_tail { rt_hdr h; uint32_t pad[2]; uint64_t cpu[3]; void (*tick)(void); } op_tail;
void vkn_rt_rec_begin(int presented) {
    if (!vkn_rt_live_ && g_mode != VKN_RT_OFF) bind_encoder();
    if (!vkn_rt_live_) { (void)vkn_rec_begin(presented); return; }
    if (g_threaded && !pthread_equal(pthread_self(), g_enc)) {   // another thread renders frames now: it becomes the encoder
        RT_REDLN(R_FRAME, "a frame begins on another thread than the last one: that thread encodes from now on");
        foreign_wait(); g_enc = pthread_self(); __atomic_store_n(&g_owner_engine, 1, __ATOMIC_RELEASE); rebase();
    }
    if (g_rebase && g_threaded) vkn_rt_sync(&g_site_rebase);
    if (presented && g_enc_due) { g_enc_frame++; g_enc_due = 0; }
    g_enc_serial++;
    if (presented) { g_ring_start[g_enc_frame & 15] = vkn_ring_cum(); for (int i = 0; i < VKN_RT_CPU_RINGS; i++) g_cr_start[i][g_enc_frame & 15] = g_cr_cum[i]; }
    int inl; op_begin *b = (op_begin *)rec_new(K_BEGIN, sizeof(op_begin), &inl);
    if (!b) { vkn_rt_own_begin(&g_site_oom); (void)vkn_rec_begin(presented); vkn_rt_own_end(); return; }
    b->presented = presented; b->predicted = g_enc_frame;
    ST_ADD(ops[VKN_RT_OP_FRAME], 1);
    rec_done(&b->h, inl, 1);
}
void vkn_rt_rec_end(int present, vkn_tex *win, const int32_t crop[4], int interval) {
    if (!vkn_rt_live_) {
        if (present) { vkn_pass_end(); if (win) (void)vkn_rec_present(win, crop, interval); }
        (void)vkn_rec_submit(!present);
        return;
    }
    if (present) g_enc_due = 1;
    int inl; op_end *e = (op_end *)rec_new(K_END, sizeof(op_end), &inl);
    if (!e) {
        vkn_rt_own_begin(&g_site_oom);
        if (present) { vkn_pass_end(); if (win) (void)vkn_rec_present(win, crop, interval); }
        (void)vkn_rec_submit(!present); vkn_rt_own_end();
    } else {
        e->present = present; e->win = win; e->interval = interval; e->has_crop = crop != NULL;
        if (crop) memcpy(e->crop, crop, sizeof e->crop); else memset(e->crop, 0, sizeof e->crop);
        ST_ADD(ops[VKN_RT_OP_FRAME], 1);
        rec_done(&e->h, inl, 1);
    }
    if (!present) vkn_rt_sync(&g_site_off);   // the engine's FenceSync + SyncWait: the GPU work is finished when EndFrame returns
}
void vkn_rt_frame_tail(const uint64_t cpu[3], void (*tick)(void)) {
    if (!vkn_rt_live_) { vkn_prof_frame_cpu(cpu[0], cpu[1], cpu[2]); if (tick) tick(); return; }
    int inl; op_tail *t = (op_tail *)rec_new(K_TAIL, sizeof(op_tail), &inl);
    if (!t) { vkn_rt_own_begin(&g_site_oom); vkn_prof_frame_cpu(cpu[0], cpu[1], cpu[2]); if (tick) tick(); vkn_rt_own_end(); return; }
    memcpy(t->cpu, cpu, sizeof t->cpu); t->tick = tick;
    ST_ADD(ops[VKN_RT_OP_FRAME], 1);
    if (!inl) g_enc_tails++;
    rec_done(&t->h, inl, 1);
    if (inl || !g_threaded) return;
    // backpressure: frames written - frames recorded <= the mode's lead
    const uint64_t wk = __atomic_load_n(&g_wk_tails, __ATOMIC_SEQ_CST), ahead = g_enc_tails - wk;
    ST_ADD(ahead[ahead > 3 ? 3 : ahead], 1);
    const uint64_t lead = g_mode == VKN_RT_AHEAD2 ? 2 : 1;
    if (ahead <= lead) return;
    const uint64_t target = g_enc_tails - lead, t0 = vkn_now_ns();
    pthread_mutex_lock(&g_mu);
    __atomic_store_n(&g_wait_tails, target, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_wk_tails, __ATOMIC_SEQ_CST) < target) pthread_cond_wait(&g_cv_done, &g_mu);
    __atomic_store_n(&g_wait_tails, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_mu);
    ST_ADD(bp_wait_ns, vkn_now_ns() - t0);
}

// ---- the host's backend calls ----
typedef struct { uint32_t n, pad; vkn_tex *c[VKN_PASS_COLORS]; uint32_t l[VKN_PASS_COLORS], m[VKN_PASS_COLORS]; vkn_tex *d; } targets_job;
static void targets_fn(void *p) { targets_job *j = (targets_job *)p; vkn_pass_targets(j->n, j->c, j->l, j->m, j->d); }
void vkn_rt_pass_targets(uint32_t n, vkn_tex *const color[VKN_PASS_COLORS], const uint32_t *layer, const uint32_t *mip, vkn_tex *depth) {
    if (!vkn_rt_live_) { vkn_pass_targets(n, color, layer, mip, depth); return; }
    targets_job j; memset(&j, 0, sizeof j);
    j.n = n; j.d = depth;
    for (int i = 0; i < VKN_PASS_COLORS; i++) { j.c[i] = color ? color[i] : NULL; j.l[i] = layer ? layer[i] : 0; j.m[i] = mip ? mip[i] : 0; }   // (NULL arrays = zeros there)
    vkn_rt_call(targets_fn, &j, sizeof j);
}
typedef struct { vkn_tex *t; uint32_t layer; int has_rect; float rgba[4]; int32_t rect[4]; } clear_job;
static void clear_fn(void *p) { clear_job *j = (clear_job *)p; vkn_pass_clear_color(j->t, j->layer, j->rgba, j->has_rect ? j->rect : NULL); }
void vkn_rt_pass_clear_color(vkn_tex *t, uint32_t layer, const float rgba[4], const int32_t rect[4]) {
    if (!vkn_rt_live_) { vkn_pass_clear_color(t, layer, rgba, rect); return; }
    clear_job j; memset(&j, 0, sizeof j);
    j.t = t; j.layer = layer; memcpy(j.rgba, rgba, sizeof j.rgba); j.has_rect = rect != NULL; if (rect) memcpy(j.rect, rect, sizeof j.rect);
    vkn_rt_call(clear_fn, &j, sizeof j);
}
typedef struct { vkn_tex *t; int what; float depth; uint32_t stencil; uint64_t *none; } cds_job;
static void cds_fn(void *p) { cds_job *j = (cds_job *)p; vkn_pass_clear_depth(j->t, j->what, j->depth, j->stencil); }
static void cds_bound_fn(void *p) {
    cds_job *j = (cds_job *)p;
    vkn_tex *bd = vkn_pass_depth();   // what NVN would clear: its bound depth target
    if (!bd) { if (j->none) __atomic_add_fetch(j->none, 1, __ATOMIC_RELAXED); return; }
    vkn_pass_clear_depth(bd, j->what, j->depth, j->stencil);
}
void vkn_rt_pass_clear_depth(vkn_tex *t, int what, float depth, uint32_t stencil) {
    if (!vkn_rt_live_) { vkn_pass_clear_depth(t, what, depth, stencil); return; }
    const cds_job j = {t, what, depth, stencil, NULL};
    vkn_rt_call(cds_fn, &j, sizeof j);
}
void vkn_rt_pass_clear_bound_depth(int what, float depth, uint32_t stencil, uint64_t *none) {
    cds_job j = {NULL, what, depth, stencil, none};
    if (!vkn_rt_live_) { cds_bound_fn(&j); return; }
    vkn_rt_call(cds_bound_fn, &j, sizeof j);
}
typedef struct { vkn_tex *dst, *src; } copy_job;
static void copy_fn(void *p) { copy_job *j = (copy_job *)p; vkn_pass_copy(j->dst, j->src); }
void vkn_rt_pass_copy(vkn_tex *dst, vkn_tex *src) {
    if (!vkn_rt_live_) { vkn_pass_copy(dst, src); return; }
    const copy_job j = {dst, src};
    vkn_rt_call(copy_fn, &j, sizeof j);
}
static void qbegin_fn(void *p) { vkn_query_begin(*(const uint64_t *)p); }
static void qend_fn(void *p) { vkn_query_end(*(const uint64_t *)p); }
void vkn_rt_query_begin(uint64_t key) { if (!vkn_rt_live_) vkn_query_begin(key); else vkn_rt_call(qbegin_fn, &key, sizeof key); }
void vkn_rt_query_end(uint64_t key) { if (!vkn_rt_live_) vkn_query_end(key); else vkn_rt_call(qend_fn, &key, sizeof key); }
typedef struct { vkn_tex *a, *b; int32_t v[4]; int k, has; } ui_job;
static void ui_fn(void *p) {
    ui_job *j = (ui_job *)p;
    switch (j->k) {
    case 0: vkn_ui_targets(j->a, j->b); break;
    case 1: vkn_ui_viewport(j->v[0], j->v[1], j->v[2], j->v[3]); break;
    case 2: vkn_ui_scissor(j->v[0], j->v[1], j->v[2], j->v[3]); break;
    case 3: vkn_ui_clear_ds(j->v[0], (uint8_t)j->v[1]); break;
    case 4: vkn_ui_clear_color(j->a, j->has ? j->v : NULL); break;
    case 6: vkn_ui_pass_fence(); break;   // [stage 5.2 audit] the UI pass piece's fence (vkn_ui.h)
    default: vkn_draw_raw_begin(); break;
    }
}
static void ui_rec(int k, vkn_tex *a, vkn_tex *b, int32_t x, int32_t y, int32_t w, int32_t h) {
    ui_job j; memset(&j, 0, sizeof j); j.k = k; j.a = a; j.b = b; j.v[0] = x; j.v[1] = y; j.v[2] = w; j.v[3] = h; j.has = 1;
    if (!vkn_rt_live_) ui_fn(&j); else vkn_rt_call(ui_fn, &j, sizeof j);
}
void vkn_rt_ui_targets(vkn_tex *color, vkn_tex *depth) { ui_rec(0, color, depth, 0, 0, 0, 0); }
void vkn_rt_ui_viewport(int32_t x, int32_t y, int32_t w, int32_t h) { ui_rec(1, NULL, NULL, x, y, w, h); }
void vkn_rt_ui_scissor(int32_t x, int32_t y, int32_t w, int32_t h) { ui_rec(2, NULL, NULL, x, y, w, h); }
void vkn_rt_ui_clear_ds(int depth, uint8_t mask) { ui_rec(3, NULL, NULL, depth, mask, 0, 0); }
void vkn_rt_ui_clear_color(vkn_tex *t, const int32_t rect[4]) {
    if (!vkn_rt_live_) { vkn_ui_clear_color(t, rect); return; }
    ui_job j; memset(&j, 0, sizeof j); j.k = 4; j.a = t; j.has = rect != NULL; if (rect) memcpy(j.v, rect, sizeof j.v);
    vkn_rt_call(ui_fn, &j, sizeof j);
}
void vkn_rt_draw_raw_begin(void) { if (!vkn_rt_live_) vkn_draw_raw_begin(); else ui_rec(5, NULL, NULL, 0, 0, 0, 0); }
void vkn_rt_ui_pass_fence(void) { ui_rec(6, NULL, NULL, 0, 0, 0, 0); }
// A UI draw: the draw input with its CPU bytes behind it (offsets instead of pointers), the vertex buffer block by value.
typedef struct { vkn_ui_draw_in d; vkn_block vb; uint8_t has_vb, pad[3]; uint32_t o_vsc, o_fsc, o_fsc1, o_verts, o_idx, bytes; } uidraw_job;
static void uidraw_fn(void *p) {
    uidraw_job *j = (uidraw_job *)p; uint8_t *b = (uint8_t *)(j + 1);
    vkn_ui_draw_in d = j->d;
    d.vsc = j->d.vsc ? b + j->o_vsc : NULL; d.fsc = j->d.fsc ? b + j->o_fsc : NULL; d.fsc1 = j->d.fsc1 ? b + j->o_fsc1 : NULL;
    d.verts = j->d.verts ? b + j->o_verts : NULL; d.idx = j->d.idx ? (const uint16_t *)(b + j->o_idx) : NULL;
    d.vb = j->has_vb ? &j->vb : NULL;
    (void)vkn_ui_draw(&d);
}
int vkn_rt_ui_draw(const vkn_ui_draw_in *in) {
    if (!vkn_rt_live_ || !in) return vkn_ui_draw(in);
    const uint32_t stride = in->vfmt == VKN_UI_VF_V2 ? 8u : 16u;
    const uint64_t nv = in->vb || !in->verts ? 0 : (uint64_t)in->nverts * stride, ni = in->vb || !in->idx || in->quads ? 0 : 2ull * in->nidx;
    const uint64_t nvs = in->vsc ? in->vsc_bytes : 0, nfs = in->fsc ? in->fsc_bytes : 0, nf1 = in->fsc1 ? in->fsc1_bytes : 0;
    const uint64_t a16 = 15;
    const uint64_t o_vsc = 0, o_fsc = (o_vsc + nvs + a16) & ~a16, o_fsc1 = (o_fsc + nfs + a16) & ~a16, o_verts = (o_fsc1 + nf1 + a16) & ~a16, o_idx = (o_verts + nv + a16) & ~a16;
    const uint64_t total = o_idx + ni;
    if (total > (32u << 20)) { vkn_rt_own_begin(&g_site_oom); const int r = vkn_ui_draw(in); vkn_rt_own_end(); return r; }   // (no record that big: executed exactly)
    uint8_t stackbuf[2048]; uint8_t *buf = sizeof(uidraw_job) + total <= sizeof stackbuf ? stackbuf : (uint8_t *)malloc(sizeof(uidraw_job) + total);
    if (!buf) { vkn_rt_own_begin(&g_site_oom); const int r = vkn_ui_draw(in); vkn_rt_own_end(); return r; }
    uidraw_job *j = (uidraw_job *)buf; uint8_t *b = buf + sizeof *j;
    memset(j, 0, sizeof *j);
    j->d = *in; j->has_vb = in->vb != NULL; if (in->vb) j->vb = *in->vb;
    j->o_vsc = (uint32_t)o_vsc; j->o_fsc = (uint32_t)o_fsc; j->o_fsc1 = (uint32_t)o_fsc1; j->o_verts = (uint32_t)o_verts; j->o_idx = (uint32_t)o_idx; j->bytes = (uint32_t)total;
    if (nvs) memcpy(b + o_vsc, in->vsc, nvs);
    if (nfs) memcpy(b + o_fsc, in->fsc, nfs);
    if (nf1) memcpy(b + o_fsc1, in->fsc1, nf1);
    if (nv) memcpy(b + o_verts, in->verts, nv);
    if (ni) memcpy(b + o_idx, in->idx, ni);
    vkn_rt_call(uidraw_fn, buf, (uint32_t)(sizeof(uidraw_job) + total));
    if (buf != stackbuf) free(buf);
    return 0;
}

// ---- the ring guard (vkn_buf.c, the encoder, threaded modes) ----
// Ring bytes of the frames the GPU may still read: from the start of the oldest unfinished frame to the end of this allocation. Over the
// ring's size the allocation would overwrite them: sync, then wait for the GPU until it fits (or nothing is left to wait for).
static vkn_rt_site g_site_cpu_ring = VKN_RT_SITE("CPU-written ring guard");
static void guard_core(const uint64_t *starts, uint64_t cum_end, uint64_t lim, vkn_rt_site *site, int cpu);
void vkn_rt_ring_guard(uint64_t cum_end) {
    if (!g_threaded || !pthread_equal(pthread_self(), g_enc) || t_dec) return;
    guard_core(g_ring_start, cum_end, g_ring_limit ? g_ring_limit : VKN_RING_BYTES, &g_site_ring, 0);
}
// [stage 5.2 audit] A ring of memory the engine writes through a buffer's CPU pointer (the immediate draws' vertex / index rings,
// hle_dos2_vkn_imm.c): `advance` = what the head moves by (the tail a wrap skips included), called BEFORE the bytes are written.
// The GPU reads them when it executes the draws; with the recording thread the engine thread can be 1-2 frames further ahead of that
// than without it, so a ring that wraps within that distance would overwrite vertices of draws not executed yet (or not recorded yet:
// the cover proof and LIGHTFUSE read them while recording). Same guard as the constant ring's: sync + wait for the GPU, never overwrite.
void vkn_rt_cpu_ring(int id, uint64_t advance, uint64_t size) {
    if (id < 0 || id >= VKN_RT_CPU_RINGS || !vkn_rt_live_ || !pthread_equal(pthread_self(), g_enc)) return;
    g_cr_cum[id] += advance;
    if (!g_threaded || t_dec) return;
    guard_core(g_cr_start[id], g_cr_cum[id], size, &g_site_cpu_ring, 1);
}
static void guard_core(const uint64_t *starts, uint64_t cum_end, uint64_t lim, vkn_rt_site *site, int cpu) {
    for (int pass = 0; pass < 16; pass++) {
        int any = 0; const uint64_t done = vkn_frame_gpu_done(&any);
        uint64_t oldest = any ? done + 1 : 0;
        if (g_enc_frame >= 15 && oldest + 15 < g_enc_frame) oldest = g_enc_frame - 15;   // (older than the table: its start is the table's oldest)
        if (oldest > g_enc_frame) return;
        const uint64_t start = starts[oldest & 15];
        if (cum_end <= start + lim) return;
        if (oldest == g_enc_frame) { RT_REDLN(R_RING, "%s: one frame needs more than the ring (%llu bytes): overwritten as without the thread", cpu ? "CPU-written ring" : "constant ring", (unsigned long long)lim); return; }
        if (!pass) { if (cpu) ST_ADD(guard_cpu, 1); else ST_ADD(guard_ring, 1); }
        vkn_rt_own_begin(site);
        vkn_frame_wait_frame(oldest);
        vkn_rt_own_end();
    }
}

// ================================================================ execution
static void exec(rt_hdr *h) {
    switch (h->kind) {
    case K_DRAW: {
        op_draw *d = (op_draw *)h;
        vkn_draw_in in = d->in;
        for (int k = 0; k < 2; k++) { in.tex[k] = (g_dslot_mask >> k & 1) ? g_dslot[k] : NULL; in.smp[k] = (g_dslot_mask >> (k + 2) & 1) ? g_dslot[k + 2] : NULL; }
        if (d->cond_on) vkn_query_cond(d->cond);
        const int prof = g_threaded;
        if (prof) vkn_prof_draw_begin();
        if (d->snap) { vkn_draw_rt_snap = DRAW_SNAP(d); vkn_draw_rt_in = &in; }
        if (d->indexed) (void)vkn_draw_indexed(&in, d->first, d->count, d->instances); else (void)vkn_draw_arrays(&in, d->first, d->count);
        vkn_draw_rt_snap = NULL; vkn_draw_rt_in = NULL;
        if (prof) vkn_prof_draw_end();
        break;
    }
    case K_DISPATCH: {
        op_disp *d = (op_disp *)h;
        vkn_draw_in in = d->in;
        for (int k = 0; k < 3; k++) { in.tex[k] = (d->mask >> k & 1) ? d->a[k] : NULL; in.smp[k] = (d->mask >> (k + 3) & 1) ? d->a[k + 3] : NULL; }
        if (d->cond_on) vkn_query_cond(d->cond);
        vkn_query_dispatch();
        (void)vkn_draw_dispatch(&in, (const vkn_img_bind *)(d + 1), d->nimg, d->x, d->y, d->z);
        break;
    }
    case K_UBO: {
        const ubo_e *e = (const ubo_e *)((op_ubo *)h + 1);
        for (uint32_t i = 0; i < h->n; i++) vkn_draw_ubo(e[i].stage, e[i].slot, e[i].off, e[i].size);
        break;
    }
    case K_SLOTS: { op_slots *s = (op_slots *)h; g_dslot_mask = s->mask; memcpy(g_dslot, s->a, sizeof g_dslot); break; }
    case K_CALL: { op_call *c = (op_call *)h; c->fn(c->bytes ? CALL_PAY(c) : NULL); break; }
    case K_BEGIN: {
        op_begin *b = (op_begin *)h;
        (void)vkn_rec_begin(b->presented);
        if (b->presented && g_threaded && vkn_frame() != b->predicted) {   // a failed begin / submit took another path than the encoder assumed
            ST_ADD(mispredict, 1); g_rebase = 1;
            RT_REDLN(R_MISPREDICT, "frame counter %llu at a recording begin, the engine thread assumed %llu: it re-reads it at its next frame", (unsigned long long)vkn_frame(), (unsigned long long)b->predicted);
        }
        break;
    }
    case K_END: {
        op_end *e = (op_end *)h;
        if (e->present) { vkn_pass_end(); if (e->win) (void)vkn_rec_present(e->win, e->has_crop ? e->crop : NULL, e->interval); }
        (void)vkn_rec_submit(!e->present);
        break;
    }
    case K_TAIL: {
        op_tail *t = (op_tail *)h;
        ST_ADD(frames, 1);
        vkn_prof_frame_cpu(t->cpu[0], t->cpu[1], t->cpu[2]);
        if (t->tick) t->tick();
        if (g_threaded && t_wk) {
            retire_pending_run(0);
            const uint64_t n = __atomic_add_fetch(&g_wk_tails, 1, __ATOMIC_SEQ_CST), w = __atomic_load_n(&g_wait_tails, __ATOMIC_SEQ_CST);
            if (w && n >= w) { pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv_done); pthread_mutex_unlock(&g_mu); }
        }
        break;
    }
    default: break;
    }
}

// ================================================================ census
void vkn_rt_stats_get(vkn_rt_stats *s) { if (!s) return; uint64_t *d = (uint64_t *)s; const uint64_t *f = (const uint64_t *)&g_st; for (size_t i = 0; i < sizeof *s / 8; i++) d[i] = __atomic_load_n(&f[i], __ATOMIC_RELAXED); }
void vkn_rt_census(void) {
    static vkn_rt_stats o;
    vkn_rt_stats s; vkn_rt_stats_get(&s);
    const uint64_t df = s.frames - o.frames; const double f = df ? (double)df : 1.0;
#define PF(x) ((double)(s.x - o.x) / f)
    // the 8 sites with the most syncs in this window
    vkn_rt_site *top[8] = {0}; uint64_t topn[8] = {0};
    pthread_mutex_lock(&g_site_mu);
    for (vkn_rt_site *p = g_sites; p; p = p->next) {
        const uint64_t n = __atomic_load_n(&p->syncs, __ATOMIC_RELAXED), d = n - p->prev_syncs; p->prev_syncs = n;
        for (int k = 0; k < 8; k++) if (d > topn[k]) { for (int j = 7; j > k; j--) { top[j] = top[j - 1]; topn[j] = topn[j - 1]; } top[k] = p; topn[k] = d; break; }
    }
    pthread_mutex_unlock(&g_site_mu);
    char ts[640]; int n = 0; ts[0] = 0;
    for (int k = 0; k < 8 && top[k] && n < (int)sizeof ts - 80; k++) n += snprintf(ts + n, sizeof ts - (size_t)n, "%s %s %.2f", k ? "," : "", top[k]->name, (double)topn[k] / f);
    vkn_logf("rt: DM_VK_NATIVE_RT=%d (%s), %llu frames: records a frame: draws %.0f, dispatches %.1f, announces %.0f, slot copies %.0f, calls %.0f, frame %.1f, index copies %.1f, "
             "%.1f KB; syncs a frame %.2f (%.3f ms):%s; engine wait at the frame end %.3f ms a frame, worker idle %.3f / busy %.3f ms a frame; frames ahead 0:%llu 1:%llu 2:%llu 3+:%llu; "
             "ring guard syncs %llu (CPU-written rings %llu), mispredicted frames %llu, foreign records %llu, executed at once %.0f a frame, stream full %llu, slot-range misses %llu",
             g_mode, g_threaded ? "recording thread" : g_mode == VKN_RT_VERIFY ? "verify" : "no thread", (unsigned long long)df, PF(ops[VKN_RT_OP_DRAW]), PF(ops[VKN_RT_OP_DISPATCH]),
             PF(ops[VKN_RT_OP_UBO]), PF(ops[VKN_RT_OP_SLOTS]), PF(ops[VKN_RT_OP_CALL]), PF(ops[VKN_RT_OP_FRAME]), PF(ops[VKN_RT_OP_SNAP]), PF(bytes) / 1024.0, PF(syncs), PF(sync_wait_ns) / 1e6,
             n ? ts : " none", PF(bp_wait_ns) / 1e6, PF(idle_ns) / 1e6, PF(busy_ns) / 1e6, (unsigned long long)(s.ahead[0] - o.ahead[0]), (unsigned long long)(s.ahead[1] - o.ahead[1]),
             (unsigned long long)(s.ahead[2] - o.ahead[2]), (unsigned long long)(s.ahead[3] - o.ahead[3]), (unsigned long long)s.guard_ring, (unsigned long long)s.guard_cpu, (unsigned long long)s.mispredict,
             (unsigned long long)s.foreign, PF(inline_ops), (unsigned long long)s.oom, (unsigned long long)s.slot_gen_miss);
#undef PF
    if (s.mispredict != o.mispredict || s.foreign != o.foreign || s.oom != o.oom)
        vkn_logf(RT_RED "rt: unexpected in this window: mispredicted frames %llu, foreign records %llu, stream full %llu", (unsigned long long)(s.mispredict - o.mispredict),
                 (unsigned long long)(s.foreign - o.foreign), (unsigned long long)(s.oom - o.oom));
    o = s;
}
#endif
