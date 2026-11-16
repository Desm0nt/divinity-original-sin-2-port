// [dos2-nb] NB-02: frame IR builder, load/store planner, text dump / parse. See nb_ir.h and docs/dos2/NATIVE_BACKEND_PLAN.md §4.
#include "nb_ir.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *const kind_names[NB_OP_KINDS] = { "pass", "copy", "blit", "resolve", "compute", "barrier", "readback", "present", "clear", "fence" };
static const char *const end_names[NB_END_KINDS] = { "open", "rt", "clear", "op", "compute", "barrier", "submit" };
const char *nb_kind_name(int kind) { kind &= 0xff; return kind >= 0 && kind < NB_OP_KINDS ? kind_names[kind] : "?"; }
const char *nb_end_name(int reason) { return reason >= 0 && reason < NB_END_KINDS ? end_names[reason] : "?"; }

/* ---------------- building ---------------- */
void nb_frame_reset(nb_frame *f, uint32_t frame_no) {
    f->frame_no = frame_no; f->final = 1; f->open = 0; f->last_natt = 0;
    f->nnode = f->nref = f->nclear = 0; f->overflow = f->stray = 0;
    f->bar_pending = 0; f->bar_kept = f->bar_split = 0; f->track = 0;
}

static nb_node *new_node(nb_frame *f, int kind) {
    if (f->nnode >= NB_MAX_NODES) { f->overflow++; return NULL; }
    nb_node *n = &f->node[f->nnode++];
    memset(n, 0, sizeof *n);
    n->kind = (uint16_t)kind; n->clear_first = (int16_t)f->nclear; n->ref_first = f->nref;
    return n;
}

void nb_pass_end(nb_frame *f, int reason) {
    if (!f->open) return;
    f->node[f->nnode - 1].end = (uint8_t)reason;
    f->open = 0; f->bar_pending = 0;
}

int nb_pass_begin(nb_frame *f, const nb_att *rt, int n) {
    nb_pass_end(f, NB_END_RT);
    if (n < 0) n = 0;
    if (n > NB_MAX_ATT) { f->overflow++; n = NB_MAX_ATT; }
    nb_node *p = new_node(f, NB_NODE_PASS);
    f->last_natt = (uint8_t)n;
    if (n) memcpy(f->last_att, rt, (size_t)n * sizeof *rt);
    if (!p) return -1;
    p->natt = (uint8_t)n;
    if (n) memcpy(p->att, rt, (size_t)n * sizeof *rt);
    f->open = 1;
    return f->nnode - 1;
}

static int att_index(const nb_node *p, uint64_t image) {
    for (int i = 0; i < p->natt; i++) if (p->att[i].image == image) return i;
    return -1;
}
static int last_att_has(const nb_frame *f, uint64_t image) {
    for (int i = 0; i < f->last_natt; i++) if (f->last_att[i].image == image) return 1;
    return 0;
}
static int reopen(nb_frame *f) {          // RT set still bound after an operation closed its pass
    if (f->open) return 1;
    if (!f->last_natt) { f->stray++; return 0; }
    nb_att tmp[NB_MAX_ATT];
    int n = f->last_natt;
    memcpy(tmp, f->last_att, (size_t)n * sizeof *tmp);
    return nb_pass_begin(f, tmp, n) >= 0;
}

static nb_clear_rec *new_clear(nb_frame *f, uint64_t image, unsigned aspects, unsigned flags, const uint32_t color[4], float depth,
                               uint32_t stencil) {
    if (f->nclear >= NB_MAX_CLEARS) { f->overflow++; return NULL; }
    nb_clear_rec *c = &f->clr[f->nclear++];
    memset(c, 0, sizeof *c);
    c->image = image; c->aspects = (uint8_t)aspects; c->flags = (uint8_t)flags; c->depth = depth; c->stencil = stencil;
    if (color) memcpy(c->color, color, sizeof c->color);
    c->node = (int16_t)(f->nnode - 1);
    return c;
}

void nb_clear(nb_frame *f, uint64_t image, unsigned aspects, unsigned flags, const uint32_t color[4], float depth, uint32_t stencil) {
    if ((flags & NB_CLEAR_RT) && !f->open && last_att_has(f, image)) reopen(f);
    if (f->open && att_index(&f->node[f->nnode - 1], image) >= 0 && (flags & NB_CLEAR_RT)) {
        nb_node *p = &f->node[f->nnode - 1];
        if (p->clear_first + p->nclear != f->nclear) { f->overflow++; return; }   // cannot happen: clears are appended to the open pass
        nb_clear_rec *c = new_clear(f, image, aspects, flags, color, depth, stencil);
        if (!c) return;
        c->after_draw = p->draws > 0;
        p->nclear++;
        return;
    }
    // Texture clear (or a clear of an image that is not bound): an operation outside passes.
    nb_pass_end(f, NB_END_CLEAR);
    nb_node *n = new_node(f, NB_OP_CLEAR | ((flags & NB_CLEAR_FULL) ? NB_OPF_DST_FULL : 0));
    if (!n) return;
    if (!new_clear(f, image, aspects, flags, color, depth, stencil)) { f->nnode--; return; }
    n->nclear = 1;
    if (f->nref < NB_MAX_REFS) { f->ref[f->nref].image = image; f->ref[f->nref].role = NB_REF_DST; f->nref++; n->nref = 1; }
    else f->overflow++;
}

void nb_draw(nb_frame *f, uint32_t cover_slots) {
    if (!reopen(f)) return;
    nb_node *p = &f->node[f->nnode - 1];
    if (p->draws == 0) p->cover = cover_slots;
    p->draws++;
}

void nb_draw_prog(nb_frame *f, uint64_t fs, int exact, const uint64_t *sampled, int n) {   // NB-18b-4a
    if (!f->open) return;
    nb_node *p = &f->node[f->nnode - 1];
    if (!(p->fsx & NB_FSX_ANY)) { p->fsx = (uint8_t)(NB_FSX_ANY | (exact ? NB_FSX_EXACT : 0)); p->fs = fs; }
    else { if (fs != p->fs) p->fsx |= NB_FSX_MIXED; if (!exact) p->fsx &= (uint8_t)~NB_FSX_EXACT; }
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < p->nsr && p->sread[k] != sampled[i]) k++;
        if (k < p->nsr || !sampled[i]) continue;
        if (p->nsr < NB_MAX_SREAD) p->sread[p->nsr++] = sampled[i]; else p->fsx &= (uint8_t)~NB_FSX_EXACT;
    }
}

int nb_read(nb_frame *f, int pass, uint64_t image) {
    if (pass < 0) pass = f->open ? f->nnode - 1 : -1;
    if (pass < 0 || !f->open || pass != f->nnode - 1) { f->stray++; return -1; }
    nb_node *p = &f->node[pass];
    for (int i = 0; i < p->nref; i++) if (f->ref[p->ref_first + i].image == image) return 0;
    if (f->nref >= NB_MAX_REFS) { f->overflow++; return -1; }
    f->ref[f->nref].image = image; f->ref[f->nref].role = NB_REF_READ; f->nref++; p->nref++;
    return 0;
}

int nb_op(nb_frame *f, int kind, const uint64_t *src, int nsrc, const uint64_t *dst, int ndst) {
    int k = kind & 0xff;
    if (k <= NB_NODE_PASS || k >= NB_OP_KINDS || k == NB_OP_CLEAR || k == NB_OP_FENCE) return -1;
    nb_pass_end(f, k == NB_OP_COMPUTE ? NB_END_COMPUTE : k == NB_OP_BARRIER ? NB_END_BARRIER : NB_END_OP);
    nb_node *n = new_node(f, kind & (0xff | NB_OPF_DST_FULL));
    if (!n) return -1;
    for (int pass = 0; pass < 2; pass++) {
        const uint64_t *v = pass ? dst : src; int c = pass ? ndst : nsrc;
        for (int i = 0; v && i < c; i++) {
            if (f->nref >= NB_MAX_REFS) { f->overflow++; break; }
            f->ref[f->nref].image = v[i]; f->ref[f->nref].role = (uint8_t)(pass ? NB_REF_DST : NB_REF_SRC); f->nref++; n->nref++;
        }
    }
    return f->nnode - 1;
}

int nb_barrier(nb_frame *f) {
    if (!f->open) return nb_op(f, NB_OP_BARRIER, NULL, 0, NULL, 0);
    nb_node *p = &f->node[f->nnode - 1];
    if (p->nbar < UINT16_MAX) p->nbar++;
    f->bar_pending = 1; f->bar_kept++;
    return f->nnode - 1;
}

int nb_barrier_hazard(nb_frame *f, uint64_t image) {
    if (!f->open || !f->bar_pending || att_index(&f->node[f->nnode - 1], image) < 0) return 0;
    f->bar_split++;
    nb_op(f, NB_OP_BARRIER, NULL, 0, NULL, 0);   // ends the pass (NB_END_BARRIER), the barrier node sits between the two passes
    return 1;
}

