// [NR-CPU] DM_CPU_SELF (docs/dos2/nr/NR-CPU.md): runtime test of hle_cpu_place.c's self check on Linux (WSL, >= 8 CPUs), the Android path
// compiled for glibc (-D__ANDROID__) as port-common's t_cpu_place_rt.c. A fake Helio G99 tree (translate = cpu7 alone, main = cpu6-7),
// a translator thread and a main thread that call dm_cpu_place_translator(own tid) / dm_cpu_place_self() every 5 ms ("a frame"); the
// 2 s check of "dm-cpu" is pushed to 60 s so that only the self check can put a mask back. "The system" (this test) resets masks to ff.
//   argv[1] = 1: a reset main / translator mask is back within a few frames; three translator resets in 10 s widen its one-core mask to
//                the strong cores (cpu6-7) and it stays there; the "where" report carries the self counters; a call from another thread
//                with the translator's tid changes nothing
//   argv[1] = 2: resets are counted and logged, the masks stay as the system left them
//   argv[1] = 0: nothing is checked (no self line)
// Build (WSL): gcc -O1 -g -Wall -Wextra -pthread -D__ANDROID__ -I.. -o /tmp/t_cpu_self_rt t_cpu_self_rt.c ../hle_cpu_place.c ../hle_cpu_topo.c
//   && for m in 1 2 0; do /tmp/t_cpu_self_rt $m || exit 1; done
#define _GNU_SOURCE
#include "cpu_topo.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

void pc_note(const char *m, int on, const char *fmt, ...) { (void)m; (void)on; (void)fmt; }   // the [pc] report (not tested here)
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)
enum { T_X, T_MAIN, T_N };
static _Atomic int g_tid[T_N], g_stop, g_frames[T_N];
static void *worker(void *a) {
    const int i = (int)(intptr_t)a; atomic_store(&g_tid[i], (int)syscall(SYS_gettid));
    while (!atomic_load(&g_stop)) { if (i == T_X) dm_cpu_place_translator((int)syscall(SYS_gettid)); else dm_cpu_place_self(); atomic_fetch_add(&g_frames[i], 1); usleep(5000); }
    return NULL;
}
int thr_guest_threads(int32_t *tid, uint8_t *kind, char (*name)[32], int max) {
    if (max < 1) return 0;
    tid[0] = atomic_load(&g_tid[T_MAIN]); kind[0] = 1; snprintf(name[0], 32, "main"); return 1;
}
static uint32_t mask_of(int i) { uint64_t m[4] = {0}; if (syscall(SYS_sched_getaffinity, atomic_load(&g_tid[i]), sizeof m, m) < 0) return 0; return (uint32_t)m[0]; }
static void set_mask(int i, uint32_t v) { uint64_t m[4] = {v, 0, 0, 0}; syscall(SYS_sched_setaffinity, atomic_load(&g_tid[i]), sizeof m, m); }
static void ms(int n) { usleep((useconds_t)n * 1000); }
static char g_log[160];
static int log_has(const char *needle) {
    fflush(stderr); FILE *f = fopen(g_log, "r"); if (!f) return 0;
    char l[2048]; int hit = 0; while (!hit && fgets(l, sizeof l, f)) hit = strstr(l, needle) != NULL; fclose(f); return hit;
}
static char g_root[128];
static void put(const char *rel, const char *text) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", g_root, rel);
    for (char *s = p + strlen(g_root) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(2); } fputs(text, f); fclose(f);
}

