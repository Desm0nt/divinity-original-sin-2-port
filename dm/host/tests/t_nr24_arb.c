// [NR-24] Multi-threaded semantics of the arbitration SVCs (hle_thread.c), the same checks on the G path (DM_NR_GATE_ARB=0) and on
// the address buckets (DM_NR_GATE_ARB=1, and =2 with the invariant checks, which must stay silent). Guest threads are real: svc 0x08
// CreateThread / 0x09 StartThread / 0x0A ExitThread through hle_thread.c, the entry runs on the host stack (dm_thread_enter below).
// The user-mode side is the SDK's protocol (nn::os InternalCriticalSection / InternalConditionVariable / LightEvent-style address waits):
// a lock word = owner handle | 0x40000000 when someone waits; a condition key that is non-zero while someone waits.
//   T1 lock: 8 threads x 20000 enter / leave around a plain counter -> exact count, lock word 0 at the end
//   T2 condition: 3 producers x 4000 items, 3 consumers, a 16-slot ring, two condition variables on one lock -> every item once
//   T3 WaitForAddress order: 6 waiters (priorities 30 10 20 10 30 20, registered in that order), 6 single SignalToAddress -> woken
//      by priority, first registered first at equal priority (Horizon's KAddressArbiter tree)
//   T4 ArbitrateLock hand-off order: 3 lock waiters (40 20 30) -> the holder's release hands to 20, then 30, then 40
//   T5 SignalProcessWideKey: count 1 wakes the best waiter; broadcast moves the rest onto the lock, which they then get in priority
//      order; the key is 0 once nobody waits
//   T6 timeouts: WaitForAddress 20 ms -> R_TIMEOUT after >= 20 ms; timeout 0 -> R_TIMEOUT at once; 0x1C 10 ms -> R_TIMEOUT, the lock is
//      not taken back by the kernel
//   T7 timeout / signal race: 6 threads wait on a condition with 30..200 us timeouts while 2 signal it nonstop; each 0 result owns the
//      lock, each R_TIMEOUT does not; a counter under the lock stays exact
//   T8 SignalToAddress values (KB HLE-13): type 1 increments if equal; type 2 with count > 0: 0 waiters +1, waiters <= count -1,
//      more than count unchanged; a mismatch -> R_BAD_STATE and no wake
//   T9 WaitForAddress types: 0 waits only if less, 1 decrements if less then waits, 2 only if equal; else R_BAD_STATE
// Build and run (WSL / Linux):
//   gcc -O1 -g -pthread -w -D_GNU_SOURCE -DDM_GAME_DOOM=1 -DDM_GAME_DOS2=1 -I.. -I../../android/compat/include \
//       -o t_nr24_arb t_nr24_arb.c t_nr24_stub.c ../hle_thread.c
//   ./t_nr24_arb && DM_NR_GATE_ARB=1 ./t_nr24_arb && DM_NR_GATE_ARB=2 ./t_nr24_arb
#include "../dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

// ---- host stubs (as t_condvar_stub.c, with a dm_thread_enter that runs the entry) ----
struct dm_flags g_flag;
uint64_t g_t0_ns;
void crash_thread_init(void) {}
void guest_thread_init(void) {}
int svc_thread(int num, uint64_t *regs);
uint32_t svc_thread_main_init(void);
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    (void)sp; ((void (*)(uint64_t))entry)(arg);
    uint64_t r[32] = {0}; svc_thread(0x0A, r); abort();
}
void dm_call_on_stack(uint64_t sp, void (*fn)(void *), void *arg) { (void)sp; fn(arg); abort(); }
void *guest_sym(const char *name) { (void)name; return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) { (void)id; (void)ns; (void)bytes; }
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
void dm_nr24_arb_stats(uint64_t out[8]);

#define WB 0x40000000u
#define R_TIMEOUT 0xEA01u
#define R_BAD_STATE 0xFA01u
static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint64_t svc(int n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) { uint64_t r[32] = {a0, a1, a2, a3}; svc_thread(n, r); return r[0]; }
static void msleep(int ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&t, NULL); }

