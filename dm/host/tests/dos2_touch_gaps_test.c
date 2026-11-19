// [DOS2] Host test of hle_dos2_touch_gaps.h (UI mode touch: the EI calls the host takes because the Switch class has no handler) and of
// the panel ids the host uses for them (hle_dos2_touchui_hud.h hu_side_panel / hu_side_type).
// Run with any C compiler: cc dos2_touch_gaps_test.c -lm -o <outside the repo>; run it.
#include "../hle_dos2_touch_gaps.h"
#include "../hle_dos2_touchui_hud.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
int main(void) {
    // the portrait's right click (= the touch long press) of the touch HUD's playerInfo (type 0x26, the class without the handler)
    assert(tg_rule("openCharInventory", 0x26, 0x1237b10) == TG_OPEN_INV);
    // any other class keeps its own: the GM possession bar handles it (0x1258500), the console playerInfo ids are other classes
    assert(tg_rule("openCharInventory", 0x6e, 0x1258500) == TG_PASS);
    assert(tg_rule("openCharInventory", 0x26, 0x1258500) == TG_PASS && tg_rule("openCharInventory", 0x26, -1) == TG_PASS);
    assert(tg_rule("openCharInventory", 0x3d, 0x121f910) == TG_PASS && tg_rule("openCharInventory", 0x73, 0x122de60) == TG_PASS);
    // names the classes do handle are never taken (dialog toggleInv: main+0x1237140; charSel of the portraits)
    assert(tg_rule("toggleInv", 0x0e, 0x1237140) == TG_PASS && tg_rule("charSel", 0x26, 0x1237b10) == TG_PASS && tg_rule(NULL, 0x26, 0x1237b10) == TG_PASS);
    // the route: dmSide code 4 = the PC hot bar's button 1 (inventory), its window type 0x74 (the "open already" test)
    assert(hu_side_panel(4) == 1 && hu_side_type(4) == 0x74);
    assert(hu_side_type(2) == 0x16 && hu_side_type(3) == 0x77 && hu_side_type(5) == 0x29 && hu_side_type(6) == 0x66 && hu_side_type(7) == 0x16);
    assert(hu_side_type(1) == -1 && hu_side_type(8) == -1);
    // the video skip: a video plays when the subtitles class's movie name is set and is not the empty name
    assert(!tg_video_on(0, 0x1234) && !tg_video_on(0x1234, 0x1234) && tg_video_on(0x5678, 0x1234));
    // a double tap is due for a short while only (the game thread takes it on its next poll; a stale one never skips a later video)
    assert(!tg_skip_due(0, 1000) && tg_skip_due(1000, 1000) && tg_skip_due(1000, 1000 + TG_DTAP_MS) && !tg_skip_due(1000, 1001 + TG_DTAP_MS));
    assert(!tg_skip_due(2000, 1000));   // a clock that went back: not due
    assert(TG_EV_SKIPVIDEO == 0xe7 && TG_SUB_UI == 0x30 && TG_SUB_TYPE == 0x20);
    puts("dos2_touch_gaps_test: PASS");
    return 0;
}
