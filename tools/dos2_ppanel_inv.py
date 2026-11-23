"""[DOS2] UI mode TOUCH, the character sheet and the inventory with the iPad's STRUCTURE, drawn with our art: the player's own PC movies (Switch
Game.pak partyInventory.swf, characterSheet.swf) transformed into partyInventory_p.swf / characterSheet_p.swf (docs/dos2/TOUCH_MODES_PLAN.md
"Implemented: лист персонажа и инвентарь — структура iPad нашей графикой"; the iPad's numbers: docs/dos2/TOUCH_LAYOUT.md section 5). No iPad file
is read (the iPad movies were only measured); the backgrounds are the player's own bitmaps re-cut into pieces (new DefineShapes with bitmap
fills), the one new drawing is the window button from our own assets/touchui. Static only: nothing here was played or run on a phone.

The geometry is NOT fixed numbers: every window is laid out by dmLay() from document variables - dmCell (the cell px the host wants on the
screen; 76 = the iPad's), dmState (0 / 1, below), dmW (the screen width in stage px at height 1080, from onEventResolution), dmB (the hot bar's
top). The layout is written once, as data (Spec / Ex below): this tool evaluates it (the placements baked into the movie = dmLay at cell 76 on
a 16:9 screen, the checks, the pictures) and compiles the same statements to AVM2 (dmLay at run time). The host sets the numbers with
dmSetCell(px), or through an invoke name the engine class registers: setAnchor(1000000 + px) for the inventory, setHelmetOptionState(1000000 +
px) for the sheet; 1000000 + 0 / 1 = the state. dmLay ends with setMcSize(w, 1080) + setPosition(topleft) (phone 23.11: 'center' cut the window).

ONE grid for both windows (win_lets: the same statements in both movies, checked equal for every cell / screen): the window WW x WH centred
above the hot bar; a title bar across; row A (IH); the doll column (LX, PY, LW x PH) | the right column (RX, PY, RW x PH); margin GM, gutter GG.
The frame = the PC sheet background re-cut (frame_groups / frame_spec): the spiked top / bottom strips repeat their 135 px period (n copies,
each x 0.9..1.1: no ornament stretched), rails and plain edges stretch, the parchment frame is 9 pieces, the black body fills only the gutters
and row A (bg_mc is drawn OVER the panels: their rects stay open).

  characterSheet_p   state 0 = THE SHEET (iPad characterSheet_t): title bar (name - level centred = charInfo_mc, close on the right tile, the
                     sheet -> inventory button on the left tile), row A = the HP / AP / Source box (the PC's infoStatList, type 0 of
                     addSecondaryStat, moved out of equip_mc) | the 5 tabs x F + the tab title on the wood plaque; the doll column (frame +
                     equip_mc x D) | ONE parchment panel (x S: a stat row 26 -> 44 px) - the attributes tab in two columns, the tab lists
                     flowed into two columns every frame (dmFlow), one-finger scroll on the lists (dmScDown).
                     state 1 = THE INVENTORY WINDOW's frame and left part: the same title bar (its button goes back to the sheet), the same HP
                     box and doll, and the stat strip in row A's right part (dmStripTick: the PC's secondaryStatList / resistanceStatList as
                     value + icon cells, two rows); the right column under the strip is left OPEN for partyInventory_p.
  partyInventory_p   the inventory's part of that window (any state; the host opens it with the sheet in state 1 - dm/host hu_inv_code): the
                     filter row (the PC header's ornate bar; party tabs, All, party view | sort, sort by, gold, x SB) and the bag (the PC list x
                     SQ = cell / 50 x INV_K, 11 columns - the engine reads the grid from the movie once: inventory_mc.invColW / cSpacing,
                     main+0x13aa5b0 - centred in the column, scrolling). The two movies' drawings never overlap: their order is the engine's.
                     State 0 (alone, a fallback only): a black ground under the whole window with the PC title and close.

Everything else is the PC movie's own mouse code driven by the gesture layer (tap, double tap = double click = use / equip, long press =
right click = context menu, still finger = hover = tooltip, drag = drag - onto a doll slot too -, two fingers = wheel = list scroll).

  python -I tools/dos2_ppanel_inv.py build  <Switch Game.pak | dir of the PC movies> <out dir>
  python -I tools/dos2_ppanel_inv.py check  <Switch Game.pak | dir of the PC movies>
  python -I tools/dos2_ppanel_inv.py render <Switch Game.pak | dir of the PC movies> <out dir> [--ipad <dir of *_t.swf, to look at only>]
        the screen with each view (inv / sheet / equip) at cells 76 96 110 130 on 16:9 and 20:9, and the movies next to the PC / iPad ones
Called from tools/dos2_pchud_touch.py (MAKE / check), so `dos2_touchui_pak.py touch` packs these two instead of the plain PC copies.
"""
import hashlib, os, re, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, NS_PUBLIC
import dos2_swf_graft as gr

STAGE_H = 1080
STAGE_WMAX = 2560     # the movies' stage width (SWF header): wider than any screen in stage px - the movie spans the screen, the window is placed in it
# ---- partyInventory (PC: inventory_mc at 0,20 on a 682 x 1152 stage; the window = bgFrame_mc 680 x 1056 at the clip's origin). Own px of the clip:
INV_W0, INV_H0 = 680, 1056
INV_COLS, INV_PITCH = 11, 56     # 11 columns of cell 50 + spacing 6 (PC 11 x 58; iPad 8 x 86 in a narrower bag): the engine reads the grid from
                                 # the movie once (inventory_mc.invColW / cSpacing / cSize, main+0x13aa5b0); 11 fill the bag column at the phone's cell
INV_ADD = INV_COLS * INV_PITCH - 11 * 58   # the list is this much wider than the PC's (-22: narrower)
INV_LIST_Y, INV_LIST_W0, INV_LIST_H0 = 202, 650, 827   # listHolder_mc.y, partyInventory_1.invW / invH
INV_LIST_W = INV_LIST_W0 + INV_ADD
INV_BAG_HEAD = 56                # a bag's header (playerInventory: name, gold, weight)
INV_K = 0.88                     # owner 23.11: the bag drawn x 0.88 of the cell the host asks (rows still decided at the full cell)
INV_CLOSE = 50                   # screen px of the close button (state 0 only: the window's own close is the sheet's)
INV_BTN = 'dmDoll_mc'            # the sheet's window button (characterSheet_p; the inventory has none: the title bar is the sheet's)
INV_BAG = (600, 710, 40, 40)     # a flat near-black patch of the PC inventory background: the bag's ground
INV_BAR = (8, 172, 30, [(48, 1, 8), (262, 0, 56), (270, 0, 362), (40, 1, 632)])   # the PC header's ornate bar (x y h, pieces: w, kept 1 /
                                 # stretched 0, source x): the filter row's ground; the two end ornaments keep their shape, the two plain spans
                                 # take the rest (the centre ornament is left out: an item stood on it)
INV_TABS_W = 184                 # own px kept for the party tabs (4 x 46: a party of four; the PC header kept ~133 before its All button)
INV_HEAD_ITEMS = 980             # own px of the filter row's items in a line: the party tabs, All, the party view check, sort, sort by, gold
TOP_M = 8                        # screen px kept free above the window
COL_RES = 40                     # px kept free at each side beyond MARGIN (the right column of the touch HUD: 92 x 1.2 + a gap)
BTN_ART = 148.0                  # px of our plate drawing (set from the asset when it is embedded)
# ---- characterSheet (PC: stats_mc at 0,20 on a 680 x 1152 stage; the window = bg_mc 680 x 1056, one bitmap with holes for the two halves). Own px of the clip:
CS_W, CS_H0 = 680, 1056
CS_S0 = (17, 10)                 # 1.70 at the iPad's cell: 680 -> 1156 px wide (iPad 1160), a stat row 26 -> 44 px (iPad 46)
CS_PB0, CS_PB_MIN = 735, 360     # the list panel's height: PC; the least that shows attributes + resistances / secondary stats + experience
CS_RES_Y = 196                   # resistances in mainStats_mc (PC 510): 10 own under the 6 attributes; secondary stats go to the second column
CS_HIT_H = 1085.0
CS_LISTS = [('combatAbilityHolder_mc', 270, 705), ('civicAbilityHolder_mc', 270, 705), ('talentHolder_mc', 328, 705), ('tagsHolder_mc', 328, 705),
            ('visualHolder_mc', 328, 705)]        # holder, list frame width, scroll bar length (the PC init values)
CS_OFF = ['leftCycleBtn_mc', 'rightCycleBtn_mc', 'onePlayerOverlay_mc', 'charList_mc', 'alignments_mc', 'aiSel_mc', 'dragHit_mc']   # the strip's content and the drag bar
CS_STAY = ['hitArea_mc', 'bg_mc', 'title_txt', 'close_mc']
CS_FAR = -4000                   # own px: where hidden things go (the PC code still finds every instance)
CS_CLOSE = 50                    # screen px of the close button at the iPad's cell
CS_PM_HIT = (-6, -3, 32, 26)     # the + / - buttons' invisible hit sprite in the button's own px (PC: 0,1 18 x 18): x, y, w, h
CS_HELMET = (205, 24, 2.0)       # helmet toggle: x, y in equip_mc, scale (PC: a 21 px badge on the helmet slot at 176,49; iPad: its own 71 px button)

# ---- the WINDOW of the sheet and the inventory (owner 23.11: "the iPad's structure, sizes and arrangement, drawn with OUR art"; not an open
# book). ONE grid for both windows, computed by the same statements in both movies (win_lets): the window WW x WH at WX, WY; a title bar across;
# under it row A (IH high: the sheet's HP / AP / Source box | its tabs + tab title; the inventory's stat strip + its filter row); then the body:
# the doll column (LX, PY, LW x PH: the doll in the SAME place and size in both windows) | the right column (RX, PY, RW x PH: the sheet's one
# parchment panel / the bag). The frame is the PC sheet background (characterSheet.swf bg_mc, 680 x 1056) re-cut: art px below.
SA_TOP_H = 50                    # rows 0..50: the spikes + the title bar
SA_L, SA_R, SA_P = 205, 475, 135 # the top / bottom strips: the left end is x 0..205, the right end 475..680, the repeated period 135 px (big spike
                                 # centre to big spike centre); the period is REPEATED (n copies, each scaled n x 135 -> the exact width, 0.9..1.1):
                                 # no ornament is stretched
SA_TILE = (58, 622, 74)          # the title bar's corner tiles hang below it: left x 0..58, right 622..680, rows 50..74
SA_BOT, SA_BOT_H, SA_BOTP = 1030, 26, 475   # the bottom border rows 1030..1056; its period from x 475 (no column divider in it)
SA_BOT_BAR = 11                  # the bottom border's opaque bar (rows 1030..1041); the spikes under it stand on nothing
SA_RAIL = 8                      # the side rails x 0..8 / 672..680 (plain: stretched)
SA_BODY = (420, 900, 160, 100)   # a plain black patch (under the PC doll): the window body between the panels (gutters, row A, under the panels)
SA_BOX = (8, 202, 331, 46, 6)    # the black framed bar of the PC tab row: x y w h, the edge rows kept at top / bottom (the middle stretches):
                                 # the HP / AP / Source box, the inventory's stat strip and filter row
SA_PLAQUE = (341, 202, 336, 48)  # the wood plaque with notched ends (PC "equipment" title): the tab title
SA_PANEL = (22, 290, 317, 740, 56)   # the parchment panel's ornate frame (x y w h, corner): 9 pieces, the edges plain (stretched), no centre (a hole)
SA_DOLL = (341, 253, 337, 627)   # the doll frame (black, ornament corners, the slots' frames baked in): scaled as one (D)
NPER = 20                        # period copies made (the widest window needs < 20)
STRETCH = 0.999                  # the static scale of a piece dmLay stretches (any scale but 1: pieces_sprite draws it without the bleed pixel)
GM, GG = 12, 12                  # the grid: outer margin inside the rails, gutter between the columns (screen px)
WIN_FMAX = (13, 10)              # the frame art's scale cap (title bar 65 px, iPad ~75)
ROWA = 108                       # row A height in frame art px (x F): the sheet's tabs 56 + 4 + the plaque 48
PADP = 10                        # the panel content's inset inside the parchment frame (frame art px, x F)
PANEL_W = 680                    # the panel content width in the sheet's own px: two stat columns of 340
COL2 = 340                       # own x of the second column
DOLL_W, DOLL_H = SA_DOLL[2], SA_DOLL[3]
INFO_W = 262                     # own px of an InfoStat row (icon at -13 .. the value's right end at 248)
STRIP_H = 62                     # frame art px (x F) of the inventory's stat strip (two rows: secondary stats / resistances); the filter row
                                 # under it gets the rest of row A
def win_lets(a, C, DW, BT, OXK):
    """the shared window grid (both movies, any state): variables F TB BB SR IH CT PY PZ PH D LW LX RX S RW WW WX WY WH"""
    B = a.let('WB', fmax(600, fmin(1080, BT)))
    WH = a.let('WH', B - TOP_M - 2)
    F = a.let('F', fmin(R(*WIN_FMAX), WH / 720))
    a.let('TB', SA_TOP_H * F); BB = a.let('BB', SA_BOT_H * F); SR = a.let('SR', SA_RAIL * F)
    IH = a.let('IH', ROWA * F)
    SH = a.let('SH', STRIP_H * F)                                # the inventory's row A: the stat strip (the sheet draws it, state 1) ...
    a.let('FH', IH - SH - 4)                                     # ... over the filter row (the inventory movie draws it)
    CT = a.let('CT', SA_TILE[2] * F + 6)                         # row A: under the title bar's corner tiles (they hang to art row 74)
    a.let('FT', CT + SH + 4)
    PY = a.let('PY', CT + IH + GG)
    PZ = a.let('PZ', WH - BB - GM)
    PH = a.let('PH', PZ - PY)
    D = a.let('D', PH / DOLL_H)                                  # the doll fills the body's height
    LW = a.let('LW', DOLL_W * D); LX = a.let('LX', SR + GM)
    RX = a.let('RX', LX + LW + GG)
    # the content scale: a stat row 26 own -> 44 px at the iPad's cell (x 1.7), no more than the panel's height holds, no wider than the screen
    S = a.let('S', fmin(fmin(R(CS_S0[0], CS_S0[1]) * C / CELL0, (PH - 2 * PADP * F) / CS_PB_MIN),
                        (DW - 2 * MARGIN - 2 * COL_RES - (RX + GM + SR) - 2 * PADP * F) / PANEL_W))
    RW = a.let('RW', PANEL_W * S + 2 * PADP * F)
    WW = a.let('WW', ffloor(RX + RW + GM + SR + R(999, 1000)))
    a.let('WX', (DW - WW) / 2 - OXK); a.let('WY', TOP_M)
    return F, S, D, WW, WH


# ---------------------------------------------------------------- SWF helpers (static edits of the player's movie)
def rect_bytes(w, h):
    vals = [0, w * 20, 0, h * 20]; nb = max(v.bit_length() for v in vals) + 1
    bits = format(nb, '05b') + ''.join(format(v, '0%db' % nb) for v in vals); bits += '0' * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def _mat(rec):
    m = gr.Bits(rec['matrix'], 0).matrix() if rec['matrix'] else [1.0, 0.0, 0.0, 1.0, 0, 0]
    assert m[1] == 0 and m[2] == 0, 'a rotated placement'
    return m[4] / 20.0, m[5] / 20.0, m[0], m[3]   # x, y, sx, sy


def _set(rec, x=None, y=None, sx=None, sy=None, dx=0.0, dy=0.0):
    ox, oy, osx, osy = _mat(rec)
    rec['matrix'] = gr.matrix_bytes((ox if x is None else x) + dx, (oy if y is None else y) + dy, osx if sx is None else sx, osy if sy is None else sy)


def _def_index(swf, cid):
    for n, (code, d, _) in enumerate(swf.tags):
        if code in gr.DEFS and struct.unpack_from('<H', d, 0)[0] == cid: return n
    raise KeyError(cid)


def _root_names(swf):
    return {p['name'][:-1].decode(): p for p in (gr.place_parse(c, t) for c, t, _ in swf.tags if c in (gr.T_PLACE2, gr.T_PLACE3)) if p.get('name')}


def edit_places(swf, sprite, fn, frame=1):
    """the PlaceObject records of frame `frame` of a sprite (id; None = the root timeline) go through fn(name | None, record) -> None | False
    (False = drop the placement). Records are rebuilt; every other tag stays as it is. Returns the names seen."""
    seen = []
    def one(code, d, f):
        if code not in (gr.T_PLACE2, gr.T_PLACE3) or f != frame: return code, d
        rec = gr.place_parse(code, d)
        if rec['move'] or rec['char'] is None: return code, d
        name = rec['name'][:-1].decode() if rec['name'] else None; seen.append(name)
        if fn(name, rec) is False: return None
        return gr.place_build(rec)
    if sprite is None:
        f = 1; out = []
        for t in swf.tags:
            r = one(t[0], t[1], f)
            if t[0] == gr.T_SHOW: f += 1
            if r is not None: out.append([r[0], r[1], False] if r[1] is not t[1] else t)
        swf.tags[:] = out; return seen
    n = _def_index(swf, sprite); d = swf.tags[n][1]; assert swf.tags[n][0] == gr.T_SPRITE
    o = d[:4]; f = 1
    for code, t in gr.subtags(d):
        r = one(code, t, f)
        if code == gr.T_SHOW: f += 1
        if r is not None: o += gr.tag_bytes(*r)
    swf.tags[n][1] = o; return seen


def bitmap_of(swf, shape, first=False):
    """(bitmap id, w px, h px) of a DefineShape that is one rectangle filled 1:1 with one bitmap (how the PC window backgrounds are made);
    first=True: the shape's FIRST fill is that rectangle and the fills after it (overlays) are dropped by the caller"""
    import dos2_swf_render as rn
    code, d, _ = swf.tags[_def_index(swf, shape)]
    sh = rn.parse_shape(d, gr.T_SHAPES[code]); assert (first or len(sh) == 1) and sh[0][0] == 'fill' and sh[0][1][0] == 'bmp', 'shape %d is not one bitmap fill' % shape
    _, bid, m, clipped, smooth = sh[0][1]
    assert [round(v, 3) for v in m[:4]] == [20.0, 0.0, 0.0, 20.0] and m[4] == 0 and m[5] == 0, 'bitmap of shape %d is not 1:1 at 0,0' % shape
    xs = [p[0] for e in sh[0][2] for p in e]; ys = [p[1] for e in sh[0][2] for p in e]
    assert min(xs) == 0 and min(ys) == 0
    return bid, max(xs) // 20, max(ys) // 20


