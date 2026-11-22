// [DOS2] VK-SHADOWS-MOVING: host test of dm/gfx/nr/nr_shs.h (pure C, no Vulkan).
//   wsl gcc -std=c11 -O1 -Wall -o /tmp/nr_shs_test dm/gfx/vulkan/engine/tests/nr_shs_test.c -lm && wsl /tmp/nr_shs_test
// Blocks:
//   prog     nr_sb_degrees / nr_sb_eval on a hand-made "World x ViewProjection" program (the shape of DOS2's static casters, 67 instructions there: 63 here
//            instructions, fma chains): the forms against direct evaluation at random points, the linear part bit-stable under a
//            translation of the projection, the refusals (a product of two inputs, abs of an input, another input location);
//   bounds   nr_sb_px contains every vertex's pixel of random boxes; a draw outside the viewport is recognised;
//   shift    nr_ss_shift (whole texels with a tolerance), nr_ss_exposed / nr_ss_pieces / nr_ss_scroll geometry (every tile pixel in
//            exactly one piece, the pieces inside the layer);
//   reg      nr_ss_reg: the same instance within the tolerance across cell borders, another one beyond it, sweep;
//   scroll   a toy rasteriser: a world of static rectangles (members), a camera that walks by whole texels, the toroidal layer kept by the
//            rules of nr_shs.h section 3 (exposed strips cleared and redrawn by the members that touch them, everything else skipped)
//            against a full render of every member - texel for texel, 400 frames x 6 seeds; members that leave (dropped only when their
//            bounds left the tile), a member removed inside the tile (must be caught), the count of member draws saved.
#include "../../../nr/nr_shs.h"
#include <stdio.h>

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; if (g_fails < 30) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t g_rng = 0x1234567;
static uint32_t rnd(void) { g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(g_rng >> 33); }
static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xffffff) / 16777216.0f; }

