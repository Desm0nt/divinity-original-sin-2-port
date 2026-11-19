// [DOS2] Host test of the UI mode switch rules (hle_dos2_uimode.h, branch dos2-uimodes): DM_DOS2_UI_MODE parsing, what each mode wants
// (mode byte, touches, path table), when the HUD part applies (A at the next load, B at once only when nothing modal is up), the JNI result.
// Run with any C compiler: cc dos2_uimode_test.c -o <outside the repo>; run it.
#include "../hle_dos2_uimode.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
int main(void) {
    // parsing: digits, names, anything else = hybrid (today's behaviour)
    assert(ui_mode_parse(NULL) == UI_HYBRID && ui_mode_parse("") == UI_HYBRID);
    assert(ui_mode_parse("0") == UI_SWITCH && ui_mode_parse("1") == UI_HYBRID && ui_mode_parse("2") == UI_TOUCH);
    assert(ui_mode_parse(" 2") == UI_TOUCH && ui_mode_parse("2\r\n") == UI_TOUCH);
    assert(ui_mode_parse("switch") == UI_SWITCH && ui_mode_parse("touch") == UI_TOUCH && ui_mode_parse("hybrid") == UI_HYBRID);
    assert(ui_mode_parse("3") == UI_HYBRID && ui_mode_parse("-1") == UI_HYBRID && ui_mode_parse("20") == UI_HYBRID && ui_mode_parse("yes") == UI_HYBRID);
    // what a mode wants
    assert(ui_mode_byte(UI_SWITCH) == 1 && ui_mode_byte(UI_HYBRID) == 1 && ui_mode_byte(UI_TOUCH) == 0);
    assert(!ui_mode_touches(UI_SWITCH) && ui_mode_touches(UI_HYBRID) && ui_mode_touches(UI_TOUCH));
    assert(ui_mode_valid(0) && ui_mode_valid(2) && !ui_mode_valid(3) && !ui_mode_valid(-1));
    // path tables: an entry of the hybrid table is on in hybrid only; SWITCH has no table, so nothing is on there
    const unsigned hy = ui_mode_mask(UI_HYBRID), to = ui_mode_mask(UI_TOUCH);
    assert(ui_mode_entry_on(hy, UI_HYBRID) && !ui_mode_entry_on(hy, UI_SWITCH) && !ui_mode_entry_on(hy, UI_TOUCH));
    assert(ui_mode_entry_on(to, UI_TOUCH) && !ui_mode_entry_on(to, UI_HYBRID) && !ui_mode_entry_on(to, UI_SWITCH));
    assert(ui_mode_mask(7) == 0 && !ui_mode_entry_on(hy | to, 5));
    // the HUD part: nothing when it already runs the mode
    assert(ui_hud_step(UI_HYBRID, UI_HYBRID, 3, 1, 1, 0) == UI_HUD_NONE);
    // A: out of game (no player UI set) = now; in game = at the next load
    assert(ui_hud_step(UI_HYBRID, UI_TOUCH, 0, 0, 1, 0) == UI_HUD_APPLY);
    assert(ui_hud_step(UI_HYBRID, UI_TOUCH, 1, 0, 1, 0) == UI_HUD_WAIT);
    assert(ui_hud_step(UI_TOUCH, UI_SWITCH, 2, 0, 0, 0) == UI_HUD_WAIT);
    // B (DM_DOS2_UI_LIVE=1): rebuilt at the request only, and only when nothing modal is up (or the host cannot tell)
    assert(ui_hud_step(UI_HYBRID, UI_SWITCH, 1, 1, 1, 0) == UI_HUD_REBUILD);
    assert(ui_hud_step(UI_HYBRID, UI_SWITCH, 1, 1, 1, 1) == UI_HUD_WAIT);
    assert(ui_hud_step(UI_HYBRID, UI_SWITCH, 1, 1, 1, -1) == UI_HUD_WAIT);
    assert(ui_hud_step(UI_HYBRID, UI_SWITCH, 1, 1, 0, 0) == UI_HUD_WAIT);     // later polls: no surprise rebuild once the modal closes
    assert(ui_hud_step(UI_HYBRID, UI_SWITCH, 0, 1, 1, 1) == UI_HUD_APPLY);    // out of game B is not needed
    // the answer for Java
    assert(ui_result_of(UI_HUD_REBUILD) == UI_RES_REBUILT && ui_result_of(UI_HUD_WAIT) == UI_RES_NEXT_LOAD && ui_result_of(UI_HUD_APPLY) == UI_RES_NEXT_LOAD);
    assert(ui_result_of(UI_HUD_NONE) == UI_RES_NEXT_LOAD);
    assert(!strcmp(ui_mode_name(UI_SWITCH), "switch") && !strcmp(ui_mode_name(UI_HYBRID), "hybrid") && !strcmp(ui_mode_name(UI_TOUCH), "touch"));
    puts("dos2_uimode_test: ok");
    return 0;
}
