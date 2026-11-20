// [DOS2] VK-NATIVE stage 5.1 piece A4: LIGHTFUSE + FOG on the backend (vkn_lightfuse.c). The old path's technique NR-LIGHTFUSE
// (translator: dm/gfx/vulkan/engine/xbg_nr_lightfuse.inc; model: dm/gfx/nr/nr_lightfuse.h, reused as it is; shader: the pack's
// "f." / "g.ClusteredLightingWithSSAO" of native-nr.xgspack, tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag -DLIGHTFUSE
// [-DLFFOG], proof tools/dos2_lightfuse.py; docs/dos2/nr/NR-LIGHTFUSE.md) ported to mode 1 through the draw-effect contract vkn_fx.h
// (one function, order 300).
//
// The frame (the old path's dumps): DeferredSky draws once into [Sp, Df] (both cleared just before; R11G11B10F), reading the G-buffer;
// ClusteredLightingWithSSAO draws next into [Df] blended ONE, ONE, ADD, reading the same G-buffer and Sp (its ambient, binding 52);
// DeferredFog then reads Df + the linear depth and writes C (B10G11R11, R G B written; C is cleared before it), FadeBlending reads C.
//
// Variant A, Sky fused (DM_VK_NATIVE_LIGHTFUSE): the Sky draw is SKIPPED (out->skip: no pass opens; the engine's clears of Sp / Df stay
// remembered by vkn_pass - Df's becomes loadOp CLEAR of the Light pass, Sp's runs at Sp's next use, if any) and the Light draw becomes
// the fused draw: out->shader = a shader object of vkn_fx_shader_create made once per Light shader from the fused module ('f') and the
// Light shader's binding table (the engine sources as they are, the shader's own C1 constants fed as effect blocks from its static
// arena blocks, bindings the fused module does not declare left out) + the extras of nr_lightfuse.h as effect bindings fed from the
// stash of the Sky draw (its 8 textures resolved at the Sky draw: out->tex[0..7]; its c8 / c6 ring blocks: out->ubo[0..1]).
// Why skip and not the rasterizer-discard pipeline of the plan: the only effects of the Sky pass besides its draw are its two clears,
// which vkn_pass keeps as remembered clears without the pass (and folds Df's into the Light pass for free); a discard pipeline would
// open and store an empty pass of two targets for nothing.
// Variant B, Sky + Fog fused (DM_VK_NATIVE_LIGHTFUSE_FOG): after one Fog draw passed every Fog check (learned), the Light draw is the
// 'g' module drawn into [Df, Sp] (Sp = output 1: DeferredFog of the stored Df, proven bit for bit against the native DeferredFog;
// blended into Sp's pending clear, which must be zero: x + 0 = x), issued by this module itself inside the Light draw's chain call
// (vkn_fx_force; the engine's draw is skipped, the target set put back); the Fog draw is skipped and the draws that sample C until C's
// next write sample Sp instead (a shader object of their own: the reader's modules and table, C's texture bindings fed as effect
// textures with Sp; C1 as effect blocks). Where that cannot be (alpha test / vertex retype variant, another kind of binding, too many
// readers) or C is about to be drawn into, Sp is copied into C first (vkn_pass_copy: same format, size, one level, one layer - C as the
// Fog draw would have left it: the Fog draw covers all of C, R G B on an alpha-less format), which ends the window. A non-draw use of C
// inside the window (copy / dispatch / clear: C's layout or remembered clears changed) ends it: counted, red when it may have read C.
//
// Checks (every one also in mode 2; reasons below, counted):
//   Sky draw: VS + FS of the DeferredSky key (nr_sh table), 2 colour targets, no depth, no alpha test, no earlier effect on the draw,
//     the 10 extras (nr_lf_x) are texture / uniform bindings of Sky's table with live textures and announced blocks.
//   Light draw: the stash of this frame and recording, the ClusteredLightingWithSSAO key, VS + FS, no alpha test / earlier effect,
//     one colour target = Sky's Df, no depth, colour 0 blended ONE, ONE, ADD with R G B written (nr_lf_blend_ok), Df's pending clear
//     zero (the fused output is what the blender gave over Sky's Df), binding 52 = Sky's Sp (the ambient the fused module computes
//     instead), the same vertex shader bundle as Sky's (byte for byte), the same vertex constant bytes, vertex / index bytes, vertex
//     format, primitive, draw range, viewport, scissor (the view ray at every pixel is Sky's), no vertex retype, Sp / Df untouched by
//     anything else since the Sky draw, the fused shader made.
//   Fog draw (after a Light of this frame that passed): as xbg_nr_lightfuse.inc fogdraw, one counter per condition, + the Fog draw
//     covers its whole target (viewport / scissor) and C / Sp / Df have one size, one level, one layer.
// Decisions: nr_lf_sky / nr_lf_light (nr_lightfuse.h): Sky is skipped only after an earlier chain passed every Light check (learned).
// Recovery (the backend cannot replay a skipped draw from a recording): the stash keeps the whole Sky draw (draw_in with the slot
// arrays copied, every constant block it binds, its target set, its range, vertex / index bytes); the FIRST later draw that is not the
// fused Light (any draw, or a Light that fails a check) draws the stashed Sky first (vkn_fx_bypass + vkn_draw_*, the engine's target
// set and announced blocks put back) and then goes on as the engine asked: exact, because nothing drew between the two (a non-draw use
// of Sp / Df in between is detected by their layout / remembered clears and counted "lost", red). A skipped Sky with no later draw in
// its frame (frame change) is "lost" too. Either way learning starts again (nr_lf_orphan / nr_lf_light).
//
// Switches (read once at install; the old names are forced to 0 in mode 1 by dm_dos2_vkn_pre):
//   DM_VK_NATIVE_LIGHTFUSE      (replaces DM_NR_LIGHTFUSE)      0 off; 1 on (default: xbg_nr_lightfuse_default = 1, every family);
//                               2 verify: every check and decision counted, the fused shader made, the engine's draws drawn;
//                               3 = 1 + poison: Sp's remembered clear becomes magenta at a fused Light (a later reader of Sky's Sp
//                               shows it); with variant B, C's clear becomes magenta at a skipped Fog (a reader the redirect misses)
//   DM_VK_NATIVE_LIGHTFUSE_FOG  (replaces DM_NR_LIGHTFUSE_FOG)  0 off; 1 on (default: xbg_nr_lightfuse_fog_default = 1); 2 verify
//                               (Fog checks counted, the Fog drawn); needs DM_VK_NATIVE_LIGHTFUSE 1 / 3 to apply.
// Interplay: DM_VK_NATIVE_SH (named bundles): the fused module replaces whatever fragment module the Light shader has (translated or
// 'x.': equal heads), Sky / Fog keep theirs; Dref (DM_VK_NATIVE_SH_PCF=3) is a shadow-class swap (CascadedPCF_*), the Light shader
// samples no depth atlas: the fused table carries VKN_BF_CMP exactly where the fused module samples with a comparison (expected none,
// logged). The shadow cache (100) and the occlusion functions (50 / 250) run before: a draw they skip never reaches this function, an
// earlier shader swap / discard / no-fragment on Sky or Light is a refusal.
// Census: vkn_lf_census (dm_vkn_census, every 10 s); red lines 【红】 for the unexpected (first 16 each kind).
// Recording thread only (the fx chain, the census on the render thread after EndFrame). No game addresses.
#ifndef DM_VKN_LIGHTFUSE_H
#define DM_VKN_LIGHTFUSE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Light refusal reasons (W_*) and Fog refusal reasons (F_*): vkn_lf_reason_name(0, i) / (1, i).
enum { VKN_LF_W_SKY_SHAPE, VKN_LF_W_SKY_STASH, VKN_LF_W_NO_STASH, VKN_LF_W_LIGHT_FS, VKN_LF_W_LIGHT_RT, VKN_LF_W_BLEND, VKN_LF_W_CLEAR,
       VKN_LF_W_AMBIENT, VKN_LF_W_VS, VKN_LF_W_VS_DATA, VKN_LF_W_DRAW, VKN_LF_W_RETYPE, VKN_LF_W_TOUCHED, VKN_LF_W_TWIN, VKN_LF_W_N };
