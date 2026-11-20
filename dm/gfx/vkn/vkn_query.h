// [DOS2] VK-NATIVE stage 4, conditional rendering and visibility queries (vkn_query.c). The engine (dm/host/hle_dos2_vkn_exec.c has
// the facts with addresses) brackets an object's own proxy draw in its pre-pass with two sample-counter reports ("begin" / "end") and
// later in the SAME recording draws the object's real passes under "render if the two reports differ" (= samples passed != 0); the
// CPU never reads the counters. A condition covers exactly one draw. A pair that was never reported still renders.
//
// Here (DM_VK_NATIVE_COND, vkn_query_init):
//   0 off      (default) brackets and conditions are counted only, every draw is recorded (the picture of every build so far);
//   1 on       a bracket = one Vulkan occlusion query (not precise: only "any sample") around the draws recorded between the two
//              reports, inside one render pass instance; when that pass ends the results of every query it closed are copied into a
//              predicate word each (vkCmdCopyQueryPoolResults WAIT, then a barrier to the conditional-rendering stage), and a later
//              conditional draw is recorded inside vkCmdBegin/EndConditionalRenderingEXT on its query's word: the GPU skips it when no
//              sample passed - the Switch's decision, same frame, nothing late. Without VK_EXT_conditional_rendering (or before the
//              copy, i.e. a consumer in the same pass as its bracket) the draw is recorded unconditionally. Exact on the CPU: a bracket
//              that recorded no draw at all has zero samples, its conditional draws are skipped without recording;
//   2 count    as 1 (queries, copies, readback) but every draw is recorded: the census says how many draws the GPU would skip;
//   3 poison   as 1 with the predicate inverted: visible objects that have a test disappear (proves the conditions reach the draws);
//   4 oracle   [stage 5.1 O1, docs/dos2/nr/VK-OCCLUSION-ALT.md section 8 "step 0"] INEXACT, a measurement while standing still, never
//              for players: one presented recording in VKN_ORACLE_PERIOD is a mode-2 recording (a query on every pair, everything
//              drawn) whose verdicts are read once it retired and kept by OBJECT KEY (below); the recordings in between make no query at
//              all and skip on the CPU (vkn_fx.h skip, order 50) every draw of a pair whose key's last verdict is "no sample" - its
//              pre-pass draw and its conditional draws - and the conditional draws of empty pairs (exact). A newly revealed object stays
//              missing until the next oracle: the frame then costs what the GPU culls of COND=1 would cost without the query overhead.
// Object key (modes 2 / 4): the pair address names another object every frame, so the key is the hash of the pair's first draw as the
// old path's nr_occ.h keyed it (primitive, first, count, instances, index buffer, vertex streams, shader) mixed with its occurrence
// number among this recording's pairs of the same draw hash (repeated meshes keep their submission order while the camera stands);
// verdicts live in an nro_hist of nr_occ.h (a key seen twice in one read frame is "repeated": never skipped).
// Census (modes 2 / 4, vkn_query_census_occ from the [vkn] query census): conditional draws by pass (named by the target set) and by
// index count x instances, pairs keyed / repeated / with several draws, share of stable keys, verdict changes per read frame, what the
// GPU culls (mode 2) or the CPU skipped (mode 4) by the same pass / size classes.
// Safety: a draw is skipped only on the GPU's own result of this recording or on an exact empty bracket. A bracket becomes
// "unusable = visible" when its pass ends inside it (the query is ended there), when a draw entered the draw path inside it but was not
// recorded (dropped / empty scissor: the Switch may have drawn it), when brackets nest, when the pool is full, or in a recording
// without a query slot (off-frame recordings). A condition naming no bracket of this recording is drawn.
// Slots: a ring of query pools + predicate words, one per presented recording; a slot is reused only when vkn_frame() says its last
// recording has finished (the retirement rule of vkn.h), then its predicates of the draws it predicated are read (exact GPU culls).
// Single-threaded: the recording thread. No game addresses.
#ifndef DM_VKN_QUERY_H
#define DM_VKN_QUERY_H
#include "vkn.h"
#include "vkn_shader.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_COND_OFF = 0, VKN_COND_ON = 1, VKN_COND_COUNT = 2, VKN_COND_POISON = 3, VKN_COND_ORACLE = 4, VKN_COND_MODES = 5 };
enum { VKN_ORACLE_PERIOD = 64 };   // mode 4: presented recordings per oracle recording
enum { VKN_Q_SLOTS = 6, VKN_Q_CAP = 2048, VKN_Q_RECS = 4096, VKN_Q_CONDS = 8192 };