# ---------------------------------------------------------------- ABC helpers (append-only: methods are added, traits re-pointed; no original body changes)
HEAD = [('getlocal0',), ('pushscope',)]
def num(v):
    v = int(v)
    # pushshort is sign-extended from 16 bits by the player (phone 23.11, shot_inv2: close_mc.x 718.5 compiled as pushshort 718500 / 1000 ran as
    # -2.396, the close button stood at the header's middle): bigger values go through the int pool (Ex.asm)
    assert -32767 <= v <= 32767, 'num(%d): out of pushshort range' % v
    if -128 <= v <= 127: return [('pushbyte', v & 0xff)]
    return [('pushshort', v)] if v >= 0 else [('pushshort', -v), ('negate',)]
def frac(f): return num(f[0]) + num(f[1]) + [('divide',)]


class Cls:
    """one class of the movie: methods are appended, a trait can be re-pointed with the original kept under another name"""
    def __init__(s, swf, abc, tag_pre, name, ref):
        s.swf = swf; s.abc = abc; s.tag, s.pre = tag_pre
        hit = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']) == name or abc.mname(c['name']).endswith('.' + name)]
        assert len(hit) == 1, 'class %s: %d found' % (name, len(hit))
        s.ci = hit[0]; s.inst = abc.instances[s.ci]; s.name = abc.mname(s.inst['name'])
        ti = abc.trait(s.ci, ref); assert ti is not None and ti.kind & 15 == 1, '%s::%s' % (name, ref)
        s.ns = abc.mn[ti.name][1]; assert abc.ns[s.ns] == (NS_PUBLIC, abc.string('')), '%s::%s is not public' % (name, ref)
        s.ref = abc.methods[ti.b].body
        s.EI = abc.pub('ExternalInterface', 'flash.external'); s.made = []
    def P(s, name): return s.abc.qname(s.ns, name)
    def S(s, text): return s.abc.string(text)
    def add(s, name, nparams, lines, like=None):
        abc = s.abc
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 64, 255, s.ref.init_scope, s.ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        if like is not None:
            abc.methods[m].ret = like.ret
            if like.flags & 0x08: abc.methods[m].flags |= 0x08; abc.methods[m].options = list(like.options)
        s.made.append(m); return m
    def method(s, name, nparams, lines):
        assert s.abc.trait(s.ci, name) is None, '%s::%s exists' % (s.name, name)
        s.inst['traits'].append(Trait(s.P(name), 1, 0, s.add(name, nparams, lines)))
    def wrap(s, name, keep, lines_of):
        """trait `name` -> a new method built by lines_of(n params); the original stays callable as `keep`"""
        t = s.abc.trait(s.ci, name); assert t is not None and t.kind & 15 == 1, '%s::%s' % (s.name, name)
        assert s.abc.trait(s.ci, keep) is None, 'already patched: %s::%s' % (s.name, keep)
        orig = s.abc.methods[t.b]; n = len(orig.params)
        m = s.add(name, n, lines_of(n), like=orig)
        old = t.b; t.b = m; s.inst['traits'].append(Trait(s.P(keep), 1, 0, old))
    def ei(s, name, *args): return [('getlex', s.EI), ('pushstring', s.S(name))] + [x for a in args for x in a] + [('callpropvoid', s.P('call'), 1 + len(args))]
    def get(s, *path): return [('getlocal0',)] + [('getproperty', s.P(p)) for p in path]
    def call_orig(s, keep, n): return [('getlocal0',)] + [('getlocal', k + 1) for k in range(n)] + [('callpropvoid', s.P(keep), n)]
    def setp(s, loc, prop, val): return [('getlocal', loc)] + val + [('setproperty', s.P(prop))]
    def centre(s): return s.ei('setPosition', [('pushstring', s.S('center'))], [('pushstring', s.S('screen'))], [('pushstring', s.S('center'))])


def set_const(c, name, value):
    """a const / var trait of class c with an integer default gets another default (a new constant pool entry; the old one stays for its other users)"""
    abc = c.abc; t = abc.trait(c.ci, name); assert t is not None and t.kind & 15 in (0, 6), '%s::%s is not a slot' % (c.name, name)
    if t.vkind == 0x03: abc.ints.append(int(value)); t.vindex = len(abc.ints) - 1
    elif t.vkind == 0x04: abc.uints.append(int(value)); t.vindex = len(abc.uints) - 1
    elif t.vkind == 0x06: abc.doubles.append(struct.pack('<d', float(value))); t.vindex = len(abc.doubles) - 1
    else: raise ValueError('%s::%s: default of kind %#x' % (c.name, name, t.vkind))


class Code:
    """the movie's one ABC block and the classes patched in it"""
    def __init__(s, swf):
        s.swf = swf; (s.tag, s.pre, s.abc), = swf.abcs(); s.cls = []
    def of(s, name, ref):
        c = Cls(s.swf, s.abc, (s.tag, s.pre), name, ref); s.cls.append(c); return c
    def done(s):
        s.swf.tags[s.tag][1] = s.pre + s.abc.write()
        return [m for c in s.cls for m in c.made]


# ---------------------------------------------------------------- the layout as data: ONE description, evaluated here (bake / checks / pictures) and compiled to AVM2 (run time)
class Ex:
    """a Number expression: literals (int, (a, b) = a / b), variables, + - * /, min, max, floor. ev(env) computes it here, asm(ctx) compiles it."""
    def __init__(s, op, *a): s.op = op; s.a = a
    @staticmethod
    def of(v): return v if isinstance(v, Ex) else Ex('n', v)
    def __add__(s, o): return Ex('add', s, Ex.of(o))
    def __radd__(s, o): return Ex('add', Ex.of(o), s)
    def __sub__(s, o): return Ex('subtract', s, Ex.of(o))
    def __rsub__(s, o): return Ex('subtract', Ex.of(o), s)
    def __mul__(s, o): return Ex('multiply', s, Ex.of(o))
    def __rmul__(s, o): return Ex('multiply', Ex.of(o), s)
    def __truediv__(s, o): return Ex('divide', s, Ex.of(o))
    def __rtruediv__(s, o): return Ex('divide', Ex.of(o), s)
    def ev(s, env):
        if s.op == 'n': return s.a[0][0] / float(s.a[0][1]) if isinstance(s.a[0], tuple) else float(s.a[0])
        if s.op == 'v': return float(env[s.a[0]])
        if s.op == 'floor': return float(int(s.a[0].ev(env)))
        x, y = s.a[0].ev(env), s.a[1].ev(env)
        if s.op == 'divide': return x / y
        return {'add': x + y, 'subtract': x - y, 'multiply': x * y, 'min': min(x, y), 'max': max(x, y)}[s.op]
    def asm(s, cx):
        if s.op == 'n':
            v = s.a[0]
            def lit(i):   # an integer literal: pushbyte / pushshort while it fits 16 bits, else the int pool (pushshort is sign-extended: see num)
                i = int(i); return num(i) if abs(i) <= 32767 else [('pushint', _int(cx._c, i))]
            if isinstance(v, tuple): return lit(v[0]) + lit(v[1]) + [('divide',)]
            if float(v) == int(v): return lit(v)
            return lit(round(v * 1000)) + num(1000) + [('divide',)]
        if s.op == 'v': return [('getlocal', cx.var(s.a[0]))]
        if s.op == 'floor': return s.a[0].asm(cx) + [('convert_i',), ('convert_d',)]
        if s.op in ('min', 'max'):
            t, u = cx.tmp(), cx.tmp(); a, b = cx.label(), cx.label()
            o = s.a[0].asm(cx) + [('convert_d',), ('setlocal', t)] + s.a[1].asm(cx) + [('convert_d',), ('setlocal', u), ('getlocal', t), ('getlocal', u),
                    ('ifnlt' if s.op == 'min' else 'ifngt', a), ('getlocal', t), ('convert_d',), ('jump', b), ('L', a), ('getlocal', u), ('convert_d',), ('L', b)]
            cx.free(t, u); return o
        return s.a[0].asm(cx) + s.a[1].asm(cx) + [(s.op,)]
def V(name): return Ex('v', name)
def fmin(a, b): return Ex('min', Ex.of(a), Ex.of(b))
def fmax(a, b): return Ex('max', Ex.of(a), Ex.of(b))
def ffloor(a): return Ex('floor', Ex.of(a))
def R(a, b): return Ex('n', (a, b))


class Spec:
    """a layout routine: statements over paths from the document (local 0). A path step: 'name' = a declared instance (getproperty),
    ('nm', name) = getChildByName, ('at', i) = getChildAt, ('dyn', name) = a member the PC code makes at run time (not on the timeline).
    Every step is null-checked at run time (a missing object skips its statement)."""
    def __init__(s): s.st = []
    def let(s, name, e): s.st.append(('let', name, Ex.of(e))); return V(name)
    def on(s, path, sets=(), calls=()): s.st.append(('on', tuple(path), [(p, Ex.of(e)) for p, e in sets], [(m, [Ex.of(e) for e in a]) for m, a in calls]))
    def root(s, prop, e): s.st.append(('on', (), [(prop, Ex.of(e))], []))
    def ei(s, name, *args): s.st.append(('ei', name, [a if isinstance(a, str) else Ex.of(a) for a in args]))
    def raw(s, tag, fn): s.st.append(('raw', tag, fn))                      # fn(spec, class) -> AVM2 lines; nothing to compute here
    def rect(s, path, x, y, w, h): s.st.append(('rect', tuple(path), [Ex.of(v) for v in (x, y, w, h)]))   # object.scrollRect = new Rectangle(x, y, w, h)
    def state(s, var, then, other):
        """var != 0 -> the statements of `then`, else those of `other` (both: fn(spec))"""
        a, b = Spec(), Spec(); then(a); other(b); s.st.append(('if', var, a.st, b.st))
    # ---- here
    def run(s, env, st=None, out=None):
        out = [] if out is None else out
        for t in (s.st if st is None else st):
            if t[0] == 'let': env[t[1]] = t[2].ev(env)
            elif t[0] == 'on':
                for p, e in t[2]: out.append(('set', t[1], p, e.ev(env)))
                for m, a in t[3]: out.append(('call', t[1], m, [e.ev(env) for e in a]))
            elif t[0] == 'ei': out.append(('ei', t[1], [a if isinstance(a, str) else a.ev(env) for a in t[2]]))
            elif t[0] == 'raw': out.append(('raw', t[1]))
            elif t[0] == 'rect': out.append(('rect', t[1], [e.ev(env) for e in t[2]]))
            else: s.run(env, t[2] if env[t[1]] else t[3], out)
        return out
    def paths(s, st=None):
        for t in (s.st if st is None else st):
            if t[0] in ('on', 'rect'): yield t[1]
            elif t[0] == 'if':
                for p in s.paths(t[2]): yield p
                for p in s.paths(t[3]): yield p
    # ---- AVM2
    def asm(s, c, inputs):
        """lines of a method body (after HEAD): inputs = [(variable, document slot)] read first; locals: 1 the object, 2.. Numbers"""
        s._v = {}; s._n = 2; s._l = 0; s._c = c; s._free = []
        o = [('pushnull',), ('coerce_a',), ('setlocal', 1)]
        for name, slot in inputs: o += [('getlocal0',), ('getproperty', c.P(slot)), ('convert_d',), ('setlocal', s.var(name))]
        body = s._lines(s.st)
        pre = []
        for k in range(2 + len(inputs), s._n): pre += [('pushbyte', 0), ('convert_d',), ('setlocal', k)]
        return o + pre + body
    def var(s, name):
        if name not in s._v: s._v[name] = s._n; s._n += 1
        return s._v[name]
    def tmp(s):
        if s._free: return s._free.pop()
        s._n += 1; return s._n - 1
    def free(s, *ts): s._free.extend(ts)   # temps are reused once their min / max is done (a long layout ran out of the 255 locals)
    def label(s): s._l += 1; return 'x%d' % s._l
    def _lines(s, st):
        c = s._c; o = []
        for t in st:
            if t[0] == 'let': o += t[2].asm(s) + [('convert_d',), ('setlocal', s.var(t[1]))]
            elif t[0] == 'ei':
                o += [('getlex', c.EI), ('pushstring', c.S(t[1]))]
                for a in t[2]: o += [('pushstring', c.S(a))] if isinstance(a, str) else a.asm(s)
                o += [('callpropvoid', c.P('call'), 1 + len(t[2]))]
            elif t[0] == 'raw': o += t[2](s, c)
            elif t[0] == 'if':
                a, b = s.label(), s.label()
                o += [('getlocal', s.var(t[1])), ('iffalse', a)] + s._lines(t[2]) + [('jump', b), ('L', a)] + s._lines(t[3]) + [('L', b)]
            else:
                end = s.label(); o += [('getlocal0',), ('coerce_a',), ('setlocal', 1)]
                for step in t[1]:
                    if isinstance(step, str) or step[0] == 'dyn': o += [('getlocal', 1), ('getproperty', c.P(step if isinstance(step, str) else step[1]))]
                    elif step[0] == 'nm': o += [('getlocal', 1), ('pushstring', c.S(step[1])), ('callproperty', c.P('getChildByName'), 1)]
                    else: o += [('getlocal', 1), ('pushbyte', step[1]), ('callproperty', c.P('getChildAt'), 1)]
                    o += [('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', end)]
                if t[0] == 'rect':
                    R_ = c.abc.pub('Rectangle', 'flash.geom')
                    o += [('getlocal', 1), ('findpropstrict', R_)] + [x for e in t[2] for x in e.asm(s)] + [('constructprop', R_, 4), ('setproperty', c.P('scrollRect')), ('L', end)]; continue
                for p, e in t[2]: o += [('getlocal', 1)] + e.asm(s) + [('setproperty', c.P(p))]
                for m, a in t[3]: o += [('getlocal', 1)] + [x for e in a for x in e.asm(s)] + [('callpropvoid', c.P(m), len(a))]
                o += [('L', end)]
        return o


def _recs(swf, cur):
    if cur is not None: return gr.frame_state(swf.tags[_def_index(swf, cur)][1], 1)
    recs = {}
    for code, d, _ in swf.tags:
        if code == gr.T_SHOW: break
        if code in (gr.T_PLACE2, gr.T_PLACE3):
            r = gr.place_parse(code, d)
            if r['char'] is not None: recs[r['depth']] = r
    return recs


def _resolve(swf, path):
    """(sprite id | None = the root timeline, depth) of the placement a static path ends at; None when a step is made at run time"""
    cur = None
    for k, step in enumerate(path):
        if not isinstance(step, str) and step[0] == 'dyn': return None
        recs = _recs(swf, cur)
        if isinstance(step, str) or step[0] == 'nm':
            nm = (step if isinstance(step, str) else step[1]).encode() + b'\0'; hit = [r for r in recs.values() if r['name'] == nm]
        else: hit = [recs[d] for d in sorted(recs)][step[1]:step[1] + 1]
        assert len(hit) == 1, 'layout path %r: step %r not found on the timeline' % (path, step)
        if k == len(path) - 1: return cur, hit[0]['depth']
        cur = hit[0]['char']


def bake(swf, results):
    """the x / y / scaleX / scaleY results of a Spec run are written into the placements (what the movie shows before any code ran);
    -> the results that are not placements (run-time members, calls, document variables)"""
    todo = {}; rest = []
    for r in results:
        if r[0] != 'set' or not r[1] or r[2] not in ('x', 'y', 'scaleX', 'scaleY'): rest.append(r); continue
        at = _resolve(swf, r[1])
        if at is None: rest.append(r); continue
        todo.setdefault(at[0], {}).setdefault(at[1], {})[r[2]] = r[3]
    for sprite, by in todo.items():
        def fn(name, rec, by=by):
            v = by.get(rec['depth'])
            if v: _set(rec, x=v.get('x'), y=v.get('y'), sx=v.get('scaleX'), sy=v.get('scaleY'))
        edit_places(swf, sprite, fn)
    return rest


def add_slot(c, name, value):
    """a public var of the class with an int default"""
    assert c.abc.trait(c.ci, name) is None
    c.inst['traits'].append(Trait(c.P(name), 0, 0, 0, _int(c, int(value)), 0x03))
def _int(c, v):
    if v in c.abc.ints[1:]: return c.abc.ints.index(v, 1)
    c.abc.ints.append(v); return len(c.abc.ints) - 1


CTX_GUARD_MS = 400     # one long press = at most one context menu (the movie's right mouse up and the engine's IE ContextMenu may both come)
CMD = 1000000          # setAnchor(CMD + n): n = 0 / 1 the window's state, n >= 40 the cell px (setAnchor is the one invoke name every UI class registers)
CELL0, SCREEN0 = 76, 1920
MARGIN = 80            # stage px kept free at each side of the screen (notch)
INPUTS = [('C', 'dmCell'), ('ST', 'dmState'), ('DW', 'dmW'), ('BT', 'dmB'), ('OX', 'dmOX'), ('OK', 'dmOXok')]
# Phone run 21.11: the inventory stood at x 784..2217 on a 2400 screen (its FlashPlayer's origin 784, the sheet's 823), not centred: the
# engine's own centring of the movie does not follow setMcSize (main+0xae2ccc applies it only in the layout mode 3). So the movie centres
# ITSELF: the host reads where the engine put the movie (the FlashPlayer's rect, hle_dos2_touchui_hud.c hu_tell_cell) and tells it, in stage
# px, through the same invoke channel: CMD + OX_CMD + OX_BIAS + origin (dmOX, dmOXok = 1) and CMD + DW_CMD + width (dmW, the screen width in
# stage px). dmLay then shifts the window by DX = dmOXok x ((dmW - MW) / 2 - dmOX): its middle on the screen's middle, whatever the engine did.
OX_CMD, OX_BIAS, DW_CMD = 100000, 5000, 200000
BOTTOM0 = 967          # dmB: the bottom limit the host sets (the hot bar's top; `cmd`(CMD + n) with n >= 400); default = the iPad's hot bar top
                       # (21.11: with it the baked inventory is the iPad's 1170 x 968, 7 rows; 1080 = no limit)
HEAD_PADS = [(('inventory_mc', 'autosortBtn_mc'), 48), (('inventory_mc', 'sortByBtn_mc'), 48), (('inventory_mc', 'allBtn_mc'), 48), (('inventory_mc', 'view_mc'), 48)]
                       # header buttons (own px at the header's scale 1.19 x INV_K = 1.05: 28..31 own = 29..33 px): invisible pads 48 own = 50 px tall (LSButton hits its own graphics)
