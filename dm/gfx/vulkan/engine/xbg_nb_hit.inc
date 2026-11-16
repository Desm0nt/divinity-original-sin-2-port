// [DOS2] NB-12: the descriptor memo HIT path of descriptors() - included by xbg_game.cpp right before uniform_snapshot(). Decisions in
// dm/gfx/nb/nb_hit.h (pure C, host test nb_hit_test.c); reasoning there and in docs/dos2/NATIVE_BACKEND_PLAN.md "NB-12 findings".
//
// nbubo (DM_NB_UBO, read by the first OP_X_NB_DRAW, so only with DM_NB=1/2 + DM_NB_OPS bit 36/37): 0 (default) - nothing here runs
// (one `mode>0` test per uniform_snapshot); 1 - a uniform range an OP_X_NB_DRAW of this submission bound (the engine's constant ring,
// written once before its bind) is bound in place through its zero-copy chunk (no zcubo bypass, no snapshot map / front / copy);
// when it cannot be (zero-copy off, not mapped, pending GPU result over it, misaligned, sticky page) the usual path runs (one copy
// into the stream buffer on a front miss). 2 - the usual path is used; the in-place decision is made too and compared: bytes now
// (snapshot vs guest memory) and later (the guest range hashed again at every following submission while the slot that read it
// is still recording / in flight: a change = an in-place read would have seen other bytes). Line "[nb] ubo:" every 10 s.
//
// hitfast (DM_GFX_HIT_FAST, read at the first memo hit): 0 (default) - the hit path as before; 1 - an image of the entry whose
// get_image + prepare_read are no-ops (nb_hit_skip) skips both; 2 - the decision is made, the full path runs anyway and must leave
// the image / pass / barrier state as the decision said (bad = it did not). Line "[gfx] hit fast:" every 10 s.
#include "../../nb/nb_hit.h"

namespace nbubo{
int mode=-1;bool busy=false;
nb_ring_tab ring;
zc::Chunk *ch=nullptr;uint64_t ch_gen=0;
struct Stats{uint64_t calls=0,ring=0,direct=0,why[NBU_N]{},cmp=0,bad_bytes=0,race_checks=0,race_bad=0,notes=0,timed=0,ns=0;} s;
struct Race{uint64_t addr,bytes,hash,serial;unsigned slot;};std::vector<Race> races;
uint64_t last_epoch=0,next_ns=0;unsigned told=0;
void init(){if(mode>=0)return;const char *e=getenv("DM_NB_UBO");mode=e && *e?std::clamp(atoi(e),0,2):0;
    xbg_logf("[nb] DM_NB_UBO=%d (NB-12: uniform ranges an OP_X_NB_DRAW bound - the engine's constant ring, written once before the bind - %s)",mode,
             mode==1?"bound in place through zero-copy":mode==2?"snapshotted as usual and compared with the in-place view":"- off");}
void differs(const char *what,uint64_t a,uint64_t n){if(told>=16)return;++told;
    xbg_logf("[nb] ubo check differs: %s, range %#llx+%llu (frame %llu)",what,(unsigned long long)a,(unsigned long long)n,(unsigned long long)stats.presents.load());}
// Every submission (the first call that sees a new snapshot_epoch): races of the =2 entries, the 10 s line.
void submission(){
    if(!races.empty()){int ready[2]={-1,-1};size_t keep=0;   // as zcubo::check: the slot still recording it, or the next one with it in flight
        auto flying=[&](unsigned k){if(ready[k]<0)ready[k]=slots[k].pending && vkGetFenceStatus(cfg.device,slots[k].fence)==VK_NOT_READY;return ready[k]!=0;};
        for(auto &r:races){const Slot &sl=slots[r.slot];const bool on=(r.serial==slot_serial && sl.recording) || (r.serial+1==slot_serial && flying(r.slot));
            if(!on)continue;++s.race_checks;
            if(nb_hash64(reinterpret_cast<const void*>(uintptr_t(r.addr)),size_t(r.bytes))!=r.hash){++s.race_bad;differs("guest bytes changed while the submission that read them is in flight",r.addr,r.bytes);continue;}
            races[keep++]=r;}
        races.resize(keep);}
    const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns || !s.calls)return;next_ns=now+10000000000ull;
    xbg_logf("[nb] ubo: DM_NB_UBO=%d; %llu uniform snapshots, %llu of ring ranges (from OP_X_NB_DRAW, %llu noted): %s %llu; not in place: no zero-copy %llu, "
        "pending GPU result %llu, no chunk %llu, align %llu, sticky %llu, UI shift %llu; not a ring range %llu; bad %llu (bytes %llu of %llu compared, "
        "race %llu of %llu checks); decision %.0f ns (1 in 64)",mode,(unsigned long long)s.calls,(unsigned long long)s.ring,(unsigned long long)s.notes,
        mode==1?"in place":"would be in place",(unsigned long long)s.direct,(unsigned long long)s.why[NBU_NOZC],(unsigned long long)s.why[NBU_PENDING],
        (unsigned long long)s.why[NBU_CHUNK],(unsigned long long)s.why[NBU_ALIGN],(unsigned long long)s.why[NBU_STICKY],(unsigned long long)s.why[NBU_UI],
        (unsigned long long)s.why[NBU_NOTRING],(unsigned long long)(s.bad_bytes+s.race_bad),(unsigned long long)s.bad_bytes,(unsigned long long)s.cmp,
        (unsigned long long)s.race_bad,(unsigned long long)s.race_checks,s.timed?double(s.ns)/double(s.timed):0.);
    s=Stats{};}
