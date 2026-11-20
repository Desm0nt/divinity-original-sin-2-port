// [NR-28] nvn_leancall.inc's C side on a real Linux kernel (WSL / any x86-64 or aarch64 Linux): the table registration under
// concurrent threads, the take-over of a dead thread's stack, mode 2's decision against the old one, the counters.
// Stubs stand for the host (LOG, die, now_ns), the slot table and the assembly entries; the thread pointer is a per-thread test value.
// Build: gcc -std=gnu11 -O1 -Wall -Wextra -pthread nr_leancall_c_test.c -o nr_leancall_c_test && ./nr_leancall_c_test
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#define NVN_LC_TEST 1
#define NVN_LC_REPORT_NS 20000000ull   // 20 ms
#define NVN_NFUNC 8
#define NVN_BIGSTACK (1u << 20)
#define LOG(...) do { printf("[log] " __VA_ARGS__); printf("\n"); } while (0)
static void die(const char *m) { fprintf(stderr, "die: %s\n", m); exit(2); }
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static int dm_cpu_lse(void) { return 1; }
typedef struct { _Atomic uint64_t cnt; void *fn; } nvn_slot_t;
nvn_slot_t nvn_slot[NVN_NFUNC];
void *nvn_real[NVN_NFUNC], *nvn_real_big[NVN_NFUNC];
void *nvn_thunk[NVN_NFUNC], *nvn_thunk_v80[NVN_NFUNC];
const char *nvn_names[NVN_NFUNC] = {"nvnA", "nvnB", "nvnC", "nvnD", "nvnE", "nvnF", "nvnG", "nvnH"};
static __thread uint8_t *t_big;
static __thread uint64_t t_tp;
static uint64_t nvn_lc_test_tp(void) { return t_tp; }
// the assembly the include names (never called here)
void nvn_bigstack_entry(void) {}
void nvn_trace_entry(void) {}
void nvn_prof_entry(void) {}
void nvn_prof_entry_v80(void) {}
void nvn_prof2_entry(void) {}
void nvn_lean_entry(void) {}
void nvn_lc_loop(void *fn, void *x16, uint64_t n) { (void)fn; (void)x16; (void)n; }
void nvn_lc_ret(void) {}
void nvn_lc_on_stack(void (*fn)(void *), void *arg, void *top) { (void)top; fn(arg); }
void *nvn_on_bigstack(void *(*fn)(void *), void *arg) { return fn(arg); }
static void nvn_nop(void) {}
static uint64_t g_frames;
uint64_t dm_nvn_frame_count(void) { return g_frames; }
#include "../../../nvn/nvn_leancall.inc"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// the old decision (nvn_bigstack_top without NR-28)
static void *old_top(uintptr_t sp) { const uintptr_t lo = (uintptr_t)t_big, hi = lo + NVN_BIGSTACK; return sp >= lo && sp <= hi ? NULL : (void *)hi; }

typedef struct { uint64_t tp; int reg; uint8_t *big; void *r1; void *r2; } job_t;
static pthread_barrier_t g_bar;
static void *racer(void *a) {
    job_t *j = a; t_tp = j->tp;
    pthread_barrier_wait(&g_bar);
    j->r1 = nvn_lc_top(0x1000);                       // off the big stack: its top
    j->reg = t_lc_reg; j->big = t_big;
    j->r2 = nvn_lc_top((uintptr_t)t_big + 0x800);     // on it: in place
    return NULL;
}

