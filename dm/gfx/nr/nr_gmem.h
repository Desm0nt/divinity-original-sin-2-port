// [DOS2] NR-GMEM DM_GFX_PASS_GMEM (docs/dos2/nr/NR-GMEM.md, KB PERF-X19 T8): our own GMEM / sysmem choice per render pass on Adreno
// with Turnip, instead of all-sysmem (TU_DEBUG=sysmem, today's default) or Turnip's autotuner. Pure C (C and C++): no Vulkan, no game
// addresses. The backend (xbg_game.cpp namespace pgm, xbg_pass_gmem.inc) and the driver loader (dm/android/vk_driver.c) use it;
// dm/gfx/vulkan/engine/tests/nr_gmem_test.c tests it on the PC.
//
// How the choice reaches stock Turnip (Mesa 1c3a936 = the bundled turnip27; tu_cmd_buffer.cc use_sysmem_rendering, decided at
// vkCmdEndRenderPass because Turnip records the pass body first and emits it per tile / once afterwards):
//   1. TU_DEBUG=gmem (set by vk_driver.c when DM_GFX_PASS_GMEM=1 and dm/env sets no TU_DEBUG): every pass that CAN be tiled goes GMEM;
//      the autotuner is not consulted (it would put every pass with < 5 draws in sysmem: "Too few draws to tune" - the reason
//      DOOM Eternal's TU_AUTOTUNE_ALGO try on 11.11 left GMEM to the 14 big passes only and gained nothing).
//   2. A pass this rule keeps in sysmem gets a VERTEX -> VERTEX pipeline barrier right before vkCmdEndRenderPass: tu_barrier sets
//      rp.disable_gmem ("Non-framebuffer-space barrier"), which use_sysmem_rendering checks BEFORE TU_DEBUG(GMEM). Valid Vulkan needs a
//      subpass self-dependency for it: vk_driver.c adds VERTEX -> VERTEX self-dependencies to every render pass (tu_pass.cc ignores
//      self-dependencies at creation: no effect on the pass itself). Same trick as DOOM Eternal's DM_GFX_PASS_GMEM (xbg_gsync.h).
//   No driver patch, no rebuild. Other drivers: the barrier is harmless; the backend applies nothing unless driverID = Turnip.
//
// The rule (owner 18.11, tiler plan step 5): GMEM for full-screen passes with few draws and a small attachment set (post, lighting,
// composite); sysmem for draw-heavy passes (G-buffer, depth pre-pass, shadows, surfaces, the Iggy UI layer). Hazards that make GMEM
// wrong keep a pass in sysmem whatever the rule says: a draw that writes storage (vertex-side effects run once per bin), a depth
// attachment sampled inside the pass after the pass wrote depth (sysmem reads the live depth, GMEM the DRAM copy of tiles stored so
// far), more than one subpass (NR-ONCHIP's M; DM_GFX_PASS_GMEM_M=1 lets M's two subpasses through: NR-FRAME) or layer. Optional gate (DM_GFX_PASS_GMEM_BW=1): Turnip's own bandwidth model
// (tu_autotune.cc bandwidth_algo, tu_pass.cc tu_render_pass_bandwidth_config) must favour GMEM - see nrgm_bytes.
#ifndef DM_NR_GMEM_H
#define DM_NR_GMEM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NRGM_OFF = 0, NRGM_APPLY = 1, NRGM_COUNT = 2, NRGM_MODES };
// why a pass stays in sysmem (the first reason that applies; NRGM_W_GMEM = picked for GMEM)
enum { NRGM_W_GMEM = 0,
       NRGM_W_OTHER,      // more than one subpass or layer, or a pass whose creation the backend did not see
       NRGM_W_STORAGE,    // a draw in it writes storage (buffers or images)
       NRGM_W_FEEDBACK,   // its depth attachment was sampled inside the pass and the pass writes depth
       NRGM_W_NODRAW,     // fewer than min_draws draws (clear-only passes, copies drawn as passes)
       NRGM_W_DRAWS,      // more than max_draws draws (G-buffer, pre-pass, shadows, surfaces, UI)
       NRGM_W_ATTS,       // more than max_atts attachments
       NRGM_W_SMALL,      // render area below min_pixels (not full-screen)
       NRGM_W_BW,         // DM_GFX_PASS_GMEM_BW=1 and Turnip's bandwidth model favours sysmem
       NRGM_W_N };
static const char *const nrgm_why_name[NRGM_W_N] = {"gmem", "other", "storage", "feedback", "nodraw", "draws", "atts", "small", "bw"};

typedef struct {
    uint32_t min_draws;   // DM_GFX_PASS_GMEM_MIN_DRAWS (default 1)
    uint32_t max_draws;   // DM_GFX_PASS_GMEM_DRAWS (default 5)
    uint32_t max_atts;    // DM_GFX_PASS_GMEM_ATTS (default 3: colour + depth/stencil attachments)
    uint64_t min_pixels;  // DM_GFX_PASS_GMEM_PIXELS (default 600000: 3D at >= ~0.6 of a 1080p-class frame; half-res chains of 1080p are below)
    int bw;               // DM_GFX_PASS_GMEM_BW (default 0): 1 = also require Turnip's model to favour GMEM
    int sub2;             // [NR-FRAME] DM_GFX_PASS_GMEM_M (default 0): 1 = a two-subpass pass (NR-ONCHIP's M: Sky -> Light, the light reads
                          // Sky's specular as an input attachment) is not vetoed as "other"; the other rules apply to its totals
} nrgm_cfg;
static inline nrgm_cfg nrgm_cfg_default(void) { nrgm_cfg c = {1u, 5u, 3u, 600000u, 0, 0}; return c; }

