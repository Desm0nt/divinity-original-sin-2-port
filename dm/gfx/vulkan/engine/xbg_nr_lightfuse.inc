// [DOS2] NR-LIGHTFUSE DM_NR_LIGHTFUSE (docs/dos2/nr/NR-LIGHTFUSE.md; pure C: dm/gfx/nr/nr_lightfuse.h; shader: the pack's
// "f.ClusteredLightingWithSSAO" = tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag -DLIGHTFUSE, proof tools/dos2_lightfuse.py).
// DeferredSky -> ClusteredLightingWithSSAO as ONE draw. The two passes are full screen and bandwidth bound on the Mi 14 (TU_DEBUG=sysmem):
// Light re-reads the G-buffer Sky has just read and reads Sky's Sp back. Here the Sky pass still begins and ends (its clears of Sp / Df,
// every plan / barrier / store path untouched) but its draw runs with rasterizer discard (a twin "<sky>#lfd" of its fragment shader, so the
// pipeline memos / the NR-09 lane / NB-13b key it apart); the Light draw gets the twin "<light>#lf": the fused module, the Light table + the
// extras of nr_lightfuse.h fed from the stash of the Sky draw (its 8 textures on spare slots 120..127, its c8 / c6 on its own uniform slots).
// The chain marks are NR-ONCHIP's (dm/gfx/nb/nb_onchip.h, front end hle_dos2_nb.c): nroc publishes the front-end mode for both.
//   DM_NR_LIGHTFUSE 0 (default, every family): nothing.
//   2  verify: every check at the Sky draw and at the Light draw, the twins and their modules built once; both draws as recorded; counts.
//   1  apply: Sky is skipped (rasterizer discard) only after an earlier chain passed every Light check (learned, nr_lf_sky); the Light
//      draw is the fused draw. A failed Light check after a skip: the recorded Light draws (Sky's output is missing that frame: "lost",
//      red line) and learning starts again (nr_lf_light).
//   3  = 1 + poison: when Sky was skipped, Sp is cleared to magenta in memory before the Light pass (nothing reads it in the fused chain; a
//      later reader of Sky's Sp would show magenta).
// Interactions (logged at init): DM_NR_ONCHIP's backend is OFF while DM_NR_LIGHTFUSE != 0 (one chain, one owner; its marks still come);
// DM_GFX_PASS_GMEM: the two passes of a fused chain stay in sysmem (pgm::note_lightfuse; counted here).
// The Light checks (nr_lf_light checks_ok): role Light after this submission's Sky stash; the ClusteredLightingWithSSAO bundle, VS + FS only, no
// discard / alpha test / alpha to coverage; one colour target = Sky's Df, no depth; colour 0 blended ONE, ONE, ADD (nr_lf_blend_ok); the same
// vertex shader bundle as Sky's (byte for byte) with the same vertex c-buffer bytes, vertex stream bytes, draw range, viewport and scissor
// (so the view ray at every pixel is Sky's); the same texture / sampler pools as at the Sky draw.
// Line "[nr] lightfuse ..." every 10 s while chains are seen; red lines 【红】 (first 16).
#include "../../nb/nb_onchip.h"
namespace nbrp{const p_nb_pass *span_hdr();}   // xbg_nb.inc (included later)
namespace nbui{const RegisteredTexture *tex_rec(uint64_t handle);}   // xbg_nb_ui.inc (included later)
namespace nrlf{
int mode=0,fog=0;bool fused_draw=false,fog_learned=false;uint32_t fog_mask=15;   // fog: DM_NR_LIGHTFUSE_FOG (0 / 2 verify / 1 apply); fog_mask: the learned Fog draw's colour 0 write mask
nr_lf_state ls{};
enum{SNAP=1024,VSNAP=256};
struct Stash{bool valid=false,skipped=false;uint32_t seq=0;int node=-1;uint64_t frame=0;
    uint64_t tex[NR_LF_NTEX]{};NVNbufferRange ub[2]{};uint32_t ub_slot[2]{};xbg_shader_binding xb[NR_LF_NX]{};
    const void *pool=nullptr,*spool=nullptr;int64_t pool_off=0,spool_off=0;std::string vs;
    std::vector<uint8_t> vub[2],vvb[4];int prim=0,first=0,count=0,indexed=0;uint32_t instances=0;VkViewport vp{};VkRect2D sc{};
    Image *sp=nullptr,*df=nullptr;const tex_t *sp_tex=nullptr;
    bool light_ok=false,fog_fused=false;uint32_t fog_mask=15;int light_node=-1;Image *depth_im=nullptr;uint64_t sp_h=0;std::vector<uint8_t> c16b,c15b;} st;   // fog: Light's view of the Fog inputs
// NR-LIGHTFUSE fog: after a skipped Fog draw its target C is not written; the draws that sample C until C's next write sample Sp instead
struct Redirect{bool on=false;Image *c=nullptr;uint64_t writer=0;uint32_t seq=0,sp_id=0;const void *pool=nullptr;int64_t pool_off=0;} rd;
enum{W_SKY_SHAPE=0,W_SKY_STASH,W_NO_STASH,W_LIGHT_FS,W_LIGHT_RT,W_BLEND,W_VS,W_VS_DATA,W_DRAW,W_POOL,W_TWIN,W_N};
enum{F_NOLIGHT=0,F_STAGES,F_RT,F_BLEND,F_MASK,F_BIND,F_TARGET,F_SCENE,F_DEPTH,F_CONST,F_VS,F_DRAW,F_POOL,F_N};
// The Fog shape check, one counter per condition (round 3 had one combined "Fog shape" counter: every phone Fog draw refused, cause hidden;
// it was the write mask, R G B on the B10G11R11 target, nr_lf_fog_mask_ok).
const char *fname_(int i){static const char *n[F_N]={"Fog without a checked Light of this submission","Fog stages (not VS + FS only, or rasterizer discard)",
    "Fog targets (not one colour without depth)","Fog colour 0 blended","Fog write mask (R G B not all written, or A not written on a target with alpha, "
    "or not the mask the fused Light wrote into Sp)","Fog bindings 16 / 10 / 22 / 28 missing or of another kind",
    "Fog's target not like Sp (format / size)","Fog does not read Light's Df","Fog's depth is not Light's","Fog's c16[4..6] / c15[12] differ from Light's",
    "another vertex shader / constants / streams than Light's","draw range / viewport / scissor differ","texture pool changed"};return i>=0 && i<F_N?n[i]:"?";}
const char *wname(int i){static const char *n[W_N]={"Sky draw shape (2 colours, no depth, VS + FS, the DeferredSky bundle)","Sky resources not all bound",
    "Light without this submission's Sky stash","Light fragment shader / stages / discard / alpha test","Light's target is not Sky's Df (or depth bound)",
    "Light's colour 0 is not blended ONE, ONE, ADD","another vertex shader than Sky's","vertex c-buffers / streams differ from Sky's (another view ray)",
    "draw range / viewport / scissor differ from Sky's","texture / sampler pool changed","fused or Sky twin not available"};return i>=0 && i<W_N?n[i]:"?";}
struct Stats{uint64_t sky=0,light=0,why[W_N]{},would=0,skipped=0,fused=0,lost=0,learn=0,poison=0,gmem=0,reds=0,changed=0,
    fogs=0,fog_ok=0,fog_why[F_N]{},fog_fused=0,fog_skipped=0,fog_back=0,redir=0,redir_draws=0;} s;
uint64_t frames0=0,next_ns=0;
void red(const char *fmt,...){const uint64_t n=++s.reds;if(n>16)return;char t[512];va_list a;va_start(a,fmt);vsnprintf(t,sizeof t,fmt,a);va_end(a);xbg_logf("【红】[nr] lightfuse %s",t);}
void init(){
    const char *e=getenv("DM_NR_LIGHTFUSE");int m=e && *e?atoi(e):xbg_nr_lightfuse_default(gpu_policy);
    if(m<0 || m>3){xbg_logf("【红】[nr] lightfuse DM_NR_LIGHTFUSE=%s is not 0 / 1 / 2 / 3: off",e);m=0;}
    mode=m;
    {const char *f=getenv("DM_NR_LIGHTFUSE_FOG");int v=f && *f?atoi(f):xbg_nr_lightfuse_fog_default(gpu_policy);if(v<0 || v>2){xbg_logf("【红】[nr] lightfuse DM_NR_LIGHTFUSE_FOG=%s is not 0 / 1 / 2: off",f);v=0;}
     fog=m?v:0;if(v && !m && f && *f)xbg_logf("[nr] lightfuse DM_NR_LIGHTFUSE_FOG=%d needs DM_NR_LIGHTFUSE: off",v);
     if(fog)xbg_logf("[nr] lightfuse DM_NR_LIGHTFUSE_FOG=%d (%s): DeferredFog of the stored Df inside the fused Light draw (g.ClusteredLightingWithSSAO, out 1 = Sky's Sp); "
        "the Fog draw is skipped (no pass begins) and the draws that sample its target until that target's next write sample Sp; a Fog check that fails draws "
        "the Fog as recorded (Df still holds Light's result)",fog,fog==2?"verify: checked and counted":mode==1 || mode==3?"apply after one checked Fog":"no effect: DM_NR_LIGHTFUSE is 2");}
    if(!m)return;
    xbg_logf("[nr] lightfuse DM_NR_LIGHTFUSE=%d (%s; family %s, default %d): DeferredSky -> ClusteredLightingWithSSAO as one draw (f.ClusteredLightingWithSSAO "
        "of native-nr.xgspack; Sky's pass keeps its clears, its draw runs with rasterizer discard); chain marks from the front end (NR-ONCHIP's "
        "planner); DM_NR_ONCHIP's backend is off while this is on; DM_GFX_PASS_GMEM keeps the fused chain's two passes in sysmem",m,
        m==2?"verify: checked and counted, both draws as recorded":m==1?"apply after one checked chain":"apply + magenta poison of the skipped Sp",
        xbg_gpu_family_name(gpu_policy),xbg_nr_lightfuse_default(gpu_policy));}
int fe_mode(int onchip){return mode?(onchip>0?onchip:1):onchip;}   // what nroc publishes to the front end (marks while either is on)
const xbg_shader_binding *entry(const Shader &sh,uint32_t binding){for(const auto &b:sh.bindings)if(b.binding==binding)return &b;return nullptr;}
// a twin: the fields of fs as nroc::twin copies them (bindings / module given)
Shader *make(const Shader &fs,const std::string &key,VkShaderModule m){Shader *t=new Shader();t->key=key;t->stage=fs.stage;t->module=m;t->bindings=fs.bindings;
    t->inputs=fs.inputs;t->input_classes=fs.input_classes;t->c1=fs.c1;t->discards=fs.discards;t->empty_fs=fs.empty_fs;t->raster=fs.raster;t->pg_coord=fs.pg_coord;
    t->storage_writes=fs.storage_writes;t->vo_read=fs.vo_read;t->vo_decl=fs.vo_decl;return t;}
std::map<std::string,Shader*> sky_twins,light_twins;VkShaderModule lf_mods[2]{};int lf_states[2]{};
Shader *sky_twin(const Shader &fs){auto it=sky_twins.find(fs.key);if(it!=sky_twins.end())return it->second;
    Shader *t=fs.module?make(fs,fs.key+"#lfd",fs.module):nullptr;sky_twins.emplace(fs.key,t);
    if(t){++s.changed;xbg_logf("[nr] lightfuse %s -> %s (the same module, drawn with rasterizer discard: Sky's pass begins and clears, no fragment runs)",fs.key.c_str(),t->key.c_str());}
    return t;}
VkShaderModule fused_module(char v='f'){
    const int vi=v=='g';VkShaderModule &lf_mod=lf_mods[vi];if(lf_states[vi])return lf_mod;lf_states[vi]=1;
    const int i=nr_sh_find_name("ClusteredLightingWithSSAO");if(i<0){red("no ClusteredLightingWithSSAO in the NR-04 table");return lf_mod;}
    const std::string key=nr_sh_table[i].key;std::vector<uint32_t> wd;uint32_t stage=0;bool ok;
    {xpack::nrsh::oc_var=v;ok=warm::bundle_words(key,wd,stage,"nr-lf");xpack::nrsh::oc_var=0;}
    if(!ok || stage!=4){red("%s: no %c.ClusteredLightingWithSSAO bundle that passes the head / interface / descriptor checks (native-nr.xgspack of setup stage 8, run setup again): no fusion",key.c_str(),v);return lf_mod;}
    if(!module_gap(wd,stage).empty() || bundle_module(wd,&lf_mod,key+(vi?"#lfg":"#lf"))!=VK_SUCCESS){lf_mod=VK_NULL_HANDLE;red("%s: the fused module could not be made: no fusion",key.c_str());return lf_mod;}
    ++s.changed;xbg_logf("[nr] lightfuse fused module %c for %s: %zu words (DeferredSky inside%s; extras: 8 textures on slots 120..127, Sky's c8 / c6)",v,key.c_str(),wd.size(),vi?", DeferredFog on out 1":"");return lf_mod;}
// The Light twin: the fused module, the Light table + the extras (Sky's entries with the fused binding and the spare / Sky's own slot).
Shader *light_twin(const Shader &fs,char v='f'){const std::string tk=fs.key+(v=='g'?"#lfg":"#lf");auto it=light_twins.find(tk);if(it!=light_twins.end())return it->second;
    Shader *t=nullptr;const VkShaderModule m=fused_module(v);
    if(m){bool clash=false;for(const auto &b:fs.bindings){if(b.source==XBG_SOURCE_TEXTURE && b.slot+b.count>NR_LF_SPARE_TEX)clash=true;
            for(int j=0;j<2;j++)if(b.source==XBG_SOURCE_UNIFORM && b.slot==st.ub_slot[j])clash=true;
            for(int k=0;k<NR_LF_NX;k++)if(b.binding==nr_lf_x[k].fused)clash=true;}
        if(clash)red("%s: the Light table uses a spare texture slot, Sky's uniform slot or an extra binding: no fusion",fs.key.c_str());
        else{t=make(fs,tk,m);for(int k=0;k<NR_LF_NX;k++)t->bindings.push_back(st.xb[k]);++s.changed;
            xbg_logf("[nr] lightfuse %s -> %s (%zu bindings: the Light table + Sky's 8 textures on slots 120..127 and c8 / c6 on uniform slots %u / %u)",
                fs.key.c_str(),t->key.c_str(),t->bindings.size(),st.ub_slot[0],st.ub_slot[1]);}}
    light_twins.emplace(tk,t);return t;}
bool same_bundle(const std::string &a,const std::string &b){static std::map<std::pair<std::string,std::string>,bool> m;if(a==b)return true;
    auto k=std::make_pair(a,b);auto it=m.find(k);if(it!=m.end())return it->second;
    std::vector<uint8_t> x,y;bool fp=false;const bool eq=xpack::read_translated(a,x,fp) && xpack::read_translated(b,y,fp) && !x.empty() && x==y;
    m[k]=eq;xbg_logf("[nr] lightfuse vertex shaders %s / %s: bundles %s",a.c_str(),b.c_str(),eq?"equal byte for byte (one view ray)":"DIFFER: no fusion");return eq;}
void snap(std::vector<uint8_t> &o,const NVNbufferRange &r,size_t cap){o.clear();if(!r.address || !r.size)return;o.resize(size_t(std::min<uint64_t>(r.size,cap)));memcpy(o.data(),reinterpret_cast<const void*>(uintptr_t(r.address)),o.size());}
bool same_snap(const std::vector<uint8_t> &o,const NVNbufferRange &r){if(o.empty())return !r.address || !r.size;if(!r.address || r.size<o.size())return false;
    return !memcmp(o.data(),reinterpret_cast<const void*>(uintptr_t(r.address)),o.size());}
bool is(const Shader *fs,const char *name){return fs && fs->key.size()>=16 && nr_sh_find_key(fs->key.substr(0,16).c_str())==nr_sh_find_name(name) && nr_sh_find_name(name)>=0;}
struct Guard{Shader *fs=nullptr;bool disc=false;uint32_t was_disc=0;bool ov=false;uint64_t tex[NR_LF_NTEX]{};NVNbufferRange ub[2]{};uint32_t ub_slot[2]{};
    bool mrt=false;p_set_rt rt{};uint32_t blend_en=0,mask=0;int nre=0;uint32_t re_slot[16]{};uint64_t re_h[16]{};
    ~Guard(){if(fs)state.shader[4]=fs;if(disc)state.rast_discard=was_disc;
        if(mrt){state.rt=rt;state.color.blend_en=blend_en;state.mask.mask=mask;}
        if(nre){for(int i=nre-1;i>=0;i--)state.tex[1][re_slot[i]]=re_h[i];++tex_gen[1];}
        if(ov){for(int i=0;i<NR_LF_NTEX;i++)state.tex[1][NR_LF_SPARE_TEX+i]=tex[i];for(int j=0;j<2;j++)state.ub[1][ub_slot[j]]=ub[j];++tex_gen[1];++buf_gen[1];}
        fused_draw=false;}};
void poison(const tex_t *t){Image &im=get_image(t,false);Commands c;im.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkClearColorValue m{};m.float32[0]=1;m.float32[2]=1;m.float32[3]=1;
    VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,im.layers};vkCmdClearColorImage(c.command,im.image,im.layout,&m,1,&r);im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();
    im.gpu_written=true;provenance_write(im,__LINE__);++s.poison;}
