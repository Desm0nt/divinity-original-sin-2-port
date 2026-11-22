// [DOS2] VK-ENGINE-SIDE stage 1: the merge census (vkn_eng.h has the key, the classes and the switch). Count only: the function never
// touches `out`. Recording thread only; no locks. No game addresses.
#ifdef DM_GAME_DOS2
#include "vkn_eng.h"
#include "vkn.h"
#include "vkn_draw.h"
#include "vkn_fx.h"
#include "vkn_pass.h"
#include "vkn_tex.h"
#include <stdlib.h>
#include <string.h>

enum { KT = 8192, KT_PROBE = 32, TILES = 4 };
typedef struct kent { uint64_t key; uint64_t frame; uint32_t count; uint8_t cls; } kent;
static struct {
    int mode, reg;
    kent *t;
    uint64_t frame; int frame_seen;
    vkn_tex *atlas; int ntiles; int32_t tile[TILES][4];   // the frame's atlas tiles in order of their first draw
    vkn_eng_stats s;
} g = {.mode = -1};

static uint64_t mix(uint64_t h, uint64_t w) { h ^= w + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdull; return h ^ (h >> 33); }
static uint64_t mix_slots(uint64_t h, const int32_t *p) {
    if (!p) return mix(h, 0x51075);
    for (int i = 0; i < VKN_TEX_SLOTS; i += 2) h = mix(h, (uint64_t)(uint32_t)p[i] | (uint64_t)(uint32_t)p[i + 1] << 32);
    return h;
}
static int is_sun_atlas(const vkn_tex *d) { return d && d->w == d->h && d->w >= 1024; }

static void eng_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)out; (void)user;
    if (!g.t) return;
    if (!g.frame_seen || ctx->frame != g.frame) { g.frame = ctx->frame; g.frame_seen = 1; g.ntiles = 0; g.atlas = NULL; g.s.frames++; }
    int colors = 0;
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) if (ctx->color[i]) colors++;
    int cls;
    const int32_t rect[4] = {(int32_t)in->viewport[0], (int32_t)in->viewport[1], (int32_t)in->viewport[2], (int32_t)in->viewport[3]};
    if (!colors && ctx->depth && is_sun_atlas(ctx->depth)) {
        if (g.atlas != ctx->depth) { g.atlas = ctx->depth; g.ntiles = 0; }
        int k = 0;
        while (k < g.ntiles && memcmp(g.tile[k], rect, sizeof rect)) k++;
        if (k == g.ntiles && g.ntiles < TILES) memcpy(g.tile[g.ntiles++], rect, sizeof rect);
        cls = k < TILES ? VKN_ENG_C0 + k : VKN_ENG_C0 + TILES - 1;
    } else if (!colors && ctx->depth) cls = ctx->depth->w == ctx->depth->h ? VKN_ENG_DEPTH : VKN_ENG_PRE;
    else if (colors >= 3) cls = VKN_ENG_GB;
    else cls = VKN_ENG_OTHER;
    g.s.draws[cls]++;
    // the key: everything but the constant blocks (and the pass: its targets; the atlas tile by its viewport)
    uint64_t h = mix(0x454e4743ull, (uint64_t)cls);
    h = mix(h, in->shader); h = mix(h, in->blend); h = mix(h, in->ds); h = mix(h, in->raster); h = mix(h, in->vfmt);
    h = mix(h, (uint64_t)in->prim | (uint64_t)in->stencil_ref << 8 | (uint64_t)ctx->indexed << 16);
    for (int i = 0; i < 3; i++) h = mix(mix(h, in->vb[i]), in->vb_off[i]);
    h = mix(h, in->ib); h = mix(h, (uint64_t)ctx->first << 32 | ctx->count); h = mix(h, ctx->instances);
    { uint32_t a; memcpy(&a, &in->alpha_ref, 4); h = mix(h, a); }
    for (int st = 0; st < 2; st++) { h = mix_slots(h, in->tex[st]); h = mix_slots(h, in->smp[st]); }
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) h = mix(h, (uint64_t)(uintptr_t)ctx->color[i]);
    h = mix(h, (uint64_t)(uintptr_t)ctx->depth);
    if (cls < VKN_ENG_PRE) { h = mix(h, (uint64_t)(uint32_t)rect[0] << 32 | (uint32_t)rect[1]); h = mix(h, (uint64_t)(uint32_t)rect[2] << 32 | (uint32_t)rect[3]); }
    if (!h) h = 1;
    for (uint32_t p = 0, i = (uint32_t)(h % KT); p < KT_PROBE; p++, i = (i + 1) % KT) {
        kent *e = &g.t[i];
        if (e->frame != g.frame || !e->key) { e->key = h; e->frame = g.frame; e->count = 1; e->cls = (uint8_t)cls; g.s.keys[cls]++; return; }
        if (e->key != h) continue;
        const uint32_t c = ++e->count;
        g.s.g2[cls] += c == 2 ? 2 : 1;
        if (c >= 4) g.s.g4[cls] += c == 4 ? 4 : 1;
        if (c >= 8) g.s.g8[cls] += c == 8 ? 8 : 1;
        return;
    }
    g.s.full++;
}

