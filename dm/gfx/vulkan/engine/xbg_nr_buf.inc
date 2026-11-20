// [DOS2] NR-06 (docs/dos2/nr/NR-06.md, NATIVE_RENDERER_PLAN.md S2b): DM_NR_BUF - engine buffers without per-frame snapshots. Included by
// xbg_game.cpp at file scope right after the zero-copy namespace (zc) and before snapshot(); decisions in dm/gfx/nr/nr_buf.h (pure C, host
// test tests/nr_buf_test.c). Needs DM_NR=1 / 2: the NR-01 hooks (dm/host/hle_dos2_nr.c) hand over the engine's vertex / index buffers
// (xbg_nr_buf_event) and the device's constant ring (xbg_nr_ring_set). Read at the first guest submission.
//
//   DM_NR_BUF        0 (default) nothing here runs: one load and a not-taken branch in uniform_snapshot, snapshot and snapshot_invalidate.
//                    1 on, by capability (xbg_vendor_policy.h xbg_nr_buf_ring_inplace / xbg_nr_buf_static_default):
//                      ring   a uniform range inside the constant ring is served from ONE buffer at (address - ring base):
//                             zero-copy on (Adreno): in place through the zero-copy chunk that holds it (no zcubo bypass, no snapshot
//                             map / front / copy; DM_NB_UBO=1 did this only for ranges an OP_X_NB_DRAW of the submission bound);
//                             zero-copy off (Mali, PowerVR, Xclipse, MediaTek...): the MIRROR - one VkBuffer of ring size + 64 KB, the
//                             256-byte granules of a bound range copied in the first time an epoch binds them (nr_ring_*: a granule a
//                             recorded / flying submission reads is never overwritten - compared instead; a CPU write in the stream
//                             holds it on the old path until those readers retired). Copy volume = the ring's increment.
//                      static static vertex / index buffers (usage != 3, with initial data) are served from an immutable copy, made at
//                             the first draw that binds them (a chunked arena, budget DM_NR_BUF_MB, default 192); the dirty-page tracker
//                             (nvn_dirty.c, as vcache) guards it: a write to its pages after the copy, a CPU write in the stream or a GPU
//                             writeback over it demotes the buffer to the old path for good. Default: only without zero-copy (with it,
//                             the old path binds the guest memory in place already).
//                    2 compute + execute old + compare: the old path is used; the new view is built too and its bytes compared with
//                      the old path's (ring: the uniform block; static: the bound vertex / index range; at most DM_NR_BUF_CMP_KB per
//                      submission, default 1024) and the creation hash of a static buffer with its bytes at its first draw ("written
//                      between creation and first draw"). Mismatches are red lines (first 16) and counted; what the old path did
//                      instead (zero-copy view, sticky miss, snapshot front / map, vcache, copied bytes) is counted too.
//                    3 = 1 with a poisoned mirror (0xFF bytes = NaN until a granule is copied: a bind that skipped its copy shows).
//   DM_NR_BUF_RING   0 off, 1 (default) by capability, 2 mirror even with zero-copy (A/B on Adreno), 3 in place only (no mirror).
//   DM_NR_BUF_STATIC 0 off, 1 (default) by capability, 2 on even with zero-copy.
//   Line "[nr] buf: ..." every 10 s; "[nr] DM_NR_BUF=..." at the start.
//
// API for NR-08 (descriptors: one UBO set per layout, dynamic offsets only):
//   nrbuf::ring_vk()          the VkBuffer every ring UBO the ring path serves lives in (the mirror, or the zero-copy chunk holding the
//                             whole ring); VK_NULL_HANDLE while the ring path is off or the ring is not inside one chunk.
//   nrbuf::ring_slack(off)    bytes a descriptor may cover from offset `off` of ring_vk() (the mirror has 64 KB past the ring).
//   nrbuf::ubo(a, copy, z)    1 = z is a view of [a, a + uniform_range(copy)) at z.offset inside ring_vk() (z.buffer == ring_vk()),
//                             aligned to minUniformBufferOffsetAlignment: a dynamic offset; 0 = not served (use uniform_snapshot's old
//                             path); 2 = mode 2 (z built, the caller runs the old path). Translation thread (the engine mutex) only.
//   Everything else here is private.
#include "../../nr/nr_buf.h"
size_t uniform_range(size_t copy);
extern std::map<uintptr_t,size_t> pools;
namespace nrbuf {
void (*po_write)(uint64_t a,uint64_t n)=nullptr;void (*po_gone)(const nr_sbuf *e)=nullptr;bool po_tab=false;   // [DOS2] NR-POSONLY (xbg_nr_posonly.inc): its hooks; it keeps the static table on without static copies
int mode=-1,ring_mode=0,st_mode=0;bool busy=false;   // ring_mode: 0 off, 1 in place, 2 mirror; busy: mode 2 recursion guard
enum{RING_INPLACE=1,RING_MIRROR=2};
uint64_t budget=192ull<<20,cmp_budget=1ull<<20,cmp_left=0;
// [DOS2] NR-29 DM_NR_MEM: a trim asks (any thread: trim_req = its tier), the next submission frees every static copy (trim()); the
// usage is published for the 10 s line (pub_*: arena and mirror bytes after each submission).
std::atomic<int> trim_req{0};std::atomic<uint64_t> pub_arena{0},pub_mirror{0};uint64_t trims=0,trim_bytes=0;
// ---- host events (any thread) ----
struct Ev{int ev,kind,cls;uint64_t handle,mem,size,hash;};
std::mutex ev_mu;std::vector<Ev> evq;std::atomic<bool> ev_any{false};uint64_t ev_lost=0;
struct RingPub{uint64_t base=0,size=0,head=0;bool fresh=false;} ring_pub;   // ev_mu
// ---- the ring ----
nr_ring ring{};VkBuffer mirror{};VkDeviceMemory mirror_mem{};uint8_t *mirror_map=nullptr;bool mirror_coherent=true;VkDeviceSize mirror_size=0;
uint64_t ring_base=0,ring_size=0,head_addr=0;uint32_t head_prev=0;bool head_seen=false;
zc::Chunk *ch=nullptr;uint64_t ch_gen=0;
// ---- static copies ----
nr_stab tab{};bool tab_ok=false;
struct Chunk{VkBuffer buffer{};VkDeviceMemory memory{};uint8_t *mapped=nullptr;VkDeviceSize size=0,used=0,live=0;bool coherent=true,dead=false;};
struct Copy{uint32_t chunk=0;VkDeviceSize off=0,bytes=0;bool live=false;};
std::vector<Chunk> chunks;std::vector<Copy> copies;std::vector<int32_t> free_copies;VkDeviceSize arena_bytes=0;int cur_chunk[2]={-1,-1};
VkDeviceSize chunk_min=16ull<<20;   // DM_NR_BUF_CHUNK_MB (16): fewer, larger chunks = more meshes behind one bind with the NR-09 folds
// [DOS2] NR-09 (DM_NR_DRAW with its folds; 0 = NR-06 as it was: one chunk sequence, copies on 256 bytes). nrdraw folds a vertex stream's
// offset into the draw's vertexOffset and an index buffer's into firstIndex, so two meshes bind the same (VkBuffer, offset) when they
// share a chunk and their offsets agree modulo the stride / index size. place bit 0: index copies get their own chunk sequence (all
// static index data in one or two VkBuffers: the index bind repeats); bit 1: a vertex copy starts on a multiple of the stride its first
// draw binds it with (stride_hint, set by the vertex-stream snapshot() callers around their call), so every mesh of a chunk folds to
// offset 0 whatever its stride.
int place=0;uint32_t stride_hint=0;
// [DOS2] NR-ZERO Z3c: st_ep moves whenever the answer of vb() for a range that was served may change without the snapshot epoch or
// the pending writebacks moving: a demote, a freed copy, a released chunk, a trim, any change of the table (add / kill / drop / apply).
uint64_t st_ep=1;
inline bool is_cur(uint32_t i){return int(i)==cur_chunk[0] || int(i)==cur_chunk[1];}
// ---- counters (the 10 s window) ----
enum{W_NOTRING,W_SPAN,W_HOLD,W_RUNS,W_PEND,W_ALIGN,W_UI,W_CHUNK,W_STICKY,W_RACE,W_N};
enum{S_NOT,S_DYN,S_NOINIT,S_DEMOTED,S_NOBUDGET,S_PEND,S_ALIGN,S_N};
struct Stats{uint64_t calls=0,ring=0,mirror=0,inplace=0,why[W_N]{},copies=0,copied=0,cmp_bytes=0,races=0,cpu_holds=0,adv=0,
    sbinds=0,served=0,swhy[S_N]{},filled=0,filled_bytes=0,demote_cpu=0,demote_dirty=0,demote_pool=0,nobudget=0,
    ring_cmp=0,ring_bad=0,st_cmp=0,st_bad=0,st_cmp_bytes=0,born_dirty=0,born_checked=0,
    old_zc=0,old_sticky=0,old_front=0,old_map=0,old_vcache=0,old_bytes=0,old_calls=0,submissions=0,timed=0,ns=0;} s;
uint64_t next_ns=0;unsigned told=0;
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
// note: the safety net working as designed (a static copy given up; counted in the summary's 'demoted:'), first 16 lines, not red.
unsigned noted=0;
void note(const char *fmt,...) __attribute__((format(printf,1,2)));
void note(const char *fmt,...){if(noted>=16)return;++noted;char b[512];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof b,fmt,ap);va_end(ap);xbg_logf("[nr] buf: %s (frame %llu)",b,(unsigned long long)stats.presents.load());}
void red(const char *fmt,...){if(told>=16)return;++told;char b[512];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof b,fmt,ap);va_end(ap);xbg_logf("【红】[nr] buf: %s (frame %llu)",b,(unsigned long long)stats.presents.load());}
inline uint32_t epoch32(){const uint32_t e=uint32_t(snapshot_epoch);return e?e:1;}

