// [DOS2] NR-HEAT DM_NR_HEAT (docs/dos2/nr/NR-HEAT.md): where the player is, for the shader / pass heat log. The game reads every level
// file out of a few LSPK packages (rom:/Data/Origins.pak holds 109 levels under Mods/DivinityOrigins_*/Levels/<name>/ and
// .../Globals/<name>/, Arena.pak the arena maps, Shared.pak the character creation / menu levels, Minimaps.pak each level's minimap,
// CommunityMods.pak the mods' additions to the main levels). Those packages' file tables say which byte range is which level's file, so a
// read of the package at offset X is a read of level L's data; a level load reads MBs of one level (TUT_Tutorial_A 1.4 MB packed,
// FJ_FortJoy_Main 30 MB, the main menu's ORI_Menuscreen_A 2.2 MB). nr_heat.c's vote turns the bytes into the current level.
//
// Cost: an OpenFile pays one call (a name compare against five names when the switch is on); a ReadFile on one of the five packages a
// binary search in its table and one atomic add (the game reads them only while loading). The tables are read once per package, on a
// thread of their own (nice 10) the first time the game opens the package (at boot): the LSPK list (Origins.pak 24898 entries, LZ4) ->
// spans of the level files only (18930 spans x 24 B for Origins.pak).
//   DM_NR_HEAT  0 = nothing (the hooks clear their descriptor slot and return), else on (the engine side decides what it samples)
// The engine's writer thread asks dm_dos2_heat_level() once a second.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "../gfx/nr/nr_heat.h"
#include "../third_party/libdeflate/libdeflate.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int64_t dm_data_logical_size(const char *hostpath, int64_t phys_size);   // hle_fs.c
uint8_t g_dm_dos2_heat_fd[4096];   // hle_fs.c: descriptor -> level package slot + 1 (0 = not one)

static const char *const k_paks[] = {"Origins.pak", "Arena.pak", "Shared.pak", "Minimaps.pak", "CommunityMods.pak"};
enum { HP_N = sizeof k_paks / sizeof k_paks[0] };
typedef struct { _Atomic(nrh_span *) spans; _Atomic uint32_t n; _Atomic int state; char host[1024]; } heat_pak;   // state 0 new, 1 loading, 2 done, 3 failed
static heat_pak g_pak[HP_N];
// [NR-BN] save games: descriptor slot HP_SAVE + 1 = a save package whose globals.lsf span names a level (g_save[fd]); see nr_heat.h
enum { HP_SAVE = HP_N };
typedef struct { uint64_t off, end; int level; } heat_save;
static heat_save g_save[4096];
static _Atomic uint32_t g_save_logs;
static _Atomic uint64_t g_rd_in, g_rd_out, g_rd_n, g_rd_max, g_rd_save;   // [NR-BN] pak reads since the last "reads" line
static nrh_names g_names;                 // written by the loaders under g_load_mu; an index is used only after its spans are published
static pthread_mutex_t g_load_mu = PTHREAD_MUTEX_INITIALIZER, g_vote_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_bytes[NRH_LEVELS];   // bytes read per level since the last vote step
static _Atomic uint64_t g_total[NRH_LEVELS];   // never reset (the switch line shows them)
static void add_level(void *c, uint32_t lv, uint64_t b) {
    (void)c;
    if (lv < NRH_LEVELS) { atomic_fetch_add_explicit(&g_bytes[lv], b, memory_order_relaxed); atomic_fetch_add_explicit(&g_total[lv], b, memory_order_relaxed); }
}
static nrh_vote g_vote; static int g_vote_init; static uint32_t g_gen; static int g_prev = -1;
static int g_mode = -1;