void nb_hazard(nb_frame *f, unsigned hz) {
    hz &= NB_HZ_ALL;
    if (!hz) return;
    if (f->open) { f->node[f->nnode - 1].hz |= (uint8_t)hz; return; }
    if (f->nnode && (f->node[f->nnode - 1].kind & 0xff) == NB_OP_FENCE) { f->node[f->nnode - 1].hz |= (uint8_t)hz; return; }
    nb_node *n = new_node(f, NB_OP_FENCE);
    if (n) n->hz = (uint8_t)hz;
}
void nb_track(nb_frame *f, unsigned track) { f->track = (uint8_t)(track & NB_TRACK_MERGE); }

void nb_submit(nb_frame *f, int final_in_frame) {
    nb_pass_end(f, NB_END_SUBMIT);
    f->final = final_in_frame ? 1 : 0;
}

/* ---------------- persistent attributes ---------------- */
void nb_persist_reset(nb_persist *p) { memset(p, 0, sizeof *p); }

static uint32_t hash64(uint64_t v) { v ^= v >> 33; v *= 0xff51afd7ed558ccdULL; v ^= v >> 33; return (uint32_t)v; }

static nb_img_state *persist_get(nb_persist *p, uint64_t image, int create) {
    uint32_t h = hash64(image) & (NB_PERSIST_CAP - 1);
    for (int i = 0; i < NB_PERSIST_CAP; i++) {
        nb_img_state *e = &p->e[(h + i) & (NB_PERSIST_CAP - 1)];
        if (e->used && e->image == image) return e;
        if (!e->used) {
            if (!create || p->count >= NB_PERSIST_CAP * 3 / 4) { if (create) p->full++; return NULL; }
            memset(e, 0, sizeof *e); e->used = 1; e->image = image; p->count++;
            return e;
        }
    }
    return NULL;
}
const nb_img_state *nb_persist_find(const nb_persist *p, uint64_t image) { return persist_get((nb_persist *)p, image, 0); }
void nb_persist_forget(nb_persist *p, uint64_t image) {
    nb_img_state *e = persist_get(p, image, 0);
    if (e) { e->wset = 0; e->wframe = 0; e->xread = 0; e->ever_read = 0; e->trans_streak = 0; }
}

/* ---------------- planner ---------------- */
enum { NEXT_NONE = 0, NEXT_READ = 1, NEXT_OVERWRITE = 2 };

typedef struct plan_ctx {
    const nb_frame *f; nb_persist *p; const nb_rules *r; nb_plan_out *o;
    uint32_t wclock, barclock;     // NB-05c: write clock of this submission; its value at the last barrier point (NB_NP_PREBAR)
} plan_ctx;
static void wrote(plan_ctx *c, nb_plan_scratch *s) { s->w = 1; s->wseq = ++c->wclock; }   // a GPU write of s in this submission

static nb_plan_scratch *img(plan_ctx *c, uint64_t image) {
    nb_plan_out *o = c->o;
    // Frame-local open-addressing table on top of the scratch array (size is a power of two).
    uint32_t h = hash64(image) & (NB_MAX_FRAME_IMAGES - 1);
    for (int i = 0; i < NB_MAX_FRAME_IMAGES; i++) {
        nb_plan_scratch *s = &o->img[(h + i) & (NB_MAX_FRAME_IMAGES - 1)];
        if (s->ps && s->image == image) return s;
        if (!s->ps) {
            if (o->nimg >= NB_MAX_FRAME_IMAGES - 1) return NULL;
            memset(s, 0, sizeof *s);
            s->image = image; s->pend = -1; o->nimg++;
            nb_img_state *e = c->p ? persist_get(c->p, image, 1) : NULL;
            if (e) {
                s->ps = e; s->persist = 1;
                s->w = e->wset && e->wframe == c->f->frame_no;   // written by an earlier submission of this frame
            } else {
                // No history (persist NULL): fresh local state. Persist table full: conservative (cross-frame reader, written).
                s->loc.image = image; s->ps = &s->loc;
                if (c->p) { s->loc.xread = 1; s->loc.ever_read = 1; s->w = 1; }
            }
            s->att_ok = 1;
            return s;
        }
    }
    return NULL;
}

// A pending full clear on s must happen now (something touches the image before a pass could absorb it).
static void materialize(plan_ctx *c, nb_plan_scratch *s) {
    if (s->pend < 0) return;
    c->o->clr[s->pend].fate = NB_CLR_STANDALONE;
    s->pend = -1; wrote(c, s);
}
static void kill_pending(plan_ctx *c, nb_plan_scratch *s) {
    if (s->pend < 0) return;
    c->o->clr[s->pend].fate = NB_CLR_DEAD;
    s->pend = -1;
}
static void read_before(plan_ctx *c, nb_plan_scratch *s, int sample) {   // old texels of s are consumed here
    materialize(c, s);
    if (!s->w && !s->ps->xread) { s->ps->xread = 1; c->o->st.xread_new++; }
    if (sample) { s->ps->ever_read = 1; s->rd = 1; }
}
static void full_clear(plan_ctx *c, nb_plan_scratch *s, int ci) {   // full clear that may fold into a later pass
    kill_pending(c, s);
    s->pend = (int16_t)ci;
}

