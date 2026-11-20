// [DOS2] NR-TRAFFIC round 2 (docs/dos2/nr/NR-TRAFFIC.md §9, port-common/dirty-order): the order "stamp -> protect until every page is
// read-only -> read" (nvn_dirty_stamp_protect) against the old orders, on the REAL tracker (nvn_dirty.c included, real mprotect, real
// SIGSEGV write faults through the same hook the host registers). Each staged case runs the old order (must be able to lose a write: the
// negative control) and the new one (must not). Expected values come from the bytes, not from the tracker: a write is "lost" when the copy
// differs from the memory and nvn_dirty_since(stamp) says nothing changed.
//   A  upload: read, then protect (old end of a texture upload)            - a store between read and protect
//   B  check: protect, then stamp (old check branch / untracked -> tracked) - a store that faults between the two, a later store
//   C  a page another thread holds BUSY (mid-unprotect) during the protect   - left writable with a generation the stamp covers
//   D  stress: a writer thread stores into 64 pages while the checker runs tracked checks; after a quiet check copy == memory
// Build (Linux / WSL; nvn_dirty.c needs the mach time shim of the Android build):
//   gcc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -I../../android/compat/include -pthread t_dirty_order.c -o t_dirty_order
#define _GNU_SOURCE
#define mprotect t_mprotect
#include "../nvn/nvn_dirty.c"
#undef mprotect
#include <signal.h>
#include <time.h>
#include <unistd.h>
int mprotect(void *, size_t, int);

static int (*g_hook)(void *, int, void *);
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) { g_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) { (void)fn; }
static void on_segv(int sig, siginfo_t *si, void *uc) { (void)sig; if (g_hook && g_hook(si->si_addr, 1, uc)) return; signal(SIGSEGV, SIG_DFL); }

// mprotect hook: case C parks a writer's unprotect (PROT_READ|PROT_WRITE) while the page is BUSY
static _Atomic int g_park, g_parked, g_release;
int t_mprotect(void *a, size_t n, int prot) {
    if (prot == (PROT_READ | PROT_WRITE) && atomic_exchange(&g_park, 0)) {
        atomic_store(&g_parked, 1);
        for (int i = 0; i < 20000 && !atomic_load(&g_release); i++) usleep(100);
    }
    return mprotect(a, n, prot);
}

static int fails, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { NP = 64 };
static uint8_t *g_buf; static size_t g_pg_sz, g_len;
static uint8_t g_copy[NP * 65536];
static uint8_t g_val;   // every staged store writes a new value (a store of the value already there would change nothing)
#define NV ((uint8_t)(++g_val ? g_val : ++g_val))

// the reader's three steps, per order. new: stamp, protect until all read-only, read. old_upload: read, protect, stamp. old_check: protect, stamp, read.
static uint64_t step_stamp_protect(int order, int *whole) {
    if (order == 0) return nvn_dirty_stamp_protect(g_buf, g_len, whole);
    *whole = 1; nvn_dirty_protect(g_buf, g_len); return nvn_dirty_now();
}

