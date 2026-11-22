// [DOS2] VK-ENGINE-SIDE I2: the pure test of one object against an earlier frame's depth (rule: vkn_eocc.h). Plain C, no state, no
// Vulkan, no environment, no game addresses: any thread (the engine's cull jobs call it in parallel) and the host test vkn_eocc_test.c.
// The moved-camera part is technique Z v2's reprojection (vkn_occz_model.c: the card, the swept region, the sheet; proof in
// docs/dos2/nr/VK-OCCLUSION-CHEAP.md section 10.3), here without Z's per-frame statics; the projection helpers are Z's (vkn_occz.h).
#ifdef DM_GAME_DOS2
#include "vkn_eocc.h"
#include "vkn_occz.h"
#include <float.h>
#include <math.h>
#include <string.h>

enum { ITER = 4 };

const char *vkn_eo_reason(int r) {
    static const char *const n[VKN_EO_R_N] = {"hidden", "frame not prepared", "no snapshot", "snapshot too old", "camera cut", "no reprojection",
        "other stage", "flags", "new object", "changed", "visible lately", "box at the camera", "off screen", "beyond the snapshot's edge",
        "depth (visible)", "hazard", "not a sheet", "reprojection", "budget", "table full"};
    return r >= 0 && r < VKN_EO_R_N ? n[r] : "?";
}

