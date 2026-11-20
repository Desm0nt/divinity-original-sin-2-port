// [DOS2] NR-09: DM_NR_DRAW - nr_draw(), the short executor of an engine draw (docs/dos2/nr/NR-09.md, plan NATIVE_RENDERER_PLAN.md §2 S5).
// Decisions (variant words, offset folds, bind runs, the comparison) in dm/gfx/nr/nr_draw.h (pure C, tests/nr_draw_test.c). Included by
// xbg_game.cpp after the NB includes (it reads uiarea / nbpipe / nbchk); declared ahead of draw().
//
// The record. An engine draw inside the NB draw bracket (DM_NB_OPS 36 / 37) is one OP_X_NB_DRAW (binds + draw, NB-09 / NB-10). With
// DM_NR_DRAW the front end adds the engine's identity of the draw - NR-02's key id and texture-set id - as a 16-byte tail (NBD_F_NR,
// nvn_nb_draw.h nrd_tail): that record is OP_X_NR_DRAW. nbdraw::apply applies the binds as before and hands the tail over (nrdraw::cur)
// for the draw() it starts.
//
// The executor. draw() runs its entry (the skip rules, the pre-pass, the diagnostics), the descriptors (NR-08 serves them from an entry)
// and create_pass as before; right after create_pass ONE line asks nrdraw::lane(). For a draw that continues the open pass it does the
// rest in a few steps and draw() returns:
//   pipeline     NR-07's direct table: row = the record's key id, variant = (vertex / fragment shader, descriptor layout and render
//                pass definitions, primitive / samples / targets / restart, NB-13b's six exact state-group ids). A hit gives the
//                VkPipeline and the vertex binding list; no key is built or hashed (the groups are re-read only after a state record).
//   vertex/index the bound ranges through snapshot() (NR-06 static copies / zero-copy / the old caches), then folded: the index buffer
//                is bound at (offset mod index size) and drawn with firstIndex; the per-vertex streams move back by k vertices and the
//                draw's vertexOffset grows by k (not for a vertex shader that reads gl_VertexIndex). Meshes that share a VkBuffer then
//                bind the same (buffer, offset) and the bind cache drops the command.
//   commands     bind pipeline (when it changed), bind the descriptor set with its dynamic offsets (when set or offsets changed), one
//                vkCmdBindVertexBuffers for the streams that changed, vkCmdBindIndexBuffer (when it changed), the draw: <= 4 on a
//                typical draw, never more than the old tail. Viewport / scissor are computed only when the state changed.
// Everything else falls back PER DRAW: lane() returns false before it recorded anything and draw() goes on exactly as before (the
// old tail then teaches the table its pipeline). Reasons are counted (nr_draw.h NRW_*).
//
//   DM_NR_DRAW   0  default: nrdraw::cur stays null - draw() pays one load and a not-taken branch after create_pass and one before its
//                   last statement; nbdraw::apply one load. No table, no memory.
//                1  on.
//                2  compare: lane() computes pipeline, vertex / index binds (folded), viewport and scissor, records nothing; the old
//                   tail draws; at its end every value is compared with what it bound (same VkPipeline, same vertex bindings, same
//                   fetch addresses, same viewport / scissor). Red lines (first 16) + the summary every 10 s. The descriptor set and
//                   its offsets are the same object on both paths (NR-08 has its own DM_NR_DESC=2).
//                3  does not exist: the short path records the same commands as the old one, there is nothing to poison.
//   DM_NR_DRAW_FOLD  bit 0 index fold, bit 1 vertex fold (default 3 with DM_NR_DRAW=1 / 2; 0 = bind at the old offsets: A/B of the folds)
// Needs: DM_NB=1 with DM_NB_OPS bits 36 / 37 (the record), DM_NR=1|2 (the flush hook that gives the identity; DM_NR_KEY optional),
// DM_NB_PIPE=1|2 (the state-group ids) and DM_GFX_DRAW_FAST=1 + the pipeline memo (all defaults of the DOS2 build but DM_NR).
// Line: "[nr] draw: DM_NR_DRAW=... " every 10 s; red lines "【红】[nr] draw: ...".
// [DOS2] NR-30 (docs/dos2/nr/NR-30.md): a G-buffer draw that carries DM_DOS2_PREPASS's override (F_PP: depth func LEQUAL + write for one
// draw, prepass::Guard) takes the lane too. Its variant is the game's own state-group ids (re-read from prepass::live->saved, so the ids
// never describe the override) plus NR_DRAW_VAR_PP; the old tail it falls back to runs with the override, so done() learns exactly that
// pipeline and DM_NR_DRAW=2 compares it. Counted as "pre-pass override ... asked / served" in the line.
//
// [DOS2] NR-12 DM_NR_UI (docs/dos2/nr/NR-12.md): the UI's draws through the same lane. Iggy's GDraw backend calls NVN outside the engine's
// draw bracket, so its draws are plain draw records (no OP_X_NR_DRAW). With DM_NR_UI the lane line of draw() also asks ui_pick() when no
// record identity is in hand: a draw whose fragment shader is an Iggy program (nb_ui.h: iggy d99f1fdb / fill b5afd82f / blur 5e26ce73;
// the class is looked at once per shader, Shader::nrui) gets the synthetic identity ui_tail (key id 0 = row 0 of the table, site
// NRD_SITE_UI) and goes through lane() like an engine draw - same variant, table, folds, commands and fallbacks - plus:
//   uiarea       in a pass whose first draw is Iggy (NB-18b-6 DM_NB_UIAREA, default on) the full uiarea::pre runs, as the old draw() does
//                (the draw's rectangle joins the pass union that seeds the next frame's area; fills are counted); a pass begun with a
//                smaller area stays on the old path (a draw outside it restarts the pass) - nr_draw.h nr_ui_area
//   quads        Iggy's DrawArrays(QUADS) (no index buffer): the index list is the same for every draw (4q + 0 1 2 0 2 3), so ONE copy
//                per submission is bound (quad_pattern) instead of a copy per draw; the old path's expansion still runs before the lane
//   DM_NR_UI     0  default: ui_pick is never called (the lane line pays one load when no record identity is in hand); no table
//                   unless DM_NR_DRAW.
//                1  on (needs only the backend: DM_NB_PIPE + DM_GFX_DRAW_FAST=1, the DOS2 build's defaults; not DM_NR / DM_NR_DRAW).
//                2  compare: as DM_NR_DRAW=2 for the UI draws (nothing recorded by the lane; the old tail draws; pipeline, vertex
//                   bindings, fetch addresses, viewport, scissor compared, and a quad draw's index list against the pattern).
//                3  does not exist (red line, off).
//   Lines "[nr] ui: DM_NR_UI=..." every 10 s ("[nr] ui check: ..." at 2); red lines "【红】[nr] draw: ... (ui ...)".
namespace nrdraw{
int mode=0,fold=3,ui_mode=0;bool read_env=false,armed=false,ui_rec=false;   // ui_mode / ui_rec: NR-12 (ui_rec = a plain draw record's draw() is running)
const nrd_tail *cur=nullptr;
const nrd_tail ui_tail{0,0,0,NRD_VER,NRD_SITE_UI,0};   // NR-12: the identity of a plain Iggy draw (key id 0 = NRD_ROW_UI)
nrp_tab T{};bool tab_ok=false;uint64_t epoch=~0ull;
uint64_t tgen=1;   // [NR-ZERO Z3] +1 at every clear of the table made here; tab_gen() adds the clears nrp_tab_put makes when the table is full
inline uint64_t tab_gen(){return tgen+T.full;}   // the table's generation: its entries (addresses nrz3 keeps) are handed out again after a clear
int zl=0;   // [NR-ZERO Z3] the lane's answer for the draw in hand: 0 recorded, 1 left before its pipeline was known, 2 left after, 3 not asked (DM_NR_Z3=2 reads it)
// the draw in hand (lane -> done)
struct Hand{nrd_tail t{};nrp_var var{};const nrp_tent *ent=nullptr;bool var_ok=false,have_ent=false,pred=false,ui=false,qpat=false;
    VkPipeline pipe{};uint16_t used=0,inst=0;int zero=-1;uint32_t stride[16]{};
    VkBuffer vb[16]{};uint64_t vbo[16]{};uint32_t k=0;VkBuffer ib{};uint64_t ibo=0;uint32_t first_index=0,count=0;VkIndexType it=VK_INDEX_TYPE_MAX_ENUM;bool indexed=false;
    VkViewport vp{};VkRect2D sc{};} H;
// NR-12: the quad index pattern of this submission (stream arena of the recording slot: valid while slot_serial is the same)
struct QuadPat{uint64_t serial=0;Buffer b{};uint32_t n=0;} QP;
// viewport / scissor of the state as last computed (pure functions of these bytes), and the uiarea call they were last reported with
struct VpCache{bool ok=false;VkViewport st{},vp{};bool sc_ok=false;VkRect2D sc_st{},sc{};uint32_t w=0,h=0;
    VkCommandBuffer ua_cmd{};VkFramebuffer ua_fb{};uint64_t ua_pass=~0ull;VkViewport ua_vp{};VkRect2D ua_sc{};} V;
struct Stats{uint64_t records=0,plain=0,lane=0,served=0,why[NRW_N]{},why_ne=0,learned=0,relearn=0,cmds=0,c_pipe=0,c_set=0,c_vb=0,c_ib=0,c_vp=0,c_sc=0,folded_i=0,folded_v=0,vindex=0,
    ua_calls=0,ua_short=0,ns=0,timed=0,checks=0,bad=0,bad_pipe=0,bad_bind=0,bad_vb=0,bad_ib=0,bad_vp=0,bad_sc=0,not_compared=0,tab_clears=0,
    quads=0,quad_pats=0,quad_bytes=0,ua_full=0,fills=0,blurs=0,pp_asked=0,pp_served=0;
    uint64_t sp_n=0,sp_key=0,sp_vtx=0,sp_vp=0,sp_cmd=0,sp_tail=0;} s,su,sz,z8sz;   // sz: [NR-ZERO Z3] the draws nrz3 answered (the same tail, its own counters)   // [NR-ZERO] a served draw by part (1 in 128; not in `ns`)   // pp_*: NR-30 DM_DOS2_PREPASS override draws   // s: engine draws (NR-09), su: the UI lane (NR-12)
Stats *S=&s;   // the counters of the draw in hand: set by lane(), read by fail() / done() of the same draw
uint64_t next_ns=0,calls=0;unsigned told=0;
// DM_NR_DRAW=2: does the record's texture-set id (NR-02, the game thread's view of the 2 x 32 slots) name the NR-08 entry the backend
// found by generation / handles? (texture-set id, shader set) -> entry, direct mapped: "same" = the id could be the front key of that table.
struct TsSlot{uint32_t ts=0;const void *plan=nullptr;int32_t ent=-1;};TsSlot tsl[4096];uint64_t ts_same=0,ts_moved=0,ts_new=0;
void texset_note(){nrdesc::Plan *pl=nrdesc::plan_for();if(!pl || pl->p.why || pl->last<0)return;
    TsSlot &x=tsl[((H.t.texset*2654435761u)^uint32_t(uintptr_t(pl)>>6))>>20&4095];
    if(x.ts==H.t.texset && x.plan==pl){if(x.ent==pl->last)++ts_same;else{++ts_moved;x.ent=pl->last;}}else{++ts_new;x.ts=H.t.texset;x.plan=pl;x.ent=pl->last;}}
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
void red(const char *fmt,...){if(told>=16)return;++told;char b[640];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof b,fmt,ap);va_end(ap);
    xbg_logf("【红】[nr] draw: %s (frame %llu, key %u%s, vs=%s ps=%s)",b,(unsigned long long)stats.presents.load(),H.t.key,H.ui?" ui":"",state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-");}
void init(){read_env=true;
    const char *e=getenv("DM_NR_DRAW");int m=e && *e?atoi(e):0;
    if(m==3){xbg_logf("【红】[nr] DM_NR_DRAW=3 does not exist (the short path records the same commands, nothing to poison): off");m=0;}
    if(m<0 || m>2){xbg_logf("【红】[nr] DM_NR_DRAW=%d: only 0 / 1 / 2: off",m);m=0;}
    if(m && getenv("DM_GFX_SKIP_SH")){xbg_logf("【红】[nr] DM_NR_DRAW=%d: DM_GFX_SKIP_SH (a bisection aid of the old path) is set: off",m);m=0;}
    // [DOS2] NR-12 DM_NR_UI: the UI lane (its own mode, the same table)
    const char *u=getenv("DM_NR_UI");int um=u && *u?atoi(u):0;
    if(um==3){xbg_logf("【红】[nr] DM_NR_UI=3 does not exist (the UI lane records the same commands as the old tail, nothing to poison): off");um=0;}
    if(um<0 || um>2){xbg_logf("【红】[nr] DM_NR_UI=%d: only 0 / 1 / 2: off",um);um=0;}
    if(um && getenv("DM_GFX_SKIP_SH")){xbg_logf("【红】[nr] DM_NR_UI=%d: DM_GFX_SKIP_SH (a bisection aid of the old path) is set: off",um);um=0;}
    if((m || um) && nrp_tab_init(&T)){xbg_logf("【红】[nr] DM_NR_DRAW=%d DM_NR_UI=%d: no memory for the pipeline table: off",m,um);m=0;um=0;}
    tab_ok=m!=0 || um!=0;
    if(m || um){e=getenv("DM_NR_DRAW_FOLD");fold=e && *e?std::clamp(atoi(e),0,3):xbg_nr_draw_fold_default();}
    mode=m;nrbuf::place=m?fold:0;   // NR-06's static copies are placed for the folds (index copies in their own chunks, vertex copies on a multiple of the stride)
    ui_mode=um;   // NR-12: the UI's vertices / indices live in the constant ring and Iggy's own buffers, not in NR-06's static copies: no placement
    if(m)xbg_logf("[nr] DM_NR_DRAW=%d (NR-09: %s): OP_X_NR_DRAW = OP_X_NB_DRAW + the engine's key id / texture-set id; nr_draw after create_pass - pipeline from the direct table "
        "[key id][variant], vertex / index binds folded (DM_NR_DRAW_FOLD=%d: index %s, vertex %s), <= 4 vkCmd* a typical draw; per-draw fallback to draw(); needs DM_NB_PIPE (now %d) "
        "and DM_GFX_DRAW_FAST=1 (now %d)",m,m==2?"compare - the old path draws, every value of the short path is checked against it":"on",fold,fold&1?"on":"off",fold&2?"on":"off",
        nbpipe::mode,draw_fast_mode());
    if(um)xbg_logf("[nr] DM_NR_UI=%d (NR-12: %s): plain draw records whose fragment shader is an Iggy program (iggy / fill / blur) take nr_draw with a synthetic identity "
        "(row 0 of the direct table, the variant decides the pipeline); Iggy passes of DM_NB_UIAREA get the full uiarea step; QUADS draws bind one index pattern a "
        "submission; folds DM_NR_DRAW_FOLD=%d; per-draw fallback to draw(); needs DM_NB_PIPE (now %d) and DM_GFX_DRAW_FAST=1 (now %d), not DM_NR",um,
        um==2?"compare - the old path draws, every value of the UI lane is checked against it":"on",fold,nbpipe::mode,draw_fast_mode());}
void reset(){if(!read_env)init();}   // the state reset (init): DM_NR_DRAW once
void release(){nrz3::release();nrzx::release();nrz5::release();if(tab_ok){nrp_tab_free(&T);tab_ok=false;}++tgen;mode=0;ui_mode=0;cur=nullptr;armed=false;QP=QuadPat{};}
// From nbdraw::apply (only while mode != 0): the tail of the record whose draw starts now (null: a plain OP_X_NB_DRAW).
void record(const nrd_tail *t){++s.records;cur=t;if(!t)++s.plain;}
// NR-12: the fragment shader is an Iggy program (looked at once per shader). true = this draw goes to lane() with the UI identity.
inline bool ui_fs(Shader *fs){if(!fs)return false;
    if(fs->nrui<0){const int c=nb_ui_shader_class(fs->key.c_str());fs->nrui=int8_t(c==NB_UISH_IGGY?1:c==NB_UISH_IGGY_FILL?2:c==NB_UISH_IGGY_BLUR?3:0);}
    return fs->nrui>0;}
// From draw()'s lane line, only while ui_mode != 0 and no record identity is in hand.
bool ui_pick(){ui_rec=false;if(!ui_fs(state.shader[4]))return false;++su.records;if(state.shader[4]->nrui==2)++su.fills;else if(state.shader[4]->nrui==3)++su.blurs;cur=&ui_tail;return true;}
void ui_tick(){   // NR-12, from tick()
    std::string w;for(int i=1;i<NRW_N;i++)if(su.why[i]){char b[96];snprintf(b,sizeof b,"%s%s %llu",w.empty()?"":", ",nr_draw_why(i),(unsigned long long)su.why[i]);w+=b;}
    const double sv=su.served?double(su.served):1.;
    xbg_logf("[nr] ui: DM_NR_UI=%d; %llu Iggy draws (plain records; fill %llu, blur %llu), %llu asked nr_draw, %s %llu (%.1f%%); old path: %s; table row 0: learned %llu (relearned %llu); "
        "vkCmd* a served draw %.2f (pipeline %.2f, set %.2f, vertex %.2f, index %.2f, viewport %.3f, scissor %.3f, + the draw); QUADS draws %llu on the submission's index pattern "
        "(%llu patterns made, %.1f KB); folds: index %llu, vertex %llu (%llu not folded); uiarea: full step in Iggy passes %llu, short %llu (other passes), calls %llu; "
        "%.0f ns a served draw after create_pass (1 in 64)",
        ui_mode,(unsigned long long)su.records,(unsigned long long)su.fills,(unsigned long long)su.blurs,(unsigned long long)su.lane,ui_mode==2?"would serve":"served",
        (unsigned long long)su.served,su.records?100.*double(su.served)/double(su.records):0.,w.empty()?"none":w.c_str(),(unsigned long long)su.learned,(unsigned long long)su.relearn,
        ui_mode==2?0.:double(su.cmds)/sv,double(su.c_pipe)/sv,double(su.c_set)/sv,double(su.c_vb)/sv,double(su.c_ib)/sv,double(su.c_vp)/sv,double(su.c_sc)/sv,
        (unsigned long long)su.quads,(unsigned long long)su.quad_pats,su.quad_bytes/1024.,(unsigned long long)su.folded_i,(unsigned long long)su.folded_v,(unsigned long long)su.vindex,
        (unsigned long long)su.ua_full,(unsigned long long)su.ua_short,(unsigned long long)su.ua_calls,su.timed?double(su.ns)/double(su.timed):0.);
    if(ui_mode==2 || su.checks || su.bad)
        xbg_logf("[nr] ui check: %llu Iggy draws compared with the old tail, bad %llu (pipeline %llu, vertex bindings %llu, vertex fetch %llu, index fetch / quad list %llu, viewport %llu, "
            "scissor %llu); predicted but the old tail did not finish the draw %llu",(unsigned long long)su.checks,(unsigned long long)su.bad,(unsigned long long)su.bad_pipe,
            (unsigned long long)su.bad_bind,(unsigned long long)su.bad_vb,(unsigned long long)su.bad_ib,(unsigned long long)su.bad_vp,(unsigned long long)su.bad_sc,(unsigned long long)su.not_compared);
    su=Stats{};}
void tick(){   // every submission (nbdraw::tick)
    nrsort::tick();   // [NR-21] its line
    if(__builtin_expect(nrz3::mode>0,0))nrz3::tick();   // [DOS2] NR-ZERO Z3: its lines
    if(__builtin_expect((nrzx::bmode|nrzx::cmode|nrzx::dmode)!=0,0))nrzx::tick();   // [DOS2] NR-ZERO Z3b / Z3c / Z3d: their line
    if(__builtin_expect(nrz5::mode!=0,0))nrz5::tick();   // [DOS2] NR-ZERO Z5: its line
    if(__builtin_expect(nrz5t::gmode!=0,0)){if(nrz5t::z8)nrz8t::tick();else nrz5t::tick();}   // [DOS2] NR-ZERO Z5, the game thread's half: its lines ([Z8] DM_NR_Z8: the submit walk's)
    if(__builtin_expect(nrmdi::mode>0,0))nrmdi::tick();   // [DOS2] NR-MDI: its line
    if(__builtin_expect(nrpo::mode>0,0))nrpo::tick();   // [DOS2] NR-POSONLY: its line
    if(__builtin_expect(nrps::mode>0,0))nrps::tick();   // [DOS2] NR-PRESKIN: its line
    if((!mode || !s.records) && (!ui_mode || !su.records))return;const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    if(ui_mode)ui_tick();
    if(!mode){T.hits=T.first_hits=T.misses=0;return;}
    if(!s.records)return;   // the table counters below then cover the UI draws of this window too
    std::string w;for(int i=1;i<NRW_N;i++)if(s.why[i]){char b[96];snprintf(b,sizeof b,"%s%s %llu",w.empty()?"":", ",nr_draw_why(i),(unsigned long long)s.why[i]);w+=b;}
    const double sv=s.served?double(s.served):1.;
    xbg_logf("[nr] draw: DM_NR_DRAW=%d; %llu draw records (%llu plain OP_X_NB_DRAW without an identity), %llu asked nr_draw, %s %llu (%.1f%% of the records; key not eligible for the "
        "engine %llu of them); old path: %s; table: %u entries, learned %llu (relearned %llu), hits %llu (first of its row %llu), misses %llu, cleared %llu; "
        "vkCmd* a served draw %.2f (pipeline %.2f, set %.2f, vertex %.2f, index %.2f, viewport %.3f, scissor %.3f, + the draw); folds: index %llu, vertex %llu (vertex shader reads "
        "gl_VertexIndex / primitive restart: %llu not folded); uiarea full calls %llu, short %llu; %.0f ns a served draw after create_pass (1 in 64); "
        "pre-pass override (DM_DOS2_PREPASS LEQUAL + write variant): asked %llu, %s %llu",
        mode,(unsigned long long)s.records,(unsigned long long)s.plain,(unsigned long long)s.lane,mode==2?"would serve":"served",(unsigned long long)s.served,
        100.*double(s.served)/double(s.records),(unsigned long long)s.why_ne,w.empty()?"none":w.c_str(),T.n,(unsigned long long)s.learned,(unsigned long long)s.relearn,
        (unsigned long long)T.hits,(unsigned long long)T.first_hits,(unsigned long long)T.misses,(unsigned long long)s.tab_clears,
        mode==2?0.:double(s.cmds)/sv,double(s.c_pipe)/sv,double(s.c_set)/sv,double(s.c_vb)/sv,double(s.c_ib)/sv,double(s.c_vp)/sv,double(s.c_sc)/sv,
        (unsigned long long)s.folded_i,(unsigned long long)s.folded_v,(unsigned long long)s.vindex,(unsigned long long)s.ua_calls,(unsigned long long)s.ua_short,
        s.timed?double(s.ns)/double(s.timed):0.,(unsigned long long)s.pp_asked,mode==2?"would serve":"served",(unsigned long long)s.pp_served);
    if(mode==2 || s.checks || s.bad)
        xbg_logf("[nr] draw check: %llu draws compared with the old tail, bad %llu (pipeline %llu, vertex bindings %llu, vertex fetch %llu, index fetch %llu, viewport %llu, "
            "scissor %llu); predicted but the old tail did not finish the draw %llu; the record's texture-set id against the NR-08 entry (needs DM_NR_DESC): same entry %llu, "
            "another entry %llu, first sight %llu",(unsigned long long)s.checks,(unsigned long long)s.bad,(unsigned long long)s.bad_pipe,
            (unsigned long long)s.bad_bind,(unsigned long long)s.bad_vb,(unsigned long long)s.bad_ib,(unsigned long long)s.bad_vp,(unsigned long long)s.bad_sc,(unsigned long long)s.not_compared,
            (unsigned long long)ts_same,(unsigned long long)ts_moved,(unsigned long long)ts_new);
    if(s.sp_n){const double n=double(s.sp_n);   // [NR-ZERO] where the lane's time goes (the split's clock reads are inside the parts)
        xbg_logf("[nr] draw by part (NR-ZERO, %llu served draws split, 1 in 128): %.0f ns after create_pass = variant, groups and table %.0f + vertex / index views %.0f + viewport and checks %.0f + bookkeeping and vkCmd* %.0f + finish %.0f",
            (unsigned long long)s.sp_n,(s.sp_key+s.sp_vtx+s.sp_vp+s.sp_cmd+s.sp_tail)/n,s.sp_key/n,s.sp_vtx/n,s.sp_vp/n,s.sp_cmd/n,s.sp_tail/n);}
    if(nrz3::span_n){   // [NR-ZERO Z3] the number that compares DM_NR_Z3=0 and =1: descriptors + pass + lane of a recorded engine draw, whichever path answered
        xbg_logf("[nr] draw span (NR-ZERO, %llu recorded engine draws timed, 1 in 64): %.0f ns from the command buffer to the end of the draw (descriptors, pass, lane; DM_NR_Z3=%d)",
            (unsigned long long)nrz3::span_n,double(nrz3::span_ns)/double(nrz3::span_n),nrz3::mode<0?0:nrz3::mode);nrz3::span_ns=nrz3::span_n=0;}
    s=Stats{};T.hits=T.first_hits=T.misses=0;ts_same=ts_moved=ts_new=0;}
// The exact state-group ids of NB-13b for the state as it is (re-read only after a state record). false: no groups (DM_NB_PIPE off).
inline bool groups(){nbp_groups *G=nbpipe::G;if(!G)return false;
    if(G->dirty && nbp_refresh(G,reinterpret_cast<const uint8_t*>(&state))){nbp_memo_clear(nbpipe::M);nbpipe::prev=nullptr;++nbpipe::s.clears_ids;++nbpipe::epoch;}   // ids restarted: as nbpipe::look
    return G->valid==(1u<<NBP_GROUPS)-1;}
// [DOS2] NR-30: the groups for a draw that carries DM_DOS2_PREPASS's override: re-read (when dirty) from the game's own depth-stencil
// state, never from the override, so the ids stay right for the draws after it (nbpipe::look does not re-read under the override at all).
inline bool groups_pp(){if(!prepass::live)return false;const ds_st cur=state.ds;state.ds=prepass::live->saved;const bool ok=groups();state.ds=cur;return ok;}
inline bool fail(int w){++S->why[w];return false;}
// The viewport / scissor draw() would set for the state now. false: a viewport the old path handles itself (skip, log, device limits).
inline bool viewport_of(const Pass &p,VkViewport &vp,VkRect2D &sc){
    if(!V.ok || memcmp(&V.st,&state.viewport,sizeof V.st)){
        const VkViewport &sv=state.viewport;VkViewport v{};
        if(sv.minDepth>sv.maxDepth)return false;   // the old path logs the reversed range once
        if(xbg_nvn_viewport(&v,sv.x,sv.y,sv.width,sv.height,sv.minDepth,sv.maxDepth)!=0)return false;   // the old path skips the draw
        if(taafix::viewport_ll()){v.y+=v.height;v.height=-v.height;}
        const auto &l=properties.limits;
        if(!(v.width<=l.maxViewportDimensions[0] && std::fabs(v.height)<=l.maxViewportDimensions[1] && v.x>=l.viewportBoundsRange[0] && v.x+v.width<=l.viewportBoundsRange[1] &&
             v.y>=l.viewportBoundsRange[0] && v.y<=l.viewportBoundsRange[1] && v.y+v.height>=l.viewportBoundsRange[0]))return false;   // the old path stops on it
        V.st=sv;V.vp=v;V.ok=true;}
    vp=V.vp;
    if(!V.sc_ok || V.w!=p.width || V.h!=p.height || memcmp(&V.sc_st,&state.scissor,sizeof V.sc_st)){V.sc=scissor(p);V.sc_st=state.scissor;V.w=p.width;V.h=p.height;V.sc_ok=true;}
    sc=V.sc;return true;}
// state.restart_eff of a draw (a key field the old fast block sets too): primitive restart is on, the draw is indexed, the topology is a
// strip / fan and the restart index is the type's all-ones value.
inline uint32_t restart_eff_of(int prim,const p_draw_idx *indexed){const VkPrimitiveTopology t=topology(prim);uint32_t re=0;
    if(state.restart.on && indexed && (t==VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || t==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || t==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN || t==VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || t==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY))
        re=state.restart.index==(indexed->itype==0?0xffu:indexed->itype==1?0xffffu:0xffffffffu)?1u:0u;
    return re;}
// [NR-ZERO Z3] The lane in two parts. lane() resolves the draw (gates, variant, the table: H holds the pipeline and its vertex bindings);
// lane_tail() does everything after that - the pass check, the vertex / index views, viewport / scissor, the bookkeeping and the
// commands. nrz3 (xbg_nr_z3.inc) fills H from its own table and calls the SAME tail: a draw it answers records what lane() would.
//   dr   the draw's Descriptors record (null: nrz3 - no record was taken; DM_NR_SORT / DM_NR_MDI, which need one, are off then)
//   ds   the set to bind: pipeline layout, set, dynamic offsets
struct Tm{bool timed=false,sp=false;uint64_t t0=0,ta=0;};   // the 1-in-64 clock of the draw in hand (sp: timed by part)
struct DSet{VkPipelineLayout layout{};VkDescriptorSet set{};uint32_t no=0;const uint32_t *off=nullptr;};
bool lane_tail(Commands &c,Descriptors *dr,const DSet &ds,Pass &p,int first,int count,const p_draw_idx *indexed,uint32_t instances,uint32_t first_instance,unsigned flags,bool ui,int lm,uint32_t active_colors,const Tm &tm);
// After create_pass. true = the draw is recorded (draw() returns); false = nothing was recorded, draw() goes on.
bool lane(Commands &c,Descriptors &d,Pass &p,int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances,uint32_t first_instance,unsigned flags,int draw_fast){NRCPU_SCOPE(NrLane);   // [DOS2] NR-CPU
    if(H.pred)++S->not_compared;   // the previous draw's prediction: its old tail returned before done()
    armed=false;H.var_ok=false;H.have_ent=false;H.pred=false;H.qpat=false;H.t=*cur;cur=nullptr;zl=1;
    const bool ui=H.t.site==NRD_SITE_UI && H.t.key==NRD_ROW_UI;H.ui=ui;S=ui?&su:&s;const int lm=ui?ui_mode:mode;++S->lane;   // NR-12: the UI lane has its own mode and counters
    if(lm==2 && nrdesc::mode>0 && H.t.texset)texset_note();
    if((!H.t.key && !ui) || H.t.key>=NRP_TAB_ROWS)return fail(NRW_NO_KEY);
    if(capture.active || pipe_stats || vtxstats::enabled() || crumbs::active || pgraph::armed || !bind_cache_enabled || ab_off("nrdraw"))return fail(NRW_GATE);
    const bool pp=(flags&F_PP)!=0;if(pp)++S->pp_asked;   // [DOS2] NR-30: DM_DOS2_PREPASS's LEQUAL + write variant
    if(draw_fast!=1 || !pipe_memo || !pipeline_fast_enabled || ab_off("pipefast") || ab_off("pipememo") || !(pp?groups_pp():groups()))return fail(NRW_NO_GROUPS);
    if(!state.shader[0] || state.shader[1] || state.shader[2] || state.shader[3])return fail(NRW_STAGES);
    if((flags&~F_PP) && !(ui && flags==(F_QUAD|F_QUADP)))return fail(NRW_SPECIAL);   // NR-12: the UI's DrawArrays(QUADS) take the submission's index pattern
    if(pp && ui)return fail(NRW_SPECIAL);
    if((flags&F_QUAD) && !nr_quad_size(QP.serial==slot_serial?QP.n:0,uint32_t(count)))return fail(NRW_SPECIAL);
    if(!d.entry || !d.entry->definition.valid || !p.definition || !p.definition->valid)return fail(NRW_DEF);
    if(epoch!=nbpipe::epoch){if(T.n){nrp_tab_clear(&T);++s.tab_clears;}++tgen;epoch=nbpipe::epoch;}   // ids restarted / pipelines, layouts or render passes were destroyed
    Tm tm;tm.timed=!(++calls&63);tm.t0=tm.timed?timing_clock():0;tm.sp=tm.timed && (calls>>6&1);   // [NR-ZERO] sp: timed by part
    // the variant (state.restart_eff: a key field the old fast block sets at this point too)
    uint32_t active_colors=0;for(int i=0;i<state.rt.n;i++)if(state.rt.color[i])active_colors|=1u<<i;
    state.restart_eff=restart_eff_of(prim,indexed);
    {nr_draw_var_t v;nr_draw_var(&v,uint64_t(uintptr_t(state.shader[0])),uint64_t(uintptr_t(state.shader[4])),uint64_t(uintptr_t(&d.entry->definition)),uint64_t(uintptr_t(p.definition)),
        unsigned(prim),unsigned(p.samples),unsigned(state.rt.n),active_colors,state.rt.depth!=nullptr,state.rt.depth?unsigned(state.rt.depth->d.format):0u,state.restart_eff,nbpipe::G->gid);
     if(pp)v.w[4]|=NR_DRAW_VAR_PP;
     if(__builtin_expect(nrpo::P.on,0))v.w[4]|=NR_DRAW_VAR_PO;
     if(__builtin_expect(nrps::P.on,0))v.w[4]|=nrps::lane_bits();   // [DOS2] NR-PRESKIN: the consumer pipeline of this layout / record binding (bits 40-63)   // [DOS2] NR-POSONLY: its own (one-binding) pipeline   // [DOS2] NR-30: the override's own variant (done() learns it from the old tail, which ran with the override)
     memcpy(H.var.w,v.w,sizeof H.var.w);}
    H.var_ok=true;armed=true;   // done(): the old tail teaches the table (and, mode 2, is compared)
    const nrp_tent *e=nrp_tab_find(&T,H.t.key,&H.var);
    if(!e)return fail(NRW_MISS);
    H.have_ent=true;H.ent=e;if(lm==1)armed=false;zl=2;
    H.pipe=VkPipeline(e->pipe);H.used=e->used;H.inst=e->inst;H.zero=e->zero;memcpy(H.stride,e->stride,sizeof H.stride);
    DRAW_MARK(3);if(tm.sp)tm.ta=timing_clock();
    const DSet ds{d.pipeline_layout,d.set,uint32_t(d.dynamic_offsets.size()),d.dynamic_offsets.data()};
    return lane_tail(c,&d,ds,p,first,count,indexed,instances,first_instance,flags,ui,lm,active_colors,tm);}
bool lane_tail(Commands &c,Descriptors *dr,const DSet &ds,Pass &p,int first,int count,const p_draw_idx *indexed,uint32_t instances,uint32_t first_instance,unsigned flags,bool ui,int lm,uint32_t active_colors,const Tm &tm){
    const bool timed=tm.timed,sp=tm.sp,pp=(flags&F_PP)!=0,qpat=(flags&F_QUAD)!=0;const uint64_t t0=tm.t0,ta=tm.ta;uint64_t tb=0,tc=0,td=0;
    // the open pass continues, and nothing the old tail does per draw besides its binds is live
    if(open_pass.cmd!=c.command || open_pass.pass!=p.pass || open_pass.framebuffer!=p.framebuffer || lazy_pending)return fail(NRW_NEW_PASS);
    const int ua=nr_ui_area(uiarea::mode!=0,uiarea::continues(c.command),uiarea::cur.rec.cls==NB_UA_CLS_OTHER,uiarea::cur.rec.reduced!=0,ui);   // NR-12: an Iggy draw in an Iggy pass gets the full step
    if(ua==NRUA_OLD)return fail(NRW_UIAREA);
    if(state.count_samples && !no_occlusion_queries && !ab_off("occ"))return fail(NRW_COUNTING);
    if(dr && dr->needs_writeback())return fail(NRW_WRITEBACK);
    // vertex streams and indices: the ranges the old tail snapshots, folded
    H.indexed=indexed!=nullptr;
    uint64_t off[16]={};unsigned z8vk=0;   // [NR-ZERO Z8 views] the number of the binding among the draw's ranges
    for(uint32_t m=H.used;m;m&=m-1){const uint32_t i=uint32_t(__builtin_ctz(m));
        if(int(i)==H.zero){const uint32_t zero[4]={0};const Buffer z=stream_buffer(sizeof(zero),zero,sizeof(zero));H.vb[i]=z.buffer;off[i]=z.offset;continue;}
        if(__builtin_expect(nrpo::P.on,0) && i==nrpo::P.k){H.vb[i]=nrpo::P.view.buffer;off[i]=nrpo::P.view.offset;continue;}   // [DOS2] NR-POSONLY: the mesh's copy
        if(__builtin_expect(nrps::P.on,0) && i==nrps::P.k){H.vb[i]=nrps::P.view.buffer;off[i]=nrps::P.view.offset;continue;}   // [DOS2] NR-PRESKIN: the record
        const NVNbufferRange r=state.vb[i];if(!(r.address && r.size))return fail(NRW_VERTEX);
        Buffer b;if(__builtin_expect(nrzx::cmode!=0,0))b=nrzx::view(r.address,r.size,1,H.stride[i]);   // [NR-ZERO Z3c] the remembered view of the range
        else if(__builtin_expect(nrz8t::vmode!=0,0))b=nrz8t::view(r.address,r.size,1,H.stride[i],z8vk++);   // [NR-ZERO Z8 views] the entry of the range's place in its submission
        else{snap_site=1;nrbuf::stride_hint=H.stride[i];b=snapshot(r.address,r.size);nrbuf::stride_hint=0;snap_site=0;}
        H.vb[i]=b.buffer;off[i]=b.offset;}
    int64_t base=indexed?int64_t(indexed->base_vertex):int64_t(first);
    // not folded: a vertex shader that reads gl_VertexIndex / gl_BaseVertex (the fold would change it), and a draw with primitive restart
    // (KB mobile-gpu-field-notes: old Mali - Midgard < r22, Bifrost < r10 - breaks restart with 16-bit indices and vertexOffset != 0)
    const bool vfold=(fold&2) && !state.shader[0]->vindex && !state.restart_eff;if((fold&2) && !vfold)++S->vindex;
    {uint32_t st[16];memcpy(st,H.stride,sizeof st);if(H.zero>=0)st[H.zero]=0;H.k=nr_fold_vertex(H.used,H.inst,st,off,base,vfold,H.vbo);}
    H.count=uint32_t(count);
    if(indexed && qpat){   // NR-12: a QUADS draw expanded by the old preamble (u32 list 4q + 0 1 2 0 2 3): the submission's one copy of that list
        if(indexed->itype!=2 || indexed->instances<=0 || indexed->base_instance<0 || !indexed->ibuf)return fail(NRW_VERTEX);
        if(QP.serial!=slot_serial || QP.n<uint32_t(count)){const uint32_t n=nr_quad_size(QP.serial==slot_serial?QP.n:0,uint32_t(count));
            QP.b=recording_slot().stream.alloc(size_t(n)*4);nr_quad_pattern(static_cast<uint32_t*>(QP.b.mapped),n);QP.b.flush();QP.n=n;QP.serial=slot_serial;++S->quad_pats;S->quad_bytes+=uint64_t(n)*4;}
        H.qpat=true;H.ib=QP.b.buffer;H.ibo=QP.b.offset;H.first_index=0;H.it=VK_INDEX_TYPE_UINT32;}
    else if(indexed){
        if(indexed->itype<1 || indexed->itype>2 || indexed->instances<=0 || indexed->base_instance<0 || !indexed->ibuf)return fail(NRW_VERTEX);
        const size_t bytes=size_t(count)*(1u<<indexed->itype);
        Buffer b;if(__builtin_expect(nrzx::cmode!=0,0))b=nrzx::view(uint64_t(indexed->ibuf),bytes,2,0);   // [NR-ZERO Z3c]
        else if(__builtin_expect(nrz8t::vmode!=0,0))b=nrz8t::view(uint64_t(indexed->ibuf),bytes,2,0,3);   // [NR-ZERO Z8 views]
        else{snap_site=2;b=snapshot(uint64_t(indexed->ibuf),bytes);snap_site=0;}
        H.ib=b.buffer;H.it=indexed->itype==1?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;nr_fold_index(b.offset,unsigned(indexed->itype),fold&1,&H.ibo,&H.first_index);}
    DRAW_MARK(4);if(sp)tb=timing_clock();
    if(!viewport_of(p,H.vp,H.sc))return fail(NRW_VIEWPORT);
    // a snapshot above may have ended the pass (a pending GPU result landing first): the old tail checks the open pass after its snapshots too
    if(open_pass.cmd!=c.command || open_pass.pass!=p.pass || open_pass.framebuffer!=p.framebuffer || lazy_pending)return fail(NRW_NEW_PASS);
    if(ua==NRUA_FULL && !(uiarea::continues(c.command) && !uiarea::cur.rec.reduced))return fail(NRW_UIAREA);   // NR-12: still the pass uiarea tracks, still not reduced
    ++S->served;if(pp)++S->pp_served;if(H.t.why)++S->why_ne;if(H.k)++S->folded_v;if(indexed && H.first_index)++S->folded_i;if(H.qpat)++S->quads;
    if(lm==2){H.pred=true;armed=true;return false;}   // the old tail draws; done() compares
    if(sp)tc=timing_clock();
    // ---- from here the draw is recorded: the old tail's per-draw bookkeeping, then the commands ----
    if(ua==NRUA_FULL){uiarea::pre(p,c.command,true);++S->ua_full;}   // NR-12: an Iggy draw in an Iggy pass - as the old draw() (not reduced: no restart)
    else if(ua==NRUA_SHORT){nb_ua_pass &q=uiarea::cur.rec;   // an OTHER-class pass: its draw count / DS use; the rectangle only when the viewport / scissor state moved
        if(V.ua_cmd!=c.command || V.ua_fb!=p.framebuffer || V.ua_pass!=perf.passes || memcmp(&V.ua_vp,&state.viewport,sizeof V.ua_vp) || memcmp(&V.ua_sc,&state.scissor,sizeof V.ua_sc)){
            uiarea::pre(p,c.command,true);V.ua_cmd=c.command;V.ua_fb=p.framebuffer;V.ua_pass=perf.passes;V.ua_vp=state.viewport;V.ua_sc=state.scissor;++S->ua_calls;}
        else{++q.draws;if(state.rt.depth && uiarea::uses_ds())q.ds_used=1;++S->ua_short;}}
    if(state.ds.dt || state.ds.st || state.bounds.enable)open_pass.depth_used=true;
    if(state.rt.depth && (state.ds.dw || state.ds.st))open_pass.depth_written=true;if(active_colors)open_pass.color_targets=true;
    if(pgm::mode)pgm::note_draw(c.command,active_colors,state.color.blend_en,state.mask.mask,state.rt.depth!=nullptr,state.ds.dt,state.ds.dw,state.ds.st,state.shader[4]?state.shader[4]->key.c_str():nullptr);   // [DOS2] NR-GMEM
    // [NR-21] DM_NR_SORT: an order-free draw waits in its segment (nrsort records its commands later, in the segment's order); any other
    // draw records the waiting ones first, before it reads the bind cache
    const bool queued=__builtin_expect(nrsort::mode!=0,0) && dr && nrsort::take(c,*dr,p,count,indexed,instances,first,first_instance);
    if(!queued){
    if(nrsort::pend.load(std::memory_order_relaxed) && !nrz5t::held)nrsort::flush_draw();   // (held: DM_NR_Z5=1 keeps a chunk for this command buffer - not a segment of nrsort)
    BindCache &bc=bind_cache;if(bc.cmd!=c.command){bc=BindCache{};bc.cmd=c.command;}
    // [DOS2] NR-ZERO Z5 DM_NR_Z5=1 (xbg_nr_z5g.inc): the game thread wrote this draw's packets from the same values and the bind cache holds what its run assumed - the command section is not run
    const bool z5cond=__builtin_expect(nrz5t::gmode!=0,0) && condr::gpu_would();const uint32_t z5i=indexed?uint32_t(indexed->instances):instances,z5f=indexed?H.first_index:uint32_t(first)+H.k,z5b=indexed?uint32_t(indexed->base_instance):first_instance;const int32_t z5v=indexed?int32_t(indexed->base_vertex)+int32_t(H.k):0;
    if(__builtin_expect(nrz5t::gmode!=0,0) && nrz5t::tail(c.command,bc,ui,z5cond,H.pipe,H.used,H.vb,H.vbo,H.ib,H.ibo,H.it,H.vp,H.sc,ds.layout,ds.set,ds.no,ds.off,indexed!=nullptr,uint32_t(count),z5i,z5f,z5v,z5b)){
        ++pipe_memo_hits;++pipe_draw_fast_hits;DRAW_MARK(5);pass_prof_draw(c.command);}
    else{
    if(__builtin_expect(nrz5::mode!=0,0))nrz5::draw_begin(c.command,bc,ui);   // [DOS2] NR-ZERO Z5 DM_NR_Z5=2: the run this draw belongs to, where its packets start
    unsigned n=1;
    if(bc.pipe!=H.pipe){vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,H.pipe);bc.pipe=H.pipe;++n;++S->c_pipe;}else ++binds_skipped;
    ++pipe_memo_hits;++pipe_draw_fast_hits;
    {const uint32_t no=ds.no;
     if(__builtin_expect(nrmdi::mode==3,0) && nrmdi::probe_keep(bc.layout==ds.layout && bc.set==ds.set && bc.noff==no && no && no<=32 && memcmp(bc.off,ds.off,no*4)!=0,active_colors==0 && state.rt.depth!=nullptr))++binds_skipped;   // [DOS2] NR-MDI =3 PROBE (breaks the picture): an offset-only set rebind of a depth-only draw is not made
     else if(bc.layout!=ds.layout || bc.set!=ds.set || bc.noff!=no || no>32 || (no && memcmp(bc.off,ds.off,no*4))){
        ++set_binds;set_bind_offsets+=no;
        if(bc.layout==ds.layout && bc.set==ds.set){++set_binds_offsets_only;
            if(bc.noff==no && no<=32){unsigned changed=0;for(uint32_t k=0;k<no;k++)if(bc.off[k]!=ds.off[k]){++changed;++offset_changed_at[k];}++offset_changes[changed<9?changed:9];}}
        vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,ds.layout,0,1,&ds.set,no,ds.off);++n;++S->c_set;
        if(no<=32){bc.layout=ds.layout;bc.set=ds.set;bc.noff=no;if(no)memcpy(bc.off,ds.off,no*4);}else bc.noff=UINT32_MAX;}
     else ++binds_skipped;}
    if(!bc.has_vp || memcmp(&bc.vp,&H.vp,sizeof H.vp)){vkCmdSetViewport(c.command,0,1,&H.vp);bc.vp=H.vp;bc.has_vp=true;++n;++S->c_vp;}else ++binds_skipped;
    if(!bc.has_sc || memcmp(&bc.sc,&H.sc,sizeof H.sc)){vkCmdSetScissor(c.command,0,1,&H.sc);bc.sc=H.sc;bc.has_sc=true;++n;++S->c_sc;}else ++binds_skipped;
    {unsigned changed=0;for(uint32_t m=H.used;m;m&=m-1){const uint32_t i=uint32_t(__builtin_ctz(m));if(bc.vb[i]!=H.vb[i] || bc.vbo[i]!=H.vbo[i])changed|=1u<<i;else ++binds_skipped;}
     if(changed){uint8_t f[8],k[8];const unsigned runs=nr_vb_runs(changed,H.used,f,k);
        for(unsigned r=0;r<runs;r++){VkDeviceSize o[16];for(unsigned j=0;j<k[r];j++){o[j]=H.vbo[f[r]+j];bc.vb[f[r]+j]=H.vb[f[r]+j];bc.vbo[f[r]+j]=H.vbo[f[r]+j];}
            vkCmdBindVertexBuffers(c.command,f[r],k[r],&H.vb[f[r]],o);++n;++S->c_vb;}}}
    DRAW_MARK(5);pass_prof_draw(c.command);
    const bool cond_gpu_draw=condr::gpu_begin(c.command);
    if(indexed){
        if(bc.ib!=H.ib || bc.ibo!=H.ibo || bc.it!=H.it){vkCmdBindIndexBuffer(c.command,H.ib,H.ibo,H.it);bc.ib=H.ib;bc.ibo=H.ibo;bc.it=H.it;++n;++S->c_ib;}else ++binds_skipped;
        vkCmdDrawIndexed(c.command,uint32_t(count),uint32_t(indexed->instances),H.first_index,int32_t(indexed->base_vertex)+int32_t(H.k),uint32_t(indexed->base_instance));}
    else vkCmdDraw(c.command,uint32_t(count),instances,uint32_t(first)+H.k,first_instance);
    if(cond_gpu_draw)condr::gpu_end(c.command);
    if(__builtin_expect(nrz5::mode!=0,0))nrz5::draw_end(bc,H.pipe,H.used,H.vb,H.vbo,H.ib,H.ibo,H.it,H.vp,H.sc,ds.layout,ds.set,ds.no,ds.off,indexed!=nullptr,uint32_t(count),   // [DOS2] NR-ZERO Z5 DM_NR_Z5=2: the same draw by the run assembler, compared
        indexed?uint32_t(indexed->instances):instances,indexed?H.first_index:uint32_t(first)+H.k,indexed?int32_t(indexed->base_vertex)+int32_t(H.k):0,indexed?uint32_t(indexed->base_instance):first_instance,cond_gpu_draw);
    S->cmds+=n;
    if(__builtin_expect(nrz5t::z8,0)){nrz8t::Hint &zh=nrz8t::hint;zh.pent=H.have_ent?H.ent:nullptr;zh.zero=H.zero;zh.qpat=H.qpat;zh.pp=pp;zh.special=nrpo::P.on || nrps::P.on;zh.lane_gen=tab_gen();}   // [NR-ZERO Z8] what a template needs besides the values
    if(__builtin_expect(nrz5t::gmode!=0,0))nrz5t::recorded(bc,ui,cond_gpu_draw,H.t.key,H.pipe,H.used,H.vb,H.vbo,H.ib,H.ibo,H.it,H.vp,H.sc,ds.layout,ds.set,ds.no,ds.off,indexed!=nullptr,uint32_t(count),z5i,z5f,z5v,z5b,H.first_index,H.k);   // [NR-ZERO Z5] =2: the comparison; the template of the record's slot
    }
    if(__builtin_expect(nrmdi::mode!=0,0) && dr)nrmdi::note(c,*dr,p,indexed,instances,first_instance);   // [DOS2] NR-MDI DM_NR_MDI=2: the batching count (-1: reads its switch once)
    }else{DRAW_MARK(5);pass_prof_draw(c.command);}   // [NR-21] queued: the commands come with the segment (nrsort::emit)
    if(sp)td=timing_clock();
    mark_storage_images();
    if(!occlusion_batch_enabled)end_occlusion_query();
    c.finish();++perf.draws;++pass_prof_draws;if(nbchk::armed)nbchk::draw();pass_prof_verts+=uint64_t(uint32_t(count))*instances;
    if(nbui::mode)nbui::draw();
    if(dr)dr->defer_writeback();stats.draws.fetch_add(1,std::memory_order_relaxed);if(prepass::live)prepass::live->done=true;   // [NR-30] the override draw is recorded   // (dr null: nrz3 - a set NR-08 serves has no storage to write back)
    if(dr)drecy::done(*dr);
    DRAW_MARK(6);zl=0;
    if(timed){const uint64_t te=timing_clock();if(sp){++S->sp_n;S->sp_key+=ta-t0;S->sp_vtx+=tb-ta;S->sp_vp+=tc-tb;S->sp_cmd+=td-tc;S->sp_tail+=te-td;}else{S->ns+=te-t0;++S->timed;}}
    if(nrz3::span_t0)nrz3::span_end();
    if(__builtin_expect(nrz3::mode>0,0) && dr && !ui)nrz3::served();   // [DOS2] NR-ZERO Z3: the path without the table recorded an engine draw - learn it / compare the table's answer (DM_NR_Z3=2)
    return true;}