enum { VKN_LF_F_NOLIGHT, VKN_LF_F_STAGES, VKN_LF_F_RT, VKN_LF_F_BLEND, VKN_LF_F_MASK, VKN_LF_F_BIND, VKN_LF_F_TARGET, VKN_LF_F_SCENE,
       VKN_LF_F_DEPTH, VKN_LF_F_CONST, VKN_LF_F_VS, VKN_LF_F_DRAW, VKN_LF_F_COVER, VKN_LF_F_SPCLEAR, VKN_LF_F_TWIN, VKN_LF_F_N };
const char *vkn_lf_reason_name(int fog, int i);

typedef struct vkn_lf_stats {
    uint64_t frames, sky, light, fog;              // frames with a Sky draw; draws seen by role
    uint64_t would, fused, sky_skipped;            // chains that passed every check while Sky was drawn (mode 2 / learning), fused Light draws, Sky draws skipped
    uint64_t recovered, lost, learn, unlearn, poison;   // skipped Skies drawn late (exact), skipped Skies whose output is missing, learning events
    uint64_t why[VKN_LF_W_N];                      // Light / Sky refusals by reason
    uint64_t fog_ok, fog_fused, fog_skipped, fog_back, fog_why[VKN_LF_F_N];   // Fog draws passing every check, fused Light draws with Fog, Fog draws skipped, drawn after a 'g' Light
    uint64_t redir_draws, redir_windows, redir_copies, redir_lost, redir_open_end, twins;   // draws reading Sp for C, windows, Sp -> C copies, non-draw reads, windows open at a frame change, reader shaders made
    uint64_t reds;
} vkn_lf_stats;

// Install (mode 1, hle_dos2_vkn.c): reads the switches, registers the function (order 300). Idempotent.
void vkn_lf_install(void);
int vkn_lf_active(void);                           // 1 = the function is in the chain
void vkn_lf_census(void);                          // one line (+ one for Fog when it is on), counters since the last line
void vkn_lf_stats_get(vkn_lf_stats *s);            // totals since install
// Host test: the switches as given (no getenv), the state cleared, the function registered once.
void vkn_lf_test_setup(int mode, int fog);

#ifdef __cplusplus
}
#endif
#endif
