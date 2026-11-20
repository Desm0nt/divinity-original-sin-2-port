// [NR-16] DM_NR_CPU: where the CPU goes, for one measuring run on the phone (docs/dos2/nr/NR-16.md).
//   DM_NR_CPU      0 / unset  nothing: no thread, the gate hook stays null, G is not counted (hle_thread.c), the NVN tap stores nothing
//                  1          from the first present on: the sampler thread dm-cpuprof reads, every DM_NR_CPU_MS (200) ms, each thread's
//                             /proc/self/task/<tid>/schedstat (ns on a cpu, ns runnable on a run queue) and, for a thread that ran, the
//                             cpu it ran on (stat field 39), /proc/stat (busy of every cpu) and each cpufreq policy's current / max
//                             frequency; every gate (svc / clock / obs) is counted per thread and every SVC timed (hle.c dm_gate_prof);
//                             the global lock G is counted (hle_thread.c gs_*); one gate round trip is measured through a probe gate.
//   DM_NR_CPU_MS   50..2000 (200): the sampling period.
// Lines every 10 s (first after 10 s): "[cpuprof] threads" (CPU and run-queue wait a frame by thread, with roles, and where it ran by
// cluster), "[cpuprof] cores" (cluster busy and frequency), "[cpuprof] hle" (gates a frame by kind and thread, SVCs by count and time,
// G); every 30 s "[cpuprof] roles" (every guest thread: tid, guest name, priority, core, mask, entry). Once: "[cpuprof] cpus", "[cpuprof]
// gate probe". tools/dos2_cpuprof.py turns the lines into the answers for NR-17 / 21 / 23 / 24 and the MX / present-thread A/B.
// Cost: the sampler runs on its own (unpinned, nice 10) thread, ~0.5 ms of CPU a sample; on the game's threads ~10-20 ns a gate
// (a TLS load, two adds, two cntvct reads around an SVC) and a trylock + two cntvct reads per take of G.
#ifndef DM_CPUPROF_RT_TEST   // tests/t_cpuprof_rt.c brings its own stubs of the host
#include "dm.h"
#endif
#include "cpuprof.h"
#include <stdatomic.h>
#include <time.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <dirent.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

extern void (*dm_gate_prof)(int kind, int svc, uint64_t ticks);   // hle.c
void gate_register(uint64_t ret_abs, const char *kind, int arg, uint64_t orig);   // hle.c
extern void dm_gate_probe(void); extern char dm_gate_probe_ret[];   // entry.S
void dm_gstat_read(uint64_t out[5]);                                // hle_thread.c
int dm_thr_census(int32_t *tid, uint32_t *handle, int32_t *prio, int32_t *core, uint64_t *mask, uint64_t *entry, char (*name)[32], int max);   // hle_thread.c
extern void dm_gate_probe_clk(void), dm_gate_probe_fast(void); extern char dm_gate_probe_clk_ret[], dm_gate_probe_fast_ret[];   // [NR-24] entry.S
__attribute__((weak)) void dm_nr24_arb_stats(uint64_t out[8]);                       // [NR-24] hle_thread_arb.inc
__attribute__((weak)) void dm_nr24_yield_stats(uint64_t out[8]);
__attribute__((weak)) uint64_t dm_tick_fast_count(void);                              // [NR-24] hle.c: fast clock reads (entry.S)
__attribute__((weak)) uint64_t dm_tick_fn_calls(void);                                // [NR-24] hle_os.c: main's GetSystemTick on the host
__attribute__((weak)) uint64_t dm_nvn_frame_count(void);                          // hle_nvn.c
__attribute__((weak)) void dm_nvn_cpuprof_tids(int *present_tid, int *submit_tid);  // hle_nvn.c (nvn_cpuprof.inc)

static inline uint64_t cp_ticks(void) {
#if defined(__aarch64__)
    uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v;
#else
    return now_ns();
#endif
}
static double cp_ns_tick(void) {
#if defined(__aarch64__)
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f ? 1e9 / (double)f : 1.0;
#else
    return 1.0;
#endif
}

