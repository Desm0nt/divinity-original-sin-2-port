// [DOOM][nonadreno] GPU family detection, per-family defaults and the DM_GFX_SIM_GPU capability simulation
// (docs/research/mali_powervr_impl.md). Pure functions over vendor IDs, names and Vulkan structs, no device calls, so
// tests/vendor_policy_test.cpp calls the production code directly.
//
// Rules:
// - Every non-Adreno path in the engine is chosen from a real capability (feature bit, format feature, limit) where Vulkan
//   offers one; the family only picks defaults for driver behaviour no query reveals (compiler thread safety, dma-buf
//   coherency, colour writes of output-less fragment shaders). On vendor 0x5143 every default below equals the code
//   before this file, so Adreno runs bit-identical unless a DM_GFX_* switch says otherwise.
// - DM_GFX_SIM_GPU narrows what the engine believes the device can do (features, subgroup, format features, a few limits)
//   to what a Mali / PowerVR / Xclipse stock driver reports (vulkan.gpuinfo.org reports 44439 Mali-G720 = Poco X7 Pro,
//   52993 / 52802 PowerVR BXM-8-256, 52657 DXT-48-1536). The device itself keeps every feature it was created with, so
//   using less of it is always valid: this is how the Mali / PowerVR paths are exercised on our Adreno phones.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#if defined(__ANDROID__) || defined(__linux__)
#include <stdio.h>
#endif

enum XbgGpuFamily { XBG_GPU_OTHER=0, XBG_GPU_ADRENO=1, XBG_GPU_MALI=2, XBG_GPU_POWERVR=3, XBG_GPU_XCLIPSE=4, XBG_GPU_MALEOON=5 };

inline const char *xbg_gpu_family_name(XbgGpuFamily f){
    switch(f){case XBG_GPU_ADRENO:return "adreno";case XBG_GPU_MALI:return "mali";case XBG_GPU_POWERVR:return "powervr";
    case XBG_GPU_XCLIPSE:return "xclipse";case XBG_GPU_MALEOON:return "maleoon";default:return "other";}
}

// PCI-style vendor IDs: 0x5143 Qualcomm (also Turnip), 0x13B5 ARM, 0x1010 Imagination, 0x144D Samsung (stock Xclipse),
// 0x19E5 Huawei (Maleoon). RADV on Xclipse reports AMD 0x1002, so the name decides there.
inline XbgGpuFamily xbg_gpu_family(uint32_t vendor_id,const char *device_name){
    switch(vendor_id){
    case 0x5143:return XBG_GPU_ADRENO;
    case 0x13B5:return XBG_GPU_MALI;
    case 0x1010:return XBG_GPU_POWERVR;
    case 0x144D:return XBG_GPU_XCLIPSE;
    case 0x19E5:return XBG_GPU_MALEOON;
    default:break;
    }
    if(device_name){
        if(strstr(device_name,"Xclipse"))return XBG_GPU_XCLIPSE;
        if(strstr(device_name,"Mali"))return XBG_GPU_MALI;
        if(strstr(device_name,"PowerVR") || strstr(device_name,"IMG "))return XBG_GPU_POWERVR;
        if(strstr(device_name,"Maleoon"))return XBG_GPU_MALEOON;
        if(strstr(device_name,"Adreno"))return XBG_GPU_ADRENO;
    }
    return XBG_GPU_OTHER;
}

// DM_GFX_SIM_GPU bits: comma / space separated tokens, or one of the profiles mali / powervr / xclipse (combinable,
// e.g. "mali,nogs"). Unknown tokens are reported through *unknown (first one) and ignored.
enum : uint32_t {
    XBG_SIM_NOBC          =1u<<0,   // no BC1-7 format features, textureCompressionBC=0 (Mali, PowerVR BXM, Maleoon)
    XBG_SIM_NOBC47        =1u<<1,   // no BC4-7 format features (stock Xclipse)
    XBG_SIM_NOFILTER16    =1u<<2,   // R16G16_UNORM / R16G16B16A16_UNORM without linear filtering (Mali r32-r54)
    XBG_SIM_DEPTHNOLINEAR =1u<<3,   // only D16 filters linearly; X8_D24 / D24S8 / D32 / D32S8 / R32_SFLOAT do not (PowerVR 25.x / DXT)
    XBG_SIM_NOD24         =1u<<4,   // no X8_D24 / D24S8 at all (stock Xclipse, AMD-based)
    XBG_SIM_PVRFORMATS    =1u<<5,   // R8G8B8A8_SRGB without storage, E5B9G9R9 sampled only (PowerVR)
    XBG_SIM_SUBGROUP16    =1u<<6,   // subgroup 16 in fragment + compute only, no quad ops in all stages (Mali Valhall / 5th gen)
    XBG_SIM_SUBGROUP1     =1u<<7,   // subgroup size 1 (PowerVR BXM-4-64 DDK 24.2)
    XBG_SIM_NOGS          =1u<<8,   // geometryShader=0 (PanVK, Maleoon, old PowerVR; Arm advises against GS anyway)
    XBG_SIM_NODEPTHBOUNDS =1u<<9,   // depthBounds=0 (every Mali / PowerVR report)
    XBG_SIM_NOFILLMODE    =1u<<10,  // fillModeNonSolid=0 (Mali, PowerVR BXM)
    XBG_SIM_NOCLIP        =1u<<11,  // shaderClipDistance / shaderCullDistance=0 (Mali)
    XBG_SIM_NOGATHER      =1u<<12,  // shaderImageGatherExtended=0 (PowerVR BXM driver 1.15)
    XBG_SIM_NOANISO       =1u<<13,  // samplerAnisotropy=0 (PowerVR BXM driver 1.15)
    XBG_SIM_TEXEL64K      =1u<<14,  // maxTexelBufferElements=65536 (PowerVR)
    XBG_SIM_NOVERTEX3     =1u<<15,  // no VERTEX_BUFFER on 3-component 8/16-bit formats (AMD-style hardware: RADV / Xclipse)
    XBG_SIM_NOSCALED      =1u<<16,  // no VERTEX_BUFFER on any *_USCALED / *_SSCALED format (AMD-style hardware: RADV / stock Xclipse) [vtxscaled]
    XBG_SIM_UBDYN8        =1u<<17,  // maxDescriptorSetUniformBuffersDynamic=8 (Mali, PowerVR, Xclipse: the spec minimum; Adreno 16). Not in the profiles: [DOS2] NR-08 asks for it by name ("mali,ubdyn8")
    XBG_SIM_FAMILY_MALI   =1u<<24,  // the per-family defaults below act as on that family
    XBG_SIM_FAMILY_POWERVR=1u<<25,
    XBG_SIM_FAMILY_XCLIPSE=1u<<26,
};
constexpr uint32_t XBG_SIM_PROFILE_MALI=XBG_SIM_NOBC|XBG_SIM_NOFILTER16|XBG_SIM_SUBGROUP16|XBG_SIM_NODEPTHBOUNDS|XBG_SIM_NOFILLMODE|XBG_SIM_NOCLIP|XBG_SIM_FAMILY_MALI;
// [pvr] + NOANISO (DOOM 2016 BUGS_AND_FIXES 2.47): the only PowerVR a DOOM player ran (moto g64, BXM-8-256 1.15) has samplerAnisotropy=0
// and the game asked aniso 4 anyway (a fatal before DM_GFX_ANISO_FALLBACK), so the profile models it.
constexpr uint32_t XBG_SIM_PROFILE_POWERVR=XBG_SIM_NOBC|XBG_SIM_DEPTHNOLINEAR|XBG_SIM_PVRFORMATS|XBG_SIM_SUBGROUP1|XBG_SIM_NODEPTHBOUNDS|XBG_SIM_NOFILLMODE|XBG_SIM_TEXEL64K|XBG_SIM_NOANISO|XBG_SIM_FAMILY_POWERVR;
constexpr uint32_t XBG_SIM_PROFILE_XCLIPSE=XBG_SIM_NOBC47|XBG_SIM_NOD24|XBG_SIM_NOVERTEX3|XBG_SIM_NOSCALED|XBG_SIM_FAMILY_XCLIPSE;

