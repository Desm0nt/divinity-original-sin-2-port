// [dos2-nb] NB-21: the frame as one job (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-21 findings"). Pure C, inline, no Vulkan, no game: the
// NVN tap (dm/gfx/nvn/nvn_nb_own.inc), the translation queue (dm/gfx/nvn/hle_nvn.c, XJ_OWN), the backend
// (dm/gfx/vulkan/engine/xbg_nb_own.inc) and the host test (tests/nb_own_test.c) run the same code.
//
// nbw_job: what the front end knows about one submission of the frame IR when it is planned, handed to the backend WITH the job that
// submits it (not written into the command stream: DM_NB_OWN never changes a record):
//   - the part: seq (= p_nb_frame.seq), frame number, part, final, planned; the address of its OP_X_NB_FRAME record; the recording that
//     holds that record and the recording the submission is recognised by (they differ when the IR ran across an EndRecording);
//   - the IR skeleton: the kind of every node (pass / operation kind), in stream order;
//   - the segments (nb_order.h: a segment = the records from a pass's OP_X_NB_PASS up to the next pass's), in STREAM order: address of
//     the PASS record, of its OP_X_NB_KEY (0 = none), the recording that holds it, the node, the NBO_E_* flags of the order;
//   - the tail: order[] = execution order as indices into seg[] (identity unless DM_NB_ORDER gave a valid order tail).
// The record memory is live until the handle is translated (the assumption of NB-13a / NB-14 / NB-17a); the job itself lives in a pool
// of the tap and is released by the translation thread after its submission was executed.
//
// nbw_hand: the hand-off rule of the queue. The final submit of a frame (inside EndFrame with the present bit, on the render thread,
// with a job) is queued at once, as before - the translation thread starts on it at once - but as an OPEN job. The present that follows
// in the same EndFrame is attached to that job (with the frame number) instead of becoming two more jobs, when the job is still the
// newest in the queue and its submission is not translated yet; the translation thread, when it has translated the submission, closes
// the job and runs what was attached. A present that comes too late, behind another job, or from another thread is queued the old
// way. So nothing is ever held back: the order of the steps the translation thread executes and of the submit numbers is the old one,
// and no waiter can wait for something that is not in the queue.
//
// nbw_chk: what the backend met while it executed the job's submission against the job (FRAME record at the job's address with the
// job's facts, every PASS record of the part at its segment's address, each exactly once, in the stream order or in the job's order).
#ifndef DM_GFX_NB_OWN_H
#define DM_GFX_NB_OWN_H
#include <stdint.h>
#include <string.h>
#include "nb_ir.h"
#include "nb_order.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { NBW_MAGIC = 0x314e574f /* "OWN1" */, NBW_MAX_SEG = NB_MAX_NODES };
enum { NBW_F_FINAL = 1,      // the last submission of the frame
       NBW_F_PLANNED = 2,    // the planner ran (the PASS headers carry a plan)
       NBW_F_ORDER = 4,      // order[] / seg[].flags come from a valid order tail (DM_NB_ORDER); else identity
       NBW_F_MERGED = 8,     // order[] is not the stream order
       NBW_F_KEYS = 16,      // every segment has its OP_X_NB_KEY (DM_NB_RELOC)
       NBW_F_SPLIT = 32 };   // a segment lies in another recording than the FRAME record, or the submitted recording is another one
typedef struct nbw_src { uint64_t pass, key, rec; } nbw_src;   // by node: PASS record header, KEY record header (0), recording (handle)
typedef struct nbw_seg { uint64_t pass, key, rec; uint16_t node, flags; uint32_t pad; } nbw_seg;
typedef struct nbw_job {
    uint32_t magic, seq, frame_no, part;
    uint64_t frame_rec, handle, last;     // OP_X_NB_FRAME record header; its recording; the recording the submission is recognised by
    uint16_t nnode, npass, nseg, flags;
    uint32_t nb_bytes, pad;
    uint16_t order[NBW_MAX_SEG];          // execution order -> index into seg[]
    uint8_t kind[NB_MAX_NODES];           // IR skeleton: NB_NODE_PASS / NB_OP_*
    nbw_seg seg[NBW_MAX_SEG];             // stream order (ascending node)
} nbw_job;

