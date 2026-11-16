// [DOS2] DM_GFX_COND_RENDER host test (dm/gfx/vulkan/engine/xbg_condr.h): the predicate of both NVN compare modes, the judgement per
// mode (fresh / stale report, bad address), which draws the host skips per mode and condition kind, and a model of the occlusion pattern
// over frames: one never-reset samples counter, a report pair (before / after the proxy) per object, the condition judged while the
// frame's own reports are still on the GPU (1 submit a frame), so guest memory holds the previous frame's pair.
// Build + run (from this directory):  clang -std=c11 -Wall -Wextra condr_test.c -o condr_test && ./condr_test
#include "../xbg_condr.h"
#include <stdio.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static void test_predicate(void) {
    CHECK(condr_predicate(CONDR_NVN_IF_EQUAL, 5, 5) == 1);
    CHECK(condr_predicate(CONDR_NVN_IF_EQUAL, 5, 6) == 0);
    CHECK(condr_predicate(CONDR_NVN_IF_NOT_EQUAL, 5, 5) == 0);
    CHECK(condr_predicate(CONDR_NVN_IF_NOT_EQUAL, 5, 6) == 1);
    CHECK(condr_predicate(7, 5, 5) == 1 && condr_predicate(-1, 1, 2) == 1);   // unknown mode: render
    CHECK(condr_clamp_mode(-3) == CONDR_OFF && condr_clamp_mode(9) == CONDR_OFF && condr_clamp_mode(4) == CONDR_COUNT && condr_clamp_mode(3) == CONDR_PREV);
}
static void test_judge(void) {
    int js;
    for (int m = 0; m < CONDR_MODES; m++) {
        CHECK(condr_judge(m, CONDR_NVN_IF_NOT_EQUAL, 0, 0, 1, 1, &js) == 1 && !js);    // bad address: render
        CHECK(condr_judge(m, CONDR_NVN_IF_NOT_EQUAL, 1, 0, 1, 1, &js) == 0 && !js);    // fresh, equal: skip
        CHECK(condr_judge(m, CONDR_NVN_IF_NOT_EQUAL, 1, 0, 1, 2, &js) == 1 && !js);
    }
    // stale: only mode 3 judges by memory
    CHECK(condr_judge(CONDR_CPU, CONDR_NVN_IF_NOT_EQUAL, 1, 1, 4, 4, &js) == 1 && !js);
    CHECK(condr_judge(CONDR_GPU, CONDR_NVN_IF_NOT_EQUAL, 1, 1, 4, 4, &js) == 1 && !js);
    CHECK(condr_judge(CONDR_COUNT, CONDR_NVN_IF_NOT_EQUAL, 1, 1, 4, 4, &js) == 1 && !js);
    CHECK(condr_judge(CONDR_PREV, CONDR_NVN_IF_NOT_EQUAL, 1, 1, 4, 4, &js) == 0 && js);
    CHECK(condr_judge(CONDR_PREV, CONDR_NVN_IF_NOT_EQUAL, 1, 1, 4, 9, &js) == 1 && js);
}
static void test_skips(void) {
    const int kinds[] = {CONDR_K_NONE, CONDR_K_ON, CONDR_K_OFF, CONDR_K_COND};
    for (int k = 0; k < 4; k++) for (int r = 0; r < 2; r++) {
        CHECK(!condr_host_skips(CONDR_OFF, kinds[k], r));
        CHECK(!condr_host_skips(CONDR_COUNT, kinds[k], r));
    }
    for (int m = CONDR_GPU; m <= CONDR_PREV; m++) {
        CHECK(!condr_host_skips(m, CONDR_K_NONE, 0) && !condr_host_skips(m, CONDR_K_ON, 0));
        CHECK(condr_host_skips(m, CONDR_K_OFF, 1) && condr_host_skips(m, CONDR_K_OFF, 0));
        CHECK(!condr_host_skips(m, CONDR_K_COND, 1));
        CHECK(condr_host_skips(m, CONDR_K_COND, 0) == (m != CONDR_GPU));   // mode 1: the GPU decides
    }
    CHECK(condr_inside(CONDR_K_OFF) && condr_inside(CONDR_K_COND) && !condr_inside(CONDR_K_ON) && !condr_inside(CONDR_K_NONE));
}
// Objects with a visible-samples sequence over frames; the counter accumulates every sample of every proxy (never reset, as DOS2: no
// ResetCounter, counter_ops resets 0). Each frame writes {before, after} for each object; the condition reads guest memory = the
// previous frame's pair (the frame's own reports are pending). Mode 3 must skip exactly the objects whose proxy passed no sample in the
// previous frame, never one that passed samples; mode 2 renders everything (all stale); mode 4 skips nothing. A pair no report has
// written yet (first frame: guest memory zero or another object's leftovers) is not usable: it renders.
enum { OBJ = 4, FR = 6 };
static void test_frames(void) {
    static const unsigned vis[FR][OBJ] = {{0, 5, 0, 1}, {0, 5, 3, 0}, {2, 0, 0, 0}, {0, 0, 0, 7}, {1, 1, 1, 1}, {0, 0, 9, 0}};
    uint64_t mem[OBJ][2] = {{0}};   // guest memory: counter words of the pair (before, after); zero before the first frame
    uint64_t counter = 1000;          // samples so far (other draws counted too)
    for (int f = 0; f < FR; f++) {
        for (int o = 0; o < OBJ; o++) {
            int js2, js3, js4;
            const int known = f > 0;   // the engine: both halves of the pair were written by a report of this run (condr reports set)
            const int r2 = condr_judge(CONDR_CPU, CONDR_NVN_IF_NOT_EQUAL, known, 1, mem[o][0], mem[o][1], &js2);
            const int r3 = condr_judge(CONDR_PREV, CONDR_NVN_IF_NOT_EQUAL, known, 1, mem[o][0], mem[o][1], &js3);
            const int r4 = condr_judge(CONDR_COUNT, CONDR_NVN_IF_NOT_EQUAL, known, 1, mem[o][0], mem[o][1], &js4);
            CHECK(r2 == 1 && !condr_host_skips(CONDR_CPU, CONDR_K_COND, r2));
            CHECK(!condr_host_skips(CONDR_COUNT, CONDR_K_COND, r4));
            if (f > 0) CHECK(r3 == (vis[f - 1][o] != 0));       // previous frame's visibility decides
            else CHECK(r3 == 1 && !js3);                         // first frame: no report written there yet -> render, not a false skip
            CHECK(condr_host_skips(CONDR_PREV, CONDR_K_COND, r3) == !r3);
        }
        for (int o = 0; o < OBJ; o++) { mem[o][0] = counter; counter += vis[f][o]; mem[o][1] = counter; counter += 13; }   // + other draws
    }
}

int main(void) {
    test_predicate(); test_judge(); test_skips(); test_frames();
    if (fails) { printf("condr_test: %d failures\n", fails); return 1; }
    printf("condr_test: all passed\n");
    return 0;
}
