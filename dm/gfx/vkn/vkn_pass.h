// [DOS2] VK-NATIVE stage 3, render passes (vkn_pass.c): the engine's "set render targets / clear / copy" events as Vulkan render
// passes. Design: docs/dos2/nr/VK-NATIVE.md section 4.3 (the rule is the iPad Metal backend's):
//   - a pass is opened LAZILY, by the first draw after the targets changed (vkn_pass_ready);
//   - a clear that arrives while no pass is open is remembered on the texture and becomes loadOp CLEAR of the next pass that has the
//     texture as an attachment; an attachment is LOADed only if it was written in this frame (or carries data from an earlier frame
//     that the engine did not clear: see "stale" below), else DONT_CARE; a remembered clear whose texture is used otherwise first
//     (sampled, copied, presented) is executed as a command of its own (vkCmdClearColorImage / vkCmdClearDepthStencilImage);
//   - a clear that arrives while a pass with that attachment is open: vkCmdClearAttachments;
//   - before a texture is sampled or copied its layout is made right: if it is an attachment of the open pass the pass ends first
//     (and reopens with LOAD at the next draw).
// "stale": a target not written in this frame and not cleared may still be read by the engine on purpose (history buffers, the
// shadow atlas kept across frames): its content is kept (LOAD) unless the texture was never written at all. So DONT_CARE is used
// only for never-written targets; the tile-GPU refinement (DONT_CARE for targets fully covered by the first draw, fresh targets,
// DONT_CARE stores by history, fragment-stage read barriers) is stage 5.2 B1: the block at the end of this header (vkn_policy.h).
// Layout bookkeeping: vkn_tex.layout is the truth; this module is the only one that changes it between vkn_rec_begin and submit
// (uploads set it before). Single-threaded (the recording thread). No game addresses.
#ifndef DM_VKN_PASS_H
#define DM_VKN_PASS_H
#include "vkn.h"
#include "vkn_tex.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_PASS_COLORS = 8 };
// The engine set its targets (SetRenderTargets 0x9279a0 semantics): n colour targets (a NULL entry = that slot unbound), layer and
// mip of each (already defaulted to 0), depth target (NULL = none). Ends an open pass if the set differs. Nothing is recorded yet.
void vkn_pass_targets(uint32_t n, vkn_tex *const color[VKN_PASS_COLORS], const uint32_t layer[VKN_PASS_COLORS], const uint32_t mip[VKN_PASS_COLORS], vkn_tex *depth);
// Depth-only target set by the engine's depth clear (0x928be0 binds (0 colours, depth) when depth differs from the bound one).
// Clears. rect NULL = whole level. mode of the depth clear: bit 0 depth, bit 1 stencil.
void vkn_pass_clear_color(vkn_tex *t, uint32_t layer, const float rgba[4], const int32_t rect[4]);
void vkn_pass_clear_depth(vkn_tex *t, int what, float depth, uint32_t stencil);

// What a draw needs to know about the pass it is drawn in.
typedef struct vkn_pass_info {
    VkRenderPass pass; uint16_t compat;          // vkn_state.h render pass + its compatibility id (pipeline key)
    uint32_t ncolor, width, height, samples;
    VkColorComponentFlags write_mask[VKN_PASS_COLORS];   // by target format (vkn_write_mask_*): 0 for an unbound slot
    uint8_t has_depth, has_stencil;
    VkFormat depth_format;
} vkn_pass_info;
// Makes the pass of the current targets open in vkn_cb() (begins it if needed, with the load ops of the rule above). 0 = ok and
// *info filled; -1 = no targets / no command buffer / failure (the draw must be dropped).
int vkn_pass_ready(vkn_pass_info *info);
// A draw is about to sample `t` (called for every texture of the draw BEFORE vkn_pass_ready): transitions it to
// SHADER_READ_ONLY_OPTIMAL if needed (ending the open pass when that is required, executing a remembered clear). Returns 0 = ok,
// 1 = `t` is an attachment of the current target set (a feedback loop: the caller binds its placeholder texture instead).
int vkn_pass_sample(vkn_tex *t);
// A compute dispatch is about to use `t` as a storage image: ends the pass, layout GENERAL.
void vkn_pass_storage(vkn_tex *t);
void vkn_pass_end(void);                         // ends the open pass, if any (copies, submit; registered as a close listener of vkn_frame)
// The engine's texture copy (0x920b00 semantics): formats equal; both 1 level: one copy of min(depth) layers; else for i < dst
// levels: src level (src levels - dst levels + i) -> dst level i.
void vkn_pass_copy(vkn_tex *dst, vkn_tex *src);
// Transition for a use outside this module (present: TRANSFER_SRC_OPTIMAL; read-back). Ends the pass if `t` is in it.
void vkn_pass_layout(vkn_tex *t, VkImageLayout to);