enum { NBW_B_NOFRAME = 1, NBW_B_LOST, NBW_B_KINDS };
static inline const char *nbw_build_name(int w) {
    static const char *const n[NBW_B_KINDS] = {"ok", "no FRAME record (no pass in the part)", "a pass record was not written"};
    return w >= 0 && w < NBW_B_KINDS ? n[w] : "?";
}
// src[node] for the pass nodes of f. ord / nord: the entries of a valid order tail in execution order (NULL = identity).
// Returns the number of segments (> 0) or -NBW_B_*.
static inline int nbw_build(nbw_job *j, const nb_frame *f, const nbw_src *src, uint64_t frame_rec, uint64_t handle, uint64_t last, uint32_t seq,
                            uint32_t part, int planned, uint32_t nb_bytes, const nbo_ent *ord, int nord) {
    const int n = f->nnode;
    if (!frame_rec || (frame_rec & 7) || n <= 0 || n > NB_MAX_NODES) return -NBW_B_NOFRAME;
    int ns = 0, keys = 1, split = handle != last;
    for (int i = 0; i < n; i++) {
        j->kind[i] = (uint8_t)(f->node[i].kind & 0xff);
        if (f->node[i].kind != NB_NODE_PASS) continue;
        const nbw_src *s = &src[i];
        if (!s->pass || (s->pass & 7)) return -NBW_B_LOST;
        nbw_seg *g = &j->seg[ns];
        g->pass = s->pass; g->key = s->key; g->rec = s->rec; g->node = (uint16_t)i; g->flags = 0; g->pad = 0;
        if (!s->key) keys = 0;
        if (s->rec != handle) split = 1;
        j->order[ns] = (uint16_t)ns; ns++;
    }
    if (!ns) return -NBW_B_NOFRAME;
    unsigned fl = (f->final ? NBW_F_FINAL : 0) | (planned ? NBW_F_PLANNED : 0) | (keys ? NBW_F_KEYS : 0) | (split ? NBW_F_SPLIT : 0);
    if (ord && nord == ns) {   // execution order by node -> by segment; anything odd keeps the identity
        int ok = 1, merged = 0; uint64_t seen[NBW_MAX_SEG / 64]; memset(seen, 0, sizeof seen);
        uint16_t o[NBW_MAX_SEG];
        for (int k = 0; k < ns && ok; k++) {
            int lo = 0, hi = ns - 1, at = -1;
            while (lo <= hi) { const int m = (lo + hi) / 2; if (j->seg[m].node == ord[k].node) { at = m; break; } if (j->seg[m].node < ord[k].node) lo = m + 1; else hi = m - 1; }
            if (at < 0 || (seen[at >> 6] >> (at & 63) & 1) || (!k && (ord[k].flags & NBO_E_CONT))) { ok = 0; break; }
            seen[at >> 6] |= 1ull << (at & 63); o[k] = (uint16_t)at;
            if (at != k) merged = 1;
        }
        if (ok) {
            for (int k = 0; k < ns; k++) { j->order[k] = o[k]; j->seg[o[k]].flags = ord[k].flags; }
            fl |= NBW_F_ORDER | (merged ? NBW_F_MERGED : 0);
        }
    }
    j->magic = NBW_MAGIC; j->seq = seq; j->frame_no = f->frame_no; j->part = part; j->frame_rec = frame_rec; j->handle = handle; j->last = last;
    j->nnode = (uint16_t)n; j->npass = (uint16_t)ns; j->nseg = (uint16_t)ns; j->flags = (uint16_t)fl; j->nb_bytes = nb_bytes; j->pad = 0;
    return ns;
}
enum { NBW_V_OK = 0, NBW_V_MAGIC, NBW_V_COUNTS, NBW_V_ADDR, NBW_V_NODES, NBW_V_ORDER, NBW_V_KINDS };
static inline const char *nbw_valid_name(int w) {
    static const char *const n[NBW_V_KINDS] = {"ok", "no magic", "counts", "an address is null or unaligned", "segment nodes", "order is not a permutation"};
    return w >= 0 && w < NBW_V_KINDS ? n[w] : "?";
}
// The backend's check before it trusts a job (the job is host memory written by the front end; this guards the hand-off, not the game).
static inline int nbw_valid(const nbw_job *j) {
    if (!j || j->magic != NBW_MAGIC) return NBW_V_MAGIC;
    if (!j->nseg || j->nseg > NBW_MAX_SEG || j->nnode > NB_MAX_NODES || j->nseg > j->nnode || j->npass != j->nseg) return NBW_V_COUNTS;
    if (!j->frame_rec || (j->frame_rec & 7) || !j->last) return NBW_V_ADDR;
    uint64_t seen[NBW_MAX_SEG / 64]; memset(seen, 0, sizeof seen);
    int np = 0;
    for (int i = 0; i < (int)j->nnode; i++) np += j->kind[i] == NB_NODE_PASS;
    if (np != (int)j->nseg) return NBW_V_COUNTS;
    for (int k = 0; k < (int)j->nseg; k++) {
        const nbw_seg *g = &j->seg[k];
        if (!g->pass || (g->pass & 7) || (g->key & 7)) return NBW_V_ADDR;
        if (g->node >= j->nnode || j->kind[g->node] != NB_NODE_PASS || (k && g->node <= j->seg[k - 1].node)) return NBW_V_NODES;
        const unsigned o = j->order[k];
        if (o >= j->nseg || (seen[o >> 6] >> (o & 63) & 1)) return NBW_V_ORDER;
        seen[o >> 6] |= 1ull << (o & 63);
        if (o != (unsigned)k && !(j->flags & NBW_F_MERGED)) return NBW_V_ORDER;
    }
    if (j->seg[j->order[0]].flags & NBO_E_CONT) return NBW_V_ORDER;
    return NBW_V_OK;
}
// Where segment k (stream index) ends: the PASS record of its stream successor when that lies in the same recording, else 0 = it runs to
// the OP_END of its recording. A worker (NB-20b) can take [seg[k].pass, nbw_stop(j, k)) with seg[k].key as its start state.
static inline uint64_t nbw_stop(const nbw_job *j, int k) {
    return k + 1 < (int)j->nseg && j->seg[k + 1].rec == j->seg[k].rec ? j->seg[k + 1].pass : 0;
}
static inline int nbw_seg_of(const nbw_job *j, unsigned node) {
    int lo = 0, hi = (int)j->nseg - 1;
    while (lo <= hi) { const int m = (lo + hi) / 2; if (j->seg[m].node == node) return m; if (j->seg[m].node < node) lo = m + 1; else hi = m - 1; }
    return -1;
}

