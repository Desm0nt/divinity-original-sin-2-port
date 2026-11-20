// [DOS2] NR-10b DM_NR_COVER: coverage proof of a pass's first draw whose vertex shader COMPUTES the position (docs/dos2/nr/NR-10b.md).
// Pure C, no Vulkan: the backend (xbg_nr_cover.inc), the SPIR-V analysis (xbg_spirv_pos.cpp) and the host test (tests/nr_cover_test.cpp)
// share it.
//
// The old proof (nvn_cover.h + XbgRasterFacts) needs "gl_Position = one input as it is" and ONE triangle around the whole target with the
// viewport = the target. DOS2's translated vertex shaders keep every register in a Function variable and write z / w as constants, so
// that proof never fires (25 refusals a frame, reason 0). Here:
//   1. nr_cv_prog - the position as a straight-line program over leaves the CPU has at the draw: float constants, components of vertex
//      inputs, floats of uniform blocks (binding + byte offset). Built once per shader (xbg_spirv_pos_prog); nr_cv_eval runs it per
//      vertex (a few dozen float ops for the 3..6 vertices of the first draw of a pass).
//   2. nr_cv_cover - which pixels the draw is PROVEN to write: the viewport rect ∩ scissor ∩ framebuffer (primitives are clipped to the
//      viewport, fragments to the scissor) when one non-culled triangle, or two non-culled triangles that share an edge and form a
//      convex quad, contain every pixel centre of that rect with a margin of NR_CV_MARGIN px. The margin pays for what the CPU cannot
//      reproduce bit for bit: fma contraction in the shader, the viewport transform, vertex snapping to the sub-pixel grid (Vulkan
//      guarantees 4 sub-pixel bits = 1/16 px). Requirements as nvn_cover: every vertex w == 1 exactly and z in [0, 1] (no perspective
//      divide, no depth clipping), finite x / y. Shared vertices of the pair must be bit-identical (the rasterizer's fill rule then
//      draws the shared edge exactly once).
// The result is a pixel rect: the whole framebuffer = the pass may begin with load DONT_CARE; a part of it (dynamic resolution: the post
// chain draws viewport 1620x729 of 2160x972) = it may begin with load DONT_CARE on that RENDER AREA (Vulkan keeps what lies outside a
// render area), as long as every later draw / clear of the pass stays inside it (the backend restarts the pass otherwise).
#ifndef DM_GFX_NR_COVER_H
#define DM_GFX_NR_COVER_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NR_CV_CONST = 0, NR_CV_INPUT, NR_CV_UBO, NR_CV_ADD, NR_CV_SUB, NR_CV_MUL, NR_CV_DIV, NR_CV_NEG, NR_CV_FMA, NR_CV_ABS, NR_CV_MIN,
       NR_CV_MAX, NR_CV_OPS };
enum { NR_CV_MAX_INS = 48, NR_CV_MAX_UBO = 4 };
// One value. CONST: k. INPUT: component b of the vertex input at location a. UBO: the float at byte b of the uniform block with Vulkan
// binding a. The others: operands are indices of earlier instructions (a, b, c).
typedef struct nr_cv_ins { uint8_t op; uint8_t pad; uint16_t a, b, c; float k; } nr_cv_ins;
typedef struct nr_cv_prog {
    uint8_t ok;                 // the position is such a program
    uint8_t n;                  // instructions
    uint8_t exact;              // every position component is a CONST or an INPUT (the GPU's value bit for bit)
    uint8_t nubo;               // uniform blocks read
    uint8_t out[4];             // instruction index of x, y, z, w
    uint32_t in_mask;           // input locations read (bit = location, < 32)
    uint32_t ubo[NR_CV_MAX_UBO];        // their Vulkan bindings
    uint32_t ubo_bytes[NR_CV_MAX_UBO];  // bytes read of each (max offset + 4)
    nr_cv_ins ins[NR_CV_MAX_INS];
} nr_cv_prog;

