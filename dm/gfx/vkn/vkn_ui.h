// [DOS2] VK-NATIVE stage 3, the UI (vkn_ui.c): Iggy's GDraw backend (written by the game directly against NVN, main+0x1bea840..
// 0x1bf4338) drawn by the native backend into the same recording as the engine (vkn_frame.h) and through the same render-pass
// bookkeeping (vkn_pass.h). dm/host/hle_dos2_vkn_ui.c replaces every GDraw function that reaches an NVN command-buffer call, keeps
// GDraw's own records and caches as the originals write them, and hands this module plain values: program objects (opaque u64 keys)
// with their control / code, textures as vkn_tex, constants and vertices as CPU bytes or arena blocks, the GDraw state indices.
// GDraw's fixed objects (built once by 0x1bea840 and only ever selected by index):
//   blend 0..5    0 none, 1 alpha (ONE, ONE_MINUS_SRC_ALPHA), 2 multiply (DST_COLOR, ONE_MINUS_SRC_ALPHA), 3 add (ONE, ONE), 4 filter
//                 (none), 5 special (none: the special programs blend in the shader); equations add, alpha factors = colour factors;
//   depth-stencil 0..7 = (state address - 0x508) / 8: bit 0 stencil test, bit 1 depth test LESS, bit 2 depth write (ALWAYS when bit 1
//                 is clear); stencil: func EQUAL against reference 0xff under the value mask, pass REPLACE, fail / depth fail KEEP,
//                 both faces, value mask and write mask per draw (SetStencilValueMask / SetStencilMask);
//   channel mask  all on / all off (a draw that only writes stencil);
//   vertex formats v2 (stride 8: location 0 RG32F), v2aa (16: + location 1 RGBA16I at 8), v2tc2 (16: + location 1 RG32F at 8);
//   sampler       every texture unit uses GDraw's sampler +0x228: clamp to edge, linear, linear mipmaps;
//   programs      one NVN program object per stage (0x1beba70); a draw binds one vertex and one fragment program; uniform block 0
//                 of each stage (0x80 vertex / 0x30 fragment bytes from the engine's ring); texture units 0, 1, 2 and 7.
// A vertex + fragment program pair becomes one vkn_shader (vkn_shader.h) under a handle of this module's own (index 0xff00 + pair,
// generation 1: far above the engine's shader indices), which gives the modules, the set / pipeline layouts and the binding table.
// Pipelines and descriptor sets are this module's own (a draw binds its pipeline and set; vkn_draw_raw_begin() afterwards makes the
// engine's next draw rebind everything). Single-threaded: every call except vkn_ui_program_put comes from the recording thread.
// No game addresses, no engine record offsets.
#ifndef DM_VKN_UI_H
#define DM_VKN_UI_H
#include "vkn.h"
#include "vkn_tex.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_UI_VF_V2 = 0, VKN_UI_VF_V2AA = 1, VKN_UI_VF_V2TC2 = 2, VKN_UI_VFS = 3 };
enum { VKN_UI_UNITS = 4 };   // texture units 0, 1, 2, 7 of the GDraw programs, in this order
enum { VKN_UI_CTL = 0xe48 }; // control bytes vkn_shader reads (a shorter control section is zero-padded by the caller)

// A GDraw program object (0x1beba70): stage 0 vertex / 1 fragment, its control section (VKN_UI_CTL bytes, copied) and code (CPU
// pointer of the code the program got; read when its pair is created, kept by GDraw for its lifetime). Any thread. 0 = known.
int vkn_ui_program_put(uint64_t prog, int stage, const void *control, const void *code);

// The current GDraw target set: one colour target and the depth-stencil target (either may be NULL). vkn_pass_targets underneath.
void vkn_ui_targets(vkn_tex *color, vkn_tex *depth);
// Viewport and scissor of the next draws (pixels, upper-left origin as NVN's and Vulkan's).
void vkn_ui_viewport(int32_t x, int32_t y, int32_t w, int32_t h);
void vkn_ui_scissor(int32_t x, int32_t y, int32_t w, int32_t h);
// nvnCommandBufferClearDepthStencil(depth 1.0, depth mask, stencil 0, stencil mask) on the bound depth target (vkn_pass_depth): depth when
// `depth`; stencil bits `mask` to 0 (a full mask is a clear, a partial one a stencil-only draw over the whole target).
void vkn_ui_clear_ds(int depth, uint8_t mask);
// A rectangle of colour target t to transparent black (TextureDrawBufferBegin's clearing quad: blend none, no depth / stencil).
void vkn_ui_clear_color(vkn_tex *t, const int32_t rect[4]);

