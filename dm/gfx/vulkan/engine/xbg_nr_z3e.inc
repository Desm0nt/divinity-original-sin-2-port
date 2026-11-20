// [DOS2] NR-ZERO Z3b / Z3c / Z3d (docs/dos2/nr/NR-ZERO.md section 13): the epochs that let a "live" step of an engine draw be checked by
// one word. Included by xbg_game.cpp right after snapshot_slow() (it reads zc, nrbuf, the pending-writeback generation); the pure part
// - the field types that move the image epoch, the tables, the ring word - is dm/gfx/nr/nr_z3e.h (host test tests/nr_z3e_test.cpp).
//
//   DM_NR_Z3B  the image word (needs DM_NR_DESC=1). An NR-08 entry whose last walk found every image quiet (nr_dimg_skip == NRD_I_SKIP)
//              is not walked again while the image epoch (nrz_img_ep: every write of an Image field that can end "quiet", by the
//              field's type; ~Image), the submission number and the slot serial stand. Both walks: serve() and nrz3::draw.
//   DM_NR_Z3C  the mesh views (needs DM_NR_DRAW=1 or DM_NR_UI=1: the lane's tail asks). The vertex / index range of a draw is answered
//              from a table when it was answered before by a zero-copy view in place (words: zc::live_ep, the chunk's wr_ver, the slot
//              serial, hit_pend_gen; only while NR-06's static copies are off) or by a static copy of NR-06 (words: the snapshot epoch,
//              nrbuf::st_ep, hit_pend_gen). Any other answer of snapshot() (a copy, the front cache, a pending GPU result) is not kept.
//   DM_NR_Z3D  the ring word (needs DM_NR_DESC=1 and the constant ring in place: zero-copy devices). xbg_nr_buf.inc ubo_fast.
//   each:      0  default (xbg_nr_z3b_default / _z3c_ / _z3d_): one load and a not-taken branch at its place
//              1  on
//              2  verify: the word / the table is asked, the walk / snapshot() / the page walk runs and is used; a standing word the
//                 walk contradicts is counted bad and logged red (first 16)
//              3  does not exist: nothing is drawn differently, there is nothing to poison (red line, off)
//   DM_NR_Z3C_MAX  entries of the view table (4096; a power of two, 256..65536; 88 bytes each)
// Lines: "[nr] DM_NR_Z3B=... DM_NR_Z3C=... DM_NR_Z3D=..." at the start, "[nr] z3 epochs: ..." every 10 s, red "【红】[nr] z3 epochs: ...".
namespace nrzx{
nrz_vtab vt{};uint32_t vmax=4096;uint64_t next_ns=0;bool read=false;
void red(const char *fmt,...){if(told>=16)return;++told;char b[640];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof b,fmt,ap);va_end(ap);
    xbg_logf("【红】[nr] z3 epochs: %s (frame %llu)",b,(unsigned long long)stats.presents.load());}
