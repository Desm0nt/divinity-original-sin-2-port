// [DOS2] NR-29 DM_NR_MEM (docs/dos2/nr/NR-29.md): one RAM-based budget for the native renderer's caches, memory trims, and a usage line.
// Rules (classes, split, trim tiers, pressure marks) in dm/gfx/nr/nr_mem.h (pure C, tests/nr_mem_test.c); the engine side is
// dm/gfx/vulkan/engine/xbg_nr_mem.inc (weak here: no engine, no engine part). Installed by main.c host_preload before dm_dos2_nr_install
// (NR-05 / NR-22 read their caps there), before any guest code runs.
//   DM_NR_MEM=0      nothing: every cache keeps its own default, no thread, the app's onTrimMemory is ignored (old behaviour).
//   DM_NR_MEM=1      (default) the caps: total RAM (ActivityManager.MemoryInfo.totalMem from the app, else /proc/meminfo MemTotal) gives a
//                    class (4 / 6 / 8 / 12+ GB) and a budget (96 / 192 / 384 MB / none) shared by the caches the switches turn on; each
//                    cache uses min(its old default, its share) - only lower, an explicit DM_* MB variable wins; 12+ GB changes nothing.
//                    NR-07 DM_NR_PIPE=1 builds lazily (=3) in the 6 GB class and below. Trims: the app's onTrimMemory level
//                    (nativeTrimMemory) and a 1 s MemAvailable poll (Android 14+ no longer sends the RUNNING_* levels) free the caches
//                    by tier, cheapest to rebuild first (nr_mem.h). Line "[nr] mem ..." every 10 s: what the caches hold, RSS, MemAvailable.
//   DM_NR_MEM=2      verify: the caps and the trims are computed and logged ("would"), the old defaults are used and nothing is freed.
//   DM_NR_MEM=3      stress: as 1, plus a forced trim every 30 s cycling tiers 1 -> 2 -> 3 (exercises every free path; hitches on purpose).
//   DM_NR_MEM_TRIM=0 no trims at all (caps and the line stay); DM_NR_MEM_POLL=0 no MemAvailable poll (the app's levels only);
//   DM_NR_MEM_RAM_MB=<n> pretend this much RAM (a 6 / 8 GB class run on a big phone).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "../gfx/nr/nr_mem.h"

void xbg_nr_mem_usage(uint64_t *u) __attribute__((weak));            // xbg_nr_mem.inc
void xbg_nr_mem_trim(int tier, uint64_t *freed) __attribute__((weak));
int vkn_shlib_on(void) __attribute__((weak));                         // vkn_shlib.cpp: 1 = DM_VK_NATIVE=1 without the translator
// The translator's caches (staging, ASTC queue, warm-up SPIR-V, static copies) do not exist when it is not initialised:
// their figures are zero and there is nothing of its to trim. The native backend retires its own memory by frame (vkn_core.c).
static int nm_xbg(void) { return !(vkn_shlib_on && vkn_shlib_on()); }
uint64_t dm_nr_tex_mem_held(void);                                     // hle_dos2_nr.c (NR-05 table; 0 without NR-05)
uint64_t dm_nr_tex_mem_trim(void);

static int g_nm_mode;              // DM_NR_MEM after install (0 off)
static int g_nm_trim = 1, g_nm_poll = 1;
static nrm_plan g_nm_plan;
static const char *g_nm_ram_src = "?";
static int64_t g_nm_java_total, g_nm_java_thr;   // atomics: ActivityManager.MemoryInfo (the app, nativeMemInfo)
static int g_nm_req = -1;                        // atomic: the highest onTrimMemory level not handled yet (-1 none)
static unsigned g_nm_told;                       // atomic: bit per cache, its cap line was written
static pthread_mutex_t g_nm_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_nm_cv = PTHREAD_COND_INITIALIZER;
static struct {   // the trim thread's
    uint64_t trims[NRM_TRIM_N], by_app, by_poll, by_stress, freed, peak, last_freed;
    int last_level, last_tier;
} g_nm;

