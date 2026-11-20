// [DOS2] NR-ONCHIP DM_NR_ONCHIP (docs/dos2/nr/NR-ONCHIP.md §8; planner + pure stepping: dm/gfx/nb/nb_onchip.h; front end: dm/host/hle_dos2_nb.c).
// The deferred sky + lighting pair in one VkRenderPass M with two subpasses: subpass 0 = DeferredSky (colours Sp, Df), subpass 1 =
// ClusteredLightingWithSSAO (colour Df, input attachment index 6 = Sp: its light.s2 variant "2.ClusteredLightingWithSSAO" of
// <shaders>/native-nr.xgspack, built by setup stage 8, binding 52 tAmbient read with subpassLoad). On a tile GPU Sp and Df stay in tile
// memory between the two draws: Sky's stores of Sp / Df, Light's load of Df and its texture read of Sp no longer go through DRAM
// (model: 25.2 MB a frame at 2160x972). Turnip with TU_DEBUG=sysmem (the Mi 14) moves the same bytes: no gain there.
// S2F (docs/dos2/nr/NR-FRAME.md §10, DM_NR_ONCHIP_FOG, default 1 with the switch; off while DM_NR_LIGHTFUSE is on - that one wins): a chain
// marked with the Fog tail runs as M with three attachments {Sp, Df, c0} and three subpasses: 0 Sky [Sp, Df]; 1 Light [Df], inputs 3 = c0
// (G-buffer emissive, binding 28) and 6 = Sp (binding 52) - the light.s2f variant "5.ClusteredLightingWithSSAO"; 2 DeferredFog [c0], input
// 1 = Df (binding 16) - "5.DeferredFog". c0 loads (LOAD: the G-buffer's emissive), the engine's ClearTexture of c0 between Light and Fog
// becomes a vkCmdClearAttachments at the start of subpass 2. c0's image is the one the G-buffer pass of this part rendered under the id
// the tap wrote into Sky's header (att[NB_REC_ATT - 1]). Everything the S2 path does stays: a tail that cannot run (no variant, no c0,
// shapes) leaves the plain S2 M and Fog as its own pass. Model: -16.8 MB a frame more (Light's Df store, Fog's Df read), 0 on the Mi 14.
//   DM_NR_ONCHIP 0 (default on every family): nothing - the front end plans nothing, images keep their usage; one integer test per host pass.
//   1: a chain the front end marked (Sky -> Light, or PCF -> Sky -> Light: PCF stays its own pass) runs as M. M begins at Sky's host pass
//      (end of the open pass, a full barrier, Sky's pending ClearTexture clears as CLEAR load ops, every other attachment LOAD, every store
//      STORE), Light's host pass steps to subpass 1 (vkCmdNextSubpass) and draws the light.s2 twin of its fragment shader (draw(): pre();
//      its descriptor 52 = M's view of Sp, INPUT_ATTACHMENT). Anything else that ends the pass ends M early: the subpasses left are stepped
//      empty (contents kept, every store STORE), so a broken chain is still the old picture and the rest runs as separate passes again.
//      Colour images get VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT (and their attachment views) while it is 1 / 3.
//   2: check: the same decisions on a shadow run (the light.s2 module and M's render pass are built once to prove they can be), nothing
//      executed differently: "would" counters. Images keep their usage.
//   3: 1 + poison: the attachments M begins with a CLEAR load are cleared to magenta in memory first (vkCmdClearColorImage before M): a
//      read that misses the subpass dependency or a load op taken from memory shows magenta.
// Line "[nr] onchip ..." every 10 s while something happened; red lines 【红】 (first 16).
// Pipelines of M are their own entries everywhere (PipelineDefinition per subpass; its words start {0, 0x4f43}: a warm-up / seed record of
// one fails make_pass's attachment count check instead of building a wrong render pass). The twin modules are not registered with the
// warm-up (no async build, no record): built in the frame once per session.
#include "../../nb/nb_onchip.h"
extern "C" void dm_nr_onchip_publish(int mode) __attribute__((weak));
extern "C" void dm_nr_onchip_publish_fog(int on) __attribute__((weak));   // [S2F] the front end marks Fog tails only when this backend asks
namespace nbrp{const p_nb_pass *span_hdr();}namespace nbchk{uint64_t key(const Image &im);}   // xbg_nb.inc (included later)
static_assert(offsetof(p_nb_pass,oc)==111 && offsetof(p_nb_pass,cover_dc)==112 && sizeof(p_nb_pass)==512,"NR-ONCHIP: p_nb_pass.oc sits in the former padding byte (layout unchanged)");
namespace nroc{
int mode=0;bool usage_on=false,active=false,twin_draw=false,fog_on=false;
nb_oc_run run{},shadow{};
struct Rp{VkRenderPass rp{};PipelineDefinition def[3];uint32_t nsub=2;};
std::map<std::array<uint32_t,7>,Rp> rps;
Rp *cur=nullptr;VkFramebuffer fb{};VkImageView sp_view{},df_view{},c0_view{};Image *sp=nullptr,*df=nullptr,*c0=nullptr;uint32_t w=0,h=0;VkFormat sp_fmt{},df_fmt{},c0_fmt{};
bool tail=false,shadow_tail=false;   // [S2F] M (or the check's shadow) has the Fog subpass
bool nexting=false,shadow_ok=false,in_brk=false;int pre_why=0;
// [S2F] the images of the host passes begun in this part, by IR image id (c0 for Sky's header); refreshed when the framebuffer changes
std::unordered_map<uint64_t,Image*> att_by_key;uint32_t att_seq=~0u;VkFramebuffer att_fb{};
struct Tw{const char *name;char var;uint32_t nin;uint32_t bind[2],idx[2];VkShaderModule mod{};int built=0;};
Tw tws[3]={{"ClusteredLightingWithSSAO",'2',1,{52,0},{6,0}},{"ClusteredLightingWithSSAO",'5',2,{28,52},{3,6}},{"DeferredFog",'5',1,{16,0},{1,0}}};   // light.s2, light.s2f, fog.s2f
enum{TW_S2=0,TW_S2F_LIGHT,TW_S2F_FOG};
std::map<std::pair<std::string,int>,Shader*> twins;
enum{R_SHAPE=0,R_IDS,R_USAGE,R_TWIN,R_RP,R_SIZE,R_N};
enum{B_AFTER_SKY=0,B_OTHER,B_LIGHT_RT,B_LIGHT_FS,B_LIGHT_CLEAR,B_LOST,B_CMD,B_FOG_RT,B_FOG_FS,B_FOG_CLEAR,B_INPUT,B_READ,B_N};
enum{T_C0=0,T_C0_SHAPE,T_TWIN,T_USAGE,T_N};
const char *rname(int i){static const char *n[R_N]={"Sky pass shape (2 colours, no depth, 1 sample, mip 0)","header attachments differ from the host's",
    "Sp without input-attachment usage","no light.s2 module (native-nr.xgspack without 2.ClusteredLightingWithSSAO? run setup again)","render pass",
    "Sky image sizes (Df != the pass area, or Sp smaller)"};return i>=0 && i<R_N?n[i]:"?";}
const char *bname(int i){static const char *n[B_N]={"ended after Sky (Light did not follow)","another pass / header came","Light's targets differ",
    "Light's fragment shader / stages","a clear or pending clear in Light","M ended between the member's checks and its draw (drawn again as its own pass)","another command buffer",
    "Fog's target differs from c0","Fog's fragment shader / stages","a pending clear in Fog other than its c0 clear","an input binding saw another image (drawn again as its own pass)",
    "the member's descriptor build would end M (a sampled binding reads an attachment of M / an image written since M's barrier)"};return i>=0 && i<B_N?n[i]:"?";}
const char *tname(int i){static const char *n[T_N]={"c0 not among this part's rendered images","c0 shape (size / format / samples / the same as Sp or Df)",
    "no light.s2f / fog.s2f module (native-nr.xgspack without 5.ClusteredLightingWithSSAO / 5.DeferredFog? run setup again)","c0 without input-attachment usage"};return i>=0 && i<T_N?n[i]:"?";}
struct Stats{uint64_t heads=0,begun=0,chains=0,s3=0,refused[R_N]{},brk[B_N]{},inputs=0,input_other=0,poison=0,reds=0,changed=0,tails=0,tail_begun=0,fogs=0,tail_refused[T_N]{},fog_clears=0,redraws=0,pflush=0,pflush_m=0;double mb=0;uint32_t sz[6]{};} s;   // sz: first R_SIZE (Sp, Df, pass)
uint64_t frames0=0,next_ns=0;bool input_bad=false;
void red(const char *fmt,...){const uint64_t n=++s.reds;if(n>16)return;char t[512];va_list a;va_start(a,fmt);vsnprintf(t,sizeof t,fmt,a);va_end(a);xbg_logf("【红】[nr] onchip %s",t);}
// What ended M before its last subpass (19.11 phone run N_oc3mr: "ended after Sky 838, M ended between Light's checks and its draw 838" with
// the Light draw skipped). Every early end is counted by its site: the end_open_pass call line, the op being recorded (cur_op), the member
// reached (k) and, when prepare_read ended it, the read image (role Sp / Df / c0 / other, why bits 1 layout, 2 written since the reader's
// barrier point, 4 attachment of the open pass) and whether it was inside a prepared twin draw. One line per new site (first 8), the counts
// in the 10 s line. Pure bookkeeping: nothing here changes what is recorded.
struct Rd{bool set=false;uint64_t serial=0;uint32_t fmt=0,w=0,h=0;unsigned why=0;int role=-1;bool twin=false;uint64_t lw=0,seen=0;} rd;   // the read break about to end the open pass
struct Site{int line=0;unsigned op=0;int k=0,role=-2;unsigned why=0;bool twin=false;uint64_t n=0;};Site sites[8];int nsites=0;
// [early end] a sampled binding of a member's draw that would end M in its descriptor build (pre()): first example
struct Ex{bool set=false;uint32_t binding=0,slot=0,w=0,h=0,fmt=0;uint64_t serial=0;unsigned why=0;int role=-1,k=0,n=0;} ex;   // n: examples written (a new binding / image / why: up to 4)
nb_oc_learned learned{};bool pf_told=false;   // [early clears] the members' sampled images (reads_ok), the first flush line written
int role_of(const Image *im){return im && im==sp?0:im && im==df?1:im && c0 && im==c0?2:3;}
const char *role_name(int r){return r==0?"Sp (Sky out 0)":r==1?"Df (Sky out 1, Light's target)":r==2?"c0 (Fog's target)":r==3?"another image":"no read";}
void note_read(const Image &im,unsigned why){   // prepare_read, right before its end_open_pass, while M is open
    rd.set=true;rd.serial=im.source.serial;rd.fmt=uint32_t(im.fmt);rd.w=im.width;rd.h=im.height;rd.why=why;rd.role=role_of(&im);rd.twin=nexting;rd.lw=im.last_write;rd.seen=read_frag_only?frag_seq:barrier_seq;}
void note_end(int line,int k){
    const int role=rd.set?rd.role:-2;Site *x=nullptr;
    for(int i=0;i<nsites;i++)if(sites[i].line==line && sites[i].op==cur_op && sites[i].k==k && sites[i].role==role && sites[i].why==(rd.set?rd.why:0u)){x=&sites[i];break;}
    if(!x && nsites<8){x=&sites[nsites++];x->line=line;x->op=cur_op;x->k=k;x->role=role;x->why=rd.set?rd.why:0u;x->twin=rd.set && rd.twin;
        char t[256];t[0]=0;if(rd.set)snprintf(t,sizeof t,", by prepare_read of %s serial %llu fmt %u %ux%u why %u (%s%s%s; last_write %llu, reader's barrier point %llu)%s",role_name(rd.role),
            (unsigned long long)rd.serial,rd.fmt,rd.w,rd.h,rd.why,rd.why&1?"layout ":"",rd.why&2?"written ":"",rd.why&4?"attachment of M":"",(unsigned long long)rd.lw,(unsigned long long)rd.seen,
            rd.twin?", inside the prepared twin draw (between pre() and adjust())":"");
        xbg_logf("[nr] onchip M ended early (after %s, subpass %d of %u): end_open_pass line %d, op %u%s",k==0?"Sky":k==1?"Light":"Fog",k,cur?cur->nsub:2u,line,cur_op,t);}
    if(x)++x->n;rd.set=false;}
void init(){
    const char *e=getenv("DM_NR_ONCHIP");int m=e && *e?atoi(e):xbg_nr_onchip_default(gpu_policy);
    if(m<0 || m>3){xbg_logf("【红】[nr] onchip DM_NR_ONCHIP=%s is not 0 / 1 / 2 / 3: off",e);m=0;}
    if(m && nrlf::mode){xbg_logf("[nr] onchip DM_NR_ONCHIP=%d is superseded by DM_NR_LIGHTFUSE=%d: the Sky -> Light chain is one fused draw, no merged "
        "render pass (onchip backend off; the front end still marks the chains for lightfuse)",m,nrlf::mode);}
    const int fe=nrlf::fe_mode(m);if(nrlf::mode)m=0;   // [DOS2] NR-LIGHTFUSE (xbg_nr_lightfuse.inc): one chain, one owner
    mode=m;usage_on=m==1 || m==3;
    const char *fge=getenv("DM_NR_ONCHIP_FOG");const bool lightfuse=nrlf::mode!=0;   // m is 0 already when DM_NR_LIGHTFUSE is on (above)
    fog_on=m>0 && !(fge && *fge && atoi(fge)==0) && !lightfuse;   // [S2F] DM_NR_LIGHTFUSE (Sky folded into the light shader) wins: no Sky pass to chain
    if(dm_nr_onchip_publish_fog)dm_nr_onchip_publish_fog(fog_on?1:0);
    if(dm_nr_onchip_publish)dm_nr_onchip_publish(fe);   // [DOS2] NR-LIGHTFUSE: the front end keeps marking chains for lightfuse (nrlf::fe_mode)
    if(m)xbg_logf("[nr] onchip DM_NR_ONCHIP=%d (%s; family %s, default %d): Sky -> Light in one render pass, Light reads Sky out 0 as an input "
        "attachment (light.s2 from native-nr.xgspack); S2F Fog tail %s (DM_NR_ONCHIP_FOG%s); needs the NB records (DM_NB=1) and the front end's chain marks",m,
        m==1?"on":m==2?"check: counted, the old passes run":"on + magenta poison of the attachments M clears",xbg_gpu_family_name(gpu_policy),xbg_nr_onchip_default(gpu_policy),
        fog_on?"on":"off",lightfuse?"; DM_NR_LIGHTFUSE is on and wins":"");}
uint32_t bpp(VkFormat f){switch(f){case VK_FORMAT_R16G16B16A16_SFLOAT:case VK_FORMAT_R32G32_SFLOAT:return 8;case VK_FORMAT_R32G32B32A32_SFLOAT:return 16;
    case VK_FORMAT_R8_UNORM:return 1;case VK_FORMAT_R16_SFLOAT:case VK_FORMAT_R8G8_UNORM:return 2;default:return 4;}}
// A twin module: the translated bundle's head + the pack's variant (xpack::nrsh::oc_apply), the load-time rewrites of every bundle; its
// subpass inputs must be exactly the table's.
VkShaderModule twin_module(int t){
    Tw &tw=tws[t];if(tw.built)return tw.mod;tw.built=1;
    const int i=nr_sh_find_name(tw.name);if(i<0){red("no %s in the NR-04 table",tw.name);return tw.mod;}
    const std::string key=nr_sh_table[i].key;std::vector<uint32_t> wd;uint32_t stage=0;bool ok;
    {xpack::nrsh::oc_var=tw.var;ok=warm::bundle_words(key,wd,stage,"nr-oc");xpack::nrsh::oc_var=0;}
    uint32_t b[4]{},ix[4]{};const int ns=ok?nb_oc_spv_inputs(wd.data(),wd.size(),b,ix,4):-1;
    if(!ok || stage!=4){red("%s: no %c.%s bundle that passes the head / interface / descriptor checks (native-nr.xgspack of setup stage 8): no merge",key.c_str(),tw.var,tw.name);return tw.mod;}
    bool same=ns==int(tw.nin);for(uint32_t k=0;same && k<tw.nin;k++){bool f=false;for(int j=0;j<ns && j<4;j++)if(b[j]==tw.bind[k] && ix[j]==tw.idx[k])f=true;same=f;}
    if(!same){red("%s: the %c.%s module has %d subpass inputs (first binding %u index %u), not the %u expected: no merge",key.c_str(),tw.var,tw.name,ns,b[0],ix[0],tw.nin);return tw.mod;}
    if(!module_gap(wd,stage).empty() || bundle_module(wd,&tw.mod,key+"#oc"+tw.var)!=VK_SUCCESS){tw.mod=VK_NULL_HANDLE;red("%s: the %c.%s module could not be made: no merge",key.c_str(),tw.var,tw.name);return tw.mod;}
    ++s.changed;xbg_logf("[nr] onchip %c.%s module for %s: %zu words, %u subpass input(s)",tw.var,tw.name,key.c_str(),wd.size(),tw.nin);return tw.mod;}
// The fragment shader twin for a draw inside M: the twin module, its input bindings INPUT_ATTACHMENT; everything else as fs.
Shader *twin(Shader &fs,int t){
    auto it=twins.find({fs.key,t});if(it!=twins.end())return it->second;
    Shader *sh=nullptr;const VkShaderModule m=twin_module(t);const Tw &tw=tws[t];
    if(m){sh=new Shader();sh->key=fs.key+"#oc"+tw.var+(t==TW_S2F_FOG?"f":"");sh->stage=4;sh->module=m;sh->bindings=fs.bindings;sh->inputs=fs.inputs;sh->input_classes=fs.input_classes;sh->c1=fs.c1;
        sh->discards=fs.discards;sh->empty_fs=fs.empty_fs;sh->raster=fs.raster;sh->pg_coord=fs.pg_coord;sh->storage_writes=fs.storage_writes;sh->vo_read=fs.vo_read;sh->vo_decl=fs.vo_decl;
        uint32_t n=0;for(auto &bd:sh->bindings)for(uint32_t k=0;k<tw.nin;k++)if(bd.binding==tw.bind[k] && bd.source==XBG_SOURCE_TEXTURE && bd.count==1 && (bd.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || bd.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)){bd.type=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;bd.sampler_slot=UINT32_MAX;++n;}
        if(n!=tw.nin){red("%s: the input bindings are not %u sampled textures in the binding table (%u): no twin",fs.key.c_str(),tw.nin,n);sh->module=VK_NULL_HANDLE;delete sh;sh=nullptr;}
        else{++s.changed;xbg_logf("[nr] onchip %s -> %s (%u input attachment binding(s))",fs.key.c_str(),sh->key.c_str(),tw.nin);}}
    twins.emplace(std::make_pair(fs.key,t),sh);return sh;}
// A Light draw may enter subpass 1: its render targets are M's Df (same view), its fragment shader is ClusteredLightingWithSSAO without
// alpha test / tessellation / geometry, no pending clear of Df (or of c0 with the tail). 0 = yes, else the B_* reason.
int light_ok(){
    if(state.rt.n!=1 || !state.rt.color[0] || state.rt.depth)return B_LIGHT_RT;
    Image &im=get_image(state.rt.color[0],false);if(&im!=df)return B_LIGHT_RT;
    const view_t *v=state.rt.has_cview[0]?&state.rt.cview[0]:nullptr;if(im.view_format(v)!=df_fmt || (v && v->set_levels && v->base_level) || (v && v->set_layers && v->min_layer))return B_LIGHT_RT;
    const Shader *fs=state.shader[4];if(!fs || fs->key.size()<16 || nr_sh_find_key(fs->key.c_str())!=nr_sh_find_name("ClusteredLightingWithSSAO"))return B_LIGHT_FS;
    if(state.shader[1] || state.shader[2] || state.shader[3] || state.rast_discard || (state.color.alpha_func!=8 && alpha_test_on()) || state.ms.a2c)return B_LIGHT_FS;
    for(auto &e:pclears)if(e.im==df || e.im==sp || (c0 && e.im==c0))return B_LIGHT_CLEAR;
    return 0;}
// [S2F] A Fog draw may enter subpass 2: its target is M's c0 (same view format, mip 0 layer 0), its fragment shader DeferredFog without
// alpha test / tessellation / geometry; a pending clear is allowed only for c0 (a whole colour clear: the engine's ClearTexture).
int fog_ok(){
    if(state.rt.n!=1 || !state.rt.color[0] || state.rt.depth)return B_FOG_RT;
    Image &im=get_image(state.rt.color[0],false);if(&im!=c0)return B_FOG_RT;
    const view_t *v=state.rt.has_cview[0]?&state.rt.cview[0]:nullptr;if(im.view_format(v)!=c0_fmt || (v && v->set_levels && v->base_level) || (v && v->set_layers && v->min_layer))return B_FOG_RT;
    const Shader *fs=state.shader[4];if(!fs || fs->key.size()<16 || nr_sh_find_key(fs->key.c_str())!=nr_sh_find_name("DeferredFog"))return B_FOG_FS;
    if(state.shader[1] || state.shader[2] || state.shader[3] || state.rast_discard || (state.color.alpha_func!=8 && alpha_test_on()) || state.ms.a2c)return B_FOG_FS;
    for(auto &e:pclears){if(e.im==df || e.im==sp)return B_FOG_CLEAR;if(e.im==c0 && (e.aspects!=VK_IMAGE_ASPECT_COLOR_BIT || e.fmt!=c0_fmt))return B_FOG_CLEAR;}
    return 0;}
struct Guard{Shader *saved=nullptr;~Guard(){if(saved)state.shader[4]=saved;nexting=false;shadow_ok=false;twin_draw=false;pre_why=0;}};
void brk(int why);
// [early end] Would the descriptor build of the member's draw end M? prepare_read ends the open pass for an image that is an attachment of
// it, not GENERAL, or written since the reader's barrier point (descriptors(): read_frag_only under DM_GFX_TILE_BARRIER). The sampled
// texture bindings of the fragment shader other than the variant's inputs (tv.bind) are resolved the way descriptors() does (handle ->
// registered texture -> its image; nothing is made or recorded). full: the whole prepare_read test against the open pass (M, modes 1 / 3);
// else (mode 2, the old Sky / Light pass is open) only "the image is Sp / Df / c0", the part the merged pass cannot run whatever the barriers.
// 0 = no; else the why bits of the first such binding (first example kept in ex).
unsigned reads_ok(const Shader &fs,const Tw &tv,bool full,int k){
    if(!state.texture_pool.pool)return 0;
    const std::map<int,RegisteredTexture> *tt=&legacy_textures;{auto f=textures.find(pool_key(&state.texture_pool));if(f!=textures.end())tt=&f->second;}
    bool fo=tile_barrier_on;   // as descriptors()' FragRead: no image / texture binding before the fragment stage
    for(unsigned st=0;st<4 && fo;st++)if(const Shader *sh=state.shader[st])for(const auto &b:sh->bindings){
        if(b.source==XBG_SOURCE_TEXTURE || b.source==XBG_SOURCE_IMAGE || b.source==XBG_SOURCE_TEXTURE_UBO || b.source==XBG_SOURCE_IMAGE_UBO || b.source==XBG_SOURCE_TEXTURE_POOL || b.source==XBG_SOURCE_IMAGE_POOL){fo=false;break;}
        if(b.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || b.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || b.type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT){fo=false;break;}}
    const bool save=read_frag_only;read_frag_only=fo;   // nrrp::lw reads it
    const unsigned ns=nvn_stage[4];unsigned bad=0;
    nb_oc_learn_reset(&learned,k);   // [early clears] every sampled image of this member, for the pending clears landed at the next M's begin
    for(const auto &b:fs.bindings){
        if(b.source!=XBG_SOURCE_TEXTURE || (b.type!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b.type!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE))continue;
        bool input=false;for(uint32_t q=0;q<tv.nin;q++)if(b.binding==tv.bind[q])input=true;if(input)continue;   // read with subpassLoad in the variant
        for(uint32_t e=0;e<b.count;e++){const unsigned slot=b.slot+e;if(slot>=128)continue;
            const int id=int(state.tex[ns][slot]&0xfffff);if(id==0 || (state.texture_pool.n>0 && id>=state.texture_pool.n))continue;   // the zero texture
            auto t=tt->find(id);if(t==tt->end())continue;Image *im=find_image(t->second.texture.serial);if(!im)continue;
            const int role=role_of(im);unsigned why=0;
            if(role==3)nb_oc_learn_add(&learned,k,im->source.serial);
            if(bad)continue;   // the first binding that would end M decides; the rest only learned
            if(full){const bool in_open=open_pass.cmd && std::find(open_pass.images.begin(),open_pass.images.end(),im)!=open_pass.images.end();
                uint64_t seen=fo?frag_seq:barrier_seq;if(opbar::mode==1)seen=nb_ob_seen(seen,fo?im->opvis_frag:im->opvis_full);
                const bool written=nrrp::lw(*im)>seen || im->storage_write>seen;why=unsigned(im->layout!=VK_IMAGE_LAYOUT_GENERAL)|unsigned(written)<<1|unsigned(in_open)<<2;
                for(const auto &pc:pclears)if(pc.im==im)why|=8;   // image_read_hook lands a pending clear of it: that ends the pass too
                if(why && !open_pass.cmd)why=0;}   // no open pass: nothing to end (prepare_read only barriers)
            else if(role<3)why=4;
            if(why){bad=why;if(!ex.set || (ex.n<4 && (ex.binding!=b.binding || ex.serial!=im->source.serial || ex.why!=why || ex.k!=k))){ex.set=true;++ex.n;ex.binding=b.binding;ex.slot=slot;ex.serial=im->source.serial;ex.fmt=uint32_t(im->fmt);ex.w=im->width;ex.h=im->height;ex.why=why;ex.role=role;ex.k=k;
                xbg_logf("[nr] onchip the %s draw's descriptor build would end M: fragment binding %u (texture slot %u) reads %s serial %llu fmt %u %ux%u, why %u (%s%s%s%s)%s: M ends "
                    "before the draw, which runs as its own pass",k==0?"Light":"Fog",b.binding,slot,role_name(role),(unsigned long long)im->source.serial,ex.fmt,ex.w,ex.h,why,why&1?"layout ":"",
                    why&2?"written since M's barrier ":"",why&4?"attachment of M ":"",why&8?"pending clear":"",full?"":" (check: would)");}}}}
    read_frag_only=save;return bad;}
// draw(), before the descriptors: the Light draw that will step M to subpass 1 (or, with the tail, the Fog draw that steps it to 2) gets its
// twin (mode 2: only the checks). A member that cannot run in M ends M here, before its descriptors, so it is drawn exactly as the old
// passes draw it (its descriptor build would otherwise end M itself, with the twin already swapped in: the 19.11 skipped Light).
void pre(Guard &g){
    nexting=shadow_ok=false;pre_why=0;input_bad=false;nb_oc_run &r=mode==2?shadow:run;if(!r.active || r.k>1)return;
    const bool tl=mode==2?shadow_tail:tail;if(r.k==1 && !tl)return;
    const p_nb_pass *hd=nbrp::span_hdr();if(nb_oc_step(&r,hd!=nullptr,hd?hd->oc:0,hd?hd->seq:0,hd?hd->node:-1)!=NB_OCS_NEXT)return;
    const int tw=r.k==1?TW_S2F_FOG:tl?TW_S2F_LIGHT:TW_S2;
    int why=r.k==0?light_ok():fog_ok();
    if(!why && reads_ok(*state.shader[4],tws[tw],mode!=2,r.k))why=B_READ;
    if(why){if(mode==2){++s.brk[why];nb_oc_apply(&shadow,NB_OCS_BREAK,0,-1);shadow_tail=false;}else brk(why);return;}   // mode 1 / 3: M ends now, the draw is the old pass
    if(mode==2){shadow_ok=true;return;}
    Shader *t=twin(*state.shader[4],tw);if(!t){brk(r.k==0?B_LIGHT_FS:B_FOG_FS);return;}
    g.saved=state.shader[4];state.shader[4]=t;nexting=twin_draw=true;}
// draw(), when adjust() returned false (M ended under the prepared twin draw, or its input binding saw another image): the draw runs as
// recorded - the game's fragment shader back, then draw() builds its descriptors and pass again (the old pass). Never a skipped draw.
uint64_t redraws_all=0;
void redo(Guard &g){if(g.saved){state.shader[4]=g.saved;g.saved=nullptr;}nexting=twin_draw=false;shadow_ok=false;pre_why=0;input_bad=false;++s.redraws;
    if(++redraws_all<=4)xbg_logf("[nr] onchip a member's twin draw lost M before it was drawn: drawn again with its own fragment shader as its own pass (%llu)",(unsigned long long)redraws_all);}
// The descriptor of an INPUT_ATTACHMENT binding (only a twin has one): M's view of that image.
VkImageView input_view(const Image &im){++s.inputs;
    if(&im==sp)return sp_view;if(tail && &im==c0)return c0_view;if(tail && &im==df)return df_view;
    ++s.input_other;input_bad=true;red("a twin draw binds another image as an input attachment (serial %llu; Sp %llu Df %llu c0 %llu): the draw is skipped",(unsigned long long)im.source.serial,
        sp?(unsigned long long)sp->source.serial:0ull,df?(unsigned long long)df->source.serial:0ull,c0?(unsigned long long)c0->source.serial:0ull);return sp_view;}
Rp *get_rp(VkFormat f0,VkFormat f1,VkAttachmentLoadOp l0,VkAttachmentLoadOp l1,VkFormat f2=VK_FORMAT_UNDEFINED){
    const uint32_t ns=f2!=VK_FORMAT_UNDEFINED?3u:2u;
    const std::array<uint32_t,7> k{uint32_t(f0),uint32_t(f1),uint32_t(l0),uint32_t(l1),tile_barrier_on?1u:0u,uint32_t(f2),ns};
    auto it=rps.find(k);if(it!=rps.end())return it->second.rp?&it->second:nullptr;
    Rp &e=rps[k];e.nsub=ns;
    VkAttachmentDescription a[3]{};const VkFormat f[3]={f0,f1,f2};const VkAttachmentLoadOp l[3]={l0,l1,VK_ATTACHMENT_LOAD_OP_LOAD};   // [S2F] c0: the G-buffer's emissive, loaded
    for(uint32_t i=0;i<ns;i++){a[i].format=f[i];a[i].samples=VK_SAMPLE_COUNT_1_BIT;a[i].loadOp=l[i];a[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;a[i].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        a[i].stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;a[i].initialLayout=a[i].finalLayout=VK_IMAGE_LAYOUT_GENERAL;}
    const VkAttachmentReference c0r[2]={{0,VK_IMAGE_LAYOUT_GENERAL},{1,VK_IMAGE_LAYOUT_GENERAL}},c1r[1]={{1,VK_IMAGE_LAYOUT_GENERAL}},c2r[1]={{2,VK_IMAGE_LAYOUT_GENERAL}};
    VkAttachmentReference in1[7];for(int i=0;i<6;i++)in1[i]={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};in1[6]={0,VK_IMAGE_LAYOUT_GENERAL};
    if(ns==3)in1[3]={2,VK_IMAGE_LAYOUT_GENERAL};   // [S2F] light.s2f: input 3 = c0 (tEmissive)
    const VkAttachmentReference in2[2]={{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL},{1,VK_IMAGE_LAYOUT_GENERAL}};   // [S2F] fog.s2f: input 1 = Df (tScene)
    VkSubpassDescription sd[3]{};for(auto &x:sd)x.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    sd[0].colorAttachmentCount=2;sd[0].pColorAttachments=c0r;sd[1].colorAttachmentCount=1;sd[1].pColorAttachments=c1r;sd[1].inputAttachmentCount=7;sd[1].pInputAttachments=in1;
    sd[2].colorAttachmentCount=1;sd[2].pColorAttachments=c2r;sd[2].inputAttachmentCount=2;sd[2].pInputAttachments=in2;
    VkSubpassDependency base[2];pass_deps(base);VkSubpassDependency d[8];uint32_t nd=0;
    d[nd]=base[0];d[nd++].dstSubpass=0;   // EXTERNAL -> 0: as every pass (the full barrier before M covers the sampled reads of the later subpasses and c0's load)
    d[nd++]={0,1,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_DEPENDENCY_BY_REGION_BIT};
    if(ns==3){   // [S2F] 1 -> 2: Df written by Light is read by Fog (RAW); c0 read by Light is written by Fog (WAR); 0 -> 2: Df of Sky (via 1 too)
        d[nd++]={1,2,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_DEPENDENCY_BY_REGION_BIT};
        d[nd++]={0,2,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_DEPENDENCY_BY_REGION_BIT};}
    for(uint32_t sp_=0;sp_<ns;sp_++){d[nd]=base[1];d[nd++].srcSubpass=sp_;}   // every subpass -> EXTERNAL: as every pass
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};ci.attachmentCount=ns;ci.pAttachments=a;ci.subpassCount=ns;ci.pSubpasses=sd;ci.dependencyCount=nd;ci.pDependencies=d;
    if(vkCreateRenderPass(cfg.device,&ci,nullptr,&e.rp)!=VK_SUCCESS){e.rp=VK_NULL_HANDLE;red("vkCreateRenderPass of M failed (formats %d %d %d, %u subpasses): no merge",int(f0),int(f1),int(f2),ns);return nullptr;}
    for(uint32_t sub=0;sub<ns;sub++){e.def[sub].words={0u,0x4f43u,uint32_t(f0),uint32_t(f1),uint32_t(l0),uint32_t(l1),sub,k[4]};if(ns==3){e.def[sub].words.push_back(uint32_t(f2));e.def[sub].words.push_back(ns);}e.def[sub].valid=true;}
    ++s.changed;xbg_logf("[nr] onchip render pass M: Sp fmt %d load %d, Df fmt %d load %d%s, %u subpasses (Sky; Light + input 6 = Sp%s), every store STORE",int(f0),int(l0),int(f1),int(l1),
        ns==3?", c0 (Fog's target) load LOAD":"",ns,ns==3?", input 3 = c0; Fog + input 1 = Df":"");
    return &e;}
VkFramebuffer get_fb(VkRenderPass rp,const VkImageView *v,uint32_t n,uint32_t fw,uint32_t fh,Image *const *im){
    static std::vector<uint64_t> key;key.assign({uint64_t(rp),fw,fh});for(uint32_t i=0;i<n;i++)key.push_back(uint64_t(v[i]));
    auto &cache=persistent_fb_enabled?persistent_framebuffers:recording_slot().framebuffers;auto it=cache.find(key);if(it!=cache.end()){++fb_hits;return it->second;}
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};ci.renderPass=rp;ci.attachmentCount=n;ci.pAttachments=v;ci.width=fw;ci.height=fh;ci.layers=1;
    VkFramebuffer f{};VK(vkCreateFramebuffer(cfg.device,&ci,nullptr,&f));cache.emplace(key,f);++fb_creates;
    if(persistent_fb_enabled)for(uint32_t i=0;i<n;i++)im[i]->fb_keys.push_back(key);
    return f;}
