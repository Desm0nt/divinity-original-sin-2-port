// [DOS2] NR-28 DM_NR_LEANCALL (default 0; docs/dos2/nr/NR-28.md): the lean path of every guest NVN call. Included by hle_nvn.c inside the
// big-stack block (DM_GAME_ETERNAL / XC2 / D3 lineage), right before nvn_bigstack_top.
//
// The old path of one call (DOS2, LSE phone): thunk (stadd of the slot's call counter, a load of the slot's function) -> _nvn_bigstack_entry
// (a 240-byte frame on the guest stack, x0-x7 and q0-q7 stored, the slot index) -> nvn_bigstack_top (C: a TLS descriptor call for t_big,
// the NR-23 counter test, the range test) -> the 16 argument registers reloaded -> the stack switch -> the body -> back.
//   DM_NR_LEANCALL 0 / unset  nothing (the slots, the thunk table and nvn_bigstack_top's path as before but for one load + not-taken branch)
//                  1          lean: the plain-counter thunks (the ARMv8.0 set, no atomic) and _nvn_lean_entry (nvn_entry.S) in front of the
//                             same bodies: the thread's big stack from nvn_lc_tab[hash(tpidr_el0)], no C call, no TLS, nothing saved but
//                             the frame pair on the big stack. A thread's first call (and a thread whose hash slot another one owns) goes
//                             the old way; nvn_bigstack_top then puts it in the table. Off with DM_NR_NOCALL=2 (NR-23 counts the calls
//                             in nvn_bigstack_top, which the lean entry skips) and with DM_NVN_BIGSTACK=0 (nothing to make lean).
//                  2          compare: the old path runs (atomic thunks, the old wrapper); nvn_bigstack_top computes for every call what the
//                             lean entry would do (table entry, in place / switch / old path) against what it does, counts every call
//                             exactly, plainly (a non-atomic counter, as =1's thunks count) and through the slot counters; mismatches as
//                             red lines (first 16 + totals every 10 s).
//   Both 1 and 2: once, after 600 frames, a host thread of its own calibrates the paths on every CPU it may run on (nvn_lc_calibrate) and
//   every 10 s the submitting thread prints the counts and the estimate (calls a frame x ns saved on the recording thread's CPU).
// The table: one entry {thread pointer, top} per thread that called NVN, written once (the thread pointer by CAS, then the top) and never
// cleared. A later thread with the same thread pointer (bionic reuses the mapping of a dead thread) takes the dead thread's stack over:
// only one live thread can have that thread pointer, and the dead one no longer runs, so the stack is free (the old code leaks it).
// Only aarch64 Linux / Android: tpidr_el0 is the host's thread pointer on guest threads too (macOS keeps it in tpidrro_el0).
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#ifndef NVN_LC_REPORT_NS
#define NVN_LC_REPORT_NS 10000000000ull     // the report interval (the test shortens it)
#endif
#define NVN_LC_CAL_FRAME 600                // the calibration starts after this many frames (past the loading screens)
#define NVN_LC_TAB 4096                     // entries (64 KiB); the mask in _nvn_lean_entry is NVN_LC_TAB - 1
typedef struct { uint64_t tp, top; } nvn_lc_ent_t;
nvn_lc_ent_t nvn_lc_tab[NVN_LC_TAB] __attribute__((aligned(64)));   // read by _nvn_lean_entry
void nvn_lean_entry(void);
void nvn_prof2_entry(void);
void nvn_lc_loop(void *fn, void *x16, uint64_t n);
void nvn_lc_ret(void);
void nvn_lc_on_stack(void (*fn)(void *), void *arg, void *top);
void *nvn_on_bigstack(void *(*fn)(void *), void *arg);
static void nvn_nop(void);
uint64_t dm_nvn_frame_count(void);
static int g_lc_mode;                       // DM_NR_LEANCALL as installed (0 / 1 / 2)
static __thread uint8_t t_lc_reg;           // 0 = not looked at, 1 = this thread's entry is in the table, 2 = not (the old path)
static __thread uint64_t t_lc_calls;        // =2: this thread's calls
static _Atomic uint64_t g_lc_slow, g_lc_regs, g_lc_coll, g_lc_adopt, g_lc_exact, g_lc_plain, g_lc_bad, g_lc_here, g_lc_pass;
#define NVN_BIG_FN(f) ((f) == (void *)nvn_bigstack_entry || (f) == (void *)nvn_lean_entry)
static inline unsigned nvn_lc_hash(uint64_t tp) { uint64_t t = tp >> 12; t ^= t >> 10; t ^= t >> 20; return (unsigned)(t & (NVN_LC_TAB - 1)); }
static inline uint64_t nvn_lc_tp(void) {
#if defined(NVN_LC_TEST)
    return nvn_lc_test_tp();   // nr_leancall_c_test.c: the thread pointer of the test thread
#elif defined(__aarch64__) && !defined(__APPLE__)
    return (uint64_t)(uintptr_t)__builtin_thread_pointer();
#else
    return 0;
#endif
}
// The thunk table nvn_lookup hands out: =1 the plain-counter set (no atomic in the thunk), else as before (LSE or ARMv8.0 by the CPU)
static void *const *nvn_thunk_tab(void) { return dm_cpu_lse() && g_lc_mode != 1 ? nvn_thunk : nvn_thunk_v80; }
static uint8_t *nvn_lc_map(void) {   // the same mapping as nvn_bigstack_top's
    uint8_t *m = mmap(NULL, NVN_BIGSTACK + 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) die("NVN big stack mmap");
    mprotect(m, 0x4000, PROT_NONE);
    return m + 0x4000;
}
static void lc_red(const char *what, uint64_t a, uint64_t b) {
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) LOG("【红】[nvn] leancall check (DM_NR_LEANCALL=2): %s (%#llx vs %#llx)", what, (unsigned long long)a, (unsigned long long)b);
}
// nvn_bigstack_top with DM_NR_LEANCALL 1 / 2. Mode 1: only the calls the lean entry passed on (a thread's first, an unregistered thread's).
// Mode 2: every call (the old wrapper is in); sp is the old wrapper's (its 240-byte frame below the caller's sp).
static __attribute__((noinline)) void *nvn_lc_top(uintptr_t sp) {
    const uint64_t tp = nvn_lc_tp();
    nvn_lc_ent_t *e = &nvn_lc_tab[nvn_lc_hash(tp)];
    if (!t_lc_reg) {
        uint64_t owner = __atomic_load_n(&e->tp, __ATOMIC_ACQUIRE);
        if (!t_big && owner == tp) { t_big = (uint8_t *)(uintptr_t)(__atomic_load_n(&e->top, __ATOMIC_RELAXED) - NVN_BIGSTACK); atomic_fetch_add(&g_lc_adopt, 1); }
        if (!t_big) t_big = nvn_lc_map();
        const uint64_t top = (uint64_t)(uintptr_t)(t_big + NVN_BIGSTACK);
        if (!owner && __atomic_compare_exchange_n(&e->tp, &owner, tp, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&e->top, top, __ATOMIC_RELEASE);   // after the claim: no other thread can match this entry, this one reads it later
            t_lc_reg = 1; atomic_fetch_add(&g_lc_regs, 1);
        } else if (owner == tp && __atomic_load_n(&e->top, __ATOMIC_RELAXED) == top) { t_lc_reg = 1; atomic_fetch_add(&g_lc_regs, 1); }
        else { t_lc_reg = 2; atomic_fetch_add(&g_lc_coll, 1); }
    }
    const uintptr_t lo = (uintptr_t)t_big, hi = lo + NVN_BIGSTACK;
    void *const r = sp >= lo && sp <= hi ? NULL : (void *)hi;
    if (g_lc_mode == 2) {
        t_lc_calls++; atomic_fetch_add_explicit(&g_lc_exact, 1, memory_order_relaxed);
        atomic_store_explicit(&g_lc_plain, atomic_load_explicit(&g_lc_plain, memory_order_relaxed) + 1, memory_order_relaxed);   // a plain counter
        const uint64_t etp = __atomic_load_n(&e->tp, __ATOMIC_ACQUIRE), etop = __atomic_load_n(&e->top, __ATOMIC_RELAXED), lsp = sp + 240;
        if (etp != tp) atomic_fetch_add_explicit(&g_lc_pass, 1, memory_order_relaxed);   // the lean entry passes on: the old path, same result
        else {
            void *const l = etop - lsp <= NVN_BIGSTACK ? NULL : (void *)(uintptr_t)etop;
            if (l != r) { atomic_fetch_add(&g_lc_bad, 1); lc_red("the lean entry's stack differs from the wrapper's", (uint64_t)(uintptr_t)l, (uint64_t)(uintptr_t)r); }
            else if (!l) atomic_fetch_add_explicit(&g_lc_here, 1, memory_order_relaxed);
        }
    } else atomic_fetch_add_explicit(&g_lc_slow, 1, memory_order_relaxed);
    return r;
}
// The slot's body under the wrappers (big stack, then DM_NVN_PROF=2 / trace / timing)
static void *lc_impl(unsigned i) {
    void *f = nvn_slot[i].fn;
    if (NVN_BIG_FN(f)) f = nvn_real_big[i];
    if (f == (void *)nvn_trace_entry || f == (void *)nvn_prof_entry || f == (void *)nvn_prof_entry_v80) f = nvn_real[i];
    return f;
}
static void *lc_unwrap(unsigned i) { void *f = lc_impl(i); return f == (void *)nvn_prof2_entry ? nvn_real[i] : f; }
// In nvn_lookup, after the big-stack wrapper and before DM_NVN_PROF=2 (which goes under whichever entry is in)
static int g_lc_inst_done;
static void nvn_lc_install(void) {
    if (g_lc_inst_done) return;
    g_lc_inst_done = 1;
    const char *e = getenv("DM_NR_LEANCALL"); const int v = e && *e ? atoi(e) : 0;
    if (v != 1 && v != 2) { if (v) LOG("[nvn] DM_NR_LEANCALL=%d: not a mode (0 / 1 / 2): off", v); return; }
#if !defined(NVN_LC_TEST) && (!defined(__aarch64__) || defined(__APPLE__))
    LOG("[nvn] DM_NR_LEANCALL=%d needs an aarch64 Linux / Android host (tpidr_el0): off", v); return;
#endif
    unsigned n = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) n += nvn_slot[i].fn == (void *)nvn_bigstack_entry;
    if (!n) { LOG("[nvn] DM_NR_LEANCALL=%d: the big-stack wrapper is not in (DM_NVN_BIGSTACK=0): off", v); return; }
    const char *nc = getenv("DM_NR_NOCALL");
    if (v == 1 && nc && atoi(nc) == 2) { LOG("[nvn] DM_NR_LEANCALL=1 with DM_NR_NOCALL=2: NR-23 counts the calls in nvn_bigstack_top, which the lean entry skips: off"); return; }
    g_lc_mode = v;   // before the slots change: nvn_bigstack_top fills the table from the first call on
    if (v == 1) for (unsigned i = 0; i < NVN_NFUNC; i++) if (nvn_slot[i].fn == (void *)nvn_bigstack_entry) nvn_slot[i].fn = (void *)nvn_lean_entry;
    LOG("[nvn] DM_NR_LEANCALL=%d (NR-28): %u NVN slots %s; %s", v, n, v == 1 ? "go through the lean entry (no C call, no TLS, nothing saved before the body)" :
        "stay on the big-stack wrapper, every call compared with what the lean entry would do",
        v == 1 ? "thunks with a plain call counter (no atomic; the calls-a-frame line can miss a few increments when threads race)" : "thunks unchanged (atomic counter)");
}
// ---- calibration (a host thread of its own, once): the paths through a no-op slot on every CPU the process may run on ----
#define LC_MAXCPU 16
typedef struct { int cpu, reg; double ghz, base, body, ta, tp, old, lean, old_here, lean_here, saved; } lc_cal_t;
static lc_cal_t g_lc_cal[LC_MAXCPU]; static int g_lc_ncal; static _Atomic int g_lc_cal_state;   // 0 not started, 1 running, 2 done
static int g_lc_nop = -1;
static _Atomic int g_lc_rec_cpu = -1;
enum { LC_N = 4096, LC_REP = 5 };
static double lc_time(void *fn, void *x16) {   // ns a call of fn (minus nothing; the caller takes the loop's own cost out)
    double best = 1e30;
    for (int r = 0; r < LC_REP; r++) { uint64_t a = now_ns(); nvn_lc_loop(fn, x16, LC_N); double d = (double)(now_ns() - a) / LC_N; if (d < best) best = d; }
    return best;
}
static void lc_cal_off(void *arg) {   // on a small stack of its own (a guest stack stands for it): every path switches to the big stack
    lc_cal_t *c = arg; void *slot = &nvn_slot[g_lc_nop];
    nvn_lc_loop((void *)nvn_bigstack_entry, slot, 1);   // this thread's big stack and table entry (nvn_lc_top)
    c->reg = t_lc_reg;
    c->base = lc_time((void *)nvn_lc_ret, NULL);
    c->body = lc_time((void *)nvn_nop, NULL) - c->base;
    c->old = lc_time((void *)nvn_bigstack_entry, slot) - c->base;
    c->lean = lc_time((void *)nvn_lean_entry, slot) - c->base;
    c->ta = dm_cpu_lse() ? lc_time(nvn_thunk[g_lc_nop], NULL) - c->base : -1;   // stadd thunk -> the slot's entry
    c->tp = lc_time(nvn_thunk_v80[g_lc_nop], NULL) - c->base;                    // plain thunk -> the slot's entry
}
static void *lc_cal_on(void *arg) {   // on this thread's big stack: both entries run the body in place
    lc_cal_t *c = arg; void *slot = &nvn_slot[g_lc_nop]; const double base = lc_time((void *)nvn_lc_ret, NULL);
    c->old_here = lc_time((void *)nvn_bigstack_entry, slot) - base;
    c->lean_here = lc_time((void *)nvn_lean_entry, slot) - base;
    return NULL;
}
static double lc_ghz(int cpu) {
    char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    FILE *f = fopen(p, "r"); long k = 0; if (f) { if (fscanf(f, "%ld", &k) != 1) k = 0; fclose(f); }
    return (double)k / 1e6;
}
static void *lc_cal_thread(void *arg) {
    (void)arg;
#if defined(__linux__) || defined(__ANDROID__)
    enum { SMALL = 256 << 10 };
    uint8_t *stk = mmap(NULL, SMALL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stk == MAP_FAILED) { atomic_store(&g_lc_cal_state, 2); return NULL; }
    int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF); if (ncpu > LC_MAXCPU) ncpu = LC_MAXCPU;
    char line[2400]; size_t o = 0; line[0] = 0;
    for (int cpu = 0; cpu < ncpu; cpu++) {
        unsigned long mask[LC_MAXCPU / (8 * sizeof(unsigned long)) + 1] = {0}; mask[cpu / (8 * sizeof(unsigned long))] = 1ul << (cpu % (8 * sizeof(unsigned long)));
        if (syscall(__NR_sched_setaffinity, 0, sizeof mask, mask)) continue;   // (the raw call: hle_nvn.c is built without _GNU_SOURCE)
        for (uint64_t t = now_ns(); now_ns() - t < 30000000ull;) { }   // 30 ms busy: the core leaves its idle clock
        lc_cal_t *c = &g_lc_cal[g_lc_ncal]; memset(c, 0, sizeof *c); c->cpu = cpu;
        nvn_lc_on_stack(lc_cal_off, c, stk + SMALL);
        nvn_on_bigstack(lc_cal_on, c);
        c->ghz = lc_ghz(cpu);
        // the whole call: thunk + entry; the thunks go to the slot's own entry (lean with =1, the old wrapper with =2)
        const double cur = g_lc_mode == 1 ? c->lean : c->old, told = c->ta >= 0 ? c->ta : c->tp;
        const double old_full = told - cur + c->old, lean_full = c->tp - cur + c->lean;
        c->saved = c->reg == 1 ? old_full - lean_full : 0;
        g_lc_ncal++;
        if (o < sizeof line - 260)
            o += (size_t)snprintf(line + o, sizeof line - o, " cpu%d %.2f GHz: old %.1f ns (thunk %s %.1f + wrapper %.1f), lean %.1f ns (plain thunk %.1f + entry %.1f%s), "
                                  "in place old %.1f / lean %.1f, body %.1f; saved %.1f ns = %.0f cycles;", cpu, c->ghz, old_full, c->ta >= 0 ? "stadd" : "plain",
                                  told - cur, c->old, lean_full, c->tp - cur, c->lean, c->reg == 1 ? "" : " - NOT REGISTERED: the old path", c->old_here, c->lean_here,
                                  c->body, c->saved, c->saved * c->ghz);
    }
    munmap(stk, SMALL);
    LOG("[nvn] leancall calibration (DM_NR_LEANCALL=%d, slot %s, %d calls a path, best of %d, a tight loop: warm caches and predictors):%s", g_lc_mode,
        nvn_names[g_lc_nop] + 3, LC_N, LC_REP, line);
