// [DOS2] VK-NATIVE [rt2 R7] DM_VK_NATIVE_SORT (vkn_sort.h has the rule, the modes and why the reorder is exact). Recording thread only.
// No game addresses.
#include "vkn_sort.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkn_shader.h"
#include "vkn_state.h"

int vkn_query_idle(void) __attribute__((weak));   // vkn_query.c: no bracket / predicate can touch the next draw

enum { MAXDYN = 64, VBN = 4, WIN = 1024, DYNPOOL = WIN * 16 };
typedef struct vst {
    VkPipeline pipe; VkPipelineLayout lay; VkDescriptorSet set; uint32_t ndyn, dyn0;   // dyn0: offset of the dynamic offsets (pool / own array)
    VkBuffer vb[VBN]; VkDeviceSize vo[VBN]; VkBuffer ib; VkDeviceSize io; VkIndexType it;
    VkViewport vp; VkRect2D sc;
    uint8_t ok;                          // bits: 1 pipeline, 2 set, 4 index buffer, 8 viewport, 16 scissor
    uint8_t vok;                         // bit b: vertex binding b known
} vst;
enum { OK_PIPE = 1, OK_SET = 2, OK_IB = 4, OK_VP = 8, OK_SC = 16 };
typedef struct pkt { vst s; uint8_t idx, draw; uint32_t count, inst, first, finst; int32_t voff; } pkt;   // draw 0: binds of a draw that ended before its command

int vkn_sort_on, vkn_sort_cap; uint32_t vkn_sort_n;
static int g_mode;
static vst g_r, g_b;                     // the command buffer's real state / what the draw path believes is bound
static uint32_t g_rdyn[MAXDYN], g_bdyn[MAXDYN];   // their dynamic offsets
static pkt *g_p; static uint32_t *g_pool; static uint32_t g_npool;
static uint16_t *g_ord;                  // emission order
static uint64_t g_last_begins = ~0ull;
static vkn_sort_stats g_s;
static int g_alloc_fail;
static uint64_t g_cmds;                  // commands this module recorded (the restore's count)
static uint8_t g_gi[WIN];                // a packet's pipeline group

void vkn_sort_mode_set(int mode) {
    g_mode = mode == 1 || mode == 2 ? mode : 0;
    if (g_mode && !g_p) {
        g_p = (pkt *)malloc(sizeof(pkt) * WIN); g_pool = (uint32_t *)malloc(sizeof(uint32_t) * DYNPOOL); g_ord = (uint16_t *)malloc(sizeof(uint16_t) * WIN);
        if (!g_p || !g_pool || !g_ord) { free(g_p); free(g_pool); free(g_ord); g_p = NULL; g_pool = NULL; g_ord = NULL; g_mode = 0; g_alloc_fail = 1; }
    }
    vkn_sort_on = g_mode != 0; vkn_sort_cap = 0; vkn_sort_n = 0; g_npool = 0;
    memset(&g_r, 0, sizeof g_r); memset(&g_b, 0, sizeof g_b);
    vkn_logf("sort: DM_VK_NATIVE_SORT=%d (%s)%s", g_mode, g_mode == 1 ? "depth-only LESS / LEQUAL runs grouped by pipeline" : g_mode == 2 ? "verify: runs captured and counted, recorded in order" : "off",
             g_alloc_fail ? " - no memory for the window: off" : "");
}
int vkn_sort_mode(void) { return g_mode; }

