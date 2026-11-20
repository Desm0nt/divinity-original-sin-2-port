// [DOS2] NB-21 host test: the frame job, the backend's compare and the hand-off rule of dm/gfx/nb/nb_own.h - the code the NVN tap
// (nvn_nb_own.inc), the translation queue (hle_nvn.c XJ_OWN) and the backend (xbg_nb_own.inc) run.
// - job: nbw_build on synthetic frames (passes and operations mixed; keys or not; a pass in another recording; no pass; a pass record not
//   written; an order tail: identity, moved, malformed -> identity), nbw_valid on the result and on every corruption, nbw_stop;
// - compare: a backend model executes the job's records (stream order, the job's order); nothing differs; mutations of the executed
//   stream (a segment skipped / run twice / at another address, a PASS of no segment, records of another part, FRAME missing / twice /
//   elsewhere / other facts, PASS before FRAME, a shuffled order, the recording not among the handles) are each reported by their bit;
// - hand-off: a model of the queue with the structure of hle_nvn.c (x_push, x_run_own with the close under the lock, x_own_submit,
//   x_own_present, the same nbw_hand_* calls) is driven by random scripts (frames with and without the present bit, with and without a
//   job, mid-frame parts, jobs / presents of other threads and a drain between the submit and its present, a full queue, the
//   translation thread at every point of the frame's job: not started, translating, closed, done). For every script the steps the
//   translation thread executes (submit, present, frame number, with their submit numbers) are exactly the steps of the old path, every
//   present ticket names the job that runs the present, an attached present saves exactly two jobs, and DM_NB_OWN=2 keeps the old queue
//   and counts the same cases. Mutations of the model (attach without the "newest job" check, without the "closed" check, a job never
//   closed, the queued job not read again after the submission, the present queued as well as attached, what is attached kept in one
//   place instead of in the queued job) are each caught.
// Build + run (from this directory):
//   clang -std=c11 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS nb_own_test.c -o nb_own_test && ./nb_own_test
#include "../../../nb/nb_own.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
static uint64_t rnd_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 7; rnd_state ^= rnd_state << 17; return rnd_state; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---------------- job ----------------
static nb_frame g_f; static nbw_src g_src[NB_MAX_NODES]; static nbw_job g_j, g_j2;
enum { REC_A = 0x10000, REC_B = 0x90000, FRAME_AT = REC_A + 0x40 };
// kinds: 'P' pass, anything else an operation. Records are laid out 0x400 apart in REC_A; split_from >= 0: passes from that node on lie
// in REC_B. keys: 1 = every pass, 0 = none, 2 = every second.
static int make(const char *kinds, int keys, int split_from, int lost_node) {
    memset(&g_f, 0, sizeof g_f); memset(g_src, 0, sizeof g_src);
    int n = 0, np = 0;
    for (; kinds[n]; n++) {
        const int pass = kinds[n] == 'P';
        g_f.node[n].kind = (uint16_t)(pass ? NB_NODE_PASS : NB_OP_COPY + (n % 3));
        if (!pass) continue;
        const uint64_t base = split_from >= 0 && n >= split_from ? REC_B : REC_A;
        if (n != lost_node) g_src[n].pass = base + 0x400 + (uint64_t)n * 0x400;
        g_src[n].rec = base;
        if (keys == 1 || (keys == 2 && (np & 1))) g_src[n].key = g_src[n].pass + 0x280;
        np++;
    }
    g_f.nnode = n; g_f.frame_no = 77; g_f.final = 1;
    return np;
}
static void test_job(void) {
    int np = make("PoPPooP", 1, -1, -1);
    int ns = nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 5, 0, 1, 1234, NULL, 0);
    CHECK(ns == np && ns == 4 && g_j.nseg == 4 && g_j.npass == 4 && g_j.nnode == 7 && g_j.seq == 5 && g_j.frame_no == 77 && g_j.nb_bytes == 1234);
    CHECK(g_j.flags == (NBW_F_FINAL | NBW_F_PLANNED | NBW_F_KEYS));
    CHECK(g_j.seg[0].node == 0 && g_j.seg[1].node == 2 && g_j.seg[2].node == 3 && g_j.seg[3].node == 6);
    CHECK(g_j.kind[0] == NB_NODE_PASS && g_j.kind[1] != NB_NODE_PASS && g_j.kind[6] == NB_NODE_PASS);
    for (int k = 0; k < ns; k++) CHECK(g_j.order[k] == k && g_j.seg[k].pass == g_src[g_j.seg[k].node].pass && g_j.seg[k].key == g_j.seg[k].pass + 0x280 && g_j.seg[k].rec == REC_A);
    CHECK(nbw_valid(&g_j) == NBW_V_OK);
    CHECK(nbw_stop(&g_j, 0) == g_j.seg[1].pass && nbw_stop(&g_j, 2) == g_j.seg[3].pass && nbw_stop(&g_j, 3) == 0);
    CHECK(nbw_seg_of(&g_j, 3) == 2 && nbw_seg_of(&g_j, 1) == -1 && nbw_seg_of(&g_j, 99) == -1);
    // no plan, not final, some keys missing
    make("PPP", 2, -1, -1); g_f.final = 0;
    CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 6, 2, 0, 0, NULL, 0) == 3 && g_j.flags == 0 && g_j.part == 2 && nbw_valid(&g_j) == NBW_V_OK);
    CHECK(g_j.seg[0].key == 0 && g_j.seg[1].key != 0);
    // a pass in another recording: the segment before it runs to the end of its recording
    make("PPoPP", 1, 3, -1);
    CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_B, 7, 0, 1, 0, NULL, 0) == 4 && (g_j.flags & NBW_F_SPLIT) && nbw_valid(&g_j) == NBW_V_OK);
    CHECK(nbw_stop(&g_j, 0) == g_j.seg[1].pass && nbw_stop(&g_j, 1) == 0 && nbw_stop(&g_j, 2) == g_j.seg[3].pass && g_j.seg[2].rec == REC_B && g_j.last == REC_B);
    // the same recording everywhere but another one submitted: still split
    make("PP", 1, -1, -1);
    CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_B, 7, 0, 1, 0, NULL, 0) == 2 && (g_j.flags & NBW_F_SPLIT));
    // no job
    make("ooo", 1, -1, -1);
    CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 8, 0, 1, 0, NULL, 0) == -NBW_B_NOFRAME);
    make("PoP", 1, -1, -1);
    CHECK(nbw_build(&g_j, &g_f, g_src, 0, REC_A, REC_A, 8, 0, 1, 0, NULL, 0) == -NBW_B_NOFRAME);
    CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT + 4, REC_A, REC_A, 8, 0, 1, 0, NULL, 0) == -NBW_B_NOFRAME);
    make("PoP", 1, -1, 2);
    CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 8, 0, 1, 0, NULL, 0) == -NBW_B_LOST);
    g_f.nnode = 0;
    CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 8, 0, 1, 0, NULL, 0) == -NBW_B_NOFRAME);
    // the order tail: identity, moved, malformed
    np = make("PPoPP", 1, -1, -1);
    nbo_ent e[8]; memset(e, 0, sizeof e);
    const uint16_t ident[4] = {0, 1, 3, 4}, moved[4] = {0, 3, 1, 4};
    for (int k = 0; k < 4; k++) e[k].node = ident[k];
    CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 9, 0, 1, 0, e, 4) == 4 && (g_j.flags & NBW_F_ORDER) && !(g_j.flags & NBW_F_MERGED) && nbw_valid(&g_j) == NBW_V_OK);
    for (int k = 0; k < 4; k++) e[k].node = moved[k];
    e[2].flags = NBO_E_PREBAR; e[3].flags = NBO_E_CONT;
    CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 9, 0, 1, 0, e, 4) == 4 && (g_j.flags & NBW_F_ORDER) && (g_j.flags & NBW_F_MERGED) && nbw_valid(&g_j) == NBW_V_OK);
    CHECK(g_j.order[0] == 0 && g_j.order[1] == 2 && g_j.order[2] == 1 && g_j.order[3] == 3);
    CHECK(g_j.seg[1].flags == NBO_E_PREBAR && g_j.seg[3].flags == NBO_E_CONT && g_j.seg[2].flags == 0);
    for (int bad = 0; bad < 4; bad++) {
        for (int k = 0; k < 4; k++) { e[k].node = moved[k]; e[k].flags = 0; }
        int n = 4;
        if (bad == 0) e[1].node = 2;            // an operation node
        if (bad == 1) e[2].node = 3;            // a node twice
        if (bad == 2) e[0].flags = NBO_E_CONT;  // CONT on the first entry
        if (bad == 3) n = 3;                    // not every pass
        CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 9, 0, 1, 0, e, n) == 4 && !(g_j.flags & (NBW_F_ORDER | NBW_F_MERGED)) && nbw_valid(&g_j) == NBW_V_OK);
        for (int k = 0; k < 4; k++) CHECK(g_j.order[k] == k && g_j.seg[k].flags == 0);
    }
    // the largest frame
    static char big[NB_MAX_NODES + 1]; memset(big, 'P', NB_MAX_NODES); big[NB_MAX_NODES] = 0;
    CHECK(make(big, 1, -1, -1) == NB_MAX_NODES && nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 9, 0, 1, 0, NULL, 0) == NB_MAX_NODES && nbw_valid(&g_j) == NBW_V_OK);
    // corruptions
    make("PoPPooP", 1, -1, -1);
    CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 5, 0, 1, 0, NULL, 0) == 4);
    CHECK(nbw_valid(NULL) == NBW_V_MAGIC);
