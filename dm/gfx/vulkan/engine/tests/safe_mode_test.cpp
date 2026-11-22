// [DOS2][safe-mode] xbg_safe_mode.h (docs/dos2/nr/SAFE-MODE.md; ported from the DOOM 2016 port's tests/safe_mode_test.cpp, B&F 2.57): the step
// decision, the DOS2 switches per step and family, the marker file, the crash classifiers, the test-trigger parsers, the player rule.
// Host build, e.g. Windows clang++ or WSL:
//   clang++ -std=c++17 -Wall -idirafter <ndk>/toolchains/llvm/prebuilt/<host>/sysroot/usr/include -o safe_mode_test safe_mode_test.cpp
#include <cstdio>
#include <cstring>
#include <string>
#include <vulkan/vulkan.h>
#include "../xbg_safe_mode.h"
#include "../../../vkn/vkn_policy.h"   // [stage 5.3 V3] the backend's readers of the ladder's switches (pure C, header only)

static int fails=0;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++fails;}}while(0)

static std::string sw(int step,XbgGpuFamily f){XbgSafeSwitch s[128];const int n=xbg_safe_switches(step,f,s,128);std::string o;for(int i=0;i<n;i++){o+=s[i].key;o+="=";o+=s[i].value;o+=" ";}return o;}
static bool has(const std::string &s,const char *kv){return s.find(std::string(kv)+" ")!=std::string::npos;}
static int count_key(const std::string &s,const char *k){int c=0;for(size_t p=s.find(k);p!=std::string::npos;p=s.find(k,p+1))if((p==0 || s[p-1]==' ') && s[p+strlen(k)]=='=')++c;return c;}

// [stage 5.3 V3] the ladder in mode 1, applied: the switches of a step put into the environment as xbg_safe_mode_boot does (no player key),
// then read by the backend's own readers (dm/gfx/vkn/vkn_policy.h: the pass policy, the driver corrections of V2) - each step changes what it
// promises; and the C rules of vkn_policy.h block V2 equal xbg_vendor_policy.h's for every family / driverID / input.
static const char *const k_v3_keys[]={"DM_VK_NATIVE_LOAD","DM_VK_NATIVE_STORE","DM_VK_NATIVE_FRESH","DM_VK_NATIVE_TILEBAR","DM_GFX_UBO_DYNAMIC","DM_NR_UBODYN",
    "DM_GFX_PIPE_SERIAL","DM_GFX_NOOUT_MASK","DM_GFX_DEPTH_BIAS_X2","DM_GFX_CLEAR_REBIND","DM_VK_NATIVE","DM_VK_NATIVE_RT","DM_VK_NATIVE_WARM","DM_VK_NATIVE_BCGPU"};
static void v3_clear(){for(const char *k:k_v3_keys)unsetenv(k);XbgSafeSwitch a[128];const int n=xbg_safe_switches(3,XBG_GPU_POWERVR,a,128);for(int i=0;i<n;i++)unsetenv(a[i].key);}
static void v3_apply(int step,XbgGpuFamily f){v3_clear();XbgSafeSwitch a[128];const int n=xbg_safe_switches(step,f,a,128);for(int i=0;i<n;i++)if(xbg_safe_should_set(getenv(a[i].key),0))setenv(a[i].key,a[i].value,1);}
static int v3_ubo_dyn(int f){const char *u=getenv("DM_GFX_UBO_DYNAMIC"),*e=getenv("DM_NR_UBODYN");const int uenv=u && *u?atoi(u):-1;int m=e && *e?atoi(e):-1;if(m==3)m=0;
    const int ro=vkn_policy_ubodyn_active(m,uenv,vkn_policy_ubo_dynamic_default(f),f);return ro?2:vkn_policy_env_or("DM_GFX_UBO_DYNAMIC",vkn_policy_ubo_dynamic_default(f),nullptr);}   // 2 = read-only rule, 1 all dynamic, 0 none
