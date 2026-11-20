// [DOS2] NR-SHCACHE: DM_NR_SHCACHE (default 0; docs/dos2/nr/NR-SHCACHE.md) - shadow cascade caching. Pure C for C and C++ (no Vulkan,
// no engine / host types, no environment, no game address): the decisions of the backend (dm/gfx/vulkan/engine/xbg_nr_shc.inc,
// namespace nrshc), tested on the PC by dm/gfx/vulkan/engine/tests/nr_shc_test.c (decisions + a model of the depth test).
//
// What a cascade is. DOS2 renders its sun shadow into one 1536x1536 D16 atlas: one full clear, then one IR pass per cascade (NB-06
// OP_X_NB_PASS with the atlas as its only attachment), each drawing its casters into its own tile (viewport 768x768). A RANGE is the
// records of one such pass (OP_X_NB_PASS .. OP_X_NB_PASS_END). Each draw of a range gets a 64-bit hash of everything its depth
// output depends on (the backend computes it from the decoded state at the top of draw(): shaders, vertex format and the vertex
// buffers it reads, index buffer, draw arguments, raster / depth state, viewport, scissor, alpha test, the CONTENT of every uniform
// range its shaders declare, its texture / sampler handles with the registration revision of each texture).
//
// Why the cache is exact. Every draw of a cached range must be order-free (nr_sort.h nrs_why: depth only, no stencil / colour / storage /
// query / bounds / alpha-to-coverage, depth effect none or min). Then the tile after the range is
//     T = min(c, z of every fragment of every draw)        (c = the clear value of the atlas; min per sample, bit-exact in fixed point)
// and min is commutative and associative, so for any split of the draws into M (a multiset of hashes) and the rest R:
//     T = min(min(c, M), R).
// A LAYER is min(c, M) kept in a separate image S (the same tile position) together with M. In a later range D (the draws of now) with
// M a sub-multiset of D (by hash, each member consumed once), the same clear value c, the same atlas and tile:
//     T(D) = min(S, D \ M)  - copy S into the tile, draw only D \ M (the "dynamic" casters: skinned / wind / moving / new).
// Equal hash => equal fragments (same inputs, same pipeline, same tile; a 64-bit hash: collisions are not excluded, as NR-02 / NR-21).
// [shc4] Bytes the hash takes by address (vertex streams, index ranges, texel buffers) and images it takes by handle carry a CONTENT
// VERSION in the hash (nrshc_wj below: the backend's own writes into guest memory; the backend adds the HLE tracker's page generations for
// pools the game's CPU may write and each image's upload number). Before shc4 bytes rewritten under a known address left the hash unchanged
// (the phone's verify, 18.11: cascade 2 differs in ~1000 bytes). A layer older than DM_NR_SHCACHE_AGE ranges is still rebuilt (default 120).
//
// The state machine of one cascade slot (k = ordinal of the atlas pass in the frame):
//   NORMAL  draw everything; feed the history (hash -> streak = consecutive ranges the hash was present in).
//   BUILD   (no valid layer, the previous range had >= min_static draws with streak >= stable - 1): execution 1 draws only the draws
//           whose streak says "static" (the members) and defers the rest; at the end the backend copies the tile to S (the layer =
//           the members) and executes the range again from its start state drawing only the deferred draws (by ordinal). The frame's
//           tile is min(c, M) then R: exact by the formula above.
//   HIT     (a valid layer, same atlas / tile / clear value, not older than the age): the backend copies S into the tile at the first
//           draw of the range; a draw whose hash is an unconsumed member is skipped (no translation work, no Vulkan, no GPU), every
//           other draw is drawn. At the end every member must have been consumed and no draw may have been tainted, else RECOVER:
//           the tile is cleared to c and the range executed again drawing everything (the frame stays exact; the layer is dropped).
//   A tainted range (a draw that is not order-free, a viewport other than the tile, an op the backend does not allow inside a range)
//   is never cached; a BUILD or HIT that meets one ends in RECOVER.
#ifndef DM_NR_SHC_H
#define DM_NR_SHC_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NRSHC_OFF = 0, NRSHC_ON = 1, NRSHC_CHECK = 2, NRSHC_POISON = 3, NRSHC_MODES };
enum { NRSHC_CAS = 4,          // cascade slots (DOS2 draws 3)
       NRSHC_DRAWS = 4096,     // draws a range may have and still be cached (Fort Joy peak: 3371 over 3 cascades)
       NRSHC_TAB = 8192 };     // hash table slots (power of 2, >= 2 x NRSHC_DRAWS)
enum { NRSHC_A_NORMAL = 0, NRSHC_A_HIT = 1, NRSHC_A_BUILD = 2 };
// why a range was not HIT / BUILD (counted per range; the first reason that applies)
enum { NRSHC_W_OK = 0, NRSHC_W_FIRST,     // nothing known yet about this cascade (first ranges)
       NRSHC_W_CLEAR,      // no full clear of the atlas this frame before the range, or another value than the layer's
       NRSHC_W_DIRTY,      // the atlas was written outside the tiles this frame (a stray viewport, a partial clear), or tiles overlap
       NRSHC_W_RECT,       // the first draw's viewport is not a whole-pixel rectangle inside the atlas
       NRSHC_W_PREV,       // the previous range of this cascade was tainted or could not be executed again
       NRSHC_W_FEW,        // too few static draws (BUILD not worth it)
       NRSHC_W_LAYER,      // a layer exists but not for this atlas / tile / clear value
       NRSHC_W_AGE,        // the layer is older than the age limit (rebuilt next range)
       NRSHC_W_OFF,        // the backend refused the slot (Vulkan objects, re-entrancy, switch interplay)
       NRSHC_W_N };
