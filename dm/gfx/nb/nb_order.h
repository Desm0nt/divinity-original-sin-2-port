// [dos2-nb] NB-17a / NB-17b: executing the passes of a submission in the order the plan gives (docs/dos2/NATIVE_BACKEND_PLAN.md
// "NB-17a findings"). Pure C, inline, no Vulkan, no game: the NVN tap (dm/gfx/nvn/nvn_nb_order.inc), the backend
// (dm/gfx/vulkan/engine/xbg_nb_order.inc) and the host test (tests/nb_order_test.c) run the same code.
//
// Unit of execution = a SEGMENT: the records from the OP_X_NB_PASS of an IR pass up to the OP_X_NB_PASS of the next pass of the stream
// (the pass, its PASS_END, and whatever follows before the next pass: the NVN records of the operations that closed it and the state
// records of the next draws). The records between OP_X_NB_FRAME and the first PASS are the prefix: always executed first. The last
// segment of the stream ends at the OP_END of its handle.
//
// Order tail of OP_X_NB_FRAME (DM_NB_ORDER != 0; replaces the NB-16 "v1" layout, which had node offsets nobody wrote): at payload offset
// NBO_TAIL_OFF, 8-byte aligned:
//   nbo_hdr  { magic, version, flags NBO_F_*, why NBO_W_* (flags without VALID), mode, seq = p_nb_frame.seq, cap, nseg, nnode, ncont }
//   nbo_ent  ent[cap], the first nseg used, in EXECUTION order: addr = address of the pass's OP_X_NB_PASS record header (the record
//            memory is live until the handle is translated: the same assumption as the REFs of NB-13a / NB-14), node, flags NBO_E_*.
// The tail is reserved when the FRAME record is written (the number of passes is not known yet: cap comes from the previous parts) and
// filled when the submission is planned, before the job is queued. Without NBO_F_VALID the backend executes the stream as recorded.
// Stream order = ascending node (PASS records are written as the passes open).
//
// nbo_build: nb_merge's node order -> segment order. The planner moves nodes, the stream can move segments, so the order is usable only
// when every operation node stays right behind the pass whose segment holds its records (barrier operations excepted: the host skips
// guest barriers, their records can stay where they are); else the identity order is written and NBO_F_GLUE says so.
// nbo_schedule: per executed segment: where it starts, where it stops (the start of its stream successor; none = runs to OP_END),
// whether it starts where the previously executed one ended (continuous) or needs its key frame (NB-14 OP_X_NB_KEY).
// nbo_scan: one walk over the part's record headers (only for an order that is not the identity, and in check mode): every PASS of the
// part is where the tail says, in stream order; per segment: the complete key right after its PASS, and whether it holds only records
// that write the backend's state (io->rec). nbo_unsafe: an inversion (two segments executed in the opposite order of the stream) is
// allowed when the planner tracked every hazard (NBO_F_TRACKED) or when one of the two is state-only (its whole effect is the state, and
// the state of every discontinuous start comes from a key).
// nbo_run: the loop itself, over callbacks (exec a range / start from a key / keep and put back the state the stream ends with).
#ifndef DM_GFX_NB_ORDER_H
#define DM_GFX_NB_ORDER_H
#include <stdint.h>
#include <string.h>
#include "nb_ir.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { NBO_MAGIC = 0x3144524f /* "ORD1" */, NBO_VERSION = 1, NBO_TAIL_OFF = 32, NBO_MAX_SEG = NB_MAX_NODES };
enum { NBO_F_VALID = 1,      // filled at submit; without it: execute as recorded (why = NBO_W_*)
       NBO_F_MERGED = 2,     // the order is not the stream order
       NBO_F_TRACKED = 4,    // the planner had every hazard (nb_frame.track == NB_TRACK_MERGE): its order needs no state-only side
       NBO_F_GLUE = 8 };     // the planner's order could not be expressed by segments: identity written
enum { NBO_E_CONT = 1,       // this pass continues the previous entry's pass (one unit: no end / begin between)
       NBO_E_PREBAR = 2 };   // barrier before this unit begins (merged orders; the PASS headers carry the same)
enum { NBO_W_OK = 0, NBO_W_UNPATCHED, NBO_W_NOPLAN, NBO_W_OVER, NBO_W_LOST, NBO_W_SPLIT, NBO_W_KINDS };
typedef struct nbo_hdr { uint32_t magic; uint8_t version, flags, why, mode; uint32_t seq; uint16_t cap, nseg, nnode, ncont; uint32_t pad; } nbo_hdr;
typedef struct nbo_ent { uint64_t addr; uint16_t node, flags; uint32_t pad; } nbo_ent;
enum { NBO_PASS_SEQ_OFF = 0, NBO_PASS_NODE_OFF = 4 };   // p_nb_pass { uint32_t seq; int16_t node; ... } (asserted by the backend)