static void test_v3_ladder(){
    // C rules = C++ rules (V2 ported them token for token)
    for(int f=0;f<=5;f++){const XbgGpuFamily xf=XbgGpuFamily(f);
        CHECK(vkn_policy_noout_mask_default(f)==int(xbg_noout_mask_default(xf)));CHECK(vkn_policy_pipe_serial_default(f)==int(xbg_pipe_serial_default(xf)));
        CHECK(vkn_policy_ubo_dynamic_default(f)==int(xbg_ubo_dynamic_default(xf)));CHECK(vkn_policy_ubodyn_default(f)==xbg_nr_ubodyn_default(xf));
        for(int m=-1;m<=3;m++)for(int u=-1;u<=2;u++)for(int on=0;on<=1;on++)CHECK(vkn_policy_ubodyn_active(m,u,on,f)==int(xbg_nr_ubodyn_active(m,u,on!=0,xf)));}
    for(uint32_t d=0;d<=30;d++){CHECK(vkn_policy_depth_bias_x2_default(d)==int(xbg_depth_bias_x2_default(d)));CHECK(vkn_policy_clear_rebind_default(d)==int(xbg_clear_rebind_default(d)));
        CHECK(vkn_policy_reversed_depth_broken(d)==int(xbg_reversed_depth_broken(d)));}
    for(const char *sim:{"mali","powervr","pvr","xclipse","mali,ubdyn8","powervr nogs","family-mali","nobc","noaniso","ubdyn8","0","",(const char*)nullptr,"malix","mali;noaniso","nod24","novertex3","nofillmode","xclipse,ubdyn8","pvr nod24"}){
        const uint32_t b=xbg_sim_parse(sim);
        CHECK(vkn_policy_sim_nobc(sim)==((b&XBG_SIM_NOBC)!=0));CHECK(vkn_policy_sim_noaniso(sim)==((b&XBG_SIM_NOANISO)!=0));CHECK(vkn_policy_sim_ubdyn8(sim)==((b&XBG_SIM_UBDYN8)!=0));
        CHECK(vkn_policy_sim_nod24(sim)==((b&XBG_SIM_NOD24)!=0));CHECK(vkn_policy_sim_novertex3(sim)==((b&XBG_SIM_NOVERTEX3)!=0));CHECK(vkn_policy_sim_nofillmode(sim)==((b&XBG_SIM_NOFILLMODE)!=0));   // [VK-DRV]
        CHECK(vkn_policy_family_from(0x5143,"Adreno (TM) 750",sim)==int(xbg_effective_family(XBG_GPU_ADRENO,b)));}
    CHECK(vkn_policy_env_or("DM_SAFE_TEST_UNSET_KEY",1,nullptr)==1);
    // no step: the family defaults (Mali: the tile pass policy on, PowerVR: UBO rule 2 = NR-08b read-only, serial on)
    v3_clear();
    CHECK(vkn_policy_load(VKN_FAM_MALI)==1 && vkn_policy_store(VKN_FAM_MALI)==1 && vkn_policy_fresh(VKN_FAM_MALI)==2 && vkn_policy_tilebar(VKN_FAM_MALI)==1);
    CHECK(v3_ubo_dyn(VKN_FAM_POWERVR)==2 && v3_ubo_dyn(VKN_FAM_MALI)==1);
    CHECK(vkn_policy_env_or("DM_GFX_PIPE_SERIAL",vkn_policy_pipe_serial_default(VKN_FAM_MALI),nullptr)==0);
    CHECK(vkn_policy_env_or("DM_GFX_NOOUT_MASK",vkn_policy_noout_mask_default(VKN_FAM_POWERVR),nullptr)==1);
    for(XbgGpuFamily f:{XBG_GPU_MALI,XBG_GPU_POWERVR}){const int vf=int(f);
        // step 1: the backend draws as stage 4: pass policy at LOAD / STORE / zero fill / full barriers, recording thread off; PowerVR no dynamic UBOs
        v3_apply(1,f);
        CHECK(vkn_policy_load(vf)==0 && vkn_policy_store(vf)==0 && vkn_policy_fresh(vf)==0 && vkn_policy_tilebar(vf)==0);
        CHECK(getenv("DM_VK_NATIVE_RT") && !strcmp(getenv("DM_VK_NATIVE_RT"),"0") && !getenv("DM_VK_NATIVE"));
        CHECK(v3_ubo_dyn(vf)==(f==XBG_GPU_POWERVR?0:1));
        CHECK(vkn_policy_env_or("DM_GFX_PIPE_SERIAL",vkn_policy_pipe_serial_default(vf),nullptr)==(f==XBG_GPU_POWERVR));
        // step 2: still the backend; + no warm-up, CPU BC decode, whole mirrors, serial creation, PowerVR no colour mask of output-less shaders
        v3_apply(2,f);
        CHECK(!getenv("DM_VK_NATIVE") && vkn_policy_load(vf)==0);
        CHECK(!strcmp(getenv("DM_VK_NATIVE_WARM"),"0") && !strcmp(getenv("DM_VK_NATIVE_BCGPU"),"0") && !strcmp(getenv("DM_VK_NATIVE_MIRROR"),"0"));
        CHECK(!getenv("DM_GFX_BC_DECODE"));   // the old path's own BC switch is not touched (step 3 draws with the old path's default)
        CHECK(vkn_policy_env_or("DM_GFX_PIPE_SERIAL",0,nullptr)==1);
        CHECK(vkn_policy_env_or("DM_GFX_NOOUT_MASK",vkn_policy_noout_mask_default(vf),nullptr)==(f==XBG_GPU_POWERVR?0:1));
        // step 3: the old path
        v3_apply(3,f);
        CHECK(getenv("DM_VK_NATIVE") && !strcmp(getenv("DM_VK_NATIVE"),"0"));
        // a player's dm/env key is kept at every step (xbg_safe_should_set with player 1)
        CHECK(!xbg_safe_should_set("1",1));}
    v3_clear();
    // a saved count of 2 (a player of the build before V3: the old path then) starts on the backend at step 2 now; count 3 = the old path
    {int how=-1;const int st=xbg_safe_step(nullptr,nullptr,false,2,&how);CHECK(st==2 && how==XBG_SAFE_COUNTED);CHECK(!has(sw(st,XBG_GPU_MALI),"DM_VK_NATIVE=0"));
     CHECK(has(sw(xbg_safe_step(nullptr,nullptr,false,3,&how),XBG_GPU_MALI),"DM_VK_NATIVE=0"));}
    // each step's own line says what it set (the texts differ and name their content)
    CHECK(strstr(xbg_safe_step_text(1),"recording thread") && strstr(xbg_safe_step_text(2),"still on the backend") && strstr(xbg_safe_step_text(3),"old path"));
    CHECK(strcmp(xbg_safe_step_text(1),xbg_safe_step_text(2)) && strcmp(xbg_safe_step_text(2),xbg_safe_step_text(3)));
}

