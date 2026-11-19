// [DOS2] NR-ZERO Z5, the game thread's half (docs/dos2/nr/NR-ZERO.md section 22): DM_NR_Z5 = 1 / 2 - the game thread writes the recorder
// packets of the engine draws it has values for; the translation thread takes them draw by draw (=1) or only compares them with its
// own (=2). Included by xbg_game.cpp twice:
//   first (right after xbg_nr_z5.inc, before the lane)   namespace nrz5g - the game thread's side (its writer, the table of templates it
//                                       reads, the ring of runs) - and namespace nrz5t - the translation thread's side (the reader, the
//                                       hooks of nbdraw::apply and of the lane's tail, the template it publishes, the lines);
//   second (XBG_NR_Z5G_BRIDGE, at the end of the file, outside the unnamed namespace)   the two functions the NVN tap calls on the game
//                                       thread (dm/gfx/nvn/nvn_nb_draw.inc Z5G_*).
// The pure parts: dm/gfx/nr/nr_z5p.h (templates: what is published, how a prediction is made), nr_z5r.h (the run protocol with a verdict
// per draw), nr_z5.h (the command section over a model of the bind cache); host tests tests/nr_z5p_test.cpp, tests/nr_z5r_test.cpp.
//
// The game thread (nvn_nb_draw.inc nbd_draw, where an OP_X_NR_DRAW is packed): the record's site words (key id, draw arguments, index
// buffer, the address it last bound to vertex stream 0) pick a slot of the table (with how many records of that site this frame had);
// the slot's template + the uniform addresses the thread bound give the draw's values (nr_z5p.h) or nothing. With values the run
// writer (nr_z5r.h) writes the draw's packets into a spare chunk of the queue - or leaves the draw to the translation thread (a head
// draw). The record carries the slot (hdr._pad2 = slot + 1) and the mark word (hdr._pad1); both are 0 without the switch.
// The translation thread executes every record exactly as today. Two places know about Z5:
//   nbdraw::apply   rec_begin / rec_end around the record's draw(): the mark and the slot of the record in hand; a marked record whose
//                   draw never reached the lane's command section cuts its run (the prologue dropped it, it took the old tail).
//   lane_tail       tail() right where the command section starts, with the values the tail resolved. =1: Reader::draw - VALID: the
//                   command section is not run, the bind cache gets the draw's values (leave), the chunk stays held; while it is
//                   held nrsort::pend names the command buffer, so EVERY vkCmd* wrapper, nbmt::end and nbmt::sync hand it over first
//                   (NBMT_PRE -> nrsort::cut -> nrz5t::cut). RECORD: the section runs as today; recorded() publishes the template
//                   of the record's slot. =2: verify_begin / verify_end around the section, nothing held.
// Never taken: a UI draw, a draw inside a conditional-rendering bracket, a submission recorded by direct calls, DM_NR_SORT / DM_NR_MDI on.
//
//   DM_NR_Z5   0  default (xbg_nr_z5_default): the tap pays one load and a not-taken branch a draw record, nbdraw::apply and the lane's
//                 tail one each. No table, no thread-local state touched.
//              2  verify: the game thread predicts and writes ahead; the translation thread records everything itself and compares
//                 the packets of every draw =1 would have taken. Also runs xbg_nr_z5.inc's own comparison of the run assembler.
//              1  apply.
//              Needs DM_NB_MT != 0, DM_NR_DRAW=1, DM_NR_DIRECT / the NR-09 record (the tail's key id).
//   DM_NR_Z5_SLOTS  slots of the template table (8192; a power of two, 1024..65536; 372 bytes each)
//   DM_NR_Z5_MIN    a run's tail shorter than this is not handed over (4; 1..64)
// Lines: "[nr] DM_NR_Z5=... game thread ..." at the start, "[nr] z5 game: ..." every 10 s, red "【红】[nr] z5 game: ...".
#ifndef XBG_NR_Z5G_BRIDGE
#include "../../nr/nr_z5p.h"
#include "../../nr/nr_z5r.h"
extern "C" void nvn_nr_z5_enable(int on) __attribute__((weak));   // nvn_nb_draw.inc: arms the tap's hooks
namespace nrz5g{
std::atomic<int> armed{0};
nrz5p::Table tab;                 // written by the translation thread (nrz5t::recorded), read here; never freed
nrz5r::Ring ring;
uint32_t min_tails=4;
// The one thread that records engine draws owns this (the first thread that comes; any other thread's records stay unmarked).
std::atomic<uintptr_t> owner{0};
enum{G_RECORDS,G_NO_SLOT,G_NO_TEMPLATE,G_NO_VALUES,G_VALUES,G_HEADS,G_TAILS,G_RUNS,G_NO_CHUNK,G_RING_FULL,G_CHUNK_FULL,G_SHORT_RUNS,G_SHORT_TAILS,G_TAIL_BYTES,G_SIDE_BYTES,G_NS,G_TIMED,G_OTHER_THREAD,G_N};
std::atomic<uint64_t> pub[G_N];   // the owner's counters, published every 64 records (relaxed; read by nrz5t::tick)
struct G{nrz5r::Writer W;uint8_t occ[4096]{};uint32_t frame=~0u;const void *cb=nullptr;uint64_t n=0,c[G_N]{};bool init=false;} g;
inline uintptr_t self(){static thread_local char me;return uintptr_t(&me);}
inline uint32_t draw(const void *cb,const nbd_hdr *h,uint32_t key,const uint64_t *ub,uint64_t vb0,uint32_t frame,int32_t *slot){
    const uintptr_t id=self();uintptr_t o=owner.load(std::memory_order_relaxed);
    if(o!=id){if(o || !owner.compare_exchange_strong(o,id,std::memory_order_relaxed)){pub[G_OTHER_THREAD].fetch_add(1,std::memory_order_relaxed);return 0;}}
    if(!g.init){g.init=true;g.W.q=&nbmt::q;g.W.ring=&ring;g.W.min_tails=min_tails;}
    const bool timed=!(g.n&63);const uint64_t t0=timed?nbmtq::now_ns():0;++g.n;++g.c[G_RECORDS];
    if(frame!=g.frame){g.frame=frame;memset(g.occ,0,sizeof g.occ);}
    if(cb!=g.cb){g.W.end_run();g.cb=cb;}   // another command buffer: its records are executed somewhere else in the order
    uint32_t mark=0;
    if(!key || (h->draw!=NBD_DRAW_IDX && h->draw!=NBD_DRAW_ARRAYS) || (h->flags&(NBD_F_CHECK|NBD_F_REN|NBD_F_COND))){++g.c[G_NO_SLOT];g.W.no_values();}
    else{
        nrz5_site site;nrz5_site_make(&site,key,h->draw==NBD_DRAW_IDX?NRZ5S_IDX:NRZ5S_ARRAYS,h->prim,h->itype,h->count,h->base_vertex,h->base_instance,h->instances,h->first,h->ibuf,vb0);
        const uint32_t oi=uint32_t(nrz5_site_hash(&site,0)>>40)&4095u,oc=g.occ[oi];if(oc<255)g.occ[oi]=uint8_t(oc+1);
        const uint32_t s=tab.slot_of(&site,oc);*slot=int32_t(s+1);
        nrz5_tmpl t;nrz5_vals v;uint32_t off[NRZ5T_MAX_DYN];
        if(!tab.get(s,&t,nullptr)){++g.c[G_NO_TEMPLATE];g.W.no_values();}
        else if(!nrz5p_vals(&t,&site,h->count,h->instances,h->first,h->base_vertex,h->base_instance,[ub](unsigned st,unsigned sl){return st<6 && sl<16?ub[st*16+sl]:uint64_t(0);},&v,off)){++g.c[G_NO_VALUES];g.W.no_values();}
        else{++g.c[G_VALUES];
            mark=g.W.draw(v,[](nbmtq::Chunk *ch,nrz5_cache *m,const nrz5_vals *vv){nbmt::Build b{ch};nbmt::Z5Sink sink{b};return nrz5_emit(sink,m,vv);});}}
    if(timed){g.c[G_NS]+=nbmtq::now_ns()-t0;++g.c[G_TIMED];}
    if(!(g.n&63)){const nrz5r::WStats &w=g.W.st;g.c[G_HEADS]=w.heads;g.c[G_TAILS]=w.tails;g.c[G_RUNS]=w.runs;g.c[G_NO_CHUNK]=w.no_chunk;g.c[G_RING_FULL]=w.ring_full;g.c[G_CHUNK_FULL]=w.chunk_full;
        g.c[G_SHORT_RUNS]=w.short_runs;g.c[G_SHORT_TAILS]=w.short_tails;g.c[G_TAIL_BYTES]=w.tail_bytes;g.c[G_SIDE_BYTES]=w.side_bytes;
        for(int i=0;i<G_OTHER_THREAD;i++)pub[i].store(g.c[i],std::memory_order_relaxed);}
    return mark;}
// Any other record of the owner thread, the end of its recording: the run ends here (published before the recording is submitted).
inline void other(){if(owner.load(std::memory_order_relaxed)==self() && g.init)g.W.end_run();}
}
namespace nrz5t{
int gmode=0;                      // 0 off, 1 apply, 2 verify (set by nrz5::init)
bool held=false;VkCommandBuffer held_cmd{};
nrz5r::Reader R;
// the record in hand (nbdraw::apply): its header, whether it carries the NR tail, whether its draw reached the command section
nbd_hdr r_h{};bool r_have=false,r_done=false;
const nr_dplan *plan_hint=nullptr;   // nrz3::draw: the plan of the draw whose tail runs now (else looked up when a template is published)
// verify: the translation thread's own packets of the draw in hand
bool v_would=false;nbmtq::Chunk *v_c0=nullptr;uint32_t v_u0=0;uint64_t v_ch0=0;
struct Stats{uint64_t tails=0,marked=0,taken=0,ui=0,cond=0,direct=0,gate=0,published=0,pub_none=0,no_plan=0,missed=0,lost=0;} s;
uint64_t next_ns=0,last[nrz5g::G_N]{};unsigned told=0;nrz5r::RStats rlast;uint64_t puts_last=0,same_last=0;
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
void red(const char *fmt,...){if(told>=16)return;++told;char b[640];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof b,fmt,ap);va_end(ap);
    xbg_logf("【红】[nr] z5 game: %s (frame %llu, vs=%s ps=%s)",b,(unsigned long long)stats.presents.load(),state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-");}
// From nrz5::init (the translation thread), once DM_NB_MT and DM_NR_DRAW are known. m: 1 apply, 2 verify. false: refused (a red line).
bool init(int m){
    const char *e=getenv("DM_NR_Z5_SLOTS");uint32_t n=e && *e?uint32_t(std::clamp(atoi(e),1024,65536)):8192u;
    e=getenv("DM_NR_Z5_MIN");nrz5g::min_tails=e && *e?uint32_t(std::clamp(atoi(e),1,64)):4u;
    if(!nvn_nr_z5_enable){xbg_logf("【红】[nr] DM_NR_Z5=%d: this build's NVN tap has no Z5 hooks: off",m);return false;}
    if(!nrz5g::tab.s && !nrz5g::tab.init(n)){xbg_logf("【红】[nr] DM_NR_Z5=%d: no memory for %u templates: off",m,n);return false;}
    R=nrz5r::Reader{};R.q=&nbmt::q;R.ring=&nrz5g::ring;held=false;r_have=false;s=Stats{};
    if(nbmt::q.spare_max<256)nbmt::q.spare_max=256;   // two chunks a run in flight
    gmode=m;nrz5g::armed.store(1,std::memory_order_release);nvn_nr_z5_enable(1);
    xbg_logf("[nr] DM_NR_Z5=%d game thread (NR-ZERO Z5: %s): where it writes an OP_X_NR_DRAW the game thread looks the record's slot up in a table of %u templates (%zu KB, published by the translation thread for the "
        "records it recorded itself: pipeline, set, the uniform slot and base of each dynamic offset, folded vertex / index views, viewport, scissor), and with values writes the draw's packets into a spare chunk of the "
        "recorder queue; the translation thread executes every record as today and takes a draw only when the bind cache holds what the run's model held in every field the draw reads and the values it resolved equal "
        "the written ones; the chunk is handed over before any other vkCmd*; a tail shorter than %u draws is not handed over",
        m,m==2?"verify - the translation thread records everything itself and compares the packets of every draw =1 would take":"apply",nrz5g::tab.n,size_t(nrz5g::tab.n)*sizeof(nrz5p::Slot)/1024,nrz5g::min_tails);
    return true;}
// Everything written ahead goes back; the tap stops. (The table and the ring stay: the game thread may be inside them.)
void release(){if(!gmode)return;if(nvn_nr_z5_enable)nvn_nr_z5_enable(0);nrz5g::armed.store(0,std::memory_order_release);
    if(held){held=false;nrsort::pend.store(VK_NULL_HANDLE,std::memory_order_relaxed);}R.drop();gmode=0;r_have=false;}
inline void sync_pend(VkCommandBuffer cmd){
    if(R.holding && gmode==1){if(!held){held=true;held_cmd=cmd;nrsort::pend.store(cmd,std::memory_order_relaxed);}}
    else if(held){held=false;nrsort::pend.store(VK_NULL_HANDLE,std::memory_order_relaxed);}}
// NBMT_PRE (through nrsort::cut): something else is about to be written into the command buffer the held chunk belongs to.
void cut(){R.flush(nrz5r::CUT_WRITE);held=false;nrsort::pend.store(VK_NULL_HANDLE,std::memory_order_relaxed);}
// A new command buffer began while a chunk was held (never expected: nbmt::end hands it over): its draws are lost with their buffer.
void lost(){if(R.holding){R.holding=false;nbmt::q.give_back_spare(R.run.chunk);nbmt::q.give_back_spare(R.run.side);++s.lost;red("a held chunk of %u taken draws outlived its command buffer: given back",R.j);}
    held=false;nrsort::pend.store(VK_NULL_HANDLE,std::memory_order_relaxed);}
// nbdraw::apply, before the draw() of a record.
void rec_begin(const nbd_hdr &h){r_h=h;r_have=(h.flags&NBD_F_NR)!=0 && !(h.flags&NBD_F_CHECK);r_done=false;}
// ... and after it: a marked record that did not reach the command section of the lane's tail.
void rec_end(){
    if(r_have && !r_done && r_h._pad1){++s.missed;
        if(R.holding){if(gmode==1){R.flush(nrz5r::CUT_RECORD);sync_pend(held_cmd);}else R.verify_miss();}
        else R.abandon(r_h._pad1);}
    r_have=false;}
inline uint64_t sig(){const nbmtq::Stats &q=nbmt::q.st;return q.cmds*8+q.sync[nbmtq::NBMT_SY_PNEXT]+q.sync[nbmtq::NBMT_SY_ALLOC]+q.sync[nbmtq::NBMT_SY_CALLER];}
inline bool is_draw(uint16_t op){return op==nbmt::OP_Draw || op==nbmt::OP_DrawIndexed;}
inline void vals_of(nrz5_vals &v,VkPipeline pipe,uint16_t used,const VkBuffer *vb,const uint64_t *vbo,VkBuffer ib,uint64_t ibo,VkIndexType it,const VkViewport &vp,const VkRect2D &sc,VkPipelineLayout layout,
                    VkDescriptorSet set,uint32_t no,const uint32_t *off,bool indexed,uint32_t count,uint32_t instances,uint32_t first,int32_t vertex_offset,uint32_t first_instance){
    memset(&v,0,sizeof v);v.pipe=uint64_t(uintptr_t(pipe));v.layout=uint64_t(uintptr_t(layout));v.set=uint64_t(uintptr_t(set));v.noff=no;v.off=off;
    memcpy(v.vp,&vp,sizeof vp);memcpy(v.sc,&sc,sizeof sc);v.used=used;
    for(uint32_t m=used;m;m&=m-1){const uint32_t i=uint32_t(__builtin_ctz(m));v.vb[i]=uint64_t(uintptr_t(vb[i]));v.vbo[i]=vbo[i];}
    v.indexed=indexed;v.count=count;v.instances=instances;v.first=first;v.first_instance=first_instance;
    if(indexed){v.it=uint32_t(it);v.ib=uint64_t(uintptr_t(ib));v.ibo=ibo;v.vertex_offset=vertex_offset;}}
// What the command section leaves in the bind cache after a draw with these values (lane_tail's assignments, without its commands).
inline void leave(BindCache &bc,const nrz5_vals &v){
    bc.pipe=VkPipeline(uintptr_t(v.pipe));
    if(v.noff<=32){bc.layout=VkPipelineLayout(uintptr_t(v.layout));bc.set=VkDescriptorSet(uintptr_t(v.set));bc.noff=v.noff;if(v.noff)memcpy(bc.off,v.off,size_t(v.noff)*4);}else bc.noff=UINT32_MAX;
    memcpy(&bc.vp,v.vp,sizeof bc.vp);bc.has_vp=true;memcpy(&bc.sc,v.sc,sizeof bc.sc);bc.has_sc=true;
    for(uint32_t m=v.used;m;m&=m-1){const uint32_t i=uint32_t(__builtin_ctz(m));bc.vb[i]=VkBuffer(uintptr_t(v.vb[i]));bc.vbo[i]=v.vbo[i];}
    if(v.indexed){bc.ib=VkBuffer(uintptr_t(v.ib));bc.ibo=v.ibo;bc.it=VkIndexType(v.it);}}
// lane_tail, right after it took the bind cache of the command buffer, before its first command; the values are the tail's.
// true (=1 only): the draw is taken - its packets are in the held chunk, the bind cache holds its values, the command section is not run.
bool tail(VkCommandBuffer cmd,BindCache &bc,bool ui,bool cond,VkPipeline pipe,uint16_t used,const VkBuffer *vb,const uint64_t *vbo,VkBuffer ib,uint64_t ibo,VkIndexType it,const VkViewport &vp,const VkRect2D &sc,
          VkPipelineLayout layout,VkDescriptorSet set,uint32_t no,const uint32_t *off,bool indexed,uint32_t count,uint32_t instances,uint32_t first,int32_t vertex_offset,uint32_t first_instance){
    v_would=false;++s.tails;
    if(R.holding && held && held_cmd!=cmd)lost();
    const bool rec=r_have && !r_done && !ui;if(!ui)r_done=true;
    const uint32_t mw=rec?r_h._pad1:0u;if(mw)++s.marked;
    // never taken: this submission is recorded by direct calls; a UI draw; a conditional bracket; switches that record the draw another way
    int no_take=0;
    if(nbmt::cur!=cmd){no_take=1;++s.direct;}else if(ui){no_take=1;++s.ui;}else if(cond){no_take=1;++s.cond;}else if(nrsort::mode!=0 || nrmdi::mode>0){no_take=1;++s.gate;}
    if(no_take){
        if(R.holding){if(gmode==1){R.flush(nrz5r::CUT_RECORD);sync_pend(cmd);}else R.verify_miss();}
        if(mw)R.abandon(mw);
        return false;}
    nrz5_vals rv;vals_of(rv,pipe,used,vb,vbo,ib,ibo,it,vp,sc,layout,set,no,off,indexed,count,instances,first,vertex_offset,first_instance);
    auto holds=[&bc](const nrz5_cache &sh,const nrz5_vals &v){const nrz5_cache real=nrz5::conv(bc);return nrz5r::cache_holds(&sh,&real,&v);};
    if(gmode==1){
        const int a=R.draw(mw,rv,holds,is_draw);
        if(a==nrz5r::Reader::VALID){leave(bc,rv);++s.taken;sync_pend(cmd);return true;}
        sync_pend(cmd);return false;}
    v_would=R.verify_begin(mw,rv,holds,is_draw,sig());
    if(v_would){++s.taken;v_c0=nbmt::q.cur;v_u0=v_c0?v_c0->used:0;v_ch0=nbmt::q.st.chunks;}
    return false;}
// lane_tail, after the command section it ran itself (the draw was not taken). =2: the comparison of a draw =1 would have taken.
// Then the template of the record's slot: what this draw bound, described so that the game thread can write the next one like it.
void recorded(const BindCache &bc,bool ui,bool cond,uint32_t key,VkPipeline pipe,uint16_t used,const VkBuffer *vb,const uint64_t *vbo,VkBuffer ib,uint64_t ibo,VkIndexType it,const VkViewport &vp,const VkRect2D &sc,
              VkPipelineLayout layout,VkDescriptorSet set,uint32_t no,const uint32_t *off,bool indexed,uint32_t count,uint32_t instances,uint32_t first,int32_t vertex_offset,uint32_t first_instance,uint32_t first_index,uint32_t k){
    (void)bc;
    if(v_would){v_would=false;
        nbmtq::Chunk *cc=v_c0?v_c0:nbmt::q.cur;const uint32_t from=v_c0?v_u0:0;const bool readable=cc && cc==nbmt::q.cur && v_ch0==nbmt::q.st.chunks;   // (else a hand-over inside the draw: its packets are no longer the producer's to read)
        const uint64_t bad0=R.st.v_bad;
        R.verify_end(readable?cc->data()+from:nullptr,readable?cc->used-from:0,[](const uint8_t *a,uint32_t na,const uint8_t *b,uint32_t nb){uint32_t at=0;return nbmt::same_stream(a,na,b,nb,&at);},sig());
        if(R.st.v_bad!=bad0)red("the packets the game thread wrote for a draw are not the packets the tail recorded (key %u, the values compared equal and the bind cache held the run's model)",key);
        return;}   // (its template gave the right values: nothing to publish)
    if(ui || cond || !r_have || r_h._pad2<=0 || !key)return;
    nrz5_vals v;vals_of(v,pipe,used,vb,vbo,ib,ibo,it,vp,sc,layout,set,no,off,indexed,count,instances,first,vertex_offset,first_instance);
    nrz5_site site;nrz5_site_make(&site,key,r_h.draw==NBD_DRAW_IDX?NRZ5S_IDX:NRZ5S_ARRAYS,r_h.prim,r_h.itype,r_h.count,r_h.base_vertex,r_h.base_instance,r_h.instances,r_h.first,r_h.ibuf,state.vb[0].address);
    // the uniform slot of each dynamic offset: the plan of the shader set (NR-08)
    const nr_dplan *p=plan_hint;
    if(!p && no && nrdesc::mode==1 && (nrdesc::z_out==NRZD_HIT || nrdesc::z_out==NRZD_BUILT)){const nrdesc::Plan *pl=nrdesc::plan_for();if(pl && !pl->p.why)p=&pl->p;}
    if(no && (!p || p->ndyn!=no))++s.no_plan;
    nrz5_tmpl t;
    nrz5_tmpl_make(&t,&site,&v,first_index,k,[p,no](uint32_t i,unsigned *st,unsigned *sl,uint64_t *a){
        if(!p || p->ndyn!=no)return false;
        for(unsigned u=0;u<p->nubo;u++){const nr_drow &r=p->row[p->ubo_row[u]];if(r.kind==NRD_K_RING_DYN && r.dyn_at==i){*st=r.ns;*sl=r.slot;*a=r.ns<6 && r.slot<16?uint64_t(state.ub[r.ns][r.slot].address):0;return true;}}
        return false;});
    if(!(t.flags&NRZ5T_F_OK))++s.pub_none;
    if(nrz5g::tab.put(uint32_t(r_h._pad2-1),&t))++s.published;}
void tick(){   // every submission (nrdraw::tick) while the switch is on
    const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    uint64_t g[nrz5g::G_N];for(int i=0;i<nrz5g::G_N;i++){const uint64_t x=nrz5g::pub[i].load(std::memory_order_relaxed);g[i]=x-last[i];last[i]=x;}
    using namespace nrz5g;
    if(!g[G_RECORDS] && !s.tails)return;
    const nrz5r::RStats r=R.st;auto d=[&](uint64_t nrz5r::RStats::*f){return r.*f-rlast.*f;};
    const double rec=g[G_RECORDS]?double(g[G_RECORDS]):1.,runs=g[G_RUNS]?double(g[G_RUNS]):1.,tails=s.tails?double(s.tails):1.;
    xbg_logf("[nr] z5 game (DM_NR_Z5=%d): game thread: %llu engine draw records, values for %llu (%.1f%%; no slot %llu, slot empty or being written %llu, template of another site / says no / unknown uniform address %llu), "
        "head draws %llu, tail draws written ahead %llu (%.1f%% of the records; %.0f bytes of packets and %.0f bytes of values each) in %llu runs (%.1f tail draws a run; published without chunks: %llu runs with %llu tail draws; "
        "no spare chunk %llu, ring full %llu, chunk full %llu), records of another thread %llu, %.0f ns a record (1 in 64)",
        gmode,(unsigned long long)g[G_RECORDS],(unsigned long long)g[G_VALUES],100.*double(g[G_VALUES])/rec,(unsigned long long)g[G_NO_SLOT],(unsigned long long)g[G_NO_TEMPLATE],(unsigned long long)g[G_NO_VALUES],
        (unsigned long long)g[G_HEADS],(unsigned long long)g[G_TAILS],100.*double(g[G_TAILS])/rec,g[G_TAILS]?double(g[G_TAIL_BYTES])/double(g[G_TAILS]):0.,g[G_TAILS]?double(g[G_SIDE_BYTES])/double(g[G_TAILS]):0.,
        (unsigned long long)g[G_RUNS],double(g[G_TAILS])/runs,(unsigned long long)g[G_SHORT_RUNS],(unsigned long long)g[G_SHORT_TAILS],(unsigned long long)g[G_NO_CHUNK],(unsigned long long)g[G_RING_FULL],
        (unsigned long long)g[G_CHUNK_FULL],(unsigned long long)g[G_OTHER_THREAD],g[G_TIMED]?double(g[G_NS])/double(g[G_TIMED]):0.);
    const uint64_t dv=r.diff[1]+r.diff[2]+r.diff[3]+r.diff[4]+r.diff[5]+r.diff[6]+r.diff[7]+r.diff[8]-(rlast.diff[1]+rlast.diff[2]+rlast.diff[3]+rlast.diff[4]+rlast.diff[5]+rlast.diff[6]+rlast.diff[7]+rlast.diff[8]);
    xbg_logf("[nr] z5 game, translation thread: %llu draws at the tail's command section (marked %llu; UI %llu, conditional %llu, direct recording %llu, DM_NR_SORT / DM_NR_MDI %llu), %s %llu (%.1f%%); "
        "runs met %llu, followed %llu: complete %llu, cut by: the bind cache did not hold the model %llu, values differ %llu (pipeline %llu, set %llu, offsets %llu, viewport %llu, scissor %llu, vertex %llu, index %llu, arguments %llu; %llu in all), "
        "chunk ends early %llu, a record did not come %llu, another command %llu; first tail draw not taken %llu; chunks %s %llu; runs gone %llu, never reached %llu, published without chunks %llu; "
        "marked records that did not reach the command section %llu; held chunks lost %llu; templates published %llu (already there %llu; described as 'no prediction' %llu, plan not in hand %llu)",
        (unsigned long long)s.tails,(unsigned long long)s.marked,(unsigned long long)s.ui,(unsigned long long)s.cond,(unsigned long long)s.direct,(unsigned long long)s.gate,gmode==2?"would take":"taken",
        (unsigned long long)s.taken,100.*double(s.taken)/tails,(unsigned long long)d(&nrz5r::RStats::runs),(unsigned long long)d(&nrz5r::RStats::started),(unsigned long long)d(&nrz5r::RStats::complete),
        (unsigned long long)(r.cut[nrz5r::CUT_CACHE]-rlast.cut[nrz5r::CUT_CACHE]),(unsigned long long)(r.cut[nrz5r::CUT_VALUES]-rlast.cut[nrz5r::CUT_VALUES]),
        (unsigned long long)(r.diff[1]-rlast.diff[1]),(unsigned long long)(r.diff[2]-rlast.diff[2]),(unsigned long long)(r.diff[3]-rlast.diff[3]),(unsigned long long)(r.diff[4]-rlast.diff[4]),(unsigned long long)(r.diff[5]-rlast.diff[5]),
        (unsigned long long)(r.diff[6]-rlast.diff[6]),(unsigned long long)(r.diff[7]-rlast.diff[7]),(unsigned long long)(r.diff[8]-rlast.diff[8]),(unsigned long long)dv,
        (unsigned long long)(r.cut[nrz5r::CUT_CHUNK]-rlast.cut[nrz5r::CUT_CHUNK]),(unsigned long long)(r.cut[nrz5r::CUT_RECORD]-rlast.cut[nrz5r::CUT_RECORD]),(unsigned long long)(r.cut[nrz5r::CUT_WRITE]-rlast.cut[nrz5r::CUT_WRITE]),
        (unsigned long long)d(&nrz5r::RStats::first_no),gmode==2?"that would be handed over":"handed over",(unsigned long long)d(&nrz5r::RStats::handed),(unsigned long long)d(&nrz5r::RStats::gone),(unsigned long long)d(&nrz5r::RStats::unreached),
        (unsigned long long)d(&nrz5r::RStats::declined_writer),(unsigned long long)s.missed,(unsigned long long)s.lost,(unsigned long long)s.published,(unsigned long long)(tab.same-same_last),(unsigned long long)s.pub_none,(unsigned long long)s.no_plan);
    if(gmode==2 || r.v_bad!=rlast.v_bad)
        xbg_logf("[nr] z5 game check (DM_NR_Z5=2): draws =1 would take: compared %llu, same %llu, bad %llu, not compared (a hand-over inside the draw) %llu",(unsigned long long)d(&nrz5r::RStats::v_cmp),
            (unsigned long long)d(&nrz5r::RStats::v_same),(unsigned long long)d(&nrz5r::RStats::v_bad),(unsigned long long)d(&nrz5r::RStats::v_skipped));
    rlast=r;same_last=tab.same;puts_last=tab.puts;s=Stats{};}
}
#else
// ======== second part: the game thread's entry points (called by the NVN tap, dm/gfx/nvn/nvn_nb_draw.inc) ========
// An OP_X_NR_DRAW is being written. h: its header as packed; key: the engine's key id of its tail; ub: the addresses the thread last
// bound to the uniform slots ([6][16]); vb0: to vertex stream 0; frame: the tap's present count. Returns the record's mark word,
// *slot = the record's slot + 1 (0: none).
extern "C" uint32_t xbg_nr_z5_game_draw(const void *cb,const nbd_hdr *h,uint32_t key,const uint64_t *ub,uint64_t vb0,uint32_t frame,int32_t *slot){
    *slot=0;if(!nrz5g::armed.load(std::memory_order_acquire))return 0;return nrz5g::draw(cb,h,key,ub,vb0,frame,slot);}
// Another record follows on the thread that wrote engine draws, or its recording ends.
extern "C" void xbg_nr_z5_game_other(void){if(nrz5g::armed.load(std::memory_order_acquire) && nrz5g::owner.load(std::memory_order_relaxed))nrz5g::other();}
#endif
