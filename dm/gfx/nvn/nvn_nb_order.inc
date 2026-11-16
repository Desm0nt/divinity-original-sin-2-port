// [DOS2] NB-17a / NB-17b: the order tail of OP_X_NB_FRAME in the NVN tap (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-17a findings"; layout and
// the node order -> segment order step in dm/gfx/nb/nb_order.h). Included at the end of nvn_nb_tap.inc (inside hle_nvn.c). Three lines in
// nbt_sync, all behind g_nbo_on (DM_NB_ORDER 1 / 2 / 3, set by hle_dos2_nb.c at install): the FRAME record is written with room for the
// tail (nbo_frame_size: entries for the passes of the previous parts + a quarter + 16, 64 .. NB_MAX_NODES), nbo_frame() notes it, nbo_pass()
// notes a PASS written into another recording than its FRAME (the backend moves segments of one handle only). nvn_nb_order_patch (from the
// front end's plan step, after nvn_nb_tap_patch, before the job is queued) fills the tail: the execution order of the passes with the
// address of each PASS record; for an order that is not the stream order the PASS headers get the planner's barrier-before flags of that
// order. At 0 nothing is reserved or written: the stream is byte for byte the old one. Cost: zeroing the tail (16 bytes a pass) + one walk
// over the nodes a part.
#ifdef DM_GAME_DOS2
#include "../nb/nb_order.h"
enum { NBOS_PARTS, NBOS_VALID, NBOS_MERGED, NBOS_GLUE, NBOS_NOPLAN, NBOS_OVER, NBOS_LOST, NBOS_SPLIT, NBOS_SEGS, NBOS_CONT, NBOS_BYTES, NBOS_MOVED, NBOS_KINDS };
_Static_assert(NBOS_KINDS == NBOS_N, "nvn_nb_tap.h NBOS_N");
_Static_assert(sizeof(p_nb_frame) <= NBO_TAIL_OFF && sizeof(nbo_hdr) == 24 && sizeof(nbo_ent) == 16, "nb_order.h tail layout");
static struct { uint8_t *payload; uint32_t bytes; const uint8_t *begin; int split, want, last; } g_nbo;   // render thread only; last: entries of g_nbo_ent the last patch wrote as a valid tail (0 = none; NB-21 reads it)
static uint64_t g_nbos[NBOS_KINDS];
static nbo_ent g_nbo_ent[NBO_MAX_SEG];

static uint32_t nbo_frame_size(void) {
    int cap = g_nbo.want + g_nbo.want / 4 + 16;
    if (cap < 64) cap = 64;
    if (cap > NBO_MAX_SEG) cap = NBO_MAX_SEG;
    return (uint32_t)nbo_frame_bytes(cap);
}
static void nbo_frame(p_nb_frame *fr, uint32_t bytes) {
    const cmd_t *o = (const cmd_t *)g_nbt.cb;
    g_nbo.payload = (uint8_t *)fr; g_nbo.bytes = bytes; g_nbo.begin = o ? (const uint8_t *)o->begin : NULL; g_nbo.split = 0;
    nbo_init(g_nbo.payload, bytes, g_nbt.seq, g_nbo_on);
}
static void nbo_pass(void) {
    const cmd_t *o = (const cmd_t *)g_nbt.cb;
    if (!o || (const uint8_t *)o->begin != g_nbo.begin || g_nbt.handle || (const uint8_t *)g_nbt.frec != g_nbo.payload) g_nbo.split = 1;
}
void nvn_nb_order_enable(int mode) { g_nbo_on = mode; }
void nvn_nb_order_stats(uint64_t out[NBOS_KINDS]) { memcpy(out, g_nbos, sizeof g_nbos); memset(g_nbos, 0, sizeof g_nbos); }   // render thread (EndFrame)
// m NULL = the stream order. planned 0: the planner failed. tracked: m comes from a frame with every hazard tracked.
void nvn_nb_order_patch(const nb_frame *f, const nb_merge_out *m, int planned, int tracked) {
    uint8_t *pl = g_nbo.payload;
    g_nbo.last = 0;
    if (!g_nbo_on || !pl || (const uint8_t *)g_nbt.frec != pl) return;
    g_nbo.payload = NULL;
    g_nbos[NBOS_PARTS]++; g_nbos[NBOS_BYTES] += g_nbo.bytes - (uint32_t)sizeof(p_nb_frame);
    uint8_t have[NB_MAX_NODES]; int npass = 0;
    for (int i = 0; i < f->nnode && i < NB_MAX_NODES; i++) { have[i] = g_nbt.prec[i] != NULL; npass += f->node[i].kind == NB_NODE_PASS; }
    g_nbo.want = npass > g_nbo.want ? npass : g_nbo.want - (g_nbo.want > npass);
    int why = NBO_W_OK;
    if (!planned) why = NBO_W_NOPLAN;
    else if (g_nbt.lost || f->overflow) why = NBO_W_LOST;
    else if (g_nbo.split) why = NBO_W_SPLIT;
    nbo_hdr h; memcpy(&h, pl + NBO_TAIL_OFF, sizeof h);
    int merged = 0, glue = 0;
    const int ns = why ? -why : nbo_build(f, have, m, g_nbo_ent, h.cap, &merged, &glue);
    if (ns < 0) {
        why = -ns; nbo_fail(pl, why);
        g_nbos[why == NBO_W_NOPLAN ? NBOS_NOPLAN : why == NBO_W_OVER ? NBOS_OVER : why == NBO_W_SPLIT ? NBOS_SPLIT : NBOS_LOST]++;
        return;
    }
    int nc = 0, moved = 0;
    for (int k = 0; k < ns; k++) {
        p_nb_pass *r = g_nbt.prec[g_nbo_ent[k].node];
        g_nbo_ent[k].addr = (uint64_t)(uintptr_t)((uint8_t *)r - 8);
        if (merged) r->flags = (uint8_t)((r->flags & ~NB_NP_PREBAR) | ((g_nbo_ent[k].flags & NBO_E_PREBAR) ? NB_NP_PREBAR : 0));
        nc += (g_nbo_ent[k].flags & NBO_E_CONT) != 0;
        if (k && g_nbo_ent[k].node < g_nbo_ent[k - 1].node) moved++;
    }
    nbo_fill(pl, g_nbo_ent, ns, f->nnode, (merged ? NBO_F_MERGED : 0) | (tracked ? NBO_F_TRACKED : 0) | (glue ? NBO_F_GLUE : 0));
    g_nbo.last = ns;
    g_nbos[NBOS_VALID]++; g_nbos[NBOS_SEGS] += (uint64_t)ns; g_nbos[NBOS_CONT] += (uint64_t)nc; g_nbos[NBOS_MOVED] += (uint64_t)moved;
    if (merged) g_nbos[NBOS_MERGED]++;
    if (glue) g_nbos[NBOS_GLUE]++;
}
#endif
