// [DOS2] NR-09 host test: the record tail of OP_X_NR_DRAW (dm/gfx/nvn/nvn_nb_draw.h nrd_tail), the decisions of the short draw path
// (dm/gfx/nr/nr_draw.h: variant words, index / vertex offset folds, bind runs, the DM_NR_DRAW=2 comparison helpers) and the short path's
// pipeline step on NR-07's direct table (dm/gfx/nr/nr_pipe.h nrp_tab) against an oracle on a random draw stream; then the model of
// vkCmd* a draw (old tail / index fold / vertex fold / both) on streams shaped like the phone's campaign frame, for docs/dos2/nr/NR-09.md.
// Build (Windows LLVM or any C11 compiler), from the repository root:
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nr_draw_test.c dm/gfx/nr/nr_pipe.c -o nr_draw_test && ./nr_draw_test
#include "../../../nvn/nvn_nb_draw.h"
#include "../../../nr/nr_draw.h"
#include "../../../nr/nr_pipe.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
static long checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { if (++fails < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint32_t rn(uint32_t n) { return (uint32_t)(rnd() % n); }

// ---- the record: OP_X_NB_DRAW + NBD_F_NR + tail ----
static void test_tail(void) {
    for (int it = 0; it < 2000; it++) {
        nbd_shadow s; memset(&s, 0, sizeof s);
        const unsigned nb = rn(NBD_MAX_BINDS + 1);
        for (unsigned i = 0; i < nb; i++) { const unsigned kind = rn(NBD_B_KINDS); CHECK(nbd_add(&s, kind, (int)rn(6), (int)rn(kind == NBD_B_UBO || kind == NBD_B_IMG ? 16 : 128), rnd(), rnd())); }
        s.h.draw = NBD_DRAW_IDX; s.h.prim = 4; s.h.count = (int32_t)rn(100000); s.h.engine_op = 36; s.h.texgen = rn(1000);
        const int with = it & 1;
        if (with) s.h.flags |= NBD_F_NR;
        uint8_t buf[sizeof(nbd_shadow) + sizeof(nrd_tail) + 8];
        memset(buf, 0xee, sizeof buf);
        const uint32_t size = nbd_size(&s) + (with ? (uint32_t)sizeof(nrd_tail) : 0u);
        CHECK(nbd_pack(&s, buf, size) == nbd_size(&s));
        const uint32_t key = rn(16384) + 1, ts = rn(65536); const unsigned why = rn(256), site = rn(25);
        CHECK(nrd_tail_put(buf, size, key, ts, why, site) == with);
        // every reader of OP_X_NB_DRAW takes the record as before (the payload may be longer: the tail, the 8-byte alignment)
        nbd_hdr h; const uint8_t *b = NULL; nbd_shadow u;
        CHECK(nbd_view(buf, size, &h, &b) == 0 && h.nbind == nb && b == buf + sizeof(nbd_hdr));
        CHECK(nbd_unpack(buf, size, &u) == 0 && u.h.count == s.h.count && !memcmp(u.bind, s.bind, sizeof(nbd_bind) * nb));
        nrd_tail t; memset(&t, 0, sizeof t);
        CHECK(nrd_tail_get(buf, size, &h, &t) == with);
        if (with) {
            CHECK(t.key == key && t.texset == ts && t.why == why && t.site == site && t.ver == NRD_VER && t._pad == 0);
            CHECK(nrd_tail_get(buf, size + 7, &h, &t) == 1);              // the record's alignment tail
            CHECK(nrd_tail_get(buf, size - 1, &h, &t) == 0);              // cut short: a plain record
            buf[nrd_tail_off(&h) + offsetof(nrd_tail, ver)] = NRD_VER + 1;
            CHECK(nrd_tail_get(buf, size, &h, &t) == 0);                  // another version: a plain record
            nbd_hdr h2 = h; h2.flags &= (uint8_t)~NBD_F_NR;
            CHECK(nrd_tail_get(buf, size, &h2, &t) == 0);
        } else CHECK(buf[nbd_size(&s)] == 0xee);                          // nothing written past a plain record
        CHECK(nrd_tail_put(buf, nrd_tail_off(&h) + sizeof(nrd_tail) - 1, 1, 1, 0, 0) == 0);   // no room
    }
    CHECK(sizeof(nrd_tail) == 16 && (NBD_F_NR & (NBD_F_REN | NBD_F_REN_ON | NBD_F_COND | NBD_F_CHECK)) == 0);
}

// ---- folds ----
static void test_index_fold(void) {
    for (int it = 0; it < 200000; it++) {
        const unsigned isz = 1 + rn(2);
        const uint64_t off = it & 1 ? rnd() % (1ull << 34) : (rnd() % (1ull << 24)) & ~((1ull << isz) - 1);
        uint64_t bo; uint32_t first;
        nr_fold_index(off, isz, 1, &bo, &first);
        CHECK(bo + ((uint64_t)first << isz) == off);
        CHECK(bo < (1ull << isz) || first == 0);
        CHECK(nr_ib_same(off, bo, first, isz));
        nr_fold_index(off, isz, 0, &bo, &first);
        CHECK(bo == off && first == 0);
    }
    uint64_t bo; uint32_t first;
    nr_fold_index((0x100000000ull << 2) + 8, 2, 1, &bo, &first);   // more than 2^32 indices in front: not folded
    CHECK(bo == (0x100000000ull << 2) + 8 && first == 0);
}
static void test_vertex_fold(void) {
    for (int it = 0; it < 200000; it++) {
        unsigned used = rn(1 << 6), inst = rn(1 << 6) & used;
        uint32_t stride[16] = {0}; uint64_t off[16] = {0}, out[16];
        for (unsigned i = 0; i < 6; i++) if (used >> i & 1) { stride[i] = rn(8) ? 4 * (1 + rn(24)) : 0; off[i] = rnd() % (1u << 26); }
        const int64_t base = (int64_t)rn(1 << 20) - (rn(4) ? 0 : 1000);
        memset(out, 0xab, sizeof out);
        const uint32_t k = nr_fold_vertex(used, inst, stride, off, base, 1, out);
        uint64_t kmin = ~0ull; int any = 0;
        for (unsigned i = 0; i < 6; i++) if ((used >> i & 1) && !(inst >> i & 1) && stride[i]) { any = 1; if (off[i] / stride[i] < kmin) kmin = off[i] / stride[i]; }
        CHECK(any ? k == kmin : k == 0);
        for (unsigned i = 0; i < 6; i++) if (used >> i & 1) {
            const int per_inst = inst >> i & 1;
            if (per_inst || !stride[i]) CHECK(out[i] == off[i]);
            else {
                CHECK(out[i] + (uint64_t)k * stride[i] == off[i]);
                // every vertex index the draw can produce is fetched from the same byte
                for (int v = 0; v < 3; v++) { const int64_t idx = base + (int64_t)rn(65536); if (idx >= 0) CHECK(out[i] + (uint64_t)(idx + k) * stride[i] == off[i] + (uint64_t)idx * stride[i]); }
            }
            CHECK(nr_vb_same(off[i], base, out[i], base + k, stride[i], per_inst));
            if (k && !per_inst && stride[i]) CHECK(!nr_vb_same(off[i], base, out[i], base, stride[i], per_inst));   // the comparison sees a lost fold
        }
        uint64_t o2[16];
        CHECK(nr_fold_vertex(used, inst, stride, off, base, 0, o2) == 0);
        for (unsigned i = 0; i < 6; i++) if (used >> i & 1) CHECK(o2[i] == off[i]);
    }
    {   // base + k past int32: not folded
        uint32_t stride[16] = {4}; uint64_t off[16] = {4ull * 0x7fffffffull}, out[16];
        CHECK(nr_fold_vertex(1, 0, stride, off, 5, 1, out) == 0 && out[0] == off[0]);
        off[0] = 4ull * 1000; CHECK(nr_fold_vertex(1, 0, stride, off, 0x7fffffff - 999, 1, out) == 0 && out[0] == off[0]);
        CHECK(nr_fold_vertex(1, 0, stride, off, 0x7fffffff - 1000, 1, out) == 1000 && out[0] == 0);
    }
}
static void test_runs(void) {
    for (int it = 0; it < 200000; it++) {
        const unsigned used = rn(1 << 16), changed = rn(1 << 16);
        uint8_t f[8], k[8];
        const unsigned n = nr_vb_runs(changed, used, f, k);
        unsigned cover = 0, prev_end = 0;
        for (unsigned r = 0; r < n; r++) {
            CHECK(k[r] >= 1 && f[r] + k[r] <= 16 && (r == 0 || f[r] > prev_end));   // ascending, apart
            for (unsigned j = 0; j < k[r]; j++) cover |= 1u << (f[r] + j);
            CHECK((changed & used) >> f[r] & 1);                                   // a run starts and ends on a changed binding
            CHECK((changed & used) >> (f[r] + k[r] - 1) & 1);
            prev_end = f[r] + k[r] - 1u;
        }
        CHECK((cover & ~used) == 0);                  // never a binding the pipeline does not use
        CHECK((cover & changed & used) == (changed & used));
        // minimal: one run per group of changed bindings that no unused binding separates
        unsigned want = 0; int open = 0;
        for (unsigned i = 0; i < 16; i++) { if (!(used >> i & 1)) { open = 0; continue; } if (changed >> i & 1) { if (!open) { want++; open = 1; } } }
        CHECK(n == want);
    }
    uint8_t f[8], k[8];
    CHECK(nr_vb_runs(0, 7, f, k) == 0);
    CHECK(nr_vb_runs(5, 7, f, k) == 1 && f[0] == 0 && k[0] == 3);   // 0 and 2 changed, 1 used: one command for 0..2
    CHECK(nr_vb_runs(5, 5, f, k) == 2 && f[0] == 0 && k[0] == 1 && f[1] == 2 && k[1] == 1);
}
static void test_var(void) {
    // every field lands in its own bits: changing one field changes the words, nothing else aliases it
    uint32_t gid[6] = {1, 2, 3, 4, 5, 6};
    nr_draw_var_t a, b;
    nr_draw_var(&a, 0x1000, 0x2000, 0x3000, 0x4000, 4, 1, 3, 7, 1, 0x34, 0, gid);
#define DIFF(...) do { nr_draw_var(&b, __VA_ARGS__); CHECK(memcmp(&a, &b, sizeof a) != 0); } while (0)
    DIFF(0x1008, 0x2000, 0x3000, 0x4000, 4, 1, 3, 7, 1, 0x34, 0, gid);
    DIFF(0x1000, 0x2008, 0x3000, 0x4000, 4, 1, 3, 7, 1, 0x34, 0, gid);
    DIFF(0x1000, 0x2000, 0x3008, 0x4000, 4, 1, 3, 7, 1, 0x34, 0, gid);
    DIFF(0x1000, 0x2000, 0x3000, 0x4008, 4, 1, 3, 7, 1, 0x34, 0, gid);
    DIFF(0x1000, 0x2000, 0x3000, 0x4000, 5, 1, 3, 7, 1, 0x34, 0, gid);
    DIFF(0x1000, 0x2000, 0x3000, 0x4000, 4, 4, 3, 7, 1, 0x34, 0, gid);
    DIFF(0x1000, 0x2000, 0x3000, 0x4000, 4, 1, 4, 7, 1, 0x34, 0, gid);
    DIFF(0x1000, 0x2000, 0x3000, 0x4000, 4, 1, 3, 15, 1, 0x34, 0, gid);
    DIFF(0x1000, 0x2000, 0x3000, 0x4000, 4, 1, 3, 7, 0, 0x34, 0, gid);
    DIFF(0x1000, 0x2000, 0x3000, 0x4000, 4, 1, 3, 7, 1, 0x35, 0, gid);
    DIFF(0x1000, 0x2000, 0x3000, 0x4000, 4, 1, 3, 7, 1, 0x34, 1, gid);
    for (int g = 0; g < 6; g++) { uint32_t g2[6]; memcpy(g2, gid, sizeof g2); g2[g] ^= 1u << rn(10); DIFF(0x1000, 0x2000, 0x3000, 0x4000, 4, 1, 3, 7, 1, 0x34, 0, g2); }
#undef DIFF
    // [NR-30] the pre-pass override bit is its own: no field of w[4] (prim 0xff, samples 0xff, rt_n 15, colours 0xff, depth, restart,
    // any depth format) reaches it, and setting it changes the words
    for (int it = 0; it < 20000; it++) {
        nr_draw_var_t v; nr_draw_var(&v, rn(1u << 30), rn(1u << 30), rn(1u << 30), rn(1u << 30), rn(256), rn(256), rn(16), rn(256), rn(2), (unsigned)rn(1u << 30) * 4u + rn(4), rn(2), gid);
        CHECK(!(v.w[4] & NR_DRAW_VAR_PP));
        nr_draw_var_t w = v; w.w[4] |= NR_DRAW_VAR_PP; CHECK(memcmp(&v, &w, sizeof v) != 0);
    }
    // the group ids are exact (each < 1024): two different tuples never pack to the same word
    for (int it = 0; it < 100000; it++) {
        uint32_t x[6], y[6]; int same = 1;
        for (int i = 0; i < 6; i++) { x[i] = rn(1024); y[i] = rn(8) ? x[i] : rn(1024); same &= x[i] == y[i]; }
        CHECK((nr_draw_gids(x) == nr_draw_gids(y)) == same);
    }
    CHECK(sizeof(nr_draw_var_t) == sizeof(nrp_var) && NRP_VAR_WORDS == 6);
}

// ---- the pipeline step: the direct table against an oracle ----
// A "state" = (key id, the 12 inputs of the variant). The oracle pipeline is a function of all of them (what the old path's memo
// returns). The short path looks the table up; a miss falls back to the oracle and teaches the table (nrdraw::done). The engine key id
// alone must never decide: two draws of one key id with different host state get different pipelines.
typedef struct { uint32_t key; uint64_t vs, fs, lay, pd; unsigned prim, samples, rt_n, colors, depth, dfmt, re; uint32_t gid[6]; } tstate;
static uint64_t oracle(const tstate *s) {
    uint64_t h = 0xcbf29ce484222325ull; const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < sizeof *s; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h | 1;
}
static void test_table(void) {
    nrp_tab T; CHECK(nrp_tab_init(&T) == 0);
    enum { NS = 600 };
    static tstate st[NS];
    memset(st, 0, sizeof st);
    for (int i = 0; i < NS; i++) {
        tstate *s = &st[i];
        s->key = 1 + rn(150);   // ~4 host variants a key id on average: the row walk is exercised
        s->vs = 0x70000000ull + 64 * rn(40); s->fs = 0x71000000ull + 64 * rn(40); s->lay = 0x72000000ull + 64 * rn(8); s->pd = 0x73000000ull + 64 * rn(6);
        s->prim = 4 + rn(2); s->samples = 1; s->rt_n = rn(4); s->colors = rn(8); s->depth = rn(2); s->dfmt = s->depth ? 0x34 : 0; s->re = rn(2);
        for (int g = 0; g < 6; g++) s->gid[g] = rn(6);
    }
    uint64_t served = 0, wrong = 0, learned = 0, clears = 0;
    for (int it = 0; it < 2000000; it++) {
        const tstate *s = &st[rn(8) ? rn(40) : rn(NS)];   // a few hot states, a long tail
        nr_draw_var_t v; nrp_var pv;
        nr_draw_var(&v, s->vs, s->fs, s->lay, s->pd, s->prim, s->samples, s->rt_n, s->colors, s->depth, s->dfmt, s->re, s->gid);
        memcpy(pv.w, v.w, sizeof pv.w);
        const nrp_tent *e = nrp_tab_find(&T, s->key, &pv);
        if (e) { served++; if (e->pipe != oracle(s)) wrong++; }
        else { nrp_tent *n = nrp_tab_put(&T, s->key, &pv); CHECK(n != NULL); if (n) { n->pipe = oracle(s); n->used = 1; n->stride[0] = 32; learned++; } }
        if (!rn(400000)) { nrp_tab_clear(&T); clears++; }                       // the caches moved (nbpipe::epoch)
        if (!rn(300000)) nrp_tab_kill(&T, oracle(&st[rn(40)]));                 // a pipeline evicted
    }
    CHECK(wrong == 0);
    CHECK(served > 1900000);
    printf("  table: 2 000 000 draws over 600 states on 150 key ids: served %llu, taught %llu, wrong pipeline %llu, %llu clears; first-of-row hits %llu of %llu\n",
           (unsigned long long)served, (unsigned long long)learned, (unsigned long long)wrong, (unsigned long long)clears, (unsigned long long)T.first_hits, (unsigned long long)T.hits);
    // mutation: a table keyed by the key id alone (the variant ignored) hands out wrong pipelines - the variant is what makes it exact
    nrp_tab_clear(&T);
    uint64_t wrong2 = 0; nrp_var zero; memset(&zero, 0, sizeof zero);
    for (int it = 0; it < 200000; it++) {
        const tstate *s = &st[rn(NS)];
        const nrp_tent *e = nrp_tab_find(&T, s->key, &zero);
        if (e) { if (e->pipe != oracle(s)) wrong2++; }
        else { nrp_tent *n = nrp_tab_put(&T, s->key, &zero); if (n) n->pipe = oracle(s); }
    }
    CHECK(wrong2 > 1000);
    printf("  mutation (variant ignored): %llu wrong pipelines of 200 000 - the test sees it\n", (unsigned long long)wrong2);
    nrp_tab_free(&T);
}

// ---- the model: vkCmd* a draw ----
// A frame like the phone's campaign frame (logs 17.11: 1159 draws, 286 pipelines, set_binds 1004 = 0.87 a draw): materials come in runs,
// a run draws several meshes; meshes live in `chunks` VkBuffers (zero-copy AHB chunks: 21 on the Mi 14; NR-06 static copies: 16 MB chunks;
// one stream arena: 1). Offsets: each vertex buffer starts on `align` bytes (0: on a multiple of its stride - NR-06 copies placed for the
// folds, nrbuf::place), index data on 4; ichunks: 0 = index data anywhere in the vertex chunks, -1 = next to its vertex data, n > 0 = in
// n chunks of its own (nrbuf::place bit 0).
typedef struct { uint32_t vb_buf, ib_buf; uint64_t vb_off, ib_off; uint32_t stride; uint8_t isz; } mesh;
static void model(const char *name, unsigned chunks, unsigned align, int ichunks) {
    enum { NM = 400, ND = 1159, FRAMES = 20 };
    static mesh m[NM];
    static const uint32_t strides[] = {24, 28, 32, 32, 36, 40, 44, 48, 52, 56};
    uint64_t fill[160] = {0};
    for (int i = 0; i < NM; i++) {
        const unsigned c = rn(chunks);
        m[i].stride = strides[rn(10)]; m[i].isz = 1;
        const uint64_t vbytes = (uint64_t)m[i].stride * (200 + rn(4000)), ibytes = 2ull * (300 + rn(9000));
        const unsigned al = align ? align : m[i].stride;
        fill[c] = (fill[c] + al - 1) / al * al; m[i].vb_buf = c; m[i].vb_off = fill[c]; fill[c] += vbytes;
        const unsigned ci = ichunks < 0 ? c : ichunks ? 100 + rn((unsigned)ichunks) : rn(chunks);
        fill[ci] = (fill[ci] + 3) & ~3ull; m[i].ib_buf = ci; m[i].ib_off = fill[ci]; fill[ci] += ibytes;
    }
    nr_mcache c[4]; memset(c, 0, sizeof c);
    for (int f = 0; f < FRAMES; f++) {
        uint32_t pipe = 0, set = 0, cur = 0;
        for (int d = 0; d < ND; d++) {
            if (!rn(5) || !d) pipe = rn(286);            // a new material run
            if (rn(100) < 87) set = rn(1u << 20);          // the set / its offsets change on 0.87 of the draws
            if (rn(100) >= 15) cur = rn(NM);               // 15 %: the same mesh again (instances, sub-meshes)
            nr_mdraw x; memset(&x, 0, sizeof x);
            x.pipe = pipe; x.set = set; x.ib_buf = m[cur].ib_buf; x.ib_off = m[cur].ib_off + 2ull * 3 * rn(200); x.isz_log2 = m[cur].isz; x.nvb = 1;
            x.vb_buf[0] = m[cur].vb_buf; x.vb_off[0] = m[cur].vb_off; x.vb_stride[0] = m[cur].stride; x.base = 0;
            for (int k = 0; k < 4; k++) nr_model_draw(&c[k], &x, k);
        }
        for (int k = 0; k < 4; k++) c[k].have = 0;        // a new command buffer
    }
    printf("  %-52s old tail %.2f | index fold %.2f | vertex fold %.2f | both %.2f   (vertex binds %.2f -> %.2f, index binds %.2f -> %.2f a draw)\n", name,
           (double)c[0].cmds / c[0].draws, (double)c[1].cmds / c[1].draws, (double)c[2].cmds / c[2].draws, (double)c[3].cmds / c[3].draws,
           (double)c[0].c_vb / c[0].draws, (double)c[3].c_vb / c[3].draws, (double)c[0].c_ib / c[0].draws, (double)c[3].c_ib / c[3].draws);
    CHECK(c[3].cmds <= c[1].cmds && c[3].cmds <= c[2].cmds && c[1].cmds <= c[0].cmds && c[2].cmds <= c[0].cmds);   // a fold never adds a command
    CHECK(c[0].c_pipe == c[3].c_pipe && c[0].c_set == c[3].c_set);
}

int main(void) {
    test_tail();
    test_index_fold();
    test_vertex_fold();
    test_runs();
    test_var();
    test_table();
    printf("model, vkCmd* a draw of the per-draw tail (draw + pipeline + set + vertex + index binds; 1159 draws a frame, synthetic meshes):\n");
    model("zero-copy (Adreno), 21 chunks, vertex data on 4096:", 21, 4096, 0);
    model("zero-copy (Adreno), 21 chunks, vertex data on 16:", 21, 16, 0);
    model("NR-06 copies as they were, 8 chunks, on 256:", 8, 256, -1);
    model("NR-06 copies placed: 8 chunks on the stride, 1 index:", 8, 0, 1);
    model("NR-06 copies placed: 3 chunks on the stride, 1 index:", 3, 0, 1);
    model("NR-06 copies placed: 1 chunk on the stride, 1 index:", 1, 0, 1);
    model("one buffer a mesh (no sharing):", 64, 16, 0);
    for (const char *w = nr_draw_why(0); 0;) (void)w;
    for (int i = 0; i < NRW_N; i++) CHECK(nr_draw_why(i)[0] != '?');
    CHECK(nr_draw_why(NRW_N)[0] == '?');
    if (fails) { printf("FAIL: %d of %ld checks\n", fails, checks); return 1; }
    printf("PASS: %ld checks, 0 failures\n", checks);
    return 0;
}
