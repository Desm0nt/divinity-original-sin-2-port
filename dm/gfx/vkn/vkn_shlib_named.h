// [DOS2] VK-NATIVE stage 5.1, piece A1: engine shaders BY NAME and their Dref shadow resolve inside the backend's shader library
// (vkn_shlib_named.cpp). The old path's technique NR-04 (translator: dm/gfx/vulkan/engine/xbg_nr_sh.inc + xbg_nr_sh_pass.inc; table and
// checks: dm/gfx/nr/nr_sh.{h,c}; pack builder tools/dos2_nr_sh_pack.py) ported to mode 1, where the translator is not initialised.
//
// Read side (one marked call at the head of vkn_shlib.cpp read_bundle): a bundle whose key is a fragment key of the NR-04 table is read
// as usual (pack / file / DM_GFX_NATIVE_SHADERS), then the named bundle "<variant>.<engine name>" of <shaders>/native-nr.xgspack replaces
// it when its head equals the translated head byte for byte (except the SPIR-V length word), the entry point's input / output locations
// are the translated ones and every descriptor it declares is in the binding table - exactly xbg_nr_sh.inc apply. The library then runs
// its whole load-time chain on the named words under the translated key (host test vkn_shlib_test.cpp, models "+nr...": the module words
// equal the translator's with DM_NR_SH=1). Every later read of the key (alpha variants) sees the same bytes.
// Pass side (vkn_shader.c vkn_shader_module, at pipeline creation on the recording thread): the pass the pipeline is built for (current
// target set: colour count, NVN format of colour 0, samples) is held against the pass the entry was verified in (nr_sh_pass_check);
// outside it the pipeline takes the translated module (vkn_shlib_named_alt: the bundle read past this module under the key
// "<key>#nrt"), except a Dref bundle (its binding has a comparison sampler for every pipeline: the named module stays, as on the old path).
// The pipeline key holds the pass's compatibility class, so the decision is made once per (shader, pass) - nothing per draw.
// Dref (DM_VK_NATIVE_SH_PCF=3): the 'd' shadow bundles sample the atlas with OpImageSampleDrefExplicitLod; vkn_shader.c sets VKN_BF_CMP
// (vkn_fx.h) on exactly the texture bindings nr_sh_depth_bindings names, and the draw path binds the slot sampler's comparison twin
// (vkn_sampler_cmp_by_id, vkn_tex.c: compare LESS, linear min / mag, the game's mip mode - xbg_game.cpp dref_sampler).
//
// Switches (read once, at the library's first bundle read; the old names are forced to 0 in mode 1 by dm_dos2_vkn_pre):
//   DM_VK_NATIVE_SH      (replaces DM_NR_SH)     0 off; 1 replace (default: xbg_nr_sh_default, every family); 2 verify: every check of 1
//                                                (pack, head, interface, descriptors, the pass of every pipeline), the translated used
//   DM_VK_NATIVE_SH_PCF  (replaces DM_NR_SH_PCF) the shadow class: 0 translated, 1 exact, 2 mobile (textureGather), 3 dref (default:
//                        xbg_nr_sh_pcf_default); unset: DM_HACK_SHPCF / DM_HACK_TIER (nr_hacks.h) wins over the default, as before.
//                        3 needs a shadow-atlas format (NVN DEPTH16, stored by vkn_tex.c as D16_UNORM) the device samples with linear
//                        filtering and depth comparison; else 2 (counted, one line). A pack without the 'd' bundle: the 'm' one.
// Not here (other pieces / plan rows): NR-ONCHIP, NR-33C surfaces. [stage 5.4 G1] DM_HACK_FSR / DM_HACK_LIGHTS: the end block.
// Census: vkn_shlib_named_census (called from vkn_shader_census): one line with the counters; red lines (first 16 events).
// No game addresses. Thread safety: atomics for the per-entry state and counters, one mutex for the pass list.
#ifndef DM_VKN_SHLIB_NAMED_H
#define DM_VKN_SHLIB_NAMED_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "../vulkan/engine/xbg_game.h"   // xbg_vkn_stage
#ifdef __cplusplus
#include <string>
#include <vector>
// The library's bundle reader (vkn_shlib.cpp read_bundle: pack first, else the file, then the DM_GFX_NATIVE_SHADERS swap).
typedef bool (*vkn_shlib_reader)(const std::string &key, std::vector<uint8_t> &out, bool &from_pack);
// Called at the head of read_bundle with the library's facts (shader directory, physical device, format_feature_flags2, policy family).
// -1 = not this module's key: the caller reads as usual; 0 = handled, no bundle; 1 = handled, `out` holds the bundle to use (the named
// one, or the translated one read through `read`; empty = a bad length, as read_bundle itself).
int vkn_shlib_named_read(const std::string &key, std::vector<uint8_t> &out, bool &from_pack, vkn_shlib_reader read, const std::string &dir,
                         VkPhysicalDevice phys, bool ffl2, int policy);
