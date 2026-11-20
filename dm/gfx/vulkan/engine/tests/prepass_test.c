// [DOS2] DM_DOS2_PREPASS host test (dm/gfx/vulkan/engine/xbg_prepass.h, NR-30 docs/dos2/nr/NR-30.md): pass / draw classification, the
// skip + G-buffer claim accounting, the history gate over frames, the fallback, and depth + colour models of a scene drawn as the game
// does (pre-pass LESS + write, G-buffer with its own funcs) and as mode 1 does.
//   test_model   a fixed scene (opaque boxes, an alpha-tested cut-out, a fading object, a terrain base + layer on one mesh, a barrel mesh
//                twice, a depth-only occluder): with the gate, every frame equals the game's pixel for pixel (depth and colour); without
//                the gate the occluder's depth is lost and the frame reports it as bad.
//   test_random  random scenes over several frames whose objects change role between frames (a fade begins, an object leaves the
//                G-buffer, a layer appears, a draw starts to blend / kill / test LESS / GREATER, raster mismatches, shared meshes, random draw
//                orders). Property: a frame whose tables report no bad key is identical to the game's (depth + colour of every pixel),
//                i.e. mode 2 / the fallback see every frame mode 1 would get wrong.
// Build + run (from this directory):  clang -std=c11 -O2 -Wall -Wextra prepass_test.c -o prepass_test && ./prepass_test
#include "../xbg_prepass.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
static long checks;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static prepass_ctx C;
static prepass_pre_in PI(uint64_t key, int why) { prepass_pre_in p = {key, 0x1000, 7, 30, why}; return p; }
static prepass_gb_in GI(uint64_t key, int df) { prepass_gb_in g = {key, 0x1000, 7, 1, 1, df, 0, 0, 0, 0, 0}; return g; }

static void test_classify(void) {
    CHECK(prepass_is_depth_only(0, 1, 2160, 972));
    CHECK(!prepass_is_depth_only(1, 1, 2160, 972));       // G-buffer
    CHECK(!prepass_is_depth_only(0, 1, 1536, 1536));      // shadow atlas
    CHECK(!prepass_is_depth_only(0, 1, 80, 100));         // portrait mini pipeline
    CHECK(!prepass_is_depth_only(0, 0, 2160, 972));
    CHECK(prepass_classify(1, 1, 1, 0, 8, 0, 1, 0) == -1);
    CHECK(prepass_classify(0, 1, 1, 0, 8, 0, 1, 0) == -1);   // no fragment stage
    CHECK(prepass_classify(2, 1, 1, 0, 8, 0, 1, 0) == PREPASS_K_KILL);
    CHECK(prepass_classify(3, 1, 1, 0, 8, 0, 1, 0) == PREPASS_K_FS);
    CHECK(prepass_classify(1, 1, 0, 0, 8, 0, 1, 0) == PREPASS_K_STATE);
    CHECK(prepass_classify(1, 0, 1, 0, 8, 0, 1, 0) == PREPASS_K_STATE);
    CHECK(prepass_classify(1, 1, 1, 0, 8, 0, 1, 1) == PREPASS_K_STATE);
    CHECK(prepass_classify(1, 1, 1, 1, 8, 0, 1, 0) == PREPASS_K_STENCIL);
    CHECK(prepass_classify(1, 1, 1, 0, 5, 0, 1, 0) == PREPASS_K_ALPHA);
    CHECK(prepass_classify(1, 1, 1, 0, 8, 1, 1, 0) == PREPASS_K_COUNTING);
    CHECK(prepass_classify(1, 1, 1, 0, 8, 0, 0, 0) == PREPASS_K_NOTINDEXED);
    CHECK(prepass_clamp_mode(-1) == 0 && prepass_clamp_mode(4) == 0 && prepass_clamp_mode(2) == PREPASS_COUNT && prepass_clamp_mode(3) == PREPASS_POISON);
    CHECK(prepass_blend_is_replace(2, 1, 2, 1, 1, 1) && !prepass_blend_is_replace(5, 6, 2, 1, 1, 1));
    CHECK(prepass_key(0x1000, 0, 36, 0, 1, 0) != prepass_key(0x1000, 36, 36, 0, 1, 0));
    CHECK(prepass_key(0x1000, 0, 36, 0, 1, 0) != prepass_key(0x1000, 0, 36, 0, 2, 0));
    CHECK(prepass_key(0, 0, 0, 0, 0, 0) != 0);
}