inline uint32_t xbg_sim_parse(const char *text,char *unknown=nullptr,size_t unknown_bytes=0){
    if(unknown && unknown_bytes)*unknown=0;
    if(!text)return 0;
    static const struct {const char *name;uint32_t bits;} names[]={
        {"mali",XBG_SIM_PROFILE_MALI},{"powervr",XBG_SIM_PROFILE_POWERVR},{"pvr",XBG_SIM_PROFILE_POWERVR},{"xclipse",XBG_SIM_PROFILE_XCLIPSE},
        {"nobc",XBG_SIM_NOBC},{"nobc47",XBG_SIM_NOBC47},{"nofilter16",XBG_SIM_NOFILTER16},{"depthnolinear",XBG_SIM_DEPTHNOLINEAR},
        {"nod24",XBG_SIM_NOD24},{"pvrformats",XBG_SIM_PVRFORMATS},{"subgroup16",XBG_SIM_SUBGROUP16},{"subgroup1",XBG_SIM_SUBGROUP1},
        {"nogs",XBG_SIM_NOGS},{"nodepthbounds",XBG_SIM_NODEPTHBOUNDS},{"nofillmode",XBG_SIM_NOFILLMODE},{"noclip",XBG_SIM_NOCLIP},
        {"nogather",XBG_SIM_NOGATHER},{"noaniso",XBG_SIM_NOANISO},{"texel64k",XBG_SIM_TEXEL64K},{"ubdyn8",XBG_SIM_UBDYN8},{"novertex3",XBG_SIM_NOVERTEX3},{"noscaled",XBG_SIM_NOSCALED},
        {"family-mali",XBG_SIM_FAMILY_MALI},{"family-powervr",XBG_SIM_FAMILY_POWERVR},{"family-xclipse",XBG_SIM_FAMILY_XCLIPSE}};
    uint32_t bits=0;const char *p=text;
    while(*p){
        while(*p==',' || *p==' ' || *p==';' || *p=='+' || *p=='"' || *p=='\'')++p;
        if(!*p)break;
        const char *e=p;while(*e && *e!=',' && *e!=' ' && *e!=';' && *e!='+' && *e!='"' && *e!='\'')++e;
        size_t n=size_t(e-p);bool found=false;
        if(n==1 && *p=='0'){p=e;continue;}   // "0" = off
        for(const auto &k:names)if(strlen(k.name)==n && !strncmp(k.name,p,n)){bits|=k.bits;found=true;break;}
        if(!found && unknown && unknown_bytes && !*unknown){size_t c=n<unknown_bytes-1?n:unknown_bytes-1;memcpy(unknown,p,c);unknown[c]=0;}
        p=e;
    }
    return bits;
}

// The family the per-family defaults use: a simulated family wins over the real one.
inline XbgGpuFamily xbg_effective_family(XbgGpuFamily real,uint32_t sim){
    if(sim&XBG_SIM_FAMILY_MALI)return XBG_GPU_MALI;
    if(sim&XBG_SIM_FAMILY_POWERVR)return XBG_GPU_POWERVR;
    if(sim&XBG_SIM_FAMILY_XCLIPSE)return XBG_GPU_XCLIPSE;
    return real;
}

// Switch precedence of these defaults: an environment value "0" / "1" wins, else the family default.
inline bool xbg_env_or(const char *environment,bool fallback){
    if(environment && *environment)return atoi(environment)!=0;
    return fallback;
}

// [pvr] DM_GFX_ANISO_FALLBACK (DOOM 2016 BUGS_AND_FIXES 2.47, docs/port-kb PVR-04): the anisotropy a sampler may use on this device. NVN
// takes any value up to 16 (Maxwell filters anisotropically); Vulkan needs samplerAnisotropy enabled and maxAnisotropy <=
// maxSamplerAnisotropy. Without the feature the sampler is trilinear / bilinear (1 = off), above the limit it is clamped. A capability
// rule (no family): it only acts where the old code stopped the game ("NVN sampler anisotropy unsupported/disabled/out of range result=-5").
inline float xbg_aniso_allowed(float wanted,bool feature,float max_aniso){
    if(!(wanted>1))return wanted;   // 1 (off), or garbage the converter rejects as before
    if(!feature)return 1;
    if(max_aniso>=1 && wanted>max_aniso)return max_aniso;
    return wanted;
}

