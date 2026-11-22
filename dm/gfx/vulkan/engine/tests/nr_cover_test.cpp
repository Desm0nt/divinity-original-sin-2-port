// NR-10b host test: dm/gfx/nr/nr_cover.h (nr_cv_eval, nr_cv_cover) + xbg_spirv_pos.cpp (the position program of a vertex shader).
// g++ -std=c++17 -O2 -Wall -Wextra dm/gfx/vulkan/engine/tests/nr_cover_test.cpp dm/gfx/vulkan/engine/xbg_spirv_pos.cpp
//     dm/gfx/vulkan/engine/xbg_spirv_raster.cpp -o nr_cover_test && ./nr_cover_test
//   (1) geometry: nr_cv_cover against a brute-force oracle - every pixel centre of the proven rect lies inside a non-culled triangle of
//       the draw even when each vertex is moved by up to 1/16 px (sub-pixel snapping) and the viewport clips; random triangles, quads
//       (lists, strips, both windings, bow-ties, non-convex pairs), full and partial viewports, scissors. Mutations of the proof must be
//       caught by the same oracle (margin 0, culling ignored, convexity ignored, viewport clip ignored).
//   (2) superset: wherever nvn_cover (the old proof) says yes, nr_cv_cover says "the whole framebuffer".
//   (3) the analysis on synthetic SPIR-V: the translator's shape (registers in Function variables, z / w constants), a scale + bias by
//       a uniform (fma), refusals (two blocks, ClipDistance, a computed component it cannot follow, an integer input, a storage block).
//       [P10] xbg_spirv_out_prog: the same walk for the Output variable at a Location (a varying: whole-vector store, x / y component
//       stores with z / w falling back to x, ClipDistance irrelevant; refusals: nothing at the location, x not followed, only y written).
// ./nr_cover_test --census <dir with *.xgs> [programs.txt]: every vertex bundle of a pack through the analysis: counts by result, and
//   (programs.txt) the program of each accepted shader for tests/nr_cover_census.py, which re-runs them against tools/spv_interp.py.
#include "../xbg_spirv_pos.h"
#include "../xbg_spirv_raster.h"
#include "../../../nvn/nvn_cover.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <dirent.h>

