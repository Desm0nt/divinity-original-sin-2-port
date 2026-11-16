// [DOS2] NB-17a / NB-17b host test: the order tail, the segment schedule, the scan and the run loop of dm/gfx/nb/nb_order.h - the code
// the NVN tap (nvn_nb_order.inc) and the backend (xbg_nb_order.inc) run.
// - tail: reserve / fill / parse round trip, every malformed case refused, "not valid" keeps its reason;
// - nbo_build: synthetic frames (identity, an empty pass moved, a barrier op left in place, an op split from its pass -> identity + glue,
//   prefix ops, lost record, more passes than reserved) and every golden IR dump given on the command line x the merge planner's modes
//   (off / as dumped / hazards assumed tracked / + HOIST / aa only): the segment order expands to exactly the planner's node order
//   (barrier ops apart) or is the identity with the glue flag; CONT only right behind the pass it continues;
// - stream model: random parts laid out as hle_nvn.c lays records out (chunks chained by OP_JUMP, also between a PASS and its KEY), a
//   backend model with the same structure as execute() (stop address, OP_END of the level, key start, keep / put). Identity: the event
//   sequence, the final state and the resume address equal the linear run, no key used. Random orders (swaps, rotations, shuffles; keys
//   complete or not; segments state-only or not; planner tracked or not): the verdict (unkeyed / unsafe) equals an independent brute force;
//   a run order gives every draw the state it has in the stream, every KEY met equals the live state, the final state is the stream's,
//   and without tracking the whole event sequence is the linear one;
// - mutations: a start() that does not set the state, a put() that does not put back, both caught; tails with a wrong address, a wrong
//   node, an address inside another record, a PASS of the part missing from the tail, a handle that ends early: scan / run report it.
// Build + run (from this directory):
//   clang -std=c11 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS nb_order_test.c ../../../nb/nb_plan.c -o nb_order_test && ./nb_order_test [dump.txt ...]
#include "../../../nb/nb_order.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
static uint64_t rnd_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 7; rnd_state ^= rnd_state << 17; return rnd_state; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---------------- tail ----------------
static void test_tail(void) {
    uint8_t rec[4096]; nbo_ent e[8], out[NBO_MAX_SEG]; nbo_hdr h;
    memset(rec, 0xee, sizeof rec);
    const size_t bytes = nbo_frame_bytes(8);
    CHECK(bytes == 32 + 24 + 8 * 16 && sizeof(nbo_hdr) == 24 && sizeof(nbo_ent) == 16);
    CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == NBO_P_NONE);     // no magic
    CHECK(nbo_parse(rec, 28, 7, &h, out, NBO_MAX_SEG) == NBO_P_NONE);        // a FRAME record without a tail
    nbo_init(rec, bytes, 7, 1);
    CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == NBO_P_INVALID && h.why == NBO_W_UNPATCHED && h.cap == 8);
    CHECK(nbo_parse(rec, bytes, 8, &h, out, NBO_MAX_SEG) == NBO_P_BAD);      // another part's seq
    nbo_fail(rec, NBO_W_SPLIT);
    CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == NBO_P_INVALID && h.why == NBO_W_SPLIT);
    for (int i = 0; i < 5; i++) { e[i].addr = 0x1000 + 64u * (unsigned)i; e[i].node = (uint16_t)(2 * i + 1); e[i].flags = 0; e[i].pad = 0; }
    nbo_fill(rec, e, 5, 12, 0);
    CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == 5 && h.nnode == 12 && h.ncont == 0 && (h.flags & NBO_F_VALID) && !(h.flags & NBO_F_MERGED));
    CHECK(!memcmp(out, e, 5 * sizeof e[0]));
    CHECK(nbo_parse(rec, nbo_frame_bytes(4), 7, &h, out, NBO_MAX_SEG) == NBO_P_BAD);   // record shorter than nseg
    CHECK(nbo_parse(rec, bytes, 7, &h, out, 4) == NBO_P_BAD);                          // caller's table too small
    nbo_ent s = e[1]; e[1] = e[3]; e[3] = s;                                           // not the stream order
    nbo_fill(rec, e, 5, 12, 0);
    CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == NBO_P_BAD);                // ... without NBO_F_MERGED
    e[2].flags = NBO_E_CONT;
    nbo_fill(rec, e, 5, 12, NBO_F_MERGED | NBO_F_TRACKED);
    CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == 5 && h.ncont == 1 && (h.flags & NBO_F_MERGED) && (h.flags & NBO_F_TRACKED));
    e[2].flags = 0;
    nbo_ent keep = e[4];
    e[4].node = e[0].node; nbo_fill(rec, e, 5, 12, NBO_F_MERGED); CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == NBO_P_BAD);   // a node twice
    e[4] = keep; e[4].node = 12; nbo_fill(rec, e, 5, 12, NBO_F_MERGED); CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == NBO_P_BAD);   // outside the frame
    e[4] = keep; e[4].addr = 0; nbo_fill(rec, e, 5, 12, NBO_F_MERGED); CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == NBO_P_BAD);
    e[4] = keep; e[4].addr = 0x1004; nbo_fill(rec, e, 5, 12, NBO_F_MERGED); CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == NBO_P_BAD);
    e[4] = keep; e[0].flags = NBO_E_CONT; nbo_fill(rec, e, 5, 12, NBO_F_MERGED); CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == NBO_P_BAD);
    e[0].flags = 0; nbo_fill(rec, e, 5, 12, NBO_F_MERGED); CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == 5);
    nbo_fill(rec, e, 9, 12, 0);                                                        // more than reserved: nothing written
    CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == 5);
    rec[NBO_TAIL_OFF + 4] = 2;                                                         // version
    CHECK(nbo_parse(rec, bytes, 7, &h, out, NBO_MAX_SEG) == NBO_P_BAD);
    uint8_t small[40]; memset(small, 0, sizeof small); nbo_init(small, sizeof small, 1, 1);   // too small for a header: untouched
    CHECK(nbo_parse(small, sizeof small, 1, &h, out, NBO_MAX_SEG) == NBO_P_NONE);
    // schedule of a merged order: E1 E2 moved before A1 (stream A1 E1 E2 A2)
    nbo_ent m[4] = {{0x2040, 3, 0, 0}, {0x2080, 5, 0, 0}, {0x2000, 1, 0, 0}, {0x20c0, 7, NBO_E_CONT, 0}};
    static nbo_sched sc; nbo_schedule(m, 4, &sc);
    CHECK(!sc.identity && sc.n == 4 && sc.first == (const uint8_t *)(uintptr_t)0x2000 && sc.ndiscont == 3);
    CHECK(sc.st[0].discont && !sc.st[1].discont && sc.st[2].discont && sc.st[3].discont);
    CHECK(sc.st[2].stop == (const uint8_t *)(uintptr_t)0x2040 && sc.st[1].stop == (const uint8_t *)(uintptr_t)0x20c0 && !sc.st[3].stop && sc.st[3].last && !sc.st[2].last);
    nbo_schedule(e, 0, &sc); CHECK(sc.identity && !sc.first && !sc.ndiscont);
}