// Zero-copy guest memory (AHardwareBuffer BLOB pages imported as VkBuffer memory, CPU-cached pages read by the GPU through IO
// coherency) was proven only on Adreno (work/zc_test). Elsewhere the copy path is the default: a GPU that does not snoop the CPU
// caches would read stale vertices / uniforms (Arm documents ACE-Lite only for some SoCs). DM_GFX_ZEROCOPY=1 forces it on.
// [DOS2] issue #3: zero-copy was off by default on kernels older than 4.14 (0.1.1-test3); the test4 logs showed the crash is the save
// swap (RENAME_EXCHANGE), not zero-copy, so the old-kernel rule is gone (owner 19.11): Adreno on, as before.
inline bool xbg_zero_copy_default(XbgGpuFamily f){return f==XBG_GPU_ADRENO;}
// [DOS2] NR-06 DM_NR_BUF=1 (xbg_nr_buf.inc): a capability rule, not a family one. With zero-copy (Adreno by default) the constant ring is
// bound in place and static vertex / index buffers already bind the guest memory (no copy to save; an immutable copy would only cost
// memory). Without it (Mali - zero-copy through WC dma-buf cost -26 % fps on a real G720, MALI-15 -, PowerVR, Xclipse, MediaTek and
// Adreno with DM_GFX_ZEROCOPY=0) every uniform block and vertex / index range is a snapshot copy: there the ring is mirrored (one copy
// of its increment per submission) and static buffers get immutable copies. DM_NR_BUF_RING / DM_NR_BUF_STATIC override.
inline bool xbg_nr_buf_ring_inplace(bool zero_copy){return zero_copy;}
inline bool xbg_nr_buf_static_default(bool zero_copy){return !zero_copy;}
// [DOS2] NR-08 DM_NR_DESC=1 (xbg_nr_desc.inc): a capability rule too. One VkDescriptorSet per draw on every family (a second set would
// add a vkCmdBindDescriptorSets to ~60 % of the draws: the texture part changes at 626 of 1007 binds a frame, the offsets at all of them).
// Where dynamic uniform buffers are on, a shader set with more uniform blocks than maxDescriptorSetUniformBuffersDynamic (8 on Mali /
// PowerVR / Xclipse, 16 on Adreno: none of DOS2's 6 130 pairs passes 15) gets the split layout (the most-changing blocks dynamic); where
// they are off (PowerVR, xbg_ubo_dynamic_default) every block is a static tuple word and the split is not asked. DM_NR_DESC_SPLIT overrides.
inline bool xbg_nr_desc_split_default(bool ubo_dynamic){return ubo_dynamic;}
// [DOS2] NR-09 DM_NR_DRAW (xbg_nr_draw.inc): the offset folds of the short draw path, the same on every family. Bit 0: an index buffer is
// bound at (offset mod index size) and drawn with firstIndex; bit 1: the per-vertex streams move back k vertices and the draw's
// vertexOffset grows by k. Both are plain Vulkan and never add a command; what they save differs by family: with zero-copy (Adreno) meshes
// sit at guest offsets in ~20 chunks (the index bind repeats inside a chunk, the vertex bind rarely: offsets are not multiples of the
// stride); without it (Mali / PowerVR / Xclipse / MTK: NR-06 static copies) the copies are placed for the folds (index data in its own
// chunks, vertex copies on a multiple of the stride) and both binds repeat for every mesh of a chunk - there a vkCmd* costs 1.2-1.8 us.
// The vertex fold is not applied to a draw with primitive restart (old Mali, Midgard < r22 / Bifrost < r10: restart with 16-bit indices
// and vertexOffset != 0 is broken) nor to a vertex shader that reads gl_VertexIndex / gl_BaseVertex. DM_NR_DRAW_FOLD overrides.
inline int xbg_nr_draw_fold_default(){return 3;}
// [DOS2] NR-21 DM_NR_SORT (docs/dos2/nr/NR-21.md): 0 on every family until the phone runs - the sort saves pipeline binds (Adreno GPU front
// end, Mali / PowerVR driver CPU) but changes the order the depth test sees: front-to-back order the engine kept would be lost (early-Z /
// LRZ efficiency). Per family after the GPASS A/B (Adreno) and a tester log (Mali, PowerVR).
inline int xbg_nr_sort_default(){return 0;}
// [DOS2] NR-ZERO Z3 / Z5 (docs/dos2/nr/NR-ZERO.md §9 / §10): DM_NR_Z3 - an engine draw whose plan, NR-08 entry and pipeline are already
// resolved is recorded from nrz3's table; DM_NR_Z5 - the recorder packet written by the game thread (not built: a design). 0 on every
// family until the phone runs (Z3: =2 on the Mi 14 Ultra, then the A/B; the target is weak CPUs, a GPU-bound phone shows no fps change).
// The app never sets them; safe-mode step 2 forces both to 0.
inline int xbg_nr_z3_default(){return 0;}
inline int xbg_nr_z5_default(){return 0;}
// [DOS2] NR-ZERO Z3b / Z3c / Z3d (docs/dos2/nr/NR-ZERO.md section 13): DM_NR_Z3B - an NR-08 entry whose images were all quiet is not walked
// while the image epoch stands; DM_NR_Z3C - the vertex / index range of a draw from a table under the zero-copy / static-copy epochs;
// DM_NR_Z3D - a ring block in place by one word. 0 on every family until the phone runs (=2 verify first: bad 0, then the A/B). The app
// never sets them; safe-mode step 2 forces them to 0.
inline int xbg_nr_z3b_default(){return 0;}
inline int xbg_nr_z3c_default(){return 0;}
inline int xbg_nr_z3d_default(){return 0;}
// [DOS2] NR-ZERO Z3 cheap key (docs/dos2/nr/NR-ZERO.md section 17): DM_NR_Z3K - the front table of DM_NR_Z3, indexed by the record's key id
// and compared in place with the live state (no plan lookup, gather or hash before the answer). 0 on every family until the phone runs
// (=2 verify first: bad 0, then the A/B of "[nr] draw span"). The app never sets it; safe-mode step 2 forces it to 0.
inline int xbg_nr_z3k_default(){return 0;}
// The PowerVR shader compiler is not thread-safe (forums.imgtec.com t/4334, BXM-8-256 1.15) and builds slowly (t/4280):
// one vkCreate*Pipelines at a time (warm-up threads included). DM_GFX_PIPE_SERIAL.
inline bool xbg_pipe_serial_default(XbgGpuFamily f){return f==XBG_GPU_POWERVR;}
// Graphics-pipeline crash guard (in-flight markers, skip after two crashed boots). Off on Adreno, whose compiler crashes are
// known and handled by the compute guard; on for the rest, where we have never run. DM_GFX_PIPE_GUARD.
inline bool xbg_pipe_guard_default(XbgGpuFamily f){return f!=XBG_GPU_ADRENO;}
// PowerVR writes the address of every dynamic uniform buffer of a set into a table sized for the ones the stage uses: a page
// fault in vkCmdDrawIndexed on BXM-8-256 (PPSSPP PVR_UNUSED_DYNAMIC_UBO_OVERRUN). No dynamic uniform buffers there. DM_GFX_UBO_DYNAMIC.
inline bool xbg_ubo_dynamic_default(XbgGpuFamily f){return f!=XBG_GPU_POWERVR;}
// [DOS2] NR-08b DM_NR_UBODYN (dm/gfx/nr/nr_ubodyn.h, docs/dos2/nr/NR-08b.md): where the line above turns dynamic uniform buffers off,
// 1 makes the per-draw blocks (game uniform blocks = the constant ring, driver blocks) dynamic in the stages whose SPIR-V reads them
// (the overrun needs an UNREAD dynamic buffer; every DOS2 game block is read, 27 211 of 27 211), support / c1 / unread ones stay at fixed
// offsets. Default 0 everywhere until a PowerVR device ran it (no crash in vkCmdDraw*, the same picture); then 1 on PowerVR. The rule
// only matters where the family default is off; on other families it changes nothing (DM_GFX_UBO_DYNAMIC=2 forces it anywhere, for A/B).
// 1 on PowerVR since 17.11 (owner; docs/dos2/nr/SAFE-MODE.md): phone SIM batch 12 (Mi 14 Ultra, DM_GFX_SIM_GPU=powervr, nr-phone-results.md):
// without it NR-08 (DM_NR_DESC) writes ~1000 descriptor sets a frame under the PowerVR policy, with it 3 a minute, the picture equal, check bad 0.
// Not yet on a real PowerVR: GPU safe mode step 1 (xbg_safe_mode.h) turns it off again after a graphics crash there.
inline int xbg_nr_ubodyn_default(XbgGpuFamily f){return f==XBG_GPU_POWERVR?1:0;}
// Is the read-only rule active: (DM_NR_UBODYN value or -1 = unset, DM_GFX_UBO_DYNAMIC value or -1 = unset, the family's default on/off).
// DM_GFX_UBO_DYNAMIC=2 (DOOM 2016's PVR-06 spelling: only blocks the stage reads) forces it; =0 / =1 keep their meaning (0 + the rule
// = DM_NR_UBODYN=1 on a device whose user turned them off; 1 = everything dynamic, the rule is not needed).
inline bool xbg_nr_ubodyn_active(int nr_ubodyn,int ubo_dynamic_env,bool family_default_on,XbgGpuFamily f){
    if(ubo_dynamic_env==2)return true;
    const bool on=ubo_dynamic_env<0?family_default_on:ubo_dynamic_env!=0;
    const int m=nr_ubodyn<0?xbg_nr_ubodyn_default(f):nr_ubodyn;
    return m==1 && !on;}
