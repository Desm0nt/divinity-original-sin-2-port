// [DOS2] NB-21: the frame job on the backend (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-21 findings"; layout, validity and the compare in
// dm/gfx/nb/nb_own.h - the host test tests/nb_own_test.c runs the same code). Included by xbg_game.cpp right before execute().
// DM_NB_OWN 1 / 2 (the front end reads the same switch): the translation queue calls mtl_nb_own(job, handles, n) before it executes a
// submission of the frame IR and mtl_nb_own(NULL, NULL, 0) after it (hle_nvn.c XOWN_RUN / XOWN_DONE). While a job is current, nbown::cur
// is the whole part before its first record is executed: FRAME record, IR skeleton, every segment's PASS / KEY address and recording in
// stream order (nbw_stop gives its end), the execution order. execute() reports the FRAME / PASS records it meets (two calls behind
// `if(nbown::cur)`); at the end the two are compared and counted. Nothing is executed differently here: the unit is for NB-20b (a worker
// takes a segment from the job) and for nbord (a schedule without parsing the tail). Line "[nb] own (backend ...)" every 10 s.
// DM_NB_OWN 0: mtl_nb_own is never called, cur stays null.
#include "../../nb/nb_own.h"
namespace nbown{
int mode=-1;
const nbw_job *cur=nullptr;    // the frame job of the submission being executed (translation thread)
nbw_chk ck;
struct Stats{uint64_t jobs,invalid[NBW_V_KINDS],same,differ,bit[NBW_D_BITS],segs,job_order,finals,split,keys,nested,frames_no_job,timed,ns;}s{};
unsigned told=0;uint64_t next_ns=0,begin_ns=0;bool timed_job=false;
// DM_NB_MX_PROF: a sum that moves when a shared cache gets an entry or a texture is uploaded (a segment that moves it is not "quiet")
uint64_t noise(){return uint64_t(pipeline_counts.misses)+hud::uploads.load(std::memory_order_relaxed)+images.size()+layout_cache.size()+sampler_cache.size()+render_pass_cache.size()+persistent_framebuffers.size()+fb_creates;}
void read_mode(){const char *e=getenv("DM_NB_OWN");mode=e && *e?atoi(e):0;if(mode<1 || mode>2)mode=0;
    if(mode)xbg_logf("[nb] DM_NB_OWN=%d: a submission of the frame IR comes with its frame job (segments, order); the records executed are compared with it (NB-21)",mode);}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    char b[400];int k=0;b[0]=0;
    for(int i=0;i<NBW_D_BITS && k<int(sizeof b)-48;i++)if(s.bit[i])k+=snprintf(b+k,sizeof b-size_t(k),"%s%s %llu",k?", ":"",nbw_diff_name(i),(unsigned long long)s.bit[i]);
    const double j=s.jobs?double(s.jobs):1.;
    xbg_logf("[nb] own (backend, DM_NB_OWN=%d, last 10 s): %llu frame jobs (%llu final, %.1f segments a job, with every key %llu, across recordings %llu), "
        "not valid %llu (magic %llu, counts %llu, address %llu, nodes %llu, order %llu); executed = the job %llu, differ %llu: %s; executed in the job's order "
        "and not the stream order %llu; a job begun inside another %llu; %.1f us a job in the compare (1 in 16 timed)",
        mode,(unsigned long long)s.jobs,(unsigned long long)s.finals,s.segs/j,(unsigned long long)s.keys,(unsigned long long)s.split,
        (unsigned long long)(s.invalid[NBW_V_MAGIC]+s.invalid[NBW_V_COUNTS]+s.invalid[NBW_V_ADDR]+s.invalid[NBW_V_NODES]+s.invalid[NBW_V_ORDER]),
        (unsigned long long)s.invalid[NBW_V_MAGIC],(unsigned long long)s.invalid[NBW_V_COUNTS],(unsigned long long)s.invalid[NBW_V_ADDR],
        (unsigned long long)s.invalid[NBW_V_NODES],(unsigned long long)s.invalid[NBW_V_ORDER],(unsigned long long)s.same,(unsigned long long)s.differ,k?b:"-",
        (unsigned long long)s.job_order,(unsigned long long)s.nested,s.timed?double(s.ns)/1e3/double(s.timed):0.);
    s=Stats{};}
