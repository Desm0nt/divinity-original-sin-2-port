// [DOS2] Host test of hle_dos2_touch_tips.h (UI mode touch: the keyboard-only loading screen tips leave the engine's PC pick).
// Run with any C compiler: cc dos2_touch_tips_test.c -o <outside the repo>; run it.
#include "../hle_dos2_touch_tips.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
int main(void) {
    // the count is bits 24..47 of the field (the pick's ubfx #0x18, #0x18); the low 24 bits and the top 16 are something else
    assert(tt_count(0) == 0 && tt_count(52ull << 24) == 52 && tt_count((52ull << 24) | 0xffffffull | (0xffffull << 48)) == 52);
    assert(tt_count(0xffffffull << 24) == 0xffffff);
    // keyboard-only keys are ours; the general ones and the console twins stay in the engine's pick
    assert(tt_off_key("HINT_STACKS") && tt_off_key("HINT_CTRL") && tt_off_key("HINT_TACTICALVIEW") && tt_off_key("HINT_PICKPOCKET"));
    assert(!tt_off_key("HINT_CTRL_CONT") && !tt_off_key("HINT_PICKPOCKET_CONSOLE") && !tt_off_key("HINT_OUTLINE_CONSOLE"));
    assert(!tt_off_key("HINT_ENVIRONMENT") && !tt_off_key("HINT_CAM") && !tt_off_key("") && !tt_off_key(NULL));
    assert(TT_NOFF == 14);
    // the plan: what keeps Keyboard set once every hit is cleared (0 = the pick would loop: do not clear)
    const uint8_t hit[5] = {1, 0, 1, 0, 0}, kb[5] = {1, 1, 1, 0, 1};
    assert(tt_plan(hit, kb, 5) == 2);
    const uint8_t all[3] = {1, 1, 1}, kb3[3] = {1, 1, 1};
    assert(tt_plan(all, kb3, 3) == 0);
    const uint8_t none[2] = {0, 0}, kb0[2] = {0, 0};
    assert(tt_plan(none, kb0, 2) == 0);   // a table with no keyboard tip at all: nothing to leave (the caller does not clear)
    // the entry layout of main.raw 4979b200 (the parser's destinations, the pick's reads)
    assert(TT_ENTRY == 0x38 && TT_E_KEY == 0 && TT_E_CONTROLLER == 0x30 && TT_E_KEYBOARD == 0x31 && TT_HINTS == 0x329a330);
    puts("dos2_touch_tips_test: PASS");
    return 0;
}
