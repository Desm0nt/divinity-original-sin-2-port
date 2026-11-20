// [DOS2] NR-07: DM_NR_PIPE - the boot warm-up ordered by engine events (docs/dos2/nr/NR-07.md; scheduler in dm/gfx/nr/nr_pipe.h).
// Included by xbg_game.cpp inside namespace warm, right before worker() (it needs Rec, recs, mu, build(), stop, ready, the counters).
// Hooks, one token / line each in xbg_game.cpp:
//   warm::worker()           if(nr::mode){nr::worker();return;}   - the scheduler picks the records instead of next.fetch_add
//   warm::start()            nr::start() after the list is read, nr::threads(n) for the thread count
//   warm::shutdown()         nr::wake() next to async_cv.notify_all(), nr::reset() at the end
//   warm::take()             nr::took(bytes) when a draw takes a ready pipeline
//   pipeline_lookup()        || nr::need(bytes,out.pipeline) after warm::take; nr::DrawBuild around the draw path's own driver build
//   extern "C" (file scope)  xbg_nr_pipe_hint -> nr::hint, xbg_nr_pipe_source -> nr::source (hle_dos2_nr.c, DM_NR=1 / 2: every program
//                            the game sets up)
// Record stage keys go through nrp_stage_key: a fused post stage of the backend (DM_NB_POST "F1." / "F1A." ... + host key) counts as its
// host shader's key, so the game's program with the host (EoCSight) hints it (NR-0407 fix: before, such a stage parsed as 0 = never
// hinted, and a draw that needed the record was a red "hint hole" line; the warm build itself could not read the fused bundle - see
// warm::bundle_words / nbpost::warm_bundle).
// DM_NR_PIPE (read once by start(); the front end reads it too, for the hints):
//   0 / unset  nothing: worker() runs its old loop, need() / took() are behind one load and a not-taken branch, no memory.
//   1          the scheduler: hinted records first (every stage key named by a program the game set up), then list order; a draw that
//              misses the ready set and needs a record in flight waits for it (DM_NR_PIPE_WAIT_MS, default 500) instead of compiling a
//              second copy; a record the draw path builds is claimed (the warm-up skips it); workers start no build while the draw path
//              builds one (serial drivers: PowerVR, old Mali with a process-wide compiler lock). Without hints (DM_NR=0) list order.
//   2          check / count: list order as before (no claim, no wait, nothing reordered); hints, build and need times recorded; lines say
//              how many in-frame builds of listed records were hinted before and how long (the gain of 1), how many met the record in
//              flight (the double builds 1 saves), and red lines for records a draw needed whose keys no program ever named (hint holes).
//   3          lazy: as 1, but only hinted records are built (none in list order). For slow compilers / little RAM: the session builds
//              the areas it loads, not every pipeline the device ever saw. Needs the hints (DM_NR=1 / 2), else it behaves as 1.
//              NR-29 DM_NR_MEM=1 / 3 (default 1) turns 1 into 3 on phones of the 6 GB class or smaller (dm_nr_mem_pipe_lazy).
// DM_NR_PIPE_THREADS=<n> overrides the warm-up thread count (default min(3, cores / 3), unchanged).
namespace nr{
int mode=0;bool read=false;std::atomic<int> source{0};   // source: the front end sends hints (DM_NR on + DM_NR_PIPE)
nrp_sched S{};bool have=false;std::condition_variable cv;std::unordered_map<std::string_view,uint32_t> idx;   // create-info bytes -> record
std::vector<uint64_t> early;   // hints before start() (keys); applied there
std::atomic<int> draw_builds{0};int active=0;uint64_t t_start=0,t_last=0,wait_ns=500000000ull;unsigned told=0;bool finished=false;
struct DrawBuild{bool on;DrawBuild():on(mode==1 || mode==3){if(on)++draw_builds;}~DrawBuild(){if(on)--draw_builds;}};
int read_mode(){if(!read){read=true;const char *e=getenv("DM_NR_PIPE");mode=e && *e?std::clamp(atoi(e),0,3):0;
    if(const char *w=getenv("DM_NR_PIPE_WAIT_MS"))wait_ns=uint64_t(std::clamp(atoi(w),0,5000))*1000000ull;
    if(mode==1 && dm_nr_mem_pipe_lazy && dm_nr_mem_pipe_lazy()){mode=3;xbg_logf("[nr] DM_NR_PIPE=1 -> 3 (lazy) by DM_NR_MEM: a 6 GB class phone or smaller builds only the pipelines of programs the game set up");}}   // [DOS2] NR-29
    return mode;}
unsigned threads(unsigned n){if(const char *e=getenv("DM_NR_PIPE_THREADS"))if(*e)n=unsigned(std::clamp(atoi(e),1,8));return n;}
void lead_text(const uint64_t *b,char *out,size_t cap){int k=0;out[0]=0;for(int i=0;i<NRP_LEAD_BUCKETS && k<int(cap)-24;i++)if(b[i])k+=snprintf(out+k,cap-size_t(k)," %s %llu",nrp_lead_name(i),(unsigned long long)b[i]);if(!k)snprintf(out,cap," -");}
void report(uint64_t now,const char *why){   // mu held
    if(!have)return;t_last=now;const nrp_stats &t=S.st;char lead[160],tl[160];lead_text(t.lead,lead,sizeof lead);lead_text(t.took_lead,tl,sizeof tl);
    xbg_logf("[nr] pipe (%s, DM_NR_PIPE=%d%s, %.1f s after start): %u records; built %llu from hints + %llu in list order, failed %llu, left %u (ready not taken %u); "
             "hints %llu: %llu keys with records (%llu records queued), %llu keys no record has (new to this list), %llu repeats; draws: took %llu ready (hinted before %llu:%s; list order %llu), "
             "missed %llu listed records (ready %llu, in flight %llu, not built %llu, failed %llu, claimed %llu; hinted before %llu:%s; never hinted %llu), waited %llu (ready %llu, timed out %llu), "
             "claimed %llu, list builds skipped as claimed %llu",
             why,mode,source.load()?"":" no hints",(now-t_start)/1e9,S.nrec,(unsigned long long)t.built_hint,(unsigned long long)t.built_list,(unsigned long long)t.failed,S.left,nrp_count(&S,NRP_READY),
             (unsigned long long)t.hints,(unsigned long long)t.hint_keys_known,(unsigned long long)t.queued,(unsigned long long)t.hint_keys_new,(unsigned long long)t.hint_dup,
             (unsigned long long)t.taken,(unsigned long long)t.took_hinted,tl,(unsigned long long)t.took_unhinted,(unsigned long long)t.need,(unsigned long long)t.need_ready,
             (unsigned long long)t.need_inflight,(unsigned long long)t.need_free,(unsigned long long)t.need_failed,(unsigned long long)t.need_claimed,(unsigned long long)t.need_hinted,lead,
             (unsigned long long)t.need_unhinted,(unsigned long long)t.waited,(unsigned long long)t.wait_ready,(unsigned long long)t.wait_timeout,(unsigned long long)t.claimed,(unsigned long long)t.dup_avoided);}
void tick(uint64_t now){if(have && now-t_last>=10000000000ull)report(now,"10 s");}   // mu held
void start(){   // the list is read (recs); before the threads start
    if(!read_mode())return;
    if(mode==3 && !source.load()){xbg_logf("[nr] DM_NR_PIPE=3 without hints (DM_NR=0 or no front end): lazy needs them - runs as 1");mode=1;}
    std::lock_guard<std::mutex> l(mu);
    const uint32_t n=uint32_t(recs.size());uint32_t nfused=0;std::vector<uint64_t> keys(size_t(n)*NRP_STAGES,0);std::vector<uint8_t> nk(n,0);
    for(uint32_t i=0;i<n;i++){nk[i]=uint8_t(std::min<size_t>(recs[i].keys.size(),NRP_STAGES));for(uint32_t k=0;k<nk[i];k++){int f=0;keys[size_t(i)*NRP_STAGES+k]=nrp_stage_key(recs[i].keys[k].data(),recs[i].keys[k].size(),&f);nfused+=uint32_t(f);}}   // NR-0407 fix: a fused post stage = its host's key
    if(nrp_init(&S,n,keys.data(),nk.data(),mode==3)){xbg_logf("[nr] DM_NR_PIPE=%d: no memory for the scheduler - off",mode);mode=0;return;}
    S.lazy=mode==3;S.shadow=mode==2;have=true;idx.reserve(n*2+1);
    for(uint32_t i=0;i<n;i++)idx.emplace(std::string_view(reinterpret_cast<const char*>(recs[i].words.data()),recs[i].words.size()*4),i);
    t_start=t_last=timing_clock();for(uint64_t k:early)nrp_hint(&S,k,t_start);const size_t ne=early.size();early.clear();early.shrink_to_fit();
    xbg_logf("[nr] DM_NR_PIPE=%d (NR-07: %s): %u warm records, %u shader keys (%u stages are our fused post bundles: hinted by the program of the host shader they replace), "
             "%zu hints before the start; hints %s; a draw waits up to %llu ms for a record in flight",mode,
             mode==1?"warm-up ordered by engine events, no double builds":mode==2?"check - list order as before, hints / build / need times counted":"lazy - only records whose shaders the game set up",
             n,S.nkent,nfused,ne,source.load()?"from every program the game sets up (DM_NR tap)":"off (DM_NR=0: list order)",(unsigned long long)(wait_ns/1000000));}
void worker(){
    setpriority(PRIO_PROCESS,0,5);   // as the old worker: below the game and graphics threads
    for(;;){int i=-1,fh=0;
        {std::unique_lock<std::mutex> l(mu);
            for(;;){if(stop.load())return;
                if(mode!=2 && draw_builds.load()){l.unlock();std::this_thread::sleep_for(std::chrono::milliseconds(1));l.lock();continue;}   // the draw path builds: let it have the driver
                i=nrp_next(&S,&fh);if(i>=0)break;
                if(mode==3){cv.wait_for(l,std::chrono::milliseconds(250));continue;}   // lazy: park until a hint queues something
                if(!finished && S.qhead>=S.qtail){finished=true;report(timing_clock(),"list done");}
                return;}
            ++active;}
        const uint64_t t0=timing_clock();const bool ok=build(recs[size_t(i)]);if(ok)++built;else ++failed;const uint64_t t1=timing_clock();build_ns+=t1-t0;
        {std::lock_guard<std::mutex> l(mu);--active;nrp_done(&S,uint32_t(i),ok,t1);tick(t1);}
        cv.notify_all();}
}
// A draw takes record bytes' ready pipeline (warm::take, mu held)
void took(const std::string &bytes){if(!have)return;auto f=idx.find(std::string_view(bytes));if(f==idx.end())return;const uint64_t now=timing_clock();nrp_took(&S,f->second,now);tick(now);}
// warm::take missed: the draw needs bytes. Mode 1 / 3: a record in flight is waited for (true = out is its pipeline); a free one is claimed.
// Mode 2: counted only. false = build it as before (async request or in the frame).
bool need(const std::string &bytes,VkPipeline &out){
    std::unique_lock<std::mutex> l(mu);if(!have)return false;
    auto f=idx.find(std::string_view(bytes));if(f==idx.end())return false;const uint32_t i=f->second;const uint64_t now=timing_clock();
    const bool first=!S.rec[i].needed;const int was=nrp_need(&S,i,now,mode!=2);
    if(first && source.load() && !S.rec[i].t_hint && told<16){++told;std::string k;for(auto &x:recs[i].keys){if(!k.empty())k+='+';k+=x;}
        xbg_logf("【红】[nr] pipe check: a draw needs warm record %u (%s, %s) but no program the game set up named all its shaders (a hint hole: DM_NR_PIPE=3 would build it in the frame)",
                 i,k.c_str(),nrp_state_name(was));}
    tick(now);
    if(mode==2)return false;
    auto grab=[&]{auto it=ready.find(bytes);if(it==ready.end())return false;out=it->second;ready.erase(it);nrp_took(&S,i,timing_clock());return true;};
    if(was==NRP_READY)return grab();
    if(was!=NRP_BUILDING || !wait_ns)return false;
    const bool done=cv.wait_for(l,std::chrono::nanoseconds(wait_ns),[&]{return S.rec[i].phase!=NRP_BUILDING || stop.load();});
    const bool got=done && S.rec[i].phase==NRP_READY && grab();nrp_wait_end(&S,got);
    return got;}   // timed out: the draw builds its own copy; the worker's lands in ready (as before NR-07)
void wake(){cv.notify_all();}
void reset(){if(have){report(timing_clock(),"shutdown");nrp_free(&S);}have=false;idx.clear();early.clear();active=0;finished=false;told=0;}
// A program the game set up: its stage's shader pack key (16 hex digits). Any thread. (extern "C" xbg_nr_pipe_hint, outside the
// anonymous namespace, calls it.)
void hint(const char *key,unsigned len){
    const uint64_t k=nrp_key_parse(key,len);if(!k)return;
    std::lock_guard<std::mutex> l(mu);
    if(!have){if(early.size()<65536)early.push_back(k);return;}
    if(nrp_hint(&S,k,timing_clock()))cv.notify_all();}
}