// From nrz3::init, once NR-08 and NR-09 read their switches.
void init(int desc_mode,int draw_mode,int ui_mode){
    if(read)return;read=true;
    auto num=[](const char *n,int d){const char *e=getenv(n);int v=e && *e?atoi(e):d;
        if(v==3){xbg_logf("【红】[nr] %s=3 does not exist (nothing is drawn differently, nothing to poison; =2 compares): off",n);v=0;}
        if(v<0 || v>2){xbg_logf("【红】[nr] %s=%d: only 0 / 1 / 2: off",n,v);v=0;}return v;};
    int b=num("DM_NR_Z3B",xbg_nr_z3b_default()),c=num("DM_NR_Z3C",xbg_nr_z3c_default()),d=num("DM_NR_Z3D",xbg_nr_z3d_default());
    if(b && desc_mode!=1){xbg_logf("【红】[nr] DM_NR_Z3B=%d needs DM_NR_DESC=1 (now %d): off",b,desc_mode);b=0;}
    if(d && desc_mode!=1){xbg_logf("【红】[nr] DM_NR_Z3D=%d needs DM_NR_DESC=1 (now %d): off",d,desc_mode);d=0;}
    if(d && nrbuf::ring_mode!=nrbuf::RING_INPLACE){xbg_logf("[nr] DM_NR_Z3D=%d: the constant ring is not served in place on this device (the mirror copies granules at their first bind - nothing to remember): off",d);d=0;}
    if(c && draw_mode!=1 && ui_mode!=1){xbg_logf("【红】[nr] DM_NR_Z3C=%d needs DM_NR_DRAW=1 (now %d) or DM_NR_UI=1 (now %d): off",c,draw_mode,ui_mode);c=0;}
    if(c){const char *e=getenv("DM_NR_Z3C_MAX");uint32_t n=e && *e?uint32_t(std::clamp(atoi(e),256,65536)):4096u;while(n&(n-1))n&=n-1;vmax=n;
        if(nrz_vinit(&vt,vmax)){xbg_logf("【红】[nr] DM_NR_Z3C=%d: no memory for %u views: off",c,vmax);c=0;}}
    bmode=b;cmode=c;dmode=d;
    if(b || c || d)xbg_logf("[nr] DM_NR_Z3B=%d DM_NR_Z3C=%d DM_NR_Z3D=%d (NR-ZERO Z3b / Z3c / Z3d; 1 on, 2 verify - the walk runs and must agree): Z3b an NR-08 entry whose images were all quiet is "
        "not walked while the image epoch, the submission and the slot serial stand; Z3c the vertex / index range of a draw from a table of %u views (in place: chunk liveness epoch, the chunk's "
        "write version, slot serial, pending-writeback generation%s; static copy: snapshot epoch, NR-06's static epoch, pending-writeback generation%s); Z3d a ring block in place = address - chunk "
        "base while the ring's chunk is the only live one over it and none of its pages is sticky (constant ring %s)",b,c,d,vmax,nrbuf::st_mode>0?" - not used here: NR-06's static copies are on":"",
        nrbuf::st_mode>0?"":" - not used here: static copies are off",nrbuf::ring_mode==nrbuf::RING_INPLACE?"in place":nrbuf::ring_mode==nrbuf::RING_MIRROR?"mirrored":"off");}
void release(){nrz_vfree(&vt);bmode=cmode=dmode=0;read=false;s=Stats{};}
// The lane's vertex (site 1: stride = the binding's stride, NR-06's placement hint) / index (site 2) range. Without the table this is
// exactly the statement it replaces: snap_site, the hint, snapshot(), both cleared.
inline Buffer view_real(uint64_t address,size_t bytes,int site,uint32_t stride){
    snap_site=unsigned(site);if(site==1)nrbuf::stride_hint=stride;const Buffer b=snapshot(address,bytes);if(site==1)nrbuf::stride_hint=0;snap_site=0;return b;}
