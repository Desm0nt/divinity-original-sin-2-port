// [DOS2] NR-10b DM_NR_COVER (docs/dos2/nr/NR-10b.md; pure parts: dm/gfx/nr/nr_cover.h, xbg_spirv_pos.cpp; host tests tests/nr_cover_test.cpp,
// tests/nr_cover_census.py). Included by xbg_game.cpp right after first_draw_covers(). Hooks there (each behind `nrcov::mode` / `nrcov::red.on`):
//   load_shader_key   a vertex shader's position program (Shader::pos), only while the switch is on
//   draw()            the pass-begin site: nrcov::at_begin() in place of first_draw_covers(); nrcov::pre() before the open-pass check
//   nbrp_begin        nrcov::armed: the pass begins on the proven rect as its render area (the plan's cover_dc attachments DONT_CARE there)
//   begin_open        the colour-DONT_CARE twin when the pass has no noload render pass (DM_GFX_NOLOAD off)
//   clear path        nrcov::before_clear(): an in-pass clear outside a smaller render area restarts the pass
//
// The old proof (first_draw_covers) never fires in DOS2: its translated vertex shaders keep the registers in Function variables and
// write z / w as constants, so "position = one input as it is" fails (phone, ab7rp3: 25 refusals a frame, reason 0, load DONT_CARE 0).
// Here the position is a small program (xbg_spirv_pos_prog: 117 of DOS2's 6132 vertex shaders, among them every post / full-screen one
// and the Iggy rect shaders), run on the CPU for the vertices this draw submits with the draw's own vertex bytes and uniform ranges,
// and nr_cv_cover proves the pixel rect the draw writes (one triangle or a convex pair, pixel centres with a 1/4 px margin).
//   whole framebuffer  -> "covered" (the plan's cover_dc attachments load DONT_CARE; without a plan: the colour-DONT_CARE twin)
//   a part of it       -> dynamic resolution (the post chain draws viewport 1620x729 of 2160x972): the pass begins with that rect as
//                         its RENDER AREA and load DONT_CARE; Vulkan leaves what lies outside a render area untouched, which is what
//                         LOAD + a draw inside the rect gives. Every later draw / clear of the pass must stay inside: one that does
//                         not ends the pass (STORE - an area pass never discards a store) and the host begins it again, full area,
//                         LOAD. The host bookkeeping (store history, transient attachments, NR-10 visibility) sees such a pass as
//                         LOADed: only the Vulkan load op and the area differ. Only passes begun by the NB plan (nbrp_begin).
// DM_NR_COVER: 0 (default, every family: xbg_nr_cover_default) - nothing here runs, first_draw_covers decides as before.
//   1  the proof decides (old proof OR new one).
//   2  the old path executes; the new proof is computed for every pass begun at the draw site and counted: would-be full / area
//      decisions, the megabytes of load they save, the first failing check of each refusal. Red line when the old proof says yes and
//      the new one does not (it must be a superset).
//   3  = 1 + every attachment that begins DONT_CARE by a proof is cleared to magenta inside the proven rect right after the begin
//      (the first draw must paint all of it over: any magenta on screen = the proof is wrong for that pass).
// DM_NR_COVER_UBO=0: a position that reads a uniform block is refused (the Iggy rect shaders, ImGui; the post chain copies its inputs).
// DM_NR_COVER_AREA=0: no area passes (a proven part of the target loads as before).
// DM_NR_COVER_FMASK=0: the old write-mask rule (0xF only); 1 (default) = every component the format stores (NR-FRAME, nr_frame.h).
// Line every 10 s: "[nr] cover DM_NR_COVER=N: ...".
namespace nrcov{
enum{W_FULL,W_AREA,W_PLAN,W_GATE,W_RASTER,W_OWN,W_SHADER,W_NOPROG,W_ATTACH,W_DS,W_STATE,W_COND,W_PRIM,W_INPUT,W_BUFFER,W_UBO,W_VERTEX,W_EMPTY,W_GEOMETRY,W_CULLED,W_SMALL,W_N};
static const char *const why_name[W_N]={"whole target","part of the target","plan needs the old texels","host draw rewrite (quads / indirect / UI shift / no instance / A-B off)",
    "raster discard / viewport / lower-left origin","samples its own target","stages / discard / sample mask / alpha test","no position program","output / blend / logic op / write mask / 3D target",
    "depth / stencil / bounds test","A2C / line mode / MSAA","conditional render","primitive / count","vertex format / stream","vertex / index bytes","uniform range","w != 1 or z outside [0,1]",
    "no pixel in viewport ∩ scissor","geometry does not cover","covered but culled","part too large to pay"};
struct Dec{int why=W_NOPROG;nr_cv_rect r{};bool exact=false,fmask=false;};
struct Stats{uint64_t begins=0,nb=0,old_yes=0,why[W_N]{},px_full=0,px_area=0,att_full=0,att_area=0,applied_full=0,applied_area=0,area_off=0,poisoned=0,restart_draw=0,restart_clear=0,
    exact=0,computed=0,red=0,twin_fail=0,timed=0,ns=0,fmask=0;} s;
uint64_t next_ns=0,frames0=0;unsigned told=0,told_why=0;
bool use_ubo=true,use_area=true;   // DM_NR_COVER_UBO / DM_NR_COVER_AREA (A/B of the two riskier parts; default on with the switch)
// [DOS2] NR-FRAME (docs/dos2/nr/NR-FRAME.md) DM_NR_COVER_FMASK: 1 (default with the switch: exact and free) = a write mask is full when it covers
// every component the attachment's format stores (nr_fr_mask_full: R11G11B10F 0x7, R8 / R16 / R16F 0x1, RG16F 0x3 - the components a format
// lacks are not stored, so the old texel cannot survive); 0 = the old rule (mask == 0xF). The game's post passes write R11G11B10F with 0x7
// and R8 / R16 with 0x1 (warm-up list play-1118: bloom, luminance, fog, fade, linear depth, shadow mask), so the old rule refused all of them.
bool use_fmask=true;
// the components the attachment of colour slot i stores: the render pass's attachment format (lazy_info, the view format) and the
// image's format, the larger of the two (unknown = 4: needs 0xF)
unsigned slot_components(const Pass &p,int i){unsigned c=0;bool found=false;auto in=lazy_info.find(p.pass);
    for(size_t k=0;k<p.images.size() && k<9;k++)if(p.att_slot[k]==i){found=true;
        c=nr_fr_vk_components(uint32_t(p.images[k]->fmt));
        if(in!=lazy_info.end() && k<in->second.desc.size())c=std::max(c,nr_fr_vk_components(uint32_t(in->second.desc[k].format)));else c=4;
        break;}
    return found && c?c:4;}
const uint32_t k_min_save=100;   // permille of the target an area must save
// The area pass being recorded (nbrp_begin set it): its identity and its render area.
// (end_open_pass clears red.on: the area pass ends with the open pass.)
inline bool continues(VkCommandBuffer cmd){return red.on && red.cmd==cmd && open_pass.cmd==cmd && open_pass.pass==red.pass && open_pass.framebuffer==red.fb;}
void twin_failed(){++s.twin_fail;}
// The proof for the draw about to begin pass p. Conditions as first_draw_covers, except the vertex shader (a position program instead of
// "position = input"), the geometry (nr_cv_cover) and one more refusal (a conditional draw may not draw at all).
Dec decide(const Pass &p,int prim,int first,int count,const p_draw_idx *indexed,const std::vector<std::pair<uint32_t,Buffer*>> &vbufs,const Buffer *ib,VkIndexType itype){
    Dec d;const Shader *vs=state.shader[0],*fs=state.shader[4];
    VkViewport tv{};
    if(state.rast_discard || xbg_nvn_viewport(&tv,state.viewport.x,state.viewport.y,state.viewport.width,state.viewport.height,state.viewport.minDepth,state.viewport.maxDepth)!=0 || taafix::viewport_ll() || taafix::winding_ll()){d.why=W_RASTER;return d;}
    if(draw_reads_overflow){d.why=W_OWN;return d;}
    for(Image *r:draw_reads)for(Image *a:p.images)if(r==a && (a->aspects&VK_IMAGE_ASPECT_COLOR_BIT)){d.why=W_OWN;return d;}
    if(!vs || !fs || state.shader[1] || state.shader[2] || state.shader[3] || fs->discards || fs->raster.sample_mask || (state.color.alpha_func!=8 && alpha_test_on())){d.why=W_SHADER;return d;}
    if(!vs->pos || !vs->pos->ok){d.why=W_NOPROG;return d;}
    if(state.color.logic_op!=3){d.why=W_ATTACH;return d;}   // a logic op other than COPY keeps / mixes the old texels (the old proof never looked)
    bool fm_used=false;   // [DOS2] NR-FRAME DM_NR_COVER_FMASK: a mask short of 0xF accepted because it covers every component of the format
    for(int i=0;i<state.rt.n;i++)if(state.rt.color[i]){
        const unsigned m4=(state.mask.mask>>(4*i))&15;
        if(!(fs->raster.outputs>>i&1) || (state.color.blend_en>>i&1) || state.rt.color[i]->d.target==2){d.why=W_ATTACH;return d;}
        if(m4!=15){if(!use_fmask || !nr_fr_mask_full(m4,slot_components(p,i))){d.why=W_ATTACH;return d;}fm_used=true;}}
    if((state.rt.depth && ((state.ds.dt && state.ds.df!=8) || state.ds.st)) || state.bounds.enable){d.why=W_DS;return d;}
    if(state.ms.a2c || state.poly.mode!=2 || p.samples!=VK_SAMPLE_COUNT_1_BIT){d.why=W_STATE;return d;}
    if(condr::cg.mode>0 && condr::cg.kind!=CONDR_K_ON){d.why=W_COND;return d;}
    if((prim!=NR_CV_TRIANGLES && prim!=NR_CV_TRIANGLE_STRIP) || count<3 || count>NR_CV_MAX_VERTS){d.why=W_PRIM;return d;}
    const nr_cv_prog &pr=*vs->pos;
    // the inputs the program reads: float formats only, per-vertex streams, bytes from the snapshots this draw hands to the GPU
    struct In{const uint8_t *base=nullptr;uint64_t size=0,stride=0,offset=0;int nc=0;} in[32];
    for(uint32_t m=pr.in_mask;m;m&=m-1){const unsigned loc=unsigned(__builtin_ctz(m));if(loc>=16){d.why=W_INPUT;return d;}
        va_st a;memcpy(&a,&state.attrib[loc],4);const VkFormat f=a.fmt?format(a.fmt):VK_FORMAT_UNDEFINED;
        const int nc=f==VK_FORMAT_R32_SFLOAT?1:f==VK_FORMAT_R32G32_SFLOAT?2:f==VK_FORMAT_R32G32B32_SFLOAT?3:f==VK_FORMAT_R32G32B32A32_SFLOAT?4:0;
        if(!nc || a.stream>=16 || !state.stream[a.stream].stride || state.stream[a.stream].divisor){d.why=W_INPUT;return d;}
        const Buffer *vb=nullptr;for(auto &b:vbufs)if(b.first==a.stream)vb=b.second;
        if(!vb || !vb->mapped){d.why=W_BUFFER;return d;}
        in[loc].base=static_cast<const uint8_t*>(vb->mapped);in[loc].size=vb->size;in[loc].stride=state.stream[a.stream].stride;in[loc].offset=a.offset;in[loc].nc=nc;}
    if(indexed && (!ib || !ib->mapped)){d.why=W_BUFFER;return d;}
    // its uniform blocks: the guest ranges bound to the vertex stage now (what descriptors() snapshots for this draw)
    const uint8_t *ub[NR_CV_MAX_UBO]{};size_t ubn[NR_CV_MAX_UBO]{};
    for(unsigned u=0;u<pr.nubo;u++){const xbg_shader_binding *sb=nullptr;
        for(auto &b:vs->bindings)if(b.binding==pr.ubo[u] && b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.source==XBG_SOURCE_UNIFORM && b.count==1 && b.slot<16)sb=&b;
        if(!sb){d.why=W_UBO;return d;}
        const NVNbufferRange &r=state.ub[nvn_stage[0]][sb->slot];const size_t bound=std::min<size_t>(r.size,sb->bytes?sb->bytes:65536);
        // refused: the host does not hand these guest bytes to the GPU as they are (UI shift patch, a GPU result not read back yet)
        if(!use_ubo || !r.address || bound<pr.ubo_bytes[u] || (g_ui_moff!=0 && r.address==g_ui_moff_addr) || overlaps_pending(r.address,bound)){d.why=W_UBO;return d;}
        ub[u]=reinterpret_cast<const uint8_t*>(uintptr_t(r.address));ubn[u]=bound;}
    const size_t isz=itype==VK_INDEX_TYPE_UINT16?2:4;
    float P[NR_CV_MAX_VERTS][4];
    for(int k=0;k<count;k++){
        int64_t vid;
        if(indexed){if(uint64_t(k+1)*isz>ib->size){d.why=W_BUFFER;return d;}const uint8_t *q=static_cast<const uint8_t*>(ib->mapped)+size_t(k)*isz;
            uint32_t x=0;if(isz==2){uint16_t h;memcpy(&h,q,2);x=h;}else memcpy(&x,q,4);vid=int64_t(x)+indexed->base_vertex;}
        else vid=int64_t(first)+k;
        if(vid<0){d.why=W_BUFFER;return d;}
        float v[32][4];
        for(uint32_t m=pr.in_mask;m;m&=m-1){const unsigned loc=unsigned(__builtin_ctz(m));const In &x=in[loc];
            const uint64_t off=uint64_t(vid)*x.stride+x.offset;if(off+4u*unsigned(x.nc)>x.size){d.why=W_BUFFER;return d;}
            v[loc][0]=v[loc][1]=v[loc][2]=0;v[loc][3]=1;memcpy(v[loc],x.base+off,4*size_t(x.nc));}
        if(!nr_cv_eval(&pr,v,ub,ubn,P[k])){d.why=W_UBO;return d;}
    }
    const VkRect2D sr=scissor(p);
    const int g=nr_cv_cover(P,count,prim,int(state.poly.cull),state.poly.front==1,tv.x,tv.y,tv.width,tv.height,sr.offset.x,sr.offset.y,sr.extent.width,sr.extent.height,p.width,p.height,&d.r);
    d.exact=pr.exact!=0;d.fmask=fm_used;
    d.why=g==NR_CVG_OK?(nr_cv_rect_full(d.r,p.width,p.height)?W_FULL:W_AREA):g==NR_CVG_VERTEX?W_VERTEX:g==NR_CVG_EMPTY?W_EMPTY:g==NR_CVG_CULLED?W_CULLED:g==NR_CVG_PRIM || g==NR_CVG_ARGS?W_PRIM:W_GEOMETRY;
    return d;
}
// draw(), the pass-begin site, in place of the old expression. nb: the NB plan begins this pass (nbrp::cur is its decision; nbrp_begin
// follows), else begin_open follows. gate: the host does not rewrite this draw (no GPU-indirect draw, no quad indices, no UI shift, at least
// one instance, A/B target "noload" on). Returns "covered" for the caller; an area decision is left in `armed` for nbrp_begin.
bool at_begin(const Pass &p,bool nb,bool gate,int prim,int first,int count,const p_draw_idx *indexed,const std::vector<std::pair<uint32_t,Buffer*>> &vbufs,const Buffer *ib,VkIndexType itype){
    const bool timed=!(s.begins&15);const uint64_t t0=timed?timing_clock():0;
    armed=false;poison_mask=0;++s.begins;if(nb)++s.nb;
    const bool plan=!nb || nbrp::cur.cover!=0;
    const bool old=plan && gate && p.pass_noload && first_draw_covers(p,prim,first,count,indexed,vbufs,ib,itype);
    if(old)++s.old_yes;
    Dec d;
    if(!plan)d.why=W_PLAN;else if(!gate)d.why=W_GATE;else d=decide(p,prim,first,count,indexed,vbufs,ib,itype);
    // what the decision is worth: colour attachments it would begin DONT_CARE x pixels
    uint32_t natt=0;if(d.why==W_FULL || d.why==W_AREA){if(nb)natt=unsigned(__builtin_popcount(nbrp::cur.cover&~nbrp::cur.clear&~nbrp::cur.dc));else for(auto *im:p.images)if(im->aspects==VK_IMAGE_ASPECT_COLOR_BIT)++natt;}
    const uint64_t px=uint64_t(d.r.x1-d.r.x0)*uint64_t(d.r.y1-d.r.y0),full_px=uint64_t(p.width)*p.height;
    if(d.why==W_AREA && (px*1000>full_px*(1000-k_min_save) || !natt))d.why=W_SMALL;
    ++s.why[d.why];
    if(d.why==W_FULL){s.px_full+=px*natt;s.att_full+=natt;if(d.exact)++s.exact;else ++s.computed;}
    if((d.why==W_FULL || d.why==W_AREA) && d.fmask)++s.fmask;   // [DOS2] NR-FRAME: proofs only the format-aware mask rule allowed
    if(d.why==W_AREA){s.px_area+=px*natt;s.att_area+=natt;if(d.exact)++s.exact;else ++s.computed;}
    if(old && d.why!=W_FULL){++s.red;if(told<16){++told;xbg_logf("【红】[nr] cover check: the old proof covers a %ux%u pass (vs %s, %d vertices) and the new one refuses: %s",p.width,p.height,
        state.shader[0]?state.shader[0]->key.c_str():"-",count,why_name[d.why]);}}
    if(mode==2 && d.why>W_PLAN && told_why<24){++told_why;   // the first refusals in full: what a phone run needs to see
        xbg_logf("[nr] cover refusal: pass %ux%u vs %s fs %s prim %d count %d viewport %g,%g %gx%g: %s",p.width,p.height,state.shader[0]?state.shader[0]->key.c_str():"-",
            state.shader[4]?state.shader[4]->key.c_str():"-",prim,count,state.viewport.x,state.viewport.y,state.viewport.width,state.viewport.height,why_name[d.why]);}
    bool covered=old;
    if(mode==1 || mode==3){
        if(d.why==W_FULL){covered=true;++s.applied_full;if(mode==3)poison_mask=~0u;area={0,0,int32_t(p.width),int32_t(p.height)};}
        else if(d.why==W_AREA && !old){
            // an area pass: only through the NB plan's begin (DM_NB_UIAREA reduces begin_open passes only), never under a poison mode that clears the whole target
            if(use_area && nb && store_hist_mode!=3 && nb_rp_mode!=3 && nrrp::mode!=3){armed=true;area=d.r;if(mode==3)poison_mask=~0u;}
            else ++s.area_off;}}
    if(timed){s.ns+=timing_clock()-t0;++s.timed;}
    tick();return covered;
}
// nbrp_begin, after the plan's twin t (computed with the caller's `covered`): the area decision. Returns the cover attachments that begin
// DONT_CARE on the area (twin bits 32+k, NOT in t.dc: the bookkeeping keeps them LOADed) and forces STORE.
uint32_t area_twin(const nb_rp_dec &dn,nb_rp_twin_out &t){
    if(!armed)return 0;
    const nb_rp_twin_out ta=nb_rp_twin(&dn,0,1);const uint32_t extra=ta.dc&~t.dc;
    if(!extra){armed=false;++s.area_off;return 0;}
    t.mask&=~(0x1ffull<<18);   // a restart must find what the pass drew so far
    return extra;}
// nbrp_begin, the pass begun: remember the area pass / poison what a proof left undefined. dc: attachments DONT_CARE by a proof.
void begun(const Pass &p,VkCommandBuffer cmd,uint32_t dc,bool area_applied){
    if(area_applied){red.on=true;red.cmd=cmd;red.pass=p.pass;red.fb=p.framebuffer;red.area=area;++s.applied_area;}
    armed=false;
    if(mode==3 && poison_mask && dc && area.x1>area.x0 && area.y1>area.y0){VkClearRect r{{{area.x0,area.y0},{uint32_t(area.x1-area.x0),uint32_t(area.y1-area.y0)}},0,1};
        for(uint32_t k=0;k<p.images.size() && k<9;k++)if((dc>>k&1u) && p.att_slot[k]!=8){VkClearAttachment a{};a.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;a.colorAttachment=p.att_slot[k];
            a.clearValue.color.float32[0]=1;a.clearValue.color.float32[2]=1;a.clearValue.color.float32[3]=1;cmd_clear_attachments(cmd,1,&a,&r);++s.poisoned;}}
    poison_mask=0;}
// draw(), before the open-pass check (red.on): a draw of the area pass that may touch pixels outside its render area restarts it.
// plain: no host UI shift; tri: filled triangles (clipped to the viewport; lines / points may leave it by their width).
void pre(const Pass &p,VkCommandBuffer cmd,bool plain,bool tri){
    if(!(open_pass.cmd==cmd && open_pass.pass==p.pass && open_pass.framebuffer==p.framebuffer && continues(cmd))){red.on=false;return;}
    nb_ua_rect r=nb_ua_full(p.width,p.height);
    if(plain){VkViewport v{};const VkRect2D sc=scissor(p);
        if(xbg_nvn_viewport(&v,state.viewport.x,state.viewport.y,state.viewport.width,state.viewport.height,state.viewport.minDepth,state.viewport.maxDepth)!=0)r=nb_ua_none();   // draw() skips it
        else r=nb_ua_draw_rect(v.x,v.y,v.width,v.height,sc.offset.x,sc.offset.y,sc.extent.width,sc.extent.height,p.width,p.height,tri?0:4);}
    const nb_ua_rect a{red.area.x0,red.area.y0,red.area.x1,red.area.y1};
    if(!nb_ua_contains(a,r)){end_open_pass();red.on=false;++s.restart_draw;}}   // the host begins it again: full area, LOAD
// clear path, before its open-pass check (red.on): r = the clear rect
void before_clear(const Pass &p,const VkRect2D &r,VkCommandBuffer cmd){
    if(!(open_pass.cmd==cmd && open_pass.pass==p.pass && open_pass.framebuffer==p.framebuffer && continues(cmd))){red.on=false;return;}
    const nb_ua_rect c{r.offset.x,r.offset.y,int32_t(r.offset.x+int32_t(r.extent.width)),int32_t(r.offset.y+int32_t(r.extent.height))},a{red.area.x0,red.area.y0,red.area.x1,red.area.y1};
    if(!nb_ua_contains(a,c)){end_open_pass();red.on=false;++s.restart_clear;}}
void init(XbgGpuFamily fam){const char *e=getenv("DM_NR_COVER");mode=e&&*e?atoi(e):xbg_nr_cover_default(fam);if(mode<0 || mode>3)mode=0;
    if(const char *u=getenv("DM_NR_COVER_UBO"))use_ubo=atoi(u)!=0;
    if(const char *a=getenv("DM_NR_COVER_AREA"))use_area=atoi(a)!=0;
    if(const char *m=getenv("DM_NR_COVER_FMASK"))use_fmask=atoi(m)!=0;   // [DOS2] NR-FRAME
    if(mode){draw_reads_on=true;   // the proof refuses a first draw that samples its own target
        xbg_logf("[nr] cover DM_NR_COVER=%d: %s (position programs of the vertex shaders, pixel-centre coverage with a %.2f px margin; line every 10 s)",mode,
            mode==2?"count only - the old proof decides, the new one is computed and compared":mode==3?"the proof decides; proven loads are DONT_CARE and poisoned magenta inside the proven rect":
            "the proof decides: whole target = load DONT_CARE, a part (dynamic resolution) = render area + load DONT_CARE",NR_CV_MARGIN);
        xbg_logf("[nr] cover: positions computed from uniforms %s (DM_NR_COVER_UBO), area passes %s (DM_NR_COVER_AREA), write masks %s (DM_NR_COVER_FMASK, NR-FRAME)",use_ubo?"proven":"refused",use_area?"on":"off",
            use_fmask?"full when they cover every component of the format":"full only at 0xF (old rule)");}}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;frames0=stats.presents.load();return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const uint64_t fr=stats.presents.load();const double f=fr>frames0?double(fr-frames0):1.;frames0=fr;
    char why[640];int n=0;why[0]=0;for(int k=W_PLAN;k<W_N;k++)if(s.why[k] && n<int(sizeof why))n+=snprintf(why+n,sizeof why-size_t(n),"%s%s %.2f",n?", ":"",why_name[k],s.why[k]/f);
    xbg_logf("[nr] cover DM_NR_COVER=%d: %llu frames; a frame: passes begun by a draw %.1f (by the NB plan %.1f); old proof yes %.2f; new proof: whole target %.2f (%.1f attachments = %.1f MB of load at 4 B/px), "
        "a part %.2f (%.1f attachments = %.1f MB); positions copied %.2f / computed %.2f; refused: %s; %s whole %.2f, area %.2f (area not usable here %.2f, twin failed %.2f), restarts by a draw %.3f / by a clear %.3f, "
        "poisoned %.2f; never expected (old yes, new no) %llu; proof %.0f ns (1 in 16); format-aware masks (DM_NR_COVER_FMASK=%d) %.2f",
        mode,(unsigned long long)f,s.begins/f,s.nb/f,s.old_yes/f,s.why[W_FULL]/f,s.att_full/f,s.px_full*4/f/1048576.,s.why[W_AREA]/f,s.att_area/f,s.px_area*4/f/1048576.,
        s.exact/f,s.computed/f,n?why:"none",mode==2?"would apply:":"applied:",mode==2?s.why[W_FULL]/f:s.applied_full/f,mode==2?s.why[W_AREA]/f:s.applied_area/f,s.area_off/f,s.twin_fail/f,
        s.restart_draw/f,s.restart_clear/f,s.poisoned/f,(unsigned long long)s.red,s.timed?double(s.ns)/double(s.timed):0.,int(use_fmask),s.fmask/f);
    s=Stats{};}
}