// in[location][component]: the vertex's inputs (locations of in_mask); ubo[i] / ubo_n[i]: bytes of the block pr->ubo[i] (ubo_n >= what
// is read, else 0 is returned). pos: x, y, z, w. Returns 1 on success.
static inline int nr_cv_eval(const nr_cv_prog *pr, const float (*in)[4], const uint8_t *const *ubo, const size_t *ubo_n, float pos[4]) {
    float v[NR_CV_MAX_INS];
    if (!pr->ok || pr->n > NR_CV_MAX_INS) return 0;
    for (unsigned i = 0; i < pr->n; i++) {
        const nr_cv_ins *x = &pr->ins[i];
        switch (x->op) {
        case NR_CV_CONST: v[i] = x->k; break;
        case NR_CV_INPUT: if (x->a >= 32 || x->b >= 4) return 0; v[i] = in[x->a][x->b]; break;
        case NR_CV_UBO: {
            unsigned u = 0;
            while (u < pr->nubo && pr->ubo[u] != x->a) u++;
            if (u >= pr->nubo || !ubo[u] || (size_t)x->b + 4 > ubo_n[u]) return 0;
            memcpy(&v[i], ubo[u] + x->b, 4);
            break;
        }
        default:
            if (x->a >= i || x->b >= i || x->c >= i) return 0;
            switch (x->op) {
            case NR_CV_ADD: v[i] = v[x->a] + v[x->b]; break;
            case NR_CV_SUB: v[i] = v[x->a] - v[x->b]; break;
            case NR_CV_MUL: v[i] = v[x->a] * v[x->b]; break;
            case NR_CV_DIV: v[i] = v[x->a] / v[x->b]; break;
            case NR_CV_NEG: v[i] = -v[x->a]; break;
            case NR_CV_FMA: v[i] = v[x->a] * v[x->b] + v[x->c]; break;
            case NR_CV_ABS: v[i] = fabsf(v[x->a]); break;
            case NR_CV_MIN: v[i] = v[x->a] < v[x->b] ? v[x->a] : v[x->b]; break;
            case NR_CV_MAX: v[i] = v[x->a] > v[x->b] ? v[x->a] : v[x->b]; break;
            default: return 0;
            }
        }
    }
    for (int c = 0; c < 4; c++) { if (pr->out[c] >= pr->n) return 0; pos[c] = v[pr->out[c]]; }
    return 1;
}

// ---- geometry ----
enum { NR_CV_TRIANGLES = 4, NR_CV_TRIANGLE_STRIP = 5 };                                   // NVNdrawPrimitive (as nvn_cover.h)
enum { NR_CV_CULL_NONE = 0, NR_CV_CULL_FRONT = 1, NR_CV_CULL_BACK = 2, NR_CV_CULL_BOTH = 3 };   // NVNface
enum { NR_CV_MAX_VERTS = 18 };
#define NR_CV_MARGIN 0.25   /* px beyond the outermost pixel centres the primitive must reach */
// Why a draw proves nothing (first failing check); 0 = proven.
enum { NR_CVG_OK = 0, NR_CVG_ARGS, NR_CVG_PRIM, NR_CVG_VERTEX /* w != 1, z outside [0,1], not finite */, NR_CVG_EMPTY /* no pixel in
       viewport ∩ scissor */, NR_CVG_GEOMETRY /* no triangle / convex pair contains the rect */, NR_CVG_CULLED /* one would, but it is
       culled */, NR_CVG_KINDS };
typedef struct nr_cv_rect { int32_t x0, y0, x1, y1; } nr_cv_rect;   // pixels [x0, x1) x [y0, y1)

