// [DOS2] VK-NATIVE stage 5 technique Z host test: the pure model dm/gfx/vkn/vkn_occz_model.c (rule and proof: vkn_occz.h,
// docs/dos2/nr/VK-OCCLUSION-CHEAP.md section 3). Scenes of a few boxes over a synthetic snapshot:
//   project / cells   the box projection (in front, behind the camera, bad box), NDC -> cells with the pad, off screen;
//   hidden            a stable occluder in front: hidden; the would-skip counts; the decision log;
//   hazards           the occluder's content / box changed, vanished, came back; a changed object BEHIND the occludee is no hazard;
//                     the occludee's own older, nearer depth in the snapshot (moved away) is a hazard of its own; an unknown rect
//                     (behind the camera) of a changed object covers the whole screen; an object born after the snapshot is none;
//   refusals          no snapshot, camera, target, scene (globals), order-sensitive frame, old snapshot, unclean, no box, no object,
//                     off screen, visible (depth) -> poison verdict;
//   self-check        the decisions of frame f re-tested against f's own snapshot: none / a violation;
//   budget            a frame's budget spent: the rest drawn (fake clock).
// Build + run (WSL gcc):
//   gcc -std=c11 -D_GNU_SOURCE -DDM_GAME_DOS2=1 -O1 -g -Wall -Wextra -o /tmp/vkn_occz_test dm/gfx/vulkan/engine/tests/vkn_occz_test.c dm/gfx/vkn/vkn_occz_model.c -lm && /tmp/vkn_occz_test
// Ends with "vkn_occz_test: N checks passed" (exit 0) or the failures and exit 1.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../../vkn/vkn_occz.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_fail++; printf("FAIL %s:%d %s = %lld, want %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

enum { W = 320, H = 160, GW = W / VKN_OZ_CELL, GH = H / VKN_OZ_CELL };
static float g_cell[GW * GH];
static vkn_oz_snap snap_at(uint64_t frame) { vkn_oz_snap s = {frame, 0xca, 0x7e, W, H, GW, GH, g_cell, NULL, NULL}; return s; }
static void fill(float d) { for (int i = 0; i < GW * GH; i++) g_cell[i] = d; }
// pixel rect -> NDC (x0, y0, x1, y1)
static void ndc_px(float o[4], float x0, float y0, float x1, float y1) { o[0] = x0 / W * 2 - 1; o[1] = y0 / H * 2 - 1; o[2] = x1 / W * 2 - 1; o[3] = y1 / H * 2 - 1; }
static void cells_set(int cx0, int cy0, int cx1, int cy1, float d) { for (int y = cy0; y <= cy1; y++) for (int x = cx0; x <= cx1; x++) g_cell[y * GW + x] = d; }
static vkn_oz_obj obj(uint64_t key, uint64_t o, float x0, float y0, float x1, float y1, float zmin, uint64_t sig) {
    vkn_oz_obj b; memset(&b, 0, sizeof b);
    b.key = key; b.obj = o; ndc_px(b.ndc, x0, y0, x1, y1); b.zmin = zmin; b.rect = 1; b.clean = 1; b.box_h = o * 31 + 7; b.sig = sig; b.ndraw = 1; b.ncond = 2;
    return b;
}
static vkn_oz_frame frame_of(uint64_t f, const vkn_oz_obj *o, uint32_t n) { vkn_oz_frame fr = {f, 0xca, 0x7e, 0x9106, W, H, 0, o, n, NULL}; return fr; }
static uint64_t g_clock; static uint64_t fake_now(void) { return g_clock += 1000; }
static uint32_t g_reds; static void red(uint64_t k, float z, float c, uint32_t x, uint32_t y, uint64_t f) { (void)k; (void)z; (void)c; (void)x; (void)y; (void)f; g_reds++; }
static vkn_oz_stats st(void) { vkn_oz_stats s; vkn_ozm_stats_get(&s); return s; }

// Runs frames 1..n of the same objects (scene stable), the snapshot of frame `sf` given from frame sf+1 on.
static void settle(vkn_oz_obj *o, uint32_t n, uint64_t from, uint64_t to, uint8_t *v) {
    for (uint64_t f = from; f <= to; f++) { const vkn_oz_frame fr = frame_of(f, o, n); (void)vkn_ozm_frame(&fr, NULL, v, NULL); }
}