int nb_plan(const nb_frame *f, nb_persist *p, const nb_rules *r, nb_plan_out *o) {
    if (!f || !r || !o || f->nnode < 0 || f->nnode > NB_MAX_NODES) return -1;
    plan_ctx cx = { f, p, r, o, 0, 0 }, *c = &cx;
    memset(&o->st, 0, sizeof o->st);
    memset(o->img, 0, sizeof o->img);
    o->nimg = 0; o->nnode = f->nnode; o->ntransient = 0;
    const int ipad = r->mode == NB_RULES_IPAD;
    for (int i = 0; i < f->nclear; i++) { o->clr[i].fate = NB_CLR_PENDING; o->clr[i].into = -1; }

    /* Forward sweep: loads, clear folding, written-this-frame, xread. */
    for (int ni = 0; ni < f->nnode; ni++) {
        const nb_node *n = &f->node[ni];
        nb_node_plan *np = &o->node[ni];
        memset(np, 0, sizeof *np);
        for (int k = 0; k < NB_MAX_ATT; k++) np->clear[k] = -1;
        const nb_ref *refs = &f->ref[n->ref_first];
        if (n->kind == NB_NODE_PASS) {
            o->st.passes++;
            if (n->draws == 0) {
                if (n->nclear == 0) { np->flags = NB_NP_EMPTY; o->st.empty++; continue; }
                np->flags = NB_NP_CLEAR_ONLY; o->st.clear_only++;
                for (int i = 0; i < n->nclear; i++) {
                    int ci = n->clear_first + i;
                    nb_plan_scratch *s = img(c, f->clr[ci].image); if (!s) return -1;
                    if (f->clr[ci].flags & NB_CLEAR_FULL) full_clear(c, s, ci);
                    else { read_before(c, s, 0); o->clr[ci].fate = NB_CLR_STANDALONE; wrote(c, s); }
                }
                continue;
            }
            np->flags = NB_NP_EXEC; o->st.exec++;
            for (int i = 0; i < n->nref; i++) {          // texture reads of other images
                if (att_index(n, refs[i].image) >= 0) { np->flags |= NB_NP_FEEDBACK; continue; }
                nb_plan_scratch *s = img(c, refs[i].image); if (!s) return -1;
                read_before(c, s, 1);                    // (a pending clear materialized here is a write before the pass too)
                if (s->wseq > c->barclock) np->flags |= NB_NP_PREBAR;
            }
            if (np->flags & NB_NP_PREBAR) { c->barclock = c->wclock; o->st.prebar++; }   // one barrier before the pass covers every write
            for (int k = 0; k < n->natt; k++) {
                nb_plan_scratch *s = img(c, n->att[k].image); if (!s) return -1;
                s->used_att = 1;
                int own = -1;                            // last full clear of this attachment before the first draw
                for (int i = 0; i < n->nclear; i++) {
                    const nb_clear_rec *cr = &f->clr[n->clear_first + i];
                    if (cr->image != n->att[k].image) continue;
                    o->clr[n->clear_first + i].fate = NB_CLR_INPASS;
                    if (!cr->after_draw && (cr->flags & NB_CLEAR_FULL)) {
                        if (own >= 0) o->clr[own].fate = NB_CLR_DEAD;
                        own = n->clear_first + i;
                    }
                }
                int feedback = 0;
                for (int i = 0; i < n->nref; i++) if (refs[i].image == n->att[k].image) feedback = 1;
                int slot = n->att[k].slot;
                uint8_t ld;
                if (own >= 0) {
                    kill_pending(c, s); ld = NB_LOAD_CLEAR; np->clear[k] = (int16_t)own; o->clr[own].fate = NB_CLR_FOLDED;
                    o->clr[own].into = (int16_t)ni;
                } else if (!ipad && slot < 32 && (n->cover >> slot & 1) && !feedback) {
                    kill_pending(c, s); ld = NB_LOAD_DONT_CARE;
                } else if (s->pend >= 0 && !feedback) {
                    ld = NB_LOAD_CLEAR; np->clear[k] = s->pend; o->clr[s->pend].fate = NB_CLR_FOLDED; o->clr[s->pend].into = (int16_t)ni;
                    o->st.folded++; s->pend = -1;
                } else if (ipad) {
                    materialize(c, s);
                    ld = s->w ? NB_LOAD : NB_LOAD_DONT_CARE;
                } else {
                    materialize(c, s);
                    if (feedback || s->w || s->ps->xread || !(r->flags & NB_RULEF_FRESH)) {
                        ld = NB_LOAD;
                        if (!s->w && !s->ps->xread) { s->ps->xread = 1; o->st.xread_new++; }   // loaded before its first write
                    } else ld = NB_LOAD_DONT_CARE;
                }
                if (feedback) { s->ps->ever_read = 1; s->rd = 1; }
                if (ld == NB_LOAD && !ipad && !feedback && slot < NB_SLOT_DS) np->cover_dc |= (uint16_t)(1u << k);
                np->load[k] = ld;
                if (ld == NB_LOAD) s->att_ok = 0;
            }
            for (int k = 0; k < n->natt; k++) { nb_plan_scratch *s = img(c, n->att[k].image); if (s) wrote(c, s); }
        } else {
            int kind = n->kind & 0xff, dfull = (n->kind & NB_OPF_DST_FULL) != 0;
            if (kind == NB_OP_CLEAR) {
                int ci = n->clear_first;
                nb_plan_scratch *s = img(c, f->clr[ci].image); if (!s) return -1;
                if (dfull) full_clear(c, s, ci);
                else { read_before(c, s, 0); o->clr[ci].fate = NB_CLR_STANDALONE; wrote(c, s); }
                continue;
            }
            for (int i = 0; i < n->nref; i++) if (refs[i].role == NB_REF_SRC) {
                nb_plan_scratch *s = img(c, refs[i].image); if (!s) return -1;
                s->other = 1; read_before(c, s, 1);
            }
            for (int i = 0; i < n->nref; i++) if (refs[i].role == NB_REF_DST) {
                nb_plan_scratch *s = img(c, refs[i].image); if (!s) return -1;
                s->other = 1;
                if (dfull) kill_pending(c, s); else read_before(c, s, 0);
                wrote(c, s);
            }
        }
    }
    for (int i = 0; i < NB_MAX_FRAME_IMAGES; i++) if (o->img[i].ps) materialize(c, &o->img[i]);   // end of submission: plain clear

    /* Backward sweep: stores from the next access of each attachment. A submission that lost records (a full node / ref / clear
     * table: overflow; a draw or read with no pass: stray) may lack the reader of an attachment after its pass: no store DONT_CARE
     * in it at all (16.11 ab7rp3: frames with overflow 263, DM_NB_RP=3 discarded the main depth that a pass past the cap read). */
    const int lost = f->overflow || f->stray;
    for (int i = 0; i < NB_MAX_FRAME_IMAGES; i++) o->img[i].next = NEXT_NONE;
    for (int ni = f->nnode - 1; ni >= 0; ni--) {
        const nb_node *n = &f->node[ni];
        nb_node_plan *np = &o->node[ni];
        const nb_ref *refs = &f->ref[n->ref_first];
        if (n->kind == NB_NODE_PASS) {
            if (np->flags & NB_NP_EMPTY) continue;
            if (np->flags & NB_NP_CLEAR_ONLY) {
                for (int i = n->nclear - 1; i >= 0; i--) {
                    int ci = n->clear_first + i;
                    if (o->clr[ci].fate != NB_CLR_STANDALONE) continue;
                    nb_plan_scratch *s = img(c, f->clr[ci].image);
                    if (s) s->next = (f->clr[ci].flags & NB_CLEAR_FULL) ? NEXT_OVERWRITE : NEXT_READ;
                }
                continue;
            }
            for (int k = 0; k < n->natt; k++) {
                nb_plan_scratch *s = img(c, n->att[k].image); if (!s) return -1;
                int depth = n->att[k].slot == NB_SLOT_DS;
                uint8_t st;
                if (s->next == NEXT_READ) st = NB_STORE;
                else if (s->next == NEXT_OVERWRITE) st = (ipad && !depth) ? NB_STORE : NB_STORE_DONT_CARE;
                else st = (depth && f->final && !s->ps->xread) ? NB_STORE_DONT_CARE : NB_STORE;
                if (lost && st == NB_STORE_DONT_CARE) { st = NB_STORE; o->st.store_lost++; }
                // Duplicate attachment of one image in a pass: keep the first decision consistent.
                for (int j = 0; j < k; j++) if (n->att[j].image == n->att[k].image) st = np->store[j];
                np->store[k] = st;
                if (st == NB_STORE) s->att_ok = 0;
            }
            for (int k = 0; k < n->natt; k++) {
                nb_plan_scratch *s = img(c, n->att[k].image);
                if (s) s->next = np->load[k] == NB_LOAD ? NEXT_READ : NEXT_OVERWRITE;
            }
            for (int i = 0; i < n->nref; i++) { nb_plan_scratch *s = img(c, refs[i].image); if (s) s->next = NEXT_READ; }
        } else {
            int kind = n->kind & 0xff, dfull = (n->kind & NB_OPF_DST_FULL) != 0;
            if (kind == NB_OP_CLEAR) {
                int ci = n->clear_first;
                if (o->clr[ci].fate == NB_CLR_STANDALONE) {
                    nb_plan_scratch *s = img(c, f->clr[ci].image);
                    if (s) s->next = dfull ? NEXT_OVERWRITE : NEXT_READ;
                }
                continue;
            }
            for (int i = 0; i < n->nref; i++) if (refs[i].role == NB_REF_DST) {
                nb_plan_scratch *s = img(c, refs[i].image); if (s) s->next = dfull ? NEXT_OVERWRITE : NEXT_READ;
            }
            for (int i = 0; i < n->nref; i++) if (refs[i].role == NB_REF_SRC) {
                nb_plan_scratch *s = img(c, refs[i].image); if (s) s->next = NEXT_READ;
            }
        }
    }

    /* Statistics, persistent state, transient candidates. */
    for (int ni = 0; ni < f->nnode; ni++) {
        const nb_node *n = &f->node[ni]; const nb_node_plan *np = &o->node[ni];
        if (n->kind != NB_NODE_PASS || !(np->flags & NB_NP_EXEC)) continue;
        for (int k = 0; k < n->natt; k++) {
            if (np->load[k] == NB_LOAD_CLEAR) o->st.load_clear++;
            else if (np->load[k] == NB_LOAD_DONT_CARE) o->st.load_dc++;
            else o->st.load_load++;
            if (np->store[k] == NB_STORE_DONT_CARE) o->st.store_dc++; else o->st.store_store++;
        }
    }
    for (int i = 0; i < f->nclear; i++) {
        if (o->clr[i].fate == NB_CLR_STANDALONE) {   // a transfer clear: the image cannot be transient
            o->st.standalone++;
            nb_plan_scratch *s = img(c, f->clr[i].image);
            if (s) s->other = 1;
        }
        else if (o->clr[i].fate == NB_CLR_DEAD) o->st.dead++;
    }
    int tf = r->transient_frames > 0 ? r->transient_frames : 8;
    for (int i = 0; i < NB_MAX_FRAME_IMAGES; i++) {
        nb_plan_scratch *s = &o->img[i];
        if (!s->ps) continue;
        o->st.images++;
        if (!s->persist) continue;
        if (s->w) { s->ps->wset = 1; s->ps->wframe = f->frame_no; }
        if (!f->final || ipad) continue;
        if (s->used_att && s->att_ok && !s->rd && !s->other && !s->ps->ever_read && !s->ps->xread) {
            if (s->ps->trans_streak < 0xffff) s->ps->trans_streak++;
        } else if (s->used_att || s->rd || s->other) s->ps->trans_streak = 0;
        if (s->used_att && s->ps->trans_streak >= tf && o->ntransient < (int)(sizeof o->transient / sizeof o->transient[0]))
            o->transient[o->ntransient++] = s->image;
    }
    o->st.transient = o->ntransient;
    return 0;
}