void end(){
    if(!cur)return;
    if(nbx::prof)nbx::mark(2,-1,noise());
    const uint64_t e0=timed_job?timing_clock():0;
    const uint32_t d=nbw_chk_end(&ck);
    if(!d)++s.same;else{++s.differ;for(int i=0;i<NBW_D_BITS;i++)if(d>>i&1)++s.bit[i];
        if(told<16){++told;char b[300];int k=0;b[0]=0;
            for(int i=0;i<NBW_D_BITS && k<int(sizeof b)-40;i++)if(d>>i&1)k+=snprintf(b+k,sizeof b-size_t(k),"%s%s",k?", ":"",nbw_diff_name(i));
            xbg_logf("[nb] own: job seq=%u frame %u part %u (%u segments%s): executed %u FRAME, %u of its PASS records, %u records of another part - differs: %s",
                cur->seq,cur->frame_no,cur->part,unsigned(cur->nseg),cur->flags&NBW_F_MERGED?", not the stream order":"",ck.frames,ck.passes,ck.other,b);}}
    if(!ck.in_stream && ck.in_order)++s.job_order;
    if(timed_job){++s.timed;s.ns+=begin_ns+(timing_clock()-e0);timed_job=false;}   // begin + end only (the per-record calls are a few compares)
    cur=nullptr;tick();}
void begin(const nbw_job *j,const uint64_t *h,int n){
    if(mode<0)read_mode();
    if(cur){++s.nested;end();}
    const bool timed=!(s.jobs&15);const uint64_t t0=timed?timing_clock():0;
    ++s.jobs;
    const int v=nbw_valid(j);
    if(v){++s.invalid[v];if(told<16){++told;xbg_logf("[nb] own: a frame job is not valid (%s) - ignored",nbw_valid_name(v));}return;}
    s.segs+=j->nseg;if(j->flags&NBW_F_FINAL)++s.finals;if(j->flags&NBW_F_SPLIT)++s.split;if(j->flags&NBW_F_KEYS)++s.keys;
    nbw_chk_begin(&ck,j,h,n);cur=j;if(nbx::prof)nbx::mark(0,-1,noise());   // [DOS2] NB-20b ctx DM_NB_MX_PROF
    if(timed){timed_job=true;begin_ns=timing_clock()-t0;}}
// From execute(): rec = the record's header, p / n = its payload.
void frame(const uint8_t *rec,const uint8_t *p,size_t n){
    if(n<sizeof(p_nb_frame))return;
    p_nb_frame f;memcpy(&f,p,sizeof f);
    nbw_chk_frame(&ck,uint64_t(uintptr_t(rec)),f.seq,f.frame_no,f.part,f.nnode,f.npass,f.final);}
void pass(const uint8_t *rec,const uint8_t *p,size_t n){
    if(n<8)return;
    uint32_t seq;int16_t node;memcpy(&seq,p+NBO_PASS_SEQ_OFF,4);memcpy(&node,p+NBO_PASS_NODE_OFF,2);
    nbw_chk_pass(&ck,uint64_t(uintptr_t(rec)),seq,unsigned(uint16_t(node)));if(nbx::prof)nbx::mark(1,int(uint16_t(node)),noise());}
}
// The translation queue (hle_nvn.c, weak there): job != null before the submission of these handles is executed, null after it.
extern "C" void mtl_nb_own(const void *job,const uint64_t *h,int n){
    if(job)nbown::begin(static_cast<const nbw_job*>(job),h,n);else nbown::end();}