static void t_helpers(void) {
    // identity camera: clip = (x, y, z, 1) -> ndc = (x, y), z / w = z
    float vp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, nd[4], z;
    const float box[6] = {-0.5f, -0.25f, 0.3f, 0.5f, 0.25f, 0.6f};
    CHECK_EQ(vkn_oz_project(box, vp, nd, &z), 0);
    CHECK(nd[0] == -0.5f && nd[1] == -0.25f && nd[2] == 0.5f && nd[3] == 0.25f && z == 0.3f);
    float vb[16]; memcpy(vb, vp, sizeof vb); vb[11] = 1.0f; vb[15] = 0.0f;   // w = z: a corner at z = 0 is on the camera plane
    const float box2[6] = {-1, -1, 0.0f, 1, 1, 1};
    CHECK_EQ(vkn_oz_project(box2, vb, nd, &z), -1);
    const float bad[6] = {1, 0, 0, 0, 1, 1};
    CHECK_EQ(vkn_oz_project(bad, vp, nd, &z), -2);
    uint32_t c[4];
    ndc_px(nd, 40, 20, 60, 50);
    CHECK_EQ(vkn_oz_cells(nd, W, H, GW, GH, 2, c), 0);
    CHECK(c[0] == 2 && c[1] == 1 && c[2] == 3 && c[3] == 3);   // 38..62 px -> cells 2..3; 18..52 -> 1..3
    ndc_px(nd, 400, 20, 500, 50);
    CHECK_EQ(vkn_oz_cells(nd, W, H, GW, GH, 2, c), 1);
    ndc_px(nd, -50, -50, 5, 5);
    CHECK_EQ(vkn_oz_cells(nd, W, H, GW, GH, 2, c), 0);
    CHECK(c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0);
}

static void t_hidden_and_hazards(void) {
    uint8_t v[8];
    vkn_ozm_reset();
    // R: a wall in front over px 0..160 x 0..80 (zmin 0.4); O behind it at px 40..80 x 20..60 (zmin 0.8); X elsewhere, visible
    vkn_oz_obj o[3] = {obj(0x100, 1, 0, 0, 160, 80, 0.4f, 11), obj(0x110, 2, 40, 20, 80, 60, 0.8f, 22), obj(0x120, 3, 200, 100, 240, 140, 0.3f, 33)};
    settle(o, 3, 1, 4, v);
    fill(1.0f); cells_set(0, 0, 9, 4, 0.5f);   // the snapshot of frame 4: the wall's depth 0.5 where it is; background far
    cells_set(12, 6, 15, 8, 0.3f);
    vkn_oz_snap s = snap_at(4);
    vkn_oz_frame fr = frame_of(5, o, 3);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 1);
    CHECK_EQ(v[0], VKN_OZ_POISON); CHECK_EQ(v[1], VKN_OZ_HIDDEN); CHECK_EQ(v[2], VKN_OZ_POISON);   // R and X: trusted, visible
    vkn_oz_stats a = st();
    CHECK_EQ(a.would_pre, 1); CHECK_EQ(a.would_cond, 2); CHECK_EQ(a.why[VKN_OZ_R_DEPTH], 2);
    // the wall's content changed in frame 6: a hazard nearer than O over O's cells -> drawn
    o[0].sig = 12; fr = frame_of(6, o, 3);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0); CHECK_EQ(v[1], VKN_OZ_DRAW);
    CHECK(st().why[VKN_OZ_R_HAZARD] >= 1);
    // a fresh snapshot after the change (frame 6's): hidden again
    s = snap_at(6); fr = frame_of(7, o, 3);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 1);
    // X changed, but X lies elsewhere: no effect on O
    o[2].sig = 34; fr = frame_of(8, o, 3);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 1);
    // a changed object BEHIND O over the same cells is no hazard for O (it is a hidden candidate itself)
    vkn_oz_obj o4[4]; memcpy(o4, o, sizeof o); o4[3] = obj(0x130, 4, 30, 10, 90, 70, 0.95f, 44);
    settle(o4, 4, 9, 11, v);
    s = snap_at(11); o4[3].sig = 45; fr = frame_of(12, o4, 4);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 2); CHECK_EQ(v[1], VKN_OZ_HIDDEN); CHECK_EQ(v[3], VKN_OZ_HIDDEN);
    // the wall vanishes in frame 13 (present at 12): hazard -> drawn; and it stays a hazard while the snapshot predates the absence
    vkn_oz_obj o2[2] = {o[1], o[2]}; s = snap_at(12); fr = frame_of(13, o2, 2);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0);
    fr = frame_of(14, o2, 2);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0);
    // a snapshot taken after it left (frame 14) does not see it: depth decides (the cells are far now)
    fill(1.0f); s = snap_at(14); fr = frame_of(15, o2, 2);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0); CHECK_EQ(v[0], VKN_OZ_POISON);
}