def window_doc(code, spec, off, btn=None, back=0, cmd='setAnchor', ctx=None, pads=(), extra=None):
    """the document class of a `_p` window: vars dmCell (cell px, CELL0 = the iPad's), dmState, dmW (the screen width in stage px, height 1080);
    dmLay() = `spec`; dmSetCell(px) / dmSetState(n) for the host; `cmd`(CMD + n) = the same two through a name the engine class registers;
    onEventInit = original + dmHook (drag bar `off` hidden, the doll button `btn` armed: dmSide(8) = the equipment view, dmSide(back) from it)
    + dmLay; onEventResolution(w, h) = original + dmW + dmLay."""
    d = code.of('MainTimeline', 'onEventInit'); P = d.P
    for n, v in (('dmCell', CELL0), ('dmState', 0), ('dmW', SCREEN0), ('dmB', BOTTOM0), ('dmOX', 0), ('dmOXok', 0)): add_slot(d, n, v)
    d.method('dmLay', 0, HEAD + spec.asm(d, INPUTS) + [('returnvoid',)])
    lay = [('getlocal0',), ('callpropvoid', P('dmLay'), 0)]
    # dmSetCell(px): 40..399 = the cell px; 400..1080 = the bottom limit dmB (the host's hot bar top), one channel for both
    # n >= DW_CMD: dmW = n - DW_CMD (the host's screen width in stage px); n >= OX_CMD: dmOX = n - OX_CMD - OX_BIAS, dmOXok = 1 (run 21.11)
    d.method('dmSetCell', 1, HEAD + [('getlocal1',), ('pushint', _int(d, DW_CMD)), ('iflt', 'nw'),
             ('getlocal0',), ('getlocal1',), ('pushint', _int(d, DW_CMD)), ('subtract',), ('setproperty', P('dmW'))] + lay + [('returnvoid',), ('L', 'nw'),
             ('getlocal1',), ('pushint', _int(d, OX_CMD)), ('iflt', 'no'),
             ('getlocal0',), ('getlocal1',), ('pushint', _int(d, OX_CMD + OX_BIAS)), ('subtract',), ('setproperty', P('dmOX')),
             ('getlocal0',), ('pushbyte', 1), ('setproperty', P('dmOXok'))] + lay + [('returnvoid',), ('L', 'no'),
             ('getlocal1',), ('pushbyte', 40), ('ifnlt', 'e'), ('returnvoid',), ('L', 'e'),
             ('getlocal1',), ('pushshort', 400), ('iflt', 'c'), ('getlocal1',), ('pushshort', 1080), ('ifgt', 'x'),
             ('getlocal0',), ('getlocal1',), ('setproperty', P('dmB'))] + lay + [('returnvoid',), ('L', 'x'), ('returnvoid',),
             ('L', 'c'), ('getlocal0',), ('getlocal1',), ('setproperty', P('dmCell'))] + lay + [('returnvoid',)])
    d.method('dmSetState', 1, HEAD + [('getlocal0',), ('getlocal1',), ('setproperty', P('dmState'))] + lay + [('returnvoid',)])
    hook = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3)]
    for k, (path, h) in enumerate(pads):   # a finger pad: a transparent rectangle in the object's own graphics, h own px tall, centred on its drawing
        hook += [('getlocal0',), ('coerce_a',), ('setlocal', 1)]
        for p in path: hook += [('getlocal', 1), ('getproperty', P(p)), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'pd%d' % k)]
        hook += [('getlocal', 1), ('getlocal', 1), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 2),
                 ('getlocal', 1), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'pd%d' % k),
                 ('getlocal', 3), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', P('beginFill'), 2),
                 ('getlocal', 3), ('getlocal', 2), ('getproperty', P('x')),
                 ('getlocal', 2), ('getproperty', P('y')), ('getlocal', 2), ('getproperty', P('height')), ('pushbyte', 2), ('divide',), ('add',)] + num(h // 2) + [('subtract',),
                 ('getlocal', 2), ('getproperty', P('width'))] + num(h) + [('callpropvoid', P('drawRect'), 4),
                 ('getlocal', 3), ('callpropvoid', P('endFill'), 0), ('L', 'pd%d' % k)]
    for k, path in enumerate(off):
        hook += [('getlocal0',), ('coerce_a',), ('setlocal', 1)]
        for p in path: hook += [('getlocal', 1), ('getproperty', P(p)), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'h%d' % k)]
        hook += [('getlocal', 1), ('pushfalse',), ('setproperty', P('visible')), ('L', 'h%d' % k)]
    if btn:
        for k, nm in enumerate(btn if isinstance(btn, (list, tuple)) else [btn]):
            hook += [('getlocal0',), ('pushstring', d.S(nm)), ('callproperty', P('getChildByName'), 1), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'nb%d' % k),
                     ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseChildren')),
                     ('getlocal', 1), ('pushstring', d.S('mouseUp')), ('getlocal0',), ('getproperty', P('dmBtnUp')), ('callpropvoid', P('addEventListener'), 2), ('L', 'nb%d' % k)]
        d.method('dmBtnUp', 1, HEAD + d.ei('PlaySound', [('pushstring', d.S('UI_Generic_Click'))]) + [('getlocal0',), ('getproperty', P('dmState')), ('iftrue', 'back')] +
                 d.ei('dmSide', [('pushbyte', 8)]) + [('returnvoid',), ('L', 'back')] + d.ei('dmSide', [('pushbyte', back)]) + [('returnvoid',)])
    if ctx:
        # the long press (the host's right click) opens the context menu through the EI the PC class handles (openContextMenu), also when the
        # engine's own "IE ContextMenu" event does not reach the movie; `ctx` = (path of the object with onContextMenuInputUp, its class)
        path, cls = ctx; g = code.of(cls, 'onContextMenuInputUp'); gt = d.abc.pub('getTimer', 'flash.utils')
        g.wrap('onContextMenuInputUp', 'dmCtxOrig', lambda n: HEAD + [('pushbyte', 0), ('convert_d',), ('setlocal', n + 1),
               ('findpropstrict', gt), ('callproperty', gt, 0), ('convert_d',), ('setlocal', n + 1),
               ('getlocal', n + 1), ('getlocal0',), ('getproperty', g.P('dmCtxT')), ('subtract',)] + num(CTX_GUARD_MS) + [('iflt', 'skip'),
               ('getlocal0',), ('callproperty', g.P('dmCtxOrig'), 0), ('convert_b',), ('dup',), ('iffalse', 'ret'),
               ('getlocal0',), ('getlocal', n + 1), ('setproperty', g.P('dmCtxT')),
               ('L', 'ret'), ('returnvalue',), ('L', 'skip'), ('pushfalse',), ('returnvalue',)])
        walk = [('getlocal0',), ('coerce_a',), ('setlocal', 2)]
        for q in path: walk += [('getlocal', 2), ('getproperty', P(q)), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'nc')]
        d.method('dmCtxUp', 1, HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2)] + walk + [('getlocal', 2), ('callproperty', P('onContextMenuInputUp'), 0), ('pop',), ('L', 'nc'), ('returnvoid',)])
        hook += [('getlocal0',), ('coerce_a',), ('setlocal', 1)]
        for q in path: hook += [('getlocal', 1), ('getproperty', P(q)), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'nx')]
        hook += [('getlocal', 1), ('pushstring', d.S('rightMouseUp')), ('getlocal0',), ('getproperty', P('dmCtxUp')), ('callpropvoid', P('addEventListener'), 2), ('L', 'nx')]
    if extra: hook += extra(d)
    d.method('dmHook', 0, hook + [('returnvoid',)])
    d.wrap('onEventInit', 'dmInitOrig', lambda n: HEAD + d.call_orig('dmInitOrig', n) + [('getlocal0',), ('callpropvoid', P('dmHook'), 0)] + lay + [('returnvoid',)])
    d.wrap('onEventResolution', 'dmResOrig', lambda n: HEAD + d.call_orig('dmResOrig', n) + [('getlocal2',), ('pushbyte', 0), ('ifngt', 'z'),
           ('getlocal0',), ('getlocal1',), ('pushshort', 1080), ('multiply',), ('getlocal2',), ('divide',), ('setproperty', P('dmW')), ('L', 'z')] + lay + [('returnvoid',)])
    d.wrap(cmd, 'dmAnchorOrig', lambda n: HEAD + [('pushbyte', 0), ('convert_d',), ('setlocal', n + 1), ('getlocal1',), ('pushint', _int(d, CMD)), ('ifnge', 'o'),
           ('getlocal1',), ('pushint', _int(d, CMD)), ('subtract',), ('convert_d',), ('setlocal', n + 1), ('getlocal', n + 1), ('pushbyte', 40), ('ifnlt', 'cell'),
           ('getlocal0',), ('getlocal', n + 1), ('callpropvoid', P('dmSetState'), 1), ('returnvoid',),
           ('L', 'cell'), ('getlocal0',), ('getlocal', n + 1), ('callpropvoid', P('dmSetCell'), 1), ('returnvoid',),
           ('L', 'o')] + d.call_orig('dmAnchorOrig', n) + [('returnvoid',)])
    return d


def pieces_sprite(swf, before, bid, bw, bh, groups):
    """new sprites made of rectangles of ONE existing bitmap. groups = [(name | None, x, y, [(dst x, dst y, src x, src y, w, h, sx[, sy])])]: every
    group is a 1-frame sprite of its pieces (each its own DefineShape: a clipped, smoothed 1:1 bitmap fill, drawn 1 px larger to the right
    and down while the bitmap has those px, then scaled sx horizontally / sy vertically), placed by name at x, y inside one outer sprite.
    -> (outer id, n tags)"""
    used = {struct.unpack_from('<H', d, 0)[0] for c, d, _ in swf.tags if c in gr.DEFS} | set(gr.Movie(swf).imports)
    nid = [max(used) + 1]; tags = []
    def nb(*v): return max(2, max(abs(int(x)).bit_length() for x in v) + 1)
    def new(): nid[0] += 1; assert nid[0] < 0xffff; return nid[0] - 1
    def place(depth, cid, x, y, sx=1.0, name=None, sy=1.0):
        return gr.tag_bytes(*gr.place_build(dict(depth=depth, char=cid, matrix=gr.matrix_bytes(x, y, sx, sy), cx=None, ratio=None, name=None if name is None else name.encode() + b'\0',
                                                 clip=None, cls=None, f2=0, rest3=b'', move=False, v3=False, actions=None)))
    outer = b''
    for gk, (name, gx, gy, pieces) in enumerate(groups):
        spr = b''
        for k, pc in enumerate(pieces):
            dx, dy, sx, sy, w, h, scx = pc[:7]; scy = pc[7] if len(pc) > 7 else 1.0
            assert 0 <= sx and sx + w <= bw and 0 <= sy and sy + h <= bh, 'piece %r outside the %d x %d bitmap' % ((sx, sy, w, h), bw, bh)
            if scx == 1.0: w = min(w + 1, bw - sx)
            if scy == 1.0: h = min(h + 1, bh - sy)
            W, H = w * 20, h * 20
            fill = b'\x01\x41' + struct.pack('<H', bid) + gr.matrix_bytes(-sx, -sy, 20.0, 20.0)
            rec = [(0, 1), (0, 1), (0, 1), (1, 1), (0, 1), (1, 1), (2, 5), (0, 2), (0, 2), (1, 1)]
            for ex, ey in ((W, 0), (0, H), (-W, 0), (0, -H)):
                n = nb(ex, ey); rec += [(1, 1), (1, 1), (n - 2, 4), (1, 1), (ex, n), (ey, n)]
            rec += [(0, 1), (0, 5)]
            n = nb(W, H); sid = new()
            tags.append([2, struct.pack('<H', sid) + gr.wbits([(n, 5), (0, n), (W, n), (0, n), (H, n)]) + fill + b'\x00' + bytes([0x10]) + gr.wbits(rec), False])
            spr += place(k + 1, sid, dx, dy, scx, None, scy)
        gid = new(); tags.append([gr.T_SPRITE, struct.pack('<HH', gid, 1) + spr + gr.tag_bytes(gr.T_SHOW, b'') + gr.tag_bytes(gr.T_END, b''), False])
        outer += place(gk + 1, gid, gx, gy, 1.0, name)
    oid = new(); tags.append([gr.T_SPRITE, struct.pack('<HH', oid, 1) + outer + gr.tag_bytes(gr.T_SHOW, b'') + gr.tag_bytes(gr.T_END, b''), False])
    at = _def_index(swf, before); swf.tags[at:at] = tags
    return oid, len(tags)


def swap_char(swf, holder, frame, old, new):
    hit = []
    def fn(name, rec):
        if rec['char'] == old: rec['char'] = new; hit.append(1)
    edit_places(swf, holder, fn, frame); assert len(hit) == 1, 'character %d is not placed once in frame %d of sprite %d' % (old, frame, holder)


def doll_button(swf, buttons):
    """the window buttons: our own art (assets/touchui: the copper plate + a glyph each), placed on the root timeline by name at 0,0 scale 1
    (dmLay / bake place them). buttons = [(name, glyph)], one plate bitmap for all. -> (the plate's px, n new definition tags)"""
    import dos2_touchart as ta
    g = gr.Graft(swf); art = ta.Art(g); sq, qb = art.png('plate_square_copper'); pw = float(qb[2] - qb[0])
    for name, glyph in buttons:
        ic, ib = art.png(glyph); ki = 0.72 * pw / max(ib[2] - ib[0], ib[3] - ib[1])
        spr = g.sprite([(sq, 1, (0, 0), None), (ic, 2, (pw / 2 - (ib[2] - ib[0]) * ki / 2, pw / 2 - (ib[3] - ib[1]) * ki / 2, ki), None)])
        g.place(spr, 0, 0, name)
    g.finish()
    return pw, len(g.new_defs)


# ---------------------------------------------------------------- partyInventory_p
INV_CLIP = 'inventory_mc'
def inv_geom(C=CELL0, ST=1, DW=SCREEN0, BT=BOTTOM0):
    """the numbers dmLay computes (the same Spec, run here): dict of the Spec's variables"""
    env = dict(C=C, ST=ST, DW=DW, BT=BT, OX=0, OK=0); inv_spec(None).run(env); return env


def inv_spec(k):
    """dmLay of partyInventory_p (owner 23.11: the iPad's inventory, one window - title bar, stat strip, filter row, the doll left and the bag
    right). The window is the SHARED grid (win_lets, the same statements as the sheet): the sheet movie in its state 1 draws the frame, the title
    bar, the HP box, the doll and the stat strip; this movie draws only what stands in the open right column from the filter row down - the
    filter row (its ground = the PC header's ornate bar; party tabs, All, the party view check, sort, sort by, the gold) and the bag (the PC
    list x SQ, centred in the column, as many rows as the height holds, scrolling). State 0 (the inventory alone - the host opens the pair, so
    only a fallback): a plain black ground under the whole window, the PC title and close button on it."""
    s = Spec(); C, DW, ST = V('C'), V('DW'), V('ST'); OXK = V('OK') * V('OX')
    F, S, D, WW, WH = win_lets(s, C, DW, V('BT'), OXK)
    WX, WY, RX, RW, PY, PH, PZ, FT, FH = [V(n) for n in ('WX', 'WY', 'RX', 'RW', 'PY', 'PH', 'PZ', 'FT', 'FH')]
    s.state('ST', lambda a: a.let('XS', 1), lambda a: a.let('XS', 0)); XS = V('XS')
    BP = 6 * F
    SQ = s.let('SQ', fmin(C / 50 * INV_K, (RW - 2 * BP) / (INV_LIST_W + 24)))   # the bag: the host's cell x INV_K, narrower when the column is
    LH = s.let('LH', (PH - 2 * BP) / SQ)                                            # the list frame's height (own px; it scrolls)
    s.let('ROWS', ffloor((LH - INV_BAG_HEAD) / INV_PITCH))
    BX = s.let('BX', RX + (RW - INV_LIST_W * SQ) / 2); BY = s.let('BY', PY + BP)
    SB = s.let('SB', fmin(F, fmin((FH - 6) / 41, (RW - 2 * INV_BAR[3][0][0] * FH / INV_BAR[2]) / INV_HEAD_ITEMS)))   # the filter row's items
    s.let('SW', ffloor(fmax(DW, WW) + R(999, 1000)))
    s.let('S', SQ); s.let('X1', WX); s.let('Y0', WY); s.let('MW', WW); s.let('HS', WH); s.let('PX', 50 * SQ)
    if k is None: return s
    w = (INV_CLIP,); bg = w + ('bgFrame_mc', ('at', 0))
    s.on(w, [('scaleX', 1), ('scaleY', 1), ('x', WX), ('y', WY)])
    FAR0 = (1 - XS) * CS_FAR * 2; FAR1 = XS * CS_FAR * 2
    def pc(g, kk, x, y, kx, ky): s.on(bg + (('nm', g), ('at', kk)), [('x', x), ('y', y), ('scaleX', kx), ('scaleY', ky)])
    gw, gh = INV_BAG[2], INV_BAG[3]
    pc('j0', 0, RX, FT + FH, RW / gw, (PZ - FT - FH) / gh)                      # the bag's ground (under the gap above the bag too)
    pc('j0', 1, 0 + FAR1, 0, WW / gw, WH / gh)                                   # state 0: the whole window
    # the filter row's ornate bar: ornaments x FH / bar height, the two plain spans share the rest of RW
    bx, by, bh, parts = INV_BAR; kb = FH / bh; fixed = sum(w_ for w_, keep, _ in parts if keep); x = RX
    nplain = len([1 for _, keep, _ in parts if not keep]); EM = s.let('EM', parts[0][0] * kb)   # the end ornament: the row's inner margin
    for kk, (w_, keep, _) in enumerate(parts):
        kx = kb if keep else (RW - fixed * kb) / (nplain * w_)
        pc('j1', kk, x, FT, kx, kb); x = x + w_ * kx
    # the bag
    s.on(w + ('listHolder_mc',), [('x', BX), ('y', BY), ('scaleX', SQ), ('scaleY', SQ)])
    s.on(w + ('faderTop',), [('x', BX), ('y', BY), ('scaleX', k['faderTop'][3] * SQ * INV_LIST_W / INV_LIST_W0), ('scaleY', k['faderTop'][4] * SQ)])
    s.on(w + ('faderBot',), [('x', BX), ('y', BY + LH * SQ), ('scaleX', k['faderBot'][3] * SQ * INV_LIST_W / INV_LIST_W0), ('scaleY', k['faderBot'][4] * SQ)])
    s.on(w + (('dyn', 'list'),), calls=[('setFrame', [INV_LIST_W, LH])])
    s.on(w + (('dyn', 'list'), ('dyn', 'm_scrollbar_mc')), calls=[('scrollbarVisible', [])])
    s.on(w + ('sortByBg_mc',), [('x', RX), ('y', FT), ('scaleX', RW / 665), ('scaleY', (PZ - FT) / 827)])
    SP = s.let('SP', fmin(F, PH / 470))
    s.on(w + ('sortBy_mc',), [('x', RX + RW / 2 - (k['sortBy_mc'][1] + 158) * 0 - 331 * SP), ('y', PY + (PH - 444 * SP) / 2 - 163 * SP), ('scaleX', SP), ('scaleY', SP)])
    # the filter row: party tabs and All at the left, the party view check after them, sort / sort by / the gold at the right
    CY_ = FT + FH / 2
    s.on(w + ('tabs_mc',), [('x', RX + EM), ('y', CY_ - 20 * SB), ('scaleX', SB), ('scaleY', SB)])
    s.on(w + ('allBtn_mc',), [('x', RX + EM + (INV_TABS_W + 8) * SB), ('y', CY_ - 13 * SB), ('scaleX', SB), ('scaleY', SB)])
    s.on(w + ('view_mc',), [('x', RX + EM + (INV_TABS_W + 100) * SB), ('y', CY_ - 14 * SB), ('scaleX', SB), ('scaleY', SB)])
    GX = RX + RW - EM - 138 * SB
    s.on(w + ('totalGold_mc',), [('x', GX), ('y', CY_ - 18 * SB), ('scaleX', SB), ('scaleY', SB)])
    s.on(w + ('sortByBtn_mc',), [('x', GX - 10 * SB - 140 * SB), ('y', CY_ - 14 * SB), ('scaleX', SB), ('scaleY', SB)])
    s.on(w + ('autosortBtn_mc',), [('x', GX - 10 * SB - 280 * SB), ('y', CY_ - 14 * SB), ('scaleX', SB), ('scaleY', SB)])
    # the clicks this window eats: the filter row and the bag (the rest is the sheet's); state 0: the whole window
    hx, hy, hw, hh = RX - GG / 2 + FAR0 * 0, FT, RW + GG, PZ - FT + GM
    s.on(w + ('hitArea_mc',), [('x', XS * hx), ('y', XS * hy), ('scaleX', k['hitArea_mc'][3] * (XS * hw + (1 - XS) * WW) / 676), ('scaleY', k['hitArea_mc'][4] * (XS * hh + (1 - XS) * WH) / 1016)])
    # the PC title, close and minimise: state 0 only (in state 1 the sheet's title bar is the window's)
    s.on(w + ('title_txt',), [('x', WW / 2 - 252 + FAR1), ('y', 14)])
    s.on(w + ('close_mc',), [('x', WW - 12 - INV_CLOSE + FAR1), ('y', 12), ('scaleX', R(INV_CLOSE, 23)), ('scaleY', R(INV_CLOSE, 23))])
    for n in ('minimise_mc', 'keyHintsHolder_mc'): s.on(w + (n,), [('x', CS_FAR * 2)])
    for prop, e in (('uiLeft', WX), ('uiRight', WX + WW), ('uiTop', WY), ('uiMinHeight', 190), ('uiMinWidth', 150)): s.root(prop, ffloor(e + R(1, 2)))
    s.let('DX', WX)
    s.ei('setMcSize', V('SW'), 1080); s.ei('setPosition', 'topleft', 'screen', 'topleft')
    return s


