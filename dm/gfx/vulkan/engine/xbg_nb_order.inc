// [DOS2] NB-17a / NB-17b: the passes of a submission executed in the order the plan gives (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-17a
// findings"; tail layout, schedule, scan and the run loop in dm/gfx/nb/nb_order.h - the host test tests/nb_order_test.c runs the same
// code). Included by xbg_game.cpp right before execute(). DM_NB_ORDER (read once; the front end reads the same switch):
//   0  off: execute() walks the handle as recorded (one never-taken compare a record, one at OP_END).
//   1  identity: at OP_X_NB_FRAME the order tail is read and the part is executed segment by segment (nbo_run -> execute() on each
//      segment's records, stopping at the next segment's PASS record) in the tail's order, which is the stream order: the same records in
//      the same order, so the Vulkan command stream is the one of 0. A tail that is missing, not valid or not the stream order = as recorded.
//   2  check: executed as recorded; the tail's order (the merge planner's, NB-16) is verified: every PASS record where the tail says
//      (nbo_scan), complete key frames (NB-14) for every start that is not continuous, no inversion without a state-only side unless the
//      planner tracked the hazards. Counted only.
//   3  the verified order is executed: a segment that does not start where the previous one ended starts from its key (nbkey::start +
//      nbpipe::reset: the state is written directly); OP_X_NB_STATE DEF blocks of the part are registered first (a REF may now run before
//      its DEF); after the run the state the stream ends with is put back (kept after the stream's last segment). A pass that continues the
//      one before it (NBO_E_CONT) needs nothing here: the host keeps its Vulkan pass open across the same target set; the unit's store ops
//      (nbrp::open) = DONT_CARE only where the unit's first and last header both say so, its barrier-before covers every member's reads.
//      Anything that does not verify = that part as recorded (counted by reason).
// Line "[nb] order (backend ...)" every 10 s while FRAME records come.
#include "../../nb/nb_order.h"
namespace nbord{
static_assert(offsetof(p_nb_pass,seq)==NBO_PASS_SEQ_OFF && offsetof(p_nb_pass,node)==NBO_PASS_NODE_OFF,"nb_order.h: OP_X_NB_PASS seq / node offsets");
static_assert(sizeof(p_nb_frame)<=NBO_TAIL_OFF,"nb_order.h: the tail starts after p_nb_frame");
int mode=-1;                 // DM_NB_ORDER
bool in_run=false;           // a part is being executed by its schedule (execute() of a segment is below us)
unsigned run_depth=0;        // CALL depth of the handle that holds the part
const uint8_t *stop=nullptr; // execute() returns before the record at this address (never matches while off)
const uint8_t *end_pc=nullptr;   // the OP_END a segment ran into (not executed)
int cur_e=-1;                // execution index of the segment being executed (-1: prefix / none)
uint32_t cur_seq=0;
nbo_sched sc;nbo_scan_out so;nbo_ent ent[NBO_MAX_SEG];
State kept;bool kept_missing=false;
struct Def{uint8_t kind;uint16_t id;uint32_t epoch;const uint8_t *p;};
enum{DEFS=4096};Def defs[DEFS];unsigned ndef=0;uint32_t scan_epoch=0;bool scan_over=false;
struct Ctx{unsigned depth;uint64_t *budget;};
struct Stats{uint64_t frames,nested,notail,bad,dup,why[NBO_W_KINDS],valid,segs,merged,glue,cont,scanned,records,pure,keyed,scan[NBO_S_KINDS],r_addr,r_key,r_unsafe,r_mode,
    would,would_merged,ran,ran_merged,discont,started,put,unit,unit_prebar,broken,timed,ns;}s{};
unsigned told=0;uint64_t next_ns=0;
void read_mode(){const char *e=getenv("DM_NB_ORDER");mode=e && *e?atoi(e):0;if(mode<1 || mode>3)mode=0;
    if(mode)xbg_logf("[nb] DM_NB_ORDER=%d: at every OP_X_NB_FRAME with an order tail the part is %s (NB-17a%s)",mode,
        mode==1?"executed segment by segment in the tail's order when that is the stream order":
        mode==2?"executed as recorded, the tail's order only verified":"executed in the tail's order when it verifies",mode==3?" / NB-17b":"");}
// A record whose execute() case only writes the backend state (`state`, tex_gen / buf_gen, missing_program, the OP_X_NB_STATE DEF table,
// the nbchk span): a segment made of these alone has no effect but the state, which every discontinuous start takes from a key.
bool pure_op(uint32_t op){
    switch(op){
    case OP_BEGIN:case OP_SET_RT:case OP_VIEWPORTS:case OP_SCISSOR:case OP_DEPTH_RANGE:case OP_BIND_PROGRAM:case OP_BIND_STATE:case OP_BIND_VATTR:
    case OP_BIND_VSTREAM:case OP_BIND_VBUF:case OP_BIND_UBO:case OP_BIND_SSBO:case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:case OP_STENCIL:
    case OP_POLY_OFFSET:case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL:case OP_COPY_STRIDE:case OP_COPY_IMAGE_STRIDE:case OP_BLEND_COLOR:case OP_COUNT_:
    case OP_COUNT_+5:case OP_X_PRIM_RESTART:case OP_X_DEPTH_CLAMP:case OP_X_RAST_DISCARD:case OP_X_ALPHA_REF:
    case OP_X_NB_PASS:case OP_X_NB_PASS_END:case OP_X_NB_KEY:case OP_X_NB_STATE:case OP_X_ENGINE_MARK:case OP_X_FENCE_SYNC:return true;
    default:return false;}}
// Bytes of command memory readable at p, 0 = none (command_available is fatal there; an address from the tail is only data).
size_t cb_avail(void *,const uint8_t *p){const uintptr_t a=uintptr_t(p);
    if(a-cmd_range_lo<cmd_range_bytes)return cmd_range_bytes-(a-cmd_range_lo);   // the range execute() is in
    auto in=[a](const std::map<uintptr_t,size_t> &m)->size_t{auto it=m.upper_bound(a);if(it==m.begin())return 0;--it;return a-it->first<it->second?it->second-(a-it->first):0;};
    if(const size_t r=in(pools))return r;return in(host_ranges);}
// Scan: state-only or not; the DEF blocks of OP_X_NB_STATE are noted (registered before an order that is not the stream order runs). A part
// whose state blocks span two dictionary epochs is refused: one id would name two contents.
int cb_rec(void *,uint32_t op,const uint8_t *p,size_t n){
    if(op!=OP_X_NB_STATE)return pure_op(op)?1:0;
    nbs_hdr h;nbs_view v[NBS_KINDS];const int nb=nbs_parse(p,n,&h,v);if(nb<=0)return -1;
    if(!scan_epoch)scan_epoch=h.epoch;else if(scan_epoch!=h.epoch)return -1;
    for(int i=0;i<nb;i++)if(v[i].b.how==NBS_B_DEF){if(ndef<DEFS)defs[ndef++]={v[i].b.kind,v[i].b.id,h.epoch,v[i].content};else scan_over=true;}
    return 1;}
int cb_key(void *,const uint8_t *p,size_t n,uint32_t seq,int node){return nbkey::usable(p,n,seq,node)?1:0;}
const uint8_t *cb_exec(void *c,int e,const uint8_t *from,const uint8_t *stp){Ctx &x=*static_cast<Ctx*>(c);
    cur_e=e;stop=stp;end_pc=nullptr;if(from!=stp)execute(from,x.depth,*x.budget);stop=nullptr;return end_pc;}
int cb_start(void *c,int,const nbo_step *st){Ctx &x=*static_cast<Ctx*>(c);
    if(!st->key || !nbkey::start(st->key,st->keyn,x.depth,*x.budget))return 0;
    nbpipe::reset();++s.started;return 1;}   // NB-13b: the state was written directly
void cb_keep(void *){memcpy(&kept,&state,sizeof(State));kept_missing=missing_program;}
void cb_put(void *){const auto re=state.restart_eff;const auto cs=state.count_samples;const auto sp=state.samples_passed;   // as nbkey: not part of a key
    memcpy(&state,&kept,sizeof(State));state.restart_eff=re;state.count_samples=cs;state.samples_passed=sp;missing_program=kept_missing;
    for(int st=0;st<6;st++){++tex_gen[st];++buf_gen[st];}nbdesc::P.valid=false;nbpipe::reset();++s.put;}
// nbrp (xbg_nb.inc): the k-th pass after h in h's unit (k >= 1), while the unit's head h is being executed by a schedule.
bool unit_member(const p_nb_pass &h,int k,p_nb_pass &out){
    if(!in_run || cur_e<0 || k<1 || h.seq!=cur_seq || sc.st[cur_e].node!=uint16_t(h.node))return false;
    for(int i=1;i<=k;i++)if(cur_e+i>=sc.n || !(sc.st[cur_e+i].flags&NBO_E_CONT))return false;
    memcpy(&out,sc.st[cur_e+k].from+8,sizeof out);return out.seq==h.seq && out.patched;}
bool unit_last(const p_nb_pass &h,p_nb_pass &out){int k=0;p_nb_pass t;while(unit_member(h,k+1,t)){out=t;++k;}if(k)++s.unit;return k>0;}
void note_unit_prebar(){++s.unit_prebar;}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const double v=s.valid?double(s.valid):1.,q=s.scanned?double(s.scanned):1.;
    xbg_logf("[nb] order (backend, DM_NB_ORDER=%d, last 10 s): %llu OP_X_NB_FRAME, order tail valid %llu (none %llu, malformed %llu, part translated again %llu, "
        "FRAME inside a run %llu; not valid: no plan %llu, more passes than reserved %llu, a record not written %llu, passes in another recording %llu, not filled %llu); "
        "%.1f segments a part, tails not in the stream order %llu (planner order not usable by segments %llu), passes that continue the one before %llu; "
        "scanned %llu parts (%.0f records, %.1f state-only segments, %.1f complete keys a part; failed: address %llu, PASS out of place %llu, handle ended early %llu, "
        "record refused %llu, guard %llu); PASS address check of a stream-order tail failed %llu; %s %llu parts (not in the stream order %llu); refused: start without a "
        "complete key %llu, inversion without a state-only side %llu, other order at =1 %llu; starts from a key %llu of %llu not continuous, end state put back %llu, "
        "units with the last header's stores %llu, with members' reads in the barrier %llu; stream != tail while running %llu; %.1f us a part before the run (1 in 16 timed)",
        mode,(unsigned long long)s.frames,(unsigned long long)s.valid,(unsigned long long)s.notail,(unsigned long long)s.bad,(unsigned long long)s.dup,(unsigned long long)s.nested,
        (unsigned long long)s.why[NBO_W_NOPLAN],(unsigned long long)s.why[NBO_W_OVER],(unsigned long long)s.why[NBO_W_LOST],(unsigned long long)s.why[NBO_W_SPLIT],
        (unsigned long long)(s.why[NBO_W_UNPATCHED]+s.why[NBO_W_OK]),s.segs/v,(unsigned long long)s.merged,(unsigned long long)s.glue,(unsigned long long)s.cont,
        (unsigned long long)s.scanned,s.records/q,s.pure/q,s.keyed/q,(unsigned long long)s.scan[NBO_S_ADDR],(unsigned long long)s.scan[NBO_S_ORDER],
        (unsigned long long)s.scan[NBO_S_MISSING],(unsigned long long)s.scan[NBO_S_REFUSED],(unsigned long long)s.scan[NBO_S_GUARD],(unsigned long long)s.r_addr,
        mode==2?"would run by the tail":"run by the tail",(unsigned long long)(mode==2?s.would:s.ran),(unsigned long long)(mode==2?s.would_merged:s.ran_merged),
        (unsigned long long)s.r_key,(unsigned long long)s.r_unsafe,(unsigned long long)s.r_mode,(unsigned long long)s.started,(unsigned long long)s.discont,
        (unsigned long long)s.put,(unsigned long long)s.unit,(unsigned long long)s.unit_prebar,(unsigned long long)s.broken,s.timed?double(s.ns)/1e3/double(s.timed):0.);
    s=Stats{};}
