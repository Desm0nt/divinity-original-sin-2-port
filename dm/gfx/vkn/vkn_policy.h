// [DOS2] VK-NATIVE stage 5.2 B1: the backend's per-GPU-family defaults (pure C, header only; C and C++). The family is the one the
// shader library decides at init (vkn_shlib.cpp: xbg_gpu_family of the device, then xbg_effective_family with DM_GFX_SIM_GPU - a
// simulated family wins over the real one) and prints in its "initialised ... family X policy Y sim Z" line; this header asks the
// library for it (vkn_shlib_policy, weak: host tests and builds without the library) and otherwise computes the same answer from the
// device's properties and the environment (vkn_policy_family_from: the vendor / name rule of xbg_gpu_family and the family bits of
// xbg_sim_parse's profiles, token for token). Existing switches of other modules keep their own readers; the getters here are the
// ones the render-pass policy (vkn_pass.c, the shader library's position programs) needs, in one place:
//   DM_VK_NATIVE_LOAD    loadOp DONT_CARE for attachments the pass's first draw provably overwrites (old names DM_NR_COVER /
//                        DM_GFX_NOLOAD): 0 off, 1 on, 2 verify (decided and counted, LOAD kept), 3 poison (1 + magenta / wrong depth
//                        under the proven rect right after the begin). Default: Mali / PowerVR 1, every other family 0 (Turnip on the
//                        Mi 14 runs sysmem: a load costs nothing there, and the proof costs the engine thread a few microseconds a pass).
//   DM_VK_NATIVE_STORE   storeOp DONT_CARE by the previous frames' history (old names DM_GFX_STORE_HIST / the NB-06 plan): 0 / 1 / 2 /
//                        3 (verify / poison). Default: Mali / PowerVR 1, others 0 (a wrong prediction is one frame of garbage; bandwidth
//                        matters on tilers only - xbg_store_hist_default).
//   DM_VK_NATIVE_FRESH   fresh targets (old name DM_NR_RP_FRESH): 0 off; 1 a never-written single-subresource target begins with loadOp
//                        CLEAR to zeros instead of the zero fill + LOAD (exact: the same zeros, one clear command and one load fewer);
//                        2 = 1 + an attachment whose shared storage another image rendered into since its own last write begins
//                        DONT_CARE (NR-10's rule: the Switch holds the other texture's bytes there); 3 = 2 + magenta poison of what 2
//                        begins DONT_CARE. Default: Mali / PowerVR 2, others 1.
//   DM_VK_NATIVE_TILEBAR read barriers of draws that sample in the fragment stage only wait in the fragment stages (old name
//                        DM_GFX_TILE_BARRIER, field notes FN-32): 0 / 1. Default: Mali / PowerVR 1 (xbg_tile_barrier_default), others 0.
// Sub-switches (A/B of the riskier parts, default on with their switch): DM_VK_NATIVE_LOAD_AREA (a proven part of the target = render
// area + DONT_CARE), DM_VK_NATIVE_LOAD_UBO (positions computed from uniform blocks), DM_VK_NATIVE_STORE_FRAMES (frames of "not read"
// before a store is discarded, default 8 as DM_GFX_STORE_HIST_FRAMES).
#ifndef DM_VKN_POLICY_H
#define DM_VKN_POLICY_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

// = XbgGpuFamily (xbg_vendor_policy.h)
enum { VKN_FAM_OTHER = 0, VKN_FAM_ADRENO = 1, VKN_FAM_MALI = 2, VKN_FAM_POWERVR = 3, VKN_FAM_XCLIPSE = 4, VKN_FAM_MALEOON = 5 };
static inline const char *vkn_policy_family_name(int f) {
    switch (f) { case VKN_FAM_ADRENO: return "adreno"; case VKN_FAM_MALI: return "mali"; case VKN_FAM_POWERVR: return "powervr";
                 case VKN_FAM_XCLIPSE: return "xclipse"; case VKN_FAM_MALEOON: return "maleoon"; default: return "other"; }
}
static inline int vkn_policy_tiler(int f) { return f == VKN_FAM_MALI || f == VKN_FAM_POWERVR; }   // xbg_tile_family
// [mgpu P0] the RENDER-PASS policy's tilers (load / store / fresh / tile barrier / UI pass / UI region / pass join defaults only; shader
// defaults keep vkn_policy_tiler): + Maleoon, + Adreno on Qualcomm's own driver (driverID 8: it picks GMEM by itself). Xclipse and
// Turnip (sysmem on the Mi 14) stay "not a tiler".
static inline int vkn_policy_tile_pass(int f, uint32_t driver_id) { return vkn_policy_tiler(f) || f == VKN_FAM_MALEOON || (f == VKN_FAM_ADRENO && driver_id == 8u); }

