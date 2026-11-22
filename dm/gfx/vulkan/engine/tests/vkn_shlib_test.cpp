// [DOS2] VK-NATIVE shader library: the comparison host test (the acceptance test of dm/gfx/vkn/vkn_shlib.cpp). For EVERY bundle of a real
// shader directory (shaders.xgspack via its index, plus the loose <key>.xgs files the pack does not list) it builds the base module and
// the per-draw variants twice, through the OLD loader (the translator itself: xbg_game.cpp compiled into this program, initialised
// "library only" as mode 1 does today with DM_VK_NATIVE_SHLIB=0, entered through its doors xbg_vkn_shader / xbg_vkn_alpha_module /
// xbg_vkn_retype_module) and through the NEW library (vkn_shlib_init / vkn_shlib_shader / _alpha_module / _retype_module), and compares
// bit for bit what each hands to vkCreateShaderModule (this program's mock of it keeps the words of every module it creates), plus the
// rest of xbg_vkn_stage (stage, binding list with the trimmed sizes and NR-08b flags, inputs, input classes, c1 bytes, usable or not)
// and, for each variant, whether it was refused (the base module returned). No device: every Vulkan entry point the two paths reach is
// mocked below from a device model; an entry point neither should reach is left unresolved (address 0: a crash names it).
// Also: the first bundle gets its own SPIR-V plus an OpNop through spv-override/<key>.spv (both loaders list that directory once), and one
// key no bundle has (both must answer "missing", 1).
// Variants: fragment bundles: alpha GREATER with references 0, 0.25, 0.5, GEQUAL 0.5, NEVER; vertex bundles: four retype codes derived
// from the shader's own inputs (first input -> sint, every input -> uint, every input -> float, first input SSCALED->SINT + the second
// -> float).
// One device model per process (the translator initialises once): argv[1] = adreno | mali | powervr | adreno-simmali | adreno-native | adreno-files,
// argv[2] = the shader directory, argv[3] (optional) = the bundle limit (0 = all). Exit 0 = no difference.
// [stage 5 A1] "With replacement" column: a model name may carry "+nr<q>" (q = 0..3: the shadow class; "+nrv": verify; "+nr3nl": the
// shadow atlas formats D16 / X8_D24 without linear filtering, so Dref falls back to mobile on both sides): the translator runs with DM_NR_SH=1 /
// DM_NR_SH_PCF=q, the library with DM_VK_NATIVE_SH=1 / DM_VK_NATIVE_SH_PCF=q (vkn_shlib_named.cpp), both reading <shader dir>/native-nr.xgspack;
// every compare above then covers the named bundles too (the module words with the replacement = the translator's with DM_NR_SH=1), and
// the summary counts the NR-04 keys replaced, those with a Dref binding, and the "#nrt" translated twin of each replaced key (loaded, a
// fragment module whose binding list equals the named one's, words other than the named module's), and [stage 5.1 A5] the pass check of each
// replaced entry (vkn_shlib_named_pass: verified pass kept; another colour count / colour-0 format / sample count -> the translated twin in
// mode 1 unless Dref, counted only in mode 2; unknown pass kept; the counters). Without "+nr" both sides run with
// the replacement off (DM_NR_SH=0 / DM_VK_NATIVE_SH=0: the stage-4 column).
// [stage 5.4 G1] "+nr<q>-fsr<N>" (N 1 / 2 / 3) and / or "-lc<N>" (N 2 / 4): DM_HACK_FSR / DM_HACK_LIGHTS on both sides (the translator's
// xbg_nr_sh.inc hacks, the library's vkn_shlib_named.cpp port): the PPFXAA / ClusteredLightingWithSSAO modules are the built-in FSR /
// light-cap modules behind the player's head, and every compare above covers them (words = the translator's); the summary adds the
// library's hack states (each asked hack swapped - FSR=2: checked, not swapped) and PASS needs them.
// [P10] every model: the input-attachment twins of the merged tile pass (vkn_shlib_named.h vkn_shlib_named_tile; the table and the words:
// dm/android/shaders/tile_twins.inc, tools/dos2_tile_shaders.py). Variant 0 for every NR-04 key: a twin exactly when the library replaced
// the key by name and it is LinearizeDepth / CascadedPCF_* (a translated module: 1); variants f / g / F / G for the Light key: a twin
// exactly when vkn_shlib_named_fused hands that fused module out (the pack's, or with "-lc<N>" the built-in capped one). Each twin: a
// fragment module whose words are the built-in file's after the library's load-time chain (the file's words unchanged, or - where the
// chain rewrote them - still declaring exactly the table row's input attachments), *var = the table row, `declared` = the bindings of
// the file; every fault of vkn_shlib_named_tile_fault refused; wrong variants / keys answer 1. PASS needs all of it.
// Build + run (WSL; tests/vkn_shlib_test.sh does exactly this, and copies the pack into the Linux file system first):
//   g++ -std=c++17 -O1 -w -D_GNU_SOURCE -DDM_GAME_DOOM=1 -DDM_GAME_D3=1 -DDM_GAME_TL2=1 -DDM_GAME_DOS2=1 -DVK_USE_PLATFORM_ANDROID_KHR
//       -DDM_ASTC_LAZY=1 -I<shim: android/hardware_buffer.h, zlib.h> -I<NDK vulkan headers> -Idm/gfx/vulkan -Idm/gfx/nvn/include -Idm/host -Idm
//       dm/gfx/vulkan/engine/*.cpp dm/gfx/vulkan/engine/*.c (not xbg_spvopt.cpp / xbg_astc_lazy.cpp) dm/gfx/vkn/vkn_shlib.cpp dm/gfx/vkn/vkn_shlib_named.cpp
//       dm/gfx/nr/nr_ubodyn.c dm/gfx/nr/nr_sh.c dm/gfx/vulkan/engine/tests/vkn_shlib_test.cpp -no-pie -Wl,--unresolved-symbols=ignore-all -lpthread -ldl -l:libz.so.1
#include <vulkan/vulkan.h>
#include <execinfo.h>
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "../xbg_game.h"
#include "../../../vkn/vkn_shlib.h"
#include "../../../nr/nr_cover.h"   // [P10 fix 5] nr_cv_prog (the position program)
#include "../../../vkn/vkn_shlib_named.h"
#include "../../../nr/nr_sh.h"

