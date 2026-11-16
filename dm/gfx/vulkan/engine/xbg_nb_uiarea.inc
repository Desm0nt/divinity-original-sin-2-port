// [DOS2] NB-18b-6: DM_NB_UIAREA (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-18b-6 findings"). Included from xbg_game.cpp after xbg_nb.inc; hooks
// there (each `if(uiarea::mode)...`, mode -1 until the switch is read, then 0 / 1 / 2):
//   create_pass  uiarea::detach      a plain draw with depth / stencil attached that does not use it: build the pass without the DS
//   begin_open   uiarea::area        the render area of the pass being begun (only when draw() armed it for exactly this pass)
//   draw()       uiarea::pre         before the pass check: the draw's pixel rect, pass tracking, restart of a smaller-area pass it leaves
//   clear path   uiarea::before_clear  an in-pass clear outside the smaller area restarts the pass (vkCmdClearAttachments must lie inside)
// Pure logic: dm/gfx/nb/nb_uiarea.h (host test tests/nb_uiarea_test.cpp).
//
// DM_NB_UIAREA=0 (default): nothing runs (the hooks see mode 0; create_pass keeps its fast path and its attachments).
// DM_NB_UIAREA=2: count only, the old path executes. Every 10 s: "[nb] uiarea DM_NB_UIAREA=2: ..." (host passes whose first draw is an
//   Iggy shader: how many qualify for a smaller render area and the pixels / bytes saved, how many carry a depth-stencil no draw uses,
//   Iggy fill draws).
// DM_NB_UIAREA=1: applied, to host passes whose first draw is an Iggy shader (nb_ui.h: d99f1fdb / fill b5afd82f / blur 5e26ce73):
//  * Render area. The host cannot know the later draws of a pass at its begin (the NB header carries no scissor / DS facts, and the
//    front end that could add them is owned by NB-10 / NB-13a now), so the design is grow-only with one restart: the pass begins with
//    (first draw rect ∪ the union the same pass start drew last time) aligned to 32 px; a later draw (or in-pass clear) outside it ends the
//    pass (STORE inside the area) and the host begins it again with the full area and LOAD. Only begin_open passes are reduced: their
//    loadOp is LOAD (or DONT_CARE: the colour-noload twin) and storeOp STORE (or DONT_CARE: STORE_HIST) - Vulkan restricts load / store
//    to the render area, so outside it the attachments keep their contents, which is what LOAD / STORE means and a subset of what
//    DONT_CARE allows. A CLEAR loadOp (lazy clear twins, NB-RP clear folds) clears only inside the area, so those passes keep the full
//    area (not hooked). Off under the poison modes (STORE_HIST=3, DM_NB_RP=3: full-target vkCmdClearAttachments before the pass end).
//  * Depth-stencil detach. Pipelines are built against the render pass (no dynamic rendering), so a pass without its DS needs its own
//    render pass + framebuffer + pipeline variants: create_pass simply leaves the depth target out, which keys a separate render pass
//    and the pipeline lookup (p.definition) builds the variants (one per Iggy pipeline, ~10, cached like any other). A draw of that pass
//    that uses depth / stencil gets the pass with its DS (a different render pass: the host ends the DS-less one and begins the full one
//    with LOAD) and marks the pass start, so it never begins without its DS again. Nothing of the DS changes while it is detached.
// Fills (b5afd82f, no inputs: out0 = c3[0], a constant from UBO vk binding 10 - checked in the translated SPIR-V): counted only. A fill
// that covers the whole target is already begun with the colour-DONT_CARE twin (first_draw_covers), so a clear gains nothing there; a
// sub-rect fill needs a rect coverage proof first (nvn_cover only proves the whole framebuffer).
#include "../../nb/nb_uiarea.h"
#include "../../nb/nb_ui.h"
namespace uiarea{
int mode=-1;
const int32_t k_align=32,k_margin=4;const uint32_t k_min_save=100;   // tile grid, viewport margin (wide points / lines), 10 % minimum saving
struct Cur{bool on=false;VkRenderPass pass{};VkFramebuffer fb{};VkCommandBuffer cmd{};uint64_t key=0,pass_no=0;decltype(state.rt) rt{};nb_ua_pass rec{};} cur;
struct ClearPass{decltype(state.rt) rt{};uint64_t pass_no=~0ull;} cpass;   // the pass the clear path opened or continued last (its RT set)
nb_ua_hist hist;nb_ua_acc acc{};bool armed=false;nb_ua_rect seed{};uint64_t last_frame=~0ull,next_ns=0,frame_no=0;
void init(){const char *e=getenv("DM_NB_UIAREA");int v=e && *e?atoi(e):0;if(v<0 || v>2){xbg_logf("[nb] DM_NB_UIAREA=%d: only 0 / 1 / 2: off",v);v=0;}
    mode=v;nb_ua_hist_reset(&hist);nb_ua_pass_init(&cur.rec);
    if(mode)xbg_logf("[nb] uiarea DM_NB_UIAREA=%d: %s; Iggy passes only, line every 10 s",mode,mode==1?"render area = union of the draws (grow-only, one restart) + depth-stencil detach":"count only, the old path executes");}
uint64_t key(){uint64_t h=0xcbf29ce484222325ull;auto mix=[&](const void *p,size_t n){const uint8_t *b=static_cast<const uint8_t*>(p);for(size_t i=0;i<n;i++)h=(h^b[i])*0x100000001B3ull;};
    mix(&state.rt,sizeof state.rt);const Shader *fs=state.shader[4];mix(&fs,sizeof fs);mix(&state.scissor,sizeof state.scissor);mix(&state.viewport,sizeof state.viewport);return h?h:1;}
int cls(){const Shader *fs=state.shader[4];const int c=fs?nb_ui_shader_class(fs->key.c_str()):NB_UISH_OTHER;
    return c==NB_UISH_IGGY?NB_UA_CLS_IGGY:c==NB_UISH_IGGY_FILL || c==NB_UISH_IGGY_BLUR?NB_UA_CLS_FILTER:NB_UA_CLS_OTHER;}
bool uses_ds(){return nb_ua_uses_ds(state.ds.dt,state.ds.st,state.bounds.enable);}
bool continues(VkCommandBuffer cmd){return cur.on && open_pass.cmd==cmd && open_pass.pass==cur.pass && open_pass.framebuffer==cur.fb && perf.passes==cur.pass_no;}
void tick();
void commit(){if(!cur.on)return;cur.on=false;nb_ua_commit(&acc,&cur.rec,k_align,k_min_save);
    if(cur.rec.cls!=NB_UA_CLS_OTHER && cur.rec.draws){nb_ua_hent *e=nb_ua_hist_put(&hist,cur.key);
        if(e){if(nb_ua_empty(e->uni) || e->frame+60<frame_no)e->uni=cur.rec.uni;else e->uni=nb_ua_union(e->uni,cur.rec.uni);e->frame=uint32_t(frame_no);}}}   // grow-only, refreshed after 60 frames
// create_pass, plain draw with a depth target: leave it out?
bool detach(VkCommandBuffer cmd){
    if(mode<0)init();if(mode!=1)return false;
    const bool uses=uses_ds();
    if(continues(cmd) && !memcmp(&state.rt,&cur.rt,sizeof state.rt)){
        if(!cur.rec.detached || !uses)return cur.rec.detached;
        if(nb_ua_hent *e=nb_ua_hist_put(&hist,cur.key))e->ds=1;++acc.ds_reattach;return false;}   // the pass gets its DS again (host restart)
    if(uses || state.rt.n<1 || cls()==NB_UA_CLS_OTHER)return false;
    if(open_pass.cmd==cmd && perf.passes==cpass.pass_no && !memcmp(&state.rt,&cpass.rt,sizeof state.rt))return false;   // the clear path's pass (with its DS) of this RT set: continue it
    const nb_ua_hent *e=nb_ua_hist_find(&hist,key());return !(e && e->ds);}
// draw(), after create_pass and the vertex streams, before the open-pass check
void pre(const Pass &p,VkCommandBuffer cmd,bool plain){
    if(mode<0)init();if(!mode)return;
    const uint64_t fr=stats.presents.load();if(fr!=last_frame){last_frame=fr;++frame_no;++acc.frames;}
    nb_ua_rect r=nb_ua_full(p.width,p.height);
    if(plain){VkViewport v{};const VkRect2D s=scissor(p);
        if(xbg_nvn_viewport(&v,state.viewport.x,state.viewport.y,state.viewport.width,state.viewport.height,state.viewport.minDepth,state.viewport.maxDepth)!=0)r=nb_ua_none();   // draw() skips it
        else r=nb_ua_draw_rect(v.x,v.y,v.width,v.height,s.offset.x,s.offset.y,s.extent.width,s.extent.height,p.width,p.height,k_margin);}
    const bool same=open_pass.cmd==cmd && open_pass.pass==p.pass && open_pass.framebuffer==p.framebuffer;
    armed=false;
    if(same && continues(cmd)){
        if(mode==1 && cur.rec.reduced && !nb_ua_contains(cur.rec.area,r)){end_open_pass();cur.rec.reduced=0;cur.rec.restarted=1;++acc.restarts_draw;++cur.pass_no;}   // the host begins it again: full area, LOAD
    }else{
        commit();cur.on=true;cur.pass=p.pass;cur.fb=p.framebuffer;cur.cmd=cmd;cur.key=key();cur.rt=state.rt;cur.pass_no=same?perf.passes:perf.passes+1;
        nb_ua_pass &q=cur.rec;nb_ua_pass_init(&q);q.w=p.width;q.h=p.height;q.natt=uint32_t(p.images.size());q.cls=cls();q.ds_att=state.rt.depth!=nullptr;
        q.detached=q.ds_att;for(auto *im:p.images)if(im->aspects!=VK_IMAGE_ASPECT_COLOR_BIT)q.detached=0;   // create_pass left the DS out
        q.clear_start=same || (pclear_n && lazy_clear_known(p) && pclear_has(p));   // begun by a clear / pending clears become its load ops
        if(!same && !q.clear_start && mode==1 && q.cls!=NB_UA_CLS_OTHER && plain && p.samples==VK_SAMPLE_COUNT_1_BIT && store_hist_mode!=3 && nb_rp_mode!=3){
            const nb_ua_hent *e=nb_ua_hist_find(&hist,cur.key);armed=nb_ua_seed(r,e && !nb_ua_empty(e->uni)?&e->uni:nullptr,p.width,p.height,k_align,k_min_save,&seed)!=0;}}
    nb_ua_pass &q=cur.rec;
    if(q.cls!=NB_UA_CLS_OTHER && state.shader[4] && nb_ui_shader_class(state.shader[4]->key.c_str())==NB_UISH_IGGY_FILL){++q.fills;
        if(!(state.color.blend_en&1) && (state.mask.mask&15)==15 && !uses_ds()){++q.fills_opaque;if(!q.draws && r.x0==0 && r.y0==0 && r.x1==int32_t(p.width) && r.y1==int32_t(p.height))++q.fills_first_full;}}
    ++q.draws;q.uni=nb_ua_union(q.uni,r);if(state.rt.depth && uses_ds())q.ds_used=1;
    tick();}
// begin_open: the render area of the pass draw() armed (exactly cur's pass), else untouched
void area(const Pass &p,VkRect2D &ra){
    if(!armed)return;armed=false;
    if(mode!=1 || !cur.on || p.pass!=cur.pass || p.framebuffer!=cur.fb)return;
    ra.offset={seed.x0,seed.y0};ra.extent={uint32_t(seed.x1-seed.x0),uint32_t(seed.y1-seed.y0)};cur.rec.reduced=1;cur.rec.area=seed;}
// clear path, before its open-pass check: r = the clear rect
void before_clear(const Pass &p,const VkRect2D &r,VkCommandBuffer cmd){
    if(mode<0)init();if(!mode)return;armed=false;
    cpass.rt=state.rt;cpass.pass_no=open_pass.cmd==cmd && open_pass.pass==p.pass && open_pass.framebuffer==p.framebuffer?perf.passes:perf.passes+1;   // this clear continues / begins it
    if(continues(cmd) && cur.rec.detached && !memcmp(&state.rt,&cur.rt,sizeof state.rt)){   // the clear's pass has the DS: the host breaks the DS-less one
        if(nb_ua_hent *e=nb_ua_hist_put(&hist,cur.key))e->ds=1;++acc.ds_reattach;return;}
    if(!(open_pass.cmd==cmd && open_pass.pass==p.pass && open_pass.framebuffer==p.framebuffer && continues(cmd)))return;
    const nb_ua_rect c{r.offset.x,r.offset.y,int32_t(r.offset.x+int32_t(r.extent.width)),int32_t(r.offset.y+int32_t(r.extent.height))};
    cur.rec.uni=nb_ua_union(cur.rec.uni,c);
    if(mode==1 && cur.rec.reduced && !nb_ua_contains(cur.rec.area,c)){end_open_pass();cur.rec.reduced=0;cur.rec.restarted=1;++acc.restarts_clear;++cur.pass_no;cpass.pass_no=perf.passes+1;}}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const double f=acc.frames?double(acc.frames):1.;
    xbg_logf("[nb] uiarea DM_NB_UIAREA=%d: %llu frames; a frame: host passes %.1f, Iggy-first %.1f (fill/blur-first %.1f, draws %.1f); "
        "area: qualify %.2f (fill/blur-first %.2f), saved %.2f Mpx = %.1f MB load+store at 4 B per attachment pixel, union smaller but begun with a clear %.2f; "
        "DS attached and unused %.2f (fill/blur-first %.2f) = %.1f MB; fills %.2f (opaque %.2f, first draw over the whole target %.2f); "
        "applied: smaller area %.2f (saved %.2f Mpx), DS detached %.2f, restarts by a draw %.3f / by a clear %.3f, DS re-attached %.3f; history %u",
        mode,(unsigned long long)acc.frames,acc.passes/f,acc.ui_passes/f,acc.flt_passes/f,acc.ui_draws/f,acc.area_q/f,acc.area_q_flt/f,acc.area_px_saved/f/1e6,
        acc.area_bytes_saved/f/1048576.,acc.no_area_clear/f,acc.ds_q/f,acc.ds_q_flt/f,acc.ds_bytes_saved/f/1048576.,acc.fills/f,acc.fills_opaque/f,acc.fills_first_full/f,
        acc.reduced/f,acc.reduced_px_saved/f/1e6,acc.detached/f,acc.restarts_draw/f,acc.restarts_clear/f,acc.ds_reattach/f,hist.n);
    acc=nb_ua_acc{};}
}