// Install time (mode 1 front end): the mode (clamped; 0 = nothing of this module runs) and the recording listeners. Idempotent.
void vkn_query_init(int mode);
int vkn_query_mode(void);
int vkn_query_gpu(void);   // 1 = VK_EXT_conditional_rendering entry points found (dm_vk_cond_render of the front end and the device)
// The engine's two reports of a bracket: key = its query object (opaque, non-zero; the same value the conditions name).
void vkn_query_begin(uint64_t key);
void vkn_query_end(uint64_t key);
// The next draw is conditional on `key` (0 = unconditional). The front end calls it for every draw it fills (vkn_draw_in); the draw
// path consumes it at vkn_query_draw_enter.
void vkn_query_cond(uint64_t key);
// A compute dispatch was filled: a pending condition is dropped (dispatches are never skipped here; counted as cond_dispatch).
void vkn_query_dispatch(void);

// ---- hooks of the draw and pass modules (declared weak there: a build without this file keeps today's behaviour) ----
int vkn_query_draw_enter(void);                  // top of vkn_draw_indexed / _arrays (count > 0): 1 = skip the draw (counted), 0 = go on
void vkn_query_draw_pre(VkCommandBuffer cb);     // right before the vkCmdDraw* of that draw (the pass is open)
void vkn_query_draw_post(VkCommandBuffer cb);    // right after it
void vkn_query_pass_closing(VkCommandBuffer cb); // right before vkCmdEndRenderPass
void vkn_query_pass_closed(VkCommandBuffer cb);  // right after it (outside the pass)

typedef struct vkn_query_stats {
    // brackets (engine report pairs)
    uint64_t begins, ends, queries, empty, copied, copies, broken, lost, nested, unmatched, no_slot, pool_full, open_at_close;
    // conditional draws by what happened to them
    uint64_t conds, cond_gpu, cond_cpu_skip, cond_count, count_empty, drawn_no_query, drawn_same_pass, drawn_open, drawn_unusable, drawn_no_ext,
             drawn_no_slot, drawn_poison_empty, cond_dispatch;
    // readback of retired slots: predicates of the draws the GPU judged (modes 1 / 3) or would have judged (mode 2)
    uint64_t results_read, gpu_culled, gpu_drawn, results_lost;
    uint64_t slots_taken, slots_busy, setup_fail, cpu_ns;
} vkn_query_stats;
void vkn_query_stats_get(vkn_query_stats *s);

