"""[DOS2] UI mode TOUCH, the inventory, the character sheet and the equipment view in the iPad's layout, fitted to a phone: the player's own
PC movies (Switch Game.pak partyInventory.swf, characterSheet.swf) transformed into partyInventory_p.swf / characterSheet_p.swf
(docs/dos2/TOUCH_MODES_PLAN.md "Implemented: iPad layout - inventory / character" and "... - phone fit"; the iPad's numbers:
docs/dos2/TOUCH_LAYOUT.md section 5). No iPad file is read; the backgrounds are the player's own bitmaps re-cut into pieces (new DefineShapes
with bitmap fills), the one new drawing is the doll button from our own assets/touchui. Static only: nothing here was played or run on a phone.

The geometry is NOT fixed numbers: every window is laid out by dmLay() from three document variables - dmCell (the cell px the host wants on
the screen; 76 = the iPad's), dmState (0 = the plain window, 1 = its half of the equipment view), dmW (the screen width in stage px at height
1080, from onEventResolution). The layout is written once, as data (Spec / Ex below): this tool evaluates it (the placements baked into the
movie = dmLay at cell 76 on a 16:9 screen, the checks, the pictures) and compiles the same statements to AVM2 (dmLay at run time). The host
sets the cell with dmSetCell(px), or through an invoke name the engine class registers: setAnchor(1000000 + px) for the inventory,
setHelmetOptionState(1000000 + px) for the sheet; 1000000 + 0 / 1 = the state. dmLay ends with setMcSize(w, 1080) + setPosition(center).

  partyInventory_p   the window clip x S = cell / 50 (cell 76: x 1.52). 13 columns of pitch 56 own px (the iPad's 13 x 86 px; the engine reads
                     the grid from the movie once: inventory_mc.invColW / cSpacing / cSize, main+0x13aa5b0), so the window is 770 own px wide
                     (PC 680: the background gets a stretched plain band, the bag header grows) = 1170 px (iPad 1160). Rows: as many as fit
                     1080 (7 at cell 76; the list scrolls); the background's body is one plain piece stretched to the list. A bigger cell =
                     a bigger window with fewer rows; never wider than the screen less 80 px margins (then S is clamped).
  characterSheet_p   state 0, the sheet (iPad characterSheet_t: no doll in it): x 1.70 (a stat row 26 -> 44 px, iPad 46), 1156 px wide, two
                     stat columns - attributes + resistances | secondary stats + experience (PC: one column 700 own px tall, more than the
                     screen holds at this size) - so the list panel needs 375..735 own px and the sheet can grow to x 1.89.
                     state 1, the doll panel of the EQUIPMENT VIEW (iPad equipmentPanel_t): the clip is cropped (scrollRect) to its doll
                     half, x 1.20 (409 x 1080 px, a slot 77 px; iPad doll 422 x 678, slot 82), at the left edge of the stage.
  equipment view     the doll panel + the inventory right of it as one centred block (host: dmSide(8) from the doll button of either
                     window, dm/host/hle_dos2_touchui_hud.c hu_view_plan): the bag keeps its 13 columns, so the block is 1580 px at cell 76
                     (iPad: 1160 with a 5 x 6 bag) - a phone screen is 1920..2520 wide, the iPad's 1440.

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
# ---- partyInventory (PC: inventory_mc at 0,20 on a 682 x 1152 stage; the window = bgFrame_mc 680 x 1056 at the clip's origin). Own px of the clip:
INV_W0, INV_H0 = 680, 1056
INV_COLS, INV_PITCH = 13, 56     # the iPad's 13 columns; cell 50 + spacing 6 (PC 11 columns, spacing 8): x 1.52 = cell 76, pitch 85 (iPad 76 / 86)
INV_ADD = 90                     # the window is this much wider than the PC's: 13 x 56 against 11 x 58
INV_W = INV_W0 + INV_ADD
INV_BAND = (226, 296)            # background columns that are plain in every row of the header: stretched to fill the extra width
INV_LIST_Y, INV_LIST_W0, INV_LIST_H0 = 202, 650, 827   # listHolder_mc.y, partyInventory_1.invW / invH
INV_LIST_W = INV_LIST_W0 + INV_ADD
INV_BAG_HEAD, INV_LIST_REST, INV_BOT = 56, 4, 27       # a bag's header, the PC's rest under the rows, the window under the list (PC: 1056 - 1029)
INV_ROWS_MAX = 12
INV_HEAD_ART, INV_BODY_SRC, INV_BODY_ART, INV_BOT_ART = 235, 400, 56, 100   # background rows: the header, plain body rows (stretched), the bottom edge
INV_SORT_BOT = 612               # the sort popup's own bottom (its y + 607 + 5)
INV_CLOSE = 50                   # screen px of the close button at the iPad's cell (iPad mcBtnCloseTouch 50 x 50)
INV_BTN, INV_BTN_AT = 'dmDoll_mc', (22, 88, 48)        # the doll button: own x, y, size (x 1.52 = 73 px, the iPad's)
BTN_ART = 148.0                  # px of our plate drawing (set from the asset when it is embedded)
# ---- characterSheet (PC: stats_mc at 0,20 on a 680 x 1152 stage; the window = bg_mc 680 x 1056, one bitmap with holes for the two halves). Own px of the clip:
CS_W, CS_H0 = 680, 1056
CS_S0 = (17, 10)                 # 1.70 at the iPad's cell: 680 -> 1156 px wide (iPad 1160), a stat row 26 -> 44 px (iPad 46)
CS_STRIP = (50, 174)             # background rows of the party portrait strip (cut out); the two corner tiles reach down to row 74
CS_TILE = (58, 626, 74)          # the corner tiles: left one ends at x 58, right one starts at x 626, both end at row 74
CS_UP = CS_STRIP[1] - CS_STRIP[0]                 # 124: everything under the strip moves up
CS_SPLIT = 339                   # own x between the list half and the doll half
CS_ROW1, CS_EQ_BAR = 253, 205    # background rows: the doll's top (= the tab title bar's top), the equipment title bar's top
CS_COL_Y, CS_BODY_Y = CS_ROW1 - CS_UP, CS_ROW1 - CS_UP + 87   # own y of the columns' art; of the stretched plain rows under the panel's top
CS_BODY_SRC, CS_BODY_ART, CS_FOOT_ART = 500, 56, 40           # plain rows of the list half (stretched by dmLay), the bottom edge
CS_HEAD, CS_BOT = 293 - CS_UP, 28                 # own y of the list panel; the window under it
CS_PB0, CS_PB_MIN = 735, 375     # the list panel's height: PC; the least that shows attributes + resistances / secondary stats + experience
CS_RES_Y = 205                   # resistances in mainStats_mc (PC 510): under the attributes; secondary stats go to the second column
CS_HIT_H = 1085.0
CS_EQ_Y, CS_EQ_Y_DOLL = CS_ROW1 - CS_UP, CS_STRIP[0] + CS_ROW1 - CS_EQ_BAR   # equip_mc.y on the sheet (off it: x far) / in the doll panel
CS_DOLL_W = CS_W - CS_SPLIT
CS_DOLL_H = CS_EQ_Y_DOLL + CS_H0 - CS_ROW1        # 901 own: the title bar, the equipment bar, the doll with its numbers
DOLL_PX = CS_DOLL_W * float(STAGE_H) / CS_DOLL_H  # the doll panel's screen width in the equipment view (its height is the screen's)
CS_TITLE_X = (42, CS_SPLIT + CS_DOLL_W // 2 - 299)             # title_txt.x (a 598 px centred field): on the sheet / centred in the doll panel
CS_BTN_AT = (626, 40)            # the doll button: own x, size (on the equipment title bar)
CS_LISTS = [('combatAbilityHolder_mc', 270, 705), ('civicAbilityHolder_mc', 270, 705), ('talentHolder_mc', 328, 705), ('tagsHolder_mc', 328, 705),
            ('visualHolder_mc', 328, 705)]        # holder, list frame width, scroll bar length (the PC init values)
CS_OFF = ['leftCycleBtn_mc', 'rightCycleBtn_mc', 'onePlayerOverlay_mc', 'charList_mc', 'alignments_mc', 'aiSel_mc', 'dragHit_mc']   # the strip's content and the drag bar
CS_STAY = ['hitArea_mc', 'bg_mc', 'title_txt', 'close_mc']     # above the strip
CS_FAR = -4000                   # own px: where hidden things go (the PC code still finds every instance)
CS_CLOSE = 50                    # screen px of the close button at the iPad's cell
CS_PM_HIT = (-6, -3, 32, 26)     # the + / - buttons' invisible hit sprite in the button's own px (PC: 0,1 18 x 18): x, y, w, h
CS_HELMET = (205, 24, 2.0)       # helmet toggle: x, y in equip_mc, scale (PC: a 21 px badge on the helmet slot at 176,49; iPad: its own 71 px button)


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
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 64, 64, s.ref.init_scope, s.ref.init_scope + 1)
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
            if isinstance(v, tuple): return num(v[0]) + num(v[1]) + [('divide',)]
            if float(v) == int(v): return num(v)
            return num(round(v * 1000)) + num(1000) + [('divide',)]
        if s.op == 'v': return [('getlocal', cx.var(s.a[0]))]
        if s.op == 'floor': return s.a[0].asm(cx) + [('convert_i',), ('convert_d',)]
        if s.op in ('min', 'max'):
            t, u = cx.tmp(), cx.tmp(); a, b = cx.label(), cx.label()
            return s.a[0].asm(cx) + [('convert_d',), ('setlocal', t)] + s.a[1].asm(cx) + [('convert_d',), ('setlocal', u), ('getlocal', t), ('getlocal', u),
                    ('ifnlt' if s.op == 'min' else 'ifngt', a), ('getlocal', t), ('convert_d',), ('jump', b), ('L', a), ('getlocal', u), ('convert_d',), ('L', b)]
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
        s._v = {}; s._n = 2; s._l = 0; s._c = c
        o = [('pushnull',), ('coerce_a',), ('setlocal', 1)]
        for name, slot in inputs: o += [('getlocal0',), ('getproperty', c.P(slot)), ('convert_d',), ('setlocal', s.var(name))]
        body = s._lines(s.st)
        pre = []
        for k in range(2 + len(inputs), s._n): pre += [('pushbyte', 0), ('convert_d',), ('setlocal', k)]
        return o + pre + body
    def var(s, name):
        if name not in s._v: s._v[name] = s._n; s._n += 1
        return s._v[name]
    def tmp(s): s._n += 1; return s._n - 1
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
INPUTS = [('C', 'dmCell'), ('ST', 'dmState'), ('DW', 'dmW')]
def window_doc(code, spec, off, btn=None, back=0, cmd='setAnchor', ctx=None):
    """the document class of a `_p` window: vars dmCell (cell px, CELL0 = the iPad's), dmState, dmW (the screen width in stage px, height 1080);
    dmLay() = `spec`; dmSetCell(px) / dmSetState(n) for the host; `cmd`(CMD + n) = the same two through a name the engine class registers;
    onEventInit = original + dmHook (drag bar `off` hidden, the doll button `btn` armed: dmSide(8) = the equipment view, dmSide(back) from it)
    + dmLay; onEventResolution(w, h) = original + dmW + dmLay."""
    d = code.of('MainTimeline', 'onEventInit'); P = d.P
    for n, v in (('dmCell', CELL0), ('dmState', 0), ('dmW', SCREEN0)): add_slot(d, n, v)
    d.method('dmLay', 0, HEAD + spec.asm(d, INPUTS) + [('returnvoid',)])
    lay = [('getlocal0',), ('callpropvoid', P('dmLay'), 0)]
    d.method('dmSetCell', 1, HEAD + [('getlocal1',), ('pushbyte', 40), ('ifnlt', 'e'), ('returnvoid',), ('L', 'e'), ('getlocal0',), ('getlocal1',), ('setproperty', P('dmCell'))] + lay + [('returnvoid',)])
    d.method('dmSetState', 1, HEAD + [('getlocal0',), ('getlocal1',), ('setproperty', P('dmState'))] + lay + [('returnvoid',)])
    hook = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1)]
    for k, path in enumerate(off):
        hook += [('getlocal0',), ('coerce_a',), ('setlocal', 1)]
        for p in path: hook += [('getlocal', 1), ('getproperty', P(p)), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'h%d' % k)]
        hook += [('getlocal', 1), ('pushfalse',), ('setproperty', P('visible')), ('L', 'h%d' % k)]
    if btn:
        hook += [('getlocal0',), ('pushstring', d.S(btn)), ('callproperty', P('getChildByName'), 1), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'nb'),
                 ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseChildren')),
                 ('getlocal', 1), ('pushstring', d.S('mouseUp')), ('getlocal0',), ('getproperty', P('dmBtnUp')), ('callpropvoid', P('addEventListener'), 2), ('L', 'nb')]
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
    """new sprites made of rectangles of ONE existing bitmap. groups = [(name | None, x, y, [(dst x, dst y, src x, src y, w, h, sx)])]: every
    group is a 1-frame sprite of its pieces (each its own DefineShape: a clipped, smoothed 1:1 bitmap fill, drawn 1 px larger to the right
    and down while the bitmap has those px, then scaled sx horizontally), placed by name at x, y inside one outer sprite. -> (outer id, n tags)"""
    used = {struct.unpack_from('<H', d, 0)[0] for c, d, _ in swf.tags if c in gr.DEFS} | set(gr.Movie(swf).imports)
    nid = [max(used) + 1]; tags = []
    def nb(*v): return max(2, max(abs(int(x)).bit_length() for x in v) + 1)
    def new(): nid[0] += 1; assert nid[0] < 0xffff; return nid[0] - 1
    def place(depth, cid, x, y, sx=1.0, name=None):
        return gr.tag_bytes(*gr.place_build(dict(depth=depth, char=cid, matrix=gr.matrix_bytes(x, y, sx, 1.0), cx=None, ratio=None, name=None if name is None else name.encode() + b'\0',
                                                 clip=None, cls=None, f2=0, rest3=b'', move=False, v3=False, actions=None)))
    outer = b''
    for gk, (name, gx, gy, pieces) in enumerate(groups):
        spr = b''
        for k, (dx, dy, sx, sy, w, h, scx) in enumerate(pieces):
            assert 0 <= sx and sx + w <= bw and 0 <= sy and sy + h <= bh, 'piece %r outside the %d x %d bitmap' % ((sx, sy, w, h), bw, bh)
            if scx == 1.0: w = min(w + 1, bw - sx)
            h = min(h + 1, bh - sy); W, H = w * 20, h * 20
            fill = b'\x01\x41' + struct.pack('<H', bid) + gr.matrix_bytes(-sx, -sy, 20.0, 20.0)
            rec = [(0, 1), (0, 1), (0, 1), (1, 1), (0, 1), (1, 1), (2, 5), (0, 2), (0, 2), (1, 1)]
            for ex, ey in ((W, 0), (0, H), (-W, 0), (0, -H)):
                n = nb(ex, ey); rec += [(1, 1), (1, 1), (n - 2, 4), (1, 1), (ex, n), (ey, n)]
            rec += [(0, 1), (0, 5)]
            n = nb(W, H); sid = new()
            tags.append([2, struct.pack('<H', sid) + gr.wbits([(n, 5), (0, n), (W, n), (0, n), (H, n)]) + fill + b'\x00' + bytes([0x10]) + gr.wbits(rec), False])
            spr += place(k + 1, sid, dx, dy, scx)
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


def doll_button(swf, name, glyph='pan_helmet'):
    """the equipment (doll) button: our own art (assets/touchui: the copper plate + a glyph), placed on the root timeline by name at 0,0 scale 1
    (dmLay / bake place it). -> (the plate's px, n new definition tags)"""
    import dos2_touchart as ta
    g = gr.Graft(swf); art = ta.Art(g); sq, qb = art.png('plate_square_copper'); ic, ib = art.png(glyph)
    pw = float(qb[2] - qb[0]); ki = 0.72 * pw / (ib[2] - ib[0])
    spr = g.sprite([(sq, 1, (0, 0), None), (ic, 2, (pw / 2 - (ib[2] - ib[0]) * ki / 2, pw / 2 - (ib[3] - ib[1]) * ki / 2, ki), None)])
    g.place(spr, 0, 0, name); g.finish()
    return pw, len(g.new_defs)


# ---------------------------------------------------------------- partyInventory_p
INV_CLIP = 'inventory_mc'
def inv_geom(C=CELL0, ST=0, DW=SCREEN0):
    """the numbers dmLay computes (the same Spec, run here): dict of the Spec's variables"""
    env = dict(C=C, ST=ST, DW=DW); inv_spec(None).run(env); return env


def inv_spec(k):
    """dmLay of partyInventory_p. k = the PC placements of the window's children (None: only the variables are wanted)"""
    s = Spec(); C, ST, DW = V('C'), V('ST'), V('DW')
    s.state('ST', lambda a: a.let('AV', DW - 2 * MARGIN - DOLL_PX), lambda a: a.let('AV', DW - 2 * MARGIN))
    S = s.let('S', fmin(C / 50, V('AV') / INV_W))
    rows = s.let('ROWS', fmax(1, fmin(INV_ROWS_MAX, ffloor((1080 / S - (INV_LIST_Y + INV_BAG_HEAD + INV_LIST_REST + INV_BOT)) / INV_PITCH))))
    LH = s.let('LH', INV_BAG_HEAD + INV_LIST_REST + rows * INV_PITCH)
    H = s.let('H', INV_LIST_Y + LH + INV_BOT)
    s.state('ST', lambda a: a.let('X0', DOLL_PX), lambda a: a.let('X0', 0))
    X0 = V('X0'); MW = s.let('MW', ffloor(X0 + INV_W * S + R(999, 1000))); Y0 = s.let('Y0', (1080 - H * S) / 2)
    s.let('PX', 50 * S)
    if k is None: return s
    w = (INV_CLIP,)
    s.on(w, [('scaleX', S), ('scaleY', S), ('x', X0), ('y', Y0)])
    s.on(w + ('bgFrame_mc', ('at', 0), ('nm', 'b')), [('scaleY', (H - INV_BOT_ART - INV_HEAD_ART) / INV_BODY_ART)])
    s.on(w + ('bgFrame_mc', ('at', 0), ('nm', 'f')), [('y', H - INV_BOT_ART)])
    s.on(w + ('faderBot',), [('y', INV_LIST_Y + LH)])
    s.on(w + ('keyHintsHolder_mc',), [('y', k['keyHintsHolder_mc'][2] - INV_H0 + H)])
    s.on(w + ('hitArea_mc',), [('scaleY', k['hitArea_mc'][4] * (H - 40) / (INV_H0 - 40))])
    s.on(w + ('sortByBg_mc',), [('scaleY', k['sortByBg_mc'][4] * LH / INV_LIST_H0)])
    s.on(w + ('sortBy_mc',), [('y', fmin(k['sortBy_mc'][2], H - INV_SORT_BOT))])     # the sort popup stays inside a short window
    s.on(w + (('dyn', 'list'),), calls=[('setFrame', [INV_LIST_W, LH])])
    s.on(w + (('dyn', 'list'), ('dyn', 'm_scrollbar_mc')), calls=[('scrollbarVisible', [])])
    s.on((('nm', INV_BTN),), [('x', X0 + INV_BTN_AT[0] * S), ('y', Y0 + INV_BTN_AT[1] * S), ('scaleX', INV_BTN_AT[2] * S / BTN_ART), ('scaleY', INV_BTN_AT[2] * S / BTN_ART)])
    # keepCustomInScreen: the part of the stage the engine keeps on the screen (PC: uiLeft 0, uiRight 680, uiTop 45, uiMinHeight 190, uiMinWidth 150)
    for prop, e in (('uiLeft', X0), ('uiRight', X0 + INV_W * S), ('uiTop', Y0 + 45 * S), ('uiMinHeight', 190 * S), ('uiMinWidth', 150 * S)): s.root(prop, ffloor(e + R(1, 2)))
    s.ei('setMcSize', MW, 1080); s.ei('setPosition', 'center', 'screen', 'center')
    return s


def _kids(swf, sprite):
    kids = {}
    def note(name, rec):
        if name: kids[name] = (rec['char'],) + _mat(rec)
    edit_places(swf, sprite, note); return kids


def party(data, env=None):
    """env = dict(C, ST, DW) the baked layout (default: the iPad's cell on a 16:9 screen, the plain window); dmLay computes the same at run time"""
    swf = Swf(data); mv = gr.Movie(swf)
    root = _root_names(swf); win = root[INV_CLIP]['char']; assert _mat(root[INV_CLIP]) == (0.0, 20.0, 1.0, 1.0), 'inventory_mc is not at 0,20'
    kids = _kids(swf, win)
    need = ['bgFrame_mc', 'hitArea_mc', 'faderTop', 'faderBot', 'close_mc', 'keyHintsHolder_mc', 'sortByBg_mc', 'sortBy_mc', 'listHolder_mc', 'minimise_mc', 'dragHit_mc', 'title_txt',
            'tabs_mc', 'view_mc', 'autosortBtn_mc', 'sortByBtn_mc', 'allBtn_mc', 'totalGold_mc']
    assert all(n in kids for n in need), 'partyInventory_1 children: %s' % sorted(kids)
    assert kids['listHolder_mc'][2] == INV_LIST_Y and kids['faderBot'][2] == INV_LIST_Y + INV_LIST_H0, 'list / fader place'
    # the background: frame 1 of bgFrame_mc is one 680 x 1056 bitmap -> head (3 columns: left, a band of the plain middle stretched, right),
    # body (56 plain rows, stretched to the list's height by dmLay), bottom edge (moved by dmLay)
    holder = kids['bgFrame_mc'][0]; shape = list(gr.frame_state(swf.tags[_def_index(swf, holder)][1], 1).values()); assert len(shape) == 1; shape = shape[0]['char']
    bid, bw, bh = bitmap_of(swf, shape); assert (bw, bh) == (INV_W0, INV_H0), 'background %d x %d' % (bw, bh)
    a, b = INV_BAND; ax = INV_ADD / float(b - a)
    def cols(sy, h): return [(0, 0, 0, sy, b, h, 1.0), (b, 0, a, sy, b - a, h, ax), (b + INV_ADD, 0, b, sy, bw - b, h, 1.0)]
    new, ndef = pieces_sprite(swf, holder, bid, bw, bh, [(None, 0, 0, cols(0, INV_HEAD_ART)), ('b', 0, INV_HEAD_ART, cols(INV_BODY_SRC, INV_BODY_ART)), ('f', 0, 0, cols(bh - INV_BOT_ART, INV_BOT_ART))])
    swap_char(swf, holder, 1, shape, new)
    # the wider window: what stands right of the band moves with the right part, what is centred moves half way, what spans the window grows
    kx = INV_W / float(INV_W0); lx = INV_LIST_W / float(INV_LIST_W0)
    def fix(name, rec):
        x, y, sx, sy = _mat(rec)
        if name in ('close_mc', 'minimise_mc', 'autosortBtn_mc', 'sortByBtn_mc', 'allBtn_mc', 'totalGold_mc'): _set(rec, dx=INV_ADD)
        elif name in ('title_txt', 'sortBy_mc', 'keyHintsHolder_mc'): _set(rec, dx=INV_ADD / 2.0)
        elif name == 'hitArea_mc': _set(rec, sx=sx * (INV_W - 4) / (INV_W0 - 4.0))
        elif name in ('faderTop', 'faderBot'): _set(rec, sx=sx * lx)
        elif name == 'sortByBg_mc': _set(rec, sx=sx * (INV_LIST_W + 15) / (INV_LIST_W0 + 15.0))
    edit_places(swf, win, fix)
    kids = _kids(swf, win)
    cs = INV_CLOSE / float(CELL0) * 50 / 23.0   # the close button: INV_CLOSE screen px at the iPad's cell
    edit_places(swf, win, lambda name, rec: _set(rec, x=kids['close_mc'][1] + 11.5 - 11.5 * cs, y=kids['close_mc'][2] + 11.5 - 11.5 * cs, sx=cs, sy=cs) if name == 'close_mc' else None)
    # a bag's header (playerInventory): its frame, body and drop mark grow with the grid, gold / weight / the fold mark stay at the right end
    bag = [c for c, n in mv.classes.items() if n == 'playerInventory']; assert len(bag) == 1
    bk = _kids(swf, bag[0]); assert all(n in bk for n in ('invBg_mc', 'frame_mc', 'drop_mc', 'weightAndGoldIcons_mc', 'gold_txt', 'weight_txt', 'toggleInd_mc')), sorted(bk)
    def bagfix(name, rec):
        x, y, sx, sy = _mat(rec)
        if name in ('invBg_mc', 'frame_mc', 'drop_mc'): _set(rec, sx=sx * lx)
        elif name in ('weightAndGoldIcons_mc', 'gold_txt', 'weight_txt', 'toggleInd_mc'): _set(rec, dx=INV_ADD)
    edit_places(swf, bag[0], bagfix)
    global BTN_ART
    BTN_ART, nbtn = doll_button(swf, INV_BTN)

    env = dict(env or {}); env.setdefault('C', CELL0); env.setdefault('ST', 0); env.setdefault('DW', SCREEN0)
    spec = inv_spec(kids); res = spec.run(env); rest = bake(swf, res)
    swf.head = rect_bytes(int(env['MW']), STAGE_H) + swf.head[-4:]

    code = Code(swf)
    d = window_doc(code, spec, [(INV_CLIP, 'minimise_mc'), (INV_CLIP, 'dragHit_mc')], INV_BTN, 4, ctx=((INV_CLIP,), 'partyInventory_1'))
    # the sort popup: its OK sends setSortFilter (handled) and applySortFilters (NOT in main.raw: nothing would be sorted) -> then `autosort`,
    # the call of the sort button, which the class handles (main+0x13a77f0)
    sp = code.of('sortPopup_21', 'okPressed')
    sp.wrap('okPressed', 'dmOkOrig', lambda n: HEAD + sp.call_orig('dmOkOrig', n) + sp.ei('autosort') + [('returnvoid',)])
    # a cell's tooltip rectangle in stage px (the PC code passes the cell's own 50 x 50)
    w = code.of('partyInventory_1', 'onCellCreate')
    px = [('getlocal0',), ('getproperty', w.P('scaleX'))] + num(50) + [('multiply',)]
    w.wrap('onCellCreate', 'dmCellOrig', lambda n: HEAD + w.call_orig('dmCellOrig', n) + [('getlocal1',), ('iffalse', 'end')] +
           w.setp(1, 'tooltipOverrideW', px) + w.setp(1, 'tooltipOverrideH', px) + [('L', 'end'), ('returnvoid',)])
    # the grid the engine reads once from inventory_mc (main+0x13aa5b0: cSize, cSpacing, bgDiscrap, invColW, portraitH / W): the iPad's 13 columns, pitch 56
    for name, v in (('invColW', INV_COLS), ('cSpacing', INV_PITCH - 50), ('invW', INV_LIST_W), ('invH', int(inv_geom()['LH']))): set_const(w, name, v)
    made = code.done()
    S = env['S']
    return swf.write(), dict(placed=[INV_BTN], defs=ndef + nbtn, methods=made, env=env, rest=rest, window=(env['X0'], env['Y0'], env['X0'] + INV_W * S, env['Y0'] + env['H'] * S), bitmap=bid, shape=new,
                             stage=(int(env['MW']), STAGE_H), spec=spec)


# ---------------------------------------------------------------- characterSheet_p
CS_CLIP = 'stats_mc'
def cs_geom(C=CELL0, ST=0, DW=SCREEN0):
    env = dict(C=C, ST=ST, DW=DW); cs_spec(None).run(env); return env


def cs_spec(k):
    """dmLay of characterSheet_p: state 0 = the sheet (two stat columns, no doll), state 1 = the doll panel of the equipment view"""
    s = Spec(); C, DW = V('C'), V('DW')
    def sheet(a):
        S = a.let('S', fmin(fmin(R(CS_S0[0], CS_S0[1]) * C / CELL0, (DW - 2 * MARGIN) / CS_W), R(STAGE_H, CS_HEAD + CS_BOT + CS_PB_MIN)))
        PB = a.let('PB', fmax(CS_PB_MIN, fmin(CS_PB0, ffloor(STAGE_H / S - CS_HEAD - CS_BOT))))
        H = a.let('H', CS_HEAD + PB + CS_BOT)
        a.let('MW', ffloor(CS_W * S + R(999, 1000))); a.let('Y0', (STAGE_H - H * S) / 2)
        for n, v in (('RX', 0), ('RW', CS_W), ('XS', 0), ('XD', CS_FAR), ('EY', CS_EQ_Y), ('TX', CS_TITLE_X[0])): a.let(n, v)
    def doll(a):
        S = a.let('S', R(STAGE_H, CS_DOLL_H)); a.let('PB', CS_PB_MIN); a.let('H', CS_DOLL_H)
        a.let('MW', ffloor(DOLL_PX + INV_W * fmin(C / 50, (DW - 2 * MARGIN - DOLL_PX) / INV_W) + R(999, 1000))); a.let('Y0', 0)
        for n, v in (('RX', CS_SPLIT), ('RW', CS_DOLL_W), ('XS', CS_FAR), ('XD', 0), ('EY', CS_EQ_Y_DOLL), ('TX', CS_TITLE_X[1])): a.let(n, v)
    s.state('ST', doll, sheet)
    S, PB, H, MW, Y0, RX, RW, XS, XD, EY = [V(n) for n in ('S', 'PB', 'H', 'MW', 'Y0', 'RX', 'RW', 'XS', 'XD', 'EY')]
    if k is None: return s
    w = (CS_CLIP,)
    s.on(w, [('scaleX', S), ('scaleY', S), ('x', 0), ('y', Y0)])
    s.rect(w, RX, 0, RW, H)                                  # the window clip shows its own px RX.. only: the doll panel = the right half
    bg = w + ('bg_mc', ('at', 0))
    s.on(bg + (('nm', 's0'),), [('x', XS)])
    s.on(bg + (('nm', 'sb'),), [('x', XS), ('scaleY', (H - CS_FOOT_ART - CS_BODY_Y) / CS_BODY_ART)])
    s.on(bg + (('nm', 'sf'),), [('x', XS), ('y', H - CS_FOOT_ART)])
    s.on(bg + (('nm', 'd0'),), [('x', CS_SPLIT + XD)])
    s.on(w + ('panelBg1_mc',), [('scaleY', PB / CS_PB0)])
    s.on(w + (('nm', 'dmPanel2'),), [('x', k['dmPanel2'][1] + XS), ('scaleY', PB / CS_PB0)])
    s.on(w + ('mainStats_mc',), [('x', k['mainStats_mc'][1] + XS)])
    s.on(w + ('charInfo_mc',), [('x', k['charInfo_mc'][1] + XS)])
    s.on(w + ('equip_mc',), [('x', k['equip_mc'][1] + XD), ('y', EY)])
    s.on(w + (('nm', 'dmDollBg'),), [('x', k['dmDollBg'][1] + XD), ('y', EY - 1)])
    s.on(w + ('equipment_txt',), [('x', k['equipment_txt'][1] + XD), ('y', EY - 44)])
    s.on(w + ('title_txt',), [('x', V('TX'))])
    s.on(w + ('hitArea_mc',), [('scaleY', k['hitArea_mc'][4] * (H - 17) / CS_HIT_H)])
    s.raw('lists', cs_lists)
    s.on((('nm', INV_BTN),), [('x', (CS_BTN_AT[0] - RX) * S), ('y', Y0 + (EY - 45) * S), ('scaleX', CS_BTN_AT[1] * S / BTN_ART), ('scaleY', CS_BTN_AT[1] * S / BTN_ART)])
    for prop, e in (('uiLeft', 0), ('uiRight', RW * S), ('uiTop', Y0 + 45 * S), ('uiMinHeight', 190 * S), ('uiMinWidth', 150 * S)): s.root(prop, ffloor(e + R(1, 2)))
    s.ei('setMcSize', MW, STAGE_H); s.ei('setPosition', 'center', 'screen', 'center')
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
                ('getlocal', o)] + num(w) + [('getlocal', pb), ('callpropvoid', P('setFrame'), 2),
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
    need = CS_OFF + CS_STAY + ['panelBg1_mc', 'panelBg2_mc', 'equip_mc', 'charInfo_mc', 'tabsHolder_mc', 'mainStats_mc', 'equipment_txt'] + [h for h, _, _ in CS_LISTS]
    assert all(n in kids for n in need), 'stats_1 children: %s' % sorted(kids)
    assert kids['bg_mc'][1:3] == (0.0, 0.0) and kids['charInfo_mc'][2] == CS_STRIP[1] - 1 and kids['panelBg1_mc'][1:3] == (24.0, 293.0) and kids['equip_mc'][1:3] == (341.0, 253.0), 'bg / name bar / panel / doll place'
    # ---- the background: one bitmap (+ four grille tiles over the portrait strip: dropped with it) -> the sheet's pieces and the doll panel's pieces
    holder = kids['bg_mc'][0]; shape = list(gr.frame_state(swf.tags[_def_index(swf, holder)][1], 1).values()); assert len(shape) == 1; shape = shape[0]['char']
    bid, bw, bh = bitmap_of(swf, shape, True); assert (bw, bh) == (CS_W, CS_H0), 'background %d x %d' % (bw, bh)
    a, b = CS_STRIP; lx, rx, ty = CS_TILE; th = ty - a; r1 = CS_ROW1; L, Rw = CS_SPLIT + 2, CS_W - CS_SPLIT - 2
    def two(sy, h): return [(0, 0, 0, sy, L, h, 1.0), (L, 0, 2, sy, Rw, h, 1.0)]      # the list column's art twice: the second stat column looks like the first
    s0 = [(0, 0, 0, 0, CS_W, a, 1.0), (0, a, 0, a, lx, th, 1.0), (lx, a, lx, b, rx - lx, th, 1.0), (rx, a, rx, a, CS_W - rx, th, 1.0),   # title; the two corner tiles end, the name bar between them
          (0, ty, 0, b + th, CS_W, r1 - (b + th), 1.0)] + [(x, CS_COL_Y, sx, r1, w, CS_BODY_Y - CS_COL_Y, 1.0) for x, _, sx, _, w, _, _ in two(0, 0)]
    assert ty + r1 - (b + th) == CS_COL_Y
    d0 = [(0, 0, 0, 0, lx, a, 1.0), (lx, 0, CS_W - (CS_DOLL_W - lx), 0, CS_DOLL_W - lx, a, 1.0),          # the title bar's two ends
          (0, a, CS_SPLIT, CS_EQ_BAR, CS_DOLL_W, r1 - CS_EQ_BAR, 1.0), (0, a + r1 - CS_EQ_BAR, CS_SPLIT, r1, CS_DOLL_W, bh - r1, 1.0)]
    assert a + r1 - CS_EQ_BAR == CS_EQ_Y_DOLL and CS_EQ_Y_DOLL + bh - r1 == CS_DOLL_H
    new, ndef = pieces_sprite(swf, holder, bid, bw, bh, [('s0', 0, 0, s0), ('sb', 0, CS_BODY_Y, two(CS_BODY_SRC, CS_BODY_ART)), ('sf', 0, 0, two(bh - CS_FOOT_ART, CS_FOOT_ART)), ('d0', CS_SPLIT, 0, d0)])
    swap_char(swf, holder, 1, shape, new)
    # drop the overlays of the old background shape's holder (the grille tiles are other fills of the same shape: gone with it)
    cs = CS_CLOSE / (CS_S0[0] / float(CS_S0[1])) / 23.0; moved = []
    hb = gr.bounds(swf, kids['hitArea_mc'][0]); assert hb[1] == 0 and abs(hb[3] - CS_HIT_H) < 0.01
    def fix(name, rec):
        x, y, sx, sy = _mat(rec)
        if name in CS_OFF: _set(rec, y=CS_FAR)
        elif name == 'close_mc': _set(rec, x=x + 11.5 - 11.5 * cs, y=y + 11.5 - 11.5 * cs, sx=cs, sy=cs)
        elif name in CS_STAY: pass
        else:
            assert y >= CS_STRIP[1] - 1, '%s at y %.1f: inside the strip' % (name, y)
            _set(rec, dy=-CS_UP)
            if name is None and (x, y) == (341.0, 252.0): rec['name'] = b'dmDollBg\0'; moved.append('dmDollBg')   # the doll's backdrop: named, so dmLay can move it
            if name: moved.append(name)
    edit_places(swf, win, fix); assert 'dmDollBg' in moved, 'the doll backdrop (an unnamed shape at 341,252) is not in stats_1'
    # the second stat column: a list panel backing under it (the shape of panelBg1_mc's frame 1), secondary stats + experience there, resistances under the attributes
    pshape = [r['char'] for r in gr.frame_state(swf.tags[_def_index(swf, kids['panelBg1_mc'][0])][1], 1).values()]; assert len(pshape) == 1
    add_place(swf, win, 6, pshape[0], 24 + CS_SPLIT + 0.0, 293 - CS_UP, 'dmPanel2')
    ms = _kids(swf, kids['mainStats_mc'][0]); assert ms['statHolder_mc'][1:3] == (20.0, 30.0) and ms['secStatHolder_mc'][1:3] == (20.0, 200.0) and ms['resistancesStatHolder_mc'][1:3] == (20.0, 510.0), ms
    def stats(name, rec):
        if name == 'secStatHolder_mc': _set(rec, x=20 + CS_SPLIT, y=30)
        elif name == 'resistancesStatHolder_mc': _set(rec, y=CS_RES_Y)
    edit_places(swf, kids['mainStats_mc'][0], stats)
    # + / - : a finger-sized invisible hit sprite (the listeners sit on hit_mc); the helmet toggle: beside the helmet slot, twice the size
    hx, hy, hw, hh = CS_PM_HIT; pm = []
    for cls in ('plusButton_62', 'minusButton_65'):
        cid = [c for c, n in mv.classes.items() if n.endswith('.' + cls)]; assert len(cid) == 1, cls
        def hit(name, rec):
            if name == 'hit_mc': assert _mat(rec) == (0.0, 1.0, 1.0, 1.0), 'hit_mc of %s' % cls; _set(rec, x=hx, y=hy, sx=hw / 18.0, sy=hh / 18.0); pm.append(cls)
        edit_places(swf, cid[0], hit)
    def helm(name, rec):
        if name == 'helmet_mc': assert _mat(rec)[:2] == (176.0, 49.0); _set(rec, x=CS_HELMET[0], y=CS_HELMET[1], sx=CS_HELMET[2], sy=CS_HELMET[2]); pm.append(name)
    edit_places(swf, kids['equip_mc'][0], helm); assert len(pm) == 3
    global BTN_ART
    BTN_ART, nbtn = doll_button(swf, INV_BTN)

    kids = _kids(swf, win)
    env = dict(env or {}); env.setdefault('C', CELL0); env.setdefault('ST', 0); env.setdefault('DW', SCREEN0)
    spec = cs_spec(kids); res = spec.run(env); rest = bake(swf, res)
    swf.head = rect_bytes(int(env['MW']), STAGE_H) + swf.head[-4:]
    code = Code(swf)
    window_doc(code, spec, [(CS_CLIP, 'dragHit_mc')], INV_BTN, 3, 'setHelmetOptionState', ctx=((CS_CLIP, 'equip_mc'), 'equipment_41'))
    # The doll turns under the finger by the PC class's own means, nothing is changed here: equipment_41.onDown sends dollDown (handled,
    # main+0x12a59c8: byte [object+0x3f8] = 1 + the mouse capture); the class's input handler main+0x12af270 turns it on the events 0x6a /
    # 0x6b (FlashMouseMoveLeft / Right: [object+0x3fc] = the turn) and STOPS it on event 1 (FlashLeftMouse, the button's release:
    # [+0x3f8] = 0, capture off). dollUp / dollOut of the movie are not in main.raw and are not needed: the engine stops on the release itself.
    made = code.done(); S = env['S']
    return swf.write(), dict(placed=[INV_BTN], defs=ndef + nbtn, methods=made, env=env, rest=rest, window=(0.0, env['Y0'], env['RW'] * S, env['Y0'] + env['H'] * S), bitmap=bid, shape=new, moved=moved,
                             stage=(int(env['MW']), STAGE_H), spec=spec)


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
            e = geom(C, state, DW); ok = e['MW'] <= DW - 2 * MARGIN + 1 and e['H'] * e['S'] <= STAGE_H + 0.01 and e['Y0'] >= -0.01
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
            'characterSheet_p.swf': {'stats_1', 'bg_22', 'plusButton_62', 'minusButton_65', 'equipment_41', '#456'}}[o]
    say(not g and {nm(c) for c in changed} == want and len(new) == rep['defs'],
        'tags: ids unique, uses after definitions; of %d PC definitions %d rebuilt (%s), +%d new (background pieces, the doll button); the rest byte for byte%s' % (
        len(ida), len(changed), ', '.join(sorted(nm(c) for c in changed)), len(new), '' if not g else ': ' + str(g)))
    bitmaps = lambda s: sorted(hashlib.sha256(d[2:]).hexdigest() for c, d, _ in s.tags if c in gr.T_BITMAPS)
    ba, bb = bitmaps(a), bitmaps(b); extra = [h for h in bb if h not in ba]
    say(all(h in bb for h in ba) and len(extra) == 2, 'art: the PC movie\'s %d bitmaps + %d of our own drawings (assets/touchui plate_square_copper, pan_helmet: the doll button); no iPad file is read' % (len(ba), len(extra)))
    say(b.head[:-4] == rect_bytes(*rep['stage']), 'stage %d x %d = the window at the iPad\'s cell (dmLay tells the engine the real size: setMcSize)' % rep['stage'])
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
    say("'center'" in l and 'setMcSize' in l and 'scrollbarVisible' in l or ("'center'" in l and 'setLength' in l), 'dmLay: computes the layout from dmCell / dmState / dmW, then setMcSize + setPosition(center, screen, center)')
    # every path of the layout exists on the timeline (or is a member the PC code makes)
    bad = []
    for path in rep['spec'].paths():
        if any(not isinstance(s, str) and s[0] == 'dyn' and s[1] not in DYN for s in path): bad.append(path); continue
        try: _resolve(b, path)
        except AssertionError as e: bad.append(str(e))
    say(not bad, 'dmLay: all %d object paths are placed instances of the built movie%s' % (len(list(rep['spec'].paths())), '' if not bad else ': ' + str(bad)))
    w = _walk(out); x0, y0, x1, y1 = rep['window']; env = rep['env']; S = env['S']
    if o == 'partyInventory_p.swf':
        bgb = w['inventory_mc.bgFrame_mc']['stage']
        say(max(abs(u - v) for u, v in zip(bgb, rep['window'])) < 1.6, 'baked (cell %d): window %.0f,%.0f..%.0f,%.0f = %.0f x %.0f px, x %.2f; iPad -3,0..1165,968 (1160 x 968); PC 680 x 1056 at the screen corner' % (
            env['C'], bgb[0], bgb[1], bgb[2], bgb[3], bgb[2] - bgb[0], bgb[3] - bgb[1], S))
        say(abs(50 * S - CELL0) < 0.01, 'cell 50 -> %.0f px, pitch %d -> %.1f (iPad 76 / 86); %d columns (iPad 13; the engine reads invColW / cSpacing from the movie: main+0x13aa5b0) x %d rows visible (iPad 8)' % (
            50 * S, INV_PITCH, INV_PITCH * S, INV_COLS, env['ROWS']))
        cl = w['inventory_mc.close_mc']['stage']; bt = w[INV_BTN]['stage']
        say(abs((cl[2] - cl[0]) - INV_CLOSE) < 1 and abs((bt[2] - bt[0]) - INV_BTN_AT[2] * S) < 1, 'close button %.0f px (iPad 50), the doll button %.0f px at %.0f,%.0f (iPad 73)' % (cl[2] - cl[0], bt[2] - bt[0], bt[0], bt[1]))
        sizes = [(n, w['inventory_mc.' + n]['stage']) for n in ('autosortBtn_mc', 'sortByBtn_mc', 'view_mc', 'allBtn_mc')]
        say(all(bx[3] - bx[1] >= 42 and bx[2] <= x1 for _, bx in sizes), 'buttons: ' + ', '.join('%s %.0f x %.0f' % (n[:-3], bx[2] - bx[0], bx[3] - bx[1]) for n, bx in sizes) + ' px; a tab 41 -> %.0f px' % (41 * S))
        t = dis('sortPopup_21', 'okPressed'); say('dmOkOrig' in t and "'autosort'" in t, 'the sort popup: OK = original (setSortFilter; applySortFilters is not in main.raw) + autosort (handled: the sort button\'s call)')
        t = dis('partyInventory_1', 'onContextMenuInputUp'); u = dis('MainTimeline', 'dmCtxUp'); h = dis('MainTimeline', 'dmHook')
        say('dmCtxOrig' in t and 'getTimer' in t and 'onContextMenuInputUp' in u and "'rightMouseUp'" in h, 'long press: right mouse up on the window -> onContextMenuInputUp (openContextMenu, handled by the class: main+0x13a7690), at most once in %d ms with the engine\'s own event' % CTX_GUARD_MS)
        t = dis('partyInventory_1', 'onCellCreate'); say('dmCellOrig' in t and 'tooltipOverrideW' in t and 'scaleX' in t, 'onCellCreate = original + the tooltip rectangle = the cell on the screen (50 x the window scale)')
        geom, wown = inv_geom, INV_W
    else:
        org = w['stats_mc']['origin']; hit = w['stats_mc.hitArea_mc']['stage']
        say(abs(org[0] - x0) < 0.6 and abs(org[1] - y0) < 0.6 and abs(hit[3] - y1) < 1.6 and abs(x1 - x0 - CS_W * S) < 1 and y1 <= STAGE_H + 0.01, 'baked (cell %d): the sheet %.0f,%.0f..%.0f,%.0f = %.0f x %.0f px, x %.2f; iPad -3,0..1165,1068; PC 680 x 1056 at the screen corner' % (
            env['C'], x0, y0, x1, y1, x1 - x0, y1 - y0, S))
        off = [n for n in CS_OFF if not (w['stats_mc.' + n]['origin'][1] < -1000)]
        say(not off, 'the portrait strip\'s content and the drag bar are off the screen: %s' % ' '.join(CS_OFF))
        eq = w['stats_mc.equip_mc']['origin']; p2 = w['stats_mc.dmPanel2']['stage']; p1 = w['stats_mc.panelBg1_mc']['stage']
        say(eq[0] < -1000 and abs((p2[0] - p1[0]) - CS_SPLIT * S) < 1 and abs((p1[3] - p1[1]) - env['PB'] * S) < 1.5 and p1[3] <= y1,
            'two stat columns: list panels %.0f x %.0f px at x %.0f and %.0f; a stat row 26 -> %.0f px (iPad 46); the doll is off the sheet (its own view)' % (p1[2] - p1[0], p1[3] - p1[1], p1[0], p2[0], 26 * S))
        d = cs_geom(CELL0, 1, SCREEN0); mvb = gr.Movie(b); hits = []
        say(abs(d['S'] * CS_DOLL_W - DOLL_PX) < 0.01 and abs(d['H'] * d['S'] - STAGE_H) < 0.01, 'the doll panel (state 1): %.0f x %.0f px, a slot 64 -> %.0f px (iPad: doll 422 x 678 in a 1160 x 1040 window, slot 82)' % (DOLL_PX, d['H'] * d['S'], 64 * d['S']))
        for cls in ('plusButton_62', 'minusButton_65'):
            cid = [k for k, n in mvb.classes.items() if n.endswith('.' + cls)][0]
            st = gr.frame_state(b.tags[_def_index(b, cid)][1], 1); hm = [_mat(r) for r in st.values() if r['name'] == b'hit_mc\0'][0]
            hits.append((hm[0], hm[1], 18 * hm[2], 18 * hm[3]))
        say(all(abs(u - v) < 0.01 for hh in hits for u, v in zip(hh, CS_PM_HIT)), '+ / - buttons: hit area %d x %d own = %.0f x %.0f px (PC 18 x 18; drawing 20 -> %.0f px; iPad 34)' % (
            CS_PM_HIT[2], CS_PM_HIT[3], CS_PM_HIT[2] * S, CS_PM_HIT[3] * S, 20 * S))
        t = dis('equipment_41', 'onDown'); say("'dollDown'" in t, 'the doll turns under the finger: onDown is the PC body (dollDown, handled: main+0x12a59c8); the class stops it on the left button\'s release itself (main+0x12af270, event 1 FlashLeftMouse)')
        t = dis('equipment_41', 'onContextMenuInputUp'); u = dis('MainTimeline', 'dmCtxUp'); h = dis('MainTimeline', 'dmHook')
        say('dmCtxOrig' in t and 'getTimer' in t and 'onContextMenuInputUp' in u and "'rightMouseUp'" in h, 'long press on a doll slot: right mouse up -> equip_mc.onContextMenuInputUp (openContextMenu, handled by the class: main+0x12a56e4), at most once in %d ms' % CTX_GUARD_MS)
        t = dis('MainTimeline', 'dmLay'); say(all(h in t for h, _, _ in CS_LISTS) and t.count('setFrame') == len(CS_LISTS) and t.count('setLength') == len(CS_LISTS) and 'scrollRect' in t,
            'dmLay: the %d tab lists get the panel height and their scroll bars the PC length less the cut; the window clip is cropped by scrollRect (the doll panel = its right half)' % len(CS_LISTS))
        geom, wown = cs_geom, CS_W
    # the fit: every cell the host may ask, every screen, both states
    for st, what in ((0, 'the window'), (1, 'the equipment view (doll + bag)')):
        f = fits(geom, wown, st); bad = [(C, DW) for C, DW, e, ok in f if not ok]
        say(not bad, 'fit, %s: inside the screen with %d px side margins and <= 1080 high for cells %s on %s%s' % (what, MARGIN, ' / '.join(map(str, CELLS)), ' '.join(n for _, n in SCREENS), '' if not bad else ': NOT ' + str(bad)))
        for C in CELLS:
            row = []
            for C2, DW, e, ok in f:
                if C2 != C: continue
                if o == 'partyInventory_p.swf': row.append('%d: cell %.0f, %d rows, %.0f x %.0f' % (DW, 50 * e['S'], e['ROWS'], INV_W * e['S'], e['H'] * e['S']))
                elif st == 0: row.append('%d: row %.0f, %.0f x %.0f' % (DW, 26 * e['S'], CS_W * e['S'], e['H'] * e['S']))
                else: row.append('%d: slot %.0f, pair %.0f wide' % (DW, 64 * e['S'], e['MW']))
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
def picture(src, view, C, DW, grid=True):
    """the screen (DW x 1080, a flat backdrop) with the window(s) of a view the way dmLay places them: 'inv', 'sheet' or 'equip' (the doll panel + the
    bag). The windows are the movies' frame 1 (lists, icons and text are the engine's); with grid the bag's cells are drawn by this tool to show their size."""
    from PIL import Image, ImageDraw
    import dos2_swf_render as rn
    img = Image.new('RGBA', (DW, STAGE_H), (58, 66, 60, 255)); dr = ImageDraw.Draw(img)
    dr.rectangle([0, 0, MARGIN - 1, STAGE_H], fill=(46, 52, 48, 255)); dr.rectangle([DW - MARGIN, 0, DW, STAGE_H], fill=(46, 52, 48, 255))   # the side margins (notch)
    st = 1 if view == 'equip' else 0; note = []
    for o, f in MAKE.items():
        if (view == 'inv' and o != 'partyInventory_p.swf') or (view == 'sheet' and o != 'characterSheet_p.swf'): continue
        dta, rep = f(src[SRC[o]], dict(C=C, ST=st, DW=DW)); e = rep['env']
        swf = Swf(dta)
        if e.get('RX'): edit_places(swf, None, lambda name, rec: _set(rec, dx=-e['RX'] * e['S']) if name == CS_CLIP else None)   # the scrollRect crop, for the picture
        # what the PC code keeps hidden is not in the picture: the sort popup (until "sort by" is tapped), the game master's tabs of the sheet
        hide = ('sortBy_mc',) if o == 'partyInventory_p.swf' else ('invTabHolder_mc', 'customStats_mc', 'skillTabHolder_mc', 'panelBg2_mc')
        edit_places(swf, _root_names(swf)[INV_CLIP if o == 'partyInventory_p.swf' else CS_CLIP]['char'], lambda name, rec: False if name in hide else None)
        pic, org = rn.render(swf, 'root', 1.0); x0 = int(round((DW - e['MW']) / 2.0))
        if e.get('RX'): pic = pic.crop((0, 0, int(e['RW'] * e['S']), pic.size[1]))
        img.alpha_composite(pic, (max(x0, 0), 0)) if x0 >= 0 else img.alpha_composite(pic.crop((-x0, 0, pic.size[0], pic.size[1])), (0, 0))
        if o == 'partyInventory_p.swf':
            S = e['S']; gx = x0 + e['X0'] + (8 + 12) * S; gy = e['Y0'] + (INV_LIST_Y + INV_BAG_HEAD) * S
            if grid:
                for r in range(int(e['ROWS'])):
                    for c in range(INV_COLS):
                        x = gx + c * INV_PITCH * S; y = gy + r * INV_PITCH * S; dr.rectangle([x, y, x + 50 * S, y + 50 * S], outline=(150, 132, 104, 255), fill=(30, 26, 24, 255))
            note.append('bag: cell %.0f px, %d x %d, window %.0f x %.0f' % (50 * S, INV_COLS, e['ROWS'], INV_W * S, e['H'] * S))
        else: note.append(('doll panel %.0f x %.0f, slot %.0f px' if st else 'sheet %.0f x %.0f, row %.0f px') % (e['RW'] * e['S'], e['H'] * e['S'], (64 if st else 26) * e['S']))
    return img, '%s, cell asked %d, screen %d x 1080: %s' % (view, C, DW, '; '.join(note))


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
