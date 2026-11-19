// [DOS2] NR-21: DM_NR_SORT - the draws of a depth-only segment recorded in pipeline order (docs/dos2/nr/NR-21.md; plan
// NATIVE_RENDERER_PLAN.md S6 / S3, audit AUDIT-REMAINING.md NR-21). Decisions (the condition, the order, the bind model, the checks) in
// dm/gfx/nr/nr_sort.h (pure C, tests/nr_sort_test.c: the proof of order independence on a model of the depth test). Included by
// xbg_game.cpp right after xbg_nr_draw.inc; declared ahead of the vkCmd* wrappers (NBMT_PRE) and ahead of draw().
//
// What moves. Only draws nr_draw (NR-09, DM_NR_DRAW=1) serves: they carry everything their commands need (pipeline from the direct
// table, descriptor set + dynamic offsets, folded vertex / index binds, viewport, scissor, the draw's arguments). When such a draw is
// order-free (nr_sort.h nrs_why: depth only - no colour channel written, no stencil, no storage, no query, no depth bounds, no alpha to
// coverage, no depth feedback read, no conditional render, a fixed-point depth attachment, depth write only with LESS / LEQUAL / NEVER),
// lane() does all its bookkeeping and hands the commands to take() instead of recording them: the draw joins the open segment.
// What ends a segment: the first other vkCmd* on that command buffer - every wrapper of xbg_nb_mt.inc calls NBMT_PRE first (pass end,
// clear, query, timestamp, barrier, push constants, conditional render, a draw of another path...), the end of the submission
// (nbmt::end) and nbmt::sync - and explicitly any draw recorded at once (lane() for a draw that is not order-free, the old tail of
// draw()) right before it reads the bind cache. The segment's draws are then recorded in the order of the mode, through the same bind
// cache rules as lane() (one command per changed bind), and every command after them comes after them: only draws of one segment, with
// nothing between them, change places - nr_sort.h proves that their result is the same bits in every order.
// The bind cache after a segment: exact when a draw path ended it (that path reads the cache next); reset when any other command did
// (that code may bind without telling the cache).
//
//   DM_NR_SORT  0  default: nrsort::pend stays null - each vkCmd* wrapper pays one relaxed load and a compare, lane() one load
//                  (nrsort::mode). No queue (the sort arrays, ~1.1 MB of .bss, stay untouched).
//               1  on: a segment is recorded sorted by DM_NR_SORT_KEY.
//               2  compare: the segment is queued and recorded in the recorded order (what 0 records, deferred); the sorted order is
//                  computed and modelled (pipeline / set / viewport / scissor / vertex / index binds of both keys), the order is
//                  checked (a permutation of the segment, stable among equal keys), the bind model of the recorded order is checked
//                  against what was really recorded, and every queued draw's pipeline against its create info (stencil / bounds /
//                  alpha to coverage off, the colour masks of its targets 0, depth write only LESS / LEQUAL / NEVER). Red lines
//                  "【红】[nr] sort: ..." (first 16) + the check line every 10 s.
//               3  poison / proof: the segment is recorded in reverse order (the most different order; the picture and NVN GVAL of
//                  the depth passes must not change).
//   DM_NR_SORT_KEY  0  viewport + scissor, then pipeline, then the first vertex stream (keeps the cascades of a shadow atlas whole);
//                   1  pipeline first (fewest pipeline binds, more viewport / scissor sets when a pass has several viewports)
//   DM_NR_SORT_CAP  2048  draws a segment holds at most (16..4096); a full segment is recorded and a new one starts
//   gfx-ab-target = nrsort  in-process A/B: in the off phase nothing is queued
// Needs DM_NR_DRAW=1 (the queued draws are nr_draw's). Line "[nr] sort: ..." every 10 s.
namespace nrsort{
std::atomic<VkCommandBuffer> pend{VK_NULL_HANDLE};
int mode=0,key=0;bool read_env=false;uint32_t cap=2048;
enum{W_DRAW=nbmt::OP_COUNT,W_SUBMIT,W_SYNC,W_PASS,W_CAP,W_LOST,W_N};
const char *by_name(unsigned w){
    static const char *const op[]={"?","BeginRenderPass","EndRenderPass","BindPipeline","BindDescriptorSets","BindVertexBuffers","BindIndexBuffer","PushConstants",
        "SetViewport","SetScissor","Draw","DrawIndexed","DrawIndirect","DrawIndexedIndirect","DrawIndirectCount","DrawIndexedIndirectCount","Dispatch","DispatchIndirect",
        "ClearAttachments","ClearColorImage","ClearDepthStencilImage","CopyBuffer","CopyBufferToImage","CopyImageToBuffer","CopyImage","BlitImage","ResolveImage",
        "FillBuffer","PipelineBarrier","BeginQuery","EndQuery","ResetQueryPool","WriteTimestamp","CopyQueryPoolResults","BeginConditionalRendering",
        "EndConditionalRendering","NextSubpass"};
    static_assert(sizeof(op)/sizeof(*op)==nbmt::OP_COUNT,"one name per nbmt op");
    static const char *const own[]={"a draw recorded at once","submission end","nbmt sync","another pass","segment full","lost (new command buffer)"};
    return w<nbmt::OP_COUNT?op[w]:w<W_N?own[w-nbmt::OP_COUNT]:"?";}
// a queued draw: its commands' arguments
struct Pk{VkPipeline pipe{};VkPipelineLayout layout{};VkDescriptorSet set{};uint32_t off0=0,noff=0,vb0=0;uint16_t used=0;uint8_t nvb=0,indexed=0;
    VkBuffer ib{};VkDeviceSize ibo=0;VkIndexType it=VK_INDEX_TYPE_UINT16;VkViewport vp{};VkRect2D sc{};uint32_t count=0,inst=0,first=0,first_inst=0;int32_t vo=0;uint32_t slots=0;};
struct Vb{VkBuffer b;VkDeviceSize o;uint32_t slot;};
std::vector<Pk> q;std::vector<uint32_t> offs;std::vector<Vb> vbs;
VkCommandBuffer seg_cmd{};VkRenderPass seg_pass{};VkFramebuffer seg_fb{};uint64_t seg_no=0,fb_seen=0;
nrs_scratch S;uint32_t perm[NRS_CAP],perm2[NRS_CAP];uint64_t kp[NRS_CAP],kv[NRS_CAP],kb[NRS_CAP];nrs_bind B[NRS_CAP];uint8_t seen[NRS_CAP];
struct Stats{uint64_t asked=0,queued=0,why[NRS_W_N]{},segs=0,seg_draws=0,longest=0,by[W_N]{},ns=0,min_eff=0,
    checks=0,perm_bad=0,unstable=0,model_bad=0,ci_ok=0,ci_bad=0,ci_unknown=0;nrs_count rec{},em{},k0{},k1{};} s;
uint64_t next_ns=0,frames0=0,salt=0;unsigned told=0;
// DM_NR_SORT=2: what each pipeline was created with (nrs_pipe_ok without the colour masks, and the masks: 4 bits a target)
std::atomic<bool> reg_on{false};std::mutex pmu;struct Props{uint8_t depth_ok;uint32_t masks;};std::unordered_map<VkPipeline,Props> props;
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
void red(const char *fmt,...){if(told>=16)return;++told;char b[512];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof b,fmt,ap);va_end(ap);
    xbg_logf("【红】[nr] sort: %s (frame %llu)",b,(unsigned long long)stats.presents.load());}
