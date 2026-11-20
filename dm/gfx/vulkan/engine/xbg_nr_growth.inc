// [DOS2] NR-DRIFT (docs/dos2/nr/NR-DRIFT.md): the "[nr] growth" line and the GPU clock probe. Included by xbg_game.cpp after every
// container it reads (near the end of the unit); recording_slot / retire_slot / the perf report call it through the forward
// declarations next to PerfCounts. Policy (load screens, probe calibration) in dm/gfx/nr/nr_drift.h (pure C, tests/nr_drift_test.c).
//
//   DM_NR_GROWTH=1 (DOS2 default; 0 = off, nothing below runs; other games 0): every DM_NR_GROWTH_S (10) s one line
//     [nr] growth (10 s, frame F, load L s ago, level X): sizes ...; since load ...; walks ...; gpu ...; mem ...; cpu ...; cost N us
//     sizes   every cache / list / pool the backend keeps that could grow with play time (pipelines and their memo maps, shaders,
//             programs, layouts, samplers, render passes, framebuffers, images + MB, descriptor records / memo / pools / spare sets,
//             image-view memo, texel memo, query epochs + queued queries, pending reports / writebacks / events, deferred-destroy
//             entries, arenas, the zero-copy chunks, the vertex cache, host segments)
//     since load   the same sizes minus their values when the last load screen ended (nrd_load_*: < 64 draws a frame)
//     walks   ns a frame spent in retire_slot's cleanup (the per-frame walks over pending reports / epochs / views / sets)
//     gpu     the probe (DM_NR_GPUPROBE): ALU index (100 = the clock of the first stable run), ns / iteration, the copy's GB/s and its
//             index; the DRS height and whether DM_DOS2_DRS_SETTLE holds it; Android thermal status + headroom
//     mem     process RSS
//     cpu     scaling_cur_freq of each cpufreq policy (MHz), and every readable devfreq / kgsl GPU clock node (none on a stock
//             Android 16 phone without root: then "devfreq none readable" once)
//   DM_NR_GPUPROBE=1 (DOS2 default with the line; 0 = off): at the start of one submission every DM_NR_GROWTH_S / 2 s, alternately
//     (a) an ALU loop (xbg_nr_probe.comp, one dispatch of 8 x 64 threads, n iterations calibrated to 40..400 us) between two
//     BOTTOM_OF_PIPE stamps, (b) a 4 MB -> 4 MB device-local buffer copy between two stamps; then a full barrier so the game's work
//     does not overlap the next measurement. Results are read without waiting at the next line. Cost: one ~0.1-0.5 ms GPU item every
//     5 s (0.00002-0.0001 ms a frame on average) + 8 MB of device memory + one tiny compute pipeline.
namespace nrgrowth{
int mode=-1,probe_mode=-1;uint64_t period_ns=10000000000ull,next_line=0,next_probe=0,lines=0,cost_ns=0;
nrd_load_t load;nrd_probe_t probe;
uint64_t retire_ns=0,retire_n=0;   // retire_slot's cleanup (from c0 to its end), accumulated between lines
// ---- sizes ----
enum{S_PIPES,S_PFAST,S_POD,S_SHADERS,S_PROGRAMS,S_LAYOUTS,S_SAMPLERS,S_SFAST,S_PASSES,S_LAZY,S_FBS,S_IMAGES,S_IMG_MB,S_HOSTSEG,
     S_DESC,S_DFREE,S_MEMO,S_PINNED,S_SPARE,S_DPOOLS,S_IMGCACHE,S_TEXEL,S_EPOCHS,S_EQUERIES,S_PREPORTS,S_PWB,S_PIWB,S_PEVENTS,S_RETIRE,
     S_OCCPOOLS,S_TSPOOLS,S_ARENA_CH,S_ZC_CH,S_ZC_MB,S_VC,S_VC_MB,S_N};
const char *const names[S_N]={"pipelines","pipe_fast","pod","shaders","programs","layouts","samplers","sampler_fast","passes","lazy",
     "fbs","images","image_mb","host_segs","desc","desc_free","memo","pinned","spare_sets","desc_pools","img_cache","texel","epochs",
     "epoch_queries","pend_reports","pend_wb","pend_img_wb","pend_events","retire","occ_pools","ts_pools","arena_chunks","zc_chunks","zc_mb",
     "vcache","vcache_mb"};
struct Snap{double v[S_N];};Snap at_load{};bool have_load=false;uint64_t load_at=0;
Snap take(){Snap s{};auto &v=s.v;
    v[S_PIPES]=double(pipeline_objects.size());v[S_PFAST]=double(pipeline_fast.size());v[S_POD]=double(pipe_pod.size());
    v[S_SHADERS]=double(shaders.size());v[S_PROGRAMS]=double(program_cache.size());v[S_LAYOUTS]=double(layout_cache.size());
    v[S_SAMPLERS]=double(sampler_cache.size());v[S_SFAST]=double(sampler_fast.size());v[S_PASSES]=double(render_pass_cache.size());
    v[S_LAZY]=double(lazy_info.size()+lazy_twins.size());v[S_FBS]=double(persistent_framebuffers.size());v[S_IMAGES]=double(images.size());
    v[S_IMG_MB]=double(image_bytes_live)/1048576.0;v[S_HOSTSEG]=double(host_segments.size());v[S_DESC]=double(desc_all.size());
    v[S_DFREE]=double(desc_free.size());v[S_MEMO]=double(desc_memo_slot[0].size()+desc_memo_slot[1].size());
    v[S_PINNED]=double(memo_pinned[0].size()+memo_pinned[1].size());v[S_IMGCACHE]=double(img_cache.size());
    v[S_TEXEL]=double(texel_resident[0].size()+texel_resident[1].size());v[S_EPOCHS]=double(epochs.size());
    {double q=0;for(auto &e:epochs)q+=double(e.second.q.size());v[S_EQUERIES]=q;}
    v[S_PREPORTS]=double(pending_reports.size());v[S_PWB]=double(pending_writebacks.size());v[S_PIWB]=double(pending_image_writebacks.size());
    v[S_PEVENTS]=double(pending_events.size()+recording_events.size());
    {double sp=0,dp=0,occ=0,ts=0,ret=0,ar=0;for(auto &s:slots){for(auto &x:s.spare_sets)sp+=double(x.second.size());dp+=double(s.pools.size());occ+=double(s.occlusion_pools.size());
        ts+=double(s.timestamp_pools.size());ret+=double(s.retire.handles.size()+s.retire.objects.size());ar+=double(s.stream.chunks.size()+s.readback.chunks.size());}
     v[S_SPARE]=sp;v[S_DPOOLS]=dp;v[S_OCCPOOLS]=occ;v[S_TSPOOLS]=ts;v[S_RETIRE]=ret;v[S_ARENA_CH]=ar;}
    v[S_ZC_CH]=zc::enabled?double(zc::live_chunks):0;v[S_ZC_MB]=zc::enabled?double(zc::mapped_bytes)/1048576.0:0;
    v[S_VC]=vcache::enabled?double(vcache::live()):0;v[S_VC_MB]=vcache::enabled?double(vcache::arena_bytes())/1048576.0:0;
    return s;}
// ---- the probe ----
const VkDeviceSize COPY_BYTES=4ull<<20;
bool tried=false,ready=false;VkBuffer src{},dst{};VkDeviceMemory src_mem{},dst_mem{};VkQueryPool qp{};VkPipeline pipe{};VkPipelineLayout layout{};
VkDescriptorSetLayout dsl{};VkDescriptorPool dpool{};VkDescriptorSet dset{};
int next_kind=0;          // 0 = ALU next, 1 = copy next
bool alu_pending=false,copy_pending=false;uint64_t alu_at=0,copy_at=0;uint32_t alu_n=0;   // *_at: when the item was recorded (read it 0.25 s later at the earliest: the reset in the command buffer must have run)
double alu_us=-1,copy_us=-1,copy_gbs=-1,copy_base=-1;uint64_t alu_runs=0,copy_runs=0,unread=0;
bool prepare(){
    if(ready)return true;if(tried)return false;tried=true;
    auto fail=[](const char *what,VkResult r){xbg_logf("[nr] growth: GPU probe off (%s result=%d)",what,int(r));return false;};VkResult r;
    if(!timestamp_bits)return fail("no timestamps on the graphics queue",VK_ERROR_FEATURE_NOT_PRESENT);
    auto buffer=[&](VkBuffer &b,VkDeviceMemory &m,VkBufferUsageFlags u)->VkResult{VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=COPY_BYTES;bi.usage=u;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VkResult rr=vkCreateBuffer(cfg.device,&bi,nullptr,&b);if(rr!=VK_SUCCESS)return rr;VkMemoryRequirements mr;vkGetBufferMemoryRequirements(cfg.device,b,&mr);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if((rr=vkAllocateMemory(cfg.device,&ma,nullptr,&m))!=VK_SUCCESS)return rr;return vkBindBufferMemory(cfg.device,b,m,0);};
    if((r=buffer(src,src_mem,VK_BUFFER_USAGE_TRANSFER_SRC_BIT))!=VK_SUCCESS)return fail("source buffer",r);
    if((r=buffer(dst,dst_mem,VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))!=VK_SUCCESS)return fail("target buffer",r);
    VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=4;if((r=vkCreateQueryPool(cfg.device,&q,nullptr,&qp))!=VK_SUCCESS)return fail("query pool",r);
    VkDescriptorSetLayoutBinding b{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=1;dl.pBindings=&b;
    if((r=vkCreateDescriptorSetLayout(cfg.device,&dl,nullptr,&dsl))!=VK_SUCCESS)return fail("set layout",r);
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,8};VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&dsl;li.pushConstantRangeCount=1;li.pPushConstantRanges=&push;
    if((r=vkCreatePipelineLayout(cfg.device,&li,nullptr,&layout))!=VK_SUCCESS)return fail("pipeline layout",r);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&ps;
    if((r=vkCreateDescriptorPool(cfg.device,&pi,nullptr,&dpool))!=VK_SUCCESS)return fail("descriptor pool",r);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=dpool;ai.descriptorSetCount=1;ai.pSetLayouts=&dsl;
    if((r=vkAllocateDescriptorSets(cfg.device,&ai,&dset))!=VK_SUCCESS)return fail("descriptor set",r);
    {VkDescriptorBufferInfo bi{dst,0,8*64*sizeof(float)};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=dset;w.dstBinding=0;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&bi;vkUpdateDescriptorSets(cfg.device,1,&w,0,nullptr);}
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=sizeof(xbg_nr_probe_cs);mi.pCode=xbg_nr_probe_cs;VkShaderModule module{};
    {gpguard::ModuleLock serial;r=vkCreateShaderModule(cfg.device,&mi,nullptr,&module);}if(r!=VK_SUCCESS)return fail("shader module",r);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=layout;
    {gpguard::Lock serial;r=vkCreateComputePipelines(cfg.device,pipeline_cache,1,&ci,nullptr,&pipe);}vkDestroyShaderModule(cfg.device,module,nullptr);
    if(r!=VK_SUCCESS){pipe={};return fail("compute pipeline",r);}
    ready=true;xbg_logf("[nr] growth: GPU probe ready (ALU loop 8 x 64 threads from %u iterations, copy %llu MB device-local, 4 stamps, one item every %.1f s)",
        probe.n,(unsigned long long)(COPY_BYTES>>20),double(period_ns)/2e9);return true;}
// Reads what the last probes left (no wait; a probe not yet executed stays pending).
void collect(uint64_t now){
    if(!ready)return;
    const uint64_t settle=250000000ull,stale=3*period_ns;   // an item never executed (its submission dropped) is given up after 3 periods
    if(alu_pending && now-alu_at>stale){alu_pending=false;++unread;}
    if(copy_pending && now-copy_at>stale){copy_pending=false;++unread;}const uint64_t mask=timestamp_bits<64?(uint64_t(1)<<timestamp_bits)-1:~0ull;const double per=properties.limits.timestampPeriod/1000.0;
    struct St{uint64_t v,a;};
    if(alu_pending && now-alu_at>=settle){St t[2]{};if(vkGetQueryPoolResults(cfg.device,qp,0,2,sizeof t,t,sizeof(St),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)!=VK_ERROR_DEVICE_LOST && t[0].a && t[1].a){
        alu_pending=false;alu_us=double((t[1].v-t[0].v)&mask)*per;if(nrd_probe_alu(&probe,alu_n,alu_us))++alu_runs;}}
    if(copy_pending && now-copy_at>=settle){St t[2]{};if(vkGetQueryPoolResults(cfg.device,qp,2,2,sizeof t,t,sizeof(St),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)!=VK_ERROR_DEVICE_LOST && t[0].a && t[1].a){
        copy_pending=false;copy_us=double((t[1].v-t[0].v)&mask)*per;copy_gbs=nrd_copy_gbs(COPY_BYTES,copy_us);if(copy_gbs>0){if(copy_base<0)copy_base=copy_gbs;++copy_runs;}}}
}
void init(){
    if(mode>=0)return;
    const char *e=getenv("DM_NR_GROWTH");
#ifdef DM_GAME_DOS2
    mode=1;
#else
    mode=0;
#endif
    if(e && *e)mode=atoi(e)?1:0;
    const char *s=getenv("DM_NR_GROWTH_S");if(s && *s){double v=atof(s);if(v<2)v=2;if(v>600)v=600;period_ns=uint64_t(v*1e9);}
    const char *p=getenv("DM_NR_GPUPROBE");probe_mode=0;if(p && *p)probe_mode=mode && atoi(p)?1:0;   // review 19.11: off unless asked (DM_NR_GPUPROBE=1) until a phone run proves it cheap
    nrd_load_init(&load);nrd_probe_init(&probe);
    xbg_logf("[nr] growth DM_NR_GROWTH=%d DM_NR_GPUPROBE=%d (NR-DRIFT): one line every %.0f s with every cache / list size, the deltas since the last load "
        "screen (< %d draws a frame), the per-frame cleanup walk, the GPU clock probe, DRS, thermal, RSS, CPU / devfreq clocks",mode,probe_mode,double(period_ns)/1e9,NRD_LOAD_DRAWS);}
// recording_slot, right after its ordering barrier: at most one probe item per half period, outside any render pass
void slot_begin(VkCommandBuffer cmd){
    if(mode<0)init();if(probe_mode<=0)return;
    const uint64_t now=timing_clock();if(now<next_probe)return;
    if(!next_probe){next_probe=now+period_ns/2;return;}   // not in the boot frames
    next_probe=now+period_ns/2;if(!prepare())return;
    VkMemoryBarrier all{VK_STRUCTURE_TYPE_MEMORY_BARRIER};all.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;all.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    if(next_kind==0 && !alu_pending){
        vkCmdResetQueryPool(cmd,qp,0,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,qp,0);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&dset,0,nullptr);   // game dispatches bind their own
        const uint32_t pc[2]={probe.n,0x3f800000u};vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof pc,pc);vkCmdDispatch(cmd,8,1,1);
        vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,qp,1);alu_n=probe.n;alu_pending=true;alu_at=now;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&all,0,nullptr,0,nullptr);
    }else if(next_kind==1 && !copy_pending){
        vkCmdResetQueryPool(cmd,qp,2,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,qp,2);
        VkBufferCopy r{0,0,COPY_BYTES};vkCmdCopyBuffer(cmd,src,dst,1,&r);
        vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,qp,3);copy_pending=true;copy_at=now;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&all,0,nullptr,0,nullptr);
    }
    next_kind^=1;}