int fail(int w){++s.why[w];return 0;}
// The Sky draw (role Sky): stash + checks; skip (rasterizer discard twin) when nr_lf_sky says so.
void sky(Guard &g,const p_nb_pass &h,int prim,int first,int count,bool indexed,uint32_t instances){
    ++s.sky;if(st.skipped && st.valid){nr_lf_orphan(&ls);++s.lost;red("the Sky draw of seq %u was skipped and no fused Light draw followed: its output is missing there",st.seq);}
    st=Stash{};Shader *fs=state.shader[4];
    if(!is(fs,"DeferredSky") || !state.shader[0] || state.shader[1] || state.shader[2] || state.shader[3] || state.rast_discard || state.rt.n!=2 || !state.rt.color[0] || !state.rt.color[1] || state.rt.depth){fail(W_SKY_SHAPE);return;}
    for(int k=0;k<NR_LF_NX;k++){const xbg_shader_binding *b=entry(*fs,nr_lf_x[k].sky);
        if(!b || (nr_lf_x[k].kind==NR_LF_TEX?b->source!=XBG_SOURCE_TEXTURE || b->count!=1 || b->slot>=128:b->source!=XBG_SOURCE_UNIFORM || b->slot>=16)){fail(W_SKY_SHAPE);return;}
        st.xb[k]=*b;st.xb[k].binding=nr_lf_x[k].fused;
        if(nr_lf_x[k].kind==NR_LF_TEX){st.tex[k]=state.tex[1][b->slot];st.xb[k].slot=NR_LF_SPARE_TEX+uint32_t(k);if(!nbui::tex_rec(st.tex[k])){fail(W_SKY_STASH);return;}}
        else{const int j=k-NR_LF_NTEX;st.ub_slot[j]=b->slot;st.ub[j]=state.ub[1][b->slot];if(!st.ub[j].address || !st.ub[j].size){fail(W_SKY_STASH);return;}}}
    st.pool=state.texture_pool.pool;st.pool_off=state.texture_pool.off;st.spool=state.sampler_pool.pool;st.spool_off=state.sampler_pool.off;
    st.vs=state.shader[0]->key;snap(st.vub[0],state.ub[0][0],SNAP);snap(st.vub[1],state.ub[0][1],SNAP);for(int i=0;i<4;i++)snap(st.vvb[i],state.vb[i],VSNAP);
    st.prim=prim;st.first=first;st.count=count;st.indexed=indexed;st.instances=instances;st.vp=state.viewport;st.sc=state.scissor;
    st.sp=&get_image(state.rt.color[0],false);st.df=&get_image(state.rt.color[1],false);st.sp_tex=state.rt.color[0];
    st.seq=h.seq;st.node=h.node;st.frame=stats.presents.load();st.valid=true;
    Shader *tw=sky_twin(*fs);const bool twins=tw && fused_module();
    if(nr_lf_sky(&ls,mode,twins)!=NR_LF_SKY_SKIP)return;
    g.fs=fs;state.shader[4]=tw;g.disc=true;g.was_disc=state.rast_discard;state.rast_discard=1;st.skipped=true;++s.skipped;fused_draw=true;}
