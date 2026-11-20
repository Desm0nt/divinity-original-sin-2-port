// [DOS2] NR-ONCHIP (DM_NR_ONCHIP, docs/dos2/nr/NR-ONCHIP.md §5 / §8): the deferred lighting chain in one render pass. Pure C, header only;
// host test tests/nb_onchip_test.cpp.
//
// Front end (dm/host/hle_dos2_nb.c, after nb_plan / nb_post_plan, before the PASS headers are patched): nb_onchip_plan marks the chains
// in nb_node_plan.oc (copied into p_nb_pass.oc by nvn_nb_tap_patch):
//   S3 = CascadedPCF -> DeferredSky -> ClusteredLightingWithSSAO (frames with shadows), S2 = DeferredSky -> ClusteredLightingWithSSAO.
//   oc = NB_OC_ON | chain length << 2 | member index (nb_oc_mark). Classes by the fragment key (first 8 hex digits, the NR-04 table).
//   A chain is marked only when every member pass is executed, one draw, one fs, exact sampled reads, no NB-16 hazard, no elide / fuse /
//   AA / feedback flag; Sky = 2 colour attachments (slots 0 Sp, 1 Df), Light = 1 (slot 0 = Df), PCF = 1 (slot 0 = S), no depth anywhere;
//   Light samples Sp (and S in S3), not Df; Sky samples neither of its targets; no in-pass clear after a draw, none at all in Light;
//   sizes (when the rules give them): Df at Sky == Df at Light, Sp at Sky >= Df (S at PCF >= Df), see nb_oc_sizes; between Sky and Light
//   only inert nodes (barrier ops, empty passes without clears); between PCF and Sky also the ClearTexture ops the planner folded into Sky.
//   S3 that fails on PCF alone is still marked S2.
// Sizes are those of each pass AS IT WAS BOUND (rules.att_size: the tap's per-pass table, nvn_nb_tap_pass_size). The image-keyed rule
// (rules.size: the tap's last size per storage address) refused every chain on the phone (18.11, D_onchip2 / D_onchip3: "sizes differ"
// on 100 % of the Light passes): the engine's RT pool binds Sky's Sp storage as the half-size bloom target later in the same part, so at
// plan time the table said Sp 1080x486 vs Df 2160x972 (rc1 dump: "# image 0x10cd7cf000 1080x486"). It stays for the host tests only.
// S2F (docs/dos2/nr/NR-FRAME.md §8 / §10; allow bit 2): Sky -> Light -> DeferredFog, the fog subpass writing G-buffer c0 (emissive), which
//   Light then reads as an input attachment (light.s2f) instead of a texture: oc = NB_OC_ON | NB_OC_TAIL | 3 << 2 | k (Sky 0, Light 1,
//   Fog 2; a PCF before Sky stays unmarked: it ran as its own pass under S3 anyway). Fog: one draw, exact, one colour attachment c0 in
//   slot 0, no depth; c0 is not Sp / Df, Light samples c0, Sky does not, Fog samples Df and neither c0 nor Sp; c0 at Fog has Df's size;
//   between Light and Fog only inert nodes and the ClearTexture ops the planner folded into Fog (the engine clears c0 there). The tap
//   writes c0's image id into att[NB_REC_ATT - 1] of Sky's header (natt stays 2): the backend needs it when M begins (nb_oc_tail_image).
// Backend (xbg_nr_onchip.inc): nb_oc_step decides, at every host pass of an IR header, whether the merged render pass begins (Sky = its
// subpass 0), continues, steps to the next subpass (Light = subpass 1) or ends early. S3 executes as PCF + the S2 merge: PCF's
// attachment is not known when the merged pass would have to begin (the stream has not reached PCF's successors), see NR-ONCHIP.md §8.
#ifndef DM_GFX_NB_ONCHIP_H
#define DM_GFX_NB_ONCHIP_H
#include <stdint.h>
#include <string.h>
#include "nb_ir.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { NB_OC_ON = 0x80, NB_OC_K = 0x03, NB_OC_N_SHIFT = 2, NB_OC_N = 0x0c, NB_OC_TAIL = 0x10 };
enum { NB_OCR_NONE = 0, NB_OCR_PCF, NB_OCR_SKY, NB_OCR_LIGHT, NB_OCR_FOG };
static inline uint8_t nb_oc_mark(int n, int k) { return (uint8_t)(NB_OC_ON | ((n & 3) << NB_OC_N_SHIFT) | (k & NB_OC_K)); }
static inline uint8_t nb_oc_mark_tail(int k) { return (uint8_t)(nb_oc_mark(3, k) | NB_OC_TAIL); }   // S2F: Sky 0 / Light 1 / Fog 2
static inline int nb_oc_tail(uint8_t oc) { return (oc & (NB_OC_ON | NB_OC_TAIL)) == (NB_OC_ON | NB_OC_TAIL); }
static inline int nb_oc_len(uint8_t oc) { return (oc & NB_OC_ON) ? (oc & NB_OC_N) >> NB_OC_N_SHIFT : 0; }
static inline int nb_oc_member(uint8_t oc) { return oc & NB_OC_K; }
// The role of a marked member (NB_OCR_NONE: not marked / not a well-formed mark).
static inline int nb_oc_role(uint8_t oc) {
    const int n = nb_oc_len(oc), k = nb_oc_member(oc);
    if (oc & 0x60) return NB_OCR_NONE;
    if (oc & NB_OC_TAIL) return n == 3 ? (k == 0 ? NB_OCR_SKY : k == 1 ? NB_OCR_LIGHT : k == 2 ? NB_OCR_FOG : NB_OCR_NONE) : NB_OCR_NONE;
    if (n == 3) return k == 0 ? NB_OCR_PCF : k == 1 ? NB_OCR_SKY : k == 2 ? NB_OCR_LIGHT : NB_OCR_NONE;
    if (n == 2) return k == 0 ? NB_OCR_SKY : k == 1 ? NB_OCR_LIGHT : NB_OCR_NONE;
    return NB_OCR_NONE;
}
static inline const char *nb_oc_role_name(int r) { return r == NB_OCR_PCF ? "PCF" : r == NB_OCR_SKY ? "Sky" : r == NB_OCR_LIGHT ? "Light" : r == NB_OCR_FOG ? "Fog" : "-"; }

