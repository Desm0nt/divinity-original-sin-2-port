// [DOS2] VK-NATIVE stage 5 technique Z: the pure model (rule and proof: vkn_occz.h, docs/dos2/nr/VK-OCCLUSION-CHEAP.md sections 3 and 10).
// Plain C, no Vulkan, no environment, no game addresses: the engine thread (vkn_occz.c) and the host test (vkn_occz_test.c) drive it.
// Single-threaded (the caller's thread).
// v1 (the snapshot's own camera): a box behind every cell of its rect, no hazard nearer there -> hidden.
// v2 (a moved camera, 22.11): the box is reprojected into the SNAPSHOT's camera; the rays of the new eye through the box are bounded in
// the snapshot's screen by the swept region (the box's corners and their central projections from the new eye onto the nearest occluder
// depth of the region); the box must lie behind every cell of its own rect there, and every cell of the swept region must be a
// continuous surface ("sheet": min / max depth of the cell close, neighbours touching) with no hazard nearer than the box. Then every
// ray of the new eye through the box crosses that surface between the eye and the box (proof: the doc, section 10).
#ifdef DM_GAME_DOS2
#include "vkn_occz.h"
#include <float.h>
#include <math.h>
#include <string.h>

enum { TAB = 8192, GLOB = 32, DEC_FRAMES = 16, ITER = 4 };
typedef struct oz_h { uint64_t frame; float ndc[4]; float zmin; uint8_t rect; } oz_h;
typedef struct oz_e {
    uint64_t obj, box_h, sig, sig_full, seen, changed, born;
    uint8_t used;          // 0 free, 1 live, 2 tombstone
    uint8_t hn;            // history entries (ring by frame & (VKN_OZ_HIST - 1))
    oz_h h[VKN_OZ_HIST];
} oz_e;
typedef struct oz_dec { uint64_t key; float zmin; uint16_t c[4]; } oz_dec;
typedef struct oz_decf { uint64_t frame; uint32_t n, gw, gh, w, h; oz_dec d[VKN_OZ_MAXN]; } oz_decf;

static oz_e g_t[TAB];
static struct { uint64_t frame, global; int order; } g_glob[GLOB];
static oz_decf g_dec[DEC_FRAMES];
static float g_hz[VKN_OZ_GH][VKN_OZ_GW];
static vkn_oz_cfg g_cfg = {1e-5f, 2, UINT64_MAX, 0.05f};
static vkn_oz_stats g_st;

void vkn_ozm_cfg(const vkn_oz_cfg *c) { if (c) g_cfg = *c; }
void vkn_ozm_stats_get(vkn_oz_stats *s) { if (s) *s = g_st; }
void vkn_ozm_reset(void) {
    memset(g_t, 0, sizeof g_t); memset(g_glob, 0, sizeof g_glob); memset(&g_st, 0, sizeof g_st);
    for (int i = 0; i < DEC_FRAMES; i++) { g_dec[i].frame = 0; g_dec[i].n = 0; }
}