static void test_accounting(void) {   // gate off (DM_DOS2_PREPASS_HIST=0): the frame's own rules only
    prepass_ctx_init(&C, 0); prepass_frame_begin(&C, 7);
    uint64_t a = prepass_key(0x100, 0, 30, 0, 1, 0), b = prepass_key(0x200, 0, 30, 0, 1, 0), z = prepass_key(0x50, 0, 3, 0, 1, 0);
    prepass_pre_in p;
    p = PI(z, -1); CHECK(prepass_note_pre(&C, 5, &p) == 0 && C.f.keep[PREPASS_K_FIRST] == 1);   // the first draw begins the pass: drawn
    p = PI(a, -1); CHECK(prepass_note_pre(&C, 5, &p) == 1);
    p = PI(a, -1); CHECK(prepass_note_pre(&C, 5, &p) == 1);            // the same mesh twice (two barrels)
    p = PI(b, PREPASS_K_KILL); CHECK(prepass_note_pre(&C, 5, &p) == 0);
    p = PI(b, -1); CHECK(prepass_note_pre(&C, 5, &p) == 0 && C.f.keep[PREPASS_K_MIXED] == 1);   // b already drawn this frame: not skipped
    p = PI(0, -1); CHECK(prepass_note_pre(&C, 5, &p) == 0);            // key 0 = not indexed: kept
    CHECK(C.f.pre_draws == 6 && C.f.pre_skip == 2 && C.f.keep[PREPASS_K_KILL] == 1 && C.f.keep[PREPASS_K_FULL] == 1);
    prepass_gb_in g;
    g = GI(a, PREPASS_NVN_EQUAL); CHECK(prepass_note_gbuffer(&C, 9, &g) == 0);   // other depth
    g = GI(a, PREPASS_NVN_EQUAL); CHECK(prepass_note_gbuffer(&C, 5, &g) == 1);
    g = GI(a, PREPASS_NVN_LEQUAL); CHECK(prepass_note_gbuffer(&C, 5, &g) == 1);
    g = GI(b, PREPASS_NVN_EQUAL); CHECK(prepass_note_gbuffer(&C, 5, &g) == 0);   // kept pre-pass draw: EQUAL stays
    g = GI(z, PREPASS_NVN_EQUAL); CHECK(prepass_note_gbuffer(&C, 5, &g) == 0);   // the first (kept) draw's G-buffer draw
    p = PI(prepass_key(0x300, 0, 3, 0, 1, 0), -1); CHECK(prepass_note_pre(&C, 5, &p) == 0 && C.f.keep[PREPASS_K_LATE] == 1);   // after the G-buffer began
    CHECK(prepass_frame_begin(&C, 8) == 1 && C.last.bad_keys == 0 && C.last.unmatched == 0 && C.prev_frame_bad == 0);
    // refused (LESS) / blending / killing / other raster: no override, the key is bad
    const int dfs[4] = {2, PREPASS_NVN_EQUAL, PREPASS_NVN_EQUAL, PREPASS_NVN_EQUAL};
    for (int k = 0; k < 4; k++) {
        prepass_ctx_init(&C, 0); prepass_frame_begin(&C, 1);
        p = PI(z, -1); prepass_note_pre(&C, 5, &p);
        p = PI(a, -1); CHECK(prepass_note_pre(&C, 5, &p) == 1);
        g = GI(a, dfs[k]); if (k == 1) g.blend = 1; if (k == 2) g.kill = 1; if (k == 3) g.raster = 8;
        CHECK(prepass_note_gbuffer(&C, 5, &g) == 0 && C.f.gb_refused[k] == 1);
        prepass_frame_begin(&C, 2); CHECK(C.last.bad_keys == 1 && C.last.refused == 1 && C.last.unmatched == 1 && C.last.ok_keys == 0);
    }
    // an override the host then did not record (prepass_note_lost): bad now, not a candidate next frame
    prepass_ctx_init(&C, 0); prepass_frame_begin(&C, 1);
    p = PI(z, -1); prepass_note_pre(&C, 5, &p); p = PI(a, -1); CHECK(prepass_note_pre(&C, 5, &p) == 1);
    g = GI(a, PREPASS_NVN_EQUAL); CHECK(prepass_note_gbuffer(&C, 5, &g) == 1); prepass_note_lost(&C, a);
    prepass_frame_begin(&C, 2); CHECK(C.last.bad_keys == 1 && C.last.refused == 1 && C.last.ok_keys == 0 && C.last.gb_refused[PREPASS_R_LOST] == 1);
    // more claimers than candidates (a mesh drawn twice in the G-buffer, once in the pre-pass): not a clean key for the next frame
    prepass_ctx_init(&C, 1); prepass_frame_begin(&C, 1);
    p = PI(z, -1); prepass_note_pre(&C, 5, &p); p = PI(a, -1); CHECK(prepass_note_pre(&C, 5, &p) == 0);   // gate: no history yet
    g = GI(a, PREPASS_NVN_EQUAL); prepass_note_gbuffer(&C, 5, &g); prepass_note_gbuffer(&C, 5, &g);
    prepass_frame_begin(&C, 2); CHECK(C.last.ok_keys == 0);
    // a full table keeps the draw (never a skip without a key to claim)
    prepass_ctx_init(&C, 0); prepass_frame_begin(&C, 9); int kept_full = 0;
    for (uint32_t i = 0; i <= PREPASS_CAP; i++) { p = PI(prepass_key(0x1000 + 64ull * i, 0, 3, 0, 1, 0), -1); if (!prepass_note_pre(&C, 5, &p)) kept_full++; }
    CHECK(kept_full == (int)(1 + PREPASS_CAP + 1 - PREPASS_CAP * 3u / 4u));   // gate off: every keyed one skipped but the first
    CHECK(C.f.keep[PREPASS_K_FULL] == PREPASS_CAP + 1 - PREPASS_CAP * 3u / 4u && C.f.keep[PREPASS_K_FIRST] == 1);
}