// The Light draw (role Light): the checks; the fused draw when Sky was skipped and every check passed.
void light(Guard &g,const p_nb_pass &h,int prim,int first,int count,bool indexed,uint32_t instances){
    ++s.light;const bool skipped=st.valid && st.skipped;int w=-1;Shader *fs=state.shader[4];
    if(!st.valid || st.seq!=h.seq || st.node>=h.node)w=W_NO_STASH;
    else if(!is(fs,"ClusteredLightingWithSSAO") || !state.shader[0] || state.shader[1] || state.shader[2] || state.shader[3] || state.rast_discard ||
            (state.color.alpha_func!=8 && alpha_test_on()) || state.ms.a2c)w=W_LIGHT_FS;
    else if(state.rt.n!=1 || !state.rt.color[0] || state.rt.depth || &get_image(state.rt.color[0],false)!=st.df)w=W_LIGHT_RT;
    else{const blend_st &b=state.blend[0];xbg_draw2d cv{};
        if(xbg_2d_blend_from_nvn(&cv,state.color.blend_en&1,b.src_rgb,b.dst_rgb,b.src_a,b.dst_a,b.eq_rgb,b.eq_a,state.mask.mask&15) ||
           !nr_lf_blend_ok(cv.blend.blendEnable,uint32_t(cv.blend.srcColorBlendFactor),uint32_t(cv.blend.dstColorBlendFactor),uint32_t(cv.blend.colorBlendOp),cv.blend.colorWriteMask))w=W_BLEND;
        else if(!same_bundle(st.vs,state.shader[0]->key))w=W_VS;
        else if(!same_snap(st.vub[0],state.ub[0][0]) || !same_snap(st.vub[1],state.ub[0][1]) || !same_snap(st.vvb[0],state.vb[0]) || !same_snap(st.vvb[1],state.vb[1]) || !same_snap(st.vvb[2],state.vb[2]) || !same_snap(st.vvb[3],state.vb[3]))w=W_VS_DATA;
        else if(prim!=st.prim || first!=st.first || count!=st.count || int(indexed)!=st.indexed || instances!=st.instances || memcmp(&state.viewport,&st.vp,sizeof st.vp) || memcmp(&state.scissor,&st.sc,sizeof st.sc))w=W_DRAW;
        else if(state.texture_pool.pool!=st.pool || state.texture_pool.off!=st.pool_off || state.sampler_pool.pool!=st.spool || state.sampler_pool.off!=st.spool_off)w=W_POOL;}
    Shader *tw=w<0?light_twin(*fs):nullptr;if(w<0 && !tw)w=W_TWIN;
    st.light_ok=false;st.fog_fused=false;
    if(w<0 && fog){   // the Fog inputs as Light sees them (checked against the Fog draw)
        const xbg_shader_binding *bd=entry(*fs,22),*b16=entry(*fs,82),*b15=entry(*fs,100),*b52=entry(*fs,52);
        if(bd && b16 && b15 && b52 && bd->source==XBG_SOURCE_TEXTURE && bd->slot<128 && b52->source==XBG_SOURCE_TEXTURE && b52->slot<128 && b16->source==XBG_SOURCE_UNIFORM && b15->source==XBG_SOURCE_UNIFORM){
            const RegisteredTexture *rdp=nbui::tex_rec(state.tex[1][bd->slot]),*rsp=nbui::tex_rec(state.tex[1][b52->slot]);
            const NVNbufferRange u16=state.ub[1][b16->slot],u15=state.ub[1][b15->slot];
            if(rdp && rsp && &get_image(&rsp->texture,false)==st.sp && u16.address && u16.size>=112 && u15.address && u15.size>=208){
                st.depth_im=&get_image(&rdp->texture,false);st.sp_h=state.tex[1][b52->slot];
                st.c16b.assign(reinterpret_cast<const uint8_t*>(uintptr_t(u16.address))+64,reinterpret_cast<const uint8_t*>(uintptr_t(u16.address))+112);
                st.c15b.assign(reinterpret_cast<const uint8_t*>(uintptr_t(u15.address))+192,reinterpret_cast<const uint8_t*>(uintptr_t(u15.address))+208);
                st.light_ok=true;st.light_node=h.node;}}}
    if(w>=0)fail(w);else if(!skipped)++s.would;
    const int act=nr_lf_light(&ls,mode,skipped,w<0);st.valid=false;
    if(act==NR_LF_LIGHT_LOST){++s.lost;red("seq %u: Sky was skipped but the Light draw failed a check (%s): the recorded Light draws, Sky's output is missing this frame; learning again",h.seq,wname(w));return;}
    if(act!=NR_LF_LIGHT_FUSED)return;
    if(mode==3 && st.sp_tex)poison(st.sp_tex);
    Shader *twg=fog==1 && fog_learned && st.light_ok && st.sp_tex?light_twin(*fs,'g'):nullptr;
    if(twg){tw=twg;g.mrt=true;g.rt=state.rt;g.blend_en=state.color.blend_en;g.mask=state.mask.mask;
        state.rt.n=2;state.rt.color[1]=st.sp_tex;state.rt.has_cview[1]=false;state.color.blend_en&=~2u;state.mask.mask=(state.mask.mask&~0xF0u)|(fog_mask<<4);
        st.fog_mask=fog_mask;st.fog_fused=true;++s.fog_fused;}   // out 1 = Sp with the learned Fog mask (R G B on the B10G11R11 target)
    g.fs=fs;state.shader[4]=tw;g.ov=true;for(int j=0;j<2;j++){g.ub_slot[j]=st.ub_slot[j];g.ub[j]=state.ub[1][st.ub_slot[j]];state.ub[1][st.ub_slot[j]]=st.ub[j];}
    for(int i=0;i<NR_LF_NTEX;i++){g.tex[i]=state.tex[1][NR_LF_SPARE_TEX+i];state.tex[1][NR_LF_SPARE_TEX+i]=st.tex[i];}
    ++tex_gen[1];++buf_gen[1];fused_draw=true;++s.fused;}
