// [DOS2] VK-NATIVE stage 5, technique A of docs/dos2/nr/VK-OCCLUSION-ALT.md section 4: "carried verdict with a proof that nothing
// changed" (vkn_occ.c). DM_VK_NATIVE_COND=5 (A) / 6 (A in verify mode), budget DM_VK_NATIVE_COND_B (vkn_query.h has the switch).
//
// Idea (design 4.1). The GPU is deterministic: if nothing that the depth pre-pass draws into the screen footprint of object O changed
// since frame a, the "0 samples" verdict of O's visibility query in frame a holds now. The recording thread cannot know depth, but it
// knows which pre-pass draws changed and which screen rectangle they cover. Then O's pre-pass draws (they wrote nothing in frame a and
// would write nothing now) and its conditional G-buffer draws (the Switch skips them: its query of this frame says 0) are not recorded.
// Queries are made only to learn verdicts: 0..B a frame (mode 5), or on every pair every frame (mode 6, the verify mode).
//
// What is proved, exactly (the premises of design 4.5, as this implementation holds them; refusals are counted by reason):
//   The pre-pass SEQUENCE of a frame = every draw that reaches the effect chain (vkn_fx.h) while the depth target is the frame's
//   pre-pass target T, from the recording begin until the first conditional draw (the window), except draws that cannot change T
//   (no depth write, no stencil test) and are outside every pair. A draw has a KEY (pair draws: the engine object + its occurrence +
//   the draw's index in the pair; other draws: the old path's draw hash + occurrence), a CONTENT hash (everything its depth / stencil
//   output depends on: shader, vertex format, depth / raster / blend states, primitive, ranges, viewport, scissor, stencil reference,
//   alpha reference, vertex / index buffers by handle + offset + content version, the bytes of the vertex shader's constant blocks
//   that its SPIR-V reads (per-binding READ MASK of 16-byte elements built at shader load, vkn_shlib_read_mask), for a fragment shader
//   that may discard / write depth / mask (or unknown facts, or an alpha test) also its constant bytes and textures (pool id + object
//   + upload number) and samplers; what a later effect piece did to the draw (vkn_fx_out) and whether the draw was finally recorded
//   at all) and a RECT (pair draws: the engine object's world box projected by its camera, + 10 % + 8 px; other draws: unknown = the
//   whole screen). Inputs without a version (a dynamic buffer, a GPU-written texture, a piece's own shader) make the content new every
//   frame.
//   DIRT. Comparing frame t's sequence with frame t-1's: a draw that is new, gone, changed (content or rect) or out of order dirties
//   its old and new rects (cells of a 64 x 36 grid over the viewport's NDC square store the frame of their last dirt; an unknown rect
//   dirties the whole screen). Order: the draws seen in frame t are matched to t-1's positions greedily: a draw whose old position is
//   below the highest one matched so far is "moved"; every old draw passed over by a match is dirtied at once (absent so far: it may
//   have gone or moved later). Any two draws whose relative order changed have at least one dirty member, so in a cell without dirt
//   the ordered list of draws covering it (and their inputs) is the same as in t-1.
//   SCREEN. The pre-pass target, its size, the viewport, the camera's view-projection bytes, the pending depth / stencil clear of the
//   first draw on T (a pre-pass without a remembered depth clear, or with a stencil test and no stencil clear, is never carried):
//   any change dirties the whole screen. A frame whose camera changed is not hashed further (the next frame then dirties everything:
//   "previous frame not hashed").
//   SKIP of a pair draw d of object O in frame t (mode 5): O's last verdict is "0 samples" from frame a (all of O's draws then), O's
//   gather of this frame is valid, d's key was in frame t-1's sequence in order (not moved) with the same content and rect, and no cell
//   of the rect (nor the whole screen) has dirt from a frame after a - including the dirt of this frame's draws so far. By induction
//   over frames a+1..t every pixel of the rect sees the same ordered draws with the same inputs up to d as in frame a, so d fails the
//   depth / stencil test everywhere as it did (it writes nothing: skipping it leaves depth and stencil as they are). The conditional
//   draws of O's pair are skipped when every draw of the pair was skipped by this rule, the pair closed, and it has as many draws as at
//   the verdict: O's query of frame t would be 0, the Switch would skip them. An EMPTY pair (no draw between its reports) skips its
//   conditional draws exactly (as COND=1). Learning queries never go to a pair whose draws are skipped.
//   Taken on trust and checked by mode 6 only: the engine's box covers what the object's draws rasterise, and the gather camera is the
//   camera of the pre-pass draws; the content hash covers everything (as listed above; copies into T and draws dropped by the draw
//   path before the chain are not seen).
// Verify (mode 6): every pair gets a query every frame (as mode 2), nothing is skipped, A's decisions are made all the same (on the
// verdicts of those queries, 3 frames old). When a slot is read back: a pair A would have skipped (all its draws proven) must have 0
// samples in that very frame's query; otherwise a violation (counted; the first 16 printed red with the object key, rect, verdict age).
// Object bounds (piece A-1; facts with addresses in dm/host/hle_dos2_vkn_occ.c): the engine object's world AABB at obj+0x14c, read by
// the host hook of the per-object gather 0xb0c570 after it allocated the object's report pair, projected there (job threads) with the
// view-projection of the stage's culling camera; handed over through a lock-free table indexed by the pair's entry address.
// Threads: vkn_occ_gather any thread (lock-free); everything else on the recording thread (vkn_rt.h: the effect chain, the query
// module's listeners and readback all run there). No game addresses.
#ifndef DM_VKN_OCC_H
#define DM_VKN_OCC_H
#include <stddef.h>
#include "vkn.h"
#include "vkn_shader.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_OCC_GW = 64, VKN_OCC_GH = 36, VKN_OCC_PER_HALF = 2048, VKN_OCC_B_ADRENO = 64, VKN_OCC_B_TILER = 16 };