static inline size_t nbo_frame_bytes(int cap) { return (size_t)NBO_TAIL_OFF + sizeof(nbo_hdr) + (size_t)cap * sizeof(nbo_ent); }
static inline const char *nbo_why_name(int w) {
    static const char *const n[NBO_W_KINDS] = {"ok", "not filled", "no plan", "more passes than reserved", "a pass record was not written", "passes in another recording"};
    return w >= 0 && w < NBO_W_KINDS ? n[w] : "?";
}
// The tail as reserved (payload = the FRAME record's payload, bytes = its size). Not valid until nbo_fill.
static inline void nbo_init(uint8_t *payload, size_t bytes, uint32_t seq, int mode) {
    if (bytes < nbo_frame_bytes(0)) return;
    nbo_hdr h; memset(&h, 0, sizeof h);
    h.magic = NBO_MAGIC; h.version = NBO_VERSION; h.why = NBO_W_UNPATCHED; h.mode = (uint8_t)mode; h.seq = seq;
    const size_t cap = (bytes - nbo_frame_bytes(0)) / sizeof(nbo_ent);
    h.cap = (uint16_t)(cap > NBO_MAX_SEG ? NBO_MAX_SEG : cap);
    memcpy(payload + NBO_TAIL_OFF, &h, sizeof h);
}
static inline int nbo_is_bar(const nb_node *n) { return (n->kind & 0xff) == NB_OP_BARRIER; }
// Segment order from the planner's node order. have[i] != 0: pass node i has its PASS record. m NULL = identity.
// Returns the number of segments (ent[k].node / flags filled, addr 0), or -NBO_W_OVER / -NBO_W_LOST. *merged: the order is not the
// stream order; *glue: m's order was not usable by segments (identity returned).
static inline int nbo_build(const nb_frame *f, const uint8_t *have, const nb_merge_out *m, nbo_ent *ent, int cap, int *merged, int *glue) {
    const int n = f->nnode; int ns = 0;
    *merged = 0; *glue = 0;
    for (int i = 0; i < n; i++) if (f->node[i].kind == NB_NODE_PASS) { if (!have[i]) return -NBO_W_LOST; ns++; }
    if (ns > cap) return -NBO_W_OVER;
    if (m && m->nnode == n) {
        // next[x]: the next node of x's segment that is not a barrier operation (-1: x is its last); the operations before the first
        // pass are one group of their own (their records lie before the FRAME record) and must stay first.
        static const int16_t none = -1;
        int16_t next[NB_MAX_NODES]; int last = -1, first_prefix = -1;
        for (int i = 0; i < n; i++) {
            next[i] = none;
            if (f->node[i].kind == NB_NODE_PASS) { last = i; continue; }
            if (nbo_is_bar(&f->node[i])) continue;
            if (last >= 0) next[last] = (int16_t)i; else if (first_prefix < 0) first_prefix = i;
            last = i;
        }
        int expect = first_prefix, k = 0, ok = 1;
        uint64_t seen[NB_MAX_NODES / 64]; memset(seen, 0, sizeof seen);
        for (int j = 0; j < n && ok; j++) {
            const int x = m->order[j];
            if (x < 0 || x >= n) { ok = 0; break; }
            if (f->node[x].kind != NB_NODE_PASS && nbo_is_bar(&f->node[x])) continue;
            if (expect >= 0) { if (x != expect) ok = 0; else expect = next[x]; continue; }
            if (f->node[x].kind != NB_NODE_PASS || k >= ns || (seen[x >> 6] >> (x & 63) & 1)) { ok = 0; break; }
            seen[x >> 6] |= 1ull << (x & 63);
            ent[k].addr = 0; ent[k].pad = 0; ent[k].node = (uint16_t)x;
            ent[k].flags = (uint16_t)((m->prebar[x] ? NBO_E_PREBAR : 0) | (k > 0 && m->cont[x] >= 0 && m->cont[x] == (int)ent[k - 1].node ? NBO_E_CONT : 0));
            k++; expect = next[x];
        }
        if (ok && (expect >= 0 || k != ns)) ok = 0;
        if (ok) {
            for (int i = 1; i < ns; i++) if (ent[i].node < ent[i - 1].node) *merged = 1;
            if (!*merged) for (int i = 0; i < ns; i++) ent[i].flags = 0;   // identity: the headers' own flags stand
            return ns;
        }
        *glue = 1;
    }
    int k = 0;
    for (int i = 0; i < n; i++) if (f->node[i].kind == NB_NODE_PASS) { ent[k].addr = 0; ent[k].pad = 0; ent[k].node = (uint16_t)i; ent[k].flags = 0; k++; }
    return ns;
}
// Writes the filled tail. ent[k].addr must be set. flags: NBO_F_MERGED / _TRACKED / _GLUE (VALID is added).
static inline void nbo_fill(uint8_t *payload, const nbo_ent *ent, int nseg, int nnode, unsigned flags) {
    nbo_hdr h; memcpy(&h, payload + NBO_TAIL_OFF, sizeof h);
    if (h.magic != NBO_MAGIC || nseg < 0 || nseg > (int)h.cap) return;
    int nc = 0;
    for (int i = 0; i < nseg; i++) nc += (ent[i].flags & NBO_E_CONT) != 0;
    memcpy(payload + NBO_TAIL_OFF + sizeof h, ent, (size_t)nseg * sizeof *ent);
    h.nseg = (uint16_t)nseg; h.nnode = (uint16_t)nnode; h.ncont = (uint16_t)nc; h.why = NBO_W_OK; h.flags = (uint8_t)(flags | NBO_F_VALID);
    memcpy(payload + NBO_TAIL_OFF, &h, sizeof h);
}
static inline void nbo_fail(uint8_t *payload, int why) {
    nbo_hdr h; memcpy(&h, payload + NBO_TAIL_OFF, sizeof h);
    if (h.magic != NBO_MAGIC) return;
    h.flags = 0; h.why = (uint8_t)why; h.nseg = 0;
    memcpy(payload + NBO_TAIL_OFF, &h, sizeof h);
}
enum { NBO_P_NONE = -1, NBO_P_BAD = -2, NBO_P_INVALID = -3 };
// payload / bytes: the FRAME record's payload. Returns nseg (ent filled) or NBO_P_*: NONE = no tail, INVALID = not valid (h->why),
// BAD = malformed (a node twice or outside the frame, a null / unaligned address, CONT on the first entry, an order that is not the
// stream order without NBO_F_MERGED, the wrong seq).
static inline int nbo_parse(const uint8_t *payload, size_t bytes, uint32_t seq, nbo_hdr *h, nbo_ent *ent, int cap) {
    if (!payload || bytes < nbo_frame_bytes(0)) return NBO_P_NONE;
    memcpy(h, payload + NBO_TAIL_OFF, sizeof *h);
    if (h->magic != NBO_MAGIC) return NBO_P_NONE;
    if (h->version != NBO_VERSION || h->seq != seq) return NBO_P_BAD;
    if (!(h->flags & NBO_F_VALID)) return NBO_P_INVALID;
    if (h->nseg > h->cap || (int)h->nseg > cap || h->nnode > NB_MAX_NODES || nbo_frame_bytes(h->nseg) > bytes) return NBO_P_BAD;
    memcpy(ent, payload + NBO_TAIL_OFF + sizeof *h, (size_t)h->nseg * sizeof *ent);
    uint64_t seen[NB_MAX_NODES / 64]; memset(seen, 0, sizeof seen);
    int asc = 1;
    for (int i = 0; i < (int)h->nseg; i++) {
        const unsigned x = ent[i].node;
        if (x >= h->nnode || (seen[x >> 6] >> (x & 63) & 1) || !ent[i].addr || (ent[i].addr & 7)) return NBO_P_BAD;
        seen[x >> 6] |= 1ull << (x & 63);
        if (i && ent[i].node < ent[i - 1].node) asc = 0;
        if (!i && (ent[i].flags & NBO_E_CONT)) return NBO_P_BAD;
    }
    if (!asc && !(h->flags & NBO_F_MERGED)) return NBO_P_BAD;
    return (int)h->nseg;
}