// One host-visible buffer (device-local preferred). false = no memory (the caller stays on the old path).
bool make(VkDeviceSize n,VkBuffer &b,VkDeviceMemory &m,uint8_t *&p,bool &coherent){
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=n;ci.usage=arena_usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(cfg.device,&ci,nullptr,&b)!=VK_SUCCESS){b={};return false;}
    VkMemoryRequirements r;vkGetBufferMemoryRequirements(cfg.device,b,&r);
    const uint32_t type=find_memory(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    coherent=(memories.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=type;void *q=nullptr;
    if(vkAllocateMemory(cfg.device,&ai,nullptr,&m)!=VK_SUCCESS){vkDestroyBuffer(cfg.device,b,nullptr);b={};m={};return false;}
    if(vkBindBufferMemory(cfg.device,b,m,0)!=VK_SUCCESS || vkMapMemory(cfg.device,m,0,VK_WHOLE_SIZE,0,&q)!=VK_SUCCESS){vkDestroyBuffer(cfg.device,b,nullptr);vkFreeMemory(cfg.device,m,nullptr);b={};m={};return false;}
    p=static_cast<uint8_t*>(q);return true;}
void flush(VkDeviceMemory m,VkDeviceSize off,VkDeviceSize n){
    const VkDeviceSize a=std::max<VkDeviceSize>(1,properties.limits.nonCoherentAtomSize),lo=off/a*a;
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};r.memory=m;r.offset=lo;r.size=VK_WHOLE_SIZE;(void)n;vkFlushMappedMemoryRanges(cfg.device,1,&r);}
inline const void *guest(uint64_t a){return reinterpret_cast<const void*>(uintptr_t(a));}

