// [DOS2] NR-13 / NR-26: DM_NR_LEAN - stop producing what nothing consumes any more (docs/dos2/nr/NR-13.md). Pure C (C and C++; no NVN,
// no Vulkan, no engine): the switch, the parts, the eligibility rules and the A/B phase. Used by the front end (dm/host/hle_dos2_nb.c),
// the backend (dm/gfx/vulkan/engine/xbg_nr_lean.inc) and the host test dm/gfx/vulkan/engine/tests/nr_lean_test.c.
//
// What the analysis found (NR-13.md §1): the NR-09 lane READS the backend state (shaders, render targets, vertex ranges, viewport,
// scissor, the six state-group ids) and the binds of its draw record (NR-08 builds the set from them), so neither the state records nor
// the OP_X_NB_DRAW of a served draw can be dropped: they are the lane's input, not a shadow of it. What has no consumer left:
//   NRL_STATE  the NB-13a state capture (shadow, per-key last-write order, content dictionary, DEF / REF blocks). Its block ids were made
//              for NB-14 keys / NB-17 order (DM_NB_RELOC / DM_NB_ORDER, out of the defaults since NR-15); the backend only replays the
//              blocks' records, and NB-13b / NR-09 key on the backend's own group ids read from `State`. Lean: the state packets are not
//              taken - their calls are the plain NVN state records again (the NB-09 / NB-10 stream, DM_NB_OPS without the state bits).
//              Fallback rule: trivial - every draw, served or not, sees exactly the state the plain records set, in call order.
//   NRL_UBO    NB-12's ring notes (nbubo::note, one table insert per uniform bind of every OP_X_NB_DRAW) while NR-06 serves the constant
//              ring (nrbuf::ring_mode > 0: uniform_snapshot asks nrbuf first). Lean: no notes. Fallback rule: a range NR-06 refuses then
//              finds no note in nbubo::fast = "not a ring range" = the usual snapshot path (copy) - the conservative answer by design.
// Counted only (nothing to skip, NR-13.md §1.3): NB-11's nb_desc_pre / post for an OP_X_NR_DRAW (NR-08's Scope already drops P when it
// serves, so the next record's pre does not run), tex-dirty / hash schedule of NR-05's static images and the snapshot front for ring
// blocks (both already off inside NR-05 / NR-06 at their =1).
//
//   DM_NR_LEAN        0  default: nothing here runs, every stream and every table is byte for byte as before.
//                     1  skip the parts of DM_NR_LEAN_PARTS that are eligible (a part that is not eligible stays on, with a line).
//                     2  produce everything as at 0 and count: what 1 would not produce (calls, bytes) and every read of it by a
//                        consumer ("read after a would-skip" must stay 0 for a part to be skippable without a slower fallback).
//   DM_NR_LEAN_PARTS  bit mask NRL_*; default NRL_ALL.
//   DM_NR_LEAN_AB     N > 0 (only with DM_NR_LEAN=1): in-process ABBA - N frames lean off, 2N on, N off, ... ; the lines give the game
//                     thread's recording time and the backend's cost per phase from ONE run (no DVFS drift between runs).
#ifndef DM_NR_LEAN_H
#define DM_NR_LEAN_H
#include <stdint.h>
#include <stdlib.h>

enum { NRL_STATE = 1, NRL_UBO = 2, NRL_ALL = 3 };
typedef struct { int mode; unsigned parts, ab; } nr_lean_cfg;
enum { NRL_BAD_MODE = 1, NRL_BAD_PARTS = 2, NRL_BAD_AB = 4 };
// The three switches as text (NULL / "" = default). Returns a mask of NRL_BAD_* (the bad one falls back to its default / off).
static inline unsigned nr_lean_parse(const char *m, const char *parts, const char *ab, nr_lean_cfg *c) {
    unsigned bad = 0;
    c->mode = 0; c->parts = NRL_ALL; c->ab = 0;
    if (m && *m) { const long v = strtol(m, NULL, 0); if (v < 0 || v > 2) bad |= NRL_BAD_MODE; else c->mode = (int)v; }
    if (parts && *parts) { const long v = strtol(parts, NULL, 0); if (v < 0 || (v & ~(long)NRL_ALL)) bad |= NRL_BAD_PARTS; else c->parts = (unsigned)v; }
    if (ab && *ab) { const long v = strtol(ab, NULL, 0); if (v < 0 || v > 100000) bad |= NRL_BAD_AB; else c->ab = (unsigned)v; }
    if (c->mode != 1) c->ab = 0;   // the A/B alternates "1" and "0": meaningless at 0 / 2
    if (!c->mode) c->parts = 0;
    return bad;
}
// NRL_STATE eligibility: who else reads the NB-13a blocks. 0 = nobody (the capture may go); else the first consumer found.
enum { NRL_ST_OK = 0, NRL_ST_NOBITS, NRL_ST_RELOC, NRL_ST_ORDER };
static inline int nr_lean_state_why(int state_bits, int reloc, int order) {
    if (!state_bits) return NRL_ST_NOBITS;   // DM_NB_OPS has no state bit: there is no capture to skip
    if (reloc) return NRL_ST_RELOC;          // NB-14 key frames walk the stream and resolve REF blocks through the NB-13a dictionary
    if (order) return NRL_ST_ORDER;          // NB-17 registers the DEF blocks of a part before it reorders
    return NRL_ST_OK;
}
static inline const char *nr_lean_state_name(int w) {
    static const char *const n[] = {"no consumer", "DM_NB_OPS has no state bit (nothing is captured)", "DM_NB_RELOC reads the block ids (NB-14 keys)",
                                    "DM_NB_ORDER registers the DEF blocks (NB-17)"};
    return w >= 0 && w < 4 ? n[w] : "?";
}
// ABBA phase of frame f with period n: off for n frames, on for 2n, off for n, ... 1 = lean is on in this frame. n == 0: always on.
static inline int nr_lean_ab_on(uint64_t f, unsigned n) {
    if (!n) return 1;
    const uint64_t q = (f / n) & 3u;
    return q == 1 || q == 2;
}
// NRL_UBO: skip the ring notes of a record? live = the part is on in this phase; ring = NR-06 serves the constant ring (nrbuf::ring_mode
// > 0, read at the record). Without the ring path NB-12 is the only in-place path: the notes stay.
static inline int nr_lean_ubo_skip(int live, int ring) { return live && ring; }
#endif
