"""[DOS2] The HUD of UI mode TOUCH: the player's own PC movies (Switch Game.pak: hotBar.swf, minimap.swf, statusConsole.swf, playerInfo.swf,
combatLog.swf, watermark.swf) laid out and touchable as the iPad's HUD (docs/dos2/TOUCH_LAYOUT.md; docs/dos2/TOUCH_MODES_PLAN.md "Implemented:
touch HUD"). Names: `<name>_p.swf` (`_p` = PC movie for touch; `_d` is the hybrid's, `_t` the iPad's). No iPad file is read: only numbers of
TOUCH_LAYOUT.md are used; the art is assets/touchui (our drawings, the same files the hybrid uses) and the PC movies' own symbols.
Static only: nothing here was played (no Flash player) or run on a phone.

Scale. The host (dm/host/hle_dos2_touchui_hud.c) gives every object it creates the class scale c = DM_DOS2_TOUCH_HUD_SCALE x (iPad class scale:
1.1 for the portraits, else 1) / S, so the engine's "fixed" layout k = c x S x screen height / 1080 (main+0xae3be0) draws a movie's stage px as
DM_DOS2_TOUCH_HUD_SCALE screen px on a 1080-high screen. So every number below is in the design px of a 1080-high screen, as TOUCH_LAYOUT.md.

  hotBar_p.swf         hotBar.swf, document class patched (methods appended, three traits re-pointed; no original body or tag changed):
      dmLay()          hotbar_mc x HB_S (pitch 55.07 -> 86 px, slot 74 x 78; iPad: 76 in an 82 cell, pitch 86), cropped by scrollRect to
                       [CROP .. 12 slots + the actions button] (the PC portrait, the six panel buttons - map / journal ... - and the actions
                       button's PC place are cut: the panels are dmSide_p's column); the PC actionsButton_mc moved after the 12th slot
                       (ACT_BTN, the iPad's actionsBtn_mc), slots 13.. hidden; the row as the iPad's hotBar_t, centred and symmetric:
                       [dmChat_mc 84] 16 [bar] 16 [dmLog_mc 84] (our square copper plates + our glyphs; the PC chat / log tabs hidden, the
                       plates call their ExternalInterface ToggleChatLog / CombatLogBtnPressed + UI_Generic_Click); slots' bottom at screen
                       y 1049, everything on one centre line; ExternalInterface updateSlots(12)
      showSkillBar(on) the original (the PC sets visible = true on its whole skillBar list: the lock, the slot numbers), then dmVis: the lock
                       only with the open deck list, no slot numbers, the plates follow (owner 23.11: "a padlock hangs over the skill bar")
      onEventResolution(w, h) -> dmLay() (the PC code sized the bar from the screen width: up to 29 slots)
      onEventInit()    the original, then dmLay() and dmHook(): a horizontal swipe over the slots (> SWIPE stage px, not while a slot is
                       dragged) = previous / next deck through the PC cycleHotBar (prevHotbar / nextHotbar); the deck arrows get a finger-sized
                       hit area; the CONFIRM circle and the cancel button (root instances dmConfirm_mc / dmCancel_mc at the iPad's place)
      showActiveSkill(slot)  the original, then CONFIRM / cancel are shown while the engine marks a prepared skill (slot >= 0) and
                       ExternalInterface dmAim(0 | 1) tells the host; a tap on them = dmAimBtn(1 confirm | 0 cancel) (host: the PC's left
                       click at the aimed point / right click = cancel). showActiveSkill(AIM_SHOW | AIM_HIDE) from the host: CONFIRM /
                       cancel for a prepared state the movie does not see (a skill from the book / another deck, a throw: the host reads the
                       input controller's tasks; owner 23.11 "a tap only aims, the confirm button casts")
      setActionPreview(id, on)  the original, then for "ActionAttackGround" ExternalInterface dmAim(2 | 3) (the engine marks / drops the
                       attack-ground preview: its hot bar update main+0x1233360 invokes this on each change of main+0x109b500)
      CONFIRM / cancel art (owner 23.11, his iPad shot): our own, drawn in code (aim_art): a thin light metal ring around a dark translucent
                       disc; CONFIRM carries the game's own word across it (dmLbl: "CONFIRM" / "ПОДТВЕРДИТЬ", english.xml / russian.xml
                       h0fb8bf07..), cancel a white X; under them the hint banner dmHint_mc (the player's notification.swf cast-notice banner,
                       3 parts stretched, + our hint text dmTxt), shown with cancel; the language: a Cyrillic letter in setFixedBtnTooltips /
                       setLockBtnTooltips texts (dmLang); words get the PC stroke filter
      HUD extras ("Implemented: touch HUD extras"): the deck list (the iPad's deck switcher) - the PC deck number cycleHotBar_mc is the button (arrows hidden, a
                       plate under the number), a tap opens / closes the backing dmDeckBg_mc with the PC lock moved in and five plates dmDeck1..5_mc
                       (k pips = deck k), a tap on one runs the PC cycleHotBar as many times as it takes (nextHotbar) and closes the list; the
                       deck indicator dmInd1..5_mc under the slots (the current deck's bar is bright; followed on enterFrame from the PC's
                       currentHotBarIndex). The ACTION PLATE: showActiveSkill(2000000 + stageX * 2000 + stageY) from the host (a world tap on
                       an object) puts the CONFIRM circle PLATE_UP px above that point, 1999999 hides it; a tap on it = dmAimBtn(1) as CONFIRM.
      Everything else is the PC movie's own mouse code: tap a slot = SlotPressed, hover (long press = the host's right click / still finger)
      = tooltip, drag from a slot = startDragging / stopDragging (unlocked bar), lock, actions list, chat, combat log buttons.
  minimap_p.swf        minimap.swf: the ring group moved (-42, +29), ping (-4, -61), home / diplomacy above the ring (TOUCH_LAYOUT 2.3);
                       a tap on the ring (press and release on hit_mc) = ExternalInterface dmSide(2) = the map; the iPad's view menu
                       (MM_BTNS): dmMmView_mc opens an arc of toggles - tactical camera dmKey(2), item labels dmKey(3), sight cones dmKey(4),
                       camera lock dmKey(5) - and "?" (dmMmInfo_mc: their names dmMmL1..4_mc); dmMmSave_mc (floppy) = dmKey(1) quick save.
                       The stage grows MM_GROW to the left (root children shifted at init). The host tells the toggles' state through
                       setBtnVisible(MM_STATE + bits) (an "on" ring + full glyph); showMiniMap hides our buttons with the map
  statusConsole_p.swf  statusConsole.swf: console / fight buttons / turn notice / GM button raised by SC_UP more (the hot bar is taller);
                       the iPad's combat row (SC_*): grey | red | blue bars (iPad 309 / 505 / 309) flush on the hot bar's frame and as wide
                       as it (dmScBars, again after every updateArmourBar: the PC forces the armour holders' scaleX -1 / 1); out of combat
                       the host shows the object again and setBtnVisible(SC_EXPLORE) = bars only (the PC frame hidden), Wait /
                       Flee as text plates dmWait_mc / dmFlee_mc (the PC End turn plate art + the game's words; they follow the PC duo
                       buttons' visible / disabled, call GuardPressed / FleePressed), the PC End turn plate in their row; finger pads
  combatLog_p.swf      combatLog.swf opened at the screen centre as the iPad's (setPosition center / screen / center in onEventInit and
                       onEventResize instead of bottomright); the PC window, its drag, resize, filters and wheel scroll are untouched
  playerInfo_p.swf     the PC movie as it is (the portraits' 1.1 is the host's class scale; their PC mouse code is the
                       iPad's: tap = select, double = centre, right click = inventory, drag = regroup). Own names so a later touch edit needs
                       no host change.
  dmSide_p.swf         watermark.swf (UI type 0x8d, a plain UIObject) + the iPad's right side (rightSideButtons_t numbers, 2.4): frame
                       SIDE_W x 1080 anchored topright / screen / topright (the physical scale grows it from the corner); options (plate_round_copper + icon_menu), the round HELMET button
                       (btn_character; toggles the column locally: the iPad's toggleSidePanel / setRolledIn pair is not in the Switch main),
                       six panel buttons 74 px every 80 px from y 422: map, character, inventory, skills, crafting, journal.
                       A tap = ExternalInterface dmSide(code): 1 options, 2..7 the panels in that order.
  Panels (stage 4, "Implemented: touch panels"): partyInventory_p characterSheet_p skills_p uiCraft_p journal_p examine_p waypoints_p = the PC
                       movies with one addition, the iPad's 50 px close button: the PC's 23 px close button gets a transparent 50 x 50 hit area
                       (PANEL_CLOSE: where it sits in each movie). Size, position and every other gesture are the PC movie's own (tap, double
                       click = use / equip, right click = context menu, drag, wheel): see the plan for why the iPad's 76 px cells are not here.
                       partyInventory_p and characterSheet_p are made by tools/dos2_ppanel_inv.py instead (the iPad's layout: centred, x 1.52 / x 1.2).

  python -I tools/dos2_pchud_touch.py build <Switch Game.pak | dir of the PC movies> <out dir>
  python -I tools/dos2_pchud_touch.py check <Switch Game.pak | dir of the PC movies>
  python -I tools/dos2_pchud_touch.py mock  <Switch Game.pak | dir> <out.png> [--w 2400]   the computed layout drawn from the movies' own
        symbols at the positions the patched code sets (a picture of the NUMBERS, not a played movie)
"""
import hashlib, math, os, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, NS_PUBLIC, MN_MULTIL
NS_AS3 = 'http://adobe.com/AS3/2006/builtin'   # String methods are called in it (charCodeAt), as the movies' own code does
import dos2_swf_graft as gr
import dos2_touchart as ta
import dos2_ppanel_skills as pps   # skills_p / uiCraft_p / journal_p (+ map) in the iPad's layout: that module builds and checks them

GUI = 'Public/Game/GUI/'
# ---- hot bar (PC stage 1948 x 1000, anchored bottom: stage x = screen x - (W - 1948) / 2, stage y = screen y - 80)
HB_S = (1562, 1000)              # hotbar_mc scale: 86 / (58 x 0.9494)
SLOTS = 12
PC_FIRST, PC_PITCH, PC_SLOT_W, PC_SLOT_Y, PC_SLOT_H = 339.0, 58 * 0.9494, 50 * 0.9494, 64.0, 50.0   # hotbar_mc px (slotholder_mc 339,64 scaleX 0.9494)
# The row as the iPad's hotBar_t (owner 23.11 "the bottom bar looks worse: the chat button apart and framed, the combat log button framed, the
# actions menu to the right as it should be, no map / journal there"): [chat plate] gap [deck number | 12 slots | actions] gap [combat log plate].
# CROP cuts the PC portrait, the six panel buttons (75..227: the map / journal the player still saw) and the actions button's PC place (255);
# the PC actions button moves into the bar right after the 12th slot (the iPad's actionsBtn_mc 1246..1330 after the slots 216..1244).
CROP = 286                       # hotbar_mc px cut on the left: the deck number's plate starts at 288.6 (DECK_PAD), the lock (295) lives in the list
ACT_BTN = (995, 63)              # actionsButton_mc (34 x 52) in hotbar_mc px: 3 px after the 12th slot (992.2); the 13th slot (999.8..) is hidden
VISW = ACT_BTN[0] + 34 + 3 - CROP   # ..to 3 px past the actions button
VISH = 180
IPAD_SLOT_BOTTOM = 1049          # screen y of the slots' bottom (hotBar_t)
_s = HB_S[0] / float(HB_S[1])
HB_X = int(round(974 - VISW * _s / 2.0))   # the bar centred, the two plates symmetric about it: the whole row is centred (owner 23.11, review rule 4)
HB_Y = int(round(IPAD_SLOT_BOTTOM - 80 - (PC_SLOT_Y + PC_SLOT_H) * _s))
SLOT_MID_Y = HB_Y + (PC_SLOT_Y + PC_SLOT_H / 2.0) * _s          # the row's centre line (stage px): slots, plates, actions button
PLATE, GAP = 84, 16              # the iPad's chatBtn_mc / combatLog_mc 84 x 84; the gap one rhythm unit (16) from the bar
PLATE_PAD = 112                  # their finger pads (transparent, centred): 141 screen px = ~9 mm at the phone's x1.26; 2 px short of the bar
GLYPH = 0.6                      # glyph size in a plate (all our plates: chat, log, minimap buttons)
CHAT_BOX = (HB_X - GAP - PLATE, int(round(SLOT_MID_Y - PLATE / 2.0)), PLATE)              # dmChat_mc: x, y, size
LOG_BOX = (HB_X + int(round(VISW * _s)) + GAP, int(round(SLOT_MID_Y - PLATE / 2.0)), PLATE)   # dmLog_mc
ACT_X, ACT_Y = int(HB_X + (ACT_BTN[0] + 17 - CROP) * _s), int(HB_Y + 57.5 * _s + 11 * _s - 144 * _s)   # actionSkillHolder_mc: over the button
EXP_SX = (517, 1000)             # expBar_mc.scaleX: from its x 69 to the actions button's right end (1029)
SWIPE = 130                      # stage px (1.5 slots)
CONFIRM = (974, 732 - 80, 118)   # centre x, y, size: the iPad's 118 circle at screen y 732 (its owner shot 23.11: the circle, then the hint
CANCEL = (974 + 201, 760 - 80, 69)   # banner under it, both over the health bar); cancel 69 at +201, its centre 28 px lower (the iPad's offset)
HINT = (974, 722, 640, 0.75)     # dmHint_mc: centre x, top y, width, scale of the Switch banner (notification.swf notCast bg, 64 high -> 48)
HINT_SRC = ('notification.swf', 107)   # the player's notification movie: its cast-notice banner (3 parts: ornamented left / stretch / right)
LBL_FONT, HINT_FONT = 24, 22     # px: the CONFIRM word, the hint (owner's rule: nothing under 22 px on a 1080 screen)
# texts: CONFIRM = the game's own "Confirm" (english.xml / russian.xml h0fb8bf07g3932g4ccbg8659g2f4f5aa7dd82, upper case as the iPad shows it);
# the hint is ours (no such string in the Switch localization). The language: a Cyrillic letter in a text the engine gives the movie.
CONF_EN, CONF_RU = 'CONFIRM', 'ПОДТВЕРДИТЬ'
CONF_HANDLE = 'h0fb8bf07g3932g4ccbg8659g2f4f5aa7dd82'
HINT_EN, HINT_RU = 'Tap Confirm to use the skill', 'Нажмите «Подтвердить», чтобы применить'
# the deck switcher as the iPad's (hotBar_t btnDeckSwitcher_mc 46 x 84 left of the slots: a tap opens / closes deckSwitcher_mc 359 x 107 above
# the bar, its lock 44 x 44 inside; owner 23.11 "do it the iPad way"): the PC deck number cycleHotBar_mc (its text_txt) becomes that button
# (arrows hidden, a plate drawn under the number, DECK_PAD own px around its bounds), the list = a backing, the PC lock moved into it, 5 plates
DECK_PAD = (12, -18, 6)                    # own px of the number's plate past its bounds: left (the lock's old place), right, top / bottom
# owner 23.11 (phone): "the 1 centred, the button nicer and 20 % narrower": the plate 44 -> DECK_W own px wide, centred where the 44 px one
# stood (the bar's crop and the host's row numbers unchanged), rounded with an inner line; text_txt spans the plate, centred (TextFormat align)
DECK_W, DECK_X, DECK_R = 35, -8, 10       # plate width, its left from the number's bounds x (= -12 + (44 - 35) / 2, rounded), corner size
DECK_TY = -4                               # the number moved up this much (its field sat 5 px under the plate's middle)
DECK_ART = (34, 52)                        # the backing: the PC actions button's art (hotBar_fla.actionButtonBG_28), its size in hotbar_mc px
def DECK_BG_CLS(d): return d.abc.pub('actionButtonBG_28', 'hotBar_fla')
# right -18: the bounds reach past the number over the first slot (phone 23.11 at x1.56: the plate covered 22 px of slot 1, owner "наплыла на скилы")
DECK_BG = (408, 709, 406, 76)              # dmDeckBg_mc: x, y, w, h (stage px; its foot (screen 865) above the combat row of statusConsole_p (875..927), 23.11)
DECK_LOCK = (416, 717, 60)                 # the PC lockButton_mc fitted (centred, by its height) into this box: x, y, size
DECK_LIST = (482, 717, 60, 66)             # dmDeck1..5_mc: x of the first, y, size, pitch
SLOT_MID_X = HB_X + (PC_FIRST + ((SLOTS - 1) * PC_PITCH + PC_SLOT_W) / 2.0 - CROP) * _s
IND = (int(round(SLOT_MID_X - 204)), 988, 80, 5, 82)   # dmInd1..5_mc: x of the first, y, bar w, h, pitch (hotBar_t indicator_mc: 5 bars 80 x 5 every 82), under the slots' middle
ROW_HALF, HB_FRAME_TOP, HB_END_TOP = 683, 875, 875   # the row's half width, its frame's top, the top at its ends (stage px): hle_dos2_touch_scale.h
                                 # TS_HB_HALF / TS_HB_TOP / TS_HB_TOP_L / _R (the plates' top 888 is under the frame's 875)
CHAT_CODE, LOG_CODE = 40, 41     # dmBAct codes of the two plates: the PC buttons' own ExternalInterface calls (ToggleChatLog / CombatLogBtnPressed)
ACT_FOOT = HB_FRAME_TOP - 2      # stage px: dmTick keeps the actions list's foot on the bar frame's top whatever its height (phone 23.11)
PLATE_UP = 110                   # the action plate's centre above the tapped point
PLATE_BASE, PLATE_HIDE, PLATE_MUL = 2000000, 1999999, 2000
AIM_SHOW, AIM_HIDE = 1999998, 1999997   # the host's CONFIRM / cancel show / hide (dm/host/hle_dos2_touchui_hud.h HU_AIM_SHOW / HU_AIM_HIDE)
# ---- minimap (PC stage 420 x 2000, anchored topright)
MM_MOVE = [('hit_mc', -42, 29), ('map_mc', -42, 29), ('tension_mc', -42, 29), ('north_mc', -42, 29), ('armourDoll_mc', -42, 29),
           ('tensionHit_mc', -42, 29), ('title_txt', -42, 29), ('pingButton_mc', -4, -61), ('homestead_mc', 136 - 111, 47 - 94),
           ('diplomacy_mc', 268 - 111, 47 - 178)]
# The iPad's view menu (minimap_t viewBtn_mc: an arc of toggles left / under the ring, a "?" for their names; owner 23.11 "where did the menu
# by the map go?"). Ours: the toggles the Switch engine has an input for (Eoc/keyboard_1.json of the player's Engine.pak, main.raw's event
# table main+0x108c1e0.. registered by 0x847850): ToggleTacticalCamera (key o, kind 2 = a toggle), ShowWorldTooltips (lalt, kind 3 = held),
# ShowSneakCones (lshift, kind 3 = held); the camera lock is the host's (CameraCenter = home, then no pan). The iPad's "active search" has no
# input event on the Switch (only the controller tutorial TUT_ActiveSearchInterface_Controller): left out.
MM_RING = (227, 187)             # the ring's centre after MM_MOVE (old-frame stage px: x of a 420 stage)
def _arc(r, deg): import math; return (MM_RING[0] + r * math.cos(math.radians(deg)), MM_RING[1] + r * math.sin(math.radians(deg)))
MM_TGL = 90                      # the view button and the toggles (iPad 67; owner 23.11: 64 -> 77 -> 90 "a bit bigger"): plate_round_copper, glyph GLYPH
MM_SMALL = 72                    # info / quick save (62 -> 72)
MM_RING_R = 130                  # the ring's radius (the map mask 260): no button drawing and no hit of ours inside it (the map's own tap)
# name, code (10 + dmKey n = the host; 20 = the arc, 21 = the names), icon, centre x, y, size, the host's state bit (0 = none)
MM_BTNS = [('dmMmView_mc', 20, 'icon_view') + _arc(MM_RING_R + MM_TGL / 2.0 + 6, 180) + (MM_TGL, 0)] + [
    (n, c, i) + _arc(285, deg) + (MM_TGL, b) for n, c, i, deg, b in (
        ('dmMmT1_mc', 12, 'icon_tactical_s', 195, 1), ('dmMmT2_mc', 13, 'icon_labels', 165, 2),
        ('dmMmT3_mc', 14, 'icon_cones', 135, 4), ('dmMmT4_mc', 15, 'icon_camlock', 105, 8))] + [
    ('dmMmInfo_mc', 21, 'icon_info') + _arc(205, 226) + (MM_SMALL, 0),   # r 205: its top stays on the stage (y 3)
    ('dmMmSave_mc', 11, 'icon_save', 355, 187, MM_SMALL, 0)]   # quick save: the iPad's quickSaveBtn_mc 1351..1399 (48) on the ring's right edge
