// [DOS2] VK-NATIVE stage 4, part "no NVN" (DM_VK_NATIVE=1 only): NVN, the HLE NVN layer (dm/gfx/nvn/hle_nvn.c) and the translator
// (nvn-translate: dm/gfx/vulkan/nvn_vk.c + engine/xbg_game.cpp) taken out of the mode-1 path. main = main.raw of update v786432, build
// 4979b200; the game's NVN function table = 515 u64 slots at main+0x2bbee98 (slot i at +8i; index -> name: the first 515 rows of
// dm/gfx/nvn/names.txt = src/work/re-nr-nrend/nvn_slot_map.tsv, checked equal); dev = [this+0x18] of device init 0x938330.
//
// Switch DM_VK_NATIVE_NONVN (read once; mode 1 only, every other mode ignores it): unset / 1 = every step; 0 = none (the behaviour
// before this part); otherwise a bit mask for a phone bisect (bit 0 B = 1, bit 1 C = 2, bit 2 D = 4; "1" means "all", so B alone is
// written 9 = B + bit 3, which means nothing else). The census wrappers of step A are no step: they are in every mode-1 run (they
// forward exactly, see dm_vkn_nv_fw_stubs).
//   B (1)  device start without NVN (below, "B"): the bootstrap and both table fills answered by the backend, no HLE NVN pointer in
//          the game's table, no HLE NVN body called at all.
//   C (2)  the translator's initialise is "library only" (xbg_game.cpp, its "no-NVN step C" lines): every switch, device fact, cap and
//          format query, the presenter's present-thread mode, the shader pack and the SPIR-V rewrite switches the backend's modules go
//          through (load_shader_key) are set up as always; NOT done: the pipeline cache (vk-pipeline-cache.bin neither read nor saved),
//          the warm-up (its list not read, no thread building the translator's pipelines, no seed), the pool reset thread. What still
//          exists of the translator in mode 1: its command pool (one object), the shader objects / modules the backend asked for, the
//          xpack index, the inert wtrack / event-guard fault hooks of nvn_vk.c, hle_nvn.c's constructors (static tables).
//   D (4)  the frame counter and the on-screen frame statistics come from this file (below, "D").
//          With DM_VK_NATIVE_SHLIB on (mode 1's default; the shader library piece, dm/gfx/vkn/vkn_shlib.h) C is superseded: the translator
//          is NOT initialised at all (nvn_vk.c dm_vk_initialize runs vkn_shlib_init instead of xbg_game_initialize, plus the presenter's
//          present-thread mode), the backend takes shaders, variants, the cache directory, the ASTC key and the GPU BC data from its own
//          library, and nothing of xbg_game.cpp runs: its extern "C" entry points count their calls into the library (XBG_VKN_ENTERED), the
//          boot line says "translator not initialised" and names any entry reached in a red line (then every 10 s while the count grows).
//          DM_VK_NATIVE_SHLIB=0: the doors into the translator and the library-only initialise of C, as before this piece.
//   Bisect on the phone: 0 = before this part; 9 = B only; 2 = C only; 4 = D only; 3 / 5 / 6 = pairs; unset = all. A wrong B shows at
//   launch (black screen / crash in device init) and in the line "nonvn: B: ..." after device init; C in the translator's log lines and
//   the boot line; D in the on-screen fps and the DRS lines.
//
// ======== A. Inventory (20.11.2026, read from the sources and main.raw; the census lines below prove it on the phone) ========
// (i) NVN entries the GAME can call in mode 1, and where they land (static: every engine NVN call is `ldr xN, [table + 8i]; blr xN`,
//     src/work/re-nr-nrend/nvn_caller_index.tsv: 175 slots are called from 1240 sites; the bootstrap import 0x1bf6710 has two callers,
//     0x938504 / 0x938518, both in device init; the stored GetProcAddress main+0x2bbfeb8 is written at 0x93850c and read at 0x93851c only)
//     what                                   slots  called by                          lands today (B off)          lands with B
//     command / queue / sync entries          132   46 of them (frame, immediate       hle_dos2_vkn_cmd.c (ACTION /  the same
//                                                   lists, UI, barriers, counters)     COUNTED / FORWARD)
//       of these QueueFenceSync (497)           1   0x939850 at 0x9399dc, 3 times       FORWARD -> HLE x_fence       nv_fence (local)
//       of these WindowAcquireTexture (77)      1   0x9215f0 only (replaced)            FORWARD -> HLE (unreachable) nv_acquire (local)
//     creation-time entries                   123   all 123 (0x91e2a0 ... 0x93c3a0, the hle_dos2_vkn_create.c stubs  the same, put in by the
//                                                   UI block, run-time getters)        (0x932260's pre-hook)        table fill itself
//       of these TextureGetStride (217)         1   9 storage writers (glue part)       glue wrapper over the stub   the same
//       of these TextureReadTexels (222)        1   0x9291c0 / 0x9296d0 / 0x929880      hle_dos2_vkn_read.c          the same
//     DeviceBuilderSetDefaults (0)            1   0x938534 (once)                     HLE NVN                      nv_dev_defaults
//     DeviceInitialize (3)                    1   0x938548 (once; slot 3 is also      HLE NVN                      nv_dev_init
//                                                   written with the bootstrap's answer at 0x938520)
//     DeviceSetWindowOriginMode (15)          1   0x938588 (once, mode 1)             HLE NVN                      nv_dev_origin
//     DeviceSetDepthMode (16)                 1   0x938570 (once, mode 1)             HLE NVN                      nv_dev_depth
//     DeviceGetProcAddress (6; via 0x2bbfeb8) 1   0x934d70: 2 x 515 calls             HLE nvn_lookup               nv_gpa
//     QueueBuilderGetCompute / Control-       2   none (the index rows 0x1bee3e0 /    HLE thunk (wrapped by A)     unexpected stub
//       MemorySize (49, 50)                           0x9201f0 / 0x920210 are plain loads, no call: checked with capstone)
//     the other slots                       256   none                                HLE thunk (wrapped by A)     unexpected stub
//   After device init 0x938330 returns nothing of the game calls HLE NVN (the 4 device slots and both fills run inside it, the 3
//   fences inside its tail 0x939b00 -> 0x939850). The census proves it: at the end of the command part's repointing every slot that
//   still holds a pointer that is not the backend's is wrapped in a counting forwarder (dm_vkn_nv_fw_stubs: x9 / x10 / x16 / x17 and
//   the flags only, then `br` to the old entry: the call is exactly the old one); the line `nvn: N game calls reached HLE NVN since
//   start (by entry)` every 10 s counts them (N = 0 expected; > 0 is red, with the entries and their last callers).
// (ii) Host calls from the backend (dm/gfx/vkn, dm/host/hle_dos2_vkn*) into hle_nvn.c / nvn_vk.c / the translator (grep of the
//     externs, 20.11):
//     hle_nvn.c     nvn_bigstack_top (hle_dos2_vkn_stack.c: the 1 MiB host stack per thread + 16 KiB guard page, one mmap per
//                   thread, shared with the NVN entry; a library function, kept), dm_nvn_vkn_frame (hle_dos2_vkn_frame.c, every
//                   presented frame: replaced by D), g_nvn_present_period_ns (a variable the frame part writes for the DRS of
//                   hle_dos2_quality.c: data, kept), nvn_slot[] (this file's census: the HLE thunks' call counters, read only)
//     nvn_entry.S   nvn_on_bigstack (dm_vkn_on_stack: the packet handlers on the host stack; library, kept)
//     nvn_vk.c      mtl_display_refresh, mtl_has_window, mtl_fps_target, dm_dos2_fps (the frame part's pacing inputs: presenter /
//                   settings queries, no translator state; kept), mtl_res_snapshot (this file's boot line: pools the translator knows)
//     nvn_tex.c     nvn_tex_layout, nvn_fmt_bpb (format tables of the creation / read-back / ASTC parts; not HLE: kept)
//     nvn_pace.h    nvn_pace*, nvn_fps_* (inline pacing math; kept)
//     xbg_game.cpp  (before the shader library piece) xbg_vkn_shader / xbg_vkn_alpha_module / xbg_vkn_retype_module (vkn_shader.c:
//                   load_shader_key with every SPIR-V rewrite and its variants, once per stage / variant), xbg_vkn_cache_dir (vkn_warm.c),
//                   xbg_astc_storage_key (vkn_astc.c, a pure hash), xbg_vkn_bc_* (vkn_bcgpu.c), this part's xbg_vkn_lib_report. With
//                   DM_VK_NATIVE_SHLIB on: none (vkn_shlib_* of dm/gfx/vkn; the GPU BC data and the ASTC key in every mode). Pure units
//                   kept: xbg_shader_key.c xbg_shader_program_key, xbg_vulkan.cpp xbg_support_defaults, the rewrite units xbg_spirv_*.cpp,
//                   xbg_vertex.c, xbg_spvopt.cpp, xbg_selftest.cpp (the d3-perf self-test off Adreno), nr_ubodyn.c
//     (No other dm_nvn_* / mtl_* / xbg_* call from the backend; the HLE NVN object layouts of nvn_obj.h are headers, not code.)
// (iii) What HLE NVN and the translator initialise and run at boot in mode 1 (before this part):
//     hle_nvn.c   constructors at load: nvn_nops_register (nvn_init_slots: the 523-slot table from the swnvn section, one log line),
//                 ub_sub_register; at the two bootstrap calls nvn_lookup: DM_NVN_TRACE / big-stack wrappers over every implemented
//                 slot, nvn_lc_install (DM_NR_LEANCALL), nvn_count_start (a thread only with DM_NVN_COUNT), nvn_p2_install
//                 (DM_NVN_PROF=2), the thunk table pick; then 1030 GetProcAddress lookups (a strcmp walk over 523 names each),
//                 DeviceInitialize / SetDepthMode / SetWindowOriginMode (2 log lines), QueueFenceSync x3 (x_fence = g_x_subs, no
//                 job). Per frame: dm_nvn_vkn_frame (fstat_present, dump_frame: a 523-slot counter diff, the 300-frame log line,
//                 cs_dump, dm_guest_frame_tick; @frame_ms with DM_NVN_TSV). NOT started in mode 1: the translation thread x_thread
//                 "nvn-translate" (8 MiB stack, lazily at the first x_push: submit / present / frame jobs; the command part sinks
//                 those slots and dm_nvn_vkn_frame pushes nothing), nvn_dirty (armed only by HLE nvnMemoryPoolInitialize:
//                 nvn_dirty_add_range registers the fault hook and the range; the creation part's 'b' group answers
//                 MemoryPoolInitialize, so no range is registered and nothing is mprotected: the translator protects registered
//                 ranges only), the zero-copy pool mapping xbg_zc_pool_map and mtl_pool_add (both only from HLE
//                 nvnMemoryPoolInitialize: never in mode 1).
//     translator  dm_vk_initialize (nvn_vk.c, before dm_main) -> xbg_game_initialize: ~70 switches and marker files, capability /
//                 format report, gpguard::load (pipe-skip files), a command pool, the pipeline cache: vk-pipeline-cache.bin read
//                 whole (< 256 MB; 17.7 MB on the Redmi K50 Ultra) + vkCreatePipelineCache, its save every 300 translator presents
//                 (never in mode 1) and at shutdown; warm::start: vk-pipeline-list.bin read (< 64 MB) and, when it holds records,
//                 min(3, max(1, cores / 3)) warm-up threads building the TRANSLATOR's pipelines (nothing in mode 1 uses them);
//                 pseed (DM_NR_SEED, forced 0 by dm_dos2_vkn_pre), poolreset (a thread only with its marker), nrheat (DM_NR_HEAT
//                 forced 0), crumbs (a thread only with DM_GFX_CRUMBS), ptq::init (the presenter's present-thread mode: the backend
//                 presents through it, kept), the shader pack index (lazily at the first bundle, 56 bytes a bundle; kept). No gfx
//                 worker thread (gfx_async false on Android), no staging arenas / slots until a submission. dm_vk_initialize also
//                 registers the wtrack / event-guard fault hooks (inactive without their markers).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/nvn/nvn_count.h"   // NVN_NFUNC: the HLE slot table (523 names; the first 515 are the game's table)
#include "../gfx/nvn/nvn_obj.h"      // dev_builder_t / dev_t_ / sync_t: the in-place layouts HLE NVN used (the creation part's too)
#include "../gfx/nvn/nvn_protos.h"   // NVNT_* prototypes: every bootstrap stub is checked against its NVN signature
#include "../gfx/nvn/nvn_mtl.h"      // mtl_res_snapshot (nvn_vk.c): the GPU pools the translator was told about (boot line)
#include "../gfx/nvn/nvn_dirty.h"    // nvn_dirty_stat / nvn_dirty_mp_stats: ranges under write tracking, mprotect calls (boot line)
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_stack.h"