// ---------- gates: per-thread counters, written by their own thread only ----------
#define CP_MAXT 384
typedef struct { int tid; _Atomic int live; uint64_t n[4]; uint64_t svc_n[256], svc_t[256]; uint64_t l_n[4], l_sn[256], l_st[256]; } gst_t;   // l_*: the sampler's last readings
static gst_t *g_gst[CP_MAXT]; static _Atomic int g_ngst; static pthread_mutex_t g_gst_mu = PTHREAD_MUTEX_INITIALIZER;
// A thread that exited leaves its counters; the sampler, after taking its last deltas, zeroes them and marks the entry free (live 0):
// a new thread takes a free entry before a new one (the game starts short-lived threads; without this the table would fill up)
static __thread gst_t *t_gst; static __thread int t_gst_full;
static void gate_prof(int kind, int svc, uint64_t ticks) {
    gst_t *g = t_gst;
    if (__builtin_expect(!g, 0)) {
        if (t_gst_full) return;
        pthread_mutex_lock(&g_gst_mu);
        int n = atomic_load(&g_ngst);
        for (int i = 0; i < n && !g; i++) if (!atomic_load(&g_gst[i]->live)) g = g_gst[i];   // a dead thread's entry, zeroed by the sampler
        if (!g && n < CP_MAXT && (g = calloc(1, sizeof *g))) { g_gst[n] = g; atomic_store(&g_ngst, n + 1); }
        if (g) { g->tid = (int)gettid(); atomic_store(&g->live, 1); }
        pthread_mutex_unlock(&g_gst_mu);
        if (!g) { t_gst_full = 1; return; }
        t_gst = g;
    }
    g->n[kind & 3]++;
    if (kind == 0) { g->svc_n[svc & 255]++; g->svc_t[svc & 255] += ticks; }
}

// ---------- threads ----------
typedef struct {
    int tid, fs, fq, alive; char comm[20];
    uint64_t run, wait, jif;                   // last readings
    uint64_t w_run, w_wait, w_cl[4];           // this window
} task_t;
static task_t g_task[CP_MAXT]; static int g_ntask;
static int g_ncpu, g_ncl, g_cl_of[64], g_cl_first[4], g_cl_count[4]; static unsigned g_cl_cap[4];
static uint64_t g_cpu_busy0[64], g_cpu_tot0[64], g_cpu_wbusy[64], g_cpu_wtot[64];
static int g_pol_n, g_pol_cpu[16]; static int g_pol_fcur[16], g_pol_fmax[16]; static uint64_t g_pol_cur_sum[16], g_pol_max_sum[16], g_pol_samples;
static int g_have_sched = -1, g_ms = 200, g_have_procstat;   // /proc/stat: apps may not read it (SELinux, Android 8+): busy shown as "-"
static long g_hz = 100;

static int rd_fd(int fd, char *b, size_t n) { if (fd < 0) return -1; ssize_t r = pread(fd, b, n - 1, 0); if (r <= 0) return -1; b[r] = 0; return (int)r; }
static int rd_path(const char *p, char *b, size_t n) { int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1; int r = rd_fd(fd, b, n); close(fd); return r; }

