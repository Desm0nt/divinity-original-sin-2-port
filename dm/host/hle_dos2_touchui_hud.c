// [DOS2] The touch HUD in game: the host creates the PC HUD classes when a player UI set is built with the mode byte 0 (UI mode TOUCH,
// docs/dos2/TOUCH_MODES_PLAN.md "Implemented: touch HUD"; first written as TU-07 of docs/dos2/TOUCH_UI_PLAN.md). Static only (not run on a phone
// when written). main = main.raw of update v786432, build 4979b200, offsets = main.raw; iPad 3.6.72 addresses = Mach-O VA.
// The iPad's per-player UI set builder 0x1012dfea4 has three branches by the mode it stores in the player's record (+0x348): 0 = keyboard and
// mouse, 1 = controller, 2 = touch. Its touch branch (0x1012e0268, creations out of line from 0x1012e0b48) makes, with
// CreateUIObject(mgr, layer, type, flags | the builder's flags, 0x80, player):
//     statusConsole_t 0x98  layer 6     flags 0x201e0      hotBar_t 0x97          layer 0x10  flags 0x201e0
//     minimap_t       0x94  layer 2     flags 0x221e0      playerInfo_t 0x96      layer 0xf   flags 0x201e0
//     combatLog_t     0xce  layer 0xa   flags 0x201e0      rightSideButtons 0x99  layer 0x12  flags 0x201e0   journal_csp_t 0x95 layer 0xf
//     character switcher 0xa8 layer 0x16, magnifier 0xcc layer 0 flags 0x261e0, panels 0x9b..0x9e / 0xa7 layer 0xa (0x9b: 0xc), then the shared
//     tail with touch ids (gameMenu_t 0xca layer 0x1f, contextMenu_t 0x9f / 0xa0 layer 0x16, msgBox_t 0xae layer 0x22, examine_t 0xb4 layer 0xd,
//     tutorialBox_t 0xb9, textDisplay_t 0xaa layer 9, worldTooltip_t 0xbe layer 0, overhead_t 0xbb layer 1, notification_t 0xcd layer 0x16).
//   Its PC branch (0x1012e0118) for comparison: statusConsole 0x75 layer 6, hotBar 0x28 layer 0xf, minimap 0x1e layer 2, combatLog 7 layer 0xa,
//   playerInfo 0x26 layer 7, all flags 0x201e0. The touch objects have record slots of their own (+0x2b8..+0x338) that the Switch record lacks.
// The Switch builder 0x10c06d0 (record = hash node of [main+0x3298140]+0xf0; mode byte main+0x2bb8918 read at 0x10c07ac): mode 1 = the `_c`
//   block 0x10c07f4.. (bottomBar_c 0x3b layer 2 flags 0x261e0 -> record +0x58, 0x10c07fc..0x10c0818; minimap_c 0x3c -> +0x50; combatLog_c 0x41
//   -> +0x68 ...), any other mode = only the shared tail 0x10c0b30 (worldTooltip 0x30, overhead 5, 0xf, 0, notification 0x24, 0x25, 0x10, 9,
//   journal_c 0x46). A slot is only filled when it is 0, nothing else is done with a new object (no Show call): the objects run themselves.
//   No CreateUIObject call for the PC HUD types exists in main, but their classes are registered (hotBar 0x28 creator 0x10b60d0 ...).
// So the host makes the HUD itself, with the PC classes (hle_dos2_touchui_hud.h holds the pure rules):
//     type 0x28 hotBar, 0x1e minimap, 0x26 playerInfo, 0x75 statusConsole, 7 combatLog: layers and flags of the iPad's touch branch (table above),
//     flags | 0x20000000 (what create-all passes); and 0x8d (watermark.swf's type, a plain UIObject; layer 0x12 = the iPad's rightSideButtons_t,
//     flags of the hybrid's column) ONLY when its creator plays GUI/dmSide_p.swf - the iPad's right side: options, the helmet, the panel column.
//   The movies are DATA of hle_dos2_touchui.c: its table `touch` (k_touch) maps these types to the `<name>_p.swf` movies of TouchUI.pak
//   (tools/dos2_pchud_touch.py: the player's own PC movies laid out as the iPad's HUD); without the package file a type plays the player's
//   stock PC movie. The objects are created through CreateUIObject (that hook logs them, watches them, and falls back to the stock movie when a
//   substituted one cannot be made). An iPad `_t` movie for an experiment: DM_DOS2_UI_TOUCH_MAP=28:hotBar_t,... (the old `_t` default is gone).
//   The handles are the host's (the Switch record has no slot for them); they are destroyed the way the game destroys a slot
//   (0x10bc7dc..0x10bc864: handle -> object through [mgr+0x10] / [mgr+0x1c] / [mgr+0x30], `obj->vt[0xf0 / 8](obj)`, DestroyUIObject
//   0xaeba10(mgr, &handle), then the message {vtable main+0x2999ec0, u32 1, obj} to 0xa47030([main+0x2bc0530], &msg)).
// When. Active = UI mode TOUCH (hle_dos2_uimode.c: requested, or its HUD part still running) or DM_DOS2_TOUCHUI_HUD=1 (any mode);
//   DM_DOS2_TOUCHUI_HUD=0 = never. Two hooks, create-all 0x10c4580 and destroy-all 0x10c4730 (the game's load / unload, the UI mode switch's
//   rebuild and DM_DOS2_TOUCH_LIVE's pad <-> touch switch all go through them):
//     before destroy-all: the host's objects go first; after create-all: with the mode byte 0 they are made for every player record.
//   And every pad poll (dm_dos2_touchui_hud_poll from hle_dos2_uimode.c): a set that exists with the byte 0 and no host HUD gets one (a player
//   added in the middle of a session through 0x10bafe0, or a set built before the hooks were in); a host HUD whose sets are gone is destroyed.
//   INSTALL ORDER (the TU-07 bug, docs/dos2/TOUCH_CONSOLE_PLAN.md "Почему TU-07 не переключил HUD": this file wrote its jumps over the two
//   prologues BEFORE hle_dos2_touch.c compared the same words for its live rebuild -> "main+0x10c4580 differs", no live switch): the install is
//   in two steps now. dm_dos2_touchui_hud_install (before dm_dos2_touch_install) only compares the sites and reads the switches;
//   dm_dos2_touchui_hud_hook (main.c, AFTER dm_dos2_touch_install) writes the jumps. Both users coexist: hle_dos2_touch.c calls the two
//   functions by address, so its rebuild runs through these hooks.
//   The gate of a live switch (dm_dos2_touchui_hud_gate, asked by hle_dos2_touch.c before every rebuild): not while a dialog / loading screen /
//   character creation is on screen (DM_DOS2_TOUCHUI_HUD_BLOCK=<hex,..>); the game state u32 [[[[main+0x3298150]+0x30]]+0x10] is logged with
//   every switch and only tested when DM_DOS2_TOUCHUI_HUD_STATES=<hex,..> names the allowed ids (0x13 = Running is a static guess, never run).
//   When the game destroys the sets itself (unload) in UI mode TOUCH the byte stays 0 (the menus are PC ones there); with the old switch
//   alone it goes back to 1 as before.
// The PC PANELS (stage 4, "Implemented: touch panels"). The player's record is the same on both builds: the iPad's node offsets are the
//   Switch's (checked on shared slots: bottomBar_c 0x3b node +0x68, contextMenu_c 0xc +0x50, worldTooltip 0x30 +0xd8, overhead 5 +0xb8; the
//   iPad only appends its touch slots +0x2b8.. after the Switch's last one). The iPad's PC branch 0x1012e0118..0x1012e0658 fills, each only
//   when the slot is 0 (CreateUIObject(mgr, layer, type, flags | builder flags, 0x80, player) -> str x0,[node,#off]):
//     statusConsole 0x75 +0x28 layer 6     hotBar 0x28 +0x20 layer 0xf    minimap 0x1e +0x58 layer 2      enemyHealthBar 0x2a +0x30 layer 2 flags 0x20000
//     combatLog 7 +0x70 layer 0xa          playerInfo 0x26 +0xd0 layer 7  (all flags 0x201e0 unless said)
//     msgBox 0x1d +0x108 layer 0x28 flags 0x21fe0      gameMenu 0x13 +0x88 layer 0x1f flags 0x61fe0     journal 0x16 +0x138 layer 0xa flags 0x400201e0
//     tutorialBox 0x37 +0x160 layer 0x16 flags 0x24000   examine 0x68 +0xa8 layer 0xa     contextMenu 0xa +0x18 / 0xb +0x48 layer 0x16 flags 0x21fe0
//     uiCraft 0x66 +0x170, partyInventory 0x74 +0x228, characterSheet 0x77 +0x238, skills 0x29 +0x258: layer 0xa
//   The Switch builder makes none of these (mode 1: the `_c` block, else only the shared tail), but its PC code READS the slots: the panel
//   toggles the PC hot bar's buttons end in (hotbarBtnPressed ids 0..5) look their window up there and do nothing when the handle is 0 -
//   skills 0x10c70a0: `ldr x9,[x22,#0x248]` (x22 = node + 0x10) then `cmp [obj+0xf0],#0x29` (0x10c71cc / 0x10c722c); partyInventory
//   0x10c82c4 `ldr x10,[x8,#0x228]`, characterSheet 0x10c8380 +0x238, uiCraft 0x10d5620 +0x170, the hot bar itself 0x10c6638 +0x20.
//   So the host fills the PC branch the way the iPad's builder does: every type of the table, created when its slot is 0, the handle written
//   into the slot. From then on the object is the engine's (found by its toggles / updates). The host still destroys what it made BEFORE the
//   game's destroy-all and zeroes the slot it filled (a handle the game already destroyed fails the salt check and is skipped).
//   DM_DOS2_TOUCHUI_HUD_SLOTS=0: the handles are not written into the record (the stage 3 behaviour: a HUD the engine cannot find).
//   Windows the game makes on demand in mode 0 by itself (trade 0x2e, containerInventory 9 / 0x25, waypoints 0x2f, dialog 0xe, saveLoad, options,
//   itemSplitter ...: the `csel` on the mode byte at their creation sites, docs/dos2/TOUCH_UI_PLAN.md section 3) need nothing here.
// Size. The engine's "fixed" layout, main+0xae3be0(obj, w, h) branch 0xae3c8c: k = c x S x h / D with c = float [obj+0x94] (the class scale,
//   set by 0xae4c40(obj, s0), which also writes the movie's root.uiScaling = c x S), S = float [[main+0x2bb8a60]+0x34] (the "UI scale"
//   setting; applied in every input mode here, the iPad applies it only off the pad), D = u32 [uimgr+0x110] = 1080 (the UI manager's
//   constructor 0xae7438 writes 0x438 there; so the "D ~ 830" guess of TOUCH_LAYOUT.md was c x S of minimap_c: its class takes c from
//   [cfg+0x3c..0x44]). The host gives each object it makes c = DM_DOS2_TOUCH_HUD_SCALE x (iPad class scale: 1.1 portraits, else 1) / S and
//   runs the engine's own "resize to the window" 0xae3880(obj): one stage px = DM_DOS2_TOUCH_HUD_SCALE screen px on a 1080-high screen.
// ExternalInterface names of the `_p` movies (taken here):
//   dmSide(code)     dmSide_p / minimap_p: 1 = options -> the escape key (PC: ToggleInGameMenu, needs Eoc/keyboard_1.json = UI mode TOUCH's
//                    bindings); 2..7 = map, character, inventory, skills, crafting, journal -> "hotbarBtnPressed"(PC id) to the player's PC
//                    hot bar object: its class's own handler main+0x122fdb0 (ids 0..5 -> the panel toggles 0x10c6360 / 0x10d6240 / 0x10c70a0 /
//                    0x10d55c0 / 0x10cc7c0 / 0x10ccb40 of the player UI manager): what a click on the PC hot bar's own button does.
//   dmAim(0 | 1)     hotBar_p: the engine marks / unmarks a prepared skill (showActiveSkill). dm_dos2_touchui_hud_aiming() tells the gestures.
//   dmAimBtn(1 | 0)  hotBar_p: CONFIRM = the PC's left click at the aimed point, cancel = the PC's right click there (PC: right click drops a
//                    prepared skill). The aimed point = dm_dos2_touchui_hud_aim_at(x, y) when the gestures call it, else the last place the
//                    cursor rested before the finger went to the button. Sent as mouse records through InjectInput on the next polls.
//   dmKey(code)      minimap_p: 1 = quick save -> the f5 key (PC: QuickSave <- key:f5), 2 = the tactical camera -> the o key (ToggleTacticalCamera
//                    <- key:o); raw key ids from the engine's name table main+0x2974b10 (index - 2: f5 0x42, o 0x32, as hle_dos2_touch.c's).
// The ACTION PLATE (the iPad's overhead_t action button; "Implemented: touch HUD extras"). The iPad's own route - the overhead class's
//   clickedAction - is not in the Switch main, and the PC acts on the first click. So the host does it: hle_dos2_touch.c asks
//   dm_dos2_touchui_hud_plate(x, y, object under the finger) on a world tap in UI mode TOUCH. With a character / item there (the picking of
//   hle_dos2_touch_world.c) the tap does not click: the cursor stays on the object (the PC's own hover shows its name / highlight) and the host
//   invokes hotBar_p's showActiveSkill(2000000 + stageX * 2000 + stageY): the CONFIRM circle appears above the object. A tap on it =
//   dmAimBtn(1) = the PC's left click at that point (open / talk / attack / pick up - whatever the PC click does). A tap on the ground walks
//   at once and hides the plate. The plate carries no action name (the engine's text for it lives in the missing touch class).
//   DM_DOS2_TOUCH_T_PLATE=0: off, a tap on an object acts at once as the PC click.
//   Who names the object (DM_DOS2_TOUCH_T_PICK=auto|pc|hover|hybrid|long, rules in hle_dos2_touchui_hud.h hu_pick_tap): `pc` = the pick
//   hle_dos2_touch_world.c noted while the finger was down; `hover` = the engine's own PC pick now ([PlayerPickingHelper+0x80] after its ray
//   set-up main+0xfa9f50 ran with the mode byte 0 - there the helper casts from the UI cursor itself, 0xfa9fdc: this IS what the PC hover
//   shows); `long` = no picking: a tap clicks, the short long press (the tooltip hold) puts the plate up at the finger. `auto` asks pc, then
//   hover, and goes to `long` when the picking proves dead (the ray set-up never runs: 3 taps; or 40 world taps and never an object), one
//   log line; a later hit revives it. `hybrid` = pc + hover without `long`: in mode 0 the hybrid's picking trick is the PC pick itself.
// The pad is never turned off by the host; in the PC set the GAME drops the pad's records (its filter 0x844014), hle_dos2_touch.c sees them in
// the poll and switches back (its live switch).
// Switches (need DM_DOS2_TOUCHUI=1 with the package; players use the gear's UI mode):
//   DM_DOS2_TOUCHUI_HUD=1 | 0        1: the PC HUD whenever a set is built with the byte 0, in any UI mode (turns DM_DOS2_TOUCH_LIVE=1 on when
//                                    it is unset, as before); 0: never, also in UI mode TOUCH
//   DM_DOS2_TOUCHUI_HUD_SET=<hex,..> only these types (default: the whole table below): to find the one that breaks
//   DM_DOS2_TOUCHUI_HUD_SLOTS=0      the handles are not written into the player's record (panels then cannot be opened)
//   DM_DOS2_TOUCH_HUD_SCALE=<f>      the one size factor, default 1 (0.5..2); 0 = class scales left to the engine
//   DM_DOS2_TOUCHUI_HUD_SHOW=1       the host also sets the visible flag on every object it makes (if the PC classes do not show themselves)
//   DM_DOS2_TOUCHUI_HUD_STATES / _BLOCK  see above
//   DM_DBG=1                         every switch asked / refused (state, blocking type), every object made or destroyed, timing, EI calls
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touchui_ei.h"
#include "hle_dos2_touchui_hud.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