// Fragment classes (dm/gfx/nr/nr_sh.c keys): DeferredSky 640a4c49, ClusteredLightingWithSSAO d236b8d5, CascadedPCF_3x768 / _2x1024 / _3x512.
static inline int nb_oc_class(uint64_t fs) {
    switch ((uint32_t)(fs >> 32)) {
    case 0x640a4c49u: return NB_OCR_SKY;
    case 0xd236b8d5u: return NB_OCR_LIGHT;
    case 0x1372db84u: case 0xa758e42au: case 0x2a221ee2u: return NB_OCR_PCF;
    case 0xd8aeec64u: return NB_OCR_FOG;   // DeferredFog (S2F tail)
    default: return NB_OCR_NONE;
    }
}

// Per Light pass (the candidate): what was marked or why not.
enum { NB_OCW_S3 = 0, NB_OCW_S2, NB_OCW_LIGHT_SHAPE, NB_OCW_NO_SKY, NB_OCW_SKY_SHAPE, NB_OCW_EDGE, NB_OCW_SIZE, NB_OCW_PCF, NB_OCW_SIZE_UNK,
       NB_OCW_S2F, NB_OCW_NO_FOG, NB_OCW_FOG, NB_OCW_KINDS };
static inline const char *nb_oc_why_name(int w) {
    static const char *const n[NB_OCW_KINDS] = { "S3", "S2", "Light pass shape", "no Sky pass right before it", "Sky pass shape",
        "Light does not read Sky out 0 / write Sky out 1", "sizes differ at the passes (Df at Sky != Df at Light, or Sp smaller than Df)",
        "S3 refused on the PCF pass (marked S2)", "a size not known (pass not in the tap's per-pass table)",
        "S2F (Fog tail)", "no Fog pass right after Light (no tail)", "Fog tail refused (shape / reads / size: S2 / S3 kept)" };
    return w >= 0 && w < NB_OCW_KINDS ? n[w] : "?";
}
typedef struct nb_oc_rules {
    int (*size)(void *ctx, uint64_t image, uint32_t *w, uint32_t *h);   // by image (last size per storage: host tests); NULL = none
    void *ctx;
    unsigned allow;                // bit 0 S2, bit 1 S3 (0 = both), bit 2 S2F (the Fog tail; never by the 0 default)
    // by pass: attachment `att` (index in nb_node.att) of node `node` as bound when that pass opened; used instead of `size` when set.
    // With both set, `size` only feeds the diagnostic count table_differ.
    int (*att_size)(void *ctx, int node, int att, uint32_t *w, uint32_t *h);
} nb_oc_rules;
typedef struct nb_oc_out {
    int nnode, s2, s3, s2f, why[NB_OCW_KINDS]; int16_t sky[NB_MAX_NODES / 8], light[NB_MAX_NODES / 8], fog[NB_MAX_NODES / 8]; int nchain;
    int sp_larger;                 // chains marked with Sp at Sky larger than Df (merged framebuffer = Df's size)
    int table_differ;              // chains marked whose Sp / Df differ in the image-keyed table (the old rule would have refused them)
    int ex_why; uint32_t ex[6];    // first size refusal of this part (NB_OCW_SIZE / _UNK, 0 none): Sp at Sky, Df at Sky, Df at Light (w, h)
} nb_oc_out;

