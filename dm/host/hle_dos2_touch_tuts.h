// [DOS2] hle_dos2_touch_tuts.c: pure rules (host-testable, dm/host/tests/dos2_touch_tuts_test.c). UI mode TOUCH: the tutorial windows
// (type 0x37, GUI/tutorialBox.swf) whose text only makes sense with a keyboard and a mouse ("Press [IE_QuickSave] ...", "Click on ...")
// are not queued at all while the HUD part of the mode is TOUCH - the iPad never shows these PC texts either (it has its own touch
// wording for them, which the release must not carry). Owner 23.11: "the PC tutorial windows about keys get in the way on touch".
// The facts below are checked against main.raw and the Switch data by tools/dos2_ptutorial.py check.
//
// main.raw 4979b200. Every tutorial goes one way (75 client call sites + the server's eocnet::ShowTutorialMessage):
//   main+0xdc0ec0  client ShowTutorial(key, ...): key -> main+0xdc15f0 (mode byte main+0x2bb8918 != 0: key + "_Controller" when that
//                  TranslatedStringKey exists; 0 = the PC UI the touch mode runs: the key as it came) -> main+0x10ced30 (the player's
//                  tutorialBox, main+0x10bfad0) -> AddTutorial main+0x126aed0
//   main+0x126aed0 AddTutorial(box, player, &key, &title, &category, modal type, ..., flags, duration, x, y, byte): IsQueued main+0x126ae70
//                  (both queues), then a 0x50-byte entry on its stack (+0 flags, +4 duration, +0x10 key, +0x18 category, +0x20 title,
//                  +0x28 modal type 0..4, +0x2c x, +0x30 y, +0x40 player, +0x44, +0x48) -> InsertTutorial main+0x126a0d0(box, &entry, 0)
//   main+0x126a0d0 InsertTutorial(box, entry, requeue): copies the entry into queue box+0x140 (modal) or box+0x158 (non-modal) by its
//                  modal type; the other caller (main+0x126a088, requeue = 1) puts the entry being shown back when another one preempts it.
// The host hooks InsertTutorial (dn_hook, 4 prologue words, none PC-relative): a keyboard-only key in TOUCH -> nothing is inserted (the
// caller's own copy is released by the caller as always); any other entry -> the original. The key is a FixedString: a pointer to its
// characters. Nothing is shown, logged in the Tutorial Log or marked as seen, so in SWITCH / HYBRID the same tutorial still comes.
#ifndef HLE_DOS2_TOUCH_TUTS_H
#define HLE_DOS2_TOUCH_TUTS_H
#include <stdint.h>
#include <string.h>

enum { TU_INSERT = 0x126a0d0,     // InsertTutorial(box, entry, requeue)
       TU_E_KEY = 0x10,           // the entry's key (FixedString)
       TU_KEY_MAX = 64 };         // longest key compared (the longest of the list is 36 characters)

// the 4 prologue words of InsertTutorial (sub sp,sp,#0x60; stp x26,x25; stp x24,x23; stp x22,x21) - another build: no hook
static const uint32_t k_tu_insert_pro[4] = {0xd10183ff, 0xa90167fa, 0xa9025ff8, 0xa90357f6};

