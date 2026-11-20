"""[DOS2] Fifth tap pass (branch dos2-touch6, owner 18.11 playing by touch): appended methods only over the built `_d` movies (every
method body of the input stays byte for byte, every tag but DoABC too). docs/dos2/TOUCH2.md.

Everything hangs on the STAGE (mouseDown / mouseUp, an enterFrame tick for the holds; tw2-tw4: stage listeners fire on the phone, element
listeners often do not) and finds its target by geometry when the finger lands. One onEventInit wrapper per movie (original kept as
dmT5InitOrig); the tick (re)tries the stage listeners.

1. saveLoad_d (load AND save screen: one movie, types 0x4a; request 2 + 6):
     tap on a save row / the "New save" row = the stock selection (loadSave_mc.setCursorPosition(index): the pad's up / down, EI selectedEntry,
     the info panel); a tap on the row that is already selected = loadSave_mc.actionPressed() = the pad's A (load / save over it / new save:
     EI loadPressed / savePressed / newPressed). A row counts when it is visible, on stage and the finger is inside the list's frame.
     The bottom hint bar ("(A) Load (X) Delete (B) Back ...", btnContainer_mc) answers taps by its own element list
     (tools/dos2_taps3_touch.py add_hintbar, as the options screen): EI dmPadBtn(iconId).
2. bottomBar_d (request 1 + 5):
     LONG PRESS on a hot bar slot (TIP_MS, finger still) = the slot's tooltip, the one the pad shows in the quick-access mode (Y): the stock
     slotsHolder.selectSlot(n, true) (frame + Slot.onMouseOver -> EI SlotHover); the movie only shows the engine's answer while `active`, so
     updateTooltip is wrapped (original kept as dmTtUpdOrig): while our tooltip is asked for, it runs with active = true. The tooltip stays
     after the finger is lifted, the next press anywhere hides it. No tooltip after TIP_FB_MS: fallback = the pad's way, EI dmPadBtn(4) (Y,
     quick access) and the slot selected once the bar is active. A hold never also presses the slot (dmSlotN of tools/dos2_taps_touch.py
     is cleared).
     "Hold (B) to end the turn" (endTurnHint_mc / holdEndTurnHint_mc, or a button info with the B icon, in combat): press = the movie's own
     onEventDown("IE UIEndTurn"), release = onEventUp (the pad's B hold: the circle fills, animationDone sends EI endTurn); a short tap only
     starts / stops the circle, as with the pad.
     The button infos over the hot bar ("(A) Move  (X) Actions", bottombar_mc.btnList: BtnInfo, iconId = the pad code): press + release on
     the same one = EI dmPadBtn(iconId). No-op mouseDown listeners on those elements make the press the HUD's (the host does not take it for
     a world finger); if they never fire, the host still has them as HUD rects ("btn<code>" / "hold<code>", tools/dos2_hudrect_touch.py).
3. partyInventory_d, inventorySkillPanel_d (request 3): LONG PRESS on an item (HOLD_MS, finger still) = its item menu, the pad's X: the item
     under the finger is selected (larInventory.selectItem(getSlotForMousePosition)), the stock hold / drag state is reset, then the party
     inventory gets EI dmPadBtn(3) (X = ControllerContextMenu -> panel showActionMenu) and the skills panel inventory_mc.cursorShowActionMenu()
     (its item half, whatever half has the pad's focus). The stock 300 ms hold timer of larInventory (EI showActionMenuItem) is stopped on
     every press, so there is never a second menu. Tap = select, double tap = use: unchanged.
4. equipmentPanel_d (request 3 audit: the doll and its inventory had no mouse code at all): tap on a doll slot / an inventory cell = select it
     (doll_mc.selectSlotByPos / eqInv_mc.selectSlot: the pad's cursor, tooltip), a tap on the selected one = mainpanel_mc.cursorAccept() (A:
     choose what to wear / equip), LONG PRESS = select + mainpanel_mc.cursorShowActionMenu() (X).

  python -I tools/dos2_taps5_touch.py check <dir with the built _d movies>
  python -I tools/dos2_taps5_touch.py build <in.swf> <out.swf>
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, OPS, verify_all
from dos2_taps_touch import HEAD, obj, num
from dos2_taps3_touch import doc_ctx, hook_init, add_hintbar

TAP_MS, DRAG_PX, MOVE_PX = 1500, 40, 18      # row tap: press + release within TAP_MS, moved at most DRAG_PX; a hold: moved at most MOVE_PX (stock drag: 20)
HOLD_MS, TIP_MS, TIP_FB_MS, Y_WAIT_MS = 400, 500, 700, 2000   # HOLD_MS from the press: on a panel the host sends it 200 ms after the finger (DM_DOS2_TOUCH_UI_PRESS_MS)
BI_PAD = 10                                    # button info hit box margin (stage px)
SAVE, BAR, PARTY, SKILLS, EQUIP = 'saveLoad_d.swf', 'bottomBar_d.swf', 'partyInventory_d.swf', 'inventorySkillPanel_d.swf', 'equipmentPanel_d.swf'


class G:
    def __init__(s): s.n = 0
    def __call__(s, base): s.n += 1; return '%s_%d' % (base, s.n)


def mn_used(abc, cls, meth, text):
    """the multiname index a method of class cls uses for `text` (private names: the class's own private namespace)"""
    t = abc.trait(abc.class_index(cls), meth); assert t is not None, '%s::%s' % (cls, meth)
    for _, op, ops, _ in abc.ins(abc.methods[t.b].body.code):
        for f, v in zip(OPS[op][1], ops):
            if f == 'm' and abc.short(v) == text: return v
    raise KeyError('%s::%s uses no %s' % (cls, meth, text))


def sxy(c):
    P = c.P
    return [('getlocal0',), ('getproperty', P('stage')), ('getproperty', P('mouseX'))], [('getlocal0',), ('getproperty', P('stage')), ('getproperty', P('mouseY'))]


def hit(c, loc, fail, shape=False):
    """goto fail unless the display object in local loc is visible, on stage and under the finger (hitTestPoint, bounding box)"""
    P = c.P; X, Y = sxy(c)
    return [('getlocal', loc), ('iffalse', fail), ('getlocal', loc), ('getproperty', P('visible')), ('iffalse', fail),
            ('getlocal', loc), ('getproperty', P('stage')), ('iffalse', fail),
            ('getlocal', loc)] + X + Y + [('pushtrue',) if shape else ('pushfalse',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', fail)]


def still(c, kx, ky, px, fail):
    """goto fail when the finger moved more than px from this.<kx>, this.<ky> (locals 9 / 10 are scratch)"""
    P = c.P; X, Y = sxy(c); out = []
    for v, k in ((X, kx), (Y, ky)):
        out += v + [('getlocal0',), ('getproperty', P(k)), ('subtract',), ('convert_d',), ('setlocal', 9),
                    ('getlocal', 9), ('pushbyte', px), ('ifgt', fail), ('getlocal', 9), ('pushbyte', -px), ('iflt', fail)]
    return out


def stamp(c, kt, kx, ky):
    P = c.P; X, Y = sxy(c)
    return [('getlocal0',)] + c.now() + [('setproperty', P(kt)), ('getlocal0',)] + X + [('setproperty', P(kx)), ('getlocal0',)] + Y + [('setproperty', P(ky))]


def elapsed_lt(c, kt, ms, lbl):
    """goto lbl when now - this.<kt> < ms"""
    return c.now() + [('getlocal0',), ('getproperty', c.P(kt)), ('subtract',), ('pushshort', ms), ('iflt', lbl)]


def add_idx(c):
    P = c.P
    c.add('dmT5Idx', 2, HEAD + num(3) + num(4) + [
        ('getlocal1',), ('iffalse', 'none'), ('getlocal2',), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('length')), ('convert_d',), ('setlocal3',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal1',), ('getlocal', 4), ('getproperty', c.late), ('getlocal2',), ('ifstrictne', 'next'),
        ('getlocal', 4), ('returnvalue',),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('L', 'none'), ('pushbyte', -1), ('returnvalue',)], c.rbody)


def add_arrhit(c, name):
    """<name>(arr): the first element of arr under the finger (visible, on stage, bounding box) or null. locals 1 arr 2 n 3 i 4 el"""
    c.add(name, 1, HEAD + num(2) + num(3) + obj(4) + [
        ('getlocal1',), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', c.P('length')), ('convert_d',), ('setlocal2',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal1',), ('getlocal3',), ('getproperty', c.late), ('coerce_a',), ('setlocal', 4)] + hit(c, 4, 'next') + [
        ('getlocal', 4), ('returnvalue',),
        ('L', 'next'), ('getlocal3',), ('increment',), ('convert_d',), ('setlocal3',),
        ('L', 'cond'), ('getlocal3',), ('getlocal2',), ('iflt', 'loop'),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], c.rbody)


def stage_hook(c, down, up, tick_extra=(), elems=()):
    """dmT5Stage(): stage mouseDown / mouseUp once (+ a no-op mouseDown on each element path in elems, so the press is the UI's);
    dmT5Tick(e): retries it, then tick_extra"""
    P, S = c.P, c.S
    c.add('dmT5Nop', 1, HEAD + [('returnvoid',)], c.rbody)
    el = []
    for k, path in enumerate(elems):
        nx = 'el%d' % k
        el += [('getlocal0',), ('coerce_a',), ('setlocal2',)]
        for st in path: el += [('getlocal2',), ('getproperty', P(st)), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', nx)]
        el += [('pushstring', S('addEventListener')), ('getlocal2',), ('in',), ('iffalse', nx),
               ('getlocal2',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmT5Nop')), ('callpropvoid', P('addEventListener'), 2),
               ('L', nx)]
    c.add('dmT5Stage', 0, HEAD + obj(1) + obj(2) + [
        ('getlocal0',), ('getproperty', P('dmT5On')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmT5On')),
        ('getlocal1',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P(down)), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal1',), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P(up)), ('callpropvoid', P('addEventListener'), 2)] + el + [
        ('L', 'end'), ('returnvoid',)], c.rbody)
    c.add('dmT5Tick', 1, HEAD + [('getlocal0',), ('callpropvoid', P('dmT5Stage'), 0)] + list(tick_extra) + [('returnvoid',)], c.rbody)


# ---------------------------------------------------------------- 1. load / save screen
def patch_save(data):
    c = doc_ctx(data); P, S, abc = c.P, c.S, c.abc
    sl = abc.class_index('saveLoad_c_fla.saveLoad_mc_1')
    for n in ('list', 'setCursorPosition', 'actionPressed'): assert abc.trait(sl, n) is not None, 'saveLoad_mc_1.' + n
    assert abc.trait(c.ci, 'loadSave_mc') is not None and abc.trait(c.ci, 'btnContainer_mc') is not None
    assert abc.trait(abc.class_index('LS_Classes.scrollList'), 'frameHit') is not None
    add_idx(c); add_arrhit(c, 'dmT5Arr')
    X, Y = sxy(c)
    # dmSlHit(): the row under the finger or null. locals 1 lm 2 list 3 fh
    c.add('dmSlHit', 0, HEAD + obj(1) + obj(2) + obj(3) + [
        ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal0',), ('getproperty', P('loadSave_mc')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('list')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'none'),
        ('getlocal2',), ('getproperty', P('frameHit')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'rows'),   # the list's frame: rows scrolled out of it do not count
        ('pushstring', S('hitTestPoint')), ('getlocal3',), ('in',), ('iffalse', 'rows'),
        ('getlocal3',)] + X + Y + [('pushfalse',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'none'),
        ('L', 'rows'),
        ('getlocal0',), ('getlocal2',), ('getproperty', P('content_array')), ('callproperty', P('dmT5Arr'), 1), ('returnvalue',),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], c.rbody)
    c.add('dmSlDown', 1, HEAD + [('getlocal0',), ('getlocal0',), ('callproperty', P('dmSlHit'), 0), ('setproperty', P('dmSlE'))] + stamp(c, 'dmSlT', 'dmSlX', 'dmSlY') + [
        ('returnvoid',)], c.rbody)
    # dmSlUp(e). locals 2 el 3 lm 4 cur 5 idx 9 10 scratch
    c.add('dmSlUp', 1, HEAD + obj(2) + obj(3) + obj(4) + num(5) + num(9) + num(10) + [
        ('getlocal0',), ('getproperty', P('dmSlE')), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmSlE')),
        ('getlocal2',), ('iffalse', 'end')] + c.now() + [('getlocal0',), ('getproperty', P('dmSlT')), ('subtract',), ('pushshort', TAP_MS), ('ifgt', 'end')] +
        still(c, 'dmSlX', 'dmSlY', DRAG_PX, 'end') + [
        ('getlocal0',), ('callproperty', P('dmSlHit'), 0), ('getlocal2',), ('ifstrictne', 'end'),
        ('getlocal0',), ('getproperty', P('loadSave_mc')), ('coerce_a',), ('setlocal3',),
        ('getlocal3',), ('getproperty', P('list')), ('callproperty', P('getCurrentMovieClip'), 0), ('coerce_a',), ('setlocal', 4),
        ('getlocal', 4), ('getlocal2',), ('ifstrictne', 'sel'),
        ('getlocal3',), ('callpropvoid', P('actionPressed'), 0), ('jump', 'end'),   # the selected row again = the pad's A
        ('L', 'sel'),
        ('getlocal0',), ('getlocal3',), ('getproperty', P('list')), ('getproperty', P('content_array')), ('getlocal2',), ('callproperty', P('dmT5Idx'), 2), ('convert_d',), ('setlocal', 5),
        ('getlocal', 5), ('pushbyte', 0), ('iflt', 'end')] + c.ei(2, ('pushstring', S('PlaySound')), ('pushstring', S('UI_Gen_CursorMove'))) + [
        ('getlocal3',), ('getlocal', 5), ('callpropvoid', P('setCursorPosition'), 1),
        ('L', 'end'), ('returnvoid',)], c.rbody)
    stage_hook(c, 'dmSlDown', 'dmSlUp')
    hb = add_hintbar(c, ('btnContainer_mc',))
    hook_init(c, 'dmT5InitOrig', ['dmT5Stage', hb[0]], ['dmT5Tick', hb[1]])
    return c


# ---------------------------------------------------------------- 2. hot bar: tooltip by long press, end turn by hold, button infos by tap
def patch_bar(data):
    c = doc_ctx(data); P, S, abc = c.P, c.S, c.abc
    assert abc.mname(c.inst['name']) == 'bottomBar_c_fla.MainTimeline'
    assert abc.trait(c.ci, 'dmSlotAt') is not None, 'not the bottomBar_d of tools/dos2_taps_touch.py (no dmSlotAt)'
    for n in ('active', 'isDragging', 'bottombar_mc', 'updateTooltip', 'inActiveCombat', 'events', 'endTurnHint_mc', 'holdEndTurnHint_mc', 'onEventDown', 'onEventUp'):
        assert abc.trait(c.ci, n) is not None, 'MainTimeline.' + n
    bb, sh = abc.class_index('bottomBar_c_fla.bottombarMC_1'), abc.class_index('bottomBar_c_fla.slotsHolder_11')
    for n in ('btnList', 'slotsHolder_mc', 'tooltipHolder_mc', 'hideTooltip'): assert abc.trait(bb, n) is not None, 'bottombarMC_1.' + n
    for n in ('selectSlot', 'getSlot', 'hideHL'): assert abc.trait(sh, n) is not None, 'slotsHolder.' + n
    for n in ('IE UIEndTurn', 'SlotHoverOut', 'iconId'): assert n.encode() in abc.strings, n
    add_arrhit(c, 'dmT5Arr')
    holder = lambda loc, fail: [('getlocal0',), ('getproperty', P('bottombar_mc')), ('coerce_a',), ('setlocal', loc), ('getlocal', loc), ('iffalse', fail),
                                ('getlocal', loc), ('getproperty', P('slotsHolder_mc')), ('coerce_a',), ('setlocal', loc), ('getlocal', loc), ('iffalse', fail)]
    # updateTooltip with active = true while our long-press tooltip is up (the movie shows the engine's tooltip only in the active bar)
    c.wrap('updateTooltip', 'dmTtUpdOrig', lambda n: [
        ('getlocal0',), ('getproperty', P('dmTtShow')), ('iffalse', 'plain'),
        ('getlocal0',), ('getproperty', P('active')), ('iftrue', 'plain'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('active')),
        ('getlocal0',), ('callpropvoid', P('dmTtUpdOrig'), 0),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('active')),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmTtSeen')), ('returnvoid',),
        ('L', 'plain'), ('getlocal0',), ('callpropvoid', P('dmTtUpdOrig'), 0), ('returnvoid',)])
    # dmTtHide(): our tooltip down again (not when the pad's active bar owns it). locals 1 h
    c.add('dmTtHide', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmTtShow')), ('iffalse', 'end'),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmTtShow')),
        ('getlocal0',), ('pushshort', 255), ('setproperty', P('dmTtFb')),
        ('getlocal0',), ('getproperty', P('active')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('bottombar_mc')), ('callpropvoid', P('hideTooltip'), 0)] + c.ei(1, ('pushstring', S('SlotHoverOut'))) + holder(1, 'end') + [
        ('getlocal1',), ('pushshort', 255), ('pushtrue',), ('callpropvoid', P('selectSlot'), 2),
        ('getlocal1',), ('callpropvoid', P('hideHL'), 0),
        ('L', 'end'), ('returnvoid',)], c.rbody)
    # dmEtOn(): the finger is on an end-turn hint (in combat). locals 1 mc
    et = [('getlocal0',), ('getproperty', P('inActiveCombat')), ('iffalse', 'no')]
    for k, n in enumerate(('endTurnHint_mc', 'holdEndTurnHint_mc')):
        et += [('getlocal0',), ('getproperty', P(n)), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'e%d' % k),
               ('getlocal1',), ('getproperty', P('alpha')), ('pushbyte', 0), ('ifngt', 'e%d' % k)] + hit(c, 1, 'e%d' % k) + [('pushtrue',), ('returnvalue',), ('L', 'e%d' % k)]
    c.add('dmEtOn', 0, HEAD + obj(1) + et + [('L', 'no'), ('pushfalse',), ('returnvalue',)], c.rbody)
    # dmBiHit(): the button info (BtnInfo of bottombar_mc.btnList) under the finger or null
    c.add('dmBiHit', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal0',), ('getproperty', P('bottombar_mc')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('btnList')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal0',), ('getlocal1',), ('getproperty', P('content_array')), ('callproperty', P('dmT5Arr'), 1), ('returnvalue',),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], c.rbody)
    # dmT5Down(e). locals 2 b 3 k
    c.add('dmT5Down', 1, HEAD + obj(2) + num(3) + [
        ('getlocal0',), ('callpropvoid', P('dmTtHide'), 0),
        # the hot bar long press: remember the slot
        ('getlocal0',), ('getlocal0',), ('callproperty', P('dmSlotAt'), 0), ('setproperty', P('dmTtN')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmTtDone'))] + stamp(c, 'dmTtT', 'dmTtX', 'dmTtY') + [
        # end turn hold / button info tap
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmBiE')),
        ('getlocal0',), ('callproperty', P('dmBiHit'), 0), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('getproperty', P('inActiveCombat')), ('iffalse', 'bi'),
        ('getlocal0',), ('callproperty', P('dmEtOn'), 0), ('iftrue', 'et'),
        ('getlocal2',), ('iffalse', 'bi'), ('getlocal2',), ('getproperty', P('iconId')), ('pushbyte', 1), ('ifne', 'bi'),
        ('L', 'et'),
        ('getlocal0',), ('getproperty', P('events')), ('pushstring', S('IE UIEndTurn')), ('callproperty', P('indexOf'), 1), ('convert_d',), ('setlocal3',),
        ('getlocal3',), ('pushbyte', 0), ('iflt', 'end'),
        ('getlocal0',), ('getlocal3',), ('setproperty', P('dmEtK')),
        ('getlocal0',), ('getlocal3',), ('callpropvoid', P('onEventDown'), 1), ('jump', 'end'),
        ('L', 'bi'),
        ('getlocal2',), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal2',), ('setproperty', P('dmBiE')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmBiT')),
        ('L', 'end'), ('returnvoid',)], c.rbody)
    # dmT5Up(e). locals 2 b 3 k
    c.add('dmT5Up', 1, HEAD + obj(2) + num(3) + [
        ('getlocal0',), ('pushshort', 255), ('setproperty', P('dmTtN')),
        ('pushstring', S('dmEtK')), ('getlocal0',), ('in',), ('iffalse', 'bi'),
        ('getlocal0',), ('getproperty', P('dmEtK')), ('convert_d',), ('setlocal3',),
        ('getlocal3',), ('pushbyte', 0), ('iflt', 'bi'),
        ('getlocal0',), ('pushbyte', -1), ('setproperty', P('dmEtK')),
        ('getlocal0',), ('getlocal3',), ('callpropvoid', P('onEventUp'), 1),
        ('L', 'bi'),
        ('getlocal0',), ('getproperty', P('dmBiE')), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmBiE')),
        ('getlocal2',), ('iffalse', 'end')] + c.now() + [('getlocal0',), ('getproperty', P('dmBiT')), ('subtract',), ('pushshort', TAP_MS), ('ifgt', 'end'),
        ('getlocal0',), ('callproperty', P('dmBiHit'), 0), ('getlocal2',), ('ifstrictne', 'end'),
        ('getlocal2',), ('getproperty', P('iconId')), ('pushundefined',), ('ifstricteq', 'end')] +
        c.ei(2, ('pushstring', S('dmPadBtn')), ('getlocal2',), ('getproperty', P('iconId'))) + [
        ('L', 'end'), ('returnvoid',)], c.rbody)
    # the long press tick. locals 1 h 2 slot 9 10 scratch
    tick = [
        # fallback: no tooltip came for our hover -> the pad's quick access (Y), then the slot once the bar is active
        ('getlocal0',), ('getproperty', P('dmTtShow')), ('iffalse', 'yw'),
        ('getlocal0',), ('getproperty', P('dmTtFb')), ('pushshort', 255), ('ifeq', 'yw')] + elapsed_lt(c, 'dmTtAt', TIP_FB_MS, 'yw') + [
        ('getlocal0',), ('getproperty', P('dmTtSeen')), ('iftrue', 'fbok'),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmTtFb')), ('setproperty', P('dmTtYn')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmTtYt')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmTtShow'))] + c.ei(2, ('pushstring', S('dmPadBtn')), ('pushbyte', 4)) + [
        ('L', 'fbok'), ('getlocal0',), ('pushshort', 255), ('setproperty', P('dmTtFb')),
        ('L', 'yw'),
        ('getlocal0',), ('getproperty', P('dmTtYn')), ('iffalse', 'lp'),
        ('getlocal0',), ('getproperty', P('dmTtYn')), ('pushshort', 255), ('ifeq', 'lp')] + elapsed_lt(c, 'dmTtYt', Y_WAIT_MS, 'ywait') + [
        ('getlocal0',), ('pushshort', 255), ('setproperty', P('dmTtYn')), ('jump', 'lp'),
        ('L', 'ywait'),
        ('getlocal0',), ('getproperty', P('active')), ('iffalse', 'lp')] + holder(1, 'lp') + [
        ('getlocal1',), ('getlocal0',), ('getproperty', P('dmTtYn')), ('pushtrue',), ('callpropvoid', P('selectSlot'), 2),
        ('getlocal0',), ('pushshort', 255), ('setproperty', P('dmTtYn')),
        # the long press itself
        ('L', 'lp'),
        ('pushstring', S('dmTtN')), ('getlocal0',), ('in',), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('dmTtN')), ('pushshort', 255), ('ifeq', 'end'),
        ('getlocal0',), ('getproperty', P('dmTtDone')), ('iftrue', 'end')] + elapsed_lt(c, 'dmTtT', TIP_MS, 'end') + [
        ('getlocal0',), ('getproperty', P('isDragging')), ('iftrue', 'end')] + still(c, 'dmTtX', 'dmTtY', MOVE_PX, 'drop') + [
        ('getlocal0',), ('callproperty', P('dmSlotAt'), 0), ('getlocal0',), ('getproperty', P('dmTtN')), ('ifne', 'drop'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmTtDone'))] + holder(1, 'end') + [
        ('getlocal1',), ('getlocal0',), ('getproperty', P('dmTtN')), ('callproperty', P('getSlot'), 1), ('coerce_a',), ('setlocal2',),
        ('getlocal2',), ('iffalse', 'end'), ('getlocal2',), ('getproperty', P('inUse')), ('iffalse', 'end'),
        ('getlocal0',), ('pushshort', 255), ('setproperty', P('dmSlotN')),   # the release must not press the slot (tools/dos2_taps_touch.py dmSlotUp)
        ('getlocal0',), ('getproperty', P('active')), ('iftrue', 'sel'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmTtShow')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmTtSeen')),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmTtN')), ('setproperty', P('dmTtFb')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmTtAt')),
        ('L', 'sel'),
        ('getlocal1',), ('getlocal0',), ('getproperty', P('dmTtN')), ('pushtrue',), ('callpropvoid', P('selectSlot'), 2),
        ('jump', 'end'),
        ('L', 'drop'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmTtDone')),
        ('L', 'end')]
    c.add('dmTtTick', 1, HEAD + obj(1) + obj(2) + num(9) + num(10) + tick + [('returnvoid',)], c.rbody)
    stage_hook(c, 'dmT5Down', 'dmT5Up', [('getlocal0',), ('getlocal1',), ('callpropvoid', P('dmTtTick'), 1)],
               elems=(('endTurnHint_mc',), ('holdEndTurnHint_mc',), ('bottombar_mc', 'btnList')))
    c.add('dmT5Init', 0, HEAD + [('getlocal0',), ('pushshort', 255), ('setproperty', P('dmTtN')), ('getlocal0',), ('pushshort', 255), ('setproperty', P('dmTtFb')),
                                 ('getlocal0',), ('pushshort', 255), ('setproperty', P('dmTtYn')), ('getlocal0',), ('pushbyte', -1), ('setproperty', P('dmEtK')),
                                 ('returnvoid',)], c.rbody)
    hook_init(c, 'dmT5InitOrig', ['dmT5Stage'], ['dmT5Tick'], first=['dmT5Init'])
    return c


