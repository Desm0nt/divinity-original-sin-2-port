// [DOS2] NR-POSONLY DM_NR_POSONLY (docs/dos2/nr/NR-POSONLY.md): depth-only draws of static meshes fetch a compact position-only copy
// instead of the interleaved vertex buffer. Included by xbg_game.cpp right before draw() (after xbg_nr_shc.inc). Core in nr_posonly.h,
// the vertex-shader proof in xbg_spirv_posonly.cpp (computed at load: Shader::po), NR-06's static buffer table (nrbuf::tab) names the
// static meshes.
//   DM_NR_POSONLY  0  default: nothing (draw() pays one load and a not-taken branch; the lane one more).
//                  2  count: every draw is classified (depth-only, fragment shader reads no varying, vertex shader proved position-only,
//                     position format, stride, static mesh), with the vertex bytes it fetches now and would fetch, and the memory the
//                     copies would take; nothing is drawn differently.
//                  1  apply: such a draw binds its mesh's copy (one arena buffer, the position's bytes of every vertex, same format) with
//                     a pipeline whose vertex input has that one binding (stride = position size); the other vertex inputs of the shader are
//                     dead (the proof) and read the same binding at offset 0. A copy is made at first use; it holds while the write-protect
//                     tracker's token for the mesh holds (else the pages are checked against its stamp and it is made again), every backend
//                     write into the mesh (snapshot_invalidate) and the mesh's end drop it. 1 in 64 uses gathers again and compares
//                     (red line on a difference). Fallback = the draw as before.
// Line "[nr] posonly (...)" every 10 s; start line "[nr] DM_NR_POSONLY=..."; red lines "【红】[nr] posonly: ...".
#include "../../nr/nr_posonly.h"
namespace nrpo{
int mode=-1;
struct Plan{bool on=false;uint32_t k=0,stride=0,psz=0;Buffer view{};} P;   // the draw in hand (decide() before the NR-09 lane, read by the lane and the old path)
// one copy: (mesh entry lo, stride, position offset, size) -> arena range
struct Copy{uint64_t lo=0,hi=0,off=0,bytes=0,nverts=0,tok=0,stamp=0;uint32_t stride=0,pos_off=0,psz=0,ready=0,uses=0;bool tracked=false,live=false;};
struct Key{uint64_t lo;uint32_t stride,pos_off,psz;bool operator==(const Key &o)const{return lo==o.lo && stride==o.stride && pos_off==o.pos_off && psz==o.psz;}};
struct KH{size_t operator()(const Key &k)const{return size_t((k.lo*0x9E3779B97F4A7C15ull)^(uint64_t(k.stride)<<40)^(uint64_t(k.pos_off)<<20)^k.psz);}};
std::unordered_map<Key,Copy,KH> copies;
std::unordered_map<uint64_t,std::vector<Key>> by_lo;   // mesh entry lo -> its copies (the drop paths)
std::unordered_map<Key,uint64_t,KH> would;uint64_t would_bytes=0;   // =2: the copies the window's draws would need (memory of the working set)
nrpo_arena *arena=nullptr;VkBuffer abuf{};VkDeviceMemory amem{};uint8_t *amap=nullptr;bool acoh=false;uint64_t asize=0;
struct Stats{uint64_t draws,depth,fs_ok,vs_ok,fmt_ok,eligible,not_static,unaligned,untracked,pending,nomem,applied,made,remade,dropped_write,dropped_dirty,dropped_gone,
    checks,bad,bytes_now,bytes_po,idx,unverified;} s{};
uint64_t next_ns=0,frames0=0;unsigned reds=0;
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
void red(const char *fmt,...){if(reds++>=16)return;char b[400];va_list a;va_start(a,fmt);vsnprintf(b,sizeof b,fmt,a);va_end(a);xbg_logf("【红】[nr] posonly: %s",b);}
bool on_at_load(){static int v=-1;if(v<0){const char *e=getenv("DM_NR_POSONLY");v=e && *e && atoi(e)!=0;}return v!=0;}
void drop(const Key &k,uint64_t *ctr){auto f=copies.find(k);if(f==copies.end())return;Copy &c=f->second;
    if(c.live){if(!nrpo_free(arena,c.off,c.bytes,uint64_t(slot_serial)))red("the arena's free list is full: %llu bytes lost until the renderer restarts",(unsigned long long)c.bytes);if(ctr)++*ctr;}
    copies.erase(f);}
void drop_lo(uint64_t lo,uint64_t *ctr){auto f=by_lo.find(lo);if(f==by_lo.end())return;for(const Key &k:f->second)drop(k,ctr);by_lo.erase(f);}
// nrbuf's hooks: a backend write into guest memory (snapshot_invalidate -> nrbuf::cpu_write), a mesh entry gone (nr_stab_apply / drop_pool)
void on_write(uint64_t a,uint64_t n){if(mode<=0 || copies.empty())return;
    nr_stab_each(&nrbuf::tab,a,n,[](void*,nr_sbuf *e){drop_lo(e->lo,&s.dropped_write);},nullptr);}
void on_gone(const nr_sbuf *e){if(mode>0 && !copies.empty())drop_lo(e->lo,&s.dropped_gone);}
void init(){const char *e=getenv("DM_NR_POSONLY");int m=e && *e?atoi(e):0;if(m!=1 && m!=2)m=0;
    if(m && (nrbuf::mode<=0 || !nrbuf::tab_ok)){xbg_logf("【红】[nr] DM_NR_POSONLY=%d needs NR-06's static buffer table (DM_NR=1 DM_NR_BUF=1): off",m);m=0;}
    if(m==1){asize=uint64_t(32)<<20;
        {const char *mb=getenv("DM_NR_POSONLY_MB");if(mb && *mb)asize=uint64_t(std::clamp(atoi(mb),1,512))<<20;else if(dm_nr_mem_cap)asize=std::min<uint64_t>(asize,dm_nr_mem_cap(NRM_BUFSTATIC,asize));}   // NR-29: the static share caps it
        arena=static_cast<nrpo_arena*>(calloc(1,sizeof(nrpo_arena)));
        if(!arena || !nrbuf::make(VkDeviceSize(asize),abuf,amem,amap,acoh)){red("no memory for the %llu MB arena: counting only (=2)",(unsigned long long)(asize>>20));m=2;free(arena);arena=nullptr;}
        else nrpo_arena_init(arena,asize);}
    nrbuf::po_write=on_write;nrbuf::po_gone=on_gone;
    if(m)xbg_logf("[nr] DM_NR_POSONLY=%d (NR-POSONLY: %s): depth-only draws whose fragment shader reads no varying and whose vertex shader is proved position-only "
        "(xbg_spirv_posonly), of static meshes (NR-06 table), %s; arena %llu MB",m,m==1?"on":"count only",
        m==1?"bind a compact copy of the position bytes with a one-binding pipeline":"nothing drawn differently",(unsigned long long)(asize>>20));
    mode=m;}
// The copy of entry e for this layout, valid now (made / checked). nullptr: the draw keeps the interleaved buffer.
Copy *get(nr_sbuf *e,uint32_t stride,uint32_t pos_off,uint32_t psz,bool make){
    const Key k{e->lo,stride,pos_off,psz};auto f=copies.find(k);const uint64_t bytes=e->hi-e->lo;const void *p=nrbuf::guest(e->lo);
    const uint32_t ep=nrbuf::epoch32();
    if(f!=copies.end()){Copy &c=f->second;
        if(c.ready==ep)return &c;
        // tracked: the token holds (no page left PROT since it was taken); else no page written after the stamp AND every page PROT now
        // (a page the protect skipped because it was BUSY stays writable without a newer generation: never trusted, made again -
        // tests/nr_posonly_inval_test.c found that race). CPU_NO_ACCESS pools: only the backend writes them (on_write drops the copy).
        bool ok=true;
        if(c.tracked && !(c.tok && nvn_dirty_tok_ok(c.tok))){uint64_t g=0,inc=0,tok=0;
            if(!nvn_dirty_since(p,size_t(bytes),c.stamp) && nvn_dirty_version_tok(p,size_t(bytes),&g,&inc,&tok))c.tok=tok;else{ok=false;++s.dropped_dirty;}}
        if(ok){c.ready=ep;return &c;}
        drop(k,nullptr);}   // dirty: made again below (the old range retires behind this submission)
    if(!make)return nullptr;
    // tracked pools: stamp, protect, gather (a later write faults and is newer than the stamp); CPU_NO_ACCESS pools: only the backend writes
    const bool tracked=nvn_dirty_tracked && nvn_dirty_now && nvn_dirty_protect && nvn_dirty_since && nvn_dirty_version_tok && nvn_dirty_tok_ok && nvn_dirty_tracked(p,size_t(bytes)) && !vcache::overlaps_wtrack(e->lo,bytes);
    if(!tracked){const int fl=nvn_pool_flags?nvn_pool_flags(p,size_t(bytes)):-1;if(fl<0 || !(fl&1)){++s.untracked;return nullptr;}}
    const uint64_t nv=nrpo_nverts(bytes,stride,pos_off,psz);if(!nv)return nullptr;
    const uint64_t need=nv*psz+16;   // +16: the dead inputs at offset 0 of the last vertex may read past its position
    recording_slot();   // a range freed by this serial is reused only after it retired (delay below)
    const uint64_t off=nrpo_alloc(arena,need,uint64_t(slot_serial),3);if(off==~0ull){++s.nomem;return nullptr;}
    Copy c;c.lo=e->lo;c.hi=e->hi;c.off=off;c.bytes=need;c.nverts=nv;c.stride=stride;c.pos_off=pos_off;c.psz=psz;c.tracked=tracked;c.live=true;
    if(tracked){c.stamp=nvn_dirty_now();nvn_dirty_protect(p,size_t(bytes));}
    {evguard::EngineRead er(p,size_t(bytes));nrpo_gather(amap+off,static_cast<const uint8_t*>(p),nv,stride,pos_off,psz);}
    memset(amap+off+nv*psz,0,16);if(!acoh)nrbuf::flush(amem,off,need);
    if(tracked){uint64_t g=0,inc=0,tok=0;   // every page PROT after the gather (none skipped, none written meanwhile): else not bound now, made again at the next use
        if(!nvn_dirty_version_tok(p,size_t(bytes),&g,&inc,&tok)){nrpo_free(arena,off,need,uint64_t(slot_serial));++s.unverified;return nullptr;}c.tok=tok;}
    c.ready=ep;++s.made;
    auto r=copies.emplace(k,c);by_lo[e->lo].push_back(k);return &r.first->second;}
// draw(), right before the NR-09 lane: the plan of this draw (P). colors: the active colour targets; special: GPU-indirect / quads.
void decide(uint32_t colors,bool special,int count,const p_draw_idx *indexed,uint32_t instances){
    P.on=false;if(mode<0)init();if(mode<=0)return;
    ++s.draws;if(colors || !state.rt.depth || special)return;++s.depth;
    const Shader *vs=state.shader[0],*fs=state.shader[4];
    if(!vs || state.shader[1] || state.shader[2] || state.shader[3])return;
    if(fs && fs->vo_read!=0)return;++s.fs_ok;
    if(vs->po!=1)return;++s.vs_ok;
    va_st a;memcpy(&a,&state.attrib[0],4);if(!a.fmt || a.stream>=16)return;
    const VkFormat vf=format(a.fmt);const uint32_t psz=nrpo_fmt_size(uint32_t(vf));if(!psz || !(format_properties(vf).bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))return;
    const uint32_t k=a.stream,stride=state.stream[k].stride;if(state.stream[k].divisor || stride<=psz || a.offset+psz>stride)return;
    for(uint32_t loc:vs->inputs)if(loc<16){va_st b;memcpy(&b,&state.attrib[loc],4);if(b.fmt && b.stream<16 && state.stream[b.stream].divisor)return;}   // an instanced input: its binding must stay
    ++s.fmt_ok;
    const uint64_t n=uint64_t(uint32_t(count))*(indexed?uint64_t(indexed->instances):instances);
    ++s.eligible;s.idx+=n;s.bytes_now+=n*stride;s.bytes_po+=n*psz;
    const NVNbufferRange r=state.vb[k];if(!(r.address && r.size))return;
    nr_sbuf *e=nrbuf::tab.n?nr_stab_find(&nrbuf::tab,r.address,r.size):nullptr;
    if(!e || e->kind!=NRS_KIND_VB || e->cls!=NRS_ELIGIBLE){++s.not_static;return;}
    if((r.address-e->lo)%stride){++s.unaligned;return;}
    if(!pending_writebacks.empty() && overlaps_pending(r.address,r.size)){++s.pending;return;}
    if(mode!=1){{const void *gp=nrbuf::guest(e->lo);const bool tr=nvn_dirty_tracked && nvn_dirty_tracked(gp,size_t(e->hi-e->lo)) && !vcache::overlaps_wtrack(e->lo,e->hi-e->lo);
        if(!tr){const int fl=nvn_pool_flags?nvn_pool_flags(gp,size_t(e->hi-e->lo)):-1;if(fl<0 || !(fl&1)){++s.untracked;return;}}}
        ++s.applied;const uint64_t nv=nrpo_nverts(e->hi-e->lo,stride,a.offset,psz);if(nv && would.emplace(Key{e->lo,stride,a.offset,psz},nv*psz).second)would_bytes+=nv*psz+16;return;}
    Copy *c=get(e,stride,a.offset,psz,true);if(!c)return;
    const uint64_t first=(r.address-e->lo)/stride;if(first>=c->nverts)return;
    // 1 in 64 uses: the copy against a fresh gather of the guest bytes
    if(!(++c->uses&63)){++s.checks;static std::vector<uint8_t> t;t.resize(size_t(c->nverts*psz));
        {evguard::EngineRead er(nrbuf::guest(c->lo),size_t(c->hi-c->lo));nrpo_gather(t.data(),static_cast<const uint8_t*>(nrbuf::guest(c->lo)),c->nverts,stride,a.offset,psz);}
        if(memcmp(t.data(),amap+c->off,t.size())){++s.bad;red("the copy of %#llx+%llu (stride %u, position +%u, %u B) differs from the guest bytes: dropped",(unsigned long long)c->lo,(unsigned long long)(c->hi-c->lo),stride,a.offset,psz);
            drop(Key{e->lo,stride,a.offset,psz},nullptr);return;}}
    P.on=true;P.k=k;P.stride=stride;P.psz=psz;P.view=Buffer{};P.view.buffer=abuf;P.view.memory=amem;P.view.offset=c->off+first*psz;P.view.size=(c->nverts-first)*psz;
    P.view.mapped=amap+P.view.offset;P.view.coherent=acoh;++s.applied;}
// draw()'s old path, the vertex input of a planned draw: every non-constant attribute on binding P.k at offset 0 (the position is at 0 of the
// copy; the other inputs are dead), the one binding with stride P.psz.
void rewrite(std::vector<VkVertexInputAttributeDescription> &attrs,VkVertexInputBindingDescription slots[16],uint32_t &used,int zero){
    for(auto &x:attrs)if(int(x.binding)!=zero){x.binding=P.k;x.offset=0;}
    used=(1u<<P.k)|(zero>=0 && zero<16?1u<<zero:0u);slots[P.k]={P.k,P.psz,VK_VERTEX_INPUT_RATE_VERTEX};}
void tick(){if(mode<=0)return;const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;frames0=stats.presents.load();return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const uint64_t f1=stats.presents.load();const double fr=f1>frames0?double(f1-frames0):1.,mb=1048576.;frames0=f1;
    uint64_t cmem=0;for(auto &c:copies)cmem+=c.second.bytes;
    if(mode==2){cmem=would_bytes;}
    xbg_logf("[nr] posonly (DM_NR_POSONLY=%d, NR-POSONLY, last 10 s, %.0f frames): draws %.1f a frame, depth-only %.1f, fragment reads no varying %.1f, vertex shader position-only %.1f, "
        "position format / stride fit %.1f = eligible (%.1f M indices a frame: vertex bytes %.2f MB now -> %.2f MB with the copies, %.2fx); not a static mesh %.1f, unaligned %.1f, "
        "untracked pool %.1f, pending writeback %.1f, arena full %llu; %s %.1f a frame; copies %zu (%.2f MB%s of %llu MB, arena peak %.2f MB, reused ranges %llu), made %llu (not verified at the making %llu), dropped: "
        "backend write %llu, dirty pages %llu, mesh gone %llu; checks %llu, bad %llu",
        mode,fr,s.draws/fr,s.depth/fr,s.fs_ok/fr,s.vs_ok/fr,s.eligible/fr,s.idx/fr/1e6,s.bytes_now/fr/mb,s.bytes_po/fr/mb,s.bytes_po?double(s.bytes_now)/double(s.bytes_po):0.,
        s.not_static/fr,s.unaligned/fr,s.untracked/fr,s.pending/fr,(unsigned long long)s.nomem,mode==1?"applied":"would apply (needs =1)",s.applied/fr,mode==2?would.size():copies.size(),cmem/mb,mode==2?" the window's meshes would need":"",
        (unsigned long long)(asize>>20),arena?arena->peak/mb:0.,arena?(unsigned long long)arena->reused:0ull,(unsigned long long)s.made,(unsigned long long)s.unverified,(unsigned long long)s.dropped_write,
        (unsigned long long)s.dropped_dirty,(unsigned long long)s.dropped_gone,(unsigned long long)s.checks,(unsigned long long)s.bad);
    s=Stats{};if(mode==2){would.clear();would_bytes=0;}}
// shutdown (the queue idle)
void release(){if(abuf){vkDestroyBuffer(cfg.device,abuf,nullptr);vkFreeMemory(cfg.device,amem,nullptr);}abuf={};amem={};amap=nullptr;free(arena);arena=nullptr;
    copies.clear();by_lo.clear();if(mode>0)mode=-1;nrbuf::po_write=nullptr;nrbuf::po_gone=nullptr;}
}
