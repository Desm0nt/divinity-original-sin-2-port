// [DOS2] NR-09: the native draw (DM_NR_DRAW, default 0; docs/dos2/nr/NR-09.md). Pure C for C and C++ (no Vulkan / engine / host types,
// no environment, no game address): the decisions of the backend's short draw path (dm/gfx/vulkan/engine/xbg_nr_draw.inc, namespace
// nrdraw) and of its DM_NR_DRAW=2 comparison, tested on the PC by dm/gfx/vulkan/engine/tests/nr_draw_test.c.
//
//   the variant words   what the host adds to the engine's key id: nr_draw_var packs it into the 6 words of NR-07's direct table
//                       (dm/gfx/nr/nr_pipe.h nrp_tab: pipeline[key id][variant])
//   offset folds        an index buffer is bound at (offset mod index size) and drawn with firstIndex = offset / index size; the
//                       per-vertex streams are bound at offset - k * stride and drawn with vertexOffset + k: the bytes fetched are the
//                       same, the bind commands of two meshes that live in one VkBuffer (a zero-copy chunk, a static copy chunk of NR-06)
//                       become equal and the bind cache drops them
//   bind runs           the vertex streams that changed go out as one vkCmdBindVertexBuffers per run of adjacent bindings
//   the comparison      DM_NR_DRAW=2: what the old draw() bound and drew against what the short path would have (same fetch addresses)
//   the model           vkCmd* a draw of the old tail and of the short path on a stream of draws (the test's numbers for NR-09.md)
#ifndef DM_NR_DRAW_H
#define DM_NR_DRAW_H
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

// ---- why a draw with an OP_X_NR_DRAW record was not drawn by the short path (it goes to the old draw(); counted per reason) ----
enum { NRW_SERVED = 0,
       NRW_NO_KEY,        // the record's key id is 0 (the front end's key table was full)
       NRW_GATE,          // a diagnostic is on (capture, pipeline statistics, vertex statistics, crumbs, pass graph) or the bind cache is off
       NRW_NO_GROUPS,     // DM_NB_PIPE off / DM_GFX_DRAW_FAST not 1 / the pipeline memo off: no exact state ids for the variant
       NRW_STAGES,        // a tessellation / geometry stage, or no vertex shader
       NRW_SPECIAL,       // GPU-indirect, quads, the UI offset (the pre-pass depth override is served since NR-30: NR_DRAW_VAR_PP)
       NRW_DEF,           // the descriptor layout / render pass has no pipeline definition
       NRW_MISS,          // no pipeline for (key id, variant) yet: the old path draws and teaches the table
       NRW_NEW_PASS,      // the draw does not continue the open pass (the old path begins it), or a lazy clear waits
       NRW_UIAREA,        // an Iggy pass of DM_NB_UIAREA (reduced render area / its own bookkeeping per draw)
       NRW_COUNTING,      // precise occlusion counting is on for this draw
       NRW_WRITEBACK,     // the draw writes storage the CPU reads back
       NRW_VERTEX,        // an unbound vertex stream, u8 indices, a negative base instance
       NRW_VIEWPORT,      // a viewport the old path skips, reverses or checks against the device limits
       NRW_N };
static inline const char *nr_draw_why(int w) {
    static const char *const n[NRW_N] = {"served", "no key id", "diagnostics / bind cache off", "no state-group ids", "extra stages", "indirect / quads / UI offset",
                                         "no pipeline definition", "table miss", "not the open pass", "uiarea pass", "occlusion counting", "storage writeback",
                                         "vertex / index input", "viewport"};
    return w >= 0 && w < NRW_N ? n[w] : "?";
}

