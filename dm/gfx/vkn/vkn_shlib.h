// [DOS2] VK-NATIVE stage 4, last piece: the backend's own SHADER LIBRARY (vkn_shlib.cpp; the BC / ASTC data of vkn_bcgpu.c in
// vkn_shlib_bc.cpp). In mode 1 (DM_VK_NATIVE=1, DM_VK_NATIVE_SHLIB=1, the default there) it replaces every door into the translator
// (nvn-translate: dm/gfx/vulkan/engine/xbg_game.cpp): xbg_game_initialize is not run, and the stage bundles of the packs are read and
// rewritten here, exactly as the translator's loader did for mode 1's configuration (the comparison host test
// dm/gfx/vulkan/engine/tests/vkn_shlib_test.cpp checks it word for word over the whole pack). Mode 0 / 2 and DM_VK_NATIVE_SHLIB=0 keep
// the old doors (xbg_vkn_shader & co, the translator initialised as before).
//
// ======== Inventory: the old chain (xbg_game.cpp of branch dos2-vk ad19671; line numbers there) and where each part went ========
// entry / reader
//   xbg_vkn_shader 10458, xbg_vkn_alpha_module 10466,     -> vkn_shlib_shader / vkn_shlib_alpha_module / vkn_shlib_retype_module (same
//     xbg_vkn_retype_module 10469                            contract, same xbg_vkn_stage output)
//   load_shader_key 5103 (the base module)               -> sl_load (vkn_shlib.cpp), the same order of steps
//   bundle_words 5362 (the variants' words again)        -> sl_words; alpha_module 8182 -> sl_alpha; vtx_retype_module 8112 -> sl_retype
//   bundle_module 5084 (spvopt lookup + nrubd::opt_ok +  -> sl_module: xbg_spvopt_lookup / _reject (xbg_spvopt.cpp, linked as it is),
//     vkCreateShaderModule under gpguard::ModuleLock)        the read check of NR-08b, the module lock of this library (below)
//   xpack::open_once / get / check / read_translated     -> sl_pack (same index checks, CRC-32, DM_GFX_SHADER_PACK 0 / 1 / 2)
//     4821-4915
//   xpack::read 4903: NR-ONCHIP oc_apply, NR-04 nrsh::apply -> dropped: DM_NR_ONCHIP / DM_NR_SH are forced 0 in mode 1 (dm_dos2_vkn_pre)
//   xpack::native 4853 (DM_GFX_NATIVE_SHADERS swap/check)  -> sl_native, kept bit for bit (native.xgspack / native/<key>.xgs)
//   nbpost::fused_key / read_bundle ("F1." keys)          -> dropped: engine stage keys are 16 hex digits, never "F1.<key>"
//   spv_may_exist / spv_override 4919-4935               -> sl_spv_override (the listing of spv-override once)
//   pgv2::note_sha 4964 (pipeguard spv identities)       -> only for the lone-lane list below (no translator pipelines exist)
//   missing-bundle log + .rec dump 5111-5122            -> sl_load, the same file format
// load-time rewrites, in this order (load_shader_key 5139..5157, bundle_words 5373)
//   gscull_rewrite 4599 (xbg_spirv_gscull.h: lone.cpp)   -> sl_gscull      gather_ext_rewrite 4624            -> sl_gather_ext
//   lone_rewrite 4658 (xbg_spirv_lone.cpp; lone_forced    -> sl_lone (+ the  gather_snap_rewrite 4652 (gather.cpp)-> sl_gather_snap
//     4530: pipeguard lone-lane list 5632)                   list reader)   module_gap / validate_capabilities -> sl_gap (no abort:
//   early_tests_rewrite 4562                             -> sl_early         4485 (cap_soft)                       a hard gap = unusable)
//   invariant_rewrite 4539                               -> sl_invariant    quad_rewrite 4505 (quad.cpp)        -> sl_quad
//   warp_rewrite 4696 (warp.cpp)                         -> sl_warp          tidy_rewrite 4802: nan_minmax_swap 4786, xbg_spirv_ldc_fold,
//   idvs_rewrite 4745 (idvs.cpp)                         -> sl_idvs            forward_temps, ldc_canon, f2i_notrunc, texcoord_unguard,
//   gfx-shader-reuse (warm::take_rewritten 5358)         -> dropped:           ubovec_rewrite 4755              -> sl_tidy, same order
//     the warm-up never runs in mode 1, nothing to take
// after the rewrites (load_shader_key 5161..5183; only what reaches xbg_vkn_shader's output)
//   uniform-extent trim (spirv_uniform_extent 4268)       -> sl_uniform_extent: binding bytes of XBG_SOURCE_UNIFORM blocks
//   vertex input classes (xbg_vertex_input_classes,       -> the same call (a failure: unusable instead of the translator's abort)
//     xbg_vertex.c)
//   nrubd::mark (xbg_nr_ubodyn.inc 51)                   -> sl_ubd_mark: XBG_BIND_SCANNED / _UNREAD flags (nr_spv_binding_reads)
//   discards / empty_fs / raster facts / vindex / pos /   -> not built: none of them reaches xbg_vkn_stage (translator draws only)
//     storage writes / pass graph / posonly / preskin /
//     xlq masks / idvs_stripped / nrmat / nrsurf / nrheat
//   warm::register_module                                -> dropped (the translator's warm-up list)
// device facts (xbg_game_initialize 9892; config from vulkan_present.cpp through nvn_vk.c dm_vk_initialize)
//   properties (vendorID, deviceName, apiVersion, driverVersion, pipelineCacheUUID: vkGetPhysicalDeviceProperties 9914), subgroup
//   properties (vkGetPhysicalDeviceProperties2 9915), cfg.enabled_core + enabled_shader_draw_parameters (the presenter's config),
//   DM_GFX_SIM_GPU narrowing (xbg_sim_parse / _features / _subgroup 9941-9944), gpu_policy = xbg_effective_family 9945,
//   format_feature_flags2 (apiVersion >= 1.3 or the extension listed, 10111), the queue family's timestampValidBits and
//   occlusionQueryPrecise (the d3-perf self-test input 9859), dm_vk_driver_status (the lone-lane list file of the running driver)
//   -> vkn_shlib_init (the same queries, on the same config, at the same moment: before dm_main, nothing else on the queue).
//
// ======== Switches (read once in vkn_shlib_init; same names, defaults and precedence as the translator's init) ========
// family default = xbg_vendor_policy.h for the policy family (DM_GFX_SIM_GPU wins over the real one). A / M / P = Adreno / Mali / PowerVR.
//   switch (marker file in the parent of shaders/)   default A / M / P        what it changes
//   DM_VK_NATIVE_SHLIB (vkn_shlib_on)                 1 in mode 1              0 = the translator's doors (initialised library-only)
//   DM_GFX_SHADER_PACK                                1                        0 loose files only, 2 read both, the file decides (counts)
//   DM_GFX_NATIVE_SHADERS                             0 (env)                  1 native.xgspack / native/<key>.xgs replaces a bundle whose
//                                                                              head equals the translated head, 2 compare only
//   DM_GFX_SPV_OVERRIDE_PROBE                         0                        1 = probe spv-override/<key>.spv per shader (else listed once)
//   DM_GFX_GSCULL_FIX                                 0x61                     geometry-shader culling rewrites (no DOS2 bundle has a GS)
//   DM_PIPEGUARD / DM_PIPEGUARD_DRIVER                2 / -                    the lone-lane list read: v2 header and spv identity / v1
//   DM_GFX_EARLY_Z                                    1                        EarlyFragmentTests for fragment shaders with stores
//   DM_GFX_INVARIANT                                  0                        Invariant + NoContraction on vertex-pipeline arithmetic
//   DM_GFX_GATHER_SNAP                                1 (DOS2)                 Switch gather footprint (1 marked shaders, 2 every gather)
//   DM_GFX_QUAD_DERIVATIVES (gfx-quad-derivatives)    off; A 740-799 / 8xx     quad shuffles -> fine derivatives (fragment); the app sets 1;
//     + DM_GFX_ADRENO8_POLICY ("0" in DOS2)            policy: off unless marker  off without fragment quad subgroup operations
//   DM_GFX_WARP32 (gfx-warp32)                        as above (app sets 1)     32-lane warps in wider subgroups (compute)
//   DM_GFX_WARP_MODE                                  lone unless subgroup 32  native / lone: the lone-lane form for every stage with
//                                                                              subgroup operations (compute keeps them under warp32)
//   DM_GFX_LDC_FOLD (gfx-ldc-fold)                    0 (app sets 1)           constant-buffer component fold
//   DM_GFX_FORWARD_TEMPS (gfx-forward-temps)          0 (app sets 1)           single-store temporaries forwarded
//   DM_GFX_NAN_MINMAX (gfx-nan-minmax)                0 (app sets 1)           FMin / FMax / FClamp -> NMin / NMax / NClamp
//   DM_GFX_NAN_SAT / _LDC_CANON / _F2I_NOTRUNC        DM_GFX_D3PERF (app 1): A on; M / P: the GPU self-test decides
//     + DM_GFX_D3PERF, DM_GFX_SELFTEST(_FORCE_FAIL)    (xbg_selftest.cpp, run here at init as the translator did); env wins
//   DM_GFX_TEXCOORD_NAN                               1                        0 = the NaN guard on sample coordinates goes
//   DM_NR_UBOVEC                                      A 0 / M 1 / P 0          vec4 loads for scalar uniform reads (checked)
//   DM_NR_IDVS                                        A 0 / M 1 / P 1          gl_PointSize store dropped from vertex shaders (checked)
//   DM_NR_UBODYN + DM_GFX_UBO_DYNAMIC                 A 0 / M 0 / P 1 (scan)   uniform bindings marked read / unread (flags)
//   DM_GFX_ALPHA_TEST                                 1 (DOS2)                 the "#at" fragment variants (0: the base module)
//   DM_GFX_SPV_OPT                                    0                        1 / 2 optimized-copy cache (<parent>/spvopt)
//   DM_GFX_PIPE_SERIAL                                A 0 / M 0 / P 1          vkCreateShaderModule under this library's module lock
//   DM_GFX_VTX_RETYPE                                 A 0 / M 1 / P 1          only whether a vertex shader's final words are kept for its
//                                                                              "#vi" variants (else they are rebuilt: the same words)
//   DM_GFX_SIM_GPU                                    -                        simulated family / features / subgroup (see above)
//   Not read (forced 0 in mode 1 by dm_dos2_vkn_pre): DM_NR_SH, DM_NR_ONCHIP, DM_NR_MAT, DM_NR_SH_SURF, DM_NR_XLQ, DM_NR_LIGHTFUSE.
//   Not used (translator-only): gfx-shader-reuse, the warm-up, pipeguard skip / inflight lists.
// Module lock (gpguard::ModuleLock, KB gpu-powervr.md PVR-05): PowerVR's compiler runs inside vkCreateShaderModule as well and is not
// thread-safe; the translator created modules under the lock its pipeline builds took (DM_GFX_PIPE_SERIAL). The backend builds
// pipelines on the render thread and the warm-up thread while this library creates modules on the shader worker: the reason applies, so
// modules are created under vkn_shlib_create_lock() with DM_GFX_PIPE_SERIAL, and the lock is exported for the backend's pipeline builds
// (vkn_state.c / vkn_warm.c / vkn_ui.c / vkn_bcgpu.c do not take it yet: open point for PowerVR).
#ifndef DM_VKN_SHLIB_H
#define DM_VKN_SHLIB_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "../vulkan/engine/xbg_game.h"   // xbg_vkn_stage, xbg_shader_binding (types only)
#ifdef __cplusplus
extern "C" {
#endif

// What the presenter gave the translator (xbg_game_config): the device and the shader directory. Copied.
typedef struct vkn_shlib_config {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t queue_family;
    VkPhysicalDeviceFeatures enabled_core; VkBool32 enabled_shader_draw_parameters;
    const char *shader_directory;   // <...>/shaders (bundles, shaders.xgspack); its parent holds markers, spv-override, caches
} vkn_shlib_config;
// Once, before the game starts (nvn_vk.c dm_vk_initialize in mode 1, in place of xbg_game_initialize): device facts, switches, the
// d3-perf self-test off Adreno (it submits to the queue: nothing else may). 0 = ok.
int vkn_shlib_init(const vkn_shlib_config *config, char *error, size_t error_bytes);
int vkn_shlib_ready(void);
// Mode 1 with DM_VK_NATIVE_SHLIB != 0 (env, read once; unset = 1) and step B of DM_VK_NATIVE_NONVN (the device start without HLE NVN,
// whose bodies would reach the translator): the backend takes its shaders from this library and the translator is not initialised.
// 0 in every other mode (and for DM_VK_NATIVE_SHLIB=0: the translator's doors, initialised library-only as before).
int vkn_shlib_on(void);

// xbg_vkn_shader's contract: 0 = loaded (out filled; module VK_NULL_HANDLE = the device lacks a capability), 1 = no bundle for the key
// (logged and dumped as a job record when code != NULL), -1 = not initialised. The module and the shader object live until the process
// ends; bindings / inputs are copies, c1 points into the shader object.
int vkn_shlib_shader(const char *key, const void *control, const void *code, xbg_vkn_stage *out);
// Fragment module with the alpha test (NVN compare func 1..8, reference), "#at" variant; the base module for ALWAYS, DM_GFX_ALPHA_TEST=0,
// a vertex / compute shader, an unusable shader or a refusal. Built once per (func, reference).
VkShaderModule vkn_shlib_alpha_module(void *shader, uint32_t func, float ref);
// Vertex module for a retype code ("#vi": low 32 bits 2 bits per location 0 keep / 1 float / 2 sint / 3 uint; high 32 bits 1 SSCALED fed
// as SINT, 2 USCALED fed as UINT); the base module for code 0, another stage or a refusal. Built once per code.
VkShaderModule vkn_shlib_retype_module(void *shader, uint64_t code);
// The directory of the backend's cache files (the parent of the shader directory, as xbg_vkn_cache_dir). 0 = written.
int vkn_shlib_cache_dir(char *out, size_t bytes);
// The module lock (taken only while DM_GFX_PIPE_SERIAL is on): vkCreateShaderModule here, and for any backend pipeline build that wants
// PowerVR's rule. Recursive-safe for one level is NOT given: do not nest.
void vkn_shlib_create_lock(void);
void vkn_shlib_create_unlock(void);
// CRC-32 + Adler-32 of a texture's storage (the ASTC pack key; xbg_astc_storage_key's 1 GiB chunks). 0 = ok.
int vkn_shlib_astc_key(const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler);

typedef struct vkn_shlib_stats {
    uint64_t loads, missing, unusable, from_pack, from_file, native_used, alpha_variants, alpha_refused, retype_variants, retype_refused;
    uint64_t module_fail, gaps_hard, ns;
} vkn_shlib_stats;
void vkn_shlib_stats_get(vkn_shlib_stats *out);
// One line: what the library is and holds (the boot line of hle_dos2_vkn_nonvn.c, the census).
int vkn_shlib_report(char *out, size_t bytes);

// ---- the guard: xbg_game.cpp's extern "C" entry points call this (weak there, XBG_VKN_ENTERED); it counts only while vkn_shlib_on() ----
enum { VKN_SHLIB_X_INIT, VKN_SHLIB_X_SHADER, VKN_SHLIB_X_ALPHA, VKN_SHLIB_X_RETYPE, VKN_SHLIB_X_CACHE_DIR, VKN_SHLIB_X_LIB_REPORT, VKN_SHLIB_X_SUBMIT,
       VKN_SHLIB_X_PRESENT, VKN_SHLIB_X_SHUTDOWN, VKN_SHLIB_X_STATS, VKN_SHLIB_X_N };
void vkn_shlib_translator_entered(int which);
// The calls so far by entry (out[VKN_SHLIB_X_N]); returns their sum. names: "xbg_game_initialize", ... ("statistics" = xbg_nr_mem_usage /
// xbg_nr_mem_trim of hle_dos2_nr_mem.c; xbg_recent_gpu_ms / _draws are not counted: source_structure_test pins their text).
uint64_t vkn_shlib_translator_calls(uint64_t out[VKN_SHLIB_X_N]);
const char *vkn_shlib_translator_entry_name(int which);

// ---- [DOS2] GPU BC route data (vkn_shlib_bc.cpp; was xbg_game.cpp's VK-NATIVE export block): pure functions of their arguments ----
// The declarations are dm/gfx/vkn/vkn_bcgpu.h (vkn_shlib_bc_*), kept there next to their only caller.

// ==== [DOS2] VK-NATIVE stage 5.1 piece O2 (vkn_query.c, DM_VK_NATIVE_GBUF_LE) ====
// A fragment shader object of this library (vkn_shader.xs[VKN_SRC_FS] while vkn_shlib_on()): what its final words (every load-time
// rewrite applied: the words its base module is made of) can do to depth / coverage. -1 = not a fragment shader of this library.
// The "#at" alpha-test variants discard on top of this (the caller knows from the draw's alpha reference). Any thread.
enum { VKN_SHLIB_FS_KILL = 1, VKN_SHLIB_FS_DEPTH = 2, VKN_SHLIB_FS_MASK = 4 };
int vkn_shlib_fs_facts(const void *shader);

// ---- host tests: the words handed to vkCreateShaderModule are what the comparison test checks (it mocks that entry point) ----
#ifdef __cplusplus
}
#endif
#endif
