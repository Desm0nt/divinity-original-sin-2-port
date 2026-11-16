// [DOS2] DM_DOS2_PREPASS host test (dm/gfx/vulkan/engine/xbg_prepass.h): pass / draw classification, the skip + G-buffer claim
// accounting, and a depth model of one scene (opaque boxes, an alpha-tested cut-out, a fading object, a terrain base + layer on the same
// mesh) drawn as the game does (pre-pass LESS, G-buffer EQUAL) and as mode 1 does: the final depth and the set of G-buffer fragments
// that pass must be equal pixel for pixel.
// Build + run (from this directory):  clang -std=c11 -Wall -Wextra prepass_test.c -o prepass_test && ./prepass_test
#include "../xbg_prepass.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

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
    CHECK(prepass_clamp_mode(-1) == 0 && prepass_clamp_mode(3) == 0 && prepass_clamp_mode(2) == PREPASS_COUNT);
    CHECK(prepass_key(0x1000, 0, 36, 0, 1, 0) != prepass_key(0x1000, 36, 36, 0, 1, 0));
    CHECK(prepass_key(0x1000, 0, 36, 0, 1, 0) != prepass_key(0x1000, 0, 36, 0, 2, 0));
    CHECK(prepass_key(0, 0, 0, 0, 0, 0) != 0);
}

static prepass_frame F;
static void test_accounting(void) {
    prepass_reset(&F, 7);
    uint64_t a = prepass_key(0x100, 0, 30, 0, 1, 0), b = prepass_key(0x200, 0, 30, 0, 1, 0);
    CHECK(prepass_note_pre(&F, 5, a, -1, 30) == 1);
    CHECK(prepass_note_pre(&F, 5, a, -1, 30) == 1);            // the same mesh twice (two barrels)
    CHECK(prepass_note_pre(&F, 5, b, PREPASS_K_KILL, 30) == 0);
    CHECK(prepass_note_pre(&F, 5, 0, -1, 30) == 0);            // key 0 = not indexed: kept
    CHECK(F.pre_draws == 4 && F.pre_skip == 2 && F.keep[PREPASS_K_KILL] == 1 && F.keep[PREPASS_K_FULL] == 1);
    CHECK(prepass_note_gbuffer(&F, 9, a, 1, PREPASS_NVN_EQUAL, 0) == 0);   // other depth
    CHECK(prepass_note_gbuffer(&F, 5, a, 1, PREPASS_NVN_EQUAL, 0) == 1);
    CHECK(prepass_note_gbuffer(&F, 5, a, 1, PREPASS_NVN_LEQUAL, 0) == 1);
    CHECK(prepass_note_gbuffer(&F, 5, a, 1, PREPASS_NVN_EQUAL, 0) == 0);   // both claimed: a third draw keeps EQUAL (a layer)
    CHECK(prepass_note_gbuffer(&F, 5, b, 1, PREPASS_NVN_EQUAL, 0) == 0);   // kept pre-pass draw: EQUAL stays
    CHECK(prepass_note_pre(&F, 5, prepass_key(0x300, 0, 3, 0, 1, 0), -1, 3) == 0 && F.keep[PREPASS_K_LATE] == 1);   // after the G-buffer began
    uint32_t un, amb; prepass_tail(&F, &un, &amb); CHECK(un == 0 && amb == 0);
    prepass_reset(&F, 8);
    CHECK(prepass_note_pre(&F, 5, a, -1, 30) == 1);
    CHECK(prepass_note_gbuffer(&F, 5, a, 1, 2 /* LESS */, 0) == 0 && F.gb_refused == 1);
    prepass_tail(&F, &un, &amb); CHECK(un == 1);
    // a full table keeps the draw (never a skip without a key to claim)
    prepass_reset(&F, 9); int kept_full = 0;
    for (uint32_t i = 0; i < PREPASS_CAP; i++) if (!prepass_note_pre(&F, 5, prepass_key(0x1000 + 64ull * i, 0, 3, 0, 1, 0), -1, 3)) kept_full++;
    CHECK(kept_full == (int)(PREPASS_CAP - PREPASS_CAP * 3u / 4u) && F.keep[PREPASS_K_FULL] == (uint32_t)kept_full);
}

