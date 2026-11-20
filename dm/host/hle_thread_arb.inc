// [NR-24] (docs/dos2/nr/NR-24.md) Included by hle_thread.c right after the G-path lock primitives; uses its thr_t, G, note_runnable,
// hand_leave, the main-thread spin settings and ld32 / st32 / cas32.
//
// DM_NR_GATE_ARB: the arbitration SVCs - 0x1A ArbitrateLock, 0x1B ArbitrateUnlock, 0x1C WaitProcessWideKeyAtomic, 0x1D
// SignalProcessWideKey, 0x34 WaitForAddress, 0x35 SignalToAddress - on 256 address buckets instead of the one global lock G and its one
// list of every waiter. 0 (default) = the G path as before. 1 = buckets. 2 = buckets plus invariant checks after every operation (a
// lock waiter whose lock word lacks the waiter bit, a condition waiter whose key is 0, a waiter linked in the wrong bucket: red lines,
// the first 16 and a count every 10 s through the cpuprof line). A wait on the G path and a bucket wait never meet: the mode is read
// once (svc_thread_main_init) before any guest thread exists, and only the six SVCs above change path.
// Semantics are the G path's, which are Horizon's (mesosphere KConditionVariable / KAddressArbiter): the waiter chosen is the lowest
// priority value, then the earliest registered (wseq); a condition waiter moved onto its lock by a signal is registered anew (after the
// lock's waiters of equal priority, as KThread::AddWaiter); 0x1C publishes key = 1 and links itself before it releases the lock word
// (t_condvar); a signal fixes the set of waiters it takes atomically (all of them under the key's bucket) and the "no waiter left: key
// = 0" write in the same critical section, then moves each taken waiter onto its lock under that lock's bucket; a timeout that races a
// signal returns either the timeout or the handed-over lock, never both (the bucket lock decides, as the scheduler lock does on the
// Switch); SignalToAddress counts and wakes under one bucket lock. 0x1A does not check the owner handle and 0x35 type 2 with count <= 0
// keeps this build's value (-2 under DM_GAME_DOOM): byte-for-byte the G path's rules, not changed here (KB HLE-13).
// Sleeping: a per-thread futex on t->woke (FUTEX_WAIT_BITSET on CLOCK_MONOTONIC, absolute deadline), so a waker never needs the
// sleeper's lock (a waiter may move between buckets while it sleeps). fsleep tells the waker whether a futex wake is needed at all.
// The main thread's DM_HLE_SPIN_US spin, bootprof accounting and the DOOM same-core handoff bookkeeping (note_runnable / hand_leave,
// under G: lock order is bucket(s) by address, then G) are kept. Linux / Android only (futex); elsewhere the switch is refused.
// Not with DM_DETCLOCK (its waits run on the present count and need dm_guest_clock_advanced's G-path kicks): refused with a log line.
//
// DM_NR_GATE_YIELD: a poll + yield loop (SleepThread 0 / -1 / -2 or a sleep under 20 us, no progress by the DOOM yield signature)
// that has run DM_NR_GATE_YIELD_US (50) us waits on a process-wide progress futex instead of sched_yield / the 2 ms back-off: woken
// by any guest kernel event that can end a poll (a thread starting / exiting / sleeping / blocking, an event / condition / address
// signal, a lock release, a native HLE wait), at the latest after a eighth of the run so far, 20..DM_NR_GATE_YIELD_CAP_US (250) us.
// Horizon's yield returns at once when no other thread of the core is runnable; on the host the other threads run in parallel anyway,
// so the wait changes timing only (bounded by the run / 8 rule of the existing back-off). 0 = off, 1 = on, 2 = count only (yields that
// would have waited and the poll time past the threshold).
#include <stdatomic.h>
#if defined(__linux__)
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#define ARB_HAVE 1
static long arb_futex(int *a, int op, int v, const struct timespec *ts) { return syscall(SYS_futex, a, op, v, ts, NULL, FUTEX_BITSET_MATCH_ANY); }
#else
#define ARB_HAVE 0
#endif

