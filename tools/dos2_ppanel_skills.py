"""[DOS2] UI mode TOUCH, panels in the iPad's layout: skills, crafting, journal + map (docs/dos2/TOUCH_MODES_PLAN.md "Implemented: iPad layout -
skills / crafting / journal / map"). The player's own PC movies (Switch Game.pak skills.swf, uiCraft.swf, journal.swf) become skills_p.swf,
uiCraft_p.swf, journal_p.swf (the names tools/dos2_pchud_touch.py and the host table k_touch already use; this module replaces its "close
button only" pass for these three). No iPad file is read: only numbers of docs/dos2/TOUCH_LAYOUT.md (sections 5) are used; the art is the PC
movies' own. Static only: nothing here was played or run on a phone.

The map: the PC engine has no map movie of its own - the map is the journal's map tab (main+0x10cc7c0, the hot bar button "map" / the
minimap tap: journal object of the player record, tab 3, "UI_Game_Map_Open"), so journal_p IS the map window.

Why containers are scaled and not the cells re-drawn: in all three movies the item / skill icons are drawn by the ENGINE (Iggy custom draw into
placeholders: iggy_grid / iggy_memory, inventoryClass m_IggyImage "inventoryIcons", iggy_map), at places the engine computes from root
variables it reads once (main+0x13ba5c8 skillSize / skillSpacing / amountOfColumns ..., main+0x131b7bc invCellSize / invCellSpacing / invCols /
invRows / invRuneCols ...). Scaling the AS3 container that holds a grid scales its cells, its placeholder and the engine's icons together, so
the engine contract stays exactly the PC's. Only column counts change (amountOfColumns, invCols, invRuneCols): the engine reads them and lays
its icons out by them, as it does for the iPad's own values (6 / 13 / 6).

Common rules (the touch HUD of tools/dos2_pchud_touch.py stays visible over the panels: hot bar layer 0xf / 0x10 over panel layer 0xa, as on
the PC, where skills are dragged from the book onto the hot bar) - every window is centred and ends above the hot bar (screen y <= ~960 at a
height of 1080; the iPad's windows are 1160 x 1040..1080 centred, the iPad's hot bar 967..1049). Sizes below are design px of a 1080-high screen.

  skills_p    skills.swf (book 1376 x 1080, centred): the book x0.95 wide / x0.88 high (1307 x 950, screen y 8..958; iPad window 1160),
              the pages' content x1.159 / x1.252 inside it (the inverse: icons stay square): skill icon 69 -> 76 on screen, pitch 73 -> 80
              (skillSpacing 4; iPad 76 / 84); 6 columns per page like the iPad (21.11; 5 before: six did not fit the page at a uniform x0.88).
              Touch as the iPad: tap a skill = select + tooltip (the PC's single click learned / memorised at once), DOUBLE tap = learn /
              memorise / forget (the PC click; skillSlot / memorySegment onUp gated), drag a skill = the PC's startDragging to the hot bar or the
              memory page (also after a hold: the host's TOUCH rule, hle_dos2_touch.c dt_panel_release), two fingers = the lists' wheel (host).
              Finger pads: close, "all schools" and the character arrows (see "Element scale-up" below).
  uiCraft_p   uiCraft.swf (680 x 1060 at the top right): centred (setPosition center / screen / center, also on resize), WIDENED to the
              iPad's one-column window (21.11, craft_art): every background bitmap re-cut (two plain column bands stretched; the header keeps
              its end blocks and tab holes whole and gets two wings), every child moved with the art (left / middle / right third) -> 1311 x 1060
              own px, x0.9 = 1180 x 954 px (iPad 1160 x 1080), stage 1220; the bag and the rune inventory x1.689 inside: cell 50 -> 76 on screen,
              pitch 56 -> 85 (invCellSpacing 6; iPad 76 / 86); invCols 13 x 5 rows visible (iPad 13 x 6), invRuneCols 6 (iPad 6) - engine
              variables, so the engine's icons follow. The recipe list scrolls by one finger. The rune tab and the close are now 309 px apart.
              Finger pads: close, character arrows, -/+, search / clear (side by side: taller only), the rune popup's close, the recipe
              categories' -/+. Everything else is the PC's mouse code (tap, double click = use, drag ingredients, right click = context menu
              by the host's long press).
  journal_p   journal.swf (1360 x 1064, centred; the map is its tab 3): x0.88 (1197 x 936, screen y 8..944; iPad 1160 x 1040); quest rows
              30 + JN_ROWPAD (45 on screen = the iPad's; same text, the list keeps its width: a scaled list would clip the one-line titles); all
              five lists and the tutorial page's text field scroll by one finger on the text (rows are not draggable). Map: one finger pans (the PC's own drag -> mapOffset), pinch = zoom (the
              host's two fingers = wheel -> zoomIn / zoomOut), tap does NOT add a flag any more (the PC's click did), a long press (the host's
              right click) adds the flag where the finger is (iPad: LongPress = addFlag); zoom buttons 75 px. Finger pads: close, the two
              bottom buttons, the "postponed" / "tutorial pop-ups" check boxes.

Element scale-up (owner 23.11, a 6.7" 2400 x 1080 phone, UI mode touch: "the close X and many labels are tiny; make them finger-sized - not
by scaling the whole window, scale the elements"; the host's physical scale there: 1 mm = 15.4 px). Screen px at a height of 1080:
  close buttons   drawn 50 x 50 (the inventory's INV_CLOSE; PC 20..21), top-right corner kept where the PC had it (grows into the
                  window), pad 96 (skills, journal, crafting and the crafting rune popup).
  small targets   scaled about anchors that keep them clear of their neighbours (*_EL): skills character arrows x1.3 outwards, "all
                  schools" x1.25 down from its top (+ text 16 -> 18), school filter icons x1.3 (52 x 48; dmFilterFit after every
                  updateSkills: shrinks back while the row is wider than SK_FMAXW); crafting character arrows x1.3 outwards, amount -/+ x2
                  (17 -> 34), search / clear x1.3 about their joint, recipe categories' -/+ x1.6 (27) with 75 x 56 pads (CraftCategory.init);
                  journal bottom buttons x1.2 about their joint (inside their bar art), check boxes x1.25, map zoom 53 -> 75. Pads (*_PAD,
                  screen px, converted to the element's own px): >= 64 x 64, aimed at 75; search / clear 47 wide (as drawn), "all schools"
                  50 high (it hangs under the icons).
  text            the key fields' font heights raised in the movie's DefineEditText (*_TXT; runtime rows of library symbols get it too):
                  skills page headers 26 -> 32, school / memory list headers 20 -> 24 (x1.25 of the pages: 26 on screen); journal quest
                  list 20 -> 24 (21 on screen; one line still fits entryHeight 30, the const the rows and highlight are cut by), quest
                  text 20 -> 26 (23), quest title 20 -> 27, tutorial page 18 -> 24 / title 27, tutorial list 18 -> 24, tabs 16 -> 21;
                  crafting recipe list 18 -> 24 (22 on screen: a recipe row is now two elemSpacing = 56 px, a finger row; the label is
                  centred in it, the category header too), tabs 14 -> 19, buttons 14 -> 17. Single-line fields grow their box up and down
                  (text centred where it was), a few are widened (school names 150 -> 300, "Schools" 125 -> 200).

  python -I tools/dos2_ppanel_skills.py check <Switch Game.pak | dir of the PC movies>
  python -I tools/dos2_ppanel_skills.py build <src> <out dir>
  python -I tools/dos2_ppanel_skills.py mock  <src> <out dir> [--bg <hud mock png>] [--ipad <dir of the iPad _t movies>]   computed layout
        pictures (the static art of the patched movie at the positions the added code sets + the cell grids where the engine / AS3 put them,
        blue, and the finger pads, orange; not a played movie) on a 2400 x 1080 screen, over --bg (tools/dos2_pchud_touch.py mock: the touch
        HUD); --ipad also draws the iPad movie's static root at its place on the same background (a separate picture), for a look at both
Hook: tools/dos2_pchud_touch.py takes MAKE (build) and check_one (its `check`) from here for these three movies.
"""
import hashlib, os, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, NS_PUBLIC
import dos2_swf_graft as gr

GUI = 'Public/Game/GUI/'
OUT = {'skills_p.swf': 'skills.swf', 'uiCraft_p.swf': 'uiCraft.swf', 'journal_p.swf': 'journal.swf'}
TAP2_MS = 450        # two taps on the same element within this = a double tap (the host sends the second one where the first was, <= 300 ms apart)
SCROLL_SLOP = 8      # list-local px of finger travel before a press on a list becomes a scroll (the host already waited for ~27 screen px)

# ---- skills (stage 1354 x 1120, "fixed", anchored center / screen / center: stage y = screen y + 20)
# 21.11 (owner: 6 columns like the iPad): six 76 px icons do not fit a PC page at the book's x0.88, and the book cannot be taller (it ends above
# the hot bar), so the book is scaled x0.95 sideways and x0.88 up (1307 x 950 px; the art 8 % wider) and the pages' content by the inverse
# (SK_KX / SK_KY): the icons stay square, 76 px, at a pitch of 80 px (skillSpacing 5 -> 4; iPad 84)
SK_S = (88, 100)                      # the book's vertical scale (skillPane_mc and its root siblings)
SK_SX = 0.95                          # ... and its horizontal one
SK_X0, SK_Y0 = (1354 - 1376 * SK_SX) / 2 + 20 * SK_SX, 45.6   # pane content -20..1356 x -20..1060 -> centred, top at screen 8
SK_K = 76.0 / (69 * 0.88)             # the pages' content, vertically: the PC's skill icon 69 -> 76 screen px
SK_KX = 76.0 / (69 * SK_SX)           # ... horizontally
SK_COLS, SK_SPACING = 6, 4            # the iPad's 6 columns; skillSpacing (engine: main+0x13ba6c8) 5 -> 4
SK_PITCH = 69 + SK_SPACING
SK_LW, SK_LH = 18 + (SK_COLS - 1) * SK_PITCH + 69 + 10, 477   # list frame (list-local px): 6 columns + the memory scroll bar
SK_LX, SK_RX, SK_Y = 130, 704, 322    # the two pages' list origins (pane px)
SK_SBLEN = 349
# ---- element scale-up (owner 23.11, 6.7" 2400 x 1080 phone, UI mode touch: "the close X and many labels are tiny - make them finger-sized,
# but NOT by scaling the whole window"). The windows keep their size; single elements grow inside them:
#   the close button: drawn CLOSE_PX on screen (= the inventory's, tools/dos2_ppanel_inv.py INV_CLOSE; iPad mcBtnCloseTouch 50), its top-right
#     corner kept where the PC had it (it grows into the window, never out of it), finger pad PAD_CLOSE;
#   other small tap targets: drawn x1.2..x2 about an anchor that keeps them clear of their neighbours (each row below says which), finger
#     pads (transparent rectangles in the element's own graphics, centred on its drawing) of >= PAD_MIN screen px where neighbours allow;
#   text: the font height of the key fields raised in the movie's own DefineEditText (the field's default format: every runtime instance of
#     a library row gets it; the PC code sizes rows from textHeight, so lists follow), single-line fields grow their box (centred titles /
#     labels grow up and down, a few are widened so longer words do not clip); see *_TXT.
# The host's physical scale on that phone: 70.1 mm per 1080 px, 1 mm = 15.4 px; 50 px = 3.2 mm (the iPad's pad minimum), PAD_MIN 75 px = 4.9 mm.
CLOSE_PX, PAD_CLOSE, PAD_MIN = 50, 96, 75
SK_WIN = (SK_SX, SK_S[0] / float(SK_S[1]))   # the book's (x, y) scale on the screen
# (path from the document, kx, ky, anchor x, anchor y in the parent's px[, dx]): x = ax + (x - ax) * kx + dx, scale *= k (after the PC init)
SK_EL = [(('skillPane_mc', 'Xclose_mc'), CLOSE_PX / (23 * SK_WIN[0]), CLOSE_PX / (23 * SK_WIN[1]), 1335, 41),   # top-right corner kept
         (('skillPane_mc', 'leftCycle_mc'), 1.3, 1.3, 635, 116),    # the character arrows grow outwards (the portrait between them stays clear)
         (('skillPane_mc', 'rightCycle_mc'), 1.3, 1.3, 719, 116),
         (('skillPane_mc', 'allBtn_mc'), 1.25, 1.25, 297, 157)]     # "all schools": grows down from its top (the school icons above it)
SK_FK, SK_FMAXW = 1.3, 520            # school filter icons 42 -> x1.3 (52 x 48 on screen), shrunk back while the row is wider than SK_FMAXW pane px
# finger pads, SCREEN px (w, h): converted to the element's own px through the window's and the element's scale
SK_PAD = [(('skillPane_mc', 'Xclose_mc'), PAD_CLOSE, PAD_CLOSE), (('skillPane_mc', 'allBtn_mc'), 100, 50),
          (('skillPane_mc', 'leftCycle_mc'), 64, 96), (('skillPane_mc', 'rightCycle_mc'), 64, 96)]   # arrows: 64 wide = the gap to the portrait
# text: (symbol class, instance names down to the field, new font height px, 'c' = single line centred vertically, new width px or None)
SK_TXT = [('skills_fla.skillPane_1', ('title1holder', 'title1_txt'), 32, 'c', None),   # page headers 26 (23 on screen) -> 32 (28)
          ('AllBtn', ('text_txt',), 18, 'c', None),                                      # "all schools" 16 -> 18 (x1.25 with its button: 20 on screen)
          ('skills_fla.skillPane_1', ('title2holder', 'title2_txt'), 32, 'c', None),
          ('skills_fla.skillPane_1', ('knownSchools_txt',), 24, 'c', 200), ('skills_fla.skillPane_1', ('deckManager_txt',), 24, 'c', None),
          ('skills_fla.skillPane_1', ('defaultDeck_txt',), 26, 'c', None), ('skills_fla.skillPane_1', ('occupiedMemory_mc', 'text_txt'), 24, 'c', None),
          ('skillList', ('title_txt',), 24, '', 300),       # school names over the lists 20 -> 24 (x1.252 of the pages: 26 on screen)
          ('skillList', ('memoryCost_mc', 'text_txt'), 22, '', None), ('memoryList', ('title_txt',), 24, '', 300)]