void note_pipeline(VkPipeline p,const VkGraphicsPipelineCreateInfo &ci){
    if(!reg_on.load(std::memory_order_relaxed) || !p)return;
    const VkPipelineDepthStencilStateCreateInfo *ds=ci.pDepthStencilState;const VkPipelineColorBlendStateCreateInfo *cb=ci.pColorBlendState;const VkPipelineMultisampleStateCreateInfo *ms=ci.pMultisampleState;
    uint32_t masks=0;if(cb)for(uint32_t i=0;i<cb->attachmentCount && i<8;i++)masks|=(cb->pAttachments[i].colorWriteMask&15u)<<(4*i);
    const Props v{uint8_t(nrs_pipe_ok(ds!=nullptr,ds && ds->depthTestEnable,ds && ds->depthWriteEnable,ds?int(ds->depthCompareOp):0,ds && ds->stencilTestEnable,ds && ds->depthBoundsTestEnable,
        ms && ms->alphaToCoverageEnable,0)),masks};
    std::lock_guard<std::mutex> l(pmu);props[p]=v;}
void forget_pipeline(VkPipeline p){if(!reg_on.load(std::memory_order_relaxed))return;std::lock_guard<std::mutex> l(pmu);props.erase(p);}
void forget_pipelines(){if(!reg_on.load(std::memory_order_relaxed))return;std::lock_guard<std::mutex> l(pmu);props.clear();}
void init(){read_env=true;
    const char *e=getenv("DM_NR_SORT");int m=e && *e?atoi(e):xbg_nr_sort_default();
    if(m<0 || m>3){xbg_logf("【红】[nr] DM_NR_SORT=%d: only 0 / 1 / 2 / 3: off",m);m=0;}
    if(m && nrdraw::mode!=1){xbg_logf("【红】[nr] DM_NR_SORT=%d needs DM_NR_DRAW=1 (now %d): the queued draws are nr_draw's: off",m,nrdraw::mode);m=0;}
    e=getenv("DM_NR_SORT_KEY");key=e && *e?std::clamp(atoi(e),0,1):0;
    e=getenv("DM_NR_SORT_CAP");cap=e && *e?uint32_t(std::clamp(atoi(e),16,int(NRS_CAP))):2048u;
    if(m){q.reserve(cap);offs.reserve(size_t(cap)*8);vbs.reserve(size_t(cap)*2);}
    mode=m;reg_on.store(m==2,std::memory_order_relaxed);
    if(m)xbg_logf("[nr] DM_NR_SORT=%d (NR-21: %s): order-free draws of nr_draw (depth only: no colour / stencil / storage / query / bounds / alpha to coverage / feedback, fixed-point "
        "depth, writes only LESS / LEQUAL) wait in their segment and are recorded %s when the next other command comes; key %d (%s), at most %u draws a segment",
        m,m==1?"on":m==2?"compare - recorded order, the sort modelled and checked":"proof - reverse order",m==1?"sorted":m==2?"in the recorded order":"in reverse order",
        key,key?"pipeline, viewport + scissor, first vertex stream":"viewport + scissor, pipeline, first vertex stream",cap);}