def _kids(swf, sprite):
    kids = {}
    def note(name, rec):
        if name: kids[name] = (rec['char'],) + _mat(rec)
    edit_places(swf, sprite, note); return kids


def party(data, env=None):
    """env = dict(C, ST, DW, BT) the baked layout (default: the iPad's cell on a 16:9 screen, the inventory window); dmLay computes the same at run time"""
    swf = Swf(data); mv = gr.Movie(swf)
    root = _root_names(swf); win = root[INV_CLIP]['char']; assert _mat(root[INV_CLIP]) == (0.0, 20.0, 1.0, 1.0), 'inventory_mc is not at 0,20'
    kids = _kids(swf, win)
    need = ['bgFrame_mc', 'hitArea_mc', 'faderTop', 'faderBot', 'close_mc', 'keyHintsHolder_mc', 'sortByBg_mc', 'sortBy_mc', 'listHolder_mc', 'minimise_mc', 'dragHit_mc', 'title_txt',
            'tabs_mc', 'view_mc', 'autosortBtn_mc', 'sortByBtn_mc', 'allBtn_mc', 'totalGold_mc']
    assert all(n in kids for n in need), 'partyInventory_1 children: %s' % sorted(kids)
    assert kids['listHolder_mc'][2] == INV_LIST_Y, 'list place'
    # the background: frame 1 of bgFrame_mc is one 680 x 1056 bitmap -> the bag's ground (j0: the bag, the whole window in state 0) and the
    # filter row's ornate bar (j1, 5 pieces); everything else of the window is the sheet movie's (state 1)
    holder = kids['bgFrame_mc'][0]; shape = list(gr.frame_state(swf.tags[_def_index(swf, holder)][1], 1).values()); assert len(shape) == 1; shape = shape[0]['char']
    bid, bw, bh = bitmap_of(swf, shape); assert (bw, bh) == (INV_W0, INV_H0), 'background %d x %d' % (bw, bh)
    gx, gy, gw, gh = INV_BAG; bx, by, bhh, parts = INV_BAR
    bar = [(0, 0, sx, by, w_, bhh, 1.0 if keep else STRETCH) for w_, keep, sx in parts]
    new, ndef = pieces_sprite(swf, holder, bid, bw, bh, [('j0', 0, 0, [(0, 0, gx, gy, gw, gh, STRETCH)] * 2), ('j1', 0, 0, bar)])
    swap_char(swf, holder, 1, shape, new)
    # a bag's header (playerInventory): its frame, body and drop mark follow the list's width, gold / weight / the fold mark stay at the right end
    lx = INV_LIST_W / float(INV_LIST_W0)
    bag = [c for c, n in mv.classes.items() if n == 'playerInventory']; assert len(bag) == 1
    bk = _kids(swf, bag[0]); assert all(n in bk for n in ('invBg_mc', 'frame_mc', 'drop_mc', 'weightAndGoldIcons_mc', 'gold_txt', 'weight_txt', 'toggleInd_mc')), sorted(bk)
    def bagfix(name, rec):
        x, y, sx, sy = _mat(rec)
        if name in ('invBg_mc', 'frame_mc', 'drop_mc'): _set(rec, sx=sx * lx)
        elif name in ('weightAndGoldIcons_mc', 'gold_txt', 'weight_txt', 'toggleInd_mc'): _set(rec, dx=INV_ADD)
    edit_places(swf, bag[0], bagfix)

    env = dict(env or {}); env.setdefault('C', CELL0); env.setdefault('ST', 1); env.setdefault('DW', SCREEN0); env.setdefault('BT', BOTTOM0); env.setdefault('OX', 0); env.setdefault('OK', 0)
    spec = inv_spec(_kids(swf, win)); res = spec.run(env); rest = bake(swf, res)
    swf.head = rect_bytes(STAGE_WMAX, STAGE_H) + swf.head[-4:]

    code = Code(swf)
    window_doc(code, spec, [(INV_CLIP, 'minimise_mc'), (INV_CLIP, 'dragHit_mc')], None, 3, ctx=((INV_CLIP,), 'partyInventory_1'), pads=HEAD_PADS)
    # the sort popup: its OK sends setSortFilter (handled) and applySortFilters (NOT in main.raw: nothing would be sorted) -> then `autosort`,
    # the call of the sort button, which the class handles (main+0x13a77f0)
    sp = code.of('sortPopup_21', 'okPressed')
    sp.wrap('okPressed', 'dmOkOrig', lambda n: HEAD + sp.call_orig('dmOkOrig', n) + sp.ei('autosort') + [('returnvoid',)])
    # a cell's tooltip rectangle in stage px (the PC code passes the cell's own 50 x 50)
    w = code.of('partyInventory_1', 'onCellCreate')
    px = [('getlocal0',), ('getproperty', w.P('scaleX')), ('getlocal0',), ('getproperty', w.P('listHolder_mc')), ('getproperty', w.P('scaleX')), ('multiply',)] + num(50) + [('multiply',)]   # the cells' scale: clip x list
    w.wrap('onCellCreate', 'dmCellOrig', lambda n: HEAD + w.call_orig('dmCellOrig', n) + [('getlocal1',), ('iffalse', 'end')] +
           w.setp(1, 'tooltipOverrideW', px) + w.setp(1, 'tooltipOverrideH', px) + [('L', 'end'), ('returnvoid',)])
    # the grid the engine reads once from inventory_mc (main+0x13aa5b0: cSize, cSpacing, bgDiscrap, invColW, portraitH / W)
    for name, v in (('invColW', INV_COLS), ('cSpacing', INV_PITCH - 50), ('invW', INV_LIST_W), ('invH', int(inv_geom()['LH']))): set_const(w, name, v)
    made = code.done()
    e = env
    return swf.write(), dict(placed=[], defs=ndef, methods=made, env=env, rest=rest, window=(e['WX'] + e['RX'], e['WY'] + e['FT'], e['WX'] + e['RX'] + e['RW'], e['WY'] + e['PZ']),
                             bitmap=bid, shape=new, stage=(STAGE_WMAX, STAGE_H), spec=spec)


# ---------------------------------------------------------------- characterSheet_p
CS_CLIP = 'stats_mc'
def cs_geom(C=CELL0, ST=0, DW=SCREEN0, BT=BOTTOM0):
    env = dict(C=C, ST=ST, DW=DW, BT=BT, OX=0, OK=0); cs_spec(None).run(env); return env


def frame_groups(prefix=''):
    """the window frame's pieces of the PC sheet background (art px, SA_*): [(group name, 0, 0, pieces)] for pieces_sprite. Every piece is
    placed at 0,0 here and put in its place / size by dmLay (frame_spec). Depth order = drawing order: the body first, the top strip last."""
    bx, by, bw, bh = SA_BODY; dx, dy, dw, dh = SA_DOLL; ox, oy, ow, oh, oe = SA_BOX; px, py, pw, ph, pc = SA_PANEL; lx, rx, ty = SA_TILE
    one = lambda x, y, w, h: (0, 0, x, y, w, h, 1.0)          # drawn 1 px larger (no hairline seam to its neighbour)
    st = lambda x, y, w, h: (0, 0, x, y, w, h, STRETCH)       # stretched by dmLay: exactly its px (a bleed pixel x 30 overshoots)
    box = [one(ox, oy, ow, oe), st(ox, oy + oe, ow, oh - 2 * oe), one(ox, oy + oh - oe, ow, oe)]
    groups = [('w0', [st(bx, by, bw, bh) for _ in range(6)]),                                           # the body: 6 rectangles round the panels
              ('w7', [one(dx, dy, dw, dh)]),                                                             # the doll frame
              ('w6', [one(px, py, pc, pc), st(px + pc, py, pw - 2 * pc, pc), one(px + pw - pc, py, pc, pc),    # the parchment frame: TL T TR
                      st(px, py + pc, pc, ph - 2 * pc), st(px + pw - pc, py + pc, pc, ph - 2 * pc),             # L R
                      one(px, py + ph - pc, pc, pc), st(px + pc, py + ph - pc, pw - 2 * pc, pc), one(px + pw - pc, py + ph - pc, pc, pc)]),   # BL B BR
              ('w4', box + box),                                                                         # the black boxes: HP / AP / Source; the stat strip
              ('w5', [one(*SA_PLAQUE)]),
              ('w3', [st(0, ty, SA_RAIL, SA_BOT - ty), st(CS_W - SA_RAIL, ty, SA_RAIL, SA_BOT - ty)]),
              ('w2', [one(0, SA_BOT, SA_L, SA_BOT_H), one(SA_R, SA_BOT, CS_W - SA_R, SA_BOT_H)] + [one(SA_BOTP, SA_BOT, SA_P, SA_BOT_H) for _ in range(NPER)]),
              ('w1', [one(0, 0, SA_L, SA_TOP_H), one(0, SA_TOP_H, lx, ty - SA_TOP_H), one(SA_R, 0, CS_W - SA_R, SA_TOP_H), one(rx, SA_TOP_H, CS_W - rx, ty - SA_TOP_H)] +
                     [one(SA_L, 0, SA_P, SA_TOP_H) for _ in range(NPER)])]
    return [(prefix + n, 0, 0, p) for n, p in groups]


def frame_spec(s, bg):
    """dmLay statements of the frame pieces (win_lets' variables; window px; XS = the state): bg = the path of the pieces' outer sprite.
    State 0 (the sheet): the body black fills row A and the gutters round the doll and the parchment panel. State 1 (the inventory window):
    the parchment and the plaque go, the stat strip's box comes, and the right column from the filter row down (FT .. PZ) stays OPEN - the
    inventory movie draws the filter row and the bag there (the two movies' drawings never overlap: their order on the screen is the engine's)."""
    F, WW, WH, PY, PZ, PH, LX, LW, RX, RW, D, CT, IH, SR, XS, FT = [V(n) for n in ('F', 'WW', 'WH', 'PY', 'PZ', 'PH', 'LX', 'LW', 'RX', 'RW', 'D', 'CT', 'IH', 'SR', 'XS', 'FT')]
    FAR = XS * CS_FAR * 2
    def pc(g, k, x, y, kx, ky): s.on(bg + (('nm', g), ('at', k)), [('x', x), ('y', y), ('scaleX', kx), ('scaleY', ky)])
    bw, bh = SA_BODY[2], SA_BODY[3]
    TB = SA_TOP_H * F; Y1 = s.let('BY1', PY + XS * (FT - PY)); H1 = PZ - Y1
    for k, (x, y, w, h) in enumerate(((SR, TB, WW - 2 * SR, Y1 - TB), (SR, PZ, WW - 2 * SR, WH - (SA_BOT_H - SA_BOT_BAR) * F - PZ), (SR, Y1, LX - SR, H1),
                                      (LX + LW, Y1, RX - LX - LW, H1), (RX + RW, Y1, WW - SR - RX - RW, H1), (LX + (1 - XS) * CS_FAR * 2, FT, LW, PY - FT))):
        pc('w0', k, x, y, w / bw, h / bh)
    pc('w7', 0, LX, PY, D, D)
    pw, ph, c = SA_PANEL[2], SA_PANEL[3], SA_PANEL[4]
    X1, Y2 = RX + RW - c * F + FAR, PY + PH - c * F; ew, eh = (RW - 2 * c * F) / (pw - 2 * c), (PH - 2 * c * F) / (ph - 2 * c)
    for k, (x, y, kx, ky) in enumerate(((RX + FAR, PY, F, F), (RX + c * F + FAR, PY, ew, F), (X1, PY, F, F), (RX + FAR, PY + c * F, F, eh), (X1, PY + c * F, F, eh),
                                         (RX + FAR, Y2, F, F), (RX + c * F + FAR, Y2, ew, F), (X1, Y2, F, F))):
        pc('w6', k, x, y, kx, ky)
    # the side rails, the bottom and top strips (the period repeated n times, each scaled to the exact width: 0.9..1.1)
    rt = SA_TILE[2]
    pc('w3', 0, 0, rt * F, F, (WH - SA_BOT_H * F - rt * F) / (SA_BOT - rt)); pc('w3', 1, WW - SR, rt * F, F, (WH - SA_BOT_H * F - rt * F) / (SA_BOT - rt))
    n = s.let('NP', fmax(1, ffloor((WW / F - SA_L - (CS_W - SA_R)) / SA_P + R(1, 2))))
    kk = s.let('KP', (WW / F - SA_L - (CS_W - SA_R)) / (n * SA_P))
    for g, y0, k0 in (('w2', WH - SA_BOT_H * F, 2), ('w1', 0, 4)):
        pc(g, 0, 0, y0, F, F); pc(g, 1 if g == 'w2' else 2, WW - (CS_W - SA_R) * F, y0, F, F)
        for k in range(NPER):
            vis = V('NV%d' % k) if g == 'w1' else s.let('NV%d' % k, fmax(0, fmin(1, n - k)))   # 1: copy k is drawn, 0: it waits off the window
            pc(g, k0 + k, vis * (SA_L * F + k * SA_P * F * kk) + (1 - vis) * CS_FAR * 2, y0, F * kk, F)
    pc('w1', 1, 0, SA_TOP_H * F, F, F); pc('w1', 3, WW - (CS_W - SA_TILE[1]) * F, SA_TOP_H * F, F, F)


def box_spec(s, bg, k3, x, y, w, h):
    """the black box (3 pieces of group w4 from index k3): x, y, w, h window px; its edge rows at the frame's scale"""
    ow, oh, oe = SA_BOX[2], SA_BOX[3], SA_BOX[4]; F = V('F')
    def pc(k, yy, ky): s.on(bg + (('nm', 'w4'), ('at', k3 + k)), [('x', x), ('y', yy), ('scaleX', w / ow), ('scaleY', ky)])
    pc(0, y, F); pc(1, y + oe * F, (h - 2 * oe * F) / (oh - 2 * oe)); pc(2, y + h - oe * F, F)


CS_PANEL0 = (24, 293)            # the PC list panel's top left (own px of stats_mc): what the panel content keeps its place to
CS_CONTENT = ['mainStats_mc', 'combatAbilityHolder_mc', 'civicAbilityHolder_mc', 'talentHolder_mc', 'tagsHolder_mc', 'visualHolder_mc', 'customStats_mc',
              'invTabHolder_mc', 'skillTabHolder_mc', 'panelBg2_mc']   # (the last three: the game master's; they keep the content's transform)
CS_WARN = ['attrPointsWrn_mc', 'combatAbilityPointsWrn_mc', 'civilAbilityPointsWrn_mc', 'talentPointsWrn_mc']   # the tabs' "points to spend" glows
CS_TABS0 = (4, 205)              # tabsHolder_mc (own px of stats_mc); a tab 76 x 56 at x 66 k, y 21 / 23 (deselected / selected)
CS_TABS_W = 340                  # 5 tabs, 76 wide at a 66 pitch
CS_SB = (342 - 293, 12)          # the lists' scroll bars (in scrollbarHolder_mc, PC at 9,342): their top below the panel's top; px left of the
                                 # content's right end (the PC had them in the frame's left groove)
CS_TOGGLE = 'dmDoll_mc'          # the window button: the sheet <-> the inventory (our plate + glyph, on the title bar's left tile)
CS_TILE_C = (33, 48)             # art px: the centre of the title bar's left tile (the PC window icon) - the toggle; the right tile centre is
                                 # mirrored (CS_W - 33): the close button
