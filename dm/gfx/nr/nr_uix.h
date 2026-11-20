// [DOS2] NR-UIX: Iggy UI pass work (docs/dos2/nr/NR-UIX.md). Pure C, inline, no Vulkan: the backend (xbg_nr_uix.inc, xbg_nb_uiarea.inc)
// and the host test tests/nr_uix_test.cpp call the same code.
//
//   ordinal keys   the n-th Iggy pass start with the same first-draw state in a frame (render targets, fragment shader, scissor, viewport)
//                  gets its own history entry: key2 = mix(key, n). DM_NB_UIAREA keyed by the state alone, so the 14..120 filter passes
//                  of a frame that share one ping-pong target, the fill shader and a full scissor shared ONE entry: one pass of them that
//                  used depth / stencil (one re-attach, ~1 in 500 frames) turned the depth-stencil detach off for all of them for good
//                  (play-1118: 14.0 of 14.0 detached on the deck, 0.09 of 23.5 after the first re-attach in Fort Joy).
//   ds decay       an entry keeps its depth-stencil only while a pass of that key used it in the last `decay` frames
//   bbox           the pixel box of a draw from its vertex positions (xbg_spirv_pos programs run on the CPU): Iggy sets viewport and
//                  scissor to the whole 2400x1080 target and draws small quads, so viewport ∩ scissor says nothing
//   cache hash     FNV-1a 64 over the UI command stream of a frame (counting mode of the UI cache)
#ifndef DM_GFX_NR_UIX_H
#define DM_GFX_NR_UIX_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../nb/nb_uiarea.h"
#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t nr_uix_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    return h ? h : 1;
}
static inline uint64_t nr_uix_key2(uint64_t key, uint32_t ord) { return nr_uix_mix(key, 0x51ed270b0ull + ord); }

// ---- occurrences of a key in the current frame
#define NR_UIX_ORD 256   // power of two; more distinct pass starts than that in a frame: the rest share ordinal 255 (still correct, coarser)
typedef struct { uint64_t key; uint32_t n, gen; } nr_uix_oent;
typedef struct { nr_uix_oent e[NR_UIX_ORD]; uint32_t gen; } nr_uix_ord;
static inline void nr_uix_ord_reset(nr_uix_ord *o) { memset(o, 0, sizeof *o); o->gen = 1; }
static inline void nr_uix_ord_frame(nr_uix_ord *o) { if (!++o->gen) nr_uix_ord_reset(o); }
static inline nr_uix_oent *nr_uix_ord_slot(nr_uix_ord *o, uint64_t key, int make) {
    for (uint32_t i = 0, j = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 56) & (NR_UIX_ORD - 1); i < 32; i++, j = (j + 1) & (NR_UIX_ORD - 1)) {
        nr_uix_oent *e = &o->e[j];
        if (e->gen != o->gen) { if (!make) return 0; e->gen = o->gen; e->key = key; e->n = 0; return e; }
        if (e->key == key) return e;
    }
    return 0;
}
// the ordinal the next pass start of `key` gets (peek: create_pass asks before draw() counts it)
static inline uint32_t nr_uix_ord_peek(nr_uix_ord *o, uint64_t key) { const nr_uix_oent *e = nr_uix_ord_slot(o, key, 0); return e ? e->n : 0; }
static inline uint32_t nr_uix_ord_take(nr_uix_ord *o, uint64_t key) {
    nr_uix_oent *e = nr_uix_ord_slot(o, key, 1);
    if (!e) return 255;
    const uint32_t n = e->n < 255 ? e->n : 255; e->n++; return n;
}

// ---- depth-stencil prediction: keep it attached? (entry ds set and refreshed within `decay` frames)
static inline int nr_uix_ds_keep(const nb_ua_hent *e, uint32_t frame, uint32_t decay) {
    return e && e->ds && (uint32_t)(frame - e->frame_ds) < decay;
}

