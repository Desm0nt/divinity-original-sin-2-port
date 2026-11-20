// [DOS2] The iPad touch UI (`*_t.swf`) on the Switch engine: first slice, TU-01..03 of docs/dos2/TOUCH_UI_PLAN.md (DM_DOS2_TOUCHUI, default 0).
// main = main.raw of update v786432, build 4979b200, offsets = main.raw. Static only so far (not run on a phone when written).
//   The UI type table is a hash in the UI manager ([main+0x2bbb230]): +0xd8 u32 buckets, +0xe0 bucket array, +0xe8 u32 entries; node
//   {+0 next, +8 u32 type id, +0x10 creator}; creator = a 0x18-byte block {+0 function, +8 engine string = the movie path}. The string
//   (0xaebdb8, 0x10b1190): byte 0 bit 0 = long form {+0 u32 (allocated size | 1), +4 u32 length, +8 char *}, else short {+0 u8 length << 1,
//   +1 chars} (up to 14 chars; every GUI path is longer). The registrar (0x10a7000..0x10b3a00) fills it once at start-up:
//     0x10b1128  type 0x48: path literal main+0x1ef529b "GUI/mainMenu_c.swf" (-> 0x10b4670), creator function x24 = main+0x10b56e0 (shared
//                with mainMenu.swf 0x1c), allocated size 32 for the 18 chars (0x10b1168..0x10b1198: (len + 16) & ~15, | 1 stored), then the
//                hash insert `mov w22,#0x48 ... str w22,[node,#8]; str x19,[node,#0x10]` (0x10b11cc..0x10b1204)
//     0x10b1464  type 0x57: the same literal, creator main+0x10b6990 (shared with mainMenu.swf 0x32)
//   CreateUIObject = 0xaebbf0(mgr, w1 layer, w2 type, w3 flags, x4, w5 player), 93 callers: bucket = type % [mgr+0xd8], walk the chain for
//   the id (0xaebc10..0xaebc44), then `ldr x0,[node,#0x10]; ldr x8,[x0],#8; blr x8` = creator function(&path) (0xaebc48..0xaebc54) -> the
//   object (its copy of the path at +0x20, its type at +0xf0); the result is a handle, `tst x0,#0x3fffff00000000` = 0 means failed
//   (0xaebfc8 / 0x1180348). The main menu is made at 0x10c68e0 (type 0x48, layer 0x13, flags 0x20021, x4 0x80) and at 0x1180340 (type
//   0x57, flags 0x1e21), neither looks at the controller mode byte. So the movie of a type is DATA: the host rewrites the creator's
//   path and the game's own class (the one mainMenu.swf and mainMenu_c.swf share) plays the iPad movie.
//   The movie comes from an extra package: the engine mounts every package of rom:/Data it lists - FindFiles(dir, "*.pak") at
//   0x82fea4 (Data), 0x82fedc (Data/Localization), 0x82ff1c (the language), 0x82ff48, 0x82ff80 (MinData), the "*.pak" literal
//   main+0x1e8671a; each name only goes through the extension test 0x828ea0, there is no list of known packages - so Data/TouchUI.pak
//   (tools/dos2_touchui_pak.py, LSPK v13 like Game.pak, Public/Game/GUI/<name>_t.swf) is mounted by the game itself. The host's rom
//   index (DM_FS_ROM_INDEX, hle_fs.c) walks the RomFS directory at mount, so a file put into <romfs>/Data is listed like the others;
//   nothing in hle_fs.c had to change for the mount. A DM_ROMFS_OVERLAY directory would work too (the overlay is merged into directory
//   listings) but turns the rom index off (every lookup back on FUSE), so the package goes into the RomFS folder itself. Loose files
//   (Data/Public/Game/GUI/x.swf outside a package) were not traced in this build and are not used.
//   Calls the class makes that the `_t` movie lacks (mainMenu_c has addBtnHint, allowChange, clearBtnHints, hideHintsAndCopyright,
//   setPressToJoin, setProfile, setProfileP2, showDifficultyIcon, showLoadingIcon; mainMenu_t does not): Iggy 1.2.56
//   IggyPlayerCallMethodRS looks the method up and for "undefined" (7) reports "No method %n found in IggyPlayerCallMethodRS" through
//   the warning callback and returns the error code 0x1f8 (0x3d0370..0x3d03e8; "not a function" = 0x1f7): no call, no crash.
// What the host does (all data, one code hook):
//   - at install: every site above is compared word for word (none uses x18, which the loader rewrites); TouchUI.pak is found in the
//     RomFS (or an overlay) and its file list read; a movie is only substituted when the package has it - without the package the
//     menus stay `_c` (one red line) and nothing is touched;
//   - CreateUIObject's first four instructions (none PC-relative) are replaced by a jump to the host (trampoline as hle_dos2_touch.c);
//     the first call (the registrar has run by then) rewrites the creators' paths - in place when the new path fits the string's
//     allocation (same length for `_c` -> `_t`), else the pointer and length are swapped for a host buffer that lives for the whole
//     run - and every call of a substituted type is logged with the handle it got;
//   - a substituted type whose creation fails gets its old path back and is created again (the old `_c` movie), one red line;
//   - DM_DOS2_TOUCH=1 and DM_DOS2_TOUCH_KBM=1 are set for hle_dos2_touch.c (installed right after this): the controller mode byte
//     stays 1, the pad works as before, mouse records from touches pass the listener filter ([mgr+0x170] = 1).
// Phone run tu03 (15.11, pack 2p), two corrections:
//   1. The creator's string is not the literal: 0x10b4670 makes the full path of it, the table holds "rom:/Data/Public/Game/GUI/mainMenu_c.swf"
//      (type table dump of that run). The first version wanted the string to START with "GUI/" and skipped both types ("0 of 2"). Now the
//      part after the last "GUI/" is what is compared and replaced; the prefix stays the engine's.
//   2. Tooltip guard. CRASH sig=11 addr=0x1e4 pc=main+0x1121220 on a tap on a party portrait in game (the controller HUD got the mouse
//      through DM_DOS2_TOUCH_KBM=1). 0x10a6920 (a UI object's pending tooltip, called from the objects' update, 59 callers) ends in
//      `bl 0x10a6fc0; ldr x1,[x19,#0x100]; b 0x1120b50` (0x10a697c..0x10a6990) and `...; b 0x1121150` (0x10a6a90..0x10a6aa4). 0x10a6fc0
//      returns the tooltip UI object from 0x110e1f0 (handle [[main+0x3298140]+0x88]) or NULL when that UI does not exist - the PC tooltip
//      UI, never created in controller mode - and both tails use it without a check (0x1121220 strb wzr,[x19,#0x1e4]; the third user,
//      0x10a7094, does check: cbz x0). On the Switch nothing sets a pending tooltip without a mouse, so the hole is never reached.
//      The host puts the missing check at the entry of both functions (each has exactly one caller, that tail): x0 == 0 -> return.
//      Installed whenever DM_DOS2_TOUCH is on (DM_DOS2_TOUCH_TIPGUARD=0 = off). And DM_DOS2_TOUCHUI now sets DM_DOS2_TOUCH_KBM=2
//      (hle_dos2_touch.c: the filter is open in menus only), so the controller HUD gets no mouse events at all until its `_t` panels exist.
// TU-04 / TU-05 (15.11, branch dos2-tu04; run tu04 showed mainMenu_t on 0x48 reacting to touch, pad alive). Static findings:
//   - Most menus are made ONCE and live hidden: the player UI set (0x10c06d0) creates gameMenu_c 0x4d, msgBox_c 0x4b, tutorialBox_c 0x5e ...
//     at game load (log tu04), the boot makes two msgBox_c; options / save-load are made on demand. So a window's "lifetime" for the host =
//     the time it is VISIBLE. UIObject flags (+0x14): SetFlags 0xae411c (orr, bit 3 = visible: its tail shows the Flash player
//     [obj+0x18] vt+0x50(1) and adds the modal bits 0x1e00 to [uimgr+0x220] only when bit 3 is set), ClearFlags 0xae3f20 (bic); type at +0xf0
//     (CreateUIObject 0xaebc5c). Handle -> object: CreateUIObject stores the object at [[uimgr+0x10] + (handle & 0xffffffff) * 8] below the
//     count [uimgr+0x1c] (0xaebfe8..0xaec010); the destroy (0xaebba0..0xaebbb8) puts NULL there. The host keeps the handles of the
//     substituted objects it saw created and reads type + flags through that slot on every pad poll (game thread): gone = NULL slot or
//     another type there.
//   - Branch: tools/dos2_swf_abc.py diff says the `_t` movies of msgBox, saveLoad, optionsSettings, tutorialBox, characterCreation have the
//     PC contract (addButton / addYesButton, addSave / addLoad, no addMenuCheckbox / addMenuSlider ...), far from `_c` (addOption, addNewSave,
//     addMenuButton ...); gameMenu_t is close to both, mainMenu_t works on the `_c` branch (run tu04). The classes pick their branch from their
//     OWN type: the main menu makes mods_c 0x67 when [obj+0xf0] == 0x48 (0x118039c), options_c 0x5b when 0x57 (0x11a2f98); the msgBox input
//     path wants `mode byte && [obj+0x178]` (0x1117ffc, 0x11182f0). The `_c` and PC ids of these menus share ONE creator (same class):
//     0x4b/0x1d, 0x4c/0x12, 0x4a/0x27, 0x58/0x01, 0x59/0x11, 0x5a/0x23, 0x5b/0x2d, 0x5e/0x37, 0x04/0x03, 0x1b/0x1a.
//   TU-05 here = the PC branch by TYPE, not by the global mode byte: when the game creates the `_c` id, the hook creates the PC id of the
//   same class instead (its path substituted with the `_t` movie). The object's type is then the PC one, children it makes follow (options
//   from the menu ...), the mode byte stays 1 and the pad's records keep reaching the game. The "mode byte 0 for the window's lifetime"
//   variant is DM_DOS2_TOUCHUI_PCWIN=2 (as 1 + main+0x2bb8918 = 0 while a retyped window is visible, 1 again when none; in mode 0 the input
//   filter 0x844014 drops the pad's records for most listeners, so the pad is weak in that window - an A/B experiment only).
// Console touch (owner 15.11, docs/dos2/TOUCH_CONSOLE_PLAN.md; this supersedes "port the iPad UI" as the DEFAULT): the iPad movies look crooked
//   on the phone and characterCreation_t is broken there (run tu07), so DM_DOS2_TOUCHUI=1 now substitutes only what is made for the console look:
//   the main menu 0x48 / 0x57 -> mainMenu_d (mainMenu_c + touch, variant D). Every `_t` substitution is off by default and stays reachable for
//   experiments (DM_DOS2_TOUCHUI_SET=ipad = the old table with the PC twins, or DM_DOS2_TOUCHUI_MAP). Console movies get touch one by one as
//   `<name>_d.swf` (same name length as `_c`: written in place); DM_DOS2_TOUCHUI_SET=hints adds the ones tools/dos2_hint_touch.py makes.
//   The mouse filter (hle_dos2_touch.c KBM=2) follows: dm_dos2_touchui_gate(). A mouse click also runs the movie's "IE UIAccept" (the phone showed
//   that with the main menu), so an open filter over a console menu WITHOUT touch code means "tap anywhere = A". Hence:
//     out of game  open, unless a console menu that has no touch code is on screen (k_block: message box, save / load, options, mods, pause menu,
//                  character creation, server list) - there the pad, the Back gesture (= B, DmActivity) and the on-screen buttons work, a tap does
//                  nothing. So the splash / "press A" / loading screens take a tap anywhere as A, the main menu takes real taps.
//     in game      closed, unless a touch-enabled window with modal bits is on screen (DM_DOS2_TOUCHUI_FILTER=1: any touch-enabled window) and no
//                  k_block window is.
//   Pad buttons from a movie: ExternalInterface "dmPadBtn"(LS_Classes.controllerHelper BTN_* code) - what a tap on a button hint sends
//   (tools/dos2_hint_touch.py) - is taken by the host (hle_dos2_touch.c dm_dos2_touch_pad_pulse: the npad button is held for a few polls, the same
//   path as the on-screen pad). It is refused when the calling window is not modal while a modal console menu is on screen (a hint under a box).
// Switches:
//   DM_DOS2_TOUCHUI=1          on: the main menu 0x48 0x57 -> mainMenu_d (the console mainMenu_c with touch handlers + tappable button hints;
//                              DM_DOS2_TOUCHUI_DENY=48,57 keeps the stock mainMenu_c), the mouse filter rule above, "dmPadBtn", Back = B
//   DM_DOS2_TOUCHUI_SET=ipad   + the iPad `_t` menus (TU-04 / TU-05, the default until 15.11): msgBox 0x4b 0x4c 0x1d 0x12, gameMenu 0x4d 0x13, saveLoad
//                              0x4a 0x27, optionsSettings 0x58..0x5b 0x01 0x11 0x23 0x2d, optionsInput 0x0d, loadingScreen 0x17, userProfile 0x33,
//                              characterCreation 0x04 0x03, connectionMenu 0x21, serverlist 0x18 0x1a 0x1b, tutorialBox 0x5e 0x37 (only the movies the
//                              package has: TouchUI.pak of `tools/dos2_touchui_pak.py menu`)
//   (default since branch dos2-tc2: the `hints` set below is ON; DM_DOS2_TOUCHUI_SET=main = the main menu only, as pack 2v)
//   DM_DOS2_TOUCHUI_SET=hints  + console menus with tappable button hints and the tap-is-not-A guard (`tools/dos2_touchui_pak.py hints`): msgBox_d 0x4b
//                              0x4c, saveLoad_d 0x4a, optionsSettings_d 0x58..0x5b, mods_d 0x67, characterCreation_d 0x04, tutorialBox_d 0x5e,
//                              gameMenu_d 0x4d (pause), dialog_d 0x42 0x65 (msgBox_d / gameMenu_d / dialog_d: tools/dos2_list_touch.py). Not the
//                              default yet: whether a touch main menu UNDER such a box also gets the click is not known (plan, experiment E1)
//   DM_DOS2_TOUCHUI_PCWIN=1    (default with DM_DOS2_TOUCHUI) the `_c` ids of the PC-contract menus are created as their PC twin; =0: those
//                              `_c` ids keep the `_c` movie (a `_t` movie on the `_c` branch would be empty); =2: as 1 + mode byte 0 while shown
//   DM_DOS2_TOUCHUI_DENY=<ids> hex type ids left alone (no substitution, no retype), e.g. "4,3,5e,37"
//   DM_DOS2_TOUCHUI_FILTER=1   hle_dos2_touch.c KBM=2 opens the mouse filter in game for ANY visible substituted window; default only for a
//                              visible one with modal bits (0x1e00: pause menu, message box, save / load, options; a tutorial box is not modal)
//   DM_DOS2_TOUCHUI_MAP=<list> own table instead, "48:mainMenu_t,57:mainMenu_t,4b:msgBox_t" (hex type id : movie; "GUI/" and ".swf" are added)
//   DM_DOS2_TOUCHUI_LOG=1      dump the whole type table (id, creator, path) once and log every CreateUIObject call (first 400)
//   DM_DOS2_TOUCHUI_PAK=<name> package file name in <romfs>/Data (default TouchUI.pak); DM_DOS2_TOUCHUI_NOCHECK=1 = substitute without it
//   DM_DOS2_TOUCH_TIPGUARD=0   no tooltip guard (it is installed whenever DM_DOS2_TOUCH is on)
//   DM_DBG=1                   which creator was patched and how, each creation of a patched type with its handle, the first read of
//                              every movie of the package (the engine loading it)
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_uimode.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { TU_CREATE = 0xaebbf0, TU_LOOKUP = 0xaebc10, TU_CALL = 0xaebc3c, TU_MGR_LD = 0xaebc60, TU_RESULT = 0xaebfc8, TU_UIMGR = 0x2bbb230,
       TU_REG48 = 0x10b1128, TU_REG48_STR = 0x10b1190, TU_REG48_INS = 0x10b11cc, TU_REG48_NODE = 0x10b11f8, TU_REG57 = 0x10b1464,
       TU_SITE48 = 0x10c68dc, TU_SITE57 = 0x118032c, TU_PAKS = 0x82fea4, TU_PAK_GLOB = 0x1e8671a, TU_PATH_C = 0x1ef529b, TU_IGGY_NOMETHOD = 0x3d0370,
       TU_HASH_N = 0xd8, TU_HASH_B = 0xe0, TU_HASH_CNT = 0xe8, TU_OBJ_PATH = 0x20 };