// ---- schedule ----
typedef struct nbo_step {
    const uint8_t *from, *stop;    // the segment's PASS record; where it ends (the PASS of its stream successor), NULL = at OP_END
    const uint8_t *key; uint32_t keyn;   // payload of its complete OP_X_NB_KEY (nbo_scan), NULL = none
    uint16_t node, flags;          // NBO_E_*
    uint8_t discont, last;         // does not start where the previously executed segment ended; the stream's last segment
} nbo_step;
typedef struct nbo_sched {
    int n, identity, ndiscont;
    const uint8_t *first;          // start of the stream's first segment (end of the prefix)
    nbo_step st[NBO_MAX_SEG];      // execution order
    uint16_t stream[NBO_MAX_SEG];  // stream rank -> execution index
} nbo_sched;
static inline void nbo_schedule(const nbo_ent *ent, int n, nbo_sched *sc) {
    int16_t at[NB_MAX_NODES];
    memset(at, 0xff, sizeof at);
    sc->n = n; sc->identity = 1; sc->ndiscont = 0; sc->first = NULL;
    for (int e = 0; e < n; e++) at[ent[e].node] = (int16_t)e;
    int r = 0;
    for (int x = 0; x < NB_MAX_NODES && r < n; x++) if (at[x] >= 0) sc->stream[r++] = (uint16_t)at[x];
    for (r = 0; r < n; r++) {
        const int e = sc->stream[r]; nbo_step *s = &sc->st[e];
        s->from = (const uint8_t *)(uintptr_t)ent[e].addr; s->key = NULL; s->keyn = 0; s->node = ent[e].node; s->flags = ent[e].flags;
        s->stop = r + 1 < n ? (const uint8_t *)(uintptr_t)ent[sc->stream[r + 1]].addr : NULL;
        s->last = r == n - 1;
        s->discont = r == 0 ? e != 0 : !(e > 0 && e - 1 == (int)sc->stream[r - 1]);
        if (s->discont) sc->ndiscont++;
        if (e != r) sc->identity = 0;
    }
    if (n) sc->first = sc->st[sc->stream[0]].from;
}

