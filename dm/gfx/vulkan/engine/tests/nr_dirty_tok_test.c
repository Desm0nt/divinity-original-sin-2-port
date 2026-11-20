// [DOS2] NR-ZERO DM_NR_SHC_MEMO (docs/dos2/nr/NR-ZERO.md section 4): the write-protect tracker's version token (nvn_dirty.c
// nvn_dirty_version_tok / nvn_dirty_tok_ok) on Linux with real mprotect and real write faults. The production file is #included as is;
// the host pieces it calls (the fault hook registry, the host-write hook) are stubbed here: a SIGSEGV handler calls the registered hook.
//   1. the plain cases: a protected range has a token; a write fault into it, a host write ahead (prep_write), the pool's removal and a new
//      registration in the same slot each make the token stale; a write into ANOTHER pool does not;
//   2. the claim the memo stands on, against an independent model: while the token holds, the range's bytes are the bytes seen when it was
//      taken (threads write random pages of 3 pools for 2 s while a reader keeps (token, copy of the bytes) per range and, whenever the token
//      holds, compares the bytes; protect calls run meanwhile, as the backend's rv() makes them). Any change under a holding token = FAIL;
//   3. the answer under a holding token equals a fresh nvn_dirty_version whenever that one is available.
// Build (WSL): gcc -O2 -std=gnu11 -pthread -I dm/android/compat/include dm/gfx/vulkan/engine/tests/nr_dirty_tok_test.c -o nr_dirty_tok_test
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdatomic.h>
static int (*g_hook)(void *addr, int is_write, void *uc);
static void (*g_prep)(void *addr, size_t len);
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) { g_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) { g_prep = fn; }
#include "../../../nvn/nvn_dirty.c"

static int checks, fails;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void on_segv(int sig, siginfo_t *si, void *uc) {
    if (g_hook && g_hook(si->si_addr, 1, uc)) return;   // handled: the write re-executes
    signal(sig, SIG_DFL); raise(sig);
}

enum { POOLS = 3, PAGES = 64, RANGES = 24 };
static uint8_t *pool[POOLS]; static size_t pg;
static _Atomic int stop_w; static _Atomic uint64_t writes;

static void *writer(void *a) {
    uint64_t r = (uint64_t)(uintptr_t)a * 0x9E3779B97F4A7C15ull + 1;
    while (!atomic_load(&stop_w)) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        const int p = (int)(r % POOLS), page = (int)((r >> 8) % PAGES), off = (int)((r >> 20) % pg);
        if (p == 2) continue;                     // pool 2 is never written by the game: its tokens must hold for good
        ((volatile uint8_t *)pool[p])[(size_t)page * pg + (size_t)off] = (uint8_t)(r >> 40);   // a game write (faults when protected)
        atomic_fetch_add(&writes, 1);
        for (volatile int i = 0; i < 2000; i++) {}
    }
    return NULL;
}