# ---------------------------------------------------------------- 3. inventories: long press = item menu (X)
def lp_common(c, find, fire):
    """down: the larInventory under the finger (find: lines leaving it in local 2 or null), stock hold timer stopped; tick: HOLD_MS still = fire"""
    P, abc = c.P, c.abc
    LI = 'LS_Classes.larInventory'
    mn = {n: mn_used(abc, LI, 'mouseDown', n) for n in ('holdTimer', 'getSlotForMousePosition', 'draggingItemHandle', 'draggingInventoryOwner', 'checkDraggingStart')}
    cls = abc.pub('larInventory', 'LS_Classes')
    c.add('dmLpDown', 1, HEAD + obj(2) + obj(3) + [
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmLpL'))] + find + [
        ('getlocal2',), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal2',), ('setproperty', P('dmLpL')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmLpDone'))] + stamp(c, 'dmLpT', 'dmLpX', 'dmLpY') + [
        ('getlocal2',), ('getproperty', mn['holdTimer']), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end'),   # no stock 300 ms menu next to ours
        ('getlocal3',), ('callpropvoid', P('stop'), 0),
        ('L', 'end'), ('returnvoid',)], c.rbody)
    c.add('dmLpUp', 1, HEAD + [('getlocal0',), ('pushnull',), ('setproperty', P('dmLpL')), ('returnvoid',)], c.rbody)
    # dmLpTick(e). locals 2 li 3 slot 4 cls 9 10 scratch
    c.add('dmLpTick', 1, HEAD + obj(2) + num(3) + obj(4) + num(9) + num(10) + [
        ('getlocal0',), ('getproperty', P('dmLpL')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('dmLpDone')), ('iftrue', 'end')] + elapsed_lt(c, 'dmLpT', HOLD_MS, 'end') + still(c, 'dmLpX', 'dmLpY', MOVE_PX, 'drop') + [
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmLpDone')),
        ('getlocal2',), ('getproperty', P('stage')), ('iffalse', 'end'),
        ('getlocal2',), ('callproperty', mn['getSlotForMousePosition'], 0), ('convert_d',), ('setlocal3',),
        ('getlocal3',), ('pushbyte', 0), ('iflt', 'end'),
        ('getlocal2',), ('getlocal3',), ('pushtrue',), ('callpropvoid', P('selectItem'), 2),
        # the stock hold / drag state back to "nothing pressed" (as its own holdTimerEnded does): no drag, no stopDragging on the release
        ('getlex', cls), ('coerce_a',), ('setlocal', 4),
        ('getlocal', 4), ('pushshort', 255), ('setproperty', mn['draggingItemHandle']),
        ('getlocal', 4), ('pushshort', 255), ('setproperty', mn['draggingInventoryOwner']),
        ('getlocal', 4), ('pushfalse',), ('setproperty', mn['checkDraggingStart'])] + fire + [
        ('jump', 'end'),
        ('L', 'drop'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmLpDone')),
        ('L', 'end'), ('returnvoid',)], c.rbody)


def patch_party(data):
    c = doc_ctx(data); P, abc = c.P, c.abc
    pc = abc.class_index('partyInventory_c_fla.panel_1')
    assert abc.trait(pc, 'inventoryList') is not None and abc.trait(c.ci, 'inMultiSelect') is not None
    # find: every party member's grid (inventoryList.content_array[i].localInventory). locals 2 li 3 arr 4 n 5 i 6 el
    find = [('getlocal0',), ('getproperty', P('inMultiSelect')), ('iftrue', 'end'),
            ('getlocal0',), ('getproperty', P('inventoryPanel_mc')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end'),
            ('getlocal3',), ('getproperty', P('visible')), ('iffalse', 'end'),
            ('getlocal3',), ('getproperty', P('inventoryList')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end'),
            ('getlocal3',), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end'),
            ('getlocal3',), ('getproperty', P('length')), ('convert_d',), ('setlocal', 4),
            ('pushbyte', 0), ('convert_d',), ('setlocal', 5), ('jump', 'fc'),
            ('L', 'fl'), ('label',),
            ('getlocal3',), ('getlocal', 5), ('getproperty', c.late), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'fn'),
            ('getlocal', 6), ('getproperty', P('localInventory')), ('coerce_a',), ('setlocal', 6)] + hit(c, 6, 'fn') + [
            ('getlocal', 6), ('setlocal2',), ('jump', 'fd'),
            ('L', 'fn'), ('getlocal', 5), ('increment',), ('convert_d',), ('setlocal', 5),
            ('L', 'fc'), ('getlocal', 5), ('getlocal', 4), ('iflt', 'fl'),
            ('L', 'fd')]
    fire = c.ei(2, ('pushstring', c.S('dmPadBtn')), ('pushbyte', 3))   # X = ControllerContextMenu -> panel_1.showActionMenu (the pad's own path)
    lp = HEAD + obj(2) + obj(3) + num(4) + num(5) + obj(6)
    find_fn = find
    # dmLpFind(): find as a method (locals of its own)
    c.add('dmLpFind', 0, lp + find_fn + [('getlocal2',), ('returnvalue',), ('L', 'end'), ('pushnull',), ('returnvalue',)], c.rbody)
    lp_common(c, [('getlocal0',), ('callproperty', P('dmLpFind'), 0), ('coerce_a',), ('setlocal2',)], fire)
    stage_hook(c, 'dmLpDown', 'dmLpUp', [('getlocal0',), ('getlocal1',), ('callpropvoid', P('dmLpTick'), 1)])
    hook_init(c, 'dmT5InitOrig', ['dmT5Stage'], ['dmT5Tick'])
    return c


def patch_skills(data):
    c = doc_ctx(data); P, abc = c.P, c.abc
    im = abc.class_index('inventorySkillPanel_c_fla.inventoryMiddle_4')
    assert abc.trait(im, 'cursorShowActionMenu') is not None and abc.trait(im, 'localInventory') is not None
    chain = []
    for n in ('mainpanel_mc', 'inventory_mc'):
        chain += [('getlocal2',), ('getproperty', P(n)), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'none'),
                  ('getlocal2',), ('getproperty', P('visible')), ('iffalse', 'none')]
    c.add('dmLpFind', 0, HEAD + obj(2) + [('getlocal0',), ('coerce_a',), ('setlocal2',)] + chain + [
        ('getlocal2',), ('getproperty', P('localInventory')), ('coerce_a',), ('setlocal2',)] + hit(c, 2, 'none') + [
        ('getlocal2',), ('returnvalue',), ('L', 'none'), ('pushnull',), ('returnvalue',)], c.rbody)
    fire = [('getlocal0',), ('getproperty', P('mainpanel_mc')), ('getproperty', P('inventory_mc')), ('callpropvoid', P('cursorShowActionMenu'), 0)]
    lp_common(c, [('getlocal0',), ('callproperty', P('dmLpFind'), 0), ('coerce_a',), ('setlocal2',)], fire)
    stage_hook(c, 'dmLpDown', 'dmLpUp', [('getlocal0',), ('getlocal1',), ('callpropvoid', P('dmLpTick'), 1)])
    hook_init(c, 'dmT5InitOrig', ['dmT5Stage'], ['dmT5Tick'])
    return c


# ---------------------------------------------------------------- 4. equipment: doll / inventory cells
def patch_equip(data):
    c = doc_ctx(data); P, S, abc = c.P, c.S, c.abc
    mp, eq, dl = (abc.class_index('equipmentPanel_c_fla.' + n) for n in ('mainPanel_1', 'EqInv_16', 'equipDoll_3'))
    for n in ('doll_mc', 'eqInv_mc', 'currentSelectedDoll', 'cursorAccept', 'cursorShowActionMenu'): assert abc.trait(mp, n) is not None, 'mainPanel_1.' + n
    for n in ('slot_array', 'selectSlot', 'getCurrentSelection'): assert abc.trait(eq, n) is not None, 'EqInv.' + n
    for n in ('slot_array', 'selectSlotByPos', 'getCurrentSelection'): assert abc.trait(dl, n) is not None, 'equipDoll.' + n
    assert abc.trait(c.ci, 'mainpanel_mc') is not None
    add_idx(c); add_arrhit(c, 'dmT5Arr')
    X, Y = sxy(c)
    # dmEqClip(o, top): false when an ancestor of o up to top clips it (scrollRect / mask) and the finger is outside. locals 3 p 4 r 5 m
    inside = lambda o, fail: [('getlocal', o), ('getproperty', P('mouseX')), ('getlocal', 4), ('getproperty', P('x')), ('ifnge', fail),
                              ('getlocal', o), ('getproperty', P('mouseX')), ('getlocal', 4), ('getproperty', P('x')), ('getlocal', 4), ('getproperty', P('width')), ('add',), ('ifnle', fail),
                              ('getlocal', o), ('getproperty', P('mouseY')), ('getlocal', 4), ('getproperty', P('y')), ('ifnge', fail),
                              ('getlocal', o), ('getproperty', P('mouseY')), ('getlocal', 4), ('getproperty', P('y')), ('getlocal', 4), ('getproperty', P('height')), ('add',), ('ifnle', fail)]
    c.add('dmEqClip', 2, HEAD + obj(3) + obj(4) + obj(5) + [
        ('getlocal1',), ('coerce_a',), ('setlocal3',),
        ('L', 'up'), ('label',),
        ('getlocal3',), ('iffalse', 'yes'),
        ('getlocal3',), ('getproperty', P('scrollRect')), ('coerce_a',), ('setlocal', 4), ('getlocal', 4), ('iffalse', 'mk')] + inside(3, 'no') + [
        ('L', 'mk'),
        ('getlocal3',), ('getproperty', P('mask')), ('coerce_a',), ('setlocal', 5), ('getlocal', 5), ('iffalse', 'nx'),
        ('getlocal', 5)] + X + Y + [('pushtrue',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'no'),
        ('L', 'nx'),
        ('getlocal3',), ('getlocal2',), ('ifstricteq', 'yes'),
        ('getlocal3',), ('getproperty', P('parent')), ('coerce_a',), ('setlocal3',), ('jump', 'up'),
        ('L', 'yes'), ('pushtrue',), ('returnvalue',),
        ('L', 'no'), ('pushfalse',), ('returnvalue',)], c.rbody)
    # dmEqFind(): sets dmEqW (1 doll, 2 inventory, 0 none), dmEqI (slot_array index), dmEqO (the slot). locals 1 mp 2 w 3 arr 4 el 5 i
    c.add('dmEqFind', 0, HEAD + obj(1) + obj(2) + obj(3) + obj(4) + num(5) + [
        ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmEqW')),
        ('getlocal0',), ('getproperty', P('mainpanel_mc')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal1',), ('getproperty', P('visible')), ('iffalse', 'end'),
        ('getlocal1',), ('getproperty', P('currentSelectedDoll')), ('iffalse', 'inv'),
        ('getlocal1',), ('getproperty', P('doll_mc')), ('coerce_a',), ('setlocal2',), ('pushbyte', 1), ('convert_d',), ('setlocal', 5), ('jump', 'go'),
        ('L', 'inv'),
        ('getlocal1',), ('getproperty', P('eqInv_mc')), ('coerce_a',), ('setlocal2',), ('pushbyte', 2), ('convert_d',), ('setlocal', 5),
        ('L', 'go'),
        ('getlocal2',), ('iffalse', 'end'), ('getlocal2',), ('getproperty', P('visible')), ('iffalse', 'end'),
        ('getlocal2',), ('getproperty', P('slot_array')), ('coerce_a',), ('setlocal3',),
        ('getlocal0',), ('getlocal3',), ('callproperty', P('dmT5Arr'), 1), ('coerce_a',), ('setlocal', 4), ('getlocal', 4), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', 4), ('getlocal2',), ('callproperty', P('dmEqClip'), 2), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', 5), ('setproperty', P('dmEqW')),
        ('getlocal0',), ('getlocal', 4), ('setproperty', P('dmEqO')),
        ('getlocal0',), ('getlocal0',), ('getlocal3',), ('getlocal', 4), ('callproperty', P('dmT5Idx'), 2), ('setproperty', P('dmEqI')),
        ('L', 'end'), ('returnvoid',)], c.rbody)
    # dmEqSel(): select dmEqW / dmEqI as the pad's cursor would. locals 1 mp
    c.add('dmEqSel', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('mainpanel_mc')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('dmEqI')), ('pushbyte', 0), ('iflt', 'end'),
        ('getlocal0',), ('getproperty', P('dmEqW')), ('pushbyte', 1), ('ifne', 'inv'),
        ('getlocal1',), ('getproperty', P('doll_mc')), ('getlocal0',), ('getproperty', P('dmEqI')), ('pushtrue',), ('callpropvoid', P('selectSlotByPos'), 2), ('jump', 'end'),
        ('L', 'inv'),
        ('getlocal1',), ('getproperty', P('eqInv_mc')), ('getlocal0',), ('getproperty', P('dmEqI')), ('pushtrue',), ('callpropvoid', P('selectSlot'), 2),
        ('L', 'end'), ('returnvoid',)], c.rbody)
    # dmEqIsSel(): the tapped slot is the current selection of its half. locals 1 mp 2 w
    c.add('dmEqIsSel', 0, HEAD + obj(1) + obj(2) + [
        ('getlocal0',), ('getproperty', P('mainpanel_mc')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'no'),
        ('getlocal1',), ('getproperty', P('eqInv_mc')), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('getproperty', P('dmEqW')), ('pushbyte', 1), ('ifne', 'w'),
        ('getlocal1',), ('getproperty', P('doll_mc')), ('coerce_a',), ('setlocal2',),
        ('L', 'w'), ('getlocal2',), ('iffalse', 'no'),
        ('getlocal2',), ('callproperty', P('getCurrentSelection'), 0), ('getlocal0',), ('getproperty', P('dmEqO')), ('ifstrictne', 'no'),
        ('pushtrue',), ('returnvalue',),
        ('L', 'no'), ('pushfalse',), ('returnvalue',)], c.rbody)
    c.add('dmEqDown', 1, HEAD + [('getlocal0',), ('callpropvoid', P('dmEqFind'), 0),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmEqW')), ('setproperty', P('dmEqDW')),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmEqO')), ('setproperty', P('dmEqDO')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmEqDone'))] + stamp(c, 'dmEqT', 'dmEqX', 'dmEqY') + [('returnvoid',)], c.rbody)
    # dmEqUp(e): a quick tap on the same slot: the selected one = A, else select. locals 9 10 scratch
    c.add('dmEqUp', 1, HEAD + num(9) + num(10) + [
        ('getlocal0',), ('getproperty', P('dmEqDW')), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('dmEqDone')), ('iftrue', 'clr')] + c.now() + [('getlocal0',), ('getproperty', P('dmEqT')), ('subtract',), ('pushshort', TAP_MS), ('ifgt', 'clr')] +
        still(c, 'dmEqX', 'dmEqY', DRAG_PX, 'clr') + [
        ('getlocal0',), ('callpropvoid', P('dmEqFind'), 0),
        ('getlocal0',), ('getproperty', P('dmEqO')), ('getlocal0',), ('getproperty', P('dmEqDO')), ('ifstrictne', 'clr'),
        ('getlocal0',), ('getproperty', P('dmEqW')), ('getlocal0',), ('getproperty', P('dmEqDW')), ('ifne', 'clr'),
        ('getlocal0',), ('callproperty', P('dmEqIsSel'), 0), ('iffalse', 'sel'),
        ('getlocal0',), ('getproperty', P('mainpanel_mc')), ('callpropvoid', P('cursorAccept'), 0), ('jump', 'clr'),
        ('L', 'sel'), ('getlocal0',), ('callpropvoid', P('dmEqSel'), 0),
        ('L', 'clr'), ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmEqDW')),
        ('L', 'end'), ('returnvoid',)], c.rbody)
    # dmEqTick(e): long press = select + X. locals 9 10 scratch
    c.add('dmEqTick', 1, HEAD + num(9) + num(10) + [
        ('getlocal0',), ('getproperty', P('dmEqDW')), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('dmEqDone')), ('iftrue', 'end')] + elapsed_lt(c, 'dmEqT', HOLD_MS, 'end') + still(c, 'dmEqX', 'dmEqY', MOVE_PX, 'drop') + [
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmEqDone')),
        ('getlocal0',), ('callpropvoid', P('dmEqFind'), 0),
        ('getlocal0',), ('getproperty', P('dmEqO')), ('getlocal0',), ('getproperty', P('dmEqDO')), ('ifstrictne', 'end'),
        ('getlocal0',), ('callproperty', P('dmEqIsSel'), 0), ('iftrue', 'menu'),
        ('getlocal0',), ('callpropvoid', P('dmEqSel'), 0),
        ('L', 'menu'), ('getlocal0',), ('getproperty', P('mainpanel_mc')), ('callpropvoid', P('cursorShowActionMenu'), 0), ('jump', 'end'),
        ('L', 'drop'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmEqDone')),
        ('L', 'end'), ('returnvoid',)], c.rbody)
    stage_hook(c, 'dmEqDown', 'dmEqUp', [('getlocal0',), ('getlocal1',), ('callpropvoid', P('dmEqTick'), 1)])
    hook_init(c, 'dmT5InitOrig', ['dmT5Stage'], ['dmT5Tick'])
    return c