enum { HU_CREATE_ALL = 0x10c4580, HU_DESTROY_ALL = 0x10c4730, HU_SLOT_DESTROY = 0x10bc7dc, HU_DESTROY_OBJ = 0xaeba10, HU_MSG_SEND = 0xa47030, HU_MSG_VT = 0x2999ec0,
       HU_MSG_TO = 0x2bc0530, HU_STATE_RD = 0x10c0764, HU_STATE = 0x3298150, HU_C_SITE = 0x10c07fc, HU_LOOKUP = 0xaebc10, HU_CREATE = 0xaebbf0, HU_UIMGR = 0x2bbb230,
       HU_UI_THIS = 0x3298140, HU_MODE = 0x2bb8918, HU_MODE_RD = 0x10c07ac, HU_VIS_RD = 0x10c0d30,
       HU_HASH_N = 0xd8, HU_HASH_B = 0xe0, HU_OBJS = 0x10, HU_OBJS_N = 0x1c, HU_SALTS = 0x30, HU_OBJ_FLAGS = 0x14, HU_OBJ_TYPE = 0xf0, HU_UI_HASH = 0xf0 };
static const uint32_t k_hu_create_all[4] = {0xd101c3ff /* sub sp,sp,#0x70 */, 0xa90457f6, 0xa9054ff4, 0xa9067bfd};
static const uint32_t k_hu_destroy_all[4] = {0xd10103ff /* sub sp,sp,#0x40 */, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd};
static const uint32_t k_hu_slot_destroy[35] = {0xf940b689 /* ldr x9,[x20,#0x168]: a slot */, 0xb4000449, 0x9105a299, 0xf260553f /* tst x9,#0x3fffff00000000 */, 0x540003c0,
    0xb9401eca /* ldr w10,[x22,#0x1c]: objects */, 0x92407d28, 0xeb0a011f, 0x54000342, 0xf9401aca /* ldr x10,[x22,#0x30]: salts */, 0xb868794a, 0xd360d529, 0x6b0a013f, 0x540002a1,
    0xf9400ac9 /* ldr x9,[x22,#0x10] */, 0xf868793a /* the object */, 0xb400025a, 0xf9400348, 0xf9407908 /* ldr x8,[x8,#0xf0] */, 0xaa1a03e0, 0xd63f0100 /* blr x8 */,
    0xaa1603e0, 0xaa1903e1 /* x1 = &slot */, 0x97e8bc76 /* bl 0xaeba10 */, 0x320003e8, 0xb90013e8 /* u32 1 */, 0xb000c6e8, 0x913b0108 /* main+0x2999ec0 */, 0xf90007e8,
    0x9000d828, 0xf9429900 /* ldr x0,[main+0x2bc0530] */, 0x910023e1, 0xf9000ffa /* the object */, 0x97e629f4 /* bl 0xa47030 */, 0xf900033f /* slot = 0 */};