// A fragment shader that declares no colour output (NVN's depth-only draw) leaves the colour attachments undefined in Vulkan;
// Adreno keeps them, Mali writes 0 (d2r-mobile issue #179, PR #229). Colour writes are masked off for such draws everywhere
// but Adreno (whose pipelines stay as they were). DM_GFX_NOOUT_MASK.
inline bool xbg_noout_mask_default(XbgGpuFamily f){return f!=XBG_GPU_ADRENO;}

// [DOS2][tile] Render-pass policy for tile-based GPUs (docs/dos2/DOS2_PORT.md "Тайловая политика проходов"; field notes R3 / R4 / R7;
// docs/port-kb MALI-03). On Adreno the Mi 14 runs Turnip with TU_DEBUG=sysmem, where load / store ops cost nothing: every default below is
// off there (bit-exact as before). Tiler family as in the DOOM 2016 port: Mali (stock + PanVK) and PowerVR.
inline bool xbg_tile_family(XbgGpuFamily f){return f==XBG_GPU_MALI || f==XBG_GPU_POWERVR;}
// DM_GFX_CLEAR_FOLD: ClearTexture and clear-only passes become loadOp CLEAR of the next pass (Arm: vkCmdClearColorImage outside a pass
// "very expensive", a CLEAR load op free, FN-13; in-pass vkCmdClearAttachments is a per-fragment shader clear on Mali, FN-12). A win on
// any non-Adreno GPU (AMD RDNA: fast clear through the load op), so on for every known non-Adreno family; unknown GPUs keep the old path.
inline int xbg_clear_fold_default(XbgGpuFamily f){return f==XBG_GPU_MALI || f==XBG_GPU_POWERVR || f==XBG_GPU_XCLIPSE || f==XBG_GPU_MALEOON?1:0;}
// DM_GFX_STORE_HIST: STORE DONT_CARE for attachments nothing ever read (FN-11: depth write-back 986 -> 431 MiB/s on Mali). Bandwidth only
// matters on tilers; a wrong prediction is a frame of garbage, so tilers only.
inline int xbg_store_hist_default(XbgGpuFamily f){return xbg_tile_family(f)?1:0;}
// DM_GFX_TILE_BARRIER: fragment-stage-only read barriers and pass dependencies (FN-32: -13 % frame on Mali). DOOM 2016 default.
inline bool xbg_tile_barrier_default(XbgGpuFamily f){return xbg_tile_family(f);}
// DM_GFX_NOLOAD: a pass whose first draw provably overwrites every colour pixel begins with colour loadOp DONT_CARE (first_draw_covers;
// DOOM 2016 MALI-03, known rare false accept -> refusals ported 14.11). Tilers only.
inline bool xbg_noload_default(XbgGpuFamily f){return xbg_tile_family(f);}
// [DOS2] DM_NB_RP (NB-06, docs/dos2/NATIVE_BACKEND_PLAN.md "NB-06 findings"): the planner's per-pass load / store / clear folding and its
// barrier before the pass (OP_X_NB_PASS header, needs DM_NB) instead of CLEAR_FOLD / STORE_HIST / NOLOAD. 0 everywhere until the phone
// confirms it (P3: DM_NB_RP=2 compare, then DM_NB_RP=3 + DM_GFX_SIM_GPU=mali without magenta); then 1 on Mali / PowerVR / Xclipse
// (the CLEAR_FOLD set), still 0 on Adreno (sysmem Turnip: load / store ops cost nothing there, only the barrier would win).
// Note: since 852c6ec the DOS2 JNI block (jni_android.c k_nb_on) sets DM_NB_RP=1 on every family, so on the phone this default is not
// reached; xbg_nr_rp_default below keeps the tile families' fragment-only barriers under it.
inline int xbg_nb_rp_default(XbgGpuFamily){return 0;}
// [DOS2] NR-03 DM_NR_OCC (docs/dos2/nr/NR-03.md, xbg_nr_occ.inc): the switch itself stays 0 everywhere until a phone run; this picks its PATH
// when DM_NR_OCC_PATH is unset. 1 = GPU (VK_EXT_conditional_rendering on the pair's query, copied at the end of the pass that closed it:
// exact, but the copy + barrier make the next pass wait for the previous pass's fragments) on Adreno (Turnip sysmem / stock: a pass boundary
// costs little), Xclipse and unknown GPUs; 2 = PREV (the host skips by the last retired frames, no GPU dependency between passes, but a
// newly revealed object can be drawn up to 2 submissions late) on the tilers Mali / PowerVR and Maleoon, where a mid-frame wait stops the
// binning of pass N+1 from overlapping the fragments of pass N.
inline int xbg_nr_occ_path_default(XbgGpuFamily f){return xbg_tile_family(f) || f==XBG_GPU_MALEOON?2:1;}
// [DOS2] NR-04 DM_NR_SH_PCF (the shadow resolve CascadedPCF_* under DM_NR_SH=1): 0 translated, 1 exact (the Switch algorithm, 9 point
// taps, ~0.6x the translated ALU), 2 mobile (4 textureGather = 2x2 PCF, 2.25x fewer fetches, a smoother edge). 0 on every family until the
// owner's screenshot A/B (docs/dos2/nr/NR-04.md). 3 = dref (PERF-X19 T11: mobile's 4 taps through sampler2DShadow, the hardware compare +
// bilinear 2x2; a device without linear filtering of the atlas format falls back to 2 by itself) on every family since 19.11 (owner, series
// Q / R, Mi 14 Ultra standing, DRS off: +2.6 fps alone, LIGHTFUSE + FOG + dref 87.7 against 82.2 base, refused 0, a smoother shadow edge).
// Safe-mode step 1 sets it back to 0 on the tile families.
inline int xbg_nr_sh_pcf_default(XbgGpuFamily){return 3;}
// [DOS2] NR-04 DM_NR_SH (engine shaders by name from native-nr.xgspack): 1 on every family (owner 17.11). Phone (Mi 14 Ultra, nr4_b2 /
// nr5_b2 =2: 7 of 10 proven bit for bit, refused 0, red 0; nr9_d10 =1: entries replaced 7 of 10, picture = base, GPU 9.47 -> 9.06 ms).
// No pack / a pack that does not match -> refused, the translated shaders as before.
inline int xbg_nr_sh_default(XbgGpuFamily){return 1;}
// [DOS2] N6 DM_HACK_FSR from a tier (DM_HACK_TIER without DM_HACK_FSR): on every family since round 2 (19.11): the tier takes the
// half-precision EASU (FsrEasuH form, mediump), malioc G57 6.62 arithmetic cycles a pixel (fp32 FsrEasuF 10.25, PPFXAA 3.0 texture-bound) -
// about +2.2 ms over the console's FXAA stretch on a G57 MC2 at 1440x810 against ~8-10 ms that 85-75 % saves there. Kept as a switch point:
// 0 here = a family keeps the console's stretch unless DM_HACK_FSR is set. docs/dos2/nr/NR-HACKS.md §3.2.
inline int xbg_hack_fsr_tier_default(XbgGpuFamily){return 1;}
// [DOS2] NR-ONCHIP DM_NR_ONCHIP (docs/dos2/nr/NR-ONCHIP.md §8, xbg_nr_onchip.inc): DeferredSky -> ClusteredLightingWithSSAO in one render pass,
// Sky out 0 read as an input attachment. 0 on every family: Adreno on Turnip runs TU_DEBUG=sysmem (no tile memory: the same bytes move,
// no gain); Mali / PowerVR / Maleoon would gain the model's 25 MB a frame (~0.7-3.5 ms by class), but no tile device has run it yet
// (night 18.11: no phone). Intent: 1 on xbg_tile_family after =3 with DM_GFX_SIM_GPU=mali on the Mi 14 (no magenta) and a tester's
// Mali / PowerVR run (=2 counts, =1 picture equal to =0).
// 19.11 (owner: on for every non-Adreno family): Mi 14 SIM mali V_chk (LIGHTFUSE off) - S2F chain every frame, refused none, model 42 MB a
// frame not moved through DRAM; P_oc3m (=3 poison + SIM mali) picture = base after f788fec. While DM_NR_LIGHTFUSE is on (default) this backend
// is superseded at start (LIGHTFUSE removes the Sky draw itself), so on the tilers it is the fallback when LIGHTFUSE is off.
inline int xbg_nr_onchip_default(XbgGpuFamily f){return xbg_tile_family(f)?1:0;}
// [DOS2] NR-LIGHTFUSE DM_NR_LIGHTFUSE (xbg_nr_lightfuse.inc, docs/dos2/nr/NR-LIGHTFUSE.md): DeferredSky -> ClusteredLightingWithSSAO as one fused
// draw (bit for bit against the chain under round-to-nearest-even R11G11B10F stores; a GPU that truncates differs by one step of the format).
// 1 on every family since 19.11 (owner; series P / Q / R on the Mi 14 Ultra standing, DRS off: +2.8 / +3.2 / +3.0 fps with FOG, refused none,
// clean picture). Safe-mode step 1 sets it to 0 on the tile families (no Mali / PowerVR device has run it yet).
inline int xbg_nr_lightfuse_default(XbgGpuFamily){return 1;}
// [DOS2] NR-ZERO Z1 DM_NR_SHC_MEMO (xbg_nr_shc.inc, NR-ZERO.md section 4): exact memos of the shadow-cache hash lookups, the hash values
// unchanged. 1 since 19.11 (Mi 14 Z_1b, =2 standing 60 s: 1.84 M texture + 1.56 M page-version compares, bad 0; textures 100 %, versions
// 97 % from memo; versions + textures were 0.85 of the 2.4 us hash). Only reached where DM_NR_SHCACHE is on (Adreno by default).
inline int xbg_nr_shc_memo_default(){return 1;}
// [DOS2] NR-LIGHTFUSE round 3 DM_NR_LIGHTFUSE_FOG (DeferredFog inside the fused draw): 1 with DM_NR_LIGHTFUSE since 19.11 (owner, same runs).
inline int xbg_nr_lightfuse_fog_default(XbgGpuFamily){return 1;}
// [DOS2] NR-33B DM_NR_MAT (docs/dos2/nr/NR-33B.md): depth-only material fragment shaders -> the empty shader when provably without
// effect (no kill, or a setup-proven uniform gate + the alpha test passing its constant .w); its pipelines' vertex shader gets NR-32's
// trim at read 0 (NRVS, xlq_twin). Exact by construction; 0 on every family until a phone run (=2 counts, =1 A/B): it has never run on
// any GPU, its exactness rests on per-draw state no host test sees (the pass's attachments, the gate uniform word, pending writebacks),
// it adds a pipeline per (material, depth pass) compiled in the frame on a player's first session, and the alpha-test family needs the
// setup-built native-nr33b.xgspack. Proposal after the Mi 14 check (NR-33B.md "NRVS"): Mali / PowerVR / Maleoon / Xclipse 1 once
// DM_NR_MAT=2 / 1 on the Mi 14 show red 0 and the base picture (the gain there is varyings + discard, est. -1.5..4 ms); Adreno 1 only
// if the Mi 14 A/B measures the shadow / pre-pass gain (est. -0.4..0.7 ms), else 0 (cost: the twin pipelines).
// 19.11 (owner): 1 on Mali / PowerVR - Mi 14 SIM mali V_chk (=2) / V_onm (=1): ~50 % of the depth-only draws swapped, red 0, picture = base.
// Adreno 0: V_mat (=1) 82.8 fps against 82.7 base, GPU 11.02 against 10.88 ms - no gain, and the twin pipelines cost compiles.
inline int xbg_nr_mat_default(XbgGpuFamily f){return xbg_tile_family(f)?1:0;}
// [DOS2] DM_NR_SH_SURF (NR-33C, docs/dos2/nr/NR-33C.md, dm/gfx/nr/nr_surf.h): surface material fragment shaders with a wave-coherent
// early out (setup-made variants, <shaders>/native-nr33c.xgspack). 0 on every family until a phone run (check 4, then 1) shows no red line.
inline int xbg_nr_surf_default(XbgGpuFamily){return 0;}
// [DOS2] DM_NR_SHCACHE (NR-SHCACHE, docs/dos2/nr/NR-SHCACHE.md, dm/gfx/nr/nr_shc.h): shadow cascade layers (static casters drawn once,
// copied back; only the dynamic ones drawn). 1 on Adreno (owner 18.11 after the shc4 phone runs, Mi 14 Ultra / Turnip: verify =2 differ 0 in the
// 3D menu, standing and walking; ABBA standing +2.15 fps, GPU -0.35 ms, submit -0.6 ms - NR-SHCACHE.md 12.x); 0 on the other families until
// a tiler run (copy of the layer on Mali / PowerVR, batch D with DM_GFX_SIM_GPU) says otherwise. dm/env DM_NR_SHCACHE=0 turns it off.
inline int xbg_nr_shcache_default(XbgGpuFamily f){return f==XBG_GPU_ADRENO?1:0;}
// [DOS2] DM_NR_RP (NR-10, docs/dos2/nr/NR-10.md, dm/gfx/nr/nr_rp.h): read barriers and the barrier before an IR pass decided by what a
// render pass's own dependency already makes visible (per-image render-pass points) instead of full memory barriers.
// 1 on the tile families (xbg_tile_family: Mali, PowerVR - the set DM_GFX_TILE_BARRIER is on for; audit P0, docs/dos2/nr/NR-10.md §9):
// the DM_NB_RP=1 the JNI sets for every family puts a full barrier before every IR pass (NB_NP_PREBAR), which undid TILE_BARRIER's
// fragment-only waits (phone log ab7rp3, SIM mali: 43 full barriers a frame before passes); with 1 that barrier is none / fragment-only
// again. Backend only: no other switch needed (DM_NB_RP=0 just leaves the pre-pass barrier out). Adreno, Xclipse, Maleoon, unknown: 0
// until the phone A/B (DM_NR_RP=2 counts, then 1 vs 0 ABBA; Adreno would lose ~60 full barriers a frame, Xclipse / Maleoon never had
// TILE_BARRIER, so the regression does not exist there).
inline int xbg_nr_rp_default(XbgGpuFamily f){return xbg_tile_family(f)?1:0;}
// DM_NR_RP_FRESH: load DONT_CARE for colour attachments whose guest storage another render target was drawn into since (the engine's
// shared RT pool); only with DM_NR_RP=1, only passes the NB plan begins (nbrp_begin). 1 on the tile families (xbg_tile_family: Mali,
// PowerVR - the DM_GFX_TILE_BARRIER set, which DM_GFX_SIM_GPU=mali also selects; tiler plan step 1, owner 18.11): batch D phone run
// D_rp3 (Mi 14 Ultra, DM_NR_RP=3 + DM_GFX_SIM_GPU=mali, walking in Fort Joy): 14.1 fresh attachments a frame begun with load DONT_CARE
// and poisoned magenta, 0 magenta pixels on the screenshots, 0 red lines - ~60 MB of loads a frame a tiler no longer does.
// Adreno / Xclipse / Maleoon / unknown: 0 (Turnip sysmem: a load costs nothing; the others never ran it). dm/env DM_NR_RP_FRESH=0 turns it off.
inline bool xbg_nr_rp_fresh_default(XbgGpuFamily f){return xbg_tile_family(f);}
// [DOS2] DM_NR_COVER (NR-10b, docs/dos2/nr/NR-10b.md, dm/gfx/nr/nr_cover.h): the first-draw coverage proof for vertex shaders that compute the
// position (every DOS2 post pass): whole target -> load DONT_CARE, a part (dynamic resolution) -> render area + load DONT_CARE. 0 everywhere
// until a phone run (=2 counts, =3 poison); then 1 on the tile families (Mali / PowerVR / Xclipse: a load is a full-target read per pass).
// Adreno: Turnip sysmem loads nothing, gmem would gain like a tiler - decide by the A/B, not here.
inline int xbg_nr_cover_default(XbgGpuFamily){return 0;}
// [DOS2] NR-UBWC DM_NR_UBWC (xbg_nr_ubwc.inc, docs/dos2/nr/NR-UBWC.md): 0 on every family - the count (=2) runs first; vulkan_present.cpp enables
// VK_EXT_image_compression_control from the env value only, so a non-zero family default here would ask a driver without the extension.
inline int xbg_nr_ubwc_default(XbgGpuFamily){return 0;}
// [DOS2] NR-UIX (docs/dos2/nr/NR-UIX.md, xbg_nr_uix.inc): Iggy UI passes. which: 0 DM_NR_UIX_DS (depth-stencil detach by ordinal pass keys),
// 1 DM_NR_UIX_AREA (render area from the vertex boxes, NB-plan passes too), 2 DM_NR_UIX_RB / 3 DM_NR_UIX_CACHE (counting only).
// DS and AREA = 1 on the tile families (xbg_tile_family: Mali, PowerVR - the DM_GFX_TILE_BARRIER set, which DM_GFX_SIM_GPU=mali also
// selects; tiler plan step 1, owner 18.11), where every Iggy pass loads and stores its whole 2400x1080 colour + D24S8 target: batch D
// phone run D_uix1 (Mi 14 Ultra, DM_NR_UIX_DS=1 DM_NR_UIX_AREA=1 + DM_GFX_SIM_GPU=mali, walking in Fort Joy): 18.8 Iggy passes a frame
// begun without their depth-stencil, the area narrowed in 9.0 passes (83 MB), re-attached 0.012, the UI on the screenshot the same,
// 0 red lines (D_uix2 counted 373 MB DS load+store and 817 MB of area a frame on the tiler model). Adreno (Turnip sysmem: ~0 either way),
// Xclipse, Maleoon, unknown: 0 until an A/B. RB / CACHE (counting only): 0 everywhere. dm/env DM_NR_UIX_DS=0 / DM_NR_UIX_AREA=0 turn them off.
inline int xbg_nr_uix_default(XbgGpuFamily f,int which){return (which==0 || which==1) && xbg_tile_family(f)?1:0;}
// [DOS2] DM_NR_UBOVEC (NR-27, docs/dos2/nr/NR-27.md, xbg_spirv_ubovec.cpp): scalar uniform-block reads of one vec4 with a computed index
// (the translator's LDC with a computed address: a material's light loop, skinning) merged into one vec4 load. Exact for every input.
// On for Mali: Arm's offline compiler gives -30..-37 % cycles on the 149 fragment and 2,062 vertex shaders of the DOS2 pack it
// changes (G52 / G57 / G715), none dearer; that compiler merges only loads that are word for word the same. Off on Adreno: Turnip's
// NIR vectorizes the loads itself (nir_opt_load_store_vectorize), so it could only cost load time. PowerVR / Xclipse / Maleoon: no
// offline compiler to measure with, 0 until a device shows it (DM_NR_UBOVEC=1 in dm/env, or DM_GFX_SIM_GPU=family-mali for an A/B).
inline int xbg_nr_ubovec_default(XbgGpuFamily f){return f==XBG_GPU_MALI?1:0;}
// [DOS2] DM_NR_XLQ (NR-32, docs/dos2/nr/NR-32.md, xbg_spirv_vsout.cpp): per pipeline, the vertex shader's stores to output components the
// fragment shader never reads are dropped (37 % of the components the translated vertex shaders of the DOS2 pack write; Fort Joy 40 %).
// Exact: only those stores go. On for Mali: Arm's offline compiler compiles the vertex stage alone and keeps every output - Fort Joy's
// vertex shaders, weighted by their vertices a frame, -12.0 % (G57) / -14.4 % (G52) / -11.8 % (G715) / -9.9 % (G720) cycles, all in the
// load/store pipeline that bounds them. On for PowerVR / Maleoon too (no offline compiler; tile-based, so every varying a vertex writes
// goes to memory and back; the trim cannot add GPU work, it costs one vertex module and pipeline per new pairing). Off on Adreno (Turnip
// links the stages: nir_remove_unused_varyings) and Xclipse (AMD's pipeline compiler links them too): there it would only cost compiles.
inline int xbg_nr_xlq_default(XbgGpuFamily f){return f==XBG_GPU_MALI || f==XBG_GPU_POWERVR || f==XBG_GPU_MALEOON?1:0;}
// [DOS2] T2 DM_NR_IDVS (xbg_spirv_idvs.cpp, docs/dos2/nr/NR-IDVS.md): the translator's gl_PointSize = 1.0 store dropped from vertex shaders
// (kept for point / tessellation / geometry pipelines). malioc: Mali binning (position shader) -21 % weighted on Valhall / 5th gen, and IDVS
// back on Bifrost (G52: whole VS for every vertex -> position part only, -75 %). 0 everywhere until a real Mali run confirms (owner rule:
// tile families only when proven); then Mali (and PowerVR / Maleoon if they gain) -> 1.
// 19.11 (owner: on for Mali and every family it helps): Mi 14 N_idvs2 / N_idvs1m (=1 + SIM mali) check_bad 0, point_bad 0, 2 732 stores dropped,
// picture = base. On Mali, PowerVR and Maleoon (the tilers that store vertex outputs per tile: one output less per vertex); 0 on Adreno /
// Xclipse (their compilers drop an unread output anyway).
inline int xbg_nr_idvs_default(XbgGpuFamily f){return f==XBG_GPU_MALI || f==XBG_GPU_POWERVR || f==XBG_GPU_MALEOON?1:0;}
// [DOS2] DM_NR_PRESKIN (NR-PRESKIN, docs/dos2/nr/NR-PRESKIN.md, xbg_spirv_preskin.cpp / xbg_nr_preskin.inc): skinning vertex shaders cut at
// load; the bone blend runs once per mesh instance and submission (a vertex-only slice in the prologue), every pass's draw reads the record.
// Proved bit-exact under SPIR-V semantics on the pack (interpreter, 0 differences). malioc G57: the depth passes' VS 37 -> 9 LS cycles, the
// G-buffer's 45.5 -> 20, slice 18-21 a pre-skinned vertex (model: Fort Joy G57 MC2 about -1.5 ms). Adreno 750: the whole skinning is
// 0.05-0.14 ms (NR-NSHADER), so off there for good (the slice + record traffic would cost more). 0 everywhere until a real Mali / PowerVR run.
// 19.11 (owner): 1 on Mali / PowerVR - Mi 14 SIM mali V_chk (=2) / V_onm (=1): 50 skinned depth-only draws a frame applied, 48 records
// (1.4 MB), picture = base (character shadows), red 0; G57 model -1.8 ms. Adreno stays 0 (the slice would cost more than the 0.05 ms skinning).
inline int xbg_nr_preskin_default(XbgGpuFamily f){return xbg_tile_family(f)?1:0;}
// [DOS2] DM_DOS2_PREPASS (NR-30, docs/dos2/nr/NR-30.md, xbg_prepass.h): the opaque draws of the scene depth pre-pass are not drawn, their
// G-buffer draws lay the depth down themselves (LEQUAL + write), gated by the previous frame's verdict per mesh. On for the tile families
// (Mali / PowerVR): each skipped draw saves its vertex shading + binning in the pre-pass and the whole translation of the draw on the CPU,
// and the G-buffer overdraw the LEQUAL draws can add is what Mali's forward pixel kill / PowerVR's hidden surface removal take away.
// Off on Adreno (sysmem Turnip, no such early kill: the extra G-buffer fragments cost) and on Xclipse / Maleoon / unknown until an A/B.
inline int xbg_dos2_prepass_default(XbgGpuFamily f){return xbg_tile_family(f)?1:0;}
// [DOS2] NR-31 DM_NR_SEED (xbg_nr_seed.inc, docs/dos2/nr/NR-31.md): the portable warm-up seed <shaders>/dos2-pipe-seed.bin, re-targeted to
// this device, built in the background once per seed and added to its warm-up list. On for every family: a draw takes a seed pipeline
// only on an exact create-info match (warm::take), a record the expander cannot re-target exactly is refused (never built), and
// without a seed file in the APK nothing happens. 2 = check (nothing built, in-frame builds compared).
inline int xbg_nr_seed_default(){return 1;}
// [DOS2] DM_NB_UIAREA (NB-18b-6, docs/dos2/NATIVE_BACKEND_PLAN.md "NB-18b-6 findings", xbg_nb_uiarea.inc): Iggy UI passes begin with the
// union of their draws as render area (grow-only, one restart) and without a depth-stencil no draw uses. No default here on purpose:
// stays 0 (read from env only) until the phone run P7 (=2 counts, then =1 with DM_GFX_SIM_GPU=mali: same picture, gpu_ms); after that
// the candidate default is 1 on Mali / PowerVR / Xclipse (tilers: load / store restricted to the area), 0 on Adreno (sysmem Turnip).