// ---------------------------------------------------------------- device models
struct Model {
    const char *name; uint32_t vendor; const char *device; uint32_t api, driver_version; VkDriverId driver_id;
    uint32_t sg_size, sg_stages, sg_ops; VkPhysicalDeviceFeatures feat; bool draw_params; bool ffl2_ext;
    std::vector<std::pair<const char *, const char *>> env;   // set with overwrite (the model's own switches)
};
static Model g_m;
static bool g_d16_nolinear;   // "+nr3nl": D16_UNORM and X8_D24 without SAMPLED_IMAGE_FILTER_LINEAR: both Dref checks refuse it.
// (D16 alone would differ by design: the translator then stores NVN DEPTH16 as X8_D24 and keeps Dref; vkn_tex.c has no such widening.)
static VkPhysicalDeviceFeatures all_features(bool gs, bool gather, bool rwf) {
    VkPhysicalDeviceFeatures f; VkBool32 *b = reinterpret_cast<VkBool32 *>(&f); for (size_t i = 0; i < sizeof f / sizeof(VkBool32); i++) b[i] = VK_TRUE;
    f.geometryShader = gs; f.shaderImageGatherExtended = gather; f.shaderStorageImageReadWithoutFormat = rwf; f.shaderStorageImageWriteWithoutFormat = rwf;
    f.shaderFloat64 = VK_FALSE; f.shaderInt64 = VK_FALSE; f.textureCompressionETC2 = VK_TRUE; return f;
}
static bool pick_model(const char *n) {
    const uint32_t ALL = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, VFC = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    if (!strncmp(n, "adreno", 6)) {   // Turnip on the Mi 14 Ultra (Adreno 750): wave128, every stage, every operation class
        g_m = {"adreno", 0x5143, "Turnip Adreno (TM) 750", VK_API_VERSION_1_3, 0x6000000, VK_DRIVER_ID_MESA_TURNIP, 128, ALL, 0xff, all_features(true, true, true), true, true, {}};
        if (!strcmp(n, "adreno-simmali")) g_m.env.push_back({"DM_GFX_SIM_GPU", "mali"});
        if (!strcmp(n, "adreno-native")) g_m.env.push_back({"DM_GFX_NATIVE_SHADERS", "1"});
        if (!strcmp(n, "adreno-files")) g_m.env.push_back({"DM_GFX_SHADER_PACK", "0"});   // the loose <key>.xgs files only
        if (!strcmp(n, "adreno-tl")) g_m.env.push_back({"DM_VK_NATIVE_TILELIGHT", "1"});   // [P10 fix 5] the merged tile pass on Turnip (LOAD 0 by family)
        return !strcmp(n, "adreno") || !strcmp(n, "adreno-simmali") || !strcmp(n, "adreno-native") || !strcmp(n, "adreno-files") || !strcmp(n, "adreno-tl");
    }
    if (!strcmp(n, "mali")) {   // Immortalis-G720 (Valhall 5th gen): subgroup 16, no formatless storage reads; no GPU self-test on a host
        g_m = {"mali", 0x13B5, "Mali-G720-Immortalis MC12", VK_API_VERSION_1_3, 0x30000, VK_DRIVER_ID_ARM_PROPRIETARY, 16, VFC, 0xbf, all_features(true, true, false), true, true,
               {{"DM_GFX_SELFTEST", "0"}}};
        return true;
    }
    if (!strcmp(n, "powervr")) {   // BXM-8-256 (driver 1.15 class): subgroup 1, Vulkan 1.1 without format_feature_flags2, no gather extended / GS
        g_m = {"powervr", 0x1010, "PowerVR B-Series BXM-8-256", VK_API_VERSION_1_1, 0x1, VK_DRIVER_ID_IMAGINATION_PROPRIETARY, 1, VFC, 0x1, all_features(false, false, false), true, false,
               {{"DM_GFX_SELFTEST", "0"}, {"DM_GFX_LDC_CANON", "1"}, {"DM_GFX_F2I_NOTRUNC", "1"}, {"DM_GFX_NAN_SAT", "1"}}};
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the Vulkan mock (the entry points the two paths reach)
static std::mutex g_mk_mu;
static std::unordered_map<uint64_t, std::vector<uint32_t>> g_mods;   // module handle -> the words vkCreateShaderModule got
static uint64_t g_next_handle = 0x10000;
static uint64_t g_mod_creates, g_mod_destroys;
#define MK extern "C"
MK void vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *p) {
    memset(p, 0, sizeof *p); p->apiVersion = g_m.api; p->driverVersion = g_m.driver_version; p->vendorID = g_m.vendor; p->deviceID = 0x43050a01;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; snprintf(p->deviceName, sizeof p->deviceName, "%s", g_m.device);
    for (int i = 0; i < VK_UUID_SIZE; i++) p->pipelineCacheUUID[i] = uint8_t(i * 7 + g_m.vendor);
    VkPhysicalDeviceLimits &l = p->limits;
    l.maxImageDimension1D = l.maxImageDimension2D = 16384; l.maxImageDimension3D = 2048; l.maxImageDimensionCube = 16384; l.maxImageArrayLayers = 2048;
    l.maxTexelBufferElements = 1u << 27; l.maxUniformBufferRange = 65536; l.maxStorageBufferRange = 1u << 30; l.maxPushConstantsSize = 256;
    l.maxMemoryAllocationCount = 4096; l.maxSamplerAllocationCount = 4000; l.bufferImageGranularity = 1; l.maxBoundDescriptorSets = 8;
    l.maxPerStageDescriptorSamplers = l.maxPerStageDescriptorUniformBuffers = l.maxPerStageDescriptorStorageBuffers = 64;
    l.maxPerStageDescriptorSampledImages = l.maxPerStageDescriptorStorageImages = 64; l.maxPerStageResources = 256;
    l.maxDescriptorSetSamplers = l.maxDescriptorSetUniformBuffers = l.maxDescriptorSetStorageBuffers = 256; l.maxDescriptorSetSampledImages = l.maxDescriptorSetStorageImages = 256;
    l.maxDescriptorSetUniformBuffersDynamic = 16; l.maxDescriptorSetStorageBuffersDynamic = 16; l.maxVertexInputAttributes = 32; l.maxVertexInputBindings = 32;
    l.maxVertexInputAttributeOffset = 2047; l.maxVertexInputBindingStride = 2048; l.maxVertexOutputComponents = 128; l.maxFragmentInputComponents = 128;
    l.maxFragmentOutputAttachments = 8; l.maxColorAttachments = 8; l.maxComputeSharedMemorySize = 32768; for (int i = 0; i < 3; i++) { l.maxComputeWorkGroupCount[i] = 65535; l.maxComputeWorkGroupSize[i] = 1024; }
    l.maxComputeWorkGroupInvocations = 1024; l.maxSamplerLodBias = 15; l.maxSamplerAnisotropy = 16; l.maxViewports = 16; l.maxViewportDimensions[0] = l.maxViewportDimensions[1] = 16384;
    l.viewportBoundsRange[0] = -32768; l.viewportBoundsRange[1] = 32767; l.minMemoryMapAlignment = 64; l.minTexelBufferOffsetAlignment = 64;
    l.minUniformBufferOffsetAlignment = 64; l.minStorageBufferOffsetAlignment = 64; l.maxFramebufferWidth = l.maxFramebufferHeight = 16384; l.maxFramebufferLayers = 2048;
    l.framebufferColorSampleCounts = l.framebufferDepthSampleCounts = l.framebufferStencilSampleCounts = l.framebufferNoAttachmentsSampleCounts = 0x5;
    l.sampledImageColorSampleCounts = l.sampledImageIntegerSampleCounts = l.sampledImageDepthSampleCounts = l.sampledImageStencilSampleCounts = l.storageImageSampleCounts = 0x5;
    l.maxSampleMaskWords = 1; l.timestampComputeAndGraphics = VK_TRUE; l.timestampPeriod = 52.08f; l.maxClipDistances = l.maxCullDistances = 8; l.maxCombinedClipAndCullDistances = 8;
    l.optimalBufferCopyOffsetAlignment = 64; l.optimalBufferCopyRowPitchAlignment = 64; l.nonCoherentAtomSize = 64;
}
MK void vkGetPhysicalDeviceProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2 *p) {
    vkGetPhysicalDeviceProperties(d, &p->properties);
    for (VkBaseOutStructure *s = reinterpret_cast<VkBaseOutStructure *>(p->pNext); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
            auto *g = reinterpret_cast<VkPhysicalDeviceSubgroupProperties *>(s);
            g->subgroupSize = g_m.sg_size; g->supportedStages = g_m.sg_stages; g->supportedOperations = g_m.sg_ops; g->quadOperationsInAllStages = g_m.sg_ops & 0x80 ? VK_TRUE : VK_FALSE;
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            auto *dp = reinterpret_cast<VkPhysicalDeviceDriverProperties *>(s); dp->driverID = g_m.driver_id; snprintf(dp->driverName, sizeof dp->driverName, "mock-%s", g_m.name);
        }
    }
}
MK void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *m) {
    memset(m, 0, sizeof *m); m->memoryTypeCount = 2; m->memoryHeapCount = 1; m->memoryHeaps[0] = {8ull << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    m->memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    m->memoryTypes[1] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0};
}
MK void vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures *f) { *f = g_m.feat; }
MK void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *f) {
    f->features = g_m.feat;
    for (VkBaseOutStructure *s = reinterpret_cast<VkBaseOutStructure *>(f->pNext); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES) reinterpret_cast<VkPhysicalDeviceShaderDrawParametersFeatures *>(s)->shaderDrawParameters = g_m.draw_params;
}
MK void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t *n, VkQueueFamilyProperties *q) {
    if (!q) { *n = 1; return; }
    if (*n >= 1) { q[0] = VkQueueFamilyProperties{VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 1, 64, {1, 1, 1}}; *n = 1; }
}
MK VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *n, VkExtensionProperties *e) {
    static const char *const names[] = {VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME, "VK_KHR_swapchain"};
    const uint32_t have = g_m.ffl2_ext ? 2 : 1, first = g_m.ffl2_ext ? 0 : 1;
    if (!e) { *n = have; return VK_SUCCESS; }
    const uint32_t k = std::min(*n, have);
    for (uint32_t i = 0; i < k; i++) { memset(&e[i], 0, sizeof e[i]); snprintf(e[i].extensionName, sizeof e[i].extensionName, "%s", names[first + i]); e[i].specVersion = 1; }
    *n = k; return k < have ? VK_INCOMPLETE : VK_SUCCESS;
}
MK void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat f, VkFormatProperties *p) {   // everything everywhere (only reports and the Dref checks read it)
    p->linearTilingFeatures = p->optimalTilingFeatures = p->bufferFeatures = 0x7fffffff;
    if (g_d16_nolinear && (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32)) p->optimalTilingFeatures &= ~VkFormatFeatureFlags(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
}
MK void vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice d, VkFormat f, VkFormatProperties2 *p) {
    vkGetPhysicalDeviceFormatProperties(d, f, &p->formatProperties);
    for (VkBaseOutStructure *s = reinterpret_cast<VkBaseOutStructure *>(p->pNext); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {   // VK_KHR_format_feature_flags2: everything, as above
            auto *p3 = reinterpret_cast<VkFormatProperties3 *>(s); p3->linearTilingFeatures = p3->optimalTilingFeatures = p3->bufferFeatures = ~VkFormatFeatureFlags2(0);
            if (g_d16_nolinear && (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32)) p3->optimalTilingFeatures &= ~VkFormatFeatureFlags2(VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        }
}
MK VkResult vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties *p) {
    p->maxExtent = {16384, 16384, 2048}; p->maxMipLevels = 15; p->maxArrayLayers = 2048; p->sampleCounts = 0x5; p->maxResourceSize = 1ull << 32; return VK_SUCCESS;
}
MK VkResult vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *p) {
    std::lock_guard<std::mutex> l(g_mk_mu); *p = reinterpret_cast<VkCommandPool>(g_next_handle++); return VK_SUCCESS;
}
MK void vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *) {}
MK VkResult vkCreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo *, const VkAllocationCallbacks *, VkPipelineCache *p) {
    std::lock_guard<std::mutex> l(g_mk_mu); *p = reinterpret_cast<VkPipelineCache>(g_next_handle++); return VK_SUCCESS;
}
MK PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice, const char *) { return nullptr; }
MK PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance, const char *) { return nullptr; }
MK VkResult vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *, VkShaderModule *m) {
    if (!ci || !ci->pCode || !ci->codeSize || ci->codeSize % 4) { *m = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED; }
    std::lock_guard<std::mutex> l(g_mk_mu);
    const uint64_t h = g_next_handle++; g_mods[h].assign(ci->pCode, ci->pCode + ci->codeSize / 4); g_mod_creates++;
    *m = reinterpret_cast<VkShaderModule>(h); return VK_SUCCESS;
}
MK void vkDestroyShaderModule(VkDevice, VkShaderModule m, const VkAllocationCallbacks *) { std::lock_guard<std::mutex> l(g_mk_mu); g_mod_destroys++; g_mods.erase(reinterpret_cast<uint64_t>(m)); }