#define BAD(stmt, why) do { g_j2 = g_j; stmt; CHECK(nbw_valid(&g_j2) == (why)); } while (0)
    BAD(g_j2.magic ^= 1, NBW_V_MAGIC);
    BAD(g_j2.nseg = 0, NBW_V_COUNTS);
    BAD(g_j2.nseg = 5, NBW_V_COUNTS);
    BAD(g_j2.npass = 3, NBW_V_COUNTS);
    BAD(g_j2.nnode = NB_MAX_NODES + 1, NBW_V_COUNTS);
    BAD(g_j2.nnode = 3, NBW_V_COUNTS);
    BAD(g_j2.kind[1] = NB_NODE_PASS, NBW_V_COUNTS);
    BAD(g_j2.frame_rec = 0, NBW_V_ADDR);
    BAD(g_j2.frame_rec += 4, NBW_V_ADDR);
    BAD(g_j2.last = 0, NBW_V_ADDR);
    BAD(g_j2.seg[2].pass = 0, NBW_V_ADDR);
    BAD(g_j2.seg[2].pass += 2, NBW_V_ADDR);
    BAD(g_j2.seg[1].key += 1, NBW_V_ADDR);
    BAD(g_j2.seg[1].node = 1; g_j2.kind[1] = NB_NODE_PASS; g_j2.kind[2] = NB_OP_COPY, NBW_V_OK);   // consistent again
    BAD(g_j2.seg[1].node = 1, NBW_V_NODES);          // an operation node
    BAD(g_j2.seg[2].node = 2, NBW_V_NODES);          // not ascending
    BAD(g_j2.seg[3].node = 9, NBW_V_NODES);          // outside the frame
    BAD(g_j2.order[1] = 2, NBW_V_ORDER);             // twice (and not merged)
    BAD(g_j2.order[1] = 2; g_j2.order[2] = 1, NBW_V_ORDER);   // moved without the flag
    BAD(g_j2.order[1] = 2; g_j2.order[2] = 1; g_j2.flags |= NBW_F_MERGED, NBW_V_OK);
    BAD(g_j2.order[3] = 7; g_j2.flags |= NBW_F_MERGED, NBW_V_ORDER);
    BAD(g_j2.seg[0].flags = NBO_E_CONT, NBW_V_ORDER);
