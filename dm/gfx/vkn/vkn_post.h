// [DOS2] VK-NATIVE stage 5.2, piece B3: the mobile post shaders and the fused post tail in mode 1 (vkn_post.c; the bundle reader
// vkn_post_pack.cpp). Plan: docs/dos2/nr/VK-STAGE5-PLAN.md section 1 rows "Мобильный пост" / "Слитый пост", section 2 row "Слитый пост",
// stage 5.2 row "Б3", section 6 answer 7 (owner: wanted, ON by default, compared with the Switch shaders, differences fixed).
// The old path's technique, ported: dm/gfx/vulkan/engine/xbg_nb_post.inc (backend: stash, fused draw, AA step, alias, carry) with its
// plans dm/host/hle_dos2_nb_post.c + dm/gfx/nb/nb_plan.c nb_post_plan (F1 / F2) and dm/gfx/nb/nb_post_aa.h (route A); the fused
// bundles (XGF1, dm/gfx/nb/nb_fused.h) of <shaders>/native-post.xgspack (setup: tools/dos2_post_fuse.py). The old plan saw the whole
// submission (the NB IR); the backend sees one draw at a time, so the proofs are made by HISTORY (a shape is armed after 8 frames of it,
// the old nb_post_plan hist_frames rule) and checked by WATCHES (below) while armed: every refusal / violation is counted by reason.
//
// ONE switch, levels (read once; replaces DM_NB_POST + DM_NB_POST_AA + DM_NB_POST_AA_OUTLINE of the old path and the post part of
// DM_GFX_NATIVE_SHADERS, which dm_dos2_vkn_pre forces off in mode 1):
//   DM_VK_NATIVE_POST  0  the engine's chain with the Switch (translated) post shaders: the NR-04 post entries of the named-bundle
//                         library (vkn_shlib_named.cpp: FadeBlending, EoCFog, EoCOutlineCombine, LinearizeDepth, PPDownSampleLog2)
//                         are kept translated even with DM_VK_NATIVE_SH=1;
//                      1  the mobile post shaders only (those five, our GLSL, bit for bit in the interpreter 400 / 400), no fusion;
//                      2  1 + LUT o Sight (F1, exact: interpreter 60 / 60 bit for bit) and, where the tail shape allows it, LUT o Sight o
//                         FXAA (F1A route A: FXAA on the pre-LUT image inside Sight's draw, the console's FXAA pass skipped, its readers
//                         sample Sight's target; EoCFog between Sight and FXAA carries F1A's output on);
//                      3  2 + Vignette o CombineUI_NoFetch (F2, <= 1 LSB: 41 / 180 pixels differ by 1 in the RGBA8 output);
//                      4  3 + EoCOutlineCombine between Sight and FXAA carries F1A's output too (the outline is drawn over the
//                         anti-aliased image, its edge is not smoothed: the old DM_NB_POST_AA_OUTLINE=1 look).
//                     default 4 (VKN_POST_DEFAULT; the old path's default set: DM_NB_POST=1, _AA=1, _AA_OUTLINE=1, owner 19.11).
//   DM_VK_NATIVE_POST_AA     1 (default) = route A at levels >= 2; 0 = F1 only (LUT o Sight, the console's FXAA runs: frame-equal).
//   DM_VK_NATIVE_POST_LUMA   F1A's luma proxy (old DM_NB_POST_LUMA): 0 = F1A linear light (default), 1 = F1AS sqrt, 2 = F1AR.
//   DM_VK_NATIVE_POST_VERIFY 1 = every decision made and counted ("would ..."), the engine's draws drawn as recorded; 2 = poison: as
//                            the level, plus every target whose write was skipped is cleared to magenta (a stale read shows).
//
// Hot path: the chain function's first test is the class of the draw's shader, decided once per shader id (a table by id). A draw of
// class "other" costs that lookup plus, while a watch is live, one layout compare and the colour-target compares per watched image.
// Post-class draws (about ten a frame) do the work: texture lookups, ring reads, decisions.
//
// Watches (at most 4 images): a target whose write the fusion skipped (STALE: the LUT's target after F1, Vignette's after F2) or
// whose content lives in another image (ALIAS: the console FXAA's target after route A, content = Sight's / the carry's target).
// The image is put into VK_IMAGE_LAYOUT_GENERAL (vkn_pass_layout: a layout nothing else here uses for a colour target), so any use
// this module did not see (a draw of class "other", a copy, a dispatch, the UI) changes its layout: seen at the next draw (counted,
// red, the pair disarmed). Before a post-class draw samples a watched image: ALIAS -> the slot is redirected to the image holding the
// content (a twin of the reader's shader with that slot fed by the effect output); STALE -> the skipped draw is drawn now from its
// stash (same recording) or the read is a violation. A draw into a watched image: a full overwrite by a post-class draw ends the
// watch; anything else first materialises the content (replay / copy of the alias image).
// Recovery: a skipped draw whose host does not follow (another draw comes, the host refuses, the recording closes) is drawn from its
// stash at that moment (vkn_fx_bypass + vkn_draw_*; targets and announced constant blocks put back).
// Expected differences per target against DM_VK_NATIVE_POST=0 (the DM_VK_NATIVE_DUMP comparison, tools/dos2_vkn_cmp.py), from the old
// path's own verification (work/shaders-native-post-a/equivalence.txt, DOS2_PORT.md "Мобильные полноэкранные пост-шейдеры", NR-04.md):
//   level 1      every target bit for bit (interpreter 400 / 400 per shader); on a GPU <= 1 ulp where the driver contracts mul + add
//                differently (the explicit fma sit where the translation has them)
//   F1 (AA=0)    Sight's target bit for bit (60 / 60, 0 LSB after its RGBA8 store); the LUT's target is not drawn (stale until the
//                console FXAA overwrites it in the same frame: equal again at the frame's end)
//   F1A          Sight's target: FXAA decided on the pre-LUT image - edge pixels differ: mean 0.40 LSB, max 27 LSB, > 4 LSB in 2.5 % of
//                edge-image pixels (110 / 4320), edges missed 0.3 %, extra 8.4 % (luma 0; F1AS / F1AR: equivalence.txt); flat areas bit for bit.
//                The console FXAA's target is never drawn (an alias of Sight's / the carry's target: compare it with that one)
//   fog carry    the fog is applied to the anti-aliased image: a fogged / clear border along a silhouette keeps a 1 px step (NB-18b-10)
//   outline (4)  the outline is drawn over the anti-aliased image: its edge is not smoothed (owner's look check, NR-33A)
//   F2 (3)       the window image <= 1 LSB (41 / 180 pixels differ by 1); Vignette's target is not drawn (stale)
// Report items for the lead: the install should be called by hle_dos2_vkn.c (now: at the library's first bundle read, vkn_post_read);
// vkn_draw.c prepare reads g_call after the chain (vertex fold) and a replay from inside the chain overwrites it (counted, R_REPLAY_FIRST).
// No game addresses. Recording thread for everything but the install, the level reads and the bundle reader.
#ifndef DM_VKN_POST_H
#define DM_VKN_POST_H
#include <stdint.h>
#include "vkn.h"
#ifdef __cplusplus
#include <string>
#include <vector>
#include "vkn_shlib_named.h"   // vkn_shlib_reader
// The shader library's bundle reader asks this first (one marked line in vkn_shlib.cpp read_bundle; weak there): the fused keys
// "F1." / "F1A." / "F1AS." / "F1AR." / "F2." + host key are composed here from <shaders>/native-post.xgspack (old xbg_nb_post.inc
// compose: parsed, host head checked against the translated host bundle, extras put on spare slots). -1 = not such a key, 0 = no
// bundle (logged), 1 = `out` holds the composed bundle. Also installs this module (first call, mode 1).
int vkn_post_read(const std::string &key, std::vector<uint8_t> &out, bool &from_pack, vkn_shlib_reader read, const std::string &dir)
    __attribute__((weak));
