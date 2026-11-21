// [DOS2] VK-NATIVE stage 5, technique A (carried verdict with proof, docs/dos2/nr/VK-OCCLUSION-ALT.md section 4). Contract, rules
// and premises: vkn_occ.h. Sections: gather hand-off, read mask, pure model (grid, sequence, objects), glue (effect functions, draw
// content hash, the query module's side), census. No game addresses.
#ifdef DM_GAME_DOS2
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkn_occ.h"
#include "vkn_query.h"
#include "vkn_fx.h"
#include "vkn_buf.h"
#include "vkn_tex.h"
#include "vkn_state.h"
#include "vkn_rt.h"
#include "vkn_frame.h"
#include "../nr/nr_occ.h"

#define O_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
extern int vkn_shlib_on(void) __attribute__((weak));
extern int vkn_shlib_fs_facts(const void *shader) __attribute__((weak));
extern int vkn_shlib_read_mask(const void *shader, uint32_t binding, const uint64_t **bits, uint32_t *nwords) __attribute__((weak));

enum { SEQ = 4096, SMAP = 8192, OBJS = 8192, OPROBE = 24, OCCM = 8192, UMEMO = 64, REDS = 16 };
// Refusal reasons of a pair draw whose object has a hidden verdict (vkn_occ_stats.refuse[]).
enum { RF_OK = 0, RF_GKEY, RF_GTORN, RF_GCONFLICT, RF_GSTALE, RF_GHALF, RF_RT2, RF_NOCAM, RF_BEHIND, RF_LATE, RF_TARGET, RF_VIEWPORT, RF_CAMERA,
       RF_UNHASHED, RF_BROKEN, RF_QUERIED, RF_UNVERSIONED, RF_NEW, RF_MOVED, RF_CHANGED, RF_RECT, RF_DIRTY, RF_NOVERDICT, RF_VISIBLE, RF_N };
static const char *const k_why[RF_N] = {"", "key outside the pair table", "gather torn", "gathered twice", "gather of another frame",
    "halves not alternating", "DM_VK_NATIVE_RT=2", "no camera", "box behind the camera", "pair after the conditional draws",
    "other depth target", "other viewport", "other camera", "frame not hashed", "pair cut / nested", "queried now", "input without a version",
    "new draw", "out of order", "content changed", "rect changed", "dirt since the verdict", "", ""};
static const char *const k_wwhy[VKN_OCC_W_N] = {"camera", "viewport", "target", "clear changed", "no depth clear", "stencil not cleared",
    "previous frame not hashed", "draw without rect changed", "sequence overflow", "a piece made a draw write depth", "target found late"};

static vkn_occ_stats g_st;
static int g_mode, g_budget = -1, g_budget_asked = -1;
static uint8_t g_red[4];
#define ORED(i, ...) do { if (g_red[i] < REDS) { g_red[i]++; vkn_logf(O_RED "occ-a: " __VA_ARGS__); } } while (0)
static inline uint64_t mix(uint64_t h, uint64_t v) { return nro_mix(h, v); }

// ================================================================ gather hand-off (any thread, lock-free)
typedef struct gslot { uint32_t seq, why; uint64_t eframe, obj, vph, conflict; uint32_t ndc[4]; } gslot;
static gslot g_gs[2][VKN_OCC_PER_HALF];
static uint64_t g_gbase[2], g_glatest[2];
static uint64_t g_gst[VKN_OCC_G_N];
#define ALD(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define AST(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)