#define ARB_NB 256
typedef struct arb_b { pthread_mutex_t mu; thr_t *head; uint64_t take, cont; } __attribute__((aligned(64))) arb_b;
static arb_b g_arb[ARB_NB];
static _Atomic uint64_t g_arb_seq;                                     // wseq of bucket waits: one total order, taken under the bucket lock
static _Atomic uint64_t g_arb_wakes, g_arb_fwakes, g_arb_red, g_arb_moves;
static inline arb_b *arb_bk(uint64_t a) { return &g_arb[(unsigned)(((a >> 2) * 0x9E3779B97F4A7C15ull) >> 56)]; }
static inline void arb_lock(arb_b *b) {
    if (__builtin_expect(gs_on(), 0)) { if (pthread_mutex_trylock(&b->mu)) { pthread_mutex_lock(&b->mu); b->cont++; } }
    else pthread_mutex_lock(&b->mu);
    b->take++;
}
static inline void arb_unlock(arb_b *b) { pthread_mutex_unlock(&b->mu); }
static void arb_lock2(arb_b *x, arb_b *y) {
    if (x == y) { arb_lock(x); return; }
    if (x > y) { arb_b *t = x; x = y; y = t; }
    arb_lock(x); arb_lock(y);
}
static void arb_unlock2(arb_b *x, arb_b *y) { arb_unlock(x); if (y != x) arb_unlock(y); }
// Holding b. t is the calling thread (a new wait) or a moved waiter; woke is 0 for both
static void arb_link(arb_b *b, thr_t *t, int kind) {
    __atomic_store_n(&t->woke, 0, __ATOMIC_RELAXED);
    t->wkind = kind; t->bnext = b->head; b->head = t;
    t->wseq = atomic_fetch_add_explicit(&g_arb_seq, 1, memory_order_relaxed) + 1;
    atomic_store_explicit(&t->wb, b, memory_order_release);
}
static void arb_unlink(arb_b *b, thr_t *t) {
    for (thr_t **p = &b->head; *p; p = &(*p)->bnext) if (*p == t) { *p = t->bnext; break; }
    t->bnext = NULL; atomic_store_explicit(&t->wb, NULL, memory_order_release);
}
static thr_t *arb_pick(arb_b *b, int kind, uint64_t a) {   // pick() over one bucket
    thr_t *best = NULL;
    for (thr_t *t = b->head; t; t = t->bnext) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->wseq < best->wseq)) best = t;
    }
    return best;
}
// w is linked nowhere (unlinked under its bucket lock, or taken by a signal and given the lock word); the caller holds the bucket lock
// that decided it. Same order as wake(): result first, woke last (the sleeper reads wres after an acquire of woke)
static void arb_wake(thr_t *w, int64_t res) {
    w->wkind = 0; w->wres = res;
#ifdef DM_GAME_DOOM
    pthread_mutex_lock(&G); note_runnable(w); pthread_mutex_unlock(&G);   // the handoff bookkeeping stays under G (bucket -> G)
#endif
    __atomic_store_n(&w->woke, 1, __ATOMIC_SEQ_CST);
    atomic_fetch_add_explicit(&g_arb_wakes, 1, memory_order_relaxed); __atomic_fetch_add(&g_wakes, 1, __ATOMIC_RELAXED);
#if ARB_HAVE
    if (__atomic_load_n(&w->fsleep, __ATOMIC_SEQ_CST)) { arb_futex(&w->woke, FUTEX_WAKE_PRIVATE, 1, NULL); atomic_fetch_add_explicit(&g_arb_fwakes, 1, memory_order_relaxed); }
    else __atomic_fetch_add(&g_wake_skips, 1, __ATOMIC_RELAXED);
#endif
}
// DM_NR_GATE_ARB=2: the bucket's invariants (holding b)
static void arb_check(arb_b *b, const char *op) {
    if (g_arb_mode != 2) return;
    for (thr_t *t = b->head; t; t = t->bnext) {
        const char *bad = NULL;
        if (atomic_load_explicit(&t->wb, memory_order_relaxed) != b) bad = "linked in a bucket it does not name";
        else if (t->wkind < 1 || t->wkind > 3) bad = "linked with no arbiter wait";
        else if (arb_bk(t->wkind == 2 ? t->wkey : t->waddr) != b) bad = "in the wrong bucket";
        else if (t->wkind == 1 && !(ld32(t->waddr) & WAITERS_BIT)) bad = "a lock waiter but the lock word has no waiter bit (a user-mode release would not wake it)";
        else if (t->wkind == 2 && !ld32(t->wkey)) bad = "a condition waiter but the key is 0 (a signal would skip the svc)";
        if (bad && atomic_fetch_add(&g_arb_red, 1) < 16)
            LOG("【红】[nr24] DM_NR_GATE_ARB=2 after %s: thread 0x%x kind %d addr %#llx key %#llx: %s", op, t->handle, t->wkind, (unsigned long long)t->waddr, (unsigned long long)t->wkey, bad);
    }
}
// A timed-out waiter takes itself off its bucket, unless a waker got there first (then the wake's result stands)
static int64_t arb_timeout(thr_t *t) {
    for (int spins = 0;; spins++) {
        arb_b *b = atomic_load_explicit(&t->wb, memory_order_acquire);
        if (!b) {   // being woken, or a signal is moving it onto its lock (taken from the key's bucket, not yet in the lock's)
            if (__atomic_load_n(&t->woke, __ATOMIC_ACQUIRE)) return t->wres;
            if (spins > 64) sched_yield();
            continue;
        }
        arb_lock(b);
        if (atomic_load_explicit(&t->wb, memory_order_relaxed) == b) { arb_unlink(b, t); t->wkind = 0; arb_check(b, "timeout"); arb_unlock(b); return R_TIMEOUT; }
        arb_unlock(b);
    }
}
// The calling thread t is linked; no lock is held. ns < 0 = no timeout (0 never gets here). Returns wres or R_TIMEOUT
static int64_t arb_sleep(thr_t *t, int kind, int64_t ns) {
    const uint64_t b0 = g_flag.bootprof ? now_ns() : 0;
#ifdef DM_GAME_DOOM
    if (t->nhand) { pthread_mutex_lock(&G); hand_leave(t); pthread_mutex_unlock(&G); }   // blocking gives the core back
#endif
    int64_t r = R_TIMEOUT;
#if ARB_HAVE
    struct timespec dl = {0, 0};
    if (ns > 0) { clock_gettime(CLOCK_MONOTONIC, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    if (g_spin_ns && t->handle == 0x1001 && (ns < 0 || (uint64_t)ns > g_spin_ns) && spin_phase_on()) {   // DM_HLE_SPIN_US, as block_inner
        const uint64_t end = now_ns() + g_spin_ns;
        while (!__atomic_load_n(&t->woke, __ATOMIC_ACQUIRE) && now_ns() < end) {
#if defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        }
    }
    for (;;) {
        if (__atomic_load_n(&t->woke, __ATOMIC_ACQUIRE)) { r = t->wres; break; }
        __atomic_store_n(&t->fsleep, 1, __ATOMIC_SEQ_CST);
        long rc = 0;
        if (!__atomic_load_n(&t->woke, __ATOMIC_SEQ_CST)) rc = arb_futex(&t->woke, FUTEX_WAIT_BITSET_PRIVATE, 0, ns > 0 ? &dl : NULL);
        __atomic_store_n(&t->fsleep, 0, __ATOMIC_RELAXED);
        if (__atomic_load_n(&t->woke, __ATOMIC_ACQUIRE)) { r = t->wres; break; }
        if (ns > 0 && rc < 0 && errno == ETIMEDOUT) { r = arb_timeout(t); break; }
    }
#else
    (void)kind; (void)ns;
#endif
    if (b0) {
        const uint64_t d = now_ns() - b0; static const int id[] = {0, PROF_WAIT_LOCK, PROF_WAIT_CV, PROF_WAIT_ADDR, PROF_WAIT_SYNC};
        dm_prof_add(id[kind & 7], d, 0); if (t->handle == 0x1001) dm_prof_add(PROF_MAIN_WAIT, d, 0);
    }
    return r;
}
// The lock word at addr is released by its holder (holding b = arb_bk(addr)): to its best waiter, or to 0 (lock_handoff)
static void arb_lock_handoff(arb_b *b, uint64_t addr) {
    thr_t *w = arb_pick(b, 1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = b->head; t; t = t->bnext) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    arb_unlink(b, w); arb_wake(w, 0);
}
// A signalled condition waiter w (linked nowhere) takes its lock, or waits on it (holding bl = arb_bk(w->waddr); lock_acquire_for)
static void arb_acquire_for(arb_b *bl, thr_t *w) {
    const uint64_t addr = w->waddr; const uint32_t tag = w->wtag; uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { arb_wake(w, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    arb_link(bl, w, 1);   // registered anew: after the lock's waiters of equal priority
    atomic_fetch_add_explicit(&g_arb_moves, 1, memory_order_relaxed);
}

#if defined(DM_TEST_HOOKS) && !defined(CV_TEST_HOOK)
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }
#elif !defined(CV_TEST_HOOK)
#define CV_TEST_HOOK()
#endif
// Returns 1 when num is one of the six and was handled
static int arb_svc(int num, uint64_t *regs) {
    switch (num) {
    case 0x1A: {  // ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); const uint64_t addr = regs[1]; arb_b *b = arb_bk(addr); int64_t r = 0;
        arb_lock(b);
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; arb_link(b, t, 1); arb_check(b, "0x1a"); arb_unlock(b); r = arb_sleep(t, 1, -1); }
        else arb_unlock(b);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: {  // ArbitrateUnlock(x0 address)
        arb_b *b = arb_bk(regs[0]);
        arb_lock(b); arb_lock_handoff(b, regs[0]); arb_check(b, "0x1b"); arb_unlock(b);
        regs[0] = 0; return 1;
    }
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout): key = 1 and linked before the lock word is released
        thr_t *t = cur(); const uint64_t addr = regs[0], key = regs[1]; const int64_t to = (int64_t)regs[3];
        arb_b *bk = arb_bk(key), *bl = arb_bk(addr);
        arb_lock2(bk, bl);
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        if (to != 0) arb_link(bk, t, 2);
        arb_lock_handoff(bl, addr); CV_TEST_HOOK();
#else   // t_condvar's negative control only: the pre-fix order, released first, key and link after
        arb_lock_handoff(bl, addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        if (to != 0) arb_link(bk, t, 2);
#endif
        arb_check(bk, "0x1c"); if (bl != bk) arb_check(bl, "0x1c");
        arb_unlock2(bk, bl);
        const int64_t r = to == 0 ? R_TIMEOUT : arb_sleep(t, 2, to);   // timeout: the lock is not taken again (user mode does that)
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 condition key, w1 count, <= 0 all): the set and the key write under the key's bucket, then each onto its lock
        const uint64_t key = regs[0]; const int cnt = (int)regs[1]; int n = 0; arb_b *bk = arb_bk(key);
        thr_t *list = NULL, **tail = &list;
        arb_lock(bk);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = arb_pick(bk, 2, key)); n++) { arb_unlink(bk, w); *tail = w; tail = &w->bnext; }
        int left = 0; for (thr_t *w = bk->head; w; w = w->bnext) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        arb_check(bk, "0x1d");
        arb_unlock(bk);
        while (list) {
            thr_t *w = list; list = w->bnext; w->bnext = NULL;
            arb_b *bl = arb_bk(w->waddr);
            arb_lock(bl); arb_acquire_for(bl, w); arb_check(bl, "0x1d move"); arb_unlock(bl);
        }
        regs[0] = 0; return 1;
    }
    case 0x34: {  // WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less, 1 decrement if less then wait, 2 wait if equal (3: 64-bit equal, DOOM)
        thr_t *t = cur(); const uint64_t addr = regs[0]; const int type = (int)regs[1]; const int32_t val = (int32_t)regs[2]; const int64_t to = (int64_t)regs[3];
        arb_b *b = arb_bk(addr); int64_t r;
        arb_lock(b);
        uint32_t v = ld32(addr);
#ifdef DM_GAME_DOOM
        int ok = type == 3 ? __atomic_load_n((uint64_t *)addr, __ATOMIC_SEQ_CST) == regs[2] : type == 2 ? (int32_t)v == val : (int32_t)v < val;
#else
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
#endif
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) { arb_unlock(b); r = R_BAD_STATE; }
        else if (to == 0) { arb_unlock(b); r = R_TIMEOUT; }
        else { t->waddr = addr; arb_link(b, t, 3); arb_check(b, "0x34"); arb_unlock(b); r = arb_sleep(t, 3, to); }
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 address, w1 type, w2 value, w3 count): count, value and wakes under one bucket lock
        const uint64_t addr = regs[0]; const int type = (int)regs[1]; const int32_t val = (int32_t)regs[2]; const int cnt = (int)regs[3];
        arb_b *b = arb_bk(addr);
        arb_lock(b);
        int nw = 0; for (thr_t *w = b->head; w; w = w->bnext) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
#ifdef DM_GAME_DOOM
                if (cnt <= 0) nv = nw > 0 ? val - 2 : val + 1;
#else
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
#endif
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0; for (thr_t *w; (cnt <= 0 || n < cnt) && (w = arb_pick(b, 3, addr)); n++) { arb_unlink(b, w); arb_wake(w, 0); } }
        arb_check(b, "0x35");
        arb_unlock(b);
        regs[0] = (uint64_t)r; return 1;
    }
    }
    return 0;
}
#undef CV_TEST_HOOK   // hle_thread.c's 0x1C defines its own