#define NV_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark, a literal of its own
enum { NV_SLOTS = 515, M_TABLE = 0x2bbee98, M_TEXT_END = 0x1e80000 };

// ---- the API of this part (the other sides declare these weak) ----
unsigned dm_vkn_nonvn_mask(void);          // the steps in effect: bit 0 B, 1 C, 2 D (0 outside mode 1); xbg_game.cpp, hle_nvn.c
void dm_vkn_nonvn_register(void);          // hle_dos2_vkn_cmd.c, at the end of its registration (install time)
void dm_vkn_nonvn_after_init(void);        // hle_dos2_vkn_cmd.c, at the end of its repointing (device init 0x938330 returned)
// From the other parts (weak: a part that is not built is absent).
int dm_vkn_cmd_slot(unsigned slot_index, void **fn) __attribute__((weak));      // hle_dos2_vkn_cmd.c
int dm_vkn_create_slot(unsigned slot_index, void **fn) __attribute__((weak));   // hle_dos2_vkn_create.c
int dm_vkn_read_slot(unsigned slot_index, void **fn) __attribute__((weak));     // hle_dos2_vkn_read.c
int dm_vkn_glue_is_mine(uint64_t fn) __attribute__((weak));                     // hle_dos2_vkn_glue.c
void dm_vkn_cmd_set_forward(unsigned slot_index, uint64_t fn) __attribute__((weak));   // hle_dos2_vkn_cmd.c (B: its FORWARD target)
void *dm_vkn_nonvn_boot(const char *name);   // hle_nvn.c h_nvnBootstrapLoader asks first (B); NULL = HLE NVN answers as before
// HLE NVN's own call counters (hle_nvn.c nvn_slot: the thunk of slot i adds to cnt before the body; layout fixed by nvn_thunks.S).
typedef struct { uint64_t cnt; void *fn; } nv_hle_slot;   // (cnt is _Atomic uint64_t there: same size and layout)
extern nv_hle_slot nvn_slot[NVN_NFUNC] __attribute__((weak));

