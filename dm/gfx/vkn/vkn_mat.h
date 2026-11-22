// [DOS2] VK-NATIVE stage 5.2 B5: MAT + XLQ on the backend (vkn_mat.c; one vkn_fx function, order 200). The old path's NR-33B
// (dm/gfx/vulkan/engine/xbg_nr_mat.inc, docs/dos2/nr/NR-33B.md) and NR-32 (xbg_game.cpp vsout_module, docs/dos2/nr/NR-32.md) ported
// to mode 1, where the translator is not initialised. The pure parts are used as they are: dm/gfx/nr/nr_mat.{h,c} (module facts, the
// gate records of <shaders>/native-nr33b.xgspack, the alpha rule), xbg_spirv_vsout.cpp (the one vertex-output trim of the tree, through
// vkn_mat_lib.cpp), the library's final words / modules / pack reader (vkn_shlib.h block "stage 5.2 B5").
//
// MAT (a draw in a pass with no colour target: depth pre-pass, shadow atlas, any depth-only set). The engine's fragment shader is dropped
// (vkn_fx_out.no_fs: a depth-only pipeline of the same vertex module) when it provably has no effect there, exactly the old rule:
//   (a) the target set has no colour target (and the draw is not discarded / swapped by an earlier technique; the backend has no alpha
//       to coverage and no tessellation / geometry stage);
//   (b) the fragment shader's final words (every load-time rewrite applied) have no fragment built-in output (depth / sample mask /
//       stencil), no side effect (image / storage writes, atomics) and a plain interface (nr_mat_scan; NR_MAT_COMPLEX_IO / _BAD refuse);
//   (c) no fragment can be killed: no kill instruction and the alpha test off (or refused by the library: the base module is drawn), or
//       the draw's constant blocks satisfy a path of the shader's gate record (nr_mat_gate_eval on the fragment blocks as they lie in the
//       ring now: vkn_draw_ubo_get + vkn_ring_cpu; an unannounced block / a word past its size fails its path) and the alpha test is off
//       or passes the path's constant Location 0 .w (nr_mat_alpha_pass with the draw's GREATER reference). A record is used only when its
//       SPIR-V hash equals the bundle's own SPIR-V bytes (stale = red line, ignored).
//   Then depth / stencil / occlusion results are the rasterised ones the shader would leave, and the colour outputs go nowhere (no
//   attachment): bit-equal depth. Variant B also swaps the vertex shader for its cut-down twin: NR-32's trim with read = declared = 0
//   on the final words (retyped first for the draw's vertex format, as vsout_module did: "#vi" then "#vo"), accepted only when
//   xbg_spirv_vsout_check agrees (only OpStores into Location outputs / their access chains / unreferenced Output variables removed:
//   every instruction gl_Position depends on is the same instruction - bit-equal position; the pack-wide proof of this trim at mask 0
//   is tools/dos2_nr_mat_proof.py / tools/dos2_xlq_check.py, NR-32.md section 5). The twin is a piece's shader (vkn_fx_shader_create):
//   the engine's vertex-stage bindings, C1 fed as an effect block (vkn_fx_out.ubo[0]) from the engine object's static block.
//   [stage 5 rest] Now the twin is NOT an object: the trimmed module rides on the engine shader object as vkn_fx_out.vin (vs + vs_id,
//   vfmt 0; vkn_fx.h stage 5.3 V1 block): the same binding table, layout and C1 feed as the engine draw (the trim only removes output
//   stores, so the module reads a subset of the engine vertex stage's bindings). No effect-shader table limit (was 896 objects).
// XLQ (DM_VK_NATIVE_XLQ, every vertex + fragment draw on a depth target that MAT did not change): the vertex shader without the stores to
//   output components the fragment shader never reads (xbg_spirv_fs_input_mask / _vs_output_mask, the same trim and check, keep = the
//   read mask, the declared inputs stay on the interface). [stage 5 rest] Drawn as vkn_fx_out.vin on the engine object, like variant B:
//   the fragment module is the draw path's own (base / "#at" for the draw's reference), one module per (shader, retype code). Refused (counted): NR-04 named entries (their fragment module depends on the pass),
//   passes without a depth target (full-screen passes: lighting / post pieces own those shaders), shaders past this piece's table.
//
// Interplay. The shadow cache (order 100) replays stored casters through vkn_fx_bypass: they do not pass the chain. [fixpp] vkn_shc.c
//   replay() asks vkn_mat_apply below for the same decision (after the draw's blocks are announced again; ctx: shader, depth, vfmt) and
//   draws under vkn_fx_force(out), putting the chain's output of the engine draw in hand back afterwards. DM_VK_NATIVE_GBUF_LE (vkn_query.c,
//   order 250 / 1000) reads the pre-pass draw as it finally became: no_fs alone keeps its exactness argument (no fragment stage = no
//   kill / depth / mask: the pre-pass z is the rasterised z, bit for bit the same as with the empty-effect fragment shader) and is
//   accepted; a swapped vertex shader (variant B / XLQ) is refused there ("swapped") - exact either way, only the LEQUAL gain is lost
//   for those objects (GBUF_LE is gated off on the tilers where B is the default).
//
// Switches (read once at install; the old names DM_NR_MAT / DM_NR_MAT_VS / DM_NR_XLQ are forced to 0 in mode 1 by dm_dos2_vkn_pre):
//   DM_VK_NATIVE_MAT (replaces DM_NR_MAT + DM_NR_MAT_VS)  0 off; 1 = no fragment stage only (variant A); 2 = 1 + the cut-down vertex
//       shader (variant B); 3 = verify: every decision made and counted, the engine's shaders drawn. Default: 2 on Mali / PowerVR /
//       Maleoon (the old app table set DM_NR_MAT=1 everywhere, DM_NR_MAT_VS defaulted to 1), 1 elsewhere (Adreno / Turnip and AMD link
//       the stages and drop dead outputs themselves; variant A keeps GBUF_LE working there). DM_GFX_SIM_GPU mali / pvr acts as that family.
//   DM_VK_NATIVE_XLQ  (replaces DM_NR_XLQ)  0 off, 1 on, 2 count (trims made and checked, the engine's shaders drawn), 3 poison (the
//       lowest read location goes too: xlq_keep mode 3, never a default). Default xbg_nr_xlq_default: 1 Mali / PowerVR / Maleoon, else 0.
//   A / B phone runs that pick between 1 and 2 (SIM mali alone shows no tiler cost): real Mali, standing, MAT=1 vs MAT=2 (GPU ms, `[vkn]
//   mat` swap counts), and on the Mi 14 MAT=1 vs MAT=2 with DM_VK_NATIVE_GBUF_LE on (B costs the LEQUAL gain of swapped objects).
// Census: one line every 10 s from the chain (`mat:`), red lines 【红】 for the unexpected (first 16 of each kind). Recording thread
// only, except the install. The first sight of a shader (and of a retype code / reference) loads its words on a thread with an 8 MB
// stack and waits for it (once per shader: the chain's rewrites need the stack). No game addresses.
#ifndef DM_VKN_MAT_H
#define DM_VKN_MAT_H
#include "vkn_fx.h"
#include "../nr/nr_mat.h"
#ifdef __cplusplus
extern "C" {
#endif

// Reads the switches (defaults by the GPU family of vendor / device name: the library's physical device), registers "mat" (order
// 200); idempotent. vkn_b5_install (vkn_shlib_init's one call) calls it.
void vkn_mat_install(uint32_t vendor, const char *device_name);
int vkn_mat_mode(void), vkn_mat_xlq(void);  // in effect (0 = off / not installed)
// The chain's decision for a draw that does not pass the chain (a piece's replay). Fills *out as the chain function would (no_fs,
// shader, ubo[0..1]); 1 = something set. Recording thread; the draw's constant blocks must be announced (vkn_draw_ubo) before.
int vkn_mat_apply(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out);
// [VK-SHADOWS-MOVING] the verdict alone (1 = MAT would drop the fragment stage of this depth-only draw), counters untouched
int vkn_mat_peek(const vkn_draw_in *in, const vkn_fx_ctx *ctx);

enum { VKN_MAT_W_FACTS, VKN_MAT_W_NOLIB, VKN_MAT_W_GATE, VKN_MAT_W_ALPHA, VKN_MAT_W_KILL, VKN_MAT_W_PENDING, VKN_MAT_W_NOTENG, VKN_MAT_W_N };
enum { VKN_MAT_V_NOTHING, VKN_MAT_V_CHECK, VKN_MAT_V_TABLE, VKN_MAT_V_MODULE, VKN_MAT_V_CODES, VKN_MAT_V_WORDS, VKN_MAT_V_KIND, VKN_MAT_V_VIN, VKN_MAT_V_N };
enum { VKN_MAT_X_FS, VKN_MAT_X_NAMED, VKN_MAT_X_NODEPTH, VKN_MAT_X_NOTHING, VKN_MAT_X_CHECK, VKN_MAT_X_TABLE, VKN_MAT_X_MODULE, VKN_MAT_X_VARIANTS,
       VKN_MAT_X_WORDS, VKN_MAT_X_KIND, VKN_MAT_X_VIN, VKN_MAT_X_N };
typedef struct vkn_mat_stats {
    uint64_t seen, no_fs, would, pure, gate_off, gate_alpha, alpha_na, refused[VKN_MAT_W_N];   // depth-only draws
    uint64_t vs_swaps, vs_refused[VKN_MAT_V_N];                                               // variant B
    uint64_t xlq_seen, xlq_swaps, xlq_would, xlq_refused[VKN_MAT_X_N];
    uint64_t learned, gates_loaded, gates_stale, gates_bad, twins, xlq_shaders, stores, outputs, reds;
    int pack;                                   // native-nr33b.xgspack: 1 open, 0 none, -1 damaged, -2 not asked yet
} vkn_mat_stats;
void vkn_mat_stats_get(vkn_mat_stats *s);
void vkn_mat_census(void);                  // the line (also printed by the chain every 10 s)
const char *vkn_mat_reason_name(int kind, int i);   // kind 0 MAT (VKN_MAT_W_*), 1 cut VS (_V_), 2 XLQ (_X_)

// ---- host tests ----
// What the library would answer for a shader (its facts): fs_flags = nr_mat_facts.flags of the fragment words (-1 = unknown), gate
// (NULL none; copied), alpha_applied = the "#at" variant differs from the base module, the read / declared / written masks for XLQ.
// vs_module: the module the cut-down vertex shader would get for (code, keep) (VK_NULL_HANDLE = the trim refused).
typedef struct vkn_mat_test_facts {
    int fs_flags; const nr_mat_gate *gate; int alpha_applied;
    uint64_t fs_read, fs_decl, vs_written;
    VkShaderModule (*vs_module)(const vkn_shader *s, uint64_t code, uint64_t keep);
} vkn_mat_test_facts;
typedef int (*vkn_mat_test_fn)(const vkn_shader *s, vkn_mat_test_facts *f);   // 0 = known
void vkn_mat_test_setup(int mode, int xlq, vkn_mat_test_fn fn);   // forgets every verdict; registers the function once

#ifdef __cplusplus
}
#endif
#endif
