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
// ==== [mgpu P1] the region of an Iggy filter target (VK-MOBILE-GPU-PLAN.md П1) ====
// DM_VK_NATIVE_UIREGION (needs UIPASS >= 1): 0 off; 1 an epoch the host names as a pushed Iggy target (vkn_ui_region: TextureDrawBufferBegin
// with a region, the rect it clears) whose first event is that region clear gets the region as its render area - no history, no
// alignment, no 10 % rule; its colour-only draws are clipped to it (their scissor), not restarted: Iggy reads nothing of a pushed target
// outside its region, and a colour target of its own is no one else's; a depth-stencil use or a clear outside the region drops the rule
// (the level-2 restart: the DS is shared by every epoch); 2 verify (counted: would region / clip / drop; the epoch drawn full area); 3 poison
// (1 + the texels outside the region cleared magenta first: a read of them shows). -1 = family default: the render-pass tilers 1, else 0.
void vkn_ui_region_config(int mode);
void vkn_ui_region(vkn_tex *t, const int32_t rect[4]);   // the host's naming (recording thread: a record)
// ==== [mgpu P2] the Iggy filter cache (VK-MOBILE-GPU-PLAN.md П2; vkn_ui.c block "mgpu P2" has the rule) ====
// DM_VK_NATIVE_UIFC: 0 off, 1 on (+ every 128th hit drawn and compared: a difference turns it off), 2 verify (every hit drawn and
// compared: "differ" must stay 0), 3 poison (1 + a hit clears its region magenta one frame in 64); -1 = default 1. Any GPU (no UIPASS
// needed). Install time.
void vkn_ui_fc_config(int mode);
void vkn_ui_pass_fence(void);
enum { VKN_UIP_DS_HIST, VKN_UIP_DS_FRAME, VKN_UIP_DS_SHARED, VKN_UIP_DS_SIZE, VKN_UIP_DS_DYING, VKN_UIP_DS_N };   // detach refusals
enum { VKN_UIP_AR_API, VKN_UIP_AR_CLEAR, VKN_UIP_AR_MS, VKN_UIP_AR_SAVE, VKN_UIP_AR_N };                          // area refusals
// [mgpu P7b] what the engine event that flushed a run touched: the layer, a depth-stencil target, a pushed target, a texture the run reads
// only, none (a pass end, the size limit, other)
enum { VKN_UILT_LAYER, VKN_UILT_DS, VKN_UILT_PUSHED, VKN_UILT_READ, VKN_UILT_NONE, VKN_UILT_N };
typedef struct vkn_ui_pass_stats {
    int mode, verify, rule, tile, api;   // in effect (mode -1 until the device is known); api: the pass module has the area functions
    uint64_t ui_frames, epochs, epochs_ds, detached, kept_off, reattach, fence_reattach, fence_end, fence_draw, set_lost;
    uint64_t area_hinted, area_applied, area_ignored, restarts, hint_dropped, px_full, px_area, ds_bytes, area_bytes;
    uint64_t ds_refused[VKN_UIP_DS_N], area_refused[VKN_UIP_AR_N];
    uint64_t w_detach, w_reattach, w_area, w_restart, w_ds_bytes, w_area_bytes;   // verify: what the level would have done
    // [mgpu P1] DM_VK_NATIVE_UIREGION: mode; region epochs (area hinted = the region), their pixels; colour draws clipped to it; region rules
    // dropped (a DS use / a clear outside); refused (counted in area_refused too); poisoned epochs; verify: would-region epochs / clips / drops
    int region_mode;
    uint64_t region_epochs, region_px, region_clipped, region_dropped, region_refused, region_poison, w_region, w_region_clip, w_region_drop;
    // [mgpu P2] DM_VK_NATIVE_UIFC (vkn_ui.c "filter cache")
    int fc_mode;
    uint64_t fc_epochs, fc_hits, fc_misses, fc_stored, fc_refused[8], fc_draws_saved, fc_px_copied, fc_verify, fc_differ, fc_poison, fc_evicted, fc_flush[6];
    // [mgpu P7] DM_VK_NATIVE_UILAYER / DM_VK_NATIVE_UICACHE (vkn_ui.c "mgpu P7"): runs held, calls / draws held, flushes by reason (VKN_UILF_*),
    // runs executed hoisted / in their order, why not hoisted (VKN_UILU_*: first reason of a run), pushed targets poisoned; the cache:
    // candidate runs, refusals (VKN_UICR_*), would-hits, hits (skipped runs), draws / calls not executed, compares, differences, poisoned hits;
    // the CPU time of the UI's entry points and flushes on the recording thread (ns) and the calls
    int layer_mode, cache_mode;
    uint64_t lh_runs, lh_calls, lh_draws, lh_flush[8], lh_hoisted, lh_ordered, lh_unsafe[8], lh_poison;
    uint64_t uc_cand, uc_refused[12], uc_would, uc_hits, uc_draws_saved, uc_calls_saved, uc_verify, uc_differ, uc_poison;
    uint64_t ui_ns, ui_calls;
    // [mgpu P7b] (vkn_ui.c "mgpu P7b"): flushes by the engine's event (index = VKN_PBE_*) and by what it touched (VKN_UILT_*); runs
    // hoisted with the layer alone (no pushed target: one pass, fences dropped); pushed targets renamed (a reuse after the layer read
    // them: the new content goes to another texture of the same shape), renames refused (no free texture), spares made (bytes alive);
    // runs whose pushed depth-stencil use was proven order-free by the symbolic check / was not; the layer epoch's depth-stencil taken
    // from the run (detached / kept), its render area from the run (passes, pixels of the full target), restarts of such a pass (0)
    uint64_t lh_fev[10], lh_ftex[VKN_UILT_N], lh_layer_only, lh_renamed, lh_rename_fail, lh_spares, lh_spare_bytes, lh_ds_ok, lh_ds_bad;
    uint64_t lh_g0_det, lh_g0_keep, lh_area, lh_area_px, lh_area_full, lh_area_restart, lh_alias_live, lh_cont;
    uint64_t lh_vdouble, lh_vdiffer;   // UILAYER=2: runs drawn twice (in order, then hoisted) and compared; bands that differed (total)
    uint64_t lh_segs, lh_seg_ds, lh_seg_rl;   // hoisted runs' segments beyond the first; started by the DS check / by a pushed read of the layer
    uint64_t lh_seg_reuse;                    // [mgpu P7c] segments the hold started at a reuse (no texture free / content drawn on top)
    // [mgpu P7c] the filter cache's pages (made, bytes alive), hits answered by a redirect (no copy), redirects made real by a copy (a page
    // region about to be reused); pushed bindings of the hold's replay made real at their first use / never used (never bound)
    uint64_t fc_pages, fc_page_bytes, fc_rd_hits, fc_rd_real, lh_lazy_bound, lh_lazy_skipped;
    uint64_t lh_dsclip, region_dsclip;   // [mgpu P7c] region epochs proven DS-dead after them; their DS draws / masked clears clipped to the region
} vkn_ui_pass_stats;
// [mgpu P7] flush reasons: an engine draw / depth read on GDraw's binding (a custom draw), a conflicting engine use, the layer used by the
// engine (Combine), a pass end (a piece, the recording's close), the size limit, other
enum { VKN_UILF_BOUND, VKN_UILF_CONFLICT, VKN_UILF_LAYER, VKN_UILF_END, VKN_UILF_SIZE, VKN_UILF_OTHER, VKN_UILF_N };
// [mgpu P7] not hoisted: no layer named, a pushed target's depth-stencil use, a pushed draw reads the layer / depth, a pushed target written
// after the layer read it, too many textures read
enum { VKN_UILU_NOLAYER, VKN_UILU_DS, VKN_UILU_READL, VKN_UILU_REUSE, VKN_UILU_MANY, VKN_UILU_N };
// [mgpu P7] cache refusals: no layer, a texture without a version, a vertex range, too many targets, a target drawn before the run cleared
// it, no layer content, no remembered clear of the layer, the first candidate, written since, another key
enum { VKN_UICR_NOLAYER, VKN_UICR_TEX, VKN_UICR_VB, VKN_UICR_MANY, VKN_UICR_INIT, VKN_UICR_NOL, VKN_UICR_NOCLEAR, VKN_UICR_FIRST, VKN_UICR_DIRTY, VKN_UICR_KEY, VKN_UICR_N };
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