static int heat_mode(void) {
    if (g_mode < 0) { const char *e = getenv("DM_NR_HEAT"); g_mode = e && *e ? atoi(e) : 0; if (g_mode < 0) g_mode = 0; }
    return g_mode;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int read_at(int fd, void *b, size_t n, off_t off) {
    size_t got = 0;
    while (got < n) { ssize_t r = pread(fd, (uint8_t *)b + got, n - got, off + (off_t)got); if (r <= 0) return -1; got += (size_t)r; }
    return 0;
}
static void *pak_loader(void *arg) {
    heat_pak *p = (heat_pak *)arg; const int slot = (int)(p - g_pak);
#if defined(__linux__)
    setpriority(PRIO_PROCESS, 0, 10);
#endif
    pthread_mutex_lock(&g_load_mu);
    const double t0 = now_ms(); const char *why = NULL; int n = -1; nrh_span *v = NULL; uint8_t *list = NULL;
    const int fd = open(p->host, O_RDONLY);
    struct stat st; uint8_t tail[8], hdr[64]; uint32_t hs = 0, lo = 0, ls = 0;
    if (fd < 0 || fstat(fd, &st) || st.st_size < 64) why = "cannot open";
    else if (dm_data_logical_size(p->host, st.st_size) != st.st_size) why = "zero-stripped (.holes) package, not read";
    else if (read_at(fd, tail, 8, st.st_size - 8) || nrh_lspk_tail(tail, &hs) || hs > sizeof hdr || (off_t)hs > st.st_size) why = "no LSPK tail";
    else if (read_at(fd, hdr, hs, st.st_size - hs) || nrh_lspk_header(hdr, hs, &lo, &ls)) why = "not an LSPK v13 header";
    else if ((off_t)lo + ls > st.st_size || ls > (64u << 20) || !(list = (uint8_t *)malloc(ls)) || read_at(fd, list, ls, lo)) why = "file list unreadable";
    else if ((n = nrh_lspk_levels(list, ls, &g_names, &v)) < 0) why = "file list corrupt (or level names full)";
    if (fd >= 0) close(fd);
    free(list);
    if (why) { atomic_store(&p->state, 3); LOG("【红】[nr] heat: %s: %s - its reads tell no location", k_paks[slot], why); }
    else {
        atomic_store_explicit(&p->n, (uint32_t)n, memory_order_relaxed);
        atomic_store_explicit(&p->spans, v, memory_order_release);
        atomic_store(&p->state, 2);
        LOG("[nr] heat: %s level table: %d level files (%u level names known), %.1f ms", k_paks[slot], n, g_names.n, now_ms() - t0);
    }
    pthread_mutex_unlock(&g_load_mu);
    return NULL;
}
// hle_fs.c OpenFile (every open: descriptors are reused, so the slot is set or cleared each time)
// [NR-BN] A save package opened for reading: its meta.lsf names the level, its globals.lsf is what a load of it reads. 1 = g_save[fd] set.
// Runs in the game's OpenFile (a save opens rarely: the load menu, a load); the level names are the pak loaders' (trylock: never waits).
static int save_scan(const char *path, const char *host, int fd) {
    const size_t pl = path ? strlen(path) : 0;
    if (pl < 5 || strcasecmp(path + pl - 4, ".lsv") || !host) return 0;
    const int hf = open(host, O_RDONLY);
    if (hf < 0) return 0;
    struct stat st; uint8_t tail[8], hdr[64]; uint32_t hs = 0, lo = 0, ls = 0; uint8_t *list = NULL, *raw = NULL, *meta = NULL;
    nrh_lspk_ent me, ge; int level = -1; const char *why = NULL;
    if (fstat(hf, &st) || st.st_size < 64) why = "too small";
    else if (read_at(hf, tail, 8, st.st_size - 8) || nrh_lspk_tail(tail, &hs) || hs > sizeof hdr || (off_t)hs > st.st_size) why = "no LSPK tail";
    else if (read_at(hf, hdr, hs, st.st_size - hs) || nrh_lspk_header(hdr, hs, &lo, &ls)) why = "not LSPK v13";
    else if ((off_t)lo + ls > st.st_size || ls > (1u << 20) || !(list = (uint8_t *)malloc(ls)) || read_at(hf, list, ls, lo)) why = "file list unreadable";
    else if (nrh_lspk_entry(list, ls, "meta.lsf", &me) || nrh_lspk_entry(list, ls, "globals.lsf", &ge)) why = "no meta.lsf / globals.lsf";
    else if (!me.disk || me.disk > (1u << 20) || me.size > (4u << 20) || (off_t)(me.off + me.disk) > st.st_size || !(raw = (uint8_t *)malloc(me.disk)) ||
             read_at(hf, raw, me.disk, (off_t)me.off)) why = "meta.lsf unreadable";
    else {
        const uint32_t method = me.flags & 0x0f; size_t mn = 0;
        if (method == 0) { meta = raw; raw = NULL; mn = me.disk; }
        else if ((meta = (uint8_t *)malloc(me.size ? me.size : 1))) {
            if (method == 1) { struct libdeflate_decompressor *z = libdeflate_alloc_decompressor(); size_t got = 0;
                if (z && libdeflate_zlib_decompress(z, raw, me.disk, meta, me.size, &got) == LIBDEFLATE_SUCCESS) mn = got;
                if (z) libdeflate_free_decompressor(z); }
            else if (method == 2) { const int got = nrh_lz4(raw, me.disk, meta, me.size); if (got > 0) mn = (size_t)got; }
        }
        if (!mn) why = "meta.lsf: cannot decompress";
        else if (pthread_mutex_trylock(&g_load_mu)) why = "level names still loading";
        else { level = nrh_lsf_level(meta, mn, &g_names); pthread_mutex_unlock(&g_load_mu); if (level < 0) why = "meta.lsf names no known level"; }
    }
    close(hf); free(list); free(raw); free(meta);
    const uint32_t nl = atomic_fetch_add(&g_save_logs, 1);
    if (nl < 12) {
        if (why) LOG("[nr] heat: save %s: %s - its load tells no location", path, why);
        else LOG("[nr] heat: save %s: level %s (globals.lsf %u KB at %llu: a load of it counts for that level)", path, g_names.name[level], ge.disk >> 10, (unsigned long long)ge.off);
    }
    if (why || !ge.disk) return 0;
    g_save[fd].off = ge.off; g_save[fd].end = ge.off + ge.disk; g_save[fd].level = level;
    return 1;
}
void dm_dos2_heat_open(const char *path, const char *host, int fd, int writing);
void dm_dos2_heat_open(const char *path, const char *host, int fd, int writing) {
    if (fd < 0 || fd >= (int)sizeof g_dm_dos2_heat_fd) return;
    int mine = 0;
    if (heat_mode() && !writing && save_scan(path, host, fd)) { g_dm_dos2_heat_fd[fd] = (uint8_t)(HP_SAVE + 1); return; }
    if (heat_mode() && path && host) {
        const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
        for (int i = 0; i < HP_N; i++) if (!strcasecmp(b, k_paks[i])) { mine = i + 1; break; }
        if (mine) {
            heat_pak *p = &g_pak[mine - 1]; int z = 0;
            if (atomic_compare_exchange_strong(&p->state, &z, 1)) {
                snprintf(p->host, sizeof p->host, "%s", host);
                pthread_t th;
                if (pthread_create(&th, NULL, pak_loader, p)) { atomic_store(&p->state, 3); LOG("【红】[nr] heat: no loader thread for %s", k_paks[mine - 1]); }
                else { pthread_setname_np(th, "dm-heatpak"); pthread_detach(th); }
            }
        }
    }
    g_dm_dos2_heat_fd[fd] = (uint8_t)mine;
}
// hle_fs.c ReadFile on such a descriptor
void dm_dos2_heat_read(int fd, int64_t off, size_t n);
void dm_dos2_heat_read(int fd, int64_t off, size_t n) {
    if (fd < 0 || fd >= (int)sizeof g_dm_dos2_heat_fd || !g_dm_dos2_heat_fd[fd] || off < 0) return;
    if (g_dm_dos2_heat_fd[fd] == HP_SAVE + 1) {   // [NR-BN] a save: only its globals.lsf (the load itself; the menu reads meta.lsf)
        const heat_save *v = &g_save[fd]; const uint64_t a = (uint64_t)off, b = a + n;
        const uint64_t lo2 = a > v->off ? a : v->off, hi2 = b < v->end ? b : v->end;
        if (hi2 > lo2 && v->level >= 0) { add_level(NULL, (uint32_t)v->level, hi2 - lo2); atomic_fetch_add_explicit(&g_rd_save, hi2 - lo2, memory_order_relaxed); }
        return;
    }
    heat_pak *p = &g_pak[g_dm_dos2_heat_fd[fd] - 1];
    const nrh_span *v = atomic_load_explicit(&p->spans, memory_order_acquire);
    if (!v) return;
    // [NR-BN] by bytes over every level file the read overlaps (was: all of it to the file it starts in); what lies outside level files
    // and the largest read go to the minute's "[nr] heat: reads" line
    const uint64_t in = nrh_span_range(v, atomic_load_explicit(&p->n, memory_order_relaxed), (uint64_t)off, n, add_level, NULL);
    atomic_fetch_add_explicit(&g_rd_in, in, memory_order_relaxed); atomic_fetch_add_explicit(&g_rd_out, n - in, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_rd_n, 1, memory_order_relaxed);
    for (uint64_t m = atomic_load_explicit(&g_rd_max, memory_order_relaxed); n > m && !atomic_compare_exchange_weak(&g_rd_max, &m, n);) { }
}
// The engine's writer thread, about once a second: one vote step. Returns the generation (+1 at every change); `cur` = the current level
// ("-" before the first), `prev` = the one before it, *score_kb = the winner's score when it changed.
int dm_dos2_heat_level(char *cur, size_t n, char *prev, size_t pn, double *score_kb);
int dm_dos2_heat_level(char *cur, size_t n, char *prev, size_t pn, double *score_kb) {
    uint64_t d[NRH_LEVELS];
    for (int i = 0; i < NRH_LEVELS; i++) d[i] = atomic_exchange_explicit(&g_bytes[i], 0, memory_order_relaxed);
    pthread_mutex_lock(&g_vote_mu);
    if (!g_vote_init) { nrh_vote_init(&g_vote); g_vote_init = 1; }
    const int was = g_vote.cur;
    if (nrh_vote_step(&g_vote, d, NRH_LEVELS)) {
        g_gen++; g_prev = was;
        if (score_kb) *score_kb = g_vote.score[g_vote.cur] / 1024;
        int second = -1;   // the evidence: the runner-up
        for (int i = 0; i < NRH_LEVELS; i++) if (i != g_vote.cur && g_vote.score[i] > 0 && (second < 0 || g_vote.score[i] > g_vote.score[second])) second = i;
        LOG("[nr] heat: location %s -> %s (%.0f KB read lately, %.0f KB in all; runner-up %s %.0f KB)", was >= 0 ? g_names.name[was] : "-",
            g_names.name[g_vote.cur], g_vote.score[g_vote.cur] / 1024, atomic_load(&g_total[g_vote.cur]) / 1024.0,
            second >= 0 ? g_names.name[second] : "-", second >= 0 ? g_vote.score[second] / 1024 : 0.0);
    }
    static uint32_t steps; static double sum[NRH_LEVELS];   // [NR-BN] the minute's reads: in / outside level files, by level (top 3), largest
    for (int i = 0; i < NRH_LEVELS; i++) sum[i] += (double)d[i];
    if (++steps >= 60) {
        steps = 0;
        const uint64_t in = atomic_exchange(&g_rd_in, 0), out = atomic_exchange(&g_rd_out, 0), rn = atomic_exchange(&g_rd_n, 0), mx = atomic_exchange(&g_rd_max, 0), sv = atomic_exchange(&g_rd_save, 0);
        if (rn || sv) {
            int top[3] = {-1, -1, -1};
            for (int i = 0; i < NRH_LEVELS; i++) if (sum[i] > 0) for (int k = 0; k < 3; k++) if (top[k] < 0 || sum[i] > sum[top[k]]) { for (int j = 2; j > k; j--) top[j] = top[j - 1]; top[k] = i; break; }
            char t[192]; size_t o = 0; t[0] = 0;
            for (int k = 0; k < 3 && top[k] >= 0; k++) o += (size_t)snprintf(t + o, sizeof t - o, "%s%s %.1f MB", k ? ", " : "", g_names.name[top[k]], sum[top[k]] / 1048576.0);
            LOG("[nr] heat: reads (60 s): %llu pak reads, %.1f MB in level files (%s), %.1f MB outside them, largest %.0f KB; save loads %.1f MB; location %s",
                (unsigned long long)rn, in / 1048576.0, o ? t : "-", out / 1048576.0, mx / 1024.0, sv / 1048576.0, g_vote.cur >= 0 ? g_names.name[g_vote.cur] : "-");
        }
        memset(sum, 0, sizeof sum);
    }
    snprintf(cur, n, "%s", g_vote.cur >= 0 ? g_names.name[g_vote.cur] : "-");
    if (prev && pn) snprintf(prev, pn, "%s", g_prev >= 0 ? g_names.name[g_prev] : "-");
    const int gen = (int)g_gen;
    pthread_mutex_unlock(&g_vote_mu);
    return gen;
}
#endif