typedef struct vkn_pass_stats { uint64_t passes, load, clear, dontcare, clears_folded, clears_cmd, clears_att, breaks_sample, feedback, copies, transitions; } vkn_pass_stats;
void vkn_pass_stats_get(vkn_pass_stats *s);

// ==== added by the pass piece (stage 3): what the contract above lacked ====
// Install time (dm_vkn_pass_register): registers the close / begin listeners of vkn_frame. Idempotent; every entry point above also
// calls it, so a caller that forgot it still gets the listeners before the first pass opens.
void vkn_pass_init(void);
// The depth target of the current set (the one NVN would have bound: 0x928be0's depth-only bind changes it), NULL = none. The
// engine's ClearDepthStencil clears whatever depth target is bound, which is not always the one its call names.
vkn_tex *vkn_pass_depth(void);
// Drop reasons and other counters of the census (the header's vkn_pass_stats keeps its fields).
typedef struct vkn_pass_stats2 {
    uint64_t no_cb, no_targets, bad_target, dead_target, samples_mismatch, size_mismatch, rp_fail, fb_fail;   // pass not opened
    uint64_t sampled_undefined, load_stale, clear_bad, clear_lost, region_pass, copy_skip_fmt, copy_drop, copy_blit;
} vkn_pass_stats2;
void vkn_pass_stats2_get(vkn_pass_stats2 *s);

// ---- debug (DM_VK_NATIVE_DUMP): see vkn_pass.c ----
int vkn_pass_dump_active(void);
vkn_tex *vkn_pass_color(uint32_t i);

// ==== [stage 5 A3] the shadow cache (vkn_shc.c): what the contract lacked ====
// The current target set as vkn_pass_targets last got it (n slots, their layer / mip, depth): a vkn_fx function that drew into its own
// targets puts it back with vkn_pass_targets (vkn_fx.h: the set must be the engine's when the function returns without skip).
void vkn_pass_cur_get(uint32_t *n, vkn_tex *color[VKN_PASS_COLORS], uint32_t layer[VKN_PASS_COLORS], uint32_t mip[VKN_PASS_COLORS], vkn_tex **depth);
// One watched texture (the shadow atlas): fn runs BEFORE this module uses it in another way than as an attachment of the open pass -
//   VKN_PASS_EV_CLEAR   vkn_pass_clear_depth of its depth aspect (depth = the value; every depth clear here is of the whole level)
//   VKN_PASS_EV_SAMPLE  vkn_pass_sample (a draw's or a dispatch's texture binding is about to read it)
//   VKN_PASS_EV_USE     vkn_pass_layout / vkn_pass_storage / vkn_pass_copy (source or destination)
// fn may call this module (the watch does not fire again while fn runs) but must not record draws: SAMPLE comes from inside a draw's
// texture binding. vkn_pass_watch(NULL, NULL) = none. Recording thread.
enum { VKN_PASS_EV_CLEAR = 1, VKN_PASS_EV_SAMPLE = 2, VKN_PASS_EV_USE = 3 };
typedef void (*vkn_pass_watch_fn)(vkn_tex *t, int ev, float depth);
void vkn_pass_watch(vkn_tex *t, vkn_pass_watch_fn fn);