void substitute(Pass &p,uint8_t k){p.pass=cur->rp;p.pass_noload=VK_NULL_HANDLE;p.framebuffer=fb;p.definition=&cur->def[k];p.subpass=k;}
// Sky's host pass can begin M: 0 = yes, else R_*. Fills the formats / views.
int sky_ok(const Pass &p,const p_nb_pass &hd,VkImageView *v,VkFormat *f){
    if(state.rt.n!=2 || !state.rt.color[0] || !state.rt.color[1] || state.rt.depth || p.images.size()!=2 || p.samples!=VK_SAMPLE_COUNT_1_BIT)return R_SHAPE;
    for(int k=0;k<2;k++){const Image &im=*p.images[k];if(im.aspects!=VK_IMAGE_ASPECT_COLOR_BIT || p.level[k] || p.layer[k])return R_SHAPE;}
    if(p.images[0]==p.images[1])return R_SHAPE;
    // Sizes: M's framebuffer is the pass area (min of the two = Df's size; Light's own pass area is Df's too). Df must equal it; Sp may be
    // larger (Vulkan: every attachment >= the framebuffer; Light reads Sp at its own pixel). The front end applies the same rule to the
    // sizes the passes were bound with (nb_oc_sizes); this is the check on the real images.
    {const Image &a=*p.images[0],&d=*p.images[1];
     if(d.width!=p.width || d.height!=p.height || a.width<p.width || a.height<p.height){
        if(!s.sz[4]){s.sz[0]=a.width;s.sz[1]=a.height;s.sz[2]=d.width;s.sz[3]=d.height;s.sz[4]=p.width;s.sz[5]=p.height;}return R_SIZE;}}
    if(hd.natt!=2)return R_IDS;for(int k=0;k<2;k++)if(hd.att[k]!=nbchk::key(*p.images[k]) || hd.slot[k]!=k)return R_IDS;
    if(!(p.images[0]->usage&VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) && mode!=2)return R_USAGE;
    if(!twin_module(TW_S2))return R_TWIN;
    for(int k=0;k<2;k++){const view_t *vw=state.rt.has_cview[k]?&state.rt.cview[k]:nullptr;f[k]=p.images[k]->view_format(vw);v[k]=p.images[k]->view(vw,true);}
    return 0;}