static const uint32_t k_create[4] = {0xd10383ff /* sub sp,sp,#0xe0 */, 0xa9086ffc, 0xa90967fa, 0xa90a5ff8};
static const uint32_t k_lookup[6] = {0xb940d808 /* ldr w8,[x0,#0xd8] */, 0x1ac80849 /* udiv w9,w2,w8 */, 0x1b088928 /* msub w8,w9,w8,w2 */,
                                     0xf9407009 /* ldr x9,[x0,#0xe0] */, 0xf8685928 /* ldr x8,[x9,w8,uxtw #3] */, 0xb4002aa8 /* cbz x8, fail */};
static const uint32_t k_call[7] = {0xb9400909 /* ldr w9,[x8,#8]: node id */, 0x6b19013f, 0x54002801, 0xf9400900 /* ldr x0,[x8,#0x10]: creator */,
                                   0xf9000be4, 0xf8408408 /* ldr x8,[x0],#8: function, x0 = &path */, 0xd63f0100 /* blr x8 */};
static const uint32_t k_mgr_ld[3] = {0x90010688 /* adrp x8,main+0x2bbb000 */, 0xb9452d2a, 0xf9411908 /* ldr x8,[x8,#0x230]: the UI manager */};
static const uint32_t k_result[2] = {0xf260541f /* tst x0,#0x3fffff00000000 */, 0x54000c40 /* b.eq failed */};
static const uint32_t k_reg48[8] = {0x90007221 /* adrp x1,main+0x1ef5000 */, 0x910a6c21 /* add x1,x1,#0x29b */, 0x910003e0, 0x94000d4f /* bl 0x10b4670 */,
                                    0xaa1303e0, 0xf9000278 /* str x24,[x19]: creator function */, 0xf9000a7f, 0xf8008c1f /* str xzr,[x0,#8]!: the string */};
static const uint32_t k_reg48_str[3] = {0x320002e8 /* orr w8,w23,#1 */, 0xf9000a60 /* str x0,[x19,#0x10]: chars */, 0x29015268 /* stp w8,w20,[x19,#8]: size|1, length */};
static const uint32_t k_reg48_ins[7] = {0xf9411b34 /* ldr x20,[x25,#0x230] */, 0xb940da88 /* ldr w8,[x20,#0xd8] */, 0x52800916 /* mov w22,#0x48 */, 0x1ac80ac9,
                                        0x1b08d935, 0xf9407288 /* ldr x8,[x20,#0xe0] */, 0xf8755908};
static const uint32_t k_reg48_node[4] = {0xf900001f, 0xb9000816 /* str w22,[x0,#8] */, 0xd37df2a9, 0xf9000813 /* str x19,[x0,#0x10] */};
static const uint32_t k_reg57[6] = {0x90007221, 0x910a6c21, 0x910003e0, 0x94000c80 /* bl 0x10b4670 */, 0xb0000028 /* adrp x8,main+0x10b6000 */, 0x91264108 /* add x8,x8,#0x990 */};
static const uint32_t k_site48[6] = {0x52800261 /* mov w1,#0x13 */, 0x52800902 /* mov w2,#0x48 */, 0x52800423, 0x72a00043 /* flags 0x20021 */, 0x321903e4, 0x97e894c0 /* bl 0xaebbf0 */};
static const uint32_t k_site57[6] = {0x52800ae2 /* mov w2,#0x57 */, 0x5283c423 /* mov w3,#0x1e21 */, 0xaa1403e0, 0x321903e4, 0x2a1f03e5, 0x97e5ae2c /* bl 0xaebbf0 */};
static const uint32_t k_paks[7] = {0xf000b2a1 /* adrp x1,main+0x1e86000 */, 0x911c6821 /* add x1,x1,#0x71a: "*.pak" */, 0x910083e0, 0x9100e3e3, 0x2a1f03e2, 0x2a1f03e4,
                                   0x97ff89c5 /* bl 0x8125d0: FindFiles */};
static const uint32_t k_iggy_nomethod[7] = {0xf1001f5f /* cmp x26,#7 */, 0x54000241, 0x9000d742, 0x910d3442 /* "No method %n found .." */, 0xaa1303e0,
                                            0x321d17e1 /* mov w1,#0x1f8 */, 0x14000011};
// TU-04 / TU-05: object table, flags, the mode byte's meaning (read only; the header's TU-04 part)
enum { TU_OBJTYPE = 0xaebc5c, TU_OBJSLOT = 0xaebfe8, TU_SETFLAGS = 0xae411c, TU_CLRFLAGS = 0xae3f20, TU_FREESLOT = 0xaebba0, TU_MODESITE = 0x11d9ee0,
       TU_MODE = 0x2bb8918, TU_MGR_OBJS = 0x10, TU_MGR_NOBJ = 0x1c, TU_OBJ_FLAGS = 0x14, TU_OBJ_TYPE = 0xf0, TU_F_VISIBLE = 0x8, TU_F_MODAL = 0x1e00,
       TU_MODALSUM = 0xae4180, TU_MGR_MODAL = 0x220 };
// SetFlags' tail (UI mode switch, dm_dos2_touchui_ui_busy): a visible object's modal bits are OR-ed into [uimgr+0x220]
static const uint32_t k_modalsum[4] = {0xb942214c /* ldr w12,[x10,#0x220] */, 0x12170d0b /* and w11,w8,#0x1e00 */, 0x2a0b018b, 0xb902214b /* str w11,[x10,#0x220] */};
static const uint32_t k_objtype[1] = {0xb900f019 /* str w25,[x0,#0xf0]: the object's type */};
static const uint32_t k_objslot[7] = {0xb9401e69 /* ldr w9,[x19,#0x1c]: count */, 0x92407ec8 /* and x8,x22,#0xffffffff: index */, 0xeb09011f, 0x54000c62,
                                      0xf9400a69 /* ldr x9,[x19,#0x10]: objects */, 0xf868792a, 0xb500008a};
static const uint32_t k_setflags[7] = {0xb9401408 /* ldr w8,[x0,#0x14] */, 0x2a010108 /* orr */, 0x2a0103f3, 0xaa0003f4, 0x72170c3f /* tst w1,#0x1e00 */,
                                       0x121d0109 /* and w9,w8,#8: visible */, 0xb9001408};
static const uint32_t k_clrflags[5] = {0xb9401415 /* ldr w21,[x0,#0x14] */, 0x2a0103f3, 0xaa0003f4, 0x72170c3f, 0x0a2102a8 /* bic w8,w21,w1 */};
static const uint32_t k_freeslot[7] = {0xf9400a68 /* ldr x8,[x19,#0x10] */, 0xf8767909, 0xb4000089, 0xb9402269, 0x51000529, 0xb9002269, 0xf836791f /* str xzr: slot freed */};
static const uint32_t k_modesite[6] = {0xf000cee8 /* adrp x8,main+0x2bb8000 */, 0x39646108 /* ldrb w8,[x8,#0x918] */, 0x7100011f, 0x52800968 /* 0x4b */,
                                       0x528003a9 /* 0x1d */, 0x1a880122 /* csel: mode 0 -> msgBox.swf */};
