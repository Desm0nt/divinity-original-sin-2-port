// [DOS2][safe-mode] GPU safe mode for Mali / PowerVR (real families and the DM_GFX_SIM_GPU models), ported from the DOOM 2016 port
// (docs/port-kb gpu-mali.md MALI-13 / gpu-powervr.md PVR-08; its xbg_safe_mode.h + xbg_game.cpp namespace safemode, B&F 2.57) and adapted to
// what DOS2 turns on by default (docs/dos2/nr/SAFE-MODE.md). Pure functions over strings and the family, no device calls and no allocation,
// so tests/safe_mode_test.cpp calls the production code directly and the crash handler may use the classifiers (they only read memory).
//
// A run that ends with a graphics failure on such a GPU (a native crash on a graphics / recording thread - the translation thread
// nvn-translate records every vkCmd* in DOS2, the NB-20b worker dm-nbmt replays them with DM_NB_MT=3 -, in the Vulkan driver's code, or in
// zero-copy memory; a GPU-hang fatal; a VK_ERROR_DEVICE_LOST fatal) adds one to <files>/gfx-safe-mode (xbg_game.cpp xbg_safe_mode_record,
// signal-safe). The next start (xbg_safe_mode_boot, before the Vulkan device exists) turns on the step that count asks for, a switch the
// player set in dm/env always winning (the app's own default table in jni_android.c does not count as the player's: safe mode overrides it):
//   step 1  the risky DOS2 defaults on tilers off: DM_GFX_ZEROCOPY=0 (no-op unless forced: off on tilers already), PowerVR
//           DM_GFX_UBO_DYNAMIC=0 (the PVR-06 crash class; already its default) and DM_NR_UBODYN=0 (NR-08b, default 1 on PowerVR since 17.11),
//           DM_NR_RP=0 (NR-10 barriers by render-pass visibility, P0 default on Mali / PowerVR), DM_DOS2_PREPASS=0 (NR-30 depth pre-pass
//           skip), DM_NR_UBOVEC=0 (NR-27 SPIR-V uniform-load rewrite, default on Mali), DM_NR_ONCHIP=0 (the merged Sky -> Light render pass),
//           DM_NR_LIGHTFUSE=0 / DM_NR_SH_PCF=0 (the 19.11 defaults: fused Sky -> Light -> Fog draw, dref shadow resolve),
//           DM_NR_UIX_DS=0 / DM_NR_UIX_AREA=0 (NR-UIX Iggy pass DS detach / render area, default on the tile families since tiler plan step 1;
//           DM_NR_RP_FRESH, its other step-1 default, needs DM_NR_RP=1 and is off with it); [VK-NATIVE stage 5] the native backend's own
//           techniques (DM_VK_NATIVE_SH / _SH_PCF / _SHCACHE / _LIGHTFUSE / _LIGHTFUSE_FOG / _COND / _GBUF_LE / _COST = 0), [stage 5.3 V3]
//           its recording thread (DM_VK_NATIVE_RT=0) and its later techniques, each at its stage-4 value (DM_VK_NATIVE_POST=0: the
//           Switch post shaders, not the mobile ones (1) nor the fused tail; _UIPASS / _MAT / _XLQ / _PREPASS / _PRESKIN = 0; the pass
//           policy DM_VK_NATIVE_LOAD / _STORE / _FRESH / _TILEBAR = 0: LOAD + STORE everywhere, zero fill + LOAD for fresh targets, full
//           barriers): mode 1 stays on, as stage 4 drew;
//   step 2  [stage 5.3 V3] the native backend's conservative basics, still ON the backend: no pipeline warm-up (DM_VK_NATIVE_WARM=0:
//           no worker, no pipeline-cache file), no GPU BC transcode (DM_VK_NATIVE_BCGPU=0: BC textures without the setup pack decoded
//           on the CPU, no compute decode / ASTC encode), whole-texture mirrors (DM_VK_NATIVE_MIRROR=0, the stage-3 rule), one module /
//           pipeline creation at a time (DM_GFX_PIPE_SERIAL=1; read by the backend too since V2), PowerVR DM_GFX_NOOUT_MASK=0 (DOOM's
//           PVR-07 ladder step 2); the conditional draws are off from step 1 (DM_VK_NATIVE_COND=0). The old path's step-2 rows follow
//           (they act only at step 3, when the old path draws; mode 1 forces those names off anyway):
//           + the tile pass policies (DOOM step 2): DM_GFX_NOLOAD, DM_GFX_STORE_HIST (DOS2's store-DONT_CARE heuristic; DOOM STORE_DEAD /
//           STORE_NONE), DM_GFX_CLEAR_FOLD, DM_GFX_TILE_BARRIER, DM_GFX_LAZY_CLEAR = 0, and their native-backend versions DM_NB_RP (the NB-06
//           planner's load / store / clear folding + pre-pass barrier), DM_NB_OPBARRIER (narrow copy barriers), DM_NB_UIAREA (UI render
//           area) = 0; + the native paths that change what is recorded: DM_NR_DIRECT, DM_NR_DRAW (NR-09 / NR-09R), DM_NR_ROTATE (NR-20:
//           identity swapchain + blit), DM_NR_SH (NR-04 engine shaders by name) = 0;
//   step 3  [stage 5.3 V3] the old path: DM_VK_NATIVE=0 (owner 20.11: only after the backend itself crashed again at step 2) + DOOM's
//           step 3 in DOS2's terms: DM_GFX_PIPE_SERIAL=1 (already from step 2; DOS2's compile lock: modules + pipelines one at a time;
//           DOOM COMPILE_LOCK=2), DM_NB_MT=0 (no vkCmd* recording on the dm-nbmt worker: DOOM's COMPILE_LOCK=2 is about its record thread),
//           PowerVR DM_GFX_NOOUT_MASK=0 (from step 2; DOOM PVR-07 ladder; on Mali the mask is the d2r #179 picture fix, kept),
//           DM_GFX_GS_PASSTHRU=0; + every NR and NB step off (DM_NR=0 with DM_NR_BUF / DESC / HEAT / SEED, DM_NB=0 with each NB step,
//           DM_GFX_PRESENT_THREAD=0): the renderer of the released v0.1.1 (before 852c6ec turned NB on), the last version players ran.
// [stage 5.3 V3] Before V3 step 2 was the old path; the stored state is the same file and the same count (the step is min(count, 3),
// nothing else is read), so a player whose file says count=2 now starts on the backend at its step 2 instead of on the old path; the
// next counted crash (count 3) puts them on the old path. A DM_VK_NATIVE the player set in dm/env is the player's: never replaced.
// DM_SAFE_MODE: unset / auto = the counted step (min(count, 3)); 0 / off = none; on = at least step 1 (the gear's "On"); 1..3 = that step.
// DOS2 never had DOOM's v0.5.0 PowerVR ladder, so there is no DM_PVR_LADDER alias (xbg_safe_step's pvr_ladder argument is passed null).
// [VK-DRV 21.11, owner approved; docs/dos2/nr/VK-DRIVERS.md] A start in mode 1 (DM_VK_NATIVE=1, the default build) gets the ladder on EVERY
// family: steps 1-2 (backend rows) and step 3 (DM_VK_NATIVE=0), counted crashes included; the old path's rows stay Mali / PowerVR only, so on
// Adreno / Xclipse / Maleoon / others step 3 is the old path with its own defaults. The decision is made early (xbg_safe_mode_pre, jni
// nativeStart, before DM_VK_NATIVE's default and the app table are applied and before dm_dos2_vkn_pre), the family-dependent rows at boot.
// A counted step 3 needs the backend's own crash at its step 2 (xbg_safe_counted_gate; the marker's native= key).
#pragma once
#include "xbg_vendor_policy.h"
#include <cstring>
#include <cstdlib>
#include <cstdint>