// ==== [stage 5.2 B1] render-pass load / store policy for tile GPUs (vkn_pass.c "stage 5.2 B1"; switches and family defaults:
// vkn_policy.h) ====
// What is in effect (all 0 = the stage-4 rule of this header: the module default, so host tests and other callers keep it):
//   load     DM_VK_NATIVE_LOAD    0 / 1 cover proof of the first draw -> loadOp DONT_CARE / 2 verify (counted, LOAD kept) / 3 poison
//   store    DM_VK_NATIVE_STORE   0 / 1 storeOp DONT_CARE by history / 2 verify / 3 poison (STORE, then magenta / depth 0.5 in the pass)
//   fresh    DM_VK_NATIVE_FRESH   0 / 1 CLEAR to zeros for never-written targets / 2 + shared-storage aliases DONT_CARE / 3 = 2 + poison
//   tilebar  DM_VK_NATIVE_TILEBAR 0 / 1 fragment-stage read barriers of fragment-only draws
//   area, ubo (DM_VK_NATIVE_LOAD_AREA / _LOAD_UBO): a proven part of the target as render area / positions from uniform blocks (1 = on)
//   store_frames (DM_VK_NATIVE_STORE_FRAMES): frames of "not read" before a store is discarded (default 8)
// [mgpu P4] store_key (DM_VK_NATIVE_STORE_KEY): 0 = the history key is the chain of every earlier set of the recording (stage 5.2: a UI
// pass count that moves by one re-keys every later pass - the 0 / 143 discards of U13r); 1 = (the set, how many times this set began a
// pass before in this recording). [mgpu P6.1] join (DM_VK_NATIVE_PASSJOIN): 0 off / 1 a depth-only pass opens as the G-buffer pass that
// followed it in the last frames (vkn_pass.c "pass join") / 2 count only / 3 = 1 + checks. Both 0 in a vkn_pass_policy_set of zeros.
typedef struct vkn_pass_policy { int8_t load, store, fresh, tilebar, area, ubo; uint16_t store_frames; int8_t join, store_key; } vkn_pass_policy;
// Exact values now (host tests; NULL = all off). A request (the host at install, from the environment) may carry -1 = the family
// default: it is applied at the first recording begin with a device (the family is known then: vkn_shlib_policy, else vkn_policy_family_from),
// with one log line. The effect function (vkn_fx.h, "pass-cover", order 900) is registered when load or tilebar is on.
void vkn_pass_policy_set(const vkn_pass_policy *p);
void vkn_pass_policy_request(const vkn_pass_policy *p);
void vkn_pass_policy_get(vkn_pass_policy *p, int *family);
// The shader facts of the cover proof: the vertex stage's position program (dm/gfx/nr/nr_cover.h), the fragment stage's Output
// locations (bit = location) and VKN_SHLIB_FS_* bits (kill / depth / sample mask). Default: the shader library (vkn_shlib_vs_pos /
// _fs_outputs / _fs_facts, weak); host tests install a stand-in (NULL = back to the default). 0 = filled, else no facts.
struct nr_cv_prog; struct vkn_shader;
typedef struct vkn_pass_cover_sh { const struct nr_cv_prog *pos; uint32_t fs_outputs, fs_facts; } vkn_pass_cover_sh;
void vkn_pass_cover_facts_fn(int (*fn)(const struct vkn_shader *s, vkn_pass_cover_sh *out));
// Outcome of the proof, by reason (VKN_PCV_FULL / _AREA = proven; the rest refusals, first failing check).
enum { VKN_PCV_FULL, VKN_PCV_AREA, VKN_PCV_NOFACTS, VKN_PCV_STALE, VKN_PCV_FX, VKN_PCV_COND, VKN_PCV_SAMPLES, VKN_PCV_SHADER, VKN_PCV_NOPROG,
       VKN_PCV_OUTPUT, VKN_PCV_BLEND, VKN_PCV_MASK, VKN_PCV_DS, VKN_PCV_RASTER, VKN_PCV_PRIM, VKN_PCV_INPUT, VKN_PCV_BUFFER, VKN_PCV_UBO,
       VKN_PCV_VERTEX, VKN_PCV_EMPTY, VKN_PCV_GEOMETRY, VKN_PCV_CULLED, VKN_PCV_SMALL, VKN_PCV_AREA_CLEAR, VKN_PCV_3D,
       VKN_PCV_HINT /* [B1 + B2] proven, but not for the whole hinted render area */, VKN_PCV_N };