// ---- the backend's compare ----
enum { NBW_D_HANDLE = 1,          // the job's recording is not among the handles of its submission
       NBW_D_NOFRAME = 2,         // its OP_X_NB_FRAME record was never executed
       NBW_D_FRAME_ADDR = 4,      // a FRAME record with the job's seq at another address
       NBW_D_FRAME_FACTS = 8,     // frame number / part / nodes / passes / final differ
       NBW_D_FRAME_TWICE = 16,    // the job's FRAME record executed again
       NBW_D_OTHER_PART = 32,     // a FRAME / PASS record of another seq in the job's submission
       NBW_D_PASS_UNKNOWN = 64,   // a PASS record of the job's seq whose node is not a segment
       NBW_D_PASS_ADDR = 128,     // a segment's PASS record at another address
       NBW_D_PASS_TWICE = 256,
       NBW_D_PASS_MISSING = 512,  // a segment whose PASS record was never executed
       NBW_D_ORDER = 1024,        // executed neither in the stream order nor in the job's order
       NBW_D_EARLY = 2048,        // a PASS record of the job before its FRAME record
       NBW_D_BITS = 12 };
static inline const char *nbw_diff_name(int bit) {
    static const char *const n[NBW_D_BITS] = {"recording not submitted", "FRAME not executed", "FRAME address", "FRAME facts", "FRAME twice",
        "records of another part", "PASS of no segment", "PASS address", "PASS twice", "PASS missing", "order", "PASS before FRAME"};
    return bit >= 0 && bit < NBW_D_BITS ? n[bit] : "?";
}
typedef struct nbw_chk {
    const nbw_job *j; uint32_t diff, frames, passes, other, next;
    uint8_t in_stream, in_order; uint8_t pad[2];
    uint64_t seen[NBW_MAX_SEG / 64];
} nbw_chk;
static inline void nbw_chk_begin(nbw_chk *c, const nbw_job *j, const uint64_t *handles, int n) {
    memset(c, 0, sizeof *c); c->j = j; c->in_stream = c->in_order = 1;
    int mine = 0;
    for (int i = 0; i < n; i++) if (handles[i] == j->last) mine = 1;
    if (!mine) c->diff |= NBW_D_HANDLE;
}
static inline void nbw_chk_frame(nbw_chk *c, uint64_t rec, uint32_t seq, uint32_t frame_no, uint32_t part, uint32_t nnode, uint32_t npass, int final) {
    const nbw_job *j = c->j;
    if (seq != j->seq) { c->other++; c->diff |= NBW_D_OTHER_PART; return; }
    if (c->frames++) { c->diff |= NBW_D_FRAME_TWICE; return; }
    if (rec != j->frame_rec) c->diff |= NBW_D_FRAME_ADDR;
    if (frame_no != j->frame_no || part != j->part || nnode != j->nnode || npass != j->npass || (final != 0) != ((j->flags & NBW_F_FINAL) != 0)) c->diff |= NBW_D_FRAME_FACTS;
}
static inline void nbw_chk_pass(nbw_chk *c, uint64_t rec, uint32_t seq, unsigned node) {
    const nbw_job *j = c->j;
    if (seq != j->seq) { c->other++; c->diff |= NBW_D_OTHER_PART; return; }
    if (!c->frames) c->diff |= NBW_D_EARLY;
    const uint32_t k = c->next++;
    int at = k < j->nseg && j->seg[k].node == node ? (int)k : nbw_seg_of(j, node);
    if (at < 0) { c->diff |= NBW_D_PASS_UNKNOWN; c->in_stream = c->in_order = 0; return; }
    if (at != (int)k) c->in_stream = 0;
    if (k >= j->nseg || j->order[k] != (uint16_t)at) c->in_order = 0;
    if (j->seg[at].pass != rec) c->diff |= NBW_D_PASS_ADDR;
    if (c->seen[at >> 6] >> (at & 63) & 1) c->diff |= NBW_D_PASS_TWICE; else { c->seen[at >> 6] |= 1ull << (at & 63); c->passes++; }
}
// Returns the NBW_D_* bits (0 = the submission was the job).
static inline uint32_t nbw_chk_end(nbw_chk *c) {
    const nbw_job *j = c->j;
    if (!c->frames) c->diff |= NBW_D_NOFRAME;
    if (c->passes != j->nseg) c->diff |= NBW_D_PASS_MISSING;
    if (!c->in_stream && !c->in_order) c->diff |= NBW_D_ORDER;
    return c->diff;
}

