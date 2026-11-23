// [DOS2] Host test of hle_dos2_touch_ctxdesc.h (UI mode touch: "Description" in the item context menu shows the item's tooltip).
// Run with any C compiler: cc dos2_touch_ctxdesc_test.c -o <outside the repo>; run it.
#include "../hle_dos2_touch_ctxdesc.h"
#include "../hle_dos2_touchui_ei.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
int main(void) {
    // the item windows of the PC set whose openContextMenu fills the item menu 0xb; not the hot bar, skills, world, the menu itself
    assert(cd_source(0x74) && cd_source(0x77) && cd_source(0x09) && cd_source(0x25));
    assert(!cd_source(0x28) && !cd_source(0x29) && !cd_source(0x0b) && !cd_source(0x0a) && !cd_source(0x2c));
    // the hover tooltip dropped: the party inventory only, only while its menu offers Description, never with DM_DOS2_TOUCH_CTXDESC=2
    assert(cd_hover_dropped(0x74, 1, 0) && !cd_hover_dropped(0x74, 0, 0) && !cd_hover_dropped(0x74, 1, 1));
    assert(!cd_hover_dropped(0x77, 1, 0) && !cd_hover_dropped(0x09, 1, 0) && !cd_hover_dropped(0x28, 1, 0));
    // openContextMenu(item, x, y) (containers, sheet) / (character, item, x, y) (party inventory): the item is third from the end
    assert(cd_item_arg(3) == 0 && cd_item_arg(4) == 1 && cd_item_arg(5) == 2 && cd_item_arg(2) == -1 && cd_item_arg(0) == -1 && cd_item_arg(6) == -1);
    // the rect without a hover: the slot size seen in that window, else the partyInventory_p slot; centred on the press
    double x, y, w, h;
    cd_rect(1582.7, 614.6, 90.64, &x, &y, &w, &h); assert(w == 90.64 && h == 90.64 && x == 1582.7 - 45.32 && y == 614.6 - 45.32);
    cd_rect(100, 200, 0, &x, &y, &w, &h); assert(w == CD_TIP_DEF_W && x == 100 - CD_TIP_DEF_W / 2);
    cd_rect(100, 200, 5000, &x, &y, &w, &h); assert(w == CD_TIP_DEF_W);
    // the movie of the menu object
    assert(cd_path_is("Public/Game/GUI/contextMenu_p.swf", 33, "contextMenu_p.swf") && cd_path_is("contextMenu_p.swf", 17, "contextMenu_p.swf"));
    assert(!cd_path_is("Public/Game/GUI/contextMenu.swf", 31, "contextMenu_p.swf") && !cd_path_is("GUI/xcontextMenu_p.swf", 22, "contextMenu_p.swf"));
    assert(!cd_path_is(NULL, 0, "contextMenu_p.swf") && !cd_path_is("x.swf", 5, "contextMenu_p.swf"));
    // the engine's inline short string (InvokeDataValue type 5 at +8): what main+0xd27568 reads (byte bit 0 clear -> characters from +9)
    dm_ei_val v; memset(&v, 0xee, sizeof v); v.type = 5;
    assert(cd_short_str(v.str, CD_SENTINEL) && v.str[0] == 2 * strlen(CD_SENTINEL) && !(v.str[0] & 1) && !strcmp((const char *)v.str + 1, CD_SENTINEL));
    assert(cd_short_str(v.str, "rightTop") && v.str[0] == 16 && !memcmp(v.str + 1, "rightTop", 9));
    assert(cd_short_str(v.str, "12345678901234") && !cd_short_str(v.str, "123456789012345"));
    assert(strlen(CD_SENTINEL) <= 14 && !strcmp(CD_EI_NAME, "dmCtxDesc") && CD_T_MENU == 0x0b && CD_ENTRY_ID > 255 && CD_ENTRY_ID < 32768);
    puts("dos2_touch_ctxdesc_test: PASS");
    return 0;
}