// xbg_gpu_family (vendor first, then the device name) + xbg_effective_family (the family bit of a DM_GFX_SIM_GPU profile wins:
// "mali" / "family-mali" -> Mali, "powervr" / "pvr" / "family-powervr" -> PowerVR, "xclipse" / "family-xclipse" -> Xclipse, in that
// order of precedence; separators , ; + space quotes; "0" = nothing).
static inline int vkn_policy_family_from(uint32_t vendor, const char *name, const char *sim) {
    int f = VKN_FAM_OTHER;
    switch (vendor) {
    case 0x5143: f = VKN_FAM_ADRENO; break;
    case 0x13B5: f = VKN_FAM_MALI; break;
    case 0x1010: f = VKN_FAM_POWERVR; break;
    case 0x144D: f = VKN_FAM_XCLIPSE; break;
    case 0x19E5: f = VKN_FAM_MALEOON; break;
    default:
        if (name) {
            if (strstr(name, "Xclipse")) f = VKN_FAM_XCLIPSE;
            else if (strstr(name, "Mali")) f = VKN_FAM_MALI;
            else if (strstr(name, "PowerVR") || strstr(name, "IMG ")) f = VKN_FAM_POWERVR;
            else if (strstr(name, "Maleoon")) f = VKN_FAM_MALEOON;
            else if (strstr(name, "Adreno")) f = VKN_FAM_ADRENO;
        }
    }
    int mali = 0, pvr = 0, xcl = 0;
    for (const char *p = sim; p && *p;) {
        while (*p == ',' || *p == ' ' || *p == ';' || *p == '+' || *p == '"' || *p == '\'') p++;
        const char *e = p; while (*e && *e != ',' && *e != ' ' && *e != ';' && *e != '+' && *e != '"' && *e != '\'') e++;
        const size_t n = (size_t)(e - p);
#define VKN_POL_TOK(s) (n == sizeof(s) - 1 && !strncmp(p, s, n))
        if (VKN_POL_TOK("mali") || VKN_POL_TOK("family-mali")) mali = 1;
        else if (VKN_POL_TOK("powervr") || VKN_POL_TOK("pvr") || VKN_POL_TOK("family-powervr")) pvr = 1;
        else if (VKN_POL_TOK("xclipse") || VKN_POL_TOK("family-xclipse")) xcl = 1;
#undef VKN_POL_TOK
        p = e;
    }
    return mali ? VKN_FAM_MALI : pvr ? VKN_FAM_POWERVR : xcl ? VKN_FAM_XCLIPSE : f;
}

// Defaults by family (see the head comment).
static inline int vkn_policy_load_default(int f) { return vkn_policy_tiler(f) ? 1 : 0; }
static inline int vkn_policy_store_default(int f) { return vkn_policy_tiler(f) ? 1 : 0; }
static inline int vkn_policy_fresh_default(int f) { return vkn_policy_tiler(f) ? 2 : 1; }
static inline int vkn_policy_tilebar_default(int f) { return vkn_policy_tiler(f) ? 1 : 0; }
// An environment value wins when set and inside [lo, hi]; else the default. *from (non-NULL) = 1 when the environment decided.
static inline int vkn_policy_env(const char *name, int def, int lo, int hi, int *from) {
    const char *e = getenv(name);
    if (from) *from = 0;
    if (e && *e) { const int v = atoi(e); if (v >= lo && v <= hi) { if (from) *from = 1; return v; } }
    return def;
}
// The four switches for a family (getenv: install time / shader-library init only, never per draw).
static inline int vkn_policy_load(int f) { return vkn_policy_env("DM_VK_NATIVE_LOAD", vkn_policy_load_default(f), 0, 3, NULL); }
static inline int vkn_policy_store(int f) { return vkn_policy_env("DM_VK_NATIVE_STORE", vkn_policy_store_default(f), 0, 3, NULL); }
static inline int vkn_policy_fresh(int f) { return vkn_policy_env("DM_VK_NATIVE_FRESH", vkn_policy_fresh_default(f), 0, 3, NULL); }
static inline int vkn_policy_tilebar(int f) { return vkn_policy_env("DM_VK_NATIVE_TILEBAR", vkn_policy_tilebar_default(f), 0, 1, NULL); }

