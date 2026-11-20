// [DOS2] NR-NATIVE (DM_NRN_<PASS>, docs/dos2/nr/NR-NATIVE.md): host test of dm/gfx/nr/nr_nrn.h - the card of an engine draw (the
// renderer's own state cache in one word), its pass, the tail of the draw record and the table of the verify.
//   1 the tail      16 bytes after the NR-09 tail of an OP_X_NB_DRAW; a record without the switch is byte for byte the old one; the old
//                   readers (nbd_unpack / nbd_view / nrd_tail_get / nrd_tail_ext_put) are not disturbed; a cut record gives no card
//   2 the pass      names of the engine's targets (opcode 55) -> class -> pass of a draw; the handle map
//   3 equivalence   a model of the engine's objects (meshes, materials, passes) drives the renderer's bytes the way the engine's
//                   handlers do - with the volatile words (conditional render, dirty masks, first index) changing at every draw -
//                   and, independently, a model of the NVN path computes the Vulkan values of the same draw from the objects. The
//                   card must be the same exactly when the objects are the same (no spurious cards from the volatile words, no two
//                   object sets under one card), and the table of the verify must report: 0 bad, "same" exactly when nothing was
//                   placed elsewhere, "moved" exactly when a buffer or a set was, "view" when only the viewport changed
//   4 the journal   one scripted frame end to end: front end (card + tail into the record) -> the record's bytes -> back end (the tail
//                   read back, the lane's values noted) -> a text journal compared line by line with the expected one
//   5 mutations     built with -DNRN_MUTATE=k (k = 1..8) the test must FAIL: each k breaks one rule the parts above stand on
// Build: clang -std=c11 -O2 -Wall -Wextra nr_nrn_test.c -o nr_nrn_test        (mutations: add -DNRN_MUTATE=k, expect exit code 1)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../nvn/nvn_nb_draw.h"
#include "../../../nr/nr_nrn.h"

static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 12) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_seed = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---- the engine's objects ----
typedef struct { uint64_t vformat, vb[2], ib; unsigned streams, itype; } mesh;                      // itype: 0 = 16 bit, 1 = 32 bit
typedef struct { uint64_t shader, blend, depth, raster; uint32_t blend_arg, stencil; float alpha; int32_t tex[2][NRN_SLOTS], smp[2][NRN_SLOTS]; } material;
typedef struct { uint64_t color[8], ds; uint8_t prim; const char *c0_name, *ds_name; int want; } pass;
enum { MESHES = 40, MATS = 60, PASSES = 6 };
static mesh g_mesh[MESHES]; static material g_mat[MATS]; static pass g_pass[PASSES];
static uint64_t handle(unsigned kind, unsigned i) { return ((uint64_t)(1 + rn(1000)) << 32) | (kind << 20 | i); }   // index in the low word, a generation above