MM_ARC = ['dmMmT1_mc', 'dmMmT2_mc', 'dmMmT3_mc', 'dmMmT4_mc', 'dmMmInfo_mc']   # shown while the view menu is open
MM_LBL = [('dmMmL1_mc', 'dmMmT1_mc', 'Tactical camera', 'Тактическая камера'), ('dmMmL2_mc', 'dmMmT2_mc', 'Item labels', 'Подписи предметов'),
          ('dmMmL3_mc', 'dmMmT3_mc', 'Sight cones', 'Конусы обзора'), ('dmMmL4_mc', 'dmMmT4_mc', 'Camera lock', 'Фиксация камеры')]
MM_LBL_W, MM_LBL_H, MM_LBL_FONT = 250, 40, 22   # the names left of their toggle ("?"), a dark translucent backing
MM_STATE = 100                   # setBtnVisible(MM_STATE + bits, ..) from the host = the toggles' state (the class registers that invoke name)
MM_W = 420                       # the PC stage width; every number here is in it (the iPad's place: the ring centred ~194 px from the right edge)
MM_GROW = 360                    # the stage grows this much to the LEFT (every root child shifted +MM_GROW at init): the arc and its names
                                 # reach x -330 of the old frame; the screen place of everything stays W - (MM_W - x)
MM_PAD = [('pingButton_mc', 64), ('homestead_mc', 64)]   # the PC ring buttons' finger pads; ours have no pads: one hit area, nearest button
# Phone 23.11 (53 measured presses, 60 % worked): 13 failed because the finger was held (the host's short long press releases off the element,
# its long press is a right click) - our buttons acted on the release; 8 landed between buttons, outside every fixed pad. Owner: "tap on, tap
# off, no holding". Now ONE transparent hit area dmMmHit_mc (just below the PC ping / home buttons, above the map): while the menu is open an
# annular sector around the ring (MM_RING_R .. MM_HIT_R, MM_HIT_DEG) - the ring itself stays the map's - plus a square on the quick save; while
# closed a square on the view button and the save square. A PRESS there picks the nearest visible button within MM_PICK_R (Voronoi: the space
# between buttons belongs to the nearest) and acts at once (dmBAct); the release only ends the press look. Our buttons take no mouse.
MM_PICK_R = 115
MM_HIT_R = 285 + MM_PICK_R
MM_HIT_DEG = (82, 256)           # T4 at 105 - 23 deg (115 px at r 285) .. info at 226 + 30 deg (115 px at r 215)
MM_HIT_SAVE = 86                 # the save square (its old pad: it sits on the ring's rim, as the iPad's)
MM_HIT_VIEW = 130                # the view button's square while the menu is closed   # owner 23.11: labels / cones still hard to hit -> toggles 30 deg apart
                                 # on r 285 (centres 148 px apart), pads 150 (~12 mm at the phone's x1.26)   # finger pads (transparent, stage px) around the small ring buttons
                                 # (48 / 64 drawn): the minimap stays at x 1 (owner 20.11), the touch target grows; the save pad stays off the ring
# ---- status console (PC stage 1920 x 1080, anchored bottom; its own code already raises by bottomBarHeight 67)
SC_UP = 33                       # health bar 1043 - 67 - 33 = 943 (statusConsole_t: 943)
SC_MOVE = ['console_mc', 'fightButtons_mc', 'turnNotice_mc', 'toGM_mc']
# The iPad's combat row (owner 23.11 "these combat buttons too", iPad shot: Wait / Flee text plates left, the turn plaque and the AP dots in the
# middle, End turn right; the health bar and the two armour bars long and thin right on the hot bar's frame; statusConsole_t, TOUCH_LAYOUT 2.5).
# Stage = statusConsole's 1920 x 1080 (anchor bottom: screen x = x - (1920 - W) / 2); console_mc / fightButtons_mc stand at y 980 after
# bottomBarHeight 67 + SC_UP (their own px below). The bars: the PC holders scaled to the iPad's lengths, their tops on screen y 940 / 942.
SC_ORG = (960, 1080 - 67 - SC_UP)                    # console_mc / fightButtons_mc origin on the stage
# The bars sit FLUSH on the hot bar's frame, together as wide as it (owner 23.11, phone: "the red one floats, the grey one is short and off to
# the left, the blue one is not seen"): grey (physical armour) | red (health) | blue (magic armour) in the iPad's 309 / 505 / 309, SC_BAR_INSET
# from the frame's ends and SC_BAR_GAP apart. Both movies are anchored bottom-centre with the same factor (the host: 0x28 and 0x75 alike),
# so stage numbers hold on every screen width: status stage x = hot bar stage x + SC_DX, y = hot bar stage y + 80.
HB_FRAME_ART = 57                # hotbar_mc own px of the frame's top line (its first opaque row: measured on the rendered symbol)
SC_DX = 960 - 974
SC_FOOT = HB_Y + HB_FRAME_ART * _s + 80                        # 960: the frame's top on the status stage (phone 23.11: ~960)
SC_SPAN = (HB_X + SC_DX, HB_X + VISW * _s + SC_DX)             # the frame's ends (377 .. 1542)
SC_BAR_INSET, SC_BAR_GAP = 6, 6
SC_BAR_K = (SC_SPAN[1] - SC_SPAN[0] - 2 * SC_BAR_INSET - 2 * SC_BAR_GAP) / float(309 + 505 + 309)
SC_GREY = (SC_SPAN[0] + SC_BAR_INSET, SC_SPAN[0] + SC_BAR_INSET + 309 * SC_BAR_K)
SC_RED = (SC_GREY[1] + SC_BAR_GAP, SC_GREY[1] + SC_BAR_GAP + 505 * SC_BAR_K)
SC_BLUE = (SC_RED[1] + SC_BAR_GAP, SC_RED[1] + SC_BAR_GAP + 309 * SC_BAR_K)
SC_HB = (SC_RED[0], SC_FOOT - 16, (SC_RED[1] - SC_RED[0]) / 476.0, 1.0)    # hbHolder_mc: left, top, scaleX, scaleY (its drawing 476 x 16)
# the armour holders (drawing 198 x 12): Console_1.setLeftArmourBar forces leftArmourBarHolder_mc.scaleX = -1 (the PC draws it mirrored,
# growing leftwards from its origin) and setRightArmourBar the right one's = 1 on every update - so the origin of the left one is the RIGHT
# end of its span, and dmScBars puts the scale back after every updateArmourBar (the engine's only path to those setters)
SC_ARM = [('leftArmourBarHolder_mc', SC_GREY[1], -1), ('rightArmourBarHolder_mc', SC_BLUE[0], 1)]   # name, origin x, scaleX sign
SC_ARM_S = (309 * SC_BAR_K / 198.0, 1.0)
SC_ARM_TOP = SC_FOOT - 12
SC_EXPLORE = 100                 # setBtnVisible(SC_EXPLORE, ..) from the host (hle_dos2_touchui_hud.c HU_SC_EXPLORE): out of combat, bars only
# One vertical stack above the bars, even gaps (owner 23.11, phone: "the turn plaque runs over the action points"): the PC resets the plaque
# to its class const originalNotificationPosY (873) on every notice - our SC_UP raise was lost and the plaque's foot (873 + 22) covered the
# dots (ap_mc at 888). Now: bars (top SC_HB[1]) - gap - ap_mc (its drawing 0..36: dots, studs, glow, the 7+ overflow 0..31) - gap - the plaque
# (centred on its y, 43 high on the phone) with the const patched to SC_NOTICE_Y; the text plates' row ends with the AP row (the iPad: at the
# dots' height). The PC's dark backing behind its old bar place (Console_1's unnamed shape #94, child 0: 500 x 35 at -250..250 x -54..-19 of
# console_mc) was the black band above the red bar: hidden. The PC number fields go ON their bars (shown by the PC hover only).
SC_STACK_GAP, SC_AP_H, SC_NOTICE_HALF = 8, 36, 22
SC_AP_Y = SC_HB[1] - SC_STACK_GAP - SC_AP_H                 # 900
SC_NOTICE_Y = SC_AP_Y - SC_STACK_GAP - SC_NOTICE_HALF       # 870: the plaque 848..892
SC_BTN_H = 52                                               # the PC End turn plate's own 52 (iPad 879..923, 44 high)
SC_BTN_Y = SC_AP_Y + SC_AP_H - SC_BTN_H                     # 884: the plates' feet on the AP row's foot (936)
SC_TXT = [('hbTxt_mc', SC_RED, 470, SC_FOOT - 8), ('abTxt_mc', SC_GREY, 225, SC_FOOT - 6), ('mabTxt_mc', SC_BLUE, 225, SC_FOOT - 6)]   # field, its bar, width, centre y
SC_END_X = 1272                                      # endBtn_mc (204 x 52, the engine's caption): iPad endBtn_mc 1272..1478
SC_PLATES = [('dmWait_mc', 1, 328, 'Delay Turn', 'Отложить ход'), ('dmFlee_mc', 2, 528, 'Flee', 'Бежать')]   # name, code, x; captions: the game's
SC_PLATE_W, SC_FONT = 192, 22                        # own words (english.xml h157c01cd.. "Delay Turn", russian.xml hcf2cc556.. "Отложить ход", hb399d83e.. Flee / Бежать)
SC_FIT_MIN = 14                  # captions shrink (TextFormat size) until they fit the plate's inner width, not below this
SC_PAD = (8, 24, 6)                                 # the plates' finger pads: px past the drawing left / right, above, below (90 high: ~7.4 mm at the
                                                     # phone's x1.26; never over the hot bar 955 below, under the hint banner's foot 850 above)
# ---- the right side (rightSideButtons_t: stage 1440 x 1080 anchored right; here the last SIDE_W px of it)
SIDE_W, SIDE_H = 140, 1080       # frame, anchored topright (the physical scale grows it from the corner: the iPad's places, owner 20.11)
_sx = lambda x: x - (1440 - SIDE_W)
SIDE = [('dmMenu_mc', 1, 'icon_menu', _sx(1348), 20, 74), ('dmHelm_mc', 20, 'btn_character', _sx(1316), 310, 105)] + [
    (n, c, i, _sx(1348), 422 + 80 * k, 74) for k, (n, c, i) in enumerate([('dmP2_mc', 2, 'icon_map'), ('dmP3_mc', 3, 'ico_charsheet'),
    ('dmP4_mc', 4, 'ico_inventory'), ('dmP5_mc', 5, 'ico_skills'), ('dmP6_mc', 6, 'ico_craft'), ('dmP7_mc', 7, 'ico_journal')])]
SIDE_COLUMN = [s[0] for s in SIDE if 2 <= s[1] <= 7]
# ---- panels: the close button of each PC panel (instance path from the root) gets a CLOSE_HIT px hit area around its 23 px drawing
CLOSE_HIT = 50
PANEL_CLOSE = {'partyInventory': ('inventory_mc', 'close_mc'), 'characterSheet': ('stats_mc', 'close_mc'), 'skills': ('skillPane_mc', 'Xclose_mc'),
               'uiCraft': ('craftPanel_mc', 'closeButton_mc'), 'journal': ('journal_mc', 'close_mc'), 'examine': ('examine_mc', 'close_mc'),
               'waypoints': ('waypoints_mc', 'Xclose_mc')}
OUT = {'hotBar_p.swf': 'hotBar.swf', 'minimap_p.swf': 'minimap.swf', 'statusConsole_p.swf': 'statusConsole.swf', 'playerInfo_p.swf': 'playerInfo.swf',
       'combatLog_p.swf': 'combatLog.swf', 'dmSide_p.swf': 'watermark.swf'}
OUT.update({n + '_p.swf': n + '.swf' for n in PANEL_CLOSE})
EI_NEW = ['dmSide', 'dmAim', 'dmAimBtn', 'dmKey']   # ExternalInterface names the host implements (hle_dos2_touchui_hud.c)


SW_PAK = None   # the Switch Game.pak sources() read (romfs/Data of the game folder)
EXTRA = ['notification.swf']   # art sources only (not rebuilt): the hint banner of hotBar_p (HINT_SRC)
SRC_EXTRA = {}


def sources(path):
    """name -> bytes of the PC movies this pass needs (a Switch Game.pak or a directory of extracted movies)"""
    need = sorted(set(OUT.values()))
    if os.path.isdir(path):
        out = {n: (Path(path) / n).read_bytes() for n in need}
        for n in EXTRA:
            if (Path(path) / n).is_file(): out[n] = (Path(path) / n).read_bytes()
        SRC_EXTRA.update({n: out[n] for n in EXTRA if n in out}); return out
    global SW_PAK; SW_PAK = path   # the checks of the tutorial / loading-tip movies read Shared.pak, Localization next to it
    import lspk
    pak = lspk.Pak(path); by = {e[0]: e for e in pak.entries}
    miss = [n for n in need if GUI + n not in by]
    if miss: sys.exit('%s: no %s' % (path, ' '.join(miss)))
    out = {n: pak.read(by[GUI + n]) for n in need}
    SRC_EXTRA.update({n: pak.read(by[GUI + n]) for n in EXTRA if GUI + n in by}); out.update(SRC_EXTRA)
    return out


# ---------------------------------------------------------------- ABC helpers (append-only, as tools/dos2_column_touch.py)
class Doc:
    """the document class of a movie: methods are appended, a trait can be re-pointed with the original kept under another name"""
    def __init__(s, swf):
        s.swf = swf; (s.tag, s.pre, s.abc), = swf.abcs(); abc = s.abc
        docs = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']).endswith('.MainTimeline')]
        assert len(docs) == 1 and not abc.instances[docs[0]]['flags'] & 1, 'document class not found or sealed'
        s.ci = docs[0]; s.inst = abc.instances[s.ci]
        ti = abc.trait(s.ci, 'onEventInit'); assert ti is not None and ti.kind & 15 == 1 and not abc.methods[ti.b].params, 'onEventInit()'
        s.ns = abc.mn[ti.name][1]; assert abc.ns[s.ns] == (NS_PUBLIC, abc.string('')), 'onEventInit is not public'
        s.ref = abc.methods[ti.b].body
        s.EI = abc.pub('ExternalInterface', 'flash.external'); s.made = []
        assert abc.trait(s.ci, 'dmHook') is None, 'already patched'
    def P(s, name): return s.abc.qname(s.ns, name)
    def S(s, text): return s.abc.string(text)
    def add(s, name, nparams, lines, like=None):
        abc = s.abc
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 64, 64, s.ref.init_scope, s.ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        if like is not None:
            abc.methods[m].ret = like.ret
            if like.flags & 0x08: abc.methods[m].flags |= 0x08; abc.methods[m].options = list(like.options)
        s.made.append(m); return m
    def method(s, name, nparams, lines):
        s.inst['traits'].append(Trait(s.P(name), 1, 0, s.add(name, nparams, lines)))
    def wrap(s, name, keep, lines_of):
        """trait `name` -> a new method built by lines_of(n params); the original stays callable as `keep`"""
        t = s.abc.trait(s.ci, name); assert t is not None and t.kind & 15 == 1, name
        orig = s.abc.methods[t.b]; n = len(orig.params)
        m = s.add(name, n, lines_of(n), like=orig)
        old = t.b; t.b = m; s.inst['traits'].append(Trait(s.P(keep), 1, 0, old))
    def done(s): s.swf.tags[s.tag][1] = s.pre + s.abc.write()
    def kc(s, code, env=None):
        """methods written in the Python subset of tools/dos2_touchkit.py kc_compile (appended as public traits, as method())"""
        import dos2_touchkit as tk
        abc = s.abc
        if not hasattr(s, 'late'): abc.nssets.append([s.ns]); s.late = len(abc.mn); abc.mn.append((MN_MULTIL, len(abc.nssets) - 1))
        full = {'EI': ('lex', s.EI), 'Math': ('lex', abc.pub('Math')), 'String': ('call', abc.pub('String'))}; full.update(env or {})
        as3 = abc.qname(abc.namespace(8, NS_AS3), 'charCodeAt'); pub = s.P('charCodeAt')
        for name, (np, rest, lines) in tk.kc_compile(abc, s.ns, s.late, full, code).items():
            assert not rest, name
            s.method(name, np, [(l[0], as3) + l[2:] if len(l) > 1 and l[1] == pub and l[0].startswith('callprop') else l for l in lines])
    def lex(s, name):
        """the multiname the movie's own code uses for a class it reaches by getlex (textEffect ...)"""
        abc = s.abc
        return next(i for i in range(1, len(abc.mn)) if abc.mn[i][0] == 7 and abc.mname(i).split('.')[-1] == name)
    def abc_int(s, v):
        """index of an int constant (append-only pool)"""
        if v in s.abc.ints[1:]: return s.abc.ints.index(v, 1)
        s.abc.ints.append(v); return len(s.abc.ints) - 1
    # code fragments
    def ei(s, name, *args): return [('getlex', s.EI), ('pushstring', s.S(name))] + [x for a in args for x in a] + [('callpropvoid', s.P('call'), 1 + len(args))]
    def get(s, *path): return [('getlocal0',)] + [('getproperty', s.P(p)) for p in path]
    def child(s, name, loc): return [('getlocal0',), ('pushstring', s.S(name)), ('callproperty', s.P('getChildByName'), 1), ('coerce_a',), ('setlocal', loc)]
    def call_orig(s, keep, n): return [('getlocal0',)] + [('getlocal', k + 1) for k in range(n)] + [('callpropvoid', s.P(keep), n)]


HEAD = [('getlocal0',), ('pushscope',)]
def num(v):
    v = int(v)
    if -128 <= v <= 127: return [('pushbyte', v & 0xff)]
    return [('pushshort', v)] if v >= 0 else [('pushshort', -v), ('negate',)]
def frac(f): return num(f[0]) + num(f[1]) + [('divide',)]