// ---- the ring ----
void ring_off(const char *why){
    if(mirror){VkBuffer b=mirror;VkDeviceMemory m=mirror_mem;retire_handle([b,m]{vkDestroyBuffer(cfg.device,b,nullptr);vkFreeMemory(cfg.device,m,nullptr);});++memo_epoch;}
    mirror={};mirror_mem={};mirror_map=nullptr;mirror_size=0;nr_ring_free(&ring);ring_base=ring_size=0;ch=nullptr;
    if(why)xbg_logf("[nr] buf: ring path off: %s",why);}
void ring_setup(uint64_t base,uint64_t size,uint64_t head){
    if(ring_base)ring_off(nullptr);
    head_addr=head;head_seen=false;
    bool in_pool=false;for(auto &p:pools)if(base>=p.first && base+size<=p.first+p.second){in_pool=true;break;}
    if(!in_pool){static unsigned tries=0;   // the pool's registration is queued on this thread too: try again at the next submissions
        if(++tries<=8){std::lock_guard<std::mutex> l(ev_mu);if(!ring_pub.fresh){ring_pub.fresh=true;ev_any.store(true);}return;}
        red("constant ring %#llx+%#llx lies in no memory pool: ring path off",(unsigned long long)base,(unsigned long long)size);return;}
    if(ring_mode==RING_INPLACE){
        zc::Chunk *c=zc::find(base,size);
        if(!c){xbg_logf("[nr] buf: constant ring %#llx+%#llx is not inside one zero-copy chunk: in place per range (ring_vk() stays null)",(unsigned long long)base,(unsigned long long)size);}
        ring_base=base;ring_size=size;
        xbg_logf("[nr] buf: constant ring %#llx+%#llx served in place (zero-copy chunk %#llx)",(unsigned long long)base,(unsigned long long)size,(unsigned long long)(c?c->lo:0));return;}
    if(nr_ring_init(&ring,base,size)){red("no memory for the ring granule tables: ring path off");return;}
    bool coh=true;mirror_size=size+65536;
    if(!make(mirror_size,mirror,mirror_mem,mirror_map,coh)){nr_ring_free(&ring);red("no memory for the %.1f MB ring mirror: ring path off",mirror_size/1048576.0);mirror_size=0;return;}
    mirror_coherent=coh;memset(mirror_map,mode==3?0xff:0,size_t(mirror_size));if(!coh)flush(mirror_mem,0,mirror_size);
    ring_base=base;ring_size=size;++memo_epoch;
    xbg_logf("[nr] buf: constant ring %#llx+%#llx mirrored into one %.1f MB buffer (%s, %s)",(unsigned long long)base,(unsigned long long)size,mirror_size/1048576.0,
             coh?"coherent":"flushed per copy",mode==3?"poisoned 0xFF until copied":"zero until copied");}
[[maybe_unused]] VkBuffer ring_vk(){
    if(ring_mode==RING_MIRROR)return mirror;
    if(ring_mode==RING_INPLACE && ring_base){zc::Chunk *c=zc::find(ring_base,ring_size);return c?c->buffer:VkBuffer{};}
    return VkBuffer{};}
[[maybe_unused]] VkDeviceSize ring_slack(VkDeviceSize off){
    if(ring_mode==RING_MIRROR)return off<mirror_size?mirror_size-off:0;
    if(ring_mode==RING_INPLACE && ring_base){zc::Chunk *c=zc::find(ring_base,ring_size);if(c){const uint64_t base_off=ring_base-c->lo;return base_off+off<c->n?c->n-base_off-off:0;}}
    return 0;}
// [DOS2] NR-ZERO Z3d (DM_NR_Z3D, nr_z3e.h nrz_ring): the constant ring served in place as ONE word. zd.r.ok = the whole ring lies in one
// live zero-copy chunk, no other live chunk meets it and none of its pages is sticky - then ubo() below answers every aligned block
// inside the ring with (address - chunk base) unless a writeback is pending or the UI shift is on. Valid while the chunk liveness epoch,
// that chunk's sticky version and the ring's base / size stand; made again (one scan of the ring's page bytes) when one moved.
struct ZD{nrz_ring r{};uint64_t live_ep=0,st_ver=0,size=0;zc::Chunk *c=nullptr;VkBuffer buf{};} zd;
bool zd_remake(){
    zd.live_ep=zc::live_ep;zd.r=nrz_ring{};zd.r.lo=ring_base;zd.size=ring_size;zd.r.hi=ring_base+ring_size;zd.c=nullptr;zd.buf={};++nrzx::s.d_reval;
    if(ring_mode!=RING_INPLACE || !ring_base || zc::enabled<=0)return false;
    zc::Chunk *c=zc::find(ring_base,ring_size);if(!c)return false;
    const int n=zc::count.load(std::memory_order_acquire);   // the holder of every ring address is c only when no other live chunk meets the ring
    for(int i=0;i<n;i++){const zc::Chunk &o=zc::table[i];if(&o!=c && o.live && o.lo<zd.r.hi && zd.r.lo<o.lo+o.n)return false;}
    zd.c=c;zd.st_ver=c->st_ver;zd.buf=c->buffer;zd.r.chunk_lo=c->lo;zd.r.align=properties.limits.minUniformBufferOffsetAlignment;
    zd.r.ok=nrz_ring_clean(c->sticky.data(),c->lo,c->n,zd.r.lo,zd.r.hi);
    return zd.r.ok!=0;}