static inline const char *nrshc_why_name(int w) {
    static const char *const n[NRSHC_W_N] = {"ok", "first", "clear", "dirty atlas", "tile rect", "previous range", "few static", "layer other",
                                             "age", "backend"};
    return w >= 0 && w < NRSHC_W_N ? n[w] : "?";
}
// [shc2] why a range was REFUSED at its first draw (it is never armed: no hash, no decision; counted per range, the first reason that
// applies). Phone 18.11 (C_st_shc / nrB_shc): every range was refused - DOS2 binds its shadow atlas with SetRenderTargets(1, {NULL}, atlas)
// (state.rt.n = 1, colour 0 NULL; NVN read-break lines "colors=0 ... rt_n=1 ... fmt=0x32 1536x1536" in 1016 of 1016 cases) and the old shape
// test asked for rt.n == 0. A NULL colour slot writes nothing: only a BOUND colour target refuses (NRSHC_RF_COLOUR).
enum { NRSHC_RF_OK = 0,
       NRSHC_RF_NODEPTH,   // no depth target at the first draw
       NRSHC_RF_IMAGE,     // the depth target is not the image the PASS record named
       NRSHC_RF_ASPECT,    // the depth image has a stencil aspect (D24S8: the scene pre-pass with the occlusion pairs) or is not depth
       NRSHC_RF_FORMAT,    // not fixed point (D16 / X8_D24 only)
       NRSHC_RF_SHAPE,     // levels / layers / samples != 1
       NRSHC_RF_SMALL,     // < 256 px a side
       NRSHC_RF_COLOUR,    // a colour target is bound (non-NULL)
       NRSHC_RF_OTHER,     // another depth-only image is the atlas (it was used < 60 frames ago)
       NRSHC_RF_SLOTS,     // more than NRSHC_CAS cascades this frame
       NRSHC_RF_OP,        // a record outside the allowed ops before the first draw
       NRSHC_RF_N };
static inline const char *nrshc_rf_name(int w) {
    static const char *const n[NRSHC_RF_N] = {"ok", "no depth", "image", "stencil aspect", "format", "levels/layers/samples", "small", "colour target",
                                              "other atlas", "cascades", "op before draw"};
    return w >= 0 && w < NRSHC_RF_N ? n[w] : "?";
}
// what arm() sees of the target at the first draw of a range
typedef struct {
    int has_depth, same_image, depth_aspect_only, fixed_point, levels, layers, samples;
    uint32_t width, height;
    int colours;          // bound (non-NULL) colour targets among state.rt.color[0 .. rt.n)
    int other_atlas;      // another image is the current atlas and was used < 60 frames ago
    int arms;             // ranges of this frame already armed
} nrshc_shape;
static inline int nrshc_shape_why(const nrshc_shape *s) {
    if (!s->has_depth) return NRSHC_RF_NODEPTH;
    if (!s->same_image) return NRSHC_RF_IMAGE;
    if (!s->depth_aspect_only) return NRSHC_RF_ASPECT;
    if (!s->fixed_point) return NRSHC_RF_FORMAT;
    if (s->levels != 1 || s->layers != 1 || s->samples != 1) return NRSHC_RF_SHAPE;
    if (s->width < 256 || s->height < 256) return NRSHC_RF_SMALL;
    if (s->colours) return NRSHC_RF_COLOUR;
    if (s->other_atlas) return NRSHC_RF_OTHER;
    if (s->arms >= NRSHC_CAS) return NRSHC_RF_SLOTS;
    return NRSHC_RF_OK;
}
// [shc2] the cascade slot of a range: the free slot that last held the same tile (a cascade keeps its slot when an earlier cascade of the
// frame has < 8 draws and is no range, or the pre-pass range comes first), else a free slot never used, else any free slot (its layer is
// then for another tile: nrshc_arm drops it, NRSHC_W_LAYER). used: bit k = slot k armed this frame. -1 = no free slot.
static inline int nrshc_pick_slot(const int32_t rects[NRSHC_CAS][4], const int has[NRSHC_CAS], uint32_t used, const int32_t rect[4]) {
    for (int i = 0; i < NRSHC_CAS; i++) if (!(used >> i & 1) && has[i] && !memcmp(rects[i], rect, 4 * sizeof rect[0])) return i;
    for (int i = 0; i < NRSHC_CAS; i++) if (!(used >> i & 1) && !has[i]) return i;
    for (int i = 0; i < NRSHC_CAS; i++) if (!(used >> i & 1)) return i;
    return -1;
}
// [shc2] why a DRAW tainted its range (the first reason the backend's hash() met; a tainted range is never cached, a HIT / BUILD that
// meets one recovers). NRSHC_T_ORDER carries the nr_sort.h nrs_why() code separately.
enum { NRSHC_T_OK = 0,
       NRSHC_T_INDIRECT,   // indirect arguments (GPU memory)
       NRSHC_T_TARGET,     // the depth target is not the range's atlas
       NRSHC_T_COLOUR,     // a colour target is bound
       NRSHC_T_VIEWPORT,   // the viewport is not the range's tile
       NRSHC_T_VERTEX,     // a vertex attribute / stream slot beyond 16
       NRSHC_T_STORAGE,    // the shaders write storage / declare storage bindings
       NRSHC_T_BINDING,    // a binding source whose reads are not known (images, bindless, slot out of range)
       NRSHC_T_ORDER,      // not order-free (nrs_why: query / condr / stencil / colour / depth function ...)
       NRSHC_T_N };