// ---- the game's table: index -> name without "nvn" (the first 515 names of dm/gfx/nvn/names.txt) ----
static const char *const k_nv_name[] = {
    "DeviceBuilderSetDefaults", "DeviceBuilderSetFlags", "DeviceBuilderGetFlags", "DeviceInitialize", "DeviceFinalize",
    "DeviceSetDebugLabel", "DeviceGetProcAddress", "DeviceGetInteger", "DeviceGetCurrentTimestampInNanoseconds",
    "DeviceSetIntermediateShaderCache", "DeviceGetTextureHandle", "DeviceGetTexelFetchHandle", "DeviceGetImageHandle",
    "DeviceInstallDebugCallback", "DeviceGenerateDebugDomainId", "DeviceSetWindowOriginMode", "DeviceSetDepthMode",
    "DeviceRegisterFastClearColor", "DeviceRegisterFastClearColori", "DeviceRegisterFastClearColorui",
    "DeviceRegisterFastClearDepth", "DeviceGetWindowOriginMode", "DeviceGetDepthMode", "DeviceGetTimestampInNanoseconds",
    "DeviceApplyDeferredFinalizes", "DeviceFinalizeCommandHandle", "DeviceWalkDebugDatabase", "DeviceGetSeparateTextureHandle",
    "DeviceGetSeparateSamplerHandle", "DeviceIsExternalDebuggerAttached", "QueueGetError", "QueueGetTotalCommandMemoryUsed",
    "QueueGetTotalControlMemoryUsed", "QueueGetTotalComputeMemoryUsed", "QueueResetMemoryUsageCounts", "QueueBuilderSetDevice",
    "QueueBuilderSetDefaults", "QueueBuilderSetFlags", "QueueBuilderSetCommandMemorySize", "QueueBuilderSetComputeMemorySize",
    "QueueBuilderSetControlMemorySize", "QueueBuilderGetQueueMemorySize", "QueueBuilderSetQueueMemory",
    "QueueBuilderSetCommandFlushThreshold", "QueueBuilderSetQueuePriority", "QueueBuilderGetQueuePriority",
    "QueueBuilderGetDevice", "QueueBuilderGetFlags", "QueueBuilderGetCommandMemorySize", "QueueBuilderGetComputeMemorySize",
    "QueueBuilderGetControlMemorySize", "QueueBuilderGetCommandFlushThreshold", "QueueBuilderGetMemorySize",
    "QueueBuilderGetMemory", "QueueInitialize", "QueueFinalize", "QueueSetDebugLabel", "QueueSubmitCommands", "QueueFlush",
    "QueueFinish", "QueuePresentTexture", "QueueAcquireTexture", "WindowBuilderSetDevice", "WindowBuilderSetDefaults",
    "WindowBuilderSetNativeWindow", "WindowBuilderSetTextures", "WindowBuilderSetPresentInterval",
    "WindowBuilderSetNumActiveTextures", "WindowBuilderGetDevice", "WindowBuilderGetNumTextures", "WindowBuilderGetTexture",
    "WindowBuilderGetNativeWindow", "WindowBuilderGetPresentInterval", "WindowBuilderGetNumActiveTextures", "WindowInitialize",
    "WindowFinalize", "WindowSetDebugLabel", "WindowAcquireTexture", "WindowGetNativeWindow", "WindowGetPresentInterval",
    "WindowSetPresentInterval", "WindowSetCrop", "WindowGetCrop", "WindowSetNumActiveTextures", "WindowGetNumActiveTextures",
    "WindowGetNumTextures", "ProgramInitialize", "ProgramFinalize", "ProgramSetDebugLabel", "ProgramSetShaders",
    "ProgramSetShadersExt", "ProgramSetSampleShading", "MemoryPoolBuilderSetDevice", "MemoryPoolBuilderSetDefaults",
    "MemoryPoolBuilderSetStorage", "MemoryPoolBuilderSetFlags", "MemoryPoolBuilderGetDevice", "MemoryPoolBuilderGetMemory",
    "MemoryPoolBuilderGetSize", "MemoryPoolBuilderGetFlags", "MemoryPoolInitialize", "MemoryPoolSetDebugLabel",
    "MemoryPoolFinalize", "MemoryPoolMap", "MemoryPoolFlushMappedRange", "MemoryPoolInvalidateMappedRange",
    "MemoryPoolGetBufferAddress", "MemoryPoolMapVirtual", "MemoryPoolGetSize", "MemoryPoolGetFlags", "TexturePoolInitialize",
    "TexturePoolSetDebugLabel", "TexturePoolFinalize", "TexturePoolRegisterTexture", "TexturePoolRegisterImage",
    "TexturePoolGetMemoryPool", "TexturePoolGetMemoryOffset", "TexturePoolGetSize", "SamplerPoolInitialize",
    "SamplerPoolSetDebugLabel", "SamplerPoolFinalize", "SamplerPoolRegisterSampler", "SamplerPoolRegisterSamplerBuilder",
    "SamplerPoolGetMemoryPool", "SamplerPoolGetMemoryOffset", "SamplerPoolGetSize", "BufferBuilderSetDevice",
    "BufferBuilderSetDefaults", "BufferBuilderSetStorage", "BufferBuilderGetDevice", "BufferBuilderGetMemoryPool",
    "BufferBuilderGetMemoryOffset", "BufferBuilderGetSize", "BufferInitialize", "BufferSetDebugLabel", "BufferFinalize",
    "BufferMap", "BufferGetAddress", "BufferFlushMappedRange", "BufferInvalidateMappedRange", "BufferGetMemoryPool",
    "BufferGetMemoryOffset", "BufferGetSize", "BufferGetDebugID", "TextureBuilderSetDevice", "TextureBuilderSetDefaults",
    "TextureBuilderSetFlags", "TextureBuilderSetTarget", "TextureBuilderSetWidth", "TextureBuilderSetHeight",
    "TextureBuilderSetDepth", "TextureBuilderSetSize1D", "TextureBuilderSetSize2D", "TextureBuilderSetSize3D",
    "TextureBuilderSetLevels", "TextureBuilderSetFormat", "TextureBuilderSetSamples", "TextureBuilderSetSwizzle",
    "TextureBuilderSetDepthStencilMode", "TextureBuilderGetStorageSize", "TextureBuilderGetStorageAlignment",
    "TextureBuilderSetStorage", "TextureBuilderSetPackagedTextureData", "TextureBuilderSetPackagedTextureLayout",
    "TextureBuilderSetStride", "TextureBuilderSetGLTextureName", "TextureBuilderGetStorageClass", "TextureBuilderGetDevice",
    "TextureBuilderGetFlags", "TextureBuilderGetTarget", "TextureBuilderGetWidth", "TextureBuilderGetHeight",
    "TextureBuilderGetDepth", "TextureBuilderGetLevels", "TextureBuilderGetFormat", "TextureBuilderGetSamples",
    "TextureBuilderGetSwizzle", "TextureBuilderGetDepthStencilMode", "TextureBuilderGetPackagedTextureData",
    "TextureBuilderGetPackagedTextureLayout", "TextureBuilderGetStride", "TextureBuilderGetSparseTileLayout",
    "TextureBuilderGetGLTextureName", "TextureBuilderGetZCullStorageSize", "TextureBuilderGetMemoryPool",
    "TextureBuilderGetMemoryOffset", "TextureBuilderGetRawStorageClass", "TextureViewSetDefaults", "TextureViewSetLevels",
    "TextureViewSetLayers", "TextureViewSetFormat", "TextureViewSetSwizzle", "TextureViewSetDepthStencilMode",
    "TextureViewSetTarget", "TextureViewGetLevels", "TextureViewGetLayers", "TextureViewGetFormat", "TextureViewGetSwizzle",
    "TextureViewGetDepthStencilMode", "TextureViewGetTarget", "TextureViewCompare", "TextureInitialize",
    "TextureGetZCullStorageSize", "TextureFinalize", "TextureSetDebugLabel", "TextureGetStorageClass", "TextureGetViewOffset",
    "TextureGetFlags", "TextureGetTarget", "TextureGetWidth", "TextureGetHeight", "TextureGetDepth", "TextureGetLevels",
    "TextureGetFormat", "TextureGetSamples", "TextureGetSwizzle", "TextureGetDepthStencilMode", "TextureGetStride",
    "TextureGetTextureAddress", "TextureGetSparseTileLayout", "TextureWriteTexels", "TextureWriteTexelsStrided",
    "TextureReadTexels", "TextureReadTexelsStrided", "TextureFlushTexels", "TextureInvalidateTexels", "TextureGetMemoryPool",
    "TextureGetMemoryOffset", "TextureGetStorageSize", "TextureCompare", "TextureGetDebugID", "TextureGetRawStorageClass",
    "SamplerBuilderSetDevice", "SamplerBuilderSetDefaults", "SamplerBuilderSetMinMagFilter", "SamplerBuilderSetWrapMode",
    "SamplerBuilderSetLodClamp", "SamplerBuilderSetLodBias", "SamplerBuilderSetCompare", "SamplerBuilderSetBorderColor",
    "SamplerBuilderSetBorderColori", "SamplerBuilderSetBorderColorui", "SamplerBuilderSetMaxAnisotropy",
    "SamplerBuilderSetReductionFilter", "SamplerBuilderSetLodSnap", "SamplerBuilderGetDevice", "SamplerBuilderGetMinMagFilter",
    "SamplerBuilderGetWrapMode", "SamplerBuilderGetLodClamp", "SamplerBuilderGetLodBias", "SamplerBuilderGetCompare",
    "SamplerBuilderGetBorderColor", "SamplerBuilderGetBorderColori", "SamplerBuilderGetBorderColorui",
    "SamplerBuilderGetMaxAnisotropy", "SamplerBuilderGetReductionFilter", "SamplerBuilderGetLodSnap", "SamplerInitialize",
    "SamplerFinalize", "SamplerSetDebugLabel", "SamplerGetMinMagFilter", "SamplerGetWrapMode", "SamplerGetLodClamp",
    "SamplerGetLodBias", "SamplerGetCompare", "SamplerGetBorderColor", "SamplerGetBorderColori", "SamplerGetBorderColorui",
    "SamplerGetMaxAnisotropy", "SamplerGetReductionFilter", "SamplerCompare", "SamplerGetDebugID", "BlendStateSetDefaults",
    "BlendStateSetBlendTarget", "BlendStateSetBlendFunc", "BlendStateSetBlendEquation", "BlendStateSetAdvancedMode",
    "BlendStateSetAdvancedOverlap", "BlendStateSetAdvancedPremultipliedSrc", "BlendStateSetAdvancedNormalizedDst",
    "BlendStateGetBlendTarget", "BlendStateGetBlendFunc", "BlendStateGetBlendEquation", "BlendStateGetAdvancedMode",
    "BlendStateGetAdvancedOverlap", "BlendStateGetAdvancedPremultipliedSrc", "BlendStateGetAdvancedNormalizedDst",
    "ColorStateSetDefaults", "ColorStateSetBlendEnable", "ColorStateSetLogicOp", "ColorStateSetAlphaTest",
    "ColorStateGetBlendEnable", "ColorStateGetLogicOp", "ColorStateGetAlphaTest", "ChannelMaskStateSetDefaults",
    "ChannelMaskStateSetChannelMask", "ChannelMaskStateGetChannelMask", "MultisampleStateSetDefaults",
    "MultisampleStateSetMultisampleEnable", "MultisampleStateSetSamples", "MultisampleStateSetAlphaToCoverageEnable",
    "MultisampleStateSetAlphaToCoverageDither", "MultisampleStateGetMultisampleEnable", "MultisampleStateGetSamples",
    "MultisampleStateGetAlphaToCoverageEnable", "MultisampleStateGetAlphaToCoverageDither", "MultisampleStateSetRasterSamples",
    "MultisampleStateGetRasterSamples", "MultisampleStateSetCoverageModulationMode",
    "MultisampleStateGetCoverageModulationMode", "MultisampleStateSetCoverageToColorEnable",
    "MultisampleStateGetCoverageToColorEnable", "MultisampleStateSetCoverageToColorOutput",
    "MultisampleStateGetCoverageToColorOutput", "MultisampleStateSetSampleLocationsEnable",
    "MultisampleStateGetSampleLocationsEnable", "MultisampleStateGetSampleLocationsGrid",
    "MultisampleStateSetSampleLocationsGridEnable", "MultisampleStateGetSampleLocationsGridEnable",
    "MultisampleStateSetSampleLocations", "PolygonStateSetDefaults", "PolygonStateSetCullFace", "PolygonStateSetFrontFace",
    "PolygonStateSetPolygonMode", "PolygonStateSetPolygonOffsetEnables", "PolygonStateGetCullFace", "PolygonStateGetFrontFace",
    "PolygonStateGetPolygonMode", "PolygonStateGetPolygonOffsetEnables", "DepthStencilStateSetDefaults",
    "DepthStencilStateSetDepthTestEnable", "DepthStencilStateSetDepthWriteEnable", "DepthStencilStateSetDepthFunc",
    "DepthStencilStateSetStencilTestEnable", "DepthStencilStateSetStencilFunc", "DepthStencilStateSetStencilOp",
    "DepthStencilStateGetDepthTestEnable", "DepthStencilStateGetDepthWriteEnable", "DepthStencilStateGetDepthFunc",
    "DepthStencilStateGetStencilTestEnable", "DepthStencilStateGetStencilFunc", "DepthStencilStateGetStencilOp",
    "VertexAttribStateSetDefaults", "VertexAttribStateSetFormat", "VertexAttribStateSetStreamIndex",
    "VertexAttribStateGetFormat", "VertexAttribStateGetStreamIndex", "VertexStreamStateSetDefaults",
    "VertexStreamStateSetStride", "VertexStreamStateSetDivisor", "VertexStreamStateGetStride", "VertexStreamStateGetDivisor",
    "CommandBufferInitialize", "CommandBufferFinalize", "CommandBufferSetDebugLabel", "CommandBufferSetMemoryCallback",
    "CommandBufferSetMemoryCallbackData", "CommandBufferSetCommandMemoryCallbackEnabled", "CommandBufferAddCommandMemory",
    "CommandBufferAddControlMemory", "CommandBufferGetCommandMemorySize", "CommandBufferGetCommandMemoryUsed",
    "CommandBufferGetCommandMemoryFree", "CommandBufferGetControlMemorySize", "CommandBufferGetControlMemoryUsed",
    "CommandBufferGetControlMemoryFree", "CommandBufferBeginRecording", "CommandBufferEndRecording",
    "CommandBufferCallCommands", "CommandBufferCopyCommands", "CommandBufferBindBlendState",
    "CommandBufferBindChannelMaskState", "CommandBufferBindColorState", "CommandBufferBindMultisampleState",
    "CommandBufferBindPolygonState", "CommandBufferBindDepthStencilState", "CommandBufferBindVertexAttribState",
    "CommandBufferBindVertexStreamState", "CommandBufferBindProgram", "CommandBufferBindVertexBuffer",
    "CommandBufferBindVertexBuffers", "CommandBufferBindUniformBuffer", "CommandBufferBindUniformBuffers",
    "CommandBufferBindTransformFeedbackBuffer", "CommandBufferBindTransformFeedbackBuffers", "CommandBufferBindStorageBuffer",
    "CommandBufferBindStorageBuffers", "CommandBufferBindTexture", "CommandBufferBindTextures", "CommandBufferBindImage",
    "CommandBufferBindImages", "CommandBufferSetPatchSize", "CommandBufferSetInnerTessellationLevels",
    "CommandBufferSetOuterTessellationLevels", "CommandBufferSetPrimitiveRestart", "CommandBufferBeginTransformFeedback",
    "CommandBufferEndTransformFeedback", "CommandBufferPauseTransformFeedback", "CommandBufferResumeTransformFeedback",
    "CommandBufferDrawTransformFeedback", "CommandBufferDrawArrays", "CommandBufferDrawElements",
    "CommandBufferDrawElementsBaseVertex", "CommandBufferDrawArraysInstanced", "CommandBufferDrawElementsInstanced",
    "CommandBufferDrawArraysIndirect", "CommandBufferDrawElementsIndirect", "CommandBufferMultiDrawArraysIndirectCount",
    "CommandBufferMultiDrawElementsIndirectCount", "CommandBufferClearColor", "CommandBufferClearColori",
    "CommandBufferClearColorui", "CommandBufferClearDepthStencil", "CommandBufferDispatchCompute",
    "CommandBufferDispatchComputeIndirect", "CommandBufferSetViewport", "CommandBufferSetViewports",
    "CommandBufferSetViewportSwizzles", "CommandBufferSetScissor", "CommandBufferSetScissors", "CommandBufferSetDepthRange",
    "CommandBufferSetDepthBounds", "CommandBufferSetDepthRanges", "CommandBufferSetTiledCacheAction",
    "CommandBufferSetTiledCacheTileSize", "CommandBufferBindSeparateTexture", "CommandBufferBindSeparateSampler",
    "CommandBufferBindSeparateTextures", "CommandBufferBindSeparateSamplers", "CommandBufferSetStencilValueMask",
    "CommandBufferSetStencilMask", "CommandBufferSetStencilRef", "CommandBufferSetBlendColor", "CommandBufferSetPointSize",
    "CommandBufferSetLineWidth", "CommandBufferSetPolygonOffsetClamp", "CommandBufferSetAlphaRef", "CommandBufferSetSampleMask",
    "CommandBufferSetRasterizerDiscard", "CommandBufferSetDepthClamp", "CommandBufferSetConservativeRasterEnable",
    "CommandBufferSetConservativeRasterDilate", "CommandBufferSetSubpixelPrecisionBias", "CommandBufferCopyBufferToTexture",
    "CommandBufferCopyTextureToBuffer", "CommandBufferCopyTextureToTexture", "CommandBufferCopyBufferToBuffer",
    "CommandBufferClearBuffer", "CommandBufferClearTexture", "CommandBufferClearTexturei", "CommandBufferClearTextureui",
    "CommandBufferUpdateUniformBuffer", "CommandBufferReportCounter", "CommandBufferResetCounter", "CommandBufferReportValue",
    "CommandBufferSetRenderEnable", "CommandBufferSetRenderEnableConditional", "CommandBufferSetRenderTargets",
    "CommandBufferDiscardColor", "CommandBufferDiscardDepthStencil", "CommandBufferDownsample", "CommandBufferTiledDownsample",
    "CommandBufferDownsampleTextureView", "CommandBufferTiledDownsampleTextureView", "CommandBufferBarrier",
    "CommandBufferWaitSync", "CommandBufferFenceSync", "CommandBufferSetTexturePool", "CommandBufferSetSamplerPool",
    "CommandBufferSetShaderScratchMemory", "CommandBufferSaveZCullData", "CommandBufferRestoreZCullData",
    "CommandBufferSetCopyRowStride", "CommandBufferSetCopyImageStride", "CommandBufferGetCopyRowStride",
    "CommandBufferGetCopyImageStride", "CommandBufferDrawTexture", "ProgramSetSubroutineLinkage",
    "CommandBufferSetProgramSubroutines", "CommandBufferBindCoverageModulationTable", "CommandBufferResolveDepthBuffer",
    "CommandBufferSetColorReductionEnable", "CommandBufferSetColorReductionThresholds", "CommandBufferPushDebugGroupStatic",
    "CommandBufferPushDebugGroupDynamic", "CommandBufferPushDebugGroup", "CommandBufferPopDebugGroup",
    "CommandBufferPopDebugGroupId", "CommandBufferInsertDebugMarkerStatic", "CommandBufferInsertDebugMarkerDynamic",
    "CommandBufferInsertDebugMarker", "CommandBufferGetMemoryCallback", "CommandBufferGetMemoryCallbackData",
    "CommandBufferIsRecording", "SyncInitialize", "SyncFinalize", "SyncSetDebugLabel", "QueueFenceSync", "SyncWait",
    "QueueWaitSync", "SyncInitializeFromFencedGLSync", "SyncCreateGLSync", "EventBuilderSetDefaults", "EventBuilderSetStorage",
    "EventBuilderGetStorage", "EventBuilderGetMemoryPool", "EventBuilderGetMemoryOffset", "EventInitialize", "EventFinalize",
    "EventGetValue", "EventSignal", "EventGetMemoryPool", "EventGetMemoryOffset", "CommandBufferWaitEvent",
    "CommandBufferSignalEvent",
};
_Static_assert(sizeof k_nv_name / sizeof k_nv_name[0] == NV_SLOTS, "515 names");