TOGGLE_PX = 54                   # frame art px of the toggle plate (x F; iPad 73 at its 1.0)
CS_TOGGLE2 = 'dmDoll2_mc'        # the same button in the inventory window (state 1): back to the sheet (its own glyph)
STRIP_PITCH = 30                 # own px between the strip's two rows (secondary stats / resistances)
def cs_spec(k, glow=None):
    """dmLay of characterSheet_p (owner 23.11: the iPad's characterSheet_t structure in our art). The clip stats_mc stands at the window's top
    left at scale 1, every child is placed in window px (win_lets): the frame pieces (frame_spec), the HP / AP / Source box (the PC's infoStatList,
    moved out of the doll to row A), the doll (equip_mc + its frame, scale D), the 5 tabs and the tab title plaque, ONE parchment panel with
    the content (mainStats / the tab lists, scale S, two columns).
    State 1 = the INVENTORY window's frame and left part (the inventory movie, partyInventory_p, draws its filter row and bag in the open
    right column): the same title bar (its button now goes back to the sheet), the same HP box and doll in the same place, and the stat strip
    in row A's right part - mainStats_mc's secondary stats and resistances laid out as two rows of value + icon (dmStrip, every frame)."""
    s = Spec(); C, DW, ST = V('C'), V('DW'), V('ST'); OXK = V('OK') * V('OX')
    F, S, D, WW, WH = win_lets(s, C, DW, V('BT'), OXK)
    WX, WY, LX, LW, RX, RW, PY, PH, CT, IH, SH, FT = [V(n) for n in ('WX', 'WY', 'LX', 'LW', 'RX', 'RW', 'PY', 'PH', 'CT', 'IH', 'SH', 'FT')]
    # the HP / AP / Source rows (InfoStat: 22 + 4 own a row, INFO_W wide) at SI: as big as the box (row A x the doll column) holds, not over S
    BP = 8 * F
    SI = s.let('SI', fmin(S, fmin((IH - 2 * BP) / 76, (LW - 2 * BP) / INFO_W)))
    SS = s.let('SS', fmin(S, (SH - BP) / (2 * STRIP_PITCH)))      # the stat strip's two rows in its box
    PBo = s.let('PB', (PH - 2 * PADP * F) / S)                     # the tab lists' frame height in their own px (cs_lists)
    PCX, PCY = s.let('PCX', RX + PADP * F), s.let('PCY', PY + PADP * F)
    s.state('ST', lambda a: a.let('XS', 1), lambda a: a.let('XS', 0))
    XS = V('XS'); FAR = XS * CS_FAR * 2
    s.let('CX', WX); s.let('CY', WY)
    s.let('SW', ffloor(fmax(DW, WW) + R(999, 1000)))
    s.let('DX', WX)
    if k is None: return s
    w = (CS_CLIP,); bg = w + ('bg_mc', ('at', 0))
    s.on(w, [('scaleX', 1), ('scaleY', 1), ('x', WX), ('y', WY)])
    s.rect(w, 0, 0, WW, WH)                                         # nothing of the clip outside the window (the PC's other parts wait far off)
    frame_spec(s, bg)
    box_spec(s, bg, 0, LX, CT, LW, IH)
    box_spec(s, bg, 3, RX + (1 - XS) * CS_FAR * 2, CT, RW, SH)     # the stat strip's box (state 1)
    s.on(bg + (('nm', 'w5'), ('at', 0)), [('x', RX + RW / 2 - SA_PLAQUE[2] / 2 * F + FAR), ('y', CT + IH - SA_PLAQUE[3] * F), ('scaleX', F), ('scaleY', F)])
    # the doll: its frame piece, equip_mc and the backdrop from the doll frame's art origin (SA_DOLL = equip_mc's PC place 341, 253)
    s.on(w + ('equip_mc',), [('x', LX), ('y', PY), ('scaleX', k['equip_mc'][3] * D), ('scaleY', k['equip_mc'][4] * D)])
    s.on(w + (('nm', 'dmDollBg'),), [('x', LX + (k['dmDollBg'][1] - SA_DOLL[0]) * D), ('y', PY + (k['dmDollBg'][2] - SA_DOLL[1]) * D),
                                      ('scaleX', k['dmDollBg'][3] * D), ('scaleY', k['dmDollBg'][4] * D)])
    # the info rows: equip_mc's infoStatHolder_mc, in equip_mc's px (D): the rows' block centred in the box, the icon 13 own right of the edge
    s.on(w + ('equip_mc', 'infoStatHolder_mc'), [('x', (BP + 13 * SI) / D), ('y', (CT + (IH - 74 * SI) / 2 + 3 * SI - PY) / D), ('scaleX', SI / D), ('scaleY', SI / D)])
    # the tabs (F), their glows, the tab title plaque with the title, the points left (right of the plaque); off in state 1
    TX, TY = s.let('TX', RX + RW / 2 - CS_TABS_W / 2 * F), s.let('TY', CT - 21 * F)
    s.on(w + ('tabsHolder_mc',), [('x', TX + FAR), ('y', TY), ('scaleX', F), ('scaleY', F)])
    for i, n in enumerate(CS_WARN):   # tab i of the PC's order (attributes, combat, civil, talents): its top right corner
        s.on(w + (n,), [('x', TX + (66 * i + 64) * F + FAR), ('y', TY + 26 * F), ('scaleX', k[n][3] * F * R(6, 10)), ('scaleY', k[n][4] * F * R(6, 10))])
    QY = CT + IH - SA_PLAQUE[3] * F
    s.on(w + ('tabTitle_txt',), [('x', RX + RW / 2 - 165 * F + FAR), ('y', QY + 6 * F), ('scaleX', F), ('scaleY', F)])
    s.on(w + ('pointsFrame_mc',), [('x', RX + RW - PADP * F - 223 * F + FAR), ('y', QY + 7 * F), ('scaleX', F), ('scaleY', F)])
    # the panel: ONE parchment under the frame's hole (two halves, even mirrored, met in a dark fold: the 'open book' the owner rejected)
    IN = 4 * F
    s.on(w + ('panelBg1_mc',), [('x', RX + IN + FAR), ('y', PY + IN), ('scaleX', (RW - 2 * IN) / 314), ('scaleY', (PH - 2 * IN) / CS_PB0)])
    for n in CS_CONTENT[1:]:   # the panel's content keeps its PC place to the panel's top left, x S
        s.on(w + (n,), [('x', PCX + (k[n][1] - CS_PANEL0[0]) * S + FAR), ('y', PCY + (k[n][2] - CS_PANEL0[1]) * S), ('scaleX', k[n][3] * S), ('scaleY', k[n][4] * S)])
    s.on(w + ('scrollbarHolder_mc',), [('x', PCX + (PANEL_W - CS_SB[1]) * S + FAR), ('y', PCY + CS_SB[0] * S), ('scaleX', S), ('scaleY', S)])
    # mainStats_mc: the attributes tab's content (state 0) / the stat strip (state 1: two rows, dmStrip lays each list out as value + icon
    # cells and centres the block on dmStX + dmStW / 2)
    ms = w + ('mainStats_mc',); m = 'mainStats_mc'
    def sheet_ms(a):
        a.on(ms, [('x', PCX + (k[m][1] - CS_PANEL0[0]) * S), ('y', PCY + (k[m][2] - CS_PANEL0[1]) * S), ('scaleX', k[m][3] * S), ('scaleY', k[m][4] * S)])
        a.on(ms + ('statHolder_mc',), [('x', 20), ('y', 30)]); a.on(ms + ('secStatHolder_mc',), [('x', 20 + COL2), ('y', 30)])
        a.on(ms + ('resistancesStatHolder_mc',), [('x', 20), ('y', CS_RES_Y)])
    def strip_ms(a):
        a.on(ms, [('x', RX + BP), ('y', CT + (SH - 2 * STRIP_PITCH * SS) / 2 + 2 * SS), ('scaleX', SS), ('scaleY', SS)])
        a.on(ms + ('statHolder_mc',), [('x', CS_FAR * 4), ('y', 30)]); a.on(ms + ('secStatHolder_mc',), [('x', 0), ('y', 0)])
        a.on(ms + ('resistancesStatHolder_mc',), [('x', 0), ('y', STRIP_PITCH)])
    s.state('ST', strip_ms, sheet_ms)
    s.root('dmStX', RX + BP); s.root('dmStW', RW - 2 * BP)
    s.raw('lists', cs_lists)
    for h, sy in sorted((glow or {}).items()):   # a tab list's glow behind it (PC: the 735 px panel)
        s.on(w + (h, 'bgGlow_mc'), [('scaleY', sy * PBo / CS_PB0)])
    # the title bar: name - level centred (charInfo_mc: its text is centred on the clip's origin by setPlayerInfo), the close button on the right tile
    TF = F * R(11, 10)
    s.on(w + ('charInfo_mc',), [('x', WW / 2), ('y', 35 * F - 16 * TF), ('scaleX', TF), ('scaleY', TF)])
    s.on(w + ('title_txt',), [('x', CS_FAR)]); s.on(w + ('equipment_txt',), [('x', CS_FAR)])
    s.on(w + ('close_mc',), [('x', WW - CS_TILE_C[0] * F - CS_CLOSE / 2), ('y', CS_TILE_C[1] * F - CS_CLOSE / 2), ('scaleX', R(CS_CLOSE, 23)), ('scaleY', R(CS_CLOSE, 23))])
    # the clicks the window eats: all of it on the sheet; in the inventory window its top (title bar + row A over the filter row) - the doll's
    # own hit, slots and the inventory movie's hit area cover the rest
    hy = XS * 0 + (1 - XS) * WH + XS * FT
    s.on(w + ('hitArea_mc',), [('x', 0), ('y', 0), ('scaleX', k['hitArea_mc'][3] * WW / CS_W), ('scaleY', k['hitArea_mc'][4] * hy / CS_HIT_H)])
    # the window button on the left tile: sheet -> inventory (state 0), inventory -> sheet (state 1)
    TP = TOGGLE_PX * F
    for name, far in ((INV_BTN, FAR), (CS_TOGGLE2, (1 - XS) * CS_FAR * 2)):
        s.on((('nm', name),), [('x', WX + CS_TILE_C[0] * F - TP / 2 + far), ('y', WY + CS_TILE_C[1] * F - TP / 2), ('scaleX', TP / BTN_ART), ('scaleY', TP / BTN_ART)])
    for prop, e in (('uiLeft', WX), ('uiRight', WX + WW), ('uiTop', WY), ('uiMinHeight', 190), ('uiMinWidth', 150)): s.root(prop, ffloor(e + R(1, 2)))
    s.ei('setMcSize', V('SW'), STAGE_H); s.ei('setPosition', 'topleft', 'screen', 'topleft')   # phone 23.11: see inv_spec ('center' put it at x 881, cut)
    return s


def cs_lists(sp, c):
    """every tab's scroll list: frame height PB; its scroll bar lives in scrollbarHolder_mc at the place the PC init gave it (kept), PC length - (735 - PB)"""
    P = c.P; pb = sp.var('PB'); o, bar, x, y = sp.tmp(), sp.tmp(), sp.tmp(), sp.tmp(); out = [('pushnull',), ('coerce_a',), ('setlocal', o), ('pushnull',), ('coerce_a',), ('setlocal', bar)]
    for k, (holder, w, ln) in enumerate(CS_LISTS):
        e = sp.label()
        out += [('getlocal0',), ('getproperty', P(CS_CLIP)), ('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', e),
                ('getlocal', o), ('getproperty', P(holder)), ('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', e),
                ('getlocal', o), ('getproperty', P('list')), ('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', e),
                ('getlocal', o), ('getproperty', P('m_scrollbar_mc')), ('coerce_a',), ('setlocal', bar), ('getlocal', bar), ('iffalse', e),
                ('getlocal', bar), ('getproperty', P('x')), ('convert_d',), ('setlocal', x), ('getlocal', bar), ('getproperty', P('y')), ('convert_d',), ('setlocal', y),
                ('getlocal', o)] + num(w + COL2) + [('getlocal', pb), ('callpropvoid', P('setFrame'), 2),
                ('getlocal', bar), ('getlocal', x), ('setproperty', P('x')), ('getlocal', bar), ('getlocal', y), ('setproperty', P('y')),
                ('getlocal', bar)] + num(ln - CS_PB0) + [('getlocal', pb), ('add',), ('callpropvoid', P('setLength'), 1), ('L', e)]
    return out


def add_place(swf, sprite, depth, char, x, y, name):
    """one more placement in frame 1 of a sprite (before its first ShowFrame)"""
    n = _def_index(swf, sprite); d = swf.tags[n][1]; assert swf.tags[n][0] == gr.T_SPRITE and depth not in gr.frame_state(d, 1), 'depth %d is taken' % depth
    rec = dict(depth=depth, char=char, matrix=gr.matrix_bytes(x, y), cx=None, ratio=None, name=name.encode() + b'\0', clip=None, cls=None, f2=0, rest3=b'', move=False, v3=False, actions=None)
    o = d[:4]; done = False
    for code, t in gr.subtags(d):
        if code == gr.T_SHOW and not done: o += gr.tag_bytes(*gr.place_build(rec)); done = True
        o += gr.tag_bytes(code, t)
    assert done; swf.tags[n][1] = o


def sheet(data, env=None):
    swf = Swf(data); mv = gr.Movie(swf)
    root = _root_names(swf); win = root[CS_CLIP]['char']; assert _mat(root[CS_CLIP]) == (0.0, 20.0, 1.0, 1.0), 'stats_mc is not at 0,20'
    kids = _kids(swf, win)
    need = CS_OFF + CS_STAY + CS_CONTENT + CS_WARN + ['panelBg1_mc', 'equip_mc', 'charInfo_mc', 'tabsHolder_mc', 'tabTitle_txt', 'pointsFrame_mc', 'equipment_txt',
                                                     'scrollbarHolder_mc'] + [h for h, _, _ in CS_LISTS]
    assert all(n in kids for n in need), 'stats_1 children: %s' % sorted(kids)
    assert kids['bg_mc'][1:3] == (0.0, 0.0) and kids['panelBg1_mc'][1:3] == CS_PANEL0 and kids['equip_mc'][1:3] == SA_DOLL[:2] and kids['tabsHolder_mc'][1:3] == CS_TABS0, \
        'bg / panel / doll / tabs place'
    # ---- the background: one bitmap (+ four grille tiles over the portrait strip: dropped with it) -> the window frame's pieces (frame_groups)
    holder = kids['bg_mc'][0]; shape = list(gr.frame_state(swf.tags[_def_index(swf, holder)][1], 1).values()); assert len(shape) == 1; shape = shape[0]['char']
    bid, bw, bh = bitmap_of(swf, shape, True); assert (bw, bh) == (CS_W, CS_H0), 'background %d x %d' % (bw, bh)
    new, ndef = pieces_sprite(swf, holder, bid, bw, bh, frame_groups())
    swap_char(swf, holder, 1, shape, new)
    moved = []
    def fix(name, rec):
        x, y, sx, sy = _mat(rec)
        if name in CS_OFF: _set(rec, y=CS_FAR)
        elif name is None and (x, y) == (341.0, 252.0): rec['name'] = b'dmDollBg\0'; moved.append('dmDollBg')   # the doll's backdrop: named, so dmLay can move it
    edit_places(swf, win, fix); assert 'dmDollBg' in moved, 'the doll backdrop (an unnamed shape at 341,252) is not in stats_1'
    # the attributes tab in two columns: attributes + resistances | secondary stats + experience (PC: one column 700 own px tall)
    ms = _kids(swf, kids['mainStats_mc'][0]); assert ms['statHolder_mc'][1:3] == (20.0, 30.0) and ms['secStatHolder_mc'][1:3] == (20.0, 200.0) and ms['resistancesStatHolder_mc'][1:3] == (20.0, 510.0), ms
    def stats(name, rec):
        if name == 'secStatHolder_mc': _set(rec, x=20 + COL2, y=30)
        elif name == 'resistancesStatHolder_mc': _set(rec, y=CS_RES_Y)
    edit_places(swf, kids['mainStats_mc'][0], stats)
    # + / - : a finger-sized invisible hit sprite (the listeners sit on hit_mc); the helmet toggle: beside the helmet slot, twice the size;
    # the close button's invisible hit_mc x 2 round its drawing (50 px drawn -> a 100 px pad)
    hx, hy, hw, hh = CS_PM_HIT; pm = []
    for cls in ('plusButton_62', 'minusButton_65'):
        cid = [c for c, n in mv.classes.items() if n.endswith('.' + cls)]; assert len(cid) == 1, cls
        def hit(name, rec):
            if name == 'hit_mc': assert _mat(rec) == (0.0, 1.0, 1.0, 1.0), 'hit_mc of %s' % cls; _set(rec, x=hx, y=hy, sx=hw / 18.0, sy=hh / 18.0); pm.append(cls)
        edit_places(swf, cid[0], hit)
    def helm(name, rec):
        if name == 'helmet_mc': assert _mat(rec)[:2] == (176.0, 49.0); _set(rec, x=CS_HELMET[0], y=CS_HELMET[1], sx=CS_HELMET[2], sy=CS_HELMET[2]); pm.append(name)
    edit_places(swf, kids['equip_mc'][0], helm)
    def chit(name, rec):
        if name == 'hit_mc': ox, oy, osx, osy = _mat(rec); _set(rec, x=ox - 11.5, y=oy - 11.5, sx=osx * 2, sy=osy * 2); pm.append('close')   # it covers the 23 px from its origin
    edit_places(swf, kids['close_mc'][0], chit); assert len(pm) == 4, pm
    global BTN_ART
    BTN_ART, nbtn = doll_button(swf, [(INV_BTN, 'ico_inventory'), (CS_TOGGLE2, 'ico_charsheet')])

    kids = _kids(swf, win)
    env = dict(env or {}); env.setdefault('C', CELL0); env.setdefault('ST', 0); env.setdefault('DW', SCREEN0); env.setdefault('BT', BOTTOM0); env.setdefault('OX', 0); env.setdefault('OK', 0)
    glow = {}
    for h, _, _ in CS_LISTS:
        hk = _kids(swf, kids[h][0])
        if 'bgGlow_mc' in hk: glow[h] = hk['bgGlow_mc'][4]
    spec = cs_spec(kids, glow); res = spec.run(env); rest = bake(swf, res)
    swf.head = rect_bytes(STAGE_WMAX, STAGE_H) + swf.head[-4:]
    code = Code(swf)
    window_doc(code, spec, [(CS_CLIP, 'dragHit_mc')], [INV_BTN, CS_TOGGLE2], 3, 'setHelmetOptionState', ctx=((CS_CLIP, 'equip_mc'), 'equipment_41'),
               extra=lambda d: list_scroll(d, [(CS_CLIP, h, 'list') for h, _, _ in CS_LISTS]) + list_flow(d, [(CS_CLIP, h) for h, _, _ in CS_LISTS]) + stat_strip(d))
    # The doll turns under the finger by the PC class's own means, nothing is changed here: equipment_41.onDown sends dollDown (handled,
    # main+0x12a59c8: byte [object+0x3f8] = 1 + the mouse capture); the class's input handler main+0x12af270 turns it on the events 0x6a /
    # 0x6b (FlashMouseMoveLeft / Right: [object+0x3fc] = the turn) and STOPS it on event 1 (FlashLeftMouse, the button's release:
    # [+0x3f8] = 0, capture off). dollUp / dollOut of the movie are not in main.raw and are not needed: the engine stops on the release itself.
    made = code.done()
    return swf.write(), dict(placed=[INV_BTN, CS_TOGGLE2], defs=ndef + nbtn, methods=made, env=env, rest=rest, window=(env['WX'], env['WY'], env['WX'] + env['WW'], env['WY'] + env['WH']),
                             bitmap=bid, shape=new, moved=moved, stage=(STAGE_WMAX, STAGE_H), spec=spec)