/* ---------------- text form ---------------- */
typedef struct sink { char *buf; size_t cap, len; } sink;
static void put(sink *k, const char *fmt, ...) {
    char line[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    if (k->buf && k->len < k->cap) {
        size_t room = k->cap - k->len, m = (size_t)n < room ? (size_t)n : room;
        memcpy(k->buf + k->len, line, m);
    }
    k->len += (size_t)n;
    if (k->buf && k->cap) k->buf[k->len < k->cap ? k->len : k->cap - 1] = 0;
}
static const char *slot_name(int slot, char *tmp) { if (slot == NB_SLOT_DS) return "ds"; tmp[0] = 'c'; tmp[1] = (char)('0' + slot); tmp[2] = 0; return tmp; }
static void put_clear(sink *k, const nb_clear_rec *c) {
    char asp[4]; int a = 0;
    if (c->aspects & NB_ASP_COLOR) asp[a++] = 'c';
    if (c->aspects & NB_ASP_DEPTH) asp[a++] = 'd';
    if (c->aspects & NB_ASP_STENCIL) asp[a++] = 's';
    if (!a) asp[a++] = '-';
    asp[a] = 0;
    put(k, "clear 0x%llx asp=%s full=%d rt=%d v=%08x,%08x,%08x,%08x d=%.9g s=%u\n", (unsigned long long)c->image, asp,
        (c->flags & NB_CLEAR_FULL) ? 1 : 0, (c->flags & NB_CLEAR_RT) ? 1 : 0, c->color[0], c->color[1], c->color[2], c->color[3],
        (double)c->depth, c->stencil);
}
static const char *const hz_names[] = { "counter", "cond", "ssbo", "cpu", "ubo" };
static void put_hazard(sink *k, unsigned hz) {
    put(k, "hazard");
    for (int i = 0; i < 5; i++) if (hz >> i & 1) put(k, " %s", hz_names[i]);
    put(k, "\n");
}
static const char *ld_name(int v) { return v == NB_LOAD_CLEAR ? "clear" : v == NB_LOAD_DONT_CARE ? "dc" : "load"; }
static const char *fate_name(int v) {
    static const char *const n[] = { "pending", "folded", "standalone", "dead", "inpass" };
    return v >= 0 && v <= NB_CLR_INPASS ? n[v] : "?";
}

size_t nb_dump_str(const nb_frame *f, const nb_plan_out *plan, char *buf, size_t cap) {
    sink k = { buf, cap, 0 };
    if (buf && cap) buf[0] = 0;
    put(&k, "nb frame %u\n", f->frame_no);
    if (f->track) put(&k, "track%s%s\n", (f->track & NB_TRACK_HAZARDS) ? " hazards" : "", (f->track & NB_TRACK_VIEWS) ? " views" : "");
    for (int ni = 0; ni < f->nnode; ni++) {
        const nb_node *n = &f->node[ni];
        const nb_ref *refs = &f->ref[n->ref_first];
        char tmp[4];
        if (n->kind == NB_NODE_PASS) {
            put(&k, "pass");
            for (int i = 0; i < n->natt; i++) put(&k, " %s=0x%llx", slot_name(n->att[i].slot, tmp), (unsigned long long)n->att[i].image);
            put(&k, "\n");
            for (int i = 0; i < n->nclear; i++) if (!f->clr[n->clear_first + i].after_draw) put_clear(&k, &f->clr[n->clear_first + i]);
            if (n->draws) put(&k, "draws %u cover=0x%x\n", n->draws, n->cover);
            if (n->nbar) put(&k, "barriers %u\n", n->nbar);   // NB-05b: kept inside the pass (nb_barrier)
            if (n->fsx & NB_FSX_ANY) {                        // NB-18b-4a
                put(&k, "fs=%016llx%s%s\n", (unsigned long long)n->fs, (n->fsx & NB_FSX_MIXED) ? " mixed" : "", (n->fsx & NB_FSX_EXACT) ? " exact" : "");
                for (int i = 0; i < n->nsr; i++) put(&k, "sread 0x%llx\n", (unsigned long long)n->sread[i]);
            }
            if (n->hz) put_hazard(&k, n->hz);                 // NB-16
            for (int i = 0; i < n->nclear; i++) if (f->clr[n->clear_first + i].after_draw) put_clear(&k, &f->clr[n->clear_first + i]);
            for (int i = 0; i < n->nref; i++) put(&k, "read 0x%llx\n", (unsigned long long)refs[i].image);
            put(&k, "end %s\n", nb_end_name(n->end));
            if (plan && ni < plan->nnode) {
                const nb_node_plan *np = &plan->node[ni];
                put(&k, "# plan %d %s", ni, (np->flags & NB_NP_EXEC) ? "exec" : (np->flags & NB_NP_CLEAR_ONLY) ? "clear-only" : "empty");
                if (np->flags & NB_NP_EXEC)
                    for (int i = 0; i < n->natt; i++)
                        put(&k, " %s:%s/%s", slot_name(n->att[i].slot, tmp), ld_name(np->load[i]), np->store[i] == NB_STORE ? "store" : "dc");
                if (np->flags & NB_NP_FEEDBACK) put(&k, " feedback");
                if (np->cover_dc) put(&k, " cover_dc=0x%x", np->cover_dc);
                put(&k, "\n");
                if (np->flags & NB_NP_PREBAR) put(&k, "# prebar %d\n", ni);   // NB-05c: own line (the golden "# plan" lines stay the phone's)
                if (np->flags & (NB_NP_ELIDE | NB_NP_FUSE | NB_NP_AA))   // NB-18b-4a; NB-18b-8: " aa" = NB_NP_AA (nb_post_aa.h)
                    put(&k, "# post %d %s%s\n", ni, (np->flags & NB_NP_ELIDE) ? "elide" : (np->flags & NB_NP_FUSE) ? "fuse" : "reads", (np->flags & NB_NP_AA) ? " aa" : "");
            }
        } else if ((n->kind & 0xff) == NB_OP_CLEAR) {
            put_clear(&k, &f->clr[n->clear_first]);
        } else if ((n->kind & 0xff) == NB_OP_FENCE) {
            put_hazard(&k, n->hz);                            // NB-16: outside passes (after the previous pass's "end")
        } else {
            put(&k, "op %s", nb_kind_name(n->kind));
            for (int i = 0; i < n->nref; i++) if (refs[i].role == NB_REF_SRC) put(&k, " src=0x%llx", (unsigned long long)refs[i].image);
            for (int i = 0; i < n->nref; i++) if (refs[i].role == NB_REF_DST) put(&k, " dst=0x%llx", (unsigned long long)refs[i].image);
            put(&k, " full=%d\n", (n->kind & NB_OPF_DST_FULL) ? 1 : 0);
        }
        if (plan && n->nclear) {
            for (int i = 0; i < n->nclear; i++) {
                const nb_clear_plan *cp = &plan->clr[n->clear_first + i];
                if (cp->fate == NB_CLR_INPASS) continue;
                put(&k, "# clear %d %s", n->clear_first + i, fate_name(cp->fate));
                if (cp->fate == NB_CLR_FOLDED) put(&k, " into %d", cp->into);
                put(&k, "\n");
            }
        }
    }
    put(&k, "submit final=%d\n", f->final);
    if (plan) {
        const nb_plan_stats *s = &plan->st;
        put(&k, "# stats passes=%d exec=%d empty=%d clear_only=%d folded=%d standalone=%d dead=%d load clear=%d dc=%d load=%d"
                " store dc=%d store=%d images=%d xread_new=%d transient=%d prebar=%d\n", s->passes, s->exec, s->empty, s->clear_only, s->folded,
            s->standalone, s->dead, s->load_clear, s->load_dc, s->load_load, s->store_dc, s->store_store, s->images, s->xread_new,
            s->transient, s->prebar);
        if (s->store_lost) put(&k, "# store DONT_CARE kept as STORE (lost records): %d\n", s->store_lost);
    }
    if (f->overflow || f->stray) put(&k, "# lost overflow=%u stray=%u\n", f->overflow, f->stray);
    return k.len;
}

void nb_dump(const nb_frame *f, const nb_plan_out *plan, FILE *out) {
    size_t need = nb_dump_str(f, plan, NULL, 0) + 1;
    char *b = (char *)malloc(need);
    if (!b) return;
    nb_dump_str(f, plan, b, need);
    fputs(b, out);
    free(b);
}

static const char *skip_ws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static int word_is(const char **s, const char *w) {
    size_t n = strlen(w);
    if (strncmp(*s, w, n) || ((*s)[n] && (*s)[n] != ' ' && (*s)[n] != '\t' && (*s)[n] != '\n' && (*s)[n] != '\r')) return 0;
    *s = skip_ws(*s + n);
    return 1;
}
static int key_u64(const char **s, const char *key, uint64_t *v) {   // key=<number>
    size_t n = strlen(key);
    if (strncmp(*s, key, n)) return 0;
    char *e; *v = strtoull(*s + n, &e, 0);
    if (e == *s + n) return 0;
    *s = skip_ws(e);
    return 1;
}

int nb_parse(const char *text, nb_frame *f) {
    int line = 0, started = 0;
    const char *s = text;
    while (*s) {
        const char *eol = strchr(s, '\n');
        size_t len = eol ? (size_t)(eol - s) : strlen(s);
        char ln[1024];
        line++;
        if (len >= sizeof ln) return line;
        memcpy(ln, s, len); ln[len] = 0;
        if (len && ln[len - 1] == '\r') ln[len - 1] = 0;
        s = eol ? eol + 1 : s + len;
        const char *p = skip_ws(ln);
        if (!*p || *p == '#') continue;
        uint64_t v;
        if (word_is(&p, "nb")) {
            if (!word_is(&p, "frame")) return line;
            char *e; unsigned long fn = strtoul(p, &e, 0);
            if (e == p) return line;
            nb_frame_reset(f, (uint32_t)fn); started = 1;
            continue;
        }
        if (!started) return line;
        if (word_is(&p, "track")) {          // NB-16
            unsigned t = 0;
            while (*p) { if (word_is(&p, "hazards")) t |= NB_TRACK_HAZARDS; else if (word_is(&p, "views")) t |= NB_TRACK_VIEWS; else return line; }
            nb_track(f, t);
        } else if (word_is(&p, "hazard")) {  // NB-16: inside a pass (before its "end") or a fence node outside passes
            unsigned hz = 0;
            while (*p) {
                int hit = 0;
                for (int i = 0; i < 5 && !hit; i++) if (word_is(&p, hz_names[i])) { hz |= 1u << i; hit = 1; }
                if (!hit) return line;
            }
            if (!hz) return line;
            nb_hazard(f, hz);
        } else if (word_is(&p, "pass")) {
            nb_att a[NB_MAX_ATT]; int n = 0;
            while (*p) {
                int slot;
                if (p[0] == 'd' && p[1] == 's' && p[2] == '=') { slot = NB_SLOT_DS; p += 2; }
                else if (p[0] == 'c' && p[1] >= '0' && p[1] <= '7' && p[2] == '=') { slot = p[1] - '0'; p += 2; }
                else return line;
                if (n >= NB_MAX_ATT || !key_u64(&p, "=", &v)) return line;
                a[n].image = v; a[n].slot = (uint8_t)slot; n++;
            }
            if (nb_pass_begin(f, a, n) < 0) return line;
        } else if (word_is(&p, "clear")) {
            char *e; uint64_t image = strtoull(p, &e, 0);
            if (e == p) return line;
            p = skip_ws(e);
            unsigned asp = 0, flags = 0; uint32_t col[4] = { 0, 0, 0, 0 }; float d = 0; uint32_t st = 0;
            if (strncmp(p, "asp=", 4)) return line;
            for (p += 4; *p && *p != ' '; p++) asp |= *p == 'c' ? NB_ASP_COLOR : *p == 'd' ? NB_ASP_DEPTH : *p == 's' ? NB_ASP_STENCIL : 0;
            p = skip_ws(p);
            if (!key_u64(&p, "full=", &v)) return line;
            if (v) flags |= NB_CLEAR_FULL;
            if (!key_u64(&p, "rt=", &v)) return line;
            if (v) flags |= NB_CLEAR_RT;
            if (strncmp(p, "v=", 2)) return line;
            p += 2;
            for (int i = 0; i < 4; i++) { col[i] = (uint32_t)strtoul(p, &e, 16); if (e == p) return line; p = e; if (i < 3) { if (*p != ',') return line; p++; } }
            p = skip_ws(p);
            if (strncmp(p, "d=", 2)) return line;
            d = (float)strtod(p + 2, &e); p = skip_ws(e);
            if (!key_u64(&p, "s=", &v)) return line;
            st = (uint32_t)v;
            nb_clear(f, image, asp, flags, col, d, st);
        } else if (word_is(&p, "draws")) {
            char *e; unsigned long n = strtoul(p, &e, 0);
            if (e == p || !n) return line;
            p = skip_ws(e);
            if (!key_u64(&p, "cover=", &v)) return line;
            for (unsigned long i = 0; i < n; i++) nb_draw(f, i ? 0 : (uint32_t)v);
        } else if (word_is(&p, "barriers")) {   // NB-05b: barriers kept inside the open pass
            char *e; unsigned long n = strtoul(p, &e, 0);
            if (e == p || !n || !f->open) return line;
            for (unsigned long i = 0; i < n; i++) nb_barrier(f);
        } else if (!strncmp(p, "fs=", 3)) {   // NB-18b-4a: program facts of the open pass
            char *e; const uint64_t key = strtoull(p + 3, &e, 16);
            if (e == p + 3 || !f->open) return line;
            p = skip_ws(e);
            nb_node *o = &f->node[f->nnode - 1];
            o->fs = key; o->fsx = NB_FSX_ANY;
            while (*p) { if (word_is(&p, "mixed")) o->fsx |= NB_FSX_MIXED; else if (word_is(&p, "exact")) o->fsx |= NB_FSX_EXACT; else return line; }
        } else if (word_is(&p, "sread")) {
            char *e; uint64_t image = strtoull(p, &e, 0);
            if (e == p || !f->open) return line;
            nb_node *o = &f->node[f->nnode - 1];
            if (o->nsr >= NB_MAX_SREAD) return line;
            o->sread[o->nsr++] = image;
        } else if (word_is(&p, "read")) {
            char *e; uint64_t image = strtoull(p, &e, 0);
            if (e == p || nb_read(f, -1, image) < 0) return line;
        } else if (word_is(&p, "end")) {
            int r = -1;
            for (int i = 1; i < NB_END_KINDS; i++) { const char *q = p; if (word_is(&q, end_names[i])) { r = i; break; } }
            if (r < 0 || !f->open) return line;
            nb_pass_end(f, r);
        } else if (word_is(&p, "op")) {
            int kind = -1;
            for (int i = 1; i < NB_OP_KINDS; i++) if (i != NB_OP_CLEAR && i != NB_OP_FENCE && word_is(&p, kind_names[i])) { kind = i; break; }
            if (kind < 0) return line;
            uint64_t src[64], dst[64]; int ns = 0, nd = 0, full = 0;
            while (*p) {
                if (key_u64(&p, "src=", &v)) { if (ns >= 64) return line; src[ns++] = v; }
                else if (key_u64(&p, "dst=", &v)) { if (nd >= 64) return line; dst[nd++] = v; }
                else if (key_u64(&p, "full=", &v)) full = v != 0;
                else return line;
            }
            if (nb_op(f, kind | (full ? NB_OPF_DST_FULL : 0), src, ns, dst, nd) < 0) return line;
        } else if (word_is(&p, "submit")) {
            if (!key_u64(&p, "final=", &v)) return line;
            nb_submit(f, (int)v);
            break;
        } else return line;
    }
    return started ? 0 : (line ? line : 1);
}

/* ---------------- NB-16: merge planner (nb_ir.h, plan §4 "Слияния") ---------------- */
enum { MC_NONE = 0, MC_PASS, MC_WRITER, MC_FENCE };   // no dependencies / executed pass / other image writer / orders against everything

static int m_exec(const nb_frame *f, const nb_plan_out *pl, int i) {
    return f->node[i].kind == NB_NODE_PASS && (pl->node[i].flags & NB_NP_EXEC);
}
static int m_class(const nb_frame *f, const nb_plan_out *pl, int i) {
    const nb_node *n = &f->node[i];
    const int k = n->kind & 0xff;
    if (k == NB_NODE_PASS) {
        if (n->hz) return MC_FENCE;
        if (pl->node[i].flags & NB_NP_EXEC) return MC_PASS;
        return (pl->node[i].flags & NB_NP_CLEAR_ONLY) ? MC_WRITER : MC_NONE;
    }
    switch (k) {
    case NB_OP_BARRIER: return MC_NONE;                // the host skips guest barriers; image writes are all tracked
    case NB_OP_CLEAR: return MC_WRITER;
    case NB_OP_COPY: case NB_OP_BLIT: case NB_OP_RESOLVE: return n->nref ? MC_WRITER : MC_FENCE;   // no images = buffer copy / clear
    default: return MC_FENCE;                          // compute (storage buffers), readback (CPU-visible), present, fence
    }
}
// Access lists into m->acc: writes, then reads. A pass reads its attachments too (LOAD), its sampled images; ops: dst / src.
static int m_lists(const nb_frame *f, const nb_plan_out *pl, nb_merge_out *m) {
    const int cap = (int)(sizeof m->acc / sizeof m->acc[0]);
    int at = 0;
    for (int i = 0; i < f->nnode; i++) {
        const nb_node *n = &f->node[i];
        const nb_ref *refs = &f->ref[n->ref_first];
        const int k = n->kind & 0xff;
        int nw = 0, nr = 0;
        m->cls[i] = (uint8_t)m_class(f, pl, i);
        m->acc_first[i] = at;
        if (m->cls[i] != MC_FENCE && m->cls[i] != MC_NONE) {
            if (at + 2 * n->natt + n->nref + n->nclear + 1 > cap) return -1;
            if (k == NB_NODE_PASS && (pl->node[i].flags & NB_NP_EXEC)) {
                for (int a = 0; a < n->natt; a++) m->acc[at + nw++] = n->att[a].image;
                for (int a = 0; a < n->natt; a++) m->acc[at + nw + nr++] = n->att[a].image;
                for (int r = 0; r < n->nref; r++) m->acc[at + nw + nr++] = refs[r].image;
            } else if (k == NB_NODE_PASS || k == NB_OP_CLEAR) {          // clear-only pass / texture clear: writes the cleared images
                for (int c = 0; c < n->nclear; c++) m->acc[at + nw++] = f->clr[n->clear_first + c].image;
            } else {
                for (int r = 0; r < n->nref; r++) if (refs[r].role == NB_REF_DST) m->acc[at + nw++] = refs[r].image;
                for (int r = 0; r < n->nref; r++) if (refs[r].role != NB_REF_DST) m->acc[at + nw + nr++] = refs[r].image;
            }
        }
        m->acc_nw[i] = (uint16_t)nw; m->acc_nr[i] = (uint16_t)nr;
        at += nw + nr;
    }
    return 0;
}
static int m_in(const uint64_t *v, int n, uint64_t x) { for (int i = 0; i < n; i++) if (v[i] == x) return 1; return 0; }
// x executes before y: does their relative order matter?
static int m_dep(const nb_merge_out *m, int x, int y) {
    if (m->cls[x] == MC_FENCE || m->cls[y] == MC_FENCE) return 1;
    if (m->cls[x] == MC_NONE || m->cls[y] == MC_NONE) return 0;
    const uint64_t *wx = &m->acc[m->acc_first[x]], *rx = wx + m->acc_nw[x], *wy = &m->acc[m->acc_first[y]], *ry = wy + m->acc_nw[y];
    for (int i = 0; i < m->acc_nw[x]; i++) if (m_in(wy, m->acc_nw[y], wx[i]) || m_in(ry, m->acc_nr[y], wx[i])) return 1;   // WAW, RAW
    for (int i = 0; i < m->acc_nr[x]; i++) if (m_in(wy, m->acc_nw[y], rx[i])) return 1;                                     // WAR
    return 0;
}
// Ranges [a0, a1) and [b0, b1) of the order, the first entirely before the second.
static int m_dep_rng(const nb_merge_out *m, int a0, int a1, int b0, int b1) {
    for (int a = a0; a < a1; a++) for (int b = b0; b < b1; b++) if (m_dep(m, m->order[a], m->order[b])) return 1;
    return 0;
}
static int m_same_set(const nb_node *a, const nb_node *b) {
    if (a->natt != b->natt || !a->natt) return 0;
    for (int i = 0; i < a->natt; i++) {
        int hit = 0;
        for (int k = 0; k < b->natt && !hit; k++) hit = a->att[i].image == b->att[k].image && a->att[i].slot == b->att[k].slot;
        if (!hit) return 0;
    }
    return 1;
}
static int hz_why(unsigned hz) {
    return (hz & NB_HZ_COUNTER) ? NB_MB_COUNTER : (hz & NB_HZ_COND) ? NB_MB_COND : (hz & NB_HZ_SSBO) ? NB_MB_SSBO : (hz & NB_HZ_CPU) ? NB_MB_CPU
         : NB_MB_UBO;
}

static const char *const why_names[NB_MB_KINDS] = { "untracked", "kind-off", "feedback", "a2-clear", "counter", "cond", "ssbo", "cpu", "ubo",
                                                    "fence", "touches-a", "chain", "a2-reads-b" };
const char *nb_merge_why_name(int why) { return why == NB_MB_MERGED ? "merged" : why >= 0 && why < NB_MB_KINDS ? why_names[why] : "?"; }

// NB-05c PREBAR for the order in m (per unit, at its head): a unit sampling (not as its own attachment) an image written after the
// last barrier point needs a barrier before it begins; the point then moves to right before the unit.
static void m_prebar(const nb_frame *f, const nb_plan_out *pl, nb_merge_out *m) {
    uint64_t im[NB_MAX_FRAME_IMAGES]; int wp[NB_MAX_FRAME_IMAGES], nim = 0, bar = -1;
    #define NOTE_W(image, at) do { uint64_t x_ = (image); int k_ = 0; while (k_ < nim && im[k_] != x_) k_++; \
        if (k_ == nim) { if (nim >= NB_MAX_FRAME_IMAGES) { bar = -2; break; } im[nim++] = x_; } wp[k_] = (at); } while (0)
    m->st.prebar = 0;
    for (int x = 0; x < f->nnode; x++) m->prebar[x] = 0;
    for (int x = 0; x < f->nnode; x++) {
        const int i = m->order[x];
        const nb_node *n = &f->node[i];
        const int k = n->kind & 0xff;
        if (k == NB_NODE_PASS && m_exec(f, pl, i)) {
            if (m->cont[i] >= 0) continue;                 // handled with its head
            int len = 1;
            while (x + len < f->nnode && m->cont[m->order[x + len]] == m->order[x + len - 1]) len++;
            int need = 0;
            for (int u = 0; u < len && !need; u++) {
                const nb_node *p = &f->node[m->order[x + u]];
                const nb_ref *refs = &f->ref[p->ref_first];
                for (int r = 0; r < p->nref && !need; r++) {
                    if (att_index(p, refs[r].image) >= 0) continue;
                    for (int q = 0; q < nim; q++) if (im[q] == refs[r].image && wp[q] > bar) { need = 1; break; }
                }
            }
            if (need) { m->prebar[i] = 1; m->st.prebar++; bar = x - 1; }
            for (int u = 0; u < len; u++) { const nb_node *p = &f->node[m->order[x + u]]; for (int a = 0; a < p->natt; a++) NOTE_W(p->att[a].image, x + u); }
            x += len - 1;
        } else if (k == NB_NODE_PASS || k == NB_OP_CLEAR) {
            for (int c = 0; c < n->nclear; c++) if (pl->clr[n->clear_first + c].fate == NB_CLR_STANDALONE) NOTE_W(f->clr[n->clear_first + c].image, x);
        } else {
            const nb_ref *refs = &f->ref[n->ref_first];
            for (int r = 0; r < n->nref; r++) if (refs[r].role == NB_REF_DST) NOTE_W(refs[r].image, x);
        }
        if (bar == -2) break;                              // image table full: cannot happen (planner has the same cap)
    }
    #undef NOTE_W
}