// What the backend knows about one render pass when it ends.
typedef struct {
    uint32_t w, h, layers, subpasses;   // render area, framebuffer layers, subpass count (0 = creation not seen)
    uint32_t atts;                      // attachments of the render pass
    uint32_t cpp_load, cpp_store, cpp_clear;   // bytes per pixel the pass loads (loadOp LOAD) / stores (STORE) / clears (CLEAR), all attachments
    uint32_t draws;                     // draws recorded in it
    uint64_t draw_bytes;                // sum over its draws of the bytes one covered sample moves (colour write x2 with blend, depth test / write, stencil x2)
    int storage;                        // a draw wrote storage
    int depth_read, depth_written;      // the depth attachment was sampled inside the pass / a draw wrote depth or stencil
} nrgm_pass;

// Turnip's bandwidth estimate of the pass in both modes (bytes). Turnip uses the mean samples-passed of earlier runs of the pass per
// draw; we have no per-draw coverage, so every draw counts as covering the render area (exact for the full-screen quads this rule is
// for, an overestimate of the draw traffic for the rest). sysmem = clears + draw traffic; gmem = 1.1 x (loads + stores) + 0.1 x draw
// traffic (Turnip's "11 / 10" for the tile state changes).
static inline void nrgm_bytes(const nrgm_pass *p, uint64_t *sys, uint64_t *gmem) {
    const uint64_t px = (uint64_t)p->w * p->h * (p->layers ? p->layers : 1u);
    const uint64_t draw = p->draws ? p->draw_bytes * px : 0;
    *sys = (uint64_t)p->cpp_clear * px + draw;
    *gmem = ((uint64_t)(p->cpp_load + p->cpp_store) * px * 11u + draw) / 10u;
}

static inline int nrgm_decide(const nrgm_cfg *c, const nrgm_pass *p) {
    if ((p->subpasses != 1 && !(c->sub2 && (p->subpasses == 2 || p->subpasses == 3))) || p->layers > 1) return NRGM_W_OTHER;   // [NR-FRAME] M: 2 (S2) or 3 (S2F) subpasses
    if (p->storage) return NRGM_W_STORAGE;
    if (p->depth_read && p->depth_written) return NRGM_W_FEEDBACK;
    if (p->draws < c->min_draws) return NRGM_W_NODRAW;
    if (p->draws > c->max_draws) return NRGM_W_DRAWS;
    if (p->atts > c->max_atts) return NRGM_W_ATTS;
    if ((uint64_t)p->w * p->h < c->min_pixels) return NRGM_W_SMALL;
    if (c->bw) { uint64_t s, g; nrgm_bytes(p, &s, &g); if (g >= s) return NRGM_W_BW; }
    return NRGM_W_GMEM;
}

// Bytes a texel of a Vulkan format (by number: no Vulkan header here) takes in Turnip's model (tu_render_pass_attachment cpp:
// depth + stencil planes summed). Unknown formats: 4.
static inline uint32_t nrgm_cpp(uint32_t vk_format) {
    const uint32_t f = vk_format;
    if (f >= 9 && f <= 15) return 1;                                                         // R8 *, S8 below
    if (f == 127) return 1;                                                                  // S8
    if ((f >= 16 && f <= 22) || (f >= 70 && f <= 76) || f == 124) return 2;                  // RG8, R16, D16
    if (f == 129 || f == 125 || f == 126) return 4;                                          // D24S8, X8D24, D32F
    if (f == 128) return 3;                                                                  // D16S8
    if (f == 130) return 5;                                                                  // D32F_S8 (two planes)
    if (f >= 84 && f <= 90) return 6;                                                        // RGB16
    if (f >= 91 && f <= 97) return 8;                                                        // RGBA16
    if (f >= 101 && f <= 103) return 8;                                                      // RG32
    if (f >= 104 && f <= 106) return 12;                                                     // RGB32
    if (f >= 107 && f <= 109) return 16;                                                     // RGBA32
    if (f >= 110 && f <= 112) return 8;                                                      // R64 (never an attachment here)
    return 4;                                                                                // RGBA8 / BGRA8 / A2B10G10R10 / B10G11R11 / RG16 / R32 ...
}

// One draw's bytes per covered sample (Turnip tu6_draw_common: colour bits written (x2 when blending reads the destination), depth cpp
// once for the test and once for the write, stencil cpp x2 with the stencil test).
static inline uint32_t nrgm_draw_bytes(const uint8_t color_cpp[8], uint32_t colors_written, uint32_t blend_mask, uint32_t depth_cpp,
                                       uint32_t stencil_cpp, int dt, int dw, int st) {
    uint32_t b = 0;
    for (unsigned i = 0; i < 8; i++) if (colors_written >> i & 1u) b += color_cpp[i] * ((blend_mask >> i & 1u) ? 2u : 1u);
    if (dt) b += depth_cpp;
    if (dw) b += depth_cpp;
    if (st) b += stencil_cpp * 2u;
    return b;
}

#ifdef __cplusplus
}
#endif
#endif