static uint64_t nm_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t nm_meminfo(const char *key) {
    char b[4096]; const int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    const ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    return nrm_meminfo(b, key);
}
static uint64_t nm_rss(void) {
    char b[128]; const int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    const ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    if (n <= 0) return 0;
    b[n] = 0; unsigned long long size = 0, res = 0;
    if (sscanf(b, "%llu %llu", &size, &res) != 2) return 0;
    const long ps = sysconf(_SC_PAGESIZE);
    return (uint64_t)res * (uint64_t)(ps > 0 ? ps : 4096);
}
static int nm_env(const char *n, int d, int lo, int hi) { const char *e = getenv(n); const int v = e && *e ? atoi(e) : d; return v < lo || v > hi ? d : v; }

// ---- the caps (the caches read them once, at their own start) ----
uint64_t dm_nr_mem_cap(int which, uint64_t dflt);
uint64_t dm_nr_mem_cap(int which, uint64_t dflt) {
    if (g_nm_mode <= 0 || which < 0 || which >= NRM_N) return dflt;
    const uint64_t v = nrm_apply(&g_nm_plan, which, dflt);
    if (v != dflt && !(__atomic_fetch_or(&g_nm_told, 1u << which, __ATOMIC_RELAXED) >> which & 1)) {
        char was[32];
        if (dflt == UINT64_MAX) snprintf(was, sizeof was, "no cap"); else snprintf(was, sizeof was, "%llu MB", (unsigned long long)(dflt >> 20));
        LOG("[nr] mem: %s %s -> %llu MB (DM_NR_MEM=%d, %s)%s", nrm_name(which), was, (unsigned long long)(v >> 20), g_nm_mode, nrm_tier_name(g_nm_plan.tier),
            g_nm_mode == 2 ? ": verify - the old default is used" : "");
    }
    return g_nm_mode == 2 ? dflt : v;
}
int dm_nr_mem_pipe_lazy(void);
int dm_nr_mem_pipe_lazy(void) { return (g_nm_mode == 1 || g_nm_mode == 3) && g_nm_plan.tier <= NRM_T_6GB; }