static inline double nr_cv_edge(const double *a, const double *b, double px, double py) {
    return (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
}
// closed containment of (px, py) in triangle t (3 points, framebuffer px), either winding; a degenerate triangle contains nothing
static inline int nr_cv_in_tri(const double (*t)[2], double px, double py) {
    const double d = nr_cv_edge(t[0], t[1], t[2][0], t[2][1]);
    if (d == 0 || d != d) return 0;
    const double e0 = nr_cv_edge(t[0], t[1], px, py), e1 = nr_cv_edge(t[1], t[2], px, py), e2 = nr_cv_edge(t[2], t[0], px, py);
    return d > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 <= 0 && e1 <= 0 && e2 <= 0);
}
// v: clip-space positions of the n vertices the draw submits, in order (indexed draws: after the index fetch). cull / front_ccw: the NVN
// cull face and front face as nvn_cover takes them (counter-clockwise in NDC with y up is front when front_ccw). vx..vh: the Vulkan
// viewport the draw sets (vh may be negative). sx..sh: the scissor. W, H: the framebuffer. *out: the proven rect.
static inline int nr_cv_cover(const float (*v)[4], int n, int prim, int cull, int front_ccw, float vx, float vy, float vw, float vh,
                              int32_t sx, int32_t sy, uint32_t sw, uint32_t sh, uint32_t W, uint32_t H, nr_cv_rect *out) {
    out->x0 = out->y0 = out->x1 = out->y1 = 0;
    if (!W || !H || n < 3 || n > NR_CV_MAX_VERTS || W > 65536 || H > 65536) return NR_CVG_ARGS;
    if (prim != NR_CV_TRIANGLES && prim != NR_CV_TRIANGLE_STRIP) return NR_CVG_PRIM;
    if (!(vw > 0) || vh == 0 || vh != vh || !isfinite(vx) || !isfinite(vy) || !isfinite(vw) || !isfinite(vh)) return NR_CVG_ARGS;
    for (int k = 0; k < n; k++)
        if (v[k][3] != 1.0f || !(v[k][2] >= 0.0f && v[k][2] <= 1.0f) || !isfinite(v[k][0]) || !isfinite(v[k][1])) return NR_CVG_VERTEX;
    // The pixels whose centre lies NR_CV_MARGIN inside the viewport rect, inside the scissor and the framebuffer.
    const double m = NR_CV_MARGIN;
    const double py0 = vh < 0 ? (double)vy + vh : (double)vy, py1 = vh < 0 ? (double)vy : (double)vy + vh;
    double fx0 = ceil((double)vx + m - 0.5), fx1 = floor((double)vx + vw - m - 0.5) + 1;   // pixel i: i + 0.5 in [vx + m, vx + vw - m]
    double fy0 = ceil(py0 + m - 0.5), fy1 = floor(py1 - m - 0.5) + 1;
    const double s0x = sx, s0y = sy, s1x = (double)sx + sw, s1y = (double)sy + sh;
    fx0 = fx0 < s0x ? s0x : fx0; fy0 = fy0 < s0y ? s0y : fy0; fx1 = fx1 > s1x ? s1x : fx1; fy1 = fy1 > s1y ? s1y : fy1;
    fx0 = fx0 < 0 ? 0 : fx0; fy0 = fy0 < 0 ? 0 : fy0; fx1 = fx1 > W ? W : fx1; fy1 = fy1 > H ? H : fy1;
    if (!(fx0 < fx1) || !(fy0 < fy1)) return NR_CVG_EMPTY;
    // Corners the primitive must contain: the outermost pixel centres pushed out by the margin.
    const double cx[2] = {fx0 + 0.5 - m, fx1 - 0.5 + m}, cy[2] = {fy0 + 0.5 - m, fy1 - 0.5 + m};
    const int ntri = prim == NR_CV_TRIANGLES ? n / 3 : n - 2;
    double T[NR_CV_MAX_VERTS][3][2]; int I[NR_CV_MAX_VERTS][3], live[NR_CV_MAX_VERTS], culled_any = 0, nt = 0;
    for (int k = 0; k < ntri; k++) {
        int i0 = prim == NR_CV_TRIANGLES ? 3 * k : k, i1 = i0 + 1, i2 = i0 + 2;
        if (prim == NR_CV_TRIANGLE_STRIP && (k & 1)) { int x = i1; i1 = i2; i2 = x; }
        const int id[3] = {i0, i1, i2};
        // facing in NDC, y up (nvn_cover's convention)
        const double ax = v[i0][0], ay = v[i0][1], bx = v[i1][0], by = v[i1][1], dx = v[i2][0], dy = v[i2][1];
        const double d = (bx - ax) * (dy - ay) - (by - ay) * (dx - ax);
        if (d == 0) continue;
        const int front = front_ccw ? d > 0 : d < 0;
        live[nt] = !(cull == NR_CV_CULL_BOTH || (cull == NR_CV_CULL_BACK && !front) || (cull == NR_CV_CULL_FRONT && front));
        for (int j = 0; j < 3; j++) {
            I[nt][j] = id[j];
            T[nt][j][0] = (double)vx + ((double)v[id[j]][0] + 1.0) * 0.5 * (double)vw;
            T[nt][j][1] = (double)vy + ((double)v[id[j]][1] + 1.0) * 0.5 * (double)vh;
        }
        nt++;
    }
    int found = 0;
    for (int a = 0; a < nt && found != 1; a++) {
        int in = 1;
        for (int c = 0; c < 4 && in; c++) in = nr_cv_in_tri(T[a], cx[c & 1], cy[c >> 1]);
        if (in) { if (live[a]) found = 1; else culled_any = 1; }
    }
    for (int a = 0; a < nt && found != 1; a++) for (int b = a + 1; b < nt && found != 1; b++) {
        // two shared vertices (bit-identical clip positions), the other two strictly on opposite sides of the shared edge, convex
        int sa[2], sb[2], ns = 0, pa = -1, pb = -1;
        for (int i = 0; i < 3; i++) {
            int hit = -1;
            for (int j = 0; j < 3; j++) if (!memcmp(v[I[a][i]], v[I[b][j]], sizeof(float) * 4)) hit = j;
            if (hit >= 0) { if (ns < 2) { sa[ns] = i; sb[ns] = hit; } ns++; } else pa = i;
        }
        if (ns != 2 || pa < 0 || sb[0] == sb[1]) continue;
        pb = 3 - sb[0] - sb[1];
        const double *S1 = T[a][sa[0]], *S2 = T[a][sa[1]], *P = T[a][pa], *Q = T[b][pb];
        const double eP = nr_cv_edge(S1, S2, P[0], P[1]), eQ = nr_cv_edge(S1, S2, Q[0], Q[1]);
        if (!((eP > 0 && eQ < 0) || (eP < 0 && eQ > 0))) continue;
        const double e1 = nr_cv_edge(P, Q, S1[0], S1[1]), e2 = nr_cv_edge(P, Q, S2[0], S2[1]);
        if (!((e1 > 0 && e2 < 0) || (e1 < 0 && e2 > 0))) continue;   // not convex: the union is not the hull
        int in = 1;
        for (int c = 0; c < 4 && in; c++) in = nr_cv_in_tri(T[a], cx[c & 1], cy[c >> 1]) || nr_cv_in_tri(T[b], cx[c & 1], cy[c >> 1]);
        if (in) { if (live[a] && live[b]) found = 1; else culled_any = 1; }
    }
    if (found != 1) return culled_any ? NR_CVG_CULLED : NR_CVG_GEOMETRY;
    out->x0 = (int32_t)fx0; out->y0 = (int32_t)fy0; out->x1 = (int32_t)fx1; out->y1 = (int32_t)fy1;
    return NR_CVG_OK;
}
static inline int nr_cv_rect_full(nr_cv_rect r, uint32_t W, uint32_t H) { return r.x0 == 0 && r.y0 == 0 && r.x1 == (int32_t)W && r.y1 == (int32_t)H; }

#ifdef __cplusplus
}
#endif
#endif
