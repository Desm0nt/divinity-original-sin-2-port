// [DOS2] hle_dos2_touch_gaps.c: pure rules (host-testable, dm/host/tests/dos2_touch_gaps_test.c). Which ExternalInterface call of a `_p`
// movie the host takes because the window's engine class has no handler for it (docs/dos2/TOUCH_MODES_PLAN.md "Implemented: iPad
// functions without a Switch EI - equivalents"; the class facts are checked against main.raw by tools/dos2_ptouch_gaps.py check).
#ifndef HLE_DOS2_TOUCH_GAPS_H
#define HLE_DOS2_TOUCH_GAPS_H
#include <stdint.h>
#include <string.h>

enum { TG_PASS = 0,          // not ours: the game's class gets the call as before
       TG_OPEN_INV = 1 };    // open the party inventory (the column's inventory button: dmSide code 4), not when it is open already
// main.raw 4979b200: OnFunctionCalled (vtable slot 0) of the playerInfo class the creator main+0x10b5f00 builds for type 0x26 (vtable
// main+0x29eb768). Its chain compares charSel / centerCamOnCharacter / piAdd* / piDetach* but not openCharInventory (only the GM
// possessionBar's main+0x1258500 does).
enum { TG_PI_TYPE = 0x26, TG_PI_CALLED = 0x1237b10, TG_UI_TYPE = 0xf0 };

// name = the EI name, type = the UI object's type ([ui+0xf0]), called = its vt[0] as an offset in main (-1 = unknown)
static inline int tg_rule(const char *name, uint32_t type, int64_t called) {
    if (!name) return TG_PASS;
    // the PC's right click on a portrait (playerInfo.onRightClick) = the iPad's long press: that character's inventory. The PC inventory
    // shows the whole party, so "open the inventory" is the whole function; the class must be the one without the handler
    if (!strcmp(name, "openCharInventory") && type == TG_PI_TYPE && called == TG_PI_CALLED) return TG_OPEN_INV;
    return TG_PASS;
}
#endif