static inline const char *nrshc_t_name(int w) {
    static const char *const n[NRSHC_T_N] = {"ok", "indirect", "target", "colour", "viewport", "vertex slot", "storage", "binding", "order"};
    return w >= 0 && w < NRSHC_T_N ? n[w] : "?";
}
// the end of a range: what the backend has to do
enum { NRSHC_E_NONE = 0,       // nothing (NORMAL, or a HIT whose members all came)
       NRSHC_E_BUILD = 1,      // copy the tile to S (the layer is kept), then execute the range again drawing the deferred draws
       NRSHC_E_REST = 2,       // execute the range again drawing the deferred draws (BUILD with too few members: no layer)
       NRSHC_E_RECOVER = 3 };  // clear the tile to c, execute the range again drawing every draw (HIT / BUILD that broke)
// a draw while the range executes again
// REST: the ordinals execution 1 skipped (BUILD: the deferred draws); ALL: every draw (RECOVER); DRAWN: the ordinals execution 1 marked
// drawn (the check mode's side runs: a BUILD's members into a cleared tile, a HIT's dynamic draws over the layer)
enum { NRSHC_R_NONE = 0, NRSHC_R_REST = 1, NRSHC_R_ALL = 2, NRSHC_R_DRAWN = 3 };
// [shc4] the channel a draw's content version moved by (diagnostics: "new by content" in the why2 line): the tag packs 10 bits of each
// channel's digest - J the journal (bytes the backend wrote), P the write-protected pages (bytes the game's CPU wrote), T the textures
// (an image upload / a texture registration / samplers)
enum { NRSHC_VC_J = 0, NRSHC_VC_P = 1, NRSHC_VC_T = 2, NRSHC_VC_N = 3 };
static inline uint32_t nrshc_vtag(uint64_t vj, uint64_t vp, uint64_t vt) { return (uint32_t)(vj & 1023) | (uint32_t)(vp & 1023) << 10 | (uint32_t)(vt & 1023) << 20; }

static inline uint64_t nrshc_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29; return h;
}
// the content hash of a uniform range: 4 lanes of 8 bytes, so ~32 bytes per round (the shadow draws' constants are 0.1-3 KB)
static inline uint64_t nrshc_bytes(uint64_t seed, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p; uint64_t a0 = seed ^ 0x243F6A8885A308D3ull, a1 = seed ^ 0x13198A2E03707344ull,
                                                    a2 = seed ^ 0xA4093822299F31D0ull, a3 = seed ^ 0x082EFA98EC4E6C89ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4]; memcpy(v, b + i, 32);
        a0 = (a0 ^ v[0]) * 0x9FB21C651E98DF25ull; a1 = (a1 ^ v[1]) * 0x9FB21C651E98DF25ull;
        a2 = (a2 ^ v[2]) * 0x9FB21C651E98DF25ull; a3 = (a3 ^ v[3]) * 0x9FB21C651E98DF25ull;
        a0 ^= a0 >> 29; a1 ^= a1 >> 29; a2 ^= a2 >> 29; a3 ^= a3 >> 29;
    }
    uint64_t t[4] = {0, 0, 0, 0}; if (i < n) memcpy(t, b + i, n - i);
    a0 ^= t[0]; a1 ^= t[1]; a2 ^= t[2]; a3 ^= t[3];
    uint64_t h = nrshc_mix(nrshc_mix(nrshc_mix(nrshc_mix((uint64_t)n, a0), a1), a2), a3);
    return h;
}

// [shc2] the 16-byte rows (vec4s) in which two uniform ranges of n bytes differ: bit i of *mask = row i (rows 0-63 = bytes 0-1023),
// *beyond = differing rows past byte 1024; returns the number of differing rows (0 = equal). Diagnostics only (the sampled uniform diff).
static inline uint32_t nrshc_rows(const void *a, const void *b, size_t n, uint64_t *mask, uint32_t *beyond) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b; uint32_t rows = 0; *mask = 0; *beyond = 0;
    for (size_t o = 0; o < n; o += 16) {
        const size_t k = n - o < 16 ? n - o : 16;
        if (!memcmp(x + o, y + o, k)) continue;
        ++rows; if (o < 1024) *mask |= 1ull << (o / 16); else ++*beyond;
    }
    return rows;
}

