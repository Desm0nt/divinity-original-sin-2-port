// [DOS2] NB-18b-4b: the post-tail fusion in the backend (DM_NB_POST=1/2/3, default 0 = nothing here runs; docs/dos2/NATIVE_BACKEND_PLAN.md
// "NB-18b-4 findings"). Included by xbg_game.cpp after xbg_nb.inc; hooks: OP_BIND_PROGRAM (nbpost_publish: the BE -> FE program table of
// dm/gfx/nb/nb_post.h), the top of draw() (nbpost::pre / post), load_shader_key (fused keys read through nbpost::read_bundle).
// The front end (hle_dos2_nb_post.c, nb_post_plan) marks the pair in the PASS headers: NB_NP_ELIDE on the first pass E (PPApplyLUT for F1,
// PPVignette for F2), NB_NP_FUSE on the next pass H (EoCSight / CombineUI_NoFetch). The bundle "F1.<host key>" / "F2.<host key>" (XGF1,
// dm/gfx/nb/nb_fused.h) comes from <shaders>/native-post.xgspack (xgs_pack v1, = work/shaders-native-post/fused.xgspack), else from
// native::get (native.xgspack / native/<key>.xgs). Its extra bindings (source 0xF0) are rewritten at load to spare state slots of the
// fragment stage (textures 120.., uniform buffers 15, 14; support buffer = the host draw's own) and fed from the stash of E's draw.
//   2  check: both passes run as recorded; at E the stash is taken and every precondition checked (bundle parses, host head equals the
//      translated bundle, the fused module + its descriptor set layout build, requirements), at H the host side; "[nb] post: fused ok N".
//   1  E's draw is skipped (its state records ran; no host pass begins), H draws the fused shader with the stash. Every precondition is
//      checked at E (a failure there: the chain runs as recorded, counted by reason); the host side at H can only be counted then
//      ("after the elide"; a different host key draws the original shader over a stale input - logged).
//   3  = 1 + E's target is cleared to magenta when E is skipped: any stale read of it shows.
// Lines "[nb] post: ..." every 10 s (with the [nb] check lines).
// Phone fix (16.11): F1's VPtoRTScaleBias requirement is NB_FX_R_VP_MAP (viewport / size of O, the engine's own formula), not UBO_EQ
// (1,1,0,0); H's viewport (with scissor) must lie inside E's. A refused requirement logs the floats it read (8 lines per reason).
// NB-18b-8 (route A, DM_NB_POST_AA=1 on top of DM_NB_POST, default 0; plan: dm/gfx/nb/nb_post_aa.h): the header bit NB_NP_AA.
//   E (PPApplyLUT) with ELIDE | AA: the stash is taken for the bundle "F1A*.<host key>" (FXAA on the pre-LUT image -> LUT -> Sight;
//      DM_NB_POST_LUMA picks the luma proxy: 0 = F1A linear light, 1 = F1AS sqrt, 2 = F1AR sqrt with a shoulder; logged). Its HDR extra
//      carries NB_FX_E_LINEAR: the stashed handle gets a linear clamp sampler found in the bound sampler pool (none: the chain runs).
//      F1A not usable: plain F1 is tried instead (then the console's FXAA pass runs: it is elided only after an F1A draw).
//   H (EoCSight) with FUSE | AA: as F1, with the stash's bundle.
//   the AA pass (PPFXAA, or the PPDefault copy at AntiAliasing 0) with ELIDE | AA: skipped when F1A was drawn into the texture it samples
//      in this submission (the copy: also without), same size / format / coverage. From then on "the content of its target O2 is in
//      Sight's target S" (al): a draw of a pass flagged AA whose fragment stage samples O2 is drawn with that slot pointing at S
//      (texture id of the AA pass's input, the draw's own sampler), in this and later submissions, until a real draw into O2 or S.
//      Refused at the AA pass after F1A was drawn: the console's FXAA runs over F1A's output for that frame (counted).
//   =2 counts what would be done (the chain runs as recorded); =3 also poisons O2 when the AA pass is skipped.
// The console's FXAA stays on in the engine: every refusal falls back to the recorded chain in the same frame, AA included.
// NB-18b-9 (phone, 17.11): below the full dynamic resolution the engine's post passes draw a part of their targets (viewport 1620x729
//   of 2160x972 at 75 %) and the console's FXAA pass is the step that scales that part up to its whole target - it cannot be elided
//   (its reader would have to sample a part of Sight's target). Before: F1A was drawn, the AA pass was refused by its coverage and FXAA
//   ran over F1A's output (double AA in every such frame). Now F1A is taken only when the LUT's draw covers its whole target; else
//   plain F1 (reason "F1A not usable, plain F1 instead: the LUT draws a part of its target ..."). The same at Sight's draw (the stash
//   goes back to plain F1 with the pre-LUT handle it had before the linear sampler was put in).
// NB-18b-10 (ship deck: LUT -> Sight -> EoCFog -> FXAA): a pass of a carry class (nb_post_aa_carry: EoCFog) flagged AA alone, drawn in a
//   submission where F1A was drawn, that samples F1A's output and writes another target of the same size / format over the same pixels
//   carries it on: "F1A's output" (la.S) is its target from then on, so the AA pass that samples that target is skipped and its readers
//   sample it. The carry pass itself runs as recorded. Refused: la is dropped, the console's FXAA runs (over F1A's output, counted).
#include "../../nb/nb_fused.h"
#include "../../nb/nb_post.h"
#include "../../nb/nb_post_aa.h"
namespace nbpost{
int mode=-1;bool inside=false;
int aa=0,luma=0,aa_outline=0;   // DM_NB_POST_AA, DM_NB_POST_LUMA, NR-33A DM_NB_POST_AA_OUTLINE (read in on())
enum{FXA=NB_FXK_KINDS,NFX};   // F[]: NB_FXK_F1, NB_FXK_F2, FXA = the F1A bundle of the chosen luma proxy (host = F1's)
const char *const luma_name[3]={"g (linear light)","sqrt(g)","sqrt(g / (g + 1))"};const char *const luma_fx[3]={"F1A.","F1AS.","F1AR."};
enum{SPARE_TEX=120,SPARE_UBO=15};
struct Fx{bool tried=false,ok=false;std::string key,why;nb_fx fx{};std::vector<uint8_t> blob,bundle;Shader *sh=nullptr;
    int spare[NB_FX_MAX_EXTRA]{};uint64_t built=0;bool linear=false;};
Fx F[NFX];std::string host_key[NB_FXK_KINDS];   // the translated key of each fx's host shader (seen at publication)
const std::string &hkey(int k){return host_key[k==FXA?int(NB_FXK_F1):k];}
struct Stash{bool valid=false,skipped=false;const void *spool=nullptr;int64_t spool_off=0;int fx=0;uint32_t seq=0;int node=0;uint64_t O=0;uint32_t ow=0,oh=0;const void *pool=nullptr;int64_t pool_off=0;
    uint64_t tex[NB_FX_MAX_EXTRA]{},tex0[NB_FX_MAX_EXTRA]{};NVNbufferRange ub[NB_FX_MAX_EXTRA]{};double rect[4]{};} st;   // tex0: the handles as bound (tex: + the linear sampler of F1A)
struct Saved{bool fused=false;Shader *fs=nullptr;uint64_t tex[8]{};NVNbufferRange ub[2]{};int nr=0;uint32_t rslot[16]{};uint64_t rh[16]{};} sv;
std::map<std::string,uint64_t> why;uint64_t n_elide=0,n_skip=0,n_fused=0,n_ok=0,n_late=0,n_poison=0,n_nostash=0,next_ns=0;
// NB-18b-8. la: the F1A draw of this submission (its target S, what it covered). al: O2's content lives in S.
struct LastA{bool ok=false;uint32_t seq=0;uint64_t S=0;double rect[4]{};uint32_t carry_seq=0;int carry_node=-1;} la;   // S: where F1A's output is now (Sight's target, then a carry pass's)
struct Alias{bool valid=false,would=false;uint64_t O2=0,S=0;uint32_t tex_id=0,w=0,h=0,seq=0;int node=0;const void *pool=nullptr;int64_t pool_off=0;} al;
uint64_t a_e=0,a_f1a=0,a_f1a_ok=0,a_plain=0,a_back=0,a_step=0,a_step_skip=0,a_step_ok=0,a_step_late=0,a_redir=0,a_redir_would=0,a_drop=0,a_carry=0,a_carry_ok=0;
int pfd=-1;std::vector<xpack::Ent> pidx;bool popen=false;
void tick();
// DM_NB_POST / DM_NB_POST_AA / DM_NB_POST_LUMA as on() reads them, without touching the state (any thread: the warm-up's workers).
void settings(int &m,int &a_,int &l_){const char *e=getenv("DM_NB_POST");const int v=e && *e?atoi(e):0;m=v>=1 && v<=3?v:0;
    const char *a=getenv("DM_NB_POST_AA"),*l=getenv("DM_NB_POST_LUMA");a_=m && a && a[0]=='1' && !a[1];l_=l && l[0]>='0' && l[0]<='2' && !l[1]?l[0]-'0':0;}
int on(){if(mode<0){const char *e=getenv("DM_NB_POST");const int m=e && *e?atoi(e):0;mode=m>=1 && m<=3?m:0;
    if(mode)xbg_logf("[nb] DM_NB_POST=%d: post-tail fusion in the backend (%s); fused bundles from %s/native-post.xgspack or native::get",mode,
        mode==2?"check only, the chain runs as recorded":mode==1?"elide + fused draw":"elide + fused draw + magenta poison of the elided target",cfg.shader_directory?cfg.shader_directory:"-");
    const char *a=getenv("DM_NB_POST_AA"),*l=getenv("DM_NB_POST_LUMA");aa=mode && a && a[0]=='1' && !a[1];{const char *o=getenv("DM_NB_POST_AA_OUTLINE");aa_outline=aa && o && o[0]=='1' && !o[1];}luma=l && l[0]>='0' && l[0]<='2' && !l[1]?l[0]-'0':0;
    if(aa)xbg_logf("[nb] DM_NB_POST_AA=1 (route A): bundle %s<Sight key> = FXAA on the pre-LUT image -> LUT -> Sight; luma proxy DM_NB_POST_LUMA=%d = %s%s; "
        "the console's FXAA pass after Sight is elided only in a submission where F1A was drawn, else it runs as recorded",luma_fx[luma],luma,luma_name[luma],
        l && *l && (l[0]<'0' || l[0]>'2' || l[1])?" (value not understood: 0 / 1 / 2)":"");}
    return mode;}
uint64_t key64(const std::string &k){return k.size()>=16?strtoull(k.substr(0,16).c_str(),nullptr,16):0;}
int fx_of_host(int cls){return cls==NB_PSH_SIGHT?NB_FXK_F1:cls==NB_PSH_COMB?NB_FXK_F2:0;}
int fx_of_elided(int cls){return cls==NB_PSH_LUT?NB_FXK_F1:cls==NB_PSH_VIG?NB_FXK_F2:0;}
bool fused_name(const std::string &k){if(k.size()<=3 || k[0]!='F' || (k[1]!='1' && k[1]!='2'))return false;   // "F1." "F2." "F1A." "F1AS." "F1AR." (any mode)
    const size_t d=k.find('.');return d!=std::string::npos && d>=2 && d<=4;}
bool fused_key(const std::string &k){return mode>0 && fused_name(k);}
bool read_bundle(const std::string &k,std::vector<uint8_t> &out){for(auto &f:F)if(f.ok || f.tried)if(f.key==k && !f.bundle.empty()){out=f.bundle;return true;}return false;}
std::once_flag ponce;   // NR-0407 fix: pack_get runs on the warm-up's workers too (warm_bundle)
bool pack_get(const std::string &key,std::vector<uint8_t> &out){
    std::call_once(ponce,[]{popen=true;if(cfg.shader_directory){const std::string path=std::string(cfg.shader_directory)+"/native-post.xgspack";int f=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
        if(f>=0){struct stat sb{};uint32_t h[4]={};bool ok=!fstat(f,&sb) && pread(f,h,16,0)==16 && h[0]==0x50534758u && h[1]==1u && h[2]<4096u;std::vector<xpack::Ent> v;
            if(ok){v.resize(h[2]);ok=pread(f,v.data(),v.size()*sizeof(xpack::Ent),16)==ssize_t(v.size()*sizeof(xpack::Ent));}
            for(size_t i=0;ok && i<v.size();i++)ok=v[i].key[31]==0 && v[i].off+v[i].size<=uint64_t(sb.st_size);
            if(ok){pfd=f;pidx.swap(v);xbg_logf("[nb] post: %s, %zu fused bundles",path.c_str(),pidx.size());}else{::close(f);xbg_logf("[nb] post: %s damaged: ignored",path.c_str());}}
        else xbg_logf("[nb] post: no %s (native::get only)",path.c_str());}});
    if(pfd>=0)for(const auto &e:pidx)if(key==e.key){out.resize(e.size);if(pread(pfd,out.data(),out.size(),off_t(e.off))==ssize_t(out.size()) && xpack::crc(out.data(),out.size())==e.hash)return true;return false;}
    return xpack::native::get(key,out);}
// The fused bundle of key ("F1." / "F2." / "F1A*." + the host's translated key): read (native-post.xgspack or native::get), parsed, its host
// head checked against the translated host bundle, the extras put on the spare slots (x.blob, x.fx, x.bundle, x.spare, x.linear). No
// shared state but the pack (opened once): any thread - prepare() on the render thread and warm_bundle() on the warm-up's workers build
// the same bytes. false: why says why.
bool compose(const std::string &key,const std::string &host,Fx &x,std::string &why){
    const char *w=nullptr;auto fail=[&](const std::string &s){why=s;return false;};
    if(!pack_get(key,x.blob))return fail("no bundle");
    if(nb_fx_parse(x.blob.data(),x.blob.size(),&x.fx,&w))return fail(std::string("XGF1 ")+w);
    if(x.fx.n_elided!=1)return fail("not one elided pass");
    {std::vector<uint8_t> tb;bool fp=false;if(!xpack::read_translated(host,tb,fp) || !nb_fx_host_head_matches(&x.fx,tb.data(),tb.size()))return fail("host head differs from the translated bundle (rebuild with tools/dos2_post_fuse.py)");}
    x.bundle.assign(x.fx.bundle,x.fx.bundle+x.fx.bundle_size);
    int nt=0,nu=0;
    for(uint32_t i=0;i<x.fx.host_bindings;i++){xbg_shader_binding b;memcpy(&b,x.bundle.data()+32+32*i,32);
        if((b.source==XBG_SOURCE_TEXTURE && b.slot+b.count>SPARE_TEX) || (b.source==XBG_SOURCE_UNIFORM && b.slot>=SPARE_UBO-1))return fail("host binding on a spare slot");}
    for(uint32_t i=0;i<x.fx.n_extra;i++){const nb_fx_extra &e=x.fx.extra[i];uint8_t *ent=x.bundle.data()+32+32*(x.fx.host_bindings+i);uint32_t src=0,slot=0;
        if(e.src_kind==NB_FX_K_TEX){if(nt>=8)return fail("too many extra textures");src=XBG_SOURCE_TEXTURE;slot=SPARE_TEX+nt++;}
        else if(e.src_kind==NB_FX_K_CBUF){if(nu>=2)return fail("too many extra c-buffers");src=XBG_SOURCE_UNIFORM;slot=SPARE_UBO-nu++;}
        else{src=XBG_SOURCE_SUPPORT;slot=0;}   // the support buffer of the host draw (render scale: E and H have one size, checked)
        if(e.src_slot>=(e.src_kind==NB_FX_K_TEX?128u:16u))return fail("extra source slot");
        if(e.flags&NB_FX_E_LINEAR){if(e.src_kind!=NB_FX_K_TEX)return fail("NB_FX_E_LINEAR on a buffer");x.linear=true;}   // NB-18b-8: fed with a linear clamp sampler (elide())
        memcpy(ent+16,&src,4);memcpy(ent+20,&slot,4);x.spare[i]=int(slot);}
    return true;}
// [DOS2] NR-0407 fix: the bundle of a fused key for the boot warm-up (warm::bundle_words, any thread). The warm-up list records the fused
// draws' pipelines with their stage keys ("F1.<host>", "F1A.<host>"), but the warm build read every stage from the game's shader pack
// (xpack::read), which has no fused bundle: each such record failed at every boot and the draw built the pipeline in the frame (phone run
// nr4_b2: records 1183 / 1188 / 1223 / 1240 'failed'). Here they are composed as prepare() does, from the key alone (its host key is the
// part after the '.'); only the bundles the current DM_NB_POST / _AA / _LUMA would draw (the others' records are not built, as an
// alpha-test record while the alpha test is off). false: not a fused key / not drawn now / why says why (first 8 logged).
std::atomic<unsigned> warm_told{0};std::atomic<uint64_t> warm_ok{0},warm_refused{0};
bool warm_bundle(const std::string &key,std::vector<uint8_t> &out){
    if(!fused_name(key))return false;const size_t d=key.find('.');
    int m=0,a=0,l=0;settings(m,a,l);std::string why;const std::string prefix=key.substr(0,d+1),host=key.substr(d+1);
    if(!m)why="DM_NB_POST off: no fused draw";
    else if(prefix!="F1." && prefix!="F2." && (!a || prefix!=luma_fx[l]))why=a?"another DM_NB_POST_LUMA's bundle":"DM_NB_POST_AA off: no F1A draw";
    else if(host.size()<16)why="no host key";
    else{Fx x;if(compose(key,host,x,why)){out=x.bundle;
            if(++warm_ok<=4)xbg_logf("[nb] post: warm-up record stage %s composed (%zu bytes, %u extra bindings): its pipeline is built by the warm-up, not in the frame",key.c_str(),out.size(),x.fx.n_extra);
            return true;}}
    ++warm_refused;if(warm_told.fetch_add(1)<8)xbg_logf("[nb] post: warm-up record stage %s not built: %s",key.c_str(),why.c_str());
    return false;}
// Load + validate + rewrite the extras of fx kind k once (its host key known from the program table). false: F[k].why says why.
bool prepare(int k){Fx &x=F[k];if(x.tried)return x.ok;if(hkey(k).empty()){x.why="host shader not seen yet";return false;}
    x.tried=true;x.key=std::string(k==NB_FXK_F1?"F1.":k==FXA?luma_fx[luma]:"F2.")+hkey(k);
    auto fail=[&](const std::string &s){x.why=s;xbg_logf("[nb] post: %s unusable: %s (the chain runs as recorded)",x.key.c_str(),s.c_str());return false;};
    {std::string why;if(!compose(x.key,hkey(k),x,why))return fail(why);}
    x.sh=load_shader_key(x.key,nullptr,nullptr);
    if(!x.sh || !x.sh->module || !x.sh->unusable.empty() || x.sh->stage!=4)return fail("fused module did not build");
    {std::vector<VkDescriptorSetLayoutBinding> lb;for(const auto &b:x.sh->bindings){VkDescriptorSetLayoutBinding l{};l.binding=b.binding;l.descriptorType=VkDescriptorType(b.type);
            l.descriptorCount=b.count;l.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;lb.push_back(l);}
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=uint32_t(lb.size());li.pBindings=lb.data();VkDescriptorSetLayout l{};
        if(vkCreateDescriptorSetLayout(cfg.device,&li,nullptr,&l)!=VK_SUCCESS)return fail("descriptor set layout of the extended table");vkDestroyDescriptorSetLayout(cfg.device,l,nullptr);}
    x.ok=true;xbg_logf("[nb] post: %s ready (%u extra bindings, %u requirements, flags %#x%s%s)",x.key.c_str(),x.fx.n_extra,x.fx.n_req,x.fx.flags,
        k==FXA?"; route A, luma proxy ":"",k==FXA?luma_name[luma]:"");return true;}
// NB-18b-8: a sampler of the bound sampler pool with linear min / mag filters, clamp to edge, no compare (NVN: filter 1 / 3 / 5 = linear
// [mipmap], wrap 7 = clamp to edge, 0 = clamp) - the id the fused shader's HDR taps use. 0: none registered.
uint32_t linear_sid(){static const void *pool=nullptr;static int64_t off=0;static uint32_t sid=0;
    auto good=[](const samp_par &p){return p.magf==1 && (p.minf==1 || p.minf==3 || p.minf==5) && (p.ws==7 || p.ws==0) && (p.wt==7 || p.wt==0) && !p.cmp_mode && p.lod_bias==0.f && p.lod_min<=0.f;};
    auto f=samplers.find(pool_key(&state.sampler_pool));const auto &tab=f==samplers.end()?legacy_samplers:f->second;
    if(sid && pool==state.sampler_pool.pool && off==state.sampler_pool.off){auto t=tab.find(int(sid));if(t!=tab.end() && good(t->second))return sid;}
    pool=state.sampler_pool.pool;off=state.sampler_pool.off;sid=0;
    for(const auto &e:tab)if(e.first>0 && e.first<4096 && good(e.second)){sid=uint32_t(e.first);break;}
    {static unsigned told=0;if(told++<4)xbg_logf("[nb] post: linear clamp sampler for the HDR taps: id %u of %zu in the bound pool%s",sid,tab.size(),sid?"":" (none: F1A cannot run)");}
    return sid;}
const tex_t *tex_of(uint64_t handle){const RegisteredTexture *r=nbui::tex_rec(handle);return r?&r->texture:nullptr;}
bool ubo_eq(const nb_fx_req &r,const NVNbufferRange &u){return u.address && nb_fx_req_ubo_ok(&r,reinterpret_cast<const void*>(uintptr_t(u.address)),size_t(u.size));}
int fail(const char *w){++why[w];return 0;}
bool whole_rect(const double r[4],uint32_t w,uint32_t h){return r[0]<=0 && r[1]<=0 && r[2]>=double(w) && r[3]>=double(h);}
void vp_of(float v[4]){v[0]=state.viewport.x;v[1]=state.viewport.y;v[2]=state.viewport.width;v[3]=state.viewport.height;}
// The pixels a full-screen draw of the current state writes in a target of w x h: viewport & scissor & target (x0, y0, x1, y1).
void cover(uint32_t w,uint32_t h,double r[4]){const double sx=state.scissor.offset.x,sy=state.scissor.offset.y;
    r[0]=std::max({0.0,double(state.viewport.x),sx});r[1]=std::max({0.0,double(state.viewport.y),sy});
    r[2]=std::min({double(w),double(state.viewport.x)+state.viewport.width,sx+double(state.scissor.extent.width)});
    r[3]=std::min({double(h),double(state.viewport.y)+state.viewport.height,sy+double(state.scissor.extent.height)});}
// One requirement of pass `pass` (0 host, 1 elided) against the current state; O = st.ow x st.oh. false: refused (`what` = the reason).
std::map<std::string,unsigned> told_req;
bool req_ok(const nb_fx_req &r,const char *&what){what=nullptr;
    if(r.kind!=NB_FX_R_UBO_EQ && r.kind!=NB_FX_R_VP_MAP)return true;
    const NVNbufferRange u=r.slot<16?state.ub[r.stage][r.slot]:NVNbufferRange{};float vp[4];vp_of(vp);float s[4]={0,0,0,0};bool ok;
    if(r.kind==NB_FX_R_UBO_EQ){ok=ubo_eq(r,u);if(u.address && u.size>=r.offset+16ull)memcpy(s,reinterpret_cast<const uint8_t*>(uintptr_t(u.address))+r.offset,16);
        what=r.pass?"elided: UBO requirement (VPtoRTScaleBias)":"host: UBO requirement (VPtoRTScaleBias)";}
    else{ok=u.address && nb_fx_req_vp_ok(&r,reinterpret_cast<const void*>(uintptr_t(u.address)),size_t(u.size),vp,st.ow,st.oh,s);
        what=r.pass?"elided: VPtoRTScaleBias does not map the viewport onto its target 1:1":"host: VPtoRTScaleBias does not map the viewport onto the elided target 1:1";}
    if(ok){what=nullptr;return true;}
    if(told_req[what]++<8)xbg_logf("[nb] post: pass seq=%u node=%d: %s: stage %u c%u+%u = (%g, %g, %g, %g)%s, viewport %g,%g %gx%g, scissor %d,%d %ux%u, "
        "elided target %ux%u%s",nbchk::sp.h.seq,int(nbchk::sp.h.node),what,unsigned(r.stage),unsigned(r.slot+3),unsigned(r.offset),s[0],s[1],s[2],s[3],
        u.address?"":" (not bound)",vp[0],vp[1],vp[2],vp[3],state.scissor.offset.x,state.scissor.offset.y,state.scissor.extent.width,state.scissor.extent.height,
        st.ow,st.oh,r.kind==NB_FX_R_UBO_EQ?" (wants the bundle's 4 floats exactly)":" (wants s.xy * size = viewport size, s.zw * size = its origin)");
    return false;}
void poison(const tex_t *t){Image &im=get_image(t,false);Commands c;im.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkClearColorValue m{};m.float32[0]=1;m.float32[2]=1;m.float32[3]=1;
    VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,im.layers};vkCmdClearColorImage(c.command,im.image,im.layout,&m,1,&r);im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();
    im.gpu_written=true;provenance_write(im,__LINE__);++n_poison;}
