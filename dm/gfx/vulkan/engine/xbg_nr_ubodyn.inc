// [DOS2] NR-08b DM_NR_UBODYN: dynamic uniform buffers for the class whose policy turned them off (PowerVR; docs/dos2/nr/NR-08b.md).
// Rule and its reasons: dm/gfx/nr/nr_ubodyn.h (pure C, host test tests/nr_ubodyn_test.c). Included by xbg_game.cpp before bundle_module.
//
//   DM_NR_UBODYN  0  (default, xbg_nr_ubodyn_default): nothing - no scan, no bit, UBO_DYN / dyn_mode_for take one not-taken branch each.
//                 1  where dynamic uniform buffers are off (PowerVR policy, or DM_GFX_UBO_DYNAMIC=0): every bundle module is scanned once
//                    at load (its final words, after every rewrite); a uniform binding the stage does not read gets XBG_BIND_UNREAD,
//                    every scanned one XBG_BIND_SCANNED. dyn_mode_for then works as with dynamic buffers on, but never above mode 2
//                    (per-draw blocks only: game blocks of the constant ring, driver blocks), and UBO_DYN refuses a binding that is not
//                    scanned-and-read. Effect: the descriptor memo hits again (VK-UBO-MEMO only refuses STATIC game blocks) and NR-08
//                    (DM_NR_DESC) writes a set once instead of rewriting it every draw. An optimized module (DM_GFX_SPV_OPT) that
//                    reads less than the module scanned is not used (the original is). Where dynamic buffers are on: nothing changes.
//                 2  dry run: scan, bits and counters as 1, behaviour as 0 (UBO_DYN ignores the bits). Shows on any phone what 1 would do.
//                 3  does not exist (nothing to poison): red line, off.
//   DM_GFX_UBO_DYNAMIC=2  DOOM 2016's spelling of the same rule (PVR-06): forces 1 on any family (A/B on an Adreno without SIM).
// Lines: "[nr] DM_NR_UBODYN=..." at the policy line; "[nr] ubodyn: ..." at most every 10 s while modules load and at the pipeline
// cache save; red lines "【红】[nr] ubodyn: ..." (first 16) for modules not understood and refused optimized copies.
#include "../../nr/nr_ubodyn.h"
constexpr uint32_t XBG_BIND_UNREAD=1u<<29,XBG_BIND_SCANNED=1u<<28;   // xbg_shader_binding.flags of a uniform binding; the bundle's are 0 (all 12 329 modules)
namespace nrubd{
int mode=0;bool ro=false,scan=false;   // ro: the rule is in force (UBO_DYN, dyn_mode_for); scan: modules are scanned (1, 2)
std::atomic<uint64_t> modules{0},not_understood{0},per_draw_read{0},per_draw_unread{0},fixed_read{0},fixed_unread{0},opt_checked{0},opt_refused{0},
    never_variants{0},reds{0},scan_ns{0};
std::atomic<uint64_t> next_ns{0},told_modules{0};
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
void red(const char *fmt,...){if(reds.fetch_add(1)>=16)return;char b[512];va_list a;va_start(a,fmt);vsnprintf(b,sizeof b,fmt,a);va_end(a);xbg_logf("【红】[nr] ubodyn: %s",b);}
void init(XbgGpuFamily fam,bool family_on){
    const char *e=getenv("DM_NR_UBODYN");int m=e && *e?atoi(e):-1;
    if(m==3){xbg_logf("【红】[nr] DM_NR_UBODYN=3 does not exist (nothing to poison): off");m=0;}
    if(m<-1 || m>3)m=0;
    const char *u=getenv("DM_GFX_UBO_DYNAMIC");const int uenv=u && *u?atoi(u):-1;
    ro=xbg_nr_ubodyn_active(m,uenv,family_on,fam);
    mode=m<0?xbg_nr_ubodyn_default(fam):m;if(ro && mode!=1)mode=1;   // DM_GFX_UBO_DYNAMIC=2 forces the rule
    scan=mode==1 || mode==2;
    if(ro)ubo_dynamic_on=true;
    xbg_logf("[nr] DM_NR_UBODYN=%d (NR-08b: %s; DM_GFX_UBO_DYNAMIC=%s, family %s default %s): %s",mode,
        mode==1?(ro?"on":"on, nothing to do"):mode==2?"dry run":"off",u && *u?u:"-",xbg_gpu_family_name(fam),family_on?"on":"off",
        ro?"dynamic uniform buffers on for per-draw blocks the stage reads (game blocks, driver blocks); support / c1 / unread blocks at fixed offsets"
          :mode==2?"modules scanned and counted, layouts as before":mode==1?"dynamic uniform buffers are already on here: layouts as before"
          :"layouts as before");}
void summary(bool force){
    if(!scan)return;const uint64_t now=timing_clock();
    if(!force){uint64_t n=next_ns.load();if(!n){next_ns.compare_exchange_strong(n,now+10000000000ull);return;}if(now<n || !next_ns.compare_exchange_strong(n,now+10000000000ull))return;}
    const uint64_t mods=modules.load();if(!force && mods==told_modules.load())return;told_modules=mods;
    xbg_logf("[nr] ubodyn: DM_NR_UBODYN=%d%s; modules %llu (not understood %llu: all their blocks fixed), %.1f us a scan; uniform bindings: per-draw read %llu (%s), "
        "per-draw NOT read %llu (fixed), support / c1 read %llu, not read %llu (fixed either way); optimized copies checked %llu, refused %llu (reads less); "
        "alpha NEVER variants under the rule %llu; red %llu",mode,ro?" (rule in force)":mode==2?" (dry run)":"",(unsigned long long)mods,
        (unsigned long long)not_understood.load(),mods?scan_ns.load()/1e3/double(mods):0.,(unsigned long long)per_draw_read.load(),ro?"dynamic":"would be dynamic",
        (unsigned long long)per_draw_unread.load(),(unsigned long long)fixed_read.load(),(unsigned long long)fixed_unread.load(),
        (unsigned long long)opt_checked.load(),(unsigned long long)opt_refused.load(),(unsigned long long)never_variants.load(),(unsigned long long)reds.load());}
// After every rewrite of load_shader_key, before bundle_module: the bits of each uniform binding from the module's final words.
void mark(Shader &p,const std::vector<uint32_t> &words){
    if(!scan)return;
    std::vector<uint32_t> bn;std::vector<size_t> at;
    for(size_t i=0;i<p.bindings.size();i++){auto &b=p.bindings[i];if(b.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)continue;b.flags&=~(XBG_BIND_UNREAD|XBG_BIND_SCANNED);bn.push_back(b.binding);at.push_back(i);}
    if(bn.empty()){++modules;summary(false);return;}
    std::vector<uint8_t> rd(bn.size());const uint64_t t0=timing_clock();
    const int why=nr_spv_binding_reads(words.data(),words.size(),bn.data(),unsigned(bn.size()),rd.data());scan_ns+=timing_clock()-t0;++modules;
    if(why!=NRU_OK){++not_understood;red("shader %s stage=%u: SPIR-V not understood (%s): its %zu uniform blocks stay at fixed offsets",p.key.c_str(),p.stage,nru_why_name(why),bn.size());}
    unsigned unread_pd=0;
    for(size_t k=0;k<at.size();k++){auto &b=p.bindings[at[k]];const bool per_draw=b.source==XBG_SOURCE_UNIFORM||b.source==XBG_SOURCE_DRIVER0||b.source==XBG_SOURCE_DRIVER2;
        b.flags|=XBG_BIND_SCANNED;if(!rd[k])b.flags|=XBG_BIND_UNREAD;
        if(per_draw){if(rd[k])++per_draw_read;else{++per_draw_unread;++unread_pd;}}else if(rd[k])++fixed_read;else ++fixed_unread;}
    if(unread_pd && why==NRU_OK){static std::atomic<unsigned> told{0};if(told.fetch_add(1)<8)xbg_logf("[nr] ubodyn: shader %s stage=%u declares %u per-draw uniform block(s) it never reads: %s at a fixed offset (PowerVR unused-dynamic-UBO overrun)",
        p.key.c_str(),p.stage,unread_pd,ro?"bound":"would be bound");}
    summary(false);}
// bundle_module: may the optimizer's copy replace `w`? It may only read more. false = create `w` (only when the rule is in force).
bool opt_ok(const std::vector<uint32_t> &w,const std::vector<uint32_t> &opt,const std::string &key){
    if(!scan)return true;++opt_checked;
    if(nr_spv_reads_kept(w.data(),w.size(),opt.data(),opt.size()))return true;
    ++opt_refused;red("module %s: the optimized copy reads fewer bindings than the module that was scanned: %s",key.c_str(),ro?"the original is created":"(dry run: the copy is used)");
    return !ro;}
}