void vkn_eng_install(void) {
    if (g.mode >= 0) return;
    const char *e = getenv("DM_VK_ENG_CENSUS");
    g.mode = e && *e ? atoi(e) : 0;
    if (g.mode < 0 || g.mode > 2) g.mode = 0;
    if (g.mode < 2) return;
    if (!g.t && !(g.t = (kent *)calloc(KT, sizeof *g.t))) { vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91" "eng merge census: no memory for the key table: off"); g.mode = 1; return; }
    if (!g.reg) g.reg = !vkn_fx_register("engcensus", eng_fx, NULL, 5);
    if (!g.reg) { vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91" "eng merge census: the draw-effect chain refused the function: off"); g.mode = 1; return; }
    vkn_logf("DM_VK_ENG_CENSUS=%d (VK-ENGINE-SIDE stage 1): the merge census is on (draws of one pass with the same mesh + material + pipeline; "
             "count only, ~0.1 us a draw on the recording thread: not for timing runs)", g.mode);
}
int vkn_eng_census_mode(void) { return g.mode; }
void vkn_eng_stats_get(vkn_eng_stats *s) { if (s) *s = g.s; }
void vkn_eng_test_setup(int mode) {
    g.mode = mode; memset(&g.s, 0, sizeof g.s); g.frame_seen = 0; g.ntiles = 0; g.atlas = NULL;
    if (mode >= 2) {
        if (!g.t) g.t = (kent *)calloc(KT, sizeof *g.t); else memset(g.t, 0, KT * sizeof *g.t);
        if (!g.reg) g.reg = !vkn_fx_register("engcensus", eng_fx, NULL, 5);
    } else if (g.reg) { vkn_fx_drop("engcensus"); g.reg = 0; }
}
static double pct(uint64_t a, uint64_t b) { return b ? 100.0 * (double)a / (double)b : 0.0; }
void vkn_eng_census(void) {
    if (g.mode < 2 || !g.reg) return;
    const vkn_eng_stats *s = &g.s; const double fr = s->frames ? (double)s->frames : 1.0;
    static const char *const nm[VKN_ENG_CLS] = {"c0", "c1", "c2", "c3", "prepass", "gbuffer", "other", "depth"};
    char b[1500]; size_t k = 0;
    uint64_t all = 0, a2 = 0, a4 = 0, a8 = 0;
    for (int c = 0; c < VKN_ENG_CLS; c++) { all += s->draws[c]; a2 += s->g2[c]; a4 += s->g4[c]; a8 += s->g8[c]; }
    k += (size_t)snprintf(b + k, sizeof b - k, "eng merge census (last 10 s, %llu frames): draws a frame %.0f, in groups of one mesh + material + pipeline within a pass "
                          ">=2 %.1f %% / >=4 %.1f %% / >=8 %.1f %%; by pass [draws a frame, keys a frame, >=2 / >=4 / >=8 %%]:", (unsigned long long)s->frames, (double)all / fr,
                          pct(a2, all), pct(a4, all), pct(a8, all));
    for (int c = 0; c < VKN_ENG_CLS && k < sizeof b - 120; c++) {
        if (!s->draws[c]) continue;
        k += (size_t)snprintf(b + k, sizeof b - k, " %s %.0f, %.0f, %.1f / %.1f / %.1f;", nm[c], (double)s->draws[c] / fr, (double)s->keys[c] / fr, pct(s->g2[c], s->draws[c]),
                              pct(s->g4[c], s->draws[c]), pct(s->g8[c], s->draws[c]));
    }
    if (k < sizeof b - 60) snprintf(b + k, sizeof b - k, " key table full %llu", (unsigned long long)s->full);
    vkn_logf("%s", b);
    memset(&g.s, 0, sizeof g.s);
}
#endif
