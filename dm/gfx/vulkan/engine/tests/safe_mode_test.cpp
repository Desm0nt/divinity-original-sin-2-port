// [DOS2][safe-mode] xbg_safe_mode.h (docs/dos2/nr/SAFE-MODE.md; ported from the DOOM 2016 port's tests/safe_mode_test.cpp, B&F 2.57): the step
// decision, the DOS2 switches per step and family, the marker file, the crash classifiers, the test-trigger parsers, the player rule.
// Host build, e.g. Windows clang++ or WSL:
//   clang++ -std=c++17 -Wall -idirafter <ndk>/toolchains/llvm/prebuilt/<host>/sysroot/usr/include -o safe_mode_test safe_mode_test.cpp
#include <cstdio>
#include <cstring>
#include <string>
#include <vulkan/vulkan.h>
#include "../xbg_safe_mode.h"

static int fails=0;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++fails;}}while(0)

static std::string sw(int step,XbgGpuFamily f){XbgSafeSwitch s[64];const int n=xbg_safe_switches(step,f,s,64);std::string o;for(int i=0;i<n;i++){o+=s[i].key;o+="=";o+=s[i].value;o+=" ";}return o;}
static bool has(const std::string &s,const char *kv){return s.find(std::string(kv)+" ")!=std::string::npos;}
static int count_key(const std::string &s,const char *k){int c=0;for(size_t p=s.find(k);p!=std::string::npos;p=s.find(k,p+1))if((p==0 || s[p-1]==' ') && s[p+strlen(k)]=='=')++c;return c;}

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
    CHECK(sw(1,XBG_GPU_MALI)=="DM_GFX_ZEROCOPY=0 DM_NR_RP=0 DM_DOS2_PREPASS=0 DM_NR_UBOVEC=0 DM_NR_ONCHIP=0 DM_NR_LIGHTFUSE=0 DM_NR_SH_PCF=0 DM_NR_PRESKIN=0 DM_NR_IDVS=0 DM_NR_UIX_DS=0 DM_NR_UIX_AREA=0 ");
    CHECK(sw(1,XBG_GPU_POWERVR)=="DM_GFX_ZEROCOPY=0 DM_GFX_UBO_DYNAMIC=0 DM_NR_UBODYN=0 DM_NR_RP=0 DM_DOS2_PREPASS=0 DM_NR_UBOVEC=0 DM_NR_ONCHIP=0 DM_NR_LIGHTFUSE=0 DM_NR_SH_PCF=0 DM_NR_PRESKIN=0 DM_NR_IDVS=0 DM_NR_UIX_DS=0 DM_NR_UIX_AREA=0 ");
    // step 2: step 1 + the tile pass policies (engine + NB) + the native recording paths; nothing of step 3
    for(XbgGpuFamily f:{XBG_GPU_MALI,XBG_GPU_POWERVR}){const std::string s1=sw(1,f),s2=sw(2,f),s3=sw(3,f);
        CHECK(s2.compare(0,s1.size(),s1)==0 && s3.compare(0,s2.size(),s2)==0);   // each step is the one before plus more, same order
        for(const char *kv:{"DM_GFX_NOLOAD=0","DM_GFX_STORE_HIST=0","DM_GFX_CLEAR_FOLD=0","DM_GFX_TILE_BARRIER=0","DM_GFX_LAZY_CLEAR=0","DM_NB_RP=0","DM_NB_OPBARRIER=0",
            "DM_NB_UIAREA=0","DM_NR_DIRECT=0","DM_NR_DRAW=0","DM_NR_ROTATE=0","DM_NR_SH=0","DM_NR_Z3=0","DM_NR_Z5=0","DM_NR_Z8=0","DM_NR_Z3B=0","DM_NR_Z3C=0","DM_NR_Z3D=0","DM_NR_Z3K=0",
            "DM_NR_SH_SURF=0","DM_NR_MAT=0","DM_NR_STATE=0","DM_NR_UBO=0","DM_NR_UBD=0","DM_NR_TRX_STICKY=0","DM_NR_TRX_ZC=0","DM_NR_TAPSET=0",
            "DM_NR_TEX=0","DM_NR_TEXSTREAM=0","DM_NB_POST_AA_OUTLINE=0"}){CHECK(has(s2,kv));CHECK(!has(s1,kv));}
        for(const char *kv:{"DM_GFX_PIPE_SERIAL=1","DM_NB_MT=0","DM_GFX_GS_PASSTHRU=0","DM_NR=0","DM_NR_BUF=0","DM_NR_DESC=0","DM_NR_HEAT=0","DM_NR_SEED=0","DM_NB=0",
            "DM_NB_PIPE=0","DM_NB_DESC=0","DM_NB_UBO=0","DM_NB_OWN=0","DM_NB_POST=0","DM_NB_POST_AA=0","DM_GFX_PRESENT_THREAD=0"}){CHECK(has(s3,kv));CHECK(!has(s2,kv));}
        // every key once per step
        XbgSafeSwitch a[64];const int n=xbg_safe_switches(3,f,a,64);
        for(int i=0;i<n;i++){CHECK(count_key(s3,a[i].key)==1);CHECK(!strncmp(a[i].key,"DM_",3));CHECK(*a[i].value=='0' || *a[i].value=='1');}
        CHECK(n<64);}
    // PowerVR-only items: the colour mask of output-less shaders (the d2r #179 fix stays on Mali), dynamic UBOs / NR-08b
    CHECK(has(sw(3,XBG_GPU_POWERVR),"DM_GFX_NOOUT_MASK=0") && sw(3,XBG_GPU_MALI).find("NOOUT_MASK")==std::string::npos);
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
    for(const char *t:{"dm-gfx","dm-present","dm-crumbs","nvn-translate","dm-nbmt","dm-nbx0","dm-nrp-park","dm-nrp-acquire","mali-cmar-backe","pvr_compiler","tu_queue"})CHECK(xbg_safe_gfx_thread(t));
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
    printf(fails?"safe_mode_test: %d FAILED\n":"safe_mode_test: all passed\n",fails);
    return fails?1:0;
}