// ---- emission against the real state ----
static void put(VkCommandBuffer cb, const vst *w, const uint32_t *wd, int count_set) {   // record what of w differs from g_r; g_r = w (known parts)
    if ((w->ok & OK_PIPE) && (!(g_r.ok & OK_PIPE) || g_r.pipe != w->pipe)) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, w->pipe); g_r.pipe = w->pipe; g_r.ok |= OK_PIPE; g_s.pipe_emit += (uint64_t)count_set; g_cmds++; }
    if ((w->ok & OK_SET) && (!(g_r.ok & OK_SET) || g_r.lay != w->lay || g_r.set != w->set || g_r.ndyn != w->ndyn || memcmp(g_rdyn, wd, w->ndyn * sizeof *wd))) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, w->lay, 0, 1, &w->set, w->ndyn, w->ndyn ? wd : NULL);
        g_r.lay = w->lay; g_r.set = w->set; g_r.ndyn = w->ndyn; memcpy(g_rdyn, wd, w->ndyn * sizeof *wd); g_r.ok |= OK_SET; g_s.set_emit += (uint64_t)count_set; g_cmds++;
    }
    for (uint32_t b = 0; b < VBN; b++) {   // runs of changed known bindings, one call a run
        if (!(w->vok >> b & 1) || ((g_r.vok >> b & 1) && g_r.vb[b] == w->vb[b] && g_r.vo[b] == w->vo[b])) continue;
        uint32_t e = b + 1;
        while (e < VBN && (w->vok >> e & 1) && !((g_r.vok >> e & 1) && g_r.vb[e] == w->vb[e] && g_r.vo[e] == w->vo[e])) e++;
        vkCmdBindVertexBuffers(cb, b, e - b, &w->vb[b], &w->vo[b]); g_cmds++;
        for (uint32_t k = b; k < e; k++) { g_r.vb[k] = w->vb[k]; g_r.vo[k] = w->vo[k]; g_r.vok |= (uint8_t)(1u << k); }
        b = e - 1;
    }
    if ((w->ok & OK_IB) && (!(g_r.ok & OK_IB) || g_r.ib != w->ib || g_r.io != w->io || g_r.it != w->it)) { vkCmdBindIndexBuffer(cb, w->ib, w->io, w->it); g_r.ib = w->ib; g_r.io = w->io; g_r.it = w->it; g_r.ok |= OK_IB; g_cmds++; }
    if ((w->ok & OK_VP) && (!(g_r.ok & OK_VP) || memcmp(&g_r.vp, &w->vp, sizeof w->vp))) { vkCmdSetViewport(cb, 0, 1, &w->vp); g_r.vp = w->vp; g_r.ok |= OK_VP; g_cmds++; }
    if ((w->ok & OK_SC) && (!(g_r.ok & OK_SC) || memcmp(&g_r.sc, &w->sc, sizeof w->sc))) { vkCmdSetScissor(cb, 0, 1, &w->sc); g_r.sc = w->sc; g_r.ok |= OK_SC; g_cmds++; }
}
VkCommandBuffer vkn_cb_peek(void);   // vkn_frame.c (no flush)
void vkn_sort_flush(void) {
    if (!vkn_sort_n) return;
    const uint32_t n = vkn_sort_n; vkn_sort_n = 0;   // (first: a command recorded below never re-enters)
    VkCommandBuffer cb = vkn_cb_peek();
    if (!cb) { g_npool = 0; return; }   // (no recording: nothing can be recorded; the packets are lost the way the draws would have been)
    g_s.windows++;
    {   // binds in the recorded order (what mode 0 records), from the real state at the window's start
        VkPipeline p = (g_r.ok & OK_PIPE) ? g_r.pipe : VK_NULL_HANDLE; VkDescriptorSet st = (g_r.ok & OK_SET) ? g_r.set : VK_NULL_HANDLE; uint32_t sn = g_r.ndyn; const uint32_t *sd = g_rdyn;
        for (uint32_t i = 0; i < n; i++) {
            const vst *s = &g_p[i].s; const uint32_t *d = g_pool + s->dyn0;
            if (g_p[i].draw) g_s.packets++;
            if ((s->ok & OK_PIPE) && s->pipe != p) { g_s.pipe_rec++; p = s->pipe; }
            if ((s->ok & OK_SET) && (s->set != st || s->ndyn != sn || memcmp(d, sd, s->ndyn * sizeof *d))) { g_s.set_rec++; st = s->set; sn = s->ndyn; sd = d; }
        }
    }
    // the sorted order: the draws grouped by pipeline (first appearance), each group in recorded order; the group of the last draw goes
    // last (its last draw is the last recorded one: the final state is then nearly the draw path's belief). Packets without a draw (the
    // binds of a draw that ended before its command) are dropped there: their state is superseded by the next draw's own (a packet is the
    // whole state) or put back at the end. Counted in both modes; mode 2 then records the recorded order instead (equal to mode 0).
    uint32_t m = 0;
    {
        enum { G = 64 }; VkPipeline gp[G]; uint32_t gc[G], gs[G], ng = 0, lastg = 0; int over = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!g_p[i].draw) continue;
            uint32_t k = 0; while (k < ng && gp[k] != g_p[i].s.pipe) k++;
            if (k == ng) { if (ng == G) { over = 1; break; } gp[ng] = g_p[i].s.pipe; gc[ng] = 0; ng++; }
            gc[k]++; g_gi[i] = (uint8_t)k; lastg = k; m++;
        }
        if (over) { m = 0; for (uint32_t i = 0; i < n; i++) if (g_p[i].draw) g_ord[m++] = (uint16_t)i; }
        else {
            uint32_t at = 0;
            for (uint32_t k = 0; k < ng; k++) if (k != lastg) { gs[k] = at; at += gc[k]; }
            gs[lastg] = at;
            for (uint32_t i = 0, r = 0; i < n; i++) if (g_p[i].draw) { const uint32_t o = gs[g_gi[i]]++; g_ord[o] = (uint16_t)i; if (o != r) g_s.moved++; r++; }
        }
        VkPipeline p = (g_r.ok & OK_PIPE) ? g_r.pipe : VK_NULL_HANDLE;
        for (uint32_t j = 0; j < m; j++) if (g_p[g_ord[j]].s.pipe != p) { g_s.pipe_sort++; p = g_p[g_ord[j]].s.pipe; }
    }
    if (g_mode != 1) { m = n; for (uint32_t i = 0; i < n; i++) g_ord[i] = (uint16_t)i; }   // mode 2: every packet, as recorded
    for (uint32_t j = 0; j < m; j++) {
        const pkt *p = &g_p[g_ord[j]];
        put(cb, &p->s, g_pool + p->s.dyn0, 1);
        if (!p->draw) continue;
        if (p->idx) vkCmdDrawIndexed(cb, p->count, p->inst, p->first, p->voff, p->finst);
        else vkCmdDraw(cb, p->count, p->inst, p->first, p->finst);
    }
    // back to what the draw path believes (its g_b): its next bind decisions stay the ones of mode 0
    const uint64_t c0 = g_cmds;
    put(cb, &g_b, g_bdyn, 0);
    if (g_cmds != c0) g_s.restores++;
    g_npool = 0;
}
void vkn_sort_forget(void) {
    if (!vkn_sort_on) return;
    vkn_sort_flush();
    memset(&g_r, 0, sizeof g_r); memset(&g_b, 0, sizeof g_b);
}

