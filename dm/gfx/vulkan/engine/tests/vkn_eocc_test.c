// [DOS2] VK-ENGINE-SIDE I2 host test: the pure test dm/gfx/vkn/vkn_eocc_model.c (rule: dm/gfx/vkn/vkn_eocc.h). Synthetic scenes of
// axis-aligned boxes are ray-cast per pixel centre (as the GPU's pre-pass depth: D3D-style 0..1, Vulkan screen y down) from the
// snapshot's camera; the snapshot = max / min depth of every 16 x 16 cell (+ the coarse level), as vkn_occz_reduce.comp builds it.
// The truth of a decision is the ray-cast of THIS frame's camera: an object "hidden" by the test must have no pixel in front of the
// scene from the new eye.
//   static     a wall in front: the box behind it hidden; a box beside / in front / just behind within the margin: drawn;
//   moved      eye moved sideways / forward / turned: hidden through the reprojection while it is truly hidden; the box at the wall's
//              edge that the new eye sees past the edge: drawn; the doorway behind a pole (hidden from the old eye, visible through
//              the doorway from the new one): drawn ("not a sheet"); a camera cut: nothing hidden; the card off the snapshot's
//              screen: drawn;
//   hazards    the wall's footprint in the hazard grid: drawn; a hazard behind the box: no effect; a hazard behind the camera: whole
//              grid;
//   coarse     the coarse level never changes a verdict (random scenes, with and without it);
//   check      the self-check against the frame's own depth: a covered decision passes, an uncovered one is flagged;
//   random     random walls / poles / boxes and small camera moves: no false hide, and a real share hidden.
// Build + run (WSL gcc):
//   gcc -std=c11 -D_GNU_SOURCE -DDM_GAME_DOS2=1 -O1 -g -Wall -Wextra -o /tmp/vkn_eocc_test dm/gfx/vulkan/engine/tests/vkn_eocc_test.c
//       dm/gfx/vkn/vkn_eocc_model.c dm/gfx/vkn/vkn_occz_model.c -lm && /tmp/vkn_eocc_test
// Ends with "vkn_eocc_test: N checks passed" (exit 0) or the failures and exit 1.
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../../vkn/vkn_eocc.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_fail++; printf("FAIL %s:%d %s = %lld, want %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

enum { W = 480, H = 272, GW = (W + 15) / 16, GH = (H + 15) / 16, MAXB = 32 };
typedef struct cam { double e[3], r[3], u[3], f[3], sx, sy, a, b, n; float vp[16]; } cam;
typedef struct box { float b[6]; } box;
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(double *a) { const double l = sqrt(dot3(a, a)); a[0] /= l; a[1] /= l; a[2] /= l; }
static void cross3(const double *a, const double *b, double *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
// look-at camera: x right, y DOWN on the screen (NDC -1 = top), depth 0 at near .. 1 at far; row-vector view-projection
static cam mk_cam(double ex, double ey, double ez, double tx, double ty, double tz, double fov_deg) {
    cam c; memset(&c, 0, sizeof c);
    c.e[0] = ex; c.e[1] = ey; c.e[2] = ez;
    c.f[0] = tx - ex; c.f[1] = ty - ey; c.f[2] = tz - ez; norm3(c.f);
    const double up[3] = {0, 1, 0}; cross3(c.f, up, c.r); norm3(c.r); cross3(c.r, c.f, c.u);
    c.sy = 1.0 / tan(fov_deg * 0.5 * 3.14159265358979 / 180.0); c.sx = c.sy * H / W;
    const double n = 0.5, fa = 200.0; c.n = n; c.a = fa / (fa - n); c.b = -n * fa / (fa - n);
    const double dr = dot3(c.e, c.r), du = dot3(c.e, c.u), df = dot3(c.e, c.f);
    for (int i = 0; i < 3; i++) {
        c.vp[4 * i + 0] = (float)(c.r[i] * c.sx); c.vp[4 * i + 1] = (float)(-c.u[i] * c.sy); c.vp[4 * i + 2] = (float)(c.f[i] * c.a); c.vp[4 * i + 3] = (float)c.f[i];
    }
    c.vp[12] = (float)(-dr * c.sx); c.vp[13] = (float)(du * c.sy); c.vp[14] = (float)(-df * c.a + c.b); c.vp[15] = (float)(-df);
    return c;
}
// the ray of pixel centre (px, py): point = e + t * d with view depth t
static int g_yup;   // [v2] 1: the image's row 0 is NDC y = +1 (the DOS2 backend: flipped viewport); the snapshot says so (s->yup)
static void ray(const cam *c, double px, double py, double d[3]) {
    const double nx = px / W * 2 - 1, ny = g_yup ? 1 - py / H * 2 : py / H * 2 - 1;
    for (int i = 0; i < 3; i++) d[i] = c->f[i] + nx / c->sx * c->r[i] - ny / c->sy * c->u[i];
}
static double slab(const cam *c, const double d[3], const float *b) {   // entry view depth, or -1
    double t0 = c->n, t1 = 1e30;
    for (int i = 0; i < 3; i++) {
        if (fabs(d[i]) < 1e-12) { if (c->e[i] < b[i] || c->e[i] > b[3 + i]) return -1; continue; }
        double ta = (b[i] - c->e[i]) / d[i], tb = (b[3 + i] - c->e[i]) / d[i]; if (ta > tb) { const double q = ta; ta = tb; tb = q; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return -1;
    }
    return t0;
}
static float g_mx[GW * GH], g_mn[GW * GH], g_cx[GW * GH];
static double g_px[W * H];   // the pixels' view depth of the last render (truth)
// renders the scene's depth from c: the snapshot planes (when snap) and the per-pixel view depth (1e30 = background)
static void render(const cam *c, const box *bx, int nb, vkn_eo_snap *s, int coarse) {
    for (int i = 0; i < GW * GH; i++) { g_mx[i] = 0.0f; g_mn[i] = 1.0f; }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        double d[3]; ray(c, x + 0.5, y + 0.5, d);
        double t = 1e30;
        for (int k = 0; k < nb; k++) { const double q = slab(c, d, bx[k].b); if (q > 0 && q < t) t = q; }
        g_px[y * W + x] = t;
        const float z = t < 1e29 ? (float)(c->a + c->b / t) : 1.0f;
        float *m = &g_mx[(y / 16) * GW + x / 16], *n = &g_mn[(y / 16) * GW + x / 16];
        if (z > *m) *m = z;
        if (z < *n) *n = z;
    }
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->w = W; s->h = H; s->gw = GW; s->gh = GH; s->mx = g_mx; s->mn = g_mn; memcpy(s->vp, c->vp, sizeof s->vp); s->yup = g_yup;
    if (coarse) { vkn_eo_coarse(g_mx, GW, GH, g_cx, &s->cgw, &s->cgh); s->cx = g_cx; }
    CHECK_EQ(vkn_eo_snap_prep(s), 0);
}
// 1 = some pixel of the box is in front of the scene from c (the box itself not part of the scene)
static int visible(const cam *c, const box *bx, int nb, const float *o) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        double d[3]; ray(c, x + 0.5, y + 0.5, d);
        const double to = slab(c, d, o);
        if (!(to > 0)) continue;
        double t = 1e30;
        for (int k = 0; k < nb; k++) { const double q = slab(c, d, bx[k].b); if (q > 0 && q < t) t = q; }
        if (to < t) return 1;
    }
    return 0;
}
static int g_box = 1;   // [v2] moved camera: the box reprojected (1) or technique Z's card (0)
static vkn_eo_cfg cfg_def(void) { vkn_eo_cfg c = {16, 0.02f, 0.05f, 2.0f, 8.0f, g_box}; return c; }
static box B(float x0, float y0, float z0, float x1, float y1, float z1) { box b = {{x0, y0, z0, x1, y1, z1}}; return b; }
static int decide(const vkn_eo_snap *s, const cam *now, const float *hz, const vkn_eo_cfg *cf, const float *o, vkn_eo_res *r) {
    vkn_eo_view v; const int e = vkn_eo_view_prep(&v, s, now->vp, hz, cf);
    if (e) return e;
    vkn_eo_res rr; return vkn_eo_test(&v, cf, o, r ? r : &rr);
}

