// [DOS2] hle_dos2_touch_ctxdesc.c: pure rules (host-testable, dm/host/tests/dos2_touch_ctxdesc_test.c) of "Description" in an item's context
// menu in UI mode TOUCH (owner 23.11: "a tap on an item gives the context menu; the description would be better as a separate item
// 'Описание' in that menu"). The movie half is tools/dos2_pctxmenu.py (contextMenu_p.swf); it checks the numbers below against this file.
#ifndef HLE_DOS2_TOUCH_CTXDESC_H
#define HLE_DOS2_TOUCH_CTXDESC_H
#include <stdint.h>
#include <string.h>

// shared with tools/dos2_pctxmenu.py (it reads them from here)
#define CD_EI_NAME "dmCtxDesc"     // contextMenu_p -> host: dmCtxDesc(1) = "Description" was chosen (the menu has closed itself already)
#define CD_SENTINEL "dmDescOn"     // host -> contextMenu_p: setTitle(CD_SENTINEL, true) = the next fill is an item's menu: add the entry
#define CD_TIP_DEF_W 90.64         // stage px of a partyInventory_p slot (the hover's showItemTooltip w / h, phone log 23.11): a rect when none was seen
enum { CD_T_MENU = 0x0b,           // the item context menu (main+0x10cc3d0 gets it from node +0x48 in mode 0); 0x0a (node +0x18) is not an item's
       CD_T_INV = 0x74, CD_T_SHEET = 0x77, CD_T_CONT = 0x09, CD_T_PICK = 0x25,
       CD_ENTRY_ID = 31337,        // the entry's id / actionID in the menu's list (the engine's buttons are numbered from 0)
       CD_WIN_MS = 2000,           // contextMenu_p takes the sentinel only for a fill this soon after it (tools/dos2_pctxmenu.py WIN_MS)
       CD_UI_PATH = 0x20, CD_UI_FP = 0x18, CD_UI_TYPE = 0xf0, CD_UI_PLAYER = 0xf4,
       CD_UIMGR = 0x2bbb230, CD_MGR_OBJS = 0x10, CD_MGR_NOBJ = 0x1c, CD_MAXARGS = 8 };

// the windows whose openContextMenu opens the item menu 0xb and whose showItemTooltip shows that item (the game UI base main+0x10a5750)
static inline int cd_source(uint32_t type) { return type == CD_T_INV || type == CD_T_SHEET || type == CD_T_CONT || type == CD_T_PICK; }
// the hover tooltip that is NOT shown in TOUCH (the menu entry shows it): the party inventory only; the hot bar, skills, sheet, world keep theirs
static inline int cd_hover_dropped(uint32_t type, int menu_ready, int keep_hover) { return type == CD_T_INV && menu_ready && !keep_hover; }
// openContextMenu(item, x, y) / (character, item, x, y) in the PC movies: the item is third from the end; -1 = not an item call
static inline int cd_item_arg(unsigned n) { return n >= 3 && n <= 5 ? (int)n - 3 : -1; }
// a tooltip rect when the hover never showed this item (a tap opened the menu at once): a slot-sized box centred on the point of the press
static inline void cd_rect(double px, double py, double w_seen, double *x, double *y, double *w, double *h) {
    const double s = w_seen > 4 && w_seen < 400 ? w_seen : CD_TIP_DEF_W;
    *x = px - s / 2; *y = py - s / 2; *w = s; *h = s;
}
// the movie's path ends with this file name ("GUI/contextMenu_p.swf" -> "contextMenu_p.swf")
static inline int cd_path_is(const char *p, uint32_t len, const char *file) {
    const size_t f = strlen(file);
    return p && len >= f && !memcmp(p + len - f, file, f) && (len == f || p[len - f - 1] == '/');
}
// the engine's short string inline (InvokeDataValue type 5 at +8: byte (length << 1), the characters, a 0); 0 = too long (> 14)
static inline int cd_short_str(uint8_t s[0x10], const char *t) {
    const size_t n = strlen(t);
    if (n > 14) return 0;
    memset(s, 0, 0x10); s[0] = (uint8_t)(n << 1); memcpy(s + 1, t, n);
    return 1;
}
#endif
