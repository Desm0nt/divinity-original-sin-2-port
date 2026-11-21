// [DOS2] VK-NATIVE stage 5.2 B4: the depth pre-pass hybrid on the backend (vkn_prepass.c; one vkn_fx function "prepass", order
// 220). The old path's DM_DOS2_PREPASS / NR-30 (xbg_game.cpp namespace prepass, docs/dos2/PREPASS.md,
// docs/dos2/nr/NR-30.md) ported to mode 1. The model is dm/gfx/vulkan/engine/xbg_prepass.h AS IT IS (pure C: the per-frame key table,
// the previous frame's history, the admission limit, the claims, the hazards, the retire check, the fallback; host test prepass_test.c:
// depth and colour bit for bit against drawing everything); this file only feeds it what the old host fed it from the NVN state:
//   pass        a target set with no colour target on a depth target of >= 640x360 that is not square = the scene pre-pass candidate
//               (prepass_is_depth_only; the 1536x1536 shadow atlas is square); a set with colour targets on the open table's depth
//               target = the G-buffer side; every other draw costs one pointer compare (ctx->depth) and returns.
//   key         prepass_key(engine index buffer handle, first index, count, base vertex 0, instances, base instance 0) of an indexed
//               draw (the engine's draws have no base vertex / instance); arrays draws have key 0 (never skipped, as before).
//   facts       fragment class 0 none / 1 empty main (the old empty_fs rule on the library's final words: one function whose body is
//               only OpLabel / OpReturn) / 2 kills (vkn_shlib_fs_facts KILL: OpKill / OpTerminateInvocation / OpDemote) / 3 other;
//               unknown (no library) = 3, a kill unknown on the G-buffer side = kill. Depth test / write / func (Vulkan op + 1 = the
//               NVN func the model counts in) and stencil from the engine's depth-stencil content id (vkn_ds_args); depth bounds: the
//               backend has none; alpha test = in->alpha_ref >= 0; rasterizer discard = an earlier technique's out->discard.
//   raster      the raster content id (cull / fill / bias enable) + viewport + scissor (the depth bias values are one global state).
//   blend       a colour target that blends for real (vkn_blend_att(...)->blendEnable: one / zero / add is off) or whose write mask
//               (vkn_write_mask_nvn of its NVN format, what the pass builds) is not RGBA.
//   reads       the scene depth's layout is a read layout (shader read / depth read-only / transfer source / general) at a chain
//               call while candidates are unclaimed = it was sampled or copied since: prepass_note_read (the old image_read_hook).
//   counting    the occlusion interplay, the old rule (NR-03: a draw inside a report pair or with sample counting is kept,
//               PREPASS_K_COUNTING, and a later one after a skip counted as occ_after_skip): with DM_VK_NATIVE_COND != 0 a pre-pass
//               draw while a report pair is open (vkn_query_stats begins > ends) is kept - a skipped draw would leave its pair with
//               no samples and its object's conditional draws skipped. With COND = 0 (default) the pairs decide nothing: kept only
//               by the model's own rules.
// Skip = out->skip (mode 1 / 3); the G-buffer override = out->ds = vkn_fx_ds(the engine's state with LEQUAL and write on; cached per
// id). [fixpp] An override whose draw a later technique skipped is prepass_note_lost (its key is then refused): the function's
// next call sees the chain's skip counter (vkn_fx_stats.skipped) moved since the override (a skip of an earlier technique on a draw in
// between counts too: one refusal too many, never a wrong frame). No later technique changes the depth state of an overridden draw:
// GBUF_LE swaps EQUAL states only (the override is LEQUAL), the others never set ds. A draw the draw path drops after the chain (unknown
// buffer / pipeline failure: red lines of vkn_draw.c) is not seen here - the open point of this port.
// GBUF_LE (vkn_query.c): a skipped pre-pass draw never reaches its capture (refused there, exact); an overridden G-buffer draw is no
// longer EQUAL (not swapped there). The shadow cache works on the square atlas only (never this table).
// Switches (read once at install; DM_DOS2_PREPASS / _HIST / _ADMIT / _LOG are the old names):
//   DM_VK_NATIVE_PREPASS        0 off, 1 hybrid skip, 2 compare (decided and checked at the frame end, nothing changes), 3 poison (skip
//                               without the override: those objects vanish; never a default). Default xbg_dos2_prepass_default: 1 on
//                               Mali / PowerVR (DM_GFX_SIM_GPU mali / pvr models them), 0 elsewhere (Adreno: plan expectation off).
//   DM_VK_NATIVE_PREPASS_HIST   history gate, default 1;  _ADMIT  newly clean keys per frame, default 32 (0 = all);  _LOG  frames per
//                               line, default 300.
// Line `prepass:` (the model's numbers of the last table, every _LOG frames) + red lines (bad keys, fallback, table memory). No game
// addresses. Recording thread only (install: any thread).
#ifndef DM_VKN_PREPASS_H
#define DM_VKN_PREPASS_H
#include "vkn_fx.h"
#ifdef __cplusplus
extern "C" {
#endif

// Defaults by the GPU family of vendor / device name (the library's physical device). Idempotent. vkn_b5_install calls it.
void vkn_prepass_install(uint32_t vendor, const char *device_name);
int vkn_prepass_mode(void);                 // in effect (0 = off / not installed; 2 after the fallback)

typedef struct vkn_prepass_stats {
    uint64_t pre_seen, gb_seen, skipped, overrides, lost, reads, counting_open, ds_full, facts_unknown, fallbacks, tables, bad_frames, bad_keys, reds;
} vkn_prepass_stats;
void vkn_prepass_stats_get(vkn_prepass_stats *s);
void vkn_prepass_census(void);              // the line of the last retired table now

// ---- host tests: the fragment class of a shader (0 none, 1 empty, 2 kills, 3 other; < 0 unknown); log every frame
typedef int (*vkn_prepass_test_fn)(const vkn_shader *s);
void vkn_prepass_test_setup(int mode, vkn_prepass_test_fn fn);
// the model's last retired table (xbg_prepass.h prepass_result; NULL = none)
const void *vkn_prepass_test_last(void);

#ifdef __cplusplus
}
#endif
#endif
