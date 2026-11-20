// [DOS2] NR-UIX: the Iggy UI passes (docs/dos2/nr/NR-UIX.md; pure parts dm/gfx/nr/nr_uix.h, host test tests/nr_uix_test.cpp). Included by
// xbg_game.cpp right after xbg_nb_uiarea.inc (namespace uiarea, whose history / pass tracking these parts extend). Hooks (each behind its mode):
//   draw()          uix::geo before uiarea::pre (the vertex box of an Iggy draw), uix::cache_draw after it, uix::begin at the open-pass check
//   end_open_pass   uix::ended (which pass ended, at which source line)        prepare_read  uix::rbreak (a read break of the open pass)
//   nbrp_begin      the smaller render area of an Iggy pass the NB plan begins (uiarea::will_area / uiarea::area)
//   uiarea          hist2 (ordinal keys), the DS prediction, the area seed, uix::commit / reattach / ui_pass / pass_seen / frame
//   load_shader_key the vertex position program (Shader::pos) is built while DM_NR_UIX_AREA is on (as for DM_NR_COVER)
// Everything needs DM_NB_UIAREA (default 1 in DOS2): the parts extend its pass tracking. Switches (unset = xbg_nr_uix_default, 0 everywhere
// until a phone run proves them):
//   DM_NR_UIX_DS     0 off; 1 depth-stencil detach of Iggy passes decided per ordinal key with decay (a pass start keeps its DS only if it
//                    used it in the last 120 frames); 2 counted, the old (sticky, state-keyed) history decides; 3 stress: every Iggy pass
//                    begins without its DS, a draw that needs it re-attaches (host restart with LOAD) - any wrong picture = a re-attach bug
//   DM_NR_UIX_AREA   0 off; 1 the draw rect of an Iggy draw is the box of its vertices (CPU position program, as NR-10b) inside viewport x
//                    scissor, the area seed comes from the ordinal history, and passes the NB plan begins get the smaller area too
//                    (stores forced STORE); 2 counted (boxes computed, nothing applied); 3 stress: the first draw's box alone, 1 px
//                    alignment, no history: most later draws restart the pass - clipped UI = a box bug
//   DM_NR_UIX_RB     0 off; 1 / 2 counted: read breaks of the open pass and whether the next pass is the same target again (only those
//                    cost a break: the rest end a pass that ends anyway), every re-begin of the target that just ended by source line
//   DM_NR_UIX_CACHE  0 off; 1 / 2 counted: a hash of the UI stream of each frame (Iggy passes: state, shaders, vertex / index / uniform
//                    bytes, textures and their content generation) against the previous frame - how often a UI cache would hit
// Lines every 10 s: "[nr] uix ds ...", "[nr] uix area ...", "[nr] uix rb ...", "[nr] uix cache ..." (only the parts that are on).
namespace uix{
struct Stats{
    uint64_t frames=0,ui_passes=0;
    uint64_t ds_att=0,ds_hit=0,ds_miss=0,ds_kept_unused=0,ds_kept_used=0,ds_applied=0,ds_reattach=0,ds_px=0;
    uint64_t area_q=0,area_q_old=0,area_px=0,area_bytes=0,reduced=0,reduced_px=0,reduced_bytes=0,gdraws=0,vdraws=0,geo_why[8]{},geo_ns=0,geo_timed=0,geo_verts=0,geo_ok=0;
    uint64_t rb=0,rb_ui=0,rb_self=0,rb_self_ui=0,rb_nat=0,rb_nat_ui=0,rb_open=0,rebegin=0,rebegin_ui=0,rebegin_clear=0;
} s;
enum{GW_OK,GW_GI,GW_NOPROG,GW_PRIM,GW_VIEW,GW_INPUT,GW_UBO,GW_VERTS};
static const char *const gw_name[8]={"box","GPU indirect","no position program","points / other primitive","raster discard / viewport","vertex format / bytes","uniform range","more than 4096 vertices"};
struct Line{int line=0;uint64_t n=0,ui=0;};Line lines[12];
struct Last{bool on=false,ui=false,rb=false;unsigned why=0;VkRenderPass pass{};VkFramebuffer fb{};int line=0;} last;
bool rb_pend=false;unsigned rb_why=0;
nr_uix_frame fprev{},fcur{NR_UIX_FNV0,NR_UIX_FNV0,0,0};nr_uix_cache_acc cacc{};uint64_t ui_gen=1;
uint64_t next_ns=0;unsigned told=0;
inline bool iggy_fs(Shader *fs){if(!fs)return false;if(fs->nrui<0){const int c=nb_ui_shader_class(fs->key.c_str());fs->nrui=int8_t(c==NB_UISH_IGGY?1:c==NB_UISH_IGGY_FILL?2:c==NB_UISH_IGGY_BLUR?3:0);}return fs->nrui>0;}
int env_mode(const char *name,int def,int hi){const char *e=getenv(name);int v=e && *e?atoi(e):def;if(v<0 || v>hi){xbg_logf("【红】[nr] %s=%d: only 0..%d: off",name,v,hi);v=0;}return v;}
void init(XbgGpuFamily fam){
    ds_mode=env_mode("DM_NR_UIX_DS",xbg_nr_uix_default(fam,0),3);area_mode=env_mode("DM_NR_UIX_AREA",xbg_nr_uix_default(fam,1),3);
    rb_mode=env_mode("DM_NR_UIX_RB",xbg_nr_uix_default(fam,2),2);cache_mode=env_mode("DM_NR_UIX_CACHE",xbg_nr_uix_default(fam,3),2);
    if(!(ds_mode || area_mode || rb_mode || cache_mode))return;
    if(cache_mode)draw_reads_on=true;   // the textures each draw reads (prepare_read), for the stream hash
    const char *ua=getenv("DM_NB_UIAREA");const bool ua_off=ua && *ua && atoi(ua)==0;
    auto src=[](const char *n){const char *v=getenv(n);return v && *v?"env":"family default";};   // tiler plan step 1: DS / AREA 1 on the tile families
    xbg_logf("[nr] uix (NR-UIX, policy family %s): DM_NR_UIX_DS=%d (%s; %s), DM_NR_UIX_AREA=%d (%s; %s), DM_NR_UIX_RB=%d, DM_NR_UIX_CACHE=%d (counts); %s; lines every 10 s",
        xbg_gpu_family_name(fam),ds_mode,src("DM_NR_UIX_DS"),ds_mode==1?"Iggy depth-stencil detach by ordinal pass keys + 120-frame decay":ds_mode==2?"counted, the old history decides":ds_mode==3?"stress: always detached, re-attach on use":"off",
        area_mode,src("DM_NR_UIX_AREA"),area_mode==1?"render area = union of the vertex boxes, also for NB-plan passes":area_mode==2?"counted":area_mode==3?"stress: the first box alone":"off",rb_mode,cache_mode,
        ua_off?"DM_NB_UIAREA=0: nothing of it runs (the parts extend uiarea)":"needs DM_NB_UIAREA (on)");}
// ---- DM_NR_UIX_AREA: the box of the vertices of the Iggy draw about to call uiarea::pre (g.valid stays false when there is none)
void geo(const Pass &p,int prim0,bool quad_plain,int first0,int count0,int first,int count,const p_draw_idx *indexed,const std::vector<std::pair<uint32_t,Buffer*>> &vbufs,const Buffer *ib,VkIndexType itype,bool gi){
    g.valid=false;if(!iggy_fs(state.shader[4]))return;
    const bool timed=!(s.geo_why[0]&15);const uint64_t t0=timed?timing_clock():0;
    auto no=[&](int w){++s.geo_why[w];};
    if(gi)return no(GW_GI);
    const Shader *vs=state.shader[0];if(!vs || !vs->pos || !vs->pos->ok)return no(GW_NOPROG);
    const int margin=nr_uix_prim_margin(prim0,state.poly.mode==2);if(margin<0)return no(GW_PRIM);
    VkViewport tv{};
    if(state.rast_discard || xbg_nvn_viewport(&tv,state.viewport.x,state.viewport.y,state.viewport.width,state.viewport.height,state.viewport.minDepth,state.viewport.maxDepth)!=0 || taafix::viewport_ll())return no(GW_VIEW);
    const nr_cv_prog &pr=*vs->pos;
    struct In{const uint8_t *base=nullptr;uint64_t size=0,stride=0,offset=0;int nc=0;} in[32];
    for(uint32_t m=pr.in_mask;m;m&=m-1){const unsigned loc=unsigned(__builtin_ctz(m));if(loc>=16)return no(GW_INPUT);
        va_st a;memcpy(&a,&state.attrib[loc],4);const VkFormat f=a.fmt?format(a.fmt):VK_FORMAT_UNDEFINED;
        const int nc=f==VK_FORMAT_R32_SFLOAT?1:f==VK_FORMAT_R32G32_SFLOAT?2:f==VK_FORMAT_R32G32B32_SFLOAT?3:f==VK_FORMAT_R32G32B32A32_SFLOAT?4:0;
        if(!nc || a.stream>=16 || !state.stream[a.stream].stride || state.stream[a.stream].divisor)return no(GW_INPUT);
        const Buffer *vb=nullptr;for(auto &b:vbufs)if(b.first==a.stream)vb=b.second;
        if(!vb || !vb->mapped)return no(GW_INPUT);
        in[loc].base=static_cast<const uint8_t*>(vb->mapped);in[loc].size=vb->size;in[loc].stride=state.stream[a.stream].stride;in[loc].offset=a.offset;in[loc].nc=nc;}
    const uint8_t *ub[NR_CV_MAX_UBO]{};size_t ubn[NR_CV_MAX_UBO]{};
    for(unsigned u=0;u<pr.nubo;u++){const xbg_shader_binding *sb=nullptr;
        for(auto &b:vs->bindings)if(b.binding==pr.ubo[u] && b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.source==XBG_SOURCE_UNIFORM && b.count==1 && b.slot<16)sb=&b;
        if(!sb)return no(GW_UBO);
        const NVNbufferRange &r=state.ub[nvn_stage[0]][sb->slot];const size_t bound=std::min<size_t>(r.size,sb->bytes?sb->bytes:65536);
        // the bytes the GPU gets: not under the host's UI shift patch, not a GPU result still on its way back
        if(!r.address || bound<pr.ubo_bytes[u] || (g_ui_moff!=0 && r.address==g_ui_moff_addr) || overlaps_pending(r.address,bound))return no(GW_UBO);
        ub[u]=reinterpret_cast<const uint8_t*>(uintptr_t(r.address));ubn[u]=bound;}
    const int n=quad_plain?count0:count;if(n<=0)return no(GW_PRIM);if(n>4096)return no(GW_VERTS);
    const bool idx=!quad_plain && indexed;if(idx && (!ib || !ib->mapped))return no(GW_INPUT);
    const size_t isz=itype==VK_INDEX_TYPE_UINT16?2:4;
    nr_uix_bb bb;nr_uix_bb_init(&bb);
    for(int k=0;k<n;k++){int64_t vid;
        if(idx){if(uint64_t(k+1)*isz>ib->size)return no(GW_INPUT);const uint8_t *q=static_cast<const uint8_t*>(ib->mapped)+size_t(k)*isz;
            uint32_t x=0;if(isz==2){uint16_t h;memcpy(&h,q,2);x=h;}else memcpy(&x,q,4);vid=int64_t(x)+indexed->base_vertex;}
        else vid=int64_t(quad_plain?first0:first)+k;
        if(vid<0)return no(GW_INPUT);
        float v[32][4],P[4];
        for(uint32_t m=pr.in_mask;m;m&=m-1){const unsigned loc=unsigned(__builtin_ctz(m));const In &x=in[loc];
            const uint64_t off=uint64_t(vid)*x.stride+x.offset;if(off+4u*unsigned(x.nc)>x.size)return no(GW_INPUT);
            v[loc][0]=v[loc][1]=v[loc][2]=0;v[loc][3]=1;memcpy(v[loc],x.base+off,4*size_t(x.nc));}
        if(!nr_cv_eval(&pr,v,ub,ubn,P))return no(GW_UBO);
        nr_uix_bb_add(&bb,P,tv.x,tv.y,tv.width,tv.height);if(bb.bad)return no(GW_VIEW);}
    if(!nr_uix_bb_rect(&bb,margin,nb_ua_full(p.width,p.height),&g.r))return no(GW_VIEW);
    g.valid=true;no(GW_OK);s.geo_verts+=uint64_t(n);
    if(timed){s.geo_ns+=timing_clock()-t0;++s.geo_timed;}}
// ---- uiarea: a pass ended (both parts), a re-attach
void commit(const nb_ua_pass &q,bool pred,bool pred_on,nb_ua_rect guni,uint32_t gdraws,uint32_t vdraws,int32_t align,uint32_t min_save){
    ++s.ui_passes;s.gdraws+=gdraws;s.vdraws+=vdraws;const uint64_t full=uint64_t(q.w)*q.h;
    if(q.ds_att){++s.ds_att;
        if(q.detached){++s.ds_applied;if(ds_mode!=2)s.ds_px+=full;}
        if(pred_on && ds_mode==2){if(pred){if(q.ds_used){++s.ds_miss;if(told<8){++told;xbg_logf("[nr] uix ds: a pass start predicted without its depth-stencil used it (%ux%u, %u draws): a re-attach at 1",q.w,q.h,q.draws);}}else{++s.ds_hit;s.ds_px+=full;}}
            else if(q.ds_used)++s.ds_kept_used;else ++s.ds_kept_unused;}}
    if(area_mode && !q.clear_start){nb_ua_rect sd;
        if(nb_ua_seed(guni,nullptr,q.w,q.h,align,min_save,&sd)){++s.area_q;const uint64_t px=full-nb_ua_px(sd);s.area_px+=px;s.area_bytes+=px*4u*q.natt*2u;}
        if(nb_ua_seed(q.uni,nullptr,q.w,q.h,align,min_save,&sd) && area_mode==2)++s.area_q_old;}
    if(q.reduced){++s.reduced;const uint64_t px=full-nb_ua_px(q.area);s.reduced_px+=px;s.reduced_bytes+=px*4u*q.natt*2u;}}
void reattach(){++s.ds_reattach;}
// ---- DM_NR_UIX_RB
void rbreak(unsigned why){++s.rb;rb_pend=true;rb_why=why;if(why&4)++s.rb_open;}
void ended(int line){last.on=true;last.pass=open_pass.pass;last.fb=open_pass.framebuffer;last.line=line;last.rb=rb_pend;last.why=rb_why;rb_pend=false;
    last.ui=uiarea::cur.on && uiarea::cur.pass==open_pass.pass && uiarea::cur.fb==open_pass.framebuffer && uiarea::cur.rec.cls!=NB_UA_CLS_OTHER;}
void begin(const Pass &p,bool clear_site){if(!last.on)return;last.on=false;
    const bool same=p.pass==last.pass && p.framebuffer==last.fb;
    if(last.rb){if(same){++s.rb_self;if(last.ui)++s.rb_self_ui;}else{++s.rb_nat;if(last.ui)++s.rb_nat_ui;}if(last.ui)++s.rb_ui;}
    if(!same)return;++s.rebegin;if(last.ui)++s.rebegin_ui;if(clear_site)++s.rebegin_clear;
    for(auto &l:lines){if(l.line==last.line || !l.n){l.line=last.line;++l.n;if(last.ui)++l.ui;return;}}}
// ---- DM_NR_UIX_CACHE
void pass_seen(bool ui){if(ui && cache_mode){++fcur.passes;}}
void ui_pass(const Pass &p){for(Image *im:p.images)im->uix_ui=ui_gen;
    uint64_t h=fcur.strict;h=nr_uix_fnv(h,&p.width,sizeof p.width);h=nr_uix_fnv(h,&p.height,sizeof p.height);for(Image *im:p.images)h=nr_uix_fnv(h,&im,sizeof im);
    fcur.strict=h;fcur.loose=nr_uix_fnv(fcur.loose,&h,sizeof h);}
void cache_draw(int prim0,bool quad_plain,int first0,int count0,uint32_t instances,const p_draw_idx *indexed,const std::vector<std::pair<uint32_t,Buffer*>> &vbufs,const Buffer *ib,VkIndexType itype){
    if(!(uiarea::cur.on && uiarea::cur.rec.cls!=NB_UA_CLS_OTHER))return;
    uint64_t h=NR_UIX_FNV0;
    for(unsigned i=0;i<5;i++){const Shader *sh=state.shader[i];h=nr_uix_fnv(h,&sh,sizeof sh);}
    h=nr_uix_fnv(h,&state.rt,sizeof state.rt);
    h=nr_uix_fnv(h,&state.attrib,size_t(reinterpret_cast<const uint8_t*>(&state.bounds+1)-reinterpret_cast<const uint8_t*>(&state.attrib)));   // vertex layout .. depth bounds (POD)
    h=nr_uix_fnv(h,&state.restart,sizeof state.restart);h=nr_uix_fnv(h,&state.alpha_ref,sizeof state.alpha_ref);
    h=nr_uix_fnv(h,state.tex[4],16*8);h=nr_uix_fnv(h,state.samp[4],16*8);h=nr_uix_fnv(h,state.tex[0],4*8);
    const int32_t d[6]={prim0,quad_plain,first0,count0,int32_t(indexed?indexed->instances:instances),indexed?indexed->base_vertex:0};h=nr_uix_fnv(h,d,sizeof d);
    // vertex bytes of the range the draw fetches, index bytes
    int64_t v0=quad_plain || !indexed?first0:INT64_MAX,v1=quad_plain || !indexed?int64_t(first0)+count0:0;bool unk=false;
    if(!quad_plain && indexed){if(!ib || !ib->mapped)unk=true;else{const size_t isz=itype==VK_INDEX_TYPE_UINT16?2:4;const size_t nb=std::min<size_t>(ib->size,size_t(std::max(count0,0))*(prim0==7?6u*isz/4u:isz));
        h=nr_uix_fnv(h,ib->mapped,nb);for(size_t k=0;k+isz<=nb;k+=isz){uint32_t x=0;if(isz==2){uint16_t y;memcpy(&y,static_cast<const uint8_t*>(ib->mapped)+k,2);x=y;}else memcpy(&x,static_cast<const uint8_t*>(ib->mapped)+k,4);
            const int64_t vv=int64_t(x)+indexed->base_vertex;if(vv<v0)v0=vv;if(vv+1>v1)v1=vv+1;}}}
    for(auto &b:vbufs){const Buffer *vb=b.second;if(!vb || !vb->mapped){unk=true;continue;}const uint64_t st=b.first<16?state.stream[b.first].stride:0;
        uint64_t o0=st && v0>=0?uint64_t(v0)*st:0,o1=st && v1>v0?uint64_t(v1)*st:vb->size;if(o1>vb->size)o1=vb->size;if(o1-o0>(1u<<20))o1=o0+(1u<<20);
        if(o1>o0)h=nr_uix_fnv(h,static_cast<const uint8_t*>(vb->mapped)+o0,size_t(o1-o0));}
    // uniform bytes of the vertex and fragment stages (Iggy: 0x80 + 0x30 B)
    for(unsigned st:{0u,4u}){const Shader *sh=state.shader[st];if(!sh)continue;
        for(auto &b:sh->bindings)if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.source==XBG_SOURCE_UNIFORM && b.slot<16){const NVNbufferRange &r=state.ub[nvn_stage[st]][b.slot];
            const size_t n=std::min<size_t>(r.size,b.bytes?b.bytes:4096);if(!r.address || overlaps_pending(r.address,n)){unk=true;continue;}h=nr_uix_fnv(h,reinterpret_cast<const void*>(uintptr_t(r.address)),n);}}
    // textures: identity always; content generation unless an Iggy pass of this frame wrote it (then it follows from the stream)
    uint64_t hl=h;
    if(draw_reads_overflow)unk=true;
    for(Image *im:draw_reads){h=nr_uix_fnv(h,&im,sizeof im);hl=nr_uix_fnv(hl,&im,sizeof im);if(im->uix_ui==ui_gen)continue;
        const uint64_t gen[3]={im->cpu_hash,im->last_write,im->storage_write};h=nr_uix_fnv(h,gen,sizeof gen);
        if(!im->gpu_written)hl=nr_uix_fnv(hl,&im->cpu_hash,sizeof im->cpu_hash);}   // loose: textures other passes render (portraits ...) taken as unchanged
    if(unk){const uint64_t u=ui_gen*0x9E3779B97F4A7C15ull;h=nr_uix_mix(h,u);hl=nr_uix_mix(hl,u);}   // bytes not known here: never equal
    fcur.strict=nr_uix_mix(fcur.strict,h);fcur.loose=nr_uix_mix(fcur.loose,hl);++fcur.draws;}