static const uint32_t k_hu_destroy_obj[8] = {0xf81c0ff7, 0xa90157f6, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0xf9400035 /* ldr x21,[x1]: the handle */, 0xf26056bf, 0x54000c80};
static const uint32_t k_hu_msg_send[4] = {0xd10103ff, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
static const uint32_t k_hu_state_rd[10] = {0x90010edc /* adrp x28,main+0x3298000 */, 0xf940ab88 /* ldr x8,[x28,#0x150] */, 0xf9401908 /* ldr x8,[x8,#0x30] */, 0xf9400108 /* ldr x8,[x8] */,
    0xf000d7c9, 0xf9410929, 0x39404129, 0xb9401108 /* ldr w8,[x8,#0x10]: the state */, 0x7100791f /* cmp w8,#0x1e */, 0x1a9f17e8};
static const uint32_t k_hu_c_site[8] = {0x79401be5 /* ldrh w5,[sp,#0xc]: player */, 0x2a1b02a3 /* orr w3,w21,w27 */, 0x52800762 /* mov w2,#0x3b */, 0xaa1403e0, 0x321f03e1 /* mov w1,#2 */,
    0x321903e4 /* mov w4,#0x80 */, 0x97e8acf7 /* bl 0xaebbf0 */, 0xf9002ec0 /* str x0,[x22,#0x58] */};
static const uint32_t k_hu_lookup[6] = {0xb940d808, 0x1ac80849, 0x1b088928, 0xf9407009, 0xf8685928, 0xb4002aa8};   // CreateUIObject's type lookup (its prologue is hooked by hle_dos2_touchui.c)
static const uint32_t k_hu_mode_rd[2] = {0x39646118 /* ldrb w24,[x8,#0x918] */, 0xf9406ec8};
static const uint32_t k_hu_vis_rd[2] = {0x39405108 /* ldrb w8,[x8,#0x14] */, 0x361fd368 /* tbz w8,#3: not visible */};

// the size: class scale setter, "resize to the window", the fixed branch of the resize (D, S, c), the UI manager's D = 1080
enum { HU_SETSCALE = 0xae4c40, HU_SETSCALE_ST = 0xae4c64, HU_RESIZE_NOW = 0xae3880, HU_FIXED = 0xae3c8c, HU_CTOR_D = 0xae7438, HU_CFG = 0x2bb8a60, HU_CFG_S = 0x34, HU_MGR_D = 0x110,
       HU_OBJ_C = 0x94, HU_OBJ_FP = 0x18, HU_OBJ_W = 0x54, HU_SETFLAGS_FN = 0xae4110, HU_CLRFLAGS_FN = 0xae3f10, HU_OBJ_PLAYER = 0xf4,
       // the PC hot bar class: OnFunctionCalled, its "hotbarBtnPressed" compare, the id switch and its table
       HU_HB_CALLED = 0x122fdb0, HU_HB_BTN = 0x122fe98, HU_HB_SWITCH = 0x12308e0, HU_HB_TABLE = 0x1f52e1e,
       HU_INJECT = 0x847690, HU_INMGR = 0x2bb8960, HU_CURSOR = 0x2bb88e8, HU_IN_DEVS = 0x18, HU_IN_DEVN = 0x24,
       HU_T_HOTBAR = 0x28, HU_T_SIDE = 0x8d, HU_F_VISIBLE = 8,
       // the record's PC slots as the Switch code reads them (node-relative; the skills toggle uses x22 = node + 0x10)
       HU_SLOT_SKILLS = 0x10c71cc, HU_SLOT_SKILLS_T = 0x10c7228, HU_SLOT_INV = 0x10c82c4, HU_SLOT_SHEET = 0x10c8380, HU_SLOT_CRAFT = 0x10d5620, HU_SLOT_HOTBAR = 0x10c6638 };
static const uint32_t k_hu_slot_skills[2] = {0xf94126c9 /* ldr x9,[x22,#0x248]: node + 0x258 */, 0xf260553f /* tst x9,#0x3fffff00000000 */};
static const uint32_t k_hu_slot_skills_t[2] = {0xb940f2a8 /* ldr w8,[x21,#0xf0] */, 0x7100a51f /* cmp w8,#0x29 */};
static const uint32_t k_hu_slot_inv[2] = {0xf941150a /* ldr x10,[x8,#0x228] */, 0xf260555f};
static const uint32_t k_hu_slot_sheet[2] = {0xf9411d0a /* ldr x10,[x8,#0x238] */, 0xf260555f};
static const uint32_t k_hu_slot_craft[2] = {0xf940b90a /* ldr x10,[x8,#0x170] */, 0xf260555f};
static const uint32_t k_hu_slot_hotbar[2] = {0xf940110a /* ldr x10,[x8,#0x20] */, 0xf260555f};
static const uint32_t k_hu_setscale[8] = {0xd10183ff, 0xfd0023e8, 0xf90027f3, 0xa9057bfd, 0x910143fd, 0xaa0003f3, 0xf9400c00 /* ldr x0,[x0,#0x18]: the player */, 0x4ea01c08 /* v8 = s0 */};
static const uint32_t k_hu_setscale_st[1] = {0xbd009668 /* 0xae4c64 str s8,[x19,#0x94] */};
static const uint32_t k_hu_resize_now[9] = {0xb00106e8, 0x900106c9, 0xf9415d08, 0xf9413129, 0xb9805108, 0xf9402129, 0xf8687928, 0x295b0901 /* ldp w1,w2,[x8,#0xd8]: the window */,
                                            0x140000d0 /* b 0xae3be0 */};
static const uint32_t k_hu_fixed[10] = {0x900106c8, 0xf9411908 /* the UI manager */, 0xbd411100 /* ldr s0,[x8,#0x110]: D */, 0xb00106a8, 0xf9453108 /* [main+0x2bb8a60] */,
                                        0xbd403509 /* ldr s9,[x8,#0x34]: S */, 0x7e21d800 /* ucvtf */, 0x1e201820 /* h / D */, 0xbd409661 /* ldr s1,[x19,#0x94]: c */, 0x1e200828};
static const uint32_t k_hu_ctor_d[3] = {0xd2808709 /* mov x9,#0x438 */, 0xf2c0f009 /* movk x9,#0x780,lsl #32 */, 0xf9008a89 /* str x9,[x20,#0x110] */};
static const uint32_t k_hu_setflags_fn[3] = {0xa9be4ff4, 0xa9017bfd, 0x910043fd};
static const uint32_t k_hu_clrflags_fn[3] = {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd};
static const uint32_t k_hu_hb_called[12] = {0xd105c3ff, 0xfd0083e8, 0xa9116ffc, 0xa91267fa, 0xa9135ff8, 0xa91457f6, 0xa9154ff4, 0xa9167bfd, 0x910583fd, 0xaa0103f6 /* x22 = name */,
                                            0xaa0003f3 /* x19 = this */, 0xf00060c1};
static const uint32_t k_hu_hb_btn[5] = {0xd0006481 /* adrp x1,main+0x1ec1000 */, 0x9116e421 /* add x1,x1,#0x5b9: "hotbarBtnPressed" */, 0xaa1603e0, 0x942711d7 /* bl strcmp */, 0x34004f20};
static const uint32_t k_hu_hb_switch[5] = {0x1e780108 /* fcvtzs w8,d8: args[0].d */, 0x7100151f /* cmp w8,#5 */, 0x54ffb688, 0xd0006909, 0x91387929 /* main+0x1f52e1e */};
static const uint16_t k_hu_hb_table[6] = {0x0, 0x1e5, 0x1c8, 0x1d6, 0x1ba, 0x1f2};   // branch offsets / 4 from 0x1230904 for the ids 0..5

typedef struct { uint32_t off, n; const uint32_t *want; } hu_site;
typedef struct { uint32_t cap1, len; char *ptr; } hu_str;
typedef struct { void *fn; hu_str path; } hu_creator;
typedef struct hu_node { struct hu_node *next; uint32_t id, pad; hu_creator *cr; } hu_node;
typedef uint64_t (*hu_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef struct { uint32_t raw; float v[2]; uint8_t pressed, pad[3], flag, pad2[3]; } hu_rec;   // InjectInputData (hle_dos2_touch.c dt_rec)
_Static_assert(sizeof(hu_rec) == 20, "InjectInputData is 20 bytes");

enum { HU_NTYPES = 18, HU_MAXOBJ = 80 };
// type, layer, flags, the record's slot (node offset, 0 = none: the host keeps the handle), the iPad's class scale of the movie (0 = the size is
// left to the engine: not a "fixed" HUD / panel movie), the movie the type must play to be made at all
static struct { uint32_t type, layer, flags, slot; float ipad_c; const char *need; int on, logged; } g_t[HU_NTYPES] = {
    {0x75, 6, 0x201e0, 0x28, 1.0f, NULL, 1, 0},       // statusConsole
    {0x1e, 2, 0x221e0, 0x58, 1.0f, NULL, 1, 0},       // minimap (flags of the iPad's touch branch)
    {0x26, 0xf, 0x201e0, 0xd0, 1.1f, NULL, 1, 0},     // playerInfo: the iPad's class scale of playerInfo_t (docs/dos2/TOUCH_LAYOUT.md section 1)
    {0x28, 0x10, 0x201e0, 0x20, 1.0f, NULL, 1, 0},    // hotBar
    {7, 0xa, 0x201e0, 0x70, 1.0f, NULL, 1, 0},        // combatLog
    {HU_T_SIDE, 0x12, 0x261e0, 0, 1.0f, "GUI/dmSide_p.swf", 1, 0},   // only with this movie: the stock watermark.swf is never shown
    {0x2a, 2, 0x20000, 0x30, 0, NULL, 1, 0},          // enemyHealthBar
    {0x1d, 0x28, 0x21fe0, 0x108, 0, NULL, 1, 0},      // msgBox
    {0x13, 0x1f, 0x61fe0, 0x88, 0, NULL, 1, 0},       // gameMenu: what the options button's escape (ToggleInGameMenu) opens
    {0x16, 0xa, 0x400201e0, 0x138, 1.0f, NULL, 1, 0}, // journal
    {0x37, 0x16, 0x24000, 0x160, 0, NULL, 1, 0},      // tutorialBox
    {0x68, 0xa, 0x201e0, 0xa8, 1.28f, NULL, 1, 0},    // examine: the iPad's window is 620 wide for the PC's 484
    {0x0a, 0x16, 0x21fe0, 0x18, 1.6f, NULL, 1, 0},    // contextMenu: contextMenu_t scales its list 1.6
    {0x0b, 0x16, 0x21fe0, 0x48, 1.6f, NULL, 1, 0},
    {0x66, 0xa, 0x201e0, 0x170, 1.0f, NULL, 1, 0},    // uiCraft
    {0x74, 0xa, 0x201e0, 0x228, 1.0f, NULL, 1, 0},    // partyInventory
    {0x77, 0xa, 0x201e0, 0x238, 1.0f, NULL, 1, 0},    // characterSheet (the PC's equipment doll is in it)
    {0x29, 0xa, 0x201e0, 0x258, 1.0f, NULL, 1, 0}};   // skills
static struct { uint64_t h; uint32_t type; uint16_t player; uint8_t *slot; } g_obj[HU_MAXOBJ];
static int g_nobj, g_env = -1, g_sites, g_hooked, g_dbg, g_switching, g_any_state = 1, g_tried, g_show, g_scale_ok, g_flags_ok, g_btn_ok, g_mdev = 1, g_slots = 1, g_slots_ok;
static float g_tune = 1.0f;
static uint32_t g_states[8] = {0x13}, g_block[16] = {0xe, 0x42, 0x65, 0x17, 3, 4};
static int g_nstates = 1, g_nblock = 6;
static unsigned g_nswitch, g_nrefused, g_nside, g_naim;
static module_t *g_main;
static hu_fn g_create_all_orig, g_destroy_all_orig;
static _Atomic int g_aim;                 // a skill is prepared (hotBar_p's dmAim)
static hu_aim g_aimp;                     // the aimed point (game thread)
static _Atomic int g_aim_set; static _Atomic uint64_t g_aim_xy;   // dm_dos2_touchui_hud_aim_at from another thread: two floats
static struct { int kind, step; float x, y; uint64_t t; uint32_t key; } g_act;   // a host click on its way: kind 1 left, 2 right, 3 a key (raw id in key)
static int g_plate = 1, g_plate_on, g_pick_mode;   // the action plate: enabled, shown now, DM_DOS2_TOUCH_T_PICK
static hu_pick g_pick;

int dm_dos2_ui_mode(void); int dm_dos2_ui_mode_hud(void);   // hle_dos2_uimode.c
int dm_dos2_touch_on(void);                                 // hle_dos2_touch.c
int dm_dos2_touchui_may_press(const void *ui);              // hle_dos2_touchui.c
static int hu_on(void) { return g_hooked && hu_active(g_env, dm_dos2_ui_mode(), dm_dos2_ui_mode_hud()); }

static uint64_t hu_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull; }
static uint8_t *hu_mgr(void) { return *(uint8_t *volatile *)(g_main->base + HU_UIMGR); }
static volatile uint8_t *hu_mode(void) { return (volatile uint8_t *)(g_main->base + HU_MODE); }
static int hu_state(void) {   // the game state id, -1 = no state machine yet
    uint8_t *p = *(uint8_t *volatile *)(g_main->base + HU_STATE); if (!p) return -1;
    uint8_t **q = *(uint8_t **volatile *)(p + 0x30); if (!q) return -1;
    uint8_t *r = *q; if (!r) return -1;
    return (int)*(volatile uint32_t *)(r + 0x10);
}
static uint8_t *hu_obj(uint8_t *mgr, uint64_t h) {   // handle -> UI object, the game's own checks (main+0x10bc7e8..0x10bc81c)
    if (!(h & 0x3fffff00000000ull)) return NULL;
    const uint64_t idx = h & 0xffffffffull;
    if (idx >= *(volatile uint32_t *)(mgr + HU_OBJS_N)) return NULL;
    const uint32_t *salt = *(const uint32_t *volatile *)(mgr + HU_SALTS); uint8_t **objs = *(uint8_t **volatile *)(mgr + HU_OBJS);
    if (!salt || !objs || salt[idx] != (uint32_t)((h >> 32) & 0x3fffff)) return NULL;
    return objs[idx];
}
static int hu_blocking(uint8_t *mgr) {   // type of a visible UI object that forbids a switch, -1 = none
    const uint32_t n = *(volatile uint32_t *)(mgr + HU_OBJS_N); uint8_t **objs = *(uint8_t **volatile *)(mgr + HU_OBJS);
    if (!objs || n > 4096) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *o = objs[i];
        if (!o || !(o[HU_OBJ_FLAGS] & 8)) continue;
        const uint32_t t = *(const volatile uint32_t *)(o + HU_OBJ_TYPE);
        for (int k = 0; k < g_nblock; k++) if (g_block[k] == t) return (int)t;
    }
    return -1;
}
static int hu_records(const uint8_t *ui, uint16_t *ids, uint8_t **nodes, int max) {   // the players that have a UI set (hash ui+0xf0: +4 buckets, +8 array; node +8 u16 id)
    const uint32_t nb = *(const volatile uint32_t *)(ui + HU_UI_HASH + 4);
    uint8_t *const volatile *bk = *(uint8_t *const volatile *const volatile *)(ui + HU_UI_HASH + 8);
    int n = 0;
    if (!bk || !nb || nb > 0x10000) return 0;
    for (uint32_t i = 0; i < nb; i++) for (uint8_t *nd = bk[i]; nd && n < max; nd = *(uint8_t *const *)(const void *)nd) {
        if (ids) ids[n] = *(const uint16_t *)(const void *)(nd + 8);
        if (nodes) nodes[n] = nd;
        n++; }
    return n;
}
// the movie a type's creator plays now: the tail from the last "GUI/" of its path ("" = no creator with a long-form path)
static void hu_tail(uint8_t *mgr, uint32_t type, char *out, size_t n) {
    out[0] = 0;
    const uint32_t nb = *(volatile uint32_t *)(mgr + HU_HASH_N); hu_node *volatile *bk = *(hu_node *volatile *volatile *)(mgr + HU_HASH_B);
    if (!bk || !nb || nb > 0x10000) return;
    hu_node *nd = bk[type % nb]; int guard = 0;
    while (nd && nd->id != type && guard++ < 4096) nd = nd->next;
    hu_creator *cr = nd && nd->id == type ? nd->cr : NULL;
    if (!cr || !(*(const uint8_t *)&cr->path & 1) || !cr->path.ptr || cr->path.len < 9 || cr->path.len >= 200 || cr->path.len + 1 > (cr->path.cap1 & ~1u)) return;
    const char *p = cr->path.ptr; const uint32_t len = cr->path.len;
    for (int k = (int)len - 8; k >= 0; k--) if (!memcmp(p + k, "GUI/", 4) && (k == 0 || p[k - 1] == '/')) { snprintf(out, n, "%.*s", (int)(len - (uint32_t)k), p + k); return; }
}