// the scene: eye at z = -20 looking along +z; a wall at z 0..1 (x -6..6, y -3..5); the floor below
static box g_scene[MAXB]; static int g_nb;
static void scene_wall(void) { g_nb = 0; g_scene[g_nb++] = B(-6, -3, 0, 6, 5, 1); }

static void t_static(void) {
    const vkn_eo_cfg cf = cfg_def(); vkn_eo_snap s; vkn_eo_res r;
    scene_wall();
    const cam c = mk_cam(0, 1, -20, 0, 1, 0, 50);
    render(&c, g_scene, g_nb, &s, 1);
    const box hid = B(-1, 0, 3, 1, 2, 5);
    CHECK_EQ(decide(&s, &c, NULL, &cf, hid.b, &r), VKN_EO_HIDDEN);
    CHECK(!visible(&c, g_scene, g_nb, hid.b));
    const box beside = B(8, 0, 3, 10, 2, 5);
    CHECK_EQ(decide(&s, &c, NULL, &cf, beside.b, NULL), VKN_EO_R_DEPTH);
    const box front = B(-1, 0, -3, 1, 2, -2);
    CHECK_EQ(decide(&s, &c, NULL, &cf, front.b, NULL), VKN_EO_R_DEPTH);
    const box close = B(-1, 0, 1.1f, 1, 2, 1.5f);   // 1.1 behind the wall's front at view depth 20: beyond the 2 % margin
    CHECK_EQ(decide(&s, &c, NULL, &cf, close.b, NULL), VKN_EO_HIDDEN);
    { box thin[1] = {B(-6, -3, 0, 6, 5, 0.1f)}; vkn_eo_snap st; render(&c, thin, 1, &st, 1);
      const box near_ = B(-1, 0, 0.2f, 1, 2, 0.6f);   // 0.2 behind a thin wall's front: inside the margin (1 %)
      CHECK_EQ(decide(&st, &c, NULL, &cf, near_.b, NULL), VKN_EO_R_DEPTH);
      vkn_eo_cfg c0 = cf; c0.margin = 0.0f;
      CHECK_EQ(decide(&st, &c, NULL, &c0, near_.b, NULL), VKN_EO_HIDDEN);
      render(&c, g_scene, g_nb, &s, 1); }
    const box peek = B(5, 0, 3, 7.5f, 2, 5);   // sticks out past the wall's edge
    CHECK_EQ(decide(&s, &c, NULL, &cf, peek.b, NULL), VKN_EO_R_DEPTH);
    CHECK(visible(&c, g_scene, g_nb, peek.b));
    const box above = B(-1, 4.6f, 3, 1, 6, 5);   // above the wall top
    CHECK(decide(&s, &c, NULL, &cf, above.b, NULL) != VKN_EO_HIDDEN);
    const box behind_cam = B(-1, 0, -30, 1, 2, -10);
    CHECK_EQ(decide(&s, &c, NULL, &cf, behind_cam.b, NULL), VKN_EO_R_NEAR);
    const box off = B(40, 0, 3, 42, 2, 5);
    CHECK_EQ(decide(&s, &c, NULL, &cf, off.b, NULL), VKN_EO_R_OFFSCREEN);
    // a coarse pass and a fine pass agree; the card of a hidden box is its cell rect + pad
    CHECK(r.c[0] <= r.c[2] && r.c[1] <= r.c[3] && r.c[2] < GW && r.c[3] < GH);
    // the self-check against this frame's own depth: covered
    CHECK_EQ(vkn_eo_check(&s, r.zmin, r.c), 0);
    // the wall gone in the frame's own depth: flagged
    box none[1]; vkn_eo_snap s2; render(&c, none, 0, &s2, 0);
    CHECK_EQ(vkn_eo_check(&s2, r.zmin, r.c), 1);
}

