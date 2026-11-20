// [DOS2] NR-08 (docs/dos2/nr/NR-08.md, NATIVE_RENDERER_PLAN.md S4): DM_NR_DESC - the descriptor set of an engine draw without the memo
// key, the per-block snapshots and the per-image read path. Included by xbg_game.cpp at file scope right before descriptors(); decisions
// and tables in dm/gfx/nr/nr_desc.h (pure C, host test tests/nr_desc_test.c). Needs the constant ring in ONE buffer: DM_NR_BUF=1 / 3
// (NR-06, which needs DM_NR=1 / 2). Read at the first draw.
//
//   DM_NR_DESC   0 (default) nothing here runs: descriptors() pays one load and a not-taken branch (nrdesc::Scope), descriptors_build
//                  and dyn_mode_for one each (cap_on / split_on stay false).
//                1 on. A graphics draw whose shader set reads only uniform blocks of the game, the support block, c1 constants and
//                  textures by slot (nr_dplan_build; anything else - storage, images, driver blocks, arrays, texel buffers - stays on
//                  the old path for good, per shader set) is served from an ENTRY:
//                    texture part  found by generation (same shader set, the backend's tex_gen of its stages, pools and viewport as at
//                                  its last draw: no key at all) or by a hash of the texture / sampler handles of the slots it reads;
//                                  made once by a capture of the old build (descriptors_build runs to its end and hands its writes
//                                  over instead of allocating a set), written once into a set of nrdesc's own pool;
//                    uniform part  each block = its offset in the ring buffer (nrbuf::ubo): a dynamic offset, or - when the layout
//                                  could not make it dynamic - one word of the static tuple that picks a set variant (rewritten
//                                  only through the recycler nr_dfifo: no submission that could read the set is alive);
//                    hazards       get_image + prepare_read only for images the GPU ever wrote and for the first use of an image in
//                                  a submission (nr_dimg_skip); a draw of an entry never touches the others.
//                  A draw that cannot be served now (a block outside the ring, held by the mirror, another bound size, no ring yet,
//                  a gate of the memo) takes the old path, counted by reason.
//                2 compute + execute old + compare: descriptors() runs as before and its result is used. The entry is looked up both
//                  ways (generation and hash must agree), its offsets, layout and static tuple are compared with the old result, "no
//                  image needs the read path" is checked against what the old path did (pass ended / barrier recorded), and one draw
//                  in DM_NR_DESC_CMP (8) is built once more as a capture and every write (view, sampler, buffer, offset, range)
//                  compared with the entry. Mismatches: red lines (first 16) + the 10 s line.
//                3 does not exist: a set is written whole when it is made, nothing can be left out to poison (red line, off).
//   DM_NR_DESC_SPLIT  1 (default with DM_NR_DESC=1): a shader set with more uniform blocks than maxDescriptorSetUniformBuffersDynamic
//                  (Mali / PowerVR: 8; 2.4 % of DOS2's pipelines, NR-02) gets a layout of its own - dyn_mode_for mode 3: the blocks
//                  whose address changed most so far stay dynamic, the others are static - instead of the all-static layout the old
//                  path gave it (no memo: a full build per draw). 0: the old layout (every block a static tuple word).
//   DM_NR_DESC_MAX    entries (4096; 256..65536). A full table is flushed.
//   Lines "[nr] desc: ..." every 10 s, "[nr] DM_NR_DESC=..." at the start; red lines "【红】[nr] desc: ...".
#include "../../nr/nr_desc.h"
#include "../../nr/nr_z3.h"   // [NR-ZERO Z3] NRZD_*: how serve() answered the draw in hand (nrz3 reads it)
static_assert(sizeof(nr_dbind)==sizeof(xbg_shader_binding) && offsetof(nr_dbind,sampler_slot)==offsetof(xbg_shader_binding,sampler_slot),"nr_dbind = xbg_shader_binding");
static_assert(int(NRD_T_COMBINED)==int(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) && int(NRD_T_SAMPLED)==int(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) && int(NRD_T_UBO)==int(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
    && int(NRD_S_SUPPORT)==int(XBG_SOURCE_SUPPORT) && int(NRD_S_UNIFORM)==int(XBG_SOURCE_UNIFORM) && int(NRD_S_C1)==int(XBG_SOURCE_C1) && int(NRD_S_DRIVER0)==int(XBG_SOURCE_DRIVER0)
    && int(NRD_S_DRIVER2)==int(XBG_SOURCE_DRIVER2) && int(NRD_S_TEXTURE)==int(XBG_SOURCE_TEXTURE),"nr_desc.h constants");
namespace nrdesc {
int mode=-1,cmp_n=8;bool cap_on=false,split_on=false;
typedef std::array<const void*,5> ShKey;
struct ShHash{size_t operator()(const ShKey &k)const{uint64_t h=0x9E3779B97F4A7C15ull;for(auto p:k){h^=uint64_t(uintptr_t(p));h*=0xff51afd7ed558ccdull;h^=h>>32;}return size_t(h);}};
inline ShKey shkey(){ShKey k;for(unsigned i=0;i<5;i++)k[i]=state.shader[i];return k;}
// ---- the capture of one descriptors_build ----
struct CapRow{uint32_t binding,type;DescEntry d;uint8_t kind;};   // kind: 0 buffer, 1 image
std::vector<CapRow> cap;bool cap_ok=false;
// descriptors_build, after its element loop and the dynamic offsets (cap_on only): the writes instead of a set
void captured(const std::vector<VkWriteDescriptorSet> &w){cap.clear();cap_ok=true;
    for(const auto &x:w){CapRow r{};r.binding=x.dstBinding;r.type=uint32_t(x.descriptorType);
        if(x.descriptorCount!=1 || x.dstArrayElement!=0)cap_ok=false;
        if(x.pBufferInfo){r.d.buffer=*x.pBufferInfo;r.kind=0;}else if(x.pImageInfo){r.d.image=*x.pImageInfo;r.kind=1;}else cap_ok=false;
        cap.push_back(r);}}
// ---- plans (one per shader set), entries, sets ----
struct Fifo{nr_dfifo f;uint32_t orphans=0;Fifo(){nr_dfifo_init(&f);}~Fifo(){nr_dfifo_free(&f);}};   // orphans: nodes at the head no entry holds (after a flush)
// (field order, [NR-ZERO Z3K]: what every served draw reads and writes - the layout, the back-off, the counters, the plan's verdict and its
// counts - shares the first cache line; the rows follow)
struct alignas(64) Plan{const LayoutEntry *layout=nullptr;uint64_t retry=0,served=0;Fifo *fifo=nullptr;int32_t last=-1;unsigned fails=0,misses=0;bool tmpl_tried=false,copy_known=false;
    nr_dplan p{};VkDescriptorUpdateTemplate tmpl{};uint32_t copy[NRD_MAX_UBO]{};
    size_t reserve=0;uint64_t last_tg[6]{},last_fixed[6]{};uint32_t types[NRD_MAX_ROWS]{};};
static_assert(offsetof(Plan,p)+offsetof(nr_dplan,ubo_row)<=64,"nrdesc::Plan: the words of a served draw in one line");
struct Var{int32_t node=-1;uint32_t gen=0;VkDescriptorSet set{};uint32_t tuple[NRD_MAX_UBO]{};};
// var: the entry's set(s), each a node of its layout's recycler - the one of the texture part, or (static tuple words) the two last tuples
// (field order, [NR-ZERO Z3K]: the words a hit checks first, then the offsets and the sets, the vectors of the image walk last)
struct alignas(64) Ent{Plan *plan=nullptr;uint64_t check_gen=0;VkBuffer ring{};
    nrz_rtq rtq{};       // [NR-ZERO Z8 images] the walk of img would do nothing, also for images the GPU wrote, under these words (nr_z3e.h; xbg_nr_z8.inc img_ask / img_done)
    nrz_quiet quiet{};   // [NR-ZERO Z3b] the walk of img found every image quiet under these words (nr_z3e.h); a remade entry starts empty
    uint8_t mru=0;bool live=false;uint32_t off[NRD_MAX_DYN]{};Var var[2];
    std::vector<MemoImage> img;std::vector<std::pair<uint64_t,const void*>> ids;std::vector<DescEntry> data;};
std::unordered_map<ShKey,std::unique_ptr<Plan>,ShHash> plans;Plan *front=nullptr;ShKey front_key{};
std::unordered_map<const LayoutEntry*,std::unique_ptr<Fifo>> fifos;
nr_dtab tab{};bool tab_ok=false;std::vector<Ent> ents;uint32_t max_ents=4096;
std::vector<VkDescriptorPool> pools;uint64_t sets_made=0;
VkBuffer ring_now{};uint64_t ring_epoch=~0ull;   // nrbuf::ring_vk() of this submission
uint64_t my_epoch=0;   // memo_epoch the entries were made at (views, samplers, the ring mirror)
// [NR-ZERO Z3] tab_gen: the generation of the entry ids - +1 at every flush (ids are handed out again) and at release (the plans go);
// nrz3 stores (plan, entry id) of a resolved draw under it. z_out / z_rd: how serve() answered the last draw (NRZD_*; images that took
// the read path on a hit) - written on every exit of serve(), read only by DM_NR_Z3.
uint64_t tab_gen=1;int z_out=NRZD_NONE;uint32_t z_rd=0;
uint32_t ubo_chg[6][16]{};uint64_t ubo_last[6][16]{};   // how often a block's address moved (DM_NR_DESC_SPLIT picks by it)
// ---- counters (the 10 s window) ----
enum{F_GATE,F_NE,F_NORING,F_UNBOUND,F_SIZE,F_RINGMISS,F_CAPTURE,F_LAYOUT,F_SET,F_BACKOFF,F_N};
struct Stats{uint64_t draws=0,served=0,by_gen=0,by_hash=0,learned=0,relearn=0,fall[F_N]{},ne[NRD_NE_N]{},img=0,img_skip=0,img_why[NRD_I_N]{},ubos=0,
    var_hit=0,var_write=0,var_new=0,flushes=0,checked=0,built=0,bad=0,bad_by[8]{},timed=0,ns=0;
    uint64_t sp_n=0,sp_hash=0,sp_find=0,sp_blocks=0,sp_img=0,sp_set=0,sp_nblk=0,sp_nimg=0,sp_rd=0;} s;   // [NR-ZERO] a served draw by part (1 in 128; its time is not in `ns`)
enum{B_GEN,B_LAYOUT,B_OFFSET,B_HAZARD,B_IMAGE,B_BUFFER,B_ROWS,B_TUPLE};
uint64_t next_ns=0,calls=0;unsigned told=0;
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
void red(const char *fmt,...){if(told>=16)return;++told;char b[640];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof b,fmt,ap);va_end(ap);
    xbg_logf("【红】[nr] desc: %s (vs %s fs %s, frame %llu)",b,state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-",(unsigned long long)stats.presents.load());}
inline void bad(int why){++s.bad;++s.bad_by[why];}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns || !s.draws)return;next_ns=now+10000000000ull;
    size_t nplan=0,nel=0;for(auto &p:plans){++nplan;if(!p.second->p.why)++nel;}
    uint64_t taken=0,fresh=0;for(auto &f:fifos){taken+=f.second->f.taken;fresh+=f.second->f.fresh;}
    xbg_logf("[nr] desc: DM_NR_DESC=%d; %llu graphics draws, %s %llu (entry by generation %llu, by hash %llu, made %llu, remade %llu); old path: gate %llu, shader set not eligible %llu "
        "(type %llu, source %llu, driver %llu, array %llu, slot %llu, rows %llu, device limit %llu), no ring buffer %llu, block unbound %llu, block size %llu, block not served by the ring %llu, capture %llu, "
        "layout %llu, no set %llu, backing off %llu; images %llu: skipped %llu, read path: first in submission %llu, GPU-written %llu, layout %llu, hook %llu; %.1f ring blocks a draw; "
        "sets: reused %llu, written %llu (new %llu); %zu shader sets (%zu eligible), %u entries, %llu sets made / %llu recycled, flushes %llu; "
        "checks %llu (%llu rebuilt and compared), bad %llu (generation %llu, layout %llu, offsets %llu, hazard %llu, image %llu, buffer %llu, rows %llu, tuple %llu); %.0f ns a served draw (1 in 64)",
        mode,(unsigned long long)s.draws,mode==2?"would serve":"served",(unsigned long long)s.served,(unsigned long long)s.by_gen,(unsigned long long)s.by_hash,(unsigned long long)s.learned,(unsigned long long)s.relearn,
        (unsigned long long)s.fall[F_GATE],(unsigned long long)s.fall[F_NE],(unsigned long long)s.ne[NRD_NE_TYPE],(unsigned long long)s.ne[NRD_NE_SOURCE],(unsigned long long)s.ne[NRD_NE_DRIVER],
        (unsigned long long)s.ne[NRD_NE_ARRAY],(unsigned long long)s.ne[NRD_NE_SLOT],(unsigned long long)s.ne[NRD_NE_ROWS],(unsigned long long)s.ne[NRD_NE_LIMIT],(unsigned long long)s.fall[F_NORING],(unsigned long long)s.fall[F_UNBOUND],
        (unsigned long long)s.fall[F_SIZE],(unsigned long long)s.fall[F_RINGMISS],(unsigned long long)s.fall[F_CAPTURE],(unsigned long long)s.fall[F_LAYOUT],(unsigned long long)s.fall[F_SET],(unsigned long long)s.fall[F_BACKOFF],
        (unsigned long long)s.img,(unsigned long long)s.img_skip,(unsigned long long)s.img_why[NRD_I_FIRST],(unsigned long long)s.img_why[NRD_I_RT],(unsigned long long)s.img_why[NRD_I_LAYOUT],
        (unsigned long long)s.img_why[NRD_I_HOOK],s.served?double(s.ubos)/double(s.served):0.,(unsigned long long)s.var_hit,(unsigned long long)s.var_write,(unsigned long long)s.var_new,
        nplan,nel,tab.n,(unsigned long long)sets_made,(unsigned long long)taken,(unsigned long long)s.flushes,(unsigned long long)s.checked,(unsigned long long)s.built,(unsigned long long)s.bad,
        (unsigned long long)s.bad_by[B_GEN],(unsigned long long)s.bad_by[B_LAYOUT],(unsigned long long)s.bad_by[B_OFFSET],(unsigned long long)s.bad_by[B_HAZARD],(unsigned long long)s.bad_by[B_IMAGE],
        (unsigned long long)s.bad_by[B_BUFFER],(unsigned long long)s.bad_by[B_ROWS],(unsigned long long)s.bad_by[B_TUPLE],s.timed?double(s.ns)/double(s.timed):0.);
    if(s.sp_n){const double n=double(s.sp_n);   // [NR-ZERO] where the microsecond of a served draw goes (clock reads of the split are inside the parts)
        xbg_logf("[nr] desc by part (NR-ZERO, %llu served draws split, 1 in 128): %.0f ns = entry lookup %.0f (by hash %.0f %%) + uniform blocks %.0f (%.1f a draw) + images %.0f (%.1f a draw, read path %.2f) + set and offsets %.0f",
            (unsigned long long)s.sp_n,(s.sp_find+s.sp_blocks+s.sp_img+s.sp_set)/n,s.sp_find/n,100.*double(s.sp_hash)/n,s.sp_blocks/n,s.sp_nblk/n,s.sp_img/n,s.sp_nimg/n,s.sp_rd/n,s.sp_set/n);}
    (void)fresh;s=Stats{};}
void init(){
    if(nrbuf::mode<0)return;   // NR-06 reads its switch at the first guest submission: ask again at the next draw
    const char *e=getenv("DM_NR_DESC");int m=e && *e?atoi(e):0;
    if(m==3){xbg_logf("【红】[nr] DM_NR_DESC=3 does not exist (a set is written whole, nothing to poison): off");m=0;}
    if(m<0 || m>2)m=0;
    if(m && nrbuf::mode!=1 && nrbuf::mode!=3){xbg_logf("【红】[nr] DM_NR_DESC=%d needs the constant ring in one buffer (DM_NR=1 DM_NR_BUF=1; DM_NR_BUF is %d): off",m,nrbuf::mode);m=0;}
    if(m){e=getenv("DM_NR_DESC_MAX");if(e && *e)max_ents=uint32_t(std::clamp(atoi(e),256,65536));
        e=getenv("DM_NR_DESC_CMP");if(e && *e)cmp_n=std::clamp(atoi(e),1,1000000);
        tab_ok=!nr_dtab_init(&tab,max_ents);if(!tab_ok){xbg_logf("【红】[nr] DM_NR_DESC=%d: no memory for %u entries: off",m,max_ents);m=0;}}
    if(m){ents.resize(max_ents);e=getenv("DM_NR_DESC_SPLIT");split_on=m==1 && ubo_dynamic_on && (e && *e?atoi(e)!=0:xbg_nr_desc_split_default(ubo_dynamic_on));
        const unsigned lim=properties.limits.maxDescriptorSetUniformBuffersDynamic;
        xbg_logf("[nr] DM_NR_DESC=%d (NR-08: %s): entries by texture generation / handle hash (max %u), uniform blocks as offsets into the constant ring buffer (%s; %s), read hazards only "
            "for GPU-written images; device: %u dynamic uniform buffers a set, dynamic uniform buffers %s -> %s; DM_NR_DESC_SPLIT=%d",m,
            m==1?"on":"old path used, entries built and compared",max_ents,nrbuf::ring_mode==nrbuf::RING_MIRROR?"mirror":"in place",nrbuf::mode==3?"poisoned":"DM_NR_BUF=1",lim,
            ubo_dynamic_on?"on":"off (policy)",!ubo_dynamic_on?"every block a static tuple word: one set rewrite a draw whose blocks moved":lim>=16?"every block a dynamic offset":
            "a shader set with more blocks than that splits them (most-changing dynamic, the rest static tuple words)",int(split_on));}
    mode=m;}
// ---- sets of nrdesc's own pools (never freed one by one: recycled through the fifo of their layout) ----
VkDescriptorSet new_set(VkDescriptorSetLayout layout){
    for(int tries=0;tries<2;tries++){
        if(pools.empty() || tries){
            const VkDescriptorPoolSize sizes[4]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8192},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1024},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,8192},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,8192}};
            VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};ci.maxSets=1024;ci.poolSizeCount=4;ci.pPoolSizes=sizes;
            VkDescriptorPool pool{};if(vkCreateDescriptorPool(cfg.device,&ci,nullptr,&pool)!=VK_SUCCESS)return VK_NULL_HANDLE;pools.push_back(pool);}
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pools.back();ai.descriptorSetCount=1;ai.pSetLayouts=&layout;
        VkDescriptorSet set{};if(vkAllocateDescriptorSets(cfg.device,&ai,&set)==VK_SUCCESS){++sets_made;return set;}}
    return VK_NULL_HANDLE;}