// [DOS2][R14] Driver quirks no query reveals, keyed by VkPhysicalDeviceDriverProperties::driverID (0 = unknown: nothing applies)
// (docs/port-kb/mobile-gpu-field-notes.md §4 and R14). Each is a default; its DM_GFX_* switch ("0" / "1") wins.
// PanVK and RADV apply half of depthBiasConstantFactor (spec ambiguity; ANGLE doubleDepthBiasConstantFactor): the constant
// factor is doubled there. DM_GFX_DEPTH_BIAS_X2.
inline bool xbg_depth_bias_x2_default(uint32_t driver_id){return driver_id==VK_DRIVER_ID_MESA_PANVK || driver_id==VK_DRIVER_ID_MESA_RADV;}
// vkCmdClearAttachments disturbs the bound vertex / index buffers on the Qualcomm stock driver (and AMD, IMG with MSAA; Skia
// mustInvalidatePrimaryCmdBufferStateAfterClearAttachments): the next draw faults or reads the wrong streams. After a clear the
// engine forgets what it bound, so every draw after it binds again. Turnip (MESA_TURNIP) is not affected. DM_GFX_CLEAR_REBIND.
inline bool xbg_clear_rebind_default(uint32_t driver_id){
    return driver_id==VK_DRIVER_ID_QUALCOMM_PROPRIETARY || driver_id==VK_DRIVER_ID_AMD_PROPRIETARY || driver_id==VK_DRIVER_ID_IMAGINATION_PROPRIETARY;
}
// The Qualcomm stock driver draws viewports with minDepth > maxDepth wrong (all versions; Dolphin BUG_BROKEN_REVERSED_DEPTH_RANGE).
inline bool xbg_reversed_depth_broken(uint32_t driver_id){return driver_id==VK_DRIVER_ID_QUALCOMM_PROPRIETARY;}