static void t_moved(void) {
    const vkn_eo_cfg cf = cfg_def(); vkn_eo_snap s; vkn_eo_res r;
    scene_wall();
    const cam c0 = mk_cam(0, 1, -20, 0, 1, 0, 50);
    render(&c0, g_scene, g_nb, &s, 1);
    const box hid = B(-1, 0, 3, 1, 2, 5);
    // small moves: still hidden, truly
    const cam m1 = mk_cam(0.6, 1, -20, 0.6, 1, 0, 50), m2 = mk_cam(0, 1.3, -19.2, 0, 1.3, 0.8, 50), m3 = mk_cam(0, 1, -20, 0.8, 1.2, 0, 50);
    const cam *ms[3] = {&m1, &m2, &m3};
    for (int i = 0; i < 3; i++) {
        vkn_eo_view v; CHECK_EQ(vkn_eo_view_prep(&v, &s, ms[i]->vp, NULL, &cf), 0); CHECK_EQ(v.moved, 1); CHECK_EQ(v.same, 0);
        CHECK_EQ(vkn_eo_test(&v, &cf, hid.b, &r), VKN_EO_HIDDEN);
        CHECK(!visible(ms[i], g_scene, g_nb, hid.b));
    }
    // the box just behind a near wall's right edge (the edge on a cell border, pad 0): hidden from the old eye, the new eye (moved
    // right) sees past the edge
    { vkn_eo_cfg c0 = cf; c0.pad_px = 0; vkn_eo_snap se;
      box ws[1] = {B(-6, -3, 0, 1.6457f, 5, 1)};
      const cam n0 = mk_cam(0, 1, -5, 0, 1, 0, 50), nr = mk_cam(1.9, 1, -5, 1.9, 1, 0, 50);
      render(&n0, ws, 1, &se, 1);
      const box edge = B(1.6f, 0, 2, 2.2f, 1.5f, 3);
      CHECK_EQ(decide(&se, &n0, NULL, &c0, edge.b, NULL), VKN_EO_HIDDEN);
      CHECK(!visible(&n0, ws, 1, edge.b));
      CHECK(visible(&nr, ws, 1, edge.b));
      CHECK(decide(&se, &nr, NULL, &c0, edge.b, NULL) != VKN_EO_HIDDEN); }
    // a camera cut: nothing hidden
    const cam cut = mk_cam(6, 1, -20, 6, 1, 0, 50);
    CHECK_EQ(decide(&s, &cut, NULL, &cf, hid.b, NULL), VKN_EO_R_CUT);
    const cam turn = mk_cam(0, 1, -20, 5, 1, 0, 50);   // ~14 degrees
    CHECK_EQ(decide(&s, &turn, NULL, &cf, hid.b, NULL), VKN_EO_R_CUT);
    // turned so that the card leaves the snapshot's screen: a box at the left edge of the old view
    box sc[2] = {B(-30, -3, 0, 30, 5, 1)}; render(&c0, sc, 1, &s, 1);
    const box left = B(-21, 0, 3, -17, 2, 5);   // (partly beyond the old view's left edge)
    CHECK_EQ(decide(&s, &c0, NULL, &cf, left.b, NULL), VKN_EO_HIDDEN);
    const cam tl = mk_cam(0, 1, -20, -1.2, 1, 0, 50);
    CHECK_EQ(decide(&s, &tl, NULL, &cf, left.b, NULL), VKN_EO_R_EDGE);
}