// E's draw: stash + every precondition that can be checked here. 1 = skip the draw.
uint64_t alias_handle(uint64_t h);
int elide(bool want_aa){st=Stash{};++n_elide;const Shader *fs=state.shader[4];if(!fs)return fail("elided: no fragment shader");
    int k=fx_of_elided(nb_post_class(key64(fs->key)));if(!k)return fail("elided: shader class");
    if(k==NB_FXK_F1 && want_aa){++a_e;   // NB-18b-8: F1A when it is usable, else plain F1 (the console's FXAA then runs)
        bool whole=false;if(state.rt.n==1 && state.rt.color[0]){const tex_t *t=state.rt.color[0];double r[4];cover(t->d.width,t->d.height,r);whole=whole_rect(r,t->d.width,t->d.height);}
        if(!whole){++a_plain;++why["F1A not usable, plain F1 instead: the LUT draws a part of its target (dynamic resolution: the console's FXAA pass scales it up)"];}
        else if(prepare(FXA) && (!F[FXA].linear || linear_sid()))k=FXA;
        else{++a_plain;++why[std::string("F1A not usable, plain F1 instead: ")+(F[FXA].ok?"no linear clamp sampler in the bound pool":F[FXA].why)];}}
    if(!prepare(k)){fail((std::string("bundle: ")+F[k].why).c_str());return 0;}
    const Fx &x=F[k];if(strncmp(fs->key.c_str(),x.fx.elided_key[0],16))return fail("elided: key differs from the bundle's");
    if(state.rt.n!=1 || !state.rt.color[0] || state.rt.depth)return fail("elided: render targets");
    const tex_t *O=state.rt.color[0];st.O=uint64_t(uintptr_t(O->mem));st.ow=O->d.width;st.oh=O->d.height;
    st.pool=state.texture_pool.pool;st.pool_off=state.texture_pool.off;st.spool=state.sampler_pool.pool;st.spool_off=state.sampler_pool.off;
    for(uint32_t i=0;i<x.fx.n_extra;i++){const nb_fx_extra &e=x.fx.extra[i];const unsigned ns=e.src_stage;   // 0 vertex, 1 fragment = NVN stage
        if(e.src_kind==NB_FX_K_TEX){st.tex[i]=state.tex[ns][e.src_slot];if(!tex_of(st.tex[i]))return fail("elided: extra texture not bound");
            if(ns==1 && mode!=2)st.tex[i]=alias_handle(st.tex[i]);   // NB-18b-8: F2's Vignette samples the elided AA pass's target: its content is in S
            st.tex0[i]=st.tex[i];
            if(e.flags&NB_FX_E_LINEAR)st.tex[i]=(st.tex[i]&0xfffffull)|(uint64_t(linear_sid())<<20);}
        else if(e.src_kind==NB_FX_K_CBUF){st.ub[i]=state.ub[ns][e.src_slot];if(!st.ub[i].address || !st.ub[i].size)return fail("elided: extra c-buffer not bound");}}
    cover(st.ow,st.oh,st.rect);
    for(uint32_t i=0;i<x.fx.n_req;i++){const nb_fx_req &r=x.fx.req[i];if(r.pass!=1)continue;const char *w=nullptr;
        if(!req_ok(r,w))return fail(w);
        if(r.kind==NB_FX_R_SAME_SIZE){const tex_t *in=nullptr;for(uint32_t j=0;j<x.fx.n_extra && !in;j++)if(x.fx.extra[j].src_kind==NB_FX_K_TEX)in=tex_of(st.tex[j]);
            if(!in || in->d.width!=st.ow || in->d.height!=st.oh)return fail("elided: input and target sizes differ");}}
    st.valid=true;st.fx=k;st.seq=nbchk::sp.h.seq;st.node=nbchk::sp.h.node;
    if(mode==2)return 0;
    if(mode==3)poison(O);
    st.skipped=true;++n_skip;return 1;}