// ---- pixel box of a draw from its clip-space positions
typedef struct { double x0, y0, x1, y1; uint32_t n; int bad; } nr_uix_bb;
static inline void nr_uix_bb_init(nr_uix_bb *b) { b->x0 = b->y0 = 1e30; b->x1 = b->y1 = -1e30; b->n = 0; b->bad = 0; }
// pos: x y z w; vx..vh: the Vulkan viewport (height may be negative). A vertex with w <= 0 (the primitive crosses the eye plane: its
// projection is unbounded) or a non-finite value makes the box unusable.
static inline void nr_uix_bb_add(nr_uix_bb *b, const float pos[4], float vx, float vy, float vw, float vh) {
    const double w = pos[3];
    if (!(w > 0) || !isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(w)) { b->bad = 1; return; }
    const double px = vx + (pos[0] / w * 0.5 + 0.5) * vw, py = vy + (pos[1] / w * 0.5 + 0.5) * vh;
    if (!isfinite(px) || !isfinite(py)) { b->bad = 1; return; }
    if (px < b->x0) b->x0 = px; if (px > b->x1) b->x1 = px;
    if (py < b->y0) b->y0 = py; if (py > b->y1) b->y1 = py;
    b->n++;
}
// 1 + *out (the box grown by `margin` px, floor / ceil, clipped to `clip`; may be empty = the draw touches no pixel of clip);
// 0 = unusable (no vertex, a bad vertex): the caller keeps the viewport ∩ scissor rect
static inline int nr_uix_bb_rect(const nr_uix_bb *b, int32_t margin, nb_ua_rect clip, nb_ua_rect *out) {
    if (b->bad || !b->n) return 0;
    double x0 = floor(b->x0) - margin, y0 = floor(b->y0) - margin, x1 = ceil(b->x1) + margin, y1 = ceil(b->y1) + margin;
    if (x0 < -1e9) x0 = -1e9; if (y0 < -1e9) y0 = -1e9; if (x1 > 1e9) x1 = 1e9; if (y1 > 1e9) y1 = 1e9;
    nb_ua_rect r = {(int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1};
    r = nb_ua_clip(r, clip);
    *out = nb_ua_empty(r) ? nb_ua_none() : r;
    return 1;
}
// filled primitives (NVN: triangles 4, strip 5, fan 6, quads 7, quad strip 8, polygon 9): the box of the vertices bounds every pixel
// centre they cover; lines (1..3) get the wide-line margin; points and the rest: no box
static inline int nr_uix_prim_margin(int prim, int filled) {
    if (prim >= 4 && prim <= 9) return filled ? 1 : 4;
    if (prim >= 1 && prim <= 3) return 4;
    return -1;
}

// ---- UI cache counting: FNV-1a 64
static inline uint64_t nr_uix_fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
#define NR_UIX_FNV0 0xcbf29ce484222325ull
// a frame of the UI stream, compared with the previous one
typedef struct { uint64_t strict, loose; uint32_t draws, passes; } nr_uix_frame;
typedef struct {
    uint64_t frames, ui_frames, same_strict, same_loose, run_strict, run_loose, best_strict, best_loose;
    uint64_t draws, draws_same_strict, draws_same_loose, passes_same_strict, passes_same_loose;
} nr_uix_cache_acc;
static inline void nr_uix_cache_close(nr_uix_cache_acc *a, const nr_uix_frame *prev, const nr_uix_frame *cur) {
    a->frames++;
    if (!cur->draws) { a->run_strict = a->run_loose = 0; return; }
    a->ui_frames++; a->draws += cur->draws;
    const int same_s = prev->draws && prev->strict == cur->strict, same_l = prev->draws && prev->loose == cur->loose;
    if (same_s) { a->same_strict++; a->draws_same_strict += cur->draws; a->passes_same_strict += cur->passes; if (++a->run_strict > a->best_strict) a->best_strict = a->run_strict; }
    else a->run_strict = 0;
    if (same_l) { a->same_loose++; a->draws_same_loose += cur->draws; a->passes_same_loose += cur->passes; if (++a->run_loose > a->best_loose) a->best_loose = a->run_loose; }
    else a->run_loose = 0;
}

#ifdef __cplusplus
}
#endif
#endif