// [shc3] the 4-byte words in which two uniform ranges of n bytes differ, words 0-63 (bytes 0-255; the DOS2 shadow blocks are 128-208 B):
// bit i of the return = word i; d[i] = new - old read as floats (diagnostics only: a camera / cascade shift moves the translation words of
// every caster's matrices by the same amount, a rotation moves whole rows, a per-object animation moves different words per draw).
static inline uint64_t nrshc_words(const void *olda, const void *newb, size_t n, float d[64]) {
    const uint8_t *x = (const uint8_t *)olda, *y = (const uint8_t *)newb; uint64_t m = 0;
    for (size_t w = 0; w < 64 && 4 * w + 4 <= n; w++) {
        if (!memcmp(x + 4 * w, y + 4 * w, 4)) continue;
        float a, b; memcpy(&a, x + 4 * w, 4); memcpy(&b, y + 4 * w, 4); m |= 1ull << w; d[w] = b - a;
    }
    return m;
}
// 1 = the deltas (m, d) are the reference's (rm, rd): the same words moved, each by the same amount (1e-3 relative; NaN = not the same)
static inline int nrshc_same_delta(uint64_t m, const float d[64], uint64_t rm, const float rd[64]) {
    if (m != rm) return 0;
    for (int w = 0; w < 64; w++) if (m >> w & 1) {
        const float r = rd[w], e = d[w] - r, ar = r < 0 ? -r : r, ae = e < 0 ? -e : e;
        if (!(ae <= 1e-3f * (ar > 1.f ? ar : 1.f))) return 0;
    }
    return 1;
}

// ---- a multiset / map of hashes (generation stamped: clear is O(1)) ----
typedef struct {
    uint64_t key[NRSHC_TAB]; uint32_t val[NRSHC_TAB], gen[NRSHC_TAB], use[NRSHC_TAB], ugen[NRSHC_TAB];
    uint32_t g, ug, n;
} nrshc_tab;
static inline void nrshc_tab_clear(nrshc_tab *t) { if (++t->g == 0) { memset(t->gen, 0, sizeof t->gen); t->g = 1; } t->n = 0; if (++t->ug == 0) { memset(t->ugen, 0, sizeof t->ugen); t->ug = 1; } }
static inline void nrshc_tab_unuse(nrshc_tab *t) { if (++t->ug == 0) { memset(t->ugen, 0, sizeof t->ugen); t->ug = 1; } }
// index of k (insert: created with val 0 when missing and room is left; -1 = missing / full)
static inline int nrshc_tab_at(nrshc_tab *t, uint64_t k, int insert) {
    uint32_t i = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 51) & (NRSHC_TAB - 1);
    for (uint32_t probe = 0; probe < NRSHC_TAB; probe++, i = (i + 1) & (NRSHC_TAB - 1)) {
        if (t->gen[i] != t->g) {
            if (!insert || t->n >= NRSHC_TAB / 2) return -1;
            t->gen[i] = t->g; t->key[i] = k; t->val[i] = 0; ++t->n; return (int)i;
        }
        if (t->key[i] == k) return (int)i;
    }
    return -1;
}
static inline uint32_t nrshc_tab_get(const nrshc_tab *t, uint64_t k) { int i = nrshc_tab_at((nrshc_tab *)t, k, 0); return i < 0 ? 0 : t->val[i]; }
// consume one unit of k (multiset member): 1 = there was one left
static inline int nrshc_tab_take(nrshc_tab *t, uint64_t k) {
    int i = nrshc_tab_at(t, k, 0); if (i < 0) return 0;
    if (t->ugen[i] != t->ug) { t->ugen[i] = t->ug; t->use[i] = 0; }
    if (t->use[i] >= t->val[i]) return 0;
    ++t->use[i]; return 1;
}

typedef struct {
    uint32_t stable;      // ranges a draw must have been present in (with this one) to be a member (default 3)
    uint32_t min_static;  // members a layer needs (default 64)
    uint32_t max_age;     // ranges a layer lives at most (default 120)
    uint32_t probe;       // a quiet cascade (moving camera: no static set for 4 ranges) is hashed in 2 ranges of every `probe` (default 8;
                          // 0 = always): the hashing cost of a moving scene drops to 2 / probe, a still scene is seen again within probe ranges
} nrshc_par;
static inline nrshc_par nrshc_par_default(void) { nrshc_par p; p.stable = 3; p.min_static = 64; p.max_age = 120; p.probe = 8; return p; }
enum { NRSHC_QUIET = 4 };   // ranges without a static set before the cascade is only probed

// one cascade slot
typedef struct {
    nrshc_tab hist[2]; int hc;           // hist[hc]: hash -> streak after the previous range; hist[hc ^ 1] is filled by this one
    nrshc_tab mem;                       // the layer's members (hash -> count)
    uint32_t nmem, ranges, layer_ranges; // members; ranges seen; ranges since the layer was built
    int layer;                           // 1 = the layer is valid (S holds min(c, members)); in the check mode it may be virtual
    uint64_t atlas; int32_t rect[4]; uint32_t clear_bits;   // the layer's atlas key, tile, clear value (float bits)
    uint32_t next_cand;                  // draws of the previous range with streak >= stable - 1 after it (BUILD predictor)
    int prev_ok;                         // the previous range was not tainted and could be executed again
    uint32_t quiet;                      // consecutive ranges with fewer than min_static draws that may be members next time (and no hit)
    // [shc2] diagnostics only (no decision reads them): the STRUCTURE hash of each draw of the previous hashed range (everything but the
    // bytes of its uniform ranges) - a draw that was not in the previous range by its full hash but was by its structure changed only in
    // uniform contents (a constant that moves every frame: time, jitter, the cascade matrix).
    nrshc_tab shist[2];                  // indexed like hist (hc)
    int prev_hashed;                     // the previous range of this slot was hashed (not blind, not refused)
    // [shc4] diagnostics only: the hash of each draw WITHOUT its content versions (structure + uniform bytes), val = its version tag
    // (nrshc_vtag): a draw new by its full hash but known by this one kept every state / uniform byte and only its vertex / index / texture
    // content moved - the class of input the phone's verify caught (18.11) that the old hash could not see
    nrshc_tab uhist[2];
} nrshc_cas;

