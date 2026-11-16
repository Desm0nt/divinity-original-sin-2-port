// [DOS2] NB-14: OP_X_NB_KEY in the backend (docs/dos2/NATIVE_BACKEND_PLAN.md NB-14; layout and front end dm/gfx/nvn/nvn_nb_key.h). Included
// at the end of xbg_nb.inc (inside xbg_game.cpp, after exec_bind_*; execute() is declared there). DM_NB_RELOC (read once, the front end reads
// the same switch): 0 off (a stray key is counted only), 1 set, 2 check.
//   frame(seq) (from nbchk::frame, every OP_X_NB_FRAME): base = a copy of `state` (+ missing_program) for this IR part.
//   exec (case OP_X_NB_KEY, right after the pass's OP_X_NB_PASS): the start state of the pass = base + the key's units, built by executing
//        the units' records with execute() itself on a copy (state is swapped: same cases, same checks), compared field by field with the
//        live state (the old path's state at this point). Fields: everything a record sets (render targets, programs, binds, pools, fixed
//        state, viewport / scissor, misc); not restart_eff (draw() derives it), count_samples / samples_passed (counter epochs: passes with
//        counters are not reordered, NB-16), nor the render-enable condition (condr, the same).
//   =2: the live state is put back (generations too): the old path runs unchanged. Lines "[nb] key check: pass ... differs: ..." (first 16)
//       and "[nb] key (backend ...)" every 10 s.
//   =1: the built state stays (restart_eff / counters kept from the live one); generations: back to the live values, then bumped for the
//       stages whose texture / buffer tables the key changed; differences are counted and the first 16 logged as "[nb] key set: ...".
//       A void key (walk lost) or one with lost units is not applied (counted): only complete keys move the state.
// NB-17a: start(p, n, depth, budget) = the =1 build for a pass executed out of order (returns false: incomplete key, nothing changed).
void execute(const uint8_t *pc,unsigned depth,uint64_t &budget);
namespace nbkey{
int mode=-1;   // DM_NB_RELOC
State base{},live{};bool base_missing=false,have=false;uint32_t base_seq=0;
enum{F_RT,F_PROG,F_MISSING,F_UBO,F_SSBO,F_VB,F_TEX,F_SAMP,F_IMG,F_POOL,F_VATTR,F_VSTREAM,F_BLEND,F_BCOLOR,F_COLOR,F_MASK,F_MS,F_AREF,F_POLY,
     F_BIAS,F_DS,F_STENCIL,F_VIEWPORT,F_SCISSOR,F_STRIDE,F_PATCH,F_BOUNDS,F_RESTART,F_CLAMP,F_DISCARD,NF};
const char *const fname[NF]={"rt","program","missing_program","ubo","ssbo","vb","tex","samp","img","pool","vattr","vstream","blend","blend_color",
    "color","chmask","multisample","alpha_ref","polygon","poly_offset","depth_stencil","stencil","viewport","scissor","copy_stride","patch_size",
    "depth_bounds","prim_restart","depth_clamp","rast_discard"};
struct Stats{uint64_t keys,bytes,stray,checked,same,differ,applied,changed,rebound,skipped_partial,nobase,lost,partial,malformed,badref,def,ref,timed,ns,field[NF];} s{};
unsigned told=0;uint64_t next_ns=0;
void read_mode(){const char *e=getenv("DM_NB_RELOC");mode=e && *e?atoi(e):0;if(mode!=1 && mode!=2)mode=0;
    if(mode)xbg_logf("[nb] DM_NB_RELOC=%d: the state at every OP_X_NB_FRAME is kept; at every OP_X_NB_KEY the pass's start state (that + the key) is %s (NB-14)",
        mode,mode==1?"set as the state":"compared with the state");}
void frame(uint32_t seq){if(mode<0)read_mode();if(!mode)return;memcpy(&base,&state,sizeof(State));base_missing=missing_program;have=true;base_seq=seq;}
// Field-by-field: c = built from the key, l = live. Fields of lost units are not compared. Up to a few names go to w.
unsigned compare(const State &c,bool cm,const State &l,bool lm,uint64_t lost,char *w,size_t cap){
    unsigned nd=0;int k=0;w[0]=0;
    auto cov=[&](unsigned u){return !(lost>>u&1);};
    auto bad=[&](int f,int i,int j,uint64_t a,uint64_t b){++nd;++s.field[f];if(k>int(cap)-96)return;
        k+=snprintf(w+k,cap-size_t(k)," %s",fname[f]);if(i>=0)k+=snprintf(w+k,cap-size_t(k),"[%d]",i);if(j>=0)k+=snprintf(w+k,cap-size_t(k),"[%d]",j);
        k+=snprintf(w+k,cap-size_t(k)," key %#llx live %#llx;",(unsigned long long)a,(unsigned long long)b);};
    auto u64=[](const void *p,size_t n){uint64_t v=0;memcpy(&v,p,std::min<size_t>(n,8));return v;};
    auto field=[&](int f,unsigned unit,const void *a,const void *b,size_t n){if(cov(unit) && memcmp(a,b,n))bad(f,-1,-1,u64(a,n),u64(b,n));};
    if(cov(NBK_U_RT)){const p_set_rt &a=c.rt,&b=l.rt;int at=-1;
        if(a.n!=b.n || a.depth!=b.depth || a.has_dview!=b.has_dview || (a.has_dview && memcmp(&a.dview,&b.dview,sizeof a.dview)))at=8;
        for(int i=0;i<8 && at<0;i++)if(a.color[i]!=b.color[i] || a.has_cview[i]!=b.has_cview[i] || (a.has_cview[i] && memcmp(&a.cview[i],&b.cview[i],sizeof(view_t))))at=i;
        if(at>=0)bad(F_RT,at,-1,at<8?(uint64_t)(uintptr_t)a.color[at]:(uint64_t)(uintptr_t)a.depth,at<8?(uint64_t)(uintptr_t)b.color[at]:(uint64_t)(uintptr_t)b.depth);}
    if(cov(NBK_U_PROG)){for(int i=0;i<6;i++)if(c.shader[i]!=l.shader[i]){bad(F_PROG,i,-1,(uint64_t)(uintptr_t)c.shader[i],(uint64_t)(uintptr_t)l.shader[i]);break;}
        if(cm!=lm)bad(F_MISSING,-1,-1,cm,lm);}
    for(int st=0;st<6;st++){
        if(cov(NBK_U_UBO+st))for(int i=0;i<16;i++)if(c.ub[st][i].address!=l.ub[st][i].address || c.ub[st][i].size!=l.ub[st][i].size){bad(F_UBO,st,i,c.ub[st][i].address,l.ub[st][i].address);break;}
        if(cov(NBK_U_SSBO+st))for(int i=0;i<16;i++)if(c.sb[st][i].address!=l.sb[st][i].address || c.sb[st][i].size!=l.sb[st][i].size){bad(F_SSBO,st,i,c.sb[st][i].address,l.sb[st][i].address);break;}
        if(cov(NBK_U_TEX+st))for(int i=0;i<128;i++)if(c.tex[st][i]!=l.tex[st][i]){bad(F_TEX,st,i,c.tex[st][i],l.tex[st][i]);break;}
        if(cov(NBK_U_SAMP+st))for(int i=0;i<128;i++)if(c.samp[st][i]!=l.samp[st][i]){bad(F_SAMP,st,i,c.samp[st][i],l.samp[st][i]);break;}
        if(cov(NBK_U_IMG+st))for(int i=0;i<16;i++)if(c.img[st][i]!=l.img[st][i]){bad(F_IMG,st,i,c.img[st][i],l.img[st][i]);break;}}
    if(cov(NBK_U_VB))for(int i=0;i<16;i++)if(c.vb[i].address!=l.vb[i].address || c.vb[i].size!=l.vb[i].size){bad(F_VB,i,-1,c.vb[i].address,l.vb[i].address);break;}
    if(cov(NBK_U_MISC)){
        for(int i=0;i<2;i++){const dpool_t &a=i?c.sampler_pool:c.texture_pool,&b=i?l.sampler_pool:l.texture_pool;
            if(a.pool!=b.pool || a.off!=b.off || a.n!=b.n)bad(F_POOL,i,-1,(uint64_t)(uintptr_t)a.pool,(uint64_t)(uintptr_t)b.pool);}
        if(c.copy_stride!=l.copy_stride || c.copy_image_stride!=l.copy_image_stride)bad(F_STRIDE,-1,-1,uint64_t(c.copy_stride),uint64_t(l.copy_stride));
        field(F_PATCH,NBK_U_MISC,&c.patch_size,&l.patch_size,sizeof c.patch_size);field(F_BOUNDS,NBK_U_MISC,&c.bounds,&l.bounds,sizeof c.bounds);
        field(F_RESTART,NBK_U_MISC,&c.restart,&l.restart,sizeof c.restart);field(F_CLAMP,NBK_U_MISC,&c.depth_clamp,&l.depth_clamp,sizeof c.depth_clamp);
        field(F_DISCARD,NBK_U_MISC,&c.rast_discard,&l.rast_discard,sizeof c.rast_discard);}
    if(cov(NBK_U_VFMT)){for(int i=0;i<16;i++)if(c.attrib[i]!=l.attrib[i]){bad(F_VATTR,i,-1,c.attrib[i],l.attrib[i]);break;}
        for(int i=0;i<16;i++)if(memcmp(&c.stream[i],&l.stream[i],sizeof(vs_st))){bad(F_VSTREAM,i,-1,u64(&c.stream[i],8),u64(&l.stream[i],8));break;}}
    if(cov(NBK_U_BLEND)){for(int i=0;i<8;i++)if(memcmp(&c.blend[i],&l.blend[i],sizeof(blend_st))){bad(F_BLEND,i,-1,u64(&c.blend[i],8),u64(&l.blend[i],8));break;}
        field(F_BCOLOR,NBK_U_BLEND,c.blend_color,l.blend_color,sizeof c.blend_color);}
    field(F_COLOR,NBK_U_COLOR,&c.color,&l.color,sizeof c.color);field(F_MASK,NBK_U_COLOR,&c.mask,&l.mask,sizeof c.mask);
    field(F_MS,NBK_U_COLOR,&c.ms,&l.ms,sizeof c.ms);field(F_AREF,NBK_U_COLOR,&c.alpha_ref,&l.alpha_ref,sizeof c.alpha_ref);
    field(F_POLY,NBK_U_POLY,&c.poly,&l.poly,sizeof c.poly);field(F_BIAS,NBK_U_POLY,c.bias,l.bias,sizeof c.bias);
    field(F_DS,NBK_U_DS,&c.ds,&l.ds,sizeof c.ds);
    if(cov(NBK_U_DS) && (memcmp(c.stencil_compare,l.stencil_compare,8) || memcmp(c.stencil_write,l.stencil_write,8) || memcmp(c.stencil_ref,l.stencil_ref,8)))
        bad(F_STENCIL,-1,-1,u64(c.stencil_ref,8),u64(l.stencil_ref,8));
    field(F_VIEWPORT,NBK_U_VPSC,&c.viewport,&l.viewport,sizeof c.viewport);field(F_SCISSOR,NBK_U_VPSC,&c.scissor,&l.scissor,sizeof c.scissor);
    return nd;}
// state = base + the key's units (execute() on each unit's records). The caller saved the live state.
void build(const nbk_view *v,int nu,unsigned depth,uint64_t &budget){
    memcpy(&state,&base,sizeof(State));missing_program=base_missing;
    for(int i=0;i<nu;i++){execute(v[i].content,depth+1,budget);++(v[i].u.how==NBK_H_DEF?s.def:s.ref);}}
// Parsed and usable against this part's base: units in v, or -1 (counted).
int open(const uint8_t *p,size_t n,nbk_hdr &h,nbk_view *v){
    const int nu=nbk_parse(p,n,&h,v);
    if(nu<0){++(nu==NBK_P_REF?s.badref:s.malformed);if(told<16){++told;xbg_logf("[nb] key: %s OP_X_NB_KEY (seq %u node %d) - ignored",nu==NBK_P_REF?"a REF to no DEF in":"malformed",h.seq,int(h.node));}return -1;}
    if(!have || h.seq!=base_seq){++s.nobase;return -1;}
    if(h.flags&NBK_F_LOST){++s.lost;return -1;}
    if(h.lost)++s.partial;
    return nu;}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    char f[480];int k=0;f[0]=0;for(int i=0;i<NF && k<int(sizeof f)-40;i++)if(s.field[i])k+=snprintf(f+k,sizeof f-size_t(k),"%s%s %llu",k?", ":"",fname[i],(unsigned long long)s.field[i]);
    xbg_logf("[nb] key (backend, DM_NB_RELOC=%d, last 10 s): %llu OP_X_NB_KEY (%.1f KB); %s %llu: same %llu, differ %llu (fields: %s); "
        "%s; not used: no FRAME base %llu, void (walk lost) %llu, malformed %llu, REF to no DEF %llu; keys with lost units %llu (their fields not compared%s); "
        "units executed DEF %llu REF %llu; %.1f us a key (1 in 16 timed)%s",
        mode,(unsigned long long)s.keys,s.bytes/1024.,mode==1?"set":"checked",(unsigned long long)s.checked,(unsigned long long)s.same,(unsigned long long)s.differ,k?f:"-",
        mode==1?"applied":"state put back after each check",(unsigned long long)s.nobase,(unsigned long long)s.lost,(unsigned long long)s.malformed,(unsigned long long)s.badref,
        (unsigned long long)s.partial,mode==1?", not applied":"",(unsigned long long)s.def,(unsigned long long)s.ref,s.timed?double(s.ns)/1e3/double(s.timed):0.,
        mode?"":" - DM_NB_RELOC is 0 in the backend: keys only counted");
    if(mode==1)xbg_logf("[nb] key set: %llu keys applied, %llu changed the state (%llu stages rebound), %llu with lost units skipped",
        (unsigned long long)s.applied,(unsigned long long)s.changed,(unsigned long long)s.rebound,(unsigned long long)s.skipped_partial);
    s=Stats{};}
