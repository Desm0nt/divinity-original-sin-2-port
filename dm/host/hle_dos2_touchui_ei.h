// [DOS2] hle_dos2_touchui_ei.c: the engine's Flash value and the host's ExternalInterface handlers (docs/dos2/TOUCH_UI_PLAN.md TU-06)
#ifndef HLE_DOS2_TOUCHUI_EI_H
#define HLE_DOS2_TOUCHUI_EI_H
#include <stddef.h>
#include <stdint.h>
// InvokeDataValue of the Switch build, 0x40 bytes: type 3 = bool (b), 4 = number (d), 5 = string (str), 6 = wide string (wstr)
typedef struct { uint32_t type, pad; uint8_t str[0x10], wstr[0x10]; uint8_t b, pad2[7]; void *ptr; double d; } dm_ei_val;
_Static_assert(sizeof(dm_ei_val) == 0x40 && offsetof(dm_ei_val, b) == 0x28 && offsetof(dm_ei_val, d) == 0x38, "InvokeDataValue layout");
// a host handler for one ExternalInterface name: player = the FlashPlayer, ui = the UI object (the game's listener); 1 = taken (the game's class does not get the call)
typedef int (*dm_ei_fn)(void *ctx, void *player, void *ui, const char *name, unsigned nargs, const dm_ei_val *args);
int dm_dos2_touchui_ei_register(const char *name, dm_ei_fn fn, void *ctx);
int dm_dos2_touchui_ei_on(void);
void dm_dos2_touchui_ei_dump(const char *why);
const char *dm_ei_str(const dm_ei_val *v, char *buf, size_t n);                                   // a string / wide string argument as ASCII
int dm_dos2_touchui_invoke(void *player, const char *name, const dm_ei_val *args, unsigned n);   // engine -> movie by name (names the class registered)
#endif
