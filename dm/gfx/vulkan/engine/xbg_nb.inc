// [DOS2] NB-05: the native backend's records in the backend (docs/dos2/NATIVE_BACKEND_PLAN.md §2, §6). Included by xbg_game.cpp before
// execute(). The front end (dm/host/hle_dos2_nb.c + the NVN tap dm/gfx/nvn/nvn_nb_tap.inc) writes OP_X_NB_FRAME / _PASS / _PASS_END into
// the command stream and back-patches each PASS header (IR attachments, draws, reads, end reason, plan load / store) before the submit is
// queued. NB-05 executes nothing differently: the NVN records around the headers are translated as before.
// Check (DM_NB=2): every IR pass is a span of the stream (PASS .. PASS_END / next NB record / end of the handle); the host's facts inside the
// span are compared with the header: host draws, the host passes begun (pass_prof_begin), the attachment set and load / store of the first
// one (pgraph's bi_clear / bi_store / bi_kind, now also set while armed), host reads (prepare_read) against the IR reads, and whether the
// host pass open at an IR end by an operation is still open after that operation ("kept"). Lines "[nb] check: pass ... differs: ..."
// (first 16) and "[nb] check: ..." every 10 s. DM_NB=1 (native): the headers are read and counted the same way (NB-06 applies them).
#include "../../nb/nb_ir.h"   // NB_END_* / NB_LOAD_* (pure C)
#include "../../nb/nb_slotmask.h"   // NB-07 (nbsm, end of file)
namespace nbrp{void tick();}
namespace nbdraw{void tick();}   // NB-10, below
namespace nbkey{void frame(uint32_t seq);}   // [DOS2] NB-14 DM_NB_RELOC (xbg_nb_key.inc, end of file): the part's base state at every FRAME
// [DOS2] NB-17b DM_NB_ORDER=3 (xbg_nb_order.inc): while a part runs by its order tail, the passes that continue the pass of header h (its unit)
namespace nbord{extern bool in_run;bool unit_member(const p_nb_pass &h,int k,p_nb_pass &out);bool unit_last(const p_nb_pass &h,p_nb_pass &out);void note_unit_prebar();}
namespace nbchk{
bool armed=false;   // a DM_NB record was seen: hooks below track the host passes
struct Span{
    bool open=false,closing=false;p_nb_pass h{};uint64_t first_id=0,start_open=0,open_at_end=0;uint8_t end_op=0;
    uint32_t begun=0,kpasses=0,draws=0;bool cont=false,have=false;
    char kind='L';uint32_t clear=0,store=0;uint32_t natt=0;uint64_t att[9]{};bool color[9]{};   // first host pass of the span
    int split_line=0;unsigned split_why=0;uint64_t split_img=0;bool split_seen=false;                // why the first host pass ended inside the span
    uint32_t miss=0,miss_null=0;uint64_t miss_img=0;
    uint32_t uid=0;   // NB-05b: span number (lazy_open remembers the span that opened the host's pending pass)
    uint32_t ends_in=0,ends_pre=0;bool in_pass_write=false;   // NB-05c: host passes ended inside the span, by a read of a pre-pass write
};
Span sp;uint64_t host_open=0,host_seq=0;bool host_open_in_span=false;
// NB-05c: write_seq when the open host pass began, and whether it is a 'K' (clear) pass. A host pass ended inside the span by prepare_read
// why 2 ("written", no layout change, not an attachment) whose image was last written (attachment or storage) before that pass began is
// the P2 "split" class: the host barriers at the first read, and that read came in draw 2+ of a pass it had begun. The IR plans that
// barrier before the pass (NB_NP_PREBAR, nb_plan.c); with the flag set it is counted apart ("split pre"), not as a difference.
uint64_t host_wseq=0;bool host_k=false;
uint32_t cur_seq=0,last_seq=0;bool part_ok=false;uint64_t next_ns=0;unsigned told=0;
// NB-05b. late: end_open_pass is flushing the host's pending (DM_GFX_LAZY_CLEAR) pass only to end it at once; that pass belongs to the IR
// span that opened it (lazy_uid), not to the span it is flushed in (P2: the "split x2 (end line 6645 / 7035)" + "attachments differ").
// bar_*: an IR pass ended by a barrier (an attachment read after a kept barrier) is judged at the next host draw: the read that ends the
// host pass (prepare_read) is in that draw, after the PASS_END / PASS records.
uint32_t span_uid=0,lazy_uid=0;bool late=false;
struct BarPend{uint64_t id=0;uint32_t seq=0;int node=0;} bar_pend;
struct Stats{uint64_t frames,parts,dup,unpatched,stray,unplanned,passes,checked,bad,d_draws,d_att,d_nohost,d_hostpass,d_split,d_kpass,d_reads,
    miss_null,kept[16],closed[16],ld[3][4],st[2][2],cover_ok,cover_bad,clear_only_run,clear_only_carried,out_begins,out_draws,bytes,late_flush,
    d_split_pre,d_split_pre_nf,prebar;}s{};
const char *endname(unsigned e){static const char *n[]={"open","rt","clear","op","compute","barrier","submit"};return e<7?n[e]:"?";}
// end_op: the IR node that closed the pass: nb_ir.h kind | 0x40 for a COPY without images (CopyBufferToBuffer / ClearBuffer)
const char *opname(unsigned k){switch(k&0x3f){case 1:return (k&0x40)?"buffer-copy":"copy";case 2:return "blit";case 3:return "resolve";case 4:return "compute";case 5:return "barrier";
    case 6:return "readback";case 7:return "present";case 8:return "clear-texture";default:return "-";}}
uint64_t key(const Image &im){return (uint64_t)(uintptr_t)im.source.mem;}   // IR image id = texture storage address
bool has_read(const p_nb_pass &h,uint64_t im){
    for(uint32_t i=0;i<h.nread && i<NB_REC_READS;i++)if(h.read[i]==im)return true;
    if(!h.read_over)return false;const unsigned a=nb_rec_bloom_bit(im,0),b=nb_rec_bloom_bit(im,1);
    return (h.bloom[a>>6]>>(a&63)&1) && (h.bloom[b>>6]>>(b&63)&1);}
void lazy_open(){lazy_uid=sp.open && !sp.closing?sp.uid:0;}   // lazy_clear_open: the host keeps a pass pending (begun at its first draw / end)
void begin(const Pass &p){host_open=++host_seq;host_open_in_span=sp.open && !sp.closing;host_wseq=write_seq;host_k=pgraph::bi_kind=='K';
    if(late && &p==&lazy.pass && lazy_uid!=(host_open_in_span?sp.uid:0)){host_open_in_span=false;++s.late_flush;return;}   // an earlier span's pass
    if(!host_open_in_span){++s.out_begins;return;}
    if(pgraph::bi_kind=='K'){++sp.kpasses;return;}   // a clear the host draws as its own pass (masked / partial clear)
    if(sp.begun++)return;
    if(nb_rp_mode)nbrp::compare(p);   // NB-06: the first host pass of the span against the header (DM_NB_RP 1 / 2 / 3)
    sp.have=true;sp.kind=pgraph::bi_kind;sp.clear=pgraph::bi_clear;sp.store=pgraph::bi_store;sp.natt=uint32_t(std::min<size_t>(p.images.size(),9));
    for(uint32_t k=0;k<sp.natt;k++){sp.att[k]=key(*p.images[k]);sp.color[k]=(p.images[k]->aspects&VK_IMAGE_ASPECT_COLOR_BIT)!=0;}}
void end(){if(!host_open)return;
    if(host_open_in_span && sp.open && !sp.closing && host_open>=sp.first_id && !host_k){const Image *im=pgraph::end_img;++sp.ends_in;
        if(pgraph::end_why==2 && im && im->last_write<=host_wseq && im->storage_write<=host_wseq)++sp.ends_pre;   // written before this host pass
        else if((pgraph::end_why&2) && im)sp.in_pass_write=true;}                                                // written inside it
    if(host_open_in_span && sp.open && !sp.closing && !sp.split_seen && host_open>=sp.first_id){sp.split_seen=true;sp.split_line=pgraph::end_line;sp.split_why=pgraph::end_why;
        sp.split_img=pgraph::end_img?key(*pgraph::end_img):0;}
    host_open=0;host_open_in_span=false;}
void bar_eval(){if(!bar_pend.id)return;   // the host pass open when the IR pass ended at a barrier: still the open one?
    if(host_open==bar_pend.id){++s.kept[5];++s.bad;if(told<16){++told;xbg_logf("[nb] check: pass seq=%u node=%d end=barrier differs: host pass kept across the IR end (barrier)",bar_pend.seq,bar_pend.node);}}
    else ++s.closed[5];
    bar_pend=BarPend{};}
void draw(){bar_eval();if(!sp.open || sp.closing){++s.out_draws;return;}++sp.draws;if(host_open && host_open<sp.first_id)sp.cont=true;}
void read(Image &im){if(!sp.open || sp.closing)return;const uint64_t m=key(im);
    if(!m){++sp.miss_null;return;}
    for(uint32_t k=0;k<sp.h.natt && k<NB_REC_ATT;k++)if(sp.h.att[k]==m)return;   // an attachment read (feedback) is the IR's too
    if(!has_read(sp.h,m)){if(!sp.miss++)sp.miss_img=m;}}
void differs(const char *what){++s.bad;if(told>=16)return;++told;
    xbg_logf("[nb] check: pass seq=%u node=%d att=%u draws=%u end=%s%s%s reads=%u differs: %s",sp.h.seq,int(sp.h.node),unsigned(sp.h.natt),sp.h.draws,endname(sp.h.end),
        sp.closing?"/":"",sp.closing?opname(sp.end_op):"",sp.h.nread,what);}
void finish(){if(!sp.open)return;Span &x=sp;const p_nb_pass &h=x.h;++s.checked;char why[480];int k=0;why[0]=0;
    auto add=[&](const char *fmt,auto... a){if(k<int(sizeof why)-64)k+=snprintf(why+k,sizeof why-size_t(k),fmt,a...);};
    if(x.draws!=h.draws){++s.d_draws;add(" draws ir=%u host=%u;",h.draws,x.draws);}
    if(h.draws && !x.begun && !x.cont){++s.d_nohost;add(" no host pass;");}
    if(!h.draws && x.begun){if(h.nclear){++s.clear_only_run;}else{++s.d_hostpass;add(" host pass for an empty IR pass;");}}
    if(!h.draws && h.nclear && !x.begun)++s.clear_only_carried;
    if(h.flags&NB_NP_PREBAR)++s.prebar;
    const bool pre=x.ends_in && x.ends_pre==x.ends_in;   // NB-05c: every host end inside the span = a read of a pre-pass write
    if(x.begun>1 && pre && (h.flags&NB_NP_PREBAR))++s.d_split_pre;   // explained: the plan puts the barrier before the pass (NB-06 applies it)
    else if(x.begun>1 && pre){++s.d_split;++s.d_split_pre_nf;add(" host split x%u by a read of a pre-pass write the plan did not flag (img %#llx);",x.begun,(unsigned long long)x.split_img);}
    else if(x.begun>1){++s.d_split;add(" host split x%u (end line %d why %u img %#llx%s);",x.begun,x.split_line,x.split_why,(unsigned long long)x.split_img,
        x.in_pass_write?", written inside the host pass":"");}
    if(x.kpasses){++s.d_kpass;add(" %u host clear passes inside;",x.kpasses);}
    if(x.miss){++s.d_reads;add(" %u host reads not in the IR (first %#llx);",x.miss,(unsigned long long)x.miss_img);}
    s.miss_null+=x.miss_null;
    if(x.closing && h.end==NB_END_BARRIER && x.open_at_end){bar_eval();bar_pend.id=x.open_at_end;bar_pend.seq=h.seq;bar_pend.node=h.node;}
    else if(x.closing && (h.end==NB_END_OP || h.end==NB_END_CLEAR || h.end==NB_END_COMPUTE) && x.open_at_end){
        const unsigned o=(x.end_op&0x40)?9u:(x.end_op&0xfu);   // 9 = buffer copy / ClearBuffer
        if(host_open==x.open_at_end){++s.kept[o<16?o:15];add(" host pass kept across the IR end (%s);",opname(x.end_op));}else ++s.closed[o<16?o:15];}
    if(x.have && h.plan_ok){   // attachment set + load / store of the first host pass, matched by image
        bool same=x.natt==h.natt;for(uint32_t a=0;a<h.natt && same;a++){bool f=false;for(uint32_t b=0;b<x.natt;b++)if(x.att[b]==h.att[a])f=true;same=f;}
        if(!same){++s.d_att;add(" attachments ir=%u host=%u differ;",unsigned(h.natt),x.natt);}
        for(uint32_t a=0;a<h.natt && a<NB_REC_ATT;a++){int j=-1;for(uint32_t b=0;b<x.natt;b++)if(x.att[b]==h.att[a])j=int(b);if(j<0)continue;
            const unsigned hl=((x.clear>>(2*j))&3u)?1u:(x.kind=='N' && x.color[j])?2u:x.kind=='F'?3u:0u,il=h.load[a]<3?h.load[a]:0u;++s.ld[il][hl];
            if(il==0 && hl==2){if(h.cover_dc>>a&1)++s.cover_ok;else ++s.cover_bad;}
            ++s.st[h.store[a]?1:0][(x.store>>j)&1u];}}
    if(k)differs(why);
    sp=Span{};}
void frame(const p_nb_frame &f){nbkey::frame(f.seq);finish();armed=true;++s.parts;if(f.final)++s.frames;s.bytes+=f.bytes;
    if(cur_seq && f.seq<=last_seq){++s.dup;part_ok=false;return;}   // the recording is translated again: its headers were checked already
    if(!f.patched){++s.unpatched;part_ok=false;return;}
    if(!f.plan_ok)++s.unplanned;
    part_ok=true;cur_seq=last_seq=f.seq;}
void pass(const p_nb_pass &r){finish();if(!part_ok || r.seq!=cur_seq){++s.stray;return;}
    if(!r.patched){++s.unpatched;return;}
    ++s.passes;sp=Span{};sp.open=true;sp.h=r;sp.first_id=host_seq+1;sp.start_open=host_open;sp.uid=++span_uid;}
void pass_end(const p_nb_pass_end &e){if(!sp.open || sp.closing || e.seq!=sp.h.seq || e.node!=sp.h.node){return;}
    sp.closing=true;sp.open_at_end=host_open;sp.end_op=e.op;}
void handle_end(){finish();bar_eval();}   // OP_END of a top-level handle: the IR's submission ends (NB_END_SUBMIT)
void tick(){if(!armed)return;nbdraw::tick();const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const double fr=s.frames?double(s.frames):1.;
    xbg_logf("[nb] check: %llu frames, %llu parts (dup %llu, unpatched %llu, unplanned %llu, stray headers %llu), %.1f NB KB a frame; %llu passes, %llu differ: "
        "draws %llu, attachments %llu, no host pass %llu, host pass for an empty IR pass %llu, split %llu (pre-pass write unflagged %llu), host clear passes %llu, reads not in IR %llu (null %llu); "
        "split by a read of a pre-pass write, barrier planned before the pass (NB_NP_PREBAR, not a difference) %llu, IR passes with a pre-pass barrier %llu; "
        "IR end by op -> host kept/closed: buffer-copy %llu/%llu copy %llu/%llu resolve %llu/%llu compute %llu/%llu barrier %llu/%llu readback %llu/%llu clear-texture %llu/%llu; "
        "clear-only IR passes run by the host %llu, carried %llu; host pending passes flushed late (counted in the span that opened them) %llu; "
        "host passes / draws outside IR passes %llu / %llu",
        (unsigned long long)s.frames,(unsigned long long)s.parts,(unsigned long long)s.dup,(unsigned long long)s.unpatched,(unsigned long long)s.unplanned,(unsigned long long)s.stray,
        s.bytes/fr/1024.,(unsigned long long)s.passes,(unsigned long long)s.bad,(unsigned long long)s.d_draws,(unsigned long long)s.d_att,(unsigned long long)s.d_nohost,
        (unsigned long long)s.d_hostpass,(unsigned long long)s.d_split,(unsigned long long)s.d_split_pre_nf,(unsigned long long)s.d_kpass,(unsigned long long)s.d_reads,(unsigned long long)s.miss_null,
        (unsigned long long)s.d_split_pre,(unsigned long long)s.prebar,
        (unsigned long long)s.kept[9],(unsigned long long)s.closed[9],(unsigned long long)s.kept[1],(unsigned long long)s.closed[1],(unsigned long long)s.kept[3],(unsigned long long)s.closed[3],
        (unsigned long long)s.kept[4],(unsigned long long)s.closed[4],(unsigned long long)s.kept[5],(unsigned long long)s.closed[5],(unsigned long long)s.kept[6],(unsigned long long)s.closed[6],
        (unsigned long long)s.kept[8],(unsigned long long)s.closed[8],(unsigned long long)s.clear_only_run,(unsigned long long)s.clear_only_carried,(unsigned long long)s.late_flush,
        (unsigned long long)s.out_begins,(unsigned long long)s.out_draws);
    // load: IR L / C / D against host L(oad) / C(lear twin) / D(noload, first_draw_covers) / F(begin + vkCmdClearAttachments); store IR S / D x host S / D
    xbg_logf("[nb] check: load ir\\host L/C/D/F: L %llu/%llu/%llu/%llu C %llu/%llu/%llu/%llu D %llu/%llu/%llu/%llu; IR L + host noload: cover_dc %llu, outside cover_dc %llu; "
        "store ir\\host S/D: S %llu/%llu D %llu/%llu",
        (unsigned long long)s.ld[0][0],(unsigned long long)s.ld[0][1],(unsigned long long)s.ld[0][2],(unsigned long long)s.ld[0][3],
        (unsigned long long)s.ld[1][0],(unsigned long long)s.ld[1][1],(unsigned long long)s.ld[1][2],(unsigned long long)s.ld[1][3],
        (unsigned long long)s.ld[2][0],(unsigned long long)s.ld[2][1],(unsigned long long)s.ld[2][2],(unsigned long long)s.ld[2][3],
        (unsigned long long)s.cover_ok,(unsigned long long)s.cover_bad,(unsigned long long)s.st[0][0],(unsigned long long)s.st[0][1],(unsigned long long)s.st[1][0],(unsigned long long)s.st[1][1]);
    if(nb_rp_mode)nbrp::tick();
    s=Stats{};}
}
// [DOS2] NB-06, DM_NB_RP (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-06 findings"). The planner's per-pass decision, carried in the PASS header,
// applied to the host's pass (xbg_game.cpp nbrp_begin / nbrp_lazy_begin / pclear_take / pclear_carry). Mapping header -> host attachments:
// dm/gfx/nb/nb_rp.h (host test tests/nb_rp_test.cpp).
//   1 / 3: open() at the first host pass of a span (draw or clear that opens the host pass; nbchk::sp.begun == 0, no continued host pass):
//          NB_NP_PREBAR -> pending clears of the header's read images (exact list) land, those images go to GENERAL, full_barrier - all
//          BEFORE the pass is begun or recorded as pending (lazy), so prepare_read in draw 2+ finds nothing written and the host pass is not
//          split; then the decision (nb_rp_map). Fallback to the heuristics, counted: no / unplanned header, a later host pass of the span
//          (split), attachments differ, DM_GFX_GUEST_BARRIER=1 (the host then ends passes on barriers the IR keeps).
//   3:     + every attachment begun with load DONT_CARE is cleared to magenta (depth 0.5 / stencil 0x55) right after vkCmdBeginRenderPass and
//          every store DONT_CARE gets the STORE_HIST poison before vkCmdEndRenderPass: a wrong plan shows on screen on a sysmem driver.
//   2:     nothing applied; compare() tallies plan vs heuristics at the first host pass of every span; first 16 differences in detail.
// Every counter is printed with the 10 s "[nb] check" lines as "[nb] rp ..." (any DM_NB_RP != 0).
namespace nbrp{
nb_rp_dec cur{},lazy{};bool cur_ok=false,lazy_ok=false;uint32_t bi_dc=0,prebar_uid=0;unsigned told=0;At cur_at{},lazy_at{};uint64_t store_kept=0;
struct Stats{uint64_t opens,decided,nohdr,later,att,guestbar,clear_missing,clear_extra,twin_fail,dc_load,store_dc,poison_load,carried,landed,
    prebar,prebar_bar,prebar_nobar,prebar_trans,prebar_land,cmp,cmp_fallback,cmp_differ,ld[3][4],st[2][2];}s{};
int map(const Pass &p,const p_nb_pass &h,nb_rp_dec &d){
    if(p.images.size()>NB_REC_ATT){d=nb_rp_dec{};return NB_RP_ATT;}
    uint64_t a[NB_REC_ATT]{};const unsigned n=unsigned(p.images.size());for(unsigned k=0;k<n;k++)a[k]=nbchk::key(*p.images[k]);
    return nb_rp_map(h.plan_ok,h.natt,h.att,h.load,h.store,h.cover_dc,h.flags,n,a,&d);}
const p_nb_pass *span_hdr(){const nbchk::Span &x=nbchk::sp;return x.open && !x.closing && x.h.patched?&x.h:nullptr;}
void prebar(const Pass &p,const p_nb_pass &h0,VkCommandBuffer cmd){if(prebar_uid==nbchk::sp.uid)return;prebar_uid=nbchk::sp.uid;++s.prebar;
    auto reads=[&](const p_nb_pass &h){
    for(uint32_t i=0;i<h.nread && i<NB_REC_READS;i++){const uint64_t m=h.read[i];if(!m)continue;bool att=false;for(auto *im:p.images)if(nbchk::key(*im)==m)att=true;if(att)continue;
        Image *hit[4]{};int nh=0;for_images_overlapping(m,1,[&](Image &im){if(nbchk::key(im)==m && nh<4)hit[nh++]=&im;});
        for(int j=0;j<nh;j++){Image &im=*hit[j];if(pclear_n){const size_t before=pclear_n;pclear_flush_image(im,0);if(pclear_n!=before)++s.prebar_land;}
            if(im.layout!=VK_IMAGE_LAYOUT_GENERAL){im.transition(cmd,VK_IMAGE_LAYOUT_GENERAL);++s.prebar_trans;}}}};
    reads(h0);
    if(nbord::in_run){p_nb_pass m;for(int k=1;nbord::unit_member(h0,k,m);k++){reads(m);if(k==1)nbord::note_unit_prebar();}}   // [DOS2] NB-17b: the unit's barrier covers every member's reads
    if(write_seq>barrier_seq || storage_unbarriered){full_barrier(cmd);for(auto *im:p.images)im->last_write=++write_seq;++s.prebar_bar;}   // the pass's own writes stay after it
    else ++s.prebar_nobar;}
bool open(const Pass &p,VkCommandBuffer cmd){cur_ok=false;++s.opens;const p_nb_pass *h=span_hdr();
    if(!h || !h->plan_ok){++s.nohdr;return false;}
    if(nbchk::sp.begun || nbchk::sp.cont){++s.later;return false;}   // a second host pass of the span (split) or a continued one: heuristics
    if(guest_barrier>0){++s.guestbar;return false;}
    if(h->flags&NB_NP_PREBAR)prebar(p,*h,cmd);
    if(map(p,*h,cur)!=NB_RP_OK){++s.att;return false;}
    // [DOS2] NB-17b: a unit (passes run as one Vulkan pass) stores what its last pass stores: DONT_CARE only where both headers say so
    if(nbord::in_run){p_nb_pass t;if(nbord::unit_last(*h,t)){nb_rp_dec d2{};if(map(p,t,d2)==NB_RP_OK)cur.store_dc&=d2.store_dc;else cur.store_dc=0;}}
    ++s.decided;cur_ok=true;cur_at.seq=h->seq;cur_at.node=h->node;return true;}
void twin(const nb_rp_twin_out &t,bool failed,uint32_t store){s.clear_missing+=t.clear_missing;s.clear_extra+=t.clear_extra;if(failed)++s.twin_fail;
    s.dc_load+=unsigned(__builtin_popcount(t.dc));s.store_dc+=unsigned(__builtin_popcount(store));}
void carried(){++s.carried;}
void landed(){++s.landed;}
void poison_load(const Pass &p,VkCommandBuffer cmd,uint32_t dc){if(!p.width || !p.height)return;VkClearRect r{{{0,0},{p.width,p.height}},0,1};
    for(uint32_t k=0;k<p.images.size() && k<9;k++)if(dc>>k&1u){const Image &im=*p.images[k];VkClearAttachment a{};
        if(p.att_slot[k]==8){a.aspectMask=im.aspects&(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT);a.clearValue.depthStencil={0.5f,0x55};}
        else{a.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;a.colorAttachment=p.att_slot[k];a.clearValue.color.float32[0]=1;a.clearValue.color.float32[2]=1;a.clearValue.color.float32[3]=1;}
        if(a.aspectMask){cmd_clear_attachments(cmd,1,&a,&r);++s.poison_load;}}}
// The host pass just begun (first of its span) against the header: load L / C / D / F (F = load + vkCmdClearAttachments) and store S / D
// by host attachment. Plan 'c' = LOAD that may become DONT_CARE by cover_dc (counted as L).
void compare(const Pass &p){const uint32_t dc=bi_dc;bi_dc=0;const p_nb_pass *h=span_hdr();if(!h)return;nb_rp_dec d{};++s.cmp;
    if(map(p,*h,d)!=NB_RP_OK){++s.cmp_fallback;return;}
    char pl[10]{},ho[10]{},ps[10]{},hs[10]{};bool diff=false;
    for(unsigned k=0;k<d.n;k++){const unsigned b=1u<<k;const bool color=(p.images[k]->aspects&VK_IMAGE_ASPECT_COLOR_BIT)!=0;
        const unsigned il=(d.clear&b)?1u:(d.dc&b)?2u:0u;
        const unsigned hl=((pgraph::bi_clear>>(2*k))&3u)?1u:((dc&b) || (pgraph::bi_kind=='N' && color))?2u:pgraph::bi_kind=='F'?3u:0u;
        const unsigned is=(d.store_dc&b)?1u:0u,hst=(pgraph::bi_store>>k)&1u;++s.ld[il][hl];++s.st[is][hst];
        pl[k]=il==1?'C':il==2?'D':(d.cover&b)?'c':'L';ho[k]="LCDF"[hl];ps[k]=is?'D':'S';hs[k]=hst?'D':'S';
        if(il!=(hl==3?0u:hl) || is!=hst)diff=true;}
    if(!diff)return;++s.cmp_differ;if(told>=16)return;++told;
    xbg_logf("[nb] rp check: pass seq=%u node=%d att=%u draws=%u flags=%#x: load plan %s host %s, store plan %s host %s (DM_NB_RP=%d)",
        h->seq,int(h->node),unsigned(d.n),h->draws,unsigned(h->flags),pl,ho,ps,hs,nb_rp_mode);}
void tick(){const nbchk::Stats &c=nbchk::s;
    xbg_logf("[nb] rp DM_NB_RP=%d: host pass opens in IR passes %llu (plan applied %llu; heuristics: no header %llu, later host pass of the span %llu, attachments differ %llu, guest barriers %llu); "
        "compared %llu (attachments differ %llu), plan != host %llu; load plan\\host L/C/D/F: L %llu/%llu/%llu/%llu C %llu/%llu/%llu/%llu D %llu/%llu/%llu/%llu; store plan\\host S/D: S %llu/%llu D %llu/%llu; "
        "plan CLEAR not pending on the host (LOAD) %llu, host clear folded beyond the plan %llu, pending clears landed for a plan LOAD %llu, twin missing %llu; "
        "applied: load DONT_CARE %llu (poisoned %llu), store DONT_CARE %llu (kept STORE for store-hist banned images %llu); clear-only IR passes: carried by the plan %llu, run by the host %llu, not run (all) %llu; "
        "prebar: spans %llu, barriers %llu (nothing to wait for %llu, layout transitions %llu, pending clears landed %llu); host splits by a pre-pass write %llu, other splits %llu",
        nb_rp_mode,(unsigned long long)s.opens,(unsigned long long)s.decided,(unsigned long long)s.nohdr,(unsigned long long)s.later,(unsigned long long)s.att,(unsigned long long)s.guestbar,
        (unsigned long long)s.cmp,(unsigned long long)s.cmp_fallback,(unsigned long long)s.cmp_differ,
        (unsigned long long)s.ld[0][0],(unsigned long long)s.ld[0][1],(unsigned long long)s.ld[0][2],(unsigned long long)s.ld[0][3],
        (unsigned long long)s.ld[1][0],(unsigned long long)s.ld[1][1],(unsigned long long)s.ld[1][2],(unsigned long long)s.ld[1][3],
        (unsigned long long)s.ld[2][0],(unsigned long long)s.ld[2][1],(unsigned long long)s.ld[2][2],(unsigned long long)s.ld[2][3],
        (unsigned long long)s.st[0][0],(unsigned long long)s.st[0][1],(unsigned long long)s.st[1][0],(unsigned long long)s.st[1][1],
        (unsigned long long)s.clear_missing,(unsigned long long)s.clear_extra,(unsigned long long)s.landed,(unsigned long long)s.twin_fail,
        (unsigned long long)s.dc_load,(unsigned long long)s.poison_load,(unsigned long long)s.store_dc,(unsigned long long)store_kept,(unsigned long long)s.carried,(unsigned long long)c.clear_only_run,(unsigned long long)c.clear_only_carried,
        (unsigned long long)s.prebar,(unsigned long long)s.prebar_bar,(unsigned long long)s.prebar_nobar,(unsigned long long)s.prebar_trans,(unsigned long long)s.prebar_land,
        (unsigned long long)c.d_split_pre,(unsigned long long)(c.d_split-c.d_split_pre_nf));
    s=Stats{};store_kept=0;}
}