inline bool zd_valid(){
    if(zd.live_ep==zc::live_ep && zd.r.lo==ring_base && zd.size==ring_size && (!zd.c || zd.st_ver==zd.c->st_ver))return zd.r.ok!=0;
    return zd_remake();}
// 1 = ubo(address, copy, z) would answer 1 with z.buffer == zd.buf and z.offset == off (count: its counters are kept as ubo keeps them);
// 0 = ask ubo().
inline int ubo_fast(uint64_t address,size_t copy,uint64_t &off,bool count){
    ++nrzx::s.d_asked;
    if(mode!=1 && mode!=3)return 0;
    if(!zd_valid() || g_ui_moff!=0 || !pending_writebacks.empty())return 0;
    if(!nrz_ring_off(&zd.r,address,uniform_range(copy),&off))return 0;
    ++nrzx::s.d_fast;if(count){++s.calls;++s.ring;++s.inplace;++zc::hits;}
    return 1;}
// uniform_snapshot's first step (see the header): 0 old path, 1 served, 2 mode 2 (z built).
int ubo(uint64_t address,size_t copy,Buffer &z){
    ++s.calls;
    if(!ring_base || address<ring_base || address-ring_base>=ring_size){++s.why[W_NOTRING];return 0;}
    ++s.ring;const bool timed=!(s.ring&63);const uint64_t t0=timed?timing_clock():0;int why=-1;
    const size_t n=uniform_range(copy);const VkDeviceSize align=properties.limits.minUniformBufferOffsetAlignment;
    if(g_ui_moff!=0 && address==g_ui_moff_addr)why=W_UI;
    else if(!pending_writebacks.empty() && overlaps_pending(address,n))why=W_PEND;
    else if(ring_mode==RING_INPLACE){
        zc::Chunk *c=ch;if(!c || !c->live || c->gen!=ch_gen || address<c->lo || address-c->lo+n>c->n){c=zc::find(address,n);ch=c;ch_gen=c?c->gen:0;}
        uint64_t off=0;const int v=c?nb_ubo_view(address,n,c->lo,c->n,align,c->sticky.data(),&off):NBU_CHUNK;
        if(v==NBU_DIRECT){z=Buffer{};z.buffer=c->buffer;z.memory=c->memory;z.offset=off;z.size=n;z.mapped=const_cast<void*>(guest(address));z.coherent=true;++s.inplace;if(mode!=2)++zc::hits;}
        else why=v==NBU_ALIGN?W_ALIGN:v==NBU_STICKY?W_STICKY:W_CHUNK;}
    else{const uint64_t off=address-ring_base;
        if(align>1 && off%align)why=W_ALIGN;
        else{nr_ring_work w;const uint32_t ep=epoch32(),se=uint32_t(slot_serial);const int r=nr_ring_check(&ring,address,n,ep,se,&w);
            if(r==NRR_WORK){bool copies_due=false;for(int i=0;i<w.n;i++)copies_due|=w.run[i].kind==NRR_COPY;
                if(copies_due)recording_slot();   // the slot that last ran serial - 2 retires before its mirror granules are rewritten
                for(int i=0;i<w.n && why<0;i++){const nr_ring_run &u=w.run[i];const uint64_t lo=nr_ring_goff(u.g0),hi=std::min<uint64_t>(nr_ring_goff(u.g1),ring_size);
                    if(u.kind==NRR_COPY){{evguard::EngineRead er(guest(ring_base+lo),size_t(hi-lo));memcpy(mirror_map+lo,guest(ring_base+lo),size_t(hi-lo));}
                        if(!mirror_coherent)flush(mirror_mem,lo,hi-lo);++s.copies;s.copied+=hi-lo;nr_ring_done(&ring,u.g0,u.g1,ep,se);}
                    else{s.cmp_bytes+=hi-lo;bool same;{evguard::EngineRead er(guest(ring_base+lo),size_t(hi-lo));same=!memcmp(mirror_map+lo,guest(ring_base+lo),size_t(hi-lo));}
                        if(same)nr_ring_done(&ring,u.g0,u.g1,ep,se);
                        else{nr_ring_race(&ring,u.g0,u.g1,se);++s.races;why=W_RACE;
                            red("ring bytes at %#llx+%llu changed while a recorded / flying submission reads its mirror copy (serial %llu): old path until it retired",(unsigned long long)(ring_base+lo),(unsigned long long)(hi-lo),(unsigned long long)slot_serial);}}}}
            else if(r!=NRR_VALID)why=r==NRR_SPAN?W_SPAN:r==NRR_HOLD?W_HOLD:r==NRR_RUNS_OVER?W_RUNS:W_NOTRING;
            if(why<0){z=Buffer{};z.buffer=mirror;z.memory=mirror_mem;z.offset=off;z.size=n;z.mapped=mirror_map+off;z.coherent=mirror_coherent;++s.mirror;}}}
    if(timed){s.ns+=timing_clock()-t0;++s.timed;}
    if(why>=0){++s.why[why];return 0;}
    if(mode==2){s.old_calls++;s.old_zc-=zc::hits;s.old_front-=snap_front_hits;s.old_map-=snap_map_hits;s.old_bytes-=perf.snapshot_bytes;return 2;}
    return 1;}