// ==== [DOS2] VK-NATIVE stage 5.3 V2: the driver corrections' rules (vkn_drv.c applies them), token for token the old path's
// (xbg_vendor_policy.h; the same switch names, so dm/env, the gear and the safe-mode ladder reach both paths). By policy family:
//   DM_GFX_NOOUT_MASK    colour writes masked off for a fragment shader without colour outputs (Mali writes 0: d2r #179). Not Adreno.
//   DM_GFX_PIPE_SERIAL   one shader-module / pipeline creation at a time (PowerVR's compiler is not thread-safe, PVR-02 / PVR-05). PowerVR.
//   DM_GFX_UBO_DYNAMIC   dynamic uniform buffers (PowerVR: off, the unused-dynamic-UBO overrun of PPSSPP, PVR-06). Not PowerVR. 2 = the
//                        read-only rule anywhere (DOOM's PVR-06 spelling).
//   DM_NR_UBODYN         NR-08b: where dynamic buffers are off, the per-draw blocks a stage reads are dynamic again (support / c1 /
//                        unread ones static). 1 on PowerVR (17.11), 0 elsewhere; 2 = dry run (no effect here), 3 = 0.
// By driverID (VkPhysicalDeviceDriverProperties, Vulkan 1.2; 0 = unknown: nothing applies):
//   DM_GFX_DEPTH_BIAS_X2 depthBiasConstantFactor doubled where the driver halves it: PanVK, RADV (ANGLE doubleDepthBiasConstantFactor).
//   DM_GFX_CLEAR_REBIND  everything bound forgotten after vkCmdClearAttachments: Qualcomm stock, AMD stock, Imagination stock (Skia
//                        mustInvalidatePrimaryCmdBufferStateAfterClearAttachments). Not Turnip.
// DM_GFX_SIM_GPU tokens the backend needs besides the family (xbg_sim_parse names): NOBC (mali, powervr / pvr, nobc), UBDYN8 (ubdyn8),
// NOANISO (powervr / pvr, noaniso).
enum { VKN_DRVID_AMD_PROPRIETARY = 1, VKN_DRVID_MESA_RADV = 3, VKN_DRVID_IMAGINATION_PROPRIETARY = 7, VKN_DRVID_QUALCOMM_PROPRIETARY = 8,
       VKN_DRVID_MESA_TURNIP = 18, VKN_DRVID_MESA_PANVK = 20 };   // = VkDriverId