// H's draw. 2 = draw it with the fused shader (pre swapped the state; post restores).
int fuse(){
    if(!st.valid || st.seq!=nbchk::sp.h.seq){++n_nostash;return 0;}   // E ran as recorded (counted by its reason): nothing to do
    Stash s=st;st.valid=false;Shader *fs=state.shader[4];bool late=false;
    if(s.fx==FXA && state.rt.n>=1 && state.rt.color[0]){   // NB-18b-9: Sight draws a part of its target: plain F1 (the console's FXAA pass will run, once)
        const tex_t *t=state.rt.color[0];double r[4];cover(t->d.width,t->d.height,r);
        if(!whole_rect(r,t->d.width,t->d.height)){const Fx &a=F[FXA];bool same=prepare(NB_FXK_F1) && F[NB_FXK_F1].fx.n_extra==a.fx.n_extra;
            for(uint32_t i=0;same && i<a.fx.n_extra;i++){const nb_fx_extra &p=a.fx.extra[i],&q=F[NB_FXK_F1].fx.extra[i];
                same=p.src_kind==q.src_kind && p.src_stage==q.src_stage && p.src_slot==q.src_slot && a.spare[i]==F[NB_FXK_F1].spare[i];}
            if(same){s.fx=NB_FXK_F1;memcpy(s.tex,s.tex0,sizeof s.tex);++a_back;++why["F1A stash drawn as plain F1: Sight draws a part of its target (dynamic resolution)"];}
            else ++why["Sight draws a part of its target and F1's extras differ from F1A's: F1A drawn, the console's FXAA runs over it"];}}
    const Fx &x=F[s.fx];
    auto host=[&](const char *w){if(!s.skipped){fail(w);return false;}++n_late;++why[std::string("after the elide: ")+w];late=true;return true;};
    if(!fs || fs->key!=hkey(s.fx) || strncmp(fs->key.c_str(),x.fx.host_key,16)){
        if(s.skipped){++n_late;++why["after the elide: host key differs - drawn with its own shader over a stale input"];static unsigned told=0;
            if(told++<8)xbg_logf("[nb] post: pass seq=%u node=%d: host shader %s is not the bundle's host after the elide",nbchk::sp.h.seq,int(nbchk::sp.h.node),fs?fs->key.c_str():"-");}
        else fail("host: key differs");return 0;}
    if(state.texture_pool.pool!=s.pool || state.texture_pool.off!=s.pool_off){if(!host("host: texture pool changed"))return 0;}
    if(x.linear && (state.sampler_pool.pool!=s.spool || state.sampler_pool.off!=s.spool_off)){if(!host("host: sampler pool changed"))return 0;}
    {bool reads=false;for(const auto &b:fs->bindings)if(b.source==XBG_SOURCE_TEXTURE && b.slot<128){const tex_t *t=tex_of(state.tex[1][b.slot]);
            if(t && uint64_t(uintptr_t(t->mem))==s.O){reads=true;if(t->d.width!=s.ow || t->d.height!=s.oh){if(!host("host: reads a view of another size"))return 0;}}}
        if(!reads && !host("host: does not sample the elided target"))return 0;}
    double hr[4]={0,0,0,0};
    {const Stash keep=st;st=s;   // req_ok / the log read O's size from st
        bool go=true;for(uint32_t i=0;i<x.fx.n_req && go;i++){const nb_fx_req &r=x.fx.req[i];if(r.pass!=0)continue;const char *w=nullptr;
            if(!req_ok(r,w) && !host(w))go=false;}
        cover(s.ow,s.oh,hr);
        if(go && (hr[0]<s.rect[0] || hr[1]<s.rect[1] || hr[2]>s.rect[2] || hr[3]>s.rect[3]) && hr[0]<hr[2] && hr[1]<hr[3]){
            static unsigned told=0;if(told++<8)xbg_logf("[nb] post: pass seq=%u node=%d: host draws %g,%g..%g,%g, the elided pass wrote %g,%g..%g,%g only",
                nbchk::sp.h.seq,int(nbchk::sp.h.node),hr[0],hr[1],hr[2],hr[3],s.rect[0],s.rect[1],s.rect[2],s.rect[3]);
            if(!host("host: draws outside the elided pass's viewport / scissor"))go=false;}
        st=keep;if(!go)return 0;}
    (void)late;
    if(s.fx==FXA && state.rt.n>=1 && state.rt.color[0]){la.ok=true;la.seq=nbchk::sp.h.seq;la.S=uint64_t(uintptr_t(state.rt.color[0]->mem));memcpy(la.rect,hr,sizeof hr);
        if(mode==2)++a_f1a_ok;else ++a_f1a;}
    if(mode==2){++n_ok;return 0;}
    sv=Saved{};sv.fused=true;sv.fs=fs;for(int i=0;i<8;i++)sv.tex[i]=state.tex[1][SPARE_TEX+i];sv.ub[0]=state.ub[1][SPARE_UBO];sv.ub[1]=state.ub[1][SPARE_UBO-1];
    for(uint32_t i=0;i<x.fx.n_extra;i++){const nb_fx_extra &e=x.fx.extra[i];
        if(e.src_kind==NB_FX_K_TEX)state.tex[1][x.spare[i]]=s.tex[i];else if(e.src_kind==NB_FX_K_CBUF)state.ub[1][x.spare[i]]=s.ub[i];}
    state.shader[4]=x.sh;++tex_gen[1];++buf_gen[1];++n_fused;return 2;}
