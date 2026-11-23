"""[DOS2] UI mode TOUCH: the windows the game makes on demand in mode 0 - trade, container, dialog, reward, waypoints, item splitter - laid
out and touchable as the iPad's (docs/dos2/TOUCH_MODES_PLAN.md "Implemented: iPad layout - trade / container / dialog / reward / waypoints").
The player's own PC movies (Switch Game.pak: trade.swf, containerInventory.swf, dialog.swf, reward.swf, waypoints.swf, itemSplitter.swf),
ABC methods appended / re-pointed (every original body kept and called), named `<name>_p.swf` like the HUD of tools/dos2_pchud_touch.py.
Only NUMBERS of the iPad movies are used (docs/dos2/TOUCH_LAYOUT.md section 5, TOUCH_UI_AUDIT.md sections 5-7); the art is the PC movies'
own symbols. Static only: nothing here was played (no Flash player) or run on a phone.

Hook: tools/dos2_pchud_touch.py calls install() at its end; that adds these movies to its sources() / build() / check() (and so to the
`touch` set of tools/dos2_touchui_pak.py). waypoints_p.swf = the shared tool's PANEL_CLOSE movie + the double tap below.

What the engine reads. The grid geometry of trade / container / reward is read by the engine from the movie's own variables (main.raw:
UIKeyboardTrade 0x134bd00 reads trade_mc.cellHeight / cellWidth / cellBorder / iconWidth / iconHeight / rows / offerRows / invCols /
offerCols / bgDiscrap / cellSpacing; UIContainerInventory 0x12c7c00 reads inv_mc.cellHeight / cellWidth / bgDiscrap / cellSpacing / cols /
rows; the reward class 0x13ea8ac reads root.icon_size) and the slot frames and item icons are drawn by the engine into the `iggy_*`
placeholders at those numbers. The iPad's _t movies set the SAME variable names to the touch numbers (trade_t: cell 76 + 2 x 3 border =
82, spacing 4, 4 inventory columns, 2 offer columns). So:
  trade_p          the iPad's window: 1160 wide (stage 1160 x 1056: the PC frame's own height; iPad 1160 x 1030; PC 1360 x 1121), anchored
                   centre / screen / centre (PC: top). The grid variables set to trade_t's numbers right after the PC frame script
                   (trade_1.frame1 wrapped): cells 82 (icon 76), pitch 86, inventories 4 columns (7 full rows + a part of the 8th seen, as the
                   iPad's 7.5) at x 15 / 804.5 y 290 (iPad 25 / 796, 272), offers 2 x 8 at x 410 / 582 (iPad 407 / 584); the PC Cell symbols
                   scaled 82 / 55. The PC frame in 11 slices (frame_mc = the centre column x1.06, ten grafted copies of its symbol cut by
                   scrollRect: the side strips with the filter tabs and repair / identify cut, the panes narrowed to their grids, one portrait
                   slot and 110 px of the plain name bars cut in the header). The filter tabs (engine addTab) a row above each inventory (the
                   iPad's filter row): the list holder turned -90, every tab +90, so the PC list lays them out left to right, upright. The
                   iPad's bottom row: offer junk (x1.9, 91 x 61) under the player's inventory, OK (308 x 52 on the PC's OK plate) in the
                   centre, repair / identify x1.164 = 64 x 64 under the trader's; close x1.6 in the top right corner (iPad 50); party
                   portraits x0.8. Finger pads: junk 96 x 72, OK 64 high, repair / identify 72, balance 64, clear 50. Mouse code is the PC's:
                   first tap = select, a second tap on the same slot = itemClick (moves the item across, as the iPad's DoubleTap), long press
                   (host right click) = itemRightClick, still finger (host hover) = tooltip, drag = startDragging / stopDragging.
  containerInventory_p   inv_mc scaled 76 / 55 (cells 76 on screen; iPad: cells 76, its art x1.2), centred on the stage (stage 1400 x 1080,
                   setPosition center / screen / center like the iPad's anchor consts; PC: no anchor in mouse mode). Take all / close
                   become 138 x 66 (iPad 118 x 56). Double tap = doubleClickItem (take), drag = the PC's.
  reward_p         reward_mc scaled 76 / 56 (cells 76, window 651 x 798; iPad 620 x 915, cells 76), accept button x1.4 more (296 x 61; iPad
                   291 x 62). Tap = itemClicked (the PC's pick by click).
  itemSplitter_p   splitter_mc scaled 1.32 (iPad 638 x 478 / PC 484 x 350), OK / cancel x1.25 more (252 x 53 each).
  dialog_p         the iPad's window: 1160 x 400 at the bottom centre (PC 1352; the stage stays 1920 x 1080, setPosition bottom): the art
                   in three slices (the strips 1:1, the text part x0.84), the text list 1010 wide (PC 1200; texts re-wrapped at 810, answers
                   at 785), trade / inventory / stop listening on the left strip (iPad: trade at the top, stop listening at the bottom; finger
                   pads 76 x 100 / 84 x 100). Every answer (the engine's: updateDialog -> addKeywordAnswer) a full-width tappable row: a
                   transparent ANSWER_W x (text + 2 x ANSWER_PAD) rect (tap anywhere on the row = that answer: the PC's ROLL_OVER select +
                   CLICK executeSelection). A one-finger drag on the text or the answers scrolls them (finger_scroll); the release that ends a
                   scroll picks no answer.
  waypoints_p      + double tap on a waypoint = travel (the iPad's DoubleTap; PC: tap selects, the travel button travels); a one-finger
                   drag on the list scrolls it (no waypoint picked).
  The container, reward, trade and splitter windows have no long text of their own (labels, item grids whose drag is the item drag; the
  item tooltip is the separate tooltip window); their grids keep the two-finger wheel.

  python -I tools/dos2_ppanel_trade.py check  <Switch Game.pak | dir of the PC movies>
  python -I tools/dos2_ppanel_trade.py build  <Switch Game.pak | dir> <out dir>
  python -I tools/dos2_ppanel_trade.py render <Switch Game.pak | dir> <iPad movie dir (study only)> <out dir>
        <out>/ppanel_<name>.png: the computed layout (the movie's own symbols drawn where the patched code puts them, the engine's grid as
        dark cells) next to the iPad movie rendered (a picture of the NUMBERS, not a played movie)
"""
import hashlib, os, sys
from fractions import Fraction
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, NS_PUBLIC
import dos2_swf_graft as gr

GUI = 'Public/Game/GUI/'

# ---------------------------------------------------------------- numbers (stage px of each movie = design px on a 1080 high screen at UI scale 1)
# trade (PC trade.swf: stage 1360 x 1121, anchored top; frame_mc 0..1360 x 0..1056: header y 0..233 (its bottom line 231..232), below it the
# body: x 0..10 border, 10..74 the filter-tab strip, 74..470 player pane, 470..512 pillar, 512..848 centre (offers), 848..890 pillar,
# 890..1285 trader pane, 1285..1349 the repair / identify strip, 1349..1360 border). iPad trade_t: window 1160 x 1030, inventories 343 x 647
# at x 25 / 796 (y 272), offers 170 x 600 at x 407 / 584, a bottom row (y 928..998) with junk 206 x 62 under the player's inventory, OK
# 206 x 62 in the centre, repair / identify 64 x 64 under the trader's; close 50 in the top right corner.
TR_CELL, TR_BORDER, TR_SPACING, TR_DISCRAP = 82, 3, 4, Fraction(-7, 2)     # trade_t: CELL_SIZE 76 + 2 x CELL_BORDER 3, CELL_BORDER_SPACING 4, -3.5
TR_PITCH = TR_CELL + TR_SPACING                                             # 86
TR_INV_COLS, TR_INV_ROWS, TR_OFF_COLS, TR_OFF_ROWS = 4, 8, 2, 8              # trade_t 4 / 8 / 2 / 7; PC 6 / 13 / 2 / 12
TR_INV_W, TR_OFF_W = TR_INV_COLS * TR_PITCH, TR_OFF_COLS * TR_PITCH          # 344 / 172 (the last column's spacing included: 340 / 168 drawn)
TR_INV_H, TR_OFF_H = 640, TR_OFF_ROWS * TR_PITCH + 1                         # inventories: 7 rows + a part of the 8th seen (iPad 7.5), offers 8 rows
TR_INV_SB, TR_OFF_SB = 646, 689                                              # scroll bar lengths (PC setLength 770 / 700 in init)
TR_FRAME_H = 1056
TR_HDR = 233                                                                 # the PC frame's header band ends here
TR_W, TR_H = 1160, TR_FRAME_H                                                # stage / setMcSize (iPad 1160 x 1030; the PC frame's own height kept)
# the PC frame in slices (x0, x1, y0, y1 of the frame -> new x, scaleX); the first is frame_mc itself, the others are grafted copies of its
# symbol. Header: one portrait slot + 18 px (168..278) and 110 px of the plain name bars (960..1070) cut; body: both side strips cut, the panes
# narrowed to their grids (350 / 349), the centre column widened to 356 (two offer grids of 168 drawn + 4)
TR_K_C, TR_K_L, TR_K_R = Fraction(356, 336), Fraction(350, 396), Fraction(349, 395)
TR_SLICES = [(512, 848, 0, TR_FRAME_H, 402, TR_K_C),
             (0, 168, 0, TR_HDR, 0, 1), (278, 512, 0, TR_HDR, 168, 1), (848, 960, 0, TR_HDR, 758, 1), (1070, 1360, 0, TR_HDR, 870, 1),
             (0, 10, TR_HDR, TR_FRAME_H, 0, 1), (74, 470, TR_HDR, TR_FRAME_H, 10, TR_K_L), (470, 512, TR_HDR, TR_FRAME_H, 360, 1),
             (848, 890, TR_HDR, TR_FRAME_H, 758, 1), (890, 1285, TR_HDR, TR_FRAME_H, 800, TR_K_R), (1349, 1360, TR_HDR, TR_FRAME_H, 1149, 1)]
TR_COPIES = ['dmFrame%d' % k for k in range(1, len(TR_SLICES))]


def tr_map(x, body):
    """new x of PC frame x in the header (body False) or the body band; a cut-out x goes to the end of the slice before it"""
    best = None
    for x0, x1, y0, y1, nx, k in TR_SLICES:
        if (y0 == 0 and y1 == TR_FRAME_H) or (y1 != TR_HDR) == body:
            if x0 <= x < x1: return nx + (x - x0) * float(k)
            if x >= x1 and (best is None or x1 > best[0]): best = (x1, nx + (x1 - x0) * float(k))
    return best[1] if best else 0.0


def _cx(x0, x1, w=None, body=False):
    """new left x of an element drawn x0..x1 (PC), kept at size w (default its own), centred where the PC centre maps to"""
    w = (x1 - x0) if w is None else w
    return Fraction(tr_map((x0 + x1) / 2.0, body) - w / 2.0).limit_denominator(10)


# the four grids: the iggy_* holders and the scroll bar holders at the cell origin - 3.5 (SIDE / TOP_SPACING = -bgDiscrap), the lists at 0, 0
TR_GRID = {'playerInventory_mc': (Fraction(23, 2), Fraction(573, 2)), 'traderInventory_mc': (801, Fraction(573, 2)),
           'playerExchange_mc': (Fraction(813, 2), 241), 'traderExchange_mc': (Fraction(1157, 2), 241)}   # cells at x 15 / 804.5 (y 290), 410 / 582 (y 244.5)
TR_LISTS = ['playerInventoryList', 'playerExchangeList', 'traderExchangeList', 'traderInventoryList']
TR_HOLDER = {'playerInventoryList': 'playerInventory_mc', 'playerExchangeList': 'playerExchange_mc', 'traderExchangeList': 'traderExchange_mc',
             'traderInventoryList': 'traderInventory_mc'}
# scroll bars (15 px) where the PC had them relative to the pillars / centre column: player inventory 1 px into the left pillar, player offers
# 36 px into it, trader offers 9 px inside the centre's right edge, trader inventory 26 px into the right pillar
TR_BAR = {'playerInventoryList': 361, 'playerExchangeList': 396, 'traderExchangeList': 748, 'traderInventoryList': 784}
TR_SB = {n: TR_BAR[n] - TR_GRID[TR_HOLDER[n]][0] - (TR_INV_W if 'Inventory' in n else TR_OFF_W) for n in TR_LISTS}   # SB_SPACING
TR_SBMC = {'playerInventorySB_mc': 'playerInventory_mc', 'traderInventorySB_mc': 'traderInventory_mc', 'playerExchangeSB_mc': 'playerExchange_mc',
           'traderExchangeSB_mc': 'traderExchange_mc'}
