// [DOS2] NR-21: DM_NR_SORT (default 0; docs/dos2/nr/NR-21.md) - the draws of a depth-only segment re-ordered by pipeline. Pure C for C
// and C++ (no Vulkan / engine / host types, no environment, no game address): the decisions of the backend's queue (dm/gfx/vulkan/engine/
// xbg_nr_sort.inc, namespace nrsort), tested on the PC by dm/gfx/vulkan/engine/tests/nr_sort_test.c.
//
//   the condition   nrs_why: is this draw order-free? (the exact list and the proof are in NR-21.md section 2 and in the comment below)
//   the order       nrs_order: a segment's emission order - sorted (by first appearance of key 1, then key 2, then key 3, then the
//                   original index: a stable counting sort, O(n)), reversed (mode 3, the proof run) or as recorded (mode 2)
//   the model       nrs_count_one: the binds the backend's bind cache lets through for one draw (the same rules as xbg_nr_draw.inc
//                   lane() and the old tail of draw()); summed over an order it gives what a segment costs in that order
//   the checks      nrs_perm_ok / nrs_stable_ok: the order is a permutation of the segment and keeps the recorded order among equal keys
//
// Why the order of these draws cannot change the result (bit for bit). A segment is a run of draws recorded one after the other into
// one render pass instance of one command buffer with no other command between them (any other command ends the segment first). Every
// draw of it satisfies nrs_why() == NRS_OK:
//   (a) it writes no colour (no colour target bound, or every bound target's channel mask is 0), no stencil (stencil test off: Vulkan
//       writes stencil only through the stencil ops of an enabled test), no storage (no storage image / buffer binding in any stage, no
//       storage writeback), and no query counts it (no occlusion query open, no samples counting) - so the depth attachment is the only
//       thing it can change, and nothing observes the order;
//   (b) its depth effect is either none (depth test off - Vulkan then never writes depth -, depth write off, or the NEVER function) or
//       "min": test on, write on, LESS or LEQUAL. For one sample with stored value d and a fragment value z the write gives
//       LESS: z < d ? z : d,  LEQUAL: z <= d ? z : d;  both are min(d, z) as numbers, and equal numbers have equal bits in a fixed-point
//       attachment (D16 / X8_D24 / D24S8 / D16S8 - the format check; a float attachment has +0 == -0 with different bits, refused).
//       min is commutative and associative, so after the segment every sample holds min(d0, all z of its fragments) whatever the order:
//       the same bits. Mixing LESS and LEQUAL draws in one segment changes nothing (both are min); a NaN z never passes either test;
//   (c) z, the coverage and the discard of a fragment depend only on the draw itself (vertices, uniforms, textures, pipeline, viewport,
//       scissor - all carried by the queued draw), never on the attachment: no depth bounds test (it reads the stored depth), no depth
//       feedback read of the open pass's depth image (the backend's DM_GFX_DEPTH_FEEDBACK path - counted, refused), no alpha to
//       coverage (its coverage from a missing colour output is not defined). Alpha test / discard, depth bias, depth clamp, sample masks,
//       gl_FragDepth and rasterizer discard are per fragment and allowed;
//   (d) the conditional render predicate of the backend (DM_GFX_COND_RENDER) is not in force (refused, so no predicate read moves).
// Within one draw the primitive order is the API's and is kept: only whole draws move.
#ifndef DM_NR_SORT_H
#define DM_NR_SORT_H
#include <stdint.h>
#include <string.h>
#include "nr_draw.h"
#ifdef __cplusplus
extern "C" {
#endif

// ---- why a draw nr_draw served does not join a segment (it is recorded at once, after the segment it ends) ----
enum { NRS_OK = 0,
       NRS_W_GATE,       // the in-process A/B "nrsort" is in its off phase
       NRS_W_QUERY,      // an occlusion query is open on the command buffer, or the draw counts samples
       NRS_W_CONDR,      // a conditional-render condition of the backend is in force
       NRS_W_FEEDBACK,   // a shader of the draw reads the depth attachment of the open pass (depth feedback)
       NRS_W_STORAGE,    // a storage image / buffer binding in a stage, storage images bound, or storage writeback
       NRS_W_NODEPTH,    // the pass has no depth attachment
       NRS_W_FORMAT,     // the depth attachment is not fixed point (D32F, D32F_S8)
       NRS_W_COLOR,      // a bound colour target with a non-zero channel mask
       NRS_W_STENCIL,    // stencil test on
       NRS_W_BOUNDS,     // depth bounds test on
       NRS_W_A2C,        // alpha to coverage on
       NRS_W_DEPTHFN,    // depth test + write with a function other than LESS / LEQUAL / NEVER
       NRS_W_N };
static inline const char *nrs_why_name(int w) {
    static const char *const n[NRS_W_N] = {"order-free", "A/B off", "occlusion query / counting", "conditional render", "depth feedback", "storage",
                                           "no depth attachment", "float depth", "colour written", "stencil", "depth bounds", "alpha to coverage",
                                           "depth function"};
    return w >= 0 && w < NRS_W_N ? n[w] : "?";
}

// One draw as the condition sees it. op: the VkCompareOp of the depth test (0 NEVER, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER, 5 NOT_EQUAL,
// 6 GEQUAL, 7 ALWAYS) - NVN's function minus 1.
typedef struct {
    uint8_t ab_off, query, condr, feedback, storage;
    uint8_t depth_att, depth_fixed, color_written, st, bounds, a2c;
    uint8_t dt, dw, op;
} nrs_in;
enum { NRS_EFFECT_NONE = 0, NRS_EFFECT_MIN = 1 };
// The depth effect of an order-free draw: none, or min(d, z).
static inline int nrs_effect(const nrs_in *d) { return d->dt && d->dw && (d->op == 1 || d->op == 3) ? NRS_EFFECT_MIN : NRS_EFFECT_NONE; }
static inline int nrs_why(const nrs_in *d) {
    if (d->ab_off) return NRS_W_GATE;
    if (d->query) return NRS_W_QUERY;
    if (d->condr) return NRS_W_CONDR;
    if (d->feedback) return NRS_W_FEEDBACK;
    if (d->storage) return NRS_W_STORAGE;
    if (!d->depth_att) return NRS_W_NODEPTH;
    if (!d->depth_fixed) return NRS_W_FORMAT;
    if (d->color_written) return NRS_W_COLOR;
    if (d->st) return NRS_W_STENCIL;
    if (d->bounds) return NRS_W_BOUNDS;
    if (d->a2c) return NRS_W_A2C;
    if (d->dt && d->dw && !(d->op == 0 || d->op == 1 || d->op == 3)) return NRS_W_DEPTHFN;
    return NRS_OK;
}
// The same condition on what a pipeline was created with (DM_NR_SORT=2 checks the queued draws' pipelines against it): stencil test,
// depth bounds test, alpha to coverage, every colour attachment's write mask, depth test / write / function. 1 = order-free.
static inline int nrs_pipe_ok(int has_ds, int dt, int dw, int op, int st, int bounds, int a2c, unsigned color_masks_or) {
    if (color_masks_or) return 0;
    if (!has_ds) return 1;
    if (st || bounds || a2c) return 0;
    return !(dt && dw) || op == 0 || op == 1 || op == 3;
}

// ---- the emission order of a segment ----
#define NRS_CAP 4096u
enum { NRS_ORDER_RECORDED = 0, NRS_ORDER_SORTED = 1, NRS_ORDER_REVERSED = 2 };
typedef struct {
    uint32_t id[3][NRS_CAP], tmp[NRS_CAP], cnt[NRS_CAP + 1];
    uint64_t hk[2 * NRS_CAP]; uint32_t hv[2 * NRS_CAP], hg[2 * NRS_CAP], gen;
} nrs_scratch;
// id[i] = the rank of k[i]'s first appearance in k[0..n): 0 for the first distinct value, 1 for the next one... Returns the count.
static inline uint32_t nrs_ids(nrs_scratch *s, const uint64_t *k, uint32_t n, uint32_t *id) {
    const uint32_t mask = 2 * NRS_CAP - 1;
    if (++s->gen == 0) { memset(s->hg, 0, sizeof s->hg); s->gen = 1; }
    uint32_t next = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h = (uint32_t)((k[i] * 0x9E3779B97F4A7C15ull) >> 40) & mask;
        for (;;) {
            if (s->hg[h] != s->gen) { s->hg[h] = s->gen; s->hk[h] = k[i]; s->hv[h] = next++; id[i] = s->hv[h]; break; }
            if (s->hk[h] == k[i]) { id[i] = s->hv[h]; break; }
            h = (h + 1) & mask;
        }
    }
    return next;
}
// Stable counting sort of perm[0..n) by id[perm[j]] (m distinct ids).
static inline void nrs_csort(nrs_scratch *s, uint32_t *perm, uint32_t n, const uint32_t *id, uint32_t m) {
    memset(s->cnt, 0, (size_t)(m + 1) * sizeof s->cnt[0]);
    for (uint32_t j = 0; j < n; j++) s->cnt[id[perm[j]] + 1]++;
    for (uint32_t g = 0; g < m; g++) s->cnt[g + 1] += s->cnt[g];
    for (uint32_t j = 0; j < n; j++) s->tmp[s->cnt[id[perm[j]]]++] = perm[j];
    memcpy(perm, s->tmp, (size_t)n * sizeof perm[0]);
}
// perm[j] = the index of the draw emitted j-th. n <= NRS_CAP. SORTED: by (first appearance of k1, of k2, of k3, recorded index).
static inline void nrs_order(nrs_scratch *s, uint32_t n, int how, const uint64_t *k1, const uint64_t *k2, const uint64_t *k3, uint32_t *perm) {
    if (n > NRS_CAP) n = NRS_CAP;
    for (uint32_t i = 0; i < n; i++) perm[i] = how == NRS_ORDER_REVERSED ? n - 1 - i : i;
    if (how != NRS_ORDER_SORTED || n < 2) return;
    const uint64_t *k[3] = {k1, k2, k3};
    uint32_t m[3];
    for (int c = 0; c < 3; c++) m[c] = k[c] ? nrs_ids(s, k[c], n, s->id[c]) : 0;
    for (int c = 2; c >= 0; c--) if (m[c] > 1) nrs_csort(s, perm, n, s->id[c], m[c]);   // LSD: the last key first, stable
}
// The order is a permutation of [0, n).
static inline int nrs_perm_ok(const uint32_t *perm, uint32_t n, uint8_t *seen) {
    memset(seen, 0, n);
    for (uint32_t j = 0; j < n; j++) { if (perm[j] >= n || seen[perm[j]]) return 0; seen[perm[j]] = 1; }
    return 1;
}
// Sorted order: non-decreasing in (id1, id2, id3) and, among equal keys, in the recorded index (s->id as nrs_order left it).
static inline int nrs_stable_ok(const nrs_scratch *s, const uint32_t *perm, uint32_t n, int nkeys) {
    for (uint32_t j = 1; j < n; j++) {
        const uint32_t a = perm[j - 1], b = perm[j];
        int c = 0;
        for (int q = 0; q < nkeys && !c; q++) c = s->id[q][a] < s->id[q][b] ? -1 : s->id[q][a] > s->id[q][b] ? 1 : 0;
        if (c > 0 || (c == 0 && a > b)) return 0;
    }
    return 1;
}