static void objects(void) {
    for (unsigned i = 0; i < MESHES; i++) {
        mesh *m = &g_mesh[i];
        m->vformat = handle(1, i % 7); m->vb[0] = handle(2, i); m->streams = 1 + i % 2; m->vb[1] = m->streams > 1 ? handle(2, 500 + i) : 0;
        m->ib = handle(3, i); m->itype = i % 3 == 0;
    }
    for (unsigned i = 0; i < MATS; i++) {
        material *m = &g_mat[i];
        m->shader = handle(4, i % 23); m->blend = handle(5, i % 3); m->depth = handle(6, i % 5); m->raster = handle(7, i % 2);
        m->blend_arg = i % 4; m->stencil = i % 3; m->alpha = i % 6 ? -1.f : 0.5f;
        memset(m->tex, 0xff, sizeof m->tex); memset(m->smp, 0xff, sizeof m->smp);
        for (unsigned s = 0; s < 1 + i % 5; s++) { m->tex[1][s] = (int32_t)(100 + (i * 7 + s * 13) % 300); m->smp[1][s] = (int32_t)((i + s) % 26); }
        if (i % 9 == 0) { m->tex[0][0] = (int32_t)(900 + i); m->smp[0][0] = 3; }
    }
    g_mat[MATS - 1] = g_mat[MATS - 2]; g_mat[MATS - 1].shader = handle(4, 22);          // two materials that differ in the shader only
    g_mat[MATS - 3] = g_mat[MATS - 4]; g_mat[MATS - 3].tex[1][0] ^= 0x400;              // ... in one texture slot only
    static const struct { const char *c0, *ds; int ncolor, want; } k[PASSES] = {
        {NULL, "ShadowRenderStage::DirectionalLightShadowDepth", 0, NRN_P_SHADOW}, {NULL, NULL, 0, NRN_P_PREPASS},
        {"DeferredBufferStage::GBuffer1", NULL, 4, NRN_P_GBUF}, {"DeferredRenderStage::HDRFullBuffer2", NULL, 1, NRN_P_LIGHT},
        {"PostProcessStage::LDRFullBuffer1", NULL, 1, NRN_P_POST}, {"SomeStage::Unknown", NULL, 1, NRN_P_OTHER}};
    for (unsigned i = 0; i < PASSES; i++) {
        pass *p = &g_pass[i]; memset(p, 0, sizeof *p);
        for (int c = 0; c < k[i].ncolor; c++) p->color[c] = handle(8, i * 8 + (unsigned)c);
        p->ds = i < 4 ? handle(9, i == 0 ? 0 : 1) : 0;
        p->prim = 4; p->c0_name = k[i].c0; p->ds_name = k[i].ds; p->want = k[i].want;
    }
}
// The renderer's bytes as the engine's handlers leave them for a draw of (mesh, material, pass). The volatile words get fresh noise.
static uint8_t R[0x1900];
static void w64(unsigned off, uint64_t v) { memcpy(R + off, &v, 8); }
static void engine_draw_state(const mesh *me, const material *ma, const pass *pa) {
    w64(0x16f0, ma->raster); w64(0x16f8, ma->blend); memcpy(R + 0x1700, &ma->blend_arg, 4); w64(0x1708, ma->depth); memcpy(R + 0x1710, &ma->stencil, 4);
    R[0x1714] = pa->prim; { uint32_t np = pa->prim; memcpy(R + 0x1718, &np, 4); }
    w64(0x1740, me->vformat); w64(NRN_R_VB, me->vb[0]); w64(NRN_R_VB + 8, me->vb[1]);
    w64(NRN_R_IB, me->ib); w64(NRN_R_SHADER, ma->shader); w64(NRN_R_SHADER_REC, 0x7100000000ull + (ma->shader & 0xfffff) * 0x80);
    memcpy(R + NRN_R_RT, pa->color, 64); w64(NRN_R_DEPTH_RT, pa->ds); memcpy(R + NRN_R_ALPHA, &ma->alpha, 4);
    memcpy(R + NRN_R_TEX_VS, ma->tex[0], 128); memcpy(R + NRN_R_TEX_FS, ma->tex[1], 128); memcpy(R + NRN_R_SMP_VS, ma->smp[0], 128); memcpy(R + NRN_R_SMP_FS, ma->smp[1], 128);
    for (unsigned o = NRN_R_VB_DIRTY; o < NRN_R_IB; o += 8) w64(o, rnd());           // dirty bookkeeping
    w64(NRN_R_IB_FIRST, rn(5000));                                                   // first index: a draw argument
    for (unsigned o = NRN_R_REN; o < NRN_R_ALPHA; o += 4) { uint32_t v = (uint32_t)rnd(); memcpy(R + o, &v, 4); }   // the query of this draw
    R[0x15dc] = (uint8_t)rn(32); R[0x1664] = (uint8_t)rn(32);                        // dirty ranges of the slots (between the tables)
}
// ---- the NVN path's answer for the same objects (independent of the bytes above) ----
static uint64_t g_place[4096];   // where the backend put a buffer (by handle index): bumped by a "moved elsewhere" event
static uint64_t g_set_gen;       // descriptor sets made again (the ring turned)
static uint64_t hh(uint64_t a, uint64_t b) { a ^= b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2); a *= 0xff51afd7ed558ccdull; return a ^ (a >> 33); }
static void nvn_path(const mesh *me, const material *ma, const pass *pa, uint64_t ibuf, uint32_t first, const uint8_t vp[24], nrn_vals *v) {
    uint64_t pipe = hh(ma->shader, ma->blend); pipe = hh(pipe, ma->depth); pipe = hh(pipe, ma->raster); pipe = hh(pipe, me->vformat); pipe = hh(pipe, pa->prim);
    pipe = hh(pipe, ma->blend_arg); pipe = hh(pipe, ma->alpha >= 0.f);
    for (int c = 0; c < 8; c++) pipe = hh(pipe, pa->color[c] ? 1 + (pa->color[c] & 7) : 0);
    pipe = hh(pipe, pa->ds != 0);
    uint64_t set = hh(ma->shader, g_set_gen);
    for (int st = 0; st < 2; st++) for (int s = 0; s < NRN_SLOTS; s++) { set = hh(set, (uint32_t)ma->tex[st][s]); set = hh(set, (uint32_t)ma->smp[st][s]); }
    uint64_t vb[16] = {0}, vbo[16] = {0};
    const unsigned used = me->streams > 1 ? 3u : 1u;
    for (unsigned i = 0; i < me->streams; i++) { vb[i] = hh(me->vb[i], g_place[(uint32_t)me->vb[i] & 4095]); vbo[i] = (uint32_t)me->vb[i] % 28; }
    const uint32_t isz = me->itype ? 4 : 2;
    nrn_vals_make(v, pipe, hh(ma->shader, 77), set, 2 + (unsigned)(ma->shader & 3), used, vb, vbo, (uint32_t)me->vformat % 5, 1, hh(me->ib, g_place[((uint32_t)me->ib + 2048) & 4095]),
                  (uint32_t)me->ib % isz, me->itype, ibuf, first, isz, vp, "0123456789abcdef");
}

