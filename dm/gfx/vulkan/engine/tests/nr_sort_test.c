// [DOS2] NR-21 host test (dm/gfx/nr/nr_sort.h, docs/dos2/nr/NR-21.md): the condition of an order-free draw, the proof that the depth of
// a segment of such draws is the same bits in every order (a model of the depth test of a fixed-point attachment, fragments with ties,
// discard, LESS / LEQUAL mixed, no-effect draws), and that every refused condition really can change the result in some order (so the
// refusals are needed, not only cautious); the emission order (permutation, stability, grouping by first appearance, reversal); the
// bind model against an independent oracle; and the numbers of NR-21.md (pipeline binds of a segment before / after the sort).
// Build (Windows LLVM or any C11 compiler), from the repository root:
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nr_sort_test.c -o nr_sort_test && ./nr_sort_test
#include "../../../nr/nr_sort.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned long long checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; if (failures <= 20) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }
static uint32_t rn(uint32_t n) { return n ? rnd() % n : 0; }

// ---------------- 1. the condition ----------------
static void test_condition(void) {
    nrs_in ok = {0}; ok.depth_att = ok.depth_fixed = 1; ok.dt = ok.dw = 1; ok.op = 1;
    CHECK(nrs_why(&ok) == NRS_OK && nrs_effect(&ok) == NRS_EFFECT_MIN);
    nrs_in d = ok; d.op = 3; CHECK(nrs_why(&d) == NRS_OK && nrs_effect(&d) == NRS_EFFECT_MIN);
    d = ok; d.op = 0; CHECK(nrs_why(&d) == NRS_OK && nrs_effect(&d) == NRS_EFFECT_NONE);   // NEVER: no write
    for (int op = 0; op < 8; op++) {
        d = ok; d.op = (uint8_t)op; d.dw = 0; CHECK(nrs_why(&d) == NRS_OK && nrs_effect(&d) == NRS_EFFECT_NONE);   // no write: any function
        d = ok; d.op = (uint8_t)op; d.dt = 0; CHECK(nrs_why(&d) == NRS_OK && nrs_effect(&d) == NRS_EFFECT_NONE);   // no test: no write either
        d = ok; d.op = (uint8_t)op; CHECK((nrs_why(&d) == NRS_OK) == (op == 0 || op == 1 || op == 3));
        if (op == 2 || op >= 4) CHECK(nrs_why(&d) == NRS_W_DEPTHFN);
    }
    struct { size_t off; int why; } flags[] = {
        {offsetof(nrs_in, ab_off), NRS_W_GATE}, {offsetof(nrs_in, query), NRS_W_QUERY}, {offsetof(nrs_in, condr), NRS_W_CONDR},
        {offsetof(nrs_in, feedback), NRS_W_FEEDBACK}, {offsetof(nrs_in, storage), NRS_W_STORAGE}, {offsetof(nrs_in, color_written), NRS_W_COLOR},
        {offsetof(nrs_in, st), NRS_W_STENCIL}, {offsetof(nrs_in, bounds), NRS_W_BOUNDS}, {offsetof(nrs_in, a2c), NRS_W_A2C}};
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) { d = ok; ((uint8_t *)&d)[flags[i].off] = 1; CHECK(nrs_why(&d) == flags[i].why); }
    d = ok; d.depth_att = 0; CHECK(nrs_why(&d) == NRS_W_NODEPTH);
    d = ok; d.depth_fixed = 0; CHECK(nrs_why(&d) == NRS_W_FORMAT);
    for (int w = 0; w < NRS_W_N; w++) CHECK(nrs_why_name(w)[0] != '?');
    // the create-info form agrees with the state form on every combination
    for (unsigned m = 0; m < 1u << 9; m++) {
        nrs_in x = ok; x.dt = m & 1; x.dw = m >> 1 & 1; x.op = (uint8_t)(m >> 2 & 7); x.st = m >> 5 & 1; x.bounds = m >> 6 & 1; x.a2c = m >> 7 & 1; x.color_written = m >> 8 & 1;
        CHECK((nrs_why(&x) == NRS_OK) == (nrs_pipe_ok(1, x.dt, x.dw, x.op, x.st, x.bounds, x.a2c, x.color_written ? 0xfu : 0u) != 0));
    }
    CHECK(nrs_pipe_ok(0, 0, 0, 0, 0, 0, 0, 0) == 1 && nrs_pipe_ok(0, 0, 0, 0, 0, 0, 0, 1) == 0);
}