// ---------- DM_NR_GATE_YIELD ----------
static _Atomic int g_yw_seq, g_yw_waiters;                  // the progress futex word and how many sleep on it
static uint64_t g_yw_start_ns = 50000, g_yw_cap_ns = 250000;
static _Atomic uint64_t g_yw_n, g_yw_woke, g_yw_tmo, g_yw_ns, g_yw_would;
static void yw_kick(void) {
#if ARB_HAVE
    if (atomic_load(&g_yw_waiters)) { atomic_fetch_add(&g_yw_seq, 1); arb_futex((int *)&g_yw_seq, FUTEX_WAKE_PRIVATE, INT_MAX, NULL); }
#endif
}
// From svc_thread_impl (mode 1): the kernel events that can end another thread's poll. Yields and the information SVCs are not
static void yw_progress(int num, const uint64_t *regs) {
    switch (num) {
    case 0x0B: if ((int64_t)regs[0] >= 20000) yw_kick(); return;   // a real sleep, not a yield
    case 0x09: case 0x0A: case 0x11: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x34: case 0x35: yw_kick(); return;
    default: return;
    }
}
// One no-progress yield of a run that has gone on run_ns. Returns 1 when it waited (mode 1) in place of yield_for
static int yw_wait(thr_t *me, uint64_t run_ns) {
    if (run_ns < g_yw_start_ns) return 0;
    if (g_yw_mode == 2) { atomic_fetch_add_explicit(&g_yw_would, 1, memory_order_relaxed); return 0; }
#if ARB_HAVE
    uint64_t to = run_ns / 8; if (to < 20000) to = 20000; if (to > g_yw_cap_ns) to = g_yw_cap_ns;
    const int seq = atomic_load(&g_yw_seq);
    atomic_fetch_add(&g_yw_waiters, 1);
    me->sleeping = 1;   // not runnable for a same-core handoff, as a back-off sleep
    const uint64_t a = now_ns(); struct timespec ts = {0, (long)to};
    syscall(SYS_futex, (int *)&g_yw_seq, FUTEX_WAIT_PRIVATE, seq, &ts, NULL, 0);
    const uint64_t d = now_ns() - a;
    me->sleeping = 0;
    atomic_fetch_sub(&g_yw_waiters, 1);
    atomic_fetch_add_explicit(&g_yw_n, 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_yw_ns, d, memory_order_relaxed);
    atomic_fetch_add_explicit(atomic_load(&g_yw_seq) != seq ? &g_yw_woke : &g_yw_tmo, 1, memory_order_relaxed);
    me->nbackoff++;
    return 1;
#else
    (void)me; return 0;
#endif
}