// ---- 1: the tail ----
static void test_tail(void) {
    for (int it = 0; it < 4000; it++) {
        nbd_shadow s; memset(&s, 0, sizeof s);
        const unsigned nb = rn(9);
        for (unsigned i = 0; i < nb; i++) nbd_add(&s, rn(4), (int)rn(5), (int)rn(16), rnd(), rnd());
        s.h.engine_op = 36; s.h.draw = NBD_DRAW_IDX; s.h.count = (int32_t)rn(9000); s.h.ibuf = rnd();
        const int nr = rn(8) != 0, nrn = nr && rn(4) != 0;
        uint8_t old[4096], rec[4096]; memset(old, 0xcc, sizeof old); memset(rec, 0xcc, sizeof rec);
        if (nr) s.h.flags |= NBD_F_NR;
        const uint32_t size_old = nbd_size(&s) + (nr ? (uint32_t)sizeof(nrd_tail) : 0u);
        nbd_pack(&s, old, size_old);
        const uint32_t key = 1 + rn(5000), ts = rn(100), x = (uint32_t)rnd();
        if (nr) { nrd_tail_put(old, size_old, key, ts, 0, 3); nrd_tail_ext_put(old, size_old, x); }
        if (nrn) s.h.flags |= NBD_F_NRN;
        const uint32_t size = size_old + (nrn ? (uint32_t)NRN_TAIL_BYTES : 0u);
        nbd_pack(&s, rec, size);
        if (nr) { nrd_tail_put(rec, size, key, ts, 0, 3); nrd_tail_ext_put(rec, size, x); }
        const uint64_t card = rnd() | 1; const int pass_ = (int)rn(NRN_P_N); const uint32_t seq = (uint32_t)rnd();
        if (nrn) CHECK(nrn_tail_put(rec, size, nrd_tail_off(&s.h) + sizeof(nrd_tail), card, pass_, seq), "tail put");
        if (!nrn) CHECK(size == size_old && !memcmp(old, rec, sizeof old), "a record without the switch is the old record, byte for byte");
        else {   // the old part differs only in the flag bit
            old[offsetof(nbd_hdr, flags)] |= NBD_F_NRN;
            CHECK(!memcmp(old, rec, size_old), "the old bytes of the record are untouched by the new tail");
        }
        nbd_shadow back; nbd_hdr h; const uint8_t *binds; nrd_tail t; nrn_tail nt;
        CHECK(nbd_unpack(rec, size, &back) == 0 && back.h.nbind == nb && back.h.count == s.h.count, "nbd_unpack reads the longer record");
        CHECK(nbd_view(rec, size, &h, &binds) == 0 && binds == rec + sizeof(nbd_hdr), "nbd_view reads the longer record");
        CHECK(nrd_tail_get(rec, size, &h, &t) == nr && (!nr || (t.key == key && t.texset == ts && t._pad == x)), "the NR-09 tail and its spare word are where they were");
        const int got = nrn_tail_get(rec, size, nrd_tail_off(&h) + sizeof(nrd_tail), h.flags, &nt);
        CHECK(got == nrn, "the card is read exactly from the records that carry it");
        if (got) CHECK(nt.card == card && nt.pass == pass_ && nt.seq == seq, "card / pass / seq read back");
        if (nrn) CHECK(!nrn_tail_get(rec, size - 1, nrd_tail_off(&h) + sizeof(nrd_tail), h.flags, &nt), "a cut record gives no card");
        if (nrn) { rec[nrd_tail_off(&h) + sizeof(nrd_tail) + offsetof(nrn_tail, ver)] = 9; CHECK(!nrn_tail_get(rec, size, nrd_tail_off(&h) + sizeof(nrd_tail), h.flags, &nt), "another version gives no card"); }
    }
}
// ---- 2: names and passes ----
static void test_pass(void) {
    static const struct { const char *n; int c; } k[] = {
        {"ShadowRenderStage::DirectionalLightShadowDepth", NRN_RC_SHADOW}, {"DeferredRenderStage::LocalLightShadowDepth", NRN_RC_SHADOW},
        {"DeferredRenderStage::LocalLightShadowBufferCube", NRN_RC_SHADOW}, {"DeferredRenderStage::LocalLightShadowBuffer", NRN_RC_SHADOW},
        {"DeferredBufferStage::GBuffer1", NRN_RC_GBUF}, {"SSAOStage::GBuffer2", NRN_RC_GBUF}, {"DeferredBufferStage::GBuffer3", NRN_RC_GBUF},
        {"DeferredBufferStage::HDRFullBuffer1", NRN_RC_HDR}, {"PostProcessStep_Color::HDRFullBuffer2", NRN_RC_HDR},
        {"PostProcessStage::LDRFullBuffer1", NRN_RC_LDR}, {"PostProcessStep_Color::LDRFullBuffer2", NRN_RC_LDR},
        {"ShadowRenderStage::ShadowResult", NRN_RC_AUX}, {"SSAOStage::m_RT_DeinterleavedDepth1", NRN_RC_AUX}, {"SSAOStage::m_RT_SAOResult2", NRN_RC_AUX},
        {"Whatever::Else", NRN_RC_NAMED}, {"", NRN_RC_NONE}, {NULL, NRN_RC_NONE}};
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) CHECK(nrn_name_class(k[i].n) == k[i].c, "class of '%s'", k[i].n ? k[i].n : "(null)");
    CHECK(nrn_pass_of(0, NRN_RC_NONE, NRN_RC_SHADOW) == NRN_P_SHADOW && nrn_pass_of(0, NRN_RC_NONE, NRN_RC_NONE) == NRN_P_PREPASS && nrn_pass_of(0, NRN_RC_NONE, NRN_RC_NAMED) == NRN_P_PREPASS, "depth-only draws");
    CHECK(nrn_pass_of(1, NRN_RC_GBUF, NRN_RC_NONE) == NRN_P_GBUF && nrn_pass_of(1, NRN_RC_HDR, NRN_RC_SHADOW) == NRN_P_LIGHT && nrn_pass_of(1, NRN_RC_LDR, 0) == NRN_P_POST &&
          nrn_pass_of(1, NRN_RC_AUX, 0) == NRN_P_POST && nrn_pass_of(1, NRN_RC_SHADOW, 0) == NRN_P_SHADOW && nrn_pass_of(1, NRN_RC_NONE, 0) == NRN_P_OTHER, "colour draws");
    static nrn_rtmap m; memset(&m, 0, sizeof m);
    const uint64_t a = 0x0000002a00000007ull, b = 0x0000002b00000007ull /* the same index, a later generation */, c = 0x0000000100000407ull /* the same slot */;
    nrn_rt_put(&m, a, NRN_RC_GBUF);
    CHECK(nrn_rt_get(&m, a) == NRN_RC_GBUF && nrn_rt_get(&m, b) == NRN_RC_NONE && nrn_rt_get(&m, c) == NRN_RC_NONE, "the map answers for the very handle only");
    nrn_rt_put(&m, c, NRN_RC_HDR);
    CHECK(nrn_rt_get(&m, c) == NRN_RC_HDR && nrn_rt_get(&m, a) == NRN_RC_NONE, "a handle pushed out of its slot is unknown, not wrong");
    CHECK(nrn_rt_get(&m, 0) == NRN_RC_NONE && nrn_rt_get(&m, ~0ull) == NRN_RC_NONE && nrn_h_none(0xffffffffull), "no handle, no class");
}
static const char *(*g_envp)[2];
static const char *env_get(const char *k) { for (int i = 0; g_envp[i][0]; i++) if (!strcmp(g_envp[i][0], k)) return g_envp[i][1]; return NULL; }
static void test_env(void) {
    static const char *e0[][2] = {{NULL, NULL}}, *e1[][2] = {{"DM_NRN", "2"}, {"DM_NRN_GBUF", "0"}, {"DM_NRN_POST", "7"}, {NULL, NULL}},
                      *e2[][2] = {{"DM_NRN_SHADOW", "2"}, {"DM_NRN_LIGHT", "1"}, {NULL, NULL}};
    uint8_t m[NRN_P_N]; unsigned bad;
    g_envp = e0; CHECK(nrn_env_modes(env_get, 0, m, &bad) == 0 && !bad && !m[0] && !m[5], "nothing set: every pass 0");
    g_envp = e1; CHECK(nrn_env_modes(env_get, 0, m, &bad) && m[NRN_P_SHADOW] == 2 && m[NRN_P_GBUF] == 0 && m[NRN_P_POST] == 0 && m[NRN_P_OTHER] == 2 && bad == 1u << NRN_P_POST,
                       "DM_NRN sets every pass, a pass switch wins, a wrong value is 0 and reported");
    g_envp = e2; CHECK(nrn_env_modes(env_get, 0, m, &bad) && m[NRN_P_SHADOW] == 2 && m[NRN_P_LIGHT] == 1 && m[NRN_P_GBUF] == 0 && !bad, "pass switches alone");
}