static void test_gate(void) {
    prepass_ctx_init(&C, 1);
    const uint64_t z = prepass_key(0x50, 0, 3, 0, 1, 0), a = prepass_key(0x100, 0, 30, 0, 1, 0), occ = prepass_key(0x900, 0, 12, 0, 1, 0),
                   s = prepass_key(0x700, 0, 30, 0, 1, 0);
    int skipped[7] = {0}, bad[7] = {0};
    for (int fr = 1; fr <= 6; fr++) {
        prepass_frame_begin(&C, (uint64_t)fr);
        if (fr > 1) bad[fr - 1] = (int)C.prev_frame_bad;
        prepass_pre_in p = PI(z, -1); prepass_note_pre(&C, 5, &p);
        p = PI(a, -1); skipped[fr] += prepass_note_pre(&C, 5, &p);
        p = PI(occ, -1); skipped[fr] += 10 * prepass_note_pre(&C, 5, &p);   // a depth-only occluder: no G-buffer draw
        p = PI(s, -1); skipped[fr] += 100 * prepass_note_pre(&C, 5, &p);
        prepass_gb_in g;
        if (fr == 4) { g = GI(z, PREPASS_NVN_EQUAL); g.st = 1; prepass_note_gbuffer(&C, 5, &g); }   // frame 4: a stencil draw (z's) before a's claim
        g = GI(a, PREPASS_NVN_EQUAL); prepass_note_gbuffer(&C, 5, &g);
        if (fr != 3) { g = GI(s, PREPASS_NVN_EQUAL); prepass_note_gbuffer(&C, 5, &g); }   // frame 3: s leaves the G-buffer
        if (fr == 2) { g = GI(z, PREPASS_NVN_EQUAL); g.st = 1; prepass_note_gbuffer(&C, 5, &g); }   // frame 2: a stencil draw after the claims (occ owed): harmless
    }
    prepass_frame_begin(&C, 7); bad[6] = (int)C.prev_frame_bad;
    CHECK(skipped[1] == 0);                 // no history: nothing skipped
    CHECK(skipped[2] == 101);               // a and s (clean in frame 1); occ never (unmatched)
    CHECK(skipped[3] == 101 && bad[3] == 1);   // s skipped on its frame-2 verdict, then it is not drawn: bad (unmatched), one frame
    CHECK(skipped[4] == 1 && bad[4] == 1);     // s not skipped (frame-3 verdict); the stencil draw before a's claim: hazard
    CHECK(skipped[5] == 0 && bad[5] == 0);     // both were owed when the hazard ran in frame 4: neither is a candidate in frame 5
    CHECK(skipped[6] == 101 && bad[6] == 0);   // clean again
    CHECK(bad[2] == 0);
    // a read of the depth image between the claims: keys claimed before it are fine, later ones unsafe
    prepass_ctx_init(&C, 0); prepass_frame_begin(&C, 1);
    { prepass_pre_in p = PI(z, -1); prepass_note_pre(&C, 5, &p);
      p = PI(a, -1); CHECK(prepass_note_pre(&C, 5, &p) == 1); p = PI(s, -1); CHECK(prepass_note_pre(&C, 5, &p) == 1);
      prepass_note_read(&C, 6);   // another image: nothing
      prepass_gb_in g = GI(a, PREPASS_NVN_EQUAL); prepass_note_gbuffer(&C, 5, &g);
      prepass_note_read(&C, 5);   // e.g. a copy of the depth for hi-z
      g = GI(s, PREPASS_NVN_EQUAL); prepass_note_gbuffer(&C, 5, &g);
      prepass_frame_begin(&C, 2); CHECK(C.last.bad_keys == 1 && C.last.unsafe == 1 && C.last.hazard[PREPASS_H_READ] == 1 && C.last.ok_keys == 1); }
    // admission limit: 10 new clean meshes, 4 a frame -> 0, 4, 8, 10, 10 skipped in frames 1..5 (established ones are not limited)
    prepass_ctx_init(&C, 1); C.admit_limit = 4;
    { const uint32_t want[6] = {0, 0, 4, 8, 10, 10};
      for (int fr = 1; fr <= 5; fr++) {
        prepass_frame_begin(&C, (uint64_t)fr);
        prepass_pre_in p = PI(z, -1); prepass_note_pre(&C, 5, &p);
        uint32_t sk = 0;
        for (int k = 0; k < 10; k++) { p = PI(prepass_key(0x2000u + 0x40u * (unsigned)k, 0, 30, 0, 1, 0), -1); sk += (uint32_t)prepass_note_pre(&C, 5, &p); }
        for (int k = 0; k < 10; k++) { prepass_gb_in g = GI(prepass_key(0x2000u + 0x40u * (unsigned)k, 0, 30, 0, 1, 0), PREPASS_NVN_EQUAL); prepass_note_gbuffer(&C, 5, &g); }
        CHECK(sk == want[fr]);
      }
      prepass_frame_begin(&C, 6); CHECK(C.prev_frame_bad == 0); }
    // fallback: 31 bad frames inside the window
    prepass_ctx_init(&C, 1); int fb = 0;
    for (int i = 0; i < 31 && !fb; i++) fb = prepass_fallback_note(&C, 1);
    CHECK(fb == 1);
    prepass_ctx_init(&C, 1); fb = 0;
    for (int i = 0; i < 3000 && !fb; i++) fb = prepass_fallback_note(&C, i % 25 == 0);   // 24 a window: below the bar
    CHECK(fb == 0);
}