// mode 2, after the old path made b: the bytes a draw would read through z must be the old path's
void ubo_verify(uint64_t address,size_t copy,const Buffer &z,const Buffer &b){
    s.old_zc+=zc::hits;s.old_front+=snap_front_hits;s.old_map+=snap_map_hits;s.old_bytes+=perf.snapshot_bytes;
    if(!b.mapped || !z.mapped || cmp_left<copy)return;
    cmp_left-=copy;++s.ring_cmp;
    if(memcmp(z.mapped,b.mapped,copy)){++s.ring_bad;size_t i=0;while(i<copy && static_cast<const uint8_t*>(z.mapped)[i]==static_cast<const uint8_t*>(b.mapped)[i])++i;
        red("ring view of %#llx+%zu differs from the old path's bytes at +%zu (%s)",(unsigned long long)address,copy,i,ring_mode==RING_MIRROR?"mirror":"in place");}}
// A CPU write in the stream / a GPU writeback over [a, a + n) (snapshot_invalidate): mirror granules re-copy or hold; static buffers demote.
void demote(nr_sbuf *e,const char *why);
void cpu_write(uint64_t a,uint64_t n);
void cpu_write_hook(uint64_t a,uint64_t n){if(mode>0)cpu_write(a,n);}   // snapshot_invalidate (declared there: it comes before this file)
void cpu_write(uint64_t a,uint64_t n){
    if(ring_mode==RING_MIRROR && ring.ngran)s.cpu_holds+=nr_ring_cpu_write(&ring,a,n,uint32_t(slot_serial));
    if(st_mode>0 && tab.n)nr_stab_each(&tab,a,n,[](void*,nr_sbuf *e){if(e->phase!=NRS_DEMOTED && e->cls==NRS_ELIGIBLE){++s.demote_cpu;demote(e,"a CPU write / GPU writeback in the stream");}},nullptr);
    if(po_write && tab.n)po_write(a,n);}   // [DOS2] NR-POSONLY: its copies of those meshes go

// ---- static copies ----
void chunk_release(uint32_t i){Chunk &c=chunks[i];if(c.dead)return;c.dead=true;++st_ep;arena_bytes-=c.size;VkBuffer b=c.buffer;VkDeviceMemory m=c.memory;
    retire_handle([b,m]{vkDestroyBuffer(cfg.device,b,nullptr);vkFreeMemory(cfg.device,m,nullptr);});c.buffer={};c.memory={};c.mapped=nullptr;for(int &cc:cur_chunk)if(int(i)==cc)cc=-1;}
void copy_free(int32_t slot){if(slot<0 || size_t(slot)>=copies.size() || !copies[slot].live)return;Copy &k=copies[slot];k.live=false;++st_ep;
    Chunk &c=chunks[k.chunk];c.live-=k.bytes;if(!c.live && !is_cur(k.chunk))chunk_release(k.chunk);free_copies.push_back(slot);}
void demote(nr_sbuf *e,const char *why){e->phase=NRS_DEMOTED;++st_ep;if(e->slot>=0){copy_free(e->slot);e->slot=-1;}
    note("static %s buffer %#llx (%#llx+%llu) demoted to the old path: %s",e->kind==NRS_KIND_VB?"vertex":"index",(unsigned long long)e->handle,(unsigned long long)e->lo,(unsigned long long)(e->hi-e->lo),why);}
// A copy of n bytes: -1 = over budget / no memory
// seq: the chunk sequence (0; 1 = index copies with place bit 0); mult > 1: the copy starts on a multiple of mult (place bit 1: the stride).
int32_t copy_alloc(VkDeviceSize n,int seq=0,VkDeviceSize mult=1){
    const VkDeviceSize a=256;VkDeviceSize need=(n+a-1)/a*a;int &cur=cur_chunk[seq];
    if(mult>1 && mult<=4096){if(mult&3)mult*=4;need+=mult;}else mult=1;   // a multiple of 4 too (snapshot()'s alignment); room for the lead
    if(cur<0 || chunks[cur].used+need>chunks[cur].size){
        const VkDeviceSize sz=std::max(chunk_min,need);if(arena_bytes+sz>budget)return -1;
        if(cur>=0 && !chunks[cur].live){const int old=cur;cur=-1;chunk_release(uint32_t(old));}
        Chunk c;c.size=sz;if(!make(sz,c.buffer,c.memory,c.mapped,c.coherent))return -1;
        size_t i=0;while(i<chunks.size() && !chunks[i].dead)++i;if(i==chunks.size())chunks.push_back(c);else chunks[i]=c;
        cur=int(i);arena_bytes+=sz;}
    Chunk &c=chunks[cur];int32_t slot;if(!free_copies.empty()){slot=free_copies.back();free_copies.pop_back();}else{slot=int32_t(copies.size());copies.emplace_back();}
    Copy &k=copies[slot];k.chunk=uint32_t(cur);k.off=mult>1?(c.used+mult-1)/mult*mult:c.used;k.bytes=need;k.live=true;c.used+=need;c.live+=need;return slot;}