enum { XBG_SAFE_MAX_STEP = 3 };
// The families the OLD path's rows act on (the policy family: a simulated one included).
inline bool xbg_safe_family(XbgGpuFamily f){return f==XBG_GPU_MALI || f==XBG_GPU_POWERVR;}
// [VK-DRV 21.11, owner approved] With the native backend (DM_VK_NATIVE=1 when the start began, before the ladder's own step 3) the ladder acts
// on EVERY family (Adreno stock / Turnip, Xclipse, Maleoon, others): its backend rows (step 1 techniques + recording thread off, step 2 the
// conservative basics, step 3 DM_VK_NATIVE=0) and the counted crashes. The old path's rows stay Mali / PowerVR only (xbg_safe_switches_mode).
inline bool xbg_safe_eligible(XbgGpuFamily f,bool native){return native || xbg_safe_family(f);}

// How this boot's step was decided (the status string for the app says it in words: xbg_safe_how_name).
enum XbgSafeHow { XBG_SAFE_COUNTED=0, XBG_SAFE_FORCED=1, XBG_SAFE_ON=2, XBG_SAFE_OFF=3, XBG_SAFE_ALIAS=4, XBG_SAFE_NOT_ELIGIBLE=5 };
inline const char *xbg_safe_how_name(int how){
    switch(how){case XBG_SAFE_COUNTED:return "counted";case XBG_SAFE_FORCED:return "forced";case XBG_SAFE_ON:return "on";case XBG_SAFE_OFF:return "off";
    case XBG_SAFE_ALIAS:return "pvr-ladder";default:return "not-eligible";}
}
// This boot's step from DM_SAFE_MODE, the DM_PVR_LADDER alias (PowerVR only; DOS2 passes null) and the counted graphics crashes. DOOM's code.
inline int xbg_safe_step(const char *safe_mode,const char *pvr_ladder,bool powervr,int count,int *how){
    int h=XBG_SAFE_COUNTED,step=count<0?0:count>XBG_SAFE_MAX_STEP?XBG_SAFE_MAX_STEP:count;
    if(safe_mode && *safe_mode && strcmp(safe_mode,"auto")){
        if(!strcmp(safe_mode,"off") || !strcmp(safe_mode,"0")){h=XBG_SAFE_OFF;step=0;}
        else if(!strcmp(safe_mode,"on")){h=XBG_SAFE_ON;if(step<1)step=1;}
        else if(*safe_mode>='1' && *safe_mode<='9'){h=XBG_SAFE_FORCED;step=atoi(safe_mode);if(step>XBG_SAFE_MAX_STEP)step=XBG_SAFE_MAX_STEP;}
        // anything else: counted (an unknown word does not turn safe mode off)
    }
    else if((!safe_mode || !*safe_mode) && powervr && pvr_ladder && *pvr_ladder){
        const int l=atoi(pvr_ladder);h=XBG_SAFE_ALIAS;step=l<=0?0:l==1?2:3;
    }
    if(how)*how=h;
    return step;
}