// ---- switch ----
static int g_mask = -1;
unsigned dm_vkn_nonvn_mask(void) {
    int m = __atomic_load_n(&g_mask, __ATOMIC_ACQUIRE);
    if (m >= 0) return (unsigned)m;
    const char *e = getenv("DM_VK_NATIVE"), *s = getenv("DM_VK_NATIVE_NONVN");
    if (!e || atoi(e) != VKN_NATIVE) m = 0;
    else if (!s || !*s || !strcmp(s, "1")) m = 7;
    else m = (int)(strtoul(s, NULL, 0) & 7u);
    __atomic_store_n(&g_mask, m, __ATOMIC_RELEASE);
    return (unsigned)m;
}

// ---- A: the counting forwarders (one 16-byte entry per slot: bti c, x17 = the slot, a branch to the common tail; the tail adds 1 to
// the slot's counter, keeps the caller's x30 and branches to the old entry with every argument register untouched) ----
uint64_t dm_vkn_nv_fw_cnt[NV_SLOTS] __attribute__((visibility("hidden")));
uint64_t dm_vkn_nv_fw_ra[NV_SLOTS] __attribute__((visibility("hidden")));
uint64_t dm_vkn_nv_fw_to[NV_SLOTS] __attribute__((visibility("hidden")));
#if defined(__aarch64__) && defined(__ELF__)
void dm_vkn_nv_fw_stubs(void) __attribute__((visibility("hidden")));
__asm__(
    "    .text\n"
    "    .p2align 4\n"
    "    .globl dm_vkn_nv_fw_stubs\n"
    "    .hidden dm_vkn_nv_fw_stubs\n"
    "    .type dm_vkn_nv_fw_stubs, %function\n"
    "dm_vkn_nv_fw_stubs:\n"
    "    .set dm_vkn_nv_i, 0\n"
    "    .rept 515\n"
    "    hint #34\n"                          // bti c (the game enters with blr)
    "    mov x17, #dm_vkn_nv_i\n"
    "    b dm_vkn_nv_fw_tail\n"
    "    nop\n"
    "    .set dm_vkn_nv_i, dm_vkn_nv_i + 1\n"
    "    .endr\n"
    "dm_vkn_nv_fw_tail:\n"
    "    adrp x16, dm_vkn_nv_fw_cnt\n"
    "    add x16, x16, :lo12:dm_vkn_nv_fw_cnt\n"
    "    add x16, x16, x17, lsl #3\n"
    "1:  ldxr x9, [x16]\n"                    // (no LSE: ARMv8.0 cores too)
    "    add x9, x9, #1\n"
    "    stxr w10, x9, [x16]\n"
    "    cbnz w10, 1b\n"
    "    adrp x16, dm_vkn_nv_fw_ra\n"
    "    add x16, x16, :lo12:dm_vkn_nv_fw_ra\n"
    "    str x30, [x16, x17, lsl #3]\n"
    "    adrp x16, dm_vkn_nv_fw_to\n"
    "    add x16, x16, :lo12:dm_vkn_nv_fw_to\n"
    "    ldr x16, [x16, x17, lsl #3]\n"
    "    br x16\n"
    "    .size dm_vkn_nv_fw_stubs, . - dm_vkn_nv_fw_stubs\n");