#undef BAD
}

// ---------------- compare ----------------
enum { MUT_NONE, MUT_SKIP, MUT_TWICE, MUT_ADDR, MUT_UNKNOWN, MUT_OTHER_PASS, MUT_OTHER_FRAME, MUT_NO_FRAME, MUT_FRAME_TWICE, MUT_FRAME_ADDR, MUT_FRAME_FACTS,
       MUT_EARLY, MUT_SHUFFLE, MUT_HANDLE, MUT_N };
// The backend model: executes FRAME, then the PASS records in `order` (indices into seg[]).
static uint32_t run_model(const nbw_job *j, const uint16_t *order, int mut, int at) {
    nbw_chk c; const uint64_t h[2] = {0x5000, mut == MUT_HANDLE ? j->last + 8 : j->last};
    uint16_t o[NBW_MAX_SEG]; const int n = j->nseg;
    memcpy(o, order, sizeof o[0] * (size_t)n);
    if (mut == MUT_SHUFFLE) { const uint16_t t = o[at]; o[at] = o[(at + 1) % n]; o[(at + 1) % n] = t; }
    nbw_chk_begin(&c, j, h, 2);
    if (mut == MUT_EARLY) nbw_chk_pass(&c, j->seg[o[0]].pass, j->seq, j->seg[o[0]].node);
    if (mut != MUT_NO_FRAME) nbw_chk_frame(&c, j->frame_rec + (mut == MUT_FRAME_ADDR ? 0x100 : 0), j->seq, j->frame_no + (mut == MUT_FRAME_FACTS && at % 5 == 0),
                                           j->part + (mut == MUT_FRAME_FACTS && at % 5 == 1), j->nnode + (mut == MUT_FRAME_FACTS && at % 5 == 2),
                                           j->npass + (mut == MUT_FRAME_FACTS && at % 5 == 3), ((j->flags & NBW_F_FINAL) != 0) ^ (mut == MUT_FRAME_FACTS && at % 5 == 4));
    if (mut == MUT_FRAME_TWICE) nbw_chk_frame(&c, j->frame_rec, j->seq, j->frame_no, j->part, j->nnode, j->npass, (j->flags & NBW_F_FINAL) != 0);
    if (mut == MUT_OTHER_FRAME) nbw_chk_frame(&c, j->frame_rec + 0x4000, j->seq + 1, j->frame_no, 1, 3, 2, 0);
    for (int k = mut == MUT_EARLY ? 1 : 0; k < n; k++) {
        const nbw_seg *g = &j->seg[o[k]];
        if (mut == MUT_SKIP && k == at) continue;
        if (mut == MUT_OTHER_PASS && k == at) nbw_chk_pass(&c, g->pass + 0x40, j->seq + 1, g->node);
        if (mut == MUT_UNKNOWN && k == at) { nbw_chk_pass(&c, g->pass, j->seq, (unsigned)j->nnode + 3); continue; }
        nbw_chk_pass(&c, g->pass + (mut == MUT_ADDR && k == at ? 8 : 0), j->seq, g->node);
        if (mut == MUT_TWICE && k == at) nbw_chk_pass(&c, g->pass, j->seq, g->node);
    }
    return nbw_chk_end(&c);
}
static unsigned g_cmp_runs, g_cmp_caught[MUT_N];
static void test_compare(void) {
    static const uint32_t want[MUT_N] = {0, NBW_D_PASS_MISSING, NBW_D_PASS_TWICE, NBW_D_PASS_ADDR, NBW_D_PASS_UNKNOWN, NBW_D_OTHER_PART, NBW_D_OTHER_PART, NBW_D_NOFRAME,
        NBW_D_FRAME_TWICE, NBW_D_FRAME_ADDR, NBW_D_FRAME_FACTS, NBW_D_EARLY, NBW_D_ORDER, NBW_D_HANDLE};
    for (int it = 0; it < 3000; it++) {
        char kinds[200]; int n = 3 + (int)rn(120), np = 0;
        for (int i = 0; i < n; i++) { kinds[i] = rn(4) ? 'P' : 'o'; np += kinds[i] == 'P'; }
        kinds[n] = 0;
        if (np < 3) continue;
        make(kinds, (int)rn(3), rn(4) ? -1 : (int)rn((unsigned)n), -1);
        // an order: identity, or a planner-like order (a block moved back)
        nbo_ent e[NB_MAX_NODES]; int k = 0; memset(e, 0, sizeof e[0] * (size_t)np);
        for (int i = 0; i < n; i++) if (kinds[i] == 'P') e[k++].node = (uint16_t)i;
        const int moved = rn(2);
        if (moved) { const int a = (int)rn((unsigned)np - 1), b = a + 1 + (int)rn((unsigned)(np - a - 1)); const nbo_ent t = e[b]; memmove(e + a + 1, e + a, sizeof e[0] * (size_t)(b - a)); e[a] = t; }
        CHECK(nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 40 + (uint32_t)it, (uint32_t)rn(3), (int)rn(2), 0, rn(3) ? e : NULL, np) == np);
        CHECK(nbw_valid(&g_j) == NBW_V_OK);
        uint16_t stream[NBW_MAX_SEG];
        for (int i = 0; i < np; i++) stream[i] = (uint16_t)i;
        g_cmp_runs++;
        CHECK(run_model(&g_j, stream, MUT_NONE, 0) == 0);      // executed as recorded
        CHECK(run_model(&g_j, g_j.order, MUT_NONE, 0) == 0);   // executed in the job's order
        for (int mut = 1; mut < MUT_N; mut++) {
            const int at = (int)rn((unsigned)np);
            const uint16_t *ord = rn(2) ? stream : g_j.order;
            if (mut == MUT_SHUFFLE) {   // a swap of neighbours is "the other order" when the job's order is exactly that swap: take one that is neither
                uint16_t o[NBW_MAX_SEG]; memcpy(o, ord, sizeof o[0] * (size_t)np);
                const uint16_t t = o[at]; o[at] = o[(at + 1) % np]; o[(at + 1) % np] = t;
                if (!memcmp(o, stream, sizeof o[0] * (size_t)np) || !memcmp(o, g_j.order, sizeof o[0] * (size_t)np)) { g_cmp_caught[mut]++; continue; }
            }
            const uint32_t d = run_model(&g_j, ord, mut, at);
            CHECK((d & want[mut]) == want[mut]);
            if ((d & want[mut]) == want[mut]) g_cmp_caught[mut]++;
            if (mut == MUT_HANDLE || mut == MUT_FRAME_ADDR || mut == MUT_FRAME_FACTS || mut == MUT_FRAME_TWICE || mut == MUT_ADDR || mut == MUT_OTHER_FRAME || mut == MUT_OTHER_PASS)
                CHECK(d == want[mut]);   // nothing else is reported for these
        }
    }
    for (int mut = 1; mut < MUT_N; mut++) CHECK(g_cmp_caught[mut] == g_cmp_runs);
}

