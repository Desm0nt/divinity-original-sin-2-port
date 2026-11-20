// [DOS2] NR-NATIVE step E (docs/dos2/nr/NR-NATIVE-E.md): DM_NRN_EV - resources by engine events instead of page tracking, the backend's
// half. Included by xbg_game.cpp right before xbg_nr_buf.inc (its static copies ask here) and long before xbg_nr_shc.inc (the shadow
// cache's content versions ask here); snapshot_invalidate and the pool events feed it. The table, the journal, the coverage and the
// verdicts of the verify are in dm/gfx/nr/nr_nrn_ev.h (pure C, host tests tests/nr_nrn_ev_test.c / nr_nrn_ev_tsan_test.cpp).
//
// The host half (dm/host/hle_dos2_nrn_ev.inc) turns every place the engine writes a vertex / index buffer - creation, destroy, the CPU
// copies of opcodes 0x2f / 0x33, the immediate-mode scratch buffer - into an event of one journal by guest range, and keeps which
// ranges ARE engine buffers. The bytes the BACKEND writes into guest memory itself (a replayed nvnCommandBufferCopyBufferToBuffer -
// every dynamic update of the engine is one -, a GPU writeback, a pool that comes or goes) are journalled here, in stream order
// (wrote, from snapshot_invalidate). So for a range inside an engine buffer the journal's version moves at every write to its bytes,
// and the dirty-page tracker (mprotect, write faults, page generations) is not needed to know that they changed.
//
//   DM_NRN_EV  0  default (xbg_nrn_ev_default; safe-mode step 2 forces 0; the app sets nothing): mode stays 0 after the first ask, one
//                 load and a not-taken branch at each of the four places below.
//              2  verify: nothing behaves differently. Every ask of the shadow cache about a covered range (rv) is also answered by
//                 the events and by the tracker, and the two are compared with the previous ask of the range (nr_nrn_ev.h
//                 nrne_verify): "the tracker moved, the events did not" is decided by a hash of the bytes - the same bytes = a write
//                 to something else on a shared page; other bytes = BAD, a write no event announced (red line, first 16, counted by
//                 kind and class). A static copy of NR-06 the tracker demotes is compared the same way (its copy against the bytes).
//              1  apply: the shadow cache's version of a covered range IS the event epoch - no nvn_dirty_version, no protect of its
//                 pages (the word is the epoch for the draw that builds a layer and for the draw that hits it; an ask the events
//                 cannot answer - not covered, a writer inside the table - takes the tracker's word, which never equals an
//                 epoch); a static copy of NR-06 over a covered range is guarded by its epoch instead of write-protected pages.
//   DM_NRN_EV_CLASSES   which classes =1 takes: bit 0 static, 1 dynamic, 2 no-init (default 7). The scratch buffer is never taken.
//   DM_NRN_EV_HASH_MB   bytes the verify may hash per 10 s (default 256).
// Lines: "[nr] native events (backend) ..." every 10 s while asked; red lines "【红】[nr] native events: ...".
#include "../../nr/nr_nrn_ev.h"
extern "C" const void *dm_nrn_ev_table(void) __attribute__((weak));   // dm/host/hle_dos2_nrn_ev.inc: the table (NULL: the host's switch is off)
namespace nrnev{
int mode=-1;nrn_ev *T=nullptr;unsigned classes=7;uint64_t hash_budget=256ull<<20,hash_left=0,next_ns=0;unsigned reds=0;
enum{VM=8192};
nrne_vmemo *vm=nullptr;
struct Stats{uint64_t asks=0,not_cov=0,scratch=0,cov[NRNE_K_N][NRNE_C_N]{},taken=0,cls_off=0,v[NRNE_K_N][NRNE_C_N][NRNE_V_N]{},no_tr=0,hashed=0,wrote=0,wrote_bytes=0,
    st_copies=0,st_taken=0,st_stale=0,st_dem=0,st_dem_bad=0,st_dem_same=0,st_dem_event=0;} s;
struct StCopy{uint64_t hi=0,ep=0;};
std::unordered_map<uint64_t,StCopy> st;   // lo -> the epoch its static copy was made under
inline uint64_t mix(uint64_t h,uint64_t v){h^=v+0x9E3779B97F4A7C15ull+(h<<6)+(h>>2);h^=h>>31;h*=0xBF58476D1CE4E5B9ull;h^=h>>29;return h;}
void red(const char *fmt,...) __attribute__((format(printf,1,2)));
void red(const char *fmt,...){if(reds++>=16)return;char b[512];va_list a;va_start(a,fmt);vsnprintf(b,sizeof b,fmt,a);va_end(a);xbg_logf("【红】[nr] native events: %s",b);}
void init(){
    const char *e=getenv("DM_NRN_EV");int m=e && *e?atoi(e):xbg_nrn_ev_default();
    if(m<0 || m>2){xbg_logf("【红】[nr] native events: DM_NRN_EV=%d is not 0 / 1 / 2: off",m);m=0;}
    if(m){T=dm_nrn_ev_table?static_cast<nrn_ev*>(const_cast<void*>(dm_nrn_ev_table())):nullptr;
        if(!T){xbg_logf("【红】[nr] native events: DM_NRN_EV=%d but the host half has no table (DM_NR off, a hook did not go in, or no memory): off",m);m=0;}}
    if(m){const char *c=getenv("DM_NRN_EV_CLASSES");if(c && *c)classes=unsigned(atoi(c))&7u;
        const char *h=getenv("DM_NRN_EV_HASH_MB");if(h && *h){const long v=atol(h);hash_budget=uint64_t(v<0?0:v>65536?65536:v)<<20;}
        if(m==2){vm=static_cast<nrne_vmemo*>(calloc(VM,sizeof *vm));if(!vm){xbg_logf("【红】[nr] native events: no memory for the verify: off");m=0;}}}
    hash_left=hash_budget;mode=m;
    if(m)xbg_logf("[nr] native events (NR-NATIVE step E, backend): DM_NRN_EV=%d - %s; classes %#x (1 static, 2 dynamic, 4 no-init); the bytes the backend writes into guest memory "
        "are journalled in stream order",m,m==1?"the shadow cache's content version of a range inside an engine buffer is the event epoch (no page generation, no protect), a "
        "static copy over such a range is guarded by its epoch":"verify - the events' answer and the page tracker's are compared at every ask of a covered range; a difference is "
        "decided by a hash of the bytes; nothing behaves differently",classes);}
void tick();
// From snapshot_invalidate (the backend wrote [a, a + n) of guest memory, or saw the stream write it) and from the pool events.
void wrote(uint64_t a,uint64_t n){if(mode<0)init();if(mode<=0)return;nrn_ev_wrote(T,a,n);++s.wrote;s.wrote_bytes+=n;}
inline bool cls_on(int cls){return cls==NRNE_C_STATIC?(classes&1u)!=0:cls==NRNE_C_DYNAMIC?(classes&2u)!=0:cls==NRNE_C_NOINIT?(classes&4u)!=0:false;}
// The shadow cache's rv(), for a range the game's CPU may write: true = *vp took the events' word (the caller asks nothing else).
bool rv(uint64_t a,uint64_t n,uint64_t &vp){
    if(mode<0)init();
    if(mode<=0)return false;
    ++s.asks;if(!(s.asks&255))tick();
    int kind=0,cls=0;const uint64_t ep=nrn_ev_range_epoch(T,a,n,&kind,&cls);
    if(!ep){if(cls==NRNE_C_SCRATCH)++s.scratch;else ++s.not_cov;return false;}
    ++s.cov[kind][cls];
    if(mode==1){if(!cls_on(cls)){++s.cls_off;return false;}
        vp=mix(mix(vp,0x4e524e45564e5431ull),ep);++s.taken;return true;}   // the tag keeps an epoch apart from every (generation, pool number) of the tracker
    // verify: the tracker's answer now (lock-free, changes nothing), the bytes' hash when something has to be decided
    uint64_t g=0,inc=0;const void *p=reinterpret_cast<const void*>(uintptr_t(a));
    const bool tr=nvn_dirty_version && nvn_dirty_version(p,size_t(n),&g,&inc)!=0;
    if(!tr)++s.no_tr;
    nrne_vmemo &m=vm[uint32_t((a*0x9E3779B97F4A7C15ull^n*0xC2B2AE3D27D4EB4Full)>>51)&(VM-1)];
    const bool known=m.used && m.a==a && m.n==n && m.have_tr;
    uint64_t h=0;
    if(tr && (!known || m.g!=g || m.inc!=inc || !m.hash) && hash_left>=n){evguard::EngineRead er(p,size_t(n));h=nrne_hash(p,size_t(n));hash_left-=n;++s.hashed;}
    const int r=nrne_verify(&m,a,n,ep,tr,g,inc,h);
    ++s.v[kind][cls][r];
    if(r==NRNE_V_BAD)red("the bytes of a covered range changed and no event announced it - %s %s buffer, range %#llx+%llu (the tracker's generation moved to %llu, the event epoch "
        "stood, the hash of the bytes differs): the engine writes this class outside the hooked places",nrne_cls_name(kind,cls),kind==NRNE_K_VB?"vertex":"index",
        (unsigned long long)a,(unsigned long long)n,(unsigned long long)g);
    return false;}
// ---- NR-06's static copies (xbg_nr_buf.inc first_use) ----
// The copy of [lo, lo + bytes) is being made. 1 = apply and the range is covered: the caller does not write-protect it.
int st_copy(uint64_t lo,uint64_t bytes){
    if(mode<0)init();
    if(mode<=0)return 0;
    int kind=0,cls=0;const uint64_t ep=nrn_ev_range_epoch(T,lo,bytes,&kind,&cls);
    ++s.st_copies;
    if(!ep){st.erase(lo);return 0;}
    st[lo]=StCopy{lo+bytes,ep};
    if(mode==1 && cls_on(cls)){++s.st_taken;return 1;}
    return 0;}
// apply, a later use of the copy: true = an event touched its bytes (or the events cannot say): the copy is not to be used any more.
bool st_stale(uint64_t lo,uint64_t bytes){
    auto f=st.find(lo);if(f==st.end() || f->second.hi!=lo+bytes)return false;   // not a copy the events guard (the tracker does)
    uint64_t ep=0;for(int k=0;k<3 && !ep;k++)ep=nrn_ev_range_epoch(T,lo,bytes,nullptr,nullptr);
    if(ep==f->second.ep)return false;
    st.erase(f);++s.st_stale;return true;}
// verify, the tracker demotes a copy: was there an event? copy = the bytes of the copy (what the range held when it was made).
void st_demoted(uint64_t lo,uint64_t bytes,const uint8_t *copy){
    if(mode!=2)return;
    auto f=st.find(lo);if(f==st.end() || f->second.hi!=lo+bytes)return;
    ++s.st_dem;const uint64_t ep=nrn_ev_range_epoch(T,lo,bytes,nullptr,nullptr);
    if(ep!=f->second.ep){++s.st_dem_event;st.erase(f);return;}
    const void *p=reinterpret_cast<const void*>(uintptr_t(lo));bool same;{evguard::EngineRead er(p,size_t(bytes));same=!memcmp(copy,p,size_t(bytes));}
    if(same)++s.st_dem_same;
    else{++s.st_dem_bad;red("a static copy's range %#llx+%llu was written (the tracker saw it, its bytes differ from the copy) and no event announced it",(unsigned long long)lo,(unsigned long long)bytes);}
    st.erase(f);}
void tick(){
    const uint64_t now=timing_clock();
    if(!next_ns){next_ns=now+10000000000ull;return;}
    if(now<next_ns)return;
    next_ns=now+10000000000ull;hash_left=hash_budget;
    uint64_t covered=0;for(int k=0;k<NRNE_K_N;k++)for(int c=0;c<NRNE_C_N;c++)covered+=s.cov[k][c];
    xbg_logf("[nr] native events (backend, DM_NRN_EV=%d, last 10 s): %llu asks of the shadow cache about a CPU-writable range - inside an engine buffer %llu (%.1f%%: vertex static %llu "
        "dynamic %llu no-init %llu, index static %llu dynamic %llu no-init %llu), the scratch buffer %llu, not an engine buffer / a writer inside %llu; apply: the event epoch taken "
        "%llu, class switched off %llu; backend writes journalled %llu (%llu KB); static copies: made %llu, guarded by their epoch %llu, dropped by an event %llu",
        mode,(unsigned long long)s.asks,(unsigned long long)covered,s.asks?100.*double(covered)/double(s.asks):0.,
        (unsigned long long)s.cov[NRNE_K_VB][NRNE_C_STATIC],(unsigned long long)s.cov[NRNE_K_VB][NRNE_C_DYNAMIC],(unsigned long long)s.cov[NRNE_K_VB][NRNE_C_NOINIT],
        (unsigned long long)s.cov[NRNE_K_IB][NRNE_C_STATIC],(unsigned long long)s.cov[NRNE_K_IB][NRNE_C_DYNAMIC],(unsigned long long)s.cov[NRNE_K_IB][NRNE_C_NOINIT],
        (unsigned long long)s.scratch,(unsigned long long)s.not_cov,(unsigned long long)s.taken,(unsigned long long)s.cls_off,(unsigned long long)s.wrote,
        (unsigned long long)(s.wrote_bytes>>10),(unsigned long long)s.st_copies,(unsigned long long)s.st_taken,(unsigned long long)s.st_stale);
    if(mode==2){
        for(int k=0;k<NRNE_K_TEX;k++)for(int c=0;c<NRNE_C_SCRATCH;c++){const uint64_t *v=s.v[k][c];uint64_t t=0;for(int i=0;i<NRNE_V_N;i++)t+=v[i];if(!t)continue;
            xbg_logf("[nr] native events verify (%s %s buffers, last 10 s): %llu compared asks - same %llu, both moved %llu, only the events moved %llu, only the tracker moved: "
                "bytes the same (a neighbour on the page) %llu, not hashed (budget) %llu, BAD (bytes differ, no event) %llu; first asks / the tracker had no version %llu",
                nrne_cls_name(k,c),k==NRNE_K_VB?"vertex":"index",(unsigned long long)(t-v[NRNE_V_FIRST]),(unsigned long long)v[NRNE_V_SAME],(unsigned long long)v[NRNE_V_BOTH],
                (unsigned long long)v[NRNE_V_EVENT],(unsigned long long)v[NRNE_V_NEIGHBOUR],(unsigned long long)v[NRNE_V_UNHASHED],(unsigned long long)v[NRNE_V_BAD],
                (unsigned long long)v[NRNE_V_FIRST]);}
        xbg_logf("[nr] native events verify (totals, last 10 s): asks the tracker could not version (a page writable or busy) %llu; ranges hashed %llu; static copies the tracker "
            "demoted %llu - an event had moved too %llu, the bytes were the same %llu, BAD (bytes differ, no event) %llu; red lines %u",
            (unsigned long long)s.no_tr,(unsigned long long)s.hashed,(unsigned long long)s.st_dem,(unsigned long long)s.st_dem_event,(unsigned long long)s.st_dem_same,
            (unsigned long long)s.st_dem_bad,reds);}
    s=Stats{};}
}