static void t_doorway(void) {
    vkn_eo_cfg cf = cfg_def(); vkn_eo_snap s; cf.pad_px = 0;
    // a wall at z 4..5 with a doorway x -1..1; a pole in front of the doorway at z 0..0.6, x -0.9..0.9; the box behind the doorway
    g_nb = 0;
    g_scene[g_nb++] = B(-8, -3, 4, -1, 5, 5); g_scene[g_nb++] = B(1, -3, 4, 8, 5, 5); g_scene[g_nb++] = B(-0.9f, -3, 0, 0.9f, 5, 0.6f);
    const cam c0 = mk_cam(0, 1, -6, 0, 1, 0, 50);
    render(&c0, g_scene, g_nb, &s, 1);
    const box o = B(-0.5f, 0, 8, 0.5f, 2, 9);
    CHECK(!visible(&c0, g_scene, g_nb, o.b));
    CHECK_EQ(decide(&s, &c0, NULL, &cf, o.b, NULL), VKN_EO_HIDDEN);   // same camera: the pole covers it
    const cam m = mk_cam(1.5, 1, -6, 1.5, 1, 0, 50);
    CHECK(visible(&m, g_scene, g_nb, o.b));   // through the doorway past the pole
    const int r = decide(&s, &m, NULL, &cf, o.b, NULL);
    CHECK(r != VKN_EO_HIDDEN);
    CHECK(r == VKN_EO_R_SHEET || r == VKN_EO_R_DEPTH || r == VKN_EO_R_REPROJ);
}

static void t_hazards(void) {
    const vkn_eo_cfg cf = cfg_def(); vkn_eo_snap s;
    scene_wall();
    const cam c = mk_cam(0, 1, -20, 0, 1, 0, 50);
    render(&c, g_scene, g_nb, &s, 1);
    static float hz[GW * GH];
    for (int i = 0; i < GW * GH; i++) hz[i] = FLT_MAX;
    const box hid = B(-1, 0, 3, 1, 2, 5);
    CHECK_EQ(decide(&s, &c, hz, &cf, hid.b, NULL), VKN_EO_HIDDEN);
    const box behind = B(-3, -1, 12, 3, 3, 14);   // a moving object behind the box: no hazard for it
    CHECK_EQ(vkn_eo_hz_add(hz, &s, behind.b, cf.pad_px), 0);
    CHECK_EQ(decide(&s, &c, hz, &cf, hid.b, NULL), VKN_EO_HIDDEN);
    CHECK_EQ(vkn_eo_hz_add(hz, &s, g_scene[0].b, cf.pad_px), 0);   // the wall itself moved / faded: its footprint
    CHECK_EQ(decide(&s, &c, hz, &cf, hid.b, NULL), VKN_EO_R_HAZARD);
    const cam m = mk_cam(0.5, 1, -20, 0.5, 1, 0, 50);
    CHECK_EQ(decide(&s, &m, hz, &cf, hid.b, NULL), VKN_EO_R_HAZARD);   // moved camera: the swept region sees it too
    for (int i = 0; i < GW * GH; i++) hz[i] = FLT_MAX;
    const box cross_cam = B(-1, 0, -25, 1, 2, 3);   // reaches behind the snapshot's camera: the whole grid
    CHECK_EQ(vkn_eo_hz_add(hz, &s, cross_cam.b, cf.pad_px), 1);
    CHECK_EQ(decide(&s, &c, hz, &cf, hid.b, NULL), VKN_EO_R_HAZARD);
}

