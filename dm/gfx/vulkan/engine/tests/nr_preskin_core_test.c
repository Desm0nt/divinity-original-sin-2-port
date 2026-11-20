// [DOS2] NR-PRESKIN (docs/dos2/nr/NR-PRESKIN.md): dm/gfx/nr/nr_preskin.h - the record table rule against a brute-force model.
//   gcc -O2 -std=c11 -I dm/gfx/nr dm/gfx/vulkan/engine/tests/nr_preskin_core_test.c -o nr_preskin_core_test && ./nr_preskin_core_test
// 1. nrps_match: random records (values with hashes / leaf masks, leaf identities) and random wanted sets: the answer equals the model
//    "every wanted value is in the record with the same hash and comps, every leaf it reads has the same non-zero identity, the record
//    covers the vertices"; pos maps each wanted component to the record word of the same value (checked word by word).
// 2. the record of a draw's own plan always matches the same draw again (own reuse) and never a draw whose any read leaf moved.
// 3. nrps_take: bump allocations are aligned, disjoint, inside the arena, refused past it; nrps_nverts against a model.
#include "nr_preskin.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned long long checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; if (fails < 20) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t rs = 0x1234567;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static int model(const nrps_rec *r, const nrps_val *w, uint32_t nw, const uint64_t *cl, const uint64_t *cs, uint32_t nv) {
    if (r->nverts < nv) return 0;
    for (uint32_t i = 0; i < nw; i++) {
        int f = -1; for (uint32_t k = 0; k < r->nvals; k++) if (r->v[k].hash == w[i].hash && r->v[k].comps == w[i].comps) { f = (int)k; break; }
        if (f < 0) return 0;
        for (int l = 0; l < 16; l++) if ((w[i].loc_mask >> l & 1) && (r->loc_id[l] == 0 || r->loc_id[l] != cl[l])) return 0;
        for (int s = 0; s < NRPS_SLOTS; s++) if ((w[i].slot_mask >> s & 1) && (r->slot_id[s] == 0 || r->slot_id[s] != cs[s])) return 0;
    }
    return 1;
}