// ==== [DOS2] VK-NATIVE stage 5.1 pieces O1 / O2 (docs/dos2/nr/VK-OCCLUSION-ALT.md sections 6, 8; VK-STAGE5-PLAN.md section 7) ====
// O1: mode 4 above and the census line (also mode 2). O2, DM_VK_NATIVE_GBUF_LE (read in vkn_query_init; technique "V"): 0 off
// (default everywhere: the rule rests on facts only the phone's target dumps confirm, see below), 1 on, 2 count only (decided, not
// applied); forced 0 on tile GPUs (vendor ARM / Imagination, DM_GFX_SIM_GPU mali / powervr models).
// A G-buffer draw that carries a condition is the object's real draw; its pair named the object's own draw in the depth pre-pass. If
// that pre-pass draw wrote depth wherever it rasterised (depth test LESS / LEQUAL with writes, stencil test off or ALWAYS, no
// discard: fragment words without OpKill / OpTerminateInvocation / OpDemoteToHelperInvocation, no FragDepth / SampleMask, no alpha-
// test variant, no effect swapped its shader or discards its primitives) and nothing raised that depth target since (no draw with
// writes under GREATER / NOTEQUAL / GEQUAL / ALWAYS, no depth clear seen pending), then the final depth D <= z of the object at every
// pixel it covers, so for the same fragments LEQUAL passes exactly where EQUAL passes - and with LEQUAL Turnip keeps LRZ (it turns
// LRZ off for EQUAL). The swap is made only when the G-buffer draw is provably the same draw: one draw in the pair, recorded, same
// depth target, primitive, index buffer / first / count / instances, vertex stream 0, rasteriser cull / fill / bias, viewport, and a
// scissor inside the pre-pass draw's. Everything else is refused and counted by reason. Not provable here (hence default 0): the two
// draws' vertex shaders give bit-equal z (EQUAL itself relies on it: where it fails the Switch shows a hole that LEQUAL may fill), and
// no piece's own draws (vkn_fx_bypass), no clear executed without any draw seeing it pending (a ClearDepthStencil while that target's
// pass is open is a vkCmdClearAttachments at once) and no texture copy writes that depth target in between.
// Implementation: vkn_fx functions "gbuf-le" (order 250: the swap, out->ds = vkn_fx_ds of the engine state with only the compare op
// changed, cached per id) and "gbuf-le-pre" (order 1000, after every other piece: what the pre-pass draw finally is, depth events).
// Refusal reasons (vkn_query_le_stats.refused[] / pre_refused[]; the census prints their names).
enum { VKN_LE_NO_PAIR = 1, VKN_LE_OPEN, VKN_LE_EMPTY, VKN_LE_SEVERAL, VKN_LE_LOST, VKN_LE_NOCAP, VKN_LE_TARGET, VKN_LE_DEPTH_EVENT, VKN_LE_DRAW,
       VKN_LE_STREAM, VKN_LE_RASTER, VKN_LE_VIEW, VKN_LE_PRE_DS, VKN_LE_PRE_FS, VKN_LE_PRE_ALPHA, VKN_LE_REASONS };
void vkn_query_le_init(int value);                // the switch (vkn_query_init calls it with the environment); idempotent
int vkn_query_le(void);                           // in effect: 0 / 1 / 2 (0 until the GPU family is known when it was asked for)
int vkn_query_le_asked(void);                     // the switch asked for 1 / 2 (the family gate may still say 0)
// Host tests: the fragment facts of a shader (VKN_SHLIB_FS_* bits, -1 unknown) instead of vkn_shlib_fs_facts; NULL = the library's.
void vkn_query_le_test_facts(int (*fn)(const vkn_shader *s));
// One or two lines (modes 2 / 4: object census; GBUF_LE != 0: swaps and refusals), from the host's [vkn] query census.
void vkn_query_census_occ(void);
typedef struct vkn_query_le_stats {
    uint64_t conds, not_equal, cand, swaps, refused[16], pre_seen, pre_refused[16], ds_table_full, depth_events, twins;
} vkn_query_le_stats;
void vkn_query_le_stats_get(vkn_query_le_stats *s);
typedef struct vkn_query_occ_stats {
    uint64_t oracle_frames, oracle_lost, keyed, repeated, several, cond_noq;
    uint64_t skip_pre, skip_cond, skip_empty, skip_idx;               // mode 4: CPU skips
    uint64_t read_frames, read_pairs, read_hidden, read_stable, read_changed;
    uint64_t cond_k[16], cond_b[6], cull_k[16], cull_b[6], skip_k[16], skip_b[6];
} vkn_query_occ_stats;
void vkn_query_occ_stats_get(vkn_query_occ_stats *s);

#ifdef __cplusplus
}
#endif
#endif