// The first bind of an entry in this epoch: its copy made (first time) or checked. false = old path for this bind.
bool first_use(nr_sbuf *e){
    const uint64_t bytes=e->hi-e->lo;const void *p=guest(e->lo);
    if(e->phase==NRS_NOBUDGET){if(slot_serial<e->stamp){++s.swhy[S_NOBUDGET];return false;}e->phase=NRS_NEW;}
    if(e->slot<0){
        recording_slot();   // a chunk released by a demote / destroy of this serial retires behind it (retire_handle)
        const int32_t slot=copy_alloc(bytes,(place&1) && e->kind!=NRS_KIND_VB?1:0,(place&2) && e->kind==NRS_KIND_VB?VkDeviceSize(stride_hint):1);   // [DOS2] NR-09: placement for the folds (place 0: as NR-06)
        if(slot<0){e->phase=NRS_NOBUDGET;e->stamp=slot_serial+600;++s.nobudget;++s.swhy[S_NOBUDGET];return false;}
        Copy &k=copies[slot];Chunk &c=chunks[k.chunk];
        const bool tracked=nvn_dirty_tracked && nvn_dirty_now && nvn_dirty_protect && nvn_dirty_since && nvn_dirty_tracked(p,size_t(bytes)) && !vcache::overlaps_wtrack(e->lo,bytes);
        uint64_t stamp=0;int whole=1;if(tracked){if(dirty_safe())stamp=nvn_dirty_stamp_protect(p,size_t(bytes),&whole);else{stamp=nvn_dirty_now();nvn_dirty_protect(p,size_t(bytes));}}   // stamp, protect (until all read-only: NR-TRAFFIC), then copy: a later write faults and is newer
        {evguard::EngineRead er(p,size_t(bytes));memcpy(c.mapped+k.off,p,size_t(bytes));}
        if(!c.coherent)flush(c.memory,k.off,bytes);
        e->slot=slot;e->stamp=tracked?stamp+1:0;e->phase=NRS_READY;++s.filled;s.filled_bytes+=bytes;
        if(tracked && !whole){++s.demote_dirty;++s.swhy[S_DEMOTED];demote(e,"its pages never all became read-only after the copy (busy / VMA limit)");return false;}   // [DOS2] NR-TRAFFIC
        if(mode==2 && e->hash){++s.born_checked;if((nr_fnv64(p,size_t(bytes))|1)!=e->hash){++s.born_dirty;
            red("static %s buffer %#llx (%#llx+%llu) was written between its creation and its first draw",e->kind==NRS_KIND_VB?"vertex":"index",(unsigned long long)e->handle,(unsigned long long)e->lo,(unsigned long long)bytes);}}}
    else if(e->stamp && nvn_dirty_since(p,size_t(bytes),e->stamp-1)){++s.demote_dirty;++s.swhy[S_DEMOTED];demote(e,"its pages were written after the copy (dirty-page tracker)");return false;}
    e->ready=epoch32();return true;}
// snapshot()'s first step for vertex / index ranges (snap_site 1 / 2): 0 old path, 1 served, 2 mode 2 (z built).
int vb(uint64_t address,size_t bytes,VkDeviceSize align,Buffer &z){
    ++s.sbinds;nr_sbuf *e=tab.n?nr_stab_find(&tab,address,bytes):nullptr;int why=-1;
    if(!e)why=S_NOT;
    else if(e->cls!=NRS_ELIGIBLE)why=e->cls==NRS_DYNAMIC?S_DYN:S_NOINIT;
    else if(e->phase==NRS_DEMOTED)why=S_DEMOTED;
    else if(!pending_writebacks.empty() && overlaps_pending(address,bytes))why=S_PEND;
    else if(e->ready!=epoch32() && !first_use(e))return 0;   // counted there
    if(why<0){const Copy &k=copies[e->slot];const Chunk &c=chunks[k.chunk];const VkDeviceSize o=k.off+(address-e->lo);
        if(align>1 && o%align)why=S_ALIGN;
        else{z=Buffer{};z.buffer=c.buffer;z.memory=c.memory;z.offset=o;z.size=bytes;z.mapped=c.mapped+(o);z.coherent=c.coherent;++s.served;}}
    if(why>=0){++s.swhy[why];return 0;}
    if(mode==2){s.old_calls++;s.old_zc-=zc::hits;s.old_sticky-=zc::sticky_misses;s.old_front-=snap_front_hits;s.old_map-=snap_map_hits;s.old_vcache-=vcache::hits;s.old_bytes-=perf.snapshot_bytes;return 2;}
    return 1;}
void vb_verify(uint64_t address,size_t bytes,const Buffer &z,const Buffer &b){
    s.old_zc+=zc::hits;s.old_sticky+=zc::sticky_misses;s.old_front+=snap_front_hits;s.old_map+=snap_map_hits;s.old_vcache+=vcache::hits;s.old_bytes+=perf.snapshot_bytes;
    (void)b;if(!z.mapped || cmp_left<bytes)return;   // against guest memory: what the old path copies / binds at this moment
    cmp_left-=bytes;++s.st_cmp;s.st_cmp_bytes+=bytes;
    bool same;{evguard::EngineRead er(guest(address),bytes);same=!memcmp(z.mapped,guest(address),bytes);}
    if(!same){++s.st_bad;red("static copy of %#llx+%zu differs from guest memory (written after the copy without a fault?)",(unsigned long long)address,bytes);}}
void gone(void*,const nr_sbuf *e){if(e->slot>=0)copy_free(e->slot);if(po_gone)po_gone(e);}   // [DOS2] NR-POSONLY: its copies of the mesh go
void drop_pool(uint64_t base,uint64_t bytes);
void drain(){
    if(!ev_any.exchange(false,std::memory_order_acq_rel))return;
    std::vector<Ev> q;RingPub rp;{std::lock_guard<std::mutex> l(ev_mu);q.swap(evq);rp=ring_pub;ring_pub.fresh=false;}
    if(rp.fresh && ring_mode>0)ring_setup(rp.base,rp.size,rp.head);
    if(!tab_ok)return;
    if(!q.empty())++st_ep;
    for(auto &v:q){if(v.ev==1)nr_stab_add(&tab,v.kind,v.handle,v.mem,v.size,v.cls,v.hash);else if(v.ev==2)nr_stab_kill(&tab,v.kind,v.handle);}}