static void topo_init(void) {
    long n = sysconf(_SC_NPROCESSORS_CONF); g_ncpu = n < 1 ? 1 : n > 64 ? 64 : (int)n;
    unsigned cap[64]; char p[128], b[64];
    for (int c = 0; c < g_ncpu; c++) {
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
        if (rd_path(p, b, sizeof b) > 0) cap[c] = (unsigned)strtoul(b, NULL, 10);
        else { snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c); cap[c] = rd_path(p, b, sizeof b) > 0 ? (unsigned)strtoul(b, NULL, 10) : 0; }
    }
    g_ncl = cpf_clusters(cap, g_ncpu, g_cl_of, g_cl_cap, g_cl_first, g_cl_count, 4);
    // cpufreq policies: the first cpu of each cluster's policy
    for (int c = 0; c < g_ncpu && g_pol_n < 16; c++) {
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", c);
        int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) continue;
        g_pol_cpu[g_pol_n] = c; g_pol_fcur[g_pol_n] = fd;
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", c); g_pol_fmax[g_pol_n] = open(p, O_RDONLY | O_CLOEXEC);
        g_pol_n++;
    }
    char line[600]; size_t o = (size_t)snprintf(line, sizeof line, "[cpuprof] cpus %d:", g_ncpu);
    for (int k = 0; k < g_ncl; k++) o += (size_t)snprintf(line + o, sizeof line - o, " %c = cpu%d-%d (%d, capacity %u);", cpf_cluster_letter(k, g_ncl), g_cl_first[k], g_cl_first[k] + g_cl_count[k] - 1, g_cl_count[k], g_cl_cap[k]);
    snprintf(line + o, sizeof line - o, " %d cpufreq policies; sampled every %d ms", g_pol_n, g_ms);
    LOG("%s", line);
}
static void task_dead(task_t *t) { t->alive = 0; if (t->fs >= 0) close(t->fs); if (t->fq >= 0) close(t->fq); t->fs = t->fq = -1; }
static task_t *task_find(int tid) { for (int i = 0; i < g_ntask; i++) if (g_task[i].tid == tid) return &g_task[i]; return NULL; }
static void tasks_rescan(void) {
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    static uint8_t seen[CP_MAXT]; memset(seen, 0, sizeof seen);
    struct dirent *e; char p[96], b[64];
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name); if (tid <= 0) continue;
        task_t *t = task_find(tid);
        if (!t) {
            int slot = -1;
            for (int i = 0; i < g_ntask; i++) if (!g_task[i].alive && !g_task[i].w_run) { slot = i; break; }   // reuse a dead thread's slot once its time is reported
            if (slot < 0) { if (g_ntask == CP_MAXT) continue; slot = g_ntask++; }
            t = &g_task[slot]; if (t->tid && t->alive) task_dead(t);
            memset(t, 0, sizeof *t); t->tid = tid; t->fs = t->fq = -1;
            snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid); t->fs = open(p, O_RDONLY | O_CLOEXEC);
            snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid); t->fq = open(p, O_RDONLY | O_CLOEXEC);
            if (g_have_sched < 0) g_have_sched = t->fq >= 0;
            char sb[600];
            if (t->fq >= 0 && rd_fd(t->fq, sb, sizeof sb) > 0) cpf_parse_schedstat(sb, &t->run, &t->wait);
            { uint64_t ut, st; int cpu; if (rd_fd(t->fs, sb, sizeof sb) > 0 && cpf_parse_stat(sb, &ut, &st, &cpu)) t->jif = ut + st; }
        }
        t->alive = 1; seen[t - g_task] = 1;
        snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
        if (rd_path(p, b, sizeof b) > 0) { b[strcspn(b, "\n")] = 0; snprintf(t->comm, sizeof t->comm, "%s", b); }
    }
    closedir(d);
    for (int i = 0; i < g_ntask; i++) if (g_task[i].alive && !seen[i]) task_dead(&g_task[i]);   // exited: its time so far stays in the window
}
static void sample(void) {
    char b[1024];
    for (int i = 0; i < g_ntask; i++) {
        task_t *t = &g_task[i]; if (!t->alive) continue;
        uint64_t d = 0;
        if (t->fq >= 0) {
            uint64_t run, wait;
            if (rd_fd(t->fq, b, sizeof b) <= 0 || !cpf_parse_schedstat(b, &run, &wait)) { task_dead(t); continue; }
            d = run - t->run; t->w_run += d; t->w_wait += wait - t->wait; t->run = run; t->wait = wait;
            if (!d) continue;
        }
        uint64_t ut, st; int cpu;
        if (rd_fd(t->fs, b, sizeof b) <= 0 || !cpf_parse_stat(b, &ut, &st, &cpu)) { task_dead(t); continue; }
        if (t->fq < 0) { d = (ut + st - t->jif) * (uint64_t)(1000000000 / g_hz); t->w_run += d; }
        t->jif = ut + st;
        if (cpu >= 0 && cpu < g_ncpu) t->w_cl[g_cl_of[cpu] & 3] += d;
    }
    static int fstat_fd = -2; static char sb[8192];
    if (fstat_fd == -2) fstat_fd = open("/proc/stat", O_RDONLY | O_CLOEXEC);
    if (rd_fd(fstat_fd, sb, sizeof sb) > 0) {
        g_have_procstat = 1;
        uint64_t busy[64] = {0}, tot[64] = {0}; int n = cpf_parse_procstat(sb, 64, busy, tot);
        for (int c = 0; c < n; c++) { if (g_cpu_tot0[c] && tot[c] >= g_cpu_tot0[c]) { g_cpu_wbusy[c] += busy[c] - g_cpu_busy0[c]; g_cpu_wtot[c] += tot[c] - g_cpu_tot0[c]; } g_cpu_busy0[c] = busy[c]; g_cpu_tot0[c] = tot[c]; }
    }
    for (int k = 0; k < g_pol_n; k++) {
        char fb[32]; if (rd_fd(g_pol_fcur[k], fb, sizeof fb) > 0) g_pol_cur_sum[k] += strtoull(fb, NULL, 10);
        if (rd_fd(g_pol_fmax[k], fb, sizeof fb) > 0) g_pol_max_sum[k] += strtoull(fb, NULL, 10);
    }
    g_pol_samples++;
}

