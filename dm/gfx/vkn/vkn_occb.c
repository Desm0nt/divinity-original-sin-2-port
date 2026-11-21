// [DOS2] VK-NATIVE stage 5, technique B (group query + group condition, docs/dos2/nr/VK-OCCLUSION-ALT.md section 5). Contract, rules and
// the exactness / latency argument: vkn_occb.h. Sections: objects (verdict table), per-recording state (pairs, groups, captures),
// effect functions (classify / capture), the flush (replays under group queries and conditions), the query module's side (copies,
// results), census. No game addresses.
#ifdef DM_GAME_DOS2
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkn_occb.h"
#include "vkn_query.h"
#include "vkn_fx.h"
#include "vkn_buf.h"
#include "vkn_tex.h"
#include "vkn_state.h"
#include "vkn_pass.h"
#include "vkn_frame.h"
#include "vkn_policy.h"
#include "../nr/nr_occ.h"

#define B_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
enum { BOBJS = 8192, BPROBE = 24, OCCM = 8192, FXOS = 256, UBS = 16, MKEYS = 4096, TCACHE = 512, REDS = 16 };
enum { CL_NONE = 0, CL_PLAIN, CL_MEMBER, CL_LEARN };
enum { K_Z = 1, K_E = 2, K_ANY = 3 };                       // pending kinds / draw classes (order-free depth writers, EQUAL readers)
enum { BQ_NONE = 0, BQ_LEARN, BQ_GROUP, BQ_MEMBER, BQ_CVER };
enum { RD_GATE, RD_LOST, RD_STALE, RD_DROP, RD_VIOL, RD_CLEAR, RD_PASS, RD_FULL, RD_N };
static const char *const k_rwhy[VKN_OB_R_N] = {"", "not order-free", "blending", "primitive rewrite", "would end the pass", "constant block never announced",
    "vertex / index buffer missing", "capture table full", "area pass", "no pass", "member split / dropped", "pair incomplete", "group word not copied",
    "not EQUAL without writes", "member drawn partly in place", "other target set"};
static const char *const k_fwhy[VKN_OB_F_N] = {"pass end", "order", "pass break", "target set", "kind", "table full", "recording end", "late"};

static int g_mode, g_budget = -1, g_budget_asked = -1;
static vkn_occb_stats g_st;
static uint8_t g_red[RD_N];
#define BRED(i, ...) do { if (g_red[i] < REDS) { g_red[i]++; vkn_logf(B_RED "occ-b: " __VA_ARGS__); } } while (0)
static inline uint64_t mix(uint64_t h, uint64_t v) { return nro_mix(h, v); }
static void (*g_t_samples)(const vkn_draw_in *in, uint32_t first, int replay);
static uint32_t g_tick;
#define T_BEGIN const int t_on_ = (++g_tick & 15) == 0; const uint64_t t0_ = t_on_ ? vkn_now_ns() : 0
#define T_END do { if (t_on_) g_st.cpu_ns += 16 * (vkn_now_ns() - t0_); } while (0)

int vkn_occb_mode(void) { return g_mode; }
int vkn_occb_budget(void) { return g_budget; }
void vkn_occb_stats_get(vkn_occb_stats *s) { if (s) *s = g_st; }
void vkn_occb_test_samples(void (*fn)(const vkn_draw_in *in, uint32_t first, int replay)) { g_t_samples = fn; }

// ================================================================ objects (key -> verdict)
typedef struct bo { uint64_t key; uint32_t seen, vf, qf; uint8_t hid, check; } bo;
static bo g_bo[BOBJS];
static uint32_t g_mf;                              // model frame: presented recordings with B on, from 1
static int32_t bo_get(uint64_t key) {
    const uint32_t h = (uint32_t)(mix(0x424f424aull, key) & (BOBJS - 1));
    int32_t victim = -1; uint32_t vseen = 0xffffffffu;
    for (uint32_t c = 0; c < BPROBE; c++) {
        const uint32_t i = (h + c) & (BOBJS - 1);
        if (g_bo[i].key == key) return (int32_t)i;
        if (!g_bo[i].key) { victim = (int32_t)i; break; }
        if (g_bo[i].seen + 600 < g_mf && g_bo[i].seen < vseen) { victim = (int32_t)i; vseen = g_bo[i].seen; }
    }
    if (victim < 0) return -1;
    bo *o = &g_bo[victim]; memset(o, 0, sizeof *o); o->key = key;
    return victim;
}
static bo *bo_find(uint64_t key) {
    const uint32_t h = (uint32_t)(mix(0x424f424aull, key) & (BOBJS - 1));
    for (uint32_t c = 0; c < BPROBE; c++) { bo *o = &g_bo[(h + c) & (BOBJS - 1)]; if (o->key == key) return o; if (!o->key) return NULL; }
    return NULL;
}

