// [DOS2] VK-NATIVE stage 5, technique B of docs/dos2/nr/VK-OCCLUSION-ALT.md section 5: "group query + group condition" (vkn_occb.c).
// DM_VK_NATIVE_COND=7 (B) / 8 (B in verify mode); learning budget DM_VK_NATIVE_COND_B (vkn_query.h has the switch). Adreno only.
//
// Why (design 2.2, Turnip at Mesa 1c3a936490): the cost of COND=1 is per OBJECT - every vkCmdEndQuery is a pipeline drain
// (CP_WAIT_FOR_IDLE) and every vkCmdBeginConditionalRenderingEXT two command-processor syncs. B makes the same GPU decision with a
// few dozen of each a frame: the objects last seen hidden are tested together, one query per GROUP, and their G-buffer draws are drawn
// together under ONE condition per group. There is no query and no condition on any single object of a group.
//
// What a group is (design 5, step 1): H = the objects whose last verdict is "0 samples". An object is keyed as the oracle keys it
// (vkn_query.c O1: the hash of its pair's first draw - primitive, ranges, instances, index / vertex buffers + offsets, shader - mixed
// with its occurrence number among this recording's pairs of the same hash). Bucket = a hash of the key mod 8 (stable membership);
// group = (bucket, segment), segment = the pre-pass render-pass instance (one per frame unless the engine breaks the pass).
//
// A frame (mode 7):
//   1. pre-pass: a draw of a pair whose object is in H (and not "to be checked") is CAPTURED (vkn_draw_in, its texture / sampler slot
//      arrays, the constant blocks its shader reads, the effect output the chain gave it, the buffer blocks it would bind) and skipped
//      at its place; at the end of the render-pass instance the captured draws are drawn again group by group, each group inside one
//      occlusion query. Exact: every captured draw is order-free (depth LESS / LEQUAL with writes, no stencil, no blending), so the
//      depth (and the winner's colour) after the pass is the same in any order; a draw that is not order-free while captures wait
//      makes them be drawn first (a flush, counted by reason), and so does a draw that would end the pass.
//   2. the group queries are copied with the pass's other queries (vkn_query_pass_closed) into predicate words;
//   3. G-buffer: a conditional draw of a member whose group's word is copied, if it is a "full replacement" (depth EQUAL without
//      writes, no stencil, no blending, no primitive rewrite), is captured too and drawn at the end of its pass instance, the group's
//      draws inside one vkCmdBegin / EndConditionalRenderingEXT on the group's word. Every other conditional draw of a member is drawn
//      at its place without a condition (as COND=0). Exact: "0 samples" for the group means no fragment of any member is nearer than
//      the final depth of the pass, so with EQUAL no member's G-buffer draw writes a pixel (the coplanar tie of design 5 aside, which
//      mode 8 measures); a group that saw a sample draws all its members as COND=0 would. EQUAL draws without depth / stencil writes
//      commute with each other except at equal depths (the same caveat); any other draw flushes the captures first.
//   4. learning: up to B (default 32) pairs a frame outside H get their own query at their place, round robin (the visible objects in
//      about 12 frames), the members of a group seen visible first ("checked one by one"); a hidden verdict puts an object into H,
//      a visible one takes it out. Verdicts are read when the query slot retired (3 frames).
// Latency (design 1: the engine's condition is about THIS frame's pair, zero frames late): B's GPU decision is about this frame too -
// the group query runs in the same pre-pass, the condition reads it in the same frame. Only the membership of H uses verdicts that are
// 3 frames old (slot retirement), and membership decides speed, never the picture: a hidden object outside H is drawn; an object in
// H that became visible is drawn in that very frame (its group's query saw it), and is then checked one by one and leaves H.
// Verify (mode 8): the same captures, reorder and flushes, but each member's captured pre-pass draws get their own query (the group
// verdict = the OR of them, the group query of mode 7 counts the same draws in the same order) and every captured G-buffer draw is
// drawn WITHOUT a condition inside its own query; every other pair is queried at its place (as mode 2). When the slot is read: a
// G-buffer draw whose group verdict was "0 samples" (B would have discarded it) must itself have passed 0 samples - else a violation
// (counted; the first 16 red with the object key, group, samples). Nothing is predicated: no extension needed.
// Mode 9 (vkn_query.h) = A + B: this module as in mode 7 next to technique A (vkn_occ.c) as in mode 5; a pair with a query (A's learning)
// is never a member, a draw A skipped is not captured (a member with such a draw has its G-buffer draws at their place).
// Without VK_EXT_conditional_rendering, or on a tile GPU family (vkn_policy.h: Mali / PowerVR - design 7: no conditional rendering
// there), mode 7 falls back to mode 0 with a line (a CPU skip from an older verdict would not be exact).
// Threads: everything on the recording thread (the effect chain, the query module's hooks and listeners, the pass hooks). No game
// addresses.
#ifndef DM_VKN_OCCB_H
#define DM_VKN_OCCB_H
#include "vkn.h"
#include "vkn_draw.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_OCCB_BUCKETS = 8, VKN_OCCB_LEARN = 32, VKN_OCCB_PERIOD = 12, VKN_OCCB_CHECKS = 64, VKN_OCCB_CAPS = 1024, VKN_OCCB_GROUPS = 64 };

