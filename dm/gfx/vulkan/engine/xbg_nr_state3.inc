// [DOS2] NR-09S3: OP_X_NR_STATE in the backend (DM_NR_STATE=4 / 5 on the front end; docs/dos2/nr/NR-09S.md §10; layout dm/gfx/nr/nr_state3.h).
// Included by xbg_game.cpp right after xbg_nb_state.inc (before execute()). One record = the state of one opcode-21 packet: applied straight
// into State, every field as execute()'s case for the record the front end's bind body would have written sets it (program binds through a
// copy of the OP_BIND_PROGRAM case: shaders, missing_program, NB-18b-4's program table). NB-13b: the groups the record wrote are re-read at
// the next draw (as nbpipe::mark does for the state records). No record of this op exists unless DM_NR_STATE is 4 or 5: nothing here runs.
// CHECK (=5): the record is applied to a copy of the compared fields; the old records follow and write the real state; CHECK_END compares
// every field the record can write (red lines 【红】[nr] state3 check, first 16; totals every 10 s).
#include "../../nr/nr_state3.h"
namespace nrstate3{
static_assert(sizeof(blend_st)==NR3_SZ_BLEND && sizeof(color_st)==NR3_SZ_COLOR && sizeof(ds_st)==NR3_SZ_DS && sizeof(poly_st)==NR3_SZ_POLY && sizeof(vs_st)==NR3_SZ_VSTREAM,"NR-09S3 object sizes");
static_assert(NR3_K_BLEND==NBP_G_BLEND && NR3_K_COLOR==NBP_G_COLOR && NR3_K_POLY==NBP_G_POLY && NR3_K_DS==NBP_G_DS && NR3_K_VFMT==NBP_G_VFMT,"NR-09S3 kinds = NB-13b groups 0..4");
struct Snap{Shader *shader[6];ds_st ds;uint32_t stencil_compare[2],stencil_write[2],stencil_ref[2];poly_st poly;blend_st blend[8];color_st color;uint32_t attrib[16];vs_st stream[16];
    NVNbufferRange vb[16];float alpha_ref;bool missing;};
// execute()'s OP_BIND_PROGRAM case (copied; tests/source_structure_test.py keeps the two equal) on a state S with its missing flag.
template<class T> inline void bind(T &S,bool &missing,const prog_t *prog,int stages,bool publish){
    need(!prog || prog->nstage<=6,"Invalid bound program");for(unsigned st=0;st<6;st++)if(stages&(1u<<nvn_stage[st]))S.shader[st]=nullptr;if(!prog)return;const std::array<Shader*,6> &loaded=program_shaders(prog);missing=false;if(publish && nbpost::mode)nbpost_publish(prog,loaded);for(unsigned i=0;i<prog->nstage;i++){Shader *s=loaded[i];if(!s){missing=true;continue;}if(stages&(1u<<nvn_stage[s->stage]))S.shader[s->stage]=s;}}
#define NR3_BIND_STATE(S,pr,st) bind(*(S),missing_program,reinterpret_cast<const prog_t*>(uintptr_t(pr)),(st),true)
#define NR3_BIND_SNAP(S,pr,st) bind(*(S),(S)->missing,reinterpret_cast<const prog_t*>(uintptr_t(pr)),(st),false)
#define NR3_VATTR(a,st,n) xbg_vertex_attributes_replace((a),(st),(n))
NR3_DEFINE_APPLY_RAW(apply_state,State,NR3_BIND_STATE,NR3_VATTR)
NR3_DEFINE_APPLY_RAW(apply_snap,Snap,NR3_BIND_SNAP,NR3_VATTR)
NR3_DEFINE_SNAP(snap_of,Snap,State)
NR3_DEFINE_CMP(cmp,Snap)
uint64_t recs=0,bytes=0,timed=0,ns=0,checks=0,bad=0,unpaired=0,badf[NR3_C_N]={},next_ns=0,n_seen=0;unsigned told=0;
thread_local Snap want;thread_local uint32_t want_seq=0;thread_local bool want_on=false;
void exec(const uint8_t *p,size_t n){NRCPU_SCOPE(NbState);   // [DOS2] NR-CPU: the state share of the translator, as OP_X_NB_STATE
    const bool tm=!(n_seen++&63);const uint64_t t0=tm?timing_clock():0;
    need(n>=sizeof(nr3_hdr),"Truncated OP_X_NR_STATE record");const uint8_t flags=p[1];
    if(!flags){need(apply_state(&state,p,n)==0,"Invalid OP_X_NR_STATE record");
        if(nbpipe::mode)nbpipe::G->dirty|=nr3_kinds_raw(p)&((1u<<(NR3_K_VFMT+1))-1);
        if(__builtin_expect(nrn::on,0))nrn::state_rec(p,n);}   // [DOS2] NR-NATIVE step B part 2: which fields the engine writes through state records   // [DOS2] NB-13b: as nbpipe::mark for the bind records
    else if(flags==NR3_F_CHECK){snap_of(&want,&state,missing_program);need(apply_snap(&want,p,n)==0,"Invalid OP_X_NR_STATE record");{uint16_t q;memcpy(&q,p+14,2);want_seq=q;}want_on=true;}
    else{nr3_rec v;need(flags==NR3_F_CHECK_END && nr3_parse(p,n,&v)!=0,"Invalid OP_X_NR_STATE record");
        if(!want_on || want_seq!=v.seq)++unpaired;
        else{Snap cur;snap_of(&cur,&state,missing_program);const int f=cmp(&cur,&want);++checks;
            if(f>=0){++bad;++badf[f];if(told<16){++told;xbg_logf("【红】[nr] state3 check: opcode-21 packet %u: after the old records the state's %s differs from what the OP_X_NR_STATE record gives",
                unsigned(v.seq),nr3_field_name(f));}}}
        want_on=false;}
    ++recs;bytes+=n+8;
    const uint64_t now=timing_clock();if(tm){++timed;ns+=now-t0;}
    if(now>=next_ns){if(next_ns){char bf[256];int k=0;bf[0]=0;for(int i=0;i<NR3_C_N;i++)if(badf[i])k+=snprintf(bf+k,sizeof bf-size_t(k),"%s%s %llu",k?", ":"",nr3_field_name(i),(unsigned long long)badf[i]);
            xbg_logf("[nr] state3 (backend, last 10 s): %llu OP_X_NR_STATE records (%.1f KB), ns a record (1 in 64 timed): %.0f (%llu); check (DM_NR_STATE=5): %llu packets compared, "
                "bad %llu%s%s%s, unpaired %llu",(unsigned long long)recs,bytes/1024.,timed?double(ns)/double(timed):0.,(unsigned long long)timed,(unsigned long long)checks,
                (unsigned long long)bad,k?" (":"",bf,k?")":"",(unsigned long long)unpaired);}
        recs=bytes=timed=ns=checks=bad=unpaired=0;memset(badf,0,sizeof badf);next_ns=now+10000000000ull;}
}
}