static inline int nb_oc_has(const uint64_t *v, int n, uint64_t x) { for (int i = 0; i < n; i++) if (v[i] == x) return 1; return 0; }
// One draw, one exact fs of class cls, executed, no hazard, no flag that changes what runs, natt colour attachments in slots 0..natt-1.
static inline int nb_oc_shape(const nb_frame *f, const nb_plan_out *pl, int i, int cls, int natt) {
    if (i < 0 || i >= f->nnode || i >= pl->nnode) return 0;
    const nb_node *n = &f->node[i]; const uint8_t fl = pl->node[i].flags;
    if (n->kind != NB_NODE_PASS || !(fl & NB_NP_EXEC) || (fl & (NB_NP_ELIDE | NB_NP_FUSE | NB_NP_AA | NB_NP_FEEDBACK)) || pl->node[i].oc) return 0;
    if (n->draws != 1 || n->hz || (n->fsx & (NB_FSX_ANY | NB_FSX_EXACT | NB_FSX_MIXED)) != (NB_FSX_ANY | NB_FSX_EXACT)) return 0;
    if (nb_oc_class(n->fs) != cls || n->natt != natt) return 0;
    for (int a = 0; a < natt; a++) if (n->att[a].slot != a || !n->att[a].image) return 0;
    for (int a = 0; a < natt; a++) for (int b = a + 1; b < natt; b++) if (n->att[a].image == n->att[b].image) return 0;
    for (int c = 0; c < n->nclear; c++) {
        const int ci = n->clear_first + c;
        if (ci < 0 || ci >= f->nclear || f->clr[ci].after_draw) return 0;
    }
    return 1;
}
// Nothing the backend executes: a barrier op, or an empty pass without clears / draws / hazards.
static inline int nb_oc_inert(const nb_frame *f, const nb_plan_out *pl, int i) {
    const nb_node *n = &f->node[i];
    if (n->kind == NB_NODE_PASS) return !(pl->node[i].flags & NB_NP_EXEC) && !n->nclear && !n->draws && !n->hz;
    return (n->kind & 0xff) == NB_OP_BARRIER;
}
// A ClearTexture the planner folded into pass `into`.
static inline int nb_oc_folded_into(const nb_frame *f, const nb_plan_out *pl, int i, int into) {
    const nb_node *n = &f->node[i];
    if ((n->kind & 0xff) != NB_OP_CLEAR || n->nclear <= 0) return 0;
    for (int c = 0; c < n->nclear; c++) {
        const int ci = n->clear_first + c;
        if (ci < 0 || ci >= f->nclear || pl->clr[ci].fate != NB_CLR_FOLDED || pl->clr[ci].into != into) return 0;
    }
    return 1;
}
// The executed pass before node i with only inert nodes (and, with fold_into >= 0, clears folded into that node) between; -1 none.
static inline int nb_oc_prev(const nb_frame *f, const nb_plan_out *pl, int i, int fold_into) {
    for (int j = i - 1; j >= 0; j--) {
        const nb_node *n = &f->node[j];
        if (n->kind == NB_NODE_PASS && (pl->node[j].flags & NB_NP_EXEC)) return j;
        if (nb_oc_inert(f, pl, j)) continue;
        if (fold_into >= 0 && nb_oc_folded_into(f, pl, j, fold_into)) continue;
        return -1;
    }
    return -1;
}
// S2F: the executed pass after node i with only inert nodes and ClearTexture ops folded into it between; -1 none.
static inline int nb_oc_next(const nb_frame *f, const nb_plan_out *pl, int i) {
    for (int j = i + 1; j < f->nnode; j++) {
        const nb_node *n = &f->node[j];
        if (n->kind == NB_NODE_PASS && (pl->node[j].flags & NB_NP_EXEC)) {
            for (int b = i + 1; b < j; b++) if (!nb_oc_inert(f, pl, b) && !nb_oc_folded_into(f, pl, b, j)) return -1;
            return j;
        }
    }
    return -1;
}
static inline int nb_oc_att_size(const nb_oc_rules *r, const nb_frame *f, int i, int a, uint32_t *w, uint32_t *h);
// S2F: the Fog pass q after Light j of the chain s -> j fits the tail (rules in the header comment). 1 = yes.
static inline int nb_oc_fog_fits(const nb_oc_rules *r, const nb_frame *f, const nb_plan_out *pl, int s, int j, int q) {
    if (q < 0 || nb_oc_class(f->node[q].fs) != NB_OCR_FOG || !nb_oc_shape(f, pl, q, NB_OCR_FOG, 1)) return 0;
    const nb_node *S = &f->node[s], *L = &f->node[j], *F = &f->node[q];
    const uint64_t sp = S->att[0].image, df = S->att[1].image, c0 = F->att[0].image;
    if (c0 == sp || c0 == df || !nb_oc_has(L->sread, L->nsr, c0) || nb_oc_has(S->sread, S->nsr, c0)) return 0;
    if (!nb_oc_has(F->sread, F->nsr, df) || nb_oc_has(F->sread, F->nsr, c0) || nb_oc_has(F->sread, F->nsr, sp)) return 0;
    if (r && (r->size || r->att_size)) {
        uint32_t cw, ch, dw, dh;
        if (!nb_oc_att_size(r, f, q, 0, &cw, &ch) || !nb_oc_att_size(r, f, j, 0, &dw, &dh) || cw != dw || ch != dh) return 0;
    }
    return 1;
}
// The tap (nvn_nb_tap_patch): c0's image id for Sky node s of an S2F chain (the next node marked Fog of the tail), 0 none.
static inline uint64_t nb_oc_tail_image(const nb_frame *f, const nb_plan_out *pl, int s) {
    if (!f || !pl || s < 0 || s >= f->nnode || s >= pl->nnode || !nb_oc_tail(pl->node[s].oc) || nb_oc_role(pl->node[s].oc) != NB_OCR_SKY) return 0;
    for (int q = s + 1; q < f->nnode && q < pl->nnode; q++)
        if (nb_oc_tail(pl->node[q].oc) && nb_oc_role(pl->node[q].oc) == NB_OCR_FOG) return f->node[q].natt == 1 ? f->node[q].att[0].image : 0;
    return 0;
}
static inline int nb_oc_same_size(const nb_oc_rules *r, const uint64_t *im, int n) {
    if (!r || !r->size) return 1;
    uint32_t w0 = 0, h0 = 0;
    for (int i = 0; i < n; i++) {
        uint32_t w = 0, h = 0;
        if (!r->size(r->ctx, im[i], &w, &h) || !w || !h) return 0;
        if (!i) { w0 = w; h0 = h; } else if (w != w0 || h != h0) return 0;
    }
    return 1;
}
// Size of attachment a of node i (1 = known, non-zero): the per-pass size when the rules have it, else the image-keyed one.
static inline int nb_oc_att_size(const nb_oc_rules *r, const nb_frame *f, int i, int a, uint32_t *w, uint32_t *h) {
    *w = *h = 0;
    if (r->att_size) return r->att_size(r->ctx, i, a, w, h) && *w && *h;
    return r->size(r->ctx, f->node[i].att[a].image, w, h) && *w && *h;
}
// The size rule of a chain Sky s -> Light j: Df at Sky == Df at Light (Light's render area = the merged pass's area), Sp at Sky >= Df in
// both dimensions (the merged framebuffer is Df's size; Vulkan only needs every attachment >= the framebuffer, and Sky's own pass area
// is min(Sp, Df) = Df already; Light reads Sp at its own pixel). No rules: 0. Returns 0 / NB_OCW_SIZE / NB_OCW_SIZE_UNK; *larger = Sp > Df.
static inline int nb_oc_sizes(const nb_oc_rules *r, const nb_frame *f, int s, int j, nb_oc_out *o, int *larger) {
    *larger = 0;
    if (!r || (!r->size && !r->att_size)) return 0;
    uint32_t v[6] = { 0, 0, 0, 0, 0, 0 };
    const int known = nb_oc_att_size(r, f, s, 0, &v[0], &v[1]) & nb_oc_att_size(r, f, s, 1, &v[2], &v[3]) & nb_oc_att_size(r, f, j, 0, &v[4], &v[5]);
    const int why = !known ? NB_OCW_SIZE_UNK : (v[2] != v[4] || v[3] != v[5] || v[0] < v[2] || v[1] < v[3]) ? NB_OCW_SIZE : 0;
    if (why && !o->ex_why) { o->ex_why = why; memcpy(o->ex, v, sizeof v); }
    *larger = !why && (v[0] > v[2] || v[1] > v[3]);
    return why;
}
// S at PCF p fits a chain whose Df is Df at Light j: S >= Df (Light reads S at its own pixel). No rules: 1.
static inline int nb_oc_pcf_fits(const nb_oc_rules *r, const nb_frame *f, int p, int j) {
    if (!r || (!r->size && !r->att_size)) return 1;
    uint32_t sw, sh, dw, dh;
    return nb_oc_att_size(r, f, p, 0, &sw, &sh) && nb_oc_att_size(r, f, j, 0, &dw, &dh) && sw >= dw && sh >= dh;
}
// After nb_plan (and nb_post_plan). Sets pl->node[*].oc (cleared first). Returns the chains marked; -1 bad input.
static inline int nb_onchip_plan(const nb_frame *f, nb_plan_out *pl, const nb_oc_rules *r, nb_oc_out *o) {
    if (!f || !pl || !o || f->nnode < 0 || f->nnode > NB_MAX_NODES || pl->nnode != f->nnode) return -1;
    memset(o, 0, sizeof *o); o->nnode = f->nnode;
    for (int i = 0; i < f->nnode; i++) pl->node[i].oc = 0;
    const unsigned allow = r && r->allow ? r->allow : 3u;
    for (int j = 0; j < f->nnode; j++) {
        const nb_node *L = &f->node[j];
        if (L->kind != NB_NODE_PASS || !(pl->node[j].flags & NB_NP_EXEC) || nb_oc_class(L->fs) != NB_OCR_LIGHT) continue;
        if (!nb_oc_shape(f, pl, j, NB_OCR_LIGHT, 1) || L->nclear) { o->why[NB_OCW_LIGHT_SHAPE]++; continue; }
        const int s = nb_oc_prev(f, pl, j, -1);
        if (s < 0 || nb_oc_class(f->node[s].fs) != NB_OCR_SKY) { o->why[NB_OCW_NO_SKY]++; continue; }
        if (!nb_oc_shape(f, pl, s, NB_OCR_SKY, 2)) { o->why[NB_OCW_SKY_SHAPE]++; continue; }
        const nb_node *S = &f->node[s];
        const uint64_t sp = S->att[0].image, df = S->att[1].image;
        if (L->att[0].image != df || !nb_oc_has(L->sread, L->nsr, sp) || nb_oc_has(L->sread, L->nsr, df) ||
            nb_oc_has(S->sread, S->nsr, sp) || nb_oc_has(S->sread, S->nsr, df)) { o->why[NB_OCW_EDGE]++; continue; }
        int larger = 0; const int szw = nb_oc_sizes(r, f, s, j, o, &larger);
        if (szw) { o->why[szw]++; continue; }
        // S3: the PCF pass right before Sky (the ClearTexture ops folded into Sky may sit between)
        int p = (allow & 2u) ? nb_oc_prev(f, pl, s, s) : -1, s3 = 0;
        if (p >= 0 && nb_oc_class(f->node[p].fs) == NB_OCR_PCF) {
            const nb_node *P = &f->node[p];
            const uint64_t sh = P->att[0].image;
            if (nb_oc_shape(f, pl, p, NB_OCR_PCF, 1) && sh != sp && sh != df && nb_oc_has(L->sread, L->nsr, sh) &&
                !nb_oc_has(P->sread, P->nsr, sh) && !nb_oc_has(S->sread, S->nsr, sh) && nb_oc_pcf_fits(r, f, p, j)) s3 = 1;
            else o->why[NB_OCW_PCF]++;
        }
        // S2F: the Fog pass right after Light (allow bit 2 only); it wins over S3 (PCF runs as its own pass either way)
        int q = -1;
        if (allow & 4u) {
            q = nb_oc_next(f, pl, j);
            if (q < 0 || nb_oc_class(f->node[q].fs) != NB_OCR_FOG) { o->why[NB_OCW_NO_FOG]++; q = -1; }
            else if (!nb_oc_fog_fits(r, f, pl, s, j, q)) { o->why[NB_OCW_FOG]++; q = -1; }
        }
        if (!s3 && q < 0 && !(allow & 1u)) continue;
        o->sp_larger += larger;
        if (r && r->att_size && r->size) { const uint64_t ims[2] = { sp, df }; o->table_differ += !nb_oc_same_size(r, ims, 2); }   // diagnostic only
        if (q >= 0) { pl->node[s].oc = nb_oc_mark_tail(0); pl->node[j].oc = nb_oc_mark_tail(1); pl->node[q].oc = nb_oc_mark_tail(2); o->s2f++; o->why[NB_OCW_S2F]++; }
        else if (s3) { pl->node[p].oc = nb_oc_mark(3, 0); pl->node[s].oc = nb_oc_mark(3, 1); pl->node[j].oc = nb_oc_mark(3, 2); o->s3++; o->why[NB_OCW_S3]++; }
        else { pl->node[s].oc = nb_oc_mark(2, 0); pl->node[j].oc = nb_oc_mark(2, 1); o->s2++; o->why[NB_OCW_S2]++; }
        if (o->nchain < NB_MAX_NODES / 8) { o->sky[o->nchain] = (int16_t)s; o->light[o->nchain] = (int16_t)j; o->fog[o->nchain] = (int16_t)q; o->nchain++; }
    }
    return o->s2 + o->s3 + o->s2f;
}