// ================================================================ per recording
typedef struct bp { uint64_t key; int32_t ob; uint8_t cls, bad, keyed, ncap; uint16_t gi; } bp;
static bp g_bp[VKN_Q_RECS];
typedef struct grp { uint8_t bucket; uint16_t seg; int32_t q; uint8_t copied, made; uint16_t members, mq, mq_copied; } grp;
static grp g_grp[VKN_OCCB_GROUPS]; static uint32_t g_ngrp; static uint16_t g_seg; static int16_t g_cur_gi[VKN_OCCB_BUCKETS];
static struct { uint64_t key; uint32_t gen, n; } g_occ[OCCM]; static uint32_t g_ogen = 1;
static int g_on, g_slot = -1, g_learn_left, g_checks_left;
// readback tables per slot: what each query of B (or of a learning pair) stands for
typedef struct bq { uint8_t kind; uint16_t gi; uint32_t mk0, mkn; uint64_t key; } bq;
static bq g_bq[VKN_Q_SLOTS][VKN_Q_CAP];
static uint64_t g_mk[VKN_Q_SLOTS][MKEYS]; static uint32_t g_nmk[VKN_Q_SLOTS];
static uint32_t g_sor[VKN_Q_SLOTS][VKN_OCCB_GROUPS]; static uint8_t g_sbucket[VKN_Q_SLOTS][VKN_OCCB_GROUPS]; static uint32_t g_smf[VKN_Q_SLOTS];
static int8_t g_last_bucket[VKN_OCCB_BUCKETS];      // the last read verdict of each bucket (1 hidden, 0 visible, -1 none): flips
// captures (the pending list: one render-pass instance's)
typedef struct cap {
    vkn_draw_in in; int32_t tex[2][VKN_TEX_SLOTS], smp[2][VKN_TEX_SLOTS]; uint8_t tex_on[2], smp_on[2];
    uint32_t first, count, instances; uint8_t indexed, nub, vread; int16_t fxo; uint16_t rec, gi;
    struct { uint8_t st, slot; uint32_t off, size; } ub[UBS];
    uint64_t vver[3], iver;
} cap;
static cap g_cap[VKN_OCCB_CAPS]; static uint32_t g_ncap; static int g_pkind;
static vkn_fx_out g_fxo[FXOS]; static uint32_t g_nfxo;
static const vkn_fx_out k_zero_out;
static struct { uint32_t n; vkn_tex *c[VKN_PASS_COLORS]; uint32_t layer[VKN_PASS_COLORS], mip[VKN_PASS_COLORS]; vkn_tex *d; } g_pset;
static uint64_t g_clr0;                             // vkn_pass_stats.clears_att at the first capture of the list
// where the recording thread is (the safety of a replay from the pass-end hook): 0 between draws, 1 an engine draw entered, 2 its
// effect chain runs (g_chain_out = its output), 3 after the chain, the draw path still preparing it (g_stamp0: draws + drops then)
static int g_phase, g_replaying, g_in_closing; static vkn_fx_out *g_chain_out; static const vkn_draw_in *g_chain_in; static uint32_t g_chain_first;
static uint64_t g_stamp0; static int g_qactive;

void vkn_occb_test_reset(void) {
    memset(g_bo, 0, sizeof g_bo); memset(g_bp, 0, sizeof g_bp); memset(g_grp, 0, sizeof g_grp); g_ngrp = 0; g_seg = 0;
    memset(g_bq, 0, sizeof g_bq); memset(g_nmk, 0, sizeof g_nmk); memset(&g_st, 0, sizeof g_st); memset(g_red, 0, sizeof g_red);
    memset(g_last_bucket, 0xff, sizeof g_last_bucket); g_ncap = 0; g_nfxo = 0; g_pkind = 0; g_mf = 0; g_on = 0; g_phase = 0;
}
static uint32_t occ_next(uint64_t k) {
    for (uint32_t i = (uint32_t)(mix(0x4f434342ull, k) & (OCCM - 1)), c = 0; c < OCCM; i = (i + 1) & (OCCM - 1), c++) {
        if (g_occ[i].gen != g_ogen) { g_occ[i].gen = g_ogen; g_occ[i].key = k; g_occ[i].n = 1; return 0; }
        if (g_occ[i].key == k) return g_occ[i].n++;
    }
    return 0xffffffffu;
}
static int gi_of(uint32_t bucket) {   // the group (bucket, current segment), made on first use; -1 = table full
    if (g_cur_gi[bucket] >= 0) return g_cur_gi[bucket];
    if (g_ngrp >= VKN_OCCB_GROUPS) return -1;
    grp *g = &g_grp[g_ngrp]; memset(g, 0, sizeof *g); g->bucket = (uint8_t)bucket; g->seg = g_seg; g->q = -1;
    g_cur_gi[bucket] = (int16_t)g_ngrp;
    g_st.groups++;
    return (int)g_ngrp++;
}

// ================================================================ draw facts
static uint8_t g_dsk[1024], g_bof[1024];
static int ds_kind(uint16_t id) {   // K_Z: depth LESS / LEQUAL with writes, no stencil; K_E: EQUAL without depth writes, no stencil; 0 other
    if (!id) return 0;
    if (id < 1024 && g_dsk[id]) return g_dsk[id] - 1;
    uint8_t a[8]; int k = 0;
    if (!vkn_ds_args(id, a) && !a[3]) {   // test, write, func (0 never 1 less 2 equal 3 lequal ...), stencil on
        if (a[0] && a[1] && (a[2] == 1 || a[2] == 3)) k = K_Z;
        else if (a[0] && !a[1] && a[2] == 2) k = K_E;
    }
    if (id < 1024) g_dsk[id] = (uint8_t)(k + 1);
    return k;
}
static int blend_off(uint16_t id) {   // vkn_state.c's rule for blendEnable = false: ONE / ZERO / ADD on colour and alpha
    if (!id) return 1;
    if (id < 1024 && g_bof[id]) return g_bof[id] - 1;
    uint8_t a[6]; int off = 0;
    if (!vkn_blend_args(id, a)) off = a[0] == 1 && a[1] == 0 && (!a[5] || (a[2] == 1 && a[3] == 0 && !(a[4] >= 1 && a[4] <= 4)));
    if (id < 1024) g_bof[id] = (uint8_t)(off + 1);
    return off;
}
static int any_color(const vkn_fx_ctx *ctx) { for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) if (ctx->color[i]) return 1; return 0; }
// The class of a draw as the chain finally made it (for the order rule): K_ANY = writes nothing (rasterizer discard).
static int draw_kind(const vkn_fx_ctx *ctx, const vkn_fx_out *out) {
    if (out->discard) return K_ANY;
    const int k = ds_kind(out->ds ? out->ds : ctx->ds);
    if (k == K_Z && !out->no_fs && any_color(ctx) && !blend_off(ctx->blend)) return 0;   // a blended colour write is not order-free
    return k;
}
// The texture behind a pool id, cached by the binding generation (vkn_tex_view_by_id takes the texture module's mutex).
typedef struct tcent { int32_t id1; uint32_t gen; vkn_tex *t; } tcent;
static tcent g_tc[TCACHE];
static vkn_tex *tex_of(int32_t id) {
    const uint32_t gen = vkn_tex_bind_gen();
    tcent *e = &g_tc[(uint32_t)id & (TCACHE - 1)];
    if (e->id1 == id + 1 && e->gen == gen) return e->t;
    vkn_tex *t = NULL; (void)vkn_tex_view_by_id((uint32_t)id, &t);
    e->id1 = id + 1; e->gen = gen; e->t = t;
    return t;
}
static int tex_breaks(const vkn_tex *t, const vkn_fx_ctx *ctx) {   // vkn_pass_sample would end the open pass for it
    if (!t || !t->view) return 0;
    if (t == ctx->depth) return 0;
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) if (ctx->color[i] == t) return 0;   // (an attachment: a placeholder, no break)
    return t->pass_nclr || t->pass_dclr || t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