static uint32_t g_rng = 12345u;
static float rnd(float a, float b) { g_rng = g_rng * 1664525u + 1013904223u; return a + (b - a) * (float)((g_rng >> 8) & 0xffffff) / 16777216.0f; }
static void t_random(int n, uint32_t pad) {
    vkn_eo_cfg cf = cfg_def(); vkn_eo_snap s, s1; cf.pad_px = pad; int why[VKN_EO_R_N] = {0};
    int hidden = 0, moved_hidden = 0, false_hide = 0, coarse_diff = 0, tests = 0, truly_hidden = 0, check_bad = 0;
    for (int it = 0; it < n; it++) {
        g_nb = 0;
        const int nw = 1 + (int)rnd(0, 3.99f);
        for (int k = 0; k < nw; k++) {   // walls
            const float x = rnd(-8, 8), z = rnd(-6, 6), wdt = rnd(2, 10), hgt = rnd(2, 6);
            g_scene[g_nb++] = B(x - wdt / 2, -3, z, x + wdt / 2, -3 + hgt, z + rnd(0.2f, 1.5f));
        }
        const int np = (int)rnd(0, 3.99f);
        for (int k = 0; k < np; k++) { const float x = rnd(-6, 6), z = rnd(-10, 4); g_scene[g_nb++] = B(x - 0.4f, -3, z, x + 0.4f, 4, z + 0.4f); }   // poles
        if (rnd(0, 1) < 0.5f) g_scene[g_nb++] = B(-40, -4, -15, 40, -3, 60);   // the floor
        const cam c0 = mk_cam(rnd(-2, 2), rnd(0, 6), -20, rnd(-2, 2), rnd(-1, 2), 0, 50);
        render(&c0, g_scene, g_nb, &s, 1);
        // this frame's camera: 15 % the same, else a small move / turn
        cam c1 = c0;
        if (rnd(0, 1) > 0.15f) {
            const double d[3] = {rnd(-0.8f, 0.8f), rnd(-0.5f, 0.5f), rnd(-0.8f, 0.8f)};
            c1 = mk_cam(c0.e[0] + d[0], c0.e[1] + d[1], c0.e[2] + d[2], c0.e[0] + d[0] + c0.f[0] * 20 + rnd(-1, 1), c0.e[1] + d[1] + c0.f[1] * 20 + rnd(-1, 1),
                        c0.e[2] + d[2] + c0.f[2] * 20, 50 + rnd(-2, 2));
        }
        for (int j = 0; j < 4; j++) {
            const float x = rnd(-9, 9), y = rnd(-3, 3), z = rnd(-4, 14), sx = rnd(0.2f, 2.5f), sy = rnd(0.2f, 2.5f), sz = rnd(0.2f, 2.5f);
            const box o = B(x, y, z, x + sx, y + sy, z + sz);
            int inside = 0; for (int k = 0; k < g_nb; k++) { const float *b = g_scene[k].b; if (o.b[0] < b[3] && o.b[3] > b[0] && o.b[1] < b[4] && o.b[4] > b[1] && o.b[2] < b[5] && o.b[5] > b[2]) inside = 1; }
            if (inside) continue;   // (an object inside a wall: not a scene the engine has)
            tests++;
            vkn_eo_view v; vkn_eo_res r; int e = vkn_eo_view_prep(&v, &s, c1.vp, NULL, &cf);
            const int d1 = e ? e : vkn_eo_test(&v, &cf, o.b, &r);
            vkn_eo_snap sn = s; sn.cx = NULL; int d2 = e;
            if (!e) { vkn_eo_view v2; (void)vkn_eo_view_prep(&v2, &sn, c1.vp, NULL, &cf); vkn_eo_res r2; d2 = vkn_eo_test(&v2, &cf, o.b, &r2); }
            coarse_diff += (d1 == VKN_EO_HIDDEN) != (d2 == VKN_EO_HIDDEN);
            const int vis = visible(&c1, g_scene, g_nb, o.b);
            truly_hidden += !vis;
            if (!vis && d1 >= 0 && d1 < VKN_EO_R_N) why[d1]++;
            if (d1 == VKN_EO_HIDDEN) {
                hidden++; moved_hidden += v.moved;
                if (vis) { false_hide++; if (false_hide <= 5) printf("false hide: scene %d box %d (moved %d, move %.2f turn %.2f)\n", it, j, v.moved, (double)v.move, (double)v.turn); }
                else {   // the self-check on this frame's own depth (the box not drawn: mode 1) passes
                    render(&c1, g_scene, g_nb, &s1, 0);
                    check_bad += vkn_eo_check(&s1, r.zmin, r.c);
                    render(&c0, g_scene, g_nb, &s, 1);   // (the snapshot back)
                }
            }
        }
    }
    printf("random (pad %u): truly hidden by reason:", pad); for (int i = 0; i < VKN_EO_R_N; i++) if (why[i]) printf(" %s %d,", vkn_eo_reason(i), why[i]);
    printf("\n");
    printf("random scenes %d: %d boxes tested, %d truly hidden, %d hidden by the test (%d under a moved camera), false hides %d, coarse differs %d, self-check flags %d\n",
           n, tests, truly_hidden, hidden, moved_hidden, false_hide, coarse_diff, check_bad);
    CHECK_EQ(false_hide, 0);
    CHECK_EQ(coarse_diff, 0);
    CHECK(hidden * 10 >= truly_hidden || pad > 4);   // a share of what is truly hidden (box rects and cells are coarser than pixels; these scenes are sparse)
    CHECK(moved_hidden > 0 || pad > 4);
}