inline void tick(){if(snapshot_epoch!=last_epoch){last_epoch=snapshot_epoch;submission();}}
// From nbdraw::apply after the record's binds landed: its uniform ranges are ring ranges for this submission.
void note(const nbd_hdr &h,const uint8_t *b){
    if(mode<0)init();if(mode<=0)return;tick();
    const nbd_bind *e=reinterpret_cast<const nbd_bind*>(b);
    for(unsigned i=0;i<h.nbind;i++){nbd_bind x;memcpy(&x,e+i,sizeof x);if(x.kind==NBD_B_UBO){nb_ring_note(&ring,x.a,x.b,snapshot_epoch);++s.notes;}}}
// uniform_snapshot's first step. 0 = the usual path; 1 = z is the in-place view (mode 1); 2 = mode 2: z is the view to compare with.
int fast(uint64_t address,size_t copy,Buffer &z){
    tick();++s.calls;const bool timed=!(s.calls&63);const uint64_t t0=timed?timing_clock():0;
    nb_ring_ent *e=nb_ring_find(&ring,address,copy,snapshot_epoch);int why=NBU_NOTRING;
    if(e){++s.ring;
        const size_t n=uniform_range(copy);
        if(g_ui_moff!=0 && address==g_ui_moff_addr)why=NBU_UI;
        else if(zc::enabled<=0)why=NBU_NOZC;
        else if(overlaps_pending(address,n))why=NBU_PENDING;
        else{zc::Chunk *c=ch;
            if(!c || !c->live || c->gen!=ch_gen || address<c->lo || address-c->lo+n>c->n){c=zc::find(address,n);ch=c;ch_gen=c?c->gen:0;}
            uint64_t off=0;why=c?nb_ubo_view(address,n,c->lo,c->n,properties.limits.minUniformBufferOffsetAlignment,c->sticky.data(),&off):NBU_CHUNK;
            if(why==NBU_DIRECT){z=Buffer{};z.buffer=c->buffer;z.memory=c->memory;z.offset=off;z.size=n;z.mapped=reinterpret_cast<void*>(uintptr_t(address));z.coherent=true;
                ++s.direct;if(mode==1){++zc::hits;zcubo::bound(address,n);}
                if(mode==2 && e->checked!=slot_serial){e->checked=slot_serial;if(races.size()<8192)races.push_back({address,uint64_t(copy),0,slot_serial,slot_index});}}}}
    if(why!=NBU_DIRECT)++s.why[why];
    if(timed){s.ns+=timing_clock()-t0;++s.timed;}
    return why!=NBU_DIRECT?0:mode==1?1:2;}
// mode 2, after the usual path made b: the bytes a GPU read in place would see now must be the snapshot's
void verify(uint64_t address,size_t copy,const Buffer &z,const Buffer &b){
    ++s.cmp;if(!b.mapped || memcmp(b.mapped,z.mapped,copy)){++s.bad_bytes;differs(b.mapped?"snapshot bytes != guest bytes at the bind":"snapshot not CPU-visible",address,copy);}
    if(!races.empty() && races.back().addr==address && races.back().serial==slot_serial && !races.back().hash)races.back().hash=nb_hash64(z.mapped,copy);}
}
namespace hitfast{
int mode=-1;
nb_hit_tab tab;
struct Stats{uint64_t images=0,skipped=0,why[NBH_N]{},bad=0,hits=0;} s;
uint64_t next_ns=0,calls=0;unsigned told=0;
void init(){if(mode>=0)return;const char *e=getenv("DM_GFX_HIT_FAST");mode=e && *e?std::clamp(atoi(e),0,2):0;
    xbg_logf("[gfx] DM_GFX_HIT_FAST=%d (NB-12: an image of a descriptor memo hit already checked in this submission %s)",mode,
             mode==1?"skips get_image + prepare_read":mode==2?"is decided and verified against the full path":"- off");}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns || !s.images)return;next_ns=now+10000000000ull;
    xbg_logf("[gfx] hit fast: DM_GFX_HIT_FAST=%d; %llu memo hits, %llu images, %s %llu; full path: storage %llu, hook %llu, first in submission %llu "
        "(new %llu, pending lists %llu), front %llu, content check %llu, layout %llu, written %llu, open pass %llu; bad %llu",mode,(unsigned long long)s.hits,
        (unsigned long long)s.images,mode==1?"skipped":"would skip",(unsigned long long)s.skipped,(unsigned long long)s.why[NBH_STORAGE],(unsigned long long)s.why[NBH_HOOK],
        (unsigned long long)(s.why[NBH_NEW]+s.why[NBH_SUB]),(unsigned long long)s.why[NBH_NEW],(unsigned long long)s.why[NBH_PEND],(unsigned long long)s.why[NBH_FRONT],
        (unsigned long long)s.why[NBH_CHECK],(unsigned long long)s.why[NBH_LAYOUT],(unsigned long long)s.why[NBH_WRITTEN],(unsigned long long)s.why[NBH_OPEN],(unsigned long long)s.bad);
    s=Stats{};}