// [vtxscaled] 1 = a *_SSCALED format, 2 = a *_USCALED one, 0 = neither (vertex-only formats: integer data fetched as float).
inline uint32_t xbg_vk_format_scaled(VkFormat f){
    switch(f){
    case VK_FORMAT_R8_USCALED:case VK_FORMAT_R8G8_USCALED:case VK_FORMAT_R8G8B8_USCALED:case VK_FORMAT_B8G8R8_USCALED:case VK_FORMAT_R8G8B8A8_USCALED:
    case VK_FORMAT_B8G8R8A8_USCALED:case VK_FORMAT_A8B8G8R8_USCALED_PACK32:case VK_FORMAT_A2R10G10B10_USCALED_PACK32:case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
    case VK_FORMAT_R16_USCALED:case VK_FORMAT_R16G16_USCALED:case VK_FORMAT_R16G16B16_USCALED:case VK_FORMAT_R16G16B16A16_USCALED:return 2;
    case VK_FORMAT_R8_SSCALED:case VK_FORMAT_R8G8_SSCALED:case VK_FORMAT_R8G8B8_SSCALED:case VK_FORMAT_B8G8R8_SSCALED:case VK_FORMAT_R8G8B8A8_SSCALED:
    case VK_FORMAT_B8G8R8A8_SSCALED:case VK_FORMAT_A8B8G8R8_SSCALED_PACK32:case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
    case VK_FORMAT_R16_SSCALED:case VK_FORMAT_R16G16_SSCALED:case VK_FORMAT_R16G16B16_SSCALED:case VK_FORMAT_R16G16B16A16_SSCALED:return 1;
    default:return 0;}
}
inline bool xbg_sim_format_in(VkFormat f,VkFormat lo,VkFormat hi){return int(f)>=int(lo) && int(f)<=int(hi);}
// Narrow the format features the driver reported to what the simulated device reports.
inline void xbg_sim_format_features(uint32_t sim,VkFormat f,VkFormatProperties *p){
    if(!sim || !p)return;
    const VkFormatFeatureFlags linear=VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT;
    if((sim&XBG_SIM_NOBC) && xbg_sim_format_in(f,VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK))*p=VkFormatProperties{};
    if((sim&XBG_SIM_NOBC47) && xbg_sim_format_in(f,VK_FORMAT_BC4_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK))*p=VkFormatProperties{};
    if((sim&XBG_SIM_NOFILTER16) && (f==VK_FORMAT_R16G16_UNORM || f==VK_FORMAT_R16G16B16A16_UNORM)){p->optimalTilingFeatures&=~linear;p->linearTilingFeatures&=~linear;}
    if((sim&XBG_SIM_DEPTHNOLINEAR) && (f==VK_FORMAT_X8_D24_UNORM_PACK32 || f==VK_FORMAT_D24_UNORM_S8_UINT || f==VK_FORMAT_D32_SFLOAT ||
        f==VK_FORMAT_D32_SFLOAT_S8_UINT || f==VK_FORMAT_R32_SFLOAT)){p->optimalTilingFeatures&=~linear;p->linearTilingFeatures&=~linear;}
    if((sim&XBG_SIM_NOD24) && (f==VK_FORMAT_X8_D24_UNORM_PACK32 || f==VK_FORMAT_D24_UNORM_S8_UINT))*p=VkFormatProperties{};
    if((sim&XBG_SIM_NOVERTEX3) && (xbg_sim_format_in(f,VK_FORMAT_R8G8B8_UNORM,VK_FORMAT_R8G8B8_SRGB) || xbg_sim_format_in(f,VK_FORMAT_R16G16B16_UNORM,VK_FORMAT_R16G16B16_SFLOAT)))
        p->bufferFeatures&=~VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if((sim&XBG_SIM_NOSCALED) && xbg_vk_format_scaled(f))p->bufferFeatures&=~VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if(sim&XBG_SIM_PVRFORMATS){
        if(f==VK_FORMAT_R8G8B8A8_SRGB)p->optimalTilingFeatures&=~(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT);
        if(f==VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)p->optimalTilingFeatures&=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    }
}
inline void xbg_sim_features(uint32_t sim,VkPhysicalDeviceFeatures *f){
    if(!sim || !f)return;
    if(sim&(XBG_SIM_NOBC))f->textureCompressionBC=VK_FALSE;
    if(sim&XBG_SIM_NOGS)f->geometryShader=VK_FALSE;
    if(sim&XBG_SIM_NODEPTHBOUNDS)f->depthBounds=VK_FALSE;
    if(sim&XBG_SIM_NOFILLMODE)f->fillModeNonSolid=VK_FALSE;
    if(sim&XBG_SIM_NOCLIP){f->shaderClipDistance=VK_FALSE;f->shaderCullDistance=VK_FALSE;}
    if(sim&XBG_SIM_NOGATHER)f->shaderImageGatherExtended=VK_FALSE;
    if(sim&XBG_SIM_NOANISO)f->samplerAnisotropy=VK_FALSE;
}
inline void xbg_sim_subgroup(uint32_t sim,VkPhysicalDeviceSubgroupProperties *s){
    if(!sim || !s)return;
    if(sim&XBG_SIM_SUBGROUP16){s->subgroupSize=16;s->supportedStages&=VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT;s->quadOperationsInAllStages=VK_FALSE;}
    if(sim&XBG_SIM_SUBGROUP1)s->subgroupSize=1;
}
inline void xbg_sim_limits(uint32_t sim,VkPhysicalDeviceLimits *l){
    if(!sim || !l)return;
    if((sim&XBG_SIM_TEXEL64K) && l->maxTexelBufferElements>65536)l->maxTexelBufferElements=65536;
    if((sim&XBG_SIM_UBDYN8) && l->maxDescriptorSetUniformBuffersDynamic>8)l->maxDescriptorSetUniformBuffersDynamic=8;
}