// a town-like view: a long wall across the screen, 40 boxes behind it at random, the camera still / moving a little each frame
static void t_dense(void) {
    const vkn_eo_cfg cf = cfg_def(); vkn_eo_snap s;
    g_nb = 0; g_scene[g_nb++] = B(-40, -3, 0, 40, 6, 1); g_scene[g_nb++] = B(-40, -4, -30, 40, -3, 80);
    const cam c0 = mk_cam(0, 3, -20, 0, 1, 0, 50);
    render(&c0, g_scene, g_nb, &s, 1);
    uint32_t rng = 99u; int hid_same = 0, hid_moved = 0, total = 0, false_hide = 0;
    const cam c1 = mk_cam(0.5, 3.1, -19.7, 0.5, 1.1, 0.3, 50);
    for (int i = 0; i < 40; i++) {
        rng = rng * 1664525u + 1013904223u; const float x = ((rng >> 8) & 0xffff) / 65536.0f * 24 - 12;
        rng = rng * 1664525u + 1013904223u; const float z = 3 + ((rng >> 8) & 0xffff) / 65536.0f * 20;
        const box o = B(x, -3, z, x + 1.5f, 0.5f, z + 1.5f);
        if (visible(&c0, g_scene, g_nb, o.b) || visible(&c1, g_scene, g_nb, o.b)) continue;
        total++;
        hid_same += decide(&s, &c0, NULL, &cf, o.b, NULL) == VKN_EO_HIDDEN;
        const int r = decide(&s, &c1, NULL, &cf, o.b, NULL);
        hid_moved += r == VKN_EO_HIDDEN;
    }
    for (int i = 0; i < 20; i++) {   // and nothing visible is hidden
        rng = rng * 1664525u + 1013904223u; const float x = ((rng >> 8) & 0xffff) / 65536.0f * 24 - 12;
        const box o = B(x, 6.5f, 5, x + 1, 9, 6);   // above the wall top: visible
        if (!visible(&c1, g_scene, g_nb, o.b)) continue;
        false_hide += decide(&s, &c1, NULL, &cf, o.b, NULL) == VKN_EO_HIDDEN;
    }
    printf("dense: %d truly hidden boxes, hidden by the test %d (same camera) / %d (moved 0.6 units), false hides %d\n", total, hid_same, hid_moved, false_hide);
    CHECK(total >= 20); CHECK(hid_same * 10 >= total * 9); CHECK(hid_moved * 10 >= total * 6); CHECK_EQ(false_hide, 0);
}


// [v2] phone F12 (OCC=1 standing: "visible near objects vanish massively", self-check VIOLATIONS 0): the image rows run from NDC y = +1
// (the backend flips the Vulkan viewport, vkn_draw.c), v1 mapped NDC y = -1 to row 0 - every box was tested against its MIRROR rows.
// Here: a town scene drawn the backend's way (row 0 = +1); the model told the truth (yup 1) hides no visible box; told v1's convention
// (yup 0) it hides visible boxes - and v1's self-check, reading the frame's own depth through the same wrong rows, flags none of them
// (the blind spot); the right rows flag them.
static void t_flip(int n) {
    const vkn_eo_cfg cf = cfg_def(); vkn_eo_snap s, own;
    int hid[2] = {0, 0}, fh[2] = {0, 0}, flag_wrong = 0, flag_right = 0, truly = 0;
    g_yup = 1;
    for (int sc = 0; sc < n; sc++) {
        g_nb = 0;
        g_scene[g_nb++] = B(-40, -4, -40, 40, -3, 60);
        const int nw = 2 + (int)rnd(0, 3.99f);
        for (int k = 0; k < nw; k++) { const float x = rnd(-8, 8), z = rnd(-2, 10), w = rnd(3, 10), h = rnd(2, 7); g_scene[g_nb++] = B(x - w / 2, -3, z, x + w / 2, -3 + h, z + rnd(0.3f, 1.2f)); }
        const cam c = mk_cam(rnd(-1, 1), 11, -11, 0, 0, 2, 45);   // iso-like: near objects at the bottom of the screen, far at the top
        render(&c, g_scene, g_nb, &s, 1);
        for (int j = 0; j < 16; j++) {
            const float x = rnd(-10, 10), z = rnd(-4, 14), sx = rnd(0.3f, 1.5f), sy = rnd(0.3f, 2.5f), sz = rnd(0.3f, 1.5f);
            const box o = B(x, -3, z, x + sx, -3 + sy, z + sz);
            int inside = 0; for (int k = 1; k < g_nb; k++) { const float *q = g_scene[k].b; if (o.b[0] < q[3] && o.b[3] > q[0] && o.b[2] < q[5] && o.b[5] > q[2]) inside = 1; }
            if (inside) continue;
            const int vis = visible(&c, g_scene, g_nb, o.b);
            truly += !vis;
            for (int conv = 0; conv < 2; conv++) {
                vkn_eo_snap t = s; t.yup = conv;
                vkn_eo_res r;
                if (decide(&t, &c, NULL, &cf, o.b, &r) != VKN_EO_HIDDEN) continue;
                hid[conv]++; fh[conv] += vis;
                if (vis) {   // the self-check on the frame's own depth without the object (= the scene here), through each convention's rows
                    own = s; own.yup = conv;
                    if (conv == 0) flag_wrong += vkn_eo_check(&own, r.zmin, r.c); else flag_right += vkn_eo_check(&own, r.zmin, r.c);
                }
            }
        }
    }
    g_yup = 0;
    printf("rows from NDC +1 (the backend's flipped viewport): truly hidden %d; the right rows hide %d (FALSE %d); v1's rows hide %d (FALSE %d), "
           "of those false ones v1's self-check flags %d\n", truly, hid[1], fh[1], hid[0], fh[0], flag_wrong);
    CHECK_EQ(fh[1], 0); CHECK(hid[1] > 0);
    CHECK(fh[0] > 0);                      // the reproduction of F12: visible objects hidden
    CHECK(flag_wrong * 2 < fh[0]);          // ... and the self-check through the same rows misses most of them (blind)
    (void)flag_right;
}

