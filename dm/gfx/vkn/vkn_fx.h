// [DOS2] VK-NATIVE stage 5, the draw-effect contract (vkn_fx.c + one call point in vkn_draw.c): how a technique changes a draw of
// the engine without editing the draw path. A technique ("piece": shadow cache, fused lighting, material variants, pre-pass
// hybrid, ...) registers one function; the draw path calls the chain ONCE per graphics draw, after the engine shader and the state
// ids are resolved and BEFORE textures, pass and pipeline are touched. A function may:
//   skip the draw                        out->skip = 1: nothing is recorded, no pass is opened; the engine sees a recorded draw;
//   swap the shader                      out->shader = an object of vkn_fx_shader_create (own modules, own binding table);
//   drop the fragment stage              out->no_fs = 1 (depth-only draw of the same vertex module);
//   rasterizer discard                   out->discard = 1 (pipeline variant with rasterizerDiscardEnable);
//   swap the depth-stencil state         out->ds = an id of vkn_fx_ds (0 = keep the engine's);
//   feed extra bindings                  out->tex[i] / out->ubo[i]: the sources of VKN_BK_FX_TEX / VKN_BK_FX_UBO bindings (slot = i)
//                                        of the swapped shader's table;
//   ask for a comparison sampler         out->cmp_mask[stage] bit = engine texture slot: the slot's sampler is replaced by its
//                                        comparison twin (vkn_sampler_cmp_by_id). A binding whose flags carry VKN_BF_CMP gets the
//                                        twin without any function (set by the shader table of a named Dref bundle).
// Everything else of the draw (vertex / index buffers, viewport, scissor, engine textures and constant blocks of bindings with the
// ordinary kinds) stays the engine's. Dispatches and the UI's raw draws do not pass the chain.
//
// Rules for a function:
//   * recording thread only, no locks held by the caller; it must be cheap (2 400 draws a frame): decide by pointer / id compares,
//     keep per-shader verdicts in an own table keyed by ctx->shader->id (ids are dense and small) or by the engine handle;
//   * it sees `out` as the earlier functions of the chain left it (order = registration order `order`, low first) and edits it in
//     place; a function that sets out->skip ends the chain;
//   * the pass is NOT open yet: ctx->color[] / ctx->depth are the current TARGET SET (vkn_pass.h). A function may itself change
//     targets, copy, and issue draws of its own (vkn_draw_indexed / vkn_draw_arrays between vkn_fx_bypass(1) / (0): those do not
//     re-enter the chain), but it must leave the target set as the engine expects before it returns without skip;
//   * a deferred replay of an engine draw needs its constant blocks: vkn_draw_ubo_get reads what the draw would bind now,
//     vkn_draw_ubo announces it again (ring blocks live for the frame); the slot arrays of vkn_draw_in must be copied by the piece;
//   * a swapped shader has no module variants: no alpha-test variant (in->alpha_ref is ignored for it) and no vertex retype;
//   * pieces do not edit vkn_draw.c / hle_dos2_vkn_draw.c (the draw-cost piece owns them); a missing capability is asked from the lead.
// The draw-cost fast paths of vkn_draw.c must treat every field of vkn_fx_out as part of what a draw binds (vkn_fx_out_same).
// No game addresses. Host tests: vkn_stage3_test (block "fx").
#ifndef DM_VKN_FX_H
#define DM_VKN_FX_H
#include "vkn.h"
#include "vkn_draw.h"
#include "vkn_pass.h"
#include "vkn_shader.h"
#include "vkn_tex.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_FX_TEX = 16, VKN_FX_UBO = 8, VKN_FX_CHAIN = 12 };
// Binding kinds of a swapped shader's table whose source is vkn_fx_out (slot = index into out->tex / out->ubo). Outside the engine
// kinds of vkn_shader.h (VKN_BK_OTHER is the last of those).
enum { VKN_BK_FX_TEX = 16, VKN_BK_FX_UBO = 17 };
// vkn_bind.flags bit: this texture binding is read with a depth comparison (sampler2DShadow): bind the slot sampler's twin.
#define VKN_BF_CMP (1u << 30)
// Shader ids of vkn_fx_shader_create start here (pipeline keys hold 24 bits; engine shader ids are dense from 1).
enum { VKN_FX_SHADER_ID0 = 0xf00000, VKN_FX_SHADERS = 256 };

// What the chain is told about the draw besides vkn_draw_in.
typedef struct vkn_fx_ctx {
    const vkn_shader *shader;                      // the engine shader's object (key[VKN_SRC_VS / FS]: the program keys; id)
    vkn_tex *color[VKN_PASS_COLORS]; vkn_tex *depth;   // current target set, NULL = none (vkn_tex.sname: the engine's target name)
    uint16_t blend, ds, raster, vfmt;              // content ids of the engine's states (vkn_state.h; 0 = default)
    uint8_t prim, indexed;                         // primitive as it will be drawn (after the fan / quad / loop rewrite); 1 = indexed
    uint32_t first, count, instances;              // as the engine passed them (before a rewrite)
    uint64_t frame; uint32_t draw_no;              // vkn_frame(), number of this draw among the frame's chain calls (from 0)
} vkn_fx_ctx;

