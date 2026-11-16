// [dos2-nb] NB-18b-6: render area / depth-stencil detach of the host's Iggy UI passes (DM_NB_UIAREA, docs/dos2/NATIVE_BACKEND_PLAN.md
// "NB-18b-6 findings"). Pure C, inline, no Vulkan: the backend (dm/gfx/vulkan/engine/xbg_nb_uiarea.inc) and the host test
// tests/nb_uiarea_test.cpp call the same code.
//
// Rects are half-open pixel boxes {x0, y0, x1, y1} (x1 <= x0 or y1 <= y0 = empty).
//   nb_ua_draw_rect  pixels a draw can touch: viewport (grown by a margin: wide points / lines may leave it) ∩ scissor ∩ framebuffer
//   nb_ua_seed       render area to begin a pass with: first draw ∪ the union the same pass start drew last time, aligned to the tile
//                    grid (`align`), clamped; returns 0 (= begin with the full area) when it would save less than min_save permille
//   nb_ua_uses_ds    a draw tests or writes depth / stencil (NVN / Vulkan: no depth write without the depth test; stencil writes need the
//                    stencil test; the depth-bounds test reads depth)
// History: a small open-addressed table keyed by the guest state of the pass's first draw (render-target set, fragment shader, scissor,
// viewport): the union of the draws that pass start drew last time and whether one of them used depth / stencil (then no detach).
#ifndef DM_GFX_NB_UIAREA_H
#define DM_GFX_NB_UIAREA_H
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int32_t x0, y0, x1, y1; } nb_ua_rect;
static inline int nb_ua_empty(nb_ua_rect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
static inline nb_ua_rect nb_ua_none(void) { nb_ua_rect r = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN}; return r; }
static inline nb_ua_rect nb_ua_full(uint32_t w, uint32_t h) { nb_ua_rect r = {0, 0, (int32_t)w, (int32_t)h}; return r; }
static inline uint64_t nb_ua_px(nb_ua_rect r) { return nb_ua_empty(r) ? 0 : (uint64_t)(r.x1 - r.x0) * (uint64_t)(r.y1 - r.y0); }
static inline nb_ua_rect nb_ua_union(nb_ua_rect a, nb_ua_rect b) {
    if (nb_ua_empty(a)) return b;
    if (nb_ua_empty(b)) return a;
    nb_ua_rect r = {a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1};
    return r;
}
static inline nb_ua_rect nb_ua_clip(nb_ua_rect a, nb_ua_rect b) {
    nb_ua_rect r = {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1};
    return r;
}
// an empty draw rect fits any area (it touches nothing)
static inline int nb_ua_contains(nb_ua_rect area, nb_ua_rect r) {
    return nb_ua_empty(r) || (r.x0 >= area.x0 && r.y0 >= area.y0 && r.x1 <= area.x1 && r.y1 <= area.y1);
}
// vx..vh: the Vulkan viewport the draw sets (height may be negative: the rows are y+h .. y); scissor: offset + extent as recorded
static inline nb_ua_rect nb_ua_draw_rect(float vx, float vy, float vw, float vh, int32_t sx, int32_t sy, uint32_t sw, uint32_t sh,
                                         uint32_t fbw, uint32_t fbh, int32_t margin) {
    const float y0 = vh < 0 ? vy + vh : vy, y1 = vh < 0 ? vy : vy + vh;
    double fx0 = (double)vx - margin, fy0 = (double)y0 - margin, fx1 = (double)vx + vw + margin, fy1 = (double)y1 + margin;
    if (!(fx0 < fx1) || !(fy0 < fy1)) return nb_ua_none();   // NaN or no area
    if (fx0 < -1e9) fx0 = -1e9; if (fy0 < -1e9) fy0 = -1e9; if (fx1 > 1e9) fx1 = 1e9; if (fy1 > 1e9) fy1 = 1e9;
    nb_ua_rect v = {(int32_t)fx0 - (fx0 < (int32_t)fx0), (int32_t)fy0 - (fy0 < (int32_t)fy0), (int32_t)fx1 + (fx1 > (int32_t)fx1),
                    (int32_t)fy1 + (fy1 > (int32_t)fy1)};   // floor / ceil
    const int64_t sx1 = (int64_t)sx + sw, sy1 = (int64_t)sy + sh;
    nb_ua_rect s = {sx, sy, (int32_t)(sx1 > INT32_MAX ? INT32_MAX : sx1), (int32_t)(sy1 > INT32_MAX ? INT32_MAX : sy1)};
    nb_ua_rect r = nb_ua_clip(nb_ua_clip(v, s), nb_ua_full(fbw, fbh));
    return nb_ua_empty(r) ? nb_ua_none() : r;
}
static inline nb_ua_rect nb_ua_align(nb_ua_rect r, int32_t align, uint32_t fbw, uint32_t fbh) {
    if (nb_ua_empty(r)) return r;
    if (align > 1) { r.x0 -= r.x0 % align; r.y0 -= r.y0 % align; r.x1 += (align - r.x1 % align) % align; r.y1 += (align - r.y1 % align) % align; }
    return nb_ua_clip(r, nb_ua_full(fbw, fbh));
}
// 1 + *out: begin with *out; 0: begin with the full area (nothing drawn yet known, or the saving is under min_save permille)
static inline int nb_ua_seed(nb_ua_rect first, const nb_ua_rect *hist, uint32_t fbw, uint32_t fbh, int32_t align, uint32_t min_save,
                             nb_ua_rect *out) {
    nb_ua_rect u = hist ? nb_ua_union(first, *hist) : first;
    if (nb_ua_empty(u) || !fbw || !fbh) return 0;
    u = nb_ua_align(u, align, fbw, fbh);
    const uint64_t full = (uint64_t)fbw * fbh, a = nb_ua_px(u);
    if ((full - a) * 1000 < (uint64_t)min_save * full) return 0;
    *out = u; return 1;
}
static inline int nb_ua_uses_ds(int depth_test, int stencil_test, int bounds_test) { return depth_test || stencil_test || bounds_test; }