// Would the draw path end the open pass while preparing this draw (a texture that needs a barrier, a storage image)? Only for draws
// about to be captured: a replay inside a group query must stay in one render-pass instance.
static int would_break(const vkn_draw_in *in, const vkn_fx_ctx *ctx, const vkn_fx_out *out) {
    const vkn_shader *s = out->shader ? out->shader : ctx->shader;
    if (!s) return 1;
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        const int st = b->stage == 0 ? 0 : b->stage == 1 ? 1 : -1;
        switch (b->kind) {
        case VKN_BK_TEXTURE:
            if (st < 0) return 1;
            for (uint32_t e = 0; e < b->count; e++) {
                const uint32_t slot = (uint32_t)b->slot + e;
                const int32_t id = slot < VKN_TEX_SLOTS && in->tex[st] ? in->tex[st][slot] : -1;
                if (id >= 0 && tex_breaks(tex_of(id), ctx)) return 1;
            }
            break;
        case VKN_BK_IMAGE: case VKN_BK_STORAGE: return 1;
        case VKN_BK_FX_TEX: if (b->slot < VKN_FX_TEX && out->tex[b->slot].tex && tex_breaks(out->tex[b->slot].tex, ctx)) return 1; break;
        default: break;
        }
    }
    return 0;
}
static int set_is_pending(const vkn_fx_ctx *ctx) {
    if (ctx->depth != g_pset.d) return 0;
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) if (ctx->color[i] != (i < g_pset.n ? g_pset.c[i] : NULL)) return 0;
    return 1;
}
static uint64_t stamp(void) {   // vkn_pass.c draw_stamp + the rewrite's drops: moves when a prepared draw ends (recorded or dropped)
    vkn_draw_stats a; vkn_draw_stats2 b; vkn_draw_rw_stats r; vkn_draw_stats_get(&a); vkn_draw_stats2_get(&b); vkn_draw_rw_stats_get(&r);
    return a.draws + a.dropped_shader + a.dropped_state + a.dropped_pipe + a.dropped_pass + a.dropped_prim + b.dropped_bind + b.dropped_vb + b.dropped_ib +
           b.dropped_viewport + b.dropped_variant + b.dropped_set + b.dropped_cb + b.scissor_empty + b.placeholder_fail + r.dropped;
}

// ================================================================ capture
static int capture(int kind, uint32_t rec, uint16_t gi, const vkn_draw_in *in, const vkn_fx_ctx *ctx, const vkn_fx_out *out) {
    if (ctx->prim != in->prim) return VKN_OB_R_REWRITE;   // fan / quads / loop: the rewrite reads the engine's indices at the draw
    if (g_ncap >= VKN_OCCB_CAPS) return VKN_OB_R_FULL;
    const int fz = memcmp(out, &k_zero_out, sizeof *out) != 0;
    if (fz && g_nfxo >= FXOS) return VKN_OB_R_FULL;
    if (kind == K_E && !blend_off(ctx->blend)) return VKN_OB_R_BLEND;
    if (would_break(in, ctx, out)) return VKN_OB_R_BREAKS;
    { VkRect2D a; if (vkn_pass_area_get(&a) && ctx->depth && (a.offset.x || a.offset.y || a.extent.width < ctx->depth->w || a.extent.height < ctx->depth->h)) return VKN_OB_R_AREA; }
    cap *c = &g_cap[g_ncap];
    c->nub = 0;
    const vkn_shader *s = out->shader ? out->shader : ctx->shader;
    for (uint32_t i = 0; s && i < s->nbind; i++) {   // the constant blocks the draw binds (engine slots: as vkn_draw_ubo announced them)
        const vkn_bind *b = &s->bind[i];
        if (b->kind != VKN_BK_UNIFORM || b->stage > 1) continue;
        for (uint32_t e = 0; e < b->count; e++) {
            const uint32_t slot = (uint32_t)b->slot + e;
            if (slot >= VKN_UBO_SLOTS) return VKN_OB_R_UBO;
            int dup = 0; for (uint32_t j = 0; j < c->nub; j++) dup |= c->ub[j].st == b->stage && c->ub[j].slot == slot;
            if (dup) continue;
            if (c->nub >= UBS) return VKN_OB_R_FULL;
            if (vkn_draw_ubo_get(b->stage, slot, &c->ub[c->nub].off, &c->ub[c->nub].size)) return VKN_OB_R_UBO;   // the zero block now, maybe not later
            c->ub[c->nub].st = b->stage; c->ub[c->nub].slot = (uint8_t)slot; c->nub++;
        }
    }
    // the buffers it reads: marked "used in this frame" as the draw would mark them (the rename rule), versions for the replay's check
    const uint16_t vid = ctx->vfmt; const vkn_vfmt *vf = vid ? vkn_vfmt_info(vid) : NULL;
    c->vread = 0;
    uint64_t vh[3]; uint32_t nh = 0;
    for (uint32_t i = 0; vf && i < vf->nbind; i++) {
        const uint32_t st = vf->bind[i].binding; if (st >= 3 || (c->vread >> st & 1)) continue;
        if (!in->vb[st]) return VKN_OB_R_BUF;
        c->vread |= (uint8_t)(1u << st); c->vver[st] = vkn_buf_version(VKN_BUF_VB, in->vb[st], NULL); vh[nh++] = in->vb[st];
    }
    if (nh) { const vkn_block *vb[3]; if (vkn_buf_get_n(VKN_BUF_VB, vh, nh, vb) != nh) return VKN_OB_R_BUF; }   // (one lock; marks them used as the draw would)
    c->iver = 0;
    if (ctx->indexed) { if (!in->ib || !vkn_buf_get(VKN_BUF_IB, in->ib)) return VKN_OB_R_BUF; c->iver = vkn_buf_version(VKN_BUF_IB, in->ib, NULL); }
    if (!g_ncap) {   // the first capture of a list: the pass begins here as the engine draw would begin it (a clear after this one is in-pass)
        vkn_pass_info pi;
        if (vkn_pass_ready(&pi) || !vkn_cb()) return VKN_OB_R_NOPASS;
        vkn_pass_cur_get(&g_pset.n, g_pset.c, g_pset.layer, g_pset.mip, &g_pset.d);
        vkn_pass_stats ps; vkn_pass_stats_get(&ps); g_clr0 = ps.clears_att;
        g_pkind = kind;
    }
    c->in = *in;
    for (int st = 0; st < 2; st++) {
        c->tex_on[st] = in->tex[st] != NULL; c->smp_on[st] = in->smp[st] != NULL;
        if (in->tex[st]) memcpy(c->tex[st], in->tex[st], sizeof c->tex[st]);
        if (in->smp[st]) memcpy(c->smp[st], in->smp[st], sizeof c->smp[st]);
    }
    c->in.tex[2] = NULL; c->in.smp[2] = NULL;
    c->first = ctx->first; c->count = ctx->count; c->instances = ctx->instances; c->indexed = ctx->indexed;
    c->rec = (uint16_t)rec; c->gi = gi;
    c->fxo = -1;
    if (fz) { g_fxo[g_nfxo] = *out; c->fxo = (int16_t)g_nfxo++; }
    g_ncap++;
    return 0;
}

