// [DOS2] NR-09R (DM_NR_DIRECT, default 0; docs/dos2/nr/NR-09R.md): the backend's side of the direct engine draw. Included by
// xbg_game.cpp before xbg_nb.inc (nbdraw::apply calls pre() before the binds of an OP_X_NR_DRAW land, nbdraw::tick calls tick()).
// The record is the OP_X_NB_DRAW + NR-09 tail it always was; the direct recorder (dm/gfx/nvn/nvn_nr_direct.inc) only leaves out texture
// binds whose slot already holds that handle in state.tex and says so in the tail's spare word (dm/gfx/nr/nr_direct.h NRD_X_*):
//   DM_NR_DIRECT=1  counted: records written by the direct recorder, texture binds not sent;
//   DM_NR_DIRECT=2  every bind is sent and the ones =1 would leave out are flagged: each is compared with the table as it is when the
//                   bind lands (state.tex after the earlier binds of the same record) - red lines, first 16, all counted.
// NR-UBD (DM_NR_UBD=2, docs/dos2/nr/NR-UBD.md): the same flags mark uniform-buffer binds of a served direct draw =1 would not send; each
// is compared with state.ub as it lands (after the earlier binds of the record) - red lines '【红】[nr] ubd check', first 16, all counted.
// With the switch unset nothing here runs (one load and a not-taken branch a record).
#include "../../nr/nr_direct.h"
namespace nrdirect{
int mode=-1;   // DM_NR_DIRECT as the backend reads it (the front end arms the recorder; the records carry the flags)
int ubd=0;     // DM_NR_UBD (NR-UBD): 2 = the line of the uniform-buffer checks
struct Stats{uint64_t recs=0,direct=0,filt=0,chk_recs=0,chk=0,bad=0,uchk=0,ubad=0;} s;
uint64_t bad_all=0,chk_all=0,ubad_all=0,uchk_all=0,next_ns=0;unsigned told=0,utold=0;
void init(){const char *e=getenv("DM_NR_DIRECT");const int v=e && *e?atoi(e):0;mode=v==1 || v==2?v:0;
    if(mode)xbg_logf("[nr] DM_NR_DIRECT=%d (NR-09R, backend): %s",mode,mode==1?"OP_X_NR_DRAW records of the direct recorder carry no texture bind whose slot already holds that handle (counted)"
        :"every texture bind is sent; the ones =1 would not send are flagged and compared with the binding table as they land");
    const char *u=getenv("DM_NR_UBD");ubd=u && *u?atoi(u):0;
    if(mode && ubd==2)xbg_logf("[nr] DM_NR_UBD=2 (NR-UBD, backend): uniform-buffer binds of direct draws DM_NR_UBD=1 would not send are flagged and compared with state.ub as they land");}
// Before nbd_apply writes the binds of the record (p, n) with header h and binds b into the tables.
void pre(const uint8_t *p,size_t n,const nbd_hdr &h,const uint8_t *b){
    nrd_tail t;if(!nrd_tail_get(p,n,&h,&t))return;
    ++s.recs;const uint32_t x=t._pad;if(!x)return;
    if(x&NRD_X_DIRECT)++s.direct;
    s.filt+=nrd_x_filtered(x);
    const uint32_t would=nrd_x_would(x);if(!would)return;
    ++s.chk_recs;
    for(unsigned i=0;i<h.nbind && i<NRD_X_BITS;i++)if(would>>i&1){
        nbd_bind e;memcpy(&e,b+sizeof e*i,sizeof e);
        if(e.kind==NBD_B_UBO){++s.uchk;++uchk_all;   // NR-UBD=2: the uniform-buffer table as this bind lands
            bool uok=e.stage<6 && e.slot<16;uint64_t ha=0,hn=0;
            if(uok){ha=state.ub[e.stage][e.slot].address;hn=state.ub[e.stage][e.slot].size;
                for(unsigned j=0;j<i;j++){nbd_bind q;memcpy(&q,b+sizeof q*j,sizeof q);if(q.kind==NBD_B_UBO && q.stage==e.stage && q.slot==e.slot){ha=q.a;hn=q.b;}}   // an earlier bind of this record
                uok=ha==e.a && hn==e.b;}
            if(!uok){++s.ubad;++ubad_all;
                if(utold<16){++utold;xbg_logf("【红】[nr] ubd check: uniform bind %u of %u of an OP_X_NR_DRAW (stage %u slot %u range %#llx+%llu) is one DM_NR_UBD=1 would not send, "
                    "but the backend's table holds %#llx+%llu there",i,unsigned(h.nbind),unsigned(e.stage),unsigned(e.slot),(unsigned long long)e.a,(unsigned long long)e.b,
                    (unsigned long long)ha,(unsigned long long)hn);}}
            continue;}
        ++s.chk;++chk_all;
        bool ok=e.kind==NBD_B_TEX && e.stage<6 && e.slot<128;uint64_t have=0;
        if(ok){have=state.tex[e.stage][e.slot];
            for(unsigned j=0;j<i;j++){nbd_bind q;memcpy(&q,b+sizeof q*j,sizeof q);if(q.kind==NBD_B_TEX && q.stage==e.stage && q.slot==e.slot)have=q.a;}   // an earlier bind of this record
            ok=have==e.a;}
        if(!ok){++s.bad;++bad_all;
            if(told<16){++told;xbg_logf("【红】[nr] direct check: texture bind %u of %u of an OP_X_NR_DRAW (kind %u stage %u slot %u handle %#llx) is one DM_NR_DIRECT=1 would not send, "
                "but the backend's table holds %#llx there",i,unsigned(h.nbind),unsigned(e.kind),unsigned(e.stage),unsigned(e.slot),(unsigned long long)e.a,(unsigned long long)have);}}}}
void tick(){if(mode<=0 || !s.recs)return;const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    if(ubd==2)xbg_logf("[nr] ubd check (backend, DM_NR_UBD=2, last 10 s): %llu uniform binds =1 would not send compared with state.ub as they land, bad %llu; totals: compared %llu, bad %llu",
        (unsigned long long)s.uchk,(unsigned long long)s.ubad,(unsigned long long)uchk_all,(unsigned long long)ubad_all);
    if(mode==1)xbg_logf("[nr] direct (backend, DM_NR_DIRECT=1, last 10 s): %llu OP_X_NR_DRAW records, %llu written by the direct recorder (%.1f%%), texture binds not sent %llu (%.2f a record)",
        (unsigned long long)s.recs,(unsigned long long)s.direct,100.*double(s.direct)/double(s.recs),(unsigned long long)s.filt,double(s.filt)/double(s.recs));
    else xbg_logf("[nr] direct check (backend, DM_NR_DIRECT=2, last 10 s): %llu OP_X_NR_DRAW records, %llu with flagged binds; %llu texture binds =1 would not send compared with the "
        "binding table as they land, bad %llu; totals: compared %llu, bad %llu",(unsigned long long)s.recs,(unsigned long long)s.chk_recs,(unsigned long long)s.chk,(unsigned long long)s.bad,
        (unsigned long long)chk_all,(unsigned long long)bad_all);
    s=Stats{};}
}
