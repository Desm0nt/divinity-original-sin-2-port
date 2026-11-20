// [DOS2] NR-ZERO Z8 (docs/dos2/nr/NR-ZERO.md section 25): DM_NR_Z8 = 1 / 2 - the thread that SUBMITS a recording knows each of its
// engine draws exactly: it folds every record into its own copy of the backend's State (in the order the translation thread will
// execute them), looks the draw up by the words of that copy, and writes the draw's recorder packets. The translation thread, for a
// draw answered this way, does not build a key, does not look anything up and does not write the command section: it takes the
// plan / NR-08 entry / NR-09 entry the walk named (checked against its table generations), runs the steps that are of the moment
// (the entry's images, the ring offsets, the set, the pass marks, the views, viewport) and hands the packets over.
// Included by xbg_game.cpp three times:
//   XBG_NR_Z8_PART 1 (right after xbg_nr_z5g.inc, before the lane)   namespace nrz8g - the submitting thread: the copy of State, the
//                    fix-id dictionaries, the walk of a submission, the run writer - and namespace nrz8t, first half - the
//                    translation thread: the switch, what it publishes per slot, the checks of =2.
//   XBG_NR_Z8_PART 2 (after xbg_nr_state3.inc, before execute())     the walk itself (it needs nr_state3.h's apply) and nrz8t::draw,
//                    the hook of draw() (it needs nrz3::answered).
//   XBG_NR_Z8_PART 3 (the end of the file, outside the unnamed namespace)   the entry points the NVN front calls.
// The pure parts: dm/gfx/nr/nr_z8.h (fold, walk, dictionaries, templates), nr_z5r.h (the run protocol: Writer on the submitting
// thread, Reader on the translation thread), nr_z5.h (the command section over a model of the bind cache). Host test tests/nr_z8_test.cpp.
//
// The transport is Z5's (xbg_nr_z5g.inc namespace nrz5t: rec_begin / rec_end around a record's draw(), tail() at the lane's command
// section, the held chunk every vkCmd* wrapper hands over first). What is new:
//   the walk     nvnQueueSubmitCommands, before the job is queued (under the front's Z8 lock: walks are in the queue's order): every
//                handle, record by record. A draw record gets: _pad0 = the submission's stamp, _pad2 = slot + 1, cond_addr = fix id |
//                template version << 32 (version 0: no template matched), _pad1 = the run's mark (nr_z5r.h; 0: a head draw).
//   the answer   priv[slot]: the plan, the NR-08 entry id, the NR-09 entry, the pass definition and the generations the translation
//                thread resolved them under, kept beside the template it published for the slot (same version).
//   nrz8t::draw  the hook in draw() (after its prologue, where nrz3 asks its table): a record whose template version is priv's and
//                whose generations stand takes nrz3::answered - the answered half of nrz3::draw - with the answer of priv.
// What is exact by construction and what is checked per draw:
//   - the answer is the one the translation thread itself resolved for a draw with the same compared words (site, fix id, texture /
//     sampler handles of the plan's rows, vertex ranges, uniform sizes) under the same table generations; the compared words are the
//     translation thread's State because the walk folds the same records in the same order with the same statements;
//   - the values the packets were written from are compared, field for field, with the values the translation thread resolves at the
//     moment (nr_z5r.h Reader: pipeline now in the NR-09 entry, set of the NR-08 entry, ring offsets, views, viewport, arguments), and
//     the bind cache with the run's model - a draw is taken only when all are equal; else its commands are recorded as today.
// Never answered: a UI draw, a draw with a render condition, QUADS, a draw the prologue changes (pre-pass override, twins, LIGHTFUSE,
// PRESKIN, POSONLY - their draws are not published), a record reached through OP_CALL, a re-executed shadow-cache range, a record of
// a handle submitted again, DM_NR_SORT / DM_NR_MDI.
//
//   DM_NR_Z8   0  default (xbg_nr_z8_default): the front pays one load a submission; nothing else exists.
//              2  verify: the walk folds, answers and writes ahead; the translation thread resolves and records everything itself and
//                 compares - its State with the walk's copy (a digest by part, 1 record in DM_NR_Z8_DIGEST), the answer of every
//                 record that had one with what it resolved, the packets of every draw =1 would take with its own.
//              1  apply.
//              Read at the backend's init (the walk must see every submission from the first). Needs DM_NB_MT != 0, DM_NR_DRAW=1,
//              DM_NR_DESC=1, DM_NR_Z5=0, DM_NB_ORDER / DM_NB_RELOC=1 / DM_NB_MX off (else a red line, off).
//   DM_NR_Z8_SLOTS   slots of the template table (8192; a power of two, 1024..32768; 960 bytes each)
//   DM_NR_Z8_MIN     a run's tail shorter than this is not handed over (4; 1..64)
//   DM_NR_Z8_DIGEST  =2: one record in this many carries the digest of the copy (8; 1..4096)
//   DM_NR_Z8_THREAD  [NR-ZERO.md section 28] 0 default (xbg_nr_z8_thread_default): the submitting thread walks, as above.
//                    1: the walk runs on a thread of its own ("dm-z8walk"): the submitting thread only posts the handles (under the
//                    same lock, in the queue's order). The walker writes nothing into the command memory: the words of a draw record
//                    go into nrz8g::marks by the record's place in its submission (nr_z8.h Marks); the translation thread takes them
//                    only when the walk of this submission wrote them for this record (a translation thread that is ahead of the
//                    walk resolves and records the draw itself), and waits for the walker at the END of a submission before the
//                    submission counts as executed (the game may reuse the command memory after that). Checked by DM_NR_Z8=2.
//   DM_NR_Z8_IMG     [section 29] 0 default (xbg_nr_z8_img_default). 1: an answered draw does not walk the images of its NR-08 entry
//                    while the entry's read word stands (nr_z3e.h nrz_rtq: the walk - get_image and prepare_read of every image the
//                    GPU wrote - would do nothing); 2: the word is asked, the walk runs and must have changed nothing.
//   DM_NR_Z8_VIEW    [section 30; needs DM_NR_Z8_THREAD=1] 0 default (xbg_nr_z8_view_default). 1: the walker numbers the distinct
//                    vertex / index ranges of a submission's answered draws (nr_z8.h Ranges); the lane's tail keeps the view of a
//                    range at the entry of its number under Z3c's words (xbg_nr_z3e.inc view_at: zero-copy view in place / static
//                    copy) - a repeat in the submission is answered from the entry, without snapshot() and without a hash;
//                    2: the entry is asked, snapshot() runs and must answer the same view.
//   DM_NR_Z8_VAL     [section 31; needs DM_NR_Z8_THREAD=1] 0 default (xbg_nr_z8_val_default). 1: the verdict of a tail draw is
//                    given from this thread's own memory - the values the walk wrote from are named from the slot's template
//                    (this thread's own, same version) and State (nr_z8.h expect_diff), the draw's packet bytes come with its
//                    mark - so neither the side chunk nor the chunk's packets are read (nr_z5r.h Reader::draw2); 2: the old
//                    verdict runs and what it finds in the chunks must be what this thread named.
//   DM_NR_Z8_SPLIT   with the thread: the first run of a submission the translation thread is close behind is published after this
//                    many tail draws, the next after twice as many, ... (8; 0 off, 1..1024)
// Lines: "[nr] DM_NR_Z8=..." at the start, "[nr] z8 walk: ...", "[nr] z8, translation thread: ...", "[nr] z8 by part ...",
// "[nr] z8 check ..." every 10 s, red "【红】[nr] z8: ...".
#if XBG_NR_Z8_PART == 1
#include "../../nr/nr_z5r.h"
#include "../../nr/nr_z8.h"
extern "C" void nvn_nr_z8_enable(int on) __attribute__((weak));   // nvn_nb_tap.inc: arms the submit hook
namespace nrdraw{struct Stats;}
namespace nrz8g{
int boot=0;                         // DM_NR_Z8 as read at init (0 / 1 / 2); 0 again when the translation thread refuses the switch
std::atomic<int> armed{0};
nrz8::Table tab;                    // written by the translation thread (nrz8t::publish), read by the walk; never freed
nrz5r::Ring ring;                   // the runs written ahead
uint32_t min_tails=4,dig_every=8;
// [the walk on its own thread, DM_NR_Z8_THREAD=1]
int thread_mode=0;                  // as read at boot (0 again when the thread could not be started)
int view_ids=0;                     // DM_NR_Z8_VIEW != 0 with the thread: the walker numbers the ranges of a submission
uint32_t split0=8;                  // DM_NR_Z8_SPLIT
nrz8::Marks marks;                  // the words of each draw record by its place in the submission: written by the walker, read by the translation thread; never freed
std::atomic<uint64_t> t_done{0};    // the last submission the translation thread finished (the walker asks: is it close behind me?)
std::atomic<uint64_t> posted{0};    // the last submission posted to the walker (a later one was never posted: nobody waits for its walk)
std::atomic<uint64_t> post_ns{0},posts{0},wakes{0},cpu_ns{0},jobs_done{0},passed{0};   // the submitting threads' cost of a post; the walker's thread CPU time
struct Sh:State{uint64_t sprog[6]{};};   // the copy: State + the program word of each stage (shader[] stays null)
// the words of State the digest of =2 covers, by part (everything fold() / nbd_apply / nr_state3 can write; not shader[], which is compared
// through the program words, and not restart_eff / count_samples / samples_passed, which draw() and the counters derive)
enum{DG_TARGET,DG_FORMAT,DG_BLEND,DG_DEPTH,DG_VB,DG_UB,DG_TEX,DG_STORE,DG_N};
inline const char *dg_name(int i){static const char *const n[DG_N]={"targets / viewport / scissor / pools","vertex attributes / streams","blend / colour / mask / multisample / alpha","depth-stencil / polygon / bias / bounds / restart",
    "vertex buffers","uniform buffers","texture / sampler handles","storage buffers / images / copy strides"};return n[i];}
template<class S> inline void digest(const S &st,uint64_t d[DG_N]){
    uint8_t buf[nrz8::GROUP_MAX];const uint64_t zero[6]={};
    d[DG_TARGET]=nrz8::hash_bytes(buf,nrz8::group_bytes(0,st,zero,buf));
    d[DG_FORMAT]=nrz8::hash_bytes(st.attrib,sizeof st.attrib)^nrz8::hash_bytes(st.stream,sizeof st.stream)*3;
    d[DG_BLEND]=nrz8::hash_bytes(buf,nrz8::group_bytes(2,st,zero,buf));
    d[DG_DEPTH]=nrz8::hash_bytes(buf,nrz8::group_bytes(3,st,zero,buf));
    d[DG_VB]=nrz8::hash_bytes(st.vb,sizeof st.vb);
    d[DG_UB]=nrz8::hash_bytes(st.ub,sizeof st.ub);
    d[DG_TEX]=nrz8::hash_bytes(st.tex,sizeof st.tex)^nrz8::hash_bytes(st.samp,sizeof st.samp)*3;
    d[DG_STORE]=nrz8::hash_bytes(st.sb,sizeof st.sb)^nrz8::hash_bytes(st.img,sizeof st.img)*3^uint64_t(st.copy_stride)*5^uint64_t(st.copy_image_stride)*7;}
// =2: the copy's digest at a draw record, for the translation thread (single producer / single consumer; a full ring drops the entry)
struct Dg{const uint8_t *rec;uint64_t sub,d[DG_N],sprog[6];uint32_t fixid,fresh;};
struct DgRing{enum{N=4096};Dg slot[N];std::atomic<uint32_t> w{0},r{0};
    bool push(const Dg &x){const uint32_t i=w.load(std::memory_order_relaxed);if(i-r.load(std::memory_order_acquire)>=N)return false;slot[i%N]=x;w.store(i+1,std::memory_order_release);return true;}
    uint32_t next() const{return w.load(std::memory_order_relaxed);}   // producer: the place the next push takes
    uint32_t at() const{return r.load(std::memory_order_relaxed);}     // consumer: the place of front()
    Dg *front(){const uint32_t i=r.load(std::memory_order_relaxed);return i==w.load(std::memory_order_acquire)?nullptr:&slot[i%N];}
    void pop(){r.store(r.load(std::memory_order_relaxed)+1,std::memory_order_release);}};
DgRing *dgr=nullptr;
enum{G_SUBS,G_RECORDS,G_DRAWS,G_NO_KEY,G_CALLED,G_EMPTY,G_MISS,G_VALUES=G_MISS+nrz8::M_N,G_HEADS,G_TAILS,G_RUNS,G_NO_CHUNK,G_RING_FULL,G_CHUNK_FULL,G_SHORT_RUNS,G_SHORT_TAILS,G_TAIL_BYTES,G_SIDE_BYTES,
     G_WALK_NS,G_DRAW_NS,G_DRAW_TIMED,G_FIX_NEW,G_FIX_REFRESH,G_FIX_CLEARS,G_FIX_STALE,G_DG_PUSHED,G_DG_FULL,G_REF_MISS,G_BAD,G_SPLITS,G_CLOSE,G_N};
std::atomic<uint64_t> pub[G_N];   // the walk's counters, published at the end of every submission (relaxed; read by nrz8t::tick)
}
namespace nrz8t{
int mode=0;                         // 0 off, 1 apply, 2 verify (set by init(), from nrz3::init on the translation thread)
bool guarded=false;                 // draw(), at the hook: the prologue changed State for the draw in hand (pre-pass override, a shader twin, LIGHTFUSE, a post fusion)
uint64_t cur_sub=0;                 // the number of the submission being executed (0: not one the front announced)
// what the translation thread resolved for the record it published a slot's template from (same version as the template)
struct Priv{uint32_t ver=0;int32_t did=-1;uint64_t plan=0,pent=0,passdef=0,stamp[3]{};uint8_t frag=0;const Shader *sh[5]{};};   // sh: the shader objects the plan and the NR-09 entry belong to
Priv *priv=nullptr;
// the draw in hand, from the lane's tail (before nrz5t::recorded): what a template needs besides the values
struct Hint{const void *pent=nullptr;int zero=-1;bool qpat=false,pp=false,special=false;uint64_t lane_gen=0;} hint;
struct Stats{uint64_t asked=0,no_tmpl=0,stamp=0,again=0,replaced=0,stale=0,passdef=0,gate=0,shaders=0,taken_path=0,published=0,pub_no=0,pub_why[8]{},a_cmp=0,a_same=0,a_stale=0,a_bad=0,
             dg_cmp=0,dg_same=0,dg_bad=0,dg_unreached=0,sh_bad=0,spot=0,spot_bad=0,pre_ns=0,pre_n=0;} s;
uint64_t next_ns=0,last[nrz8g::G_N]{},pre_calls=0;unsigned told=0;
nrz8::MarkReader mr;                // [DM_NR_Z8_THREAD=1] the place of the record in hand in its submission
// ---- [DM_NR_Z8_IMG] the read word of an NR-08 entry (nrz3::answered asks it for a draw the walk answered) ----
int imode=0;
// ---- [DM_NR_Z8_VIEW] the view of a range at the entry of its place among the submission's ranges ----
int vmode=0;uint64_t cur_vw=0;      // cur_vw: the places of the ranges of the record in hand (0: none)
// ---- [DM_NR_Z8_VAL] the verdict of a tail draw without the walker's chunks ----
int kmode=0;uint64_t cur_pk=0;      // cur_pk: the packets of the record in hand in its run's chunk, bytes | count << 32 (0: not a tail draw)
struct KStats{uint64_t asked=0,named=0,differ=0;} ks;
bool known(const nbd_hdr &rh,const nrz5_vals &rv,nrz5r::Reader::Known *k);
enum{VC_N=4096};nrz_view vc[VC_N];nrzx::Stats vs;uint64_t v_none=0;
// lane_tail: vertex binding number k of the draw (in ascending order; 3: the index range).
inline Buffer view(uint64_t address,size_t bytes,int site,uint32_t stride,unsigned k){
    const uint32_t id=k<4?uint32_t(cur_vw>>(16*k))&0xffffu:0u;
    if(!id || id>=VC_N || snap_no_sync || nrbuf::busy || nrbuf::mode==2){++v_none;return nrzx::view_real(address,bytes,site,stride);}
    ++vs.c_asked;return nrzx::view_at(&vc[id],address,bytes,site,stride,vmode,vs);}
struct IStats{uint64_t asked=0,hooks=0,stood=0,set=0,checked=0,bad=0,no_set[5]{};} is;
// What prepare_read does besides its checks, per read, when it has nothing to do: none of it may be armed (each is a note, a
// list or a pending clear to land that a skipped read would not make).
inline bool img_hooks_off(){return !pclear_n && !draw_reads_on && nbtr::mode<=0 && !nbchk::armed && prepass::mode<=0 && !hist_on && nrrp::mode==0 && opbar::mode!=2 && nrpres::mode==0 && !passval::on();}
inline bool img_ask(const nrdesc::Ent &e,bool hook){++is.asked;
    if(hook || !img_hooks_off()){++is.hooks;return false;}
    if(!nrz_rtq_ok(&e.rtq,nrz_img_ep,nrz_sub_now(),slot_serial,write_seq-nrz_pm,uint64_t(open_pass.pass),uint64_t(open_pass.framebuffer),read_frag_only))return false;
    ++is.stood;return true;}
struct ImgSnap{VkCommandBuffer cmd{};uint64_t bs=0,fs=0,ws=0,serial=0,ep=0,cg=0,dfr=0;};
inline ImgSnap img_snap(const nrdesc::Ent &e){ImgSnap z;z.cmd=open_pass.cmd;z.bs=barrier_seq;z.fs=frag_seq;z.ws=write_seq;z.serial=slot_serial;z.ep=nrz_img_ep;z.cg=uint64_t(e.check_gen);z.dfr=uint64_t(depth_feedback_reads);return z;}
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
// After the walk of the entry's images (the draw is answered so far; cont: the slot and the open pass are still the draw's).
// stood (=2): the word stood before the walk - the walk must have changed nothing. Else: the word is set when every image is now
// quiet or "read with nothing to do".
inline void img_done(nrdesc::Ent &e,bool hook,bool stood,const ImgSnap &z0,bool cont){
    if(stood){++is.checked;const ImgSnap z1=img_snap(e);
        if(z0.cmd!=z1.cmd || z0.bs!=z1.bs || z0.fs!=z1.fs || z0.ws!=z1.ws || z0.serial!=z1.serial || z0.ep!=z1.ep || z0.cg!=z1.cg || z0.dfr!=z1.dfr){++is.bad;
            red("the read word of an NR-08 entry stood, its walk %s",z0.cmd!=z1.cmd?"ended the open pass":z0.bs!=z1.bs || z0.fs!=z1.fs?"recorded a barrier":z0.ws!=z1.ws?"moved a write mark":z0.serial!=z1.serial?"ended the submission":
                z0.ep!=z1.ep?"moved the image epoch":z0.cg!=z1.cg?"found another image behind a texture":"read the depth of the open pass");}
        return;}
    if(hook || !cont || !img_hooks_off() || !e.check_gen){++is.no_set[0];return;}
    const bool fo=read_frag_only;
    for(const MemoImage &ti:e.img){Image &im=*ti.second;
        const nr_dimg_in x{im.last_write,im.storage_write,im.opw,uint8_t(im.checked_submission==stats.submissions),uint8_t(im.uploaded),uint8_t(im.gpu_written),uint8_t(im.layout==VK_IMAGE_LAYOUT_GENERAL),uint8_t(hook)};
        const int k=nr_dimg_skip(&x);nrz_rt_in r{};r.skip=uint8_t(k==NRD_I_SKIP);r.rt=uint8_t(k==NRD_I_RT);
        if(!r.skip){r.checked=x.checked;r.content=uint8_t(im.uploaded || im.gpu_written);r.general=x.general;
            r.in_open=uint8_t(std::find(open_pass.images.begin(),open_pass.images.end(),&im)!=open_pass.images.end());
            const bool as_storage=!im.sampled_ever;uint64_t seen=fo?frag_seq:barrier_seq;if(opbar::mode==1)seen=nb_ob_seen(seen,fo?im.opvis_frag:im.opvis_full);   // (as prepare_read)
            r.written=uint8_t(nrrp::lw(im)>seen || (!as_storage && im.storage_write>seen));}
        if(!nrz_rt_img_ok(&r)){++is.no_set[!r.rt?1:r.in_open?2:r.written?3:4];return;}}
    nrz_rtq_set(&e.rtq,nrz_img_ep,nrz_sub_now(),slot_serial,write_seq-nrz_pm,uint64_t(open_pass.pass),uint64_t(open_pass.framebuffer),fo);++is.set;}
struct TStats{uint64_t waits=0,wait_ns=0,ends=0,recs=0;} ts;uint64_t tl_post=0,tl_posts=0,tl_cpu=0,tl_jobs=0,tl_wakes=0,tl_passed=0;nrz8::MarkReader mrl;
void guard(bool on){guarded=on;}
// nrz5t::tail, with the values the tail resolved (rv): what this thread can say of the tail draw in hand by itself. false: nothing
// (not a tail draw of a walk on its own thread, the slot was published again, the prologue changed State for this draw).
bool known(const nbd_hdr &rh,const nrz5_vals &rv,nrz5r::Reader::Known *k){
    ++ks.asked;
    const uint32_t ver=uint32_t(rh.cond_addr>>32);
    if(!cur_pk || !ver || guarded || (rh.flags&(NBD_F_REN|NBD_F_COND)) || rh._pad2<=0 || uint32_t(rh._pad2-1)>=nrz8g::tab.n || priv[rh._pad2-1].ver!=ver)return false;
    const nrz8::Slot &x=nrz8g::tab.s[rh._pad2-1];   // this thread is the slot's only writer: while priv's version is the record's, these are the words the walk read
    struct A{bool ub(unsigned st,unsigned sl,uint64_t *a,uint64_t *z) const{if(st>=6 || sl>=16)return false;*a=uint64_t(state.ub[st][sl].address);*z=uint64_t(state.ub[st][sl].size);return true;}};
    k->d=nrz8::expect_diff([&x](unsigned j){return x.w[j].load(std::memory_order_relaxed);},&rv,rh.count,rh.instances,rh.first,rh.base_vertex,rh.base_instance,A{});
    k->bytes=uint32_t(cur_pk);k->n=uint32_t(cur_pk>>32);++ks.named;if(k->d)++ks.differ;return true;}
uint64_t pre_begin(){return nrdraw::cur && !(++pre_calls&63)?timing_clock():0;}
void pre(uint64_t t0){s.pre_ns+=timing_clock()-t0;++s.pre_n;}   // draw(): the prologue's time up to the hook, 1 engine draw in 64
void red(const char *fmt,...){if(told>=16)return;++told;char b[640];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof b,fmt,ap);va_end(ap);
    xbg_logf("【红】[nr] z8: %s (frame %llu, vs=%s ps=%s)",b,(unsigned long long)stats.presents.load(),state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-");}
// The backend's init, right after `state` got its start values: the switch is read here because the walk must fold every submission
// from the first one. The copy starts as the State of this moment.
void boot();
// From nrz5::init (the translation thread, once DM_NB_MT, NR-08 and NR-09 are known). m = nrz8g::boot. false: refused (a red line; the walk stops).
bool init(int draw_mode);
// The lane is released (shutdown): the walk stops, quietly. The copy is not kept up from here on, so the switch does not come back.
void stop(){if(nrz8g::armed.load(std::memory_order_relaxed)){nrz8g::armed.store(0,std::memory_order_release);if(nvn_nr_z8_enable)nvn_nr_z8_enable(0);}mode=0;nrz8g::boot=0;}
void off(const char *why){if(nrz8g::armed.load(std::memory_order_relaxed)){nrz8g::armed.store(0,std::memory_order_release);if(nvn_nr_z8_enable)nvn_nr_z8_enable(0);}
    if(mode || nrz8g::boot)xbg_logf("【红】[nr] DM_NR_Z8=%d: %s: off",nrz8g::boot,why);mode=0;nrz8g::boot=0;}
// The program word the walk keeps for a stage and the shader the translation thread has there must name the same thing.
inline Shader *shader_of(uint64_t w,unsigned st){
    if(!w || (w>>63))return nullptr;auto it=program_cache.find(w);if(it==program_cache.end())return nullptr;
    Shader *out=nullptr;for(Shader *sh:it->second)if(sh && unsigned(sh->stage)==st)out=sh;return out;}
// nbdraw::apply, before the draw() of a record (from nrz5t::rec_begin): a record the walk of THIS submission did not write its words
// into carries nothing; =2: the copy's digest at this record against State.
inline void rec_begin(nbd_hdr &h,const uint8_t *rec){
    cur_vw=0;cur_pk=0;
    if(__builtin_expect(nrz8g::thread_mode,0)){   // [DM_NR_Z8_THREAD=1] the record carries nothing: its words are the walker's entry of its place in this submission - or it has none
        if(nrshc::again!=NRSHC_R_NONE){++s.again;return;}   // (a re-executed range is not counted: the walk met its records once)
        if(mr.sub!=cur_sub)mr.begin(cur_sub);
        ++ts.recs;nrz8::MarkVal mv;h._pad0=0;h._pad1=0;h._pad2=0;if(!(h.flags&NBD_F_COND))h.cond_addr=0;   // (whatever the record's unused words hold is not read)
        if(!cur_sub || !mr.next(nrz8g::marks,rec,&mv))return;
        h._pad0=nrz8::stamp_of(cur_sub);h._pad1=mv.mark;h._pad2=mv.slot1;if(!(h.flags&NBD_F_COND))h.cond_addr=mv.ca;
        cur_vw=mv.vw;cur_pk=mv.pk;
        if(mode!=2 || !nrz8g::dgr || !mv.aux)return;
        const uint32_t want=uint32_t(mv.aux-1);
        while(nrz8g::dgr->front() && int32_t(want-nrz8g::dgr->at())>0){nrz8g::dgr->pop();++s.dg_unreached;}   // (digests of records this thread executed before the walk reached them)
        if(!nrz8g::dgr->front() || nrz8g::dgr->at()!=want)return;}
    else{
    const bool mine=cur_sub && h._pad0==nrz8::stamp_of(cur_sub) && nrshc::again==NRSHC_R_NONE;
    if(!mine){if(h._pad0 && nrshc::again!=NRSHC_R_NONE)++s.again;else if(h._pad0)++s.stamp;h._pad1=0;h._pad2=0;h.cond_addr&=0xffffffffull;return;}
    if(mode!=2 || !nrz8g::dgr)return;}
    for(;;){nrz8g::Dg *d=nrz8g::dgr->front();if(!d)return;
        if(d->sub<cur_sub){nrz8g::dgr->pop();++s.dg_unreached;continue;}   // (a record of an earlier submission that was never executed)
        if(d->sub!=cur_sub || d->rec!=rec)return;   // (a digest of a later record: this record carries none)
        uint64_t now[nrz8g::DG_N];nrz8g::digest(state,now);++s.dg_cmp;int bad=-1;
        for(int i=0;i<nrz8g::DG_N;i++)if(now[i]!=d->d[i]){bad=i;break;}
        bool shb=false;for(unsigned st=0;st<6 && !shb;st++){const uint64_t w=d->sprog[st];if(w>>63)continue;if(shader_of(w,st)!=state.shader[st])shb=true;}   // (a program without a serial is never a key)
        if(bad>=0){++s.dg_bad;red("the walk's copy of State differs from State at a draw record (submission %llu): %s",(unsigned long long)cur_sub,nrz8g::dg_name(bad));}
        else if(shb){++s.sh_bad;red("the walk's program words do not name the shaders State holds at a draw record (submission %llu)",(unsigned long long)cur_sub);}
        else ++s.dg_same;
        if(d->fixid!=d->fresh)red("the walk's fix id (%u) is not the id of the copy's bytes (%u): a record wrote a group the fold did not mark",d->fixid,d->fresh);
        nrz8g::dgr->pop();return;}}
// lane_tail, after the command section it ran itself (through nrz5t::recorded): the template of the record's slot and the answer
// beside it; =2 first: the answer the walk used for this record against what the path resolved.
void publish(const nbd_hdr &rh,uint32_t key,const nrz5_vals &v,uint32_t first_index,uint32_t k,const nr_dplan *plan_hint){
    if(!rh._pad0 || rh._pad2<=0 || uint32_t(rh._pad2-1)>=nrz8g::tab.n || !key)return;
    const uint32_t slot=uint32_t(rh._pad2-1),ver=uint32_t(rh.cond_addr>>32);Priv &pv=priv[slot];
    nrdesc::Plan *pl=(nrdesc::mode==1 && (plan_hint || nrdesc::z_out==NRZD_HIT || nrdesc::z_out==NRZD_BUILT))?nrdesc::plan_for():nullptr;
    const uint64_t stamp[3]={nrdesc::tab_gen,hint.lane_gen,nbpipe::epoch};
    if(ver && mode==2 && pv.ver==ver && !guarded && !hint.pp){++s.a_cmp;   // the walk had a template: its answer (a draw the prologue changed is never answered)
        bool shs=true;for(unsigned i=0;i<5;i++)shs=shs && pv.sh[i]==state.shader[i];
        if(!shs || memcmp(pv.stamp,stamp,sizeof stamp) || pv.passdef!=uint64_t(uintptr_t(last_pass.pass.definition)))++s.a_stale;   // (=1 would not have used it)
        else if(pl && pl->last>=0 && pv.plan==uint64_t(uintptr_t(pl)) && pv.did==pl->last && pv.pent==uint64_t(uintptr_t(hint.pent)) && pv.frag==uint8_t(desc_frag_only_shaders()))++s.a_same;
        else{++s.a_bad;red("the answer the walk took for a draw is not what the path resolved: plan %p / %p, NR-08 entry %d / %d, NR-09 entry %p / %p (key %u, slot %u)",
            (void*)uintptr_t(pv.plan),(void*)pl,pv.did,pl?pl->last:-1,(void*)uintptr_t(pv.pent),hint.pent,key,slot);}}
    // what cannot be a template (never answered)
    int why=0;
    if(!pl || pl->p.why || pl->last<0)why=1;                                   // NR-08 did not serve the draw
    else if(pl->p.nubo!=pl->p.ndyn || pl->p.ndyn!=v.noff || pl->p.nstat)why=2;  // a static uniform block: the set depends on its offset
    else if(pl->p.nimg>nrz8::MAX_ROWS || v.noff>nrz8::MAX_DYN)why=3;
    else if(!hint.pent || hint.special || hint.qpat || hint.zero>=0)why=4;      // no NR-09 entry of its own (PRESKIN / POSONLY variant), a pattern or zero stream
    else if(hint.pp || guarded)why=5;                                           // the prologue changed State for this draw: its words are not the walk's copy
    else if(!state.shader[0] || state.shader[1] || state.shader[2] || state.shader[3])why=6;
    if(why){++s.pub_no;++s.pub_why[why];if(pv.ver){const uint64_t none[nrz8::HEAD_WORDS]={uint64_t(nrz8::HEAD_WORDS)};pv=Priv{};pv.ver=nrz8g::tab.put(slot,none,nrz8::HEAD_WORDS);}return;}
    const nr_dplan &p=pl->p;nrz8::Desc d;
    nrz5_site_make(&d.site,key,rh.draw==NBD_DRAW_IDX?NRZ5S_IDX:NRZ5S_ARRAYS,rh.prim,rh.itype,rh.count,rh.base_vertex,rh.base_instance,rh.instances,rh.first,rh.ibuf,state.vb[0].address);
    d.fixid=uint32_t(rh.cond_addr);d.v=&v;d.first_index=first_index;d.k=k;d.noff=v.noff;
    for(unsigned u=0;u<p.nubo;u++){const nr_drow &r=p.row[p.ubo_row[u]];if(r.kind!=NRD_K_RING_DYN || r.dyn_at>=v.noff || r.ns>=6 || r.slot>=16){why=2;break;}
        d.dyn[r.dyn_at].st=r.ns;d.dyn[r.dyn_at].sl=r.slot;d.dyn[r.dyn_at].addr=uint64_t(state.ub[r.ns][r.slot].address);d.dyn[r.dyn_at].rbytes=r.bytes;d.dyn[r.dyn_at].copy=pl->copy[u];}
    unsigned nvb=0;for(uint32_t m=v.used;m && !why;m&=m-1){const uint32_t i=uint32_t(__builtin_ctz(m));if(nvb>=nrz8::MAX_VB){why=3;break;}
        d.vb[nvb].at=uint8_t(i);d.vb[nvb].addr=uint64_t(state.vb[i].address);d.vb[nvb].size=uint64_t(state.vb[i].size);++nvb;}
    d.nvb=nvb;d.nrow=p.nimg;
    for(unsigned i=0;i<p.nimg && !why;i++){const nr_drow &r=p.row[p.img_row[i]];d.row[i].ns=r.ns;d.row[i].slot=r.slot;d.row[i].samp=r.samp;
        d.row[i].tex=state.tex[r.ns][r.slot];d.row[i].smp=r.samp!=0xff?state.samp[r.ns][r.samp]:0;}
    uint64_t w[nrz8::MAX_WORDS];const unsigned nw=why?0u:nrz8::pack(d,w);
    if(!nw){++s.pub_no;++s.pub_why[why?why:7];return;}
    Priv n;n.ver=nrz8g::tab.put(slot,w,nw);n.plan=uint64_t(uintptr_t(pl));n.did=pl->last;n.pent=uint64_t(uintptr_t(hint.pent));n.passdef=uint64_t(uintptr_t(last_pass.pass.definition));
    memcpy(n.stamp,stamp,sizeof stamp);n.frag=uint8_t(desc_frag_only_shaders());for(unsigned i=0;i<5;i++)n.sh[i]=state.shader[i];
    if(n.ver!=pv.ver)++s.published;
    pv=n;}
void tick();
}
#elif XBG_NR_Z8_PART == 2
namespace nrz8g{
// ---- the walk (the submitting thread; one walk at a time, under the front's lock) ----
inline bool bind_prog(Sh &S,const prog_t *prog,int32_t stages){
    if(prog && prog->nstage>6)return false;
    // (execute()'s OP_BIND_PROGRAM: the stages of the mask lose their shader, then take the program's shader of that stage - or none)
    const uint64_t w=!prog?0:prog->serial?prog->serial:(1ull<<63|uint64_t(uintptr_t(prog)));   // no serial: a program the backend does not cache - never a key
    for(unsigned st=0;st<6;st++)if(stages&(1u<<nvn_stage[st]))S.sprog[st]=w;
    return true;}
#define NRZ8_BIND(S,pr,st) do{if(!nrz8g::bind_prog(*(S),reinterpret_cast<const prog_t*>(uintptr_t(pr)),(st)))return -1;}while(0)
#define NRZ8_VATTR(a,st,n) xbg_vertex_attributes_replace((a),(st),(n))
NR3_DEFINE_APPLY_RAW(apply_copy,Sh,NRZ8_BIND,NRZ8_VATTR)
struct Hnd;
struct G{Sh S;nrz8::Fix fix;nrz8::Walker<Hnd> W;nrz5r::Writer wr;nrz8::Ranges vr;bool ready=false,answers=false,verify=false;std::atomic<bool> bad{false};uint64_t c[G_N]{},sub=0,n=0;uint8_t stamp=0;} g;
struct Acc{
    bool ub(unsigned st,unsigned sl,uint64_t *a,uint64_t *z) const{if(st>=6 || sl>=16)return false;*a=uint64_t(g.S.ub[st][sl].address);*z=uint64_t(g.S.ub[st][sl].size);return true;}
    bool vb(unsigned i,uint64_t *a,uint64_t *z) const{if(i>=16)return false;*a=uint64_t(g.S.vb[i].address);*z=uint64_t(g.S.vb[i].size);return true;}
    uint64_t tex(unsigned ns,unsigned slot) const{return ns<6 && slot<128?g.S.tex[ns][slot]:~0ull;}
    uint64_t samp(unsigned ns,unsigned slot) const{return ns<6 && slot<128?g.S.samp[ns][slot]:~0ull;}};
struct Hnd{
    void state_op(uint32_t op,const uint8_t *p,size_t n){
        const unsigned d=nrz8::fold(g.S,op,p,n,[](const prog_t *pr,int32_t stages){return bind_prog(g.S,pr,stages);},xbg_vertex_attributes_replace);
        if(d&nrz8::D_BAD)g.bad=true;else g.fix.mark(d);}
    void nr_state(const uint8_t *p,size_t n){
        if(n<sizeof(nr3_hdr)){g.bad=true;return;}
        if(p[1])return;   // DM_NR_STATE=5's check records: applied to a copy by the backend, State is written by the old records that follow
        if(apply_copy(&g.S,p,n)!=0){g.bad=true;return;}
        g.fix.mark(nrz8::dirty_of_nr3(p,n));}
    void other(uint32_t){g.wr.end_run();}
    void nb_draw(uint8_t *rec,size_t n,bool top){
        nbd_hdr h;const uint8_t *b=nullptr;
        if(nbd_view(rec,n,&h,&b)!=0){g.bad=true;return;}
        if(h.flags&NBD_F_CHECK)return;   // compared only by the backend (the old records carry the binds and the draw)
        unsigned ts=0,bs=0;
        if(nbd_apply(&h,b,g.S.tex,g.S.samp,g.S.img,reinterpret_cast<nbd_range(*)[16]>(g.S.ub),&ts,&bs)!=0){g.bad=true;return;}
        ++g.c[G_DRAWS];
        nrd_tail t;const bool nr=nrd_tail_get(rec,n,&h,&t)!=0;
        const bool timed=!(++g.n&63);const uint64_t t0=timed?nbmtq::now_ns():0;
        uint32_t mark=0;int32_t slot1=0;uint64_t ca=0,dg_at=0,vw=0;
        if(!top){++g.c[G_CALLED];g.wr.no_values();}
        else if(!g.answers || !nr || !t.key || t.site==NRD_SITE_UI || (h.draw!=NBD_DRAW_IDX && h.draw!=NBD_DRAW_ARRAYS) || (h.flags&(NBD_F_REN|NBD_F_COND)) || h.prim==7 || h.count<=0 ||
                (h.draw==NBD_DRAW_IDX && (h.instances<=0 || h.base_instance<0 || h.itype<1 || h.itype>2 || !h.ibuf)) || (g.S.sprog[0]>>63) || (g.S.sprog[4]>>63)){++g.c[G_NO_KEY];g.wr.no_values();}
        else{
            nrz5_site site;nrz5_site_make(&site,t.key,h.draw==NBD_DRAW_IDX?NRZ5S_IDX:NRZ5S_ARRAYS,h.prim,h.itype,h.count,h.base_vertex,h.base_instance,h.instances,h.first,h.ibuf,uint64_t(g.S.vb[0].address));
            const uint32_t fixid=g.fix.get(g.S,g.S.sprog);const uint32_t s=tab.slot_of(&site,fixid);slot1=int32_t(s+1);ca=fixid;
            uint64_t w[nrz8::MAX_WORDS];uint32_t ver=0;nrz5_vals v;uint32_t off[nrz8::MAX_DYN];
            if(!fixid || !tab.get(s,w,&ver)){++g.c[G_EMPTY];g.wr.no_values();}
            else{const int m=nrz8::match(w,&site,fixid,h.count,h.instances,h.first,h.base_vertex,h.base_instance,Acc{},&v,off);
                if(m){++g.c[G_MISS+m];g.wr.no_values();}
                else{++g.c[G_VALUES];ca|=uint64_t(ver)<<32;
                    mark=g.wr.draw(v,[](nbmtq::Chunk *ch,nrz5_cache *mm,const nrz5_vals *vv){nbmt::Build bb{ch};nbmt::Z5Sink sink{bb};return nrz5_emit(sink,mm,vv);});
                    if(view_ids)vw=nrz8::ranges_word(g.vr,v.used,[](uint32_t i,uint64_t *a,uint64_t *z){*a=uint64_t(g.S.vb[i].address);*z=uint64_t(g.S.vb[i].size);return i<16;},   // [DM_NR_Z8_VIEW] the ranges the lane's tail will ask for
                        h.draw==NBD_DRAW_IDX,uint64_t(h.ibuf),uint64_t(uint32_t(h.count))<<h.itype);}}
            if(g.verify && dgr && !(g.c[G_DRAWS]%dig_every)){Dg x;x.rec=rec;x.sub=g.sub;digest(g.S,x.d);memcpy(x.sprog,g.S.sprog,sizeof x.sprog);x.fixid=fixid;x.fresh=g.fix.fresh(g.S,g.S.sprog);
                const uint32_t at=dgr->next();
                if(x.fixid!=x.fresh)++g.c[G_FIX_STALE];if(dgr->push(x)){++g.c[G_DG_PUSHED];dg_at=uint64_t(at)+1;}else ++g.c[G_DG_FULL];}}
        if(thread_mode){   // [DM_NR_Z8_THREAD=1] the translation thread may be executing this record: nothing is written into it
            nrz8::MarkVal mv;mv.ca=(h.flags&NBD_F_COND)?0:ca;mv.mark=mark;mv.slot1=slot1;mv.aux=dg_at;mv.vw=vw;mv.pk=mark?uint64_t(g.wr.last_bytes)|uint64_t(g.wr.last_cmds)<<32:0;marks.put(rec,mv);}
        else{
        // the record's words (plain stores: the job that carries this recording is not queued yet)
        rec[offsetof(nbd_hdr,_pad0)]=g.stamp;memcpy(rec+offsetof(nbd_hdr,_pad1),&mark,4);memcpy(rec+offsetof(nbd_hdr,_pad2),&slot1,4);
        if(!(h.flags&NBD_F_COND))memcpy(rec+offsetof(nbd_hdr,cond_addr),&ca,8);}
        if(timed){g.c[G_DRAW_NS]+=nbmtq::now_ns()-t0;++g.c[G_DRAW_TIMED];}}
};
// nvnQueueSubmitCommands, before the job of this recording is queued. sub: the number the job will get.
inline void submit(const uint64_t *handles,int n,uint64_t sub){
    if(!g.ready || g.bad)return;
    const uint64_t t0=nbmtq::now_ns();
    g.sub=sub;g.stamp=nrz8::stamp_of(sub);g.answers=armed.load(std::memory_order_acquire)==2;g.verify=boot==2;
    g.vr.begin();
    if(thread_mode){const bool close=t_done.load(std::memory_order_relaxed)+1>=sub;   // the translation thread has nothing older to do: it starts this submission with the walk
        g.wr.max_tails=close?split0:0;if(close)++g.c[G_CLOSE];}
    uint64_t budget=0;const uint64_t r0=g.W.records;
    for(int i=0;i<n && !g.bad;i++){if(!g.W.walk(reinterpret_cast<const uint8_t*>(uintptr_t(handles[i])),0,budget,true))g.bad=true;g.wr.end_run();}   // (the end of a handle ends the run: the backend closes its handle there)
    g.wr.end_run();
    ++g.c[G_SUBS];g.c[G_RECORDS]+=g.W.records-r0;g.c[G_WALK_NS]+=nbmtq::now_ns()-t0;
    const nrz5r::WStats &w=g.wr.st;g.c[G_HEADS]=w.heads;g.c[G_TAILS]=w.tails;g.c[G_RUNS]=w.runs;g.c[G_NO_CHUNK]=w.no_chunk;g.c[G_RING_FULL]=w.ring_full;g.c[G_CHUNK_FULL]=w.chunk_full;
    g.c[G_SHORT_RUNS]=w.short_runs;g.c[G_SHORT_TAILS]=w.short_tails;g.c[G_TAIL_BYTES]=w.tail_bytes;g.c[G_SIDE_BYTES]=w.side_bytes;g.c[G_SPLITS]=g.wr.splits;
    g.c[G_FIX_NEW]=g.fix.tuple.news;g.c[G_FIX_REFRESH]=g.fix.refreshed;g.c[G_FIX_CLEARS]=g.fix.tuple.clears;g.c[G_REF_MISS]=g.W.ref_miss;g.c[G_BAD]=g.bad.load()?1:0;
    for(int i=0;i<G_N;i++)pub[i].store(g.c[i],std::memory_order_relaxed);}
// ---- [DM_NR_Z8_THREAD=1] the walker thread: the submitting thread posts, this thread walks ----
struct Job{uint64_t sub;int n;uint64_t *big;uint64_t in[8];};
struct Wk{std::mutex mu;std::condition_variable cv;std::deque<Job> q;bool idle=false;} wk;
// nvnQueueSubmitCommands, under the front's Z8 lock, before the job is queued: the handles are copied (the array is the game's).
inline void post(const uint64_t *handles,int n,uint64_t sub){
    const uint64_t t0=nbmtq::now_ns();Job j;j.sub=sub;j.n=n>0?n:0;j.big=nullptr;
    if(j.n>8){j.big=static_cast<uint64_t*>(malloc(size_t(j.n)*8));if(!j.big){g.bad.store(true);j.n=0;}}
    if(j.n)memcpy(j.big?j.big:j.in,handles,size_t(j.n)*8);
    bool wake;{std::lock_guard<std::mutex> l(wk.mu);wk.q.push_back(j);wake=wk.idle;posted.store(sub,std::memory_order_release);}
    if(wake){wk.cv.notify_one();wakes.fetch_add(1,std::memory_order_relaxed);}
    posts.fetch_add(1,std::memory_order_relaxed);post_ns.fetch_add(nbmtq::now_ns()-t0,std::memory_order_relaxed);}
inline uint64_t thread_cpu_ns(){timespec t;if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)!=0)return 0;return uint64_t(t.tv_sec)*1000000000ull+uint64_t(t.tv_nsec);}
// Every posted job leaves through marks.end() / marks.pass(): the translation thread waits for exactly that (nrz8t::exec_done).
void walker_main(){
    pthread_setname_np(pthread_self(),"dm-z8walk");
    for(;;){Job j;
        {std::unique_lock<std::mutex> l(wk.mu);while(wk.q.empty()){wk.idle=true;wk.cv.wait(l);}wk.idle=false;j=wk.q.front();wk.q.pop_front();}
        if(armed.load(std::memory_order_acquire) && g.ready && !g.bad){
            const uint64_t c0=thread_cpu_ns();
            marks.begin(j.sub);submit(j.big?j.big:j.in,j.n,j.sub);marks.end();
            cpu_ns.fetch_add(thread_cpu_ns()-c0,std::memory_order_relaxed);jobs_done.fetch_add(1,std::memory_order_relaxed);}
        else{marks.pass(j.sub);passed.fetch_add(1,std::memory_order_relaxed);}   // (the walk stopped: the command memory is not read)
        free(j.big);}}
}
namespace nrz8t{
// [DM_NR_Z8_THREAD=1] x_submit, after the last record of submission `sub`: the walker must have left this submission's command memory
// before the submission counts as executed (the fence the game waits on before it reuses the memory). The walker is normally far ahead.
void exec_done(uint64_t sub){
    nrz8g::t_done.store(sub,std::memory_order_relaxed);
    if(!nrz8g::thread_mode)return;
    if(sub>nrz8g::posted.load(std::memory_order_acquire))return;   // (never posted: submitted before the switch was armed or after it went off)
    ++ts.ends;
    if(nrz8g::marks.done.load(std::memory_order_acquire)>=sub)return;
    const uint64_t t0=timing_clock();++ts.waits;
    for(unsigned i=0;nrz8g::marks.done.load(std::memory_order_acquire)<sub;i++){
        if(i<64)sched_yield();else usleep(50);
        if((i&255)==255 && timing_clock()-t0>5000000000ull){red("the walker thread did not leave submission %llu in 5 s (it left %llu): the walk is switched off",(unsigned long long)sub,
            (unsigned long long)nrz8g::marks.done.load(std::memory_order_relaxed));nrz8g::g.bad.store(true);break;}}
    ts.wait_ns+=timing_clock()-t0;}
void boot(){
    const char *e=getenv("DM_NR_Z8");int m=e && *e?atoi(e):xbg_nr_z8_default();
    if(m<0 || m>2){xbg_logf("【红】[nr] DM_NR_Z8=%d: only 0 / 1 / 2: off",m);m=0;}
    if(nrz8g::g.ready){nrz8g::g.bad.store(true);xbg_logf("【红】[nr] DM_NR_Z8: the backend's State was reset again after the walk started: the walk stops");return;}   // (the copy would no longer be State)
    if(!m)return;
    if(!nvn_nr_z8_enable){xbg_logf("【红】[nr] DM_NR_Z8=%d: this build's NVN front has no submit hook: off",m);return;}
    e=getenv("DM_NR_Z8_SLOTS");uint32_t n=e && *e?uint32_t(std::clamp(atoi(e),1024,32768)):8192u;
    e=getenv("DM_NR_Z8_MIN");nrz8g::min_tails=e && *e?uint32_t(std::clamp(atoi(e),1,64)):4u;
    e=getenv("DM_NR_Z8_DIGEST");nrz8g::dig_every=e && *e?uint32_t(std::clamp(atoi(e),1,4096)):8u;
    nrz8g::G &g=nrz8g::g;
    if(!nrz8g::tab.init(n) || !g.fix.init() || !g.W.init() || !(priv=new(std::nothrow) Priv[nrz8g::tab.n]) || (m==2 && !(nrz8g::dgr=new(std::nothrow) nrz8g::DgRing))){
        xbg_logf("【红】[nr] DM_NR_Z8=%d: no memory for %u templates and the dictionaries: off",m,n);return;}
    static_cast<State&>(g.S)=state;for(auto &x:g.S.shader)x=nullptr;memset(g.S.sprog,0,sizeof g.S.sprog);   // the copy = State as the backend starts it (no program bound)
    g.W.h=new(std::nothrow) nrz8g::Hnd;if(!g.W.h){xbg_logf("【红】[nr] DM_NR_Z8=%d: no memory: off",m);return;}
    g.wr.q=&nbmt::q;g.wr.ring=&nrz8g::ring;g.wr.min_tails=nrz8g::min_tails;g.ready=true;
    {const char *t=getenv("DM_NR_Z8_THREAD");int tm=t && *t?atoi(t):xbg_nr_z8_thread_default();
     if(tm<0 || tm>1){xbg_logf("【红】[nr] DM_NR_Z8_THREAD=%d: only 0 / 1: 0",tm);tm=0;}
     const char *o=getenv("DM_NB_OWN");
     if(tm && o && *o && atoi(o)!=0){xbg_logf("[nr] DM_NR_Z8_THREAD=1: DM_NB_OWN queues a frame's last submission under a number the walk cannot know: the submitting thread walks");tm=0;}
     if(tm){e=getenv("DM_NR_Z8_SPLIT");nrz8g::split0=e && *e?uint32_t(std::clamp(atoi(e),0,1024)):8u;
        if(!nrz8g::marks.init(65536)){xbg_logf("【红】[nr] DM_NR_Z8_THREAD=1: no memory for the marks: the submitting thread walks");tm=0;}
        else{pthread_t th;
            if(pthread_create(&th,nullptr,[](void*)->void*{nrz8g::walker_main();return nullptr;},nullptr)!=0){xbg_logf("【红】[nr] DM_NR_Z8_THREAD=1: the walker thread could not be started: the submitting thread walks");tm=0;}
            else pthread_detach(th);}}
     nrz8g::thread_mode=tm;
     {const char *v=getenv("DM_NR_Z8_VIEW");const int vm=v && *v?atoi(v):xbg_nr_z8_view_default();nrz8g::view_ids=tm && vm>=1 && vm<=2;}
     if(tm)xbg_logf("[nr] DM_NR_Z8_THREAD=1 (NR-ZERO Z8, the walk on its own thread \"dm-z8walk\"): the submitting thread posts the handles of a recording (under the same lock, before the job is queued); the walker folds, answers "
        "and writes ahead as the submitting thread did, and puts the words of each draw record into a ring of %u entries by the record's place in its submission - nothing is written into the command memory; the "
        "translation thread takes a record's words only when the walk of this submission wrote them for this record (ahead of the walk it resolves and records the draw itself) and waits for the walker at the end "
        "of a submission; the first run of a submission the translation thread is close behind is published after %u tail draws, then doubling",nrz8g::marks.n,nrz8g::split0);}
    nrz8g::boot=m;nrz8g::armed.store(1,std::memory_order_release);nvn_nr_z8_enable(1);   // 1: the walk folds; 2 (init): it also answers
    xbg_logf("[nr] DM_NR_Z8=%d (NR-ZERO Z8, read at init): every submission is walked before it is queued - its records folded into a copy of State; answers start when the lane's switches are read",m);}
bool init(int draw_mode){
    const int m=nrz8g::boot;if(!m)return false;
    const char *z5=getenv("DM_NR_Z5");
    if(nbmt::mode==0 || draw_mode!=1 || nrdesc::mode!=1){char b[160];snprintf(b,sizeof b,"needs DM_NB_MT (now %d), DM_NR_DRAW=1 (now %d) and DM_NR_DESC=1 (now %d)",nbmt::mode,draw_mode,nrdesc::mode);off(b);return false;}
    if(z5 && *z5 && atoi(z5)!=0){off("DM_NR_Z5 is set too (the two write the same words of a draw record)");return false;}
    if(nbord::mode>0 || nbkey::mode==1 || nbx::mode!=0){off("DM_NB_ORDER, DM_NB_RELOC=1 or DM_NB_MX is on (records are not executed in the order of the recording / State is set from a key frame)");return false;}
    nrz5t::R=nrz5r::Reader{};nrz5t::R.q=&nbmt::q;nrz5t::R.ring=&nrz8g::ring;nrz5t::held=false;nrz5t::r_have=false;nrz5t::s=nrz5t::Stats{};nrz5t::z8=true;
    if(nbmt::q.spare_max<256)nbmt::q.spare_max=256;
    mode=m;nrz5t::gmode=m;nrz8g::armed.store(2,std::memory_order_release);
    {auto part=[](const char *n,int d){const char *e=getenv(n);int v=e && *e?atoi(e):d;if(v<0 || v>2){xbg_logf("【红】[nr] %s=%d: only 0 / 1 / 2: 0",n,v);v=0;}return v;};
     imode=part("DM_NR_Z8_IMG",xbg_nr_z8_img_default());
     kmode=part("DM_NR_Z8_VAL",xbg_nr_z8_val_default());
     if(kmode && !nrz8g::thread_mode){xbg_logf("[nr] DM_NR_Z8_VAL=%d needs DM_NR_Z8_THREAD=1 (the packet count of a tail draw comes with its mark): off",kmode);kmode=0;}
     if(kmode)xbg_logf("[nr] DM_NR_Z8_VAL=%d (NR-ZERO Z8 values, %s): the verdict of a tail draw from the translation thread's own memory - the values the walk wrote from are the slot's template (this thread's own, same "
        "version) over State's uniform addresses and the record's arguments, compared with the values resolved at the moment; the draw's packet bytes come with its mark; the side chunk and the chunk's packets are not read",
        kmode,kmode==2?"verify - the old verdict runs and must find in the chunks what this thread named":"on");
     vmode=part("DM_NR_Z8_VIEW",xbg_nr_z8_view_default());
     if(vmode && !nrz8g::view_ids){xbg_logf("[nr] DM_NR_Z8_VIEW=%d needs DM_NR_Z8_THREAD=1 (the ranges of a submission are numbered by the walker thread): off",vmode);vmode=0;}
     if(vmode && nrzx::cmode){xbg_logf("[nr] DM_NR_Z8_VIEW=%d: DM_NR_Z3C is on - its table answers the lane's ranges: off",vmode);vmode=0;}
     if(vmode)xbg_logf("[nr] DM_NR_Z8_VIEW=%d (NR-ZERO Z8 views, %s): the walker numbers the distinct vertex / index ranges of a submission's answered draws; the lane's tail keeps the view of a range at the entry of its number "
        "(%d entries) under the words of DM_NR_Z3C (in place: chunk liveness epoch, the chunk's write version, slot serial, pending-writeback generation; static copy: snapshot epoch, NR-06's static epoch, "
        "pending-writeback generation) - a range bound again in the submission is answered without snapshot() and without a hash",vmode,vmode==2?"verify - snapshot() runs and must answer the same view":"on",int(VC_N));
     if(imode)xbg_logf("[nr] DM_NR_Z8_IMG=%d (NR-ZERO Z8 images, %s): an answered draw does not walk the images of its NR-08 entry while the entry's read word stands - set after a walk that left every image quiet or "
        "\"read with nothing to do\" (checked in this submission, in the layout sets read, no attachment of the open pass, not written since the barrier the read sees), under the image epoch, the submission, the slot "
        "serial, the open pass, and the count of write marks other than the marks draws give the attachments of the pass they continue; never while a clear is pending or a read hook is armed (tile policies)",
        imode,imode==2?"verify - the walk runs and must change nothing":"on");}
    xbg_logf("[nr] DM_NR_Z8=%d (NR-ZERO Z8: %s): the thread that submits walks its recording first - folds every state record, OP_X_NR_STATE and the binds of every draw record into a copy of State, "
        "gives the fixed part of the copy an id by content (5 groups), finds the draw's template by its site words and that id in a table of %u (%zu KB; the template names the texture / sampler handles, vertex ranges "
        "and uniform sizes the resolution read, with their values - all compared), writes the draw's packets into a spare chunk; the translation thread takes the plan, NR-08 entry and NR-09 entry it resolved for "
        "the template (while NR-08's generation, NR-09's generation, the object epoch and the pass definition stand), runs the steps of the moment and takes the packets when the values it resolves equal the written ones; "
        "a tail shorter than %u draws is not handed over%s",
        m,m==2?"verify - the translation thread resolves and records everything itself and compares":"apply",nrz8g::tab.n,size_t(nrz8g::tab.n)*sizeof(nrz8::Slot)/1024,nrz8g::min_tails,
        m==2?"; the copy's digest rides with 1 draw record in the number DM_NR_Z8_DIGEST gives":"");
    return true;}
// ---- the hook of draw(): a record the walk answered ----
enum{W_NONE,W_NO_TMPL,W_REPLACED,W_STALE,W_PASSDEF,W_GATE,W_N};
nrz3::Stats zs;   // the counters of nrz3::answered for the draws answered here
uint64_t calls=0;
// 0: not answered here (nothing was touched: the path without the answer runs from its first line); 1 / 2 / 3: as nrz3::draw.
int draw(Commands &c,Descriptors *&dp,Pass &p,int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances,uint32_t first_instance,bool pp,int draw_fast){NRCPU_SCOPE(NrZ3);
    const nbd_hdr &rh=nrz5t::r_h;
    if(!nrz5t::r_have || nrz5t::r_done)return 0;
    ++s.asked;
    const uint32_t ver=uint32_t(rh.cond_addr>>32);
    if(!ver || (rh.flags&(NBD_F_REN|NBD_F_COND)) || rh._pad2<=0 || uint32_t(rh._pad2-1)>=nrz8g::tab.n){++s.no_tmpl;return 0;}
    const Priv &pv=priv[rh._pad2-1];
    if(pv.ver!=ver){++s.replaced;return 0;}   // the slot was published again since the walk read it
    if(pv.stamp[0]!=nrdesc::tab_gen || pv.stamp[1]!=nrdraw::tab_gen() || pv.stamp[2]!=nbpipe::epoch || nrdraw::epoch!=nbpipe::epoch){++s.stale;return 0;}
    if(pp || !last_pass.valid || pv.passdef!=uint64_t(uintptr_t(last_pass.pass.definition))){++s.passdef;return 0;}
    nrdraw::Tm tm;tm.timed=!(++calls&63);tm.t0=tm.timed?timing_clock():0;tm.sp=tm.timed && (calls>>6&1);
    const nrd_tail &t=*nrdraw::cur;
    if(nrz3::gate(t,pp,draw_fast,false)){++s.gate;return 0;}
    nrdesc::Plan *pl=reinterpret_cast<nrdesc::Plan*>(uintptr_t(pv.plan));
    if(nrps::mode==1 && state.shader[0]->psk==1){++s.gate;return 0;}   // (never published; a shader set does not change its kind)
    for(unsigned i=0;i<5;i++)if(pv.sh[i]!=state.shader[i]){++s.shaders;return 0;}   // the shader objects the answer was resolved for are the ones State holds (program words -> shaders: the backend's table)
    uint32_t active_colors=0;for(int i=0;i<state.rt.n;i++)if(state.rt.color[i])active_colors|=1u<<i;
    uint64_t f[6];nrdesc::fixed_words(f);
    nrz_val v{};v.plan=pv.plan;v.did=pv.did;v.pent=pv.pent;v.frag=pv.frag;
    nrz3::Ans a;a.v=&v;a.pl=pl;a.frag=pv.frag!=0;a.fhit=true;a.psk=false;a.pp=false;a.z8=true;a.f=f;a.varw=nullptr;a.re=nrdraw::restart_eff_of(prim,indexed);a.active_colors=active_colors;a.t=&t;
    const uint64_t tn=tm.sp?timing_clock():0;a.tg=a.tf=a.tk=tn;
    const int r=nrz3::answered(zs,a,c,dp,p,prim,first,count,indexed,instances,first_instance,draw_fast,tm);
    if(r)++s.taken_path;
    // 1 answered draw in 64: the words the answer stands on, read back from State (the fold itself is checked by =2)
    if(r==1 && !(calls&63)){++s.spot;uint64_t w[nrz8::MAX_WORDS];uint32_t tv=0;
        if(nrz8g::tab.get(uint32_t(rh._pad2-1),w,&tv) && tv==ver){
            struct A{bool ub(unsigned st,unsigned sl,uint64_t *x,uint64_t *z) const{if(st>=6 || sl>=16)return false;*x=uint64_t(state.ub[st][sl].address);*z=uint64_t(state.ub[st][sl].size);return true;}
                     bool vb(unsigned i,uint64_t *x,uint64_t *z) const{if(i>=16)return false;*x=uint64_t(state.vb[i].address);*z=uint64_t(state.vb[i].size);return true;}
                     uint64_t tex(unsigned ns,unsigned sl) const{return ns<6 && sl<128?state.tex[ns][sl]:~0ull;}uint64_t samp(unsigned ns,unsigned sl) const{return ns<6 && sl<128?state.samp[ns][sl]:~0ull;}};
            nrz5_site site;nrz5_site_make(&site,t.key,rh.draw==NBD_DRAW_IDX?NRZ5S_IDX:NRZ5S_ARRAYS,rh.prim,rh.itype,rh.count,rh.base_vertex,rh.base_instance,rh.instances,rh.first,rh.ibuf,state.vb[0].address);
            nrz5_vals vv;uint32_t off[nrz8::MAX_DYN];const int m=nrz8::match(w,&site,uint32_t(w[0]>>32),rh.count,rh.instances,rh.first,rh.base_vertex,rh.base_instance,A{},&vv,off);
            if(m){++s.spot_bad;red("a draw was answered from a template whose compared words are not State's (%s): off",m==nrz8::M_SITE?"site":m==nrz8::M_TEX?"texture / sampler handles":m==nrz8::M_VB?"vertex ranges":m==nrz8::M_UBO?"uniform ranges":"form");
                mode=0;nrz8g::armed.store(1,std::memory_order_release);}}}
    return r;}
void tick(){   // every submission (nrdraw::tick) while the switch is on
    const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    using namespace nrz8g;
    uint64_t g[G_N];for(int i=0;i<G_N;i++){const uint64_t x=pub[i].load(std::memory_order_relaxed);g[i]=x-last[i];last[i]=x;}
    if(pub[G_BAD].load(std::memory_order_relaxed) && mode){red("the walk met a record it cannot follow (the copy is no longer State): no answers from here on");mode=0;}   // (the draws it marked before are still taken draw by draw: their values are compared)
    if(!g[G_RECORDS] && !s.asked)return;
    const double dr=g[G_DRAWS]?double(g[G_DRAWS]):1.,runs=g[G_RUNS]?double(g[G_RUNS]):1.,subs=g[G_SUBS]?double(g[G_SUBS]):1.;
    xbg_logf("[nr] z8 walk (DM_NR_Z8=%d): %llu submissions walked, %.0f records and %.0f draw records each; answered (template matched) %llu (%.1f%%); not: no key / UI / condition / QUADS %llu, through OP_CALL %llu, "
        "slot empty or being written %llu, another site %llu, another fix id %llu, texture / sampler handles differ %llu, vertex ranges differ %llu, uniform range / size %llu, form %llu; head draws %llu, "
        "tail draws written ahead %llu (%.1f%% of the draw records; %.0f bytes of packets and %.0f bytes of values each) in %llu runs (%.1f tail draws a run; published without chunks: %llu runs with %llu tail draws; "
        "no spare chunk %llu, ring full %llu, chunk full %llu); fix ids: %llu new, %llu group refreshes (%.2f a draw record), dictionary cleared %llu; "
        "walk %.0f ns a record = %.3f ms a submission; a draw record (fold of its binds, fix id, template, packets; 1 in 64) %.0f ns",
        boot,(unsigned long long)g[G_SUBS],double(g[G_RECORDS])/subs,double(g[G_DRAWS])/subs,(unsigned long long)g[G_VALUES],100.*double(g[G_VALUES])/dr,(unsigned long long)g[G_NO_KEY],(unsigned long long)g[G_CALLED],
        (unsigned long long)g[G_EMPTY],(unsigned long long)g[G_MISS+nrz8::M_SITE],(unsigned long long)g[G_MISS+nrz8::M_FIX],(unsigned long long)g[G_MISS+nrz8::M_TEX],(unsigned long long)g[G_MISS+nrz8::M_VB],
        (unsigned long long)g[G_MISS+nrz8::M_UBO],(unsigned long long)g[G_MISS+nrz8::M_FORM],(unsigned long long)g[G_HEADS],(unsigned long long)g[G_TAILS],100.*double(g[G_TAILS])/dr,
        g[G_TAILS]?double(g[G_TAIL_BYTES])/double(g[G_TAILS]):0.,g[G_TAILS]?double(g[G_SIDE_BYTES])/double(g[G_TAILS]):0.,(unsigned long long)g[G_RUNS],double(g[G_TAILS])/runs,
        (unsigned long long)g[G_SHORT_RUNS],(unsigned long long)g[G_SHORT_TAILS],(unsigned long long)g[G_NO_CHUNK],(unsigned long long)g[G_RING_FULL],(unsigned long long)g[G_CHUNK_FULL],
        (unsigned long long)g[G_FIX_NEW],(unsigned long long)g[G_FIX_REFRESH],double(g[G_FIX_REFRESH])/dr,(unsigned long long)g[G_FIX_CLEARS],
        g[G_RECORDS]?double(g[G_WALK_NS])/double(g[G_RECORDS]):0.,double(g[G_WALK_NS])/subs/1e6,g[G_DRAW_TIMED]?double(g[G_DRAW_NS])/double(g[G_DRAW_TIMED]):0.);
    if(thread_mode){   // [DM_NR_Z8_THREAD=1] who pays what: the submitting thread, the walker, the translation thread's wait
        const uint64_t pn=post_ns.load(std::memory_order_relaxed),pc=posts.load(std::memory_order_relaxed),cn=cpu_ns.load(std::memory_order_relaxed),jd=jobs_done.load(std::memory_order_relaxed),
            wkk=wakes.load(std::memory_order_relaxed),ps=passed.load(std::memory_order_relaxed);
        const uint64_t dp=pc-tl_posts,dj=jd-tl_jobs;
        xbg_logf("[nr] z8 thread (DM_NR_Z8_THREAD=1): submitting thread: %llu posts, %.0f ns a post (%llu woke the walker); walker thread dm-z8walk: %llu submissions walked (%llu passed over), %.3f ms thread CPU a submission, "
            "%llu runs split for a close translation thread (in %llu submissions it was close); translation thread: %llu draw records asked for the walk's words: there %llu (%.1f%%), not yet written %llu, walk of the "
            "submission not begun %llu, found further on %llu, counts differ %llu; waited for the walker at the end of %llu of %llu submissions, %.3f ms a wait",
            (unsigned long long)dp,dp?double(pn-tl_post)/double(dp):0.,(unsigned long long)(wkk-tl_wakes),(unsigned long long)dj,(unsigned long long)(ps-tl_passed),dj?double(cn-tl_cpu)/double(dj)/1e6:0.,
            (unsigned long long)g[G_SPLITS],(unsigned long long)g[G_CLOSE],(unsigned long long)ts.recs,(unsigned long long)(mr.got-mrl.got),ts.recs?100.*double(mr.got-mrl.got)/double(ts.recs):0.,
            (unsigned long long)(mr.not_there-mrl.not_there),(unsigned long long)(mr.no_base-mrl.no_base),(unsigned long long)(mr.resync-mrl.resync),(unsigned long long)(mr.desync-mrl.desync),
            (unsigned long long)ts.waits,(unsigned long long)ts.ends,ts.waits?double(ts.wait_ns)/double(ts.waits)/1e6:0.);
        if(mr.desync!=mrl.desync)red("the translation thread and the walker count the draw records of a submission differently (%llu records without their words)",(unsigned long long)(mr.desync-mrl.desync));
        tl_post=pn;tl_posts=pc;tl_cpu=cn;tl_jobs=jd;tl_wakes=wkk;tl_passed=ps;mrl=mr;ts=TStats{};}
    if(kmode){const nrz5r::RStats &kr=nrz5t::R.st;static uint64_t kl[4];
        xbg_logf("[nr] z8 values (DM_NR_Z8_VAL=%d): %llu tail draws asked, named from this thread's memory %llu (of them the values differ %llu); taken by that verdict %llu, runs ended because a later draw could not be named %llu; "
            "checked against the chunks %llu, bad %llu",kmode,(unsigned long long)ks.asked,(unsigned long long)ks.named,(unsigned long long)ks.differ,(unsigned long long)(kr.known-kl[0]),(unsigned long long)(kr.known_cut-kl[1]),
            (unsigned long long)(kr.k_cmp-kl[2]),(unsigned long long)(kr.k_bad-kl[3]));
        if(kr.k_bad!=kl[3])red("the values / packet bytes this thread named for a tail draw are not what the walker's chunks hold (%llu draws)",(unsigned long long)(kr.k_bad-kl[3]));
        kl[0]=kr.known;kl[1]=kr.known_cut;kl[2]=kr.k_cmp;kl[3]=kr.k_bad;ks=KStats{};}
    if(vmode){xbg_logf("[nr] z8 views (DM_NR_Z8_VIEW=%d): %llu vertex / index ranges of answered draws asked at the entry of their number, the entry stood %llu (%.1f%%; %s), stale %llu, stored in place %llu / static copy %llu, "
            "answered another way (not kept) %llu; ranges without a number %llu; checked %llu, bad %llu",vmode,(unsigned long long)vs.c_asked,(unsigned long long)(vmode==1?vs.c_hit:vs.c_checked),
            vs.c_asked?100.*double(vmode==1?vs.c_hit:vs.c_checked)/double(vs.c_asked):0.,vmode==1?"snapshot() not asked":"snapshot() asked and compared",(unsigned long long)vs.c_stale,(unsigned long long)vs.c_inplace,
            (unsigned long long)vs.c_static,(unsigned long long)vs.c_other,(unsigned long long)v_none,(unsigned long long)vs.c_checked,(unsigned long long)vs.c_bad);
        vs=nrzx::Stats{};v_none=0;}
    if(imode){xbg_logf("[nr] z8 images (DM_NR_Z8_IMG=%d): %llu walks of an answered draw asked, the read word stood %llu (%.1f%%; %s), a read hook or a pending clear %llu; set %llu, not set: the pass or the slot ended / a hook %llu, "
            "an image at its first use or in another layout %llu, an attachment of the open pass %llu, written since the barrier %llu, other %llu; checked %llu, bad %llu",imode,(unsigned long long)is.asked,(unsigned long long)is.stood,
            is.asked?100.*double(is.stood)/double(is.asked):0.,imode==1?"not walked":"walked and compared",(unsigned long long)is.hooks,(unsigned long long)is.set,(unsigned long long)is.no_set[0],(unsigned long long)is.no_set[1],
            (unsigned long long)is.no_set[2],(unsigned long long)is.no_set[3],(unsigned long long)is.no_set[4],(unsigned long long)is.checked,(unsigned long long)is.bad);
        is=IStats{};}
    const nrz5r::RStats &r=nrz5t::R.st;static nrz5r::RStats rl;auto d=[&](uint64_t nrz5r::RStats::*f){return r.*f-rl.*f;};
    const nrz5t::Stats &q=nrz5t::s;const double tails=q.tails?double(q.tails):1.;
    xbg_logf("[nr] z8, translation thread (DM_NR_Z8=%d): %llu engine draw records at the hook of draw(); took the walk's answer %llu (%.1f%%); not: the walk had no template %llu, the slot was published again %llu, "
        "a generation moved (NR-08 / NR-09 / objects) %llu, another pass definition or a pre-pass override %llu, a gate %llu, other shader objects %llu; records of another walk %llu, of a re-executed range %llu; "
        "at the lane's command section %llu draws (marked %llu), %s %llu (%.1f%%); runs met %llu, followed %llu: complete %llu, cut by: the bind cache did not hold the model %llu, values differ %llu "
        "(pipeline %llu, set %llu, offsets %llu, viewport %llu, scissor %llu, vertex %llu, index %llu, arguments %llu), chunk ends early %llu, a record did not come %llu, another command %llu; first tail draw not taken %llu; "
        "chunks %s %llu; runs gone %llu, never reached %llu; templates published %llu, not describable %llu (NR-08 did not serve %llu, a static uniform block %llu, too many rows / offsets %llu, "
        "no entry of its own / pattern / zero stream %llu, State changed by the prologue %llu, stages %llu, an offset above its address %llu); spot checks of an answered draw's words %llu, bad %llu",
        mode,(unsigned long long)s.asked,(unsigned long long)s.taken_path,s.asked?100.*double(s.taken_path)/double(s.asked):0.,(unsigned long long)s.no_tmpl,(unsigned long long)s.replaced,(unsigned long long)s.stale,
        (unsigned long long)s.passdef,(unsigned long long)s.gate,(unsigned long long)s.shaders,(unsigned long long)s.stamp,(unsigned long long)s.again,(unsigned long long)q.tails,(unsigned long long)q.marked,mode==2?"would take":"taken",
        (unsigned long long)q.taken,100.*double(q.taken)/tails,(unsigned long long)d(&nrz5r::RStats::runs),(unsigned long long)d(&nrz5r::RStats::started),(unsigned long long)d(&nrz5r::RStats::complete),
        (unsigned long long)(r.cut[nrz5r::CUT_CACHE]-rl.cut[nrz5r::CUT_CACHE]),(unsigned long long)(r.cut[nrz5r::CUT_VALUES]-rl.cut[nrz5r::CUT_VALUES]),
        (unsigned long long)(r.diff[1]-rl.diff[1]),(unsigned long long)(r.diff[2]-rl.diff[2]),(unsigned long long)(r.diff[3]-rl.diff[3]),(unsigned long long)(r.diff[4]-rl.diff[4]),(unsigned long long)(r.diff[5]-rl.diff[5]),
        (unsigned long long)(r.diff[6]-rl.diff[6]),(unsigned long long)(r.diff[7]-rl.diff[7]),(unsigned long long)(r.diff[8]-rl.diff[8]),
        (unsigned long long)(r.cut[nrz5r::CUT_CHUNK]-rl.cut[nrz5r::CUT_CHUNK]),(unsigned long long)(r.cut[nrz5r::CUT_RECORD]-rl.cut[nrz5r::CUT_RECORD]),(unsigned long long)(r.cut[nrz5r::CUT_WRITE]-rl.cut[nrz5r::CUT_WRITE]),
        (unsigned long long)d(&nrz5r::RStats::first_no),mode==2?"that would be handed over":"handed over",(unsigned long long)d(&nrz5r::RStats::handed),(unsigned long long)d(&nrz5r::RStats::gone),(unsigned long long)d(&nrz5r::RStats::unreached),
        (unsigned long long)s.published,(unsigned long long)s.pub_no,(unsigned long long)s.pub_why[1],(unsigned long long)s.pub_why[2],(unsigned long long)s.pub_why[3],(unsigned long long)s.pub_why[4],
        (unsigned long long)s.pub_why[5],(unsigned long long)s.pub_why[6],(unsigned long long)s.pub_why[7],(unsigned long long)s.spot,(unsigned long long)s.spot_bad);
    const nrdraw::Stats &z=nrdraw::z8sz;
    if(z.sp_n && zs.sp_n){const double n=double(z.sp_n),m=double(zs.sp_n);   // where an answered draw's time goes on the translation thread (as "[nr] z3 by part"; no key, no lookup)
        xbg_logf("[nr] z8 by part (NR-ZERO, %llu answered draws split, 1 in 128): %.0f ns from the hook to the end = entry checks %.0f + uniform blocks %.0f + images %.0f + set and the marks of the answer %.0f + pass marks %.0f + "
            "vertex / index views %.0f + viewport and checks %.0f + bookkeeping and the verdict (packets taken or recorded) %.0f + finish %.0f; %.0f ns an answered draw (1 in 64, unsplit); "
            "the prologue of draw() before the hook %.0f ns (1 in 64 engine draws)",(unsigned long long)z.sp_n,(z.sp_key+z.sp_vtx+z.sp_vp+z.sp_cmd+z.sp_tail)/n,zs.sp_look/m,zs.sp_blk/m,zs.sp_img/m,(zs.sp_desc-zs.sp_blk-zs.sp_img)/m,zs.sp_pass/m,
            z.sp_vtx/n,z.sp_vp/n,z.sp_cmd/n,z.sp_tail/n,z.timed?double(z.ns)/double(z.timed):0.,s.pre_n?double(s.pre_ns)/double(s.pre_n):0.);}
    else if(s.pre_n)xbg_logf("[nr] z8 by part (NR-ZERO): no answered draw was timed; the prologue of draw() before the hook %.0f ns (1 in 64 engine draws)",double(s.pre_ns)/double(s.pre_n));
    if(mode==2 || s.a_bad || s.dg_bad || s.sh_bad || g[G_FIX_STALE])
        xbg_logf("[nr] z8 check (DM_NR_Z8=2): State against the walk's copy: compared %llu, same %llu, bad %llu, program words bad %llu, digests never reached %llu (pushed %llu, ring full %llu), fix ids not the id of the "
            "copy's bytes %llu; answers of the walk against the path's own resolution: compared %llu, same %llu, a generation or the pass definition had moved %llu, bad %llu; draws =1 would take: compared %llu, same %llu, "
            "bad %llu, not compared (a hand-over inside the draw) %llu; state blocks REF without DEF on the walk %llu",(unsigned long long)s.dg_cmp,(unsigned long long)s.dg_same,(unsigned long long)s.dg_bad,(unsigned long long)s.sh_bad,
            (unsigned long long)s.dg_unreached,(unsigned long long)g[G_DG_PUSHED],(unsigned long long)g[G_DG_FULL],(unsigned long long)g[G_FIX_STALE],(unsigned long long)s.a_cmp,(unsigned long long)s.a_same,
            (unsigned long long)s.a_stale,(unsigned long long)s.a_bad,(unsigned long long)d(&nrz5r::RStats::v_cmp),(unsigned long long)d(&nrz5r::RStats::v_same),(unsigned long long)d(&nrz5r::RStats::v_bad),
            (unsigned long long)d(&nrz5r::RStats::v_skipped),(unsigned long long)g[G_REF_MISS]);
    rl=r;s=Stats{};zs=nrz3::Stats{};nrdraw::z8sz=nrdraw::Stats{};nrz5t::s=nrz5t::Stats{};}
}
#else
// ======== third part: the entry points of the NVN front (dm/gfx/nvn/hle_nvn.c nvnQueueSubmitCommands, x_submit) ========
// A recording is about to be queued for translation as submission `sub` (the front holds its Z8 lock: one walk at a time, in the
// queue's order). The handles are the game's; their records are in the command memory this thread wrote.
extern "C" void xbg_nr_z8_game_submit(const uint64_t *handles,int n,uint64_t sub){
    if(!nrz8g::armed.load(std::memory_order_acquire))return;
    if(nrz8g::thread_mode)nrz8g::post(handles,n,sub);else nrz8g::submit(handles,n,sub);}
// The translation thread executed the last record of submission `sub` (before the submission counts as executed).
extern "C" void xbg_nr_z8_exec_done(uint64_t sub){nrz8t::exec_done(sub);}
// The translation thread is about to execute submission `sub` (xbg_submit takes the number with the handles).
static thread_local uint64_t nrz8_next_sub=0;
extern "C" void xbg_nr_z8_exec_sub(uint64_t sub){nrz8_next_sub=sub;}
#endif