static void t_own_depth_and_unknown(void) {
    uint8_t v[4];
    vkn_ozm_reset();
    // O was in front (zmin 0.45, its own depth 0.5 in the snapshot), then moves back to zmin 0.8 with the same rect
    vkn_oz_obj o[1] = {obj(0x200, 9, 40, 20, 80, 60, 0.45f, 5)};
    settle(o, 1, 1, 3, v);
    fill(1.0f); cells_set(2, 1, 5, 3, 0.5f);
    vkn_oz_snap s = snap_at(3);
    o[0].zmin = 0.8f; o[0].box_h = 777; vkn_oz_frame fr = frame_of(4, o, 1);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0); CHECK_EQ(v[0], VKN_OZ_DRAW);   // its own older, nearer depth is not an occluder
    // an object without a known rect (behind the camera) that changed: the whole screen is a hazard
    vkn_ozm_reset();
    vkn_oz_obj p[2] = {obj(0x300, 1, 0, 0, 320, 160, 0.4f, 1), obj(0x310, 2, 40, 20, 80, 60, 0.8f, 2)};
    p[0].rect = 0;
    settle(p, 2, 1, 3, v);
    fill(0.5f); s = snap_at(3);
    fr = frame_of(4, p, 2);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 1);   // stable: fine
    p[0].sig = 99; fr = frame_of(5, p, 2);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0);
    CHECK(st().hazard_whole >= 1);
    // an object born after the snapshot is no hazard (it was not in it)
    vkn_ozm_reset();
    vkn_oz_obj q[2] = {obj(0x400, 1, 0, 0, 160, 80, 0.4f, 1), obj(0x410, 2, 40, 20, 80, 60, 0.8f, 2)};
    settle(q, 2, 1, 3, v);
    fill(1.0f); cells_set(0, 0, 9, 4, 0.5f); s = snap_at(3);
    vkn_oz_obj q3[3] = {q[0], q[1], obj(0x420, 3, 30, 10, 90, 70, 0.2f, 3)};
    fr = frame_of(4, q3, 3);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 1); CHECK_EQ(v[1], VKN_OZ_HIDDEN);
    q3[2].sig = 4; fr = frame_of(5, q3, 3);   // born at 4 > 3, changed at 5: still not in the snapshot
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 1);
}