// ---- depth + colour model: W pixels; an object covers [x0, x1) at depth z ----
enum { W = 48, MAXO = 24, MAXG = 3 };
enum { F_NEVER = 1, F_LESS = 2, F_EQUAL = 3, F_LEQUAL = 4, F_GREATER = 5, F_ALWAYS = 8 };
typedef struct { int df, dw, blend, shift, st; uint64_t kill; } gdraw;
typedef struct {
    int key, x0, x1, pre, prekills, ng; float z; uint64_t prekill; gdraw g[MAXG];
} obj;
typedef struct { int obj, gi; } gref;
static int dtest(int f, float z, float d) {
    switch (f) { case F_NEVER: return 0; case F_LESS: return z < d; case F_EQUAL: return z == d; case F_LEQUAL: return z <= d;
                 case F_GREATER: return z > d; case F_ALWAYS: return 1; default: return 0; }
}
// one frame; mode 0 = the game, 1 = DM_DOS2_PREPASS=1 through C. pre_order / gb_order: draw orders.
static void frame(const obj *o, int n, const int *pre_order, const gref *gb, int ng, int mode, uint64_t fr, float *depth, uint32_t *col,
                  int *nskip) {
    for (int x = 0; x < W; x++) { depth[x] = 1.0f; col[x] = 0; }
    if (mode) prepass_frame_begin(&C, fr);
    *nskip = 0;
    for (int k = 0; k < n; k++) {
        const obj *b = &o[pre_order[k]];
        if (!b->pre) continue;
        if (mode) {
            prepass_pre_in p = {prepass_key(0x1000u + 0x100u * (unsigned)b->key, 0, 6, 0, 1, 0), 0x40, 7, 6, b->prekills ? PREPASS_K_KILL : -1};
            if (prepass_note_pre(&C, 3, &p)) { (*nskip)++; continue; }
        }
        for (int x = b->x0; x < b->x1; x++) if (!(b->prekill >> x & 1) && b->z < depth[x]) depth[x] = b->z;
    }
    for (int k = 0; k < ng; k++) {
        const obj *b = &o[gb[k].obj]; const gdraw *g = &b->g[gb[k].gi];
        int df = g->df, dw = g->dw;
        if (mode) {
            prepass_gb_in q = {prepass_key(0x1000u + 0x100u * (unsigned)b->key, 0, 6, 0, 1, 0), 0x40, 7u + (unsigned)g->shift, 1, 1, g->df, g->dw, g->st, 0, g->blend, g->kill != 0};
            if (prepass_note_gbuffer(&C, 3, &q) && mode == 1) { df = F_LEQUAL; dw = 1; }
        }
        for (int x = b->x0 + g->shift; x < b->x1 + g->shift && x < W; x++) {
            if (g->kill >> x & 1) continue;
            if (!dtest(df, b->z, depth[x])) continue;
            if (dw) depth[x] = b->z;
            col[x] = g->blend ? col[x] * 7u + (uint32_t)(gb[k].obj + 1) * 131u + (uint32_t)gb[k].gi : (uint32_t)(gb[k].obj + 1) * 1000u + (uint32_t)gb[k].gi;
            // (the stencil flag only marks the draw for the hazard rule; the model has no stencil buffer)
        }
    }
}

