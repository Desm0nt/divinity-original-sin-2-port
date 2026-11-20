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
// Not here (other pieces / plan rows): DM_HACK_FSR / DM_HACK_LIGHTS module swaps, NR-ONCHIP / LIGHTFUSE variants, NR-33C surfaces.
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

#ifdef __cplusplus
}
#endif
#endif
