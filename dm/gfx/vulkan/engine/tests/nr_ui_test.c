// [DOS2] NR-12 host test (docs/dos2/nr/NR-12.md, DM_NR_UI): the pure decisions of the UI lane in dm/gfx/nr/nr_draw.h -
//   nr_ui_area     the uiarea step of a draw the short path records (all 32 inputs; for an engine draw it is NR-09's old condition);
//   quad pattern   the submission's one index list for Iggy's DrawArrays(QUADS) against the old path's per-draw expansion
//                  (xbg_quad_plan, linked as is) for every length, nr_quad_ok against single-index corruption, nr_quad_size;
//   row 0          the UI's synthetic key id in NR-07's direct table next to engine key ids: an oracle on a mixed draw stream (no wrong
//                  pipeline, engine rows keep their first-of-row hits), the "variant ignored" mutation caught in row 0 too;
//   the model      vkCmd* a UI draw (old tail vs the lane) on GDraw-shaped streams: QUADS draws with their vertices in the constant ring
//                  (UBO 0x80 + 0x30 on 256, vertices on 4 - GDraw 0x1bf0cd0 / 0x1bf12e8) and indexed shape draws in mesh buffers.
// Build (Windows LLVM or any C11 compiler), from the repository root:
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nr_ui_test.c dm/gfx/nr/nr_pipe.c dm/gfx/vulkan/engine/xbg_quad.c -o nr_ui_test && ./nr_ui_test
#include "../../../nr/nr_draw.h"
#include "../../../nr/nr_pipe.h"
#include "../xbg_quad.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
static long checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { if (++fails < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t rng = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint32_t rn(uint32_t n) { return (uint32_t)(rnd() % n); }

// ---- nr_ui_area ----
static void test_area(void) {
    int full = 0, shortp = 0;
    for (int m = 0; m < 32; m++) {
        const int ua = m & 1, cont = m >> 1 & 1, other = m >> 2 & 1, red = m >> 3 & 1, ui = m >> 4 & 1;
        const int r = nr_ui_area(ua, cont, other, red, ui);
        if (!ui) {   // an engine draw: exactly NR-09's lane (fail unless continues && OTHER && !reduced; the short bookkeeping when on)
            const int nr09_fail = ua && !(cont && other && !red);
            CHECK(nr09_fail ? r == NRUA_OLD : ua ? r == NRUA_SHORT : r == NRUA_NONE);
        }
        if (!ua) CHECK(r == NRUA_NONE);
        if (ua && (!cont || red)) CHECK(r == NRUA_OLD);      // a reduced pass may restart on a draw: never the lane
        if (r == NRUA_FULL) { CHECK(ui && ua && cont && !red && !other); full++; }
        if (r == NRUA_SHORT) { CHECK(ua && cont && !red && other); shortp++; }
    }
    CHECK(full == 1 && shortp == 2);
    CHECK(NRD_SITE_UI == 0xfe && NRD_ROW_UI == 0 && NRD_ROW_UI < NRP_TAB_ROWS);
}

// ---- the quad pattern against the old path's expansion ----
static void test_quads(void) {
    static uint32_t pat[NR_QUAD_MAX], old[NR_QUAD_MAX];
    nr_quad_pattern(pat, NR_QUAD_MAX);
    uint64_t lens = 0;
    for (int it = 0; it < 4000; it++) {
        const int32_t verts = it < 64 ? 4 * it : (int32_t)(4 * (1 + rn(it < 3000 ? 2048 : 262144))) + (rn(4) ? 0 : (int32_t)rn(4));   // a ragged tail too
        const int32_t first = (int32_t)rn(1 << 20), inst = 1 + (int32_t)rn(3), binst = (int32_t)rn(5);
        uint32_t n = 0;
        CHECK(xbg_quad_index_count(verts, &n) == 0);
        xbg_draw_call src = {7, -1, first, verts, inst, binst, 0, 0}, tri;
        memset(&tri, 0, sizeof tri);
        if (n > NR_QUAD_MAX) continue;
        CHECK(xbg_quad_plan(&src, &tri, old, NR_QUAD_MAX) == 0);
        CHECK(tri.primitive == 4 && tri.index_type == 2 && tri.count == (int32_t)n && tri.base_vertex == first && tri.instances == inst && tri.base_instance == binst);
        CHECK(n % 6 == 0);
        CHECK(!memcmp(old, pat, (size_t)n * 4));          // the same indices as the pattern's first n: the same vertices fetched
        CHECK(nr_quad_ok(old, n));
        if (n) { const uint32_t at = rn(n); old[at] ^= 1u + rn(7); CHECK(!nr_quad_ok(old, n)); }   // the comparison sees one wrong index
        const uint32_t s = nr_quad_size(0, n);
        CHECK(s >= n && s % 6 == 0 && s <= NR_QUAD_MAX && (s >= NR_QUAD_MIN || !n));
        lens += n;
    }
    CHECK(nr_quad_ok(pat, NR_QUAD_MAX) && !nr_quad_ok(pat, 5));
    // growth: one pattern a submission, grown by doubling; never shrinks; 0 beyond the cap
    uint32_t have = 0, made = 0;
    for (int it = 0; it < 100000; it++) {
        const uint32_t n = 6 * (1 + rn(it < 99000 ? 512 : 300000));
        const uint32_t s = nr_quad_size(have, n);
        if (n > NR_QUAD_MAX) { CHECK(s == 0); continue; }
        CHECK(s >= n && s >= have && s % 6 == 0);
        if (s != have) made++;
        have = s;
    }
    CHECK(made <= 10 && nr_quad_size(NR_QUAD_MAX, NR_QUAD_MAX + 6) == 0 && nr_quad_size(0, 6) == NR_QUAD_MIN);
    printf("  quads: 4000 draws (%llu indices) - the pattern equals xbg_quad_plan's list for every length; %u patterns for 100 000 draws of growing size\n",
           (unsigned long long)lens, made);
}

// ---- row 0 next to the engine's rows ----
typedef struct { uint32_t key; uint64_t vs, fs, lay, pd; unsigned prim, colors, depth; uint32_t gid[6]; } tstate;
static uint64_t oracle(const tstate *s) {
    uint64_t h = 0xcbf29ce484222325ull; const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < sizeof *s; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h | 1;
}
static void var_of(const tstate *s, nrp_var *pv) {
    nr_draw_var_t v;
    nr_draw_var(&v, s->vs, s->fs, s->lay, s->pd, s->prim, 1, 1, s->colors, s->depth, s->depth ? 0x34 : 0, 0, s->gid);
    memcpy(pv->w, v.w, sizeof pv->w);
}
static uint64_t rx(uint64_t *st) { *st ^= *st << 13; *st ^= *st >> 7; *st ^= *st << 17; return *st; }
static void run_stream(nrp_tab *T, const tstate *eng, int ne, const tstate *ui, int nu, int with_ui, uint64_t *wrong, uint64_t *served, uint64_t *eng_first, uint64_t *eng_hits) {
    *wrong = *served = *eng_first = *eng_hits = 0;
    uint64_t ge = 0x1234567ull, gu = 0x89abcdefull, ge_n = 0;   // ge_n: engine draws so far (400 000 in both runs)   // the engine stream is the same with and without the UI draws between its draws
    for (int it = 0; it < 460000; it++) {
        const int is_ui = with_ui ? rx(&gu) % 100 < 13 : 0;   // the campaign: ~150 Iggy draws of ~1150
        if (!with_ui && it >= 400000) break;
        const tstate *s;
        if (is_ui) { const uint64_t a = rx(&gu); s = &ui[a % 4 ? (a >> 8) % 4 : (a >> 8) % (uint64_t)nu]; }
        else { if (ge_n >= 400000) continue; ge_n++; const uint64_t a = rx(&ge); s = &eng[a % 8 ? (a >> 8) % 40 : (a >> 8) % (uint64_t)ne]; }
        nrp_var pv; var_of(s, &pv);
        const uint64_t fh = T->first_hits, h = T->hits;
        const nrp_tent *e = nrp_tab_find(T, s->key, &pv);
        if (e) { (*served)++; if (e->pipe != oracle(s)) (*wrong)++; if (!is_ui) { *eng_first += T->first_hits - fh; *eng_hits += T->hits - h; } }
        else { nrp_tent *n = nrp_tab_put(T, s->key, &pv); CHECK(n != NULL); if (n) n->pipe = oracle(s); }
    }
}
static void test_row0(void) {
    enum { NE = 600, NU = 24 };
    static tstate eng[NE], ui[NU];
    memset(eng, 0, sizeof eng); memset(ui, 0, sizeof ui);
    for (int i = 0; i < NE; i++) {
        tstate *s = &eng[i]; s->key = 1 + rn(150);
        s->vs = 0x70000000ull + 64 * rn(40); s->fs = 0x71000000ull + 64 * rn(40); s->lay = 0x72000000ull + 64 * rn(8); s->pd = 0x73000000ull + 64 * rn(6);
        s->prim = 4; s->colors = 1 + rn(7); s->depth = rn(2); for (int g = 0; g < 6; g++) s->gid[g] = rn(6);
    }
    for (int i = 0; i < NU; i++) {   // Iggy: 3 fragment shaders, a few vertex shaders (vertex formats), blend / stencil-mask states, DS attached or detached
        tstate *s = &ui[i]; s->key = NRD_ROW_UI;
        s->vs = 0x7a000000ull + 64 * rn(3); s->fs = 0x7b000000ull + 64 * rn(3); s->lay = 0x7c000000ull + 64 * rn(3); s->pd = 0x7d000000ull + 64 * rn(2);
        s->prim = 4; s->colors = 1; s->depth = rn(2); for (int g = 0; g < 6; g++) s->gid[g] = g < 2 ? rn(4) : 0;
    }
    nrp_tab T; CHECK(nrp_tab_init(&T) == 0);
    uint64_t w0, s0, f0, h0, w1, s1, f1, h1;
    run_stream(&T, eng, NE, ui, NU, 0, &w0, &s0, &f0, &h0);
    nrp_tab_clear(&T);
    run_stream(&T, eng, NE, ui, NU, 1, &w1, &s1, &f1, &h1);
    CHECK(w0 == 0 && w1 == 0);
    CHECK(f1 == f0 && h1 == h0);   // the UI's row 0 never touches an engine row: the engine draws hit exactly as without it
    printf("  row 0: 400 000 engine draws + ~13%% Iggy draws ( %d UI states in row 0; the engine on 150 key ids): served %llu, wrong pipeline %llu; engine first-of-row hits %llu of %llu, "
           "the same without the UI draws (%llu of %llu)\n", NU, (unsigned long long)s1, (unsigned long long)w1, (unsigned long long)f1, (unsigned long long)h1,
           (unsigned long long)f0, (unsigned long long)h0);
    // mutation: row 0 with the variant ignored hands out wrong pipelines - the variant is what makes one row exact
    nrp_tab_clear(&T);
    uint64_t wrong2 = 0; nrp_var zero; memset(&zero, 0, sizeof zero);
    for (int it = 0; it < 20000; it++) {
        const tstate *s = &ui[rn(NU)];
        const nrp_tent *e = nrp_tab_find(&T, NRD_ROW_UI, &zero);
        if (e) { if (e->pipe != oracle(s)) wrong2++; } else { nrp_tent *n = nrp_tab_put(&T, NRD_ROW_UI, &zero); if (n) n->pipe = oracle(s); }
    }
    CHECK(wrong2 > 1000);
    printf("  mutation (row 0, variant ignored): %llu wrong pipelines of 20 000 - the test sees it\n", (unsigned long long)wrong2);
    nrp_tab_free(&T);
}

// ---- the model: vkCmd* a UI draw ----
// GDraw 0x1bf0cd0: BindProgram every draw (the pipeline changes with the program / blend), 2 uniform blocks from the constant ring every
// draw (new dynamic offsets: the set is bound every draw on both paths). QUADS path (no index buffer): VS block 0x80 and FS block 0x30
// on 256 bytes, then the vertices on 4 bytes (4 x quads x stride) - all in the ring, one VkBuffer (zero-copy pool / NR-06's mirror);
// the old path copies the u32 index list into the stream arena per draw (a new offset: an index bind per draw), the lane binds the
// submission's pattern once. Indexed path: shapes in mesh buffers (`meshes` of them in `chunks` VkBuffers), a draw at an offset inside.
static void model_quads(const char *name, uint32_t stride, unsigned pipes) {
    nr_mcache old, lane; memset(&old, 0, sizeof old); memset(&lane, 0, sizeof lane);
    uint64_t head = 0, arena = 0;
    for (int d = 0; d < 20 * 150; d++) {
        if (!(d % 150)) { old.have = lane.have = 0; }   // a new command buffer a frame
        const uint32_t quads = 1 + (rn(4) ? rn(3) : rn(40));
        head = (head + 255) & ~255ull; head += 0x80; head = (head + 255) & ~255ull; head += 0x30;   // the two uniform blocks
        head = (head + 3) & ~3ull; const uint64_t voff = head; head += (uint64_t)4 * quads * stride;
        nr_mdraw x; memset(&x, 0, sizeof x);
        x.pipe = rn(100) < 70 ? 0 : 1 + rn(pipes); x.set = (uint32_t)d;   // the set's offsets move every draw
        x.nvb = 1; x.vb_buf[0] = 7; x.vb_off[0] = voff; x.vb_stride[0] = stride; x.base = 0; x.isz_log2 = 2;
        x.ib_buf = 9; x.ib_off = arena; arena += 24ull * quads;   // old: the expanded list, a new place each draw
        nr_model_draw(&old, &x, 0);
        x.ib_buf = 11; x.ib_off = 0;                                // the lane: the pattern
        nr_model_draw(&lane, &x, 3);
    }
    printf("  %-58s old tail %.2f | UI lane %.2f   (vertex binds %.2f -> %.2f, index binds %.2f -> %.2f a draw)\n", name,
           (double)old.cmds / old.draws, (double)lane.cmds / lane.draws, (double)old.c_vb / old.draws, (double)lane.c_vb / lane.draws,
           (double)old.c_ib / old.draws, (double)lane.c_ib / lane.draws);
    CHECK(lane.cmds <= old.cmds && lane.c_pipe == old.c_pipe && lane.c_set == old.c_set);
}
static void model_shapes(const char *name, unsigned chunks, uint32_t stride) {
    enum { NM = 120 };
    uint32_t vb[NM], ib[NM]; uint64_t vo[NM], io[NM]; uint64_t fill[64] = {0};
    for (int i = 0; i < NM; i++) {
        const unsigned c = rn(chunks); fill[c] = (fill[c] + 255) & ~255ull;   // a GDraw vertex buffer: vertices then indices in one buffer
        vb[i] = ib[i] = c; vo[i] = fill[c]; fill[c] += (uint64_t)stride * (8 + rn(2000)); fill[c] = (fill[c] + 3) & ~3ull; io[i] = fill[c]; fill[c] += 2ull * (12 + rn(6000));
    }
    nr_mcache old, lane; memset(&old, 0, sizeof old); memset(&lane, 0, sizeof lane);
    for (int d = 0; d < 20 * 150; d++) {
        if (!(d % 150)) { old.have = lane.have = 0; }
        const int m = (int)rn(NM);
        nr_mdraw x; memset(&x, 0, sizeof x);
        x.pipe = rn(100) < 70 ? 0 : 1 + rn(4); x.set = (uint32_t)d; x.nvb = 1; x.vb_buf[0] = vb[m]; x.vb_off[0] = vo[m] + (uint64_t)stride * 4 * rn(8);
        x.vb_stride[0] = stride; x.isz_log2 = 1; x.ib_buf = ib[m]; x.ib_off = io[m] + 2ull * 6 * rn(16);
        nr_model_draw(&old, &x, 0); nr_model_draw(&lane, &x, 3);
    }
    printf("  %-58s old tail %.2f | UI lane %.2f   (vertex binds %.2f -> %.2f, index binds %.2f -> %.2f a draw)\n", name,
           (double)old.cmds / old.draws, (double)lane.cmds / lane.draws, (double)old.c_vb / old.draws, (double)lane.c_vb / lane.draws,
           (double)old.c_ib / old.draws, (double)lane.c_ib / lane.draws);
    CHECK(lane.cmds <= old.cmds);
}

int main(void) {
    test_area();
    test_quads();
    test_row0();
    printf("model, vkCmd* a UI draw (draw + pipeline + set + vertex + index binds; 150 Iggy draws a frame, 20 frames):\n");
    model_quads("QUADS in the ring, stride 16 (v2tc2):", 16, 4);
    model_quads("QUADS in the ring, stride 8 (v2):", 8, 4);
    model_quads("QUADS in the ring, stride 20:", 20, 4);
    model_shapes("indexed shapes, zero-copy: 4 chunks, stride 16:", 4, 16);
    model_shapes("indexed shapes, stream copies (each its own buffer), stride 16:", 64, 16);
    if (fails) { printf("FAIL: %d of %ld checks\n", fails, checks); return 1; }
    printf("PASS: %ld checks, 0 failures\n", checks);
    return 0;
}
