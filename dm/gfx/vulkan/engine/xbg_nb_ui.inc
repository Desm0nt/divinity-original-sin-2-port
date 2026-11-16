// [DOS2] NB-18b-2: the UI / post-tail audit, count only (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-18a findings" п. 4, "NB-18b-2 results").
// Included from xbg_nb.inc; one hook in draw() (xbg_game.cpp: `if(nbui::mode)nbui::draw();` after the draw is recorded). The old path runs
// unchanged; nothing here touches Vulkan state. Independent of DM_NB (needs no NB records).
//   DM_NB_UIPASS=2  every frame, at its CombineUI* draw: the UI segment (passes from the first draw into the combine's Tex to the last one
//                   before the combine), Iggy filter chains (runs of passes inside the segment not drawing into Tex), blend classes of the
//                   draws into Tex and into the filter targets (nb_ui.h), passes with depth / stencil attached / tested / written, scissor
//                   union of the filter passes, readers of Tex other than the combine, c5 Params.y / ViewportOffset of the combine.
//                   Line "[nb] ui: ..." every 10 s.
//   DM_NB_POST=2    the post tail walked back from the combine's Base by shader keys ([LUT] Sight [FXAA | PPDefault] [Vignette] Combine,
//                   1:1 edges, no other reader of an intermediate, one size). Lines "[nb] post: chain <chain> ok N | <reason> N" every 10 s.
// Passes here = runs of draws into the same render-target set (the guest's view; a host pass split inside one is not seen). Image ids =
// texture storage addresses (tex_t::mem). Bindings of CombineUI* (NB-18a): Base = NVN texture slot 0, Tex = slot 1, c5 = vk binding 22.
// Cost when on: per draw a few texture-pool lookups (fragment bindings) and a key compare; off (default) one branch per draw.
#include "../../nb/nb_ui.h"
namespace nbui{
int mode=-1;   // -1 not read yet, 0 off, bit 0 DM_NB_UIPASS=2, bit 1 DM_NB_POST=2
nb_ui_pass P[NB_UI_MAXP];char fsk[NB_UI_MAXP][13];int np=0;
bool after_comb=false;uint64_t comb_rt=0;uint64_t next_ns=0;
struct Acc{uint64_t frames,overflow,seg,ui_passes,ui_draws,flt_passes,flt_draws,chains,chains_max,flt_rts,rt_reused,between,
    blend_ui[NB_UIB_N],blend_flt[NB_UIB_N],ds_att,ds_used,dt,dw,st,sw,flt_ds_unused,flt_area,flt_rt_area,flt_small,flt_noin,ui_readers,
    c5,py_one,direct,direct_blend,direct_py,direct_rd;float py_min,py_max,vp[2];};
Acc a{};std::map<std::string,uint64_t> why_ui,post;
void reset_acc(){a=Acc{};a.py_min=1e30f;a.py_max=-1e30f;why_ui.clear();post.clear();}
void init(){
    auto rd=[](const char *n){const char *e=getenv(n);const int v=e && *e?atoi(e):0;
        if(v && v!=2 && strcmp(n,"DM_NB_POST"))xbg_logf("[nb] %s=%d: only 2 (audit, count only) exists so far (the applied mode is NB-18b-7): off",n,v);return v==2;};   // DM_NB_POST 1 / 3: xbg_nb_post.inc
    mode=(rd("DM_NB_UIPASS")?1:0)|(rd("DM_NB_POST")?2:0);reset_acc();
    if(mode)xbg_logf("[nb] ui/post audit on (DM_NB_UIPASS=%d DM_NB_POST=%d): counted at every CombineUI* draw, lines every 10 s; the old path executes",
        mode&1?2:0,mode&2?2:0);}
uint64_t mem(const tex_t *t){return t?uint64_t(uintptr_t(t->mem)):0;}
const RegisteredTexture *tex_rec(uint64_t handle){const int id=int(handle&0xfffff);if(!id || !state.texture_pool.pool)return nullptr;
    if(state.texture_pool.n>0 && id>=state.texture_pool.n)return nullptr;
    const std::map<int,RegisteredTexture> *tt=&legacy_textures;auto f=textures.find(pool_key(&state.texture_pool));if(f!=textures.end())tt=&f->second;
    auto t=tt->find(id);return t==tt->end()?nullptr:&t->second;}
uint64_t tex_mem(uint32_t slot){if(slot>=128)return 0;const RegisteredTexture *r=tex_rec(state.tex[1][slot]);   // [NB-18b-4] NVN stage 1 = fragment (was [4], tess eval: always empty)
   return r?uint64_t(uintptr_t(r->texture.mem)):0;}
uint64_t rt_key(){uint64_t h=uint64_t(state.rt.n)*0x9E3779B97F4A7C15ull^mem(state.rt.depth);
    for(int i=0;i<state.rt.n && i<8;i++)h=(h^mem(state.rt.color[i]))*0x100000001B3ull;return h;}
bool same_rt(const nb_ui_pass &q){if(q.ncolor!=uint32_t(std::clamp(state.rt.n,0,8)) || q.depth!=mem(state.rt.depth))return false;
    for(uint32_t i=0;i<q.ncolor;i++)if(q.color[i]!=mem(state.rt.color[i]))return false;return true;}
void tick();
void frame(const Shader &fs){
    nb_ui_comb c{};
    for(const auto &b:fs.bindings){
        if(b.source==XBG_SOURCE_TEXTURE && b.slot==0)c.base=tex_mem(b.slot);
        else if(b.source==XBG_SOURCE_TEXTURE && b.slot==1)c.ui=tex_mem(b.slot);
        else if(b.source==XBG_SOURCE_UNIFORM && b.binding==22 && b.slot<16){const NVNbufferRange &u=state.ub[1][b.slot];
            if(u.address && u.size>=32){float v[8];memcpy(v,reinterpret_cast<const void*>(uintptr_t(u.address)),32);   // what the draw's snapshot copied
                c.have_c5=1;c.vp_off[0]=v[0];c.vp_off[1]=v[1];for(int k=0;k<4;k++)c.params[k]=v[4+k];}}}
    nb_ui_res r;nb_ui_analyze(P,np,&c,&r);++a.frames;
    if(r.ui_why)++why_ui[r.ui_why];
    else{++a.seg;a.ui_passes+=r.ui_passes;a.ui_draws+=r.ui_draws;a.flt_passes+=r.flt_passes;a.flt_draws+=r.flt_draws;a.chains+=r.chains;
        a.chains_max=std::max<uint64_t>(a.chains_max,r.chains);a.flt_rts+=r.flt_rts;a.rt_reused+=r.rt_reused;a.between+=r.between;
        for(int k=0;k<NB_UIB_N;k++){a.blend_ui[k]+=r.blend_ui[k];a.blend_flt[k]+=r.blend_flt[k];}
        a.ds_att+=r.ds_att;a.ds_used+=r.ds_used;a.dt+=r.dt;a.dw+=r.dw;a.st+=r.st;a.sw+=r.sw;a.flt_ds_unused+=r.flt_ds_unused;
        a.flt_area+=r.flt_area;a.flt_rt_area+=r.flt_rt_area;a.flt_small+=r.flt_small;a.flt_noin+=r.flt_noin;a.ui_readers+=r.ui_readers;
        bool exact=true;for(int k=0;k<NB_UIB_N;k++)if(r.blend_ui[k] && !nb_ui_blend_exact(k))exact=false;
        if(!exact)++a.direct_blend;else if(r.params_one!=1)++a.direct_py;else if(r.ui_readers)++a.direct_rd;else ++a.direct;}
    if(c.have_c5){++a.c5;if(r.params_one==1)++a.py_one;a.py_min=std::min(a.py_min,c.params[1]);a.py_max=std::max(a.py_max,c.params[1]);a.vp[0]=c.vp_off[0];a.vp[1]=c.vp_off[1];}
    std::string k=r.chain;k+=r.post_why?" | ":" ok";if(r.post_why){k+=r.post_why;if(r.post_bad_pass>=0 && r.post_bad_pass<np){k+=" (fs ";k+=fsk[r.post_bad_pass];k+=")";}}
    if(post.size()<24 || post.count(k))++post[k];
    tick();}
void draw(){
    if(mode<0){init();if(!mode)return;}
    const Shader *fs=state.shader[4];const int cls=fs?nb_ui_shader_class(fs->key.c_str()):NB_UISH_OTHER;
    if(after_comb){if(rt_key()==comb_rt)return;after_comb=false;}
    if(!np || !same_rt(P[np-1])){
        if(np==NB_UI_MAXP){++a.overflow;np=0;}   // a frame without a combine (loading, video): dropped
        nb_ui_pass &q=P[np];nb_ui_pass_init(&q);q.ncolor=uint32_t(std::clamp(state.rt.n,0,8));for(uint32_t i=0;i<q.ncolor;i++)q.color[i]=mem(state.rt.color[i]);
        q.depth=mem(state.rt.depth);const tex_t *t0=q.ncolor && state.rt.color[0]?state.rt.color[0]:state.rt.depth;
        if(t0){q.w=t0->d.width;q.h=t0->d.height;}snprintf(fsk[np],sizeof fsk[np],"%s",fs?fs->key.substr(0,12).c_str():"-");q.sh=cls;++np;}
    nb_ui_pass &q=P[np-1];++q.draws;q.sh_mask|=1u<<cls;
    {const blend_st &b=state.blend[0];const int k=state.rt.n<1?NB_UIB_NOCOLOR:nb_ui_blend_class(state.color.blend_en&1,b.src_rgb,b.dst_rgb,b.src_a,b.dst_a,b.eq_rgb,b.eq_a,state.mask.mask&15);++q.blend[k];}
    if(state.rt.depth){const ds_st &d=state.ds;q.dt|=uint8_t(d.dt);q.dw|=uint8_t(d.dt && d.dw);q.st|=uint8_t(d.st);
        const bool ops=d.fail_f!=1 || d.zfail_f!=1 || d.zpass_f!=1 || d.fail_b!=1 || d.zfail_b!=1 || d.zpass_b!=1;
        q.sw|=uint8_t(d.st && ops && (state.stencil_write[0] || state.stencil_write[1]));}
    {const VkViewport &v=state.viewport;int32_t x0=int32_t(v.x),x1=int32_t(v.x+v.width),y0=int32_t(std::min(v.y,v.y+v.height)),y1=int32_t(std::max(v.y,v.y+v.height));
        const VkRect2D &s=state.scissor;const int64_t sx1=int64_t(s.offset.x)+int64_t(s.extent.width),sy1=int64_t(s.offset.y)+int64_t(s.extent.height);
        x0=std::max<int32_t>({x0,s.offset.x,0});y0=std::max<int32_t>({y0,s.offset.y,0});
        x1=int32_t(std::min<int64_t>({int64_t(x1),sx1,int64_t(q.w)}));y1=int32_t(std::min<int64_t>({int64_t(y1),sy1,int64_t(q.h)}));nb_ui_add_rect(&q,x0,y0,x1,y1);}
    if(fs)for(const auto &b:fs->bindings)if(b.source==XBG_SOURCE_TEXTURE)nb_ui_add_read(&q,tex_mem(b.slot));
    if(nb_ui_is_combine(cls)){frame(*fs);np=0;after_comb=true;comb_rt=rt_key();}}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const double s=a.seg?double(a.seg):1.,fr=a.frames?double(a.frames):1.;
    if(mode&1){std::string w;for(auto &x:why_ui){char b[96];snprintf(b,sizeof b,", %s %llu",x.first.c_str(),(unsigned long long)x.second);w+=b;}
        std::string bu,bf;for(int k=0;k<NB_UIB_N;k++){char b[64];snprintf(b,sizeof b,"%s%s %llu",k?", ":"",nb_ui_blend_name[k],(unsigned long long)a.blend_ui[k]);bu+=b;
            snprintf(b,sizeof b,"%s%s %llu",k?", ":"",nb_ui_blend_name[k],(unsigned long long)a.blend_flt[k]);bf+=b;}
        xbg_logf("[nb] ui: %llu frames (dropped without a combine %llu), UI segment in %llu%s; a frame: UI passes %.1f (draws %.1f), filter passes %.1f (draws %.1f), "
            "chains %.2f (max %llu), filter targets %.1f, reused across chains %.1f, passes between the segment and the combine %.1f; "
            "blend draws into Tex: %s; into filter targets: %s; DS attached %.1f, used %.1f (depth test %.1f, write %.1f, stencil test %.1f, write %.1f), "
            "filter passes with DS unused %.1f; filter scissor union %.1f %% of the target (passes under 25 %% %.1f, without inputs %.1f); "
            "Tex read by other passes %.2f; c5 Params.y == 1 in %llu of %llu (min %g max %g), ViewportOffset %g,%g; "
            "UI straight into the final pass: equal %llu, blend outside over %llu, Params.y != 1 %llu, Tex read elsewhere %llu",
            (unsigned long long)a.frames,(unsigned long long)a.overflow,(unsigned long long)a.seg,w.c_str(),a.ui_passes/s,a.ui_draws/s,a.flt_passes/s,a.flt_draws/s,
            a.chains/s,(unsigned long long)a.chains_max,a.flt_rts/s,a.rt_reused/s,a.between/s,bu.c_str(),bf.c_str(),a.ds_att/s,a.ds_used/s,a.dt/s,a.dw/s,a.st/s,a.sw/s,
            a.flt_ds_unused/s,a.flt_rt_area?100.*double(a.flt_area)/double(a.flt_rt_area):0.,a.flt_small/s,a.flt_noin/s,a.ui_readers/s,
            (unsigned long long)a.py_one,(unsigned long long)a.c5,a.c5?double(a.py_min):0.,a.c5?double(a.py_max):0.,double(a.vp[0]),double(a.vp[1]),
            (unsigned long long)a.direct,(unsigned long long)a.direct_blend,(unsigned long long)a.direct_py,(unsigned long long)a.direct_rd);}
    if(mode&2)for(auto &x:post)xbg_logf("[nb] post: chain %s %llu (%.0f %%)",x.first.c_str(),(unsigned long long)x.second,100.*double(x.second)/fr);
    reset_acc();}
}