// ---- the variant: 6 words (NRP_VAR_WORDS), every bit set by the caller ----
// vs / fs: the backend's shader objects of the vertex and fragment stage (the engine's shader handle names them, the pointers are exact);
// layout / pass_def: the descriptor layout's and the render pass's pipeline definition; gid: the six state-group ids of NB-13b
// (dm/gfx/nb/nb_pipe.h: exact content ids of blend / colour / polygon / depth-stencil / vertex format / misc, each < 1024).
typedef struct { uint64_t w[6]; } nr_draw_var_t;
static inline uint64_t nr_draw_gids(const uint32_t gid[6]) {   // 6 x 10 bits, exact while every id < 1024
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v |= (uint64_t)(gid[i] & 1023u) << (10 * i);
    return v;
}
static inline void nr_draw_var(nr_draw_var_t *v, uint64_t vs, uint64_t fs, uint64_t layout, uint64_t pass_def, unsigned prim, unsigned samples, unsigned rt_n,
                               unsigned colors, unsigned depth, unsigned depth_format, unsigned restart_eff, const uint32_t gid[6]) {
    v->w[0] = vs; v->w[1] = fs; v->w[2] = layout; v->w[3] = pass_def;
    v->w[4] = (uint64_t)(prim & 0xffu) | (uint64_t)(samples & 0xffu) << 8 | (uint64_t)(rt_n & 0xfu) << 16 | (uint64_t)(colors & 0xffu) << 20 |
              (uint64_t)(depth & 1u) << 28 | (uint64_t)(restart_eff & 1u) << 29 | (uint64_t)depth_format << 32;
    v->w[5] = nr_draw_gids(gid);
}
// [DOS2] NR-30: a G-buffer draw that carries DM_DOS2_PREPASS's override (depth func LEQUAL + depth write over the game's depth-stencil
// state). gid[3] stays the id of the game's own depth-stencil group; this bit says the override is on top. The override is a function of
// that group alone, so (gid, bit) names the pipeline's depth state exactly; bits 30-31 of w[4] were unused.
#define NR_DRAW_VAR_PP (1ull << 30)
// [DOS2] NR-POSONLY DM_NR_POSONLY=1: the draw binds its mesh's position-only copy (one binding, stride = position size): its own pipeline
#define NR_DRAW_VAR_PO (1ull << 31)

// ---- offset folds ----
// Index buffer: offset `off` into the VkBuffer, index size 1 << isz_log2 (1 = u16, 2 = u32). Bound at *bind_off, drawn with *first.
// fold = 0: bound at `off`, first index 0 (the old path).
static inline void nr_fold_index(uint64_t off, unsigned isz_log2, int fold, uint64_t *bind_off, uint32_t *first) {
    const uint64_t k = fold ? off >> isz_log2 : 0;
    if (!fold || k > 0xffffffffull) { *bind_off = off; *first = 0; return; }
    *bind_off = off - (k << isz_log2); *first = (uint32_t)k;
}
// Vertex streams: binding i (bit i of `used`) at offset off[i] with stride[i]; bit i of `inst` = per-instance rate. The per-vertex
// streams with a stride move back by k vertices each (k = the smallest off[i] / stride[i] over them), the draw's vertexOffset /
// firstVertex grows by k: stream i then fetches vertex v at out[i] + (v + k) * stride[i] = off[i] + v * stride[i]. Streams with stride 0
// (constant inputs) and per-instance streams keep their offset. Returns k (0 = nothing folded: fold off, no per-vertex stream, or
// base + k would leave int32).
static inline uint32_t nr_fold_vertex(unsigned used, unsigned inst, const uint32_t stride[16], const uint64_t off[16], int64_t base, int fold, uint64_t out[16]) {
    uint64_t k = ~0ull;
    for (unsigned m = used; m; m &= m - 1) { const unsigned i = (unsigned)__builtin_ctz(m); out[i] = off[i]; if (!(inst >> i & 1) && stride[i]) { const uint64_t q = off[i] / stride[i]; if (q < k) k = q; } }
    if (!fold || k == ~0ull || !k) return 0;
    if (base + (int64_t)k > 0x7fffffffll || k > 0x7fffffffull) return 0;
    for (unsigned m = used; m; m &= m - 1) { const unsigned i = (unsigned)__builtin_ctz(m); if (!(inst >> i & 1) && stride[i]) out[i] = off[i] - k * stride[i]; }
    return (uint32_t)k;
}