int main(void) {
    // 1 + 2
    for (int t = 0; t < 20000; t++) {
        nrps_rec r; memset(&r, 0, sizeof r); r.nverts = (uint32_t)(rnd() % 5000 + 1); r.nvals = (uint32_t)(rnd() % NRPS_MAXV + 1);
        uint32_t first = 0;
        for (uint32_t k = 0; k < r.nvals; k++) { r.v[k].hash = rnd() % 24 + 1; r.v[k].comps = (uint32_t)(rnd() % 2 + 1); r.v[k].first = first; first += r.v[k].comps;
            r.v[k].loc_mask = (uint32_t)(rnd() & 0xc00f); r.v[k].slot_mask = (uint32_t)(rnd() & 7); }
        r.m = first;
        for (int l = 0; l < 16; l++) r.loc_id[l] = rnd() % 3;
        for (int s = 0; s < NRPS_SLOTS; s++) r.slot_id[s] = rnd() % 3;
        nrps_val w[NRPS_MAXV]; uint32_t nw = (uint32_t)(rnd() % 6 + 1), wf = 0;
        for (uint32_t i = 0; i < nw; i++) {
            if (rnd() & 1) { const nrps_val *src = &r.v[rnd() % r.nvals]; w[i] = *src; }
            else { w[i].hash = rnd() % 24 + 1; w[i].comps = (uint32_t)(rnd() % 2 + 1); w[i].loc_mask = (uint32_t)(rnd() & 0xc00f); w[i].slot_mask = (uint32_t)(rnd() & 7); }
            for (int dup = 1; dup;) { dup = 0; for (uint32_t q = 0; q < i; q++) if (w[q].hash == w[i].hash) { w[i].hash += 1000 + i; dup = 1; } }   // a plan's values have distinct hashes
            w[i].first = wf; wf += w[i].comps;
        }
        uint64_t cl[16], cs[NRPS_SLOTS];
        for (int l = 0; l < 16; l++) cl[l] = (rnd() & 3) ? r.loc_id[l] : rnd() % 3;
        for (int s = 0; s < NRPS_SLOTS; s++) cs[s] = (rnd() & 3) ? r.slot_id[s] : rnd() % 3;
        const uint32_t nv = (uint32_t)(rnd() % 6000);
        uint32_t pos[64]; for (int j = 0; j < 64; j++) pos[j] = 0xFFFFFFFFu;
        const int got = nrps_match(&r, w, nw, cl, cs, nv, pos), want = model(&r, w, nw, cl, cs, nv);
        CHECK(got == want);
        if (got) for (uint32_t i = 0; i < nw; i++) {   // pos: the record word of the same value (first match by hash + comps)
            int f = -1; for (uint32_t k = 0; k < r.nvals; k++) if (r.v[k].hash == w[i].hash && r.v[k].comps == w[i].comps) { f = (int)k; break; }
            for (uint32_t j = 0; j < w[i].comps; j++) CHECK(pos[w[i].first + j] == r.v[f].first + j);
        }
        // own reuse: a record made from a plan's values with the draw's identities matches the same draw, and fails once a read leaf moves
        nrps_rec o; memset(&o, 0, sizeof o); o.nverts = nv + 1; o.nvals = nw; memcpy(o.v, w, sizeof(nrps_val) * nw); o.m = wf;
        uint32_t lm = 0, sm = 0; for (uint32_t i = 0; i < nw; i++) { lm |= w[i].loc_mask; sm |= w[i].slot_mask; }
        for (int l = 0; l < 16; l++) { cl[l] = rnd() | 1; o.loc_id[l] = (lm >> l & 1) ? cl[l] : 0; }
        for (int s = 0; s < NRPS_SLOTS; s++) { cs[s] = rnd() | 1; o.slot_id[s] = (sm >> s & 1) ? cs[s] : 0; }
        CHECK(nrps_match(&o, w, nw, cl, cs, nv, pos) == 1);
        CHECK(nrps_pos_identity(pos, wf));
        if (lm) { int l; do l = (int)(rnd() % 16); while (!(lm >> l & 1)); cl[l] ^= 0x100; CHECK(nrps_match(&o, w, nw, cl, cs, nv, pos) == 0); cl[l] ^= 0x100; }
        if (sm) { int s; do s = (int)(rnd() % NRPS_SLOTS); while (!(sm >> s & 1)); cs[s] ^= 0x100; CHECK(nrps_match(&o, w, nw, cl, cs, nv, pos) == 0); cs[s] ^= 0x100; }
        CHECK(nrps_match(&o, w, nw, cl, cs, nv + 2, pos) == 0);   // more vertices than the record covers
    }
    // identities are never 0
    for (int t = 0; t < 100000; t++) { CHECK(nrps_id_attr((uint32_t)rnd(), (uint32_t)rnd(), (uint32_t)rnd(), rnd(), rnd(), rnd(), rnd()) != 0); CHECK(nrps_id_ubo(rnd(), rnd(), rnd(), rnd()) != 0); }
    // 3
    for (int t = 0; t < 2000; t++) {
        nrps_bump b = {0, 0, 0}; b.size = rnd() % (1u << 20) + 1; b.used = 64; uint64_t last_end = 64; const uint64_t al = 1ull << (rnd() % 8);
        for (int k = 0; k < 64; k++) {
            const uint64_t n = rnd() % 70000, at = nrps_take(&b, n, al);
            const uint64_t a2 = (last_end + al - 1) / al * al;
            if (a2 > b.size || n > b.size - a2) CHECK(at == ~0ull);
            else { CHECK(at == a2); CHECK(at % al == 0); CHECK(at >= last_end); CHECK(at + n <= b.size); last_end = at + n; }
        }
        CHECK(b.peak <= b.size);
    }
    for (int t = 0; t < 100000; t++) {
        const uint64_t bytes = rnd() % 100000; const uint32_t stride = (uint32_t)(rnd() % 64), off = (uint32_t)(rnd() % 64), fsz = (uint32_t)(rnd() % 17);
        uint64_t m = 0; if (stride && fsz) for (uint64_t i = 0;; i++) { if ((uint64_t)off + i * stride + fsz > bytes) break; m = i + 1; if (i > 200000) break; }
        CHECK(nrps_nverts(bytes, stride, off, fsz) == m);
    }
    printf("nr_preskin_core_test: %llu checks, %llu failures -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
