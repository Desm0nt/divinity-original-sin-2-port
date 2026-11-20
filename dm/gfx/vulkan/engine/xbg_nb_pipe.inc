// [DOS2] NB-13b: DM_NB_PIPE - draw()'s pipeline memo by state-group id (docs/dos2/NATIVE_BACKEND_PLAN.md NB-13b; ids, memo and their
// soundness in dm/gfx/nb/nb_pipe.h). Included by xbg_game.cpp right after pipe_memo_fill (it needs State, PipeMemoKey, pipe_memo_next).
// Hooks, one line / one token each in xbg_game.cpp:
//   execute()                 mark: before the switch, the groups a state record writes are dirty (OP_X_NB_STATE blocks run their records
//                             through execute(), so this covers the NB-13a shadow and plain records alike)
//   draw() fast block         look: after restart_eff, before pipe_memo_fill; a hit (mode 1) sets the pipeline, the vertex binding list and
//                             the zero binding and skips pipe_memo_fill / the POD lookup / the binding loop; otherwise the old block runs
//   draw() memo stores        store: where the old fast block takes its hit and where the slow path stores the memo key (same condition:
//                             a cacheable, non-transient pipeline); mode 2 compares the would-be hit there (full key, pipeline, bindings)
//   pipe_pod_erase / clears   erase / clear: the memo follows the POD map (eviction, pipeline / descriptor / render pass cache clears)
//   the state reset (init)    reset: reads DM_NB_PIPE once, every group dirty
// DM_NB_PIPE 0 (default): mark / look are behind one load + not-taken branch, store behind a draw() local that stays false, erase / clear
// return at once: nothing else runs, the pipelines and the command stream are the old ones. 1: hits taken (only with DM_GFX_DRAW_FAST=1:
// DRAW_FAST=2 verifies, then nothing is taken). 2: check, nothing taken: every would-be hit is compared with what the old path gives.
#include "../../nb/nb_pipe.h"
namespace nbpipe{
static_assert(NBP_G_BLEND==NBS_K_BLEND && NBP_G_COLOR==NBS_K_COLOR && NBP_G_POLY==NBS_K_POLY && NBP_G_DS==NBS_K_DS && NBP_G_VFMT==NBS_K_VFMT,"NB-13b groups = NB-13a kinds 0..4");
// The group spans tile the PipeMemoKey state slices exactly (fixed / stencil / tail), restart_eff (set by draw() itself) goes in the key.
#define NBP_O(f) offsetof(State,f)
static_assert(NBP_O(attrib)<NBP_O(stream) && NBP_O(stream)<NBP_O(blend) && NBP_O(blend)<NBP_O(color) && NBP_O(color)<NBP_O(mask) && NBP_O(mask)<NBP_O(ms) &&
              NBP_O(ms)<NBP_O(poly) && NBP_O(poly)<NBP_O(ds) && NBP_O(ds)<NBP_O(viewport),"PipeMemoKey fixed slice: attrib .. ds");
static_assert(NBP_O(blend_color)<NBP_O(bias) && NBP_O(bias)<NBP_O(stencil_compare) && NBP_O(stencil_compare)<NBP_O(stencil_write) &&
              NBP_O(stencil_write)<NBP_O(stencil_ref) && NBP_O(stencil_ref)<NBP_O(copy_stride),"PipeMemoKey stencil slice: blend_color .. stencil_ref");
static_assert(NBP_O(patch_size)<NBP_O(bounds) && NBP_O(bounds)<NBP_O(restart) && NBP_O(restart)<NBP_O(depth_clamp) && NBP_O(depth_clamp)<NBP_O(rast_discard) &&
              NBP_O(rast_discard)<NBP_O(restart_eff) && NBP_O(restart_eff)+4==NBP_O(alpha_ref) && NBP_O(alpha_ref)<NBP_O(count_samples) && NBP_O(count_samples)<65536,
              "PipeMemoKey tail slice: patch_size .. alpha_ref, restart_eff a 4-byte word");
static_assert(sizeof(PipeMemoKey::fixed)==NBP_O(viewport)-NBP_O(attrib) && sizeof(PipeMemoKey::stencil)==NBP_O(copy_stride)-NBP_O(blend_color) &&
              sizeof(PipeMemoKey::tail)==NBP_O(count_samples)-NBP_O(patch_size),"the spans below are the memo key's slices");
inline nbp_span span(size_t a,size_t b,size_t c=0,size_t d=0){nbp_span s{};s.off[0]=uint16_t(a);s.len[0]=uint16_t(b-a);s.off[1]=uint16_t(c);s.len[1]=uint16_t(d-c);return s;}
int mode=0;bool read_env=false;
uint64_t epoch=0;   // [DOS2] NR-09: moves whenever group ids restart or cached pipelines / layouts / render passes go (the table of nrdraw keys on the ids and the pointers)
nbp_groups *G=nullptr;nbp_memo *M=nullptr;
std::vector<PipeMemoKey> full;      // mode 2: the full memo key each entry was stored under (index = entry)
nbp_key key{};uint64_t khash=0;bool khash_ok=false;
nbp_ent *prev=nullptr;              // the entry the previous keyed draw hit or stored (its key is compared before hashing)
nbp_ent *would=nullptr;bool would_prev=false;uint64_t would_pipe=0;   // a hit not taken (mode 2 / DRAW_FAST=2): compared at the store
struct Stats{uint64_t looks,keyed,hit_prev,hit_map,taken,miss_new,miss_dead,skip_pp,stored,revived,clears_ids,clears_cache,cmp,bad,bad_key,bad_pipe,bad_bind,
    unchecked,evicted,stale,hit_ns,hit_t,miss_ns,miss_t;}s{};
uint64_t next_ns=0;unsigned told=0;
const char *gname(int g){static const char *n[NBP_GROUPS]={"blend","colour","polygon","depth-stencil","vertex format","misc"};return g>=0 && g<NBP_GROUPS?n[g]:"?";}
// State offset -> field name (check lines)
const char *sfield(size_t o){
    static const struct{size_t o;const char *n;}f[]={{NBP_O(attrib),"attrib"},{NBP_O(stream),"stream"},{NBP_O(blend),"blend"},{NBP_O(color),"color"},
        {NBP_O(mask),"mask"},{NBP_O(ms),"ms"},{NBP_O(poly),"poly"},{NBP_O(ds),"ds"},{NBP_O(viewport),"viewport"},{NBP_O(blend_color),"blend_color"},
        {NBP_O(bias),"bias"},{NBP_O(stencil_compare),"stencil_compare"},{NBP_O(stencil_write),"stencil_write"},{NBP_O(stencil_ref),"stencil_ref"},
        {NBP_O(copy_stride),"copy_stride"},{NBP_O(patch_size),"patch_size"},{NBP_O(bounds),"bounds"},{NBP_O(restart),"restart"},{NBP_O(depth_clamp),"depth_clamp"},
        {NBP_O(rast_discard),"rast_discard"},{NBP_O(restart_eff),"restart_eff"},{NBP_O(alpha_ref),"alpha_ref"},{NBP_O(count_samples),"count_samples"}};
    const char *n="?";for(const auto &x:f)if(o>=x.o)n=x.n;return n;}
// PipeMemoKey offset -> field name
const char *kfield(size_t o){
    if(o<offsetof(PipeMemoKey,layout))return "shader";if(o<offsetof(PipeMemoKey,pass_def))return "layout";if(o<offsetof(PipeMemoKey,pass))return "pass_def";
    if(o<offsetof(PipeMemoKey,pipeline_layout))return "pass";if(o<offsetof(PipeMemoKey,prim))return "pipeline_layout";if(o<offsetof(PipeMemoKey,samples))return "prim";
    if(o<offsetof(PipeMemoKey,rt_n))return "samples";if(o<offsetof(PipeMemoKey,colors))return "rt_n";if(o<offsetof(PipeMemoKey,depth))return "colors";
    if(o<offsetof(PipeMemoKey,depth_format))return "depth";if(o<offsetof(PipeMemoKey,fixed))return "depth_format";
    if(o<offsetof(PipeMemoKey,stencil))return sfield(NBP_O(attrib)+(o-offsetof(PipeMemoKey,fixed)));
    if(o<offsetof(PipeMemoKey,tail))return sfield(NBP_O(blend_color)+(o-offsetof(PipeMemoKey,stencil)));
    return sfield(NBP_O(patch_size)+(o-offsetof(PipeMemoKey,tail)));}
void reset(){   // the state reset (init): DM_NB_PIPE once; every group is re-read at the next draw
    if(!read_env){read_env=true;const char *e=getenv("DM_NB_PIPE");int m=e && *e?std::clamp(atoi(e),0,2):0;
        if(m){const nbp_span sp[NBP_GROUPS]={span(NBP_O(blend),NBP_O(color),NBP_O(blend_color),NBP_O(bias)),span(NBP_O(color),NBP_O(poly),NBP_O(alpha_ref),NBP_O(count_samples)),
                span(NBP_O(poly),NBP_O(ds),NBP_O(bias),NBP_O(stencil_compare)),span(NBP_O(ds),NBP_O(viewport),NBP_O(stencil_compare),NBP_O(copy_stride)),
                span(NBP_O(attrib),NBP_O(blend)),span(NBP_O(patch_size),NBP_O(restart_eff))};
            G=static_cast<nbp_groups*>(calloc(1,sizeof(nbp_groups)));M=static_cast<nbp_memo*>(calloc(1,sizeof(nbp_memo)));
            if(!G || !M || nbp_groups_init(G,sp) || nbp_memo_init(M)){xbg_logf("[nb] DM_NB_PIPE=%d: no memory for the group dictionaries / memo - off",m);m=0;}}
        mode=m;
        xbg_logf("[nb] DM_NB_PIPE=%d (NB-13b: draw()'s pipeline memo by state-group id %s; works inside DM_GFX_DRAW_FAST's fast block, i.e. with gfx-pipe-memo / DM_GFX_PIPE_MEMO=1)",mode,
                 mode==1?"- hits taken when DM_GFX_DRAW_FAST=1":mode==2?"- check: would-be hits compared with the old memo / full lookup, none taken":"- off");}
    if(G)G->dirty=(1u<<NBP_GROUPS)-1;}
void clear(){++epoch;if(!M)return;nbp_memo_clear(M);prev=nullptr;would=nullptr;++s.clears_cache;}   // wherever the POD map is cleared
void erase(VkPipeline old){++epoch;if(!M)return;uint64_t v=0;memcpy(&v,&old,sizeof old);nbp_kill(M,v);}   // an evicted pipeline: its entries go dead
// execute(): the groups this record writes (NB-13a's kind table for the state ops, + the misc group)
inline void mark(uint32_t op,const uint8_t *p,size_t n){
    uint32_t k32;const int k=nbs_kind(op,p,uint32_t(n),&k32);
    if(k>=0 && k<=NBS_K_VFMT){G->dirty|=1u<<k;return;}
    if(k<0){const unsigned ks=nbs_op_kinds(op)&((1u<<(NBS_K_VFMT+1))-1);if(ks){G->dirty|=ks;return;}}
    switch(op){case OP_X_PATCH_SIZE:case OP_X_DEPTH_BOUNDS:case OP_X_PRIM_RESTART:case OP_X_DEPTH_CLAMP:case OP_X_RAST_DISCARD:G->dirty|=1u<<NBP_G_MISC;break;default:break;}}
void tick(uint64_t now){   // the 10 s lines; the window's counters start again
    if(next_ns){
        const uint64_t keyed=s.keyed?s.keyed:1,looks=s.looks?s.looks:1;
        xbg_logf("[nb] pipe (10 s): DM_NB_PIPE=%d; %llu fast-block draws, %llu keyed: hit %llu (%.1f%%: previous draw %llu, map %llu; taken %llu), miss %llu (new key %llu, "
                 "evicted entry %llu); not keyed: pre-pass depth override %llu; stored %llu (revived %llu), entries %u / %d, memo cleared %llu (ids %llu, full %llu, caches %llu), "
                 "pipelines evicted %llu; groups: %.2f re-read a draw (%.2f unchanged, %.3f new ids); NB step ns a draw (1 in 64): hit %.0f, miss %.0f",
            mode,(unsigned long long)s.looks,(unsigned long long)s.keyed,(unsigned long long)(s.hit_prev+s.hit_map),100.0*double(s.hit_prev+s.hit_map)/double(keyed),
            (unsigned long long)s.hit_prev,(unsigned long long)s.hit_map,(unsigned long long)s.taken,(unsigned long long)(s.miss_new+s.miss_dead),(unsigned long long)s.miss_new,
            (unsigned long long)s.miss_dead,(unsigned long long)s.skip_pp,(unsigned long long)s.stored,(unsigned long long)s.revived,M->n,int(NBP_ENTRIES),
            (unsigned long long)(s.clears_ids+M->st[NBP_M_FULL]+s.clears_cache),(unsigned long long)s.clears_ids,(unsigned long long)M->st[NBP_M_FULL],(unsigned long long)s.clears_cache,
            (unsigned long long)M->st[NBP_M_KILLED],double(G->st[NBP_S_REFRESH])/double(looks),double(G->st[NBP_S_SAME])/double(looks),double(G->st[NBP_S_NEWID])/double(looks),
            s.hit_t?double(s.hit_ns)/double(s.hit_t):0.0,s.miss_t?double(s.miss_ns)/double(s.miss_t):0.0);
        if(mode==2 || s.cmp || s.unchecked || s.stale)
            xbg_logf("[nb] pipe check (10 s): %llu would-be hits compared, bad %llu (full memo key %llu, pipeline %llu, vertex bindings %llu); not compared: the old path stored "
                     "nothing %llu, entry evicted meanwhile %llu; groups changed without a marking record %llu",(unsigned long long)s.cmp,(unsigned long long)s.bad,
                     (unsigned long long)s.bad_key,(unsigned long long)s.bad_pipe,(unsigned long long)s.bad_bind,(unsigned long long)s.unchecked,(unsigned long long)s.evicted,
                     (unsigned long long)s.stale);}
    s=Stats{};memset(G->st,0,sizeof G->st);M->st[NBP_M_KILLED]=M->st[NBP_M_FULL]=0;
    next_ns=now+10000000000ull;}
inline void key_fill(const void *layout,const void *pass_def,VkRenderPass pass,VkPipelineLayout pl,int prim,VkSampleCountFlagBits samples,uint32_t colors){
    uint64_t *w=key.w;for(unsigned i=0;i<5;i++)w[i]=uint64_t(uintptr_t(state.shader[i]));
    w[5]=uint64_t(uintptr_t(layout));w[6]=uint64_t(uintptr_t(pass_def));w[7]=0;memcpy(&w[7],&pass,sizeof pass);w[8]=0;memcpy(&w[8],&pl,sizeof pl);
    w[9]=uint64_t(uint32_t(prim))|uint64_t(uint32_t(samples))<<32;w[10]=uint64_t(uint32_t(state.rt.n))|uint64_t(colors)<<32;
    w[11]=uint64_t(state.rt.depth!=nullptr)|uint64_t(state.rt.depth?uint32_t(state.rt.depth->d.format):0u)<<32;
    w[12]=G->gid[0]|uint64_t(G->gid[1])<<32;w[13]=G->gid[2]|uint64_t(G->gid[3])<<32;w[14]=G->gid[4]|uint64_t(G->gid[5])<<32;w[15]=state.restart_eff;}
// The fast block, after restart_eff: 2 = hit taken (out / bindings / zero_binding set), 1 = keyed (store() once the pipeline is known),
// 0 = not keyed (this draw's depth-stencil is the pre-pass override, not the group's bytes).
int look(VkPipeline &out,const void *layout,const void *pass_def,VkRenderPass pass,VkPipelineLayout pl,int prim,VkSampleCountFlagBits samples,uint32_t colors,
         bool ds_override,int draw_fast,std::vector<VkVertexInputBindingDescription> &bindings,int &zero_binding){
    if(would){++s.unchecked;would=nullptr;}   // the previous keyed draw never reached a store (no / transient pipeline)
    ++s.looks;
    if(!(s.looks&255)){const uint64_t now=timing_clock();if(now>=next_ns)tick(now);}
    if(ds_override){++s.skip_pp;return 0;}
    const bool timed=!(s.looks&63);const uint64_t t0=timed?timing_clock():0;
    const uint8_t *base=reinterpret_cast<const uint8_t*>(&state);
    if(mode==2){const uint32_t st=nbp_stale(G,base);
        if(st){++s.stale;G->dirty|=st;
            if(told<16){++told;for(int g=0;g<NBP_GROUPS;g++)if(st>>g&1)xbg_logf("[nb] pipe check differs: the %s group changed with no record marking it (frame %llu; vs=%s)",gname(g),
                (unsigned long long)stats.presents.load(),state.shader[0]?state.shader[0]->key.c_str():"-");}}}
    if(G->dirty && nbp_refresh(G,base)){nbp_memo_clear(M);prev=nullptr;++s.clears_ids;++epoch;}
    key_fill(layout,pass_def,pass,pl,prim,samples,colors);++s.keyed;khash_ok=false;
    nbp_ent *e=nullptr;bool via_prev=false;
    if(prev && !memcmp(&prev->k,&key,sizeof key)){e=prev;via_prev=true;}
    else{khash=nbp_key_hash(&key);khash_ok=true;e=nbp_find(M,&key,khash);}
    if(e && e->pipe){++(via_prev?s.hit_prev:s.hit_map);
        if(mode==1 && draw_fast==1){memcpy(&out,&e->pipe,sizeof out);
            bindings.clear();for(uint32_t m=e->used;m;m&=m-1){const uint32_t i=uint32_t(__builtin_ctz(m));bindings.push_back({i,e->stride[i],(e->inst>>i&1)?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX});}
            zero_binding=e->zero;prev=e;++s.taken;
            if(timed){s.hit_ns+=timing_clock()-t0;++s.hit_t;}
            return 2;}
        would=e;would_prev=via_prev;would_pipe=e->pipe;}
    else if(e)++s.miss_dead;else ++s.miss_new;
    if(timed){s.miss_ns+=timing_clock()-t0;++s.miss_t;}
    return 1;}
// The pipeline this keyed draw got from the old path (its memo hit or the slow path's store), with its vertex bindings.
void store(VkPipeline p,const std::vector<VkVertexInputBindingDescription> &b,int zero){
    uint64_t pv=0;memcpy(&pv,&p,sizeof p);
    uint16_t used=0,inst=0;uint32_t stride[16]={};
    for(const auto &x:b){if(x.binding>=16){would=nullptr;return;}used=uint16_t(used|1u<<x.binding);stride[x.binding]=x.stride;if(x.inputRate==VK_VERTEX_INPUT_RATE_INSTANCE)inst=uint16_t(inst|1u<<x.binding);}
    if(!pv || zero<-1 || zero>=16){would=nullptr;return;}
    if(would){
        if(!would->pipe || would->pipe!=would_pipe){++s.evicted;}
        else{++s.cmp;const size_t i=size_t(would-M->e);
            const bool kb=mode==2 && i<full.size() && memcmp(&full[i],&pipe_memo_next,sizeof pipe_memo_next)!=0;
            const bool pb=would->pipe!=pv,bb=would->used!=used || would->inst!=inst || would->zero!=zero || memcmp(would->stride,stride,sizeof stride)!=0;
            if(kb || pb || bb){++s.bad;s.bad_key+=kb;s.bad_pipe+=pb;s.bad_bind+=bb;
                if(told<16){++told;size_t at=0;if(kb){const uint8_t *x=reinterpret_cast<const uint8_t*>(&full[i]),*y=reinterpret_cast<const uint8_t*>(&pipe_memo_next);while(at<sizeof(PipeMemoKey) && x[at]==y[at])at++;}
                    xbg_logf("[nb] pipe check differs: %s hit (frame %llu): %s%s%s%s; entry pipeline %#llx bindings %#x/%#x zero %d, old path %#llx %#x/%#x zero %d (vs=%s ps=%s)",
                        would_prev?"previous-draw":"map",(unsigned long long)stats.presents.load(),kb?"full memo key differs at ":"",kb?kfield(at):"",pb?" pipeline":"",bb?" bindings":"",
                        (unsigned long long)would->pipe,unsigned(would->used),unsigned(would->inst),int(would->zero),(unsigned long long)pv,unsigned(used),unsigned(inst),zero,
                        state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-");}}}
        would=nullptr;}
    if(!khash_ok){khash=nbp_key_hash(&key);khash_ok=true;}
    int cleared=0,fresh=0;nbp_ent *e=nbp_put(M,&key,khash,&cleared,&fresh);if(cleared)prev=nullptr;
    if(!fresh && !e->pipe)++s.revived;
    e->pipe=pv;e->used=used;e->inst=inst;memcpy(e->stride,stride,sizeof stride);e->zero=int8_t(zero);
    if(mode==2){const size_t i=size_t(e-M->e);if(full.size()<=i)full.resize(i+1);full[i]=pipe_memo_next;}
    prev=e;++s.stored;}
#undef NBP_O
}
