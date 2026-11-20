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
              Finger pads: close, "all schools" and the character arrows >= 50 on screen.
  uiCraft_p   uiCraft.swf (680 x 1060 at the top right): centred (setPosition center / screen / center, also on resize), WIDENED to the
              iPad's one-column window (21.11, craft_art): every background bitmap re-cut (two plain column bands stretched; the header keeps
              its end blocks and tab holes whole and gets two wings), every child moved with the art (left / middle / right third) -> 1311 x 1060
              own px, x0.9 = 1180 x 954 px (iPad 1160 x 1080), stage 1220; the bag and the rune inventory x1.689 inside: cell 50 -> 76 on screen,
              pitch 56 -> 85 (invCellSpacing 6; iPad 76 / 86); invCols 13 x 5 rows visible (iPad 13 x 6), invRuneCols 6 (iPad 6) - engine
              variables, so the engine's icons follow. The recipe list scrolls by one finger. The rune tab and the close are now 309 px apart.
              Finger pads: close, character arrows, -/+, search / clear (side by side: taller only), the rune popup's close. Everything else
              is the PC's mouse code (tap, double click = use, drag ingredients, right click = context menu by the host's long press).
  journal_p   journal.swf (1360 x 1064, centred; the map is its tab 3): x0.88 (1197 x 936, screen y 8..944; iPad 1160 x 1040); quest rows
              30 + JN_ROWPAD (45 on screen = the iPad's; same text, the list keeps its width: a scaled list would clip the one-line titles); all
              five lists and the tutorial page's text field scroll by one finger on the text (rows are not draggable). Map: one finger pans (the PC's own drag -> mapOffset), pinch = zoom (the
              host's two fingers = wheel -> zoomIn / zoomOut), tap does NOT add a flag any more (the PC's click did), a long press (the host's
              right click) adds the flag where the finger is (iPad: LongPress = addFlag); zoom buttons x1.4. Finger pads: close, the two
              bottom buttons, the "postponed" check box.

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
# run 21.11: the close buttons' pads 103 / 100 own px = >= 90 screen px at x0.88 / x0.9 (they were 50; the journal's close was missed)
SK_PAD = [('skillPane_mc', 'Xclose_mc', 103, 103), ('skillPane_mc', 'allBtn_mc', 80, 57), ('skillPane_mc', 'leftCycle_mc', 57, 91),
          ('skillPane_mc', 'rightCycle_mc', 57, 91)]   # pane px: x0.88 -> >= 50 screen px
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
CR_PAD = [('craftPanel_mc', 'closeButton_mc', 100, 100), ('craftPanel_mc', 'prevChar_mc', 56, 64), ('craftPanel_mc', 'nextChar_mc', 56, 64),
          ('craftPanel_mc', 'recipesPanel_mc', 'plus_mc', 56, 56), ('craftPanel_mc', 'recipesPanel_mc', 'min_mc', 56, 56),
          ('craftPanel_mc', 'recipesPanel_mc', 'searchBtn_mc', 40, 56), ('craftPanel_mc', 'recipesPanel_mc', 'cancelBtn_mc', 40, 56),
          ('craftPanel_mc', 'runesPanel_mc', 'inventory_mc', 'close_mc', 100, 100)]
PAD_TIGHT = {'searchBtn_mc', 'cancelBtn_mc'}   # the recipe search / clear buttons touch each other (40 px each): their pads only grow in height
# ---- journal (stage 1360 x 1128, anchored center: stage y = screen y + 24)
JN_S = (88, 100)
JN_X0, JN_Y0 = 81.6, 27.6             # journal_mc content 0..1360 x 5..1069 -> centred, top at screen 8
JN_ROWPAD = 21                        # quest rows: 30 -> 51 journal px (45 on screen = the iPad's; 40 before 21.11), same text size and width (scaling the list
                                      # would clip the titles: they are one autoSize line in a 400 px list)
JN_ZOOM = (1170, 821, 1240, 821, 1.4)  # map zoom in / out: x, y in map_mc px (PC 1216 / 1270, 838), scale 43 -> 60 (53 on screen)
JN_PAD = [('journal_mc', 'close_mc', 103, 103), ('journal_mc', 'onMapBtn_mc', 229, 57), ('journal_mc', 'postponeBtn_mc', 229, 57),
          ('journal_mc', 'showPostponed_mc', 370, 57), ('journal_mc', 'tutorialContainer_mc', 'showTutorialPopups_mc', 370, 57)]
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
def skills(data):
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
    for p in SK_PAD: lay.pad(list(p[:-2]), p[-2], p[-1])
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