// ---- the draw path's commands ----
#define DIRECT() (!vkn_sort_cap)
void vkn_sort_pipe(VkCommandBuffer cb, VkPipeline p) {
    if (DIRECT()) { vkn_sort_flush(); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g_r.pipe = p; g_r.ok |= OK_PIPE; }
    g_b.pipe = p; g_b.ok |= OK_PIPE;
}
void vkn_sort_sets(VkCommandBuffer cb, VkPipelineLayout layout, VkDescriptorSet set, uint32_t ndyn, const uint32_t *dyn) {
    if (ndyn > MAXDYN) ndyn = MAXDYN;   // (the draw path's own limit is the same)
    if (DIRECT()) {
        vkn_sort_flush(); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, ndyn, ndyn ? dyn : NULL);
        g_r.lay = layout; g_r.set = set; g_r.ndyn = ndyn; memcpy(g_rdyn, dyn, ndyn * sizeof *dyn); g_r.ok |= OK_SET;
    }
    g_b.lay = layout; g_b.set = set; g_b.ndyn = ndyn; memcpy(g_bdyn, dyn, ndyn * sizeof *dyn); g_b.ok |= OK_SET;
}
void vkn_sort_vb(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *buf, const VkDeviceSize *off) {
    if (DIRECT()) { vkn_sort_flush(); vkCmdBindVertexBuffers(cb, first, n, buf, off); }
    for (uint32_t i = 0; i < n && first + i < VBN; i++) {
        const uint32_t b = first + i;
        if (DIRECT()) { g_r.vb[b] = buf[i]; g_r.vo[b] = off[i]; g_r.vok |= (uint8_t)(1u << b); }
        g_b.vb[b] = buf[i]; g_b.vo[b] = off[i]; g_b.vok |= (uint8_t)(1u << b);
    }
}
void vkn_sort_ib(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type) {
    if (DIRECT()) { vkn_sort_flush(); vkCmdBindIndexBuffer(cb, buf, off, type); g_r.ib = buf; g_r.io = off; g_r.it = type; g_r.ok |= OK_IB; }
    g_b.ib = buf; g_b.io = off; g_b.it = type; g_b.ok |= OK_IB;
}
void vkn_sort_vp(VkCommandBuffer cb, const VkViewport *vp) {
    if (DIRECT()) { vkn_sort_flush(); vkCmdSetViewport(cb, 0, 1, vp); g_r.vp = *vp; g_r.ok |= OK_VP; }
    g_b.vp = *vp; g_b.ok |= OK_VP;
}
void vkn_sort_sc(VkCommandBuffer cb, const VkRect2D *sc) {
    if (DIRECT()) { vkn_sort_flush(); vkCmdSetScissor(cb, 0, 1, sc); g_r.sc = *sc; g_r.ok |= OK_SC; }
    g_b.sc = *sc; g_b.ok |= OK_SC;
}
static void capture(uint8_t draw, uint8_t idx, uint32_t count, uint32_t inst, uint32_t first, int32_t voff, uint32_t finst) {
    if (vkn_sort_n == WIN || g_npool + g_b.ndyn > DYNPOOL) { g_s.full++; vkn_sort_flush(); }
    pkt *p = &g_p[vkn_sort_n++];
    p->s = g_b; p->s.dyn0 = g_npool; memcpy(g_pool + g_npool, g_bdyn, g_b.ndyn * sizeof *g_bdyn); g_npool += g_b.ndyn;
    p->draw = draw; p->idx = idx; p->count = count; p->inst = inst; p->first = first; p->voff = voff; p->finst = finst;
}
void vkn_sort_nodraw(void) { if (vkn_sort_cap) { capture(0, 0, 0, 0, 0, 0, 0); vkn_sort_cap = 0; } }
void vkn_sort_draw(VkCommandBuffer cb, uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) {
    if (DIRECT()) { vkn_sort_flush(); vkCmdDraw(cb, count, instances, first, first_instance); return; }
    capture(1, 0, count, instances, first, 0, first_instance); vkn_sort_cap = 0;
}
void vkn_sort_draw_idx(VkCommandBuffer cb, uint32_t count, uint32_t instances, uint32_t first, int32_t vertex_offset, uint32_t first_instance) {
    if (DIRECT()) { vkn_sort_flush(); vkCmdDrawIndexed(cb, count, instances, first, vertex_offset, first_instance); return; }
    capture(1, 1, count, instances, first, vertex_offset, first_instance); vkn_sort_cap = 0;
}