// Keys of Public/Shared/Localization/TutorialMessages.lsb whose shipped text names a key, the mouse or a pad button: a key token ([IE_...],
// [LeftStickX]) in any of the 14 Switch languages, or a mouse act ("click", "double click", "arrow keys", "cursor") in English / Russian.
// First the 45 PC keys (what the touch mode gets: the base key), then the 44 console keys - the PC UI never asks for key + "_Controller",
// but the client names some of them itself (TUT_Combat_HotBar_Controller_2 ... are strings of main.raw); a pad text is as wrong on touch.
// Kept against the rule (tools/dos2_ptutorial.py KEEP): TUT_Combat (only the Japanese text adds the End Turn key), TUT_TradeIntro (the trade
// window's own buttons; the iPad shows this PC text too), TUT_PressurePlates ("a clicking sound"). The table's own English is older than the
// shipped texts: TUT_Equipment, TUT_Locked_UseLockpick and TUT_Stealing_Noticed name a key only in english.xml / russian.xml / the others.
// tools/dos2_ptutorial.py check derives the same set from the Switch data and compares it with this list word for word.
static const char *const k_tu_off[] = {
    "TUT_Movement", "TUT_QuestLog", "TUT_Camera", "TUT_Inventory", "TUT_Drag", "TUT_SaveLoad", "TUT_Tooltips", "TUT_Combat_Examine",
    "TUT_ReflectionDialog", "TUT_Locked_Destroy", "TUT_Locked_UseLockpick", "TUT_Equipment", "TUT_Stats_Overview", "TUT_Combat_HotBar",
    "TUT_Combat_AttackMovement", "TUT_Combat_EndTurn", "TUT_Combat_Melee", "TUT_Character_Creation_Class_Basic", "TUT_CompanionSwitch",
    "TUT_Map", "TUT_TradeBarter", "TUT_WoodenDoor", "TUT_Hammer", "TUT_IdentifyingGlass", "TUT_LevelUp", "TUT_RPS", "TUT_SkillUnlock",
    "TUT_DragIcon", "TUT_UseScroll", "TUT_Stealing_Noticed", "TUT_Combat_TacticalView", "TUT_CharacterAssign", "TUT_EquipmentScreen",
    "TUT_InventoryScreen", "TUT_HoldInHand_ContextMenu", "TUT_CameraFree", "TUT_SightRangeIndicator", "TUT_QuestLog_SP", "TUT_QuestLog_MP",
    "TUT_Relationship_Examine", "TUT_WaresInventory", "TUT_HotbarDeck", "TUT_CameraTacticalView", "TUT_HighlightCharacters",
    "TUT_QuestLog_Marker",
    // console texts (pad buttons, the pad cursor) the client may name itself (TUT_Combat_HotBar_Controller_2 and others are in main.raw)
    "TUT_Drag_Controller", "TUT_Homestead_Controller", "TUT_Movement_Controller", "TUT_CompanionSplit_Controller", "TUT_Trade_Controller",
    "TUT_Camera_Controller", "TUT_Selection_Controller", "TUT_Inventory_Controller", "TUT_Combat_AttackMovement_Controller",
    "TUT_Combat_Melee_Controller", "TUT_Combat_HotBar_Controller", "TUT_Book_Controller", "TUT_ReflectionDialog_Controller",
    "TUT_Locked_Destroy_Controller", "TUT_MoveItem_Controller", "TUT_SelectionActiveSearch_Controller",
    "TUT_SelectionCursorMode_Controller", "TUT_Locked_UseKey_Controller", "TUT_Equipment_Controller", "TUT_HotbarAssignment_Controller",
    "TUT_CraftingIntro_Controller", "TUT_CraftingScreen_Controller", "TUT_CompanionSwitch_Controller", "TUT_Combat_Examine_Controller",
    "TUT_CompanionJoinAdvanced_Controller", "TUT_ItemCombo_Controller", "TUT_Lockpick_Controller", "TUT_WoodenDoor_Controller",
    "TUT_UseScroll_Controller", "TUT_Disarm_Controller", "TUT_Hammer_Controller", "TUT_IdentifyingGlass_Controller",
    "TUT_SaveLoad_Controller", "TUT_HotbarFull_Controller", "TUT_Tooltips_Controller", "TUT_CharacterAssignIntro_Controller",
    "TUT_Combat_HotBar_Controller_2", "TUT_WaresInventory_Controller", "TUT_HotbarDeck_Controller", "TUT_SightRangeIndicator_Controller",
    "TUT_Combat_TargetCycle_Controller", "TUT_HotbarSkillInventory_Controller", "TUT_ActiveSearch_Controller",
    "TUT_CombineCrafting_Controller",
};
enum { TU_NOFF = (int)(sizeof k_tu_off / sizeof *k_tu_off) };

// key = the entry's key characters (NULL = none): one of the keyboard-only tutorials (exact match, not a prefix: TUT_QuestLog is in the
// list, TUT_QuestLog_Title or TUT_QuestLog_Controller are not)
static inline int tu_off_key(const char *key) {
    if (!key) return 0;
    for (int i = 0; i < TU_NOFF; i++) if (!strncmp(key, k_tu_off[i], TU_KEY_MAX)) return 1;
    return 0;
}
#endif