// ---- pure helpers ----
int vkn_oz_project(const float b[6], const float vp[16], float ndc[4], float *zmin) {
    if (!(b[0] <= b[3] && b[1] <= b[4] && b[2] <= b[5])) return -2;   // (NaN fails too)
    float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX, z0 = FLT_MAX;
    for (int k = 0; k < 8; k++) {
        const float p[3] = {k & 1 ? b[3] : b[0], k & 2 ? b[4] : b[1], k & 4 ? b[5] : b[2]};
        float c[4];
        for (int j = 0; j < 4; j++) c[j] = p[0] * vp[j] + p[1] * vp[4 + j] + p[2] * vp[8 + j] + vp[12 + j];
        if (!(c[3] > 1e-6f)) return -1;
        const float x = c[0] / c[3], y = c[1] / c[3], z = c[2] / c[3];
        x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1; y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1; z0 = z < z0 ? z : z0;
    }
    ndc[0] = x0; ndc[1] = y0; ndc[2] = x1; ndc[3] = y1; *zmin = z0;
    return z0 == z0 && x0 == x0 && y0 == y0 ? 0 : -2;
}
// NDC -> pixels of the target (Vulkan conventions: -1 = left / top), + pad px, -> inclusive cells. 1 = entirely off the target.
int vkn_oz_cells(const float ndc[4], uint32_t w, uint32_t h, uint32_t gw, uint32_t gh, uint32_t pad, uint32_t c[4]) {
    const float px0 = (ndc[0] * 0.5f + 0.5f) * (float)w - (float)pad, px1 = (ndc[2] * 0.5f + 0.5f) * (float)w + (float)pad;
    const float py0 = (ndc[1] * 0.5f + 0.5f) * (float)h - (float)pad, py1 = (ndc[3] * 0.5f + 0.5f) * (float)h + (float)pad;
    if (!(px1 >= 0.0f && py1 >= 0.0f && px0 < (float)w && py0 < (float)h)) return 1;
    const float cs = (float)VKN_OZ_CELL;
    int a = (int)(px0 / cs), b = (int)(py0 / cs), e = (int)(px1 / cs), f = (int)(py1 / cs);
    a = px0 < 0.0f ? 0 : a; b = py0 < 0.0f ? 0 : b;
    e = e >= (int)gw ? (int)gw - 1 : e; f = f >= (int)gh ? (int)gh - 1 : f;
    c[0] = (uint32_t)a; c[1] = (uint32_t)b; c[2] = (uint32_t)e; c[3] = (uint32_t)f;
    return 0;
}
// [v2] The cells of the snapshot whose pixels an NDC rect touches (no pad: the card carries the frame's pad already; the far edges
// half-open, a rect ending on a cell border does not take the next cell). 0 = the rect lies entirely on the target (the snapshot
// knows the depth of all of it), 1 = it does not.
static int cells_in(const float ndc[4], uint32_t w, uint32_t h, uint32_t c[4]) {
    const double px0 = ((double)ndc[0] * 0.5 + 0.5) * w, px1 = ((double)ndc[2] * 0.5 + 0.5) * w - 1e-3;
    const double py0 = ((double)ndc[1] * 0.5 + 0.5) * h, py1 = ((double)ndc[3] * 0.5 + 0.5) * h - 1e-3;
    if (!(px0 >= 0.0 && py0 >= 0.0 && px1 < (double)w && py1 < (double)h)) return 1;
    c[0] = (uint32_t)(px0 / VKN_OZ_CELL); c[1] = (uint32_t)(py0 / VKN_OZ_CELL);
    c[2] = (uint32_t)((px1 > px0 ? px1 : px0) / VKN_OZ_CELL); c[3] = (uint32_t)((py1 > py0 ? py1 : py0) / VKN_OZ_CELL);
    return 0;
}
// column j of a row-vector matrix: (vp[j], vp[4 + j], vp[8 + j]; vp[12 + j])
static void col(const float vp[16], int j, double c[4]) { c[0] = vp[j]; c[1] = vp[4 + j]; c[2] = vp[8 + j]; c[3] = vp[12 + j]; }
int vkn_oz_eye(const float vp[16], double e[3]) {
    double a[4], b[4], c[4]; col(vp, 0, a); col(vp, 1, b); col(vp, 3, c);   // a.p + a3 = 0, b.p + b3 = 0, c.p + c3 = 0 (clip x = y = w = 0)
    const double det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    const double sc = (fabs(a[0]) + fabs(a[1]) + fabs(a[2])) * (fabs(b[0]) + fabs(b[1]) + fabs(b[2])) * (fabs(c[0]) + fabs(c[1]) + fabs(c[2]));
    if (!(sc > 0.0) || !(fabs(det) > 1e-9 * sc)) return -1;
    const double r[3] = {-a[3], -b[3], -c[3]};
    e[0] = (r[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (r[1] * c[2] - b[2] * r[2]) + a[2] * (r[1] * c[1] - b[1] * r[2])) / det;
    e[1] = (a[0] * (r[1] * c[2] - b[2] * r[2]) - r[0] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * r[2] - r[1] * c[0])) / det;
    e[2] = (a[0] * (b[1] * r[2] - r[1] * c[1]) - a[1] * (b[0] * r[2] - r[1] * c[0]) + r[0] * (b[0] * c[1] - b[1] * c[0])) / det;
    return e[0] == e[0] && e[1] == e[1] && e[2] == e[2] ? 0 : -1;
}
int vkn_oz_lin(const float vp[16], double *a, double *b) {
    double z[4], w[4]; col(vp, 2, z); col(vp, 3, w);
    const double ww = w[0] * w[0] + w[1] * w[1] + w[2] * w[2], zz = z[0] * z[0] + z[1] * z[1] + z[2] * z[2];
    if (!(ww > 0.0)) return -1;   // orthographic: no eye, no parallax model here
    const double k = (z[0] * w[0] + z[1] * w[1] + z[2] * w[2]) / ww;
    const double r = (z[0] - k * w[0]) * (z[0] - k * w[0]) + (z[1] - k * w[1]) * (z[1] - k * w[1]) + (z[2] - k * w[2]) * (z[2] - k * w[2]);
    if (!(r <= 1e-8 * (zz > ww ? zz : ww))) return -1;   // clip z not a function of the view depth alone (an oblique projection)
    *a = k; *b = z[3] - k * w[3];
    return *b != 0.0 ? 0 : -1;
}