// From case OP_X_NB_FRAME of execute(), after nbchk::frame: p / n = the record's payload, after = the next record. true: the rest of the
// handle up to its OP_END was executed here by the tail's schedule; execute() goes on at `resume` (that OP_END).
bool frame(const uint8_t *p,size_t n,const uint8_t *after,unsigned depth,uint64_t &budget,const uint8_t *&resume){
    if(mode<0)read_mode();
    if(!mode)return false;
    tick();++s.frames;
    if(in_run){++s.nested;return false;}
    uint32_t seq=0;memcpy(&seq,p,4);
    nbo_hdr h{};const int ns=nbo_parse(p,n,seq,&h,ent,NBO_MAX_SEG);
    if(ns<0){if(ns==NBO_P_NONE)++s.notail;else if(ns==NBO_P_BAD)++s.bad;else ++s.why[h.why<NBO_W_KINDS?h.why:0];return false;}
    if(!nbchk::part_ok){++s.dup;return false;}   // translated again (or not patched): as recorded
    const bool timed=!(s.valid&15);const uint64_t t0=timed?timing_clock():0;
    ++s.valid;s.segs+=uint64_t(ns);s.cont+=h.ncont;if(h.flags&NBO_F_MERGED)++s.merged;if(h.flags&NBO_F_GLUE)++s.glue;
    nbo_schedule(ent,ns,&sc);
    const bool ident=sc.identity!=0;bool ok=true;
    if(!ident || mode==2){
        static const nbo_scan_io io{nullptr,OP_END,OP_JUMP,OP_X_NB_PASS,OP_X_NB_KEY,cb_avail,cb_rec,cb_key};
        ndef=0;scan_epoch=0;scan_over=false;
        const int w=nbo_scan(&sc,after,seq,&io,&so);
        if(w!=NBO_S_OK || scan_over){++s.scan[w!=NBO_S_OK?w:NBO_S_REFUSED];ok=false;
            if(told<16){++told;xbg_logf("[nb] order: part seq=%u (%d segments%s): scan failed - %s; executed as recorded",seq,ns,ident?"":", not the stream order",
                w!=NBO_S_OK?nbo_scan_name(w):"more OP_X_NB_STATE DEF blocks than the table holds");}}
        else{++s.scanned;s.records+=so.records;s.pure+=uint64_t(so.npure);s.keyed+=uint64_t(so.nkeyed);
            if(!ident){const int nk=nbo_unkeyed(&sc),nu=nk?0:nbo_unsafe(&sc,so.pure,(h.flags&NBO_F_TRACKED)!=0);
                if(nk){++s.r_key;ok=false;}else if(nu){++s.r_unsafe;ok=false;}
                if(!ok && told<16){++told;
                    if(nk)xbg_logf("[nb] order: part seq=%u: %d of %d starts that are not continuous have no complete key (DM_NB_RELOC?); executed as recorded",seq,nk,sc.ndiscont);
                    else xbg_logf("[nb] order: part seq=%u: %d inversions without a state-only side and the planner did not track the hazards (first record that is not state: "
                        "op %u in the segment of node %d); executed as recorded",seq,nu,so.bad_op,so.bad_seg>=0?int(sc.st[so.bad_seg].node):-1);}}}}
    else for(int e=0;e<ns && ok;e++){   // stream order, 1 / 3: every address is a PASS record of this part (nothing else is assumed)
        const uint8_t *a=sc.st[e].from;op_hdr rh{};uint32_t rs=0;uint16_t rn=0;
        if(cb_avail(nullptr,a)<8+sizeof(p_nb_pass)){ok=false;break;}
        memcpy(&rh,a,8);memcpy(&rs,a+8+NBO_PASS_SEQ_OFF,4);memcpy(&rn,a+8+NBO_PASS_NODE_OFF,2);
        if(rh.op!=OP_X_NB_PASS || rh.size<8+sizeof(p_nb_pass) || rs!=seq || rn!=sc.st[e].node)ok=false;}
    if(!ident || mode==2){}else if(!ok){++s.r_addr;if(told<16){++told;xbg_logf("[nb] order: part seq=%u: an address of the tail is not its OP_X_NB_PASS record; executed as recorded",seq);}}
    if(timed){++s.timed;s.ns+=timing_clock()-t0;}
    if(mode==2){if(ok){++s.would;if(!ident)++s.would_merged;s.discont+=uint64_t(sc.ndiscont);}return false;}
    if(!ok)return false;
    if(!ident){
        if(mode!=3){++s.r_mode;return false;}
        for(unsigned i=0;i<ndef;i++)nbstate::defs[defs[i].kind][defs[i].id]={defs[i].epoch,defs[i].p};   // a REF may run before its DEF now
        s.discont+=uint64_t(sc.ndiscont);}
    Ctx cx{depth,&budget};const nbo_ops ops{&cx,cb_exec,cb_start,cb_keep,cb_put};
    in_run=true;run_depth=depth;cur_seq=seq;
    int res=NBO_R_OK,done=0;
    resume=nbo_run(&sc,after,&ops,&res,&done);
    in_run=false;stop=nullptr;cur_e=-1;
    if(res!=NBO_R_OK){++s.broken;
        xbg_logf("[nb] order: part seq=%u: the stream did not match the tail while running (%d, %d of %d segments run, %s order)",seq,res,done,ns,ident?"stream":"planner");
        need(ident && res==NBO_R_EARLY_END,"NB order: the command stream does not match a verified order tail");}   // stream order + early end = as recorded so far
    need(resume!=nullptr,"NB order: no end of the handle after the run");
    ++s.ran;if(!ident)++s.ran_merged;
    return true;}
}