// ---------- roles ----------
#define CP_MAXG 160
static int32_t r_tid[CP_MAXG], r_prio[CP_MAXG], r_core[CP_MAXG]; static uint32_t r_handle[CP_MAXG]; static uint64_t r_mask[CP_MAXG], r_entry[CP_MAXG]; static char r_name[CP_MAXG][32]; static int r_n;
static int g_present_tid, g_submit_tid;
static void roles_read(void) {
    r_n = dm_thr_census(r_tid, r_handle, r_prio, r_core, r_mask, r_entry, r_name, CP_MAXG);
    g_present_tid = g_submit_tid = 0; if (dm_nvn_cpuprof_tids) dm_nvn_cpuprof_tids(&g_present_tid, &g_submit_tid);
}
// "main" = the guest main thread (0x1001), "rec" = the thread that submits NVN work, "pres" = the one that presents (when not main)
static void label(const task_t *t, char *out, size_t n) {
    char role[24] = "";
    if (t->tid == g_submit_tid) strcat(role, "rec,");
    if (t->tid == g_present_tid) strcat(role, "pres,");
    for (int i = 0; i < r_n; i++) if (r_tid[i] == t->tid) {
        if (r_handle[i] == 0x1001) strcat(role, "main,");
        snprintf(out, n, "%sg:%s(p%d)", role, r_name[i][0] ? r_name[i] : "?", r_prio[i]);
        return;
    }
    snprintf(out, n, "%s%s", role, t->comm);
}
static void roles_line(void) {
    char line[3600]; size_t o = (size_t)snprintf(line, sizeof line, "[cpuprof] roles (%d guest threads; main = handle 0x1001, rec = submits NVN %d, pres = presents %d):", r_n, g_submit_tid, g_present_tid);
    for (int i = 0; i < r_n; i++) {
        if (o > 1700) { LOG("%s", line); o = (size_t)snprintf(line, sizeof line, "[cpuprof] roles (cont.):"); }   // logcat lines stay under ~4 KB
        module_t *m = r_entry[i] ? module_of((void *)(uintptr_t)r_entry[i]) : NULL;
        o += (size_t)snprintf(line + o, sizeof line - o, " %d 0x%x '%s' p%d c%d m%llx %s+0x%llx;", r_tid[i], r_handle[i], r_name[i], r_prio[i], r_core[i], (unsigned long long)r_mask[i],
                              m ? m->name : "-", (unsigned long long)(m ? r_entry[i] - (uint64_t)(uintptr_t)m->base : r_entry[i]));
    }
    LOG("%s", line);
}