// [I2 v2] the follow camera of phone runs F13 / F14: the camera keeps an iso-like offset to a walking (and turning) character; the
// character is an occluder that moves with the camera; a cloth (awning) sways in the wind INSIDE its constant engine box (its depth
// changes, its box does not: the vertex shader moves it). The snapshot is 4 frames old. The truth: the frame's own camera, the character
// and the cloth where they are now. Set-ups:
//   exact        the snapshot camera right, the character's boxes over the 4 frames and the cloth's engine box (an animated object,
//                vkn_eocc.c learns it from its non-affine vertex shader) in the hazard grid: no false hide, objects still hidden;
//   label -1     the snapshot drawn with the camera of the frame BEFORE the one it is labelled with (a snapshot tagged by the frame
//                counter at the draw's begin while the engine builds the next frame's lists: what the encoder's pair label fixes);
//   no hazard    the character not in the hazard grid;
//   cloth        the swaying cloth not in the hazard grid - the reproduction of the F13 / F14 violations: objects under / behind an
//                awning, a banner, a tree, hidden by a snapshot in which the cloth covered them.
typedef struct fc_res { int truly, hidden, moved, false_hide, why[VKN_EO_R_N]; } fc_res;
static double g_fc_v = 1.0;   // the walking speed (0: standing still - the camera does not move at all)
static cam fc_cam(int t, double turn_deg) {   // the character at (0.12 t, 0, 0.08 t), the eye 11 up and 11 back, orbiting by turn_deg a frame
    const double px = 0.12 * t * g_fc_v, pz = 0.08 * t * g_fc_v, a = turn_deg * t * 3.14159265358979 / 180.0;
    const double ox = -11.0 * sin(a), oz = -11.0 * cos(a);
    return mk_cam(px + ox, 11.0, pz + oz, px, 0.0, pz, 45);
}
static box fc_char(int t) { const float px = 0.12f * (float)t * (float)g_fc_v, pz = 0.08f * (float)t * (float)g_fc_v; return B(px - 0.3f, -3, pz - 0.3f, px + 0.3f, 1.8f, pz + 0.3f); }
static box fc_cloth(float cx, float cz, int t) { const float x = cx + 1.1f * sinf(0.7f * (float)t); return B(x - 1.2f, -2.2f, cz, x + 1.2f, 1.5f, cz + 0.1f); }
static box fc_cloth_box(float cx, float cz) { return B(cx - 2.3f, -2.2f, cz, cx + 2.3f, 1.5f, cz + 0.1f); }
static int fc_overlap_xz(const float *a, const float *b) { return a[0] < b[3] && a[3] > b[0] && a[2] < b[5] && a[5] > b[2]; }
static void fc_run(int scenes, int setup, double turn, fc_res *o) {
    vkn_eo_cfg cf = cfg_def(); cf.pad_px = 4;   // (4 px at 480 x 272 = the 16 px of 2160 x 972 in screen share)
    static float hz[GW * GH];
    memset(o, 0, sizeof *o);
    for (int sc = 0; sc < scenes; sc++) {
        box base[MAXB]; int nb = 0;
        base[nb++] = B(-40, -4, -40, 40, -3, 60);   // the ground
        const int nw = 2 + (int)rnd(0, 3.99f);
        for (int k = 0; k < nw; k++) { const float x = rnd(-6, 6), z = rnd(-2, 10), w = rnd(3, 10), h = rnd(3, 8); base[nb++] = B(x - w / 2, -3, z, x + w / 2, -3 + h, z + rnd(0.3f, 1.2f)); }   // houses
        const int np = 1 + (int)rnd(0, 3.99f);
        for (int k = 0; k < np; k++) { const float x = rnd(-5, 5), z = rnd(-3, 9); base[nb++] = B(x - 0.3f, -3, z - 0.3f, x + 0.3f, rnd(-1.5f, 2), z + 0.3f); }   // posts, crates
        const int s = 4 + (int)rnd(0, 20), E = s + 4;
        const float ccx = 0.12f * (float)E + rnd(-3, 3), ccz = 0.08f * (float)E + rnd(1, 6);   // the awning near the path
        const int sdraw = setup == 1 ? s - 1 : s;   // label -1: the depth is the previous frame's
        const cam cs = fc_cam(sdraw, turn), cl = fc_cam(s, turn), ce = fc_cam(E, turn);
        box scn[MAXB + 2]; memcpy(scn, base, (size_t)nb * sizeof *base);
        scn[nb] = fc_char(sdraw); scn[nb + 1] = fc_cloth(ccx, ccz, sdraw);
        vkn_eo_snap sn; render(&cs, scn, nb + 2, &sn, 1);
        memcpy(sn.vp, cl.vp, sizeof sn.vp); CHECK_EQ(vkn_eo_snap_prep(&sn), 0);   // the camera the model is told
        for (int i = 0; i < GW * GH; i++) hz[i] = FLT_MAX;
        if (setup != 2) {   // the character's boxes s..E (the union the runtime keeps while it moves)
            const box a = fc_char(sdraw), b = fc_char(E);
            const box u = B(a.b[0] < b.b[0] ? a.b[0] : b.b[0], a.b[1], a.b[2] < b.b[2] ? a.b[2] : b.b[2], a.b[3] > b.b[3] ? a.b[3] : b.b[3], a.b[4], a.b[5] > b.b[5] ? a.b[5] : b.b[5]);
            (void)vkn_eo_hz_add(hz, &sn, u.b, cf.pad_px);
        }
        if (setup != 3) { const box cb = fc_cloth_box(ccx, ccz); (void)vkn_eo_hz_add(hz, &sn, cb.b, cf.pad_px); }
        scn[nb] = fc_char(E); scn[nb + 1] = fc_cloth(ccx, ccz, E);   // the truth: the frame's own scene
        for (int j = 0; j < 14; j++) {
            const float *wb = base[1 + (int)rnd(0, (float)nw - 0.01f)].b;
            const float pick = rnd(0, 1);
            float x, z;
            if (pick < 0.45f) { x = rnd(wb[0] - 1, wb[3] + 1); z = wb[5] + rnd(0.2f, 4); }          // behind a house
            else if (pick < 0.85f) { x = ccx + rnd(-2.5f, 2.0f); z = ccz + rnd(0.3f, 2.5f); }       // behind the awning
            else { x = 0.12f * (float)E + rnd(-5, 5); z = 0.08f * (float)E + rnd(-2, 10); }
            const float sx = rnd(0.2f, 0.8f), sy = rnd(0.2f, 1.5f), sz = rnd(0.2f, 0.8f);
            const box ob = B(x, -3, z, x + sx, -3 + sy, z + sz);
            int inside = 0;
            for (int k = 1; k < nb; k++) { const float *q = scn[k].b; if (fc_overlap_xz(ob.b, q) && ob.b[1] < q[4] && ob.b[4] > q[1]) inside = 1; }
            const box cbx = fc_cloth_box(ccx, ccz), chb = fc_char(sdraw), che = fc_char(E);
            if (fc_overlap_xz(ob.b, cbx.b) || fc_overlap_xz(ob.b, chb.b) || fc_overlap_xz(ob.b, che.b)) inside = 1;
            if (inside) continue;
            const int vis = visible(&ce, scn, nb + 2, ob.b);
            o->truly += !vis;
            vkn_eo_view v; vkn_eo_res r; const int e = vkn_eo_view_prep(&v, &sn, ce.vp, hz, &cf);
            const int rr = e ? e : vkn_eo_test(&v, &cf, ob.b, &r);
            if (!vis && rr >= 0 && rr < VKN_EO_R_N) o->why[rr]++;
            if (rr != VKN_EO_HIDDEN) continue;
            o->hidden++; o->moved += v.moved; o->false_hide += vis;
        }
    }
}
static void t_followcam_one(int n);
static void t_followcam(int n) {   // [v2] the card (v1) and the box (v2) under the same follow camera
    for (int b = 0; b < 2; b++) { g_box = b; printf("-- moved camera: %s\n", b ? "the box reprojected (v2)" : "the card reprojected (v1)"); t_followcam_one(n); }
    g_box = 1;
}
static void t_followcam_one(int n) {
    static const char *const nm[4] = {"exact", "label -1", "no hazard of the character", "no hazard of the swaying cloth"};
    for (int turn = -1; turn < 3; turn++) for (int setup = 0; setup < 4; setup++) {   // (turn -1: standing still)
        g_fc_v = turn < 0 ? 0.0 : 1.0;
        const double tr = turn <= 0 ? 0.0 : turn == 1 ? 1.5 : 3.0;
        fc_res r; g_rng = 4242u + (uint32_t)setup * 7u + (uint32_t)turn * 101u; fc_run(n, setup, tr, &r);
        printf("follow cam, %s, turn %.1f deg a frame, %s: truly hidden %d, hidden by the test %d (moved camera %d), FALSE HIDES %d\n", turn < 0 ? "standing" : "walking", tr, nm[setup], r.truly, r.hidden, r.moved, r.false_hide);
        if (getenv("FC_WHY")) { for (int i = 0; i < VKN_EO_R_N; i++) if (r.why[i]) printf(" %s %d,", vkn_eo_reason(i), r.why[i]); printf("\n"); }
        if (setup == 0) { CHECK_EQ(r.false_hide, 0); CHECK(r.moved > 0 || !g_box || turn != 0); CHECK(r.hidden > 0 || turn > 0 || !g_box); }
        if (setup == 3 && turn < 0) CHECK(r.false_hide > 0);   // the reproduction: standing, an animated occluder without its hazard hides visible objects
    }
}

int main(int argc, char **argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 200;
    t_static();
    t_moved();
    t_doorway();
    t_hazards();
    t_dense();
    t_followcam(n / 2);
    t_flip(n / 2);
    t_random(n, 4);
    g_rng = 777u; t_random(n / 2, 16);
    if (g_fail) { printf("vkn_eocc_test: %d of %d checks FAILED\n", g_fail, g_checks); return 1; }
    printf("vkn_eocc_test: %d checks passed\n", g_checks);
    return 0;
}