// [S2F] the Fog tail of a Sky header: c0 = the image the G-buffer pass of this part rendered under the id in att[NB_REC_ATT - 1]; 0 = yes
// (c0 / its view / format set), else T_*. A refusal keeps the plain S2 M (Fog runs as its own pass).
int tail_ok(const Pass &p,const p_nb_pass &hd,Image *&im,VkImageView &v,VkFormat &f){
    im=nullptr;const uint64_t id=hd.att[NB_REC_ATT-1];
    auto it=id?att_by_key.find(id):att_by_key.end();if(it==att_by_key.end() || !it->second)return T_C0;
    Image *c=it->second;
    if(c==p.images[0] || c==p.images[1] || c->aspects!=VK_IMAGE_ASPECT_COLOR_BIT || c->samples!=VK_SAMPLE_COUNT_1_BIT || c->width!=p.width || c->height!=p.height || c->levels!=1 || c->layers!=1)return T_C0_SHAPE;
    if(!(c->usage&VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) && mode!=2)return T_USAGE;
    if(!twin_module(TW_S2F_LIGHT) || !twin_module(TW_S2F_FOG))return T_TWIN;
    im=c;f=c->view_format(nullptr);v=c->view(nullptr,true);return 0;}
// End M early or after its last member: the subpasses left are stepped empty (Vulkan ends a render pass in its last subpass). end_open_pass,
// right before its vkCmdEndRenderPass.
void before_end(VkCommandBuffer cmd,int line){
    if(!active)return;
    if(!in_brk && run.k+1<int(cur?cur->nsub:2))note_end(line,run.k);else rd.set=false;   // an early end that no member check decided: where it came from
    for(int i=nb_oc_steps_to_end(int(cur?cur->nsub:2),run.k);i>0;i--)vkCmdNextSubpass(cmd,VK_SUBPASS_CONTENTS_INLINE);
    if(!in_brk){if(run.k)++s.chains;else ++s.brk[B_AFTER_SKY];if(run.k==2)++s.fogs;}
    nb_oc_apply(&run,NB_OCS_BREAK,0,-1);active=false;cur=nullptr;tail=false;c0=nullptr;}