// ---------------- 2. the theorem on a model of the depth test ----------------
// A fragment: pixel, depth value (fixed point: an integer; float: a float), discarded or not. A draw: fragments in primitive order,
// a depth state, and what else it touches (colour, stencil, bounds, query) for the counterexamples.
#define W 8
#define H 8
#define PIX (W * H)
#define MAXF 48
#define MAXD 40
typedef struct { uint8_t px, kill; uint32_t z; float zf; } frag;
typedef struct {
    int dt, dw, op;                 // op: VkCompareOp
    int color;                      // 0 none, 1 "last writer wins" colour, 2 additive float colour
    int stencil;                    // 1: stencil test on (ALWAYS) with INCR on pass
    int bounds;                     // 1: depth bounds [lo, hi] on the stored value
    uint32_t lo, hi;
    int query;                      // 1: this draw's samples-passed count is reported
    uint32_t id;                    // its "colour" value
    int nf; frag f[MAXF];
} mdraw;
static uint32_t g_init = 0xffffffu;   // the depth the segment starts from (cleared to far; the GREATER counterexamples start from 0)
typedef struct { uint32_t d[PIX]; float df[PIX]; uint32_t c[PIX]; float cf[PIX]; uint8_t s[PIX]; uint64_t q[MAXD]; } mbuf;
static int cmp_u(int op, uint32_t z, uint32_t d) { switch (op) { case 0: return 0; case 1: return z < d; case 2: return z == d; case 3: return z <= d; case 4: return z > d; case 5: return z != d; case 6: return z >= d; default: return 1; } }
static int cmp_f(int op, float z, float d) { switch (op) { case 0: return 0; case 1: return z < d; case 2: return z == d; case 3: return z <= d; case 4: return z > d; case 5: return z != d; case 6: return z >= d; default: return 1; } }
static void run(const mdraw *dr, const uint32_t *perm, int n, int fl, mbuf *b) {
    for (int p = 0; p < PIX; p++) { b->d[p] = g_init; b->df[p] = (float)g_init / 16777215.0f; b->c[p] = 0; b->cf[p] = 0.0f; b->s[p] = 0; }
    memset(b->q, 0, sizeof b->q);
    for (int j = 0; j < n; j++) {
        const int i = perm ? (int)perm[j] : j; const mdraw *x = &dr[i];
        for (int k = 0; k < x->nf; k++) {
            const frag *f = &x->f[k]; if (f->kill) continue;
            if (x->bounds && !(b->d[f->px] >= x->lo && b->d[f->px] <= x->hi)) continue;   // the bounds test reads the stored value
            const int pass = !x->dt ? 1 : fl ? cmp_f(x->op, f->zf, b->df[f->px]) : cmp_u(x->op, f->z, b->d[f->px]);
            if (!pass) continue;
            if (x->dt && x->dw) { if (fl) b->df[f->px] = f->zf; else b->d[f->px] = f->z; }   // Vulkan: no depth write without the test
            if (x->stencil) b->s[f->px]++;
            if (x->color == 1) b->c[f->px] = x->id;
            if (x->color == 2) b->cf[f->px] += 0.1f * (float)(x->id + 1) + 1e7f * (float)(x->id & 1);
            if (x->query) b->q[i]++;
        }
    }
}
static int same(const mbuf *a, const mbuf *b, int fl) {
    if (fl) { if (memcmp(a->df, b->df, sizeof a->df)) return 0; } else if (memcmp(a->d, b->d, sizeof a->d)) return 0;
    return !memcmp(a->c, b->c, sizeof a->c) && !memcmp(a->cf, b->cf, sizeof a->cf) && !memcmp(a->s, b->s, sizeof a->s) && !memcmp(a->q, b->q, sizeof a->q);
}
static void gen_frags(mdraw *x, int ties) {
    x->nf = 1 + (int)rn(MAXF);
    for (int k = 0; k < x->nf; k++) {
        frag *f = &x->f[k]; f->px = (uint8_t)rn(PIX); f->kill = rn(4) == 0;
        f->z = ties ? 0x100000u * (1 + rn(6)) : rn(0x1000000u);   // few distinct values: many exact ties
        f->zf = (float)f->z / 16777215.0f;
    }
}
// an order-free draw of nrs_why: min writer (LESS / LEQUAL) or no effect (write off / test off / NEVER)
static void gen_free(mdraw *x, uint32_t id) {
    memset(x, 0, sizeof *x); x->id = id;
    switch (rn(6)) { case 0: case 1: x->dt = x->dw = 1; x->op = 1; break; case 2: case 3: x->dt = x->dw = 1; x->op = 3; break;
                     case 4: x->dt = 1; x->dw = 0; x->op = (int)rn(8); break; default: x->dt = 0; x->dw = (int)rn(2); x->op = (int)rn(8); if (rn(2)) { x->dt = 1; x->dw = 1; x->op = 0; } }
    gen_frags(x, rn(2) == 0);
}
static void shuffle(uint32_t *p, int n) { for (int i = n - 1; i > 0; i--) { const int j = (int)rn((uint32_t)i + 1); const uint32_t t = p[i]; p[i] = p[j]; p[j] = t; } }

