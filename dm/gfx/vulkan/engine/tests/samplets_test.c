// [DOS2] NR-14 DM_GFX_SAMPLES_TS policy test (dm/gfx/vulkan/engine/xbg_samplets.h): env parsing, the DOS2 auto default (0 without a
// consumer, 4 with DM_GFX_COND_RENDER / DM_NR_OCC), the other games' default 1, the per-report effective mode (0 while counting -> 4,
// check -> 1), the address-range guard and what a touch does per mode; plus a model of one DOS2 frame: 263 report pairs in the 128 KB
// counter buffer (two halves alternating), the game timestamps of the DRS pair outside it, and the backend accesses a frame makes.
// Build + run (from this directory):  clang -std=c11 -Wall -Wextra samplets_test.c -o samplets_test && ./samplets_test
#include "../xbg_samplets.h"
#include <stdio.h>

static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static void test_env(void) {
    int w = -1;
    CHECK(samplets_mode(1, NULL, NULL, NULL, &w) == SAMPLETS_DROP && w == SAMPLETS_WHY_NO_CONSUMER);
    CHECK(samplets_mode(1, "", "", "", &w) == SAMPLETS_DROP && w == SAMPLETS_WHY_NO_CONSUMER);
    CHECK(samplets_mode(1, NULL, "0", "0", &w) == SAMPLETS_DROP && w == SAMPLETS_WHY_NO_CONSUMER);
    CHECK(samplets_mode(1, NULL, "1", NULL, &w) == SAMPLETS_SHARED && w == SAMPLETS_WHY_CONDR);
    CHECK(samplets_mode(1, NULL, "4", NULL, &w) == SAMPLETS_SHARED && w == SAMPLETS_WHY_CONDR);
    CHECK(samplets_mode(1, NULL, NULL, "2", &w) == SAMPLETS_SHARED && w == SAMPLETS_WHY_NR_OCC);
    CHECK(samplets_mode(1, NULL, "2", "1", &w) == SAMPLETS_SHARED && w == SAMPLETS_WHY_CONDR);
    // env always wins, 0 too (A/B), and on other games
    for (int v = 0; v <= 4; v++) { char e[2] = {(char)('0' + v), 0};
        CHECK(samplets_mode(1, e, "1", "1", &w) == v && w == SAMPLETS_WHY_ENV);
        CHECK(samplets_mode(0, e, NULL, NULL, &w) == v && w == SAMPLETS_WHY_ENV); }
    // a value that is not one digit 0..4 is the old behaviour
    CHECK(samplets_mode(1, "5", NULL, NULL, &w) == SAMPLETS_BOP && w == SAMPLETS_WHY_BAD_ENV);
    CHECK(samplets_mode(1, "04", NULL, NULL, &w) == SAMPLETS_BOP && w == SAMPLETS_WHY_BAD_ENV);
    CHECK(samplets_mode(1, "x", NULL, NULL, &w) == SAMPLETS_BOP && w == SAMPLETS_WHY_BAD_ENV);
    CHECK(samplets_mode(1, "-1", NULL, NULL, &w) == SAMPLETS_BOP);
    // not DOS2: 1 whatever the consumers say (D3 culls by these reports on the CPU)
    CHECK(samplets_mode(0, NULL, NULL, NULL, &w) == SAMPLETS_BOP && w == SAMPLETS_WHY_OTHER_GAME);
    CHECK(samplets_mode(0, NULL, "0", "0", &w) == SAMPLETS_BOP);
    CHECK(samplets_mode(1, NULL, NULL, NULL, NULL) == SAMPLETS_DROP);
    CHECK(samplets_why_name(SAMPLETS_WHY_NO_CONSUMER)[0] == 'a');
}

static void test_effective(void) {
    CHECK(samplets_effective(SAMPLETS_DROP, 0) == SAMPLETS_DROP);
    CHECK(samplets_effective(SAMPLETS_DROP, 1) == SAMPLETS_SHARED);   // a counter reset: a real occlusion run to close
    CHECK(samplets_effective(SAMPLETS_CHECK, 0) == SAMPLETS_BOP && samplets_effective(SAMPLETS_CHECK, 1) == SAMPLETS_BOP);
    for (int m = 1; m <= 4; m++) if (m != SAMPLETS_CHECK) { CHECK(samplets_effective(m, 0) == m); CHECK(samplets_effective(m, 1) == m); }
}