// ---- NB-18b-8: the elided AA pass and its readers ----
// h samples O2 while its content is in S: the same sampler on S's texture id (the id the AA pass's input had in the same texture pool)
uint64_t alias_handle(uint64_t h){if(!al.valid || state.texture_pool.pool!=al.pool || state.texture_pool.off!=al.pool_off)return h;
    const tex_t *t=tex_of(h);if(!t || uint64_t(uintptr_t(t->mem))!=al.O2 || t->d.width!=al.w || t->d.height!=al.h)return h;
    ++a_redir;return (h&~0xfffffull)|al.tex_id;}
// The AA pass's draw (PPFXAA / the PPDefault copy), header ELIDE | AA. 1 = skip it.
int step(int cls){++a_step;const Shader *fs=state.shader[4];const uint32_t seq=nbchk::sp.h.seq;
    if(state.rt.n!=1 || !state.rt.color[0] || state.rt.depth)return fail("AA pass: render targets");
    const tex_t *O=state.rt.color[0];uint64_t h=0;int nt=0;
    for(const auto &b:fs->bindings)if(b.source==XBG_SOURCE_TEXTURE && b.slot<128){if(!nt)h=state.tex[1][b.slot];nt++;}
    const tex_t *B=nt==1?tex_of(h):nullptr;if(!B)return fail("AA pass: not one bound input texture");
    const uint64_t S=uint64_t(uintptr_t(B->mem)),O2=uint64_t(uintptr_t(O->mem));const bool drawn=la.ok && la.seq==seq && la.S==S;
    auto no=[&](const char *w){if(drawn && cls==NB_PSA_FXAA && mode!=2){++a_step_late;++why[std::string("after F1A (the console's FXAA runs over it this frame): ")+w];return 0;}return fail(w);};
    if(cls==NB_PSA_FXAA && !drawn)return fail("AA pass: F1A was not drawn into its input in this submission (the console's FXAA runs)");
    if(S==O2 || B->d.width!=O->d.width || B->d.height!=O->d.height || B->d.format!=O->d.format)return no("AA pass: input and target differ in size / format");
    double r[4];cover(O->d.width,O->d.height,r);
    if(drawn?(r[0]!=la.rect[0] || r[1]!=la.rect[1] || r[2]!=la.rect[2] || r[3]!=la.rect[3]):(r[0]>0 || r[1]>0 || r[2]<O->d.width || r[3]<O->d.height)){
        static unsigned told=0;if(told++<8)xbg_logf("[nb] post: pass seq=%u node=%d: the AA pass covers %g,%g..%g,%g of %ux%u, %s %g,%g..%g,%g",seq,int(nbchk::sp.h.node),r[0],r[1],r[2],r[3],
            O->d.width,O->d.height,drawn?"F1A drew":"a copy must cover",drawn?la.rect[0]:0.0,drawn?la.rect[1]:0.0,drawn?la.rect[2]:double(O->d.width),drawn?la.rect[3]:double(O->d.height));
        return no("AA pass: its coverage differs from the fused draw's");}
    if(mode==2){++a_step_ok;al.would=true;al.O2=O2;al.w=O->d.width;al.h=O->d.height;return 0;}
    al=Alias{};al.valid=true;al.O2=O2;al.S=S;al.tex_id=uint32_t(h&0xfffff);al.w=O->d.width;al.h=O->d.height;al.seq=seq;al.node=int(nbchk::sp.h.node);
    al.pool=state.texture_pool.pool;al.pool_off=state.texture_pool.off;
    if(mode==3)poison(O);
    ++a_step_skip;return 1;}