static void t_refusals(void) {
    uint8_t v[8];
    vkn_ozm_reset();
    vkn_oz_obj o[2] = {obj(0x500, 1, 0, 0, 160, 80, 0.4f, 1), obj(0x510, 2, 40, 20, 80, 60, 0.8f, 2)};
    settle(o, 2, 1, 3, v);
    fill(1.0f); cells_set(0, 0, 9, 4, 0.5f);
    vkn_oz_snap s = snap_at(3);
    vkn_oz_frame fr = frame_of(4, o, 2);
    const uint64_t ns0 = st().why[VKN_OZ_R_NOSNAP];
    CHECK_EQ(vkn_ozm_frame(&fr, NULL, v, NULL), 0); CHECK(st().why[VKN_OZ_R_NOSNAP] == ns0 + 2);
    fr = frame_of(5, o, 2); fr.cam = 0xcb;
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0); CHECK(st().why[VKN_OZ_R_CAMERA] == 2);
    fr = frame_of(6, o, 2); fr.w = 640;
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0); CHECK(st().why[VKN_OZ_R_TARGET] == 2);
    fr = frame_of(7, o, 2); fr.global = 1;   // a draw outside pairs on the pre-pass target changed
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0); CHECK(st().why[VKN_OZ_R_SCENE] == 2);
    s = snap_at(7);
    fr = frame_of(8, o, 2); fr.global = 1;
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 1);   // (a snapshot after the change: fine again)
    fr = frame_of(9, o, 2); fr.global = 1; fr.order_sensitive = 1;
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0);
    fr = frame_of(10, o, 2); fr.global = 1;
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0);   // frame 9 lies between: still refused
    s = snap_at(10); fr = frame_of(18, o, 2); fr.global = 1;
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0); CHECK(st().why[VKN_OZ_R_OLD] == 2);
    // per pair
    settle(o, 2, 19, 20, v); for (uint64_t f = 19; f <= 20; f++) { fr = frame_of(f, o, 2); fr.global = 1; (void)vkn_ozm_frame(&fr, NULL, v, NULL); }
    s = snap_at(20);
    vkn_oz_obj u[5] = {o[0], o[1], o[1], o[1], o[1]};
    u[1].clean = 0; u[2].rect = 0; u[3].obj = 0; ndc_px(u[4].ndc, 400, 10, 420, 20); u[4].key = 0x599;
    fr = frame_of(21, u, 5); fr.global = 1;
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0);
    vkn_oz_stats a = st();
    CHECK(a.why[VKN_OZ_R_UNCLEAN] == 1 && a.why[VKN_OZ_R_NORECT] == 1 && a.why[VKN_OZ_R_NOOBJ] == 1 && a.why[VKN_OZ_R_OFFSCREEN] == 1);
    // the pad: O's rect ends 1 px before a far cell; pad 2 reaches it -> visible; pad 0 -> hidden
    vkn_ozm_reset();
    vkn_oz_obj e[2] = {obj(0x600, 1, 0, 0, 160, 80, 0.4f, 1), obj(0x610, 2, 40, 20, 63, 60, 0.8f, 2)};
    settle(e, 2, 1, 3, v);
    fill(1.0f); cells_set(0, 0, 3, 4, 0.5f); s = snap_at(3);   // cell 4 (64..79 px) far
    fr = frame_of(4, e, 2);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0);
    vkn_oz_cfg c = {1e-5f, 0, UINT64_MAX, 0.05f}; vkn_ozm_cfg(&c);
    fr = frame_of(5, e, 2);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 1);
    vkn_oz_cfg c2 = {1e-5f, 2, UINT64_MAX, 0.05f}; vkn_ozm_cfg(&c2);
    // eps: a cell exactly at zmin - eps / 2 is not "in front"
    cells_set(0, 0, 9, 4, 0.8f - 0.5e-5f); fr = frame_of(6, e, 2); e[1].ndc[2] = e[1].ndc[0];
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 0);
}

static void t_check_and_budget(void) {
    uint8_t v[64];
    vkn_ozm_reset();
    vkn_oz_obj o[2] = {obj(0x700, 1, 0, 0, 160, 80, 0.4f, 1), obj(0x710, 2, 40, 20, 80, 60, 0.8f, 2)};
    settle(o, 2, 1, 3, v);
    fill(1.0f); cells_set(0, 0, 9, 4, 0.5f);
    vkn_oz_snap s = snap_at(3);
    vkn_oz_frame fr = frame_of(4, o, 2);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 1);
    // frame 4's own snapshot agrees: no violation
    vkn_oz_snap s4 = snap_at(4); g_reds = 0;
    CHECK_EQ(vkn_ozm_check(&s4, red), 0); CHECK_EQ(st().checked, 1);
    fr = frame_of(5, o, 2);
    CHECK_EQ(vkn_ozm_frame(&fr, &s, v, NULL), 1);
    cells_set(3, 2, 3, 2, 0.95f);   // something the model could not see opened a hole in frame 5
    vkn_oz_snap s5 = snap_at(5);
    CHECK_EQ(vkn_ozm_check(&s5, red), 1); CHECK_EQ(g_reds, 1); CHECK_EQ(st().violations, 1);
    CHECK_EQ(vkn_ozm_check(&s5, red), 0);   // (once)
    // budget: 63 hidden candidates and a visible one, a 0.5 us budget on a clock that ticks 1 us a call
    vkn_ozm_reset();
    vkn_oz_obj b[64]; b[0] = obj(0x800, 1, 0, 0, 320, 160, 0.3f, 1);
    for (int i = 1; i < 64; i++) b[i] = obj(0x800 + 16u * (uint32_t)i, 1 + (uint64_t)i, 40, 20, 80, 60, 0.8f, 2);
    settle(b, 64, 1, 3, v);
    fill(0.35f); s = snap_at(3);
    vkn_oz_cfg c = {1e-5f, 2, 500, 0.05f}; vkn_ozm_cfg(&c);   // (the clock ticks 1 us a read: spent at the 32nd candidate)
    fr = frame_of(4, b, 64); g_clock = 0;
    const uint32_t h = vkn_ozm_frame(&fr, &s, v, fake_now);
    CHECK(h > 0 && h < 63); CHECK(st().why[VKN_OZ_R_BUDGET] > 0);
    CHECK_EQ(st().why[VKN_OZ_R_BUDGET] + st().hidden + st().why[VKN_OZ_R_DEPTH], 64);
    vkn_oz_cfg c2 = {1e-5f, 2, UINT64_MAX, 0.05f}; vkn_ozm_cfg(&c2);
}

