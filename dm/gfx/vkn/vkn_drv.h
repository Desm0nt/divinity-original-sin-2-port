// [DOS2] VK-NATIVE stage 5.3, piece V2: driver corrections of the backend (vkn_drv.c). Plan: docs/dos2/nr/VK-STAGE5-PLAN.md section 1.в
// rows "Замок компилятора PowerVR", "Без динамических блоков на PowerVR", "Маска цвета у FS без выходов", "Смещение глубины x2 ..., повторная
// привязка после очистки", stage 5.3 row "В2". The old path's rules, ported (not re-decided): dm/gfx/vulkan/engine/xbg_vendor_policy.h
// (their history and sources there) as read by xbg_game.cpp (NVN gpu policy / NVN driver quirks lines); the rules in C: vkn_policy.h block
// "stage 5.3 V2". The SAME switch names as the old path (none of them is forced off in mode 1: dm_dos2_vkn_pre's k_pre_off has no DM_GFX_*
// policy), so dm/env, the gear and the safe-mode ladder (xbg_safe_mode.h) act on both paths with one key:
//   correction                         switch                   default (policy family / driverID)     where
//   colour mask 0, FS without outputs   DM_GFX_NOOUT_MASK        1 every family but Adreno              vkn_state.c pipe_build (key unchanged)
//   depth bias constant x2              DM_GFX_DEPTH_BIAS_X2     1 PanVK, RADV                          vkn_state.c raster bias at build
//   forget bindings after an in-pass   DM_GFX_CLEAR_REBIND      1 Qualcomm / AMD / Imagination stock   vkn_pass.c / vkn_shc.c after each
//     vkCmdClearAttachments                                      drivers (not Turnip)                   vkCmdClearAttachments -> vkn_draw_raw_begin
//   module + pipeline creation lock    DM_GFX_PIPE_SERIAL       1 PowerVR                              vkn_state.c (graphics + compute builds,
//                                                                                                       render thread and warm-up worker),
//                                                                                                       vkn_ui.c, vkn_bcgpu.c; modules: vkn_shlib
//   dynamic uniform buffers            DM_GFX_UBO_DYNAMIC       0 PowerVR, 1 others (2 = rule below)   vkn_shader.c layouts (dyn_policy)
//   NR-08b read-only dynamic blocks    DM_NR_UBODYN             1 PowerVR (only where the above is off) vkn_shader.c: a per-draw block is
//                                                                                                       dynamic only when the shader library
//                                                                                                       scanned it and the stage reads it
// Family = the shader library's policy family (vkn_shlib_policy: real + DM_GFX_SIM_GPU), else vkn_policy_family_from over the device; the
// driverID from VkPhysicalDeviceDriverProperties (Vulkan 1.2; 0 = unknown: no driver correction applies). The SIM token ubdyn8 narrows the
// dynamic-buffer limit the layouts see to 8 (the old xbg_sim_limits), noaniso / the powervr profile turn sampler anisotropy off (xbg_sim_features).
// Resolved ONCE, at the first use with a device (any thread; one install line "driver corrections (...)"); the census line "driver
// corrections:" counts what each did. Hot path: nothing per draw. A pipeline build / clear / shader creation reads one cached word.
// On Adreno with Turnip (the Mi 14) every correction is off: pipelines, layouts and recordings are bit for bit those of stage 5.2.
// Limits: the census line is registered in mode 1 only (hle_dos2_vkn_pass.c); without the shader library (mode 2, DM_VK_NATIVE_SHLIB=0)
// the creation lock is this module's own mutex (the translator's modules stay under its gpguard lock: not one mutex) and the colour-output
// fact is unknown (masks as the key, counted, red lines for engine shaders). Not ported: the old live fallback (a failed pipeline build
// turns DM_GFX_PIPE_SERIAL on in the running process) and DM_GFX_PIPE_GUARD (crash markers of in-flight builds across boots).
#ifndef DM_VKN_DRV_H
#define DM_VKN_DRV_H
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "vkn_state.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_DRV_E_NOOUT = 1, VKN_DRV_E_BIAS = 2, VKN_DRV_E_REBIND = 4, VKN_DRV_E_SERIAL = 8, VKN_DRV_E_UBO = 16, VKN_DRV_E_UBODYN = 32, VKN_DRV_E_FBX = 64 };
typedef struct vkn_drv_cfg {
    int resolved;            // 1 = decided from a device (else the "nothing changes" answer before the device: not latched)
    int family;              // VKN_FAM_* (vkn_policy.h)
    uint32_t driver_id;      // VkDriverId, 0 = unknown
    uint8_t noout;           // DM_GFX_NOOUT_MASK in effect
    uint8_t bias_x2;         // DM_GFX_DEPTH_BIAS_X2
    uint8_t rebind;          // DM_GFX_CLEAR_REBIND
    uint8_t serial;          // DM_GFX_PIPE_SERIAL
    uint8_t ubo_dyn;         // dynamic uniform buffers in layouts (DM_GFX_UBO_DYNAMIC; on again when the NR-08b rule is in force)
    uint8_t ubo_ro;          // the NR-08b read-only rule is in force (per-draw blocks only, scanned and read)
    int8_t ubodyn;           // DM_NR_UBODYN mode in effect (0 / 1 / 2)
    uint8_t sim_noaniso;     // DM_GFX_SIM_GPU: no sampler anisotropy in the model (vkn_tex.c samplers)
    uint8_t sim_ubdyn8;      // DM_GFX_SIM_GPU ubdyn8: the layouts see maxDescriptorSetUniformBuffersDynamic at most 8
    uint8_t lock_lib;        // the creation lock is the shader library's (else this module's own)
    uint16_t from_env;       // VKN_DRV_E_*: the environment decided that switch (else the family / driver default)
    uint8_t fb_exact;        // [VK-DRV] DM_VK_NATIVE_FB_EXACT: framebuffers made with the exact pass begun (vkn_policy_fb_exact_default)
} vkn_drv_cfg;
// The rules over explicit facts (reads the switches from the environment; pure otherwise): what the backend resolves at its first use.
void vkn_drv_decide(int family, uint32_t driver_id, const char *sim, vkn_drv_cfg *out);
// The configuration in effect (resolved at the first call with a device: install line). Never NULL.
const vkn_drv_cfg *vkn_drv(void);

