// [DOS2] NR-TCOUNT DM_NR_TCOUNT (docs/dos2/nr/NR-TCOUNT.md; analysis dm/gfx/nr/nr_tcount.h, host test tests/nr_tcount_test.cpp): tiler plan
// step 3 (PERF-X19 T3 T4 T5), COUNTING ONLY. Included by xbg_game.cpp right after namespace pgraph, whose recorder is the frame model:
// pgraph::present arms a frame for the pass-graph dump (DM_GFX_PASS_GRAPH, every 10 s), for this count (1 frame in DM_NR_TCOUNT_EVERY) or
// both; at the next present the armed frame's passes / ops / events are copied into an nrtc_frame and analysed. Nothing is drawn, ordered
// or skipped differently; the armed frame takes the recorder's slow paths (no descriptor memo, no NR-09 draw lane: pgraph sees every
// read), so CPU time of the sampled frames is not representative - the count is.
//   DM_NR_TCOUNT        0 (default) nothing (present() reads one int); 1 count: lines every 10 s
//   DM_NR_TCOUNT_EVERY  sample 1 frame in N (default 4, 1..600)
// Lines: "[nr] tcount T3 ...", "[nr] tcount T4 ...", "[nr] tcount T5 ...", "[nr] tcount top: ..." (4 lines: xbg_logf cuts at 1023 B).
static_assert(OP_SIGNAL_EVENT==4 && OP_REPORT_COUNTER==5 && OP_COPY_B2B==6 && OP_UPDATE_UB==7 && OP_COPY_T2B==34 && OP_X_RESOLVE==46 && OP_X_TYPED_CLEAR==49 &&
              OP_X_CLEAR_BUF==53 && OP_X_DRAW_TEX==55,"NR-TCOUNT: nrtc_opname's table and the C / B event split follow nvn_obj.h");
namespace tcount{
int mode=-1;unsigned every=4;uint64_t n=0,next_ns=0;nrtc_acc acc;nrtc_frame fr;
void init(){const char *e=getenv("DM_NR_TCOUNT");int v=e && *e?atoi(e):0;if(v<0 || v>1){xbg_logf("【红】[nr] DM_NR_TCOUNT=%d: only 0..1: off",v);v=0;}mode=v;
    if(const char *p=getenv("DM_NR_TCOUNT_EVERY"))if(*p){const int k=atoi(p);if(k>=1 && k<=600)every=unsigned(k);else xbg_logf("【红】[nr] DM_NR_TCOUNT_EVERY=%d: only 1..600: %u",k,every);}
    if(mode)xbg_logf("[nr] tcount (NR-TCOUNT): DM_NR_TCOUNT=%d, 1 frame in %u (DM_NR_TCOUNT_EVERY) through the pass-graph recorder; count only (T3 VS-only draws, T4 re-opened targets, T5 copy / clear breaks); lines every 10 s",mode,every);}
bool on(){if(__builtin_expect(mode<0,0))init();return mode>0;}
bool want(){return (n++%every)==0;}
// pgraph's frame -> nrtc_frame (reads keep image + how; the event kinds C split into query / report and buffer writes by the command stream)
void frame(){
    fr.imgs.resize(pgraph::imgs.size());for(size_t i=0;i<pgraph::imgs.size();i++){const auto &m=pgraph::imgs[i];fr.imgs[i]=nrtc_img{m.serial,m.fmt,m.w,m.h,m.depth};}
    auto rds=[](const std::vector<pgraph::Rd> &v,std::vector<nrtc_rd> &o){o.clear();o.reserve(v.size());for(const auto &r:v)o.push_back(nrtc_rd{r.img,r.how});};
    fr.passes.resize(pgraph::passes.size());
    for(size_t i=0;i<pgraph::passes.size();i++){const pgraph::PassR &r=pgraph::passes[i];nrtc_pass &q=fr.passes[i];
        q.att=r.att;q.w=r.w;q.h=r.h;q.kind=r.kind;q.clear=r.clear;q.store=r.store;q.dc=r.dc;q.draws=r.draws;q.vso=r.vso;q.vsw=r.vsw;
        q.first_vso=r.first_vso;q.last_vso=r.last_vso;q.first_vsw=r.first_vsw;q.last_vsw=r.last_vsw;q.ui=r.ui;q.buf_write=r.buf_write;q.side=r.side;q.sub=r.sub;
        rds(r.in,q.in);q.xout=r.xout;q.end_op=r.end_op;q.end_line=r.end_line;q.end_img=r.end_img;q.end_why=r.end_why;q.ended=r.ended;
        q.fs=r.fs.size()>=8?uint32_t(strtoul(r.fs.substr(0,8).c_str(),nullptr,16)):0;q.vs=r.vs;}
    fr.ops.resize(pgraph::ops.size());
    for(size_t i=0;i<pgraph::ops.size();i++){const pgraph::OpR &o=pgraph::ops[i];nrtc_op &q=fr.ops[i];q.k=o.k;rds(o.in,q.in);q.out=o.out;q.host_op=o.host_op;q.buf_write=o.buf_write;}
    fr.evs.clear();fr.evs.reserve(pgraph::evs.size());
    for(const auto &e:pgraph::evs){if(e.k=='S')continue;   // a submit is the pass's sub number
        if(e.k=='C')fr.evs.push_back(nrtc_ev{(e.i==OP_SIGNAL_EVENT || e.i==OP_REPORT_COUNTER)?'C':'B',e.i});else fr.evs.push_back(nrtc_ev{e.k,e.i});}
    fr.lost_reads=pgraph::rd_lost;
    nrtc_analyze(fr,acc);}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    if(!acc.frames)return;std::string l[4];nrtc_lines(acc,mode,every,l);for(auto &s:l)xbg_logf("%s",s.c_str());acc=nrtc_acc{};}
}