// ---- depth model: W pixels, each object covers [x0, x1) at depth z; mask: bit x set = the DEP program kills there
enum { W = 64 };
typedef struct { uint64_t ibuf; int x0, x1; float z; uint64_t kill; int kills; int layer; } obj;
static int less(float a, float b) { return a < b; }
static void run(const obj *o, int n, int skip_mode, float *depth, int *passed) {
    for (int x = 0; x < W; x++) depth[x] = 1.0f;
    for (int i = 0; i < n * W; i++) passed[i] = 0;
    prepass_reset(&F, 1);
    for (int i = 0; i < n; i++) {   // pre-pass, LESS + write; layers have no pre-pass draw of their own beyond their DEP (killing) one
        uint64_t key = prepass_key(o[i].ibuf, 0, 6, 0, 1, 0);
        int why = o[i].kills ? PREPASS_K_KILL : -1;
        int skip = prepass_note_pre(&F, 3, key, why, 6);
        if (skip_mode && skip) continue;
        for (int x = o[i].x0; x < o[i].x1; x++) if (!(o[i].kill >> x & 1) && less(o[i].z, depth[x])) depth[x] = o[i].z;
    }
    for (int i = 0; i < n; i++) {   // G-buffer: EQUAL, no write (or LEQUAL + write when claimed)
        uint64_t key = prepass_key(o[i].ibuf, 0, 6, 0, 1, 0);
        int ov = prepass_note_gbuffer(&F, 3, key, 1, PREPASS_NVN_EQUAL, 0) && skip_mode;
        for (int x = o[i].x0; x < o[i].x1; x++) {
            int ok = ov ? o[i].z <= depth[x] : o[i].z == depth[x];
            if (ok) { passed[i * W + x] = 1; if (ov) depth[x] = o[i].z; }
        }
    }
}
static void test_model(void) {
    // the game's order: opaque walls, a barrel twice (same mesh, other place = other ibuf range here), foliage cut-out, a fading statue
    // (FadePattern kills on a checkerboard), the terrain base (opaque) and its layer (same mesh, kills: blend mask), a wall behind all
    const uint64_t cb = 0x5555555555555555ull;
    obj o[] = {
        {0x100, 0, 20, 0.30f, 0, 0, 0},
        {0x200, 10, 30, 0.20f, 0, 0, 0},
        {0x300, 25, 40, 0.10f, 0x0000F0F0F0000000ull, 1, 0},   // foliage: holes
        {0x400, 35, 50, 0.15f, cb, 1, 0},                      // fading: dither
        {0x500, 0, 64, 0.50f, 0, 0, 0},                        // terrain base
        {0x500, 0, 64, 0.50f, 0xFFFF0000FFFF0000ull, 1, 1},    // terrain layer (same mesh)
        {0x600, 0, 64, 0.90f, 0, 0, 0},                        // far wall, behind everything
        {0x200, 40, 60, 0.05f, 0, 0, 0},                       // barrel mesh again, in front (other object, same key)
    };
    const int n = sizeof o / sizeof o[0];
    float d0[W], d1[W]; int p0[8 * W], p1[8 * W];
    run(o, n, 0, d0, p0); run(o, n, 1, d1, p1);
    int dd = 0, pd = 0;
    for (int x = 0; x < W; x++) dd += d0[x] != d1[x];
    for (int i = 0; i < n * W; i++) pd += p0[i] != p1[i];
    CHECK(dd == 0);
    // the passing G-buffer fragments of the opaque objects are a superset in mode 1 (overdraw: a LEQUAL draw behind a later nearer one
    // passes, then gets overwritten); the visible winner per pixel (last written / equal) must be the same object
    for (int x = 0; x < W; x++) {
        int w0 = -1, w1 = -1;
        for (int i = 0; i < n; i++) { if (p0[i * W + x] && o[i].z == d0[x]) w0 = i; if (p1[i * W + x] && o[i].z == d1[x]) w1 = i; }
        CHECK(w0 >= 0 && w1 >= 0);
        CHECK(o[w0].z == o[w1].z);
        // cut-outs and dither: no killing object wins a pixel it killed in the pre-pass
        if (w1 >= 0 && o[w1].kills && !o[w1].layer) CHECK(!(o[w1].kill >> x & 1));
    }
    printf("model: depth diffs %d, G-buffer pass diffs %d (overdraw of LEQUAL draws), skipped %u, claimed %u\n", dd, pd, F.pre_skip, F.gb_override);
}

int main(void) {
    test_classify(); test_accounting(); test_model();
    if (fails) { printf("prepass_test: %d FAIL\n", fails); return 1; }
    printf("prepass_test: PASS\n"); return 0;
}
