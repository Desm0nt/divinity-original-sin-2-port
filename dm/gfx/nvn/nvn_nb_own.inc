// [DOS2] NB-21: the frame job in the NVN tap (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-21 findings"; layout, build, the backend's compare and
// the hand-off rule in dm/gfx/nb/nb_own.h). Included at the end of nvn_nb_tap.inc (inside hle_nvn.c, after nvn_nb_order.inc). Behind
// g_nbw_on (DM_NB_OWN 1 / 2, set by hle_dos2_nb.c at install); it never writes a record: the command stream is the same at 0 / 1 / 2.
//   nbt_sync: nbw_frame() after the FRAME record (the recording that holds it), nbw_pass(node) after a PASS record (its address and
//   recording); nbk_pass: nbw_key(node, payload) after an OP_X_NB_KEY (its address).
//   nvn_nb_own_build (from the front end's plan step, after nvn_nb_tap_patch / nvn_nb_order_patch, before the submit is queued): a job
//   from the pool = the part, the IR skeleton, the segments in stream order, the execution order (the order tail's when DM_NB_ORDER
//   wrote a valid one). nbw_take (nvnQueueSubmitCommands, same thread, right after): the job of this submit, or none.
//   nbw_release (translation thread, after the submission was executed): the pool slot is free again.
// The queue part (XJ_OWN, the open job, the attached present) is in hle_nvn.c next to x_run / x_push. Cost at 1 / 2: three stores a pass, one pass
// over the nodes and 32 bytes a segment at each submission of the IR.
#ifdef DM_GAME_DOS2
#include "../nb/nb_own.h"
enum { NBWS_BUILT, NBWS_SEGS, NBWS_KEYS, NBWS_SPLIT, NBWS_ORDER, NBWS_MERGED, NBWS_NOFREE, NBWS_NOFRAME, NBWS_LOST, NBWS_NOPLAN, NBWS_TAKEN, NBWS_STALE, NBWS_KINDS };
enum { NBW_POOL = 8 };   // jobs alive at once: one a queued submission of the IR (the game is paced by the present, 1 - 2 in flight)
static struct { _Atomic int busy; nbw_job j; } g_nbw_pool[NBW_POOL];
static nbw_src g_nbw_src[NB_MAX_NODES];      // render thread only (the tap's thread)
static int g_nbw_hi; static uint64_t g_nbw_frec_rec;
static __thread nbw_job *t_nbw_ready;        // built for the submit this thread is about to queue
static uint64_t g_nbws[NBWS_KINDS];

static void nbw_frame(void) {
    const cmd_t *o = (const cmd_t *)g_nbt.cb;
    if (g_nbw_hi) memset(g_nbw_src, 0, sizeof g_nbw_src[0] * (size_t)g_nbw_hi);
    g_nbw_hi = 0; g_nbw_frec_rec = o ? (uint64_t)(uintptr_t)o->begin : 0;
}
static void nbw_pass(int node) {
    const cmd_t *o = (const cmd_t *)g_nbt.cb;
    if (node < 0 || node >= NB_MAX_NODES || !g_nbt.prec[node]) return;
    g_nbw_src[node].pass = (uint64_t)(uintptr_t)((const uint8_t *)g_nbt.prec[node] - 8);
    g_nbw_src[node].key = 0; g_nbw_src[node].rec = o ? (uint64_t)(uintptr_t)o->begin : 0;
    if (node >= g_nbw_hi) g_nbw_hi = node + 1;
}
static void nbw_key(int node, const uint8_t *payload) {
    if (node >= 0 && node < g_nbw_hi && g_nbw_src[node].pass) g_nbw_src[node].key = (uint64_t)(uintptr_t)(payload - 8);
}
void nvn_nb_own_enable(int mode) { g_nbw_on = mode; }
void nvn_nb_own_build(const nb_frame *f, int planned, uint32_t part) {
    t_nbw_ready = NULL;
    if (!g_nbw_on) return;
    if (!g_nbt.frec) { g_nbws[NBWS_NOFRAME]++; return; }
    int k = 0;
    for (; k < NBW_POOL; k++) { int z = 0; if (atomic_compare_exchange_strong(&g_nbw_pool[k].busy, &z, 1)) break; }
    if (k == NBW_POOL) { g_nbws[NBWS_NOFREE]++; return; }
    nbw_job *j = &g_nbw_pool[k].j;
    const int nord = g_nbo_on ? g_nbo.last : 0;
    const int ns = nbw_build(j, f, g_nbw_src, (uint64_t)(uintptr_t)((const uint8_t *)g_nbt.frec - 8), g_nbw_frec_rec, g_nbt.handle, g_nbt.seq, part,
                             planned, g_nbt.bytes, nord > 0 ? g_nbo_ent : NULL, nord);
    if (ns < 0) { atomic_store(&g_nbw_pool[k].busy, 0); g_nbws[-ns == NBW_B_LOST ? NBWS_LOST : NBWS_NOFRAME]++; return; }
    g_nbws[NBWS_BUILT]++; g_nbws[NBWS_SEGS] += (uint64_t)ns;
    if (j->flags & NBW_F_KEYS) g_nbws[NBWS_KEYS]++;
    if (j->flags & NBW_F_SPLIT) g_nbws[NBWS_SPLIT]++;
    if (j->flags & NBW_F_ORDER) g_nbws[NBWS_ORDER]++;
    if (j->flags & NBW_F_MERGED) g_nbws[NBWS_MERGED]++;
    if (!planned) g_nbws[NBWS_NOPLAN]++;
    t_nbw_ready = j;
}
static void nbw_release(const void *job) {
    for (int k = 0; k < NBW_POOL; k++) if ((const void *)&g_nbw_pool[k].j == job) { atomic_store(&g_nbw_pool[k].busy, 0); return; }
}
// The job built for the submit of these handles (same thread, right after the front end planned it), or NULL.
static const nbw_job *nbw_take(const uint64_t *h, int n) {
    nbw_job *j = t_nbw_ready;
    if (!j) return NULL;
    t_nbw_ready = NULL;
    for (int i = 0; i < n; i++) if (h[i] == j->last) { g_nbws[NBWS_TAKEN]++; return j; }
    g_nbws[NBWS_STALE]++; nbw_release(j); return NULL;
}
#endif