// ---------- the gate probe ----------
static double g_probe_ns, g_clock_ns;
static void probe(void) {
    gate_register((uint64_t)(uintptr_t)dm_gate_probe_ret, "probe", 0, 0);
    for (int k = 0; k < 1000; k++) dm_gate_probe();
    uint64_t a = now_ns(); for (int k = 0; k < 20000; k++) dm_gate_probe(); g_probe_ns = (double)(now_ns() - a) / 20000;
    volatile uint64_t s = 0; a = now_ns(); for (int k = 0; k < 20000; k++) s += guest_ns(); g_clock_ns = (double)(now_ns() - a) / 20000; (void)s;
    LOG("[cpuprof] gate probe: one gate round trip (dm_gate frame of 800 bytes, lookup, dispatch, back) %.1f ns; the clock gate's own guest_ns() %.1f ns", g_probe_ns, g_clock_ns);
    // [NR-24] a clock gate (cntvct into x0) through the full gate and through the short entry (with DM_NR_GATE_TICK unset the short entry
    // finds no tick block and passes on to the full gate: then both numbers are the full gate's)
    gate_register((uint64_t)(uintptr_t)dm_gate_probe_clk_ret, "cntvct", 0, 0); gate_register((uint64_t)(uintptr_t)dm_gate_probe_fast_ret, "cntvct", 0, 0);
    for (int k = 0; k < 1000; k++) { dm_gate_probe_clk(); dm_gate_probe_fast(); }
    a = now_ns(); for (int k = 0; k < 20000; k++) dm_gate_probe_clk(); const double clk = (double)(now_ns() - a) / 20000;
    a = now_ns(); for (int k = 0; k < 20000; k++) dm_gate_probe_fast(); const double fast = (double)(now_ns() - a) / 20000;
    LOG("[cpuprof] nr24 probe (DM_NR_GATE_TICK=%d): a clock gate through the full gate %.1f ns, through the short entry %.1f ns", g_tick_mode, clk, fast);
}