// The switches of a step on a family, in the order they are set. Returns how many were written to out (at most max).
// native: the start began in mode 1 (DM_VK_NATIVE=1). On Mali / PowerVR the list is the same either way (the backend rows are inert in mode
// 0, the old-path rows inert in mode 1 until step 3 turns the old path on); on any other family it is the backend rows alone, and only in mode 1.
struct XbgSafeSwitch { const char *key, *value; };
inline int xbg_safe_switches_mode(int step,XbgGpuFamily f,bool native,XbgSafeSwitch *out,int max){
    int n=0;auto add=[&](const char *k,const char *v){if(n<max)out[n++]={k,v};};
    if(!xbg_safe_eligible(f,native) || step<=0)return 0;
    const bool pvr=f==XBG_GPU_POWERVR,old=xbg_safe_family(f);   // old: the old path's rows (Mali / PowerVR only, owner 21.11)
    // step 1: zero-copy (DOOM: + ZC3_SELFTEST / FENCE_TRYLOCK, which DOS2 does not have), the dynamic-UBO class on PowerVR, the NR defaults
    // the tile families run since 17.11
    if(old){add("DM_GFX_ZEROCOPY","0");
    if(pvr){add("DM_GFX_UBO_DYNAMIC","0");add("DM_NR_UBODYN","0");}   // DOOM 0.4.22: no dynamic UBOs on PowerVR (the backend reads both since V2); NR-08b is their per-draw form
    add("DM_NR_RP","0");add("DM_DOS2_PREPASS","0");add("DM_NR_UBOVEC","0");add("DM_NR_ONCHIP","0");   // NR-ONCHIP: the merged Sky -> Light pass
    add("DM_NR_LIGHTFUSE","0");add("DM_NR_SH_PCF","0");add("DM_NR_PRESKIN","0");add("DM_NR_IDVS","0");   // 19.11 defaults (fused Sky -> Light -> Fog draw, dref shadows), untried on tilers
    add("DM_NR_UIX_DS","0");add("DM_NR_UIX_AREA","0");}   // tiler plan step 1 (NR-UIX defaults on the tile families); DM_NR_RP=0 above also ends DM_NR_RP_FRESH
    // [DOS2] VK-NATIVE stage 5 (mode 1, docs/dos2/nr/VK-STAGE5-PLAN.md 1.e): the backend's techniques off - named shaders + Dref, shadow cache,
    // fused lighting, conditional rendering / its oracle, LEQUAL G-buffer, the draw-cost shortcuts - so a technique's fault keeps the player
    // on the backend (step 2 is the old path: DM_VK_NATIVE=0). Not here: DM_VK_NATIVE_SHFAR (a quality choice of DM_HACK_SHFAR / _TIER),
    // DM_VK_NATIVE_PROF (measurement). Without effect in modes 0 / 2 (only mode 1 reads them).
    add("DM_VK_NATIVE_SH","0");add("DM_VK_NATIVE_SH_PCF","0");add("DM_VK_NATIVE_SHCACHE","0");add("DM_VK_NATIVE_LIGHTFUSE","0");add("DM_VK_NATIVE_LIGHTFUSE_FOG","0");
    add("DM_VK_NATIVE_COND","0");add("DM_VK_NATIVE_GBUF_LE","0");add("DM_VK_NATIVE_COST","0");
    // [stage 5.3 V3] the recording thread and the later stage-5 switches at their stage-4 values (see the head comment)
    add("DM_VK_NATIVE_RT","0");add("DM_VK_NATIVE_POST","0");add("DM_VK_NATIVE_UIPASS","0");add("DM_VK_NATIVE_MAT","0");add("DM_VK_NATIVE_XLQ","0");
    add("DM_VK_NATIVE_PREPASS","0");add("DM_VK_NATIVE_PRESKIN","0");
    add("DM_VK_NATIVE_LOAD","0");add("DM_VK_NATIVE_STORE","0");add("DM_VK_NATIVE_FRESH","0");add("DM_VK_NATIVE_TILEBAR","0");
    add("DM_VK_NATIVE_TILELIGHT","0");   // [P10] the merged tile pass (docs/dos2/nr/VK-P10.md)
    if(step>=2){
        // [stage 5.3 V3] the backend's conservative basics (still on the backend)
        add("DM_VK_NATIVE_WARM","0");add("DM_VK_NATIVE_BCGPU","0");add("DM_VK_NATIVE_MIRROR","0");add("DM_GFX_PIPE_SERIAL","1");
        if(pvr)add("DM_GFX_NOOUT_MASK","0");
        if(old){   // the old path's step-2 rows (they act at step 3, when the old path draws)
        add("DM_GFX_NOLOAD","0");add("DM_GFX_STORE_HIST","0");add("DM_GFX_CLEAR_FOLD","0");add("DM_GFX_TILE_BARRIER","0");add("DM_GFX_LAZY_CLEAR","0");
        add("DM_NB_RP","0");add("DM_NB_OPBARRIER","0");add("DM_NB_UIAREA","0");
        add("DM_NR_DIRECT","0");add("DM_NR_DRAW","0");add("DM_NR_ROTATE","0");add("DM_NR_SH","0");
        add("DM_NR_Z3","0");add("DM_NR_Z5","0");   // NR-ZERO Z3 / Z5: resolved draws from a table / the packet from the game thread (they sit on NR-09)
        add("DM_NRN","0");add("DM_NRN_LIVE","0");add("DM_NRN_SHC","0");add("DM_NRN_EV","0");   /* [DOS2] NR-NATIVE step E */add("DM_NRN_LIST","0");add("DM_NRN_REC","0");/* [DOS2] NR-NATIVE step D */add("DM_NRN_NOBODY","0");/* [DOS2] NR-NATIVE step D half b */add("DM_NRN_PKT","0");/* [DOS2] NR-NATIVE step D half c */add("DM_NRN_SHORT","0");add("DM_NRN_SHADOW","0");add("DM_NRN_PREPASS","0");add("DM_NRN_GBUF","0");add("DM_NRN_LIGHT","0");add("DM_NRN_POST","0");add("DM_NRN_OTHER","0");   // NR-NATIVE: the card of an engine draw (sits on NR-09)
        add("DM_NR_Z8","0");   // NR-ZERO Z8: the submit walk (a copy of State on the submitting thread, answers and packets from it)
        add("DM_NR_Z8_THREAD","0");   // NR-ZERO Z8, section 28: the walk on its own thread
        add("DM_NR_Z8_IMG","0");      // section 29: the read word of an NR-08 entry
        add("DM_NR_Z8_VIEW","0");     // section 30: views by the range's number in its submission
        add("DM_NR_Z8_VAL","0");      // section 31: the verdict of a tail draw without the walker's chunks
        add("DM_NR_Z3B","0");add("DM_NR_Z3C","0");add("DM_NR_Z3D","0");add("DM_NR_Z3K","0");   // NR-ZERO Z3b / Z3c / Z3d / Z3K: the image word, the mesh view table, the ring word, the cheap key
        // batch C defaults (19.11): shader twins, native state / uniform / translator / texture paths, AA with the outline
        add("DM_NR_SH_SURF","0");add("DM_NR_MAT","0");add("DM_NR_STATE","0");add("DM_NR_UBO","0");add("DM_NR_UBD","0");
        add("DM_NR_TRX_STICKY","0");add("DM_NR_TRX_ZC","0");add("DM_NR_TAPSET","0");add("DM_NR_TEX","0");add("DM_NR_TEXSTREAM","0");
        add("DM_NB_POST_AA_OUTLINE","0");}
    }
    if(step>=3){
        add("DM_VK_NATIVE","0");   // [stage 5.3 V3] the old path (dm/gfx/vkn off); DM_GFX_PIPE_SERIAL=1 / PowerVR NOOUT_MASK=0 are set from step 2
        // [VK-DRV] on Adreno / Xclipse / others the old path draws with its own defaults (the jni table applies its NB / NR rows when the early
        // decision put DM_VK_NATIVE=0 in first: xbg_safe_mode_pre): the renderer those players ran up to v0.2.1. The v0.1.1 rows below are the
        // Mali / PowerVR ladder's.
        if(!old)return n;
        add("DM_NB_MT","0");
        add("DM_GFX_GS_PASSTHRU","0");
        add("DM_NR","0");add("DM_NR_BUF","0");add("DM_NR_DESC","0");add("DM_NR_HEAT","0");add("DM_NR_SEED","0");
        add("DM_NB","0");add("DM_NB_PIPE","0");add("DM_NB_DESC","0");add("DM_NB_UBO","0");add("DM_NB_OWN","0");add("DM_NB_POST","0");add("DM_NB_POST_AA","0");
        add("DM_GFX_PRESENT_THREAD","0");
    }
    return n;
}
// The list of the old-path ladder (a start in mode 0): Mali / PowerVR only, the same rows as before VK-DRV.
inline int xbg_safe_switches(int step,XbgGpuFamily f,XbgSafeSwitch *out,int max){return xbg_safe_switches_mode(step,f,false,out,max);}
inline const char *xbg_safe_step_text(int step){
    switch(step){case 0:return "nothing changed";
    case 1:return "the risky tile-GPU defaults off (zero-copy, PowerVR dynamic UBOs / NR-08b, NR-10 barriers, NR-30 pre-pass skip, NR-27 uniform-load rewrite, NR-ONCHIP merged lighting pass; native backend: its stage-5 techniques, its recording thread, its pass policy - the backend draws as stage 4 did)";
    case 2:return "step 1 + the native backend's conservative basics (no pipeline warm-up, BC textures decoded on the CPU, whole-texture mirrors, one pipeline build at a time, PowerVR colour mask of output-less shaders off), still on the backend; the old path's tile pass policies and native recording paths off for step 3";
    default:return "the old path (DM_VK_NATIVE=0) + step 2 + no worker recording, culling GS kept, every NR / NB step off (the v0.1.1 renderer)";}
}
// [VK-DRV] the same for a mode-1 start on a family without the old path's rows (Adreno, Xclipse, Maleoon, others)
inline const char *xbg_safe_step_text_native(int step){
    switch(step){case 0:return "nothing changed";
    case 1:return "native backend: its stage-5 techniques, its recording thread and its pass policy off - the backend draws as stage 4 did";
    case 2:return "step 1 + the native backend's conservative basics (no pipeline warm-up, BC textures decoded on the CPU, whole-texture mirrors, one pipeline build at a time), still on the backend";
    default:return "the old path (DM_VK_NATIVE=0) with this family's own defaults (the renderer of v0.2.1)";}
}