SCROLL_SLOP = 8      # list-local px of finger travel before a press on a list becomes a scroll (tools/dos2_ppanel_skills.py SCROLL_SLOP)
def list_scroll(d, paths):
    """one-finger scroll of scroll lists (tools/dos2_ppanel_skills.py doc_scroll, the same code on this tool's Cls): dmScDown(e) on the list's
    mouseDown remembers the list, the finger's list y and the bar's scrolledY; dmScMove follows the finger on ENTER_FRAME (stage MOUSE_MOVE is
    not reliable on the touch path) and, past SCROLL_SLOP list px, scrolls with the bar's public scrollTo(y, false, false) (it clamps) and takes
    the mouse from the rows (mouseChildren off: the release does not click the row under the finger); dmScUp (stage mouseUp) ends it.
    -> the dmHook lines that hang dmScDown on each list"""
    P, S_ = d.P, d.S
    for n, v in (('dmScY', 0), ('dmScS', 0)): add_slot(d, n, v)
    d.inst['traits'].append(Trait(P('dmScL'), 0, 0, 0)); d.inst['traits'].append(Trait(P('dmScOn'), 0, 0, 0))
    stage = [('getlocal0',), ('getproperty', P('stage'))]
    c = HEAD + [('getlocal0',), ('getlocal1',), ('getproperty', P('currentTarget')), ('setproperty', P('dmScL')),
        ('getlocal0',), ('getlocal1',), ('getproperty', P('currentTarget')), ('getproperty', P('mouseY')), ('setproperty', P('dmScY')),
        ('getlocal0',), ('getlocal1',), ('getproperty', P('currentTarget')), ('getproperty', P('m_scrollbar_mc')), ('getproperty', P('scrolledY')), ('setproperty', P('dmScS')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmScOn'))]
    c += [('getlocal0',), ('pushstring', S_('enterFrame')), ('getlocal0',), ('getproperty', P('dmScMove')), ('callpropvoid', P('addEventListener'), 2)]
    c += stage + [('pushstring', S_('mouseUp')), ('getlocal0',), ('getproperty', P('dmScUp')), ('callpropvoid', P('addEventListener'), 2)]
    d.method('dmScDown', 1, c + [('returnvoid',)])
    c = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushbyte', 0), ('convert_d',), ('setlocal', 3), ('pushbyte', 0), ('convert_d',), ('setlocal', 4),
        ('getlocal0',), ('getproperty', P('dmScL')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('getproperty', P('mouseY')), ('getlocal0',), ('getproperty', P('dmScY')), ('subtract',), ('convert_d',), ('setlocal', 3),
        ('getlocal0',), ('getproperty', P('dmScOn')), ('iftrue', 'go'),
        ('getlex', d.abc.pub('Math')), ('getlocal', 3), ('callproperty', P('abs'), 1)] + num(SCROLL_SLOP) + [('ifngt', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmScOn')), ('getlocal', 2), ('pushfalse',), ('setproperty', P('mouseChildren')),
        ('L', 'go'),
        ('getlocal0',), ('getproperty', P('dmScS')), ('getlocal', 3), ('subtract',), ('convert_d',), ('setlocal', 4),
        ('getlocal', 2), ('getproperty', P('m_scrollbar_mc')), ('getlocal', 4), ('pushfalse',), ('pushfalse',), ('callpropvoid', P('scrollTo'), 3),
        ('L', 'end'), ('returnvoid',)]
    d.method('dmScMove', 1, c)
    c = HEAD + [('getlocal0',), ('pushstring', S_('enterFrame')), ('getlocal0',), ('getproperty', P('dmScMove')), ('callpropvoid', P('removeEventListener'), 2)]
    c += stage + [('pushstring', S_('mouseUp')), ('getlocal0',), ('getproperty', P('dmScUp')), ('callpropvoid', P('removeEventListener'), 2)]
    c += [('getlocal0',), ('getproperty', P('dmScL')), ('iffalse', 'end'), ('getlocal0',), ('getproperty', P('dmScL')), ('pushtrue',), ('setproperty', P('mouseChildren')),
          ('getlocal0',), ('pushnull',), ('setproperty', P('dmScL')), ('L', 'end'), ('returnvoid',)]
    d.method('dmScUp', 1, c)
    out = []
    for k, path in enumerate(paths):
        out += [('getlocal0',), ('coerce_a',), ('setlocal', 1)]
        for p in path: out += [('getlocal', 1), ('getproperty', P(p)), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'sc%d' % k)]
        out += [('getlocal', 1), ('pushstring', S_('mouseDown')), ('getlocal0',), ('getproperty', P('dmScDown')), ('callpropvoid', P('addEventListener'), 2), ('L', 'sc%d' % k)]
    return out


def list_flow(d, holders):
    """two columns in each tab list (owner 23.11, iPad: abilities, talents, tags in two columns of the one panel): dmFlow(holder) - when the
    holder is shown and its list's last element is still in the first column (the PC's positionElements puts every element at x SIDE_SPACING,
    one under the other), the elements from the first one whose middle is past the half of the column move to x + COL2 and up by that
    element's y less the first's: two columns, the second starting at the first's top; the bar's content height = the taller column (until
    the PC re-positions: then the next frame flows again). dmFlowTick (ENTER_FRAME) does it for every holder. Elements: the list's public
    length / getAt (listDisplay; the grouped ability lists' elements are the groups). -> the dmHook lines that start the tick"""
    P = d.P
    L, N, EL, Y0, TOT, HALF, I, DY, J, C1 = range(2, 12)
    def at(i): return [('getlocal', L), ('getlocal', i), ('callproperty', P('getAt'), 1), ('coerce_a',), ('setlocal', EL)]
    c = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', L), ('pushnull',), ('coerce_a',), ('setlocal', EL)] + \
        [x for v in (N, Y0, TOT, HALF, I, DY, J, C1) for x in (('pushbyte', 0), ('convert_d',), ('setlocal', v))] + [
        ('getlocal1',), ('iffalse', 'end'), ('getlocal1',), ('getproperty', P('visible')), ('iffalse', 'end'),
        ('getlocal1',), ('getproperty', P('list')), ('coerce_a',), ('setlocal', L), ('getlocal', L), ('iffalse', 'end'),
        ('getlocal', L), ('getproperty', P('length')), ('convert_d',), ('setlocal', N), ('getlocal', N), ('pushbyte', 2), ('iflt', 'end'),
        ('getlocal', N), ('pushbyte', 1), ('subtract',), ('convert_d',), ('setlocal', J)] + at(J) + [('getlocal', EL), ('iffalse', 'end'),
        ('getlocal', EL), ('getproperty', P('x'))] + num(COL2 // 2) + [('ifge', 'end'),          # already in two columns
        ('getlocal', EL), ('getproperty', P('y')), ('getlocal', EL), ('getproperty', P('height')), ('add',), ('convert_d',), ('setlocal', TOT),
        ('pushbyte', 0), ('convert_d',), ('setlocal', J)] + at(J) + [('getlocal', EL), ('iffalse', 'end'),
        ('getlocal', EL), ('getproperty', P('y')), ('convert_d',), ('setlocal', Y0),
        ('getlocal', Y0), ('getlocal', TOT), ('getlocal', Y0), ('subtract',), ('pushbyte', 2), ('divide',), ('add',), ('convert_d',), ('setlocal', HALF),
        ('pushbyte', 1), ('convert_d',), ('setlocal', I),
        ('L', 'find'), ('getlocal', I), ('getlocal', N), ('ifge', 'end')] + at(I) + [('getlocal', EL), ('iffalse', 'next'),
        ('getlocal', EL), ('getproperty', P('y')), ('getlocal', EL), ('getproperty', P('height')), ('pushbyte', 2), ('divide',), ('add',),
        ('getlocal', HALF), ('ifgt', 'found'),
        ('L', 'next'), ('getlocal', I), ('pushbyte', 1), ('add',), ('convert_d',), ('setlocal', I), ('jump', 'find'),
        ('L', 'found'), ('getlocal', EL), ('getproperty', P('y')), ('getlocal', Y0), ('subtract',), ('convert_d',), ('setlocal', DY),
        ('getlocal', EL), ('getproperty', P('y')), ('convert_d',), ('setlocal', C1),                        # the first column's bottom, about
        ('getlocal', I), ('convert_d',), ('setlocal', J),
        ('L', 'move'), ('getlocal', J), ('getlocal', N), ('ifge', 'moved')] + at(J) + [('getlocal', EL), ('iffalse', 'skip'),
        ('getlocal', EL), ('getlocal', EL), ('getproperty', P('x'))] + num(COL2) + [('add',), ('setproperty', P('x')),
        ('getlocal', EL), ('getlocal', EL), ('getproperty', P('y')), ('getlocal', DY), ('subtract',), ('setproperty', P('y')),
        ('L', 'skip'), ('getlocal', J), ('pushbyte', 1), ('add',), ('convert_d',), ('setlocal', J), ('jump', 'move'),
        ('L', 'moved'), ('getlocal', TOT), ('getlocal', DY), ('subtract',), ('convert_d',), ('setlocal', TOT),
        ('getlocal', C1), ('getlocal', TOT), ('ifngt', 'c2'), ('getlocal', C1), ('convert_d',), ('setlocal', TOT), ('L', 'c2'),
        ('getlocal', L), ('getproperty', P('m_scrollbar_mc')), ('coerce_a',), ('setlocal', EL), ('getlocal', EL), ('iffalse', 'end'),
        ('getlocal', EL), ('getlocal', TOT), ('setproperty', P('m_contentHeight')),
        ('getlocal', EL), ('callpropvoid', P('scrollbarVisible'), 0),
        ('L', 'end'), ('returnvoid',)]
    d.method('dmFlow', 1, c)
    c = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2)]
    for k, path in enumerate(holders):
        c += [('getlocal0',), ('coerce_a',), ('setlocal', 2)]
        for q in path: c += [('getlocal', 2), ('getproperty', P(q)), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'f%d' % k)]
        c += [('getlocal0',), ('getlocal', 2), ('callpropvoid', P('dmFlow'), 1), ('L', 'f%d' % k)]
    d.method('dmFlowTick', 1, c + [('returnvoid',)])
    return [('getlocal0',), ('pushstring', d.S('enterFrame')), ('getlocal0',), ('getproperty', P('dmFlowTick')), ('callpropvoid', P('addEventListener'), 2)]


def stat_strip(d):
    """the inventory window's stat strip (owner 23.11, iPad: armour, damage, crit, accuracy, dodge, movement, initiative, resistances in a strip
    over the bag): in state 1 dmStripTick (ENTER_FRAME) lays the sheet's own lists out as cells of value + icon - stats_mc.secondaryStatList
    (row 1) and resistanceStatList (row 2, its holder one row lower: dmLay) - every element cropped by scrollRect to its value and icon (the label
    stays in it, unseen) and put at x = the cells before it; the block is centred on the strip (dmStX, dmStW: dmLay). expStatList is hidden
    there. In state 0 the crops go and the PC's positionElements puts the lists back (once: the last element's crop tells). The data is the
    PC's: whatever addSecondaryStat sent (type 1 / 2), so the strip shows the stats the Switch sends, nothing invented.
    -> the dmHook lines that start the tick"""
    P = d.P; Rc = d.abc.pub('Rectangle', 'flash.geom')
    for n in ('dmStX', 'dmStW'): add_slot(d, n, 0)
    N, I, EL, TM, TT, X0, W, CUR = range(3, 11)
    def at(i): return [('getlocal1',), ('getlocal', i), ('callproperty', P('getAt'), 1), ('coerce_a',), ('setlocal', EL)]
    c = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', EL), ('pushnull',), ('coerce_a',), ('setlocal', TM), ('pushnull',), ('coerce_a',), ('setlocal', TT)] + \
        [x for v in (N, I, X0, W, CUR) for x in (('pushbyte', 0), ('convert_d',), ('setlocal', v))] + [
        ('getlocal1',), ('iffalse', 'ret'), ('getlocal1',), ('getproperty', P('length')), ('convert_d',), ('setlocal', N),
        ('getlocal', N), ('pushbyte', 0), ('ifngt', 'ret'),
        ('getlocal2',), ('iftrue', 'on'),
        ('getlocal', N), ('pushbyte', 1), ('subtract',), ('convert_d',), ('setlocal', I)] + at(I) + [('getlocal', EL), ('iffalse', 'ret'),
        ('getlocal', EL), ('getproperty', P('scrollRect')), ('iffalse', 'ret'),
        ('pushbyte', 0), ('convert_d',), ('setlocal', I),
        ('L', 'off'), ('getlocal', I), ('getlocal', N), ('ifge', 'offd')] + at(I) + [('getlocal', EL), ('iffalse', 'offn'),
        ('getlocal', EL), ('pushnull',), ('setproperty', P('scrollRect')),
        ('L', 'offn'), ('getlocal', I), ('pushbyte', 1), ('add',), ('convert_d',), ('setlocal', I), ('jump', 'off'),
        ('L', 'offd'), ('getlocal1',), ('callpropvoid', P('positionElements'), 0),
        ('L', 'ret'), ('pushbyte', 0), ('returnvalue',),
        ('L', 'on'), ('pushbyte', 0), ('convert_d',), ('setlocal', I),
        ('L', 'loop'), ('getlocal', I), ('getlocal', N), ('ifge', 'done')] + at(I) + [('getlocal', EL), ('iffalse', 'next'),
        ('getlocal', EL), ('getproperty', P('texts_mc')), ('coerce_a',), ('setlocal', TM), ('getlocal', TM), ('iffalse', 'next'),
        ('getlocal', TM), ('getproperty', P('text_txt')), ('coerce_a',), ('setlocal', TT), ('getlocal', TT), ('iffalse', 'next'),
        ('getlocal', TM), ('getproperty', P('x')), ('getlocal', TT), ('getproperty', P('x')), ('add',), ('pushbyte', 4), ('subtract',), ('convert_d',), ('setlocal', X0),
        ('getlocal', EL), ('getproperty', P('icon_mc')), ('getproperty', P('visible')), ('iffalse', 'noic'),
        ('getlocal', EL), ('getproperty', P('icon_mc')), ('getproperty', P('x')), ('pushbyte', 32), ('add',), ('getlocal', X0), ('subtract',), ('convert_d',), ('setlocal', W),
        ('jump', 'cell'),
        ('L', 'noic'), ('getlocal', TT), ('getproperty', P('width')), ('pushbyte', 8), ('add',), ('convert_d',), ('setlocal', W),
        ('L', 'cell'), ('getlocal', EL), ('getproperty', P('scrollRect')), ('iftrue', 'put'),
        ('getlocal', EL), ('findpropstrict', Rc), ('getlocal', X0), ('pushbyte', 3), ('negate',), ('getlocal', W), ('pushbyte', 32), ('constructprop', Rc, 4),
        ('setproperty', P('scrollRect')),
        ('L', 'put'), ('getlocal', EL), ('getlocal', CUR), ('setproperty', P('x')), ('getlocal', EL), ('pushbyte', 0), ('setproperty', P('y')),
        ('getlocal', CUR), ('getlocal', W), ('add',), ('pushbyte', 12), ('add',), ('convert_d',), ('setlocal', CUR),
        ('L', 'next'), ('getlocal', I), ('pushbyte', 1), ('add',), ('convert_d',), ('setlocal', I), ('jump', 'loop'),
        ('L', 'done'), ('getlocal', CUR), ('returnvalue',)]
    d.method('dmStripRow', 2, c)
    SM, A, B, MS = 2, 3, 4, 5
    c = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', SM), ('pushbyte', 0), ('convert_d',), ('setlocal', A), ('pushbyte', 0), ('convert_d',), ('setlocal', B),
        ('pushnull',), ('coerce_a',), ('setlocal', MS),
        ('getlocal0',), ('getproperty', P(CS_CLIP)), ('coerce_a',), ('setlocal', SM), ('getlocal', SM), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', SM), ('getproperty', P('secondaryStatList')), ('getlocal0',), ('getproperty', P('dmState')), ('callproperty', P('dmStripRow'), 2), ('convert_d',), ('setlocal', A),
        ('getlocal0',), ('getlocal', SM), ('getproperty', P('resistanceStatList')), ('getlocal0',), ('getproperty', P('dmState')), ('callproperty', P('dmStripRow'), 2), ('convert_d',), ('setlocal', B),
        ('getlocal', SM), ('getproperty', P('expStatList')), ('coerce_a',), ('setlocal', MS), ('getlocal', MS), ('iffalse', 'nx'),
        ('getlocal', MS), ('getlocal0',), ('getproperty', P('dmState')), ('not',), ('setproperty', P('visible')),
        ('L', 'nx'), ('getlocal0',), ('getproperty', P('dmState')), ('iffalse', 'end'),
        ('getlocal', SM), ('getproperty', P('mainStats_mc')), ('coerce_a',), ('setlocal', MS), ('getlocal', MS), ('iffalse', 'end'),
        ('getlocal', A), ('getlocal', B), ('ifnlt', 'wa'), ('getlocal', B), ('convert_d',), ('setlocal', A), ('L', 'wa'),
        ('getlocal', MS), ('getlocal0',), ('getproperty', P('dmStX')), ('getlocal0',), ('getproperty', P('dmStW')), ('getlocal', A), ('getlocal', MS), ('getproperty', P('scaleX')),
        ('multiply',), ('subtract',), ('pushbyte', 2), ('divide',), ('add',), ('setproperty', P('x')),
        ('L', 'end'), ('returnvoid',)]
    d.method('dmStripTick', 1, c)
    return [('getlocal0',), ('pushstring', d.S('enterFrame')), ('getlocal0',), ('getproperty', P('dmStripTick')), ('callpropvoid', P('addEventListener'), 2)]


MAKE = {'partyInventory_p.swf': party, 'characterSheet_p.swf': sheet}
SRC = {'partyInventory_p.swf': 'partyInventory.swf', 'characterSheet_p.swf': 'characterSheet.swf'}
IPAD = {'partyInventory_p.swf': ['partyInventory_t.swf'], 'characterSheet_p.swf': ['characterSheet_t.swf', 'equipmentPanel_t.swf']}


# ---------------------------------------------------------------- checks
CELLS = (76, 96, 110, 130)                                   # cell px asked by the host: the iPad's, and what a phone needs for the same finger size
SCREENS = ((1440, '4:3'), (1920, '16:9'), (2160, '18:9'), (2400, '20:9'), (2520, '21:9'))
DYN = {'list', 'm_scrollbar_mc'}                             # members the PC code makes at run time
def _ei(abc):
    """{ExternalInterface name: sorted arg counts} of an ABC block"""
    import dos2_ui_audit as au
    out = {}
    for who, rec in au.scan(abc).items():
        for name, argc in rec['ei']: out.setdefault(name, set()).add(argc)
    return out


def fits(geom, width_own, state=0):
    """[(C, DW, env, ok)]: the window (and in the equipment view the pair) inside the screen with the side margins, not taller than the screen"""
    out = []
    for C in CELLS:
        for DW, _ in SCREENS:
            e = geom(C, state, DW)
            if 'WW' in e: ok = e['WW'] <= DW - 2 * MARGIN - 2 * COL_RES + 1 and e['WY'] + e['WH'] <= e['WB'] + 0.01 and e['WX'] >= MARGIN + COL_RES - 1
            else: hs = e.get('HS', e['H'] * e['S']); ok = e['MW'] <= DW - 2 * MARGIN + 1 and e['Y0'] + hs <= STAGE_H + 0.01 and e['Y0'] >= -0.01
            out.append((C, DW, e, ok))
    return out