// ---- the decision (vkn_draw.c prepare, after vkn_pass_ready) ----
int vkn_sort_eligible(uint16_t depth_state, uint32_t ncolor, const VkColorComponentFlags *write_masks, int has_depth, const struct vkn_shader *s,
                      int nested, int rewritten, uint64_t pass_begins) {
    const int first = pass_begins != g_last_begins; g_last_begins = pass_begins;
    int why = -1;
    if (nested) why = VKN_SORT_W_NESTED;
    else if (rewritten) why = VKN_SORT_W_REWRITE;
    else {
        for (uint32_t i = 0; i < ncolor && i < 8; i++) if (write_masks[i]) { why = VKN_SORT_W_COLOUR; break; }
        if (why < 0) {
            const VkPipelineDepthStencilStateCreateInfo *ds = has_depth && depth_state ? vkn_ds_info(depth_state) : NULL;
            if (!ds || !ds->depthTestEnable || (ds->depthCompareOp != VK_COMPARE_OP_LESS && ds->depthCompareOp != VK_COMPARE_OP_LESS_OR_EQUAL) || ds->depthBoundsTestEnable) why = VKN_SORT_W_DEPTH;
            else if (ds->stencilTestEnable) why = VKN_SORT_W_STENCIL;
        }
        if (why < 0 && s) for (uint32_t i = 0; i < s->nbind; i++) if (s->bind[i].kind == VKN_BK_IMAGE || s->bind[i].kind == VKN_BK_STORAGE) { why = VKN_SORT_W_SHADER; break; }
        if (why < 0 && vkn_query_idle && !vkn_query_idle()) why = VKN_SORT_W_QUERY;
        if (why < 0 && first) why = VKN_SORT_W_FIRST;
    }
    if (why >= 0) { g_s.why_not[why]++; vkn_sort_flush(); vkn_sort_cap = 0; return 0; }
    vkn_sort_cap = 1;
    return 1;
}

void vkn_sort_stats_get(vkn_sort_stats *s) { if (s) *s = g_s; }
void vkn_sort_census(void) {
    static vkn_sort_stats o;
    if (!g_mode) return;
    const vkn_sort_stats s = g_s;
#define D(x) (unsigned long long)(s.x - o.x)
    vkn_logf("sort (DM_VK_NATIVE_SORT=%d, since the last line): windows %llu, draws in them %llu, moved %llu; pipeline binds: recorded order %llu, sorted order %llu, emitted %llu; "
             "set binds: recorded order %llu, emitted %llu; restores %llu, full windows %llu; not taken: nested %llu, colour %llu, depth op %llu, stencil %llu, storage %llu, query %llu, "
             "first of a pass %llu, rewritten %llu", g_mode, D(windows), D(packets), D(moved), D(pipe_rec), D(pipe_sort), D(pipe_emit), D(set_rec), D(set_emit), D(restores), D(full),
             D(why_not[0]), D(why_not[1]), D(why_not[2]), D(why_not[3]), D(why_not[4]), D(why_not[5]), D(why_not[6]), D(why_not[7]));
#undef D
    o = s;
}