// ---- the bind model: what the bind cache lets through for one draw ----
// Every value is a non-zero digest of what the backend compares (pipeline handle; layout + set + dynamic offsets; viewport; scissor;
// buffer + offset of each vertex stream; index buffer + offset + type); 0 = not known (the cache was reset: the next draw binds).
typedef struct { uint64_t pipe, set, vp, sc, ib; uint64_t vb[16]; uint16_t used; uint8_t indexed; } nrs_bind;
typedef struct { uint64_t pipe, set, vp, sc, ib; uint64_t vb[16]; } nrs_bstate;
typedef struct { uint64_t pipe, set, vp, sc, vb, ib, cmds, draws; } nrs_count;
static inline uint64_t nrs_mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdull; return h ^ (h >> 33); }
static inline unsigned nrs_count_one(nrs_bstate *st, const nrs_bind *b, nrs_count *c) {
    unsigned n = 1;
    if (st->pipe != b->pipe || !b->pipe) { st->pipe = b->pipe; n++; c->pipe++; }
    if (st->set != b->set || !b->set) { st->set = b->set; n++; c->set++; }
    if (st->vp != b->vp || !b->vp) { st->vp = b->vp; n++; c->vp++; }
    if (st->sc != b->sc || !b->sc) { st->sc = b->sc; n++; c->sc++; }
    unsigned changed = 0;
    for (unsigned i = 0; i < 16; i++) if ((b->used >> i & 1) && (st->vb[i] != b->vb[i] || !b->vb[i])) { changed |= 1u << i; st->vb[i] = b->vb[i]; }
    if (changed) { uint8_t f[8], k[8]; const unsigned r = nr_vb_runs(changed, b->used, f, k); n += r; c->vb += r; }
    if (b->indexed && (st->ib != b->ib || !b->ib)) { st->ib = b->ib; n++; c->ib++; }
    c->cmds += n; c->draws++;
    return n;
}
static inline void nrs_count_order(const nrs_bstate *init, const nrs_bind *b, const uint32_t *perm, uint32_t n, nrs_count *c) {
    nrs_bstate st = *init;
    for (uint32_t j = 0; j < n; j++) nrs_count_one(&st, &b[perm ? perm[j] : j], c);
}
static inline void nrs_count_add(nrs_count *a, const nrs_count *b) {
    a->pipe += b->pipe; a->set += b->set; a->vp += b->vp; a->sc += b->sc; a->vb += b->vb; a->ib += b->ib; a->cmds += b->cmds; a->draws += b->draws;
}

#ifdef __cplusplus
}
#endif
#endif