void reset(){if(!read_env)init();}
void release(){pend.store(VK_NULL_HANDLE);q.clear();offs.clear();vbs.clear();mode=0;reg_on.store(false);std::lock_guard<std::mutex> l(pmu);props.clear();}
inline bool storage_stage(const Shader *sh){if(!sh)return false;if(!sh->storage_writes.empty())return true;
    for(const auto &x:sh->bindings)if(x.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || x.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)return true;return false;}
inline bool fixed_depth(VkFormat f){return f==VK_FORMAT_D16_UNORM || f==VK_FORMAT_X8_D24_UNORM_PACK32 || f==VK_FORMAT_D24_UNORM_S8_UINT || f==VK_FORMAT_D16_UNORM_S8_UINT;}
// the digests of the bind model (nr_sort.h nrs_bind): what the bind cache compares, never 0
inline uint64_t h_set(VkPipelineLayout l,VkDescriptorSet st,uint32_t no,const uint32_t *off){if(no>32)return nrs_mix(++salt,0x5e7)|1;   // the cache never keeps > 32 offsets
    uint64_t h=nrs_mix(nrs_mix(uint64_t(uintptr_t(l)),uint64_t(uintptr_t(st))),no);for(uint32_t i=0;i<no;i++)h=nrs_mix(h,off[i]);return h|1;}
