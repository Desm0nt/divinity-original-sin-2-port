// [DOS2] NR-TRAFFIC round 2 DM_NR_ARENA host test (docs/dos2/nr/NR-TRAFFIC.md §10): the stream-arena keep rule of dm/gfx/nr/nr_arena.h.
// Invariants on random traces against a reference (the max of the current window and of the window before, re-derived per step):
// the limit never drops below the last submission's need (the same submission again never reallocates), it shrinks only when a window
// ends or a trim request comes, a trim shrinks to the last need once; plus a DOS2-shaped trace (a level-load spike of 3 chunks, then play
// at 1): rule 1 ends at 1 chunk a slot after one quiet window, the old rule keeps 3 for good.
// Build: clang -std=c11 -O2 -Wall -Wextra nr_arena_test.c   (no other file)
#include "../../../nr/nr_arena.h"
#include <stdio.h>
#include <string.h>

static int fails = 0; static long long checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 30) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static unsigned long long rs = 0x2545F4914F6CDD1Dull;
static unsigned rn(unsigned n) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return n ? (unsigned)(rs % n) : 0; }

static void test_random(void) {
    for (int it = 0; it < 2000; it++) {
        nrar_state s; memset(&s, 0, sizeof s);
        const unsigned hold = 1 + rn(50); unsigned gen = 0, prev = 0, wmax = 0, n = 0; int trimmed = 0;
        for (int k = 0; k < 400; k++) {
            const unsigned used = rn(10) < 8 ? rn(2) : 1 + rn(6);
            const int trim = rn(100) == 0; if (trim) gen++;
            int rg = 0; const unsigned lim = nrar_keep(&s, used, hold, gen, &rg);
            CHECK(lim >= 1, "limit >= 1");
            CHECK(lim >= used, "never below the last need (%u < %u)", lim, used);
            CHECK(rg == (prev && used > prev), "regrown = needed more than kept");
            // reference: window bookkeeping (a trim restarts the window)
            if (used > wmax) wmax = used;
            const int end = ++n >= hold;
            if (trim) { CHECK(lim == (used ? used : 1), "a trim shrinks to the last need"); wmax = 0; n = 0; trimmed = 1; }
            else if (end) { CHECK(lim == (wmax ? wmax : 1), "window end: the window's max (%u vs %u)", lim, wmax); wmax = 0; n = 0; trimmed = 0; }
            if (!trim && !end && prev && !trimmed) CHECK(lim >= prev, "no shrink inside a window (%u after %u)", lim, prev);
            prev = lim;
        }
    }
}
static void test_dos2_trace(void) {
    // one slot: a load (3 chunks for 40 submissions), then play (1 chunk), hold 600
    nrar_state s; memset(&s, 0, sizeof s); int rg = 0; unsigned lim = 0, regrows = 0;
    for (int k = 0; k < 40; k++) { lim = nrar_keep(&s, 3, 600, 0, &rg); regrows += rg; }
    CHECK(lim == 3, "during the load: 3");
    for (int k = 0; k < 559; k++) { lim = nrar_keep(&s, 1, 600, 0, &rg); regrows += rg; }
    CHECK(lim == 3, "until the window ends: 3 (no thrash)");
    lim = nrar_keep(&s, 1, 600, 0, &rg);
    CHECK(lim == 3, "the first window held the load: its max 3");
    for (int k = 0; k < 600; k++) { lim = nrar_keep(&s, 1, 600, 0, &rg); regrows += rg; }
    CHECK(lim == 1, "after a quiet window: 1 chunk (old rule: 3 for good)");
    lim = nrar_keep(&s, 3, 600, 0, &rg); regrows += rg;
    CHECK(rg == 1 && lim == 3, "the next load grows at once (one reallocation)");
    CHECK(regrows == 1, "one regrow in the whole trace (%u)", regrows);
    lim = nrar_keep(&s, 1, 600, 1, &rg);
    CHECK(lim == 1, "an NR-29 trim shrinks to the last need at once");
    printf("  DOS2 trace: load 3 chunks -> play 1: rule 1 holds 1 chunk (32 MB) a slot after one quiet window, the old rule 3 (96 MB)\n");
}
int main(void) {
    test_random(); test_dos2_trace();
    printf("%s: %lld checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
