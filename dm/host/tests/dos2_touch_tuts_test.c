// [DOS2] Host test of hle_dos2_touch_tuts.h (UI mode touch: the tutorials about keys and the mouse are not queued).
// Run with any C compiler: cc dos2_touch_tuts_test.c -o <outside the repo>; run it.
#include "../hle_dos2_touch_tuts.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
int main(void) {
    // keyboard-only keys are left out; exact names only (no prefix match: titles, console twins and longer keys stay)
    assert(tu_off_key("TUT_Movement") && tu_off_key("TUT_Camera") && tu_off_key("TUT_SaveLoad") && tu_off_key("TUT_QuestLog_Marker"));
    assert(tu_off_key("TUT_Character_Creation_Class_Basic") && tu_off_key("TUT_HighlightCharacters"));
    assert(tu_off_key("TUT_Equipment") && tu_off_key("TUT_Locked_UseLockpick") && tu_off_key("TUT_Stealing_Noticed"));   // keys only in the shipped texts
    assert(tu_off_key("TUT_QuestLog") && !tu_off_key("TUT_QuestLog_Title") && !tu_off_key("TUT_QuestLog_Controller"));   // its console text names no button
    assert(tu_off_key("TUT_Movement_Controller") && tu_off_key("TUT_Combat_HotBar_Controller_2") && !tu_off_key("TUT_Camera_Title") && !tu_off_key("TUT_Came"));
    // tutorials that read fine on a touch screen stay
    assert(!tu_off_key("TUT_Combat") && !tu_off_key("TUT_TutorialLog") && !tu_off_key("TUT_Lockpick") && !tu_off_key("TUT_PressurePlates"));
    assert(!tu_off_key("TUT_TradeIntro") && !tu_off_key("TUT_Homestead") && !tu_off_key("") && !tu_off_key(NULL));
    assert(TU_NOFF == 89);
    for (int i = 0; i < TU_NOFF; i++) {   // every key is one the engine knows by that name, and short enough for the compare
        assert(!strncmp(k_tu_off[i], "TUT_", 4) && strlen(k_tu_off[i]) < TU_KEY_MAX);
        for (int j = 0; j < i; j++) assert(strcmp(k_tu_off[i], k_tu_off[j]));
    }
    // the hook site of main.raw 4979b200 and the entry layout
    assert(TU_INSERT == 0x126a0d0 && TU_E_KEY == 0x10 && k_tu_insert_pro[0] == 0xd10183ff && k_tu_insert_pro[3] == 0xa90357f6);
    puts("dos2_touch_tuts_test: PASS");
    return 0;
}