inline uint64_t h_bytes(const void *p,size_t n){uint64_t h=0x51ed;const uint8_t *b=static_cast<const uint8_t*>(p);for(size_t i=0;i+8<=n;i+=8){uint64_t v;memcpy(&v,b+i,8);h=nrs_mix(h,v);}
    if(n&7){uint64_t v=0;memcpy(&v,b+(n&~size_t(7)),n&7);h=nrs_mix(h,v);}return h|1;}
inline uint64_t h_vb(VkBuffer b,VkDeviceSize o){return nrs_mix(uint64_t(uintptr_t(b)),o)|1;}
inline uint64_t h_ib(VkBuffer b,VkDeviceSize o,VkIndexType t){return nrs_mix(nrs_mix(uint64_t(uintptr_t(b)),o),uint64_t(t))|1;}
nrs_bstate bstate(const BindCache &bc){nrs_bstate st{};st.pipe=uint64_t(uintptr_t(bc.pipe));
    if(bc.layout && bc.noff!=UINT32_MAX)st.set=h_set(bc.layout,bc.set,bc.noff,bc.off);if(bc.has_vp)st.vp=h_bytes(&bc.vp,sizeof bc.vp);if(bc.has_sc)st.sc=h_bytes(&bc.sc,sizeof bc.sc);
    for(int i=0;i<16;i++)if(bc.vb[i])st.vb[i]=h_vb(bc.vb[i],bc.vbo[i]);if(bc.ib)st.ib=h_ib(bc.ib,bc.ibo,bc.it);return st;}
void flush(unsigned why,bool exact);
// lane(), after its bookkeeping, before its commands: true = the draw waits in the segment (lane() records nothing for it)
bool take(Commands &c,Descriptors &d,Pass &p,int count,const p_draw_idx *indexed,uint32_t instances,int first,uint32_t first_instance){
    ++s.asked;const nrdraw::Hand &H=nrdraw::H;
    nrs_in in{};
    in.ab_off=ab_off("nrsort");
    in.query=open_occlusion.cmd!=VK_NULL_HANDLE || state.count_samples;
    in.condr=condr::cg.mode>0 && condr::cg.kind!=CONDR_K_ON;
    in.feedback=depth_feedback_reads!=fb_seen;fb_seen=depth_feedback_reads;   // the counter moved since the last draw asked: a depth feedback read (or its 10 s reset)
    in.storage=!storage_images_bound.empty() || d.needs_writeback() || storage_stage(state.shader[0]) || storage_stage(state.shader[4]);
    in.depth_att=state.rt.depth!=nullptr;
    for(const Image *im:p.images)if(im->aspects&VK_IMAGE_ASPECT_DEPTH_BIT){in.depth_fixed=fixed_depth(im->fmt);break;}
    {const Shader *fs=state.shader[4];const bool noout=noout_mask_on && (!fs || (!fs->raster.outputs && !fs->raster.sample_mask));   // draw(): such targets are masked off
     uint32_t slots=0;for(int i=0;i<state.rt.n && i<8;i++)if(state.rt.color[i]){slots|=15u<<(4*i);if(!noout && ((state.mask.mask>>(4*i))&15u))in.color_written=1;}
     in.st=state.ds.st;in.bounds=state.bounds.enable!=0;in.a2c=state.ms.a2c!=0;in.dt=state.ds.dt;in.dw=state.ds.dw;in.op=state.ds.df>=1 && state.ds.df<=8?uint8_t(state.ds.df-1):uint8_t(255);
     const int w=nrs_why(&in);if(w){++s.why[w];return false;}
     if(!q.empty() && (seg_cmd!=c.command || seg_pass!=p.pass || seg_fb!=p.framebuffer || seg_no!=perf.passes))flush(W_PASS,false);   // (a pass end has recorded it already)
     if(q.size()>=cap)flush(W_CAP,true);
     if(q.empty()){seg_cmd=c.command;seg_pass=p.pass;seg_fb=p.framebuffer;seg_no=perf.passes;}
     Pk k{};k.slots=slots;
     k.pipe=H.pipe;k.layout=d.pipeline_layout;k.set=d.set;k.noff=uint32_t(d.dynamic_offsets.size());k.off0=uint32_t(offs.size());offs.insert(offs.end(),d.dynamic_offsets.begin(),d.dynamic_offsets.end());
     k.used=H.used;k.vb0=uint32_t(vbs.size());for(uint32_t m=H.used;m;m&=m-1){const uint32_t i=uint32_t(__builtin_ctz(m));vbs.push_back(Vb{H.vb[i],H.vbo[i],i});}k.nvb=uint8_t(vbs.size()-k.vb0);
     k.vp=H.vp;k.sc=H.sc;k.count=uint32_t(count);
     if(indexed){k.indexed=1;k.ib=H.ib;k.ibo=H.ibo;k.it=H.it;k.inst=uint32_t(indexed->instances);k.first=H.first_index;k.vo=int32_t(indexed->base_vertex)+int32_t(H.k);k.first_inst=uint32_t(indexed->base_instance);}
     else{k.inst=instances;k.first=uint32_t(first)+H.k;k.first_inst=first_instance;}
     if(nrs_effect(&in)==NRS_EFFECT_MIN)++s.min_eff;
     q.push_back(k);}
    pend.store(c.command,std::memory_order_relaxed);++s.queued;++pipe_memo_hits;++pipe_draw_fast_hits;
    return true;}