// ---- 3: equivalence ----
typedef struct { uint64_t card; unsigned mesh, mat, pass; } seen;
static void test_equiv(void) {
    enum { DRAWS = 60000, COMBOS = 700 };
    static struct { unsigned mesh, mat, pass; } combo[COMBOS];
    for (unsigned i = 0; i < COMBOS; i++) { combo[i].mesh = rn(MESHES); combo[i].mat = rn(MATS); combo[i].pass = rn(PASSES); }
    for (unsigned i = 0; i < 4; i++) { combo[i].mesh = 0; combo[i].mat = MATS - 1 - i; combo[i].pass = 2; }   // the near-twins meet under one mesh and pass
    static seen sn[COMBOS]; unsigned nseen = 0;
    static nrn_rtmap rt; memset(&rt, 0, sizeof rt);
    for (unsigned i = 0; i < PASSES; i++) { nrn_rt_put(&rt, g_pass[i].color[0], nrn_name_class(g_pass[i].c0_name)); nrn_rt_put(&rt, g_pass[i].ds, nrn_name_class(g_pass[i].ds_name)); }
    nrn_tab T; CHECK(nrn_tab_init(&T, 4096) == 0, "table");
    nrn_pstat st[NRN_P_N]; memset(st, 0, sizeof st);
    static struct { nrn_vals v; int have; } last[COMBOS];
    uint8_t vp[24]; memset(vp, 1, sizeof vp);
    unsigned long long want_same = 0, want_moved = 0, want_view = 0, want_first = 0, got_bad = 0;
    for (int d = 0; d < DRAWS; d++) {
        if (rn(900) == 0) g_place[rn(4096)]++;          // the backend places a buffer elsewhere (budget trim, a view made again)
        if (rn(7000) == 0) g_set_gen++;                 // the sets are made again
        if (rn(9000) == 0) vp[0] ^= 1;                  // the viewport of the passes changes (resolution)
        const unsigned ci = rn(COMBOS);
        const mesh *me = &g_mesh[combo[ci].mesh]; const material *ma = &g_mat[combo[ci].mat]; const pass *pa = &g_pass[combo[ci].pass];
        engine_draw_state(me, ma, pa);
        const uint64_t card = nrn_card_of(R, NRN_W_DEFAULT);
        const int p = nrn_pass_of_r(R, &rt);
        CHECK(p == pa->want, "pass of a draw: %s, want %s", nrn_pass_name(p), nrn_pass_name(pa->want));
        // the same objects <=> the same card
        unsigned k;
        for (k = 0; k < nseen; k++) if (sn[k].mesh == combo[ci].mesh && sn[k].mat == combo[ci].mat && sn[k].pass == combo[ci].pass) break;
        if (k < nseen) CHECK(sn[k].card == card, "the same objects gave another card (a volatile word is read)");
        else {
            for (unsigned j = 0; j < nseen; j++) {
                const mesh *m2 = &g_mesh[sn[j].mesh]; const material *a2 = &g_mat[sn[j].mat]; const pass *p2 = &g_pass[sn[j].pass];
                const int same_objects = !memcmp(m2, me, sizeof *me) && !memcmp(a2, ma, sizeof *ma) && !memcmp(p2->color, pa->color, 64) && p2->ds == pa->ds && p2->prim == pa->prim;
                if (!same_objects) CHECK(sn[j].card != card, "two object sets under one card (mesh %u/%u material %u/%u pass %u/%u)", sn[j].mesh, combo[ci].mesh, sn[j].mat, combo[ci].mat, sn[j].pass, combo[ci].pass);
            }
            sn[nseen].card = card; sn[nseen].mesh = combo[ci].mesh; sn[nseen].mat = combo[ci].mat; sn[nseen].pass = combo[ci].pass; nseen++;
        }
        // what the NVN path records for it, noted under the card
        const uint32_t first = rn(3000); const uint64_t ibuf = 0x8000000000ull + (me->ib & 0xfffff) * 0x10000 + (uint64_t)first * (me->itype ? 4 : 2) + (uint32_t)me->ib % (me->itype ? 4 : 2);
        nrn_vals v; nvn_path(me, ma, pa, ibuf, first, vp, &v);
        unsigned diff = 0;
        const int r = nrn_tab_note(&T, card, p, &v, &diff);
        nrn_pstat_note(&st[p], r, diff);
        // the expectation, from the model's own memory of the combination (cards that share objects share the expectation: use the card's owner)
        unsigned own = k < nseen ? k : nseen - 1;
        for (unsigned j = 0; j < nseen; j++) if (sn[j].card == card) { own = j; break; }
        if (!last[own].have) { want_first++; CHECK(r == 0, "first recording of a card"); }
        else {
            const unsigned wd = nrn_vals_diff(&last[own].v, &v);
            CHECK(!(wd & NRN_D_BAD), "the model itself changed a function of the objects");
            if (!wd) { want_same++; CHECK(r == 1, "the same values: same"); }
            else { CHECK(r == 2 && diff == wd, "differences %#x, want %#x", diff, wd); if (wd & NRN_D_MOVED) want_moved++; else want_view++; }
        }
        last[own].v = v; last[own].have = 1;
        if (diff & (NRN_D_BAD | NRN_D_PASS)) got_bad++;
    }
    unsigned long long same = 0, moved = 0, view = 0, first = 0, bad = 0;
    for (int p = 0; p < NRN_P_N; p++) { same += st[p].same; moved += st[p].moved_only; view += st[p].view_only; first += st[p].first; bad += st[p].bad; }
    CHECK(bad == 0 && got_bad == 0, "bad draws in a correct model: %llu", bad);
    CHECK(same == want_same && moved == want_moved && view == want_view && first == want_first, "counters: same %llu/%llu moved %llu/%llu view %llu/%llu first %llu/%llu", same, want_same, moved,
          want_moved, view, want_view, first, want_first);
    CHECK(want_moved > 50 && want_view > 50 && want_same > 40000, "the model exercised every class (same %llu, moved %llu, view %llu)", want_same, want_moved, want_view);
    // a card that does NOT describe its draw is reported: the same card, another pipeline
    engine_draw_state(&g_mesh[1], &g_mat[1], &g_pass[2]);
    { nrn_vals a, b; nvn_path(&g_mesh[1], &g_mat[1], &g_pass[2], 0x1000, 0, vp, &a); b = a; b.pipe ^= 0x10; unsigned d1, d2;
      const uint64_t c = nrn_card_of(R, NRN_W_DEFAULT) ^ 0x5555000000000000ull;
      nrn_tab_note(&T, c, NRN_P_GBUF, &a, &d1); const int r = nrn_tab_note(&T, c, NRN_P_GBUF, &b, &d2);
      CHECK(r == 2 && (d2 & NRN_D_PIPE), "another pipeline under one card is a difference of class bad");
      unsigned d3; CHECK(nrn_tab_note(&T, c, NRN_P_GBUF, &b, &d3) == 1, "the entry holds the latest recording");
      unsigned d4; CHECK(nrn_tab_note(&T, c, NRN_P_LIGHT, &b, &d4) == 2 && (d4 & NRN_D_PASS), "one card in two passes is reported"); }
    // sequence numbers
    { nrn_pstat s; memset(&s, 0, sizeof s); uint32_t l = 0; nrn_seq_note(&s, &l, 5); nrn_seq_note(&s, &l, 6); nrn_seq_note(&s, &l, 9); nrn_seq_note(&s, &l, 10);
      CHECK(s.seq_gaps == 1, "one gap in 5 6 9 10, got %llu", (unsigned long long)s.seq_gaps); }
    // the table starts again when full
    { nrn_tab t2; nrn_tab_init(&t2, 64); nrn_vals z; memset(&z, 0, sizeof z); unsigned dd; for (uint64_t c = 1; c <= 200; c++) nrn_tab_note(&t2, c * 0x9e3779b97f4a7c15ull | 1, 0, &z, &dd);
      CHECK(t2.clears >= 3 && t2.n < 64, "a full table is cleared (%llu clears)", (unsigned long long)t2.clears); nrn_tab_free(&t2); }
    nrn_tab_free(&T);
}