int vkn_eo_snap_prep(vkn_eo_snap *s) {
    if (!s || !s->mx || !s->mn || !s->gw || !s->gh || s->gw > VKN_EO_GW || s->gh > VKN_EO_GH) return -1;
    s->persp = !vkn_oz_lin(s->vp, &s->la, &s->lb);
    return 0;
}
void vkn_eo_coarse(const float *mx, uint32_t gw, uint32_t gh, float *cx, uint32_t *cgw, uint32_t *cgh) {
    const uint32_t w = (gw + VKN_EO_CQ - 1) / VKN_EO_CQ, h = (gh + VKN_EO_CQ - 1) / VKN_EO_CQ;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        float m = -FLT_MAX;
        for (uint32_t j = y * VKN_EO_CQ; j < gh && j < (y + 1) * VKN_EO_CQ; j++)
            for (uint32_t i = x * VKN_EO_CQ; i < gw && i < (x + 1) * VKN_EO_CQ; i++) { const float v = mx[(size_t)j * gw + i]; m = v > m || v != v ? v : m; }
        cx[(size_t)y * w + x] = m;
    }
    *cgw = w; *cgh = h;
}
// The depth an occluder must be nearer than to hide something whose nearest depth is z (the snapshot's NDC): `margin` of its linear depth
// nearer (perspective), else a plain epsilon.
static float lim_of(const vkn_eo_snap *s, float z, float margin) {
    if (s->persp) {
        const double d = (double)z - s->la, l = d != 0.0 ? s->lb / d : -1.0;
        if (!(l > 0.0) || !(l < 1e30)) return -FLT_MAX;
        const double l2 = l * (1.0 - (double)margin);
        return l2 > 0.0 ? (float)(s->la + s->lb / l2) : -FLT_MAX;
    }
    return z - 1e-4f;
}
// 1 = every cell of c has max depth < lim (the coarse level first: a superset of the cells, so its pass is a pass)
static int depth_ok(const vkn_eo_snap *s, const uint32_t c[4], float lim, uint32_t *cells) {
    if (s->cx) {
        int ok = 1;
        for (uint32_t y = c[1] / VKN_EO_CQ; y <= c[3] / VKN_EO_CQ && ok; y++) {
            const float *row = s->cx + (size_t)y * s->cgw;
            for (uint32_t x = c[0] / VKN_EO_CQ; x <= c[2] / VKN_EO_CQ; x++) { (*cells)++; if (!(row[x] < lim)) { ok = 0; break; } }
        }
        if (ok) return 1;
    }
    for (uint32_t y = c[1]; y <= c[3]; y++) {
        const float *row = s->mx + (size_t)y * s->gw;
        for (uint32_t x = c[0]; x <= c[2]; x++) { (*cells)++; if (!(row[x] < lim)) return 0; }
    }
    return 1;
}
// [v2] NDC -> pixel rows. yup = 1: NDC y = +1 is row 0 - the DOS2 backend's convention (vkn_draw.c: the packs' SPIR-V keeps the game's
// clip space and the Vulkan viewport is flipped, negative height); 0: NDC y = -1 is row 0 (plain Vulkan). v1 assumed 0 (technique Z's
// vkn_oz_cells too): every box was tested against the cells of its MIRROR image row, and the self-check, using the same rows, agreed.
static inline double row_of(double ny, uint32_t h, int yup) { return yup ? (0.5 - 0.5 * ny) * h : (0.5 * ny + 0.5) * h; }
static inline double ndc_of_row(double py, uint32_t h, int yup) { return yup ? 1.0 - py / h * 2.0 : py / h * 2.0 - 1.0; }
// NDC rect -> inclusive cells of the area, + pad px (clamped). 1 = entirely off the area.
static int cells_pad(const float ndc[4], const vkn_eo_snap *s, uint32_t pad, uint32_t c[4]) {
    const double px0 = ((double)ndc[0] * 0.5 + 0.5) * s->w - pad, px1 = ((double)ndc[2] * 0.5 + 0.5) * s->w + pad;
    const double ra = row_of(ndc[1], s->h, s->yup), rb = row_of(ndc[3], s->h, s->yup);
    const double py0 = (ra < rb ? ra : rb) - pad, py1 = (ra < rb ? rb : ra) + pad;
    if (!(px1 >= 0.0 && py1 >= 0.0 && px0 < (double)s->w && py0 < (double)s->h)) return 1;
    const double cs = (double)VKN_EO_CELL;
    int a = (int)(px0 / cs), b = (int)(py0 / cs), e = (int)(px1 / cs), f = (int)(py1 / cs);
    a = px0 < 0.0 ? 0 : a; b = py0 < 0.0 ? 0 : b;
    e = e >= (int)s->gw ? (int)s->gw - 1 : e; f = f >= (int)s->gh ? (int)s->gh - 1 : f;
    c[0] = (uint32_t)a; c[1] = (uint32_t)b; c[2] = (uint32_t)e; c[3] = (uint32_t)f;
    return 0;
}
static int hz_ok(const float *hz, uint32_t gw, const uint32_t c[4], float lim) {
    if (!hz) return 1;
    for (uint32_t y = c[1]; y <= c[3]; y++) { const float *row = hz + (size_t)y * gw; for (uint32_t x = c[0]; x <= c[2]; x++) if (row[x] < lim) return 0; }
    return 1;
}
// The cells of the snapshot whose pixels an NDC rect touches (no pad; far edges half-open). 0 = the rect is entirely on the snapshot.
static int cells_in(const float ndc[4], uint32_t w, uint32_t h, int yup, uint32_t c[4]) {
    const double px0 = ((double)ndc[0] * 0.5 + 0.5) * w, px1 = ((double)ndc[2] * 0.5 + 0.5) * w - 1e-3;
    const double ra = row_of(ndc[1], h, yup), rb = row_of(ndc[3], h, yup);
    const double py0 = ra < rb ? ra : rb, py1 = (ra < rb ? rb : ra) - 1e-3;
    if (!(px0 >= 0.0 && py0 >= 0.0 && px1 < (double)w && py1 < (double)h)) return 1;
    c[0] = (uint32_t)(px0 / VKN_EO_CELL); c[1] = (uint32_t)(py0 / VKN_EO_CELL);
    c[2] = (uint32_t)((px1 > px0 ? px1 : px0) / VKN_EO_CELL); c[3] = (uint32_t)((py1 > py0 ? py1 : py0) / VKN_EO_CELL);
    return 0;
}
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
// the turn of the view axis (clip w's direction) and of the screen x axis between two view-projections, degrees
static float turn_of(const float *a0, const float *b0) {
    double ang = 0.0;
    for (int j = 0; j < 4; j += 3) {
        const double a[3] = {a0[j], a0[4 + j], a0[8 + j]}, b[3] = {b0[j], b0[4 + j], b0[8 + j]};
        const double na = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), nb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        if (!(na > 0 && nb > 0)) return 180.0f;
        double c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (na * nb); c = c > 1 ? 1 : c < -1 ? -1 : c;
        const double x = acos(c) * 57.29577951308232; ang = x > ang ? x : ang;
    }
    return (float)ang;
}
int vkn_eo_view_prep(vkn_eo_view *v, const vkn_eo_snap *s, const float vp_now[16], const float *hz, const vkn_eo_cfg *cfg) {
    memset(v, 0, sizeof *v);
    v->s = s; v->hz = hz; memcpy(v->vp, vp_now, sizeof v->vp);
    if (!memcmp(vp_now, s->vp, sizeof v->vp)) { v->same = 1; return 0; }
    double e0[3], e1[3];
    if (vkn_oz_eye(s->vp, e0) || vkn_oz_eye(vp_now, e1) || !s->persp || inv4(vp_now, v->inv)) return VKN_EO_R_NOREPROJ;
    v->move = (float)sqrt((e1[0] - e0[0]) * (e1[0] - e0[0]) + (e1[1] - e0[1]) * (e1[1] - e0[1]) + (e1[2] - e0[2]) * (e1[2] - e0[2]));
    v->turn = turn_of(s->vp, vp_now);
    if (!(v->move <= cfg->max_move) || !(v->turn <= cfg->max_turn)) return VKN_EO_R_CUT;
    for (int j = 0; j < 4; j++) v->ce[j] = e1[0] * s->vp[j] + e1[1] * s->vp[4 + j] + e1[2] * s->vp[8 + j] + s->vp[12 + j];
    v->moved = 1;
    return 0;
}