static const char k_path_c[] = "GUI/mainMenu_c.swf", k_glob[] = "*.pak";
// the default table (TU-04): type, movie, the tail it must have now, the PC twin of the same class (TU-05; 0 = none / stays on its branch)
typedef struct { uint16_t id, pc; const char *movie, *expect; } tu_def;
static const tu_def k_def[] = {
    // main menu, variant D (owner 15.11: the console mainMenu_c LOOK, real touch): mainMenu_d.swf = mainMenu_c.swf + touch handlers in its
    // ABC (tools/dos2_mainmenu_touch.py; same name length, written in place; the type and so the class branch stay `_c`). Off:
    // DM_DOS2_TOUCHUI_DENY=48,57. The iPad movie instead: DM_DOS2_TOUCHUI_MAP=48:mainMenu_t,57:mainMenu_t (replaces the whole table).
    {0x48, 0, "mainMenu_d", "GUI/mainMenu_c.swf"}, {0x57, 0, "mainMenu_d", "GUI/mainMenu_c.swf"},
};
// DM_DOS2_TOUCHUI_SET=ipad: the iPad menus (TU-04 / TU-05; the default table until 15.11)
static const tu_def k_ipad[] = {
    {0x4b, 0x1d, "msgBox_t", "GUI/msgBox_c.swf"}, {0x4c, 0x12, "msgBox_t", "GUI/msgBox_c.swf"}, {0x1d, 0, "msgBox_t", "GUI/msgBox.swf"}, {0x12, 0, "msgBox_t", "GUI/msgBox.swf"},
    {0x4d, 0, "gameMenu_t", "GUI/gameMenu_c.swf"}, {0x13, 0, "gameMenu_t", "GUI/gameMenu.swf"},
    {0x4a, 0x27, "saveLoad_t", "GUI/saveLoad_c.swf"}, {0x27, 0, "saveLoad_t", "GUI/saveLoad.swf"},
    {0x58, 0x01, "optionsSettings_t", "GUI/optionsSettings_c.swf"}, {0x59, 0x11, "optionsSettings_t", "GUI/optionsSettings_c.swf"},
    {0x5a, 0x23, "optionsSettings_t", "GUI/optionsSettings_c.swf"}, {0x5b, 0x2d, "optionsSettings_t", "GUI/optionsSettings_c.swf"},
    {0x01, 0, "optionsSettings_t", "GUI/optionsSettings.swf"}, {0x11, 0, "optionsSettings_t", "GUI/optionsSettings.swf"},
    {0x23, 0, "optionsSettings_t", "GUI/optionsSettings.swf"}, {0x2d, 0, "optionsSettings_t", "GUI/optionsSettings.swf"},
    {0x0d, 0, "optionsInput_t", "GUI/optionsInput.swf"}, {0x17, 0, "loadingScreen_t", "GUI/loadingScreen.swf"}, {0x33, 0, "userProfile_t", "GUI/userProfile.swf"},
    {0x04, 0x03, "characterCreation_t", "GUI/characterCreation_c.swf"}, {0x03, 0, "characterCreation_t", "GUI/characterCreation.swf"},
    {0x21, 0, "connectionMenu_t", "GUI/connectionMenu.swf"},
    {0x18, 0, "serverlist_t", "GUI/serverlist.swf"}, {0x1a, 0, "serverlist_t", "GUI/serverlist.swf"}, {0x1b, 0x1a, "serverlist_t", "GUI/serverlist_c.swf"},
    {0x5e, 0x37, "tutorialBox_t", "GUI/tutorialBox_c.swf"}, {0x37, 0, "tutorialBox_t", "GUI/tutorialBox.swf"},
};
// DM_DOS2_TOUCHUI_SET=hints: console menus patched by tools/dos2_hint_touch.py (button hints act as pad buttons, a tap is not "accept"); characterCreation_d = that + taps on its own elements (tools/dos2_cc_touch.py)
static const tu_def k_hint[] = {
    {0x4b, 0, "msgBox_d", "GUI/msgBox_c.swf"}, {0x4c, 0, "msgBox_d", "GUI/msgBox_c.swf"}, {0x4a, 0, "saveLoad_d", "GUI/saveLoad_c.swf"},
    {0x58, 0, "optionsSettings_d", "GUI/optionsSettings_c.swf"}, {0x59, 0, "optionsSettings_d", "GUI/optionsSettings_c.swf"},
    {0x5a, 0, "optionsSettings_d", "GUI/optionsSettings_c.swf"}, {0x5b, 0, "optionsSettings_d", "GUI/optionsSettings_c.swf"},
    {0x67, 0, "mods_d", "GUI/mods_c.swf"}, {0x04, 0, "characterCreation_d", "GUI/characterCreation_c.swf"}, {0x5e, 0, "tutorialBox_d", "GUI/tutorialBox_c.swf"},
    // Options -> Controls (the pad layout picture, owner 21.11: its "B Back" took no tap in either mode): a tap on the hint = dmPadBtn(1), the pad's B
    // (tools/dos2_taps3_touch.py add_blback); its own code closes it on IE UICancel (PlaySound UI_Gen_Back + requestCloseUI). Off: DM_DOS2_TOUCHUI_DENY=5f
    {0x5f, 0, "buttonLayoutNX_d", "GUI/buttonLayoutNX_c.swf"},
    // tools/dos2_list_touch.py: pause menu (taps on its list), dialog answers (a tap runs THAT answer; dialog_c's own drag / release code stays)
    {0x4d, 0, "gameMenu_d", "GUI/gameMenu_c.swf"}, {0x42, 0, "dialog_d", "GUI/dialog_c.swf"}, {0x65, 0, "dialog_d", "GUI/dialog_c.swf"},
    // the HUD with the iPad's on-screen buttons drawn in (owner 15.11, docs/dos2/TOUCH_VISIBLE.md): bottomBar_d = bottomBar_c + panel column, hot bar row arrows, cancel,
    // end turn (tools/dos2_hud_touch.py; "dmPanel" -> hle_dos2_touchui_vis.c). Off: DM_DOS2_TOUCHUI_DENY=3b
    {0x3b, 0, "bottomBar_d", "GUI/bottomBar_c.swf"},
    // the iPad button column in its own movie (owner 15.11, packs 3i..3o: grafted into bottomBar_d it could not reach the screen edge - Iggy draws a
    // movie only inside its rect, bottomBar_c is a centred 1920 frame). watermark.swf's type 0x8d (never created on the console; creator main+0x10b9520,
    // a plain UIObject) plays GUI/dmColumn_d.swf = watermark.swf + the column, anchored topRight / screen like the minimap (tools/dos2_column_touch.py).
    // The host creates it next to bottomBar_c and shows / hides it with bottomBar (tu_col_*). Off: DM_DOS2_TOUCHUI_DENY=8d (the column is then gone).
    {0x8d, 0, "dmColumn_d", "GUI/watermark.swf"},
    {0x3c, 0, "minimap_d", "GUI/minimap_c.swf"},   // tap on the minimap = the pad's '-' (open the map), tools/dos2_minimap_touch.py; DM_DOS2_TOUCHUI_DENY=3c = stock
    // the panels with a row of the iPad's buttons drawn in (close, tab arrows, item menu, info, helmet, + / -, balance, confirm; tools/dos2_panel_touch.py,
    // TOUCH_VISIBLE.md section 5): the buttons call "dmPadBtn" (hle_dos2_touch.c) or the movie's own ExternalInterface. Off per panel: DM_DOS2_TOUCHUI_DENY=<type>
    {0x8e, 0, "partyInventory_d", "GUI/partyInventory_c.swf"}, {0x40, 0, "equipmentPanel_d", "GUI/equipmentPanel_c.swf"}, {0x3f, 0, "statsPanel_d", "GUI/statsPanel_c.swf"},
    {0x3e, 0, "inventorySkillPanel_d", "GUI/inventorySkillPanel_c.swf"}, {0x46, 0, "journal_d", "GUI/journal_c.swf"}, {0x8c, 0, "journal_csp_d", "GUI/journal_csp.swf"}, {0x54, 0, "craftPanel_d", "GUI/craftPanel_c.swf"},
    {0x49, 0, "trade_d", "GUI/trade_c.swf"}, {0x89, 0, "reward_d", "GUI/reward_c.swf"}, {0x43, 0, "examine_d", "GUI/examine_c.swf"},
    // portraits (run tw2: the release never reached the stock MOUSE_UP listener, no charSel): playerInfo_d = playerInfo.swf + a stage release that runs the
    // stock onIconUp, second tap = the character sheet (tools/dos2_taps_touch.py; the same pass also fixes msgBox_d prompts, tutorialBox_d "hold A" and
    // the hot bar slots of bottomBar_d). Off: DM_DOS2_TOUCHUI_DENY=26,3d,73
    {0x26, 0, "playerInfo_d", "GUI/playerInfo.swf"}, {0x3d, 0, "playerInfo_d", "GUI/playerInfo.swf"}, {0x73, 0, "playerInfo_d", "GUI/playerInfo.swf"},
    // the overhead texts / the selected target's info (branch dos2-dpadbtns): overhead_d = overhead.swf + tools/dos2_hudrect_touch.py only - its "(X) Actions..."
    // line reports its rect as "hint3", a world tap there = the pad's X (hle_dos2_touch.c dt_hint_tap). The type still gets no mouse (route.c). Off: DM_DOS2_TOUCHUI_DENY=5
    {0x05, 0, "overhead_d", "GUI/overhead.swf"},
};
// UI mode TOUCH (hle_dos2_uimode.c, owner 19.11): the mode byte is 0 there, so the game creates the PC types (msgBox 0x1d, gameMenu 0x13, saveLoad
// 0x27, hotBar 0x28 through hle_dos2_touchui_hud.c ...); this table maps PC types to our PC-based `<name>_d.swf` movies (docs/dos2/TOUCH_MODES_PLAN.md
// T2). An entry is only substituted when TouchUI.pak has its file (tu_apply), the rule of every table here: without it the type keeps the
// player's stock PC movie. More entries for a test: DM_DOS2_UI_TOUCH_MAP=<hex id>:<movie>,... (the format of DM_DOS2_TOUCHUI_MAP; added to this table).
// The table of SWITCH is empty by design (stock `_c` movies); HYBRID's is k_def + k_hint (or DM_DOS2_TOUCHUI_SET / _MAP) as before.
// The HUD of UI mode TOUCH (docs/dos2/TOUCH_MODES_PLAN.md "Implemented: touch HUD"): the PC HUD types the host creates (hle_dos2_touchui_hud.c)
// play the PC movies laid out as the iPad's HUD (tools/dos2_pchud_touch.py, package set `touch`); 0x8d (watermark.swf, a plain UIObject: the
// hybrid's column type) plays the iPad's right side there: options, the helmet button, the panel column.
static const tu_def k_touch[] = {
    {0x28, 0, "hotBar_p", "GUI/hotBar.swf"}, {0x1e, 0, "minimap_p", "GUI/minimap.swf"}, {0x26, 0, "playerInfo_p", "GUI/playerInfo.swf"},
    {0x75, 0, "statusConsole_p", "GUI/statusConsole.swf"}, {0x07, 0, "combatLog_p", "GUI/combatLog.swf"}, {0x8d, 0, "dmSide_p", "GUI/watermark.swf"},
    // the PC panels (hle_dos2_touchui_hud.c makes the set's ones and puts them into the player's record; waypoints is made by the game on demand):
    // the PC movies + the iPad's 50 px close button (tools/dos2_pchud_touch.py PANEL_CLOSE)
    {0x74, 0, "partyInventory_p", "GUI/partyInventory.swf"}, {0x77, 0, "characterSheet_p", "GUI/characterSheet.swf"}, {0x29, 0, "skills_p", "GUI/skills.swf"},
    {0x66, 0, "uiCraft_p", "GUI/uiCraft.swf"}, {0x16, 0, "journal_p", "GUI/journal.swf"}, {0x68, 0, "examine_p", "GUI/examine.swf"}, {0x2f, 0, "waypoints_p", "GUI/waypoints.swf"},
    // the windows the game makes on demand in mode 0, laid out as the iPad's (iPad cells 76, centred windows, finger-sized buttons, dialog answers as
    // tappable rows; tools/dos2_ppanel_trade.py): trade 0x2e, container 9 / 0x25 (two classes, one movie), dialog 0xe, reward 0x88, item splitter 0x15
    {0x2e, 0, "trade_p", "GUI/trade.swf"}, {0x09, 0, "containerInventory_p", "GUI/containerInventory.swf"}, {0x25, 0, "containerInventory_p", "GUI/containerInventory.swf"},
    {0x0e, 0, "dialog_p", "GUI/dialog.swf"}, {0x88, 0, "reward_p", "GUI/reward.swf"}, {0x15, 0, "itemSplitter_p", "GUI/itemSplitter.swf"},
    // the video window: its skip hint says "Double tap to skip" (tools/dos2_ptouch_gaps.py; the double tap itself: hle_dos2_touch_gaps.c)
    {0x20, 0, "subtitles_p", "GUI/subtitles.swf"},
    // the loading screen: one of our own touch tips before every second engine tip (tools/dos2_ptouch_tips.py; the keyboard-only engine tips
    // leave the pick in hle_dos2_touch_tips.c)
    {0x17, 0, "loadingScreen_p", "GUI/loadingScreen.swf"},
    {0, 0, NULL, NULL}   // end
};
// console menus that take the whole input while shown: with one of them on screen and no touch code in it the mouse filter stays closed
// (a click would be its "accept"). A type that is substituted with a touch movie is a touch window instead.
static const uint16_t k_block[] = {0x4b, 0x4c, 0x1d, 0x12, 0x4a, 0x27, 0x58, 0x59, 0x5a, 0x5b, 0x01, 0x11, 0x23, 0x2d, 0x0d, 0x67, 0x4d, 0x13, 0x04, 0x03, 0x1b, 0x1a, 0x18};