int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL);
    pg = (size_t)getpagesize();
    for (int i = 0; i < POOLS; i++) { pool[i] = mmap(NULL, PAGES * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); memset(pool[i], i + 1, PAGES * pg); nvn_dirty_add_range(pool[i], PAGES * pg); }
    uint64_t g, inc, tok, g2, inc2, tok2;
    // ---- 1. the plain cases
    nvn_dirty_protect(pool[0] + 4 * pg, 4 * pg);
    CHECK(nvn_dirty_version_tok(pool[0] + 4 * pg + 100, 3 * pg, &g, &inc, &tok) == 1, "protected range: an answer + token");
    CHECK(nvn_dirty_tok_ok(tok), "token holds right after");
    CHECK(nvn_dirty_version_tok(pool[0] + 3 * pg, 2 * pg, &g2, &inc2, &tok2) == 0, "a range with a writable page: no answer");
    pool[1][5] = 9;                                       // another pool, not protected: nothing changes for pool 0
    CHECK(nvn_dirty_tok_ok(tok), "a write into another pool leaves the token");
    nvn_dirty_protect(pool[1], 2 * pg); pool[1][7] = 3;   // another pool's write FAULT: still not pool 0's
    CHECK(nvn_dirty_tok_ok(tok), "a write fault in another pool leaves the token");
    pool[0][6 * pg + 1] = 0x55;                           // a game write into the range: fault
    CHECK(!nvn_dirty_tok_ok(tok), "a write fault into the range makes the token stale");
    CHECK(nvn_dirty_version_tok(pool[0] + 4 * pg + 100, 3 * pg, &g2, &inc2, &tok2) == 0, "the written page is writable now: no answer");
    nvn_dirty_protect(pool[0] + 4 * pg, 4 * pg);
    CHECK(nvn_dirty_version_tok(pool[0] + 4 * pg + 100, 3 * pg, &g2, &inc2, &tok2) == 1 && g2 > g && inc2 == inc, "protected again: a newer generation, same pool");
    g_prep(pool[0] + 5 * pg, 8);                          // a host write ahead (a file read into the pool)
    CHECK(!nvn_dirty_tok_ok(tok2), "a host write ahead makes the token stale");
    nvn_dirty_protect(pool[0] + 4 * pg, 4 * pg);
    CHECK(nvn_dirty_version_tok(pool[0] + 4 * pg, 4 * pg, &g, &inc, &tok) == 1, "again an answer");
    nvn_dirty_remove_range(pool[0]);
    CHECK(!nvn_dirty_tok_ok(tok), "removing the pool makes the token stale");
    nvn_dirty_add_range(pool[0], PAGES * pg);             // the same slot, a new registration
    CHECK(!nvn_dirty_tok_ok(tok), "a new registration in the slot keeps it stale");
    nvn_dirty_protect(pool[0] + 4 * pg, 4 * pg);
    CHECK(nvn_dirty_version_tok(pool[0] + 4 * pg, 4 * pg, &g2, &inc2, &tok2) == 1 && inc2 != inc, "the new registration answers with another pool number");
    CHECK(nvn_dirty_tok_ok(tok2) && tok2 != tok, "its own token holds");
    CHECK(!nvn_dirty_tok_ok(((uint64_t)(MAX_RANGE - 1) << 52) | 1), "a token of an unused slot never holds");
    // ---- 2 + 3. the claim, against a copy of the bytes, while 3 threads write and the reader protects
    struct R { uint8_t *a; size_t n; int have; uint64_t tok, g, inc; uint8_t *copy; } rr[RANGES];
    uint64_t rng = 77;
    for (int i = 0; i < RANGES; i++) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        const int p = i % POOLS, first = (int)(rng % (PAGES - 8)), np = 1 + (int)((rng >> 16) % 6);
        rr[i].a = pool[p] + (size_t)first * pg + (rng >> 30) % 64; rr[i].n = (size_t)np * pg - 128; rr[i].have = 0; rr[i].copy = malloc(rr[i].n); }
    pthread_t th[3]; for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, writer, (void *)(uintptr_t)(i + 1));
    long held = 0, taken = 0, stale = 0, bad_bytes = 0, cmp = 0, bad_ans = 0, pool2_held = 0, pool2_lost = 0;
    const uint64_t t_end = mach_absolute_time() + 2000000000ull;
    while (mach_absolute_time() < t_end) {
        for (int i = 0; i < RANGES; i++) {
            struct R *x = &rr[i];
            if (x->have && nvn_dirty_tok_ok(x->tok)) {
                ++held; if (x->a >= pool[2] && x->a < pool[2] + PAGES * pg) ++pool2_held;
                if (memcmp(x->copy, x->a, x->n)) ++bad_bytes;    // the bytes moved while the token held: the memo would be wrong
                uint64_t fg, fi; if (nvn_dirty_version(x->a, x->n, &fg, &fi)) { ++cmp; if (fg != x->g || fi != x->inc) ++bad_ans; }
                continue;
            }
            if (x->have) { ++stale; if (x->a >= pool[2] && x->a < pool[2] + PAGES * pg) ++pool2_lost; x->have = 0; }
            uint64_t tg, ti, tt;
            if (nvn_dirty_version_tok(x->a, x->n, &tg, &ti, &tt)) { memcpy(x->copy, x->a, x->n);   // the bytes the version stands for
                if (nvn_dirty_tok_ok(tt)) { x->have = 1; x->tok = tt; x->g = tg; x->inc = ti; ++taken; } }
            else nvn_dirty_protect(x->a, x->n);                     // as the backend's rv(): protect, ask again next time
        }
    }
    atomic_store(&stop_w, 1); for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    CHECK(bad_bytes == 0, "bytes changed under a holding token: %ld of %ld checks", bad_bytes, held);
    CHECK(bad_ans == 0, "the token's answer differs from a fresh scan: %ld of %ld", bad_ans, cmp);
    CHECK(taken > 100 && stale > 100 && held > 1000, "the race really ran (tokens taken %ld, made stale %ld, held checks %ld, writes %llu)", taken, stale, held, (unsigned long long)atomic_load(&writes));
    CHECK(pool2_lost == 0 && pool2_held > 100, "a pool nobody writes keeps its tokens (held %ld, lost %ld)", pool2_held, pool2_lost);
    printf("nr_dirty_tok_test: tokens taken %ld, stale %ld, held checks %ld (pool 2: %ld), compared with a fresh scan %ld, game writes %llu\n",
           taken, stale, held, pool2_held, cmp, (unsigned long long)atomic_load(&writes));
    printf("nr_dirty_tok_test: %d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