// ---- 4: the journal of one frame, end to end ----
static void test_journal(void) {
    static const struct { int mesh, mat, pass, event; } script[] = {   // event: 1 = the mesh's vertex buffer is placed elsewhere before the draw, 2 = the viewport changes
        {3, 4, 0, 0}, {5, 4, 0, 0}, {3, 4, 0, 0}, {3, 4, 1, 0}, {3, 4, 2, 0}, {3, 4, 2, 0}, {3, 4, 2, 1}, {3, 4, 2, 0}, {7, 9, 3, 0}, {7, 9, 3, 2}, {7, 9, 4, 0}, {3, 4, 0, 0}};
    static const char *want[] = {"SHADOW seq 1 first", "SHADOW seq 2 first", "SHADOW seq 3 same", "PREPASS seq 1 first", "GBUF seq 1 first", "GBUF seq 2 same",
                                 "GBUF seq 3 moved: vertex buffer", "GBUF seq 4 same", "LIGHT seq 1 first", "LIGHT seq 2 view: viewport", "POST seq 1 first", "SHADOW seq 4 moved: vertex buffer viewport"};
    static nrn_rtmap rt; memset(&rt, 0, sizeof rt);
    for (unsigned i = 0; i < PASSES; i++) { nrn_rt_put(&rt, g_pass[i].color[0], nrn_name_class(g_pass[i].c0_name)); nrn_rt_put(&rt, g_pass[i].ds, nrn_name_class(g_pass[i].ds_name)); }
    nrn_tab T; nrn_tab_init(&T, 1024);
    uint32_t seq[NRN_P_N] = {0}, last[NRN_P_N] = {0}; nrn_pstat st[NRN_P_N]; memset(st, 0, sizeof st);
    uint8_t vp[24]; memset(vp, 2, sizeof vp);
    for (unsigned i = 0; i < sizeof script / sizeof script[0]; i++) {
        const mesh *me = &g_mesh[script[i].mesh]; const material *ma = &g_mat[script[i].mat]; const pass *pa = &g_pass[script[i].pass];
        if (script[i].event == 1) g_place[(uint32_t)me->vb[0] & 4095]++;
        if (script[i].event == 2) vp[3] ^= 0x40;
        // front end: the engine set its state, the draw record is written with the NR-09 tail and the card
        engine_draw_state(me, ma, pa);
        const int p = nrn_pass_of_r(R, &rt);
        nbd_shadow s; memset(&s, 0, sizeof s); s.h.engine_op = 37; s.h.draw = NBD_DRAW_IDX; s.h.count = 300; s.h.ibuf = 0x9000000000ull + (uint64_t)i * 0 + (me->ib & 0xfffff) * 0x10000;
        nbd_add(&s, NBD_B_UBO, 0, 1, 0x1000 + i * 256, 256);
        s.h.flags |= NBD_F_NR | NBD_F_NRN;
        uint8_t rec[512]; const uint32_t size = nbd_size(&s) + (uint32_t)sizeof(nrd_tail) + (uint32_t)NRN_TAIL_BYTES;
        nbd_pack(&s, rec, size); nrd_tail_put(rec, size, 40 + (uint32_t)script[i].mat, 0, 0, 1);
        nrn_tail_put(rec, size, nrd_tail_off(&s.h) + sizeof(nrd_tail), nrn_card_of(R, NRN_W_DEFAULT), p, ++seq[p]);
        // back end: the record is read, the path records the draw, the values are noted under the card
        nbd_hdr h; const uint8_t *binds; nrn_tail nt; char line[96];
        const int got = nbd_view(rec, size, &h, &binds) == 0 && nrn_tail_get(rec, size, nrd_tail_off(&h) + sizeof(nrd_tail), h.flags, &nt);
        CHECK(got && nt.seq == seq[p] && nt.pass == p, "journal: the record carries its card");
        if (!got || nt.pass != p) continue;
        nrn_seq_note(&st[nt.pass], &last[nt.pass], nt.seq);
        nrn_vals v; nvn_path(me, ma, pa, h.ibuf, 0, vp, &v);
        unsigned d = 0; const int r = nrn_tab_note(&T, nt.card, nt.pass, &v, &d);
        nrn_pstat_note(&st[nt.pass], r, d);
        int n = snprintf(line, sizeof line, "%s seq %u %s", nrn_pass_name(nt.pass), nt.seq, r == 0 ? "first" : r == 1 ? "same" : d & NRN_D_BAD ? "BAD" : d & NRN_D_MOVED ? "moved" : "view");
        if (r == 2) { n += snprintf(line + n, sizeof line - (size_t)n, ":"); for (int b = 0; b < NRN_D_BITS; b++) if (d >> b & 1u) n += snprintf(line + n, sizeof line - (size_t)n, " %s", nrn_diff_name(b)); }
        CHECK(!strcmp(line, want[i]), "journal line %u: '%s', want '%s'", i, line, want[i]);
    }
    unsigned long long gaps = 0, bad = 0; for (int p = 0; p < NRN_P_N; p++) { gaps += st[p].seq_gaps; bad += st[p].bad; }
    CHECK(!gaps && !bad, "journal: no gaps, nothing bad");
    nrn_tab_free(&T);
}

int main(void) {
    objects();
    test_tail(); test_pass(); test_env(); test_equiv(); test_journal();
    printf("nr_nrn_test%s: %llu checks, %llu failed: %s\n", NRN_MUTATE ? " (mutation)" : "", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