void tick();
void frame(){++s.frames;if(cache_mode){nr_uix_cache_close(&cacc,&fprev,&fcur);fprev=fcur;fcur=nr_uix_frame{NR_UIX_FNV0,NR_UIX_FNV0,0,0};++ui_gen;}tick();}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const double f=s.frames?double(s.frames):1.;
    if(ds_mode)xbg_logf("[nr] uix ds DM_NR_UIX_DS=%d: %llu frames; a frame: Iggy passes %.1f, with a depth-stencil attached by the guest %.2f; %s; applied: without DS %.2f (%.1f MB load+store at 4 B/px, tiler model), re-attached %.3f",
        ds_mode,(unsigned long long)s.frames,s.ui_passes/f,s.ds_att/f,
        ds_mode==2?[&]{static char b[200];snprintf(b,sizeof b,"would detach %.2f (unused), would miss %.3f (used: a re-attach each), kept and unused %.2f, kept and used %.2f",s.ds_hit/f,s.ds_miss/f,s.ds_kept_unused/f,s.ds_kept_used/f);return (const char*)b;}():"ordinal history",
        s.ds_applied/f,(ds_mode==2?s.ds_hit:s.ds_applied)?double(s.ds_px)*8./f/1048576.:0.,s.ds_reattach/f);
    if(area_mode){char why[400];int n=0;why[0]=0;for(int k=1;k<8;k++)if(s.geo_why[k] && n<int(sizeof why))n+=snprintf(why+n,sizeof why-size_t(n),"%s%s %.2f",n?", ":"",gw_name[k],s.geo_why[k]/f);
        xbg_logf("[nr] uix area DM_NR_UIX_AREA=%d: %llu frames; a frame: Iggy draws with a vertex box %.1f (%.1f vertices, %.0f ns a box, 1 in 16), no box %s; Iggy passes whose box union saves >=10%% %.2f%s = %.1f MB load+store (4 B/px); applied: smaller area %.2f (%.2f Mpx, %.1f MB)",
            area_mode,(unsigned long long)s.frames,s.geo_why[0]/f,s.geo_why[0]?double(s.geo_verts)/double(s.geo_why[0]):0.,s.geo_timed?double(s.geo_ns)/double(s.geo_timed):0.,n?why:"none",s.area_q/f,
            area_mode==2?[&]{static char b[64];snprintf(b,sizeof b," (by viewport x scissor %.2f)",s.area_q_old/f);return (const char*)b;}():"",s.area_bytes/f/1048576.,s.reduced/f,s.reduced_px/f/1e6,s.reduced_bytes/f/1048576.);}
    if(rb_mode){char ln[300];int n=0;ln[0]=0;for(auto &l:lines)if(l.n && n<int(sizeof ln))n+=snprintf(ln+n,sizeof ln-size_t(n),"%sL%d %.2f (UI %.2f)",n?", ":"",l.line,l.n/f,l.ui/f);
        xbg_logf("[nr] uix rb DM_NR_UIX_RB=%d: %llu frames; a frame: read breaks %.2f (of an attachment of the open pass %.2f; UI passes %.2f): the next pass is the same target %.2f (UI %.2f) = the only ones a feedback loop / merge could save, another target %.2f (UI %.2f); "
            "re-begins of the target that just ended %.2f (UI %.2f, by a clear %.2f) by the line that ended it: %s",
            rb_mode,(unsigned long long)s.frames,s.rb/f,s.rb_open/f,s.rb_ui/f,s.rb_self/f,s.rb_self_ui/f,s.rb_nat/f,s.rb_nat_ui/f,s.rebegin/f,s.rebegin_ui/f,s.rebegin_clear/f,n?ln:"none");
        for(auto &l:lines)l=Line{};}
    if(cache_mode){const double uf=cacc.ui_frames?double(cacc.ui_frames):1.;
        xbg_logf("[nr] uix cache DM_NR_UIX_CACHE=%d: %llu frames, %llu with UI (%.1f Iggy passes, %.1f draws a UI frame); the UI stream equals the previous frame's: strict %.1f %% (longest run %llu), "
            "loose (textures other passes render taken as unchanged) %.1f %% (longest %llu); a cache would skip %.1f %% (strict) / %.1f %% (loose) of the UI draws",
            cache_mode,(unsigned long long)cacc.frames,(unsigned long long)cacc.ui_frames,double(fprev.passes),cacc.draws/uf,100.*cacc.same_strict/uf,(unsigned long long)cacc.best_strict,
            100.*cacc.same_loose/uf,(unsigned long long)cacc.best_loose,cacc.draws?100.*cacc.draws_same_strict/cacc.draws:0.,cacc.draws?100.*cacc.draws_same_loose/cacc.draws:0.);
        const uint64_t rs=cacc.run_strict,rl=cacc.run_loose;cacc=nr_uix_cache_acc{};cacc.run_strict=rs;cacc.run_loose=rl;}
    s=Stats{};}
}