// M ends here (k >= 1: the chain is done, the next pass came; k == 0: early, reason why).
void brk(int why){const int k=run.k;in_brk=true;if(active)end_open_pass();in_brk=false;if(k)++s.chains;else ++s.brk[why];if(k==2)++s.fogs;
    nb_oc_apply(&run,NB_OCS_BREAK,0,-1);active=false;cur=nullptr;tail=false;c0=nullptr;}
// [early clears] (NR-ONCHIP.md §11; 19.11 P_oc3m: Light's binding 40 = the G-buffer's fifth target, serial 29 RGBA8, cleared by a
// ClearTexture before the G-buffer pass and never drawn, stayed a pending clear until Light's descriptor build read it: that read lands it
// with vkCmdClearColorImage outside a render pass, so it would end M every frame). At M's begin - after end_open_pass, after M's render pass
// exists, before M's full barrier - the pending clears of the images the members sampled the last time (learned) land here, through the
// same pclear_flush_image the member's read runs. Exact: a pending clear is landed by any use of its image (sampled / storage read, an
// attachment, a copy / blit / resolve, a submit), so between its ClearTexture and this point nothing used the image and it lands after
// every earlier writer either way; M's full barrier right after makes it visible to the member's read (prepare_read then finds nothing
// pending, written or to transition). Only an image that has a pending clear moves (one clear done earlier, nothing extra); M's own
// attachments (Sp, Df, c0) keep their rules (CLEAR load ops / landed above / Fog's in-pass clear); Fog's images only when M has the Fog
// subpass. exec false (=2, the shadow begin): counted as "would". Returns the number of pending clears picked.
uint32_t pre_flush(const Image *a,const Image *b,const Image *c,bool fog,bool exec){
    Image *pick[16];uint32_t n=0;
    for(const auto &e:pclears)if(e.im!=a && e.im!=b && e.im!=c && nb_oc_learned_has(&learned,fog?2:1,e.im->source.serial) && n<16)pick[n++]=e.im;
    if(!n)return 0;
    char t[200];int o=0;t[0]=0;for(uint32_t i=0;i<n && o<(int)sizeof t-24;i++)o+=snprintf(t+o,sizeof t-size_t(o),"%s%llu",i?", ":"",(unsigned long long)pick[i]->source.serial);
    if(exec)for(uint32_t i=0;i<n;i++)pclear_flush_image(*pick[i],0);   // as the member's read would (pc_flush_why 0), only earlier: outside M
    s.pflush+=n;++s.pflush_m;
    if(!pf_told){pf_told=true;xbg_logf("[nr] onchip %s %u pending clear(s) before M (image serial %s): the %s sampled %s last time; %s",exec?"flushed":"would flush",n,t,
        fog?"Light / Fog draws":"Light draw",n>1?"them":"it",exec?"landed outside M, before its full barrier, so the member's descriptor build does not end M":"check: nothing landed, the old passes run");}
    return n;}
