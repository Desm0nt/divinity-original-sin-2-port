// [NR-24] DM_NR_GATE_TICK: the guest tick domain of hle_os.c (dm_tick_read, freezes / thaws, the pause-menu hold, the 19.2 MHz
// scaling) on a counter this test drives: real CLOCK_MONOTONIC_RAW time at T_FRQ Hz plus a jump the test can add. Checks:
//   K1 init: mode 1, the first value continues guest_ns() * 12 / 625
//   K2 rate: over 200 ms the guest ticks advance 19.2 MHz x the time within 0.5 % at 19.2 / 26 / 13 / 24.576 MHz counters
//   K3 freeze: while frozen every read returns one value, it is >= every read before; the thaw continues from it (no jump, no step back)
//   K4 four threads read nonstop across 400 freeze / thaw cycles: each thread's values never go back, nobody reads past a frozen
//      value while frozen
//   K5 hold (reason 3, the pause menu): a reader waits until the release and then reads >= the frozen value
//   K6 a 2^40-tick counter jump at 26 MHz is scaled exactly (128-bit multiply), no wrap
// Build and run (WSL / Linux; the arm64 counter read is replaced by dm_test_cnt):
//   gcc -O1 -g -pthread -w -D_GNU_SOURCE -DDM_TICK_TEST -DDM_GAME_DOOM=1 -I.. -I../../android/compat/include \
//       -o t_nr24_tick t_nr24_tick.c ../hle_os.c && for f in 19200000 26000000 13000000 24576000; do T_FRQ=$f ./t_nr24_tick || break; done
#define _GNU_SOURCE
#include "../dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

// ---- stubs of the host ----
void (*dm_gate_prof)(int kind, int svc, uint64_t ticks);
uint32_t arc4random(void) { return 4; }
void arc4random_buf(void *p, size_t n) { memset(p, 4, n); }
void crash_watch_frame(void) {}
void dm_doom_tick_diag(void) {}
void dm_guest_clock_advanced(void) {}
void uimsg_subscribe(void (*f)(const char *, const char *, const char *)) { (void)f; }
// ---- the counter ----
static uint64_t g_frq = 19200000; static _Atomic uint64_t g_jump;
static uint64_t raw_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC_RAW, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t dm_test_cnt(void) { return (uint64_t)((unsigned __int128)raw_ns() * g_frq / 1000000000u) + atomic_load(&g_jump) + 0x100000000ull; }
uint64_t dm_test_frq(void) { return g_frq; }

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void usleep_(long us) { struct timespec t = {us / 1000000, (us % 1000000) * 1000}; nanosleep(&t, NULL); }

static _Atomic int k4_stop, k4_frozen; static _Atomic uint64_t k4_frozen_at, k4_reads, k4_back, k4_past;
static void *k4_reader(void *a) {
    (void)a; uint64_t last = 0;
    while (!atomic_load(&k4_stop)) {
        const int fz = atomic_load(&k4_frozen); const uint64_t fa = atomic_load(&k4_frozen_at);
        const uint64_t v = dm_tick_read();
        if (v < last) atomic_fetch_add(&k4_back, 1);
        if (fz && atomic_load(&k4_frozen) && atomic_load(&k4_frozen_at) == fa && v > fa) atomic_fetch_add(&k4_past, 1);   // read wholly inside one freeze
        last = v; atomic_fetch_add(&k4_reads, 1);
    }
    return NULL;
}
static _Atomic int k5_done; static _Atomic uint64_t k5_v;
static void *k5_reader(void *a) { (void)a; atomic_store(&k5_v, dm_tick_read()); atomic_store(&k5_done, 1); return NULL; }
uint64_t g_k4_snap;
static void alarm_h(int s) { (void)s; printf("FAIL: hang (watchdog)\n"); _exit(3); }