static nrs_scratch S;
static void test_theorem(void) {
    static mdraw dr[MAXD]; static mbuf a, b; uint32_t perm[MAXD]; uint64_t k1[MAXD], k2[MAXD], k3[MAXD];
    unsigned long long orders = 0;
    for (int seg = 0; seg < 20000; seg++) {
        const int n = 2 + (int)rn(MAXD - 1);
        for (int i = 0; i < n; i++) { gen_free(&dr[i], (uint32_t)i); nrs_in q = {0}; q.depth_att = q.depth_fixed = 1; q.dt = (uint8_t)dr[i].dt; q.dw = (uint8_t)dr[i].dw; q.op = (uint8_t)dr[i].op; CHECK(nrs_why(&q) == NRS_OK); }
        run(dr, NULL, n, 0, &a);
        for (int i = 0; i < n; i++) { k1[i] = 1 + rn(5); k2[i] = 1 + rn(3); k3[i] = 1 + rn(4); }
        for (int how = 0; how < 3; how++) { nrs_order(&S, (uint32_t)n, how, k1, k2, k3, perm); run(dr, perm, n, 0, &b); CHECK(same(&a, &b, 0)); ++orders; }
        for (int r = 0; r < 4; r++) { for (int i = 0; i < n; i++) perm[i] = (uint32_t)i; shuffle(perm, n); run(dr, perm, n, 0, &b); CHECK(same(&a, &b, 0)); ++orders; }
        // and the depth equals min(initial, every surviving fragment of a min writer), the closed form of section 2 (b)
        uint32_t mn[PIX]; for (int p = 0; p < PIX; p++) mn[p] = 0xffffffu;
        for (int i = 0; i < n; i++) if (dr[i].dt && dr[i].dw && (dr[i].op == 1 || dr[i].op == 3)) for (int k = 0; k < dr[i].nf; k++) if (!dr[i].f[k].kill && dr[i].f[k].z < mn[dr[i].f[k].px]) mn[dr[i].f[k].px] = dr[i].f[k].z;
        CHECK(!memcmp(mn, a.d, sizeof mn));
    }
    printf("theorem: 20000 segments of order-free draws, %llu orders (recorded / sorted / reversed / 4 random each): depth bit-identical, = min(d0, z) closed form\n", orders);
}
// Every refusal is needed: for each refused condition there is a segment and an order whose result differs.
static int differs(void (*mk)(mdraw *, uint32_t), int fl, int tries) {
    static mdraw dr[8]; static mbuf a, b; uint32_t perm[8];
    for (int t = 0; t < tries; t++) {
        const int n = 2 + (int)rn(6);
        for (int i = 0; i < n; i++) mk(&dr[i], (uint32_t)i);
        run(dr, NULL, n, fl, &a);
        for (int r = 0; r < 8; r++) { for (int i = 0; i < n; i++) perm[i] = (uint32_t)i; shuffle(perm, n); run(dr, perm, n, fl, &b); if (!same(&a, &b, fl)) return 1; }
    }
    return 0;
}
// GREATER / GEQUAL writes alone are max(d, z) - order-free as well (a reversed-Z pass would qualify); mixed with LESS / LEQUAL they are
// not, which is why the condition keeps to one direction (LESS / LEQUAL: the NVN default and what DOS2's depth passes use)
static void mk_greater_only(mdraw *x, uint32_t id) { gen_free(x, id); x->dt = x->dw = 1; x->op = 4; }
static void mk_greater(mdraw *x, uint32_t id) { gen_free(x, id); x->dt = x->dw = 1; x->op = rn(2) ? 4 : 1; }
static void mk_always(mdraw *x, uint32_t id) { gen_free(x, id); x->dt = x->dw = 1; x->op = 7; }
static void mk_gequal(mdraw *x, uint32_t id) { gen_free(x, id); x->dt = x->dw = 1; x->op = rn(2) ? 6 : 3; }
static void mk_notequal(mdraw *x, uint32_t id) { gen_free(x, id); x->dt = x->dw = 1; x->op = 5; }
static void mk_color(mdraw *x, uint32_t id) { gen_free(x, id); x->dt = 1; x->dw = 1; x->op = 3; x->color = 1; }
static void mk_blend(mdraw *x, uint32_t id) { gen_free(x, id); x->dt = 0; x->dw = 0; x->color = 2; }   // additive float blend: rounding depends on order
static void mk_stencil(mdraw *x, uint32_t id) { gen_free(x, id); x->stencil = 1; x->dt = x->dw = 1; x->op = 1; }
static void mk_bounds(mdraw *x, uint32_t id) { gen_free(x, id); x->dt = x->dw = 1; x->op = 1; x->bounds = 1; x->lo = 0x300000u; x->hi = 0xffffffu; }
static void mk_query(mdraw *x, uint32_t id) { gen_free(x, id); x->dt = x->dw = 1; x->op = 1; x->query = 1; }
// float depth with signed zeros: LESS and LEQUAL keep / replace +0 by -0 depending on the order
static void mk_float0(mdraw *x, uint32_t id) { memset(x, 0, sizeof *x); x->id = id; x->dt = x->dw = 1; x->op = rn(2) ? 1 : 3; x->nf = 4;
    for (int k = 0; k < 4; k++) { x->f[k].px = (uint8_t)rn(2); x->f[k].zf = rn(2) ? 0.0f : -0.0f; } }