def press_code(d, act, dim=True, note=None):
    """dmBDown(e) / dmBUp(e) / dmBAct(code): press (dims), the stage's release on the same button runs act(code in local 1) -> lines"""
    P, S = d.P, d.S
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
        ('getlocal1',), ('getproperty', P('currentTarget')), ('coerce_a',), ('setlocal', 2),
        ('getlocal0',), ('getlocal', 2), ('setproperty', P('dmB'))]
    if dim: code += [('getlocal', 2), ('pushbyte', 6), ('pushbyte', 10), ('divide',), ('setproperty', P('alpha'))]
    if note: code += [('getlocal0',), ('getlocal0',), ('getproperty', P(note)), ('setproperty', P('dmNote'))]   # a state of the movie as it was at the press
    code += [('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal', 3), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmBUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)]
    d.method('dmBDown', 1, code)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
        ('getlocal0',), ('getproperty', P('dmB')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmB'))]
    if dim: code += [('getlocal', 2), ('pushbyte', 1), ('setproperty', P('alpha'))]
    code += [('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal', 3), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmBUp')), ('callpropvoid', P('removeEventListener'), 2),
        ('getlocal', 2), ('getlocal', 3), ('getproperty', P('mouseX')), ('getlocal', 3), ('getproperty', P('mouseY')), ('pushfalse',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', 2), ('getproperty', P('dmCode')), ('callpropvoid', P('dmBAct'), 1),
        ('L', 'end'), ('returnvoid',)]
    d.method('dmBUp', 1, code)
    d.method('dmBAct', 1, HEAD + act + [('returnvoid',)])


def arm(d, loc, code, kids=False):
    """lines: the object in local `loc` becomes a button with action code `code`"""
    P, S = d.P, d.S
    o = [('getlocal', loc), ('pushbyte', code), ('setproperty', P('dmCode'))]
    if not kids: o += [('getlocal', loc), ('pushfalse',), ('setproperty', P('mouseChildren'))]
    return o + [('getlocal', loc), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmBDown')), ('callpropvoid', P('addEventListener'), 2)]


# ---------------------------------------------------------------- hot bar
def _canvas(size):
    """a square RGBA canvas at 2x (ta.PX) with drawing helpers: (n, R, s, dx, dy, r, yy, over, band, seg, done)"""
    import numpy as np
    n = int(round(size / ta.PX)); R = n / 2.0; s = n / 236.0
    yy, xx = (np.mgrid[0:n, 0:n].astype(np.float32) + 0.5)
    dx, dy = xx - R, yy - R; r = np.hypot(dx, dy)
    img = np.zeros((n, n, 4), np.float32)   # premultiplied while drawing
    def over(rgb, a):
        a = np.clip(a, 0, 1)[..., None]; col = np.array(rgb, np.float32) / 255.0
        img[..., :3] = col * a + img[..., :3] * (1 - a); img[..., 3:] = a + img[..., 3:] * (1 - a)
    def band(r0, w): return np.clip(np.minimum(r0 - r, r - (r0 - w)) + 0.5, 0, 1)   # a ring of width w ending at radius r0 (1 px soft edges)
    def seg(p, q, w):   # a stroke from p to q (units of R about the centre), width w px, round caps
        px, py, qx, qy = p[0] * R, p[1] * R, q[0] * R, q[1] * R; vx, vy = qx - px, qy - py
        t = np.clip(((dx - px) * vx + (dy - py) * vy) / (vx * vx + vy * vy), 0, 1)
        return np.clip(w / 2.0 - np.hypot(dx - px - t * vx, dy - py - t * vy) + 0.5, 0, 1)
    def done():
        a = img[..., 3:4]
        rgb = np.divide(img[..., :3], a, out=np.zeros_like(img[..., :3]), where=a > 1e-6)
        return (np.concatenate([np.clip(rgb, 0, 1), np.clip(a, 0, 1)], -1) * 255 + 0.5).astype(np.uint8)
    return n, R, s, dx, dy, r, yy, over, band, seg, done


def aim_art(size, glyph):
    """CONFIRM (glyph None: the movie writes the word across it) / cancel ('cross'): our own drawing at 2x (ta.PX), straight RGBA, after the
    owner's iPad shot of 23.11 - a thin light metal ring (a bright outer line, a darker groove, a fainter inner line: brushed-silver look,
    lighter at the top) around a DARK translucent disc (black ~42..50 %), a faint dark halo outside (legible on snow / fog); cancel adds a
    white X over a soft shadow. size = stage px (the hit area: the whole square)."""
    import numpy as np
    n, R, s, dx, dy, r, yy, over, band, seg, done = _canvas(size)
    ring_o = R - 4 * s; lit = 1 - 0.25 * (yy / n)                    # top lighter than the bottom: a lit metal rim
    over((0, 0, 0), 0.28 * np.clip((R - 0.5 - r) / (4 * s), 0, 1) * np.clip((r - (ring_o - 6 * s)) / (4 * s), 0, 1))   # halo
    over((8, 10, 12), (0.42 + 0.08 * (yy / n)) * np.clip(ring_o - 1 - r + 0.5, 0, 1))                                 # dark glass
    over((236, 238, 240), 0.95 * lit * band(ring_o, 3.0 * max(s, 0.8)))                                               # the rim: outer line
    over((90, 94, 100), 0.80 * band(ring_o - 3.0 * max(s, 0.8), 2.0 * max(s, 0.75)))                                  # groove
    over((205, 208, 212), 0.70 * lit * band(ring_o - 5.0 * max(s, 0.8), 1.6 * max(s, 0.75)))                         # inner line
    if glyph == 'cross':
        m = np.zeros((n, n), np.float32)
        for p, q in [((-0.30, -0.30), (0.30, 0.30)), ((-0.30, 0.30), (0.30, -0.30))]: m = np.maximum(m, seg(p, q, 0.15 * R))
        sh = np.roll(np.roll(m, int(round(3 * s)) or 1, 0), int(round(1 * s)), 1)
        over((0, 0, 0), 0.35 * sh)
        over((255, 255, 255), 0.97 * m)
    return done()


def on_art(size):
    """the toggled-on mark of a round minimap button (plate_round_copper underneath): a bright warm ring just inside the plate's rim and a soft
    glow inside it - a SHAPE added, not only a colour (review rule 3); the glyph also goes from dimmed to full (dmMmState)"""
    import numpy as np
    n, R, s, dx, dy, r, yy, over, band, seg, done = _canvas(size)
    over((255, 196, 90), 0.30 * np.clip(1 - np.abs(r - R * 0.80) / (R * 0.14), 0, 1))     # glow
    over((255, 222, 140), 0.95 * band(R * 0.86, max(2.4, 4.5 * s)))                         # the ring
    return done()


def plate_kids(art, plate, icon, size, icon_name=None, on=None, depth=1):
    """children of a plate button: the plate scaled to `size`, the icon centred at GLYPH of it (a named sprite when icon_name: its alpha is
    the toggle's state), an optional named overlay sprite `on` (on_art) -> [(cid, depth, matrix, name)]"""
    pc, pb = art.png(plate); kids = [(pc, depth, (0, 0, size / (pb[2] - pb[0])), None)]
    ic, ib = art.png(icon); iw, ih = ib[2] - ib[0], ib[3] - ib[1]; ki = size * GLYPH / max(iw, ih)
    at = ((size - iw * ki) / 2.0, (size - ih * ki) / 2.0, ki)
    kids.append((art.clip(ic) if icon_name else ic, depth + 1, at, icon_name))
    if on is not None: kids.append((art.clip(on), depth + 2, (0, 0), 'on'))
    return kids


def _sprite_places(swf, cid):
    for c, t, _ in swf.tags:
        if c == 39 and struct.unpack_from('<H', t, 0)[0] == cid: return [gr.place_parse(sc, sd) for sc, sd in gr.subtags(t) if sc in (4, 26, 70)]
    raise KeyError('sprite %d' % cid)


def banner_kids(g, src, cid, width, k):
    """the Switch banner `cid` of `src` (3 parts: left / a stretchable middle / right, 64 high) copied into g, stretched to `width` x 64 k"""
    parts = _sprite_places(src, cid); assert len(parts) == 3, parts
    kids = []
    for n, p in enumerate(parts):
        m = gr.Bits(p['matrix'], 0).matrix() if p['matrix'] else (1, 0, 0, 1, 0, 0)
        a, dd, tx, ty = m[0], m[3], m[4] / 20.0, m[5] / 20.0
        c = g.copy(p['char'])
        if n == 0: mx = gr.matrix_bytes(tx * k, ty * k, a * k, dd * k)                       # left: its own (mirrored) matrix, scaled
        elif n == 1: mx = gr.matrix_bytes(84 * k, ty * k, (width - 166 * k) / 4.0, dd * k)    # middle: 4 px stretched over the width
        else: mx = gr.matrix_bytes(width - 84 * k, ty * k, a * k, dd * k)                     # right
        kids.append((c, 1 + 2 * n, mx, None))
    return kids


def hotbar_art(swf, notif=None):
    g = gr.Graft(swf, Swf(notif) if notif else None); art = ta.Art(g); placed = []
    # CONFIRM: the ring + the word across it (dmLbl, set by dmLang); cancel: the ring + X; the hint banner under them (dmTxt)
    cf, cb = ta.bitmap_shape(g, aim_art(CONFIRM[2], None)); cs = CONFIRM[2]
    lbl = ta.edit_text(g, (cs / 2.0 - 150, cs / 2.0 - LBL_FONT * 0.72, cs / 2.0 + 150, cs / 2.0 + LBL_FONT * 0.72), CONF_EN, height=LBL_FONT)
    g.place(g.sprite([(cf, 1, (0, 0), None), (lbl, 2, (0, 0), 'dmLbl')]), CONFIRM[0] - cs / 2.0, CONFIRM[1] - cs / 2.0, 'dmConfirm_mc'); placed.append(('dmConfirm_mc', CONFIRM))
    xf, xb = ta.bitmap_shape(g, aim_art(CANCEL[2], 'cross'), centre=True)
    g.place(g.sprite([(xf, 1, (0, 0), None)]), CANCEL[0], CANCEL[1], 'dmCancel_mc', 1.0); placed.append(('dmCancel_mc', CANCEL))
    hx, hy, hw, hk = HINT
    if notif:   # the player's own banner art (Switch notification.swf); without it (a dir without the movie) a plain dark plate
        kids = banner_kids(g, Swf(notif), HINT_SRC[1], hw, hk)
    else:
        kids = [(ta.poly_shape(g, [(0, 4), (hw, 4), (hw, 60 * hk), (0, 60 * hk)], (24, 20, 16))[0], 1, (0, 0), None)]
    txt = ta.edit_text(g, (48 * hk, 32 * hk - HINT_FONT * 0.72, hw - 48 * hk, 32 * hk + HINT_FONT * 0.72), HINT_EN, height=HINT_FONT)
    g.place(g.sprite(kids + [(txt, 9, (0, 0), 'dmTxt')]), hx - hw / 2.0, hy, 'dmHint_mc'); placed.append(('dmHint_mc', HINT))
    # chat / combat log: our square copper plate (the right column's family) + our glyph, the iPad's 84 boxes on the row's centre line
    for name, box, icon in (('dmChat_mc', CHAT_BOX, 'icon_chat'), ('dmLog_mc', LOG_BOX, 'icon_combatlog')):
        g.place(g.sprite(plate_kids(art, 'plate_square_copper', icon, box[2])), box[0], box[1], name); placed.append((name, box))
    # the deck list: our plates (assets/touchui) + pips / bars drawn in code
    sq, qb = art.png('plate_square_copper'); qw = qb[2] - qb[0]; ink = (236, 226, 200)
    def rect(x0, y0, w, h, rgb=ink): return ta.poly_shape(g, [(x0, y0), (x0 + w, y0), (x0 + w, y0 + h), (x0, y0 + h)], rgb)[0]
    def pip(cx, cy, r=5): return ta.poly_shape(g, [(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)], ink)[0]
    gx, gy, gw, gh = DECK_BG
    g.place(g.sprite([(rect(-2, -2, gw + 4, gh + 4, (122, 94, 58)), 1, (0, 0), None), (rect(0, 0, gw, gh, (30, 24, 18)), 2, (0, 0), None)]), gx, gy, 'dmDeckBg_mc')
    placed.append(('dmDeckBg_mc', DECK_BG))
    lx, ly, ls, lp = DECK_LIST
    for k in range(1, 6):
        xs = [ls / 2.0 + (i - (k - 1) / 2.0) * 9 for i in range(k)]
        g.place(g.sprite([(sq, 1, (0, 0, ls / qw), None)] + [(pip(xv, ls / 2.0, 3.5), 2 + i, (0, 0), None) for i, xv in enumerate(xs)]), lx + lp * (k - 1), ly, 'dmDeck%d_mc' % k)
    ix, iy, iw, ih, ip = IND
    for k in range(1, 6): g.place(g.sprite([(rect(0, 0, iw, ih), 1, (0, 0), None)]), ix + ip * (k - 1), iy, 'dmInd%d_mc' % k)
    placed.append(('dmDeck1..5_mc', DECK_LIST)); placed.append(('dmInd1..5_mc', IND))
    g.finish()
    return placed, len(g.new_defs)


def hotbar(data, notif=None):
    notif = notif or SRC_EXTRA.get(HINT_SRC[0])
    swf = Swf(data); placed, ndefs = hotbar_art(swf, notif); d = Doc(swf); P, S, abc = d.P, d.S, d.abc
    def aimvis(first, lab):   # lines: push Boolean(first || this.dmAimH) - CONFIRM / cancel shown for the movie's own mark or the host's
        return first + [('convert_b',), ('dup',), ('iftrue', lab), ('pop',)] + d.get('dmAimH') + [('convert_b',), ('L', lab)]
    Rect = abc.pub('Rectangle', 'flash.geom')
    def setn(loc, prop, val): return [('getlocal', loc)] + val + [('setproperty', P(prop))]
    # dmLay(): locals 1 hotbar_mc, 2 a child
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('pushnull',), ('coerce_a',), ('setlocal', 2)]
    code += d.get('hotbar_mc') + [('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'end')]
    code += setn(1, 'scaleX', frac(HB_S)) + setn(1, 'scaleY', frac(HB_S))
    code += [('getlocal', 1), ('findpropstrict', Rect)] + num(CROP) + num(0) + num(VISW) + num(VISH) + [('constructprop', Rect, 4), ('setproperty', P('scrollRect'))]
    code += setn(1, 'x', num(HB_X)) + setn(1, 'y', num(HB_Y))
    # the PC chat tab / combat log tab are replaced by our plates dmChat_mc / dmLog_mc (they call the same ExternalInterface names)
    code += [('getlocal', 1), ('getproperty', P('chatBtn_mc')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'nc'), ('getlocal', 2), ('pushfalse',), ('setproperty', P('visible')), ('L', 'nc')]
    code += [('getlocal', 1), ('getproperty', P('actionsButton_mc')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'nab')] + setn(2, 'x', num(ACT_BTN[0])) + setn(2, 'y', num(ACT_BTN[1])) + [('L', 'nab')]
    code += [('getlocal', 1), ('getproperty', P('expBar_mc')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'ne')] + setn(2, 'scaleX', frac(EXP_SX)) + [('L', 'ne')]
    code += d.get('showLog_mc') + [('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'nl'), ('getlocal', 2), ('pushfalse',), ('setproperty', P('visible')), ('L', 'nl')]
    code += d.get('endPiece_mc') + [('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'np'), ('getlocal', 2), ('pushfalse',), ('setproperty', P('visible')), ('L', 'np')]
    code += d.get('actionSkillHolder_mc') + [('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'na')]
    code += setn(2, 'scaleX', frac(HB_S)) + setn(2, 'scaleY', frac(HB_S)) + setn(2, 'x', num(ACT_X)) + setn(2, 'y', num(ACT_Y)) + [('L', 'na')]
    code += d.ei('updateSlots', num(SLOTS)) + [('L', 'end'), ('returnvoid',)]
    d.method('dmLay', 0, code)
    # the swipe over the slots: dmSwDown(e) / dmSwUp(e); locals 2 stage, 3 dx. Phone run 23.11 (owner: "a long press on a hot bar icon also
    # flips the skill deck"): a still hold's lift is sent with the cursor parked off the screen (the host's no-click release, window -64,-64), and
    # dmSwUp took that jump for a swipe (nextHotbar after every short long press). The travel is now the finger's: dmSwL = the cursor's x as
    # dmTick sampled it on the frames while the press was down (SWIPE_TRACK), not the x at the release; the host also keeps x at the release
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2)] + d.get('stage') + [('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', 2), ('getproperty', P('mouseX')), ('setproperty', P('dmSwX')),
        ('getlocal0',), ('getlocal', 2), ('getproperty', P('mouseX')), ('setproperty', P('dmSwL')), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmSwOn')),
        ('getlocal', 2), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmSwUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)]
    d.method('dmSwDown', 1, code)
    def cycle(prev): return d.get('hotbar_mc', 'cycleHotBar_mc') + [('pushtrue',) if prev else ('pushfalse',), ('newarray', 1), ('callpropvoid', P('cycleHotBar'), 1)]
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushbyte', 0), ('convert_d',), ('setlocal', 3)] + d.get('stage') + [('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmSwUp')), ('callpropvoid', P('removeEventListener'), 2),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmSwOn'))] + d.get('dmSwL') + d.get('dmSwX') + [('subtract',), ('convert_d',), ('setlocal', 3)] + d.get('isDragging') + [('iftrue', 'end'),
        ('getlocal', 3)] + num(SWIPE) + [('ifngt', 'left')] + cycle(True) + [('returnvoid',),
        ('L', 'left'), ('getlocal', 3)] + num(-SWIPE) + [('ifnlt', 'end')] + cycle(False) + [('L', 'end'), ('returnvoid',)]
    d.method('dmSwUp', 1, code)
    # dmBAct(code): 30 = the deck list open / closed, 31..35 = that deck, 1 / 0 = CONFIRM (also the action plate) / cancel. locals 2 steps, 3 mc
    idx = d.get('hotbar_mc', 'cycleHotBar_mc', 'currentHotBarIndex')
    snd = d.ei('PlaySound', [('pushstring', S('UI_Generic_Click'))])   # the PC buttons' own click sound
    act = [('pushbyte', 0), ('convert_i',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
           ('getlocal1',), ('pushbyte', CHAT_CODE), ('ifne', 'nchat')] + snd + d.ei('ToggleChatLog') + [('returnvoid',), ('L', 'nchat'),
           ('getlocal1',), ('pushbyte', LOG_CODE), ('ifne', 'nlog')] + snd + d.ei('CombatLogBtnPressed') + [('returnvoid',), ('L', 'nlog'),
           ('getlocal1',), ('pushbyte', 30), ('ifne', 'pick'),
           ('getlocal0',)] + d.get('dmDeckOpen') + [('not',), ('setproperty', P('dmDeckOpen')), ('getlocal0',), ('callpropvoid', P('dmDeckShow'), 0), ('returnvoid',),
           ('L', 'pick'), ('getlocal1',), ('pushbyte', 30), ('ifngt', 'aim'),
           ('getlocal1',), ('pushbyte', 30), ('subtract',)] + idx + [('subtract',), ('pushbyte', 5), ('add',), ('pushbyte', 5), ('modulo',), ('convert_i',), ('setlocal', 2)]
    for k in range(1, 5): act += [('getlocal', 2), ('pushbyte', k), ('iflt', 'pd')] + cycle(False)
    act += [('L', 'pd'), ('getlocal0',), ('pushfalse',), ('setproperty', P('dmDeckOpen')), ('getlocal0',), ('callpropvoid', P('dmDeckShow'), 0), ('returnvoid',),
            ('L', 'aim')] + d.get('dmPlate') + [('iffalse', 'send'), ('getlocal1',), ('pushbyte', 1), ('ifne', 'send'),
            ('getlocal0',), ('pushfalse',), ('setproperty', P('dmPlate'))] + d.child('dmConfirm_mc', 3) + [('getlocal', 3), ('iffalse', 'send'),
            ('getlocal', 3)] + aimvis(d.get('dmAimV'), 'bv') + [('setproperty', P('visible')),
            ('L', 'send')] + d.ei('dmAimBtn', [('getlocal1',)])
    press_code(d, act)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1)]
    for k in range(1, 6):
        code += d.child('dmDeck%d_mc' % k, 1) + [('getlocal', 1), ('iffalse', 's%d' % k), ('getlocal', 1)] + d.get('dmDeckOpen') + [('convert_b',), ('setproperty', P('visible')), ('L', 's%d' % k)]
    code += d.child('dmDeckBg_mc', 1) + [('getlocal', 1), ('iffalse', 'sbg'), ('getlocal', 1)] + d.get('dmDeckOpen') + [('convert_b',), ('setproperty', P('visible')), ('L', 'sbg')]
    code += d.get('hotbar_mc', 'lockButton_mc') + [('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'slk'), ('getlocal', 1)] + d.get('dmDeckOpen') + [
        ('convert_b',), ('setproperty', P('visible')), ('L', 'slk')]
    d.method('dmDeckShow', 0, code + [('returnvoid',)])
    # dmTick(e): the indicator follows the PC's current deck. locals 2 current, 3 mc
    code = HEAD + [('pushbyte', 0), ('convert_i',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3), ('pushnull',), ('coerce_a',), ('setlocal', 4)]
    # SWIPE_TRACK: while a press on the slots is down, the cursor's x of this frame is the swipe's end (dmSwUp; the release's own x may be the
    # host's off-screen park)
    code += d.get('dmSwOn') + [('iffalse', 'nsw')] + d.get('stage') + [('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'nsw'),
        ('getlocal0',), ('getlocal', 3), ('getproperty', P('mouseX')), ('setproperty', P('dmSwL')), ('pushnull',), ('coerce_a',), ('setlocal', 3), ('L', 'nsw')]
    # phone 23.11: the actions list (attack / weapon / sneak) covered its own button and could not be closed - the PC's
    # actionSkillHolder_31.positionElements puts its foot at originalPosY + hotbar_mc.y - (bottom_mc.y + height) in its OWN px, so at our scale
    # x1.56 the list hung ~100 px lower, over the bar. Every frame its foot is put back on the bar's top: y = ACT_FOOT - (bottom.y + h) x scaleY
    code += d.get('actionSkillHolder_mc') + [('coerce_a',), ('setlocal', 4), ('getlocal', 4), ('iffalse', 'af'), ('getlocal', 4)] + num(ACT_FOOT) + [
        ('getlocal', 4), ('getproperty', P('bottom_mc')), ('getproperty', P('y')), ('getlocal', 4), ('getproperty', P('bottom_mc')), ('getproperty', P('height')), ('add',),
        ('getlocal', 4), ('getproperty', P('scaleY')), ('multiply',), ('subtract',), ('setproperty', P('y')), ('L', 'af')]
    code += d.get('hotbar_mc') + [('iffalse', 'end')] + d.get(
        'hotbar_mc', 'cycleHotBar_mc') + [('iffalse', 'end')] + idx + [('convert_i',), ('setlocal', 2), ('getlocal', 2)] + d.get('dmIndCur') + [('ifstricteq', 'end'),
        ('getlocal0',), ('getlocal', 2), ('setproperty', P('dmIndCur'))]
    for k in range(1, 6):
        code += d.child('dmInd%d_mc' % k, 3) + [('getlocal', 3), ('iffalse', 'i%d' % k), ('getlocal', 3), ('pushbyte', 3), ('pushbyte', 10), ('divide',),
            ('getlocal', 2), ('pushbyte', k), ('ifne', 'j%d' % k), ('pop',), ('pushbyte', 1), ('L', 'j%d' % k), ('setproperty', P('alpha')), ('L', 'i%d' % k)]
    d.method('dmTick', 1, code + [('L', 'end'), ('returnvoid',)])
    # dmHook(): once. locals 1 object, 2 graphics
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
                   ('pushnull',), ('coerce_a',), ('setlocal', 4)] + d.get('dmOn') + [('iftrue', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmOn'))]
    code += d.get('hotbar_mc', 'slotholder_mc') + [('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'ns'),
        ('getlocal', 1), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmSwDown')), ('callpropvoid', P('addEventListener'), 2), ('L', 'ns')]
    # the iPad's deck button (DECK_PAD): the PC number cycleHotBar_mc, arrows hidden, a plate under the number = its hit area, a tap = 30
    # (the list). Bounds are read before the arrows go (they are the number's column). locals 1 the number, 2 graphics, 3 bounds, 4 the lock
    def bx(f): return [('getlocal', 3), ('getproperty', P(f))]
    code += d.get('hotbar_mc', 'cycleHotBar_mc') + [('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'nb'),
        ('getlocal', 1), ('getlocal', 1), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 3),
        # owner 23.11: "the 1 styled as the actions button on the right": the deck button's backing = the PC actions button's own art
        # (hotBar_fla.actionButtonBG_28, 34 x 52, the same symbol and size as the actionsButton_mc beside the slots), behind the number,
        # centred on the number's column, vertically on its bounds (the row's height)
        ('findpropstrict', DECK_BG_CLS(d)), ('constructprop', DECK_BG_CLS(d), 0), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('pushstring', S('dmDeckArt')), ('setproperty', P('name')),
        ('getlocal', 2)] + bx('x') + num(DECK_X + (DECK_W - DECK_ART[0]) // 2) + [('add',), ('setproperty', P('x')),
        ('getlocal', 2)] + bx('y') + bx('height') + num(DECK_ART[1]) + [('subtract',), ('pushbyte', 2), ('divide',), ('add',), ('setproperty', P('y')),
        ('getlocal', 1), ('getlocal', 2), ('pushbyte', 0), ('callpropvoid', P('addChildAt'), 2),
        # the number centred on the plate: the field spans it, align center (defaultTextFormat too: the PC code sets the text again per deck)
        ('getlocal', 1), ('getproperty', P('text_txt')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'ntx'),
        ('getlocal', 2), ('pushstring', S('none')), ('setproperty', P('autoSize')),
        ('getlocal', 2)] + bx('x') + num(DECK_X) + [('add',), ('setproperty', P('x')),
        ('getlocal', 2)] + num(DECK_W) + [('setproperty', P('width')),
        ('getlocal', 2), ('getlocal', 2), ('getproperty', P('y'))] + num(DECK_TY) + [('add',), ('setproperty', P('y')),
        ('getlocal', 2), ('callproperty', P('getTextFormat'), 0), ('coerce_a',), ('setlocal', 4),
        ('getlocal', 4), ('pushstring', S('center')), ('setproperty', P('align')),
        ('getlocal', 2), ('getlocal', 4), ('setproperty', P('defaultTextFormat')),
        ('getlocal', 2), ('getlocal', 4), ('callpropvoid', P('setTextFormat'), 1), ('L', 'ntx')]
    for btn in ('upBtn_mc', 'downBtn_mc'):
        code += [('getlocal', 1), ('getproperty', P(btn)), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'nv' + btn),
                 ('getlocal', 2), ('pushfalse',), ('setproperty', P('visible')), ('L', 'nv' + btn)]
    code += arm(d, 1, 30) + [('L', 'nb')]
    # the PC lock into the list (the iPad's lock sits in deckSwitcher_mc): re-parented to the root (its slot hotbar_mc.lockButton_mc and its own
    # listeners stay), scaled by its height into DECK_LOCK and centred there, a transparent hit area of the whole box; shown with the list
    lx, ly, lsz = DECK_LOCK
    def cx(f, half): return bx(f) + bx(half) + [('pushbyte', 2), ('divide',), ('add',)]   # the bounds' centre on axis f
    code += d.get('hotbar_mc', 'lockButton_mc') + [('coerce_a',), ('setlocal', 4), ('getlocal', 4), ('iffalse', 'nl'),
        ('getlocal', 4), ('getlocal', 4), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 3),
        ('getlocal0',), ('getlocal', 4), ('callpropvoid', P('addChild'), 1),
        ('getlocal', 4)] + num(lsz - 4) + bx('height') + [('divide',), ('setproperty', P('scaleX')),
        ('getlocal', 4), ('getlocal', 4), ('getproperty', P('scaleX')), ('setproperty', P('scaleY')),
        ('getlocal', 4)] + num(lx + lsz // 2) + cx('x', 'width') + [('getlocal', 4), ('getproperty', P('scaleX')), ('multiply',), ('subtract',), ('setproperty', P('x')),
        ('getlocal', 4)] + num(ly + lsz // 2) + cx('y', 'height') + [('getlocal', 4), ('getproperty', P('scaleY')), ('multiply',), ('subtract',), ('setproperty', P('y')),
        ('getlocal', 4), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', P('beginFill'), 2),
        ('getlocal', 2)] + cx('x', 'width') + num(lsz // 2) + [('getlocal', 4), ('getproperty', P('scaleX')), ('divide',), ('subtract',)] + \
        cx('y', 'height') + num(lsz // 2) + [('getlocal', 4), ('getproperty', P('scaleY')), ('divide',), ('subtract',)] + \
        num(lsz) + [('getlocal', 4), ('getproperty', P('scaleX')), ('divide',)] + num(lsz) + [('getlocal', 4), ('getproperty', P('scaleY')), ('divide',),
        ('callpropvoid', P('drawRect'), 4), ('getlocal', 2), ('callpropvoid', P('endFill'), 0),
        ('getlocal', 4), ('pushfalse',), ('setproperty', P('visible')), ('L', 'nl')]
    for k, (name, c) in enumerate((('dmConfirm_mc', 1), ('dmCancel_mc', 0))):
        code += d.child(name, 1) + [('getlocal', 1), ('iffalse', 'b%d' % k)] + arm(d, 1, c) + [('getlocal', 1), ('pushfalse',), ('setproperty', P('visible')), ('L', 'b%d' % k)]
    for k, (name, c) in enumerate((('dmChat_mc', CHAT_CODE), ('dmLog_mc', LOG_CODE))):
        code += d.child(name, 1) + [('getlocal', 1), ('iffalse', 'pl%d' % k)] + arm(d, 1, c) + [('L', 'pl%d' % k)]
    code += d.child('dmHint_mc', 1) + [('getlocal', 1), ('iffalse', 'nh'), ('getlocal', 1), ('pushfalse',), ('setproperty', P('visible')),
             ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseEnabled')), ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseChildren')), ('L', 'nh')]
    code += [('getlocal0',), ('callpropvoid', P('dmSlotsCut'), 0), ('getlocal0',), ('callpropvoid', P('dmLblFx'), 0), ('getlocal0',), ('callpropvoid', P('dmPads'), 0)]
    code += d.get('hotbar_mc', 'hotkeys_mc') + [('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'nk'), ('getlocal', 1), ('pushfalse',), ('setproperty', P('visible')), ('L', 'nk')]
    code += d.child('dmDeckBg_mc', 1) + [('getlocal', 1), ('iffalse', 'nd'), ('getlocal', 1), ('pushfalse',), ('setproperty', P('visible')),
             ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseChildren')), ('L', 'nd')]
    for k in range(1, 6):
        code += d.child('dmDeck%d_mc' % k, 1) + [('getlocal', 1), ('iffalse', 'dk%d' % k)] + arm(d, 1, 30 + k) + [('getlocal', 1), ('pushfalse',), ('setproperty', P('visible')), ('L', 'dk%d' % k)]
        code += d.child('dmInd%d_mc' % k, 1) + [('getlocal', 1), ('iffalse', 'ik%d' % k), ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseEnabled')),
                                                    ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseChildren')), ('L', 'ik%d' % k)]
    code += [('getlocal0',), ('pushfalse',), ('setproperty', P('dmDeckOpen')),
             ('getlocal0',), ('pushstring', S('enterFrame')), ('getlocal0',), ('getproperty', P('dmTick')), ('callpropvoid', P('addEventListener'), 2)]
    code += [('L', 'end'), ('returnvoid',)]
    d.method('dmHook', 0, code)
    # Python-subset methods (tools/dos2_touchkit.py kc_compile)
    d.kc('''
def dmSlotsCut():
    c = this.hotbar_mc.slotholder_mc.slotContainer_mc
    if not c:
        return
    i = %d
    while i < 40:
        o = c.getChildByName('slot' + String(i))
        if o:
            o.visible = False
        i = i + 1
def dmPads():
    for_chat = this.getChildByName('dmChat_mc')
    if for_chat:
        g = for_chat.graphics
        g.beginFill(0, 0)
        g.drawRect(%d, %d, %d, %d)
        g.endFill()
    for_log = this.getChildByName('dmLog_mc')
    if for_log:
        g = for_log.graphics
        g.beginFill(0, 0)
        g.drawRect(%d, %d, %d, %d)
        g.endFill()
def dmLblFx():
    m = this.getChildByName('dmConfirm_mc')
    if m:
        f = m.getChildByName('dmLbl')
        if f:
            f.filters = textEffect.createStrokeFilter(0, 1.2, 0.8, 1, 3)
    m = this.getChildByName('dmHint_mc')
    if m:
        f = m.getChildByName('dmTxt')
        if f:
            f.filters = textEffect.createStrokeFilter(0, 1.2, 0.8, 1, 3)
def dmLang(t):
    s = String(t)
    ru = False
    i = 0
    k = s.length
    while i < k:
        c = s.charCodeAt(i)
        if c >= 1024 and c <= 1279:
            ru = True
            i = k
        i = i + 1
    if not ru:
        return
    if this.dmRu == True:
        return
    this.dmRu = True
    m = this.getChildByName('dmConfirm_mc')
    if m:
        f = m.getChildByName('dmLbl')
        if f:
            f.text = CONF_RU
    m = this.getChildByName('dmHint_mc')
    if m:
        f = m.getChildByName('dmTxt')
        if f:
            f.text = HINT_RU
def dmVis(on):
    if not on:
        this.dmDeckOpen = False
    this.dmDeckShow()
    h = this.hotbar_mc.hotkeys_mc
    if h:
        h.visible = False
    h = this.showLog_mc
    if h:
        h.visible = False
    h = this.endPiece_mc
    if h:
        h.visible = False
    m = this.getChildByName('dmChat_mc')
    if m:
        m.visible = on == True
    m = this.getChildByName('dmLog_mc')
    if m:
        m.visible = on == True
''' % ((SLOTS,) + ((PLATE - PLATE_PAD) // 2, (PLATE - PLATE_PAD) // 2, PLATE_PAD, PLATE_PAD) * 2), {'textEffect': ('lex', d.lex('textEffect')), 'CONF_RU': ('const', CONF_RU), 'HINT_RU': ('const', HINT_RU)})
    # phone 23.11 ("a padlock hangs over the skill bar and never goes"): the engine's showSkillBar(true) runs the PC bottombar.showSkillBar,
    # which sets .visible = true on every element of its skillBar list - lockButton_mc (moved into the deck list) and hotkeys_mc among them.
    # After the original our visibility is put back: the lock only with the open list, no slot numbers, the plates follow the bar.
    # Phone 23.11 (combat shot): MainTimeline.showSkillBar also sets showLog_mc.visible / endPiece_mc.visible = on - the PC combat log tab
    # (crossed swords, 1876,874: "a small button right of our log plate") came back at its PC place; dmLog_mc covers it: hidden again
    d.wrap('showSkillBar', 'dmSsbOrig', lambda n: HEAD + d.call_orig('dmSsbOrig', n) + [('getlocal0',), ('getlocal1',), ('callpropvoid', P('dmVis'), 1), ('returnvoid',)])
    # the language of our words: a Cyrillic letter in the tooltips the engine gives (setFixedBtnTooltips: chat / log / deck arrows; the lock's)
    d.wrap('setFixedBtnTooltips', 'dmFbtOrig', lambda n: HEAD + d.call_orig('dmFbtOrig', n) + [('getlocal0',), ('getlocal2',), ('callpropvoid', P('dmLang'), 1), ('returnvoid',)])
    d.wrap('setLockBtnTooltips', 'dmLbtOrig', lambda n: HEAD + d.call_orig('dmLbtOrig', n) + [('getlocal0',), ('getlocal1',), ('callpropvoid', P('dmLang'), 1), ('returnvoid',)])
    d.wrap('onEventInit', 'dmInitOrig', lambda n: HEAD + d.call_orig('dmInitOrig', n) + [('getlocal0',), ('callpropvoid', P('dmLay'), 0), ('getlocal0',), ('callpropvoid', P('dmHook'), 0), ('returnvoid',)])
    d.wrap('onEventResolution', 'dmResOrig', lambda n: HEAD + [('getlocal0',), ('callpropvoid', P('dmLay'), 0), ('returnvoid',)])
    # showActiveSkill(slot): the original; CONFIRM / cancel follow "a skill is prepared"; the host hears every change. locals: n+1 shown, n+2 mc
    def sas(n):
        a, m = n + 1, n + 2
        v = n + 3
        c = HEAD + [('pushfalse',), ('setlocal', a), ('pushnull',), ('coerce_a',), ('setlocal', m), ('pushbyte', 0), ('convert_i',), ('setlocal', v)]
        # the host's AIM_SHOW / AIM_HIDE (a prepared state the movie does not see): dmAimH, CONFIRM back at its own place
        c += [('getlocal1',), ('pushint', d.abc_int(AIM_HIDE)), ('ifnge', 'nh'), ('getlocal1',), ('pushint', d.abc_int(PLATE_HIDE)), ('ifnlt', 'nh'),
              ('getlocal0',), ('getlocal1',), ('pushint', d.abc_int(AIM_SHOW)), ('equals',), ('setproperty', P('dmAimH')),
              ('getlocal0',), ('pushfalse',), ('setproperty', P('dmPlate'))]
        for k, name in enumerate(('dmConfirm_mc', 'dmCancel_mc', 'dmHint_mc')):
            c += d.child(name, m) + [('getlocal', m), ('iffalse', 'h%d' % k), ('getlocal', m)] + aimvis(d.get('dmAimV'), 'hv%d' % k) + [('setproperty', P('visible'))]
            if not k: c += [('getlocal', m)] + num(CONFIRM[0] - CONFIRM[2] // 2) + [('setproperty', P('x')), ('getlocal', m)] + num(CONFIRM[1] - CONFIRM[2] // 2) + [('setproperty', P('y'))]
            c += [('L', 'h%d' % k)]
        c += [('returnvoid',), ('L', 'nh')]
        # the host's action plate (never a slot number): PLATE_HIDE, or PLATE_BASE + stageX * PLATE_MUL + stageY
        c += [('getlocal1',), ('pushint', d.abc_int(PLATE_HIDE)), ('ifnge', 'slot')] + d.child('dmConfirm_mc', m) + [('getlocal', m), ('iffalse', 'pe'),
              ('getlocal1',), ('pushint', d.abc_int(PLATE_HIDE)), ('ifne', 'pshow'),
              ('getlocal0',), ('pushfalse',), ('setproperty', P('dmPlate')), ('getlocal', m)] + aimvis(d.get('dmAimV'), 'pv') + [('setproperty', P('visible')), ('returnvoid',),
              ('L', 'pshow'), ('getlocal1',), ('pushint', d.abc_int(PLATE_BASE)), ('subtract',), ('convert_i',), ('setlocal', v),
              ('getlocal', m), ('getlocal', v)] + num(PLATE_MUL) + [('divide',), ('convert_i',)] + num(CONFIRM[2] // 2) + [('subtract',), ('setproperty', P('x')),
              ('getlocal', m), ('getlocal', v)] + num(PLATE_MUL) + [('modulo',)] + num(CONFIRM[2] // 2 + PLATE_UP) + [('subtract',), ('setproperty', P('y')),
              ('getlocal', m), ('pushtrue',), ('setproperty', P('visible')), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmPlate')),
              ('L', 'pe'), ('returnvoid',), ('L', 'slot')]
        c += d.call_orig('dmSasOrig', n) + [('getlocal1',), ('pushbyte', 0), ('greaterequals',), ('setlocal', a), ('getlocal0',), ('pushfalse',), ('setproperty', P('dmPlate'))]
        for k, name in enumerate(('dmConfirm_mc', 'dmCancel_mc', 'dmHint_mc')):
            c += d.child(name, m) + [('getlocal', m), ('iffalse', 'v%d' % k), ('getlocal', m)] + aimvis([('getlocal', a)], 'sv%d' % k) + [('setproperty', P('visible'))]
            if not k: c += [('getlocal', m)] + num(CONFIRM[0] - CONFIRM[2] // 2) + [('setproperty', P('x')), ('getlocal', m)] + num(CONFIRM[1] - CONFIRM[2] // 2) + [('setproperty', P('y'))]
            c += [('L', 'v%d' % k)]
        c += d.get('dmAimV') + [('getlocal', a), ('ifstricteq', 'end'), ('getlocal0',), ('getlocal', a), ('setproperty', P('dmAimV')),
            ('getlocal', a), ('iffalse', 'off')] + d.ei('dmAim', num(1)) + [('returnvoid',), ('L', 'off')] + d.ei('dmAim', num(0)) + [('L', 'end'), ('returnvoid',)]
        return c
    d.wrap('showActiveSkill', 'dmSasOrig', sas)
    # setActionPreview(id, on): the original; "ActionAttackGround" on / off -> dmAim(2 | 3) (the host aims taps while it is on)
    d.wrap('setActionPreview', 'dmSapOrig', lambda n: HEAD + d.call_orig('dmSapOrig', n) + [('getlocal1',), ('pushstring', S('ActionAttackGround')), ('ifne', 'end'),
        ('getlocal2',), ('iffalse', 'off')] + d.ei('dmAim', num(2)) + [('returnvoid',), ('L', 'off')] + d.ei('dmAim', num(3)) + [('L', 'end'), ('returnvoid',)])
    # phone run 21.11: no PC keyboard labels (the iPad's hot bar has none) - the slot numbers 1..9 0 - = (hotbar_mc.hotkeys_mc, filled by the
    # engine through setText / setAllText) are hidden in dmHook and get no text, the buttons' letters (M L X: setButton(id, text, tooltip) ->
    # hotBarButton.btn_txt) get an empty text; the tooltips stay
    d.wrap('setText', 'dmSetTextOrig', lambda n: HEAD + [('returnvoid',)])
    d.wrap('setAllText', 'dmSetAllTextOrig', lambda n: HEAD + [('returnvoid',)])
    d.wrap('setButton', 'dmSetButtonOrig', lambda n: HEAD + [('getlocal0',), ('getlocal1',), ('pushstring', S('')), ('getlocal3',), ('callpropvoid', P('dmSetButtonOrig'), 3), ('returnvoid',)])
    d.done()
    return swf.write(), dict(placed=placed, defs=ndefs, methods=d.made)


# ---------------------------------------------------------------- minimap, status console
def moved(data, moves, tap=None, btns=(), pads=(), art=None, act_pre=(), init_pre=(), init_post=(), setup=None):
    """onEventInit: the original, then named root instances moved by (dx, dy); tap = (instance, dmSide code) for a press-and-release button;
    btns = MM_BTNS-like grafted buttons (code 10 + n = ExternalInterface dmKey(n)); art(swf) -> (placed, new definitions) grafts them;
    act_pre = dmBAct lines run first (local 1 = the code); init_pre / init_post = onEventInit lines (method calls) before the moves / after
    the pads; setup(d) adds methods / wraps"""
    swf = Swf(data); nd = 0; placed = [(b[0], b[3:]) for b in btns]
    if art: placed, nd = art(swf)
    d = Doc(swf); P = d.P
    act_pre, init_pre, init_post = [x(d) if callable(x) else list(x) for x in (act_pre, init_pre, init_post)]
    # the ring: not in ping mode (there the PC code's own press sends minimapPing and leaves the mode; our listener was added first, so dmNote = the mode at the press)
    if tap: press_code(d, list(act_pre) + [('getlocal1',), ('pushbyte', 10), ('ifngt', 'side')] + d.ei('dmKey', [('getlocal1',), ('pushbyte', 10), ('subtract',)]) + d.ei(
        'PlaySound', [('pushstring', d.S('UI_Generic_Click'))]) + [('returnvoid',), ('L', 'side')] + d.get('dmNote') + [('iftrue', 'ping')] + d.ei('dmSide', [('getlocal1',)]) + [('L', 'ping')],
        dim=False, note='pingModeEnabled')
    def init(n):
        c = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', n + 1)] + d.call_orig('dmInitOrig', n) + list(init_pre)
        for k, (name, dx, dy) in enumerate(moves):
            c += d.get(name) + [('coerce_a',), ('setlocal', n + 1), ('getlocal', n + 1), ('iffalse', 'm%d' % k)]
            for prop, v in (('x', dx), ('y', dy)):
                if v: c += [('getlocal', n + 1), ('getlocal', n + 1), ('getproperty', P(prop))] + num(v) + [('add',), ('setproperty', P(prop))]
            c += [('L', 'm%d' % k)]
        if tap: c += d.get(tap[0]) + [('coerce_a',), ('setlocal', n + 1), ('getlocal', n + 1), ('iffalse', 'nt')] + arm(d, n + 1, tap[1], kids=True) + [('L', 'nt')]
        for k, b in enumerate(btns): c += d.child(b[0], n + 1) + [('getlocal', n + 1), ('iffalse', 'bt%d' % k)] + arm(d, n + 1, b[1]) + [('L', 'bt%d' % k)]
        for k, (name, size) in enumerate(pads):   # a transparent square of `size` px centred on the button's drawing (its own graphics: hit only)
            o, g, bb = n + 1, n + 2, n + 3
            c += [('pushnull',), ('coerce_a',), ('setlocal', g), ('pushnull',), ('coerce_a',), ('setlocal', bb)] + d.child(name, o) + [('getlocal', o), ('iffalse', 'pd%d' % k),
                  ('getlocal', o), ('getlocal', o), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', bb),
                  ('getlocal', o), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', g), ('getlocal', g), ('iffalse', 'pd%d' % k),
                  ('getlocal', g), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', P('beginFill'), 2), ('getlocal', g)]
            for pos, sz in (('x', 'width'), ('y', 'height')):
                c += [('getlocal', bb), ('getproperty', P(pos)), ('getlocal', bb), ('getproperty', P(sz)), ('pushbyte', 2), ('divide',), ('add',)] + num(size // 2) + [('subtract',)]
            c += num(size) + num(size) + [('callpropvoid', P('drawRect'), 4), ('getlocal', g), ('callpropvoid', P('endFill'), 0), ('L', 'pd%d' % k)]
        return c + list(init_post) + [('returnvoid',)]
    assert d.abc.trait(d.ci, 'dmHook') is None
    d.wrap('onEventInit', 'dmInitOrig', init)
    if setup: setup(d)
    d.inst['traits'].append(Trait(P('dmHook'), 1, 0, d.add('dmHook', 0, HEAD + [('returnvoid',)])))   # the "already patched" mark
    d.done()
    return swf.write(), dict(placed=placed, defs=nd, methods=d.made)


def closer(data, path):
    """onEventInit: the original, then the close button at `path` gets a transparent CLOSE_HIT square centred on its 23 px drawing"""
    swf = Swf(data); d = Doc(swf); P = d.P
    def init(n):
        o, g, b = n + 1, n + 2, n + 3
        c = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', o), ('pushnull',), ('coerce_a',), ('setlocal', g), ('pushnull',), ('coerce_a',), ('setlocal', b)] + d.call_orig('dmInitOrig', n)
        c += [('getlocal0',), ('coerce_a',), ('setlocal', o)]
        for name in path: c += [('getlocal', o), ('getproperty', P(name)), ('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', 'end')]
        # bounds of the drawing in the button's own space, then the square around their centre
        c += [('getlocal', o), ('getlocal', o), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', b),
              ('getlocal', o), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', g), ('getlocal', g), ('iffalse', 'end'),
              ('getlocal', g), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', P('beginFill'), 2), ('getlocal', g)]
        for pos, size in (('x', 'width'), ('y', 'height')):
            c += [('getlocal', b), ('getproperty', P(pos)), ('getlocal', b), ('getproperty', P(size)), ('pushbyte', 2), ('divide',), ('add',)] + num(CLOSE_HIT // 2) + [('subtract',)]
        c += num(CLOSE_HIT) + num(CLOSE_HIT) + [('callpropvoid', P('drawRect'), 4), ('getlocal', g), ('callpropvoid', P('endFill'), 0), ('L', 'end'), ('returnvoid',)]
        return c
    d.wrap('onEventInit', 'dmInitOrig', init)
    d.inst['traits'].append(Trait(P('dmHook'), 1, 0, d.add('dmHook', 0, HEAD + [('returnvoid',)])))   # the "already patched" mark
    d.done()
    return swf.write(), dict(placed=[], defs=0, methods=d.made)


def label_art(w, h):
    """the names' backing (the "?" of the view menu): a dark translucent rounded box with a faint light edge, 2x, straight RGBA"""
    import numpy as np
    W, H = int(round(w / ta.PX)), int(round(h / ta.PX)); rr = H * 0.30
    yy, xx = (np.mgrid[0:H, 0:W].astype(np.float32) + 0.5)
    qx = np.maximum(np.abs(xx - W / 2.0) - (W / 2.0 - rr), 0); qy = np.maximum(np.abs(yy - H / 2.0) - (H / 2.0 - rr), 0)
    dist = np.hypot(qx, qy) - rr                                     # < 0 inside the rounded box
    inside = np.clip(-dist + 0.5, 0, 1); edge = np.clip(1 - np.abs(dist + 1.5) / 1.5, 0, 1)
    a = np.maximum(0.72 * inside, 0.35 * edge)
    rgb = np.where(edge[..., None] > 0.5, np.array([200, 190, 170], np.float32), np.array([14, 12, 10], np.float32)) * np.ones((H, W, 1), np.float32)
    return (np.concatenate([rgb / 255.0, a[..., None]], -1) * 255 + 0.5).astype(np.uint8)


def mm_art(swf):
    """the minimap's buttons (MM_BTNS: round copper plates, our glyphs, an 'on' ring on the toggles / the view button) and the names (MM_LBL)"""
    swf.head = rect_bytes(MM_W + MM_GROW, 2000) + swf.head[-4:]   # the stage grows to the left; onEventInit shifts every root child by MM_GROW
    g = gr.Graft(swf); art = ta.Art(g); placed = []; ons = {}
    g.place(g.sprite([]), 0, 0, 'dmMmHit_mc')   # the one hit area (dmMmHitDraw draws it; dmMmHook puts it just below the PC ping / home)
    for name, code, icon, cx, cy, size, bit in MM_BTNS:
        on = None
        if bit or code in (20, 21):
            if size not in ons: ons[size] = ta.bitmap_shape(g, on_art(size))[0]
            on = ons[size]
        x0, y0 = cx - size / 2.0, cy - size / 2.0
        g.place(g.sprite(plate_kids(art, 'plate_round_copper', icon, size, 'ic', on)), x0, y0, name); placed.append((name, (x0, y0, x0 + size, y0 + size)))
    bg = ta.bitmap_shape(g, label_art(MM_LBL_W, MM_LBL_H))[0]
    for lname, tname, en, ru in MM_LBL:
        t = next(b for b in MM_BTNS if b[0] == tname); x1 = t[3] - t[5] / 2.0 - 10; x0 = x1 - MM_LBL_W; y0 = t[4] - MM_LBL_H / 2.0
        txt = ta.edit_text(g, (14, MM_LBL_H / 2.0 - MM_LBL_FONT * 0.72, MM_LBL_W - 14, MM_LBL_H / 2.0 + MM_LBL_FONT * 0.72), en, height=MM_LBL_FONT, align=1)
        g.place(g.sprite([(bg, 1, (0, 0), None), (txt, 2, (0, 0), 'txt')]), x0, y0, lname); placed.append((lname, (x0, y0, x1, y0 + MM_LBL_H)))
    g.finish()
    return placed, len(g.new_defs)


def _mm_code():
    """the view menu's methods (tools/dos2_touchkit.py subset): shift, open / close, names, the host's state, language"""
    show = ''.join('''
    o = this.getChildByName('%s')
    if o:
        o.visible = a''' % n for n in MM_ARC) + ''.join('''
    o = this.getChildByName('%s')
    if o:
        o.visible = l''' % l[0] for l in MM_LBL)
    state = ''.join('''
    o = this.getChildByName('%s')
    if o:
        n = o.getChildByName('on')
        if n:
            n.visible = b %% %d >= %d
        n = o.getChildByName('ic')
        if n:
            if b %% %d >= %d:
                n.alpha = 1
            else:
                n.alpha = 0.55''' % (b[0], 2 * b[6], b[6], 2 * b[6], b[6]) for b in MM_BTNS if b[6])
    lang = ''.join('''
    o = this.getChildByName('%s')
    if o:
        n = o.getChildByName('txt')
        if n:
            n.text = '%s\'''' % (l[0], l[3]) for l in MM_LBL)
    hook = ''.join('''
    o = this.getChildByName('%s')
    if o:
        o.dmCode = %d
        o.mouseEnabled = False
        o.mouseChildren = False''' % (b[0], b[1]) for b in MM_BTNS)
    pick = ''.join('''
    o = this.getChildByName('%s')
    if o:
        if o.visible:
            dx = x - %r
            dy = y - %r
            d = dx * dx + dy * dy
            if d < bd:
                bd = d
                best = o''' % (b[0], b[3], b[4]) for b in MM_BTNS)
    a0, a1 = MM_HIT_DEG; n = 30
    pts = [(MM_RING[0] + MM_HIT_R * math.cos(math.radians(a0 + (a1 - a0) * k / float(n))), MM_RING[1] + MM_HIT_R * math.sin(math.radians(a0 + (a1 - a0) * k / float(n)))) for k in range(n + 1)]
    pts += [(MM_RING[0] + MM_RING_R * math.cos(math.radians(a1 - (a1 - a0) * k / float(n))), MM_RING[1] + MM_RING_R * math.sin(math.radians(a1 - (a1 - a0) * k / float(n)))) for k in range(n + 1)]
    sector = '''
        g.moveTo(%r, %r)''' % (round(pts[0][0], 1), round(pts[0][1], 1)) + ''.join('''
        g.lineTo(%r, %r)''' % (round(x, 1), round(y, 1)) for x, y in pts[1:] + pts[:1])
    vw = next(b for b in MM_BTNS if b[1] == 20); sv = next(b for b in MM_BTNS if b[0] == 'dmMmSave_mc')
    view_sq = (vw[3] - MM_HIT_VIEW / 2.0, vw[4] - MM_HIT_VIEW / 2.0, MM_HIT_VIEW, MM_HIT_VIEW)
    save_sq = (sv[3] - MM_HIT_SAVE / 2.0, sv[4] - MM_HIT_SAVE / 2.0, MM_HIT_SAVE, MM_HIT_SAVE)
    hit = '''
def dmMmHook():%s
    h = this.getChildByName('dmMmHit_mc')
    if not h:
        return
    i = this.getChildIndex(h)
    p = this.pingButton_mc
    if p:
        k = this.getChildIndex(p)
        if k < i:
            i = k
    p = this.homestead_mc
    if p:
        k = this.getChildIndex(p)
        if k < i:
            i = k
    this.setChildIndex(h, i)
    h.addEventListener('mouseDown', this.dmMmPick)
def dmMmHitDraw(a, vis):
    h = this.getChildByName('dmMmHit_mc')
    if not h:
        return
    g = h.graphics
    g.clear()
    if not vis:
        return
    g.beginFill(0, 0)
    if a:%s
    else:
        g.drawRect(%r, %r, %r, %r)
    g.endFill()
    g.beginFill(0, 0)
    g.drawRect(%r, %r, %r, %r)
    g.endFill()
def dmMmPick(e):
    h = this.getChildByName('dmMmHit_mc')
    x = h.mouseX
    y = h.mouseY
    best = None
    bd = %d%s
    if not best:
        return
    best.alpha = 0.6
    this.dmMmDim = best
    s = this.stage
    if s:
        s.addEventListener('mouseUp', this.dmMmUp)
    this.dmBAct(best.dmCode)
def dmMmUp(e):
    o = this.dmMmDim
    if o:
        o.alpha = 1
    this.dmMmDim = None
    s = this.stage
    if s:
        s.removeEventListener('mouseUp', this.dmMmUp)
''' % ((hook, sector) + view_sq + save_sq + (MM_PICK_R * MM_PICK_R, pick))
    return hit + '''
def dmMmGrow():
    i = 0
    k = this.numChildren
    while i < k:
        o = this.getChildAt(i)
        o.x = o.x + %d
        i = i + 1
def dmMmArc():
    this.dmArcOn = not (this.dmArcOn == True)
    if not this.dmArcOn:
        this.dmInfoOn = False
    this.dmMmShow()
def dmMmInfo():
    this.dmInfoOn = not (this.dmInfoOn == True)
    this.dmMmShow()
def dmMmShow():
    vis = not (this.dmMapOn == False)
    a = False
    if vis:
        if this.dmArcOn == True:
            a = True
    l = False
    if a:
        if this.dmInfoOn == True:
            l = True%s
    o = this.getChildByName('dmMmView_mc')
    if o:
        o.visible = vis
        n = o.getChildByName('on')
        if n:
            n.visible = a
    o = this.getChildByName('dmMmInfo_mc')
    if o:
        n = o.getChildByName('on')
        if n:
            n.visible = l
    o = this.getChildByName('dmMmSave_mc')
    if o:
        o.visible = vis
    this.dmMmHitDraw(a, vis)
def dmMmState(b):
    this.dmBits = b%s
def dmMmLang(t):
    s = String(t)
    ru = False
    i = 0
    k = s.length
    while i < k:
        c = s.charCodeAt(i)
        if c >= 1024 and c <= 1279:
            ru = True
            i = k
        i = i + 1
    if not ru:
        return%s
''' % (MM_GROW, show, state, lang)


def minimap(data):
    """the PC minimap moved to the iPad's place + the iPad's view menu (MM_BTNS / MM_LBL; codes: 20 the menu, 21 the names, 10 + n dmKey(n))"""
    def setup(d):
        P = d.P
        d.kc(_mm_code())
        # showMiniMap(on): the original (map, tension, ping), then our buttons follow it
        d.wrap('showMiniMap', 'dmSmmOrig', lambda n: HEAD + d.call_orig('dmSmmOrig', n) + [('getlocal0',), ('getlocal1',), ('setproperty', P('dmMapOn')),
               ('getlocal0',), ('callpropvoid', P('dmMmShow'), 0), ('returnvoid',)])
        # setBtnVisible(MM_STATE + bits, ..) from the host (an invoke name the minimap class registers, main+0x1239a9c id 0xe) = the toggles' state
        d.wrap('setBtnVisible', 'dmSbvOrig', lambda n: HEAD + [('getlocal1',), ('pushbyte', MM_STATE), ('ifnge', 'orig'),
               ('getlocal0',), ('getlocal1',), ('pushbyte', MM_STATE), ('subtract',), ('convert_i',), ('callpropvoid', P('dmMmState'), 1), ('returnvoid',),
               ('L', 'orig')] + d.call_orig('dmSbvOrig', n) + [('returnvoid',)])
        # setBtnTooltip(i, text): the original, then the names' language (a Cyrillic letter in the engine's tooltip)
        d.wrap('setBtnTooltip', 'dmSbtOrig', lambda n: HEAD + d.call_orig('dmSbtOrig', n) + [('getlocal0',), ('getlocal2',), ('callpropvoid', P('dmMmLang'), 1), ('returnvoid',)])
    def lines(d, name, args=()): return [('getlocal0',)] + list(args) + [('callpropvoid', d.P(name), len(args))]
    click = lambda d: d.ei('PlaySound', [('pushstring', d.S('UI_Generic_Click'))])
    act = lambda d: [('getlocal1',), ('pushbyte', 20), ('ifne', 'n20')] + lines(d, 'dmMmArc') + click(d) + [('returnvoid',), ('L', 'n20'),
                     ('getlocal1',), ('pushbyte', 21), ('ifne', 'n21')] + lines(d, 'dmMmInfo') + click(d) + [('returnvoid',), ('L', 'n21')]
    ipre = lambda d: lines(d, 'dmMmGrow')
    ipost = lambda d: lines(d, 'dmMmHook') + lines(d, 'dmMmShow') + lines(d, 'dmMmState', [('pushbyte', 0)])
    # our buttons are not armed (no press / release listeners): dmMmHook gives them their codes, the hit area dmMmHit_mc acts on the press
    return moved(data, MM_MOVE, ('hit_mc', 2), [], MM_PAD, art=mm_art, act_pre=act, init_pre=ipre, init_post=ipost, setup=setup)




def combatlog(data):
    """the iPad's place: the window opens at the screen centre (onEventInit after the original, onEventResize instead of the original's bottomright)"""
    swf = Swf(data); d = Doc(swf); P, S = d.P, d.S
    centre = d.ei('setPosition', [('pushstring', S('center'))], [('pushstring', S('screen'))], [('pushstring', S('center'))])
    d.wrap('onEventInit', 'dmInitOrig', lambda n: HEAD + d.call_orig('dmInitOrig', n) + centre + [('returnvoid',)])
    d.wrap('onEventResize', 'dmResizeOrig', lambda n: HEAD + d.get('log_mc', 'moved') + [('iftrue', 'end')] + centre + [('L', 'end'), ('returnvoid',)])
    d.inst['traits'].append(Trait(P('dmHook'), 1, 0, d.add('dmHook', 0, HEAD + [('returnvoid',)])))
    d.done()
    return swf.write(), dict(placed=[], defs=0, methods=d.made)
def sc_art(swf):
    """Wait / Flee as the iPad's text plates: the PC End turn plate's own background (EndTurnBackground_28 of this movie) at SC_PLATE_W, a caption"""
    g = gr.Graft(swf); placed = []
    bgc = gr.Movie(swf).resolve('EndTurnBackground_28'); bw = 204.0
    for name, code, x, en, ru in SC_PLATES:
        txt = ta.edit_text(g, (8, SC_BTN_H / 2.0 - SC_FONT * 0.72, SC_PLATE_W - 8, SC_BTN_H / 2.0 + SC_FONT * 0.72), en, height=SC_FONT)
        g.place(g.sprite([(bgc, 1, (0, 0, SC_PLATE_W / bw, 1.0), None), (txt, 2, (0, 0), 'txt')]), x, SC_BTN_Y, name)
        placed.append((name, (x, SC_BTN_Y, x + SC_PLATE_W, SC_BTN_Y + SC_BTN_H)))
    g.finish()
    return placed, len(g.new_defs)


def _sc_code():
    ox, oy = SC_ORG
    arm = ''.join('''
    a = c.%s
    if a:
        a.x = %r
        a.y = %r
        a.scaleX = %r
        a.scaleY = %r''' % (n, x - ox, SC_ARM_TOP - oy, sg * SC_ARM_S[0], SC_ARM_S[1]) for n, x, sg in SC_ARM)
    txt = ''.join('''
    f = c.%s
    if f:
        f.x = %r
        f.y = %r''' % (n, (bar[0] + bar[1]) / 2.0 - w / 2.0 - ox, cy - 11 - oy) for n, bar, w, cy in SC_TXT)
    fit = ''.join('''
    m = this.getChildByName('%s')
    if m:
        f = m.getChildByName('txt')
        if f:
            this.dmFit(f, %d)''' % (n, SC_PLATE_W - 16) for n, code, x, en, ru in SC_PLATES)
    vis = ''.join('''
    m = this.getChildByName('%s')
    if m:
        b = d.%s
        v = False
        if d.visible:
            if b.visible:
                v = True
        if this.dmExplore == True:
            v = False
        m.visible = v
        if b.disabled_mc.visible:
            m.alpha = 0.45
        else:
            m.alpha = 1''' % (n, 'waitBtn_mc' if code == 1 else 'fleeBtn_mc') for n, code, x, en, ru in SC_PLATES)
    hook = ''.join('''
    m = this.getChildByName('%s')
    if m:
        m.dmCode = %d
        m.mouseChildren = False
        g = m.graphics
        g.beginFill(0, 0)
        g.drawRect(%d, %d, %d, %d)
        g.endFill()
        m.addEventListener('mouseDown', this.dmBDown)''' % (n, code, -SC_PAD[0], -SC_PAD[1], SC_PLATE_W + 2 * SC_PAD[0], SC_BTN_H + SC_PAD[1] + SC_PAD[2]) for n, code, x, en, ru in SC_PLATES)
    lang = ''.join('''
    m = this.getChildByName('%s')
    if m:
        f = m.getChildByName('txt')
        if f:
            f.text = '%s'
            this.dmFit(f, %d)''' % (n, ru, SC_PLATE_W - 16) for n, code, x, en, ru in SC_PLATES)
    return '''
def dmScBars():
    c = this.console_mc
    if not c:
        return
    c.frame_mc.visible = False
    b = c.getChildAt(0)
    if b:
        b.visible = False%s
    h = c.hbHolder_mc
    if h:
        h.x = %r
        h.y = %r
        h.scaleX = %r
        h.scaleY = %r%s
def dmFit(f, w):
    i = 0
    while i < 12:
        if f.textWidth <= w:
            return
        t = f.getTextFormat()
        if t.size <= %d:
            return
        t.size = t.size - 1
        f.setTextFormat(t)
        f.defaultTextFormat = t
        i = i + 1
def dmScMode():
    x = not (this.dmExplore == True)
    f = this.fightButtons_mc
    if f:
        f.visible = x
    c = this.console_mc
    if c:
        a = c.ap_mc
        if a:
            a.visible = x
    if not x:
        n = this.turnNotice_mc
        if n:
            n.visible = False
    this.dmScVis()
def dmScLay():
    this.dmScBars()
    a = this.console_mc.ap_mc
    if a:
        a.y = %r
    n = this.turnNotice_mc
    if n:
        n.y = %r%s
    f = this.fightButtons_mc
    if not f:
        return
    e = f.endBtn_mc
    if e:
        e.x = %d
        e.y = %d
        g = e.graphics
        g.beginFill(0, 0)
        g.drawRect(%d, %d, %d, %d)
        g.endFill()
    d = f.duoBtns_mc
    if d:
        d.alpha = 0
        d.mouseEnabled = False
        d.mouseChildren = False%s
    this.dmScVis()
def dmScVis():
    d = this.fightButtons_mc.duoBtns_mc
    if not d:
        return%s
def dmScAct(k):
    d = this.fightButtons_mc.duoBtns_mc
    if k == 1:
        if d.waitBtn_mc.disabled_mc.visible:
            return
        EI.call('PlaySound', 'UI_Gen_XButton_Click')
        EI.call('GuardPressed')
    if k == 2:
        if d.fleeBtn_mc.disabled_mc.visible:
            return
        EI.call('PlaySound', 'UI_Gen_XButton_Click')
        EI.call('FleePressed')
def dmScLang(t):
    s = String(t)
    ru = False
    i = 0
    k = s.length
    while i < k:
        c = s.charCodeAt(i)
        if c >= 1024 and c <= 1279:
            ru = True
            i = k
        i = i + 1
    if not ru:
        return%s
''' % (txt, SC_HB[0] - ox, SC_HB[1] - oy, SC_HB[2], SC_HB[3], arm, SC_FIT_MIN, SC_AP_Y - oy, SC_NOTICE_Y, fit, SC_END_X - ox, SC_BTN_Y - oy, -SC_PAD[0], -SC_PAD[1], 204 + 2 * SC_PAD[0],
       SC_BTN_H + SC_PAD[1] + SC_PAD[2], hook, vis, lang)


def status(data):
    """the PC status console raised by SC_UP; in touch the iPad's combat row (SC_*): bars on the hot bar, Wait / Flee / End turn as text plates"""
    def setup(d):
        P = d.P
        tc = d.abc.trait(d.ci, 'originalNotificationPosY'); assert tc is not None and tc.kind & 15 == 6 and tc.vkind == 6, 'originalNotificationPosY: a Number const'
        assert struct.unpack('<d', d.abc.doubles[tc.vindex])[0] == 873.0, 'originalNotificationPosY is not 873'
        d.abc.doubles.append(struct.pack('<d', float(SC_NOTICE_Y))); tc.vindex = len(d.abc.doubles) - 1
        press_code(d, [('getlocal0',), ('getlocal1',), ('callpropvoid', P('dmScAct'), 1)])   # the plates: press dims, release on the plate acts
        d.kc(_sc_code())
        # the PC's own visibility / disabled calls drive the plates (combat only, as the PC duo buttons); setBtnText(0, caption) -> language
        d.wrap('setBtnDisabled', 'dmSbdOrig', lambda n: HEAD + d.call_orig('dmSbdOrig', n) + [('getlocal0',), ('callpropvoid', P('dmScVis'), 0), ('returnvoid',)])
        # setBtnVisible(i, on): the host's SC_EXPLORE (the engine hid the object out of combat, the host shows it again for the bars) =
        # explore mode: bars only; an engine call that SHOWS a fight button (i < SC_EXPLORE, on) = combat again; the plates follow
        d.wrap('setBtnVisible', 'dmSbvOrig', lambda n: HEAD + [('getlocal1',), ('pushbyte', SC_EXPLORE), ('ifnge', 'orig'),
               ('getlocal0',), ('pushtrue',), ('setproperty', P('dmExplore')), ('getlocal0',), ('callpropvoid', P('dmScMode'), 0), ('returnvoid',),
               ('L', 'orig')] + d.call_orig('dmSbvOrig', n) + [('getlocal2',), ('iffalse', 'vis'), ('getlocal0',), ('getproperty', P('dmExplore')), ('iffalse', 'vis'),
               ('getlocal0',), ('pushfalse',), ('setproperty', P('dmExplore')), ('getlocal0',), ('callpropvoid', P('dmScMode'), 0), ('returnvoid',),
               ('L', 'vis'), ('getlocal0',), ('callpropvoid', P('dmScVis'), 0), ('returnvoid',)])
        # setCombatTurnNotification(n, ..): n >= 0 = a turn notice = combat
        d.wrap('setCombatTurnNotification', 'dmSctnOrig', lambda n: HEAD + d.call_orig('dmSctnOrig', n) + [('getlocal1',), ('pushbyte', 0), ('ifnge', 'end'),
               ('getlocal0',), ('getproperty', P('dmExplore')), ('iffalse', 'end'), ('getlocal0',), ('pushfalse',), ('setproperty', P('dmExplore')),
               ('getlocal0',), ('callpropvoid', P('dmScMode'), 0), ('L', 'end'), ('returnvoid',)])
        # updateArmourBar(): the original (Console_1.setLeft / setRightArmourBar force scaleX -1 / 1), then our bars' geometry again
        d.wrap('updateArmourBar', 'dmUabOrig', lambda n: HEAD + d.call_orig('dmUabOrig', n) + [('getlocal0',), ('callpropvoid', P('dmScBars'), 0), ('returnvoid',)])
        d.wrap('setBtnText', 'dmSbtxOrig', lambda n: HEAD + d.call_orig('dmSbtxOrig', n) + [('getlocal0',), ('getlocal2',), ('callpropvoid', P('dmScLang'), 1), ('returnvoid',)])
    post = lambda d: [('getlocal0',), ('callpropvoid', d.P('dmScLay'), 0)]
    return moved(data, [(n, 0, -SC_UP) for n in SC_MOVE], art=sc_art, init_post=post, setup=setup)
def same(data): return data, dict(placed=[], defs=0, methods=[])


# ---------------------------------------------------------------- the right side
def rect_bytes(w, h):
    vals = [0, w * 20, 0, h * 20]; nb = max(v.bit_length() for v in vals) + 1
    bits = format(nb, '05b') + ''.join(format(v, '0%db' % nb) for v in vals); bits += '0' * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def side(data):
    swf = Swf(data); g = gr.Graft(swf); art = ta.Art(g); placed = []
    sq, _ = art.png('plate_square_copper'); rd, _ = art.png('plate_round_copper')
    for name, code, icon, x, y, size in SIDE:
        if icon == 'btn_character':                              # the round helmet button: our drawing of the iPad's side panel toggle, 105 px
            c, cb = art.png(icon); btn = g.sprite([(c, 1, (0, 0, size / (cb[2] - cb[0])), None)])
        else:
            ic, ib = art.png(icon)
            btn = g.sprite([(rd if code == 1 else sq, 1, (0, 0), None), (ic, 2, (size / 2.0 - (ib[2] - ib[0]) / 2.0, size / 2.0 - (ib[3] - ib[1]) / 2.0), None)])
        g.place(btn, x, y, name); placed.append((name, (x, y, x + size, y + size)))
    g.finish(); ndefs = len(g.new_defs)
    swf.head = rect_bytes(SIDE_W, SIDE_H) + swf.head[-4:]
    d = Doc(swf); P, S = d.P, d.S
    # dmBAct(code): 20 = the helmet: the column shown / hidden (local, as the iPad's setRolledIn); else dmSide(code) to the host
    act = [('getlocal1',), ('pushbyte', 20), ('ifne', 'host'),
           ('getlocal0',)] + d.get('dmRolled') + [('not',), ('setproperty', P('dmRolled')), ('getlocal0',), ('callpropvoid', P('dmRoll'), 0)] + d.ei(
           'PlaySound', [('pushstring', S('UI_Generic_Click'))]) + [('returnvoid',), ('L', 'host')] + d.ei('dmSide', [('getlocal1',)])
    press_code(d, act)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1)]
    for k, name in enumerate(SIDE_COLUMN):
        code += d.child(name, 1) + [('getlocal', 1), ('iffalse', 'r%d' % k), ('getlocal', 1)] + d.get('dmRolled') + [('not',), ('setproperty', P('visible')), ('L', 'r%d' % k)]
    d.method('dmRoll', 0, code + [('returnvoid',)])
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1)] + d.get('dmOn') + [('iftrue', 'end'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmOn')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmRolled'))]
    for k, (name, c, _, _, _, _) in enumerate(SIDE):
        code += d.child(name, 1) + [('getlocal', 1), ('iffalse', 'n%d' % k)] + arm(d, 1, c) + [('L', 'n%d' % k)]
    d.method('dmHook', 0, code + [('L', 'end'), ('returnvoid',)])
    # onEventInit(): the iPad's anchor for this movie's own id, then the hook (the original centred "watermark": kept as dmInitOrig, not called)
    d.wrap('onEventInit', 'dmInitOrig', lambda n: HEAD + d.ei('registerAnchorId', [('pushstring', S('dmSide'))]) + d.ei(
        'setAnchor', [('pushstring', S('topright'))], [('pushstring', S('screen'))], [('pushstring', S('topright'))]) + [('getlocal0',), ('callpropvoid', P('dmHook'), 0), ('returnvoid',)])
    d.done()
    return swf.write(), dict(placed=placed, defs=ndefs, methods=d.made)


def _closer(name): return lambda data: closer(data, PANEL_CLOSE[name])
MAKE = {'hotBar_p.swf': hotbar, 'minimap_p.swf': minimap, 'statusConsole_p.swf': status, 'playerInfo_p.swf': same, 'combatLog_p.swf': combatlog, 'dmSide_p.swf': side}
MAKE.update({n + '_p.swf': _closer(n) for n in PANEL_CLOSE})
MAKE.update(pps.MAKE)   # iPad layout instead of the close button only (tools/dos2_ppanel_skills.py)
import dos2_ppanel_inv as ppi   # partyInventory_p / characterSheet_p: the iPad's layout (its own module and checks) instead of the close-button pass
MAKE.update(ppi.MAKE)


def build(src):
    """{out name: bytes} from sources()"""
    return {o: MAKE[o](src[OUT[o]])[0] for o in OUT}


def check(src):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    for o in OUT:
        data = src[OUT[o]]; out, rep = MAKE[o](data)
        print('%s -> %s: %d B -> %d B, +%d definition tags, +%d root instances, +%d methods, sha256 %s' % (
            OUT[o], o, len(data), len(out), rep['defs'], len(rep['placed']), len(rep['methods']), hashlib.sha256(out).hexdigest()[:16]))
        if MAKE[o] is same: say(out == data, 'the PC movie byte for byte'); continue
        if o in ppi.MAKE: ppi.check(o, data, out, rep, say); continue
        b = Swf(out); (tb, pb, B), = b.abcs()
        errs = verify_all(B)
        for who, why in errs: print('       %s: %s' % (who, why))
        say(not errs, 'ABC verifies: %d method bodies' % len(B.bodies))
        say(Swf(Swf(out).write()).body() == b.body(), 'the movie parses and round-trips')
        g = pps.tag_filter(o, gr.check(data, out, quiet=True)); want = ['header / tail differ'] if o in ('dmSide_p.swf', 'minimap_p.swf') else []   # uiCraft_p: widened art (dos2_ppanel_skills)
        say(g == want, 'every original tag kept, ids unique, uses after definitions%s' % ('' if g == want else ': ' + str(g)))
        ib = {v: k for k, v in B.owners().items()}; doc = [B.mname(c['name']) for c in B.instances if B.mname(c['name']).endswith('.MainTimeline')][0]
        dis = lambda m: '\n'.join(B.dis(B.methods[ib[doc + '::' + m]].body))
        names = {gr.place_parse(c, t)['name'] for c, t, _ in b.tags if c in (4, 26, 70)}
        if o == 'hotBar_p.swf':
            t = dis('dmLay')
            say("'updateSlots'" in t and 'scrollRect' in t and 'scaleX' in t, 'dmLay: scale %.3f, crop %d..%d, hotbar_mc at %d,%d, updateSlots(%d)' % (_s, CROP, CROP + VISW, HB_X, HB_Y, SLOTS))
            first = HB_X + (PC_FIRST - CROP) * _s; top = HB_Y + PC_SLOT_Y * _s + 80
            say(abs(PC_PITCH * _s - 86) < 0.2 and abs(top + PC_SLOT_H * _s - IPAD_SLOT_BOTTOM) < 1.5,
                'slots: pitch %.1f (iPad 86), cell %.1f x %.1f (iPad icon 76 / cell 82), stage x %.0f..%.0f, screen y %.0f..%.0f (iPad 967..1049)' % (
                PC_PITCH * _s, PC_SLOT_W * _s, PC_SLOT_H * _s, first, first + ((SLOTS - 1) * PC_PITCH + PC_SLOT_W) * _s, top, top + PC_SLOT_H * _s))
            lo, hi = CHAT_BOX[0], LOG_BOX[0] + LOG_BOX[2]; bar_r = HB_X + VISW * _s
            say(abs((lo + hi) / 2.0 - 974) <= 1 and abs((hi - lo) / 2.0 - ROW_HALF) <= 1 and abs((HB_X - CHAT_BOX[0] - PLATE) - GAP) <= 1 and abs(LOG_BOX[0] - bar_r - GAP) <= 1,
                'the row %d..%d [chat %d | bar %d..%.0f | log %d] centred on the stage centre 974, half width %.0f (TS_HB_HALF %d), gaps %d both sides' % (lo, hi, PLATE, HB_X, bar_r, PLATE, (hi - lo) / 2.0, ROW_HALF, GAP))
            say(abs(CHAT_BOX[1] + PLATE / 2.0 - SLOT_MID_Y) <= 1 and CHAT_BOX[1] == LOG_BOX[1] and CHAT_BOX[1] >= HB_END_TOP and abs(HB_Y + 54 * _s - HB_FRAME_TOP) <= 2,
                'one centre line %.0f: slots, actions button, chat / log plates %d..%d; tops: the frame %.0f (TS_HB_TOP / _L / _R 125 above the bottom)' % (SLOT_MID_Y, CHAT_BOX[1], CHAT_BOX[1] + PLATE, HB_Y + 54 * _s))
            act_l = HB_X + (ACT_BTN[0] - CROP) * _s; slot_r = HB_X + (PC_FIRST + (SLOTS - 1) * PC_PITCH + PC_SLOT_W - CROP) * _s
            t = dis('dmLay'); say('actionsButton_mc' in t and act_l > slot_r and act_l + 34 * _s <= bar_r and abs(ACT_X - (act_l + 17 * _s)) <= 1,
                'actions button (the PC actionsButton_mc) in the bar after the 12th slot: %.0f..%.0f (slot 12 ends %.0f), its list above it at %d' % (act_l, act_l + 34 * _s, slot_r, ACT_X))
            say('chatBtn_mc' in t and 'showLog_mc' in t and t.count('visible') >= 3 and all((n.encode() + b'\0') in names for n in ('dmChat_mc', 'dmLog_mc'))
                and "'ToggleChatLog'" in dis('dmBAct') and "'CombatLogBtnPressed'" in dis('dmBAct') and "'UI_Generic_Click'" in dis('dmBAct'),
                'chat / combat log: our plates %s / %s (PC tabs hidden) -> ToggleChatLog / CombatLogBtnPressed + UI_Generic_Click' % (CHAT_BOX, LOG_BOX))
            t = dis('dmPads'); say('dmPads' in dis('dmHook') and 'drawRect' in t and (PLATE_PAD - PLATE) // 2 < GAP, 'chat / log finger pads %d x %d (drawing %d), they stop short of the bar' % (PLATE_PAD, PLATE_PAD, PLATE))
            t = dis('dmSlotsCut'); say("'slot'" in t and 'visible' in t and 'dmSlotsCut' in dis('dmHook'), 'slots 13.. hidden (the actions button sits where the 13th was)')
            t = dis('showSkillBar'); tv = dis('dmVis')
            say('dmSsbOrig' in t and 'dmVis' in t and 'dmDeckShow' in tv and 'hotkeys_mc' in tv and 'dmChat_mc' in tv and 'showLog_mc' in tv and 'endPiece_mc' in tv,
                'showSkillBar(on): the original (it sets visible on lockButton_mc / hotkeys_mc), then dmVis: the lock only with the open deck list, no slot numbers, the plates follow')
            t = dis('dmLang'); say('dmFbtOrig' in dis('setFixedBtnTooltips') and 'dmLang' in dis('setFixedBtnTooltips') and 'dmLang' in dis('setLockBtnTooltips') and 'charCodeAt' in t
                and CONF_RU in t and HINT_RU in t, 'language: a Cyrillic letter in the engine tooltips -> %s / %s' % (CONF_RU, HINT_RU))
            t = dis('dmLblFx'); say('createStrokeFilter' in t and 'dmLbl' in t and 'dmTxt' in t, 'the words get the PC stroke (textEffect.createStrokeFilter, as the hot bar numbers)')
            say('dmLay' in dis('onEventResolution') and 'dmInitOrig' in dis('onEventInit') and 'dmHook' in dis('onEventInit'), 'onEventInit = original + dmLay + dmHook; onEventResolution = dmLay')
            t = dis('dmSwUp'); say('cycleHotBar' in t and 'isDragging' in t, 'swipe over the slots (%d px, not while dragging) -> cycleHotBar' % SWIPE)
            tk = dis('dmTick'); td = dis('dmSwDown')
            say('dmSwL' in t and 'mouseX' not in t and 'dmSwOn' in t and 'dmSwL' in tk and 'mouseX' in tk and 'dmSwOn' in tk and 'dmSwL' in td and 'dmSwOn' in td,
                "swipe travel = the cursor x the frames saw while the press was down (dmTick -> dmSwL), never the release's x (phone 23.11: the host's "
                'off-screen park after a long press flipped the deck)')
            t = dis('showActiveSkill'); say('dmSasOrig' in t and "'dmAim'" in t and 'visible' in t, 'showActiveSkill: original, CONFIRM / cancel visible, dmAim(0 | 1)')
            say("'dmAimBtn'" in dis('dmBAct') and all((n.encode() + b'\0') in names for n in ('dmConfirm_mc', 'dmCancel_mc')), 'CONFIRM %s / cancel %s on the root, tap -> dmAimBtn(1 | 0)' % (CONFIRM, CANCEL))
            t = dis('dmBAct'); say('dmDeckShow' in t and 'cycleHotBar' in t and all((('dmDeck%d_mc' % k).encode() + b'\0') in names and (('dmInd%d_mc' % k).encode() + b'\0') in names for k in range(1, 6))
                                   and (b'dmDeckBg_mc\0') in names and 'currentHotBarIndex' in dis('dmTick'), 'deck list (button, 5 plates -> cycleHotBar) and the deck indicator (5 bars, enterFrame)')
            t = dis('dmHook'); say('cycleHotBar_mc' in t and 'upBtn_mc' in t and 'downBtn_mc' in t and 'lockButton_mc' in t and 'addChild' in t and 'getBounds' in t
                                   and 'lockButton_mc' in dis('dmDeckShow') and 'dmDeckBg_mc' in dis('dmDeckShow'),
                                   'iPad deck switcher: the PC number = the button (arrows hidden, plate), the lock moved into the list %s, shown with it' % (DECK_BG,))
            say('dmSetButtonOrig' in dis('setButton') and "''" in dis('setButton') and 'dmSetTextOrig' not in dis('setText').split('returnvoid')[0] and 'hotkeys_mc' in dis('dmHook'),
                'no PC keyboard labels: slot numbers hidden (hotkeys_mc), setText / setAllText dropped, setButton with an empty letter')
            t = dis('showActiveSkill'); say('dmPlate' in t and str(PLATE_BASE) in t and str(PLATE_HIDE) in t, 'action plate: showActiveSkill(%d + x * %d + y) puts CONFIRM %d px above the point, %d hides' % (PLATE_BASE, PLATE_MUL, PLATE_UP, PLATE_HIDE))
            say('dmAimH' in t and str(AIM_SHOW) in t and str(AIM_HIDE) in t and t.count('dmAimH') >= 4, 'host aim: showActiveSkill(%d | %d) shows / hides CONFIRM / cancel (dmAimH), every path shows them for dmAimV || dmAimH' % (AIM_SHOW, AIM_HIDE))
            t = dis('setActionPreview'); say('dmSapOrig' in t and "'ActionAttackGround'" in t and "'dmAim'" in t, 'setActionPreview: original, "ActionAttackGround" -> dmAim(2 | 3)')
            art_ok = True
            for nm, sz, gl in (('dmConfirm_mc', CONFIRM[2], None), ('dmCancel_mc', CANCEL[2], 'cross')):
                im = aim_art(sz, gl); al = im[..., 3].astype(float) / 255; c0 = im.shape[0] // 2
                ring = max(al[c0, :c0 // 3].max(), al[:c0 // 3, c0].max())          # the ring on the left / top
                glass = al[int(c0 * 0.55), int(c0 * 1.45)]                           # inside, off the glyph
                lum = im[int(c0 * 0.55), int(c0 * 1.45), :3].mean()
                art_ok &= im.shape[:2] == (int(round(sz / ta.PX)),) * 2 and al[0, 0] == 0 and ring >= 0.85 and 0.35 <= glass <= 0.60 and lum < 40
                if gl: art_ok &= im[c0, c0, :3].min() >= 200
            say(art_ok, 'CONFIRM / cancel art: a thin light ring around a dark translucent disc (%d / %d px; inside alpha 42..50 %%, transparent corners; cancel: a white X)' % (CONFIRM[2], CANCEL[2]))
            t = dis('showActiveSkill'); say(b'dmHint_mc\0' in names and t.count('dmHint_mc') >= 2 and 'dmHint_mc' in dis('dmHook'),
                'hint banner %s (the Switch %s #%d: %s) shown with cancel while aiming, never takes a tap' % (HINT, HINT_SRC[0], HINT_SRC[1], 'copied' if SRC_EXTRA.get(HINT_SRC[0]) else 'NOT FOUND, a plain plate'))
            say(LBL_FONT >= 22 and HINT_FONT >= 22, 'text sizes: CONFIRM %d px, hint %d px (>= 22)' % (LBL_FONT, HINT_FONT))
            if SW_PAK:   # the game's own word for CONFIRM (the player's Localization paks next to Game.pak)
                import lspk, re as _re
                base = os.path.join(os.path.dirname(SW_PAK), 'Localization'); ok = True; seen = []
                for pk, xml, want_w in (('English.pak', 'Localization/English/english.xml', CONF_EN), ('Russian/Russian.pak', 'Localization/Russian/russian.xml', CONF_RU)):
                    f = os.path.join(base, pk)
                    if not os.path.isfile(f): continue
                    lp = lspk.Pak(f); by = {e[0]: e for e in lp.entries}
                    m = _re.search(r'contentuid="%s"[^>]*>([^<]*)<' % CONF_HANDLE, lp.read(by[xml]).decode('utf-8'))
                    ok &= bool(m) and m.group(1).upper() == want_w; seen.append('%s %r' % (pk.split('/')[-1], m.group(1) if m else None))
                say(ok and bool(seen), "CONFIRM = the game's own word (%s): %s" % (CONF_HANDLE, ', '.join(seen) or 'no Localization pak found'))
        elif o == 'combatLog_p.swf':
            t = dis('onEventInit'); say('dmInitOrig' in t and "'center'" in t and "'center'" in dis('onEventResize'), 'onEventInit = original + setPosition center / screen / center; onEventResize the same unless moved')
        elif o == 'minimap_p.swf':
            say(b.head[:-4] == rect_bytes(MM_W + MM_GROW, 2000) and 'dmMmGrow' in dis('onEventInit') and str(MM_GROW) in dis('dmMmGrow') and 'numChildren' in dis('dmMmGrow'),
                'stage %d wide (the PC %d + %d to the left), every root child shifted +%d at init: screen x = W - (%d - x) as before' % (MM_W + MM_GROW, MM_W, MM_GROW, MM_GROW, MM_W))
            t = dis('dmBAct'); say("'dmKey'" in t and 'dmMmArc' in t and 'dmMmInfo' in t and all((n[0].encode() + b'\0') in names for n in MM_BTNS + MM_LBL),
                'view menu: %s on the root; view -> the arc, "?" -> the names, toggles -> dmKey(%s), quick save -> dmKey(1)' % (
                    ', '.join(b2[0] for b2 in MM_BTNS), ' / '.join(str(b2[1] - 10) for b2 in MM_BTNS if b2[6])))
            xs = [p2[1] for p2 in rep['placed']]
            say(min(x[0] for x in xs) >= -MM_GROW and max(x[2] for x in xs) <= MM_W and min(x[1] for x in xs) >= 0,
                'everything inside the grown stage: x %.0f..%.0f of -%d..%d, top %.0f' % (min(x[0] for x in xs), max(x[2] for x in xs), MM_GROW, MM_W, min(x[1] for x in xs)))
            clash = []
            for i, a2 in enumerate(MM_BTNS):
                for c2 in MM_BTNS[i + 1:]:
                    dd = ((a2[3] - c2[3]) ** 2 + (a2[4] - c2[4]) ** 2) ** 0.5
                    if dd < (a2[5] + c2[5]) / 2.0 + 8: clash.append('%s/%s %.0f' % (a2[0], c2[0], dd))
            onring = [b2[0] for b2 in MM_BTNS if b2[0] != 'dmMmSave_mc' and math.hypot(b2[3] - MM_RING[0], b2[4] - MM_RING[1]) - b2[5] / 2.0 < MM_RING_R]
            say(not clash and not onring and all(b2[5] == (MM_TGL if b2[1] in (12, 13, 14, 15, 20) else MM_SMALL) for b2 in MM_BTNS),
                'buttons: toggles / view %d, info / save %d, drawings >= 8 px apart, none over the ring (save on its rim, as the iPad)%s' % (MM_TGL, MM_SMALL, ': ' + ', '.join(clash + onring) if clash or onring else ''))
            t = dis('dmMmPick'); th = dis('dmMmHook'); tw = dis('dmMmHitDraw')
            say((b'dmMmHit_mc\0') in names and 'mouseDown' in th and 'setChildIndex' in th and 'pingButton_mc' in th and 'mouseEnabled' in th and "'dmBDown'" not in th
                and 'dmBAct' in t and 'mouseUp' in t and 'alpha' in t and str(MM_PICK_R * MM_PICK_R) in t and 'dmMmHitDraw' in dis('dmMmShow') and 'lineTo' in tw and 'clear' in tw,
                'one hit area under the PC ping / home: a PRESS picks the nearest visible button within %d px and acts at once (the release only ends the press look); '
                'open: the sector r %d..%d, %d..%d deg + the save square %d; closed: the view square %d + save' % (MM_PICK_R, MM_RING_R, MM_HIT_R, MM_HIT_DEG[0], MM_HIT_DEG[1], MM_HIT_SAVE, MM_HIT_VIEW))
            import itertools
            far = [b2[0] for b2 in MM_BTNS if b2[0] not in ('dmMmSave_mc',) and b2[1] != 20 and not (MM_HIT_DEG[0] <= (math.degrees(math.atan2(b2[4] - MM_RING[1], b2[3] - MM_RING[0])) % 360) <= MM_HIT_DEG[1]
                   and MM_RING_R < math.hypot(b2[3] - MM_RING[0], b2[4] - MM_RING[1]) + MM_PICK_R and math.hypot(b2[3] - MM_RING[0], b2[4] - MM_RING[1]) + 45 <= MM_HIT_R)]
            say(not far and min(MM_RING[0] - MM_HIT_R, 0) >= -MM_GROW, 'every arc button lies inside the hit sector, the sector inside the grown stage (x >= %d)%s' % (MM_RING[0] - MM_HIT_R, ': ' + ', '.join(far) if far else ''))
            t = dis('dmMmState'); say('dmSbvOrig' in dis('setBtnVisible') and str(MM_STATE) in dis('setBtnVisible') and 'alpha' in t and "'on'" in t,
                'state from the host: setBtnVisible(%d + bits) -> the "on" ring + full glyph (off: glyph 55 %%); bits 1 tactical, 2 labels, 4 cones, 8 camera lock' % MM_STATE)
            say('dmSmmOrig' in dis('showMiniMap') and 'dmMmShow' in dis('showMiniMap') and 'dmSbtOrig' in dis('setBtnTooltip') and 'dmMmLang' in dis('setBtnTooltip'),
                "showMiniMap hides / shows our buttons too; setBtnTooltip -> the names' language")
            say('drawRect' in dis('onEventInit') and all(n in dis('onEventInit') for n, _ in MM_PAD), 'PC ring buttons\' finger pads: %s' % ', '.join('%s %d' % p2 for p2 in MM_PAD))
            t = dis('onEventInit'); say('dmInitOrig' in t and all(n in t for n, _, _ in MM_MOVE) and "'dmSide'" in dis('dmBAct'), 'onEventInit = original + %d moves; ring tap -> dmSide(2)' % len(MM_MOVE))
        elif o == 'statusConsole_p.swf':
            t = dis('onEventInit'); say('dmInitOrig' in t and all(n in t for n in SC_MOVE), 'onEventInit = original + console / fight buttons / turn notice / GM button raised by %d' % SC_UP)
            t = dis('dmScBars'); tl = dis('dmScLay')
            spans = [SC_GREY, SC_RED, SC_BLUE]; widths = [s2[1] - s2[0] for s2 in spans]
            say('dmScBars' in tl and 'dmScLay' in dis('onEventInit') and 'hbHolder_mc' in t and all(n in t for n, _, _ in SC_ARM) and 'frame_mc' in t
                and abs(SC_GREY[0] - SC_SPAN[0] - SC_BAR_INSET) < 0.01 and abs(SC_SPAN[1] - SC_BLUE[1] - SC_BAR_INSET) < 0.01
                and abs(SC_RED[0] - SC_GREY[1] - SC_BAR_GAP) < 0.01 and abs(SC_BLUE[0] - SC_RED[1] - SC_BAR_GAP) < 0.01
                and abs(widths[0] / widths[1] - 309 / 505.0) < 1e-3 and abs(widths[2] - widths[0]) < 0.01,
                'bars flush on the hot bar frame %.0f..%.0f (status stage; the frame top %.0f): grey %.0f..%.0f | red %.0f..%.0f | blue %.0f..%.0f (309 / 505 / 309 x %.3f, inset %d, gaps %d)' % (
                    SC_SPAN[0], SC_SPAN[1], SC_FOOT, SC_GREY[0], SC_GREY[1], SC_RED[0], SC_RED[1], SC_BLUE[0], SC_BLUE[1], SC_BAR_K, SC_BAR_INSET, SC_BAR_GAP))
            say(abs(SC_HB[1] + 16 - SC_FOOT) < 0.01 and abs(SC_ARM_TOP + 12 - SC_FOOT) < 0.01 and abs(SC_FOOT - 960) < 1,
                'feet on the frame: health %.0f..%.0f, armour %.0f..%.0f (screen at factor 1; phone 23.11: the frame top ~960)' % (SC_HB[1], SC_HB[1] + 16, SC_ARM_TOP, SC_ARM_TOP + 12))
            t = dis('updateArmourBar'); say('dmUabOrig' in t and 'dmScBars' in t and 'leftArmourBarHolder_mc' in dis('dmScBars') and 'rightArmourBarHolder_mc' in dis('dmScBars'),
                'updateArmourBar: the original (Console_1 forces scaleX -1 / 1), then dmScBars: left x%.3f mirrored with its origin at the right end %.0f, right x%.3f from %.0f' % (
                    -SC_ARM_S[0], SC_GREY[1], SC_ARM_S[0], SC_BLUE[0]))
            t = dis('setBtnVisible'); tm = dis('dmScMode')
            say(str(SC_EXPLORE) in t and 'dmExplore' in t and 'dmScMode' in t and 'dmExplore' in dis('setCombatTurnNotification') and 'fightButtons_mc' in tm and 'ap_mc' in tm
                and 'turnNotice_mc' in tm and 'dmExplore' in dis('dmScVis'),
                'out of combat: the host shows the object again and sends setBtnVisible(%d) = bars only (fight buttons, AP, turn plaque, plates hidden); a fight button shown / a turn notice = combat' % SC_EXPLORE)
            t = dis('dmScBars'); tl = dis('dmScLay'); tc = B.trait(ib[doc] if False else [i for i, c2 in enumerate(B.instances) if B.mname(c2['name']) == doc][0], 'originalNotificationPosY')
            say('getChildAt' in t and all(n in t for n, *_ in SC_TXT) and 'ap_mc' in tl and 'turnNotice_mc' in tl and struct.unpack('<d', B.doubles[tc.vindex])[0] == SC_NOTICE_Y
                and SC_NOTICE_Y + SC_NOTICE_HALF + SC_STACK_GAP == SC_AP_Y and SC_AP_Y + SC_AP_H + SC_STACK_GAP == SC_HB[1] and SC_BTN_Y + SC_BTN_H == SC_AP_Y + SC_AP_H,
                'one stack, gaps %d: plaque %d..%d (originalNotificationPosY 873 -> %d) | AP row %d..%d (overflow inside) | bars %d..%d; plates %d..%d (feet on the AP row); the dark backing (child 0) hidden, the numbers on their bars' % (
                    SC_STACK_GAP, SC_NOTICE_Y - SC_NOTICE_HALF, SC_NOTICE_Y + SC_NOTICE_HALF, SC_NOTICE_Y, SC_AP_Y, SC_AP_Y + SC_AP_H, SC_HB[1], SC_FOOT, SC_BTN_Y, SC_BTN_Y + SC_BTN_H))
            say(SC_PLATES[1][2] + SC_PLATE_W + 10 <= 960 - 230, 'the plates end at %d, 10+ px left of the turn plaque (ВАШ ХОД: ~460 wide on the phone 23.11, from %d)' % (SC_PLATES[1][2] + SC_PLATE_W, 960 - 230))
            t = dis('dmFit'); say('textWidth' in t and 'setTextFormat' in t and 'dmFit' in dis('dmScLay') and 'dmFit' in dis('dmScLang'),
                'captions shrink to the plate inner width %d (TextFormat size, not below %d)' % (SC_PLATE_W - 16, SC_FIT_MIN))
            t = dis('dmScAct'); say(all((n.encode() + b'\0') in names for n, *_ in SC_PLATES) and "'GuardPressed'" in t and "'FleePressed'" in t and 'disabled_mc' in t and "'UI_Gen_XButton_Click'" in t,
                'Wait / Flee text plates %s (the PC End turn plate art, %d x %d) -> GuardPressed / FleePressed (the PC sound; nothing while the PC button is disabled)' % (
                    ', '.join('%s %d' % (n, x) for n, c, x, *_ in SC_PLATES), SC_PLATE_W, SC_BTN_H))
            t = dis('dmScVis'); say('dmScVis' in dis('setBtnVisible') and 'dmScVis' in dis('setBtnDisabled') and 'waitBtn_mc' in t and 'fleeBtn_mc' in t and 'alpha' in t,
                'the plates follow the PC duo buttons (setBtnVisible / setBtnDisabled: combat only; disabled = 45 % and no action)')
            t = dis('dmScLang'); say('dmScLang' in dis('setBtnText') and all(ru in t for *_, ru in SC_PLATES) and SC_FONT >= 22,
                'captions: %s (English) / %s (a Cyrillic letter in the engine\'s End turn caption), %d px' % (' / '.join(p2[3] for p2 in SC_PLATES), ' / '.join(p2[4] for p2 in SC_PLATES), SC_FONT))
            say(SC_BTN_Y + SC_BTN_H + SC_PAD[2] < SC_HB[1] and SC_BTN_Y - SC_PAD[1] >= HINT[1] + 64 * HINT[3] + 80 and SC_PLATES[0][2] + SC_PLATE_W + 8 <= SC_PLATES[1][2],
                'finger pads %d x %d (plates) / %d x %d (End turn): screen y %d..%d, between the hint banner (foot %.0f) and the bars (%.0f)' % (
                    SC_PLATE_W + 2 * SC_PAD[0], SC_BTN_H + SC_PAD[1] + SC_PAD[2], 204 + 2 * SC_PAD[0], SC_BTN_H + SC_PAD[1] + SC_PAD[2], SC_BTN_Y - SC_PAD[1], SC_BTN_Y + SC_BTN_H + SC_PAD[2], HINT[1] + 64 * HINT[3] + 80, SC_HB[1]))
        elif o in pps.MAKE: pps.check_one(o, data, out, say)   # tools/dos2_ppanel_skills.py
        elif o[:-6] in PANEL_CLOSE:
            import dos2_layout as lay, tempfile
            t = dis('onEventInit'); path = PANEL_CLOSE[o[:-6]]
            say('dmInitOrig' in t and all(n in t for n in path) and 'drawRect' in t and 'getBounds' in t, 'onEventInit = original + a %d px hit area on %s' % (CLOSE_HIT, '.'.join(path)))
            with tempfile.TemporaryDirectory() as td:   # the instance really is there, with the PC's small drawing
                f = os.path.join(td, OUT[o]); open(f, 'wb').write(data)
                hit = [e for e in lay.walk(lay.Mov.load(f), 3) if e['npath'] == '.'.join(path)]
            sz = (hit[0]['stage'][2] - hit[0]['stage'][0], hit[0]['stage'][3] - hit[0]['stage'][1]) if hit and hit[0]['stage'] else None
            say(bool(sz) and max(sz) < CLOSE_HIT, 'the PC close button %s: %s px drawn' % ('.'.join(path), '%.0f x %.0f' % sz if sz else 'NOT FOUND'))
        elif o == 'dmSide_p.swf':
            say(b.head[:-4] == rect_bytes(SIDE_W, SIDE_H), 'frame %dx%d' % (SIDE_W, SIDE_H))
            say(all((s[0].encode() + b'\0') in names for s in SIDE) and all(0 <= p[1][0] and p[1][2] <= SIDE_W and p[1][3] <= SIDE_H for p in rep['placed']), 'all %d buttons on the root, inside the frame' % len(SIDE))
            t = dis('onEventInit'); say("'registerAnchorId'" in t and "'topright'" in t and "'screen'" in t and 'dmHook' in t, 'onEventInit: anchor topright / screen / topright, then the hook')
            bot = max(p[1][3] for p in rep['placed']); right = min(SIDE_W - p[1][2] for p in rep['placed'])
            say(bot == 896 and SIDE_W - _sx(1348) == 92, 'the iPad places: stack bottom %d (TS_SIDE_BOT 896), %d px from the right edge, column edge %d (TS_SIDE_COL 92)' % (bot, right, SIDE_W - _sx(1348)))
            t = dis('dmBAct'); say("'dmSide'" in t and 'dmRoll' in t, 'tap: helmet -> the column shown / hidden; others -> dmSide(code)')
    return bad


def Movie_find(swf, cls):
    """the character id of a symbol class (e.g. actionsButton) or None"""
    try: return gr.Movie(swf).resolve(cls)
    except KeyError: return None


def mock(src, path, W=2400, F=None, made=None):
    """the computed layout: the movies' own symbols drawn where the patched code puts them, on a W x 1080 screen (not a played movie).
    F = the host's factors (dm/host/hle_dos2_touch_scale.h; default all 1): hb (hot bar + status console), mm (minimap), side (dmSide_p), pi
    (portraits, over their class scale 1.1); every object scales about its anchor as the engine's fixed layout does. -> the picture"""
    import numpy as np
    from PIL import Image, ImageDraw
    import dos2_swf_render as rn
    F = dict(dict(hb=1.0, mm=1.0, side=1.0, pi=1.0), **(F or {}))
    canvas = Image.new('RGBA', (W, 1080), (38, 44, 40, 255))
    def put(swf, what, x, y, sx=1.0, sy=None, crop=None, flip=False):
        sy = sx if sy is None else sy
        img, org = rn.render(swf, what, 1.0)
        if flip: bb = img.getbbox(); img = img.crop(bb).transpose(Image.FLIP_LEFT_RIGHT); org = (0, org[1] + bb[1])   # the drawing only (its 0.. box), mirrored
        if crop:   # (x0, y0, x1, y1) in the character's own px
            img = img.crop((int(crop[0] - org[0]), int(crop[1] - org[1]), int(crop[2] - org[0]), int(crop[3] - org[1]))); org = (crop[0], crop[1])
        img = img.resize((max(1, int(img.size[0] * sx)), max(1, int(img.size[1] * sy))), Image.LANCZOS)
        canvas.alpha_composite(img, (int(round(x + (0 if crop else org[0] * sx))), int(round(y + (0 if crop else org[1] * sy)))))
    def root_ids(swf):
        return {p['name'][:-1].decode(): p['char'] for p in (gr.place_parse(c, t) for c, t, _ in swf.tags if c in (4, 26, 70)) if p.get('name')}
    made = made or build(src)
    def font(px):
        from PIL import ImageFont
        for nm in ('arialbd.ttf', 'DejaVuSans-Bold.ttf'):
            try: return ImageFont.truetype(nm, int(px))
            except OSError: pass
        return ImageFont.load_default()
    def text(box, s, px, align='c'):   # the engine draws the words ($Title font); here an approximation with the PC stroke
        dr = ImageDraw.Draw(canvas); fo = font(px); w = dr.textlength(s, font=fo)
        x = box[0] + {'c': (box[2] - box[0] - w) / 2.0, 'r': box[2] - box[0] - w, 'l': 0}[align]
        dr.text((x, (box[1] + box[3]) / 2.0), s, font=fo, fill=(255, 255, 255, 255), anchor='lm', stroke_width=2, stroke_fill=(0, 0, 0, 255))
    def strip(swf, cid, names):   # a copy whose sprite `cid` no longer places these named children (what the code moves / hides at run time)
        s2 = Swf(swf.write())
        for k, (c, d, x) in enumerate(s2.tags):
            if c == 39 and struct.unpack_from('<H', d, 0)[0] == cid:
                out = d[:4]
                for sc_, sd in gr.subtags(d):
                    nm = gr.place_parse(sc_, sd).get('name') if sc_ in (26, 70) else None
                    if nm and nm[:-1].decode() in names: continue
                    out += gr.tag_bytes(sc_, sd)
                s2.tags[k][1] = out
        return s2
    lang = F.get('lang', 'ru')
    # hot bar (stage 1948 x 1000, anchor bottom): screen = (W / 2 + (x - 974) f, 1080 - (1000 - y) f)
    f = F['hb']; hx = lambda x: W / 2.0 + (x - 974) * f; hy = lambda y: 1080 - (1000 - y) * f
    hb = Swf(made['hotBar_p.swf']); ids = root_ids(hb)
    hb2 = strip(hb, ids['hotbar_mc'], {'lockButton_mc', 'actionsButton_mc', 'chatBtn_mc', 'hotkeys_mc'})
    # the deck number's plate (dmHook draws it under cycleHotBar_mc: its bounds 300.6..350.6 x 64..113 own px + DECK_PAD)
    dx0, dy0, dx1, dy1 = 304.3 + DECK_X, 64 - DECK_PAD[2], 304.3 + DECK_X + DECK_W, 113 + DECK_PAD[2]
    put(hb2, ids['hotbar_mc'], hx(HB_X), hy(HB_Y), _s * f, crop=(CROP, 0, CROP + VISW, VISH))
    ImageDraw.Draw(canvas).rectangle([hx(HB_X + (dx0 - CROP) * _s), hy(HB_Y + dy0 * _s), hx(HB_X + (dx1 - CROP) * _s), hy(HB_Y + dy1 * _s)], fill=(30, 24, 18, 230), outline=(122, 94, 58, 255), width=2)
    put(hb2, ids['hotbar_mc'], hx(HB_X), hy(HB_Y), _s * f, crop=(dx0 + 2, 0, dx1 - 2, VISH))   # the number over its plate (as the movie draws it)
    am = Movie_find(hb, 'actionsButton')
    if am: put(hb, am, hx(HB_X + (ACT_BTN[0] - CROP) * _s), hy(HB_Y + ACT_BTN[1] * _s), _s * f)
    for nm, box in (('dmChat_mc', CHAT_BOX), ('dmLog_mc', LOG_BOX)): put(hb, ids[nm], hx(box[0]), hy(box[1]), f)
    if F.get('decks'):
        put(hb, ids['dmDeckBg_mc'], hx(DECK_BG[0]), hy(DECK_BG[1]), f)
        for k in range(1, 6): put(hb, ids['dmDeck%d_mc' % k], hx(DECK_LIST[0] + DECK_LIST[3] * (k - 1)), hy(DECK_LIST[1]), f)
    for k in range(1, 6): put(hb, ids['dmInd%d_mc' % k], hx(IND[0] + IND[4] * (k - 1)), hy(IND[1]), f)
    if F.get('aim', True):
        cs = CONFIRM[2]
        put(hb, ids['dmConfirm_mc'], hx(CONFIRM[0] - cs / 2.0), hy(CONFIRM[1] - cs / 2.0), f)
        text((hx(CONFIRM[0] - 150), hy(CONFIRM[1] - 15), hx(CONFIRM[0] + 150), hy(CONFIRM[1] + 15)), CONF_RU if lang == 'ru' else CONF_EN, LBL_FONT * f)
        put(hb, ids['dmCancel_mc'], hx(CANCEL[0]), hy(CANCEL[1]), f)
        hx0, hy0, hw, hk = HINT
        put(hb, ids['dmHint_mc'], hx(hx0 - hw / 2.0), hy(hy0), f)
        text((hx(hx0 - hw / 2.0 + 48 * hk), hy(hy0), hx(hx0 + hw / 2.0 - 48 * hk), hy(hy0 + 64 * hk)), HINT_RU if lang == 'ru' else HINT_EN, HINT_FONT * f)
    # status console (stage 1920 x 1080, anchor bottom)
    sc = Swf(made['statusConsole_p.swf']); ids = root_ids(sc); mv = gr.Movie(sc)
    sx = lambda x: W / 2.0 + (x - 960) * f; sy = lambda y: 1080 - (1080 - y) * f
    ox, oy = SC_ORG
    put(sc, mv.resolve('healthBarHolder'), sx(SC_HB[0]), sy(SC_HB[1]), SC_HB[2] * f, SC_HB[3] * f)
    for (nm, x, sg), cls in zip(SC_ARM, ('armourBarHolderPhys', 'armourBarHolder')):
        put(sc, mv.resolve(cls), sx(x - (198 * SC_ARM_S[0] if sg < 0 else 0)), sy(SC_ARM_TOP), SC_ARM_S[0] * f, SC_ARM_S[1] * f, flip=sg < 0)
    if F.get('combat', True):   # out of combat (explore: the host's setBtnVisible(SC_EXPLORE)) the bars only
        put(sc, mv.resolve('apBar_10'), sx(ox - 81), sy(SC_AP_Y), f)
        # the turn plaque: its art is attached at run time (scalableBG_22.setMovieClips) - an approximation of its box (43 high, centred on its y)
        ImageDraw.Draw(canvas).rounded_rectangle([sx(960 - 230), sy(SC_NOTICE_Y - SC_NOTICE_HALF), sx(960 + 230), sy(SC_NOTICE_Y + SC_NOTICE_HALF)], radius=6, fill=(14, 12, 22, 235), outline=(70, 160, 170, 255), width=2)
        text((sx(960 - 200), sy(SC_NOTICE_Y - SC_NOTICE_HALF), sx(960 + 200), sy(SC_NOTICE_Y + SC_NOTICE_HALF)), 'ВАШ ХОД' if lang == 'ru' else 'YOUR TURN', 18 * f)
        put(sc, mv.resolve('endTurnButton'), sx(SC_END_X), sy(SC_BTN_Y), f)
        text((sx(SC_END_X + 20), sy(SC_BTN_Y), sx(SC_END_X + 184), sy(SC_BTN_Y + SC_BTN_H)), 'КОНЕЦ ХОДА' if lang == 'ru' else 'END TURN', SC_FONT * f)
        for nm, code, x, en, ru in SC_PLATES:
            put(sc, ids[nm], sx(x), sy(SC_BTN_Y), f)
            text((sx(x + 8), sy(SC_BTN_Y), sx(x + SC_PLATE_W - 8), sy(SC_BTN_Y + SC_BTN_H)), ru if lang == 'ru' else en, SC_FONT * f)
    # minimap (stage MM_W wide, anchor topright)
    f = F['mm']; mw = MM_W
    mm = Swf(made['minimap_p.swf']); ids = root_ids(mm)
    at = {p['name'][:-1].decode(): gr.Bits(p['matrix'], 0).matrix() for p in (gr.place_parse(c, t) for c, t, _ in mm.tags if c in (4, 26, 70)) if p.get('name') and p.get('matrix')}
    for name, dx, dy in MM_MOVE:
        if name in ('hit_mc', 'tension_mc', 'armourDoll_mc', 'tensionHit_mc', 'title_txt', 'diplomacy_mc', 'north_mc'): continue
        m = at[name]; put(mm, ids[name], W - (mw - (m[4] / 20.0 + dx)) * f, (m[5] / 20.0 + dy) * f, f)
    # the view menu, drawn from its parts as the movie shows it (F['arc']: open with the names; F['on']: the host's state bits)
    arc, on_bits = F.get('arc', True), F.get('on', 1 | 4)
    def asset_img(name, size):
        im = Image.fromarray(ta.asset(name)); return im.resize((max(1, int(im.size[0] * size)), max(1, int(im.size[1] * size))), Image.LANCZOS)
    for name, code, icon, cx, cy, size, bit in MM_BTNS:
        if name in MM_ARC and not arc: continue
        k = size * f / (ta.asset('plate_round_copper').shape[1] * ta.PX); x0, y0 = W - (mw - (cx - size / 2.0)) * f, (cy - size / 2.0) * f
        canvas.alpha_composite(asset_img('plate_round_copper', k * ta.PX), (int(round(x0)), int(round(y0))))
        ia = ta.asset(icon); ki = size * GLYPH * f / (max(ia.shape[:2]) * ta.PX)
        ic = asset_img(icon, ki * ta.PX); lit = (on_bits & bit) if bit else (code == 20 and arc) or (code == 21 and arc)
        if bit and not lit: ic.putalpha(Image.fromarray((np.asarray(ic)[..., 3] * 0.55).astype(np.uint8)))
        canvas.alpha_composite(ic, (int(round(x0 + (size * f - ic.size[0]) / 2.0)), int(round(y0 + (size * f - ic.size[1]) / 2.0))))
        if lit: canvas.alpha_composite(Image.fromarray(on_art(size)).resize((int(size * f), int(size * f)), Image.LANCZOS), (int(round(x0)), int(round(y0))))
    if arc:
        for lname, tname, en, ru in MM_LBL:
            tb = next(b2 for b2 in MM_BTNS if b2[0] == tname); x1 = tb[3] - tb[5] / 2.0 - 10; x0 = x1 - MM_LBL_W; y0 = tb[4] - MM_LBL_H / 2.0
            canvas.alpha_composite(Image.fromarray(label_art(MM_LBL_W, MM_LBL_H)).resize((int(MM_LBL_W * f), int(MM_LBL_H * f)), Image.LANCZOS), (int(W - (mw - x0) * f), int(y0 * f)))
            text((W - (mw - x0 - 14) * f, y0 * f, W - (mw - x1 + 14) * f, (y0 + MM_LBL_H) * f), ru if lang == 'ru' else en, MM_LBL_FONT * f, 'r')
    if F.get('hits'):   # the hit area of the view menu (dmMmHitDraw: the sector while open) and its nearest-button cells, for the review
        import math as _m
        sx2 = lambda x: W - (mw - x) * f
        a0, a1 = MM_HIT_DEG; pts = [(MM_RING[0] + r * _m.cos(_m.radians(a)), MM_RING[1] + r * _m.sin(_m.radians(a))) for r, a in
            [(MM_HIT_R, a0 + (a1 - a0) * k / 30.0) for k in range(31)] + [(MM_RING_R, a1 - (a1 - a0) * k / 30.0) for k in range(31)]]
        dr = ImageDraw.Draw(canvas); dr.polygon([(sx2(x), y * f) for x, y in pts], outline=(255, 220, 80, 255))
        vis = [b2 for b2 in MM_BTNS if arc or b2[0] not in MM_ARC]
        for yy in range(0, int(MM_RING[1] + MM_HIT_R), 4):
            for xx in range(int(MM_RING[0] - MM_HIT_R), int(MM_RING[0] + 10), 4):
                rr = _m.hypot(xx - MM_RING[0], yy - MM_RING[1]); aa = _m.degrees(_m.atan2(yy - MM_RING[1], xx - MM_RING[0])) % 360
                if not (MM_RING_R <= rr <= MM_HIT_R and a0 <= aa <= a1): continue
                d2 = sorted(((xx - b2[3]) ** 2 + (yy - b2[4]) ** 2, b2[0]) for b2 in vis)
                if d2[0][0] > MM_PICK_R ** 2: continue
                if len(d2) > 1 and abs(d2[0][0] ** 0.5 - d2[1][0] ** 0.5) < 3: dr.point((sx2(xx), yy * f), fill=(255, 220, 80, 255))
    # the right side (stage SIDE_W x 1080, anchor topright)
    f = F['side']; sd = Swf(made['dmSide_p.swf']); ids = root_ids(sd)
    for name, code, icon, x, y, size in SIDE: put(sd, ids[name], W - (SIDE_W - x) * f, y * f, f)
    # portraits (anchor topleft, class scale 1.1): one portrait drawn; the box is a party of four (TS_PI_W x TS_PI_BOT, an estimate)
    f = F['pi'] * 1.1; pi = Swf(made['playerInfo_p.swf'])
    try: put(pi, 'playerInfo', 25 * f, 30 * f, f)
    except Exception as e: print('portrait symbol not drawn: %s' % e)
    dr = ImageDraw.Draw(canvas)
    dr.rectangle([int(25 * f), int(30 * f), int(180 * F['pi']), int(720 * F['pi'])], outline=(120, 160, 200, 255), width=2)
    if path: canvas.save(path); print('%s: %dx%d (computed layout, factors %s)' % (path, W, 1080, ', '.join('%s %.3f' % kv for kv in sorted(F.items()) if kv[0] in ('hb', 'mm', 'side', 'pi'))))
    return canvas


# trade / container / dialog / reward / waypoints / item splitter in the iPad's layout: tools/dos2_ppanel_trade.py adds its movies to sources() / build() / check()
import dos2_ppanel_trade; dos2_ppanel_trade.install(sys.modules[__name__])
# iPad functions the Switch classes have no handler for (trade long press, container context menu): tools/dos2_ptouch_gaps.py patches the built movies
import dos2_ptouch_gaps; dos2_ptouch_gaps.install(sys.modules[__name__])
# the loading screen's touch tips (loadingScreen_p: ours before every second engine tip): tools/dos2_ptouch_tips.py adds its movie
import dos2_ptouch_tips; dos2_ptouch_tips.install(sys.modules[__name__])
# the PC tooltip window as one self-contained movie (tooltip_p: tooltip.swf + its library tooltipHelper_kb.swf inside, own class names, no
# global library registration): tools/dos2_ptooltip.py adds its movie
import dos2_ptooltip; dos2_ptooltip.install(sys.modules[__name__])
# the tutorial window (tutorialBox_p: a tap on the whole pop-up / bubble = its X, on the modal box = OK): tools/dos2_ptutorial.py adds its movie
import dos2_ptutorial; dos2_ptutorial.install(sys.modules[__name__])
# the item context menu (contextMenu_p: the engine's entries + "Description" at the bottom when an item window opened it; the host shows the
# item's tooltip): tools/dos2_pctxmenu.py adds its movie
import dos2_pctxmenu; dos2_pctxmenu.install(sys.modules[__name__])


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == 'build':
        out = Path(a[2]); out.mkdir(parents=True, exist_ok=True)
        for n, dta in build(sources(a[1])).items(): (out / n).write_bytes(dta); print('%s: %d B' % (out / n, len(dta)))
        return
    if len(a) == 2 and a[0] == 'check': sys.exit(1 if check(sources(a[1])) else 0)
    if len(a) >= 3 and a[0] == 'mock': mock(sources(a[1]), a[2], int(a[a.index('--w') + 1]) if '--w' in a else 2400); return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