namespace {
int fails = 0;
void check(bool ok, const char *what) { if (!ok) { ++fails; printf("FAIL: %s\n", what); } }
uint64_t rng_s = 0x9E3779B97F4A7C15ull;
uint32_t rnd() { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return uint32_t(rng_s >> 32); }
double frand() { return rnd() / 4294967296.0; }

// ---- oracle: is pixel centre (px + .5, py + .5) drawn? Triangles in framebuffer px after the viewport transform (double), each vertex
// moved to the worst of 9 positions within +-1/16 px per triangle (a point is "drawn" only if it is inside for every tested shift of
// that triangle), non-culled only, and only inside the viewport rect and the scissor.
struct Draw { float v[NR_CV_MAX_VERTS][4]; int n, prim, cull, front_ccw; float vx, vy, vw, vh; int sx, sy; unsigned sw, sh, W, H; };
bool in_tri(const double t[3][2], double x, double y) {
    const double d = (t[1][0] - t[0][0]) * (t[2][1] - t[0][1]) - (t[1][1] - t[0][1]) * (t[2][0] - t[0][0]);
    if (d == 0) return false;
    double e[3];
    for (int k = 0; k < 3; k++) { const double *a = t[k], *b = t[(k + 1) % 3]; e[k] = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]); }
    return d > 0 ? (e[0] >= 0 && e[1] >= 0 && e[2] >= 0) : (e[0] <= 0 && e[1] <= 0 && e[2] <= 0);
}
bool oracle_pixel(const Draw &d, int px, int py, bool shifts) {
    const double x = px + 0.5, y = py + 0.5;
    const double y0 = d.vh < 0 ? double(d.vy) + d.vh : d.vy, y1 = d.vh < 0 ? d.vy : double(d.vy) + d.vh;
    if (x < d.vx || x > double(d.vx) + d.vw || y < y0 || y > y1) return false;   // clipped to the viewport
    if (px < d.sx || py < d.sy || px >= d.sx + int(d.sw) || py >= d.sy + int(d.sh)) return false;
    const int ntri = d.prim == NR_CV_TRIANGLES ? d.n / 3 : d.n - 2;
    for (int k = 0; k < ntri; k++) {
        int i0 = d.prim == NR_CV_TRIANGLES ? 3 * k : k, i1 = i0 + 1, i2 = i0 + 2;
        if (d.prim == NR_CV_TRIANGLE_STRIP && (k & 1)) std::swap(i1, i2);
        const int id[3] = {i0, i1, i2};
        const double ndc = (double(d.v[i1][0]) - d.v[i0][0]) * (double(d.v[i2][1]) - d.v[i0][1]) - (double(d.v[i1][1]) - d.v[i0][1]) * (double(d.v[i2][0]) - d.v[i0][0]);
        if (ndc == 0) continue;
        const bool front = d.front_ccw ? ndc > 0 : ndc < 0;
        if (d.cull == 3 || (d.cull == 2 && !front) || (d.cull == 1 && front)) continue;
        double t[3][2];
        for (int j = 0; j < 3; j++) { t[j][0] = double(d.vx) + (double(d.v[id[j]][0]) + 1) * 0.5 * d.vw; t[j][1] = double(d.vy) + (double(d.v[id[j]][1]) + 1) * 0.5 * d.vh; }
        bool all = true;
        if (!shifts) all = in_tri(t, x, y);
        else for (int s = 0; s < 27 && all; s++) {   // each vertex shifted independently along a few directions
            double u[3][2]; int q = s;
            for (int j = 0; j < 3; j++) { const int dir = q % 3; q /= 3; const double sh = (dir - 1) / 16.0; u[j][0] = t[j][0] + sh * ((j + s) & 1 ? 1 : -1); u[j][1] = t[j][1] + sh; }
            all = in_tri(u, x, y);
        }
        if (all) return true;
    }
    return false;
}
// The pair case draws the shared edge once by the fill rule; with independent vertex shifts the oracle above would open a crack along
// the diagonal, so for a pixel no single triangle holds under shifts the union is tested with the SAME shift on shared vertices.
bool oracle_union(const Draw &d, int px, int py) {
    if (oracle_pixel(d, px, py, true)) return true;
    const double x = px + 0.5, y = py + 0.5;
    const double y0 = d.vh < 0 ? double(d.vy) + d.vh : d.vy, y1 = d.vh < 0 ? d.vy : double(d.vy) + d.vh;
    if (x < d.vx || x > double(d.vx) + d.vw || y < y0 || y > y1) return false;
    if (px < d.sx || py < d.sy || px >= d.sx + int(d.sw) || py >= d.sy + int(d.sh)) return false;
    const int ntri = d.prim == NR_CV_TRIANGLES ? d.n / 3 : d.n - 2;
    for (int s = 0; s < 9; s++) {   // one shift per VERTEX POSITION (equal clip positions move together)
        bool any = false;
        for (int k = 0; k < ntri && !any; k++) {
            int i0 = d.prim == NR_CV_TRIANGLES ? 3 * k : k, i1 = i0 + 1, i2 = i0 + 2;
            if (d.prim == NR_CV_TRIANGLE_STRIP && (k & 1)) std::swap(i1, i2);
            const int id[3] = {i0, i1, i2};
            const double ndc = (double(d.v[i1][0]) - d.v[i0][0]) * (double(d.v[i2][1]) - d.v[i0][1]) - (double(d.v[i1][1]) - d.v[i0][1]) * (double(d.v[i2][0]) - d.v[i0][0]);
            if (ndc == 0) continue;
            const bool front = d.front_ccw ? ndc > 0 : ndc < 0;
            if (d.cull == 3 || (d.cull == 2 && !front) || (d.cull == 1 && front)) continue;
            double t[3][2];
            for (int j = 0; j < 3; j++) {
                uint32_t hx, hy; memcpy(&hx, &d.v[id[j]][0], 4); memcpy(&hy, &d.v[id[j]][1], 4);
                const unsigned h = (hx * 2654435761u ^ hy * 40503u) + unsigned(s) * 7u;   // a shift that depends on the position only
                t[j][0] = double(d.vx) + (double(d.v[id[j]][0]) + 1) * 0.5 * d.vw + (int(h % 3) - 1) / 16.0;
                t[j][1] = double(d.vy) + (double(d.v[id[j]][1]) + 1) * 0.5 * d.vh + (int(h / 3 % 3) - 1) / 16.0;
            }
            any = in_tri(t, x, y);
        }
        if (!any) return false;
    }
    return true;
}
// 0 = sound (or refused); else the number of uncovered pixels in the proven rect
int unsound(const Draw &d, const nr_cv_rect &r) {
    int bad = 0;
    for (int y = r.y0; y < r.y1; y++) for (int x = r.x0; x < r.x1; x++) if (!oracle_union(d, x, y)) ++bad;
    return bad;
}
int prove(const Draw &d, nr_cv_rect *r) { return nr_cv_cover(d.v, d.n, d.prim, d.cull, d.front_ccw, d.vx, d.vy, d.vw, d.vh, d.sx, d.sy, d.sw, d.sh, d.W, d.H, r); }

