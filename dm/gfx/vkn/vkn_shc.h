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
//                again; [shcfix 22.11] every replay of a kept draw binds the vertex / index blocks the engine's draw read (vkn_draw.h
//                vkn_draw_bufs), not the handles' blocks of now: the engine rewrites some buffers after their shadow draw in the frame
//                (phone: "reads a vertex / index buffer written since it was drawn (not replayed)" - a repaired tile without them);
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
//                          equal E differ F"; [shcfix 22.11] a would-be hit that ends in a RECOVER: the repair mode 1 makes - X's
//                          tile cleared, every kept draw drawn again - compared the same way, "of them repairs R"); 3 poison - as 1, but a hit does not copy the layer: static shadows vanish on screen.
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
    // [shcfix 22.11] kept draws whose vertex / index buffer the engine wrote after the draw (before the cache drew it again): drawn from
    // the blocks the engine's draw read (vkn_draw.h vkn_draw_bufs) - in total, and the replays that had one: repairs (RECOVER), layers
    // (BUILD), verifies (mode 2), strips (DM_VK_NATIVE_SHSCROLL); verify_rec: mode 2 would-be RECOVERs verified (the tile cleared, every
    // kept draw drawn again into the side image, compared with the engine's tile: the exactness of the repair mode 1 makes)
    uint64_t replay_moved, moved_recover, moved_build, moved_verify, moved_strips, verify_rec;
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
// ==== [VK-SHADOWS-MOVING] (docs/dos2/nr/VK-SHADOWS-MOVING.md; switches and bounds: vkn_shs.h; rules and proofs: dm/gfx/nr/nr_shs.h) ====
// DM_VK_NATIVE_SHCULL: a caster whose CPU bounds do not touch its viewport and scissor is skipped before it is hashed or recorded.
// DM_VK_NATIVE_SHSCROLL: the layer follows a cascade whose projection only translates by whole texels.
//   T caster     a draw with bounds whose fragment stage provably does nothing (the verdict of MAT, vkn_mat_peek), whose scissor covers
//                the tile and whose per-view block is the one of the range. The model (nr_shc.h) gets hash(structure, linear part of
//                its clip transform, instance id) instead of the hash of its bytes; the instance = its clip translation relative to the
//                reference translation of the cascade (the ViewProjection translation words of the per-view block), found again within
//                1 / 64 px (nr_shs.h registry). While the cascade stands (reference bit-equal to that of the previous range) an
//                instance gets another id when a constant its position reads changed (vkn_shs_b.pih): standing, a member has bit-equal
//                position inputs, the same mesh bytes and no acting fragment stage - its depth is bit for bit the same (exact; and
//                a caster whose OTHER constants change every frame, e.g. the time in a block its position does not read, stays one).
//   anchor       at the BUILD of a layer: the reference rows, offset 0. A later HIT with the same rows = the cache as before (copy / precopy).
//   scrolled hit the linear part bit-equal, the translation moved by whole texels (within the tolerance) from the anchor, the shift of
//                this frame under half a tile, every member a T caster, D16 atlas cleared to 1.0. Then: the layer piece draws the kept
//                part of the tile from the layer (toroidal offset, + the z translation since the anchor) inside the pass of the engine; a
//                member whose bounds do not touch the exposed strips is skipped; one that does is drawn as the engine asks and kept;
//                a member that does not come must have its bounds outside the tile (it leaves the member set), else RECOVER. At the
//                first draw of the frame outside the atlas the exposed strips of every scrolled layer are cleared and drawn by the kept
//                touchers in ONE pass of the layer image (viewport moved to the place of the strip in the layer, scissor = the strip,
//                the per-view block with the z-translation words of the anchor: the layer stays in the z frame of its build - checked
//                by numbers for every toucher).
//   refresh      casters that came into the cascade since the build are drawn every frame; when the stable ones reach a quarter of the
//                members the layer is dropped and built again (4 frames of full draws).
//   verify       DM_VK_NATIVE_SHSCROLL=2: as mode 2 of the cache; for a scrolled range the side image gets the cleared tile, the layer
//                piece and the drawn casters, compared with the tile the engine drew: "scroll verify ... texels differing (max steps)".
// Not bit-exact by construction (the vertex shader rounds a translated position differently in the last float bit, the z of the layer is
// re-added in one float op): the verify line measures it. Lines: "[vkn] shcache moving ..." / "... moving why ..." in the census.
// [fix 22.11] (phone M09: every verified scrolled tile differed, 12-31 % of the texels, up to 7673 steps; host test MV_HARD) two causes:
//   z            the layer's depths are D16 steps already; a z shift that is not a whole step rounds a second time (about 31 % of the
//                covered texels one step off). Now a layer whose cascade's z words are not those of its build is dropped ("z moved"),
//                and none is built while they changed within the last `stable` ranges ("builds held"). DM_VK_NATIVE_SHSCROLL_Z=1: the
//                old path, measured (vkn_shs.h).
//   identity     a caster whose own position constants changed (World) by less than the instance tolerance kept its id while the
//                cascade moved (the standing rule above only acts while it stands): its old shadow stayed in the layer (large steps at
//                its edges). Now an instance gets a new id whenever the words of its own blocks (vkn_shs_b.pio: all but the per-view
//                block) differ from its last draw, standing or moving: a member has bit-equal own inputs in every frame.
//   The verify line counts by case (x / y only, z) and by kind (kept part 1 step / more, inside the exposed strips).
typedef struct vkn_shc_scroll_stats {
    uint64_t plain_ranges, scroll_ranges, moved_ranges, dz_ranges;       // hits as before; scrolled hits (with a shift / with a z move)
    uint64_t drop_ref, drop_lin, drop_shift, drop_big, drop_fmem, drop_fmt, job_late;   // layers dropped at a hit, by reason
    uint64_t t_draws, t_fs, t_full, epoch;                                // T casters; refused: fragment stage / registry full; ids renewed
    uint64_t skipped, touched, absent_out, absent_in, zguard, f_member;  // members of scrolled hits: skipped / drawn into strips / ...
    uint64_t builds, build_t, build_f, refresh;
    uint64_t jobs, job_pieces, job_draws, job_fail, job_lost, job_full, patch_bad;
    uint64_t quad, quad_fail;                                             // the layer piece drawn into a tile
    uint64_t cull_seen, culled, cull_would;
    uint64_t sv_cmp, sv_eq, sv_all, sv_px; uint32_t sv_max, n_max; double err_max;
    // [fix 22.11] the verify by case (0: the layer moved only in x / y since its build; 1: in z too - DM_VK_NATIVE_SHSCROLL_Z=1 only):
    // tiles compared / equal / texels, texels differing in the kept part by one D16 step / by more, texels differing inside this
    // frame's exposed strips (there the side image has only the drawn casters: a difference = a member's bounds missed a strip)
    uint64_t svc_cmp[2], svc_eq[2], svc_all[2], svc_k1[2], svc_kn[2], svc_st[2]; uint32_t svc_max[2];
    uint64_t drop_z, build_held, epoch_obj, zcmp, zstep;   // layers dropped: z moved; builds held (z moving); ids renewed: the object's
                                                           // own position words changed; ranges compared with the slot's previous / z moved
} vkn_shc_scroll_stats;
void vkn_shc_scroll_stats_get(vkn_shc_scroll_stats *s);   // the current window
struct vkn_shader;
void vkn_shc_test_nofs(int (*fn)(const struct vkn_shader *s));   // host tests: "the fragment stage does nothing" (NULL = vkn_mat_peek)
// [rt P4] DM_VK_NATIVE_SHC_EHASH (vkn_rt.h): the engine thread's block hashes taken / compared (mode 2) / different (totals)
void vkn_shc_ehash_stats(uint64_t *used, uint64_t *verified, uint64_t *differ);
// ==== [eng I1b] (docs/dos2/nr/VK-ENGINE-SIDE.md sections 4 I1b and 11) the far cascade every N-th frame, left out BY THE ENGINE ====
// DM_VK_ENG_FARRATE  0 off (default: it changes the picture - a moving caster's far shadow is up to N-1 frames late - until the owner
//                    has seen the clips); 1 on; 2 count (the engine decides and says so, nothing is left out); 3 poison (left out, the
//                    tile NOT put back: the far shadows blink - proof that the path is live). DM_VK_ENG_FARRATE_N = N (2..4, default 2).
//                    Replaces DM_VK_NATIVE_SHRATE (set to 1 while this is on); off with DM_VK_NATIVE_SHFAR=1.
// The host side (dm/host/hle_dos2_vkn_eng.c) decides per shadow build: the cull receiver of the far cascade puts no object into its
// list (no draw is made by the engine, none reaches the recording thread), and the far cascade's matrices of the last drawn build
// are put back into the engine's shadow data, so the resolve of the frame reads the far tile with the projection it was drawn with.
// Its shadow-stage execute (the encoder thread, in the order of the frame's records) hands the decision to vkn_shc_eng_far: 0 drawn,
// 1 left out, 2 would be left out. Here: at the safe point after the frame's atlas draws (the first draw outside the atlas) a drawn far
// tile (the tile nrh_far names) is copied into F; in a left-out frame F's tile goes back into the cleared atlas - if F holds this
// atlas's tile with the same clear value, drawn at most N-1 decisions ago. A far draw in a left-out frame, a tile that cannot be put
// back, a safe point that never comes (the atlas sampled / cleared / copied first, the recording's end) count as bad (red line); the
// host side stops leaving out after 3 for the session (auto-fallback). The resolve of a left-out frame samples F's tile at the old
// projection: static far shadows are where they were (bit for bit, the same tile and matrix), only casters that moved since lag.
// Line "[vkn] shcache eng far ..." in the 10 s census.
typedef struct vkn_shc_eng_stats {
    uint64_t dec, drawn, drawn_draws, empty, skipped, restored, poisoned, empty_kept, missing, would, would_draws, saved, unsaved, lost, bad, reds;
} vkn_shc_eng_stats;
void vkn_shc_eng_far(int dec);                 // recording thread
int vkn_shc_eng_far_bad(void);                 // any thread: bad decisions so far (the host side's fallback)
int vkn_shc_eng_far_tile(void);                // any thread: the far tile's width in texels (0 = none seen yet)
int vkn_shc_eng_far_mode(void);
void vkn_shc_eng_stats_get(vkn_shc_eng_stats *s);   // the current window
void vkn_shc_eng_test(int mode, int period);   // host tests
// ==== [eng I1a] (docs/dos2/nr/VK-ENGINE-SIDE.md section 13) the persistent static-caster layer, driven BY THE ENGINE ====
// DM_VK_ENG_SHSTATIC  0 off (default); 1 on; 2 verify (the engine draws everything; every _VERIFY-th would-be reuse of a cascade the
//                     tile is read back after the dynamic casters (V1), after the members that touch no strip (V2) and at the end, with
//                     the layer: min(tile after V1, layer shifted) against the tile drawn is compared texel by texel at retire, and the
//                     strips must not get a fragment from the members the engine would leave out); 3 poison (as 1, but every
//                     _POISON-th reuse leaves the cleared tile: the static shadows blink - the layer is what stands there).
//                     _FROM = the first cascade (default 1: the near cascade has ~10 casters and its list holds the atlas clear).
// The engine side (dm/host/hle_dos2_vkn_eng.c with dm/gfx/nr/nr_shst.h, which has the rules) takes over the draw build of a cascade:
// it decides per frame and gives the draw build its casters in groups, with markers in the cascade's command list (an engine callback
// command; they arrive here in the order of the draws, vkn_shc_eng_st on the recording thread):
//   START(act)   OFF / HOLD: everything is drawn as the engine asks (the cache of this file leaves the cascade alone: owned);
//                BUILD: the static members come first; REUSE: the layer goes into the cleared tile now (shifted by nx, ny texels: tile
//                pixel p <- layer pixel p - n), the members are not drawn at all but for the strip touchers; VERIFY (mode 2).
//   OBJ(slot, tag)  the draws until the next marker are one caster's: classified (order-free depth-only into the tile, affine position
//                with bounds, no acting fragment stage, no storage) and signed (mesh content versions, shader / states / arguments, its
//                own position words); the verdict (per slot + tag; ONCE after one good observation, OK after two equal ones in two
//                frames, NOT, UNKNOWN) is published for the engine side (vkn_shc_eng_st_verdict).
//   NONE         no caster is observed until the next marker.   MID   BUILD: the tile (clear + the members) is copied into the layer
//                image E; the ack (epoch, good: every member's draws were OK / bad) is published (vkn_shc_eng_st_ack).
//   V1 / V2      mode 2 read-backs.   END   the cascade's casters are done.
// A REUSE that cannot be honoured (no layer of that epoch, another atlas / tile / clear value, the copy cannot be recorded, the first
// draw's per-view block not the layer's shifted by (nx, ny)) is BAD (red line): the frame lacks the static shadows; the engine side stops
// for the session after 3. Line "[vkn] shcache eng static ..." in the census.
enum { VKN_SHST_START = 1, VKN_SHST_OBJ, VKN_SHST_NONE, VKN_SHST_MID, VKN_SHST_V1, VKN_SHST_V2, VKN_SHST_END, VKN_SHST_SKIP0, VKN_SHST_SKIP1 };
// [fix2 22.11] (phone G06: per-view mismatches; the layer's camera was taken from the last announcement) DM_VK_ENG_SHSTATIC_SAFE (default 1):
// the members of a REUSE are still given to the draw build, between SKIP0 / SKIP1 (first in the cascade); the layer goes into the tile at
// the cascade's first draw, after that draw's per-view block matched the layer's camera shifted by (nx, ny) - then the members are skipped
// here, else they are drawn (a fallback: the tile is exact). 0: the members are not given at all; a layer is reused only after a later
// frame of its epoch (HOLD / VERIFY) confirmed its camera. Acks: 1 confirmed, 2 bad, 3 built (camera not yet confirmed). The layer's
// camera = the per-view block a draw of the cascade read (never "the last announcement"); a mismatch dumps both blocks (first 8).
typedef struct vkn_shst_mark {
    uint32_t magic, kind, k, act;      // magic VKN_SHST_MAGIC; act: nr_shst.h NRST_A_* (START)
    uint32_t epoch, slot, tag, pad;    // START: the layer's epoch; OBJ: the caster's slot and tag
    int32_t nx, ny, rect[4];           // START: the shift (REUSE / VERIFY), the tile (atlas pixels)
    uint32_t n[6];                     // START: casters by group (nr_shst.h NRST_G_*; [5] the members), census only
} vkn_shst_mark;
#define VKN_SHST_MAGIC 0x54534853u
void vkn_shc_eng_st(const vkn_shst_mark *m);          // recording thread
int vkn_shc_eng_st_mode(void);                         // DM_VK_ENG_SHSTATIC as the backend took it (0 when not installed)
int vkn_shc_eng_st_from(void);
int vkn_shc_eng_st_safe(void);                         // [fix2] DM_VK_ENG_SHSTATIC_SAFE
int vkn_shc_eng_st_verdict(int k, uint32_t slot, uint32_t tag);   // any thread: nr_shst.h NRST_V_*
int vkn_shc_eng_st_ack(int k, uint32_t *epoch);       // any thread: 0 none yet, 1 good (confirmed), 2 bad, 3 built (for *epoch)
int vkn_shc_eng_st_bad(void);                          // any thread: reuses that could not be honoured (session)
typedef struct vkn_shc_st_stats {
    uint64_t starts[VKN_SHC_CAS][5], draws[VKN_SHC_CAS][5], built[VKN_SHC_CAS], built_bad[VKN_SHC_CAS], layer_draws[VKN_SHC_CAS];
    uint64_t reused, copies, poisoned, bad, lost, xchk, xchk_bad, stray, unterminated;
    uint64_t fallback, wbad, skipped_members, drawn_members;   // [fix2]
    uint64_t groups, v[4], v_empty, not_why[12];
    uint64_t vcmp, veq, vall, vk1, vkn, vstrip, vskip; uint32_t vmax;
} vkn_shc_st_stats;
void vkn_shc_eng_st_stats_get(vkn_shc_st_stats *s);   // the current window
#ifdef __cplusplus
}
#endif
#endif
