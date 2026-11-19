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
                       [CROP .. 12 slots] (the PC portrait and the six panel buttons left of it are cut: the panels are dmSide_p's column),
                       slots centred 20 px left of the screen centre with their top at screen y 967 (iPad 216..1244 of a 1500 stage, y 967..1049);
                       chat button left of the bar, log button right of it, both enlarged; ExternalInterface updateSlots(12)
      onEventResolution(w, h) -> dmLay() (the PC code sized the bar from the screen width: up to 29 slots)
      onEventInit()    the original, then dmLay() and dmHook(): a horizontal swipe over the slots (> SWIPE stage px, not while a slot is
                       dragged) = previous / next deck through the PC cycleHotBar (prevHotbar / nextHotbar); the deck arrows get a finger-sized
                       hit area; the CONFIRM circle and the cancel button (root instances dmConfirm_mc / dmCancel_mc at the iPad's place)
      showActiveSkill(slot)  the original, then CONFIRM / cancel are shown while the engine marks a prepared skill (slot >= 0) and
                       ExternalInterface dmAim(0 | 1) tells the host; a tap on them = dmAimBtn(1 confirm | 0 cancel) (host: the PC's left
                       click at the aimed point / right click = cancel)
      HUD extras ("Implemented: touch HUD extras"): the deck list - dmDeck_mc (above the deck number) opens / closes five plates dmDeck1..5_mc
                       (k pips = deck k), a tap on one runs the PC cycleHotBar as many times as it takes (nextHotbar) and closes the list; the
                       deck indicator dmInd1..5_mc under the slots (the current deck's bar is bright; followed on enterFrame from the PC's
                       currentHotBarIndex). The ACTION PLATE: showActiveSkill(2000000 + stageX * 2000 + stageY) from the host (a world tap on
                       an object) puts the CONFIRM circle PLATE_UP px above that point, 1999999 hides it; a tap on it = dmAimBtn(1) as CONFIRM.
      Everything else is the PC movie's own mouse code: tap a slot = SlotPressed, hover (long press = the host's right click / still finger)
      = tooltip, drag from a slot = startDragging / stopDragging (unlocked bar), lock, actions list, chat, combat log buttons.
  minimap_p.swf        minimap.swf: the ring group moved (-42, +29), ping (-4, -61), home / diplomacy above the ring (TOUCH_LAYOUT 2.3);
                       a tap on the ring (press and release on hit_mc) = ExternalInterface dmSide(2) = the map; two of the iPad's buttons:
                       dmMmTac_mc (the tactical camera, where the iPad's view-menu button sits) = dmKey(2), dmMmSave_mc (quick save) = dmKey(1)
  statusConsole_p.swf  statusConsole.swf: console / fight buttons / turn notice / GM button raised by SC_UP more (the hot bar is taller):
                       health bar at screen y 943 as the iPad's
  combatLog_p.swf      combatLog.swf opened at the screen centre as the iPad's (setPosition center / screen / center in onEventInit and
                       onEventResize instead of bottomright); the PC window, its drag, resize, filters and wheel scroll are untouched
  playerInfo_p.swf     the PC movie as it is (the portraits' 1.1 is the host's class scale; their PC mouse code is the
                       iPad's: tap = select, double = centre, right click = inventory, drag = regroup). Own names so a later touch edit needs
                       no host change.
  dmSide_p.swf         watermark.swf (UI type 0x8d, a plain UIObject) + the iPad's right side (rightSideButtons_t numbers, 2.4): frame
                       SIDE_W x 1080 anchored right / screen / right; options (plate_round_copper + icon_menu), the round HELMET button
                       (btn_character; toggles the column locally: the iPad's toggleSidePanel / setRolledIn pair is not in the Switch main),
                       six panel buttons 74 px every 80 px from y 422: map, character, inventory, skills, crafting, journal.
                       A tap = ExternalInterface dmSide(code): 1 options, 2..7 the panels in that order.
  Panels (stage 4, "Implemented: touch panels"): partyInventory_p characterSheet_p skills_p uiCraft_p journal_p examine_p waypoints_p = the PC
                       movies with one addition, the iPad's 50 px close button: the PC's 23 px close button gets a transparent 50 x 50 hit area
                       (PANEL_CLOSE: where it sits in each movie). Size, position and every other gesture are the PC movie's own (tap, double
                       click = use / equip, right click = context menu, drag, wheel): see the plan for why the iPad's 76 px cells are not here.

  python -I tools/dos2_pchud_touch.py build <Switch Game.pak | dir of the PC movies> <out dir>
  python -I tools/dos2_pchud_touch.py check <Switch Game.pak | dir of the PC movies>
  python -I tools/dos2_pchud_touch.py mock  <Switch Game.pak | dir> <out.png> [--w 2400]   the computed layout drawn from the movies' own
        symbols at the positions the patched code sets (a picture of the NUMBERS, not a played movie)
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, NS_PUBLIC
import dos2_swf_graft as gr
import dos2_touchart as ta

GUI = 'Public/Game/GUI/'
# ---- hot bar (PC stage 1948 x 1000, anchored bottom: stage x = screen x - (W - 1948) / 2, stage y = screen y - 80)
HB_S = (1562, 1000)              # hotbar_mc scale: 86 / (58 x 0.9494)
SLOTS = 12
PC_FIRST, PC_PITCH, PC_SLOT_W, PC_SLOT_Y, PC_SLOT_H = 339.0, 58 * 0.9494, 50 * 0.9494, 64.0, 50.0   # hotbar_mc px (slotholder_mc 339,64 scaleX 0.9494)
CROP = 196                       # hotbar_mc px cut on the left (portrait 16..76, panel buttons 75..225); actions 255, lock 295, deck 304..
VISW = int(PC_FIRST + (SLOTS - 1) * PC_PITCH + PC_SLOT_W + 29) - CROP   # ..to 29 px past the 12th slot
VISH = 180
IPAD_SLOT_MID_X, IPAD_SLOT_BOTTOM = 730 - 750, 1049                    # of the screen centre / screen y (hotBar_t)
_s = HB_S[0] / float(HB_S[1])
HB_X = int(round(974 + IPAD_SLOT_MID_X - ((PC_FIRST + ((SLOTS - 1) * PC_PITCH + PC_SLOT_W) / 2.0) - CROP) * _s))
HB_Y = int(round(IPAD_SLOT_BOTTOM - 80 - (PC_SLOT_Y + PC_SLOT_H) * _s))
CHAT = (185, 38, (13, 10))       # chatBtn_mc x, y in hotbar_mc px, scale 1.3 (its 31 px button -> 63 screen px, left of the actions button)
LOG_S = (2, 1)                   # showLog_mc scale (31 px button -> 62)
LOG_X, LOG_Y = HB_X + int(VISW * _s) + 8 - 46, int(HB_Y + (PC_SLOT_Y + PC_SLOT_H / 2.0) * _s) - 31 - 50
ACT_X, ACT_Y = int(HB_X + (272 - CROP) * _s), int(HB_Y + 57.5 * _s + 11 * _s - 144 * _s)   # actionSkillHolder_mc: its foot on the bar's top
EXP_SX = (49, 100)               # expBar_mc.scaleX: ends near the last slot
SWIPE = 130                      # stage px (1.5 slots)
CONFIRM = (974, 788 - 80, 118)   # centre x, y, size: statusConsole_t confirmActionBtn_mc centre (W/2, 788), 118
CANCEL = (974 + 201, 816 - 80, 69)   # cancelBtn_mc centre (W/2 + 201, 816), 69
DECK_BTN = (396, 836, 44)        # dmDeck_mc: x, y, size (just above the deck number of the bar)
DECK_LIST = (396, 778, 52, 58)   # dmDeck1..5_mc: x of the first, y, size, pitch
IND = (750, 988, 80, 5, 82)      # dmInd1..5_mc: x of the first, y, bar w, h, pitch (hotBar_t indicator_mc: 5 bars 80 x 5 every 82)
PLATE_UP = 110                   # the action plate's centre above the tapped point
PLATE_BASE, PLATE_HIDE, PLATE_MUL = 2000000, 1999999, 2000
# ---- minimap (PC stage 420 x 2000, anchored topright)
MM_MOVE = [('hit_mc', -42, 29), ('map_mc', -42, 29), ('tension_mc', -42, 29), ('north_mc', -42, 29), ('armourDoll_mc', -42, 29),
           ('tensionHit_mc', -42, 29), ('title_txt', -42, 29), ('pingButton_mc', -4, -61), ('homestead_mc', 136 - 111, 47 - 94),
           ('diplomacy_mc', 268 - 111, 47 - 178)]
MM_BTNS = [('dmMmTac_mc', 12, 'icon_tactical', 420 - 378, 154, 67), ('dmMmSave_mc', 11, None, 420 - 89, 163, 48)]   # name, code (10 + dmKey), art, x, y, size
# ---- status console (PC stage 1920 x 1080, anchored bottom; its own code already raises by bottomBarHeight 67)
SC_UP = 33                       # health bar 1043 - 67 - 33 = 943 (statusConsole_t: 943)
SC_MOVE = ['console_mc', 'fightButtons_mc', 'turnNotice_mc', 'toGM_mc']
# ---- the right side (rightSideButtons_t: stage 1440 x 1080 anchored right; here the last SIDE_W px of it)
SIDE_W, SIDE_H = 140, 1080
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


def sources(path):
    """name -> bytes of the PC movies this pass needs (a Switch Game.pak or a directory of extracted movies)"""
    need = sorted(set(OUT.values()))
    if os.path.isdir(path): return {n: (Path(path) / n).read_bytes() for n in need}
    import lspk
    pak = lspk.Pak(path); by = {e[0]: e for e in pak.entries}
    miss = [n for n in need if GUI + n not in by]
    if miss: sys.exit('%s: no %s' % (path, ' '.join(miss)))
    return {n: pak.read(by[GUI + n]) for n in need}


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
def hotbar_art(swf):
    g = gr.Graft(swf); art = ta.Art(g); placed = []
    rp, rb = art.png('plate_round_copper'); pw = rb[2] - rb[0]
    k = CONFIRM[2] / pw
    tick, tb = ta.poly_shape(g, [(-22, 2), (-13, -8), (-5, 2), (17, -21), (26, -11), (-5, 21)], (236, 226, 200))   # our own check mark (vec)
    c = g.sprite([(rp, 1, (0, 0, k), None), (tick, 2, (CONFIRM[2] / 2.0, CONFIRM[2] / 2.0), None)])
    g.place(c, CONFIRM[0] - CONFIRM[2] / 2.0, CONFIRM[1] - CONFIRM[2] / 2.0, 'dmConfirm_mc'); placed.append(('dmConfirm_mc', CONFIRM))
    sp, sb = art.png('plate_round_steel', centre=True); ic, ib = art.png('glyph_cancel', centre=True)
    x = g.sprite([(sp, 1, (0, 0), None), (ic, 2, (0, 0), None)])
    g.place(x, CANCEL[0], CANCEL[1], 'dmCancel_mc', CANCEL[2] / (sb[2] - sb[0])); placed.append(('dmCancel_mc', CANCEL))
    # the deck list: our plates (assets/touchui) + pips / bars drawn in code
    sq, qb = art.png('plate_square_copper'); qw = qb[2] - qb[0]; ink = (236, 226, 200)
    def rect(x0, y0, w, h, rgb=ink): return ta.poly_shape(g, [(x0, y0), (x0 + w, y0), (x0 + w, y0 + h), (x0, y0 + h)], rgb)[0]
    def pip(cx, cy, r=5): return ta.poly_shape(g, [(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)], ink)[0]
    bx, by, bs = DECK_BTN
    g.place(g.sprite([(sq, 1, (0, 0, bs / qw), None)] + [(rect(bs * 0.25, bs * (0.28 + 0.19 * i), bs * 0.5, bs * 0.09), 2 + i, (0, 0), None) for i in range(3)]), bx, by, 'dmDeck_mc')
    placed.append(('dmDeck_mc', DECK_BTN))
    lx, ly, ls, lp = DECK_LIST
    for k in range(1, 6):
        xs = [ls / 2.0 + (i - (k - 1) / 2.0) * 9 for i in range(k)]
        g.place(g.sprite([(sq, 1, (0, 0, ls / qw), None)] + [(pip(xv, ls / 2.0, 3.5), 2 + i, (0, 0), None) for i, xv in enumerate(xs)]), lx + lp * (k - 1), ly, 'dmDeck%d_mc' % k)
    ix, iy, iw, ih, ip = IND
    for k in range(1, 6): g.place(g.sprite([(rect(0, 0, iw, ih), 1, (0, 0), None)]), ix + ip * (k - 1), iy, 'dmInd%d_mc' % k)
    placed.append(('dmDeck1..5_mc', DECK_LIST)); placed.append(('dmInd1..5_mc', IND))
    g.finish()
    return placed, len(g.new_defs)


def hotbar(data):
    swf = Swf(data); placed, ndefs = hotbar_art(swf); d = Doc(swf); P, S, abc = d.P, d.S, d.abc
    Rect = abc.pub('Rectangle', 'flash.geom')
    def setn(loc, prop, val): return [('getlocal', loc)] + val + [('setproperty', P(prop))]
    # dmLay(): locals 1 hotbar_mc, 2 a child
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('pushnull',), ('coerce_a',), ('setlocal', 2)]
    code += d.get('hotbar_mc') + [('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'end')]
    code += setn(1, 'scaleX', frac(HB_S)) + setn(1, 'scaleY', frac(HB_S))
    code += [('getlocal', 1), ('findpropstrict', Rect)] + num(CROP) + num(0) + num(VISW) + num(VISH) + [('constructprop', Rect, 4), ('setproperty', P('scrollRect'))]
    code += setn(1, 'x', num(HB_X)) + setn(1, 'y', num(HB_Y))
    code += [('getlocal', 1), ('getproperty', P('chatBtn_mc')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'nc')]
    code += setn(2, 'x', num(CHAT[0])) + setn(2, 'y', num(CHAT[1])) + setn(2, 'scaleX', frac(CHAT[2])) + setn(2, 'scaleY', frac(CHAT[2])) + [('L', 'nc')]
    code += [('getlocal', 1), ('getproperty', P('expBar_mc')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'ne')] + setn(2, 'scaleX', frac(EXP_SX)) + [('L', 'ne')]
    code += d.get('showLog_mc') + [('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'nl')]
    code += setn(2, 'scaleX', frac(LOG_S)) + setn(2, 'scaleY', frac(LOG_S)) + setn(2, 'x', num(LOG_X)) + setn(2, 'y', num(LOG_Y)) + [('L', 'nl')]
    code += d.get('endPiece_mc') + [('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'np'), ('getlocal', 2), ('pushfalse',), ('setproperty', P('visible')), ('L', 'np')]
    code += d.get('actionSkillHolder_mc') + [('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'na')]
    code += setn(2, 'scaleX', frac(HB_S)) + setn(2, 'scaleY', frac(HB_S)) + setn(2, 'x', num(ACT_X)) + setn(2, 'y', num(ACT_Y)) + [('L', 'na')]
    code += d.ei('updateSlots', num(SLOTS)) + [('L', 'end'), ('returnvoid',)]
    d.method('dmLay', 0, code)
    # the swipe over the slots: dmSwDown(e) / dmSwUp(e); locals 2 stage, 3 dx
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2)] + d.get('stage') + [('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', 2), ('getproperty', P('mouseX')), ('setproperty', P('dmSwX')),
        ('getlocal', 2), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmSwUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)]
    d.method('dmSwDown', 1, code)
    def cycle(prev): return d.get('hotbar_mc', 'cycleHotBar_mc') + [('pushtrue',) if prev else ('pushfalse',), ('newarray', 1), ('callpropvoid', P('cycleHotBar'), 1)]
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushbyte', 0), ('convert_d',), ('setlocal', 3)] + d.get('stage') + [('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmSwUp')), ('callpropvoid', P('removeEventListener'), 2),
        ('getlocal', 2), ('getproperty', P('mouseX'))] + d.get('dmSwX') + [('subtract',), ('convert_d',), ('setlocal', 3)] + d.get('isDragging') + [('iftrue', 'end'),
        ('getlocal', 3)] + num(SWIPE) + [('ifngt', 'left')] + cycle(True) + [('returnvoid',),
        ('L', 'left'), ('getlocal', 3)] + num(-SWIPE) + [('ifnlt', 'end')] + cycle(False) + [('L', 'end'), ('returnvoid',)]
    d.method('dmSwUp', 1, code)
    # dmBAct(code): 30 = the deck list open / closed, 31..35 = that deck, 1 / 0 = CONFIRM (also the action plate) / cancel. locals 2 steps, 3 mc
    idx = d.get('hotbar_mc', 'cycleHotBar_mc', 'currentHotBarIndex')
    act = [('pushbyte', 0), ('convert_i',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
           ('getlocal1',), ('pushbyte', 30), ('ifne', 'pick'),
           ('getlocal0',)] + d.get('dmDeckOpen') + [('not',), ('setproperty', P('dmDeckOpen')), ('getlocal0',), ('callpropvoid', P('dmDeckShow'), 0), ('returnvoid',),
           ('L', 'pick'), ('getlocal1',), ('pushbyte', 30), ('ifngt', 'aim'),
           ('getlocal1',), ('pushbyte', 30), ('subtract',)] + idx + [('subtract',), ('pushbyte', 5), ('add',), ('pushbyte', 5), ('modulo',), ('convert_i',), ('setlocal', 2)]
    for k in range(1, 5): act += [('getlocal', 2), ('pushbyte', k), ('iflt', 'pd')] + cycle(False)
    act += [('L', 'pd'), ('getlocal0',), ('pushfalse',), ('setproperty', P('dmDeckOpen')), ('getlocal0',), ('callpropvoid', P('dmDeckShow'), 0), ('returnvoid',),
            ('L', 'aim')] + d.get('dmPlate') + [('iffalse', 'send'), ('getlocal1',), ('pushbyte', 1), ('ifne', 'send'),
            ('getlocal0',), ('pushfalse',), ('setproperty', P('dmPlate'))] + d.child('dmConfirm_mc', 3) + [('getlocal', 3), ('iffalse', 'send'),
            ('getlocal', 3)] + d.get('dmAimV') + [('convert_b',), ('setproperty', P('visible')),
            ('L', 'send')] + d.ei('dmAimBtn', [('getlocal1',)])
    press_code(d, act)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1)]
    for k in range(1, 6):
        code += d.child('dmDeck%d_mc' % k, 1) + [('getlocal', 1), ('iffalse', 's%d' % k), ('getlocal', 1)] + d.get('dmDeckOpen') + [('convert_b',), ('setproperty', P('visible')), ('L', 's%d' % k)]
    d.method('dmDeckShow', 0, code + [('returnvoid',)])
    # dmTick(e): the indicator follows the PC's current deck. locals 2 current, 3 mc
    code = HEAD + [('pushbyte', 0), ('convert_i',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3)] + d.get('hotbar_mc') + [('iffalse', 'end')] + d.get(
        'hotbar_mc', 'cycleHotBar_mc') + [('iffalse', 'end')] + idx + [('convert_i',), ('setlocal', 2), ('getlocal', 2)] + d.get('dmIndCur') + [('ifstricteq', 'end'),
        ('getlocal0',), ('getlocal', 2), ('setproperty', P('dmIndCur'))]
    for k in range(1, 6):
        code += d.child('dmInd%d_mc' % k, 3) + [('getlocal', 3), ('iffalse', 'i%d' % k), ('getlocal', 3), ('pushbyte', 3), ('pushbyte', 10), ('divide',),
            ('getlocal', 2), ('pushbyte', k), ('ifne', 'j%d' % k), ('pop',), ('pushbyte', 1), ('L', 'j%d' % k), ('setproperty', P('alpha')), ('L', 'i%d' % k)]
    d.method('dmTick', 1, code + [('L', 'end'), ('returnvoid',)])
    # dmHook(): once. locals 1 object, 2 graphics
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('pushnull',), ('coerce_a',), ('setlocal', 2)] + d.get('dmOn') + [('iftrue', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmOn'))]
    code += d.get('hotbar_mc', 'slotholder_mc') + [('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'ns'),
        ('getlocal', 1), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmSwDown')), ('callpropvoid', P('addEventListener'), 2), ('L', 'ns')]
    for k, (btn, ry) in enumerate((('upBtn_mc', -14), ('downBtn_mc', 0))):   # a finger-sized (50 x 47 screen px) transparent hit area on each deck arrow
        code += d.get('hotbar_mc', 'cycleHotBar_mc', btn) + [('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'h%d' % k),
            ('getlocal', 1), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', 2),
            ('getlocal', 2), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', P('beginFill'), 2),
            ('getlocal', 2)] + num(-16) + num(ry) + num(32) + num(30) + [('callpropvoid', P('drawRect'), 4),
            ('getlocal', 2), ('callpropvoid', P('endFill'), 0), ('L', 'h%d' % k)]
    for k, (name, c) in enumerate((('dmConfirm_mc', 1), ('dmCancel_mc', 0))):
        code += d.child(name, 1) + [('getlocal', 1), ('iffalse', 'b%d' % k)] + arm(d, 1, c) + [('getlocal', 1), ('pushfalse',), ('setproperty', P('visible')), ('L', 'b%d' % k)]
    code += d.child('dmDeck_mc', 1) + [('getlocal', 1), ('iffalse', 'nd')] + arm(d, 1, 30) + [('L', 'nd')]
    for k in range(1, 6):
        code += d.child('dmDeck%d_mc' % k, 1) + [('getlocal', 1), ('iffalse', 'dk%d' % k)] + arm(d, 1, 30 + k) + [('getlocal', 1), ('pushfalse',), ('setproperty', P('visible')), ('L', 'dk%d' % k)]
        code += d.child('dmInd%d_mc' % k, 1) + [('getlocal', 1), ('iffalse', 'ik%d' % k), ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseEnabled')),
                                                    ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseChildren')), ('L', 'ik%d' % k)]
    code += [('getlocal0',), ('pushfalse',), ('setproperty', P('dmDeckOpen')),
             ('getlocal0',), ('pushstring', S('enterFrame')), ('getlocal0',), ('getproperty', P('dmTick')), ('callpropvoid', P('addEventListener'), 2)]
    code += [('L', 'end'), ('returnvoid',)]
    d.method('dmHook', 0, code)
    d.wrap('onEventInit', 'dmInitOrig', lambda n: HEAD + d.call_orig('dmInitOrig', n) + [('getlocal0',), ('callpropvoid', P('dmLay'), 0), ('getlocal0',), ('callpropvoid', P('dmHook'), 0), ('returnvoid',)])
    d.wrap('onEventResolution', 'dmResOrig', lambda n: HEAD + [('getlocal0',), ('callpropvoid', P('dmLay'), 0), ('returnvoid',)])
    # showActiveSkill(slot): the original; CONFIRM / cancel follow "a skill is prepared"; the host hears every change. locals: n+1 shown, n+2 mc
    def sas(n):
        a, m = n + 1, n + 2
        v = n + 3
        c = HEAD + [('pushfalse',), ('setlocal', a), ('pushnull',), ('coerce_a',), ('setlocal', m), ('pushbyte', 0), ('convert_i',), ('setlocal', v)]
        # the host's action plate (never a slot number): PLATE_HIDE, or PLATE_BASE + stageX * PLATE_MUL + stageY
        c += [('getlocal1',), ('pushint', d.abc_int(PLATE_HIDE)), ('ifnge', 'slot')] + d.child('dmConfirm_mc', m) + [('getlocal', m), ('iffalse', 'pe'),
              ('getlocal1',), ('pushint', d.abc_int(PLATE_HIDE)), ('ifne', 'pshow'),
              ('getlocal0',), ('pushfalse',), ('setproperty', P('dmPlate')), ('getlocal', m)] + d.get('dmAimV') + [('convert_b',), ('setproperty', P('visible')), ('returnvoid',),
              ('L', 'pshow'), ('getlocal1',), ('pushint', d.abc_int(PLATE_BASE)), ('subtract',), ('convert_i',), ('setlocal', v),
              ('getlocal', m), ('getlocal', v)] + num(PLATE_MUL) + [('divide',), ('convert_i',)] + num(CONFIRM[2] // 2) + [('subtract',), ('setproperty', P('x')),
              ('getlocal', m), ('getlocal', v)] + num(PLATE_MUL) + [('modulo',)] + num(CONFIRM[2] // 2 + PLATE_UP) + [('subtract',), ('setproperty', P('y')),
              ('getlocal', m), ('pushtrue',), ('setproperty', P('visible')), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmPlate')),
              ('L', 'pe'), ('returnvoid',), ('L', 'slot')]
        c += d.call_orig('dmSasOrig', n) + [('getlocal1',), ('pushbyte', 0), ('greaterequals',), ('setlocal', a), ('getlocal0',), ('pushfalse',), ('setproperty', P('dmPlate'))]
        for k, name in enumerate(('dmConfirm_mc', 'dmCancel_mc')):
            c += d.child(name, m) + [('getlocal', m), ('iffalse', 'v%d' % k), ('getlocal', m), ('getlocal', a), ('setproperty', P('visible'))]
            if not k: c += [('getlocal', m)] + num(CONFIRM[0] - CONFIRM[2] // 2) + [('setproperty', P('x')), ('getlocal', m)] + num(CONFIRM[1] - CONFIRM[2] // 2) + [('setproperty', P('y'))]
            c += [('L', 'v%d' % k)]
        c += d.get('dmAimV') + [('getlocal', a), ('ifstricteq', 'end'), ('getlocal0',), ('getlocal', a), ('setproperty', P('dmAimV')),
            ('getlocal', a), ('iffalse', 'off')] + d.ei('dmAim', num(1)) + [('returnvoid',), ('L', 'off')] + d.ei('dmAim', num(0)) + [('L', 'end'), ('returnvoid',)]
        return c
    d.wrap('showActiveSkill', 'dmSasOrig', sas)
    d.done()
    return swf.write(), dict(placed=placed, defs=ndefs, methods=d.made)


# ---------------------------------------------------------------- minimap, status console
def moved(data, moves, tap=None, btns=()):
    """onEventInit: the original, then named root instances moved by (dx, dy); tap = (instance, dmSide code) for a press-and-release button;
    btns = MM_BTNS-like grafted buttons (code 10 + n = ExternalInterface dmKey(n))"""
    swf = Swf(data); nd = 0
    if btns:
        g = gr.Graft(swf); art = ta.Art(g); rp, rb = art.png('plate_round_copper'); pw = rb[2] - rb[0]; ink = (236, 226, 200)
        for name, code, icon, x, y, size in btns:
            k = size / pw; kids = [(rp, 1, (0, 0, k), None)]
            if icon: ic, ib = art.png(icon); ki = k * 0.9; kids.append((ic, 2, (size / 2.0 - (ib[2] - ib[0]) * ki / 2.0, size / 2.0 - (ib[3] - ib[1]) * ki / 2.0, ki), None))
            else:   # quick save: our own glyph drawn in code - an arrow down into a tray
                u = size / 48.0; P2 = lambda pts: ta.poly_shape(g, [(a * u, b * u) for a, b in pts], ink)[0]
                kids += [(P2([(21, 12), (27, 12), (27, 22), (32, 22), (24, 30), (16, 22), (21, 22)]), 2, (0, 0), None), (P2([(14, 32), (34, 32), (34, 36), (14, 36)]), 3, (0, 0), None)]
            g.place(g.sprite(kids), x, y, name)
        g.finish(); nd = len(g.new_defs)
    d = Doc(swf); P = d.P
    # the ring: not in ping mode (there the PC code's own press sends minimapPing and leaves the mode; our listener was added first, so dmNote = the mode at the press)
    if tap: press_code(d, [('getlocal1',), ('pushbyte', 10), ('ifngt', 'side')] + d.ei('dmKey', [('getlocal1',), ('pushbyte', 10), ('subtract',)]) + d.ei(
        'PlaySound', [('pushstring', d.S('UI_Generic_Click'))]) + [('returnvoid',), ('L', 'side')] + d.get('dmNote') + [('iftrue', 'ping')] + d.ei('dmSide', [('getlocal1',)]) + [('L', 'ping')],
        dim=False, note='pingModeEnabled')
    def init(n):
        c = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', n + 1)] + d.call_orig('dmInitOrig', n)
        for k, (name, dx, dy) in enumerate(moves):
            c += d.get(name) + [('coerce_a',), ('setlocal', n + 1), ('getlocal', n + 1), ('iffalse', 'm%d' % k)]
            for prop, v in (('x', dx), ('y', dy)):
                if v: c += [('getlocal', n + 1), ('getlocal', n + 1), ('getproperty', P(prop))] + num(v) + [('add',), ('setproperty', P(prop))]
            c += [('L', 'm%d' % k)]
        if tap: c += d.get(tap[0]) + [('coerce_a',), ('setlocal', n + 1), ('getlocal', n + 1), ('iffalse', 'nt')] + arm(d, n + 1, tap[1], kids=True) + [('L', 'nt')]
        for k, b in enumerate(btns): c += d.child(b[0], n + 1) + [('getlocal', n + 1), ('iffalse', 'bt%d' % k)] + arm(d, n + 1, b[1]) + [('L', 'bt%d' % k)]
        return c + [('returnvoid',)]
    assert d.abc.trait(d.ci, 'dmHook') is None
    d.wrap('onEventInit', 'dmInitOrig', init)
    d.inst['traits'].append(Trait(P('dmHook'), 1, 0, d.add('dmHook', 0, HEAD + [('returnvoid',)])))   # the "already patched" mark
    d.done()
    return swf.write(), dict(placed=[(b[0], b[3:]) for b in btns], defs=nd, methods=d.made)


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


def minimap(data): return moved(data, MM_MOVE, ('hit_mc', 2), MM_BTNS)


def combatlog(data):
    """the iPad's place: the window opens at the screen centre (onEventInit after the original, onEventResize instead of the original's bottomright)"""
    swf = Swf(data); d = Doc(swf); P, S = d.P, d.S
    centre = d.ei('setPosition', [('pushstring', S('center'))], [('pushstring', S('screen'))], [('pushstring', S('center'))])
    d.wrap('onEventInit', 'dmInitOrig', lambda n: HEAD + d.call_orig('dmInitOrig', n) + centre + [('returnvoid',)])
    d.wrap('onEventResize', 'dmResizeOrig', lambda n: HEAD + d.get('log_mc', 'moved') + [('iftrue', 'end')] + centre + [('L', 'end'), ('returnvoid',)])
    d.inst['traits'].append(Trait(P('dmHook'), 1, 0, d.add('dmHook', 0, HEAD + [('returnvoid',)])))
    d.done()
    return swf.write(), dict(placed=[], defs=0, methods=d.made)
def status(data): return moved(data, [(n, 0, -SC_UP) for n in SC_MOVE])
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
        'setAnchor', [('pushstring', S('right'))], [('pushstring', S('screen'))], [('pushstring', S('right'))]) + [('getlocal0',), ('callpropvoid', P('dmHook'), 0), ('returnvoid',)])
    d.done()
    return swf.write(), dict(placed=placed, defs=ndefs, methods=d.made)


def _closer(name): return lambda data: closer(data, PANEL_CLOSE[name])
MAKE = {'hotBar_p.swf': hotbar, 'minimap_p.swf': minimap, 'statusConsole_p.swf': status, 'playerInfo_p.swf': same, 'combatLog_p.swf': combatlog, 'dmSide_p.swf': side}
MAKE.update({n + '_p.swf': _closer(n) for n in PANEL_CLOSE})


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
        b = Swf(out); (tb, pb, B), = b.abcs()
        errs = verify_all(B)
        for who, why in errs: print('       %s: %s' % (who, why))
        say(not errs, 'ABC verifies: %d method bodies' % len(B.bodies))
        say(Swf(Swf(out).write()).body() == b.body(), 'the movie parses and round-trips')
        g = gr.check(data, out, quiet=True); want = ['header / tail differ'] if o == 'dmSide_p.swf' else []
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
            say(HB_X >= 14 and HB_X + VISW * _s + 80 <= 1934, 'the bar %d..%.0f + the log button stay inside a 16:9 screen (stage 14..1934)' % (HB_X, HB_X + VISW * _s))
            say('dmLay' in dis('onEventResolution') and 'dmInitOrig' in dis('onEventInit') and 'dmHook' in dis('onEventInit'), 'onEventInit = original + dmLay + dmHook; onEventResolution = dmLay')
            t = dis('dmSwUp'); say('cycleHotBar' in t and 'isDragging' in t, 'swipe over the slots (%d px, not while dragging) -> cycleHotBar' % SWIPE)
            t = dis('showActiveSkill'); say('dmSasOrig' in t and "'dmAim'" in t and 'visible' in t, 'showActiveSkill: original, CONFIRM / cancel visible, dmAim(0 | 1)')
            say("'dmAimBtn'" in dis('dmBAct') and all((n.encode() + b'\0') in names for n in ('dmConfirm_mc', 'dmCancel_mc')), 'CONFIRM %s / cancel %s on the root, tap -> dmAimBtn(1 | 0)' % (CONFIRM, CANCEL))
            t = dis('dmBAct'); say('dmDeckShow' in t and 'cycleHotBar' in t and all((('dmDeck%d_mc' % k).encode() + b'\0') in names and (('dmInd%d_mc' % k).encode() + b'\0') in names for k in range(1, 6))
                                   and (b'dmDeck_mc\0') in names and 'currentHotBarIndex' in dis('dmTick'), 'deck list (button, 5 plates -> cycleHotBar) and the deck indicator (5 bars, enterFrame)')
            t = dis('showActiveSkill'); say('dmPlate' in t and str(PLATE_BASE) in t and str(PLATE_HIDE) in t, 'action plate: showActiveSkill(%d + x * %d + y) puts CONFIRM %d px above the point, %d hides' % (PLATE_BASE, PLATE_MUL, PLATE_UP, PLATE_HIDE))
        elif o == 'combatLog_p.swf':
            t = dis('onEventInit'); say('dmInitOrig' in t and "'center'" in t and "'center'" in dis('onEventResize'), 'onEventInit = original + setPosition center / screen / center; onEventResize the same unless moved')
        elif o == 'minimap_p.swf':
            say("'dmKey'" in dis('dmBAct') and all((b[0].encode() + b'\0') in names for b in MM_BTNS), 'tactical camera / quick save buttons on the root -> dmKey(2 | 1)')
            t = dis('onEventInit'); say('dmInitOrig' in t and all(n in t for n, _, _ in MM_MOVE) and "'dmSide'" in dis('dmBAct'), 'onEventInit = original + %d moves; ring tap -> dmSide(2)' % len(MM_MOVE))
        elif o == 'statusConsole_p.swf':
            t = dis('onEventInit'); say('dmInitOrig' in t and all(n in t for n in SC_MOVE), 'onEventInit = original + console / fight buttons / turn notice / GM button raised by %d' % SC_UP)
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
            t = dis('onEventInit'); say("'registerAnchorId'" in t and "'right'" in t and "'screen'" in t and 'dmHook' in t, 'onEventInit: anchor right / screen / right, then the hook')
            t = dis('dmBAct'); say("'dmSide'" in t and 'dmRoll' in t, 'tap: helmet -> the column shown / hidden; others -> dmSide(code)')
    return bad


def mock(src, path, W=2400):
    """the computed layout: the movies' own symbols drawn where the patched code puts them, on a W x 1080 screen (not a played movie)"""
    import numpy as np
    from PIL import Image
    import dos2_swf_render as rn
    canvas = Image.new('RGBA', (W, 1080), (38, 44, 40, 255))
    def put(swf, what, x, y, sx=1.0, sy=None, crop=None):
        sy = sx if sy is None else sy
        img, org = rn.render(swf, what, 1.0)
        if crop:   # (x0, y0, x1, y1) in the character's own px
            img = img.crop((int(crop[0] - org[0]), int(crop[1] - org[1]), int(crop[2] - org[0]), int(crop[3] - org[1]))); org = (crop[0], crop[1])
        img = img.resize((max(1, int(img.size[0] * sx)), max(1, int(img.size[1] * sy))), Image.LANCZOS)
        canvas.alpha_composite(img, (int(round(x + (0 if crop else org[0] * sx))), int(round(y + (0 if crop else org[1] * sy)))))
    def root_ids(swf):
        return {p['name'][:-1].decode(): p['char'] for p in (gr.place_parse(c, t) for c, t, _ in swf.tags if c in (4, 26, 70)) if p.get('name')}
    made = build(src)
    # hot bar: stage x = screen x - (W - 1948) / 2, stage y = screen y - 80
    hb = Swf(made['hotBar_p.swf']); ids = root_ids(hb); ox, oy = (W - 1948) / 2.0, 80
    put(hb, ids['hotbar_mc'], ox + HB_X, oy + HB_Y, _s, crop=(CROP, 0, CROP + VISW, VISH))
    put(hb, ids['showLog_mc'], ox + LOG_X, oy + LOG_Y, LOG_S[0] / LOG_S[1])
    put(hb, ids['dmConfirm_mc'], ox + CONFIRM[0] - CONFIRM[2] / 2.0, oy + CONFIRM[1] - CONFIRM[2] / 2.0)
    put(hb, ids['dmCancel_mc'], ox + CANCEL[0], oy + CANCEL[1], CANCEL[2] / 69.0)
    sc = Swf(made['statusConsole_p.swf']); ids = root_ids(sc); ox = (W - 1920) / 2.0
    put(sc, ids['console_mc'], ox + 960, 1080 - 67 - SC_UP)
    mm = Swf(made['minimap_p.swf']); ids = root_ids(mm); ox = W - 420
    at = {p['name'][:-1].decode(): gr.Bits(p['matrix'], 0).matrix() for p in (gr.place_parse(c, t) for c, t, _ in mm.tags if c in (4, 26, 70)) if p.get('name') and p.get('matrix')}
    for name, dx, dy in MM_MOVE:
        if name in ('hit_mc', 'tension_mc', 'armourDoll_mc', 'tensionHit_mc', 'title_txt', 'diplomacy_mc', 'north_mc'): continue
        m = at[name]; put(mm, ids[name], ox + m[4] / 20.0 + dx, m[5] / 20.0 + dy)
    sd = Swf(made['dmSide_p.swf']); ids = root_ids(sd); ox = W - SIDE_W
    for name, code, icon, x, y, size in SIDE: put(sd, ids[name], ox + x, y)
    pi = Swf(made['playerInfo_p.swf'])
    try: put(pi, 'playerInfo', 10 * 1.1, 20 * 1.1, 1.1)
    except Exception as e: print('portrait symbol not drawn: %s' % e)
    canvas.save(path); print('%s: %dx%d (computed layout, DM_DOS2_TOUCH_HUD_SCALE 1)' % (path, W, 1080))


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