// The merge of queued adds moves the whole table (~0.3-0.7 ms for 30 000 buffers on x86): batched - at 256 queued, after 16 submissions
// with any queued, or when a quarter of the table is dead. Until then a new buffer takes the old path (kills and reuse act at once).
uint32_t since_apply=0;
void maybe_apply(){if(!tab_ok)return;++since_apply;
    if(tab.nb>=256 || (tab.nb && since_apply>=16) || tab.ndead>std::max<uint32_t>(1024,tab.n/4)){nr_stab_apply(&tab,gone,nullptr);since_apply=0;++st_ep;}}
// [DOS2] NR-29: every static copy freed (translation thread, submission()): each entry with a copy goes back to NRS_NEW, so its next
// bind copies it again (first_use: only the meshes still drawn come back); the chunks retire behind this slot (retire_handle). The
// ring mirror stays (the next submission would rebuild it at once).
void trim(){
    trim_req.store(0,std::memory_order_relaxed);if(st_mode<=0 || !tab_ok)return;
    ++st_ep;
    const VkDeviceSize before=arena_bytes;uint32_t n=0;
    for(uint32_t i=0;i<tab.n;i++){nr_sbuf &e=tab.e[i];if(e.slot<0)continue;copy_free(e.slot);e.slot=-1;if(e.phase==NRS_READY)e.phase=NRS_NEW;e.ready=0;e.stamp=0;++n;}
    for(int k=0;k<2;k++){const int c=cur_chunk[k];if(c>=0 && !chunks[size_t(c)].live && !chunks[size_t(c)].dead)chunk_release(uint32_t(c));}
    ++trims;trim_bytes+=before-arena_bytes;
    xbg_logf("[nr] buf: memory trim (DM_NR_MEM): %u static copies dropped, %.1f of %.1f MB of chunks released (retire behind this submission), %.1f MB left; copied again at their next bind",
             n,(before-arena_bytes)/1048576.0,before/1048576.0,arena_bytes/1048576.0);}
void drop_pool(uint64_t base,uint64_t bytes){
    if(mode<=0)return;drain();
    if(ring_base && ring_base<base+bytes && base<ring_base+ring_size)ring_off("the memory pool of the constant ring was finalized");
    if(tab_ok){const uint32_t k=nr_stab_drop(&tab,base,bytes);s.demote_pool+=k;nr_stab_apply(&tab,gone,nullptr);++st_ep;}}
void init(){
    auto num=[](const char *n,int d,int lo,int hi){const char *e=getenv(n);const int v=e && *e?atoi(e):d;return v<lo||v>hi?d:v;};
    mode=num("DM_NR_BUF",0,0,3);if(!mode)return;
    zc::on();const bool zcon=zc::enabled>0;
    const int rm=num("DM_NR_BUF_RING",1,0,3),sm=num("DM_NR_BUF_STATIC",1,0,2);
    ring_mode=rm==0?0:rm==2?RING_MIRROR:rm==3?(zcon?RING_INPLACE:0):xbg_nr_buf_ring_inplace(zcon)?RING_INPLACE:RING_MIRROR;
    st_mode=sm==0?0:sm==2?1:int(xbg_nr_buf_static_default(zcon));
    {const char *be=getenv("DM_NR_BUF_MB");budget=be && *be?uint64_t(num("DM_NR_BUF_MB",192,0,4096))<<20:dm_nr_mem_cap?dm_nr_mem_cap(NRM_BUFSTATIC,192ull<<20):192ull<<20;}   // [DOS2] NR-29: the default by the RAM budget
    chunk_min=VkDeviceSize(num("DM_NR_BUF_CHUNK_MB",16,4,256))<<20;cmp_budget=uint64_t(num("DM_NR_BUF_CMP_KB",1024,0,1<<20))<<10;
    {const char *pe=getenv("DM_NR_POSONLY");po_tab=pe && *pe && atoi(pe)!=0;}   // [DOS2] NR-POSONLY: the table names the static meshes (no copies of NR-06 unless st_mode)
    if((st_mode || po_tab) && nr_stab_init(&tab)){st_mode=0;po_tab=false;red("no memory for the static buffer table: static path off");}
    tab_ok=st_mode>0 || po_tab;
    xbg_logf("[nr] DM_NR_BUF=%d (NR-06: %s) ring %s, static %s (budget %llu MB), zero-copy %s; DM_NR_BUF_RING=%d DM_NR_BUF_STATIC=%d DM_NR_BUF_CMP_KB=%llu",mode,
             mode==2?"new path built and compared, the old path drawn":mode==3?"on, mirror poisoned":"on",ring_mode==RING_MIRROR?"mirrored":ring_mode==RING_INPLACE?"in place":"off",
             st_mode?"immutable copies":"off",(unsigned long long)(budget>>20),zcon?"on":"off",rm,sm,(unsigned long long)(cmp_budget>>10));}