// A draw of a pass flagged AA alone: its fragment texture slots that sample O2 point at S for this draw. 2 = swapped (post restores).
int redirect(){const Shader *fs=state.shader[4];if(!fs || (!al.valid && !al.would))return 0;
    for(int i=0;i<int(state.rt.n) && i<8;i++)if(state.rt.color[i] && uint64_t(uintptr_t(state.rt.color[i]->mem))==al.S){++why["reader of the aliased image draws into the image that holds it: not redirected"];return 0;}
    if(mode==2){for(const auto &b:fs->bindings)if(b.source==XBG_SOURCE_TEXTURE && b.slot<128){const tex_t *t=tex_of(state.tex[1][b.slot]);
            if(t && uint64_t(uintptr_t(t->mem))==al.O2 && t->d.width==al.w && t->d.height==al.h)++a_redir_would;}
        return 0;}
    sv=Saved{};
    for(const auto &b:fs->bindings)if(b.source==XBG_SOURCE_TEXTURE && b.slot<128 && sv.nr<16){const uint64_t h=state.tex[1][b.slot],n=alias_handle(h);
        if(n!=h){sv.rslot[sv.nr]=b.slot;sv.rh[sv.nr++]=h;state.tex[1][b.slot]=n;++tex_gen[1];}}
    return sv.nr?2:0;}