// ---------------- hand-off ----------------
// The queue model: hle_nvn.c's structure (x_push, x_run / x_run_own with the close under the lock, x_own_submit, x_own_present,
// nvn_nb_own_end) and the same nbw_hand_* calls. The translation thread is driven by the script: one call = one step of one job
// (an own job: start -> its submission translated and the job closed -> present / frame number), so a present can arrive before the
// job starts, while its submission is being translated, or after it was closed.
enum { ST_SUBMIT, ST_PRESENT, ST_FRAME };
typedef struct { uint8_t kind; uint64_t sub, id; } step;   // id: which submit / present / frame of the script
typedef struct { int kind; uint64_t sub, id, pid, fid; int parts; } mjob;   // kind as XJ_*: 0 submit 1 present 2 frame 3 own
enum { MQ = 4, MAXSTEPS = 4096 };
enum { HM_NONE, HM_NO_NEWEST, HM_NO_DONE, HM_NO_CLOSE, HM_NO_REREAD, HM_DOUBLE, HM_ONE_SLOT, HM_N };
typedef struct {
    int mode, mut;                 // DM_NB_OWN 0 / 1 / 2
    nbw_hand h; int armed;
    uint64_t subs, jobs_in, jobs_done, sub_done;
    mjob ring[MQ]; mjob run; int started;   // ring: g_xq; run: the translation thread's copy of the job it is in
    mjob one; int one_set;                  // HM_ONE_SLOT: what is attached kept in one place instead of in the queued job
    step out[MAXSTEPS]; int nout;
    uint64_t job_of_present[MAXSTEPS], ticket[MAXSTEPS];
} mq;
static void mq_parts(mq *m, const mjob *j, uint64_t job) {
    if (j->kind == 1 || (j->kind == 3 && (j->parts & NBW_P_PRESENT))) { const uint64_t p = j->kind == 3 ? j->pid : j->id; m->out[m->nout++] = (step){ST_PRESENT, 0, p}; if (p < MAXSTEPS) m->job_of_present[p] = job; }
    if (j->kind == 2 || (j->kind == 3 && (j->parts & NBW_P_FRAME))) m->out[m->nout++] = (step){ST_FRAME, 0, j->kind == 3 ? j->fid : j->id};
}
// One step of the translation thread. Returns 0 when the queue is empty.
static int mq_step(mq *m) {
    if (m->jobs_done == m->jobs_in || m->nout >= MAXSTEPS - 8) return 0;
    const uint64_t job = m->jobs_done + 1; mjob *q = &m->ring[job % MQ];
    if (!m->started) {
        m->run = *q;
        if (m->run.kind == 3) { m->started = 1; return 1; }                 // x_run_own: the submission is being translated
        if (m->run.kind == 0) { m->out[m->nout++] = (step){ST_SUBMIT, m->run.sub, m->run.id}; m->sub_done = m->run.sub; }
        mq_parts(m, &m->run, job); m->jobs_done = job; return 1;
    }
    m->out[m->nout++] = (step){ST_SUBMIT, m->run.sub, m->run.id};          // translated; now closed under the lock, the queued job read again
    if (m->mut != HM_NO_CLOSE) q->parts |= NBW_P_CLOSED;
    if (m->mut != HM_NO_REREAD) m->run = *q;
    if (m->mut == HM_ONE_SLOT && m->one_set) { m->run.pid = m->one.pid; m->run.fid = m->one.fid; m->run.parts |= NBW_P_PRESENT | NBW_P_FRAME; m->one_set = 0; }
    m->sub_done = m->run.sub;
    mq_parts(m, &m->run, job); m->jobs_done = job; m->started = 0;
    return 1;
}
static uint64_t mq_push(mq *m, mjob *j) {
    while (m->jobs_in - m->jobs_done >= MQ) mq_step(m);   // pthread_cond_wait until the translation thread made room
    if (j->kind == 0 || j->kind == 3) j->sub = ++m->subs;
    const uint64_t job = ++m->jobs_in;
    if (j->kind == 3) nbw_hand_opened(&m->h, job, j->sub, 0);
    m->ring[job % MQ] = *j;
    return job;
}
static void mq_submit(mq *m, uint64_t id, int has_job, int armed_thread) {   // nvnQueueSubmitCommands
    mjob j = {0, 0, id, 0, 0, 0};
    if (!m->mode) { mq_push(m, &j); return; }
    const int virt = m->mode != 1;
    const int open = nbw_hand_submit(&m->h, armed_thread && m->armed, has_job, 0);
    if (m->mut == HM_ONE_SLOT) m->one_set = 0;   // the bug this mutation models: one flag for every frame, reset by the next submit
    if (open && !virt) { j.kind = 3; j.parts = NBW_P_SUBMIT; }
    const uint64_t job = mq_push(m, &j);
    if (open && virt) nbw_hand_opened(&m->h, job, j.sub, 1);
}
static void mq_present(mq *m, uint64_t pid, uint64_t fid, int armed_thread) {   // nvnQueuePresentTexture
    uint64_t own = 0;
    if (m->mode) {
        mjob *q = &m->ring[m->h.job % MQ];
        int done = m->h.virt ? m->sub_done >= m->h.sub : (q->parts & NBW_P_CLOSED) != 0;
        if (m->mut == HM_NO_DONE) done = 0;
        if (nbw_hand_present(&m->h, armed_thread && m->armed, m->mut == HM_NO_NEWEST ? m->h.job : m->jobs_in, done)) {
            if (m->mut == HM_ONE_SLOT) { m->one.pid = pid; m->one.fid = fid; m->one_set = 1; }
            else { q->pid = pid; q->fid = fid; q->parts |= NBW_P_PRESENT | NBW_P_FRAME; }
            own = m->h.job;
        }
    }
    if (pid < MAXSTEPS) m->ticket[pid] = own;
    if (!own || m->mut == HM_DOUBLE) {
        mjob p = {1, 0, pid, 0, 0, 0}, f = {2, 0, fid, 0, 0, 0};
        const uint64_t t = mq_push(m, &p); if (!own && pid < MAXSTEPS) m->ticket[pid] = t;
        mq_push(m, &f);
    }
}
static uint64_t g_sid, g_pid, g_fid;
static void run_script(mq *m, const uint8_t *scr, int len, int mode, int mut) {
    memset(m, 0, sizeof *m); m->mode = mode; m->mut = mut;
    g_sid = g_pid = g_fid = 0;
    for (int i = 0; i + 1 < len && m->nout < MAXSTEPS - 64; i += 2) {
        const uint8_t c = scr[i], t = scr[i + 1];
        for (int k = 0; k < (t & 3); k++) mq_step(m);                                     // the translation thread catches up (or not)
        if (c & 1) mq_submit(m, ++g_sid, 1, 1);                                           // a mid-frame part (ReadTexels): not armed
        const int present_bit = (c & 12) != 0, has_job = (c & 16) == 0, inter = (c >> 5) & 7;
        m->armed = present_bit;
        mq_submit(m, ++g_sid, has_job, 1);                                                // EndFrame's submit
        for (int k = 0; k < ((t >> 2) & 3); k++) mq_step(m);                              // ... it may start, even finish, before the present
        if (inter == 1) { mjob o = {0, 0, ++g_sid, 0, 0, 0}; mq_push(m, &o); }            // another thread submits
        if (inter == 2) { mjob o = {2, 0, 5000 + (uint64_t)i, 0, 0, 0}; mq_push(m, &o); } // another job is queued
        if (inter == 3) { const uint64_t p = ++g_pid; mq_present(m, p, 6000 + (uint64_t)i, 0); }   // a present of another thread
        if (inter == 4) while (mq_step(m)) {}                                             // somebody waits for the queue (x_drain)
        if (present_bit && (c & 12) != 12) { const uint64_t p = ++g_pid, f = ++g_fid; mq_present(m, p, f, 1); }
        m->armed = 0; if (m->mode) nbw_hand_end(&m->h);                                   // nvn_nb_own_end
        for (int k = 0; k < ((t >> 4) & 3); k++) mq_step(m);
    }
    while (mq_step(m)) {}
}
static int same_steps(const mq *a, const mq *b) {
    if (a->nout != b->nout) return 0;
    for (int i = 0; i < a->nout; i++) if (a->out[i].kind != b->out[i].kind || a->out[i].sub != b->out[i].sub || a->out[i].id != b->out[i].id) return 0;
    return 1;
}
static int tickets_ok(const mq *m) {
    for (uint64_t p = 1; p <= g_pid && p < MAXSTEPS; p++) if (m->ticket[p] != m->job_of_present[p]) return 0;
    return 1;
}
static void test_hand(void) {
    static mq ref, one, chk, bad;
    unsigned scripts = 0, caught[HM_N] = {0};
    uint64_t st1[NBW_H_N] = {0}, st2[NBW_H_N] = {0}, jobs_ref = 0, jobs_one = 0;
    for (int it = 0; it < 4000; it++) {
        uint8_t scr[192]; const int len = 2 + 2 * (int)rn(95);
        for (int i = 0; i < len; i += 2) { scr[i] = (uint8_t)(rn(3) ? (rnd() & 0xe8) | 8 : rnd()); scr[i + 1] = (uint8_t)(rn(3) ? rnd() & 0x33 : rnd()); }
        run_script(&ref, scr, len, 0, HM_NONE);
        run_script(&one, scr, len, 1, HM_NONE);
        scripts++; jobs_ref += ref.jobs_in; jobs_one += one.jobs_in;
        CHECK(same_steps(&ref, &one));                 // the translation thread does what it did, in the same order, with the same submit numbers
        CHECK(one.subs == ref.subs && one.jobs_done == one.jobs_in && !one.h.open);
        CHECK(tickets_ok(&one) && tickets_ok(&ref));   // the ticket of a present names the job that runs it
        const uint64_t *s = one.h.st;
        CHECK(one.jobs_in + 2 * s[NBW_H_ONE] == ref.jobs_in);   // exactly two jobs saved a frame whose present was attached
        CHECK(s[NBW_H_OPEN] == s[NBW_H_ONE] + s[NBW_H_LATE] + s[NBW_H_BEHIND] + s[NBW_H_NOPRESENT] && s[NBW_H_OPEN] == s[NBW_H_FINAL]);
        run_script(&chk, scr, len, 2, HM_NONE);        // check mode: the old queue; the counters of the rule
        CHECK(same_steps(&ref, &chk) && chk.jobs_in == ref.jobs_in && tickets_ok(&chk));
        const uint64_t *v = chk.h.st;
        CHECK(v[NBW_H_OPEN] == v[NBW_H_ONE] + v[NBW_H_LATE] + v[NBW_H_BEHIND] + v[NBW_H_NOPRESENT]);
        // what does not depend on how far the translation thread got is the same number at 1 and at 2
        CHECK(v[NBW_H_FINAL] == s[NBW_H_FINAL] && v[NBW_H_OPEN] == s[NBW_H_OPEN] && v[NBW_H_BEHIND] == s[NBW_H_BEHIND] && v[NBW_H_NOPRESENT] == s[NBW_H_NOPRESENT] &&
              v[NBW_H_UNARMED] == s[NBW_H_UNARMED] && v[NBW_H_NOJOB] == s[NBW_H_NOJOB] && v[NBW_H_PRESENT_OLD] == s[NBW_H_PRESENT_OLD]);
        for (int k = 0; k < NBW_H_N; k++) { st1[k] += s[k]; st2[k] += v[k]; }
        for (int mut = 1; mut < HM_N; mut++) {
            run_script(&bad, scr, len, 1, mut);
            if (!same_steps(&ref, &bad) || !tickets_ok(&bad) || bad.subs != ref.subs) caught[mut]++;
        }
    }
    printf("hand-off: %u scripts; =1: %llu final submits, one job %llu, present after the submission was translated %llu, behind another job %llu, no present %llu; "
           "queue jobs %llu -> %llu; =2 would say: one job %llu, late %llu; mutations caught: no newest check %u, no closed check %u, job never closed %u, "
           "queued job not read again %u, present queued twice %u, attached in one place %u\n",
           scripts, (unsigned long long)st1[NBW_H_FINAL], (unsigned long long)st1[NBW_H_ONE], (unsigned long long)st1[NBW_H_LATE], (unsigned long long)st1[NBW_H_BEHIND],
           (unsigned long long)st1[NBW_H_NOPRESENT], (unsigned long long)jobs_ref, (unsigned long long)jobs_one, (unsigned long long)st2[NBW_H_ONE], (unsigned long long)st2[NBW_H_LATE],
           caught[HM_NO_NEWEST], caught[HM_NO_DONE], caught[HM_NO_CLOSE], caught[HM_NO_REREAD], caught[HM_DOUBLE], caught[HM_ONE_SLOT]);
    CHECK(st1[NBW_H_ONE] > 20000 && st1[NBW_H_LATE] > 5000 && st1[NBW_H_BEHIND] > 5000 && st1[NBW_H_NOPRESENT] > 1000);
    for (int mut = 1; mut < HM_N; mut++) CHECK(caught[mut] > scripts / 2);
    // the rule itself, case by case
    nbw_hand h; memset(&h, 0, sizeof h);
    CHECK(nbw_hand_submit(&h, 0, 1, 0) == 0 && h.st[NBW_H_UNARMED] == 1 && !h.open);             // a mid-frame part
    CHECK(nbw_hand_submit(&h, 0, 0, 0) == 0 && h.st[NBW_H_UNARMED] == 1);                         // any other submit: not counted
    CHECK(nbw_hand_submit(&h, 1, 0, 0) == 0 && h.st[NBW_H_NOJOB] == 1 && !h.open);
    CHECK(nbw_hand_submit(&h, 1, 1, 1) == 0 && h.st[NBW_H_SYNC] == 1 && h.st[NBW_H_FINAL] == 1 && !h.open);
    CHECK(nbw_hand_present(&h, 1, 5, 0) == 0 && h.st[NBW_H_PRESENT_OLD] == 1);
    CHECK(nbw_hand_submit(&h, 1, 1, 0) == 1 && !h.open);
    nbw_hand_opened(&h, 7, 3, 0);
    CHECK(h.open && h.job == 7 && h.sub == 3 && h.st[NBW_H_OPEN] == 1);
    CHECK(nbw_hand_present(&h, 0, 7, 0) == 0 && h.open && h.st[NBW_H_PRESENT_OLD] == 2);         // another thread's present does not take it
    CHECK(nbw_hand_present(&h, 1, 7, 0) == 1 && !h.open && h.st[NBW_H_ONE] == 1);
    CHECK(nbw_hand_present(&h, 1, 7, 0) == 0 && h.st[NBW_H_PRESENT_OLD] == 3);                    // once
    nbw_hand_opened(&h, 8, 4, 0);
    CHECK(nbw_hand_present(&h, 1, 9, 0) == 0 && !h.open && h.st[NBW_H_BEHIND] == 1);              // a job behind it
    nbw_hand_opened(&h, 10, 5, 0);
    CHECK(nbw_hand_present(&h, 1, 10, 1) == 0 && !h.open && h.st[NBW_H_LATE] == 1);               // already translated
    nbw_hand_opened(&h, 11, 6, 0); nbw_hand_end(&h);
    CHECK(!h.open && h.st[NBW_H_NOPRESENT] == 1);
    nbw_hand_end(&h); CHECK(h.st[NBW_H_NOPRESENT] == 1);
    nbw_hand_opened(&h, 12, 7, 0);
    CHECK(nbw_hand_submit(&h, 1, 1, 0) == 1 && !h.open);                                          // the next submit: the old open job is forgotten
    nbw_hand_opened(&h, 13, 8, 1);
    CHECK(nbw_hand_present(&h, 1, 13, 0) == 0 && !h.open && h.st[NBW_H_ONE] == 2);                // check mode: counted, queued by the caller
}

