// [DOS2] VK-NATIVE stage 5.4: the DM_HACK_* glue of mode 1 (vkn_hacks.h: where each hack lives, the switches, the lines).
#ifdef DM_GAME_DOS2
#include "vkn_hacks.h"
#include "vkn.h"
#include "vkn_draw.h"
#include "vkn_frame.h"
#include "vkn_tex.h"
#include "vkn_shc.h"
#include "vkn_shlib_named.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nr/nr_hacks.h"

#define HK_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
// The parts this line reports (weak: a host test links some of them only).
void vkn_shlib_named_hacks_get(vkn_shlib_named_hacks *h) __attribute__((weak));
void vkn_shlib_named_stats_get(vkn_shlib_named_stats *s) __attribute__((weak));
int vkn_shc_far(void) __attribute__((weak));
void vkn_shc_stats_get(vkn_shc_stats *s) __attribute__((weak));
void vkn_tex_hack_get(vkn_tex_hack_stats *s) __attribute__((weak));
int vkn_lf_active(void) __attribute__((weak));
int vkn_post_level(void) __attribute__((weak));

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_v[NRH_N]; static char g_from[NRH_N]; static int g_lcap, g_any, g_lf_yield, g_lsel = 1;
static void read_once(void) {
    for (int k = 0; k < NRH_N; k++) g_v[k] = nrh_value(k, &g_from[k]);
    const char *e = getenv("DM_VK_NATIVE_LCAP"); int m = e && *e ? atoi(e) : 2;
    if (m < 0 || m > 2) { vkn_logf(HK_RED "hacks: DM_VK_NATIVE_LCAP=%s is not 0 / 1 / 2: 2 (the cap inside the fused Sky -> Light draw)", e); m = 2; }
    // [gpu2] which lights the cap drops (vkn_hacks.h DM_VK_NATIVE_LCAP_SEL): 1 the weak ones, each judged alone (default), 0 the old
    // "after the first N in the engine's list" (flickers: the list order changes from frame to frame), 2 = 1 + poison (the cap module
    // only: LIGHTFUSE yields as with DM_VK_NATIVE_LCAP=1, the poison twins have no fused form)
    const char *se = getenv("DM_VK_NATIVE_LCAP_SEL"); int sel = se && *se ? atoi(se) : 1;
    if (sel < 0 || sel > 2) { vkn_logf(HK_RED "hacks: DM_VK_NATIVE_LCAP_SEL=%s is not 0 / 1 / 2: 1 (the weak lights of each pixel)", se); sel = 1; }
    g_lsel = sel;
    if (sel == 2 && m == 2) { m = 1; vkn_logf("hacks: DM_VK_NATIVE_LCAP_SEL=2 (poison): DM_VK_NATIVE_LCAP=2 -> 1 (the poison cap module has no fused form: LIGHTFUSE yields)"); }
    g_lcap = m;
    for (int k = 0; k < NRH_N; k++) g_any |= g_v[k] != 0;
}
void vkn_hacks_values(int *v, char *from) {
    pthread_once(&g_once, read_once);
    if (v) memcpy(v, g_v, sizeof g_v);
    if (from) memcpy(from, g_from, sizeof g_from);
}
int vkn_hacks_lcap_mode(void) { pthread_once(&g_once, read_once); return g_lcap; }
int vkn_hacks_lcap_sel(void) { pthread_once(&g_once, read_once); return g_lsel; }
int vkn_hacks_lightfuse_ok(void) {
    pthread_once(&g_once, read_once);
    if (!g_v[NRH_LIGHTS] || g_lcap != 1) return 1;   // (2: the fused draw carries the cap itself)
    const char *e = getenv("DM_VK_NATIVE_SH"); const int sh = e && *e ? atoi(e) : 1;   // xbg_nr_sh_default: 1 on every family
    if (sh != 1) return 1;   // the cap module is not used (vkn_shlib_named.cpp prints that red): LIGHTFUSE keeps its gain
    if (!g_lf_yield) {
        g_lf_yield = 1;
        vkn_logf("hacks: DM_HACK_LIGHTS=%d%s with DM_VK_NATIVE_LCAP=1: LIGHTFUSE (DeferredSky inside the Light draw) is NOT installed - its fused bundle "
                 "replaces the Light module without the cap; the cap module draws the lights (DM_VK_NATIVE_LCAP=0: LIGHTFUSE, no cap)",
                 g_v[NRH_LIGHTS], g_from[NRH_LIGHTS] == 't' ? " (tier)" : "");
    }
    return 0;
}