// ---- the object table (key: engine object pointer) ----
static uint32_t hslot(uint64_t k) { return (uint32_t)((k * 0x9e3779b97f4a7c15ull) >> 51) & (TAB - 1); }
static oz_e *find(uint64_t obj, int add) {
    oz_e *tomb = NULL;
    for (uint32_t i = hslot(obj), n = 0; n < TAB; i = (i + 1) & (TAB - 1), n++) {
        oz_e *e = &g_t[i];
        if (e->used == 1 && e->obj == obj) return e;
        if (e->used == 2 && !tomb) tomb = e;
        if (e->used == 0) {
            if (!add) return NULL;
            e = tomb ? tomb : e;
            memset(e, 0, sizeof *e); e->used = 1; e->obj = obj;
            return e;
        }
    }
    if (add && tomb) { memset(tomb, 0, sizeof *tomb); tomb->used = 1; tomb->obj = obj; return tomb; }
    return NULL;   // full: the caller treats the object as without identity
}
static void hist_put(oz_e *e, uint64_t f, const vkn_oz_obj *o) {
    oz_h *h = &e->h[f & (VKN_OZ_HIST - 1)];
    h->frame = f; memcpy(h->ndc, o->ndc, sizeof h->ndc); h->zmin = o->zmin; h->rect = o->rect;
    if (e->hn < VKN_OZ_HIST) e->hn++;
}
static void glob_put(uint64_t f, uint64_t g, int order) { const uint32_t i = (uint32_t)(f % GLOB); g_glob[i].frame = f; g_glob[i].global = g; g_glob[i].order = order; }
// 1 = the globals of every frame in [s, f] are known, equal and order-insensitive
static int glob_same(uint64_t s, uint64_t f) {
    const uint32_t is = (uint32_t)(s % GLOB);
    if (g_glob[is].frame != s || g_glob[is].order) return 0;
    for (uint64_t x = s + 1; x <= f; x++) {
        const uint32_t i = (uint32_t)(x % GLOB);
        if (g_glob[i].frame != x || g_glob[i].order || g_glob[i].global != g_glob[is].global) return 0;
    }
    return 1;
}
static void hz_fill(const uint32_t c[4], float z) {
    for (uint32_t y = c[1]; y <= c[3]; y++) for (uint32_t x = c[0]; x <= c[2]; x++) if (z < g_hz[y][x]) g_hz[y][x] = z;
}
// The footprint of hazard e in the snapshot (frame s): its rect AT s, in s's own camera - only there can its depth of the snapshot be gone
// now (where it is later it can only add depth). 1 = it covered the whole screen (an unknown rect). Not in frame s: no contribution.
static int hazard(const oz_e *e, uint64_t s, const vkn_oz_snap *sn) {
    const oz_h *h = &e->h[s & (VKN_OZ_HIST - 1)];
    if (h->frame != s) return 0;   // absent at s (the ring keeps every frame of [s, F]: F - s < VKN_OZ_HIST)
    g_st.hazards++;
    uint32_t c[4];
    if (!h->rect) { const uint32_t a[4] = {0, 0, sn->gw - 1, sn->gh - 1}; hz_fill(a, -FLT_MAX); return 1; }
    if (!vkn_oz_cells(h->ndc, sn->w, sn->h, sn->gw, sn->gh, g_cfg.pad_px, c)) hz_fill(c, h->zmin);
    return 0;
}