typedef struct vkn_ui_draw_in {
    uint64_t vs, fs;                    // program objects (vkn_ui_program_put)
    uint8_t vfmt;                       // VKN_UI_VF_*
    uint8_t quads;                      // 1: nverts vertices, 4 a quad (NVN QUADS, no indices); 0: nidx u16 indices, triangles
    uint8_t blend, ds, color_off;       // GDraw state indices (header)
    uint8_t stencil_cmp, stencil_write; // value mask / write mask (reference 0xff); used when ds bit 0
    vkn_tex *tex[VKN_UI_UNITS];         // units 0, 1, 2, 7; NULL = this module's placeholder (1x1 transparent black)
    const void *vsc, *fsc;              // uniform block 0 of the vertex / fragment stage (CPU bytes, copied into the ring)
    uint32_t vsc_bytes, fsc_bytes;
    const void *fsc1; uint32_t fsc1_bytes;   // uniform block 1 of the fragment stage (the blur pass: taps), NULL = zeros
    const void *verts; const uint16_t *idx;   // CPU vertices / indices (copied into the ring) when vb is NULL
    const vkn_block *vb; uint64_t vb_off, ib_off;   // or an arena block (GDraw vertex buffer): byte offsets of vertices / indices
    uint32_t nverts, nidx;
} vkn_ui_draw_in;
// Records the draw (pass opened by vkn_pass_ready, textures made sampleable by vkn_pass_sample first). 0 = drawn; 1 = dropped
// (counted by reason; the first 16 of each reason print a red line).
int vkn_ui_draw(const vkn_ui_draw_in *in);

typedef struct vkn_ui_stats {
    uint64_t draws, quads, indexed, vertices, ring_bytes, targets, clears_ds, clears_masked, clears_color;
    uint64_t pipelines, pipe_fail, pairs, pair_fail, sets, pools, feedback, placeholder;
    uint64_t drop_cb, drop_pass, drop_shader, drop_bind, drop_pipe, drop_set, drop_ring, drop_vb, drop_empty, drop_prog;
} vkn_ui_stats;
void vkn_ui_stats_get(vkn_ui_stats *s);