static void mk_float0_less(mdraw *x, uint32_t id) { mk_float0(x, id); x->op = 1; }
static void mk_float0_lequal(mdraw *x, uint32_t id) { mk_float0(x, id); x->op = 3; }
static void mk_fixed0(mdraw *x, uint32_t id) { mk_float0(x, id); for (int k = 0; k < 4; k++) x->f[k].z = 0; }   // the same in fixed point: one zero
static void test_counterexamples(void) {
    CHECK(differs(mk_always, 0, 200)); CHECK(differs(mk_notequal, 0, 200));
    g_init = 0x800000u; CHECK(differs(mk_greater, 0, 200)); CHECK(differs(mk_gequal, 0, 200)); g_init = 0; CHECK(!differs(mk_greater_only, 0, 400)); g_init = 0xffffffu;
    CHECK(differs(mk_color, 0, 200)); CHECK(differs(mk_blend, 0, 2000)); CHECK(differs(mk_stencil, 0, 200));
    CHECK(differs(mk_bounds, 0, 400)); CHECK(differs(mk_query, 0, 200));
    CHECK(differs(mk_float0_less, 1, 400)); CHECK(differs(mk_float0_lequal, 1, 400)); CHECK(!differs(mk_fixed0, 0, 400));
    // stencil: INCR on pass of a LESS test counts the passes, and how many fragments pass LESS depends on the order
    {   static mdraw dr[2]; static mbuf a, b; uint32_t rev[2] = {1, 0};
        memset(dr, 0, sizeof dr);
        for (int i = 0; i < 2; i++) { dr[i].dt = dr[i].dw = 1; dr[i].op = 1; dr[i].stencil = 1; dr[i].nf = 1; dr[i].f[0].px = 0; }
        dr[0].f[0].z = 100; dr[1].f[0].z = 200;                       // near then far: the far one fails; far then near: both pass
        run(dr, NULL, 2, 0, &a); run(dr, rev, 2, 0, &b); CHECK(!same(&a, &b, 0) && a.s[0] == 1 && b.s[0] == 2 && a.d[0] == b.d[0]); }
    printf("counterexamples: GREATER / GEQUAL writes mixed with LESS / LEQUAL (alone: max, order-free), ALWAYS / NOT_EQUAL writes, colour, float blend, stencil, depth bounds, samples counting, float +-0 (LESS and LEQUAL): each order-dependent; fixed-point zero: not\n");
}