// One queued draw, through the bind cache exactly as lane() records a served draw (the same counters).
inline void emit(VkCommandBuffer cmd,BindCache &bc,const Pk &k,nrs_count &em){
    unsigned n=1;nrdraw::Stats &ns=nrdraw::s;
    if(bc.pipe!=k.pipe){vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,k.pipe);bc.pipe=k.pipe;++n;++ns.c_pipe;++em.pipe;}else ++binds_skipped;
    {const uint32_t no=k.noff;const uint32_t *off=no?&offs[k.off0]:nullptr;
     if(bc.layout!=k.layout || bc.set!=k.set || bc.noff!=no || no>32 || (no && memcmp(bc.off,off,no*4))){
        ++set_binds;set_bind_offsets+=no;
        if(bc.layout==k.layout && bc.set==k.set){++set_binds_offsets_only;
            if(bc.noff==no && no<=32){unsigned changed=0;for(uint32_t j=0;j<no;j++)if(bc.off[j]!=off[j]){++changed;++offset_changed_at[j];}++offset_changes[changed<9?changed:9];}}
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,k.layout,0,1,&k.set,no,off);++n;++ns.c_set;++em.set;
        if(no<=32){bc.layout=k.layout;bc.set=k.set;bc.noff=no;if(no)memcpy(bc.off,off,no*4);}else bc.noff=UINT32_MAX;}
     else ++binds_skipped;}
    if(!bc.has_vp || memcmp(&bc.vp,&k.vp,sizeof k.vp)){vkCmdSetViewport(cmd,0,1,&k.vp);bc.vp=k.vp;bc.has_vp=true;++n;++ns.c_vp;++em.vp;}else ++binds_skipped;
    if(!bc.has_sc || memcmp(&bc.sc,&k.sc,sizeof k.sc)){vkCmdSetScissor(cmd,0,1,&k.sc);bc.sc=k.sc;bc.has_sc=true;++n;++ns.c_sc;++em.sc;}else ++binds_skipped;
    {VkBuffer vb[16]{};VkDeviceSize vo[16]{};unsigned changed=0;
     for(uint32_t j=0;j<k.nvb;j++){const Vb &v=vbs[k.vb0+j];vb[v.slot]=v.b;vo[v.slot]=v.o;if(bc.vb[v.slot]!=v.b || bc.vbo[v.slot]!=v.o)changed|=1u<<v.slot;else ++binds_skipped;}
     if(changed){uint8_t f[8],c[8];const unsigned runs=nr_vb_runs(changed,k.used,f,c);
        for(unsigned r=0;r<runs;r++){for(unsigned j=0;j<c[r];j++){bc.vb[f[r]+j]=vb[f[r]+j];bc.vbo[f[r]+j]=vo[f[r]+j];}
            vkCmdBindVertexBuffers(cmd,f[r],c[r],&vb[f[r]],&vo[f[r]]);++n;++ns.c_vb;++em.vb;}}}
    if(k.indexed){
        if(bc.ib!=k.ib || bc.ibo!=k.ibo || bc.it!=k.it){vkCmdBindIndexBuffer(cmd,k.ib,k.ibo,k.it);bc.ib=k.ib;bc.ibo=k.ibo;bc.it=k.it;++n;++ns.c_ib;++em.ib;}else ++binds_skipped;
        vkCmdDrawIndexed(cmd,k.count,k.inst,k.first,k.vo,k.first_inst);}
    else vkCmdDraw(cmd,k.count,k.inst,k.first,k.first_inst);
    ns.cmds+=n;em.cmds+=n;++em.draws;}