// ---- the moved camera (technique Z v2) ----
typedef struct rbox { double cq[8][4]; float r0[4]; int n; } rbox;   // the corners in the snapshot's clip space (4 of a card, 8 of a box)
// The card (this frame's cells c at depth z, a quad in this frame's camera): its corners unprojected and projected into the snapshot's
// camera; the rect / cells there and its nearest depth there.
static int reproj_card(const vkn_eo_view *v, const uint32_t c[4], float z, rbox *q, uint32_t c0[4], float *zb) {
    const vkn_eo_snap *s = v->s; const double cs = (double)VKN_EO_CELL;
    const double px0 = c[0] * cs, py0 = c[1] * cs, px1 = (c[2] + 1) * cs < s->w ? (c[2] + 1) * cs : s->w, py1 = (c[3] + 1) * cs < s->h ? (c[3] + 1) * cs : s->h;
    float *r0 = q->r0; double z0 = DBL_MAX;
    r0[0] = r0[1] = FLT_MAX; r0[2] = r0[3] = -FLT_MAX; q->n = 4;
    for (int k = 0; k < 4; k++) {
        const double nx = (k & 1 ? px1 : px0) / s->w * 2.0 - 1.0, ny = ndc_of_row(k & 2 ? py1 : py0, s->h, s->yup);
        double w[4];
        for (int j = 0; j < 4; j++) w[j] = nx * v->inv[j] + ny * v->inv[4 + j] + (double)z * v->inv[8 + j] + v->inv[12 + j];
        if (!(w[3] != 0.0)) return VKN_EO_R_REPROJ;
        const double p[3] = {w[0] / w[3], w[1] / w[3], w[2] / w[3]};
        double *cl = q->cq[k];
        for (int j = 0; j < 4; j++) cl[j] = p[0] * s->vp[j] + p[1] * s->vp[4 + j] + p[2] * s->vp[8 + j] + s->vp[12 + j];
        if (!(cl[3] > 1e-6)) return VKN_EO_R_REPROJ;
        const float x = (float)(cl[0] / cl[3]), y = (float)(cl[1] / cl[3]); const double zz = cl[2] / cl[3];
        r0[0] = x < r0[0] ? x : r0[0]; r0[2] = x > r0[2] ? x : r0[2]; r0[1] = y < r0[1] ? y : r0[1]; r0[3] = y > r0[3] ? y : r0[3]; z0 = zz < z0 ? zz : z0;
    }
    if (!(z0 == z0)) return VKN_EO_R_REPROJ;
    if (cells_in(r0, s->w, s->h, s->yup, c0)) return VKN_EO_R_EDGE;   // (the snapshot must have seen all of it)
    *zb = (float)z0;
    return 0;
}
// [v2] The BOX itself into the snapshot's camera (its 8 world corners: what the engine culls with), the rect / cells there (+ pad, clamped
// for the depth test; the unpadded rect must lie on the snapshot) and its nearest depth there. The rays of the new eye through the box
// are bounded by the region built from these 8 corners exactly as from a card's 4 (the proof of VK-OCCLUSION-CHEAP.md 10.3 needs only
// that every ray to a point of the object starts at a corner hull point behind the surface: the box contains the object).
static int reproj_box(const vkn_eo_view *v, const float b[6], uint32_t pad, rbox *q, uint32_t c0[4], float *zb) {
    const vkn_eo_snap *s = v->s;
    float *r0 = q->r0; double z0 = DBL_MAX;
    r0[0] = r0[1] = FLT_MAX; r0[2] = r0[3] = -FLT_MAX; q->n = 8;
    for (int k = 0; k < 8; k++) {
        const double p[3] = {k & 1 ? b[3] : b[0], k & 2 ? b[4] : b[1], k & 4 ? b[5] : b[2]};
        double *cl = q->cq[k];
        for (int j = 0; j < 4; j++) cl[j] = p[0] * s->vp[j] + p[1] * s->vp[4 + j] + p[2] * s->vp[8 + j] + s->vp[12 + j];
        if (!(cl[3] > 1e-6)) return VKN_EO_R_REPROJ;
        const float x = (float)(cl[0] / cl[3]), y = (float)(cl[1] / cl[3]); const double zz = cl[2] / cl[3];
        r0[0] = x < r0[0] ? x : r0[0]; r0[2] = x > r0[2] ? x : r0[2]; r0[1] = y < r0[1] ? y : r0[1]; r0[3] = y > r0[3] ? y : r0[3]; z0 = zz < z0 ? zz : z0;
    }
    uint32_t chk[4];
    if (!(z0 == z0) || cells_in(r0, s->w, s->h, s->yup, chk)) return VKN_EO_R_EDGE;
    if (cells_pad(r0, s, pad, c0)) return VKN_EO_R_EDGE;
    *zb = (float)z0;
    return 0;
}
static float cmin_over(const vkn_eo_snap *s, const uint32_t c[4]) {
    float z = FLT_MAX;
    for (uint32_t y = c[1]; y <= c[3]; y++) { const float *r = s->mn + (size_t)y * s->gw; for (uint32_t x = c[0]; x <= c[2]; x++) z = r[x] < z ? r[x] : z; }
    return z;
}
static int reproj_region(const vkn_eo_view *v, const rbox *q, const uint32_t c0[4], uint32_t cr[4]) {
    const vkn_eo_snap *s = v->s;
    float zo = cmin_over(s, c0);
    for (int it = 0; it < ITER; it++) {
        float r[4]; memcpy(r, q->r0, sizeof r);
        for (int k = 0; k < q->n; k++) {
            const double *c = q->cq[k];
            const double f0 = c[2] - (double)zo * c[3], f1 = v->ce[2] - (double)zo * v->ce[3];
            if (!(f0 > 0.0) || !(f1 < 0.0)) return VKN_EO_R_REPROJ;
            const double t = f0 / (f0 - f1);
            double p[4]; for (int j = 0; j < 4; j++) p[j] = c[j] + t * (v->ce[j] - c[j]);
            if (!(p[3] > 1e-6)) return VKN_EO_R_REPROJ;
            const float x = (float)(p[0] / p[3]), y = (float)(p[1] / p[3]);
            r[0] = x < r[0] ? x : r[0]; r[2] = x > r[2] ? x : r[2]; r[1] = y < r[1] ? y : r[1]; r[3] = y > r[3] ? y : r[3];
        }
        if (!(r[0] == r[0] && r[1] == r[1] && r[2] == r[2] && r[3] == r[3])) return VKN_EO_R_REPROJ;
        if (cells_in(r, s->w, s->h, s->yup, cr)) return VKN_EO_R_EDGE;
        const float z2 = cmin_over(s, cr);
        if (z2 >= zo) return 0;
        zo = z2;
    }
    return VKN_EO_R_REPROJ;
}
static int sheet_ok(const vkn_eo_snap *s, float sheet, const uint32_t c[4], uint32_t *cells) {
    double lo_prev[VKN_EO_GW], hi_prev[VKN_EO_GW];
    const double t = (double)sheet;
    for (uint32_t y = c[1]; y <= c[3]; y++) {
        const float *mx = s->mx + (size_t)y * s->gw, *mn = s->mn + (size_t)y * s->gw;
        double lo_l = 0.0, hi_l = 0.0;
        for (uint32_t x = c[0]; x <= c[2]; x++) {
            (*cells)++;
            if (!(mx[x] < 1.0f) || !(mn[x] <= mx[x])) return 0;
            const double d0 = (double)mn[x] - s->la, d1 = (double)mx[x] - s->la;
            double l0 = d0 != 0.0 ? s->lb / d0 : -1.0, l1 = d1 != 0.0 ? s->lb / d1 : -1.0;
            if (l0 > l1) { const double qq = l0; l0 = l1; l1 = qq; }
            if (!(l0 > 0.0) || !(l1 < 1e30) || l1 - l0 > t * l0) return 0;
            if (x > c[0] && (l0 > hi_l * (1.0 + t) || lo_l > l1 * (1.0 + t))) return 0;
            if (y > c[1] && (l0 > hi_prev[x] * (1.0 + t) || lo_prev[x] > l1 * (1.0 + t))) return 0;
            lo_l = l0; hi_l = l1; lo_prev[x] = l0; hi_prev[x] = l1;
        }
    }
    return 1;
}