// the range being executed
typedef struct {
    int action, why, live, tainted, overflow, blind;   // blind: a quiet cascade's range that is not hashed (nrshc_draw is not called)
    uint32_t ndraw, nskip, nmem_drawn, matched, cand_next;
    uint32_t known, new_ubo, new_other, unc;   // [shc2] draws found in the previous range by full hash; new, by structure only (uniform bytes
                                               // changed); new otherwise; not compared (the previous range was not hashed)
    uint32_t new_content, nc[NRSHC_VC_N];      // [shc4] new by content only (state and uniform bytes known, a version moved), by channel
    uint64_t h[NRSHC_DRAWS]; uint8_t f[NRSHC_DRAWS];   // per ordinal: hash; 1 = drawn in execution 1 (BUILD member / HIT dynamic), 2 = skipped
    uint64_t v[NRSHC_DRAWS];                   // [shc4] per ordinal: the content version execution 1 mixed into h (execution 2 reuses it: it never
                                               // protects or salts again, so its hashes match execution 1 by construction of the same inputs)
} nrshc_range;
enum { NRSHC_F_DRAWN = 1, NRSHC_F_SKIP = 2 };

// A quiet cascade without a layer is hashed only in ranges probe*k and probe*k+1 (two consecutive ones: enough to see a static set again).
static inline int nrshc_blind(const nrshc_cas *c, const nrshc_par *p) {
    return p->probe >= 2 && !c->layer && c->quiet >= NRSHC_QUIET && (c->ranges % p->probe) >= 2;
}
// The decision at the first draw of a range. clear_ok: a full clear of the atlas with value clear_bits happened this frame before
// the range; dirty: the atlas was written outside the tiles this frame or this tile overlaps another one; rect_ok: the tile is a
// whole-pixel rectangle inside the atlas. Returns the action; *why = the reason it is not HIT / BUILD.
static inline int nrshc_arm(nrshc_cas *c, const nrshc_par *p, uint64_t atlas, const int32_t rect[4], int clear_ok, uint32_t clear_bits,
                            int dirty, int rect_ok, int backend_ok, int *why) {
    *why = NRSHC_W_OK;
    if (!backend_ok) { *why = NRSHC_W_OFF; return NRSHC_A_NORMAL; }
    if (!c->ranges) { *why = NRSHC_W_FIRST; return NRSHC_A_NORMAL; }
    if (nrshc_blind(c, p)) { *why = NRSHC_W_FEW; return NRSHC_A_NORMAL; }
    if (!rect_ok) { *why = NRSHC_W_RECT; return NRSHC_A_NORMAL; }
    if (dirty) { *why = NRSHC_W_DIRTY; return NRSHC_A_NORMAL; }
    if (!clear_ok) { *why = NRSHC_W_CLEAR; return NRSHC_A_NORMAL; }
    if (!c->prev_ok) { *why = NRSHC_W_PREV; return NRSHC_A_NORMAL; }
    if (c->layer) {
        const int same = c->atlas == atlas && !memcmp(c->rect, rect, sizeof c->rect) && c->clear_bits == clear_bits;
        if (same && c->layer_ranges < p->max_age) return NRSHC_A_HIT;
        *why = same ? NRSHC_W_AGE : NRSHC_W_LAYER; c->layer = 0;   // dropped: rebuilt below if the history allows it
    }
    if (c->next_cand < p->min_static) { if (*why == NRSHC_W_OK) *why = NRSHC_W_FEW; return NRSHC_A_NORMAL; }
    return NRSHC_A_BUILD;
}
static inline void nrshc_begin(nrshc_cas *c, const nrshc_par *p, nrshc_range *r, int action, int why) {
    r->action = action; r->why = why; r->live = 1; r->tainted = 0; r->overflow = 0; r->blind = action == NRSHC_A_NORMAL && nrshc_blind(c, p);
    r->ndraw = r->nskip = r->nmem_drawn = r->matched = r->cand_next = 0;
    r->known = r->new_ubo = r->new_other = r->unc = 0; r->new_content = 0; for (int i = 0; i < NRSHC_VC_N; i++) r->nc[i] = 0;
    nrshc_tab_clear(&c->hist[c->hc ^ 1]); nrshc_tab_clear(&c->shist[c->hc ^ 1]); nrshc_tab_clear(&c->uhist[c->hc ^ 1]);
    if (action == NRSHC_A_HIT) nrshc_tab_unuse(&c->mem);
}
// One draw of execution 1 (exec: the backend draws as usual whatever this returns, check mode). Returns 1 = skip the draw.
// hs: the draw's structure hash (h without the uniform contents; diagnostics only, see nrshc_cas.shist).
// [shc4] hn: h without the content versions (diagnostics: nrshc_cas.uhist), vtag: nrshc_vtag of the versions, v: the version mixed into h
// (kept per ordinal for execution 2). A caller without versions passes hn = h, vtag = 0, v = 0.
static inline int nrshc_draw(nrshc_cas *c, const nrshc_par *p, nrshc_range *r, uint64_t h, uint64_t hs, uint64_t hn, uint32_t vtag, uint64_t v, int order_free) {
    if (!order_free) r->tainted = 1;
    const uint32_t streak = nrshc_tab_get(&c->hist[c->hc], h);
    if (!c->prev_hashed) ++r->unc;
    else if (streak) ++r->known;
    else { const int u = nrshc_tab_at(&c->uhist[c->hc], hn, 0);
        if (u >= 0) { ++r->new_content; const uint32_t o = c->uhist[c->hc].val[u];
            for (int k = 0; k < NRSHC_VC_N; k++) if (((o ^ vtag) >> (10 * k)) & 1023) ++r->nc[k]; }
        else if (nrshc_tab_get(&c->shist[c->hc], hs)) ++r->new_ubo;
        else ++r->new_other; }
    { const int j = nrshc_tab_at(&c->shist[c->hc ^ 1], hs, 1); if (j >= 0) c->shist[c->hc ^ 1].val[j] = 1; }
    { const int j = nrshc_tab_at(&c->uhist[c->hc ^ 1], hn, 1); if (j >= 0) c->uhist[c->hc ^ 1].val[j] = vtag; }
    { const int i = nrshc_tab_at(&c->hist[c->hc ^ 1], h, 1); if (i >= 0 && c->hist[c->hc ^ 1].val[i] == 0) c->hist[c->hc ^ 1].val[i] = streak + 1; }
    if (streak + 2 >= p->stable) ++r->cand_next;   // a member of the next range if it comes again (streak after it >= stable)
    if (r->ndraw >= NRSHC_DRAWS) { r->overflow = 1; ++r->ndraw; return 0; }
    const uint32_t o = r->ndraw++;
    r->h[o] = h; r->f[o] = NRSHC_F_DRAWN; r->v[o] = v;
    if (r->action == NRSHC_A_HIT) {
        if (!r->tainted && nrshc_tab_take(&c->mem, h)) { r->f[o] = NRSHC_F_SKIP; ++r->nskip; ++r->matched; return 1; }
        return 0;
    }
    if (r->action == NRSHC_A_BUILD) {
        if (streak + 1 >= p->stable) { ++r->nmem_drawn; return 0; }   // a member (present in `stable` ranges with this one): drawn now
        r->f[o] = NRSHC_F_SKIP; ++r->nskip; return 1;                                     // deferred to execution 2
    }
    return 0;
}
// The end of execution 1. simple: the range can be executed again (it lies in one command handle, the backend has its start state).
// Returns NRSHC_E_*; for BUILD the layer's members are taken from the ordinals drawn in execution 1.
static inline int nrshc_end(nrshc_cas *c, const nrshc_par *p, nrshc_range *r, uint64_t atlas, const int32_t rect[4], uint32_t clear_bits,
                            int simple) {
    int e = NRSHC_E_NONE;
    const int broken = r->tainted || r->overflow;
    if (r->action == NRSHC_A_HIT) {
        if (broken || r->matched != c->nmem) { c->layer = 0; e = NRSHC_E_RECOVER; }
        else ++c->layer_ranges;
    } else if (r->action == NRSHC_A_BUILD) {
        if (broken) { c->layer = 0; e = NRSHC_E_RECOVER; }
        else if (r->nmem_drawn >= p->min_static) {
            nrshc_tab_clear(&c->mem); c->nmem = 0;
            for (uint32_t i = 0; i < r->ndraw; i++) if (r->f[i] == NRSHC_F_DRAWN) { const int j = nrshc_tab_at(&c->mem, r->h[i], 1); if (j >= 0) { ++c->mem.val[j]; ++c->nmem; } }
            if (c->nmem != r->nmem_drawn) { c->layer = 0; e = r->nskip ? NRSHC_E_REST : NRSHC_E_NONE; }   // the table was full: no layer
            else { c->layer = 1; c->layer_ranges = 0; c->atlas = atlas; memcpy(c->rect, rect, sizeof c->rect); c->clear_bits = clear_bits; e = NRSHC_E_BUILD; }
        } else { c->layer = 0; e = r->nskip ? NRSHC_E_REST : NRSHC_E_NONE; }
    }
    if (e != NRSHC_E_NONE && !simple) e = -1;   // execution 2 is impossible: the caller turns the switch off (red line); arm() never starts
                                                // a HIT / BUILD after a range that was not simple, so this needs a range that changed shape
    c->prev_ok = !broken && simple;
    c->next_cand = broken || r->blind ? 0 : r->cand_next;
    if (r->action == NRSHC_A_HIT && e == NRSHC_E_NONE) c->quiet = 0;
    else if (c->next_cand < p->min_static) ++c->quiet; else c->quiet = 0;
    c->prev_hashed = !r->blind;
    c->hc ^= 1; ++c->ranges; r->live = 0;
    return e;
}
// Execution 2 (the range again from its start state): 1 = draw this ordinal. bad: the hash differs from execution 1 (a desync).
// [shc3] The backend must SEE the draws of execution 2: its draw() hook is gated by `live`, and the phone's first BUILD (18.11, C_st_shc1 /
// shc2 / shc3: "execution 2 met 0 draws, execution 1 had 206 / 257") ran execution 2 with the hook closed (pass_end had cleared `live`) -
// every draw was drawn unhashed and uncounted, the ordinal stayed 0 and the switch went off. again_run() now opens the hook around it.
// [shc4] the content version execution 1 mixed into the hash of ordinal ord (0 past the range: such a draw is drawn either way)
static inline uint64_t nrshc_again_v(const nrshc_range *r, uint32_t ord) { return ord < r->ndraw && ord < NRSHC_DRAWS ? r->v[ord] : 0; }
static inline int nrshc_again(const nrshc_range *r, int how, uint32_t ord, uint64_t h, int *bad) {
    *bad = 0;
    if (ord >= NRSHC_DRAWS) { *bad = how != NRSHC_R_ALL; return 1; }   // only RECOVER (ALL) runs a range that overflowed
    if (ord >= r->ndraw || r->h[ord] != h) *bad = 1;
    switch (how) {
    case NRSHC_R_REST: return r->f[ord] == NRSHC_F_SKIP;
    case NRSHC_R_DRAWN: return r->f[ord] == NRSHC_F_DRAWN;
    default: return 1;
    }
}
// [shc3] The uniform updates of an open range (OP_UPDATE_UB: the backend writes the record's bytes into memory when it executes the record,
// as the GPU does on the console). Execution 2 runs the range again from its start, so a draw that came BEFORE an update would read the
// bytes AFTER it (the memory still holds the end of execution 1): its hash differs (a desync) and it draws with the wrong constants. So each
// update's old bytes are noted before the record runs (execution 1 only), and before execution 2 they are written back in reverse order:
// memory is then what it was at the range's start and execution 2 replays the same updates in the same order - every draw reads what it
// read in execution 1 (tested: nr_shc_test "updates"). A log that is full makes the range not executable again (as one that crosses a handle).
enum { NRSHC_UNDO_N = 1024, NRSHC_UNDO_BYTES = 256 * 1024 };
typedef struct { uint64_t addr; uint32_t len, off; } nrshc_undo_e;
typedef struct { nrshc_undo_e e[NRSHC_UNDO_N]; uint32_t n, used; int over; uint8_t b[NRSHC_UNDO_BYTES]; } nrshc_undo;
static inline void nrshc_undo_reset(nrshc_undo *u) { u->n = 0; u->used = 0; u->over = 0; }
// cur = the len bytes at addr now (before the update is applied); 0 = not noted (the log is full: u->over is set)
static inline int nrshc_undo_note(nrshc_undo *u, uint64_t addr, const void *cur, uint32_t len) {
    if (u->over || u->n >= NRSHC_UNDO_N || len > NRSHC_UNDO_BYTES - u->used) { u->over = 1; return 0; }
    nrshc_undo_e *e = &u->e[u->n++]; e->addr = addr; e->len = len; e->off = u->used;
    memcpy(u->b + u->used, cur, len); u->used += len; return 1;
}
// entry i of the restore order (reverse of the updates): the address, length and the old bytes to write back
static inline const uint8_t *nrshc_undo_back(const nrshc_undo *u, uint32_t i, uint64_t *addr, uint32_t *len) {
    const nrshc_undo_e *e = &u->e[u->n - 1 - i]; *addr = e->addr; *len = e->len; return u->b + e->off;
}