// ---------------------------------------------------------------- the backend core the library uses (vkn.h)
static FILE *g_log;
extern "C" void vkn_logf(const char *fmt, ...) {
    va_list a; va_start(a, fmt); if (g_log) { fputs("[vkn] ", g_log); vfprintf(g_log, fmt, a); fputc('\n', g_log); } va_end(a);
}
extern "C" uint64_t vkn_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec); }
// The translator's step C (hle_dos2_vkn_nonvn.c): library-only initialise, as mode 1 with DM_VK_NATIVE_SHLIB=0 runs it today.
extern "C" unsigned dm_vkn_nonvn_mask(void) { return 7; }
static void xlog(void *, const char *m) { if (g_log) fprintf(g_log, "[xbg] %s\n", m); }
static void xfatal(void *, const char *m) { fprintf(stderr, "OLD PATH FATAL: %s\n", m); fflush(stdout); _exit(3); }
static void on_crash(int sig) {
    void *bt[48]; const int n = backtrace(bt, 48); fprintf(stderr, "signal %d (an entry point neither path should reach is unresolved: see the frames)\n", sig);
    backtrace_symbols_fd(bt, n, 2); _exit(4);
}

// ---------------------------------------------------------------- the keys of a directory
static std::vector<std::string> keys_of(const std::string &dir, size_t *in_pack) {
    std::set<std::string> keys; *in_pack = 0;
    if (FILE *f = fopen((dir + "/shaders.xgspack").c_str(), "rb")) {
        uint32_t h[4] = {};
        if (fread(h, 1, 16, f) == 16 && h[0] == 0x50534758u && h[1] == 1u) {
            struct Ent { char key[32]; uint64_t off; uint32_t size, pad; uint64_t hash; } e;
            for (uint32_t i = 0; i < h[2] && fread(&e, sizeof e, 1, f) == 1; i++) { e.key[31] = 0; keys.insert(e.key); }
            *in_pack = keys.size();
        }
        fclose(f);
    }
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *de = readdir(d)) { std::string n = de->d_name; if (n.size() == 20 && !n.compare(16, 4, ".xgs")) keys.insert(n.substr(0, 16)); }
        closedir(d);
    }
    return std::vector<std::string>(keys.begin(), keys.end());
}

