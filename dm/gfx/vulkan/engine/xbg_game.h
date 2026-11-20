#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Original NVN ABI is provided by xbg_nvn_get_proc_address. Guest addresses are
 * directly accessible 64-bit pointers, as in the original host. One game/device
 * per process. Initialize before entering the original nnMain. */
typedef struct xbg_game_config {
    uint32_t size;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkPhysicalDeviceFeatures enabled_core;
    VkBool32 enabled_shader_draw_parameters;
    uint64_t fence_timeout_ns;
    const char *shader_directory; /* <control-sha256-first16>.xgs bundles */
    void *userdata;
    /* Optional guest trampoline. NULL means callbacks are directly callable. */
    uint64_t (*invoke_guest)(void *userdata,void *function,const uint64_t *args,uint32_t count);
    /* Synchronous: source is TRANSFER_SRC_OPTIMAL and queue-family owned.
     * Restore that layout and complete all external GPU use before returning.
     * Return zero only when presentation was actually submitted/completed.
     * crop={0,0,0,0} means full image. Native window is the game's opaque token. */
    int (*present)(void *userdata,void *native_window,VkImage image,VkFormat format,
        uint32_t width,uint32_t height,const int32_t crop[4],int32_t interval);
    void (*log)(void *userdata,const char *message);
    /* Called immediately before abort for unsupported called APIs/GPU errors.
     * May terminate/unwind the runtime. It must not return to guest execution. */
    void (*fatal)(void *userdata,const char *message);
} xbg_game_config;

int xbg_game_initialize(const xbg_game_config *config,char *error,size_t error_bytes);
/* Call after initialize and before nnMain. True only if the logical device was
 * actually created with VK_KHR_sampler_mirror_clamp_to_edge enabled (Vulkan1.1)
 * or VkPhysicalDeviceVulkan12Features.samplerMirrorClampToEdge enabled (1.2+).
 * Supported-but-not-enabled is false. Default false; existing config ABI intact. */
int xbg_game_set_sampler_mirror_clamp_to_edge(VkBool32 enabled);
/* Same contract for VK_EXT_sampler_filter_minmax / VkPhysicalDeviceVulkan12Features.samplerFilterMinmax: true only if actually
 * enabled on the logical device. Enables NVN min/max sampler reduction (formats with the MINMAX feature); default false = average. */
int xbg_game_set_sampler_filter_minmax(VkBool32 enabled);
/* CPU访问事件前解析覆盖该地址的延后SignalEvent。
 * Resolve deferred SignalEvents covering this address before CPU event access. */
void xbg_game_event_read(const void *addr);
/* [D3] DM_GFX_IDLE_RETIRE (default 0). event_peek: nvnEventGetValue's hook; at 2 it retires signalled slots without
 * blocking and returns whatever the GPU has reached (Switch semantics), below 2 it is xbg_game_event_read. idle_poll: the
 * translation thread calls it when it has had no job for a while (idle_ns); it retires signalled slots (publishing their
 * events, counter reports and writebacks) and submits a recording slot that holds deferred events or reports; returns
 * nonzero while GPU work is outstanding, so the caller keeps polling. */
void xbg_game_event_peek(const void *addr);
int xbg_game_idle_poll(uint64_t idle_ns);
int xbg_game_idle_retire_mode(void);
/* 宿主管SIGSEGV时先注册use_host_hooks；fault返回是否命中跟踪页，宿主写客体前调用prepare。
 * With host-owned SIGSEGV, register use_host_hooks first; fault reports tracked-page hits and prepare precedes host writes into guest memory. */
void xbg_wtrack_use_host_hooks(void);
int xbg_wtrack_fault(void *addr);
void xbg_wtrack_prepare(void *addr,size_t len);
/* Optional one-frame diagnostics, disabled by default. Call after initialize,
 * before nnMain. directory must already exist and be private/writable. Captures
 * the 1-based present frame, at most 16 draw/clear events and 64 MiB total raw
 * inputs/outputs. No shader/state/image replacement. The directory is copied.
 * Returns 0, -1 for invalid arguments/timing, -2 if capture.txt cannot be opened.
 * Read capture.txt for actual bytes, VkFormat, row size, mip/layer and limits. */
/* Default-off single-frame provenance. Exclusive new file; nullptr closes.
 * Same-thread synchronous cfg.present callback only; no Session/window locks. */
int xbg_game_provenance(const char *path);
int xbg_game_capture_frame(const char *directory,uint64_t frame);
/* Default-off CPU-side elapsed profiling, not GPU timestamps. Before nnMain,
 * after initialize: first_frame is 1-based; count 1..120. No config ABI change.
 * Starts after first_frame-1 successful presents (1 includes startup). Stops at
 * requested last present or first present after 60 seconds. Emits 15 bounded
 * summary lines plus one pipeline-cache counter line through cfg.log. Excludes guest work/lock contention; nested
 * exclusive_ns sum without double counting, inclusive_ns/max_ns do not.
 * Mutually exclusive with capture_frame. Returns 0, or -1 invalid/timing/busy.
 * Fatal/no further present can leave no summary; shutdown emits partial only. */
int xbg_game_profile_cpu(uint64_t first_frame,uint32_t frame_count);
/* One accepted live window per initialized session; 1..60 frames,60s next-present bound.
 * Call only from synchronous guest present callback after frontend Session unlock,
 * with no concurrent hostPerf/provenance/capture. Return0 schedules next full frame;
 * missing completion callback is INCOMPLETE, never elapsed-time success. */