// ---- the size: class scale + the engine's own resize ----
static float hu_cfg_s(void) { const uint8_t *cfg = *(const uint8_t *volatile *)(g_main->base + HU_CFG); return cfg ? *(const volatile float *)(cfg + HU_CFG_S) : 0; }
static void hu_scale(uint8_t *mgr, uint8_t *o, int ti, const char *movie) {
    if (!g_scale_ok || g_tune == 0 || g_t[ti].ipad_c == 0) return;
    const float s = hu_cfg_s(), was = *(volatile float *)(o + HU_OBJ_C), c = hu_class_scale(g_tune, g_t[ti].ipad_c, s);
    const uint32_t d = *(volatile uint32_t *)(mgr + HU_MGR_D);
    ((void (*)(void *, float))(void *)(g_main->base + HU_SETSCALE))(o, c);     // [obj+0x94] = c, root.uiScaling = c x S when the movie has it
    ((void (*)(void *))(void *)(g_main->base + HU_RESIZE_NOW))(o);             // main+0xae3be0(obj, window w, h); a movie not loaded yet: the engine's own first resize uses c
    if (g_t[ti].logged && !g_dbg) return;
    g_t[ti].logged = 1;
    const uint8_t *fp = *(uint8_t *const volatile *)(o + HU_OBJ_FP); const int32_t *r = fp ? (const int32_t *)(const void *)(fp + 0x10) : NULL;
    const float sw = *(volatile float *)(o + HU_OBJ_W), sh = *(volatile float *)(o + HU_OBJ_W + 4);
    const float eff = s >= 0.2f && s <= 4.0f ? s : 1;   // the S hu_class_scale cancelled
    LOG("[hud] size of type %#x %s: D %u, S %.3f%s, DM_DOS2_TOUCH_HUD_SCALE %.3f x iPad class scale %.2f -> class scale c %.4f (was %.3f); fixed k = c x S x h / D = %.4f x h / %u; "
        "drawn %.0fx%.0f px, FlashPlayer rect %d,%d %dx%d", g_t[ti].type, movie, d, s, eff == s ? "" : " (not usable: taken as 1)", g_tune, g_t[ti].ipad_c, c, was, c * eff, d,
        (double)sw, (double)sh, r ? r[0] : 0, r ? r[1] : 0, r ? r[2] : 0, r ? r[3] : 0);
    if (g_t[ti].type == HU_T_HOTBAR && r && d) {
        const float k = sh > 0 ? sh / 1000.0f : hu_fixed_k(c, eff, (float)d, (float)d);   // the drawn height of the 1000 px stage
        LOG("[hud] hot bar on this screen (tools/dos2_pchud_touch.py numbers x k %.3f): slot pitch 86 -> %.1f px, slot 74 x 78 -> %.1f x %.1f px; on a 1080-high screen at scale 1: 86 / 74 x 78 (iPad: pitch 86, icon 76, cell 82)",
            k, 86.0 * k, 74.1 * k, 78.1 * k);
    }
}

