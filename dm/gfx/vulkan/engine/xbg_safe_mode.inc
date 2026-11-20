// [DOS2][safe-mode] GPU safe mode for Mali / PowerVR: the engine side (docs/dos2/nr/SAFE-MODE.md; the rules in xbg_safe_mode.h). Ported from the
// DOOM 2016 port's xbg_game.cpp namespace safemode (docs/port-kb MALI-13 / PVR-08, its B&F 2.57), included by xbg_game.cpp inside its anonymous
// namespace right after gpguard (live_pipe needs it).
//
// Boot: xbg_safe_mode_boot (vulkan_present.cpp, the physical device known and before the device is made and before DM_NR_ROTATE is read; the
// engine calls it itself when no presenter did) reads <files>/gfx-safe-mode, decides the step and sets its switches. dm/env wins: a key the player
// set (jni_android.c dm_env_player) is kept; the app's own default table (jni k_nb_on, setenv without overwrite) is replaced (setenv overwrite).
// Everything that reads these switches runs later: the engine's initialize, host_preload's NB / NR installs in dm_main, the presenter's swapchain.
// Record: xbg_safe_mode_record, once per process and signal-safe (open / read / write / close), from the crash classifier (xbg_crash_classify,
// host/crash.c) and xbg_fatalf (a VK_ERROR_DEVICE_LOST or GPU-hang text). Live (in-process, no restart): live_pipe (a failed pipeline build ->
// DM_GFX_PIPE_SERIAL on) queues a message for the app (xbg_safe_mode_event -> JNI nativeSafeModeEvent -> SafeMode.java toast). status: one line
// for the app (JNI nativeSafeModeStatus). DOOM's other live fallbacks (zero-copy self-test, fence trylock / guard) have no DOS2 counterpart.
extern "C" __attribute__((weak)) int dm_env_player(const char *key);   // jni_android.c: 1 = dm/env (or the launch environment) set it, 0 = not
namespace safemode {
char path[512]="";bool booted=false,eligible=false;int step=0,how=XBG_SAFE_NOT_ELIGIBLE,count=0;XbgGpuFamily family=XBG_GPU_OTHER;
std::atomic<int> recorded{0};
int fake_kind=0,fake_s=0;std::atomic<uint64_t> fake_rec_at{0};uint64_t fake_at_ns=0;   // DM_GFX_FAKE_GPU_CRASH (armed at engine init)
int fb_pipe_s=-1;uint64_t fb_pipe_at=0;   // DM_GFX_FAKE_FALLBACK=pipe:<s>
char status[1024]="";std::mutex ev_mu;std::deque<std::string> events;
std::atomic<bool> pipe_done{false};
// signal-safe text helpers
inline size_t cat(char *o,size_t at,size_t cap,const char *s){while(s && *s && at+1<cap)o[at++]=*s++;o[at]=0;return at;}
inline size_t dec(char *o,size_t at,size_t cap,long long v){char t[24];int n=0;unsigned long long u=v<0?0ull-(unsigned long long)v:(unsigned long long)v;do{t[n++]=char('0'+u%10);u/=10;}while(u && n<23);
    if(v<0 && at+1<cap)o[at++]='-';while(n && at+1<cap)o[at++]=t[--n];o[at]=0;return at;}
void event(const char *kind,const std::string &detail){std::lock_guard<std::mutex> l(ev_mu);if(events.size()<16)events.push_back(std::string(kind)+"\t"+detail);}
// DM_GFX_FAKE_GPU_CRASH=<s>[:rec]: NBMT_ENDRP_HOOK (xbg_nb_mt.inc end_rp_call) once the time has come. Once per process.
void fake_rec_fire(){
    uint64_t at=fake_rec_at.load(std::memory_order_relaxed);if(!at || timing_clock()<at || !fake_rec_at.compare_exchange_strong(at,0))return;
    char tn[32]="?";pthread_getname_np(pthread_self(),tn,sizeof tn);
    xbg_logf("NVN safe mode test: DM_GFX_FAKE_GPU_CRASH=rec: SIGBUS inside vkCmdEndRenderPass on thread %s",tn);
    raise(SIGBUS);}
// Once per present (xbg_present_texture_now, translation thread): the test triggers that are due (lost / hang fatals, the pipe fallback).
void frame_tick(){
    if(!fake_at_ns && !fb_pipe_at)return;
    const uint64_t now=timing_clock();
    if(fake_at_ns && now>=fake_at_ns){fake_at_ns=0;
        if(fake_kind==XBG_FAKE_LOST)xbg_fatalf("vkQueueSubmit (test: DM_GFX_FAKE_GPU_CRASH=lost) failed: %d",int(VK_ERROR_DEVICE_LOST));
        if(fake_kind==XBG_FAKE_HANG)xbg_fatalf("The GPU did not finish this slot's work within %llu ms (fence wait timed out), so the device is hung (test: DM_GFX_FAKE_GPU_CRASH=hang)",
            (unsigned long long)(std::max<uint64_t>(cfg.fence_timeout_ns,10000000000ull)/1000000ull));}
    if(fb_pipe_at && now>=fb_pipe_at){fb_pipe_at=0;gpguard::live_pipe("test:DM_GFX_FAKE_FALLBACK=pipe",VK_ERROR_INITIALIZATION_FAILED);}
}
}
namespace gpguard {
void live_pipe(const std::string &combo,VkResult r){
    if(!safemode::eligible || safemode::pipe_done.exchange(true))return;
    const bool was=serial.exchange(true);
    xbg_logf("NVN safe mode (live): graphics pipeline %s failed to build (result=%d): DM_GFX_PIPE_SERIAL %d -> 1 for the rest of this run (no restart)",combo.c_str(),int(r),int(was));
    char d[200];snprintf(d,sizeof d,"%.160s result=%d",combo.c_str(),int(r));safemode::event("pipe",d);}
}
// [safe-mode] engine init, right after gpu_policy (before the family switches are read): the boot decision if the presenter made none, the policy
// family as the authority for recording (the same family the presenter computed: real + DM_GFX_SIM_GPU), the test timers.
void safe_mode_engine_init();
} // namespace