#endif
    atomic_store(&g_lc_cal_state, 2);
    return NULL;
}
static uint64_t lc_slot_calls(void) {   // calls of the slots the lean entry (or the wrapper) is in front of, from the thunks' counters
    uint64_t s = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) if (NVN_BIG_FN(nvn_slot[i].fn)) s += atomic_load_explicit(&nvn_slot[i].cnt, memory_order_relaxed);
    return s;
}
// From nvnQueueSubmitCommands (the submitting thread = the game's recording thread), modes 1 / 2
static void nvn_lc_submit(void) {
    static _Atomic int busy; static uint64_t rep_ns, rep_fr, c0, e0, p0, s0, b0, h0, q0, my0; static int bad_tol_red;
    if (atomic_exchange(&busy, 1)) return;
#if defined(__linux__) || defined(__ANDROID__)
    { unsigned cpu = 0; if (!syscall(__NR_getcpu, &cpu, NULL, NULL)) atomic_store_explicit(&g_lc_rec_cpu, (int)cpu, memory_order_relaxed); }
#endif
    const uint64_t now = now_ns(), fr = dm_nvn_frame_count();
    if (!rep_ns) { rep_ns = now; rep_fr = fr; c0 = lc_slot_calls(); e0 = atomic_load(&g_lc_exact); p0 = atomic_load(&g_lc_plain); s0 = atomic_load(&g_lc_slow);
                   b0 = atomic_load(&g_lc_bad); h0 = atomic_load(&g_lc_here); q0 = atomic_load(&g_lc_pass); my0 = t_lc_calls; }
    if (fr >= NVN_LC_CAL_FRAME && atomic_load(&g_lc_cal_state) == 0) {
        for (unsigned i = 0; i < NVN_NFUNC && g_lc_nop < 0; i++) if (NVN_BIG_FN(nvn_slot[i].fn) && lc_unwrap(i) == (void *)nvn_nop) g_lc_nop = (int)i;
        if (g_lc_nop < 0) { atomic_store(&g_lc_cal_state, 2); LOG("[nvn] leancall: no no-op slot under the big stack: no calibration"); }
        else { atomic_store(&g_lc_cal_state, 1); pthread_t t; if (pthread_create(&t, NULL, lc_cal_thread, NULL)) atomic_store(&g_lc_cal_state, 2); else pthread_detach(t); }
    }
    if (now - rep_ns >= NVN_LC_REPORT_NS) {
        const double sec = (double)(now - rep_ns) / 1e9, frames = fr > rep_fr ? (double)(fr - rep_fr) : 1;
        const uint64_t c = lc_slot_calls(), ex = atomic_load(&g_lc_exact), pl = atomic_load(&g_lc_plain), sl = atomic_load(&g_lc_slow), bd = atomic_load(&g_lc_bad),
                       hr = atomic_load(&g_lc_here), ps = atomic_load(&g_lc_pass);
        const double cpf = (double)(c - c0) / frames;
        // the estimate: ns saved a call on the recording thread's CPU (calibration) x the calls a frame
        double sv = 0, svmax = 0; int rc = atomic_load_explicit(&g_lc_rec_cpu, memory_order_relaxed), have = 0;
        if (atomic_load(&g_lc_cal_state) == 2) for (int k = 0; k < g_lc_ncal; k++) { if (g_lc_cal[k].cpu == rc) { sv = g_lc_cal[k].saved; have = 1; } if (g_lc_cal[k].saved > svmax) svmax = g_lc_cal[k].saved; }
        char est[200]; if (have) snprintf(est, sizeof est, "%.3f ms a frame on cpu%d (%.1f ns a call), %.3f ms on the slowest calibrated core", cpf * sv / 1e6, rc, sv, cpf * svmax / 1e6);
                       else snprintf(est, sizeof est, "(no calibration yet for cpu%d)", rc);
        if (g_lc_mode == 1)
            LOG("[nvn] leancall (DM_NR_LEANCALL=1, %.0f s, %.0f frames): %.0f NVN calls a frame through the lean entry; passed on to the old wrapper %.1f a frame "
                "(first calls of threads / threads without a table entry); threads in the table %llu, without (hash slot taken) %llu, stacks taken over %llu; saved %s",
                sec, frames, cpf, (double)(sl - s0) / frames, (unsigned long long)atomic_load(&g_lc_regs), (unsigned long long)atomic_load(&g_lc_coll),
                (unsigned long long)atomic_load(&g_lc_adopt), est);
        else {
            const double dex = (double)(ex - e0), dsl = (double)(c - c0), dpl = (double)(pl - p0);
            const double tol = 64 + dex / 1000;
            if ((dex - dsl > tol || dsl - dex > tol) && bad_tol_red++ < 16)
                LOG("【红】[nvn] leancall check (DM_NR_LEANCALL=2): the wrapper saw %.0f calls, the slot counters %.0f in %.0f s", dex, dsl, sec);
            LOG("[nvn] leancall (DM_NR_LEANCALL=2, %.0f s, %.0f frames): %.0f calls a frame compared (%.0f on this submitting thread), the lean entry would take the "
                "big stack in place %.1f a frame and pass on %.1f; bad %llu (total %llu); counters: wrapper %.0f, thunks (atomic) %.0f, plain counter %.0f (lost %.0f = "
                "%.4f%% when threads race); saved by =1 %s", sec, frames, dex / frames, (double)(t_lc_calls - my0) / frames, (double)(hr - h0) / frames,
                (double)(ps - q0) / frames, (unsigned long long)(bd - b0), (unsigned long long)bd, dex, dsl, dpl, dex - dpl, dex > 0 ? 100.0 * (dex - dpl) / dex : 0, est);
        }
        rep_ns = now; rep_fr = fr; c0 = c; e0 = ex; p0 = pl; s0 = sl; b0 = bd; h0 = hr; q0 = ps; my0 = t_lc_calls;
    }
    atomic_store(&busy, 0);
}