int nb_merge(const nb_frame *f, const nb_plan_out *pl, const nb_merge_rules *r, nb_merge_out *m) {
    if (!f || !pl || !r || !m || f->nnode < 0 || f->nnode > NB_MAX_NODES || pl->nnode != f->nnode) return -1;
    const int nn = f->nnode;
    unsigned fl = r->flags;
    if (!(fl & (NB_MERGEF_AA | NB_MERGEF_ABA))) fl |= NB_MERGEF_AA | NB_MERGEF_ABA;
    const int tracked = (f->track & NB_TRACK_MERGE) == NB_TRACK_MERGE || (fl & NB_MERGEF_ASSUME_TRACKED);
    memset(&m->st, 0, sizeof m->st);
    m->nnode = nn; m->nrec = 0;
    for (int i = 0; i < nn; i++) { m->order[i] = m->pos[i] = m->head[i] = (int16_t)i; m->cont[i] = -1; m->prebar[i] = 0; }
    const int lists_ok = m_lists(f, pl, m) == 0;

    for (int j = 0; j < nn; j++) {
        if (!m_exec(f, pl, j)) continue;
        const nb_node *nj = &f->node[j];
        const int q = m->pos[j];
        int t = -1;
        for (int x = q - 1; x >= 0; x--) { const int y = m->order[x]; if (m_exec(f, pl, y) && m_same_set(&f->node[y], nj)) { t = y; break; } }
        if (t < 0) continue;
        const int p = m->pos[t], h = m->head[t], s0 = m->pos[h];
        nb_merge_rec *rc = &m->rec[m->nrec++];
        rc->node = (int16_t)j; rc->prev = (int16_t)t; rc->head = (int16_t)h; rc->kind = NB_MK_AA;
        rc->between = (int16_t)(q - p - 1); rc->before = rc->after = 0; rc->culprit = -1;
        for (int x = p + 1; x < q; x++) if (m_exec(f, pl, m->order[x])) rc->kind = NB_MK_ABA;
        if (rc->kind == NB_MK_AA) m->st.cand_aa++; else m->st.cand_aba++;
        int why = -1;
        unsigned hz = nj->hz;
        int fb = (pl->node[j].flags & NB_NP_FEEDBACK) != 0, a2c = 0;
        for (int x = s0; x <= p; x++) { hz |= f->node[m->order[x]].hz; fb |= (pl->node[m->order[x]].flags & NB_NP_FEEDBACK) != 0; }
        for (int a = 0; a < nj->natt; a++) a2c |= pl->node[j].load[a] == NB_LOAD_CLEAR;
        if (!tracked) why = NB_MB_UNTRACKED;
        else if (!(fl & (rc->kind == NB_MK_AA ? NB_MERGEF_AA : NB_MERGEF_ABA))) why = NB_MB_KIND_OFF;
        else if (fb) why = NB_MB_FEEDBACK;
        else if (a2c) why = NB_MB_A2_CLEAR;
        else if (hz) why = hz_why(hz);
        else if (!lists_ok) why = NB_MB_FENCE;
        if (why < 0) {
            // Items between: merged units stay atomic (consecutive entries with the same head).
            int ni = 0;
            for (int x = p + 1; x < q; x++) if (x == p + 1 || m->head[m->order[x]] != m->head[m->order[x - 1]]) m->item[ni++] = (int16_t)x;
            m->item[ni] = (int16_t)q;
            for (int it = 0; it < ni; it++) {             // must stay after the unit: depends on A1.. (or on such an item)
                int a = m_dep_rng(m, s0, p + 1, m->item[it], m->item[it + 1]);
                for (int z = 0; z < it && !a; z++) a = m->after[z] && m_dep_rng(m, m->item[z], m->item[z + 1], m->item[it], m->item[it + 1]);
                m->after[it] = (uint8_t)a;
            }
            for (int it = ni - 1; it >= 0; it--) {        // must stay before A2: A2 depends on it (or on such an item)
                int b = m_dep_rng(m, m->item[it], m->item[it + 1], q, q + 1);
                for (int z = it + 1; z < ni && !b; z++) b = m->need[z] && m_dep_rng(m, m->item[it], m->item[it + 1], m->item[z], m->item[z + 1]);
                m->need[it] = (uint8_t)b;
            }
            int fence = -1, touch = -1, chain = -1, bpass = -1;
            for (int it = 0; it < ni; it++) {
                if (m->need[it] && !m->after[it] && bpass < 0)
                    for (int x = m->item[it]; x < m->item[it + 1]; x++) if (m_exec(f, pl, m->order[x])) { bpass = x; break; }
                if (!(m->after[it] && m->need[it])) continue;
                for (int x = m->item[it]; x < m->item[it + 1]; x++) {
                    const int y = m->order[x];
                    if (m->cls[y] == MC_FENCE) { if (fence < 0) fence = x; continue; }
                    const uint64_t *v = &m->acc[m->acc_first[y]];
                    int hit = 0;
                    for (int a = 0; a < nj->natt && !hit; a++) hit = m_in(v, m->acc_nw[y] + m->acc_nr[y], nj->att[a].image);
                    if (hit) { if (touch < 0) touch = x; }
                    else if (chain < 0) chain = x;
                }
            }
            if (fence >= 0) { const nb_node *c = &f->node[m->order[fence]]; why = c->hz ? hz_why(c->hz) : NB_MB_FENCE; rc->culprit = m->order[fence]; }
            else if (touch >= 0) { why = NB_MB_TOUCHES_A; rc->culprit = m->order[touch]; }
            else if (chain >= 0) { why = NB_MB_CHAIN; rc->culprit = m->order[chain]; }
            else if (bpass >= 0 && !(fl & NB_MERGEF_HOIST)) { why = NB_MB_A2_READS_B; rc->culprit = m->order[bpass]; }
            if (why < 0) {                                 // merge: [between, not after] [unit A1..] [A2] [between, after]
                int w = 0;
                for (int it = 0; it < ni; it++) if (!m->after[it]) for (int x = m->item[it]; x < m->item[it + 1]; x++) { m->tmp[w++] = m->order[x]; rc->before++; }
                for (int x = s0; x <= p; x++) m->tmp[w++] = m->order[x];
                m->tmp[w++] = (int16_t)j;
                for (int it = 0; it < ni; it++) if (m->after[it]) for (int x = m->item[it]; x < m->item[it + 1]; x++) { m->tmp[w++] = m->order[x]; rc->after++; }
            }
        }
        rc->why = (uint8_t)(why < 0 ? NB_MB_MERGED : why);
        if (why >= 0) { m->st.blocked[why]++; continue; }
        if (rc->kind == NB_MK_AA) m->st.aa++; else m->st.exact++;
        if (r->mode != NB_MERGE_ON) continue;              // analysis only: the order stays the identity
        const int len = q - s0 + 1;
        for (int x = 0; x < len; x++) { m->order[s0 + x] = m->tmp[x]; m->pos[m->tmp[x]] = (int16_t)(s0 + x); }
        m->cont[j] = (int16_t)t; m->head[j] = (int16_t)h;
        m->st.merged++; m->st.moved += rc->between;
    }
    for (int i = 0; i < nn; i++) if (m_exec(f, pl, i) && m->cont[i] < 0) m->st.units++;
    m_prebar(f, pl, m);
    return 0;
}