// The 4-component format a 3-component vertex format widens to (same channel type), or VK_FORMAT_UNDEFINED. Only used when the
// driver lacks VERTEX_BUFFER on the 3-component one (none of the Mali / PowerVR reports we checked; AMD-style hardware lacks the
// 8/16-bit ones, cf. yuzu's GetFormatAlternatives). The fourth channel then reads the next bytes of the vertex instead of 1.
inline VkFormat xbg_vertex_widen(VkFormat f){
    switch(f){
    case VK_FORMAT_R8G8B8_UNORM:return VK_FORMAT_R8G8B8A8_UNORM;case VK_FORMAT_R8G8B8_SNORM:return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT:return VK_FORMAT_R8G8B8A8_UINT;case VK_FORMAT_R8G8B8_SINT:return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_UNORM:return VK_FORMAT_R16G16B16A16_UNORM;case VK_FORMAT_R16G16B16_SNORM:return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT:return VK_FORMAT_R16G16B16A16_UINT;case VK_FORMAT_R16G16B16_SINT:return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R32G32B32_UINT:return VK_FORMAT_R32G32B32A32_UINT;case VK_FORMAT_R32G32B32_SINT:return VK_FORMAT_R32G32B32A32_SINT;
    case VK_FORMAT_R32G32B32_SFLOAT:return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:return VK_FORMAT_UNDEFINED;
    }
}