// ================================================================ replay / flush
static int replay_one(const cap *c) {
    int stale = c->indexed && vkn_buf_version(VKN_BUF_IB, c->in.ib, NULL) != c->iver;
    for (int st = 0; st < 3; st++) if ((c->vread >> st & 1) && vkn_buf_version(VKN_BUF_VB, c->in.vb[st], NULL) != c->vver[st]) stale = 1;
    if (stale) { g_st.replay_stale++; BRED(RD_STALE, "replay: a captured draw (pair %u) reads a vertex / index buffer written since it was captured (drawn with the new bytes)", c->rec); }
    for (uint32_t i = 0; i < c->nub; i++) vkn_draw_ubo(c->ub[i].st, c->ub[i].slot, c->ub[i].off, c->ub[i].size);
    vkn_draw_in in = c->in;
    in.tex[0] = c->tex_on[0] ? c->tex[0] : NULL; in.tex[1] = c->tex_on[1] ? c->tex[1] : NULL;
    in.smp[0] = c->smp_on[0] ? c->smp[0] : NULL; in.smp[1] = c->smp_on[1] ? c->smp[1] : NULL;
    if (g_t_samples) g_t_samples(&in, c->first, 1);
    vkn_fx_force(c->fxo >= 0 ? &g_fxo[c->fxo] : &k_zero_out);
    const int rc = c->indexed ? vkn_draw_indexed(&in, c->first, c->count, c->instances) : vkn_draw_arrays(&in, c->first, c->count);
    vkn_fx_force(NULL);
    if (rc) { g_st.replay_drop++; BRED(RD_DROP, "replay: the draw path dropped a captured draw (pair %u): its object counts as visible", c->rec); g_bp[c->rec].bad = 1; }
    else g_st.replays++;
    return rc;
}
static uint64_t passes_now(void) { vkn_pass_stats p; vkn_pass_stats_get(&p); return p.passes; }
static void slot_note(uint32_t q, uint8_t kind, uint16_t gi, uint64_t key, uint32_t mk0, uint32_t mkn) {
    if (g_slot < 0 || q >= VKN_Q_CAP) return;
    bq *e = &g_bq[g_slot][q]; e->kind = kind; e->gi = gi; e->key = key; e->mk0 = mk0; e->mkn = mkn;
}
static void flush_pre(VkCommandBuffer cb) {
    for (uint32_t gi = 0; gi < g_ngrp; gi++) {
        grp *g = &g_grp[gi];
        uint32_t n = 0; for (uint32_t i = 0; i < g_ncap; i++) n += g_cap[i].gi == gi;
        if (!n) continue;
        const uint64_t p0 = passes_now();
        if (g_mode == VKN_COND_B) {
            VkQueryPool pool = VK_NULL_HANDLE; uint32_t q = 0;
            const int qok = !vkn_query_b_qalloc(&pool, &q);
            if (qok) { vkCmdBeginQuery(cb, pool, q, 0); g_qactive = 1; }
            uint32_t mk0 = g_slot >= 0 ? g_nmk[g_slot] : 0, mkn = 0; int last = -1;
            for (uint32_t i = 0; i < g_ncap; i++) {
                const cap *c = &g_cap[i]; if (c->gi != gi) continue;
                (void)replay_one(c);
                if ((int)c->rec != last) {   // a member (its draws are consecutive: pairs do not nest)
                    last = c->rec;
                    if (g_slot >= 0 && g_nmk[g_slot] < MKEYS) { g_mk[g_slot][g_nmk[g_slot]++] = g_bp[c->rec].key; mkn++; }
                }
            }
            if (qok) { vkCmdEndQuery(cb, pool, q); g_qactive = 0; g->q = (int32_t)q; g->made = 1; g_st.q_group++; slot_note(q, BQ_GROUP, (uint16_t)gi, 0, mk0, mkn); }
            else { g->q = -1; BRED(RD_FULL, "no query left for group %u: its members are drawn without a condition", gi); }
            if (g_slot >= 0 && gi < VKN_OCCB_GROUPS) g_sbucket[g_slot][gi] = g->bucket;
        } else {   // verify: one query per member, in the same order
            uint32_t i = 0;
            while (i < g_ncap) {
                if (g_cap[i].gi != gi) { i++; continue; }
                const uint16_t rec = g_cap[i].rec;
                VkQueryPool pool = VK_NULL_HANDLE; uint32_t q = 0;
                const int qok = !vkn_query_b_qalloc(&pool, &q);
                if (qok) { vkCmdBeginQuery(cb, pool, q, 0); g_qactive = 1; }
                for (; i < g_ncap; i++) { const cap *c = &g_cap[i]; if (c->gi != gi) continue; if (c->rec != rec) break; (void)replay_one(c); }
                if (qok) { vkCmdEndQuery(cb, pool, q); g_qactive = 0; g->mq++; g_st.q_member++; slot_note(q, BQ_MEMBER, (uint16_t)gi, g_bp[rec].key, 0, 0); }
                else g_bp[rec].bad = 1;
            }
            g->made = 1;
            if (g_slot >= 0 && gi < VKN_OCCB_GROUPS) g_sbucket[g_slot][gi] = g->bucket;
        }
        if (passes_now() != p0) BRED(RD_PASS, "group %u: a replay began another render pass inside its query (a barrier the capture did not foresee)", gi);
        { uint32_t mem = 0; int last = -1; for (uint32_t i = 0; i < g_ncap; i++) if (g_cap[i].gi == gi && (int)g_cap[i].rec != last) { last = g_cap[i].rec; mem++; }
          g_st.hist[mem <= 1 ? 0 : mem <= 4 ? 1 : mem <= 16 ? 2 : mem <= 64 ? 3 : mem <= 256 ? 4 : 5]++; }
    }
    g_seg++;
    for (int b = 0; b < VKN_OCCB_BUCKETS; b++) g_cur_gi[b] = -1;   // members after this pass instance form new groups
}
static void flush_cond(VkCommandBuffer cb) {
    for (uint32_t gi = 0; gi < g_ngrp; gi++) {
        grp *g = &g_grp[gi];
        uint32_t n = 0; for (uint32_t i = 0; i < g_ncap; i++) n += g_cap[i].gi == gi;
        if (!n) continue;
        if (g_mode == VKN_COND_B) {
            VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize off = 0;
            vkn_query_b_pred((uint32_t)g->q, &buf, &off);
            const int on = !vkn_query_b_cond(cb, 1, buf, off);
            for (uint32_t i = 0; i < g_ncap; i++) if (g_cap[i].gi == gi) (void)replay_one(&g_cap[i]);
            if (on) { (void)vkn_query_b_cond(cb, 0, VK_NULL_HANDLE, 0); g_st.conds++; g_st.cond_draws += n; } else g_st.cond_nopred += n;
        } else {   // verify: every captured G-buffer draw in its own query, drawn
            for (uint32_t i = 0; i < g_ncap; i++) {
                const cap *c = &g_cap[i]; if (c->gi != gi) continue;
                VkQueryPool pool = VK_NULL_HANDLE; uint32_t q = 0;
                const int qok = !vkn_query_b_qalloc(&pool, &q);
                if (qok) { vkCmdBeginQuery(cb, pool, q, 0); g_qactive = 1; }
                (void)replay_one(c);
                if (qok) { vkCmdEndQuery(cb, pool, q); g_qactive = 0; g_st.q_cver++; slot_note(q, BQ_CVER, (uint16_t)gi, g_bp[c->rec].key, c->rec, 0); }
            }
        }
    }
}
// Draw the pending list now: the capture set must be current (or is made current and put back, outside the pass-end hook).
static void flush(int why) {
    if (!g_ncap || g_replaying) return;
    T_BEGIN;
    g_replaying = 1; g_st.flushes++; if (why < VKN_OB_F_N) g_st.flush_why[why]++;
    if (g_mode == VKN_COND_B_VERIFY || g_pkind == K_Z) vkn_query_b_end_active(vkn_cb());   // (one occlusion query at a time: an open pair's ends, counts as visible)
    vkn_fx_out keep; vkn_fx_out *co = g_phase == 2 ? g_chain_out : NULL; if (co) keep = *co;
    uint32_t n0 = 0, lay[VKN_PASS_COLORS], mip[VKN_PASS_COLORS]; vkn_tex *col[VKN_PASS_COLORS], *dep = NULL;
    vkn_pass_cur_get(&n0, col, lay, mip, &dep);
    int same = n0 == g_pset.n && dep == g_pset.d;
    for (uint32_t i = 0; same && i < n0; i++) same = col[i] == g_pset.c[i] && (!col[i] || (lay[i] == g_pset.layer[i] && mip[i] == g_pset.mip[i]));
    int ok = 1;
    if (!same) {
        if (g_in_closing) { ok = 0; g_st.lost++; BRED(RD_LOST, "the pass of %u captured draws ends under another target set: drawn later", g_ncap); }
        else { g_st.lost_replays++; vkn_pass_targets(g_pset.n, g_pset.c, g_pset.layer, g_pset.mip, g_pset.d); }
    }
    if (ok) {
        struct { uint32_t off, size; int ok; } ub0[2][VKN_UBO_SLOTS];
        for (int st = 0; st < 2; st++) for (uint32_t sl = 0; sl < VKN_UBO_SLOTS; sl++) ub0[st][sl].ok = !vkn_draw_ubo_get(st, sl, &ub0[st][sl].off, &ub0[st][sl].size);
        { vkn_pass_stats ps; vkn_pass_stats_get(&ps);
          if (ps.clears_att != g_clr0) { g_st.clear_after++; BRED(RD_CLEAR, "a clear inside the pass came after %u captured draws: they are drawn after it", g_ncap); } }
        vkn_pass_info pi;
        VkCommandBuffer cb = vkn_pass_ready(&pi) ? VK_NULL_HANDLE : vkn_cb();
        if (!cb) { g_st.lost++; BRED(RD_LOST, "no render pass for %u captured draws: dropped", g_ncap); }
        else if (g_pkind == K_Z) flush_pre(cb); else flush_cond(cb);
        for (int st = 0; st < 2; st++) for (uint32_t sl = 0; sl < VKN_UBO_SLOTS; sl++) if (ub0[st][sl].ok) vkn_draw_ubo(st, sl, ub0[st][sl].off, ub0[st][sl].size);
        if (!same) vkn_pass_targets(n0, col, lay, mip, dep);
        g_ncap = 0; g_nfxo = 0; g_pkind = 0;
    }
    if (co) *co = keep;
    g_replaying = 0;
    T_END;
}