int main(int argc, char **argv) {
    const int mode = argc > 1 ? atoi(argv[1]) : 1;
    if (sysconf(_SC_NPROCESSORS_ONLN) < 8) { printf("skipped: needs 8 online CPUs\n"); return 0; }
    snprintf(g_root, sizeof g_root, "/tmp/t_cpu_self_rt.%d", (int)getpid()); mkdir(g_root, 0755);
    for (int c = 0; c < 8; c++) {   // fake Helio G99: A55 x6 cpu0-5, A76 x2 cpu6-7
        char rel[128], v[64];
        snprintf(rel, sizeof rel, "sys/devices/system/cpu/cpu%d/cpu_capacity", c); put(rel, c >= 6 ? "1024\n" : "466\n");
        snprintf(rel, sizeof rel, "sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c); put(rel, c >= 6 ? "2200000\n" : "2000000\n");
        snprintf(rel, sizeof rel, "sys/devices/system/cpu/cpu%d/regs/identification/midr_el1", c); snprintf(v, sizeof v, "0x00000000%08x\n", c >= 6 ? 0x410fd0b1u : 0x410fd051u); put(rel, v);
    }
    char ms_[8]; snprintf(ms_, sizeof ms_, "%d", mode);
    setenv("DM_CPU_SYSFS_ROOT", g_root, 1); setenv("DM_CPU_CHECK_MS", "60000", 1); setenv("DM_CPU_REPORT_S", "2", 1); unsetenv("DM_CPU_CORES"); setenv("DM_CPU_SELF", ms_, 1);
    snprintf(g_log, sizeof g_log, "%s.log", g_root); if (!freopen(g_log, "w", stderr)) return 2; setvbuf(stderr, NULL, _IOLBF, 0);
    { uint64_t m[4] = {0xff, 0, 0, 0}; syscall(SYS_sched_setaffinity, 0, sizeof m, m); }
    pthread_t th[T_N];
    for (int i = 0; i < T_N; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    while (!atomic_load(&g_tid[T_X]) || !atomic_load(&g_tid[T_MAIN])) ms(1);
    dm_cpu_place_start();
    ms(700);   // the first pass (the translator's tid is known: "translator" kick)
    printf("DM_CPU_SELF=%d\n", mode);
    CHECK(mask_of(T_X) == 0x80 && mask_of(T_MAIN) == 0xc0, "placed: translator cpu7 (%x), main cpu6-7 (%x)", mask_of(T_X), mask_of(T_MAIN));
    CHECK(log_has(mode == 1 ? "[cpu] DM_CPU_SELF=1 (NR-CPU): the translator and the main thread check" : mode == 2 ? "[cpu] DM_CPU_SELF=2 (NR-CPU)" : "[cpu] DM_CPU_SELF=0 (NR-CPU): off"), "log: the switch line");
    // the system resets main
    set_mask(T_MAIN, 0xff); ms(60);
    if (mode == 1) CHECK(mask_of(T_MAIN) == 0xc0, "main reset to ff is back to c0 within 60 ms (%x)", mask_of(T_MAIN));
    else CHECK(mask_of(T_MAIN) == 0xff, "main reset to ff stays ff (%x): %s", mask_of(T_MAIN), mode == 2 ? "counted only" : "off");
    CHECK(mode == 0 ? !log_has("[cpu] self (") : log_has("[cpu] self (DM_CPU_SELF=") && log_has("main tid"), "log: the main reset %s", mode ? "is logged" : "is not looked at");
    set_mask(T_MAIN, 0xc0);
    // a call from this thread with the translator's tid: nothing (only the thread itself checks)
    { uint64_t m[4] = {0xff, 0, 0, 0}; syscall(SYS_sched_setaffinity, 0, sizeof m, m); }
    dm_cpu_place_translator(atomic_load(&g_tid[T_X]));
    { uint64_t m[4] = {0}; syscall(SYS_sched_getaffinity, 0, sizeof m, m); CHECK((uint32_t)m[0] == 0xff, "a caller that is not the translator keeps its own mask (%x)", (unsigned)m[0]); }
    // three translator resets
    for (int k = 0; k < 3; k++) { set_mask(T_X, 0xff); ms(40); }
    if (mode == 1) {
        CHECK(mask_of(T_X) == 0xc0, "translator: three resets in 10 s widen its mask to the strong cores c0 (%x)", mask_of(T_X));
        CHECK(log_has("one-core mask 80 was reset 3 times") && log_has("widened to c0"), "log: widened line");
        set_mask(T_X, 0xff); ms(40);
        CHECK(mask_of(T_X) == 0xc0, "translator: widened mask kept by the self check (%x)", mask_of(T_X));
    } else CHECK(mask_of(T_X) == 0xff, "translator reset stays ff (%x)", mask_of(T_X));
    ms(2200);   // a "where" report
    CHECK(mode == 0 ? !log_has("; self (DM_CPU_SELF") : log_has("; self (DM_CPU_SELF=") && log_has("checks, reset found"), "where report: self counters %s", mode ? "present" : "absent");
    if (mode == 1) CHECK(log_has("translator mask c0 (widened)"), "where report: the widened translator mask");
    atomic_store(&g_stop, 1); for (int i = 0; i < T_N; i++) pthread_join(th[i], NULL);
    printf("frames: translator %d, main %d\n", atomic_load(&g_frames[T_X]), atomic_load(&g_frames[T_MAIN]));
    if (fails) { fflush(stderr); char c[300]; snprintf(c, sizeof c, "cat %s", g_log); if (system(c)) {} }
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