# the filter tabs (Tab 41 x 41; PC: a vertical list in each side strip): a row above each inventory (the iPad's filter row, y 219..268): the
# tab holder turned -90 and every tab +90, so the list's own vertical layout lays them out left to right, upright
TR_TABS = {'playerTabs_mc': (15, 240), 'traderTabs_mc': (Fraction(1609, 2), 240)}
TR_TAB_SP = 2                                                                # PC 12: 8 tabs in 342 px
_BAL_X = _cx(652, 709)                                                       # 552
# every other element: path -> its new x, y (sizes kept unless TR_SCALE)
TR_POS = {
    # header, left: 4 party portraits x0.8 (80 x 100 at a pitch of 79 -> 253 px) over the three portrait slots left, the sort row and the gold row moved with the cut
    ('characterList_mc',): (82, 71), ('playerSortLabel_txt',): (26, 179), ('playerCombo_mc',): (133, 176), ('playerGold_mc',): (274, 202),
    # centre column (header and body)
    ('balance_mc',): (_BAL_X, 41), ('playerExchangeGold_mc',): (_cx(559, 643), 204), ('traderExchangeGold_mc',): (_cx(718, 802), 204),
    ('clearSupplyBtn_mc',): (_cx(587, 612, 35), 153), ('clearDemandBtn_mc',): (_cx(747, 772, 35), 153),
    ('ok_mc',): (410, 923),                                                  # its drawing (local 16..324 x 23..75) centred at 580: 426..734 x 946..998
    # header, right (the ring with the trader's portrait 980..1152): name / title / reputation / sort row left of it
    ('mcRepContainer',): (844, 132), ('vendorSortLabel_txt',): (762, 179), ('vendorCombo_mc',): (843, 176), ('traderGold_mc',): (999, 202),
    ('balloon_mc',): (945, 20), ('traderImg_mc',): (1023, 60), ('traderTooltip_mc',): (801, 60),
    ('closeX_mc',): (1086, 0),                                               # x1.6: the X 1105..1142 x 19..56, hit 74 in the corner (iPad 50 at 1105..1155 x 5..55)
    # the bottom row (y 935..999): junk under the player's inventory, repair / identify under the trader's (iPad 904 / 972 under 796..1139)
    ('offerJunk_mc',): (Fraction(697, 5), Fraction(4678, 5)), ('repair_mc',): (908, 935), ('identify_mc',): (976, 935),
}
TR_X = {('namesHolder_mc', 'playerName_txt'): 85, ('namesHolder_mc', 'title_txt'): 762, ('namesHolder_mc', 'label_txt'): 802}
TR_WIDTH = {('namesHolder_mc', 'playerName_txt'): 186, ('namesHolder_mc', 'title_txt'): 210, ('namesHolder_mc', 'label_txt'): 122,
            ('vendorSortLabel_txt',): 76}
TR_SCALE = {('characterList_mc',): Fraction(4, 5), ('closeX_mc',): Fraction(8, 5), ('offerJunk_mc',): Fraction(19, 10), ('repair_mc',): Fraction(64, 55),
            ('identify_mc',): Fraction(64, 55), ('clearSupplyBtn_mc',): Fraction(7, 5), ('clearDemandBtn_mc',): Fraction(7, 5)}
TR_SCALEX = {('traderTooltip_mc',): Fraction(379, 40)}                      # the trader's hover zone 891..1304 -> 801..1104 (PC scaleX 12.915)
TR_PILLAR = (4, int(round(_BAL_X - 652)), 9)  # child index, x shift, timeline depth of the unnamed shape behind balance_mc (652..709 x 16..185)
# finger pads (object px x its scale = on-screen px): junk 96 x 72, OK the bar x 64 high, repair / identify 72, balance 64, clear 50 (close: its
# own 46 px hit area x1.6)
TR_PAD = [('offerJunk_mc', (96, 72)), ('ok_mc', (308, 64)), ('clearSupplyBtn_mc', 50), ('clearDemandBtn_mc', 50), ('repair_mc', 72), ('identify_mc', 72),
          ('balance_mc', 64)]
TR_CELL_PC = 55                                                              # the PC Cell symbol (cellWidth 55)

# container (PC containerInventory.swf: stage 1160 x 1080; inv_mc's own drawing without the per-type background 184..631 x 310..756)
CI_K = Fraction(76, 55)                                                      # cells 55 -> 76 on screen
CI_STAGE = (1400, 1080)
CI_CENTRE = (407.5, 533.0)                                                   # inv_mc px centred on the stage (the iPad centres the same block)
CI_X, CI_Y = int(round(CI_STAGE[0] / 2.0 - CI_CENTRE[0] * float(CI_K))), int(round(CI_STAGE[1] / 2.0 - CI_CENTRE[1] * float(CI_K)))

# reward (PC reward.swf: stage 480 x 588, anchored centre; cells 56)
RW_K = Fraction(76, 56)
RW_STAGE = (651, 798)                                                        # 480 x 588 x RW_K
RW_ACC = (161.3, 465.9, 156, 32)                                             # acceptBtn x, y, w, h in reward_mc
RW_ACC_K = Fraction(7, 5)
RW_ACC_XY = (RW_ACC[0] + RW_ACC[2] / 2.0 - RW_ACC[2] * float(RW_ACC_K) / 2.0, RW_ACC[1] + RW_ACC[3] / 2.0 - RW_ACC[3] * float(RW_ACC_K) / 2.0)

# item splitter (PC itemSplitter.swf: stage 484 x 350; splitter_mc 484 x 340)
IS_K = Fraction(33, 25)                                                      # 1.32: iPad 638 x 478
IS_STAGE = (639, 462)
IS_BTN = (150.0, 214.9, (61 + 421) / 2.0, (203 + 256) / 2.0)                 # buttonGroup_mc x, y and the centre of its drawing (splitter_mc px)
IS_BTN_K = Fraction(5, 4)
IS_BTN_XY = (IS_BTN[2] - (IS_BTN[2] - IS_BTN[0]) * float(IS_BTN_K), IS_BTN[3] - (IS_BTN[3] - IS_BTN[1]) * float(IS_BTN_K))

# dialog (PC dialog.swf: stage 1920 x 1080, setPosition bottom / screen / bottom; dialog_mc's window art = an unnamed shape (child #2,
# depth 4) drawn 274..1646 x 678..1126, the window seen 284..1636 x 680..1080 = 1352 x 400: left strip 284..360 (buttons, chain), the
# text 360..1562, right strip 1562..1636 (the scroll bar slot 1575..1603, a red pillar). iPad dialog_t: window 1160 x 400 at the bottom,
# text holder at 85.7 (x0.933, frame 1070), trade / stop listening 50 x 100 on the left strip (y 30 / 277), no inventory button.
DL_ART = 2                                                                   # child index of the art shape in dialog_mc's frame 1
DL_SHIFT = 96                                                                # (1352 - 1160) / 2: the strips move in by this
DL_K = Fraction(1010, 1202)                                                  # the text part 360..1562 -> 456..1466
DL_SLICES = [(274, 360, 274 + DL_SHIFT, 1), (360, 1562, 360 + DL_SHIFT, DL_K), (1562, 1646, 1562 - DL_SHIFT, 1)]   # x0, x1 -> new x, scaleX
DL_Y0, DL_H = 670, 460                                                       # the slices' rows (the art 678..1126)
DL_COPIES = ['dmDlgL', 'dmDlgR']                                             # the art shape itself is the middle slice
DL_HIT = (303 + DL_SHIFT, Fraction(1617 - 303 - 2 * DL_SHIFT, 1314))         # hit_mc (the window's mouse area 303..1617) -> 399..1521
DL_TEXT_X = 469                                                              # textHolder_mc (PC 375; 360 + 15 x DL_K + 96)
DL_TEXT_W = 1010                                                             # the text list's frame (PC 1200): its right edge stays at the scroll slot
DL_TC_W = 810                                                                # TextComp.text_txt width (PC 1000 in the 1200 frame)
DL_KW_W, DL_Q_W = 785, 660                                                   # KeywordTextWidth (PC 975), questionWidth (850)
# the left strip, top to bottom (iPad: trade at the top, stop listening at the bottom): trade 56 x 76, the inventory button 56 x 76 (PC: at the
# bottom; the iPad has none), stop listening 69 x 69 (PC: top right in the text)
DL_POS = {('trade_mc',): (401, 698), ('invBtn_mc',): (401, 852), ('stoplisten_mc',): (Fraction(789, 2), 1005), ('hit_mc',): (DL_HIT[0], 682),
          ('textHolder_mc',): (DL_TEXT_X, 706)}
DL_PAD = [('trade_mc', (76, 100)), ('invBtn_mc', (76, 100)), ('stoplisten_mc', (84, 100))]
# every answer (AnswerKeyword: number + text from x 0, 30 px a line) a full-width row: a transparent rect from x ANSWER_X, ANSWER_W wide,
# ANSWER_PAD above and below its text (the rows stack: the PC puts the next answer at this one's y + height - 2)
ANSWER_W, ANSWER_X, ANSWER_PAD = 836, -6, 12                                 # the answers sit at x 150 of the text list: to 980 (the bar is at 992)
# one-finger scroll (dialog text + answers, waypoint list): list px of travel before a press becomes a scroll; ms a tap / answer is ignored
# after the release that ended a scroll (the release is a CLICK on what is under the finger too)
DRAG_START, DRAG_GUARD = 14, 300
DL_SCROLL_X = (0, DL_TEXT_W - 24)                                            # textHolder px a scroll may start in (not on the buttons, not on the bar)

# waypoints
DTAP_MS = 450
WP_SCROLL_X = (0, 556)                                                       # listCont px (the 560 frame; its scroll bar at 560)

SRC = {'trade_p.swf': 'trade.swf', 'containerInventory_p.swf': 'containerInventory.swf', 'dialog_p.swf': 'dialog.swf', 'reward_p.swf': 'reward.swf',
       'waypoints_p.swf': 'waypoints.swf', 'itemSplitter_p.swf': 'itemSplitter.swf'}
# the host's table k_touch (dm/host/hle_dos2_touchui.c): PC UI type ids the engine creates in mode 0 (tools/dos2_ui_types.py sw main.raw)
TYPES = {'trade_p': [0x2e], 'containerInventory_p': [0x09, 0x25], 'dialog_p': [0x0e], 'reward_p': [0x88], 'waypoints_p': [0x2f], 'itemSplitter_p': [0x15]}
EI_ALLOWED_NEW = {'setAnchor', 'setPosition', 'setMcSize'}                   # engine UIObject calls a movie may add (all in main.raw)


# ---------------------------------------------------------------- ABC: a movie with its one ABC block, classes patched append-only
class Movie:
    def __init__(s, data):
        s.swf = Swf(data); (s.tag, s.pre, s.abc), = s.swf.abcs(); s.made = []
        s.EI = s.abc.pub('ExternalInterface', 'flash.external')
    def P(s, name): return s.abc.pub(name)
    def S(s, text): return s.abc.string(text)
    def done(s): s.swf.tags[s.tag][1] = s.pre + s.abc.write(); return s.swf.write()


class Cls:
    """one class of the movie: methods appended, a trait re-pointed with the original kept under another (public) name"""
    def __init__(s, mv, name):
        s.mv = mv; abc = mv.abc; s.ci = abc.class_index(name); s.inst = abc.instances[s.ci]; s.name = name
        assert not s.inst['flags'] & 1, '%s is sealed' % name
        ref = [abc.methods[t.b].body for t in s.inst['traits'] if t.kind & 15 == 1 and abc.methods[t.b].body]
        assert ref, name
        sc = [r.init_scope for r in ref]; s.scope = max(set(sc), key=sc.count)   # the class's method scope depth (a frame script with an activation differs)
        assert sc.count(s.scope) * 2 > len(sc), '%s: no common method scope depth %s' % (name, sc)
    def P(s, n): return s.mv.P(n)
    def S(s, t): return s.mv.S(t)
    def add(s, name, nparams, lines, like=None):
        abc = s.mv.abc
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 255, 255, s.scope, s.scope + 1)   # verify() sets the real counts
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        if like is not None:
            abc.methods[m].ret = like.ret
            if like.flags & 0x08: abc.methods[m].flags |= 0x08; abc.methods[m].options = list(like.options)
        s.mv.made.append(m); return m
    def method(s, name, nparams, lines):
        assert s.mv.abc.trait(s.ci, name) is None, '%s::%s exists' % (s.name, name)
        s.inst['traits'].append(Trait(s.P(name), 1, 0, s.add(name, nparams, lines)))
    def wrap(s, name, keep, body):
        """trait `name` -> a new method: body(B) writes it (B.orig() calls the original, kept as `keep`)"""
        abc = s.mv.abc; t = abc.trait(s.ci, name); assert t is not None and t.kind & 15 == 1, '%s::%s' % (s.name, name)
        assert abc.trait(s.ci, keep) is None, 'already patched: %s::%s' % (s.name, keep)
        orig = abc.methods[t.b]; n = len(orig.params)
        b = B(s, n, keep); body(b)
        m = s.add(name, n, b.code(), like=orig)
        old = t.b; t.b = m; s.inst['traits'].append(Trait(s.P(keep), 1, 0, old))