# ---- crafting: the iPad's uiCraft_t is the PC's one-column window made 1160 wide (recipes 1119, experiment 1104, bag 13 columns of 76 / 86).
# 21.11 (owner: "the item grid has 5 rows - better like the iPad"; "a tap on the runes hit the close"): the PC art is WIDENED, not scaled:
# every background bitmap is re-cut (tools/dos2_ppanel_inv.py pieces_sprite) - the panels' two plain column bands CR_BANDS stretch by CR_EXW / 2
# each, the header keeps its three tab holes whole and gets two wings (its title bar / dark wood rows stretched) between its end blocks and
# the holes - and every placed child moves with the art: left third 0, middle CR_EXW / 2, right third CR_EXW (cr_shift). The window is then
# x0.9 = 1180 x 954 px (the PC 680 x 1060 own px; height: it ends above the hot bar), the stage CR_STAGE_W wide, centred by the engine
# (setPosition center: the phone run put uiCraft_p at (screen - stage) / 2).
CR_S = (9, 10)
CR_EXW = 631                          # own px added to the width: (680 + 631) x 0.9 = 1180 screen px (iPad 1160)
CR_W = 680 + CR_EXW
CR_STAGE_W = 1220
CR_X0, CR_Y0 = (CR_STAGE_W - CR_W * 0.9) / 2, -2
CR_BANDS = ((140, 164), (516, 540))   # plain columns of every panel bitmap (experiment / recipes / runes / bottom edge): each stretched by CR_EXW / 2
CR_HEAD = dict(ends=(60, 620), rows=((0, 56), (56, 124), (124, 172)), col=(100, 124), mid_src=(132, 172))   # the header's wings (see craft_art)
CR_K = 76.0 / (50 * 0.9)              # cell 50 -> 76 screen px
CR_SPACING = 6                        # invCellSpacing 8 -> 6: pitch 56 own = 85 px (iPad 86), as the inventory
CR_PITCH = 50 + CR_SPACING
CR_COLS, CR_RUNE_COLS = 13, 6         # the iPad's 13 (bag) / 6 (runes)
CR_BAG = (CR_COLS * CR_PITCH + 4, 5 * CR_PITCH + 2)   # experiment bag frame (grid-local px): 13 x 5 rows of 56 (476 own: the PC's 461 + 15)
CR_BAG_Y = 537                        # its top (PC 544): 7 own px up; the last row ends 7 own px over the bottom edge art (drawn under it)
CR_RUNE = (CR_RUNE_COLS * CR_PITCH + 4, 190)   # rune inventory frame: 6 x 56; (480 - 105) / CR_K high
CR_SB = 34                            # own px of a grid's scroll bar beside its frame
CR_BAG_X = (CR_W - CR_BAG[0] * CR_K - CR_SB) / 2.0   # the bag centred in the widened panel
CR_RUNE_X = 4                         # the rune grid's holder in the 614 px rune popup (6 columns: 574 + the bar)
CR_WIN = (0.9, 0.9)
_RP = ('craftPanel_mc', 'recipesPanel_mc')
CR_EL = [(('craftPanel_mc', 'closeButton_mc'), CLOSE_PX / (23 * 0.9), CLOSE_PX / (23 * 0.9), 1292, 38),   # top-right corner kept
         (('craftPanel_mc', 'prevChar_mc'), 1.3, 1.3, 614.5, 147),  # the character arrows grow outwards, away from the portrait
         (('craftPanel_mc', 'nextChar_mc'), 1.3, 1.3, 698.5, 147),
         (_RP + ('min_mc',), 2.0, 2.0, 59, 981.5),                  # amount - / + (19 px: 17 on screen): x2, away from the amount between them
         (_RP + ('plus_mc',), 2.0, 2.0, 128, 981.5),
         (_RP + ('searchBtn_mc',), 1.3, 1.3, 1208, 188),            # search / clear touch each other: x1.3 about the joint
         (_RP + ('cancelBtn_mc',), 1.3, 1.3, 1208, 188),
         (_RP + ('filterName_txt',), 1.0, 1.0, 0, 0, -10), (_RP + ('filterHl_mc',), 1.0, 1.0, 0, 0, -10),   # the search text: room for them
         (('craftPanel_mc', 'runesPanel_mc', 'inventory_mc', 'close_mc'), CLOSE_PX / (23 * 0.9), CLOSE_PX / (23 * 0.9), 595, 18)]
CR_PAD = [(('craftPanel_mc', 'closeButton_mc'), PAD_CLOSE, PAD_CLOSE), (('craftPanel_mc', 'prevChar_mc'), 64, 84), (('craftPanel_mc', 'nextChar_mc'), 64, 84),
          (_RP + ('plus_mc',), PAD_MIN, PAD_MIN), (_RP + ('min_mc',), PAD_MIN, PAD_MIN),
          (_RP + ('searchBtn_mc',), 47, PAD_MIN), (_RP + ('cancelBtn_mc',), 47, PAD_MIN),
          (('craftPanel_mc', 'runesPanel_mc', 'inventory_mc', 'close_mc'), PAD_CLOSE, PAD_CLOSE)]
PAD_TIGHT = {'searchBtn_mc', 'cancelBtn_mc'}   # the recipe search / clear buttons touch each other (47 px drawn each): their pads only grow in height
PAD_LOW = {'allBtn_mc'}               # "all schools" hangs right under the school icons: its pad is wide, 50 high (taller would take the icons' bottom)
# recipe rows: the label 18 -> 24 makes a row two elemSpacing (31) high (62 own = 56 on screen, the PC 28: a finger-sized row, the iPad's 41 +
# padding); the label sits centred in it (y 2 -> CR_LABEL_Y). Category headers the same: title y 3 -> CR_CAT_Y, their -/+ (19 px) x CR_CAT_K
CR_LABEL_Y, CR_CAT_Y, CR_CAT_K, CR_CAT_BTN = 15, 16, 1.6, (29, 15.8)   # the header -/+ drawn at (x, y), 30 own px
CR_CAT_PAD = (PAD_MIN, 56)            # their finger pads (screen px; 56 = the header row)
CR_TXT = [('Recipe', ('label_txt',), 24, '', None), ('CraftCategory', ('title_txt',), 24, '', None),   # the recipe list 18 (16 on screen) -> 24 (22)
          ('Tab', ('text_txt',), 19, 'c', None), ('uiCraft_fla.craftPanel_1', ('title_txt',), 20, 'c', None),
          ('uiCraft_fla.craftPanel_1', ('charName_txt',), 19, '', None),
          ('uiCraft_fla.recipesPanel_4', ('name_txt',), 24, '', None), ('uiCraft_fla.recipesPanel_4', ('amount_txt',), 24, 'c', None),
          ('uiCraft_fla.recipesPanel_4', ('filterName_txt',), 18, 'c', None), ('uiCraft_fla.checkButtonLabel_27', ('text_txt',), 19, 'c', None),
          ('smallRedBtn', ('text_txt',), 17, 'c', None), ('smallBrownBtn', ('text_txt',), 17, 'c', None), ('smallGreenBtn', ('text_txt',), 17, 'c', None),
          ('uiCraft_fla.stationOverlay_12', ('stationText_txt',), 21, 'c', None),
          ('uiCraft_fla.experimentPanel_36', ('result_txt',), 26, '', None), ('uiCraft_fla.experimentPanel_36', ('currentStep_txt',), 24, '', None),
          ('uiCraft_fla.craftResult_39', ('noResult_txt',), 26, '', None),
          ('uiCraft_fla.RunesPanel_48', ('title_txt',), 22, 'c', None), ('uiCraft_fla.RunesPanel_48', ('name_txt',), 22, 'c', None),
          ('uiCraft_fla.RunesPanel_48', ('error_txt',), 22, '', None), ('uiCraft_fla.RuneInventory_54', ('title_txt',), 24, 'c', None),
          ('RuneSlot', ('title_txt',), 16, '', None), ('RuneSlot', ('name_txt',), 20, '', None), ('RuneSlot', ('desc_txt',), 18, '', None)]
# ---- journal (stage 1360 x 1128, anchored center: stage y = screen y + 24)
JN_S = (88, 100)
JN_X0, JN_Y0 = 81.6, 27.6             # journal_mc content 0..1360 x 5..1069 -> centred, top at screen 8
JN_ROWPAD = 21                        # quest rows: 30 -> 51 journal px (45 on screen = the iPad's; 40 before 21.11), same text size and width (scaling the list
                                      # would clip the titles: they are one autoSize line in a 400 px list)
JN_ZOOM = (1150, 812, 1240, 812, 75 / (43 * 0.88))   # map zoom in / out: x, y in map_mc px (PC 1216 / 1270, 838), 43 -> 85 (75 on screen; 53 before
                                      # 23.11), side by side in the map's bottom-right corner (its content ends at map y ~900)
JN_WIN = (0.88, 0.88)
JN_EL = [(('journal_mc', 'close_mc'), CLOSE_PX / (23 * 0.88), CLOSE_PX / (23 * 0.88), 1338, 42.5),   # top-right corner kept
         (('journal_mc', 'onMapBtn_mc'), 1.2, 1.2, 899, 993.9),    # the two bottom buttons x1.2 about their joint: 623..1175, inside their
         (('journal_mc', 'postponeBtn_mc'), 1.2, 1.2, 899, 993.9),  # bar art (bg_mc.btnBG_mc 619..1179 x 969..1021)
         (('journal_mc', 'showPostponed_mc'), 1.25, 1.25, 169, 996),   # check boxes x1.25 from their left end (box 24 -> 30, label 16 -> 18 x1.25)
         (('journal_mc', 'tutorialContainer_mc', 'showTutorialPopups_mc'), 1.25, 1.25, 145, 860)]
JN_PAD = [(('journal_mc', 'close_mc'), PAD_CLOSE, PAD_CLOSE), (('journal_mc', 'onMapBtn_mc'), 242, PAD_MIN), (('journal_mc', 'postponeBtn_mc'), 242, PAD_MIN),
          (('journal_mc', 'showPostponed_mc'), 407, PAD_MIN), (('journal_mc', 'tutorialContainer_mc', 'showTutorialPopups_mc'), 407, PAD_MIN)]
# quest rows keep entryHeight 30 (cLineHeight, a const): one line of a 24 px font fits it (27 would make the highlight two rows)
JN_TXT = [('QuestEntry', ('title_mc', 'name_txt'), 24, '', None),          # quest list 20 (18 on screen) -> 24 (21)
          ('QuestAction', ('objective_txt',), 26, '', None), ('QuestAction', ('name_txt',), 26, '', None),   # quest text 20 -> 26 (23)
          ('questFilter', ('title_txt',), 26, '', None),
          ('journal_fla.detailsTitle_28', ('title_txt',), 27, '', None), ('journal_fla.detailsTitle_28', ('subTitle_txt',), 24, '', None),
          ('journal_fla.tutorialHolder_34', ('title_txt',), 27, '', None), ('journal_fla.tutorialHolder_34', ('desc_txt',), 24, '', None),  # 18 -> 24
          ('TutorialEntry', ('title_txt',), 24, '', None), ('TutCategory', ('title_txt',), 24, '', None), ('TutCategory', ('amount_txt',), 20, '', None),
          ('DialogLine', ('line_txt',), 24, '', None), ('DialogLine', ('iline_txt',), 24, '', None),
          ('DialogEntry', ('title_mc', 'name_txt'), 22, 'c', None), ('DialogEntry', ('title_mc', 'dateTime_txt'), 22, 'c', None),
          ('DialogEntry', ('title_mc', 'level_txt'), 22, 'c', None),
          ('SecretEntry', ('title_mc', 'name_txt'), 24, '', None), ('SecretInfo', ('name_txt',), 22, '', None),
          ('stateButton', ('text_txt',), 21, 'c', None), ('button', ('text_txt',), 18, 'c', None), ('CheckBoxWlabel', ('text_txt',), 18, 'c', None),
          ('tabBtn', ('text_txt',), 21, 'c', None),
          ('LegendMC', ('label_mc', 'text_txt'), 21, '', None), ('journal_fla.legendHolder_13', ('title_txt',), 22, '', None),
          ('journal_fla.map_2', ('info_mc',), 22, '', None), ('journal_fla.journalMC_1', ('mapName_txt',), 32, '', None)]
JN_LISTS = ['activeList', 'closedList', 'dialogList', 'tutorialList', 'infoList']   # journalMC_1 properties (scrollList / scrollListGrouped)
JN_TEXTS = [('tutorialContainer_mc', 'desc_txt')]   # TextFields that scroll by themselves (the tutorial page's text, 450 x 600)
CR_LISTS = [('craftPanel_mc', 'recipesPanel_mc', 'recipeList')]


def sources(path):
    need = sorted(set(OUT.values()))
    if os.path.isdir(path): return {n: (Path(path) / n).read_bytes() for n in need}
    import lspk
    pak = lspk.Pak(path); by = {e[0]: e for e in pak.entries}
    miss = [n for n in need if GUI + n not in by]
    if miss: sys.exit('%s: no %s' % (path, ' '.join(miss)))
    return {n: pak.read(by[GUI + n]) for n in need}


# ================================================================ ABC: append-only patching of any class of a movie
HEAD = (('getlocal0',), ('pushscope',))


def num(v):
    v = int(v)
    # pushshort is sign-extended from 16 bits by the player (tools/dos2_ppanel_inv.py num, phone 23.11: 718500 ran as -2.396): never past 32767;
    # bigger numbers go through Movie.f (pushdouble)
    assert -32767 <= v <= 32767, 'num(%d): out of pushshort range, use Movie.f' % v
    if -128 <= v <= 127: return [('pushbyte', v & 0xff)]
    return [('pushshort', v)] if v >= 0 else [('pushshort', -v), ('negate',)]




class Movie:
    """one ABC block; classes are patched through Cls"""
    def __init__(s, data):
        s.swf = Swf(data); (s.tag, s.pre, s.abc), = s.swf.abcs()
        s.ns = s.abc.namespace(NS_PUBLIC, ''); s.made = []
        s.EI = s.abc.pub('ExternalInterface', 'flash.external')
    def P(s, name): return s.abc.qname(s.ns, name)
    def S(s, text): return s.abc.string(text)
    def cls(s, name): return Cls(s, name)
    def done(s):
        s.swf.tags[s.tag][1] = s.pre + s.abc.write()
        return s.swf.write()
    def ei(s, name, *args): return [('getlex', s.EI), ('pushstring', s.S(name))] + [x for a in args for x in a] + [('callpropvoid', s.P('call'), 1 + len(args))]
    def f(s, v):
        """a Number on the stack: (a, b) = a / b, or a float (pushdouble; the double is appended to the pool)"""
        if isinstance(v, tuple): v = v[0] / float(v[1])
        if float(v).is_integer() and -32768 < v < 32768: return num(v)
        b = struct.pack('<d', float(v))
        if b in s.abc.doubles[1:]: i = s.abc.doubles.index(b, 1)
        else: s.abc.doubles.append(b); i = len(s.abc.doubles) - 1
        return [('pushdouble', i)]
    def timer(s):
        q = s.abc.pub('getTimer', 'flash.utils'); return [('findpropstrict', q), ('callproperty', q, 0)]