// ---- scan ----
typedef struct nbo_scan_io {
    void *ctx;
    uint32_t op_end, op_jump, op_pass, op_key;
    size_t (*avail)(void *ctx, const uint8_t *p);                             // bytes of command memory readable at p (0 = none)
    int (*rec)(void *ctx, uint32_t op, const uint8_t *payload, size_t n);     // 1 = writes only the state, 0 = anything else, -1 = refuse the part
    int (*key)(void *ctx, const uint8_t *payload, size_t n, uint32_t seq, int node);   // 1 = the complete key of that pass
} nbo_scan_io;
enum { NBO_S_OK = 0, NBO_S_ADDR, NBO_S_ORDER, NBO_S_MISSING, NBO_S_REFUSED, NBO_S_GUARD, NBO_S_KINDS };
typedef struct nbo_scan_out { uint32_t records; int npure, nkeyed, bad_seg; uint32_t bad_op; uint8_t pure[NBO_MAX_SEG], keyed[NBO_MAX_SEG]; } nbo_scan_out;   // by execution index
static inline const char *nbo_scan_name(int w) {
    static const char *const n[NBO_S_KINDS] = {"ok", "an address is not a PASS record of the part", "a PASS record where the tail has none",
        "the handle ended before every segment", "a record refused the part", "walk guard"};
    return w >= 0 && w < NBO_S_KINDS ? n[w] : "?";
}
// after = the record after OP_X_NB_FRAME. Fills sc->st[].key / keyn. bad_seg / bad_op: the first record that made a segment not
// state-only (execution index, op), -1 = none.
static inline int nbo_scan(nbo_sched *sc, const uint8_t *after, uint32_t seq, const nbo_scan_io *io, nbo_scan_out *o) {
    const uint8_t *pc = after; int r = 0, cur = -1, want_key = 0; uint32_t guard = 0;
    memset(o, 0, sizeof *o); o->bad_seg = -1;
    for (int e = 0; e < sc->n; e++) o->pure[e] = 1;
    for (;;) {
        if (++guard > (1u << 22)) return NBO_S_GUARD;
        int head = 0;
        if (r < sc->n && pc == sc->st[sc->stream[r]].from) { cur = sc->stream[r++]; head = 1; }
        const size_t av = io->avail(io->ctx, pc);
        uint32_t hd[2];
        if (av < 8) return NBO_S_ADDR;
        memcpy(hd, pc, 8);
        const uint32_t op = hd[0], size = hd[1];
        if (size < 8 || (size & 7) || size > av) return NBO_S_ADDR;
        const uint8_t *p = pc + 8; const size_t n = size - 8;
        if (op == io->op_end) { if (head) return NBO_S_ADDR; break; }
        if (op == io->op_jump) {
            uint64_t t;
            if (head || n < 8) return NBO_S_ADDR;
            memcpy(&t, p, 8); pc = (const uint8_t *)(uintptr_t)t; continue;
        }
        o->records++;
        if (op == io->op_pass) {
            uint32_t s2; int16_t nd;
            if (n < 8) return NBO_S_ADDR;
            memcpy(&s2, p + NBO_PASS_SEQ_OFF, 4); memcpy(&nd, p + NBO_PASS_NODE_OFF, 2);
            if (head) { if (s2 != seq || (uint16_t)nd != sc->st[cur].node) return NBO_S_ADDR; want_key = 1; }
            else if (s2 == seq) return NBO_S_ORDER;
        } else {
            if (head) return NBO_S_ADDR;
            if (want_key) {
                want_key = 0;
                if (op == io->op_key && io->key(io->ctx, p, n, seq, (int)sc->st[cur].node)) { sc->st[cur].key = p; sc->st[cur].keyn = (uint32_t)n; o->keyed[cur] = 1; }
            }
        }
        const int c = io->rec(io->ctx, op, p, n);
        if (c < 0) return NBO_S_REFUSED;
        if (!c && cur >= 0 && o->pure[cur]) { o->pure[cur] = 0; if (o->bad_seg < 0) { o->bad_seg = cur; o->bad_op = op; } }
        pc += size;
    }
    if (r != sc->n) return NBO_S_MISSING;
    for (int e = 0; e < sc->n; e++) { o->npure += o->pure[e]; o->nkeyed += o->keyed[e]; }
    return NBO_S_OK;
}
// Discontinuous starts without a complete key (0 = the order can run).
static inline int nbo_unkeyed(const nbo_sched *sc) {
    int k = 0;
    for (int e = 0; e < sc->n; e++) if (sc->st[e].discont && !sc->st[e].key) k++;
    return k;
}
// Inversions with no state-only side and no tracked planner behind them (0 = the order can run).
static inline int nbo_unsafe(const nbo_sched *sc, const uint8_t *pure, int tracked) {
    if (tracked || sc->identity) return 0;
    int bad = 0;
    for (int a = 0; a < sc->n; a++) {
        if (pure[a]) continue;
        for (int b = a + 1; b < sc->n; b++) if (!pure[b] && sc->st[a].node > sc->st[b].node) bad++;
    }
    return bad;
}