// ---- install (vkn_query_init for modes 7 / 8) ----
void vkn_occb_init(int mode, int budget);   // budget < 0 = the default (VKN_OCCB_LEARN)
int vkn_occb_mode(void);                    // 0 / 7 / 8
int vkn_occb_budget(void);
// The fallback decision (vkn_query.c setup, once the device is known): mode 7 needs the extension and a non-tile family.
// Returns the mode to run (0 = fall back; the line is printed here).
int vkn_occb_gate(int mode, int gpu_pred);

// ---- the query module's side (vkn_query.c; recording thread) ----
void vkn_occb_q_rec_begin(int presented, int slot);       // a recording begins (slot: the query slot it took, -1 = none)
void vkn_occb_q_rec_close(void);                          // it closes
void vkn_occb_q_pair_begin(uint32_t rec);                 // a pair's begin report
void vkn_occb_q_slot_taken(int slot);                     // the slot starts a new use
void vkn_occb_q_copied(int slot, uint32_t q, uint32_t rec, int whole);   // query q of the slot was copied (rec: its pair, or VKN_Q_RECS = B's own)
void vkn_occb_q_result(int slot, uint32_t q, uint32_t samples);          // its result after retirement
void vkn_occb_q_enter(void);                              // an engine draw enters the draw path (a safe point)
void vkn_occb_q_draw_post(void);                          // its draw command was recorded
void vkn_occb_q_dispatch(void);                           // a dispatch is filled (a safe point)
void vkn_occb_q_draw_pre_test(void);                      // an engine draw's command is about to be recorded (the test hook below)
void vkn_occb_pass_closing(VkCommandBuffer cb);           // the render pass instance is about to end (after the open pair's query ended)
int vkn_occb_replaying(void);                             // 1 = B is drawing its captures now (the query hooks leave the draws alone)
int vkn_occb_q_active(void);                              // 1 = one of B's own queries is open now (the pass end must not copy it)
void vkn_occb_census(void);                               // the "[vkn] occ-b" line

// ---- host tests ----
// Called right before every draw command of an engine draw (from vkn_query_draw_pre, `in` = the draw's input as the chain saw it)
// and before every replayed capture: lets the mock's sample count follow the draw (mk_draw_samples). NULL = none.
void vkn_occb_test_samples(void (*fn)(const vkn_draw_in *in, uint32_t first, int replay));
void vkn_occb_test_reset(void);             // forget objects, groups, counters

typedef struct vkn_occb_stats {
    uint64_t frames, pairs, keyed, members, learners, checks, cap_pre, cap_cond, uncond_cond, flushes, replays, replay_drop, replay_stale;
    uint64_t groups, q_group, q_learn, q_member, q_cver, conds, cond_draws, cond_nopred;
    uint64_t read_groups, culled_groups, flips, read_learn, read_learn_hidden, joined, left, member_q_read, cver_read;
    uint64_t violations, verify_judged, verify_agree;
    uint64_t refuse[24], flush_why[12], hist[6], postponed, lost, lost_replays, clear_after, cpu_ns;
} vkn_occb_stats;
void vkn_occb_stats_get(vkn_occb_stats *s);
// Refusal reasons of a member draw (not captured: drawn at its place; vkn_occb_stats.refuse[]) and flush reasons (flush_why[]).
enum { VKN_OB_R_OK = 0, VKN_OB_R_ORDER, VKN_OB_R_BLEND, VKN_OB_R_REWRITE, VKN_OB_R_BREAKS, VKN_OB_R_UBO, VKN_OB_R_BUF, VKN_OB_R_FULL, VKN_OB_R_AREA,
       VKN_OB_R_NOPASS, VKN_OB_R_BAD, VKN_OB_R_OPEN, VKN_OB_R_NOWORD, VKN_OB_R_NOTEQ, VKN_OB_R_SPLIT, VKN_OB_R_TARGET, VKN_OB_R_N };
enum { VKN_OB_F_CLOSE = 0, VKN_OB_F_ORDER, VKN_OB_F_BREAK, VKN_OB_F_SET, VKN_OB_F_KIND, VKN_OB_F_FULL, VKN_OB_F_REC, VKN_OB_F_LOST, VKN_OB_F_N };

#ifdef __cplusplus
}
#endif
#endif