// ==== [mgpu P7] the UI layer in one pass and the whole-UI cache (vkn_ui.c block "mgpu P7" has the rule) ====
// DM_VK_NATIVE_UILAYER: 0 off / 1 filter blocks executed before the layer (one layer pass) / 2 verify / 3 poison; DM_VK_NATIVE_UICACHE:
// 0 off / 1 an unchanged UI run is not executed again / 2 verify / 3 poison; -1 = family default (render-pass tilers 1, else 0). Any GPU.
// Install time. Every vkn_ui_* call above is held while either is on (and executed at the run's flush).
// [mgpu P7b] UILAYER 1 also: a run with the layer alone is hoisted (its fences dropped, one pass); a pushed target GDraw binds again after
// a layer draw of the run read it is renamed at its region clear (the new content in another texture of its shape: the target itself or a
// spare of this module, at most 4) instead of running the run in order; a pushed depth-stencil use or a pushed draw that reads the layer
// splits the run into segments, each hoisted inside itself, where a symbolic depth-stencil check (or the layer read) asks for it; the
// layer epoch takes its depth-stencil (attached only when a layer entry tests / writes it, or the engine draws into the layer with it
// right after the run) and its render area (the union of the run's layer rects) from the run. UILAYER 2 draws a run whose layer begins
// with a remembered whole clear twice (in its order, then hoisted) and compares a band of the layer. Run log: DM_VK_NATIVE_PASSLOG.
void vkn_ui_layer_config(int layer, int cache);
void vkn_ui_layer(vkn_tex *L);   // the host (RenderingBegin, a record): GDraw's base colour target (the layer)
void vkn_ui_layer_flush(void);   // a held run executed now (tests)

// ==== [rt P6] DM_VK_NATIVE_UISET (vkn_ui.c "[rt P6]"): 0 a fresh set per UI draw (as before), 1 sets of the recording by content (the
// copies' offsets as dynamic offsets), 2 = 1 without the reuse (fresh sets, lookups counted). Install time. Totals of hits / misses / would-hit (mode 2).
void vkn_ui_set_cache(int mode);
void vkn_ui_set_cache_stats(uint64_t *hits, uint64_t *miss, uint64_t *would);
#ifdef __cplusplus
}
#endif
#endif