inline void bind_of(const Pk &k,nrs_bind &b){b=nrs_bind{};b.pipe=uint64_t(uintptr_t(k.pipe));b.set=h_set(k.layout,k.set,k.noff,k.noff?&offs[k.off0]:nullptr);
    b.vp=h_bytes(&k.vp,sizeof k.vp);b.sc=h_bytes(&k.sc,sizeof k.sc);b.used=k.used;b.indexed=k.indexed;if(k.indexed)b.ib=h_ib(k.ib,k.ibo,k.it);
    for(uint32_t j=0;j<k.nvb;j++){const Vb &v=vbs[k.vb0+j];b.vb[v.slot]=h_vb(v.b,v.o);}}
inline void order_by(int which,uint32_t n,uint32_t *out){
    if(which)nrs_order(&S,n,NRS_ORDER_SORTED,kp,kv,kb,out);else nrs_order(&S,n,NRS_ORDER_SORTED,kv,kp,kb,out);}
// Record the segment (from a wrapper, the submission end, a draw recorded at once, take()). exact: the bind cache stays what the
// segment left (a draw path reads it next); else it is reset (whoever called may bind without the cache).
void flush(unsigned why,bool exact){
    if(__builtin_expect(nrz5t::held,0)){nrz5t::cut();return;}   // [NR-ZERO Z5] DM_NR_Z5=1: pend names the command buffer of a held chunk (DM_NR_SORT is 0 then): a draw path about to read the bind cache and record - the chunk goes in first
    const VkCommandBuffer c=pend.exchange(VK_NULL_HANDLE,std::memory_order_relaxed);if(!c)return;
    const uint32_t n=uint32_t(q.size());if(!n)return;
    const uint64_t t0=timing_clock();
    ++s.by[why<W_N?why:W_LOST];++s.segs;s.seg_draws+=n;if(n>s.longest)s.longest=n;
    BindCache &bc=bind_cache;if(bc.cmd!=c){bc=BindCache{};bc.cmd=c;}
    const nrs_bstate st0=bstate(bc);
    for(uint32_t i=0;i<n;i++){bind_of(q[i],B[i]);kp[i]=B[i].pipe;kv[i]=nrs_mix(B[i].vp,B[i].sc);const Pk &k=q[i];kb[i]=k.nvb?B[i].vb[vbs[k.vb0].slot]:0;}
    nrs_count rec{};nrs_count_order(&st0,B,nullptr,n,&rec);nrs_count_add(&s.rec,&rec);   // what the recorded order costs (= what DM_NR_SORT=0 records)
    if(mode==1)order_by(key,n,perm);
    else nrs_order(&S,n,mode==3?NRS_ORDER_REVERSED:NRS_ORDER_RECORDED,nullptr,nullptr,nullptr,perm);
    if(mode==2){++s.checks;
        for(int w=0;w<2;w++){order_by(w,n,perm2);nrs_count m{};nrs_count_order(&st0,B,perm2,n,&m);nrs_count_add(w?&s.k1:&s.k0,&m);
            if(!nrs_perm_ok(perm2,n,seen)){++s.perm_bad;red("key %d: the order of a %u-draw segment is not a permutation of it",w,n);}
            else if(!nrs_stable_ok(&S,perm2,n,3)){++s.unstable;red("key %d: the order of a %u-draw segment does not keep the recorded order among equal keys",w,n);}}
        std::lock_guard<std::mutex> l(pmu);
        for(uint32_t i=0;i<n;i++){auto it=props.find(q[i].pipe);if(it==props.end()){++s.ci_unknown;continue;}
            if(it->second.depth_ok && !(it->second.masks&q[i].slots))++s.ci_ok;
            else{++s.ci_bad;red("queued draw %u of %u: pipeline %p was created %s (colour masks %#x on targets %#x)",i,n,(void*)q[i].pipe,
                it->second.depth_ok?"with a colour channel written":"with stencil / depth bounds / alpha to coverage / a depth write that is not LESS / LEQUAL / NEVER",it->second.masks,q[i].slots);}}}
    nrs_count em{};
    for(uint32_t j=0;j<n;j++)emit(c,bc,q[perm[j]],em);
    nrs_count_add(&s.em,&em);
    if(mode==2 && (em.pipe!=rec.pipe || em.set!=rec.set || em.vp!=rec.vp || em.sc!=rec.sc || em.vb!=rec.vb || em.ib!=rec.ib || em.cmds!=rec.cmds)){++s.model_bad;
        red("the bind model of a %u-draw segment is not what was recorded: pipeline %llu / %llu, set %llu / %llu, vertex %llu / %llu, index %llu / %llu, vkCmd* %llu / %llu",n,
            (unsigned long long)rec.pipe,(unsigned long long)em.pipe,(unsigned long long)rec.set,(unsigned long long)em.set,(unsigned long long)rec.vb,(unsigned long long)em.vb,
            (unsigned long long)rec.ib,(unsigned long long)em.ib,(unsigned long long)rec.cmds,(unsigned long long)em.cmds);}
    if(!exact){bc=BindCache{};bc.cmd=c;}
    q.clear();offs.clear();vbs.clear();
    s.ns+=timing_clock()-t0;}