static void test_model(void) {
    const uint64_t cb = 0x5555555555555555ull;
    // key, x0, x1, pre, prekills, ng, z, prekill, G-buffer draws
    obj o[] = {
        {1, 0, 20, 1, 0, 1, 0.30f, 0, {{F_EQUAL, 0, 0, 0, 0, 0}}},
        {2, 10, 30, 1, 0, 1, 0.20f, 0, {{F_EQUAL, 0, 0, 0, 0, 0}}},
        {3, 25, 40, 1, 1, 1, 0.10f, 0x0000F0F0F0000000ull, {{F_EQUAL, 0, 0, 0, 0, 0}}},   // foliage: holes
        {4, 35, 48, 1, 1, 1, 0.15f, cb, {{F_EQUAL, 0, 0, 0, 0, 0}}},                      // fading: dither
        {5, 0, 48, 1, 0, 1, 0.50f, 0, {{F_EQUAL, 0, 0, 0, 0, 0}}},                        // terrain base ...
        {5, 0, 48, 1, 1, 1, 0.50f, 0xFFFF0000FFFFull, {{F_EQUAL, 0, 1, 0, 0, 0}}},       // ... and its layer (same mesh, kills, blends)
        {6, 0, 48, 1, 0, 1, 0.90f, 0, {{F_EQUAL, 0, 0, 0, 0, 0}}},                        // far wall
        {2, 40, 48, 1, 0, 1, 0.05f, 0, {{F_EQUAL, 0, 0, 0, 0, 0}}},                       // barrel mesh again, in front
        {7, 5, 15, 1, 0, 0, 0.25f, 0, {{0}}},                                              // depth-only occluder
        {8, 0, 48, 0, 0, 1, 0.95f, 0, {{F_LEQUAL, 0, 1, 0, 0, 0}}},                       // a blended decal-ish draw without pre-pass
    };
    const int n = sizeof o / sizeof o[0];
    int po[MAXO]; gref gb[MAXO * MAXG]; int ng = 0;
    for (int i = 0; i < n; i++) po[i] = i;
    for (int i = 0; i < n; i++) for (int j = 0; j < o[i].ng; j++) { gb[ng].obj = i; gb[ng].gi = j; ng++; }
    for (int gate = 1; gate >= 0; gate--) {
        prepass_ctx_init(&C, gate);
        int skipped_total = 0, bad_frames = 0, diff_frames = 0;
        for (int fr = 1; fr <= 4; fr++) {
            float d0[W], d1[W]; uint32_t c0[W], c1[W]; int s0, s1;
            frame(o, n, po, gb, ng, 0, 0, d0, c0, &s0);
            frame(o, n, po, gb, ng, 1, (uint64_t)fr, d1, c1, &s1);
            prepass_retire(&C);   // the frame's check now (the next frame_begin finds the table retired)
            const uint32_t bad = C.frame_bad;
            int diff = 0; for (int x = 0; x < W; x++) diff += d0[x] != d1[x] || c0[x] != c1[x];
            skipped_total += s1; bad_frames += bad != 0; diff_frames += diff != 0;
            if (gate) CHECK(diff == 0 && bad == 0);
            if (diff) CHECK(bad != 0);
        }
        if (gate) CHECK(skipped_total > 0);
        else CHECK(diff_frames > 0 && bad_frames > 0);   // ungated: the occluder is skipped, its depth lost, the frame says so
        printf("model (gate %d): skipped %d draws in 4 frames, frames bad %d, frames differing %d\n", gate, skipped_total, bad_frames, diff_frames);
    }
}