// ---- the gather hand-off (any thread; the host hook of 0xb0c570) ----
// The two halves of the engine's pair table (entry k of half h = base[h] + 16 k, 2048 a half). Read by the hook at every call.
void vkn_occ_gather_bases(uint64_t base0, uint64_t base1);
// One allocated pair: key = the entry address (the key vkn_query_begin gets), eframe = the engine frame the allocation wrote, obj =
// the engine object, aabb = {min x, y, z, max x, y, z} world box, vp = the camera's view-projection (clip[c] = sum_r p[r] * vp[4 r + c],
// p = (x, y, z, 1)), vp NULL = no camera. Returns 0 = recorded, else a reason VKN_OCC_G_* (counted).
enum { VKN_OCC_G_OK = 0, VKN_OCC_G_KEY, VKN_OCC_G_STALE, VKN_OCC_G_CONFLICT, VKN_OCC_G_NOCAM, VKN_OCC_G_BOX, VKN_OCC_G_BEHIND, VKN_OCC_G_N };
int vkn_occ_gather(uint64_t key, uint64_t eframe, uint64_t obj, const float aabb[6], const float vp[16]);
// Pure: the NDC rectangle {x0, y0, x1, y1} of a box seen through vp (-1 = a corner at or behind the camera plane w <= 1e-6, -2 = bad box).
int vkn_occ_project(const float aabb[6], const float vp[16], float ndc[4]);

// ---- the read mask of a constant block (pure; vkn_shlib.cpp calls it at load on the final words) ----
// Bits of the 16-byte elements of the Uniform block with Binding `binding` that the words can read: 1 = *bits holds them (nwords 64-bit
// words; an element >= 64 * nwords is "whole"), 0 = the whole block may be read (a dynamic index, another use of the variable, a
// layout other than one array of 16-byte stride), -1 = no such block / malformed words.
int vkn_occ_spv_read_mask(const uint32_t *w, size_t n, uint32_t binding, uint64_t *bits, uint32_t nwords);

// ---- install (vkn_query_init for modes 5 / 6) ----
void vkn_occ_init(int mode, int budget);   // budget < 0 = the family default (DM_VK_NATIVE_COND_B unset)
int vkn_occ_mode(void);                    // 0 / 5 / 6
int vkn_occ_budget(void);
// Host tests: the read-mask lookup (vkn_shlib_read_mask's contract) and the fragment facts (vkn_shlib_fs_facts) of a shader; NULL = the library's.
void vkn_occ_test_fns(int (*mask)(const vkn_shader *s, int src, uint32_t binding, const uint64_t **bits, uint32_t *nwords), int (*fs_facts)(const vkn_shader *s));
void vkn_occ_test_reset(void);             // forget everything (tables, grid, verdicts, counters); the switches stay