bool begin(Pass &p,Commands &c,const p_nb_pass &hd){
    VkImageView v[3]{};VkFormat f[3]{};const int why=sky_ok(p,hd,v,f);if(why){++s.refused[why];return false;}
    Image *ci=nullptr;bool tl=false;
    if(fog_on && nb_oc_tail(hd.oc)){++s.tails;const int tw=tail_ok(p,hd,ci,v[2],f[2]);if(tw)++s.tail_refused[tw];else tl=true;}
    VkCommandBuffer cmd=c.command;end_open_pass();
    // Sky's pending ClearTexture clears (DM_GFX_CLEAR_FOLD / DM_NB_RP) that the plan folded (header load CLEAR) become M's CLEAR load ops;
    // any other pending clear of its attachments lands first (then LOAD). Taken only once M's render pass exists.
    VkAttachmentLoadOp ld[2]={VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_LOAD_OP_LOAD};VkClearValue cv[3]{};uint32_t clr=0;
    for(int k=0;k<2;k++){Image *im=p.images[k];
        for(size_t i=0;i<pclears.size();i++)if(pclears[i].im==im){const PendingClear &e=pclears[i];
            if(hd.load[k]==NB_LOAD_CLEAR && e.aspects==VK_IMAGE_ASPECT_COLOR_BIT && e.fmt==f[k]){ld[k]=VK_ATTACHMENT_LOAD_OP_CLEAR;cv[k]=e.value;clr|=3u<<(2*k);}
            else pclear_flush_image(*im,3);   // the plan LOADs it / another format: landed, then LOAD
            break;}}
    if(tl)for(size_t i=0;i<pclears.size();i++)if(pclears[i].im==ci){pclear_flush_image(*ci,3);break;}   // [S2F] c0 loads: a clear still pending lands first
    Rp *r=get_rp(f[0],f[1],ld[0],ld[1],tl?f[2]:VK_FORMAT_UNDEFINED);if(!r && tl){++s.refused[R_RP];tl=false;r=get_rp(f[0],f[1],ld[0],ld[1]);}
    if(!r){++s.refused[R_RP];return false;}   // nothing taken: the normal path begins Sky with its pending clears
    pre_flush(p.images[0],p.images[1],tl?ci:nullptr,tl,true);   // [early clears] the members' sampled images: their pending clears land now, outside M
    for(int k=0;k<2;k++)if(ld[k]==VK_ATTACHMENT_LOAD_OP_CLEAR)for(size_t i=0;i<pclears.size();i++)if(pclears[i].im==p.images[k]){pclears.erase(pclears.begin()+long(i));pclear_n=pclears.size();++pc_folded;break;}
    if(mode==3)for(int k=0;k<2;k++)if(ld[k]!=VK_ATTACHMENT_LOAD_OP_LOAD){Image &im=*p.images[k];full_barrier(cmd);
        const VkClearColorValue mg{{1.f,0.f,1.f,1.f}};const VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd,im.image,VK_IMAGE_LAYOUT_GENERAL,&mg,1,&rg);++s.poison;}
    full_barrier(cmd);   // every write before M (G-buffer, linear depth, shadow mask, exposure) is visible to every subpass's reads and to c0's load
    cur=r;Image *ims[3]={p.images[0],p.images[1],ci};fb=get_fb(r->rp,v,tl?3u:2u,p.width,p.height,ims);sp=p.images[0];df=p.images[1];sp_view=v[0];df_view=v[1];sp_fmt=f[0];df_fmt=f[1];w=p.width;h=p.height;
    tail=tl;c0=tl?ci:nullptr;c0_view=tl?v[2]:VK_NULL_HANDLE;c0_fmt=tl?f[2]:VK_FORMAT_UNDEFINED;if(tl)++s.tail_begun;
    if(nbtr::mode>0)nbtr::decide(p,0,0);   // [DOS2] NB-19: every store STORE (nothing transient)
    if(hist_on)hist_begin(p,clr,0,'n');
    open_pass_marks(p,0,0);lrz_touch_before(p,cmd);pass_prof_begin(p,cmd);
    VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=r->rp;b.framebuffer=fb;b.renderArea.extent={p.width,p.height};b.clearValueCount=tl?3u:2u;b.pClearValues=cv;
    vkCmdBeginRenderPass(cmd,&b,VK_SUBPASS_CONTENTS_INLINE);
    cpf::pass_open(p.width,p.height,p.color_slots,false);   // [NR-16] DM_NR_CPU
    open_pass.cmd=cmd;open_pass.pass=r->rp;open_pass.framebuffer=fb;open_pass.images=p.images;open_pass.depth_attached=false;open_pass.dirty_begin=open_pass.dirty_begin_f=false;
    if(nrrp::mode)nrrp_open(p);++perf.passes;
    active=true;nb_oc_apply(&run,NB_OCS_BEGIN,hd.seq,hd.node);++s.begun;if(nb_oc_len(hd.oc)==3 && !nb_oc_tail(hd.oc))++s.s3;
    return true;}