Image *img_of(uint64_t h){const RegisteredTexture *r=nbui::tex_rec(h);return r?&get_image(&r->texture,false):nullptr;}
int ffail(int w){++s.fog_why[w];return 0;}
// The DeferredFog draw after a checked Light of this submission: checks; 1 = skip it (the fused Light wrote its result into Sp).
int fogdraw(const p_nb_pass &h,int prim,int first,int count,bool indexed,uint32_t instances){
    ++s.fogs;const bool fused=st.fog_fused;st.fog_fused=false;int w=-1;const Shader *fs=state.shader[4];
    const xbg_shader_binding *bs=entry(*fs,16),*bd=entry(*fs,10),*b16=entry(*fs,22),*b15=entry(*fs,28);
    if(!st.light_ok || st.seq!=h.seq || h.node<=st.light_node)w=F_NOLIGHT;
    else if(!state.shader[0] || state.shader[1] || state.shader[2] || state.shader[3] || state.rast_discard)w=F_STAGES;
    else if(state.rt.n!=1 || !state.rt.color[0] || state.rt.depth)w=F_RT;
    else if(state.color.blend_en&1)w=F_BLEND;
    else if(!nr_lf_fog_mask_ok(state.mask.mask&15,uint32_t(get_image(state.rt.color[0],false).fmt)) || (fused && (state.mask.mask&15)!=st.fog_mask))w=F_MASK;
    else if(!bs || !bd || !b16 || !b15 || bs->source!=XBG_SOURCE_TEXTURE || bd->source!=XBG_SOURCE_TEXTURE ||
            bs->slot>=128 || bd->slot>=128 || b16->source!=XBG_SOURCE_UNIFORM || b15->source!=XBG_SOURCE_UNIFORM)w=F_BIND;
    else{const tex_t *c=state.rt.color[0];Image &ci=get_image(c,false);
        if(!st.sp_tex || ci.fmt!=st.sp->fmt || c->d.width!=st.sp_tex->d.width || c->d.height!=st.sp_tex->d.height || &ci==st.sp || &ci==st.df)w=F_TARGET;
        else if(img_of(state.tex[1][bs->slot])!=st.df)w=F_SCENE;
        else if(img_of(state.tex[1][bd->slot])!=st.depth_im)w=F_DEPTH;
        else{const NVNbufferRange u16=state.ub[1][b16->slot],u15=state.ub[1][b15->slot];
            if(!u16.address || u16.size<112 || !u15.address || u15.size<208 || memcmp(reinterpret_cast<const void*>(uintptr_t(u16.address)+64),st.c16b.data(),48) ||
               memcmp(reinterpret_cast<const void*>(uintptr_t(u15.address)+192),st.c15b.data(),16))w=F_CONST;
            else if(!same_bundle(st.vs,state.shader[0]->key) || !same_snap(st.vub[0],state.ub[0][0]) || !same_snap(st.vub[1],state.ub[0][1]) || !same_snap(st.vvb[0],state.vb[0]) ||
                    !same_snap(st.vvb[1],state.vb[1]) || !same_snap(st.vvb[2],state.vb[2]) || !same_snap(st.vvb[3],state.vb[3]))w=F_VS;
            else if(prim!=st.prim || first!=st.first || count!=st.count || int(indexed)!=st.indexed || instances!=st.instances || memcmp(&state.viewport,&st.vp,sizeof st.vp) || memcmp(&state.scissor,&st.sc,sizeof st.sc))w=F_DRAW;
            else if(state.texture_pool.pool!=st.pool || state.texture_pool.off!=st.pool_off)w=F_POOL;}}
    {static bool told=false;if(!told && state.rt.n>=1 && state.rt.color[0]){told=true;   // once: the state the model has to match
        xbg_logf("[nr] lightfuse fog: first Fog draw: colour targets %d, depth %d, blend %u, write mask %#x, target format %u (alpha-less %d), first refusal: %s",
            state.rt.n,state.rt.depth?1:0,state.color.blend_en&1,state.mask.mask&15,uint32_t(get_image(state.rt.color[0],false).fmt),
            nr_lf_vk_no_alpha(uint32_t(get_image(state.rt.color[0],false).fmt)),w>=0?fname_(w):"none");}}
    st.light_ok=false;
    if(w>=0){ffail(w);fog_learned=false;if(fused)++s.fog_back;return 0;}   // the Fog draws as recorded (exact: Df holds Light's result)
    ++s.fog_ok;if(fog==1 && (mode==1 || mode==3)){fog_learned=true;fog_mask=state.mask.mask&15;}
    if(!fused)return 0;
    Image &ci=get_image(state.rt.color[0],false);
    if(mode==3)poison(state.rt.color[0]);   // a reader of C that the redirect misses shows magenta
    rd=Redirect{};rd.on=true;rd.c=&ci;rd.writer=ci.provenance_writer;rd.seq=h.seq;rd.sp_id=uint32_t(st.sp_h&0xfffffull);rd.pool=state.texture_pool.pool;rd.pool_off=state.texture_pool.off;
    ++s.fog_skipped;++s.redir;return 1;}