// A: first upload. Old: read -> [store] -> protect -> stamp. New: stamp+protect -> read -> [store faults].
static int case_a(int order) {
    memset(g_buf, 1, g_len);
    uint64_t stamp; int whole;
    if (order == 0) { stamp = step_stamp_protect(0, &whole); memcpy(g_copy, g_buf, g_len); g_buf[5 * g_pg_sz + 7] = NV; }
    else { memcpy(g_copy, g_buf, g_len); g_buf[5 * g_pg_sz + 7] = NV; stamp = step_stamp_protect(1, &whole); }
    const int differs = memcmp(g_copy, g_buf, g_len) != 0, seen = nvn_dirty_since(g_buf, g_len, stamp);
    return differs && !seen;   // 1 = lost
}
// B: tracked check. Pages protected; old: protect -> [store faults: generation g] -> stamp (>= g) -> read -> [store again: no fault].
// New: stamp -> protect -> [store faults: generation > stamp] -> read -> [store again].
static int case_b(int order) {
    int whole; uint64_t s0 = nvn_dirty_stamp_protect(g_buf, g_len, &whole); (void)s0;   // everything read-only first
    uint64_t stamp;
    if (order == 0) { stamp = nvn_dirty_stamp_protect(g_buf, g_len, &whole); g_buf[3 * g_pg_sz] = NV; }
    else { nvn_dirty_protect(g_buf, g_len); g_buf[3 * g_pg_sz] = NV; stamp = nvn_dirty_now(); }
    memcpy(g_copy, g_buf, g_len);
    g_buf[3 * g_pg_sz + 1] = NV;   // the page stayed writable: no fault
    const int differs = memcmp(g_copy, g_buf, g_len) != 0, seen = nvn_dirty_since(g_buf, g_len, stamp);
    return differs && !seen;
}
// C: a writer's fault handler holds the page BUSY (parked in its mprotect) while the reader protects.
static void *writer_c(void *arg) { (void)arg; g_buf[7 * g_pg_sz] = NV; return NULL; }
static void *releaser(void *arg) { (void)arg; usleep(300); atomic_store(&g_release, 1); return NULL; }
static int case_c(int order, int *not_whole) {
    int whole; nvn_dirty_stamp_protect(g_buf, g_len, &whole);
    atomic_store(&g_release, 0); atomic_store(&g_parked, 0); atomic_store(&g_park, 1);
    pthread_t w; pthread_create(&w, NULL, writer_c, NULL);
    while (!atomic_load(&g_parked)) usleep(50);   // the page is BUSY now
    uint64_t stamp;
    if (order == 0) { pthread_t r; pthread_create(&r, NULL, releaser, NULL); stamp = nvn_dirty_stamp_protect(g_buf, g_len, &whole); pthread_join(r, NULL); pthread_join(w, NULL); }
    else { nvn_dirty_protect(g_buf, g_len); atomic_store(&g_release, 1); pthread_join(w, NULL); stamp = nvn_dirty_now(); whole = 1; }
    memcpy(g_copy, g_buf, g_len);
    g_buf[7 * g_pg_sz + 1] = NV;   // a later store: faults only if the page is read-only again
    *not_whole = !whole;
    const int differs = memcmp(g_copy, g_buf, g_len) != 0, seen = nvn_dirty_since(g_buf, g_len, stamp);
    return differs && !seen && whole;   // with whole = 0 the caller re-checks / does not cache (counted apart)
}
// D: stress, the tracked-check loop as the engine runs it (copy only pages dirty since the old stamp)
static _Atomic int g_stop;
static void *writer_d(void *arg) {
    uint64_t x = (uint64_t)(uintptr_t)arg | 1;
    while (!atomic_load(&g_stop)) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; g_buf[x % g_len] = (uint8_t)(x >> 32); if (!(x & 63)) sched_yield(); }
    return NULL;
}
static void copy_dirty(uint64_t since) { for (size_t p = 0; p < NP; p++) if (nvn_dirty_since(g_buf + p * g_pg_sz, g_pg_sz, since)) memcpy(g_copy + p * g_pg_sz, g_buf + p * g_pg_sz, g_pg_sz); }
static int case_d(int order, int ms) {
    int whole; uint64_t stamp = step_stamp_protect(order, &whole); memcpy(g_copy, g_buf, g_len);
    atomic_store(&g_stop, 0); pthread_t w[2]; for (int i = 0; i < 2; i++) pthread_create(&w[i], NULL, writer_d, (void *)(uintptr_t)(0x9e3779b97f4a7c15ull * (i + 1)));
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        if (nvn_dirty_since(g_buf, g_len, stamp)) { const uint64_t old = stamp; stamp = step_stamp_protect(order, &whole); copy_dirty(old); if (!whole) stamp = old; }
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 < ms);
    atomic_store(&g_stop, 1); for (int i = 0; i < 2; i++) pthread_join(w[i], NULL);
    // quiet check (nothing writes any more): one more check, then the copy must equal the memory
    if (nvn_dirty_since(g_buf, g_len, stamp)) { const uint64_t old = stamp; stamp = step_stamp_protect(order, &whole); copy_dirty(old); }
    return memcmp(g_copy, g_buf, g_len) != 0;
}

int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL);
    g_pg_sz = (size_t)getpagesize(); g_len = NP * g_pg_sz;
    g_buf = mmap(NULL, g_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    nvn_dirty_add_range(g_buf, g_len);
    CHECK(nvn_dirty_tracked(g_buf, g_len), "range tracked");
    CHECK(nvn_dirty_safe_order() == 1, "DM_DIRTY_PROTECT_ORDER default 1");
    int la_new = 0, la_old = 0, lb_new = 0, lb_old = 0, lc_new = 0, lc_old = 0, nw = 0, nw_old = 0;
    for (int i = 0; i < 20; i++) { la_new += case_a(0); la_old += case_a(1); lb_new += case_b(0); lb_old += case_b(1); int x; lc_new += case_c(0, &x); nw += x; lc_old += case_c(1, &x); nw_old += x; }
    printf("A upload (read vs protect):      new order lost %d / 20, old order lost %d / 20\n", la_new, la_old);
    printf("B check (protect vs stamp):      new order lost %d / 20, old order lost %d / 20\n", lb_new, lb_old);
    printf("C busy page during the protect:  new order lost %d / 20 (%d not wholly read-only -> re-check), old order lost %d / 20\n", lc_new, nw, lc_old);
    CHECK(la_new == 0 && lb_new == 0 && lc_new == 0, "the new order loses no write");
    CHECK(la_old == 20 && lb_old == 20 && lc_old == 20, "negative controls: each old order loses the staged write every time");
    int d_new = 0, d_old = 0;
    for (int i = 0; i < 6; i++) { d_new += case_d(0, 150); d_old += case_d(1, 150); }
    uint64_t c, r, n; nvn_dirty_order_stats(&c, &r, &n); uint64_t f, p; nvn_dirty_stats(&f, &p);
    printf("D stress (2 writers, 64 pages, 6 x 150 ms): new order stale %d / 6, old order stale %d / 6 (racy: may be 0); faults %llu, stamp+protect %llu, repeated rounds %llu, never whole %llu\n",
           d_new, d_old, (unsigned long long)f, (unsigned long long)c, (unsigned long long)r, (unsigned long long)n);
    CHECK(d_new == 0, "stress: the new order leaves no stale page");
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
