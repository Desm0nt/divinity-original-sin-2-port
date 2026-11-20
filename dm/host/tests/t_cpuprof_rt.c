// [NR-16] runtime test of hle_cpuprof.c (DM_NR_CPU=1) on a real Linux kernel: the sampler thread reads /proc for a thread that is busy
// half of every 10 ms "frame" (expected 5 ms a frame), counts its gates (100 clock + 100 SleepThread a frame), reads the G totals and
// the census through stubs of the host, and prints every line kind in ~11 s; the test checks the lines it captures.
// Build (Linux / WSL; not the Mac, not Windows): gcc -O1 -g -Wall -pthread t_cpuprof_rt.c -o t_cpuprof_rt && ./t_cpuprof_rt
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
typedef struct { char name[32]; uint8_t *base; } module_t;
static char g_out[64][4096]; static _Atomic int g_nout;
#define LOG(...) do { int k_ = atomic_fetch_add(&g_nout, 1); if (k_ < 64) snprintf(g_out[k_], sizeof g_out[k_], __VA_ARGS__); fprintf(stderr, "[dm] " __VA_ARGS__); fputc(10, stderr); } while (0)
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t guest_ns(void) { return now_ns(); }
module_t *module_of(const void *p) { (void)p; return NULL; }
void (*dm_gate_prof)(int kind, int svc, uint64_t ticks);
static uint64_t g_probe_ret; void gate_register(uint64_t a, const char *k, int arg, uint64_t o) { (void)arg; (void)o; if (!strcmp(k, "probe")) g_probe_ret = a; }
int g_tick_mode; char dm_gate_probe_clk_ret[4], dm_gate_probe_fast_ret[4]; void dm_gate_probe_clk(void) {} void dm_gate_probe_fast(void) {}   // [NR-24]
char dm_gate_probe_ret[4];
void dm_gate_probe(void) { if (dm_gate_prof) dm_gate_prof(3, 0, 0); }
static _Atomic uint64_t g_takes;
void dm_gstat_read(uint64_t out[5]) { uint64_t t = atomic_load(&g_takes); out[0] = t; out[1] = t / 50; out[2] = t * 100; out[3] = 5000; out[4] = t * 300; }
static _Atomic int g_busy_tid;
int dm_thr_census(int32_t *tid, uint32_t *handle, int32_t *prio, int32_t *core, uint64_t *mask, uint64_t *entry, char (*name)[32], int max) {
    (void)max; tid[0] = getpid(); handle[0] = 0x1001; prio[0] = 44; core[0] = 0; mask[0] = 1; entry[0] = 0; strcpy(name[0], "main");
    tid[1] = atomic_load(&g_busy_tid); handle[1] = 0x1002; prio[1] = 20; core[1] = 1; mask[1] = 2; entry[1] = 0x1234; strcpy(name[1], "JobWorker 3"); return 2; }
static _Atomic uint64_t g_frames;
uint64_t dm_nvn_frame_count(void) { return atomic_load(&g_frames); }
void dm_nvn_cpuprof_tids(int *p, int *s) { *p = getpid(); *s = atomic_load(&g_busy_tid); }
#define DM_CPUPROF_RT_TEST 1
#include "../hle_cpuprof.c"
static void *busy(void *a) {
    (void)a; atomic_store(&g_busy_tid, (int)gettid()); pthread_setname_np(pthread_self(), "busyjob");
    volatile double x = 0;
    for (;;) {
        uint64_t t = now_ns(); while (now_ns() - t < 5000000) x += 1.0;
        if (dm_gate_prof) for (int k = 0; k < 100; k++) { dm_gate_prof(1, 0, 0); dm_gate_prof(0, 0x0B, 1000); }
        struct timespec ts = {0, 5000000}; nanosleep(&ts, 0);
    }
    return 0;
}
static int has(const char *s) { for (int k = 0; k < atomic_load(&g_nout) && k < 64; k++) if (strstr(g_out[k], s)) return 1; return 0; }
int main(void) {
    setenv("DM_NR_CPU", "1", 1); setenv("DM_NR_CPU_MS", "100", 1);
    pthread_t th; pthread_create(&th, 0, busy, 0);
    while (!atomic_load(&g_busy_tid)) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
    dm_cpuprof_present();
    for (int i = 0; i < 1150; i++) { atomic_fetch_add(&g_frames, 1); atomic_fetch_add(&g_takes, 600); struct timespec ts = {0, 10000000}; nanosleep(&ts, 0); }
    int fails = 0;
    const char *want[] = {"[cpuprof] DM_NR_CPU=1: sampler", "[cpuprof] cpus ", "[cpuprof] gate probe: ", "[cpuprof] threads (", "[cpuprof] cores (",
                          "[cpuprof] hle (", "[cpuprof] hle svc (", "[cpuprof] roles (", "[cpuprof] nr24 probe (", "[cpuprof] nr24 ("};
    for (unsigned w = 0; w < sizeof want / sizeof *want; w++) if (!has(want[w])) { printf("FAIL: no line %s\n", want[w]); fails++; }
    for (int k = 0; k < atomic_load(&g_nout) && k < 64; k++) {
        const char *l = g_out[k], *t = strstr(l, "rec,g:JobWorker 3(p20) ");
        if (strstr(l, "[cpuprof] threads (")) {
            int tid = 0; double ms = 0;
            if (!t || sscanf(t, "rec,g:JobWorker 3(p20) %d %lf", &tid, &ms) != 2 || ms < 3.5 || ms > 6.5) { printf("FAIL: busy thread %.2f ms a frame (5 expected): %s\n", ms, l); fails++; }
            if (!strstr(l, "pres,main,g:main(p44) ")) { printf("FAIL: main role: %s\n", l); fails++; }
        }
        if (strstr(l, "[cpuprof] hle (") && !strstr(l, "gates 200 a frame (svc 100, clock 100, obs 0)")) { printf("FAIL: gates: %s\n", l); fails++; }
        if (strstr(l, "[cpuprof] hle svc (") && (!strstr(l, "0x0b SleepThread* 100") || !strstr(l, "G: 60"))) { printf("FAIL: svc / G: %s\n", l); fails++; }
    }
    printf("%s: t_cpuprof_rt (%d lines)\n", fails ? "FAIL" : "PASS", atomic_load(&g_nout));
    return fails != 0;
}
