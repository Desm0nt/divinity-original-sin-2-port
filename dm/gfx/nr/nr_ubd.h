// [DOS2] NR-UBD (DM_NR_UBD, default 0; docs/dos2/nr/NR-UBD.md): a BindUniformBuffer of a served direct engine draw whose (stage, slot)
// already holds exactly that range in the backend's table is not sent - the uniform-buffer analogue of NR-09R's NRDP_TEXF mirror
// (nr_direct.h nrd_mirror). Pure C, no engine / host / NVN / Vulkan type: used by the NVN layer (dm/gfx/nvn/nvn_nr_direct.inc, inside
// hle_nvn.c) and the host test dm/gfx/vulkan/engine/tests/nr_ubd_test.c (which runs it against a model of the backend).
//
// Why a bind may stay unsent (the argument of NRDP_TEXF, for state.ub):
//   The backend's uniform-buffer table state.ub[6][16] is written only by binds, in record order: exec_bind_buf (OP_BIND_UBO) and
//   nbd_apply (the binds of an OP_X_NB_DRAW, always applied before its draw). The post fusion (xbg_nb_post.inc) swaps two spare slots
//   and puts them back inside one draw; NR-SHCACHE's second execution of a range starts from the state saved at the range's start and
//   puts the end state back; DM_NB_ORDER / DM_NB_RELOC / DM_NB_MX execute parts of a recording out of order / from key frames (the
//   switch stays off with them, as TEXF). So for ONE recording of one command buffer, written by one thread: after the recording
//   recorded a bind of (address, size) to (stage, slot) and no later bind of that slot, the table holds (address, size) there whenever
//   the backend executes a later record of the recording - whatever ran before it, however often the handle is replayed, whatever
//   other handles ran in between in a submission. A bind of the same (address, size) to that slot then writes the value already there.
//   What else a bind does in the backend: ++buf_gen[stage] (a "the tables may have changed" counter; every reader caches a function of
//   the tables - driver_block's bank 0 copies state.sb / state.ub of compute, the NB-11 guard, the per-draw pool-index evaluation; the
//   descriptor memo keys uniform buffers by {address, size}, not by buf_gen; uniform buffer CONTENTS are never keyed by buf_gen: in-place
//   updates are tracked by memo_mod_gen / snapshot_invalidate, NR-06 reads the ring by address), and NB-12 notes the range for in-place
//   serving in this submission (the earlier bind of the same recording noted it already; a missing note only takes the copy path).
//   With equal tables every reader sees the same; the counters that did not move only keep more caches valid.
// Nothing is assumed across recordings (reset at BeginRecording), about another command buffer, after a nested command handle
// (CopyCommands / CallCommands execute foreign records in the middle of this one: forget), or after a bind by another thread into the
// tracked command buffer (order unknown: forget). Every UBO bind of the owner thread into the tracked command buffer - inside a draw
// bracket or not, BindUniformBuffer or BindUniformBuffers - updates the table; only a single bind inside a served direct draw
// (scope) that goes into the draw's record (adopt: inside the NB-09 bracket) is left out, the class DM_NR_UBD=2 checks on the backend.
#ifndef DM_NR_UBD_H
#define DM_NR_UBD_H
#include <stdint.h>
#include <string.h>

enum { NRUB_STAGES = 6, NRUB_SLOTS = 16 };   // the backend's limits (exec_bind_buf: stage < 6, index < 16; nbd_bind_ok)
typedef struct nrub_mirror {
    const void *cb, *owner;                  // the command buffer / the thread (any per-thread address) the table describes; NULL = none
    uint32_t epoch;                          // at[] == epoch: a[] / n[] is what this recording bound there
    uint32_t at[NRUB_STAGES][NRUB_SLOTS];
    uint64_t a[NRUB_STAGES][NRUB_SLOTS], n[NRUB_STAGES][NRUB_SLOTS];
} nrub_mirror;
static inline void nrub_reset(nrub_mirror *m) { if (!++m->epoch) { memset(m->at, 0, sizeof m->at); m->epoch = 1; } }
static inline void nrub_forget(nrub_mirror *m) { m->cb = NULL; m->owner = NULL; nrub_reset(m); }
// BeginRecording of cb: a new recording knows nothing.
static inline void nrub_begin(nrub_mirror *m, const void *cb) { if (cb == m->cb) nrub_reset(m); }
// CopyCommands / CallCommands into cb.
static inline void nrub_nested(nrub_mirror *m, const void *cb) { if (cb == m->cb) nrub_forget(m); }
static inline int nrub_ok(int stage, int slot) { return stage >= 0 && stage < NRUB_STAGES && slot >= 0 && slot < NRUB_SLOTS; }
// The mirror changes owner (adopt into another cb / thread) or must be forgotten (another thread into the tracked cb): the caller takes
// its lock for those (the owner's own binds touch only its table).
static inline int nrub_slow(const nrub_mirror *m, const void *cb, const void *me, int adopt) {
    return adopt ? (m->cb != cb || m->owner != me) : (m->cb == cb && m->owner != me);
}
// A bind call of `me` into cb: n ranges (a[i], sz[i]) from slot `first` (about to be recorded - or, when 1 is returned and the caller
// filters, not sent). adopt: the caller is the thread that records engine draws into cb right now (inside an engine draw bracket) - the
// table follows it. Returns 1 when n == 1, the table describes (cb, me) and holds exactly this range there; else 0. The table then holds
// the ranges of the call (a no-op when 1 was returned). *forgot = 1 when another thread's bind made the table forget.
static inline int nrub_bind(nrub_mirror *m, const void *cb, const void *me, int adopt, int stage, int first, int n, const uint64_t *a,
                            const uint64_t *sz, int *forgot) {
    if (adopt) { if (m->cb != cb || m->owner != me) { nrub_reset(m); m->cb = cb; m->owner = me; } }
    else if (cb != m->cb) return 0;                               // another command buffer: its records do not touch this recording
    else if (m->owner != me) { nrub_forget(m); if (forgot) *forgot = 1; return 0; }   // another thread writes into the tracked recording
    int same = 0;
    for (int i = 0; i < n; i++) {
        const int s = first + i;
        if (!nrub_ok(stage, s)) continue;                         // outside the table: never known, never filtered
        uint32_t *at = &m->at[stage][s]; uint64_t *va = &m->a[stage][s], *vn = &m->n[stage][s];
        if (n == 1 && *at == m->epoch && *va == a[0] && *vn == sz[0]) same = 1;
        *at = m->epoch; *va = a[i]; *vn = sz[i];
    }
    return same;
}
// What may stay unsent when nrub_bind said "the table holds it": a single bind recorded inside an engine draw bracket (adopt) while this
// thread serves a direct draw (scope) - it goes into that draw's OP_X_NR_DRAW record, where DM_NR_UBD=2 flags it for the backend.
static inline int nrub_filter(int same, int adopt, int scope, int n) { return same && adopt && scope && n == 1; }
#endif