// ---- bind runs: which vertex bindings to (re)bind, as few commands as possible ----
// `changed`: bindings whose (buffer, offset) differ from the bind cache; `used`: bindings of the pipeline (all have a buffer now).
// A run may cover unchanged bindings of the pipeline between two changed ones (binding the same again is harmless), never a binding
// the pipeline does not use (nothing to bind there). Fills first[] / count[] (at most 8 runs: 16 bindings); returns the number of runs.
static inline unsigned nr_vb_runs(unsigned changed, unsigned used, uint8_t first[8], uint8_t count[8]) {
    unsigned n = 0;
    changed &= used;
    while (changed && n < 8) {
        const unsigned lo = (unsigned)__builtin_ctz(changed);
        unsigned hi = lo;
        for (unsigned i = lo + 1; i < 16 && (used >> i & 1); i++) if (changed >> i & 1) hi = i;   // extend over used bindings up to the last changed one
        first[n] = (uint8_t)lo; count[n] = (uint8_t)(hi - lo + 1); n++;
        changed &= ~((2u << hi) - 1u);
    }
    return n;
}

// ---- DM_NR_DRAW=2: the old path's binds against the short path's ----
// One vertex stream: the old path bound (old_off, vertex offset old_base), the short path (new_off, new_base) on the same buffer.
// 1 = every vertex is fetched from the same bytes.
static inline int nr_vb_same(uint64_t old_off, int64_t old_base, uint64_t new_off, int64_t new_base, uint32_t stride, int per_instance) {
    if (per_instance || !stride) return old_off == new_off;
    return (int64_t)old_off + old_base * (int64_t)stride == (int64_t)new_off + new_base * (int64_t)stride;
}
static inline int nr_ib_same(uint64_t old_off, uint64_t new_off, uint32_t new_first, unsigned isz_log2) {
    return old_off == new_off + ((uint64_t)new_first << isz_log2);
}

// ---- NR-12 (DM_NR_UI, docs/dos2/nr/NR-12.md): the UI lane - plain Iggy draw records through the same short path ----
// Iggy's GDraw backend in main (the draw 0x1bf0cd0, the filter / blur entries) calls NVN outside the engine's draw bracket: its draws reach
// the backend as plain draw records with no OP_X_NR_DRAW identity. The backend gives each one whose fragment shader is an Iggy program
// (iggy / fill / blur) a synthetic identity - key id 0, the row of NR-07's direct table no NR-02 key id uses, texture-set id 0, site
// NRD_SITE_UI (NR-02's call sites are < 0xfe) - and asks the short path. The variant words hold everything the pipeline depends on, so
// one row for all UI pipelines is exact (a row is only a bucket; it is a move-to-front list).
enum { NRD_SITE_UI = 0xfe, NRD_ROW_UI = 0 };
// The uiarea step (DM_NB_UIAREA, NB-18b-6) of a draw the short path would record, as the old draw() would run it:
//   NRUA_NONE   DM_NB_UIAREA off: nothing
//   NRUA_SHORT  a pass whose first draw is not Iggy (NR-09: the draw count / DS use; the rectangle only when viewport / scissor moved)
//   NRUA_FULL   an Iggy pass and an Iggy draw (NR-12): the full uiarea::pre - its rectangle joins the pass union (the next frame's seed),
//               fills are counted. It cannot restart the pass: only a pass begun with a smaller area restarts, and that is NRUA_OLD
//   NRUA_OLD    the old path: uiarea does not track this pass as the open one, the pass was begun with a smaller area (a draw outside it
//               restarts the pass), or an engine draw in an Iggy pass (NR-09 left those to the old path)
enum { NRUA_NONE = 0, NRUA_SHORT, NRUA_FULL, NRUA_OLD };
static inline int nr_ui_area(int ua_mode, int continues, int cls_other, int reduced, int ui) {
    if (!ua_mode) return NRUA_NONE;
    if (!continues || reduced) return NRUA_OLD;
    if (cls_other) return NRUA_SHORT;
    return ui ? NRUA_FULL : NRUA_OLD;
}
// Iggy's DrawArrays draws QUADS (GDraw 0x1bf1428: no index buffer, the vertices copied into the constant ring); the old draw() expands
// each into a host u32 list 4q + {0, 1, 2, 0, 2, 3} (xbg_quad_plan, the first vertex goes to vertexOffset) and copies it into the stream
// arena per draw. The list does not depend on the draw: the UI lane binds ONE copy per submission (the bind cache then drops the index
// bind of every later quad draw). nr_quad_pattern writes the first n indices (n a multiple of 6); nr_quad_ok checks a list against it.
static inline void nr_quad_pattern(uint32_t *v, uint32_t n) {
    for (uint32_t q = 0; q < n / 6; q++) { const uint32_t b = q * 4; uint32_t *o = v + 6 * (size_t)q; o[0] = b; o[1] = b + 1; o[2] = b + 2; o[3] = b; o[4] = b + 2; o[5] = b + 3; }
}
static inline int nr_quad_ok(const uint32_t *list, uint32_t n) {
    if (n % 6) return 0;
    for (uint32_t q = 0; q < n / 6; q++) { const uint32_t b = q * 4; const uint32_t *o = list + 6 * (size_t)q;
        if (o[0] != b || o[1] != b + 1 || o[2] != b + 2 || o[3] != b || o[4] != b + 2 || o[5] != b + 3) return 0; }
    return 1;
}
// The pattern's size for a draw of n indices: at least NR_QUAD_MIN, grown by doubling (a submission holds one, made at its first quad draw
// and again only when a longer draw comes). 0 = more than NR_QUAD_MAX indices (the draw takes the old path).
enum { NR_QUAD_MIN = 6 * 1024, NR_QUAD_MAX = 6 * 262144 };
static inline uint32_t nr_quad_size(uint32_t have, uint32_t n) {
    if (n > NR_QUAD_MAX) return 0;
    if (n <= have) return have;
    uint32_t s = have ? have : NR_QUAD_MIN;
    while (s < n) s *= 2;
    return s > NR_QUAD_MAX ? NR_QUAD_MAX : s;
}

