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
//   3 poison   as 1 with the predicate inverted: visible objects that have a test disappear (proves the conditions reach the draws).
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
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_COND_OFF = 0, VKN_COND_ON = 1, VKN_COND_COUNT = 2, VKN_COND_POISON = 3, VKN_COND_MODES = 4 };
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

#ifdef __cplusplus
}
#endif
#endif
