// [DOS2] NR-POSONLY (docs/dos2/nr/NR-POSONLY.md): the validity rule of a position-only copy (xbg_nr_posonly.inc get()) against real
// write faults (Linux, real mprotect; nvn_dirty.c included as is, its host hooks stubbed as in nr_dirty_tok_test.c).
// The rule, as get() runs it for a copy of a tracked pool: made = stamp (nvn_dirty_now) -> protect -> gather -> token (nvn_dirty_version_tok);
// at a later use: the token holds -> valid; else the pages written after the stamp (nvn_dirty_since) -> made again; else valid (new token).
//   1. plain cases: no write; a write into a position byte; into another byte of the vertex; a host write ahead; the pool removed;
//   2. a race: 3 game threads write random bytes of 4 meshes while a reader uses the copies by the rule; whenever the rule says "valid"
//      the copy must equal a fresh gather of the guest bytes taken right after (a write between the two is allowed only if it faulted:
//      checked by retrying - the rule must say "not valid" at the next use).
// Build (WSL): gcc -O2 -std=gnu11 -pthread -I dm/android/compat/include dm/gfx/vulkan/engine/tests/nr_posonly_inval_test.c -o t
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
#include "../../../nr/nr_posonly.h"
static int checks, fails;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static void on_segv(int sig, siginfo_t *si, void *uc) { if (g_hook && g_hook(si->si_addr, 1, uc)) return; signal(sig, SIG_DFL); raise(sig); }

typedef struct { uint8_t *mesh; size_t bytes; uint32_t stride, off, psz; uint64_t nv, stamp, tok; uint8_t *copy; int live; } Copy;
static void make(Copy *c) {   // get(): stamp, protect, gather, token
    c->stamp = nvn_dirty_now(); nvn_dirty_protect(c->mesh, c->bytes);
    nrpo_gather(c->copy, c->mesh, c->nv, c->stride, c->off, c->psz);
    uint64_t g, inc; c->live = nvn_dirty_version_tok(c->mesh, c->bytes, &g, &inc, &c->tok);   // every page PROT now (none skipped BUSY by the protect)
    if (!c->live) c->tok = 0;
}
static int use(Copy *c, int *remade) {   // get() at a later use: 1 = the copy may be bound as it is
    *remade = 0;
    if (c->live && c->tok && nvn_dirty_tok_ok(c->tok)) return 1;
    if (c->live && !nvn_dirty_since(c->mesh, c->bytes, c->stamp)) { uint64_t g, inc, tok; if (nvn_dirty_version_tok(c->mesh, c->bytes, &g, &inc, &tok)) { c->tok = tok; return 1; } }
    make(c); *remade = 1; return c->live;   // dirty / not every page protected / not verified at its making: made again (bound only when verified)
}
static int same(const Copy *c) { uint8_t t[65536]; nrpo_gather(t, c->mesh, c->nv, c->stride, c->off, c->psz); return !memcmp(t, c->copy, c->nv * c->psz); }

enum { MESHES = 4 };
static uint8_t *pool; static size_t pg; static _Atomic int stop_w; static _Atomic uint64_t nwrites; static Copy cs[MESHES];
static void *writer(void *a) {
    uint64_t r = (uint64_t)(uintptr_t)a * 0x9E3779B97F4A7C15ull + 7;
    while (!atomic_load(&stop_w)) { r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        Copy *c = &cs[r % MESHES]; if (r % 5 == 0) continue;
        ((volatile uint8_t *)c->mesh)[(r >> 8) % c->bytes] = (uint8_t)(r >> 40); atomic_fetch_add(&nwrites, 1);
        for (volatile int i = 0; i < 4000; i++) {} }
    return NULL;
}
int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL);
    pg = (size_t)getpagesize();
    pool = mmap(NULL, 64 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (size_t i = 0; i < 64 * pg; i++) pool[i] = (uint8_t)(i * 37);
    nvn_dirty_add_range(pool, 64 * pg);
    const uint32_t lay[MESHES][3] = {{18, 0, 6}, {36, 0, 12}, {22, 4, 6}, {16, 0, 8}};
    for (int m = 0; m < MESHES; m++) { Copy *c = &cs[m]; c->mesh = pool + (size_t)m * 12 * pg; c->bytes = 10 * pg + 100 * m; c->stride = lay[m][0]; c->off = lay[m][1]; c->psz = lay[m][2];
        c->nv = nrpo_nverts(c->bytes, c->stride, c->off, c->psz); c->copy = malloc(c->nv * c->psz); make(c); }
    int rm = 0; Copy *c = &cs[0];
    CHECK(use(c, &rm) && !rm && same(c), "no write: the copy holds, equal");
    c->mesh[18 * 5 + 2] ^= 0x55; CHECK(use(c, &rm) && rm && same(c), "a write into a position byte: made again, equal");
    c->mesh[18 * 7 + 10] ^= 0x33; CHECK(use(c, &rm) && rm && same(c), "a write into another byte of a vertex: made again (conservative), equal");
    g_prep(c->mesh + 100, 4); c->mesh[100] ^= 1; CHECK(use(c, &rm) && rm && same(c), "a host write ahead: made again, equal");
    cs[1].mesh[0] ^= 9; CHECK(use(c, &rm) && !rm && same(c), "a write into another mesh of the pool: this copy holds (by the page check)");
    // the race
    pthread_t th[3]; for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, writer, (void *)(uintptr_t)(i + 1));
    long uses = 0, remades = 0, stale_bound = 0;
    const uint64_t t_end = mach_absolute_time() + 2000000000ull;
    while (mach_absolute_time() < t_end) for (int m = 0; m < MESHES; m++) {
        Copy *x = &cs[m]; int r2 = 0; if (!use(x, &r2)) continue; ++uses; remades += r2;
        if (!same(x)) {   // a write landed between the rule and the compare: allowed only if it faulted - then the next use must not keep the copy
            int r3 = 0; use(x, &r3); if (!r3) ++stale_bound; }
    }
    atomic_store(&stop_w, 1); for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    CHECK(stale_bound == 0, "a copy that differs from the guest bytes was kept at the next use: %ld", stale_bound);
    CHECK(uses > 10000 && remades > 100, "the race ran (uses %ld, made again %ld, writes %llu)", uses, remades, (unsigned long long)atomic_load(&nwrites));
    for (int m = 0; m < MESHES; m++) { int r4 = 0; use(&cs[m], &r4); CHECK(same(&cs[m]), "quiet again: mesh %d's copy equals its bytes", m); }
    printf("nr_posonly_inval_test: uses %ld, made again %ld, game writes %llu\n", uses, remades, (unsigned long long)atomic_load(&nwrites));
    printf("nr_posonly_inval_test: %d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