// ---- sysfs clocks ----
// Clock nodes (KB PERF-X09, from D3 perf_android.c gpu_probe): Adreno kgsl (Hz), MediaTek GED "<opp> <kHz>" (the last number), gpufreq v2
// "... Freq: <kHz>", gpufreq v1 "cur_gpu_freq = <kHz>", Mali generic, and every /sys/class/devfreq/*/cur_freq (GPU and, on MediaTek /
// Exynos, the DDR / bus governors). Hz / kHz / MHz told apart by size; printed in MHz. Most are closed to apps (SELinux): listed once.
enum{NK_NUM=NRD_CLK_NUM,NK_LAST=NRD_CLK_LAST,NK_TAG_FREQ=NRD_CLK_TAG_FREQ,NK_TAG_CUR=NRD_CLK_TAG_CUR};
struct Node{std::string path,name;int kind;};std::vector<Node> freq_nodes;bool nodes_scanned=false;int cpu_policies[16];int n_policies=0;
long read_long(const char *path){FILE *f=fopen(path,"re");if(!f)return -1;long v=-1;if(fscanf(f,"%ld",&v)!=1)v=-1;fclose(f);return v;}
long node_mhz(const char *path,int kind){char buf[2048];FILE *f=fopen(path,"re");if(!f)return -1;const size_t n=fread(buf,1,sizeof buf-1,f);fclose(f);if(!n)return -1;buf[n]=0;return nrd_clock_mhz(buf,kind);}
void scan_nodes(){
    nodes_scanned=true;
    for(int p=0;p<16;p++){char path[96];snprintf(path,sizeof path,"/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq",p);if(read_long(path)>0)cpu_policies[n_policies++]=p;}
    static const struct{const char *path,*name;int kind;} fixed[]={{"/sys/class/kgsl/kgsl-3d0/gpuclk","kgsl.gpuclk",NK_NUM},{"/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq","kgsl.devfreq",NK_NUM},
        {"/sys/kernel/ged/hal/current_freqency","ged.cur",NK_LAST},{"/sys/kernel/ged/hal/gpu_cur_freq","ged.gpu_cur",NK_LAST},{"/proc/gpufreqv2/gpufreq_status","gpufreqv2",NK_TAG_FREQ},
        {"/proc/gpufreq/gpufreq_var_dump","gpufreq",NK_TAG_CUR},{"/sys/class/misc/mali0/device/cur_freq","mali.cur",NK_NUM},{"/sys/kernel/gpu/gpu_clock","gpu.clock",NK_NUM}};
    for(auto &f:fixed)if(node_mhz(f.path,f.kind)>0)freq_nodes.push_back({f.path,f.name,f.kind});
    if(DIR *d=opendir("/sys/class/devfreq")){while(dirent *e=readdir(d)){if(e->d_name[0]=='.')continue;std::string p=std::string("/sys/class/devfreq/")+e->d_name+"/cur_freq";
        if(node_mhz(p.c_str(),NK_NUM)>0 && freq_nodes.size()<14)freq_nodes.push_back({p,std::string("devfreq.")+e->d_name,NK_NUM});}closedir(d);}
    std::string list;for(auto &n:freq_nodes)list+=" "+n.name;
    xbg_logf("[nr] growth: cpufreq policies %d readable; GPU / bus clock nodes readable (MHz in the line):%s",n_policies,list.empty()?" none (SELinux / no root: the probe stands in)":list.c_str());}