// ---- a program builder
typedef struct pb { nr_sb_prog *p; unsigned n; } pb;
static int pb_add(pb *b, uint8_t op, uint16_t a, uint16_t bb, uint16_t c, float k) { nr_cv_ins *x = &b->p->ins[b->n]; memset(x, 0, sizeof *x); x->op = op; x->a = a; x->b = bb; x->c = c; x->k = k; return (int)b->n++; }
// clip = VP(rows at vpw, binding 12) * (World(binding 6) * (pos, 1)), each row dot as an fma chain
static nr_sb_prog *caster_prog(int flavour) {
    pb b; b.p = (nr_sb_prog *)calloc(1, nr_sb_prog_bytes(200)); b.n = 0;
    int in[3], wp[4], cl[4];
    for (int k = 0; k < 3; k++) in[k] = pb_add(&b, NR_CV_INPUT, 0, (uint16_t)k, 0, 0);
    for (int r = 0; r < 4; r++) {
        int m[4]; for (int c = 0; c < 4; c++) m[c] = pb_add(&b, NR_CV_UBO, 6, (uint16_t)(16 * r + 4 * c), 0, 0);
        int acc = pb_add(&b, NR_CV_FMA, (uint16_t)in[0], (uint16_t)m[0], (uint16_t)m[3], 0);
        acc = pb_add(&b, NR_CV_FMA, (uint16_t)in[1], (uint16_t)m[1], (uint16_t)acc, 0);
        wp[r] = pb_add(&b, NR_CV_FMA, (uint16_t)in[2], (uint16_t)m[2], (uint16_t)acc, 0);
    }
    for (int r = 0; r < 4; r++) {
        int m[4]; for (int c = 0; c < 4; c++) m[c] = pb_add(&b, NR_CV_UBO, 12, (uint16_t)(128 + 16 * r + 4 * c), 0, 0);
        int acc = pb_add(&b, NR_CV_MUL, (uint16_t)wp[3], (uint16_t)m[3], 0, 0);
        acc = pb_add(&b, NR_CV_FMA, (uint16_t)wp[0], (uint16_t)m[0], (uint16_t)acc, 0);
        acc = pb_add(&b, NR_CV_FMA, (uint16_t)wp[1], (uint16_t)m[1], (uint16_t)acc, 0);
        cl[r] = pb_add(&b, NR_CV_FMA, (uint16_t)wp[2], (uint16_t)m[2], (uint16_t)acc, 0);
    }
    if (flavour == 1) cl[0] = pb_add(&b, NR_CV_MUL, (uint16_t)cl[0], (uint16_t)in[1], 0, 0);   // a product of two inputs
    if (flavour == 2) cl[1] = pb_add(&b, NR_CV_ABS, (uint16_t)cl[1], 0, 0, 0);               // abs of an input
    if (flavour == 3) { const int o = pb_add(&b, NR_CV_INPUT, 2, 0, 0, 0); cl[2] = pb_add(&b, NR_CV_ADD, (uint16_t)cl[2], (uint16_t)o, 0, 0); b.p->in_mask |= 4; }
    b.p->n = (uint16_t)b.n; b.p->ok = 1; b.p->in_mask |= 1; b.p->nubo = 2; b.p->ubo[0] = 6; b.p->ubo_bytes[0] = 64; b.p->ubo[1] = 12; b.p->ubo_bytes[1] = 192;
    for (int k = 0; k < 4; k++) b.p->out[k] = (uint16_t)cl[k];
    nr_sb_degrees(b.p);
    return b.p;
}
static void mats(float world[16], float pv[48], float tx, float ty, float tz) {
    // world: a rotation about y, a scale and a translation (rows, last column the translation); the last row 0 0 0 1
    const float a = frand(0, 6.28f), s = frand(0.5f, 3.0f), c = cosf(a) * s, sn = sinf(a) * s;
    const float w[16] = {c, 0, sn, frand(-200, 200), 0, s, 0, frand(-20, 20), -sn, 0, c, frand(-200, 200), 0, 0, 0, 1};
    memcpy(world, w, sizeof w);
    memset(pv, 0, 48 * sizeof(float));
    // view-projection (rows 8..11 of the block): a slanted orthographic light
    const float vp[16] = {0.07f, 0.01f, 0.03f, tx, -0.02f, 0.06f, 0.04f, ty, 0.001f, -0.002f, 0.0015f, tz, 0, 0, 0, 1};
    memcpy(pv + 32, vp, sizeof vp);
}
static void direct(const float world[16], const float pv[48], const float p[3], float out[4]) {
    float wp[4];
    for (int r = 0; r < 4; r++) { float acc = p[0] * world[4 * r] + world[4 * r + 3]; acc = p[1] * world[4 * r + 1] + acc; wp[r] = p[2] * world[4 * r + 2] + acc; }
    for (int r = 0; r < 4; r++) { const float *m = pv + 32 + 4 * r; float acc = wp[3] * m[3]; acc = wp[0] * m[0] + acc; acc = wp[1] * m[1] + acc; out[r] = wp[2] * m[2] + acc; }
}
static void test_prog(void) {
    nr_sb_prog *p = caster_prog(0);
    CHECK(p->affine == 1); CHECK(p->n == 63); CHECK(p->in_w == 0);
    for (int it = 0; it < 200; it++) {
        float world[16], pv[48]; mats(world, pv, frand(-30, 30), frand(-30, 30), frand(0, 1));
        const uint8_t *ub[2] = {(const uint8_t *)world, (const uint8_t *)pv}; const size_t un[2] = {64, 192};
        nr_sb_form f[4]; CHECK(nr_sb_eval(p, ub, un, 1.0f, f) == 1);
        CHECK(f[3].c[0] == 1.0f && f[3].c[1] == 0.0f && f[3].c[2] == 0.0f && f[3].c[3] == 0.0f);
        for (int q = 0; q < 8; q++) {
            const float pt[3] = {frand(-10, 10), frand(-10, 10), frand(-10, 10)}; float d[4]; direct(world, pv, pt, d);
            for (int k = 0; k < 3; k++) { const float e = f[k].c[0] + f[k].c[1] * pt[0] + f[k].c[2] * pt[1] + f[k].c[3] * pt[2]; CHECK(fabsf(e - d[k]) <= 2e-4f * (1.0f + fabsf(d[k]))); }
        }
        // a translated projection: the linear coefficients are bit for bit the same, the constant terms move by the translation
        float pv2[48]; memcpy(pv2, pv, sizeof pv2); pv2[35] += 0.3359375f; pv2[39] -= 1.25f; pv2[43] += 0.01f;
        const uint8_t *ub2[2] = {(const uint8_t *)world, (const uint8_t *)pv2}; nr_sb_form g[4]; CHECK(nr_sb_eval(p, ub2, un, 1.0f, g) == 1);
        for (int k = 0; k < 3; k++) CHECK(!memcmp(&f[k].c[1], &g[k].c[1], 12));
        CHECK(fabsf((g[0].c[0] - f[0].c[0]) - 0.3359375f) < 1e-4f); CHECK(fabsf((g[1].c[0] - f[1].c[0]) + 1.25f) < 1e-4f);
        const size_t short_n[2] = {64, 176}; CHECK(nr_sb_eval(p, ub, short_n, 1.0f, f) == 0);   // a block too short
    }
    free(p);
    for (int fl = 1; fl <= 3; fl++) { nr_sb_prog *q = caster_prog(fl); CHECK(q->affine == 0); nr_sb_form f[4]; const uint8_t *ub[2] = {0, 0}; const size_t un[2] = {0, 0}; CHECK(nr_sb_eval(q, ub, un, 1.0f, f) == 0); free(q); }
}
static void test_bounds(void) {
    nr_sb_prog *p = caster_prog(0);
    int outside = 0, inside = 0;
    for (int it = 0; it < 2000; it++) {
        float world[16], pv[48]; mats(world, pv, frand(-30, 30), frand(-30, 30), frand(0.2f, 0.6f));
        const uint8_t *ub[2] = {(const uint8_t *)world, (const uint8_t *)pv}; const size_t un[2] = {64, 192};
        nr_sb_form f[4]; CHECK(nr_sb_eval(p, ub, un, 1.0f, f) == 1);
        nr_sb_box b; for (int k = 0; k < 3; k++) { const float a = frand(-5, 5), c = frand(0, 6); b.lo[k] = a; b.hi[k] = a + c; }
        const float vp[4] = {768, 0, 768, 768}; nr_ss_rect r; float z0, z1;
        CHECK(nr_sb_px(f, &b, vp, &r, &z0, &z1) == 1);
        for (int q = 0; q < 16; q++) {
            float pt[3]; for (int k = 0; k < 3; k++) pt[k] = q < 8 ? ((q >> k & 1) ? b.hi[k] : b.lo[k]) : frand(b.lo[k], b.hi[k]);
            float d[4]; direct(world, pv, pt, d);
            const double px = vp[0] + (d[0] + 1.0) * 0.5 * vp[2], py = vp[1] + (1.0 - d[1]) * 0.5 * vp[3];
            CHECK(px >= r.x0 && px <= r.x1 && py >= r.y0 && py <= r.y1); CHECK(d[2] >= z0 - 1e-5f && d[2] <= z1 + 1e-5f);
        }
        const nr_ss_rect tile = {768, 0, 1536, 768};
        if (nr_ss_isect(&r, &tile)) inside++; else outside++;
    }
    CHECK(outside > 100 && inside + outside == 2000);
    {   // w not exactly 1: no bounds
        float world[16], pv[48]; mats(world, pv, 0, 0, 0.5f); pv[47] = 1.0000001f;
        const uint8_t *ub[2] = {(const uint8_t *)world, (const uint8_t *)pv}; const size_t un[2] = {64, 192};
        nr_sb_form f[4]; CHECK(nr_sb_eval(p, ub, un, 1.0f, f) == 1);
        nr_sb_box b = {{0, 0, 0}, {1, 1, 1}}; const float vp[4] = {0, 0, 768, 768}; nr_ss_rect r; float z0, z1;
        CHECK(nr_sb_px(f, &b, vp, &r, &z0, &z1) == 0);
    }
    free(p);
}
static void test_shift(void) {
    int32_t nx, ny; double err;
    CHECK(nr_ss_shift(10.0 * 2 / 768, -3.0 * 2 / 768, 768, 768, 1.0 / 64, &nx, &ny, &err) == 1); CHECK(nx == 10 && ny == 3);
    CHECK(nr_ss_shift(10.4 * 2 / 768, 0, 768, 768, 1.0 / 64, &nx, &ny, &err) == 0);
    CHECK(nr_ss_shift(10.01 * 2 / 768, 0, 768, 768, 1.0 / 64, &nx, &ny, &err) == 1); CHECK(nx == 10 && err < 0.011);
    CHECK(nr_ss_shift(0.0 / 0.0, 0, 768, 768, 1.0 / 64, &nx, &ny, &err) == 0);
    for (int it = 0; it < 3000; it++) {
        const int32_t W = 8 + (int32_t)(rnd() % 120), H = 8 + (int32_t)(rnd() % 120);
        const int32_t sx = (int32_t)(rnd() % (uint32_t)(2 * W + 9)) - W - 4, sy = (int32_t)(rnd() % (uint32_t)(2 * H + 9)) - H - 4;
        nr_ss_rect e[2]; const int ne = nr_ss_exposed(W, H, sx, sy, e);
        // a tile pixel is exposed exactly when p - n is outside the tile; the rectangles do not overlap
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int in = 0; for (int i = 0; i < ne; i++) in += x >= e[i].x0 && x < e[i].x1 && y >= e[i].y0 && y < e[i].y1;
            const int want = x - sx < 0 || x - sx >= W || y - sy < 0 || y - sy >= H;
            if (in != want) { CHECK(in == want); y = H; break; }
        }
        int32_t O[2] = {(int32_t)(rnd() % (uint32_t)W), (int32_t)(rnd() % (uint32_t)H)};
        const nr_ss_rect r = {(int32_t)(rnd() % (uint32_t)W), (int32_t)(rnd() % (uint32_t)H), 0, 0};
        nr_ss_rect rr = r; rr.x1 = rr.x0 + 1 + (int32_t)(rnd() % (uint32_t)(W - rr.x0)); rr.y1 = rr.y0 + 1 + (int32_t)(rnd() % (uint32_t)(H - rr.y0));
        nr_ss_piece pc[4]; const int np = nr_ss_pieces(W, H, O, &rr, pc);
        CHECK(np >= 1 && np <= 4);
        long area = 0;
        for (int i = 0; i < np; i++) {
            area += (long)(pc[i].r.x1 - pc[i].r.x0) * (pc[i].r.y1 - pc[i].r.y0);
            CHECK(pc[i].r.x0 + pc[i].dx >= 0 && pc[i].r.x1 + pc[i].dx <= W && pc[i].r.y0 + pc[i].dy >= 0 && pc[i].r.y1 + pc[i].dy <= H);
            CHECK(pc[i].r.x0 + pc[i].dx == nr_ss_mod(pc[i].r.x0 + O[0], W) && pc[i].r.y0 + pc[i].dy == nr_ss_mod(pc[i].r.y0 + O[1], H));
            for (int j = 0; j < i; j++) CHECK(!nr_ss_isect(&pc[i].r, &pc[j].r));
        }
        CHECK(area == (long)(rr.x1 - rr.x0) * (rr.y1 - rr.y0));
    }
}
static void test_reg(void) {
    nr_ss_reg *g = (nr_ss_reg *)malloc(sizeof *g); nr_ss_reg_init(g, 1.0 / 64, 4e-6);
    const double u0[3] = {100.999, -5.0001, 0.25};
    const int a = nr_ss_reg_get(g, 77, u0, 1); CHECK(a >= 0);
    const double u1[3] = {101.005, -4.9995, 0.2500019};   // across the cell borders, within the tolerance
    CHECK(nr_ss_reg_get(g, 77, u1, 2) == a); CHECK(g->in[a].seen == 2);
    const double u2[3] = {101.03, -5.0001, 0.25}; const int b = nr_ss_reg_get(g, 77, u2, 2); CHECK(b >= 0 && b != a);
    const double u3[3] = {100.999, -5.0001, 0.25001}; CHECK(nr_ss_reg_get(g, 77, u3, 2) != a);
    CHECK(nr_ss_reg_get(g, 78, u0, 2) != a);
    CHECK(g->in[a].id != g->in[b].id && g->in[a].id != 0);
    int same = 0;
    for (int i = 0; i < 2000; i++) {   // many instances of one structure
        const double u[3] = {(double)(i % 50) * 3.0 + 0.5, (double)(i / 50) * 3.0 + 0.5, 0.5};
        const int x = nr_ss_reg_get(g, 99, u, 3); CHECK(x >= 0);
        const double v[3] = {u[0] + 0.01, u[1] - 0.012, u[2] + 2e-6}; same += nr_ss_reg_get(g, 99, v, 3) == x;
    }
    CHECK(same == 2000);
    g->in[a].mem = 1;
    const uint32_t live = nr_ss_reg_sweep(g, 500, 100); CHECK(live == 1);
    CHECK(nr_ss_reg_get(g, 77, u1, 501) == a);
    const double u9[3] = {0.0 / 0.0, 0, 0}; CHECK(nr_ss_reg_get(g, 1, u9, 1) == -1);
    free(g);
}