// ==== [stage 5.2 B2] the UI pass on tile GPUs (DM_VK_NATIVE_UIPASS; replaces DM_NR_UIX_DS / DM_NR_UIX_AREA of the old path) ====
// Port of NB-18b-6 + NR-UIX (xbg_nb_uiarea.inc, xbg_nr_uix.inc, docs/dos2/nr/NR-UIX.md) with their pure parts as they are
// (dm/gfx/nb/nb_uiarea.h rects / seed / history, dm/gfx/nr/nr_uix.h ordinal keys / decay). The unit is an "epoch": one GDraw target
// set from vkn_ui_targets to the next different one (or the next recording, or a fence); its first pass-opening event (a draw, a
// region clear, a depth-stencil clear) takes the decisions, before vkn_pass_ready opens anything:
//   depth-stencil detach (level 1): GDraw gives every target its UI depth-stencil (+0xb8), the filter ping-pong targets too, but only
//     clip-mask draws (stencil) and depth-tested draws use it. The epoch is drawn into (colour, no depth) unless THIS pass start (key =
//     colour, depth, first event kind / fragment program, scissor, viewport + how many such starts came before in this frame) used its
//     DS within the last 120 frames (NR-UIX rule, play-1118 evidence) - or, rule 1, unless the previous frame's UI used DS at all.
//     A draw that needs depth / stencil (or a masked stencil clear) in a detached epoch re-attaches: vkn_pass_targets(colour, depth)
//     ends the DS-less pass and the next one LOADs both (exact either way: nothing touches the DS while it is detached; clears of it
//     are remembered by vkn_pass on the texture and folded / run as before).
//   render area (level 2): the epoch's first event rect (scissor ∩ viewport + 4 px, the region of a region clear) ∪ what the same pass
//     start drew last time (history, refreshed after 60 frames), aligned to 32 px, at least 10 % saved; hinted to vkn_pass
//     (vkn_pass_area_hint below, weak); every later event whose rect is not inside it ends the narrowed pass and drops the hint (the
//     rest of the epoch is drawn full area, LOAD): exact on any GPU because Vulkan keeps the attachments outside renderArea (LOAD /
//     STORE). No vertex boxes: the backend has no CPU position program of GDraw's vertex shaders (the old path ran NR-10b's SPIR-V
//     interpreter); scissor ∩ viewport is the proven superset.
//   fence (vkn_ui_pass_fence): before the engine may draw / clear with GDraw's targets bound (custom draws, RenderingEnd) the epoch
//     puts the NVN-visible set back (re-attach) and drops the area hint.
// Levels: DM_VK_NATIVE_UIPASS 0 off (the stage-3 code path exactly), 1 DS detach, 2 = 1 + area, 3 stress (every epoch with a DS
// detached, area = the first rect alone, no history / alignment: exercises re-attach and restart); unset = family default (2 on Mali /
// PowerVR and DM_GFX_SIM_GPU mali / powervr models, 0 elsewhere). DM_VK_NATIVE_UIPASS_VERIFY=1: the level's decisions are taken and
// counted (would detach / would re-attach / would narrow / would restart, bytes), the old targets and area are drawn.
// DM_VK_NATIVE_UIPASS_RULE: 0 per pass start (default, NR-UIX), 1 per frame (the whole UI layer detached only after a frame without DS).
// mode -1 = the family default; sim_tiler: DM_GFX_SIM_GPU names a tile model. Install time (the host's registration).
void vkn_ui_pass_config(int mode, int verify, int rule, int sim_tiler);
void vkn_ui_pass_fence(void);
enum { VKN_UIP_DS_HIST, VKN_UIP_DS_FRAME, VKN_UIP_DS_SHARED, VKN_UIP_DS_SIZE, VKN_UIP_DS_DYING, VKN_UIP_DS_N };   // detach refusals
enum { VKN_UIP_AR_API, VKN_UIP_AR_CLEAR, VKN_UIP_AR_MS, VKN_UIP_AR_SAVE, VKN_UIP_AR_N };                          // area refusals
typedef struct vkn_ui_pass_stats {
    int mode, verify, rule, tile, api;   // in effect (mode -1 until the device is known); api: the pass module has the area functions
    uint64_t ui_frames, epochs, epochs_ds, detached, kept_off, reattach, fence_reattach, fence_end, fence_draw, set_lost;
    uint64_t area_hinted, area_applied, area_ignored, restarts, hint_dropped, px_full, px_area, ds_bytes, area_bytes;
    uint64_t ds_refused[VKN_UIP_DS_N], area_refused[VKN_UIP_AR_N];
    uint64_t w_detach, w_reattach, w_area, w_restart, w_ds_bytes, w_area_bytes;   // verify: what the level would have done
} vkn_ui_pass_stats;
void vkn_ui_pass_stats_get(vkn_ui_pass_stats *s);
// What the pass module must provide for level 2 (declared weak in vkn_ui.c; absent = level 2 acts as level 1, refusal "api"):
//   int vkn_pass_area_hint(const VkRect2D *area)  the passes vkn_pass opens for the CURRENT target set begin with renderArea = *area
//       (clipped to the attachments) until the set changes, a recording begins, or the next call (NULL = full area again). Ignored
//       (full area) by a pass that loads an attachment with CLEAR and under the poison load / store modes. Does not end an open pass.
//       While a narrowed pass is open: an in-pass clear (vkn_pass_clear_color / _clear_depth) whose rect is not inside renderArea must
//       end the pass first (vkCmdClearAttachments must lie inside it; the depth clear's rect is the whole level), and the region clip of
//       vkn_pass_clear_color must use offset + extent; vkn_pass_targets() of any caller ends a narrowed pass even for the same set.
//       0 = stored.
//   int vkn_pass_area_get(VkRect2D *out)  1 + the render area of the pass open in vkn_cb() for the current set; 0 = none open.

// ==== [rt P6] DM_VK_NATIVE_UISET (vkn_ui.c "[rt P6]"): 0 a fresh set per UI draw (as before), 1 sets of the recording by content (the
// copies' offsets as dynamic offsets), 2 = 1 without the reuse (fresh sets, lookups counted). Install time. Totals of hits / misses / would-hit (mode 2).
void vkn_ui_set_cache(int mode);
void vkn_ui_set_cache_stats(uint64_t *hits, uint64_t *miss, uint64_t *would);
#ifdef __cplusplus
}
#endif
#endif