/* ---------------- backend stepping (pure; xbg_nr_onchip.inc) ----------------
   The merged render pass M = { Sky's attachments }: subpass 0 = Sky (colours Sp, Df in slots 0 / 1), subpass 1 = Light (colour Df in
   slot 0, input attachment index 6 = Sp: light.s2, binding 52 tAmbient). At every host pass the backend asks with the IR header it is in
   (oc, seq, node; have = a patched header is open): */
enum { NB_OCS_NONE = 0,   // not ours: the normal path (nothing open)
       NB_OCS_BEGIN,      // begin M at this Sky pass (subpass 0)
       NB_OCS_SAME,       // M continues in the current subpass
       NB_OCS_NEXT,       // vkCmdNextSubpass, then M continues (Light = subpass 1)
       NB_OCS_BREAK };    // M ends here (the rest of its subpasses stepped empty, every store kept), then the normal path
typedef struct nb_oc_run { uint8_t active, k; uint32_t seq; int16_t head, light, fog; } nb_oc_run;
static inline int nb_oc_step(const nb_oc_run *r, int have, uint8_t oc, uint32_t seq, int node) {
    const int role = have ? nb_oc_role(oc) : NB_OCR_NONE;
    if (!r->active) return role == NB_OCR_SKY ? NB_OCS_BEGIN : NB_OCS_NONE;
    if (!have || seq != r->seq) return NB_OCS_BREAK;
    if (role == NB_OCR_SKY && node == r->head && r->k == 0) return NB_OCS_SAME;
    if (role == NB_OCR_LIGHT && r->k == 0 && node > r->head) return NB_OCS_NEXT;
    if (role == NB_OCR_LIGHT && r->k == 1 && node == r->light) return NB_OCS_SAME;
    if (role == NB_OCR_FOG && r->k == 1 && nb_oc_tail(oc) && node > r->light) return NB_OCS_NEXT;   // S2F: Fog = subpass 2
    if (role == NB_OCR_FOG && r->k == 2 && node == r->fog) return NB_OCS_SAME;
    return NB_OCS_BREAK;
}
// Apply a decision to the run (after the backend did what it says; begin / next may still be refused by the backend: then it calls
// nb_oc_apply with NB_OCS_BREAK / NB_OCS_NONE instead).
static inline void nb_oc_apply(nb_oc_run *r, int what, uint32_t seq, int node) {
    if (what == NB_OCS_BEGIN) { r->active = 1; r->k = 0; r->seq = seq; r->head = (int16_t)node; r->light = r->fog = -1; }
    else if (what == NB_OCS_NEXT && r->k == 0) { r->k = 1; r->light = (int16_t)node; }
    else if (what == NB_OCS_NEXT && r->k == 1) { r->k = 2; r->fog = (int16_t)node; }
    else if (what == NB_OCS_BREAK) { r->active = 0; r->k = 0; r->head = r->light = r->fog = -1; }
}
// vkCmdNextSubpass calls before vkCmdEndRenderPass of M (n subpasses, now in k): Vulkan ends a render pass in its last subpass only.
static inline int nb_oc_steps_to_end(int n, int k) { return k < n - 1 ? n - 1 - k : 0; }

