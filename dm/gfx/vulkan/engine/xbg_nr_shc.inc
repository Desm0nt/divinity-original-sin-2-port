// [DOS2] NR-SHCACHE DM_NR_SHCACHE: shadow cascade caching (docs/dos2/nr/NR-SHCACHE.md; decisions, proof and state machine in
// dm/gfx/nr/nr_shc.h, host test tests/nr_shc_test.c). Included by xbg_game.cpp right after xbg_nr_occ.inc (before draw()). Hooks, one
// line each, all behind nrshc::mode / nrshc::live (tests/source_structure_test.py block NR-SHCACHE keeps them in place):
//   execute() loop      op(): while a range is open, every record is checked against the ops a cached range may contain
//   OP_END              op_end(): a command handle ended while a range was open (the range cannot be executed again)
//   OP_X_NB_FRAME       frame(): the frame number (cascade ordinals restart), the 10 s line, the switch is read here once
//   OP_X_NB_PASS        pass(): a depth-only IR pass with one attachment opens a range (its start state is kept)
//   OP_X_NB_PASS_END    pass_end(): the range ends: HIT / BUILD bookkeeping, the copies, execution 2 (before nbchk::pass_end)
//   OP_CLEAR_DS         clear_ds(): the frame's full clear of the atlas (value, full or not)
//   draw() first line   draw(): the hash of the draw, arm() at the range's first draw; true = the draw is skipped (a layer member)
// What the backend does per range (nr_shc.h): HIT - copy the layer image S's tile into the atlas at the first draw, skip the members;
// BUILD - draw the members, defer the rest, copy the tile to S at the end, execute the range again from its start state drawing only
// the deferred draws; RECOVER - clear the tile (copy from Z, cleared to the frame's clear value), execute the range again drawing all.
// Execution 2 = execute(start, depth) with nbord::stop at the PASS_END record, the state put back as NB-17's cb_put does.
//
//   DM_NR_SHCACHE  0  default (xbg_nr_shcache_default, every family): nothing - mode stays 0 after the first FRAME record, live stays
//                     false; draw() pays one load, execute() one load a record and one compare at FRAME / PASS / PASS_END / CLEAR_DS.
//                  1  on.
//                  2  check: every decision is made and counted, every draw is drawn as before (the picture is the old one). Every
//                     DM_NR_SHCACHE_VERIFY frames per cascade (default 30, 0 = never) the would-be result is made on the GPU next to the
//                     real one: a would-BUILD makes the layer image (truth saved in X, tile cleared from Z, members drawn, tile -> S,
//                     X -> tile); a would-HIT copies S into the tile, draws only the would-be dynamic draws, reads that tile and the
//                     saved truth back and compares them byte for byte at retire ("verify equal / differ", red line on a difference),
//                     then puts the truth back. The atlas the frame uses is always the truth.
//                  3  poison: as 1, but a HIT does not copy the layer into the tile: the static casters' shadows vanish on screen.
//   DM_NR_SHCACHE_STABLE 3   ranges a draw must be present in (identical) to become a layer member
//   DM_NR_SHCACHE_MIN    64  members a layer needs
//   DM_NR_SHCACHE_AGE    120 ranges a layer lives (then it is rebuilt)
//   DM_NR_SHCACHE_PROBE  8   a cascade without a static set for 4 ranges (moving camera) is hashed in 2 ranges of every 8 (0 = always)
//   DM_NR_SHCACHE_CPUW   1   [shc4] vertex / index / texel bytes in a pool the game's CPU may write (not CPU_NO_ACCESS): 1 versioned by the
//                            write-protect tracker (nvn_dirty_version; an unprotected range is protected and is no member this time, one
//                            written again within 8 frames of its protect is left alone 600 frames), 0 never a member, 2 trusted (only the
//                            write journal: NOT exact against the game's CPU writes; an A/B of what the tracker costs)
// [shc4] Content versions (NR-SHCACHE.md section 12): the hash took the vertex / index buffers and textures by address / handle, so bytes the
// engine rewrote under them left it unchanged (the phone's verify: cascade 2 differs in ~1000 bytes while every draw was "known"). Now each
// hashed draw also mixes in the version of every byte range it reads (write journal of the backend's own writes into guest memory + the
// tracker's page generations for CPU-writable pools) and of every image it samples (the image's upload number after the check its own
// descriptors would make at this point - so a skipped member re-uploads exactly when the drawn one would; a GPU-written image = no member).
// [NR-ZERO] DM_NR_SHC_MEMO (docs/dos2/nr/NR-ZERO.md section 4): the draw hash is the largest single item on the translator thread in the
// census (O_ab_* standing in Fort Joy with the layer warm: 1.0-1.9 us a draw x 840-960 cascade draws = 0.9-1.6 ms a frame). What it looks up
// again for every draw of every range is remembered EXACTLY (nr_shc.h nrshc_pmemo / nvn_dirty_version_tok):
//                  0  default: as before (one load a lookup).
//                  1  the texture registration node of (table, id) from the memo (std::map nodes are never erased at run time; stamp
//                     images_epoch, which the shutdown bumps), the pool's table by its key; the page version of a vertex / index range from
//                     its VMemo while the tracker's token holds (no page of the range left PROT since): no page scan.
//                  2  check: the old lookups decide (the hash is today's), the memo answers next to them and are compared (red lines).
//                  The hash VALUES are the same in all three modes (same inputs, same order): only how they are found changes.
//   Every second timed draw (1 in 128) also times the hash by part - uniform bytes, vertex / index versions, textures, the rest (the state
//   and the order test): one run says where its microseconds go. Line "[nr] shcache memo (...)" every 10 s.
// Lines: "[nr] DM_NR_SHCACHE=..." once; "[nr] shcache (...)" every 10 s; red lines "【红】[nr] shcache: ..." (first 16).
#include "../../nr/nr_shc.h"
#include "../../nr/nr_sort.h"
namespace nbord{extern const uint8_t *stop;}   // xbg_nb_order.inc (later in this unit): execute() returns before the record at stop
void execute(const uint8_t *pc,unsigned depth,uint64_t &budget);
namespace nrshc{
int mode=-1;bool live=false;   // mode: -1 not read yet, 0 off, 1-3 on, -2 switch off after this range, -3 switched off (the images go at the next FRAME)
nrshc_par par=nrshc_par_default();int verify_every=30;
nrshc_cas *cas[NRSHC_CAS]{};nrshc_range *rg=nullptr;
nrshc_undo *ud=nullptr;   // [shc3] the old bytes of the uniform updates of the open range (written back before execution 2)
nrshc_wj *wj=nullptr;     // [shc4] the write journal (every byte range the backend writes into guest memory while the switch is on)
int cpuw=1;uint64_t pool_gen=1,salt_n=0;   // [shc4] DM_NR_SHCACHE_CPUW; pools that came / went (the class memo below); salts (a draw that is no member)
// [shc4] per byte range: its pool class (1 = CPU_NO_ACCESS: only the backend writes it, 2 = the game's CPU may write it) and for class 2 the
// frame it was last protected / until which it is left alone (written again soon after a protect: volatile)
struct VMemo{uint64_t a=0,n=0,gen=0;uint32_t prot=0,vol=0;uint8_t cls=0;bool vok=false;uint64_t tok=0,vg=0,vinc=0;};VMemo vm[8192];   // vok..vinc: [NR-ZERO] DM_NR_SHC_MEMO, the tracker's answer and its token
// [NR-ZERO] DM_NR_SHC_MEMO: mode, the (table, id) -> node memo, the pool table of the last pool key, the timed parts of a split draw
int zmode{0};nrshc_pmemo *pm=nullptr;struct TabMemo{PoolKey k{};uint64_t gen=0;const std::map<int,RegisteredTexture> *m=nullptr;} tabm;bool tsplit=false;
// [shc4] what one draw's inputs are versioned by: J the journal, P the protected pages, T the images / texture registrations / samplers
struct Ver{uint64_t j=0x4A,p=0x50,t=0x54;bool salt=false;};
// the range being executed
struct Range{bool armed=false,refused=false,in_clear=false;int side=0;bool vprobe=false;uint32_t pcmp=0,pvb=0,pib=0,ptex=0,pmiss=0;std::string pdesc;int k=-1;const tex_t *tex=nullptr;unsigned depth=0;uint64_t ends0=0,atlas=0;const uint8_t *start=nullptr;
    int32_t rect[4]{};uint32_t clear_bits=0;uint32_t seq=0;int16_t node=0;uint8_t load=0;State snap;bool snap_missing=false;
    const std::map<int,RegisteredTexture> *tt=nullptr;int diag=0;uint32_t pdraws=0;} R;   // [shc3] pdraws: the PASS record's IR draw count   // [shc2] diag: 1 = this range records its uniform bytes, 2 = compares with the recorded one
int again=NRSHC_R_NONE;uint32_t ord=0;   // execution 2: how, next ordinal
uint64_t ends=0;                          // OP_END of a handle at or above an open range's depth
// the frame
uint32_t cur_frame=0;bool frame_seen=false;int arms=0;int32_t tiles[NRSHC_CAS][4]{};int ntiles=0;bool dirty=false;
uint32_t used=0;int32_t slot_rect[NRSHC_CAS][4]{};int slot_has[NRSHC_CAS]{};   // [shc2] slots armed this frame (bit k); the tile each slot last held
struct FrameClear{uint64_t key=0;uint32_t frame=0,bits=0;} fcs[8];unsigned fci=0;   // the last full depth clears (by image): the atlas's of this frame is looked up at arm
// the atlas and the images that go with it (same format, size and tile coordinates)
struct Atlas{uint64_t key=0;uint32_t last=0;VkFormat fmt{};uint32_t w=0,h=0,bpp=0;std::unique_ptr<Image> S,Z,X;uint32_t zbits=0;bool zok=false;bool real[NRSHC_CAS]{};uint32_t vnext[NRSHC_CAS]{};} A;
// uniform contents already hashed in this range (address + size -> hash; generation: a new range, an OP_UPDATE_UB)
struct Memo{uint64_t addr=0;uint32_t n=0,gen=0;uint64_t h=0;};Memo memo[1024];uint32_t memo_gen=1;
struct Stats{uint64_t ranges,armed,refused,act[3],why[NRSHC_W_N],hit_ok,recover,build_ok,rest,unrepairable,draws,skipped,re_drawn,re_draws,
    tainted,desync,blind,hash_ns,hash_timed,hashed,ubo_bytes,ubo_memo,copies,verify_eq,verify_ne,side_builds,frames,cdraws[NRSHC_CAS],cskip[NRSHC_CAS];
    // [shc2] the reasons (one phone run tells them): refusals by NRSHC_RF_*, the ops of a refusal before the first draw, taints by op and by
    // draw (NRSHC_T_*, nrs_why for T_ORDER), ops met inside a refused range (no taint: nothing is decided there), tainted armed ranges,
    // per cascade the draws known from the previous range / new by uniform bytes only / new otherwise / not compared
    uint64_t rf[NRSHC_RF_N],rop[128],top[128],tw[NRSHC_T_N],nrs[NRS_W_N],ignored,tranges,known[NRSHC_CAS],newu[NRSHC_CAS],newo[NRSHC_CAS],unc[NRSHC_CAS];
    uint64_t dg_ranges,dg_draws,dg_equal,dg_changed,dg_nomatch,dg_short;
    // [shc3] frames with an armed range (the 10 s line's "a frame" numbers include loading screens: per range numbers divide by these),
    // armed ranges per cascade; uniform updates inside open ranges (bytes), undone before an execution 2, ranges whose log was full;
    // executions 2 run (by how) and their draws seen
    uint64_t aframes,cranges[NRSHC_CAS],upd,upd_bytes,undo_runs,undo_over,runs[4],run_draws;
    // [shc4] content versions: byte ranges versioned (CPU_NO_ACCESS pool / CPU pool protected and unchanged / protected now / volatile /
    // untracked / trusted), images versioned (GPU-written: no member), texel ranges, salted draws (no member this time), journal writes and
    // bytes, pool events, atlas passes outside a range (the frame's later ranges are not cached), new by content per cascade and channel,
    // the verify's member probes (compared, vertex / index / texture bytes changed since the layer was built, not found)
    uint64_t v_ranges,v_nocpu,v_cpu_ok,v_cpu_prot,v_cpu_vol,v_cpu_untr,v_trust,v_tex,v_tex_gpu,v_texel,salted,wj_writes,wj_bytes,pool_events,outside,
        ncont[NRSHC_CAS],ncc[NRSHC_CAS][NRSHC_VC_N],pr_cmp,pr_vb,pr_ib,pr_tex,pr_miss,pr_noted;
    // [NR-ZERO] DM_NR_SHC_MEMO: texture lookups / memo answers / compared / bad; pool tables asked / memo / bad; tracker versions asked / memo
    // answers / pages not scanned / compared / bad / fresh answer unavailable (a page BUSY that moment); the split draws (count, ns: all,
    // uniform bytes, vertex / index versions, textures)
    uint64_t zt_q,zt_hit,zt_cmp,zt_bad,zp_q,zp_hit,zp_bad,zv_q,zv_hit,zv_pages,zv_cmp,zv_bad,zv_nf,sp_n,sp_all,sp_ubo,sp_vtx,sp_tex;}s{};
// [shc3] the refused ranges by image (w x h, Vulkan format, aspects): which pass the refusals are (DOS2: the D24S8 scene pre-pass)
struct RefImg{uint32_t w=0,h=0,fmt=0,asp=0;uint64_t n=0,draws=0;};RefImg refimg[4];uint64_t refimg_other=0;
void ref_note(const Image *im,uint32_t draws){if(!im){++refimg_other;return;}
    for(RefImg &r:refimg)if(r.n && r.w==im->width && r.h==im->height && r.fmt==uint32_t(im->fmt) && r.asp==uint32_t(im->aspects)){++r.n;r.draws+=draws;return;}
    for(RefImg &r:refimg)if(!r.n){r.w=im->width;r.h=im->height;r.fmt=uint32_t(im->fmt);r.asp=uint32_t(im->aspects);r.n=1;r.draws=draws;return;}
    ++refimg_other;}
// [shc2] the sampled uniform diff (DM_NR_SHCACHE_DIAG, default 64; 0 = off): per cascade, range r with r % DIAG == 0 keeps every draw's
// uniform ranges (stage, slot, bytes, content hash, a copy of the bytes, <= 4 MB) under its structure hash + occurrence; range r + 1 looks
// each draw up and, where the structure matches and a uniform range's bytes differ, counts (stage, slot, size) with the 16-byte rows that
// moved. One line every 10 s ("[nr] shcache uniforms"): a constant that changes every frame shows as the same rows in every draw.
uint32_t diag_every=64;
struct DiagU{uint8_t st=0,slot=0;uint32_t n=0,off=UINT32_MAX;uint64_t h=0;};
struct DiagD{uint32_t first=0,nu=0;};
struct Diag{std::unordered_map<uint64_t,DiagD> map;std::vector<DiagU> us;std::vector<uint8_t> arena;std::unordered_map<uint64_t,uint32_t> occ;bool have=false;uint32_t range=0;} D[NRSHC_CAS];
struct CurU{uint8_t st,slot;uint32_t n;uint64_t addr,h;};CurU cu[24];unsigned ncu=0;   // the uniform ranges of the draw being hashed (diag ranges only)
// [shc3] + the 4-byte words that moved (words 0-63) and whether every changed draw moved them by the deltas of the first one (a shift common
// to all casters: the camera / cascade matrix) or not (per-object: animation), with the first changed word of the first draw as an example
struct DiagAgg{uint64_t draws=0,mask=0;uint64_t beyond=0;uint32_t rows_max=0;uint64_t wmask=0,rmask=0,same=0,other=0;bool ref=false;int ex_w=-1;float ex_a=0,ex_b=0;float rd[64]{};};
std::map<uint32_t,DiagAgg> dg_agg;   // (stage << 24 | slot << 16 | min(size, 65535))
int nrs_last=0;   // the nrs_why code of the draw hash() just tainted with NRSHC_T_ORDER
std::atomic<uint64_t> v_eq{0},v_ne{0};
unsigned reds=0;uint64_t next_ns=0;
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
void red(const char *fmt,...){if(reds++>=16)return;char b[512];va_list a;va_start(a,fmt);vsnprintf(b,sizeof b,fmt,a);va_end(a);xbg_logf("【红】[nr] shcache: %s",b);}
inline uint64_t img_key(const tex_t *t){return t?uint64_t(uintptr_t(t->mem)):0;}
void init(){
    const char *e=getenv("DM_NR_SHCACHE");int m=e && *e?atoi(e):xbg_nr_shcache_default(gpu_policy);if(m<0 || m>=NRSHC_MODES){xbg_logf("【红】[nr] shcache: DM_NR_SHCACHE=%d is not 0-3: off",m);m=0;}
    auto num=[](const char *n,uint32_t def,uint32_t lo,uint32_t hi){const char *v=getenv(n);if(!v || !*v)return def;long x=atol(v);return x<long(lo)?lo:x>long(hi)?hi:uint32_t(x);};
    par.stable=num("DM_NR_SHCACHE_STABLE",3,1,60);par.min_static=num("DM_NR_SHCACHE_MIN",64,1,NRSHC_DRAWS);par.max_age=num("DM_NR_SHCACHE_AGE",120,1,100000);par.probe=num("DM_NR_SHCACHE_PROBE",8,0,1000);
    verify_every=int(num("DM_NR_SHCACHE_VERIFY",30,0,100000));diag_every=num("DM_NR_SHCACHE_DIAG",64,0,100000);if(diag_every==1)diag_every=2;
    cpuw=int(num("DM_NR_SHCACHE_CPUW",1,0,2));if(cpuw==1 && !nvn_dirty_version)cpuw=0;   // [shc4] no tracker: CPU-writable ranges are no members
    // switches that execute the stream in another order or from key frames: execution 2 needs the stream order
    for(const char *sw:{"DM_NB_ORDER","DM_NB_RELOC","DM_NB_MX"}){const char *v=getenv(sw);if(m && v && *v && atoi(v)!=0){xbg_logf("[nr] shcache: %s=%s executes the stream out of order: DM_NR_SHCACHE off",sw,v);m=0;}}
    if(m){for(int i=0;i<NRSHC_CAS;i++)if(!(cas[i]=nrshc_cas_new()))m=0;rg=static_cast<nrshc_range*>(calloc(1,sizeof(nrshc_range)));if(!rg)m=0;
        ud=static_cast<nrshc_undo*>(calloc(1,sizeof(nrshc_undo)));if(!ud)m=0;wj=static_cast<nrshc_wj*>(calloc(1,sizeof(nrshc_wj)));if(!wj)m=0;}
    // [NR-ZERO] DM_NR_SHC_MEMO: exact memos of the hash's lookups (0 default / 1 on / 2 check)
    {const char *z=getenv("DM_NR_SHC_MEMO");int zm=z && *z?atoi(z):xbg_nr_shc_memo_default();if(zm<0 || zm>2){xbg_logf("【红】[nr] shcache: DM_NR_SHC_MEMO=%d is not 0-2: off",zm);zm=0;}
     if(zm && !m)zm=0;if(zm && !pm && !(pm=static_cast<nrshc_pmemo*>(calloc(1,sizeof(nrshc_pmemo)))))zm=0;zmode=zm;
     if(zm)xbg_logf("[nr] DM_NR_SHC_MEMO=%d (NR-ZERO: %s): texture registration nodes by (table, id) under images_epoch, the pool's table by its key%s; the hash values are those of DM_NR_SHC_MEMO=0",zm,
         zm==1?"the draw hash's lookups answered from exact memos":"check - the old lookups decide, the memos are compared",
         nvn_dirty_version_tok && nvn_dirty_tok_ok?", vertex / index page versions by the tracker's token (no page scan while no page of the range left PROT)":"; NO tracker token here: page versions scanned as before");}
    mode=m;
    if(m)xbg_logf("[nr] DM_NR_SHCACHE=%d (NR-SHCACHE: shadow cascade layers; %s) stable=%u min=%u age=%u probe=%u verify=%d diag=%u cpuw=%d (shc2: a NULL colour slot is no colour target; reasons line; shc3: execution 2 hashed, uniform updates undone; shc4: vertex / index / texel / image contents versioned (%s), atlas passes outside a range dirty the frame, verify probes)",m,m==1?"on":m==2?"check: decided and counted, drawn as before":"poison: a hit keeps the cleared tile",
        par.stable,par.min_static,par.max_age,par.probe,verify_every,diag_every,cpuw,cpuw==1?"CPU-writable pools by the write-protect tracker":cpuw==2?"CPU-writable pools TRUSTED, not exact":"CPU-writable pools never members");}
// [shc4] the backend wrote [a, a+n) of guest memory (snapshot_invalidate: replay CPU writes, GPU writebacks, reports, events) / a pool came or went
void wrote(uint64_t a,uint64_t n){if(wj && n){nrshc_wj_write(wj,a,n);++s.wj_writes;s.wj_bytes+=n;}}
void pool_event(uint64_t a,uint64_t n){wrote(a,n?n:1);++pool_gen;++s.pool_events;}
// ---- images ----
Image *mk_image(const Image &a){std::unique_ptr<Image> im(new Image());VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=a.fmt;ci.extent={a.width,a.height,1};
    ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(vkCreateImage(cfg.device,&ci,nullptr,&im->image)!=VK_SUCCESS){im->image=VK_NULL_HANDLE;return nullptr;}
    VkMemoryRequirements r;vkGetImageMemoryRequirements(cfg.device,im->image,&r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(cfg.device,&ai,nullptr,&im->memory)!=VK_SUCCESS){im->memory=VK_NULL_HANDLE;return nullptr;}
    if(vkBindImageMemory(cfg.device,im->image,im->memory,0)!=VK_SUCCESS)return nullptr;
    im->fmt=a.fmt;im->aspects=VK_IMAGE_ASPECT_DEPTH_BIT;im->usage=ci.usage;im->width=a.width;im->height=a.height;im->levels=1;im->layers=1;im->create=ci;
    return im.release();}
uint32_t depth_bpp(VkFormat f){return f==VK_FORMAT_D16_UNORM?2u:f==VK_FORMAT_X8_D24_UNORM_PACK32?4u:0u;}   // fixed point, no stencil aspect
// the atlas of this range: images made / kept for it; false = refused
bool atlas_for(const Image &a,bool need_s,bool need_x){
    const uint64_t key=uint64_t(uintptr_t(a.source.mem));
    if(A.key!=key || A.fmt!=a.fmt || A.w!=a.width || A.h!=a.height){
        if(A.key)for(int i=0;i<NRSHC_CAS;i++)cas[i]->layer=0;   // another atlas: every layer is gone
        A.S.reset();A.Z.reset();A.X.reset();A.key=key;A.fmt=a.fmt;A.w=a.width;A.h=a.height;A.bpp=depth_bpp(a.fmt);A.zok=false;for(int i=0;i<NRSHC_CAS;i++){A.real[i]=false;A.vnext[i]=0;}}
    if(need_s && !A.S){A.S.reset(mk_image(a));if(!A.S)return false;}
    if(need_s && !A.Z){A.Z.reset(mk_image(a));if(!A.Z)return false;}
    if(need_x && !A.X){A.X.reset(mk_image(a));if(!A.X)return false;}
    return true;}
void copy_tile(Image &src,Image &dst,const int32_t r[4]){OpCommands c(1,src,&dst);c.pre(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);c.pre(dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy g{};g.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};g.dstSubresource=g.srcSubresource;g.srcOffset={r[0],r[1],0};g.dstOffset=g.srcOffset;g.extent={uint32_t(r[2]),uint32_t(r[3]),1};
    vkCmdCopyImage(c.command,src.image,src.layout,dst.image,dst.layout,1,&g);c.post(src);c.post(dst);c.finish();dst.gpu_written=true;++s.copies;}
void tile_to_buffer(Image &src,const int32_t r[4],const Buffer &b){OpCommands c(6,src);c.pre(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy g{};g.bufferOffset=b.offset;g.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};g.imageOffset={r[0],r[1],0};g.imageExtent={uint32_t(r[2]),uint32_t(r[3]),1};
    vkCmdCopyImageToBuffer(c.command,src.image,src.layout,b.buffer,1,&g);c.post(src);c.finish();++s.copies;}
void clear_z(uint32_t bits){if(A.zok && A.zbits==bits)return;Image &z=*A.Z;OpCommands c(8,z);c.pre(z,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    float v;memcpy(&v,&bits,4);VkClearDepthStencilValue cv{v,0};VkImageSubresourceRange sr{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    vkCmdClearDepthStencilImage(c.command,z.image,z.layout,&cv,1,&sr);c.post(z);c.finish();A.zok=true;A.zbits=bits;}
// ---- the state put back (as NB-17 cb_put: the pipeline groups, descriptor generations and memo are re-read) ----
void put(const State &st,bool miss){const auto re=state.restart_eff;const auto cs=state.count_samples;const auto sp=state.samples_passed;
    memcpy(&state,&st,sizeof(State));state.restart_eff=re;state.count_samples=cs;state.samples_passed=sp;missing_program=miss;
    for(int i=0;i<6;i++){++tex_gen[i];++buf_gen[i];}nbdesc::P.valid=false;nbpipe::reset();}
// [shc3] the switch goes off: no hook runs any more (mode -3 is <= 0 for every gate; the FRAME hook, gated by mode != 0, frees the images once
// and sets 0). The images may still be in this frame's copies: they are released when the recording slot retires (its GPU work is done).
void switch_off(){mode=-3;}
void release(){std::shared_ptr<Image> a(A.S.release()),b(A.Z.release()),c(A.X.release());if(a || b || c)retire_handle([a,b,c](){});
    for(int i=0;i<NRSHC_CAS;i++)if(cas[i])cas[i]->layer=0;A.key=0;A.zok=false;}
// [shc3] the uniform updates of the range written back (reverse order) as OP_UPDATE_UB writes them: memory is the range's start again
void undo_apply(){if(!ud || !ud->n)return;++s.undo_runs;
    for(uint32_t i=0;i<ud->n;i++){uint64_t a=0;uint32_t n=0;const uint8_t *old=nrshc_undo_back(ud,i,&a,&n);
        if(gscull_mask()&64)cpu_access(a,n,11);zc::mark(a,n);invalidate_cpu_range(a,n);memcpy(reinterpret_cast<void*>(uintptr_t(a)),old,n);}}
// execution 2 of the open range: from its start state, records [start, stop) drawing the ordinals `how` selects.
// [shc3] The draw() hook is gated by `live`, which pass_end has already cleared: execution 2 opens it (live = true) or its draws are neither
// hashed nor counted nor selected - the phone's "execution 2 met 0 draws, execution 1 had 206" (18.11, every BUILD). The uniform memo starts
// a new generation (its entries hold the END of execution 1) and the range's uniform updates are undone first.
void again_run(int how,const uint8_t *stop,uint64_t &budget){
    State *end=new State(state);const bool end_missing=missing_program;const auto saved_op=cur_op;
    undo_apply();put(R.snap,R.snap_missing);++memo_gen;++s.runs[how&3];const uint64_t d0=s.re_draws;
    live=true;again=how;ord=0;nbord::stop=stop;execute(R.start,R.depth,budget);nbord::stop=nullptr;again=NRSHC_R_NONE;live=false;
    s.run_draws+=s.re_draws-d0;
    if(mode>0 && ord!=rg->ndraw && rg->ndraw<=NRSHC_DRAWS){++s.desync;red("execution 2 met %u draws, execution 1 had %u (range seq %u node %d, cascade %d, %s, how %d, updates %u) - switch off",
        ord,rg->ndraw,R.seq,int(R.node),R.k,rg->action==NRSHC_A_HIT?"hit":rg->action==NRSHC_A_BUILD?"build":"normal",how,ud?ud->n:0u);switch_off();}
    put(*end,end_missing);delete end;cur_op=saved_op;}
// ---- the hash of a draw (see nr_shc.h: everything its depth output depends on) and its order-freedom ----
uint64_t ubo(uint64_t addr,size_t n){Memo &m=memo[((addr>>8)^(addr>>20))&1023];if(m.gen==memo_gen && m.addr==addr && m.n==n){++s.ubo_memo;return m.h;}
    const void *p=reinterpret_cast<const void*>(uintptr_t(addr));evguard::EngineRead er(p,n);const uint64_t h=nrshc_bytes(0x55424f,p,n);s.ubo_bytes+=n;
    m.gen=memo_gen;m.addr=addr;m.n=uint32_t(n);m.h=h;return h;}
// [NR-ZERO] the registration node of id in table tt (nullptr: not registered): the memo (DM_NR_SHC_MEMO=1), the map (0), both compared (2)
const RegisteredTexture *tfind(const std::map<int,RegisteredTexture> *tt,int id){
    if(!zmode){auto f=tt->find(id);return f==tt->end()?nullptr:&f->second;}
    ++s.zt_q;const RegisteredTexture *c=static_cast<const RegisteredTexture*>(nrshc_pm_get(pm,tt,int32_t(id),images_epoch));
    if(c && zmode==1){++s.zt_hit;return c;}
    auto f=tt->find(id);const RegisteredTexture *r=f==tt->end()?nullptr:&f->second;
    if(c){++s.zt_hit;++s.zt_cmp;if(c!=r){++s.zt_bad;red("memo: texture id %d of table %p: the memo holds node %p, the map %p",id,(const void*)tt,(const void*)c,(const void*)r);}}
    else nrshc_pm_put(pm,tt,int32_t(id),images_epoch,r);
    return r;}
uint64_t texrev(uint64_t hd){if(!hd || !R.tt)return 0;const RegisteredTexture *f=tfind(R.tt,int(hd&0xFFFFFu));if(!f)return 1;return nrshc_mix(f->revision,f->texture.serial);}
// [shc4] the textures of the draw's CURRENT pool (R.tt is the one of the range's first draw; a draw that switched pools must not version
// another pool's texture), as the descriptor code finds them
const std::map<int,RegisteredTexture> *cur_tmap(){if(!state.texture_pool.pool)return &legacy_textures;
    if(zmode){const PoolKey k=pool_key(&state.texture_pool);++s.zp_q;   // [NR-ZERO] the outer map's nodes are never erased at run time either
        if(tabm.m && tabm.gen==images_epoch && tabm.k==k){++s.zp_hit;if(zmode==1)return tabm.m;
            auto f=textures.find(k);if(f==textures.end() || &f->second!=tabm.m){++s.zp_bad;red("memo: the table of pool key %#llx+%lld moved",(unsigned long long)k.first,(long long)k.second);}
            return f==textures.end()?&legacy_textures:&f->second;}
        auto f=textures.find(k);if(f==textures.end())return &legacy_textures;tabm.k=k;tabm.gen=images_epoch;tabm.m=&f->second;return tabm.m;}
    auto f=textures.find(pool_key(&state.texture_pool));return f==textures.end()?&legacy_textures:&f->second;}
// [shc4] the version of the guest bytes [a, a+n) a draw reads (vertex stream, index range, texel buffer): the journal of the backend's own
// writes, and for a pool the game's CPU may write the tracker's page generations (protected now = a version; not protected = protect it,
// no member this time; protected again and again = volatile, left alone and never a member)
void rv(uint64_t a,uint64_t n,Ver &V){
    if(!a || !n)return;++s.v_ranges;V.j=nrshc_mix(V.j,nrshc_wj_ver(wj,a,n));
    VMemo &m=vm[uint32_t((a*0x9E3779B97F4A7C15ull^n*0xC2B2AE3D27D4EB4Full)>>51)&8191u];
    if(m.a!=a || m.n!=n || m.gen!=pool_gen){m=VMemo{};m.a=a;m.n=n;m.gen=pool_gen;const int f=nvn_pool_flags?nvn_pool_flags(reinterpret_cast<const void*>(uintptr_t(a)),size_t(n)):-1;m.cls=f>=0 && (f&1)?1:2;}
    if(m.cls==1){++s.v_nocpu;return;}   // NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS: only the GPU / the backend write it
    if(__builtin_expect(nrnev::mode!=0,0) && nrnev::rv(a,n,V.p)){++s.v_cpu_ok;return;}   // [DOS2] NR-NATIVE step E: DM_NRN_EV=1 - a range inside an engine buffer is versioned by the engine's events, not by page generations (=2: compared, the tracker decides)
    if(cpuw==2){++s.v_trust;return;}
    if(cpuw==0){V.salt=true;++s.v_cpu_untr;return;}
    const void *p=reinterpret_cast<const void*>(uintptr_t(a));uint64_t g=0,inc=0;
    if(zmode && nvn_dirty_version_tok && nvn_dirty_tok_ok){++s.zv_q;   // [NR-ZERO] DM_NR_SHC_MEMO: the tracker's answer while its token holds
        const uint64_t pages=((a+n-1)>>12)-(a>>12)+1;
        if(zmode==1){
            if(m.vok && nvn_dirty_tok_ok(m.tok)){++s.zv_hit;s.zv_pages+=pages;V.p=nrshc_mix(nrshc_mix(V.p,m.vg),m.vinc);++s.v_cpu_ok;return;}
            uint64_t tok=0;m.vok=nvn_dirty_version_tok(p,size_t(n),&g,&inc,&tok)!=0;
            if(m.vok){m.tok=tok;m.vg=g;m.vinc=inc;V.p=nrshc_mix(nrshc_mix(V.p,g),inc);++s.v_cpu_ok;return;}
            if(nvn_dirty_version(p,size_t(n),&g,&inc)){V.p=nrshc_mix(nrshc_mix(V.p,g),inc);++s.v_cpu_ok;return;}}   // the token gave up (an epoch moved under its scan): today's answer, no memo
        else{   // =2: today's answer decides; the memo's is compared with it
            const int fresh=nvn_dirty_version(p,size_t(n),&g,&inc);
            if(m.vok && nvn_dirty_tok_ok(m.tok)){++s.zv_hit;if(!fresh)++s.zv_nf;else{++s.zv_cmp;if(g!=m.vg || inc!=m.vinc){++s.zv_bad;
                red("memo: range %#llx+%llu: the token still holds generation %llu / pool %llu, the tracker now %llu / %llu",(unsigned long long)a,(unsigned long long)n,(unsigned long long)m.vg,(unsigned long long)m.vinc,(unsigned long long)g,(unsigned long long)inc);}}}
            else{uint64_t g2=0,i2=0,tok=0;m.vok=nvn_dirty_version_tok(p,size_t(n),&g2,&i2,&tok)!=0;if(m.vok){m.tok=tok;m.vg=g2;m.vinc=i2;}}
            if(fresh){V.p=nrshc_mix(nrshc_mix(V.p,g),inc);++s.v_cpu_ok;return;}}}
    else if(nvn_dirty_version(p,size_t(n),&g,&inc)){V.p=nrshc_mix(nrshc_mix(V.p,g),inc);++s.v_cpu_ok;return;}
    V.salt=true;
    if(m.vol>cur_frame){++s.v_cpu_vol;return;}
    if(m.prot && cur_frame-m.prot<8){m.vol=cur_frame+600;++s.v_cpu_vol;return;}   // written again within 8 frames of its protect: leave it alone
    if(nvn_dirty_tracked && nvn_dirty_protect && nvn_dirty_tracked(p,size_t(n))){nvn_dirty_protect(p,size_t(n));m.prot=cur_frame?cur_frame:1;++s.v_cpu_prot;}
    else{m.vol=cur_frame+600;++s.v_cpu_untr;}}
// [shc4] the version of a texture the draw samples. An image: get_image() makes the check its descriptors would make now (once a submission:
// a skipped member re-uploads exactly when the drawn draw would), then the image's upload number (every upload / partial re-upload takes a
// new one); a GPU-written image changes without an upload: no member. A texel buffer: the version of its guest bytes.
void texver(uint64_t hd,uint32_t type,Ver &V){
    const int id=int(hd&0xFFFFFu);if(!id || (state.texture_pool.n>0 && id>=state.texture_pool.n))return;   // the descriptors bind the null texture there
    const auto *tt=cur_tmap();const RegisteredTexture *f=tfind(tt,id);if(!f)return;   // [NR-ZERO] tfind: the memo or the map
    const tex_t &t=f->texture;
    if(type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || t.d.target==10){++s.v_texel;rv(uint64_t(uintptr_t(t.mem)),t.L.size,V);return;}
    if(type==VK_DESCRIPTOR_TYPE_SAMPLER || !t.mem)return;
    Image &im=get_image(&t,true);++s.v_tex;
    if(im.gpu_written){V.salt=true;++s.v_tex_gpu;return;}
    V.t=nrshc_mix(V.t,uint64_t(uintptr_t(&im))^(im.nrshc_up<<1));}
// [shc2] tw: the first NRSHC_T_* reason the draw taints its range for (0 = none); hs: the structure hash (h without the uniform contents,
// diagnostics only). h = mix(hs, the chained content hashes of the uniform ranges) - the same inputs as before, in another order.
// [shc4] wv: execution 1 also versions the contents (V); execution 2 takes execution 1's version per ordinal instead (no protect, no check)
uint64_t hash(int prim,int first,int count,const p_draw_idx *ix,uint32_t inst,uint32_t first_inst,bool gi,int &tw,uint64_t &hs,bool wv,Ver &V){
    auto taint=[&tw](int w){if(!tw)tw=w;};uint64_t hu=0x55424f;ncu=0;uint32_t vseen=0;
    uint64_t h=nrshc_mix(0x53484331,uint64_t(uint32_t(prim))|uint64_t(uint32_t(count))<<32);h=nrshc_mix(h,uint64_t(uint32_t(first))|uint64_t(inst)<<32);h=nrshc_mix(h,first_inst);
    if(ix){h=nrshc_mix(h,ix->ibuf);h=nrshc_mix(h,uint64_t(uint32_t(ix->itype))|uint64_t(uint32_t(ix->base_vertex))<<32);h=nrshc_mix(h,uint64_t(uint32_t(ix->base_instance))|uint64_t(uint32_t(ix->instances))<<32);
        if(wv && count>0 && ix->itype>=0 && ix->itype<=2){const uint64_t q0=tsplit?timing_clock():0;rv(ix->ibuf,uint64_t(uint32_t(count))<<ix->itype,V);if(tsplit)s.sp_vtx+=timing_clock()-q0;}}   // [shc4] the index bytes
    if(gi)taint(NRSHC_T_INDIRECT);   // indirect arguments live in GPU memory
    // the target: the atlas only, the tile as viewport
    if(img_key(state.rt.depth)!=R.atlas)taint(NRSHC_T_TARGET);for(int i=0;i<state.rt.n && i<8;i++)if(state.rt.color[i])taint(NRSHC_T_COLOUR);
    h=nrshc_mix(h,uint64_t(uintptr_t(state.rt.depth)));if(state.rt.has_dview)h=nrshc_bytes(h,&state.rt.dview,sizeof state.rt.dview);
    if(state.viewport.x!=float(R.rect[0]) || state.viewport.y!=float(R.rect[1]) || state.viewport.width!=float(R.rect[2]) || state.viewport.height!=float(R.rect[3]))taint(NRSHC_T_VIEWPORT);
    for(int st=0;st<5;st++)h=nrshc_mix(h,uint64_t(uintptr_t(state.shader[st])));
    // vertex fetch: the attributes the vertex shader reads and their streams (stale binds of other slots do not count)
    const Shader *vs=state.shader[0];
    auto stream=[&](unsigned k){if(k>=16){taint(NRSHC_T_VERTEX);return;}h=nrshc_mix(h,uint64_t(state.stream[k].stride)|uint64_t(state.stream[k].divisor)<<32);h=nrshc_mix(h,state.vb[k].address);h=nrshc_mix(h,state.vb[k].size);
        if(wv && !(vseen>>k&1)){vseen|=1u<<k;const uint64_t q0=tsplit?timing_clock():0;rv(state.vb[k].address,state.vb[k].size,V);if(tsplit)s.sp_vtx+=timing_clock()-q0;}};   // [shc4] the vertex bytes (each stream once)
    if(vs && !vs->inputs.empty()){for(uint32_t loc:vs->inputs){if(loc>=16){taint(NRSHC_T_VERTEX);continue;}const uint32_t a=state.attrib[loc];h=nrshc_mix(h,uint64_t(a)|uint64_t(loc)<<32);stream((a>>27)&31u);}}
    else for(unsigned k=0;k<16;k++){h=nrshc_mix(h,state.attrib[k]);stream(k);}
    // fixed function
    h=nrshc_bytes(h,&state.poly,sizeof state.poly);h=nrshc_bytes(h,&state.ds,sizeof state.ds);h=nrshc_bytes(h,state.bias,sizeof state.bias);
    h=nrshc_bytes(h,&state.viewport,sizeof state.viewport);h=nrshc_bytes(h,&state.scissor,sizeof state.scissor);h=nrshc_bytes(h,&state.color,sizeof state.color);
    h=nrshc_bytes(h,&state.ms,sizeof state.ms);h=nrshc_bytes(h,&state.mask,sizeof state.mask);h=nrshc_bytes(h,&state.alpha_ref,sizeof state.alpha_ref);
    h=nrshc_mix(h,uint64_t(state.depth_clamp)|uint64_t(state.rast_discard)<<32);h=nrshc_bytes(h,&state.restart,sizeof state.restart);h=nrshc_bytes(h,&state.bounds,sizeof state.bounds);
    h=nrshc_bytes(h,state.stencil_compare,sizeof state.stencil_compare);h=nrshc_bytes(h,state.stencil_write,sizeof state.stencil_write);h=nrshc_bytes(h,state.stencil_ref,sizeof state.stencil_ref);
    h=nrshc_mix(h,uint64_t(uintptr_t(state.texture_pool.pool))^uint64_t(state.texture_pool.off)<<1);h=nrshc_mix(h,uint64_t(uintptr_t(state.sampler_pool.pool))^uint64_t(state.sampler_pool.off)<<1);
    // what the shaders read: the content of each declared uniform range, each texture / sampler handle (+ the texture's registration)
    for(int st=0;st<5;st++){const Shader *sh=state.shader[st];if(!sh)continue;const unsigned ns=nvn_stage[st];if(!sh->storage_writes.empty())taint(NRSHC_T_STORAGE);
        for(const auto &b:sh->bindings){
            if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || b.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)taint(NRSHC_T_STORAGE);
            if(b.source==XBG_SOURCE_DRIVER2){for(unsigned i=0;i<128;i++){if(state.tex[ns][i]){h=nrshc_mix(h,state.tex[ns][i]^uint64_t(i)<<56);h=nrshc_mix(h,texrev(state.tex[ns][i]));}if(state.samp[ns][i])h=nrshc_mix(h,state.samp[ns][i]^uint64_t(i)<<56);}continue;}
            for(unsigned e=0;e<b.count;e++){const unsigned slot=b.slot+e;
                switch(b.source){
                case XBG_SOURCE_UNIFORM:{if(slot>=16){taint(NRSHC_T_BINDING);break;}const NVNbufferRange &r=state.ub[ns][slot];
                    if(!r.address || !r.size){h=nrshc_mix(h,0x55420000u+slot);break;}const size_t n=std::min<size_t>(r.size,b.bytes?b.bytes:65536);
                    h=nrshc_mix(h,0x55430000u+slot+(uint64_t(st)<<8)+(uint64_t(n)<<32));const uint64_t q0=tsplit?timing_clock():0;const uint64_t uh=ubo(r.address,n);if(tsplit)s.sp_ubo+=timing_clock()-q0;hu=nrshc_mix(hu,uh);   // the structure keeps (stage, slot, size); the bytes go to hu
                    if(R.diag && ncu<sizeof cu/sizeof cu[0])cu[ncu++]=CurU{uint8_t(st),uint8_t(slot),uint32_t(n),r.address,uh};break;}
                case XBG_SOURCE_TEXTURE:{if(slot>=128){taint(NRSHC_T_BINDING);break;}const uint64_t t=state.tex[ns][slot];const uint64_t q0=tsplit?timing_clock():0;h=nrshc_mix(h,t);h=nrshc_mix(h,texrev(t));if(wv)texver(t,b.type,V);if(tsplit)s.sp_tex+=timing_clock()-q0;   // [shc4] its content
                    if(b.sampler_slot!=UINT32_MAX){if(b.sampler_slot>=128){taint(NRSHC_T_BINDING);break;}h=nrshc_mix(h,state.samp[ns][b.sampler_slot]);}break;}
                case XBG_SOURCE_SUPPORT:case XBG_SOURCE_C1:break;   // viewport size / alpha function (above); the shader's own constants (its pointer)
                default:taint(NRSHC_T_BINDING);   // storage buffers, images, the storage table, bindless sources: what they read is not known here
                }}}}
    // order-freedom (nr_sort.h nrs_why): depth only, min or no depth effect, nothing reads the attachment
    nrs_in in{};in.query=open_occlusion.cmd!=VK_NULL_HANDLE || state.count_samples;
    in.condr=(condr::cg.mode>0 && condr::cg.kind!=CONDR_K_ON) || (nrocc::mode>0 && (nrocc::kind!=CONDR_K_ON || nrocc::in_pair()));
    in.storage=!storage_images_bound.empty();in.depth_att=state.rt.depth!=nullptr;in.depth_fixed=A.bpp!=0;
    in.st=state.ds.st;in.bounds=state.bounds.enable!=0;in.a2c=state.ms.a2c!=0;in.dt=state.ds.dt;in.dw=state.ds.dw;in.op=state.ds.df>=1 && state.ds.df<=8?uint8_t(state.ds.df-1):uint8_t(255);
    {const int nw=nrs_why(&in);if(nw!=NRS_OK){if(!tw)nrs_last=nw;taint(NRSHC_T_ORDER);}}
    if(wv)V.t=nrshc_mix(V.t,memo_bump[1]);   // [shc4] a sampler registration changed somewhere (its parameters are read by handle)
    hs=h;return nrshc_mix(h,hu);}
// ---- the range ----
void arm(){R.armed=true;
    // [shc2] the target's shape (nr_shc.h nrshc_shape_why, every refusal counted by reason). DOS2 binds the atlas as SetRenderTargets(1, {NULL},
    // atlas): rt.n = 1 with a NULL colour 0 - only a BOUND colour target refuses (the old test `rt.n == 0` refused every cascade on the phone).
    // One atlas at a time: another depth-only image takes over only after 60 frames without the current one (no thrashing of layers).
    nrshc_shape sh{};sh.has_depth=state.rt.depth!=nullptr;Image *ap=nullptr;
    if(sh.has_depth){Image &im=get_image(state.rt.depth,false);ap=&im;sh.same_image=img_key(state.rt.depth)==R.atlas;sh.depth_aspect_only=im.aspects==VK_IMAGE_ASPECT_DEPTH_BIT;
        sh.fixed_point=depth_bpp(im.fmt)!=0;sh.levels=int(im.levels);sh.layers=int(im.layers);sh.samples=int(im.samples);sh.width=im.width;sh.height=im.height;}
    for(int i=0;i<state.rt.n && i<8;i++)if(state.rt.color[i])++sh.colours;
    sh.other_atlas=A.key && A.key!=R.atlas && cur_frame-A.last<60;sh.arms=arms;
    const int rf=nrshc_shape_why(&sh);
    if(rf!=NRSHC_RF_OK){R.refused=true;++s.refused;++s.rf[rf];ref_note(ap,R.pdraws);return;}
    Image &a=*ap;
    const VkViewport &v=state.viewport;
    R.rect[0]=int32_t(v.x);R.rect[1]=int32_t(v.y);R.rect[2]=int32_t(v.width);R.rect[3]=int32_t(v.height);
    // [shc2] the slot by tile (nrshc_pick_slot), not by ordinal: a cascade keeps its layer when an earlier one is no range this frame
    R.k=nrshc_pick_slot(slot_rect,slot_has,used,R.rect);if(R.k<0){R.refused=true;++s.refused;++s.rf[NRSHC_RF_SLOTS];return;}
    used|=1u<<R.k;if(++arms==1)++s.aframes;memcpy(slot_rect[R.k],R.rect,sizeof R.rect);slot_has[R.k]=1;
    nrshc_cas *c=cas[R.k];R.tex=state.rt.depth;A.last=cur_frame;
    const bool rect_ok=float(R.rect[0])==v.x && float(R.rect[1])==v.y && float(R.rect[2])==v.width && float(R.rect[3])==v.height && R.rect[0]>=0 && R.rect[1]>=0 && R.rect[2]>0 && R.rect[3]>0
        && uint32_t(R.rect[0]+R.rect[2])<=a.width && uint32_t(R.rect[1]+R.rect[3])<=a.height;
    bool overlap=false;for(int i=0;i<ntiles;i++){const int32_t *t=tiles[i];if(R.rect[0]<t[0]+t[2] && t[0]<R.rect[0]+R.rect[2] && R.rect[1]<t[1]+t[3] && t[1]<R.rect[1]+R.rect[3])overlap=true;}
    if(ntiles<NRSHC_CAS)memcpy(tiles[ntiles++],R.rect,sizeof R.rect);
    bool clear_ok=false;for(const FrameClear &f:fcs)if(f.key==R.atlas && f.frame==cur_frame){clear_ok=!R.in_clear;R.clear_bits=f.bits;}
    const bool tm=mode==NRSHC_CHECK;
    const bool objs=atlas_for(a,true,tm && verify_every>0);
    R.tt=&legacy_textures;if(state.texture_pool.pool){auto f=textures.find(pool_key(&state.texture_pool));if(f!=textures.end())R.tt=&f->second;}
    int why=0;const int act=nrshc_arm(c,&par,R.atlas,R.rect,clear_ok,R.clear_bits,dirty || overlap,rect_ok,objs && !capture.active,&why);
    ++s.armed;++s.act[act];++s.why[why];
    nrshc_begin(c,&par,rg,act,why);++memo_gen;if(rg->blind)++s.blind;
    // [shc2] the sampled uniform diff: record in range r (r % diag_every == 0), compare in range r + 1 of the same cascade
    R.diag=0;if(diag_every && !rg->blind){const uint32_t rr=c->ranges;Diag &dg=D[R.k];
        // [shc3] not range 0: the first frames after a level load (the camera still settles) are no sample
        if(rr%diag_every==0 && rr>=diag_every){R.diag=1;dg.map.clear();dg.us.clear();dg.arena.clear();dg.occ.clear();dg.have=true;dg.range=rr;}
        else if(rr%diag_every==1 && dg.have && dg.range+1==rr){R.diag=2;dg.occ.clear();++s.dg_ranges;}}
    if(act==NRSHC_A_HIT && (mode==NRSHC_ON) && A.S)copy_tile(*A.S,a,R.rect);   // mode 3: the cleared tile stays (poison)
    R.vprobe=act==NRSHC_A_HIT && mode==NRSHC_CHECK && verify_every>0 && A.real[R.k] && cur_frame>=A.vnext[R.k];   // [shc4] side() verifies this hit
}
// [shc2] the sampled uniform diff of one draw (R.diag 1 = keep its uniform ranges, 2 = compare with the kept ones)
void diag_draw(uint64_t hs){Diag &dg=D[R.k];const uint64_t key=nrshc_mix(hs,dg.occ[hs]++);
    if(R.diag==1){DiagD dd;dd.first=uint32_t(dg.us.size());dd.nu=ncu;
        for(unsigned i=0;i<ncu;i++){DiagU u;u.st=cu[i].st;u.slot=cu[i].slot;u.n=cu[i].n;u.h=cu[i].h;
            if(dg.arena.size()+cu[i].n<=(4u<<20)){const void *p=reinterpret_cast<const void*>(uintptr_t(cu[i].addr));evguard::EngineRead er(p,cu[i].n);
                u.off=uint32_t(dg.arena.size());const uint8_t *b=static_cast<const uint8_t*>(p);dg.arena.insert(dg.arena.end(),b,b+cu[i].n);}
            dg.us.push_back(u);}
        dg.map.emplace(key,dd);return;}
    ++s.dg_draws;auto f=dg.map.find(key);if(f==dg.map.end()){++s.dg_nomatch;return;}
    const DiagD &dd=f->second;if(dd.nu!=ncu){++s.dg_nomatch;return;}
    bool changed=false;
    for(unsigned i=0;i<ncu;i++){const DiagU &u=dg.us[dd.first+i];if(u.st!=cu[i].st || u.slot!=cu[i].slot || u.n!=cu[i].n){changed=true;continue;}
        if(u.h==cu[i].h)continue;changed=true;
        DiagAgg &g=dg_agg[uint32_t(u.st)<<24|uint32_t(u.slot)<<16|std::min<uint32_t>(u.n,65535)];++g.draws;
        if(u.off==UINT32_MAX){++s.dg_short;continue;}
        const void *p=reinterpret_cast<const void*>(uintptr_t(cu[i].addr));evguard::EngineRead er(p,u.n);uint64_t m=0;uint32_t beyond=0;
        const uint32_t rows=nrshc_rows(dg.arena.data()+u.off,p,u.n,&m,&beyond);g.mask|=m;g.beyond+=beyond;g.rows_max=std::max(g.rows_max,rows);
        // [shc3] the words and their deltas: the same shift in every caster (camera / cascade matrix) or per-object changes
        float d[64]{};const uint64_t wm=nrshc_words(dg.arena.data()+u.off,p,u.n,d);g.wmask|=wm;
        if(!g.ref){g.ref=true;g.rmask=wm;memcpy(g.rd,d,sizeof d);
            if(wm){const int w=__builtin_ctzll(wm);g.ex_w=w;memcpy(&g.ex_a,dg.arena.data()+u.off+4*w,4);memcpy(&g.ex_b,static_cast<const uint8_t*>(p)+4*w,4);}}
        else if(nrshc_same_delta(wm,d,g.rmask,g.rd))++g.same;else ++g.other;}
    if(changed)++s.dg_changed;else ++s.dg_equal;}
// [shc4] check mode: the member probes. When a would-BUILD makes its layer for real (side build), each member drawn into it has its inputs
// noted: the BYTES of its vertex streams and index range (up to 1 MB each), and per texture the image's upload number / GPU-written mark and
// a 16 KB sample of its guest storage (8 x 2 KB, evenly spread). When a would-HIT is verified, each member it would skip is probed again and compared: the phone run
// names the class of an input that moved under an unchanged hash (vertex / index / texture) with addresses, instead of only "differs".
struct Probe{uint64_t vb=0,ib=0,tx=0,vba=0,iba=0,txa=0;uint32_t vbn=0,ibn=0,ntx=0,stride=0;};
std::unordered_map<uint64_t,Probe> PR[NRSHC_CAS];
uint64_t bytes_h(uint64_t a,uint64_t n){if(!a || !n)return 0;const uint64_t m=std::min<uint64_t>(n,1u<<20);const void *p=reinterpret_cast<const void*>(uintptr_t(a));evguard::EngineRead er(p,size_t(m));return nrshc_bytes(n,p,size_t(m));}
Probe probe(const p_draw_idx *ix,int count){Probe q;uint32_t seen=0;const Shader *vs=state.shader[0];
    auto one=[&](unsigned k){if(k>=16 || (seen>>k&1))return;seen|=1u<<k;const auto &b=state.vb[k];q.vb=nrshc_mix(q.vb,bytes_h(b.address,b.size));if(!q.vba){q.vba=b.address;q.vbn=uint32_t(b.size);q.stride=state.stream[k].stride;}};
    if(vs && !vs->inputs.empty()){for(uint32_t loc:vs->inputs)if(loc<16)one((state.attrib[loc]>>27)&31u);}else for(unsigned k=0;k<16;k++)one(k);
    if(ix && count>0 && ix->itype>=0 && ix->itype<=2){q.iba=ix->ibuf;q.ibn=uint32_t(uint64_t(uint32_t(count))<<ix->itype);q.ib=bytes_h(ix->ibuf,q.ibn);}
    const auto *tt=cur_tmap();
    for(int st=0;st<5;st++){const Shader *sh=state.shader[st];if(!sh)continue;const unsigned ns=nvn_stage[st];
        for(const auto &b:sh->bindings){if(b.source!=XBG_SOURCE_TEXTURE)continue;
            for(unsigned e=0;e<b.count;e++){const unsigned slot=b.slot+e;if(slot>=128)continue;const uint64_t hd=state.tex[ns][slot];if(!hd)continue;auto f=tt->find(int(hd&0xFFFFFu));if(f==tt->end())continue;
                const tex_t &t=f->second.texture;uint64_t x=nrshc_mix(hd,uint64_t(t.serial));
                if(const Image *im=find_image(t.serial))x=nrshc_mix(x,(im->nrshc_up<<2)|(im->gpu_written?1u:0u)|(im->uploaded?2u:0u));
                if(t.mem && t.L.size){const uint64_t n=t.L.size,step=n/8;for(unsigned i=0;i<8;i++){const uint64_t o=step*i,len=std::min<uint64_t>(2048,n-o);x=nrshc_mix(x,bytes_h(uint64_t(uintptr_t(t.mem))+o,len));}}
                q.tx=nrshc_mix(q.tx,x);if(!q.txa)q.txa=uint64_t(uintptr_t(t.mem));++q.ntx;}}}
    return q;}
void probe_note(uint64_t h,const p_draw_idx *ix,int count){if(R.k<0)return;auto &m=PR[R.k];if(m.size()>=8192 || m.count(h))return;m.emplace(h,probe(ix,count));++s.pr_noted;}
void probe_check(uint64_t h,const p_draw_idx *ix,int count){auto &m=PR[R.k];auto f=m.find(h);if(f==m.end()){++s.pr_miss;++R.pmiss;return;}
    const Probe a=f->second,b=probe(ix,count);++R.pcmp;++s.pr_cmp;const bool cv=a.vb!=b.vb,ci=a.ib!=b.ib,ct=a.tx!=b.tx;
    if(cv){++R.pvb;++s.pr_vb;}if(ci){++R.pib;++s.pr_ib;}if(ct){++R.ptex;++s.pr_tex;}
    if((cv || ci || ct) && R.pdesc.size()<400){char d[200];snprintf(d,sizeof d,"%s[ord %u: %s%s%s vb %#llx+%u stride %u ib %#llx+%u tex %u at %#llx]",R.pdesc.empty()?"":" ",rg->ndraw-1,cv?"vertex ":"",ci?"index ":"",ct?"texture ":"",
        (unsigned long long)b.vba,b.vbn,b.stride,(unsigned long long)b.iba,b.ibn,b.ntx,(unsigned long long)b.txa);R.pdesc+=d;}}
bool draw(int prim,int first,int count,const p_draw_idx *ix,uint32_t inst,uint32_t first_inst,bool gi){
    if(R.refused || (mode<=0 && !again))return false;   // [shc3] execution 2 counts its draws to the end even after a desync (mode -2)
    if(!R.armed){arm();if(R.refused)return false;}
    if(rg->blind && !again)return false;   // a quiet cascade between its probes: not hashed
    const bool timed=!(s.hashed&63);const uint64_t t0=timed?timing_clock():0;tsplit=timed && (s.hashed>>6&1);   // [NR-ZERO] 1 in 128: the hash by part
    int tw=NRSHC_T_OK;uint64_t hs=0;Ver V;const uint64_t hn=hash(prim,first,count,ix,inst,first_inst,gi,tw,hs,!again,V);
    // [shc4] the content version: execution 1 computes it (a draw whose inputs have no version now gets a salt: new every time, never a
    // member), execution 2 takes execution 1's for the same ordinal
    uint64_t v;if(again)v=nrshc_again_v(rg,ord);else{v=nrshc_mix(nrshc_mix(nrshc_mix(V.j,V.p),V.t),V.salt?nrshc_mix(0x5A17,++salt_n):0);if(V.salt)++s.salted;}
    const uint64_t h=nrshc_mix(hn,v);
    if(timed){const uint64_t dt=timing_clock()-t0;if(tsplit){s.sp_all+=dt;++s.sp_n;tsplit=false;}else{s.hash_ns+=dt;++s.hash_timed;}}++s.hashed;
    if(again){int bad=0;const bool d=nrshc_again(rg,again,ord,h,&bad);++ord;++s.re_draws;
        if(bad){++s.desync;red("execution 2: draw %u differs from execution 1 (range seq %u node %d, how %d) - switch off after this range",ord-1,R.seq,int(R.node),again);if(mode>0)mode=-2;}
        if(d && R.side==1)probe_note(h,ix,count);   // [shc4] check mode: a member drawn into the side-built layer - its inputs as they are now
        if(d)++s.re_drawn;return !d;}
    if(tw!=NRSHC_T_OK){++s.tw[tw];++s.tainted;if(tw==NRSHC_T_ORDER && nrs_last>=0 && nrs_last<NRS_W_N)++s.nrs[nrs_last];}
    if(R.diag)diag_draw(hs);
    const int skip=nrshc_draw(cas[R.k],&par,rg,h,hs,hn,nrshc_vtag(V.j,V.p,V.t),v,tw==NRSHC_T_OK);
    ++s.draws;++s.cdraws[R.k];if(skip){++s.cskip[R.k];++s.skipped;if(R.vprobe)probe_check(h,ix,count);}   // [shc4] a member of a verified hit: did its inputs change since the build?
    return mode!=NRSHC_CHECK && skip;}
// [shc3] an OP_UPDATE_UB inside an open range (execution 1): its old bytes are noted for execution 2 (read before the record writes them)
void upd(const uint8_t *p,size_t n){if(n<16)return;uint64_t a[2];memcpy(a,p,16);if(!a[0] || a[1]>n-16)return;   // the record checks it again
    ++s.upd;s.upd_bytes+=a[1];if(ud->over)return;
    if(gscull_mask()&64)cpu_access(a[0],a[1],11);   // as the record does before it writes: pending GPU results of these bytes first
    const void *cur=reinterpret_cast<const void*>(uintptr_t(a[0]));evguard::EngineRead er(cur,a[1]);
    if(!nrshc_undo_note(ud,a[0],cur,uint32_t(a[1])))++s.undo_over;}
// a record while a range is open (execution 1 only; [shc3] an OP_UPDATE_UB starts a new uniform memo generation in execution 2 too)
void op(uint32_t o,const uint8_t *p,size_t n){
    if(o==OP_UPDATE_UB){++memo_gen;if(!again && !R.refused)upd(p,n);return;}   // the bytes of a range may change under its address: hash again
    if(again)return;
    switch(o){
    case OP_END:case OP_JUMP:case OP_BEGIN:case OP_CALL:case OP_SET_RT:case OP_VIEWPORTS:case OP_SCISSOR:case OP_DEPTH_RANGE:case OP_BIND_PROGRAM:case OP_BIND_STATE:
    case OP_BIND_VATTR:case OP_BIND_VSTREAM:case OP_BIND_VBUF:case OP_BIND_UBO:case OP_BIND_TEX:case OP_BIND_SAMP:case OP_STENCIL:case OP_POLY_OFFSET:case OP_DRAW:case OP_DRAW_IDX:
    case OP_BLEND_COLOR:case OP_COPY_STRIDE:case OP_COPY_IMAGE_STRIDE:case OP_X_PATCH_SIZE:case OP_X_DEPTH_BOUNDS:case OP_X_DRAW_CALL:case OP_X_DRAW_INDIRECT:case OP_X_MULTI_DRAW_INDIRECT:
    case OP_X_PRIM_RESTART:case OP_X_DEPTH_CLAMP:case OP_X_RAST_DISCARD:case OP_X_FENCE_SYNC:case OP_X_ALPHA_REF:case OP_X_ENGINE_MARK:case OP_X_NB_PASS:case OP_X_NB_PASS_END:
    case OP_X_NB_DRAW:case OP_X_NB_STATE:case OP_CLEAR_DS:return;
    case OP_BARRIER:return;   // [shc2] a memory barrier changes no byte (DOS2 keeps one inside a cascade pass: IR "barriers 1"); execution 2 repeats it harmlessly
    case OP_X_NR_STATE:return;   // [shc2] NR-09S3 (DM_NR_STATE=4 / 5): only State fields, every one the hash reads from `state` (programs, DS, stencil, poly, blend, vertex format / buffers, alpha ref)
    default:   // [shc2] counted by where it is: an armed range is tainted; before the first draw the range is refused; in a refused range it is nothing
        if(R.armed && rg->live){rg->tainted=1;++s.tainted;++s.top[o&127];}
        else if(!R.armed){if(!R.refused){R.refused=true;++s.refused;++s.rf[NRSHC_RF_OP];}++s.rop[o&127];}
        else ++s.ignored;}}
void op_end(unsigned depth){if(!again && depth<=R.depth)++ends;}
// [shc2] the reasons line, every 10 s with the main line: why ranges were refused, what tainted the armed ones (ops by opcode, draws by
// NRSHC_T_* / nrs_why), ops inside refused ranges (not a taint), and per cascade how stable the draws were (known from the previous range /
// new by uniform bytes only / new otherwise / not compared); then the sampled uniform diff when it compared something.
struct Txt{char b[1536];size_t k=0;void put(const char *fmt,...) __attribute__((format(printf,2,3)));};
void Txt::put(const char *fmt,...){if(k>=sizeof b-1)return;va_list a;va_start(a,fmt);const int w=vsnprintf(b+k,sizeof b-k,fmt,a);va_end(a);if(w>0)k=std::min(sizeof b-1,k+size_t(w));}
void top_ops(Txt &t,uint64_t *v,int n){int shown=0;   // the 4 most counted opcodes (the counts are consumed)
    for(int round=0;round<4;round++){int best=-1;for(int i=0;i<n;i++)if(v[i] && (best<0 || v[i]>v[best]))best=i;if(best<0)break;
        t.put("%sop %d x %llu",shown++?", ":" (",best,(unsigned long long)v[best]);v[best]=0;}
    if(shown)t.put(")");}
void why_line(double fr){
    Txt t;t.b[0]=0;int n=0;
    t.put("refused %llu [",(unsigned long long)s.refused);for(int i=1;i<NRSHC_RF_N;i++)if(s.rf[i])t.put("%s%s %llu",n++?", ":"",nrshc_rf_name(i),(unsigned long long)s.rf[i]);t.put("]");
    if(s.rf[NRSHC_RF_OP]){t.put(" before the first draw:");top_ops(t,s.rop,128);}
    uint64_t dt=0;for(int i=1;i<NRSHC_T_N;i++)dt+=s.tw[i];
    t.put("; tainted ranges %llu: by ops %llu",(unsigned long long)s.tranges,(unsigned long long)(s.tainted-dt));top_ops(t,s.top,128);
    t.put(", by draws %llu [",(unsigned long long)dt);n=0;
    for(int i=1;i<NRSHC_T_N;i++)if(s.tw[i]){t.put("%s%s %llu",n++?", ":"",nrshc_t_name(i),(unsigned long long)s.tw[i]);
        if(i==NRSHC_T_ORDER){int m=0;t.put(" (");for(int j=1;j<NRS_W_N;j++)if(s.nrs[j])t.put("%s%s %llu",m++?", ":"",nrs_why_name(j),(unsigned long long)s.nrs[j]);t.put(")");}}
    t.put("]; ops in refused ranges %llu (no taint); draws a frame known / new by uniform bytes / new other / not compared:",(unsigned long long)s.ignored);
    for(int i=0;i<NRSHC_CAS;i++)if(s.known[i]+s.newu[i]+s.newo[i]+s.unc[i])t.put(" c%d %.0f/%.0f/%.0f/%.0f",i,s.known[i]/fr,s.newu[i]/fr,s.newo[i]/fr,s.unc[i]/fr);
    xbg_logf("[nr] shcache why (last 10 s): %s",t.b);
    // [shc3] a second line (xbg_logf cuts at 1023 characters): the refused images, the same stability numbers per armed range (the "a frame"
    // numbers above divide by every FRAME record, loading screens too: a window that holds a level load reads ~0), the uniform updates inside
    // ranges and their undo, the executions 2 by how and the draws they saw (must equal the draws of execution 1: no red line)
    Txt w;w.b[0]=0;int m=0;
    // which images the refusals were (w x h, VkFormat, aspect bits 2 = depth 4 = stencil; IR draws a range): the D24S8 scene pre-pass
    // (2160x972, VK_FORMAT_D24_UNORM_S8_UINT = 129, aspects 6) is right to refuse; the D16 atlas (124 or X8_D24 125, aspects 2) never is
    w.put("refused images:");for(RefImg &r:refimg)if(r.n){w.put("%s %ux%u vk %u asp %u x%llu (%.0f IR draws)",m++?",":"",r.w,r.h,r.fmt,r.asp,(unsigned long long)r.n,double(r.draws)/double(r.n));r=RefImg{};}
    if(refimg_other)w.put("%s other %llu",m++?",":"",(unsigned long long)refimg_other);if(!m)w.put(" none");refimg_other=0;
    w.put("; frames with an armed range %llu of %.0f; per armed range known / new by uniform bytes / new other / not compared:",(unsigned long long)s.aframes,fr);
    for(int i=0;i<NRSHC_CAS;i++)if(s.cranges[i]){const double r=double(s.cranges[i]);w.put(" c%d (%llu ranges) %.0f/%.0f/%.0f/%.0f",i,(unsigned long long)s.cranges[i],s.known[i]/r,s.newu[i]/r,s.newo[i]/r,s.unc[i]/r);}
    w.put("; uniform updates in ranges %llu (%.1f KB), undone for execution 2 %llu, log full %llu; executions 2: rest %llu all %llu drawn %llu, %llu draws seen",(unsigned long long)s.upd,s.upd_bytes/1024.,
        (unsigned long long)s.undo_runs,(unsigned long long)s.undo_over,(unsigned long long)s.runs[NRSHC_R_REST],(unsigned long long)s.runs[NRSHC_R_ALL],(unsigned long long)s.runs[NRSHC_R_DRAWN],(unsigned long long)s.run_draws);
    xbg_logf("[nr] shcache why2 (last 10 s): %s",w.b);
    // [shc4] the content versions: what the draws' bytes were versioned by, how many draws were no member for want of a version, the draws
    // new by content only (state + uniform bytes known from the previous range, a version moved: J backend writes, P CPU-written pages, T
    // images / samplers) per cascade and armed range, atlas passes outside a range, the journal, and the verify's member probes
    Txt x;x.b[0]=0;
    x.put("byte ranges versioned %llu: CPU_NO_ACCESS pool %llu, CPU pool protected and unchanged %llu, protected now %llu, volatile %llu, untracked %llu, trusted %llu; images %llu (GPU-written %llu), texel ranges %llu; draws without a version (no member) %llu;",
        (unsigned long long)s.v_ranges,(unsigned long long)s.v_nocpu,(unsigned long long)s.v_cpu_ok,(unsigned long long)s.v_cpu_prot,(unsigned long long)s.v_cpu_vol,(unsigned long long)s.v_cpu_untr,(unsigned long long)s.v_trust,
        (unsigned long long)s.v_tex,(unsigned long long)s.v_tex_gpu,(unsigned long long)s.v_texel,(unsigned long long)s.salted);
    x.put(" new by content per armed range (J backend writes / P CPU pages / T images):");
    for(int i=0;i<NRSHC_CAS;i++)if(s.cranges[i]){const double r=double(s.cranges[i]);x.put(" c%d %.1f (%.1f/%.1f/%.1f)",i,s.ncont[i]/r,s.ncc[i][0]/r,s.ncc[i][1]/r,s.ncc[i][2]/r);}
    x.put("; atlas passes outside a range %llu; journal writes %llu (%.1f KB), pools %llu; verify probes: members noted %llu, compared %llu, changed vertex %llu index %llu texture %llu, not noted %llu",
        (unsigned long long)s.outside,(unsigned long long)s.wj_writes,s.wj_bytes/1024.,(unsigned long long)s.pool_events,(unsigned long long)s.pr_noted,(unsigned long long)s.pr_cmp,
        (unsigned long long)s.pr_vb,(unsigned long long)s.pr_ib,(unsigned long long)s.pr_tex,(unsigned long long)s.pr_miss);
    xbg_logf("[nr] shcache inputs (last 10 s): %s",x.b);
    if(!s.dg_ranges)return;
    Txt u;u.b[0]=0;
    u.put("ranges compared %llu, draws %llu: same uniforms %llu, uniform bytes changed %llu, no structure match %llu (bytes not kept %llu); changed by (stage slot size):",
        (unsigned long long)s.dg_ranges,(unsigned long long)s.dg_draws,(unsigned long long)s.dg_equal,(unsigned long long)s.dg_changed,(unsigned long long)s.dg_nomatch,(unsigned long long)s.dg_short);
    static const char *const stn[5]={"vs","tc","te","gs","fs"};
    DiagAgg top[4];uint32_t keys[4];int ntop=0;
    for(int round=0;round<4;round++){auto best=dg_agg.end();for(auto it=dg_agg.begin();it!=dg_agg.end();++it)if(it->second.draws && (best==dg_agg.end() || it->second.draws>best->second.draws))best=it;
        if(best==dg_agg.end())break;const uint32_t key=best->first;const DiagAgg &g=best->second;
        u.put("%s %s u%u %u B: %llu draws, rows %#llx (bit i = bytes 16i..16i+15 of the first 1 KB), %llu past 1 KB, max %u rows a draw",round?";":"",stn[(key>>24)<5?(key>>24):0],(key>>16)&255u,key&65535u,
            (unsigned long long)g.draws,(unsigned long long)g.mask,(unsigned long long)g.beyond,g.rows_max);
        top[ntop]=g;keys[ntop++]=key;best->second.draws=0;}
    xbg_logf("[nr] shcache uniforms (sampled: range pairs 1 of every %u per cascade, last 10 s): %s",diag_every,u.b);
    // [shc3] one line per block: the 4-byte words that moved (bit i = bytes 4i..4i+3 of the first 256 B) and whether every changed draw moved
    // them by the deltas of the first one (a shift common to all casters: camera-relative positions / a re-snapped cascade matrix) or not
    // (per object: animation, wind), with the first changed word of the first draw as an example
    for(int i=0;i<ntop;i++){const DiagAgg &g=top[i];const uint32_t key=keys[i];
        if(g.ex_w>=0)xbg_logf("[nr] shcache words (%s u%u %u B, %llu draws): words %#llx, deltas as the first draw's %llu / other %llu (e.g. word %d %.7g -> %.7g)",stn[(key>>24)<5?(key>>24):0],(key>>16)&255u,key&65535u,
            (unsigned long long)g.draws,(unsigned long long)g.wmask,(unsigned long long)g.same,(unsigned long long)g.other,g.ex_w,double(g.ex_a),double(g.ex_b));
        else xbg_logf("[nr] shcache words (%s u%u %u B, %llu draws): no word of the first 256 B moved",stn[(key>>24)<5?(key>>24):0],(key>>16)&255u,key&65535u,(unsigned long long)g.draws);}}
void frame(const p_nb_frame &f){if(mode==-1)init();if(mode==-3){release();mode=0;}if(mode<=0)return;   // [shc3] -3: switched off, the images go once
    if(!frame_seen || f.frame_no!=cur_frame){cur_frame=f.frame_no;frame_seen=true;arms=0;used=0;ntiles=0;dirty=false;++s.frames;}
    const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const double fr=s.frames?double(s.frames):1.;s.verify_eq=v_eq.exchange(0);s.verify_ne=v_ne.exchange(0);
    char cb[256];int k=0;cb[0]=0;for(int i=0;i<NRSHC_CAS;i++)if(s.cdraws[i])k+=snprintf(cb+k,sizeof cb-size_t(k)," c%d %.0f/%.0f",i,s.cskip[i]/fr,s.cdraws[i]/fr);
    xbg_logf("[nr] shcache (DM_NR_SHCACHE=%d, last 10 s, %.0f frames): ranges %llu armed %llu (not hashed: quiet %llu) refused %llu tainted %llu; %s %llu (ok %llu, recover %llu), build %llu (layer %llu, rest %llu), normal %llu "
        "[first %llu clear %llu dirty %llu rect %llu prev %llu few %llu layer %llu age %llu backend %llu]; draws %.0f a frame, %s %.0f a frame (%.1f %%;%s skipped/drawn by cascade); execution 2: %.0f draws a frame "
        "drawn %.0f; hash %.2f us a draw (ubo %.1f KB a frame, memo %llu); copies %.1f a frame; verify equal %llu differ %llu, side builds %llu; unrepairable %llu desync %llu; red %u",
        mode<0?0:mode,fr,(unsigned long long)s.ranges,(unsigned long long)s.armed,(unsigned long long)s.blind,(unsigned long long)s.refused,(unsigned long long)s.tainted,mode==NRSHC_CHECK?"would hit":"hit",
        (unsigned long long)s.act[NRSHC_A_HIT],(unsigned long long)s.hit_ok,(unsigned long long)s.recover,(unsigned long long)s.act[NRSHC_A_BUILD],(unsigned long long)s.build_ok,(unsigned long long)s.rest,
        (unsigned long long)s.act[NRSHC_A_NORMAL],(unsigned long long)s.why[NRSHC_W_FIRST],(unsigned long long)s.why[NRSHC_W_CLEAR],(unsigned long long)s.why[NRSHC_W_DIRTY],(unsigned long long)s.why[NRSHC_W_RECT],
        (unsigned long long)s.why[NRSHC_W_PREV],(unsigned long long)s.why[NRSHC_W_FEW],(unsigned long long)s.why[NRSHC_W_LAYER],(unsigned long long)s.why[NRSHC_W_AGE],(unsigned long long)s.why[NRSHC_W_OFF],
        s.draws/fr,mode==NRSHC_CHECK?"would skip":"skipped",s.skipped/fr,s.draws?100.*double(s.skipped)/double(s.draws):0.,cb,s.re_draws/fr,s.re_drawn/fr,s.hash_timed?double(s.hash_ns)/double(s.hash_timed)/1000.:0.,
        s.ubo_bytes/fr/1024.,(unsigned long long)s.ubo_memo,s.copies/fr,(unsigned long long)s.verify_eq,(unsigned long long)s.verify_ne,(unsigned long long)s.side_builds,
        (unsigned long long)s.unrepairable,(unsigned long long)s.desync,reds);
    why_line(fr);
    // [NR-ZERO] DM_NR_SHC_MEMO: the memos' answers and the hash by part (split draws, 1 in 128: their clock reads are in the parts)
    if(zmode || s.sp_n){const double sn=s.sp_n?double(s.sp_n):1.;const double all=s.sp_all/sn/1000.,ub=s.sp_ubo/sn/1000.,vx=s.sp_vtx/sn/1000.,tx=s.sp_tex/sn/1000.;
        xbg_logf("[nr] shcache memo (DM_NR_SHC_MEMO=%d, NR-ZERO, last 10 s): texture nodes asked %.0f a frame, memo %.0f (%.1f %%), compared %llu bad %llu; pool tables asked %.0f, memo %.0f, bad %llu; "
            "page versions asked %.0f a frame, memo %.0f (%.1f %%, %.0f pages a frame not scanned), compared %llu bad %llu, tracker busy at the compare %llu; "
            "hash by part (%llu split draws): %.2f us = uniform bytes %.2f + vertex / index versions %.2f + textures %.2f + state, order test and the rest %.2f",
            zmode,s.zt_q/fr,s.zt_hit/fr,s.zt_q?100.*double(s.zt_hit)/double(s.zt_q):0.,(unsigned long long)s.zt_cmp,(unsigned long long)s.zt_bad,s.zp_q/fr,s.zp_hit/fr,(unsigned long long)s.zp_bad,
            s.zv_q/fr,s.zv_hit/fr,s.zv_q?100.*double(s.zv_hit)/double(s.zv_q):0.,s.zv_pages/fr,(unsigned long long)s.zv_cmp,(unsigned long long)s.zv_bad,(unsigned long long)s.zv_nf,
            (unsigned long long)s.sp_n,all,ub,vx,tx,all-ub-vx-tx);}
    s=Stats{};dg_agg.clear();}
void pass_end(const p_nb_pass_end *e,const uint8_t *pc,unsigned depth,uint64_t &budget);
void pass(const p_nb_pass &h,const uint8_t *pc,const uint8_t *next,unsigned depth,uint64_t &budget){
    if(mode<=0 || again)return;
    if(live)pass_end(nullptr,pc,depth,budget);   // no PASS_END before the next PASS: the range ends here
    const bool one=h.patched && h.plan_ok && h.natt==1 && h.slot[0]==8 && (h.load[0]==NB_LOAD || h.load[0]==NB_LOAD_CLEAR) && h.store[0]==NB_STORE && !(h.cover_dc&1) && h.draws>=8;
    // [shc4] a pass with draws on the atlas that is no range (< 8 draws, another shape) writes the atlas outside the tiles' ranges: a layer
    // copied over its tile would erase what it drew, so the frame's later ranges are not cached (as a partial clear: `dirty`)
    if(!one){if(A.key && h.draws){for(unsigned i=0;i<h.natt && i<NB_REC_ATT;i++)if(h.att[i]==A.key){dirty=true;++s.outside;break;}}return;}
    ++s.ranges;live=true;R.armed=false;R.refused=false;R.in_clear=false;R.k=-1;R.tex=nullptr;R.depth=depth;R.ends0=ends;R.atlas=h.att[0];R.start=next;R.seq=h.seq;R.node=h.node;R.load=h.load[0];
    R.snap=state;R.snap_missing=missing_program;R.pdraws=h.draws;nrshc_undo_reset(ud);
    R.side=0;R.vprobe=false;R.pcmp=R.pvb=R.pib=R.ptex=R.pmiss=0;R.pdesc.clear();}   // [shc4]   // [shc3] a new undo log per range
// check mode, after the range drew as usual: the would-be layer made for real (side build) or the would-be hit compared on the GPU
void side(int e,uint64_t &budget,const uint8_t *stop){
    const int k=R.k;if(verify_every<=0 || !A.S || !A.X || !A.Z || cur_frame<A.vnext[k])return;
    Image &a=get_image(R.tex,false);
    if(e==NRSHC_E_BUILD){A.vnext[k]=cur_frame+uint32_t(verify_every);clear_z(R.clear_bits);PR[k].clear();   // [shc4] the members' probes are noted in execution 2
        copy_tile(a,*A.X,R.rect);copy_tile(*A.Z,a,R.rect);R.side=1;again_run(NRSHC_R_DRAWN,stop,budget);R.side=0;copy_tile(a,*A.S,R.rect);copy_tile(*A.X,a,R.rect);A.real[k]=true;++s.side_builds;return;}
    if(rg->action==NRSHC_A_HIT && e==NRSHC_E_NONE && A.real[k]){A.vnext[k]=cur_frame+uint32_t(verify_every);
        const size_t n=size_t(R.rect[2])*size_t(R.rect[3])*A.bpp;Buffer b1=readback_buffer(n),b2=readback_buffer(n);
        copy_tile(a,*A.X,R.rect);copy_tile(*A.S,a,R.rect);again_run(NRSHC_R_DRAWN,stop,budget);
        tile_to_buffer(a,R.rect,b2);tile_to_buffer(*A.X,R.rect,b1);copy_tile(*A.X,a,R.rect);
        const uint32_t seq=R.seq;const int16_t node=R.node;const int32_t w=R.rect[2];
        const uint32_t pc=R.pcmp,pv=R.pvb,pi=R.pib,pt=R.ptex,pm=R.pmiss;const std::string pd=R.pdesc;   // [shc4] the members' probes of this hit
        recording_slot().retire.handles.push_back([b1,b2,n,seq,node,w,k,pc,pv,pi,pt,pm,pd]()mutable{
            if(!b1.coherent || !b2.coherent){VkMappedMemoryRange r[2]{{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE},{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}};r[0].memory=b1.memory;r[0].size=VK_WHOLE_SIZE;r[1].memory=b2.memory;r[1].size=VK_WHOLE_SIZE;vkInvalidateMappedMemoryRanges(cfg.device,2,r);}
            const uint8_t *x=static_cast<const uint8_t*>(b1.mapped),*y=static_cast<const uint8_t*>(b2.mapped);
            if(!memcmp(x,y,n)){++v_eq;return;}
            ++v_ne;size_t i=0,bad=0;while(i<n && x[i]==y[i])++i;for(size_t j=0;j<n;j++)if(x[j]!=y[j])++bad;
            const size_t px=i/A.bpp;red("verify: cascade %d (range seq %u node %d): the cached tile differs from the drawn one in %zu bytes, first at pixel (%zu, %zu)",k,seq,int(node),bad,px%size_t(w),px/size_t(w));
            // [shc4] which members' inputs moved since the layer was built (none: the cause is not an input of the members)
            static unsigned told=0;if(told++<16)xbg_logf("[nr] shcache verify detail: cascade %d seq %u: members probed %u, inputs changed since the build: vertex %u index %u texture %u (members not noted %u)%s%s",
                k,seq,pc,pv,pi,pt,pm,pd.empty()?"":"; ",pd.empty()?(pc?"; no member input changed: the difference is not in the members' bytes":""):pd.c_str());});}}
void pass_end(const p_nb_pass_end *e,const uint8_t *pc,unsigned depth,uint64_t &budget){
    if(!live || again)return;
    if(e && (e->seq!=R.seq || e->node!=R.node))return;
    live=false;
    if(!R.armed || R.refused || R.k<0){if(R.armed && R.k>=0 && rg->live){rg->tainted=1;(void)nrshc_end(cas[R.k],&par,rg,R.atlas,R.rect,R.clear_bits,0);}return;}
    const bool simple=depth==R.depth && ends==R.ends0 && !ud->over;   // [shc3] a full update log: not executable again either
    const bool broke=rg->tainted || rg->overflow;
    const int act=rg->action;
    if(broke)++s.tranges;++s.cranges[R.k];s.known[R.k]+=rg->known;s.newu[R.k]+=rg->new_ubo;s.newo[R.k]+=rg->new_other;s.unc[R.k]+=rg->unc;   // [shc2]
    s.ncont[R.k]+=rg->new_content;for(int i=0;i<NRSHC_VC_N;i++)s.ncc[R.k][i]+=rg->nc[i];   // [shc4]
    const int r=nrshc_end(cas[R.k],&par,rg,R.atlas,R.rect,R.clear_bits,simple);
    if(broke)dirty=true;   // a draw outside the tile / an unknown op: later cascades of this frame are not cached
    if(act==NRSHC_A_HIT && r==NRSHC_E_NONE)++s.hit_ok;
    if(r==NRSHC_E_RECOVER || (r<0 && act==NRSHC_A_HIT))++s.recover;
    if(r==NRSHC_E_BUILD)++s.build_ok;if(r==NRSHC_E_REST)++s.rest;
    if(r==NRSHC_E_RECOVER || r==NRSHC_E_BUILD)A.real[R.k]=false;
    if(mode==NRSHC_CHECK){if(r<0)++s.unrepairable;else if(simple && (r==NRSHC_E_BUILD || r==NRSHC_E_NONE))side(r,budget,pc);if(mode==-2)switch_off();return;}
    if(r<0){++s.unrepairable;red("range seq %u node %d (cascade %d, %s) cannot be executed again (it crosses a command handle, or its uniform update log is full: %d): this frame's tile may be wrong - switch off",
        R.seq,int(R.node),R.k,act==NRSHC_A_HIT?"hit":"build",ud->over);switch_off();return;}
    Image &a=get_image(R.tex,false);
    switch(r){
    case NRSHC_E_BUILD:copy_tile(a,*A.S,R.rect);A.real[R.k]=true;again_run(NRSHC_R_REST,pc,budget);break;
    case NRSHC_E_REST:again_run(NRSHC_R_REST,pc,budget);break;
    case NRSHC_E_RECOVER:clear_z(R.clear_bits);copy_tile(*A.Z,a,R.rect);again_run(NRSHC_R_ALL,pc,budget);break;
    default:break;}
    if(mode==-2)switch_off();}
void clear_ds(const p_clear_ds &c){if(mode<=0 || again)return;
    const tex_t *t=state.rt.depth;if(!t || !c.depth_mask)return;const uint64_t key=img_key(t);
    const VkRect2D &sc=state.scissor;
    const bool full=sc.offset.x<=0 && sc.offset.y<=0 && int64_t(sc.offset.x)+int64_t(sc.extent.width)>=int64_t(t->d.width) && int64_t(sc.offset.y)+int64_t(sc.extent.height)>=int64_t(t->d.height);
    if(live && key==R.atlas){if(R.armed && rg->live)rg->tainted=1;else R.in_clear=true;}   // a clear inside a range: execution 2 would repeat it
    if(full){uint32_t bits;memcpy(&bits,&c.depth,4);FrameClear *f=nullptr;for(FrameClear &x:fcs)if(x.key==key)f=&x;if(!f)f=&fcs[fci++&7];
        f->key=key;f->frame=cur_frame;f->bits=bits;if(key==A.key){ntiles=0;dirty=false;}}   // the atlas cleared whole: no tile of this frame is drawn yet
    else{if(key==A.key)dirty=true;for(FrameClear &x:fcs)if(x.key==key)x.frame=0;}}   // a partial clear: the atlas is no longer "clear + tiles"
}