// ---- the model: vkCmd* of the per-draw tail on a stream of draws (nr_draw_test.c; the numbers of NR-09.md) ----
// One draw of the stream: its pipeline, descriptor set + dynamic offsets (as one id), index buffer and up to 3 per-vertex streams,
// each (VkBuffer id, byte offset, stride).
typedef struct { uint32_t pipe, set; uint32_t ib_buf; uint64_t ib_off; uint8_t isz_log2, nvb; uint32_t vb_buf[3]; uint64_t vb_off[3]; uint32_t vb_stride[3]; int32_t base; } nr_mdraw;
typedef struct { uint32_t pipe, set, ib_buf, vb_buf[16]; uint64_t ib_off, vb_off[16]; uint8_t it; int have; uint64_t cmds, draws, c_pipe, c_set, c_vb, c_ib; } nr_mcache;
// Commands the tail records for draw d with the bind cache c (updated): fold = 0 the old tail (one vkCmdBindVertexBuffers per stream),
// 1 index fold, 2 vertex fold, 3 both (+ runs). The draw command itself is counted.
static inline unsigned nr_model_draw(nr_mcache *c, const nr_mdraw *d, int fold) {
    unsigned n = 1;
    if (!c->have || c->pipe != d->pipe) { c->pipe = d->pipe; n++; c->c_pipe++; }
    if (!c->have || c->set != d->set) { c->set = d->set; n++; c->c_set++; }
    uint32_t stride[16] = {0}; uint64_t off[16] = {0}, out[16] = {0};
    unsigned used = 0;
    for (unsigned i = 0; i < d->nvb && i < 3; i++) { used |= 1u << i; stride[i] = d->vb_stride[i]; off[i] = d->vb_off[i]; }
    nr_fold_vertex(used, 0, stride, off, d->base, fold & 2, out);
    unsigned changed = 0;
    for (unsigned i = 0; i < d->nvb && i < 3; i++) if (!c->have || c->vb_buf[i] != d->vb_buf[i] || c->vb_off[i] != out[i]) { changed |= 1u << i; c->vb_buf[i] = d->vb_buf[i]; c->vb_off[i] = out[i]; }
    if (fold) { uint8_t f[8], k[8]; const unsigned r = nr_vb_runs(changed, used, f, k); n += r; c->c_vb += r; }
    else { const unsigned r = (unsigned)__builtin_popcount(changed); n += r; c->c_vb += r; }
    uint64_t bo; uint32_t first;
    nr_fold_index(d->ib_off, d->isz_log2, fold & 1, &bo, &first);
    if (!c->have || c->ib_buf != d->ib_buf || c->ib_off != bo || c->it != d->isz_log2) { c->ib_buf = d->ib_buf; c->ib_off = bo; c->it = d->isz_log2; n++; c->c_ib++; }
    c->have = 1; c->cmds += n; c->draws++;
    return n;
}

#ifdef __cplusplus
}
#endif
#endif
