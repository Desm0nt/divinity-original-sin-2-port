// [DOS2] NR-ZERO Z3 (docs/dos2/nr/NR-ZERO.md §9): DM_NR_Z3 - "the same draw that is already resolved". Included by xbg_game.cpp right
// after xbg_nr_draw.inc (it reads nrdesc, nrdraw, nbpipe, uiarea); declared ahead of draw(). The key, the table, the stamps and the
// classes of the comparison are in dm/gfx/nr/nr_z3.h (pure C, host test tests/nr_z3_test.c).
//
// Today an engine draw the short path records goes: descriptors_stable -> descriptors() -> NR-08 serve() (the shader set's plan, its entry
// by texture generation or by a hash of the handles, the blocks, the images, the set) -> create_pass (the open pass continues) ->
// nrdraw::lane() (gates, state-group ids, the variant, NR-09's table, then the tail: views, viewport, bookkeeping, commands). Three
// lookups in it answer the same for every draw of the same material in the same pass state: the plan, the entry id, the pipeline.
// nrz3 keeps ONE table from the words those lookups read (nr_z3.h nrz_key) to their answers and, on a hit, runs the rest in a straight
// line: no Descriptors record, no descriptors() prologue, no second key, no Pass copy on the heap, no variant, no table walk.
//
// Exact by construction. What a hit skips are lookups; every statement with a side effect runs as on the path without the table, in
// the same order, most of it the same code:
//   checked live, as serve() / create_pass / lane() check it   the gates of descriptors() and of the lane, the plan's back-off, the
//       memo epoch, the entry (live, its ring buffer, images_alive), create_pass's own "the open pass continues" test,
//       uiarea::detach (asked without side effects: detach_peek), the state-group ids (nrdraw::groups, the lane's own call)
//   computed live with the path's own functions                 nrdesc::blocks (ring offsets through nrbuf::ubo: the validity of an in-place
//       view - chunk liveness, sticky pages, pending writebacks, the UI shift - is asked per draw as before), the entry's set and its
//       recycler node (nr_dfifo_use), serve()'s image loop (nr_dimg_skip, get_image + prepare_read for an image on the read path; when
//       that ended the submission the draw is built again by the path without the table, as descriptors_stable does; when it ended the
//       open pass draw() gets the answered descriptors and runs create_pass and the lane: return 3), the attachments' write marks, and
//       all of nrdraw::lane_tail (pass check, vertex / index views
//       through snapshot(), viewport / scissor, uiarea, the bind cache, the vkCmd*, the counters)
//   from the table                                               plan, NR-08 entry id, NR-09 table entry (its pipeline and vertex binding list are
//       read from the entry at every draw) - valid while the key words are the same and the three generations (NR-08's ids, NR-09's
//       table clears, the backend's object epoch) did not move
// Nothing is changed before the last check that can fail (but for the idempotent calls of blocks() and of the set's recycler node): a
// draw that is not answered takes the path without the table from its first line. One exception by design: the tail may leave the draw to the old tail after the answer (a view, the viewport, the pass ended
// inside a snapshot) - then draw() gets the Descriptors record and the Pass serve() and create_pass would have left and goes on exactly
// as after a lane() that returned false (return 2).
//
//   DM_NR_Z3   0  default (xbg_nr_z3_default): draw() pays one load and a not-taken branch; no table.
//              1  on. Needs DM_NR_DRAW=1 and DM_NR_DESC=1 (else a red line, off).
//              2  verify: the table is asked (every check before the first side effect), nothing is taken from it; the path without the
//                 table draws; where it recorded the draw its plan / NR-08 entry id / NR-09 entry are compared with the table's
//                 answer, where it left the short path the step is classified (nr_z3.h nrz_classify): bad = a step the table's key,
//                 stamps or checks must cover. Red lines (first 16) + the "[nr] z3 check" line.
//              3  does not exist: an answered draw records the same commands, there is nothing to poison (red line, off).
//   DM_NR_Z3_MAX  entries of the table (4096; a power of two, 256..65536; 448 bytes each)
//   DM_NR_Z5      xbg_nr_z5.inc: =2 compares the packet side of Z5 (the run assembler against the tail's packets); =1 is refused - the
//                 game thread's half is not built (NR-ZERO.md section 14).
//   DM_NR_Z3B / DM_NR_Z3C / DM_NR_Z3D   xbg_nr_z3e.inc: the image word, the mesh view table, the ring word (each 0 / 2 verify / 1).
//   DM_NR_Z3K     the cheap key (NR-ZERO.md section 17; dm/gfx/nr/nr_z3k.h): a front table indexed by the record's key id (the game
//                 thread's identity of the draw) and the words in hand, its entry compared IN PLACE with the live state - no plan
//                 lookup, no gather, no hash, no 448-byte entry before the answer. 0 default (xbg_nr_z3k_default): one load and a
//                 not-taken branch. 1 on: a front hit is the table's answer (with DM_NR_Z3=2 it is the prediction the path without the
//                 table is compared with). 2 verify: the front table is asked, the table of nr_z3.h is asked too and answers; where
//                 both answer they must agree, where only the front does its answer is compared with what the path then resolves.
//                 Needs DM_NR_Z3 != 0. Lines "[nr] z3 key: ..." and "[nr] z3 key by part ..." every 10 s.
//   With DM_NR_PRESKIN: nrps::decide runs at its place (after the answered pass); a draw of a skinning vertex shader under
//                 DM_NR_PRESKIN=1 gets the answered descriptors and pass and goes through nrdraw::lane itself (the variant carries
//                 PRESKIN's bits, the lane resolves it); DM_NR_ONCHIP superseded by DM_NR_LIGHTFUSE leaves nroc::mode 0: not a gate.
// Lines every 10 s: "[nr] z3: ...", "[nr] z3 by part ...", with =2 "[nr] z3 check: ..."; at the start "[nr] DM_NR_Z3=..."; red lines
// "【红】[nr] z3: ...". "[nr] draw span ..." (nrdraw::tick, any mode): the time from the command buffer to the end of a recorded engine
// draw, the number that compares DM_NR_Z3=0 and =1.
#include "../../nr/nr_z3.h"
#include "../../nr/nr_z3k.h"
static_assert(sizeof(state.tex)==sizeof(uint64_t)*NRZK_STAGES*NRZK_SLOTS && sizeof(state.samp)==sizeof(state.tex) && NRD_KEY_WORDS>=NRZ_KEY_SH+NRZ_KEY_FIXED+2*NRZ_IMG_MAX,"nr_z3k.h reads the binding tables of the backend");
namespace nrz3{
int mode=-1;   // -1: not read yet (NR-08 reads its switch at the first draw)
nrz_tab T{};bool tab_ok=false;uint32_t max_ents=4096;
uint64_t span_t0=0,span_calls=0,span_ns=0,span_n=0;   // "[nr] draw span": set by draw() for 1 engine draw in 64, closed by the tail that recorded it
void span_end(){span_ns+=timing_clock()-span_t0;++span_n;span_t0=0;}
// the shader set's plan without the map (direct mapped by vertex / fragment shader; the five objects are compared)
struct PlanSlot{nrdesc::ShKey k{};nrdesc::Plan *pl=nullptr;uint64_t gen=0;bool frag=false;};PlanSlot pslot[1024];
// the draw in hand: its key (kept when the table did not answer: served() stores the resolution under it), the prediction of mode 2
struct Key{bool ok=false;unsigned n=0,ng=0;uint64_t h=0,w[NRZ_KEY_MAX]{};nrdesc::Plan *pl=nullptr;bool frag=false;} K;
struct Pred{bool valid=false;nrz_val v{};uint64_t pfh=0;uint64_t vs=0,fs=0;} P;
// [Z3K] the front table (nr_z3k.h). FP: the front table's answer of the draw in hand when the table of nr_z3.h did not hold the key
// (DM_NR_Z3K=2): compared with what the path without the tables resolves (served()).
int kmode=0;nrzk_tab F{};bool f_ok=false;
struct FPred{bool valid=false;nrz_val v{};} FP;
struct KStats{uint64_t asked=0,hit=0,promoted=0,stored=0,checked=0,same=0,front_only=0,bad=0;} ks;
struct Stats{uint64_t asked=0,served=0,why[NRZW_N]{},learned=0,odd=0,ns=0,timed=0,sp_n=0,sp_look=0,sp_gate=0,sp_front=0,sp_full=0,sp_desc=0,sp_blk=0,sp_img=0,sp_pass=0,img=0,img_rd=0,ubos=0,
    predicted=0,same=0,live=0,bad=0,unresolved=0,psk_rec=0,psk_old=0;} s;
// [PRESKIN] ps_called: this draw's nrps::decide ran here (draw() does not call it again when the old tail finishes the draw: return 2)
bool ps_called=false;
uint64_t next_ns=0,calls=0;unsigned told=0;
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
void red(const char *fmt,...){if(told>=16)return;++told;char b[640];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof b,fmt,ap);va_end(ap);
    xbg_logf("【红】[nr] z3: %s (frame %llu, vs=%s ps=%s)",b,(unsigned long long)stats.presents.load(),state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-");}
void init(){
    if(nrdesc::mode<0 || !nrdraw::read_env)return;   // their switches first (asked again at the next draw)
    const char *e=getenv("DM_NR_Z3");int m=e && *e?atoi(e):xbg_nr_z3_default();
    if(m==3){xbg_logf("【红】[nr] DM_NR_Z3=3 does not exist (an answered draw records the same commands, nothing to poison; =2 compares): off");m=0;}
    if(m<0 || m>2){xbg_logf("【红】[nr] DM_NR_Z3=%d: only 0 / 1 / 2: off",m);m=0;}
    if(m && (nrdraw::mode!=1 || nrdesc::mode!=1)){xbg_logf("【红】[nr] DM_NR_Z3=%d needs DM_NR_DRAW=1 (now %d) and DM_NR_DESC=1 (now %d): off",m,nrdraw::mode,nrdesc::mode);m=0;}
    if(m){e=getenv("DM_NR_Z3_MAX");uint32_t n=e && *e?uint32_t(std::clamp(atoi(e),256,65536)):4096u;while(n&(n-1))n&=n-1;max_ents=n;
        tab_ok=!nrz_init(&T,max_ents);if(!tab_ok){xbg_logf("【红】[nr] DM_NR_Z3=%d: no memory for %u entries: off",m,max_ents);m=0;}}
    {e=getenv("DM_NR_Z3K");int k=e && *e?atoi(e):xbg_nr_z3k_default();   // [Z3K] the cheap key
     if(k==3){xbg_logf("【红】[nr] DM_NR_Z3K=3 does not exist (the front table answers what the table answers, nothing to poison; =2 compares): off");k=0;}
     if(k<0 || k>2){xbg_logf("【红】[nr] DM_NR_Z3K=%d: only 0 / 1 / 2: off",k);k=0;}
     if(k && !m){xbg_logf("【红】[nr] DM_NR_Z3K=%d needs DM_NR_Z3=1 or 2 (now 0): off",k);k=0;}
     if(k){f_ok=!nrzk_init(&F,max_ents,16384);if(!f_ok){xbg_logf("【红】[nr] DM_NR_Z3K=%d: no memory for %u entries: off",k,max_ents);k=0;}}
     if(k)xbg_logf("[nr] DM_NR_Z3K=%d (NR-ZERO Z3, the cheap key: %s): a front table of %u entries (%zu KB) indexed by the record's key id, the variant words, the pass definition and up to %d texture handles; "
        "its entry holds the full key and is compared in place with the shader objects, the fixed words, the bound texture / sampler handles of its rows, the key id, the variant and the pass definition; "
        "cleared when NR-08's table generation, NR-09's table generation or the object epoch moves",k,k==2?"verify - asked, the table of DM_NR_Z3 answers, the two are compared":"on",max_ents,size_t(max_ents)*sizeof(nrzk_ent)/1024,int(NRZK_HINT_ROWS));
     kmode=k;}
    nrz5::init(nrdraw::mode);   // [Z5] DM_NR_Z5 (xbg_nr_z5.inc): =2 the comparison of the packet side; =1 refused - the game thread's half is not built
    nrz8t::init(nrdraw::mode);   // [Z8] DM_NR_Z8 (xbg_nr_z8.inc): the submit walk starts answering (or the switch is refused)
    if(m)xbg_logf("[nr] DM_NR_Z3=%d (NR-ZERO Z3: %s): an engine draw whose shader set, texture / sampler handles, pool, key id, variant and render pass definition are in the table "
        "(%u entries, %zu KB) takes its plan, NR-08 entry and pipeline from it; ring offsets, image checks, the set, the pass check, the views, viewport and the commands by the "
        "same code as without the table; stale on NR-08's table generation, NR-09's table generation, the object epoch",
        m,m==2?"verify - the table is asked, the path without it draws, the answers are compared":"on",max_ents,size_t(max_ents)*sizeof(nrz_ent)/1024);
    nrzx::init(nrdesc::mode,nrdraw::mode,nrdraw::ui_mode);   // [Z3b / Z3c / Z3d] their switches (independent of DM_NR_Z3)
    mode=m;}
void release(){if(tab_ok){nrz_free(&T);tab_ok=false;}if(f_ok){nrzk_free(&F);f_ok=false;}kmode=0;FP.valid=false;if(mode>0)mode=0;K.ok=false;P.valid=false;for(auto &x:pslot)x=PlanSlot{};}
// DM_NR_Z3=2: the prediction of a draw the lane did not record (it never reached served()): which step left the short path
void resolve_prev(){if(!P.valid)return;P.valid=false;
    const int c=nrz_classify(1,nrdesc::z_out,int(nrdesc::z_rd),pass_fast_hits!=P.pfh,nrdraw::zl?nrdraw::zl:1,0);
    if(c==NRZC_LIVE){++s.live;return;}
    ++s.bad;red("the table answered a draw the path without it did not record: NR-08 %s, images on the read path %u, create_pass %s, the lane %s",
        nrdesc::z_out==NRZD_HIT?"hit its entry":nrdesc::z_out==NRZD_BUILT?"made / remade the entry":nrdesc::z_out==NRZD_FELL_LIVE?"left at a block / set":nrdesc::z_out==NRZD_FELL_GATE?"left at a gate":"was not asked",
        nrdesc::z_rd,pass_fast_hits!=P.pfh?"continued the open pass":"did not take its fast path",nrdraw::zl==1?"left before its pipeline was known":nrdraw::zl==2?"left in its tail":nrdraw::zl==3?"was not asked":"recorded");}
void tick(){   // every submission (nrdraw::tick), mode > 0
    const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns || !s.asked)return;next_ns=now+10000000000ull;
    if(mode==2)resolve_prev();
    std::string w;for(int i=1;i<NRZW_N;i++)if(s.why[i]){char b[96];snprintf(b,sizeof b,"%s%s %llu",w.empty()?"":", ",nrz_why(i),(unsigned long long)s.why[i]);w+=b;}
    const nrdraw::Stats &z=nrdraw::sz;const double sv=z.served?double(z.served):1.;
    xbg_logf("[nr] z3: DM_NR_Z3=%d; %llu engine draws asked, %s %llu (%.1f%%), recorded by the shared tail %llu; not answered: %s; table: %u entries max, hits %llu, no entry %llu, "
        "stale %llu, stored %llu (replaced %llu), odd %llu; DM_NR_PRESKIN draws answered and handed to the lane: recorded %llu, old tail %llu; %.1f ring blocks and %.1f images an answered draw (read path %.2f); vkCmd* a served draw %.2f (pipeline %.2f, set %.2f, vertex %.2f, index %.2f, "
        "viewport %.3f, scissor %.3f, + the draw); %.0f ns a served draw from the lookup to the end (1 in 64)",
        mode,(unsigned long long)s.asked,mode==2?"would answer":"answered",(unsigned long long)(mode==2?s.predicted:s.served+s.why[NRZW_LATE]+s.psk_rec+s.psk_old),
        100.*double(mode==2?s.predicted:s.served+s.why[NRZW_LATE]+s.psk_rec+s.psk_old)/double(s.asked),(unsigned long long)s.served,w.empty()?"none":w.c_str(),max_ents,
        (unsigned long long)T.hits,(unsigned long long)T.misses,(unsigned long long)T.stale,(unsigned long long)T.puts,(unsigned long long)T.replaced,(unsigned long long)s.odd,(unsigned long long)s.psk_rec,(unsigned long long)s.psk_old,
        s.served?double(s.ubos)/double(s.served):0.,s.served?double(s.img)/double(s.served):0.,s.served?double(s.img_rd)/double(s.served):0.,
        double(z.cmds)/sv,double(z.c_pipe)/sv,double(z.c_set)/sv,double(z.c_vb)/sv,double(z.c_ib)/sv,double(z.c_vp)/sv,double(z.c_sc)/sv,z.timed?double(z.ns)/double(z.timed):0.);
    if(z.sp_n && s.sp_n){const double n=double(z.sp_n),m=double(s.sp_n);   // where an answered draw's time goes (the clock reads of the split are inside the parts)
        xbg_logf("[nr] z3 by part (NR-ZERO, %llu served draws split, 1 in 128): %.0f ns = gates, key and lookup %.0f + entry, blocks, images and set %.0f + pass marks %.0f + vertex / index views %.0f + "
            "viewport and checks %.0f + bookkeeping and vkCmd* %.0f + finish %.0f",(unsigned long long)z.sp_n,(z.sp_key+z.sp_vtx+z.sp_vp+z.sp_cmd+z.sp_tail)/n,s.sp_look/m,s.sp_desc/m,s.sp_pass/m,
            z.sp_vtx/n,z.sp_vp/n,z.sp_cmd/n,z.sp_tail/n);
        // [Z3K] the first two parts once more, finer (their clock reads are inside the parts: 5 more a split draw)
        xbg_logf("[nr] z3 key by part (NR-ZERO, DM_NR_Z3K=%d, %llu served draws split): gates, key and lookup %.0f ns = gates, state-group ids, variant and fixed words %.0f + front table %.0f + "
            "plan, pass checks, full key and the table of DM_NR_Z3 %.0f + entry checks %.0f; entry, blocks, images and set %.0f ns = uniform blocks %.0f + images %.0f + set and the marks of the answer %.0f",
            kmode,(unsigned long long)s.sp_n,s.sp_look/m,s.sp_gate/m,s.sp_front/m,s.sp_full/m,(s.sp_look-s.sp_gate-s.sp_front-s.sp_full)/m,s.sp_desc/m,s.sp_blk/m,s.sp_img/m,(s.sp_desc-s.sp_blk-s.sp_img)/m);}
    if(kmode){const uint64_t fa=ks.asked?ks.asked:1;
        xbg_logf("[nr] z3 key: DM_NR_Z3K=%d; %llu draws asked the front table, it %s %llu (%.1f%%); entries learned from the table of DM_NR_Z3 %llu, stored after the path without the tables %llu, "
            "replaced %llu, table cleared %llu (of them the fixed-words ids were full %llu), fixed-words sets %llu; check: compared %llu, same %llu, bad %llu; "
            "front answers the table of DM_NR_Z3 no longer held (its two ways were taken) %llu",kmode,(unsigned long long)ks.asked,kmode==2?"would answer":"answered",(unsigned long long)ks.hit,
            100.*double(ks.hit)/double(fa),(unsigned long long)ks.promoted,(unsigned long long)ks.stored,(unsigned long long)F.replaced,(unsigned long long)F.clears,(unsigned long long)F.fixed_full,
            (unsigned long long)F.fixed_new,(unsigned long long)ks.checked,(unsigned long long)ks.same,(unsigned long long)ks.bad,(unsigned long long)ks.front_only);
        ks=KStats{};F.hits=F.misses=F.puts=F.replaced=F.clears=F.fixed_new=F.fixed_full=0;}
    if(mode==2 || s.bad)
        xbg_logf("[nr] z3 check (DM_NR_Z3=2): the table answered %llu draws before the path without it ran: same plan / NR-08 entry / NR-09 entry %llu, the path left at a step "
            "the table does not cache (block, set, views, viewport, pass check) %llu, bad %llu; stored after a miss %llu",
            (unsigned long long)s.predicted,(unsigned long long)s.same,(unsigned long long)s.live,(unsigned long long)s.bad,(unsigned long long)s.learned);
    s=Stats{};nrdraw::sz=nrdraw::Stats{};T.hits=T.misses=T.stale=T.puts=T.replaced=0;}
// draw(), every draw while mode != 0, before it asks: the key of the draw before is not this draw's.
void begin(){if(mode<0){init();if(mode<0)return;}K.ok=false;ps_called=false;FP.valid=false;}
inline int out(int w){++s.why[w];return 0;}
inline nrdesc::Plan *plan_of(bool &frag){
    const nrdesc::ShKey k=nrdesc::shkey();
    PlanSlot &x=pslot[((uint64_t(uintptr_t(k[0]))>>4)*0x9E3779B97F4A7C15ull^(uint64_t(uintptr_t(k[4]))>>4)*0xC2B2AE3D27D4EB4Full)>>54];
    if(!(x.pl && x.gen==nrdesc::tab_gen && x.k==k)){x.pl=nrdesc::plan_for();x.k=k;x.gen=nrdesc::tab_gen;x.frag=desc_frag_only_shaders();}
    frag=x.frag;return x.pl;}
// create_pass's fast path would answer now: a plain draw (the gates above), uiarea::detach false without doing anything, the open pass is
// last_pass's with the same targets.
inline bool pass_continues(const Commands &c){
    if(uiarea::mode && state.rt.depth && uiarea::detach_peek(c.command)!=0)return false;
    return last_pass.valid && open_pass.cmd==c.command && open_pass.pass==last_pass.pass.pass && open_pass.framebuffer==last_pass.pass.framebuffer && !memcmp(&state.rt,&last_pass.rt,sizeof state.rt);}
// [Z3K] the rows of a plan as nr_z3k.h keeps them: where nrdesc::gather reads the (texture, sampler) words of the key
inline void front_rows(const nr_dplan &p,nrzk_row *row){for(unsigned i=0;i<p.nimg && i<NRZ_IMG_MAX;i++){const nr_drow &r=p.row[p.img_row[i]];row[i]=nrzk_row{uint8_t(r.ns),uint8_t(r.slot),uint8_t(r.samp)};}}
// [Z3K] after the table of nr_z3.h was asked for the key K of a draw the front table did not answer for (a miss, or DM_NR_Z3K=2):
// where the table answers, the front table learns the entry (or, =2 with a front answer, the two must agree); a front answer the table
// does not hold waits for the path's own resolution (served()).
inline void front_after(const nrz_val *tv,int stale,const nrzk_ent *fe,const nrz_val &fv,nrdesc::Plan &pl,const uint64_t stamp[NRZ_STAMPS]){
    if(fe){++ks.checked;
        if(tv){if(nrz_val_same(tv,&fv))++ks.same;else{++ks.bad;red("Z3K: the front table answers plan %p / NR-08 entry %d / NR-09 entry %p, the table of the full key plan %p / %d / %p",
            (void*)uintptr_t(fv.plan),fv.did,(void*)uintptr_t(fv.pent),(void*)uintptr_t(tv->plan),tv->did,(void*)uintptr_t(tv->pent));}}
        else if(stale){++ks.bad;red("Z3K: the front table answers a key whose entry in the table of the full key is stale (a stamp the front table did not see)");}
        else{++ks.front_only;FP.valid=true;FP.v=fv;}
        return;}
    if(!tv)return;
    nrzk_row row[NRZ_IMG_MAX];front_rows(pl.p,row);
    if(nrzk_put(&F,stamp,K.w,K.n,pl.p.nimg,row,tv))++ks.promoted;}
// The gates of descriptors() and nrdesc::Scope, of the lane, and the switches whose draws need what an answered draw does not make.
// 0: none refuses; else the NRZW_* reason. groups: also refresh the state-group ids the variant word names ([NR-ZERO Z8] nrz8t::draw
// takes its NR-09 entry without the variant: false).
inline int gate(const nrd_tail &t,bool pp,int draw_fast,bool groups){
    if(nrdesc::mode!=1 || nrdraw::mode!=1)return NRZW_GATE;
    if(!t.key || t.key>=NRP_TAB_ROWS || t.site==NRD_SITE_UI)return NRZW_SPECIAL;
    if(!desc_memo_enabled || capture.active || provenance_file || g_ui_moff!=0 || pgraph::armed || nroc::twin_draw || nrlf::fused_draw || read_preflight)return NRZW_GATE;
    if(pipe_stats || vtxstats::enabled() || crumbs::active || !bind_cache_enabled || ab_off("nrdraw") || ab_off("passfast"))return NRZW_GATE;
    if(nroc::mode>0 || nrpo::mode!=0 || nrps::mode<0 || nrsort::mode!=0 || nrmdi::mode!=0)return NRZW_GATE;   // ONCHIP (when DM_NR_LIGHTFUSE supersedes it nroc::mode is 0) adjusts the pass after create_pass; the others read the Descriptors record / their own views; PRESKIN not read yet: the path without the table reads it
    if(draw_fast!=1 || !pipe_memo || !pipeline_fast_enabled || ab_off("pipefast") || ab_off("pipememo") || (groups && !(pp?nrdraw::groups_pp():nrdraw::groups())))return NRZW_GATE;
    if(!state.shader[0] || state.shader[1] || state.shader[2] || state.shader[3])return NRZW_SPECIAL;
    return 0;}
// The answer of a draw and what the answered half needs of the first half. v: the plan, NR-08 entry id and NR-09 entry; varw: the
// lane's variant words (null: [NR-ZERO Z8] the answer came without them - nothing is learned from this draw); z8: the caller is
// nrz8t::draw (nrz3's own key and prediction are not touched); tg / tf / tk: the clock reads of the first half (a draw timed by part).
struct Ans{const nrz_val *v=nullptr;nrdesc::Plan *pl=nullptr;bool frag=false,fhit=false,psk=false,pp=false,z8=false;const uint64_t *f=nullptr,*varw=nullptr;uint32_t re=0,active_colors=0;const nrd_tail *t=nullptr;uint64_t tg=0,tf=0,tk=0;};
// The plan of the shader set and create_pass's fast path as they would answer now: 0, or the NRZW_* reason. Nothing is changed.
inline int plan_pass(const nrdesc::Plan *pl,const Commands &c){
    const nr_dplan &dpn=pl->p;
    if(dpn.why || pl->retry || !pl->layout || !pl->layout->definition.valid)return NRZW_NO_PLAN;
    if(memo_epoch!=nrdesc::my_epoch)return NRZW_DESC;   // views, samplers or the ring buffer were replaced: serve() flushes
    if(!pass_continues(c))return NRZW_PASS;
    if(!last_pass.pass.definition || !last_pass.pass.definition->valid)return NRZW_GATE;
    return 0;}
int answered(Stats &s,const Ans &a,Commands &c,Descriptors *&dp,Pass &p,int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances,uint32_t first_instance,int draw_fast,nrdraw::Tm &tm);
// See the header and the declaration before draw(). 0: not answered (the path without the table runs from its first line); 1: recorded;
// 2: answered, the old tail finishes; 3: the descriptors are answered (dp), the pass is not - create_pass and the lane run.
int draw(Commands &c,Descriptors *&dp,Pass &p,int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances,uint32_t first_instance,bool pp,int draw_fast){NRCPU_SCOPE(NrZ3);
    if(mode<=0)return 0;
    ++s.asked;nrdraw::Tm tm;tm.timed=!(++calls&63);tm.t0=tm.timed?timing_clock():0;tm.sp=tm.timed && (calls>>6&1);uint64_t tg=0,tf=0,tk=0;
    if(mode==2){resolve_prev();nrdraw::zl=3;}
    // ---- gates: of descriptors() and nrdesc::Scope, of the lane, and the switches whose draws need what a hit does not make ----
    const nrd_tail &t=*nrdraw::cur;
    if(const int gw=gate(t,pp,draw_fast,true))return out(gw);
    // [PRESKIN] DM_NR_PRESKIN=1 and a skinning vertex shader: nrps::decide (live: the record of this submission) picks the lane's variant, so
    // the pipeline is not the table's - the answered descriptors and pass go to the lane itself (nrdraw::lane, the whole of it)
    const bool psk=nrps::mode==1 && state.shader[0]->psk==1;
    if(nrdraw::epoch!=nbpipe::epoch)return out(NRZW_STALE);   // the lane clears its table at its next draw
    // ---- the variant words and NR-08's fixed words: in hand without the plan (pure reads; the pass they name is checked below) ----
    uint32_t active_colors=0;for(int i=0;i<state.rt.n;i++)if(state.rt.color[i])active_colors|=1u<<i;
    const uint32_t re=nrdraw::restart_eff_of(prim,indexed);
    nr_draw_var_t var;nr_draw_var(&var,uint64_t(uintptr_t(state.shader[0])),uint64_t(uintptr_t(state.shader[4])),0,uint64_t(uintptr_t(last_pass.pass.definition)),
        unsigned(prim),unsigned(last_pass.pass.samples),unsigned(state.rt.n),active_colors,state.rt.depth!=nullptr,state.rt.depth?unsigned(state.rt.depth->d.format):0u,re,nbpipe::G->gid);
    if(pp)var.w[4]|=NR_DRAW_VAR_PP;
    uint64_t f[6];nrdesc::fixed_words(f);
    const uint64_t stamp[NRZ_STAMPS]={nrdesc::tab_gen,nrdraw::tab_gen(),nbpipe::epoch};
    if(tm.sp)tg=timing_clock();
    // ---- [Z3K] the front table: the entry whose full key is the live state's (nr_z3k.h: indexed by the record's key id, compared in place) ----
    const nrzk_ent *fe=nullptr;nrz_val fv{};
    if(__builtin_expect(kmode!=0,0)){++ks.asked;nrzk_sync(&F,stamp);
        if(const uint32_t fid=nrzk_fixed(&F,f,0))fe=nrzk_find(&F,t.key,var.w[0],var.w[1],var.w[4],var.w[5],var.w[3],fid,state.tex,state.samp);
        if(fe){nrzk_val_of(fe,&fv);++ks.hit;}}
    const bool fhit=fe && kmode==1;   // (=2: asked and compared, the table of nr_z3.h answers)
    if(tm.sp)tf=timing_clock();
    // ---- the plan of the shader set (made by NR-08 at the set's first draw; a front hit holds it: plans go only with NR-08's generation) ----
    bool frag=false;nrdesc::Plan *pl=nullptr;
    if(fhit){pl=reinterpret_cast<nrdesc::Plan*>(uintptr_t(fv.plan));frag=fv.frag!=0;}else pl=plan_of(frag);
    const nr_dplan &dpn=pl->p;
    // ---- the plan can be served, create_pass's fast path would answer now ----
    if(const int w=plan_pass(pl,c))return out(w);
    if(dpn.nimg>NRZ_IMG_MAX)return out(NRZW_LONG);
    var.w[2]=uint64_t(uintptr_t(&pl->layout->definition));K.pl=pl;K.frag=frag;
    const nrz_val *v=&fv;
    if(!fhit){
        // ---- the key: NR-08's words + the lane's variant ----
        uint64_t g[NRD_KEY_WORDS];const unsigned ng=nrdesc::gather(dpn,f,g);   // g: 5 shader objects, the 6 fixed words, (texture, sampler) of each texture row
        K.n=nrz_key(K.w,g,g+NRZ_KEY_SH,g+NRZ_KEY_SH+NRZ_KEY_FIXED,dpn.nimg,t.key,var.w[4],var.w[5],var.w[3]);K.ng=ng;K.h=nrz_hash(K.w,K.n);
        int stale=0;const nrz_val *tv=nrz_find(&T,K.h,K.w,K.n,stamp,&stale);
        if(__builtin_expect(kmode!=0,0))front_after(tv,stale,fe,fv,*pl,stamp);
        if(!tv){K.ok=true;return out(stale?NRZW_STALE:NRZW_MISS);}   // served() stores this draw's resolution under K
        v=tv;}
    if(tm.sp)tk=timing_clock();
    Ans a;a.v=v;a.pl=pl;a.frag=frag;a.fhit=fhit;a.psk=psk;a.pp=pp;a.f=f;a.varw=var.w;a.re=re;a.active_colors=active_colors;a.t=&t;a.tg=tg;a.tf=tf;a.tk=tk;
    return answered(s,a,c,dp,p,prim,first,count,indexed,instances,first_instance,draw_fast,tm);}
// The answered half of draw(): NR-08's entry as serve() checks it, the blocks, the set, the image walk, the pass marks, then the
// lane's shared tail. Returns as draw() does. Nothing is changed before the draw is "answered" (the checks that can refuse come first).
int answered(Stats &s,const Ans &a,Commands &c,Descriptors *&dp,Pass &p,int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances,uint32_t first_instance,int draw_fast,nrdraw::Tm &tm){
    auto out=[&s](int w){++s.why[w];return 0;};
    const nrz_val *v=a.v;nrdesc::Plan *pl=a.pl;const nr_dplan &dpn=pl->p;const uint64_t *f=a.f;const bool psk=a.psk,pp=a.pp,frag=a.frag;const uint32_t active_colors=a.active_colors,re=a.re;
    uint64_t tl=0,td=0,tb=0,ti=0;
    if(a.z8){if(const int w=plan_pass(pl,c))return out(w);}   // the plan and the pass as draw() checks them before its key (the answer names the plan; the caller compared its pass definition)
    // ---- NR-08's entry, as serve() checks it ----
    if(reinterpret_cast<nrdesc::Plan*>(uintptr_t(v->plan))!=pl || v->did<0 || uint32_t(v->did)>=nrdesc::tab.n){++s.odd;if(!a.z8)K.ok=!a.fhit;return out(NRZW_DESC);}
    nrdesc::Ent &e=nrdesc::ents[size_t(v->did)];
    const nrp_tent *pe=reinterpret_cast<const nrp_tent*>(uintptr_t(v->pent));   // NR-09's entry of (key id, variant): the pipeline it holds NOW (0: destroyed - the old tail teaches it again)
    if(!psk && (!pe || !pe->pipe))return out(NRZW_PIPE);
    const VkBuffer rb=nrdesc::ring();
    if(e.plan!=pl || !e.live || e.ring!=rb || !nrdesc::images_alive(e))return out(NRZW_DESC);
    if(!a.z8 && mode==2){P.valid=true;P.v=*v;P.pfh=pass_fast_hits;P.vs=a.varw[0];P.fs=a.varw[1];++s.predicted;return 0;}   // verify: the path without the table draws; served() / resolve_prev() compare
    if(tm.sp)tl=timing_clock();
    uint32_t off[NRD_MAX_DYN],tuple[NRD_MAX_UBO];memcpy(off,e.off,dpn.ndyn*sizeof(uint32_t));
    if(nrdesc::blocks(*pl,rb,off,tuple,false))return out(NRZW_BLOCK);   // (the calls it made are repeated by serve(): idempotent)
    if(tm.sp)tb=timing_clock();
    nrdesc::Var *sv=nullptr;unsigned sk=0;
    for(unsigned k=0;k<2 && !sv;k++){nrdesc::Var &cv=e.var[k];if(cv.set && !memcmp(cv.tuple,tuple,dpn.nstat*sizeof(uint32_t)) && (cv.node<0 || nr_dfifo_use(&pl->fifo->f,cv.node,cv.gen,uint32_t(slot_serial)))){sv=&cv;sk=k;}}
    if(!sv)return out(NRZW_SET);   // serve() takes a set of the recycler and writes it
    // ---- answered: from here on what descriptors() + serve()'s hit, create_pass's fast path and lane() do, in their order ----
    storage_images_bound.clear();pgraph::cx_sh=nullptr;nrrp::frag_first=tile_barrier_on && frag;read_frag_only=nrrp::frag_first;   // descriptors(): its entry, its FragRead
    const uint64_t s0=slot_serial;uint32_t nrd=0;
    {const bool hook=!img_fast;   // serve()'s image loop, statement for statement: the read path only where it can do something
     const bool zq=__builtin_expect(nrzx::bmode!=0,0) && nrdesc::quiet_ask(e,hook);   // [Z3b] the image word, as in serve()
     if(!(zq && nrzx::bmode==1)){
     for(const MemoImage &ti:e.img){Image &im=*ti.second;
        const nr_dimg_in x{im.last_write,im.storage_write,im.opw,uint8_t(im.checked_submission==stats.submissions),uint8_t(im.uploaded),uint8_t(im.gpu_written),uint8_t(im.layout==VK_IMAGE_LAYOUT_GENERAL),uint8_t(hook)};
        if(nr_dimg_skip(&x)==NRD_I_SKIP){im.slot_use=slot_serial;continue;}
        ++nrd;Image &gi=get_image(ti.first,true);prepare_read(gi,c.command,-1);
        if(&gi!=&im)e.check_gen=0;}
     if(__builtin_expect(nrzx::bmode!=0,0))nrdesc::quiet_done(e,hook,zq,nrd);}}
    read_frag_only=false;if(tm.sp)ti=timing_clock();
    if(slot_serial!=s0){c.command=recording_slot().cmd;return out(NRZW_IMAGE);}   // a get_image ended the submission: as descriptors_stable, the draw is built again in the new slot (by the path without the table)
    e.mru=uint8_t(sk);   // serve(): the set of this tuple
    nrdesc::note_last(*pl,v->did,f);++pl->served;pl->fails=pl->misses=0;s.ubos+=dpn.nubo;s.img+=e.img.size();s.img_rd+=nrd;
    if(nbdesc::mode>0){nbdesc::cand=false;nbdesc::P.valid=false;}   // nrdesc::Scope after a served draw
    if(nrd && !pass_continues(c)){   // the read path ended the open pass: the descriptors stand, create_pass begins the pass, the lane decides
        Descriptors &d=acquire_descriptors();d.entry=pl->layout;d.set=sv->set;d.pipeline_layout=pl->layout->pipeline_layout;d.dynamic_offsets.assign(off,off+dpn.ndyn);
        if(d.buffers.capacity()<pl->reserve)d.buffers.reserve(pl->reserve);
        dp=&d;nrdesc::z_out=NRZD_NONE;++s.why[NRZW_PASS_LATE];return 3;}
    if(tm.sp)td=timing_clock();
    static Pass zp;zp=last_pass.pass;   // create_pass: the same Pass, the attachments' write marks (no heap: the image list keeps its capacity)
    for(auto *im:zp.images){im->prior_write=im->last_write;im->last_write=++write_seq;im->gpu_written=true;}++pass_fast_hits;
    DRAW_MARK(2);
    // draw(): NR-PRESKIN's plan of this draw, at its place (after create_pass, before the lane); every draw, as there (its counters)
    if(__builtin_expect(nrps::mode!=0,0)){nrps::decide(active_colors,false,prim,count,indexed,instances);ps_called=true;}
    if(psk){   // the lane resolves the variant with PRESKIN's bits and runs its tail; false = as any lane() that returned false
        Descriptors &d=acquire_descriptors();d.entry=pl->layout;d.set=sv->set;d.pipeline_layout=pl->layout->pipeline_layout;d.dynamic_offsets.assign(off,off+dpn.ndyn);
        if(d.buffers.capacity()<pl->reserve)d.buffers.reserve(pl->reserve);
        if(nrdraw::lane(c,d,zp,prim,first,count,indexed,instances,first_instance,pp?unsigned(nrdraw::F_PP):0u,draw_fast)){++s.psk_rec;return 1;}
        dp=&d;p=zp;++s.psk_old;return 2;}
    // lane(): the draw in hand, its counters, the pipeline of the table
    nrdraw::Hand &H=nrdraw::H;nrdraw::Stats &lz=a.z8?nrdraw::z8sz:nrdraw::sz;
    nrdraw::armed=false;H.var_ok=a.varw!=nullptr;H.have_ent=true;H.pred=false;H.qpat=false;H.t=*a.t;nrdraw::cur=nullptr;H.ui=false;nrdraw::S=&lz;++lz.lane;if(pp)++lz.pp_asked;
    state.restart_eff=re;if(a.varw)memcpy(H.var.w,a.varw,sizeof H.var.w);
    H.ent=pe;H.pipe=VkPipeline(pe->pipe);H.used=pe->used;H.inst=pe->inst;H.zero=pe->zero;memcpy(H.stride,pe->stride,sizeof H.stride);
    DRAW_MARK(3);if(tm.sp){tm.ta=timing_clock();++s.sp_n;s.sp_look+=tl-tm.t0;s.sp_desc+=td-tl;s.sp_pass+=tm.ta-td;s.sp_gate+=a.tg-tm.t0;s.sp_front+=a.tf-a.tg;s.sp_full+=a.tk-a.tf;s.sp_blk+=tb-tl;s.sp_img+=ti-tb;}
    const nrdraw::DSet ds{pl->layout->pipeline_layout,sv->set,uint32_t(dpn.ndyn),off};
    nrz5t::plan_hint=&dpn;   // [Z5] the template of this record names the uniform slot of each dynamic offset (cleared below)
    if(nrdraw::lane_tail(c,nullptr,ds,zp,first,count,indexed,instances,first_instance,pp?unsigned(nrdraw::F_PP):0u,false,1,active_colors,tm)){nrz5t::plan_hint=nullptr;++s.served;return 1;}
    nrz5t::plan_hint=nullptr;
    // the tail left the draw to the old tail: hand draw() what descriptors_stable and create_pass would have (as after a lane() that returned false)
    Descriptors &d=acquire_descriptors();d.entry=pl->layout;d.set=sv->set;d.pipeline_layout=pl->layout->pipeline_layout;d.dynamic_offsets.assign(off,off+dpn.ndyn);
    if(d.buffers.capacity()<pl->reserve)d.buffers.reserve(pl->reserve);
    dp=&d;p=zp;++s.why[NRZW_LATE];return 2;}
// nrdraw::lane_tail recorded an engine draw on the path without the table (its Descriptors record in hand). The draw's resolution is
// what that path used: NR-08's plan and entry (serve() answered: z_out), the lane's pipeline (nrdraw::H).
void served(){
    if(mode<=0)return;
    const bool have=nrdesc::z_out==NRZD_HIT || nrdesc::z_out==NRZD_BUILT;
    nrdesc::Plan *pl=have?nrdesc::plan_for():nullptr;const nrdraw::Hand &H=nrdraw::H;
    nrz_val now{};
    // [PRESKIN] a draw nrps planned was resolved by the lane under its own variant bits (40-63 of word 4): its NR-09 entry is not the table's to keep
    const bool psk=nrps::mode==1 && state.shader[0] && state.shader[0]->psk==1;const uint64_t w4=nrps::P.on?H.var.w[4]&((1ull<<40)-1):H.var.w[4];
    if(pl && pl->last>=0){now.plan=uint64_t(uintptr_t(pl));now.pent=nrps::P.on?0:uint64_t(uintptr_t(H.ent));now.did=pl->last;}
    if(P.valid){P.valid=false;   // mode 2
        const bool frag_ok=nrrp::frag_first==(tile_barrier_on && K.frag);now.frag=P.v.frag;if(psk)now.pent=P.v.pent;   // (a skinned draw: the lane looks its entry up itself)
        const bool same=pl && pl->last>=0 && nrz_val_same(&P.v,&now) && frag_ok && P.vs==H.var.w[0] && P.fs==H.var.w[1];
        const int c=nrz_classify(1,nrdesc::z_out,int(nrdesc::z_rd),pass_fast_hits!=P.pfh,0,same);
        if(c==NRZC_SAME)++s.same;else if(c==NRZC_LIVE)++s.live;
        else{++s.bad;red("the table's answer is not what the path without it did: plan %p / %p, NR-08 entry %d / %d, NR-09 entry %p / %p, "
            "fragment-only reads %d / %d, NR-08 %d (read path %u), pass fast path %d",(void*)uintptr_t(P.v.plan),(void*)pl,P.v.did,pl?pl->last:-1,(void*)uintptr_t(P.v.pent),(const void*)H.ent,
            int(tile_barrier_on && K.frag),int(nrrp::frag_first),nrdesc::z_out,nrdesc::z_rd,int(pass_fast_hits!=P.pfh));}
        return;}
    if(!K.ok)return;K.ok=false;   // no key of this draw in hand (not asked, a gate, or the table answered)
    if(!pl || pl!=K.pl || pl->last<0 || !pl->layout || !H.ent)return;
    // the key was taken before the path ran: it must be the key of what the path resolved (else a word of the key is not what the path reads)
    if(H.var.w[3]!=K.w[K.n-1] || H.var.w[5]!=K.w[K.n-2] || w4!=K.w[K.n-3] || H.var.w[2]!=uint64_t(uintptr_t(&pl->layout->definition)) ||
       nr_dtab_find(&nrdesc::tab,nr_dkey_hash64(K.w,K.ng),K.w,K.ng)!=pl->last){++s.odd;
        red("a draw's key taken before the path does not name what the path resolved (variant %#llx / %#llx, state ids %#llx / %#llx, pass %#llx / %#llx, entry %d)",
            (unsigned long long)K.w[K.n-3],(unsigned long long)w4,(unsigned long long)K.w[K.n-2],(unsigned long long)H.var.w[5],(unsigned long long)K.w[K.n-1],(unsigned long long)H.var.w[3],pl->last);return;}
    now.frag=uint8_t(K.frag);
    const uint64_t stamp[NRZ_STAMPS]={nrdesc::tab_gen,nrdraw::tab_gen(),nbpipe::epoch};
    nrz_put(&T,K.h,K.w,K.n,stamp,&now);++s.learned;
    if(__builtin_expect(kmode!=0,0)){   // [Z3K] the same resolution under the same key and stamps
        if(FP.valid){FP.valid=false;if(nrz_val_same(&FP.v,&now))++ks.same;else{++ks.bad;red("Z3K: the front table answered plan %p / NR-08 entry %d / NR-09 entry %p, the path without the tables resolved plan %p / %d / %p",
            (void*)uintptr_t(FP.v.plan),FP.v.did,(void*)uintptr_t(FP.v.pent),(void*)uintptr_t(now.plan),now.did,(void*)uintptr_t(now.pent));}}
        nrzk_row row[NRZ_IMG_MAX];front_rows(pl->p,row);
        if(nrzk_put(&F,stamp,K.w,K.n,pl->p.nimg,row,&now))++ks.stored;}}
}