Fifo *fifo_of(const LayoutEntry *l){auto &f=fifos[l];if(!f)f=std::make_unique<Fifo>();return f.get();}
// A set whose contents may be written now: a recycled one, or a new one. *node / *gen: its recycler node (-1: no memory for the node).
VkDescriptorSet take_set(Plan &pl,int32_t *node,uint32_t *gen){
    recording_slot();   // the slot that ran serial - 2 has retired (the recycler's rule)
    // a layout without static tuple words writes a set once: only the sets of long-idle entries (or of flushed ones) are taken
    nr_dfifo &f=pl.fifo->f;int32_t n=nr_dfifo_take(&f,uint32_t(slot_serial),pl.p.nstat || pl.fifo->orphans?NRD_AGE_SAFE:NRD_AGE_IDLE);
    if(n>=0 && pl.fifo->orphans)--pl.fifo->orphans;
    if(n<0){VkDescriptorSet set=new_set(pl.layout->layout);if(!set)return VK_NULL_HANDLE;n=nr_dfifo_push(&f,uint64_t(set),uint32_t(slot_serial));
        if(n<0){*node=-1;*gen=0;return set;}++s.var_new;}
    *node=n;*gen=f.node[n].gen;return VkDescriptorSet(f.node[n].set);}
void write_set(Plan &pl,VkDescriptorSet set,const DescEntry *data){
    const nr_dplan &p=pl.p;
    if(!pl.tmpl && !pl.tmpl_tried){pl.tmpl_tried=true;VkDescriptorUpdateTemplateEntry en[NRD_MAX_ROWS];
        for(unsigned i=0;i<p.nrow;i++)en[i]={p.row[i].binding,0,1,VkDescriptorType(pl.types[i]),i*sizeof(DescEntry),sizeof(DescEntry)};
        VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};ti.descriptorUpdateEntryCount=p.nrow;ti.pDescriptorUpdateEntries=en;
        ti.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;ti.descriptorSetLayout=pl.layout->layout;
        if(!template_enabled || vkCreateDescriptorUpdateTemplate(cfg.device,&ti,nullptr,&pl.tmpl)!=VK_SUCCESS)pl.tmpl=VK_NULL_HANDLE;}
    if(!p.nrow)return;
    if(pl.tmpl){vkUpdateDescriptorSetWithTemplate(cfg.device,set,pl.tmpl,data);return;}
    VkWriteDescriptorSet w[NRD_MAX_ROWS];
    for(unsigned i=0;i<p.nrow;i++){w[i]=VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w[i].dstSet=set;w[i].dstBinding=p.row[i].binding;w[i].descriptorCount=1;w[i].descriptorType=VkDescriptorType(pl.types[i]);
        if(p.row[i].kind==NRD_K_IMAGE)w[i].pImageInfo=&data[i].image;else w[i].pBufferInfo=&data[i].buffer;}
    vkUpdateDescriptorSets(cfg.device,p.nrow,w,0,nullptr);}
