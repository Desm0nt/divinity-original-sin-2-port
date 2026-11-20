// [DOS2] NR-MDI (docs/dos2/nr/NR-MDI.md): the batching model nr_mdi.h against a brute-force model on random draw streams.
//   - recorded-order batches = maximal runs of consecutive depth-only draws equal in every key field (brute force: field-by-field compare
//     of neighbours, a non-depth draw breaks the run); sizes histogram, draws in runs of 2+, set rebinds inside runs;
//   - sorted batches = per pass, distinct keys among the order-free depth draws + one per non-order-free depth draw;
//   - the change counters between consecutive depth draws of one pass;
//   - mutations of nrmdi_same (a field ignored) are caught by the brute force.
#include "../../../nr/nr_mdi.h"
#include <stdio.h>
#include <stdlib.h>
static int checks, fails;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static int brute_same(const nrmdi_draw *a, const nrmdi_draw *b) {
    if (a->pass != b->pass || a->pipe != b->pipe || a->layout != b->layout || a->set != b->set || a->used != b->used || a->ib != b->ib || a->ibo != b->ibo
        || a->it != b->it || a->vp != b->vp || a->sc != b->sc || a->inst != b->inst || a->first_inst != b->first_inst || a->noff != b->noff) return 0;
    for (int i = 0; i < 16; i++) if ((a->used >> i & 1) && (a->vb[i] != b->vb[i] || a->vbo[i] != b->vbo[i])) return 0;
    return a->noff <= NRMDI_MAXOFF;
}
enum { N = 4000 };
static int big_pass, ring;   // 1: passes may outgrow the model's key set (then it may only count MORE sorted batches)
static nrmdi_draw ds[N];
static void gen(int variety) {
    uint64_t pass = 1; int in_pass = 0;
    for (int i = 0; i < N; i++) {
        nrmdi_draw *d = &ds[i]; memset(d, 0, sizeof *d);
        if (rnd() % 300 == 0 || (big_pass ? 0 : in_pass >= 1500)) { ++pass; in_pass = 0; }
        ++in_pass;
        const nrmdi_draw *p = i ? &ds[i - 1] : NULL;
        const int keep = p && (int)(rnd() % 100) < 100 - variety;   // keep the previous draw's state (then change one field maybe)
        if (keep) { *d = *p; d->pass = pass; }
        else { d->pass = pass; d->pipe = 1 + rnd() % 6; d->layout = 1 + rnd() % 2; d->set = 1 + rnd() % 3; d->used = 1u | (unsigned)(rnd() % 2) << 1;
               for (int b = 0; b < 2; b++) if (d->used >> b & 1) { d->vb[b] = 1 + rnd() % 4; d->vbo[b] = (rnd() % 3) * 4; }
               d->ib = 1 + rnd() % 3; d->ibo = 0; d->it = 1 + (unsigned)(rnd() % 2); d->vp = 7; d->sc = 9; d->inst = 1; d->first_inst = 0; d->noff = 2 + (uint32_t)(rnd() % 2); }
        for (uint32_t k = 0; k < d->noff; k++) { if (ring && keep && k == 0) d->off[k] = p->off[k] + 256; else if (rnd() % 3 == 0) d->off[k] = (uint32_t)(rnd() % 50) * 256; }   // per-draw blocks move (ring: slot 0 steps like the engine ring)
        if (rnd() % 20 == 0) d->vp = 8;
        d->depth_only = rnd() % 25 != 0; d->order_free = d->depth_only && rnd() % 10 != 0;
    }
}
static void run(int variety) {
    gen(variety);
    static nrmdi_state t; memset(&t, 0, sizeof t); nrmdi_stats s; memset(&s, 0, sizeof s);
    for (int i = 0; i < N; i++) nrmdi_note(&t, &s, &ds[i]);
    nrmdi_flush(&t, &s);
    // brute force
    uint64_t batches = 0, in_multi = 0, rebinds = 0, sizes[6] = {0}, depth = 0, sorted = 0, chg[8] = {0}, chg_draws = 0;
    int run_n = 0;
    for (int i = 0; i < N; i++) {
        const nrmdi_draw *d = &ds[i];
        if (!d->depth_only) { if (run_n) { ++batches; if (run_n >= 2) in_multi += (uint64_t)run_n; sizes[run_n <= 1 ? 0 : run_n == 2 ? 1 : run_n <= 4 ? 2 : run_n <= 8 ? 3 : run_n <= 32 ? 4 : 5]++; } run_n = 0; continue; }
        ++depth;
        const nrmdi_draw *p = i && ds[i - 1].depth_only ? &ds[i - 1] : NULL;
        if (p && p->pass == d->pass) {
            ++chg_draws;
            if (p->pipe != d->pipe) chg[0]++;
            int setc = p->layout != d->layout || p->set != d->set || p->noff != d->noff, offc = 0;
            if (!setc) for (uint32_t k = 0; k < d->noff; k++) if (p->off[k] != d->off[k]) offc = 1;
            if (setc) chg[1]++; if (offc) chg[2]++;
        }
        if (p && brute_same(p, d)) { ++run_n; int offc = 0; for (uint32_t k = 0; k < d->noff; k++) if (p->off[k] != d->off[k]) offc = 1; if (offc) ++rebinds; }
        else { if (run_n) { ++batches; if (run_n >= 2) in_multi += (uint64_t)run_n; sizes[run_n <= 1 ? 0 : run_n == 2 ? 1 : run_n <= 4 ? 2 : run_n <= 8 ? 3 : run_n <= 32 ? 4 : 5]++; } run_n = 1; }
    }
    if (run_n) { ++batches; if (run_n >= 2) in_multi += (uint64_t)run_n; sizes[run_n <= 1 ? 0 : run_n == 2 ? 1 : run_n <= 4 ? 2 : run_n <= 8 ? 3 : run_n <= 32 ? 4 : 5]++; }
    // sorted: per pass, distinct brute keys among order-free depth draws
    for (int i = 0; i < N; i++) {
        const nrmdi_draw *d = &ds[i]; if (!d->depth_only) continue;
        if (!d->order_free) { ++sorted; continue; }
        int seen = 0; for (int j = i - 1; j >= 0 && ds[j].pass == d->pass; j--) if (ds[j].depth_only && ds[j].order_free && brute_same(&ds[j], d)) { seen = 1; break; }
        // the model keys by hash: equal key fields -> same key; (noff > MAXOFF never generated here)
        if (!seen) ++sorted;
    }
    CHECK(s.depth == depth, "depth %llu vs %llu", (unsigned long long)s.depth, (unsigned long long)depth);
    CHECK(s.batches == batches, "variety %d: batches %llu vs brute %llu", variety, (unsigned long long)s.batches, (unsigned long long)batches);
    CHECK(s.in_multi == in_multi, "in batches of 2+: %llu vs %llu", (unsigned long long)s.in_multi, (unsigned long long)in_multi);
    CHECK(s.batched_rebinds == rebinds, "rebinds %llu vs %llu", (unsigned long long)s.batched_rebinds, (unsigned long long)rebinds);
    for (int k = 0; k < 6; k++) CHECK(s.sizes[k] == sizes[k], "size bin %d: %llu vs %llu", k, (unsigned long long)s.sizes[k], (unsigned long long)sizes[k]);
    if (big_pass) CHECK(s.sorted_batches >= sorted, "big passes: sorted %llu below brute %llu", (unsigned long long)s.sorted_batches, (unsigned long long)sorted);
    else CHECK(s.sorted_batches == sorted, "variety %d: sorted %llu vs brute %llu", variety, (unsigned long long)s.sorted_batches, (unsigned long long)sorted);
    CHECK(s.chg_draws == chg_draws && s.chg[0] == chg[0] && s.chg[1] == chg[1] && s.chg[2] == chg[2], "changes %llu/%llu/%llu/%llu vs %llu/%llu/%llu/%llu",
          (unsigned long long)s.chg_draws, (unsigned long long)s.chg[0], (unsigned long long)s.chg[1], (unsigned long long)s.chg[2],
          (unsigned long long)chg_draws, (unsigned long long)chg[0], (unsigned long long)chg[1], (unsigned long long)chg[2]);
    CHECK(s.sorted_batches <= s.batches + s.depth, "sanity");
    // arithmetic batches: every maximal run of 2+ whose every offset slot steps by one constant delta
    { uint64_t ab = 0, ad = 0; int i = 0;
      while (i < N) { if (!ds[i].depth_only) { ++i; continue; }
          int j = i + 1; while (j < N && ds[j].depth_only && brute_same(&ds[j - 1], &ds[j])) ++j;
          if (j - i >= 2) { int ok = 1; for (uint32_t k = 0; k < ds[i].noff && ok; k++) { const int64_t dl = (int64_t)ds[i + 1].off[k] - (int64_t)ds[i].off[k];
                  for (int q = i + 2; q < j; q++) if ((int64_t)ds[q].off[k] - (int64_t)ds[q - 1].off[k] != dl) { ok = 0; break; } }
              if (ok) { ++ab; ad += (uint64_t)(j - i); } }
          i = j; }
      CHECK(s.arith_batches == ab && s.arith_draws == ad, "arithmetic batches %llu/%llu vs brute %llu/%llu", (unsigned long long)s.arith_batches, (unsigned long long)s.arith_draws, (unsigned long long)ab, (unsigned long long)ad); }
}
int main(void) {
    for (int v = 0; v <= 100; v += 5) for (int r = 0; r < 20; r++) run(v);
    big_pass = 1; for (int r = 0; r < 50; r++) run(100); big_pass = 0;
    ring = 1; for (int v = 0; v <= 60; v += 10) for (int r = 0; r < 20; r++) run(v); ring = 0;
    // every key field matters: two draws differing in exactly one field never merge
    nrmdi_draw a; memset(&a, 0, sizeof a); a.pass = 1; a.pipe = 2; a.layout = 3; a.set = 4; a.used = 3; a.vb[0] = 5; a.vb[1] = 6; a.vbo[1] = 8; a.ib = 7; a.it = 2; a.vp = 1; a.sc = 1; a.inst = 1; a.noff = 2; a.depth_only = 1;
    CHECK(nrmdi_same(&a, &a), "identical draws merge");
    nrmdi_draw b = a; b.off[0] = 256; CHECK(nrmdi_same(&a, &b), "dynamic offsets may differ");
#define DIFF(f, v) do { nrmdi_draw c = a; c.f = v; CHECK(!nrmdi_same(&a, &c) && nrmdi_key(&a) != nrmdi_key(&c), "field " #f " separates"); } while (0)
    DIFF(pass, 9); DIFF(pipe, 9); DIFF(layout, 9); DIFF(set, 9); DIFF(used, 1); DIFF(vb[1], 9); DIFF(vbo[0], 4); DIFF(ib, 9); DIFF(ibo, 2); DIFF(it, 1); DIFF(vp, 2); DIFF(sc, 2); DIFF(inst, 2); DIFF(first_inst, 1); DIFF(noff, 3);
    { nrmdi_draw c = a; c.noff = NRMDI_MAXOFF + 1; nrmdi_draw e = c; CHECK(!nrmdi_same(&c, &e), "more offsets than the model holds: never batched"); }
    printf("nr_mdi_test: %d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