class Cls:
    def __init__(s, mv, name):
        s.mv = mv; abc = mv.abc; s.abc = abc
        s.ci = abc.class_index(name); s.inst = abc.instances[s.ci]
        assert not s.inst['flags'] & 1, '%s is sealed' % name
        bodies = [abc.methods[t.b].body for t in s.inst['traits'] if t.kind & 15 in (1, 2, 3) and abc.methods[t.b].body]
        assert bodies, name
        s.ref = bodies[0]
        assert abc.trait(s.ci, 'dmHook') is None, '%s already patched' % name
    def add(s, name, nparams, lines, like=None):
        abc = s.abc
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 64, 64, s.ref.init_scope, s.ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        if like is not None:
            abc.methods[m].ret = like.ret
            if like.flags & 0x08: abc.methods[m].flags |= 0x08; abc.methods[m].options = list(like.options)
        s.mv.made.append(m); return m
    def method(s, name, nparams, lines):
        s.inst['traits'].append(Trait(s.mv.P(name), 1, 0, s.add(name, nparams, lines)))
    def wrap(s, name, keep, lines_of, kind=1):
        """trait `name` (1 method, 2 getter) -> a new body built by lines_of(n params); the original stays callable as the method `keep`"""
        hit = [t for t in s.inst['traits'] if s.abc.short(t.name) == name and t.kind & 15 == kind]
        assert len(hit) == 1, (name, kind)
        t = hit[0]; orig = s.abc.methods[t.b]; n = len(orig.params)
        m = s.add(name, n, lines_of(n), like=orig)
        old = t.b; t.b = m; s.inst['traits'].append(Trait(s.mv.P(keep), 1, 0, old))
    def mark(s):   # "already patched"
        s.inst['traits'].append(Trait(s.mv.P('dmHook'), 1, 0, s.add('dmHook', 0, list(HEAD) + [('returnvoid',)])))


def path_to(mv, path, loc, end):
    """lines: local `loc` = this.<path...>; jump to `end` when a link is missing"""
    c = [('getlocal0',), ('coerce_a',), ('setlocal', loc)]
    for p in path: c += [('getlocal', loc), ('getproperty', mv.P(p)), ('coerce_a',), ('setlocal', loc), ('getlocal', loc), ('iffalse', end)]
    return c