void flush_draw(){flush(W_DRAW,true);}
// NBMT_PRE: a vkCmd* wrapper (or nbmt::end / sync) on the command buffer with queued draws.
// The bind cache stays exact unless the command binds / draws / dispatches (that code may bind without telling the cache: the masked
// clear resets the cache and binds its own pipeline). A segment still open at a BeginRenderPass would be a missed pass end: its draws
// would land outside a render pass, so they are dropped instead (red; never expected - every pass end goes through EndRenderPass first).
void cut(VkCommandBuffer c,unsigned op){if(pend.load(std::memory_order_relaxed)!=c)return;
    if(__builtin_expect(nrz5t::held,0)){nrz5t::cut();return;}   // [NR-ZERO Z5] DM_NR_Z5=1: the chunk the game thread wrote is held for this command buffer - it goes in first (DM_NR_SORT is 0 then)
    if(op==nbmt::OP_BeginRenderPass){red("a segment of %zu draws was still open when a render pass began (its end did not record it): dropped",q.size());
        ++s.by[W_LOST];pend.store(VK_NULL_HANDLE);q.clear();offs.clear();vbs.clear();return;}
    const bool binds=op==nbmt::OP_BindPipeline || op==nbmt::OP_BindDescriptorSets || op==nbmt::OP_BindVertexBuffers || op==nbmt::OP_BindIndexBuffer || op==nbmt::OP_SetViewport ||
        op==nbmt::OP_SetScissor || op==nbmt::OP_PushConstants || (op>=nbmt::OP_Draw && op<=nbmt::OP_DispatchIndirect);
    flush(op,!binds);}