typedef struct { uint32_t off, n; const uint32_t *want; } tu_site;
static int tu_poke(module_t *m, uint32_t off, const uint32_t *w, int n) {   // code is sealed RX in 16 KB granules (as hle_dos2_touch.c dt_poke)
    uint32_t *at = (uint32_t *)(void *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    const size_t len = (size_t)(((uintptr_t)(at + n) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, w, 4 * (size_t)n);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[touchui] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + n));
    return 0;
}
typedef struct { uint32_t cap1, len; char *ptr; } tu_str;          // the engine string, long form
typedef struct { void *fn; tu_str path; } tu_creator;
typedef struct tu_node { struct tu_node *next; uint32_t id, pad; tu_creator *cr; } tu_node;
_Static_assert(sizeof(tu_creator) == 0x18 && sizeof(tu_node) == 0x18, "creator / hash node are 0x18 bytes");
typedef uint64_t (*tu_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

enum { TU_MAX = 128, TU_PATH = 96, TU_FULL = 256 };
typedef struct {
    uint32_t id; int state;                 // 0 = to do, 1 = substituted, 2 = created as its PC twin (TU-05), -1 = skipped / given back
    uint32_t pc; int pc_ok;                 // the PC twin of the same class (0 = none); 1 = checked: same creator function, twin substituted
    char path[TU_PATH], old[TU_PATH];       // "GUI/x_t.swf"; the path it had
    tu_str saved; tu_creator *cr; int in_place; uint32_t tail, old_len;
    const char *expect;                     // the "GUI/x.swf" tail this type must have now (the default table), NULL = any
    char full[TU_FULL];                     // the whole new path (the engine's prefix + path) when it does not fit in place
    unsigned modes;                         // UI modes whose table holds it (hle_dos2_uimode.h ui_mode_mask); states 4 / 5 = 1 / 2 prepared but parked
} tu_map;
static tu_map g_map[TU_MAX];
static int g_nmap, g_on, g_log, g_dbg, g_applied, g_nocheck, g_pcwin = 1, g_filter_any, g_watch_ok, g_mode_ok;
static int g_ui = UI_HYBRID, g_modal_ok;   // the UI mode whose path table is written now (hle_dos2_uimode.c); [uimgr+0x220] checked (dm_dos2_touchui_ui_busy)
static uint32_t g_deny[32]; static int g_ndeny;
// TU-04: the substituted objects the host saw created (handle, type), polled for visibility from hle_dos2_touch.c (game thread)
enum { TU_LIVE = 96 };
typedef struct { uint64_t h; uint32_t type, asked; int vis; uint32_t flags; int touch; } tu_live;   // touch 0 = a k_block window (watched only)
static tu_live g_live[TU_LIVE]; static int g_nlive;
static _Atomic unsigned g_nev;   // window events logged
static module_t *g_main;
static tu_fn g_create_orig;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned g_ncalls;

// ---- the package: Data/TouchUI.pak, read by the host only to know what it holds (the game mounts and reads it itself) ----
typedef struct { char name[256]; uint32_t off, disk, size; _Atomic int seen; } tu_file;
static tu_file *g_files; static int g_nfiles;
static char g_pak_name[64] = "TouchUI.pak", g_pak_host[1024];
uint8_t g_dm_dos2_touchui_fd[4096];   // hle_fs.c: descriptors of the package the game has open (1 = log the first read of each movie)

static int tu_lz4(const uint8_t *s, size_t sn, uint8_t *d, size_t dn) {   // LZ4 block, every access bounds-checked; -1 = corrupt
    size_t si = 0, di = 0;
    while (si < sn) {
        const unsigned t = s[si++]; size_t lit = t >> 4, ml = t & 15;
        if (lit == 15) { unsigned b; do { if (si >= sn) return -1; b = s[si++]; lit += b; } while (b == 255); }
        if (lit > sn - si || lit > dn - di) return -1;
        memcpy(d + di, s + si, lit); si += lit; di += lit;
        if (si >= sn) break;   // the last sequence has literals only
        if (sn - si < 2) return -1;
        const size_t off = (size_t)s[si] | (size_t)s[si + 1] << 8; si += 2;
        if (!off || off > di) return -1;
        if (ml == 15) { unsigned b; do { if (si >= sn) return -1; b = s[si++]; ml += b; } while (b == 255); }
        ml += 4;
        if (ml > dn - di) return -1;
        for (size_t k = 0; k < ml; k++) d[di + k] = d[di - off + k];
        di += ml;
    }
    return (int)di;
}
// LSPK v13 (tools/lspk.py): the last 8 bytes = u32 header size + "LSPK"; header = u32 13, u32 list offset, u32 list size, ...; list = u32 count +
// one LZ4 block of count x 280 bytes {name[256], u32 offset, u32 size on disk, u32 size, u32 part, u32 flags, u32 crc}
static int tu_pak_read(const char *host) {
    FILE *f = fopen(host, "rb"); if (!f) return 0;
    uint8_t tail[8], hd[12], *raw = NULL, *tab = NULL; int ok = 0; long size;
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 48 || fseek(f, size - 8, SEEK_SET) || fread(tail, 1, 8, f) != 8 || memcmp(tail + 4, "LSPK", 4)) goto out;
    uint32_t hs, ver, lo, ls, n;
    memcpy(&hs, tail, 4);
    if (hs < 20 || (long)hs > size || fseek(f, size - (long)hs, SEEK_SET) || fread(hd, 1, 12, f) != 12) goto out;
    memcpy(&ver, hd, 4); memcpy(&lo, hd + 4, 4); memcpy(&ls, hd + 8, 4);
    if (ver != 13 || ls < 5 || ls > (16u << 20) || (uint64_t)lo + ls > (uint64_t)size || fseek(f, (long)lo, SEEK_SET)) goto out;
    if (!(raw = malloc(ls)) || fread(raw, 1, ls, f) != ls) goto out;
    memcpy(&n, raw, 4);
    if (!n || n > 4096 || !(tab = malloc((size_t)n * 280)) || tu_lz4(raw + 4, ls - 4, tab, (size_t)n * 280) != (int)(n * 280)) goto out;
    if (!(g_files = calloc(n, sizeof *g_files))) goto out;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = tab + (size_t)i * 280; tu_file *t = &g_files[i];
        memcpy(t->name, e, 255); t->name[255] = 0;
        memcpy(&t->off, e + 256, 4); memcpy(&t->disk, e + 260, 4); memcpy(&t->size, e + 264, 4);
        if (!t->size) t->size = t->disk;
    }
    g_nfiles = (int)n; ok = 1;
out:
    free(raw); free(tab); fclose(f);
    return ok;
}
static int tu_pak_has(const char *gui_path) {   // "GUI/x.swf" -> Public/Game/GUI/x.swf in the package
    char full[300]; snprintf(full, sizeof full, "Public/Game/%s", gui_path);
    for (int i = 0; i < g_nfiles; i++) if (!strcasecmp(g_files[i].name, full)) return 1;
    return 0;
}
static int tu_pak_find(void) {
    int dm_overlay_file(const char *rel, char *out, size_t n);
    char rel[128]; snprintf(rel, sizeof rel, "Data/%s", g_pak_name);
    if (dm_overlay_file(rel, g_pak_host, sizeof g_pak_host)) return 2;
    const char *r = getenv("DM_ROMFS");
    if (r && *r && snprintf(g_pak_host, sizeof g_pak_host, "%s/%s", r, rel) < (int)sizeof g_pak_host && !access(g_pak_host, R_OK)) return 1;
    return 0;
}
// hle_fs.c OpenFile: is this descriptor the package (descriptors are reused, so every open sets or clears its slot)
void dm_dos2_touchui_open(const char *path, int fd);
void dm_dos2_touchui_open(const char *path, int fd) {
    if (fd < 0 || fd >= (int)sizeof g_dm_dos2_touchui_fd) return;
    int mine = 0;
    if (g_on && g_nfiles && path) { const size_t pl = strlen(path), nl = strlen(g_pak_name); mine = pl > nl && path[pl - nl - 1] == '/' && !strcasecmp(path + pl - nl, g_pak_name); }
    if (mine) { static _Atomic int once; if (!atomic_exchange(&once, 1)) LOG("[touchui] the game opened %s: the package is mounted", path); }
    g_dm_dos2_touchui_fd[fd] = (uint8_t)mine;
}
// hle_fs.c ReadFile on such a descriptor: the first read inside a movie = the engine is loading it
void dm_dos2_touchui_read(int64_t off, size_t n);
void dm_dos2_touchui_read(int64_t off, size_t n) {
    for (int i = 0; i < g_nfiles; i++) {
        tu_file *t = &g_files[i];
        if (off + (int64_t)n <= (int64_t)t->off || off >= (int64_t)t->off + (int64_t)t->disk) continue;
        if (!atomic_exchange(&t->seen, 1)) LOG("[touchui] movie load: the game reads %s from %s (offset %#x, %u B packed, %u B)", t->name, g_pak_name, t->off, t->disk, t->size);
        return;
    }
}