// ================================================================ [v2] a moving camera (the reprojection)
// Perspective cameras (eye, yaw about y; Vulkan depth 0..1; row vectors as the engine's) over a ray-cast scene of rectangles of constant
// world z. The snapshot is made the way the GPU makes it: every pixel centre, max / min per 16 x 16 cell. The truth of a decision is the
// self-check itself against the frame's own depth without the object (what DM_VK_NATIVE_COND=12 would see), plus the object's own pixels.
typedef struct cam { double e[3], yaw; } cam;
static const double k_f = 1.5, k_n = 0.5, k_far = 200.0;
static void cam_vp(const cam *c, float vp[16]) {
    const double cs = cos(c->yaw), sn = sin(c->yaw), a = k_far / (k_far - k_n), b = -k_n * k_far / (k_far - k_n);
    const double vz0 = -(sn * c->e[0] + cs * c->e[2]);   // view depth = sn x + cs z + vz0
    const double m[16] = {k_f * cs, 0, a * sn, sn,   0, k_f, 0, 0,   -k_f * sn, 0, a * cs, cs,
                          -k_f * (cs * c->e[0] - sn * c->e[2]), -k_f * c->e[1], b + a * vz0, vz0};
    for (int i = 0; i < 16; i++) vp[i] = (float)m[i];
}
typedef struct rct { double z, x0, x1, y0, y1; } rct;
static double zdepth(double vz) { const double a = k_far / (k_far - k_n), b = -k_n * k_far / (k_far - k_n); return a + b / vz; }
// The pre-pass depth of the scene (rects r) seen by camera c: cells' max / min; *opx = pixels where rect `own` (-1 none) is the nearest.
static void render(const cam *c, const rct *r, int nr, int own, float *mx, float *mn, int *opx) {
    const double cs = cos(c->yaw), sn = sin(c->yaw);
    for (int i = 0; i < GW * GH; i++) { mx[i] = 0.0f; mn[i] = 1.0f; }
    if (opx) *opx = 0;
    for (int py = 0; py < H; py++) for (int px = 0; px < W; px++) {
        const double dvx = ((px + 0.5) / W * 2 - 1) / k_f, dvy = ((py + 0.5) / H * 2 - 1) / k_f;   // view direction (dvx, dvy, 1): t = view depth
        const double dx = cs * dvx + sn, dy = dvy, dz = -sn * dvx + cs;
        double best = k_far; int who = -1;
        for (int k = 0; k < nr; k++) {
            if (fabs(dz) < 1e-12) continue;
            const double t = (r[k].z - c->e[2]) / dz;
            if (!(t > k_n) || t >= best) continue;
            const double x = c->e[0] + t * dx, y = c->e[1] + t * dy;
            if (x >= r[k].x0 && x <= r[k].x1 && y >= r[k].y0 && y <= r[k].y1) { best = t; who = k; }
        }
        const float d = best >= k_far ? 1.0f : (float)zdepth(best);
        const int ci = (py / 16) * GW + px / 16;
        if (d > mx[ci]) mx[ci] = d;
        if (d < mn[ci]) mn[ci] = d;
        if (opx && who == own && own >= 0) (*opx)++;
    }
}
static vkn_oz_obj obj_box(uint64_t key, uint64_t o, const double *b, const float *vp, uint64_t sig) {
    vkn_oz_obj r; memset(&r, 0, sizeof r);
    float bx[6]; for (int i = 0; i < 6; i++) bx[i] = (float)b[i];
    r.key = key; r.obj = o; r.rect = vkn_oz_project(bx, vp, r.ndc, &r.zmin) == 0; r.clean = 1;
    r.box_h = o * 31 + 7; r.sig = sig; r.sig_full = sig; r.ndraw = 1; r.ncond = 2;
    return r;
}
static float g_mx[GW * GH], g_mn[GW * GH], g_fx[GW * GH], g_fn[GW * GH];
typedef struct v2case {
    cam s, f; rct occ[4]; int nocc; double box[6];
    uint64_t occ_sig_f, occ_full_f;   // the first occluder's signatures in the decided frame (0 = as before)
} v2case;
typedef struct v2out { uint8_t v; int viol, opx; vkn_oz_stats d; } v2out;
// frames 1..3 under camera s (the occluders, each a pair, + O), the snapshot of frame 3 (O drawn), frame 4 under camera f decided; then the
// self-check of frame 4 against its own depth without O (as mode 12 draws it) and O's pixels at f (the truth).
static void v2_run(const v2case *c, v2out *out) {
    vkn_ozm_reset();
    float vs[16], vf[16]; cam_vp(&c->s, vs); cam_vp(&c->f, vf);
    rct sc[6]; int ns = 0;
    for (int i = 0; i < c->nocc; i++) sc[ns++] = c->occ[i];
    const rct of = {c->box[2], c->box[0], c->box[3], c->box[1], c->box[4]};   // O's front face (its nearest side to the cameras: they look +z)
    sc[ns] = of;
    vkn_oz_obj o[6]; uint8_t v[6];
    const int same = !memcmp(&c->s, &c->f, sizeof c->s);
    for (uint64_t f = 1; f <= 4; f++) {
        const float *vp = f < 4 ? vs : vf;
        for (int i = 0; i < c->nocc; i++) {
            const double b[6] = {c->occ[i].x0, c->occ[i].y0, c->occ[i].z, c->occ[i].x1, c->occ[i].y1, c->occ[i].z + 0.01};
            o[i] = obj_box(0x1000 + 16u * (uint64_t)i, 100 + (uint64_t)i, b, vp, 1000 + (uint64_t)i);
            o[i].clean = 0;   // (occluders are no candidates here: their verdicts do not matter)
        }
        if (f == 4 && c->occ_sig_f) o[0].sig = c->occ_sig_f;
        if (f == 4 && c->occ_full_f) o[0].sig_full = c->occ_full_f;
        o[c->nocc] = obj_box(0x2000, 200, c->box, vp, 2000);
        vkn_oz_frame fr = frame_of(f, o, (uint32_t)c->nocc + 1); fr.vp = vp;
        if (f == 4 && !same) fr.cam = 0xcb;
        vkn_oz_snap sn = snap_at(3); sn.cell = g_mx; sn.cmin = g_mn; sn.vp = vs;
        if (f == 4) { render(&c->s, sc, ns + 1, -1, g_mx, g_mn, NULL); vkn_oz_stats a = st(); (void)vkn_ozm_frame(&fr, &sn, v, NULL); out->d = st(); for (int k = 0; k < VKN_OZ_R_N; k++) out->d.why[k] -= a.why[k]; out->d.moved_hidden -= a.moved_hidden; out->d.moved_frames -= a.moved_frames; out->d.changed_cam_only -= a.changed_cam_only; }
        else (void)vkn_ozm_frame(&fr, NULL, v, NULL);
    }
    out->v = v[c->nocc];
    render(&c->f, sc, ns, -1, g_fx, g_fn, NULL);   // frame 4's own depth without O
    vkn_oz_snap s4 = snap_at(4); s4.cell = g_fx;
    out->viol = (int)vkn_ozm_check(&s4, NULL);
    render(&c->f, sc, ns + 1, ns, g_fx, g_fn, &out->opx);   // O's pixels at f, had it been drawn
}
static const rct k_wall = {0.0, -5.0, 5.0, -3.5, 3.5}, k_far_wall = {30.0, -60.0, 60.0, -40.0, 40.0}, k_pillar = {-2.0, 0.4, 1.6, -3.0, 3.0};
static v2case base_case(void) {
    v2case c; memset(&c, 0, sizeof c);
    c.s.e[2] = -10.0; c.f = c.s;
    c.occ[0] = k_wall; c.occ[1] = k_far_wall; c.nocc = 2;
    const double b[6] = {-0.5, -0.5, 2.0, 0.5, 0.5, 3.0}; memcpy(c.box, b, sizeof b);
    return c;
}
static void t_v2_cases(void) {
    v2out r;
    // helpers: the eye and the linear depth of a perspective camera
    { cam c = {{0.7, -0.2, -9.0}, 0.05}; float vp[16]; cam_vp(&c, vp); double e[3], a, b;
      CHECK_EQ(vkn_oz_eye(vp, e), 0); CHECK(fabs(e[0] - 0.7) < 1e-4 && fabs(e[1] + 0.2) < 1e-4 && fabs(e[2] + 9.0) < 1e-4);
      CHECK_EQ(vkn_oz_lin(vp, &a, &b), 0); CHECK(fabs(b / (zdepth(12.5) - a) - 12.5) < 1e-3);
      const float ortho[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; CHECK_EQ(vkn_oz_eye(ortho, e), -1); CHECK_EQ(vkn_oz_lin(ortho, &a, &b), -1); }
    // the camera as it was (v1 path): hidden
    v2case c = base_case(); v2_run(&c, &r);
    CHECK_EQ(r.v, VKN_OZ_HIDDEN); CHECK_EQ(r.viol, 0); CHECK_EQ(r.opx, 0); CHECK_EQ(r.d.moved_frames, 0);
    // a small move of the eye sideways / up: hidden through the reprojection, the self-check agrees
    c = base_case(); c.f.e[0] = 0.3; c.f.e[1] = 0.1; v2_run(&c, &r);
    CHECK_EQ(r.v, VKN_OZ_HIDDEN); CHECK_EQ(r.viol, 0); CHECK_EQ(r.opx, 0); CHECK_EQ(r.d.moved_frames, 1); CHECK_EQ(r.d.moved_hidden, 1);
    // forward / back
    c = base_case(); c.f.e[2] = -9.5; v2_run(&c, &r); CHECK_EQ(r.v, VKN_OZ_HIDDEN); CHECK_EQ(r.viol, 0);
    c = base_case(); c.f.e[2] = -10.6; v2_run(&c, &r); CHECK_EQ(r.v, VKN_OZ_HIDDEN); CHECK_EQ(r.viol, 0);
    // a turn only (the same eye)
    c = base_case(); c.f.yaw = 0.03; v2_run(&c, &r); CHECK_EQ(r.v, VKN_OZ_HIDDEN); CHECK_EQ(r.viol, 0); CHECK_EQ(r.d.moved_frames, 1);
    // behind the wall's edge: from the moved eye O shows past the edge -> never hidden (the swept region crosses the edge: no sheet)
    c = base_case(); { const double b[6] = {4.2, -0.3, 6.0, 4.9, 0.3, 7.0}; memcpy(c.box, b, sizeof b); } c.f.e[0] = 7.0; v2_run(&c, &r);
    CHECK(r.opx > 0); CHECK(r.v != VKN_OZ_HIDDEN);
    CHECK(r.d.why[VKN_OZ_R_SHEET] + r.d.why[VKN_OZ_R_REPROJ] + r.d.why[VKN_OZ_R_DEPTH] >= 1);
    // the same box from the snapshot's eye: hidden there (v1) - the move is what makes it visible
    c.f = c.s; v2_run(&c, &r); CHECK_EQ(r.v, VKN_OZ_HIDDEN); CHECK_EQ(r.opx, 0);
    // a gap behind a pillar: the pillar hides O at s, the wall behind the gap does not; a move opens the gap -> not hidden
    c = base_case(); c.occ[2] = k_pillar; c.nocc = 3; c.occ[0].x0 = 2.0;   // the wall now starts at x 2: O behind the pillar only
    { const double b[6] = {0.9, -0.2, 1.0, 1.1, 0.2, 1.5}; memcpy(c.box, b, sizeof b); }
    { v2case h = c; v2_run(&h, &r); CHECK_EQ(r.v, VKN_OZ_HIDDEN); CHECK_EQ(r.opx, 0); }   // (from the snapshot's eye: hidden by the pillar)
    c.f.e[0] = -1.5; v2_run(&c, &r);
    CHECK(r.opx > 0); CHECK(r.v != VKN_OZ_HIDDEN); CHECK_EQ(r.viol, 0);
    // the occluder changed in the decided frame: a hazard (its snapshot depth may be gone)
    c = base_case(); c.f.e[0] = 0.3; c.occ_sig_f = 77; v2_run(&c, &r);
    CHECK_EQ(r.v, VKN_OZ_DRAW); CHECK(r.d.why[VKN_OZ_R_HAZARD] >= 1);
    // its camera bytes alone changed (sig_full): no hazard, counted as "camera only"
    c = base_case(); c.f.e[0] = 0.3; c.occ_full_f = 78; v2_run(&c, &r);
    CHECK_EQ(r.v, VKN_OZ_HIDDEN); CHECK_EQ(r.d.changed_cam_only, 1);
    // big moves: hidden only when still true (never wrong)
    for (int k = -8; k <= 8; k += 2) { c = base_case(); c.f.e[0] = k; c.f.e[1] = 0.5 * k; v2_run(&c, &r); CHECK_EQ(r.viol, 0); if (r.v == VKN_OZ_HIDDEN) CHECK_EQ(r.opx, 0); }
    // reprojection off (sheet 0): a moved camera is refused as in v1
    vkn_oz_cfg off = {1e-5f, 2, UINT64_MAX, 0.0f}; vkn_ozm_cfg(&off);
    c = base_case(); c.f.e[0] = 0.3; v2_run(&c, &r); CHECK_EQ(r.v, VKN_OZ_DRAW); CHECK_EQ(r.d.why[VKN_OZ_R_CAMERA], (uint64_t)c.nocc + 1);
    vkn_oz_cfg on = {1e-5f, 2, UINT64_MAX, 0.05f}; vkn_ozm_cfg(&on);
}
// Random scenes: a wall, a pillar in front, a far wall; small camera moves and turns (and a share of standing frames); random boxes
// behind / beside / in front. Every HIDDEN must pass the frame's own self-check and draw no pixel; enough of them must be hidden.
#ifndef V2_SCENES
#define V2_SCENES 1500   // (-DV2_SCENES=20000 for a long run)
#endif
static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static double rnd(double a, double b) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return a + (b - a) * (double)(g_rng >> 11) / 9007199254740992.0; }
static void t_v2_random(void) {
    int hidden = 0, moved_hidden = 0, bad = 0, wrong_px = 0, n = 0, still = 0;
    for (int it = 0; it < V2_SCENES; it++) {
        v2case c; memset(&c, 0, sizeof c);
        c.s.e[0] = rnd(-1, 1); c.s.e[1] = rnd(-1, 1); c.s.e[2] = rnd(-11, -9); c.s.yaw = rnd(-0.05, 0.05);
        c.f = c.s;
        const double k = rnd(0, 1);
        if (k < 0.15) still++;
        else if (k < 0.35) c.f.yaw += rnd(-0.035, 0.035);
        else { c.f.e[0] += rnd(-0.8, 0.8); c.f.e[1] += rnd(-0.8, 0.8); c.f.e[2] += rnd(-0.5, 0.5); c.f.yaw += rnd(-0.02, 0.02); }
        c.occ[0] = k_wall; c.occ[1] = k_pillar; c.occ[2] = k_far_wall; c.nocc = 3;
        const double cx = rnd(-4, 4), cy = rnd(-2.5, 2.5), hx = rnd(0.1, 1.0), hy = rnd(0.1, 1.0), z0 = rnd(-1.5, 12), dz = rnd(0.2, 2);
        const double b[6] = {cx - hx, cy - hy, z0, cx + hx, cy + hy, z0 + dz}; memcpy(c.box, b, sizeof b);
        v2out r; v2_run(&c, &r); n++;
        if (r.v == VKN_OZ_HIDDEN) { hidden++; moved_hidden += (int)r.d.moved_hidden; if (r.viol) bad++; if (r.opx) wrong_px++; }
    }
    printf("  v2 random: %d scenes (%d standing), hidden %d (under a moved camera %d), self-check violations %d, hidden with pixels %d\n", n, still, hidden, moved_hidden, bad, wrong_px);
    CHECK_EQ(bad, 0); CHECK_EQ(wrong_px, 0);
    CHECK(hidden > 100); CHECK(moved_hidden > 50);
}

int main(void) {
    t_helpers();
    t_hidden_and_hazards();
    t_own_depth_and_unknown();
    t_refusals();
    t_check_and_budget();
    t_v2_cases();
    t_v2_random();
    if (g_fail) { printf("vkn_occz_test: %d of %d checks FAILED\n", g_fail, g_checks); return 1; }
    printf("vkn_occz_test: %d checks passed\n", g_checks);
    return 0;
}