// The marker file <files>/gfx-safe-mode: "key=value" lines (count, step, pid, time = wall clock ms, family, why). Unknown keys are
// skipped; a file without "count=" is count 0. DOOM's format (frontend-common SafeMode.Marker / DmGuard read the same file).
// [VK-DRV] native=: the renderer of the run that wrote it (1 = the native backend, 0 = the old path, -1 = a file of a build before VK-DRV).
struct XbgSafeMarker { int count=0, step=-1, pid=0, native=-1; long long time_ms=0; char family[16]={}; char why[200]={}; };
inline void xbg_safe_parse(const char *text,XbgSafeMarker *m){
    *m=XbgSafeMarker{};if(!text)return;
    for(const char *p=text;*p;){
        const char *e=p;while(*e && *e!='\n')++e;
        auto val=[&](const char *k)->const char*{size_t n=strlen(k);return size_t(e-p)>n && !strncmp(p,k,n) && p[n]=='='?p+n+1:nullptr;};
        auto copy=[&](const char *v,char *dst,size_t cap){size_t n=size_t(e-v);if(n && v[n-1]=='\r')--n;if(n>=cap)n=cap-1;memcpy(dst,v,n);dst[n]=0;};
        if(const char *v=val("count"))m->count=atoi(v);
        else if(const char *v=val("step"))m->step=atoi(v);
        else if(const char *v=val("pid"))m->pid=atoi(v);
        else if(const char *v=val("native"))m->native=atoi(v)?1:0;
        else if(const char *v=val("time"))m->time_ms=atoll(v);
        else if(const char *v=val("family"))copy(v,m->family,sizeof m->family);
        else if(const char *v=val("why"))copy(v,m->why,sizeof m->why);
        p=*e?e+1:e;
    }
    if(m->count<0)m->count=0;
}