// ---- [v2] reprojection ----
typedef struct mv { double ce[4]; double la, lb; double inv[16]; } mv;   // the new eye in the snapshot's clip space; the snapshot's linear depth; the frame's inverse view-projection
static double lin_of(const mv *m, float z) { const double d = (double)z - m->la; return d != 0.0 ? m->lb / d : -1.0; }
// 1 = every cell of c is part of one continuous surface: the cell's own linear depth range and the step to its right / lower neighbour
// within `sheet` of the nearer depth, nothing at the far plane (1.0: no occluder drawn)
static int sheet_ok(const vkn_oz_snap *sn, const mv *m, const uint32_t c[4]) {
    static double lo_prev[VKN_OZ_GW], hi_prev[VKN_OZ_GW];
    const double t = (double)g_cfg.sheet;
    for (uint32_t y = c[1]; y <= c[3]; y++) {
        const float *mx = sn->cell + (size_t)y * sn->gw, *mn = sn->cmin + (size_t)y * sn->gw;
        double lo_l = 0.0, hi_l = 0.0;
        for (uint32_t x = c[0]; x <= c[2]; x++) {
            if (!(mx[x] < 1.0f) || !(mn[x] <= mx[x])) return 0;
            double l0 = lin_of(m, mn[x]), l1 = lin_of(m, mx[x]);
            if (l0 > l1) { const double q = l0; l0 = l1; l1 = q; }
            if (!(l0 > 0.0) || !(l1 < 1e30) || l1 - l0 > t * l0) return 0;
            if (x > c[0] && (l0 > hi_l * (1.0 + t) || lo_l > l1 * (1.0 + t))) return 0;   // a jump to the left neighbour
            if (y > c[1] && (l0 > hi_prev[x] * (1.0 + t) || lo_prev[x] > l1 * (1.0 + t))) return 0;   // to the upper one
            lo_l = l0; hi_l = l1; lo_prev[x] = l0; hi_prev[x] = l1;
        }
    }
    return 1;
}
static float cmin_over(const vkn_oz_snap *sn, const uint32_t c[4]) {
    float z = FLT_MAX;
    for (uint32_t y = c[1]; y <= c[3]; y++) { const float *r = sn->cmin + (size_t)y * sn->gw; for (uint32_t x = c[0]; x <= c[2]; x++) z = r[x] < z ? r[x] : z; }
    return z;
}
// The CARD of a pair: the frame's own cells under its rect (whole cells + pad, the self-check's cells) at the box's nearest depth z, a
// planar quad in the frame's camera. Proving it hidden proves both the box (every ray through the box crosses the card first) and the
// self-check of frame F (every pixel of those cells nearer than z). Here: its 4 corners unprojected (the frame's inverse
// view-projection) and projected into the snapshot's camera: their clip coordinates, the rect r0 / cells c0 there and the card's
// nearest depth zb there. 0 or the reason.
typedef struct rbox { double cq[4][4]; float r0[4]; } rbox;
static int reproj_card(const uint32_t c[4], float z, const double inv[16], const vkn_oz_snap *sn, rbox *q, uint32_t c0[4], float *zb) {
    const double cs = (double)VKN_OZ_CELL;
    const double px0 = c[0] * cs, py0 = c[1] * cs, px1 = (c[2] + 1) * cs < sn->w ? (c[2] + 1) * cs : sn->w, py1 = (c[3] + 1) * cs < sn->h ? (c[3] + 1) * cs : sn->h;
    float *r0 = q->r0; double z0 = DBL_MAX;
    r0[0] = r0[1] = FLT_MAX; r0[2] = r0[3] = -FLT_MAX;
    for (int k = 0; k < 4; k++) {
        const double nx = (k & 1 ? px1 : px0) / sn->w * 2.0 - 1.0, ny = (k & 2 ? py1 : py0) / sn->h * 2.0 - 1.0;
        double w[4];
        for (int j = 0; j < 4; j++) w[j] = nx * inv[j] + ny * inv[4 + j] + (double)z * inv[8 + j] + inv[12 + j];
        if (!(w[3] != 0.0)) return VKN_OZ_R_REPROJ;
        const double p[3] = {w[0] / w[3], w[1] / w[3], w[2] / w[3]};
        double *cl = q->cq[k];
        for (int j = 0; j < 4; j++) cl[j] = p[0] * sn->vp[j] + p[1] * sn->vp[4 + j] + p[2] * sn->vp[8 + j] + sn->vp[12 + j];
        if (!(cl[3] > 1e-6)) return VKN_OZ_R_REPROJ;   // behind the snapshot's camera
        const float x = (float)(cl[0] / cl[3]), y = (float)(cl[1] / cl[3]); const double zz = cl[2] / cl[3];
        r0[0] = x < r0[0] ? x : r0[0]; r0[2] = x > r0[2] ? x : r0[2]; r0[1] = y < r0[1] ? y : r0[1]; r0[3] = y > r0[3] ? y : r0[3]; z0 = zz < z0 ? zz : z0;
    }
    if (!(z0 == z0) || cells_in(r0, sn->w, sn->h, c0)) return VKN_OZ_R_REPROJ;   // (the snapshot must know all of it)
    *zb = (float)z0;
    return 0;
}
// 4 x 4 inverse (row vectors as stored); 0 ok
static int inv4(const float m[16], double o[16]) {
    double a[16]; for (int i = 0; i < 16; i++) a[i] = m[i];
    double r[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int c = 0; c < 4; c++) {
        int pv = c; for (int i = c + 1; i < 4; i++) if (fabs(a[4 * i + c]) > fabs(a[4 * pv + c])) pv = i;
        if (!(fabs(a[4 * pv + c]) > 1e-12)) return -1;
        if (pv != c) for (int j = 0; j < 4; j++) { double t = a[4 * c + j]; a[4 * c + j] = a[4 * pv + j]; a[4 * pv + j] = t; t = r[4 * c + j]; r[4 * c + j] = r[4 * pv + j]; r[4 * pv + j] = t; }
        const double d = a[4 * c + c];
        for (int j = 0; j < 4; j++) { a[4 * c + j] /= d; r[4 * c + j] /= d; }
        for (int i = 0; i < 4; i++) if (i != c) { const double f = a[4 * i + c]; if (f != 0.0) for (int j = 0; j < 4; j++) { a[4 * i + j] -= f * a[4 * c + j]; r[4 * i + j] -= f * r[4 * c + j]; } }
    }
    memcpy(o, r, sizeof r);
    return 0;
}
// The swept region R (cells, in the snapshot) of the rays from the new eye through the card: the card's corners and the points where
// their rays to the new eye reach zo, the nearest depth of R itself (iterated until zo holds for the region it built). 0 or the reason.
static int reproj_region(const rbox *q, const vkn_oz_snap *sn, const mv *m, const uint32_t c0[4], uint32_t cr[4]) {
    float zo = cmin_over(sn, c0);
    for (int it = 0; it < ITER; it++) {
        float r[4]; memcpy(r, q->r0, sizeof r);
        for (int k = 0; k < 4; k++) {   // the corner's ray to the new eye meets the snapshot depth zo (clip z = zo * clip w) at t
            const double *c = q->cq[k];
            const double f0 = c[2] - (double)zo * c[3], f1 = m->ce[2] - (double)zo * m->ce[3];
            if (!(f0 > 0.0) || !(f1 < 0.0)) return VKN_OZ_R_REPROJ;   // the corner is not behind zo, or the new eye not in front of it
            const double t = f0 / (f0 - f1);
            double s[4]; for (int j = 0; j < 4; j++) s[j] = c[j] + t * (m->ce[j] - c[j]);
            if (!(s[3] > 1e-6)) return VKN_OZ_R_REPROJ;
            const float x = (float)(s[0] / s[3]), y = (float)(s[1] / s[3]);
            r[0] = x < r[0] ? x : r[0]; r[2] = x > r[2] ? x : r[2]; r[1] = y < r[1] ? y : r[1]; r[3] = y > r[3] ? y : r[3];
        }
        if (!(r[0] == r[0] && r[1] == r[1] && r[2] == r[2] && r[3] == r[3]) || cells_in(r, sn->w, sn->h, cr)) return VKN_OZ_R_REPROJ;
        const float z2 = cmin_over(sn, cr);
        if (z2 >= zo) return 0;   // settled: zo is the nearest depth of the whole region it was built with
        zo = z2;
    }
    return VKN_OZ_R_REPROJ;
}