// [VK-DRV 21.11] the ladder of a mode-1 start on every family: the backend rows alone on Adreno / Xclipse / Maleoon / others (step 3 = only
// DM_VK_NATIVE=0: the old path with its own defaults), the unchanged lists on Mali / PowerVR; the counted gate to step 3; the marker's native=.
static std::string swm(int step,XbgGpuFamily f,bool native){XbgSafeSwitch s[128];const int n=xbg_safe_switches_mode(step,f,native,s,128);std::string o;for(int i=0;i<n;i++){o+=s[i].key;o+="=";o+=s[i].value;o+=" ";}return o;}
static void v3_apply_mode(int step,XbgGpuFamily f,bool native){v3_clear();XbgSafeSwitch a[128];const int n=xbg_safe_switches_mode(step,f,native,a,128);for(int i=0;i<n;i++)if(xbg_safe_should_set(getenv(a[i].key),0))setenv(a[i].key,a[i].value,1);}
static void test_vkdrv_ladder(){
    for(int f=0;f<=5;f++){const XbgGpuFamily xf=XbgGpuFamily(f);
        CHECK(xbg_safe_eligible(xf,true));CHECK(xbg_safe_eligible(xf,false)==xbg_safe_family(xf));
        for(int st=0;st<=3;st++)CHECK(swm(st,xf,false)==sw(st,xf));}   // the old-path ladder (a mode-0 start) is unchanged
    for(XbgGpuFamily f:{XBG_GPU_MALI,XBG_GPU_POWERVR})for(int st=0;st<=3;st++)CHECK(swm(st,f,true)==sw(st,f));   // Mali / PowerVR: the same list in both modes
    const char *step1="DM_VK_NATIVE_SH=0 DM_VK_NATIVE_SH_PCF=0 DM_VK_NATIVE_SHCACHE=0 DM_VK_NATIVE_LIGHTFUSE=0 DM_VK_NATIVE_LIGHTFUSE_FOG=0 DM_VK_NATIVE_COND=0 DM_VK_NATIVE_GBUF_LE=0 DM_VK_NATIVE_COST=0 "
        "DM_VK_NATIVE_RT=0 DM_VK_NATIVE_POST=0 DM_VK_NATIVE_UIPASS=0 DM_VK_NATIVE_MAT=0 DM_VK_NATIVE_XLQ=0 DM_VK_NATIVE_PREPASS=0 DM_VK_NATIVE_PRESKIN=0 DM_VK_NATIVE_LOAD=0 DM_VK_NATIVE_STORE=0 DM_VK_NATIVE_FRESH=0 DM_VK_NATIVE_TILEBAR=0 DM_VK_NATIVE_TILELIGHT=0 ";
    for(XbgGpuFamily f:{XBG_GPU_ADRENO,XBG_GPU_XCLIPSE,XBG_GPU_MALEOON,XBG_GPU_OTHER}){
        CHECK(swm(0,f,true).empty());
        CHECK(swm(1,f,true)==step1);
        CHECK(swm(2,f,true)==std::string(step1)+"DM_VK_NATIVE_WARM=0 DM_VK_NATIVE_BCGPU=0 DM_VK_NATIVE_MIRROR=0 DM_GFX_PIPE_SERIAL=1 ");
        CHECK(swm(3,f,true)==swm(2,f,true)+"DM_VK_NATIVE=0 ");
        // no old-path row on these families (owner 21.11: they stay Mali / PowerVR only)
        for(const char *k:{"DM_GFX_ZEROCOPY","DM_NR_","DM_NB","DM_NRN","DM_GFX_NOLOAD","DM_GFX_NOOUT_MASK","DM_GFX_UBO_DYNAMIC","DM_DOS2_PREPASS","DM_GFX_PRESENT_THREAD","DM_GFX_GS_PASSTHRU"})CHECK(swm(3,f,true).find(k)==std::string::npos);
        // applied and read by the backend's own readers: step 1 = stage-4 pass policy + no recording thread, step 2 still on the backend,
        // step 3 the old path
        const int vf=int(f);
        v3_apply_mode(1,f,true);
        CHECK(vkn_policy_load(vf)==0 && vkn_policy_store(vf)==0 && vkn_policy_fresh(vf)==0 && vkn_policy_tilebar(vf)==0);
        CHECK(getenv("DM_VK_NATIVE_RT") && !strcmp(getenv("DM_VK_NATIVE_RT"),"0") && !getenv("DM_VK_NATIVE"));
        v3_apply_mode(2,f,true);
        CHECK(!getenv("DM_VK_NATIVE") && !strcmp(getenv("DM_VK_NATIVE_WARM"),"0") && vkn_policy_env_or("DM_GFX_PIPE_SERIAL",0,nullptr)==1);
        v3_apply_mode(3,f,true);
        CHECK(getenv("DM_VK_NATIVE") && !strcmp(getenv("DM_VK_NATIVE"),"0") && !getenv("DM_NB_MT") && !getenv("DM_NR"));}
    v3_clear();
    // the marker's native= key (the renderer of the run that crashed)
    {XbgSafeMarker m;xbg_safe_parse("count=3\nstep=2\nnative=1\nfamily=adreno\n",&m);CHECK(m.native==1 && m.step==2 && m.count==3);
     xbg_safe_parse("count=3\nstep=2\nnative=0\n",&m);CHECK(m.native==0);
     xbg_safe_parse("count=3\nstep=2\n",&m);CHECK(m.native==-1);}
    // the counted gate: step 3 only after the backend itself crashed at step 2 (or the old path already ran)
    auto mk=[](const char *t){XbgSafeMarker m;xbg_safe_parse(t,&m);return m;};
    CHECK(xbg_safe_counted_gate(3,XBG_SAFE_COUNTED,true,mk("count=3\nstep=2\nnative=1\nfamily=adreno\n"))==3);
    CHECK(xbg_safe_counted_gate(3,XBG_SAFE_COUNTED,true,mk("count=3\nstep=1\nnative=1\nfamily=adreno\n"))==2);   // crashed at step 1: step 2 first
    CHECK(xbg_safe_counted_gate(3,XBG_SAFE_COUNTED,true,mk("count=3\nstep=0\nnative=1\nfamily=adreno\n"))==2);   // safe mode was off when it crashed
    CHECK(xbg_safe_counted_gate(3,XBG_SAFE_COUNTED,true,mk("count=4\nstep=3\nnative=0\nfamily=adreno\n"))==3);   // the old path crashed too: stays
    CHECK(xbg_safe_counted_gate(3,XBG_SAFE_COUNTED,true,mk("count=3\nstep=2\nnative=0\nfamily=adreno\n"))==2);   // a crash of the old path at step 2 says nothing of the backend
    CHECK(xbg_safe_counted_gate(3,XBG_SAFE_COUNTED,true,mk("count=3\nstep=1\nfamily=mali\n"))==3);    // a file of a build before VK-DRV, Mali: the old rule
    CHECK(xbg_safe_counted_gate(3,XBG_SAFE_COUNTED,true,mk("count=3\nstep=1\nfamily=adreno\n"))==2);  // ... on Adreno there was no count before
    CHECK(xbg_safe_counted_gate(3,XBG_SAFE_FORCED,true,mk("count=0\n"))==3 && xbg_safe_counted_gate(3,XBG_SAFE_ON,true,mk("count=3\nstep=0\nnative=1\n"))==3);   // the player's / gear's choice
    CHECK(xbg_safe_counted_gate(3,XBG_SAFE_COUNTED,false,mk("count=3\nstep=0\nnative=0\nfamily=mali\n"))==3);   // a mode-0 start: the old ladder
    for(int st=0;st<=2;st++)CHECK(xbg_safe_counted_gate(st,XBG_SAFE_COUNTED,true,mk("count=2\n"))==st);
    CHECK(strstr(xbg_safe_step_text_native(1),"recording thread") && strstr(xbg_safe_step_text_native(2),"still on the backend") && strstr(xbg_safe_step_text_native(3),"old path"));
    CHECK(!strstr(xbg_safe_step_text_native(1),"zero-copy"));
}