inline bool my_epoch_moved(){const uint64_t e=memo_epoch;if(e==my_epoch)return false;my_epoch=e;return tab.n!=0;}
inline void done(Descriptors &out,Plan &pl){if(out.buffers.capacity()<pl.reserve)out.buffers.reserve(pl.reserve);++pl.served;pl.fails=pl.misses=0;}
// Every entry is gone (the views / samplers / buffers its sets hold are, or the table is full).
void flush(const char *why){
    for(uint32_t i=0;i<tab.n;i++)ents[i]=Ent{};   // their sets stay nodes of the recyclers: rewritten once no submission can read them
    nr_dtab_clear(&tab);++tab_gen;for(auto &p:plans)p.second->last=-1;for(auto &q:fifos)q.second->orphans=q.second->f.n;++s.flushes;
    static unsigned n=0;if(n++<8)xbg_logf("[nr] desc: entries flushed (%s)",why);}
// Shutdown (before clear_descriptor_caches: the layouts and the static arena go there).
void release(){
    for(auto &p:plans)if(p.second->tmpl)vkDestroyDescriptorUpdateTemplate(cfg.device,p.second->tmpl,nullptr);
    plans.clear();front=nullptr;fifos.clear();ents.clear();++tab_gen;if(tab_ok){nr_dtab_free(&tab);tab_ok=false;}
    for(auto pool:pools)vkDestroyDescriptorPool(cfg.device,pool,nullptr);pools.clear();if(mode>0)mode=0;split_on=false;}