const char *vkn_pass_cover_name(int why);
// Why an attachment began without LOAD (counted per attachment): CLEAR of a remembered clear, the cover proof (whole / area), a
// shared-storage alias (fresh 2), CLEAR to zeros (fresh 1), never written (the stage-3 DONT_CARE).
enum { VKN_PDC_CLEAR, VKN_PDC_COVER, VKN_PDC_AREA, VKN_PDC_ALIAS, VKN_PDC_ZERO, VKN_PDC_NEVER, VKN_PDC_N };
typedef struct vkn_pass_stats3 {
    uint64_t frames;                                  // vkn_frame() when read (the census divides deltas by it)
    uint64_t att_load, att_nload[VKN_PDC_N];          // attachments begun LOAD / not LOAD by reason
    uint64_t would_dc;                                // load 2: attachments the proof would have begun DONT_CARE
    uint64_t cover_tried, cover_why[VKN_PCV_N], cover_exact, cover_depth;   // proofs run / by outcome; proven positions copied as they are; depth DONT_CARE
    uint64_t area_passes, area_restart_draw, area_restart_clear, area_restart_other;
    uint64_t poison_load, cover_lost;                 // attachments poisoned after the begin; a rule's pass whose first draw was not recorded
    uint64_t att_store, store_dc, store_would, store_poison, store_noelig, store_banned;
    uint64_t violations, would_violations, bans, table_full;
    uint64_t load_bytes, store_bytes, load_saved_bytes, store_saved_bytes, would_bytes, store_would_bytes;   // the model: w x h x texel bytes
    uint64_t tilebar_frag, tilebar_full, tilebar_fix;   // read barriers fragment-only / full; full barriers owed to a later non-fragment read
    // [B1 + B2] the UI's area hint: hints given; passes begun narrowed by one; hints not applied (empty after the clip, a remembered
    // CLEAR, a poison mode); narrowed passes ended by vkn_pass_targets of the same set; depth targets marked "never discard the store"
    // (the same colours seen with and without them) and stores kept for that reason
    uint64_t hints, hint_passes, hint_empty, hint_clear, hint_poison, area_restart_targets, store_keep_marked, store_keep;
    uint64_t hint_engine;   // not applied: the pass's first draw is an engine draw that reaches outside the hint
} vkn_pass_stats3;
void vkn_pass_stats3_get(vkn_pass_stats3 *s);
// ==== [stage 5.2 B1 + B2] the render-area hint (the UI pass, vkn_ui.h "the UI pass on tile GPUs", level 2) ====
// The passes this module opens for the CURRENT target set begin with renderArea = *area (clipped to the attachments) until the set
// changes, a recording begins, or the next call (NULL = full area again); a hint does not end an open pass. Not applied (full area)
// when an attachment begins with a remembered CLEAR or under a poison mode (load / store / fresh 3). The caller keeps every draw of
// the pass inside the area (it ends the pass before one that is not); in-pass clears outside it and vkn_pass_targets() of any caller
// - the same set included - end a narrowed pass. Combined with the cover proof: DONT_CARE only when the proven rect contains the hinted
// area (else LOAD, counted as VKN_PCV_HINT); a narrowed pass never discards a store. 0 = stored (-1: empty extent, the hint is dropped).
// A pass whose first draw is an engine draw (the effect chain saw it) reaching outside the hint begins full (the promise is the UI's).
// Recording thread (the UI runs there in every DM_VK_NATIVE_RT mode).
int vkn_pass_area_hint(const VkRect2D *area);
// 1 + the render area of the pass open in vkn_cb() for the current set; 0 = none open.
int vkn_pass_area_get(VkRect2D *out);