inline bool on(){if(mode<0)init();return mode>0;}
// Once per memo hit: any read hook armed (then every image takes the full path)
inline int hooks(){return pgraph::armed || nbchk::armed || pclear_n || hist_on || draw_reads_on || slots[slot_index].prof || provenance_file || capture.active || !img_fast;}
inline void fill(nb_hit_in &x,const MemoImage &ti,int hk){const tex_t *t=ti.first;Image &im=*ti.second;
    x.tex=t;x.im=ti.second;x.serial=t->serial;x.mem=uint64_t(uintptr_t(t->mem));x.size=uint64_t(t->L.size);x.sub=stats.submissions;x.pend=hit_pend_gen;
    x.storage=ti.storage;x.hooks=hk;
    const ImgFront &f=img_front[(t->serial*0x9E3779B97F4A7C15ull)>>54];x.front_ok=f.epoch==images_epoch && f.serial==t->serial && f.im==ti.second;
    x.checked=im.checked_submission==stats.submissions;x.has_contents=im.uploaded || im.gpu_written;x.general=im.layout==VK_IMAGE_LAYOUT_GENERAL;
    x.last_write=im.last_write;x.storage_write=im.storage_write;x.seen=read_frag_only?frag_seq:barrier_seq;if(opbar::mode==1)x.seen=nb_ob_seen(x.seen,read_frag_only?im.opvis_frag:im.opvis_full);   /* [DOS2] NB-08: as prepare_read */x.sampled_ever=im.sampled_ever;
    x.in_open=open_pass.cmd && std::find(open_pass.images.begin(),open_pass.images.end(),ti.second)!=open_pass.images.end();}
// Before the full path of one image: true = skip it (mode 1). ok / snap: mode 2 state for after().
struct Pre{int why;VkCommandBuffer open;uint64_t bseq,fseq,pend;VkImageLayout layout;};
inline bool before(const MemoImage &ti,int hk,Pre &p){++s.images;nb_hit_in x;fill(x,ti,hk);p.why=nb_hit_skip(&tab,&x);
    if(p.why==NBH_SKIP){++s.skipped;if(mode==1){ti.second->slot_use=slot_serial;return true;}}else ++s.why[p.why];
    p.open=open_pass.cmd;p.bseq=barrier_seq;p.fseq=frag_seq;p.pend=hit_pend_gen;p.layout=ti.second->layout;return false;}
// After the full path (get_image returned im): note it; mode 2: a skip decision must have been a no-op.
inline void after(const MemoImage &ti,const Image &im,int hk,const Pre &p){
    if(mode==2 && p.why==NBH_SKIP && (&im!=ti.second || open_pass.cmd!=p.open || barrier_seq!=p.bseq || frag_seq!=p.fseq || hit_pend_gen!=p.pend || im.layout!=p.layout)){
        ++s.bad;if(told<16){++told;xbg_logf("[gfx] hit fast check differs: serial %llu: %s",(unsigned long long)ti.first->serial,&im!=ti.second?"get_image returned another image":
            open_pass.cmd!=p.open?"the full path ended the open pass":barrier_seq!=p.bseq || frag_seq!=p.fseq?"the full path recorded a barrier":hit_pend_gen!=p.pend?"the full path resolved pending results":"the layout changed");}}
    if(ti.storage || hk || &im!=ti.second)return;
    nb_hit_in x;fill(x,ti,hk);nb_hit_note(&tab,&x);}
inline void hit(){++s.hits;if(!(++calls&4095))tick();}
}
