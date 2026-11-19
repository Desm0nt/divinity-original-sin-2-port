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
//           DM_NR_RP_FRESH, its other step-1 default, needs DM_NR_RP=1 and is off with it);
//   step 2  + the tile pass policies (DOOM step 2): DM_GFX_NOLOAD, DM_GFX_STORE_HIST (DOS2's store-DONT_CARE heuristic; DOOM STORE_DEAD /
//           STORE_NONE), DM_GFX_CLEAR_FOLD, DM_GFX_TILE_BARRIER, DM_GFX_LAZY_CLEAR = 0, and their native-backend versions DM_NB_RP (the NB-06
//           planner's load / store / clear folding + pre-pass barrier), DM_NB_OPBARRIER (narrow copy barriers), DM_NB_UIAREA (UI render
//           area) = 0; + the native paths that change what is recorded: DM_NR_DIRECT, DM_NR_DRAW (NR-09 / NR-09R), DM_NR_ROTATE (NR-20:
//           identity swapchain + blit), DM_NR_SH (NR-04 engine shaders by name) = 0;
//   step 3  + DOOM's step 3 in DOS2's terms: DM_GFX_PIPE_SERIAL=1 (DOS2's compile lock: modules + pipelines one at a time; DOOM
//           COMPILE_LOCK=2), DM_NB_MT=0 (no vkCmd* recording on the dm-nbmt worker: DOOM's COMPILE_LOCK=2 is about its record thread),
//           PowerVR DM_GFX_NOOUT_MASK=0 (DOOM PVR-07 ladder; on Mali the mask is the d2r #179 picture fix, kept), DM_GFX_GS_PASSTHRU=0;
//           + every NR and NB step off (DM_NR=0 with DM_NR_BUF / DESC / HEAT / SEED, DM_NB=0 with each NB step, DM_GFX_PRESENT_THREAD=0):
//           the renderer of the released v0.1.1 (before 852c6ec turned NB on), the last version players ran.
// DM_SAFE_MODE: unset / auto = the counted step (min(count, 3)); 0 / off = none; on = at least step 1 (the gear's "On"); 1..3 = that step.
// DOS2 never had DOOM's v0.5.0 PowerVR ladder, so there is no DM_PVR_LADDER alias (xbg_safe_step's pvr_ladder argument is passed null).
#pragma once
#include "xbg_vendor_policy.h"
#include <cstring>
#include <cstdlib>
#include <cstdint>

enum { XBG_SAFE_MAX_STEP = 3 };
// The families safe mode acts on (the policy family: a simulated one included).
inline bool xbg_safe_family(XbgGpuFamily f){return f==XBG_GPU_MALI || f==XBG_GPU_POWERVR;}

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
struct XbgSafeSwitch { const char *key, *value; };
inline int xbg_safe_switches(int step,XbgGpuFamily f,XbgSafeSwitch *out,int max){
    int n=0;auto add=[&](const char *k,const char *v){if(n<max)out[n++]={k,v};};
    if(!xbg_safe_family(f) || step<=0)return 0;
    const bool pvr=f==XBG_GPU_POWERVR;
    // step 1: zero-copy (DOOM: + ZC3_SELFTEST / FENCE_TRYLOCK, which DOS2 does not have), the dynamic-UBO class on PowerVR, the NR defaults
    // the tile families run since 17.11
    add("DM_GFX_ZEROCOPY","0");
    if(pvr){add("DM_GFX_UBO_DYNAMIC","0");add("DM_NR_UBODYN","0");}   // DOOM 0.4.22: no dynamic UBOs on PowerVR; NR-08b is their per-draw form
    add("DM_NR_RP","0");add("DM_DOS2_PREPASS","0");add("DM_NR_UBOVEC","0");add("DM_NR_ONCHIP","0");   // NR-ONCHIP: the merged Sky -> Light pass
    add("DM_NR_LIGHTFUSE","0");add("DM_NR_SH_PCF","0");add("DM_NR_PRESKIN","0");add("DM_NR_IDVS","0");   // 19.11 defaults (fused Sky -> Light -> Fog draw, dref shadows), untried on tilers
    add("DM_NR_UIX_DS","0");add("DM_NR_UIX_AREA","0");   // tiler plan step 1 (NR-UIX defaults on the tile families); DM_NR_RP=0 above also ends DM_NR_RP_FRESH
    if(step>=2){
        add("DM_GFX_NOLOAD","0");add("DM_GFX_STORE_HIST","0");add("DM_GFX_CLEAR_FOLD","0");add("DM_GFX_TILE_BARRIER","0");add("DM_GFX_LAZY_CLEAR","0");
        add("DM_NB_RP","0");add("DM_NB_OPBARRIER","0");add("DM_NB_UIAREA","0");
        add("DM_NR_DIRECT","0");add("DM_NR_DRAW","0");add("DM_NR_ROTATE","0");add("DM_NR_SH","0");
        add("DM_NR_Z3","0");add("DM_NR_Z5","0");   // NR-ZERO Z3 / Z5: resolved draws from a table / the packet from the game thread (they sit on NR-09)
        add("DM_NR_Z3B","0");add("DM_NR_Z3C","0");add("DM_NR_Z3D","0");add("DM_NR_Z3K","0");   // NR-ZERO Z3b / Z3c / Z3d / Z3K: the image word, the mesh view table, the ring word, the cheap key
        // batch C defaults (19.11): shader twins, native state / uniform / translator / texture paths, AA with the outline
        add("DM_NR_SH_SURF","0");add("DM_NR_MAT","0");add("DM_NR_STATE","0");add("DM_NR_UBO","0");add("DM_NR_UBD","0");
        add("DM_NR_TRX_STICKY","0");add("DM_NR_TRX_ZC","0");add("DM_NR_TAPSET","0");add("DM_NR_TEX","0");add("DM_NR_TEXSTREAM","0");
        add("DM_NB_POST_AA_OUTLINE","0");
    }
    if(step>=3){
        add("DM_GFX_PIPE_SERIAL","1");add("DM_NB_MT","0");
        if(pvr)add("DM_GFX_NOOUT_MASK","0");
        add("DM_GFX_GS_PASSTHRU","0");
        add("DM_NR","0");add("DM_NR_BUF","0");add("DM_NR_DESC","0");add("DM_NR_HEAT","0");add("DM_NR_SEED","0");
        add("DM_NB","0");add("DM_NB_PIPE","0");add("DM_NB_DESC","0");add("DM_NB_UBO","0");add("DM_NB_OWN","0");add("DM_NB_POST","0");add("DM_NB_POST_AA","0");
        add("DM_GFX_PRESENT_THREAD","0");
    }
    return n;
}
inline const char *xbg_safe_step_text(int step){
    switch(step){case 0:return "nothing changed";
    case 1:return "the risky tile-GPU defaults off (zero-copy, PowerVR dynamic UBOs / NR-08b, NR-10 barriers, NR-30 pre-pass skip, NR-27 uniform-load rewrite, NR-ONCHIP merged lighting pass)";
    case 2:return "step 1 + the tile pass policies off (noload / store-hist / clear fold / tile barrier / lazy clear, NB-06 planner, NB barriers, UI area) and the native recording paths (NR-09 direct / draw, NR-ZERO Z3 / Z3b / Z3c / Z3d / Z5, NR-20 rotate, NR-04 shaders)";
    default:return "step 2 + one pipeline build at a time, no worker recording, PowerVR colour mask of output-less shaders off, culling GS kept, every NR / NB step off (the v0.1.1 renderer)";}
}