// ---- the toy rasteriser: world pixels are whole numbers, a caster = a rectangle with one depth
enum { TW = 96, TH = 80, NC = 260 };
typedef struct cst { int32_t x0, y0, x1, y1; uint16_t z; int alive, member; nr_ss_rect bb; } cst;
static void full(const cst *c, int n, int32_t cx, int32_t cy, uint16_t *t) {
    for (int i = 0; i < TW * TH; i++) t[i] = 0xffff;
    for (int k = 0; k < n; k++) if (c[k].alive)
        for (int32_t y = c[k].y0 - cy; y < c[k].y1 - cy; y++) for (int32_t x = c[k].x0 - cx; x < c[k].x1 - cx; x++)
            if (x >= 0 && x < TW && y >= 0 && y < TH && c[k].z < t[y * TW + x]) t[y * TW + x] = c[k].z;
}
// one member into the layer through one piece (the viewport moved by d, the scissor = the piece)
static void draw_piece(uint16_t *L, const cst *c, int32_t cx, int32_t cy, const nr_ss_piece *p) {
    for (int32_t y = p->r.y0; y < p->r.y1; y++) for (int32_t x = p->r.x0; x < p->r.x1; x++)
        if (x >= c->x0 - cx && x < c->x1 - cx && y >= c->y0 - cy && y < c->y1 - cy) { uint16_t *d = &L[(y + p->dy) * TW + (x + p->dx)]; if (c->z < *d) *d = c->z; }
}
static void test_scroll(void) {
    long saved = 0, drawn = 0, frames = 0, recovers = 0, caught = 0;
    for (int seed = 0; seed < 6; seed++) {
        g_rng = 0x9000 + (uint64_t)seed * 7919;
        static cst c[NC]; static uint16_t L[TW * TH], T[TW * TH], A[TW * TH];
        for (int k = 0; k < NC; k++) {
            c[k].x0 = (int32_t)(rnd() % 900) - 300; c[k].y0 = (int32_t)(rnd() % 900) - 300; c[k].x1 = c[k].x0 + 1 + (int32_t)(rnd() % 60); c[k].y1 = c[k].y0 + 1 + (int32_t)(rnd() % 60);
            c[k].z = (uint16_t)(100 + rnd() % 60000); c[k].alive = 1; c[k].member = 0;
        }
        int32_t cx = 100, cy = 100, O[2] = {0, 0}, sh[2] = {0, 0}; int layer = 0;
        for (int f = 0; f < 400; f++) {
            frames++;
            const int32_t nx = f % 7 == 3 ? 0 : (int32_t)(rnd() % 31) - 15, ny = f % 5 == 1 ? 0 : (int32_t)(rnd() % 23) - 11;
            cx -= nx; cy -= ny;   // the content moves by (nx, ny)
            int removed = -1;
            if (f % 37 == 30) { removed = (int)(rnd() % NC); c[removed].alive = 0; }   // an object vanishes (maybe inside the tile)
            const nr_ss_rect tile = {0, 0, TW, TH};
            int ok = layer;
            if (layer) {
                nr_ss_rect e[2]; const int ne = nr_ss_exposed(TW, TH, nx, ny, e);
                int32_t O2[2] = {O[0], O[1]}; nr_ss_scroll(TW, TH, O2, nx, ny); sh[0] += nx; sh[1] += ny;
                // the engine's draws of this frame: every live caster whose bounds touch the tile (its own culling)
                for (int i = 0; i < ne; i++) { nr_ss_piece pc[4]; const int np = nr_ss_pieces(TW, TH, O2, &e[i], pc);
                    for (int q = 0; q < np; q++) for (int32_t y = pc[q].r.y0; y < pc[q].r.y1; y++) for (int32_t x = pc[q].r.x0; x < pc[q].r.x1; x++) L[(y + pc[q].dy) * TW + x + pc[q].dx] = 0xffff; }
                for (int k = 0; k < NC && ok; k++) {
                    if (!c[k].member) continue;
                    const nr_ss_rect now = {c[k].bb.x0 + sh[0], c[k].bb.y0 + sh[1], c[k].bb.x1 + sh[0], c[k].bb.y1 + sh[1]};
                    const nr_ss_rect b = {c[k].x0 - cx - 1, c[k].y0 - cy - 1, c[k].x1 - cx + 1, c[k].y1 - cy + 1};
                    if (c[k].alive) CHECK(!memcmp(&now, &b, sizeof b));   // BUILD coordinates + the shift = this frame's bounds
                    const int comes = c[k].alive && nr_ss_isect(&b, &tile);
                    if (!comes) {   // the member did not come: fine only when its bounds left the tile
                        if (nr_ss_isect(&now, &tile)) { ok = 0; caught++; } else c[k].member = 0;
                        continue;
                    }
                    if (nr_ss_touch(&b, e, ne)) {
                        drawn++;
                        for (int i = 0; i < ne; i++) { nr_ss_piece pc[4]; const int np = nr_ss_pieces(TW, TH, O2, &e[i], pc); for (int q = 0; q < np; q++) draw_piece(L, &c[k], cx, cy, &pc[q]); }
                    } else saved++;
                }
                O[0] = O2[0]; O[1] = O2[1];
            }
            full(c, NC, cx, cy, T);   // (every caster is static here: the truth of the members = the truth of the tile, as long as all are members)
            if (ok) {
                // members only: the truth restricted to the members
                static cst m[NC]; int nm = 0; for (int k = 0; k < NC; k++) if (c[k].member && c[k].alive) m[nm++] = c[k];
                full(m, nm, cx, cy, T);
                for (int32_t y = 0; y < TH; y++) for (int32_t x = 0; x < TW; x++) A[y * TW + x] = L[nr_ss_mod(y + O[1], TH) * TW + nr_ss_mod(x + O[0], TW)];
                const int eq = !memcmp(A, T, sizeof T);
                CHECK(eq);
                if (!eq && g_fails < 5) printf("  seed %d frame %d shift %d %d\n", seed, f, nx, ny);
                if (removed >= 0 && c[removed].member) { CHECK(0); }   // (a removed member inside the tile must have been caught above)
            }
            if (!ok) {   // BUILD: every caster the engine draws becomes a member, the layer is its tile
                if (layer) recovers++;
                for (int i = 0; i < TW * TH; i++) L[i] = 0xffff;
                O[0] = O[1] = 0; sh[0] = sh[1] = 0;
                for (int k = 0; k < NC; k++) {
                    const nr_ss_rect b = {c[k].x0 - cx - 1, c[k].y0 - cy - 1, c[k].x1 - cx + 1, c[k].y1 - cy + 1};
                    c[k].member = c[k].alive && nr_ss_isect(&b, &tile); c[k].bb = b;
                    if (c[k].member) { const nr_ss_piece whole = {{0, 0, TW, TH}, 0, 0}; draw_piece(L, &c[k], cx, cy, &whole); }
                }
                layer = 1;
            } else if (f % 16 == 15) {   // admission of the casters that came into the tile since (the cache's refresh = a BUILD)
                layer = 0;
            }
        }
    }
    printf("  scroll: %ld frames, member draws skipped %ld, drawn into strips %ld (%.1f %% skipped), rebuilds after a vanished member %ld (caught %ld)\n", frames, saved, drawn,
           100.0 * (double)saved / (double)(saved + drawn ? saved + drawn : 1), recovers, caught);
    CHECK(saved > drawn); CHECK(caught > 0);
}
int main(void) {
    test_prog(); test_bounds(); test_shift(); test_reg(); test_scroll();
    printf("nr_shs_test: %d checks, %d FAIL\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