// Pending clears landed before M (NR-ONCHIP.md §11): the images (texture serials) each member's draw sampled the last time it was checked
// (k 0 = Light, 1 = Fog; M's input attachments and M's own attachments are not in it). Learned in pre(), replaced by every new check of that
// member; at M's begin a pending clear of a learned image lands before M instead of at the member's read (which would end M).
enum { NB_OC_LEARN = 16 };
typedef struct nb_oc_learned { uint64_t s[2][NB_OC_LEARN]; uint8_t n[2]; } nb_oc_learned;
static inline void nb_oc_learn_reset(nb_oc_learned *l, int k) { if (k >= 0 && k < 2) l->n[k] = 0; }
static inline void nb_oc_learn_add(nb_oc_learned *l, int k, uint64_t serial) {
    if (k < 0 || k > 1 || !serial) return;
    for (int i = 0; i < l->n[k]; i++) if (l->s[k][i] == serial) return;
    if (l->n[k] < NB_OC_LEARN) l->s[k][l->n[k]++] = serial;
}
// members: 1 = Light only (M without the Fog subpass), 2 = Light and Fog (S2F)
static inline int nb_oc_learned_has(const nb_oc_learned *l, int members, uint64_t serial) {
    if (!serial) return 0;
    for (int k = 0; k < members && k < 2; k++) for (int i = 0; i < l->n[k]; i++) if (l->s[k][i] == serial) return 1;
    return 0;
}