typedef struct vkn_fx_tex {
    VkImageView view; VkSampler sampler;           // sampler VK_NULL_HANDLE = the backend's linear clamp sampler
    vkn_tex *tex;                                  // non-NULL: the draw path makes it readable (vkn_pass_sample: layout, pass break)
} vkn_fx_tex;
typedef struct vkn_fx_ubo {
    VkBuffer buf;                                  // VK_NULL_HANDLE = the constant ring (off = ring offset, as vkn_draw_ubo)
    uint64_t off; uint32_t size;                   // size 0 = not given (the 64 KiB zero block is bound, counted)
} vkn_fx_ubo;
typedef struct vkn_fx_out {
    uint8_t skip, discard, no_fs;
    uint16_t ds;                                   // 0 = keep
    const vkn_shader *shader;                      // NULL = keep
    uint32_t cmp_mask[VKN_STAGE_SLOTS];            // per draw-path stage (0 vertex, 1 fragment): bit = engine texture slot
    vkn_fx_tex tex[VKN_FX_TEX];
    vkn_fx_ubo ubo[VKN_FX_UBO];
} vkn_fx_out;

typedef void (*vkn_fx_fn)(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user);
// Adds a function to the chain (install time or later; any thread). order: low runs first (suggested: 100 shadow cache,
// 200 material / pre-pass variants, 300 fused lighting, 400 post). 0 = ok, -1 = chain full / duplicate name.
int vkn_fx_register(const char *name, vkn_fx_fn fn, void *user, int order);
// The draw path's side. vkn_fx_active: any function registered and no bypass. vkn_fx_run: *out zeroed here, then the chain.
int vkn_fx_active(void);
void vkn_fx_run(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out);
// A piece's own draws: between (1) and (0) the draw path does not call the chain. Counted (nesting allowed). Recording thread.
void vkn_fx_bypass(int on);
// 1 = both outputs make the draw path bind the same things (NULL = the all-zero output).
int vkn_fx_out_same(const vkn_fx_out *a, const vkn_fx_out *b);

// A shader that is not an engine shader: the piece's modules and binding table. bind[]: binding order ascending; kinds
// VKN_BK_UNIFORM / SUPPORT / TEXTURE with the engine's stage and slot (fed from the draw as usual; C1 is not available),
// VKN_BK_FX_TEX / VKN_BK_FX_UBO fed from vkn_fx_out; type = COMBINED_IMAGE_SAMPLER or UNIFORM_BUFFER(_DYNAMIC); count 1;
// bundle_stage 0 vertex / 4 fragment tells the stage flags; `dyn` is filled here. The table is copied. inputs: the vertex module's
// input locations and scalar classes (as vkn_shader.inputs). The object lives until the process ends; NULL = failure (logged red).
typedef struct vkn_fx_shader_src {
    const char *name;                              // logs; also key[VKN_SRC_VS] / key[VKN_SRC_FS] of the object (cut to 16 characters)
    VkShaderModule vs, fs;                         // fs VK_NULL_HANDLE = no fragment stage
    const vkn_bind *bind; uint32_t nbind;
    uint8_t ninputs; uint8_t inputs[16], input_classes[16];
} vkn_fx_shader_src;
const vkn_shader *vkn_fx_shader_create(const vkn_fx_shader_src *src);
static inline int vkn_fx_shader_is(const vkn_shader *s) { return s && s->id >= (uint32_t)VKN_FX_SHADER_ID0; }
// A depth-stencil state by content that no engine handle names (arguments of vkn_ds_put). 0 = the id table is full.
uint16_t vkn_fx_ds(uint8_t depth_test, uint8_t depth_write, uint8_t depth_func, uint8_t stencil_enable, uint8_t stencil_mask,
                   uint8_t stencil_func, uint8_t depth_fail_op, uint8_t pass_op);

// ---- provided by vkn_draw.c for the pieces ----
// The constant block a draw would bind now for (stage 0 vertex / 1 fragment, slot): 0 = ok, -1 = never announced.
int vkn_draw_ubo_get(int stage, uint32_t slot, uint32_t *ring_off, uint32_t *size);
// ---- provided by the named-bundle piece (vkn_tex.c); weak in the draw path until it lands ----
// The comparison twin of an engine sampler (same filter / wrap, compare enabled, LESS_OR_EQUAL), created on first use.
VkSampler vkn_sampler_cmp_by_id(uint32_t sampler_id);

typedef struct vkn_fx_stats {
    uint64_t calls, skipped, shader_swaps, ds_swaps, discards, no_fs, cmp_slots, bypassed;   // draws
    uint64_t by_fn_skip[VKN_FX_CHAIN];             // skips per chain position
    uint32_t nfn, nshaders;
} vkn_fx_stats;
void vkn_fx_stats_get(vkn_fx_stats *s);
void vkn_fx_census(void);                          // one line through vkn_logf (names of the chain, counters)

#ifdef __cplusplus
}
#endif
#endif
