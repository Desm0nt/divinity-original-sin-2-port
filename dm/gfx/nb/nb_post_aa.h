// [DOS2] NB-18b-8 (route A): the plan of the fused anti-aliasing, DM_NB_POST_AA=1 on top of DM_NB_POST (default 0 = nothing here runs;
// docs/dos2/NATIVE_BACKEND_PLAN.md "NB-18b-8 (route A) findings"). Pure C, header only; host test tests/nb_post_test.c.
//
// Bundle F1A (tools/dos2_post_fuse.py) = FXAA on the pre-LUT image -> LUT -> Sight, drawn at Sight's draw: it replaces F1 (LUT -> Sight)
// AND the console's FXAA pass that follows Sight. The console's FXAA stays ON in the engine: the frame is recorded with its PPFXAA pass,
// and this plan decides per submission whether that pass is elided. So a submission where anything is refused runs the recorded chain
// with the console's FXAA - no frame ever loses anti-aliasing, and no engine setting is flipped (GraphicSettings +0x64 is read while the
// engine builds the frame, long before the plan knows whether the fusion applies; a flip could only take effect a frame or two late).
//
// After nb_post_plan, for every F1 pair it fused (E = PPApplyLUT, H = EoCSight, S = H's target):
//   - the AA step = the first executed node after H that reads S. A pass of class PPFXAA (691768aa) or PPDefault (7b321019: the copy the
//     engine draws instead of FXAA with AntiAliasing 0 at a render scale != 1) is an elidable step when: one draw, one fs, exact reads,
//     no hazards, no clear, one colour attachment O2 != S, it samples S and nothing else, does not keep O2's old texels, O2 and S have
//     one size, and until O2's next full overwrite in this submission nobody writes S, no operation reads O2 and every pass that reads
//     O2 has exact reads (those passes get NB_NP_AA: the backend makes them sample S instead - "alias").
//       FXAA step ok   : E |= AA, H |= AA (the backend draws F1A), step |= ELIDE | AA.
//       FXAA step not ok: nothing (plain F1 + the console's FXAA; reason counted).
//       copy step      : E |= AA, H |= AA; the copy is elided too when ok, else it runs (F1A's output is copied as recorded).
//   - no step (the engine's AA is off and Vignette reads S itself, or nothing reads S): E |= AA, H |= AA.
//   - any other executed PPFXAA pass after H in the submission (an outline / fog step between Sight and FXAA): nothing - that FXAA would
//     run over F1A's output.
// An elided step leaves O2 stale for as long as the fusion goes on (its writers, the LUT and the step, are both elided), so the alias
// outlives the submission: the backend keeps "O2's content is in S" until something really draws into O2 or S, and this plan flags
// EVERY executed pass with exact reads that samples a tracked O2 (st->t, the O2 of the last alias plans) with NB_NP_AA, in every
// submission - a loading screen that re-combines the last 3D image gets it from S. A reader that cannot be flagged (an operation with
// O2 as a source: a copy / readback) marks the image bad for good: no alias for it any more (plain F1 + FXAA from then on).
// Header bits: NB_NP_AA alone = "samples an aliased image"; with ELIDE: E of F1A (class LUT) or the elided step (class FXAA / copy);
// with FUSE: H draws F1A.
#ifndef DM_GFX_NB_POST_AA_H
#define DM_GFX_NB_POST_AA_H
#include <string.h>
#include "nb_ir.h"

enum { NB_PSA_NONE = 0, NB_PSA_FXAA, NB_PSA_COPY };
enum { NB_PAW_ALIAS = 0, NB_PAW_NOSTEP, NB_PAW_OTHER_FXAA, NB_PAW_HOST, NB_PAW_SHAPE, NB_PAW_INEXACT, NB_PAW_INPUT, NB_PAW_KEEPS, NB_PAW_READER,
       NB_PAW_S_WRITTEN, NB_PAW_SIZE, NB_PAW_BAD, NB_PAW_KINDS };
