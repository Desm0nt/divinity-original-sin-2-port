// [DOS2] UI mode TOUCH: no PC tutorial windows about keys and the mouse (owner 23.11: "the game keeps popping PC tutorials about keys,
// F-keys and so on; on the iPad there were none, they get in the way"). The touch mode runs the PC UI (mode byte 0), so the engine takes
// the PC text of every tutorial ("Press [IE_QuickSave] to quick save", "Click on ..."). While the HUD part of the mode is TOUCH the host
// does not let those tutorials into the tutorial box's queues: InsertTutorial (main+0x126a0d0) is hooked and returns at once for the 89
// keys of hle_dos2_touch_tuts.h k_tu_off (45 PC texts about keys and the mouse, 44 console texts about pad buttons); every other
// tutorial (lore, combat rules, items, the Tutorial Log hint) goes on as before. The facts, the hook site and the key list: hle_dos2_touch_tuts.h, checked by tools/dos2_ptutorial.py check.
// The window itself: tutorialBox_p (type 0x37 in k_touch, tools/dos2_ptutorial.py) closes a pop-up or a pointer bubble by a tap anywhere
// on it (as its X) and the modal box by a tap anywhere on it (as its OK), the iPad way.
// main = main.raw of update v786432, build 4979b200, offsets = main.raw. Static only (not run on a phone when written).
// Installed from dm_dos2_touch_tips_install (hle_dos2_touch_tips.c; main.c host_preload, before any guest code runs). The hook runs on the
// game's UI thread; it reads the mode (an atomic) and the entry's key, nothing else.
// Switch: DM_DOS2_TOUCH_TUTS=0 = no hook (the PC tutorials stay). Log: one line at install, one per tutorial left out ("[tuts] ...").
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touch_tuts.h"
#include "hle_dos2_uimode.h"
#include <stdlib.h>
#include <string.h>

int dm_dos2_ui_mode_hud(void);   // hle_dos2_uimode.c
module_t *dn_main(void);         // hle_dos2_native.c
int dn_words_ok(module_t *m, uint32_t off, const uint32_t pro[4]);
int dn_hook(module_t *m, uint32_t off, const uint32_t pro[4], void *hook, void **orig);

typedef uint64_t (*tu_insert_fn)(uint64_t box, uint64_t entry, uint64_t requeue);
static tu_insert_fn g_insert_orig;
static unsigned g_left;   // tutorials left out so far (the log line only)

// InsertTutorial(box, entry, requeue): integer arguments only (the caller AddTutorial keeps its floats in d8..d10, callee-saved)
static uint64_t tu_insert_hook(uint64_t box, uint64_t entry, uint64_t requeue) {
    if (entry && dm_dos2_ui_mode_hud() == UI_TOUCH) {
        const char *key = *(const char *const volatile *)(uintptr_t)(entry + TU_E_KEY);
        if (tu_off_key(key)) {
            if (g_left++ < 200) LOG("[tuts] UI mode touch: tutorial %s needs a keyboard or a mouse: not shown (%u so far)", key, g_left);
            return 0;   // the caller ignores the result (main+0x126b040: the next instruction releases its own copy of the entry)
        }
    }
    return g_insert_orig(box, entry, requeue);
}

void dm_dos2_touch_tuts_install(void);   // hle_dos2_touch_tips.c dm_dos2_touch_tips_install
void dm_dos2_touch_tuts_install(void) {
    const char *e = getenv("DM_DOS2_TOUCH_TUTS");
    if (e && *e == '0') { LOG("[tuts] DM_DOS2_TOUCH_TUTS=0: UI mode touch keeps the PC tutorials about keys and the mouse"); return; }
    module_t *m = dn_main();
    if (!m) return;
    if (!dn_words_ok(m, TU_INSERT, k_tu_insert_pro)) {
        LOG("【红】[tuts] main+%#x differs (InsertTutorial of build 4979b200): the PC tutorials about keys stay in UI mode touch", TU_INSERT); return; }
    // dn_hook stores the trampoline before it writes the jump: g_insert_orig is set before the hook can run
    if (dn_hook(m, TU_INSERT, k_tu_insert_pro, (void *)tu_insert_hook, (void **)(void *)&g_insert_orig)) {
        LOG("【红】[tuts] InsertTutorial main+%#x: mmap / mprotect failed, no hook: the PC tutorials about keys stay", TU_INSERT); return; }
    LOG("[tuts] InsertTutorial main+%#x hooked: in UI mode touch the %d tutorials about keys and the mouse are not shown; DM_DOS2_TOUCH_TUTS=0 keeps them",
        TU_INSERT, TU_NOFF);
}
#endif