size_t nb_merge_summary(const nb_merge_out *m, char *buf, size_t cap) {
    sink k = { buf, cap, 0 };
    if (buf && cap) buf[0] = 0;
    const nb_merge_stats *s = &m->st;
    int nb = 0;
    for (int i = 0; i < NB_MB_KINDS; i++) nb += s->blocked[i];
    put(&k, "aa=%d/%d aba=%d exact=%d blocked(", s->aa, s->cand_aa, s->cand_aba, s->exact);
    int first = 1;
    for (int i = 0; i < NB_MB_KINDS; i++) if (s->blocked[i]) { put(&k, "%s%s %d", first ? "" : ", ", why_names[i], s->blocked[i]); first = 0; }
    put(&k, ")=%d merged=%d moved=%d units=%d prebar=%d", nb, s->merged, s->moved, s->units, s->prebar);
    return k.len;
}

size_t nb_merge_tail_size(int nnode, int with_offsets) {
    if (nnode < 0) nnode = 0;
    return sizeof(nb_merge_tail_hdr) + (((size_t)nnode * 2 + 3) & ~(size_t)3) + (with_offsets ? (size_t)nnode * 4 : 0);
}
size_t nb_merge_tail_pack(const nb_merge_out *m, uint32_t seq, const uint32_t *offsets, void *buf, size_t cap) {
    const int n = m->nnode;
    const size_t need = nb_merge_tail_size(n, offsets != NULL);
    if (!buf || cap < need || n > NB_MAX_NODES || need > 0xffff) return 0;
    unsigned char *b = (unsigned char *)buf;
    nb_merge_tail_hdr h;
    memset(&h, 0, sizeof h);
    h.version = NB_MERGE_TAIL_VERSION; h.flags = (uint8_t)((offsets ? NB_MTF_OFFSETS : 0) | (m->st.merged ? NB_MTF_MERGED : 0));
    h.bytes = (uint16_t)need; h.nnode = (uint16_t)n; h.seq = seq;
    size_t at = sizeof h;
    for (int x = 0; x < n; x++) {
        const int i = m->order[x];
        uint16_t e = (uint16_t)(i & NB_MT_NODE);
        if (m->cont[i] >= 0) { e |= NB_MT_CONT; h.ncont++; }
        if (m->prebar[i]) e |= NB_MT_PREBAR;
        memcpy(b + at, &e, 2); at += 2;
    }
    while (at & 3) { b[at++] = 0xff; }
    if (offsets) { memcpy(b + at, offsets, (size_t)n * 4); at += (size_t)n * 4; }
    memcpy(b, &h, sizeof h);
    return at;
}
int nb_merge_tail_parse(const void *buf, size_t size, uint16_t *entry, int cap, uint32_t *offsets, nb_merge_tail_hdr *hdr) {
    nb_merge_tail_hdr h;
    if (!buf || size < sizeof h) return -1;
    memcpy(&h, buf, sizeof h);
    if (h.version != NB_MERGE_TAIL_VERSION || h.nnode > NB_MAX_NODES || h.nnode > cap || h.bytes != size
        || size != nb_merge_tail_size(h.nnode, (h.flags & NB_MTF_OFFSETS) != 0)) return -1;
    const unsigned char *b = (const unsigned char *)buf;
    uint8_t seen[NB_MAX_NODES];
    memset(seen, 0, sizeof seen);
    int ncont = 0;
    for (int x = 0; x < h.nnode; x++) {
        uint16_t e; memcpy(&e, b + sizeof h + (size_t)x * 2, 2);
        const int i = e & NB_MT_NODE;
        if (i >= h.nnode || seen[i] || (e & ~(NB_MT_NODE | NB_MT_CONT | NB_MT_PREBAR)) || ((e & NB_MT_CONT) && (x == 0 || (e & NB_MT_PREBAR)))) return -1;
        seen[i] = 1; ncont += (e & NB_MT_CONT) != 0;
        if (entry) entry[x] = e;
    }
    if (ncont != h.ncont) return -1;
    if (offsets && (h.flags & NB_MTF_OFFSETS)) memcpy(offsets, b + sizeof h + (((size_t)h.nnode * 2 + 3) & ~(size_t)3), (size_t)h.nnode * 4);
    if (hdr) *hdr = h;
    return h.nnode;
}