// ================================================================ effect functions
static int want_learn(bo *o) {
    if (g_mode == VKN_COND_B_VERIFY) return 1;
    if (o->qf && o->qf + 8 > g_mf && o->vf < o->qf) return 0;   // a query of it is in flight
    if (o->hid && o->check) { if (g_checks_left > 0) { g_checks_left--; g_st.checks++; return 1; } return 0; }
    if (o->hid || g_learn_left <= 0) return 0;
    if ((uint32_t)((o->key >> 24) + g_mf) % VKN_OCCB_PERIOD) return 0;
    g_learn_left--;
    return 1;
}
static void classify(uint32_t rec, const vkn_draw_in *in, const vkn_fx_ctx *ctx) {
    bp *p = &g_bp[rec];
    p->keyed = 1; g_st.keyed++;
    uint64_t h = nro_mix(0x6f636331ull, (uint64_t)ctx->prim | (uint64_t)ctx->count << 32);   // the oracle's draw identity (vkn_query.c occ_fx)
    h = nro_mix(h, (uint64_t)ctx->first | (uint64_t)ctx->instances << 32);
    h = nro_mix(h, ctx->indexed ? in->ib : 0);
    for (int i = 0; i < 3; i++) h = nro_mix(h, in->vb[i] ^ ((uint64_t)in->vb_off[i] << 40));
    h = nro_mix(h, in->shader);
    p->key = nro_mix(h, occ_next(h)) | 1;
    p->ob = bo_get(p->key); p->cls = CL_PLAIN;
    if (p->ob < 0) return;
    bo *o = &g_bo[p->ob]; o->seen = g_mf;
    // (only a pair without a query: a pending one whose draws were all captured would end "empty" and skip its consumers)
    if (o->hid && !o->check && o->vf && vkn_query_b_noq(rec)) {
        const int gi = gi_of((uint32_t)(mix(0x42554b54ull, p->key) & (VKN_OCCB_BUCKETS - 1)));
        if (gi >= 0) { p->cls = CL_MEMBER; p->gi = (uint16_t)gi; g_grp[gi].members++; g_st.members++; return; }
    }
    if (want_learn(o)) { p->cls = CL_LEARN; o->qf = g_mf; vkn_query_b_learn(rec); g_st.learners++; }
}
// order 3 (first after the UI fence): where the chain is, the pair's class at its first draw
static void fxb_early(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    g_phase = 2; g_chain_out = out; g_chain_in = in; g_chain_first = ctx->first;
    if (!g_on) return;
    const int open = vkn_query_occ_open();
    if (open < 0 || open >= VKN_Q_RECS || g_bp[open].keyed) return;
    uint32_t seen = 0, done = 0; int closed = 0, empty = 0;
    if (vkn_query_occ_rec((uint32_t)open, &seen, &done, &closed, &empty) || seen != 1) return;
    T_BEGIN; classify((uint32_t)open, in, ctx); T_END;
}
// order 1002 (after every other piece): the order rule, then the capture of a member's draw
static void fxb_late(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    if (!g_on) { g_phase = 3; return; }
    const int open = vkn_query_occ_open(), cond = vkn_query_occ_cond();
    const int dk = draw_kind(ctx, out);
    if (g_ncap) {
        // the order rule: Z draws commute with Z captures (the depth minimum; colour: the nearest wins, blending excluded by draw_kind)
        // and with E captures (an E capture's pixel needs its depth to be the final one: a nearer Z draw hides it in either order);
        // E draws commute with E captures (both write only at depth ties); anything else - or an E draw while Z draws wait (it would
        // test against a depth without them) - draws the captures first
        T_BEGIN;
        if (!set_is_pending(ctx)) flush(VKN_OB_F_SET);
        else if (dk != K_ANY && dk != g_pkind && !(g_pkind == K_E && dk == K_Z)) flush(VKN_OB_F_ORDER);
        T_END;
    }
    int why = -1, kind = 0; uint32_t rec = 0; uint16_t gi = 0;
    if (open >= 0 && open < VKN_Q_RECS && g_bp[open].cls == CL_MEMBER) {   // a member's pre-pass draw
        bp *p = &g_bp[open];
        rec = (uint32_t)open; gi = p->gi; kind = K_Z;
        why = p->bad ? VKN_OB_R_SPLIT : dk != K_Z ? VKN_OB_R_ORDER : -2;
        if (why == -2 && (gi >= g_ngrp || g_grp[gi].made)) why = VKN_OB_R_SPLIT;   // its group was drawn already (a pass end inside the pair)
        if (why == -2 && p->ncap == 0) {   // its first capture: the member's draws so far were all captured (it is its first draw)
            uint32_t seen = 0, done = 0; int closed = 0, empty = 0;
            if (!vkn_query_occ_rec(rec, &seen, &done, &closed, &empty) && seen != 1) why = VKN_OB_R_SPLIT;
        }
    } else if (cond >= 0 && cond < VKN_Q_RECS && g_bp[cond].cls == CL_MEMBER) {   // a member's conditional draw
        const bp *p = &g_bp[cond];
        rec = (uint32_t)cond; gi = p->gi; kind = K_E;
        uint32_t seen = 0, done = 0; int closed = 0, empty = 0;
        const int r = vkn_query_occ_rec(rec, &seen, &done, &closed, &empty);
        const grp *g = gi < g_ngrp ? &g_grp[gi] : NULL;
        if (p->bad) why = VKN_OB_R_BAD;
        else if (r || !closed || seen != done || seen != p->ncap) why = VKN_OB_R_OPEN;
        else if (!g || !g->copied) why = VKN_OB_R_NOWORD;
        else if (dk != K_E) why = VKN_OB_R_NOTEQ;
        else why = -2;
    }
    if (why == -2) {
        T_BEGIN;
        if (g_ncap && g_pkind != kind) flush(VKN_OB_F_KIND);
        if (g_ncap >= VKN_OCCB_CAPS || g_nfxo >= FXOS) flush(VKN_OB_F_FULL);
        why = capture(kind, rec, gi, in, ctx, out);
        T_END;
        if (!why) {
            out->skip = 1; g_phase = 0;   // (the draw path returns at once: no pass end can come from this draw)
            if (kind == K_Z) { g_bp[rec].ncap++; vkn_query_occ_skipped(); g_st.cap_pre++; } else g_st.cap_cond++;
            return;
        }
    }
    if (why > 0) {
        if (why < VKN_OB_R_N) g_st.refuse[why]++;
        if (kind == K_Z) g_bp[rec].bad = 1; else g_st.uncond_cond++;   // drawn here, without a condition (as COND=0)
    }
    g_phase = 3;
    if (g_ncap) g_stamp0 = stamp();
}