// The marker file <files>/gfx-safe-mode: "key=value" lines (count, step, pid, time = wall clock ms, family, why). Unknown keys are
// skipped; a file without "count=" is count 0. DOOM's format (frontend-common SafeMode.Marker / DmGuard read the same file).
struct XbgSafeMarker { int count=0, step=-1, pid=0; long long time_ms=0; char family[16]={}; char why[200]={}; };
inline void xbg_safe_parse(const char *text,XbgSafeMarker *m){
    *m=XbgSafeMarker{};if(!text)return;
    for(const char *p=text;*p;){
        const char *e=p;while(*e && *e!='\n')++e;
        auto val=[&](const char *k)->const char*{size_t n=strlen(k);return size_t(e-p)>n && !strncmp(p,k,n) && p[n]=='='?p+n+1:nullptr;};
        auto copy=[&](const char *v,char *dst,size_t cap){size_t n=size_t(e-v);if(n && v[n-1]=='\r')--n;if(n>=cap)n=cap-1;memcpy(dst,v,n);dst[n]=0;};
        if(const char *v=val("count"))m->count=atoi(v);
        else if(const char *v=val("step"))m->step=atoi(v);
        else if(const char *v=val("pid"))m->pid=atoi(v);
        else if(const char *v=val("time"))m->time_ms=atoll(v);
        else if(const char *v=val("family"))copy(v,m->family,sizeof m->family);
        else if(const char *v=val("why"))copy(v,m->why,sizeof m->why);
        p=*e?e+1:e;
    }
    if(m->count<0)m->count=0;
}

// Crash classification (signal handler): a thread of the graphics engine / presenter or one a GPU driver names (Mali "mali-*", IMG
// "pvr*" / "IMG*" / "Vk*", Mesa "tu_*" / "pan*" / "vk*" queue / compiler threads), and the code of a GPU driver library.
// DOS2's own: nvn-translate (the translation thread: every vkCmd* without DM_NB_MT, pipeline builds; the warm-up / seed builders it
// starts inherit its name), dm-nbmt (NB-20b worker: vkCmd* packets), dm-nbx (NB-20b context), dm-nrp (NR-11 present), dm-present / dm-gfx /
// dm-crumbs (as DOOM). Not: xbg-bcdec (CPU BC decode), dm-android / dm-lifecycle / dm-log / guest threads.
inline bool xbg_safe_gfx_thread(const char *t){
    if(!t || !*t)return false;
    static const char *const ours[]={"dm-gfx","dm-record","dm-present","dm-crumbs","dm-acquire","nvn-translate","dm-nbmt","dm-nbx","dm-nrp"};
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
