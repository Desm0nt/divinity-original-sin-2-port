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
uint64_t vkn_query_draw_cond_key(void);          // [VK-ENGINE-SIDE I2] the report-pair key of the draw's condition (set by vkn_query_draw_enter, 0 = none)
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

// ==== [DOS2] VK-NATIVE stage 5 technique A (docs/dos2/nr/VK-OCCLUSION-ALT.md section 4; model and premises: vkn_occ.h) ====
//   5 A        carried verdicts: a draw of a pair whose object's last verdict is "0 samples" is not recorded while vkn_occ.c proves nothing
//              it depends on changed (its pre-pass draws, then its conditional draws when all of them were proven); empty pairs skip
//              their conditional draws exactly (as 1); queries only to learn (0..DM_VK_NATIVE_COND_B a frame, vkn_occ.c chooses), read
//              when the slot retired; no GPU predicates (no extension needed). Never the default (Adreno: 0).
//   6 verify   a query on every pair every frame, everything drawn (as 2); A's decisions made all the same and checked against the
//              query of the same frame when its slot retires: violations counted / printed red (vkn_occ_census).
// DM_VK_NATIVE_COND_B (read in vkn_query_init): the learning budget, default 64 on Adreno, 16 on tile GPUs (vendor / DM_GFX_SIM_GPU).
enum { VKN_COND_A = 5, VKN_COND_A_VERIFY = 6, VKN_COND_MODES_ALL = 7 };
// For vkn_occ.c (recording thread): the open pair (-1 none), the draw's condition (-1 unconditional, -2 names no pair, else the pair),
// facts of pair `rec` of this recording (0 = ok), the pairs so far, and "the draw being made is skipped by an exact decision".
int vkn_query_occ_open(void);
int vkn_query_occ_cond(void);
int vkn_query_occ_rec(uint32_t rec, uint32_t *seen, uint32_t *done, int *closed, int *empty);
void vkn_query_occ_nrec(uint32_t *n);
void vkn_query_occ_skipped(void);

// ==== [DOS2] VK-NATIVE stage 5 technique B (docs/dos2/nr/VK-OCCLUSION-ALT.md section 5; model, exactness, latency: vkn_occb.h) ====
//   7 B        group queries + group conditions (Adreno): the pre-pass draws of objects last seen hidden are drawn at the end of their
//              pass instance in 8 buckets, one occlusion query per bucket; their "full replacement" G-buffer draws at the end of their
//              pass instance under one VK_EXT_conditional_rendering bracket per bucket; learning queries on up to DM_VK_NATIVE_COND_B
//              (default 32) other pairs a frame. No query and no condition on a single object of a group. Needs the extension and a
//              non-tile family, else DM_VK_NATIVE_COND=0 with a line. Never the default (Adreno: 0, VK-STAGE5-PLAN.md section 7).
//   8 verify   the same captures and order, each member's pre-pass draws in their own query, the captured G-buffer draws drawn
//              without a condition each in its own query, every other pair queried at its place (as 2); a G-buffer draw whose group
//              passed 0 samples must pass 0 itself: violations counted / printed red (vkn_occb_census). No predicate anywhere.
//   9 A + B    both (design 5: "A and B compose"): A's CPU skips first (vkn_occ.c as in mode 5, its learning queries kept), B's groups
//              for the rest (as mode 7; a pair with a query - A's learning or B's - is never a group member). Without the extension or
//              on a tile family: mode 5 with a line. Exact as each part: A skips only draws its proof says write nothing (their
//              samples are 0, so a group query without them reports the same); B moves only order-free draws, so the final depth A's
//              proof is about is the same in any order, and A's view of a frame (B's captures are "not recorded" there) is the same
//              in two frames only if B captured the same draws - the same GPU order.
// vkn_cond_is_a / vkn_cond_is_b say which module(s) a mode drives.
enum { VKN_COND_B = 7, VKN_COND_B_VERIFY = 8, VKN_COND_AB = 9, VKN_COND_MODES_B = 10 };
static inline int vkn_cond_is_a(int m) { return m == VKN_COND_A || m == VKN_COND_A_VERIFY || m == VKN_COND_AB; }
static inline int vkn_cond_is_b(int m) { return m == VKN_COND_B || m == VKN_COND_B_VERIFY || m == VKN_COND_AB; }
// 1 = the mode in effect may record GPU predicates (1, 3, 7, 9) or redraw captured draws at a pass end (7, 8, 9): the pass module's
// cover proof of the first draw refuses then (a predicated or replayed draw can be the pass's first). Modes 2 / 4 / 5 / 6 record neither.
int vkn_query_gpu_pred(void);
// ==== [stage 5 technique Z] depth snapshot + engine boxes, decided on the engine thread (vkn_occz.h; docs/dos2/nr/VK-OCCLUSION-CHEAP.md) ====
//   10 count   the pairs are tracked without any query; the engine thread decides every pair of every presented frame and counts what it
//              would skip (and its own cost against the engine's slack); everything drawn. The self-check runs on every snapshot.
//   11 verify  as 10 + a query on every pair every frame (as 2): the GPU's own verdict of the pairs Z called hidden is counted.
//   12 on      the worker skips the pre-pass draws and the conditional draws of the pairs Z called hidden (one verdict lookup a pair
//              draw), and the conditional draws of empty pairs (exact, as 1). A self-check violation turns the skips off for the session.
//   13 poison  the worker skips the pairs Z trusts but sees VISIBLE: visible objects vanish (the decisions reach the draws).
// Never the default: the count-only mode decides (net gain > 0 at the measured own cost, owner rule 21.11).
enum { VKN_COND_Z = 10, VKN_COND_Z_VERIFY = 11, VKN_COND_Z_ON = 12, VKN_COND_Z_POISON = 13, VKN_COND_MODES_Z = 14 };
static inline int vkn_cond_is_z(int m) { return m >= VKN_COND_Z && m <= VKN_COND_Z_POISON; }
uint64_t vkn_query_occ_key(uint32_t rec);   // the pair key (the engine's report object) of record rec of this recording, 0 = none

// For vkn_occb.c (recording thread): a query of this recording's slot for B's own bracket (0 = ok, -1 = no slot / pool full; its
// result is copied with the pass's others and handed to vkn_occb_q_result), the predicate word of a query of this slot, a
// conditional-rendering bracket on a word (on 1 begin / 0 end; -1 = no extension), "pair rec gets its own query" (a learning pair:
// its first recorded draw begins it), and "end the open pair's active query now" (it counts as visible: B's own query comes).
int vkn_query_b_qalloc(VkQueryPool *pool, uint32_t *q);
void vkn_query_b_pred(uint32_t q, VkBuffer *buf, VkDeviceSize *off);
int vkn_query_b_cond(VkCommandBuffer cb, int on, VkBuffer buf, VkDeviceSize off);
void vkn_query_b_learn(uint32_t rec);
int vkn_query_b_noq(uint32_t rec);   // 1 = pair rec of this recording has no query (only such a pair's draws may be captured)
void vkn_query_b_end_active(VkCommandBuffer cb);

#ifdef __cplusplus
}
#endif
#endif