// ================================================================ the query module's side
void vkn_occb_q_enter(void) { if (!g_replaying) g_phase = 1; }
void vkn_occb_q_draw_post(void) { if (!g_replaying) g_phase = 0; }
void vkn_occb_q_dispatch(void) { if (!g_replaying) g_phase = 0; }
int vkn_occb_replaying(void) { return g_replaying; }
int vkn_occb_q_active(void) { return g_qactive; }
void vkn_occb_pass_closing(VkCommandBuffer cb) {
    (void)cb;
    if (!g_ncap || g_replaying) return;
    // inside the preparation of an engine draw (its texture needs a barrier): the pass breaks for ANOTHER image (an attachment would be
    // a placeholder), the draw begins the same set again right after - the captures wait for the end of that instance
    if (g_phase == 3 && stamp() == g_stamp0) { g_st.postponed++; return; }
    g_in_closing = 1; flush(VKN_OB_F_CLOSE); g_in_closing = 0;
}
void vkn_occb_q_rec_begin(int presented, int slot) {
    g_on = g_mode && presented && slot >= 0;
    g_slot = g_on ? slot : -1;
    g_phase = 0; g_ngrp = 0; g_seg = 0; for (int b = 0; b < VKN_OCCB_BUCKETS; b++) g_cur_gi[b] = -1;
    if (g_ncap) { g_st.lost++; BRED(RD_LOST, "%u captured draws of the last recording were never drawn", g_ncap); }
    g_ncap = 0; g_nfxo = 0; g_pkind = 0;
    if (!g_on) return;
    if (++g_ogen == 0) { memset(g_occ, 0, sizeof g_occ); g_ogen = 1; }
    g_mf++; g_st.frames++;
    g_learn_left = g_budget > 0 ? g_budget : 0; g_checks_left = VKN_OCCB_CHECKS;
    g_smf[slot] = g_mf;
}
void vkn_occb_q_rec_close(void) {
    if (g_ncap && !g_replaying) {   // (the pass module's close listener ends the pass before this one: what is left is drawn now)
        g_phase = 0; flush(VKN_OB_F_REC);
        vkn_pass_end();
    }
    g_on = 0;
}
void vkn_occb_q_pair_begin(uint32_t rec) {
    if (rec >= VKN_Q_RECS) return;
    bp *p = &g_bp[rec]; memset(p, 0, sizeof *p); p->ob = -1; p->gi = 0xffff;
    if (g_on) g_st.pairs++;
}
void vkn_occb_q_slot_taken(int slot) {
    if (slot < 0 || slot >= VKN_Q_SLOTS) return;
    memset(g_bq[slot], 0, sizeof g_bq[slot]); g_nmk[slot] = 0; memset(g_sor[slot], 0, sizeof g_sor[slot]);
}
void vkn_occb_q_copied(int slot, uint32_t q, uint32_t rec, int whole) {
    if (!g_mode || slot < 0 || slot >= VKN_Q_SLOTS || q >= VKN_Q_CAP) return;
    bq *e = &g_bq[slot][q];
    if (rec >= VKN_Q_RECS) {   // B's own query: its group's word is usable now (this recording only)
        if (slot == g_slot && e->gi < g_ngrp) {
            grp *g = &g_grp[e->gi];
            if (e->kind == BQ_GROUP && g->q == (int32_t)q) g->copied = 1;
            if (e->kind == BQ_MEMBER && ++g->mq_copied == g->mq) g->copied = 1;
        }
        return;
    }
    if (slot != g_slot || !whole || g_bp[rec].cls != CL_LEARN) { if (e->kind == BQ_LEARN) e->kind = BQ_NONE; return; }
    e->kind = BQ_LEARN; e->key = g_bp[rec].key; g_st.q_learn++;
}
static void verdict(uint64_t key, int hidden, uint32_t mf) {
    bo *o = bo_find(key);
    if (!o || o->vf > mf) return;
    if (hidden && !o->hid) g_st.joined++;
    if (!hidden && o->hid) g_st.left++;
    o->hid = (uint8_t)hidden; o->check = 0; o->vf = mf;
}
void vkn_occb_q_result(int slot, uint32_t q, uint32_t samples) {
    if (slot < 0 || slot >= VKN_Q_SLOTS || q >= VKN_Q_CAP) return;
    bq *e = &g_bq[slot][q];
    const uint32_t mf = g_smf[slot];
    const int hidden = samples == 0;
    switch (e->kind) {
    case BQ_LEARN: g_st.read_learn++; g_st.read_learn_hidden += (uint64_t)hidden; verdict(e->key, hidden, mf); break;
    case BQ_GROUP: {
        g_st.read_groups++; g_st.culled_groups += (uint64_t)hidden;
        const uint8_t b = e->gi < VKN_OCCB_GROUPS ? g_sbucket[slot][e->gi] : 0;
        if (b < VKN_OCCB_BUCKETS) { if (g_last_bucket[b] >= 0 && g_last_bucket[b] != hidden) g_st.flips++; g_last_bucket[b] = (int8_t)hidden; }
        for (uint32_t i = 0; i < e->mkn && e->mk0 + i < MKEYS; i++) {
            bo *o = bo_find(g_mk[slot][e->mk0 + i]);
            if (!o || o->vf > mf) continue;
            if (hidden) { o->hid = 1; o->vf = mf; } else o->check = 1;   // seen: its members are checked one by one
        }
        break;
    }
    case BQ_MEMBER:
        g_st.member_q_read++;
        if (e->gi < VKN_OCCB_GROUPS) g_sor[slot][e->gi] += samples;
        verdict(e->key, hidden, mf);
        break;
    case BQ_CVER: {
        g_st.cver_read++;
        const uint32_t gor = e->gi < VKN_OCCB_GROUPS ? g_sor[slot][e->gi] : 1;
        if (!gor) {   // B would have discarded this draw under its group's condition
            g_st.verify_judged++;
            if (!samples) g_st.verify_agree++;
            else {
                g_st.violations++;
                BRED(RD_VIOL, "VERIFY VIOLATION: frame %u object %#llx (group %u): its group passed 0 samples in the pre-pass, its G-buffer draw passed %u samples "
                              "(B would have discarded a visible draw)", mf, (unsigned long long)e->key, e->gi, samples);
            }
        }
        break;
    }
    default: break;
    }
    e->kind = BQ_NONE;
}
int vkn_occb_gate(int mode, int gpu_pred) {
    if (mode != VKN_COND_B) return mode;
    const vkn_dev *d = vkn_device();
    const int fam = vkn_policy_family_from(d ? d->props.vendorID : 0, d ? d->props.deviceName : NULL, getenv("DM_GFX_SIM_GPU"));
    if (vkn_policy_tiler(fam) || !gpu_pred) {
        vkn_logf("occ-b: technique B (DM_VK_NATIVE_COND 7 / 9) needs VK_EXT_conditional_rendering on a non-tile GPU (family %s, extension %s): B is off "
                 "(7 -> 0, 9 -> 5; a CPU skip from an older group verdict would not be exact)", vkn_policy_family_name(fam), gpu_pred ? "on" : "off");
        return VKN_COND_OFF;
    }
    return mode;
}
void vkn_occb_init(int mode, int budget) {
    g_mode = mode == VKN_COND_B || mode == VKN_COND_B_VERIFY ? mode : 0;
    g_budget_asked = budget; g_budget = budget >= 0 ? budget : VKN_OCCB_LEARN;
    memset(g_last_bucket, 0xff, sizeof g_last_bucket);
    for (int b = 0; b < VKN_OCCB_BUCKETS; b++) g_cur_gi[b] = -1;
    if (!g_mode) return;
    static int reg;
    if (!reg) reg = !vkn_fx_register("occ-b", fxb_early, NULL, 3) && !vkn_fx_register("occ-b-late", fxb_late, NULL, 1002);
    if (!reg) BRED(RD_GATE, "the effect chain is full: technique B does nothing");
    vkn_logf("occ-b: DM_VK_NATIVE_COND=%d (%s), learning budget %d pairs a frame (%s), %d buckets", g_mode,
             g_mode == VKN_COND_B ? "technique B: group queries and group conditions" : "technique B VERIFY: per-member queries, captured G-buffer draws queried and drawn",
             g_budget, budget >= 0 ? "DM_VK_NATIVE_COND_B" : "default", VKN_OCCB_BUCKETS);
}
// a draw of the engine about to be recorded (vkn_query_draw_pre): the test's sample hook
void vkn_occb_q_draw_pre_test(void) { if (g_t_samples && g_chain_in && !g_replaying) g_t_samples(g_chain_in, g_chain_first, 0); }