// ---- the query module's side (vkn_query.c, modes 5 / 6; recording thread) ----
void vkn_occ_q_rec_begin(int presented, int slot);        // a recording begins (slot: the query slot it took, -1 = none)
void vkn_occ_q_rec_close(void);                           // it closes (the window and the frame end)
int vkn_occ_q_pair_begin(uint32_t rec, uint64_t key);    // a pair's begin report: 1 = give it a query
void vkn_occ_q_pair_broken(uint32_t rec);                 // nested / cut: its verdict is not a pair's
void vkn_occ_q_draw_pre(void);                            // the draw being made is recorded (right before its vkCmdDraw*)
void vkn_occ_q_copied(int slot, uint32_t q, uint32_t rec, int whole);   // query q of the slot holds pair rec (whole: a complete verdict)
void vkn_occ_q_slot_taken(int slot);                      // the slot starts a new use
void vkn_occ_q_result(int slot, uint32_t q, uint32_t samples);   // its result after retirement
void vkn_occ_census(void);                                // the "[vkn] occ-a" line(s)

// ---- pure model (the glue above drives it; the host test drives it directly) ----
// A rect in grid cells (inclusive): kind 0 = unknown (the whole screen), 1 = cells, 2 = nothing (off screen).
typedef struct vkn_occ_rect { uint8_t kind, x0, y0, x1, y1; } vkn_occ_rect;
vkn_occ_rect vkn_occ_cells(const float ndc[4], float vw, float vh);   // + 10 % of the size + 8 px each side, clamped
void vkn_occm_frame_begin(void);                          // a new model frame (presented recording)
uint32_t vkn_occm_frame(void);                            // its number (from 1)
void vkn_occm_whole(int reason);                          // whole-screen dirt now (VKN_OCC_W_*)
// A draw of the sequence: the match of its key (skip-over dirt applied now); returns its old content / rect through *e (NULL = new)
// and *moved. vkn_occm_fin then appends it with its final content and rect (dirt for new / changed / moved).
typedef struct vkn_occm_old { uint64_t content; vkn_occ_rect r; } vkn_occm_old;
int vkn_occm_pre(uint64_t key, vkn_occm_old *e, int *moved);   // 1 = found
void vkn_occm_fin(uint64_t key, uint64_t content, vkn_occ_rect r);
uint32_t vkn_occm_maxdirt(vkn_occ_rect r);                // the newest dirt frame over the rect (whole screen included)
void vkn_occm_frame_end(int hashed);                      // the gone draws dirtied; this frame becomes the one compared with
enum { VKN_OCC_W_CAMERA = 0, VKN_OCC_W_VIEWPORT, VKN_OCC_W_TARGET, VKN_OCC_W_CLEAR, VKN_OCC_W_NOCLEAR, VKN_OCC_W_STENCIL, VKN_OCC_W_UNHASHED,
       VKN_OCC_W_UNKNOWN_RECT, VKN_OCC_W_OVERFLOW, VKN_OCC_W_PIECE, VKN_OCC_W_LATE_T, VKN_OCC_W_N };

typedef struct vkn_occ_stats {
    uint64_t frames, hashed_frames, tracked, pair_draws, skip_pre, skip_cond, skip_empty, skip_idx, would_pre, would_cond;
    uint64_t learn, queries_v, verdicts, verdict_hidden, dirty_cells, whole[VKN_OCC_W_N], refuse[24];
    uint64_t verify_pairs, verify_agree, violations, verify_partial;
    uint64_t gather[VKN_OCC_G_N], pairs, pairs_gathered, ubo_bytes, ubo_memo, unversioned, cpu_ns;
} vkn_occ_stats;
void vkn_occ_stats_get(vkn_occ_stats *s);
typedef struct vkn_occ_census_v { uint32_t objects, hidden, visible, none, valid_hidden; } vkn_occ_census_v;
void vkn_occ_objects(vkn_occ_census_v *v);               // the object table now

#ifdef __cplusplus
}
#endif
#endif