// NR-15 (docs/dos2/nr/NR-15.md): the DOS2 defaults without DM_NB_RELOC and DM_NB_ORDER. No pass has a key (nbw_key is never called) and
// the tap passes no order tail (nvn_nb_own_build: nord = 0 while g_nbo_on is 0): the job is still valid, in the identity order, without
// NBW_F_KEYS / NBW_F_ORDER, and a submission executed as recorded compares equal - the frame job and its hand-off need neither switch.
static void test_nr15_no_keys_no_order(void) {
    const char *kinds = "PPoPPPooPPPPoP";   // passes and operations as in a DOS2 part
    const int np = make(kinds, 0, -1, -1);
    const int ns = nbw_build(&g_j, &g_f, g_src, FRAME_AT, REC_A, REC_A, 91, 0, 1, 0, NULL, 0);
    CHECK(ns == np && nbw_valid(&g_j) == NBW_V_OK);
    CHECK(g_j.flags == (NBW_F_FINAL | NBW_F_PLANNED));   // no KEYS, no ORDER, no MERGED
    for (int k = 0; k < ns; k++) CHECK(g_j.seg[k].key == 0 && g_j.order[k] == k);
    uint16_t stream[NBW_MAX_SEG];
    for (int i = 0; i < np; i++) stream[i] = (uint16_t)i;
    CHECK(run_model(&g_j, stream, MUT_NONE, 0) == 0);
    CHECK(run_model(&g_j, stream, MUT_SKIP, 2) & NBW_D_PASS_MISSING);   // the compare still works without keys
    CHECK(run_model(&g_j, stream, MUT_ADDR, 3) == NBW_D_PASS_ADDR);
}
int main(void) {
    test_job();
    test_nr15_no_keys_no_order();
    test_compare();
    test_hand();
    printf("compare: %u jobs x %d mutations of the executed stream, each caught\n", g_cmp_runs, MUT_N - 1);
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("nb_own_test: all passed\n");
    return 0;
}
