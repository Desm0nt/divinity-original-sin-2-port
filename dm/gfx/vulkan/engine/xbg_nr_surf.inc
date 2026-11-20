// [DOS2] NR-33C DM_NR_SH_SURF (docs/dos2/nr/NR-33C.md; record / gate / choice: dm/gfx/nr/nr_surf.{h,c}; variants: tools/dos2_nr_surf.py,
// setup stage 8 -> <shaders>/native-nr33c.xgspack). Included after xbg_nr_mat.inc (xbg_game.cpp); hooks, one line each:
//   load_shader   nrsurf::note         a fragment shader with a record "sr.<key>" made from these very SPIR-V bytes (FNV) gets its variants
//   draw()        nrsurf::pre          right after nrmat::pre: the shader is swapped for its "<key>#s<v>" twin for this one draw (Guard puts
//                                      the game's shader back); the pipeline memo / NB-13b / warm-up key on the shader pointer / module key
//   warm-up       nrsurf::warm_module  the "#s<v>" stages of a recorded pipeline are made the same way
//   submission    nrsurf::tick         the '[nr] surf' summary every 10 s when a count changed
// The variants (setup-made from the player's own translated SPIR-V, spirv-val + interpreter proof there): after the top-level kills /
// once colour 0's alpha is known, "if (subgroupAny(live)) { the rest, unchanged } else { kill }" - a wave none of whose lanes can write
// anything skips the rest of the shader (a kill alone does not stop a wave on Turnip / most tilers: the lanes go on as helpers).
// DM_NR_SH_SURF: 0 (default) nothing - one integer test per shader load and per draw. 1 = exact: 'a' (alpha +-0 dead) where the gate holds
// (colour 0 blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA, no depth / stencil write, no query, no alpha to coverage / logic op), else 'k' (kills
// only, no gate). 2 = mobile: 'm' (0 <= alpha < 1/255 dead) under the same gate, else 'k'. 3 = poison: 'p' (a skipped wave draws magenta
// where the alpha is dead) / 'k'. 4 = check: every decision made and counted, the game's shaders drawn. Never with the NVN alpha test on
// (the draw keeps the game's shader) nor without subgroup vote in fragment shaders (the device would get the lone-lane form: refused).
namespace nrsurf {
int mode=0;std::once_flag init_once,pack_once;int fd=-1;std::vector<xpack::Ent> idx;bool vote=false;
struct Stats{uint64_t seen=0,swapped=0,would=0,v[4]{},alpha_test=0,refused_twin=0,gate_bits[7]{},recs=0,stale=0,bad=0,twins=0,reds=0,changed=0;}s;
void red(const char *fmt,...){const uint64_t n=++s.reds;if(n>16)return;char t[640];va_list a;va_start(a,fmt);vsnprintf(t,sizeof t,fmt,a);va_end(a);xbg_logf("【红】[nr] surf %s",t);}
int vi(char v){return v=='k'?0:v=='a'?1:v=='m'?2:v=='p'?3:-1;}
int on(){std::call_once(init_once,[]{const char *e=getenv("DM_NR_SH_SURF");int m=e && *e?atoi(e):xbg_nr_surf_default(gpu_policy);
        if(m<0 || m>4){red("DM_NR_SH_SURF=%s is not 0..4: off",e);m=0;}
        vote=(subgroup.supportedStages&VK_SHADER_STAGE_FRAGMENT_BIT) && (subgroup.supportedOperations&VK_SUBGROUP_FEATURE_VOTE_BIT);
        if(m && !vote){red("DM_NR_SH_SURF=%d: the device has no subgroup vote in fragment shaders (stages %#x ops %#x): off",m,subgroup.supportedStages,subgroup.supportedOperations);m=0;}
        mode=m;
        if(m)xbg_logf("[nr] surf DM_NR_SH_SURF=%d (%s): surface materials with a wave-coherent early out, variants <shaders>/native-nr33c.xgspack "
            "(setup stage 8); subgroup size %u; policy family %s",m,m==1?"exact":m==2?"mobile: alpha < 1/255 not drawn":m==3?"POISON: skipped waves magenta":"check: the game's shaders drawn",
            subgroup.subgroupSize,xbg_gpu_family_name(gpu_policy));});
    return mode;}
void open_pack(){std::call_once(pack_once,[]{if(!cfg.shader_directory)return;const std::string path=std::string(cfg.shader_directory)+"/native-nr33c.xgspack";
    int f=::open(path.c_str(),O_RDONLY|O_CLOEXEC);if(f<0){red("no %s: run setup again (stage 8 builds it from your shader pack); the game's shaders are used",path.c_str());return;}
    struct stat st{};uint32_t h[4]={};bool ok=!fstat(f,&st) && pread(f,h,16,0)==16 && h[0]==0x50534758u && h[1]==1u && h[2]<65536;
    std::vector<xpack::Ent> v;if(ok){v.resize(h[2]);ok=pread(f,v.data(),v.size()*sizeof(xpack::Ent),16)==ssize_t(v.size()*sizeof(xpack::Ent));}
    for(size_t i=0;ok && i<v.size();i++)ok=v[i].key[31]==0 && v[i].off+v[i].size<=uint64_t(st.st_size) && (!i || strcmp(v[i-1].key,v[i].key)<0);
    if(!ok){::close(f);red("%s is damaged or of another version: ignored (run setup again)",path.c_str());return;}
    fd=f;idx.swap(v);xbg_logf("[nr] surf %s: %zu bundles",path.c_str(),idx.size());});}
bool get(const std::string &key,std::vector<uint8_t> &out){open_pack();if(fd<0)return false;
    auto it=std::lower_bound(idx.begin(),idx.end(),key.c_str(),[](const xpack::Ent &a,const char *k){return strcmp(a.key,k)<0;});
    if(it==idx.end() || strcmp(key.c_str(),it->key))return false;out.resize(it->size);size_t got=0;
    while(got<out.size()){ssize_t r=pread(fd,out.data()+got,out.size()-got,off_t(it->off+got));if(r<0 && errno==EINTR)continue;if(r<=0)break;got+=size_t(r);}
    return got==out.size() && xpack::crc(out.data(),out.size())==it->hash;}
// load_shader: raw = the translated bundle's own SPIR-V bytes (the record names their FNV), bundle = the whole translated bundle (its head)
void note(Shader &p,const uint8_t *bundle,size_t bundle_n,const uint8_t *raw,size_t raw_n){
    if(on()<=0 || p.stage!=4 || p.key.size()<16)return;
    std::vector<uint8_t> rec;if(!get("sr."+p.key.substr(0,16),rec))return;
    nr_surf_rec r;const char *why=nr_surf_rec_parse(rec.data(),rec.size(),&r);
    if(why){++s.bad;++s.changed;red("record of %s refused: %s (run setup again)",p.key.c_str(),why);return;}
    const uint64_t h=nr_surf_fnv64(raw,raw_n);
    if(h!=r.spirv_fnv){++s.stale;++s.changed;red("record of %s is for other SPIR-V (%016llx, the bundle %016llx): ignored (run setup again)",p.key.c_str(),
        (unsigned long long)r.spirv_fnv,(unsigned long long)h);return;}
    std::vector<uint32_t> w(raw_n/4);memcpy(w.data(),raw,w.size()*4);uint32_t in=0,out=0;
    if(nr_sh_io(w.data(),w.size(),&in,&out)){++s.bad;red("%s: interface not readable: no variants",p.key.c_str());return;}
    p.ns_var=uint8_t(r.variants);p.ns_out=out;p.ns_head.assign(bundle,bundle+(bundle_n-raw_n));++s.recs;++s.changed;
    if(s.recs<=4)xbg_logf("[nr] surf %s: variants%s%s%s%s",p.key.c_str(),r.variants&NR_SURF_V_K?" k":"",r.variants&NR_SURF_V_A?" a":"",r.variants&NR_SURF_V_M?" m":"",r.variants&NR_SURF_V_P?" p":"");}
// The SPIR-V of variant v of the translated bundle whose head is head: the pack bundle must carry that head byte for byte (but its
// SPIR-V length), then the load-time rewrites of bundle_words, in the same order.
bool words(const std::string &fs_key,const std::vector<uint8_t> &head,char v,std::vector<uint32_t> &w,const std::string &key){
    std::vector<uint8_t> b;if(!get(std::string(1,v)+"."+fs_key.substr(0,16),b))return false;
    if(head.size()<32 || b.size()<=head.size())return false;
    for(size_t i=0;i<head.size();i++)if((i<24 || i>=28) && head[i]!=b[i])return false;   // header word 6 (SPIR-V length) is the variant's own
    const size_t n=(b.size()-head.size())/4;uint32_t len=0;memcpy(&len,b.data()+24,4);if(len!=n*4)return false;
    w.resize(n);memcpy(w.data(),b.data()+head.size(),n*4);
    gscull_rewrite(w,4,key.c_str(),"nr-surf");lone_rewrite(w,4,key.c_str(),"nr-surf");early_tests_rewrite(w,4,key.c_str());invariant_rewrite(w,4,key.c_str());gather_ext_rewrite(w,key.c_str());gather_snap_rewrite(w,4,key.c_str());quad_rewrite(w,4,key.c_str(),"nr-surf");warp_rewrite(w,4,key.c_str(),"nr-surf");tidy_rewrite(w,key.c_str());
    return true;}
Shader *twin(Shader &fs,char v){
    const int i=vi(v);if(i<0)return nullptr;if(fs.ns_twin[i])return fs.ns_twin[i]==&fs?nullptr:fs.ns_twin[i];
    const std::string key=fs.key+"#s"+v;std::vector<uint32_t> w;
    if(!words(fs.key,fs.ns_head,v,w,key)){red("%s: variant %c missing or not made from this bundle (run setup again): the game's shader is kept",fs.key.c_str(),v);fs.ns_twin[i]=&fs;++s.refused_twin;return nullptr;}
    auto *t=new Shader();t->key=key;t->stage=4;t->bindings=fs.bindings;t->c1=fs.c1;t->discards=true;t->empty_fs=false;t->pg_coord=fs.pg_coord;
    t->raster=xbg_spirv_raster_facts(w.data(),w.size(),4);t->vo_read=fs.vo_read;t->vo_decl=fs.vo_decl;t->ns_is_twin=true;
    if(bundle_module(w,&t->module,key)!=VK_SUCCESS){delete t;red("%s: the module of variant %c could not be made: the game's shader is kept",fs.key.c_str(),v);fs.ns_twin[i]=&fs;++s.refused_twin;return nullptr;}
    warm::register_module(t->module,key);fs.ns_twin[i]=t;++s.twins;++s.changed;
    if(s.twins<=4)xbg_logf("[nr] surf %s -> %s (%zu words)",fs.key.c_str(),key.c_str(),w.size());
    return t;}
struct Guard{Shader *saved=nullptr;~Guard(){if(saved)state.shader[4]=saved;}};
void pre(Guard &g){
    Shader *fs=state.shader[4];if(!fs || !fs->ns_var || fs->ns_is_twin || fs->nm_is_twin)return;
    ++s.seen;
    if(state.color.alpha_func!=8 && alpha_test_on()){++s.alpha_test;return;}   // the NVN alpha test would build its variant of the twin: kept
    nr_surf_att at[8]{};int n=0;
    for(int i=0;i<state.rt.n && i<8;i++){const blend_st &b=state.blend[i];xbg_draw2d c{};
        if(!state.rt.color[i]){at[i]=nr_surf_att{};n=i+1;continue;}
        if(xbg_2d_blend_from_nvn(&c,(state.color.blend_en>>i)&1,b.src_rgb,b.dst_rgb,b.src_a,b.dst_a,b.eq_rgb,b.eq_a,(state.mask.mask>>(4*i))&15)){n=0;break;}
        at[i]=nr_surf_att{int(c.blend.blendEnable),int(c.blend.srcColorBlendFactor),int(c.blend.dstColorBlendFactor),int(c.blend.colorBlendOp),
            int(c.blend.srcAlphaBlendFactor),int(c.blend.dstAlphaBlendFactor),int(c.blend.alphaBlendOp),unsigned(c.blend.colorWriteMask)};n=i+1;}
    const bool sw=state.rt.depth && state.ds.st && (state.stencil_write[0] || state.stencil_write[1]);
    const unsigned gate=nr_surf_alpha_gate(at,n,fs->ns_out,state.rt.depth && state.ds.dw,sw,state.ms.a2c,state.color.logic_op!=3,
        state.count_samples || nrocc::in_pair());
    for(int b=0;b<7;b++)if(gate>>b&1)++s.gate_bits[b];
    const char v=nr_surf_choose(mode,fs->ns_var,gate);if(!v)return;
    ++s.v[vi(v)];
    if(mode==NR_SURF_CHECK){++s.would;return;}
    Shader *t=twin(*fs,v);if(!t)return;
    g.saved=fs;state.shader[4]=t;++s.swapped;}
// warm-up: 1 = a module of ours (m), -1 = ours but not made, 0 = not an NR-33C key ("<16 hex>#s<k|a|m|p>")
int warm_module(const std::string &key,VkShaderModule &m){
    const size_t c=key.find("#s");if(c==std::string::npos || c<16 || key.size()!=c+3 || vi(key[c+2])<0)return 0;
    if(on()<=0 || mode==NR_SURF_CHECK)return -1;
    std::vector<uint8_t> b;bool from_pack=false;const std::string fk=key.substr(0,c);
    if(!xpack::read(fk,b,from_pack) || b.size()<32)return -1;
    xbg_shader_header h;memcpy(&h,b.data(),sizeof(h));if(h.magic!=0x31534758 || h.stage!=4 || h.spirv_bytes%4 || h.spirv_bytes>=b.size())return -1;
    std::vector<uint8_t> rec;nr_surf_rec r;if(!get("sr."+fk.substr(0,16),rec) || nr_surf_rec_parse(rec.data(),rec.size(),&r) ||
        r.spirv_fnv!=nr_surf_fnv64(b.data()+b.size()-h.spirv_bytes,h.spirv_bytes))return -1;
    const std::vector<uint8_t> head(b.begin(),b.end()-h.spirv_bytes);std::vector<uint32_t> w;
    if(!words(fk,head,key[c+2],w,key) || bundle_module(w,&m,key)!=VK_SUCCESS)return -1;
    return 1;}
void tick(){static uint64_t next=0,last=~0ull;const uint64_t now=timing_clock();if(now<next)return;next=now+10000000000ull;
    const uint64_t c=s.changed+s.swapped+s.would+s.seen;if(c==last)return;last=c;
    xbg_logf("[nr] surf DM_NR_SH_SURF=%d (%s): pack %s, records %llu (stale %llu, bad %llu); draws of surface shaders %llu: %s %llu (k %llu, a %llu, m %llu, p %llu), "
        "kept: NVN alpha test %llu, variant missing %llu; alpha gate refused by blend %llu, another written attachment %llu, depth write %llu, "
        "stencil write %llu, alpha to coverage %llu, logic op %llu, sample counting %llu; twins %llu; red lines %llu",mode,mode==1?"exact":mode==2?"mobile":mode==3?"POISON":"check",
        fd>=0?"open":"none",(unsigned long long)s.recs,(unsigned long long)s.stale,(unsigned long long)s.bad,(unsigned long long)s.seen,
        mode==NR_SURF_CHECK?"would swap":"swapped",(unsigned long long)(mode==NR_SURF_CHECK?s.would:s.swapped),(unsigned long long)s.v[0],
        (unsigned long long)s.v[1],(unsigned long long)s.v[2],(unsigned long long)s.v[3],(unsigned long long)s.alpha_test,(unsigned long long)s.refused_twin,
        (unsigned long long)s.gate_bits[0],(unsigned long long)s.gate_bits[1],(unsigned long long)s.gate_bits[2],(unsigned long long)s.gate_bits[3],
        (unsigned long long)s.gate_bits[4],(unsigned long long)s.gate_bits[5],(unsigned long long)s.gate_bits[6],(unsigned long long)s.twins,(unsigned long long)s.reds);}
}