// ---------------- nbo_build ----------------
static nb_frame F; static nb_plan_out PO; static nb_merge_out MO; static nb_persist PS;
static nbo_ent g_ent[NBO_MAX_SEG];
// The segment order expanded back to nodes equals m's order without barrier ops (or, with glue, the entries are the identity).
static int expand_ok(const nb_frame *f, const nb_merge_out *m, const nbo_ent *ent, int ns, int merged, int glue) {
    static int16_t a[NB_MAX_NODES], b[NB_MAX_NODES]; int na = 0, nb = 0, npass = 0, bad = 0;
    for (int i = 0; i < f->nnode; i++) npass += f->node[i].kind == NB_NODE_PASS;
    if (ns != npass) return 0;
    for (int i = 1; i < ns; i++) if ((ent[i].node < ent[i - 1].node) && !merged) bad++;
    if (glue || !m) { for (int i = 1; i < ns; i++) if (ent[i].node < ent[i - 1].node) bad++; for (int i = 0; i < ns; i++) if (ent[i].flags) bad++; return !bad && !merged; }
    for (int i = 0; i < f->nnode && f->node[i].kind != NB_NODE_PASS; i++) if (!nbo_is_bar(&f->node[i])) a[na++] = (int16_t)i;   // prefix ops
    for (int k = 0; k < ns; k++) {
        a[na++] = (int16_t)ent[k].node;
        for (int i = ent[k].node + 1; i < f->nnode && f->node[i].kind != NB_NODE_PASS; i++) if (!nbo_is_bar(&f->node[i])) a[na++] = (int16_t)i;
        if (ent[k].flags & NBO_E_CONT) { if (!k || m->cont[ent[k].node] != (int)ent[k - 1].node) bad++; }
        else if (merged && k && m->cont[ent[k].node] == (int)ent[k - 1].node) bad++;
        if (merged && ((ent[k].flags & NBO_E_PREBAR) != 0) != (m->prebar[ent[k].node] != 0)) bad++;
    }
    for (int j = 0; j < m->nnode; j++) { const int x = m->order[j]; if (f->node[x].kind == NB_NODE_PASS || !nbo_is_bar(&f->node[x])) b[nb++] = (int16_t)x; }
    if (na != nb || memcmp(a, b, (size_t)na * sizeof a[0])) bad++;
    return !bad;
}
static void ident_merge(const nb_frame *f, nb_merge_out *m) {
    memset(m, 0, sizeof *m); m->nnode = f->nnode;
    for (int i = 0; i < f->nnode; i++) { m->order[i] = (int16_t)i; m->pos[i] = (int16_t)i; m->cont[i] = -1; m->head[i] = (int16_t)i; }
}
static void test_build(void) {
    uint8_t have[NB_MAX_NODES]; int merged, glue;
    memset(have, 1, sizeof have);
    // nodes: 0 op copy (prefix), 1 pass A1, 2 op barrier, 3 pass E1 (empty), 4 pass E2, 5 pass A2, 6 op copy, 7 pass B
    const uint64_t img[3] = {0x100, 0x200, 0x300};
    nb_frame_reset(&F, 1);
    nb_op(&F, NB_OP_COPY, &img[2], 1, &img[1], 1);
    nb_att a = {img[0], 0}, e1 = {img[1], 0}, e2 = {img[2], 0};
    nb_pass_begin(&F, &a, 1); nb_draw(&F, 0);
    nb_op(&F, NB_OP_BARRIER, NULL, 0, NULL, 0);
    nb_pass_begin(&F, &e1, 1); nb_pass_begin(&F, &e2, 1);
    nb_pass_begin(&F, &a, 1); nb_draw(&F, 0);
    nb_op(&F, NB_OP_COPY, &img[0], 1, &img[1], 1);
    nb_pass_begin(&F, &e1, 1); nb_draw(&F, 0);
    nb_submit(&F, 1);
    CHECK(F.nnode == 8 && F.node[0].kind != NB_NODE_PASS && F.node[1].kind == NB_NODE_PASS && nbo_is_bar(&F.node[2]) && F.node[5].kind == NB_NODE_PASS && F.node[7].kind == NB_NODE_PASS);
    int ns = nbo_build(&F, have, NULL, g_ent, NBO_MAX_SEG, &merged, &glue);
    CHECK(ns == 5 && !merged && !glue && g_ent[0].node == 1 && g_ent[1].node == 3 && g_ent[2].node == 4 && g_ent[3].node == 5 && g_ent[4].node == 7);
    ident_merge(&F, &MO);
    ns = nbo_build(&F, have, &MO, g_ent, NBO_MAX_SEG, &merged, &glue);
    CHECK(ns == 5 && !merged && !glue && expand_ok(&F, &MO, g_ent, ns, merged, glue));
    // the planner's aa order: empty passes before the unit, the barrier op wherever: 0 3 4 2 1 5 6 7, A2 continues A1
    static const int16_t o1[8] = {0, 3, 4, 2, 1, 5, 6, 7};
    memcpy(MO.order, o1, sizeof o1); MO.cont[5] = 1; MO.prebar[1] = 1;
    ns = nbo_build(&F, have, &MO, g_ent, NBO_MAX_SEG, &merged, &glue);
    CHECK(ns == 5 && merged && !glue && g_ent[0].node == 3 && g_ent[1].node == 4 && g_ent[2].node == 1 && g_ent[3].node == 5 && g_ent[4].node == 7);
    CHECK(g_ent[2].flags == NBO_E_PREBAR && g_ent[3].flags == NBO_E_CONT && !g_ent[0].flags && expand_ok(&F, &MO, g_ent, ns, merged, glue));
    // an op taken away from its pass (6 before the unit): not expressible by segments -> identity + glue
    static const int16_t o2[8] = {0, 3, 4, 6, 1, 2, 5, 7};
    memcpy(MO.order, o2, sizeof o2);
    ns = nbo_build(&F, have, &MO, g_ent, NBO_MAX_SEG, &merged, &glue);
    CHECK(ns == 5 && !merged && glue && g_ent[0].node == 1 && g_ent[4].node == 7 && !g_ent[3].flags && expand_ok(&F, &MO, g_ent, ns, merged, glue));
    // a prefix op not first
    static const int16_t o3[8] = {3, 0, 4, 2, 1, 5, 6, 7};
    memcpy(MO.order, o3, sizeof o3);
    ns = nbo_build(&F, have, &MO, g_ent, NBO_MAX_SEG, &merged, &glue);
    CHECK(ns == 5 && !merged && glue);
    // a pass whose op trails later (5 then 7 then 6)
    static const int16_t o4[8] = {0, 1, 2, 3, 4, 5, 7, 6};
    memcpy(MO.order, o4, sizeof o4);
    ns = nbo_build(&F, have, &MO, g_ent, NBO_MAX_SEG, &merged, &glue);
    CHECK(ns == 5 && !merged && glue);
    // a segment with its op moved as one: 0 5 6 1 2 3 4 7
    static const int16_t o5[8] = {0, 5, 6, 1, 2, 3, 4, 7};
    memcpy(MO.order, o5, sizeof o5); MO.cont[5] = -1; MO.prebar[1] = 0;
    ns = nbo_build(&F, have, &MO, g_ent, NBO_MAX_SEG, &merged, &glue);
    CHECK(ns == 5 && merged && !glue && g_ent[0].node == 5 && g_ent[1].node == 1 && expand_ok(&F, &MO, g_ent, ns, merged, glue));
    // a node twice in the order / out of range
    static const int16_t o6[8] = {0, 1, 2, 3, 3, 5, 6, 7};
    memcpy(MO.order, o6, sizeof o6);
    ns = nbo_build(&F, have, &MO, g_ent, NBO_MAX_SEG, &merged, &glue); CHECK(ns == 5 && !merged && glue);
    static const int16_t o7[8] = {0, 1, 2, 3, 4, 5, 6, 99};
    memcpy(MO.order, o7, sizeof o7);
    ns = nbo_build(&F, have, &MO, g_ent, NBO_MAX_SEG, &merged, &glue); CHECK(ns == 5 && !merged && glue);
    CHECK(nbo_build(&F, have, NULL, g_ent, 4, &merged, &glue) == -NBO_W_OVER);
    have[4] = 0; CHECK(nbo_build(&F, have, NULL, g_ent, NBO_MAX_SEG, &merged, &glue) == -NBO_W_LOST); have[4] = 1;
    have[6] = 0; CHECK(nbo_build(&F, have, NULL, g_ent, NBO_MAX_SEG, &merged, &glue) == 5);   // an op has no record of its own
}
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    n = (long)fread(b, 1, (size_t)n, f); b[n] = 0; fclose(f); return b;
}
static void golden(const char *path) {
    char *t = slurp(path);
    if (!t) { printf("FAIL: cannot read %s\n", path); fails++; return; }
    const int line = nb_parse(t, &F); free(t);
    if (line) { printf("FAIL: %s: parse error at line %d\n", path, line); fails++; return; }
    nb_persist_reset(&PS);
    const nb_rules rules = {NB_RULES_EXACT, 0, 0};
    if (nb_plan(&F, &PS, &rules, &PO)) { printf("FAIL: %s: plan\n", path); fails++; return; }
    uint8_t have[NB_MAX_NODES]; memset(have, 1, sizeof have);
    static const struct { const char *name; int mode; unsigned flags; } modes[] = {
        {"off", NB_MERGE_OFF, 0}, {"as dumped", NB_MERGE_ON, 0}, {"assumed tracked", NB_MERGE_ON, NB_MERGEF_ASSUME_TRACKED},
        {"assumed + hoist", NB_MERGE_ON, NB_MERGEF_ASSUME_TRACKED | NB_MERGEF_HOIST}, {"aa only, assumed", NB_MERGE_ON, NB_MERGEF_AA | NB_MERGEF_ASSUME_TRACKED}};
    printf("# golden %s: %d nodes;", path, F.nnode);
    for (unsigned k = 0; k < sizeof modes / sizeof modes[0]; k++) {
        const nb_merge_rules mr = {modes[k].mode, modes[k].flags};
        if (nb_merge(&F, &PO, &mr, &MO)) { printf(" FAIL merge (%s)", modes[k].name); fails++; continue; }
        int merged = 0, glue = 0;
        const int ns = nbo_build(&F, have, &MO, g_ent, NBO_MAX_SEG, &merged, &glue);
        CHECK(ns >= 0 && expand_ok(&F, &MO, g_ent, ns, merged, glue));
        if (modes[k].mode == NB_MERGE_OFF || k == 1) CHECK(!merged && !glue);   // identity: off, and untracked as dumped
        if (ns < 0) continue;
        for (int i = 0; i < ns; i++) g_ent[i].addr = 0x10000 + 512ull * g_ent[i].node;
        static uint8_t rec[32 + 24 + 16 * NBO_MAX_SEG]; static nbo_ent back[NBO_MAX_SEG]; nbo_hdr h; static nbo_sched sc;
        nbo_init(rec, sizeof rec, 3, 3); nbo_fill(rec, g_ent, ns, F.nnode, merged ? NBO_F_MERGED : 0);
        CHECK(nbo_parse(rec, sizeof rec, 3, &h, back, NBO_MAX_SEG) == ns && !memcmp(back, g_ent, (size_t)ns * sizeof back[0]));
        nbo_schedule(back, ns, &sc);
        CHECK(sc.identity == !merged);
        int moved = 0; for (int i = 1; i < ns; i++) moved += g_ent[i].node < g_ent[i - 1].node;
        printf(" %s: %d segments%s%s, planner merged %d, moved back %d, cont %d, starts from a key %d;", modes[k].name, ns, merged ? " MERGED" : "", glue ? " (planner order not usable: identity)" : "",
               MO.st.merged, moved, h.ncont, sc.ndiscont);
    }
    printf("\n");
}