def craft(data):
    data, art = craft_art(data)
    mv = Movie(data); P, S = mv.P, mv.S
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
    for p in CR_PAD: lay.pad(list(p[:-2]), p[-2], p[-1])
    doc.wrap('onEventInit', 'dmInitOrig', lambda n: lay.end())
    doc.mark()
    return mv.done(), dict(placed=[], defs=art['defs'], methods=mv.made, art=art)


# ================================================================ journal + map
def journal(data):
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
    for p in JN_PAD: lay.pad(list(p[:-2]), p[-2], p[-1])
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


def _put(canvas, swf, cid, x, y, sc, hide=()):
    """the character drawn with its own origin at screen (x, y), scale sc; `hide` = names of its direct children left out"""
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
    img, org = rn.render(swf, cid, sc)
    if sx != sc: img = img.resize((max(1, int(round(img.size[0] * sx / sc))), img.size[1]), Image.LANCZOS)
    canvas.alpha_composite(img, (int(round(x + org[0] * sx)), int(round(y + org[1] * sc))))


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


def mock_skills(src, out, W=2400, bg=None, ipad=None):
    swf = Swf(MAKE['skills_p.swf'](src['skills.swf'])[0])
    cv = _canvas(W, bg); ox, oy = (W - 1354) / 2.0, -20      # stage -> screen
    S = SK_S[0] / float(SK_S[1]); SX = SK_SX
    pane = _sym(swf, ['skillPane_mc'])
    _put(cv, swf, pane, ox + SK_X0, oy + SK_Y0, (SX, S))
    rects = []
    for x0 in (SK_LX, SK_RX):   # 2 lists x rows of SK_COLS skill icons: list header 48 (iggy_grid y), icon 69 at pitch SK_PITCH (skillSize + skillSpacing)
        for row in range(5):
            for col in range(SK_COLS):
                lx, ly = x0 + (18 + col * SK_PITCH) * SK_KX, SK_Y + (48 + row * SK_PITCH) * SK_K
                if ly + 69 * SK_K > SK_Y + SK_LH * SK_K: continue
                rects.append((ox + SK_X0 + SX * lx, oy + SK_Y0 + S * ly, ox + SK_X0 + SX * (lx + 69 * SK_KX), oy + SK_Y0 + S * (ly + 69 * SK_K)))
    _cells(cv, rects, (90, 200, 255, 255))
    _cells(cv, _pads(swf, SK_PAD, lambda x, y: (ox + SK_X0 + SX * x, oy + SK_Y0 + S * y), S), (255, 160, 60, 255))
    cv.save(out); r = rects[0]
    print('%s: skills_p computed layout, icon %.1f x %.1f px at pitch %.1f, book %.0f..%.0f x %.0f..%.0f (screen px)' % (
        out, r[2] - r[0], r[3] - r[1], rects[1][0] - r[0], ox + SK_X0 - 20 * SX, ox + SK_X0 + 1356 * SX, oy + SK_Y0 - 20 * S, oy + SK_Y0 + 1060 * S))
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


def _pads(swf, pads, to_screen, S):
    """screen rectangles of the finger pads: centred on the instance's PC drawing (stage bounds relative to the first path link's origin,
    i.e. the panel's own px: the first link is the panel at 0,0 on the PC stage)"""
    out = []
    for p in pads:
        b = _bounds(swf, list(p[:-2]))
        if not b: continue
        cx, cy = (b[0] + b[2]) / 2.0, (b[1] + b[3]) / 2.0; w, h = p[-2], p[-1]
        x0, y0 = to_screen(cx - w / 2.0, cy - h / 2.0); x1, y1 = to_screen(cx + w / 2.0, cy + h / 2.0); out.append((x0, y0, x1, y1))
    return out


