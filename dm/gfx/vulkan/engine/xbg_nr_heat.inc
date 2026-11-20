// [DOS2] NR-HEAT DM_NR_HEAT (docs/dos2/nr/NR-HEAT.md): the always-on shader / pass heat logger. Included by xbg_game.cpp after the
// Shader struct; the hooks the code before it calls are declared at namespace passval's forward declaration (pass_prof_reserve).
//
// Sample, do not trace. One frame every DM_NR_HEAT_S seconds goes through the existing pass profiler (DM_GFX_PASS_PROF's machinery:
// two BOTTOM_OF_PIPE timestamps around each render pass / dispatch, written OUTSIDE the render pass - inside a pass a tiler (Adreno
// binning, Mali, PowerVR) gives no per-draw time worth having), plus, per pass, the shader programs its draws used with their draw and
// vertex counts (no strings: the profiler's GPASS key text is only built when GPASS lines are wanted). Everything else - stamps -> us,
// shader ids, the location, the aggregation, the file - happens after the slot's fence: the translation thread converts the records
// (tens of us a sample) and hands them to the writer thread dm-nrheat (nice 10), which aggregates per level and appends to
// <logs>/shader-heat.tsv (format in dm/gfx/nr/nr_heat.h), rotated at DM_NR_HEAT_MB, and logs one "[nr] heat" line a minute.
// Frames between samples pay nothing new: the profiler's per-draw / per-pass hooks were already there and return on `!s.prof`.
//   DM_NR_HEAT        0  off: no sampling, no thread, no file (the profiler as before)
//                     1  one frame every DM_NR_HEAT_S (20) s, the first after DM_NR_HEAT_FIRST_S (30) s; block flush every
//                        DM_NR_HEAT_FLUSH_S (300) s and at a location change
//                     2  phone test: every frame of a burst of DM_NR_HEAT_BURST (120) frames every DM_NR_HEAT_S, P lines per pass, a flush
//                        after each burst
//   DM_NR_HEAT_MB     4    bytes per file (MB, 1..64); the previous file is kept as shader-heat.1.tsv
//   DM_NR_HEAT_DIR         the directory (default DM_LOG_DIR = Download/<game>/logs, set by jni_android.c; else DM_RUNTIME_DIR)
// With DM_GFX_PASS_PROF on, heat takes no frames of its own: it reads the profiler's frames (their GPASS lines stay as they were).
// Not involved: NR-14's samples reports (their own pool), NB-20b packets (vkCmdWriteTimestamp / vkCmdResetQueryPool are packets like any
// other command), the present thread and NR-20's rotation (after the frame's passes).
#include "../../nr/nr_heat.h"
#include <unordered_map>
#include <unordered_set>
#include <sys/resource.h>
extern "C" int dm_dos2_heat_level(char *cur,size_t n,char *prev,size_t pn,double *score_kb) __attribute__((weak));   // dm/host/hle_dos2_heat.c (DOS2 only)
namespace nrheat{
int mode=0;
double period_s=20,first_s=30,flush_s=300;uint32_t burst=120;uint64_t cap_bytes=4ull<<20;
uint64_t t0_ns=0,next_ns=0,boot_ns=0;uint32_t burst_left=0;
std::string &dir=*new std::string;   // heap, never destroyed: the detached writer may still run during exit()

// ---- recording (translation thread, sampled frames only) ----
void close(Slot::PassRec &r){
    if(r.hcur<r.heat.size()){auto &e=r.heat[r.hcur];e.draws+=uint32_t(pass_prof_draws-r.hd0);e.verts+=pass_prof_verts-r.hv0;}
    r.hd0=pass_prof_draws;r.hv0=pass_prof_verts;}
// pass_prof_draw: the draw about to be recorded uses (vs, fs); what was drawn since the last mark belongs to the previous program
void draw(Slot::PassRec &r,const void *vs,const void *fs){
    close(r);uint32_t i=r.hcur;
    if(!(i<r.heat.size() && r.heat[i].vs==vs && r.heat[i].fs==fs)){
        i=UINT32_MAX;for(uint32_t k=0;k<r.heat.size();k++)if(r.heat[k].vs==vs && r.heat[k].fs==fs){i=k;break;}
        if(i==UINT32_MAX){if(r.heat.size()>=96)i=uint32_t(r.heat.size()-1);   // a pathological pass: the rest goes to its last program
            else{r.heat.push_back(Slot::PassRec::HeatProg{vs,fs,0,0});i=uint32_t(r.heat.size()-1);}}}
    r.hcur=i;}
bool due(uint64_t frame){(void)frame;const uint64_t now=timing_clock();
    if(!t0_ns){t0_ns=now;next_ns=now+uint64_t(first_s*1e9);}
    if(burst_left){--burst_left;return true;}
    if(now<next_ns)return false;
    next_ns=now+uint64_t(period_s*1e9);if(mode==2 && burst>1)burst_left=burst-1;return true;}

// ---- hand-over ----
struct PassS{uint8_t compute;uint32_t w,h,fmt,att;double us;uint32_t draws;uint64_t verts;uint32_t p0,np;};
struct Sample{uint64_t frame=0,t_ns=0,unix_s=0;double span_us=0,convert_us=0;uint32_t stamps=0,skipped=0;std::vector<PassS> passes;std::vector<nrh_prog> progs;};
struct Note{uint64_t id;std::string key;uint32_t stage;char src;std::vector<uint32_t> spv;};
struct W{std::mutex mu;std::condition_variable cv;std::deque<Sample> q;std::vector<Note> notes;bool started=false;};
W *w=nullptr;   // never destroyed (the writer thread outlives static destruction)
Sample cur;bool cur_open=false;uint64_t conv_t0=0;

inline uint64_t shader_id(const void *p){return p?nrh_key_id(static_cast<const Shader*>(p)->key.c_str()):0;}
void writer();
void start(){if(!w)w=new W;std::lock_guard<std::mutex> l(w->mu);if(w->started)return;w->started=true;std::thread(writer).detach();}   // init() only
void begin_slot(){cur=Sample{};cur_open=true;conv_t0=timing_clock();}
// retire_slot, per complete pass record (us = its two stamps)
void pass(const Slot::PassRec &r,double us){if(!cur_open)return;
    PassS p{uint8_t(r.compute),r.w,r.h,r.fmt,r.att,us,r.draws1-r.draws0,r.verts1-r.verts0,uint32_t(cur.progs.size()),0};
    if(r.compute){cur.progs.push_back(nrh_prog{0,r.first.empty()?0:nrh_key_id(r.first.c_str()),1,0,-1,-1});p.np=1;}
    else for(auto &h:r.heat){cur.progs.push_back(nrh_prog{shader_id(h.vs),shader_id(h.fs),h.draws,h.verts,-1,-1});++p.np;}
    cur.passes.push_back(p);}
void end_slot(const Slot &s,double span_us){if(!cur_open)return;cur_open=false;
    cur.frame=s.prof_frame;cur.span_us=span_us;cur.stamps=s.prof_used;cur.skipped=s.prof_skipped;cur.t_ns=timing_clock();cur.unix_s=uint64_t(time(nullptr));
    cur.convert_us=double(cur.t_ns-conv_t0)/1e3;
    if(!w)return;
    {std::lock_guard<std::mutex> l(w->mu);if(w->q.size()<256)w->q.push_back(std::move(cur));}w->cv.notify_one();cur=Sample{};}
// load_shader_key: the key's stage, final SPIR-V (a copy: the writer thread counts it) and source (x translated, nr NR-04 table, f fused post)
void note_shader(const std::string &key,uint32_t stage,const std::vector<uint32_t> &spv,char src){
    if(!w)return;
    {std::lock_guard<std::mutex> l(w->mu);if(w->notes.size()<65536)w->notes.push_back(Note{nrh_key_id(key.c_str()),key,stage,src,spv});}}

// ---- the writer thread ----
struct Known{std::string key;uint32_t stage,words;char src;bool costed;nrh_cost c;};
std::unordered_map<uint64_t,Known> &known=*new std::unordered_map<uint64_t,Known>;   // writer thread only; heap (as dir)
const char *key_name(uint64_t id,void*){auto it=known.find(id);return it==known.end()?nullptr:it->second.key.c_str();}
void writer(){
#if defined(__linux__)
    setpriority(PRIO_PROCESS,0,10);pthread_setname_np(pthread_self(),"dm-nrheat");
#endif
    nrh_agg agg;if(nrh_agg_init(&agg,8192)){xbg_logf("【红】[nr] heat: no memory for the aggregation - heat off");return;}
    nrh_out out{};bool out_ok=false,out_tried=false;std::unordered_set<uint64_t> emitted;
    std::vector<std::string> lvname{"-"};std::unordered_map<std::string,uint32_t> lvid{{"-",0}};uint32_t level=0;int gen=-1;
    uint64_t last_flush=timing_clock(),last_line=last_flush,last_sample_ns=0,last_frame=0,pend_t=0;
    struct Fsum{uint64_t frame=0,unix_s=0,t_ns=0;uint32_t slots=0,passes=0,cpasses=0,skipped=0;uint64_t draws=0;double span=0,pass_us=0,compute_us=0,conv=0;uint32_t stamps=0;}F;
    struct Minute{uint32_t samples=0;double conv_us=0,span_us=0,pass_us=0;uint32_t passes=0,stamps=0;uint64_t draws=0;}M;
    char line[8192];
    const auto t_s=[&](uint64_t ns){return ns>boot_ns?double(ns-boot_ns)/1e9:0.0;};
    const auto put=[&](const char *s,size_t n){if(out_ok)nrh_out_write(&out,s,n);};
    const auto header=[&]{if(!out_ok)return;char gpu[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE+1],app[64];snprintf(gpu,sizeof gpu,"%s",properties.deviceName);nrh_clean(gpu);
        snprintf(app,sizeof app,"%s",getenv("DM_APP_VERSION")?getenv("DM_APP_VERSION"):"?");nrh_clean(app);
        int n=snprintf(line,sizeof line,"H\tv=%d\tunix=%llu\tmode=%d\tperiod_s=%.0f\tfirst_s=%.0f\tburst=%u\tflush_s=%.0f\tgpu=%s\tvendor=0x%04x\tdevice=0x%04x\tdriver=0x%08x\tts_ns=%.3f\tapp=%s\n",
            NRH_FORMAT_VERSION,(unsigned long long)time(nullptr),mode,period_s,first_s,burst,flush_s,gpu,properties.vendorID,properties.deviceID,properties.driverVersion,
            double(properties.limits.timestampPeriod),app);
        if(n>0 && size_t(n)<sizeof line)put(line,size_t(n));emitted.clear();};
    const auto open_out=[&]{if(out_ok || out_tried)return;out_tried=true;   // the run's first record opens the file; H starts the run's records
        if(dir.empty() || nrh_out_open(&out,dir.c_str(),"shader-heat.tsv",cap_bytes)){xbg_logf("【红】[nr] heat: cannot open %s/shader-heat.tsv - the log stays off (lines only)",dir.c_str());return;}
        out_ok=true;header();};
    const auto rotate_if=[&](size_t more){if(out_ok && nrh_out_full(&out,more)){if(nrh_out_rotate(&out)){out_ok=false;xbg_logf("【红】[nr] heat: rotation of %s failed - the log stops",out.path);return;}header();
        xbg_logf("[nr] heat: %s reached %llu KB, the previous one kept as %s",out.path,(unsigned long long)(cap_bytes>>10),out.old);}};
    const auto finish_frame=[&]{if(!F.frame)return;
        const double secs=last_sample_ns && F.t_ns>last_sample_ns?double(F.t_ns-last_sample_ns)/1e9:0;const double fps=secs>0 && F.frame>last_frame?double(F.frame-last_frame)/secs:0;
        int n=snprintf(line,sizeof line,"F\t%llu\t%.0f\t%s\t%llu\t%u\t%u\t%u\t%llu\t%.0f\t%.0f\t%.0f\t%.1f\t%u\n",(unsigned long long)F.unix_s,t_s(F.t_ns),lvname[level].c_str(),
            (unsigned long long)F.frame,F.slots,F.passes,F.cpasses,(unsigned long long)F.draws,F.span,F.pass_us,F.compute_us,fps,F.skipped);
        if(n>0 && size_t(n)<sizeof line){rotate_if(size_t(n));put(line,size_t(n));if(out_ok)fflush(out.f);}
        ++M.samples;M.conv_us+=F.conv;M.span_us+=F.span;M.pass_us+=F.pass_us;M.passes+=F.passes+F.cpasses;M.draws+=F.draws;M.stamps+=F.stamps;
        last_sample_ns=F.t_ns;last_frame=F.frame;F=Fsum{};};
    const auto flush=[&](const char *why){(void)why;uint32_t lv[NRH_LEVELS];const int nl=nrh_agg_levels(&agg,lv,NRH_LEVELS);
        if(!nl){nrh_agg_reset(&agg);return;}open_out();
        std::string buf;buf.reserve(size_t(agg.n)*120+1024);
        for(int k=0;k<nl;k++){const uint32_t L=lv[k];
            for(uint32_t i=0;i<agg.cap;i++){const nrh_row &r=agg.rows[i];if(!r.used || r.k.level!=L || !r.passes)continue;   // S lines first: keys of this block not in the file yet
                for(uint64_t id:{r.k.vs,r.k.fs})if(id && !emitted.count(id)){auto it=known.find(id);if(it==known.end())continue;emitted.insert(id);char hx[17];nrh_key_hex(id,hx);
                    const Known &K=it->second;const nrh_cost &c=K.c;int n;
                    if(K.costed)n=snprintf(line,sizeof line,"S\t%s\t%u\t%u\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n",id>>63?K.key.c_str():hx,K.stage,K.words,K.src=='n'?"nr":K.src=='f'?"f":"x",
                        c.inst,c.alu,c.trans,c.tex,c.branch,c.loops,c.ubo,c.ubo_dyn,c.bitcast,c.cost);
                    else n=snprintf(line,sizeof line,"S\t%s\t%u\t%u\t%s\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\n",id>>63?K.key.c_str():hx,K.stage,K.words,K.src=='n'?"nr":K.src=='f'?"f":"x");
                    if(n>0 && size_t(n)<sizeof line)buf.append(line,size_t(n));}}
            int n=nrh_fmt_block(line,sizeof line,lvname[L<lvname.size()?L:0].c_str(),&agg.lt[L],uint64_t(time(nullptr)),t_s(timing_clock()));if(n>0)buf.append(line,size_t(n));
            for(uint32_t i=0;i<agg.cap;i++){const nrh_row &r=agg.rows[i];if(!r.used || r.k.level!=L || !r.passes)continue;n=nrh_fmt_row(line,sizeof line,&r,key_name,nullptr);if(n>0)buf.append(line,size_t(n));}}
        if(agg.dropped){int n=snprintf(line,sizeof line,"# rows dropped (table full): %llu\n",(unsigned long long)agg.dropped);if(n>0)buf.append(line,size_t(n));}
        rotate_if(buf.size());put(buf.data(),buf.size());if(out_ok)fflush(out.f);nrh_agg_reset(&agg);};
    xbg_logf("[nr] heat: writer started (file %s/shader-heat.tsv)",dir.c_str());
    for(;;){std::deque<Sample> got;std::vector<Note> notes;
        {std::unique_lock<std::mutex> l(w->mu);w->cv.wait_for(l,std::chrono::seconds(1),[&]{return !w->q.empty();});got.swap(w->q);notes.swap(w->notes);}
        for(auto &n:notes){Known k{n.key,n.stage,uint32_t(n.spv.size()),n.src,false,{}};k.costed=nrh_spirv_cost(n.spv.data(),n.spv.size(),&k.c)==0;known[n.id]=std::move(k);}
        const auto costof=[&](uint64_t id)->float{if(!id)return -1;auto it=known.find(id);return it!=known.end() && it->second.costed?float(it->second.c.cost):-1.0f;};
        const uint64_t now=timing_clock();
        // the location (hle_dos2_heat.c; other games: none)
        if(dm_dos2_heat_level){char c[64],p[64];double kb=0;const int g=dm_dos2_heat_level(c,sizeof c,p,sizeof p,&kb);
            if(g!=gen){const bool first=gen<0;gen=g;std::string name=c;auto it=lvid.find(name);uint32_t id;
                if(it==lvid.end()){id=uint32_t(lvname.size()<NRH_LEVELS?lvname.size():NRH_LEVELS-1);if(lvname.size()<NRH_LEVELS){lvname.push_back(name);lvid.emplace(name,id);}}else id=it->second;
                if(!first || id!=level){finish_frame();flush("location");level=id;open_out();
                    int n=snprintf(line,sizeof line,"L\t%llu\t%.0f\t%s\t%.0f\t%s\n",(unsigned long long)time(nullptr),t_s(now),c,kb,p);
                    if(n>0 && size_t(n)<sizeof line){rotate_if(size_t(n));put(line,size_t(n));if(out_ok)fflush(out.f);}}}}
        for(auto &s:got){
            if(F.frame && s.frame!=F.frame)finish_frame();
            if(!F.frame){F.frame=s.frame;F.unix_s=s.unix_s;open_out();}
            pend_t=s.t_ns;F.t_ns=s.t_ns;++F.slots;F.span+=s.span_us;F.conv+=s.convert_us;F.stamps+=s.stamps;F.skipped+=s.skipped;
            for(auto &g:s.progs){g.cvs=costof(g.vs);g.cfs=costof(g.fs);}   // static costs of the keys (unknown: -1)
            std::vector<nrh_pass> ps(s.passes.size());
            for(size_t i=0;i<s.passes.size();i++){const PassS &q=s.passes[i];ps[i]=nrh_pass{q.compute,q.w,q.h,q.fmt,q.att,q.us,q.draws,q.verts,q.np,s.progs.data()+q.p0};
                if(q.compute){++F.cpasses;F.compute_us+=q.us;}else{++F.passes;F.pass_us+=q.us;F.draws+=q.draws;}
                if(mode==2){int n=nrh_fmt_pass(line,sizeof line,s.frame,uint32_t(i),&ps[i],key_name,nullptr);if(n>0){rotate_if(size_t(n));put(line,size_t(n));}}}
            nrh_agg_add(&agg,level,s.frame,ps.data(),uint32_t(ps.size()));nrh_agg_span(&agg,level,s.frame,s.span_us);}
        if(F.frame && got.empty() && now-pend_t>500000000ull)finish_frame();   // no more parts of that frame for 0.5 s
        if(mode==2 && last_sample_ns && !F.frame && now-last_sample_ns>2000000000ull && agg.n){flush("burst");}
        if(now-last_flush>=uint64_t(flush_s*1e9)){finish_frame();flush("period");last_flush=now;}
        if(now-last_line>=60000000000ull){last_line=now;
            if(M.samples){uint32_t top[3];const int k=nrh_agg_top(&agg,level,top,3);const nrh_ltot &T=agg.lt[level<NRH_LEVELS?level:0];const double fr=T.frames?double(T.frames):1;
                std::string tops;for(int i=0;i<k;i++){const nrh_row &r=agg.rows[top[i]];char a[17],b[17];const char *vs=r.k.vs?(r.k.vs>>63 && key_name(r.k.vs,nullptr)?key_name(r.k.vs,nullptr):(nrh_key_hex(r.k.vs,a),a)):"-";
                    const char *fs=r.k.fs?(r.k.fs>>63 && key_name(r.k.fs,nullptr)?key_name(r.k.fs,nullptr):(nrh_key_hex(r.k.fs,b),b)):"-";
                    char t[200];snprintf(t,sizeof t,"%s%s %s/%s %ux%u %.2f ms",i?"; ":"",r.k.kind?"cs":"",vs,fs,r.k.w,r.k.h,r.us_d/fr/1e3);tops+=t;}
                xbg_logf("[nr] heat (60 s, %s): %u samples, a sample %.1f ms GPU span, passes %.1f ms, %.0f passes, %.0f draws, %.0f stamps; convert %.0f us a sample (translation thread); "
                    "top programs since the last block (%u frames): %s; file %s %llu KB (rotations %u)",lvname[level].c_str(),M.samples,M.span_us/M.samples/1e3,M.pass_us/M.samples/1e3,
                    double(M.passes)/M.samples,double(M.draws)/M.samples,double(M.stamps)/M.samples,M.conv_us/M.samples,T.frames,k?tops.c_str():"-",out_ok?out.path:"(none)",
                    (unsigned long long)(out_ok?out.bytes>>10:0),out_ok?out.rotations:0u);}
            M=Minute{};}
    }
}

// engine init, after DM_GFX_PASS_PROF and the device's timestamp bits
void init(){
    const char *e=getenv("DM_NR_HEAT");mode=e && *e?atoi(e):0;if(mode<0 || mode>2){xbg_logf("【红】[nr] heat DM_NR_HEAT=%s is not 0..2 - off",e);mode=0;}
    const auto num=[](const char *k,double d,double lo,double hi){const char *v=getenv(k);double x=v && *v?atof(v):d;return x<lo?lo:x>hi?hi:x;};
    period_s=num("DM_NR_HEAT_S",20,1,3600);first_s=num("DM_NR_HEAT_FIRST_S",30,0,3600);flush_s=num("DM_NR_HEAT_FLUSH_S",300,10,86400);
    burst=uint32_t(num("DM_NR_HEAT_BURST",120,1,100000));cap_bytes=uint64_t(num("DM_NR_HEAT_MB",4,1,64))<<20;
    const char *d=getenv("DM_NR_HEAT_DIR");if(!d || !*d)d=getenv("DM_LOG_DIR");if(!d || !*d)d=getenv("DM_RUNTIME_DIR");
    if(d && *d)dir=d;else{dir=cfg.shader_directory?cfg.shader_directory:"";size_t cut=dir.find_last_of('/');if(cut!=std::string::npos)dir.resize(cut);}
    boot_ns=timing_clock();
    if(!mode)return;
    if(!timestamp_bits){xbg_logf("[nr] heat DM_NR_HEAT=%d: the queue has no timestamps - off",mode);mode=0;return;}
    xbg_logf("[nr] heat DM_NR_HEAT=%d: %s every %.0f s (first after %.0f s), pass-boundary timestamps only; blocks every %.0f s and at a location change; %s/shader-heat.tsv (%llu MB, one previous kept)%s",
        mode,mode==2?"a burst of frames":"one frame",period_s,first_s,flush_s,dir.c_str(),(unsigned long long)(cap_bytes>>20),pass_prof_enabled?"; DM_GFX_PASS_PROF is on: heat reads its frames":"");
    if(mode==2)xbg_logf("[nr] heat: burst %u frames, P lines per pass",burst);
    start();}
}   // namespace nrheat
