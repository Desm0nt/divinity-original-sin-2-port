// [DOS2] NR-07: pipelines of the native renderer (DM_NR_PIPE, default 0; docs/dos2/nr/NR-07.md). Pure C: no Vulkan, no game addresses,
// no host types. Two parts:
//   1. The warm-up scheduler (nrp_sched). The boot warm-up (xbg_game.cpp namespace warm: files/vk-pipeline-list.bin, one record per
//      pipeline the device built before = shader bundle keys + the exact create-info key) builds every record in list order on 2-3
//      background threads. NR-07 orders it by ENGINE EVENTS: every NVN program the game sets up (nvnProgramSetShaders, the NR-01 tap,
//      while the game loads an area - before any draw uses it) names its shader pack key; a record whose every stage key has been named
//      goes to the front of the queue ("hinted"). A draw that needs a record the warm-up is building waits for that build instead of
//      compiling a second copy, and a record the draw path builds itself is claimed so the warm-up never builds it again. Lazy mode
//      builds hinted records only (the slow-compile tilers: no work for areas the session never loads).
//   2. The direct pipeline table (nrp_tab) for the native draw (NR-09): pipeline[engine key id][host variant] with the NR-02 dense key id
//      (nr_key.h nr_key_note) as the row - a handful of word compares instead of hashing the draw state. Wired once a record carries the
//      key id (NR-09 OP_X_NR_DRAW); until then the per-draw memo of the old path is NB-13b DM_NB_PIPE (on by default). The host test
//      dm/gfx/vulkan/engine/tests/nr_pipe_test.c measures both on the PC.
// Not thread safe: the caller serialises every call on one nrp_sched / nrp_tab (the backend: the warm-up mutex). Tables are bounded
// and allocated once; nothing fails harder than "not scheduled".
#ifndef DM_NR_PIPE_H
#define DM_NR_PIPE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// ---- shader keys: the 16 hex digits of xbg_shader_program_key (the shader pack key); "<key>#at..." / "#vi..." variants count as
// their base key. 0 = not a key. ----
uint64_t nrp_key_parse(const char *s, size_t n);
// A warm record's stage key: nrp_key_parse, and for a fused post bundle of the backend (DM_NB_POST, xbg_nb_post.inc: "F1.<key>",
// "F2.<key>", "F1A.<key>", "F1AS.<key>", "F1AR.<key>" = 'F', '1' / '2', up to two upper-case letters, '.', the 16 hex digits of the
// HOST shader it replaces) the host's key: the fused shader is drawn in place of the host's fragment stage, so the program the game
// sets up with the host (EoCSight / CombineUI_NoFetch) names it. Hints stay plain keys (nrp_key_parse). *fused = 1 for such a stage.
uint64_t nrp_stage_key(const char *s, size_t n, int *fused);