def num(v):
    """push a number: ints as pushbyte / pushshort, fractions as a division"""
    f = Fraction(v).limit_denominator(1000)
    if f.denominator != 1: return num(f.numerator) + num(f.denominator) + [('divide',)]
    v = int(f)
    if -128 <= v <= 127: return [('pushbyte', v & 0xff)]
    assert abs(v) < 32768, v
    return [('pushshort', v)] if v >= 0 else [('pushshort', -v), ('negate',)]


class B:
    """a method body: locals after the parameters, nulled at the head; every object path is null-checked"""
    def __init__(s, c, nparams, keep=None):
        s.c = c; s.n = nparams; s.keep = keep; s.L = []; s.nloc = 0; s.nlab = 0
    def P(s, n): return s.c.P(n)
    def S(s, t): return s.c.S(t)
    def loc(s): s.nloc += 1; return s.n + s.nloc
    def lab(s): s.nlab += 1; return 'k%d' % s.nlab
    def code(s):
        head = [('getlocal0',), ('pushscope',)]
        for k in range(s.nloc): head += [('pushnull',), ('coerce_a',), ('setlocal', s.n + 1 + k)]
        return head + s.L + [('returnvoid',)]
    def emit(s, *lines): s.L += list(lines)
    # values
    def path(s, path, base=None):
        """lines pushing this.<path> (or local `base`.<path>)"""
        return ([('getlocal', base)] if base is not None else [('getlocal0',)]) + [('getproperty', s.P(p)) for p in path]
    def orig(s):
        s.emit(('getlocal0',), *[('getlocal', k + 1) for k in range(s.n)], ('callpropvoid', s.P(s.keep), s.n))
    def ei(s, name, *args):
        s.emit(('getlex', s.c.mv.EI), ('pushstring', s.S(name)), *[x for a in args for x in a], ('callpropvoid', s.P('call'), 1 + len(args)))
    def strv(s, t): return [('pushstring', s.S(t))]
    # statements on an object: obj(path) loads it into a fresh local and returns (local, label); close(label) after the statements
    def obj(s, path, base=None):
        o = s.loc(); k = s.lab()
        s.emit(*s.path(path, base), ('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', k))
        return o, k
    def close(s, k): s.emit(('L', k))
    def setp(s, o, prop, val): s.emit(('getlocal', o), *val, ('setproperty', s.P(prop)))
    def addp(s, o, prop, d): s.emit(('getlocal', o), ('getlocal', o), ('getproperty', s.P(prop)), *num(d), ('add',), ('setproperty', s.P(prop)))
    def callv(s, o, meth, *args): s.emit(('getlocal', o), *[x for a in args for x in a], ('callpropvoid', s.P(meth), len(args)))
    def set_this(s, prop, val): s.emit(('getlocal0',), *val, ('setproperty', s.P(prop)))
    # layout ops (shared with the render: see ops_apply)
    def apply(s, ops, base=None):
        for op in ops:
            o, k = s.obj(op[1], base)
            if op[0] == 'pos': s.setp(o, 'x', num(op[2])); s.setp(o, 'y', num(op[3]))
            elif op[0] == 'x': s.setp(o, 'x', num(op[2]))
            elif op[0] == 'move':
                if op[2]: s.addp(o, 'x', op[2])
                if op[3]: s.addp(o, 'y', op[3])
            elif op[0] == 'scale': s.setp(o, 'scaleX', num(op[2])); s.setp(o, 'scaleY', num(op[2]))
            elif op[0] == 'scalex': s.setp(o, 'scaleX', num(op[2]))
            elif op[0] == 'rot': s.setp(o, 'rotation', num(op[2]))
            elif op[0] == 'width': s.setp(o, 'width', num(op[2]))
            elif op[0] == 'pad': s.pad(o, op[2])
            else: raise ValueError(op)
            s.close(k)
    def pad(s, o, size):
        """a transparent w x h (object px; one number = a square) rect centred on what the object draws: a finger-sized hit area"""
        w, h = size if isinstance(size, tuple) else (size, size)
        b, g = s.loc(), s.loc(); k = s.lab()
        s.emit(('getlocal', o), ('getlocal', o), ('callproperty', s.P('getBounds'), 1), ('coerce_a',), ('setlocal', b),
               ('getlocal', o), ('getproperty', s.P('graphics')), ('coerce_a',), ('setlocal', g), ('getlocal', g), ('iffalse', k),
               ('getlocal', g), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', s.P('beginFill'), 2), ('getlocal', g))
        for pos, sz, v in (('x', 'width', w), ('y', 'height', h)):
            s.emit(('getlocal', b), ('getproperty', s.P(pos)), ('getlocal', b), ('getproperty', s.P(sz)), ('pushbyte', 2), ('divide',), ('add',),
                   *num(Fraction(v) / 2), ('subtract',))
        s.emit(*num(w), *num(h), ('callpropvoid', s.P('drawRect'), 4), ('getlocal', g), ('callpropvoid', s.P('endFill'), 0), ('L', k))


def ops_pad(pads, scale_of=lambda n: 1):
    def per(size, k): return tuple(Fraction(v) / k for v in size) if isinstance(size, tuple) else Fraction(size) / k
    return [('pad', (n,), per(size, scale_of(n))) for n, size in pads]


def rect_bytes(w, h):
    vals = [0, w * 20, 0, h * 20]; nb = max(v.bit_length() for v in vals) + 1
    bits = format(nb, '05b') + ''.join(format(v, '0%db' % nb) for v in vals); bits += '0' * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def set_stage(swf, w, h): swf.head = rect_bytes(w, h) + swf.head[-4:]


# ---------------------------------------------------------------- trade
def trade_ops():
    """the layout ops of trade_1 (paths from trade_mc); also used by the render"""
    ops = [('pos', (n,), x, y) for n, (x, y) in TR_GRID.items()] + [('pos', (n,), *TR_GRID[g]) for n, g in TR_SBMC.items()]
    ops += [('pos', (n,), x, y) for n, (x, y) in TR_TABS.items()] + [('rot', (n,), -90) for n in TR_TABS]
    ops += [('scale', p, k) for p, k in TR_SCALE.items()] + [('scalex', p, k) for p, k in TR_SCALEX.items()]
    ops += [('pos', p, x, y) for p, (x, y) in TR_POS.items()] + [('x', p, x) for p, x in TR_X.items()]
    ops += [('width', p, w) for p, w in TR_WIDTH.items()]
    # the engine's draw placeholders (iggy_*: a red bg_mc) as wide as the new grids (their height follows the rows: updateRows)
    for holder, w in (('playerInventory_mc', TR_INV_W), ('playerExchange_mc', TR_OFF_W), ('traderExchange_mc', TR_OFF_W), ('traderInventory_mc', TR_INV_W)):
        ops.append(('width', (holder, 'iggy_' + holder[:-3], 'bg_mc'), w - 1))
    ops += ops_pad(TR_PAD, lambda n: TR_SCALE.get((n,), 1))
    return ops


def trade(data):
    """trade.swf -> trade_p.swf"""
    swf = Swf(data)
    # the frame in slices: plain sprites holding the PC frame symbol, placed on the root; trade_1 moves them under its frame_mc
    g = gr.Graft(swf); recs = gr.frame_state(swf.tags[gr.Movie(swf).defs[gr.Movie(swf).resolve('trade_fla.trade_1')]][1], 1)
    fid = [r['char'] for r in recs.values() if r['name'] == b'frame_mc\0'][0]
    for nm in TR_COPIES: g.place(g.sprite([(fid, 1, (0, 0), None)]), 0, 0, nm)
    g.finish(); set_stage(swf, TR_W, TR_H)
    mv = Movie(swf.write(compress=False)); t = Cls(mv, 'trade_fla.trade_1'); root = Cls(mv, 'trade_fla.MainTimeline')
    Rect = mv.abc.pub('Rectangle', 'flash.geom')
    def rect(b, x, y, w, h): return [('findpropstrict', Rect)] + num(x) + num(y) + num(w) + num(h) + [('constructprop', Rect, 4)]
    # dmSlices(): frame_mc = the centre column, the copies = the other slices (right under frame_mc). Nothing is cut until frame_mc and every
    # copy are found (frame1: the root's copies may not exist yet; onEventInit again), so the PC frame stays whole if one is missing; idempotent
    b = B(t, 0)
    fr, par, end = b.loc(), b.loc(), b.lab()
    b.emit(('getlocal0',), ('getproperty', b.P('frame_mc')), ('coerce_a',), ('setlocal', fr), ('getlocal', fr), ('iffalse', end),
           ('getlocal0',), ('getproperty', b.P('parent')), ('coerce_a',), ('setlocal', par), ('getlocal', par), ('iffalse', end))
    cps = []
    for nm in TR_COPIES:
        o = b.loc(); cps.append(o)
        b.emit(('getlocal', par), ('pushstring', b.S(nm)), ('callproperty', b.P('getChildByName'), 1), ('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', end))
    for o, (x0, x1, y0, y1, nx, k) in zip([fr] + cps, TR_SLICES):
        if o != fr:
            b.emit(('getlocal0',), ('getlocal', o), ('getlocal0',), ('getlocal', fr), ('callproperty', b.P('getChildIndex'), 1), ('callpropvoid', b.P('addChildAt'), 2))
            b.setp(o, 'mouseEnabled', [('pushfalse',)])
        b.setp(o, 'scrollRect', rect(b, x0, y0, x1 - x0, y1 - y0)); b.setp(o, 'x', num(nx)); b.setp(o, 'y', num(y0)); b.setp(o, 'scaleX', num(k))
    b.emit(('L', end))
    t.method('dmSlices', 0, b.code())
    # frame1: the original (lists made at the PC numbers), then the iPad's grid numbers, frames, scroll bars, tab rows, the layout
    def f1(b):
        b.orig()
        for prop, v in (('cellBorder', TR_BORDER), ('cellHeight', TR_CELL), ('cellWidth', TR_CELL), ('cellSpacing', TR_SPACING), ('bgDiscrap', TR_DISCRAP),
                        ('invCols', TR_INV_COLS), ('offerCols', TR_OFF_COLS), ('rows', TR_INV_ROWS), ('minRows', TR_INV_ROWS), ('offerRows', TR_OFF_ROWS),
                        ('minOfferRows', TR_OFF_ROWS), ('scrollInvRectWidth', TR_INV_W), ('scrollOfferRectWidth', TR_OFF_W)):
            b.set_this(prop, num(v))
        for n in TR_LISTS:
            o, k = b.obj((n,)); b.setp(o, 'SB_SPACING', num(TR_SB[n]))
            inv = 'Inventory' in n
            b.callv(o, 'setFrame', num(TR_INV_W if inv else TR_OFF_W), num(TR_INV_H if inv else TR_OFF_H))
            b.setp(o, 'x', num(0)); b.setp(o, 'y', num(0))
            b.close(k)
        for n in ('playerTabList', 'traderTabList'):   # PC: 12 apart, the trader's mirrored (scaleX -1) to grow left from the right strip
            o, k = b.obj((n,)); b.setp(o, 'EL_SPACING', num(TR_TAB_SP)); b.setp(o, 'scaleX', num(1)); b.close(k)
        b.apply(trade_ops())
        # the unnamed balance pillar shape (depth 9 = child index TR_PILLAR[0] while the timeline's children are all there is) moves with balance_mc
        o = b.loc(); k = b.lab(); Shape = mv.abc.pub('Shape', 'flash.display')
        b.emit(('getlocal0',), ('getproperty', b.P('numChildren')), *num(TR_PILLAR[0] + 1), ('lessthan',), ('iftrue', k),
               ('getlocal0',), *num(TR_PILLAR[0]), ('callproperty', b.P('getChildAt'), 1), ('coerce_a',), ('setlocal', o),
               ('getlocal', o), ('getlex', Shape), ('istypelate',), ('iffalse', k), ('getlocal', o), ('getproperty', b.P('x')), ('pushbyte', 0), ('ifne', k))
        b.setp(o, 'x', num(TR_PILLAR[1])); b.emit(('L', k))
        b.emit(('getlocal0',), ('callpropvoid', b.P('dmSlices'), 0))
    t.wrap('frame1', 'dmF1Orig', f1)
    # addTab / addTraderTab (engine): the original, then the new tab (the list's last element) turned +90 (its holder is turned -90)
    def tab(lst):
        def body(b):
            b.orig()
            o, k = b.obj((lst,)); e = b.loc(); k2 = b.lab()
            b.emit(('getlocal', o), ('getproperty', b.P('length')), ('pushbyte', 1), ('lessthan',), ('iftrue', k2),
                   ('getlocal', o), ('getlocal', o), ('getproperty', b.P('length')), ('pushbyte', 1), ('subtract',), ('callproperty', b.P('getAt'), 1),
                   ('coerce_a',), ('setlocal', e), ('getlocal', e), ('iffalse', k2))
            b.setp(e, 'rotation', num(90)); b.emit(('L', k2)); b.close(k)
        return body
    t.wrap('addTab', 'dmTabOrig', tab('playerTabList'))
    t.wrap('addTraderTab', 'dmTTabOrig', tab('traderTabList'))
    # extendGrid(list, rows): the original, then every Cell of the four grids drawn at the cell size (the PC Cell symbol is 55 px)
    def ext(b):
        b.orig()
        i, c = b.loc(), b.loc()
        for n in TR_LISTS:
            o, k = b.obj((n,)); top, nxt, end = b.lab(), b.lab(), b.lab()
            b.emit(('pushbyte', 0), ('setlocal', i), ('L', top), ('getlocal', i), ('getlocal', o), ('getproperty', b.P('length')), ('ifnlt', end),
                   ('getlocal', o), ('getlocal', i), ('callproperty', b.P('getAt'), 1), ('coerce_a',), ('setlocal', c), ('getlocal', c), ('iffalse', nxt))
            b.setp(c, 'scaleX', num(Fraction(TR_CELL, TR_CELL_PC))); b.setp(c, 'scaleY', num(Fraction(TR_CELL, TR_CELL_PC)))
            b.emit(('L', nxt), ('getlocal', i), ('increment',), ('setlocal', i), ('jump', top), ('L', end))
            b.close(k)
    t.wrap('extendGrid', 'dmExtOrig', ext)
    # init(): the original (it tells the engine setMcSize(frame_mc) and sets the PC scroll bar lengths 770 / 700), then the whole window and
    # the new lengths
    def ini(b):
        b.orig(); b.ei('setMcSize', num(TR_W), num(TR_H))
        for n in TR_LISTS:
            o, k = b.obj((n, 'm_scrollbar_mc')); b.callv(o, 'setLength', num(TR_INV_SB if 'Inventory' in n else TR_OFF_SB)); b.close(k)
    t.wrap('init', 'dmInitOrig', ini)
    # onContainerUp(e): the PC sends itemClick (= the engine moves the item across; the iPad's DoubleTap sends the same itemClick) on every
    # press + release on one slot. As the iPad: a first tap only selects (downList = -1 skips the PC's itemClick; stopDragging / slotUp stay),
    # a second tap on the same slot within DTAP_MS runs the PC code as it is (itemClick). The PC's own itemDoubleClick is not in main.raw.
    def up(b):
        tm, k, first = b.loc(), b.lab(), b.lab()
        b.emit(('getlocal0',), ('getproperty', b.P('currentHLList')), *num(-1), ('ifeq', k),
               ('getlocal0',), ('getproperty', b.P('currentHLList')), ('getlocal0',), ('getproperty', b.P('downList')), ('ifne', k),
               ('getlocal0',), ('getproperty', b.P('currentHLSlot')), ('getlocal0',), ('getproperty', b.P('downSlot')), ('ifne', k),
               *_now(mv), ('setlocal', tm),
               ('getlocal0',), ('getproperty', b.P('dmTapL')), ('getlocal0',), ('getproperty', b.P('currentHLList')), ('ifne', first),
               ('getlocal0',), ('getproperty', b.P('dmTapS')), ('getlocal0',), ('getproperty', b.P('currentHLSlot')), ('ifne', first),
               ('getlocal', tm), ('getlocal0',), ('getproperty', b.P('dmTapT')), ('subtract',), *num(DTAP_MS), ('ifnlt', first))
        b.set_this('dmTapS', num(-1)); b.emit(('jump', k), ('L', first))
        b.set_this('dmTapL', b.path(('currentHLList',))); b.set_this('dmTapS', b.path(('currentHLSlot',))); b.set_this('dmTapT', [('getlocal', tm)])
        b.set_this('downList', num(-1))
        b.emit(('L', k)); b.orig()
    t.wrap('onContainerUp', 'dmUpOrig', up)
    # onEventInit(): the original (anchor top / screen / top), then the iPad's centre / screen / centre; the slices again (all root children exist)
    def ev(b):
        b.orig(); b.ei('setAnchor', b.strv('center'), b.strv('screen'), b.strv('center'))
        o, k = b.obj(('trade_mc',)); b.callv(o, 'dmSlices'); b.close(k)
    root.wrap('onEventInit', 'dmEvInitOrig', ev)
    return mv.done()


# ---------------------------------------------------------------- uniformly scaled windows: container, reward, item splitter
def scaled(data, cls, stage, ops, extra=None, wraps=('onEventInit',), more=None):
    swf = Swf(data); set_stage(swf, *stage)
    mv = Movie(swf.write(compress=False)); c = Cls(mv, cls)
    if more: more(mv)
    b = B(c, 0); b.apply(ops)
    c.method('dmLay', 0, b.code())
    for w in wraps:
        def body(b, w=w):
            b.orig(); b.emit(('getlocal0',), ('callpropvoid', b.P('dmLay'), 0))
            if extra and w == wraps[0]: extra(b)
        c.wrap(w, 'dmOrig_' + w, body)
    return mv.done()


def container_ops(): return [('scale', ('inv_mc',), CI_K), ('pos', ('inv_mc',), CI_X, CI_Y)]
def reward_ops(): return [('scale', ('reward_mc',), RW_K), ('pos', ('reward_mc',), 0, 0), ('scale', ('reward_mc', 'acceptBtn'), RW_ACC_K),
                          ('pos', ('reward_mc', 'acceptBtn'), Fraction(RW_ACC_XY[0]).limit_denominator(10), Fraction(RW_ACC_XY[1]).limit_denominator(10))]
def splitter_ops(): return [('scale', ('splitter_mc',), IS_K), ('pos', ('splitter_mc',), 0, 0), ('scale', ('splitter_mc', 'buttonGroup_mc'), IS_BTN_K),
                            ('pos', ('splitter_mc', 'buttonGroup_mc'), Fraction(IS_BTN_XY[0]).limit_denominator(10), Fraction(IS_BTN_XY[1]).limit_denominator(10))]


def _now(mv):
    gt = mv.abc.pub('getTimer', 'flash.utils'); return [('findpropstrict', gt), ('callproperty', gt, 0)]


def container_taps(mv):
    """inventory_1.onContainerUp(e): the PC takes the item (doubleClickItem) on every release over a filled slot of a container (not the
    player's inventory). As the iPad: a first tap only selects (base.playerInventory = true around the PC code skips that branch), a second tap
    on the same slot within DTAP_MS runs the PC code as it is (take). The PC's double click listener stays (onDoubleClick)."""
    inv = Cls(mv, 'containerInventory_fla.inventory_1')
    def up(b):
        sl, tm, k, first, done = b.loc(), b.loc(), b.lab(), b.lab(), b.lab()
        b.emit(('getlocal0',), ('getlocal0',), ('getproperty', b.P('currentHLSlot')), ('callproperty', b.P('getSlot'), 1), ('coerce_a',), ('setlocal', sl),
               ('getlocal', sl), ('iffalse', k), ('getlocal', sl), ('getproperty', b.P('amount')), ('pushbyte', 0), ('ifngt', k),
               ('getlocal0',), ('getproperty', b.P('base')), ('iffalse', k),
               ('getlocal0',), ('getproperty', b.P('base')), ('getproperty', b.P('isDragging')), ('iftrue', k),
               ('getlocal0',), ('getproperty', b.P('base')), ('getproperty', b.P('playerInventory')), ('iftrue', k),
               *_now(mv), ('setlocal', tm),
               ('getlocal0',), ('getproperty', b.P('dmTapS')), ('getlocal0',), ('getproperty', b.P('currentHLSlot')), ('ifne', first),
               ('getlocal', tm), ('getlocal0',), ('getproperty', b.P('dmTapT')), ('subtract',), *num(DTAP_MS), ('ifnlt', first))
        b.set_this('dmTapS', num(-1)); b.emit(('jump', k), ('L', first))
        b.set_this('dmTapS', b.path(('currentHLSlot',))); b.set_this('dmTapT', [('getlocal', tm)])
        b.emit(('getlocal0',), ('getproperty', b.P('base')), ('pushtrue',), ('setproperty', b.P('playerInventory')))
        b.orig()
        b.emit(('getlocal0',), ('getproperty', b.P('base')), ('pushfalse',), ('setproperty', b.P('playerInventory')), ('jump', done))
        b.emit(('L', k)); b.orig(); b.emit(('L', done))
    inv.wrap('onContainerUp', 'dmUpOrig', up)


def container(data):
    # setAnchor(id, side) moves inv_mc in the PC code (x 328 / 32): the layout again after it; one setPosition centre / screen / centre
    return scaled(data, 'containerInventory_fla.MainTimeline', CI_STAGE, container_ops(),
                  extra=lambda b: b.ei('setPosition', b.strv('center'), b.strv('screen'), b.strv('center')), wraps=('onEventInit', 'setAnchor'),
                  more=container_taps)
def reward(data): return scaled(data, 'reward_fla.MainTimeline', RW_STAGE, reward_ops())
def splitter(data): return scaled(data, 'itemSplitter_fla.MainTimeline', IS_STAGE, splitter_ops())


# ---------------------------------------------------------------- one-finger scroll of a scrollList (dialog text + answers, waypoint list)
def finger_scroll(c, lst, area, xr):
    """methods dmScDown(e) / dmScMove(e) / dmScUp(e) on class c: a press inside this.<area> (its own x in xr) that travels more than
    DRAG_START px vertically scrolls this.<lst> with the finger (scrollTo on its scroll bar, no animation; the bar clamps); while it scrolls the
    list's rows get no mouse (mouseChildren off: no hover selection, the release clicks nothing); dmScMoved / dmScUpT let a wrapper drop a
    click that still comes within DRAG_GUARD ms of the release. A press that never moves is the PC's own click."""
    mv = c.mv; gt = mv.abc.pub('getTimer', 'flash.utils')
    b = B(c, 1); st, a, l, mx = b.loc(), b.loc(), b.loc(), b.loc(); k = b.lab()
    b.emit(('getlocal0',), ('getproperty', b.P('stage')), ('coerce_a',), ('setlocal', st), ('getlocal', st), ('iffalse', k),
           *b.path(area), ('coerce_a',), ('setlocal', a), ('getlocal', a), ('iffalse', k),
           *b.path(lst), ('coerce_a',), ('setlocal', l), ('getlocal', l), ('iffalse', k),
           ('getlocal', l), ('pushtrue',), ('setproperty', b.P('mouseChildren')),          # a release that never came: the rows get the mouse back
           ('getlocal', a), ('getproperty', b.P('mouseX')), ('setlocal', mx),
           ('getlocal', mx), *num(xr[0]), ('lessthan',), ('iftrue', k), ('getlocal', mx), *num(xr[1]), ('greaterthan',), ('iftrue', k))
    b.set_this('dmScY', b.path(('mouseY',))); b.set_this('dmScS', b.path(('m_scrollbar_mc', 'scrolledY'), l))
    b.set_this('dmScOn', [('pushfalse',)]); b.set_this('dmScMoved', [('pushfalse',)])
    # phone run 23.11 (66b3b2d6): the dialog text did not follow the finger - stage MOUSE_MOVE is not relied on any more (tools/dos2_ppanel_skills.py,
    # the note above doc_text_scroll): the scroll follows the finger on this movie clip's ENTER_FRAME while the press is down; mouseUp (stage) ends it
    b.emit(('getlocal0',), ('pushstring', b.S('enterFrame')), ('getlocal0',), ('getproperty', b.P('dmScMove')), ('callpropvoid', b.P('addEventListener'), 2),
           ('getlocal', st), ('pushstring', b.S('mouseUp')), ('getlocal0',), ('getproperty', b.P('dmScUp')), ('callpropvoid', b.P('addEventListener'), 2))
    b.emit(('L', k)); c.method('dmScDown', 1, b.code())
    b = B(c, 1); l, dy, sb = b.loc(), b.loc(), b.loc(); k, go = b.lab(), b.lab()
    b.emit(*b.path(lst), ('coerce_a',), ('setlocal', l), ('getlocal', l), ('iffalse', k),
           ('getlocal0',), ('getproperty', b.P('mouseY')), ('getlocal0',), ('getproperty', b.P('dmScY')), ('subtract',), ('setlocal', dy),
           ('getlocal0',), ('getproperty', b.P('dmScOn')), ('iftrue', go),
           ('getlocal', dy), *num(DRAG_START), ('greaterthan',), ('getlocal', dy), *num(-DRAG_START), ('lessthan',), ('bitor',), ('iffalse', k))
    b.set_this('dmScOn', [('pushtrue',)]); b.set_this('dmScMoved', [('pushtrue',)])
    b.emit(('getlocal', l), ('pushfalse',), ('setproperty', b.P('mouseChildren')), ('L', go),
           ('getlocal', l), ('getproperty', b.P('m_scrollbar_mc')), ('coerce_a',), ('setlocal', sb), ('getlocal', sb), ('iffalse', k),
           ('getlocal', sb), ('getlocal0',), ('getproperty', b.P('dmScS')), ('getlocal', dy), ('subtract',), ('pushfalse',), ('pushfalse',),
           ('callpropvoid', b.P('scrollTo'), 3), ('L', k))
    c.method('dmScMove', 1, b.code())
    b = B(c, 1); st, l = b.loc(), b.loc(); k, k2 = b.lab(), b.lab()
    b.emit(('getlocal0',), ('pushstring', b.S('enterFrame')), ('getlocal0',), ('getproperty', b.P('dmScMove')), ('callpropvoid', b.P('removeEventListener'), 2))
    b.emit(('getlocal0',), ('getproperty', b.P('stage')), ('coerce_a',), ('setlocal', st), ('getlocal', st), ('iffalse', k))
    b.emit(('getlocal', st), ('pushstring', b.S('mouseUp')), ('getlocal0',), ('getproperty', b.P('dmScUp')), ('callpropvoid', b.P('removeEventListener'), 2))
    b.emit(('L', k), ('getlocal0',), ('getproperty', b.P('dmScOn')), ('iffalse', k2))
    b.set_this('dmScUpT', [('findpropstrict', gt), ('callproperty', gt, 0)])
    b.emit(*b.path(lst), ('coerce_a',), ('setlocal', l), ('getlocal', l), ('iffalse', k2), ('getlocal', l), ('pushtrue',), ('setproperty', b.P('mouseChildren')),
           ('L', k2))
    b.set_this('dmScOn', [('pushfalse',)])
    c.method('dmScUp', 1, b.code())


def scroll_guard(b, owner, skip):
    """lines of b: jump to label `skip` when the scroll owner (this.<owner path>, () = this) ended a scroll within DRAG_GUARD ms"""
    gt = b.c.mv.abc.pub('getTimer', 'flash.utils'); o = b.loc(); go = b.lab()
    b.emit(*b.path(owner), ('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', go),
           ('getlocal', o), ('getproperty', b.P('dmScMoved')), ('iffalse', go),
           ('findpropstrict', gt), ('callproperty', gt, 0), ('getlocal', o), ('getproperty', b.P('dmScUpT')), ('subtract',), *num(DRAG_GUARD),
           ('lessthan',), ('iftrue', skip), ('L', go))


# ---------------------------------------------------------------- dialog
def dialog_ops():
    """the layout ops of Dialog_1 (paths from dialog_mc); also used by the render"""
    ops = [('pos', p, x, y) for p, (x, y) in DL_POS.items()] + [('scalex', ('hit_mc',), DL_HIT[1])]
    return ops + ops_pad(DL_PAD)


def dialog(data):
    """dialog.swf -> dialog_p.swf"""
    swf = Swf(data)
    # the window art in three slices: the art shape itself (the text part, narrowed) and two plain sprites holding it (the strips), placed on the
    # root; onEventInit moves them into dialog_mc right under the art
    mvd = gr.Movie(swf); kids = _children(swf, mvd, mvd.resolve('dialog_fla.Dialog_1'))
    art = kids[DL_ART][1]['char']
    g = gr.Graft(swf)
    for nm in DL_COPIES: g.place(g.sprite([(art, 1, (0, 0), None)]), 0, 0, nm)
    g.finish()
    mv = Movie(swf.write(compress=False)); d = Cls(mv, 'dialog_fla.Dialog_1'); root = Cls(mv, 'dialog_fla.MainTimeline')
    Rect, Shape = mv.abc.pub('Rectangle', 'flash.geom'), mv.abc.pub('Shape', 'flash.display')
    def rect(x, y, w, h): return [('findpropstrict', Rect)] + num(x) + num(y) + num(w) + num(h) + [('constructprop', Rect, 4)]
    # dmSlices(): the art (child DL_ART: a Shape over 1000 px wide; kept in dmArt) = the middle slice, the two copies = the strips; idempotent
    b = B(d, 0); art_l, par, k = b.loc(), b.loc(), b.lab(); found = b.lab()
    b.emit(('getlocal0',), ('getproperty', b.P('dmArt')), ('coerce_a',), ('setlocal', art_l), ('getlocal', art_l), ('iftrue', found),
           ('getlocal0',), ('getproperty', b.P('numChildren')), *num(DL_ART + 1), ('lessthan',), ('iftrue', k),
           ('getlocal0',), *num(DL_ART), ('callproperty', b.P('getChildAt'), 1), ('coerce_a',), ('setlocal', art_l),
           ('getlocal', art_l), ('getlex', Shape), ('istypelate',), ('iffalse', k),
           ('getlocal', art_l), ('getproperty', b.P('width')), *num(1000), ('ifngt', k))
    b.set_this('dmArt', [('getlocal', art_l)])
    b.emit(('L', found), ('getlocal0',), ('getproperty', b.P('parent')), ('coerce_a',), ('setlocal', par), ('getlocal', par), ('iffalse', k))
    cp = []
    for nm in DL_COPIES:
        o = b.loc(); cp.append(o)
        b.emit(('getlocal', par), ('pushstring', b.S(nm)), ('callproperty', b.P('getChildByName'), 1), ('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', k))
    for o, (x0, x1, nx, kx) in zip([cp[0], art_l, cp[1]], DL_SLICES):
        if o != art_l:
            b.emit(('getlocal0',), ('getlocal', o), ('getlocal0',), ('getlocal', art_l), ('callproperty', b.P('getChildIndex'), 1), ('callpropvoid', b.P('addChildAt'), 2))
            b.setp(o, 'mouseEnabled', [('pushfalse',)])
        b.setp(o, 'scrollRect', rect(x0, DL_Y0, x1 - x0, DL_H)); b.setp(o, 'x', num(nx)); b.setp(o, 'y', num(DL_Y0)); b.setp(o, 'scaleX', num(kx))
    b.emit(('L', k)); d.method('dmSlices', 0, b.code())
    # frame1: the original (the text list made at the PC numbers), then the iPad's places, the narrower text list and answer / text widths
    def f1(b):
        b.orig(); b.apply(dialog_ops())
        o, k = b.obj(('textContainer',)); b.callv(o, 'setFrameWidth', num(DL_TEXT_W)); b.close(k)
        b.set_this('KeywordTextWidth', num(DL_KW_W)); b.set_this('questionWidth', num(DL_Q_W))
    d.wrap('frame1', 'dmF1Orig', f1)
    # addText_Internal(...): the original, then a new text block (lastText: every new TextComp is it) re-wrapped at the narrower width
    def txt(b):
        b.orig()
        o, k = b.obj(('lastText',)); t, k2 = b.obj(('text_txt',), o)
        b.emit(('getlocal', t), ('getproperty', b.P('width')), *num(DL_TC_W), ('ifeq', k2))
        b.setp(t, 'width', num(DL_TC_W))
        b.emit(('getlocal0',), ('getlocal', o), ('getlocal0',), ('getproperty', b.P('minHeight')), ('callpropvoid', b.P('setTextHolderHeight'), 2))
        b.close(k2); b.close(k)
    d.wrap('addText_Internal', 'dmTextOrig', txt)
    # addKeywordAnswer (the engine's answers: MainTimeline.updateDialog -> addKWAnswerArray) / addAnswers (PC, not called by the engine): the
    # original, then the answer it added (the last child of the answer container) gets a full-width row rect
    def add(b):
        b.orig()
        o, k = b.obj(('playerOptionsHolder', 'container_mc')); a = b.loc(); k2 = b.lab()
        b.emit(('getlocal', o), ('getproperty', b.P('numChildren')), ('pushbyte', 1), ('lessthan',), ('iftrue', k2),
               ('getlocal', o), ('getlocal', o), ('getproperty', b.P('numChildren')), ('pushbyte', 1), ('subtract',), ('callproperty', b.P('getChildAt'), 1),
               ('coerce_a',), ('setlocal', a), ('getlocal', a), ('iffalse', k2))
        bb, g = b.loc(), b.loc()
        b.emit(('getlocal', a), ('getlocal', a), ('callproperty', b.P('getBounds'), 1), ('coerce_a',), ('setlocal', bb),
               ('getlocal', a), ('getproperty', b.P('graphics')), ('coerce_a',), ('setlocal', g), ('getlocal', g), ('iffalse', k2),
               ('getlocal', g), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', b.P('beginFill'), 2),
               ('getlocal', g), *num(ANSWER_X), ('getlocal', bb), ('getproperty', b.P('y')), *num(ANSWER_PAD), ('subtract',), *num(ANSWER_W),
               ('getlocal', bb), ('getproperty', b.P('height')), *num(2 * ANSWER_PAD), ('add',), ('callpropvoid', b.P('drawRect'), 4),
               ('getlocal', g), ('callpropvoid', b.P('endFill'), 0), ('L', k2))
        b.close(k)
    d.wrap('addKeywordAnswer', 'dmKwOrig', add)
    d.wrap('addAnswers', 'dmAddOrig', add)
    # executeSelection(id): dropped right after a scroll (the release that ends it is a CLICK on an answer)
    def ex(b):
        k = b.lab(); scroll_guard(b, (), k); b.orig(); b.emit(('L', k))
    d.wrap('executeSelection', 'dmExOrig', ex)
    finger_scroll(d, ('textContainer',), ('textHolder_mc',), DL_SCROLL_X)
    # onEventInit(): the original, then the art slices (the copies on the root exist now) and the press listener on the dialog window
    def ev(b):
        b.orig()
        o, k = b.obj(('dialog_mc',)); b.callv(o, 'dmSlices')
        b.emit(('getlocal', o), ('pushstring', b.S('mouseDown')), ('getlocal', o), ('getproperty', b.P('dmScDown')), ('callpropvoid', b.P('addEventListener'), 2))
        b.close(k)
    root.wrap('onEventInit', 'dmEvInitOrig', ev)
    return mv.done()


# ---------------------------------------------------------------- waypoints: the shared PANEL_CLOSE movie + double tap = travel + finger scroll
def waypoints(data):
    mv = Movie(data); lst = Cls(mv, 'waypoints_fla.WaypointsList_1'); wp = Cls(mv, 'Waypoint')
    getTimer = mv.abc.pub('getTimer', 'flash.utils')
    b = B(lst, 1); t = b.loc(); k = b.lab()
    b.emit(('findpropstrict', getTimer), ('callproperty', getTimer, 0), ('setlocal', t),
           ('getlocal1',), ('getlocal0',), ('getproperty', b.P('dmTapId')), ('ifne', k),
           ('getlocal', t), ('getlocal0',), ('getproperty', b.P('dmTapT')), ('subtract',), *num(DTAP_MS), ('ifnlt', k))
    b.set_this('dmTapId', num(-1)); b.emit(('getlocal0',), ('callpropvoid', b.P('useWP'), 0), ('returnvoid',), ('L', k))
    b.set_this('dmTapId', [('getlocal1',)]); b.set_this('dmTapT', [('getlocal', t)])
    lst.method('dmTap', 1, b.code())
    finger_scroll(lst, ('listCont',), ('listCont',), WP_SCROLL_X)
    # frame1: the original (listCont made), then the press listener on the list
    def f1(b):
        b.orig()
        o, k = b.obj(('listCont',))
        b.emit(('getlocal', o), ('pushstring', b.S('mouseDown')), ('getlocal0',), ('getproperty', b.P('dmScDown')), ('callpropvoid', b.P('addEventListener'), 2))
        b.close(k)
    lst.wrap('frame1', 'dmF1Orig', f1)
    # Waypoint.onUp: nothing right after a scroll; else the original (select) + dmTap(id)
    def up(b):
        skip = b.lab(); scroll_guard(b, ('base',), skip)
        b.orig()
        o, k = b.obj(('base',)); b.callv(o, 'dmTap', b.path(('id',))); b.close(k)
        b.emit(('L', skip))
    wp.wrap('onUp', 'dmUpOrig', up)
    return mv.done()


# ---------------------------------------------------------------- the hook into tools/dos2_pchud_touch.py
MAKE = {'trade_p.swf': trade, 'containerInventory_p.swf': container, 'dialog_p.swf': dialog, 'reward_p.swf': reward, 'itemSplitter_p.swf': splitter}
_host = None


def _wp(data):
    """waypoints_p.swf: the shared tool's close-button movie (PANEL_CLOSE), then the double tap"""
    return waypoints(_host.closer(data, _host.PANEL_CLOSE['waypoints'])[0])


def sources(path):
    need = sorted(set(SRC.values()))
    if os.path.isdir(path): return {n: (Path(path) / n).read_bytes() for n in need}
    import lspk
    pak = lspk.Pak(path); by = {e[0]: e for e in pak.entries}
    miss = [n for n in need if GUI + n not in by]
    if miss: sys.exit('%s: no %s' % (path, ' '.join(miss)))
    return {n: pak.read(by[GUI + n]) for n in need}


def build(src):
    out = {o: MAKE[o](src[SRC[o]]) for o in MAKE}
    if _host is not None: out['waypoints_p.swf'] = _wp(src[SRC['waypoints_p.swf']])
    return out


def install(host):
    """called at the end of tools/dos2_pchud_touch.py: its sources() / build() / check() also make and check these movies"""
    global _host
    _host = host
    h_sources, h_build, h_check = host.sources, host.build, host.check
    def sources_(path):
        s = h_sources(path); s.update(sources(path)); return s
    def build_(src):
        out = h_build(src); out.update(build(src)); return out
    def check_(src): return h_check(src) + check(src)
    host.sources, host.build, host.check = sources_, build_, check_


# ---------------------------------------------------------------- checks
def ei_names(abc):
    import dos2_ui_audit as au
    return {n for rec in au.scan(abc).values() for n, _ in rec['ei'] if n != '<dynamic>'}


def check(src, main_raw=None):
    import dos2_layout as lay, tempfile
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    made = build(src)
    main = None
    for p in [main_raw] + [d / 'work' / 'exefs-update' / 'main.raw' for d in Path(__file__).resolve().parents]:   # dos2-port/work (also from a worktree)
        if p and os.path.exists(str(p)): main = open(str(p), 'rb').read(); break
    for o, out in made.items():
        data = src[SRC[o]]
        print('%s -> %s: %d B -> %d B, sha256 %s' % (SRC[o], o, len(data), len(out), hashlib.sha256(out).hexdigest()[:16]))
        b = Swf(out); (tb, pb, Bc), = b.abcs(); (_, _, A0), = Swf(data).abcs()
        errs = verify_all(Bc)
        for who, why in errs: print('       %s: %s' % (who, why))
        say(not errs, 'ABC verifies: %d method bodies' % len(Bc.bodies))
        say(Swf(Swf(out).write()).body() == b.body(), 'the movie parses and round-trips')
        want = ['header / tail differ'] if o in ('trade_p.swf', 'containerInventory_p.swf', 'reward_p.swf', 'itemSplitter_p.swf') else []
        g = gr.check(data, out, quiet=True); say(g == want, 'every original tag kept, ids unique, uses after definitions%s' % ('' if g == want else ': ' + str(g)))
        e0, e1 = ei_names(A0), ei_names(Bc)
        say(e0 <= e1 and e1 - e0 <= EI_ALLOWED_NEW, 'engine contract: every PC ExternalInterface name kept (%d), new: %s' % (len(e0), ' '.join(sorted(e1 - e0)) or 'none'))
        if main is not None:
            dead = sorted(n for n in e1 if n.encode() not in main)
            say(not dead or set(dead) <= {n for n in e0 if n.encode() not in main}, 'every new ExternalInterface name is in main.raw (PC dead names: %s)' % (' '.join(dead) or 'none'))
        own = {v: k for k, v in Bc.owners().items()}
        def dis(cls, m): return '\n'.join(Bc.dis(Bc.methods[own[cls + '::' + m]].body)) if cls + '::' + m in own else ''
        if o == 'trade_p.swf':
            t = dis('trade_fla.trade_1', 'frame1')
            say('dmF1Orig' in t and all(("'%s'" % n) in t or n in t for n in ('cellWidth', 'invCols', 'offerCols', 'setFrame', 'SB_SPACING', 'dmSlices', 'EL_SPACING', 'rotation')),
                'trade_1.frame1 = PC frame1 + grid numbers (cell %d, pitch %d, %d x %d / %d x %d) + frames + scroll bars + tab rows + layout' % (TR_CELL, TR_PITCH, TR_INV_COLS, TR_INV_ROWS, TR_OFF_COLS, TR_OFF_ROWS))
            # the frame slices tile 0..TR_W in the header and in the body (no gap, no overlap)
            def tiles(body):
                sp = sorted((nx, nx + (x1 - x0) * float(k)) for x0, x1, y0, y1, nx, k in TR_SLICES if (y0 == 0 and y1 == TR_FRAME_H) or (y1 != TR_HDR) == body)
                return all(abs(a[1] - b_[0]) < 0.01 for a, b_ in zip(sp, sp[1:])) and sp[0][0] == 0 and abs(sp[-1][1] - TR_W) < 0.01
            say(tiles(False) and tiles(True), 'frame: %d slices tile 0..%d in the header (y 0..%d) and the body: side strips cut, panes x%.3f / x%.3f, centre x%.3f'
                % (len(TR_SLICES), TR_W, TR_HDR, float(TR_K_L), float(TR_K_R), float(TR_K_C)))
            # grids inside their panes, the inventories end above the bottom row, the offers above the OK plate
            cells = {n: (float(x) + 3.5, float(y) + 3.5) for n, (x, y) in TR_GRID.items()}
            pane = {'playerInventory_mc': (10, 360), 'traderInventory_mc': (800, 1149), 'playerExchange_mc': (402, 758), 'traderExchange_mc': (402, 758)}
            okg = all(pane[n][0] <= cx and cx + (TR_INV_W if 'Inventory' in n else TR_OFF_W) - TR_SPACING <= pane[n][1] for n, (cx, cy) in cells.items())
            inv_end = float(TR_GRID['playerInventory_mc'][1]) + TR_INV_H; off_end = float(TR_GRID['playerExchange_mc'][1]) + TR_OFF_H
            say(okg and inv_end <= 935 and off_end <= 935 and cells['traderExchange_mc'][0] - cells['playerExchange_mc'][0] == TR_OFF_W,
                'grids: inventories at x %.1f / %.1f (%d drawn; iPad 25 / 796, 343), offers at %.1f / %.1f (%d; iPad 407 / 584, 170), all in their panes; '
                'inventories to y %.0f (%d full rows of %d, iPad 7.5), offers to %.0f, the button row from 935'
                % (cells['playerInventory_mc'][0], cells['traderInventory_mc'][0], TR_INV_W - TR_SPACING, cells['playerExchange_mc'][0], cells['traderExchange_mc'][0],
                   TR_OFF_W - TR_SPACING, inv_end, int((TR_INV_H - 3.5) // TR_PITCH), TR_PITCH, off_end))
            say(360 <= TR_BAR['playerInventoryList'] and TR_BAR['playerExchangeList'] + 15 <= 412 and 745 <= TR_BAR['traderExchangeList'] <= 758 - 9
                and 758 <= TR_BAR['traderInventoryList'] and TR_BAR['traderInventoryList'] + 15 <= 800,
                'scroll bars in the pillars / the centre edge as on the PC: x %s' % ' / '.join('%d' % TR_BAR[n] for n in TR_LISTS))
            tabs_end = float(TR_TABS['playerTabs_mc'][0]) + 44 + TR_TAB_SP + 6 * (41 + TR_TAB_SP) + 41   # the first tab 44 high (heightOverride)
            say(tabs_end <= 360 + 0.5 and float(TR_TABS['traderTabs_mc'][0]) + tabs_end - 15 <= 1149 + 0.5 and TR_TABS['playerTabs_mc'][1] + 41 <= cells['playerInventory_mc'][1],
                'filter tabs: a row of up to 8 above each inventory (y %d..%d; iPad filter row 219..268), the player\'s to x %.0f' % (TR_TABS['playerTabs_mc'][1], TR_TABS['playerTabs_mc'][1] + 41, tabs_end))
            t = dis('trade_fla.trade_1', 'addTab') + dis('trade_fla.trade_1', 'addTraderTab')
            say(t.count('rotation') == 2 and 'dmTabOrig' in t and 'dmTTabOrig' in t, 'addTab / addTraderTab: original + the new tab turned +90 (holders -90: upright, left to right)')
            mvd = gr.Movie(Swf(data)); kids = _children(Swf(data), mvd, mvd.resolve('trade_fla.trade_1'))
            pil = kids[TR_PILLAR[0]][1] if len(kids) > TR_PILLAR[0] else None
            say(pil is not None and kids[TR_PILLAR[0]][0] is None and pil['depth'] == TR_PILLAR[2] and Swf(data).tags[mvd.defs[pil['char']]][0] in gr.T_SHAPES
                and kids[1][0] == 'frame_mc' and kids[0][0] is None, 'trade_1 children: #0 = the unnamed shape under frame_mc (#1), #%d = the unnamed balance pillar shape (depth %d) -> x %+d' % (TR_PILLAR[0], TR_PILLAR[2], TR_PILLAR[1]))
            say('getChildIndex' in dis('trade_fla.trade_1', 'dmSlices') and 'istypelate' in dis('trade_fla.trade_1', 'frame1'), 'slices go right under frame_mc; the pillar is moved only if child #%d is a Shape at x 0' % TR_PILLAR[0])
            t = dis('trade_fla.trade_1', 'dmSlices'); say(t.count('scrollRect') == len(TR_SLICES) and t.count('addChildAt') == len(TR_COPIES), 'dmSlices: frame_mc + %d copies cut by scrollRect' % len(TR_COPIES))
            say('getAt' in dis('trade_fla.trade_1', 'extendGrid') and 'dmExtOrig' in dis('trade_fla.trade_1', 'extendGrid'), 'extendGrid: original + Cell symbols scaled %d / %d' % (TR_CELL, TR_CELL_PC))
            t = dis('trade_fla.trade_1', 'onContainerUp'); say('dmUpOrig' in t and 'downList' in t and 'getTimer' in t, 'onContainerUp: a first tap selects (no itemClick), a second tap on that slot within %d ms = the PC itemClick (moves across)' % DTAP_MS)
            t = dis('trade_fla.trade_1', 'init')
            say("'setMcSize'" in t and t.count('setLength') == 4, 'init: original + setMcSize(%d, %d) + scroll bar lengths %d / %d' % (TR_W, TR_H, TR_INV_SB, TR_OFF_SB))
            t = dis('trade_fla.MainTimeline', 'onEventInit'); say("'center'" in t and 'dmEvInitOrig' in t, 'onEventInit: original + setAnchor center / screen / center')
            names = {gr.place_parse(c, tt)['name'] for c, tt, _ in b.tags if c in (26, 70)}
            say(all((n.encode() + b'\0') in names for n in TR_COPIES), '%d frame copies grafted onto the root (the PC frame symbol)' % len(TR_COPIES))
            say(b.head[:-4] == rect_bytes(TR_W, TR_H), 'stage %d x %d (PC 1360 x 1121, iPad 1160 x 1030)' % (TR_W, TR_H))
            # every path the layout touches exists in the PC movie; every moved element inside the window
            with tempfile.TemporaryDirectory() as td:
                f = os.path.join(td, 'trade.swf'); open(f, 'wb').write(data)
                ent = {e['npath']: e for e in lay.walk(lay.Mov.load(f), 4)}
            paths = {'trade_mc.' + '.'.join(op[1]) for op in trade_ops()}
            miss = sorted(p for p in paths if p not in ent)
            say(not miss, 'all %d instance paths of the layout are in trade.swf%s' % (len(paths), (': missing ' + ' '.join(miss)) if miss else ''))
            out_of = []
            for p, (x, y) in TR_POS.items():
                e = ent.get('trade_mc.' + '.'.join(p))
                if not e or not e['local'] or p == ('traderTooltip_mc',): continue
                k = float(TR_SCALE.get(p, e['sx'])); l = e['local']
                bx = (float(x) + l[0] * k, float(y) + l[1] * k, float(x) + l[2] * k, float(y) + l[3] * k)
                if bx[0] < -0.5 or bx[1] < -0.5 or bx[2] > TR_W + 0.5 or bx[3] > TR_H + 0.5: out_of.append('%s %s' % ('.'.join(p), [round(v) for v in bx]))
            say(not out_of, 'every placed element inside the %d x %d window%s' % (TR_W, TR_H, (': ' + '; '.join(out_of)) if out_of else ''))
            jb = ent['trade_mc.offerJunk_mc']['local']; rb = ent['trade_mc.repair_mc']['local']; ob = ent['trade_mc.ok_mc']['local']
            say(True, 'bottom row: junk %.0f x %.0f (iPad 206 x 62; the PC icon x%.1f), OK %.0f x %.0f (iPad 206 x 62), repair / identify %.0f (iPad 64), close x%.1f (iPad 50)'
                % ((jb[2] - jb[0]) * float(TR_SCALE[('offerJunk_mc',)]), (jb[3] - jb[1]) * float(TR_SCALE[('offerJunk_mc',)]), float(TR_SCALE[('offerJunk_mc',)]),
                   ob[2] - ob[0], ob[3] - ob[1], (rb[2] - rb[0]) * float(TR_SCALE[("repair_mc",)]), float(TR_SCALE[("closeX_mc",)])))
        elif o in ('containerInventory_p.swf', 'reward_p.swf', 'itemSplitter_p.swf'):
            cls, ops, stage, what = {'containerInventory_p.swf': ('containerInventory_fla.MainTimeline', container_ops(), CI_STAGE, 'inv_mc x%.3f at %d,%d' % (float(CI_K), CI_X, CI_Y)),
                                     'reward_p.swf': ('reward_fla.MainTimeline', reward_ops(), RW_STAGE, 'reward_mc x%.3f, accept x%.1f' % (float(RW_K), float(RW_ACC_K))),
                                     'itemSplitter_p.swf': ('itemSplitter_fla.MainTimeline', splitter_ops(), IS_STAGE, 'splitter_mc x%.2f, buttons x%.2f' % (float(IS_K), float(IS_BTN_K)))}[o]
            t = dis(cls, 'dmLay'); ti = dis(cls, 'onEventInit')
            say('dmLay' in ti and 'dmOrig_onEventInit' in ti and 'scaleX' in t, 'onEventInit = original + dmLay: %s' % what)
            say(b.head[:-4] == rect_bytes(*stage), 'stage %d x %d' % stage)
            with tempfile.TemporaryDirectory() as td:
                f = os.path.join(td, SRC[o]); open(f, 'wb').write(data)
                ent = {e['npath']: e for e in lay.walk(lay.Mov.load(f), 3)}
            paths = {'.'.join(op[1]) for op in ops}; miss = sorted(p for p in paths if p not in ent)
            say(not miss, 'all instance paths of the layout are in %s%s' % (SRC[o], (': missing ' + ' '.join(miss)) if miss else ''))
            top = ops[0][1][0]; k = float(ops[0][2]); x, y = float(ops[1][2]), float(ops[1][3])
            box = ent[top]['local'] if ent.get(top) else None
            if o == 'containerInventory_p.swf':
                t = dis('containerInventory_fla.inventory_1', 'onContainerUp'); say('dmUpOrig' in t and 'playerInventory' in t and 'getTimer' in t, 'onContainerUp: a first tap selects, a second tap on that slot within %d ms = the PC take (doubleClickItem)' % DTAP_MS)
                say('setPosition' in ti and 'dmLay' in dis(cls, 'setAnchor'), 'setPosition centre once; setAnchor (PC: moves inv_mc) = original + dmLay')
                lb = (184, 310, 631, 756); sb = (x + lb[0] * k, y + lb[1] * k, x + lb[2] * k, y + lb[3] * k)
                say(0 <= sb[0] and sb[2] <= stage[0] and 0 <= sb[1] and sb[3] <= stage[1] and abs((sb[0] + sb[2]) / 2 - stage[0] / 2) < 2,
                    'window %.0f,%.0f..%.0f,%.0f (%.0f x %.0f) centred on the %d x %d stage; cells %.0f, take all / close %.0f x %.0f' % (sb + (sb[2] - sb[0], sb[3] - sb[1]) + stage + (55 * k, 100 * k, 48 * k)))
            else:
                ex = (480, 588) if o == 'reward_p.swf' else (484, 350)
                say(ex[0] * k <= stage[0] + 1 and ex[1] * k <= stage[1] + 1, '%s drawn %.0f x %.0f in the stage' % (top, ex[0] * k, ex[1] * k))
                if o == 'reward_p.swf':
                    ax, ay = RW_ACC_XY; kk = float(RW_ACC_K)
                    say(abs(ax + RW_ACC[2] * kk / 2 - (RW_ACC[0] + RW_ACC[2] / 2)) < 0.5, 'cells %.0f (iPad 76), accept %.0f x %.0f (iPad 291 x 62)' % (56 * k, RW_ACC[2] * kk * k, RW_ACC[3] * kk * k))
                else:
                    say(IS_BTN_XY[0] + (61 - 150) * float(IS_BTN_K) >= 0 and IS_BTN_XY[0] + (421 - 150) * float(IS_BTN_K) <= 484, 'OK / cancel %.0f x %.0f each (iPad confirm 291 x 62), group inside the window' % (153 * k * float(IS_BTN_K), 32 * k * float(IS_BTN_K)))
        elif o == 'dialog_p.swf':
            mvd = gr.Movie(Swf(data)); kids = _children(Swf(data), mvd, mvd.resolve('dialog_fla.Dialog_1'))
            art = kids[DL_ART][1] if len(kids) > DL_ART else None
            with tempfile.TemporaryDirectory() as td:
                f = os.path.join(td, 'dialog.swf'); open(f, 'wb').write(data)
                walk = lay.walk(lay.Mov.load(f), 3)
            ent = {e['npath'] for e in walk}
            ae = [e for e in walk if art is not None and e['path'] == 'dialog_mc.[d%d]' % art['depth']]
            say(art is not None and kids[DL_ART][0] is None and Swf(data).tags[mvd.defs[art['char']]][0] in gr.T_SHAPES and len(ae) == 1
                and ae[0]['x'] == 0 and ae[0]['y'] == 0 and ae[0]['local'][2] - ae[0]['local'][0] > 1000,
                'Dialog_1 child #%d = the unnamed window art shape at 0, 0 (%s)' % (DL_ART, ae[0]['local'] if ae else None))
            sl = [(nx, nx + (x1 - x0) * float(k)) for x0, x1, nx, k in DL_SLICES]
            say(all(abs(a[1] - b_[0]) < 0.01 for a, b_ in zip(sl, sl[1:])) and abs((sl[0][0] + 10 + sl[-1][1] - 10) / 2 - 960) < 0.5 and abs((sl[-1][1] - 10) - (sl[0][0] + 10) - 1160) < 0.5,
                'art: 3 slices (strips 1:1, text part x%.3f) = a window %.0f..%.0f (1160 wide, centred on the 1920 stage, bottom 1080; iPad 1160 x 400; PC 1352)'
                % (float(DL_K), sl[0][0] + 10, sl[-1][1] - 10))
            t = dis('dialog_fla.Dialog_1', 'dmSlices')
            say(t.count('scrollRect') == 3 and t.count('addChildAt') == 2 and 'istypelate' in t and 'dmArt' in t, 'dmSlices: the art (a Shape > 1000 px at #%d) cut to the text part, two grafted copies = the strips' % DL_ART)
            names = {gr.place_parse(c, tt)['name'] for c, tt, _ in b.tags if c in (26, 70)}
            say(all((n.encode() + b'\0') in names for n in DL_COPIES), 'two art copies grafted onto the root')
            t = dis('dialog_fla.Dialog_1', 'frame1')
            say('dmF1Orig' in t and 'setFrameWidth' in t and 'KeywordTextWidth' in t and all(("'%s'" % p[0]) in t or p[0] in t for p in DL_POS),
                'frame1: original + trade / inventory / stop listening on the left strip (y 698 / 852 / 1005), text list at x %d, %d wide (PC 375, 1200), answers wrap at %d (PC 975)'
                % (DL_TEXT_X, DL_TEXT_W, DL_KW_W))
            say(DL_TEXT_X + DL_TEXT_W == 1575 - DL_SHIFT and DL_TEXT_X >= 456 and float(DL_HIT[0]) + 1314 * float(DL_HIT[1]) <= 1540,
                'text frame ends at the scroll slot (%d, PC 1575); hit_mc %s..%.0f inside the window' % (DL_TEXT_X + DL_TEXT_W, DL_HIT[0], float(DL_HIT[0]) + 1314 * float(DL_HIT[1])))
            t = dis('dialog_fla.Dialog_1', 'addText_Internal'); say('dmTextOrig' in t and 'setTextHolderHeight' in t, 'addText_Internal: original + the new text re-wrapped at %d (PC 1000)' % DL_TC_W)
            for m in ('addKeywordAnswer', 'addAnswers'):
                t = dis('dialog_fla.Dialog_1', m); say(('dmKwOrig' in t or 'dmAddOrig' in t) and 'drawRect' in t and 'getChildAt' in t,
                                                       '%s: original + a %d px wide row (+%d above / below) on the new answer' % (m, ANSWER_W, ANSWER_PAD))
            say(ANSWER_X + ANSWER_W <= DL_TEXT_W - 18 - 150 + 4, 'answer rows end at %d of the text list (its scroll bar at %d)' % (150 + ANSWER_X + ANSWER_W, DL_TEXT_W - 18))
            t = dis('dialog_fla.Dialog_1', 'executeSelection'); say('dmExOrig' in t and 'dmScUpT' in t, 'executeSelection dropped %d ms after a scroll' % DRAG_GUARD)
            t = dis('dialog_fla.Dialog_1', 'dmScMove'); say('scrollTo' in t and 'mouseChildren' in t, 'one-finger scroll: past %d px the text list follows the finger (scrollTo), its rows get no mouse' % DRAG_START)
            t = dis('dialog_fla.MainTimeline', 'onEventInit'); say("'mouseDown'" in t and 'dmSlices' in t, 'onEventInit: original + art slices + the press listener on dialog_mc')
            miss = sorted(p for p in ('dialog_mc.' + '.'.join(op[1]) for op in dialog_ops()) if p not in ent)
            say(not miss, 'all instance paths of the dialog layout are in dialog.swf%s' % ((': missing ' + ' '.join(miss)) if miss else ''))
            # the engine adds answers through updateDialog -> addKeywordAnswer (addKWAnswerArray), not addAnswers
            if main is not None: say(b'addKWAnswerArray' in main and b'updateDialog' in main, 'main.raw fills addKWAnswerArray and calls updateDialog (the answers come through addKeywordAnswer)')
        elif o == 'waypoints_p.swf':
            t = dis('Waypoint', 'onUp'); say('dmUpOrig' in t and 'dmTap' in t and 'dmScUpT' in t, 'Waypoint.onUp: nothing right after a scroll, else original (select) + dmTap(id)')
            say('useWP' in dis('waypoints_fla.WaypointsList_1', 'dmTap'), 'dmTap: the same waypoint twice within %d ms = useWP (travel)' % DTAP_MS)
            t = dis('waypoints_fla.WaypointsList_1', 'frame1'); say('dmF1Orig' in t and 'dmScDown' in t, 'WaypointsList_1.frame1: original + one-finger scroll of listCont')
            say('scrollTo' in dis('waypoints_fla.WaypointsList_1', 'dmScMove'), 'one-finger scroll: past %d px the list follows the finger' % DRAG_START)
            say('drawRect' in dis('waypoints_fla.MainTimeline', 'onEventInit'), 'the shared PANEL_CLOSE hit area is in (onEventInit)')
    # the host table: one row per UI type
    c = Path(__file__).resolve().parent.parent / 'dm' / 'host' / 'hle_dos2_touchui.c'
    if c.exists():
        import re
        txt = c.read_text(encoding='utf-8'); m = re.search(r'static const tu_def k_touch\[\] = \{(.*?)\n\};', txt, re.S)
        rows = {(int(i, 16), mv_) for i, mv_ in re.findall(r'\{(0x[0-9a-f]+), 0, "(\w+)", "GUI/\w+\.swf"\}', m.group(1))} if m else set()
        want = {(i, n) for n, ids in TYPES.items() for i in ids}
        say(want <= rows, 'k_touch maps %s' % ', '.join('0x%x %s' % r for r in sorted(want)))
    return bad


# ---------------------------------------------------------------- render: the computed layout next to the iPad movie
def _children(swf, mv, cid):
    """[(name, rec)] of a sprite's frame 1 in depth order"""
    code, d, _ = swf.tags[mv.defs[cid]]
    dl = gr.frame_state(d, 1)
    return [((dl[k]['name'][:-1].decode() if dl[k]['name'] else None), dl[k]) for k in sorted(dl)]


def _mat(rec):
    mm = gr.Bits(rec['matrix'], 0).matrix() if rec['matrix'] else [1.0, 0.0, 0.0, 1.0, 0, 0]
    return [mm[0], mm[1], mm[2], mm[3], mm[4] / 20.0, mm[5] / 20.0]


def _mul(p, q):
    return (p[0] * q[0] + p[2] * q[1], p[1] * q[0] + p[3] * q[1], p[0] * q[2] + p[2] * q[3], p[1] * q[2] + p[3] * q[3], p[0] * q[4] + p[2] * q[5] + p[4], p[1] * q[4] + p[3] * q[5] + p[5])


def _apply(m, ops, path):
    """a child's local matrix after the layout ops that name its path (op semantics of B.apply)"""
    m = list(m)
    for op in ops:
        if tuple(op[1]) != tuple(path): continue
        if op[0] == 'pos': m[4], m[5] = float(op[2]), float(op[3])
        elif op[0] == 'x': m[4] = float(op[2])
        elif op[0] == 'move': m[4] += float(op[2]); m[5] += float(op[3])
        elif op[0] in ('scale', 'scalex'):
            k = float(op[2]); sx = (m[0] ** 2 + m[1] ** 2) ** 0.5 or 1; sy = (m[2] ** 2 + m[3] ** 2) ** 0.5 or 1
            ky = k if op[0] == 'scale' else sy
            m[0], m[1], m[2], m[3] = m[0] / sx * k, m[1] / sx * k, m[2] / sy * ky, m[3] / sy * ky
    return m


def scene(swf, cid, M, canvas, ops, path=(), skip=(), expand=(), ctx=None):
    """draw sprite cid with matrix M, its children moved / scaled by ops; children whose path is in `expand` are drawn child by child"""
    import dos2_swf_render as rn
    mv = ctx.mv
    for name, rec in _children(swf, mv, cid):
        if rec['char'] is None or rec['clip'] is not None: continue
        p = path + (name or '#%d' % rec['depth'],)   # an unnamed child is '#<depth>'
        if p in skip: continue
        m = _apply(_mat(rec), ops, p)
        wd = [op[2] for op in ops if op[0] == 'width' and tuple(op[1]) == p]   # a text field made narrower: drawn as its box x the ratio
        if wd and not (name and p in expand):
            bx = gr.bounds(swf, rec['char'])
            if bx and bx[2] > bx[0]: k = float(wd[-1]) / ((bx[2] - bx[0]) * ((m[0] ** 2 + m[1] ** 2) ** 0.5 or 1)); m[0] *= k; m[1] *= k
        MM = _mul(M, m)
        if name and p in expand: scene(swf, rec['char'], MM, canvas, ops, p, skip, expand, ctx)
        else: rn.draw(ctx, rec['char'], MM, canvas, rn._cx(rec), 1, 1)


def _cells(canvas, M, x0, y0, cols, rows, cell, pitch, inset=3, col=(0.11, 0.09, 0.08, 1.0), edge=(0.42, 0.36, 0.28, 1.0)):
    """the engine's slot frames (drawn by the engine into the iggy_* placeholders; here dark squares at the grid numbers)"""
    import numpy as np
    H, W = canvas.shape[:2]
    for r in range(rows):
        for c in range(cols):
            ax, ay = M[0] * (x0 + c * pitch) + M[4], M[3] * (y0 + r * pitch) + M[5]
            bx, by = ax + M[0] * cell, ay + M[3] * cell
            X0, Y0, X1, Y1 = int(round(ax)), int(round(ay)), int(round(bx)), int(round(by))
            if X1 <= 0 or Y1 <= 0 or X0 >= W or Y0 >= H: continue
            canvas[max(0, Y0):min(H, Y1), max(0, X0):min(W, X1)] = edge
            i = max(1, int(round(inset * M[0])))
            canvas[max(0, Y0 + i):min(H, Y1 - i), max(0, X0 + i):min(W, X1 - i)] = col


def render_one(name, data, ipad_dir, out_dir, stage_only=False):
    """stage_only: -> the movie's stage as computed (PIL RGBA, transparent where nothing is drawn) instead of the picture with the iPad's"""
    import numpy as np
    from PIL import Image, ImageDraw
    import dos2_swf_render as rn
    swf = Swf(data)
    def blank(w, h): return np.zeros((h, w, 4), np.float32)
    def bg(canvas):
        out = np.empty_like(canvas); out[...] = (0.16, 0.17, 0.16, 1.0); rn._over(out, canvas); return out
    ctx = rn.Ctx(swf)
    if name == 'trade':
        W, H = TR_W, TR_H
        mv = ctx.mv; tcid = mv.resolve('trade_fla.trade_1'); fr = [r for n, r in _children(swf, mv, tcid) if n == 'frame_mc'][0]
        full = blank(1360, TR_FRAME_H); rn.draw(ctx, fr['char'], (1, 0, 0, 1, 0, 0), full, None, 1, 1)
        im = Image.fromarray((np.clip(full, 0, 1) * 255).astype(np.uint8), 'RGBA')
        canvas_im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        for x0, x1, y0, y1, nx, k in TR_SLICES:   # the frame slices (scrollRect + x + scaleX of dmSlices)
            piece = im.crop((x0, y0, x1, y1)).resize((int(round((x1 - x0) * float(k))), y1 - y0), Image.LANCZOS); canvas_im.alpha_composite(piece, (int(round(nx)), y0))
        cv = np.asarray(canvas_im).astype(np.float32) / 255.0; cv = np.ascontiguousarray(cv)
        cv[..., :3] *= cv[..., 3:4]
        ops = trade_ops()
        # the grids first (the engine draws them inside the placeholders; the inventories show 7 full rows), then the movie's own elements
        for cname, (x, y) in TR_GRID.items():
            inv = 'Inventory' in cname
            _cells(cv, (1, 0, 0, 1, 0, 0), float(x) + 3.5, float(y) + 3.5, TR_INV_COLS if inv else TR_OFF_COLS, 7 if inv else TR_OFF_ROWS, TR_CELL, TR_PITCH)
        # the filter tabs the engine adds (addTab): 8 Tab symbols in each row (the holder -90, every tab +90 = upright, left to right)
        tab = mv.resolve('Tab')
        for hx, hy in TR_TABS.values():
            x = float(hx)
            for i in range(8):
                rn.draw(ctx, tab, (1, 0, 0, 1, x, float(hy)), cv, None, 1, 1); x += (44 if i == 0 else 41) + TR_TAB_SP
        skip = {('frame_mc',), ('#1',), ('playerInventory_mc',), ('playerExchange_mc',), ('traderExchange_mc',), ('traderInventory_mc',), ('tweenItem_mc',),
                ('traderTooltip_mc',)}
        ops = ops + [('move', ('#%d' % TR_PILLAR[2],), TR_PILLAR[1], 0)]
        scene(swf, tcid, (1, 0, 0, 1, 0, 0), cv, ops, (), skip, {('namesHolder_mc',)}, ctx)
        mine = cv
    elif name in ('containerInventory', 'reward', 'itemSplitter'):
        W, H = {'containerInventory': CI_STAGE, 'reward': RW_STAGE, 'itemSplitter': IS_STAGE}[name]
        ops = {'containerInventory': container_ops(), 'reward': reward_ops(), 'itemSplitter': splitter_ops()}[name]
        cv = blank(W, H); mv = ctx.mv
        top = ops[0][1][0]
        root = [(n, r) for n, r in ((rec['name'][:-1].decode() if rec['name'] else None, rec) for rec in
                (gr.place_parse(c, t) for c, t, _ in swf.tags if c in (26, 70))) if n]
        for n, rec in root:
            if n != top: continue
            M = _apply(_mat(rec), ops, (top,))
            if name == 'containerInventory':   # the engine's grid: 5 x 4 cells of 55, pitch 57, at container_mc 256,358 (+1 bgDiscrap)
                _cells(cv, M, 256 + 1, 358 + 1, 5, 4, 55, 57, 2)
            elif name == 'reward':             # two grids of 6 x 2 cells 56 / 58 at optionalReward_mc 65,126 and mandatoryReward_mc 65,300 (+4)
                for gy in (126, 300): _cells(cv, M, 65, gy + 4, 6, 2, 56, 58, 2)
            scene(swf, rec['char'], M, cv, ops, (top,), set(), set(), ctx)
        mine = cv
    elif name == 'dialog':
        # the window (the stage's 380..1540 x 680..1080, where the iPad's 1160 x 400 stage is): the art in its three slices, the left strip's
        # buttons and hit_mc where frame1 puts them, then a sample of the run-time content (made by code: a text block, three answer rows)
        mv = ctx.mv; dcid = mv.resolve('dialog_fla.Dialog_1'); kids = _children(swf, mv, dcid); art = kids[DL_ART][1]
        full = blank(1920, 1200); rn.draw(ctx, art['char'], (1, 0, 0, 1, 0, 0), full, None, 1, 1)
        im = Image.fromarray((np.clip(full, 0, 1) * 255).astype(np.uint8), 'RGBA')
        canvas_im = Image.new('RGBA', (1920, 1080), (0, 0, 0, 0))
        for x0, x1, nx, k in DL_SLICES:
            piece = im.crop((x0, DL_Y0, x1, DL_Y0 + DL_H)).resize((int(round((x1 - x0) * float(k))), DL_H), Image.LANCZOS); canvas_im.alpha_composite(piece, (int(round(nx)), DL_Y0))
        cv = np.ascontiguousarray(np.asarray(canvas_im).astype(np.float32) / 255.0); cv[..., :3] *= cv[..., 3:4]
        skip = {('#1',), ('hit_mc',), ('#%d' % art['depth'],), ('playerOptionsHolder',), ('textHolder_mc',)}
        scene(swf, dcid, (1, 0, 0, 1, 0, 0), cv, dialog_ops(), (), skip, set(), ctx)
        tx, ty = DL_TEXT_X, 706 + 10
        tc = blank(1920, 1080); rn.draw(ctx, mv.resolve('TextComp'), (1, 0, 0, 1, tx, ty), tc, None, 1, 1)   # a text block: the speaker's portrait ...
        tc[:, tx + 98:] = 0; rn._over(cv, tc)
        cv[ty:ty + 62, tx + 102:tx + 102 + DL_TC_W] = cv[ty:ty + 62, tx + 102:tx + 102 + DL_TC_W] * 0.4 + np.array([0.18, 0.17, 0.2, 0.6], np.float32)   # ... its text at DL_TC_W
        kw = mv.resolve('AnswerKeyword')
        for i in range(3):   # the answer rows: AnswerKeyword at x 150 of the text list, the transparent row rect (outlined here)
            ay = ty + 100 + i * (30 + 2 * ANSWER_PAD - 2) + ANSWER_PAD; ax = tx + 150
            x0, x1, y0, y1 = ax + ANSWER_X, ax + ANSWER_X + ANSWER_W, ay - ANSWER_PAD, ay + 30 + ANSWER_PAD
            cv[y0:y1, x0:x1] = cv[y0:y1, x0:x1] * 0.7 + np.array([0.12, 0.05, 0.02, 0.3], np.float32)
            cv[y0, x0:x1] = cv[y1 - 1, x0:x1] = (0.8, 0.7, 0.4, 1); cv[y0:y1, x0] = cv[y0:y1, x1 - 1] = (0.8, 0.7, 0.4, 1)
            rn.draw(ctx, kw, (1, 0, 0, 1, ax, ay), cv, None, 1, 1)
        # the text list's frame (the scroll area: a drag anywhere in it scrolls) and its scroll bar place
        x0, x1, y0, y1 = tx, tx + DL_TEXT_W, 706, 1060
        cv[y0, x0:x1] = cv[y1, x0:x1] = (0.4, 0.8, 0.9, 1); cv[y0:y1, x0] = cv[y0:y1, x1] = (0.4, 0.8, 0.9, 1)
        cv[y0 + 20:y1 - 20, x1 - 18:x1 - 3] = (0.35, 0.25, 0.2, 1)
        mine = np.ascontiguousarray(cv[680:1080, 380:1540])
    else:   # waypoints: the PC layout, the 50 px close area marked
        rn_img, org = rn.render(swf, 'root', 1.0); mine = np.asarray(rn_img).astype(np.float32) / 255.0; mine = np.ascontiguousarray(mine); mine[..., :3] *= mine[..., 3:4]
        cx, cy = (1270 + 1293) / 2.0, (125 + 148) / 2.0
        mine[int(cy - 25), int(cx - 25):int(cx + 25)] = (0.9, 0.8, 0.3, 1); mine[int(cy + 25), int(cx - 25):int(cx + 25)] = (0.9, 0.8, 0.3, 1)
        mine[int(cy - 25):int(cy + 25), int(cx - 25)] = (0.9, 0.8, 0.3, 1); mine[int(cy - 25):int(cy + 25), int(cx + 25)] = (0.9, 0.8, 0.3, 1)
    if stage_only: return rn._img(mine)
    a = rn._img(bg(mine))
    ip = None
    tpath = os.path.join(ipad_dir, name + '_t.swf') if ipad_dir else None
    if tpath and os.path.exists(tpath):
        ti, _ = rn.render(Swf(open(tpath, 'rb').read()), 'root', 1.0); ip = rn._img(bg(np.ascontiguousarray((np.asarray(ti).astype(np.float32) / 255.0) * [1, 1, 1, 1])))
    gap = 24; Wt = a.size[0] + (ip.size[0] + gap if ip else 0); Ht = max(a.size[1], ip.size[1] if ip else 0) + 40
    sheet = Image.new('RGBA', (Wt, Ht), (20, 20, 20, 255)); sheet.alpha_composite(a, (0, 40))
    if ip: sheet.alpha_composite(ip, (a.size[0] + gap, 40))
    dr = ImageDraw.Draw(sheet)
    dr.text((8, 8), '%s_p (computed layout, %dx%d stage px)' % (name, a.size[0], a.size[1]), fill=(240, 230, 200, 255))
    if ip: dr.text((a.size[0] + gap + 8, 8), '%s_t (iPad, study only, %dx%d)' % (name, ip.size[0], ip.size[1]), fill=(240, 230, 200, 255))
    os.makedirs(out_dir, exist_ok=True)
    p = os.path.join(out_dir, 'ppanel_%s.png' % name); sheet.convert('RGB').save(p); print('%s: %dx%d' % (p, Wt, Ht))
    return p


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == 'check': sys.exit(1 if check(sources(a[1])) else 0)
    if len(a) == 3 and a[0] == 'build':
        out = Path(a[2]); out.mkdir(parents=True, exist_ok=True)
        for n, d in build(sources(a[1])).items(): (out / n).write_bytes(d); print('%s: %d B' % (out / n, len(d)))
        return
    if len(a) == 4 and a[0] == 'render':
        src = sources(a[1])
        for o, s in SRC.items(): render_one(o[:-6], src[s], a[2], a[3])
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    import dos2_ppanel_trade as _me   # the module object tools/dos2_pchud_touch.py installs its hook into (this file runs as __main__)
    import dos2_pchud_touch           # noqa: F401  (install(): waypoints_p = the shared close-button movie + the double tap)
    _me.main()