// Mutations of the proof (each must be caught by the oracle on the same script).
enum { MUT_NONE, MUT_MARGIN, MUT_CULL, MUT_CONVEX, MUT_VIEWPORT, MUT_N };
int prove_mut(const Draw &d0, nr_cv_rect *r, int mut) {
    Draw d = d0;
    if (mut == MUT_CULL) d.cull = 0;
    if (mut == MUT_VIEWPORT) {   // the triangle test in the true viewport, the rect of a larger one
        int rc = prove(d, r); if (rc != NR_CVG_OK) return rc;
        r->x0 = 0; r->y0 = 0; r->x1 = int(d.W); r->y1 = int(d.H); return rc;
    }
    if (mut == MUT_MARGIN) {   // the rect grown by a pixel on each side (as a proof without margin / with pixel edges would give)
        int rc = prove(d, r); if (rc != NR_CVG_OK) return rc;
        r->x0 -= r->x0 > 0; r->y0 -= r->y0 > 0; r->x1 += r->x1 < int(d.W); r->y1 += r->y1 < int(d.H);
        return rc;
    }
    if (mut == MUT_CONVEX) {   // "any two triangles whose union holds the four corners": no shared edge / convexity
        int rc = prove(d, r); if (rc == NR_CVG_OK) return rc;
        const double m = NR_CV_MARGIN; const int W = int(d.W), H = int(d.H);
        if (!(d.vx == 0 && d.vy + (d.vh < 0 ? d.vh : 0) == 0 && d.vw == W && (d.vh < 0 ? -d.vh : d.vh) == H)) return rc;
        const double cx[2] = {0.5 - m, W - 0.5 + m}, cy[2] = {0.5 - m, H - 0.5 + m};
        const int ntri = d.prim == NR_CV_TRIANGLES ? d.n / 3 : d.n - 2; int in = 0;
        for (int c = 0; c < 4; c++) for (int k = 0; k < ntri; k++) {
            int i0 = d.prim == NR_CV_TRIANGLES ? 3 * k : k; double t[3][2];
            for (int j = 0; j < 3; j++) { t[j][0] = double(d.vx) + (double(d.v[i0 + j][0]) + 1) * 0.5 * d.vw; t[j][1] = double(d.vy) + (double(d.v[i0 + j][1]) + 1) * 0.5 * d.vh; }
            if (in_tri(t, cx[c & 1], cy[c >> 1])) { in |= 1 << c; break; }
        }
        if (in != 15) return rc;
        r->x0 = 0; r->y0 = 0; r->x1 = W; r->y1 = H; return NR_CVG_OK;
    }
    return prove(d, r);
}

Draw random_draw() {
    Draw d{}; d.W = 8 + rnd() % 40; d.H = 8 + rnd() % 24;
    const int kind = rnd() % 10;
    // viewport: full, or a part (dynamic resolution), sometimes fractional / off the edge
    if (rnd() % 3) { d.vx = 0; d.vy = float(d.H); d.vw = float(d.W); d.vh = -float(d.H); }
    else { const float w = float(4 + rnd() % d.W), h = float(4 + rnd() % d.H); d.vx = rnd() % 4 ? 0.f : float(int(rnd() % 9) - 3) + (rnd() % 4 ? 0.f : 0.5f);
           const float y = rnd() % 4 ? 0.f : float(int(rnd() % 9) - 3); d.vw = w; if (rnd() & 1) { d.vy = y + h; d.vh = -h; } else { d.vy = y; d.vh = h; } }
    if (rnd() % 4) { d.sx = 0; d.sy = 0; d.sw = d.W; d.sh = d.H; } else { d.sx = rnd() % 5; d.sy = rnd() % 5; d.sw = 1 + rnd() % d.W; d.sh = 1 + rnd() % d.H; }
    d.cull = rnd() % 5 == 0 ? 1 + rnd() % 3 : 0; d.front_ccw = rnd() & 1;
    auto set = [&](int i, double x, double y) { d.v[i][0] = float(x); d.v[i][1] = float(y); d.v[i][2] = rnd() % 16 ? 0.f : float(frand()); d.v[i][3] = 1.f; };
    auto jit = [&]() { return rnd() % 3 ? 0.0 : (frand() - 0.5) * (rnd() % 2 ? 0.02 : 0.6); };
    if (kind < 3) {   // one big triangle (the usual full-screen triangle, sometimes too small)
        d.prim = NR_CV_TRIANGLES; d.n = 3; const double s = rnd() % 4 ? 3 : 1 + frand() * 2;
        set(0, -1 + jit(), -1 + jit()); set(1, s + jit(), -1 + jit()); set(2, -1 + jit(), s + jit());
        if (rnd() & 1) { float t[4]; memcpy(t, d.v[1], 16); memcpy(d.v[1], d.v[2], 16); memcpy(d.v[2], t, 16); }
    } else if (kind < 6) {   // quad as a strip
        d.prim = NR_CV_TRIANGLE_STRIP; d.n = 4; const double x0 = -1 + jit(), x1 = 1 + jit(), y0 = -1 + jit(), y1 = 1 + jit();
        const double P[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}}; int o[4] = {0, 1, 2, 3};
        if (rnd() % 3 == 0) std::swap(o[rnd() % 4], o[rnd() % 4]);   // sometimes a bow-tie
        for (int i = 0; i < 4; i++) set(i, P[o[i]][0] + (rnd() % 6 ? 0 : jit()), P[o[i]][1]);
    } else if (kind < 9) {   // quad as a list of six (shared vertices repeated)
        d.prim = NR_CV_TRIANGLES; d.n = 6; const double x0 = -1 + jit(), x1 = 1 + jit(), y0 = -1 + jit(), y1 = 1 + jit();
        double P[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        if (rnd() % 5 == 0) { P[2][0] = frand() * 2 - 1; P[2][1] = frand() * 2 - 1; }   // dented: maybe not convex
        static const int L[4][6] = {{0, 1, 2, 0, 2, 3}, {0, 1, 2, 2, 3, 0}, {0, 2, 1, 0, 3, 2}, {0, 1, 3, 1, 2, 3}};
        const int *l = L[rnd() % 4];
        for (int i = 0; i < 6; i++) set(i, P[l[i]][0], P[l[i]][1]);
        if (rnd() % 8 == 0) d.v[rnd() % 6][0] += 1e-3f;   // a "shared" vertex that is not the same: must not count as shared
    } else {   // noise
        d.prim = rnd() & 1 ? NR_CV_TRIANGLES : NR_CV_TRIANGLE_STRIP; d.n = 3 + rnd() % 6;
        for (int i = 0; i < d.n; i++) set(i, frand() * 6 - 3, frand() * 6 - 3);
        if (rnd() % 4 == 0) d.v[rnd() % d.n][3] = 0.5f;
        if (rnd() % 6 == 0) d.v[rnd() % d.n][2] = 1.5f;
    }
    return d;
}