int xbg_game_profile_cpu_live(uint32_t frame_count);
int xbg_game_shutdown(void); /* all game threads must have exited */
void *xbg_nvn_get_proc_address(const char *name);
const char *xbg_game_last_error(void);
typedef struct xbg_game_stats { uint64_t submissions,draws,dispatches,presents,shader_loads; } xbg_game_stats;
void xbg_game_get_stats(xbg_game_stats *out);
/* Counter reads above remain safe after fatal. GPU operations and shutdown do
 * not: a runtime nonlocal exit requires restarting the application process. */

/* Wire format for offline-exported genuine shaders, little endian. No pointers.
 * Header, bindings[binding_count], input_locations[input_count], c1[c1_bytes],
 * then SPIR-V. Unknown source bindings are rejected by the exporter. */
enum { XBG_SOURCE_SUPPORT=1,XBG_SOURCE_UNIFORM=2,XBG_SOURCE_STORAGE=3,
    XBG_SOURCE_C1=4,XBG_SOURCE_DRIVER0=5,XBG_SOURCE_DRIVER2=6,
    XBG_SOURCE_TEXTURE=7,XBG_SOURCE_IMAGE=8,
    /* [ETERNAL] bindless (pack_game_shaders.py, research/shaders_xlate.md): handles / addresses kept in the game's own uniform
     * buffers (slot = bank<<16 | word, NVN uniform slot = bank - 3) and descriptor arrays mirroring the NVN pools. */
    XBG_SOURCE_TEXTURE_UBO=9,XBG_SOURCE_IMAGE_UBO=10,XBG_SOURCE_STORAGE_UBO=11,
    XBG_SOURCE_TEXTURE_POOL=12,XBG_SOURCE_SAMPLER_POOL=13,XBG_SOURCE_IMAGE_POOL=14 };
typedef struct xbg_shader_header {
    uint32_t magic,version,stage,binding_count,input_count,c1_bytes,spirv_bytes,reserved;
} xbg_shader_header; /* magic 0x31534758 (XGS1), version 1, stage VS0..CS5 */
typedef struct xbg_shader_binding {
    uint32_t binding,type,count,bytes,source,slot,sampler_slot,flags;
} xbg_shader_binding; /* flags: bit0/1 formatless write/read; bit31 image shape present,
                       * dim in bits8..10, array12, MS13, depth14, scalar16..17 */
/* [DOS2] VK-NATIVE export (xbg_game.cpp, end of file; caller dm/gfx/vkn/vkn_shader.c in mode 2 and in mode 1 with DM_VK_NATIVE_SHLIB=0;
 * mode 1's default is the backend's own shader library dm/gfx/vkn/vkn_shlib.h, which fills the same xbg_vkn_stage word for word - host test
 * engine/tests/vkn_shlib_test.cpp - and the GPU BC data doors moved to dm/gfx/vkn/vkn_shlib_bc.cpp): one stage bundle of the packs as the translator
 * loads it (load_shader_key: every load-time SPIR-V rewrite applied), by its program key (xbg_shader_program_key of control + code).
 * The module and the shader object belong to the translator and live until xbg_game_shutdown; bindings / inputs are copies; c1 points
 * into the shader object (immutable after load). Each call takes the engine mutex. */
typedef struct xbg_vkn_stage {
    void *shader;                      /* opaque: the translator's shader object (argument of the variant getters) */
    VkShaderModule module;             /* VK_NULL_HANDLE: the device lacks a capability the module needs (its draws are skipped) */
    uint32_t stage;                    /* bundle stage: 0 vertex, 4 fragment, 5 compute */
    uint32_t binding_count,input_count,c1_bytes;
    xbg_shader_binding bindings[256];  /* binding = Vulkan binding (original binding x 6 + bundle stage), one descriptor set */
    uint32_t inputs[16],input_classes[16];   /* vertex: input locations and their scalar class (xbg_vertex_input_classes) */
    const uint8_t *c1;                 /* XBG_SOURCE_C1 data (the shader's own constants), c1_bytes long */
} xbg_vkn_stage;
/* 0 = loaded; 1 = no bundle for the key (logged and dumped as the translator does for any missing stage); -1 = translator not ready. */
int xbg_vkn_shader(const char *key,const void *control,const void *code,xbg_vkn_stage *out);
/* Fragment module with the alpha test of NVN compare func (1 NEVER .. 8 ALWAYS) and reference (alpha_module, key "#at"); the base module
 * for ALWAYS, with DM_GFX_ALPHA_TEST off, or when the rewrite is refused. */
VkShaderModule xbg_vkn_alpha_module(void *shader,uint32_t func,float ref);
/* Vertex module for a retype code (vtx_retype_module, key "#vi": low 32 bits 2 bits per location 0 keep / 1 float / 2 sint / 3 uint,
 * high 32 bits 2 bits per location 1 = SSCALED bound as SINT, 2 = USCALED bound as UINT); the base module for code 0 or a refusal. */
VkShaderModule xbg_vkn_retype_module(void *shader,uint64_t code);
int xbg_vkn_cache_dir(char *out,size_t bytes);   // [DOS2] VK-NATIVE stage 4: directory of the pipeline cache files (0 = ok)
unsigned xbg_vkn_lib_report(unsigned *warm);   // [DOS2] VK-NATIVE no-NVN: 1 library-only, 2 pipeline cache, 4 warm-up threads, 8 pool reset, 16 gfx worker
#ifdef __cplusplus
}
#endif

// Optional descriptor detail v2; same once-only live CPU window; default OFF.
#ifdef __cplusplus
extern "C"
#endif
int xbg_game_profile_descriptor_live(uint32_t frame_count);

// Optional Draw detail v1; same once-only live window; default OFF.
// Host exclusive time, not GPU/thread CPU. See frozen INTEGRATION.txt.
#ifdef __cplusplus
extern "C"
#endif
int xbg_game_profile_draw_live(uint32_t frame_count);