// NB-18b-10: the carry pass's draw (EoCFog between Sight and the AA pass). Never skips or changes the draw.
void carry(){++a_carry;const Shader *fs=state.shader[4];
    auto no=[&](const char *w){la.ok=false;++why[std::string("carry pass (the console's FXAA runs over F1A's output this frame): ")+w];};
    if(state.rt.n!=1 || !state.rt.color[0] || state.rt.depth)return no("render targets");
    const tex_t *O=state.rt.color[0],*B=nullptr;
    for(const auto &b:fs->bindings)if(b.source==XBG_SOURCE_TEXTURE && b.slot<128){const tex_t *t=tex_of(state.tex[1][b.slot]);if(t && uint64_t(uintptr_t(t->mem))==la.S)B=t;}
    if(!B)return no("does not sample F1A's target");
    const uint64_t T=uint64_t(uintptr_t(O->mem));
    if(T==la.S || B->d.width!=O->d.width || B->d.height!=O->d.height || B->d.format!=O->d.format)return no("input and target differ in size / format");
    double r[4];cover(O->d.width,O->d.height,r);
    if(r[0]!=la.rect[0] || r[1]!=la.rect[1] || r[2]!=la.rect[2] || r[3]!=la.rect[3]){
        static unsigned told=0;if(told++<8)xbg_logf("[nb] post: pass seq=%u node=%d: the carry pass covers %g,%g..%g,%g of %ux%u, F1A drew %g,%g..%g,%g",nbchk::sp.h.seq,int(nbchk::sp.h.node),
            r[0],r[1],r[2],r[3],O->d.width,O->d.height,la.rect[0],la.rect[1],la.rect[2],la.rect[3]);
        return no("its coverage differs from the fused draw's");}
    la.S=T;++a_carry_ok;}