// ---- DRS signals (vkn_hacks.h) ----
double vkn_hacks_gpu_ms(void) {
    uint64_t b = 0, e = 0;
    if (!vkn_frame_gpu_ns(&b, &e) || e <= b) return -1.0;
    return (double)(e - b) / 1e6;
}
int vkn_hacks_recent_gpu_ms(double *ms) {
    if (vkn_mode() != VKN_NATIVE) return 0;
    if (ms) *ms = vkn_hacks_gpu_ms();
    return 1;
}
static double g_draws_last = -1.0;   // the last answer of vkn_hacks_draws (the census reads it without moving the window)
double vkn_hacks_draws(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t f0, d0; static int have; static double last = -1.0;
    vkn_draw_stats st; memset(&st, 0, sizeof st);
    vkn_draw_stats_get(&st);   // (a copy of the recording thread's counters: an aligned 64-bit read, never torn on arm64)
    const uint64_t f = vkn_frame();
    pthread_mutex_lock(&mu);
    if (!have) { have = 1; f0 = f; d0 = st.draws; }
    else if (f > f0) { last = (double)(st.draws - d0) / (double)(f - f0); f0 = f; d0 = st.draws; }
    const double r = last;
    g_draws_last = r;
    pthread_mutex_unlock(&mu);
    return r;
}