typedef struct nb_post_aa_state { struct { uint64_t image; uint8_t bad; } t[8]; int n; } nb_post_aa_state;   // zero = empty
typedef struct nb_post_aa_out {
    int f1a, f1a_fxaa, f1a_copy, f1a_copy_kept, f1a_nostep;   // F1A hosts; with the FXAA pass elided / the copy elided / the copy left / no AA pass
    int plain;                                                // F1 pairs left as F1 (the console's FXAA runs)
    int readers, op_reads;                                    // passes flagged NB_NP_AA; operations reading a tracked image (now bad)
    int why[NB_PAW_KINDS];
    int16_t step[NB_MAX_NODES];                               // by node: H of the step it is (-1)
} nb_post_aa_out;

static inline int nb_post_aa_class(uint64_t fs) {
    switch ((uint32_t)(fs >> 32)) { case 0x691768aau: return NB_PSA_FXAA; case 0x7b321019u: return NB_PSA_COPY; default: return NB_PSA_NONE; }
}
static inline const char *nb_post_aa_why_name(int w) {
    static const char *const n[NB_PAW_KINDS] = { "AA pass elided", "no AA pass after Sight", "another FXAA pass follows", "host pass shape",
        "AA pass shape", "AA pass reads not exact", "AA pass samples more than Sight's target", "AA pass keeps old texels",
        "a reader of its target cannot be redirected", "Sight's target is written again", "sizes differ", "its target was read by an operation" };
    return w >= 0 && w < NB_PAW_KINDS ? n[w] : "?";
}
static inline int nbpa_has(const uint64_t *v, int n, uint64_t x) { for (int i = 0; i < n; i++) if (v[i] == x) return 1; return 0; }
static inline int nbpa_ir_reads(const nb_frame *f, const nb_node *n, uint64_t x) {
    for (int i = 0; i < n->nref; i++) { const nb_ref *r = &f->ref[n->ref_first + i]; if (r->image == x && r->role != NB_REF_DST) return 1; }
    return 0;
}
static inline int nbpa_reads(const nb_frame *f, const nb_node *n, uint64_t x) {   // exact sampled list when the pass has one, else every IR read
    if (n->kind == NB_NODE_PASS && (n->fsx & NB_FSX_EXACT)) return nbpa_has(n->sread, n->nsr, x);
    return nbpa_ir_reads(f, n, x);
}
static inline int nbpa_op_dst(const nb_frame *f, const nb_node *n, uint64_t x) {
    for (int i = 0; i < n->nref; i++) { const nb_ref *r = &f->ref[n->ref_first + i]; if (r->image == x && r->role == NB_REF_DST) return 1; }
    return 0;
}
static inline int nbpa_op_full(const nb_frame *f, const nb_node *n) {
    if ((n->kind & 0xff) == NB_OP_CLEAR) return n->nclear > 0 && (f->clr[n->clear_first].flags & NB_CLEAR_FULL);
    return (n->kind & NB_OPF_DST_FULL) != 0;
}
// as nb_plan.c pp_keeps_old: LOAD without cover_dc keeps old texels, unless the feedback flag came from stale bindings the exact reads disprove
static inline int nbpa_keeps_old(const nb_frame *f, const nb_plan_out *pl, int k, int a) {
    const nb_node *n = &f->node[k]; const nb_node_plan *np = &pl->node[k];
    (void)f;
    if (!(np->flags & NB_NP_EXEC) || np->load[a] != NB_LOAD || (np->cover_dc >> a & 1)) return 0;
    if ((np->flags & NB_NP_FEEDBACK) && (n->fsx & NB_FSX_EXACT) && n->draws && n->att[a].slot != NB_SLOT_DS) {
        for (int b = 0; b < n->natt; b++) if (nbpa_has(n->sread, n->nsr, n->att[b].image)) return 1;
        return 0;
    }
    return 1;
}
static inline int nbpa_tracked(nb_post_aa_state *st, uint64_t im, int create) {
    for (int i = 0; i < st->n; i++) if (st->t[i].image == im) return i;
    if (!create) return -1;
    if (st->n == (int)(sizeof st->t / sizeof st->t[0])) {   // full: drop the oldest that is not bad (a bad one stays bad)
        int d = -1;
        for (int i = 0; i < st->n && d < 0; i++) if (!st->t[i].bad) d = i;
        if (d < 0) return -1;
        memmove(&st->t[d], &st->t[d + 1], (size_t)(st->n - d - 1) * sizeof st->t[0]); st->n--;
    }
    st->t[st->n].image = im; st->t[st->n].bad = 0;
    return st->n++;
}
// The step k of host i (S = H's target): NB_PAW_ALIAS when it can be elided, else why not.
static inline int nbpa_step(const nb_frame *f, const nb_plan_out *pl, const nb_post_rules *r, nb_post_aa_state *st, int k, uint64_t S) {
    const nb_node *n = &f->node[k];
    if (!(pl->node[k].flags & NB_NP_EXEC) || n->draws != 1 || !(n->fsx & NB_FSX_ANY) || (n->fsx & NB_FSX_MIXED) || n->hz || n->nclear || n->natt != 1 ||
        n->att[0].slot != 0 || (pl->node[k].flags & (NB_NP_ELIDE | NB_NP_FUSE))) return NB_PAW_SHAPE;
    if (!(n->fsx & NB_FSX_EXACT)) return NB_PAW_INEXACT;
    const uint64_t O2 = n->att[0].image;
    if (n->nsr != 1 || n->sread[0] != S || O2 == S) return NB_PAW_INPUT;
    if (nbpa_keeps_old(f, pl, k, 0)) return NB_PAW_KEEPS;
    if (r && r->size) {
        uint32_t w, h, ow, oh;
        if (!r->size(r->ctx, S, &w, &h) || !r->size(r->ctx, O2, &ow, &oh) || w != ow || h != oh) return NB_PAW_SIZE;
    }
    { const int t = nbpa_tracked(st, O2, 0); if (t >= 0 && st->t[t].bad) return NB_PAW_BAD; }
    for (int q = k + 1; q < f->nnode; q++) {
        const nb_node *m = &f->node[q];
        if (m->kind == NB_NODE_PASS) {
            if (!(pl->node[q].flags & NB_NP_EXEC)) continue;
            int rewritten = 0;
            for (int a = 0; a < m->natt; a++) {
                if (m->att[a].image == S) return NB_PAW_S_WRITTEN;
                if (m->att[a].image == O2) { if (nbpa_keeps_old(f, pl, q, a)) return NB_PAW_READER; rewritten = 1; }
            }
            if (!(m->fsx & NB_FSX_EXACT) && nbpa_ir_reads(f, m, O2)) return NB_PAW_READER;
            if (rewritten) { if (nbpa_reads(f, m, O2)) return NB_PAW_READER; break; }
        } else {
            if (nbpa_ir_reads(f, m, O2)) return NB_PAW_READER;
            if (nbpa_op_dst(f, m, S)) return NB_PAW_S_WRITTEN;
            if (nbpa_op_dst(f, m, O2)) { if (!nbpa_op_full(f, m)) return NB_PAW_READER; break; }
        }
    }
    return NB_PAW_ALIAS;
}
// After nb_post_plan (po = its output). Returns the number of F1A hosts; -1 bad input.
static inline int nb_post_aa_plan(const nb_frame *f, nb_plan_out *pl, const nb_post_out *po, const nb_post_rules *r, nb_post_aa_state *st,
                                  nb_post_aa_out *o) {
    if (!f || !pl || !po || !st || !o || f->nnode > NB_MAX_NODES || pl->nnode != f->nnode || po->nnode != f->nnode) return -1;
    memset(o, 0, sizeof *o);
    for (int i = 0; i < f->nnode; i++) o->step[i] = -1;
    // operations that read a tracked image cannot be redirected: bad from now on (before the pairs: a bad image is not aliased again)
    for (int q = 0; q < f->nnode; q++) {
        const nb_node *m = &f->node[q];
        if (m->kind == NB_NODE_PASS) continue;
        for (int t = 0; t < st->n; t++) if (!st->t[t].bad && nbpa_ir_reads(f, m, st->t[t].image)) { st->t[t].bad = 1; o->op_reads++; }
    }
    for (int i = 0; i < f->nnode; i++) {
        if (po->fx[i] != NB_FXK_F1 || !(pl->node[i].flags & NB_NP_FUSE)) continue;
        const nb_node *H = &f->node[i]; const int j = po->peer[i];
        if (j < 0 || H->natt != 1 || H->att[0].slot != 0) { o->why[NB_PAW_HOST]++; o->plain++; continue; }
        const uint64_t S = H->att[0].image;
        int k = -1, other = 0;
        for (int q = i + 1; q < f->nnode; q++) {
            const nb_node *m = &f->node[q];
            if (m->kind == NB_NODE_PASS && !(pl->node[q].flags & NB_NP_EXEC)) continue;
            if (k < 0 && nbpa_reads(f, m, S)) { k = q; continue; }
            if (m->kind == NB_NODE_PASS && m->draws && nb_post_aa_class(m->fs) == NB_PSA_FXAA) other = 1;
        }
        const int cls = k >= 0 && f->node[k].kind == NB_NODE_PASS ? nb_post_aa_class(f->node[k].fs) : NB_PSA_NONE;
        if (other) { o->why[NB_PAW_OTHER_FXAA]++; o->plain++; continue; }
        if (cls == NB_PSA_NONE) {
            pl->node[i].flags |= NB_NP_AA; pl->node[j].flags |= NB_NP_AA; o->f1a++; o->f1a_nostep++; o->why[NB_PAW_NOSTEP]++;
            continue;
        }
        const int w = nbpa_step(f, pl, r, st, k, S);
        o->why[w]++;
        if (w != NB_PAW_ALIAS && cls == NB_PSA_FXAA) { o->plain++; continue; }
        pl->node[i].flags |= NB_NP_AA; pl->node[j].flags |= NB_NP_AA; o->f1a++;
        if (w != NB_PAW_ALIAS) { o->f1a_copy_kept++; continue; }
        pl->node[k].flags |= NB_NP_ELIDE | NB_NP_AA; o->step[k] = (int16_t)i;
        if (cls == NB_PSA_FXAA) o->f1a_fxaa++; else o->f1a_copy++;
        nbpa_tracked(st, f->node[k].att[0].image, 1);
    }
    // Every executed pass with exact reads that samples a tracked image while its content may live in S gets NB_NP_AA (the backend
    // knows whether it does: a flag on a pass whose image is not aliased costs nothing). In stream order: a real write of the image in
    // this submission (a pass that is not elided, an operation) makes it fresh again - later readers are left alone - until its step is
    // elided. Not the elided steps themselves, not a fused F1 / F1A host (its Base binding is gone from the fused module; the LUT's
    // target is the same image as the step's in P2).
    uint8_t fresh[sizeof st->t / sizeof st->t[0]] = { 0 };
    for (int q = 0; q < f->nnode; q++) {
        const nb_node *m = &f->node[q];
        if (m->kind != NB_NODE_PASS) { for (int t = 0; t < st->n; t++) if (nbpa_op_dst(f, m, st->t[t].image)) fresh[t] = 1; continue; }
        if (!(pl->node[q].flags & NB_NP_EXEC)) continue;
        if (o->step[q] >= 0) { for (int t = 0; t < st->n; t++) if (m->att[0].image == st->t[t].image) fresh[t] = 0; continue; }
        if (m->draws && (m->fsx & NB_FSX_EXACT) && !(po->fx[q] == NB_FXK_F1 && (pl->node[q].flags & NB_NP_FUSE)))
            for (int t = 0; t < st->n; t++) if (!st->t[t].bad && !fresh[t] && nbpa_has(m->sread, m->nsr, st->t[t].image)) { pl->node[q].flags |= NB_NP_AA; o->readers++; break; }
        if (!(pl->node[q].flags & NB_NP_ELIDE))
            for (int t = 0; t < st->n; t++) for (int a = 0; a < m->natt; a++) if (m->att[a].image == st->t[t].image) fresh[t] = 1;
    }
    return o->f1a;
}
#endif
