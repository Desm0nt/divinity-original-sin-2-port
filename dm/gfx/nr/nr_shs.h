// [DOS2] VK-SHADOWS-MOVING (docs/dos2/nr/VK-SHADOWS-MOVING.md): the pure parts of "shadows cheap while the camera moves" - caster bounds
// on the CPU, caster culling, and the scrolling (toroidal) layer of the static shadow cache. Pure C for C and C++ (no Vulkan, no engine
// types, no environment, no game address); used by dm/gfx/vkn/vkn_shs.c / vkn_shc.c and tested by tests/nr_shs_test.c (a toy rasteriser:
// the scrolled layer against a full render, texel for texel).
//
// 1. nr_sb_prog - a vertex shader's gl_Position as a straight-line program (the long form of nr_cover.h's nr_cv_prog: same
//    instructions, up to NR_SB_MAX_INS of them) with the DEGREE of every value in the position attribute (Location 0 x / y / z):
//    0 = a per-draw constant, 1 = affine. A program is `affine` when x, y, z, w are of degree <= 1 and no other input is read. Then
//        clip(p) = c0 + cx * p.x + cy * p.y + cz * p.z          (per output component; nr_sb_eval gives the four coefficients)
//    for every vertex p of the draw, so the draw's fragments lie inside the image of the mesh's bounding box (nr_sb_range: interval
//    arithmetic on the box) - the CPU needs no vertex but the box. The coefficients cx / cy / cz never see a translation word of any
//    matrix (a translation only enters c0), so they are bit-stable while the cascade's projection only translates.
// 2. Culling: a draw whose pixel bounds (nr_sb_px: the viewport transform of the range, 1 px of margin outwards for what the CPU cannot
//    reproduce bit for bit - fma contraction, the viewport transform, vertex snapping) do not touch viewport and scissor rasterises
//    nothing: skipped, exact.
// 3. The scrolling layer. The layer image keeps min(clear, static members) of a cascade tile. When the cascade's projection moved by a
//    whole number of texels (nx, ny) since the layer was drawn, the same texels are valid at the shifted place: tile pixel p of now holds
//    what tile pixel p - n held. The layer is addressed toroidally (layer address of tile pixel p = (p + O) mod size, O -= n per shift:
//    nothing is copied inside the layer); the pixels with p - n outside the tile are EXPOSED (nr_ss_exposed: up to two strips) and must be
//    drawn again: by every member whose bounds touch them (nr_ss_touch) - all other members are skipped. The layer's own strips are
//    cleared and get those members too (nr_ss_pieces: where a tile rectangle lies in the layer, up to four pieces).
//    Invariant (induction over frames; tests/nr_shs_test.c checks it on random walks): for every tile pixel p,
//        layer(p) = min(clear, fragments at p of every member)                                              (*)
//    - p not exposed: p - n was in the tile, (*) held there for the same members at their shifted place (a member's fragments move with
//      the projection by exactly n: same triangles on the same texel grid);
//    - p exposed: every member with a fragment at p touches the strip, so it was drawn into the cleared strip.
//    A member that does not come any more is fine exactly when its bounds left the tile (its texels scrolled out); otherwise the layer
//    is wrong and the cascade is repaired (the cache's RECOVER).
// 4. Instances: a member is found again under a moved projection by what does NOT move - its structure (shader, states, mesh, the linear
//    part of its clip transform) and its translation RELATIVE to the cascade's reference translation, u = c0 - t_ref (= the linear part
//    of the view-projection times the object's world origin). nr_ss_reg gives every (structure, u) a stable id (u compared with a
//    tolerance: the engine recomputes its matrices in floats every frame), which the cache's hash tables (nr_shc.h) take as the draw's hash.
#ifndef DM_NR_SHS_H
#define DM_NR_SHS_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nr_cover.h"
#ifdef __cplusplus
extern "C" {
#endif

// ================================================================================================ 1. the long position program
enum { NR_SB_MAX_INS = 640, NR_SB_MAX_UBO = 6 };
// nr_cv_ins.pad carries the degree of the value: 0 constant per draw, 1 affine in Location 0 x / y / z.
typedef struct nr_sb_prog {
    uint8_t ok;                 // the position is a program
    uint8_t affine;             // ... and x, y, z, w are affine in Location 0 x / y / z (no other input)
    uint8_t in_w;               // it reads component 3 of Location 0 (fine only when the vertex format gives a constant there)
    uint8_t nubo;
    uint16_t n, out[4];
    uint32_t in_mask;           // input locations read
    uint32_t ubo[NR_SB_MAX_UBO], ubo_bytes[NR_SB_MAX_UBO];
    nr_cv_ins ins[1];           // n of them (allocated with the struct: nr_sb_prog_bytes)
} nr_sb_prog;
static inline size_t nr_sb_prog_bytes(unsigned n) { return sizeof(nr_sb_prog) + (n ? n - 1 : 0) * sizeof(nr_cv_ins); }

// The degrees (ins[i].pad) and the affine flag from the instructions. Returns the flag.
static inline int nr_sb_degrees(nr_sb_prog *pr) {
    int ok = 1;
    pr->in_w = 0;
    for (unsigned i = 0; i < pr->n; i++) {
        nr_cv_ins *x = &pr->ins[i]; uint8_t d;
        switch (x->op) {
        case NR_CV_CONST: case NR_CV_UBO: d = 0; break;
        case NR_CV_INPUT:
            if (x->a != 0 || x->b > 3) { d = 2; break; }
            if (x->b == 3) { pr->in_w = 1; d = 0; } else d = 1;
            break;
        default: {
            const uint8_t da = x->a < i ? pr->ins[x->a].pad : 2, db = x->b < i ? pr->ins[x->b].pad : 2, dc = x->c < i ? pr->ins[x->c].pad : 2;
            switch (x->op) {
            case NR_CV_ADD: case NR_CV_SUB: d = da > db ? da : db; break;
            case NR_CV_MUL: d = (uint8_t)(da + db); break;
            case NR_CV_DIV: d = db ? 2 : da; break;
            case NR_CV_NEG: d = da; break;
            case NR_CV_FMA: { const uint8_t m = (uint8_t)(da + db); d = m > dc ? m : dc; break; }
            case NR_CV_ABS: d = da ? 2 : 0; break;
            case NR_CV_MIN: case NR_CV_MAX: d = (da | db) ? 2 : 0; break;
            default: d = 2; break;
            }
        } }
        if (d > 2) d = 2;
        x->pad = d;
    }
    for (int c = 0; c < 4; c++) if (pr->out[c] >= pr->n || pr->ins[pr->out[c]].pad > 1) ok = 0;
    if (pr->in_mask & ~1u) ok = 0;
    pr->affine = (uint8_t)(pr->ok && ok);
    return pr->affine;
}

typedef struct nr_sb_form { float c[4]; } nr_sb_form;   // c[0] + c[1] x + c[2] y + c[3] z
// The four position components as affine forms. ubo[i] / ubo_n[i]: the bytes of block pr->ubo[i]; in_w: the constant Location 0 has in
// component 3. 1 = ok; 0 = not affine / a block too short. The linear coefficients are computed from the same operands in the same
// order whatever the constant terms are.
static inline int nr_sb_eval(const nr_sb_prog *pr, const uint8_t *const *ubo, const size_t *ubo_n, float in_w, nr_sb_form out[4]) {
    nr_sb_form v[NR_SB_MAX_INS];
    if (!pr->ok || !pr->affine || pr->n > NR_SB_MAX_INS) return 0;
    for (unsigned i = 0; i < pr->n; i++) {
        const nr_cv_ins *x = &pr->ins[i]; nr_sb_form *r = &v[i];
        r->c[1] = r->c[2] = r->c[3] = 0.0f;
        switch (x->op) {
        case NR_CV_CONST: r->c[0] = x->k; break;
        case NR_CV_INPUT: if (x->b == 3) r->c[0] = in_w; else { r->c[0] = 0.0f; r->c[1 + x->b] = 1.0f; } break;
        case NR_CV_UBO: {
            unsigned u = 0;
            while (u < pr->nubo && pr->ubo[u] != x->a) u++;
            if (u >= pr->nubo || !ubo[u] || (size_t)x->b + 4 > ubo_n[u]) return 0;
            memcpy(&r->c[0], ubo[u] + x->b, 4);
            break;
        }
        default: {
            const nr_sb_form *a = &v[x->a], *b = &v[x->b], *c = &v[x->c];
            if (!x->pad) {   // constants: plain floats
                switch (x->op) {
                case NR_CV_ADD: r->c[0] = a->c[0] + b->c[0]; break;
                case NR_CV_SUB: r->c[0] = a->c[0] - b->c[0]; break;
                case NR_CV_MUL: r->c[0] = a->c[0] * b->c[0]; break;
                case NR_CV_DIV: r->c[0] = a->c[0] / b->c[0]; break;
                case NR_CV_NEG: r->c[0] = -a->c[0]; break;
                case NR_CV_FMA: r->c[0] = a->c[0] * b->c[0] + c->c[0]; break;
                case NR_CV_ABS: r->c[0] = fabsf(a->c[0]); break;
                case NR_CV_MIN: r->c[0] = a->c[0] < b->c[0] ? a->c[0] : b->c[0]; break;
                case NR_CV_MAX: r->c[0] = a->c[0] > b->c[0] ? a->c[0] : b->c[0]; break;
                default: return 0;
                }
                break;
            }
            switch (x->op) {
            case NR_CV_ADD: for (int k = 0; k < 4; k++) r->c[k] = a->c[k] + b->c[k]; break;
            case NR_CV_SUB: for (int k = 0; k < 4; k++) r->c[k] = a->c[k] - b->c[k]; break;
            case NR_CV_NEG: for (int k = 0; k < 4; k++) r->c[k] = -a->c[k]; break;
            case NR_CV_DIV: for (int k = 0; k < 4; k++) r->c[k] = a->c[k] / b->c[0]; break;
            case NR_CV_MUL: case NR_CV_FMA: {
                const nr_sb_form *f = pr->ins[x->a].pad ? a : b; const float s = pr->ins[x->a].pad ? b->c[0] : a->c[0];   // (one side is a constant)
                for (int k = 0; k < 4; k++) r->c[k] = f->c[k] * s;
                if (x->op == NR_CV_FMA) for (int k = 0; k < 4; k++) r->c[k] += c->c[k];
                break;
            }
            default: return 0;
            }
        } }
    }
    for (int c = 0; c < 4; c++) out[c] = v[pr->out[c]];
    return 1;
}

// ================================================================================================ 2. bounds and culling
typedef struct nr_sb_box { float lo[3], hi[3]; } nr_sb_box;   // the mesh's bounding box (model space: the position attribute)
static inline void nr_sb_range(const nr_sb_form *f, const nr_sb_box *b, float *lo, float *hi) {
    float l = f->c[0], h = f->c[0];
    for (int k = 0; k < 3; k++) {
        const float p = f->c[1 + k] * b->lo[k], q = f->c[1 + k] * b->hi[k];
        l += p < q ? p : q; h += p < q ? q : p;
    }
    *lo = l; *hi = h;
}
typedef struct nr_ss_rect { int32_t x0, y0, x1, y1; } nr_ss_rect;   // pixels [x0, x1) x [y0, y1)
static inline int nr_ss_empty(const nr_ss_rect *r) { return r->x0 >= r->x1 || r->y0 >= r->y1; }
static inline int nr_ss_isect(const nr_ss_rect *a, const nr_ss_rect *b) { return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1; }
enum { NR_SB_MARGIN = 1 };   // px outwards
// The pixels a draw with the affine position f[0..3] (w == 1) over box b can touch, for the engine viewport (x, y, w, h: upper-left
// origin; the draw path flips y: pixel y = vy + (1 - clip y) * vh / 2). 1 = ok (r: framebuffer pixels, z0 / z1: its clip z range);
// 0 = not finite / w not exactly 1.
static inline int nr_sb_px(const nr_sb_form f[4], const nr_sb_box *b, const float vp[4], nr_ss_rect *r, float *z0, float *z1) {
    if (f[3].c[0] != 1.0f || f[3].c[1] != 0.0f || f[3].c[2] != 0.0f || f[3].c[3] != 0.0f) return 0;
    float xl, xh, yl, yh;
    nr_sb_range(&f[0], b, &xl, &xh); nr_sb_range(&f[1], b, &yl, &yh); nr_sb_range(&f[2], b, z0, z1);
    const double px0 = (double)vp[0] + ((double)xl + 1.0) * 0.5 * (double)vp[2], px1 = (double)vp[0] + ((double)xh + 1.0) * 0.5 * (double)vp[2];
    const double py0 = (double)vp[1] + (1.0 - (double)yh) * 0.5 * (double)vp[3], py1 = (double)vp[1] + (1.0 - (double)yl) * 0.5 * (double)vp[3];
    if (!(px0 == px0 && px1 == px1 && py0 == py0 && py1 == py1) || !(*z0 == *z0 && *z1 == *z1)) return 0;
    if (fabs(px0) > 1e9 || fabs(px1) > 1e9 || fabs(py0) > 1e9 || fabs(py1) > 1e9) return 0;
    r->x0 = (int32_t)floor(px0) - NR_SB_MARGIN; r->x1 = (int32_t)ceil(px1) + NR_SB_MARGIN;
    r->y0 = (int32_t)floor(py0) - NR_SB_MARGIN; r->y1 = (int32_t)ceil(py1) + NR_SB_MARGIN;
    return 1;
}

// ================================================================================================ 3. the scrolling layer
// A shift of the projection in pixels from the change of its clip-space translation (dtx, dty: now - then): the content of the tile
// moved by (nx, ny) = (dtx * vw / 2, -dty * vh / 2). 1 = both within eps px of whole numbers (nx / ny: them); 0 = not a texel shift.
static inline int nr_ss_shift(double dtx, double dty, double vw, double vh, double eps, int32_t *nx, int32_t *ny, double *err) {
    const double sx = dtx * 0.5 * vw, sy = -dty * 0.5 * vh;
    if (!(sx == sx && sy == sy) || fabs(sx) > 1e6 || fabs(sy) > 1e6) { *nx = *ny = 0; *err = 1e9; return 0; }
    const double rx = floor(sx + 0.5), ry = floor(sy + 0.5), ex = fabs(sx - rx), ey = fabs(sy - ry);
    *nx = (int32_t)rx; *ny = (int32_t)ry; *err = ex > ey ? ex : ey;
    return ex <= eps && ey <= eps;
}
// The exposed pixels of a W x H tile (tile-local) after its content moved by (nx, ny): 0..2 rectangles (1 = the whole tile when the
// shift is as large as the tile).
static inline int nr_ss_exposed(int32_t W, int32_t H, int32_t nx, int32_t ny, nr_ss_rect e[2]) {
    int k = 0;
    if (nx >= W || nx <= -W || ny >= H || ny <= -H) { e[0].x0 = e[0].y0 = 0; e[0].x1 = W; e[0].y1 = H; return 1; }
    if (nx > 0) { e[k].x0 = 0; e[k].y0 = 0; e[k].x1 = nx; e[k].y1 = H; k++; }
    else if (nx < 0) { e[k].x0 = W + nx; e[k].y0 = 0; e[k].x1 = W; e[k].y1 = H; k++; }
    const int32_t x0 = nx > 0 ? nx : 0, x1 = nx < 0 ? W + nx : W;
    if (ny > 0) { e[k].x0 = x0; e[k].y0 = 0; e[k].x1 = x1; e[k].y1 = ny; k++; }
    else if (ny < 0) { e[k].x0 = x0; e[k].y0 = H + ny; e[k].x1 = x1; e[k].y1 = H; k++; }
    return k;
}
static inline int32_t nr_ss_mod(int32_t a, int32_t m) { const int32_t r = a % m; return r < 0 ? r + m : r; }
// The layer's offset after the shift: the layer address of tile pixel p is (p + O) mod size.
static inline void nr_ss_scroll(int32_t W, int32_t H, int32_t O[2], int32_t nx, int32_t ny) { O[0] = nr_ss_mod(O[0] - nx, W); O[1] = nr_ss_mod(O[1] - ny, H); }
// Where the tile-local rectangle r lies in the layer: 1..4 pieces, each a tile-local rectangle and (dx, dy) with layer address = local + d.
typedef struct nr_ss_piece { nr_ss_rect r; int32_t dx, dy; } nr_ss_piece;
static inline int nr_ss_pieces(int32_t W, int32_t H, const int32_t O[2], const nr_ss_rect *r, nr_ss_piece p[4]) {
    int n = 0;
    const int32_t sx = W - O[0], sy = H - O[1];   // local x < sx: address x + O[0]; else x + O[0] - W
    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
        nr_ss_rect q;
        q.x0 = i ? (r->x0 > sx ? r->x0 : sx) : r->x0; q.x1 = i ? r->x1 : (r->x1 < sx ? r->x1 : sx);
        q.y0 = j ? (r->y0 > sy ? r->y0 : sy) : r->y0; q.y1 = j ? r->y1 : (r->y1 < sy ? r->y1 : sy);
        if (nr_ss_empty(&q)) continue;
        p[n].r = q; p[n].dx = i ? O[0] - W : O[0]; p[n].dy = j ? O[1] - H : O[1]; n++;
    }
    return n;
}
// 1 = the tile-local bounds b touch one of the exposed rectangles.
static inline int nr_ss_touch(const nr_ss_rect *b, const nr_ss_rect *e, int ne) {
    for (int i = 0; i < ne; i++) if (nr_ss_isect(b, &e[i])) return 1;
    return 0;
}