// ---- the host's objects ----
static void hu_destroy(const char *why) {
    uint8_t *mgr = hu_mgr();
    if (!g_nobj) return;
    const uint64_t t0 = hu_us(); int gone = 0;
    for (int i = g_nobj - 1; i >= 0 && mgr; i--) {
        uint8_t *o = hu_obj(mgr, g_obj[i].h);
        if (!o || *(volatile uint32_t *)(o + HU_OBJ_TYPE) != g_obj[i].type) {
            if (g_obj[i].slot && *(volatile uint64_t *)(void *)g_obj[i].slot == g_obj[i].h) *(volatile uint64_t *)(void *)g_obj[i].slot = 0;   // a stale handle of ours
            if (g_dbg) LOG("[hud] type %#x player %u handle %#llx: already gone", g_obj[i].type, g_obj[i].player, (unsigned long long)g_obj[i].h);
            continue; }
        void **vt = *(void ***)(void *)o;
        ((void (*)(void *))vt[0xf0 / 8])(o);                                                              // as main+0x10bc820
        uint64_t h = g_obj[i].h;
        ((void (*)(void *, uint64_t *))(void *)(g_main->base + HU_DESTROY_OBJ))(mgr, &h);                  // as main+0x10bc830
        struct { void *vt; uint32_t one, pad; void *obj; } msg = {g_main->base + HU_MSG_VT, 1, 0, o};     // as main+0x10bc83c..0x10bc860
        void *to = *(void *volatile *)(g_main->base + HU_MSG_TO);
        if (to) ((void (*)(void *, void *))(void *)(g_main->base + HU_MSG_SEND))(to, &msg);
        if (g_obj[i].slot && *(volatile uint64_t *)(void *)g_obj[i].slot == g_obj[i].h) *(volatile uint64_t *)(void *)g_obj[i].slot = 0;   // the game's own destroy skips an empty slot
        gone++;
        if (g_dbg) LOG("[hud] destroyed type %#x player %u handle %#llx", g_obj[i].type, g_obj[i].player, (unsigned long long)g_obj[i].h);
    }
    LOG("[hud] touch HUD down (%s): %d of %d object(s) destroyed, %llu us", why, gone, g_nobj, (unsigned long long)(hu_us() - t0));
    g_nobj = 0; atomic_store(&g_aim, 0); g_act.kind = 0; g_plate_on = 0;
}
static void hu_create(uint8_t *ui, const char *why) {
    uint8_t *mgr = hu_mgr();
    if (!mgr || !ui || g_nobj) return;
    uint16_t ids[8]; uint8_t *nodes[8]; const int np = hu_records(ui, ids, nodes, 8);
    if (!np) return;
    g_tried = 1;
    const uint64_t t0 = hu_us(); int made = 0, failed = 0, had = 0; char list[640]; size_t lo = 0; list[0] = 0;
    for (int p = 0; p < np; p++) for (int i = 0; i < HU_NTYPES && g_nobj < HU_MAXOBJ; i++) {
        if (!g_t[i].on) continue;
        char mv[96]; hu_tail(mgr, g_t[i].type, mv, sizeof mv);
        if (!mv[0]) { if (!p) LOG("【红】[hud] type %#x: no creator with a long-form GUI path: not made", g_t[i].type); continue; }
        volatile uint64_t *slot = g_slots && g_slots_ok && g_t[i].slot ? (volatile uint64_t *)(void *)(nodes[p] + g_t[i].slot) : NULL;
        if (slot && (*slot & 0x3fffff00000000ull)) { had++; if (g_dbg) LOG("[hud] type %#x player %u: the record's slot +%#x already holds %#llx: left alone", g_t[i].type, ids[p], g_t[i].slot, (unsigned long long)*slot); continue; }
        if (g_t[i].need && strcmp(mv, g_t[i].need)) { if (!p) LOG("[hud] type %#x plays %s, not %s (UI mode not touch, or the package has no such movie): not made", g_t[i].type, mv, g_t[i].need); continue; }
        const uint64_t h = ((hu_fn)(void *)(g_main->base + HU_CREATE))((uint64_t)(uintptr_t)mgr, g_t[i].layer, g_t[i].type, g_t[i].flags | 0x20000000u, 0x80, ids[p]);
        uint8_t *o = hu_obj(mgr, h);
        const int ok = o != NULL;
        if (ok) {
            g_obj[g_nobj].h = h; g_obj[g_nobj].type = g_t[i].type; g_obj[g_nobj].player = ids[p]; g_obj[g_nobj].slot = (uint8_t *)(void *)slot; g_nobj++; made++;
            if (slot) *slot = h;   // as the iPad's builder: str x0,[node,#off] - the engine's PC code finds the window there
            hu_tail(mgr, g_t[i].type, mv, sizeof mv);   // after the creation: hle_dos2_touchui.c may have gone back to the stock movie
            hu_scale(mgr, o, i, mv);
            if (g_show && g_flags_ok) ((void (*)(void *, uint32_t))(void *)(g_main->base + HU_SETFLAGS_FN))(o, HU_F_VISIBLE);
            if (!p && lo + 40 < sizeof list) lo += (size_t)snprintf(list + lo, sizeof list - lo, "%s%#x %s", lo ? ", " : "", g_t[i].type, mv + 4);
        } else failed++;
        if (g_dbg || !ok) LOG("%s[hud] CreateUIObject type %#x = %s layer %#x flags %#x player %u -> handle %#llx%s", ok ? "" : "【红】", g_t[i].type, mv, g_t[i].layer,
                              g_t[i].flags | 0x20000000u, ids[p], (unsigned long long)h, ok ? "" : ": FAILED");
    }
    LOG("%s[hud] touch HUD up (%s): %d object(s) for %d player(s)%s, %d slot(s) were filled already, handles %s: %s; %llu us (game state %#x, UI mode %s)", failed ? "【红】" : "", why, made, np,
        failed ? ", some failed" : "", had, g_slots && g_slots_ok ? "written into the player's record (the PC branch of the set)" : "kept by the host only (panels cannot be opened)", list,
        (unsigned long long)(hu_us() - t0), hu_state(), ui_mode_name(dm_dos2_ui_mode()));
    dm_dos2_touchui_ei_dump("touch HUD up");
}
static uint64_t hu_destroy_all_hook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (g_nobj) hu_destroy(g_switching ? "switch" : "the game unloads the UI set");
    g_tried = 0;
    // the old switch alone: menus and loading stay controller. UI mode TOUCH keeps its byte 0 (PC menus; hle_dos2_uimode.c owns the byte)
    if (g_env > 0 && !g_switching && *hu_mode() == 0 && dm_dos2_ui_mode() != UI_TOUCH && dm_dos2_ui_mode_hud() != UI_TOUCH) {
        *hu_mode() = 1; LOG("[hud] the game destroys the player UI sets with the PC set up: mode byte main+%#x back to 1 (menus and loading stay controller)", HU_MODE); }
    return g_destroy_all_orig(a0, a1, a2, a3, a4, a5);
}
static uint64_t hu_create_all_hook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    const uint64_t v = g_create_all_orig(a0, a1, a2, a3, a4, a5);
    if (hu_on() && a0 && *hu_mode() == 0) hu_create((uint8_t *)(uintptr_t)a0, "the player UI sets were built with the mode byte 0");
    g_switching = 0;
    return v;
}
// hle_dos2_touch.c, right before a rebuild changes the mode (dt_live_switch, dm_dos2_touch_ui_hud): 1 = go on, 0 = not now.
// to_touch: 1 = the PC / touch set is wanted, 0 = the controller set
int dm_dos2_touchui_hud_gate(int to_touch, int records);
int dm_dos2_touchui_hud_gate(int to_touch, int records) {
    if (!g_hooked || (!hu_on() && !g_nobj)) return 1;
    if (!records) return 1;   // no player UI set: only the byte changes, nothing is rebuilt
    uint8_t *mgr = hu_mgr();
    const int st = hu_state(), blk = mgr ? hu_blocking(mgr) : -1;
    int st_ok = g_any_state;
    for (int i = 0; i < g_nstates; i++) if ((int)g_states[i] == st) st_ok = 1;
    const char *why = !mgr ? "no UI manager" : !st_ok ? "game state" : blk >= 0 ? "a blocking UI is on screen" : NULL;
    if (why) {
        if (g_nrefused < 12 || (g_dbg && !(g_nrefused % 300)))
            LOG("[hud] switch to the %s set refused: %s (game state %#x, blocking type %#x, records %d)%s", to_touch ? "touch" : "controller", why, st, blk, records,
                to_touch ? "" : "; asked again on the next poll");
        g_nrefused++;
        return 0;
    }
    g_switching = 1; g_nswitch++;
    if (g_dbg || g_nswitch <= 8) LOG("[hud] switch #%u to the %s set (game state %#x, records %d)", g_nswitch, to_touch ? "touch" : "controller", st, records);
    return 1;
}