// ---------------- 3. the emission order ----------------
static void test_order(void) {
    static uint64_t k1[NRS_CAP], k2[NRS_CAP], k3[NRS_CAP]; static uint32_t perm[NRS_CAP]; static uint8_t seen[NRS_CAP];
    for (int t = 0; t < 3000; t++) {
        const uint32_t n = t < 10 ? (uint32_t)t : t == 10 ? NRS_CAP : 1 + rn(1200);
        const uint32_t g1 = 1 + rn(t & 1 ? 8 : 300), g2 = 1 + rn(6), g3 = 1 + rn(50);
        for (uint32_t i = 0; i < n; i++) { k1[i] = 0x7000000000ull + 0x40 * rn(g1); k2[i] = rn(g2) * 0x1111; k3[i] = 0xabc00000ull + rn(g3); }
        nrs_order(&S, n, NRS_ORDER_SORTED, k1, k2, k3, perm);
        CHECK(nrs_perm_ok(perm, n, seen)); CHECK(nrs_stable_ok(&S, perm, n, 3));
        // grouping: the draws of one k1 are contiguous, and the groups come in the order of their first appearance
        { uint32_t groups = 0, firsts = 0; for (uint32_t j = 0; j < n; j++) if (!j || k1[perm[j]] != k1[perm[j - 1]]) ++groups;
          for (uint32_t i = 0; i < n; i++) { int seen_before = 0; for (uint32_t q = 0; q < i && !seen_before && n <= 1200; q++) seen_before = k1[q] == k1[i]; if (!seen_before) ++firsts; }
          if (n <= 1200) CHECK(groups == firsts);
          for (uint32_t j = 1; j < n; j++) CHECK(S.id[0][perm[j - 1]] <= S.id[0][perm[j]]); }
        nrs_order(&S, n, NRS_ORDER_REVERSED, k1, k2, k3, perm);
        CHECK(nrs_perm_ok(perm, n, seen)); for (uint32_t j = 0; j < n; j++) CHECK(perm[j] == n - 1 - j);
        nrs_order(&S, n, NRS_ORDER_RECORDED, k1, k2, k3, perm);
        for (uint32_t j = 0; j < n; j++) CHECK(perm[j] == j);
    }
    // a broken order is caught
    { uint32_t p[4] = {0, 1, 1, 3}; CHECK(!nrs_perm_ok(p, 4, seen)); uint32_t q[3] = {0, 3, 1}; CHECK(!nrs_perm_ok(q, 3, seen)); }
    { uint64_t a[4] = {5, 6, 5, 6}; uint32_t p[4]; nrs_order(&S, 4, NRS_ORDER_SORTED, a, NULL, NULL, p); CHECK(p[0] == 0 && p[1] == 2 && p[2] == 1 && p[3] == 3);
      uint32_t bad[4] = {2, 0, 1, 3}; CHECK(!nrs_stable_ok(&S, bad, 4, 1)); }
    // the hash table generation wraps
    { S.gen = 0xffffffffu; uint64_t a[3] = {1, 2, 1}; uint32_t p[3]; nrs_order(&S, 3, NRS_ORDER_SORTED, a, NULL, NULL, p); CHECK(p[0] == 0 && p[1] == 2 && p[2] == 1); }
}