// [shc4] THE WRITE JOURNAL: a version of guest byte ranges, for the inputs the draw hash cannot read byte by byte (vertex streams, index
// ranges, texel buffers). Phone 18.11 (C_s4_shc2 / C_m4_shc2): `verify ... cascade 2 ... differs in ~1000 bytes` while every draw hashed as
// known - the hash took a vertex / index buffer by its ADDRESS, so bytes rewritten under the same address (an OP_COPY_B2B upload, a GPU
// writeback, an in-stream fill) left the hash unchanged and the layer kept the old shape. The backend calls nrshc_wj_write for every byte
// range it writes into guest memory (snapshot_invalidate: replay CPU writes, GPU writebacks, reports, events) and for every pool that comes or
// goes; a draw mixes nrshc_wj_ver of each range it reads into its hash.
// Slots: a page (4 KB) slot and a block (64 KB) slot per address hash, each holding the serial of the newest write that touched a page / block
// mapping to it (serials only grow). The version of a range = the max over the slots it covers: the page slots of the pages in a partly
// covered block, the block slot of a fully covered block. Why it is a version (tested: test_journal): a write W with serial s touching the
// range sets the slot of every page and block it touches to s, and s is larger than every slot before; a later query of the range reads at
// least one of those slots, so its version is >= s > every earlier version of the range. A version changes ONLY when some slot it reads
// changes, i.e. a write touched a page / block of the range or one that shares its slot (a false change: the draw is drawn, never wrong).
enum { NRSHC_WJ_PG = 1 << 16, NRSHC_WJ_BK = 1 << 12, NRSHC_WJ_BSH = 4 };   // page slots; block slots; pages a block = 1 << BSH (64 KB)
typedef struct { uint64_t serial, writes, bytes; uint64_t pg[NRSHC_WJ_PG], bk[NRSHC_WJ_BK]; } nrshc_wj;
static inline uint32_t nrshc_wj_ps(uint64_t page) { return (uint32_t)((page * 0x9E3779B97F4A7C15ull) >> 40) & (NRSHC_WJ_PG - 1); }
static inline uint32_t nrshc_wj_bs(uint64_t blk) { return (uint32_t)((blk * 0xC2B2AE3D27D4EB4Full) >> 40) & (NRSHC_WJ_BK - 1); }
static inline void nrshc_wj_write(nrshc_wj *j, uint64_t a, uint64_t n) {
    if (!n) return;
    const uint64_t s = ++j->serial; ++j->writes; j->bytes += n;
    const uint64_t p0 = a >> 12, p1 = (a + n - 1) >> 12, b0 = p0 >> NRSHC_WJ_BSH, b1 = p1 >> NRSHC_WJ_BSH;
    if (p1 - p0 >= NRSHC_WJ_PG) { for (uint32_t i = 0; i < NRSHC_WJ_PG; i++) j->pg[i] = s; }
    else for (uint64_t p = p0; p <= p1; p++) j->pg[nrshc_wj_ps(p)] = s;
    if (b1 - b0 >= NRSHC_WJ_BK) { for (uint32_t i = 0; i < NRSHC_WJ_BK; i++) j->bk[i] = s; }
    else for (uint64_t b = b0; b <= b1; b++) j->bk[nrshc_wj_bs(b)] = s;
}
static inline uint64_t nrshc_wj_ver(const nrshc_wj *j, uint64_t a, uint64_t n) {
    if (!n) return 0;
    const uint64_t p0 = a >> 12, p1 = (a + n - 1) >> 12, b0 = p0 >> NRSHC_WJ_BSH, b1 = p1 >> NRSHC_WJ_BSH; uint64_t v = 0;
    for (uint64_t b = b0; b <= b1; b++) {
        const uint64_t lo = b << NRSHC_WJ_BSH, hi = lo + (1u << NRSHC_WJ_BSH) - 1;
        if (p0 <= lo && hi <= p1) { const uint64_t x = j->bk[nrshc_wj_bs(b)]; if (x > v) v = x; continue; }
        for (uint64_t p = p0 > lo ? p0 : lo, e = p1 < hi ? p1 : hi; p <= e; p++) { const uint64_t x = j->pg[nrshc_wj_ps(p)]; if (x > v) v = x; }
    }
    return v;
}
// [NR-ZERO] DM_NR_SHC_MEMO (docs/dos2/nr/NR-ZERO.md): what the draw hash looks up again and again, kept between draws - EXACT, it only
// remembers answers that cannot change while their stamp holds.
// (1) nrshc_pmemo: (table, id) -> the table's node for id. The backend's texture registration tables are std::map whose nodes are never
//     erased at run time (only at shutdown, which bumps the stamp `gen` the backend passes: images_epoch), so a node found once stays the
//     node of that id; only FOUND answers are stored (a missing id may be registered later). Direct mapped, 4096 slots, no allocation.
// (2) the vertex / index ranges' page versions: nvn_dirty_version_tok's token (the tracker's slot epoch, nvn_dirty.c) - the backend keeps
//     (g, inc, token) per range in its VMemo and asks nvn_dirty_tok_ok instead of scanning the range's pages.
enum { NRSHC_PM = 4096 };
typedef struct { const void *map; const void *node; uint64_t gen; int32_t id; uint32_t pad; } nrshc_pment;
typedef struct { nrshc_pment e[NRSHC_PM]; } nrshc_pmemo;
static inline uint32_t nrshc_pm_slot(const void *map, int32_t id) {
    const uint64_t k = ((uint64_t)(uintptr_t)map >> 4) * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)id * 0xC2B2AE3D27D4EB4Full;
    return (uint32_t)(k >> 52) & (NRSHC_PM - 1);
}
// the node stored for (map, id) under stamp gen, or NULL (never stored / another key in the slot / an older stamp)
static inline const void *nrshc_pm_get(const nrshc_pmemo *m, const void *map, int32_t id, uint64_t gen) {
    const nrshc_pment *e = &m->e[nrshc_pm_slot(map, id)];
    return e->gen == gen && e->map == map && e->id == id ? e->node : NULL;
}
static inline void nrshc_pm_put(nrshc_pmemo *m, const void *map, int32_t id, uint64_t gen, const void *node) {
    if (!node) return;   // only found answers
    nrshc_pment *e = &m->e[nrshc_pm_slot(map, id)]; e->map = map; e->id = id; e->gen = gen; e->node = node;
}
static inline nrshc_cas *nrshc_cas_new(void) {
    nrshc_cas *c = (nrshc_cas *)calloc(1, sizeof(nrshc_cas));
    if (c) { c->hist[0].g = c->hist[1].g = c->mem.g = c->shist[0].g = c->shist[1].g = c->uhist[0].g = c->uhist[1].g = 1;
             c->hist[0].ug = c->hist[1].ug = c->mem.ug = c->shist[0].ug = c->shist[1].ug = c->uhist[0].ug = c->uhist[1].ug = 1; }
    return c;
}

#ifdef __cplusplus
}
#endif
#endif