// ---- host clicks: mouse / key records through the engine's InjectInput, on the game thread ----
static void hu_send(uint32_t dev, uint32_t id, float a, float b, int pressed, int motion) {
    void *mgr = *(void *volatile *)(g_main->base + HU_INMGR);
    if (!mgr) return;
    hu_rec r; memset(&r, 0, sizeof r);
    r.raw = dev << 16 | id; r.v[0] = a; r.v[1] = b; r.pressed = (uint8_t)pressed;
    ((void (*)(void *, const hu_rec *, uint64_t))(void *)(g_main->base + HU_INJECT))(mgr, &r, (uint64_t)motion);
}
static int hu_input_ok(uint32_t dev) {   // the raw-state array hle_dos2_touch.c keeps must hold the device (a NULL array crashes ProcessInputEvent)
    const uint8_t *mgr = *(const uint8_t *volatile *)(g_main->base + HU_INMGR);
    if (!mgr || !dm_dos2_touch_on()) return 0;
    return *(void *const volatile *)(mgr + HU_IN_DEVS) != NULL && *(const volatile uint32_t *)(mgr + HU_IN_DEVN) > dev;
}
static void hu_act_start_key(int kind, const char *what, uint32_t key);
static void hu_act_start(int kind, const char *what) { hu_act_start_key(kind, what, 1 /* escape */); }
static void hu_act_start_key(int kind, const char *what, uint32_t key) {
    float x = -1, y = -1;
    if (kind != 3 && !hu_aim_point(&g_aimp, &x, &y)) { LOG("[hud] %s: no aimed point yet (the cursor has not rested anywhere): nothing sent", what); return; }
    if (!hu_input_ok(kind == 3 ? 0 : (uint32_t)g_mdev)) { LOG("【红】[hud] %s: touch as mouse is off or the input manager has no state array for the device: nothing sent", what); return; }
    g_act.kind = kind; g_act.step = 0; g_act.x = x; g_act.y = y; g_act.t = 0; g_act.key = key;
    if (g_dbg || g_naim++ < 12) LOG("[hud] %s -> %s%s (sent on the next polls)", what, kind == 1 ? "left click" : kind == 2 ? "right click" : "a key record",
                                    kind == 3 ? "" : " at the aimed point");
    if (kind != 3 && (g_dbg || g_naim < 14)) LOG("[hud]   aimed point %.0f,%.0f (window px)", (double)x, (double)y);
}
static void hu_act_step(void) {
    enum { ID_LEFT = 0x74, ID_RIGHT = 0x76, ID_MOTION = 0x79 };
    if (!g_act.kind) return;
    const uint64_t now = hu_us() / 1000;
    const uint32_t dev = g_act.kind == 3 ? 0 : (uint32_t)g_mdev, id = g_act.kind == 1 ? ID_LEFT : g_act.kind == 2 ? ID_RIGHT : g_act.key;
    if (!hu_input_ok(dev)) { g_act.kind = 0; return; }
    if (g_act.step == 0) {
        if (g_act.kind != 3) {   // the iPad's order: the cursor, a motion record, then the button
            float *c = *(float *volatile *)(g_main->base + HU_CURSOR);
            if (c) { c[0] = g_act.x; c[1] = g_act.y; }
            hu_send(dev, ID_MOTION, 1, 1, 1, 1);
            g_act.step = 1; g_act.t = now; return;   // the button on the next poll: the world's picking follows the cursor first
        }
        hu_send(dev, id, 1, 1, 1, 0); g_act.step = 2; g_act.t = now; return;
    }
    if (now - g_act.t < 30) return;
    if (g_act.step == 1) { hu_send(dev, id, 1, 1, 1, 0); g_act.step = 2; g_act.t = now; return; }
    hu_send(dev, id, 0, 0, 0, 0); g_act.kind = 0;
}