int main(){
    // families: Mali / PowerVR (incl. the SIM models on an Adreno), never Adreno / Xclipse / others
    CHECK(xbg_safe_family(XBG_GPU_MALI) && xbg_safe_family(XBG_GPU_POWERVR));
    CHECK(!xbg_safe_family(XBG_GPU_ADRENO) && !xbg_safe_family(XBG_GPU_XCLIPSE) && !xbg_safe_family(XBG_GPU_OTHER) && !xbg_safe_family(XBG_GPU_MALEOON));
    CHECK(xbg_effective_family(XBG_GPU_ADRENO,xbg_sim_parse("mali"))==XBG_GPU_MALI);
    CHECK(xbg_effective_family(XBG_GPU_ADRENO,xbg_sim_parse("powervr"))==XBG_GPU_POWERVR);
    CHECK(xbg_effective_family(XBG_GPU_ADRENO,xbg_sim_parse("powervr,nogs"))==XBG_GPU_POWERVR);
    // the step (DOOM's rules; DOS2 passes no ladder alias)
    int how=-1;
    CHECK(xbg_safe_step(nullptr,nullptr,false,0,&how)==0 && how==XBG_SAFE_COUNTED);
    CHECK(xbg_safe_step(nullptr,nullptr,false,1,&how)==1 && how==XBG_SAFE_COUNTED);
    CHECK(xbg_safe_step("",nullptr,true,2,&how)==2 && how==XBG_SAFE_COUNTED);
    CHECK(xbg_safe_step(nullptr,nullptr,true,7,&how)==3);
    CHECK(xbg_safe_step("auto",nullptr,false,2,&how)==2 && how==XBG_SAFE_COUNTED);
    CHECK(xbg_safe_step("0",nullptr,false,3,&how)==0 && how==XBG_SAFE_OFF);
    CHECK(xbg_safe_step("off",nullptr,false,3,&how)==0 && how==XBG_SAFE_OFF);
    CHECK(xbg_safe_step("on",nullptr,false,0,&how)==1 && how==XBG_SAFE_ON);
    CHECK(xbg_safe_step("on",nullptr,false,2,&how)==2 && how==XBG_SAFE_ON);
    CHECK(xbg_safe_step("2",nullptr,false,0,&how)==2 && how==XBG_SAFE_FORCED);
    CHECK(xbg_safe_step("9",nullptr,false,0,&how)==3 && how==XBG_SAFE_FORCED);
    CHECK(xbg_safe_step("bogus",nullptr,false,1,&how)==1 && how==XBG_SAFE_COUNTED);
    CHECK(xbg_safe_step(nullptr,nullptr,true,0,&how)==0 && how==XBG_SAFE_COUNTED);   // no alias: a PowerVR boot without count / DM_SAFE_MODE is step 0
    CHECK(xbg_safe_step(nullptr,nullptr,false,-3,&how)==0);
    for(int st=0;st<=3;st++)CHECK(xbg_safe_how_name(st) && *xbg_safe_how_name(st));
    CHECK(!strcmp(xbg_safe_how_name(XBG_SAFE_NOT_ELIGIBLE),"not-eligible"));

    // switches: nothing at step 0 or off the two families
    for(XbgGpuFamily f:{XBG_GPU_ADRENO,XBG_GPU_XCLIPSE,XBG_GPU_MALEOON,XBG_GPU_OTHER})for(int st=0;st<=3;st++)CHECK(sw(st,f).empty());
    CHECK(sw(0,XBG_GPU_MALI).empty() && sw(0,XBG_GPU_POWERVR).empty() && sw(-1,XBG_GPU_MALI).empty());
    // step 1 exactly
    CHECK(sw(1,XBG_GPU_MALI)=="DM_GFX_ZEROCOPY=0 DM_NR_RP=0 DM_DOS2_PREPASS=0 DM_NR_UBOVEC=0 DM_NR_ONCHIP=0 DM_NR_LIGHTFUSE=0 DM_NR_SH_PCF=0 DM_NR_PRESKIN=0 DM_NR_IDVS=0 DM_NR_UIX_DS=0 DM_NR_UIX_AREA=0 DM_VK_NATIVE_SH=0 DM_VK_NATIVE_SH_PCF=0 DM_VK_NATIVE_SHCACHE=0 DM_VK_NATIVE_LIGHTFUSE=0 DM_VK_NATIVE_LIGHTFUSE_FOG=0 DM_VK_NATIVE_COND=0 DM_VK_NATIVE_GBUF_LE=0 DM_VK_NATIVE_COST=0 "
          "DM_VK_NATIVE_RT=0 DM_VK_NATIVE_POST=0 DM_VK_NATIVE_UIPASS=0 DM_VK_NATIVE_MAT=0 DM_VK_NATIVE_XLQ=0 DM_VK_NATIVE_PREPASS=0 DM_VK_NATIVE_PRESKIN=0 DM_VK_NATIVE_LOAD=0 DM_VK_NATIVE_STORE=0 DM_VK_NATIVE_FRESH=0 DM_VK_NATIVE_TILEBAR=0 DM_VK_NATIVE_TILELIGHT=0 ");
    CHECK(sw(1,XBG_GPU_POWERVR)=="DM_GFX_ZEROCOPY=0 DM_GFX_UBO_DYNAMIC=0 DM_NR_UBODYN=0 DM_NR_RP=0 DM_DOS2_PREPASS=0 DM_NR_UBOVEC=0 DM_NR_ONCHIP=0 DM_NR_LIGHTFUSE=0 DM_NR_SH_PCF=0 DM_NR_PRESKIN=0 DM_NR_IDVS=0 DM_NR_UIX_DS=0 DM_NR_UIX_AREA=0 DM_VK_NATIVE_SH=0 DM_VK_NATIVE_SH_PCF=0 DM_VK_NATIVE_SHCACHE=0 DM_VK_NATIVE_LIGHTFUSE=0 DM_VK_NATIVE_LIGHTFUSE_FOG=0 DM_VK_NATIVE_COND=0 DM_VK_NATIVE_GBUF_LE=0 DM_VK_NATIVE_COST=0 "
          "DM_VK_NATIVE_RT=0 DM_VK_NATIVE_POST=0 DM_VK_NATIVE_UIPASS=0 DM_VK_NATIVE_MAT=0 DM_VK_NATIVE_XLQ=0 DM_VK_NATIVE_PREPASS=0 DM_VK_NATIVE_PRESKIN=0 DM_VK_NATIVE_LOAD=0 DM_VK_NATIVE_STORE=0 DM_VK_NATIVE_FRESH=0 DM_VK_NATIVE_TILEBAR=0 DM_VK_NATIVE_TILELIGHT=0 ");
    // [VK-NATIVE stage 5] step 1 turns the backend's techniques off (mode 1 stays on: a technique bug does not end on the old path);
    // [stage 5.3 V3] step 2 stays on the backend too, step 3 is the old path; the far-cascade hack and the measurement stay the player's / the tier's
    for(XbgGpuFamily f:{XBG_GPU_MALI,XBG_GPU_POWERVR}){const std::string s1=sw(1,f),s2=sw(2,f);
        for(const char *kv:{"DM_VK_NATIVE_SH=0","DM_VK_NATIVE_SH_PCF=0","DM_VK_NATIVE_SHCACHE=0","DM_VK_NATIVE_LIGHTFUSE=0","DM_VK_NATIVE_LIGHTFUSE_FOG=0","DM_VK_NATIVE_COND=0",
            "DM_VK_NATIVE_GBUF_LE=0","DM_VK_NATIVE_COST=0"}){CHECK(has(s1,kv));CHECK(has(s2,kv));}
        const std::string s3=sw(3,f);
        CHECK(!has(s1,"DM_VK_NATIVE=0") && !has(s2,"DM_VK_NATIVE=0") && has(s3,"DM_VK_NATIVE=0")); CHECK(s1.find("DM_VK_NATIVE_SHFAR")==std::string::npos && s1.find("DM_VK_NATIVE_PROF")==std::string::npos);}
    // step 2: step 1 + the tile pass policies (engine + NB) + the native recording paths; nothing of step 3
    for(XbgGpuFamily f:{XBG_GPU_MALI,XBG_GPU_POWERVR}){const std::string s1=sw(1,f),s2=sw(2,f),s3=sw(3,f);
        CHECK(s2.compare(0,s1.size(),s1)==0 && s3.compare(0,s2.size(),s2)==0);   // each step is the one before plus more, same order
        for(const char *kv:{"DM_GFX_NOLOAD=0","DM_GFX_STORE_HIST=0","DM_GFX_CLEAR_FOLD=0","DM_GFX_TILE_BARRIER=0","DM_GFX_LAZY_CLEAR=0","DM_NB_RP=0","DM_NB_OPBARRIER=0",
            "DM_NB_UIAREA=0","DM_NR_DIRECT=0","DM_NR_DRAW=0","DM_NR_ROTATE=0","DM_NR_SH=0","DM_NR_Z3=0","DM_NR_Z5=0","DM_NRN=0","DM_NRN_LIVE=0","DM_NRN_SHC=0","DM_NRN_EV=0","DM_NRN_LIST=0","DM_NRN_REC=0","DM_NRN_NOBODY=0","DM_NRN_PKT=0","DM_NRN_SHORT=0","DM_NRN_SHADOW=0","DM_NRN_PREPASS=0","DM_NRN_GBUF=0","DM_NRN_LIGHT=0","DM_NRN_POST=0","DM_NRN_OTHER=0","DM_NR_Z8=0","DM_NR_Z8_THREAD=0","DM_NR_Z8_IMG=0","DM_NR_Z8_VIEW=0","DM_NR_Z8_VAL=0","DM_NR_Z3B=0","DM_NR_Z3C=0","DM_NR_Z3D=0","DM_NR_Z3K=0",
            "DM_NR_SH_SURF=0","DM_NR_MAT=0","DM_NR_STATE=0","DM_NR_UBO=0","DM_NR_UBD=0","DM_NR_TRX_STICKY=0","DM_NR_TRX_ZC=0","DM_NR_TAPSET=0",
            "DM_NR_TEX=0","DM_NR_TEXSTREAM=0","DM_NB_POST_AA_OUTLINE=0",
            "DM_VK_NATIVE_WARM=0","DM_VK_NATIVE_BCGPU=0","DM_VK_NATIVE_MIRROR=0","DM_GFX_PIPE_SERIAL=1"}){CHECK(has(s2,kv));CHECK(!has(s1,kv));}
        for(const char *kv:{"DM_VK_NATIVE=0","DM_NB_MT=0","DM_GFX_GS_PASSTHRU=0","DM_NR=0","DM_NR_BUF=0","DM_NR_DESC=0","DM_NR_HEAT=0","DM_NR_SEED=0","DM_NB=0",
            "DM_NB_PIPE=0","DM_NB_DESC=0","DM_NB_UBO=0","DM_NB_OWN=0","DM_NB_POST=0","DM_NB_POST_AA=0","DM_GFX_PRESENT_THREAD=0"}){CHECK(has(s3,kv));CHECK(!has(s2,kv));}
        // every key once per step
        XbgSafeSwitch a[128];const int n=xbg_safe_switches(3,f,a,128);
        for(int i=0;i<n;i++){CHECK(count_key(s3,a[i].key)==1);CHECK(!strncmp(a[i].key,"DM_",3));CHECK(*a[i].value=='0' || *a[i].value=='1');}
        CHECK(n<128-16);}   // room in the engine's array (xbg_safe_mode.inc: 128) for the switches to come
    // PowerVR-only items: the colour mask of output-less shaders (the d2r #179 fix stays on Mali; [stage 5.3 V3] from step 2), dynamic UBOs / NR-08b
    CHECK(has(sw(3,XBG_GPU_POWERVR),"DM_GFX_NOOUT_MASK=0") && sw(3,XBG_GPU_MALI).find("NOOUT_MASK")==std::string::npos);
    CHECK(has(sw(2,XBG_GPU_POWERVR),"DM_GFX_NOOUT_MASK=0") && !has(sw(1,XBG_GPU_POWERVR),"DM_GFX_NOOUT_MASK=0"));
    CHECK(sw(3,XBG_GPU_MALI).find("UBO_DYNAMIC")==std::string::npos && sw(3,XBG_GPU_MALI).find("UBODYN")==std::string::npos);
    // no COMPILE_LOCK / FENCE_TRYLOCK / ZC3_SELFTEST / STORE_DEAD / STORE_NONE / LEAN_USAGE / TILE_AREA: DOOM switches DOS2 does not have
    for(XbgGpuFamily f:{XBG_GPU_MALI,XBG_GPU_POWERVR})for(const char *k:{"COMPILE_LOCK","FENCE_TRYLOCK","ZC3_SELFTEST","STORE_DEAD","STORE_NONE","LEAN_USAGE","TILE_AREA","PVR_LADDER"})
        CHECK(sw(3,f).find(k)==std::string::npos);
    {XbgSafeSwitch s[4];CHECK(xbg_safe_switches(3,XBG_GPU_POWERVR,s,4)==4);}   // capped by max
    for(int st=0;st<=3;st++)CHECK(xbg_safe_step_text(st) && *xbg_safe_step_text(st));

    // the player rule: dm/env wins, the app's default table does not
    CHECK(xbg_safe_should_set(nullptr,0)==1);   // unset: set
    CHECK(xbg_safe_should_set("1",0)==1);       // the app table's "1": replaced
    CHECK(xbg_safe_should_set("1",1)==0 && xbg_safe_should_set(nullptr,1)==0);   // the player's: kept
    CHECK(xbg_safe_should_set("1",-1)==0 && xbg_safe_should_set(nullptr,-1)==1);   // no tracking (host build): DOOM's rule

    // the marker file (DOOM's format)
    {XbgSafeMarker m;xbg_safe_parse("count=2\nstep=1\npid=4242\ntime=1731230000123\nfamily=powervr\nwhy=crash on the graphics thread dm-nbmt, signal 7\n",&m);
     CHECK(m.count==2 && m.step==1 && m.pid==4242 && m.time_ms==1731230000123ll && !strcmp(m.family,"powervr") && !strcmp(m.why,"crash on the graphics thread dm-nbmt, signal 7"));
     xbg_safe_parse("why=x\r\ncount=3\r\n",&m);CHECK(m.count==3 && !strcmp(m.why,"x"));
     xbg_safe_parse("",&m);CHECK(m.count==0 && m.pid==0 && !m.why[0]);
     xbg_safe_parse(nullptr,&m);CHECK(m.count==0);
     xbg_safe_parse("count=-5\ncounter=9\n",&m);CHECK(m.count==0);
     std::string longwhy="why="+std::string(400,'a');xbg_safe_parse(longwhy.c_str(),&m);CHECK(strlen(m.why)==sizeof m.why-1);}

    // classifiers: DOS2's recording threads count, the app / log / guest / CPU-decode threads do not
    for(const char *t:{"dm-gfx","dm-present","dm-crumbs","nvn-translate","dm-nbmt","dm-nbx0","dm-nrp-park","dm-nrp-acquire","mali-cmar-backe","pvr_compiler","tu_queue",
                       "vkn-record"})CHECK(xbg_safe_gfx_thread(t));   // [stage 5.3 V3] the backend's recording thread
    for(const char *t:{"dm-android","dm-lifecycle","dm-log","dm-input","dm-audio-keep","xbg-bcdec","dm-nrmem","dm-nrheat","MainThread","RenderThread","dm-vma"})CHECK(!xbg_safe_gfx_thread(t));
    CHECK(!xbg_safe_gfx_thread(nullptr) && !xbg_safe_gfx_thread(""));
    CHECK(xbg_safe_gfx_lib("/vendor/lib64/hw/vulkan.mtk.so") && xbg_safe_gfx_lib("/vendor/lib64/egl/libGLES_mali.so") && xbg_safe_gfx_lib("/vendor/lib64/libsrv_um.so"));
    CHECK(xbg_safe_gfx_lib("/vendor/lib64/hw/vulkan.powervr.so") && xbg_safe_gfx_lib("/data/user/0/org.dos2.port/files/drivers/panvk/libvulkan_panfrost.so"));
    CHECK(!xbg_safe_gfx_lib("/apex/com.android.runtime/lib64/bionic/libc.so") && !xbg_safe_gfx_lib("/data/app/x/lib/arm64/libdm.so") && !xbg_safe_gfx_lib(nullptr));
    CHECK(xbg_safe_text_device_lost("vkQueueSubmit(...) failed: -4") && xbg_safe_text_device_lost("present failed: VkResult -4") && xbg_safe_text_device_lost("result=-4)"));
    CHECK(xbg_safe_text_device_lost("vkQueueSubmit (test: DM_GFX_FAKE_GPU_CRASH=lost) failed: -4"));   // the fake trigger's text (xbg_safe_mode.inc)
    CHECK(xbg_safe_text_device_lost("VK_ERROR_DEVICE_LOST in vkWaitForFences"));
    CHECK(!xbg_safe_text_device_lost("failed: -42") && !xbg_safe_text_device_lost("failed: -1000072003") && !xbg_safe_text_device_lost("x-4") && !xbg_safe_text_device_lost(nullptr));
    // the GPU-hang fatal of xbg_game.cpp (recording_slot / poolreset) and of the fake trigger
    CHECK(xbg_safe_text_gpu_hang("The GPU did not finish this slot's work within 10000 ms (fence wait timed out), so the device is hung"));
    CHECK(xbg_safe_text_gpu_hang("The GPU did not finish this slot's work within 10000 ms (fence wait timed out), so the device is hung (test: DM_GFX_FAKE_GPU_CRASH=hang)"));
    CHECK(!xbg_safe_text_gpu_hang("Unsupported NVN format 0x12") && !xbg_safe_text_gpu_hang(nullptr) && !xbg_safe_text_device_lost("The GPU did not finish this slot's work within 10000 ms (fence wait timed out), so the device is hung"));

    // test triggers
    int s=0;
    CHECK(xbg_safe_fake_parse("60",&s)==XBG_FAKE_REC && s==60);
    CHECK(xbg_safe_fake_parse("45:lost",&s)==XBG_FAKE_LOST && s==45);
    CHECK(xbg_safe_fake_parse("30:hang",&s)==XBG_FAKE_HANG && s==30);
    CHECK(xbg_safe_fake_parse("30:rec",&s)==XBG_FAKE_REC);
    CHECK(xbg_safe_fake_parse("0",&s)==XBG_FAKE_NONE && xbg_safe_fake_parse("",&s)==XBG_FAKE_NONE && xbg_safe_fake_parse(nullptr,&s)==XBG_FAKE_NONE);
    CHECK(xbg_safe_fallback_fake("pipe:90","pipe")==90 && xbg_safe_fallback_fake("zc,fence:60,pipe:90","pipe")==90);
    CHECK(xbg_safe_fallback_fake("fence:60","pipe")==-1 && xbg_safe_fallback_fake("pipex","pipe")==-1 && xbg_safe_fallback_fake(nullptr,"pipe")==-1);
    CHECK(xbg_safe_fallback_fake(" pipe , fence:5","pipe")==0);
    test_v3_ladder();   // [stage 5.3 V3]
    test_vkdrv_ladder();   // [VK-DRV]
    printf(fails?"safe_mode_test: %d FAILED\n":"safe_mode_test: all passed\n",fails);
    return fails?1:0;
}