// The subpass-input variables of a SPIR-V module (OpTypeImage Dim SubpassData): binding / input_attachment_index pairs, at most cap;
// returns how many (-1: malformed). The backend checks the light.s2 module it built: exactly {binding 52: index 6}.
static inline int nb_oc_spv_inputs(const uint32_t *w, size_t n, uint32_t *binding, uint32_t *index, int cap) {
    enum { CAP = 64 };
    uint32_t img[CAP], ptr_id[CAP], ptr_ty[CAP], var_id[CAP], var_ty[CAP]; int ni = 0, np = 0, nv = 0, out = 0;
    if (!w || n < 5 || w[0] != 0x07230203u) return -1;
    for (size_t i = 5; i < n;) {   // pass 1: types and variables
        const uint32_t len = w[i] >> 16, op = w[i] & 0xffffu;
        if (!len || i + len > n) return -1;
        if (op == 25 && len >= 4 && w[i + 3] == 6 && ni < CAP) img[ni++] = w[i + 1];                          // OpTypeImage, Dim SubpassData
        else if (op == 32 && len >= 4 && np < CAP) { ptr_id[np] = w[i + 1]; ptr_ty[np++] = w[i + 3]; }         // OpTypePointer
        else if (op == 59 && len >= 4 && nv < CAP) { var_ty[nv] = w[i + 1]; var_id[nv++] = w[i + 2]; }        // OpVariable
        i += len;
    }
    for (int v = 0; v < nv; v++) {
        int sub = 0;
        for (int p = 0; p < np && !sub; p++) if (ptr_id[p] == var_ty[v]) for (int k = 0; k < ni; k++) if (img[k] == ptr_ty[p]) sub = 1;
        if (!sub) continue;
        uint32_t b = ~0u, ix = ~0u;
        for (size_t i = 5; i < n;) {   // OpDecorate <var> Binding (33) / InputAttachmentIndex (43)
            const uint32_t len = w[i] >> 16, op = w[i] & 0xffffu;
            if (op == 71 && len >= 4 && w[i + 1] == var_id[v]) { if (w[i + 2] == 33) b = w[i + 3]; else if (w[i + 2] == 43) ix = w[i + 3]; }
            i += len;
        }
        if (out < cap) { binding[out] = b; index[out] = ix; }
        out++;
    }
    return out;
}

/* The front end's switch: DM_NR_ONCHIP as the backend resolved it (env / family default), published by the backend at init
   (dm_nr_onchip_publish); the front end plans while it is > 0. Defined in dm/host/hle_dos2_nb.c. */
void dm_nr_onchip_publish(int mode);

#ifdef __cplusplus
}
#endif
#endif