void report(){
    const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const double f=s.submissions?double(s.submissions):1.,mb=1048576.;
    char old[400]="";if(mode==2)snprintf(old,sizeof old,"; old path took (%llu compared calls): zero-copy %llu, sticky miss %llu, front %llu, map %llu, vcache %llu, copied %.2f MB/submission",
        (unsigned long long)s.old_calls,(unsigned long long)s.old_zc,(unsigned long long)s.old_sticky,(unsigned long long)s.old_front,(unsigned long long)s.old_map,(unsigned long long)s.old_vcache,s.old_bytes/mb/f);
    uint32_t live=0;for(auto &k:copies)live+=k.live;
    xbg_logf("[nr] buf: DM_NR_BUF=%d, %llu submissions; ring (%s): %llu of %llu uniform binds in the ring, %s %llu; old path: not ring %llu, span %llu, hold %llu, runs %llu, "
        "pending %llu, align %llu, UI shift %llu, no chunk %llu, sticky %llu, race %llu; mirror copied %.3f MB / %.1f copies per submission, compared %.3f MB, CPU-write holds %llu, "
        "ring advanced %.3f MB per submission; decision %.0f ns (1 in 64); static: %llu vertex / index binds, served %llu; old path: not an engine buffer %llu, dynamic %llu, "
        "no-init %llu, demoted %llu, no budget %llu, pending %llu, align %llu; %u copies live in %.1f of %llu MB, filled %llu (%.2f MB); demoted: CPU write %llu, dirty %llu, "
        "pool gone %llu; checks: ring %llu compared / %llu bad, static %llu compared (%.2f MB) / %llu bad, written before first draw %llu of %llu%s; events lost %llu",
        mode,(unsigned long long)s.submissions,ring_mode==RING_MIRROR?"mirror":ring_mode==RING_INPLACE?"in place":"off",(unsigned long long)s.ring,(unsigned long long)s.calls,
        mode==2?"would serve":"served",(unsigned long long)(s.mirror+s.inplace),(unsigned long long)s.why[W_NOTRING],(unsigned long long)s.why[W_SPAN],(unsigned long long)s.why[W_HOLD],
        (unsigned long long)s.why[W_RUNS],(unsigned long long)s.why[W_PEND],(unsigned long long)s.why[W_ALIGN],(unsigned long long)s.why[W_UI],(unsigned long long)s.why[W_CHUNK],
        (unsigned long long)s.why[W_STICKY],(unsigned long long)s.why[W_RACE],s.copied/mb/f,s.copies/f,s.cmp_bytes/mb,(unsigned long long)s.cpu_holds,s.adv/mb/f,
        s.timed?double(s.ns)/double(s.timed):0.,(unsigned long long)s.sbinds,(unsigned long long)s.served,(unsigned long long)s.swhy[S_NOT],(unsigned long long)s.swhy[S_DYN],
        (unsigned long long)s.swhy[S_NOINIT],(unsigned long long)s.swhy[S_DEMOTED],(unsigned long long)s.swhy[S_NOBUDGET],(unsigned long long)s.swhy[S_PEND],(unsigned long long)s.swhy[S_ALIGN],
        live,arena_bytes/mb,(unsigned long long)(budget>>20),(unsigned long long)s.filled,s.filled_bytes/mb,(unsigned long long)s.demote_cpu,(unsigned long long)s.demote_dirty,
        (unsigned long long)s.demote_pool,(unsigned long long)s.ring_cmp,(unsigned long long)s.ring_bad,(unsigned long long)s.st_cmp,s.st_cmp_bytes/mb,(unsigned long long)s.st_bad,
        (unsigned long long)s.born_dirty,(unsigned long long)s.born_checked,old,(unsigned long long)ev_lost);
    s=Stats{};}
// Every guest submission, before it is translated (xbg_submit_now): the switches (first call), host events, the ring head, the line.
void submission(){
    if(mode<0)init();if(mode<=0)return;
    drain();maybe_apply();if(trim_req.load(std::memory_order_relaxed))trim();   // [DOS2] NR-29 DM_NR_MEM
    if(ev_lost && st_mode){st_mode=0;red("%llu buffer events lost (queue full): static path off for good",(unsigned long long)ev_lost);}
    ++s.submissions;cmp_left=cmp_budget;
    if(head_addr && ring_base){uint32_t h=0;memcpy(&h,guest(head_addr),4);if(head_seen)s.adv+=h>=head_prev?h-head_prev:uint32_t(ring_size)-head_prev+h;head_prev=h;head_seen=true;}
    pub_arena.store(arena_bytes,std::memory_order_relaxed);pub_mirror.store(mirror_size,std::memory_order_relaxed);   // [DOS2] NR-29
    report();}
// Renderer shutdown, the queue idle (after wtrack::release): everything goes; a restarted renderer reads the switches again.
void release(){
    if(mirror){vkDestroyBuffer(cfg.device,mirror,nullptr);vkFreeMemory(cfg.device,mirror_mem,nullptr);}mirror={};mirror_mem={};mirror_map=nullptr;nr_ring_free(&ring);ring_base=ring_size=0;
    for(auto &c:chunks)if(!c.dead){vkDestroyBuffer(cfg.device,c.buffer,nullptr);vkFreeMemory(cfg.device,c.memory,nullptr);}
    ++st_ep;chunks.clear();copies.clear();free_copies.clear();arena_bytes=0;cur_chunk[0]=cur_chunk[1]=-1;if(tab_ok)nr_stab_free(&tab);tab_ok=false;
    {std::lock_guard<std::mutex> l(ev_mu);if(ring_pub.base){ring_pub.fresh=true;ev_any.store(true);}}   // the host hands the ring over once: set it up again at the next start
    mode=-1;ring_mode=st_mode=0;pub_arena.store(0);pub_mirror.store(0);trim_req.store(0);}
}
extern "C" void xbg_nr_buf_event(int ev,int kind,uint64_t handle,uint64_t mem,uint64_t size,int cls,uint64_t hash){
    std::lock_guard<std::mutex> l(nrbuf::ev_mu);
    if(nrbuf::evq.size()<(1u<<18))nrbuf::evq.push_back({ev,kind,cls,handle,mem,size,hash});else ++nrbuf::ev_lost;
    nrbuf::ev_any.store(true,std::memory_order_release);}
extern "C" void xbg_nr_ring_set(uint64_t base,uint64_t size,uint64_t head_addr){
    std::lock_guard<std::mutex> l(nrbuf::ev_mu);nrbuf::ring_pub.base=base;nrbuf::ring_pub.size=size;nrbuf::ring_pub.head=head_addr;nrbuf::ring_pub.fresh=true;
    nrbuf::ev_any.store(true,std::memory_order_release);}