// ================================================================ census
void vkn_occb_census(void) {
    if (!g_mode) return;
    static vkn_occb_stats p;
    const vkn_occb_stats a = g_st;
    const double df = a.frames > p.frames ? (double)(a.frames - p.frames) : 1.0;
#define PF(f) ((double)(a.f - p.f) / df)
    uint32_t inh = 0, chk = 0, objs = 0;
    for (uint32_t i = 0; i < BOBJS; i++) { const bo *o = &g_bo[i]; if (!o->key || o->seen + 600 < g_mf) continue; objs++; inh += o->hid && !o->check; chk += o->hid && o->check; }
    char o[2400]; int n = 0; const int cap = (int)sizeof o;
    n += snprintf(o + n, (size_t)(cap - n), "occ-b: DM_VK_NATIVE_COND=%d%s; objects %u (in H %u, to check %u); a frame: pairs %.1f, members %.1f, learners %.1f (checks %.1f); "
                  "groups %.1f; queries %.1f (group %.1f, learning %.1f, member %.1f, G-buffer verify %.1f); conditions %.1f, draws under a condition %.1f (no predicate %.1f); "
                  "captured pre-pass %.1f, G-buffer %.1f; member G-buffer draws at their place %.1f; replays %.1f (dropped %.1f, stale %.1f); flushes %.1f:",
                  g_mode, g_mode == VKN_COND_B_VERIFY ? " (VERIFY)" : "", objs, inh, chk, PF(pairs), PF(members), PF(learners), PF(checks), PF(groups),
                  PF(q_group) + PF(q_learn) + PF(q_member) + PF(q_cver), PF(q_group), PF(q_learn), PF(q_member), PF(q_cver), PF(conds), PF(cond_draws), PF(cond_nopred),
                  PF(cap_pre), PF(cap_cond), PF(uncond_cond), PF(replays), PF(replay_drop), PF(replay_stale), PF(flushes));
    for (int i = 0; i < VKN_OB_F_N && n < cap - 48; i++) if (a.flush_why[i] - p.flush_why[i]) n += snprintf(o + n, (size_t)(cap - n), " %s %.1f,", k_fwhy[i], PF(flush_why[i]));
    n += snprintf(o + n, (size_t)(cap - n), " | members per group [1: %llu, 2-4: %llu, 5-16: %llu, 17-64: %llu, 65-256: %llu, more: %llu]",
                  (unsigned long long)(a.hist[0] - p.hist[0]), (unsigned long long)(a.hist[1] - p.hist[1]), (unsigned long long)(a.hist[2] - p.hist[2]),
                  (unsigned long long)(a.hist[3] - p.hist[3]), (unsigned long long)(a.hist[4] - p.hist[4]), (unsigned long long)(a.hist[5] - p.hist[5]));
    n += snprintf(o + n, (size_t)(cap - n), " | read a frame: groups %.1f, GPU-culled %.1f, flips %.1f; learning verdicts %.1f (hidden %.1f); joined H %.1f, left %.1f",
                  PF(read_groups), PF(culled_groups), PF(flips), PF(read_learn), PF(read_learn_hidden), PF(joined), PF(left));
    n += snprintf(o + n, (size_t)(cap - n), " | refused (member draws, at their place) a frame:");
    for (int i = 1; i < VKN_OB_R_N && n < cap - 64; i++) if (a.refuse[i] - p.refuse[i]) n += snprintf(o + n, (size_t)(cap - n), " %s %.1f,", k_rwhy[i], PF(refuse[i]));
    n += snprintf(o + n, (size_t)(cap - n), " | pass ends postponed %.1f, lost %llu (late replays %llu), clears after a capture %llu",
                  PF(postponed), (unsigned long long)a.lost, (unsigned long long)a.lost_replays, (unsigned long long)a.clear_after);
    if (g_mode == VKN_COND_B_VERIFY)
        n += snprintf(o + n, (size_t)(cap - n), " | VERIFY: G-buffer draws B would discard, judged %llu, agree %llu, VIOLATIONS %llu (total %llu)",
                      (unsigned long long)(a.verify_judged - p.verify_judged), (unsigned long long)(a.verify_agree - p.verify_agree),
                      (unsigned long long)(a.violations - p.violations), (unsigned long long)a.violations);
    n += snprintf(o + n, (size_t)(cap - n), " | recording-thread CPU %.3f ms a frame", (double)(a.cpu_ns - p.cpu_ns) / df / 1e6);
    (void)n;
    vkn_logf("%s", o);
    p = a;
#undef PF
}
#endif
