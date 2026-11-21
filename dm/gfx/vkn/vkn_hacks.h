// [DOS2] VK-NATIVE stage 5.4: the player's speed hacks (DM_HACK_*, the gear's "Хаки скорости" page, its 3D % / FSR rows and the hack
// tiers; values and tiers: dm/gfx/nr/nr_hacks.h; docs/dos2/nr/NR-HACKS.md) in mode 1, where the translator that implemented the backend
// half of them (dm/gfx/vulkan/engine/xbg_nr_hacks.inc, xbg_nr_sh.inc hacks) is not initialised. Plan: docs/dos2/nr/VK-STAGE5-PLAN.md
// stage 5.4 (G1 FSR + light cap, G2 sampler and far cascade, G3 DRS and the gear). vkn_hacks.c holds the glue; each hack lives where
// its technique lives:
//   DM_HACK_SCALE    the game's DRS object (dm/host/hle_dos2_quality.c): the engine draws a part of its full-size 3D targets (the
//                    post chain's AA step stretches it), so mode 1 needs no backend part - the targets keep their size
//   DM_HACK_FSR      vkn_shlib_named.cpp: PPFXAA -> the FXAA + EASU module (needs DM_VK_NATIVE_SH=1); composes with vkn_post.c (its
//                    route A skips the AA step only at 100 %, where the module is FXAA bit for bit: vkn_shlib_named.h end block)
//   DM_HACK_LIGHTS   vkn_shlib_named.cpp: ClusteredLightingWithSSAO -> the LCAP module, and with LIGHTFUSE the capped fused module
//                    (DM_VK_NATIVE_LCAP below)
//   DM_HACK_SHPCF    vkn_shlib_named.cpp: the shadow class when DM_VK_NATIVE_SH_PCF is not set (as before)
//   DM_HACK_SHFAR    vkn_shc.c (DM_VK_NATIVE_SHFAR, default from DM_HACK_SHFAR / DM_HACK_TIER): the sun atlas's far tile not drawn
//   DM_HACK_TEXBIAS  vkn_tex.c hack twins of the samplers (DM_VK_NATIVE_TEXHACK: 1 per texture = the old rule, 2 per sampler)
//   DM_HACK_ANISO    the same twins
//   DM_HACK_NOBLOOM, _NOAA, _SHADOWS, _LOD   hle_dos2_quality.c (game settings / code caves): no backend part
// Switches (read once):
//   DM_VK_NATIVE_LCAP  (new; the old path had none: there the cap was silently lost under DM_NR_LIGHTFUSE) what DM_HACK_LIGHTS does
//                      against LIGHTFUSE (vkn_lightfuse.c), whose pack bundle replaces the Light shader's module without a cap:
//                      2 (default) both: the fused draw takes a built-in capped fused module (vkn_shlib_named.h end block; proven = Sky ->
//                      the capped Light, bit for bit, tools/dos2_light_cap_fused.py) when the player's DeferredSky c1 matches it (else red,
//                      no fusion: the cap module draws); 1 the cap wins the simple way: LIGHTFUSE is not installed while the cap module
//                      will be used (DM_HACK_LIGHTS > 0 and DM_VK_NATIVE_SH=1), one line; 0 no cap (red line, LIGHTFUSE as it is).
//                      A/B on the phone: LCAP=2 against 1 with DM_HACK_LIGHTS=4 (fps, "[vkn] lightfuse" fused count, the picture).
//   DM_VK_NATIVE_TEXHACK  vkn_tex.h end block.
// Lines: "[vkn] hacks: ..." once at the first census (every DM_HACK_* value in effect, where it came from, which backend switch / file
// implements it and what that part reports), "【红】[vkn] hacks: ..." for a value that is set but has no working implementation; every
// 10 s while any hack is on, "[vkn] hacks (10 s): ..." with the counters of the parts.
// DRS in mode 1 (G3): the game's controller reads the GPU ms the engine keeps at renderer +0x18f0, which dm/host/hle_dos2_vkn_frame.c
// writes at every presented BeginFrame from the backend's own top / bottom timestamps of the newest finished presented recording
// (vkn_frame_gpu_ns; the same per-frame pair the old path's NVN timestamps gave) - DRS, DM_DOS2_DRS_SPIKE (a cave on that read) work as
// they are. What did not: the translator's xbg_recent_gpu_ms / xbg_recent_draws stay -1 in mode 1, so DM_DOS2_DRS_SETTLE saw "a load
// screen" forever (a held target) and the 10 s line printed "backend measured -1.00". hle_dos2_quality.c asks the functions below in
// mode 1 instead. No game addresses.
#ifndef DM_VKN_HACKS_H
#define DM_VKN_HACKS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

int vkn_hacks_lcap_mode(void);       // DM_VK_NATIVE_LCAP (0 / 1 / 2), read once; any thread
// 0 = LIGHTFUSE must not install: the light cap module will replace the Light shader (DM_HACK_LIGHTS > 0, DM_VK_NATIVE_LCAP=1,
// DM_VK_NATIVE_SH=1 - unset = 1, xbg_nr_sh_default on every family). 1 = no conflict. Install time (vkn_lightfuse.c vkn_lf_install).
int vkn_hacks_lightfuse_ok(void);
// The hacks lines (vkn_shlib_named_census calls it: the 10 s census, render / recording thread). The first call prints the install line.
void vkn_hacks_census(void);
// DRS signals of mode 1 (any thread; hle_dos2_quality.c's monitor, 4 Hz). gpu_ms: the newest presented recording the GPU finished, its
// bottom - top timestamp in ms (-1: none yet / no timestamps). draws: draws a presented frame (vkn_draw_stats draws / vkn_frame) since
// the previous call that saw a frame pass (-1: not known yet; the previous value while no frame passed).
double vkn_hacks_gpu_ms(void);
double vkn_hacks_draws(void);
// The values in effect (nr_hacks.h nrh_value, read once): v[NRH_N], from[NRH_N] ('e' explicit, 't' tier, 0 none). Any thread.
void vkn_hacks_values(int *v, char *from);

#ifdef __cplusplus
}
#endif
#endif