int main(void) {
    // 1. 16 threads, 8 of them hash to one slot (tp differ above the hash bits: same hash), 8 to their own: exactly one of the 8 wins
    g_lc_mode = 1;
    enum { N = 16 };
    pthread_t th[N]; job_t jb[N]; memset(jb, 0, sizeof jb);
    const uint64_t base = 0x7f0000000010ull;
    const unsigned h0 = nvn_lc_hash(base);
    int same = 0;
    for (int k = 0; k < N; k++) {
        if (k < 8) {   // find thread pointers with the same hash
            uint64_t t = base + (uint64_t)k * 0x1000000000ull; while (nvn_lc_hash(t) != h0) t += 0x1000; jb[k].tp = t; same++;
        } else { uint64_t t = base + 0x104000ull * (uint64_t)(k + 1); jb[k].tp = t; }
    }
    pthread_barrier_init(&g_bar, NULL, N);
    for (int k = 0; k < N; k++) pthread_create(&th[k], NULL, racer, &jb[k]);
    for (int k = 0; k < N; k++) pthread_join(th[k], NULL);
    int won = 0;
    for (int k = 0; k < N; k++) {
        CHECK(jb[k].r1 == (void *)(jb[k].big + NVN_BIGSTACK) && jb[k].r2 == NULL, "thread %d: the old decision (top, then in place)", k);
        if (k < 8) won += jb[k].reg == 1;
        else CHECK(jb[k].reg == 1 || nvn_lc_hash(jb[k].tp) == h0, "thread %d with its own slot is registered", k);
    }
    CHECK(won == 1, "8 threads on one hash slot: exactly one registered (%d)", won);
    for (int k = 0; k < 8; k++) if (jb[k].reg == 1) {
        nvn_lc_ent_t *e = &nvn_lc_tab[h0];
        CHECK(e->tp == jb[k].tp && e->top == (uint64_t)(uintptr_t)(jb[k].big + NVN_BIGSTACK), "the winner's entry holds its own thread pointer and stack");
    }
    CHECK(atomic_load(&g_lc_regs) + atomic_load(&g_lc_coll) == N, "every thread counted once (registered %llu + without %llu)", (unsigned long long)g_lc_regs, (unsigned long long)g_lc_coll);
    // 2. a new thread with a dead thread's thread pointer takes its stack over
    for (int k = 8; k < 9; k++) {
        job_t j = {.tp = jb[k].tp}; pthread_t t; pthread_barrier_destroy(&g_bar); pthread_barrier_init(&g_bar, NULL, 1);
        pthread_create(&t, NULL, racer, &j); pthread_join(t, NULL);
        CHECK(j.big == jb[k].big && j.reg == 1 && atomic_load(&g_lc_adopt) == 1, "same thread pointer: the dead thread's stack (%p vs %p), registered", (void *)j.big, (void *)jb[k].big);
    }
    // 3. mode 2: every decision against the old one, sp everywhere around the stack; the lean answer (sp + 240) agrees
    g_lc_mode = 2; t_tp = 0x7e0000abc010ull; t_big = NULL; t_lc_reg = 0;
    nvn_lc_top(0x1000);
    CHECK(t_lc_reg == 1, "mode 2: the test thread is registered");
    const uintptr_t lo = (uintptr_t)t_big, hi = lo + NVN_BIGSTACK;
    const uint64_t bad0 = atomic_load(&g_lc_bad);
    uintptr_t sps[] = {0x1000, lo - 0x10000, lo - 240 - 16, lo - 240, lo - 16, lo, lo + 16, lo + 0x8000, hi - 0x1000, hi - 240 - 16, hi - 240, hi - 16, hi, hi + 16, hi + 0x10000};
    for (unsigned k = 0; k < sizeof sps / sizeof *sps; k++) {
        void *want = old_top(sps[k]), *got = nvn_lc_top(sps[k]);
        CHECK(got == want, "mode 2 returns the old decision at sp %#lx", (unsigned long)sps[k]);
    }
    const uint64_t dbad = atomic_load(&g_lc_bad) - bad0;
    // the lean entry sees the caller's sp = sp + 240: it differs from the wrapper's test only within 240 bytes of the edges (here lo - 240,
    // lo - 16: the caller is on the big stack, lean stays; hi - 16, hi: the caller is above the top, lean switches)
    CHECK(dbad == 4, "mode 2: the lean answer differs only on the 240-byte edges: %llu", (unsigned long long)dbad);
    // 4. counters
    const uint64_t e0 = atomic_load(&g_lc_exact), p0 = atomic_load(&g_lc_plain);
    for (int k = 0; k < 1000; k++) nvn_lc_top(0x1000);
    CHECK(atomic_load(&g_lc_exact) - e0 == 1000 && atomic_load(&g_lc_plain) - p0 == 1000 && t_lc_calls >= 1000, "mode 2: exact / plain / thread counters count every call");
    // 5. the thunk table and the install refusals
    g_lc_mode = 1; CHECK(nvn_thunk_tab() == (void *const *)nvn_thunk_v80, "=1: the plain thunk set");
    g_lc_mode = 2; CHECK(nvn_thunk_tab() == (void *const *)nvn_thunk, "=2: the stadd set (LSE CPU)");
    g_lc_mode = 0; CHECK(nvn_thunk_tab() == (void *const *)nvn_thunk, "=0: the stadd set (LSE CPU)");
    // 6. the install and the report lines of both modes (their text is what tools/dos2_cpuprof.py parses: run it on this output)
    for (int mode = 1; mode <= 2; mode++) {
        g_lc_mode = 0;
        for (int i = 0; i < NVN_NFUNC; i++) { nvn_slot[i].fn = (void *)nvn_bigstack_entry; nvn_real_big[i] = i == 3 ? (void *)nvn_nop : (void *)nvn_lc_ret; }
        setenv("DM_NR_LEANCALL", mode == 1 ? "1" : "2", 1);
        g_lc_inst_done = 0; nvn_lc_install();
        CHECK(g_lc_mode == mode, "install: mode %d", mode);
        int lean = 0; for (int i = 0; i < NVN_NFUNC; i++) lean += nvn_slot[i].fn == (void *)nvn_lean_entry;
        CHECK(lean == (mode == 1 ? NVN_NFUNC : 0), "install: mode %d puts the lean entry in %d slots", mode, lean);
        atomic_store(&g_lc_cal_state, 0); g_lc_ncal = 0; g_lc_nop = -1;
        for (int k = 0; k < 400; k++) {
            g_frames += 3; for (int i = 0; i < NVN_NFUNC; i++) atomic_fetch_add(&nvn_slot[i].cnt, 25);
            for (int c = 0; c < 200; c++) nvn_lc_top(0x1000);
            nvn_lc_submit();
            struct timespec ts = {0, 200000}; nanosleep(&ts, NULL);
        }
        for (int w = 0; w < 400 && atomic_load(&g_lc_cal_state) != 2; w++) { struct timespec ts = {0, 5000000}; nanosleep(&ts, NULL); }
        CHECK(atomic_load(&g_lc_cal_state) == 2 && g_lc_ncal > 0 && g_lc_nop == 3, "mode %d: calibration ran (%d cpus, no-op slot %d)", mode, g_lc_ncal, g_lc_nop);
        for (int k = 0; k < 30; k++) { g_frames += 3; nvn_lc_submit(); struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL); }
    }
    printf("%s: nr_leancall_c_test (%d checks): table registration of 16 racing threads (one winner per hash slot), the take-over of a dead thread's stack, mode 2's "
           "decision = the old one (lean differs only on the wrapper's 240-byte edges), counters, thunk table\n", fails ? "FAIL" : "PASS", checks);
    return fails != 0;
}