// ---- ExternalInterface of the `_p` movies ----
static int hu_ei_side(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {   // dmSide(code)
    (void)ctx; (void)player; (void)name;
    const int code = n && a && a[0].type == 4 ? (int)a[0].d : -1, pc = hu_side_panel(code);
    static const char *const k_nm[8] = {"?", "options", "map", "character", "inventory", "skills", "crafting", "journal"};
    if (code < 1 || code > 7) { if (g_nside++ < 20) LOG("[hud] dmSide(%d): no such button (1..7)", code); return 1; }
    if (!dm_dos2_touchui_may_press(ui)) { if (g_nside++ < 20) LOG("[hud] dmSide(%d %s): refused, the HUD is under a modal window", code, k_nm[code]); return 1; }
    if (code == 1) { hu_act_start(3, "dmSide(1 options)"); return 1; }
    uint8_t *mgr = hu_mgr(), *hb = NULL;
    const uint16_t who = ui ? *(const volatile uint16_t *)((const uint8_t *)ui + HU_OBJ_PLAYER) : 0;
    for (int i = 0; i < g_nobj && mgr; i++) if (g_obj[i].type == HU_T_HOTBAR && (!hb || g_obj[i].player == who)) { uint8_t *o = hu_obj(mgr, g_obj[i].h); if (o && *(volatile uint32_t *)(o + HU_OBJ_TYPE) == HU_T_HOTBAR) hb = o; }
    if (!hb || !g_btn_ok) { if (g_nside++ < 20) LOG("【红】[hud] dmSide(%d %s): %s: nothing opened", code, k_nm[code], hb ? "the hot bar class's handler differs in this build" : "no PC hot bar object of the host"); return 1; }
    void *fn = g_main->base + HU_HB_CALLED;
    if ((*(void ***)(void *)hb)[0] != fn) { if (g_nside++ < 20) LOG("【红】[hud] dmSide(%d %s): the hot bar object's vt[0] is not main+%#x: nothing opened", code, k_nm[code], HU_HB_CALLED); return 1; }
    dm_ei_val v; memset(&v, 0, sizeof v); v.type = 4; v.d = (double)pc;
    if (g_dbg || g_nside++ < 12) LOG("[hud] dmSide(%d %s): \"hotbarBtnPressed\"(%d) to the PC hot bar object %p (player %u), as its own button does", code, k_nm[code], pc, (void *)hb,
                                     (unsigned)*(volatile uint16_t *)(hb + HU_OBJ_PLAYER));
    ((void (*)(void *, const char *, unsigned, const dm_ei_val *))fn)(hb, "hotbarBtnPressed", 1, &v);
    return 1;
}
static int hu_ei_aim(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {   // dmAim(0 | 1)
    (void)ctx; (void)player; (void)ui; (void)name;
    const int on = n && a && a[0].type == 4 && a[0].d != 0;
    atomic_store(&g_aim, on);
    static unsigned cnt; if (g_dbg || cnt++ < 12) LOG("[hud] dmAim(%d): the hot bar %s a prepared skill; CONFIRM / cancel %s", on, on ? "marks" : "has no", on ? "shown" : "hidden");
    return 1;
}
static int hu_ei_aimbtn(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {   // dmAimBtn(1 confirm | 0 cancel)
    (void)ctx; (void)player; (void)name;
    const int ok = n && a && a[0].type == 4 && a[0].d != 0;
    if (!dm_dos2_touchui_may_press(ui)) return 1;
    hu_act_start(ok ? 1 : 2, ok ? "dmAimBtn(1 CONFIRM)" : "dmAimBtn(0 cancel)");
    if (ok) g_plate_on = 0;   // the movie hid the plate itself
    return 1;
}
static int hu_ei_key(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {   // dmKey(1 quick save | 2 tactical camera)
    (void)ctx; (void)player; (void)name;
    enum { ID_KEY_O = 0x32, ID_KEY_F5 = 0x42 };   // raw name table main+0x2974b10, index - 2
    const int code = n && a && a[0].type == 4 ? (int)a[0].d : -1;
    if (code != 1 && code != 2) { LOG("[hud] dmKey(%d): no such key button (1 quick save, 2 tactical camera)", code); return 1; }
    if (!dm_dos2_touchui_may_press(ui)) return 1;
    hu_act_start_key(3, code == 1 ? "dmKey(1 quick save: f5)" : "dmKey(2 tactical camera: o)", code == 1 ? ID_KEY_F5 : ID_KEY_O);
    return 1;
}
// hle_dos2_touch.c, the release of a short long press over the world in UI mode TOUCH: with the `long` rule on, the plate goes up there (1)
int dm_dos2_touchui_hud_plate(float x, float y, int obj);
int dm_dos2_touchui_hud_plate_long(float x, float y);
int dm_dos2_touchui_hud_plate_long(float x, float y) {
    if (!g_plate || !g_nobj || !hu_pick_long_on(&g_pick, g_pick_mode)) return 0;
    return dm_dos2_touchui_hud_plate(x, y, -2);
}
// hle_dos2_touch.c, a world tap in UI mode TOUCH (game thread; x, y window px; obj = a character / item is under the finger): 1 = the tap is
// taken - no click, the action plate is up over the object; 0 = act as always (the plate, if any, is hidden)
int dm_dos2_touchui_hud_plate(float x, float y, int obj);
int dm_dos2_touchui_hud_plate(float x, float y, int obj) {
    enum { PLATE_BASE = 2000000, PLATE_HIDE = 1999999, PLATE_MUL = 2000, HB_W = 1948, HB_H = 1000 };   // tools/dos2_pchud_touch.py
    if (!g_plate || !g_nobj || !g_main) return 0;
    uint8_t *mgr = hu_mgr(), *hb = NULL;
    for (int i = 0; i < g_nobj && mgr && !hb; i++) if (g_obj[i].type == HU_T_HOTBAR) { uint8_t *o = hu_obj(mgr, g_obj[i].h); if (o && *(volatile uint32_t *)(o + HU_OBJ_TYPE) == HU_T_HOTBAR) hb = o; }
    uint8_t *fp = hb ? *(uint8_t *volatile *)(hb + HU_OBJ_FP) : NULL;
    if (!fp) return 0;
    dm_ei_val v; memset(&v, 0, sizeof v); v.type = 4;
    static unsigned cnt;
    if (obj >= 0) {   // a world tap: who names an object (obj = the `pc` source; -2 = the short long press of the `long` rule: the plate at once)
        int dm_dos2_touch_world_obj_live(void);
        const int live = dm_dos2_touch_world_obj_live(); int died = 0;
        const int src = hu_pick_tap(&g_pick, g_pick_mode, obj, live, &died);
        if (died) LOG("【红】[hud] action plate: the object picking is dead in this input mode (%u world taps, %s): from now on a tap clicks at once and the SHORT LONG PRESS puts the "
                      "plate up (DM_DOS2_TOUCH_T_PICK=auto -> long)", g_pick.taps, live < 0 ? "the picking helper's ray set-up does not run" : "no source ever named an object");
        if (g_dbg || cnt < 20) LOG("[hud] action plate pick #%u: source %s (pc %d, hover %d; DM_DOS2_TOUCH_T_PICK=%s%s)", g_pick.taps, hu_src_name(src), obj, live, hu_pick_name(g_pick_mode),
                                   g_pick.dead ? ", picking dead: long press rule" : "");
        obj = src != HU_SRC_NONE;
    }
    if (!obj) {
        if (g_plate_on) { v.d = PLATE_HIDE; dm_dos2_touchui_invoke(fp, "showActiveSkill", &v, 1); g_plate_on = 0; if (g_dbg || cnt++ < 20) LOG("[hud] action plate hidden (a tap on the ground)"); }
        return 0;
    }
    const int32_t *r = (const int32_t *)(const void *)(fp + 0x10);   // the movie's rect in the window: x, y, w, h
    if (r[2] <= 0 || r[3] <= 0) return 0;
    const int sx = (int)((x - (float)r[0]) * HB_W / (float)r[2]), sy = (int)((y - (float)r[1]) * HB_H / (float)r[3]);
    if (sx < 0 || sx >= HB_W || sy < 0 || sy >= HB_H) return 0;   // outside hotBar_p's frame (the far left / right of a wide screen): no plate there
    v.d = (double)(PLATE_BASE + sx * PLATE_MUL + sy);
    if (!dm_dos2_touchui_invoke(fp, "showActiveSkill", &v, 1)) { if (cnt++ < 20) LOG("【红】[hud] action plate: hotBar_p's player has no invoke name \"showActiveSkill\": the tap acts at once"); return 0; }
    hu_aim_set(&g_aimp, x, y); g_plate_on = 1;
    if (g_dbg || cnt++ < 20) LOG("[hud] action plate over the object at %.0f,%.0f (stage %d,%d of hotBar_p): a tap on it = the PC click there", (double)x, (double)y, sx, sy);
    return 1;
}
// for the gestures (hle_dos2_touch.c; any thread): is a skill prepared (CONFIRM / cancel on screen), and "the player aimed here" (window px)
int dm_dos2_touchui_hud_aiming(void);
int dm_dos2_touchui_hud_aiming(void) { return g_nobj > 0 && atomic_load(&g_aim); }
void dm_dos2_touchui_hud_aim_at(float x, float y);
void dm_dos2_touchui_hud_aim_at(float x, float y) {
    uint32_t a, b; memcpy(&a, &x, 4); memcpy(&b, &y, 4);
    atomic_store(&g_aim_xy, (uint64_t)a << 32 | b); atomic_store(&g_aim_set, 1);
}

// hle_dos2_uimode.c dm_dos2_uimode_poll, every pad poll (game thread)
void dm_dos2_touchui_hud_poll(void);
void dm_dos2_touchui_hud_poll(void) {
    if (!g_hooked) return;
    uint8_t *ui = *(uint8_t *volatile *)(g_main->base + HU_UI_THIS), *mgr = hu_mgr();
    if (!ui || !mgr) return;
    const int recs = hu_records(ui, NULL, NULL, 64);
    if (!recs) g_tried = 0;
    if (g_nobj && !recs) hu_destroy("the player UI sets are gone");
    else if (!g_nobj && recs && !g_tried && hu_on() && *hu_mode() == 0) hu_create(ui, "a player UI set exists with the mode byte 0 and no host HUD");
    if (!g_nobj) return;
    // the right side shows / hides with the player's hot bar (a plain UIObject: nobody else shows it)
    if (g_flags_ok && !g_show) for (int i = 0; i < g_nobj; i++) {
        if (g_obj[i].type != HU_T_SIDE) continue;
        uint8_t *o = hu_obj(mgr, g_obj[i].h); int want = 1;
        if (!o || *(volatile uint32_t *)(o + HU_OBJ_TYPE) != HU_T_SIDE) continue;
        for (int k = 0; k < g_nobj; k++) if (g_obj[k].type == HU_T_HOTBAR && g_obj[k].player == g_obj[i].player) { const uint8_t *hb = hu_obj(mgr, g_obj[k].h); if (hb) want = (hb[HU_OBJ_FLAGS] & HU_F_VISIBLE) != 0; }
        const int vis = (o[HU_OBJ_FLAGS] & HU_F_VISIBLE) != 0;
        if (vis == want) continue;
        ((void (*)(void *, uint32_t))(void *)(g_main->base + (want ? HU_SETFLAGS_FN : HU_CLRFLAGS_FN)))(o, HU_F_VISIBLE);
        static unsigned n; if (n++ < 24 || g_dbg) LOG("[hud] right side (dmSide_p) %s with the hot bar (flags now %#x)", want ? "SHOWN" : "hidden", *(volatile uint32_t *)(o + HU_OBJ_FLAGS));
    }
    // the aimed point: named by the gestures, else where the cursor rested
    if (atomic_exchange(&g_aim_set, 0)) { const uint64_t v = atomic_load(&g_aim_xy); const uint32_t a = (uint32_t)(v >> 32), b = (uint32_t)v; float x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4); hu_aim_set(&g_aimp, x, y); }
    else if (!g_act.kind) { const float *c = *(float *const volatile *)(g_main->base + HU_CURSOR); if (c) hu_aim_feed(&g_aimp, c[0], c[1], hu_us() / 1000); }
    hu_act_step();
}

static int hu_poke(module_t *m, uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(void *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    const size_t len = (size_t)(((uintptr_t)(at + n) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, w, 4 * (size_t)n);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[hud] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + n));
    return 0;
}
static int hu_list(const char *v, uint32_t *out, int max) {
    int n = 0; char *dup = strdup(v), *save = NULL;
    for (char *t = dup ? strtok_r(dup, ",; ", &save) : NULL; t && n < max; t = strtok_r(NULL, ",; ", &save)) { char *end; const unsigned long x = strtoul(t, &end, 16); if (end != t && !*end) out[n++] = (uint32_t)x; }
    free(dup); return n;
}
static int hu_same(module_t *m, const hu_site *s, unsigned n) {   // -1 = all equal, else the first site that differs
    for (unsigned i = 0; i < n; i++) if ((uint64_t)s[i].off + 4ull * s[i].n > m->text_end || memcmp(m->base + s[i].off, s[i].want, 4ull * s[i].n)) return (int)i;
    return -1;
}
// Step 1, main.c host_preload: after dm_dos2_touchui_install / _ei_install, BEFORE dm_dos2_touch_install. Compares the sites and reads the
// switches; writes nothing into the game (hle_dos2_touch.c compares the prologues of create-all / destroy-all after this).
void dm_dos2_touchui_hud_install(void);
void dm_dos2_touchui_hud_install(void) {
    int dm_dos2_touchui_on(void);
    const char *e = getenv("DM_DOS2_TOUCHUI_HUD"), *v;
    g_env = e && *e ? atoi(e) > 0 : -1;
    if (!dm_dos2_touchui_on()) { if (g_env > 0) LOG("【红】[hud] DM_DOS2_TOUCHUI_HUD=1 needs DM_DOS2_TOUCHUI=1 with its package (the CreateUIObject hook and the movies): touch HUD off"); return; }
    if (g_env == 0) { LOG("[hud] DM_DOS2_TOUCHUI_HUD=0: the host never makes the PC HUD (UI mode touch then has menus only)"); return; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) return;
    g_dbg = (v = getenv("DM_DBG")) && atoi(v) > 0;
    static const hu_site k_sites[] = {{HU_CREATE_ALL, 4, k_hu_create_all}, {HU_DESTROY_ALL, 4, k_hu_destroy_all}, {HU_SLOT_DESTROY, 35, k_hu_slot_destroy}, {HU_DESTROY_OBJ, 8, k_hu_destroy_obj},
        {HU_MSG_SEND, 4, k_hu_msg_send}, {HU_STATE_RD, 10, k_hu_state_rd}, {HU_C_SITE, 8, k_hu_c_site}, {HU_LOOKUP, 6, k_hu_lookup}, {HU_MODE_RD, 2, k_hu_mode_rd}, {HU_VIS_RD, 2, k_hu_vis_rd}};
    const int bad = hu_same(m, k_sites, sizeof k_sites / sizeof *k_sites);
    if (bad >= 0) { LOG("【红】[hud] main+%#x differs: not DOS2 update v786432 main (build 4979b200) at the player UI set: touch HUD off, nothing changed", k_sites[bad].off); return; }
    static const hu_site k_size[] = {{HU_SETSCALE, 8, k_hu_setscale}, {HU_SETSCALE_ST, 1, k_hu_setscale_st}, {HU_RESIZE_NOW, 9, k_hu_resize_now}, {HU_FIXED, 10, k_hu_fixed}, {HU_CTOR_D, 3, k_hu_ctor_d}};
    const int bs = hu_same(m, k_size, sizeof k_size / sizeof *k_size);
    g_scale_ok = bs < 0;
    if (bs >= 0) LOG("【红】[hud] main+%#x differs (the UI object's class scale / resize): the touch HUD keeps the engine's own size, DM_DOS2_TOUCH_HUD_SCALE not applied", k_size[bs].off);
    static const hu_site k_fl[] = {{HU_SETFLAGS_FN, 3, k_hu_setflags_fn}, {HU_CLRFLAGS_FN, 3, k_hu_clrflags_fn}};
    g_flags_ok = hu_same(m, k_fl, 2) < 0;
    if (!g_flags_ok) LOG("【红】[hud] main+%#x / +%#x differ (SetFlags / ClearFlags): the right side (dmSide_p) is not shown by the host", HU_SETFLAGS_FN, HU_CLRFLAGS_FN);
    static const hu_site k_hb[] = {{HU_HB_CALLED, 12, k_hu_hb_called}, {HU_HB_BTN, 5, k_hu_hb_btn}, {HU_HB_SWITCH, 5, k_hu_hb_switch}};
    g_btn_ok = hu_same(m, k_hb, 3) < 0 && m->size > HU_HB_TABLE + sizeof k_hu_hb_table && !memcmp(m->base + HU_HB_TABLE, k_hu_hb_table, sizeof k_hu_hb_table);
    if (!g_btn_ok) LOG("【红】[hud] the PC hot bar class's hotbarBtnPressed (main+%#x) differs: the panel column of dmSide_p opens nothing", HU_HB_CALLED);
    static const hu_site k_sl[] = {{HU_SLOT_SKILLS, 2, k_hu_slot_skills}, {HU_SLOT_SKILLS_T, 2, k_hu_slot_skills_t}, {HU_SLOT_INV, 2, k_hu_slot_inv}, {HU_SLOT_SHEET, 2, k_hu_slot_sheet},
                                   {HU_SLOT_CRAFT, 2, k_hu_slot_craft}, {HU_SLOT_HOTBAR, 2, k_hu_slot_hotbar}};
    const int bl = hu_same(m, k_sl, sizeof k_sl / sizeof *k_sl);
    g_slots_ok = bl < 0;
    if (bl >= 0) LOG("【红】[hud] main+%#x differs (the record's PC slots): the host keeps its handles to itself, the PC panels cannot be opened", k_sl[bl].off);
    g_slots = !((v = getenv("DM_DOS2_TOUCHUI_HUD_SLOTS")) && *v == '0');
    if ((v = getenv("DM_DOS2_TOUCHUI_HUD_SET")) && *v) { uint32_t ids[HU_NTYPES]; const int n = hu_list(v, ids, HU_NTYPES);
        for (int i = 0; i < HU_NTYPES; i++) { g_t[i].on = 0; for (int k = 0; k < n; k++) if (ids[k] == g_t[i].type) g_t[i].on = 1; } }
    if ((v = getenv("DM_DOS2_TOUCHUI_HUD_STATES")) && *v) { if (!strcmp(v, "any")) g_any_state = 1; else { g_nstates = hu_list(v, g_states, 8); g_any_state = 0; } }
    if ((v = getenv("DM_DOS2_TOUCHUI_HUD_BLOCK")) && *v) g_nblock = hu_list(v, g_block, 16);
    g_show = (v = getenv("DM_DOS2_TOUCHUI_HUD_SHOW")) && atoi(v) > 0;
    g_plate = !((v = getenv("DM_DOS2_TOUCH_T_PLATE")) && *v == '0');
    g_pick_mode = hu_pick_parse(getenv("DM_DOS2_TOUCH_T_PICK"));
    LOG("[hud] action plate %s; object picking DM_DOS2_TOUCH_T_PICK=%s (auto: the tap's pick, then the engine's PC hover pick, then the short long press when the picking is dead)",
        g_plate ? "on" : "OFF (DM_DOS2_TOUCH_T_PLATE=0)", hu_pick_name(g_pick_mode));
    v = getenv("DM_DOS2_TOUCH_HUD_SCALE"); g_tune = hu_tune(v && *v ? (float)atof(v) : 0, v && *v);
    if ((v = getenv("DM_DOS2_TOUCH_DEV")) && atoi(v) > 0 && atoi(v) < 64) g_mdev = atoi(v);
    g_sites = 1;
    if (dm_dos2_touchui_ei_on()) {
        dm_dos2_touchui_ei_register("dmSide", hu_ei_side, NULL); dm_dos2_touchui_ei_register("dmAim", hu_ei_aim, NULL); dm_dos2_touchui_ei_register("dmAimBtn", hu_ei_aimbtn, NULL);
        dm_dos2_touchui_ei_register("dmKey", hu_ei_key, NULL);
    } else LOG("【红】[hud] the ExternalInterface hook is off (DM_DOS2_TOUCHUI_EI=0 or its sites differ): dmSide / dmAim / dmAimBtn of the touch HUD movies do nothing");
    if (g_env > 0) {   // the old switch: the PC set follows the last input, rebuilt in game
        const char *lv = getenv("DM_DOS2_TOUCH_LIVE");
        if (!lv || !*lv) setenv("DM_DOS2_TOUCH_LIVE", "1", 1);
        if (lv && atoi(lv) != 1) LOG("【红】[hud] DM_DOS2_TOUCH_LIVE=%s: DM_DOS2_TOUCHUI_HUD=1 needs 1 (the set rebuilt in game)", lv);
    }
}
// Step 2, main.c host_preload: AFTER dm_dos2_touch_install (which compares the same prologues for its live rebuild): the two jumps.
void dm_dos2_touchui_hud_hook(void);
void dm_dos2_touchui_hud_hook(void) {
    module_t *m = g_main;
    if (!g_sites || !m || g_hooked) return;
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[hud] mmap failed: touch HUD off"); return; }
    const struct { uint32_t off; const uint32_t *pro; hu_fn host; hu_fn *orig; } hk[2] = {{HU_CREATE_ALL, k_hu_create_all, hu_create_all_hook, &g_create_all_orig},
                                                                                         {HU_DESTROY_ALL, k_hu_destroy_all, hu_destroy_all_hook, &g_destroy_all_orig}};
    for (int i = 0; i < 2; i++) { uint32_t *s = t + 8 * i; const uint64_t rest = (uint64_t)(uintptr_t)(m->base + hk[i].off + 16);
        memcpy(s, hk[i].pro, 16); s[4] = 0x58000050 /* ldr x16,#8 */; s[5] = 0xd61f0200 /* br x16 */; s[6] = (uint32_t)rest; s[7] = (uint32_t)(rest >> 32); }
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[hud] mprotect RX failed: touch HUD off"); munmap(t, 0x4000); return; }
    __builtin___clear_cache((char *)t, (char *)(t + 16));
    for (int i = 0; i < 2; i++) *hk[i].orig = (hu_fn)(void *)(t + 8 * i);
    for (int i = 0; i < 2; i++) {
        const uint64_t to = (uint64_t)(uintptr_t)(void *)hk[i].host;
        const uint32_t jump[4] = {0x58000050, 0xd61f0200, (uint32_t)to, (uint32_t)(to >> 32)};
        if (hu_poke(m, hk[i].off, jump, 4)) {
            LOG("【红】[hud] mprotect RW failed at main+%#x: touch HUD off", hk[i].off);
            if (i == 1) hu_poke(m, hk[0].off, hk[0].pro, 4);   // take the first hook back: never one without the other
            return; }
    }
    g_hooked = 1;
    char list[200]; size_t o = 0; list[0] = 0;
    for (int i = 0; i < HU_NTYPES; i++) if (g_t[i].on && o + 8 < sizeof list) o += (size_t)snprintf(list + o, sizeof list - o, "%s%#x", o ? ", " : "", g_t[i].type);
    LOG("[hud] touch HUD: create-all main+%#x / destroy-all main+%#x hooked (after hle_dos2_touch.c compared them); a player UI set built with the mode byte 0 gets the PC branch of the set (HUD + panels: types %s) "
        "from the host, handles %s (movies: the `touch` path table) %s; active now: %s; DM_DOS2_TOUCH_HUD_SCALE %.3f%s; a live switch needs %s and no dialog / loading / character creation on screen",
        HU_CREATE_ALL, HU_DESTROY_ALL, list, g_slots && g_slots_ok ? "in the record's PC slots" : "NOT in the record", g_env > 0 ? "in every UI mode (DM_DOS2_TOUCHUI_HUD=1)" : "in UI mode touch", hu_on() ? "yes" : "no (UI mode not touch)", g_tune,
        !g_scale_ok ? " (NOT applied: sites differ)" : g_tune == 0 ? " (class scales left to the engine)" : " (class scale = it x iPad class scale / S)",
        g_any_state ? "any game state (logged)" : "a game state of DM_DOS2_TOUCHUI_HUD_STATES");
}
#endif
