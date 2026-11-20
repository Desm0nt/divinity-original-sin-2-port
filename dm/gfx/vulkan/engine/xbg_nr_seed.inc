// [DOS2] NR-31: DM_NR_SEED - the portable pipeline warm-up seed (docs/dos2/nr/NR-31.md; core dm/gfx/nr/nr_seed.{h,c}).
// Included by xbg_game.cpp inside namespace warm, right after xbg_nr_pipe.inc (it needs Rec, recs, known, ready, mu, stop, threads,
// build(), bundle_words(), list_path(), nr::draw_builds). Hooks, one line each in xbg_game.cpp:
//   warm::record()          pseed::created(bytes)        an in-frame build: did the seed have it / what did it predict for these shaders
//   warm::take()            pseed::took(bytes)           a seed pipeline taken by a draw
//   warm::flush()           pseed::merge(pending), pseed::mark()   the expanded records go into this device's list; the marker after
//   warm::shutdown()        pseed::reset()
//   xbg_game_initialize     warm::pseed::launch(nrseed_policy())   at the END of initialisation (the depth / feature policies are set
//                           there, after warm::start); nrseed_policy() (xbg_nr_seed_policy.inc) reads this device's policy
// The seed is <shaders>/dos2-pipe-seed.bin or the APK asset the app copies to files/ (tools/dos2_pipe_seed.py build: a list pulled from one
// device + that device's policy; tools/build_apk.ps1 bundles dm/android/dos2/dos2-pipe-seed.bin when it exists).
// DM_NR_SEED (default xbg_nr_seed_default(): 1 - without a seed file in the APK nothing happens):
//   0  nothing (no file read, hooks behind one load and a not-taken branch)
//   1  once per seed id (marker <list>.seed): every record is re-targeted to this device (nsd_expand; refused records are counted,
//      never built), deduplicated against the list, built on background threads (as many as the warm-up when the list is empty -
//      a first boot - else 1) while the menu runs, and appended to this device's list once built; draws take them like list records
//   2  check: every boot, re-target all records, build nothing, write nothing; each in-frame build is looked up in the expansion
//      (would the seed have had it ready) and, when the seed holds a record for the same shaders that differs, a red line names the
//      first differing field (an expander model error) - first 16, then the summary lines
namespace pseed{
int mode=0;nsd_policy src{},dst{};nsd_dev dev{};uint64_t id=0;
std::vector<uint8_t> file;std::vector<size_t> offs;std::atomic<size_t> next{0};std::atomic<int> running{0};
std::atomic<uint64_t> expanded{0},dup{0},built_n{0},failed_n{0},refused[NSD_E_COUNT];std::atomic<bool> done{false};
std::unordered_set<std::string> mine;                       // mode 1: bytes of records the seed built, not yet taken (mu)
std::unordered_map<std::string,uint32_t> pred;              // bytes of every expanded record -> record index (mu)
std::unordered_map<std::string,std::vector<std::string>> by_keys;   // base stage keys "vs+fs" -> expanded bytes (mu)
std::vector<uint8_t> pending2;bool marked=false;            // list bytes waiting for warm::flush (mu); marker written
uint64_t taken=0,created_n=0,created_pred=0,created_differ=0,created_new=0,t_launch=0;unsigned red=0,told[NSD_E_COUNT]{};
std::mutex smu;struct Facts{nsd_shader s;unsigned have=0;bool ok=false;};std::unordered_map<std::string,std::unique_ptr<Facts>> facts;   // bundle facts (smu)
uint32_t vfmt[256]{},vconv[256]{};uint8_t att_c[256]{},att_b[256]{},att_d[256]{};   // this device's vertex fetch / attachment support, read once at launch
std::string marker_path(){return list_path()+".seed";}
std::string base_keys(const std::vector<std::string> &k){std::string s;for(auto &x:k){if(!s.empty())s+='+';s+=x.substr(0,x.find('#'));}return s;}
// the bundle's binding table / inputs (header) and, when asked, input classes / fragment outputs / uniform reads (NR-08b) from its final
// words (warm::bundle_words: the module the warm-up and load_shader build). [NR-31b] A fused post stage ("F1.<host>", "F1A.<host>": nsd_key
// base = the whole key) is composed as the draw path does (nbpost::warm_bundle; refused there when this device does not draw it).
int shader_cb(void*,const char *base,nsd_shader *out,unsigned want){
    const std::string key(base);Facts *f=nullptr;
    {std::lock_guard<std::mutex> l(smu);auto it=facts.find(key);if(it!=facts.end())f=it->second.get();}
    if(!f){auto nf=std::make_unique<Facts>();std::vector<uint8_t> b;bool from_pack=false;
        if((nbpost::fused_name(key)?nbpost::warm_bundle(key,b):xpack::read(key,b,from_pack)) && b.size()>=32){xbg_shader_header h;memcpy(&h,b.data(),sizeof h);
            const size_t need=sizeof h+size_t(h.binding_count)*sizeof(xbg_shader_binding)+size_t(h.input_count)*4;
            if(h.magic==0x31534758 && h.version==1 && h.stage<6 && h.binding_count<=NSD_MAX_BIND && h.input_count<=NSD_MAX_IN && need<=b.size()){
                nf->s.stage=h.stage;nf->s.nb=h.binding_count;for(uint32_t i=0;i<h.binding_count;i++){xbg_shader_binding x;memcpy(&x,b.data()+sizeof h+i*sizeof x,sizeof x);nf->s.b[i]={x.binding,x.type,x.count,x.source,x.slot};}
                nf->s.nin=h.input_count;memcpy(nf->s.in_loc,b.data()+sizeof h+size_t(h.binding_count)*sizeof(xbg_shader_binding),size_t(h.input_count)*4);nf->ok=true;}}
        std::lock_guard<std::mutex> l(smu);auto ins=facts.emplace(key,std::move(nf));f=ins.first->second.get();}
    if(!f->ok)return -1;
    unsigned miss;{std::lock_guard<std::mutex> l(smu);miss=want&~f->have;}
    if(miss){std::vector<uint32_t> w;uint32_t stage=0;nsd_shader t;{std::lock_guard<std::mutex> l(smu);t=f->s;}unsigned got=0;
        if(bundle_words(key,w,stage,"seed")){
            if((miss&NSD_W_CLASSES) && stage==0 && !xbg_vertex_input_classes(w.data(),w.size(),t.in_loc,t.nin,t.in_class)){t.classes=1;got|=NSD_W_CLASSES;}
            if((miss&NSD_W_FACTS) && stage==4){const XbgRasterFacts r=xbg_spirv_raster_facts(w.data(),w.size(),4);t.outputs=r.outputs;t.sample_mask=r.sample_mask;t.facts=1;got|=NSD_W_FACTS;}
            if(miss&NSD_W_READS){   // [NR-31b] nrubd::mark's scan: a uniform binding the final words do not read is never dynamic under the read-only rule
                uint32_t bn[NSD_MAX_BIND];uint8_t rd[NSD_MAX_BIND]{};unsigned n=0,at[NSD_MAX_BIND];
                for(uint32_t i=0;i<t.nb;i++)if(t.b[i].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER){bn[n]=t.b[i].binding;at[n++]=i;}
                if(n)nr_spv_binding_reads(w.data(),w.size(),bn,n,rd);
                memset(t.read,0,sizeof t.read);for(unsigned k=0;k<n;k++)t.read[at[k]]=rd[k];t.reads=1;got|=NSD_W_READS;}}
        std::lock_guard<std::mutex> l(smu);if(got&NSD_W_CLASSES){f->s.classes=1;memcpy(f->s.in_class,t.in_class,sizeof t.in_class);}if(got&NSD_W_FACTS){f->s.facts=1;f->s.outputs=t.outputs;f->s.sample_mask=t.sample_mask;}
        if(got&NSD_W_READS){f->s.reads=1;memcpy(f->s.read,t.read,sizeof t.read);}f->have|=got;}
    std::lock_guard<std::mutex> l(smu);*out=f->s;return 0;}
uint32_t vtx_cb(void*,uint32_t f,uint32_t *conv){*conv=f<256?vconv[f]:0;return f<256?vfmt[f]:0;}
uint32_t class_cb(uint32_t f){return xbg_vertex_format_class(int(f));}
int att_cb(void*,uint32_t f,int depth,int blend){if(f>=256)return 0;return depth?att_d[f]:blend?att_b[f]:att_c[f];}
void report(const char *why){   // mu held
    char r[400];int k=0;r[0]=0;for(int i=1;i<NSD_E_COUNT && k<int(sizeof r)-40;i++)if(refused[i].load())k+=snprintf(r+k,sizeof r-size_t(k)," %s %llu",nsd_reason_name(i),(unsigned long long)refused[i].load());
    xbg_logf("[nr] seed (%s, DM_NR_SEED=%d, %.1f s after launch): %zu records: expanded %llu, refused%s; %s%llu already in the list, built %llu, failed %llu, taken by draws %llu; "
             "in-frame builds since launch %llu: the seed had %llu of them%s, a different record for the same shaders %llu, shaders the seed has no record of %llu",
             why,mode,(timing_clock()-t_launch)/1e9,offs.size(),(unsigned long long)expanded.load(),k?r:" none",mode==2?"(check: nothing built) ":"",(unsigned long long)dup.load(),
             (unsigned long long)built_n.load(),(unsigned long long)failed_n.load(),(unsigned long long)taken,(unsigned long long)created_n,(unsigned long long)created_pred,
             mode==2?" (would have been warm)":" (not ready yet)",(unsigned long long)created_differ,(unsigned long long)created_new);}
void mark(){   // graphics thread (warm::flush) or the last seed thread with nothing to append: the seed is in this device's list
    if(mode!=1 || marked || !done.load())return;{std::lock_guard<std::mutex> l(mu);if(!pending2.empty())return;}
    marked=true;const std::string p=marker_path();if(FILE *f=fopen(p.c_str(),"wb")){fprintf(f,"%016llx\n",(unsigned long long)id);fclose(f);}
    xbg_logf("[nr] seed %016llx is in this device's list now (%s): later boots warm it from the list",(unsigned long long)id,p.c_str());}
void merge(std::vector<uint8_t> &into){if(mode!=1)return;std::lock_guard<std::mutex> l(mu);into.insert(into.end(),pending2.begin(),pending2.end());pending2.clear();}
void worker(){
    setpriority(PRIO_PROCESS,0,5);   // as the warm-up: below the game and graphics threads
    auto wk=std::make_unique<nsd_work>();auto out=std::make_unique<nsd_out>();auto r=std::make_unique<nsd_rec>();
    for(;;){if(stop.load())break;
        if(mode==1 && nr::draw_builds.load()){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}   // [NR-07] the draw path has the driver
        const size_t i=next.fetch_add(1);if(i>=offs.size())break;size_t at=offs[i];
        if(nsd_rec_next(file.data(),file.size(),&at,r.get())!=1)continue;
        const int e=nsd_expand(&src,&dst,&dev,r->key,r->klen,r->nk,r->words,r->nw,wk.get(),out.get());
        if(e){++refused[e];std::lock_guard<std::mutex> l(mu);if(told[e]++<2){std::string k;for(uint32_t s=0;s<r->nk;s++){if(s)k+='+';k.append(r->key[s],r->klen[s]);}
                xbg_logf("[nr] seed record %zu (%s) refused: %s (%s)",i,k.c_str(),nsd_reason_name(e),out->field);}continue;}
        ++expanded;Rec rec;for(uint32_t s=0;s<out->nk;s++)rec.keys.emplace_back(out->key[s],out->klen[s]);rec.words.assign(out->words,out->words+out->nw);
        std::string bytes(reinterpret_cast<const char*>(rec.words.data()),rec.words.size()*4);
        {std::lock_guard<std::mutex> l(mu);pred.emplace(bytes,uint32_t(i));by_keys[base_keys(rec.keys)].push_back(bytes);
            if(mode==2)continue;
            if(!known.insert(bytes).second){++dup;continue;}}
        if(build(rec)){++built_n;std::lock_guard<std::mutex> l(mu);mine.insert(bytes);   // in ready; this device's list gets it (warm::record's format)
            auto put=[&](uint32_t v){for(int b=0;b<4;b++)pending2.push_back(uint8_t(v>>(8*b)));};
            put(0x57504c58);put(uint32_t(rec.keys.size()));for(auto &k:rec.keys){put(uint32_t(k.size()));pending2.insert(pending2.end(),k.begin(),k.end());}
            put(uint32_t(rec.words.size()));pending2.insert(pending2.end(),bytes.begin(),bytes.end());}
        else ++failed_n;}
    if(--running==0){done=true;{std::lock_guard<std::mutex> l(smu);facts.clear();}   // ~4.4 KB a shader key: not kept past the expansion
        {std::lock_guard<std::mutex> l(mu);report(stop.load()?"stopped":"done");}if(!stop.load())mark();}
}
// at the end of xbg_game_initialize: d = this device's policy (nrseed_policy)
void launch(const nsd_policy &d){
    {const char *e=getenv("DM_NR_SEED");mode=e && *e?std::clamp(atoi(e),0,2):xbg_nr_seed_default();}
    if(!mode)return;
    if(!enabled){xbg_logf("[nr] DM_NR_SEED=%d: the warm-up is off (gfx-no-warm): nothing",mode);mode=0;return;}
    // <shaders>/dos2-pipe-seed.bin (a test push) first, else the APK's copy the app put in files/ (DmActivity.dos2PipeSeed; DM_MARKER_DIR = files/)
    std::string path=std::string(cfg.shader_directory)+"/dos2-pipe-seed.bin";
    {FILE *f=fopen(path.c_str(),"rb");
        if(!f){const char *fd=getenv("DM_MARKER_DIR");if(fd && *fd){const std::string p2=std::string(fd)+"/dos2-pipe-seed.bin";f=fopen(p2.c_str(),"rb");if(f)path=p2;}}
        if(!f){xbg_logf("[nr] DM_NR_SEED=%d: no seed (%s, files/): nothing to do",mode,path.c_str());mode=0;return;}
        fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n>0 && n<(64l<<20)){file.resize(size_t(n));if(fread(file.data(),1,file.size(),f)!=file.size())file.clear();}fclose(f);}
    nsd_seed_hdr h;if(nsd_seed_open(file.data(),file.size(),&h)){xbg_logf("【红】[nr] DM_NR_SEED=%d: %s is not a version-%u seed or damaged: not used",mode,path.c_str(),NSD_SEED_VERSION);file.clear();mode=0;return;}
    src=h.src;dst=d;id=h.id;
    // the dependencies this device's create_pass writes must be the core's model of them
    {VkSubpassDependency dd[2];pass_deps(dd);nsd_dep m[2];nsd_pass_deps(dst.tile_barrier,m);bool same=true;
        for(int i=0;i<2;i++)same=same && m[i].src==dd[i].srcSubpass && m[i].dst==dd[i].dstSubpass && m[i].src_stage==dd[i].srcStageMask && m[i].dst_stage==dd[i].dstStageMask
            && m[i].src_access==dd[i].srcAccessMask && m[i].dst_access==dd[i].dstAccessMask && m[i].flags==dd[i].dependencyFlags;
        if(!same){xbg_logf("【红】[nr] DM_NR_SEED=%d: create_pass's dependencies differ from nr_seed.c nsd_pass_deps (model drift): seed not used",mode);file.clear();mode=0;return;}}
    if(src.unmodelled){xbg_logf("【红】[nr] DM_NR_SEED=%d: the seed's source policy says its layouts are not the model's: not used",mode);file.clear();mode=0;return;}
    if(mode==1){FILE *m=fopen(marker_path().c_str(),"rb");if(m){unsigned long long got=0;const bool have=fscanf(m,"%llx",&got)==1 && got==id;fclose(m);
        if(have){xbg_logf("[nr] DM_NR_SEED=1: seed %016llx already expanded into this device's list (%s): nothing to do",(unsigned long long)id,marker_path().c_str());file.clear();mode=0;return;}}}
    // this device's vertex fetch (vertex_format_fallback, mirrored without its log lines: those stay with the draw that meets them) and attachments,
    // read here on the initialising thread (format_properties fills its cache lazily: not from the seed threads)
    for(uint32_t f=1;f<256;f++){const VkFormatProperties &p=format_properties(VkFormat(f));
        att_c[f]=(p.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)!=0;att_b[f]=(p.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)!=0;
        att_d[f]=(p.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)!=0;}
    auto usable=[](VkFormat x){return x!=VK_FORMAT_UNDEFINED && uint32_t(x)<256 && (format_properties(x).bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)!=0;};
    for(uint32_t f=1;f<256;f++){VkFormat v=VkFormat(f);uint32_t conv=0;
        if(!usable(v)){uint32_t kind=0;const int integer=xbg_vertex_scaled_int(int(v),&kind);
            if(integer){VkFormat t=VkFormat(integer);if(!usable(t))t=xbg_vertex_widen(t);if(usable(t)){v=t;conv=kind;}}
            else{const VkFormat w=xbg_vertex_widen(v);if(usable(w))v=w;}}
        vfmt[f]=usable(v)?uint32_t(v):0;vconv[f]=conv;}
    const VkPhysicalDeviceLimits &L=properties.limits;
    dev.u=nullptr;dev.shader=shader_cb;dev.vtx_format=vtx_cb;dev.vtx_class=class_cb;dev.att_ok=att_cb;
    dev.max_color=L.maxColorAttachments;dev.max_vi_bind=L.maxVertexInputBindings;dev.max_vi_attr=L.maxVertexInputAttributes;dev.max_vi_offset=L.maxVertexInputAttributeOffset;
    dev.max_vi_stride=L.maxVertexInputBindingStride;dev.color_samples=L.framebufferColorSampleCounts;dev.depth_samples=L.framebufferDepthSampleCounts;
    const uint32_t sl[5]={L.maxPerStageDescriptorSamplers,L.maxPerStageDescriptorUniformBuffers,L.maxPerStageDescriptorStorageBuffers,L.maxPerStageDescriptorSampledImages,L.maxPerStageDescriptorStorageImages};
    const uint32_t tl[5]={L.maxDescriptorSetSamplers,L.maxDescriptorSetUniformBuffers,L.maxDescriptorSetStorageBuffers,L.maxDescriptorSetSampledImages,L.maxDescriptorSetStorageImages};
    memcpy(dev.stage_lim,sl,sizeof sl);memcpy(dev.set_lim,tl,sizeof tl);dev.set_ubo_dyn=L.maxDescriptorSetUniformBuffersDynamic;dev.set_ssbo_dyn=L.maxDescriptorSetStorageBuffersDynamic;
    {size_t at=h.body;nsd_rec *r=new nsd_rec;for(;;){const size_t was=at;const int g=nsd_rec_next(file.data(),file.size(),&at,r);if(g!=1)break;offs.push_back(was);}delete r;}
    unsigned n=1;if(mode==1 && recs.empty())n=std::min<unsigned>(3,std::max(1u,std::thread::hardware_concurrency()/3));if(nr::mode)n=nr::threads(n);
    auto pol=[](const nsd_policy &p){char b[240];snprintf(b,sizeof b,"tile %u, depth stand-ins %u%u%u, bias x2 %u, dynamic UBO %u (limit %u%s%s), NOOUT %u, retype %u, alpha test %u, features %#x",
        p.tile_barrier,p.d24s8_as_d32s8,p.x8d24_as_d32,p.depth16_as_d24,p.bias_x2,p.ubo_dynamic,p.ubo_dyn_limit,p.split?", NR-08 split":"",p.ubo_ro?", NR-08b read-only":"",p.noout,p.retype,p.alpha_test,p.features);return std::string(b);};
    xbg_logf("[nr] DM_NR_SEED=%d (NR-31: %s): seed %016llx, %zu records, from a device with %s; this device: %s; list %zu records; %u thread(s)",mode,
             mode==1?"re-targeted, built in the background, added to the list":"check - re-targeted, nothing built, in-frame builds compared",(unsigned long long)id,offs.size(),
             pol(src).c_str(),pol(dst).c_str(),recs.size(),n);
    t_launch=timing_clock();stop=false;running=int(n);for(unsigned i=0;i<n;i++)threads.emplace_back(worker);}