// Once, from svc_thread_main_init: before any guest thread can wait
static void arb_init(void) {
    static int done; if (done++) return;
    for (int i = 0; i < ARB_NB; i++) pthread_mutex_init(&g_arb[i].mu, NULL);
    if (!g_spin_init) { const char *e = getenv("DM_HLE_SPIN_US"); g_spin_ns = e ? (uint64_t)atoi(e) * 1000 : 0; e = getenv("DM_HLE_SPIN_AB"); g_spin_ab = e ? atoi(e) : 0; g_spin_init = 1; }
    const char *det = getenv("DM_DETCLOCK"); const int detc = det && strtoull(det, 0, 0) > 0;
    const char *e = getenv("DM_NR_GATE_ARB"); int m = e && *e ? atoi(e) : 0;
    if (m > 0 && (!ARB_HAVE || detc)) LOG("[nr24] DM_NR_GATE_ARB=%d refused: %s (the G path kept)", m, detc ? "DM_DETCLOCK waits need the G path" : "needs Linux futexes");
    else if (m > 0) { g_arb_mode = m >= 2 ? 2 : 1;
        LOG("[nr24] DM_NR_GATE_ARB=%d: 0x1A-0x1D / 0x34 / 0x35 on %d address buckets with futex sleeps%s", g_arb_mode, ARB_NB, g_arb_mode == 2 ? ", invariants checked after every operation" : ""); }
    e = getenv("DM_NR_GATE_YIELD"); m = e && *e ? atoi(e) : 0;
    if ((e = getenv("DM_NR_GATE_YIELD_US")) && *e) g_yw_start_ns = strtoull(e, 0, 10) * 1000;
    if ((e = getenv("DM_NR_GATE_YIELD_CAP_US")) && *e) { g_yw_cap_ns = strtoull(e, 0, 10) * 1000; if (g_yw_cap_ns < 20000) g_yw_cap_ns = 20000; }
#ifndef DM_GAME_DOOM
    if (m > 0) { LOG("[nr24] DM_NR_GATE_YIELD=%d refused: the yield run tracking is the DOOM core's (thr_yield)", m); m = 0; }
#endif
    if (m == 1 && (!ARB_HAVE || detc)) { LOG("[nr24] DM_NR_GATE_YIELD=1 refused: %s", detc ? "DM_DETCLOCK" : "needs Linux futexes"); m = 0; }
    if (m > 0) { g_yw_mode = m >= 2 ? 2 : 1;
        LOG("[nr24] DM_NR_GATE_YIELD=%d: a poll + yield run past %llu us %s (wait = run / 8, 20..%llu us, woken by guest kernel events)", g_yw_mode,
            (unsigned long long)(g_yw_start_ns / 1000), g_yw_mode == 1 ? "waits on the progress futex" : "is counted only", (unsigned long long)(g_yw_cap_ns / 1000)); }
}
// hle_cpuprof.c (DM_NR_CPU=1): running totals for the [cpuprof] nr24 line
void dm_nr24_arb_stats(uint64_t out[8]) {
    uint64_t take = 0, cont = 0;
    for (int i = 0; i < ARB_NB; i++) { take += __atomic_load_n(&g_arb[i].take, __ATOMIC_RELAXED); cont += __atomic_load_n(&g_arb[i].cont, __ATOMIC_RELAXED); }
    out[0] = (uint64_t)g_arb_mode; out[1] = take; out[2] = cont; out[3] = atomic_load(&g_arb_wakes); out[4] = atomic_load(&g_arb_fwakes); out[5] = atomic_load(&g_arb_moves);
    out[6] = atomic_load(&g_arb_red); out[7] = 0;
}
void dm_nr24_yield_stats(uint64_t out[8]) {
    out[0] = (uint64_t)g_yw_mode; out[1] = atomic_load(&g_yw_n); out[2] = atomic_load(&g_yw_woke); out[3] = atomic_load(&g_yw_tmo); out[4] = atomic_load(&g_yw_ns);
    out[5] = atomic_load(&g_yw_would); out[6] = g_yw_start_ns; out[7] = g_yw_cap_ns;
}