// ---------------------------------------------------------------- comparison
struct Count { uint64_t compared = 0, same = 0, differ = 0; };
static Count c_base, c_alpha, c_retype;
static uint64_t g_unusable, g_missing, g_alpha_refused, g_retype_refused, g_vs, g_fs, g_cs;
static uint64_t g_nr_keys, g_nr_replaced, g_nr_same, g_nr_dref, g_nr_alt, g_nr_alt_bad;   // [stage 5 A1] the "with replacement" column
static uint64_t g_nr_pass, g_nr_pass_bad;   // [stage 5.1 A5] the pass check of every replaced entry (vkn_shlib_named_pass)
static int g_diff_lines;
static void diff(const char *what, const std::string &key, const char *fmt, ...) {
    if (g_diff_lines++ >= 40) return;
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    printf("DIFF %s %s: %s\n", what, key.c_str(), b);
}
static const std::vector<uint32_t> *words(VkShaderModule m) {
    std::lock_guard<std::mutex> l(g_mk_mu); auto it = g_mods.find(reinterpret_cast<uint64_t>(m)); return it == g_mods.end() ? nullptr : &it->second;
}
static uint64_t g_digest = 1469598103934665603ull;   // FNV-1a over every compared module's words: shows the models really differ
static void digest(VkShaderModule m) {
    const std::vector<uint32_t> *w = words(m); if (!w) return;
    for (uint32_t x : *w) { g_digest ^= x; g_digest *= 1099511628211ull; }
}
static void forget(VkShaderModule m) { std::lock_guard<std::mutex> l(g_mk_mu); g_mods.erase(reinterpret_cast<uint64_t>(m)); }
// a module pair: both refused (= their base), or both built with equal words
static bool same_module(VkShaderModule o, VkShaderModule ob, VkShaderModule n, VkShaderModule nb, const char *what, const std::string &key, bool *refused) {
    const bool ro = o == ob, rn = n == nb; *refused = ro && rn;
    if (ro != rn) { diff(what, key, "old %s, new %s", ro ? "refused" : "built", rn ? "refused" : "built"); return false; }
    if (ro) return true;
    const std::vector<uint32_t> *wo = words(o), *wn = words(n);
    if (!wo || !wn) { diff(what, key, "module words missing (old %p new %p)", (const void *)wo, (const void *)wn); return false; }
    digest(n);
    if (*wo != *wn) {
        size_t at = 0; while (at < wo->size() && at < wn->size() && (*wo)[at] == (*wn)[at]) at++;
        diff(what, key, "words %zu vs %zu, first difference at word %zu", wo->size(), wn->size(), at); return false;
    }
    return true;
}
static void tally(Count &c, bool ok) { c.compared++; (ok ? c.same : c.differ)++; }