// ---- the SDK's user-mode protocol ----
static void cs_enter(_Atomic uint32_t *w, uint32_t tag) {
    for (;;) {
        uint32_t v = 0;
        if (atomic_compare_exchange_strong(w, &v, tag)) return;
        if (!(v & WB)) { uint32_t e = v; if (!atomic_compare_exchange_strong(w, &e, v | WB)) continue; }
        svc(0x1A, v & ~WB, (uint64_t)w, tag, 0);
        if ((atomic_load(w) & ~WB) == tag) return;   // handed over
    }
}
static void cs_leave(_Atomic uint32_t *w, uint32_t tag) { uint32_t v = tag; if (!atomic_compare_exchange_strong(w, &v, 0)) svc(0x1B, (uint64_t)w, 0, 0, 0); }
static uint64_t cv_wait(_Atomic uint32_t *lock, _Atomic uint32_t *key, uint32_t tag, int64_t ns) {
    uint64_t r = svc(0x1C, (uint64_t)lock, (uint64_t)key, tag, (uint64_t)ns);
    if ((atomic_load(lock) & ~WB) != tag) cs_enter(lock, tag);   // a timeout: take the lock back in user mode
    return r;
}
static void cv_signal(_Atomic uint32_t *key, int n) { if (atomic_load(key)) svc(0x1D, (uint64_t)key, (uint64_t)(int64_t)n, 0, 0); }

// ---- guest threads ----
typedef struct { void (*fn)(int, uint32_t); int i; _Atomic uint32_t h; } targ_t;
static targ_t g_ta[64]; static _Atomic int g_live;
static void tramp(uint64_t a) { targ_t *t = (targ_t *)a; while (!atomic_load(&t->h)) { } t->fn(t->i, atomic_load(&t->h)); atomic_fetch_sub(&g_live, 1); }
static char g_stack[16];
static uint32_t spawn(int slot, void (*fn)(int, uint32_t), int i, int prio) {
    targ_t *t = &g_ta[slot]; t->fn = fn; t->i = i; atomic_store(&t->h, 0); atomic_fetch_add(&g_live, 1);
    uint64_t r[32] = {0, (uint64_t)tramp, (uint64_t)t, (uint64_t)(g_stack + 16), (uint64_t)prio, 0};
    svc_thread(0x08, r); uint32_t h = (uint32_t)r[1];
    atomic_store(&t->h, h);
    uint64_t s[32] = {h}; svc_thread(0x09, s);
    return h;
}
static int join_all(int sec) { for (int k = 0; k < sec * 1000 && atomic_load(&g_live); k++) msleep(1); return atomic_load(&g_live) == 0; }