// The count of graphics failures in gfx-safe-mode + 1, written once per process. Signal-safe. Returns the new count (0 = not recorded:
// not Mali / PowerVR, no files dir, or already recorded by this process).
extern "C" int xbg_safe_mode_record(const char *why){
    using namespace safemode;
    if(!eligible || !path[0] || recorded.exchange(1))return 0;
    const int fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC,0600);if(fd<0)return 0;
    char b[1024];const ssize_t r=read(fd,b,sizeof b-1);int c=0;
    if(r>0){b[r]=0;for(const char *p=b;*p;){if(!strncmp(p,"count=",6)){for(const char *q=p+6;*q>='0' && *q<='9';q++)c=c*10+(*q-'0');break;}while(*p && *p!='\n')++p;if(*p)++p;}}
    c=c<99?c+1:99;
    struct timespec ts;clock_gettime(CLOCK_REALTIME,&ts);const long long ms=(long long)ts.tv_sec*1000+ts.tv_nsec/1000000;
    char o[512];size_t n=0;n=cat(o,n,sizeof o,"count=");n=dec(o,n,sizeof o,c);n=cat(o,n,sizeof o,"\nstep=");n=dec(o,n,sizeof o,step);
    n=cat(o,n,sizeof o,"\npid=");n=dec(o,n,sizeof o,getpid());n=cat(o,n,sizeof o,"\ntime=");n=dec(o,n,sizeof o,ms);
    n=cat(o,n,sizeof o,"\nfamily=");n=cat(o,n,sizeof o,xbg_gpu_family_name(family));n=cat(o,n,sizeof o,"\nwhy=");
    for(const char *w=why?why:"?";*w && n+2<sizeof o;w++)o[n++]=*w=='\n' || *w=='\t'?' ':*w;
    o[n++]='\n';o[n]=0;
    if(lseek(fd,0,SEEK_SET)==0 && ftruncate(fd,0)==0 && write(fd,o,n)==ssize_t(n)){}
    fsync(fd);close(fd);
    const int next=c<XBG_SAFE_MAX_STEP?c:XBG_SAFE_MAX_STEP;
    fprintf(stderr,"[dm] safe mode: graphics failure on %s (%s): gfx-safe-mode count -> %d (this run had step %d; the next start, counted, turns on step %d)\n",
        xbg_gpu_family_name(family),why?why:"?",c,step,next);
    return c;
}
// [safe-mode] this boot's step: before the Vulkan device exists (vulkan_present.cpp, physical device known) or at engine init. Once per process.
extern "C" void xbg_safe_mode_boot(uint32_t vendor,const char *name){
    using namespace safemode;
    if(booted)return;booted=true;
    const uint32_t sim=xbg_sim_parse(getenv("DM_GFX_SIM_GPU"));
    family=xbg_effective_family(xbg_gpu_family(vendor,name),sim);eligible=xbg_safe_family(family);
    {const char *d=getenv("DM_SAFE_DIR");if(!(d && *d))d=getenv("DM_FILES_DIR");if(d && *d)snprintf(path,sizeof path,"%s/gfx-safe-mode",d);}
    XbgSafeMarker m;std::string text;
    if(path[0])if(FILE *f=fopen(path,"rb")){char b[1024];const size_t k=fread(b,1,sizeof b-1,f);b[k]=0;text=b;fclose(f);}
    xbg_safe_parse(text.c_str(),&m);
    count=m.count;
    const char *sm=getenv("DM_SAFE_MODE");
    step=xbg_safe_step(sm,nullptr,family==XBG_GPU_POWERVR,count,&how);
    if(!eligible){step=0;how=XBG_SAFE_NOT_ELIGIBLE;
        snprintf(status,sizeof status,"0\t0\tnot-eligible\t%s\t%d\t%s\t",xbg_gpu_family_name(family),count,m.why);
        xbg_logf("NVN safe mode: not used on %s (Mali / PowerVR only, DM_GFX_SIM_GPU=%s; %d graphics failure(s) counted in %s)",xbg_gpu_family_name(family),
            getenv("DM_GFX_SIM_GPU")?getenv("DM_GFX_SIM_GPU"):"-",count,path[0]?path:"-");
        return;}
    XbgSafeSwitch sw[96];const int n=xbg_safe_switches(step,family,sw,96);   /* (64 until the NR-ZERO Z8 parts: with them step 3 on PowerVR is exactly 64 switches - the list must never be cut) */std::string set,player;
    for(int i=0;i<n;i++){const char *have=getenv(sw[i].key);const int pl=dm_env_player?dm_env_player(sw[i].key):-1;
        if(!xbg_safe_should_set(have,pl)){player+=" ";player+=sw[i].key;player+="=";player+=have?have:"?";continue;}
        setenv(sw[i].key,sw[i].value,1);set+=" ";set+=sw[i].key;set+="=";set+=sw[i].value;
        if(have && strcmp(have,sw[i].value)){set+=" (was ";set+=have;set+=")";}}
    snprintf(status,sizeof status,"1\t%d\t%s\t%s\t%d\t%s\t%s",step,xbg_safe_how_name(how),xbg_gpu_family_name(family),count,m.why,set.empty()?"":set.c_str()+1);
    xbg_logf("NVN safe mode: step %d of 3 on %s (%s; %d graphics failure(s) counted in %s, last: %s; DM_SAFE_MODE=%s): %s.%s%s%s%s",
        step,xbg_gpu_family_name(family),xbg_safe_how_name(how),count,path[0]?path:"-",m.why[0]?m.why:"-",sm && *sm?sm:"-",xbg_safe_step_text(step),
        set.empty()?"":" Set:",set.c_str(),player.empty()?"":" The player's own (dm/env) values kept:",player.c_str());
    // test triggers (only here: Mali / PowerVR, real or simulated)
    {const char *e=getenv("DM_GFX_FAKE_GPU_CRASH");fake_kind=xbg_safe_fake_parse(e,&fake_s);
     const char *f=getenv("DM_GFX_FAKE_FALLBACK");fb_pipe_s=xbg_safe_fallback_fake(f,"pipe");
     if(fake_kind)xbg_logf("NVN safe mode test: DM_GFX_FAKE_GPU_CRASH=%s: a fake %s %d s after the engine starts",e,fake_kind==XBG_FAKE_REC?"SIGBUS inside vkCmdEndRenderPass":fake_kind==XBG_FAKE_LOST?"VK_ERROR_DEVICE_LOST fatal":"GPU-hang fatal",fake_s);
     if(f && *f)xbg_logf("NVN safe mode test: DM_GFX_FAKE_FALLBACK=%s (pipe %d s; DOOM's zc / fence kinds do not exist here)",f,fb_pipe_s);}
}
// For the app (JNI nativeSafeModeStatus): "<eligible>\t<step>\t<how>\t<family>\t<count>\t<last why>\t<switches set>", "" before the boot decision.
extern "C" const char *xbg_safe_mode_status(void){return safemode::booted?safemode::status:"";}
// The next in-process fallback message for the app ("<kind>\t<detail>"), copied into out; 0 = none.
extern "C" int xbg_safe_mode_event(char *out,size_t n){
    std::lock_guard<std::mutex> l(safemode::ev_mu);if(safemode::events.empty() || !n)return 0;
    snprintf(out,n,"%s",safemode::events.front().c_str());safemode::events.pop_front();return 1;}