uint32_t calls=0;
int pre(){if(on()<=0)return 0;if(!(++calls&255))tick();   // the 10 s line: the clock is read once in 256 draws
    const nbchk::Span &x=nbchk::sp;const bool planned=x.open && !x.closing && x.h.patched && x.h.plan_ok;const unsigned fl=planned?x.h.flags:0u;int r=0,cls=0;
    if(fl&NB_NP_ELIDE){
        if((fl&NB_NP_AA) && aa && state.shader[4])cls=nb_post_aa_class(key64(state.shader[4]->key));
        if(cls){if(al.valid && al.seq==x.h.seq && al.node==int(x.h.node)){++why["AA pass: a second draw (skipped too)"];return 1;}r=step(cls);}
        else{if(st.valid && st.skipped && st.seq==x.h.seq && st.node==x.h.node){++why["elided pass: a second draw (skipped too)"];return 1;}r=elide((fl&NB_NP_AA) && aa);}}
    else if(fl&NB_NP_FUSE)r=fuse();
    if(r==1)return 1;
    if(al.valid && state.rt.n>=1 && state.rt.color[0]){   // a real draw into O2, or into S by anything but the F1A draw: the alias ends
        const uint64_t m=uint64_t(uintptr_t(state.rt.color[0]->mem));
        if(m==al.O2 || (m==al.S && !(r==2 && la.ok && la.seq==x.h.seq && la.S==m))){al.valid=false;++a_drop;}}
    if(!r && (fl&NB_NP_AA) && aa && !cls && !((fl&NB_NP_FUSE) && state.shader[4] && fx_of_host(nb_post_class(key64(state.shader[4]->key)))==NB_FXK_F1)){   // not Sight: its Base is the LUT's output, not an alias
        if(!(fl&NB_NP_FUSE) && la.ok && la.seq==x.h.seq && state.shader[4] && nb_post_aa_carry2(key64(state.shader[4]->key),aa_outline)){   // NB-18b-10: F1A's output goes through this pass
            if(!(la.carry_seq==x.h.seq && la.carry_node==int(x.h.node))){la.carry_seq=x.h.seq;la.carry_node=int(x.h.node);carry();}}
        else r=redirect();}
    return r;}