#ifdef DM_GAME_DOS2
extern "C" int dm_dos2_drs_info(float *pct,int *hold,float *target_ms) __attribute__((weak));
#endif
extern "C" int dm_thermal_now(void) __attribute__((weak));
extern "C" float dm_thermal_headroom(void) __attribute__((weak));
// the perf report, every 60 frames: load screens, and the line once a period
void window(double draws_per_frame,uint64_t now){
    if(mode<0)init();if(mode<=0)return;
    const int ev=nrd_load_step(&load,draws_per_frame,now);
    if(ev==1){at_load=take();have_load=true;load_at=now;}
    if(!next_line){next_line=now+period_ns;return;}
    if(now<next_line)return;next_line=now+period_ns;
    const uint64_t t0=timing_clock();
    if(!nodes_scanned)scan_nodes();
    collect(timing_clock());
    const Snap cur=take();
    std::string l;l.reserve(1800);char b[256];
    {char lvl[64]="-",prev[64]="",when[48];double kb=0;if(dm_dos2_heat_level)dm_dos2_heat_level(lvl,sizeof lvl,prev,sizeof prev,&kb);
     if(load.in_load)snprintf(when,sizeof when,"load screen now");else if(have_load)snprintf(when,sizeof when,"load %.0f s ago",double(now-load_at)/1e9);else snprintf(when,sizeof when,"no load seen yet");
     snprintf(b,sizeof b,"[nr] growth (%.0f s, frame %llu, %s, loads %u, level %s): sizes",double(period_ns)/1e9,(unsigned long long)stats.presents.load(),when,load.loads,lvl);l+=b;}
    for(int i=0;i<S_N;i++){if(cur.v[i]<0)continue;snprintf(b,sizeof b," %s=%.*f",names[i],(i==S_IMG_MB||i==S_ZC_MB||i==S_VC_MB)?1:0,cur.v[i]);l+=b;}
    if(have_load){l+="; since load";int shown=0;for(int i=0;i<S_N;i++){if(cur.v[i]<0)continue;const double d=cur.v[i]-at_load.v[i];if(d>-0.05 && d<0.05)continue;
        snprintf(b,sizeof b," %s%+.*f",names[i],(i==S_IMG_MB||i==S_ZC_MB||i==S_VC_MB)?1:0,d);l+=b;++shown;}if(!shown)l+=" none";}
    snprintf(b,sizeof b,"; walks retire cleanup %.1f us a frame (%llu frames)",retire_n?double(retire_ns)/1000.0/double(retire_n):0.0,(unsigned long long)retire_n);l+=b;retire_ns=retire_n=0;
    if(probe_mode>0){
        if(!ready)l+="; gpu probe off";
        else{snprintf(b,sizeof b,"; gpu alu %s%.1f (%.3f ns/iter, %.0f us, n %u, runs %llu, recal %u) copy %.0f us %.1f GB/s index %s%.1f (runs %llu)",
                nrd_probe_index(&probe)>=0?"":"? ",nrd_probe_index(&probe),probe.last_ns_iter,alu_us,probe.n,(unsigned long long)alu_runs,probe.recal,
                copy_us,copy_gbs,copy_base>0?"":"? ",copy_base>0 && copy_gbs>0?100.0*copy_gbs/copy_base:-1.0,(unsigned long long)copy_runs);l+=b;
             if(unread){snprintf(b,sizeof b," given up %llu",(unsigned long long)unread);l+=b;}}}
#ifdef DM_GAME_DOS2
    if(dm_dos2_drs_info){float pct=-1,tgt=0;int hold=0;dm_dos2_drs_info(&pct,&hold,&tgt);snprintf(b,sizeof b,"; drs %.1f %% target %.2f ms%s",pct,tgt,hold?" (settle hold)":"");l+=b;}
#endif
    {const int ts=dm_thermal_now?dm_thermal_now():-1;const float h=dm_thermal_headroom?dm_thermal_headroom():NAN;snprintf(b,sizeof b,"; thermal %d headroom %.2f",ts,double(h));l+=b;}
    {long pages=-1,res=-1;if(FILE *f=fopen("/proc/self/statm","re")){if(fscanf(f,"%ld %ld",&pages,&res)!=2)res=-1;fclose(f);}
     snprintf(b,sizeof b,"; mem rss %.0f MB",res>0?double(res)*double(sysconf(_SC_PAGESIZE))/1048576.0:-1.0);l+=b;
     }   // (VK_EXT_memory_budget: vkGetPhysicalDeviceMemoryProperties2 is not exported by the driver shim here - RSS only)
    if(n_policies || !freq_nodes.empty()){l+="; cpu MHz";for(int i=0;i<n_policies;i++){char path[96];snprintf(path,sizeof path,"/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq",cpu_policies[i]);
        snprintf(b,sizeof b," p%d %ld",cpu_policies[i],read_long(path)/1000);l+=b;}
     for(auto &n:freq_nodes){snprintf(b,sizeof b," %s %ld",n.name.c_str(),node_mhz(n.path.c_str(),n.kind));l+=b;}}
    const uint64_t dt=timing_clock()-t0;cost_ns+=dt;++lines;
    snprintf(b,sizeof b,"; cost %.0f us (%.1f us a line on average)",double(dt)/1000.0,double(cost_ns)/1000.0/double(lines));l+=b;
    xbg_logf("%s",l.c_str());}
void shutdown(){if(!ready && !tried)return;
    if(pipe)vkDestroyPipeline(cfg.device,pipe,nullptr);if(layout)vkDestroyPipelineLayout(cfg.device,layout,nullptr);if(dpool)vkDestroyDescriptorPool(cfg.device,dpool,nullptr);
    if(dsl)vkDestroyDescriptorSetLayout(cfg.device,dsl,nullptr);if(qp)vkDestroyQueryPool(cfg.device,qp,nullptr);
    if(src)vkDestroyBuffer(cfg.device,src,nullptr);if(dst)vkDestroyBuffer(cfg.device,dst,nullptr);if(src_mem)vkFreeMemory(cfg.device,src_mem,nullptr);if(dst_mem)vkFreeMemory(cfg.device,dst_mem,nullptr);
    pipe={};layout={};dpool={};dsl={};qp={};src=dst={};src_mem=dst_mem={};dset={};ready=false;tried=false;alu_pending=copy_pending=false;}
}