static uint32_t rs = 12345u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static void shuffle(int *a, int n) { for (int i = n - 1; i > 0; i--) { int j = (int)(rnd() % (uint32_t)(i + 1)); int t = a[i]; a[i] = a[j]; a[j] = t; } }
static void rand_gdraw(gdraw *g, int prekills) {
    memset(g, 0, sizeof *g);
    const uint32_t r = rnd() % 100;
    g->df = r < 70 ? F_EQUAL : r < 82 ? F_LEQUAL : r < 90 ? F_LESS : r < 95 ? F_GREATER : F_ALWAYS;
    g->dw = rnd() % 5 == 0;
    g->blend = rnd() % 6 == 0;
    if (prekills ? rnd() % 2 : rnd() % 12 == 0) g->kill = ((uint64_t)rnd() << 32 | rnd()) & ((uint64_t)rnd() << 32 | rnd());
    g->shift = rnd() % 15 == 0;
    g->st = rnd() % 40 == 0;
}
// Assumption A1 (NR-30.md §2): within the G-buffer pass an object draws its geometry at most once as a full replacement (EQUAL / LEQUAL,
// no blend / partial mask, no kill, same raster); further draws of it there are layers that blend, mask, kill or test otherwise. (Two
// full replacements of one mesh would be redundant; with another instance of the mesh that only the pre-pass draws, the claim count
// would balance and the second instance would lose its depth unseen.)
static int claims_full(const gdraw *g) { return (g->df == F_EQUAL || g->df == F_LEQUAL) && !g->blend && !g->kill && !g->shift && !g->st; }
// Assumption A2: an object the game does not draw in the pre-pass shows itself in the G-buffer with a depth write and a test other than
// EQUAL (with EQUAL against a depth it never wrote it would be invisible; without a write, later draws behind it would cover it).
static void a1(obj *b) { int seen = 0; for (int j = 0; j < b->ng; j++) if (claims_full(&b->g[j])) { if (seen) b->g[j].blend = 1; seen = 1; }
    if (!b->pre) for (int j = 0; j < b->ng; j++) { b->g[j].dw = 1; if (b->g[j].df == F_EQUAL) b->g[j].df = F_LEQUAL; } }