extern "C" {
#endif

enum { VKN_POST_OFF = 0, VKN_POST_MOBILE = 1, VKN_POST_AA = 2, VKN_POST_VIG = 3, VKN_POST_OUTLINE = 4, VKN_POST_DEFAULT = 4 };
int vkn_post_level(void);        // DM_VK_NATIVE_POST in effect (env read once; any thread)
int vkn_post_mobile(void);       // 1 = the NR-04 post entries may be replaced (level >= 1): asked by vkn_shlib_named.cpp (weak there)
void vkn_post_install(void);     // mode 1, level >= 2: registers the chain function (order 400). Idempotent; any thread.
void vkn_post_census(void);      // one line (also printed by the module itself every 10 s)

// ---- the pure parts (host test: vkn_stage3_test.c test_post) ----
enum { VKN_PC_OTHER = 0, VKN_PC_LUT, VKN_PC_SIGHT, VKN_PC_VIG, VKN_PC_COMB, VKN_PC_FXAA, VKN_PC_COPY, VKN_PC_FOG, VKN_PC_OUTLINE, VKN_PC_N };
int vkn_post_class_key(const char *fs_key);   // class of a fragment program key (16 hex digits; nb_post_class / nb_post_aa_class)
// Pixels a full-screen draw writes in a w x h target: viewport & scissor & target, r = x0, y0, x1, y1 (empty: x1 <= x0).
void vkn_post_cover(const float vp[4], const int32_t sc[4], uint32_t w, uint32_t h, int32_t r[4]);
// The arming rule (old nb_post_plan hist_frames): a shape is armed after VKN_POST_ARM consecutive good frames; a bad frame starts over.
enum { VKN_POST_ARM = 8 };
typedef struct vkn_post_arm { uint32_t good; uint8_t armed; uint64_t last; } vkn_post_arm;
// One frame's verdict (frame number, 1 good / 0 bad). Several verdicts of one frame: the first bad one wins, a good one counts once.
// Returns the armed state after it.
int vkn_post_arm_note(vkn_post_arm *a, uint64_t frame, int good);

typedef struct vkn_post_stats {
    int level, aa, luma, verify;
    uint64_t draws_post, skipped, fused[3], would_skip, would_fuse;            // fused[F1 / F1A / F2]
    uint64_t step_skipped, step_would, carried, redirected, twins;
    uint64_t recovered, recovered_close, materialized, overwritten, late_aa, violations;
    uint64_t armed_frames[4], refused_total;                                   // armed now [F1, F2, route A, -]
    uint64_t dump_reads;                                                       // watched images copied out by DM_VK_NATIVE_DUMP (not violations)
} vkn_post_stats;
void vkn_post_stats_get(vkn_post_stats *s);
// Host tests: a stand-in for the fused bundles' door (the library load of "F1.<host>" ... and vkn_post_pack.cpp's parsed table). fn fills
// the stage (as vkn_shlib_shader) and the nb_fx metadata (dm/gfx/nb/nb_fused.h); 0 = ok. NULL = the real door.
struct xbg_vkn_stage;
typedef int (*vkn_post_load_fn)(const char *key, struct xbg_vkn_stage *out, void *nb_fx_meta);
void vkn_post_test_loader(vkn_post_load_fn fn);

#ifdef __cplusplus
}
#endif
#endif
