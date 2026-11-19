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

// The video skip (the owner, 20.11: the console's "(Press B to skip)" becomes a double tap). The video plays in the subtitles UI (type 0x20,
// GUI/subtitles.swf: iggy_video + the hint hintHolder_mc). Its class object: the video base at +8, the UIObject at +0x30 (the pointer the UI
// manager and the EI hand out; ctor main+0x1194710). Video on <=> the movie name [full+0x18] is not the engine's empty name [main+0x32bd180]
// (the skip main+0x1195580 compares exactly these and resets it); the class's OnInputEvent main+0x1194d00 skips on input event 0xe7
// (SkipVideo) when [full+0x180] (skip allowed) is set: the host calls it with that one event.
enum { TG_SUB_TYPE = 0x20, TG_SUB_UI = 0x30, TG_SUB_NAME = 0x18, TG_SUB_ALLOW = 0x180, TG_SUB_UIVT = 0x29e6770, TG_SUB_INPUT = 0x1194d00,
       TG_SUB_SKIP = 0x1195580, TG_EMPTY_NAME = 0x32bd180, TG_EV_SKIPVIDEO = 0xe7, TG_DTAP_MS = 400 };
// a video plays: its name is set and is not the empty name
static inline int tg_video_on(uint64_t name, uint64_t empty) { return name != 0 && name != empty; }
// a double tap asked at req_ms (0 = none) is still due at now_ms (the game thread polls every frame; older than TG_DTAP_MS = dropped)
static inline int tg_skip_due(uint64_t req_ms, uint64_t now_ms) { return req_ms != 0 && now_ms >= req_ms && now_ms - req_ms <= TG_DTAP_MS; }
#endif