def check(o, data, out, rep, say):
    """the checks of one movie (called from tools/dos2_pchud_touch.py check): data = the PC movie, out = the `_p` movie, rep = the build report"""
    a = Swf(data); b = Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs()
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies (PC: %d, +%d)' % (len(B.bodies), len(A.bodies), len(B.bodies) - len(A.bodies)))
    say(Swf(Swf(out).write()).body() == b.body(), 'the movie parses and round-trips')
    g = [m for m in gr.check(data, out, quiet=True) if not m.startswith(('header / tail differ', 'original tag #'))]
    ida = {}; changed = []; new = []
    for code, d, _ in a.tags:
        if code in gr.DEFS: ida[struct.unpack_from('<H', d, 0)[0]] = (code, d)
    for code, d, _ in b.tags:
        if code not in gr.DEFS: continue
        cid = struct.unpack_from('<H', d, 0)[0]
        if cid not in ida: new.append((cid, code))
        elif ida[cid] != (code, d): changed.append(cid)
    mv = gr.Movie(a); nm = lambda c: mv.classes.get(c, '#%d' % c).split('.')[-1]
    want = {'partyInventory_p.swf': {'partyInventory_1', 'bgFrame_3', 'playerInventory'},
            'characterSheet_p.swf': {'stats_1', 'bg_22', 'plusButton_62', 'minusButton_65', 'equipment_41', '#456', 'CloseButton_58',
                                     'abilitiesholder_9', 'tagsholder_13', 'talentsHolder_11', 'visualsHolder_18'}}[o]   # the holders: their glow's height (21.11)
    say(not g and {nm(c) for c in changed} == want and len(new) == rep['defs'],
        'tags: ids unique, uses after definitions; of %d PC definitions %d rebuilt (%s), +%d new (background pieces, the doll button); the rest byte for byte%s' % (
        len(ida), len(changed), ', '.join(sorted(nm(c) for c in changed)), len(new), '' if not g else ': ' + str(g)))
    bitmaps = lambda s: sorted(hashlib.sha256(d[2:]).hexdigest() for c, d, _ in s.tags if c in gr.T_BITMAPS)
    ba, bb = bitmaps(a), bitmaps(b); extra = [h for h in bb if h not in ba]
    nx = 3 if o == 'characterSheet_p.swf' else 0   # the plate + two glyphs
    say(all(h in bb for h in ba) and len(extra) == nx, 'art: the PC movie\'s %d bitmaps + %d of our own drawings (%s); no iPad file is read' % (len(ba), len(extra),
        'assets/touchui plate_square_copper + ico_inventory / ico_charsheet: the window button' if nx else 'none: the window button is the sheet movie\'s'))
    say(b.head[:-4] == rect_bytes(*rep['stage']), 'stage %d x %d: wider than any screen; dmLay sets the movie to the screen width (setMcSize) and centres the window inside it (phone 21.11: a window shifted inside a window-wide movie was cut at the movie rect)' % rep['stage'])
    ea, eb = _ei(A), _ei(B); added = sorted(set(eb) - set(ea))
    say(set(ea) <= set(eb) and set(added) <= {'dmSide', 'setMcSize'}, 'ExternalInterface: the PC movie\'s %d names kept; added: %s (dmSide = the host\'s; setMcSize = the engine\'s own, as the PC inventory calls it)' % (len(ea), ' '.join(added)))
    def doc(X):
        ci = [i for i, c in enumerate(X.instances) if X.mname(c['name']).endswith('.MainTimeline')][0]
        return {X.mname(t.name).split('::')[-1].split('.')[-1]: (len(X.methods[t.b].params) if t.kind & 15 in (1, 2, 3) else -1) for t in X.instances[ci]['traits']}
    da, db = doc(A), doc(B)
    say(all(k in db and db[k] == v for k, v in da.items()) and all(k in db for k in ('dmLay', 'dmSetCell', 'dmSetState', 'dmCell', 'dmState', 'dmW')),
        'document class: all %d PC traits kept with their parameter counts (+%d: %s)' % (len(da), len(db) - len(da), ' '.join(sorted(set(db) - set(da)))))
    ib = {v: k for k, v in B.owners().items()}
    def dis(cls, m):
        k = [w for w in ib if w.endswith(cls + '::' + m)]; assert len(k) == 1, (cls, m, k)
        return '\n'.join(B.dis(B.methods[ib[k[0]]].body))
    t = dis('MainTimeline', 'onEventInit'); r = dis('MainTimeline', 'onEventResolution'); cmd = {'partyInventory_p.swf': 'setAnchor', 'characterSheet_p.swf': 'setHelmetOptionState'}[o]
    c = dis('MainTimeline', cmd); l = dis('MainTimeline', 'dmLay')
    say('dmInitOrig' in t and 'dmHook' in t and 'dmLay' in t and 'dmResOrig' in r and 'dmLay' in r and 'dmW' in r, 'onEventInit = original + dmHook + dmLay; onEventResolution(w, h) = original + dmW (w * 1080 / h) + dmLay')
    say('dmSetCell' in c and 'dmSetState' in c and 'dmAnchorOrig' in c and str(CMD) in c, '%s(%d + n) = dmSetState(n < 40) / dmSetCell(n): the host\'s way in through a name the engine class registers; below %d the original' % (cmd, CMD, CMD))
    say("'topleft'" in l and "'center'" not in l and 'setMcSize' in l and ('scrollbarVisible' in l or 'setLength' in l), 'dmLay: computes the layout from dmCell / dmState / dmW, then setMcSize + setPosition(topleft, screen, topleft): origin 0, the window centres itself (phone 23.11)')
    # every path of the layout exists on the timeline (or is a member the PC code makes)
    bad = []
    for path in rep['spec'].paths():
        if any(not isinstance(s, str) and s[0] == 'dyn' and s[1] not in DYN for s in path): bad.append(path); continue
        try: _resolve(b, path)
        except AssertionError as e: bad.append(str(e))
    say(not bad, 'dmLay: all %d object paths are placed instances of the built movie%s' % (len(list(rep['spec'].paths())), '' if not bad else ': ' + str(bad)))
    w = _walk(out); x0, y0, x1, y1 = rep['window']; env = rep['env']; S = env['S']
    if o == 'partyInventory_p.swf':
        e = env; X, Y = e['WX'], e['WY']
        def box(n): return w['inventory_mc.' + n]['stage']
        rx0, ft, rx1, pz = X + e['RX'], Y + e['FT'], X + e['RX'] + e['RW'], Y + e['PZ']
        hb = box('hitArea_mc'); tb = box('tabs_mc') if box('tabs_mc') else (rx0, ft)
        say(e['ST'] == 1 and hb[0] < rx0 and hb[2] > rx1 and abs(hb[1] - ft) < 1.5 and not rep['placed'] and box('title_txt')[0] < -1000 and box('close_mc')[0] < -1000,
            'baked (cell %d, %d px screen, the inventory window = state 1): this movie draws only the open right column of the shared window: %.0f,%.0f..%.0f,%.0f '
            '(the filter row + the bag); the frame, title bar, HP box, doll and stat strip are the sheet movie\'s (its state 1), no doll button here' % (e['C'], e['DW'], rx0, ft, rx1, pz))
        bad = []
        for C in CELLS:
            for DW, _ in SCREENS:
                a_, b_ = inv_geom(C, 1, DW), cs_geom(C, 1, DW)
                if any(abs(a_[n] - b_[n]) > 0.001 for n in ('WX', 'WY', 'WW', 'WH', 'RX', 'RW', 'FT', 'FH', 'PY', 'PZ', 'LX', 'LW')): bad.append((C, DW))
        say(not bad, 'one grid: both movies compute the same window, columns, row A split and bag rect (win_lets) for every cell / screen%s' % ('' if not bad else ': NOT ' + str(bad)))
        lo = w['inventory_mc.listHolder_mc']['origin']
        say(abs(lo[0] - (X + e['BX'])) < 1 and abs((lo[0] - rx0) - (rx1 - lo[0] - INV_LIST_W * e['SQ'])) < 1.5 and e['SQ'] <= e['C'] / 50 * INV_K + 1e-9,
            'the bag: cell 50 -> %.0f px (the cell asked x INV_K %.2f, smaller only when the column is narrow), pitch %.1f, %d columns (iPad 8; the engine reads invColW / cSpacing '
            'from the movie once: main+0x13aa5b0) x %d rows in view, centred in the right column; the list scrolls' % (50 * e['SQ'], INV_K, INV_PITCH * e['SQ'], INV_COLS, e['ROWS']))
        items = [('tabs_mc', INV_TABS_W), ('allBtn_mc', 80), ('view_mc', 268), ('autosortBtn_mc', 140), ('sortByBtn_mc', 140), ('totalGold_mc', 138)]
        xs = [(w['inventory_mc.' + n]['origin'][0], w['inventory_mc.' + n]['origin'][0] + wd * e['SB'], w['inventory_mc.' + n]['origin'][1]) for n, wd in items]
        say(all(xs[i][1] <= xs[i + 1][0] + 0.5 for i in range(len(xs) - 1)) and xs[0][0] >= rx0 and xs[-1][1] <= rx1 + 0.5 and all(ft <= q[2] <= ft + e['FH'] for q in xs),
            'the filter row %.0f px under the stat strip (the PC header\'s ornate bar, its plain spans stretched): party tabs, All, party view | sort, sort by, gold - in one line x%.2f, none over another' % (
            e['FH'], e['SB']))
        h = dis('MainTimeline', 'dmHook'); padh = min(p_[1] for p_ in HEAD_PADS) * e['SB']
        say(h.count('drawRect') == len(HEAD_PADS) and all(p_[0][-1] in h for p_ in HEAD_PADS),
            'buttons: invisible finger pads %d own = %.0f px tall on sort / sort by / All / party view (dmHook); a party tab 41 -> %.0f px' % (HEAD_PADS[0][1], padh, 41 * e['SB']))
        sp = box('sortBy_mc'); hb = box('hitArea_mc')
        say(sp[0] >= rx0 - 1 and sp[2] <= rx1 + 1 and sp[1] >= Y + e['PY'] - 1 and sp[3] <= pz + 1 and abs(hb[1] - ft) < 1.5 and abs(hb[3] - (pz + GM)) < 2 and hb[0] < rx0 and hb[2] > rx1,
            'the sort popup inside the bag; the window\'s hit area = the filter row + the bag (the sheet movie eats the rest: no overlap of the two movies\' parts)')
        t = dis('sortPopup_21', 'okPressed'); say('dmOkOrig' in t and "'autosort'" in t, 'the sort popup: OK = original (setSortFilter; applySortFilters is not in main.raw) + autosort (handled: the sort button\'s call)')
        t = dis('partyInventory_1', 'onContextMenuInputUp'); u = dis('MainTimeline', 'dmCtxUp'); h = dis('MainTimeline', 'dmHook')
        say('dmCtxOrig' in t and 'getTimer' in t and 'onContextMenuInputUp' in u and "'rightMouseUp'" in h, 'long press: right mouse up on the window -> onContextMenuInputUp (openContextMenu, handled by the class: main+0x13a7690), at most once in %d ms with the engine\'s own event' % CTX_GUARD_MS)
        t = dis('partyInventory_1', 'onCellCreate'); say('dmCellOrig' in t and 'tooltipOverrideW' in t and 'scaleX' in t, 'onCellCreate = original + the tooltip rectangle = the cell on the screen (50 x the window scale)')
        geom, wown = inv_geom, INV_LIST_W
    else:
        e = env; X, Y = e['WX'], e['WY']
        def box(n): return w['stats_mc.' + n]['stage']
        def near(u, v, tol=1.6): return all(abs(p - q) < tol for p, q in zip(u, v))
        hit = box('hitArea_mc')
        say(near(w['stats_mc']['origin'], (X, Y), 0.6) and near(hit, (X, Y, X + e['WW'], Y + e['WH']), 2.5) and Y + e['WH'] <= e['WB'] + 0.01,
            'baked (cell %d, %d px screen, hot bar top %d): ONE window %.0f x %.0f at %.0f,%.0f (iPad characterSheet_t 1168 x 1068), above the hot bar; the clip at scale 1, every part placed in window px' % (
            e['C'], e['DW'], e['BT'], e['WW'], e['WH'], X, Y))
        off = [n for n in CS_OFF if not (w['stats_mc.' + n]['origin'][1] < -1000)]
        say(not off, 'the PC portrait strip\'s content and the drag bar are off the window: %s' % ' '.join(CS_OFF))
        # the grid: the doll column, the right column, row A
        eq = box('equip_mc.container_mc'); dl = (X + e['LX'], Y + e['PY'], X + e['LX'] + e['LW'], Y + e['PY'] + e['PH'])
        say(eq[0] >= dl[0] - 1 and eq[2] <= dl[2] + 1 and eq[1] >= dl[1] - 1 and eq[3] <= dl[3] + 1 and abs(e['RX'] - e['LX'] - e['LW'] - GG) < 0.01 and
            abs(e['WW'] - (e['RX'] + e['RW'] + GM + e['SR'])) < 1.01,
            'grid: margin %d, gutter %d px; the doll column %.0f,%.0f %.0f x %.0f (doll x%.2f: a slot 64 -> %.0f px; iPad 422 x 678, slot 82), the right column x %.0f, %.0f wide' % (
            GM, GG, dl[0], dl[1], e['LW'], e['PH'], e['D'], 64 * e['D'], X + e['RX'], e['RW']))
        tb = w['stats_mc.tabsHolder_mc']['origin']; ci = w['stats_mc.charInfo_mc']['origin']; cl = box('close_mc.bg_mc'); tg = w[INV_BTN]['stage']
        say(abs(tb[0] + CS_TABS_W / 2 * e['F'] - (X + e['RX'] + e['RW'] / 2)) < 1 and abs(ci[0] - (X + e['WW'] / 2)) < 1 and abs(cl[2] - cl[0] - CS_CLOSE) < 1 and
            cl[2] <= X + e['WW'] and tg[0] >= X and tg[1] >= Y,
            'title bar: name - level centred (charInfo_mc at %.0f), close %.0f px at the right tile (hit x2: %.0f px), the sheet <-> inventory button %.0f px on the left tile; '
            'the 5 tabs x%.2f (a tab %.0f x %.0f px) centred over the panel, the tab title on the plaque under them' % (
            ci[0], cl[2] - cl[0], 2 * (cl[2] - cl[0]), tg[2] - tg[0], e['F'], 76 * e['F'], 56 * e['F']))
        ih = w['stats_mc.equip_mc.infoStatHolder_mc']['origin']
        say(X + e['LX'] < ih[0] < X + e['LX'] + 30 * e['F'] and Y + e['CT'] < ih[1] < Y + e['CT'] + e['IH'] and e['SI'] * INFO_W <= e['LW'] - 16 * e['F'] + 0.01,
            'HP / AP / Source (the PC\'s infoStatList, type 0 of addSecondaryStat; equip_mc.infoStatHolder_mc) in the box of row A over the doll: rows of %.0f px, x%.2f (iPad: 3 rows of 36 px top left)' % (
            26 * e['SI'], e['SI']))
        pb = box('panelBg1_mc'); ms = w['stats_mc.mainStats_mc']['origin']
        say(near(pb, (X + e['RX'] + 4 * e['F'], Y + e['PY'] + 4 * e['F'], X + e['RX'] + e['RW'] - 4 * e['F'], Y + e['PY'] + e['PH'] - 4 * e['F']), 2.5) and 'stats_mc.dmPanel2' not in w and
            abs(ms[0] - (X + e['PCX'] + 20 * e['S'])) < 1,
            'ONE parchment panel %.0f x %.0f under one frame (9 pieces: plain edges stretched, corners whole; no second page, no fold); content x%.2f: a stat row 26 -> %.0f px (iPad 46), '
            'two columns %d own apart' % (e['RW'], e['PH'], e['S'], 26 * e['S'], COL2))
        hits = []
        for cls in ('plusButton_62', 'minusButton_65'):
            cid = [q for q, n in gr.Movie(b).classes.items() if n.endswith('.' + cls)][0]
            st = gr.frame_state(b.tags[_def_index(b, cid)][1], 1); hm = [_mat(r) for r in st.values() if r['name'] == b'hit_mc\0'][0]
            hits.append((hm[0], hm[1], 18 * hm[2], 18 * hm[3]))
        say(all(abs(u - v) < 0.01 for hh in hits for u, v in zip(hh, CS_PM_HIT)), '+ / - buttons: hit area %d x %d own = %.0f x %.0f px (PC 18 x 18; drawing 20 -> %.0f px; iPad 34)' % (
            CS_PM_HIT[2], CS_PM_HIT[3], CS_PM_HIT[2] * e['S'], CS_PM_HIT[3] * e['S'], 20 * e['S']))
        t = dis('equipment_41', 'onDown'); say("'dollDown'" in t, 'the doll turns under the finger: onDown is the PC body (dollDown, handled: main+0x12a59c8); the class stops it on the left button\'s release itself (main+0x12af270, event 1 FlashLeftMouse)')
        t = dis('equipment_41', 'onContextMenuInputUp'); u = dis('MainTimeline', 'dmCtxUp'); h = dis('MainTimeline', 'dmHook')
        say('dmCtxOrig' in t and 'getTimer' in t and 'onContextMenuInputUp' in u and "'rightMouseUp'" in h, 'long press on a doll slot: right mouse up -> equip_mc.onContextMenuInputUp (openContextMenu, handled by the class: main+0x12a56e4), at most once in %d ms' % CTX_GUARD_MS)
        t = dis('MainTimeline', 'dmLay'); fl = dis('MainTimeline', 'dmFlow'); sc = dis('MainTimeline', 'dmScMove')
        say(all(hh in t for hh, _, _ in CS_LISTS) and t.count('setFrame') == len(CS_LISTS) and t.count('setLength') == len(CS_LISTS) and 'scrollRect' in t and
            h.count("'mouseDown'") == len(CS_LISTS) and "'enterFrame'" in h and 'getAt' in fl and 'm_contentHeight' in fl and 'scrollTo' in sc and 'getContDiff' not in sc,
            'the %d tab lists: frames two columns wide, the panel\'s height; dmFlow (every frame) puts the second half in the second column and the bar\'s content height to the taller one; '
            'ONE-FINGER scroll on the list itself (dmScDown / dmScMove: the bar\'s public scrollTo, followed on ENTER_FRAME) + the PC bar at the panel\'s right edge' % len(CS_LISTS))
        bad = []
        for C in CELLS:
            for DW, _ in SCREENS:
                a0, a1 = cs_geom(C, 0, DW), cs_geom(C, 1, DW)
                if any(abs(a1[n] - a0[n]) > 1e-9 for n in ('WX', 'WY', 'WW', 'WH', 'LX', 'LW', 'PY', 'PH', 'D', 'SI', 'CT', 'IH')): bad.append((C, DW))
        say(not bad, 'the window, its title bar, the HP box and the doll stand in the SAME place at the same size on the sheet and in the inventory window (state 1) for every cell / screen%s' % (
            '' if not bad else ': NOT ' + str(bad)))
        ph = cs_geom(103, 0, 2400, 949)
        say(26 * ph['S'] >= 43.5 and 64 * ph['D'] >= 64 and 76 * ph['F'] >= 95 and ph['WY'] + ph['WH'] <= 949,
            'phone (cell 103, 2400 x 1080, hot bar top 949): window %.0f x %.0f, stat row %.0f px, info row %.0f px, tab %.0f x %.0f, doll slot %.0f, close pad %.0f, + / - %.0f x %.0f px' % (
            ph['WW'], ph['WH'], 26 * ph['S'], 26 * ph['SI'], 76 * ph['F'], 56 * ph['F'], 64 * ph['D'], 2 * CS_CLOSE, CS_PM_HIT[2] * ph['S'], CS_PM_HIT[3] * ph['S']))
        geom, wown = cs_geom, CS_W
    # the fit: every cell the host may ask, every screen, both states
    for st, what in (((1, 'the inventory window'),) if o == 'partyInventory_p.swf' else ((0, 'the sheet'), (1, 'the inventory window (this movie: frame, doll, strip)'))):
        f = fits(geom, wown, st); bad = [(C, DW) for C, DW, e, ok in f if not ok]
        say(not bad, 'fit, %s: inside the screen with %d px side margins and <= 1080 high for cells %s on %s%s' % (what, MARGIN, ' / '.join(map(str, CELLS)), ' '.join(n for _, n in SCREENS), '' if not bad else ': NOT ' + str(bad)))
        for C in CELLS:
            row = []
            for C2, DW, e, ok in f:
                if C2 != C: continue
                if o == 'partyInventory_p.swf': row.append('%d: cell %.0f, %d x %d, filter row x%.2f' % (DW, 50 * e['SQ'], INV_COLS, e['ROWS'], e['SB']))
                else: row.append('%d: %.0f x %.0f, row %.0f, slot %.0f' % (DW, e['WW'], e['WH'], 26 * e['S'], 64 * e['D']))
            print('       asked %3d -> %s' % (C, '; '.join(row)))


