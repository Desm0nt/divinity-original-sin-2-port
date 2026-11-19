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
  trade_p          the grid variables set to trade_t's numbers right after the PC frame script (trade_1.frame1 wrapped): cells 82 (icon
                   76), pitch 86, 4 x 9 inventories, 2 x 8 offers; the PC window kept, its centre column widened by 32 px for the wider offer
                   grids (the PC frame drawn as three slices: frame_mc cut to the left part by scrollRect, two grafted copies of the same
                   frame symbol cut to the centre (x 1.095) and right parts), the right column moved +32; anchored centre / screen / centre
                   (PC: top), stage 1392 x 1056 (PC 1360 x 1121, iPad 1160 x 1030: the PC side strips - filter tabs, repair / identify - are
                   kept); the PC Cell symbols scaled 82 / 55; offer wares and clear-offer buttons x1.4, finger hit areas (close 56, offer
                   wares / repair / identify / balance 64, clear 50). Mouse code is the PC's: tap = itemClick, double tap = itemDoubleClick
                   (moves the item across, as the iPad's DoubleTap), long press (host right click) = itemRightClick, still finger (host
                   hover) = tooltip, drag = startDragging / stopDragging between the four grids.
  containerInventory_p   inv_mc scaled 76 / 55 (cells 76 on screen; iPad: cells 76, its art x1.2), centred on the stage (stage 1400 x 1080,
                   setPosition center / screen / center like the iPad's anchor consts; PC: no anchor in mouse mode). Take all / close
                   become 138 x 66 (iPad 118 x 56). Double tap = doubleClickItem (take), drag = the PC's.
  reward_p         reward_mc scaled 76 / 56 (cells 76, window 651 x 798; iPad 620 x 915, cells 76), accept button x1.4 more (296 x 61; iPad
                   291 x 62). Tap = itemClicked (the PC's pick by click).
  itemSplitter_p   splitter_mc scaled 1.32 (iPad 638 x 478 / PC 484 x 350), OK / cancel x1.25 more (252 x 53 each).
  dialog_p         the PC window (1352 x 398 at the bottom; iPad 1160 x 400) with every answer a full-width tappable row: a transparent
                   ANSWER_W x (text + 2 x ANSWER_PAD) rect in the answer (tap anywhere on the row = that answer: the PC's ROLL_OVER select +
                   CLICK executeSelection), one-finger drag on the window = scroll the text (scrollTo on the text list's scrollbar); a tap
                   that ended a drag runs no answer (executeSelection ignored for DRAG_GUARD ms after a scroll step).
  waypoints_p      + double tap on a waypoint = travel (the iPad's DoubleTap; PC: tap selects, the travel button travels).

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
# trade (PC trade.swf: stage 1360 x 1121, anchored top; frame_mc 0..1360 x 0..1056; columns: left 0..512, centre 512..848, right 848..1360)
TR_CELL, TR_BORDER, TR_SPACING, TR_DISCRAP = 82, 3, 4, Fraction(-7, 2)     # trade_t: CELL_SIZE 76 + 2 x CELL_BORDER 3, CELL_BORDER_SPACING 4, -3.5
TR_PITCH = TR_CELL + TR_SPACING                                             # 86
TR_INV_COLS, TR_INV_ROWS, TR_OFF_COLS, TR_OFF_ROWS = 4, 9, 2, 8              # trade_t 4 / 8 / 2 / 7; PC 6 / 13 / 2 / 12 (rows fill the PC's 770 / 700 high panes)
TR_INV_W, TR_OFF_W = TR_INV_COLS * TR_PITCH, TR_OFF_COLS * TR_PITCH          # 344 (PC 6 x 57 = 342: the same pane) / 172 (PC 114)
TR_INV_H, TR_OFF_H = 770, TR_OFF_ROWS * TR_PITCH + 1                         # PC setLength(770) / 8 rows
TR_CUT = (512, 848)                                                          # PC frame x of the centre column
TR_DW = 32                                                                   # the centre column 336 -> 368 px (two offer grids 2 x 172 + 2 + borders)
TR_CS = Fraction(TR_CUT[1] - TR_CUT[0] + TR_DW, TR_CUT[1] - TR_CUT[0])      # 368 / 336
TR_FRAME_H = 1056
TR_W, TR_H = 1360 + TR_DW, TR_FRAME_H                                        # stage / setMcSize
TR_OFF_X = (523, 697)                                                        # playerExchange_mc / traderExchange_mc (PC 522 / 682 + list offset 20)
# scroll bar x = SB holder x + SB_SPACING + grid width (LS_Classes.scrollGrid.setFrame): kept where the PC had them (inventory bars in the
# pillars, offer bars on the centre column's borders)
TR_SB = {'playerInventoryList': 471 - 78 - TR_INV_W, 'traderInventoryList': 874 + TR_DW - (891 + TR_DW) - TR_INV_W,
         'playerExchangeList': 506 - TR_OFF_X[0] - TR_OFF_W, 'traderExchangeList': 870 - TR_OFF_X[1] - TR_OFF_W}
TR_LISTS = ['playerInventoryList', 'playerExchangeList', 'traderExchangeList', 'traderInventoryList']
TR_RIGHT = ['traderImg_mc', 'traderInventory_mc', 'traderInventorySB_mc', 'traderTabs_mc', 'traderGold_mc', 'vendorCombo_mc', 'vendorSortLabel_txt',
            'mcRepContainer', 'repair_mc', 'identify_mc', 'balloon_mc', 'traderTooltip_mc', 'closeX_mc']
TR_RIGHT_NAMES = ['title_txt', 'label_txt']                                  # inside namesHolder_mc (the trader's name / title)


def _centre_shift(x0, x1):
    """px a centre-column element moves when the column is stretched by TR_CS from TR_CUT[0]"""
    c = (x0 + x1) / 2.0
    return int(round(TR_CUT[0] + (c - TR_CUT[0]) * float(TR_CS) - c))


TR_SHIFT = {'balance_mc': _centre_shift(652, 709), 'ok_mc': _centre_shift(526, 834), 'playerExchangeGold_mc': _centre_shift(559, 643),
            'traderExchangeGold_mc': _centre_shift(718, 802)}
TR_PILLAR = (4, _centre_shift(652, 709), 9)   # child index, x shift, timeline depth of the unnamed shape behind balance_mc (652..709 x 16..185)
# (name, PC box x0, y0, x1, y1 at its PC place, scale): enlarged about its centre (plus the column shift)
TR_GROW = [('offerJunk_mc', (657, 126, 705, 158), Fraction(7, 5)), ('clearSupplyBtn_mc', (587, 158, 612, 183), Fraction(7, 5)),
           ('clearDemandBtn_mc', (747, 158, 772, 183), Fraction(7, 5))]
TR_PAD = [('closeX_mc', 56), ('offerJunk_mc', 64), ('clearSupplyBtn_mc', 50), ('clearDemandBtn_mc', 50), ('repair_mc', 64), ('identify_mc', 64),
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

# dialog
ANSWER_W, ANSWER_X, ANSWER_PAD = 900, -10, 12                                # the row rect in the answer's px (questionWidth 850 + margins)
DRAG_START, DRAG_GUARD = 14, 250                                             # px before a press becomes a scroll; ms an answer is ignored after one

# waypoints
DTAP_MS = 450

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
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 64, 64, s.scope, s.scope + 1)
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
            elif op[0] == 'width': s.setp(o, 'width', num(op[2]))
            elif op[0] == 'pad': s.pad(o, op[2])
            else: raise ValueError(op)
            s.close(k)
    def pad(s, o, size):
        """a transparent size x size (object px) square centred on what the object draws: a finger-sized hit area"""
        b, g = s.loc(), s.loc(); k = s.lab()
        s.emit(('getlocal', o), ('getlocal', o), ('callproperty', s.P('getBounds'), 1), ('coerce_a',), ('setlocal', b),
               ('getlocal', o), ('getproperty', s.P('graphics')), ('coerce_a',), ('setlocal', g), ('getlocal', g), ('iffalse', k),
               ('getlocal', g), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', s.P('beginFill'), 2), ('getlocal', g))
        for pos, sz in (('x', 'width'), ('y', 'height')):
            s.emit(('getlocal', b), ('getproperty', s.P(pos)), ('getlocal', b), ('getproperty', s.P(sz)), ('pushbyte', 2), ('divide',), ('add',),
                   *num(Fraction(size) / 2), ('subtract',))
        s.emit(*num(size), *num(size), ('callpropvoid', s.P('drawRect'), 4), ('getlocal', g), ('callpropvoid', s.P('endFill'), 0), ('L', k))


def ops_pad(pads, scale_of=lambda n: 1):
    return [('pad', (n,), Fraction(size) / scale_of(n)) for n, size in pads]


def rect_bytes(w, h):
    vals = [0, w * 20, 0, h * 20]; nb = max(v.bit_length() for v in vals) + 1
    bits = format(nb, '05b') + ''.join(format(v, '0%db' % nb) for v in vals); bits += '0' * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def set_stage(swf, w, h): swf.head = rect_bytes(w, h) + swf.head[-4:]


# ---------------------------------------------------------------- trade
def trade_ops():
    """the layout ops of trade_1 (paths from trade_mc); also used by the render"""
    ops = [('x', (n,), TR_OFF_X[k]) for k, n in enumerate(('playerExchange_mc', 'traderExchange_mc'))]
    ops += [('x', (n,), TR_OFF_X[k]) for k, n in enumerate(('playerExchangeSB_mc', 'traderExchangeSB_mc'))]
    ops += [('move', (n,), TR_DW, 0) for n in TR_RIGHT] + [('move', ('namesHolder_mc', n), TR_DW, 0) for n in TR_RIGHT_NAMES]
    ops += [('move', (n,), d, 0) for n, d in TR_SHIFT.items()]
    for n, (x0, y0, x1, y1), k in TR_GROW:
        cx = (x0 + x1) / 2.0 + _centre_shift(x0, x1); cy = (y0 + y1) / 2.0
        ops += [('scale', (n,), k), ('pos', (n,), Fraction(cx - (x1 - x0) * float(k) / 2.0).limit_denominator(10), Fraction(cy - (y1 - y0) * float(k) / 2.0).limit_denominator(10))]
    # the engine's draw placeholders (iggy_*: a red bg_mc) as wide as the new grids (their height follows the rows: updateRows)
    for holder, w in (('playerInventory_mc', TR_INV_W), ('playerExchange_mc', TR_OFF_W), ('traderExchange_mc', TR_OFF_W), ('traderInventory_mc', TR_INV_W)):
        ops.append(('width', (holder, 'iggy_' + holder[:-3], 'bg_mc'), w - 1))
    grow = {n: k for n, _, k in TR_GROW}
    ops += ops_pad(TR_PAD, lambda n: grow.get(n, 1))
    return ops


def trade(data):
    """trade.swf -> trade_p.swf"""
    swf = Swf(data)
    # the frame in three slices: two plain sprites holding the PC frame symbol, placed on the root; trade_1 moves them under its frame_mc
    g = gr.Graft(swf); recs = gr.frame_state(swf.tags[gr.Movie(swf).defs[gr.Movie(swf).resolve('trade_fla.trade_1')]][1], 1)
    fid = [r['char'] for r in recs.values() if r['name'] == b'frame_mc\0'][0]
    for nm in ('dmFrameC', 'dmFrameR'): g.place(g.sprite([(fid, 1, (0, 0), None)]), 0, 0, nm)
    g.finish(); set_stage(swf, TR_W, TR_H)
    mv = Movie(swf.write(compress=False)); t = Cls(mv, 'trade_fla.trade_1'); root = Cls(mv, 'trade_fla.MainTimeline')
    Rect = mv.abc.pub('Rectangle', 'flash.geom')
    def rect(b, x, y, w, h): return [('findpropstrict', Rect)] + num(x) + num(y) + num(w) + num(h) + [('constructprop', Rect, 4)]
    # dmSlices(): the frame cut into left (frame_mc itself) / centre (stretched) / right (moved) parts; idempotent
    b = B(t, 0)
    o, k = b.obj(('frame_mc',)); b.setp(o, 'scrollRect', rect(b, 0, 0, TR_CUT[0], TR_FRAME_H)); b.close(k)
    for nm, x0, x1, at, sx in (('dmFrameC', TR_CUT[0], TR_CUT[1], TR_CUT[0], TR_CS), ('dmFrameR', TR_CUT[1], 1360, TR_CUT[1] + TR_DW, 1)):
        o = b.loc(); k = b.lab(); par = b.loc()
        # right under frame_mc (above the unnamed depth-1 shape that frame_mc hides in the PC movie)
        b.emit(('getlocal0',), ('getproperty', b.P('parent')), ('coerce_a',), ('setlocal', par), ('getlocal', par), ('iffalse', k),
               ('getlocal', par), ('pushstring', b.S(nm)), ('callproperty', b.P('getChildByName'), 1), ('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', k),
               ('getlocal0',), ('getproperty', b.P('frame_mc')), ('iffalse', k),
               ('getlocal0',), ('getlocal', o), ('getlocal0',), ('getlocal0',), ('getproperty', b.P('frame_mc')), ('callproperty', b.P('getChildIndex'), 1),
               ('callpropvoid', b.P('addChildAt'), 2))
        b.setp(o, 'scrollRect', rect(b, x0, 0, x1 - x0, TR_FRAME_H)); b.setp(o, 'x', num(at)); b.setp(o, 'scaleX', num(sx))
        b.setp(o, 'mouseEnabled', [('pushfalse',)]); b.close(k)
    t.method('dmSlices', 0, b.code())
    # frame1: the original (lists made at the PC numbers), then the iPad's grid numbers, frames, scroll bars, the layout
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
            if not inv: b.setp(o, 'x', num(0))
            b.close(k)
        b.apply(trade_ops())
        # the unnamed balance pillar shape (depth 9 = child index TR_PILLAR[0] while the timeline's children are all there is) moves with the centre column
        o = b.loc(); k = b.lab(); Shape = mv.abc.pub('Shape', 'flash.display')
        b.emit(('getlocal0',), ('getproperty', b.P('numChildren')), *num(TR_PILLAR[0] + 1), ('lessthan',), ('iftrue', k),
               ('getlocal0',), *num(TR_PILLAR[0]), ('callproperty', b.P('getChildAt'), 1), ('coerce_a',), ('setlocal', o),
               ('getlocal', o), ('getlex', Shape), ('istypelate',), ('iffalse', k), ('getlocal', o), ('getproperty', b.P('x')), ('pushbyte', 0), ('ifne', k))
        b.setp(o, 'x', num(TR_PILLAR[1])); b.emit(('L', k))
        b.emit(('getlocal0',), ('callpropvoid', b.P('dmSlices'), 0))
    t.wrap('frame1', 'dmF1Orig', f1)
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
    # init(): the original (it tells the engine setMcSize(frame_mc) = the left slice now), then the whole window
    t.wrap('init', 'dmInitOrig', lambda b: (b.orig(), b.ei('setMcSize', num(TR_W), num(TR_H))))
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


# ---------------------------------------------------------------- dialog
def dialog(data):
    mv = Movie(data); d = Cls(mv, 'dialog_fla.Dialog_1'); root = Cls(mv, 'dialog_fla.MainTimeline')
    getTimer = mv.abc.pub('getTimer', 'flash.utils')
    def now(): return [('findpropstrict', getTimer), ('callproperty', getTimer, 0)]
    # addAnswers(text): the original, then the answer it added (the last child of the answer container) gets a full-width row rect
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
    d.wrap('addAnswers', 'dmAddOrig', add)
    # executeSelection(id): ignored for DRAG_GUARD ms after a scroll step (the release that ends a drag is a CLICK on an answer too)
    def ex(b):
        k = b.lab()
        b.emit(*now(), ('getlocal0',), ('getproperty', b.P('dmDragT')), ('subtract',), *num(DRAG_GUARD), ('lessthan',), ('iftrue', k))
        b.orig(); b.emit(('L', k))
    d.wrap('executeSelection', 'dmExOrig', ex)
    # one-finger scroll: dmDown(e) on the window, dmMove(e) / dmUp(e) on the stage
    b = B(d, 1); st = b.loc(); k = b.lab()
    b.emit(('getlocal0',), ('getproperty', b.P('stage')), ('coerce_a',), ('setlocal', st), ('getlocal', st), ('iffalse', k))
    b.set_this('dmY0', [('getlocal', st), ('getproperty', b.P('mouseY'))]); b.set_this('dmMoved', [('pushfalse',)])
    b.set_this('dmS0', b.path(('textContainer', 'm_scrollbar_mc', 'scrolledY')))
    for ev, h in (('mouseMove', 'dmMove'), ('mouseUp', 'dmUp')):
        b.emit(('getlocal', st), ('pushstring', b.S(ev)), ('getlocal0',), ('getproperty', b.P(h)), ('callpropvoid', b.P('addEventListener'), 2))
    b.emit(('L', k)); d.method('dmDown', 1, b.code())
    b = B(d, 1); st, dy = b.loc(), b.loc(); k, go = b.lab(), b.lab()
    b.emit(('getlocal0',), ('getproperty', b.P('stage')), ('coerce_a',), ('setlocal', st), ('getlocal', st), ('iffalse', k),
           ('getlocal', st), ('getproperty', b.P('mouseY')), ('getlocal0',), ('getproperty', b.P('dmY0')), ('subtract',), ('setlocal', dy),
           ('getlocal0',), ('getproperty', b.P('dmMoved')), ('iftrue', go),
           ('getlocal', dy), *num(DRAG_START), ('greaterthan',), ('getlocal', dy), *num(-DRAG_START), ('lessthan',), ('bitor',),
           ('iffalse', k))
    b.set_this('dmMoved', [('pushtrue',)])
    b.emit(('L', go))
    o, k2 = b.obj(('textContainer', 'm_scrollbar_mc'))
    b.callv(o, 'scrollTo', [('getlocal0',), ('getproperty', b.P('dmS0')), ('getlocal', dy), ('subtract',)], [('pushfalse',)], [('pushfalse',)])
    b.close(k2)
    b.set_this('dmDragT', now())
    b.emit(('L', k)); d.method('dmMove', 1, b.code())
    b = B(d, 1); st = b.loc(); k = b.lab()
    b.emit(('getlocal0',), ('getproperty', b.P('stage')), ('coerce_a',), ('setlocal', st), ('getlocal', st), ('iffalse', k))
    for ev, h in (('mouseMove', 'dmMove'), ('mouseUp', 'dmUp')):
        b.emit(('getlocal', st), ('pushstring', b.S(ev)), ('getlocal0',), ('getproperty', b.P(h)), ('callpropvoid', b.P('removeEventListener'), 2))
    b.emit(('L', k)); d.method('dmUp', 1, b.code())
    # onEventInit(): the original, then the press listener on the dialog window (once)
    def ev(b):
        b.orig()
        o, k = b.obj(('dialog_mc',))
        b.emit(('getlocal', o), ('pushstring', b.S('mouseDown')), ('getlocal', o), ('getproperty', b.P('dmDown')), ('callpropvoid', b.P('addEventListener'), 2))
        b.setp(o, 'dmDragT', num(-10000)); b.close(k)
    root.wrap('onEventInit', 'dmEvInitOrig', ev)
    return mv.done()


# ---------------------------------------------------------------- waypoints: the shared PANEL_CLOSE movie + double tap = travel
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
    def up(b):
        b.orig()
        o, k = b.obj(('base',)); b.callv(o, 'dmTap', b.path(('id',))); b.close(k)
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
            say('dmF1Orig' in t and all(("'%s'" % n) in t or n in t for n in ('cellWidth', 'invCols', 'offerCols', 'setFrame', 'SB_SPACING', 'dmSlices')),
                'trade_1.frame1 = PC frame1 + grid numbers (cell %d, pitch %d, %d x %d / %d x %d) + frames + scroll bars + layout' % (TR_CELL, TR_PITCH, TR_INV_COLS, TR_INV_ROWS, TR_OFF_COLS, TR_OFF_ROWS))
            say(TR_INV_W <= 392 - 2 * 24 + 1 and 3.5 + TR_INV_W - TR_SPACING <= TR_INV_W and TR_INV_ROWS * TR_PITCH <= TR_INV_H + TR_PITCH // 2,
                'inventory grid %d px wide in the PC pane of 392 (PC 6 x 57 = 342), %d rows of %d in %d' % (TR_INV_W, TR_INV_ROWS, TR_PITCH, TR_INV_H))
            say(TR_OFF_X[0] >= TR_CUT[0] + 10 and TR_OFF_X[1] == TR_OFF_X[0] + TR_OFF_W + 2 and TR_OFF_X[1] + TR_OFF_W <= TR_CUT[1] + TR_DW - 10,
                'offer grids at x %d / %d (2 x %d) inside the widened centre column %d..%d' % (TR_OFF_X[0], TR_OFF_X[1], TR_OFF_W, TR_CUT[0], TR_CUT[1] + TR_DW))
            say(TR_OFF_ROWS * TR_PITCH + 1 <= 700, 'offer panes %d px of the PC 700' % (TR_OFF_ROWS * TR_PITCH + 1))
            mvd = gr.Movie(Swf(data)); kids = _children(Swf(data), mvd, mvd.resolve('trade_fla.trade_1'))
            pil = kids[TR_PILLAR[0]][1] if len(kids) > TR_PILLAR[0] else None
            say(pil is not None and kids[TR_PILLAR[0]][0] is None and pil['depth'] == TR_PILLAR[2] and Swf(data).tags[mvd.defs[pil['char']]][0] in gr.T_SHAPES
                and kids[1][0] == 'frame_mc' and kids[0][0] is None, 'trade_1 children: #0 = the unnamed shape under frame_mc (#1), #%d = the unnamed balance pillar shape (depth %d) -> x +%d' % (TR_PILLAR[0], TR_PILLAR[2], TR_PILLAR[1]))
            say('getChildIndex' in dis('trade_fla.trade_1', 'dmSlices') and 'istypelate' in dis('trade_fla.trade_1', 'frame1'), 'slices go right under frame_mc; the pillar is moved only if child #%d is a Shape at x 0' % TR_PILLAR[0])
            t = dis('trade_fla.trade_1', 'dmSlices'); say(t.count('scrollRect') == 3 and 'addChildAt' in t, 'frame slices: left 0..%d, centre %d..%d x%.3f, right +%d' % (TR_CUT[0], TR_CUT[0], TR_CUT[1], float(TR_CS), TR_DW))
            say('getAt' in dis('trade_fla.trade_1', 'extendGrid') and 'dmExtOrig' in dis('trade_fla.trade_1', 'extendGrid'), 'extendGrid: original + Cell symbols scaled %d / %d' % (TR_CELL, TR_CELL_PC))
            t = dis('trade_fla.trade_1', 'onContainerUp'); say('dmUpOrig' in t and 'downList' in t and 'getTimer' in t, 'onContainerUp: a first tap selects (no itemClick), a second tap on that slot within %d ms = the PC itemClick (moves across)' % DTAP_MS)
            say("'setMcSize'" in dis('trade_fla.trade_1', 'init'), 'init: original + setMcSize(%d, %d)' % (TR_W, TR_H))
            t = dis('trade_fla.MainTimeline', 'onEventInit'); say("'center'" in t and 'dmEvInitOrig' in t, 'onEventInit: original + setAnchor center / screen / center')
            names = {gr.place_parse(c, tt)['name'] for c, tt, _ in b.tags if c in (26, 70)}
            say(all((n.encode() + b'\0') in names for n in ('dmFrameC', 'dmFrameR')), 'two frame slices grafted onto the root (the PC frame symbol)')
            say(b.head[:-4] == rect_bytes(TR_W, TR_H), 'stage %d x %d (PC 1360 x 1121, iPad 1160 x 1030)' % (TR_W, TR_H))
            # every path the layout touches exists in the PC movie
            with tempfile.TemporaryDirectory() as td:
                f = os.path.join(td, 'trade.swf'); open(f, 'wb').write(data)
                have = {e['npath'] for e in lay.walk(lay.Mov.load(f), 4)}
            paths = {'trade_mc.' + '.'.join(op[1]) for op in trade_ops()}
            miss = sorted(p for p in paths if p not in have)
            say(not miss, 'all %d instance paths of the layout are in trade.swf%s' % (len(paths), (': missing ' + ' '.join(miss)) if miss else ''))
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
            t = dis('dialog_fla.Dialog_1', 'addAnswers'); say('dmAddOrig' in t and 'drawRect' in t and 'getChildAt' in t, 'addAnswers: original + a %d px wide row (+%d above / below) on the new answer' % (ANSWER_W, ANSWER_PAD))
            t = dis('dialog_fla.Dialog_1', 'executeSelection'); say('dmExOrig' in t and 'getTimer' in t, 'executeSelection ignored %d ms after a scroll step' % DRAG_GUARD)
            t = dis('dialog_fla.Dialog_1', 'dmMove'); say('scrollTo' in t and 'dmDragT' in t, 'one-finger scroll: past %d px the text list follows the finger (scrollTo)' % DRAG_START)
            say("'mouseDown'" in dis('dialog_fla.MainTimeline', 'onEventInit'), 'onEventInit: original + the press listener on dialog_mc')
        elif o == 'waypoints_p.swf':
            say('dmUpOrig' in dis('Waypoint', 'onUp') and 'dmTap' in dis('Waypoint', 'onUp'), 'Waypoint.onUp: original (select) + dmTap(id)')
            say('useWP' in dis('waypoints_fla.WaypointsList_1', 'dmTap'), 'dmTap: the same waypoint twice within %d ms = useWP (travel)' % DTAP_MS)
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
        elif op[0] == 'scale':
            k = float(op[2]); sx = (m[0] ** 2 + m[1] ** 2) ** 0.5 or 1; sy = (m[2] ** 2 + m[3] ** 2) ** 0.5 or 1
            m[0], m[1], m[2], m[3] = m[0] / sx * k, m[1] / sx * k, m[2] / sy * k, m[3] / sy * k
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
        W, H = TR_W, TR_H; cv = blank(W, H)
        mv = ctx.mv; tcid = mv.resolve('trade_fla.trade_1'); fr = [r for n, r in _children(swf, mv, tcid) if n == 'frame_mc'][0]
        full = blank(1360, TR_FRAME_H); rn.draw(ctx, fr['char'], (1, 0, 0, 1, 0, 0), full, None, 1, 1)
        im = Image.fromarray((np.clip(full, 0, 1) * 255).astype(np.uint8), 'RGBA')
        parts = [(0, TR_CUT[0], 0, 1.0), (TR_CUT[0], TR_CUT[1], TR_CUT[0], float(TR_CS)), (TR_CUT[1], 1360, TR_CUT[1] + TR_DW, 1.0)]
        canvas_im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        for x0, x1, at, sx in parts:
            piece = im.crop((x0, 0, x1, TR_FRAME_H)).resize((int(round((x1 - x0) * sx)), TR_FRAME_H), Image.LANCZOS); canvas_im.alpha_composite(piece, (at, 0))
        cv = np.asarray(canvas_im).astype(np.float32) / 255.0; cv = np.ascontiguousarray(cv)
        cv[..., :3] *= cv[..., 3:4]
        ops = trade_ops(); ops = [('pos', ('playerExchange_mc',), TR_OFF_X[0], 234), ('pos', ('traderExchange_mc',), TR_OFF_X[1], 234)] + ops
        # the grids first (the engine draws them inside the placeholders), then the movie's own elements over them
        for cname, cols, rows, lx, ly, x in (('playerInventory_mc', TR_INV_COLS, TR_INV_ROWS, 25, 15, 78), ('playerExchange_mc', TR_OFF_COLS, TR_OFF_ROWS, 0, 7, TR_OFF_X[0]),
                                             ('traderExchange_mc', TR_OFF_COLS, TR_OFF_ROWS, 0, 7, TR_OFF_X[1]), ('traderInventory_mc', TR_INV_COLS, TR_INV_ROWS, 25, 15, 891 + TR_DW)):
            vis = TR_INV_H if cols == TR_INV_COLS else TR_OFF_H
            _cells(cv, (1, 0, 0, 1, 0, 0), x + lx + 3.5, 234 + ly + 3.5, cols, min(rows, int(vis // TR_PITCH)), TR_CELL, TR_PITCH)
        skip = {('frame_mc',), ('#1',), ('playerInventory_mc',), ('playerExchange_mc',), ('traderExchange_mc',), ('traderInventory_mc',), ('tweenItem_mc',)}
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
        W, H = 1920, 1080; cv = blank(W, H)
        rn_img, org = rn.render(swf, 'root', 1.0); mine = np.asarray(rn_img).astype(np.float32) / 255.0; mine = np.ascontiguousarray(mine); mine[..., :3] *= mine[..., 3:4]
        # three sample answer rows (the rect each answer gets) where the answers end up at run time: the answer holder is moved into the text
        # list (addAnswerHolder: textHolder_mc 375,706, answers at x 90 in it); the y is only an example (the list grows from the bottom)
        for k in range(3):
            y = 900 + k * (26 + 2 * ANSWER_PAD); x = 375 + 90 + ANSWER_X
            mine[y - ANSWER_PAD:y + 26 + ANSWER_PAD, x:x + ANSWER_W] *= 0.6
            mine[y - ANSWER_PAD:y + 26 + ANSWER_PAD, x:x + ANSWER_W, 0] += 0.15
            mine[y - ANSWER_PAD, x:x + ANSWER_W] = (0.8, 0.7, 0.4, 1)
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