uint32_t vkn_ozm_frame(const vkn_oz_frame *fr, const vkn_oz_snap *sn, uint8_t *verdict, uint64_t (*now)(void)) {
    const uint64_t t0 = now ? now() : 0, F = fr->frame;
    g_st.frames++;
    glob_put(F, fr->global, fr->order_sensitive);
    g_st.order_frames += (uint64_t)(fr->order_sensitive != 0);
    // 1. the object table: presence, changes, history
    for (uint32_t i = 0; i < fr->n; i++) {
        const vkn_oz_obj *o = &fr->o[i];
        verdict[i] = VKN_OZ_DRAW;
        if (!o->obj) continue;
        oz_e *e = find(o->obj, 1);
        if (!e) continue;
        const int was = e->seen == F || e->seen + 1 == F;
        if (!e->seen) { e->born = F; e->changed = F; }
        else if (!was) e->changed = F;   // came back after an absence
        else if (e->box_h != o->box_h || e->sig != o->sig) {
            e->changed = F;
            if (e->seen != F) { g_st.changed_box += (uint64_t)(e->box_h != o->box_h); g_st.changed_sig += (uint64_t)(e->box_h == o->box_h); }
        } else if (e->seen != F && e->sig_full != o->sig_full) g_st.changed_cam_only++;
        if (e->changed == F && e->born != F && e->seen != F) g_st.changed++;
        e->box_h = o->box_h; e->sig = o->sig; e->sig_full = o->sig_full; e->seen = F;
        hist_put(e, F, o);
    }
    // 2. the snapshot
    int why = 0, moved = 0; mv m; memset(&m, 0, sizeof m);
    if (!sn || !sn->cell) why = VKN_OZ_R_NOSNAP;
    else if (sn->tex != fr->tex || sn->w != fr->w || sn->h != fr->h || !sn->gw || !sn->gh || sn->gw > VKN_OZ_GW || sn->gh > VKN_OZ_GH) why = VKN_OZ_R_TARGET;
    else if (sn->frame >= F || F - sn->frame >= VKN_OZ_HIST) why = VKN_OZ_R_OLD;
    else if (!fr->cam || sn->cam != fr->cam) {   // [v2] a moved camera: the reprojection, when it is possible
        double eye[3];
        if (!(g_cfg.sheet > 0.0f) || !fr->vp || !sn->vp || !sn->cmin || vkn_oz_eye(fr->vp, eye) || vkn_oz_lin(sn->vp, &m.la, &m.lb) || inv4(fr->vp, m.inv)) why = VKN_OZ_R_CAMERA;
        else {
            moved = 1;
            for (int j = 0; j < 4; j++) m.ce[j] = eye[0] * sn->vp[j] + eye[1] * sn->vp[4 + j] + eye[2] * sn->vp[8 + j] + sn->vp[12 + j];
        }
    }
    if (!why && !glob_same(sn->frame, F)) why = VKN_OZ_R_SCENE;
    // 3. hazards (and the table's housekeeping)
    if (!why) {
        for (uint32_t y = 0; y < sn->gh; y++) for (uint32_t x = 0; x < sn->gw; x++) g_hz[y][x] = FLT_MAX;
        g_st.snaps_used++; g_st.snap_age_sum += F - sn->frame; g_st.moved_frames += (uint64_t)moved;
    }
    for (uint32_t i = 0; i < TAB; i++) {
        oz_e *e = &g_t[i];
        if (e->used != 1) continue;
        if (e->seen + 2 * VKN_OZ_HIST < F) { e->used = 2; continue; }
        if (why || e->seen < sn->frame || e->born > sn->frame) continue;   // not in the snapshot: adds depth at most
        if (e->changed > sn->frame || e->seen < F) g_st.hazard_whole += (uint64_t)hazard(e, sn->frame, sn);
    }
    // 4. the pairs
    uint32_t hidden = 0, done = 0; oz_decf *dl = &g_dec[F % DEC_FRAMES];
    dl->frame = F; dl->n = 0; if (sn) { dl->gw = sn->gw; dl->gh = sn->gh; dl->w = sn->w; dl->h = sn->h; }
    for (uint32_t i = 0; i < fr->n; i++) {
        const vkn_oz_obj *o = &fr->o[i];
        g_st.pairs++;
        int r = why;
        uint32_t c[4] = {0, 0, 0, 0}, cs[4], cr[4];   // c: the frame's own cells (the decision log); cs / cr: the snapshot's box rect / region
        float lim = 0.0f;
        if (!r && !o->obj) r = VKN_OZ_R_NOOBJ;
        if (!r && !o->rect) r = VKN_OZ_R_NORECT;
        if (!r && !o->clean) r = VKN_OZ_R_UNCLEAN;
        if (!r && vkn_oz_cells(o->ndc, sn->w, sn->h, sn->gw, sn->gh, g_cfg.pad_px, c)) r = VKN_OZ_R_OFFSCREEN;
        if (!r && now && g_cfg.budget_ns != UINT64_MAX && (++done & 31) == 0 && now() - t0 > g_cfg.budget_ns) r = VKN_OZ_R_BUDGET;
        rbox q;
        if (!r && !moved) { memcpy(cs, c, sizeof cs); lim = o->zmin - g_cfg.eps; }
        if (!r && moved) { float zb = 0.0f; r = reproj_card(c, o->zmin, m.inv, sn, &q, cs, &zb); lim = zb - g_cfg.eps; }
        if (!r) {
            g_st.cands++; g_st.moved_cands += (uint64_t)moved;
            int depth_ok = 1, hz_ok = 1;
            for (uint32_t y = cs[1]; y <= cs[3] && depth_ok; y++) {   // the box behind every cell of its rect (in the snapshot's camera)
                const float *row = sn->cell + (size_t)y * sn->gw;
                for (uint32_t x = cs[0]; x <= cs[2]; x++) if (!(row[x] < lim)) { depth_ok = 0; break; }
            }
            // the cells where the rays of this frame's eye through the box may cross the snapshot's surface: the box's own (v1) or the
            // swept region (v2); no hazard nearer than the box there, and (v2) one continuous surface
            memcpy(cr, cs, sizeof cr);
            if (depth_ok && moved) { r = reproj_region(&q, sn, &m, cs, cr); if (!r) g_st.moved_region_cells += (uint64_t)(cr[2] - cr[0] + 1) * (cr[3] - cr[1] + 1); }
            for (uint32_t y = cr[1]; y <= cr[3] && hz_ok; y++)
                for (uint32_t x = cr[0]; x <= cr[2]; x++) if (g_hz[y][x] < lim) { hz_ok = 0; break; }
            if (!r) r = !hz_ok ? VKN_OZ_R_HAZARD : !depth_ok ? VKN_OZ_R_DEPTH : 0;
            if (!r && moved && !sheet_ok(sn, &m, cr)) r = VKN_OZ_R_SHEET;
            if (hz_ok && !depth_ok) { verdict[i] = VKN_OZ_POISON; g_st.poison++; }   // trusted and visible (mode 13 skips these)
        }
        if (r == VKN_OZ_R_BUDGET) {   // (the rest of the frame is drawn)
            for (uint32_t j = i; j < fr->n; j++) { g_st.why[VKN_OZ_R_BUDGET]++; if (j > i) g_st.pairs++; }
            break;
        }
        g_st.why[r]++;
        if (r) continue;
        verdict[i] = VKN_OZ_HIDDEN; hidden++; g_st.hidden++; g_st.moved_hidden += (uint64_t)moved;
        g_st.would_pre += o->ndraw; g_st.would_cond += o->ncond;
        if (dl->n < VKN_OZ_MAXN) { oz_dec *d = &dl->d[dl->n++]; d->key = o->key; d->zmin = o->zmin; for (int k = 0; k < 4; k++) d->c[k] = (uint16_t)c[k]; }
    }
    if (now) g_st.model_ns += now() - t0;
    return hidden;
}

// The decisions of frame s->frame against that frame's own depth (the frame's own camera, the frame's own box rect: what really happened).
uint32_t vkn_ozm_check(const vkn_oz_snap *s, vkn_oz_red_fn red) {
    if (!s || !s->cell) return 0;
    oz_decf *dl = &g_dec[s->frame % DEC_FRAMES];
    if (dl->frame != s->frame) return 0;
    g_st.check_frames++;
    if (dl->gw != s->gw || dl->gh != s->gh || dl->w != s->w || dl->h != s->h) { g_st.check_lost += dl->n; dl->frame = 0; return 0; }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < dl->n; i++) {
        const oz_dec *d = &dl->d[i];
        g_st.checked++;
        int ok = 1;
        for (uint32_t y = d->c[1]; y <= d->c[3] && ok; y++) for (uint32_t x = d->c[0]; x <= d->c[2]; x++) {
            const float v = s->cell[(size_t)y * s->gw + x];
            if (!(v < d->zmin)) { ok = 0; if (red) red(d->key, d->zmin, v, x, y, s->frame); break; }
        }
        if (!ok) { bad++; g_st.violations++; }
    }
    dl->frame = 0;   // (checked once)
    return bad;
}
#endif