static void rand_obj(obj *b, int i, int keys) {
    memset(b, 0, sizeof *b);
    b->key = (int)(rnd() % (uint32_t)keys);
    b->x0 = (int)(rnd() % W); b->x1 = b->x0 + 1 + (int)(rnd() % (W - (uint32_t)b->x0));
    b->z = 0.01f + 0.0371f * (float)i;   // distinct depths (exact ties = coplanar z-fighting: order-dependent in the game too, NR-30.md §2)
    b->pre = rnd() % 10 != 0;
    b->prekills = rnd() % 3 == 0;
    if (b->prekills) b->prekill = ((uint64_t)rnd() << 32 | rnd()) & ((uint64_t)rnd() << 32 | rnd());
    const uint32_t r = rnd() % 10;
    b->ng = r == 0 ? 0 : r < 8 ? 1 : 2;
    for (int j = 0; j < b->ng; j++) rand_gdraw(&b->g[j], b->prekills);
    if (rnd() % 2) { b->g[0].df = F_EQUAL; b->g[0].dw = 0; b->g[0].blend = 0; b->g[0].kill = 0; b->g[0].shift = 0; b->g[0].st = 0; }   // the common case
    a1(b);
}
static void test_random(void) {
    long frames = 0, clean = 0, skipped = 0, bad_fr = 0, caught = 0;
    const int scenes = getenv("PP_SCENES") ? atoi(getenv("PP_SCENES")) : 6000;   // PP_SCENES / PP_SEED: longer runs
    if (getenv("PP_SEED")) rs = (uint32_t)strtoul(getenv("PP_SEED"), 0, 10) | 1u;
    for (int scene = 0; scene < scenes; scene++) {
        const int n = 4 + (int)(rnd() % (MAXO - 4)), keys = 2 + (int)(rnd() % 12);
        obj o[MAXO]; for (int i = 0; i < n; i++) rand_obj(&o[i], i, keys);
        prepass_ctx_init(&C, 1); C.admit_limit = getenv("PP_ADMIT") ? (uint32_t)atoi(getenv("PP_ADMIT")) : 0;
        for (int fr = 1; fr <= 6; fr++) {
            if (fr > 1) {   // roles change between frames
                const int changes = (int)(rnd() % 3);
                for (int c = 0; c < changes; c++) {
                    obj *b = &o[rnd() % (uint32_t)n];
                    switch (rnd() % 6) {
                    case 0: b->prekills = !b->prekills; b->prekill = b->prekills ? ((uint64_t)rnd() << 32 | rnd()) : 0; break;   // a fade begins / ends
                    case 1: b->ng = b->ng ? 0 : 1; if (b->ng) rand_gdraw(&b->g[0], b->prekills); break;             // leaves / enters the G-buffer
                    case 2: b->pre = !b->pre; break;   // (a1 below: then its G-buffer draws write depth)
                    case 3: if (b->ng < MAXG) { rand_gdraw(&b->g[b->ng], b->prekills); b->ng++; } break;         // a layer appears
                    case 4: if (b->ng) rand_gdraw(&b->g[rnd() % (uint32_t)b->ng], b->prekills); break;             // a draw changes state
                    default: b->key = (int)(rnd() % (uint32_t)keys); break;                                           // another mesh
                    }
                    a1(b);
                }
            }
            int po[MAXO]; for (int i = 0; i < n; i++) po[i] = i;
            gref gb[MAXO * MAXG]; int ng = 0, gi[MAXO * MAXG];
            for (int i = 0; i < n; i++) for (int j = 0; j < o[i].ng; j++) { gb[ng].obj = i; gb[ng].gi = j; ng++; }
            // the game's order: a random pre-pass order; G-buffer random, but an object's own draws stay in their order (layer after base)
            shuffle(po, n);
            for (int k = 0; k < ng; k++) gi[k] = k;
            shuffle(gi, ng);
            gref g2[MAXO * MAXG]; int used[MAXO] = {0};
            for (int k = 0; k < ng; k++) { const int ob = gb[gi[k]].obj; g2[k].obj = ob; g2[k].gi = used[ob]++; }
            float d0[W], d1[W]; uint32_t c0[W], c1[W]; int s0, s1;
            frame(o, n, po, g2, ng, 0, 0, d0, c0, &s0);
            frame(o, n, po, g2, ng, 1, (uint64_t)(fr * 2), d1, c1, &s1);
            prepass_retire(&C);   // the frame's check now (the next frame_begin retires an empty table)
            const uint32_t bad = C.frame_bad;
            int diff = 0; for (int x = 0; x < W; x++) diff += d0[x] != d1[x] || c0[x] != c1[x];
            frames++; skipped += s1; if (!bad) clean++; else bad_fr++;
            if (diff) { CHECK(bad != 0); if (bad) caught++;
                if (!bad && getenv("PP_DEBUG")) { static int shown = 0; if (shown++ < 2) {
                    printf("scene %d frame %d n %d: skipped %d\n", scene, fr, n, s1);
                    for (int i = 0; i < n; i++) { const obj *b = &o[i]; printf(" o%d key %d [%d,%d) z %.4f pre %d kills %d ng %d:", i, b->key, b->x0, b->x1, b->z, b->pre, b->prekills, b->ng);
                        for (int j = 0; j < b->ng; j++) printf(" {df %d dw %d bl %d sh %d st %d kill %d}", b->g[j].df, b->g[j].dw, b->g[j].blend, b->g[j].shift, b->g[j].st, b->g[j].kill != 0); printf("\n"); }
                    printf(" pre order:"); for (int i = 0; i < n; i++) printf(" %d", po[i]); printf("\n gb order:"); for (int k = 0; k < ng; k++) printf(" %d.%d", g2[k].obj, g2[k].gi); printf("\n");
                    for (int x = 0; x < W; x++) if (d0[x] != d1[x] || c0[x] != c1[x]) printf("  x %d: depth %.4f / %.4f col %u / %u\n", x, d0[x], d1[x], c0[x], c1[x]); } } }
            if (fails > 20) return;
        }
    }
    printf("random: %ld frames, %ld clean (identical by the property), %ld with bad keys (%ld of them differ - all caught), %ld draws skipped\n",
           frames, clean, bad_fr, caught, skipped);
    CHECK(skipped > 0 && clean > frames / 4);
}