// ---------------------------------------------------------------- [P10] tile twins
namespace tile {
#include "../../../../android/shaders/tile_twins.inc"
static const int rows = int(sizeof k_tile_rows / sizeof k_tile_rows[0]);
// (binding, input_attachment_index) pairs of a module's words, by ascending binding
static std::vector<std::pair<uint32_t, uint32_t>> inputs(const std::vector<uint32_t> &w) {
    std::map<uint32_t, uint32_t> idx, bind; std::vector<std::pair<uint32_t, uint32_t>> out;
    for (size_t k = 5; k < w.size();) {
        const uint32_t c = w[k] >> 16, op = w[k] & 0xffff; if (!c || k + c > w.size()) break;
        if (op == 71 && c >= 4) { if (w[k + 2] == 43) idx[w[k + 1]] = w[k + 3]; if (w[k + 2] == 33) bind[w[k + 1]] = w[k + 3]; }
        k += c;
    }
    for (auto &e : idx) out.push_back({bind.count(e.first) ? bind[e.first] : ~0u, e.second});
    std::sort(out.begin(), out.end()); return out;
}
static uint64_t made, same_words, rewritten, none_ok, faults_ok, faults, bad;
static std::set<std::string> names;   // the built-in files handed out
static void fail(const char *fmt, ...) {
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (bad++ < 24) printf("TILE %s\n", b);
}
// one call of vkn_shlib_named_tile held against what must come back (want: a twin is due)
static void one(const char *name, const char *key, char variant, bool want) {
    xbg_vkn_stage *sf = new xbg_vkn_stage; uint64_t decl[4] = {~0ull, ~0ull, ~0ull, ~0ull}; vkn_tile_var var; memset(&var, 0xee, sizeof var);
    const int rc = vkn_shlib_named_tile(key, variant, sf, decl, &var); const char vc = variant ? variant : '0';
    if (!want) {
        if (rc != 1 || var.n || decl[0] || decl[3]) fail("%s variant %c: no twin is due, rc %d (n %u)", name, vc, rc, var.n); else none_ok++;
        delete sf; return;
    }
    const std::vector<uint32_t> *w = rc == 0 && sf->module ? words(sf->module) : nullptr;
    if (rc || !w || sf->stage != 4) { fail("%s variant %c: a twin is due, rc %d", name, vc, rc); delete sf; return; }
    // the row: the built-in file the library names (vkn_tile_stats.last), of this shader and variant
    const vkn_tile_row *hit = nullptr; const auto fin = inputs(*w); vkn_tile_stats ts; vkn_shlib_named_tile_stats(&ts);
    for (int r = 0; r < rows && !hit; r++) { const vkn_tile_row &t = k_tile_rows[r]; if (ts.last && !strcmp(t.inc, ts.last) && !strcmp(t.name, name) && t.variant == variant) hit = &t; }
    if (!hit) { fail("%s variant %c: no table row for the file the library names (%s)", name, vc, ts.last ? ts.last : "none"); delete sf; return; }
    bool ok = true;
    // the words: the file's, or (the load-time chain rewrote them: uniform blocks) still declaring exactly the row's inputs - checked below
    if (w->size() == hit->spv_bytes / 4 && !memcmp(w->data(), hit->spv, hit->spv_bytes)) same_words++; else rewritten++;
    names.insert(hit->inc);
    ok = ok && var.n == hit->n && !memcmp(var.binding, hit->binding, hit->n) && !memcmp(var.index, hit->index, hit->n) && var.cond == hit->cond && var.center_loc == hit->center_loc;
    ok = ok && fin.size() == var.n;
    uint32_t seen = 0;
    for (size_t q = 0; ok && q < fin.size(); q++) {
        ok = fin[q].first == var.binding[q] && fin[q].second == var.index[q] && var.index[q] < 16 && !(seen >> var.index[q] & 1) && (decl[var.binding[q] >> 6] >> (var.binding[q] & 63) & 1);
        seen |= 1u << var.index[q];
    }
    // `declared` = the Binding decorations of the built-in file
    uint64_t fd[4] = {0, 0, 0, 0};
    for (size_t k = 5; k < hit->spv_bytes / 4;) { const uint32_t c = hit->spv[k] >> 16, op = hit->spv[k] & 0xffff; if (!c) break;
        if (op == 71 && c >= 4 && hit->spv[k + 2] == 33 && hit->spv[k + 3] < 256) fd[hit->spv[k + 3] >> 6] |= 1ull << (hit->spv[k + 3] & 63); k += c; }
    ok = ok && !memcmp(fd, decl, sizeof fd);
    if (variant) {   // the fused forms: Sky's uniform extras always; its four G-buffer bindings only in the own-input form
        const bool own = variant == 'f' || variant == 'g', b200 = decl[3] >> (200 - 192) & 1, b254 = decl[3] >> (254 - 192) & 1;
        ok = ok && b254 && b200 == own && var.n == (own ? 11 : 7) && var.cond == (own ? 1u : 5u);
    } else ok = ok && var.n == 1 && var.binding[0] == 10 && var.index[0] == 0 && (var.cond & 1u);
    if (!ok) fail("%s variant %c: twin handed out but its facts are wrong (inputs in the words %zu, var.n %u, cond %u, words %zu, row %s)", name, vc, fin.size(), var.n, var.cond, w->size(), hit->inc);
    else made++;
    for (int f = 1; f <= 4; f++) { faults++; const int fr = vkn_shlib_named_tile_fault(key, variant, f); if (fr == 1) faults_ok++; else fail("%s variant %c: fault %d answered %d (1 = refused is due)", name, vc, f, fr); }
    forget(sf->module); delete sf;
}
static bool run(bool nr_mode1) {
    const int li = nr_sh_find_name("ClusteredLightingWithSSAO");
    for (int i = 0; i < nr_sh_count; i++) {
        const nr_sh_entry &e = nr_sh_table[i]; char key[17]; memcpy(key, e.key, 16); key[16] = 0;
        const bool chain = !strcmp(e.name, "LinearizeDepth") || !strncmp(e.name, "CascadedPCF_", 12);
        one(e.name, key, 0, nr_mode1 && chain && vkn_shlib_named_entry(key, nullptr) >= 0);
        if (i == li) continue;
        one(e.name, key, 'f', false); one(e.name, key, 'G', false);   // only the Light key has fused forms
    }
    if (li >= 0) {
        char key[17]; memcpy(key, nr_sh_table[li].key, 16); key[16] = 0;
        for (char v : {'f', 'g'}) {
            xbg_vkn_stage *sf = new xbg_vkn_stage; uint64_t decl[4], dr = 0;
            const bool fused = vkn_shlib_named_fused(key, v, sf, decl, &dr) == 0 && sf->module;
            if (sf->module) forget(sf->module);
            delete sf;
            one("ClusteredLightingWithSSAO", key, v, fused); one("ClusteredLightingWithSSAO", key, char(v - 32), fused);
        }
        xbg_vkn_stage *sf = new xbg_vkn_stage; vkn_tile_var var;
        if (vkn_shlib_named_tile(key, 'x', sf, nullptr, &var) != 1 || vkn_shlib_named_tile("0123456789abcdef", 0, sf, nullptr, &var) != 1 || vkn_shlib_named_tile("short", 0, sf, nullptr, &var) != 1)
            fail("a wrong variant / an unknown key / a short key must answer 1");
        else none_ok += 3;
        delete sf;
    }
    vkn_tile_stats ts; vkn_shlib_named_tile_stats(&ts);
    std::string nl; for (const std::string &n : names) nl += " " + n.substr(5);
    if (ts.refused) fail("the library refused %llu twins by a check (none is due to fail here)", (unsigned long long)ts.refused);
    printf("tile twins (P10): table rows %d; handed out and right %llu (words = the built-in file %llu, rewritten by the load-time chain %llu), no twin as due %llu, "
           "faults refused %llu of %llu; library counters ok %llu none %llu refused %llu; files:%s: %s\n", rows, (unsigned long long)made, (unsigned long long)same_words,
           (unsigned long long)rewritten, (unsigned long long)none_ok, (unsigned long long)faults_ok, (unsigned long long)faults, (unsigned long long)ts.ok,
           (unsigned long long)ts.none, (unsigned long long)ts.refused, nl.c_str(), bad ? "BAD" : "ok");
    return !bad;
}
}  // namespace tile