// ---- the type table ----
static int tu_str_get(const tu_str *s, const char **p, uint32_t *len) {   // long form with sane fields, or the short form
    const uint8_t b0 = *(const uint8_t *)s;
    if (b0 & 1) { if (!s->ptr || s->len >= 512 || s->len + 1 > (s->cap1 & ~1u)) return 0; *p = s->ptr; *len = s->len; return 1; }
    *p = (const char *)s + 1; *len = b0 >> 1; return *len <= 14;
}
static tu_node *tu_find(uint8_t *mgr, uint32_t id) {
    const uint32_t nb = *(volatile uint32_t *)(mgr + TU_HASH_N);
    tu_node *volatile *bk = *(tu_node *volatile *volatile *)(mgr + TU_HASH_B);
    if (!bk || !nb || nb > 0x10000) return NULL;
    int guard = 0;
    for (tu_node *nd = bk[id % nb]; nd && guard < 4096; nd = nd->next, guard++) if (nd->id == id) return nd;
    return NULL;
}
static void tu_dump(uint8_t *mgr) {
    const uint32_t nb = *(volatile uint32_t *)(mgr + TU_HASH_N), cnt = *(volatile uint32_t *)(mgr + TU_HASH_CNT);
    tu_node *volatile *bk = *(tu_node *volatile *volatile *)(mgr + TU_HASH_B);
    LOG("[touchui] UI type table: manager %p, %u buckets, %u types", (void *)mgr, nb, cnt);
    if (!bk || !nb || nb > 0x10000) return;
    for (uint32_t id = 0; id < 0x100; id++) {
        tu_node *nd = tu_find(mgr, id); const char *p; uint32_t len;
        if (!nd || !nd->cr) continue;
        if (tu_str_get(&nd->cr->path, &p, &len)) LOG("[touchui]   type %#04x creator main+%#llx %.*s", id, (unsigned long long)((uint8_t *)nd->cr->fn - g_main->base), (int)len, p);
        else LOG("[touchui]   type %#04x creator main+%#llx (path not readable)", id, (unsigned long long)((uint8_t *)nd->cr->fn - g_main->base));
    }
}
static int tu_on(const tu_map *m) { return ui_mode_entry_on(m->modes, g_ui); }   // in the table of the UI mode written now
static void tu_write(tu_map *m) {   // a prepared entry (state 4) -> its path in the creator (state 1); under g_lock, game thread
    tu_creator *cr = m->cr; const uint32_t pl = (uint32_t)strlen(m->path), nl = m->tail + pl;
    if (m->in_place) { memcpy(cr->path.ptr + m->tail, m->path, pl + 1); cr->path.len = nl; }   // the engine's own buffer, same allocation
    else { cr->path.ptr = m->full; cr->path.len = nl; }                                          // host buffer (static, never freed: the table lives for the whole run)
    m->state = 1;
}
static void tu_unwrite(tu_map *m) {   // state 1 -> the creator's own path back, the entry stays prepared (state 4) for a later mode change
    if (m->in_place) { memcpy(m->cr->path.ptr + m->tail, m->old, strlen(m->old) + 1); m->cr->path.len = m->old_len; }
    else m->cr->path = m->saved;
    m->state = 4;
}
static void tu_apply(uint8_t *mgr) {   // under g_lock, once, at the first CreateUIObject (the registrar is done by then)
    uint8_t *glob = *(uint8_t *volatile *)(g_main->base + TU_UIMGR);
    if (mgr != glob) { LOG("【红】[touchui] CreateUIObject manager %p is not [main+%#x] = %p: nothing substituted", (void *)mgr, TU_UIMGR, (void *)glob); g_on = 0; return; }
    if (g_log) tu_dump(mgr);
    if (!g_on) return;
    int done = 0, parked = 0;
    // pass 1: every entry of every mode's table reads the creator's own path (nothing is written yet: two tables may hold the same type)
    for (int i = 0; i < g_nmap; i++) {
        tu_map *m = &g_map[i]; m->state = -1;
        tu_node *nd = tu_find(mgr, m->id); const char *p; uint32_t len;
        if (!nd || !nd->cr) { LOG("【红】[touchui] type %#x is not in the type table: skipped", m->id); continue; }
        tu_creator *cr = nd->cr;
        // the table holds the full path ("rom:/Data/Public/Game/GUI/mainMenu_c.swf", run tu03): the tail from the last "GUI/" is the movie
        int tail = -1;
        if ((*(const uint8_t *)&cr->path & 1) && tu_str_get(&cr->path, &p, &len) && len >= 9 && len < TU_FULL && !memcmp(p + len - 4, ".swf", 4))
            for (int k = (int)len - 8; k >= 0; k--) if (!memcmp(p + k, "GUI/", 4) && (k == 0 || p[k - 1] == '/')) { tail = k; break; }
        if (tail < 0 || len - (uint32_t)tail >= TU_PATH) { LOG("【红】[touchui] type %#x: the creator's path is not a long-form .../GUI/*.swf string: skipped", m->id); continue; }
        memcpy(m->old, p + tail, len - (uint32_t)tail); m->old[len - (uint32_t)tail] = 0;
        if (m->expect && strcmp(m->old, m->expect)) { LOG("【红】[touchui] type %#x has %s, expected %s: skipped", m->id, m->old, m->expect); continue; }
        if (!strcmp(m->old, m->path)) { LOG("[touchui] type %#x already is %s", m->id, m->path); continue; }
        if (m->pc) { m->state = 2; continue; }   // a `_c` id of a PC-contract menu: never gets the `_t` movie itself (TU-05: created as its PC twin, checked below)
        if (!g_nocheck && !tu_pak_has(m->path)) { LOG("【红】[touchui] type %#x: %s has no Public/Game/%s: stays %s", m->id, g_pak_name, m->path, m->old); continue; }
        const uint32_t pl = (uint32_t)strlen(m->path), nl = (uint32_t)tail + pl;
        if (nl + 1 > TU_FULL) { LOG("【红】[touchui] type %#x: the new path is too long: skipped", m->id); continue; }
        memcpy(m->full, p, (size_t)tail); memcpy(m->full + tail, m->path, pl + 1);
        m->cr = cr; m->saved = cr->path; m->tail = (uint32_t)tail; m->old_len = len; m->in_place = nl + 1 <= (cr->path.cap1 & ~1u);
        m->state = 4;   // prepared
    }
    // pass 2: the table of the UI mode now is written; the other modes' entries stay prepared (dm_dos2_touchui_set_mode writes them later)
    for (int i = 0; i < g_nmap; i++) {
        tu_map *m = &g_map[i]; if (m->state != 4) continue;
        if (!tu_on(m)) {
            parked++;
            LOG("[touchui] type %#x -> %s prepared for UI mode %s (was %s; not written: the %s table is on)", m->id, m->path,
                (m->modes & ui_mode_mask(UI_TOUCH)) ? "touch" : "hybrid", m->old, ui_mode_name(g_ui));
            continue;
        }
        tu_write(m); done++;
        LOG("[touchui] type %#x -> %s (was %s; creator %p function main+%#llx, %s)", m->id, m->full, m->old, (void *)m->cr,
            (unsigned long long)((uint8_t *)m->cr->fn - g_main->base), m->in_place ? "written in place" : "pointer and length swapped for a host buffer");
    }
    int nre = 0;
    for (int i = 0; i < g_nmap; i++) {   // TU-05: `_c` id -> PC twin, only when both share the creator function (= one class) and the twin plays the `_t` movie
        tu_map *m = &g_map[i]; if (m->state != 2) continue;
        tu_map *tw = NULL; for (int k = 0; k < g_nmap; k++) if (g_map[k].id == m->pc && (g_map[k].modes & m->modes)) tw = &g_map[k];
        tu_node *a = tu_find(mgr, m->id), *b = tu_find(mgr, m->pc);
        if (!g_pcwin) { m->state = -1; LOG("[touchui] type %#x stays %s: its `_t` movie has the PC contract and DM_DOS2_TOUCHUI_PCWIN=0", m->id, m->old); continue; }
        const int tw_ok = tw && (tw->state == 1 || tw->state == 4);
        if (!tw_ok || !a || !b || !a->cr || !b->cr || a->cr->fn != b->cr->fn) {
            m->state = -1;
            LOG("【红】[touchui] type %#x: PC twin %#x %s: stays %s", m->id, m->pc, !tw_ok ? "not substituted" : "has another creator function (not the same class)", m->old);
            continue; }
        m->pc_ok = 1; nre++;
        if (!tu_on(m)) m->state = 5;   // prepared, parked with its twin
        LOG("[touchui] type %#x (%s) will be created as %#x -> %s (PC branch of the same class, creator function main+%#llx)%s", m->id, m->old, m->pc, tw->path,
            (unsigned long long)((uint8_t *)a->cr->fn - g_main->base), m->state == 5 ? " - in its UI mode only" : "");
    }
    LOG("[touchui] %d of %d type(s) substituted, %d `_c` type(s) created as their PC twin (DM_DOS2_TOUCHUI_PCWIN=%d) (UI manager %p, %u types registered)", done, g_nmap, nre,
        g_pcwin, (void *)mgr, *(volatile uint32_t *)(mgr + TU_HASH_CNT));
    if (parked) LOG("[touchui] UI mode %s: %d prepared substitution(s) of the other modes' tables left unwritten (written on a mode change)", ui_mode_name(g_ui), parked);
}
// hle_dos2_uimode.c (game thread, the pad poll): the path table of `mode` is written, every other one given back. Windows made from now on
// play the new movies; objects alive keep the movie they were made with (the HUD sets change at the next load or the live rebuild).
// Returns the entries of that table now in the creators, -1 = no touch UI in this run (no package / DM_DOS2_TOUCHUI=0).
int dm_dos2_touchui_set_mode(int mode);
int dm_dos2_touchui_set_mode(int mode) {
    if (!g_on || !g_main || !ui_mode_valid(mode)) return -1;
    pthread_mutex_lock(&g_lock);
    const int was = g_ui; g_ui = mode;
    int on = 0, off = 0;
    if (g_applied) {   // before the first CreateUIObject tu_apply reads g_ui itself
        for (int i = 0; i < g_nmap; i++) {   // first give back what leaves (a type in two tables: the old entry first)
            tu_map *m = &g_map[i];
            if (m->state == 1 && !tu_on(m)) { tu_unwrite(m); off++; }
            else if (m->state == 2 && !tu_on(m)) { m->state = 5; off++; }
        }
        for (int i = 0; i < g_nmap; i++) {
            tu_map *m = &g_map[i];
            if (m->state == 4 && tu_on(m)) { tu_write(m); on++; }
            else if (m->state == 5 && tu_on(m)) { m->state = 2; on++; }
        }
    } else for (int i = 0; i < g_nmap; i++) on += tu_on(&g_map[i]);
    pthread_mutex_unlock(&g_lock);
    LOG("[touchui] path table %s -> %s: %d substitution(s) written, %d given back%s", ui_mode_name(was), ui_mode_name(mode), on, off,
        g_applied ? "" : " (before the first CreateUIObject: written there)");
    return on;
}
// hle_dos2_uimode.c, DM_DOS2_UI_LIVE=1: may the player UI sets be rebuilt now? 1 = no (*type = the window: a visible modal one, a dialog, the
// loading screen, character creation; TU_MGR_MODAL = the modal bits [uimgr+0x220]), 0 = nothing of that, -1 = cannot tell (no window watch)
int dm_dos2_touchui_ui_busy(uint32_t *type);
int dm_dos2_touchui_ui_busy(uint32_t *type) {
    static const uint32_t k_busy_types[] = {0xe, 0x42, 0x65, 0x17, 3, 4};   // dialog, dialog_c, loading screen, character creation (as hle_dos2_touchui_hud.c)
    if (type) *type = 0;
    if (!g_watch_ok || !g_main) return -1;
    uint8_t *mgr = *(uint8_t *volatile *)(g_main->base + TU_UIMGR);
    if (!mgr) return -1;
    if (g_modal_ok && (*(volatile uint32_t *)(mgr + TU_MGR_MODAL) & TU_F_MODAL)) { if (type) *type = TU_MGR_MODAL; return 1; }
    uint8_t *const *objs = *(uint8_t *const *volatile *)(mgr + TU_MGR_OBJS);
    const uint32_t nobj = *(volatile uint32_t *)(mgr + TU_MGR_NOBJ);
    if (!objs || nobj >= 0x100000) return -1;
    for (uint32_t i = 0; i < nobj; i++) {
        const uint8_t *o = objs[i]; if (!o) continue;
        const uint32_t fl = *(const volatile uint32_t *)(o + TU_OBJ_FLAGS), ty = *(const volatile uint32_t *)(o + TU_OBJ_TYPE);
        if (!(fl & TU_F_VISIBLE)) continue;
        int hit = (fl & TU_F_MODAL) != 0;
        for (unsigned k = 0; k < sizeof k_busy_types / sizeof *k_busy_types && !hit; k++) hit = ty == k_busy_types[k];
        if (hit) { if (type) *type = ty; return 1; }
    }
    return 0;
}
// ---- TU-04: which substituted windows are on screen (polled by hle_dos2_touch.c on the game thread, every pad poll) ----
static void tu_live_add(uint64_t h, uint32_t type, uint32_t asked, int touch) {   // under g_lock
    for (int i = 0; i < g_nlive; i++) if (g_live[i].h == h) return;
    if (g_nlive == TU_LIVE) { int k = 0; for (int i = 1; i < g_nlive; i++) if (!g_live[i].vis) { k = i; break; } g_live[k] = g_live[--g_nlive]; }   // full: drop a hidden one
    g_live[g_nlive++] = (tu_live){h, type, asked, -1, 0, touch};
}
static const char *tu_name(uint32_t type) {
    for (int i = 0; i < g_nmap; i++) if (g_map[i].id == type && (g_map[i].state == 1 || g_map[i].state == 2)) return g_map[i].path;
    return "console menu, no touch";
}
static int tu_is_block(uint32_t type) { for (unsigned i = 0; i < sizeof k_block / sizeof *k_block; i++) if (k_block[i] == type) return 1; return 0; }
int dm_dos2_touchui_is_block(uint32_t type);   // hle_dos2_touchui_route.c
int dm_dos2_touchui_is_block(uint32_t type) { return tu_is_block(type); }
static int g_vis_touch, g_vis_touch_modal, g_vis_block, g_vis_block_modal;   // what the last poll saw (game thread)
// the column movie (tu_col_*, below the create hook)
enum { TU_COL_TYPE = 0x8d, TU_BB_TYPE = 0x3b, TU_SETFLAGS_FN = 0xae4110, TU_CLRFLAGS_FN = 0xae3f10 };
static uint64_t g_col_h;
static void tu_col_mirror(uint8_t *mgr, int bb_vis);
int dm_dos2_touchui_poll(int *pc_shown);   // hle_dos2_touch.c: visible substituted windows (modal ones unless DM_DOS2_TOUCHUI_FILTER=1); *pc_shown = retyped ones
int dm_dos2_touchui_poll(int *pc_shown) {
    if (pc_shown) *pc_shown = 0;
    if (!g_on || !g_watch_ok || !g_main) return 0;
    uint8_t *mgr = *(uint8_t *volatile *)(g_main->base + TU_UIMGR);
    if (!mgr) return 0;
    if (pthread_mutex_trylock(&g_lock)) return -1;   // creation in flight: the caller keeps its last state
    uint8_t *const *objs = *(uint8_t *const *volatile *)(mgr + TU_MGR_OBJS);
    const uint32_t nobj = *(volatile uint32_t *)(mgr + TU_MGR_NOBJ);
    int up = 0, pcn = 0, vt = 0, vtm = 0, vb = 0, vbm = 0;
    for (int i = 0; i < g_nlive; i++) {
        tu_live *l = &g_live[i];
        const uint32_t idx = (uint32_t)l->h;
        const uint8_t *o = objs && idx < nobj && nobj < 0x100000 ? objs[idx] : NULL;
        const int alive = o && *(const volatile uint32_t *)(o + TU_OBJ_TYPE) == l->type;
        const uint32_t fl = alive ? *(const volatile uint32_t *)(o + TU_OBJ_FLAGS) : 0;
        const int vis = alive && (fl & TU_F_VISIBLE);
        if (!alive || vis != l->vis || (vis && (fl & TU_F_MODAL) != (l->flags & TU_F_MODAL))) {
            const unsigned n = atomic_fetch_add(&g_nev, 1);
            if (n < 600) LOG("[touchui] window type %#x%s%s (%s, handle %#llx): %s (flags %#x%s)", l->type, l->asked != l->type ? " made for " : "",
                             l->asked != l->type ? tu_name(l->asked) + 0 : "", tu_name(l->type), (unsigned long long)l->h,
                             !alive ? "destroyed" : vis ? "SHOWN" : "hidden", fl, vis && (fl & TU_F_MODAL) ? ", modal" : "");
            if (n == 599) LOG("[touchui] (window events: no more lines)");
        }
        if (!alive) { g_live[i--] = g_live[--g_nlive]; continue; }
        l->vis = vis; l->flags = fl;
        if (vis && !l->touch) { vb++; vbm += (fl & TU_F_MODAL) != 0; continue; }
        if (vis) { vt++; vtm += (fl & TU_F_MODAL) != 0; }
        if (vis && (g_filter_any || (fl & TU_F_MODAL))) up++;
        if (vis && l->asked != l->type) pcn++;
    }
    g_vis_touch = vt; g_vis_touch_modal = vtm; g_vis_block = vb; g_vis_block_modal = vbm;
    int bb_vis = 0; for (int i = 0; i < g_nlive; i++) if (g_live[i].type == TU_BB_TYPE && g_live[i].vis > 0) bb_vis = 1;
    pthread_mutex_unlock(&g_lock);
    if (g_col_h) tu_col_mirror(mgr, bb_vis);
    if (pc_shown) *pc_shown = pcn;
    // DM_DOS2_TOUCHUI_PCWIN=2: the mode byte 0 while a retyped window is on screen, back to 1 when none (only the host's own flips)
    static int g_flipped;
    if (g_pcwin == 2 && g_mode_ok) {
        volatile uint8_t *mode = (volatile uint8_t *)(g_main->base + TU_MODE);
        if (pcn && !g_flipped && *mode == 1) { *mode = 0; g_flipped = 1; LOG("[touchui] DM_DOS2_TOUCHUI_PCWIN=2: mode byte main+%#x 1 -> 0 (%d PC-branch window(s) on screen)", TU_MODE, pcn); }
        else if (!pcn && g_flipped) { if (*mode == 0) *mode = 1; g_flipped = 0; LOG("[touchui] DM_DOS2_TOUCHUI_PCWIN=2: mode byte main+%#x -> 1 (no PC-branch window on screen)", TU_MODE); }
    }
    return up;
}
// hle_dos2_touch.c KBM=2, every pad poll: may mouse records reach the movies now (1 / 0; -1 = busy, keep the last state). The rule: the header.
int dm_dos2_touchui_gate(int in_game, int *why);
int dm_dos2_touchui_gate(int in_game, int *why) {
    if (!g_on || !g_watch_ok) { if (why) *why = 0; return !in_game; }   // no watch: the old rule (menus only)
    if (dm_dos2_touchui_poll(NULL) < 0) return -1;
    const int open = g_vis_block ? 0 : in_game ? (g_filter_any ? g_vis_touch : g_vis_touch_modal) > 0 : 1;
    if (why) *why = g_vis_block ? 3 : open ? (g_vis_touch ? 1 : 2) : 4;   // 1 touch window, 2 no console menu (splash / loading / press A), 3 console menu without touch, 4 in game
    return open;
}
// ExternalInterface "dmPadBtn" (hle_dos2_touch.c): may this UI object press pad buttons now? Not when it is not modal and a modal console menu is up.
int dm_dos2_touchui_may_press(const void *ui);
int dm_dos2_touchui_may_press(const void *ui) {
    if (!ui || !g_watch_ok) return 1;
    const uint32_t fl = *(const volatile uint32_t *)((const uint8_t *)ui + TU_OBJ_FLAGS);
    if (!(fl & TU_F_VISIBLE)) return 0;
    return (fl & TU_F_MODAL) || !(g_vis_block_modal + g_vis_touch_modal);
}
static void tu_restore(tu_map *m) {   // back to the movie the type had
    if (m->state != 1 || !m->cr) return;
    if (m->in_place) { memcpy(m->cr->path.ptr + m->tail, m->old, strlen(m->old) + 1); m->cr->path.len = m->old_len; }
    else m->cr->path = m->saved;
    m->state = -1;
}
// ---- the column movie (type 0x8d -> dmColumn_d.swf): made by the host right after bottomBar_c (same manager, layer, flags, player); the game never
// shows or hides it (it has no record slot), so the poll mirrors bottomBar's visibility onto it with the engine's own SetFlags / ClearFlags (bit 3) ----
static const uint32_t k_setflags_fn[3] = {0xa9be4ff4 /* stp x20,x19,[sp,#-0x20]! */, 0xa9017bfd, 0x910043fd};
static const uint32_t k_clrflags_fn[3] = {0xf81d0ff5 /* str x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd};
static int g_col_ok = -1;
static int tu_col_type_mapped(void) { for (int i = 0; i < g_nmap; i++) if (g_map[i].id == TU_COL_TYPE && g_map[i].state == 1) return 1; return 0; }
static uint8_t *tu_obj(uint8_t *mgr, uint64_t h, uint32_t type) {
    uint8_t *const *objs = *(uint8_t *const *volatile *)(mgr + TU_MGR_OBJS); const uint32_t nobj = *(volatile uint32_t *)(mgr + TU_MGR_NOBJ), idx = (uint32_t)h;
    uint8_t *o = h && objs && idx < nobj && nobj < 0x100000 ? objs[idx] : NULL;
    return o && *(const volatile uint32_t *)(o + TU_OBJ_TYPE) == type ? o : NULL;
}
static void tu_col_make(uint64_t mgr, uint64_t layer, uint64_t flags, uint64_t a4, uint64_t player) {   // after bottomBar_c was created (game thread)
    if (g_col_ok < 0) {
        g_col_ok = !memcmp(g_main->base + TU_SETFLAGS_FN, k_setflags_fn, sizeof k_setflags_fn) && !memcmp(g_main->base + TU_CLRFLAGS_FN, k_clrflags_fn, sizeof k_clrflags_fn);
        if (!g_col_ok) LOG("【红】[touchui] column movie: SetFlags / ClearFlags main+%#x / %#x differ: no column movie", TU_SETFLAGS_FN, TU_CLRFLAGS_FN);
    }
    if (!g_col_ok || !tu_col_type_mapped()) return;
    if (tu_obj((uint8_t *)(uintptr_t)mgr, g_col_h, TU_COL_TYPE)) return;   // still there (the HUD set was rebuilt): reuse it
    const uint64_t h = g_create_orig(mgr, layer, TU_COL_TYPE, flags, a4, player);
    const int ok = (h & 0x3fffff00000000ull) != 0;
    LOG("%s[touchui] column movie: CreateUIObject type %#x = GUI/dmColumn_d.swf (layer %#x flags %#x player %u, next to bottomBar_c) -> handle %#llx: %s",
        ok ? "" : "【红】", TU_COL_TYPE, (unsigned)layer, (unsigned)flags, (unsigned)player & 0xffff, (unsigned long long)h, ok ? "created" : "FAILED");
    if (!ok) return;
    g_col_h = h;
    pthread_mutex_lock(&g_lock); tu_live_add(h, TU_COL_TYPE, TU_COL_TYPE, 1); pthread_mutex_unlock(&g_lock);
}
static void tu_col_mirror(uint8_t *mgr, int bb_vis) {   // game thread, g_lock NOT held: the column shows / hides with bottomBar
    uint8_t *o = g_col_ok > 0 ? tu_obj(mgr, g_col_h, TU_COL_TYPE) : NULL;
    if (!o) return;
    const int vis = (*(const volatile uint32_t *)(o + TU_OBJ_FLAGS) & TU_F_VISIBLE) != 0;
    if (vis == bb_vis) return;
    typedef void (*flag_fn)(void *, uint32_t);
    ((flag_fn)(void *)(g_main->base + (bb_vis ? TU_SETFLAGS_FN : TU_CLRFLAGS_FN)))(o, TU_F_VISIBLE);
    static unsigned n; if (n++ < 40) {
        const uint8_t *fp = *(uint8_t *const *)(o + 0x18); const int32_t *f = fp ? (const int32_t *)(fp + 0x10) : NULL;
        LOG("[touchui] column movie %s with bottomBar (flags now %#x)%s%d,%d %dx%d", bb_vis ? "SHOWN" : "hidden", *(const volatile uint32_t *)(o + TU_OBJ_FLAGS),
            f ? "; its FlashPlayer rect " : "", f ? f[0] : 0, f ? f[1] : 0, f ? f[2] : 0, f ? f[3] : 0); }
}
static uint64_t tu_create_hook(uint64_t mgr, uint64_t layer, uint64_t type, uint64_t flags, uint64_t a4, uint64_t player) {
    const uint32_t asked = (uint32_t)type;
    uint32_t id = asked;
    tu_map *hit = NULL, *re = NULL;
    if ((g_on || g_log) && mgr) {
        pthread_mutex_lock(&g_lock);
        if (!g_applied) { g_applied = 1; tu_apply((uint8_t *)(uintptr_t)mgr); }
        for (int i = 0; i < g_nmap; i++) if (g_map[i].id == id && g_map[i].state == 2 && g_map[i].pc_ok) { re = &g_map[i]; id = re->pc; }   // TU-05: the PC twin
        for (int i = 0; i < g_nmap; i++) if (g_map[i].id == id && g_map[i].state == 1) hit = &g_map[i];
        if (re && !hit) { re = NULL; id = asked; }
        pthread_mutex_unlock(&g_lock);
    }
    uint64_t h = g_create_orig(mgr, layer, id, flags, a4, player);
    int ok = (h & 0x3fffff00000000ull) != 0;
    const unsigned n = atomic_fetch_add(&g_ncalls, 1);
    if (hit) {
        char tw[48] = "";
        if (re) snprintf(tw, sizeof tw, " as its PC twin %#x", id);
        if ((g_dbg || g_log || !ok) && n < 4000)
            LOG("%s[touchui] CreateUIObject type %#x%s = %s (layer %#x flags %#x player %u) -> handle %#llx: %s", ok ? "" : "【红】", asked, tw, hit->path, (unsigned)layer,
                (unsigned)flags, (unsigned)player & 0xffff, (unsigned long long)h, ok ? "created" : "FAILED");
        if (!ok && re) {   // the PC twin could not be made: this `_c` type is created as itself from now on (its own `_c` movie)
            pthread_mutex_lock(&g_lock); re->pc_ok = 0; re->state = -1; pthread_mutex_unlock(&g_lock);
            id = asked; h = g_create_orig(mgr, layer, id, flags, a4, player); ok = (h & 0x3fffff00000000ull) != 0;
            LOG("【红】[touchui] type %#x is created as itself again (%s) -> handle %#llx (%s)", asked, re->old, (unsigned long long)h, ok ? "created" : "failed too");
            hit = NULL;
        } else if (!ok) {   // the touch movie could not be made: this type goes back to its own movie, for good
            pthread_mutex_lock(&g_lock); tu_restore(hit); pthread_mutex_unlock(&g_lock);
            h = g_create_orig(mgr, layer, type, flags, a4, player); ok = (h & 0x3fffff00000000ull) != 0;
            LOG("【红】[touchui] type %#x is back to %s -> handle %#llx (%s)", id, hit->old, (unsigned long long)h, ok ? "created" : "failed too");
            hit = NULL;
        }
        if (hit && ok && g_watch_ok) { pthread_mutex_lock(&g_lock); tu_live_add(h, id, asked, 1); pthread_mutex_unlock(&g_lock); }
        if (hit && ok && g_watch_ok && asked == TU_BB_TYPE) tu_col_make(mgr, layer, flags, a4, player);
    } else {
      if (g_on && ok && g_watch_ok && tu_is_block(id)) { pthread_mutex_lock(&g_lock); tu_live_add(h, id, asked, 0); pthread_mutex_unlock(&g_lock); }   // a console menu without touch: watched
      if (g_log && n < 400)
        LOG("[touchui] CreateUIObject type %#x (layer %#x flags %#x player %u) -> handle %#llx%s", id, (unsigned)layer, (unsigned)flags, (unsigned)player & 0xffff,
            (unsigned long long)h, ok ? "" : " (failed)");
    }
    if (ok && g_on) { void dm_dos2_touchui_hud_created(void *mgr, uint64_t h, uint32_t type); dm_dos2_touchui_hud_created((void *)(uintptr_t)mgr, h, id); }   // UI mode TOUCH: the physical scale of the game's own windows
    return h;
}

static int tu_add(uint32_t id, const char *movie, const char *expect, uint32_t pc, unsigned modes) {   // modes: the UI modes' tables it belongs to
    if (g_nmap == TU_MAX || id > 0xffff) return 0;
    for (int i = 0; i < g_ndeny; i++) if (g_deny[i] == id || (pc && g_deny[i] == pc)) { LOG("[touchui] type %#x: DM_DOS2_TOUCHUI_DENY, left alone", id); return 1; }
    for (const char *c = movie; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '/' || *c == '.')) return 0;
    if (!*movie || strstr(movie, "..")) return 0;
    tu_map *m = &g_map[g_nmap];
    const size_t l = strlen(movie); const int has_gui = !strncmp(movie, "GUI/", 4), has_ext = l > 4 && !strcmp(movie + l - 4, ".swf");
    if (snprintf(m->path, sizeof m->path, "%s%s%s", has_gui ? "" : "GUI/", movie, has_ext ? "" : ".swf") >= (int)sizeof m->path) return 0;
    for (int i = 0; i < g_nmap; i++) if (g_map[i].id == id && (g_map[i].modes & modes)) return 0;   // one entry per type in a mode's table
    m->id = id; m->expect = expect; m->state = 0; m->pc = pc; m->pc_ok = 0; m->modes = modes; g_nmap++;
    return 1;
}
static void tu_add_map(const char *v, unsigned modes, const char *what) {   // "<hex id>:<movie>,..." ("GUI/" and ".swf" are added) into the tables of `modes`
    char *dup = strdup(v), *save = NULL; int bad = 0; const int n0 = g_nmap;
    for (char *t = dup ? strtok_r(dup, ",; ", &save) : NULL; t; t = strtok_r(NULL, ",; ", &save)) {
        char *c = strchr(t, ':'), *end = NULL;
        if (!c) { bad++; continue; }
        *c = 0; const unsigned long id = strtoul(t, &end, 16);
        if (end == t || *end || !tu_add((uint32_t)id, c + 1, NULL, 0, modes)) bad++;
    }
    free(dup);
    LOG("%s[touchui] %s: %d type(s)%s", bad ? "【红】" : "", what, g_nmap - n0, bad ? ", some entries were not understood (want <hex id>:<movie>,...)" : "");
}

// ---- tooltip guard (the header's correction 2): `if (!tooltip_ui) return;` at the entry of main+0x1120b50 and main+0x1121150 ----
enum { TG_C = 0x111fec0, TG_TAIL_C = 0x10a7404, TG_A = 0x1120b50, TG_B = 0x1121150, TG_TAIL_A = 0x10a697c, TG_TAIL_B = 0x10a6a90, TG_GET = 0x10a6fd8, TG_USE = 0x1121220, TG_CHECKED = 0x10a7090 };
static const uint32_t k_tg_a[4] = {0xd10503ff /* sub sp,sp,#0x140 */, 0xa90e6ffc, 0xa90f67fa, 0xa9105ff8};
static const uint32_t k_tg_b[4] = {0xd10703ff /* sub sp,sp,#0x1c0 */, 0xa9166ffc, 0xa91767fa, 0xa9185ff8};
// third tail (phone 20.11, UI mode TOUCH: a tap on a hotBar_p slot, CRASH sig=11 addr=0x1a0 pc=main+0x111fee4 lr=main+0x10a6238): 0x10a7390 =>
// `bl 0x10a6fc0` (the tooltip UI or NULL) ... `b 0x111fec0`, whose 1st store is `str xzr,[x0,#0x1a0]`; 0x111fec0 has that one caller
static const uint32_t k_tg_c[4] = {0xd10543ff /* sub sp,sp,#0x150 */, 0x6d0e23e9, 0xa90f6ffc, 0xa91067fa};
static const uint32_t k_tg_tail_c[11] = {0x97fffeef /* bl 0x10a6fc0 */, 0x6d4123e9, 0x12000283, 0xaa1603e1, 0xaa1503e2, 0xf94013f7, 0xa9457bfd, 0xa9444ff4, 0xa94357f6, 0x6cc62beb, 0x1401e2a5 /* b 0x111fec0 */};
static const uint32_t k_tg_tail_a[6] = {0x94000191 /* bl 0x10a6fc0 */, 0xf9408261 /* ldr x1,[x19,#0x100] */, 0xa9427bfd, 0xa9414ff4, 0x9100c3ff, 0x1401e870 /* b 0x1120b50 */};
static const uint32_t k_tg_tail_b[6] = {0x9400014c /* bl 0x10a6fc0 */, 0xf9408261, 0xa9427bfd, 0xa9414ff4, 0x9100c3ff, 0x1401e9ab /* b 0x1121150 */};
static const uint32_t k_tg_get[3] = {0x94019c86 /* bl 0x110e1f0: the tooltip UI or NULL */, 0xaa0003f4, 0xb4000140 /* cbz x0, return it */};
static const uint32_t k_tg_use[2] = {0x3907927f /* strb wzr,[x19,#0x1e4]: x19 = that object, unchecked */, 0xf9400288};
static const uint32_t k_tg_checked[2] = {0x97ffffcc /* bl 0x10a6fc0 */, 0xb4000160 /* cbz x0: the caller that does check */};
// Run 20.11 review (docs/dos2/TOUCH_MODES_PLAN.md "Прогон 20.11"): 0x10a6fc0 has nine callers; besides the three tails above and the two that
// check (0x10a7090, 0x10a77b0) three more hand its NULL straight to a function of the tooltip class that uses it first:
//   0x10a726c `bl 0x10a6fc0; mov x1,sp; bl 0x111f1e0`      0x111f1e0 starts with `ldr w8,[x0,#0x198]` (callers 0x109a3ec checked, 0x10a7274, 0x1120894)
//   0x1323434 `bl 0x10a6fc0; ldr w2,..; mov x1,x21; bl 0x111fb40`   0x111fb40: `bl 0xaf0b20` then `ldr x8,[x19,#0x140]` (one caller)
//   0x10a6a40 `bl 0x10a6fc0; ... bl 0x1121f20`             0x1121f20: returns at once for an empty handle (x1), else stores into x0 (one caller)
// Every caller of these three ignores x0 afterwards (0x10a7278 epilogue / 0x10a6360 reads its own stack, 0x1323444 reads its stack, 0x10a6a64
// `b 0x10a6940` epilogue), so the same entry stub ("x0 == 0: return to the caller") is safe there. They get it as a second group: a site of
// this group that differs only leaves this group out (the first three stay guarded). The real cure is the tooltip object itself: in UI mode
// TOUCH hle_dos2_touchui_hud.c makes it (type 0x2c, as the iPad's PC branch does); this guard is for the time before / without it.
enum { TG_D = 0x111f1e0, TG_E = 0x111fb40, TG_F = 0x1121f20, TG_CALL_D = 0x10a726c, TG_CALL_E = 0x1323434, TG_CALL_F = 0x10a6a40, TG_F_CHK = 0x1121f48 };
static const uint32_t k_tg_d[4] = {0xd10503ff /* sub sp,sp,#0x140 */, 0x6d0d23e9, 0xa90e6ffc, 0xa90f67fa};
static const uint32_t k_tg_e[4] = {0xd10643ff /* sub sp,sp,#0x190 */, 0x6d1223e9, 0xa9136ffc, 0xa91467fa};
static const uint32_t k_tg_f[4] = {0xd10583ff /* sub sp,sp,#0x160 */, 0xfd0073ea /* str d10,[sp,#0xe0] */, 0x6d0f23e9, 0xa9106ffc};
static const uint32_t k_tg_call_d[3] = {0x97ffff55 /* bl 0x10a6fc0 */, 0x910003e1 /* mov x1,sp */, 0x9401dfdb /* bl 0x111f1e0 */};
static const uint32_t k_tg_call_e[4] = {0x97f60ee3 /* bl 0x10a6fc0 */, 0xb940ba62, 0xaa1503e1, 0x97f7f1c0 /* bl 0x111fb40 */};
static const uint32_t k_tg_call_f[9] = {0x94000160 /* bl 0x10a6fc0 */, 0xf85f8e68, 0xf9401508, 0xaa0003f4 /* x20 = it */, 0xaa1303e0, 0xd63f0100, 0xaa0003e1, 0xaa1403e0, 0x9401ed30 /* bl 0x1121f20 */};
static const uint32_t k_tg_f_chk[2] = {0xf260543f /* tst x1,#0x3fffff00000000 */, 0x54000161};
static _Atomic unsigned g_tg_n;
static module_t *g_tg_main;
static uint64_t tu_tip_null(uint64_t a0, uint64_t a1) {   // reached with x0 == 0 only; returns to the game's caller (the stub jumped with br: lr is the game's)
    (void)a0;
    const unsigned n = atomic_fetch_add(&g_tg_n, 1);
    const uintptr_t lr = (uintptr_t)__builtin_return_address(0), mb = g_tg_main ? (uintptr_t)g_tg_main->base : 0;
    if (n < 8 || (n & (n - 1)) == 0) LOG("[touchui] tooltip guard: no tooltip UI object (x1 %#llx), call skipped (#%u; returns to main+%#llx; the game would have used a NULL object, crash of run tu03 at main+%#x)",
                                         (unsigned long long)a1, n + 1, (unsigned long long)(lr >= mb ? lr - mb : 0), TG_USE);
    return 0;
}
void dm_dos2_touch_guard_install(void);   // main.c host_preload, after dm_dos2_touchui_install (which may turn DM_DOS2_TOUCH on)
void dm_dos2_touch_guard_install(void) {
    const char *e = getenv("DM_DOS2_TOUCH"), *g = getenv("DM_DOS2_TOUCH_TIPGUARD");
    if (!e || atoi(e) <= 0) return;
    if (g && *g == '0') { LOG("[touchui] DM_DOS2_TOUCH_TIPGUARD=0: no tooltip guard"); return; }
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) return;
    g_tg_main = m;
    static const tu_site ck[] = {{TG_A, 4, k_tg_a}, {TG_B, 4, k_tg_b}, {TG_TAIL_A, 6, k_tg_tail_a}, {TG_TAIL_B, 6, k_tg_tail_b}, {TG_GET, 3, k_tg_get}, {TG_USE, 2, k_tg_use}, {TG_CHECKED, 2, k_tg_checked}, {TG_C, 4, k_tg_c}, {TG_TAIL_C, 11, k_tg_tail_c}};
    for (unsigned i = 0; i < sizeof ck / sizeof *ck; i++)
        if ((uint64_t)ck[i].off + 4ull * ck[i].n > m->text_end || memcmp(m->base + ck[i].off, ck[i].want, 4ull * ck[i].n)) {
            LOG("【红】[touchui] main+%#x differs: no tooltip guard (a mouse hover over the controller HUD may crash)", ck[i].off); return; }
    // the second group (run 20.11 review, above): all its sites or none of it
    static const tu_site ck2[] = {{TG_D, 4, k_tg_d}, {TG_E, 4, k_tg_e}, {TG_F, 4, k_tg_f}, {TG_CALL_D, 3, k_tg_call_d}, {TG_CALL_E, 4, k_tg_call_e}, {TG_CALL_F, 9, k_tg_call_f}, {TG_F_CHK, 2, k_tg_f_chk}};
    int nf = 6;
    for (unsigned i = 0; i < sizeof ck2 / sizeof *ck2; i++)
        if ((uint64_t)ck2[i].off + 4ull * ck2[i].n > m->text_end || memcmp(m->base + ck2[i].off, ck2[i].want, 4ull * ck2[i].n)) {
            LOG("【红】[touchui] main+%#x differs: the second tooltip guard group (main+%#x / %#x / %#x) is left out", ck2[i].off, TG_D, TG_E, TG_F); nf = 3; break; }
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[touchui] mmap failed: no tooltip guard"); return; }
    const struct { uint32_t off; const uint32_t *pro; } f[6] = {{TG_A, k_tg_a}, {TG_B, k_tg_b}, {TG_C, k_tg_c}, {TG_D, k_tg_d}, {TG_E, k_tg_e}, {TG_F, k_tg_f}};
    const uint64_t host = (uint64_t)(uintptr_t)(void *)tu_tip_null;
    for (int i = 0; i < nf; i++) {   // stub: cbz x0, null; <4 prologue instructions>; jump to function + 16;  null: jump to the host (it returns to the caller)
        uint32_t *s = t + 16 * i; const uint64_t rest = (uint64_t)(uintptr_t)(m->base + f[i].off + 16);
        s[0] = 0xb4000120 /* cbz x0, +9 words */; memcpy(s + 1, f[i].pro, 16);
        s[5] = 0x58000050 /* ldr x16,#8 */; s[6] = 0xd61f0200 /* br x16 */; s[7] = (uint32_t)rest; s[8] = (uint32_t)(rest >> 32);
        s[9] = 0x58000050; s[10] = 0xd61f0200; s[11] = (uint32_t)host; s[12] = (uint32_t)(host >> 32);
    }
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[touchui] mprotect RX failed: no tooltip guard"); munmap(t, 0x4000); return; }
    __builtin___clear_cache((char *)t, (char *)(t + 16 * 6));
    for (int i = 0; i < nf; i++) {
        const uint64_t to = (uint64_t)(uintptr_t)(t + 16 * i);
        const uint32_t jump[4] = {0x58000050, 0xd61f0200, (uint32_t)to, (uint32_t)(to >> 32)};
        if (tu_poke(m, f[i].off, jump, 4)) { LOG("【红】[touchui] mprotect RW failed: tooltip guard not complete (main+%#x)", f[i].off); return; }
    }
    LOG("[touchui] tooltip guard: main+%#x, main+%#x and main+%#x return when the tooltip UI object is NULL (pending tooltip main+0x10a6920 with no tooltip UI: crash of run tu03 at main+%#x; hot bar slot tap in TOUCH 20.11 at main+0x111fee4)%s",
        TG_A, TG_B, TG_C, TG_USE, nf == 6 ? "; also main+0x111f1e0 / 0x111fb40 / 0x1121f20 (the other unchecked users of main+0x10a6fc0, review of 20.11)" : "");
}