// ---- run ----
typedef struct nbo_ops {
    void *ctx;
    // Executes the records from `from` until the next record is at `stop` (returns NULL) or is the OP_END of this level (returns its
    // address; the OP_END is not executed). e = execution index of the segment, -1 = the prefix.
    const uint8_t *(*exec)(void *ctx, int e, const uint8_t *from, const uint8_t *stop);
    int (*start)(void *ctx, int e, const nbo_step *s);   // the state from the step's key; 0 = failed
    void (*keep)(void *ctx);                             // the state the stream ends with (the stream's last segment was not executed last)
    void (*put)(void *ctx);                              // put it back
} nbo_ops;
enum { NBO_R_OK = 0, NBO_R_EARLY_END, NBO_R_NO_END, NBO_R_START };
// Returns where the caller goes on (the OP_END of the handle). *res != NBO_R_OK: the stream was not what the tail said. EARLY_END with
// the identity order is harmless (everything before was executed as recorded; the caller goes on at the OP_END met); any other failure
// of an order that is not the identity means records were skipped or executed from the wrong state (the callers scan first, so it
// cannot happen with an intact stream).
static inline const uint8_t *nbo_run(const nbo_sched *sc, const uint8_t *after, const nbo_ops *o, int *res, int *done) {
    const uint8_t *end = o->exec(o->ctx, -1, after, sc->first), *resume = NULL;
    int kept = 0;
    *res = NBO_R_OK; *done = 0;
    if (end) { if (sc->n) *res = NBO_R_EARLY_END; return end; }
    for (int e = 0; e < sc->n; e++) {
        const nbo_step *s = &sc->st[e];
        if (s->discont && !o->start(o->ctx, e, s)) { *res = NBO_R_START; break; }
        end = o->exec(o->ctx, e, s->from, s->stop);
        ++*done;
        if (s->last) {
            if (!end) { *res = NBO_R_NO_END; break; }
            resume = end;
            if (e != sc->n - 1) { o->keep(o->ctx); kept = 1; }
        } else if (end) { *res = NBO_R_EARLY_END; resume = end; break; }
    }
    if (kept) o->put(o->ctx);
    return resume;
}

#ifdef __cplusplus
}
#endif
#endif