int main(int argc, char **argv) {
    // [stage 5 A1] "<model>+nr<q>" / "+nrv" / "+nr3nl": the named-bundle replacement on both sides (head comment)
    std::string mname = argc > 1 ? argv[1] : "", nr; const size_t plus = mname.find('+');
    if (plus != std::string::npos) { nr = mname.substr(plus + 1); mname.resize(plus); }
    std::string hk_fsr, hk_lc;   // [stage 5.4 G1] "-fsr<N>" / "-lc<N>" after "+nr..."
    for (size_t d; (d = nr.rfind('-')) != std::string::npos;) {
        const std::string o = nr.substr(d + 1); nr.resize(d);
        if (o == "fsr1" || o == "fsr2" || o == "fsr3") hk_fsr = o.substr(3);
        else if (o == "lc2" || o == "lc4") hk_lc = o.substr(2);
        else { fprintf(stderr, "unknown option -%s\n", o.c_str()); return 2; }
    }
    const bool nr_on = !nr.empty();
    if (nr_on && !(nr == "nr0" || nr == "nr1" || nr == "nr2" || nr == "nr3" || nr == "nrv" || nr == "nr3nl")) { fprintf(stderr, "unknown option +%s\n", nr.c_str()); return 2; }
    if ((!hk_fsr.empty() || !hk_lc.empty()) && !nr_on) { fprintf(stderr, "-fsr / -lc need +nr<q>\n"); return 2; }
    if (argc < 3 || !pick_model(mname.c_str())) { fprintf(stderr, "usage: %s adreno|mali|powervr|adreno-simmali|adreno-native|adreno-files[+nr0..3|+nrv|+nr3nl[-fsr1..3][-lc2|-lc4]] <shader dir> [limit]\n", argv[0]); return 2; }
    if (nr_on) {
        const char *q = nr == "nrv" ? "3" : nr == "nr3nl" ? "3" : nr.c_str() + 2, *m = nr == "nrv" ? "2" : "1";
        g_m.env.push_back({"DM_NR_SH", m}); g_m.env.push_back({"DM_NR_SH_PCF", q}); g_m.env.push_back({"DM_VK_NATIVE_SH", m}); g_m.env.push_back({"DM_VK_NATIVE_SH_PCF", q});
        g_d16_nolinear = nr == "nr3nl";
        if (!hk_fsr.empty()) g_m.env.push_back({"DM_HACK_FSR", hk_fsr.c_str()});
        if (!hk_lc.empty()) g_m.env.push_back({"DM_HACK_LIGHTS", hk_lc.c_str()});
    }
    signal(SIGSEGV, on_crash); signal(SIGBUS, on_crash); signal(SIGILL, on_crash);
    const std::string dir = argv[2]; const size_t limit = argc > 3 ? size_t(atol(argv[3])) : 0;
    // the phone's environment in mode 1: the app's defaults (jni_android.c, setenv without overwrite), the switches dm_dos2_vkn_pre forces
    // to 0, then the model's own switches
    static const char *const app[][2] = {{"DM_GFX_QUAD_DERIVATIVES", "1"}, {"DM_GFX_WARP32", "1"}, {"DM_GFX_LDC_FOLD", "1"}, {"DM_GFX_FORWARD_TEMPS", "1"},
                                         {"DM_GFX_NAN_MINMAX", "1"}, {"DM_GFX_D3PERF", "1"}, {"DM_GFX_PRESENT_THREAD", "1"}, {"DM_VK_NATIVE", "1"}};
    for (auto &e : app) setenv(e[0], e[1], 0);
    static const char *const off[] = {"DM_NR_SH", "DM_NR_ONCHIP", "DM_NR_ONCHIP_FOG", "DM_NR_MAT", "DM_NR_MAT_VS", "DM_NR_SH_SURF", "DM_NR_XLQ", "DM_NR_LIGHTFUSE",
                                      "DM_NR_LIGHTFUSE_FOG", "DM_NR_SH_PCF", "DM_HACK_FSR", "DM_NR_HEAT", "DM_NR_SEED", "DM_NB", "DM_NR", "DM_NB_POST",
                                      "DM_VK_NATIVE_SH", "DM_HACK_SHPCF", "DM_HACK_LIGHTS", "DM_HACK_TIER"};   // [stage 5 A1] the library's replacement: off unless "+nr"
    for (const char *k : off) setenv(k, "0", 1);
    for (auto &e : g_m.env) setenv(e.first, e.second, 1);
    const std::string logp = std::string("/tmp/vkn_shlib_test_") + g_m.name + (argc > 1 ? std::string("_") + argv[1] : std::string()) + ".log";
    g_log = fopen(logp.c_str(), "w");
    const VkPhysicalDeviceFeatures feat = g_m.feat;
    size_t in_pack = 0; std::vector<std::string> keys = keys_of(dir, &in_pack);
    if (limit && keys.size() > limit) keys.resize(limit);
    // the diagnostic spv-override (<shaders>/../spv-override/<key>.spv, listed once by both loaders): the first bundle's own SPIR-V with an
    // OpNop after the header (so a loader that ignored the override builds other words); and one key no bundle has (both: missing)
    const std::string ovr_dir = dir + "/../spv-override", ovr = keys.size() > 1 ? ovr_dir + "/" + keys[0] + ".spv" : std::string();
    if (!ovr.empty()) {
        std::vector<uint8_t> b; FILE *f = fopen((dir + "/" + keys[0] + ".xgs").c_str(), "rb");
        if (f) { fseek(f, 0, SEEK_END); b.resize(size_t(ftell(f))); rewind(f); if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear(); fclose(f); }
        xbg_shader_header h; if (b.size() >= sizeof h) memcpy(&h, b.data(), sizeof h); else h.spirv_bytes = 0;
        if (h.spirv_bytes && h.spirv_bytes <= b.size()) {
            mkdir(ovr_dir.c_str(), 0775);
            const uint8_t *w = b.data() + b.size() - h.spirv_bytes; const uint32_t nop = 0x00010000u;   // OpNop, word count 1
            if (FILE *o = fopen(ovr.c_str(), "wb")) { fwrite(w, 1, 20, o); fwrite(&nop, 4, 1, o); fwrite(w + 20, 1, h.spirv_bytes - 20, o); fclose(o); }
        }
    }
    keys.push_back("0000000000000000");
    // OLD: the translator, initialised exactly as dm_vk_initialize does (library only: step C)
    xbg_game_config c; memset(&c, 0, sizeof c); c.size = sizeof c;
    c.physical = reinterpret_cast<VkPhysicalDevice>(uintptr_t(0x1001)); c.device = reinterpret_cast<VkDevice>(uintptr_t(0x1002)); c.queue = reinterpret_cast<VkQueue>(uintptr_t(0x1003));
    c.queue_family = 0; c.enabled_core = feat; c.enabled_shader_draw_parameters = g_m.draw_params; c.fence_timeout_ns = 10000000000ull; c.shader_directory = dir.c_str();
    c.log = xlog; c.fatal = xfatal;
    char err[256] = {0};
    const uint64_t t0 = vkn_now_ns();
    if (xbg_game_initialize(&c, err, sizeof err)) { printf("FAIL old init: %s\n", err); return 1; }
    // NEW: the library on the same config
    vkn_shlib_config n; memset(&n, 0, sizeof n);
    n.physical = c.physical; n.device = c.device; n.queue = c.queue; n.queue_family = 0; n.enabled_core = feat; n.enabled_shader_draw_parameters = c.enabled_shader_draw_parameters;
    n.shader_directory = dir.c_str();
    if (vkn_shlib_init(&n, err, sizeof err)) { printf("FAIL new init: %s\n", err); return 1; }
    printf("model %s (%s), %zu bundles (%zu in the pack, %zu loose only)\n", argv[1], g_m.device, keys.size(), in_pack, keys.size() > in_pack ? keys.size() - in_pack : 0);
    static const struct { uint32_t func; float ref; } alphas[] = {{5, 0.0f}, {5, 0.25f}, {5, 0.5f}, {7, 0.5f}, {1, 0.0f}};
    xbg_vkn_stage *so = new xbg_vkn_stage, *sn = new xbg_vkn_stage;
    for (const std::string &key : keys) {
        const int ro = xbg_vkn_shader(key.c_str(), nullptr, nullptr, so), rn = vkn_shlib_shader(key.c_str(), nullptr, nullptr, sn);
        bool ok = ro == rn;
        if (!ok) diff("base", key, "rc old %d new %d", ro, rn);
        if (ok && ro == 1) g_missing++;
        if (ok && ro == 0) {
            if (so->stage != sn->stage) { ok = false; diff("base", key, "stage %u vs %u", so->stage, sn->stage); }
            if (so->binding_count != sn->binding_count || memcmp(so->bindings, sn->bindings, so->binding_count * sizeof(xbg_shader_binding))) {
                ok = false; uint32_t at = 0; while (at < so->binding_count && at < sn->binding_count && !memcmp(&so->bindings[at], &sn->bindings[at], sizeof(xbg_shader_binding))) at++;
                diff("base", key, "bindings %u vs %u, first difference at %u (binding %u: bytes %u/%u flags %#x/%#x)", so->binding_count, sn->binding_count, at,
                     at < so->binding_count ? so->bindings[at].binding : 0, at < so->binding_count ? so->bindings[at].bytes : 0, at < sn->binding_count ? sn->bindings[at].bytes : 0,
                     at < so->binding_count ? so->bindings[at].flags : 0, at < sn->binding_count ? sn->bindings[at].flags : 0);
            }
            if (so->input_count != sn->input_count || memcmp(so->inputs, sn->inputs, so->input_count * 4) || memcmp(so->input_classes, sn->input_classes, so->input_count * 4)) { ok = false; diff("base", key, "inputs / classes differ"); }
            if (so->c1_bytes != sn->c1_bytes || (so->c1_bytes && memcmp(so->c1, sn->c1, so->c1_bytes))) { ok = false; diff("base", key, "c1 differs"); }
            if (!so->module != !sn->module) { ok = false; diff("base", key, "usable old %d new %d", int(!!so->module), int(!!sn->module)); }
            if (ok && so->module) { bool r; ok = same_module(so->module, VK_NULL_HANDLE, sn->module, VK_NULL_HANDLE, "base", key, &r); }
            if (!so->module) g_unusable++;
            (so->stage == 0 ? g_vs : so->stage == 4 ? g_fs : g_cs)++;
        }
        tally(c_base, ok);
        if (nr_on && ro == 0 && nr_sh_find_key(key.c_str()) >= 0 && key.size() == 16) {   // [stage 5 A1] an NR-04 key
            g_nr_keys++; uint64_t dref = 0;
            const int ent = vkn_shlib_named_entry(key.c_str(), &dref);
            if (ent >= 0) {
                g_nr_replaced++; if (ok) g_nr_same++; if (dref) g_nr_dref++;
                {   // [stage 5.1 A5] the pass check (vkn_shlib_named_pass, at pipeline creation in vkn_shader.c): the verified pass keeps the
                    // module; one colour more, another format of colour 0, two samples take the translated twin in mode 1 - except a Dref
                    // module (its binding has the comparison sampler in every pipeline: kept, as on the old path) - and are only counted in
                    // mode 2 (verify); a pass not known (built off the recording thread) keeps the module; every call is counted
                    const nr_sh_entry &e = nr_sh_table[ent]; uint32_t f[8], g[8];
                    for (int c = 0; c < 8; c++) { f[c] = e.color0; g[c] = e.color0; }
                    g[0] = e.color0 == 0x25 ? 0x3f : 0x25;
                    vkn_shlib_named_stats a, b; vkn_shlib_named_stats_get(&a);
                    const int v = vkn_shlib_named_pass(ent, e.colors, f, 0x32, 1, 1920, 1080), c1 = vkn_shlib_named_pass(ent, e.colors + 1, f, 0x32, 1, 1920, 1080),
                              fo = vkn_shlib_named_pass(ent, e.colors, g, 0x32, 1, 1920, 1080), sa = vkn_shlib_named_pass(ent, e.colors, f, 0x32, 2, 1920, 1080),
                              un = vkn_shlib_named_pass(ent, -1, nullptr, 0, 1, 0, 0), bad = vkn_shlib_named_pass(-1, 1, f, 0, 1, 0, 0);
                    vkn_shlib_named_stats_get(&b);
                    const int want = nr != "nrv" && !dref ? 1 : 0;
                    const bool pg = v == 0 && c1 == want && fo == want && sa == want && un == 0 && bad == 0 && b.pass_verified == a.pass_verified + 1 &&
                                    b.pass_outside == a.pass_outside + 3 && b.pass_unchecked == a.pass_unchecked + 1 &&
                                    b.pass_dref_kept == a.pass_dref_kept + (nr != "nrv" && dref ? 3 : 0) && b.passes >= a.passes;
                    (pg ? g_nr_pass : g_nr_pass_bad)++;
                    if (!pg) diff("nr-pass", key, "entry %d (%s, %u colours, colour 0 %#x, dref %d): verified %d, +1 colour %d, other format %d, 2 samples %d (want %d), unknown %d, bad entry %d; "
                                  "counted verified +%llu outside +%llu unchecked +%llu dref kept +%llu", ent, e.name, e.colors, e.color0, int(dref != 0), v, c1, fo, sa, want, un, bad,
                                  (unsigned long long)(b.pass_verified - a.pass_verified), (unsigned long long)(b.pass_outside - a.pass_outside),
                                  (unsigned long long)(b.pass_unchecked - a.pass_unchecked), (unsigned long long)(b.pass_dref_kept - a.pass_dref_kept));
                }
                xbg_vkn_stage *sa = new xbg_vkn_stage;
                const int ra = vkn_shlib_named_alt(key.c_str(), sa);
                const std::vector<uint32_t> *wa = ra == 0 && sa->module ? words(sa->module) : nullptr, *wn = sn->module ? words(sn->module) : nullptr;
                bool good = wa && wn && sa->stage == 4 && sa->binding_count == sn->binding_count;
                for (uint32_t b = 0; good && b < sa->binding_count; b++) good = sa->bindings[b].binding == sn->bindings[b].binding && sa->bindings[b].type == sn->bindings[b].type;
                if (good && nr != "nrv" && *wa == *wn) good = false;   // mode 1: the twin is the translated module, not the named one
                if (good && nr == "nrv" && *wa != *wn) good = false;   // mode 2: both are the translated module
                (good ? g_nr_alt : g_nr_alt_bad)++;
                if (!good) diff("nr-alt", key, "translated twin rc %d stage %u bindings %u/%u words %zu/%zu", ra, sa->stage, sa->binding_count, sn->binding_count, wa ? wa->size() : 0, wn ? wn->size() : 0);
                if (sa->module) forget(sa->module);
                delete sa;
            }
        }
        if (!ok || ro != 0) continue;
        if (so->stage == 4) for (auto &a : alphas) {
            const VkShaderModule mo = xbg_vkn_alpha_module(so->shader, a.func, a.ref), mn = vkn_shlib_alpha_module(sn->shader, a.func, a.ref);
            bool refused = false; char what[32]; snprintf(what, sizeof what, "alpha %u/%g", a.func, double(a.ref));
            tally(c_alpha, same_module(mo, so->module, mn, sn->module, what, key, &refused)); if (refused) g_alpha_refused++;
            if (mo != so->module) forget(mo);
            if (mn != sn->module) forget(mn);
        }
        if (so->stage == 0 && so->input_count) {
            uint64_t codes[4] = {0, 0, 0, 0}; const uint32_t l0 = so->inputs[0];
            codes[0] = uint64_t(2u << (2 * l0));
            for (uint32_t i = 0; i < so->input_count; i++) { codes[1] |= uint64_t(3u << (2 * so->inputs[i])); codes[2] |= uint64_t(1u << (2 * so->inputs[i])); }
            codes[3] = uint64_t(1u << (2 * l0)) << 32; if (so->input_count > 1) codes[3] |= uint64_t(1u << (2 * so->inputs[1]));
            for (uint64_t code : codes) {
                const VkShaderModule mo = xbg_vkn_retype_module(so->shader, code), mn = vkn_shlib_retype_module(sn->shader, code);
                bool refused = false; char what[40]; snprintf(what, sizeof what, "retype %#llx", (unsigned long long)code);
                tally(c_retype, same_module(mo, so->module, mn, sn->module, what, key, &refused)); if (refused) g_retype_refused++;
                if (mo != so->module) forget(mo);
                if (mn != sn->module) forget(mn);
            }
        }
        if (so->module) forget(so->module);
        if (sn->module) forget(sn->module);
    }
    const double s = double(vkn_now_ns() - t0) / 1e9;
    vkn_shlib_stats st; vkn_shlib_stats_get(&st);
    printf("model %s: bundles compared %llu identical %llu different %llu (vertex %llu fragment %llu compute %llu, unusable %llu, missing %llu); "
           "alpha variants compared %llu identical %llu different %llu (both refused %llu); retype variants compared %llu identical %llu different %llu (both refused %llu); "
           "library: %llu from the pack, %llu from files, %llu native; modules created %llu; words digest %016llx; %.1f s\n"
           "with replacement (%s): NR-04 keys %llu, replaced by name %llu (identical to the translator %llu), with a Dref binding %llu; translated twins ok %llu, bad %llu; pass checks ok %llu, bad %llu\n",
           argv[1], (unsigned long long)c_base.compared, (unsigned long long)c_base.same, (unsigned long long)c_base.differ, (unsigned long long)g_vs, (unsigned long long)g_fs,
           (unsigned long long)g_cs, (unsigned long long)g_unusable, (unsigned long long)g_missing, (unsigned long long)c_alpha.compared, (unsigned long long)c_alpha.same,
           (unsigned long long)c_alpha.differ, (unsigned long long)g_alpha_refused, (unsigned long long)c_retype.compared, (unsigned long long)c_retype.same,
           (unsigned long long)c_retype.differ, (unsigned long long)g_retype_refused, (unsigned long long)st.from_pack, (unsigned long long)st.from_file,
           (unsigned long long)st.native_used, (unsigned long long)g_mod_creates, (unsigned long long)g_digest, s,
           nr_on ? nr.c_str() : "off", (unsigned long long)g_nr_keys, (unsigned long long)g_nr_replaced, (unsigned long long)g_nr_same, (unsigned long long)g_nr_dref,
           (unsigned long long)g_nr_alt, (unsigned long long)g_nr_alt_bad, (unsigned long long)g_nr_pass, (unsigned long long)g_nr_pass_bad);
    bool hk_ok = true;   // [stage 5.4 G1] the asked hacks reached the library (the words compares above held them against the translator)
    if (!hk_fsr.empty() || !hk_lc.empty()) {
        vkn_shlib_named_hacks h; vkn_shlib_named_hacks_get(&h);
        const bool want_f = !hk_fsr.empty(), want_l = !hk_lc.empty(), check_f = hk_fsr == "2";
        const bool fok = !want_f || (h.fsr == atoi(hk_fsr.c_str()) && h.state[0] == 1 && (check_f ? h.swapped[0] == 0 && h.checked > 0 : h.swapped[0] > 0));
        const bool lok = !want_l || (h.lights == atoi(hk_lc.c_str()) && h.state[1] == 1 && h.swapped[1] > 0);
        hk_ok = fok && lok && !h.refused[0] && !h.refused[1];
        if (want_l && h.lcap == 2) {   // DM_VK_NATIVE_LCAP=2 (default): the capped fused modules behind the Light head, for 'f' and 'g'
            const int li = nr_sh_find_name("ClusteredLightingWithSSAO"); int made = 0;
            for (char v : {'f', 'g'}) {
                xbg_vkn_stage *sf = new xbg_vkn_stage; uint64_t decl[4], dr = 0;
                const int rc = li >= 0 ? vkn_shlib_named_fused(nr_sh_table[li].key, v, sf, decl, &dr) : -2;
                made += rc == 0 && sf->module && sf->stage == 4 && (decl[3] >> (254 - 192) & 1);   // binding 254 (Sky's c6 extra) declared
                if (sf->module) forget(sf->module);
                delete sf;
            }
            vkn_shlib_named_hacks_get(&h);
            const bool fused_ok = made == 2 && h.lcf == 1 && h.lcf_bundles >= 2;
            printf("hacks: DM_VK_NATIVE_LCAP=2: capped fused modules made %d of 2 (Sky c1 check %d, bundles %llu): %s\n", made, h.lcf, (unsigned long long)h.lcf_bundles, fused_ok ? "ok" : "BAD");
            hk_ok = hk_ok && fused_ok;
        }
        printf("hacks: DM_HACK_FSR=%s (library %d: state %d, swapped %llu, checked %llu, refused %llu), DM_HACK_LIGHTS=%s (library %d: state %d, swapped %llu, refused %llu): %s\n",
               want_f ? hk_fsr.c_str() : "0", h.fsr, h.state[0], (unsigned long long)h.swapped[0], (unsigned long long)h.checked, (unsigned long long)h.refused[0],
               want_l ? hk_lc.c_str() : "0", h.lights, h.state[1], (unsigned long long)h.swapped[1], (unsigned long long)h.refused[1], hk_ok ? "ok" : "BAD");
    }
    // [P10 fix 5] (phone run F06) the position program of LinearizeDepth's vertex shader and its fragment outputs - what the centre proof
    // of the merged tile pass reads - are built wherever the tile pass is on, not only where DM_VK_NATIVE_LOAD is: "adreno-tl" must have
    // them; plain "adreno" (LOAD 0, TILELIGHT 0) builds none (the load-time cost stays where nothing reads them)
    bool posl_ok = true;
    if (g_m.name == std::string("adreno")) {
        xbg_vkn_stage *v = new xbg_vkn_stage(), *f = new xbg_vkn_stage();
        const int rv = vkn_shlib_shader("06b17b62ec233b75", nullptr, nullptr, v), rf = vkn_shlib_shader("f23c09d198b55dc5", nullptr, nullptr, f);
        const bool have = !rv && !rf && vkn_shlib_vs_pos(v->shader) && vkn_shlib_vs_pos(v->shader)->ok && vkn_shlib_fs_outputs(f->shader) >= 0;
        const char *tl = getenv("DM_VK_NATIVE_TILELIGHT"); const bool want = tl && atoi(tl) > 0;
        const char *ld = getenv("DM_VK_NATIVE_LOAD"); const bool load = ld && atoi(ld) > 0;
        posl_ok = rv || rf ? true : (want || load) ? have : !have;
        printf("tile pass facts: LinearizeDepth position program %s, fragment outputs %s (DM_VK_NATIVE_TILELIGHT %s): %s\n", have ? "built" : "none", have ? "built" : "none",
               tl ? tl : "-", rv || rf ? "bundles missing (not checked)" : posl_ok ? "ok" : "BAD");
        delete v; delete f;
    }
    const bool tile_ok = limit ? true : tile::run(nr_on && nr != "nrv");   // [P10] (needs every NR-04 key loaded: not with a bundle limit)
    if (nr_on) vkn_shlib_named_census();
    if (g_log) fclose(g_log);
    if (!ovr.empty()) remove(ovr.c_str());
    const bool pass = !c_base.differ && !c_alpha.differ && !c_retype.differ && c_base.compared && !g_nr_alt_bad && !g_nr_pass_bad && (!nr_on || nr == "nr0" || g_nr_replaced || limit) &&
                      (hk_ok || limit) && tile_ok && posl_ok;   // [stage 5.4 G1] [P10] [P10 fix 5]
    printf("%s\n", pass ? "PASS" : "FAIL");
    fflush(stdout); _exit(pass ? 0 : 1);   // no static destructors: the translator's state is not shut down
}
