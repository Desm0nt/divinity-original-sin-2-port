// [DOS2] hle_dos2_touch_tips.c: pure rules (host-testable, dm/host/tests/dos2_touch_tips_test.c). UI mode TOUCH: the loading screen's tips
// that only make sense with a keyboard and a mouse are taken out of the engine's own pick, the way the iPad's Hints.lsx leaves them out of
// its TouchScreen set (docs/dos2/TOUCH_MODES_PLAN.md "Подсказки экрана загрузки в касании"; the facts below are checked against main.raw by
// tools/dos2_ptouch_tips.py check and tools/dos2_touchui_check.py).
//
// main.raw 4979b200. The table: Public/Game/Hints/Hints.lsx, loaded once at client start (main+0xd07190: [main+0x329a330] -> main+0x1110570).
// [main+0x329a330] = the hint manager: +0x10 the entries, +0x18 bits 24..47 the count. One entry is 0x38 bytes: +0 HintKey (FixedString =
// a pointer to its characters), +8 Description (TranslatedString), +0x30 Controller (byte), +0x31 Keyboard (byte) - the parser
// main+0x110fd88.. reads them with these destinations. The loading screen (type 0x17, GUI/loadingScreen.swf, class main+0x114f5f0) picks a
// random entry every HintDuration seconds in main+0x114fd40: mode byte main+0x2bb8918 != 0 -> one with Controller set, 0 (PC UI: the
// touch mode runs it) -> one with Keyboard set, never the same index twice in a row; the text goes to the movie's setInfoText (invoke
// index 1). The pick loops until it finds one: at least one Keyboard entry must stay set (tt_plan keeps that).
#ifndef HLE_DOS2_TOUCH_TIPS_H
#define HLE_DOS2_TOUCH_TIPS_H
#include <stdint.h>
#include <string.h>

enum { TT_HINTS = 0x329a330,          // the hint manager pointer
       TT_MGR_ARR = 0x10, TT_MGR_CNT = 0x18,
       TT_ENTRY = 0x38, TT_E_KEY = 0x0, TT_E_CONTROLLER = 0x30, TT_E_KEYBOARD = 0x31,
       TT_MAX = 512 };                // more entries than this = not the table we know: nothing is touched

// the count field: bits 24..47 of the qword at +0x18 (the pick's `ubfx x21, x8, #0x18, #0x18`)
static inline uint32_t tt_count(uint64_t field) { return (uint32_t)((field >> 24) & 0xffffffu); }

// HintKey values of Switch Hints.lsx whose text needs a keyboard or a mouse: a key token ([IE_...]) or a mouse-only act (cursor colour,
// edge panning, "click"). Each one either has a touch twin in our own tips (tools/dos2_ptouch_tips.py TIPS, "from") or none exists on
// touch (split stacks, queue orders, weapon on the ground: held keys; edge panning: no screen edge on a phone).
static const char *const k_tt_off[] = {
    "HINT_CAM_HOME", "HINT_CAM_ZOOM", "HINT_CAM_ROTATE", "HINT_STACKS", "HINT_TRAPS", "HINT_THEFT", "HINT_CTRL", "HINT_EDGEPANNING",
    "HINT_QUEUE", "HINT_ELEMWEAPON", "HINT_TACTICALVIEW", "HINT_OUTLINE", "HINT_PERCEPTION", "HINT_PICKPOCKET",
};
enum { TT_NOFF = (int)(sizeof k_tt_off / sizeof *k_tt_off) };

// key = the entry's HintKey characters (NULL = none): one of the keyboard-only tips
static inline int tt_off_key(const char *key) {
    if (!key) return 0;
    for (int i = 0; i < TT_NOFF; i++) if (!strncmp(key, k_tt_off[i], 48)) return 1;
    return 0;
}

// hit[i] = entry i is a keyboard-only tip, kb[i] = its Keyboard byte as loaded. Returns how many entries keep Keyboard set when every hit is
// cleared; 0 = clearing would leave the PC pick nothing (it would loop forever): the caller must not clear.
static inline uint32_t tt_plan(const uint8_t *hit, const uint8_t *kb, uint32_t n) {
    uint32_t keep = 0;
    for (uint32_t i = 0; i < n; i++) keep += kb[i] != 0 && !hit[i];
    return keep;
}
#endif
