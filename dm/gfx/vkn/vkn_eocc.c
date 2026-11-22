// [DOS2] VK-ENGINE-SIDE I2: occlusion culling in the engine's main-view receiver (contract, rule, threads: vkn_eocc.h; the pure test:
// vkn_eocc_model.c). This file: the switches, the worker's snapshot (one compute dispatch a presented frame, technique Z's reduce
// shader) and its frame tag, the per-frame preparation and the per-object decision of the cull jobs, the object table and the hazard
// grid, the self-check, the poison twin shaders, the census. No game addresses.
#ifdef DM_GAME_DOS2
#include "vkn_eocc.h"
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkn.h"
#include "vkn_draw.h"
#include "vkn_frame.h"
#include "vkn_fx.h"
#include "vkn_pass.h"
#include "vkn_query.h"
#include "vkn_rt.h"
#include "vkn_shader.h"
#include "vkn_tex.h"
#include "vkn_state.h"

#define EO_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
uint64_t vkn_frame_gpu_done(int *any);   // vkn_frame.c (any thread)
extern int vkn_shlib_on(void) __attribute__((weak));   // vkn_shlib.h (the shader library; absent in host tests)
struct nr_sb_prog;
const struct nr_sb_prog *vkn_shlib_vs_posl(const void *shader) __attribute__((weak));

static const uint32_t k_reduce_spv[] = {   // [v2] the cells pass with textureGather (2 x 2 texels a fetch; dm/android/shaders/vkn_eocc_reduce.comp)
#include "../../android/shaders/vkn_eocc_reduce.comp.spv.inc"
};
static const uint32_t k_magcnt_spv[] = {   // [v2] poison truth: the visible magenta pixels of G-buffer target 0
#include "../../android/shaders/vkn_eocc_magenta_count.comp.spv.inc"
};
static const uint32_t k_mag_spv[] = {
#include "../../android/shaders/vkn_eocc_magenta.frag.spv.inc"
};

enum { SNAPS = 8, PLANE = VKN_EO_GW * VKN_EO_GH, CPLANE = (VKN_EO_GW / VKN_EO_CQ) * (VKN_EO_GH / VKN_EO_CQ), FR = 32, TAB = 16384, PROBE = 32,
       STALE = 600, DECN = 4096, DECF = 16, DRAWN = 4096, PSET = 8192, PFR = 4, STAGES = 4, TWINS = 4096, HZW = 16, VIOL_HOLD = 60,
       KMAP = 8192, KMF = 4, ANIM_KEEP = 120, IDLE_FRAMES = 120 };

// ================================================================ switches
static int g_mode;
static vkn_eo_cfg g_cfg = {16, 0.02f, 0.05f, 2.0f, 8.0f, 1};
static uint32_t g_hold = 4, g_age = 6;
static int g_anim_on = 1;                 // [v2] DM_VK_ENG_OCC_ANIM: objects drawn with a non-affine vertex shader are no occluders
static uint64_t g_budget_ns = 2000000;
int vkn_eocc_mode(void) { return g_mode; }