// T1
static _Atomic uint32_t t1_lock; static uint64_t t1_count;
static void t1_body(int i, uint32_t h) { (void)i; for (int k = 0; k < 20000; k++) { cs_enter(&t1_lock, h); t1_count++; cs_leave(&t1_lock, h); } }
// T2
#define Q 16
static _Atomic uint32_t t2_lock, t2_nf, t2_ne; static int t2_q[Q], t2_head, t2_n; static uint64_t t2_sum, t2_got; static int t2_done;
static void t2_prod(int i, uint32_t h) {
    for (int k = 0; k < 4000; k++) {
        cs_enter(&t2_lock, h);
        while (t2_n == Q) cv_wait(&t2_lock, &t2_nf, h, -1);
        t2_q[(t2_head + t2_n++) % Q] = i * 100000 + k + 1;
        cv_signal(&t2_ne, 1);
        cs_leave(&t2_lock, h);
    }
}
static void t2_cons(int i, uint32_t h) {
    (void)i;
    for (;;) {
        cs_enter(&t2_lock, h);
        while (!t2_n && !t2_done) cv_wait(&t2_lock, &t2_ne, h, -1);
        if (!t2_n && t2_done) { cs_leave(&t2_lock, h); return; }
        int v = t2_q[t2_head]; t2_head = (t2_head + 1) % Q; t2_n--; t2_sum += (uint64_t)v; t2_got++;
        cv_signal(&t2_nf, 1);
        cs_leave(&t2_lock, h);
    }
}
// T3
static _Atomic uint32_t t3_word; static _Atomic int t3_seq, t3_order[8], t3_ready;
static void t3_body(int i, uint32_t h) { (void)h; atomic_fetch_add(&t3_ready, 1); uint64_t r = svc(0x34, (uint64_t)&t3_word, 2, 0, (uint64_t)-1); CHECK(r == 0, "T3 waiter %d result %#llx", i, (unsigned long long)r); t3_order[atomic_fetch_add(&t3_seq, 1)] = i; }
// T4
static _Atomic uint32_t t4_lock; static _Atomic int t4_seq, t4_order[8];
static void t4_body(int i, uint32_t h) { cs_enter(&t4_lock, h); t4_order[atomic_fetch_add(&t4_seq, 1)] = i; msleep(2); cs_leave(&t4_lock, h); }
// T5
static _Atomic uint32_t t5_lock, t5_key; static _Atomic int t5_seq, t5_order[8], t5_in;
static void t5_body(int i, uint32_t h) {
    cs_enter(&t5_lock, h); atomic_fetch_add(&t5_in, 1);
    uint64_t r = cv_wait(&t5_lock, &t5_key, h, -1);
    CHECK(r == 0, "T5 waiter %d result %#llx", i, (unsigned long long)r);
    CHECK((atomic_load(&t5_lock) & ~WB) == h, "T5 waiter %d does not own the lock on return", i);
    t5_order[atomic_fetch_add(&t5_seq, 1)] = i; msleep(2);
    cs_leave(&t5_lock, h);
}
// T7
static _Atomic uint32_t t7_lock, t7_key; static uint64_t t7_count; static _Atomic int t7_stop; static _Atomic uint64_t t7_ok, t7_tmo;
static _Atomic int t7_ph[8]; static uint32_t t7_h[8];
static void t7_wait(int i, uint32_t h) {
    t7_h[i] = h;
    for (int k = 0; k < 3000; k++) {
        atomic_store(&t7_ph[i], 1); cs_enter(&t7_lock, h); atomic_store(&t7_ph[i], 2);
        uint64_t r = svc(0x1C, (uint64_t)&t7_lock, (uint64_t)&t7_key, h, (uint64_t)(30000 + (k % 6) * 30000));
        uint32_t w = atomic_load(&t7_lock) & ~WB;
        if (r == 0) { atomic_fetch_add(&t7_ok, 1); CHECK(w == h, "T7 thread %d: 0 result without the lock (word %#x)", i, w); }
        else { atomic_fetch_add(&t7_tmo, 1); CHECK(r == R_TIMEOUT, "T7 result %#llx", (unsigned long long)r); CHECK(w != h, "T7 thread %d: a timeout that holds the lock", i); cs_enter(&t7_lock, h); }
        atomic_store(&t7_ph[i], 3); t7_count++;
        cs_leave(&t7_lock, h); atomic_store(&t7_ph[i], 4);
    }
}
static void t7_sig(int i, uint32_t h) { t7_h[6 + i] = h; while (!atomic_load(&t7_stop)) { atomic_store(&t7_ph[6 + i], 5); cs_enter(&t7_lock, h); atomic_store(&t7_ph[6 + i], 6); t7_count++; cv_signal(&t7_key, i ? -1 : 1); atomic_store(&t7_ph[6 + i], 7); cs_leave(&t7_lock, h); atomic_store(&t7_ph[6 + i], 8); } }
// T10: 4 independent pairs ping-pong through WaitForAddress / SignalToAddress (a job system's wake / wait pattern): on the G path
// every pair goes through the one lock and list, on buckets only the pair's own
static _Atomic uint32_t t10_w[8];
static void t10_body(int i, uint32_t h) {
    (void)h; _Atomic uint32_t *mine = &t10_w[i], *peer = &t10_w[i ^ 1];
    for (uint32_t k = 0; k < 20000; k++) {
        if (!(i & 1)) { atomic_store(peer, k + 1); svc(0x35, (uint64_t)peer, 0, 0, 1); }
        while (atomic_load(mine) != k + 1) svc(0x34, (uint64_t)mine, 2, k, (uint64_t)-1);
        if (i & 1) { atomic_store(peer, k + 1); svc(0x35, (uint64_t)peer, 0, 0, 1); }
    }
}
// T11 (DM_NR_GATE_YIELD): a poll + yield loop (SleepThread 0, the same callee-saved registers: no progress) waits ~1.5 ms for a flag
// another guest thread sets; with kick the setter then makes a kernel event (SignalToAddress on another word), as a job worker that
// goes back to wait does. Measured: the poller's CPU time per wait and the delay from the flag to the poller seeing it
static _Atomic uint32_t t11_go, t11_flag, t11_dummy; static _Atomic uint64_t t11_set_ns, t11_lat_ns, t11_cpu_ns; static int t11_kick;
static uint64_t thr_cpu_ns(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void t11_poll(int i, uint32_t h) {
    (void)i; (void)h;
    for (uint32_t k = 0; k < 100; k++) {
        while (atomic_load(&t11_go) != k + 1) svc(0x34, (uint64_t)&t11_go, 2, k, (uint64_t)-1);   // a blocking SVC ends the yield run
        const uint64_t c0 = thr_cpu_ns();
        while (atomic_load(&t11_flag) != k + 1) svc(0x0B, 0, 0, 0, 0);
        const uint64_t seen = now_ns();
        atomic_fetch_add(&t11_cpu_ns, thr_cpu_ns() - c0); atomic_fetch_add(&t11_lat_ns, seen - atomic_load(&t11_set_ns));
    }
}
static void t11_set(int i, uint32_t h) {
    (void)i; (void)h;
    for (uint32_t k = 0; k < 100; k++) {
        msleep(1); atomic_store(&t11_go, k + 1); svc(0x35, (uint64_t)&t11_go, 0, 0, 1);
        struct timespec t = {0, 1500000}; nanosleep(&t, NULL);
        atomic_store(&t11_set_ns, now_ns()); atomic_store(&t11_flag, k + 1);
        if (t11_kick) svc(0x35, (uint64_t)&t11_dummy, 0, 0, 1);
    }
}
// T8 / T9
static _Atomic uint32_t t8_word; static _Atomic int t8_woke;
static void t8_body(int i, uint32_t h) { (void)i; (void)h; uint64_t r = svc(0x34, (uint64_t)&t8_word, 2, 5, (uint64_t)-1); CHECK(r == 0, "T8 waiter result %#llx", (unsigned long long)r); atomic_fetch_add(&t8_woke, 1); }

static uint64_t g_t0;
#define STEP(n) printf("  %s done at %.2f s\n", n, (now_ns() - g_t0) / 1e9)
static void alarm_h(int s) { (void)s; printf("FAIL: hang (watchdog)\n"); _exit(3); }
int main(void) {
    signal(SIGALRM, alarm_h); alarm(120);
    setvbuf(stdout, NULL, _IONBF, 0);
    svc_thread_main_init(); g_t0 = now_ns();
    const char *m = getenv("DM_NR_GATE_ARB"); printf("t_nr24_arb: DM_NR_GATE_ARB=%s\n", m ? m : "0");
    // T1
    for (int i = 0; i < 8; i++) spawn(i, t1_body, i, 44);
    CHECK(join_all(60), "T1 threads did not finish");
    CHECK(t1_count == 8 * 20000, "T1 count %llu", (unsigned long long)t1_count);
    CHECK(atomic_load(&t1_lock) == 0, "T1 lock word %#x", atomic_load(&t1_lock));
    STEP("T1");
    // T2
    for (int i = 0; i < 3; i++) spawn(i, t2_prod, i, 44);
    for (int i = 0; i < 3; i++) spawn(3 + i, t2_cons, i, 44);
    for (int k = 0; k < 60000 && atomic_load(&g_live) > 3; k++) msleep(1);   // the producers are done
    { uint32_t me = 0x7777; cs_enter(&t2_lock, me); t2_done = 1; cs_leave(&t2_lock, me); }
    { uint64_t r[32] = {(uint64_t)&t2_ne, (uint64_t)-1}; if (atomic_load(&t2_ne)) svc_thread(0x1D, r); }
    CHECK(join_all(60), "T2 threads did not finish");
    uint64_t want = 0; for (int i = 0; i < 3; i++) for (int k = 0; k < 4000; k++) want += (uint64_t)(i * 100000 + k + 1);
    CHECK(t2_got == 12000 && t2_sum == want, "T2 got %llu items, sum %llu (want 12000, %llu)", (unsigned long long)t2_got, (unsigned long long)t2_sum, (unsigned long long)want);
    CHECK(atomic_load(&t2_ne) == 0 && atomic_load(&t2_nf) == 0, "T2 keys %u %u", atomic_load(&t2_ne), atomic_load(&t2_nf));
    STEP("T2");
    // T3
    static const int p3[6] = {30, 10, 20, 10, 30, 20};
    for (int i = 0; i < 6; i++) { spawn(i, t3_body, i, p3[i]); while (atomic_load(&t3_ready) <= i) { } msleep(15); }
    for (int i = 0; i < 6; i++) { uint64_t r = svc(0x35, (uint64_t)&t3_word, 0, 0, 1); CHECK(r == 0, "T3 signal %#llx", (unsigned long long)r); msleep(10); }
    CHECK(join_all(10), "T3 threads did not finish");
    static const int e3[6] = {1, 3, 2, 5, 0, 4};
    for (int i = 0; i < 6; i++) CHECK(t3_order[i] == e3[i], "T3 wake %d: waiter %d, want %d (priority, then first registered)", i, t3_order[i], e3[i]);
    STEP("T3");
    // T4
    { uint32_t me = 0x7001; cs_enter(&t4_lock, me);
      static const int p4[3] = {40, 20, 30};
      for (int i = 0; i < 3; i++) { spawn(i, t4_body, i, p4[i]); msleep(15); }
      CHECK((atomic_load(&t4_lock) & WB) && (atomic_load(&t4_lock) & ~WB) == me, "T4 lock word %#x", atomic_load(&t4_lock));
      cs_leave(&t4_lock, me); }
    CHECK(join_all(10), "T4 threads did not finish");
    CHECK(t4_order[0] == 1 && t4_order[1] == 2 && t4_order[2] == 0, "T4 order %d %d %d (want 1 2 0)", t4_order[0], t4_order[1], t4_order[2]);
    CHECK(atomic_load(&t4_lock) == 0, "T4 lock word at the end %#x", atomic_load(&t4_lock));
    STEP("T4");
    // T5
    static const int p5[4] = {30, 10, 40, 20};
    for (int i = 0; i < 4; i++) { spawn(i, t5_body, i, p5[i]); while (atomic_load(&t5_in) <= i) { } msleep(15); }
    { uint32_t me = 0x7002; cs_enter(&t5_lock, me);
      CHECK(atomic_load(&t5_key) == 1, "T5 key %u with waiters", atomic_load(&t5_key));
      cv_signal(&t5_key, 1); cs_leave(&t5_lock, me); msleep(20);
      CHECK(atomic_load(&t5_seq) == 1 && t5_order[0] == 1, "T5 signal one: %d woke, first %d (want 1, waiter 1)", atomic_load(&t5_seq), t5_order[0]);
      cs_enter(&t5_lock, me); cv_signal(&t5_key, -1);
      CHECK(atomic_load(&t5_key) == 0, "T5 key %u after the broadcast", atomic_load(&t5_key));
      msleep(5); cs_leave(&t5_lock, me); }
    CHECK(join_all(10), "T5 threads did not finish");
    CHECK(t5_order[1] == 3 && t5_order[2] == 0 && t5_order[3] == 2, "T5 broadcast order %d %d %d (want 3 0 2: by priority through the lock)", t5_order[1], t5_order[2], t5_order[3]);
    STEP("T5");
    // T6
    { static _Atomic uint32_t w6, l6, k6; uint32_t me = 0x7003; uint64_t a = now_ns();
      uint64_t r = svc(0x34, (uint64_t)&w6, 2, 0, 20000000); uint64_t d = now_ns() - a;
      CHECK(r == R_TIMEOUT && d >= 19000000 && d < 500000000, "T6 WaitForAddress 20 ms: %#llx after %.1f ms", (unsigned long long)r, d / 1e6);
      a = now_ns(); r = svc(0x34, (uint64_t)&w6, 2, 0, 0); d = now_ns() - a;
      CHECK(r == R_TIMEOUT && d < 5000000, "T6 timeout 0: %#llx after %.2f ms", (unsigned long long)r, d / 1e6);
      cs_enter(&l6, me); a = now_ns(); r = svc(0x1C, (uint64_t)&l6, (uint64_t)&k6, me, 10000000); d = now_ns() - a;
      CHECK(r == R_TIMEOUT && d >= 9000000, "T6 0x1C 10 ms: %#llx after %.1f ms", (unsigned long long)r, d / 1e6);
      CHECK(atomic_load(&l6) == 0, "T6 the lock word after a 0x1C timeout %#x (released, not taken back)", atomic_load(&l6));
      r = svc(0x1C, (uint64_t)&l6, (uint64_t)&k6, me, 0);
      CHECK(r == R_TIMEOUT && atomic_load(&k6) == 1, "T6 0x1C timeout 0: %#llx key %u", (unsigned long long)r, atomic_load(&k6)); }
    STEP("T6");
    // T7
    for (int i = 0; i < 6; i++) spawn(i, t7_wait, i, 40 + i);
    spawn(6, t7_sig, 0, 50); spawn(7, t7_sig, 1, 50);   // below every waiter: a same-core handoff to a nonstop signaller would starve the waker (Horizon priorities, DOOM handoff)
    for (int k = 0; atomic_load(&g_live) > 2; k++) { msleep(5); if (getenv("T7_TRACE") && k % 200 == 0) { printf("  T7 %.1f s: %llu ok %llu timeouts; lock %#x key %u;", (now_ns() - g_t0) / 1e9, (unsigned long long)t7_ok, (unsigned long long)t7_tmo, atomic_load(&t7_lock), atomic_load(&t7_key));
          for (int q = 0; q < 8; q++) printf(" %x:%d", t7_h[q], atomic_load(&t7_ph[q])); printf("\n"); } }
    atomic_store(&t7_stop, 1);
    CHECK(join_all(30), "T7 threads did not finish");
    CHECK(atomic_load(&t7_ok) + atomic_load(&t7_tmo) == 18000, "T7 waits %llu + %llu", (unsigned long long)t7_ok, (unsigned long long)t7_tmo);
    CHECK(atomic_load(&t7_lock) == 0, "T7 lock word at the end %#x", atomic_load(&t7_lock));
    printf("  T7: %llu waits got the lock, %llu timed out, %llu counted\n", (unsigned long long)t7_ok, (unsigned long long)t7_tmo, (unsigned long long)t7_count);
    // T8: SignalToAddress values
    { uint64_t r;
      atomic_store(&t8_word, 7); r = svc(0x35, (uint64_t)&t8_word, 1, 7, 1); CHECK(r == 0 && atomic_load(&t8_word) == 8, "T8 type 1 equal: %#llx -> %u", (unsigned long long)r, atomic_load(&t8_word));
      r = svc(0x35, (uint64_t)&t8_word, 1, 7, 1); CHECK(r == R_BAD_STATE && atomic_load(&t8_word) == 8, "T8 type 1 not equal: %#llx -> %u", (unsigned long long)r, atomic_load(&t8_word));
      atomic_store(&t8_word, 5); r = svc(0x35, (uint64_t)&t8_word, 2, 5, 2); CHECK(r == 0 && atomic_load(&t8_word) == 6, "T8 type 2 no waiter: %#llx -> %u (want 6)", (unsigned long long)r, atomic_load(&t8_word));
      atomic_store(&t8_word, 5); for (int i = 0; i < 3; i++) spawn(i, t8_body, i, 44); msleep(30);
      atomic_store(&t8_word, 5); r = svc(0x35, (uint64_t)&t8_word, 2, 5, 2); msleep(20);
      CHECK(r == 0 && atomic_load(&t8_word) == 5 && atomic_load(&t8_woke) == 2, "T8 type 2, 3 waiters > count 2: %#llx -> %u, %d woke (want 5, 2)", (unsigned long long)r, atomic_load(&t8_word), atomic_load(&t8_woke));
      atomic_store(&t8_word, 5); r = svc(0x35, (uint64_t)&t8_word, 2, 5, 2); msleep(20);
      CHECK(r == 0 && atomic_load(&t8_word) == 4 && atomic_load(&t8_woke) == 3, "T8 type 2, 1 waiter <= count 2: %#llx -> %u, %d woke (want 4, 3)", (unsigned long long)r, atomic_load(&t8_word), atomic_load(&t8_woke));
      CHECK(join_all(10), "T8 threads did not finish");
      r = svc(0x35, (uint64_t)&t8_word, 2, 9, 1); CHECK(r == R_BAD_STATE && atomic_load(&t8_word) == 4, "T8 type 2 mismatch: %#llx -> %u", (unsigned long long)r, atomic_load(&t8_word)); }
    // T9: WaitForAddress types (timeouts 1 ms where it would wait)
    { static _Atomic uint32_t w9; uint64_t r;
      atomic_store(&w9, 3); r = svc(0x34, (uint64_t)&w9, 0, 3, 1000000); CHECK(r == R_BAD_STATE, "T9 type 0, 3 < 3 false: %#llx", (unsigned long long)r);
      r = svc(0x34, (uint64_t)&w9, 0, 4, 1000000); CHECK(r == R_TIMEOUT, "T9 type 0, 3 < 4: %#llx", (unsigned long long)r);
      r = svc(0x34, (uint64_t)&w9, 1, 4, 1000000); CHECK(r == R_TIMEOUT && atomic_load(&w9) == 2, "T9 type 1, 3 < 4: %#llx, word %u (want 2)", (unsigned long long)r, atomic_load(&w9));
      r = svc(0x34, (uint64_t)&w9, 1, 2, 1000000); CHECK(r == R_BAD_STATE && atomic_load(&w9) == 2, "T9 type 1, 2 < 2 false: %#llx, word %u", (unsigned long long)r, atomic_load(&w9));
      r = svc(0x34, (uint64_t)&w9, 2, 2, 1000000); CHECK(r == R_TIMEOUT, "T9 type 2 equal: %#llx", (unsigned long long)r);
      r = svc(0x34, (uint64_t)&w9, 2, 1, 1000000); CHECK(r == R_BAD_STATE, "T9 type 2 not equal: %#llx", (unsigned long long)r); }
    STEP("T9");
    { uint64_t a = now_ns(); for (int i = 0; i < 8; i++) spawn(i, t10_body, i, 44);
      CHECK(join_all(60), "T10 threads did not finish");
      for (int i = 0; i < 8; i++) CHECK(atomic_load(&t10_w[i]) == 20000, "T10 word %d = %u", i, atomic_load(&t10_w[i]));
      printf("  T10: 4 pairs x 20000 address ping-pongs in %.2f s\n", (now_ns() - a) / 1e9); }
    STEP("T10");
    { const char *y = getenv("DM_NR_GATE_YIELD"); const int ym = y ? atoi(y) : 0;
      for (t11_kick = 1; t11_kick >= 0; t11_kick--) {
          atomic_store(&t11_go, 0); atomic_store(&t11_flag, 0); atomic_store(&t11_lat_ns, 0); atomic_store(&t11_cpu_ns, 0);
          spawn(0, t11_poll, 0, 44); spawn(1, t11_set, 0, 44);
          CHECK(join_all(30), "T11 threads did not finish");
          const double cpu = atomic_load(&t11_cpu_ns) / 100 / 1e3, lat = atomic_load(&t11_lat_ns) / 100 / 1e3;
          printf("  T11 (DM_NR_GATE_YIELD=%d, %s): the poller's CPU %.0f us per 1.5 ms wait, flag seen %.0f us after it was set\n", ym, t11_kick ? "the setter kicks" : "no kick", cpu, lat);
          if (ym == 1) { CHECK(cpu < 600, "T11 yield waits: %.0f us CPU per wait (want < 600 of 1500)", cpu);
                         CHECK(lat < (t11_kick ? 300 : 600), "T11 yield waits: the flag seen %.0f us late", lat); }
      } }
    uint64_t st[8]; dm_nr24_arb_stats(st);
    if (st[0]) { printf("  buckets: %llu takes, %llu wakes (%llu futex wakes), %llu moved onto a lock\n", (unsigned long long)st[1], (unsigned long long)st[3], (unsigned long long)st[4], (unsigned long long)st[5]);
                 CHECK(st[6] == 0, "DM_NR_GATE_ARB=2 invariant red lines: %llu", (unsigned long long)st[6]); }
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