// ---- the hand-off rule ----
enum { NBW_P_SUBMIT = 1, NBW_P_PRESENT = 2, NBW_P_FRAME = 4,
       NBW_P_CLOSED = 8 };   // the translation thread has translated the submission: nothing can be attached any more
enum { NBW_H_FINAL = 0,      // final submits with a job seen inside an armed EndFrame
       NBW_H_OPEN,           // queued as the frame's open job (=2: would have been)
       NBW_H_ONE,            // the present and the frame number attached: one job a frame (=2: would have been)
       NBW_H_LATE,           // the present came after the submission was translated: the old jobs
       NBW_H_BEHIND,         // another job was queued between the submit and the present: the old jobs
       NBW_H_NOPRESENT,      // EndFrame returned without a present
       NBW_H_UNARMED,        // a job outside an armed EndFrame (mid-frame part, EndFrame without the present bit)
       NBW_H_NOJOB,          // a submit inside an armed EndFrame without a job (not the IR's recording / no pass / no free job)
       NBW_H_SYNC,           // DM_NVN_SYNC: jobs run in place, nothing can be attached
       NBW_H_PRESENT_OLD,    // a present with no open job (after one of the cases above)
       NBW_H_N };
// State of the armed (render) thread only: which job of the queue is this frame's open one. What is attached lives in the queued job.
typedef struct nbw_hand { uint8_t open, virt; uint8_t pad[6]; uint64_t job, sub; uint64_t st[NBW_H_N]; } nbw_hand;
// A submit is about to be queued. armed = render thread inside EndFrame with the present bit; job = it carries a frame job; sync = jobs
// run in place. Returns 1 = queue it as the frame's open job (at =2: as before) and report its numbers with nbw_hand_opened.
static inline int nbw_hand_submit(nbw_hand *h, int armed, int job, int sync) {
    h->open = 0;   // a frame has one
    if (!armed) { if (job) h->st[NBW_H_UNARMED]++; return 0; }
    if (!job) { h->st[NBW_H_NOJOB]++; return 0; }
    h->st[NBW_H_FINAL]++;
    if (sync) { h->st[NBW_H_SYNC]++; return 0; }
    return 1;
}
// The submit got its queue job number and submit number. virt = check mode (it was queued as an ordinary submit job).
static inline void nbw_hand_opened(nbw_hand *h, uint64_t job, uint64_t sub, int virt) {
    h->open = 1; h->virt = (uint8_t)(virt != 0); h->job = job; h->sub = sub; h->st[NBW_H_OPEN]++;
}
// The present of the armed thread. jobs_in = the number of the newest job in the queue; done = the open job's submission is already
// translated (the job is closed). Returns 1 = attach the present and the frame number to the open job (the caller queues nothing),
// 0 = queue PRESENT and FRAME as before (always at =2).
static inline int nbw_hand_present(nbw_hand *h, int armed, uint64_t jobs_in, int done) {
    if (!h->open || !armed) { h->st[NBW_H_PRESENT_OLD]++; return 0; }
    h->open = 0;
    if (jobs_in != h->job) { h->st[NBW_H_BEHIND]++; return 0; }
    if (done) { h->st[NBW_H_LATE]++; return 0; }
    h->st[NBW_H_ONE]++;
    return !h->virt;
}
// EndFrame returned.
static inline void nbw_hand_end(nbw_hand *h) { if (h->open) { h->open = 0; h->st[NBW_H_NOPRESENT]++; } }

#ifdef __cplusplus
}
#endif
#endif