// recording_slot() began a command buffer: a queue left over would belong to a finished one (never expected; counted, dropped).
void cb_begun(){if(__builtin_expect(nrz5t::held,0))nrz5t::lost();if(!pend.load(std::memory_order_relaxed) && q.empty())return;
    red("%zu queued draws left when a new command buffer began: dropped",q.size());++s.by[W_LOST];pend.store(VK_NULL_HANDLE);q.clear();offs.clear();vbs.clear();}
void tick(){   // from nrdraw::tick (every submission)
    if(!mode)return;const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;frames0=stats.presents.load();return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const uint64_t fr=stats.presents.load();const double f=fr>frames0?double(fr-frames0):1.;frames0=fr;
    std::string w;for(int i=1;i<NRS_W_N;i++)if(s.why[i]){char b[80];snprintf(b,sizeof b,"%s%s %.1f",w.empty()?"":", ",nrs_why_name(i),double(s.why[i])/f);w+=b;}
    std::string by;for(unsigned i=0;i<W_N;i++)if(s.by[i]){char b[80];snprintf(b,sizeof b,"%s%s %.1f",by.empty()?"":", ",by_name(i),double(s.by[i])/f);by+=b;}
    const nrs_count &o=mode==2?(key?s.k1:s.k0):s.em;
    xbg_logf("[nr] sort: DM_NR_SORT=%d key %d; a frame: %.1f draws asked, %.1f queued (%.1f writing depth) in %.1f segments (%.1f draws a segment, longest %llu); not queued: %s; "
        "segments ended by: %s; the queued draws, recorded order -> %s: pipeline binds %.1f -> %.1f, sets %.1f -> %.1f, viewport %.1f -> %.1f, scissor %.1f -> %.1f, vertex %.1f -> %.1f, "
        "index %.1f -> %.1f, vkCmd* %.1f -> %.1f (saved %.1f a frame); %.1f us a frame to sort and record them",
        mode,key,double(s.asked)/f,double(s.queued)/f,double(s.min_eff)/f,double(s.segs)/f,s.segs?double(s.seg_draws)/double(s.segs):0.,(unsigned long long)s.longest,w.empty()?"none":w.c_str(),
        by.empty()?"none":by.c_str(),mode==1?"sorted":mode==2?"sorted (modelled; recorded as recorded)":"reversed",
        double(s.rec.pipe)/f,double(o.pipe)/f,double(s.rec.set)/f,double(o.set)/f,double(s.rec.vp)/f,double(o.vp)/f,double(s.rec.sc)/f,double(o.sc)/f,double(s.rec.vb)/f,double(o.vb)/f,
        double(s.rec.ib)/f,double(o.ib)/f,double(s.rec.cmds)/f,double(o.cmds)/f,(double(s.rec.cmds)-double(o.cmds))/f,double(s.ns)/1000./f);
    if(mode==2)
        xbg_logf("[nr] sort check: %llu segments; orders: not a permutation %llu, unstable %llu; the bind model of the recorded order against what was recorded: bad %llu; "
            "queued draws against their pipeline's create info: order-free %llu, NOT %llu, not registered %llu; key 0 -> pipeline %.1f vkCmd* %.1f, key 1 -> pipeline %.1f vkCmd* %.1f a frame",
            (unsigned long long)s.checks,(unsigned long long)s.perm_bad,(unsigned long long)s.unstable,(unsigned long long)s.model_bad,(unsigned long long)s.ci_ok,
            (unsigned long long)s.ci_bad,(unsigned long long)s.ci_unknown,double(s.k0.pipe)/f,double(s.k0.cmds)/f,double(s.k1.pipe)/f,double(s.k1.cmds)/f);
    s=Stats{};}
}