// ================================================================ counters (relaxed atomics; a window per census line)
static struct {
    uint64_t frames, calls, other_stage, tested, hidden, would, poison, moved_hidden, why[VKN_EO_R_N], fwhy[VKN_EO_R_N];
    uint64_t test_ns, setup_ns, test_max_ns, cells, hazards, hazard_whole, age_sum, age_n, moved_frames;
    uint64_t checked, violations, check_lost, held_viol;
    uint64_t snaps, snap_fail, snap_vp_bad, snap_ns, fx_ns, fx_calls, gpu_ns, gpu_n, recs;
    uint64_t pre_draws, gb_draws, other_draws, gb_would, tinted, untinted, twins, twin_fail;
    uint64_t table_full, pset_full, dec_full;
    // [v2] the snapshot's label (the cull frame of the lists it drew, from the first report pair of its recording) against the frame tag;
    // animated objects; refusals by flag group; the idle cells pass; poison draws not tinted because of a vertex retype
    uint64_t mag_frames, mag_px, mag_any, mag_max;
    uint64_t lab_d[5], lab_far, lab_none, anim_draws, anim_marked, anim_sh_unknown, hz_anim, fl[VKN_EO_FL_N], snap_idle, untinted_retype;
} g_c;
#define ADD(f, n) __atomic_add_fetch(&g_c.f, (uint64_t)(n), __ATOMIC_RELAXED)
static void max_ns(uint64_t v) { uint64_t m = __atomic_load_n(&g_c.test_max_ns, __ATOMIC_RELAXED); while (v > m && !__atomic_compare_exchange_n(&g_c.test_max_ns, &m, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {} }

// ================================================================ snapshots (the worker writes, the cull jobs read)
typedef struct snap {
    uint32_t state;                       // atomic: 0 free, 1 recorded (meta valid), 2 pinned by a frame of the jobs
    uint64_t eframe, rframe;              // the engine (cull) frame of the lists its recording drew / vkn_frame() of the recording (GPU done test)
    uint32_t w, h, gw, gh;
    uint8_t ts;                           // timestamps written, not read yet
    VkDescriptorSet set;
} snap;
static snap g_sn[SNAPS];
static vkn_block g_sbuf; static float *g_scpu;
static inline float *snap_max(int i) { return g_scpu + (size_t)i * 2u * PLANE; }
static inline float *snap_min(int i) { return g_scpu + (size_t)i * 2u * PLANE + PLANE; }
static int g_gpu_st;                      // 0 untried, 1 ok, -1 failed
static VkSampler g_smp; static VkDescriptorSetLayout g_dsl; static VkPipelineLayout g_pl; static VkPipeline g_pipe; static VkDescriptorPool g_pool; static VkShaderModule g_mod;
static VkQueryPool g_tsq; static double g_ts_period;
// [v2] poison truth (mode 3): a counter per recording ring slot, the magenta-count pipeline (same layout as the cells pass)
static VkPipeline g_mpipe; static VkShaderModule g_mmod; static VkDescriptorSet g_mset[SNAPS]; static vkn_block g_mbuf;
static struct { uint64_t rframe; uint8_t pending; } g_mc[SNAPS];

// ---- the cameras the cull jobs saw (main stage, by engine frame) ----
typedef struct camr { uint64_t frame; uint32_t ok, writing; float vp[16]; } camr;
static camr g_cam[FR];

// ================================================================ the object table (by object address)
typedef struct ent {
    uint64_t key;                         // atomic (CAS)
    uint32_t bh, fl, born, seen, chg, fchg, vis, drawn, hw;   // box hash, flags; frames (low 32 bits): born, last seen, box / flag change, held
                                                              // visible until, drawn, the start of the current movement window
    float box[6], hb0[6], hb1[6];         // the latest box; the unions of its boxes in the previous / current window of HZW frames while it
                                          // moves (the hazard footprint is both: every box it had in the last HZW..2 HZW frames)
} ent;
static ent *g_t;
static uint32_t g_live;
static uint32_t *g_anim;                  // [v2] per entry: the last cull frame one of its G-buffer draws used a non-affine vertex shader (0 = never)
// [v2] report pair key -> entry, per cull frame (jobs write in vkn_eocc_after, the worker reads for its recording's label)
typedef struct kmapf { uint64_t frame; uint64_t k[KMAP]; uint32_t e[KMAP]; } kmapf;
static kmapf *g_km;
static uint32_t hslot(uint64_t k) { return (uint32_t)(((k >> 4) * 0x9E3779B97F4A7C15ull) >> 50) & (TAB - 1); }
// 1 = found, 2 = inserted, 0 = full
static int ent_get(uint64_t obj, uint32_t fr, ent **out) {
    uint32_t i = hslot(obj);
    for (int p = 0; p < PROBE; p++, i = (i + 1) & (TAB - 1)) {
        ent *e = &g_t[i];
        uint64_t k = __atomic_load_n(&e->key, __ATOMIC_ACQUIRE);
        if (k == obj) { *out = e; return 1; }
        if (k && fr - __atomic_load_n(&e->seen, __ATOMIC_RELAXED) < STALE) continue;
        if (!__atomic_compare_exchange_n(&e->key, &k, obj, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { if (k == obj) { *out = e; return 1; } continue; }
        if (!k) __atomic_add_fetch(&g_live, 1, __ATOMIC_RELAXED);
        e->bh = 0; e->fl = 0; e->born = fr; e->chg = fr; e->fchg = fr; e->vis = 0; e->drawn = 0; e->hw = fr;
        __atomic_store_n(&e->seen, fr, __ATOMIC_RELEASE);   // (before any other thread could take it for stale)
        *out = e; return 2;
    }
    return 0;
}

// ---- per engine frame lists: objects drawn (indices, for the hazards of a later frame); hidden decisions (the self-check) ----
typedef struct drawnf { uint64_t frame; uint32_t n; uint32_t i[DRAWN]; } drawnf;
static drawnf g_dr[DECF];
typedef struct dec { uint32_t ei; float zmin; uint16_t c[4]; } dec;
typedef struct decf { uint64_t frame; uint32_t n, checked; uint32_t w, h, gw, gh; dec d[DECN]; } decf;
static decf g_dec[DECF];
// ---- poisoned / would-hide pair keys of a frame (jobs write, the worker reads) ----
typedef struct psetf { uint64_t frame; uint64_t k[PSET]; } psetf;
static psetf g_ps[PFR];
static void pset_put(uint64_t fr, uint64_t key) {
    psetf *p = &g_ps[fr % PFR];
    if (__atomic_load_n(&p->frame, __ATOMIC_ACQUIRE) != fr) return;   // (the frame's preparation cleared it)
    uint32_t i = (uint32_t)((key >> 4) * 0x9E3779B97F4A7C15ull >> 51) & (PSET - 1);
    for (int n = 0; n < 64; n++, i = (i + 1) & (PSET - 1)) {
        uint64_t z = 0;
        if (__atomic_compare_exchange_n(&p->k[i], &z, key, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED) || z == key) return;
    }
    ADD(pset_full, 1);
}
static int pset_has(uint64_t fr, uint64_t key) {
    const psetf *p = &g_ps[fr % PFR];
    if (!key || __atomic_load_n(&p->frame, __ATOMIC_ACQUIRE) != fr) return 0;
    uint32_t i = (uint32_t)((key >> 4) * 0x9E3779B97F4A7C15ull >> 51) & (PSET - 1);
    for (int n = 0; n < 64; n++, i = (i + 1) & (PSET - 1)) { const uint64_t k = __atomic_load_n(&p->k[i], __ATOMIC_ACQUIRE); if (!k) return 0; if (k == key) return 1; }
    return 0;
}

// ---- stages: the main view = the stage whose objects took the most report pairs in the previous frame ----
static struct { uint64_t stage; uint64_t cnt[2]; } g_stg[STAGES];
static void stage_count(uint64_t stage, uint64_t fr) {
    for (int i = 0; i < STAGES; i++) {
        uint64_t s = __atomic_load_n(&g_stg[i].stage, __ATOMIC_RELAXED);
        if (!s && __atomic_compare_exchange_n(&g_stg[i].stage, &s, stage, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) s = stage;
        if (s == stage) { __atomic_add_fetch(&g_stg[i].cnt[fr & 1], 1, __ATOMIC_RELAXED); return; }
    }
}

// ================================================================ the frame (prepared once by the first main-stage call)
typedef struct fstate {
    uint64_t frame; int why, si;          // the reason nothing is tested this frame (0 = tests run); the pinned snapshot slot
    vkn_eo_snap s; vkn_eo_view v;
    float hz[PLANE], cx[CPLANE];
    uint64_t t0;                          // (the budget)
    uint64_t spent;                       // atomic: the tests' time this frame
    uint64_t hid;                         // atomic: [v2] objects hidden / would / poisoned this frame (the idle cells pass)
} fstate;
static fstate g_fs[2];
static uint64_t g_ready;                  // atomic: the frame whose state is published
static uint64_t g_prep;                   // atomic: the frame being prepared (election)
static uint64_t g_main;                   // atomic: the main stage of the frame being decided
static int g_pin[2] = {-1, -1};           // the slots pinned for the last two frames
static uint32_t g_idle;                   // [v2] prepared frames in a row that hid nothing
static int g_idle_skip;                   // atomic: the worker records the cells pass every other frame only

static void check_decisions(uint64_t now_frame) {   // the self-check: decisions of frame f against f's own depth
    int any = 0; const uint64_t done = vkn_frame_gpu_done(&any);
    if (!any) return;
    for (int k = 0; k < DECF; k++) {
        decf *d = &g_dec[k];
        if (!d->frame || d->checked || d->frame >= now_frame) continue;
        int si = -1;
        for (int i = 0; i < SNAPS; i++) { const uint32_t st = __atomic_load_n(&g_sn[i].state, __ATOMIC_ACQUIRE); if (st && g_sn[i].eframe == d->frame && g_sn[i].rframe <= done) { si = i; break; } }
        if (si < 0) { if (now_frame - d->frame > 12) { d->checked = 1; ADD(check_lost, d->n); } continue; }
        uint32_t one = 1; const int pinned = __atomic_compare_exchange_n(&g_sn[si].state, &one, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);   // (else 2: a frame's pin)
        if (!pinned && one != 2) continue;   // (the worker took it just now)
        d->checked = 1;
        const snap *s = &g_sn[si];
        if (s->eframe != d->frame || s->gw != d->gw || s->gh != d->gh || s->w != d->w || s->h != d->h) { ADD(check_lost, d->n); if (pinned) __atomic_store_n(&g_sn[si].state, 1, __ATOMIC_RELEASE); continue; }
        vkn_eo_snap v; memset(&v, 0, sizeof v); v.w = s->w; v.h = s->h; v.gw = s->gw; v.gh = s->gh; v.mx = snap_max(si); v.mn = snap_min(si); v.yup = 1;
        const uint32_t n = d->n < DECN ? d->n : DECN;
        uint32_t bad = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!vkn_eo_check(&v, d->d[i].zmin, d->d[i].c)) continue;
            bad++;
            ent *e = &g_t[d->d[i].ei & (TAB - 1)];
            const uint32_t until = (uint32_t)now_frame + VIOL_HOLD; if ((int32_t)(until - e->vis) > 0) e->vis = until;
        }
        if (pinned) __atomic_store_n(&g_sn[si].state, 1, __ATOMIC_RELEASE);
        ADD(checked, n); ADD(violations, bad); ADD(held_viol, bad);
        static uint64_t said;
        if (bad && said++ < 8) vkn_logf(EO_RED "eng occ: self-check: %u of %u objects hidden in engine frame %llu were not covered by that frame's own depth (they could have shown; held drawn %d frames)",
                                        bad, n, (unsigned long long)d->frame, VIOL_HOLD);
    }
}
static int prepare(uint64_t E, const float *vp, fstate *f) {
    { const fstate *pf = &g_fs[(E - 1) & 1];   // [v2] idle: nothing hidden for IDLE_FRAMES prepared frames -> the cells pass every other frame
      if (pf->frame + 1 == E && !__atomic_load_n(&pf->hid, __ATOMIC_RELAXED)) g_idle++; else if (pf->frame + 1 == E) g_idle = 0;
      __atomic_store_n(&g_idle_skip, g_idle > IDLE_FRAMES, __ATOMIC_RELAXED); }
    f->frame = E; f->si = -1; __atomic_store_n(&f->spent, 0, __ATOMIC_RELAXED); __atomic_store_n(&f->hid, 0, __ATOMIC_RELAXED);
    if (g_km) { kmapf *m = &g_km[E % KMF]; __atomic_store_n(&m->frame, 0, __ATOMIC_RELEASE); memset(m->k, 0, sizeof m->k); __atomic_store_n(&m->frame, E, __ATOMIC_RELEASE); }
    { const uint32_t i = (uint32_t)(E % DECF); g_dec[i].frame = E; g_dec[i].n = 0; g_dec[i].checked = 0; g_dr[i].frame = E; __atomic_store_n(&g_dr[i].n, 0, __ATOMIC_RELAXED); }
    if (g_mode >= 2) { psetf *p = &g_ps[E % PFR]; __atomic_store_n(&p->frame, 0, __ATOMIC_RELEASE); memset(p->k, 0, sizeof p->k); __atomic_store_n(&p->frame, E, __ATOMIC_RELEASE); }
    check_decisions(E);
    if (!vp) return VKN_EO_R_NOSNAP;
    // the newest finished snapshot of an earlier frame whose camera is known
    int any = 0; const uint64_t done = vkn_frame_gpu_done(&any);
    int best = -1;
    for (int i = 0; i < SNAPS && any; i++) {
        const uint32_t st = __atomic_load_n(&g_sn[i].state, __ATOMIC_ACQUIRE);
        if (!st || g_sn[i].rframe > done || g_sn[i].eframe >= E) continue;
        const camr *c = &g_cam[g_sn[i].eframe % FR];
        if (__atomic_load_n(&c->frame, __ATOMIC_ACQUIRE) != g_sn[i].eframe || !c->ok) continue;
        if (best < 0 || g_sn[i].eframe > g_sn[best].eframe) best = i;
    }
    // pins: this frame's slot; the one of the frame before stays pinned (its late tests), older ones are released
    const int old = g_pin[0];
    g_pin[0] = g_pin[1]; g_pin[1] = best;
    if (old >= 0 && old != g_pin[0] && old != g_pin[1]) { uint32_t two = 2; __atomic_compare_exchange_n(&g_sn[old].state, &two, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
    if (best < 0) return VKN_EO_R_NOSNAP;
    { uint32_t one = 1; if (!__atomic_compare_exchange_n(&g_sn[best].state, &one, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && one != 2) { g_pin[1] = -1; return VKN_EO_R_NOSNAP; } }
    const snap *sn = &g_sn[best];
    const uint64_t age = E - sn->eframe;
    ADD(age_sum, age); ADD(age_n, 1);
    if (age > g_age) return VKN_EO_R_OLD;
    f->si = best;
    vkn_eo_snap *s = &f->s; memset(s, 0, sizeof *s);
    s->w = sn->w; s->h = sn->h; s->gw = sn->gw; s->gh = sn->gh; s->mx = snap_max(best); s->mn = snap_min(best);
    s->yup = 1;   // [v2] the backend draws with a flipped viewport (vkn_draw.c): row 0 of the depth is NDC y = +1 (v1 had it mirrored)
    memcpy(s->vp, g_cam[sn->eframe % FR].vp, sizeof s->vp);
    vkn_eo_coarse(s->mx, s->gw, s->gh, f->cx, &s->cgw, &s->cgh); s->cx = f->cx;
    if (vkn_eo_snap_prep(s)) return VKN_EO_R_NOSNAP;
    // the hazard grid: objects drawn in the snapshot's frame that changed since, or were not seen in the frame before this one, or [v2]
    // are animated (a G-buffer draw of theirs with a non-affine vertex shader in the last ANIM_KEEP frames: wind, bones - their depth
    // moves inside a box that does not)
    for (size_t i = 0; i < (size_t)s->gw * s->gh; i++) f->hz[i] = FLT_MAX;
    const drawnf *dr = &g_dr[sn->eframe % DECF];
    if (dr->frame == sn->eframe) {
        const uint32_t n = __atomic_load_n(&dr->n, __ATOMIC_ACQUIRE), es = (uint32_t)sn->eframe, e1 = (uint32_t)E - 1u;
        uint32_t hz = 0, whole = 0;
        for (uint32_t k = 0; k < n && k < DRAWN; k++) {
            const ent *e = &g_t[dr->i[k] & (TAB - 1)];
            const uint32_t seen = __atomic_load_n(&e->seen, __ATOMIC_RELAXED);
            const uint32_t an = g_anim ? __atomic_load_n(&g_anim[dr->i[k] & (TAB - 1)], __ATOMIC_RELAXED) : 0;
            const int anim = g_anim_on && an && (int32_t)(an + ANIM_KEEP - es) > 0;
            if (!((int32_t)(e->chg - es) > 0 || (int32_t)(e->fchg - es) > 0 || (int32_t)(seen - e1) < 0 || anim)) continue;
            if (anim) ADD(hz_anim, 1);
            float b[6];
            for (int j = 0; j < 3; j++) { b[j] = e->hb0[j] < e->hb1[j] ? e->hb0[j] : e->hb1[j]; b[3 + j] = e->hb0[3 + j] > e->hb1[3 + j] ? e->hb0[3 + j] : e->hb1[3 + j]; }
            hz++; whole += (uint32_t)vkn_eo_hz_add(f->hz, s, b, g_cfg.pad_px);
            if (whole) break;
        }
        ADD(hazards, hz); ADD(hazard_whole, whole);
    }
    const int r = vkn_eo_view_prep(&f->v, s, vp, f->hz, &g_cfg);
    if (r) return r;
    ADD(moved_frames, f->v.moved);
    return 0;
}

// ================================================================ the cull jobs
int vkn_eocc_obj(uint64_t stage, uint64_t obj, uint64_t eframe, const float *vp, const float box[6], int eligible, uint32_t fl_hash, int *poison) {
    *poison = 0;
    if (!g_mode || !g_t || !obj) return 0;
    ADD(calls, 1);
    const uint32_t E = (uint32_t)eframe;
    // the main stage of this frame (from the last frame's pair counts), the frame's camera, the preparation
    uint64_t ready = __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE), main = __atomic_load_n(&g_main, __ATOMIC_ACQUIRE);
    if (ready != eframe) {
        uint64_t p = __atomic_load_n(&g_prep, __ATOMIC_ACQUIRE);
        if (p != eframe && p < eframe && __atomic_compare_exchange_n(&g_prep, &p, eframe, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {   // elected: choose the main stage
            uint64_t best = 0, bc = 0;   // (no counts - a frame without culls: the last main stays)
            for (int i = 0; i < STAGES; i++) { const uint64_t c = __atomic_exchange_n(&g_stg[i].cnt[(eframe - 1) & 1], 0, __ATOMIC_RELAXED); if (c > bc) { bc = c; best = g_stg[i].stage; } }
            if (best) __atomic_store_n(&g_main, best, __ATOMIC_RELEASE);
            main = __atomic_load_n(&g_main, __ATOMIC_ACQUIRE);
        } else main = __atomic_load_n(&g_main, __ATOMIC_ACQUIRE);
    }
    if (!main || stage != main) { ADD(other_stage, 1); ADD(why[main ? VKN_EO_R_STAGE : VKN_EO_R_NOTREADY], 1); return 0; }
    camr *cr = &g_cam[eframe % FR];
    if (__atomic_load_n(&cr->frame, __ATOMIC_ACQUIRE) != eframe) {
        uint32_t z = 0;
        if (__atomic_compare_exchange_n(&cr->writing, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            cr->ok = vp != NULL; if (vp) memcpy(cr->vp, vp, sizeof cr->vp);
            __atomic_store_n(&cr->frame, eframe, __ATOMIC_RELEASE); __atomic_store_n(&cr->writing, 0, __ATOMIC_RELEASE);
        }
    }
    // the object's history (also while the frame is being prepared)
    ent *e = NULL; const int ge = ent_get(obj, E, &e);
    if (!ge) { ADD(table_full, 1); ADD(why[VKN_EO_R_TABLE], 1); return 0; }
    uint32_t bh = 2166136261u; { const uint32_t *w = (const uint32_t *)(const void *)box; for (int i = 0; i < 6; i++) { bh ^= w[i]; bh *= 16777619u; } } bh |= 1;
    const uint32_t prev_seen = __atomic_load_n(&e->seen, __ATOMIC_ACQUIRE);
    const int isnew = ge == 2, fresh_seen = !isnew && prev_seen + 1u == E;
    int fchanged = 0;
    if (isnew) { memcpy(e->box, box, 24); memcpy(e->hb0, box, 24); memcpy(e->hb1, box, 24); e->bh = bh; e->fl = fl_hash; e->hw = E; }
    else if (prev_seen != E) {
        if (e->bh != bh) {
            if (E - e->hw >= HZW) { memcpy(e->hb0, e->hb1, 24); memcpy(e->hb1, e->box, 24); e->hw = E; }   // a new window (from the box it leaves)
            for (int i = 0; i < 3; i++) { if (box[i] < e->hb1[i]) e->hb1[i] = box[i]; if (box[3 + i] > e->hb1[3 + i]) e->hb1[3 + i] = box[3 + i]; }
            e->chg = E; e->bh = bh; memcpy(e->box, box, 24);
        } else if (E - e->chg > 2u * HZW) { memcpy(e->hb0, box, 24); memcpy(e->hb1, box, 24); }   // at rest
        if (e->fl != fl_hash) { e->fl = fl_hash; e->fchg = E; fchanged = 1; }
    }
    __atomic_store_n(&e->seen, E, __ATOMIC_RELEASE);
    if (isnew || !fresh_seen || fchanged) e->vis = E + g_hold;   // new, back in the view, or its flags changed: drawn the next HOLD frames
    fstate *f = &g_fs[eframe & 1];
    if (ready != eframe) {   // the first main-stage call prepares the frame; calls meanwhile draw
        uint64_t r0 = ready;
        static uint32_t prepping;
        uint32_t z = 0;
        if (__atomic_compare_exchange_n(&prepping, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) == r0 && r0 != eframe) {
                const uint64_t t0 = vkn_now_ns();
                f->why = prepare(eframe, vp, f);
                f->t0 = t0;
                ADD(frames, 1); ADD(fwhy[f->why], 1);
                ADD(setup_ns, vkn_now_ns() - t0);
                __atomic_store_n(&g_ready, eframe, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&prepping, 0, __ATOMIC_RELEASE);
        }
        if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) != eframe) { ADD(why[VKN_EO_R_NOTREADY], 1); return 0; }   // (its history is kept: not "new" next frame)
    }
    int r;
    if (eligible != VKN_EO_FL_OK) { r = VKN_EO_R_FLAGS; ADD(fl[eligible >= 0 && eligible < VKN_EO_FL_N ? eligible : 0], 1); }
    else if (isnew || !fresh_seen) r = VKN_EO_R_NEW;
    else if (fchanged) r = VKN_EO_R_CHANGED;
    else if ((int32_t)(e->vis - E) >= 0) r = VKN_EO_R_HOLD;
    else if (f->why) r = f->why;
    else if (__atomic_load_n(&f->spent, __ATOMIC_RELAXED) > g_budget_ns) r = VKN_EO_R_BUDGET;
    else {
        const uint64_t t0 = vkn_now_ns();
        vkn_eo_res res; r = vkn_eo_test(&f->v, &g_cfg, box, &res);
        const uint64_t dt = vkn_now_ns() - t0;
        __atomic_add_fetch(&f->spent, dt, __ATOMIC_RELAXED); ADD(test_ns, dt); max_ns(dt); ADD(tested, 1); ADD(cells, res.cells);
        if (r == VKN_EO_HIDDEN) {
            decf *d = &g_dec[eframe % DECF];
            const uint32_t k = __atomic_fetch_add(&d->n, 1, __ATOMIC_RELAXED);
            if (k < DECN) { d->d[k].ei = (uint32_t)(e - g_t); d->d[k].zmin = res.zmin; memcpy(d->d[k].c, res.c, sizeof res.c); }
            else ADD(dec_full, 1);
            if (k == 0) { d->w = f->s.w; d->h = f->s.h; d->gw = f->s.gw; d->gh = f->s.gh; }
            ADD(moved_hidden, f->v.moved);
        } else if (r == VKN_EO_R_DEPTH) e->vis = E + g_hold;   // found visible: drawn for the next frames too (its occluder may be leaving)
        // ([v2] the other refusals - edge, sheet, hazard, reprojection - prove nothing about visibility: tested again next frame)
    }
    ADD(why[r], 1);
    if (r != VKN_EO_HIDDEN) return 0;
    __atomic_add_fetch(&f->hid, 1, __ATOMIC_RELAXED);
    if (g_mode == 1) { ADD(hidden, 1); stage_count(stage, eframe); return 1; }   // (it would have taken a pair: the main stage stays the main stage)
    if (g_mode == 2) ADD(would, 1); else { ADD(poison, 1); *poison = 1; }
    *poison |= g_mode == 2 ? 2 : 0;   // (count mode: the key is kept too, for the G-buffer draws it would save)
    return 0;
}
void vkn_eocc_after(uint64_t stage, uint64_t obj, uint64_t eframe, int drew, uint64_t pair_key, int poison) {
    if (!g_mode || !g_t || !obj) return;
    if (pair_key) stage_count(stage, eframe);
    if (stage != __atomic_load_n(&g_main, __ATOMIC_ACQUIRE)) return;
    if (poison && pair_key) pset_put(eframe, pair_key);
    if (pair_key && g_km && g_anim_on) {   // [v2] the pair -> the object, for the worker's animated draws
        ent *e = NULL;
        if (ent_get(obj, (uint32_t)eframe, &e) == 1) {
            kmapf *m = &g_km[eframe % KMF];
            if (__atomic_load_n(&m->frame, __ATOMIC_ACQUIRE) == eframe) {
                uint32_t i = (uint32_t)((pair_key >> 4) * 0x9E3779B97F4A7C15ull >> 51) & (KMAP - 1);
                for (int n = 0; n < 32; n++, i = (i + 1) & (KMAP - 1)) {
                    uint64_t z = 0;
                    if (__atomic_compare_exchange_n(&m->k[i], &z, pair_key, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) || z == pair_key) { __atomic_store_n(&m->e[i], (uint32_t)(e - g_t), __ATOMIC_RELEASE); break; }
                }
            }
        }
    }
    if (!drew) return;
    ent *e = NULL; if (ent_get(obj, (uint32_t)eframe, &e) != 1) return;
    e->drawn = (uint32_t)eframe;
    drawnf *d = &g_dr[eframe % DECF];
    if (d->frame != eframe) return;
    const uint32_t k = __atomic_fetch_add(&d->n, 1, __ATOMIC_RELAXED);
    if (k < DRAWN) d->i[k] = (uint32_t)(e - g_t);
}

// ================================================================ the worker
static int g_wtag;                        // this recording is tagged (presented, culling on)
static uint64_t g_wE, g_wF;               // its frame counter at the begin; vkn_frame() of the recording (an off-screen one after it is not it)
static uint64_t g_wL; static int g_wlab;  // [v2] its label: the cull frame of the lists it draws (the first report pair's entry + 8)
static vkn_tex *g_wT; static int g_wphase, g_wsnap, g_wvbad; static float g_wvw, g_wvh;
static vkn_tex *g_wgb; static int g_wmag;   // [v2] (poison truth: the G-buffer's target 0 of this recording; counted)
static void tag_fn(void *p) { memcpy(&g_wE, p, 8); g_wF = vkn_frame(); g_wtag = vkn_rec_presented(); g_wT = NULL; g_wphase = 0; g_wsnap = 0; g_wvbad = 0; g_wvw = g_wvh = 0; g_wlab = 0; g_wgb = NULL; g_wmag = 0; ADD(recs, 1); }
static void label_fn(void *p) {
    uint64_t L; memcpy(&L, p, 8);
    if (!g_wtag || g_wlab || vkn_frame() != g_wF || !vkn_rec_presented()) return;
    const int64_t d = (int64_t)(L - g_wE);
    if (d < -2 || d > 2) { ADD(lab_far, 1); return; }   // (not a cull frame near this one: no label, no snapshot)
    ADD(lab_d[d + 2], 1);
    g_wL = L; g_wlab = 1;
}
static uint64_t g_enc_tag; static int g_enc_lab;   // encoder thread
void vkn_eocc_enc_frame(uint64_t eframe) { if (!g_mode) return; g_enc_tag = eframe; g_enc_lab = 0; vkn_rt_call(tag_fn, &eframe, sizeof eframe); }
void vkn_eocc_enc_pair(uint64_t key) {
    if (!g_mode || g_enc_lab || !key || (key & 7)) return;
    g_enc_lab = 1;
    const uint64_t L = __atomic_load_n((const uint64_t *)(uintptr_t)(key + 8), __ATOMIC_RELAXED);   // the engine's entry: + 8 = the cull frame
    vkn_rt_call(label_fn, &L, sizeof L);
}

static int gpu_setup(void) {
    if (g_gpu_st) return g_gpu_st;
    g_gpu_st = -1;
    const vkn_dev *d = vkn_device();
    if (!d) { g_gpu_st = 0; return -1; }
    const VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f};
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    const VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    const VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    const VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_reduce_spv, .pCode = k_reduce_spv};
    if (vkCreateSampler(d->dev, &si, NULL, &g_smp) != VK_SUCCESS || vkCreateDescriptorSetLayout(d->dev, &li, NULL, &g_dsl) != VK_SUCCESS) goto fail;
    { const VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
      if (vkCreatePipelineLayout(d->dev, &pi, NULL, &g_pl) != VK_SUCCESS) goto fail; }
    if (vkCreateShaderModule(d->dev, &mi, NULL, &g_mod) != VK_SUCCESS) goto fail;
    { const VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = g_mod, .pName = "main"}, .layout = g_pl};
      if (vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_pipe) != VK_SUCCESS) goto fail; }
    { const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * SNAPS}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * SNAPS}};
      const VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2 * SNAPS, .poolSizeCount = 2, .pPoolSizes = ps};
      if (vkCreateDescriptorPool(d->dev, &pi, NULL, &g_pool) != VK_SUCCESS) goto fail; }
    for (int i = 0; i < SNAPS; i++) {
        const VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_dsl};
        if (vkAllocateDescriptorSets(d->dev, &ai, &g_sn[i].set) != VK_SUCCESS) goto fail;
    }
    if (vkn_block_alloc(VKN_ARENA_STATIC, (uint64_t)SNAPS * 2u * PLANE * 4u, 256, &g_sbuf) || !g_sbuf.cpu) goto fail;
    g_scpu = (float *)g_sbuf.cpu;
    if (g_mode == 3) {   // [v2] the poison truth counter (optional: without it the census says "not measured")
        const VkShaderModuleCreateInfo mi2 = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_magcnt_spv, .pCode = k_magcnt_spv};
        int ok = vkCreateShaderModule(d->dev, &mi2, NULL, &g_mmod) == VK_SUCCESS;
        if (ok) { const VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = g_mmod, .pName = "main"}, .layout = g_pl};
                  ok = vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_mpipe) == VK_SUCCESS; }
        for (int i = 0; ok && i < SNAPS; i++) {
            const VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_dsl};
            ok = vkAllocateDescriptorSets(d->dev, &ai, &g_mset[i]) == VK_SUCCESS;
        }
        if (ok) ok = !vkn_block_alloc(VKN_ARENA_STATIC, 256, 256, &g_mbuf) && g_mbuf.cpu;
        if (!ok) { g_mpipe = VK_NULL_HANDLE; vkn_logf(EO_RED "eng occ: the poison truth counter could not be made: magenta pixels not measured"); }
    }
    if (d->props.limits.timestampPeriod > 0 && d->props.limits.timestampComputeAndGraphics) {   // (GPU time of the cells pass; optional)
        const VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * SNAPS};
        if (vkCreateQueryPool(d->dev, &qi, NULL, &g_tsq) != VK_SUCCESS) g_tsq = VK_NULL_HANDLE;
        g_ts_period = d->props.limits.timestampPeriod;
    }
    g_gpu_st = 1;
    vkn_logf("eng occ: depth cells pipeline ready (%d slots of %dx%d cells of %d px, max + min depth; GPU time %s)", SNAPS, VKN_EO_GW, VKN_EO_GH, VKN_EO_CELL,
             g_tsq ? "measured" : "not measured");
    return 1;
