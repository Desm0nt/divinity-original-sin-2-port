// [DOS2] VK-NATIVE stage 5.1 A3, the static shadow cache (vkn_shc.c; plan docs/dos2/nr/VK-STAGE5-PLAN.md, rows "Кэш теней" and "Без
// дальнего каскада"): the static casters of a sun-shadow cascade are drawn once into a layer image; later frames copy the layer's tile
// into the atlas and skip those draws before they are recorded (vkn_fx.h skip: no Vulkan, no recording work on the engine thread).
// The decisions are the old path's pure model dm/gfx/nr/nr_shc.h (hash multisets, cascade slots, BUILD / HIT / RECOVER, probing of a
// moving camera, nrshc_shape_why), used as it is; the far-cascade skip is dm/gfx/nr/nr_hacks.h nrh_far. No game addresses.
//
// The scheme (the old path replayed a recorded IR range; the backend has no recording to replay, so it keeps copies of draws instead):
//   cascade      a run of draws into the atlas (depth-only target: no colour target bound) with one viewport rectangle = its tile;
//   BUILD        every draw goes into the atlas as the engine asks (engine order: the frame is the truth whatever happens next); the
//                draws' inputs are kept (vkn_draw_in, texture / sampler ids, the announced constant blocks of the ring); when the
//                cascade ends, the members are drawn again into the layer image S (its tile cleared to the frame's clear value first);
//   HIT          the tile of S is copied into the atlas tile - for every slot that will hit at once at the frame's first cascade
//                ("precopy": one pass break a frame; a precopied tile no hit takes is put back to the clear value), else at the
//                cascade's first draw; a member is skipped (out->skip), every other draw is drawn; when the cascade ends and a member did
//                not come or a draw broke the order rule (RECOVER), the tile is cleared and every draw of the cascade (kept) is drawn
//                again;
//   "the cascade ends" = the next draw of the chain is not an atlas draw of the same tile: that call is the safe point where this
//                module records its own draws (vkn_fx_bypass); a clear / read / copy of the atlas or the end of the recording seen
//                first (vkn_pass_watch, a close listener) ends it too: the layer is still built at the next draw, a RECOVER that can no
//                longer come before the atlas is read is counted as lost (red line).
// Why the order does not matter: every cached draw writes depth only (no colour target, no stencil test, no depth bounds, no storage)
// with LESS / LEQUAL or no depth effect into a fixed-point D16 / X8_D24 atlas, so a sample of the tile after the cascade is
// min(clear, z of every fragment) - each fragment's z does not depend on what was drawn before (same pipeline, viewport, render area),
// and min is commutative and associative: min(min(clear, members), rest) = the engine's order. A draw that breaks this (nrs_why of
// dm/gfx/nr/nr_sort.h) taints the cascade: it is never cached, a HIT that meets one recovers.
//
// Switches (read once by vkn_shc_install):
//   DM_VK_NATIVE_SHCACHE   (replaces DM_NR_SHCACHE / DM_NR_SHC_MEMO for the backend; default 1 in mode 1)
//                          0 off; 1 on; 2 verify - every decision is made and counted and the layer is built, but every draw is drawn
//                          as the engine asks; every _VERIFY frames per cascade a would-be hit is made for real in a side image X (the
//                          layer tile + the would-be drawn casters) and compared byte for byte with the atlas tile at retire ("verify
//                          equal E differ F"); 3 poison - as 1, but a hit does not copy the layer: static shadows vanish on screen.
//   DM_VK_NATIVE_SHCACHE_STABLE / _MIN / _AGE / _PROBE / _VERIFY   the model's parameters (defaults 3 / 64 / 120 / 8, verify 30)
//   DM_VK_NATIVE_SHFAR     (replaces DM_HACK_SHFAR for the backend; default = DM_HACK_SHFAR / DM_HACK_TIER as before, i.e. 0)
//                          1 the far cascade's draws are skipped before any hashing (its tile keeps the clear: no far shadows);
//                          2 counted only.
//   DM_VK_NATIVE_SHRATE    [gpu2 P8, docs/dos2/nr/VK-MOBILE-GPU-PLAN.md section 8] the far cascade every second frame; needs SHCACHE=1:
//                          1 every frame (default on every GPU class until the phone's verify run); 2 a far cascade whose static casters
//                          hit the cache (so the light's projection did not move: a member with another hash ends in a RECOVER - every
//                          kept draw drawn again, exact) and whose frame before drew the far tile takes that tile from the image F and
//                          skips every far draw - static shadows bit for bit, a MOVING caster's far shadow one frame late (13..33 ms);
//                          frames alternate (F is saved only from a drawn tile); 3 verify: drawn every frame, the frame before's tile
//                          compared with this frame's every second frame (census "rate cmp": texels differing, largest depth step).
//                          F = one more image of the atlas's size (4.5 MB at 1536^2 D16), made at the first far save.
// Lines: "[vkn] DM_VK_NATIVE_SHCACHE=..." once; "[vkn] shcache ..." / "[vkn] shcache why ..." in the 10 s census; red lines
// "【红】[vkn] shcache: ..." (first 16 of each kind).
#ifndef DM_VKN_SHC_H
#define DM_VKN_SHC_H
#include "vkn.h"
#ifdef __cplusplus
extern "C" {
#endif

// Mode 1 install time (dm/host/hle_dos2_vkn.c, marked line): reads the switches, registers the vkn_fx function "shcache" (order 100)
// and a close listener of vkn_frame. Idempotent. Nothing is allocated before the first draw into a shadow atlas.
void vkn_shc_install(void);
int vkn_shc_active(void);          // 1 = installed with DM_VK_NATIVE_SHCACHE or DM_VK_NATIVE_SHFAR non-zero
void vkn_shc_census(void);         // the lines of the 10 s census (recording thread); the window counters restart after it

enum { VKN_SHC_RF = 11, VKN_SHC_W = 10, VKN_SHC_T = 9, VKN_SHC_NRS = 13, VKN_SHC_CAS = 4 };   // nr_shc.h / nr_sort.h reason counts
typedef struct vkn_shc_stats {
    uint64_t frames, atlas_changes, colour, ranges, refused, rf[VKN_SHC_RF];   // frames with an atlas draw; refusals by nrshc_rf_name
    uint64_t armed, blind, act[3], why[VKN_SHC_W];                             // nrshc NORMAL / HIT / BUILD at arm; nrshc_why_name reasons
    uint64_t hit_ok, recover, recover_now, recover_cleared, recover_lost, recover_bad;   // HIT ends; RECOVER: repaired now / tile cleared
                                                                               // by the engine anyway / could not be repaired / repaired with drops
    uint64_t build, build_ok, build_fail, build_lost, rest, evicted;           // BUILD ends; layers made / not made / lost at the recording's
                                                                               // end; layers of other slots overwritten in S
    uint64_t draws, skipped, saved, replayed, replay_drop, replay_stale, copies, tile_clears;   // draws hashed, skipped (mode 2: would be)
    uint64_t precopied, pre_hit, pre_undone, pre_lost;                        // tiles copied at the frame's first cascade; taken by a hit /
                                                                               // put back to the clear value / neither (red)
    uint64_t tainted, tranges, tw[VKN_SHC_T], nrs[VKN_SHC_NRS], unrep, salted, ubo_bytes, ubo_memo, ubo_unrestored;
    uint64_t cranges[VKN_SHC_CAS], cdraws[VKN_SHC_CAS], cskip[VKN_SHC_CAS], known[VKN_SHC_CAS], newu[VKN_SHC_CAS], newo[VKN_SHC_CAS],
             unc[VKN_SHC_CAS], ncont[VKN_SHC_CAS];
    uint64_t verify_runs, verify_eq, verify_ne, verify_skip;
    uint64_t far_draws, far_skipped;
    uint64_t img_bytes, model_bytes;                                           // layer (+ verify) images, model tables + kept draws (alive)
    uint64_t far_not_sun;                                                      // [stage 5.4 G2] atlas draws the far rule left alone: not square >= 1024
    // [gpu2 P8] DM_VK_NATIVE_SHRATE: far cascades seen, reused (the frame before's tile), would be reused (mode 3), reused but recovered,
    // draws skipped by a reuse (non-members; the members are in `skipped`), tiles saved into F / lost (the atlas cleared or copied first);
    // mode 3: tiles compared, texels compared / differing, the largest difference in depth steps (1 / 65535 for D16)
    uint64_t rate_far, rate_reused, rate_would, rate_recover, rate_skipped, rate_saved, rate_lost, rate_cmp, rate_cmp_all, rate_cmp_px; uint32_t rate_cmp_max;
    uint64_t rate_drawn;                                                       // draws of a reused cascade that could not be kept: drawn over F's tile
} vkn_shc_stats;
void vkn_shc_stats_get(vkn_shc_stats *s);   // the current window (since the last census)
// [stage 5.4 G2] DM_VK_NATIVE_SHFAR in effect (0 / 1 / 2), -1 = not installed yet. The far-cascade rule is the old path's
// (xbg_nr_hacks.inc far_draw): only the sun-shadow atlas - square, >= 1024 (ShadowQuality 0 / 1 / 2 = 2048 / 1536 / 1024) - has its
// cascades as tiles; another depth-only image the cache adopted (>= 256, any shape) is left alone (counted far_not_sun).
int vkn_shc_far(void);

// [rt P3-GPU] DM_VK_NATIVE_SHC_MASK (docs/dos2/nr/VK-MOBILE-GPU-PLAN.md item P3): 0 = a caster's constant block is hashed whole (as
// before), 1 = only the 16-byte rows its shader's SPIR-V reads (the library's read mask, vkn_shlib_read_mask: every use of the block is an
// access chain (block, member 0, constant index) into its vec4 array; anything else - a dynamic index, the whole array - = the whole
// block); 2 = hashed whole, the masked hash made too and counted (bytes, blocks narrowed). Exact: a byte the shader never reads cannot
// change the depth it writes. Read at vkn_shc_install (the library builds the masks at shader load: the same switch, read there).
int vkn_shc_mask(void);
void vkn_shc_mask_set(int mode);
typedef struct vkn_shc_mask_stats { uint64_t blocks, narrowed, bytes_whole, bytes_masked; } vkn_shc_mask_stats;
void vkn_shc_mask_stats_get(vkn_shc_mask_stats *s);
struct vkn_shader;
uint64_t vkn_shc_masked_hash(const uint8_t *p, uint32_t n, const uint64_t *bits, uint32_t nwords, uint32_t *hashed);   // (the masked hash: tests)
// Host tests: the read-mask lookup (vkn_shlib_read_mask's contract); NULL = the library's.
void vkn_shc_test_mask(int (*fn)(const struct vkn_shader *s, int src, uint32_t binding, const uint64_t **bits, uint32_t *nwords));
// [rt P4] DM_VK_NATIVE_SHC_EHASH (vkn_rt.h): the engine thread's block hashes taken / compared (mode 2) / different (totals)
void vkn_shc_ehash_stats(uint64_t *used, uint64_t *verified, uint64_t *differ);
#ifdef __cplusplus
}
#endif
#endif