// ==== [mgpu P0] the pass journal (VK-MOBILE-GPU-PLAN.md П0) ====
// Every pass that ends is counted by WHY it ended (always on: the census line "pass ends"), with its tile-traffic model bytes (load +
// store of its attachments over its render area) and its draws (engine + UI). DM_VK_NATIVE_PASSLOG=k: the k-th heavy presented frame
// (>= 40 passes) prints one line per pass in order (targets by name, size, area, load / store of each attachment, MB, draws, why it
// ended) and one line of MB by reason. Recording thread.
enum { VKN_PE_TARGETS, VKN_PE_SAMPLE, VKN_PE_COPY, VKN_PE_CLEAR, VKN_PE_AREA, VKN_PE_LAYOUT, VKN_PE_STORAGE, VKN_PE_BARRIER, VKN_PE_END, VKN_PE_OTHER, VKN_PE_N };
const char *vkn_pass_end_name(int why);
typedef struct vkn_pass_stats4 {
    uint64_t frames, passes, draws, ui_draws;
    uint64_t ended[VKN_PE_N], ended_bytes[VKN_PE_N];
    uint64_t join_tried, joined, join_hit, join_miss, join_early, join_would, join_refused[6];   // [mgpu P6.1] (vkn_pass.c "pass join")
    uint64_t ord_full;                                                                        // [mgpu P4] ordinal table full (chain key used)
    // [mgpu P7] the UI's share: passes with a UI colour target (vkn_tex.ui_tgt: GDraw's targets and the filter cache atlas), their model
    // bytes (load / store over the render area) and draws; region copies from / into a UI texture (read + write bytes)
    uint64_t ui_passes, ui_load, ui_store, ui_pass_draws, ui_copies, ui_copy;
} vkn_pass_stats4;
void vkn_pass_stats4_get(vkn_pass_stats4 *s);
// ==== [mgpu P2] a region copy between two colour textures of one format (the UI filter cache, vkn_ui.c): layouts by this module ====
// (dst x, y) <- src (x, y, w, h), level 0 layer 0; the open pass ends. 0 = recorded.
int vkn_pass_copy_rect(vkn_tex *dst, int32_t dx, int32_t dy, vkn_tex *src, int32_t sx, int32_t sy, uint32_t w, uint32_t h);
// [mgpu P2] a function run at the entry of every vkn_pass_* call that records or changes state (targets, clears, copies, layouts,
// samples, ready, end): the UI cache flushes the calls it holds back. NULL = none. The hook is cleared while it runs.
void vkn_pass_before(void (*fn)(void));
// [mgpu P2] fn(t) after every GPU write this module records into t (attachment of a pass that begins, clear, copy destination, storage).
void vkn_pass_on_write(void (*fn)(vkn_tex *t));
// ==== [mgpu P7] the UI layer hold (vkn_ui.c "mgpu P7") ====
// fn(ev, a, b) at the entry of every vkn_pass_* call below, BEFORE the P2 hook and before the call reads or changes anything: TARGETS
// (the set about to change), READY (a pass for the current set), SAMPLE / STORAGE / LAYOUT / CLEAR (texture a: colour or depth clear),
// COPY (a = destination, b = source; copy and copy_rect), END (vkn_pass_end: a piece, the recording's close), DEPTH (vkn_pass_depth:
// the bound depth target is about to be read). fn may call this module (its own calls do not reach fn again). NULL = none.
enum { VKN_PBE_TARGETS = 1, VKN_PBE_READY, VKN_PBE_SAMPLE, VKN_PBE_STORAGE, VKN_PBE_LAYOUT, VKN_PBE_CLEAR, VKN_PBE_COPY, VKN_PBE_END, VKN_PBE_DEPTH };
typedef void (*vkn_pass_ev_fn)(int ev, vkn_tex *a, vkn_tex *b);
void vkn_pass_before_ev(vkn_pass_ev_fn fn);
// A remembered whole clear of colour texture t (level 0, layer 0; vkn_pass_clear_color without a rect while no pass of t was open):
// 1 + its value (take = 1: it leaves the record - the texture keeps what it holds), 0 = none.
int vkn_pass_clear_take(vkn_tex *t, float rgba[4], int take);

#ifdef __cplusplus
}
#endif
#endif