// Builds: the creation lock (DM_GFX_PIPE_SERIAL): the shader library's module lock when the library is initialised (modules and
// pipelines under ONE mutex, as gpguard::Lock / ModuleLock), else a lock of this module. Returns 1 when taken (pass it to unlock).
// Never nested; never held across anything but the one vkCreate* call.
int vkn_drv_lock(void);
void vkn_drv_unlock(int took);
// Colour write mask decision of a graphics pipeline (vkn_state.c pipe_build): 1 = every colour attachment's mask 0 (no fragment stage,
// or a fragment shader of the library with no colour output and no sample-mask write), 0 = as the key says. Counted.
int vkn_drv_noout(const vkn_pipe_desc *k, const vkn_pipe_src *src);
// The depth bias constant factor scale of this driver (1 or 2); counted per built pipeline with bias on (vkn_drv_bias_note).
float vkn_drv_bias_scale(void);
void vkn_drv_bias_note(void);
// After a vkCmdClearAttachments recorded inside a pass (recording thread): with DM_GFX_CLEAR_REBIND the draw path forgets what it bound
// (vkn_draw_raw_begin), so the next draw binds pipeline, set, streams, viewport and scissor again. Counted.
void vkn_drv_cleared(void);
// vkn_shader.c layout counters: shaders whose uniform buffers are all static because the family turned dynamic buffers off; bindings
// the NR-08b rule kept static (not scanned / not read) / made dynamic.
void vkn_drv_ubo_note(int all_static, uint32_t ro_static, uint32_t ro_dynamic);

typedef struct vkn_drv_stats {
    uint64_t noout_masked, noout_nofs, noout_kept, noout_unknown;   // pipelines: masked (FS without outputs) / masked (no FS) / FS has outputs / facts unknown
    uint64_t bias_x2;                                               // pipelines built with depth bias on and the x2 scale
    uint64_t rebinds;                                               // in-pass clears after which the binds were forgotten
    uint64_t locked;                                                // vkCreate*Pipelines / vkCreateShaderModule calls of the backend under the lock
    uint64_t ubo_static_shaders, ubo_ro_static, ubo_ro_dynamic;     // vkn_drv_ubo_note
    uint64_t reds;
} vkn_drv_stats;
void vkn_drv_stats_get(vkn_drv_stats *s);
void vkn_drv_census(void);   // one line (the host's 10 s census: hle_dos2_vkn_pass.c registers it)
// Host tests: put a configuration in effect (NULL = forget: the next use resolves again from the device); counters reset.
void vkn_drv_test_set(const vkn_drv_cfg *c);
struct vkn_shader;
void vkn_drv_test_facts(int (*fn)(const struct vkn_shader *s));   // the "no colour output" fact (NULL = the shader library's)

#ifdef __cplusplus
}
#endif
#endif