// [safe-mode] host/crash.c (weak), in the signal handler after the "crash thread" line: is this crash a graphics failure? A fault in zero-copy
// guest memory, a graphics / recording / GPU-driver thread, pc (or lr) in a GPU driver library, or an engine abort with VK_ERROR_DEVICE_LOST /
// a GPU hang. Then counted (xbg_safe_mode_record). Only table reads and string compares. rec: DOOM's dm-record packet (DOS2: always null).
extern "C" void xbg_crash_classify(int sig,const char *thread,const char *rec,uint64_t addr,const char *pc_lib,const char *lr_lib){
    using namespace safemode;
    if(!eligible)return;
    char why[200];size_t n=0;why[0]=0;
    bool zcmem=false;if(sig==SIGSEGV || sig==SIGBUS){const int c=zc::count.load(std::memory_order_relaxed);
        for(int i=0;i<c && i<int(zc::MAX_CHUNKS);i++){const uintptr_t lo=zc::table[i].lo;const size_t sz=zc::table[i].n;if(lo && addr>=lo && addr<lo+sz){zcmem=true;break;}}}
    if(rec){n=cat(why,n,sizeof why,"driver crash inside the recording packet ");n=cat(why,n,sizeof why,rec);}
    else if(zcmem){n=cat(why,n,sizeof why,"fault in zero-copy guest memory on thread ");n=cat(why,n,sizeof why,thread);}
    else if(xbg_safe_gfx_thread(thread)){n=cat(why,n,sizeof why,"crash on the graphics thread ");n=cat(why,n,sizeof why,thread);}
    else if(xbg_safe_gfx_lib(pc_lib) || xbg_safe_gfx_lib(lr_lib)){const char *lib=xbg_safe_gfx_lib(pc_lib)?pc_lib:lr_lib,*b=strrchr(lib,'/');n=cat(why,n,sizeof why,"crash in the GPU driver ");n=cat(why,n,sizeof why,b?b+1:lib);
        n=cat(why,n,sizeof why," on thread ");n=cat(why,n,sizeof why,thread);}
    else if(sig==SIGABRT && failed && xbg_safe_text_device_lost(error_text))n=cat(why,n,sizeof why,"VK_ERROR_DEVICE_LOST fatal");
    else if(sig==SIGABRT && failed && xbg_safe_text_gpu_hang(error_text))n=cat(why,n,sizeof why,"GPU hang fatal");
    if(!why[0]){fprintf(stderr,"[dm] safe mode: crash (signal %d, thread %s) not counted as a graphics failure: not a graphics thread, pc / lr not in a GPU driver, not zero-copy memory, no device-lost / hang fatal\n",sig,thread?thread:"?");return;}
    n=cat(why,n,sizeof why,", signal ");n=dec(why,n,sizeof why,sig);
    if(!xbg_safe_mode_record(why))fprintf(stderr,"[dm] safe mode: %s (this run's failure is already counted)\n",why);
}
namespace {
void safe_mode_engine_init(){
    using namespace safemode;
    if(!booted)xbg_safe_mode_boot(properties.vendorID,properties.deviceName);
    if(xbg_safe_family(gpu_policy)!=eligible){xbg_logf("NVN safe mode: the engine's policy family %s differs from the boot's %s; recording follows the engine",xbg_gpu_family_name(gpu_policy),xbg_gpu_family_name(family));
        eligible=xbg_safe_family(gpu_policy);family=gpu_policy;}
    const uint64_t now=timing_clock();
    fake_rec_at.store(eligible && fake_kind==XBG_FAKE_REC?now+uint64_t(fake_s)*1000000000ull:0);
    fake_at_ns=eligible && (fake_kind==XBG_FAKE_LOST || fake_kind==XBG_FAKE_HANG)?now+uint64_t(fake_s)*1000000000ull:0;
    fb_pipe_at=eligible && fb_pipe_s>=0?now+uint64_t(fb_pipe_s)*1000000000ull:0;
}