// [VK-DRV] owner 21.11: the counted ladder puts a mode-1 start on the old path (step 3) only when the native backend itself crashed at its
// step 2 (the marker's last failure: native=1, step>=2), or when the old path already ran (step>=3: nothing better to go back to). Otherwise
// a counted step 3 stays at step 2, on the backend. A file of a build before VK-DRV (no native=) keeps the old rule on Mali / PowerVR (their
// count reached 3 under it). DM_SAFE_MODE=3 / the gear decide by themselves (not counted). Returns the step to use.
inline int xbg_safe_counted_gate(int step,int how,bool native,const XbgSafeMarker &m){
    if(how!=XBG_SAFE_COUNTED || step<3 || !native)return step;
    if(m.step>=3 || (m.native==1 && m.step>=2))return step;
    if(m.native<0 && (!strcmp(m.family,"mali") || !strcmp(m.family,"powervr")))return step;
    return 2;
}

// Crash classification (signal handler): a thread of the graphics engine / presenter or one a GPU driver names (Mali "mali-*", IMG
// "pvr*" / "IMG*" / "Vk*", Mesa "tu_*" / "pan*" / "vk*" queue / compiler threads), and the code of a GPU driver library.
// DOS2's own: nvn-translate (the translation thread: every vkCmd* without DM_NB_MT, pipeline builds; the warm-up / seed builders it
// starts inherit its name), dm-nbmt (NB-20b worker: vkCmd* packets), dm-nbx (NB-20b context), dm-nrp (NR-11 present), dm-present / dm-gfx /
// dm-crumbs (as DOOM). Not: xbg-bcdec (CPU BC decode), dm-android / dm-lifecycle / dm-log / guest threads.
inline bool xbg_safe_gfx_thread(const char *t){
    if(!t || !*t)return false;
    static const char *const ours[]={"dm-gfx","dm-record","dm-present","dm-crumbs","dm-acquire","nvn-translate","dm-nbmt","dm-nbx","dm-nrp",
                                     "vkn-"};   // [stage 5.3 V3] the native backend's threads (vkn-record: every vkCmd* of mode 1 with DM_VK_NATIVE_RT)
    for(const char *o:ours)if(!strncmp(t,o,strlen(o)))return true;
    static const char *const drv[]={"mali-","pvr","IMG","Vk","vk","tu_","pan","Mali"};
    for(const char *d:drv)if(!strncmp(t,d,strlen(d)))return true;
    return false;
}
inline bool xbg_safe_gfx_lib(const char *path){
    if(!path || !*path)return false;
    const char *b=strrchr(path,'/');b=b?b+1:path;
    static const char *const libs[]={"vulkan","mali","GLES_","pvr","IMG","srv_um","freedreno","panfrost","panvk","adreno","libgsl","llvm-glnext","libVkLayer"};
    for(const char *l:libs)if(strstr(b,l))return true;
    return false;
}
// An engine FATAL text that names VK_ERROR_DEVICE_LOST: the word, or a result -4 ("failed: -4", "VkResult -4", "result=-4").
inline bool xbg_safe_text_device_lost(const char *t){
    if(!t)return false;
    if(strstr(t,"VK_ERROR_DEVICE_LOST") || strstr(t,"DEVICE_LOST"))return true;
    for(const char *p=strstr(t,"-4");p;p=strstr(p+1,"-4")){
        const char before=p>t?p[-1]:' ',after=p[2];
        if((before==' ' || before=='=' || before=='(' || before==':') && !(after>='0' && after<='9'))return true;
    }
    return false;
}
// [DOS2] An engine FATAL text of a GPU hang: DOS2 has no fence_fatal (DOOM records there); its slot fence timeout goes through xbg_fatalf
// as "... (fence wait timed out), so the device is hung" (xbg_game.cpp recording_slot / poolreset).
inline bool xbg_safe_text_gpu_hang(const char *t){return t && (strstr(t,"device is hung") || strstr(t,"fence wait timed out"));}