// [S2F] the images of this part's host passes by IR id (c0 for a tail chain's Sky): kept while the switch is on, cheap (framebuffer changes only)
void note_pass(const Pass &p,const p_nb_pass *hd){
    if(!fog_on || !hd)return;
    if(hd->seq!=att_seq){att_by_key.clear();att_seq=hd->seq;att_fb=VK_NULL_HANDLE;}
    if(p.framebuffer==att_fb)return;att_fb=p.framebuffer;
    for(Image *im:p.images)if(im && im->aspects==VK_IMAGE_ASPECT_COLOR_BIT)att_by_key[nbchk::key(*im)]=im;}
// After create_pass of a draw / an in-pass clear (and before the open-pass check): begin / continue / step M, or end it. false: skip the draw.
bool adjust(Pass &p,Commands &c){
    nb_oc_run &r=mode==2?shadow:run;const p_nb_pass *hd=nbrp::span_hdr();
    note_pass(p,hd);
    const int st=nb_oc_step(&r,hd!=nullptr,hd?hd->oc:0,hd?hd->seq:0,hd?hd->node:-1);
    if(mode==2){
        if(st==NB_OCS_BEGIN){++s.heads;VkImageView v[3]{};VkFormat f[3]{};const int why=sky_ok(p,*hd,v,f);
            if(why){++s.refused[why];return true;}
            shadow_tail=false;Image *ci=nullptr;
            if(fog_on && nb_oc_tail(hd->oc)){++s.tails;const int tw=tail_ok(p,*hd,ci,v[2],f[2]);if(tw)++s.tail_refused[tw];else shadow_tail=true;}
            const VkAttachmentLoadOp l0=hd->load[0]==NB_LOAD_CLEAR?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD,l1=hd->load[1]==NB_LOAD_CLEAR?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD;
            if(!get_rp(f[0],f[1],l0,l1,shadow_tail?f[2]:VK_FORMAT_UNDEFINED)){++s.refused[R_RP];return true;}
            pre_flush(p.images[0],p.images[1],shadow_tail?ci:nullptr,shadow_tail,false);   // [early clears] check: counted as would
            sp=p.images[0];df=p.images[1];c0=ci;sp_fmt=f[0];df_fmt=f[1];c0_fmt=f[2];w=p.width;h=p.height;nb_oc_apply(&shadow,NB_OCS_BEGIN,hd->seq,hd->node);++s.begun;
            if(shadow_tail)++s.tail_begun;else if(nb_oc_len(hd->oc)==3 && !nb_oc_tail(hd->oc))++s.s3;}
        else if(st==NB_OCS_NEXT && shadow.k==0){if(shadow_ok){nb_oc_apply(&shadow,NB_OCS_NEXT,hd->seq,hd->node);++s.chains;s.mb+=double(w)*h*(bpp(sp_fmt)+2.*bpp(df_fmt))/1e6;if(!shadow_tail)nb_oc_apply(&shadow,NB_OCS_BREAK,0,-1);}
            else{++s.brk[B_LIGHT_CLEAR];nb_oc_apply(&shadow,NB_OCS_BREAK,0,-1);}}
        else if(st==NB_OCS_NEXT && shadow.k==1){if(shadow_ok && shadow_tail){++s.fogs;s.mb+=double(w)*h*2.*bpp(df_fmt)/1e6;}else ++s.brk[B_FOG_FS];nb_oc_apply(&shadow,NB_OCS_BREAK,0,-1);shadow_tail=false;c0=nullptr;}
        else if(st==NB_OCS_BREAK){++s.brk[shadow.k?B_OTHER:B_AFTER_SKY];nb_oc_apply(&shadow,NB_OCS_BREAK,0,-1);shadow_tail=false;c0=nullptr;}
        return true;}
    if(nexting && st!=NB_OCS_NEXT){red("M ended between the checks of a twin draw and the draw (header %s, open pass %s): the draw runs again as its own pass",
        hd?"present":"none",active?"M":"another");if(active)brk(B_LOST);else ++s.brk[B_LOST];nexting=false;return false;}   // false: draw() puts the game's shader back and draws it (redo)
    switch(st){
    case NB_OCS_NONE:return true;
    case NB_OCS_BEGIN:++s.heads;if(begin(p,c,*hd))substitute(p,0);return true;
    case NB_OCS_SAME:
        if(!active || open_pass.cmd!=c.command || open_pass.pass!=cur->rp){brk(B_CMD);return true;}
        if(r.k==0?(p.images.size()!=2 || p.images[0]!=sp || p.images[1]!=df):r.k==1?(p.images.size()!=1 || p.images[0]!=df):(p.images.size()!=1 || p.images[0]!=c0)){brk(B_OTHER);return true;}
        substitute(p,r.k);return true;
    case NB_OCS_NEXT:
        if(r.k==1 && !tail){brk(B_OTHER);return true;}   // [S2F] a tail mark, but M began without the Fog subpass: Fog runs as its own pass
        if(!nexting){brk(pre_why?pre_why:r.k==0?B_LIGHT_CLEAR:B_FOG_CLEAR);return true;}   // pre() refused the draw, or a clear in its span
        if(input_bad){brk(B_INPUT);return false;}   // a twin's input binding saw another image: the draw cannot run in M
        {Image *want=r.k==0?df:c0;
         if(!active || open_pass.cmd!=c.command || open_pass.pass!=cur->rp || p.images.size()!=1 || p.images[0]!=want){brk(B_LOST);red("M is not open at the twin draw it prepared: the draw runs again as its own pass");return false;}}
        vkCmdNextSubpass(c.command,VK_SUBPASS_CONTENTS_INLINE);nb_oc_apply(&run,NB_OCS_NEXT,hd->seq,hd->node);
        if(run.k==2){   // [S2F] the engine's ClearTexture of c0 (pending since Light): a clear of the subpass's colour 0 before Fog
            for(size_t i=0;i<pclears.size();i++)if(pclears[i].im==c0){VkClearAttachment a{};a.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;a.colorAttachment=0;a.clearValue=pclears[i].value;
                const VkClearRect rc{{{0,0},{w,h}},0,1};vkCmdClearAttachments(c.command,1,&a,1,&rc);pclears.erase(pclears.begin()+long(i));pclear_n=pclears.size();++s.fog_clears;break;}
            open_pass.images.push_back(c0);   // c0 is now written in M: reads of it end M (prepare_read)
            s.mb+=double(w)*h*2.*bpp(df_fmt)/1e6;}
        else s.mb+=double(w)*h*(bpp(sp_fmt)+2.*bpp(df_fmt))/1e6;
        substitute(p,uint8_t(run.k));return true;
    default:brk(B_OTHER);return true;}}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;frames0=stats.presents.load();return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const uint64_t fr=stats.presents.load();const double f=fr>frames0?double(fr-frames0):1.;frames0=fr;
    if(!s.heads && !s.changed && !s.reds)return;
    char rf[520],bk[700],tr[320];int a=0,b=0,t=0;rf[0]=bk[0]=tr[0]=0;
    for(int i=0;i<R_N;i++)if(s.refused[i] && a<(int)sizeof rf-80)a+=snprintf(rf+a,sizeof rf-size_t(a),"%s%s %llu",a?", ":"",rname(i),(unsigned long long)s.refused[i]);
    if(s.sz[4] && a<(int)sizeof rf-80)a+=snprintf(rf+a,sizeof rf-size_t(a)," (first: Sp %ux%u, Df %ux%u, pass %ux%u)",s.sz[0],s.sz[1],s.sz[2],s.sz[3],s.sz[4],s.sz[5]);
    for(int i=0;i<B_N;i++)if(s.brk[i] && b<(int)sizeof bk-80)b+=snprintf(bk+b,sizeof bk-size_t(b),"%s%s %llu",b?", ":"",bname(i),(unsigned long long)s.brk[i]);
    for(int i=0;i<T_N;i++)if(s.tail_refused[i] && t<(int)sizeof tr-80)t+=snprintf(tr+t,sizeof tr-size_t(t),"%s%s %llu",t?", ":"",tname(i),(unsigned long long)s.tail_refused[i]);
    char es[520];int e=0;es[0]=0;   // early ends by site (since the start): line / op / member / read
    for(int i=0;i<nsites && e<(int)sizeof es-100;i++){const Site &x=sites[i];
        e+=snprintf(es+e,sizeof es-size_t(e),"%sL%d op%u k%d%s%s%s %llu",e?", ":"",x.line,x.op,x.k,x.role>=-1?" read ":"",x.role>=-1?(x.role==0?"Sp":x.role==1?"Df":x.role==2?"c0":"other"):"",
            x.role>=-1?(x.twin?" (twin)":""):"",(unsigned long long)x.n);}
    xbg_logf("[nr] onchip DM_NR_ONCHIP=%d%s: a frame: marked heads %.2f, M begun %.2f (S3-marked %.2f: PCF kept as its own pass), Sky -> Light chains %.2f; "
        "S2F (Fog %s): tail heads %.2f, begun with the Fog subpass %.2f, Fog drawn in M %.2f (c0 clears in M %.2f), tail refused: %s; "
        "DRAM traffic not moved (model, own pixels): %.1f MB a frame; pending clears flushed before M%s %.2f a frame (in %.2f of the Ms); begin refused: %s; ended early: %s; early-end sites (total): %s; twin draws drawn again as their own pass %llu; "
        "input-attachment descriptors %llu (another image bound %llu); magenta poison clears %llu; red lines %llu",mode,mode==2?" (check: would)":"",double(s.heads)/f,double(s.begun)/f,double(s.s3)/f,double(s.chains)/f,
        fog_on?"on":"off",double(s.tails)/f,double(s.tail_begun)/f,double(s.fogs)/f,double(s.fog_clears)/f,t?tr:"none",s.mb/f,mode==2?" (would)":"",double(s.pflush)/f,s.begun?double(s.pflush_m)/double(s.begun):0.,a?rf:"none",b?bk:"none",e?es:"none",(unsigned long long)s.redraws,
        (unsigned long long)s.inputs,(unsigned long long)s.input_other,(unsigned long long)s.poison,(unsigned long long)s.reds);
    const uint64_t reds=s.reds;s=Stats{};s.reds=reds;}
}