// ================================================================================================ 4. instances
// One (structure, relative translation) seen in a cascade slot. u: x / y in pixels, z in clip units (relative to the cascade's reference).
enum { NR_SS_INST = 4096, NR_SS_TAB = 8192 };
typedef struct nr_ss_inst {
    uint64_t hs, id;            // structure hash; the stable id (never 0 while used)
    double u[3];
    uint32_t seen, prev;        // the range serial it was last looked up in; the one before that
    uint64_t hf;                // the caller's: a hash of the position inputs of its last draw
    uint64_t ho;                // the caller's: a hash of the object's own position inputs (all but the cascade's block) when last seen
    uint16_t mem, cons;         // member units in the layer; units consumed in the range `cstamp`
    uint32_t cstamp;
    nr_ss_rect bb;              // members: the bounds in BUILD coordinates (tile-local pixels at the layer's build; now = bb + shift since)
    uint8_t used, bad;          // bad: must not be a member (a clip-range guard failed)
    uint8_t ho_ok, pad;         // ho_ok: ho holds a value
} nr_ss_inst;
typedef struct nr_ss_reg {
    nr_ss_inst in[NR_SS_INST]; uint32_t n, nfree, free_[NR_SS_INST];
    int32_t tab[NR_SS_TAB];     // open addressing by cell hash: instance index + 1 (0 empty)
    uint64_t serial;
    double cell_xy, cell_z, tol_xy, tol_z;
} nr_ss_reg;
static inline uint64_t nr_ss_mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29; return h; }
static inline void nr_ss_reg_init(nr_ss_reg *g, double tol_xy, double tol_z) {
    memset(g, 0, sizeof *g); g->tol_xy = tol_xy; g->tol_z = tol_z;
    g->cell_xy = 1.0; g->cell_z = 1.0 / 4096.0;
    if (g->cell_xy < 8 * tol_xy) g->cell_xy = 8 * tol_xy;
    if (g->cell_z < 8 * tol_z) g->cell_z = 8 * tol_z;
}
static inline uint64_t nr_ss_cellh(uint64_t hs, int64_t qx, int64_t qy, int64_t qz) {
    return nr_ss_mix(nr_ss_mix(nr_ss_mix(hs, (uint64_t)qx), (uint64_t)qy), (uint64_t)qz);
}
static inline int nr_ss_reg_probe(const nr_ss_reg *g, uint64_t hs, const double u[3], int64_t qx, int64_t qy, int64_t qz) {
    uint32_t i = (uint32_t)(nr_ss_cellh(hs, qx, qy, qz) >> 40) & (NR_SS_TAB - 1);
    for (uint32_t k = 0; k < NR_SS_TAB; k++, i = (i + 1) & (NR_SS_TAB - 1)) {
        const int32_t e = g->tab[i];
        if (!e) return -1;
        const nr_ss_inst *x = &g->in[e - 1];
        if (x->used && x->hs == hs && fabs(x->u[0] - u[0]) <= g->tol_xy && fabs(x->u[1] - u[1]) <= g->tol_xy && fabs(x->u[2] - u[2]) <= g->tol_z) return e - 1;
    }
    return -1;
}
static inline void nr_ss_reg_put(nr_ss_reg *g, int idx) {
    const nr_ss_inst *x = &g->in[idx];
    const int64_t qx = (int64_t)floor(x->u[0] / g->cell_xy), qy = (int64_t)floor(x->u[1] / g->cell_xy), qz = (int64_t)floor(x->u[2] / g->cell_z);
    uint32_t i = (uint32_t)(nr_ss_cellh(x->hs, qx, qy, qz) >> 40) & (NR_SS_TAB - 1);
    while (g->tab[i]) i = (i + 1) & (NR_SS_TAB - 1);
    g->tab[i] = idx + 1;
}
// The instance of (hs, u): found within the tolerance (the home cell and, near a cell border, its neighbours) or made. -1 = full.
static inline int nr_ss_reg_get(nr_ss_reg *g, uint64_t hs, const double u[3], uint32_t range) {
    if (!(u[0] == u[0] && u[1] == u[1] && u[2] == u[2]) || fabs(u[0]) > 1e12 || fabs(u[1]) > 1e12 || fabs(u[2]) > 1e12) return -1;
    int64_t q[3], alt[3]; int na[3];
    for (int k = 0; k < 3; k++) {
        const double cell = k < 2 ? g->cell_xy : g->cell_z, tol = k < 2 ? g->tol_xy : g->tol_z, t = u[k] / cell, fl = floor(t);
        q[k] = (int64_t)fl; na[k] = 0;
        if ((t - fl) * cell <= tol) { alt[k] = q[k] - 1; na[k] = 1; }
        else if ((fl + 1.0 - t) * cell <= tol) { alt[k] = q[k] + 1; na[k] = 1; }
    }
    for (int m = 0; m < 8; m++) {
        if (((m & 1) && !na[0]) || ((m & 2) && !na[1]) || ((m & 4) && !na[2])) continue;
        const int f = nr_ss_reg_probe(g, hs, u, (m & 1) ? alt[0] : q[0], (m & 2) ? alt[1] : q[1], (m & 4) ? alt[2] : q[2]);
        if (f >= 0) { if (g->in[f].seen != range) { g->in[f].prev = g->in[f].seen; g->in[f].seen = range; } return f; }
    }
    int idx;
    if (g->nfree) idx = (int)g->free_[--g->nfree];
    else if (g->n < NR_SS_INST) idx = (int)g->n++;
    else return -1;
    nr_ss_inst *x = &g->in[idx]; memset(x, 0, sizeof *x);
    x->used = 1; x->hs = hs; x->u[0] = u[0]; x->u[1] = u[1]; x->u[2] = u[2]; x->seen = range;
    x->id = nr_ss_mix(hs, ++g->serial) | 1u;
    nr_ss_reg_put(g, idx);
    return idx;
}
// Forgets the instances that are no members and were not seen for `age` ranges (the table is made again). Returns how many stay.
static inline uint32_t nr_ss_reg_sweep(nr_ss_reg *g, uint32_t range, uint32_t age) {
    uint32_t live = 0;
    memset(g->tab, 0, sizeof g->tab);
    for (uint32_t i = 0; i < g->n; i++) {
        nr_ss_inst *x = &g->in[i];
        if (!x->used) continue;
        if (!x->mem && range - x->seen > age) { x->used = 0; g->free_[g->nfree++] = i; continue; }
        nr_ss_reg_put(g, (int)i); live++;
    }
    return live;
}

#ifdef __cplusplus
}
#endif
#endif