fail:
    vkn_logf(EO_RED "eng occ: the depth cells pipeline could not be created: DM_VK_ENG_OCC hides nothing (everything drawn)");
    return -1;
}
static void read_ts(void) {   // timestamps and [v2] poison counters of finished recordings (worker)
    int any = 0; const uint64_t done = vkn_frame_gpu_done(&any);
    if (!any) return;
    for (int i = 0; i < SNAPS && g_mpipe; i++) if (g_mc[i].pending && g_mc[i].rframe <= done) {
        g_mc[i].pending = 0;
        const uint32_t v = __atomic_load_n((const uint32_t *)g_mbuf.cpu + i, __ATOMIC_RELAXED);
        __atomic_store_n((uint32_t *)g_mbuf.cpu + i, 0u, __ATOMIC_RELAXED);
        ADD(mag_frames, 1); ADD(mag_px, v); ADD(mag_any, v != 0); if (v > g_c.mag_max) g_c.mag_max = v;
    }
    if (!g_tsq) return;
    for (int i = 0; i < SNAPS; i++) {
        snap *s = &g_sn[i];
        if (!s->ts || !__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) || s->rframe > done) continue;
        s->ts = 0;
        uint64_t t[2] = {0, 0};
        if (vkGetQueryPoolResults(vkn_device()->dev, g_tsq, 2u * (uint32_t)i, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
            ADD(gpu_ns, (uint64_t)((double)(t[1] - t[0]) * g_ts_period)); ADD(gpu_n, 1);
        }
    }
}
static int is_scene_depth(const vkn_tex *d) { return d && d->w != d->h && d->w >= 640 && d->h >= 360; }
// The snapshot: when the target set leaves the scene depth (after its pre-pass and G-buffer)
// [v2] poison truth: when the target set leaves the G-buffer, count the magenta pixels of its target 0 (vkn_eocc_magenta_count.comp)
static void wk_magenta(void) {
    g_wmag = 1;
    vkn_tex *A = g_wgb;
    if (!g_mpipe || !A || !A->view || !g_wvw || !g_wvh) return;
    read_ts();
    const int i = (int)(vkn_frame() % SNAPS);
    if (g_mc[i].pending) return;   // (an older one not read yet: skip this frame)
    VkCommandBuffer cb = vkn_cb();
    if (!cb) return;
    vkn_pass_layout(A, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkDescriptorImageInfo ii = {g_smp, A->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo bi = {g_mbuf.buf, g_mbuf.off, 4u * SNAPS};
    const VkWriteDescriptorSet wr[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_mset[i], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_mset[i], .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    vkUpdateDescriptorSets(vkn_device()->dev, 2, wr, 0, NULL);
    // (the counter is 0: set by the host when it read the last value - the buffer is host-visible and coherent)
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mpipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &g_mset[i], 0, NULL);
    const uint32_t w = (uint32_t)g_wvw < A->w ? (uint32_t)g_wvw : A->w, h = (uint32_t)g_wvh < A->h ? (uint32_t)g_wvh : A->h;
    const int32_t pc[4] = {(int32_t)w, (int32_t)h, i, 0};
    vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 15) / 16, (h + 15) / 16, 1);
    const VkMemoryBarrier m1 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &m1, 0, NULL, 0, NULL);
    g_mc[i].rframe = vkn_frame(); g_mc[i].pending = 1;
}
void vkn_eocc_wk_targets(uint32_t n, vkn_tex *const *color, vkn_tex *depth) {
    if (!g_mode || !g_wtag || !g_wphase || vkn_frame() != g_wF || !vkn_rec_presented()) return;
    uint32_t nc = 0;
    for (uint32_t i = 0; color && i < n && i < VKN_PASS_COLORS; i++) nc += color[i] != NULL;
    if (g_mode == 3 && g_wgb && !g_wmag && !(depth == g_wT && nc >= 3)) wk_magenta();
    if (g_wsnap) return;
    if (depth == g_wT && !nc) return;   // still the pre-pass
    // [v2] the snapshot = the depth PRE-PASS only, as it ends: the depth every opaque G-buffer draw is tested against (EQUAL), i.e. exactly
    // what decides whether an opaque object shows. v1 took it after the G-buffer, where draws outside the pre-pass may write depth too
    // (the pre-pass hybrid's direct draws, 20-odd G-buffer draws a frame without a pre-pass draw in F12): only a surface every later
    // G-buffer draw is tested against is a safe occluder.
    g_wsnap = 1;
    if (!g_wlab) { ADD(lab_none, 1); return; }   // (no report pair before the pre-pass ended: the cull frame unknown)
    if (__atomic_load_n(&g_idle_skip, __ATOMIC_RELAXED) && (g_wF & 1)) { ADD(snap_idle, 1); return; }
    const uint64_t t0 = vkn_now_ns();
    vkn_tex *T = g_wT;
    if (g_wvbad || !(g_wvw > 0) || !(g_wvh > 0)) { ADD(snap_vp_bad, 1); return; }
    if (gpu_setup() != 1 || !T || !T->view) { ADD(snap_fail, 1); return; }
    const uint32_t w = (uint32_t)g_wvw, h = (uint32_t)g_wvh;
    const uint32_t gw = (w + VKN_EO_CELL - 1) / VKN_EO_CELL, gh = (h + VKN_EO_CELL - 1) / VKN_EO_CELL;
    if (w > T->w || h > T->h || gw > VKN_EO_GW || gh > VKN_EO_GH) { ADD(snap_vp_bad, 1); return; }
    read_ts();
    // a slot: free, or recorded >= 3 frames ago (its GPU write long done) and not pinned; the oldest first
    int si = -1; const uint64_t f = vkn_frame();
    for (int i = 0; i < SNAPS; i++) {
        const uint32_t st = __atomic_load_n(&g_sn[i].state, __ATOMIC_ACQUIRE);
        if (st == 2) continue;
        if (st == 1 && g_sn[i].rframe + 3 > f) continue;
        if (si < 0 || st == 0 || (__atomic_load_n(&g_sn[si].state, __ATOMIC_ACQUIRE) == 1 && g_sn[i].rframe < g_sn[si].rframe)) si = i;
        if (st == 0) break;
    }
    if (si < 0) { ADD(snap_fail, 1); return; }
    snap *s = &g_sn[si];
    uint32_t exp = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    if (exp == 2 || !__atomic_compare_exchange_n(&s->state, &exp, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { ADD(snap_fail, 1); return; }
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { ADD(snap_fail, 1); return; }
    vkn_pass_layout(T, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // (ends the open pass; the next one brings T back as an attachment)
    const VkDescriptorImageInfo ii = {g_smp, T->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDeviceSize sb = (VkDeviceSize)2u * PLANE * 4u;
    const VkDescriptorBufferInfo bi = {g_sbuf.buf, g_sbuf.off + (VkDeviceSize)si * sb, sb};
    const VkWriteDescriptorSet wr[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    vkUpdateDescriptorSets(vkn_device()->dev, 2, wr, 0, NULL);
    if (g_tsq) { vkCmdResetQueryPool(cb, g_tsq, 2u * (uint32_t)si, 2); vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_tsq, 2u * (uint32_t)si); }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &s->set, 0, NULL);
    const int32_t pc[4] = {(int32_t)w, (int32_t)h, (int32_t)gw, (int32_t)PLANE};
    vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, gw, gh, 1);
    if (g_tsq) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsq, 2u * (uint32_t)si + 1u);
    const VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    s->eframe = g_wL; s->rframe = f; s->w = w; s->h = h; s->gw = gw; s->gh = gh; s->ts = g_tsq != VK_NULL_HANDLE;
    __atomic_store_n(&s->state, 1, __ATOMIC_RELEASE);
    ADD(snaps, 1); ADD(snap_ns, vkn_now_ns() - t0);
}

// ---- the poison twins: the engine's vertex module + a magenta fragment module (worker) ----
static VkShaderModule g_magfs; static int g_mag_st;
static struct { uint64_t handle; const vkn_shader *t; int8_t st; } g_tw[TWINS];
static const vkn_shader *twin_of(const vkn_shader *s) {
    if (!s || vkn_fx_shader_is(s) || !s->module[VKN_SRC_VS] || s->id >= TWINS) return NULL;
    if (g_tw[s->id].st && g_tw[s->id].handle == s->handle) return g_tw[s->id].st > 0 ? g_tw[s->id].t : NULL;
    g_tw[s->id].handle = s->handle; g_tw[s->id].st = -1; g_tw[s->id].t = NULL;
    if (!g_mag_st) {
        g_mag_st = -1;
        const VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_mag_spv, .pCode = k_mag_spv};
        if (vkn_device() && vkCreateShaderModule(vkn_device()->dev, &mi, NULL, &g_magfs) == VK_SUCCESS) g_mag_st = 1;
    }
    if (g_mag_st != 1) { ADD(twin_fail, 1); return NULL; }
    vkn_bind tb[64]; uint32_t n = 0;
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        if (b->stage != 0) continue;   // (the vertex stage's bindings only: the magenta stage reads nothing)
        if ((b->kind != VKN_BK_UNIFORM && b->kind != VKN_BK_SUPPORT && b->kind != VKN_BK_TEXTURE && b->kind != VKN_BK_C1) || n >= 64) { ADD(twin_fail, 1); return NULL; }
        tb[n] = *b; tb[n].dyn = VKN_NO_DYN; n++;
    }
    vkn_fx_shader_src src; memset(&src, 0, sizeof src);
    src.name = "eocc-magenta"; src.vs = s->module[VKN_SRC_VS]; src.fs = g_magfs; src.bind = tb; src.nbind = n; src.c1_from = s;
    src.ninputs = s->ninputs; memcpy(src.inputs, s->inputs, sizeof src.inputs); memcpy(src.input_classes, s->input_classes, sizeof src.input_classes);
    const vkn_shader *t = vkn_fx_shader_create(&src);
    if (!t) { ADD(twin_fail, 1); return NULL; }
    g_tw[s->id].t = t; g_tw[s->id].st = 1; ADD(twins, 1);
    return t;
}
// [v2] 1 = the engine shader's vertex position is not affine in the vertex position alone (wind, bones, time: the object's depth moves
// inside its box). Cached per shader id; no shader library (host tests): unknown = static.
static uint8_t g_ash[TWINS];
static int anim_shader(const vkn_shader *sh) {
    if (!sh || vkn_fx_shader_is(sh) || sh->id >= TWINS || !vkn_shlib_on || !vkn_shlib_on() || !vkn_shlib_vs_posl) return 0;
    uint8_t *c = &g_ash[sh->id];
    if (!*c) {
        if (!sh->xs[VKN_SRC_VS]) { ADD(anim_sh_unknown, 1); return 0; }
        const struct nr_sb_prog *pr = vkn_shlib_vs_posl(sh->xs[VKN_SRC_VS]);   // (kept only when affine: NULL = not affine)
        *c = pr ? 1 : 2;
    }
    return *c == 2;
}
// [v2] the vertex retype the draw path would apply (vkn_draw.c retype_code): the twin has the base vertex module only, so a draw that
// needs a retype is not tinted (its magenta would be garbage geometry, not an answer)
static int needs_retype(const vkn_shader *sh, uint16_t vfmt) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_GFX_VTX_RETYPE"); const vkn_dev *d = vkn_device(); on = e && *e ? atoi(e) != 0 : d ? d->props.vendorID != 0x5143 : 0; }
    if (!on || !sh || !vfmt) return 0;
    const vkn_vfmt *vf = vkn_vfmt_info(vfmt);
    if (!vf) return 1;
    for (uint32_t i = 0; i < sh->ninputs && i < 16; i++) { const uint32_t l = sh->inputs[i]; if (l < VKN_VFMT_LOCS && vf->loc_class[l] && vf->loc_class[l] - 1u != sh->input_classes[i]) return 1; }
    return 0;
}
static uint64_t g_fx_tns;                 // the cost of one timer read (ns), measured at install
static void eo_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    if (!g_wtag || !ctx->depth || ctx->frame != g_wF || !vkn_rec_presented()) return;
    const uint64_t t0 = (++g_c.fx_calls & 63) == 0 ? vkn_now_ns() : 0;
    int nc = 0; for (int i = 0; i < VKN_PASS_COLORS; i++) nc += ctx->color[i] != NULL;
    // (depth-only sets reach this function on their first draw only - the gate, DM_VK_NATIVE_FXGATE=1: that is all it needs of them)
    if (!g_wphase && !nc && is_scene_depth(ctx->depth)) { g_wT = ctx->depth; g_wphase = 1; g_wvw = in->viewport[2]; g_wvh = in->viewport[3]; }
    if (ctx->depth == g_wT && g_wphase) {
        if (!nc) {
            g_c.pre_draws++;
            if (in->viewport[0] != 0.0f || in->viewport[1] != 0.0f || in->viewport[2] != g_wvw || in->viewport[3] != g_wvh) g_wvbad = 1;   // (one viewport from 0, 0)
        } else if (nc >= 3) {
            g_c.gb_draws++; if (!g_wgb) g_wgb = ctx->color[0];
            const uint64_t key = vkn_query_draw_cond_key();
            if (key && g_wlab && g_anim_on && g_km && anim_shader(ctx->shader)) {   // [v2] an animated object: no occluder (vkn_eocc_after mapped its pair)
                g_c.anim_draws++;
                const kmapf *m = &g_km[g_wL % KMF];
                if (__atomic_load_n(&m->frame, __ATOMIC_ACQUIRE) == g_wL) {
                    uint32_t i = (uint32_t)((key >> 4) * 0x9E3779B97F4A7C15ull >> 51) & (KMAP - 1);
                    for (int n = 0; n < 32; n++, i = (i + 1) & (KMAP - 1)) {
                        const uint64_t k = __atomic_load_n(&m->k[i], __ATOMIC_ACQUIRE);
                        if (!k) break;
                        if (k == key) { const uint32_t ei = __atomic_load_n(&m->e[i], __ATOMIC_ACQUIRE) & (TAB - 1); if (__atomic_exchange_n(&g_anim[ei], (uint32_t)g_wL, __ATOMIC_RELAXED) != (uint32_t)g_wL) g_c.anim_marked++; break; }
                    }
                }
            }
            if (g_mode >= 2 && g_wlab && pset_has(g_wL, key)) {
                g_c.gb_would++;
                if (g_mode == 3 && !out->shader) {
                    if (needs_retype(ctx->shader, ctx->vfmt)) g_c.untinted_retype++;
                    else { const vkn_shader *t = twin_of(ctx->shader); if (t) { out->shader = t; g_c.tinted++; } else g_c.untinted++; }
                }
            }
        } else g_c.other_draws++;
    } else g_c.other_draws++;
    if (t0) { const uint64_t dt = vkn_now_ns() - t0; g_c.fx_ns += 64 * (dt > g_fx_tns ? dt - g_fx_tns : 0); }
}
static int eo_gate_still(void) { return 1; }   // the gate always holds for depth-only sets (the edge call is all the function needs)