// ---- the lines ----
typedef struct txt { char b[2400]; size_t k; } txt;
static void put(txt *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(txt *t, const char *fmt, ...) {
    if (t->k >= sizeof t->b - 1) return;
    va_list a; va_start(a, fmt); const int w = vsnprintf(t->b + t->k, sizeof t->b - t->k, fmt, a); va_end(a);
    if (w > 0) t->k = t->k + (size_t)w < sizeof t->b - 1 ? t->k + (size_t)w : sizeof t->b - 1;
}
static const char *src(int k) { return g_from[k] == 'e' ? "" : g_from[k] == 't' ? "(tier)" : ""; }
static int g_reds;
static void red(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void red(const char *fmt, ...) {
    if (g_reds++ >= 16) return;
    char b[640]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    vkn_logf(HK_RED "hacks: %s", b);
}
static const char *mstate(int st) { return st == 1 ? "swapped" : st < 0 ? "REFUSED (the usual module)" : "no bundle read yet"; }
static void install_line(void) {
    vkn_shlib_named_hacks nh; memset(&nh, 0, sizeof nh); nh.sh_mode = -1;
    if (vkn_shlib_named_hacks_get) vkn_shlib_named_hacks_get(&nh);
    vkn_tex_hack_stats th; memset(&th, 0, sizeof th); th.mode = -1;
    if (vkn_tex_hack_get) vkn_tex_hack_get(&th);
    const int far = vkn_shc_far ? vkn_shc_far() : -1, lf = vkn_lf_active ? vkn_lf_active() : -1;
    const char *tier = getenv("DM_HACK_TIER");
    txt t; t.k = 0; t.b[0] = 0;
    put(&t, "hacks (stage 5.4): DM_HACK_TIER=%s; SCALE=%d%s -> the game's DRS object (hle_dos2_quality.c: the engine draws a part of its full-size targets, no backend part)",
        tier && *tier ? tier : "0", g_v[NRH_SCALE], src(NRH_SCALE));
    put(&t, " | FSR=%d%s -> vkn_shlib_named PPFXAA module (DM_VK_NATIVE_SH=%d): %s", g_v[NRH_FSR], src(NRH_FSR), nh.sh_mode,
        !g_v[NRH_FSR] ? "off" : !nh.read ? "library not read yet" : !nh.fsr ? "NOT APPLIED" : nh.fsr == 2 ? (nh.state[0] == 1 ? "checked (=2: not used)" : mstate(nh.state[0])) : mstate(nh.state[0]));
    put(&t, " | LIGHTS=%d%s -> vkn_shlib_named LCAP module (DM_VK_NATIVE_LCAP=%d, DM_VK_NATIVE_LCAP_SEL=%d %s, LIGHTFUSE %s%s): %s", g_v[NRH_LIGHTS], src(NRH_LIGHTS), g_lcap,
        g_lsel, g_lsel == 0 ? "first N of the list" : g_lsel == 1 ? "weak lights of each pixel" : "weak lights + poison",
        lf < 0 ? "n/a" : lf ? "on" : "off", g_lcap != 2 || !g_v[NRH_LIGHTS] ? "" : nh.lcf > 0 ? ", the capped fused module" : nh.lcf < 0 ? ", capped fusion REFUSED (Sky c1)" : ", capped fusion not asked yet",
        !g_v[NRH_LIGHTS] ? "off" : !nh.read ? "library not read yet" : !nh.lights ? "NOT APPLIED" : mstate(nh.state[1]));
    put(&t, " | SHPCF=%d%s -> vkn_shlib_named shadow class (DM_VK_NATIVE_SH_PCF%s)", g_v[NRH_SHPCF], src(NRH_SHPCF), getenv("DM_VK_NATIVE_SH_PCF") ? " set: it wins" : " unset: the hack wins");
    put(&t, " | SHFAR=%d%s -> vkn_shc (DM_VK_NATIVE_SHFAR=%d)", g_v[NRH_SHFAR], src(NRH_SHFAR), far);
    put(&t, " | TEXBIAS=%.2f%s ANISO=%d%s -> vkn_tex hack twins (DM_VK_NATIVE_TEXHACK=%d: %s; %u twins)", g_v[NRH_TEXBIAS] / 100.0, src(NRH_TEXBIAS), g_v[NRH_ANISO],
        src(NRH_ANISO), th.mode, th.mode == 1 ? "per texture" : th.mode == 2 ? "per sampler" : th.mode == 0 ? "off" : "n/a", th.twins);
    put(&t, " | NOBLOOM=%d%s NOAA=%d%s SHADOWS=%d%s LOD=%d%s -> hle_dos2_quality.c (game settings / code caves, no backend part)", g_v[NRH_NOBLOOM], src(NRH_NOBLOOM),
        g_v[NRH_NOAA], src(NRH_NOAA), g_v[NRH_SHADOWS], src(NRH_SHADOWS), g_v[NRH_LOD], src(NRH_LOD));
    if (vkn_post_level) put(&t, "; DM_VK_NATIVE_POST=%d (the AA step is skipped only at 100 %%, where the FSR module is FXAA; below it route A leaves out the FSR pre pass: DM_VK_NATIVE_POST_PART)", vkn_post_level());
    vkn_logf("%s", t.b);
    // a value that is set and has no working implementation
    if (g_v[NRH_FSR] && nh.read && !nh.fsr) red("DM_HACK_FSR=%d%s set: not applied (DM_VK_NATIVE_SH=%d; the swap needs 1)", g_v[NRH_FSR], src(NRH_FSR), nh.sh_mode);
    if (g_v[NRH_FSR] && nh.read && nh.fsr && nh.state[0] < 0) red("DM_HACK_FSR=%d: the FSR module was refused (red line of shlib named above): the console's stretch", g_v[NRH_FSR]);
    if (g_v[NRH_LIGHTS] && nh.read && !nh.lights) red("DM_HACK_LIGHTS=%d%s set: not applied (DM_VK_NATIVE_LCAP=%d, DM_VK_NATIVE_SH=%d)", g_v[NRH_LIGHTS], src(NRH_LIGHTS), g_lcap, nh.sh_mode);
    if (g_v[NRH_LIGHTS] && nh.read && nh.lights && nh.state[1] < 0)
        red("DM_HACK_LIGHTS=%d: the light cap module was refused: all lights%s", g_v[NRH_LIGHTS], g_lf_yield ? ", and LIGHTFUSE was not installed for it (DM_VK_NATIVE_LCAP=0 brings it back)" : "");
    if (g_v[NRH_LIGHTS] && nh.lights && lf > 0 && g_lcap != 2) red("DM_HACK_LIGHTS=%d with LIGHTFUSE on: the fused Light draw has no cap", g_v[NRH_LIGHTS]);
    if (g_v[NRH_SHPCF] && nh.read && nh.sh_mode == 0) red("DM_HACK_SHPCF=%d set: DM_VK_NATIVE_SH=0, no shadow class swap", g_v[NRH_SHPCF]);
    if (g_v[NRH_SHFAR] && far != g_v[NRH_SHFAR]) red("DM_HACK_SHFAR=%d%s set: the shadow-cache piece runs DM_VK_NATIVE_SHFAR=%d (%s)", g_v[NRH_SHFAR], src(NRH_SHFAR), far,
                                                    far < 0 ? "not installed" : "DM_VK_NATIVE_SHFAR set explicitly");
    if ((g_v[NRH_TEXBIAS] || g_v[NRH_ANISO]) && th.mode == 0) red("DM_HACK_TEXBIAS=%.2f / DM_HACK_ANISO=%d set: DM_VK_NATIVE_TEXHACK=0, the game's samplers", g_v[NRH_TEXBIAS] / 100.0, g_v[NRH_ANISO]);
    if ((g_v[NRH_TEXBIAS] || g_v[NRH_ANISO]) && th.mode > 0 && th.made == 0 && th.failed) red("DM_HACK_TEXBIAS / DM_HACK_ANISO: every hack twin failed (%llu)", (unsigned long long)th.failed);
}
static int g_lines;
void vkn_hacks_census(void) {
    pthread_once(&g_once, read_once);
    if (!g_lines++) { install_line(); return; }
    if (!g_any) return;
    vkn_shlib_named_hacks nh; memset(&nh, 0, sizeof nh);
    if (vkn_shlib_named_hacks_get) vkn_shlib_named_hacks_get(&nh);
    vkn_tex_hack_stats th; memset(&th, 0, sizeof th);
    if (vkn_tex_hack_get) vkn_tex_hack_get(&th);
    vkn_shc_stats sc; memset(&sc, 0, sizeof sc);
    if (vkn_shc_stats_get) vkn_shc_stats_get(&sc);
    const double fr = sc.frames ? (double)sc.frames : 1.0;
    vkn_logf("hacks (10 s): FSR module %s (swapped %llu, refused %llu), light cap %s (swapped %llu, refused %llu, capped fused bundles %llu); sampler twins %u alive (made %llu, failed %llu, retired %llu), "
             "lookups: twin %llu, game sampler kept %llu, comparison twins with the values %llu; far cascade (shadow cache window) %.0f draws a frame, %s %.0f "
             "(atlas not square >= 1024 %llu); DRS signals: GPU %.2f ms (newest finished frame), %.0f draws a frame",
             mstate(nh.state[0]), (unsigned long long)nh.swapped[0], (unsigned long long)nh.refused[0], mstate(nh.state[1]), (unsigned long long)nh.swapped[1],
             (unsigned long long)nh.refused[1], (unsigned long long)nh.lcf_bundles, th.twins, (unsigned long long)th.made, (unsigned long long)th.failed, (unsigned long long)th.retired, (unsigned long long)th.picked,
             (unsigned long long)th.kept, (unsigned long long)th.cmp, (double)sc.far_draws / fr, vkn_shc_far && vkn_shc_far() == 1 ? "skipped" : "would skip",
             (double)(vkn_shc_far && vkn_shc_far() == 1 ? sc.far_skipped : sc.far_draws) / fr, (unsigned long long)sc.far_not_sun, vkn_hacks_gpu_ms(), g_draws_last);
}
#endif