// ---------------- stream model ----------------
enum { T_END = 0, T_JUMP = 1, T_FRAME = 9, T_PASS = 10, T_PASS_END = 11, T_KEY = 12, T_SET = 20, T_DRAW = 21, T_SIDE = 22, T_REFUSE = 23 };
enum { NV = 12, CHUNK = 1024, NCHUNK = 512, MAXEV = 8192, MAXSEG = 48 };
static uint64_t g_mem[NCHUNK][CHUNK / 8];
static int g_chunk; static uint8_t *g_cur, *g_end;
static void new_chunk(void) { if (g_chunk >= NCHUNK) { printf("out of test chunks\n"); exit(2); } g_cur = (uint8_t *)g_mem[g_chunk++]; g_end = g_cur + CHUNK; }
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) { uint32_t h[2] = {op, size}; memcpy(p, h, 8); }
static uint8_t *rec(uint32_t op, uint32_t payload, int force_jump) {
    const uint32_t size = (8 + payload + 7) & ~7u;
    if (force_jump || g_cur + size + 24 > g_end) {   // chain a new chunk as cmd_alloc does
        uint8_t *j = g_cur; new_chunk();
        uint64_t t = (uint64_t)(uintptr_t)g_cur; put_hdr(j, T_JUMP, 16); memcpy(j + 8, &t, 8);
    }
    uint8_t *p = g_cur; g_cur += size; put_hdr(p, op, size); memset(p + 8, 0, size - 8); return p + 8;
}
typedef struct { uint32_t seq; int16_t node; uint8_t complete, pad; uint32_t var[NV]; } t_key;
typedef struct { uint32_t seq; int16_t node; uint16_t pad; } t_pass;
// model backend
typedef struct { uint32_t id, hash; } ev;
typedef struct {
    uint32_t var[NV], kept[NV]; ev e[MAXEV]; int ne;
    const uint8_t *stop; int starts, keeps, puts, key_bad, keys_met, mut_start, mut_put;
} be;
static uint32_t hash_state(const uint32_t *v) { uint32_t h = 2166136261u; for (int i = 0; i < NV; i++) { h ^= v[i]; h *= 16777619u; } return h; }
static size_t t_avail(void *c, const uint8_t *p) {
    (void)c; const uint8_t *lo = (const uint8_t *)g_mem, *hi = lo + (size_t)g_chunk * CHUNK;
    if (p < lo || p >= hi) return 0;
    return CHUNK - (size_t)(p - lo) % CHUNK;
}
// execute(): records from `from`; returns NULL at `stop`, else the OP_END met (not executed)
static const uint8_t *t_exec(void *c, int e, const uint8_t *from, const uint8_t *stop) {
    be *b = (be *)c; const uint8_t *pc = from; (void)e;
    for (;;) {
        if (pc == stop) return NULL;
        uint32_t h[2]; memcpy(h, pc, 8); const uint8_t *p = pc + 8;
        switch (h[0]) {
        case T_END: return pc;
        case T_JUMP: { uint64_t t; memcpy(&t, p, 8); pc = (const uint8_t *)(uintptr_t)t; continue; }
        case T_SET: { uint32_t a[2]; memcpy(a, p, 8); b->var[a[0] % NV] = a[1]; break; }
        case T_DRAW: case T_SIDE: { uint32_t id; memcpy(&id, p, 4); if (b->ne < MAXEV) { b->e[b->ne].id = id; b->e[b->ne].hash = h[0] == T_DRAW ? hash_state(b->var) : 0; b->ne++; } break; }
        case T_KEY: { t_key k; memcpy(&k, p, sizeof k); b->keys_met++; if (k.complete && memcmp(k.var, b->var, sizeof k.var)) b->key_bad++; break; }   // the DM_NB_RELOC=2 check
        default: break;
        }
        pc += h[1];
    }
}
static int t_start(void *c, int e, const nbo_step *s) {
    be *b = (be *)c; t_key k; (void)e;
    if (!s->key) return 0;
    memcpy(&k, s->key, sizeof k); if (!k.complete) return 0;
    b->starts++;
    if (!b->mut_start) memcpy(b->var, k.var, sizeof k.var);
    return 1;
}
static void t_keep(void *c) { be *b = (be *)c; b->keeps++; memcpy(b->kept, b->var, sizeof b->var); }
static void t_put(void *c) { be *b = (be *)c; b->puts++; if (!b->mut_put) memcpy(b->var, b->kept, sizeof b->var); }
static int t_rec(void *c, uint32_t op, const uint8_t *p, size_t n) {
    (void)c; (void)p; (void)n;
    if (op == T_REFUSE) return -1;
    return op == T_SET || op == T_PASS || op == T_PASS_END || op == T_KEY;
}
static int t_key_ok(void *c, const uint8_t *p, size_t n, uint32_t seq, int node) {
    (void)c; t_key k; if (n < sizeof k) return 0;
    memcpy(&k, p, sizeof k); return k.complete && k.seq == seq && k.node == node;
}
static const nbo_scan_io g_io = {NULL, T_END, T_JUMP, T_PASS, T_KEY, t_avail, t_rec, t_key_ok};

