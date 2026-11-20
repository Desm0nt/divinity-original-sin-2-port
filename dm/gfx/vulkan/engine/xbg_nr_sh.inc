// [DOS2] NR-04 DM_NR_SH (docs/dos2/nr/NR-04.md; table and checks: dm/gfx/nr/nr_sh.{h,c}): engine shaders by name. Included inside
// namespace xpack, next to namespace native; the pass half (pipeline creation) is xbg_nr_sh_pass.inc.
// Source: <shaders>/native-nr.xgspack, built by setup stage 8 (tools/dos2_nr_sh_pack.py) from the player's own translated bundles + this
// port's GLSL rewrites, keyed "<variant>.<engine name>". The translated pack key of a bundle read here names a table entry; its bundle
// is taken only when its head equals the translated head byte for byte (except the SPIR-V length word), the entry point's input / output
// locations equal the translated ones and every descriptor it declares is in the binding table.
//   DM_NR_SH=0 (default): nothing (no pack opened, one integer test per bundle read).
//   1: post (FadeBlending, EoCFog, EoCOutlineCombine, LinearizeDepth, PPDownSampleLog2) and lighting (DeferredSky,
//      ClusteredLightingWithSSAO) - interpreter-proven bit for bit - replaced; shadows (CascadedPCF_*) by DM_NR_SH_PCF (0 translated =
//      default, 1 exact, 2 mobile, 3 dref = PERF-X19 T11: hardware depth compares, see dref_device below). A failed check keeps the translated bundle (red line). At pipeline creation a pass outside the one the
//      shader was verified in takes the translated module for that pipeline (xbg_nr_sh_pass.inc).
//   2: every check of 1 (pack read, head, interface, bindings, the pass of every pipeline) - the translated bundles are used.
// Red lines 【红】 (first 16, all counted); '[nr] sh' summary every 10 s from the submission when something changed.
// Every reader of a bundle goes through xpack::read (load_shader, warm-up, alpha variants, pipeguard's spv), so they all see the same
// bytes; DM_GFX_NATIVE_SHADERS (keyed by the translated key) is not applied to a bundle this replaced.
namespace nrsh {
int mode=0,pcf=0,fd=-1,fsr=0,lights=0;   // fsr / lights: [DOS2] N6 DM_HACK_FSR / DM_HACK_LIGHTS (hacks() below)
const char *pcf_name(int q){return q==1?"exact":q==2?"mobile":q==3?"dref":"translated";}
// [DOS2] PERF-X19 T11 (DM_NR_SH_PCF=3, docs/dos2/DOS2_PORT.md "Мобильный резолв теней", Dref): the 'd' shadow bundles sample the atlas with
// OpImageSampleDrefExplicitLod; the host gives that binding a compare sampler (LESS, linear: hardware 2x2 PCF per tap, xbg_game.cpp
// dref_sampler). The atlas is NVN DEPTH16 (0x32, NR-SHCACHE §11.3), stored as D16_UNORM or as X8_D24 (depth16_as_d24: a device that cannot
// filter D16 linearly). Linear filtering of depth formats is optional in Vulkan (the required-format table marks D16 / D32 sampled, not
// filter-linear; Adreno 730 512.615 has none on D16 / D32); depth comparison is required for every sampled depth format with Vulkan 1.3 /
// VK_KHR_format_feature_flags2 (VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT), checked when the device reports it. Without both the
// 'm' (gather) bundles are used, logged once. Called once from on().
bool dref_device(){const VkFormat f=depth16_as_d24?VK_FORMAT_X8_D24_UNORM_PACK32:VK_FORMAT_D16_UNORM;const VkFormatFeatureFlags ff=format_properties(f).optimalTilingFeatures;
    bool cmp=true;if(format_feature_flags2){VkFormatProperties3 p3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};VkFormatProperties2 p2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};p2.pNext=&p3;
        vkGetPhysicalDeviceFormatProperties2(cfg.physical,f,&p2);cmp=(p3.optimalTilingFeatures&VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT)!=0;}
    const bool ok=(ff&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (ff&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && cmp;
    xbg_logf("[nr] sh DM_NR_SH_PCF=3 (dref, PERF-X19 T11): shadow atlas (NVN DEPTH16) stored as %s, features %#x (sampled %d, linear filter %d), depth comparison %s: %s",
        f==VK_FORMAT_D16_UNORM?"D16_UNORM":"X8_D24",unsigned(ff),int((ff&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)!=0),int((ff&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)!=0),
        format_feature_flags2?(cmp?"yes":"NO"):"not reported (pre-1.3: allowed)",ok?"hardware compare used":"NOT usable - DM_NR_SH_PCF=2 (mobile, textureGather) used instead");
    return ok;}std::vector<Ent> idx;std::once_flag init_once,pack_once;thread_local int bypass=0;
std::atomic<int> st[16];          // per entry: 0 not read yet, 1 replaced (mode 1) / passes every check (mode 2), -1 refused / missing / kept
std::atomic<uint64_t> used{0},would{0},refused{0},missing{0},kept{0},offs{0},reds{0},changed{0};
void red(const char *fmt,...){const uint64_t n=++reds;if(n>16)return;char t[640];va_list a;va_start(a,fmt);vsnprintf(t,sizeof t,fmt,a);va_end(a);xbg_logf("【红】[nr] sh %s",t);}
int on(){std::call_once(init_once,[]{const char *e=getenv("DM_NR_SH");int m=e && *e?atoi(e):xbg_nr_sh_default(gpu_policy);
        if(m<0 || m>2){red("DM_NR_SH=%s is not 0 / 1 / 2: off",e);m=0;}
        const char *p=getenv("DM_NR_SH_PCF");int q=p && *p?atoi(p):xbg_nr_sh_pcf_default(gpu_policy);if(q<0 || q>3)q=0;
        {char hf=0;const int hq=nrh_value(NRH_SHPCF,&hf);   // [DOS2] N6 DM_HACK_SHPCF (2 mobile / 3 dref) when DM_NR_SH_PCF is not set
         if(hq && !(p && *p)){q=hq;xbg_logf("[hack] DM_HACK_SHPCF=%d%s: shadow resolve %s (as DM_NR_SH_PCF=%d)",hq,hf=='t'?" (tier)":"",pcf_name(hq),hq);}}
        {char hf=0;fsr=nrh_value(NRH_FSR,&hf);
         if(fsr && hf=='t' && !xbg_hack_fsr_tier_default(gpu_policy)){fsr=0;xbg_logf("[hack] DM_HACK_TIER: FSR 1 EASU not used on the %s family (DM_HACK_FSR=1 forces it)",
             xbg_gpu_family_name(gpu_policy));}
         if(fsr)xbg_logf("[hack] DM_HACK_FSR=%d%s: PPFXAA bundle -> %s (FXAA as before at a 1:1 stretch, FSR 1 EASU when the 3D "
            "is below 100 %%: tools/dos2_post/PPFXAA_fsr.frag; RCAS = the gear's sharpening at present)%s",fsr,hf=='t'?" (tier)":"",
            fsr==1?"the FXAA + EASU half-precision module":fsr==3?"the FXAA + EASU fp32 module":"check only",m?"":" - DM_NR_SH=0: not used");
         char lf=0;lights=nrh_value(NRH_LIGHTS,&lf);
         if(lights)xbg_logf("[hack] DM_HACK_LIGHTS=%d%s: ClusteredLightingWithSSAO -> the first %d clustered lights that light a pixel (points, then spots, list order)%s",lights,lf=='t'?" (tier)":"",
            lights<=2?2:4,m?"":" - DM_NR_SH=0: not used");}
        if(m && q==NR_SH_PCF_DREF && !dref_device())q=NR_SH_PCF_MOBILE;   // T11: no linear compare on the atlas format -> the gather variant
        pcf=q;mode=m;
        if(m)xbg_logf("[nr] sh DM_NR_SH=%d (%s): engine shaders by name, %d in the table (post 5, lighting 2, shadows 3: DM_NR_SH_PCF=%d %s, "
            "policy family %s); source <shaders>/native-nr.xgspack (setup stage 8)",m,m==1?"replace":"check: translated used",nr_sh_count,q,
            pcf_name(q),xbg_gpu_family_name(gpu_policy));});
    return mode;}
void open_pack(){
    std::call_once(pack_once,[]{if(!cfg.shader_directory)return;const std::string path=std::string(cfg.shader_directory)+"/native-nr.xgspack";int f=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
        if(f<0){red("no %s: run setup again (stage 8 builds it from your shader pack); translated shaders used",path.c_str());return;}
        struct stat s{};uint32_t h[4]={};bool ok=!fstat(f,&s) && pread(f,h,16,0)==16 && h[0]==0x50534758u && h[1]==1u && h[2]<4096;
        std::vector<Ent> v;if(ok){v.resize(h[2]);ok=pread(f,v.data(),v.size()*sizeof(Ent),16)==ssize_t(v.size()*sizeof(Ent));}
        for(size_t i=0;ok && i<v.size();i++)ok=v[i].key[31]==0 && v[i].off>=16+v.size()*sizeof(Ent) && v[i].off+v[i].size<=uint64_t(s.st_size) && (!i || strcmp(v[i-1].key,v[i].key)<0);
        if(!ok){::close(f);red("%s is damaged or of another version: ignored (run setup again)",path.c_str());return;}
        fd=f;idx.swap(v);xbg_logf("[nr] sh %s: %zu bundles",path.c_str(),idx.size());});}
bool get(const char *key,std::vector<uint8_t> &out){
    open_pack();if(fd<0)return false;
    auto it=std::lower_bound(idx.begin(),idx.end(),key,[](const Ent &a,const char *k){return strcmp(a.key,k)<0;});
    if(it==idx.end() || strcmp(key,it->key))return false;
    out.resize(it->size);size_t got=0;while(got<out.size()){ssize_t r=pread(fd,out.data()+got,out.size()-got,off_t(it->off+got));if(r<0 && errno==EINTR)continue;if(r<=0)break;got+=size_t(r);}
    return got==out.size() && crc(out.data(),out.size())==it->hash;}
void io(const std::vector<uint8_t> &b,uint32_t &in,uint32_t &out){in=out=0;const size_t h=nr_sh_head(b.data(),b.size());if(!h)return;
    std::vector<uint32_t> w((b.size()-h)/4);memcpy(w.data(),b.data()+h,w.size()*4);if(nr_sh_io(w.data(),w.size(),&in,&out))in=out=~0u;}
// The entry of a bundle key (16 hex characters) or -1.
int entry(const std::string &key){return key.size()==16?nr_sh_find_key(key.c_str()):-1;}
// [DOS2] N6 hacks (docs/dos2/nr/NR-HACKS.md): an entry's bundle -> a module built into the backend (no setup run) behind the translated
// bundle's own head - the same checks as a pack bundle (head byte for byte by construction, interface, descriptors); swapped = the entry is
// marked as replaced (the NR-04 pass check applies at pipeline creation). A refusal keeps the usual path.
//   DM_HACK_FSR    PPFXAA -> tools/dos2_post/PPFXAA_fsr.frag: its FXAA half = PPFXAA_mobile.frag (bit for bit), FSR 1 EASU when the 3D is
//                  below 100 % (stretch ratio = coordinate derivative x texture width). 1 = half precision (FsrEasuH form, mediump: fp16 ALUs;
//                  ppfxaa_fsrh), 3 = fp32 (FsrEasuF, ppfxaa_fsr), 2 = checked only.
//   DM_HACK_LIGHTS ClusteredLightingWithSSAO -> ClusteredLightingWithSSAO_mobile.frag + LCAP 2 / 4 (tools/dos2_light_cap.py): the first N
//                  clustered lights that light the pixel, then the loops stop; = the 'x' bundle on a prefix of the list, bit for bit.
std::atomic<int> hack_st[2]={{0},{0}};
static const uint32_t fsr_spv[]={
#include "../../../android/shaders/ppfxaa_fsr.frag.spv.inc"
};
static const uint32_t fsrh_spv[]={
#include "../../../android/shaders/ppfxaa_fsrh.frag.spv.inc"
};
static const uint32_t lcap2_spv[]={
#include "../../../android/shaders/light_lcap2.frag.spv.inc"
};
static const uint32_t lcap4_spv[]={
#include "../../../android/shaders/light_lcap4.frag.spv.inc"
};
bool hack_apply(int i,int slot,const std::string &key,std::vector<uint8_t> &bytes,const uint32_t *spv,size_t spv_bytes,const char *what,bool use){
    const size_t th=nr_sh_head(bytes.data(),bytes.size());if(!th)return false;
    std::vector<uint8_t> nb(bytes.begin(),bytes.begin()+th);nb.resize(th+spv_bytes);memcpy(nb.data()+th,spv,spv_bytes);
    const uint32_t len=uint32_t(spv_bytes);memcpy(nb.data()+24,&len,4);   // the SPIR-V length word (h[6])
    uint32_t ti,to,ni,no;io(bytes,ti,to);io(nb,ni,no);const bool binds=nr_sh_bindings_ok(nb.data(),nb.size()),head=nr_sh_head_equal(bytes.data(),bytes.size(),nb.data(),nb.size());
    if(!head || !binds || ti!=ni || to!=no){if(hack_st[slot].exchange(-1)!=-1){red("%s (%s): %s refused - head %s, inputs %#x/%#x, outputs %#x/%#x, descriptors %s: the usual path",
        nr_sh_table[i].name,key.c_str(),what,head?"equal":"DIFFERS",ti,ni,to,no,binds?"in the table":"NOT in the binding table");}return false;}
    int was=0;if(hack_st[slot].compare_exchange_strong(was,1))xbg_logf("[hack] %s: %s (%s) %s (%zu -> %zu words), inputs %#x outputs %#x",what,nr_sh_table[i].name,key.c_str(),
        use?"->":"check only:",(bytes.size()-th)/4,spv_bytes/4,ti,to);
    if(!use)return false;
    was=0;st[i].compare_exchange_strong(was,1);   // marked: the NR-04 pass check at pipeline creation
    bytes=std::move(nb);++used;return true;}   // a hack module (NR-04 swaps in apply, mode 1)
bool hacks(int i,const std::string &key,std::vector<uint8_t> &bytes){
    if(mode!=1 || !nr_sh_enabled(i))return false;
    const char *n=nr_sh_table[i].name;
    if(fsr && !strcmp(n,"PPFXAA"))return fsr==3?hack_apply(i,0,key,bytes,fsr_spv,sizeof fsr_spv,"DM_HACK_FSR=3 FXAA + EASU fp32 module",true)
                                              :hack_apply(i,0,key,bytes,fsrh_spv,sizeof fsrh_spv,"DM_HACK_FSR FXAA + EASU half module",fsr==1);
    if(lights && !strcmp(n,"ClusteredLightingWithSSAO"))return lights<=2?hack_apply(i,1,key,bytes,lcap2_spv,sizeof lcap2_spv,"DM_HACK_LIGHTS=2 light cap module",true)
                                                                       :hack_apply(i,1,key,bytes,lcap4_spv,sizeof lcap4_spv,"DM_HACK_LIGHTS=4 light cap module",true);
    return false;}
// With the translated bundle in hand: mode 1 may swap in the named one (true), mode 2 checks it.
bool apply(const std::string &key,std::vector<uint8_t> &bytes){
    if(bypass)return false;const int i=entry(key);if(i<0 || i>=16)return false;
    if((fsr || lights) && hacks(i,key,bytes))return true;   // [DOS2] N6 DM_HACK_FSR / DM_HACK_LIGHTS
    const nr_sh_entry &e=nr_sh_table[i];const char v=nr_sh_variant(i,pcf);int was=0;
    if(!v){if(st[i].compare_exchange_strong(was,-1)){++kept;++changed;xbg_logf("[nr] sh %s (%s): translated kept (DM_NR_SH_PCF=0; 1 exact, 2 mobile, 3 dref)",e.name,key.c_str());}return false;}
    if(!nr_sh_enabled(i)){if(st[i].exchange(-1)!=-1){++offs;++changed;red("%s (%s): off - the engine named another shader with this key or name (registry, DM_NR): translated used",e.name,key.c_str());}return false;}
    char pk[32];if(!nr_sh_pack_key(i,v,pk,sizeof pk))return false;
    std::vector<uint8_t> nb;if(!get(pk,nb)){if(st[i].compare_exchange_strong(was,-1)){++missing;++changed;if(fd>=0)red("%s: no bundle %s in native-nr.xgspack (run setup again): translated used",e.name,pk);}return false;}
    const bool head=nr_sh_head_equal(bytes.data(),bytes.size(),nb.data(),nb.size()),binds=nr_sh_bindings_ok(nb.data(),nb.size());
    uint32_t ti,to,ni,no;io(bytes,ti,to);io(nb,ni,no);
    if(!head || !binds || ti!=ni || to!=no){if(st[i].exchange(-1)!=-1){++refused;++changed;
            red("%s: %s refused - head %s (translated %016llx, pack %016llx), inputs %#x/%#x, outputs %#x/%#x (translated/pack), descriptors %s: "
                "translated used (the pack is not from this shader pack: run setup again)",e.name,pk,head?"equal":"DIFFERS",(unsigned long long)nr_sh_head_hash(bytes.data(),bytes.size()),
                (unsigned long long)nr_sh_head_hash(nb.data(),nb.size()),ti,ni,to,no,binds?"in the table":"NOT in the binding table");}return false;}
    const size_t th=nr_sh_head(bytes.data(),bytes.size());
    if(st[i].compare_exchange_strong(was,1)){++changed;
        xbg_logf("[nr] sh %s %s %s (%s%s): head byte for byte (%016llx), inputs %#x outputs %#x, SPIR-V %zu -> %zu words%s",e.name,mode==1?"->":"check:",pk,
            e.cls==NR_SH_SHADOW?(v=='x'?"exact":v=='d'?"dref: compare sampler on its depth binding":"mobile"):"proven bit for bit",e.proven?"":", no interpreter proof",(unsigned long long)nr_sh_head_hash(bytes.data(),bytes.size()),ti,to,
            (bytes.size()-th)/4,(nb.size()-th)/4,mode==1?"":" - translated used (DM_NR_SH=2)");}
    if(mode==1){bytes.swap(nb);++used;return true;}
    ++would;return false;}
// [DOS2] NR-LIGHTFUSE (xbg_nr_lightfuse.inc): oc_var 'f' = the fused Sky -> Light bundle, whose extra bindings (nr_lightfuse.h) are allowed.
// [DOS2] NR-ONCHIP (xbg_nr_onchip.inc twin_module): with oc_var set ('2' = light.s2), read() hands out the pack's "<oc_var>.<name>" bundle for
// the translated one under the same checks as apply (head byte for byte, interface, descriptors), whatever DM_NR_SH is; false: none.
thread_local char oc_var=0;
bool oc_apply(const std::string &key,std::vector<uint8_t> &bytes){
    const int i=entry(key);if(i<0 || i>=16)return false;char pk[32];if(!nr_sh_pack_key(i,oc_var,pk,sizeof pk))return false;
    std::vector<uint8_t> nb;if(!get(pk,nb))return false;
    uint32_t ti,to,ni,no;io(bytes,ti,to);io(nb,ni,no);
    const bool lf=oc_var=='f' || oc_var=='g';   // [DOS2] NR-LIGHTFUSE: the f / g variants add Sky's extras (nr_lightfuse.h); g also writes out 1 (Fog)
    const bool binds=lf?nr_lf_bindings_ok(nb.data(),nb.size())!=0:nr_sh_bindings_ok(nb.data(),nb.size())!=0;
    if(!nr_sh_head_equal(bytes.data(),bytes.size(),nb.data(),nb.size()) || !binds || ti!=ni || (oc_var=='g'?no!=(to|2u):to!=no)){
        red("%s: %s refused (head / interface / descriptors differ from the translated bundle): no on-chip merge",nr_sh_table[i].name,pk);return false;}
    bytes.assign(nb.begin(),nb.end());return true;}   // the NR-ONCHIP variant only (NR-04 swaps in apply, mode 1)
// load_shader: the entry a loaded bundle took (mode 1) / would take (mode 2), else -1 (Shader::nr_sh: the pass check at pipeline creation)
int marked(const std::string &key){if(mode<=0)return -1;const int i=entry(key);return i>=0 && i<16 && st[i].load()==1?i:-1;}
}