KINDS = {SAVE: (patch_save, ["'mouseDown'", 'setCursorPosition', 'actionPressed', 'frameHit', "'dmPadBtn'", 'btnContainer_mc', 'iconId']),
         BAR: (patch_bar, ["'mouseDown'", 'dmTtUpdOrig', 'selectSlot', "'SlotHoverOut'", "'IE UIEndTurn'", 'onEventDown', 'onEventUp', 'btnList', "'dmPadBtn'", 'dmSlotN']),
         PARTY: (patch_party, ["'mouseDown'", 'holdTimer', 'getSlotForMousePosition', 'selectItem', 'draggingItemHandle', "'dmPadBtn'", 'inMultiSelect']),
         SKILLS: (patch_skills, ["'mouseDown'", 'holdTimer', 'getSlotForMousePosition', 'selectItem', 'cursorShowActionMenu']),
         EQUIP: (patch_equip, ["'mouseDown'", 'selectSlotByPos', 'selectSlot', 'getCurrentSelection', 'cursorAccept', 'cursorShowActionMenu', 'scrollRect'])}
POST = list(KINDS)
WALKS = {SAVE: {'loadSave_mc', 'list', 'frameHit', 'content_array', 'btnContainer_mc', 'setCursorPosition', 'actionPressed', 'getCurrentMovieClip', 'iconId'},
         BAR: {'bottombar_mc', 'slotsHolder_mc', 'btnList', 'tooltipHolder_mc', 'endTurnHint_mc', 'holdEndTurnHint_mc', 'inActiveCombat', 'events', 'active',
               'isDragging', 'content_array', 'hideTooltip', 'hideHL', 'getSlot', 'selectSlot'},   # BtnInfo.iconId / Slot.inUse: dynamic, set by the stock code
         PARTY: {'inventoryPanel_mc', 'inventoryList', 'content_array', 'localInventory', 'inMultiSelect', 'selectItem'},
         SKILLS: {'mainpanel_mc', 'inventory_mc', 'localInventory', 'cursorShowActionMenu', 'selectItem'},
         EQUIP: {'mainpanel_mc', 'doll_mc', 'eqInv_mc', 'currentSelectedDoll', 'slot_array', 'selectSlotByPos', 'selectSlot', 'getCurrentSelection', 'cursorAccept',
                 'cursorShowActionMenu'}}