// a still scene (same objects, same draw orders every frame): from the second frame on (first with a verdict) never a bad key, and the
// picture is identical in every frame
static void test_still(void) {
    long skipped = 0, cand = 0;
    for (int scene = 0; scene < 4000; scene++) {
        const int n = 4 + (int)(rnd() % (MAXO - 4)), keys = 2 + (int)(rnd() % 12);
        obj o[MAXO]; for (int i = 0; i < n; i++) rand_obj(&o[i], i, keys);
        int po[MAXO]; for (int i = 0; i < n; i++) po[i] = i;
        shuffle(po, n);
        gref gb[MAXO * MAXG]; int ng = 0, gi[MAXO * MAXG];
        for (int i = 0; i < n; i++) for (int j = 0; j < o[i].ng; j++) { gb[ng].obj = i; gb[ng].gi = j; ng++; }
        for (int k = 0; k < ng; k++) gi[k] = k;
        shuffle(gi, ng);
        gref g2[MAXO * MAXG]; int used[MAXO] = {0};
        for (int k = 0; k < ng; k++) { const int ob = gb[gi[k]].obj; g2[k].obj = ob; g2[k].gi = used[ob]++; }
        prepass_ctx_init(&C, 1);
        for (int fr = 1; fr <= 4; fr++) {
            float d0[W], d1[W]; uint32_t c0[W], c1[W]; int s0, s1;
            frame(o, n, po, g2, ng, 0, 0, d0, c0, &s0);
            frame(o, n, po, g2, ng, 1, (uint64_t)fr, d1, c1, &s1);
            const uint32_t pc = C.f.pre_cand;
            prepass_retire(&C);
            int diff = 0; for (int x = 0; x < W; x++) diff += d0[x] != d1[x] || c0[x] != c1[x];
            CHECK(C.frame_bad == 0 && diff == 0);
            if (fr == 1) CHECK(s1 == 0);
            if (fr >= 2) { skipped += s1; cand += pc; }
        }
    }
    printf("still: 4000 scenes x 4 frames, no bad key, identical; frames 2-4 skipped %ld of %ld candidates\n", skipped, cand);
}

int main(void) {
    test_classify(); test_accounting(); test_gate(); test_model(); test_random(); test_still();
    if (fails) { printf("prepass_test: %d FAIL of %ld checks\n", fails, checks); return 1; }
    printf("prepass_test: PASS (%ld checks)\n", checks); return 0;
}