extern "C" {
#endif

// The NR-04 table entry (nr_sh_table index) of a fragment key the library replaced (DM_VK_NATIVE_SH=1) or checked successfully (=2),
// else -1. *dref_mask (may be NULL): descriptor-set-0 bindings (< 64) the replaced module samples with a depth comparison (0 in mode 2).
int vkn_shlib_named_entry(const char *fs_key, uint64_t *dref_mask);
// The pass of a pipeline about to be built with entry's fragment module: active colour targets in slot order (NVN formats), depth NVN
// format (0 none), samples, size (logs). ncolor < 0 = not known (built off the recording thread: warm-up), counted. Every distinct
// (entry, pass) is logged once. 1 = take the translated module (vkn_shlib_named_alt) for this pipeline, 0 = keep the module.
int vkn_shlib_named_pass(int entry, int ncolor, const uint32_t *fmt, uint32_t depth_fmt, uint32_t samples, uint32_t w, uint32_t h);
// The translated bundle of a replaced fragment key, loaded by the library (all its rewrites) under "<key>#nrt". The worker thread of
// vkn_shader.c (8 MB stack). vkn_shlib_shader's return codes.
int vkn_shlib_named_alt(const char *fs_key, xbg_vkn_stage *out);
// vkn_shader.c: texture bindings flagged VKN_BF_CMP / translated modules made or not for a pass outside the verified one.
void vkn_shlib_named_flagged(uint32_t bindings);
void vkn_shlib_named_alt_result(int ok);

typedef struct vkn_shlib_named_stats {
    int mode, pcf;                                 // in effect (-1 = not read yet)
    uint32_t pack_bundles; int pack_open;
    uint32_t replaced;                             // entries replaced (mode 1) / passing every check (mode 2)
    uint64_t swapped, would, kept, refused_head, refused_io, refused_desc, missing, off_name, dref_to_mobile, dref_missing;
    uint64_t dref_shaders, dref_flagged;           // shaders with a Dref module, texture bindings flagged VKN_BF_CMP
    uint64_t pass_verified, pass_outside, pass_unchecked, pass_dref_kept, alt_made, alt_failed;
    uint32_t passes;                               // distinct (entry, pass) seen
    uint64_t reds;
} vkn_shlib_named_stats;
void vkn_shlib_named_stats_get(vkn_shlib_named_stats *s);
void vkn_shlib_named_census(void);

// ==== [stage 5.1 A4] LIGHTFUSE (dm/gfx/vkn/vkn_lightfuse.c): the fused DeferredSky -> ClusteredLightingWithSSAO bundle by name ====
// The pack's "<variant>.ClusteredLightingWithSSAO" ('f': DeferredSky inside the Light shader; 'g': + DeferredFog on output 1) for
// the Light shader's fragment key, under the checks of the old path (xbg_nr_sh.inc oc_apply): head equal to the translated head byte
// for byte (except the SPIR-V length word), input locations equal, output locations equal ('g': the translated ones | location 1),
// every descriptor in the head table or one of nr_lightfuse.h's extras and each extra declared (nr_lf_bindings_ok). The module is
// loaded through the library's whole load-time chain under the key "<key>#nlf" / "<key>#nlg" (as the translator's bundle_words +
// bundle_module did for "#lf" / "#lfg"); out as vkn_shlib_shader (bindings = the head table, uniform extents trimmed by the fused
// words). declared[4]: bit b = the fused SPIR-V declares descriptor set 0 binding b (< 256). *dref: nr_sh_depth_bindings of the fused
// words (bindings < 64 sampled with a depth comparison). Independent of DM_VK_NATIVE_SH. The library's rewrites want a big stack:
// call it on a thread with >= 8 MB. 0 = ok; 1 = no such bundle / refused (one line, red); -1 = the library is not up.
int vkn_shlib_named_fused(const char *light_fs_key, char variant, xbg_vkn_stage *out, uint64_t declared[4], uint64_t *dref);
// 1 = the translated bundles of the two keys (the library's packs / files, as read_translated) are equal byte for byte, 0 = they
// differ or one is missing, -1 = the library has read no bundle yet. File reads: once per pair (the caller keeps the answer).
int vkn_shlib_named_same_bundle(const char *key_a, const char *key_b);

// ==== [stage 5.4 G1] DM_HACK_FSR / DM_HACK_LIGHTS (nr_hacks.h; the old path: xbg_nr_sh.inc hack_apply / hacks) ====
// The module swaps of the old path, ported as they are: with DM_VK_NATIVE_SH=1 (as the old path wanted DM_NR_SH=1) and before the pack
// swap of the same entry, the bundle of PPFXAA (DM_HACK_FSR 1 / 3; 2 = checked only) or of ClusteredLightingWithSSAO (DM_HACK_LIGHTS
// 2 / 4) becomes the translated head + a module built into the backend (dm/android/shaders/ppfxaa_fsr*.frag.spv.inc,
// light_lcap*.frag.spv.inc), under the same checks (head, interface, descriptors); a swapped entry is "replaced" (pass check, "#nrt" twin).
// Values: nr_hacks.h nrh_value (explicit DM_HACK_<name> over DM_HACK_TIER; the tier's FSR obeys xbg_hack_fsr_tier_default), read once
// at the library's first bundle read. Host test: vkn_shlib_test "+nr<q>-fsr<N>" / "-lc<N>" (the module words = the translator's).
// How they compose with the other mode-1 techniques:
//   FSR and the fused post (vkn_post.c): the PPFXAA draw is the AA step. Route A (F1A: FXAA inside Sight's draw, the AA step skipped) is
//     taken only when the LUT and Sight draws cover their whole targets, i.e. at 100 % (vkn_post.c R_PART "a part of the target is drawn
//     (dynamic resolution)"); below 100 % the AA step runs - and that is the stretch where EASU works. At 100 % the FSR module is the
//     FXAA path bit for bit, so skipping it changes nothing. The old path composed the same way (xbg_nb_post.inc NB-18b-9).
//   FSR and DM_VK_NATIVE_POST=0: the hack wins (the player's choice; the old path had no such level).
//   The light cap and LIGHTFUSE (vkn_lightfuse.c): the fused bundle "f." / "g.ClusteredLightingWithSSAO" of the pack replaces the Light
//     shader's module and has no cap (on the old path the cap was lost the same way while DM_NR_LIGHTFUSE was on). DM_VK_NATIVE_LCAP
//     (vkn_hacks.h) picks: 1 = the cap wins - LIGHTFUSE is not installed while the cap is in use (one line); 0 = no cap (red line);
//     2 = both: "#nlf" / "#nlg" (vkn_shlib_named_fused) hand out the translated Light head + a built-in capped fused module
//     (light_lcap<N>_<f|g>.frag.spv.inc, tools/dos2_light_cap_fused.py: = Sky -> q11 -> the capped Light -> blend, bit for bit) instead of
//     the pack's bundle, under the same checks - only when the player's DeferredSky c1 equals the bits they were compiled with (checked once,
//     one line; else red, no fusion: the cap module draws and LIGHTFUSE refuses its Light draws, counted there).
typedef struct vkn_shlib_named_hacks {
    int read, sh_mode;              // read: the library read its first bundle (values below valid); DM_VK_NATIVE_SH in effect
    int fsr, lights, lcap;          // DM_HACK_FSR / DM_HACK_LIGHTS in effect here (0 = not applied), DM_VK_NATIVE_LCAP
    int lsel;                       // [gpu2] DM_VK_NATIVE_LCAP_SEL (0 first N, 1 weak lights, 2 weak lights + poison)
    int state[2];                   // [0 FSR, 1 light cap]: 0 no bundle seen yet, 1 swapped (or checked: FSR=2), -1 refused
    uint64_t swapped[2], refused[2], checked;
    int lcf; uint64_t lcf_bundles;  // DM_VK_NATIVE_LCAP=2: 0 not checked, 1 the capped fused modules match the player's Sky c1, -1 not; bundles made
} vkn_shlib_named_hacks;
void vkn_shlib_named_hacks_get(vkn_shlib_named_hacks *h);

#ifdef __cplusplus
}
#endif
#endif