// ================================================================ install, census, tests
static float env_f(const char *n, float def, float lo, float hi) { const char *v = getenv(n); if (!v || !*v) return def; const float x = (float)atof(v); return x >= lo && x <= hi ? x : def; }
void vkn_eocc_init(int mode) {
    g_mode = mode >= 0 && mode <= 3 ? mode : 0;
    if (!g_mode) return;
    g_cfg.pad_px = (uint32_t)env_f("DM_VK_ENG_OCC_PAD", 16.0f, 0.0f, 64.0f);
    g_cfg.margin = env_f("DM_VK_ENG_OCC_MARGIN", 0.02f, 0.0f, 0.5f);
    g_cfg.sheet = env_f("DM_VK_ENG_OCC_SHEET", 0.05f, 0.0f, 1.0f);
    g_cfg.max_move = env_f("DM_VK_ENG_OCC_MOVE", 2.0f, 0.0f, 100.0f);
    g_cfg.max_turn = env_f("DM_VK_ENG_OCC_TURN", 8.0f, 0.0f, 90.0f);
    g_cfg.box_reproj = (int)env_f("DM_VK_ENG_OCC_BOX", 1.0f, 0.0f, 1.0f);
    g_hold = (uint32_t)env_f("DM_VK_ENG_OCC_HOLD", 4.0f, 0.0f, 120.0f);
    g_age = (uint32_t)env_f("DM_VK_ENG_OCC_AGE", 6.0f, 1.0f, 16.0f);
    g_budget_ns = (uint64_t)env_f("DM_VK_ENG_OCC_BUDGET_US", 2000.0f, 0.0f, 100000.0f) * 1000u;
    g_anim_on = (int)env_f("DM_VK_ENG_OCC_ANIM", 1.0f, 0.0f, 1.0f);
    if (!g_t && !(g_t = (ent *)calloc(TAB, sizeof *g_t))) { vkn_logf(EO_RED "eng occ: no memory for the object table: off"); g_mode = 0; return; }
    if (!g_anim) g_anim = (uint32_t *)calloc(TAB, sizeof *g_anim);
    if (!g_km) g_km = (kmapf *)calloc(KMF, sizeof *g_km);
    if (!g_anim || !g_km) { vkn_logf(EO_RED "eng occ: no memory for the animated-object maps: animated occluders not known - off"); g_mode = 0; return; }
    { uint64_t t = vkn_now_ns(), t1 = t; for (int i = 0; i < 64; i++) t1 = vkn_now_ns(); g_fx_tns = (t1 - t) / 64; }
    static int reg;
    if (!reg) {
        reg = 1;
        if (vkn_fx_register("eng-occ", eo_fx, NULL, 6)) vkn_logf(EO_RED "eng occ: the draw-effect chain refused \"eng-occ\": no snapshots (nothing hidden), no poison");
        (void)vkn_fx_gate("eng-occ", VKN_FX_G_NOC | VKN_FX_G_D, eo_gate_still);   // [v2] depth-only sets (pre-pass, shadow atlas): their first draw only
    }
    vkn_logf("eng occ (VK-ENGINE-SIDE I2): DM_VK_ENG_OCC=%d (%s): the main-view receiver tests each opaque object's box against the depth cells of an earlier frame; "
             "pad %u px, margin %.1f %%, sheet %.2f, camera cut beyond %.2f units / %.1f deg, visible objects held %u frames, snapshot age <= %u, budget %llu us a frame; "
             "v2: snapshot = the depth pre-pass as it ends, labelled with the cull frame of its report pairs; animated objects (non-affine vertex shader) no occluders %s",
             g_mode, g_mode == 1 ? "on" : g_mode == 2 ? "count: decided, nothing hidden" : "POISON: what it would hide is drawn magenta", g_cfg.pad_px, 100.0 * g_cfg.margin, (double)g_cfg.sheet,
             (double)g_cfg.max_move, (double)g_cfg.max_turn, g_hold, g_age, (unsigned long long)(g_budget_ns / 1000), g_anim_on ? "on" : "OFF (DM_VK_ENG_OCC_ANIM=0)");
}
void vkn_eocc_stats_get(vkn_eocc_stats *s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->frames = g_c.frames; s->calls = g_c.calls; s->tested = g_c.tested; s->hidden = g_c.hidden; s->would = g_c.would; s->poison = g_c.poison;
    memcpy(s->why, g_c.why, sizeof s->why); s->snaps = g_c.snaps; s->snap_fail = g_c.snap_fail + g_c.snap_vp_bad; s->checked = g_c.checked; s->violations = g_c.violations;
    s->gb_draws = g_c.gb_draws; s->gb_would = g_c.gb_would; s->pre_draws = g_c.pre_draws; s->twins = g_c.twins;
}
void vkn_eocc_test_reset(int mode) {
    g_mode = mode;
    if (!g_t) g_t = (ent *)calloc(TAB, sizeof *g_t); else memset(g_t, 0, TAB * sizeof *g_t);
    g_live = 0; memset(&g_c, 0, sizeof g_c); memset(g_cam, 0, sizeof g_cam); memset(g_dec, 0, sizeof g_dec); memset(g_dr, 0, sizeof g_dr); memset(g_ps, 0, sizeof g_ps);
    memset(g_stg, 0, sizeof g_stg); g_ready = g_prep = g_main = 0; g_pin[0] = g_pin[1] = -1;
    for (int i = 0; i < SNAPS; i++) { g_sn[i].state = 0; g_sn[i].eframe = g_sn[i].rframe = 0; g_sn[i].ts = 0; }
    g_wtag = 0; g_wlab = 0; g_enc_lab = 0; g_idle = 0; g_idle_skip = 0; memset(g_ash, 0, sizeof g_ash);
    if (!g_anim) g_anim = (uint32_t *)calloc(TAB, sizeof *g_anim); else memset(g_anim, 0, TAB * sizeof *g_anim);
    if (!g_km) g_km = (kmapf *)calloc(KMF, sizeof *g_km); else memset(g_km, 0, KMF * sizeof *g_km);
    static int reg;
    if (mode && !reg) { reg = 1; (void)vkn_fx_register("eng-occ", eo_fx, NULL, 6); }
}
float *vkn_eocc_test_slot(int i, uint64_t *eframe, uint64_t *rframe) {
    if (i < 0 || i >= SNAPS || !g_scpu) return NULL;
    if (eframe) *eframe = g_sn[i].eframe;
    if (rframe) *rframe = g_sn[i].rframe;
    return snap_max(i);
}
void vkn_eocc_census(void) {
    if (!g_mode) return;
    __typeof__(g_c) c; memcpy(&c, &g_c, sizeof c);
    // (the window restarts; racy by a few counts)
    memset(&g_c, 0, sizeof g_c);
    const double fr = c.frames ? (double)c.frames : 1.0, rr = c.recs ? (double)c.recs : 1.0;
    char b[2048]; int n = 0;
#define P(...) do { if (n < (int)sizeof b - 1) n += snprintf(b + n, sizeof b - (size_t)n, __VA_ARGS__); } while (0)
    P("eng occ: DM_VK_ENG_OCC=%d; a frame (%llu prepared): main-view objects %.0f (other stages %.0f), tested %.1f, HIDDEN %.1f", g_mode, (unsigned long long)c.frames,
      (double)c.calls / fr - (double)c.other_stage / fr, (double)c.other_stage / fr, (double)c.tested / fr, (double)(c.hidden + c.would + c.poison) / fr);
    if (g_mode != 1) P(" (%s, nothing hidden)", g_mode == 2 ? "would" : "drawn magenta");
    P(", of them under a moved camera %.1f; not hidden because:", (double)c.moved_hidden / fr);
    for (int i = 1; i < VKN_EO_R_N; i++) if (c.why[i]) P(" %s %.1f,", vkn_eo_reason(i), (double)c.why[i] / fr);
    vkn_logf("%s", b); n = 0;
    P("eng occ (cont.): frames without tests:");
    for (int i = 1; i < VKN_EO_R_N; i++) if (c.fwhy[i]) P(" %s %.0f %%,", vkn_eo_reason(i), 100.0 * (double)c.fwhy[i] / fr);
    P(" | tested under a moved camera (reprojection) in %.0f %% of frames; snapshot age %.1f frames; hazards %.1f a frame (animated %.1f, whole screen %llu)",
      100.0 * (double)c.moved_frames / fr, c.age_n ? (double)c.age_sum / (double)c.age_n : 0.0, (double)c.hazards / fr, (double)c.hz_anim / fr, (unsigned long long)c.hazard_whole);
    { static const char *const fn[VKN_EO_FL_N] = {"not opaque", "", "alpha / overlay", "parts", "other passes", "special", "bit 0", "character"};
      P(" | flags refusals a frame:"); for (int i = 0; i < VKN_EO_FL_N; i++) if (i != VKN_EO_FL_OK && c.fl[i]) P(" %s %.1f,", fn[i], (double)c.fl[i] / fr); }
    P(" | snapshot label (cull frame - frame at the draw's begin) -2..+2: %llu / %llu / %llu / %llu / %llu, farther %llu, no pair %llu; animated G-buffer draws %.1f a frame, objects marked %.1f",
      (unsigned long long)c.lab_d[0], (unsigned long long)c.lab_d[1], (unsigned long long)c.lab_d[2], (unsigned long long)c.lab_d[3], (unsigned long long)c.lab_d[4],
      (unsigned long long)c.lab_far, (unsigned long long)c.lab_none, (double)c.anim_draws / rr, (double)c.anim_marked / rr);
    vkn_logf("%s", b);
    n = 0;
    const double gbw = (double)c.gb_would / rr, ratio = c.gb_draws ? (double)c.pre_draws / (double)c.gb_draws : 1.0;
    P("eng occ (cont.): draws a presented frame: pre-pass %.0f, G-buffer %.0f, other %.0f", (double)c.pre_draws / rr, (double)c.gb_draws / rr, (double)c.other_draws / rr);
    if (g_mode >= 2) P("; G-buffer draws of the objects it would hide %.1f (pre-pass ~%.1f by the pass ratio)%s", gbw, gbw * ratio,
                       g_mode == 3 ? "" : "");
    if (g_mode == 3) P("; tinted %.1f, untinted %.1f (vertex retype %.1f), twins made %llu (failed %llu); MAGENTA PIXELS VISIBLE (the truth): %.0f a frame, in %.1f %% of "
                       "%llu measured frames, at most %llu", (double)c.tinted / rr, (double)c.untinted / rr, (double)c.untinted_retype / rr, (unsigned long long)c.twins,
                       (unsigned long long)c.twin_fail, c.mag_frames ? (double)c.mag_px / (double)c.mag_frames : 0.0, c.mag_frames ? 100.0 * (double)c.mag_any / (double)c.mag_frames : 0.0,
                       (unsigned long long)c.mag_frames, (unsigned long long)c.mag_max);
    vkn_logf("%s", b); n = 0;
    P("eng occ (cont.): self-check: decisions checked %.1f a frame, VIOLATIONS %llu (held drawn %llu), lost %llu", (double)c.checked / fr, (unsigned long long)c.violations, (unsigned long long)c.held_viol,
      (unsigned long long)c.check_lost);
    P(" | COST us a frame: job threads %.0f (preparation %.0f, tests %.0f, slowest test %.1f, cells read %.0f), recording thread %.1f (cells pass record %.1f, draw function %.1f), GPU cells pass %.1f",
      (double)(c.setup_ns + c.test_ns) / fr / 1e3, (double)c.setup_ns / fr / 1e3, (double)c.test_ns / fr / 1e3, (double)c.test_max_ns / 1e3, (double)c.cells / fr,
      (double)(c.snap_ns + c.fx_ns) / rr / 1e3, (double)c.snap_ns / rr / 1e3, (double)c.fx_ns / rr / 1e3, c.gpu_n ? (double)c.gpu_ns / (double)c.gpu_n / 1e3 : -1.0);
    P(" | snapshots %llu (failed %llu, viewport not one from 0,0 %llu, skipped while idle %llu); table %u live, full %llu; decision log full %llu, key set full %llu",
      (unsigned long long)c.snaps, (unsigned long long)c.snap_fail, (unsigned long long)c.snap_vp_bad, (unsigned long long)c.snap_idle, __atomic_load_n(&g_live, __ATOMIC_RELAXED), (unsigned long long)c.table_full,
      (unsigned long long)c.dec_full, (unsigned long long)c.pset_full);
#undef P
    vkn_logf("%s", b);
}
#endif