static uint64_t fw_stub(unsigned i) { return (uint64_t)(uintptr_t)dm_vkn_nv_fw_stubs + 16ull * i; }
#else
static uint64_t fw_stub(unsigned i) { (void)i; return 0; }   // (no forwarders on this target: nothing is wrapped)
#endif

// ======== B. The device start without NVN ========
// Engine facts (device init 0x938330; disassembly of 0x9384f0..0x938590 and of 0x934d70):
//   0x938504  bootstrap("nvnDeviceGetProcAddress") -> main+0x2bbfeb8 (stored at 0x93850c, read back at 0x93851c only)
//   0x938518  bootstrap("nvnDeviceInitialize")    -> slot 3 (main+0x2bbeeb0)
//   0x934d70(0, gpa): first table fill, a straight run of `x0 = gpa(device, name); str x0, [table + 8i]` for the 515 names
//   0x938534  slot 0  DeviceBuilderSetDefaults(&builder)       (builder on 0x938330's stack)
//   0x938548  slot 3  DeviceInitialize(dev + 8, &builder)       (the result is not tested)
//   0x934d70(dev + 8, gpa): second fill (the same 515 names)
//   0x938570  slot 16 DeviceSetDepthMode(dev + 8, 1), 0x938588 slot 15 DeviceSetWindowOriginMode(dev + 8, 1)
//   then 0x932260 (pools: the creation part's pre-hook), the remaining creation entries, the contexts 0x939850 (QueueFenceSync of
//   ctx+0xd8 at 0x9399dc: the command part's slot, FORWARD for creation callers -> nv_fence here).
// The NVN device object dev+8 (opaque to the game: nothing of main reads it; the creation stubs take the pointer and ignore it,
// builders store it) gets HLE NVN's dev_t_ in place (magic, flags, origin, depth mode), so it reads the same as before.
// The vi / native-window part of device init (display dev+0x3008, layer dev+0x3010, native window dev+0x3018 through the nn::vi
// imports 0x1bf66b0..0x1bf66e0, the graphics allocator 0x1bf66f0 + 16 MB at 0x1bf6700) is OS HLE, not NVN, and stays: dev+0x3018 is
// also what the creation part's WindowBuilderSetNativeWindow stores, but those imports are the same ones every Switch title's
// nn::vi start makes (OS-level, no NVN state), so they are not stubbed here.
// Every function below is entered straight from guest code on the guest stack (16 KiB at the least): each is a few stores; nothing
// logs or calls Vulkan here (unknown names are kept for the census, which logs on the render thread's host stack).
static uint64_t g_fn[NV_SLOTS];                    // B: what slot i gets (both fills, and the bootstrap for any other name)
static int g_b;                                    // B in effect (decided at registration, install time)
static int g_d;                                    // D in effect (section D)
static const char *g_b_why = "not decided";
static uint64_t g_boot_n, g_gpa_n, g_gpa_unknown, g_fence_n, g_acquire_n;
static const char *g_unknown_name[8];
static uint32_t g_dev_origin = ~0u, g_dev_depth = ~0u;
static uint16_t g_hash[2048];                      // name hash -> slot + 1 (open addressing)
static uint32_t nv_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static int slot_of(const char *name) {   // "nvn<Name>" -> its slot, -1 = not a name of the game's table
    if (!name || name[0] != 'n' || name[1] != 'v' || name[2] != 'n') return -1;
    for (uint32_t h = nv_hash(name + 3) & 2047u, n = 0; n < 2048; n++, h = (h + 1) & 2047u) {
        const unsigned v = g_hash[h];
        if (!v) return -1;
        if (!strcmp(k_nv_name[v - 1], name + 3)) return (int)v - 1;
    }
    return -1;
}

// "Unexpected NVN entry" stubs: 515 + 1 (a name outside the table) entries, the same shape as the forwarders; the tail counts, keeps
// the caller and returns 0 in x0 and d0 (void / FALSE / NULL / 0.0).
uint64_t dm_vkn_nv_un_cnt[NV_SLOTS + 1] __attribute__((visibility("hidden")));
uint64_t dm_vkn_nv_un_ra[NV_SLOTS + 1] __attribute__((visibility("hidden")));
#if defined(__aarch64__) && defined(__ELF__)
void dm_vkn_nv_un_stubs(void) __attribute__((visibility("hidden")));
__asm__(
    "    .text\n"
    "    .p2align 4\n"
    "    .globl dm_vkn_nv_un_stubs\n"
    "    .hidden dm_vkn_nv_un_stubs\n"
    "    .type dm_vkn_nv_un_stubs, %function\n"
    "dm_vkn_nv_un_stubs:\n"
    "    .set dm_vkn_nv_u, 0\n"
    "    .rept 516\n"
    "    hint #34\n"                          // bti c
    "    mov x17, #dm_vkn_nv_u\n"
    "    b dm_vkn_nv_un_tail\n"
    "    nop\n"
    "    .set dm_vkn_nv_u, dm_vkn_nv_u + 1\n"
    "    .endr\n"
    "dm_vkn_nv_un_tail:\n"
    "    adrp x16, dm_vkn_nv_un_cnt\n"
    "    add x16, x16, :lo12:dm_vkn_nv_un_cnt\n"
    "    add x16, x16, x17, lsl #3\n"
    "1:  ldxr x9, [x16]\n"
    "    add x9, x9, #1\n"
    "    stxr w10, x9, [x16]\n"
    "    cbnz w10, 1b\n"
    "    adrp x16, dm_vkn_nv_un_ra\n"
    "    add x16, x16, :lo12:dm_vkn_nv_un_ra\n"
    "    str x30, [x16, x17, lsl #3]\n"
    "    mov x0, xzr\n"
    "    movi d0, #0\n"
    "    ret\n"
    "    .size dm_vkn_nv_un_stubs, . - dm_vkn_nv_un_stubs\n");
static uint64_t un_stub(unsigned i) { return (uint64_t)(uintptr_t)dm_vkn_nv_un_stubs + 16ull * i; }
#else
static uint64_t un_stub(unsigned i) { (void)i; return 0; }   // (B needs the stubs: it stays off on this target)
#endif