// Every draw while the redirect is on: its fragment textures that are C sample Sp (same pool); off at C's next write / another submission.
void redirect(Guard &g){
    const p_nb_pass *hd=nbrp::span_hdr();
    if((hd && hd->seq!=rd.seq) || rd.c->provenance_writer!=rd.writer || state.texture_pool.pool!=rd.pool || state.texture_pool.off!=rd.pool_off){rd.on=false;return;}
    for(int i=0;i<state.rt.n;i++)if(state.rt.color[i] && &get_image(state.rt.color[i],false)==rd.c){rd.on=false;return;}
    const Shader *fs=state.shader[4];if(!fs)return;
    for(const auto &b:fs->bindings){if(b.source!=XBG_SOURCE_TEXTURE || b.slot>=128 || g.nre>=16)continue;const uint64_t hh=state.tex[1][b.slot];
        if(img_of(hh)!=rd.c)continue;g.re_slot[g.nre]=b.slot;g.re_h[g.nre]=hh;++g.nre;state.tex[1][b.slot]=(hh&~0xfffffull)|rd.sp_id;}
    if(g.nre){++tex_gen[1];fused_draw=true;++s.redir_draws;
        static unsigned told=0;if(told++<4)xbg_logf("[nr] lightfuse fog: draw with fs %s samples the skipped Fog's target: %d binding(s) -> Sp",fs->key.c_str(),g.nre);}}