void exec(const uint8_t *p,size_t n,unsigned depth,uint64_t &budget){
    if(mode<0)read_mode();
    ++s.keys;s.bytes+=n+8;
    if(!mode){++s.stray;tick();return;}
    nbk_hdr h{};nbk_view v[NBK_UNITS];const int nu=open(p,n,h,v);
    if(nu<0){tick();return;}
    if(mode==1 && h.lost){++s.skipped_partial;tick();return;}
    const bool timed=!(s.keys&15);const uint64_t t0=timed?timing_clock():0;
    memcpy(&live,&state,sizeof(State));const bool live_missing=missing_program;const unsigned saved_op=cur_op;
    uint64_t tg[6],bg[6];memcpy(tg,tex_gen,sizeof tg);memcpy(bg,buf_gen,sizeof bg);
    build(v,nu,depth,budget);
    char w[400];const unsigned nd=compare(state,missing_program,live,live_missing,h.lost,w,sizeof w);
    cur_op=saved_op;memcpy(tex_gen,tg,sizeof tg);memcpy(buf_gen,bg,sizeof bg);
    if(mode==2){memcpy(&state,&live,sizeof(State));missing_program=live_missing;nbpipe::reset();/*NB-13b: groups re-read after the direct restore*/++s.checked;}
    else{state.restart_eff=live.restart_eff;state.count_samples=live.count_samples;state.samples_passed=live.samples_passed;++s.applied;++s.checked;
        unsigned rb=0;for(int st=0;st<6;st++){
            if(memcmp(state.tex[st],live.tex[st],sizeof state.tex[st]) || memcmp(state.samp[st],live.samp[st],sizeof state.samp[st]) || memcmp(state.img[st],live.img[st],sizeof state.img[st])){++tex_gen[st];++rb;}
            if(memcmp(state.ub[st],live.ub[st],sizeof state.ub[st]) || memcmp(state.sb[st],live.sb[st],sizeof state.sb[st])){++buf_gen[st];++rb;}}
        if(nd || rb){++s.changed;s.rebound+=rb;nbdesc::P.valid=false;}}
    if(nd){++s.differ;if(told<16){++told;xbg_logf("[nb] key %s: pass seq=%u node=%d units=%u (lost %#llx, %u records walked) differs:%s",mode==1?"set":"check",h.seq,int(h.node),
        unsigned(h.nunit),(unsigned long long)h.lost,h.walked,w);}}
    else ++s.same;
    if(timed){++s.timed;s.ns+=timing_clock()-t0;}
    tick();}
[[maybe_unused]] bool start(const uint8_t *p,size_t n,unsigned depth,uint64_t &budget){   // NB-17a: a pass executed out of order starts from its key
    nbk_hdr h{};nbk_view v[NBK_UNITS];const int nu=open(p,n,h,v);
    if(nu<0 || h.lost)return false;
    memcpy(&live,&state,sizeof(State));const unsigned saved_op=cur_op;
    build(v,nu,depth,budget);cur_op=saved_op;
    state.restart_eff=live.restart_eff;state.count_samples=live.count_samples;state.samples_passed=live.samples_passed;
    for(int st=0;st<6;st++){++tex_gen[st];++buf_gen[st];}nbdesc::P.valid=false;
    return true;}
// NB-17a: would start() work for this key (the complete key of pass seq / node against the base kept at its FRAME)? Nothing counted or changed.
bool usable(const uint8_t *p,size_t n,uint32_t seq,int node){
    nbk_hdr h{};nbk_view v[NBK_UNITS];
    return mode>0 && nbk_parse(p,n,&h,v)>=0 && have && h.seq==base_seq && h.seq==seq && int(h.node)==node && !(h.flags&NBK_F_LOST) && !h.lost;}
}