def _walk(data, name=None, depth=4):
    """{named path: dict(stage = box on the stage px | None, origin = the instance's origin on the stage)} of frame 1, `depth` levels deep"""
    swf = Swf(data); mv = gr.Movie(swf); out = {}
    def go(recs, pm, path, level):
        for rec in recs:
            if rec['char'] is None: continue
            x, y, sx, sy = _mat(rec); m = (pm[0] * sx, pm[1] * sy, pm[2] + pm[0] * x, pm[3] + pm[1] * y)   # scale x, scale y, tx, ty
            name = rec['name'][:-1].decode() if rec['name'] else None
            if name:
                bx = gr.bounds(swf, rec['char'], 1, mv); np_ = (path + '.' if path else '') + name
                out[np_] = dict(origin=(m[2], m[3]), stage=(m[2] + m[0] * bx[0], m[3] + m[1] * bx[1], m[2] + m[0] * bx[2], m[3] + m[1] * bx[3]) if bx else None)
                if level < depth and rec['char'] in mv.defs and swf.tags[mv.defs[rec['char']]][0] == gr.T_SPRITE:
                    go(gr.frame_state(swf.tags[mv.defs[rec['char']]][1], 1).values(), m, np_, level + 1)
    go(_recs(swf, None).values(), (1.0, 1.0, 0.0, 0.0), '', 1)
    return out


# ---------------------------------------------------------------- pictures
def picture(src, view, C, DW, grid=True, BT=BOTTOM0, base=None, mock=True):
    """the screen (DW x 1080, a flat backdrop) with the window of a view the way dmLay places it: 'sheet' (characterSheet_p state 0) or 'inv' /
    'equip' (the inventory window: characterSheet_p state 1 + partyInventory_p). The windows are the movies' frame 1; what the engine fills in
    at run time (tabs, rows, icons, text, bag cells) is drawn over them by this tool when mock / grid (mock_*: NOT in the movies)."""
    from PIL import Image, ImageDraw
    import dos2_swf_render as rn
    img = base.copy() if base is not None else Image.new('RGBA', (DW, STAGE_H), (58, 66, 60, 255)); dr = ImageDraw.Draw(img)
    if base is None: dr.rectangle([0, 0, MARGIN - 1, STAGE_H], fill=(46, 52, 48, 255)); dr.rectangle([DW - MARGIN, 0, DW, STAGE_H], fill=(46, 52, 48, 255))   # the side margins (notch)
    st = 0 if view == 'sheet' else 1; note = []
    for o in ('characterSheet_p.swf', 'partyInventory_p.swf'):
        if view == 'sheet' and o != 'characterSheet_p.swf': continue
        dta, rep = MAKE[o](src[SRC[o]], dict(C=C, ST=st, DW=DW, BT=BT)); e = rep['env']
        swf = Swf(dta)
        # what the PC code keeps hidden is not in the picture: the sort popup (until "sort by" is tapped), the game master's tabs of the sheet
        hide = ('sortBy_mc', 'sortByBg_mc') if o == 'partyInventory_p.swf' else ('invTabHolder_mc', 'customStats_mc', 'skillTabHolder_mc', 'panelBg2_mc')
        edit_places(swf, _root_names(swf)[INV_CLIP if o == 'partyInventory_p.swf' else CS_CLIP]['char'], lambda name, rec: False if name in hide else None)
        # both movies span the screen from x 0 (setMcSize(screen width)); only the window is drawn (the sheet's scrollRect = the window)
        x0, y0 = int(round(e['WX'])), int(round(e['WY'])); bx = (x0, y0, x0 + int(e['WW']) + 1, y0 + int(e['WH']) + 1)
        pic, org = rn.render(swf, 'root', 1.0, box=bx); img.alpha_composite(pic, (x0, y0))
        if o == 'characterSheet_p.swf':
            if mock: (mock_strip if st else mock_sheet)(img, swf, e)
            if st: note.append('the inventory window %.0f x %.0f at %.0f,%.0f: doll %.0f x %.0f (slot %.0f px), stat strip x%.2f' % (e['WW'], e['WH'], e['WX'], e['WY'], e['LW'], e['PH'], 64 * e['D'], e['SS']))
            else: note.append('sheet %.0f x %.0f at %.0f,%.0f, stat row %.0f px, info row %.0f px, doll %.0f x %.0f (slot %.0f px), tabs x%.2f' % (
                e['WW'], e['WH'], e['WX'], e['WY'], 26 * e['S'], 26 * e['SI'], e['LW'], e['PH'], 64 * e['D'], e['F']))
        else:
            S = e['SQ']; gx = e['WX'] + e['BX'] + 12 * S; gy = e['WY'] + e['BY'] + INV_BAG_HEAD * S
            if grid:
                for r in range(int(e['ROWS'])):
                    for c in range(INV_COLS):
                        x = gx + c * INV_PITCH * S; y = gy + r * INV_PITCH * S; dr.rectangle([x, y, x + 50 * S, y + 50 * S], outline=(150, 132, 104, 255), fill=(30, 26, 24, 255))
            if mock: mock_inv(img, swf, e)
            note.append('bag: cell %.0f px, %d x %d in view, filter row x%.2f' % (50 * S, INV_COLS, e['ROWS'], e['SB']))
    return img, '%s, cell asked %d, screen %d x 1080: %s' % (view, C, DW, '; '.join(note))


def _font(px):
    from PIL import ImageFont
    for n in ('arial.ttf', 'DejaVuSans.ttf'):
        try: return ImageFont.truetype(n, max(8, int(px)))
        except OSError: pass
    return ImageFont.load_default()


def _sym(img, swf, name, k, x, y, frame=1):
    import dos2_swf_render as rn
    mv = gr.Movie(swf); cid = [c for c, n in mv.classes.items() if n.split('.')[-1] == name][0]; im, org = rn.render(swf, cid, k, frame)
    img.alpha_composite(im, (int(x + org[0] * k), int(y + org[1] * k)))


MOCK_STRIP = [[('0/0', 11), ('0/0', 12), ('1-2', 14), ('5%', 13), ('95%', 20), ('0%', 18), ('5.0', 15), ('10', 16)], [('0%', 5), ('0%', 6), ('0%', 7), ('0%', 8), ('0%', 9)]]
def mock_strip(img, swf, e):
    """the inventory window's stat strip as dmStripTick lays it out (value + icon cells, two rows, centred) - drawn here (the engine's data)"""
    from PIL import ImageDraw
    dr = ImageDraw.Draw(img); SS = e['SS']; fs = _font(19 * SS); X, Y = e['WX'], e['WY']
    cells = [[(v, ic, dr.textlength(v, font=fs) / SS + 8 + 32) for v, ic in row] for row in MOCK_STRIP]
    tot = max(sum(c[2] + 12 for c in row) for row in cells)
    x0 = X + e['RX'] + 8 * e['F'] + (e['RW'] - 16 * e['F'] - tot * SS) / 2; y0 = Y + e['CT'] + (e['SH'] - 2 * STRIP_PITCH * SS) / 2 + 2 * SS
    for r, row in enumerate(cells):
        x = x0
        for v, ic, wd in row:
            dr.text((x + 4 * SS, y0 + r * STRIP_PITCH * SS), v, font=fs, fill=(220, 210, 190, 255))
            _sym(img, swf, 'statIcon_125', SS, x + (wd - 24) * SS, y0 + (r * STRIP_PITCH + 3) * SS, ic)
            x += (wd + 12) * SS
    mock_info(img, swf, e)
    ft = _font(26 * e['F']); dr.text((X + e['WW'] / 2, Y + 35 * e['F']), 'FANE - LEVEL 1', font=ft, fill=(220, 210, 190, 255), anchor='mm')


def mock_inv(img, swf, e):
    """the inventory's run-time parts: the party tabs (the movie's own Tab), the bag header's name / gold / weight"""
    from PIL import ImageDraw
    dr = ImageDraw.Draw(img); X, Y = e['WX'], e['WY']; SB, SQ = e['SB'], e['SQ']
    cy = Y + e['FT'] + e['FH'] / 2
    for i in range(4): _sym(img, swf, 'Tab', SB, X + e['RX'] + e['EM'] + i * 46 * SB, cy - 20 * SB)
    f = _font(20 * SQ); hy = Y + e['BY'] + 18 * SQ
    dr.text((X + e['BX'] + 58 * SQ, hy), 'Fane', font=f, fill=(220, 210, 190, 255))
    dr.text((X + e['BX'] + (423 + INV_ADD) * SQ, hy), '0', font=f, fill=(220, 210, 190, 255)); dr.text((X + e['BX'] + (544 + INV_ADD) * SQ, hy), '6/100', font=f, fill=(220, 210, 190, 255))


def mock_info(img, swf, e):
    from PIL import ImageDraw
    dr = ImageDraw.Draw(img); X, Y = e['WX'], e['WY']; SI = e['SI']
    fi = _font(19 * SI); ry0 = e['CT'] + (e['IH'] - 74 * SI) / 2 + 3 * SI; ix = e['LX'] + 8 * e['F'] + 13 * SI
    for r, (lab, val, ic) in enumerate(MOCK_INFO):
        y = ry0 + r * 26 * SI
        _sym(img, swf, 'statIcon_125', SI, X + ix + 3 * SI, Y + y + 3 * SI, ic)
        dr.text((X + ix + 48 * SI, Y + y), lab, font=fi, fill=(200, 190, 170, 255)); dr.text((X + ix + 240 * SI, Y + y), val, font=fi, fill=(230, 220, 200, 255), anchor='ra')


MOCK_INFO = [('Vitality', '32/32', 1), ('Action Points', '6 (+4)', 2), ('Source Points', '0/1', 3)]
MOCK_ATTR = ['Strength', 'Finesse', 'Intelligence', 'Constitution', 'Memory', 'Wits']
MOCK_SEC = ['Damage', 'Critical Chance', 'Accuracy', 'Dodging', 'Physical Armour', 'Magic Armour', '', 'Movement', 'Initiative']
MOCK_RES = ['Fire', 'Water', 'Earth', 'Air', 'Poison']
def mock_sheet(img, swf, e):
    """what the engine fills in, drawn over the picture to judge the layout (NOT in the movie): the tabs (the movie's own StatsTabButton), the
    HP / AP / Source rows (InfoStat), the attributes tab's rows - in the places and scales dmLay gives their holders; text in a plain font"""
    from PIL import ImageDraw, ImageFont
    import dos2_swf_render as rn
    dr = ImageDraw.Draw(img); mv = gr.Movie(swf); X, Y = e['WX'], e['WY']
    def font(px):
        for n in ('arial.ttf', 'DejaVuSans.ttf'):
            try: return ImageFont.truetype(n, int(px))
            except OSError: pass
        return ImageFont.load_default()
    def sym(name, k, x, y, frame=1):
        cid = [c for c, n in mv.classes.items() if n.split('.')[-1] == name][0]; im, org = rn.render(swf, cid, k, frame)
        img.alpha_composite(im, (int(X + x + org[0] * k), int(Y + y + org[1] * k)))
    F, S, SI = e['F'], e['S'], e['SI']
    for i in range(5): sym('StatsTabButton', F, e['TX'] + 66 * i * F, e['TY'] + (23 if i == 0 else 21) * F)
    mock_info(img, swf, e)
    ft = font(26 * F); dr.text((X + e['WW'] / 2, Y + 35 * F), 'FANE - LEVEL 1', font=ft, fill=(220, 210, 190, 255), anchor='mm')
    dr.text((X + e['RX'] + e['RW'] / 2, Y + e['CT'] + e['IH'] - 24 * F), 'Attributes', font=ft, fill=(220, 210, 190, 255), anchor='mm')
    fs = font(19 * S); ink = (60, 40, 30, 255)
    def rows(x, y, labels, pitch):
        for r, lab in enumerate(labels):
            if lab: dr.text((X + x, Y + y + r * pitch), lab, font=fs, fill=ink); dr.text((X + x + 250 * S, Y + y + r * pitch), str(10 + r), font=fs, fill=ink, anchor='ra')
    mx, my = e['PCX'] + 20 * S, e['PCY'] + 9 * S
    rows(mx + 20 * S, my + 30 * S, MOCK_ATTR, 26 * S); rows(mx + 20 * S, my + CS_RES_Y * S, MOCK_RES, 30 * S)
    rows(mx + (20 + COL2) * S, my + 30 * S, MOCK_SEC, 28 * S); rows(mx + (20 + COL2) * S, my + (30 + 240) * S, ['Experience', 'Next level'], 28 * S)


def render(src, out, ipad=None):
    """pictures into `out`: every view at every asked cell on 16:9 and 20:9 (<view>_c<cell>_<width>.png, halved sheets <view>_cells.png), the baked
    movies' root next to the PC and the iPad movies (<name>_compare.png)"""
    from PIL import Image
    import dos2_swf_render as rn
    out = Path(out); out.mkdir(parents=True, exist_ok=True); bgc = (38, 44, 40, 255)
    for view in ('inv', 'sheet', 'equip'):
        tiles = []
        for DW in (1920, 2400):
            for C in CELLS:
                img, text = picture(src, view, C, DW); img.save(out / ('%s_c%d_%d.png' % (view, C, DW))); print(text); tiles.append(img)
        k = 0.25; W = int(2400 * k) + 10; Hh = int(STAGE_H * k) + 10
        sh = Image.new('RGBA', (W * len(CELLS) + 10, Hh * 2 + 10), (20, 22, 20, 255))
        for n, t in enumerate(tiles):
            sh.alpha_composite(t.resize((int(t.size[0] * k), int(t.size[1] * k)), Image.LANCZOS), (10 + (n % len(CELLS)) * W, 10 + (n // len(CELLS)) * Hh))
        sh.save(out / ('%s_cells.png' % view))
    def shot(swf, path):
        img, _ = rn.render(swf, 'root', 1.0); base = Image.new('RGBA', img.size, bgc); base.alpha_composite(img); base.save(path); return base
    for o, f in MAKE.items():
        dta, rep = f(src[SRC[o]]); (out / o).write_bytes(dta)
        imgs = [shot(Swf(dta), out / (o[:-4] + '.root.png')), shot(Swf(src[SRC[o]]), out / (SRC[o][:-4] + '.root.png'))]; names = ['touch (this pass)', 'PC movie']
        if o == 'characterSheet_p.swf': imgs.insert(1, picture(src, 'equip', CELL0, 1920)[0]); names.insert(1, 'touch: the equipment view')
        for n in (IPAD[o] if ipad else []): imgs.append(shot(Swf((Path(ipad) / n).read_bytes()), out / (n[:-4] + '.root.png'))); names.append('iPad ' + n)
        W = sum(i.size[0] for i in imgs) + 20 * (len(imgs) + 1); H = max(i.size[1] for i in imgs) + 40
        sheet_img = Image.new('RGBA', (W, H), (20, 22, 20, 255)); x = 20
        for i in imgs: sheet_img.alpha_composite(i, (x, 20)); x += i.size[0] + 20
        sheet_img.save(out / (o[:-6] + '_compare.png')); sheet_img.resize((W // 2, H // 2), Image.LANCZOS).save(out / (o[:-6] + '_compare_half.png'))
        print('%s: %s' % (out / (o[:-6] + '_compare.png'), ' | '.join(names)))


def main():
    a = sys.argv[1:]
    import dos2_pchud_touch as hud
    def sources(path):
        if os.path.isdir(path): return {n: (Path(path) / n).read_bytes() for n in SRC.values()}
        return hud.sources(path)
    if len(a) == 3 and a[0] == 'build':
        src = sources(a[1]); out = Path(a[2]); out.mkdir(parents=True, exist_ok=True)
        for o, f in MAKE.items():
            dta, rep = f(src[SRC[o]]); (out / o).write_bytes(dta); print('%s: %d B' % (out / o, len(dta)))
        return
    if len(a) == 2 and a[0] == 'check':
        src = sources(a[1]); bad = [0]
        def say(ok, text): bad[0] += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
        for o, f in MAKE.items():
            data = src[SRC[o]]; out, rep = f(data)
            print('%s -> %s: %d B -> %d B, +%d definition tags, +%d methods, sha256 %s' % (SRC[o], o, len(data), len(out), rep['defs'], len(rep['methods']), hashlib.sha256(out).hexdigest()[:16]))
            check(o, data, out, rep, say)
        sys.exit(1 if bad[0] else 0)
    if len(a) >= 3 and a[0] == 'render':
        render(sources(a[1]), a[2], a[a.index('--ipad') + 1] if '--ipad' in a else None); return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