def pad_lines(mv, loc, g, b, w, h, lab):
    """the object in local `loc` gets a transparent w x h rectangle in its own graphics, centred on what it draws (its own px)"""
    P = mv.P
    c = [('getlocal', loc), ('getlocal', loc), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', b),
         ('getlocal', loc), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', g), ('getlocal', g), ('iffalse', lab),
         ('getlocal', g), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', P('beginFill'), 2), ('getlocal', g)]
    for pos, size, v in (('x', 'width', w), ('y', 'height', h)):
        c += [('getlocal', b), ('getproperty', P(pos)), ('getlocal', b), ('getproperty', P(size)), ('pushbyte', 2), ('divide',), ('add',)] + num(v // 2) + [('subtract',)]
    return c + num(w) + num(h) + [('callpropvoid', P('drawRect'), 4), ('getlocal', g), ('callpropvoid', P('endFill'), 0)]


class Lay:
    """a method body that walks paths from `this` and sets layout; locals 1 = object, 2 / 3 / 4 = scratch; labels unique"""
    def __init__(s, mv, nparams=0, pre=()):
        s.mv = mv; s.base = nparams; s.c = list(HEAD) + [x for k in range(1, 6) for x in (('pushnull',), ('coerce_a',), ('setlocal', nparams + k))] + list(pre); s.n = 0
    def L(s, k): return s.base + k
    def lab(s): s.n += 1; return 'l%d' % s.n
    def at(s, path, body):
        """run body (lines using local L(1) = the object) when this.<path> exists"""
        e = s.lab(); s.c += path_to(s.mv, path, s.L(1), e) + body + [('L', e)]
    def set(s, prop, val): return [('getlocal', s.L(1))] + val + [('setproperty', s.mv.P(prop))]
    def get(s, *props): return [('getlocal', s.L(1))] + [('getproperty', s.mv.P(p)) for p in props]
    def call(s, props, meth, *args):
        return s.get(*props) + [x for a in args for x in a] + [('callpropvoid', s.mv.P(meth), len(args))]
    def xform(s, path, x0, y0, sc, scy=None):
        """x = x0 + x * sc, y = y0 + y * scy, scaleX *= sc, scaleY *= scy (scy None = sc; the object's own transform is kept inside)"""
        P = s.mv.P; scy = sc if scy is None else scy
        b = []
        for p, o, k in (('x', x0, sc), ('y', y0, scy)):
            b += [('getlocal', s.L(1))] + s.mv.f(o) + [('getlocal', s.L(1)), ('getproperty', P(p))] + s.mv.f(k) + [('multiply',), ('add',), ('setproperty', P(p))]
        for p, k in (('scaleX', sc), ('scaleY', scy)):
            b += [('getlocal', s.L(1)), ('getlocal', s.L(1)), ('getproperty', P(p))] + s.mv.f(k) + [('multiply',), ('setproperty', P(p))]
        s.at(path, b)
    def place(s, path, x=None, y=None, sc=None, scy=None):
        b = []
        if sc is not None: b += s.set('scaleX', s.mv.f(sc)) + s.set('scaleY', s.mv.f(sc if scy is None else scy))
        if x is not None: b += s.set('x', s.mv.f(x))
        if y is not None: b += s.set('y', s.mv.f(y))
        s.at(path, b)
    def pad(s, path, w, h):
        e = s.lab(); s.at(path, pad_lines(s.mv, s.L(1), s.L(2), s.L(3), w, h, e) + [('L', e)])
    def scroll(s, path, doc_listener='dmScDown'):
        """one-finger drag scroll on the scrollList at path (the document's dmScDown / dmScMove / dmScUp)"""
        P = s.mv.P
        s.at(path, [('getlocal', s.L(1)), ('pushstring', s.mv.S('mouseDown')), ('getlocal0',), ('getproperty', P(doc_listener)),
                    ('callpropvoid', P('addEventListener'), 2)])
    def end(s): return s.c + [('returnvoid',)]


# ================================================================ element scale-up: static text sizes, runtime element scales, pads in screen px
def edit_info(d):
    """DefineEditText body -> dict(fh = font height px, rect = (x0, y0, x1, y1) px, align 0 left / 1 right / 2 centre, ww, ml, auto, fh_at = byte
    offset of the font height, rect_end = byte offset after the rect)"""
    r = gr.Bits(d, 2); x0, x1, y0, y1 = r.rect(); p = r.p
    f1, f2 = d[p], d[p + 1]; q = p + 2
    assert f1 & 1 or f2 & 0x80, 'DefineEditText %d has no font' % struct.unpack_from('<H', d, 0)[0]
    if f1 & 1: q += 2
    if f2 & 0x80: q = d.index(b'\0', q) + 1
    fh = struct.unpack_from('<H', d, q)[0] / 20.0; o = q + 2 + (4 if f1 & 4 else 0) + (2 if f1 & 2 else 0)
    align = d[o] if f2 & 0x20 else 0
    return dict(fh=fh, rect=(x0 / 20.0, y0 / 20.0, x1 / 20.0, y1 / 20.0), align=align, ww=bool(f1 & 0x40), ml=bool(f1 & 0x20), auto=bool(f2 & 0x40),
                fh_at=q, rect_end=p)


def _rect_bytes(x0, y0, x1, y1):
    v = [int(round(t * 20)) for t in (x0, x1, y0, y1)]; n = max(abs(t).bit_length() for t in v) + 1
    return gr.wbits([(n, 5)] + [(t, n) for t in v])


def _symbols(swf):
    """symbol class name -> character id (SymbolClass tags)"""
    out = {}
    for c, d, _ in swf.tags:
        if c != gr.T_SYMCLASS: continue
        n = struct.unpack_from('<H', d, 0)[0]; p = 2
        for _ in range(n):
            cid = struct.unpack_from('<H', d, p)[0]; name, p = gr.cstr(d, p + 2); out[name.decode()] = cid
    return out


def text_id(swf, cls, names, mvx=None, syms=None):
    """character id of the TextField reached from symbol `cls` through the instance names (frame 1 of each sprite)"""
    mvx = mvx or gr.Movie(swf); syms = syms or _symbols(swf); cid = syms[cls]
    for nm in names:
        code, d, _ = swf.tags[mvx.defs[cid]]; assert code == gr.T_SPRITE, (cls, names)
        hit = [r['char'] for r in gr.frame_state(d, 1).values() if r.get('name') and r['name'][:-1].decode() == nm]
        assert len(hit) == 1, 'no single %s in %s %s' % (nm, cls, names); cid = hit[0]
    assert swf.tags[mvx.defs[cid]][0] == gr.T_EDIT, (cls, names)
    return cid


def texts(data, table):
    """the PC movie with the font heights of `table` (*_TXT) raised in its DefineEditText tags -> (bytes, [(cls.names, id, old fh, new fh, old w, new w)]).
    A single-line field ('c') grows its box up and down by the extra line height (the text stays centred where the PC had it), any other
    grows down (multi-line / autoSize fields: the code sizes them); a new width widens the box to the right (left aligned) or both ways."""
    swf = Swf(data); mvx = gr.Movie(swf); syms = _symbols(swf); rep = []; done = {}
    for cls, names, fh, how, w in table:
        cid = text_id(swf, cls, names, mvx, syms); n = mvx.defs[cid]; code, d, lng = swf.tags[n]
        if cid in done: assert done[cid] == (fh, how, w), 'field %d listed twice with different sizes' % cid; continue
        done[cid] = (fh, how, w); e = edit_info(d); x0, y0, x1, y1 = e['rect']
        grow = (fh - e['fh']) * 1.2
        if how == 'c': y0 -= grow / 2.0; y1 += grow / 2.0
        else: y1 += grow
        if w:
            dw = w - (x1 - x0)
            if e['align'] == 2: x0 -= dw / 2.0; x1 += dw / 2.0
            else: x1 += dw
        body = d[:2] + _rect_bytes(x0, y0, x1, y1) + d[e['rect_end']:e['fh_at']] + struct.pack('<H', int(round(fh * 20))) + d[e['fh_at'] + 2:]
        swf.tags[n] = [code, body, lng]
        rep.append(('%s.%s' % (cls, '.'.join(names)), cid, e['fh'], fh, e['rect'][2] - e['rect'][0], x1 - x0))
    return swf.write(), rep


def unfont(pc, out):
    """the built movie with every DefineEditText the PC movie has put back to the PC's bytes (the check that nothing else changed)"""
    a = {struct.unpack_from('<H', d, 0)[0]: d for c, d, _ in Swf(pc).tags if c == gr.T_EDIT}
    b = Swf(out)
    for t in b.tags:
        if t[0] == gr.T_EDIT: t[1] = a.get(struct.unpack_from('<H', t[1], 0)[0], t[1])
    return b.write()


def el_xform(e):
    """(path, kx, ky, ax, ay[, dx]) -> (x0, y0, kx, ky): x = x0 + x * kx (Lay.xform)"""
    path, kx, ky, ax, ay = e[:5]; dx = e[5] if len(e) > 5 else 0.0
    return ax * (1 - kx) + dx, ay * (1 - ky), kx, ky


def el_scale(els, path):
    for e in els:
        if tuple(e[0]) == tuple(path): return e[1], e[2]
    return 1.0, 1.0


def pad_own(pad, win, els):
    """a finger pad of (path, w, h screen px) -> (w, h) in the element's own px (the window's scale x the element's)"""
    kx, ky = el_scale(els, pad[0])
    return int(round(pad[1] / (win[0] * kx))), int(round(pad[2] / (win[1] * ky)))


def apply_els(lay, els, pads, win):
    """the layout code: every element of els scaled about its anchor, then the pads (sizes in screen px)"""
    for e in els:
        x0, y0, kx, ky = el_xform(e); lay.xform(list(e[0]), x0, y0, kx, ky)
    for p in pads:
        w, h = pad_own(p, win, els); lay.pad(list(p[0]), w, h)


def doc_text_scroll(doc):
    """document methods dmTxDown(e) / dmTxMove(e) / dmTxUp(e): a finger on a long TextField (not in a scroll list) scrolls it line by line
    (scrollV = the line under the press - the finger's travel / the line height, clamped 1..maxScrollV) - owner 21.11: "the text scrolls by a
    finger on the text itself, not only by the bar"; the PC moved such fields with the mouse wheel only"""
    mv = doc.mv; P, S = mv.P, mv.S
    stage = [('getlocal0',), ('getproperty', P('stage'))]
    c = list(HEAD) + [('getlocal0',), ('getlocal1',), ('getproperty', P('currentTarget')), ('setproperty', P('dmTxT')),
        ('getlocal0',), ('getlocal1',), ('getproperty', P('currentTarget')), ('getproperty', P('mouseY')), ('setproperty', P('dmTxY')),
        ('getlocal0',), ('getlocal1',), ('getproperty', P('currentTarget')), ('getproperty', P('scrollV')), ('setproperty', P('dmTxV'))]
    for ev, fn in (('mouseMove', 'dmTxMove'), ('mouseUp', 'dmTxUp')):
        c += stage + [('pushstring', S(ev)), ('getlocal0',), ('getproperty', P(fn)), ('callpropvoid', P('addEventListener'), 2)]
    doc.method('dmTxDown', 1, c + [('returnvoid',)])
    # dmTxMove: locals 2 the field, 3 the line height, 4 the new line
    c = list(HEAD) + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushbyte', 0), ('convert_d',), ('setlocal', 3), ('pushbyte', 0), ('convert_d',), ('setlocal', 4),
        ('getlocal0',), ('getproperty', P('dmTxT')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('pushbyte', 0), ('callproperty', P('getLineMetrics'), 1), ('getproperty', P('height')), ('convert_d',), ('setlocal', 3),
        ('getlocal', 3), ('pushbyte', 1), ('ifnge', 'end'),
        ('getlocal0',), ('getproperty', P('dmTxV')), ('getlocal', 2), ('getproperty', P('mouseY')), ('getlocal0',), ('getproperty', P('dmTxY')), ('subtract',),
        ('getlocal', 3), ('divide',), ('subtract',), ('convert_d',), ('setlocal', 4),
        ('getlocal', 4), ('pushbyte', 1), ('ifnlt', 'lo'), ('pushbyte', 1), ('convert_d',), ('setlocal', 4), ('L', 'lo'),
        ('getlocal', 2), ('getlocal', 4), ('convert_i',), ('setproperty', P('scrollV')),
        ('L', 'end'), ('returnvoid',)]
    doc.method('dmTxMove', 1, c)
    c = list(HEAD)
    for ev, fn in (('mouseMove', 'dmTxMove'), ('mouseUp', 'dmTxUp')):
        c += stage + [('pushstring', S(ev)), ('getlocal0',), ('getproperty', P(fn)), ('callpropvoid', P('removeEventListener'), 2)]
    doc.method('dmTxUp', 1, c + [('getlocal0',), ('pushnull',), ('setproperty', P('dmTxT')), ('returnvoid',)])


def doc_scroll(doc):
    """document methods: dmScDown(e) / dmScMove(e) / dmScUp(e): a press on a list that travels > SCROLL_SLOP list px vertically scrolls it
    (scrollbar.scrolledY, clamped to getContDiff); while scrolling the list takes the mouse from its rows (mouseChildren off) so the release
    does not click the row under the finger; given back at the release"""
    mv = doc.mv; P, S = mv.P, mv.S
    stage = [('getlocal0',), ('getproperty', P('stage'))]
    c = list(HEAD) + [('getlocal0',), ('getlocal1',), ('getproperty', P('currentTarget')), ('setproperty', P('dmScL')),
        ('getlocal0',), ('getlocal1',), ('getproperty', P('currentTarget')), ('getproperty', P('mouseY')), ('setproperty', P('dmScY')),
        ('getlocal0',), ('getlocal1',), ('getproperty', P('currentTarget')), ('getproperty', P('m_scrollbar_mc')), ('getproperty', P('scrolledY')), ('setproperty', P('dmScS')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmScOn'))]
    for ev, fn in (('mouseMove', 'dmScMove'), ('mouseUp', 'dmScUp')):
        c += stage + [('pushstring', S(ev)), ('getlocal0',), ('getproperty', P(fn)), ('callpropvoid', P('addEventListener'), 2)]
    doc.method('dmScDown', 1, c + [('returnvoid',)])
    # dmScMove: locals 2 list, 3 dy, 4 target, 5 max
    c = list(HEAD) + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushbyte', 0), ('convert_d',), ('setlocal', 3), ('pushbyte', 0), ('convert_d',), ('setlocal', 4),
        ('pushbyte', 0), ('convert_d',), ('setlocal', 5),
        ('getlocal0',), ('getproperty', P('dmScL')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('getproperty', P('mouseY')), ('getlocal0',), ('getproperty', P('dmScY')), ('subtract',), ('convert_d',), ('setlocal', 3),
        ('getlocal0',), ('getproperty', P('dmScOn')), ('iftrue', 'go'),
        ('getlex', mv.abc.pub('Math')), ('getlocal', 3), ('callproperty', P('abs'), 1)] + num(SCROLL_SLOP) + [('ifngt', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmScOn')), ('getlocal', 2), ('pushfalse',), ('setproperty', P('mouseChildren')),
        ('L', 'go'),
        ('getlocal0',), ('getproperty', P('dmScS')), ('getlocal', 3), ('subtract',), ('convert_d',), ('setlocal', 4),
        ('getlocal', 2), ('getproperty', P('m_scrollbar_mc')), ('callproperty', P('getContDiff'), 0), ('convert_d',), ('setlocal', 5),
        ('getlocal', 4), ('getlocal', 5), ('ifngt', 'nmax'), ('getlocal', 5), ('setlocal', 4), ('L', 'nmax'),
        ('getlocal', 4), ('pushbyte', 0), ('ifnlt', 'nmin'), ('pushbyte', 0), ('convert_d',), ('setlocal', 4), ('L', 'nmin'),
        ('getlocal', 2), ('getproperty', P('m_scrollbar_mc')), ('getlocal', 4), ('setproperty', P('scrolledY')),
        ('L', 'end'), ('returnvoid',)]
    doc.method('dmScMove', 1, c)
    c = list(HEAD)
    for ev, fn in (('mouseMove', 'dmScMove'), ('mouseUp', 'dmScUp')):
        c += stage + [('pushstring', S(ev)), ('getlocal0',), ('getproperty', P(fn)), ('callpropvoid', P('removeEventListener'), 2)]
    c += [('getlocal0',), ('getproperty', P('dmScL')), ('iffalse', 'end'), ('getlocal0',), ('getproperty', P('dmScL')), ('pushtrue',), ('setproperty', P('mouseChildren')),
          ('getlocal0',), ('pushnull',), ('setproperty', P('dmScL')), ('L', 'end'), ('returnvoid',)]
    doc.method('dmScUp', 1, c)


def doc_tap(doc):
    """document dmTap(o): true when o got the previous tap too, within TAP2_MS (then forgotten: a third tap starts again)"""
    mv = doc.mv; P = mv.P
    c = list(HEAD) + [('pushbyte', 0), ('convert_d',), ('setlocal', 2)] + mv.timer() + [('convert_d',), ('setlocal', 2),
        ('getlocal0',), ('getproperty', P('dmTapO')), ('getlocal1',), ('ifstrictne', 'first'),
        ('getlocal', 2), ('getlocal0',), ('getproperty', P('dmTapT')), ('subtract',)] + num(TAP2_MS) + [('ifnlt', 'first'),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmTapO')), ('pushtrue',), ('returnvalue',),
        ('L', 'first'), ('getlocal0',), ('getlocal1',), ('setproperty', P('dmTapO')), ('getlocal0',), ('getlocal', 2), ('setproperty', P('dmTapT')),
        ('pushfalse',), ('returnvalue',)]
    doc.method('dmTap', 1, c)


def call_orig(keep, n): return [('getlocal0',)] + [('getlocal', k + 1) for k in range(n)]


# ================================================================ skills
def filter_fit(doc):
    """document dmFilterFit(): the school filter row (filterButtons, a listDisplay in skillPane_mc.filterContainer_mc) drawn x SK_FK, or
    smaller while it would be wider than SK_FMAXW pane px; centred as the PC's updateSkills does (x = -width / 2 - width is in the
    container's px, so the PC's own centring and filterBG_mc sizing keep working at the new scale), vertically on the PC's row centre
    (filter 42 px high at y 0: y = 21 - 21 k); filterBG_mc (the row's backing) re-placed / re-sized as the PC does"""
    mv = doc.mv; P = mv.P
    c = list(HEAD) + [('pushnull',), ('coerce_a',), ('setlocal1',), ('pushbyte', 0), ('convert_d',), ('setlocal2',), ('pushbyte', 0), ('convert_d',), ('setlocal3',),
        ('pushnull',), ('coerce_a',), ('setlocal', 4),
        ('getlocal0',), ('getproperty', P('filterButtons')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal1',), ('getproperty', P('width')), ('getlocal1',), ('getproperty', P('scaleX')), ('divide',), ('convert_d',), ('setlocal2',)] + \
        mv.f(SK_FK) + [('convert_d',), ('setlocal3',),
        ('getlocal2',), ('pushbyte', 0), ('ifngt', 'set'),
        ('getlocal2',)] + mv.f(SK_FK) + [('multiply',)] + mv.f(SK_FMAXW) + [('ifngt', 'set')] + mv.f(SK_FMAXW) + [('getlocal2',), ('divide',), ('convert_d',), ('setlocal3',),
        ('L', 'set'),
        ('getlocal1',), ('getlocal3',), ('setproperty', P('scaleX')), ('getlocal1',), ('getlocal3',), ('setproperty', P('scaleY')),
        ('getlocal1',), ('pushbyte', 21), ('pushbyte', 21), ('getlocal3',), ('multiply',), ('subtract',), ('setproperty', P('y')),
        ('getlocal1',), ('pushbyte', 0), ('getlocal1',), ('getproperty', P('width')), ('pushbyte', 2), ('divide',), ('subtract',), ('setproperty', P('x')),
        ('getlocal0',), ('getproperty', P('skillPane_mc')), ('getproperty', P('filterBG_mc')), ('coerce_a',), ('setlocal', 4), ('getlocal', 4), ('iffalse', 'end'),
        ('getlocal', 4), ('getlocal0',), ('getproperty', P('skillPane_mc')), ('getproperty', P('filterContainer_mc')), ('getproperty', P('x')),
        ('getlocal1',), ('getproperty', P('x')), ('add',), ('getlocal0',), ('getproperty', P('filterBGSpacing')), ('subtract',), ('setproperty', P('x')),
        ('getlocal', 4), ('getlocal1',), ('getproperty', P('width')), ('getlocal0',), ('getproperty', P('filterBGSpacing')), ('pushbyte', 2), ('multiply',), ('add',),
        ('callpropvoid', P('setWidth'), 1),
        ('L', 'end'), ('returnvoid',)]
    doc.method('dmFilterFit', 0, c)


def skills(data):
    data = texts(data, SK_TXT)[0]
    mv = Movie(data); P, S = mv.P, mv.S
    doc = mv.cls('skills_fla.MainTimeline'); pane = mv.cls('skills_fla.skillPane_1')
    slot = mv.cls('skillSlot'); mem = mv.cls('memorySegment')
    # frame1: the original, then 5 columns (the engine's UISkillTree reads amountOfColumns with skillSize ...: main+0x13ba6c8)
    doc.wrap('frame1', 'dmF1Orig', lambda n: list(HEAD) + call_orig('dmF1Orig', n) + [('callpropvoid', P('dmF1Orig'), n),
        ('getlocal0',)] + num(SK_COLS) + [('setproperty', P('amountOfColumns')), ('getlocal0',)] + num(SK_SPACING) + [('setproperty', P('skillSpacing')), ('returnvoid',)])
    # isDragging: true while a single tap's release runs the PC's onUp (it then skips the learn / memorise call)
    doc.wrap('isDragging', 'dmIsDragOrig', lambda n: list(HEAD) + [('getlocal0',), ('getproperty', P('dmHold')), ('dup',), ('iftrue', 'r'), ('pop',),
        ('getlocal0',), ('callproperty', P('dmIsDragOrig'), 0), ('L', 'r'), ('returnvalue',)], kind=2)
    doc_tap(doc)
    # skillSlot.onUp(e): a double tap = the PC's click (learn / memorise toggle); a single tap = the same release with isDragging held true
    def slot_up(n):
        return list(HEAD) + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('getlocal0',), ('getproperty', P('root_mc')), ('coerce_a',), ('setlocal', 2),
            ('getlocal', 2), ('iffalse', 'plain'), ('getlocal', 2), ('getlocal0',), ('callproperty', P('dmTap'), 1), ('iftrue', 'plain'),
            ('getlocal', 2), ('pushtrue',), ('setproperty', P('dmHold'))] + call_orig('dmUpOrig', n) + [('callpropvoid', P('dmUpOrig'), n),
            ('getlocal', 2), ('pushfalse',), ('setproperty', P('dmHold')), ('returnvoid',),
            ('L', 'plain')] + call_orig('dmUpOrig', n) + [('callpropvoid', P('dmUpOrig'), n), ('returnvoid',)]
    slot.wrap('onUp', 'dmUpOrig', slot_up); slot.mark()
    # memorySegment.onUp(e): a drop (isDragging) and a double tap = the PC's; a single tap = the release with the slot's id hidden (no forget)
    def mem_up(n):
        return list(HEAD) + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
            ('getlocal0',), ('getproperty', P('root_mc')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'plain'),
            ('getlocal', 2), ('getproperty', P('isDragging')), ('iftrue', 'plain'),
            ('getlocal', 2), ('getlocal0',), ('callproperty', P('dmTap'), 1), ('iftrue', 'plain'),
            ('getlocal0',), ('getproperty', P('id')), ('coerce_a',), ('setlocal', 3), ('getlocal0',), ('pushnull',), ('setproperty', P('id'))] + \
            call_orig('dmUpOrig', n) + [('callpropvoid', P('dmUpOrig'), n), ('getlocal0',), ('getlocal', 3), ('setproperty', P('id')), ('returnvoid',),
            ('L', 'plain')] + call_orig('dmUpOrig', n) + [('callpropvoid', P('dmUpOrig'), n), ('returnvoid',)]
    mem.wrap('onUp', 'dmUpOrig', mem_up); mem.mark()
    # skillPane: the frames that decorate a list's cells follow the column count (the PC wrote 6 into skillFrame.init)
    def cols_of(getter):
        return [('getlocal0',), ('getproperty', P('root_mc')), ('getproperty', P('amountOfColumns'))]
    pane.wrap('getSkillsFrame', 'dmGsfOrig', lambda n: list(HEAD) + [('pushnull',), ('coerce_a',), ('setlocal', n + 1)] + call_orig('dmGsfOrig', n) + [
        ('callproperty', P('dmGsfOrig'), n), ('coerce_a',), ('setlocal', n + 1), ('getlocal', n + 1), ('iffalse', 'r'),
        ('getlocal', n + 1)] + cols_of(0) + [('callpropvoid', P('setNumColumns'), 1), ('L', 'r'), ('getlocal', n + 1), ('returnvalue',)])
    pane.wrap('createMemorySkillList', 'dmCmlOrig', lambda n: list(HEAD) + [('pushnull',), ('coerce_a',), ('setlocal', n + 1)] + call_orig('dmCmlOrig', n) + [
        ('callproperty', P('dmCmlOrig'), n), ('coerce_a',), ('setlocal', n + 1), ('getlocal', n + 1), ('iffalse', 'r'),
        ('getlocal', n + 1), ('getproperty', P('skillFrame_mc')), ('iffalse', 'r'),
        ('getlocal', n + 1), ('getproperty', P('skillFrame_mc'))] + cols_of(0) + [('callpropvoid', P('setNumColumns'), 1), ('L', 'r'), ('getlocal', n + 1), ('returnvalue',)])
    pane.mark()
    # onEventInit: the original (skillPane_mc.init() builds the lists), then the layout
    lay = Lay(mv, 0, call_orig('dmInitOrig', 0) + [('callpropvoid', P('dmInitOrig'), 0)])
    for name in ('skillPane_mc', 'memoryPageHitRect_mc', 'dragHit_mc'): lay.xform([name], SK_X0, SK_Y0, SK_SX, SK_S[0] / float(SK_S[1]))
    lay.at(['skillPane_mc'], lay.set('listWidth', num(SK_LW)))   # lists made later (getSkillList) get headers SK_LW wide
    # each list as the PC's initSkillLists / initMemoryList built it, with SK_LW x SK_LH: setFrame, then the scroll bar spacing again (its
    # setter places the bar at width + spacing + bar width), then the PC's own -12 / +12 nudge
    for cont, frame, lst, x, fx, spacing, sbdx in (('skillContainer_mc', 'leftPageFrame_mc', 'skillLists', SK_LX, -12, ('scrollbarSpacing', -(SK_LW + 58)), -12),
                                                  ('memoryContainer_mc', 'rightPageFrame_mc', 'memoryLists', SK_RX, -20, ('SB_SPACING', -45), 12)):
        lay.place(['skillPane_mc', cont], x, SK_Y, SK_KX, SK_K)
        lay.place(['skillPane_mc', frame], x + fx * SK_KX, SK_Y - 2 * SK_K, SK_KX, SK_K)
        b = lay.call((), 'setFrame', num(SK_LW), num(SK_LH)) + lay.set(spacing[0], num(spacing[1]))
        b += [('getlocal', lay.L(1)), ('getproperty', P('m_scrollbar_mc')), ('getlocal', lay.L(1)), ('getproperty', P('m_scrollbar_mc')), ('getproperty', P('x'))] + num(sbdx) + [('add',), ('setproperty', P('x'))]
        b += lay.call(('m_scrollbar_mc',), 'setLength', num(SK_SBLEN))
        lay.at(['skillPane_mc', lst], b)
        # the page frame around the list: as the PC sized it after its setFrame
        e = lay.lab()
        lay.at(['skillPane_mc', frame], [('getlocal0',), ('getproperty', P('skillPane_mc')), ('getproperty', P(lst)), ('coerce_a',), ('setlocal', lay.L(2)), ('getlocal', lay.L(2)), ('iffalse', e),
            ('getlocal', lay.L(1)), ('getlocal', lay.L(2)), ('getproperty', P('width')), ('getlocal', lay.L(2)), ('getproperty', P('height')), ('pushbyte', 4), ('add',),
            ('callpropvoid', P('setSize'), 2), ('L', e)])
    apply_els(lay, SK_EL, SK_PAD, SK_WIN)
    # the school filter row: x SK_FK at init and after every updateSkills (the PC re-centres it there from its width)
    filter_fit(doc)
    lay.c += [('getlocal0',), ('callpropvoid', P('dmFilterFit'), 0)]
    doc.wrap('updateSkills', 'dmUpdOrig', lambda n: list(HEAD) + call_orig('dmUpdOrig', n) + [('callpropvoid', P('dmUpdOrig'), n),
        ('getlocal0',), ('callpropvoid', P('dmFilterFit'), 0), ('returnvoid',)])
    doc.wrap('onEventInit', 'dmInitOrig', lambda n: lay.end())
    doc.mark()
    return mv.done(), dict(placed=[], defs=0, methods=mv.made)


# ================================================================ crafting
def _bmp_shape(swf, sid):
    """(bitmap id, x, y of the bitmap's origin in the shape, w, h) of a DefineShape that is one 1:1 bitmap rectangle"""
    import dos2_swf_render as rn
    mvx = gr.Movie(swf); code, d, _ = swf.tags[mvx.defs[sid]]
    sh = rn.parse_shape(d, gr.T_SHAPES[code]); assert len(sh) == 1 and sh[0][0] == 'fill' and sh[0][1][0] == 'bmp', 'shape %d is not one bitmap' % sid
    _, bid, m, _, _ = sh[0][1]; assert [round(v, 3) for v in m[:4]] == [20.0, 0.0, 0.0, 20.0], 'shape %d: bitmap not 1:1' % sid
    xs = [p[0] / 20.0 for e in sh[0][2] for p in e]; ys = [p[1] / 20.0 for e in sh[0][2] for p in e]
    tx, ty = m[4] / 20.0, m[5] / 20.0
    assert min(xs) == tx and min(ys) == ty, 'shape %d: the rectangle does not start at the bitmap origin' % sid
    return bid, tx, ty, int(max(xs) - tx), int(max(ys) - ty)


def _banded(w, h, y0=0, y1=None):
    """pieces of a w-wide bitmap (rows y0..y1) with the CR_BANDS columns stretched by CR_EXW / 2 each"""
    y1 = h if y1 is None else y1; (a0, a1), (b0, b1) = CR_BANDS; e = CR_EXW / 2.0
    k0 = (a1 - a0 + e) / float(a1 - a0); k1 = (b1 - b0 + e) / float(b1 - b0)
    return [(0, y0, 0, y0, a0, y1 - y0, 1.0), (a0, y0, a0, y0, a1 - a0, y1 - y0, k0), (a1 + e, y0, a1, y0, b0 - a1, y1 - y0, 1.0),
            (b0 + e, y0, b0, y0, b1 - b0, y1 - y0, k1), (b1 + 2 * e, y0, b1, y0, w - b1, y1 - y0, 1.0)]


def cr_shift(cx):
    """own px a child of the widened panels moves, by the middle of what it draws: left third 0, middle CR_EXW / 2, right third CR_EXW"""
    return 0.0 if cx < 680 / 3.0 else CR_EXW if cx > 2 * 680 / 3.0 else CR_EXW / 2.0


CR_KEEP_LEFT = {'view_mc', 'charName_txt', 'min_mc', 'plus_mc', 'amount_txt', 'filterAmHl_mc'}   # the left groups stay together at the left
CR_RIGHT = {'filterHl_mc', 'filterName_txt', 'searchBtn_mc', 'cancelBtn_mc', 'sb_mc'}
CR_MID = {'craftResultBG_mc', 'resultIconHolder_mc'}                   # the recipe's result stays beside its name                    # the search field stays with its buttons


def craft_art(data):
    """the static half of uiCraft_p: the widened backgrounds and the children moved with them (frame 1 of every panel). -> (bytes, report)"""
    import dos2_ppanel_inv as pi
    swf = Swf(data); mvx = gr.Movie(swf)
    root = pi._root_names(swf); panel = root['craftPanel_mc']['char']
    kids = pi._kids(swf, panel); sub = {n: kids[n][0] for n in ('recipesPanel_mc', 'experimentPanel_mc', 'runesPanel_mc')}
    rep = dict(shapes=[], moved={}, defs=0)
    def bg(holder, sid, groups_of):
        bid, tx, ty, w, h = _bmp_shape(swf, sid)
        new, n = pi.pieces_sprite(swf, holder, bid, w, h, [(None, tx, ty, groups_of(w, h))])
        pi.swap_char(swf, holder, 1, sid, new); rep['shapes'].append(sid); rep['defs'] += n
        return new
    def shapes_in(holder):
        m2 = gr.Movie(swf); st = gr.frame_state(swf.tags[pi._def_index(swf, holder)][1], 1)
        return [r['char'] for r in st.values() if r['char'] in m2.defs and swf.tags[m2.defs[r['char']]][0] in gr.T_SHAPES]
    skip = set()
    for holder in (sub['experimentPanel_mc'], sub['runesPanel_mc']):
        for sid in shapes_in(holder): skip.add(bg(holder, sid, lambda w, h: _banded(w, h)))
    rb = pi._kids(swf, sub['recipesPanel_mc'])['bg_mc'][0]
    for sid in shapes_in(rb): bg(rb, sid, lambda w, h: _banded(w, h))
    head = bot = None
    for sid in shapes_in(panel):
        try: bid, tx, ty, w, h = _bmp_shape(swf, sid)
        except AssertionError: continue
        if ty == 0 and h == 172 and w == 680: head = sid
        elif ty > 900 and w == 680: bot = sid
    assert head and bot, 'craftPanel_1: the header / bottom bitmaps are not where the PC had them'
    skip.add(bg(panel, bot, lambda w, h: _banded(w, h)))
    # the header: end blocks and the three tab holes whole; two wings of CR_EXW / 2 between them (the title bar row and the dark wood row
    # stretched sideways, the hole row filled with the dark wood stretched up)
    def header(w, h):
        (e0, e1), c, e = CR_HEAD['ends'], CR_HEAD['col'], CR_EXW / 2.0; cw = c[1] - c[0]; kx = e / float(cw)
        (r0, r1), (m0, m1), (b0_, b1_) = CR_HEAD['rows']; ms0, ms1 = CR_HEAD['mid_src']
        def wing(x):
            return [(x, r0, c[0], r0, cw, r1 - r0, kx), (x, m0, c[0], ms0, cw, ms1 - ms0, kx, (m1 - m0) / float(ms1 - ms0)), (x, b0_, c[0], b0_, cw, b1_ - b0_, kx)]
        return [(0, 0, 0, 0, e0, h, 1.0)] + wing(e0) + [(e0 + e, 0, e0, 0, e1 - e0, h, 1.0)] + wing(e1 + e) + [(e1 + 2 * e, 0, e1, 0, w - e1, h, 1.0)]
    skip.add(bg(panel, head, header))
    mvx = gr.Movie(swf)
    # every placed child moves with the art (frame 1); spans (the hit area, the drag bar) grow instead
    def mover(holder, rule):
        def fn(name, rec):
            if rec['char'] in skip: return
            x, y, sx, sy = pi._mat(rec)
            if name in ('frameHit_mc', 'dragHeader_mc'): pi._set(rec, sx=sx * (CR_W - 7) / 673.0); rep['moved'][name] = 'wider'; return
            if name in sub or name == 'bg_mc': return
            b = gr.bounds(swf, rec['char'], 1, mvx) if rec['char'] in mvx.defs else None
            cx = x + sx * (b[0] + b[2]) / 2.0 if b else 340.0   # empty at build time (lists, tab bars the code fills): with the middle
            d = rule(name, cx)
            if d: pi._set(rec, dx=d); rep['moved'][name or '#%d' % rec['char']] = d
        pi.edit_places(swf, holder, fn)
    side = lambda n, cx: 0.0 if n in CR_KEEP_LEFT else CR_EXW if n in CR_RIGHT else CR_EXW / 2.0 if n in CR_MID else cr_shift(cx)
    mover(panel, side)
    mover(sub['recipesPanel_mc'], side)
    mover(sub['experimentPanel_mc'], side)
    mover(sub['runesPanel_mc'], lambda n, cx: CR_EXW / 2.0)
    swf.head = pi.rect_bytes(CR_STAGE_W, 1060) + swf.head[-4:]
    return swf.write(), rep


def craft_rows(data):
    """the recipe list's row symbols for the bigger text (static: every runtime Recipe / CraftCategory gets it): the recipe label CR_LABEL_Y
    lower (centred in its now 62 px row), the category title CR_CAT_Y, its -/+ x CR_CAT_K at CR_CAT_BTN. -> (bytes, report)"""
    import dos2_ppanel_inv as pi
    swf = Swf(data); syms = _symbols(swf); rep = {}
    def fn(name, rec):
        if name == 'label_txt': pi._set(rec, y=CR_LABEL_Y); rep[name] = CR_LABEL_Y
        elif name == 'title_txt': pi._set(rec, y=CR_CAT_Y); rep[name] = CR_CAT_Y
        elif name in ('collapse_mc', 'decollapse_mc'): pi._set(rec, x=CR_CAT_BTN[0], y=CR_CAT_BTN[1], sx=CR_CAT_K, sy=CR_CAT_K); rep[name] = CR_CAT_K
    pi.edit_places(swf, syms['Recipe'], lambda n, r: fn(n, r) if n == 'label_txt' else None)
    pi.edit_places(swf, syms['CraftCategory'], lambda n, r: fn(n, r) if n in ('title_txt', 'collapse_mc', 'decollapse_mc') else None)
    return swf.write(), rep


def craft(data):
    data, art = craft_art(data)
    data, rows = craft_rows(texts(data, CR_TXT)[0]); art['rows'] = rows
    mv = Movie(data); P, S = mv.P, mv.S
    # CraftCategory.init: the original, then finger pads on the header's -/+ (library rows: no path from the document)
    cat = mv.cls('CraftCategory')
    def cat_init(n):
        c = list(HEAD) + [('pushnull',), ('coerce_a',), ('setlocal', n + 1), ('pushnull',), ('coerce_a',), ('setlocal', n + 2), ('pushnull',), ('coerce_a',), ('setlocal', n + 3)]
        c += call_orig('dmInitOrig', n) + [('callpropvoid', P('dmInitOrig'), n)]
        w, h = int(round(CR_CAT_PAD[0] / (CR_WIN[0] * CR_CAT_K))), int(round(CR_CAT_PAD[1] / (CR_WIN[1] * CR_CAT_K)))
        for k, name in enumerate(('collapse_mc', 'decollapse_mc')):
            e = 'p%d' % k
            c += [('getlocal0',), ('getproperty', P(name)), ('coerce_a',), ('setlocal', n + 1), ('getlocal', n + 1), ('iffalse', e)]
            c += pad_lines(mv, n + 1, n + 2, n + 3, w, h, e) + [('L', e)]
        return c + [('returnvoid',)]
    cat.wrap('init', 'dmInitOrig', cat_init); cat.mark()
    doc = mv.cls('uiCraft_fla.MainTimeline')
    # frame1: the original, then the column counts of the bag and the rune inventory (engine variables, main+0x131b87c / 0x131b90c)
    doc.wrap('frame1', 'dmF1Orig', lambda n: list(HEAD) + call_orig('dmF1Orig', n) + [('callpropvoid', P('dmF1Orig'), n),
        ('getlocal0',)] + num(CR_COLS) + [('setproperty', P('invCols')), ('getlocal0',)] + num(CR_RUNE_COLS) + [('setproperty', P('invRuneCols')),
        ('getlocal0',)] + num(CR_SPACING) + [('setproperty', P('invCellSpacing')), ('getlocal0',)] + num(CR_STAGE_W) + [('setproperty', P('uiRight')), ('returnvoid',)])
    doc_scroll(doc)
    centre = mv.ei('setPosition', [('pushstring', S('center'))], [('pushstring', S('screen'))], [('pushstring', S('center'))])
    # onEventResize: the original (setPosition topright / splitscreen / topright), then centred on the screen
    doc.wrap('onEventResize', 'dmResOrig', lambda n: list(HEAD) + call_orig('dmResOrig', n) + [('callpropvoid', P('dmResOrig'), n)] + centre + [('returnvoid',)])
    lay = Lay(mv, 0, call_orig('dmInitOrig', 0) + [('callpropvoid', P('dmInitOrig'), 0)] + centre)
    lay.xform(['craftPanel_mc'], CR_X0, CR_Y0, CR_S)
    for holder, inv, frame, x in ((['craftPanel_mc', 'experimentPanel_mc', 'container_mc'], ['craftPanel_mc', 'experimentPanel_mc', 'inv'], CR_BAG, CR_BAG_X),
                                  (['craftPanel_mc', 'runesPanel_mc', 'inventory_mc', 'inventoryHolder_mc'], ['craftPanel_mc', 'runesPanel_mc', 'inventory_mc', 'inv'], CR_RUNE, CR_RUNE_X)):
        lay.place(holder, x, CR_BAG_Y if frame is CR_BAG else None, CR_K)   # the grid with its cells, its icon image and its scroll bar
        lay.at(inv, lay.call((), 'setFrame', num(frame[0]), num(frame[1])))
    for p in CR_LISTS: lay.scroll(list(p))
    apply_els(lay, CR_EL, CR_PAD, CR_WIN)
    doc.wrap('onEventInit', 'dmInitOrig', lambda n: lay.end())
    doc.mark()
    return mv.done(), dict(placed=[], defs=art['defs'], methods=mv.made, art=art)


# ================================================================ journal + map
def journal(data):
    data = texts(data, JN_TXT)[0]
    mv = Movie(data); P, S = mv.P, mv.S
    doc = mv.cls('journal_fla.MainTimeline'); mp = mv.cls('journal_fla.map_2'); qe = mv.cls('QuestEntry')
    doc_scroll(doc); doc_text_scroll(doc)
    # quest rows: the PC's height (whole text lines of entryHeight 30), then JN_ROWPAD more for the finger (its highlight too)
    qe.wrap('INTUpd4PosEl', 'dmPosOrig', lambda n: list(HEAD) + call_orig('dmPosOrig', n) + [('callpropvoid', P('dmPosOrig'), n),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('heightOverride'))] + num(JN_ROWPAD) + [('add',), ('setproperty', P('heightOverride')),
        ('getlocal0',), ('getproperty', P('title_mc')), ('getproperty', P('hl_mc')), ('dup',), ('getproperty', P('height'))] + num(JN_ROWPAD) + [
        ('add',), ('setproperty', P('height')), ('returnvoid',)])
    qe.mark()
    # map: a tap (press + release without a drag) no longer adds a flag (the PC's onMapUp did, mapClickable); a long press does (dmMapFlag)
    mp.wrap('onMapUp', 'dmUpOrig', lambda n: list(HEAD) + [('getlocal0',), ('pushfalse',), ('setproperty', P('mapClickable'))] + call_orig('dmUpOrig', n) + [
        ('callpropvoid', P('dmUpOrig'), n), ('returnvoid',)])
    mp.mark()
    # dmMapFlag(e): the host's long press = a right click; on the map, not over an icon: addFlag where the finger is (the PC's own call)
    c = list(HEAD) + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
        ('getlocal0',), ('getproperty', P('journal_mc')), ('getproperty', P('map_mc')), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('getproperty', P('hoverIcon')), ('iftrue', 'end'),
        ('getlocal', 2), ('getproperty', P('mapContentHolder_mc')), ('getproperty', P('mapContent_mc')), ('getproperty', P('iggy_map')), ('coerce_a',), ('setlocal', 3),
        ('getlocal', 3), ('iffalse', 'end')] + mv.ei('addFlag', [('getlocal', 3), ('getproperty', P('mouseX'))], [('getlocal', 3), ('getproperty', P('mouseY'))]) + [
        ('L', 'end'), ('returnvoid',)]
    doc.method('dmMapFlag', 1, c)
    lay = Lay(mv, 0, call_orig('dmInitOrig', 0) + [('callpropvoid', P('dmInitOrig'), 0)])
    lay.xform(['journal_mc'], JN_X0, JN_Y0, JN_S)
    for name in JN_LISTS: lay.scroll(['journal_mc', name])
    for path in JN_TEXTS: lay.scroll(['journal_mc'] + list(path), 'dmTxDown')   # long text fields outside the lists: the finger scrolls them too
    lay.at(['journal_mc', 'map_mc', 'mapContentHolder_mc', 'mapContent_mc'], [('getlocal', lay.L(1)), ('pushstring', S('rightMouseDown')),
        ('getlocal0',), ('getproperty', P('dmMapFlag')), ('callpropvoid', P('addEventListener'), 2)])
    for name, x, y in (('zoomInBtn', JN_ZOOM[0], JN_ZOOM[1]), ('zoomOutBtn', JN_ZOOM[2], JN_ZOOM[3])): lay.place(['journal_mc', 'map_mc', name], x, y, JN_ZOOM[4])
    apply_els(lay, JN_EL, JN_PAD, JN_WIN)
    doc.wrap('onEventInit', 'dmInitOrig', lambda n: lay.end())
    doc.mark()
    return mv.done(), dict(placed=[], defs=0, methods=mv.made)


MAKE = {'skills_p.swf': skills, 'uiCraft_p.swf': craft, 'journal_p.swf': journal}



# ================================================================ mock: the computed layout as a picture (static art + the grids' cells)
def _sym(swf, path):
    """character id of the instance at a named path from the root (frame 1)"""
    tags = [(c, d) for c, d, _ in swf.tags]; mvx = gr.Movie(swf); cid = None
    for k, name in enumerate(path):
        recs = [gr.place_parse(c, d) for c, d in tags if c in (4, 26, 70)]
        hit = [r for r in recs if r.get('name') and r['name'][:-1].decode() == name and r['char'] is not None]
        if not hit: return None
        cid = hit[0]['char']
        if k + 1 < len(path):
            code, d, _ = swf.tags[mvx.defs[cid]]; tags = gr.subtags(d)
    return cid


def _canvas(W, bg):
    from PIL import Image
    if bg and os.path.exists(bg):
        im = Image.open(bg).convert('RGBA')
        if im.size != (W, 1080): im = im.resize((W, 1080))
        return im
    return Image.new('RGBA', (W, 1080), (38, 44, 40, 255))


# sample strings of the mock pictures (English, typical lengths), by the instance name that places a TextField; fields without one keep
# the renderer's grey box
SAMPLE = {'title1_txt': 'Skills', 'title2_txt': 'Memory', 'knownSchools_txt': 'Schools', 'deckManager_txt': 'Skill decks',
          'defaultDeck_txt': 'Default deck', 'mapName_txt': 'Fort Joy', 'charName_txt': 'Sebille', 'amount_txt': '1', 'filterName_txt': 'Search',
          'result_txt': 'Unknown result'}
SAMPLE_IN = {   # (the name the field's sprite is first placed under, field) -> sample (a list: one per instance drawn, in depth order)
    ('leftJournalBtn_mc', 'text_txt'): ['Active', 'Completed'], ('onMapBtn_mc', 'text_txt'): ['Show on map', 'Postpone'],
    ('showPostponed_mc', 'text_txt'): ['Show postponed quests'], ('showTutorialPopups_mc', 'text_txt'): ['Show postponed quests'], ('allBtn_mc', 'text_txt'): 'ALL',
    ('occupiedMemory_mc', 'text_txt'): 'Memory 3 / 8', ('view_mc', 'text_txt'): 'Show unknown recipes', ('button_mc', 'text_txt'): 'COMBINE',
    ('title_mc', 'title_txt'): 'The Wrath of the Cursed', ('title_mc', 'subTitle_txt'): 'Main quest', ('craftPanel_mc', 'title_txt'): 'CRAFTING',
    ('recipesPanel_mc', 'name_txt'): 'Medium Healing Potion', ('closeContainerButton_mc', 'text_txt'): 'CLOSE', ('accept_mc', 'text_txt'): 'ACCEPT',
    ('select_mc', 'text_txt'): 'SELECT'}
FONT_TTF = (r'C:\Windows\Fonts\georgia.ttf', r'C:\Windows\Fonts\times.ttf', 'DejaVuSerif.ttf')


def _font(px):
    from PIL import ImageFont
    for f in FONT_TTF:
        try: return ImageFont.truetype(f, max(4, int(round(px))))
        except OSError: pass
    return ImageFont.load_default()


def _text(canvas, box, fh, s, align=0, wrap=False):
    """sample text s at font height fh (screen px) inside the screen box (x0, y0, x1, y1), cut at the box (as a field clips)"""
    from PIL import Image, ImageDraw
    x0, y0, x1, y1 = [int(round(v)) for v in box]; w, h = max(1, x1 - x0), max(1, y1 - y0)
    f = _font(fh); im = Image.new('RGBA', (w, h), (0, 0, 0, 0)); dr = ImageDraw.Draw(im); lines = [s]
    if wrap:
        lines = []; cur = ''
        for word in s.split():
            t = (cur + ' ' + word).strip()
            if cur and dr.textlength(t, font=f) > w - 4: lines.append(cur); cur = word
            else: cur = t
        lines.append(cur)
    y = 2.0
    for ln in lines:
        tw = dr.textlength(ln, font=f); x = 2 if align == 0 else (w - tw) / 2.0 if align == 2 else w - 2 - tw
        dr.text((x, y), ln, font=f, fill=(250, 240, 215, 255), stroke_width=max(1, int(fh / 14)), stroke_fill=(20, 12, 8, 255))
        y += fh * 1.2
    canvas.paste(im, (x0, y0), im)


def _edit_names(swf):
    """DefineEditText id -> (the name its parent sprite is placed under, its own instance name), of the first placement"""
    alltags = [((c, d), None) for c, d, _ in swf.tags]
    for c, d, _ in swf.tags:
        if c == gr.T_SPRITE: alltags += [(t, struct.unpack_from('<H', d, 0)[0]) for t in gr.subtags(d)]
    owner, out = {}, {}
    recs = [(gr.place_parse(c, d), sp) for (c, d), sp in alltags if c in (gr.T_PLACE2, gr.T_PLACE3)]
    for r, sp in recs:
        if r.get('name') and r['char'] is not None: owner.setdefault(r['char'], r['name'][:-1].decode())
    edits = {struct.unpack_from('<H', d, 0)[0] for c, d, _ in swf.tags if c == gr.T_EDIT}
    for r, sp in recs:
        if r.get('name') and r['char'] in edits: out.setdefault(r['char'], (owner.get(sp), r['name'][:-1].decode()))
    return out


def _put(canvas, swf, cid, x, y, sc, hide=(), text=True):
    """the character drawn with its own origin at screen (x, y), scale sc; `hide` = names of its direct children left out; text: the
    TextFields with a SAMPLE string get it at their font height (the movie's DefineEditText, x the drawing's vertical scale)"""
    import dos2_swf_render as rn
    from PIL import Image
    if hide:   # a copy of the movie whose sprite cid places no instance named in hide
        s2 = Swf(swf.write()); mvx = gr.Movie(s2); n = mvx.defs[cid]; code, d, lng = s2.tags[n]
        out = d[:4]
        for c, t in gr.subtags(d):
            if c in (26, 70):
                r = gr.place_parse(c, t)
                if r.get('name') and r['name'][:-1].decode() in hide: continue
            out += gr.tag_bytes(c, t)
        s2.tags[n] = [code, out, True]; swf = s2
    sx, sc = sc if isinstance(sc, tuple) else (sc, sc)   # (x scale, y scale): drawn at y's, then resized sideways
    seen = []; orig = rn.draw
    def hook(ctx, c, m, cv, cx=None, frame=1, depth=0):
        if c in ctx.mv.defs and ctx.swf.tags[ctx.mv.defs[c]][0] == gr.T_EDIT: seen.append((c, m))
        return orig(ctx, c, m, cv, cx, frame, depth)
    rn.draw = hook
    try: img, org = rn.render(swf, cid, sc)
    finally: rn.draw = orig
    if sx != sc: img = img.resize((max(1, int(round(img.size[0] * sx / sc))), img.size[1]), Image.LANCZOS)
    ox, oy = int(round(x + org[0] * sx)), int(round(y + org[1] * sc))
    canvas.alpha_composite(img, (ox, oy))
    if not text: return
    mvx = gr.Movie(swf); names = _edit_names(swf); used = {}
    for c, m in seen:
        par, nm = names.get(c, (None, None)); s = SAMPLE_IN.get((par, nm)) or SAMPLE.get(nm)
        if isinstance(s, list): k = used.get((par, nm), 0); used[(par, nm)] = k + 1; s = s[k] if k < len(s) else None
        if not s: continue
        e = edit_info(swf.tags[mvx.defs[c]][1]); r = e['rect']; k = sx / sc
        box = (ox + (m[0] * r[0] + m[4]) * k, oy + m[3] * r[1] + m[5], ox + (m[0] * r[2] + m[4]) * k, oy + m[3] * r[3] + m[5])
        _text(canvas, box, e['fh'] * abs(m[3]), s, e['align'], e['ww'])


def _mock_els(swf, els, places=()):
    """the static picture of the runtime layout: each element of els scaled about its anchor (as Lay.xform does after the PC init), each
    (path, x, y, scale) of places set (Lay.place) - on the PlaceObject of the instance in its parent sprite (this copy only)"""
    import dos2_ppanel_inv as pi
    def one(path, f):
        par = _sym(swf, list(path[:-1])) if len(path) > 1 else None
        pi.edit_places(swf, par, lambda n, rec: f(rec) if n == path[-1] else None)
    for e in els:
        x0, y0, kx, ky = el_xform(e)
        def f(rec, x0=x0, y0=y0, kx=kx, ky=ky):
            x, y, sx, sy = pi._mat(rec); pi._set(rec, x=x0 + x * kx, y=y0 + y * ky, sx=sx * kx, sy=sy * ky)
        one(e[0], f)
    for path, x, y, s in places:
        one(path, lambda rec, x=x, y=y, s=s: pi._set(rec, x=x, y=y, sx=s, sy=s))
    return swf


def _cells(canvas, rects, colour):
    from PIL import ImageDraw
    dr = ImageDraw.Draw(canvas)
    for r in rects: dr.rectangle([round(r[0]), round(r[1]), round(r[2]) - 1, round(r[3]) - 1], outline=colour, width=2)


def _ipad(canvas, path, sw, sh, W, anchor='center'):
    """the iPad movie's static root at its place on this canvas (stage sw x sh centred), drawn faint, for a look at both"""
    import dos2_swf_render as rn
    s = Swf(open(path, 'rb').read()); img, org = rn.render(s, 'root', 1.0)
    img.putalpha(img.getchannel('A').point(lambda a: a * 0.9))
    canvas.alpha_composite(img, (int((W - sw) / 2), int((1080 - sh) / 2)))


def _fh(swf, cls, names):
    """font height (px) of a TextField of the built movie"""
    mvx = gr.Movie(swf); return edit_info(swf.tags[mvx.defs[text_id(swf, cls, names, mvx)]][1])['fh']


def mock_skills(src, out, W=2400, bg=None, ipad=None):
    swf = _mock_els(Swf(MAKE['skills_p.swf'](src['skills.swf'])[0]), SK_EL)
    cv = _canvas(W, bg); ox, oy = (W - 1354) / 2.0, -20      # stage -> screen
    S = SK_S[0] / float(SK_S[1]); SX = SK_SX
    to = lambda x, y: (ox + SK_X0 + SX * x, oy + SK_Y0 + S * y)   # pane px -> screen
    pane = _sym(swf, ['skillPane_mc'])
    _put(cv, swf, pane, ox + SK_X0, oy + SK_Y0, (SX, S))
    rects = []
    for x0 in (SK_LX, SK_RX):   # 2 lists x rows of SK_COLS skill icons: list header 48 (iggy_grid y), icon 69 at pitch SK_PITCH (skillSize + skillSpacing)
        for row in range(5):
            for col in range(SK_COLS):
                lx, ly = x0 + (18 + col * SK_PITCH) * SK_KX, SK_Y + (48 + row * SK_PITCH) * SK_K
                if ly + 69 * SK_K > SK_Y + SK_LH * SK_K: continue
                rects.append(to(lx, ly) + to(lx + 69 * SK_KX, ly + 69 * SK_K))
    # the list headers (school name / memory) at their font size: skillList title_txt at list (22, 12), memoryList title_txt at (12, 12)
    for x0, cls, s in ((SK_LX, 'skillList', 'PYROKINETIC'), (SK_RX, 'memoryList', 'MEMORY')):
        fx = 22 if cls == 'skillList' else 12
        _text(cv, to(x0 + fx * SK_KX, SK_Y + 10 * SK_K) + to(x0 + (fx + 300) * SK_KX, SK_Y + 48 * SK_K), _fh(swf, cls, ('title_txt',)) * SK_K * S, s)
    # the school filter row as dmFilterFit sets it for 6 known schools: 42 px icons at pitch 45 x k, centred on filterContainer_mc (297, 108)
    k = min(SK_FK, SK_FMAXW / (6 * 45.0)); wrow = 6 * 45 * k
    frect = [to(297 - wrow / 2 + i * 45 * k, 108 + 21 - 21 * k) + to(297 - wrow / 2 + i * 45 * k + 42 * k, 108 + 21 + 21 * k) for i in range(6)]
    _cells(cv, rects + frect, (90, 200, 255, 255))
    _cells(cv, _pads(swf, SK_PAD, to), (255, 160, 60, 255))
    cv.save(out); r = rects[0]
    print('%s: skills_p computed layout, icon %.1f x %.1f px at pitch %.1f, school filter %.0f x %.0f px, book %.0f..%.0f x %.0f..%.0f (screen px)' % (
        out, r[2] - r[0], r[3] - r[1], rects[1][0] - r[0], frect[0][2] - frect[0][0], frect[0][3] - frect[0][1], ox + SK_X0 - 20 * SX, ox + SK_X0 + 1356 * SX, oy + SK_Y0 - 20 * S, oy + SK_Y0 + 1060 * S))
    if ipad:
        cv2 = _canvas(W, bg); _ipad(cv2, os.path.join(ipad, 'skills_t.swf'), 1160, 1080, W); p2 = out.replace('.png', '.ipad.png'); cv2.save(p2); print(p2)
    return dict(icon=r[2] - r[0], pitch=rects[1][0] - r[0], bottom=oy + SK_Y0 + 1060 * S, top=oy + SK_Y0 - 20 * S, left=ox + SK_X0 - 20 * SX, right=ox + SK_X0 + 1356 * SX)


def _bounds(swf, path):
    """stage bounds of a named instance (frame 1, from the root) through tools/dos2_layout.py, or None"""
    import dos2_layout as lay, tempfile
    with tempfile.TemporaryDirectory() as td:
        f = os.path.join(td, 'm.swf'); open(f, 'wb').write(swf.write())
        hit = [e for e in lay.walk(lay.Mov.load(f), len(path) + 1) if e['npath'] == '.'.join(path)]
    return hit[0]['stage'] if hit and hit[0]['stage'] else None


def _pads(swf, pads, to_screen):
    """screen rectangles of the finger pads (w x h SCREEN px), centred on the instance's drawing (stage bounds relative to the first path
    link's origin, i.e. the panel's own px: the first link is the panel at 0,0 on the PC stage)"""
    out = []
    for p in pads:
        b = _bounds(swf, list(p[0]))
        if not b: continue
        cx, cy = to_screen((b[0] + b[2]) / 2.0, (b[1] + b[3]) / 2.0); w, h = p[1], p[2]
        out.append((cx - w / 2.0, cy - h / 2.0, cx + w / 2.0, cy + h / 2.0))
    return out


def mock_craft(src, out, W=2400, bg=None, ipad=None):
    swf = _mock_els(Swf(MAKE['uiCraft_p.swf'](src['uiCraft.swf'])[0]), CR_EL)
    S = CR_S[0] / float(CR_S[1]); ox, oy = (W - CR_STAGE_W) / 2.0, 10      # setPosition center: stage CR_STAGE_W x 1060 centred
    to = lambda x, y: (ox + CR_X0 + S * x, oy + CR_Y0 + S * y)
    panel = _sym(swf, ['craftPanel_mc']); res = {}
    for tab, hide, cells in (('experiment', ('recipesPanel_mc', 'runesPanel_mc'), (CR_BAG_X, CR_BAG_Y, CR_COLS, CR_BAG)),
                             ('runes', ('recipesPanel_mc', 'experimentPanel_mc'), (32 + CR_EXW / 2.0 + CR_RUNE_X, 172 + 316 + 105, CR_RUNE_COLS, CR_RUNE)),
                             ('recipes', ('experimentPanel_mc', 'runesPanel_mc'), None)):
        pads = [p for p in CR_PAD if not set(p[0]) & set(hide)]
        if cells is None:   # the recipe tab: the art + the list's rows as the code makes them (a category header, then recipes, 62 own px each)
            cv = _canvas(W, bg); _put(cv, swf, panel, ox + CR_X0, oy + CR_Y0, S, hide)
            lx, ly = 337.5, 204   # recipesPanel_mc.container_mc (the list) in panel px
            rfh, cfh = _fh(swf, 'Recipe', ('label_txt',)) * S, _fh(swf, 'CraftCategory', ('title_txt',)) * S
            rows = [to(lx + CR_CAT_BTN[0], ly + CR_CAT_BTN[1]) + to(lx + CR_CAT_BTN[0] + 19 * CR_CAT_K, ly + CR_CAT_BTN[1] + 19 * CR_CAT_K)]
            _text(cv, to(lx + 61, ly + CR_CAT_Y) + to(lx + 634, ly + 62), cfh, 'POTIONS')
            names = ['[1] Medium Healing Potion', '[-] Huge Poison Potion', '[2] Fire Arrowheads', '[-] Scroll of Resurrection', '[3] Fish Pie',
                     '[-] Elemental Arrowheads', '[1] Strong Invisibility Potion']
            for i, s in enumerate(names):
                y = ly + 62 * (i + 1)
                if y + 62 > ly + 17 * 31: break
                rows.append(to(lx + 60, y) + to(lx + 60 + 620, y + 62)); _text(cv, to(lx + 60 + 3, y + CR_LABEL_Y) + to(lx + 60 + 620, y + 62), rfh, s)
            cat_pad = [(c[0] + c[2]) / 2.0 for c in rows[:1]] + [(rows[0][1] + rows[0][3]) / 2.0]
            pads_px = _pads(swf, pads, to) + [(cat_pad[0] - CR_CAT_PAD[0] / 2.0, cat_pad[1] - CR_CAT_PAD[1] / 2.0, cat_pad[0] + CR_CAT_PAD[0] / 2.0, cat_pad[1] + CR_CAT_PAD[1] / 2.0)]
            _cells(cv, rows, (90, 200, 255, 255)); _cells(cv, pads_px, (255, 160, 60, 255))
            p = out.replace('.png', '_%s.png' % tab); cv.save(p)
            res[tab] = dict(row=(rows[1][3] - rows[1][1]) if len(rows) > 1 else 0, rows=len(rows) - 1, label=rfh)
            print('%s: uiCraft_p recipes tab, recipe row %.0f px, label font %.1f px, %d rows visible' % (p, res[tab]['row'], rfh, res[tab]['rows'])); continue
        cv = _canvas(W, bg); _put(cv, swf, panel, ox + CR_X0, oy + CR_Y0, S, hide)
        x0, y0, cols, frame = cells; rects = []
        for row in range(20):   # inventoryClass.createCell: x = (cell + spacing) * col - bgDiscrap (-2), same for y; visible inside the frame
            for col in range(cols):
                lx, ly = CR_PITCH * col + 2, CR_PITCH * row + 2
                if ly + 50 > frame[1]: continue
                rects.append(to(x0 + CR_K * lx, y0 + CR_K * ly) + to(x0 + CR_K * (lx + 50), y0 + CR_K * (ly + 50)))
        _cells(cv, rects, (90, 200, 255, 255))
        _cells(cv, _pads(swf, pads, to), (255, 160, 60, 255))
        p = out.replace('.png', '_%s.png' % tab); cv.save(p); r = rects[0]
        res[tab] = dict(icon=r[2] - r[0], pitch=rects[1][0] - r[0], cols=cols, rows=len(rects) // cols, right=rects[cols - 1][2])
        print('%s: uiCraft_p %s tab, cell %.1f px at pitch %.1f, %d x %d visible, window %.0f..%.0f x %.0f..%.0f' % (
            p, tab, r[2] - r[0], rects[1][0] - r[0], cols, len(rects) // cols, to(0, 0)[0], to(CR_W, 0)[0], to(0, 0)[1], to(0, 1060)[1]))
    res['window'] = to(0, 0) + to(CR_W, 1060)
    if ipad:
        cv2 = _canvas(W, bg); _ipad(cv2, os.path.join(ipad, 'uiCraft_t.swf'), 1160, 1080, W); p2 = out.replace('.png', '.ipad.png'); cv2.save(p2); print(p2)
    return res


def mock_journal(src, out, W=2400, bg=None, ipad=None):
    zoom = [(('journal_mc', 'map_mc', n), x, y, JN_ZOOM[4]) for n, x, y in (('zoomInBtn', JN_ZOOM[0], JN_ZOOM[1]), ('zoomOutBtn', JN_ZOOM[2], JN_ZOOM[3]))]
    swf = _mock_els(Swf(MAKE['journal_p.swf'](src['journal.swf'])[0]), JN_EL, zoom)
    S = JN_S[0] / float(JN_S[1]); ox, oy = (W - 1360) / 2.0, -24
    to = lambda x, y: (ox + JN_X0 + S * x, oy + JN_Y0 + S * (y + 5))   # journal_mc px (it sits at 0, 5 on the PC stage)
    jm = _sym(swf, ['journal_mc']); res = {}
    qfh, ffh, afh = _fh(swf, 'QuestEntry', ('title_mc', 'name_txt')) * S, _fh(swf, 'questFilter', ('title_txt',)) * S, _fh(swf, 'QuestAction', ('name_txt',)) * S
    for tab, hide in (('quests', ('map_mc', 'tutorialContainer_mc', 'dialogLogContainer_mc', 'mapName_txt')),
                      ('map', ('tutorialContainer_mc', 'dialogLogContainer_mc', 'infoContainer_mc', 'onMapBtn_mc', 'postponeBtn_mc', 'showPostponed_mc',
                               'leftJournalBtn_mc', 'rightJournalBtn_mc', 'paperTop_mc'))):
        cv = _canvas(W, bg); _put(cv, swf, jm, ox + JN_X0, oy + JN_Y0 + 5 * S, S, hide)
        rects = []
        if tab == 'quests':   # journalContainer_mc 64,198 + list y 35; a category header, then one-line quest rows 30 + JN_ROWPAD, full list width 400
            names = ['The Wrath of the Cursed', 'Escape from Fort Joy', 'The Teleporter Pyramid', 'A Taste of Freedom', 'Hammer and Anvil',
                     'The Imprisoned Elf', 'The Shocking Scion', 'The Missing Sailor', 'Mysterious Merchant', 'Hidden Pleasures', 'A Familiar Face',
                     'The Collector', 'Fishy Business', 'The Lone Wolves', 'Blood of the Elder']
            y = 198 + 35; _text(cv, to(64 + 25, y + 2) + to(64 + 420, y + 40), ffh, 'MAIN QUESTS'); y += 40
            for k in range(16):
                if y + 30 + JN_ROWPAD > 198 + 35 + 724: break
                rects.append(to(64 + 20, y) + to(64 + 20 + 380, y + 30 + JN_ROWPAD))
                _text(cv, to(64 + 20 + 19, y + 2) + to(64 + 20 + 380, y + 30 + JN_ROWPAD), qfh, names[k % len(names)]); y += 30 + JN_ROWPAD
            # the quest log on the right: infoList (infoContainer_mc 477,187 + x 54): quest text at QuestAction's size
            iy = 187 + 130
            for s in ('Find a way out of Fort Joy.', 'We met a strange elf near the docks. She said the Magisters keep a secret in the dungeon under the fort.'):
                _text(cv, to(477 + 54 + 38, iy) + to(477 + 54 + 700, iy + 3.6 * afh / S), afh, s, 0, True); iy += 3.6 * afh / S + 10
            res[tab] = dict(row=rects[0][3] - rects[0][1], rows=len(rects), quest_font=qfh, text_font=afh)
        else:                 # zoom buttons where the added code puts them (map_mc at 16,133), JN_ZOOM px
            for x, y in ((JN_ZOOM[0], JN_ZOOM[1]), (JN_ZOOM[2], JN_ZOOM[3])):
                rects.append(to(16 + x, 133 + y) + to(16 + x + 43 * JN_ZOOM[4], 133 + y + 43 * JN_ZOOM[4]))
            res[tab] = dict(zoom=rects[0][2] - rects[0][0])
        _cells(cv, rects, (90, 200, 255, 255))
        _cells(cv, _pads(swf, [p for p in JN_PAD if not set(p[0]) & set(hide)], lambda x, y: to(x, y - 5)), (255, 160, 60, 255))
        p = out.replace('.png', '_%s.png' % tab); cv.save(p)
        print('%s: journal_p %s, %s, window %.0f..%.0f x %.0f..%.0f' % (p, tab, {k: round(v, 1) for k, v in res[tab].items()}, to(0, 0)[0], to(1360, 0)[0], to(0, 0)[1], to(0, 1064)[1]))
    res['window'] = to(0, 0) + to(1360, 1064)
    if ipad:
        cv2 = _canvas(W, bg); _ipad(cv2, os.path.join(ipad, 'journal_csp_t.swf'), 1160, 1040, W); p2 = out.replace('.png', '.ipad.png'); cv2.save(p2); print(p2)
    return res


def build(src): return {o: MAKE[o](src[OUT[o]])[0] for o in OUT if o in MAKE}


# ================================================================ static checks (also run by tools/dos2_pchud_touch.py check)
TABLES = {'skills_p.swf': (SK_EL, SK_PAD, SK_WIN, SK_TXT), 'uiCraft_p.swf': (CR_EL, CR_PAD, CR_WIN, CR_TXT), 'journal_p.swf': (JN_EL, JN_PAD, JN_WIN, JN_TXT)}
HOTBAR_TOP = 967      # screen y of the touch hot bar's slots (hotBar_p, TOUCH_MODES_PLAN.md "Implemented: touch HUD"); panels end above it
WANT = {   # o -> (document class, methods that must be wrapped / added, EI-free numbers checked below)
    'skills_p.swf': ('skills_fla.MainTimeline', ['dmInitOrig', 'dmF1Orig', 'dmIsDragOrig', 'dmTap']),
    'uiCraft_p.swf': ('uiCraft_fla.MainTimeline', ['dmInitOrig', 'dmF1Orig', 'dmResOrig', 'dmScDown', 'dmScMove', 'dmScUp']),
    'journal_p.swf': ('journal_fla.MainTimeline', ['dmInitOrig', 'dmScDown', 'dmScMove', 'dmScUp', 'dmMapFlag', 'dmTxDown', 'dmTxMove', 'dmTxUp'])}


def _ei_names(abc):
    """strings pushed right after `getlex ExternalInterface` (the EI names a movie calls with a literal)"""
    out = set()
    for b in abc.bodies:
        ins = abc.ins(b.code)
        for a, c in zip(ins, ins[1:]):
            if a[1] == 0x60 and abc.short(a[2][0]) == 'ExternalInterface' and c[1] == 0x2c: out.add(abc.S(c[2][0]))
    return out


def _paths(o):
    """every instance path the added layout code walks (from the document root)"""
    els, pads = TABLES[o][0], TABLES[o][1]
    extra = [list(e[0]) for e in els] + [list(p[0]) for p in pads]
    if o == 'skills_p.swf':
        return [['skillPane_mc'], ['memoryPageHitRect_mc'], ['dragHit_mc']] + [['skillPane_mc', n] for n in (
            'skillContainer_mc', 'memoryContainer_mc', 'leftPageFrame_mc', 'rightPageFrame_mc', 'filterContainer_mc', 'filterBG_mc')] + extra
    if o == 'uiCraft_p.swf':
        return [['craftPanel_mc'], ['craftPanel_mc', 'experimentPanel_mc', 'container_mc'], ['craftPanel_mc', 'runesPanel_mc', 'inventory_mc', 'inventoryHolder_mc'],
                ['craftPanel_mc', 'recipesPanel_mc']] + extra
    return [['journal_mc'], ['journal_mc', 'map_mc', 'mapContentHolder_mc', 'mapContent_mc'], ['journal_mc', 'map_mc', 'zoomInBtn'],
            ['journal_mc', 'map_mc', 'zoomOutBtn']] + extra + [['journal_mc'] + list(p) for p in JN_TEXTS]


def _traits(abc, cls):
    try: ci = abc.class_index(cls)
    except KeyError: return set()
    return {abc.short(t.name) for t in abc.instances[ci]['traits']}


def check_one(o, data, out, say):
    """the checks of one movie (data = the PC movie, out = ours); say(ok, text) as tools/dos2_pchud_touch.py; returns nothing (say counts)"""
    import dos2_layout as lay, tempfile
    from dos2_abc import Swf as _Swf
    a = _Swf(data); (_, _, A), = a.abcs(); b = _Swf(out); (_, _, B), = b.abcs()
    doc, need = WANT[o]
    ea, eb = _ei_names(A), _ei_names(B)
    say(ea == eb, 'ExternalInterface names: the PC movie\'s %d, none added or lost%s' % (len(ea), '' if ea == eb else ': +%s -%s' % (sorted(eb - ea), sorted(ea - eb))))
    ta, tb = _traits(A, doc), _traits(B, doc)
    say(ta <= tb and all(n in tb for n in need), 'document class: every PC method / property kept (the engine\'s invokes), added %s' % ', '.join(need))
    with tempfile.TemporaryDirectory() as td:
        f = os.path.join(td, OUT[o]); open(f, 'wb').write(data)
        have = {e['npath'] for e in lay.walk(lay.Mov.load(f), 5)}
    miss = ['.'.join(p) for p in _paths(o) if '.'.join(p) not in have]
    say(not miss, 'every instance path the layout code walks is on the PC timeline (%d)%s' % (len(_paths(o)), ': missing ' + ', '.join(miss) if miss else ''))
    ib = {v: k for k, v in B.owners().items()}
    dis = lambda m: '\n'.join(B.dis(B.methods[ib[m]].body)) if m in ib else ''
    if o == 'skills_p.swf':
        t = dis(doc + '::frame1'); say('dmF1Orig' in t and 'amountOfColumns' in t, 'frame1 = the original, then amountOfColumns = %d (engine: UISkillTree reads it, main+0x13ba6c8)' % SK_COLS)
        t = dis('skillSlot::onUp') + dis('memorySegment::onUp')
        say(t.count('dmTap') == 2 and t.count('dmUpOrig') >= 4 and 'dmHold' in t and "setproperty      id" in t, 'skill / memory slot release: double tap = the PC click, single = held (no learn / forget)')
        t = dis('skills_fla.skillPane_1::getSkillsFrame') + dis('skills_fla.skillPane_1::createMemorySkillList')
        say(t.count('setNumColumns') == 2, 'skill frames follow amountOfColumns (getSkillsFrame, createMemorySkillList)')
        t = dis(doc + '::onEventInit'); S = SK_S[0] / float(SK_S[1])
        icon = 69 * SK_K * S; iconx = 69 * SK_KX * SK_SX; top = SK_Y0 - 20 * S - 20; bottom = SK_Y0 + 1060 * S - 20; width = 1376 * SK_SX
        content = SK_LW * SK_KX   # pane px of a list: the left page's must end before the right page's list, the right one inside the book
        fits = SK_LX + content + 20 <= SK_RX and SK_RX + content <= 1356 - 30 and SK_X0 - 20 * SK_SX >= 0 and SK_X0 + 1356 * SK_SX <= 1354
        say(t.startswith('    0  getlocal0') and 'dmInitOrig' in t and 'setFrame' in t and 'listWidth' in t,
            'onEventInit = original + layout: book x%.2f / x%.2f (wide / high), pages x%.4f / x%.4f, list frames %d x %d, listWidth %d' % (SK_SX, S, SK_KX, SK_K, SK_LW, SK_LH, SK_LW))
        say(abs(icon - 76) < 0.5 and abs(iconx - 76) < 0.5 and bottom < HOTBAR_TOP and top >= 0 and fits and 'skillSpacing' in dis(doc + '::frame1'),
            'skill icon %.1f px on screen (iPad 76), pitch %.1f; book %.0f wide, screen y %.0f..%.0f (above the hot bar %d); %d columns %.0f of %.0f pane px' % (
                icon, SK_PITCH * SK_KX * SK_SX, width, top, bottom, HOTBAR_TOP, SK_COLS, content, SK_RX - SK_LX - 20))
    elif o == 'uiCraft_p.swf':
        t = dis(doc + '::frame1'); say('dmF1Orig' in t and 'invCols' in t and 'invRuneCols' in t, 'frame1 = the original, then invCols %d / invRuneCols %d (engine: main+0x131b87c / 0x131b90c)' % (CR_COLS, CR_RUNE_COLS))
        t = dis(doc + '::onEventInit') + dis(doc + '::onEventResize')
        say(t.count("'setPosition'") == 2 and t.count("'center'") >= 4 and 'dmScDown' in t, 'centred (setPosition center / screen / center after the PC\'s topright, init and resize); recipe list scrolls by finger')
        S = CR_S[0] / float(CR_S[1]); cell = 50 * CR_K * S; bag = CR_BAG[0] * CR_K; rune = CR_RUNE[0] * CR_K
        top = CR_Y0 + 10; bottom = CR_Y0 + 1060 * S + 10; rows = (CR_BAG[1] - 2) / float(CR_PITCH)
        say(abs(cell - 76) < 0.5 and CR_BAG_X >= 0 and CR_BAG_X + bag + CR_SB <= CR_W and CR_RUNE_X + rune + CR_SB <= 614 and bottom < HOTBAR_TOP and top >= 0
            and CR_X0 >= 0 and CR_X0 + CR_W * S <= CR_STAGE_W and t.count("'invCellSpacing'") + dis(doc + '::frame1').count('invCellSpacing') >= 1,
            'cell %.1f px on screen (iPad 76), pitch %.1f (iPad 86); bag %d columns x %.2f rows visible, %.0f + its bar of %d own px; runes %d columns, %.0f + bar of the 614 px popup; window %.0f x %.0f (iPad 1160 x 1080), screen y %.0f..%.0f; stage %d' % (
                cell, CR_PITCH * CR_K * S, CR_COLS, rows, bag, CR_W, CR_RUNE_COLS, CR_RUNE_X + rune, CR_W * S, 1060 * S, top, bottom, CR_STAGE_W))
        art = MAKE[o](data)[1]['art']; mvd = art['moved']
        say(len(art['shapes']) == 5 and mvd.get('closeButton_mc') == CR_EXW and mvd.get('tabBar_mc') == CR_EXW / 2.0 and 'view_mc' not in mvd and mvd.get('frameHit_mc') == 'wider',
            'art widened by %d own px: 5 background bitmaps re-cut (panels: 2 plain bands stretched; header: end blocks + tab holes whole, 2 wings), %d children moved (close %+d, tabs %+.0f: the rune tab and the close %.0f px apart on screen)' % (
                CR_EXW, len(mvd), CR_EXW, CR_EXW / 2.0, (638 + CR_EXW - (470 + 140 + CR_EXW / 2.0)) * S))
    else:
        t = dis('QuestEntry::INTUpd4PosEl'); say('dmPosOrig' in t and 'heightOverride' in t, 'quest rows: the PC height + %d (highlight too)' % JN_ROWPAD)
        t = dis('journal_fla.map_2::onMapUp'); say(t.find('mapClickable') < t.find('dmUpOrig') and 'pushfalse' in t, 'map: a tap no longer adds a flag (mapClickable false before the PC onMapUp)')
        t = dis(doc + '::dmMapFlag') + dis(doc + '::onEventInit')
        say("'addFlag'" in t and "'rightMouseDown'" in t and 'hoverIcon' in t, 'map: long press (the host\'s right click) on the map, not on an icon = addFlag at the finger')
        S = JN_S[0] / float(JN_S[1]); row = (30 + JN_ROWPAD) * S; top = JN_Y0 + 5 * S - 24; bottom = JN_Y0 + (5 + 1064) * S - 24
        t = dis(doc + '::onEventInit')
        say(row >= 44.5 and bottom < HOTBAR_TOP and top >= 0 and 43 * JN_ZOOM[4] * S >= 50 and JN_ZOOM[0] + 43 * JN_ZOOM[4] <= JN_ZOOM[2] and t.count("'dmTxDown'") + t.count('dmTxDown') >= len(JN_TEXTS),
            'quest row %.1f px on screen (iPad 45); zoom buttons %.0f px; window %.0f x %.0f, screen y %.0f..%.0f; %d lists + %d text field(s) scroll by finger' % (
                row, 43 * JN_ZOOM[4] * S, 1360 * S, 1064 * S, top, bottom, len(JN_LISTS), len(JN_TEXTS)))
    # element scale-up (owner 23.11): close drawn CLOSE_PX, pads, the elements inside their window, the text sizes
    els, pads, win, txt = TABLES[o]
    t = dis(doc + '::onEventInit')
    close = [e for e in els if 'clos' in e[0][-1].lower()]
    say(close and all(abs(23 * e[1] * win[0] - CLOSE_PX) < 0.5 and abs(23 * e[2] * win[1] - CLOSE_PX) < 0.5 for e in close),
        'close button(s) drawn %d x %d px on screen (the inventory\'s INV_CLOSE, iPad 50), top-right corner where the PC had it: %s' % (
            CLOSE_PX, CLOSE_PX, ', '.join('.'.join(e[0][1:]) for e in close)))
    own = [pad_own(p, win, els) for p in pads]
    tight = [p for p in pads if p[0][-1] in PAD_TIGHT]
    low = [p for p in pads if p[0][-1] in PAD_LOW]
    say(all(min(p[1], p[2]) >= 64 or (p in tight and p[2] >= PAD_MIN) or (p in low and p[1] >= PAD_MIN and p[2] >= 50) for p in pads) and t.count('drawRect') == len(pads) and all(min(w) > 0 for w in own),
        '%d finger pads, each >= 64 x 64 px on screen (%s)%s' % (len(pads), ', '.join('%s %dx%d' % (p[0][-1], p[1], p[2]) for p in pads),
            ''.join([' (%s: as wide as drawn - side by side: only taller)' % ', '.join(p[0][-1] for p in tight)] if tight else []) +
            ''.join([' (%s: under the school icons: wide, 50 high)' % ', '.join(p[0][-1] for p in low)] if low else [])))
    # every scaled child of the window clip: its PC drawing (frame 1 bounds) through the scale stays inside the window's own box
    box = {'skills_p.swf': (0, -20, 1356, 1060), 'uiCraft_p.swf': (0, 0, CR_W, 1060), 'journal_p.swf': (0, 0, 1360, 1064)}[o]
    import dos2_layout as lay, tempfile
    with tempfile.TemporaryDirectory() as td:
        f = os.path.join(td, OUT[o]); open(f, 'wb').write(out)
        wk = {e['npath']: e for e in lay.walk(lay.Mov.load(f), 5)}
    outside = []; n_in = 0
    for e in els:
        if len(e[0]) > 2: continue   # nested deeper than the window's own child (the recipe panel's, the rune popup's): the mock shows them
        st = wk.get('.'.join(e[0]), {}).get('stage')
        if not st: outside.append('.'.join(e[0]) + ' (no bounds)'); continue
        dy0 = 5.0 if o == 'journal_p.swf' else 0.0   # journal_mc sits at 0,5 on the PC stage; the others at 0,0
        x0, y0, kx, ky = el_xform(e); n_in += 1
        bx = [x0 + st[0] * kx, y0 + (st[1] - dy0) * ky, x0 + st[2] * kx, y0 + (st[3] - dy0) * ky]
        if bx[0] > bx[2]: bx[0], bx[2] = bx[2], bx[0]
        if bx[0] < box[0] or bx[1] < box[1] or bx[2] > box[2] or bx[3] > box[3]: outside.append('%s %s' % ('.'.join(e[0]), [round(v) for v in bx]))
    say(not outside, '%d elements scaled x%.2f..x%.2f about anchors that keep them clear of their neighbours; the %d children of the window clip stay inside it%s' % (
        len(els), min(min(e[1], e[2]) for e in els), max(max(e[1], e[2]) for e in els), n_in, '' if not outside else ': ' + '; '.join(outside)))
    bs = Swf(out); mvb = gr.Movie(bs); syms = _symbols(bs); wrong = []
    for cls, names, fh, how, w in txt:
        got = edit_info(bs.tags[mvb.defs[text_id(bs, cls, names, mvb, syms)]][1])['fh']
        if abs(got - fh) > 0.05: wrong.append('%s.%s %.1f' % (cls, '.'.join(names), got))
    pc = texts(data, txt)[1]
    say(not wrong, '%d text fields raised (font px; on screen x%.2f): %s%s' % (len(pc), win[1], ', '.join('%s %g->%g' % ('.'.join(r[0].split('.')[-2:]), r[2], r[3]) for r in pc),
        '' if not wrong else ': NOT SET ' + ', '.join(wrong)))


def tag_filter(o, g):
    """tools/dos2_swf_graft.py check messages that are by design: the raised text sizes change DefineEditText tags (code 37; this module's
    own check compares the movie with them put back, see check); uiCraft_p's widened art changes the stage size and re-places frame 1 of
    the panel sprites (DefineSprite, code 39; the recipe rows too); also used by tools/dos2_pchud_touch.py check"""
    g = [m for m in g if not (m.startswith('original tag #') and '(code 37)' in m)]
    if o != 'uiCraft_p.swf': return g
    return [m for m in g if m != 'header / tail differ' and not (m.startswith('original tag #') and '(code 39)' in m)]


def check(src):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    for o in OUT:
        data = src[OUT[o]]; out = MAKE[o](data)[0]; b = Swf(out); (_, _, B), = b.abcs()
        print('%s -> %s: %d B -> %d B, sha256 %s' % (OUT[o], o, len(data), len(out), hashlib.sha256(out).hexdigest()[:16]))
        errs = verify_all(B)
        for who, why in errs: print('       %s: %s' % (who, why))
        say(not errs, 'ABC verifies: %d method bodies' % len(B.bodies))
        say(Swf(Swf(out).write()).body() == b.body(), 'the movie parses and round-trips')
        g = tag_filter(o, gr.check(data, unfont(data, out), quiet=True))
        say(g == [], 'every original tag kept (the raised text fields put back: nothing else differs; uiCraft_p: but the re-placed sprites and the stage), ids unique, uses after definitions%s' % ('' if not g else ': ' + str(g)))
        check_one(o, data, out, say)
    return bad


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == 'build':
        out = Path(a[2]); out.mkdir(parents=True, exist_ok=True)
        for n, d in build(sources(a[1])).items(): (out / n).write_bytes(d); print('%s: %d B' % (out / n, len(d)))
        return
    if len(a) == 2 and a[0] == 'check': sys.exit(1 if check(sources(a[1])) else 0)
    if len(a) >= 3 and a[0] == 'mock':
        src = sources(a[1]); out = Path(a[2]); out.mkdir(parents=True, exist_ok=True)
        bg = a[a.index('--bg') + 1] if '--bg' in a else None; ipad = a[a.index('--ipad') + 1] if '--ipad' in a else None
        for name, fn in (('skills', mock_skills), ('uiCraft', mock_craft), ('journal', mock_journal)):
            fn(src, str(out / ('%s_p_mock_2400.png' % name)), 2400, bg, ipad)
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