void test_geometry() {
    long proven = 0, full = 0, part = 0, bad = 0, why[NR_CVG_KINDS]{}, old_yes = 0, old_not_new = 0, caught[MUT_N]{}, mproven[MUT_N]{};
    for (int it = 0; it < 400000; it++) {
        const Draw d = random_draw(); nr_cv_rect r{};
        const int rc = prove(d, &r); ++why[rc];
        if (rc == NR_CVG_OK) { ++proven; if (nr_cv_rect_full(r, d.W, d.H)) ++full; else ++part; if (unsound(d, r)) { if (bad++ < 5) printf("  unsound: it %d rect %d,%d..%d,%d of %ux%u n %d prim %d\n", it, r.x0, r.y0, r.x1, r.y1, d.W, d.H, d.n, d.prim); } }
        // (2) the old proof (whole framebuffer, viewport = framebuffer in NVN terms)
        const float vp[4] = {d.vx, d.vh < 0 ? d.vy + d.vh : d.vy, d.vw, d.vh < 0 ? -d.vh : d.vh}; const int sc[4] = {d.sx, d.sy, int(d.sw), int(d.sh)};
        if (d.vh < 0 && nvn_cover(d.v, d.n, d.prim, d.cull, d.front_ccw, 1, vp, 1, sc, long(d.W), long(d.H))) { ++old_yes; if (!(rc == NR_CVG_OK && nr_cv_rect_full(r, d.W, d.H))) ++old_not_new; }
        for (int m = 1; m < MUT_N; m++) { nr_cv_rect q{}; if (prove_mut(d, &q, m) == NR_CVG_OK) { ++mproven[m]; if (unsound(d, q)) ++caught[m]; } }
    }
    printf("geometry: 400000 draws, proven %ld (whole framebuffer %ld, a part %ld), unsound %ld; refusals args %ld prim %ld vertex %ld empty %ld geometry %ld culled %ld\n",
           proven, full, part, bad, why[NR_CVG_ARGS], why[NR_CVG_PRIM], why[NR_CVG_VERTEX], why[NR_CVG_EMPTY], why[NR_CVG_GEOMETRY], why[NR_CVG_CULLED]);
    printf("  old proof (nvn_cover) yes %ld, of them not proven whole by the new one %ld\n", old_yes, old_not_new);
    printf("  mutations caught by the oracle: rect grown by 1 px %ld of %ld, culling ignored %ld of %ld, any two triangles %ld of %ld, viewport clip ignored %ld of %ld\n",
           caught[MUT_MARGIN], mproven[MUT_MARGIN], caught[MUT_CULL], mproven[MUT_CULL], caught[MUT_CONVEX], mproven[MUT_CONVEX], caught[MUT_VIEWPORT], mproven[MUT_VIEWPORT]);
    check(bad == 0, "nr_cv_cover proved a rect with an uncovered pixel");
    check(proven > 20000 && full > 5000 && part > 5000, "the script proves too little to mean anything");
    check(old_yes > 1000 && old_not_new == 0, "nr_cv_cover is not a superset of nvn_cover");
    for (int m = 1; m < MUT_N; m++) check(caught[m] > 0, "a mutation of the proof was not caught");
    // fixed cases
    auto fs = [](Draw &d, unsigned W, unsigned H) { d = Draw{}; d.W = W; d.H = H; d.vx = 0; d.vy = float(H); d.vw = float(W); d.vh = -float(H); d.sx = d.sy = 0; d.sw = W; d.sh = H; d.front_ccw = 1; };
    Draw d; nr_cv_rect r{};
    fs(d, 2160, 972); d.prim = NR_CV_TRIANGLE_STRIP; d.n = 4;
    const float q[4][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1}, {1, 1, 0, 1}}; memcpy(d.v, q, sizeof q);
    check(prove(d, &r) == NR_CVG_OK && nr_cv_rect_full(r, 2160, 972), "full-screen strip quad, z = 0");
    d.vw = 1620; d.vy = 729; d.vh = -729;   // dynamic resolution 75 %: the viewport is a part of the target
    check(prove(d, &r) == NR_CVG_OK && r.x0 == 0 && r.y0 == 0 && r.x1 == 1620 && r.y1 == 729, "75 % viewport: the proven rect is the viewport");
    d.sw = 1000; check(prove(d, &r) == NR_CVG_OK && r.x1 == 1000 && r.y1 == 729, "scissor cuts the proven rect");
    fs(d, 2160, 972); d.prim = NR_CV_TRIANGLE_STRIP; d.n = 4; memcpy(d.v, q, sizeof q); d.cull = NR_CV_CULL_BACK; d.front_ccw = 0;
    check(prove(d, &r) == NR_CVG_CULLED, "a culled quad proves nothing (reason: culled)");
    fs(d, 2160, 972); d.prim = NR_CV_TRIANGLE_STRIP; d.n = 4; memcpy(d.v, q, sizeof q); d.v[3][0] = 0.999f;   // 1 px short at the right edge
    check(prove(d, &r) == NR_CVG_GEOMETRY, "a quad one pixel short of the edge");
    fs(d, 2160, 972); d.prim = NR_CV_TRIANGLE_STRIP; d.n = 4; memcpy(d.v, q, sizeof q); d.v[3][0] = 0.99999994f; d.v[1][0] = 0.99999994f;   // one ulp short: an fma's worth
    check(prove(d, &r) == NR_CVG_OK && nr_cv_rect_full(r, 2160, 972), "a quad one ulp short of the edge still covers every pixel centre");
    fs(d, 2160, 972); d.prim = NR_CV_TRIANGLE_STRIP; d.n = 4; memcpy(d.v, q, sizeof q); d.v[2][3] = 0.999f;
    check(prove(d, &r) == NR_CVG_VERTEX, "w != 1");
    fs(d, 64, 64); d.prim = NR_CV_TRIANGLES; d.n = 3; const float t3[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}; memcpy(d.v, t3, sizeof t3);
    check(prove(d, &r) == NR_CVG_OK && nr_cv_rect_full(r, 64, 64), "the full-screen triangle");
}

