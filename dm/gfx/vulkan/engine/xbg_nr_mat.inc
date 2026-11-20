// [DOS2] NR-33B DM_NR_MAT (docs/dos2/nr/NR-33B.md; analysis and checks: dm/gfx/nr/nr_mat.{h,c}; gate records: tools/dos2_nr_mat.py,
// setup stage 8 -> <shaders>/native-nr33b.xgspack). Included after alpha_module (xbg_game.cpp); hooks, one line each:
//   load_shader   nrmat::note      what a loaded fragment module is (kill / built-in outputs / side effects, the gate record of its key)
//   draw()        nrmat::pre       right after the pre-pass check: a material fragment shader in a pass without colour attachments that
//                                  provably has no effect there is swapped for its "<key>#dz" twin (the empty fragment shader, same bindings)
//                                  for this one draw (Guard puts the game's shader back); the pipeline memo / NB-13b / warm-up key on the
//                                  shader pointer / module key, so the twin's pipeline is its own entry everywhere
//   draw() build  (NR-32)          the vertex shader of a twin pipeline: NR-32's vsout_module (xbg_spirv_vsout, one trim for the tree) with
//                                  read = declared = 0 -> "<vs>[#vi...]#vo0000000000000000_0000000000000000": every Location output store
//                                  and the unreferenced outputs go, gl_Position untouched; composes with the vertex retype (#vi). On while
//                                  xlq_twin (DM_NR_MAT 1 / 3 with DM_NR_MAT_VS=1) whatever DM_NR_XLQ is; other pipelines follow DM_NR_XLQ only
//   warm-up       nrmat::warm_module  the "#dz" stage of a recorded pipeline is made the same way (its "#vo" vertex stage by load_module);
//                                  "#dp<keep>" keys (the earlier NR-33B-only prune, never shipped) are not built
//   submission    nrmat::tick      the '[nr] mat' summary every 10 s when a count changed
// DM_NR_MAT: 0 (default) nothing - one integer test per shader load and per draw. 1 = on. 2 = check: every decision made and counted
// ('would'), the game's shaders drawn. 3 = poison: also swaps the gated draws whose gate FAILS (the camera-fade dissolve of Base_PBR
// objects is then missing in the depth pre-pass / shadows: a visible proof that the per-draw gate is what decides). DM_NR_MAT_VS=0: NR-33B
// adds no vertex trim (only the fragment twin; DM_NR_XLQ 1 / 3 still trims every pipeline, twins included), 1 (default with DM_NR_MAT=1/3):
// twin pipelines get the NR-32 trim (xlq_twin).
// Exactness (no tolerance): the twin is used only when (a) the pass has no colour attachment, no tessellation / geometry stage, no
// rasterizer discard, no alpha to coverage; (b) the fragment shader has no fragment built-in output (depth / sample mask / stencil) and
// no side effect; and (c) no fragment can be killed: no kill instruction with the alpha test off, or the draw's uniforms satisfy a gate
// path of its record (setup proved: no kill on that path, Location 0 .w = w) and the NVN alpha test passes w (nr_mat_alpha_pass). Then
// depth / stencil / occlusion results are the shader's own and the colour outputs go nowhere. The vertex trim removes only stores into
// outputs the twin does not read (all Location outputs); position math is untouched (xbg_spirv_vsout_check per module; proofs:
// tools/dos2_xlq_check.py, NR-32.md section 5, and tools/dos2_nr_mat_proof.py at mask 0).
namespace nrmat {
enum{C_NONE=0,C_PURE=1,C_KILL=2};
int mode=0,vsmode=0;std::once_flag init_once,pack_once;int fd=-1;std::vector<xpack::Ent> idx;std::mutex mu;std::deque<nr_mat_gate> gates;   // stable addresses: Shader::nm_gate points into it
struct Stats{uint64_t seen=0,swapped=0,would=0,poison=0,pure=0,gate_off=0,gate_alpha=0,alpha_na=0,refuse_gate=0,refuse_alpha=0,refuse_nogate=0,
    refuse_kill=0,a2c=0,already_empty=0,twins=0,gates_loaded=0,gates_stale=0,gates_bad=0,reds=0,changed=0;}s;
void red(const char *fmt,...){const uint64_t n=++s.reds;if(n>16)return;char t[640];va_list a;va_start(a,fmt);vsnprintf(t,sizeof t,fmt,a);va_end(a);xbg_logf("【红】[nr] mat %s",t);}
int on(){std::call_once(init_once,[]{const char *e=getenv("DM_NR_MAT");int m=e && *e?atoi(e):xbg_nr_mat_default(gpu_policy);
        if(m<0 || m>3){red("DM_NR_MAT=%s is not 0 / 1 / 2 / 3: off",e);m=0;}
        const char *v=getenv("DM_NR_MAT_VS");vsmode=m==2?0:v && *v?(atoi(v)==1):1;mode=m;xlq_twin=(m==1 || m==3) && vsmode;   // NR-32's trim for the twin pipelines
        if(m)xbg_logf("[nr] mat DM_NR_MAT=%d (%s), DM_NR_MAT_VS=%d: depth-only material fragment shaders -> the empty shader when provably "
            "without effect, their pipelines' vertex outputs trimmed (NR-32 vsout, '#vo' keys); gate records <shaders>/native-nr33b.xgspack (setup stage 8); policy family %s",m,
            m==1?"on":m==2?"check: the game's shaders drawn":"POISON: gated draws swapped even when their gate fails",vsmode,xbg_gpu_family_name(gpu_policy));});
    return mode;}
void open_pack(){std::call_once(pack_once,[]{if(!cfg.shader_directory)return;const std::string path=std::string(cfg.shader_directory)+"/native-nr33b.xgspack";
    int f=::open(path.c_str(),O_RDONLY|O_CLOEXEC);if(f<0){xbg_logf("[nr] mat no %s: no gate records (run setup again, stage 8): only shaders without a kill are swapped",path.c_str());return;}
    struct stat st{};uint32_t h[4]={};bool ok=!fstat(f,&st) && pread(f,h,16,0)==16 && h[0]==0x50534758u && h[1]==1u && h[2]<65536;
    std::vector<xpack::Ent> v;if(ok){v.resize(h[2]);ok=pread(f,v.data(),v.size()*sizeof(xpack::Ent),16)==ssize_t(v.size()*sizeof(xpack::Ent));}
    for(size_t i=0;ok && i<v.size();i++)ok=v[i].key[31]==0 && v[i].off+v[i].size<=uint64_t(st.st_size) && (!i || strcmp(v[i-1].key,v[i].key)<0);
    if(!ok){::close(f);red("%s is damaged or of another version: ignored (run setup again)",path.c_str());return;}
    fd=f;idx.swap(v);xbg_logf("[nr] mat %s: %zu gate records",path.c_str(),idx.size());});}
bool get(const std::string &key,std::vector<uint8_t> &out){open_pack();if(fd<0)return false;
    auto it=std::lower_bound(idx.begin(),idx.end(),key.c_str(),[](const xpack::Ent &a,const char *k){return strcmp(a.key,k)<0;});
    if(it==idx.end() || strcmp(key.c_str(),it->key))return false;out.resize(it->size);size_t got=0;
    while(got<out.size()){ssize_t r=pread(fd,out.data()+got,out.size()-got,off_t(it->off+got));if(r<0 && errno==EINTR)continue;if(r<=0)break;got+=size_t(r);}
    return got==out.size() && xpack::crc(out.data(),out.size())==it->hash;}
// load_shader: words = the final SPIR-V (after the load-time rewrites), raw = the bundle's own SPIR-V bytes (the gate's proof is on those)
void note(Shader &p,const std::vector<uint32_t> &words,const uint8_t *raw,size_t raw_n){
    if(on()<=0 || p.stage!=4)return;
    nr_mat_facts f;if(nr_mat_scan(words.data(),words.size(),&f))return;
    if(f.flags&(NR_MAT_FRAGOUT|NR_MAT_SIDE|NR_MAT_BAD|NR_MAT_COMPLEX_IO))return;
    p.nm_cls=uint8_t(f.flags&NR_MAT_KILL?C_KILL:C_PURE);
    std::vector<uint8_t> rec;if(!get("dg."+p.key.substr(0,16),rec))return;
    nr_mat_gate g;const char *why=nr_mat_gate_parse(rec.data(),rec.size(),&g);
    if(why){++s.gates_bad;++s.changed;red("gate record of %s refused: %s (run setup again)",p.key.c_str(),why);return;}
    const uint64_t h=nr_mat_fnv64(raw,raw_n);
    if(h!=g.spirv_fnv){++s.gates_stale;++s.changed;red("gate record of %s is for other SPIR-V (%016llx, the bundle %016llx): ignored (run setup again)",p.key.c_str(),
        (unsigned long long)g.spirv_fnv,(unsigned long long)h);return;}
    {std::lock_guard<std::mutex> l(mu);gates.push_back(g);p.nm_gate=&gates.back();}++s.gates_loaded;++s.changed;
    if(s.gates_loaded<=4)xbg_logf("[nr] mat %s: gate record, %u path%s (%s), %s",p.key.c_str(),g.paths,g.paths==1?"":"s",g.path[0].w_known?"Location 0 .w known":".w unknown",
        p.nm_cls==C_KILL?"a shader with a kill":"no kill instruction");}
// One word of a fragment uniform buffer (bank_word = Maxwell bank << 16 | word, NVN slot = bank - 3), read as GsPassthru reads its gate:
// unbound / out of range / covered by a pending GPU writeback = unreadable (the path fails, the game's shader is drawn).
int rd(void*,uint32_t bw,uint32_t *out){const unsigned bank=bw>>16,word=bw&0xffffu;if(bank<3 || bank-3>=16)return 0;
    const NVNbufferRange &r=state.ub[nvn_stage[4]][bank-3];const uint64_t a=r.address+uint64_t(word)*4;if(!r.address || uint64_t(word)*4+4>r.size)return 0;
    for(auto &w:pending_writebacks)if(xbg_wb_overlaps(a,4,w.addr,w.bytes))return 0;
    memcpy(out,reinterpret_cast<const void*>(uintptr_t(a)),4);return 1;}
Shader *twin(Shader &fs){
    if(fs.nm_twin)return fs.nm_twin;
    size_t n=0;const uint32_t *e=nr_mat_empty_fs(&n);const std::vector<uint32_t> w(e,e+n);const std::string key=fs.key+"#dz";
    auto *t=new Shader();t->key=key;t->stage=4;t->bindings=fs.bindings;t->c1=fs.c1;t->discards=false;t->empty_fs=fs.empty_fs;t->raster=fs.raster;t->pg_coord=fs.pg_coord;
    t->nm_is_twin=true;t->vo_read=0;t->vo_decl=0;   // reads / declares no varying: NR-32's vsout_module drops every Location output of the vertex shader
    if(bundle_module(w,&t->module,key)!=VK_SUCCESS){delete t;red("%s: the empty fragment module could not be made: the shader is kept",fs.key.c_str());fs.nm_cls=C_NONE;return nullptr;}
    warm::register_module(t->module,key);fs.nm_twin=t;++s.twins;++s.changed;
    if(s.twins<=4)xbg_logf("[nr] mat %s -> %s (the empty fragment shader in passes without colour attachments)",fs.key.c_str(),key.c_str());
    return t;}
struct Guard{Shader *saved=nullptr;~Guard(){if(saved)state.shader[4]=saved;}};
// draw(): swap the fragment shader for its twin when it provably has no effect in this pass (see the top)
void pre(Guard &g){
    Shader *fs=state.shader[4];if(!fs || fs->nm_cls==C_NONE || fs->nm_is_twin)return;
    for(int i=0;i<state.rt.n && i<8;i++)if(state.rt.color[i])return;   // a colour pass: the outputs are seen
    if(state.shader[1] || state.shader[2] || state.shader[3] || state.rast_discard)return;
    ++s.seen;
    if(fs->empty_fs){++s.already_empty;return;}
    if(state.ms.a2c){++s.a2c;return;}
    const uint32_t func=state.color.alpha_func;const bool alpha=func!=8 && alpha_test_on();
    const bool applied=alpha && alpha_module(*fs,func,state.alpha_ref)!=fs->module;   // a refused alpha test (no colour 0 .w) draws without it
    int ok=0;uint64_t *why=nullptr;
    if(!applied && fs->nm_cls==C_PURE){ok=1;++s.pure;}
    else if(fs->nm_gate){const nr_mat_gate &gt=*fs->nm_gate;const int p=nr_mat_gate_eval(&gt,rd,nullptr);
        if(p<0)why=&s.refuse_gate;
        else if(!applied){ok=1;++s.gate_off;}
        else if(gt.path[p].w_known && nr_mat_alpha_pass(func,gt.path[p].w_bits,state.alpha_ref)){ok=1;++s.gate_alpha;}
        else why=&s.refuse_alpha;}
    else why=fs->nm_cls==C_KILL?&s.refuse_kill:&s.refuse_nogate;
    if(!ok && mode==3 && why==&s.refuse_gate){ok=2;++s.poison;}
    if(!ok){++*why;return;}
    if(alpha && !applied)++s.alpha_na;
    if(mode==2){++s.would;return;}
    Shader *t=twin(*fs);if(!t)return;
    g.saved=fs;state.shader[4]=t;++s.swapped;}
// warm-up: 1 = a module of ours (m), -1 = ours but not made, 0 = not an NR-33B key
int warm_module(const std::string &key,VkShaderModule &m){
    const size_t c=key.find('#');if(c==std::string::npos || key.size()<c+3 || key[c+1]!='d' || (key[c+2]!='z' && key[c+2]!='p'))return 0;
    if(on()<=0 || key[c+2]=='p' || c+3!=key.size())return -1;   // "#dp<keep>": the earlier prune's key, not built (the twin's vertex stage is "#vo...")
    size_t n=0;const uint32_t *e=nr_mat_empty_fs(&n);const std::vector<uint32_t> w(e,e+n);
    if(bundle_module(w,&m,key)!=VK_SUCCESS)return -1;
    return 1;}
void tick(){static uint64_t next=0,last=~0ull;const uint64_t now=timing_clock();if(now<next)return;next=now+10000000000ull;
    const uint64_t c=s.changed+s.swapped+s.would+s.seen;if(c==last)return;last=c;
    xbg_logf("[nr] mat DM_NR_MAT=%d (%s) VS=%d: pack %s, gate records loaded %llu (stale %llu, bad %llu); depth-only material draws %llu: %s %llu "
        "(no kill + alpha off %llu, gate + alpha off %llu, gate + alpha passes w %llu, alpha test not applied %llu%s), kept: gate fails %llu, "
        "alpha fails w %llu, no record %llu, kill without gate %llu, alpha to coverage %llu, already empty %llu; twins %llu; twin pipelines with the NR-32 vertex trim "
        "%llu (NVN PERF xlq); red lines %llu",mode,mode==1?"on":mode==2?"check":"POISON",vsmode,fd>=0?"open":"none",
        (unsigned long long)s.gates_loaded,(unsigned long long)s.gates_stale,(unsigned long long)s.gates_bad,(unsigned long long)s.seen,
        mode==2?"would swap":"swapped",(unsigned long long)(mode==2?s.would:s.swapped),(unsigned long long)s.pure,(unsigned long long)s.gate_off,
        (unsigned long long)s.gate_alpha,(unsigned long long)s.alpha_na,mode==3?(", POISONED "+std::to_string(s.poison)).c_str():"",
        (unsigned long long)s.refuse_gate,(unsigned long long)s.refuse_alpha,(unsigned long long)s.refuse_nogate,(unsigned long long)s.refuse_kill,
        (unsigned long long)s.a2c,(unsigned long long)s.already_empty,(unsigned long long)s.twins,(unsigned long long)xlq_twin_pipes.load(),(unsigned long long)s.reds);}
}