void vkn_occ_gather_bases(uint64_t base0, uint64_t base1) { AST(&g_gbase[0], base0); AST(&g_gbase[1], base1); }
static int half_of(uint64_t key, uint32_t *k) {
    for (int h = 0; h < 2; h++) {
        const uint64_t b = ALD(&g_gbase[h]);
        if (b && key >= b && key < b + 16ull * VKN_OCC_PER_HALF && !((key - b) & 15)) { *k = (uint32_t)((key - b) >> 4); return h; }
    }
    return -1;
}
int vkn_occ_project(const float a[6], const float vp[16], float ndc[4]) {
    for (int i = 0; i < 6; i++) if (!isfinite(a[i])) return -2;
    if (a[0] > a[3] || a[1] > a[4] || a[2] > a[5]) return -2;
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int i = 0; i < 8; i++) {
        const float px = a[(i & 1) ? 3 : 0], py = a[(i & 2) ? 4 : 1], pz = a[(i & 4) ? 5 : 2];
        const float cx = px * vp[0] + py * vp[4] + pz * vp[8] + vp[12];
        const float cy = px * vp[1] + py * vp[5] + pz * vp[9] + vp[13];
        const float cw = px * vp[3] + py * vp[7] + pz * vp[11] + vp[15];
        if (!(cw > 1e-6f)) return -1;   // (NaN too)
        const float nx = cx / cw, ny = cy / cw;
        if (!isfinite(nx) || !isfinite(ny)) return -1;
        x0 = nx < x0 ? nx : x0; x1 = nx > x1 ? nx : x1; y0 = ny < y0 ? ny : y0; y1 = ny > y1 ? ny : y1;
    }
    ndc[0] = x0; ndc[1] = y0; ndc[2] = x1; ndc[3] = y1;
    return 0;
}
int vkn_occ_gather(uint64_t key, uint64_t eframe, uint64_t obj, const float aabb[6], const float vp[16]) {
    uint32_t k = 0; const int h = half_of(key, &k);
    if (h < 0) { __atomic_add_fetch(&g_gst[VKN_OCC_G_KEY], 1, __ATOMIC_RELAXED); return VKN_OCC_G_KEY; }
    uint64_t cur = __atomic_load_n(&g_glatest[h], __ATOMIC_ACQUIRE);
    while (eframe > cur && !__atomic_compare_exchange_n(&g_glatest[h], &cur, eframe, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    if (eframe < cur) { __atomic_add_fetch(&g_gst[VKN_OCC_G_STALE], 1, __ATOMIC_RELAXED); return VKN_OCC_G_STALE; }
    float ndc[4] = {0, 0, 0, 0}; uint32_t why = VKN_OCC_G_OK; uint64_t vph = 0;
    if (!vp) why = VKN_OCC_G_NOCAM;
    else {
        const int r = vkn_occ_project(aabb, vp, ndc);
        why = r == -2 ? VKN_OCC_G_BOX : r == -1 ? VKN_OCC_G_BEHIND : VKN_OCC_G_OK;
        vph = nro_hash_bytes(0x56504831ull, vp, 64) | 1;
    }
    gslot *s = &g_gs[h][k];
    uint32_t q = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    if ((q & 1) || !__atomic_compare_exchange_n(&s->seq, &q, q + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {   // another writer right now
        AST(&s->conflict, eframe); __atomic_add_fetch(&g_gst[VKN_OCC_G_CONFLICT], 1, __ATOMIC_RELAXED); return VKN_OCC_G_CONFLICT;
    }
    int ret = (int)why;
    if (ALD(&s->eframe) == eframe) { AST(&s->conflict, eframe); ret = VKN_OCC_G_CONFLICT; }   // the same entry twice in one frame
    AST(&s->eframe, eframe); AST(&s->obj, obj); AST(&s->vph, vph); AST(&s->why, why);
    uint32_t nb[4]; memcpy(nb, ndc, sizeof nb); for (int i = 0; i < 4; i++) AST(&s->ndc[i], nb[i]);
    __atomic_store_n(&s->seq, q + 2, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_gst[ret], 1, __ATOMIC_RELAXED);
    return ret;
}
// The recording thread's read: 0 = a consistent gather of this pair's own allocation, else R_*. *o filled when the object is known.
static int gather_get(uint64_t key, gslot *o) {
    uint32_t k = 0; const int h = half_of(key, &k);
    if (h < 0) return RF_GKEY;
    const gslot *s = &g_gs[h][k];
    int ok = 0;
    for (int t = 0; t < 3 && !ok; t++) {
        const uint32_t q1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (q1 & 1) continue;
        o->eframe = ALD(&s->eframe); o->obj = ALD(&s->obj); o->vph = ALD(&s->vph); o->why = ALD(&s->why); o->conflict = ALD(&s->conflict);
        for (int i = 0; i < 4; i++) o->ndc[i] = ALD(&s->ndc[i]);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        ok = ALD(&s->seq) == q1 && q1;
    }
    if (!ok) return RF_GTORN;
    if (o->conflict == o->eframe) return RF_GCONFLICT;
    // the hand-off rule (vkn_occ.h): no gather of a later frame began on this half, and the other half holds the frame next to it
    const uint64_t lh = __atomic_load_n(&g_glatest[h], __ATOMIC_ACQUIRE), lo = __atomic_load_n(&g_glatest[h ^ 1], __ATOMIC_ACQUIRE);
    if (lh != o->eframe) return RF_GSTALE;
    if (!(lo + 1 == o->eframe || o->eframe + 1 == lo)) return RF_GHALF;
    if (vkn_rt_mode() == VKN_RT_AHEAD2) return RF_RT2;
    if (o->why == VKN_OCC_G_NOCAM) return RF_NOCAM;
    if (o->why) return RF_BEHIND;
    return RF_OK;
}

// ================================================================ read mask (pure)
// As uniform_extent of vkn_shlib.cpp (the translator's spirv_uniform_extent), per element instead of the highest one.
int vkn_occ_spv_read_mask(const uint32_t *w, size_t n, uint32_t binding, uint64_t *bits, uint32_t nwords) {
    if (!w || n < 5 || w[0] != 0x07230203u) return -1;
    uint32_t var = 0, var_type = 0; const uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) return -1;
    // pass 1: the variable with this Binding and its type chain
    uint32_t *bind_of = calloc(bound, 4), *stride = calloc(bound, 4), *pointee = calloc(bound, 4), *mem0 = calloc(bound, 4), *cval = calloc(bound, 4);
    uint8_t *isc = calloc(bound, 1), *isuv = calloc(bound, 1), *hasb = calloc(bound, 1);
    uint32_t *vtype = calloc(bound, 4);
    int ret = -1;
    if (!bind_of || !stride || !pointee || !mem0 || !cval || !isc || !isuv || !hasb || !vtype) goto out;
    for (size_t i = 5; i < n;) {
        const uint32_t c = w[i] >> 16, op = w[i] & 65535;
        if (!c || i + c > n) goto out;
        switch (op) {
        case 71: if (c >= 4 && w[i + 1] < bound) { if (w[i + 2] == 33) { bind_of[w[i + 1]] = w[i + 3]; hasb[w[i + 1]] = 1; } else if (w[i + 2] == 6) stride[w[i + 1]] = w[i + 3]; } break;
        case 43: if (c >= 4 && w[i + 2] < bound) { cval[w[i + 2]] = w[i + 3]; isc[w[i + 2]] = 1; } break;
        case 30: if (c >= 3 && w[i + 1] < bound) mem0[w[i + 1]] = w[i + 2]; break;
        case 32: if (c >= 4 && w[i + 1] < bound) pointee[w[i + 1]] = w[i + 3]; break;
        case 59: if (c >= 4 && w[i + 3] == 2 && w[i + 2] < bound) { isuv[w[i + 2]] = 1; vtype[w[i + 2]] = w[i + 1]; } break;
        default: break;
        }
        i += c;
    }
    for (uint32_t id = 1; id < bound; id++) if (isuv[id] && hasb[id] && bind_of[id] == binding) { var = id; var_type = vtype[id]; break; }
    if (!var) goto out;
    ret = 0;   // found: whole unless proven otherwise
    if (var_type >= bound || pointee[var_type] >= bound) goto out;
    { const uint32_t st = pointee[var_type], m0 = st < bound ? mem0[st] : 0; if (!m0 || m0 >= bound || stride[m0] != 16) goto out; }
    memset(bits, 0, sizeof *bits * nwords);
    // pass 2: every use of the variable
    for (size_t i = 5; i < n;) {
        const uint32_t c = w[i] >> 16, op = w[i] & 65535;
        if (op == 59 || op == 5 || op == 6 || op == 15 || op == 71 || op == 72 || op == 332 || op == 5632) { i += c; continue; }
        for (uint32_t k = 1; k < c; k++) {
            if (w[i + k] != var) continue;
            int ok = (op == 65 || op == 66) && k == 3 && c >= 6;
            if (ok) {
                const uint32_t mi = w[i + 4], ei = w[i + 5];
                ok = mi < bound && ei < bound && isc[mi] && isc[ei] && cval[mi] == 0 && cval[ei] < 64u * nwords;
                if (ok) bits[cval[ei] >> 6] |= 1ull << (cval[ei] & 63);
            }
            if (!ok) { ret = 0; goto out; }
        }
        i += c;
    }
    ret = 1;
out:
    free(bind_of); free(stride); free(pointee); free(mem0); free(cval); free(isc); free(isuv); free(hasb); free(vtype);
    return ret;
}

// ================================================================ pure model
static uint32_t g_cell[VKN_OCC_GH][VKN_OCC_GW], g_whole, g_cmax, g_mf, g_wmask;
typedef struct sq { uint64_t key, content; vkn_occ_rect r; uint32_t seen; } sq;
static sq g_sa[SEQ], g_sb[SEQ]; static sq *g_prev = g_sa, *g_cur = g_sb; static uint32_t g_nprev, g_ncur;
static struct { uint64_t key; uint32_t gen, idx; } g_smap[SMAP]; static uint32_t g_sgen = 1;
static int32_t g_m = -1; static int g_prev_ok, g_over;
static struct { uint64_t key; int found, moved; uint32_t idx; } g_pre;

vkn_occ_rect vkn_occ_cells(const float ndc[4], float vw, float vh) {
    vkn_occ_rect r = {0, 0, 0, 0, 0};
    vw = fabsf(vw); vh = fabsf(vh);
    if (!(vw >= 1.0f) || !(vh >= 1.0f)) return r;
    for (int i = 0; i < 4; i++) if (!isfinite(ndc[i])) return r;
    const float mx = (ndc[2] - ndc[0]) * 0.1f + 16.0f / vw, my = (ndc[3] - ndc[1]) * 0.1f + 16.0f / vh;
    const float x0 = ndc[0] - mx, x1 = ndc[2] + mx, y0 = ndc[1] - my, y1 = ndc[3] + my;
    if (x1 < -1.0f || x0 > 1.0f || y1 < -1.0f || y0 > 1.0f) { r.kind = 2; return r; }
    int cx0 = (int)floorf((x0 + 1.0f) * 0.5f * VKN_OCC_GW), cx1 = (int)floorf((x1 + 1.0f) * 0.5f * VKN_OCC_GW);
    int cy0 = (int)floorf((y0 + 1.0f) * 0.5f * VKN_OCC_GH), cy1 = (int)floorf((y1 + 1.0f) * 0.5f * VKN_OCC_GH);
#define CL(v, hi) ((v) < 0 ? 0 : (v) > (hi) ? (hi) : (v))
    r.kind = 1; r.x0 = (uint8_t)CL(cx0, VKN_OCC_GW - 1); r.x1 = (uint8_t)CL(cx1, VKN_OCC_GW - 1); r.y0 = (uint8_t)CL(cy0, VKN_OCC_GH - 1); r.y1 = (uint8_t)CL(cy1, VKN_OCC_GH - 1);
#undef CL
    return r;
}
static int rect_eq(vkn_occ_rect a, vkn_occ_rect b) {
    return a.kind == b.kind && (a.kind != 1 || (a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1));
}
uint32_t vkn_occm_frame(void) { return g_mf; }
void vkn_occm_whole(int reason) { g_whole = g_mf; if (reason >= 0 && reason < VKN_OCC_W_N) g_wmask |= 1u << reason; }
static void dirty(vkn_occ_rect r) {
    if (r.kind == 2) return;
    if (r.kind == 0) { vkn_occm_whole(VKN_OCC_W_UNKNOWN_RECT); return; }
    for (uint32_t y = r.y0; y <= r.y1; y++) for (uint32_t x = r.x0; x <= r.x1; x++) if (g_cell[y][x] != g_mf) { g_cell[y][x] = g_mf; g_st.dirty_cells++; }
    g_cmax = g_mf;
}
uint32_t vkn_occm_maxdirt(vkn_occ_rect r) {
    uint32_t m = g_whole;
    if (r.kind == 2) return m;
    if (r.kind == 0) return g_cmax > m ? g_cmax : m;
    if (g_cmax <= m) return m;   // (no cell is newer than the whole-screen dirt)
    for (uint32_t y = r.y0; y <= r.y1; y++) for (uint32_t x = r.x0; x <= r.x1; x++) if (g_cell[y][x] > m) m = g_cell[y][x];
    return m;
}
static uint32_t smap_slot(uint64_t k) { return (uint32_t)(mix(0x534d4150ull, k) & (SMAP - 1)); }
static int smap_get(uint64_t k, uint32_t *idx) {
    for (uint32_t i = smap_slot(k), c = 0; c < SMAP; i = (i + 1) & (SMAP - 1), c++) {
        if (g_smap[i].gen != g_sgen) return 0;
        if (g_smap[i].key == k) { *idx = g_smap[i].idx; return 1; }
    }
    return 0;
}
static void smap_put(uint64_t k, uint32_t idx) {
    for (uint32_t i = smap_slot(k), c = 0; c < SMAP; i = (i + 1) & (SMAP - 1), c++) {
        if (g_smap[i].gen != g_sgen) { g_smap[i].gen = g_sgen; g_smap[i].key = k; g_smap[i].idx = idx; return; }
        if (g_smap[i].key == k) return;   // a duplicate key: the first one stays (the second is "new" next frame)
    }
}
void vkn_occm_frame_begin(void) {
    g_mf++; g_wmask = 0; g_ncur = 0; g_m = -1; g_over = 0; g_pre.key = 0; g_pre.found = 0;
    if (!g_prev_ok) vkn_occm_whole(VKN_OCC_W_UNHASHED);
}
int vkn_occm_pre(uint64_t key, vkn_occm_old *e, int *moved) {
    g_pre.key = key; g_pre.found = 0; g_pre.moved = 0;
    if (moved) *moved = 0;
    uint32_t idx = 0;
    if (!g_prev_ok || !smap_get(key, &idx) || idx >= g_nprev || g_prev[idx].seen == g_mf) return 0;
    sq *p = &g_prev[idx];
    if ((int32_t)idx > g_m) { for (uint32_t i = (uint32_t)(g_m + 1); i < idx; i++) if (g_prev[i].seen != g_mf) dirty(g_prev[i].r); }   // passed over
    else g_pre.moved = 1;
    g_pre.found = 1; g_pre.idx = idx;
    if (moved) *moved = g_pre.moved;
    if (e) { e->content = p->content; e->r = p->r; }
    return 1;
}
void vkn_occm_fin(uint64_t key, uint64_t content, vkn_occ_rect r) {
    if (g_pre.key != key || !key) vkn_occm_pre(key, NULL, NULL);
    g_pre.key = 0;
    if (g_ncur >= SEQ) { if (!g_over) vkn_occm_whole(VKN_OCC_W_OVERFLOW); g_over = 1; dirty(r); return; }
    if (g_pre.found) {
        sq *p = &g_prev[g_pre.idx]; p->seen = g_mf;
        if (g_pre.moved) { dirty(p->r); dirty(r); }
        else { g_m = (int32_t)g_pre.idx; if (p->content != content || !rect_eq(p->r, r)) { dirty(p->r); dirty(r); } }
    } else dirty(r);
    sq *c = &g_cur[g_ncur++]; c->key = key; c->content = content; c->r = r; c->seen = 0;
}
void vkn_occm_frame_end(int hashed) {
    if (g_prev_ok) for (uint32_t i = (uint32_t)(g_m + 1); i < g_nprev; i++) if (g_prev[i].seen != g_mf) dirty(g_prev[i].r);   // gone
    for (int i = 0; i < VKN_OCC_W_N; i++) if (g_wmask >> i & 1) g_st.whole[i]++;
    if (!hashed || g_over) { g_prev_ok = 0; g_nprev = 0; }
    else {
        sq *t = g_prev; g_prev = g_cur; g_cur = t; g_nprev = g_ncur; g_prev_ok = 1;
        if (++g_sgen == 0) { memset(g_smap, 0, sizeof g_smap); g_sgen = 1; }
        for (uint32_t i = 0; i < g_nprev; i++) smap_put(g_prev[i].key, i);
        g_st.hashed_frames++;
    }
    g_ncur = 0; g_m = -1; g_over = 0;
}

// ---- objects ----
typedef struct ob { uint64_t key; uint32_t seen, va, vn, learn, qf, idx; uint8_t vh; vkn_occ_rect r; } ob;
static ob g_ob[OBJS];
static int32_t ob_get(uint64_t key) {
    const uint32_t h = (uint32_t)(mix(0x4f424a31ull, key) & (OBJS - 1));
    int32_t victim = -1; uint32_t vseen = 0xffffffffu;
    for (uint32_t c = 0; c < OPROBE; c++) {
        const uint32_t i = (h + c) & (OBJS - 1);
        if (g_ob[i].key == key) return (int32_t)i;
        if (!g_ob[i].key) { victim = (int32_t)i; vseen = 0; break; }
        if (g_ob[i].seen + 600 < g_mf && g_ob[i].seen < vseen) { victim = (int32_t)i; vseen = g_ob[i].seen; }
    }
    if (victim < 0) return -1;   // (all recent: not modelled; counted by the caller as "no verdict")
    ob *o = &g_ob[victim]; memset(o, 0, sizeof *o); o->key = key;
    return victim;
}
void vkn_occ_objects(vkn_occ_census_v *v) {
    memset(v, 0, sizeof *v);
    for (uint32_t i = 0; i < OBJS; i++) {
        const ob *o = &g_ob[i]; if (!o->key || o->seen + 600 < g_mf) continue;
        v->objects++;
        if (!o->va) v->none++; else if (o->vh) { v->hidden++; if (vkn_occm_maxdirt(o->r) <= o->va) v->valid_hidden++; } else v->visible++;
    }
}

// ================================================================ glue: per-recording state
typedef struct pr { uint64_t pkey; int32_t ob; float ndc[4]; uint64_t vph, idx; vkn_occ_rect r; uint16_t nchain, nproven; uint8_t gat, elig, queried, rset; uint32_t why, a_used, maxd; } pr;
static pr g_pr[VKN_Q_RECS];
typedef struct qi { int32_t ob; uint64_t okey; uint32_t mf, a_used, maxd, n; vkn_occ_rect r; uint8_t elig, decided, valid; } qi;
static qi g_qi[VKN_Q_SLOTS][VKN_Q_CAP]; static uint32_t g_qn[VKN_Q_SLOTS];
static struct { uint64_t key; uint32_t gen, n; } g_occ[OCCM];
static int g_on, g_win, g_hash_on, g_first_done, g_scr_done, g_slot = -1, g_learn_left, g_any_pair;
static vkn_tex *g_T, *g_Tpred;
typedef struct scr { vkn_tex *t; uint32_t w, h; float vp[4]; uint64_t vph; uint8_t clr; uint32_t cdepth, cst; int valid; } scr;
static scr g_scr, g_scr_prev;
static uint8_t g_clr; static uint32_t g_cdepth, g_cst; static int g_stencil_used;
static struct { int on; uint32_t no; uint64_t key, early, outh; vkn_occ_rect r; uint8_t reached, recorded; } g_pd;
static uint32_t g_chain_no, g_untracked_no = 0xffffffffu; static int g_untracked;
static struct { uint64_t key; uint32_t gen; uint64_t h; } g_um[UMEMO];
static uint32_t g_tick;
static int (*g_t_mask)(const vkn_shader *s, int src, uint32_t binding, const uint64_t **bits, uint32_t *nwords);
static int (*g_t_facts)(const vkn_shader *s);

int vkn_occ_mode(void) { return g_mode; }
int vkn_occ_budget(void) { return g_budget; }
void vkn_occ_stats_get(vkn_occ_stats *s) {
    if (!s) return;
    *s = g_st;
    for (int i = 0; i < VKN_OCC_G_N; i++) s->gather[i] = __atomic_load_n(&g_gst[i], __ATOMIC_RELAXED);
}
void vkn_occ_test_fns(int (*mask)(const vkn_shader *, int, uint32_t, const uint64_t **, uint32_t *), int (*facts)(const vkn_shader *)) { g_t_mask = mask; g_t_facts = facts; }
void vkn_occ_test_reset(void) {
    memset(g_cell, 0, sizeof g_cell); g_whole = g_cmax = g_mf = g_wmask = 0;
    g_nprev = g_ncur = 0; g_prev_ok = 0; g_over = 0; g_m = -1; memset(g_smap, 0, sizeof g_smap); g_sgen = 1;
    memset(g_ob, 0, sizeof g_ob); memset(g_pr, 0, sizeof g_pr); memset(g_qi, 0, sizeof g_qi); memset(g_qn, 0, sizeof g_qn);
    memset(g_gs, 0, sizeof g_gs); memset(g_glatest, 0, sizeof g_glatest); memset(g_gst, 0, sizeof g_gst);
    memset(&g_st, 0, sizeof g_st); memset(&g_pd, 0, sizeof g_pd); memset(g_red, 0, sizeof g_red);
    g_T = g_Tpred = NULL; memset(&g_scr, 0, sizeof g_scr); memset(&g_scr_prev, 0, sizeof g_scr_prev); g_on = g_win = 0;
}
static uint32_t occ_next(uint64_t k) {
    for (uint32_t i = (uint32_t)(mix(0x4f434331ull, k) & (OCCM - 1)), c = 0; c < OCCM; i = (i + 1) & (OCCM - 1), c++) {
        if (g_occ[i].gen != g_mf) { g_occ[i].gen = g_mf; g_occ[i].key = k; g_occ[i].n = 1; return 0; }
        if (g_occ[i].key == k) return g_occ[i].n++;
    }
    return 0xffffffffu;
}

// ---- draw facts ----
static int ds_writes(uint16_t id, int *stencil) {   // 1 = the state can change the depth / stencil target
    if (!id) return 0;
    uint8_t a[8];
    if (vkn_ds_args(id, a)) return 1;   // unknown: as writing
    if (a[3]) { if (stencil) *stencil = 1; return 1; }
    return a[0] && a[1];
}
static int fs_relevant(const vkn_shader *s, const vkn_draw_in *in) {
    if (!(s->stages & (1u << VKN_SRC_FS))) return 0;
    if (in->alpha_ref >= 0.0f) return 1;
    int f = -1;
    if (g_t_facts) f = g_t_facts(s);
    else if (vkn_shlib_on && vkn_shlib_on() && vkn_shlib_fs_facts) f = vkn_shlib_fs_facts(s->xs[VKN_SRC_FS]);
    return f != 0;
}
static int rmask(const vkn_shader *s, int src, uint32_t binding, const uint64_t **bits, uint32_t *nw) {
    if (g_t_mask) return g_t_mask(s, src, binding, bits, nw);
    if (vkn_shlib_on && vkn_shlib_on() && vkn_shlib_read_mask) return vkn_shlib_read_mask(s->xs[src], binding, bits, nw);
    return -1;
}
static uint64_t ubo_hash(int st, uint32_t slot, const vkn_bind *b, const vkn_shader *s, int src, int *unver) {
    uint32_t off = 0, size = 0;
    if (vkn_draw_ubo_get(st, slot, &off, &size)) return mix(0x55420000ull + slot, (uint64_t)st);   // never announced: the zero block
    const uint32_t lim = b->bytes ? b->bytes : 65536u, n = size < lim ? size : lim;
    const uint64_t *bits = NULL; uint32_t nw = 0;
    const int mk = rmask(s, src, b->binding, &bits, &nw);
    const uint64_t mkey = mix(mix(mix(0x554d454dull, (uint64_t)st << 32 | slot), (uint64_t)off << 32 | n), (uint64_t)(uintptr_t)(mk == 1 ? bits : NULL));
    uint64_t *memo = NULL;
    { const uint32_t i = (uint32_t)(mkey & (UMEMO - 1)); if (g_um[i].gen == g_mf && g_um[i].key == mkey) { g_st.ubo_memo++; return g_um[i].h; } g_um[i].gen = g_mf; g_um[i].key = mkey; memo = &g_um[i].h; }
    const uint8_t *ring = (const uint8_t *)vkn_ring_cpu();
    if (!ring || (uint64_t)off + n > VKN_RING_BYTES) { *unver = 1; g_um[mkey & (UMEMO - 1)].gen = 0; return 0; }
    uint64_t h = mix(0x55424f31ull, n);
    if (mk == 1 && bits) {
        for (uint32_t wi = 0; wi < nw; wi++) for (uint64_t m = bits[wi]; m; m &= m - 1) {
            const uint32_t e = wi * 64u + (uint32_t)__builtin_ctzll(m), at = e * 16u;
            if (at >= n) continue;
            const uint32_t len = n - at < 16 ? n - at : 16;
            h = nro_hash_bytes(mix(h, e), ring + off + at, len); g_st.ubo_bytes += len;
        }
    } else { h = nro_hash_bytes(h, ring + off, n); g_st.ubo_bytes += n; }
    *memo = h;
    return h;
}
// The content hash (vkn_occ.h). *unver = an input without a version.
static uint64_t content_of(const vkn_draw_in *in, const vkn_fx_ctx *ctx, int *unver) {
    const vkn_shader *sh = ctx->shader;
    if (!sh) { *unver = 1; return 0; }
    uint32_t ar; memcpy(&ar, &in->alpha_ref, 4);
    uint64_t h = mix(0x4f434341ull, (uint64_t)sh->id | (uint64_t)vkn_h_idx(sh->handle) << 32);
    h = mix(h, sh->handle);
    h = mix(h, (uint64_t)ctx->blend | (uint64_t)ctx->ds << 16 | (uint64_t)ctx->raster << 32 | (uint64_t)ctx->vfmt << 48);
    h = mix(h, (uint64_t)ctx->prim | (uint64_t)in->stencil_ref << 8 | (uint64_t)ctx->indexed << 16 | (uint64_t)ar << 32);
    h = mix(h, (uint64_t)ctx->first | (uint64_t)ctx->count << 32); h = mix(h, ctx->instances);
    h = nro_hash_bytes(h, in->viewport, sizeof in->viewport); h = nro_hash_bytes(h, in->scissor, sizeof in->scissor);
    const vkn_vfmt *vf = ctx->vfmt ? vkn_vfmt_info(ctx->vfmt) : NULL;
    for (uint32_t i = 0; vf && i < vf->nbind; i++) {
        const uint32_t st = vf->bind[i].binding; if (st >= 3) continue;
        int none = 1; const uint64_t v = vkn_buf_version(VKN_BUF_VB, in->vb[st], &none);
        h = mix(mix(h, in->vb[st]), (uint64_t)in->vb_off[st] | (uint64_t)st << 32 | (uint64_t)vf->stride[st] << 40); h = mix(h, v);
        if (none) *unver = 1;
    }
    if (ctx->indexed) { int none = 1; const uint64_t v = vkn_buf_version(VKN_BUF_IB, in->ib, &none); h = mix(mix(h, in->ib), v); if (none) *unver = 1; }
    const int fsr = fs_relevant(sh, in);
    for (uint32_t i = 0; i < sh->nbind; i++) {
        const vkn_bind *b = &sh->bind[i];
        const int st = b->stage == 0 ? 0 : b->stage == 1 ? 1 : -1;
        if (st < 0 || (st == 1 && !fsr)) continue;
        const int src = st == 0 ? VKN_SRC_VS : VKN_SRC_FS;
        for (uint32_t e = 0; e < b->count; e++) {
            const uint32_t slot = (uint32_t)b->slot + e;
            switch (b->kind) {
            case VKN_BK_UNIFORM:
                if (slot >= VKN_UBO_SLOTS) { *unver = 1; break; }
                h = mix(h, ubo_hash(st, slot, b, sh, src, unver));
                break;
            case VKN_BK_SUPPORT: case VKN_BK_C1: break;   // the viewport size (hashed) / the shader's own constants (its object)
            case VKN_BK_TEXTURE: {
                if (slot >= VKN_TEX_SLOTS) { *unver = 1; break; }
                const int32_t id = in->tex[st] ? in->tex[st][slot] : -1, sid = in->smp[st] ? in->smp[st][slot] : -1;
                h = mix(h, (uint64_t)(uint32_t)id | (uint64_t)(uint32_t)sid << 32);
                if (id >= 0) {
                    vkn_tex *tx = NULL; (void)vkn_tex_view_by_id((uint32_t)id, &tx);
                    h = mix(h, (uint64_t)(uintptr_t)tx);
                    if (!tx || tx->pass_ever) *unver = 1;   // written by the GPU: no upload number stands for its texels
                    else h = mix(h, __atomic_load_n(&tx->shc_up, __ATOMIC_ACQUIRE));
                }
                if (sid >= 0) h = mix(h, (uint64_t)(uintptr_t)vkn_sampler_by_id((uint32_t)sid));
                break;
            }
            default: *unver = 1; break;   // images / storage / driver tables: no version
            }
        }
    }
    return h;
}
static uint64_t draw_hash(const vkn_draw_in *in, const vkn_fx_ctx *ctx) {   // the old path's draw identity (vkn_query.c occ_fx)
    uint64_t h = mix(0x6f636341ull, (uint64_t)ctx->prim | (uint64_t)ctx->count << 32);
    h = mix(h, (uint64_t)ctx->first | (uint64_t)ctx->instances << 32);
    h = mix(h, ctx->indexed ? in->ib : 0);
    for (int i = 0; i < 3; i++) h = mix(h, in->vb[i] ^ ((uint64_t)in->vb_off[i] << 40));
    return mix(h, in->shader);
}
static uint64_t out_hash(const vkn_fx_out *o) {
    static const vkn_fx_out z;
    if (!memcmp(o, &z, sizeof z)) return 0;
    uint64_t h = mix(0x4f555431ull, (uint64_t)o->discard | (uint64_t)o->no_fs << 8 | (uint64_t)o->ds << 16);
    h = mix(h, o->shader ? (uint64_t)o->shader->id : 0);
    h = nro_hash_bytes(h, o->cmp_mask, sizeof o->cmp_mask);
    if (o->shader) h = mix(h, 0x5ced0000ull + g_mf);   // a piece's shader with its own inputs: no version
    return h | 1;
}
static void finalize(void) {
    if (!g_pd.on) return;
    g_pd.on = 0;
    const uint64_t fin = g_pd.recorded ? (g_pd.outh ? mix(g_pd.early, g_pd.outh) : g_pd.early) : mix(g_pd.early, 0x5ced0001ull);
    vkn_occm_fin(g_pd.key, fin, g_pd.r);
}
static void close_window(void) { finalize(); g_win = 0; }
static void stop_hashing(int reason) { finalize(); vkn_occm_whole(reason); g_hash_on = 0; }

// ---- the screen identity (first pair draw of the frame) ----
static void screen_check(const vkn_draw_in *in, const vkn_fx_ctx *ctx, const pr *p) {
    g_scr_done = 1;
    scr c; memset(&c, 0, sizeof c);
    c.t = ctx->depth; c.w = ctx->depth->w; c.h = ctx->depth->h; memcpy(c.vp, in->viewport, sizeof c.vp); c.vph = p->gat ? p->vph : 0;
    c.clr = g_clr; c.cdepth = g_cdepth; c.cst = g_cst; c.valid = 1;
    const scr *o = &g_scr_prev;
    if (!c.vph) { stop_hashing(VKN_OCC_W_CAMERA); }
    else if (o->valid) {
        if (o->vph != c.vph) stop_hashing(VKN_OCC_W_CAMERA);
        else if (o->t != c.t || o->w != c.w || o->h != c.h) vkn_occm_whole(VKN_OCC_W_TARGET);
        else if (memcmp(o->vp, c.vp, sizeof c.vp)) vkn_occm_whole(VKN_OCC_W_VIEWPORT);
        else if (o->clr != c.clr || o->cdepth != c.cdepth || o->cst != c.cst) vkn_occm_whole(VKN_OCC_W_CLEAR);
    }
    if (!(c.clr & 1)) vkn_occm_whole(VKN_OCC_W_NOCLEAR);
    g_scr = c;
}

// ---- effect functions ----
static void pair_rect(pr *p, const vkn_draw_in *in) {
    p->rset = 1;
    if (p->gat) { float n[4]; memcpy(n, p->ndc, sizeof n); p->r = vkn_occ_cells(n, in->viewport[2], in->viewport[3]); }
    else p->r.kind = 0;
    if (p->ob >= 0) g_ob[p->ob].r = p->r;
}
static int all_proven(const pr *p, uint32_t rec) {
    uint32_t seen = 0, done = 0; int closed = 0, empty = 0;
    if (vkn_query_occ_rec(rec, &seen, &done, &closed, &empty)) return 0;
    const ob *o = p->ob >= 0 ? &g_ob[p->ob] : NULL;
    return p->elig && closed && p->nchain && p->nproven == p->nchain && seen == p->nchain && o && o->va && o->vh && o->vn == p->nchain;
}
static void cond_draw(int cond, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    if (g_win) close_window();
    if (cond < 0) return;
    const pr *p = &g_pr[cond];
    if (!all_proven(p, (uint32_t)cond)) return;
    if (g_mode == VKN_COND_A) { out->skip = 1; g_st.skip_cond++; g_st.skip_idx += (uint64_t)ctx->count * ctx->instances; }
    else g_st.would_cond++;
}
static void pair_draw(int rec, const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    pr *p = &g_pr[rec];
    const uint32_t j = p->nchain++;
    p->idx += (uint64_t)ctx->count * ctx->instances;
    if (!p->pkey) { const uint64_t k = draw_hash(in, ctx); p->pkey = mix(mix(0x6e6f6761ull, k), occ_next(mix(k, 0x6e6fu))) | 1; }
    if (!g_scr_done) screen_check(in, ctx, p);
    if (!p->rset) pair_rect(p, in);
    if (!g_hash_on) { if (p->elig) { p->elig = 0; p->why = RF_UNHASHED; } return; }
    g_st.tracked++; g_st.pair_draws++;
    vkn_occ_rect r = p->r;
    if (p->elig && memcmp(in->viewport, g_scr.vp, sizeof g_scr.vp)) { p->elig = 0; p->why = RF_VIEWPORT; }
    if (p->elig && p->vph != g_scr.vph) { p->elig = 0; p->why = RF_CAMERA; }
    if (!p->elig) r.kind = 0;
    int st = 0; (void)ds_writes(ctx->ds, &st); if (st) g_stencil_used = 1;
    if (st && !(g_clr & 2)) vkn_occm_whole(VKN_OCC_W_STENCIL);
    const uint64_t key = mix(p->pkey, j) | 1;
    int unver = 0; uint64_t early = content_of(in, ctx, &unver);
    if (unver) { early = mix(early, 0x5ced0002ull + g_mf); g_st.unversioned++; }
    vkn_occm_old e; int moved = 0; const int found = vkn_occm_pre(key, &e, &moved);
    const ob *o = p->ob >= 0 ? &g_ob[p->ob] : NULL;
    int why = RF_OK;
    if (!p->elig) why = p->why ? (int)p->why : RF_NOVERDICT;
    else if (g_mode == VKN_COND_A && p->queried) why = RF_QUERIED;
    else if (!o || !o->va) why = RF_NOVERDICT;
    else if (!o->vh) why = RF_VISIBLE;
    else if (unver) why = RF_UNVERSIONED;
    else if (!found) why = RF_NEW;
    else if (moved) why = RF_MOVED;
    else if (e.content != early) why = RF_CHANGED;
    else if (!rect_eq(e.r, r)) why = RF_RECT;
    else { const uint32_t d = vkn_occm_maxdirt(r); if (d > o->va) why = RF_DIRTY; if (!j) p->maxd = d; }
    if (why && o && o->va && o->vh && why < RF_N) g_st.refuse[why]++;
    if (!why) {
        p->nproven++;
        if (!j) p->a_used = o->va;
        if (g_mode == VKN_COND_A) {
            out->skip = 1; vkn_query_occ_skipped();
            vkn_occm_fin(key, early, r);   // a proven draw writes nothing: as if drawn unchanged
            g_st.skip_pre++; g_st.skip_idx += (uint64_t)ctx->count * ctx->instances;
            return;
        }
        g_st.would_pre++;
    }
    g_pd.on = 1; g_pd.no = ctx->draw_no; g_pd.key = key; g_pd.early = early; g_pd.outh = 0; g_pd.r = r; g_pd.reached = g_pd.recorded = 0;
}
static void plain_draw(const vkn_draw_in *in, const vkn_fx_ctx *ctx) {
    if (!g_hash_on) return;
    int st = 0;
    if (!ds_writes(ctx->ds, &st)) { g_untracked = 1; g_untracked_no = ctx->draw_no; return; }   // cannot change T (unless a piece swaps its state)
    if (st) { g_stencil_used = 1; if (!(g_clr & 2)) vkn_occm_whole(VKN_OCC_W_STENCIL); }
    g_st.tracked++;
    const uint64_t dh = draw_hash(in, ctx), key = mix(dh, occ_next(dh)) | 1;
    int unver = 0; uint64_t early = content_of(in, ctx, &unver);
    if (unver) { early = mix(early, 0x5ced0002ull + g_mf); g_st.unversioned++; }
    vkn_occm_pre(key, NULL, NULL);
    vkn_occ_rect r = {0, 0, 0, 0, 0};
    g_pd.on = 1; g_pd.no = ctx->draw_no; g_pd.key = key; g_pd.early = early; g_pd.outh = 0; g_pd.r = r; g_pd.reached = g_pd.recorded = 0;
}
#define T_BEGIN const int t_on_ = (++g_tick & 15) == 0; const uint64_t t0_ = t_on_ ? vkn_now_ns() : 0
#define T_END do { if (t_on_) g_st.cpu_ns += 16 * (vkn_now_ns() - t0_); } while (0)
static void fx_a(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    g_chain_no = ctx->draw_no;
    if (!g_on) return;
    const int open = vkn_query_occ_open(), cond = vkn_query_occ_cond();
    if (cond == -1 && !g_win) return;
    T_BEGIN;
    if (cond != -1) { cond_draw(cond, ctx, out); T_END; return; }
    vkn_tex *d = ctx->depth;
    if (!d || d != g_T) {
        if (open >= 0 && d && !g_any_pair) {   // the frame's first pair draw on another target than the last frame's: adopt it
            finalize(); g_T = d; vkn_occm_whole(g_Tpred ? VKN_OCC_W_TARGET : VKN_OCC_W_LATE_T); g_hash_on = 0;
        } else {
            if (open >= 0 && g_pr[open].elig) { g_pr[open].elig = 0; g_pr[open].why = RF_TARGET; }
            if (open >= 0) g_pr[open].nchain++;
            T_END; return;
        }
    }
    finalize();
    if (!g_first_done) { g_first_done = 1; g_clr = d->pass_dclr; uint32_t f; memcpy(&f, &d->pass_depth, 4); g_cdepth = f; g_cst = d->pass_stencil; }
    if (open >= 0) { g_any_pair = 1; pair_draw(open, in, ctx, out); } else plain_draw(in, ctx);
    T_END;
}
static void fx_post(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)in; (void)user;
    if (!g_on) return;
    if (g_pd.on && g_pd.no == ctx->draw_no) { g_pd.outh = out_hash(out); g_pd.reached = 1; return; }
    if (g_untracked && g_untracked_no == ctx->draw_no && g_win && g_hash_on && ctx->depth == g_T && out->ds && ds_writes(out->ds, NULL)) vkn_occm_whole(VKN_OCC_W_PIECE);
    g_untracked = 0;
}

// ---- the query module's side ----
void vkn_occ_q_draw_pre(void) { if (g_pd.on && g_pd.reached && g_pd.no == g_chain_no) g_pd.recorded = 1; }
void vkn_occ_q_rec_begin(int presented, int slot) {
    g_on = g_mode && presented;
    g_slot = slot;
    if (!g_on) return;
    if (g_budget < 0) {   // the family default, once the device is known
        const vkn_dev *dv = vkn_device();
        const char *sim = getenv("DM_GFX_SIM_GPU");
        const uint32_t v = dv ? dv->props.vendorID : 0;
        const int tiler = v == 0x13B5 || v == 0x1010 || (sim && (strstr(sim, "mali") || strstr(sim, "pvr") || strstr(sim, "powervr")));
        g_budget = g_budget_asked >= 0 ? g_budget_asked : tiler ? VKN_OCC_B_TILER : VKN_OCC_B_ADRENO;
        vkn_logf("occ-a: DM_VK_NATIVE_COND=%d (%s), learning budget B=%d queries a frame (%s)", g_mode, g_mode == VKN_COND_A ? "technique A: carried verdicts, CPU skips" :
                 "technique A VERIFY: a query on every pair, nothing skipped, A's decisions compared", g_budget, g_budget_asked >= 0 ? "DM_VK_NATIVE_COND_B" : tiler ? "tile GPU default" : "Adreno default");
    }
    vkn_occm_frame_begin(); g_st.frames++;
    g_win = 1; g_hash_on = 1; g_first_done = 0; g_scr_done = 0; g_any_pair = 0; g_stencil_used = 0; g_untracked = 0;
    g_T = g_Tpred; g_pd.on = 0; g_clr = 0; g_cdepth = g_cst = 0;
    g_learn_left = g_budget;
}
static int by_idx(const void *a, const void *b) {
    const uint32_t x = g_ob[*(const int32_t *)a].idx, y = g_ob[*(const int32_t *)b].idx;
    return x < y ? 1 : x > y ? -1 : (*(const int32_t *)a > *(const int32_t *)b) - (*(const int32_t *)a < *(const int32_t *)b);
}
// learning (mode 5): the objects of this frame that need a verdict, whose rect and screen stayed clean 2 frames; the biggest B
static void learn_select(uint32_t npairs) {
    static int32_t cand[VKN_Q_RECS]; uint32_t n = 0;
    const uint32_t mf = g_mf;
    if (g_whole + 2 > mf) return;
    for (uint32_t i = 0; i < npairs && n < VKN_Q_RECS; i++) {
        const pr *p = &g_pr[i];
        if (!p->elig || p->ob < 0 || p->queried || !p->nchain || p->nproven || p->r.kind == 0) continue;
        ob *o = &g_ob[p->ob];
        if (o->learn == mf + 1) continue;
        const uint32_t d = vkn_occm_maxdirt(p->r);
        if (o->va && d <= o->va) continue;                          // its verdict still holds (hidden or visible)
        if (o->qf && o->va < o->qf && o->qf + 8 > mf) continue;     // a query of it is still in flight
        if (d + 2 > mf) continue;                                   // hot: dirty in this or the last frame
        o->idx = (uint32_t)(p->idx > 0xffffffffull ? 0xffffffffu : p->idx);
        cand[n++] = p->ob;
    }
    const uint32_t B = g_budget > 0 ? (uint32_t)g_budget : 0;
    if (n > B) { qsort(cand, n, sizeof cand[0], by_idx); n = B; }   // the B with the most indices first
    for (uint32_t i = 0; i < n; i++) g_ob[cand[i]].learn = mf + 1;
}
void vkn_occ_q_rec_close(void) {
    if (!g_on) return;
    close_window();
    uint32_t npairs = 0; vkn_query_occ_nrec(&npairs);
    if (g_mode == VKN_COND_A) learn_select(npairs);
    vkn_occm_frame_end(g_hash_on);
    if (g_T) g_Tpred = g_T;
    if (g_scr_done) { g_scr_prev = g_scr; g_scr_prev.valid = g_scr.vph != 0; }
    g_on = 0;
}
int vkn_occ_q_pair_begin(uint32_t rec, uint64_t key) {
    if (!g_mode || rec >= VKN_Q_RECS) return 0;
    if (!g_on) return g_mode == VKN_COND_A_VERIFY;
    pr *p = &g_pr[rec]; memset(p, 0, sizeof *p); p->ob = -1;
    g_st.pairs++;
    gslot gs; memset(&gs, 0, sizeof gs);
    const int why = gather_get(key, &gs);
    if (why == RF_OK || why == RF_BEHIND || why == RF_NOCAM) {
        uint64_t okey = mix(mix(0x4f424a00ull, gs.obj), gs.vph);
        okey = mix(okey, occ_next(okey)) | 1;
        p->pkey = okey; p->ob = ob_get(okey);
        if (p->ob >= 0) g_ob[p->ob].seen = g_mf;
    }
    if (why == RF_OK) { p->gat = 1; p->vph = gs.vph; memcpy(p->ndc, gs.ndc, sizeof p->ndc); g_st.pairs_gathered++; }
    p->elig = why == RF_OK && g_win && p->ob >= 0;
    p->why = why ? (uint32_t)why : !g_win ? RF_LATE : RF_OK;
    if (!p->elig && !p->why) p->why = RF_NOVERDICT;   // (the object table had no room)
    if (g_mode == VKN_COND_A_VERIFY) return 1;
    ob *o = p->ob >= 0 ? &g_ob[p->ob] : NULL;
    if (o && p->elig && g_slot >= 0 && o->learn == g_mf && g_learn_left > 0) { g_learn_left--; p->queried = 1; o->qf = g_mf; g_st.learn++; return 1; }
    return 0;
}
void vkn_occ_q_pair_broken(uint32_t rec) { if (rec < VKN_Q_RECS && g_on) { g_pr[rec].elig = 0; g_pr[rec].why = RF_BROKEN; } }
void vkn_occ_q_slot_taken(int slot) {
    if (slot < 0 || slot >= VKN_Q_SLOTS) return;
    for (uint32_t q = 0; q < g_qn[slot]; q++) g_qi[slot][q].valid = 0;
    g_qn[slot] = 0;
}
void vkn_occ_q_copied(int slot, uint32_t q, uint32_t rec, int whole) {
    if (!g_mode || slot < 0 || slot >= VKN_Q_SLOTS || q >= VKN_Q_CAP || rec >= VKN_Q_RECS) return;
    qi *x = &g_qi[slot][q]; const pr *p = &g_pr[rec];
    memset(x, 0, sizeof *x);   // (a copy at a pass end after the recording's close listener: the frame's state is still this one)
    uint32_t seen = 0, done = 0; int closed = 0, empty = 0;
    (void)vkn_query_occ_rec(rec, &seen, &done, &closed, &empty);
    x->valid = 1; x->ob = p->ob; x->okey = p->ob >= 0 ? g_ob[p->ob].key : 0; x->mf = g_mf; x->n = p->nchain; x->r = p->r;
    x->elig = whole && p->elig && g_hash_on && p->nchain && seen == p->nchain && done == seen && closed;
    x->decided = (uint8_t)all_proven(p, rec); x->a_used = p->a_used; x->maxd = p->maxd;
    if (q + 1 > g_qn[slot]) g_qn[slot] = q + 1;
    g_st.queries_v++;
}
void vkn_occ_q_result(int slot, uint32_t q, uint32_t samples) {
    if (slot < 0 || slot >= VKN_Q_SLOTS || q >= VKN_Q_CAP) return;
    qi *x = &g_qi[slot][q];
    if (!x->valid) return;
    x->valid = 0;
    const int hidden = samples == 0;
    g_st.verdicts++; g_st.verdict_hidden += (uint64_t)hidden;
    if (x->decided) {
        g_st.verify_pairs++;
        if (hidden) g_st.verify_agree++;
        else {
            g_st.violations++;
            ORED(0, "VERIFY VIOLATION: object %#llx in frame %u: A would skip it on the verdict of frame %u (age %u, newest dirt in its rect frame %u), "
                    "rect cells x %u..%u y %u..%u (kind %u), %u draws; the query of that frame saw %u samples",
                 (unsigned long long)x->okey, x->mf, x->a_used, x->mf - x->a_used, x->maxd, x->r.x0, x->r.x1, x->r.y0, x->r.y1, x->r.kind, x->n, samples);
        }
    }
    if (x->elig && x->ob >= 0 && g_ob[x->ob].key == x->okey) {
        ob *o = &g_ob[x->ob];
        if (x->mf > o->va) { o->va = x->mf; o->vh = (uint8_t)hidden; o->vn = x->n; }
    }
}
void vkn_occ_init(int mode, int budget) {
    g_mode = mode == VKN_COND_A || mode == VKN_COND_A_VERIFY ? mode : 0;
    g_budget_asked = budget; g_budget = -1;
    if (!g_mode) return;
    static int reg;
    if (!reg) reg = !vkn_fx_register("occ-a", fx_a, NULL, 45) && !vkn_fx_register("occ-a-post", fx_post, NULL, 1001);
    if (!reg) ORED(1, "the effect chain is full: technique A does nothing");
}

// ================================================================ census
void vkn_occ_census(void) {
    if (!g_mode) return;
    static vkn_occ_stats p; static uint64_t pg[VKN_OCC_G_N];
    vkn_occ_stats a; vkn_occ_stats_get(&a);
    const double df = a.frames > p.frames ? (double)(a.frames - p.frames) : 1.0;
#define PF(f) ((double)(a.f - p.f) / df)
    vkn_occ_census_v v; vkn_occ_objects(&v);
    char o[2400]; int n = 0; const int cap = (int)sizeof o;
    n += snprintf(o + n, (size_t)(cap - n), "occ-a: DM_VK_NATIVE_COND=%d%s B=%d; objects %u (verdict hidden %u, of them valid now %u; visible %u; none %u); a frame: tracked draws %.1f "
                  "(pair draws %.1f), %s pre-pass draws %.1f, conditional draws %.1f, indices %.0f; learning queries %.1f; verdicts read %.1f (hidden %.1f); dirty cells %.1f; "
                  "frames %llu, hashed %llu; whole-screen dirty frames:", g_mode, g_mode == VKN_COND_A_VERIFY ? " (VERIFY)" : "", g_budget, v.objects, v.hidden, v.valid_hidden, v.visible, v.none,
                  PF(tracked), PF(pair_draws), g_mode == VKN_COND_A ? "CPU-skipped" : "would skip", g_mode == VKN_COND_A ? PF(skip_pre) : PF(would_pre),
                  g_mode == VKN_COND_A ? PF(skip_cond) : PF(would_cond), PF(skip_idx), PF(learn), PF(verdicts), PF(verdict_hidden), PF(dirty_cells),
                  (unsigned long long)(a.frames - p.frames), (unsigned long long)(a.hashed_frames - p.hashed_frames));
    for (int i = 0; i < VKN_OCC_W_N && n < cap - 64; i++) if (a.whole[i] - p.whole[i]) n += snprintf(o + n, (size_t)(cap - n), " %s %llu,", k_wwhy[i], (unsigned long long)(a.whole[i] - p.whole[i]));
    n += snprintf(o + n, (size_t)(cap - n), " | refused (draws of objects with a hidden verdict) a frame:");
    for (int i = 1; i < RF_NOVERDICT && n < cap - 64; i++) if (a.refuse[i] - p.refuse[i]) n += snprintf(o + n, (size_t)(cap - n), " %s %.1f,", k_why[i], PF(refuse[i]));
    n += snprintf(o + n, (size_t)(cap - n), " | inputs without a version %.1f; constant bytes hashed %.1f KB (memo %.1f); pairs %.1f, gathered %.1f; gather handoffs:", PF(unversioned),
                  PF(ubo_bytes) / 1024.0, PF(ubo_memo), PF(pairs), PF(pairs_gathered));
    static const char *const gn[VKN_OCC_G_N] = {"ok", "bad key", "stale", "conflict", "no camera", "bad box", "behind camera"};
    for (int i = 0; i < VKN_OCC_G_N && n < cap - 48; i++) if (a.gather[i] - pg[i]) n += snprintf(o + n, (size_t)(cap - n), " %s %.1f,", gn[i], (double)(a.gather[i] - pg[i]) / df);
    if (g_mode == VKN_COND_A_VERIFY)
        n += snprintf(o + n, (size_t)(cap - n), " | VERIFY: pairs A would skip, judged %llu, agree %llu, VIOLATIONS %llu (total %llu)", (unsigned long long)(a.verify_pairs - p.verify_pairs),
                      (unsigned long long)(a.verify_agree - p.verify_agree), (unsigned long long)(a.violations - p.violations), (unsigned long long)a.violations);
    n += snprintf(o + n, (size_t)(cap - n), " | recording-thread CPU %.3f ms a frame", (double)(a.cpu_ns - p.cpu_ns) / df / 1e6);
    (void)n;
    vkn_logf("%s", o);
    p = a; memcpy(pg, a.gather, sizeof pg);
#undef PF
}
#endif