/* ---------------- NB-18b-4a: post-tail fusion plan (nb_ir.h) ---------------- */
int nb_post_class(uint64_t fs) {   // the keys of NB-18a's table (work/shaders-vk/<key>.xgs), by their first 8 hex digits
    switch ((uint32_t)(fs >> 32)) {
    case 0x921728a5u: return NB_PSH_LUT;
    case 0x91eb2d97u: return NB_PSH_SIGHT;
    case 0x43e8e8edu: return NB_PSH_VIG;
    case 0x98186c22u: return NB_PSH_COMB;   // CombineUI_NoFetch (the only combine variant in P2)
    default: return NB_PSH_OTHER;
    }
}
static const char *const post_cls_names[NB_PSH_KINDS] = { "other", "LUT", "Sight", "Vignette", "CombineUI_NoFetch" };
const char *nb_post_class_name(int cls) { return cls >= 0 && cls < NB_PSH_KINDS ? post_cls_names[cls] : "?"; }
static const char *const post_why_names[NB_PW_KINDS] = { "fused", "host pass shape", "no elidable pass before it", "elided pass shape",
    "elided pass keeps old texels", "reads not exact", "host does not sample the elided target", "intermediate read by another pass",
    "later pass loads the intermediate", "intermediate survives the frame without history", "sizes differ" };
const char *nb_post_why_name(int why) { return why >= 0 && why < NB_PW_KINDS ? post_why_names[why] : "?"; }

