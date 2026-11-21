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

#ifdef __cplusplus
}
#endif
#endif