static inline int vkn_policy_noout_mask_default(int f) { return f != VKN_FAM_ADRENO; }        // xbg_noout_mask_default
static inline int vkn_policy_pipe_serial_default(int f) { return f == VKN_FAM_POWERVR; }      // xbg_pipe_serial_default
static inline int vkn_policy_ubo_dynamic_default(int f) { return f != VKN_FAM_POWERVR; }      // xbg_ubo_dynamic_default
static inline int vkn_policy_ubodyn_default(int f) { return f == VKN_FAM_POWERVR ? 1 : 0; }   // xbg_nr_ubodyn_default
// xbg_nr_ubodyn_active: (DM_NR_UBODYN value or -1 unset, DM_GFX_UBO_DYNAMIC value or -1 unset, the family default on / off, family)
static inline int vkn_policy_ubodyn_active(int nr_ubodyn, int ubo_dynamic_env, int family_default_on, int f) {
    if (ubo_dynamic_env == 2) return 1;
    const int on = ubo_dynamic_env < 0 ? family_default_on : ubo_dynamic_env != 0;
    const int m = nr_ubodyn < 0 ? vkn_policy_ubodyn_default(f) : nr_ubodyn;
    return m == 1 && !on;
}
static inline int vkn_policy_depth_bias_x2_default(uint32_t driver_id) { return driver_id == VKN_DRVID_MESA_PANVK || driver_id == VKN_DRVID_MESA_RADV; }
static inline int vkn_policy_clear_rebind_default(uint32_t driver_id) {
    return driver_id == VKN_DRVID_QUALCOMM_PROPRIETARY || driver_id == VKN_DRVID_AMD_PROPRIETARY || driver_id == VKN_DRVID_IMAGINATION_PROPRIETARY;
}
static inline int vkn_policy_reversed_depth_broken(uint32_t driver_id) { return driver_id == VKN_DRVID_QUALCOMM_PROPRIETARY; }   // (a fact: not emulated)
// xbg_env_or: a set value wins ("0" off, any other number on), else the default. *from (non-NULL) = 1 when the environment decided.
static inline int vkn_policy_env_or(const char *name, int def, int *from) {
    const char *e = getenv(name);
    if (from) *from = e && *e;
    return e && *e ? atoi(e) != 0 : def;
}
// One DM_GFX_SIM_GPU token present (the separators of xbg_sim_parse).
static inline int vkn_policy_sim_token(const char *sim, const char *tok) {
    const size_t tn = strlen(tok);
    for (const char *p = sim; p && *p;) {
        while (*p == ',' || *p == ' ' || *p == ';' || *p == '+' || *p == '"' || *p == '\'') p++;
        const char *e = p; while (*e && *e != ',' && *e != ' ' && *e != ';' && *e != '+' && *e != '"' && *e != '\'') e++;
        if ((size_t)(e - p) == tn && tn && !strncmp(p, tok, tn)) return 1;
        p = e;
    }
    return 0;
}
static inline int vkn_policy_sim_nobc(const char *sim) {   // XBG_SIM_NOBC: the mali / powervr profiles and the nobc token
    return vkn_policy_sim_token(sim, "mali") || vkn_policy_sim_token(sim, "powervr") || vkn_policy_sim_token(sim, "pvr") || vkn_policy_sim_token(sim, "nobc");
}
static inline int vkn_policy_sim_ubdyn8(const char *sim) { return vkn_policy_sim_token(sim, "ubdyn8"); }   // XBG_SIM_UBDYN8 (by name only)
static inline int vkn_policy_sim_noaniso(const char *sim) {   // XBG_SIM_NOANISO: the powervr profile and the noaniso token
    return vkn_policy_sim_token(sim, "powervr") || vkn_policy_sim_token(sim, "pvr") || vkn_policy_sim_token(sim, "noaniso");
}
// ==== [/stage 5.3 V2]

// ==== [VK-DRV] driver compatibility (docs/dos2/nr/VK-DRIVERS.md): the DM_GFX_SIM_GPU tokens of the capability models the backend
// applies itself, token for token xbg_sim_parse's bits (XBG_SIM_NOD24: the xclipse profile and nod24).
static inline int vkn_policy_sim_nod24(const char *sim) { return vkn_policy_sim_token(sim, "xclipse") || vkn_policy_sim_token(sim, "nod24"); }
// DM_VK_NATIVE_FB_EXACT: a framebuffer made with the very render pass it is begun with (not the pass of its compatibility class).
// Vulkan allows any compatible pass; PowerVR stock 1.7-1.10 rejects a framebuffer whose pass had other load / store ops (Dolphin
// BUG_BROKEN_CLEAR_LOADOP_RENDERPASS, KB PVR-03) - and the backend's pass policy makes such twins on tilers. Default: Imagination stock.
static inline int vkn_policy_fb_exact_default(uint32_t driver_id) { return driver_id == VKN_DRVID_IMAGINATION_PROPRIETARY; }
// XBG_SIM_NOVERTEX3 (the xclipse profile, novertex3): no VERTEX_BUFFER on 3-component 8 / 16-bit formats (AMD-style hardware)
static inline int vkn_policy_sim_novertex3(const char *sim) { return vkn_policy_sim_token(sim, "xclipse") || vkn_policy_sim_token(sim, "novertex3"); }
// XBG_SIM_NOFILLMODE (the mali and powervr profiles, nofillmode): fillModeNonSolid = 0
static inline int vkn_policy_sim_nofillmode(const char *sim) {
    return vkn_policy_sim_token(sim, "mali") || vkn_policy_sim_token(sim, "powervr") || vkn_policy_sim_token(sim, "pvr") || vkn_policy_sim_token(sim, "nofillmode");
}
// ==== [/VK-DRV]

#ifdef __cplusplus
}
#endif
#endif