// ---- history of pass starts
#define NB_UA_HIST 512   // power of two
typedef struct { uint64_t key; nb_ua_rect uni; uint8_t ds, used; uint32_t frame; } nb_ua_hent;
typedef struct { nb_ua_hent e[NB_UA_HIST]; uint32_t n; } nb_ua_hist;
static inline void nb_ua_hist_reset(nb_ua_hist *h) { memset(h, 0, sizeof *h); }
static inline nb_ua_hent *nb_ua_hist_find(nb_ua_hist *h, uint64_t key) {
    if (!key) key = 1;
    for (uint32_t i = 0, j = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 55) & (NB_UA_HIST - 1); i < 16; i++, j = (j + 1) & (NB_UA_HIST - 1)) {
        if (!h->e[j].used) return 0;
        if (h->e[j].key == key) return &h->e[j];
    }
    return 0;
}
// the entry of key, created if missing; the table is emptied when it gets 3/4 full or a probe run is long (UI layouts change rarely)
static inline nb_ua_hent *nb_ua_hist_put(nb_ua_hist *h, uint64_t key) {
    if (!key) key = 1;
    nb_ua_hent *f = nb_ua_hist_find(h, key);
    if (f) return f;
    if (h->n * 4 >= NB_UA_HIST * 3) nb_ua_hist_reset(h);
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0, j = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 55) & (NB_UA_HIST - 1); i < 16; i++, j = (j + 1) & (NB_UA_HIST - 1))
            if (!h->e[j].used) { memset(&h->e[j], 0, sizeof h->e[j]); h->e[j].used = 1; h->e[j].key = key; h->e[j].uni = nb_ua_none(); h->n++; return &h->e[j]; }
        nb_ua_hist_reset(h);
    }
    return 0;
}

// ---- one host pass, as seen at its end (both modes count; =2 applies nothing)
enum { NB_UA_CLS_OTHER = 0, NB_UA_CLS_IGGY, NB_UA_CLS_FILTER };   // first draw's fragment shader: Iggy d99f1fdb / Iggy fill or blur
typedef struct {
    uint32_t w, h, natt, draws;          // framebuffer, attachments (colour + depth), draws
    int cls;                             // NB_UA_CLS_* of the first draw
    nb_ua_rect uni;                      // union of nb_ua_draw_rect of its draws
    uint8_t clear_start;                 // begun with a clear (lazy clear twin / folded pending clears / in-pass clear): no smaller area
    uint8_t ds_att, ds_used;             // the guest's render-target set has depth / stencil; any draw used it
    uint8_t reduced, detached, restarted;   // =1: begun with a smaller area / without its depth-stencil; a draw outside the area restarted it
    nb_ua_rect area;                     // =1: the area it began with (reduced)
    uint32_t fills, fills_opaque, fills_first_full;   // draws of the Iggy fill shader; opaque (no blend, full mask, no DS); first draw of the pass covering the framebuffer rect
} nb_ua_pass;
static inline void nb_ua_pass_init(nb_ua_pass *p) { memset(p, 0, sizeof *p); p->uni = nb_ua_none(); p->area = nb_ua_none(); }
typedef struct {
    uint64_t frames, passes, ui_passes, flt_passes, ui_draws;
    uint64_t area_q, area_q_flt, area_px_saved, area_bytes_saved;        // qualify for a smaller area (and their pixel / byte saving estimate)
    uint64_t ds_q, ds_q_flt, ds_bytes_saved;                             // depth / stencil attached but unused
    uint64_t fills, fills_opaque, fills_first_full;
    uint64_t reduced, reduced_px_saved, detached, restarts_draw, restarts_clear, ds_reattach, no_area_clear;   // =1: applied
} nb_ua_acc;
// align / min_save as nb_ua_seed; bytes: 4 per attachment pixel (RGBA8, D24S8), load + store on a tiler
static inline void nb_ua_commit(nb_ua_acc *a, const nb_ua_pass *p, int32_t align, uint32_t min_save) {
    a->passes++;
    if (p->cls == NB_UA_CLS_OTHER || !p->draws) return;
    a->ui_passes++; a->ui_draws += p->draws; const int flt = p->cls == NB_UA_CLS_FILTER; a->flt_passes += (uint64_t)flt;
    const uint64_t full = (uint64_t)p->w * p->h;
    nb_ua_rect s;
    if (!p->clear_start && nb_ua_seed(p->uni, 0, p->w, p->h, align, min_save, &s)) {
        const uint64_t px = full - nb_ua_px(s);
        a->area_q++; a->area_q_flt += (uint64_t)flt; a->area_px_saved += px; a->area_bytes_saved += px * 4u * p->natt * 2u;
    } else if (p->clear_start && !nb_ua_empty(p->uni) && nb_ua_px(nb_ua_align(p->uni, align, p->w, p->h)) < full) a->no_area_clear++;
    if (p->ds_att && !p->ds_used) { a->ds_q++; a->ds_q_flt += (uint64_t)flt; a->ds_bytes_saved += full * 4u * 2u; }
    a->fills += p->fills; a->fills_opaque += p->fills_opaque; a->fills_first_full += p->fills_first_full;
    if (p->reduced) { a->reduced++; a->reduced_px_saved += full - nb_ua_px(p->area); }
    a->detached += p->detached;
}

#ifdef __cplusplus
}
#endif
#endif