// ---- NB-09 / NB-10: OP_X_NB_DRAW (DM_NB_OPS / DM_NB_OPS_CHECK bits 36 / 37; layout and helpers dm/gfx/nvn/nvn_nb_draw.h) ----
// NB-10 apply (a record without NBD_F_CHECK): nbd_view + nbd_apply write the record's binds straight into state.tex / samp / img / ub (the
// last write per slot wins = the old bind records in call order; no record built per bind, no copy of the payload, one limit test per
// bind instead of a need), then one tex_gen / buf_gen bump per stage the record touched (the old records bumped per bind; every reader
// only asks "changed since" and none runs between the binds of one record), then the same draw() as OP_DRAW_IDX / OP_DRAW with cur_op =
// that opcode (pass-end / barrier attribution unchanged). SetRenderEnable(Conditional) flags are kept in `ren` and counted; with
// DM_GFX_COND_RENDER != 0 they are applied before the draw as an OP_X_RENDER_COND would be (condr::from_nb; the tap then keeps only the
// bracket's last call), else not executed (the NVN path ignores those calls too). DM_NB_DRAW_UNPACK=1: the NB-09 faithful unpack instead (A/B of the CPU cost, same picture).
// NB-10 check (NBD_F_CHECK, DM_NB_OPS_CHECK): the old records of the same draw were executed already (the binds) or come next (the draw
// record): the tables are compared with the record now (nbd_compare), its draw arguments at the next OP_DRAW_IDX / OP_DRAW (seen(), from
// those cases of execute); nothing is drawn for the check record. Lines "[nb] draw check: ..." every 10 s + the first 16 differences.
namespace nbdraw{
static_assert(sizeof(NVNbufferRange)==sizeof(nbd_range) && offsetof(NVNbufferRange,size)==offsetof(nbd_range,size),"nbd_range = NVNbufferRange");
enum{BY_LIMIT=NBD_B_KINDS,BY_DRAW,BY_NODRAW,BY_N};
struct Stats{uint64_t records=0,binds=0,unpacked=0,tex_bumps=0,buf_bumps=0,timed=0,bind_ns=0,ren_off=0,cond=0;
             uint64_t checks=0,bad=0,by[BY_N]{},cbinds=0;} s;
uint64_t records=0,binds=0;   // since start
uint8_t ren=0;                // flags NBD_F_REN / _REN_ON / _COND of the last applied record that had them
int unpack_mode=-1;
bool pend=false,pend_bad=false;nbd_hdr pend_h{};unsigned told=0;uint64_t next_ns=0;
inline nbd_range (*ub_tab())[16]{return reinterpret_cast<nbd_range(*)[16]>(state.ub);}
void note_ren(const nbd_hdr &h){if(h.flags&NBD_F_REN && !(h.flags&NBD_F_REN_ON))++s.ren_off;if(h.flags&NBD_F_COND)++s.cond;
    if(h.flags&(NBD_F_REN|NBD_F_COND))ren=uint8_t(h.flags&(NBD_F_REN|NBD_F_REN_ON|NBD_F_COND));}
void do_draw(const nbd_hdr &h){const unsigned saved=cur_op;
    if(h.draw==NBD_DRAW_IDX){cur_op=OP_DRAW_IDX;p_draw_idx d{h.prim,h.itype,h.count,h.base_vertex,h.base_instance,h.instances,h.ibuf};draw(d.prim,0,d.count,&d);}
    else if(h.draw==NBD_DRAW_ARRAYS){cur_op=OP_DRAW;draw(h.prim,h.first,h.count,nullptr);}
    cur_op=saved;}
void apply(const uint8_t *p,size_t n){
    nbd_hdr h;const uint8_t *b=nullptr;
    need(nbd_view(p,n,&h,&b)==0,"Invalid OP_X_NB_DRAW record");
    const bool timed=!(s.records&63);const uint64_t t0=timed?timing_clock():0;
    unsigned ts=0,bs=0;
    if(nbdesc::mode<0){const char *e=getenv("DM_NB_DESC");nbdesc::mode=e && *e?std::clamp(atoi(e),0,2):0;
        xbg_logf("[nb] DM_NB_DESC=%d (NB-11: an OP_X_NB_DRAW that leaves the descriptor memo key as the previous draw keyed it %s)",nbdesc::mode,
                 nbdesc::mode==1?"reuses that draw's set without building the key":nbdesc::mode==2?"is verified against the key built as usual":"- off");}
    int nbw=NBDS_PREV;   // [DOS2] NB-11: nb_desc_pre before the binds land (the record against the previous descriptors() call)
    if(nbdesc::mode>0){++nbdesc::s.records;const bool same_tg=nbdesc::have_texgen && h.texgen==nbdesc::last_texgen;nbdesc::last_texgen=h.texgen;nbdesc::have_texgen=true;
        int texb=0;if(nbdesc::P.valid)nbw=nb_desc_pre(&nbdesc::P.s,tex_gen,buf_gen,&h,b,(const uint64_t(*)[128])state.tex,(const uint64_t(*)[128])state.samp,
                                                      (const uint64_t(*)[16])state.img,(const nbd_range(*)[16])ub_tab(),&texb);
        if(same_tg){++nbdesc::s.texgen_same;if(texb)++nbdesc::s.texgen_tex;}}
    need(nbd_apply(&h,b,state.tex,state.samp,state.img,ub_tab(),&ts,&bs)==0,"Invalid OP_X_NB_DRAW bind");
    for(unsigned m=ts;m;m&=m-1){++tex_gen[__builtin_ctz(m)];++s.tex_bumps;}
    for(unsigned m=bs;m;m&=m-1){++buf_gen[__builtin_ctz(m)];++s.buf_bumps;}
    if(nbubo::mode!=0)nbubo::note(h,b);   // [DOS2] NB-12 DM_NB_UBO: the record's uniform ranges are the engine's constant ring (first call reads the switch)
    if(nbdesc::mode>0){if(nbw==NBDS_OK)nbw=nb_desc_post(&nbdesc::P.s,ts|bs);
        if(nbw!=NBDS_OK)++nbdesc::s.why[nbw];   // reasons found in descriptors() are counted there
        nbdesc::cand=nbw==NBDS_OK;memcpy(nbdesc::etex,tex_gen,sizeof nbdesc::etex);memcpy(nbdesc::ebuf,buf_gen,sizeof nbdesc::ebuf);}
    if(timed){s.bind_ns+=timing_clock()-t0;++s.timed;}
    ++s.records;s.binds+=h.nbind;++records;binds+=h.nbind;note_ren(h);
    condr::from_nb(h.flags,h.cond_mode,h.cond_addr);   // [DOS2] DM_GFX_COND_RENDER (no-op at 0)
    do_draw(h);
    nbdesc::cand=false;}   // [DOS2] NB-11: a draw() that returned before descriptors() leaves no candidate
// NB-09: the faithful unpack - the binds become, one by one in call order, exactly the NVN bind records they replaced (exec_bind_buf /
// exec_bind_handles of xbg_game.cpp, same checks, tables and per-bind generation bumps). Kept behind DM_NB_DRAW_UNPACK=1.
void unpack(const uint8_t *p,size_t n){
    nbd_shadow sh;
    const bool timed=!(s.unpacked&63);const uint64_t t0=timed?timing_clock():0;
    need(nbd_unpack(p,n,&sh)==0,"Invalid OP_X_NB_DRAW record");
    const unsigned saved=cur_op;
    for(unsigned i=0;i<sh.h.nbind;i++){const nbd_bind &e=sh.bind[i];
        if(e.kind==NBD_B_UBO){cur_op=OP_BIND_UBO;p_bind_buf a{int32_t(e.stage),int32_t(e.slot),e.a,e.b};exec_bind_buf(OP_BIND_UBO,a);}
        else{const uint32_t op=e.kind==NBD_B_TEX?OP_BIND_TEX:e.kind==NBD_B_SAMP?OP_BIND_SAMP:OP_BIND_IMG;cur_op=op;
            p_bind_handles a{};a.stage=e.stage;a.first=e.slot;a.n=1;a.h[0]=e.a;exec_bind_handles(op,a,offsetof(p_bind_handles,h)+8);}}
    cur_op=saved;
    if(timed){s.bind_ns+=timing_clock()-t0;++s.timed;}
    ++s.unpacked;s.binds+=sh.h.nbind;++records;binds+=sh.h.nbind;note_ren(sh.h);
    condr::from_nb(sh.h.flags,sh.h.cond_mode,sh.h.cond_addr);   // [DOS2] DM_GFX_COND_RENDER (no-op at 0)
    do_draw(sh.h);}
void differs(const nbd_hdr &h,const char *what){if(told>=16)return;++told;
    xbg_logf("[nb] draw check: engine op %u, draw kind %u (prim %d count %d), %u binds, texgen %u differs: %s",unsigned(h.engine_op),unsigned(h.draw),
             h.prim,h.count,unsigned(h.nbind),h.texgen,what);}
void check(const uint8_t *p,size_t n){
    nbd_hdr h;const uint8_t *b=nullptr;
    need(nbd_view(p,n,&h,&b)==0,"Invalid OP_X_NB_DRAW record");
    if(pend){++s.by[BY_NODRAW];if(!pend_bad)++s.bad;differs(pend_h,"no OP_DRAW_IDX / OP_DRAW record after the check record");}
    ++s.checks;s.cbinds+=h.nbind;note_ren(h);
    nbd_diff d{};
    const unsigned bad=nbd_compare(&h,b,(const uint64_t(*)[128])state.tex,(const uint64_t(*)[128])state.samp,(const uint64_t(*)[16])state.img,
                                   (const nbd_range(*)[16])ub_tab(),&d);
    for(unsigned k=0;k<=BY_LIMIT;k++)if(bad>>k&1)++s.by[k];
    if(bad){++s.bad;char w[200];static const char *const kn[]={"texture","sampler","image","uniform buffer"};
        if(bad&~(1u<<BY_LIMIT))snprintf(w,sizeof w,"%s stage %u slot %u: record %#llx+%llu, state %#llx+%llu (kinds %#x)",kn[d.kind<4?d.kind:0],d.stage,d.slot,
            (unsigned long long)d.want_a,(unsigned long long)d.want_b,(unsigned long long)d.got_a,(unsigned long long)d.got_b,bad);
        else snprintf(w,sizeof w,"a bind outside the limits");
        differs(h,w);}
    pend=true;pend_bad=bad!=0;pend_h=h;}
// From case OP_DRAW_IDX (d) / OP_DRAW (prim, first, count) while a check record waits.
void seen(int prim,int first,int count,const p_draw_idx *d){pend=false;
    const int ok=d?nbd_same_draw(&pend_h,NBD_DRAW_IDX,d->prim,d->itype,d->count,d->base_vertex,d->base_instance,d->instances,d->ibuf,0)
                  :nbd_same_draw(&pend_h,NBD_DRAW_ARRAYS,prim,0,count,0,0,0,0,first);
    if(ok)return;
    ++s.by[BY_DRAW];if(!pend_bad)++s.bad;
    char w[256];
    if(d)snprintf(w,sizeof w,"draw arguments: record kind %u prim %d itype %d count %d bv %d bi %d inst %d ibuf %#llx; old DRAW_IDX prim %d itype %d count %d bv %d bi %d inst %d ibuf %#llx",
        unsigned(pend_h.draw),pend_h.prim,pend_h.itype,pend_h.count,pend_h.base_vertex,pend_h.base_instance,pend_h.instances,(unsigned long long)pend_h.ibuf,
        d->prim,d->itype,d->count,d->base_vertex,d->base_instance,d->instances,(unsigned long long)d->ibuf);
    else snprintf(w,sizeof w,"draw arguments: record kind %u prim %d first %d count %d; old DRAW prim %d first %d count %d",unsigned(pend_h.draw),pend_h.prim,
        pend_h.first,pend_h.count,prim,first,count);
    differs(pend_h,w);}
void exec(const uint8_t *p,size_t n){
    need(p && n>=sizeof(nbd_hdr),"Invalid OP_X_NB_DRAW record");
    if(p[offsetof(nbd_hdr,flags)]&NBD_F_CHECK){check(p,n);return;}
    if(unpack_mode<0){const char *e=getenv("DM_NB_DRAW_UNPACK");unpack_mode=e && atoi(e)==1;
        xbg_logf("[nb] OP_X_NB_DRAW: %s (DM_NB_DRAW_UNPACK=%d, NB-10)",unpack_mode?"NB-09 faithful unpack into the old bind records":"applied to the state directly",unpack_mode);}
    if(unpack_mode)unpack(p,n);else apply(p,n);}
// Every submission (from nbchk::tick); one pair of lines every 10 s while records come.
void tick(){nbdesc::tick();   // [DOS2] NB-11 "[nb] desc" line (own 10 s clock)
    if(!s.records && !s.unpacked && !s.checks)return;const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const uint64_t rec=s.records+s.unpacked;
    if(rec)xbg_logf("[nb] draw apply (10 s): %llu OP_X_NB_DRAW %s (%.1f binds a record; tex_gen %.2f / buf_gen %.2f bumps a record), binds %.0f ns a record "
        "(1 in 64 timed, draw() not included); render enable off %llu, conditional %llu (drawn anyway, as the NVN path)",(unsigned long long)rec,
        s.unpacked?"unpacked (DM_NB_DRAW_UNPACK=1)":"applied",double(s.binds)/double(rec),double(s.tex_bumps)/double(rec),double(s.buf_bumps)/double(rec),
        s.timed?double(s.bind_ns)/double(s.timed):0.,(unsigned long long)s.ren_off,(unsigned long long)s.cond);
    if(s.checks)xbg_logf("[nb] draw check: %llu draws, bad %llu (texture %llu, sampler %llu, image %llu, uniform buffer %llu, limits %llu, draw arguments %llu, "
        "no draw record %llu); %.1f binds a record; render enable off %llu, conditional %llu (executed by neither path)",(unsigned long long)s.checks,
        (unsigned long long)s.bad,(unsigned long long)s.by[NBD_B_TEX],(unsigned long long)s.by[NBD_B_SAMP],(unsigned long long)s.by[NBD_B_IMG],
        (unsigned long long)s.by[NBD_B_UBO],(unsigned long long)s.by[BY_LIMIT],(unsigned long long)s.by[BY_DRAW],(unsigned long long)s.by[BY_NODRAW],
        double(s.cbinds)/double(s.checks),(unsigned long long)s.ren_off,(unsigned long long)s.cond);
    s=Stats{};}
}
#include "xbg_nb_ui.inc"   // [DOS2] NB-18b-2: DM_NB_UIPASS=2 / DM_NB_POST=2 audit (namespace nbui)
// [DOS2] NB-07: the table "program -> texture / image slots its shaders sample, per NVN stage" (dm/gfx/nb/nb_slotmask.h) for the front end
// (nvn_nb_tap.inc nbt_mask: a draw records as IR reads only the bound slots the bound program samples). Published once per program when
// program_shaders resolves it (key = prog_t::serial), from Shader::bindings: XBG_SOURCE_TEXTURE -> texture slots, XBG_SOURCE_IMAGE -> image
// slots (slot .. slot + count - 1, < 64), by nvn_stage of the shader. A stage that reads textures not by slot (UBO handles, pools) gets
// NB_SM_ALL; a program with a shader that failed to load is not published (unknown = conservative). Only while DM_NB is 1 / 2.
namespace nbsm{
nb_sm_table table;   // zero (BSS): pages are touched only by the programs published
int on=-1;
void publish(const prog_t *prog,const std::array<Shader*,6> &sh){
    if(on<0){const char *e=getenv("DM_NB");on=e && (atoi(e)==1 || atoi(e)==2);}
    if(!on || !prog || !prog->serial)return;
    nb_sm_masks m{};
    for(unsigned i=0;i<prog->nstage && i<6;i++){
        const Shader *s=sh[i];if(!s || s->stage>=6)return;
        const unsigned ns=nvn_stage[s->stage];
        if(s->pool_mask){m.tex[ns]=m.img[ns]=NB_SM_ALL;continue;}
        for(const auto &b:s->bindings){
            if(b.source==XBG_SOURCE_TEXTURE_UBO || b.source==XBG_SOURCE_IMAGE_UBO){m.tex[ns]=m.img[ns]=NB_SM_ALL;break;}
            if(b.source!=XBG_SOURCE_TEXTURE && b.source!=XBG_SOURCE_IMAGE)continue;
            uint64_t &w=b.source==XBG_SOURCE_IMAGE?m.img[ns]:m.tex[ns];
            for(uint32_t e=0;e<b.count && b.slot+e<64;e++)w|=1ull<<(b.slot+e);
        }
    }
    nb_sm_publish(&table,prog->serial,&m);
}
}
extern "C" nb_sm_table *xbg_nb_slotmask_table(void){return &nbsm::table;}   // external C linkage (read by the front end, weak there)
#include "xbg_nb_key.inc"   // [DOS2] NB-14: DM_NB_RELOC, OP_X_NB_KEY (namespace nbkey: base at FRAME, check / set at every pass, start() for NB-17a)