// DM_GFX_FAKE_GPU_CRASH=<s>[:rec|lost|hang] (test; Mali / PowerVR incl. DM_GFX_SIM_GPU only): rec = SIGBUS inside the first
// vkCmdEndRenderPass replayed after s seconds (on dm-nbmt with DM_NB_MT=3, else on nvn-translate: the thread that makes the call), lost = a
// VK_ERROR_DEVICE_LOST fatal on the translation thread, hang = the GPU-hang fatal. Returns the kind (0 = none) and *seconds. DOOM's code.
enum { XBG_FAKE_NONE=0, XBG_FAKE_REC=1, XBG_FAKE_LOST=2, XBG_FAKE_HANG=3 };
inline int xbg_safe_fake_parse(const char *e,int *seconds){
    *seconds=0;if(!e || !*e)return XBG_FAKE_NONE;
    const int s=atoi(e);if(s<=0)return XBG_FAKE_NONE;
    *seconds=s;const char *k=strchr(e,':');
    if(!k || !strcmp(k+1,"rec"))return XBG_FAKE_REC;
    if(!strcmp(k+1,"lost"))return XBG_FAKE_LOST;
    if(!strcmp(k+1,"hang"))return XBG_FAKE_HANG;
    return XBG_FAKE_REC;
}
// DM_GFX_FAKE_FALLBACK=<kind>[:<s>][,...] (test, the in-process fallbacks): DOS2 has pipe only (a pipeline build failure after s seconds;
// DOOM's zc / fence fallbacks need its zero-copy self-test and fence guard, which DOS2 does not have). Returns s (0 without ":s") or -1.
inline int xbg_safe_fallback_fake(const char *e,const char *kind){
    if(!e || !kind)return -1;
    const size_t n=strlen(kind);
    for(const char *p=e;*p;){
        while(*p==',' || *p==' ')++p;
        if(!strncmp(p,kind,n) && (p[n]==0 || p[n]==',' || p[n]==':' || p[n]==' '))return p[n]==':'?atoi(p+n+1):0;
        while(*p && *p!=',')++p;
    }
    return -1;
}
// [DOS2] Who decides a switch of the step: the player's own value (dm/env, or a launch environment variable) is kept; a value the app's
// default table set (jni_android.c, setenv without overwrite before the Vulkan start) or none is replaced. Returns 1 = set the step's value,
// 0 = keep. player: 1 = dm/env / launch env set it, 0 = not the player's, -1 = unknown (no JNI tracking: a host build) -> any set value counts
// as the player's (DOOM's rule).
inline int xbg_safe_should_set(const char *have,int player){
    if(player>0)return 0;
    if(player<0)return have?0:1;
    return 1;
}