// draw(), before the other twins: the role of this draw's pass in a marked chain. 1 = skip this draw (a fused Fog).
int pre(Guard &g,int prim,int first,int count,bool indexed,uint32_t instances){
    if(rd.on)redirect(g);
    const p_nb_pass *hd=nbrp::span_hdr();if(!hd)return 0;const int r=nb_oc_role(hd->oc);
    if(r==NB_OCR_SKY)sky(g,*hd,prim,first,count,indexed,instances);else if(r==NB_OCR_LIGHT)light(g,*hd,prim,first,count,indexed,instances);
    else if(fog && st.light_ok && is(state.shader[4],"DeferredFog"))return fogdraw(*hd,prim,first,count,indexed,instances);
    return 0;}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;frames0=stats.presents.load();return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const uint64_t fr=stats.presents.load();const double f=fr>frames0?double(fr-frames0):1.;frames0=fr;
    if(!s.sky && !s.light && !s.changed && !s.reds)return;
    char wy[900];int a=0;wy[0]=0;for(int i=0;i<W_N;i++)if(s.why[i] && a<(int)sizeof wy-120)a+=snprintf(wy+a,sizeof wy-size_t(a),"%s%s %llu",a?", ":"",wname(i),(unsigned long long)s.why[i]);
    xbg_logf("[nr] lightfuse DM_NR_LIGHTFUSE=%d: a frame: Sky draws %.2f, Light draws %.2f, %s %.2f, Sky skipped %.2f, fused Light draws %.2f; learned %d "
        "(learned %llu, unlearned %llu so far); lost (Sky skipped, no fused Light) %llu; refused: %s; passes kept sysmem for DM_GFX_PASS_GMEM %llu; "
        "magenta poison clears %llu; red lines %llu",mode,double(s.sky)/f,double(s.light)/f,mode==2?"checked chains (would fuse)":"chains that passed every check",
        double(s.would+s.fused)/f,double(s.skipped)/f,double(s.fused)/f,ls.learned,(unsigned long long)ls.learn,(unsigned long long)ls.unlearn,(unsigned long long)s.lost,
        a?wy:"none",(unsigned long long)s.gmem,(unsigned long long)s.poison,(unsigned long long)s.reds);
    if(fog){char fw[700];int b=0;fw[0]=0;for(int i=0;i<F_N;i++)if(s.fog_why[i] && b<(int)sizeof fw-100)b+=snprintf(fw+b,sizeof fw-size_t(b),"%s%s %llu",b?", ":"",fname_(i),(unsigned long long)s.fog_why[i]);
        xbg_logf("[nr] lightfuse fog DM_NR_LIGHTFUSE_FOG=%d: a frame: Fog draws %.2f, passed every check %.2f, fused into Light %.2f, Fog skipped %.2f, drawn as recorded after a "
            "fused Light %llu; learned %d; draws redirected to Sp %llu (windows %llu); refused: %s",fog,double(s.fogs)/f,double(s.fog_ok)/f,double(s.fog_fused)/f,double(s.fog_skipped)/f,
            (unsigned long long)s.fog_back,fog_learned,(unsigned long long)s.redir_draws,(unsigned long long)s.redir,b?fw:"none");}
    const uint64_t reds=s.reds;s=Stats{};s.reds=reds;}
}