static void test_range(void) {
    samplets_range r = {0, 0};
    CHECK(!samplets_range_hit(&r, 0, ~0ull >> 1));                      // empty: nothing hits
    samplets_range_add(&r, 0x1000);
    CHECK(r.lo == 0x1000 && r.hi == 0x1010);
    CHECK(samplets_range_hit(&r, 0x1000, 1) && samplets_range_hit(&r, 0x100f, 1) && samplets_range_hit(&r, 0xff0, 0x11));
    CHECK(!samplets_range_hit(&r, 0x1010, 16) && !samplets_range_hit(&r, 0xff0, 0x10) && !samplets_range_hit(&r, 0x1000, 0));
    samplets_range_add(&r, 0x800); samplets_range_add(&r, 0x3000); samplets_range_add(&r, 0x2000);
    CHECK(r.lo == 0x800 && r.hi == 0x3010);
    CHECK(samplets_range_hit(&r, 0x2500, 8));                            // bounding box: a gap counts (errs to safety)
}

static void test_touch(void) {
    CHECK(samplets_touch_action(SAMPLETS_DROP, 1) == 1 && samplets_touch_action(SAMPLETS_DROP, 2) == 1);   // GPU snapshots: count only
    for (unsigned w = 0; w <= 12; w++) if (w != 1 && w != 2) {
        CHECK(samplets_touch_action(SAMPLETS_DROP, w) == 3);
        CHECK(samplets_touch_action(SAMPLETS_CHECK, w) == 2);
        CHECK(samplets_touch_action(SAMPLETS_BOP, w) == 1 && samplets_touch_action(SAMPLETS_SHARED, w) == 1 && samplets_touch_action(SAMPLETS_TOP, w) == 1);
    }
}

// One DOS2 frame (NR-03.md §1): the counter buffer (128 KB) at B, frame f uses half f & 1, object k's pair = B + half * 0x10000 + 32 k;
// 263 pairs = 526 samples reports (ship deck). The DRS pair (TIMESTAMP_TOP / TIMESTAMP) lives elsewhere (renderer object), so the guard
// never sees it; nor the backend's accesses of a frame (vertex / index / uniform ranges far from the buffer).
static void test_frame_model(void) {
    const uint64_t B = 0x8a000000ull, drs = 0x7f000100ull;
    samplets_range r = {0, 0};
    int dropped = 0, kept = 0, fall = 0;
    int mode = samplets_mode(1, NULL, NULL, NULL, NULL);
    for (int f = 0; f < 4; f++) {
        const uint64_t half = (uint64_t)(f & 1) * 0x10000;
        for (int k = 0; k < 263; k++) for (int s = 0; s < 2; s++) {
            const uint64_t a = B + half + 32ull * (uint64_t)k + 16ull * (uint64_t)s;
            samplets_range_add(&r, a);
            if (samplets_effective(mode, 0) == SAMPLETS_DROP) dropped++; else kept++;
        }
        // the backend's own in-stream accesses this frame: uniform updates, index reads, a copy - none near the buffer
        const uint64_t acc[][2] = {{0x60000000ull, 4096}, {0x9a000000ull, 65536}, {drs, 16}, {B - 4096, 4096}, {B + 0x20000, 64}};
        for (unsigned i = 0; i < sizeof acc / sizeof acc[0]; i++)
            if (samplets_range_hit(&r, acc[i][0], acc[i][1]) && samplets_touch_action(mode, 11) == 3) { mode = SAMPLETS_SHARED; fall++; }
    }
    CHECK(dropped == 4 * 526 && kept == 0 && fall == 0 && mode == SAMPLETS_DROP);
    CHECK(r.lo == B && r.hi == B + 0x10000 + 32 * 262 + 32);           // both halves, nothing outside the buffer
    // a game that copies its counter buffer (a reader): the run falls back to 4 at once, later reports are stamped per pass
    if (samplets_range_hit(&r, B + 0x100, 32) && samplets_touch_action(mode, 12) == 3) { mode = SAMPLETS_SHARED; fall++; }
    CHECK(fall == 1 && mode == SAMPLETS_SHARED && samplets_effective(mode, 0) == SAMPLETS_SHARED);
    CHECK(samplets_touch_action(mode, 12) == 1);                         // after the switch only counted
}

int main(void) {
    test_env(); test_effective(); test_range(); test_touch(); test_frame_model();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