// ---------------- 4. the bind model against an oracle ----------------
// The oracle: the bind cache of xbg_nr_draw.inc lane() written out per field, without digests or shortcuts.
typedef struct { int have_pipe, have_set, have_vp, have_sc, have_ib; uint64_t pipe, set, vp, sc, ib, vb[16]; int have_vb[16]; } oracle;
static unsigned oracle_one(oracle *o, const nrs_bind *b, nrs_count *c) {
    unsigned n = 1;
    if (!o->have_pipe || o->pipe != b->pipe) { o->have_pipe = 1; o->pipe = b->pipe; n++; c->pipe++; }
    if (!o->have_set || o->set != b->set) { o->have_set = 1; o->set = b->set; n++; c->set++; }
    if (!o->have_vp || o->vp != b->vp) { o->have_vp = 1; o->vp = b->vp; n++; c->vp++; }
    if (!o->have_sc || o->sc != b->sc) { o->have_sc = 1; o->sc = b->sc; n++; c->sc++; }
    // one bind per run of adjacent used bindings from a changed one up to the last changed one before a gap in the used set
    int changed[16] = {0};
    for (int i = 0; i < 16; i++) if ((b->used >> i & 1) && (!o->have_vb[i] || o->vb[i] != b->vb[i])) { changed[i] = 1; o->have_vb[i] = 1; o->vb[i] = b->vb[i]; }
    for (int i = 0; i < 16;) {
        if (!changed[i]) { i++; continue; }
        int last = i; for (int j = i + 1; j < 16 && (b->used >> j & 1); j++) if (changed[j]) last = j;
        n++; c->vb++; i = last + 1;
    }
    if (b->indexed && (!o->have_ib || o->ib != b->ib)) { o->have_ib = 1; o->ib = b->ib; n++; c->ib++; }
    c->cmds += n; c->draws++; return n;
}
static void test_model(void) {
    static nrs_bind b[1200]; static uint32_t perm[1200];
    for (int t = 0; t < 400; t++) {
        const uint32_t n = 1 + rn(1200);
        for (uint32_t i = 0; i < n; i++) {
            nrs_bind *x = &b[i]; memset(x, 0, sizeof *x);
            x->pipe = 1 + rn(12); x->set = rn(3) ? 1000 + i : 999; x->vp = 1 + rn(2); x->sc = 1 + rn(2); x->indexed = rn(5) != 0; x->ib = 1 + rn(3);
            x->used = (uint16_t)(1u | (rn(3) ? 0 : 2u) | (rn(4) ? 0 : 4u) | (rn(9) ? 0 : 0x10u));
            for (int s = 0; s < 16; s++) if (x->used >> s & 1) x->vb[s] = 1 + rn(4);
        }
        { static uint64_t kp[1200], kv[1200]; for (uint32_t i = 0; i < n; i++) { kp[i] = b[i].pipe; kv[i] = b[i].vb[0]; } nrs_order(&S, n, t & 1 ? NRS_ORDER_SORTED : NRS_ORDER_REVERSED, kp, kv, NULL, perm); }
        for (int ord = 0; ord < 2; ord++) {
            nrs_bstate st0; memset(&st0, 0, sizeof st0); nrs_count m = {0}, o = {0}; oracle orc; memset(&orc, 0, sizeof orc);
            nrs_count_order(&st0, b, ord ? perm : NULL, n, &m);
            for (uint32_t j = 0; j < n; j++) oracle_one(&orc, &b[ord ? perm[j] : j], &o);
            CHECK(!memcmp(&m, &o, sizeof m));
        }
        if (t & 1) {   // sorted by pipeline: one bind per distinct pipeline
            nrs_count m = {0}; nrs_bstate st0; memset(&st0, 0, sizeof st0); nrs_count_order(&st0, b, perm, n, &m);
            int seenp[13] = {0}; uint64_t distinct = 0; for (uint32_t i = 0; i < n; i++) if (!seenp[b[i].pipe]) { seenp[b[i].pipe] = 1; ++distinct; }
            CHECK(m.pipe == distinct);
            nrs_count r = {0}; nrs_count_order(&st0, b, NULL, n, &r); CHECK(m.pipe <= r.pipe);
        }
    }
    // an initial state that matches the first draw saves its binds
    { nrs_bind x; memset(&x, 0, sizeof x); x.pipe = 7; x.set = 8; x.vp = 9; x.sc = 10; x.used = 1; x.vb[0] = 11; x.indexed = 1; x.ib = 12;
      nrs_bstate st; memset(&st, 0, sizeof st); st.pipe = 7; st.set = 8; st.vp = 9; st.sc = 10; st.vb[0] = 11; st.ib = 12;
      nrs_count c = {0}; CHECK(nrs_count_one(&st, &x, &c) == 1 && c.cmds == 1); }
}