// The four device slots the game calls once each: HLE NVN's bodies without their log lines.
static void nv_dev_defaults(NVNdeviceBuilder *b) { memset(b, 0, sizeof(dev_builder_t)); }
static NVNboolean nv_dev_init(NVNdevice *d, const NVNdeviceBuilder *b) {
    dev_t_ *o = (dev_t_ *)d; memset(o, 0, sizeof *o);
    o->magic = MAGIC('D', 'E', 'V', 0); o->flags = b ? ((const dev_builder_t *)b)->flags : 0;
    return 1;
}
static void nv_dev_origin(NVNdevice *d, NVNwindowOriginMode m) { ((dev_t_ *)d)->origin = (uint32_t)m; g_dev_origin = (uint32_t)m; }
static void nv_dev_depth(NVNdevice *d, NVNdepthMode m) { ((dev_t_ *)d)->depth_mode = (uint32_t)m; g_dev_depth = (uint32_t)m; }
static PFNNVNGENERICFUNCPTRPROC nv_gpa(const NVNdevice *d, const char *name) {
    (void)d; __atomic_add_fetch(&g_gpa_n, 1, __ATOMIC_RELAXED);
    const int i = slot_of(name);
    if (i >= 0) return (PFNNVNGENERICFUNCPTRPROC)(uintptr_t)g_fn[i];
    const uint64_t k = __atomic_fetch_add(&g_gpa_unknown, 1, __ATOMIC_RELAXED);
    if (k < 8) g_unknown_name[k] = name;   // (a string of main's rodata: kept by pointer)
    return (PFNNVNGENERICFUNCPTRPROC)(uintptr_t)un_stub(NV_SLOTS);
}
// The command part's FORWARD targets (it calls them on the host stack). QueueFenceSync from 0x939850 (ctx+0xd8, three contexts):
// HLE NVN wrote {fenced = 1, fence = x_fence() = the translator's submissions so far = 0 at device init}; the sync is waited only by
// SyncWait in EndFrame(win, 0) and the screenshot branch of 0xa66bc0, both answered by the command part ("satisfied") in mode 1.
static void nv_fence(NVNqueue *q, NVNsync *s, NVNsyncCondition c, int f) {
    (void)q; (void)c; (void)f; __atomic_add_fetch(&g_fence_n, 1, __ATOMIC_RELAXED);
    if (s) { ((sync_t *)s)->fenced = 1; ((sync_t *)s)->fence = 0; }
}
// WindowAcquireTexture: only 0x9215f0 calls it, and the frame part replaces 0x9215f0 (index from its own pacing): unreachable.
static NVNwindowAcquireTextureResult nv_acquire(NVNwindow *w, NVNsync *s, int *idx) {
    (void)w; (void)s; __atomic_add_fetch(&g_acquire_n, 1, __ATOMIC_RELAXED);
    if (idx) *idx = 0;
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
static const NVNT_nvnDeviceBuilderSetDefaults k_chk0 __attribute__((unused)) = nv_dev_defaults;
static const NVNT_nvnDeviceInitialize k_chk3 __attribute__((unused)) = nv_dev_init;
static const NVNT_nvnDeviceSetWindowOriginMode k_chk15 __attribute__((unused)) = nv_dev_origin;
static const NVNT_nvnDeviceSetDepthMode k_chk16 __attribute__((unused)) = nv_dev_depth;
static const NVNT_nvnDeviceGetProcAddress k_chk6 __attribute__((unused)) = nv_gpa;
static const NVNT_nvnQueueFenceSync k_chk497 __attribute__((unused)) = nv_fence;
static const NVNT_nvnWindowAcquireTexture k_chk77 __attribute__((unused)) = nv_acquire;
enum { S_DEV_DEFAULTS = 0, S_DEV_INIT = 3, S_GPA = 6, S_DEV_ORIGIN = 15, S_DEV_DEPTH = 16, S_ACQUIRE = 77, S_FENCE = 497 };

// hle_nvn.c h_nvnBootstrapLoader (the game's only NVN import; callers 0x938504 / 0x938518): the backend's answer, NULL = HLE NVN.
void *dm_vkn_nonvn_boot(const char *name) {
    if (!g_b) return NULL;
    __atomic_add_fetch(&g_boot_n, 1, __ATOMIC_RELAXED);
    const int i = slot_of(name);
    if (i >= 0) return (void *)(uintptr_t)g_fn[i];   // ("nvnDeviceGetProcAddress" -> nv_gpa, "nvnDeviceInitialize" -> nv_dev_init)
    const uint64_t k = __atomic_fetch_add(&g_gpa_unknown, 1, __ATOMIC_RELAXED);
    if (k < 8) g_unknown_name[k] = name;
    return (void *)(uintptr_t)un_stub(NV_SLOTS);
}

// Builds the slot table (registration, install time, after the command / creation / read-back parts made their stubs).
static int b_build(void) {
    if (!un_stub(0)) { g_b_why = "no stub code on this target"; return 0; }
    const char *ce = getenv("DM_VK_NATIVE_CREATE");
    if (ce && *ce && strcmp(ce, "1")) { g_b_why = "DM_VK_NATIVE_CREATE is not 1 (B needs every creation group)"; return 0; }
    if (!dm_vkn_cmd_slot || !dm_vkn_create_slot || !dm_vkn_cmd_set_forward) { g_b_why = "the command or creation part is not built"; return 0; }
    memset(g_hash, 0, sizeof g_hash);
    for (unsigned i = 0; i < NV_SLOTS; i++) {
        uint32_t h = nv_hash(k_nv_name[i]) & 2047u;
        while (g_hash[h]) h = (h + 1) & 2047u;
        g_hash[h] = (uint16_t)(i + 1);
    }
    int ncmd = 0, ncr = 0, nrd = 0, nun = 0;
    for (unsigned i = 0; i < NV_SLOTS; i++) {
        void *f = NULL;
        switch (i) {
        case S_DEV_DEFAULTS: g_fn[i] = (uint64_t)(uintptr_t)nv_dev_defaults; continue;
        case S_DEV_INIT: g_fn[i] = (uint64_t)(uintptr_t)nv_dev_init; continue;
        case S_GPA: g_fn[i] = (uint64_t)(uintptr_t)nv_gpa; continue;
        case S_DEV_ORIGIN: g_fn[i] = (uint64_t)(uintptr_t)nv_dev_origin; continue;
        case S_DEV_DEPTH: g_fn[i] = (uint64_t)(uintptr_t)nv_dev_depth; continue;
        default: break;
        }
        if (dm_vkn_cmd_slot(i, &f) && f) { g_fn[i] = (uint64_t)(uintptr_t)f; ncmd++; continue; }
        f = NULL; if (dm_vkn_read_slot && dm_vkn_read_slot(i, &f) && f) { g_fn[i] = (uint64_t)(uintptr_t)f; nrd++; continue; }
        f = NULL; if (dm_vkn_create_slot(i, &f) && f) { g_fn[i] = (uint64_t)(uintptr_t)f; ncr++; continue; }
        g_fn[i] = un_stub(i); nun++;
    }
    // The parts' lists as built 20.11 (hle_dos2_vkn_cmd.c CMD_SLOTS 132, hle_dos2_vkn_create.c CR_SLOTS 123 with ReadTexels, which the
    // read-back part takes when it is on): fewer means a part without its stubs (no stub page) - those slots would be "unexpected".
    if (ncmd < 132 || ncr + nrd < 123) { g_b_why = "a part lacks stubs (command < 132 or creation < 123 slots)"; return 0; }
    // The command part forwards some calls (creation callers, host code, WindowAcquireTexture) to "the entry the table held": with B
    // that is this part's local answer, never HLE NVN.
    for (unsigned i = 0; i < NV_SLOTS; i++) {
        void *f = NULL;
        if (!dm_vkn_cmd_slot(i, &f) || !f) continue;
        dm_vkn_cmd_set_forward(i, i == S_FENCE ? (uint64_t)(uintptr_t)nv_fence : i == S_ACQUIRE ? (uint64_t)(uintptr_t)nv_acquire : un_stub(i));
    }
    vkn_logf("nonvn: B: the bootstrap and both table fills answered by the backend: %d command, %d creation, %d read-back, 5 device-bootstrap entries, "
             "%d unexpected-entry stubs (counted)", ncmd, ncr, nrd, nun);
    return 1;
}

// ---- which pointer is the backend's for slot i ----
static int is_ours(unsigned i, uint64_t cur) {
    if (g_b && (cur == g_fn[i] || cur == un_stub(i))) return 1;
    void *f = NULL;
    if (dm_vkn_cmd_slot && dm_vkn_cmd_slot(i, &f) && f && (uint64_t)(uintptr_t)f == cur) return 1;
    f = NULL; if (dm_vkn_read_slot && dm_vkn_read_slot(i, &f) && f && (uint64_t)(uintptr_t)f == cur) return 1;
    f = NULL; if (dm_vkn_create_slot && dm_vkn_create_slot(i, &f) && f && (uint64_t)(uintptr_t)f == cur) return 1;
    if (dm_vkn_glue_is_mine && dm_vkn_glue_is_mine(cur)) return 1;
    return 0;
}

// ---- state ----
static struct { int on, after_init, native, wrapped, nulls, kept, b_mismatch; uint64_t hle_at_init; } g_nv;
static uint64_t g_fw_prev[NV_SLOTS], g_un_prev[NV_SLOTS + 1];
static uint64_t hle_calls(void) {   // every HLE NVN body entered through its thunk since the process started (any caller)
    uint64_t s = 0;
    if (nvn_slot) for (unsigned i = 0; i < NVN_NFUNC; i++) s += __atomic_load_n(&nvn_slot[i].cnt, __ATOMIC_RELAXED);
    return s;
}
static uint64_t main_off(uint64_t ra) { const uint64_t b = (uint64_t)(uintptr_t)dm_vkn_base(); return ra >= b && ra - b < M_TEXT_END ? ra - b : ~0ull; }

// The end of the command part's repointing (device init returned): every slot the backend does not own is wrapped (A); with B every
// slot must hold what the fills got from nv_gpa (the glue part's wrapper of TextureGetStride aside).
void dm_vkn_nonvn_after_init(void) {
    if (!g_nv.on) return;
    uint64_t *tab = (uint64_t *)(void *)(dm_vkn_base() + M_TABLE);
    int native = 0, wrapped = 0, nulls = 0, kept = 0, mism = 0; char ml[200]; int k = 0; ml[0] = 0;
    for (unsigned i = 0; i < NV_SLOTS; i++) {
        const uint64_t cur = __atomic_load_n(&tab[i], __ATOMIC_ACQUIRE);
        if (g_b && cur != g_fn[i] && !(dm_vkn_glue_is_mine && dm_vkn_glue_is_mine(cur))) {
            mism++;
            if (k < (int)sizeof ml - 48) k += snprintf(ml + k, sizeof ml - (size_t)k, " %u (%s)", i, k_nv_name[i]);
        }
        if (!cur) { nulls++; continue; }
        if (is_ours(i, cur)) { native++; continue; }
        if (!fw_stub(i)) { kept++; continue; }
        if (cur == fw_stub(i)) { wrapped++; continue; }   // (a second repointing: in already)
        __atomic_store_n(&dm_vkn_nv_fw_to[i], cur, __ATOMIC_RELEASE);
        __atomic_store_n(&tab[i], fw_stub(i), __ATOMIC_RELEASE);
        wrapped++;
    }
    g_nv.after_init = 1; g_nv.native = native; g_nv.wrapped = wrapped; g_nv.nulls = nulls; g_nv.kept = kept; g_nv.b_mismatch = mism; g_nv.hle_at_init = hle_calls();
    vkn_logf("nonvn: game NVN table after device init: %d of %d entries native, %d wrapped in a counting forwarder (HLE NVN behind it), %d null%s; "
             "HLE NVN bodies entered so far %llu", native, NV_SLOTS, wrapped, nulls, kept ? ", the rest kept (no forwarders on this target)" : "",
             (unsigned long long)g_nv.hle_at_init);
    if (g_b) {
        vkn_logf("nonvn: B: bootstrap answered %llu times, GetProcAddress %llu (names outside the table %llu), device origin mode %d / depth mode %d (1 / 1 = "
                 "Vulkan's conventions, VK-NATIVE.md section 0), context fences answered here %llu", (unsigned long long)g_boot_n, (unsigned long long)g_gpa_n,
                 (unsigned long long)g_gpa_unknown, (int)g_dev_origin, (int)g_dev_depth, (unsigned long long)g_fence_n);
        if (mism) vkn_logf(NV_RED "nonvn: B: %d slots of the game's table do not hold what GetProcAddress gave them:%s", mism, ml);
        // (the game asks for names outside its 515-slot table too: nvnCommandBufferSetStencilCullCriteria, once a fill - phone run V5_a:
        // 1032 = 2 x 515 + 2; they get the counting stub and are never called)
        if (g_gpa_n != 2 * NV_SLOTS + g_gpa_unknown) vkn_logf(NV_RED "nonvn: B: GetProcAddress was called %llu times, not 2 x %d (0x934d70 twice): the device start went another way",
                                              (unsigned long long)g_gpa_n, NV_SLOTS);
        if (g_dev_origin != 1 || g_dev_depth != 1) vkn_logf(NV_RED "nonvn: B: the device modes are not (1, 1): origin %d depth %d", (int)g_dev_origin, (int)g_dev_depth);
    }
}

// ---- census (every 10 s): the game's calls that went through a forwarder to HLE NVN; with B the unexpected entries ----
static uint64_t fw_total(void) { uint64_t t = 0; for (unsigned i = 0; i < NV_SLOTS; i++) t += __atomic_load_n(&dm_vkn_nv_fw_cnt[i], __ATOMIC_RELAXED); return t; }
static uint64_t un_total(void) { uint64_t t = 0; for (unsigned i = 0; i <= NV_SLOTS; i++) t += __atomic_load_n(&dm_vkn_nv_un_cnt[i], __ATOMIC_RELAXED); return t; }
static int by_entry(const uint64_t *cnt, const uint64_t *ras, uint64_t *prev, unsigned n, uint64_t *tot, uint64_t *win, char *by, int cap) {
    int k = 0; by[0] = 0; *tot = *win = 0;
    for (unsigned i = 0; i < n; i++) {
        const uint64_t c = __atomic_load_n(&cnt[i], __ATOMIC_RELAXED), d = c - prev[i];
        prev[i] = c; *tot += c; *win += d;
        if (c && k < cap - 112) {
            const uint64_t ra = __atomic_load_n(&ras[i], __ATOMIC_RELAXED), o = main_off(ra);
            k += snprintf(by + k, (size_t)(cap - k), "%s%s x %llu (last from %s%#llx)", k ? ", " : "", i < NV_SLOTS ? k_nv_name[i] : "(a name outside the table)",
                          (unsigned long long)c, o != ~0ull ? "main+" : "host ", (unsigned long long)(o != ~0ull ? o : ra));
        }
    }
    return k;
}
// ======== E. The boot line (the first census after device init: the first presented frames are behind it) ========
// `[vkn] no-NVN path: game table 515 of 515 entries native, HLE NVN calls since start N, translator thread not started, ...` and one
// red line naming every item that is not as it should be in a full no-NVN run (expected red items when a step is switched off).
unsigned xbg_vkn_lib_report(unsigned *warm) __attribute__((weak));   // xbg_game.cpp (VK-NATIVE export block; only with DM_VK_NATIVE_SHLIB=0)
// The backend's shader library (dm/gfx/vkn/vkn_shlib.h): with DM_VK_NATIVE_SHLIB on (mode 1's default) the translator is not initialised at
// all and every extern "C" entry point of xbg_game.cpp reports a call to it (XBG_VKN_ENTERED): the boot line names them, red.
int vkn_shlib_on(void) __attribute__((weak));
int vkn_shlib_report(char *out, size_t bytes) __attribute__((weak));
uint64_t vkn_shlib_translator_calls(uint64_t out[]) __attribute__((weak));
const char *vkn_shlib_translator_entry_name(int which) __attribute__((weak));
enum { NV_XN = 10 };   // VKN_SHLIB_X_N
static int shlib_on(void) { return vkn_shlib_on && vkn_shlib_on() && vkn_shlib_translator_calls; }
static int translator_calls(char *by, int cap, uint64_t *tot) {   // "name x N, ..." of the entries reached; returns the length
    uint64_t n[NV_XN] = {0}; *tot = vkn_shlib_translator_calls(n); int k = 0; by[0] = 0;
    for (int i = 0; i < NV_XN; i++) if (n[i] && k < cap - 96) k += snprintf(by + k, (size_t)(cap - k), "%s%s x %llu", k ? ", " : "", vkn_shlib_translator_entry_name ? vkn_shlib_translator_entry_name(i) : "?", (unsigned long long)n[i]);
    return k;
}
static int threads_named(const char *want) {   // threads of this process whose name (comm) is `want`; -1 = /proc unreadable
    DIR *d = opendir("/proc/self/task");
    if (!d) return -1;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[96], name[32] = {0}; snprintf(p, sizeof p, "/proc/self/task/%.24s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) { char *nl = strchr(name, '\n'); if (nl) *nl = 0; if (!strcmp(name, want)) n++; }
        fclose(f);
    }
    closedir(d);
    return n;
}
static void nv_boot_line(void) {
    uint64_t *tab = (uint64_t *)(void *)(dm_vkn_base() + M_TABLE);
    int native = 0;
    for (unsigned i = 0; i < NV_SLOTS; i++) { const uint64_t cur = __atomic_load_n(&tab[i], __ATOMIC_ACQUIRE); if (cur && is_ours(i, cur)) native++; }
    const uint64_t hle = hle_calls();
    const int xt = threads_named("nvn-translate");
    const int shl = shlib_on();
    unsigned warm = 0; const unsigned lib = shl ? 0 : xbg_vkn_lib_report ? xbg_vkn_lib_report(&warm) : 0xffffffffu;   // (no call into xbg_game.cpp with the library)
    char shrep[400] = "", xby[400] = ""; uint64_t xtot = 0;
    if (shl) { if (vkn_shlib_report) vkn_shlib_report(shrep, sizeof shrep); translator_calls(xby, (int)sizeof xby, &xtot); }
    int live = 0, peak = 0, cap = 0; nvn_dirty_stat(&live, &peak, &cap);
    uint64_t mpc = 0, mpp = 0, mpn = 0; nvn_dirty_mp_stats(&mpc, &mpp, &mpn);
    mtl_res_count rc; memset(&rc, 0, sizeof rc); mtl_res_snapshot(&rc);
    if (shl)
        vkn_logf("no-NVN path: game table %d of %d entries native, HLE NVN calls since start %llu, translator thread %s, translator not initialised "
                 "(DM_VK_NATIVE_SHLIB: its entry points reached %llu times%s%s; %s), dirty-tracked pool ranges %d (peak %d, mprotect calls %llu), pools "
                 "known to the translator %u, frame source %s, device bootstrap %s (DM_VK_NATIVE_NONVN steps %#x)",
                 native, NV_SLOTS, (unsigned long long)hle, xt < 0 ? "unknown (/proc unreadable)" : xt ? "STARTED" : "not started", (unsigned long long)xtot,
                 xtot ? ": " : "", xby, shrep[0] ? shrep : "no shader library report", live, peak, (unsigned long long)mpc, rc.pools,
                 g_d ? "the backend" : "hle_nvn.c", g_b ? "the backend" : "HLE NVN", dm_vkn_nonvn_mask());
    else
        vkn_logf("no-NVN path: game table %d of %d entries native, HLE NVN calls since start %llu, translator thread %s, translator %s (pipeline cache %s, "
                 "warm-up threads %u, pool reset thread %s, gfx worker %s), dirty-tracked pool ranges %d (peak %d, mprotect calls %llu), pools known to the "
                 "translator %u, frame source %s, device bootstrap %s (DM_VK_NATIVE_NONVN steps %#x; DM_VK_NATIVE_SHLIB=0: shaders through the translator)",
                 native, NV_SLOTS, (unsigned long long)hle, xt < 0 ? "unknown (/proc unreadable)" : xt ? "STARTED" : "not started",
                 lib == 0xffffffffu ? "?" : lib & 1 ? "library-only" : "fully initialised", lib != 0xffffffffu && lib & 2 ? "loaded" : "none", warm,
                 lib != 0xffffffffu && lib & 8 ? "on" : "off", lib != 0xffffffffu && lib & 16 ? "on" : "off", live, peak, (unsigned long long)mpc, rc.pools,
                 g_d ? "the backend" : "hle_nvn.c", g_b ? "the backend" : "HLE NVN", dm_vkn_nonvn_mask());
    char bad[400]; int k = 0; bad[0] = 0;
#define NV_BAD(cond, ...) do { if ((cond) && k < (int)sizeof bad - 80) k += snprintf(bad + k, sizeof bad - (size_t)k, __VA_ARGS__); } while (0)
    NV_BAD(native != NV_SLOTS, " table %d of %d native;", native, NV_SLOTS);
    NV_BAD(hle != 0, " %llu HLE NVN calls;", (unsigned long long)hle);
    NV_BAD(xt != 0, " translator thread %s;", xt < 0 ? "unknown" : "started");
    NV_BAD(shl && xtot, " translator entry points reached %llu times (%s);", (unsigned long long)xtot, xby);
    NV_BAD(!shl && (lib == 0xffffffffu || !(lib & 1)), " translator not library-only;");
    NV_BAD(!shl && lib != 0xffffffffu && (lib & 0x1e), " translator runs:%s%s%s%s;", lib & 2 ? " pipeline cache" : "", lib & 4 ? " warm-up threads" : "",
           lib & 8 ? " pool reset" : "", lib & 16 ? " gfx worker" : "");
    NV_BAD(live || mpc, " write tracking armed (%d ranges, %llu mprotect calls);", live, (unsigned long long)mpc);
    NV_BAD(rc.pools != 0, " %u pools registered with the translator;", rc.pools);
    NV_BAD(!g_d, " frame source hle_nvn.c;");
    NV_BAD(!g_b, " device bootstrap through HLE NVN;");
#undef NV_BAD
    if (k) vkn_logf(NV_RED "no-NVN path NOT complete:%s", bad);
}
static void nv_census(void) {
    if (!g_nv.on || !g_nv.after_init) return;
    static int booted;
    if (!booted) { booted = 1; nv_boot_line(); }
    if (shlib_on()) {   // a translator entry point reached after the boot line (DM_VK_NATIVE_SHLIB): one red line a window, with the entries
        static uint64_t seen; char xby[400]; uint64_t xtot = 0; translator_calls(xby, (int)sizeof xby, &xtot);
        if (xtot != seen) { vkn_logf(NV_RED "nonvn: translator entry points reached although it is not initialised: %llu calls since start (%s)", (unsigned long long)xtot, xby); seen = xtot; }
    }
    uint64_t tot, win; char by[640];
    const int k = by_entry(dm_vkn_nv_fw_cnt, dm_vkn_nv_fw_ra, g_fw_prev, NV_SLOTS, &tot, &win, by, (int)sizeof by);
    const uint64_t h = hle_calls();
    vkn_logf("%snvn: %llu game calls reached HLE NVN since start (by entry: %s); %llu in this window; HLE NVN bodies entered %llu since the process "
             "started, %llu after device init", tot ? NV_RED : "", (unsigned long long)tot, k ? by : "none", (unsigned long long)win, (unsigned long long)h,
             (unsigned long long)(h - g_nv.hle_at_init));
    if (!g_b) return;
    const int u = by_entry(dm_vkn_nv_un_cnt, dm_vkn_nv_un_ra, g_un_prev, NV_SLOTS + 1, &tot, &win, by, (int)sizeof by);
    char un[200]; int j = 0; un[0] = 0;
    for (unsigned i = 0; i < 8 && i < g_gpa_unknown; i++) if (g_unknown_name[i] && j < (int)sizeof un - 64) j += snprintf(un + j, sizeof un - (size_t)j, " %.48s", g_unknown_name[i]);
    if (tot)   // (names outside the table that nobody calls are not an error: the boot line counts them)
        vkn_logf(NV_RED "nonvn: B: unexpected NVN entries called %llu (%llu in this window; by entry: %s); names outside the table %llu:%s", (unsigned long long)tot,
                 (unsigned long long)win, u ? by : "none", (unsigned long long)g_gpa_unknown, j ? un : " none");
}

// ======== D. The frame source of the port ========
// What hle_nvn.c dm_nvn_vkn_frame did for the rest of the port at a presented native frame, here: the frame counter (g_frame there:
// dm_nvn_frame_count, read by hle_oe.c dynamic 3D scale, hle_dos2_quality.c DRS, hle_dos2_nr.c windows, diag.c, hle_cpuprof.c rates,
// nvn_leancall.inc), the on-screen frame statistics (fstat_present there: dm_frame_stats -> jni nativeFrameStats: fps over 0.5 s windows,
// mean / max frame interval ms, mean GPU ms = -1: mode 1 never had GPU readings there either) and the deterministic guest clock tick
// (dm_guest_frame_tick, hle_os.c; a no-op without DM_DETCLOCK). Not carried (diagnostics of the HLE layer, empty in mode 1): dump_frame's
// per-slot NVN call diff and its log line every 300 frames (this file's census says it better), cs_dump, @frame_ms of DM_NVN_TSV.
// Counter semantics as g_frame: the number of presented frames so far, +1 after each present (the frame part's EndFrame, render thread).
// (g_d: D in effect, declared with g_b)
static uint64_t g_frames;
static _Atomic float g_fstat[4] = {0, 0, 0, -1};
int dm_vkn_nonvn_frame(void);
int dm_vkn_nonvn_frame_count(uint64_t *out);
int dm_vkn_nonvn_frame_stats(float out[4]);
int dm_vkn_nonvn_frame(void) {
    if (!g_d) return 0;
    static int64_t last, win0; static unsigned n; static double sum, mx;   // (render thread only, as fstat_present)
    const int64_t t = (int64_t)vkn_now_ns();
    if (last) { const double ms = (double)(t - last) / 1e6; sum += ms; if (ms > mx) mx = ms; n++; }
    last = t; if (!win0) win0 = t;
    if (t - win0 >= 500000000LL && n) {
        atomic_store(&g_fstat[0], (float)(n * 1e9 / (double)(t - win0))); atomic_store(&g_fstat[1], (float)(sum / n));
        atomic_store(&g_fstat[2], (float)mx); atomic_store(&g_fstat[3], -1.f);
        win0 = t; n = 0; sum = mx = 0;
    }
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELEASE);
    dm_guest_frame_tick();
    return 1;
}
int dm_vkn_nonvn_frame_count(uint64_t *out) { if (!g_d) return 0; *out = __atomic_load_n(&g_frames, __ATOMIC_ACQUIRE); return 1; }
int dm_vkn_nonvn_frame_stats(float out[4]) { if (!g_d) return 0; for (int i = 0; i < 4; i++) out[i] = atomic_load(&g_fstat[i]); return 1; }

void dm_vkn_nonvn_register(void) {
    if (vkn_mode() != VKN_NATIVE) return;
    g_nv.on = 1;
    const unsigned m = dm_vkn_nonvn_mask();
    g_d = (m & 4) != 0;
    if (m & 1) { g_b = b_build(); if (!g_b) vkn_logf(NV_RED "nonvn: B (device start without NVN) off: %s - HLE NVN answers the bootstrap", g_b_why); }
    dm_vkn_census(nv_census);
    vkn_logf("nonvn: DM_VK_NATIVE_NONVN steps %#x (B device start %s, C translator library-only %s, D frame source %s); census forwarders on", m,
             g_b ? "on" : "off", m & 2 ? "on" : "off", g_d ? "on" : "off");
}
#endif