// ---- synthetic SPIR-V ----
using Module = std::vector<uint32_t>;
struct B {
    Module w; uint32_t next = 1;
    uint32_t id() { return next++; }
    void op(uint32_t o, std::initializer_list<uint32_t> a) { w.push_back(uint32_t((a.size() + 1) << 16) | o); for (uint32_t x : a) w.push_back(x); }
};
enum { V_TRANSLATOR, V_SCALE_BIAS, V_TWO_BLOCKS, V_CLIP, V_UNKNOWN_OP, V_INT_INPUT, V_STORAGE_BLOCK, V_VEC_STORE, V_PTR_COPY,
       V_OUT_VEC, V_OUT_COMP, V_OUT_CLIP, V_OUT_UNKNOWN, V_OUT_Y_ONLY };   // [P10] a varying at Location 1 (xbg_spirv_out_prog)
Module vertex(int variant) {
    B b; b.w = {0x07230203u, 0x00010000u, 0, 0, 0};
    const uint32_t glsl = b.id(), tvoid = b.id(), tfn = b.id(), tf = b.id(), tv4 = b.id(), ti = b.id(), tu = b.id(), tarr1 = b.id(), tpv = b.id(), tarr = b.id(), tblk = b.id(),
        p_out_pv = b.id(), p_in_v4 = b.id(), p_in_f = b.id(), p_out_f = b.id(), p_fn_f = b.id(), p_uni_blk = b.id(), p_uni_f = b.id(), p_out_v4 = b.id(), tiv4 = b.id(), p_in_iv4 = b.id(),
        c0 = b.id(), c1 = b.id(), c2 = b.id(), c3 = b.id(), f0 = b.id(), f1 = b.id(), u1 = b.id(), u16 = b.id(), pv = b.id(), in0 = b.id(), in4 = b.id(), ubo = b.id(), main = b.id(), iin = b.id(), ovar = b.id(), fh = b.id();
    b.op(11, {glsl, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0});          // "GLSL.std.450"
    b.op(71, {in0, 30, 0}); b.op(71, {in4, 30, 4}); b.op(71, {iin, 30, 2}); b.op(71, {ovar, 30, 1});
    b.op(72, {tpv, 0, 11, 0}); b.op(72, {tpv, 1, 11, 1}); b.op(72, {tpv, 2, 11, 3}); b.op(71, {tpv, 2});
    b.op(71, {tarr, 6, 16}); b.op(72, {tblk, 0, 35, 0}); b.op(71, {tblk, variant == V_STORAGE_BLOCK ? 3u : 2u}); b.op(71, {ubo, 34, 0}); b.op(71, {ubo, 33, 6});
    b.op(19, {tvoid}); b.op(33, {tfn, tvoid}); b.op(22, {tf, 32}); b.op(23, {tv4, tf, 4}); b.op(21, {ti, 32, 1}); b.op(21, {tu, 32, 0});
    b.op(43, {tu, u1, 1}); b.op(43, {tu, u16, 16});
    b.op(28, {tarr1, tf, u1}); b.op(30, {tpv, tv4, tf, tarr1}); b.op(28, {tarr, tv4, u16}); b.op(30, {tblk, tarr});
    b.op(32, {p_out_pv, 3, tpv}); b.op(32, {p_in_v4, 1, tv4}); b.op(32, {p_in_f, 1, tf}); b.op(32, {p_out_f, 3, tf}); b.op(32, {p_fn_f, 7, tf});
    b.op(32, {p_uni_blk, 2, tblk}); b.op(32, {p_uni_f, 2, tf}); b.op(32, {p_out_v4, 3, tv4}); b.op(23, {tiv4, ti, 4}); b.op(32, {p_in_iv4, 1, tiv4});
    b.op(43, {ti, c0, 0}); b.op(43, {ti, c1, 1}); b.op(43, {ti, c2, 2}); b.op(43, {ti, c3, 3}); b.op(43, {tf, f0, 0}); b.op(43, {tf, f1, 0x3f800000u}); b.op(43, {tf, fh, 0x3f000000u});
    b.op(59, {p_out_pv, pv, 3}); b.op(59, {p_in_v4, in0, 1}); b.op(59, {p_in_v4, in4, 1}); b.op(59, {p_uni_blk, ubo, 2}); b.op(59, {p_in_iv4, iin, 1}); b.op(59, {p_out_v4, ovar, 3});
    b.op(54, {tvoid, main, 0, tfn}); b.op(248, {b.id()});
    const uint32_t rx = b.id(), ry = b.id(); b.op(59, {p_fn_f, rx, 7}); b.op(59, {p_fn_f, ry, 7});
    auto out = [&](uint32_t comp) { uint32_t p = b.id(); b.op(65, {p_out_f, p, pv, c0, comp}); return p; };
    auto inp = [&](uint32_t var, uint32_t comp) { uint32_t p = b.id(), v = b.id(); b.op(65, {p_in_f, p, var, comp}); b.op(61, {tf, v, p}); return v; };
    auto uni = [&](uint32_t elem, uint32_t comp) { uint32_t p = b.id(), v = b.id(); b.op(65, {p_uni_f, p, ubo, c0, elem, comp}); b.op(61, {tf, v, p}); return v; };
    // the translator's opening: point size 1, position (0, 0, 0, 1)
    { uint32_t p = b.id(); b.op(65, {p_out_f, p, pv, c1}); b.op(62, {p, f1}); }
    b.op(62, {out(c0), f0}); b.op(62, {out(c1), f0}); b.op(62, {out(c2), f0}); b.op(62, {out(c3), f1});
    b.op(62, {rx, inp(in0, c0)}); b.op(62, {ry, inp(in0, c1)});
    uint32_t x = b.id(), y = b.id(); b.op(61, {tf, x, rx}); b.op(61, {tf, y, ry});
    if (variant == V_SCALE_BIAS) {   // x * c[1].x + c[1].z, y * c[1].y + c[1].w
        uint32_t sx = uni(c1, c0), bx = uni(c1, c2), sy = uni(c1, c1), by = uni(c1, c3), nx = b.id(), ny = b.id();
        b.op(12, {tf, nx, glsl, 50, x, sx, bx}); b.op(12, {tf, ny, glsl, 50, y, sy, by}); x = nx; y = ny;
    }
    if (variant == V_STORAGE_BLOCK) { uint32_t s = uni(c0, c0), nx = b.id(); b.op(133, {tf, nx, x, s}); x = nx; }
    if (variant == V_UNKNOWN_OP) { uint32_t nx = b.id(); b.op(12, {tf, nx, glsl, 13 /* Sin */, x}); x = nx; }
    if (variant == V_INT_INPUT) { uint32_t p = b.id(), v = b.id(), nx = b.id(); b.op(65, {b.id(), p, iin, c0}); b.op(61, {ti, v, p}); b.op(111 /* ConvertSToF */, {tf, nx, v}); x = nx; }
    if (variant == V_VEC_STORE) {   // gl_Position = vec4(x, y, 0, 1) as one store
        uint32_t v = b.id(), p = b.id(); b.op(80, {tv4, v, x, y, f0, f1}); b.op(65, {p_out_v4, p, pv, c0}); b.op(62, {p, v});
    } else if (variant == V_PTR_COPY) {   // the store goes through a copy of the access chain
        uint32_t p = out(c0), q = b.id(); b.op(83, {p_out_f, q, p}); b.op(62, {q, x}); b.op(62, {out(c1), y});
    } else { b.op(62, {out(c0), x}); b.op(62, {out(c1), y}); }
    if (variant >= V_OUT_VEC) {   // [P10] the varying at Location 1: (x * 0.5 + 0.5, y * 0.5[, 0, 1])
        uint32_t u = b.id(), v = b.id(); b.op(12, {tf, u, glsl, 50, x, fh, fh}); b.op(133, {tf, v, y, fh});
        if (variant == V_OUT_UNKNOWN) { uint32_t nu = b.id(); b.op(12, {tf, nu, glsl, 13 /* Sin */, u}); u = nu; }
        auto oc = [&](uint32_t comp) { uint32_t p = b.id(); b.op(65, {p_out_f, p, ovar, comp}); return p; };
        if (variant == V_OUT_VEC) { uint32_t w = b.id(); b.op(80, {tv4, w, u, v, f0, f1}); b.op(62, {ovar, w}); }
        else { if (variant != V_OUT_Y_ONLY) b.op(62, {oc(c0), u}); b.op(62, {oc(c1), v}); }
    }
    if (variant == V_CLIP || variant == V_OUT_CLIP) { uint32_t p = b.id(); b.op(65, {p_out_f, p, pv, c2, c0}); b.op(62, {p, f0}); }
    if (variant == V_TWO_BLOCKS) { uint32_t l = b.id(); b.op(249, {l}); b.op(248, {l}); }
    b.op(253, {}); b.op(56, {});
    b.w[3] = b.next; return b.w;
}
void test_analysis() {
    nr_cv_prog p; const char *why = "";
    auto run = [&](int v) { Module m = vertex(v); xbg_spirv_pos_prog(m.data(), m.size(), &p, &why); return p.ok != 0; };
    const float in[32][4] = {{0.25f, -0.5f, 9, 9}, {}, {}, {}, {7, 7, 7, 7}};
    float pos[4];
    check(run(V_TRANSLATOR) && p.exact && p.in_mask == 1 && p.nubo == 0, "translator shape: x, y from input 0, z = 0, w = 1, exact");
    check(nr_cv_eval(&p, in, nullptr, nullptr, pos) && pos[0] == 0.25f && pos[1] == -0.5f && pos[2] == 0 && pos[3] == 1, "translator shape evaluates");
    { Module m = vertex(V_TRANSLATOR); XbgRasterFacts f = xbg_spirv_raster_facts(m.data(), m.size(), 0); check(f.pos_input == -1, "the old facts refuse the same module (the reason-0 refusals of the phone)"); }
    check(run(V_SCALE_BIAS) && !p.exact && p.nubo == 1 && p.ubo[0] == 6 && p.ubo_bytes[0] == 32 && p.in_mask == 1, "scale + bias by uniform block 6, 16..32");
    { float u[8] = {0, 0, 0, 0, 2.0f, 4.0f, -1.0f, 1.0f}; const uint8_t *ub[1] = {reinterpret_cast<const uint8_t *>(u)}; size_t un[1] = {32};
      check(nr_cv_eval(&p, in, ub, un, pos) && pos[0] == -0.5f && pos[1] == -1.0f && pos[2] == 0 && pos[3] == 1, "scale + bias evaluates");
      un[0] = 28; check(!nr_cv_eval(&p, in, ub, un, pos), "a uniform range that is too short refuses");
      ub[0] = nullptr; un[0] = 32; check(!nr_cv_eval(&p, in, ub, un, pos), "an unbound uniform block refuses"); }
    check(run(V_VEC_STORE) && p.exact, "whole vec4 store of a constructed vector");
    check(run(V_PTR_COPY) && p.exact, "a store through a copied access chain is followed");
    check(!run(V_TWO_BLOCKS), "two blocks"); check(!run(V_CLIP), "ClipDistance written");
    check(!run(V_UNKNOWN_OP), "a component through an op the walk does not know"); check(!run(V_INT_INPUT), "an integer input");
    check(!run(V_STORAGE_BLOCK), "a BufferBlock (GPU-writable) is not a uniform leaf");
    { Module m = vertex(V_TRANSLATOR); m[0] = 0; xbg_spirv_pos_prog(m.data(), m.size(), &p, &why); check(!p.ok, "bad magic"); }
    // [P10] xbg_spirv_out_prog: the Output variable at Location 1
    auto outp = [&](int v, uint32_t loc) { Module m = vertex(v); xbg_spirv_out_prog(m.data(), m.size(), loc, &p, &why); return p.ok != 0; };
    check(outp(V_OUT_VEC, 1) && p.in_mask == 1 && p.nubo == 0 && !p.exact, "varying: a whole vec4 store");
    check(nr_cv_eval(&p, in, nullptr, nullptr, pos) && pos[0] == 0.625f && pos[1] == -0.25f && pos[2] == 0 && pos[3] == 1, "varying: (x * 0.5 + 0.5, y * 0.5, 0, 1) evaluates");
    check(outp(V_OUT_COMP, 1) && nr_cv_eval(&p, in, nullptr, nullptr, pos) && pos[0] == 0.625f && pos[1] == -0.25f && pos[2] == pos[0] && pos[3] == pos[0],
          "varying: x / y component stores, z / w (never written) fall back to x");
    check(outp(V_OUT_CLIP, 1) && nr_cv_eval(&p, in, nullptr, nullptr, pos) && pos[0] == 0.625f, "varying: a ClipDistance write does not refuse it");
    check(!run(V_OUT_CLIP), "... while the position of the same module is refused");
    check(run(V_OUT_VEC) && p.exact && nr_cv_eval(&p, in, nullptr, nullptr, pos) && pos[0] == 0.25f && pos[1] == -0.5f, "the position of a module with a varying is unchanged");
    check(!outp(V_OUT_VEC, 2) && !outp(V_OUT_VEC, 0) && !outp(V_OUT_VEC, 40), "varying: no Output at the location (an Input's location is not one)");
    check(!outp(V_TRANSLATOR, 1), "varying: declared, never written");
    check(!outp(V_OUT_UNKNOWN, 1), "varying: x through an op the walk does not know");
    check(!outp(V_OUT_Y_ONLY, 1), "varying: only y written");
    check(!outp(V_TWO_BLOCKS, 1), "varying: two blocks");
    { Module m = vertex(V_OUT_VEC); for (size_t cut = 5; cut < m.size(); cut += 5) { xbg_spirv_out_prog(m.data(), cut, 1, &p, &why); } check(true, "varying: truncated modules do not crash"); }
    { Module m = vertex(V_SCALE_BIAS); for (size_t cut = 5; cut < m.size(); cut += 7) { xbg_spirv_pos_prog(m.data(), cut, &p, &why); } check(true, "truncated modules do not crash"); }
}

