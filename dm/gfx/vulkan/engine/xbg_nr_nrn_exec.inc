// [DOS2] NR-NATIVE step B (docs/dos2/nr/NR-NATIVE.md section 8): the ROW of a card and the draw executed from it. Second half of
// namespace nrn (the first, xbg_nr_nrn.inc, is included ahead of the lane); included by xbg_game.cpp right after xbg_nr_z3.inc - it
// takes nrz3::answered, the answered half of an engine draw. The row, the publication and the counters are in dm/gfx/nr/nr_nrn.h
// (pure C, host tests tests/nr_nrn_test.c and tests/nr_nrn_pub_test.cpp).
//
// The card (written by the game thread into the draw record) names the engine's objects of the draw. When the path without a row
// records a draw that carries a card, what that path resolved - NR-08's plan and entry, NR-09's pipeline entry - becomes the card's
// row (learn, from the lane's tail), stamped with the three generations those ids live under, and the card is published to the game
// thread (nrn_pub: one word a slot, no lock). The next draw with the same card:
//   DM_NRN_<PASS>=1   asks the row before anything is looked up (draw, the hook of draw() after its prologue). The generations must
//                     stand; the words the card does not hold are compared live (the render pass definition of the open pass, the
//                     backend's five shader objects, NR-08's six fixed words); the gates of the short path are the ones nrz3 /
//                     nrz8t ask. Then nrz3::answered runs with the row's answer: the entry as NR-08's serve() checks it, the
//                     blocks, the set, the image walk, the pass marks and the lane's own tail - every statement with a side
//                     effect is the path's own code in the path's order. What is skipped: descriptors()' key and table, the
//                     variant, the state-group ids and NR-09's table. A draw that is not answered takes the path from its first
//                     line (nothing was changed) and teaches the row again.
//   DM_NRN_<PASS>=2   the row is asked the same way, nothing is taken from it; when the path without it has recorded the draw the
//                     two resolutions are compared word for word (nrn_ans_same): `bad` = the card's row would have drawn another
//                     draw - a red line (first 16) and the row is replaced.
// Never asked: a draw the prologue changed (DM_DOS2_PREPASS's override, a draw inside a fused / elided post chain, NR-LIGHTFUSE /
// NR-ONCHIP twins and redirects: the card describes what the engine asked for, not what is drawn - phone series N_v1 showed exactly
// that as "pipeline differs" at the frame LIGHTFUSE switched) and the special forms (QUADS, the UI shift, a geometry stage).
// Lines every 10 s: "[nr] native <PASS> rows ...".
namespace nrn{
struct Pred{bool valid=false;nrn_ans a{};} P;bool full_told=false;int lane_prim=0;
nrz3::Stats zs;uint64_t ecalls=0,stamp_seen[NRN_STAMPS]{};
inline void stamps(uint64_t st[NRN_STAMPS]){st[0]=nrdesc::tab_gen;st[1]=nrdraw::tab_gen();st[2]=nbpipe::epoch;}
// The generations moved: no row answers any more, so no card is published.
// (part 2: a card is published for its IMAGE, which no generation retires - the answer of a row is only asked for when the record is executed)
inline void sync(const uint64_t st[NRN_STAMPS]){memcpy(stamp_seen,st,sizeof stamp_seen);}
inline int out(int w){++b[cur.pass].why[w];if(cur.rsv&NRN_T_READY)++b[cur.pass].ready_not;return 0;}
// 0: not answered (nothing was touched); 1 / 2 / 3: as nrz3::draw. form: a special draw form (never answered).
int draw(Commands &c,Descriptors *&dp,Pass &p,int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances,uint32_t first_instance,bool pp,int draw_fast,bool form){
    nrn_bstat &x=b[cur.pass];++x.asked;P.valid=false;answering=false;
    // the lane word of this draw (series CN): what NR-09's table is asked with besides the state groups - learn() keeps it in the row
    uint32_t active_colors=0;for(int i=0;i<state.rt.n;i++)if(state.rt.color[i])active_colors|=1u<<i;
    lane_prim=prim;
    lane_cur=nrdraw::cur?nrn_lane_word(nrdraw::cur->key,unsigned(prim),last_pass.valid?unsigned(last_pass.pass.samples):0u,unsigned(state.rt.n),active_colors,
        state.rt.depth?unsigned(state.rt.depth->d.format):0u,nrdraw::restart_eff_of(prim,indexed)):0;lane_ok=lane_cur!=0;
    if(pgd)return out(NRN_BW_GUARD);
    if(form || pp)return out(NRN_BW_FORM);
    uint64_t st[NRN_STAMPS];stamps(st);sync(st);
    if(nrdraw::epoch!=nbpipe::epoch)return out(NRN_BW_STALE);   // the lane clears its table at its next draw
    int why=0;nrn_row *r=cur_row;   // the row record() / pre() found for this record: one probe of the table a record
    if(r && r->card==cur.card){if(memcmp(r->stamp,st,sizeof r->stamp)){why=NRN_F_STALE;r=nullptr;}else if(r->pass!=cur.pass){why=NRN_F_PASS;r=nullptr;}else why=NRN_F_HIT;}
    else r=nrn_rows_find(&R,cur.card,cur.pass,st,&why);
    if(!r)return out(why==NRN_F_NONE?NRN_BW_NO_ROW:why==NRN_F_STALE?NRN_BW_STALE:NRN_BW_PASS);
    const nrd_tail &t=*nrdraw::cur;
    if(nrz3::gate(t,pp,draw_fast,false))return out(NRN_BW_GATE);
    if(nrps::mode==1 && state.shader[0]->psk==1)return out(NRN_BW_GATE);   // PRESKIN's variant is the lane's to pick
    if(!last_pass.valid)return out(NRN_BW_PASSDEF);
    uint64_t sh[5],f[6];for(unsigned i=0;i<5;i++)sh[i]=uint64_t(uintptr_t(state.shader[i]));nrdesc::fixed_words(f);
    if(const unsigned l=nrn_ans_live(&r->a,uint64_t(uintptr_t(last_pass.pass.definition)),sh,f))return out(l&NRN_L_PASSDEF?NRN_BW_PASSDEF:l&NRN_L_SHADERS?NRN_BW_SHADERS:NRN_BW_FIXED);
    if(r->a.lane!=lane_cur)return out(NRN_BW_LANE);   // another key id of the front end or another variant word: NR-09 would be asked for another entry
    if(mode[cur.pass]==2){P.valid=true;P.a=r->a;++x.predicted;if(cur.rsv&NRN_T_READY)++x.ready_ans;return 0;}   // verify: the path without the row draws; learn() compares
    nrdraw::Tm tm;tm.timed=!(++ecalls&63);tm.t0=tm.timed?timing_clock():0;tm.sp=false;
    nrz_val v{};v.plan=r->a.plan;v.pent=r->a.pent;v.did=r->a.did;v.frag=r->a.frag;
    nrz3::Ans a;a.v=&v;a.pl=reinterpret_cast<nrdesc::Plan*>(uintptr_t(r->a.plan));a.frag=r->a.frag!=0;a.fhit=true;a.psk=false;a.pp=false;a.z8=true;a.f=f;a.varw=nullptr;
    a.re=nrdraw::restart_eff_of(prim,indexed);a.active_colors=active_colors;a.t=&t;
    nrz3::ps_called=false;answering=true;
    const int rr=nrz3::answered(zs,a,c,dp,p,prim,first,count,indexed,instances,first_instance,draw_fast,tm);
    answering=false;
    if(!rr)return out(NRN_BW_REFUSED);
    ++x.answered[rr];++r->answered;if(cur.rsv&NRN_T_READY)++x.ready_ans;
    return rr;}
// From the lane's tail (draw_end, own = the path without a row recorded this draw with its own Descriptors record and no variant of
// another module): what it resolved is the row of the card.
void learn(const Pass &p){
    nrn_bstat &x=b[cur.pass];
    const bool was=P.valid;P.valid=false;
    if(answering)return;
    if(!(nrdesc::z_out==NRZD_HIT || nrdesc::z_out==NRZD_BUILT)){if(was)++x.p_lost;return;}   // NR-08 did not serve this draw: there is no entry to name
    nrdesc::Plan *pl=nrdesc::plan_for();const nrdraw::Hand &H=nrdraw::H;
    if(!pl || pl->last<0 || !pl->layout || !H.have_ent || !H.ent || H.qpat || H.ui || !lane_ok){if(was)++x.p_lost;return;}
    nrn_ans now;memset(&now,0,sizeof now);
    now.plan=uint64_t(uintptr_t(pl));now.pent=uint64_t(uintptr_t(H.ent));now.did=pl->last;now.frag=uint8_t(desc_frag_only_shaders());now.passdef=uint64_t(uintptr_t(p.definition));
    {uint32_t ac=0;for(int i=0;i<state.rt.n;i++)if(state.rt.color[i])ac|=1u<<i;   // the lane word from what the lane itself used (its tail, its pass, its restart word)
     now.lane=nrn_lane_word(H.t.key,unsigned(lane_prim),unsigned(p.samples),unsigned(state.rt.n),ac,state.rt.depth?unsigned(state.rt.depth->d.format):0u,state.restart_eff);}
    for(unsigned i=0;i<5;i++)now.sh[i]=uint64_t(uintptr_t(state.shader[i]));nrdesc::fixed_words(now.f);
    if(was){
        if(nrn_ans_same(&P.a,&now))++x.p_same;
        else{++x.p_bad;if(told<16){++told;
            xbg_logf("【红】[nr] native: the row of a card is not what the path without it resolved (pass %s, card %016llx, frame %llu): plan %p / %p, NR-08 entry %d / %d, pipeline entry %p / %p, lane word %016llx / %016llx, "
                "fragment-only reads %d / %d, pass definition %p / %p, vs=%s ps=%s - the card does not describe this draw",nrn_pass_name(cur.pass),(unsigned long long)cur.card,
                (unsigned long long)stats.presents.load(),(void*)uintptr_t(P.a.plan),(void*)pl,P.a.did,now.did,(void*)uintptr_t(P.a.pent),(const void*)H.ent,(unsigned long long)P.a.lane,(unsigned long long)now.lane,int(P.a.frag),int(now.frag),
                (void*)uintptr_t(P.a.passdef),(void*)uintptr_t(now.passdef),state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-");}}}
    uint64_t st[NRN_STAMPS];stamps(st);sync(st);
    if(nrdraw::epoch!=nbpipe::epoch)return;   // the caches moved inside this draw
    int full=0;const int k=nrn_rows_put(&R,cur.card,cur.pass,&now,st,&full);
    if(full){if(!full_told){full_told=true;xbg_logf("【红】[nr] native: the rows of the cards are full (%u) - new cards get no row (drawn by the path without it; raise DM_NRN_MAX)",R.cap);}return;}
    if(k==0)++x.learned;else if(k==2)++x.relearned;else ++x.kept;}

// ---- part 2: the image of a card and the short record ----
// A short record (NRN_T_SHORT) carries no state record and no texture bind of its flush; its nrn_short_ext names the fields and the
// texture slots those records would have written. Before it is executed exactly those are put into State from the IMAGE of its card.
// The image is State as a LONG record with the same card left it (image(), from nbdraw::apply after the binds landed, before the
// draw): the card is the whole state cache of the engine, the image is what the records made of that cache. Why the values are right:
// a field a short draw leaves out is one the engine's own filter decided to send at this draw, so its value is the cache's - the same
// cache the long record had, where the field was either sent too or held back by the same filter because NVN already had it.
// Every later long record of the card compares what ITS records wrote (W: the fields of the state records executed since the previous
// draw record, the texture slots it binds) with the image: bad = the card does not decide a field the engine writes - red line, the
// card is never written short again. Rows are never dropped, so a short record in flight always finds its image. After the image is
// in State the record is executed like any other: the hook of draw() asks the row, the path without it draws from State.
struct Img{nrstate3::Snap s;uint64_t tex[NRN_TEX_STAGES][NRN_TEX_SLOTS];};
std::vector<std::unique_ptr<Img>> imgs;
struct Wrote{nrn_emask m{};uint32_t tex[NRN_TEX_STAGES]{};} W;   // what the engine's records wrote since the previous draw record
uint64_t st_recs=0;
inline Img *img_of(const nrn_row *r){const size_t i=size_t(r-R.e);return i<imgs.size()?imgs[i].get():nullptr;}
// From nrstate3::exec (a record applied to State).
void state_rec(const uint8_t *p,size_t n){++st_recs;nrn_emask_of_state(&W.m,p,n);}
// The image against State under a mask (put=false: compare only; put=true: write what differs). Returns the first differing part:
// -1 none, an NR3_C_* field, or NR3_C_N for a texture slot; tex_stages: the stages whose table differed.
inline int img_walk(Img &I,const nrn_emask &m,const uint32_t tex[NRN_TEX_STAGES],bool put,unsigned &tex_stages){
    int first=-1;tex_stages=0;
#define NRN_F(field,cond,dst,src) if((cond) && memcmp(&(dst),&(src),sizeof(dst))){if(first<0)first=(field);if(put)memcpy(&(dst),&(src),sizeof(dst));}
    if(m.stages){for(unsigned st=0;st<6;st++)NRN_F(NR3_C_SHADER,m.stages&(1u<<nvn_stage[st]),state.shader[st],I.s.shader[st])
        if(missing_program!=I.s.missing){if(first<0)first=NR3_C_MISSING;if(put)missing_program=I.s.missing;}}
    NRN_F(NR3_C_DS,m.have&NR3_H_DS,state.ds,I.s.ds)
    for(unsigned i=0;i<2;i++){NRN_F(NR3_C_SCMP,m.sten[0]>>i&1u,state.stencil_compare[i],I.s.stencil_compare[i])NRN_F(NR3_C_SWRITE,m.sten[1]>>i&1u,state.stencil_write[i],I.s.stencil_write[i])
        NRN_F(NR3_C_SREF,m.sten[2]>>i&1u,state.stencil_ref[i],I.s.stencil_ref[i])}
    NRN_F(NR3_C_POLY,m.have&NR3_H_POLY,state.poly,I.s.poly)
    for(unsigned t=0;t<8;t++)NRN_F(NR3_C_BLEND,m.blend>>t&1u,state.blend[t],I.s.blend[t])
    NRN_F(NR3_C_COLOR,m.have&NR3_H_COLOR,state.color,I.s.color)
    NRN_F(NR3_C_ATTRIB,m.have&NR3_H_VATTR,state.attrib,I.s.attrib)
    for(unsigned k=0;k<m.nstream && k<16;k++)NRN_F(NR3_C_STREAM,true,state.stream[k],I.s.stream[k])
    for(unsigned k=0;k<16;k++)NRN_F(NR3_C_VB,m.vb>>k&1u,state.vb[k],I.s.vb[k])
    NRN_F(NR3_C_ALPHA,m.have&NR3_H_ALPHA,state.alpha_ref,I.s.alpha_ref)
#undef NRN_F
    for(unsigned st=0;st<NRN_TEX_STAGES;st++)for(uint32_t b=tex[st];b;b&=b-1){const unsigned i=unsigned(__builtin_ctz(b));
        if(state.tex[st][i]==I.tex[st][i])continue;
        if(first<0)first=NR3_C_N;tex_stages|=1u<<st;if(put)state.tex[st][i]=I.tex[st][i];}
    return first;}
inline const char *part_name(int f){return f==NR3_C_N?"texture slot":nr3_field_name(f);}
// From nbdraw::apply, before anything of the record is applied (the header and the binds of the record were viewed).
void pre(const uint8_t *p,size_t n,const nbd_hdr &h,const uint8_t *binds){
    for(unsigned i=0;i<h.nbind;i++){nbd_bind bd;memcpy(&bd,binds+sizeof bd*i,sizeof bd);if(bd.kind==NBD_B_TEX && bd.stage<NRN_TEX_STAGES && bd.slot<NRN_TEX_SLOTS)W.tex[bd.stage]|=1u<<bd.slot;}   // the texture slots this record binds
    if(!(h.flags&NBD_F_NRN) || !(h.flags&NBD_F_NR))return;
    nrn_tail t;if(!nrn_tail_get(p,n,nrn_tail_off(&h),h.flags,&t) || !(t.rsv&NRN_T_SHORT))return;
    nrn_bstat &x=b[t.pass];++x.shorts;
    nrn_short_ext sx;const bool ext=nrn_short_ext_get(p,n,nrn_tail_off(&h)+NRN_TAIL_BYTES,&sx)!=0;
    int made=0;nrn_row *r=nrn_rows_get(&R,t.card,t.pass,&made);Img *I=r && !made?img_of(r):nullptr;pre_row=r;pre_card=r?t.card:0;
    if(!I || !ext){++x.s_noimg;if(told<16){++told;xbg_logf("【红】[nr] native: a short record %s (pass %s, card %016llx, frame %llu) - the state and the textures it left out are whatever State holds",
        ext?"of a card that has no image":"without its list of what was left out",nrn_pass_name(t.pass),(unsigned long long)t.card,(unsigned long long)stats.presents.load());}return;}
    uint32_t tm[NRN_TEX_STAGES]={};for(int k=0;k<2;k++)if(sx.st[k]!=0xff)tm[sx.st[k]]|=sx.tm[k];
    unsigned ts=0;
    if(img_walk(*I,sx.m,tm,true,ts)>=0){++x.s_changed;
        if(nbpipe::mode)nbpipe::G->dirty|=(1u<<(NR3_K_VFMT+1))-1;   // as nrstate3::exec after a state record: the groups are read again
        for(unsigned m=ts;m;m&=m-1)++tex_gen[__builtin_ctz(m)];     // as nbdraw::apply after texture binds
        if(nbdesc::mode>0){nbdesc::cand=false;nbdesc::P.valid=false;}}}
// From nbdraw::apply (record), after the binds of a LONG record with a card landed, before its draw.
void image(){
    nrn_bstat &x=b[cur.pass];
    int made=0;nrn_row *r=nrn_rows_get(&R,cur.card,cur.pass,&made);
    if(!r){++x.img_full;if(!full_told){full_told=true;xbg_logf("【红】[nr] native: the rows of the cards are full (%u) - new cards get no row (drawn by the path without it; raise DM_NRN_MAX)",R.cap);}return;}
    cur_row=r;
    const size_t i=size_t(r-R.e);if(imgs.size()<R.cap)imgs.resize(R.cap);
    Img *I=imgs[i].get();
    // series CN (CN_a0: the translator paid more for the images than the rows saved): in apply mode the image of a published card is
    // compared on 1 long record in 8, in verify mode on every one (the image is only ever READ by a short record of the card)
    if(I && mode[cur.pass]==1 && !r->unstable && (++img_tick&7)){++x.img_skip;return;}
    if(I){unsigned ts=0;const int f=img_walk(*I,W.m,W.tex,false,ts);
        if(f<0){++x.img_same;return;}
        ++x.img_bad;r->unstable=1;nrn_pub_drop(&PUB,cur.card);
        if(told<16){++told;xbg_logf("【红】[nr] native: what the records of a draw wrote is not the image of its card - %s differs (pass %s, card %016llx, frame %llu, vs=%s ps=%s): the card does "
            "not decide what the state packets and texture binds leave; the card is not written short any more",part_name(f),nrn_pass_name(cur.pass),(unsigned long long)cur.card,
            (unsigned long long)stats.presents.load(),state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-");}}
    else{imgs[i]=std::make_unique<Img>();I=imgs[i].get();++x.img_new;}
    nrstate3::snap_of(&I->s,&state,missing_program);
    for(unsigned st=0;st<NRN_TEX_STAGES;st++)memcpy(I->tex[st],state.tex[st],sizeof I->tex[st]);
    if(!r->unstable)nrn_pub_set(&PUB,cur.card);}
void img_release(){imgs.clear();imgs.shrink_to_fit();W=Wrote{};full_told=false;cur_row=pre_row=nullptr;pre_card=0;}
// Eviction (nr_nrn.h): once a submission. The images follow their rows into the rebuilt table.
std::vector<std::unique_ptr<Img>> imgs2;uint64_t ev_told=0;
void ev_move(void *,uint32_t from,uint32_t to){if(from<imgs.size() && imgs[from]){if(imgs2.size()<R.cap)imgs2.resize(R.cap);imgs2[to]=std::move(imgs[from]);}}
void evict_tick(){
    const int k=nrn_rows_tick(&R,&PUB,&idle,ev_move,nullptr,nullptr);
    if(k!=2)return;
    imgs.swap(imgs2);imgs2.clear();cur_row=pre_row=nullptr;pre_card=0;P.valid=false;full_told=false;}
// From nbdraw::apply after the draw: a prediction whose draw never reached the lane's tail (the old tail finished it, a prologue dropped it).
void lost(){W=Wrote{};if(P.valid){P.valid=false;++b[cur.pass].p_lost;}}   // (W: what was written since the previous draw record starts again)
void exec_tick(){
    for(int p=0;p<NRN_P_N;p++){const nrn_bstat &x=b[p];
        if(!x.asked && !x.learned && !x.kept && !x.shorts && !x.img_new && !x.img_same)continue;
        std::string w;for(int i=0;i<NRN_BW_N;i++)if(x.why[i]){char t[96];snprintf(t,sizeof t,"%s%s %llu",w.empty()?"":", ",nrn_bw_name(i),(unsigned long long)x.why[i]);w+=t;}
        const double as=x.asked?double(x.asked):1.;const uint64_t ans=x.answered[1]+x.answered[2]+x.answered[3];
        xbg_logf("[nr] native %s rows (DM_NRN_%s=%d, step B, last 10 s): %llu draws with a card asked their row; answered %llu (%.1f%%: recorded from the row %llu, the old tail finished %llu, "
            "the pass began anew %llu); verify: predicted %llu (%.1f%%), the path without the row resolved the same %llu, BAD %llu, not compared (the draw left the short path) %llu; not asked / "
            "refused: %s; rows learnt %llu, replaced %llu, confirmed %llu; table %u of %u rows; the game thread saw the card published for %llu records - %llu of them "
            "answered / predicted here, %llu not; part 2: SHORT records %llu (State changed by the image %llu, WITHOUT an image %llu); images of long records: new %llu, the same as State %llu, "
            "DIFFER %llu, not kept (table full) %llu, not compared (apply: 1 in 8 is) %llu; eviction: rows retired %llu, removed %llu, revived %llu, idle limit %u ticks",
            nrn_pass_name(p),nrn_pass_name(p),int(mode[p]),(unsigned long long)x.asked,(unsigned long long)ans,100.*double(ans)/as,(unsigned long long)x.answered[1],(unsigned long long)x.answered[2],
            (unsigned long long)x.answered[3],(unsigned long long)x.predicted,100.*double(x.predicted)/as,(unsigned long long)x.p_same,(unsigned long long)x.p_bad,(unsigned long long)x.p_lost,
            w.empty()?"none":w.c_str(),(unsigned long long)x.learned,(unsigned long long)x.relearned,(unsigned long long)x.kept,R.n,R.cap,
            (unsigned long long)x.ready,(unsigned long long)x.ready_ans,(unsigned long long)x.ready_not,(unsigned long long)x.shorts,(unsigned long long)x.s_changed,(unsigned long long)x.s_noimg,
            (unsigned long long)x.img_new,(unsigned long long)x.img_same,(unsigned long long)x.img_bad,(unsigned long long)x.img_full,(unsigned long long)x.img_skip,
            (unsigned long long)R.retired,(unsigned long long)R.removed,(unsigned long long)R.revived,idle);
        b[p]=nrn_bstat{};}}
}
extern "C" const void *xbg_nrn_pub(void){return &nrn::PUB;}