// warm::record (an in-frame build of this draw path, graphics thread): bytes = its create-info key
void created(const VkGraphicsPipelineCreateInfo &ci,const std::string &bytes){
    std::lock_guard<std::mutex> l(mu);++created_n;
    std::vector<std::string> keys;for(uint32_t i=0;i<ci.stageCount;i++){auto it=ids.find(ci.pStages[i].module);if(it==ids.end()){++created_new;return;}keys.push_back(id_keys[it->second]);}
    if(pred.count(bytes)){++created_pred;return;}
    auto f=by_keys.find(base_keys(keys));if(f==by_keys.end()){++created_new;return;}
    ++created_differ;
    if(red<16){++red;const std::string &s=f->second.front();const uint32_t *a=reinterpret_cast<const uint32_t*>(bytes.data()),*b=reinterpret_cast<const uint32_t*>(s.data());
        const size_t na=bytes.size()/4,nb=s.size()/4;size_t at=0;while(at<na && at<nb && a[at]==b[at])++at;
        std::string k;for(auto &x:keys){if(!k.empty())k+='+';k+=x;}
        xbg_logf("【红】[nr] seed check: an in-frame build (%s) has a seed record for the same shaders that differs at word %zu (%s: draw %#x, seed %#x; %zu / %zu words, %zu seed records for them) - "
                 "a policy the expander does not model, or another draw state",k.c_str(),at,nsd_field_at(a,na,at),at<na?a[at]:0u,at<nb?b[at]:0u,na,nb,f->second.size());}
    if(!(created_n&(created_n-1)) && created_n>=8)report("in-frame builds");}
void took(const std::string &bytes){if(mine.erase(bytes))++taken;}   // mu held (warm::take)
void reset(){if(mode && !offs.empty())report("shutdown");   // mu held (warm::shutdown, after the threads joined)
    mine.clear();pred.clear();by_keys.clear();pending2.clear();offs.clear();file.clear();file.shrink_to_fit();facts.clear();next=0;running=0;done=false;mode=0;}
}