// The old tail finished a draw lane() left to it (armed): teach the table; mode 2: compare the prediction.
void done(VkPipeline pipe,bool transient,const std::vector<VkVertexInputBindingDescription> &b,int zero,const std::vector<std::pair<uint32_t,Buffer*>> &vb,const Buffer *ib,VkIndexType it,
          const VkViewport &vp,const VkRect2D &sc,const p_draw_idx *indexed,int first){
    armed=false;if(!H.var_ok)return;H.var_ok=false;
    uint16_t used=0,inst=0;uint32_t stride[16]={};bool ok=pipe && !transient && zero>=-1 && zero<16;
    for(const auto &x:b){if(x.binding>=16){ok=false;break;}used=uint16_t(used|1u<<x.binding);stride[x.binding]=x.stride;if(x.inputRate==VK_VERTEX_INPUT_RATE_INSTANCE)inst=uint16_t(inst|1u<<x.binding);}
    if(H.pred){H.pred=false;++S->checks;bool bad=false;
        uint64_t pv=0;memcpy(&pv,&pipe,sizeof pipe);uint64_t hv=0;memcpy(&hv,&H.pipe,sizeof H.pipe);
        if(pv!=hv){bad=true;++S->bad_pipe;red("the table's pipeline %#llx is not the old path's %#llx",(unsigned long long)hv,(unsigned long long)pv);}
        if(used!=H.used || inst!=H.inst || zero!=H.zero || memcmp(stride,H.stride,sizeof stride)){bad=true;++S->bad_bind;
            red("vertex bindings: table %#x / inst %#x / zero %d, old path %#x / %#x / %d (or a stride)",unsigned(H.used),unsigned(H.inst),H.zero,unsigned(used),unsigned(inst),zero);}
        else{const int64_t ob=indexed?int64_t(indexed->base_vertex):int64_t(first);
            for(const auto &v:vb){const uint32_t i=v.first;if(i>=16 || int(i)==zero)continue;
                if(v.second->buffer!=H.vb[i] || !nr_vb_same(v.second->offset,ob,H.vbo[i],ob+int64_t(H.k),stride[i],inst>>i&1)){bad=true;++S->bad_vb;
                    red("vertex stream %u: old path buffer %p + %llu, short path %p + %llu with %u vertices folded (stride %u)",i,(void*)v.second->buffer,(unsigned long long)v.second->offset,
                        (void*)H.vb[i],(unsigned long long)H.vbo[i],H.k,stride[i]);break;}}}
        if(H.qpat){if(!ib || !ib->mapped || it!=VK_INDEX_TYPE_UINT32 || !nr_quad_ok(static_cast<const uint32_t*>(ib->mapped),H.count)){bad=true;++S->bad_ib;   // NR-12: the old copy = the pattern's first indices
                red("QUADS index list: the old path's %u indices (type %d) are not the pattern 4q + 0 1 2 0 2 3",H.count,int(it));}}
        else if(indexed && ib){if(ib->buffer!=H.ib || it!=H.it || !nr_ib_same(ib->offset,H.ibo,H.first_index,it==VK_INDEX_TYPE_UINT16?1u:2u)){bad=true;++S->bad_ib;
                red("index buffer: old path %p + %llu type %d, short path %p + %llu first index %u type %d",(void*)ib->buffer,(unsigned long long)ib->offset,int(it),(void*)H.ib,
                    (unsigned long long)H.ibo,H.first_index,int(H.it));}}
        else if((indexed!=nullptr)!=H.indexed){bad=true;++S->bad_ib;red("indexed draw on one path only");}
        if(memcmp(&vp,&H.vp,sizeof vp)){bad=true;++S->bad_vp;red("viewport: old path %g,%g %gx%g, short path %g,%g %gx%g",vp.x,vp.y,vp.width,vp.height,H.vp.x,H.vp.y,H.vp.width,H.vp.height);}
        if(memcmp(&sc,&H.sc,sizeof sc)){bad=true;++S->bad_sc;red("scissor: old path %d,%d %ux%u, short path %d,%d %ux%u",sc.offset.x,sc.offset.y,sc.extent.width,sc.extent.height,
            H.sc.offset.x,H.sc.offset.y,H.sc.extent.width,H.sc.extent.height);}
        if(bad)++S->bad;}
    if(!ok)return;
    // a shader set with a storage image never enters the table (the old tail keeps its storage bookkeeping)
    for(unsigned st=0;st<5;st++)if(state.shader[st])for(const auto &x:state.shader[st]->bindings)if(x.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)return;
    if(epoch!=nbpipe::epoch)return;   // the caches moved inside this draw: the variant was taken before
    nrp_tent *e=nrp_tab_put(&T,H.t.key,&H.var);if(!e)return;
    uint64_t pv=0;memcpy(&pv,&pipe,sizeof pipe);
    if(e->pipe!=pv || e->used!=used || e->inst!=inst || e->zero!=zero || memcmp(e->stride,stride,sizeof stride)){if(e->pipe)++S->relearn;else ++S->learned;}
    e->pipe=pv;e->used=used;e->inst=inst;e->zero=int8_t(zero);memcpy(e->stride,stride,sizeof stride);}
// A draw that returned before the end of the old tail leaves a prediction behind (mode 2): counted at the next record.
void stale(){if(H.pred){H.pred=false;++S->not_compared;}armed=false;cur=nullptr;}
// NR-12: around the draw() of a plain draw record (execute OP_DRAW / OP_DRAW_IDX, only while ui_mode != 0): ui_rec lets the lane line ask
// ui_pick for this draw only; ui_end drops what a draw() that returned early left behind (as stale() after an OP_X_NR_DRAW).
void ui_end(){ui_rec=false;stale();}
}