Buffer view(uint64_t address,size_t bytes,int site,uint32_t stride){
    if(!vt.e || snap_no_sync || nrbuf::busy || nrbuf::mode==2)return view_real(address,bytes,site,stride);
    ++s.c_asked;nrz_view *e=nrz_vslot(&vt,address,bytes);bool fresh=false;
    if(nrz_vmatch(e,address,bytes,uint32_t(site))){
        if(e->kind==NRZV_INPLACE){
            if(zc::enabled>0 && nrbuf::st_mode<=0 && e->chunk>=0 && e->chunk<zc::MAX_CHUNKS){
                const uint64_t st[NRZV_STAMPS]={zc::live_ep,zc::table[e->chunk].wr_ver,slot_serial,hit_pend_gen};fresh=nrz_vfresh(e,st)!=0;}}
        else if(nrbuf::st_mode>0){const uint64_t st[NRZV_STAMPS]={snapshot_epoch,nrbuf::st_ep,0,hit_pend_gen};fresh=nrz_vfresh(e,st)!=0;}
        if(!fresh)++s.c_stale;}
    if(fresh && cmode==1){++s.c_hit;   // the answer of the table; the counters the answer would have moved
        if(e->kind==NRZV_INPLACE)++zc::hits;else{++nrbuf::s.sbinds;++nrbuf::s.served;}
        Buffer b;b.buffer=VkBuffer(e->buf);b.memory=VkDeviceMemory(e->mem);b.offset=e->off;b.size=size_t(e->bytes);b.mapped=reinterpret_cast<void*>(uintptr_t(e->mapped));b.coherent=e->coherent!=0;return b;}
    snap_how=0;const Buffer b=view_real(address,bytes,site,stride);const int how=snap_how;
    if(fresh){++s.c_checked;   // DM_NR_Z3C=2: the table stood - snapshot() must have answered the same way with the same view
        if(how!=int(e->kind) || uint64_t(b.buffer)!=e->buf || uint64_t(b.memory)!=e->mem || b.offset!=e->off || b.size!=size_t(e->bytes) || uint64_t(uintptr_t(b.mapped))!=e->mapped || uint32_t(b.coherent)!=e->coherent){++s.c_bad;
            red("Z3c: the table holds a %s view of %#llx+%zu (buffer %#llx + %llu), snapshot() answered %s (buffer %#llx + %llu)",e->kind==NRZV_INPLACE?"zero-copy":"static-copy",(unsigned long long)address,bytes,
                (unsigned long long)e->buf,(unsigned long long)e->off,how==1?"a static copy":how==2?"a zero-copy view":"another way",(unsigned long long)uint64_t(b.buffer),(unsigned long long)b.offset);}}
    // remember the answer under the words of now (after the call: it noted the serve / made the copy of this epoch)
    nrz_view n{};n.addr=address;n.bytes=bytes;n.tag=uint32_t(site);n.buf=uint64_t(b.buffer);n.mem=uint64_t(b.memory);n.off=b.offset;n.mapped=uint64_t(uintptr_t(b.mapped));n.coherent=uint32_t(b.coherent);n.chunk=-1;
    if(how==1 && nrbuf::st_mode>0){n.kind=NRZV_STATIC;n.st[0]=snapshot_epoch;n.st[1]=nrbuf::st_ep;n.st[2]=0;n.st[3]=hit_pend_gen;*e=n;++s.c_static;}
    else if(how==2 && nrbuf::st_mode<=0 && zc::last_chunk>=0){n.kind=NRZV_INPLACE;n.chunk=zc::last_chunk;n.st[0]=zc::live_ep;n.st[1]=zc::table[zc::last_chunk].wr_ver;n.st[2]=slot_serial;n.st[3]=hit_pend_gen;*e=n;++s.c_inplace;}
    else{++s.c_other;if(nrz_vmatch(e,address,bytes,uint32_t(site)))e->kind=NRZV_NONE;}   // answered another way now: the old entry of the range goes
    return b;}
void tick(){   // every submission (nrdraw::tick) while a switch is on
    const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    if(!(s.b_asked|s.c_asked|s.d_asked))return;
    auto pc=[](uint64_t a,uint64_t b){return b?100.*double(a)/double(b):0.;};
    xbg_logf("[nr] z3 epochs: DM_NR_Z3B=%d: %llu image walks asked, the word stood %llu (%.1f%%; %s), set %llu, checked %llu, bad %llu; "
        "DM_NR_Z3C=%d: %llu vertex / index views asked, the table stood %llu (%.1f%%; %s %llu), stale %llu, stored in place %llu / static copy %llu, answered another way (not kept) %llu, checked %llu, bad %llu; "
        "DM_NR_Z3D=%d: %llu ring blocks asked, the ring word answered %llu (%.1f%%), made again %llu, checked %llu, bad %llu",
        bmode,(unsigned long long)s.b_asked,(unsigned long long)s.b_quiet,pc(s.b_quiet,s.b_asked),bmode==1?"not walked":"walked and compared",(unsigned long long)s.b_set,(unsigned long long)s.b_checked,(unsigned long long)s.b_bad,
        cmode,(unsigned long long)s.c_asked,(unsigned long long)(s.c_hit+s.c_checked),pc(s.c_hit+s.c_checked,s.c_asked),cmode==1?"answered from it":"snapshot() asked and compared",(unsigned long long)s.c_hit,(unsigned long long)s.c_stale,
        (unsigned long long)s.c_inplace,(unsigned long long)s.c_static,(unsigned long long)s.c_other,(unsigned long long)s.c_checked,(unsigned long long)s.c_bad,
        dmode,(unsigned long long)s.d_asked,(unsigned long long)s.d_fast,pc(s.d_fast,s.d_asked),(unsigned long long)s.d_reval,(unsigned long long)s.d_checked,(unsigned long long)s.d_bad);
    s=Stats{};}
}