// ---------------- 5. numbers for NR-21.md ----------------
// A depth-only segment shaped like the pre-pass (gp1 / PREPASS.md: 526 draws, one DEP program per material) and like the shadow atlas
// (1116 draws over cascades with their own viewport): pipeline binds before / after, the vertex binds, and both keys of DM_NR_SORT_KEY.
static void numbers(const char *name, uint32_t n, uint32_t pipes, uint32_t run, uint32_t cascades, uint32_t chunks) {
    static nrs_bind b[2048]; static uint32_t perm[2048]; static uint64_t kp[2048], kv[2048], kb[2048];
    uint32_t cur = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!run || i % run == 0 || rn(4) == 0) cur = rn(pipes);   // runs of one material, the engine's order otherwise
        const uint32_t cas = cascades ? i * cascades / n : 0;
        nrs_bind *x = &b[i]; memset(x, 0, sizeof *x);
        x->pipe = 0x100 + cur + 1000 * (cascades ? cas % 2 : 0); x->set = 0x5000 + i; x->vp = 1 + cas; x->sc = 1 + cas; x->indexed = 1; x->ib = 1 + rn(chunks > 1 ? 2 : 1);
        x->used = 1; x->vb[0] = 1 + rn(chunks);
        kp[i] = x->pipe; kv[i] = x->vp; kb[i] = x->vb[0];
    }
    nrs_bstate st0; memset(&st0, 0, sizeof st0);
    nrs_count r = {0}, s0 = {0}, s1 = {0};
    nrs_count_order(&st0, b, NULL, n, &r);
    nrs_order(&S, n, NRS_ORDER_SORTED, kv, kp, kb, perm); nrs_count_order(&st0, b, perm, n, &s0);   // DM_NR_SORT_KEY=0: viewport / scissor, pipeline, vertex
    nrs_order(&S, n, NRS_ORDER_SORTED, kp, kv, kb, perm); nrs_count_order(&st0, b, perm, n, &s1);   // DM_NR_SORT_KEY=1: pipeline, viewport / scissor, vertex
    printf("  %-34s %4u draws: pipeline binds %4llu -> key 0 %4llu / key 1 %4llu; viewport+scissor %3llu -> %3llu / %3llu; vertex %4llu -> %4llu / %4llu; vkCmd* %5llu -> %5llu / %5llu\n",
           name, n, (unsigned long long)r.pipe, (unsigned long long)s0.pipe, (unsigned long long)s1.pipe, (unsigned long long)(r.vp + r.sc), (unsigned long long)(s0.vp + s0.sc),
           (unsigned long long)(s1.vp + s1.sc), (unsigned long long)r.vb, (unsigned long long)s0.vb, (unsigned long long)s1.vb, (unsigned long long)r.cmds,
           (unsigned long long)s0.cmds, (unsigned long long)s1.cmds);
    CHECK(s0.pipe <= r.pipe && s1.pipe <= s0.pipe && s0.vp + s0.sc <= r.vp + r.sc);
}

int main(void) {
    test_condition();
    test_theorem();
    test_counterexamples();
    test_order();
    test_model();
    printf("model (bind cache rules of nr_draw, random engine order inside runs of one material):\n");
    numbers("pre-pass, 120 materials, runs of 4", 526, 120, 4, 0, 1);
    numbers("pre-pass, 60 materials, runs of 2", 526, 60, 2, 0, 1);
    numbers("pre-pass, 200 materials, no runs", 526, 200, 0, 0, 1);
    numbers("shadow atlas, 4 cascades, 80 progs", 1116, 80, 3, 4, 1);
    numbers("shadow atlas, 3 cascades, 40 progs", 1116, 40, 6, 3, 3);
    printf("%s: %llu checks, %llu failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
