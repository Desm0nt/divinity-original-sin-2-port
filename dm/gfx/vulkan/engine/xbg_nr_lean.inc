// [DOS2] NR-13 / NR-26: DM_NR_LEAN in the backend (docs/dos2/nr/NR-13.md; the switch, parts and rules in dm/gfx/nr/nr_lean.h, host test
// tests/nr_lean_test.c). Included by xbg_game.cpp right before xbg_nb_hit.inc (it needs nrbuf::ring_mode, xbg_logf, timing_clock).
// Hooks, one line / one token each:
//   nbdraw::apply       skip_notes(): NRL_UBO - the record's uniform ranges are not noted for NB-12 (nbubo::note) while NR-06 serves the
//                       constant ring. A range NR-06 refuses then finds no note in nbubo::fast and takes the usual snapshot path.
//                       record(): the census of NB-11 for an OP_X_NR_DRAW (did nb_desc_pre run, was a candidate left).
//                       apply_ns(): the timed records of nbdraw (1 in 64) per A/B phase.
//   uniform_snapshot    ubo_off(): NRL_UBO - NB-12 is not asked (nbubo::fast, a table lookup per uniform block NR-06 did not answer) while
//                       NR-06 serves the ring: with no notes it can only say "not a ring range".
//   nbubo::fast         ubo_read(): a note was found (= read by its only consumer) - at DM_NR_LEAN=2 with the ring on this is the number
//                       of ranges 1 would send to the copy path instead.
//   nbstate::exec       state_block(): OP_X_NB_STATE blocks executed and the bytes of state records they replay (the NRL_STATE part lives in
//                       the front end, dm/host/hle_dos2_nb.c: at 1 no block arrives, the plain records run through execute() as before NB-13a).
//   nbdraw::tick        tick(): the line "[nr] lean (backend ...)" every 10 s.
//   the state reset     on(): DM_NR_LEAN is read once (with nrdraw::reset).
// DM_NR_LEAN=0 (default): mode stays 0 after the first read; every hook is one load and a not-taken branch; nothing else runs.
// The A/B phase (DM_NR_LEAN_AB) is set by the front end at EndFrame (xbg_nr_lean_phase): the backend runs a frame or two behind, so the
// phase of a record is approximate at the edges (N >= 100 frames makes that noise).
#include "../../nr/nr_lean.h"
namespace nrlean{
int mode=-1;unsigned parts=0,ab=0;
std::atomic<int> phase{0};   // lean is on now (mode 1: always, or the front end's A/B phase)
struct Stats{uint64_t records=0,nr_records=0,skip_records=0,fast_skip=0,ubo_reads=0,ubo_direct=0,desc_pre=0,desc_cand=0,
    blocks=0,block_bytes=0,ap_n[2]{},ap_ns[2]{},subs=0;} s;
uint64_t next_ns=0;
void init(){nr_lean_cfg c;const unsigned bad=nr_lean_parse(getenv("DM_NR_LEAN"),getenv("DM_NR_LEAN_PARTS"),getenv("DM_NR_LEAN_AB"),&c);
    mode=c.mode;parts=c.parts;ab=c.ab;phase.store(mode==1 && !ab,std::memory_order_relaxed);
    if(bad)xbg_logf("【红】[nr] DM_NR_LEAN: a value out of range was ignored (mask %#x; DM_NR_LEAN 0 / 1 / 2, DM_NR_LEAN_PARTS a mask of %#x, DM_NR_LEAN_AB 0..100000)",bad,unsigned(NRL_ALL));
    if(mode)xbg_logf("[nr] DM_NR_LEAN=%d parts %#x (NR-13 / NR-26, backend): NB-12 ring notes of every OP_X_NB_DRAW %s while NR-06 serves the constant ring (a range it refuses "
        "then takes the usual snapshot path); counted: notes read by nbubo::fast, nb_desc_pre runs / candidates for OP_X_NR_DRAW records, OP_X_NB_STATE blocks replayed%s",
        mode,parts,mode==1 && (parts&NRL_UBO)?"are not made":mode==2?"are made and counted":"are made (part off)",ab?"; A/B phases from the front end":"");}
inline bool on(){if(__builtin_expect(mode<0,0))init();return mode>0;}
// nbdraw::apply, before nbubo::note: true = do not note this record's uniform ranges.
inline bool skip_notes(){if(!on())return false;++s.records;
    const bool part=(parts&NRL_UBO)!=0,ring=nrbuf::ring_mode>0;
    if(mode==2){if(nr_lean_ubo_skip(part,ring))++s.skip_records;return false;}   // counted, made
    const bool sk=nr_lean_ubo_skip(part && phase.load(std::memory_order_relaxed),ring);if(sk)++s.skip_records;
    return sk;}
// uniform_snapshot, before nbubo::fast (only reached when NR-06 did not answer: a block outside the ring, or one it refused): true = do
// not ask NB-12 - no notes are made while NR-06 serves the ring, so the lookup could only find a note of this submission made before the
// ring came up; without it the range takes the usual snapshot path, exactly as a range NB-12 calls "not a ring range".
inline bool ubo_off(){if(__builtin_expect(mode<=0,1))return false;
    const bool part=(parts&NRL_UBO)!=0,ring=nrbuf::ring_mode>0;
    if(mode==2){if(nr_lean_ubo_skip(part,ring))++s.fast_skip;return false;}   // counted, asked
    const bool sk=nr_lean_ubo_skip(part && phase.load(std::memory_order_relaxed),ring);if(sk)++s.fast_skip;
    return sk;}
// nbdraw::apply, after the NB-11 block: nr = the record has the NR-09 tail; pre = nb_desc_pre ran (P was valid); cand = a candidate is left.
inline void record(bool nr,bool pre,bool cand){if(!nr)return;++s.nr_records;if(pre)++s.desc_pre;if(cand)++s.desc_cand;}
inline void apply_ns(uint64_t dt){const int p=phase.load(std::memory_order_relaxed)!=0;++s.ap_n[p];s.ap_ns[p]+=dt;}
// nbubo::fast found a note for the range (direct = it served the range in place).
inline void ubo_read(bool direct){++s.ubo_reads;if(direct)++s.ubo_direct;}
inline void state_block(uint32_t bytes){++s.blocks;s.block_bytes+=bytes;}
void tick(){if(mode<=0)return;++s.subs;const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns || !s.records)return;next_ns=now+10000000000ull;
    const double r=double(s.records);
    xbg_logf("[nr] lean (backend, DM_NR_LEAN=%d parts %#x, NR-06 ring %s): %llu OP_X_NB_DRAW records in %llu submissions; NB-12 ring notes: %s for %llu of them, "
        "nbubo::fast lookups %s %llu (uniform blocks NR-06 did not answer; the '[nb] ubo' line has their ns); notes READ by nbubo::fast %llu (served in place by NB-12 %llu%s); NB-11 on %llu OP_X_NR_DRAW records: nb_desc_pre ran %llu, "
        "candidates left %llu (NR-08 drops the previous-draw snapshot when it serves, so both stay near 0 without a skip); OP_X_NB_STATE: %llu blocks executed, %.1f KB of state "
        "records replayed (%.0f bytes a draw record; at DM_NR_LEAN=1 the front end sends none - see its line); apply %.0f ns a record lean (%llu timed) / %.0f full (%llu timed)",
        mode,parts,nrbuf::ring_mode==0?"off":nrbuf::ring_mode==1?"in place":"mirror",(unsigned long long)s.records,(unsigned long long)s.subs,
        mode==1?"not made":"made, 1 would not make them",(unsigned long long)s.skip_records,mode==1?"skipped":"1 would skip",(unsigned long long)s.fast_skip,
        (unsigned long long)s.ubo_reads,(unsigned long long)s.ubo_direct,
        mode==2 && nrbuf::ring_mode>0?" - what 1 would send to the copy path":"",(unsigned long long)s.nr_records,(unsigned long long)s.desc_pre,(unsigned long long)s.desc_cand,
        (unsigned long long)s.blocks,s.block_bytes/1024.,double(s.block_bytes)/r,s.ap_n[1]?double(s.ap_ns[1])/double(s.ap_n[1]):0.,(unsigned long long)s.ap_n[1],
        s.ap_n[0]?double(s.ap_ns[0])/double(s.ap_n[0]):0.,(unsigned long long)s.ap_n[0]);
    s=Stats{};}
}
// The front end's A/B phase (dm/host/hle_dos2_nb.c nrl_phase, the game's render thread): 1 = lean on from now.
extern "C" void xbg_nr_lean_phase(int on){nrlean::phase.store(on!=0,std::memory_order_relaxed);}