def patch(name, data):
    c = KINDS[name][0](data)
    return c.done(), {'methods': list(c.made), 'moved': dict(c.moved)}


def check(name, src):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(name, src)
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs(); n = len(rep['methods'])
    print('%s (taps5): %d B -> %d B, +%d methods, sha256 %s' % (name, len(src), len(out), n, hashlib.sha256(out).hexdigest()[:16]))
    say(b.ver == a.ver and b.head == a.head and len(a.tags) == len(b.tags) and all(x[0] == y[0] and (k == ta or x[1] == y[1]) for k, (x, y) in enumerate(zip(a.tags, b.tags))),
        'every tag but DoABC #%d is byte-identical (%d tags)' % (ta, len(a.tags)))
    say(B.strings[:len(A.strings)] == A.strings and B.ns[:len(A.ns)] == A.ns and B.nssets == A.nssets and B.mn[:len(A.mn)] == A.mn and B.ints[:len(A.ints)] == A.ints and B.doubles == A.doubles,
        'constant pool only appended: +%d strings, +%d multinames' % (len(B.strings) - len(A.strings), len(B.mn) - len(A.mn)))
    same = all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc, x.method) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc, y.method)
               for x, y in zip(A.bodies, B.bodies))
    say(same and len(B.bodies) == len(A.bodies) + n and len(B.methods) == len(A.methods) + n, 'all %d method bodies of the input byte-identical; %d methods appended' % (len(A.bodies), n))
    oa, ob = A.owners(), B.owners()
    moved = sorted('%s -> %s' % (oa[m].split('::')[-1], ob[m].split('::')[-1]) for m in oa if ob.get(m) != oa[m])
    want_moved = ['onEventInit -> dmT5InitOrig'] + (['updateTooltip -> dmTtUpdOrig'] if name == BAR else [])
    say(moved == sorted(want_moved), 'renamed methods: ' + ', '.join(moved))
    ta_, tb_ = [{(X.mname(k['name']), X.short(t.name)) for k in X.instances for t in k['traits']} for X in (A, B)]
    say(ta_ <= tb_, 'no trait removed (+%d)' % len(tb_ - ta_))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies (stack, scope, locals, branch targets, constant indices)' % len(B.bodies))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips')
    d = '\n'.join('\n'.join(B.dis(B.methods[m].body)) for m in rep['methods'])
    miss = [w for w in KINDS[name][1] if w not in d]
    say(not miss, 'the added code holds: %s%s' % (', '.join(KINDS[name][1]), '  MISSING ' + str(miss) if miss else ''))
    names = {X for k in B.instances for X in (B.short(t.name) for t in k['traits'])}
    lost = sorted(w for w in WALKS[name] if w not in names)
    say(not lost, 'every property the added code walks is a trait of the movie: %d%s' % (len(WALKS[name]), '  MISSING ' + str(lost) if lost else ''))
    return bad


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    if a[0] == 'build':
        if len(a) < 3: sys.exit(__doc__)
        name = os.path.basename(a[2])
        if name not in KINDS: sys.exit('output name must be one of: ' + ' '.join(KINDS))
        out, rep = patch(name, open(a[1], 'rb').read()); open(a[2], 'wb').write(out)
        print('%s: %d B, sha256 %s' % (a[2], len(out), hashlib.sha256(out).hexdigest())); return
    bad = 0
    for name in POST:
        p = Path(a[1]) / name
        if p.exists(): bad += check(name, p.read_bytes())
    print('FAILED: %d' % bad if bad else 'taps5: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
