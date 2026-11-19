// [DOS2] NR-ZERO Z5 (docs/dos2/nr/NR-ZERO.md section 14): DM_NR_Z5 - the recorder packets of a run of engine draws written apart from the
// translation thread. Included by xbg_game.cpp right before xbg_nr_draw.inc (the lane's tail calls draw_begin / draw_end around its
// command section). The run assembler is dm/gfx/nr/nr_z5.h, the chunk builder and the stream comparison xbg_nb_mt.inc (Build, Z5Sink,
// same_stream), the hand-over at a position nbmtq::Queue::splice (nb_mt.h); host test tests/nr_z5_test.cpp.
//
// What is built: the packet side (a chunk of a run's packets, handed over whole at the position of its first draw; the head of a run
// - the draws that read a bind-cache field the run has not set - stays the translation thread's) and its comparison on the device.
// What is NOT built: the game thread's half - resolving a draw (pipeline, set, ring offsets, views, viewport) from published tables
// while the translator is still behind. So:
//   DM_NR_Z5   0  default (xbg_nr_z5_default): the lane's tail pays one load and a not-taken branch twice a draw.
//              2  verify, nothing is recorded differently: for every engine draw the lane's tail records, the run assembler - fed with
//                 the values the tail holds and a model of the bind cache that starts EMPTY at every run - writes the draw's packets
//                 into a chunk of its own when the draw reads only fields the run already set; they are compared, packet for packet,
//                 with the packets the tail just wrote (same operation, same arguments, same array contents: raw bytes cannot be equal -
//                 a packet holds pointers into its own chunk and the address of its thunk). After every draw the model's known fields
//                 must be the real bind cache's; before every draw of a run the real cache must still hold them (else something moved
//                 the cache without writing a packet: the run ends there, counted - a game thread could not have seen it).
//                 Counted too: the runs (a run ends at any packet that is not an engine draw's, at another command buffer, at a UI
//                 draw, at a conditional-rendering bracket), their lengths, head and tail draws - the numbers the decision needs.
//                 Needs DM_NB_MT != 0 (packets) and DM_NR_DRAW=1.
//              1  apply: the game thread's half (xbg_nr_z5g.inc; NR-ZERO.md section 22) - the game thread writes the packets of the draws it
//                 has values for, the translation thread takes them draw by draw. The comparison below does not run. =2 runs both:
//                 this comparison and the game thread's chunks against the translation thread's packets.
// Lines: "[nr] DM_NR_Z5=..." at the start, "[nr] z5 check (DM_NR_Z5=2): ..." every 10 s, red "【红】[nr] z5: ...".
#include "../../nr/nr_z5.h"
namespace nrz5t{bool init(int m);void release();}   // xbg_nr_z5g.inc: the game thread's half (DM_NR_Z5 = 1 / 2)
namespace nrz5{
int mode=0;bool read=false;
nrz5_cache model{};bool in_run=false,live=false;VkCommandBuffer run_cmd{};uint32_t run_len=0;uint64_t sig_after=0;
nbmtq::Chunk *scratch=nullptr,*c0=nullptr;uint32_t u0=0;uint64_t ch0=0;
enum{E_OTHER,E_CMD,E_CACHE,E_UI,E_COND,E_DIRECT,E_N};
struct Stats{uint64_t draws=0,runs=0,run_max=0,head=0,tail=0,cmp=0,same=0,bad=0,skipped=0,nofit=0,cache_chk=0,cache_bad=0,end[E_N]{},tail_cmds=0,tail_bytes=0,len_sum=0,len_n=0,len_ge[4]{};} s;
uint64_t next_ns=0;unsigned told=0;
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
void red(const char *fmt,...){if(told>=16)return;++told;char b[640];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof b,fmt,ap);va_end(ap);
    xbg_logf("【红】[nr] z5: %s (frame %llu, vs=%s ps=%s)",b,(unsigned long long)stats.presents.load(),state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-");}
// From nrz3::init, once NR-09 read its switch.
void init(int draw_mode){
    if(read)return;read=true;
    const char *e=getenv("DM_NR_Z5");int m=e && *e?atoi(e):xbg_nr_z5_default();
    if(m<0 || m>2){xbg_logf("【红】[nr] DM_NR_Z5=%d: only 0 / 1 / 2: off",m);m=0;}
    if(m && (nbmt::mode==0 || draw_mode!=1)){xbg_logf("【红】[nr] DM_NR_Z5=%d needs DM_NB_MT (now %d) and DM_NR_DRAW=1 (now %d): off",m,nbmt::mode,draw_mode);m=0;}
    if(m && !nrz5t::init(m))m=0;   // the game thread's half (xbg_nr_z5g.inc): 1 apply, 2 verify
    if(m==1)m=0;   // (below: the comparison of the run assembler on the translation thread - =2 only)
    if(m){scratch=static_cast<nbmtq::Chunk*>(malloc(sizeof(nbmtq::Chunk)+16384));if(!scratch){xbg_logf("【红】[nr] DM_NR_Z5=2: no memory: off");m=0;}else{memset(scratch,0,sizeof *scratch);scratch->cap=16384;}}
    if(m)xbg_logf("[nr] DM_NR_Z5=2 (NR-ZERO Z5, verify: nothing is recorded differently): every engine draw the lane's tail records is written again by the run assembler over a model of the bind cache "
        "that starts empty at each run - a draw that reads only fields its run set is compared packet for packet with the tail's packets; the model's known fields against the real bind cache "
        "after every draw; runs counted (ended by any other packet, another command buffer, a moved cache, a UI draw, a conditional bracket)");
    mode=m;}
void release(){nrz5t::release();free(scratch);scratch=nullptr;mode=0;read=false;in_run=false;live=false;s=Stats{};}
inline nrz5_cache conv(const BindCache &bc){
    nrz5_cache c;nrz5_cache_unknown(&c);c.known=NRZ5_K_PIPE|NRZ5_K_SET|NRZ5_K_VP|NRZ5_K_SC|NRZ5_K_IB;c.known_vb=0xffff;
    c.pipe=uint64_t(uintptr_t(bc.pipe));c.layout=uint64_t(uintptr_t(bc.layout));c.set=uint64_t(uintptr_t(bc.set));c.noff=bc.noff;memcpy(c.off,bc.off,sizeof c.off);
    c.has_vp=bc.has_vp;c.has_sc=bc.has_sc;memcpy(c.vp,&bc.vp,sizeof bc.vp);memcpy(c.sc,&bc.sc,sizeof bc.sc);
    for(int i=0;i<16;i++){c.vb[i]=uint64_t(uintptr_t(bc.vb[i]));c.vbo[i]=bc.vbo[i];}
    c.ib=uint64_t(uintptr_t(bc.ib));c.ibo=bc.ibo;c.it=uint32_t(bc.it);return c;}
static_assert(sizeof(((BindCache*)nullptr)->off)==sizeof(((nrz5_cache*)nullptr)->off) && sizeof(VkViewport)==NRZ5_VP_BYTES && sizeof(VkRect2D)==NRZ5_SC_BYTES,"nr_z5.h: the bind cache's offsets, viewport and scissor");
// what was written into the command buffer so far: packets, and the calls made past the packets (a drain: pNext, no memory, a caller)
inline uint64_t sig(){const nbmtq::Stats &q=nbmt::q.st;return q.cmds*8+q.sync[nbmtq::NBMT_SY_PNEXT]+q.sync[nbmtq::NBMT_SY_ALLOC]+q.sync[nbmtq::NBMT_SY_CALLER];}
inline void end_run(int why){if(!in_run)return;in_run=false;++s.end[why];
    if(run_len){s.len_sum+=run_len;++s.len_n;if(run_len>s.run_max)s.run_max=run_len;if(run_len>=2)++s.len_ge[0];if(run_len>=4)++s.len_ge[1];if(run_len>=8)++s.len_ge[2];if(run_len>=16)++s.len_ge[3];}}
// lane_tail, right after it took the bind cache of the command buffer (bc), before its first command.
inline void draw_begin(VkCommandBuffer cmd,const BindCache &bc,bool ui){
    live=false;
    if(nbmt::cur!=cmd){end_run(E_DIRECT);return;}   // this submission is recorded by direct calls
    if(ui){end_run(E_UI);return;}                   // a UI draw of the lane is not an engine record
    if(in_run){
        if(cmd!=run_cmd)end_run(E_CMD);
        else if(sig()!=sig_after)end_run(E_OTHER);
        else{const nrz5_cache real=conv(bc);if(nrz5_cache_diff(&model,&real))end_run(E_CACHE);}}   // the cache moved and no packet was written
    if(!in_run){nrz5_cache_unknown(&model);in_run=true;run_cmd=cmd;run_len=0;++s.runs;}
    c0=nbmt::q.cur;u0=c0?c0->used:0;ch0=nbmt::q.st.chunks;live=true;}
// lane_tail, right after the draw command (and the end of a conditional bracket). The values are the tail's: H and ds as it bound them.
inline void draw_end(const BindCache &bc,VkPipeline pipe,uint16_t used,const VkBuffer *vb,const uint64_t *vbo,VkBuffer ib,uint64_t ibo,VkIndexType it,const VkViewport &vp,const VkRect2D &sc,
                     VkPipelineLayout layout,VkDescriptorSet set,uint32_t no,const uint32_t *off,bool indexed,uint32_t count,uint32_t instances,uint32_t first,int32_t vertex_offset,uint32_t first_instance,bool cond){
    if(!live)return;live=false;
    if(cond){end_run(E_COND);return;}   // vkCmdBegin/EndConditionalRendering bracket this draw: not a run draw
    nrz5_vals v;memset(&v,0,sizeof v);v.pipe=uint64_t(uintptr_t(pipe));v.layout=uint64_t(uintptr_t(layout));v.set=uint64_t(uintptr_t(set));v.noff=no;v.off=off;
    memcpy(v.vp,&vp,sizeof vp);memcpy(v.sc,&sc,sizeof sc);v.used=used;
    for(uint32_t m=used;m;m&=m-1){const uint32_t i=uint32_t(__builtin_ctz(m));v.vb[i]=uint64_t(uintptr_t(vb[i]));v.vbo[i]=vbo[i];}
    v.indexed=indexed;v.it=uint32_t(it);v.ib=uint64_t(uintptr_t(ib));v.ibo=ibo;v.count=count;v.instances=instances;v.first=first;v.first_instance=first_instance;v.vertex_offset=vertex_offset;
    ++s.draws;++run_len;
    if(nrz5_reads_unknown(&model,&v)){nrz5_after(&model,&v);++s.head;}   // a head draw: only the translation thread can record it
    else{++s.tail;scratch->used=scratch->cmds=0;nbmt::Build b{scratch};nbmt::Z5Sink sink{b};
        const unsigned n=nrz5_emit(sink,&model,&v);
        if(!n){++s.nofit;nrz5_after(&model,&v);}   // (more than the scratch chunk holds: not compared)
        else{nbmtq::Chunk *cc=c0?c0:nbmt::q.cur;const uint32_t from=c0?u0:0;   // (no current chunk before the draw: its first packet opened one)
            if(cc && cc==nbmt::q.cur && ch0==nbmt::q.st.chunks){uint32_t at=0;++s.cmp;s.tail_cmds+=n;s.tail_bytes+=scratch->used;
                if(nbmt::same_stream(cc->data()+from,cc->used-from,scratch->data(),scratch->used,&at))++s.same;
                else{++s.bad;red("the packets of a draw written ahead are not the tail's: packet %u of %u (tail wrote %u bytes, the assembler %u; run length %u)",at,n,cc->used-from,scratch->used,run_len);}}
            else ++s.skipped;}}   // a hand-over inside the draw: its packets are no longer the producer's to read
    {const nrz5_cache real=conv(bc);const int d=nrz5_cache_diff(&model,&real);++s.cache_chk;
     if(d){++s.cache_bad;red("after a draw the model of the bind cache differs from the real one: %s",d==1?"pipeline":d==2?"descriptor set / offsets":d==3?"viewport":d==4?"scissor":d==5?"a vertex stream":"index buffer");end_run(E_CACHE);}}
    sig_after=sig();}
void tick(){   // every submission (nrdraw::tick) while the switch is on
    const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns || !s.draws)return;next_ns=now+10000000000ull;
    const double ln=s.len_n?double(s.len_n):1.;
    xbg_logf("[nr] z5 check (DM_NR_Z5=2): %llu engine draws recorded by the lane's tail in %llu runs (ended: another packet %llu, another command buffer %llu, the bind cache moved without a packet %llu, "
        "a UI draw %llu, a conditional bracket %llu, direct recording %llu); run length %.1f on average, longest %llu, >= 2: %.0f%%, >= 4: %.0f%%, >= 8: %.0f%%, >= 16: %.0f%% of the runs; head draws %llu "
        "(read a field the run had not set: the translation thread's), tail draws %llu (%.1f%% - could be written ahead: %.2f packets and %.0f bytes each); compared %llu, same %llu, bad %llu, "
        "not compared: a hand-over inside the draw %llu, too large %llu; bind cache after a draw: checked %llu, bad %llu",
        (unsigned long long)s.draws,(unsigned long long)s.runs,(unsigned long long)s.end[E_OTHER],(unsigned long long)s.end[E_CMD],(unsigned long long)s.end[E_CACHE],(unsigned long long)s.end[E_UI],
        (unsigned long long)s.end[E_COND],(unsigned long long)s.end[E_DIRECT],double(s.len_sum)/ln,(unsigned long long)s.run_max,100.*double(s.len_ge[0])/ln,100.*double(s.len_ge[1])/ln,100.*double(s.len_ge[2])/ln,
        100.*double(s.len_ge[3])/ln,(unsigned long long)s.head,(unsigned long long)s.tail,100.*double(s.tail)/double(s.draws),s.cmp?double(s.tail_cmds)/double(s.cmp):0.,s.cmp?double(s.tail_bytes)/double(s.cmp):0.,
        (unsigned long long)s.cmp,(unsigned long long)s.same,(unsigned long long)s.bad,(unsigned long long)s.skipped,(unsigned long long)s.nofit,(unsigned long long)s.cache_chk,(unsigned long long)s.cache_bad);
    s=Stats{};}
}