// ---------- report ----------
static uint64_t g_rep_ns, g_rep_frames, g_gs_last[5]; static int g_reports;
typedef struct { int i; uint64_t v; } kv_t;
static int kv_desc(const void *a, const void *b) { uint64_t x = ((const kv_t *)a)->v, y = ((const kv_t *)b)->v; return x < y ? 1 : x > y ? -1 : 0; }
static void report(uint64_t now) {
    const double sec = (double)(now - g_rep_ns) / 1e9; const uint64_t fr = dm_nvn_frame_count ? dm_nvn_frame_count() : 0;
    const double frames = fr > g_rep_frames ? (double)(fr - g_rep_frames) : 0, fd = frames > 0 ? frames : 1, fps = frames / sec, tick = cp_ns_tick();
    roles_read();
    // threads
    kv_t ord[CP_MAXT]; int n = 0; uint64_t tot = 0, totw = 0;
    for (int i = 0; i < g_ntask; i++) { tot += g_task[i].w_run; totw += g_task[i].w_wait; if (g_task[i].w_run) { ord[n].i = i; ord[n].v = g_task[i].w_run; n++; } }
    qsort(ord, (size_t)n, sizeof *ord, kv_desc);
    char line[3000]; size_t o = (size_t)snprintf(line, sizeof line, "[cpuprof] threads (%.1f s, %.0f frames = %.1f fps; %s): process %.2f ms CPU a frame (%.2f cores), run-queue wait %.2f ms a frame; per thread ms a frame (wait) [%%",
        sec, frames, fps, g_have_sched > 0 ? "schedstat" : "stat ticks", (double)tot / 1e6 / fd, (double)tot / 1e9 / sec, (double)totw / 1e6 / fd);
    for (int k = 0; k < g_ncl; k++) o += (size_t)snprintf(line + o, sizeof line - o, "%c", cpf_cluster_letter(k, g_ncl));
    o += (size_t)snprintf(line + o, sizeof line - o, "]:");
    uint64_t rest = 0; int shown = 0;
    for (int j = 0; j < n; j++) {
        task_t *t = &g_task[ord[j].i];
        if (shown >= 16 || o > sizeof line - 160) { rest += t->w_run; continue; }
        char lb[64]; label(t, lb, sizeof lb);
        o += (size_t)snprintf(line + o, sizeof line - o, " %s %d %.2f (%.2f) [", lb, t->tid, (double)t->w_run / 1e6 / fd, (double)t->w_wait / 1e6 / fd);
        for (int k = 0; k < g_ncl; k++) o += (size_t)snprintf(line + o, sizeof line - o, "%s%.0f", k ? " " : "", t->w_run ? 100.0 * (double)t->w_cl[k] / (double)t->w_run : 0);
        o += (size_t)snprintf(line + o, sizeof line - o, "];"); shown++;
    }
    snprintf(line + o, sizeof line - o, " %d more threads %.2f", n - shown, (double)rest / 1e6 / fd);
    LOG("%s", line);
    // cores
    o = (size_t)snprintf(line, sizeof line, "[cpuprof] cores (%.1f s):", sec);
    for (int k = 0; k < g_ncl; k++) {
        uint64_t b = 0, t = 0; for (int c = 0; c < g_ncpu; c++) if (g_cl_of[c] == k) { b += g_cpu_wbusy[c]; t += g_cpu_wtot[c]; }
        double cur = 0, mx = 0; for (int p = 0; p < g_pol_n; p++) if (g_cl_of[g_pol_cpu[p]] == k && g_pol_samples) { cur = (double)g_pol_cur_sum[p] / (double)g_pol_samples / 1e6; mx = (double)g_pol_max_sum[p] / (double)g_pol_samples / 1e6; }
        if (g_have_procstat) o += (size_t)snprintf(line + o, sizeof line - o, " %c %d cpus busy %.0f%% (of %d cores: %.2f), %.2f GHz avg, cap %.2f GHz;", cpf_cluster_letter(k, g_ncl), g_cl_count[k],
                                                   t ? 100.0 * (double)b / (double)t : 0, g_cl_count[k], t ? (double)b / (double)t * g_cl_count[k] : 0, cur, mx);
        else o += (size_t)snprintf(line + o, sizeof line - o, " %c %d cpus busy - (/proc/stat not readable), %.2f GHz avg, cap %.2f GHz;", cpf_cluster_letter(k, g_ncl), g_cl_count[k], cur, mx);
    }
    LOG("%s", line);
    memset(g_cpu_wbusy, 0, sizeof g_cpu_wbusy); memset(g_cpu_wtot, 0, sizeof g_cpu_wtot); memset(g_pol_cur_sum, 0, sizeof g_pol_cur_sum); memset(g_pol_max_sum, 0, sizeof g_pol_max_sum); g_pol_samples = 0;
    // hle: gates by kind and by thread, SVCs
    uint64_t kind[4] = {0}, sn[256] = {0}, st[256] = {0}; kv_t byt[CP_MAXT]; int nb = 0; const int ng = atomic_load(&g_ngst);
    for (int i = 0; i < ng; i++) {
        gst_t *g = g_gst[i]; uint64_t sum = 0;
        for (int k = 0; k < 4; k++) { uint64_t v = g->n[k]; if (k < 3) sum += v - g->l_n[k]; kind[k] += v - g->l_n[k]; g->l_n[k] = v; }
        for (int s = 0; s < 256; s++) { uint64_t a = g->svc_n[s], b = g->svc_t[s]; sn[s] += a - g->l_sn[s]; st[s] += b - g->l_st[s]; g->l_sn[s] = a; g->l_st[s] = b; }
        if (sum) { byt[nb].i = i; byt[nb].v = sum; nb++; }
    }
    qsort(byt, (size_t)nb, sizeof *byt, kv_desc);
    const double gates = (double)(kind[0] + kind[1] + kind[2]);
    o = (size_t)snprintf(line, sizeof line, "[cpuprof] hle (%.1f s): gates %.0f a frame (svc %.0f, clock %.0f, obs %.0f) = %.0f k/s; their frames at the probe's %.0f ns + the clock's %.0f ns = %.3f ms a frame; by thread a frame:",
        sec, gates / fd, (double)kind[0] / fd, (double)kind[1] / fd, (double)kind[2] / fd, gates / sec / 1000, g_probe_ns, g_clock_ns, (gates * g_probe_ns + (double)kind[1] * g_clock_ns) / 1e6 / fd);
    for (int j = 0; j < nb && j < 8; j++) {
        gst_t *g = g_gst[byt[j].i]; task_t *t = task_find(g->tid); char lb[64];
        if (t) label(t, lb, sizeof lb); else snprintf(lb, sizeof lb, "tid");
        o += (size_t)snprintf(line + o, sizeof line - o, " %s %d %.0f;", lb, g->tid, (double)byt[j].v / fd);
    }
    LOG("%s", line);
    // entries of exited threads: their last deltas are in; zero them and free them for new threads
    for (int i = 0; i < ng; i++) {
        gst_t *g = g_gst[i]; char tp[48]; if (!atomic_load(&g->live)) continue;
        snprintf(tp, sizeof tp, "/proc/self/task/%d", g->tid);
        if (!access(tp, F_OK)) continue;
        pthread_mutex_lock(&g_gst_mu);
        memset(g->n, 0, sizeof g->n); memset(g->svc_n, 0, sizeof g->svc_n); memset(g->svc_t, 0, sizeof g->svc_t);
        memset(g->l_n, 0, sizeof g->l_n); memset(g->l_sn, 0, sizeof g->l_sn); memset(g->l_st, 0, sizeof g->l_st);
        atomic_store(&g->live, 0);
        pthread_mutex_unlock(&g_gst_mu);
    }
    kv_t sv[256]; int ns = 0; for (int s = 0; s < 256; s++) if (sn[s]) { sv[ns].i = s; sv[ns].v = sn[s]; ns++; }
    qsort(sv, (size_t)ns, sizeof *sv, kv_desc);
    uint64_t gs[5]; dm_gstat_read(gs);
    const double takes = (double)(gs[0] - g_gs_last[0]), cont = (double)(gs[1] - g_gs_last[1]);
    o = (size_t)snprintf(line, sizeof line, "[cpuprof] hle svc (%.1f s; a frame: count x us = ms, * = blocks by design):", sec);
    for (int j = 0; j < ns && j < 14; j++) { int s = sv[j].i; double ms = (double)st[s] * tick / 1e6;
        o += (size_t)snprintf(line + o, sizeof line - o, " 0x%02x %s%s %.1f x %.1f = %.3f;", s, cpf_svc_name(s), cpf_svc_waits(s) ? "*" : "", (double)sn[s] / fd, (double)sn[s] ? ms * 1000 / (double)sn[s] : 0, ms / fd); }
    o += (size_t)snprintf(line + o, sizeof line - o, " | G: %.0f takes a frame, %.1f%% found it held, waited %.1f us a frame (longest %.1f us), held %.1f us a frame",
        takes / fd, takes ? 100.0 * cont / takes : 0, (double)(gs[2] - g_gs_last[2]) / 1e3 / fd, (double)gs[3] / 1e3, (double)(gs[4] - g_gs_last[4]) / 1e3 / fd);
    LOG("%s", line);
    memcpy(g_gs_last, gs, sizeof gs);
    {   // [NR-24] the short clock paths, the arbiter buckets, the yield waits (running totals; deltas a frame)
        static uint64_t l_fast, l_fn, l_arb[8], l_yw[8];
        uint64_t ar[8] = {0}, yw[8] = {0}; if (dm_nr24_arb_stats) dm_nr24_arb_stats(ar); if (dm_nr24_yield_stats) dm_nr24_yield_stats(yw);
        const uint64_t fast = dm_tick_fast_count ? dm_tick_fast_count() : 0, fn = dm_tick_fn_calls ? dm_tick_fn_calls() : 0;
        const double bt = (double)(ar[1] - l_arb[1]), bc = (double)(ar[2] - l_arb[2]), yn = (double)(yw[1] - l_yw[1]);
        LOG("[cpuprof] nr24 (%.1f s; a frame): DM_NR_GATE_TICK=%d clock reads by the short entry %.0f, main's GetSystemTick on the host %.0f, full clock gates %.0f; "
            "DM_NR_GATE_ARB=%d bucket takes %.0f (%.1f%% found held), wakes %.0f (futex %.0f), moved onto a lock %.0f, red %llu; "
            "DM_NR_GATE_YIELD=%d waits %.0f (%.0f woken by progress, %.0f timed out, %.1f us each), would wait %.0f",
            sec, g_tick_mode, (double)(fast - l_fast) / fd, (double)(fn - l_fn) / fd, (double)kind[1] / fd,
            (int)ar[0], bt / fd, bt ? 100.0 * bc / bt : 0, (double)(ar[3] - l_arb[3]) / fd, (double)(ar[4] - l_arb[4]) / fd, (double)(ar[5] - l_arb[5]) / fd, (unsigned long long)ar[6],
            (int)yw[0], yn / fd, (double)(yw[2] - l_yw[2]) / fd, (double)(yw[3] - l_yw[3]) / fd, yn ? (double)(yw[4] - l_yw[4]) / 1e3 / yn : 0, (double)(yw[5] - l_yw[5]) / fd);
        l_fast = fast; l_fn = fn; memcpy(l_arb, ar, sizeof ar); memcpy(l_yw, yw, sizeof yw);
    }
    if (g_reports++ % 3 == 0) roles_line();
    for (int i = 0; i < g_ntask; i++) { g_task[i].w_run = g_task[i].w_wait = 0; memset(g_task[i].w_cl, 0, sizeof g_task[i].w_cl); }
    g_rep_ns = now; g_rep_frames = fr;
}