// ---- the app (jni_android.c) ----
void dm_nr_mem_java(int64_t total, int64_t threshold);
void dm_nr_mem_java(int64_t total, int64_t threshold) {
    __atomic_store_n(&g_nm_java_total, total, __ATOMIC_RELAXED);
    __atomic_store_n(&g_nm_java_thr, threshold, __ATOMIC_RELAXED);
}
void dm_nr_mem_trim_request(int level);
void dm_nr_mem_trim_request(int level) {   // the UI thread: only noted, the trim thread frees
    LOG("[nr] mem: onTrimMemory level %d (%s)%s", level, nrm_trim_tier(level) ? "a trim tier" : "below RUNNING_MODERATE: nothing",
        g_nm_mode <= 0 ? " - DM_NR_MEM=0: ignored" : !g_nm_trim ? " - DM_NR_MEM_TRIM=0: ignored" : "");
    if (g_nm_mode <= 0 || !g_nm_trim) return;
    int cur = __atomic_load_n(&g_nm_req, __ATOMIC_RELAXED);
    while (level > cur && !__atomic_compare_exchange_n(&g_nm_req, &cur, level, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    pthread_mutex_lock(&g_nm_mu); pthread_cond_signal(&g_nm_cv); pthread_mutex_unlock(&g_nm_mu);
}

// ---- usage, trims, the line ----
static void nm_usage(uint64_t u[NRM_U_N]) {
    memset(u, 0, sizeof(uint64_t) * NRM_U_N);
    if (nm_xbg() && xbg_nr_mem_usage) xbg_nr_mem_usage(u);
    u[NRM_U_TEXBLOB] = dm_nr_tex_mem_held();
}
static void nm_trim(int tier, const char *why) {
    uint64_t u[NRM_U_N], f[NRM_U_N];
    nm_usage(u);
    const uint64_t held = nrm_usage_bytes(u), rss = nm_rss();
    const double mb = 1048576.;
    if (g_nm_mode == 2) {   // what each tier would free (the NR-06 static copies and the NR-22 images free at their next step)
        LOG("[nr] mem: trim tier %d (%s) would free: NR-05/22 table %.1f MB, NR-22 images %.1f, staging kept %.1f, ASTC queue %.1f, warm-up SPIR-V %.1f, NR-06 static copies %.1f, "
            "warm pipelines not taken %llu; held %.1f MB, RSS %.0f MB (verify: nothing freed)",
            tier, why, u[NRM_U_TEXBLOB] / mb, u[NRM_U_TS_IMG] / mb, u[NRM_U_TS_FREE] / mb, u[NRM_U_ASTCQ] / mb, tier >= 2 ? u[NRM_U_SPIRV] / mb : 0., tier >= 2 ? u[NRM_U_BUFSTATIC] / mb : 0.,
            (unsigned long long)(tier >= 3 ? u[NRM_U_PIPES] : 0), held / mb, rss / mb);
        return;
    }
    memset(f, 0, sizeof f);
    const uint64_t t0 = nm_now();
    f[NRM_U_TEXBLOB] = dm_nr_tex_mem_trim();
    if (nm_xbg() && xbg_nr_mem_trim) xbg_nr_mem_trim(tier, f);
    f[NRM_U_TS_IMG] = u[NRM_U_TS_IMG];   // the table's trim released them all: the worker destroys them (its next idle)
    const uint64_t freed = nrm_usage_bytes(f) - f[NRM_U_RING];
    g_nm.trims[tier]++; g_nm.freed += freed; g_nm.last_freed = freed; g_nm.last_tier = tier;
    LOG("[nr] mem: trim tier %d (%s): freed %.1f MB of %.1f held in %.2f ms - NR-05/22 table %.1f, NR-22 images %.1f (destroyed by the worker), staging chunks %.1f, ASTC queue %.1f, "
        "warm-up SPIR-V %.1f, NR-06 static copies %.1f (at the next submission), warm pipelines not taken %llu; RSS before %.0f MB",
        tier, why, freed / mb, held / mb, (nm_now() - t0) / 1e6, f[NRM_U_TEXBLOB] / mb, f[NRM_U_TS_IMG] / mb, f[NRM_U_TS_FREE] / mb, f[NRM_U_ASTCQ] / mb,
        f[NRM_U_SPIRV] / mb, f[NRM_U_BUFSTATIC] / mb, (unsigned long long)f[NRM_U_PIPES], rss / mb);
}
static void nm_line(void) {
    uint64_t u[NRM_U_N]; nm_usage(u);
    const uint64_t held = nrm_usage_bytes(u), avail = nm_meminfo("MemAvailable"), rss = nm_rss();
    const int64_t thr = __atomic_load_n(&g_nm_java_thr, __ATOMIC_RELAXED);
    if (held > g_nm.peak) g_nm.peak = held;
    const double mb = 1048576.;
    char caps[200] = "none (old defaults)";
    if (g_nm_plan.budget) {
        int k = 0; caps[0] = 0;
        for (int i = 0; i < NRM_N && k < (int)sizeof caps - 40; i++)
            if (g_nm_plan.cap[i]) k += snprintf(caps + k, sizeof caps - (size_t)k, "%s%s %llu", k ? ", " : "", i == NRM_TEXBLOB ? "NR-05" : i == NRM_TEXSTREAM ? "NR-22" : i == NRM_BUFSTATIC ? "NR-06" : i == NRM_ASTCQ ? "ASTC" : "SPIR-V",
                                    (unsigned long long)(g_nm_plan.cap[i] >> 20));
        if (!k) snprintf(caps, sizeof caps, "-");
    }
    LOG("[nr] mem (DM_NR_MEM=%d, %.2f GiB RAM = %s, budget %llu MB%s): NR caches hold %.1f MB (peak %.1f) = NR-05/22 table %.1f + NR-22 images %.1f + staging kept %.1f + NR-06 static %.1f "
        "+ ring mirror %.1f + ASTC queue %.1f + warm-up SPIR-V %.1f; warm pipelines ready %llu; caps MB: %s; process RSS %.0f MB, MemAvailable %.0f MB%s; "
        "trims %llu / %llu / %llu by tier (app %llu, poll %llu, stress %llu), freed %.1f MB in all",
        g_nm_mode, g_nm_plan.ram / (1024. * mb), nrm_tier_name(g_nm_plan.tier), (unsigned long long)(g_nm_plan.budget >> 20), g_nm_mode == 2 ? ", verify" : "",
        held / mb, g_nm.peak / mb, u[NRM_U_TEXBLOB] / mb, u[NRM_U_TS_IMG] / mb, u[NRM_U_TS_FREE] / mb, u[NRM_U_BUFSTATIC] / mb, u[NRM_U_RING] / mb, u[NRM_U_ASTCQ] / mb,
        u[NRM_U_SPIRV] / mb, (unsigned long long)u[NRM_U_PIPES], caps, rss / mb, avail / mb, thr > 0 ? "" : " (no ActivityManager threshold)",
        (unsigned long long)g_nm.trims[1], (unsigned long long)g_nm.trims[2], (unsigned long long)g_nm.trims[3], (unsigned long long)g_nm.by_app,
        (unsigned long long)g_nm.by_poll, (unsigned long long)g_nm.by_stress, g_nm.freed / mb);
}
static void *nm_thread(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    nrm_gate gate_app, gate_poll; memset(&gate_app, 0, sizeof gate_app); memset(&gate_poll, 0, sizeof gate_poll);
    uint64_t next_line = nm_now() + 10000000000ull, next_stress = nm_now() + 30000000000ull; int stress_tier = 0;
    for (;;) {
        pthread_mutex_lock(&g_nm_mu);
        if (__atomic_load_n(&g_nm_req, __ATOMIC_RELAXED) < 0) {
            struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += 1;
            pthread_cond_timedwait(&g_nm_cv, &g_nm_mu, &t);
        }
        pthread_mutex_unlock(&g_nm_mu);
        const uint64_t now = nm_now();
        const int lvl = __atomic_exchange_n(&g_nm_req, -1, __ATOMIC_RELAXED);
        if (lvl >= 0 && g_nm_trim) {
            const int tier = nrm_trim_tier(lvl);
            char why[48]; snprintf(why, sizeof why, "onTrimMemory level %d", lvl);
            if (tier && nrm_gate_pass(&gate_app, tier, now, 5000000000ull)) { g_nm.by_app++; g_nm.last_level = lvl; nm_trim(tier, why); }
        }
        if (g_nm_trim && g_nm_poll) {
            const int64_t thr = __atomic_load_n(&g_nm_java_thr, __ATOMIC_RELAXED);
            const uint64_t avail = nm_meminfo("MemAvailable");
            const int tier = nrm_pressure(avail, g_nm_plan.ram, thr > 0 ? (uint64_t)thr : 0);
            if (tier && nrm_gate_pass(&gate_poll, tier, now, 30000000000ull)) {
                char why[64]; snprintf(why, sizeof why, "MemAvailable %llu MB", (unsigned long long)(avail >> 20));
                g_nm.by_poll++; nm_trim(tier, why);
            }
        }
        if (g_nm_mode == 3 && g_nm_trim && now >= next_stress) {
            next_stress = now + 30000000000ull; stress_tier = stress_tier % 3 + 1;
            g_nm.by_stress++; nm_trim(stress_tier, "DM_NR_MEM=3 stress");
        }
        if (now >= next_line) { next_line = now + 10000000000ull; nm_line(); }
    }
    return NULL;
}

void dm_dos2_nr_mem_install(void);   // main.c host_preload, before dm_dos2_nr_install
void dm_dos2_nr_mem_install(void) {
    const int mode = nm_env("DM_NR_MEM", 1, 0, 3);
    if (!mode) { LOG("[nr] DM_NR_MEM=0: every NR cache keeps its own default, no trims (onTrimMemory ignored), no usage line"); return; }
    g_nm_trim = nm_env("DM_NR_MEM_TRIM", 1, 0, 1); g_nm_poll = nm_env("DM_NR_MEM_POLL", 1, 0, 1);
    uint64_t ram = 0;
    const int ram_mb = nm_env("DM_NR_MEM_RAM_MB", 0, 0, 1 << 20);
    const int64_t jt = __atomic_load_n(&g_nm_java_total, __ATOMIC_RELAXED);
    if (ram_mb > 0) { ram = (uint64_t)ram_mb << 20; g_nm_ram_src = "DM_NR_MEM_RAM_MB"; }
    else if (jt > 0) { ram = (uint64_t)jt; g_nm_ram_src = "ActivityManager"; }
    else if ((ram = nm_meminfo("MemTotal"))) g_nm_ram_src = "/proc/meminfo";
    else { const long p = sysconf(_SC_PHYS_PAGES), s = sysconf(_SC_PAGESIZE); ram = p > 0 && s > 0 ? (uint64_t)p * (uint64_t)s : 0; g_nm_ram_src = ram ? "sysconf" : "unknown"; }
    // the caches the switches turn on (the same rules as their installs: hle_dos2_nr.c dm_dos2_nr_install / nrt_start, xbg_nr_buf.inc)
    const char *te = getenv("DM_NR_TEX"), *se = getenv("DM_NR_TEXSTREAM"), *ne = getenv("DM_NR"), *be = getenv("DM_NR_BUF");
    const int ts = se ? atoi(se) : 0, tex = te ? atoi(te) : ts >= 1 && ts <= 2 ? 1 : 0, nr = ne ? atoi(ne) : 0, buf = be ? atoi(be) : 0;
    const int nr_on = nr == 1 || nr == 2 || (tex >= 1 && tex <= 3);
    unsigned on = 1u << NRM_ASTCQ | 1u << NRM_PIPE;   // the encoder (no-BC GPUs) and the warm-up SPIR-V (gfx-shader-reuse): any build
    if (nr_on && tex >= 1 && tex <= 3) on |= 1u << NRM_TEXBLOB;
    if (nr_on && (tex == 1 || tex == 3) && (ts == 1 || ts == 2)) on |= 1u << NRM_TEXSTREAM;
    if (nr_on && buf >= 1 && buf <= 3) on |= 1u << NRM_BUFSTATIC | 1u << NRM_RING;
    nrm_plan_make(&g_nm_plan, ram, on);
    g_nm_mode = mode;   // before the caches read their caps
    char caps[256] = "none: every cache keeps its default"; int k = 0;
    if (g_nm_plan.budget) {
        caps[0] = 0;
        for (int i = 0; i < NRM_N && k < (int)sizeof caps - 48; i++)
            if (on >> i & 1) k += i == NRM_RING ? snprintf(caps + k, sizeof caps - (size_t)k, "%s%s %llu reserved", k ? ", " : "", nrm_name(i), (unsigned long long)(NRM_RING_RESERVE >> 20))
                                                : snprintf(caps + k, sizeof caps - (size_t)k, "%s%s %llu", k ? ", " : "", nrm_name(i), (unsigned long long)(g_nm_plan.cap[i] >> 20));
    }
    LOG("[nr] DM_NR_MEM=%d (NR-29: %s): RAM %.2f GiB (%s) = %s, budget %llu MB%s; shares in MB (each cache min(its default, share); explicit DM_* MB wins): %s; "
        "trims %s (onTrimMemory + %s), NR-07 DM_NR_PIPE=1 %s",
        mode, mode == 2 ? "verify - caps and trims computed and logged, old defaults used, nothing freed" : mode == 3 ? "on + a forced trim every 30 s (stress)" : "on",
        ram / 1073741824.0, g_nm_ram_src, nrm_tier_name(g_nm_plan.tier), (unsigned long long)(g_nm_plan.budget >> 20),
        g_nm_plan.floored ? " (below the floors: the floors kept)" : "", caps, g_nm_trim ? "on" : "off (DM_NR_MEM_TRIM=0)",
        g_nm_poll ? "a 1 s MemAvailable poll" : "no poll", dm_nr_mem_pipe_lazy() ? "-> 3 (lazy)" : "unchanged");
    pthread_t th;
    if (pthread_create(&th, NULL, nm_thread, NULL)) { LOG("【红】[nr] DM_NR_MEM: no thread - caps stay, no trims, no usage line"); g_nm_trim = 0; return; }
    pthread_setname_np(th, "dm-nrmem");
    pthread_detach(th);
}
#endif