def mock_craft(src, out, W=2400, bg=None, ipad=None):
    swf = Swf(MAKE['uiCraft_p.swf'](src['uiCraft.swf'])[0])
    S = CR_S[0] / float(CR_S[1]); ox, oy = (W - CR_STAGE_W) / 2.0, 10      # setPosition center: stage CR_STAGE_W x 1060 centred
    to = lambda x, y: (ox + CR_X0 + S * x, oy + CR_Y0 + S * y)
    panel = _sym(swf, ['craftPanel_mc']); res = {}
    for tab, hide, cells in (('experiment', ('recipesPanel_mc', 'runesPanel_mc'), (CR_BAG_X, CR_BAG_Y, CR_COLS, CR_BAG)),
                             ('runes', ('recipesPanel_mc', 'experimentPanel_mc'), (32 + CR_EXW / 2.0 + CR_RUNE_X, 172 + 316 + 105, CR_RUNE_COLS, CR_RUNE)),
                             ('recipes', ('experimentPanel_mc', 'runesPanel_mc'), None)):
        if cells is None:   # the recipe tab: the art only (the list is the engine's)
            cv = _canvas(W, bg); _put(cv, swf, panel, ox + CR_X0, oy + CR_Y0, S, hide)
            _cells(cv, _pads(swf, [p for p in CR_PAD if not set(p) & set(hide)], to, S), (255, 160, 60, 255))
            p = out.replace('.png', '_%s.png' % tab); cv.save(p); print(p); continue
        cv = _canvas(W, bg); _put(cv, swf, panel, ox + CR_X0, oy + CR_Y0, S, hide)
        x0, y0, cols, frame = cells; rects = []
        for row in range(20):   # inventoryClass.createCell: x = (cell + spacing) * col - bgDiscrap (-2), same for y; visible inside the frame
            for col in range(cols):
                lx, ly = CR_PITCH * col + 2, CR_PITCH * row + 2
                if ly + 50 > frame[1]: continue
                rects.append(to(x0 + CR_K * lx, y0 + CR_K * ly) + to(x0 + CR_K * (lx + 50), y0 + CR_K * (ly + 50)))
        rects = [(r[0], r[1], r[2], r[3]) for r in rects]
        _cells(cv, rects, (90, 200, 255, 255))
        _cells(cv, _pads(swf, [p for p in CR_PAD if not set(p) & set(hide)], to, S), (255, 160, 60, 255))
        p = out.replace('.png', '_%s.png' % tab); cv.save(p); r = rects[0]
        res[tab] = dict(icon=r[2] - r[0], pitch=rects[1][0] - r[0], cols=cols, rows=len(rects) // cols, right=rects[cols - 1][2])
        print('%s: uiCraft_p %s tab, cell %.1f px at pitch %.1f, %d x %d visible, window %.0f..%.0f x %.0f..%.0f' % (
            p, tab, r[2] - r[0], rects[1][0] - r[0], cols, len(rects) // cols, to(0, 0)[0], to(CR_W, 0)[0], to(0, 0)[1], to(0, 1060)[1]))
    res['window'] = to(0, 0) + to(CR_W, 1060)
    if ipad:
        cv2 = _canvas(W, bg); _ipad(cv2, os.path.join(ipad, 'uiCraft_t.swf'), 1160, 1080, W); p2 = out.replace('.png', '.ipad.png'); cv2.save(p2); print(p2)
    return res


def mock_journal(src, out, W=2400, bg=None, ipad=None):
    swf = Swf(MAKE['journal_p.swf'](src['journal.swf'])[0])
    S = JN_S[0] / float(JN_S[1]); ox, oy = (W - 1360) / 2.0, -24
    to = lambda x, y: (ox + JN_X0 + S * x, oy + JN_Y0 + S * (y + 5))   # journal_mc px (it sits at 0, 5 on the PC stage)
    jm = _sym(swf, ['journal_mc']); res = {}
    for tab, hide in (('quests', ('map_mc', 'tutorialContainer_mc', 'dialogLogContainer_mc', 'mapName_txt')),
                      ('map', ('tutorialContainer_mc', 'dialogLogContainer_mc', 'infoContainer_mc', 'onMapBtn_mc', 'postponeBtn_mc', 'showPostponed_mc',
                               'leftJournalBtn_mc', 'rightJournalBtn_mc', 'paperTop_mc'))):
        cv = _canvas(W, bg); _put(cv, swf, jm, ox + JN_X0, oy + JN_Y0 + 5 * S, S, hide)
        rects = []
        if tab == 'quests':   # journalContainer_mc 64,198 + list y 35; a one-line quest row 30 + JN_ROWPAD, full list width 400
            for k in range(16):
                y = 198 + 35 + k * (30 + JN_ROWPAD)
                if y + 30 + JN_ROWPAD > 198 + 35 + 724: break
                rects.append(to(64 + 20, y) + to(64 + 20 + 380, y + 30 + JN_ROWPAD))
            res[tab] = dict(row=rects[0][3] - rects[0][1], rows=len(rects))
        else:                 # zoom buttons where the added code puts them (map_mc at 16,133), JN_ZOOM px
            for x, y in ((JN_ZOOM[0], JN_ZOOM[1]), (JN_ZOOM[2], JN_ZOOM[3])):
                rects.append(to(16 + x, 133 + y) + to(16 + x + 43 * JN_ZOOM[4], 133 + y + 43 * JN_ZOOM[4]))
            res[tab] = dict(zoom=rects[0][2] - rects[0][0])
        _cells(cv, rects, (90, 200, 255, 255))
        _cells(cv, _pads(swf, [p for p in JN_PAD if not set(p) & set(hide)], lambda x, y: to(x, y - 5), S), (255, 160, 60, 255))
        p = out.replace('.png', '_%s.png' % tab); cv.save(p)
        print('%s: journal_p %s, %s, window %.0f..%.0f x %.0f..%.0f' % (p, tab, res[tab], to(0, 0)[0], to(1360, 0)[0], to(0, 0)[1], to(0, 1064)[1]))
    res['window'] = to(0, 0) + to(1360, 1064)
    if ipad:
        cv2 = _canvas(W, bg); _ipad(cv2, os.path.join(ipad, 'journal_csp_t.swf'), 1160, 1040, W); p2 = out.replace('.png', '.ipad.png'); cv2.save(p2); print(p2)
    return res


def build(src): return {o: MAKE[o](src[OUT[o]])[0] for o in OUT if o in MAKE}


# ================================================================ static checks (also run by tools/dos2_pchud_touch.py check)
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
    if o == 'skills_p.swf':
        return [['skillPane_mc'], ['memoryPageHitRect_mc'], ['dragHit_mc']] + [['skillPane_mc', n] for n in (
            'skillContainer_mc', 'memoryContainer_mc', 'leftPageFrame_mc', 'rightPageFrame_mc')] + [list(p[:-2]) for p in SK_PAD]
    if o == 'uiCraft_p.swf':
        return [['craftPanel_mc'], ['craftPanel_mc', 'experimentPanel_mc', 'container_mc'], ['craftPanel_mc', 'runesPanel_mc', 'inventory_mc', 'inventoryHolder_mc'],
                ['craftPanel_mc', 'recipesPanel_mc']] + [list(p[:-2]) for p in CR_PAD]
    return [['journal_mc'], ['journal_mc', 'map_mc', 'mapContentHolder_mc', 'mapContent_mc'], ['journal_mc', 'map_mc', 'zoomInBtn'],
            ['journal_mc', 'map_mc', 'zoomOutBtn']] + [list(p[:-2]) for p in JN_PAD] + [['journal_mc'] + list(p) for p in JN_TEXTS]


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
    pads = SK_PAD if o == 'skills_p.swf' else CR_PAD if o == 'uiCraft_p.swf' else JN_PAD
    S = (SK_S if o == 'skills_p.swf' else CR_S if o == 'uiCraft_p.swf' else JN_S); S = S[0] / float(S[1])
    tight = [p for p in pads if p[-3] in PAD_TIGHT]
    say(all(min(p[-2], p[-1]) * S >= 50 or (p in tight and p[-1] * S >= 50) for p in pads) and dis(doc + '::onEventInit').count('drawRect') == len(pads),
        '%d finger pads, each >= 50 px on screen%s' % (len(pads), '' if not tight else ' (%s: %.0f wide - side by side, 40 px apart: only taller)' % (
            ', '.join(p[-3] for p in tight), min(p[-2] for p in tight) * S)))


def tag_filter(o, g):
    """tools/dos2_swf_graft.py check messages that are by design: uiCraft_p's widened art changes the stage size and re-places frame 1 of the
    panel sprites (DefineSprite, code 39); also used by tools/dos2_pchud_touch.py check"""
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
        g = tag_filter(o, gr.check(data, out, quiet=True))
        say(g == [], 'every original tag kept (uiCraft_p: but the re-placed sprites and the stage), ids unique, uses after definitions%s' % ('' if not g else ': ' + str(g)))
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