static void *sampler(void *a) {
    (void)a;
    pthread_setname_np(pthread_self(), "dm-cpuprof");
    setpriority(PRIO_PROCESS, 0, 10);
#ifdef CPU_SETSIZE
    { cpu_set_t all; CPU_ZERO(&all); long nc = sysconf(_SC_NPROCESSORS_CONF); for (long c = 0; c < nc && c < CPU_SETSIZE; c++) CPU_SET((int)c, &all);
      sched_setaffinity(0, sizeof all, &all); }   // not the presenting guest thread's mask it was created from: the scheduler places it (nice 10)
#endif
    g_hz = sysconf(_SC_CLK_TCK); if (g_hz <= 0) g_hz = 100;
    topo_init(); probe();
    dm_gate_prof = gate_prof;
    tasks_rescan(); sample();
    g_rep_ns = now_ns(); g_rep_frames = dm_nvn_frame_count ? dm_nvn_frame_count() : 0; { uint64_t gs[5]; dm_gstat_read(gs); memcpy(g_gs_last, gs, sizeof gs); }
    for (int k = 0; k < g_ntask; k++) { g_task[k].w_run = g_task[k].w_wait = 0; memset(g_task[k].w_cl, 0, sizeof g_task[k].w_cl); }
    memset(g_cpu_wbusy, 0, sizeof g_cpu_wbusy); memset(g_cpu_wtot, 0, sizeof g_cpu_wtot);
    uint64_t last_scan = now_ns();
    for (;;) {
        struct timespec ts = {g_ms / 1000, (long)(g_ms % 1000) * 1000000L}; nanosleep(&ts, NULL);
        const uint64_t now = now_ns();
        if (now - last_scan >= 2000000000ull) { tasks_rescan(); last_scan = now; }   // new threads within 2 s (their time from then on)
        sample();
        if (now - g_rep_ns >= 10000000000ull) report(now);
    }
    return NULL;
}
static int g_cp_on = -1;
// From hle_nvn.c's nvnQueuePresentTexture (only with DM_NR_CPU=1): the first present starts the sampler
void dm_cpuprof_present(void) {
    if (__builtin_expect(g_cp_on < 0, 0)) {
        const char *e = getenv("DM_NR_CPU"); g_cp_on = e && atoi(e) > 0;
        if (!g_cp_on) return;
        if ((e = getenv("DM_NR_CPU_MS")) && *e) { int v = atoi(e); g_ms = v < 50 ? 50 : v > 2000 ? 2000 : v; }
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, sampler, NULL)) { LOG("【红】[cpuprof] DM_NR_CPU=1: the sampler thread did not start"); g_cp_on = 0; }
        else LOG("[cpuprof] DM_NR_CPU=1: sampler dm-cpuprof every %d ms, lines every 10 s (docs/dos2/nr/NR-16.md)", g_ms);
        pthread_attr_destroy(&at);
    }
}
#else
void dm_cpuprof_present(void) {}
#endif