// ---- 1. the warm-up scheduler ----
enum { NRP_STAGES = 5 };
enum { NRP_FREE, NRP_BUILDING, NRP_READY, NRP_FAILED, NRP_CLAIMED, NRP_TAKEN, NRP_STATES };   // record states
enum { NRP_LEAD_BUCKETS = 6 };   // hint -> need lead time: < 50 ms, < 250 ms, < 1 s, < 5 s, < 30 s, more
typedef struct nrp_rec {
    uint64_t key[NRP_STAGES];
    uint64_t t_hint, t_ready, t_need;   // ns; 0 = not yet
    uint8_t nkey, phase, queued, from_hint;   // phase: NRP_FREE ... (not "state": a macro in xbg_game.cpp)
    uint8_t needed, inflight_at_need, phase_at_need, pad;
} nrp_rec;
typedef struct nrp_kent { uint64_t key; uint32_t first, n; uint8_t hinted, pad[7]; } nrp_kent;   // records of one key: krec[first .. first+n)
typedef struct nrp_stats {
    uint64_t hints, hint_keys_known, hint_keys_new, hint_dup, queued;              // hint calls; keys with / without a record; repeats; records queued
    uint64_t built_hint, built_list, failed, claimed, waited, wait_ready, wait_timeout, taken;
    uint64_t need, need_ready, need_inflight, need_free, need_failed, need_claimed;   // draws that missed the ready set: state of their record
    uint64_t need_hinted, need_unhinted, lead[NRP_LEAD_BUCKETS];                  // ... hinted before (lead time) or never
    uint64_t took_hinted, took_unhinted, took_lead[NRP_LEAD_BUCKETS];             // ready pipelines taken: hinted before, or built in list order
    uint64_t dup_avoided;                                                         // list builds skipped because the draw path built it
} nrp_stats;
typedef struct nrp_sched {
    nrp_rec *rec; uint32_t nrec;
    nrp_kent *kent; uint32_t nkent, *krec, *kidx, kcap;   // kidx: open addressing key -> kent index + 1 (kcap = power of two)
    uint32_t *q, qhead, qtail;                           // FIFO of hinted records (each queued once: nrec slots)
    uint32_t cursor;                                     // list order: the next record not looked at
    int lazy;                                            // 1: nrp_next gives hinted records only
    int shadow;                                          // 1: hints are timed but queue nothing (list order: DM_NR_PIPE=2)
    uint32_t left;                                       // records not yet BUILDING / READY / FAILED / CLAIMED / TAKEN
    nrp_stats st;
} nrp_sched;
// keys: nrec x NRP_STAGES base keys (nrp_key_parse; 0 = no stage / not a key), nkeys[i] the stage count of record i. A record with a
// stage that is not a key can never be hinted (it is built in list order, or never when lazy). 0 = ok, -1 = no memory.
int nrp_init(nrp_sched *s, uint32_t nrec, const uint64_t *keys, const uint8_t *nkeys, int lazy);
void nrp_free(nrp_sched *s);
// The game set up a program with this shader key (now = ns): returns the records it queued (every stage of theirs now named).
uint32_t nrp_hint(nrp_sched *s, uint64_t key, uint64_t now);
// The next record to build (-> NRP_BUILDING): hinted records first (FIFO), then list order (not when lazy); -1 = nothing to do now.
// *from_hint = 1 when it came from the hint queue.
int nrp_next(nrp_sched *s, int *from_hint);
void nrp_done(nrp_sched *s, uint32_t i, int ok, uint64_t now);   // a build finished (NRP_READY / NRP_FAILED)
// A draw needs record i and its pipeline was not in the ready set: returns the state the record was in. claim = 1: a FREE record
// becomes NRP_CLAIMED (the draw path builds it; the warm-up skips it). The first need of a record is counted (lead time from the hint).
int nrp_need(nrp_sched *s, uint32_t i, uint64_t now, int claim);
void nrp_wait_end(nrp_sched *s, int ready);                      // a draw waited for an in-flight build: it was ready (1) or timed out
void nrp_took(nrp_sched *s, uint32_t i, uint64_t now);           // a draw took record i's ready pipeline (-> NRP_TAKEN)
int nrp_lead_bucket(uint64_t ns);
const char *nrp_lead_name(int bucket);                           // "<50ms" ...
const char *nrp_state_name(int st);
uint32_t nrp_count(const nrp_sched *s, int phase);               // records in a state (linear: for reports)

// ---- 2. the direct pipeline table: (engine key id, host variant) -> pipeline + vertex bindings ----
// The variant is what the host adds to the engine's state and the engine key does not name: render pass / its definition, pipeline
// layout, sample count and active colour mask, effective primitive restart, the alpha-test / vertex-retype module variants, the
// pre-pass depth override. The caller packs it into NRP_VAR_WORDS words (every word set).
enum { NRP_VAR_WORDS = 6, NRP_TAB_ROWS = 16385, NRP_TAB_MAX = 8192 };   // rows = NR_KEY_MAX + 1 (id 0 unused)
typedef struct nrp_var { uint64_t w[NRP_VAR_WORDS]; } nrp_var;
typedef struct nrp_tent {
    nrp_var v; uint64_t pipe;            // pipe 0 = dead (destroyed): a find misses, a put revives the entry
    uint32_t next, key;                  // next entry of the same row (index + 1, 0 = end)
    uint32_t stride[16]; uint16_t used, inst; int8_t zero; uint8_t pad[3];   // vertex bindings, as NB-13b's nbp_ent
} nrp_tent;
typedef struct nrp_tab { uint32_t *head; nrp_tent *e; uint32_t n; uint64_t hits, first_hits, misses, full, killed, clears; } nrp_tab;
int nrp_tab_init(nrp_tab *t);            // 0 = ok, -1 = no memory
void nrp_tab_free(nrp_tab *t);
void nrp_tab_clear(nrp_tab *t);
// The live entry of (key, variant), NULL = miss. The row's first entry is compared first (a row has 1 entry on ~all draws: the variant
// changes only with the pass / layout); a hit further down moves to the front.
const nrp_tent *nrp_tab_find(nrp_tab *t, uint32_t key, const nrp_var *v);
// The entry of (key, variant) to fill (existing - live or dead - or new). NULL = key out of range; a full table is cleared first.
nrp_tent *nrp_tab_put(nrp_tab *t, uint32_t key, const nrp_var *v);
void nrp_tab_kill(nrp_tab *t, uint64_t pipe);   // the pipeline was destroyed: its entries go dead

#ifdef __cplusplus
}
#endif
#endif