void post(){if(sv.fused){state.shader[4]=sv.fs;for(int i=0;i<8;i++)state.tex[1][SPARE_TEX+i]=sv.tex[i];state.ub[1][SPARE_UBO]=sv.ub[0];state.ub[1][SPARE_UBO-1]=sv.ub[1];++buf_gen[1];}
    for(int i=0;i<sv.nr;i++){state.tex[1][sv.rslot[i]]=sv.rh[i];++tex_gen[1];}
    ++tex_gen[1];sv=Saved{};}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    std::string w;for(auto &e:why){char b[160];snprintf(b,sizeof b,", %s %llu",e.first.c_str(),(unsigned long long)e.second);w+=b;}
    xbg_logf("[nb] post DM_NB_POST=%d: elided passes %llu (skipped %llu, poisoned %llu); %s %llu; fused pass without a stash (the chain ran) %llu; "
        "host failures after the elide %llu; fallback by reason%s",mode,(unsigned long long)n_elide,(unsigned long long)n_skip,(unsigned long long)n_poison,
        mode==2?"fused ok (would apply)":"fused draws",(unsigned long long)(mode==2?n_ok:n_fused),(unsigned long long)n_nostash,(unsigned long long)n_late,w.empty()?": none":w.c_str());
    if(aa)xbg_logf("[nb] post aa DM_NB_POST_AA=1 luma %d (%s, bundle %s): LUT passes planned for F1A %llu (plain F1 instead %llu); %s %llu; AA passes (PPFXAA / copy) planned "
        "for elision %llu: %s %llu, run after an F1A draw (double AA) %llu; %s %llu; alias ended by a real draw %llu; alias now %s; F1A stashes drawn as plain F1 (part of the target) %llu; "
        "carry passes (EoCFog between Sight and the AA pass) %llu, carried F1A's output %llu",luma,luma_name[luma],F[FXA].key.empty()?luma_fx[luma]:F[FXA].key.c_str(),
        (unsigned long long)a_e,(unsigned long long)a_plain,mode==2?"F1A ok (would draw)":"F1A draws",(unsigned long long)(mode==2?a_f1a_ok:a_f1a),(unsigned long long)a_step,
        mode==2?"ok (would skip)":"skipped",(unsigned long long)(mode==2?a_step_ok:a_step_skip),(unsigned long long)a_step_late,
        mode==2?"texture slots that would sample Sight's target":"texture slots redirected to Sight's target",(unsigned long long)(mode==2?a_redir_would:a_redir),(unsigned long long)a_drop,
        al.valid?"on":mode==2?(al.would?"would be on":"off"):"off",(unsigned long long)a_back,(unsigned long long)a_carry,(unsigned long long)a_carry_ok);
    why.clear();n_elide=n_skip=n_fused=n_ok=n_late=n_poison=n_nostash=0;a_e=a_f1a=a_f1a_ok=a_plain=a_back=a_step=a_step_skip=a_step_ok=a_step_late=a_redir=a_redir_would=a_drop=a_carry=a_carry_ok=0;}
}
// OP_BIND_PROGRAM: publish the program's fs key for the front end (once per serial; the sampled slots are NB-07's nbsm table, same
// serial); remember the hosts' keys.
void nbpost_publish(const prog_t *prog,const std::array<Shader*,6> &sh){
    if(nbpost::on()<=0 || !prog || !prog->serial)return;
#ifdef DM_GAME_DOS2
    static std::unordered_set<uint64_t> done;if(!done.insert(prog->serial).second)return;
    nb_post_prog e{};e.serial=prog->serial;
    for(const Shader *s:sh)if(s && s->stage==4){e.fs=nbpost::key64(s->key);const int k=nbpost::fx_of_host(nb_post_class(e.fs));if(k && nbpost::host_key[k].empty())nbpost::host_key[k]=s->key;}
    dm_nb_post_publish(&e);
#else
    (void)sh;
#endif
}