// Bytes per texel of the uncompressed formats texel buffers use (0 = unknown: no clamp).
inline uint32_t xbg_vk_texel_bytes(VkFormat f){
    switch(f){
    case VK_FORMAT_R8_UNORM:case VK_FORMAT_R8_SNORM:case VK_FORMAT_R8_UINT:case VK_FORMAT_R8_SINT:return 1;
    case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_R8G8_SNORM:case VK_FORMAT_R8G8_UINT:case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16_UNORM:case VK_FORMAT_R16_SNORM:case VK_FORMAT_R16_UINT:case VK_FORMAT_R16_SINT:case VK_FORMAT_R16_SFLOAT:return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R8G8B8A8_SNORM:case VK_FORMAT_R8G8B8A8_UINT:case VK_FORMAT_R8G8B8A8_SINT:case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_A2B10G10R10_UNORM_PACK32:case VK_FORMAT_A2B10G10R10_UINT_PACK32:case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_UNORM:case VK_FORMAT_R16G16_SNORM:case VK_FORMAT_R16G16_UINT:case VK_FORMAT_R16G16_SINT:case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_UINT:case VK_FORMAT_R32_SINT:case VK_FORMAT_R32_SFLOAT:return 4;
    case VK_FORMAT_R16G16B16A16_UNORM:case VK_FORMAT_R16G16B16A16_SNORM:case VK_FORMAT_R16G16B16A16_UINT:case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:case VK_FORMAT_R32G32_UINT:case VK_FORMAT_R32G32_SINT:case VK_FORMAT_R32G32_SFLOAT:return 8;
    case VK_FORMAT_R32G32B32_UINT:case VK_FORMAT_R32G32B32_SINT:case VK_FORMAT_R32G32B32_SFLOAT:return 12;
    case VK_FORMAT_R32G32B32A32_UINT:case VK_FORMAT_R32G32B32A32_SINT:case VK_FORMAT_R32G32B32A32_SFLOAT:return 16;
    default:return 0;
    }
}

// Bytes of a texel buffer view the device can address: at most maxTexelBufferElements texels (PowerVR: 65536). Returns the
// clamped range, rounded down to whole texels; bytes itself when it fits or the texel size is unknown.
inline uint64_t xbg_texel_range_clamp(uint64_t bytes,uint32_t texel_bytes,uint32_t max_elements){
    if(!texel_bytes || !max_elements)return bytes;
    const uint64_t cap=uint64_t(max_elements)*texel_bytes;
    return bytes>cap?cap:bytes;
}