// ---- census ----
const char *opname[NR_CV_OPS] = {"const", "input", "ubo", "add", "sub", "mul", "div", "neg", "fma", "abs", "min", "max"};
int census(const char *dir, const char *out_path) {
    DIR *d = opendir(dir); if (!d) { printf("cannot open %s\n", dir); return 2; }
    FILE *out = out_path ? fopen(out_path, "w") : nullptr;
    std::map<std::string, long> by_why; long vs = 0, ok = 0, exact = 0, with_ubo = 0, old_ok = 0, files = 0;
    while (dirent *e = readdir(d)) {
        const std::string name = e->d_name; if (name.size() < 5 || name.substr(name.size() - 4) != ".xgs") continue;
        FILE *f = fopen((std::string(dir) + "/" + name).c_str(), "rb"); if (!f) continue;
        std::vector<uint8_t> b; uint8_t buf[65536]; size_t got; while ((got = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + got); fclose(f); ++files;
        if (b.size() < 32) continue;
        uint32_t h[8]; memcpy(h, b.data(), 32);
        if (h[0] != 0x31534758u || h[2] != 0) continue;   // XGS1, stage 0 = vertex
        const size_t o = 32 + size_t(h[3]) * 32 + size_t(h[4]) * 4 + h[5]; if (o + h[6] > b.size()) continue;
        std::vector<uint32_t> w(h[6] / 4); memcpy(w.data(), b.data() + o, w.size() * 4); ++vs;
        nr_cv_prog p; const char *why = ""; xbg_spirv_pos_prog(w.data(), w.size(), &p, &why); ++by_why[why];
        if (xbg_spirv_raster_facts(w.data(), w.size(), 0).pos_input >= 0) ++old_ok;
        if (!p.ok) continue;
        ++ok; exact += p.exact; with_ubo += p.nubo != 0;
        if (out) { fprintf(out, "%s n=%u exact=%u in=%#x out=%u,%u,%u,%u", name.substr(0, name.size() - 4).c_str(), p.n, p.exact, p.in_mask, p.out[0], p.out[1], p.out[2], p.out[3]);
            for (unsigned i = 0; i < p.n; i++) { const nr_cv_ins &x = p.ins[i]; uint32_t kb; memcpy(&kb, &x.k, 4); fprintf(out, " %s:%u:%u:%u:%08x", opname[x.op], x.a, x.b, x.c, kb); }
            fputc('\n', out); }
    }
    closedir(d); if (out) fclose(out);
    printf("census %s: %ld bundles, %ld vertex; position program %ld (exact %ld, with a uniform %ld); the old facts accept %ld\n", dir, files, vs, ok, exact, with_ubo, old_ok);
    for (auto &k : by_why) printf("  %7ld  %s\n", k.second, k.first.c_str());
    return 0;
}
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--census")) return census(argv[2], argc >= 4 ? argv[3] : nullptr);
    test_geometry(); test_analysis();
    printf(fails ? "nr_cover_test: %d FAILED\n" : "nr_cover_test: all passed\n", fails);
    return fails ? 1 : 0;
}