// ---- DM_NR_DESC_SPLIT: dyn_mode_for's mode 3 (declared before it) ----
struct Split{std::vector<const xbg_shader_binding*> dyn;bool on=false;};
std::unordered_map<ShKey,Split,ShHash> splits;const Split *split_cur=nullptr;ShKey split_key{};
bool split_has(const xbg_shader_binding *b){if(split_cur)for(auto *x:split_cur->dyn)if(x==b)return true;return false;}
// The graphics shader set of the state has more per-draw uniform buffers than `lim`: true = it takes mode 3 (split_has names its dynamic blocks).
bool split_pick(unsigned lim){
    const ShKey k=shkey();
    if(!split_cur || k!=split_key){auto it=splits.find(k);
        if(it==splits.end()){Split sp;const xbg_shader_binding *b[NRD_MAX_UBO];uint32_t chg[NRD_MAX_UBO];unsigned n=0;bool ok=lim>0;
            for(unsigned st=0;st<5 && ok;st++)if(state.shader[st]){const unsigned ns=nvn_stage[st];for(const auto &x:state.shader[st]->bindings){
                if(x.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)continue;
                if(x.source==XBG_SOURCE_DRIVER0 || x.source==XBG_SOURCE_DRIVER2){ok=false;break;}   // the old path keys their static blocks; not an NR shader set anyway
                if(x.source!=XBG_SOURCE_UNIFORM)continue;
                if(x.count!=1 || x.slot>=16 || n>=NRD_MAX_UBO){ok=false;break;}
                b[n]=&x;chg[n]=ubo_chg[ns][x.slot];++n;}}
            if(ok && n>lim){uint8_t dyn[NRD_MAX_UBO];const unsigned nstat=nr_dsplit(chg,n,lim,dyn);for(unsigned i=0;i<n;i++)if(dyn[i])sp.dyn.push_back(b[i]);sp.on=true;
                static unsigned told_split=0;if(told_split++<8)xbg_logf("[nr] desc: shader set vs %s fs %s has %u uniform blocks, the device allows %u dynamic: %u stay dynamic (the most-changing), %u are static tuple words",
                    state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-",n,lim,n-nstat,nstat);}
            it=splits.emplace(k,std::move(sp)).first;}
        split_cur=&it->second;split_key=k;}
    return split_cur->on;}
// ---- the plan of the state's shader set ----
Plan *plan_for(){
    const ShKey k=shkey();if(front && k==front_key)return front;
    auto it=plans.find(k);
    if(it==plans.end()){auto pl=std::make_unique<Plan>();
        const unsigned dmode=dyn_mode_for(false);
        nr_dstage st[NRD_STAGES]{};std::vector<uint8_t> dyn[NRD_STAGES];size_t elements=0;
        for(unsigned i=0;i<5;i++)if(const Shader *sh=state.shader[i]){st[i].present=1;st[i].ns=nvn_stage[i];st[i].n=uint32_t(sh->bindings.size());
            st[i].b=reinterpret_cast<const nr_dbind*>(sh->bindings.data());dyn[i].resize(sh->bindings.size());
            for(size_t j=0;j<sh->bindings.size();j++){const auto &b=sh->bindings[j];dyn[i][j]=UBO_DYN(dmode,b)?1:0;elements+=b.count;}
            st[i].dyn=dyn[i].data();}
        nr_dplan_build(&pl->p,st);pl->reserve=elements*2+40;
        {const auto &l=properties.limits;   // a layout over a per-stage limit of the device was the old path's to make (or to fail): not served here
         if(nr_dplan_limits(&pl->p,std::min(l.maxPerStageDescriptorSamplers,l.maxPerStageDescriptorSampledImages),l.maxPerStageDescriptorUniformBuffers,l.maxDescriptorSetUniformBuffersDynamic)==NRD_NE_LIMIT){
            static unsigned told_lim=0;if(told_lim++<8)xbg_logf("[nr] desc: shader set vs %s fs %s is over a descriptor limit of the device (stage textures %u, stage uniform buffers %u, dynamic %u): old path",
                state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-",std::min(l.maxPerStageDescriptorSamplers,l.maxPerStageDescriptorSampledImages),l.maxPerStageDescriptorUniformBuffers,l.maxDescriptorSetUniformBuffersDynamic);}}
        it=plans.emplace(k,std::move(pl)).first;}
    front=it->second.get();front_key=k;return front;}
inline void fixed_words(uint64_t f[6]){f[0]=(uint64_t(uint32_t(state.viewport.width))<<32)|uint32_t(state.viewport.height);
    f[1]=state.texture_pool.pool?uint64_t(uintptr_t(state.texture_pool.pool->mem)):0;f[2]=uint64_t(state.texture_pool.off);f[3]=uint64_t(state.texture_pool.n);
    f[4]=state.sampler_pool.pool?uint64_t(uintptr_t(state.sampler_pool.pool->mem)):0;f[5]=uint64_t(state.sampler_pool.off);}
inline unsigned gather(const nr_dplan &p,const uint64_t f[6],uint64_t *key){unsigned n=0;
    for(unsigned i=0;i<5;i++)key[n++]=uint64_t(uintptr_t(state.shader[i]));
    for(unsigned i=0;i<6;i++)key[n++]=f[i];
    for(unsigned i=0;i<p.nimg;i++){const nr_drow &r=p.row[p.img_row[i]];key[n++]=state.tex[r.ns][r.slot];key[n++]=r.samp!=0xff?state.samp[r.ns][r.samp]:0;}
    return n;}
inline bool same_gen(const Plan &pl,const uint64_t f[6]){
    if(pl.last<0 || memcmp(f,pl.last_fixed,sizeof pl.last_fixed))return false;
    for(unsigned m=pl.p.tex_stages;m;m&=m-1){const unsigned ns=unsigned(__builtin_ctz(m));if(tex_gen[ns]!=pl.last_tg[ns])return false;}
    return true;}
inline void note_last(Plan &pl,int32_t id,const uint64_t f[6]){pl.last=id;memcpy(pl.last_fixed,f,sizeof pl.last_fixed);for(unsigned ns=0;ns<6;ns++)pl.last_tg[ns]=tex_gen[ns];}
inline VkBuffer ring(){if(ring_epoch!=snapshot_epoch){ring_epoch=snapshot_epoch;ring_now=nrbuf::ring_vk();}return ring_now;}
// The uniform blocks of the draw as ring offsets: dynamic ones into off[], static ones into tuple[]. 0 = ok, else F_*.
int blocks(Plan &pl,VkBuffer rb,uint32_t *off,uint32_t *tuple,bool learn){
    const nr_dplan &p=pl.p;
    for(unsigned i=0;i<p.nubo;i++){const nr_drow &r=p.row[p.ubo_row[i]];const NVNbufferRange &u=state.ub[r.ns][r.slot];
        if(!u.address || !u.size)return F_UNBOUND;
        const size_t copy=std::min<size_t>(u.size,r.bytes?r.bytes:65536);
        if(learn && !pl.copy_known)pl.copy[i]=uint32_t(copy);else if(copy!=pl.copy[i])return F_SIZE;
        if(u.address!=ubo_last[r.ns][r.slot]){ubo_last[r.ns][r.slot]=u.address;++ubo_chg[r.ns][r.slot];}
        if(u.address<nrbuf::ring_base || u.address-nrbuf::ring_base>=nrbuf::ring_size)return F_RINGMISS;   // not a block of the constant ring
        Buffer z;int zr=-1;
        if(__builtin_expect(nrzx::dmode!=0,0)){uint64_t zo=0;   // [NR-ZERO Z3d] the ring word answers first (=2: the walk too, and both must agree)
            if(nrbuf::ubo_fast(u.address,copy,zo,nrzx::dmode==1)){
                if(nrzx::dmode==1){z.buffer=nrbuf::zd.buf;z.offset=zo;zr=1;}
                else{zr=nrbuf::ubo(u.address,copy,z);++nrzx::s.d_checked;if(zr!=1 || z.buffer!=nrbuf::zd.buf || z.offset!=zo){++nrzx::s.d_bad;
                    nrzx::red("Z3d: the ring word answers block %#llx+%zu with offset %#llx, the page walk %s (offset %#llx)",(unsigned long long)u.address,copy,(unsigned long long)zo,zr==1?"another view":"does not serve it",(unsigned long long)z.offset);}}}}
        if(zr<0)zr=nrbuf::ubo(u.address,copy,z);
        if(zr!=1 || z.buffer!=rb || z.offset>0xffffffffull)return F_RINGMISS;   // held / raced / pending / unaligned (nrbuf counts why)
        if(r.kind==NRD_K_RING_DYN)off[r.dyn_at]=uint32_t(z.offset);else tuple[r.stat_at]=uint32_t(z.offset);}
    if(learn)pl.copy_known=true;
    return 0;}
// The entry's images as the old hit path checks them when a registration / image changed (memo_check_gen moved).
bool images_alive(Ent &e){
    if(e.check_gen==memo_check_gen)return true;
    for(size_t k=0;k<e.img.size();k++){const MemoImage &c=e.img[k];const tex_t *t=c.first;
        if(c.record->revision!=c.revision || t->serial!=e.ids[k].first || t->mem!=e.ids[k].second || find_image(t->serial)!=c.second)return false;}
    e.check_gen=memo_check_gen;return true;}
// [NR-ZERO Z3b] quiet_ask: the image word of the entry stands (its walk would find every image quiet: nr_dimg_skip == NRD_I_SKIP, and
// write nothing but slot_use = the slot serial it already holds). quiet_done: after a walk - it found all quiet: remember under the words
// of now; DM_NR_Z3B=2: a walk that followed a standing word must have found all quiet.
inline bool quiet_ask(const Ent &e,bool hook){++nrzx::s.b_asked;
    if(hook || !nrz_quiet_ok(&e.quiet,nrz_img_ep,nrz_sub_now(),slot_serial))return false;
    ++nrzx::s.b_quiet;return true;}
inline void quiet_done(Ent &e,bool hook,bool asked_quiet,uint32_t nrd){
    if(asked_quiet){++nrzx::s.b_checked;if(nrd){++nrzx::s.b_bad;nrzx::red("Z3b: the image word of an entry stood, its walk sent %u of %zu images to the read path",nrd,e.img.size());}}
    if(!nrd && !hook && !asked_quiet){nrz_quiet_set(&e.quiet,nrz_img_ep,nrz_sub_now(),slot_serial);++nrzx::s.b_set;}}
// The capture of a build of this draw -> the entry `id` (made or remade). 0 = ok (out holds the build's entry / layout / offsets), else F_*.
int build_entry(Plan &pl,int32_t id,Descriptors &out,Commands &commands,bool write){
    const nr_dplan &p=pl.p;const VkBuffer rb=ring();
    if(p.nubo && !rb)return F_NORING;
    uint32_t tuple[NRD_MAX_UBO]{},off0[NRD_MAX_DYN]{};
    if(int w=blocks(pl,rb,off0,tuple,true))return w;
    const bool rec0=desc_memo_recording;desc_memo_images.clear();desc_memo_dyn.clear();desc_memo_texel.clear();desc_memo_texel_fail=false;
    desc_memo_recording=true;cap_on=true;cap_ok=false;cap.clear();
    descriptors_build(out,commands,false);
    cap_on=false;desc_memo_recording=rec0;
    if(!cap_ok || !out.entry || cap.size()!=p.nrow || out.dynamic_offsets.size()!=p.ndyn || desc_memo_images.size()!=p.nimg)return F_CAPTURE;
    if(pl.layout && pl.layout!=out.entry)return F_LAYOUT;
    for(unsigned i=0;i<p.nrow;i++){const nr_drow &r=p.row[i];const CapRow &c=cap[i];
        const bool dyn=r.kind==NRD_K_RING_DYN || r.kind==NRD_K_FIXED_DYN;
        if(c.binding!=r.binding || (r.kind==NRD_K_IMAGE)!=(c.kind==1))return F_CAPTURE;
        if(r.kind!=NRD_K_IMAGE && c.type!=uint32_t(dyn?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER))return F_CAPTURE;
        if((r.kind==NRD_K_RING_DYN || r.kind==NRD_K_RING_STAT) && c.d.buffer.buffer!=rb)return F_RINGMISS;   // this draw's block is not in the ring buffer
        if(r.kind==NRD_K_RING_STAT && c.d.buffer.offset!=tuple[r.stat_at])return F_CAPTURE;
        if(r.kind==NRD_K_RING_DYN && out.dynamic_offsets[r.dyn_at]!=off0[r.dyn_at])return F_CAPTURE;}
    if(!pl.layout){pl.layout=out.entry;pl.fifo=fifo_of(pl.layout);for(unsigned i=0;i<p.nrow;i++)pl.types[i]=cap[i].type;}
    Ent &e=ents[size_t(id)];
    // (remade: the old sets stay nodes of the recycler, readable until their submissions retired)
    e=Ent{};e.plan=&pl;e.data.resize(p.nrow);for(unsigned i=0;i<p.nrow;i++)e.data[i]=cap[i].d;
    memcpy(e.off,out.dynamic_offsets.data(),p.ndyn*sizeof(uint32_t));
    e.img=desc_memo_images;for(auto &ti:e.img)e.ids.push_back({ti.first->serial,ti.first->mem});
    e.check_gen=memo_check_gen;e.ring=rb;
    if(write){Var &v=e.var[0];v.set=take_set(pl,&v.node,&v.gen);if(!v.set)return F_SET;
        memcpy(v.tuple,tuple,sizeof v.tuple);write_set(pl,v.set,e.data.data());++s.var_write;e.mru=0;out.set=v.set;}
    e.live=true;
    return 0;}
// The entry of the draw: by generation, by hash, or made now. *id >= 0: found (not built now).
int32_t find_entry(Plan &pl,const uint64_t f[6],uint64_t *key,unsigned *nkey,uint64_t *h,bool *by_gen){
    *nkey=0;*by_gen=same_gen(pl,f);if(*by_gen)return pl.last;
    *nkey=gather(pl.p,f,key);*h=nr_dkey_hash64(key,*nkey);return nr_dtab_find(&tab,*h,key,*nkey);}
inline int32_t add_entry(Plan &pl,const uint64_t f[6],uint64_t *key,unsigned *nkey,uint64_t *h){
    if(!*nkey){*nkey=gather(pl.p,f,key);*h=nr_dkey_hash64(key,*nkey);}
    if(tab.n>=tab.max)flush("table full");
    return nr_dtab_add(&tab,*h,key,*nkey);}
// A build that failed: the id stays in the table, empty (the next draw of the key tries again); a shader set that keeps failing backs off.
void failed(Plan &pl,int32_t id,int w){ents[size_t(id)]=Ent{};ents[size_t(id)].plan=&pl;pl.last=-1;++s.fall[w];
    if(w==F_CAPTURE || w==F_LAYOUT){if(++pl.fails>=8){pl.p.why=NRD_NE_ROWS;red("the capture of the old build does not match the plan of this shader set 8 times: it stays on the old path");}}
    else if(w==F_RINGMISS || w==F_SIZE || w==F_UNBOUND){if(++pl.misses>=64){pl.misses=0;pl.retry=slot_serial+600;}}}   // e.g. blocks that never live in the ring: no capture + old path twice a draw
// DM_NR_DESC=1: serve the draw. true = out is complete (set, layout, dynamic offsets); false = the old path runs (out untouched).
bool serve(Descriptors &out,Commands &commands){
    ++s.draws;if(!(++calls&1023))tick();z_out=NRZD_FELL_GATE;z_rd=0;   // [NR-ZERO Z3] until a later line says more
    const bool timed=!(calls&63);const uint64_t t0=timed?timing_clock():0;const bool sp=timed && (calls>>6&1);uint64_t ta=0,tb=0,tc=0;uint32_t nrd=0;   // [NR-ZERO] sp: this draw is timed by part
    Plan *plp=plan_for();if(plp->p.why){++s.fall[F_NE];++s.ne[plp->p.why];return false;}
    Plan &pl=*plp;const nr_dplan &p=pl.p;
    if(pl.retry){if(slot_serial<pl.retry){++s.fall[F_BACKOFF];return false;}pl.retry=0;}
    if(my_epoch_moved())flush("views, samplers or the ring buffer were replaced");
    uint64_t f[6],key[NRD_KEY_WORDS],h=0;unsigned nkey=0;bool by_gen=false;fixed_words(f);
    int32_t id=find_entry(pl,f,key,&nkey,&h,&by_gen);
    uint32_t off[NRD_MAX_DYN],tuple[NRD_MAX_UBO];
    const VkBuffer rb=ring();if(sp)ta=timing_clock();
    if(id>=0){Ent &e=ents[size_t(id)];
        if(!e.live || e.ring!=rb || !images_alive(e)){z_out=NRZD_BUILT;const bool re=e.live;const int w=build_entry(pl,id,out,commands,true);
            if(w){desc_reset(&out);failed(pl,id,w);return false;}
            if(re)++s.relearn;else ++s.learned;note_last(pl,id,f);done(out,pl);++s.served;return true;}
        memcpy(off,e.off,p.ndyn*sizeof(uint32_t));
        if(int w=blocks(pl,rb,off,tuple,false)){z_out=NRZD_FELL_LIVE;++s.fall[w];if(++pl.misses>=64){pl.misses=0;pl.retry=slot_serial+600;}return false;}
        if(sp)tb=timing_clock();
        // images: the read path only where it can do something
        const bool hook=!img_fast;   // (nbchk::read only counts host reads the IR lacks: a skipped material read cannot raise it)
        // [NR-ZERO Z3b] the image word: the entry's last walk found every image quiet and nothing that can end that has happened since
        const bool zq=__builtin_expect(nrzx::bmode!=0,0) && quiet_ask(e,hook);
        if(zq && nrzx::bmode==1){s.img+=e.img.size();s.img_skip+=e.img.size();}else{
        for(const MemoImage &ti:e.img){Image &im=*ti.second;++s.img;
            const nr_dimg_in x{im.last_write,im.storage_write,im.opw,uint8_t(im.checked_submission==stats.submissions),uint8_t(im.uploaded),uint8_t(im.gpu_written),uint8_t(im.layout==VK_IMAGE_LAYOUT_GENERAL),uint8_t(hook)};
            const int w=nr_dimg_skip(&x);if(w==NRD_I_SKIP){im.slot_use=slot_serial;++s.img_skip;continue;}
            ++s.img_why[w];++nrd;Image &g=get_image(ti.first,true);prepare_read(g,commands.command,-1);
            if(&g!=&im)e.check_gen=0;}   // another image answers for the texture now: the entry is checked (and remade) at its next draw
        if(__builtin_expect(nrzx::bmode!=0,0))quiet_done(e,hook,zq,nrd);}
        if(sp)tc=timing_clock();
        // the set: the one of the same static tuple (none: the texture part's one set), still this entry's node; else a set rewritten from the entry's data
        Var *v=nullptr;
        for(unsigned k=0;k<2 && !v;k++){Var &c=e.var[k];if(c.set && !memcmp(c.tuple,tuple,p.nstat*sizeof(uint32_t)) && (c.node<0 || nr_dfifo_use(&pl.fifo->f,c.node,c.gen,uint32_t(slot_serial)))){v=&c;e.mru=uint8_t(k);++s.var_hit;}}
        if(!v){Var &c=e.var[p.nstat?e.mru^1:0];c=Var{};c.set=take_set(pl,&c.node,&c.gen);if(!c.set){z_out=NRZD_FELL_LIVE;++s.fall[F_SET];return false;}
            static std::vector<DescEntry> data;data.assign(e.data.begin(),e.data.end());
            for(unsigned i=0;i<p.nrow;i++)if(p.row[i].kind==NRD_K_RING_STAT)data[i].buffer.offset=tuple[p.row[i].stat_at];
            memcpy(c.tuple,tuple,sizeof c.tuple);write_set(pl,c.set,data.data());e.mru=uint8_t(&c-e.var);v=&c;++s.var_write;}
        const VkDescriptorSet set=v->set;
        if(by_gen)++s.by_gen;else ++s.by_hash;
        out.entry=pl.layout;out.set=set;out.pipeline_layout=pl.layout->pipeline_layout;out.dynamic_offsets.assign(off,off+p.ndyn);
        note_last(pl,id,f);done(out,pl);++s.served;s.ubos+=p.nubo;z_out=NRZD_HIT;z_rd=nrd;
        if(timed){const uint64_t te=timing_clock();if(sp){++s.sp_n;if(!by_gen)++s.sp_hash;s.sp_find+=ta-t0;s.sp_blocks+=tb-ta;s.sp_img+=tc-tb;s.sp_set+=te-tc;s.sp_nblk+=p.nubo;s.sp_nimg+=e.img.size();s.sp_rd+=nrd;}else{s.ns+=te-t0;++s.timed;}}
        return true;}
    z_out=NRZD_BUILT;id=add_entry(pl,f,key,&nkey,&h);if(id<0){++s.fall[F_SET];return false;}
    {const int w=build_entry(pl,id,out,commands,true);if(w){desc_reset(&out);failed(pl,id,w);return false;}}
    ++s.learned;note_last(pl,id,f);done(out,pl);++s.served;return true;}
// ---- DM_NR_DESC=2: the prediction before the old path, the comparison after it ----
struct Pred{Plan *pl=nullptr;int32_t id=-1;bool valid=false,skip_all=false,have_blocks=false;uint32_t off[NRD_MAX_DYN]{},tuple[NRD_MAX_UBO]{};
    VkCommandBuffer open{};uint64_t bseq=0,fseq=0,pend=0;uint64_t f[6]{},key[NRD_KEY_WORDS]{},h=0;unsigned nkey=0;} pred;
void predict(){
    pred.pl=nullptr;pred.id=-1;pred.valid=false;pred.have_blocks=false;
    ++s.draws;if(!(++calls&1023))tick();
    Plan *plp=plan_for();if(plp->p.why){++s.fall[F_NE];++s.ne[plp->p.why];return;}
    Plan &pl=*plp;const nr_dplan &p=pl.p;
    if(pl.retry){if(slot_serial<pl.retry){++s.fall[F_BACKOFF];return;}pl.retry=0;}
    pred.pl=plp;
    if(my_epoch_moved())flush("views, samplers or the ring buffer were replaced");
    fixed_words(pred.f);pred.nkey=gather(p,pred.f,pred.key);pred.h=nr_dkey_hash64(pred.key,pred.nkey);
    const int32_t by_hash=nr_dtab_find(&tab,pred.h,pred.key,pred.nkey);
    if(same_gen(pl,pred.f)){++s.by_gen;if(pl.last!=by_hash){bad(B_GEN);red("the entry by texture generation (%d) is not the entry of the handles (%d)",pl.last,by_hash);}}else ++s.by_hash;
    pred.id=by_hash;
    if(by_hash<0)return;
    Ent &e=ents[size_t(by_hash)];const VkBuffer rb=ring();
    if(!e.live || e.ring!=rb || !images_alive(e)){pred.id=-2;return;}   // would be remade: remake it after the old path
    memcpy(pred.off,e.off,p.ndyn*sizeof(uint32_t));
    if(int w=blocks(pl,rb,pred.off,pred.tuple,false)){++s.fall[w];return;}
    pred.have_blocks=true;pred.skip_all=true;const bool hook=!img_fast;   // (nbchk::read only counts host reads the IR lacks: a skipped material read cannot raise it)
    for(const MemoImage &ti:e.img){const Image &im=*ti.second;++s.img;
        const nr_dimg_in x{im.last_write,im.storage_write,im.opw,uint8_t(im.checked_submission==stats.submissions),uint8_t(im.uploaded),uint8_t(im.gpu_written),uint8_t(im.layout==VK_IMAGE_LAYOUT_GENERAL),uint8_t(hook)};
        const int w=nr_dimg_skip(&x);if(w==NRD_I_SKIP)++s.img_skip;else{++s.img_why[w];pred.skip_all=false;}}
    pred.open=open_pass.cmd;pred.bseq=barrier_seq;pred.fseq=frag_seq;pred.pend=hit_pend_gen;pred.valid=true;++s.served;s.ubos+=p.nubo;}
void compare(Descriptors &out,Commands &commands){
    if(!pred.pl || !out.set || !out.entry)return;
    Plan &pl=*pred.pl;const nr_dplan &p=pl.p;
    static Descriptors tmp;
    if(pred.valid){++s.checked;Ent &e=ents[size_t(pred.id)];note_last(pl,pred.id,pred.f);   // (the generation shortcut is then checked at the next draw of the shader set)
        if(pl.layout!=out.entry){bad(B_LAYOUT);red("the old path used another layout than the entry's");}
        else if(out.dynamic_offsets.size()!=p.ndyn || memcmp(out.dynamic_offsets.data(),pred.off,p.ndyn*sizeof(uint32_t))){bad(B_OFFSET);unsigned i=0;while(i<p.ndyn && i<out.dynamic_offsets.size() && out.dynamic_offsets[i]==pred.off[i])++i;
            red("dynamic offsets differ at %u of %u (old %zu): entry %#x, old path %#x",i,unsigned(p.ndyn),out.dynamic_offsets.size(),i<p.ndyn?pred.off[i]:0u,i<out.dynamic_offsets.size()?out.dynamic_offsets[i]:0u);}
        if(pred.skip_all && (open_pass.cmd!=pred.open || barrier_seq!=pred.bseq || frag_seq!=pred.fseq || hit_pend_gen!=pred.pend)){bad(B_HAZARD);
            red("no image of the entry needed the read path, but the old path %s",open_pass.cmd!=pred.open?"ended the open pass":barrier_seq!=pred.bseq || frag_seq!=pred.fseq?"recorded a barrier":"resolved pending results");}
        if(pred.skip_all)for(const MemoImage &ti:e.img){const Image &im=*ti.second;if(im.gpu_written || im.last_write || im.storage_write || im.opw || im.layout!=VK_IMAGE_LAYOUT_GENERAL){bad(B_HAZARD);red("image serial %llu skipped by the entry was touched by the old path",(unsigned long long)ti.first->serial);break;}}
        if(s.checked%uint64_t(cmp_n))return;
        // one draw in cmp_n: built once more as a capture, every write against the entry
        desc_reset(&tmp);const bool rec0=desc_memo_recording;desc_memo_images.clear();desc_memo_dyn.clear();desc_memo_texel.clear();desc_memo_texel_fail=false;
        desc_memo_recording=true;cap_on=true;cap_ok=false;cap.clear();descriptors_build(tmp,commands,false);cap_on=false;desc_memo_recording=rec0;++s.built;
        if(!cap_ok || cap.size()!=p.nrow || tmp.dynamic_offsets.size()!=p.ndyn){bad(B_ROWS);red("the build wrote %zu descriptors / %zu dynamic offsets, the plan has %u / %u",cap.size(),tmp.dynamic_offsets.size(),unsigned(p.nrow),unsigned(p.ndyn));return;}
        if(memcmp(tmp.dynamic_offsets.data(),pred.off,p.ndyn*sizeof(uint32_t))){bad(B_OFFSET);red("dynamic offsets differ from the rebuilt set's");}
        for(unsigned i=0;i<p.nrow;i++){const nr_drow &r=p.row[i];const CapRow &c=cap[i];const DescEntry &d=e.data[i];
            if(c.binding!=r.binding || c.type!=pl.types[i] || (r.kind==NRD_K_IMAGE)!=(c.kind==1)){bad(B_ROWS);red("descriptor %u: binding %u type %u, the plan has binding %u type %u",i,c.binding,c.type,r.binding,pl.types[i]);return;}
            if(r.kind==NRD_K_IMAGE){if(c.d.image.imageView!=d.image.imageView || c.d.image.sampler!=d.image.sampler || c.d.image.imageLayout!=d.image.imageLayout){bad(B_IMAGE);
                    red("binding %u (stage %u texture slot %u): the entry holds view %p sampler %p, the build wrote view %p sampler %p",r.binding,unsigned(r.ns),unsigned(r.slot),(void*)d.image.imageView,(void*)d.image.sampler,(void*)c.d.image.imageView,(void*)c.d.image.sampler);return;}}
            else if(r.kind==NRD_K_RING_STAT){if(c.d.buffer.buffer!=d.buffer.buffer || c.d.buffer.range!=d.buffer.range){bad(B_BUFFER);red("binding %u (static block): buffer / range differ",r.binding);return;}
                if(c.d.buffer.offset!=pred.tuple[r.stat_at]){bad(B_TUPLE);red("binding %u (static block): the entry's offset %#x, the build wrote %#llx",r.binding,pred.tuple[r.stat_at],(unsigned long long)c.d.buffer.offset);return;}}
            else if(c.d.buffer.buffer!=d.buffer.buffer || c.d.buffer.offset!=d.buffer.offset || c.d.buffer.range!=d.buffer.range){bad(B_BUFFER);
                red("binding %u (%s): the entry holds buffer %p+%llu range %llu, the build wrote %p+%llu range %llu",r.binding,r.kind==NRD_K_RING_DYN?"ring block":"support / c1",(void*)d.buffer.buffer,(unsigned long long)d.buffer.offset,
                    (unsigned long long)d.buffer.range,(void*)c.d.buffer.buffer,(unsigned long long)c.d.buffer.offset,(unsigned long long)c.d.buffer.range);return;}}
        return;}
    if(pred.id!=-1 && pred.id!=-2)return;   // blocks not served now: counted in predict
    // no usable entry: make it from a capture of this draw (sets are written too: the pool and the recycler run as in mode 1)
    int32_t id=pred.id==-2?nr_dtab_find(&tab,pred.h,pred.key,pred.nkey):-1;
    if(id<0){if(tab.n>=tab.max)flush("table full");id=nr_dtab_add(&tab,pred.h,pred.key,pred.nkey);}
    if(id<0)return;
    desc_reset(&tmp);const int w=build_entry(pl,id,tmp,commands,true);++s.built;
    if(w){failed(pl,id,w);return;}
    if(pred.id==-2)++s.relearn;else ++s.learned;
    if(pl.layout!=out.entry){bad(B_LAYOUT);red("the capture build and the old path disagree on the layout");}
    note_last(pl,id,pred.f);}
// descriptors(): one object right after its memo gate. served: DM_NR_DESC=1 answered the draw (the caller returns).
struct Scope{bool served=false,cmp=false;Descriptors &out;Commands &commands;
    Scope(bool ok,bool compute,Descriptors &o,Commands &c):out(o),commands(c){
        if(mode==0)return;
        if(mode<0){init();if(mode<=0)return;}
        if(compute)return;
        if(!ok || read_preflight){++s.draws;++s.fall[F_GATE];z_out=NRZD_FELL_GATE;return;}
        if(mode==1){{NRCPU_SCOPE(NrDesc);served=serve(o,c);}if(served && nbdesc::mode>0){nbdesc::cand=false;nbdesc::P.valid=false;}}
        else{predict();cmp=true;}}
    ~Scope(){if(cmp)compare(out,commands);}};
}