int dm_dos2_touchui_on(void);
int dm_dos2_touchui_on(void) { return g_on; }
int dm_dos2_touchui_pak_has(const char *gui_path);   // hle_dos2_touchui_hud.c: is "GUI/x.swf" in the package
int dm_dos2_touchui_pak_has(const char *gui_path) { return g_nocheck || (g_nfiles && tu_pak_has(gui_path)); }
void dm_dos2_touchui_install(void);   // main.c host_preload, BEFORE dm_dos2_touch_install (it turns DM_DOS2_TOUCH / _KBM on for it)
void dm_dos2_touchui_install(void) {
    const char *e = getenv("DM_DOS2_TOUCHUI"), *lg = getenv("DM_DOS2_TOUCHUI_LOG"), *v;
    const int want = e && *e ? atoi(e) > 0 : tu_pak_find() != 0;   // owner 15.11: on by default whenever <romfs>/Data/TouchUI.pak is there (DM_DOS2_TOUCHUI=0 off)
    g_log = lg && atoi(lg) > 0;
    if (!want && !g_log) return;
    g_dbg = (v = getenv("DM_DBG")) && atoi(v) > 0;
    { int dm_dos2_ui_mode(void); g_ui = dm_dos2_ui_mode(); }   // hle_dos2_uimode.c (installed first): the path table to write
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) { LOG("【红】[touchui] no main module: touch UI off"); return; }
    static const tu_site k_sites[] = {{TU_CREATE, 4, k_create}, {TU_LOOKUP, 6, k_lookup}, {TU_CALL, 7, k_call}, {TU_MGR_LD, 3, k_mgr_ld}, {TU_RESULT, 2, k_result},
        {TU_REG48, 8, k_reg48}, {TU_REG48_STR, 3, k_reg48_str}, {TU_REG48_INS, 7, k_reg48_ins}, {TU_REG48_NODE, 4, k_reg48_node}, {TU_REG57, 6, k_reg57},
        {TU_SITE48, 6, k_site48}, {TU_SITE57, 6, k_site57}, {TU_PAKS, 7, k_paks}};
    int bad_site = -1;
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites && bad_site < 0; i++)
        if ((uint64_t)k_sites[i].off + 4ull * k_sites[i].n > m->text_end || memcmp(m->base + k_sites[i].off, k_sites[i].want, 4ull * k_sites[i].n)) bad_site = (int)i;
    const int bad_data = m->size < TU_UIMGR + 8 || memcmp(m->base + TU_PATH_C, k_path_c, sizeof k_path_c) || memcmp(m->base + TU_PAK_GLOB, k_glob, sizeof k_glob);
    if (bad_site >= 0 || bad_data) {
        LOG("【红】[touchui] main+%#x differs: not DOS2 update v786432 main (build 4979b200) at the UI type table / CreateUIObject / package listing: touch UI off, nothing changed",
            bad_site >= 0 ? k_sites[bad_site].off : (uint32_t)TU_PATH_C);
        return; }
    const int iggy_ok = m->text_end >= TU_IGGY_NOMETHOD + sizeof k_iggy_nomethod && !memcmp(m->base + TU_IGGY_NOMETHOD, k_iggy_nomethod, sizeof k_iggy_nomethod);
    if (want) {
        if ((v = getenv("DM_DOS2_TOUCHUI_PAK")) && *v && strlen(v) < sizeof g_pak_name && !strchr(v, '/')) strcpy(g_pak_name, v);
        g_nocheck = (v = getenv("DM_DOS2_TOUCHUI_NOCHECK")) && atoi(v) > 0;
        if ((v = getenv("DM_DOS2_TOUCHUI_PCWIN")) && *v) g_pcwin = atoi(v) < 0 ? 0 : atoi(v) > 2 ? 2 : atoi(v);
        g_filter_any = (v = getenv("DM_DOS2_TOUCHUI_FILTER")) && atoi(v) > 0;
        if ((v = getenv("DM_DOS2_TOUCHUI_DENY")) && *v) {
            char *dup = strdup(v), *save = NULL;
            for (char *t = dup ? strtok_r(dup, ",; ", &save) : NULL; t && g_ndeny < 32; t = strtok_r(NULL, ",; ", &save)) { char *end; unsigned long id = strtoul(t, &end, 16); if (end != t && !*end) g_deny[g_ndeny++] = (uint32_t)id; }
            free(dup);
        }
        // TU-04 visibility watch (read only: the object slot, type and flags) and the mode byte of DM_DOS2_TOUCHUI_PCWIN=2
        static const tu_site k_watch[] = {{TU_OBJTYPE, 1, k_objtype}, {TU_OBJSLOT, 7, k_objslot}, {TU_SETFLAGS, 7, k_setflags}, {TU_CLRFLAGS, 5, k_clrflags}, {TU_FREESLOT, 7, k_freeslot}};
        g_watch_ok = 1;
        for (unsigned i = 0; i < sizeof k_watch / sizeof *k_watch; i++)
            if ((uint64_t)k_watch[i].off + 4ull * k_watch[i].n > m->text_end || memcmp(m->base + k_watch[i].off, k_watch[i].want, 4ull * k_watch[i].n)) {
                LOG("【红】[touchui] main+%#x differs: no window watch (KBM=2 keeps the mouse filter closed in game, DM_DOS2_TOUCHUI_PCWIN=2 off)", k_watch[i].off); g_watch_ok = 0; break; }
        static const tu_site k_ms = {TU_MODESITE, 6, k_modesite};
        g_mode_ok = (uint64_t)k_ms.off + 24 <= m->text_end && !memcmp(m->base + k_ms.off, k_ms.want, 24) && m->size > TU_MODE;
        if (g_pcwin == 2 && !g_mode_ok) { LOG("【红】[touchui] main+%#x differs: DM_DOS2_TOUCHUI_PCWIN=2 acts as 1 (no mode byte flips)", TU_MODESITE); g_pcwin = 1; }
        const int where = tu_pak_find();
        if (where && !tu_pak_read(g_pak_host)) LOG("【红】[touchui] %s is not a readable LSPK v13 package", g_pak_host);
        if (g_nfiles) {
            struct stat st; long long sz = !stat(g_pak_host, &st) ? (long long)st.st_size : -1;
            LOG("[touchui] package %s: %d file(s), %lld B%s", g_pak_host, g_nfiles, sz, where == 2 ? " (from DM_ROMFS_OVERLAY: the rom index is off with an overlay, better put it into <romfs>/Data)" : "");
            if (g_dbg || g_log) for (int i = 0; i < g_nfiles && i < 80; i++) LOG("[touchui]   %s (%u B)", g_files[i].name, g_files[i].size);
        }
        if (!g_nfiles && !g_nocheck) {
            LOG("【红】[touchui] DM_DOS2_TOUCHUI=1 but no %s in <romfs>/Data (%s): the menus stay the controller ones, nothing changed", g_pak_name,
                getenv("DM_ROMFS") ? getenv("DM_ROMFS") : "DM_ROMFS not set");
            if (!g_log) return;
        } else {
            if ((v = getenv("DM_DOS2_TOUCHUI_MAP")) && *v) tu_add_map(v, ui_mode_mask(UI_HYBRID), "DM_DOS2_TOUCHUI_MAP");   // "48:mainMenu_t,57:mainMenu_t"
            else {
                const char *set = getenv("DM_DOS2_TOUCHUI_SET");
                for (unsigned i = 0; i < sizeof k_def / sizeof *k_def; i++) tu_add(k_def[i].id, k_def[i].movie, k_def[i].expect, k_def[i].pc, ui_mode_mask(UI_HYBRID));
                // since 15.11 (branch dos2-tc2) the console menus with tappable hints are part of the default: the routing hook's modal guard keeps a click
                // away from a touch menu under a box (hle_dos2_touchui_route.c). DM_DOS2_TOUCHUI_SET=main = the main menu only (the default of pack 2v).
                const int hints = !set || !*set || !strcmp(set, "hints");
                if (hints) for (unsigned i = 0; i < sizeof k_hint / sizeof *k_hint; i++) tu_add(k_hint[i].id, k_hint[i].movie, k_hint[i].expect, k_hint[i].pc, ui_mode_mask(UI_HYBRID));
                if (set && !strcmp(set, "ipad")) for (unsigned i = 0; i < sizeof k_ipad / sizeof *k_ipad; i++) tu_add(k_ipad[i].id, k_ipad[i].movie, k_ipad[i].expect, k_ipad[i].pc, ui_mode_mask(UI_HYBRID));
                else if (set && *set && !hints && strcmp(set, "main")) LOG("【红】[touchui] DM_DOS2_TOUCHUI_SET=%s is not main / hints / ipad: the main menu only", set);
                LOG("[touchui] set: %s", set && !strcmp(set, "ipad") ? "main menu D + the iPad `_t` menus (DM_DOS2_TOUCHUI_SET=ipad)" : hints
                    ? "main menu D + console menus with tappable hints (the default; DM_DOS2_TOUCHUI_SET=main = main menu only)" : "main menu D only; every other window stays the console one (DM_DOS2_TOUCHUI_SET=main)");
            }
            // UI mode TOUCH's own table (hle_dos2_uimode.c): k_touch + DM_DOS2_UI_TOUCH_MAP; SWITCH has none
            { const int n0 = g_nmap;
              for (unsigned i = 0; k_touch[i].movie; i++) tu_add(k_touch[i].id, k_touch[i].movie, k_touch[i].expect, k_touch[i].pc, ui_mode_mask(UI_TOUCH));
              // the console menus the game makes whatever the mode byte is (the main menu 0x48 / 0x57 never reads it; msgBox / saveLoad / options /
              // character creation / pause menu / dialog _c, and the console HUD after a pad press) keep the hybrid's touch movies (mainMenu_d with the
              // splash tap, hint taps ...): without them the main menu was the bare mainMenu_c in TOUCH and a tap did nothing (phone 20.11). A type the
              // touch table has (0x26 portraits, 0x8d the right side) keeps the touch movie.
              for (unsigned i = 0; i < sizeof k_def / sizeof *k_def + sizeof k_hint / sizeof *k_hint; i++) {
                  const tu_def *d = i < sizeof k_def / sizeof *k_def ? &k_def[i] : &k_hint[i - sizeof k_def / sizeof *k_def];
                  int own = 0; for (unsigned j = 0; k_touch[j].movie; j++) if (k_touch[j].id == d->id) own = 1;
                  if (!own) tu_add(d->id, d->movie, d->expect, d->pc, ui_mode_mask(UI_TOUCH));
              }
              if ((v = getenv("DM_DOS2_UI_TOUCH_MAP")) && *v) tu_add_map(v, ui_mode_mask(UI_TOUCH), "DM_DOS2_UI_TOUCH_MAP");
              if (g_nmap > n0 || g_ui != UI_HYBRID) LOG("[touchui] UI mode %s at start: the %s path table is written (hybrid %d entr%s, touch %d, switch none: stock movies)", ui_mode_name(g_ui),
                                                      ui_mode_name(g_ui), n0, n0 == 1 ? "y" : "ies", g_nmap - n0); }
            g_on = g_nmap > 0;
        }
        // the modal bits' sum [uimgr+0x220] for the UI mode switch's live rebuild (dm_dos2_touchui_ui_busy); without it the walk of the objects alone decides
        g_modal_ok = g_watch_ok && (uint64_t)TU_MODALSUM + sizeof k_modalsum <= m->text_end && !memcmp(m->base + TU_MODALSUM, k_modalsum, sizeof k_modalsum);
    }
    // the hook: CreateUIObject's first 4 instructions -> trampoline, a jump to the host in their place
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[touchui] mmap failed: touch UI off"); g_on = 0; return; }
    const uint64_t rest = (uint64_t)(uintptr_t)(m->base + TU_CREATE + 16);
    memcpy(t, k_create, 16); t[4] = 0x58000050 /* ldr x16,#8 */; t[5] = 0xd61f0200 /* br x16 */; t[6] = (uint32_t)rest; t[7] = (uint32_t)(rest >> 32);
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[touchui] mprotect RX failed: touch UI off"); munmap(t, 0x4000); g_on = 0; return; }
    __builtin___clear_cache((char *)t, (char *)(t + 8));
    g_create_orig = (tu_fn)(void *)t;
    const uint64_t to = (uint64_t)(uintptr_t)(void *)tu_create_hook;
    const uint32_t jump[4] = {0x58000050, 0xd61f0200, (uint32_t)to, (uint32_t)(to >> 32)};
    if (tu_poke(m, TU_CREATE, jump, 4)) { LOG("【红】[touchui] mprotect RW failed: CreateUIObject not hooked, touch UI off"); g_on = 0; g_create_orig = NULL; return; }
    if (g_on) {
        char list[2560]; size_t o = 0; list[0] = 0;
        for (int i = 0, j = 0; i < g_nmap && o + 64 < sizeof list; i++) {   // the start mode's table (hle_dos2_uimode.c); SWITCH: none
            if (!ui_mode_entry_on(g_map[i].modes, g_ui)) continue;
            o += (size_t)(g_map[i].pc ? snprintf(list + o, sizeof list - o, "%s%#x -> %#x", j ? ", " : "", g_map[i].id, g_map[i].pc)
                                      : snprintf(list + o, sizeof list - o, "%s%#x -> %s", j ? ", " : "", g_map[i].id, g_map[i].path));
            j++;
        }
        if (!o) snprintf(list, sizeof list, "none (UI mode %s: stock movies until a mode change)", ui_mode_name(g_ui));
        LOG("[touchui] DM_DOS2_TOUCHUI_PCWIN=%d (%s), window watch %s, mouse filter in game: %s", g_pcwin,
            g_pcwin == 0 ? "the `_c` ids of PC-contract menus keep their `_c` movie" : g_pcwin == 1 ? "`_c` id -> PC twin of the same class, mode byte untouched" : "PC twin + mode byte 0 while shown",
            g_watch_ok ? "on" : "OFF", g_filter_any ? "any visible touch window (DM_DOS2_TOUCHUI_FILTER=1)" : "while a modal touch window is visible; out of game: open unless a console menu without touch is up");
        LOG("[touchui] DM_DOS2_TOUCHUI=1: CreateUIObject main+%#x hooked; at its first call the creators' paths change: %s (movies from Data/%s%s)", TU_CREATE, list, g_pak_name,
            g_nocheck ? ", not checked: DM_DOS2_TOUCHUI_NOCHECK" : "");
        LOG("[touchui] methods the controller branch calls and the touch movie lacks (addBtnHint, setProfile, setPressToJoin ...): %s", iggy_ok
            ? "Iggy reports 'No method found' and returns an error (main+0x3d0370), no crash" : "main+0x3d0370 differs: Iggy's missing-method path is not confirmed in this build");
        // the controller UI stays (mode byte 1, the pad works); touches become mouse records that pass the listener filter
        const char *tc = getenv("DM_DOS2_TOUCH"), *kb = getenv("DM_DOS2_TOUCH_KBM");
        if (!tc || atoi(tc) <= 0) setenv("DM_DOS2_TOUCH", "1", 1);
        if (!kb || !*kb) setenv("DM_DOS2_TOUCH_KBM", "2", 1);   // menus only: the controller HUD gets no mouse events (run tu03)
        { const char *tw = getenv("DM_DOS2_TOUCH_WORLD");   // the world by touch: PC clicks at the finger (hle_dos2_touch_world.c; its sites differing = it runs as =1)
          if (!tw || !*tw) setenv("DM_DOS2_TOUCH_WORLD", "3", 1);
          LOG("[touchui] DM_DOS2_TOUCH_WORLD=%s%s: a tap on the world = the PC's click at the finger, hold / drag = left stick (0 = the world gets no touches)",
              getenv("DM_DOS2_TOUCH_WORLD"), tw && *tw ? " (from the environment)" : " (set here: the default with DM_DOS2_TOUCHUI)"); }
        LOG("[touchui] DM_DOS2_TOUCH=1%s, DM_DOS2_TOUCH_KBM=%s%s: touches off the pad's controls reach the movies as the mouse, the pad keeps working (mode byte stays 1)",
            tc && atoi(tc) > 0 ? "" : " (set here)", getenv("DM_DOS2_TOUCH_KBM"), kb && *kb ? " (from the environment)" : " (set here)");
        if (((v = getenv("DM_DOS2_TOUCH_PCUI")) && atoi(v) > 0) || ((v = getenv("DM_DOS2_TOUCH_LIVE")) && atoi(v) > 0))
            LOG("【红】[touchui] DM_DOS2_TOUCH_PCUI / DM_DOS2_TOUCH_LIVE are set too: the mode byte will not stay 1 (this slice was made for the controller mode)");
    } else LOG("[touchui] DM_DOS2_TOUCHUI_LOG=1: CreateUIObject main+%#x hooked for the log only (no substitution)", TU_CREATE);
}
#endif