static int pp_has(const uint64_t *v, int n, uint64_t x) { for (int i = 0; i < n; i++) if (v[i] == x) return 1; return 0; }
// node n reads image x (its exact sampled list when it has one, else every IR read; ops: their sources)
static int pp_reads(const nb_frame *f, const nb_node *n, uint64_t x) {
    if (n->kind == NB_NODE_PASS && (n->fsx & NB_FSX_EXACT)) return pp_has(n->sread, n->nsr, x);
    for (int i = 0; i < n->nref; i++) { const nb_ref *r = &f->ref[n->ref_first + i]; if (r->image == x && r->role != NB_REF_DST) return 1; }
    return 0;
}
// Attachment a of pass k keeps its old texels: load LOAD without cover_dc - unless the pass's feedback flag came from IR reads that its
// exact sampled list disproves (stale bindings: no NB-07 slot masks in the IR yet); then its first draw is taken to cover the target like
// a cover_dc pass (*assumed = 1; DM_NB_POST=3 poisons what such an assumption would expose).
static int pp_keeps_old(const nb_frame *f, const nb_plan_out *pl, int k, int a, int *assumed) {
    const nb_node *n = &f->node[k]; const nb_node_plan *np = &pl->node[k];
    *assumed = 0;
    if (!(np->flags & NB_NP_EXEC) || np->load[a] != NB_LOAD || (np->cover_dc >> a & 1)) return 0;
    if ((np->flags & NB_NP_FEEDBACK) && (n->fsx & NB_FSX_EXACT) && n->draws && n->att[a].slot != NB_SLOT_DS) {
        for (int b = 0; b < n->natt; b++) if (pp_has(n->sread, n->nsr, n->att[b].image)) return 1;
        *assumed = 1; return 0;
    }
    return 1;
}
static int pp_size(const nb_post_rules *r, uint64_t im, uint32_t *w, uint32_t *h) { return r && r->size && r->size(r->ctx, im, w, h); }
static int pp_hist(nb_post_hist *h, uint64_t im, int create) {
    for (int i = 0; i < h->n; i++) if (h->e[i].image == im) return i;
    if (!create) return -1;
    if (h->n < (int)(sizeof h->e / sizeof h->e[0])) { int i = h->n++; h->e[i].image = im; h->e[i].seen = 0; h->e[i].xread = 0; return i; }
    return -1;
}
// op node n overwrites its dst image entirely (a texture clear: its record's NB_CLEAR_FULL; else NB_OPF_DST_FULL)
static int pp_op_full(const nb_frame *f, const nb_node *n) {
    if ((n->kind & 0xff) == NB_OP_CLEAR) return n->nclear > 0 && (f->clr[n->clear_first].flags & NB_CLEAR_FULL);
    return (n->kind & NB_OPF_DST_FULL) != 0;
}
// first access of im in the submission: 1 = a read (old texels used), 0 = a write, -1 = none
static int pp_first_access(const nb_frame *f, const nb_plan_out *pl, uint64_t im) {
    for (int k = 0; k < f->nnode; k++) {
        const nb_node *n = &f->node[k];
        if (n->kind == NB_NODE_PASS) {
            if (!(pl->node[k].flags & NB_NP_EXEC)) continue;
            if (pp_reads(f, n, im)) return 1;
            for (int a = 0; a < n->natt; a++) if (n->att[a].image == im) { int as; return pp_keeps_old(f, pl, k, a, &as); }
        } else {
            if (pp_reads(f, n, im)) return 1;
            for (int i = 0; i < n->nref; i++) {
                const nb_ref *r = &f->ref[n->ref_first + i];
                if (r->image == im && r->role == NB_REF_DST) return pp_op_full(f, n) ? 0 : 1;
            }
        }
    }
    return -1;
}
static int pp_exec1(const nb_frame *f, const nb_plan_out *pl, int k) {
    const nb_node *n = &f->node[k];
    return n->kind == NB_NODE_PASS && (pl->node[k].flags & NB_NP_EXEC) && n->draws == 1 && (n->fsx & NB_FSX_ANY) && !(n->fsx & NB_FSX_MIXED) && !n->hz;
}
static int pp_check(const nb_frame *f, const nb_plan_out *pl, const nb_post_rules *r, const nb_post_hist *h, nb_post_out *o, int i, int fx, int *je) {
    const nb_node *H = &f->node[i];
    if (!pp_exec1(f, pl, i) || o->fx[i]) return NB_PW_HOST;
    int j = i - 1;
    while (j >= 0) {   // only barrier ops / empty passes between E and H
        const nb_node *n = &f->node[j];
        if ((n->kind & 0xff) == NB_OP_BARRIER || (n->kind == NB_NODE_PASS && !n->draws && !n->nclear)) { j--; continue; }
        break;
    }
    if (j < 0 || f->node[j].kind != NB_NODE_PASS || nb_post_class(f->node[j].fs) != (fx == NB_FXK_F1 ? NB_PSH_LUT : NB_PSH_VIG)) return NB_PW_NO_ELIDED;
    const nb_node *E = &f->node[j];
    *je = j;
    if (!pp_exec1(f, pl, j) || o->fx[j] || E->nclear || E->natt != 1 || E->att[0].slot != 0) return NB_PW_ELIDED_SHAPE;
    if (!(E->fsx & NB_FSX_EXACT) || !(H->fsx & NB_FSX_EXACT)) return NB_PW_INEXACT;
    const uint64_t O = E->att[0].image;
    if (pp_has(E->sread, E->nsr, O)) return NB_PW_ELIDED_SHAPE;
    int assumed = 0;
    if (pp_keeps_old(f, pl, j, 0, &assumed)) return NB_PW_ELIDED_LOADS;
    if (!pp_has(H->sread, H->nsr, O)) return NB_PW_NO_EDGE;
    for (int a = 0; a < H->natt; a++) if (H->att[a].image == O) return NB_PW_HOST;
    int rewritten = 0, later_cover = 0, later_assumed = 0;
    for (int k = j + 1; k < f->nnode && !rewritten; k++) {
        if (k == i) continue;
        const nb_node *n = &f->node[k];
        if (n->kind == NB_NODE_PASS) {
            if (!(pl->node[k].flags & NB_NP_EXEC)) continue;
            if (pp_reads(f, n, O)) return NB_PW_OTHER_READER;
            for (int a = 0; a < n->natt; a++) if (n->att[a].image == O) {
                int as = 0;
                if (pp_keeps_old(f, pl, k, a, &as)) return NB_PW_LATER_LOAD;
                if (pl->node[k].load[a] == NB_LOAD) { if (as) later_assumed = 1; else later_cover = 1; }
                rewritten = 1;
            }
        } else {
            if (pp_reads(f, n, O)) return NB_PW_OTHER_READER;
            for (int q = 0; q < n->nref; q++) {
                const nb_ref *rf = &f->ref[n->ref_first + q];
                if (rf->image == O && rf->role == NB_REF_DST) { if (!pp_op_full(f, n)) return NB_PW_OTHER_READER; rewritten = 1; }
            }
        }
    }
    if (!rewritten) {   // O carries into the next submission: there its first access must have been a write, hist_frames times in a row
        const int need = r && r->hist_frames > 0 ? r->hist_frames : 8;
        int e = -1;
        for (int q = 0; q < h->n; q++) if (h->e[q].image == O) e = q;
        if (e < 0 || h->e[e].xread || (int)h->e[e].seen < need) return NB_PW_PERSIST;
    }
    if (r && r->size) {
        uint32_t ow, oh, w, hh;
        if (!pp_size(r, O, &ow, &oh)) return NB_PW_SIZE;
        if (fx == NB_FXK_F1 && (!H->natt || !pp_size(r, H->att[0].image, &w, &hh) || w != ow || hh != oh)) return NB_PW_SIZE;
        for (int q = 0; q < E->nsr; q++) if (pp_size(r, E->sread[q], &w, &hh) && (w != ow || hh != oh)) return NB_PW_SIZE;
    }
    o->later_cover += later_cover; o->assumed_cover += assumed + later_assumed;
    return NB_PW_FUSED;
}
int nb_post_plan(const nb_frame *f, nb_plan_out *pl, const nb_post_rules *r, nb_post_hist *h, nb_post_out *o) {
    if (!f || !pl || !o || f->nnode > NB_MAX_NODES || pl->nnode != f->nnode) return -1;
    nb_post_hist tmp; memset(&tmp, 0, sizeof tmp);
    if (!h) h = &tmp;
    memset(o, 0, sizeof *o); o->nnode = f->nnode;
    for (int i = 0; i < f->nnode; i++) o->peer[i] = -1;
    const unsigned allow = r && r->fx ? r->fx : ~0u;
    int pairs = 0;
    for (int i = 0; i < f->nnode; i++) {
        const nb_node *H = &f->node[i];
        if (H->kind != NB_NODE_PASS || !(H->fsx & NB_FSX_ANY)) continue;
        const int c = nb_post_class(H->fs), fx = c == NB_PSH_SIGHT ? NB_FXK_F1 : c == NB_PSH_COMB ? NB_FXK_F2 : NB_FXK_NONE;
        if (!fx || !(allow >> fx & 1)) continue;
        int j = -1; const int why = pp_check(f, pl, r, h, o, i, fx, &j);
        if (j >= 0 && why >= NB_PW_PERSIST) pp_hist(h, f->node[j].att[0].image, 1);   // start the first-access history of this intermediate
        o->why[fx][why]++;
        if (why != NB_PW_FUSED) continue;
        pl->node[j].flags |= NB_NP_ELIDE; pl->node[i].flags |= NB_NP_FUSE;
        o->fx[j] = o->fx[i] = (uint8_t)fx; o->peer[j] = (int16_t)i; o->peer[i] = (int16_t)j; o->fused[fx]++; pairs++;
    }
    for (int e = 0; e < h->n; e++) {   // history: was the first access of each tracked image in this submission a read? (sticky)
        const int a = pp_first_access(f, pl, h->e[e].image);
        if (a == 1) h->e[e].xread = 1;
        if (a >= 0 && h->e[e].seen < 0xffffffffu) h->e[e].seen++;
    }
    return pairs;
}
