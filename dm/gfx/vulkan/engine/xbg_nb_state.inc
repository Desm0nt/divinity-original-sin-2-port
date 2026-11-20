// [DOS2] NB-13a: OP_X_NB_STATE in the backend (docs/dos2/NATIVE_BACKEND_PLAN.md NB-13a; layout dm/gfx/nvn/nvn_nb_state.h). Included by
// xbg_game.cpp right before execute(). Faithful unpack: every block's content is the NVN state records the entry points would have written
// (+ OP_END), so it is executed in place by the same execute() cases (nested, like OP_X_NATIVE_DRAW): same checks, same state, same order.
// DEF block: content follows; its address is kept per (kind, id) with the record's epoch. REF block: the content of that DEF, executed from
// where the DEF lies (earlier in the same command handle: the front end resets its dictionaries per recording, so the memory is live).
// A REF without its DEF in this epoch is counted and logged (first 16), not executed (cannot happen with an intact stream).
// The ids are not used for anything else yet: NB-13b keys the pipeline memo on them.
void execute(const uint8_t *pc,unsigned depth,uint64_t &budget);
namespace nbstate{
struct Def{uint32_t epoch=0;const uint8_t *p=nullptr;uint32_t bytes=0;};   // bytes: the content's size (NR-13 census)
Def defs[NBS_KINDS][NBS_MAX_IDS];
uint64_t records=0,bytes=0,blocks[3]={0,0,0},miss=0,next_ns=0;unsigned told=0;
void exec(const uint8_t *p,size_t n,unsigned depth,uint64_t &budget){NRCPU_SCOPE(NbState);   // [DOS2] NR-CPU
    nbs_hdr h;nbs_view v[NBS_KINDS];
    const int nb=nbs_parse(p,n,&h,v);need(nb>0,"Invalid OP_X_NB_STATE record");
    for(int i=0;i<nb;i++){const nbs_view &b=v[i];const uint8_t *c=b.content;
        uint32_t cb=b.b.bytes;
        if(b.b.how==NBS_B_REF){const Def &d=defs[b.b.kind][b.b.id];
            if(d.epoch!=h.epoch || !d.p){++miss;if(told<16){++told;xbg_logf("[nb] state: REF kind %u id %u of epoch %u has no DEF in this epoch (kept epoch %u) - block skipped",unsigned(b.b.kind),unsigned(b.b.id),h.epoch,d.epoch);}continue;}
            c=d.p;cb=d.bytes;}
        else if(b.b.how==NBS_B_DEF)defs[b.b.kind][b.b.id]={h.epoch,c,b.b.bytes};
        ++blocks[b.b.how];if(__builtin_expect(nrlean::mode>0,0))nrlean::state_block(cb);   // [DOS2] NR-13 DM_NR_LEAN: what the capture makes the backend replay
        execute(c,depth+1,budget);}
    ++records;bytes+=n+8;
    const uint64_t now=timing_clock();
    if(now>=next_ns){if(next_ns)xbg_logf("[nb] state (backend, last 10 s): %llu OP_X_NB_STATE records (%.1f KB), blocks executed: %llu DEF %llu REF %llu without id; REF without DEF %llu",
        (unsigned long long)records,bytes/1024.,(unsigned long long)blocks[NBS_B_DEF],(unsigned long long)blocks[NBS_B_REF],(unsigned long long)blocks[NBS_B_NOID],(unsigned long long)miss);
        records=bytes=miss=0;blocks[0]=blocks[1]=blocks[2]=0;next_ns=now+10000000000ull;}
}
}