typedef struct {
    int nseg; uint32_t seq; const uint8_t *after, *end, *pass[MAXSEG]; uint16_t node[MAXSEG]; uint8_t pure[MAXSEG], keyed[MAXSEG];
    uint32_t init[NV];               // the state at the FRAME record
    uint32_t first_ev[MAXSEG + 1];   // linear event index where each segment's events begin
    be lin;                          // the linear run
} part;
static part P; static be B; static nbo_sched SC; static nbo_scan_out SO;
// One part: prefix, nseg segments (PASS [KEY] records ... ), OP_END. Keys hold the state the linear stream has at the PASS.
static void gen_part(int nseg, int all_keys, int pure_pct, int extra_pass) {
    uint32_t var[NV]; static uint32_t seqs = 100, ids = 1;
    g_chunk = 0; new_chunk();
    memset(&P, 0, sizeof P); P.nseg = nseg; P.seq = ++seqs;
    for (int i = 0; i < NV; i++) var[i] = (uint32_t)rnd();
    memcpy(P.lin.var, var, sizeof var); memcpy(P.init, var, sizeof var);   // the state at the FRAME record
    rec(T_FRAME, 28, 0);
    P.after = g_cur;                                       // the next record (a chunk's JUMP may sit here; the walk follows it)
    int node = (int)rn(3);
    const int npre = (int)rn(4);
    for (int i = 0; i < npre; i++) { const uint32_t op = rn(3) ? T_SET : T_DRAW; uint8_t *p = rec(op, 8, !rn(6)); uint32_t a[2] = {op == T_SET ? rn(NV) : ids++, (uint32_t)rnd()}; memcpy(p, a, 8); }
    for (int s = 0; s < nseg; s++) {
        const int pure = (int)rn(100) < pure_pct;
        uint8_t *pp = rec(T_PASS, sizeof(t_pass), !rn(5));
        t_pass tp = {P.seq, (int16_t)node, 0}; memcpy(pp, &tp, sizeof tp);
        P.pass[s] = pp - 8; P.node[s] = (uint16_t)node; P.pure[s] = (uint8_t)pure;
        const int has_key = all_keys || rn(4), complete = all_keys || rn(5) != 0;
        if (has_key) { uint8_t *kp = rec(T_KEY, sizeof(t_key), !rn(4)); t_key k; memset(&k, 0, sizeof k); k.seq = P.seq; k.node = (int16_t)node; k.complete = (uint8_t)complete; P.keyed[s] = (uint8_t)complete; memcpy(kp, &k, sizeof k); }
        const int nr = 1 + (int)rn(7); int impure = 0;
        for (int i = 0; i < nr; i++) {
            const unsigned w = rn(10);
            const uint32_t op = (pure || w < 5) ? T_SET : w < 9 ? T_DRAW : T_SIDE;
            uint8_t *p = rec(op, 8, !rn(8)); impure |= op != T_SET;
            uint32_t a[2] = {op == T_SET ? rn(NV) : ids++, (uint32_t)rnd()}; memcpy(p, a, 8);
            if (i == nr / 2) rec(T_PASS_END, 8, 0);
        }
        P.pure[s] = (uint8_t)!impure;
        if (extra_pass && s == nseg / 2) { uint8_t *x = rec(T_PASS, sizeof(t_pass), 0); t_pass t2 = {P.seq, 500, 0}; memcpy(x, &t2, sizeof t2); }   // a PASS the tail does not know
        node += 1 + (int)rn(3);
    }
    P.end = rec(T_END, 0, 0) - 8;
    // linear run: fills the keys' snapshots on the way (the tap writes the key from its own mirror of the stream)
    const uint8_t *pc = P.after; int s = -1; be *b = &P.lin; b->ne = 0;
    for (;;) {
        uint32_t h[2]; memcpy(h, pc, 8); uint8_t *p = (uint8_t *)pc + 8;
        if (h[0] == T_END) break;
        if (h[0] == T_JUMP) { uint64_t t; memcpy(&t, p, 8); pc = (const uint8_t *)(uintptr_t)t; continue; }
        if (h[0] == T_PASS) { t_pass tp; memcpy(&tp, p, sizeof tp); if (tp.node != 500) { s++; P.first_ev[s] = (uint32_t)b->ne; } }
        if (h[0] == T_KEY) { t_key k; memcpy(&k, p, sizeof k); memcpy(k.var, b->var, sizeof k.var); memcpy(p, &k, sizeof k); }
        if (h[0] == T_SET) { uint32_t a[2]; memcpy(a, p, 8); b->var[a[0] % NV] = a[1]; }
        if (h[0] == T_DRAW || h[0] == T_SIDE) { uint32_t id; memcpy(&id, p, 4); b->e[b->ne].id = id; b->e[b->ne].hash = h[0] == T_DRAW ? hash_state(b->var) : 0; b->ne++; }
        pc += h[1];
    }
    P.first_ev[nseg] = (uint32_t)b->ne;
}
static void be_reset(be *b, int mut_start, int mut_put) {
    memset(b, 0, sizeof *b); b->mut_start = mut_start; b->mut_put = mut_put;
}
static uint32_t g_init[NV];
static void run_order(const int *order, int n, nbo_ent *ent) {
    for (int k = 0; k < n; k++) { ent[k].addr = (uint64_t)(uintptr_t)P.pass[order[k]]; ent[k].node = P.node[order[k]]; ent[k].flags = 0; ent[k].pad = 0; }
}
static const nbo_ops g_ops = {&B, t_exec, t_start, t_keep, t_put};
static uint64_t st_parts, st_ident, st_run, st_refused_key, st_refused_unsafe, st_starts, st_puts, st_events, st_mut_start, st_mut_put, st_scan_bad;
// events of B against the linear run: same multiset with the same state per draw; exact = the same sequence
static int same_events(int exact) {
    if (B.ne != P.lin.ne) return 0;
    if (exact) return !memcmp(B.e, P.lin.e, (size_t)B.ne * sizeof(ev));
    for (int i = 0; i < B.ne; i++) {
        int j = 0; while (j < P.lin.ne && P.lin.e[j].id != B.e[i].id) j++;
        if (j == P.lin.ne || P.lin.e[j].hash != B.e[i].hash) return 0;
    }
    return 1;
}
static void test_stream(void) {
    static nbo_ent ent[MAXSEG]; int order[MAXSEG];
    for (int it = 0; it < 4000; it++) {
        const int nseg = 1 + (int)rn(MAXSEG - 1), all_keys = !rn(3), pure_pct = (int)rn(101);
        gen_part(nseg, all_keys, pure_pct, 0);
        st_parts++;
        memcpy(g_init, P.init, sizeof g_init);
        // --- identity ---
        for (int k = 0; k < nseg; k++) order[k] = k;
        run_order(order, nseg, ent); nbo_schedule(ent, nseg, &SC);
        CHECK(SC.identity && SC.ndiscont == 0);
        be_reset(&B, 0, 0); memcpy(B.var, g_init, sizeof g_init);
        int res = -1, done = -1;
        const uint8_t *resume = nbo_run(&SC, P.after, &g_ops, &res, &done);
        CHECK(res == NBO_R_OK && done == nseg && resume == P.end && !B.starts && !B.keeps && !B.puts && !B.key_bad);
        CHECK(same_events(1) && !memcmp(B.var, P.lin.var, sizeof B.var));
        st_ident++; st_events += (uint64_t)B.ne;
        // scan of the identity (check mode): finds every key and purity as generated
        CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) == NBO_S_OK);
        for (int k = 0; k < nseg; k++) CHECK(SO.pure[k] == P.pure[k] && SO.keyed[k] == P.keyed[k]);
        // --- other orders ---
        for (int v = 0; v < 6 && nseg > 1; v++) {
            for (int k = 0; k < nseg; k++) order[k] = k;
            if (v < 2) { const int i = (int)rn((unsigned)nseg - 1); const int t = order[i]; order[i] = order[i + 1]; order[i + 1] = t; }      // one swap
            else if (v < 4) { const int a = (int)rn((unsigned)nseg - 1), l = 2 + (int)rn((unsigned)(nseg - a - 1)); const int t = order[a]; memmove(order + a, order + a + 1, (size_t)(l - 1) * sizeof order[0]); order[a + l - 1] = t; }   // rotate a block
            else for (int k = nseg - 1; k > 0; k--) { const int j = (int)rn((unsigned)k + 1); const int t = order[k]; order[k] = order[j]; order[j] = t; }   // shuffle
            int ident = 1; for (int k = 0; k < nseg; k++) if (order[k] != k) ident = 0;
            if (ident) continue;
            const int tracked = (int)rn(2);
            run_order(order, nseg, ent); nbo_schedule(ent, nseg, &SC);
            CHECK(!SC.identity);
            CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) == NBO_S_OK);
            // brute force: discontinuous starts and their keys; inversions with two impure sides
            int bf_unkeyed = 0, bf_unsafe = 0, bf_disc = 0;
            for (int k = 0; k < nseg; k++) {
                const int cont = order[k] == 0 ? k == 0 : (k > 0 && order[k - 1] == order[k] - 1);
                if (!cont) { bf_disc++; if (!P.keyed[order[k]]) bf_unkeyed++; }
                CHECK(SO.pure[k] == P.pure[order[k]] && SO.keyed[k] == P.keyed[order[k]]);
                for (int j = k + 1; j < nseg; j++) if (order[k] > order[j] && !P.pure[order[k]] && !P.pure[order[j]]) bf_unsafe++;
            }
            CHECK(SC.ndiscont == bf_disc && nbo_unkeyed(&SC) == bf_unkeyed && nbo_unsafe(&SC, SO.pure, 0) == bf_unsafe && nbo_unsafe(&SC, SO.pure, 1) == 0);
            if (bf_unkeyed) { st_refused_key++; continue; }
            if (!tracked && bf_unsafe) { st_refused_unsafe++; continue; }
            be_reset(&B, 0, 0); memcpy(B.var, g_init, sizeof g_init);
            resume = nbo_run(&SC, P.after, &g_ops, &res, &done);
            CHECK(res == NBO_R_OK && done == nseg && resume == P.end && B.starts == bf_disc && !B.key_bad);
            CHECK(B.keeps == (order[nseg - 1] != nseg - 1) && B.puts == B.keeps);
            CHECK(same_events(!tracked || !bf_unsafe) && !memcmp(B.var, P.lin.var, sizeof B.var));
            // per segment the events keep their order
            for (int k = 0, at = (int)P.first_ev[0]; k < nseg; k++) { const int sg = order[k]; const uint32_t a = P.first_ev[sg], z = P.first_ev[sg + 1];
                for (uint32_t i = a; i < z; i++, at++) if (at >= B.ne || B.e[at].id != P.lin.e[i].id) { CHECK(!"segment events out of order"); break; } }
            st_run++; st_starts += (uint64_t)B.starts; st_puts += (uint64_t)B.puts;
            // mutation: start() does not set the state -> a draw sees another state, or a key check fails, or the final state differs
            be_reset(&B, 1, 0); memcpy(B.var, g_init, sizeof g_init);
            nbo_run(&SC, P.after, &g_ops, &res, &done);
            if (!same_events(0) || B.key_bad || memcmp(B.var, P.lin.var, sizeof B.var)) st_mut_start++;
            // mutation: put() does not put back
            if (order[nseg - 1] != nseg - 1) {
                be_reset(&B, 0, 1); memcpy(B.var, g_init, sizeof g_init);
                nbo_run(&SC, P.after, &g_ops, &res, &done);
                if (memcmp(B.var, P.lin.var, sizeof B.var)) st_mut_put++;
            }
        }
    }
    CHECK(st_run > 500 && st_refused_key > 100 && st_refused_unsafe > 100);
    CHECK(st_mut_start > st_run / 2);   // most orders that start from a key somewhere show the missing state
    CHECK(st_mut_put > 50);
}
static void test_broken(void) {
    static nbo_ent ent[MAXSEG]; int order[MAXSEG]; int res, done;
    for (int it = 0; it < 300; it++) {
        const int nseg = 4 + (int)rn(20);
        gen_part(nseg, 1, 50, 0); memcpy(g_init, P.init, sizeof g_init);
        for (int k = 0; k < nseg; k++) order[k] = k;
        const int a = (int)rn((unsigned)nseg - 1); { const int t = order[a]; order[a] = order[a + 1]; order[a + 1] = t; }
        run_order(order, nseg, ent);
        // an address that is not a record start / not a PASS record / of another node
        nbo_ent sv = ent[a];
        ent[a].addr += 8; nbo_schedule(ent, nseg, &SC); { const int w = nbo_scan(&SC, P.after, P.seq, &g_io, &SO); CHECK(w == NBO_S_ADDR || w == NBO_S_ORDER || w == NBO_S_MISSING); st_scan_bad++; }
        ent[a] = sv; ent[a].addr = (uint64_t)(uintptr_t)P.end; nbo_schedule(ent, nseg, &SC); CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) != NBO_S_OK);
        ent[a] = sv; ent[a].addr = 0x10; nbo_schedule(ent, nseg, &SC); CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) != NBO_S_OK);   // not command memory
        ent[a] = sv; ent[a].node = (uint16_t)(sv.node + 400); nbo_schedule(ent, nseg, &SC); CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) != NBO_S_OK);
        ent[a] = sv; nbo_schedule(ent, nseg, &SC); CHECK(nbo_scan(&SC, P.after, P.seq + 1, &g_io, &SO) == NBO_S_ADDR);                   // another part's seq
        CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) == NBO_S_OK);
        // a segment of the stream missing from the tail (its PASS is met where the tail has none)
        memmove(ent + 1, ent + 2, (size_t)(nseg - 2) * sizeof ent[0]); nbo_schedule(ent, nseg - 1, &SC);
        CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) == NBO_S_ORDER);
        // a PASS of the part the tail does not know
        gen_part(nseg, 1, 50, 1); memcpy(g_init, P.init, sizeof g_init);
        for (int k = 0; k < nseg; k++) order[k] = k;
        run_order(order, nseg, ent); nbo_schedule(ent, nseg, &SC);
        CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) == NBO_S_ORDER);
        // identity run of that stream all the same: as recorded
        be_reset(&B, 0, 0); memcpy(B.var, g_init, sizeof g_init);
        const uint8_t *resume = nbo_run(&SC, P.after, &g_ops, &res, &done);
        CHECK(res == NBO_R_OK && resume == P.end && same_events(1) && !memcmp(B.var, P.lin.var, sizeof B.var));
        // a tail with one segment more than the handle holds (its PASS lies in another handle): the identity run ends at the handle's
        // OP_END having executed everything as recorded
        gen_part(nseg, 1, 50, 0); memcpy(g_init, P.init, sizeof g_init);
        run_order(order, nseg, ent);
        static uint64_t other[8]; put_hdr((uint8_t *)other, T_PASS, 16);
        ent[nseg].addr = (uint64_t)(uintptr_t)other; ent[nseg].node = (uint16_t)(P.node[nseg - 1] + 1); ent[nseg].flags = 0; ent[nseg].pad = 0;
        nbo_schedule(ent, nseg + 1, &SC); CHECK(SC.identity);
        be_reset(&B, 0, 0); memcpy(B.var, g_init, sizeof g_init);
        resume = nbo_run(&SC, P.after, &g_ops, &res, &done);
        CHECK(res == NBO_R_EARLY_END && resume == P.end && done == nseg && same_events(1) && !memcmp(B.var, P.lin.var, sizeof B.var));
        CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) == NBO_S_MISSING);
        // a record that refuses the part
        uint8_t *x = (uint8_t *)P.pass[nseg - 1]; uint32_t h[2]; memcpy(h, x, 8);
        nbo_schedule(ent, nseg, &SC); CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) == NBO_S_OK);
        // (the last record before OP_END becomes the refusing one)
        const uint8_t *pc = P.pass[nseg - 1], *last = NULL;
        for (;;) { memcpy(h, pc, 8); if (h[0] == T_END) break; if (h[0] == T_JUMP) { uint64_t t; memcpy(&t, pc + 8, 8); pc = (const uint8_t *)(uintptr_t)t; continue; } last = pc; pc += h[1]; }
        memcpy(h, last, 8);
        if (h[0] != T_PASS && h[0] != T_KEY) { h[0] = T_REFUSE; memcpy((uint8_t *)last, h, 8); CHECK(nbo_scan(&SC, P.after, P.seq, &g_io, &SO) == NBO_S_REFUSED); }
        (void)x;
    }
    // a run whose start() fails (a key vanished between scan and run): reported, not silent
    gen_part(6, 1, 0, 0); memcpy(g_init, P.init, sizeof g_init);
    for (int k = 0; k < 6; k++) order[k] = k;
    order[2] = 3; order[3] = 2;
    run_order(order, 6, ent); nbo_schedule(ent, 6, &SC);   // no scan: no keys in the steps
    be_reset(&B, 0, 0); memcpy(B.var, g_init, sizeof g_init);
    nbo_run(&SC, P.after, &g_ops, &res, &done);
    CHECK(res == NBO_R_START && done == 2);
}

int main(int argc, char **argv) {
    test_tail();
    test_build();
    for (int i = 1; i < argc; i++) golden(argv[i]);
    test_stream();
    test_broken();
    printf("# stream model: %llu parts, identity runs %llu (%llu events), other orders run %llu (starts from a key %llu, end state put back %llu), refused: no complete key %llu, "
           "inversion without a state-only side %llu; mutations caught: start without the key %llu, no put-back %llu; broken tails scanned %llu\n",
           (unsigned long long)st_parts, (unsigned long long)st_ident, (unsigned long long)st_events, (unsigned long long)st_run, (unsigned long long)st_starts,
           (unsigned long long)st_puts, (unsigned long long)st_refused_key, (unsigned long long)st_refused_unsafe, (unsigned long long)st_mut_start,
           (unsigned long long)st_mut_put, (unsigned long long)st_scan_bad);
    printf(fails ? "FAILED: %d\n" : "nb_order_test: all passed\n", fails);
    return fails ? 1 : 0;
}
