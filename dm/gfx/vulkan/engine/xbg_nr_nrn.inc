// [DOS2] NR-NATIVE (docs/dos2/nr/NR-NATIVE.md): DM_NRN / DM_NRN_<PASS> - the backend's half of "the draw as the engine holds it".
// Included by xbg_game.cpp ahead of xbg_nb.inc (nbdraw::apply hands the record over) and of xbg_nr_draw.inc (the lane's tail hands the
// values over). The card, the tail, the table and the classes are in dm/gfx/nr/nr_nrn.h (pure C, host test tests/nr_nrn_test.c).
//
// The front end (dm/host/hle_dos2_nrn.inc) writes, for every engine draw of a pass that is on, the CARD of the draw - one word of the
// engine renderer's own state cache: its mesh, material and target objects - and the pass into the draw record. Here, on the
// translation thread, the record is executed exactly as before; when the lane's tail has recorded the draw, the Vulkan values it
// used (pipeline, pipeline layout, descriptor set, number of dynamic offsets, vertex bindings with their buffers and folded offsets,
// index buffer / type / address, viewport, scissor) are noted under the card and compared with the last recording of the same card:
//   bad      a word that is a function of the engine's objects differs (pipeline, layout, bindings, index type, offset count), or
//            one card was seen in two passes: the card does not describe the draw - a red line (first 16), counted per pass;
//   moved    only words that follow a resource the backend may place elsewhere differ (descriptor set, vertex / index buffer or
//            offset): correct, and exactly what an epoch must cover before a draw can be replayed from its card;
//   view     only viewport / scissor differ (their packets are not in the card);
//   same     nothing differs: this draw could have been recorded from the card with the record's own arguments.
// Draws of a pass that carry a card but never reach the lane's tail (the old tail finished them, the shadow cache or a prologue
// dropped them, the UI area refused) are "records - noted" in the line.
//
//   DM_NRN_<PASS>  0  default (xbg_nrn_default; DM_NRN sets every pass without a switch of its own): one load in nbdraw::apply and one
//                     at the lane's tail; no table.
//                  2  verify, as above. Nothing is drawn differently.
//                  1  apply (step B, xbg_nr_nrn_exec.inc): a draw whose card has a live row is executed from the row; =2 also
//                     compares the row with what the path without it resolves.
//   DM_NRN_MAX     cards the table holds (16384; a power of two, 1024..262144; about 200 bytes each; full = cleared and learnt again)
// Needs DM_NR_DRAW=1|2 (the lane and the NR-09 tail). Safe-mode step 2 forces every DM_NRN switch to 0; the app sets none.
// Lines every 10 s: "[nr] native <PASS>: ..." for each pass that saw a card; at the start "[nr] native (NR-NATIVE, backend) ..."; red
// lines "【红】[nr] native: ...".
#include "../../nr/nr_nrn.h"
static_assert(NBD_F_NRN==NRN_F_FLAG && sizeof(VkViewport)==sizeof(((nrn_vals*)nullptr)->vp) && sizeof(VkRect2D)==sizeof(((nrn_vals*)nullptr)->sc),"nr_nrn.h holds viewport and scissor as bytes");
namespace nrn{
bool on=false,read_env=false,have=false,pgd=false,answering=false;uint8_t mode[NRN_P_N]{};   // pgd: the prologue changed this draw (not answered, not learnt, not noted); answering: its tail runs from a row
nrn_tab T{};bool tab_ok=false;
// step B (xbg_nr_nrn_exec.inc, after nrz3): the rows, the publication, the hook of draw() and what the lane's tail teaches
nrn_rows R{};bool rows_ok=false;nrn_pub PUB{};nrn_bstat b[NRN_P_N]{};
int draw(Commands &c,Descriptors *&dp,Pass &p,int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances,uint32_t first_instance,bool pp,int draw_fast,bool form);
void learn(const Pass &p);void lost();void exec_tick();
// part 2 (the image of a card, the short record): what a state record wrote, a record before its binds are applied, the image of a long record, release
void state_rec(const uint8_t *p,size_t n);void pre(const uint8_t *p,size_t n,const nbd_hdr &h,const uint8_t *binds);void image();void img_release();
nrn_tail cur{};uint64_t cur_ibuf=0;   // the record whose draw is running
nrn_pstat s[NRN_P_N]{};uint32_t last_seq[NRN_P_N]{};uint64_t next_ns=0,cards_seen=0;unsigned told=0;
const char *env(const char *k){return getenv(k);}
void init(int draw_mode){read_env=true;
    unsigned bad=0;uint8_t m[NRN_P_N];const unsigned any=nrn_env_modes(env,xbg_nrn_default(),m,&bad);
    if(bad)xbg_logf("【红】[nr] native: a DM_NRN switch is not 0 / 1 / 2 (mask %#x: bits 0..5 = SHADOW PREPASS GBUF LIGHT POST OTHER, bit 6 = DM_NRN): those are off",bad);
    if(!any)return;
    char names[96];names[0]=0;
    for(int p=0;p<NRN_P_N;p++){
        if(m[p]){if(names[0])strncat(names," ",sizeof names-strlen(names)-1);strncat(names,nrn_pass_name(p),sizeof names-strlen(names)-1);}}
    if(!names[0])return;
    if(!draw_mode){xbg_logf("【红】[nr] native: DM_NRN (%s) needs DM_NR_DRAW=1|2 (the lane's tail is where a recorded draw's values are in hand): off",names);return;}
    const char *e=getenv("DM_NRN_MAX");uint32_t cap=e && *e?uint32_t(atoi(e)):16384u;
    if(cap<1024 || cap>262144 || (cap&(cap-1)))cap=16384;
    if(nrn_tab_init(&T,cap)){xbg_logf("【红】[nr] native: no memory for the card table (%u entries): off",cap);return;}
    if(nrn_rows_init(&R,cap)){nrn_tab_free(&T);xbg_logf("【红】[nr] native: no memory for the rows of the cards (%u): off",cap);return;}
    rows_ok=true;tab_ok=true;memcpy(mode,m,sizeof mode);on=true;
    xbg_logf("[nr] native (NR-NATIVE, backend): passes %s (1 = apply: a draw whose card has a live row takes its plan, NR-08 entry and pipeline from the row; 2 = verify) - the Vulkan values the lane records for an engine draw are noted under the draw's card (the engine's "
        "mesh / material / target objects in one word, written by the front end) and compared with the card's last recording: bad = a function of the objects differs, "
        "moved = a resource was placed elsewhere, view = viewport / scissor; table %u cards of %zu bytes; nothing is drawn differently",names,cap,sizeof(nrn_ent));}
void release(){if(rows_ok){nrn_pub_bump(&PUB);img_release();nrn_rows_free(&R);rows_ok=false;}if(tab_ok){nrn_tab_free(&T);tab_ok=false;}on=false;have=false;memset(mode,0,sizeof mode);}
// From nbdraw::apply: the record whose draw() runs next (p / n: its payload, h: its header).
inline void record(const uint8_t *p,size_t n,const nbd_hdr &h){
    have=false;
    if(!(h.flags&NBD_F_NRN) || !(h.flags&NBD_F_NR))return;
    nrn_tail t;
    if(!nrn_tail_get(p,n,nrn_tail_off(&h),h.flags,&t) || !mode[t.pass])return;
    cur=t;cur_ibuf=h.ibuf;have=true;pgd=false;answering=false;
    if(!(t.rsv&NRN_T_SHORT))image();   // part 2: the State this long record left is the image of its card (compared when the card has one)
    if(t.rsv&NRN_T_READY)++b[t.pass].ready;
    ++s[t.pass].records;nrn_seq_note(&s[t.pass],&last_seq[t.pass],t.seq);}
inline uint32_t index_bytes(VkIndexType it){return it==VK_INDEX_TYPE_UINT16?2u:it==VK_INDEX_TYPE_UINT32?4u:1u;}
// From the lane's tail, after the commands of the draw were recorded (only while `have`).
void draw_end(VkPipeline pipe,uint16_t used,const VkBuffer *vb,const uint64_t *vbo,VkBuffer ib,uint64_t ibo,VkIndexType it,const VkViewport &vp,const VkRect2D &sc,
              VkPipelineLayout layout,VkDescriptorSet set,uint32_t noff,bool indexed,uint32_t first_index,uint32_t k,const Pass &p,bool own){
    have=false;
    if(pgd)return;
    if(own)learn(p);   // step B: the path without the row recorded this draw - its resolution is the card's row (=2: compared with the row first)
    if(mode[cur.pass]!=2)return;
    uint64_t b[16];for(unsigned i=0;i<16;i++)b[i]=(used>>i&1u)?uint64_t(vb[i]):0;
    nrn_vals v;
    nrn_vals_make(&v,uint64_t(pipe),uint64_t(layout),uint64_t(set),noff,used,b,vbo,k,indexed,uint64_t(ib),ibo,uint32_t(it),cur_ibuf,first_index,index_bytes(it),&vp,&sc);
    unsigned d=0;const uint64_t clears=T.clears;
    const int r=nrn_tab_note(&T,cur.card,cur.pass,&v,&d);
    if(T.clears!=clears && told<16){++told;xbg_logf("【红】[nr] native: the card table is full (%u cards) and starts again - raise DM_NRN_MAX; the shares of this window are low by that",T.cap);}
    nrn_pstat_note(&s[cur.pass],r,d);
    if(r==2 && (d&(NRN_D_BAD|NRN_D_PASS)) && told<16){++told;
        char w[160];w[0]=0;for(int i=0;i<NRN_D_BITS;i++)if(d>>i&1u){if(w[0])strncat(w,", ",sizeof w-strlen(w)-1);strncat(w,nrn_diff_name(i),sizeof w-strlen(w)-1);}
        xbg_logf("【红】[nr] native: two draws of one card differ in what the engine's objects must decide - %s (pass %s, card %016llx, frame %llu, vs=%s ps=%s): the card "
            "does not describe this draw",w,nrn_pass_name(cur.pass),(unsigned long long)cur.card,(unsigned long long)stats.presents.load(),
            state.shader[0]?state.shader[0]->key.c_str():"-",state.shader[4]?state.shader[4]->key.c_str():"-");}}
void tick(){   // every submission (nrdraw::tick)
    const uint64_t now=timing_clock();
    if(!next_ns){next_ns=now+10000000000ull;return;}
    if(now<next_ns)return;
    next_ns=now+10000000000ull;
    exec_tick();
    for(int p=0;p<NRN_P_N;p++){const nrn_pstat &x=s[p];
        if(!x.records)continue;
        const double n=x.noted?double(x.noted):1.;
        if(mode[p]!=2){s[p]=nrn_pstat{};continue;}
        xbg_logf("[nr] native %s (DM_NRN_%s=2, last 10 s): %llu draw records with a card, %llu recorded by the lane and noted (%.1f%%); of those: the card's first recording %llu, "
            "same as its last %llu (%.1f%% - a replay from the card alone), moved only %llu (%.1f%%), viewport / scissor only %llu, BAD %llu; differences by word: pipeline %llu, "
            "set layout %llu, vertex bindings %llu, index type %llu, offset count %llu | descriptor set %llu, vertex buffer %llu, vertex offset / fold %llu, index buffer %llu, "
            "index address %llu | viewport %llu, scissor %llu | pass %llu; sequence gaps %llu; table %u of %u cards, cleared %llu times",
            nrn_pass_name(p),nrn_pass_name(p),(unsigned long long)x.records,(unsigned long long)x.noted,100.*double(x.noted)/double(x.records),(unsigned long long)x.first,
            (unsigned long long)x.same,100.*double(x.same)/n,(unsigned long long)x.moved_only,100.*double(x.moved_only)/n,(unsigned long long)x.view_only,(unsigned long long)x.bad,
            (unsigned long long)x.diff[0],(unsigned long long)x.diff[1],(unsigned long long)x.diff[2],(unsigned long long)x.diff[3],(unsigned long long)x.diff[4],
            (unsigned long long)x.diff[5],(unsigned long long)x.diff[6],(unsigned long long)x.diff[7],(unsigned long long)x.diff[8],(unsigned long long)x.diff[9],
            (unsigned long long)x.diff[10],(unsigned long long)x.diff[11],(unsigned long long)x.diff[12],(unsigned long long)x.seq_gaps,T.n,T.cap,(unsigned long long)T.clears);
        s[p]=nrn_pstat{};}}
}