int main(void) {
    signal(SIGALRM, alarm_h); alarm(60); setvbuf(stdout, NULL, _IONBF, 0);
    const char *e = getenv("T_FRQ"); if (e) g_frq = strtoull(e, 0, 10);
    setenv("DM_NR_GATE_TICK", "1", 1);
    const uint64_t g0 = guest_ns() * 12 / 625;
    CHECK(dm_tick_init() == 1, "K1 mode %d", g_tick_mode);
    const uint64_t t0 = dm_tick_read();
    CHECK(t0 >= g0 && t0 - g0 < 19200 * 20, "K1 first tick %llu vs guest_ns ticks %llu (diff %lld)", (unsigned long long)t0, (unsigned long long)g0, (long long)(t0 - g0));
    // K2
    { const uint64_t a = dm_tick_read(), ra = raw_ns(); usleep_(200000); const uint64_t b = dm_tick_read(), rb = raw_ns();
      const double want = (double)(rb - ra) * 19.2e6 / 1e9, got = (double)(b - a);
      CHECK(got > want * 0.995 && got < want * 1.005, "K2 at %llu Hz: %.0f ticks over %.1f ms, want %.0f", (unsigned long long)g_frq, got, (rb - ra) / 1e6, want); }
    // K3
    { uint64_t before = dm_tick_read();
      guest_clock_freeze(1);
      const uint64_t f1 = dm_tick_read(); usleep_(30000); const uint64_t f2 = dm_tick_read();
      CHECK(f1 == f2 && f1 >= before, "K3 frozen reads %llu %llu, before %llu", (unsigned long long)f1, (unsigned long long)f2, (unsigned long long)before);
      guest_clock_freeze(0);
      const uint64_t a1 = dm_tick_read();
      CHECK(a1 >= f1 && a1 - f1 < 19200 * 5, "K3 thaw: %llu after the frozen %llu (+%lld ticks; want < 5 ms)", (unsigned long long)a1, (unsigned long long)f1, (long long)(a1 - f1)); }
    // K4
    { pthread_t th[4]; for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, k4_reader, NULL);
      for (int c = 0; c < 400; c++) {
          guest_clock_freeze(1); atomic_store(&k4_frozen_at, dm_tick_read()); atomic_store(&k4_frozen, 1); usleep_(200);
          atomic_store(&k4_frozen, 0); guest_clock_freeze(0); usleep_(300);
      }
      atomic_store(&k4_stop, 1); for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
      CHECK(atomic_load(&k4_back) == 0 && atomic_load(&k4_past) == 0, "K4 %llu reads: %llu went back, %llu read past a frozen value", (unsigned long long)k4_reads, (unsigned long long)k4_back, (unsigned long long)k4_past);
      printf("  K4: %llu reads across 400 freeze / thaw cycles\n", (unsigned long long)k4_reads); }
    // K5
    { guest_clock_freeze_for(3, 1);
      pthread_t th; pthread_create(&th, NULL, k5_reader, NULL); usleep_(30000);
      CHECK(!atomic_load(&k5_done), "K5 a read during the hold returned");
      guest_clock_freeze_for(3, 0); pthread_join(th, NULL);
      CHECK(atomic_load(&k5_done), "K5 the read did not return after the release"); }
    // K6
    if (g_frq == 26000000) {
        const uint64_t a = dm_tick_read(); atomic_fetch_add(&g_jump, 1ull << 40); const uint64_t b = dm_tick_read();
        const double want = (double)(1ull << 40) * 19.2e6 / 26e6, got = (double)(b - a);
        CHECK(got > want * (1 - 1e-9) && got < want * (1 + 1e-9) + 19.2e6 * 0.01, "K6 a 2^40 jump: %.0f ticks, want %.0f (1e-9 of the scale, 10 ms of reading)", got, want);
    }
    printf("%s (counter %llu Hz): %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", (unsigned long long)g_frq, g_checks, g_fail);
    return g_fail ? 1 : 0;
}