int vkn_eo_test(const vkn_eo_view *v, const vkn_eo_cfg *cfg, const float box[6], vkn_eo_res *r) {
    const vkn_eo_snap *s = v->s;
    float ndc[4], zmin = 0.0f; uint32_t c[4];
    r->cells = 0;
    if (vkn_oz_project(box, v->vp, ndc, &zmin)) return VKN_EO_R_NEAR;
    if (cells_pad(ndc, s, cfg->pad_px, c)) return VKN_EO_R_OFFSCREEN;
    r->zmin = zmin; for (int k = 0; k < 4; k++) r->c[k] = (uint16_t)c[k];
    if (v->same) {
        const float lim = lim_of(s, zmin, cfg->margin);
        if (!depth_ok(s, c, lim, &r->cells)) return VKN_EO_R_DEPTH;
        if (!hz_ok(v->hz, s->gw, c, lim)) return VKN_EO_R_HAZARD;
        return VKN_EO_HIDDEN;
    }
    if (!v->moved) return VKN_EO_R_NOREPROJ;
    rbox q; uint32_t cs[4], cr[4]; float zb = 0.0f;
    int e;
    if (cfg->box_reproj) {   // [v2] the box, not the card: the self-check cells are then the box's own rect in this frame (no pad)
        uint32_t c1[4]; if (!cells_pad(ndc, s, 0, c1)) for (int k = 0; k < 4; k++) r->c[k] = (uint16_t)c1[k];
        e = reproj_box(v, box, cfg->pad_px, &q, cs, &zb);
    } else e = reproj_card(v, c, zmin, &q, cs, &zb);
    if (e) return e;
    const float lim = lim_of(s, zb, cfg->margin);
    if (!depth_ok(s, cs, lim, &r->cells)) return VKN_EO_R_DEPTH;
    if ((e = reproj_region(v, &q, cs, cr))) return e;
    if (!hz_ok(v->hz, s->gw, cr, lim)) return VKN_EO_R_HAZARD;
    if (!sheet_ok(s, cfg->sheet, cr, &r->cells)) return VKN_EO_R_SHEET;
    return VKN_EO_HIDDEN;
}

int vkn_eo_hz_add(float *hz, const vkn_eo_snap *s, const float box[6], uint32_t pad) {
    float ndc[4], z = 0.0f; uint32_t c[4];
    const int p = vkn_oz_project(box, s->vp, ndc, &z);
    if (p == -1) { for (size_t i = 0; i < (size_t)s->gw * s->gh; i++) hz[i] = -FLT_MAX; return 1; }
    if (p || cells_pad(ndc, s, pad, c)) return 0;
    for (uint32_t y = c[1]; y <= c[3]; y++) { float *row = hz + (size_t)y * s->gw; for (uint32_t x = c[0]; x <= c[2]; x++) if (z < row[x]) row[x] = z; }
    return 0;
}

int vkn_eo_check(const vkn_eo_snap *s, float zmin, const uint16_t c[4]) {
    if (c[2] >= s->gw || c[3] >= s->gh || c[0] > c[2] || c[1] > c[3]) return 0;
    for (uint32_t y = c[1]; y <= c[3]; y++) { const float *row = s->mx + (size_t)y * s->gw; for (uint32_t x = c[0]; x <= c[2]; x++) if (!(row[x] < zmin)) return 1; }
    return 0;
}
#endif
