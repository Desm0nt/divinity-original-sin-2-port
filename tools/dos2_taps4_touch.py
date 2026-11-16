"""[DOS2] Fourth tap pass (branch dos2-taps4): the panels' own tabs by tap, the skills panel (skill select / use by tap, its item half), the
party inventory multi-select. Appended methods only (every method body of the input stays byte for byte, every tag but DoABC too), over the
built `_d` panel movies (tools/dos2_panel_touch.py + tools/dos2_taps3_touch.py). docs/dos2/TOUCH_CONSOLE_PLAN.md section 10.

Everything hangs on the STAGE (mouseDown / mouseUp / click; tw2-tw3: stage listeners fire on the phone) and finds its target by geometry
when the finger lands. One onEventInit wrapper (original kept as dmT4InitOrig) adds the listeners once; an enterFrame tick retries them.

1. TABS (TABS below): press and release within TAP_MS on the same visible tab of the panel's own tab list (its element's getBounds + margin,
   live) = the movie's own tab switch:
     partyInventory_d   inventoryPanel_mc.tabs_mc.selectTab(tab.id)            = the stock CLICK closure of the tab (fn of addInvTab)
     inventorySkillPanel_d  skills_mc.tabBar_mc: tabList.selectMC(tab) + refreshPos + skills_mc.setPanel(leftPanelActive) (= nextSubTab);
                        inventory_mc.tabBar_mc: tabList.selectMC(tab) + refreshPos (= inventoryMiddle.nextSubTab)
     statsPanel_d       ExternalInterface selectStatsTab(tab.id) + its sound    (= statsMC.nextSubTab, which sends the next tab's id)
     craftPanel_d       craftPanel_mc.currentPanel = index; ExternalInterface setCurrentTab(index) + sound   (= craftPanel.subTabNext)
     trade_d            trade_mc.nextMenuTab / previousMenuTab as many times as the tapped tab is away from the current one (the pad path)
   A second (stock CLICK) switch to the same tab, if a tab's own listener also fires, is a no-op: every action selects, none steps relative
   (trade steps from the CURRENT tab, which is already the goal then).
2. SKILLS (inventorySkillPanel_d): the stock code already has "tap = select, second tap within 550 ms = cursorAccept (useSkill: memorize /
   unmemorize / use)" in a CLICK listener on skills_mc that reads stage.mouseX / Y (skillMiddle frame1 closure). tw2 showed every press
   "handled 0" (fixed in taps3 by a no-op mouseDown) and no proof that CLICK reaches it. Fallback: a release on skills_mc after a press on it
   within TAP_MS, with no click seen on the stage by the next-but-one frame, dispatches a synthetic MouseEvent "click" on skills_mc = that very
   stock closure. The first real click seen anywhere on the stage turns the fallback off for good (no double select / double use).
   The row of tools/dos2_panel_touch.py gets an "Ok" (A = cursorAccept) button. Dragging a skill to the hot bar: the Switch movie has no
   skill drag code (no startDragging in skillMiddle) - not done.
   Item half (inventory_mc.localInventory, a larInventory with no listener wired in this movie): stage press / release / tap feed the class's
   own mouseDown / mouseUp / mouseClick (owner -1: no drag start, no stopDragging); onDoubleClickCallback = inventory_mc.cursorAccept
   (ExternalInterface useItem, the pad's A). Tap = select (slotOver, tooltip), second tap = use, hold = its own hold timer.
3. PARTY INVENTORY multi-select (owner run tw4: after the row's "multi" button a tap USED the item): the stock double click
   (larInventory.mouseClick -> onDoubleClickCallback = inventoryPanel_mc.useItem) does not look at the mode. Now every larInventory of the
   panel gets onDoubleClickCallback = dmPiDbl (multi-select: nothing; else the stock useItem), and in multi-select a short tap (< TAP_MS,
   < MOVE_PX) on the current grid with a slot under it = what the pad's A does there: inventoryPanel_mc.setPaintMode(true) (toggles the
   slot: selectItem) + setPaintMode(false).

  python -I tools/dos2_taps4_touch.py check <dir with the built _d movies>
  python -I tools/dos2_taps4_touch.py build <in.swf> <out.swf>
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, verify_all
from dos2_taps_touch import Ctx, HEAD, obj, num
from dos2_taps3_touch import doc_ctx, hook_init

TAP_MS, MOVE_PX, MX, MY, CLICK_FRAMES = 1500, 24, 6, 10, 2

# movie: [(path to the tab bar from the document, list property of the bar or None = the bar is the list, action)]
TABS = {
    'partyInventory_d.swf': [(['inventoryPanel_mc', 'tabs_mc'], 'tabList', ('selectTab',))],
    'inventorySkillPanel_d.swf': [(['mainpanel_mc', 'skills_mc', 'tabBar_mc'], 'tabList', ('selectMC', ['mainpanel_mc', 'skills_mc'])),
                                  (['mainpanel_mc', 'inventory_mc', 'tabBar_mc'], 'tabList', ('selectMC', None))],
    'statsPanel_d.swf': [(['mainpanel_mc', 'stats_mc', 'tabBar_mc'], 'tabList', ('ei', 'selectStatsTab', 'UI_Game_Journal_Click'))],
    'craftPanel_d.swf': [(['craftPanel_mc', 'tabBar_mc'], 'tabList', ('craft', ['craftPanel_mc']))],
    'trade_d.swf': [(['trade_mc'], 'menuItems', ('step', ['trade_mc'], 'nextMenuTab', 'previousMenuTab'))],
}
SKILLS, PARTY = 'inventorySkillPanel_d.swf', 'partyInventory_d.swf'


class G:
    """label factory: unique names inside one method"""
    def __init__(s): s.n = 0
    def __call__(s, base): s.n += 1; return '%s_%d' % (base, s.n)


def chain(c, names, loc, fail):
    """local <loc> = this.a.b... ; every step present and visible, else goto fail"""
    P = c.P; out = [('getlocal0',), ('coerce_a',), ('setlocal', loc)]
    for n in names:
        out += [('getlocal', loc), ('getproperty', P(n)), ('coerce_a',), ('setlocal', loc), ('getlocal', loc), ('iffalse', fail),
                ('getlocal', loc), ('getproperty', P('visible')), ('iffalse', fail)]
    return out


def sxy(c):
    P = c.P
    return [('getlocal0',), ('getproperty', P('stage')), ('getproperty', P('mouseX'))], [('getlocal0',), ('getproperty', P('stage')), ('getproperty', P('mouseY'))]


def add_common(c):
    """dmTbHit(list): the visible element of list.content_array under the finger (getBounds + margin) or null; dmIdx(arr, el) -> index or -1"""
    P, ref = c.P, c.rbody
    rr = lambda f: [('getlocal', 6), ('getproperty', P(f))]
    c.add('dmTbHit', 1, HEAD + obj(2) + num(3) + num(4) + obj(5) + obj(6) + [
        ('getlocal1',), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'none'),
        ('getlocal2',), ('getproperty', P('length')), ('convert_d',), ('setlocal3',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal2',), ('getlocal', 4), ('getproperty', c.late), ('coerce_a',), ('setlocal', 5),
        ('getlocal', 5), ('iffalse', 'next'), ('getlocal', 5), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('stage')), ('iffalse', 'next'),
        ('getlocal', 5), ('getlocal', 5), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'next'),
        ] + rr('width') + [('pushbyte', 0), ('ifngt', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseX'))] + rr('x') + [('pushbyte', MX), ('subtract',), ('ifnge', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseX'))] + rr('x') + rr('width') + [('add',), ('pushbyte', MX), ('add',), ('ifnle', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseY'))] + rr('y') + [('pushbyte', MY), ('subtract',), ('ifnge', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseY'))] + rr('y') + rr('height') + [('add',), ('pushbyte', MY), ('add',), ('ifnle', 'next'),
        ('getlocal', 5), ('returnvalue',),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], ref)
    c.add('dmIdx', 2, HEAD + num(3) + num(4) + [
        ('getlocal1',), ('iffalse', 'none'), ('getlocal2',), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('length')), ('convert_d',), ('setlocal3',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal1',), ('getlocal', 4), ('getproperty', c.late), ('getlocal2',), ('ifstrictne', 'next'),
        ('getlocal', 4), ('returnvalue',),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('L', 'none'), ('pushbyte', -1), ('returnvalue',)], ref)


def tab_down(c, name, g):
    """press: remember the tab under the finger (bar, list, element, config index, time). locals 2 bar 3 list 4 el"""
    P = c.P; out = []
    done = g('tbdone')
    for k, (path, lst, act) in enumerate(TABS[name]):
        nx = g('tbnext')
        out += chain(c, path, 2, nx)
        out += [('getlocal2',), ('coerce_a',), ('setlocal3',)]
        if lst:
            out += [('getlocal2',), ('getproperty', P(lst)), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', nx),
                    ('getlocal3',), ('getproperty', P('visible')), ('iffalse', nx)]
        out += [('getlocal0',), ('getlocal3',), ('callproperty', P('dmTbHit'), 1), ('coerce_a',), ('setlocal', 4), ('getlocal', 4), ('iffalse', nx),
                ('getlocal0',), ('getlocal', 4), ('setproperty', P('dmTbE')),
                ('getlocal0',), ('getlocal3',), ('setproperty', P('dmTbL')),
                ('getlocal0',), ('getlocal2',), ('setproperty', P('dmTbB')),
                ('getlocal0',), ('pushbyte', k), ('setproperty', P('dmTbK')),
                ('getlocal0',)] + c.now() + [('setproperty', P('dmTbT')), ('jump', done),
                ('L', nx)]
    return out + [('L', done)]


def tab_up(c, name, g):
    """release on the same tab = its action. locals 2 bar 3 list 4 el 5 idx 6 cur 7 owner"""
    P, S = c.P, c.S; end = g('tbend')
    out = [('getlocal0',), ('getproperty', P('dmTbE')), ('coerce_a',), ('setlocal', 4),
           ('getlocal0',), ('pushnull',), ('setproperty', P('dmTbE')),
           ('getlocal', 4), ('iffalse', end)] + c.now() + [('getlocal0',), ('getproperty', P('dmTbT')), ('subtract',), ('pushshort', TAP_MS), ('ifgt', end),
           ('getlocal0',), ('getproperty', P('dmTbB')), ('coerce_a',), ('setlocal2',),
           ('getlocal0',), ('getproperty', P('dmTbL')), ('coerce_a',), ('setlocal3',),
           ('getlocal0',), ('getlocal3',), ('callproperty', P('dmTbHit'), 1), ('getlocal', 4), ('ifstrictne', end)]
    for k, (path, lst, act) in enumerate(TABS[name]):
        nx = g('tbk')
        out += [('getlocal0',), ('getproperty', P('dmTbK')), ('pushbyte', k), ('ifne', nx)]
        kind = act[0]
        if kind == 'selectTab':
            out += [('getlocal2',), ('getlocal', 4), ('getproperty', P('id')), ('callpropvoid', P('selectTab'), 1)]
        elif kind == 'selectMC':
            out += [('getlocal3',), ('getlocal', 4), ('callpropvoid', P('selectMC'), 1), ('getlocal2',), ('callpropvoid', P('refreshPos'), 0)]
            if act[1]:
                out += chain(c, act[1], 7, end) + [('getlocal', 7), ('getlocal', 7), ('getproperty', P('leftPanelActive')), ('callpropvoid', P('setPanel'), 1)]
        elif kind == 'ei':
            out += c.ei(2, ('pushstring', S('PlaySound')), ('pushstring', S(act[2]))) + c.ei(2, ('pushstring', S(act[1])), ('getlocal', 4), ('getproperty', P('id')))
        elif kind == 'craft':
            out += [('getlocal0',), ('getlocal3',), ('getproperty', P('content_array')), ('getlocal', 4), ('callproperty', P('dmIdx'), 2), ('convert_d',), ('setlocal', 5),
                    ('getlocal', 5), ('pushbyte', 0), ('iflt', end)] + chain(c, act[1], 7, end) + [
                    ('getlocal', 7), ('getlocal', 5), ('setproperty', P('currentPanel'))] + c.ei(2, ('pushstring', S('PlaySound')), ('pushstring', S('UI_Generic_Click'))) + \
                    c.ei(2, ('pushstring', S('setCurrentTab')), ('getlocal', 5))
        elif kind == 'step':
            lp, ln, lq = g('stp'), g('stn'), g('stq')
            out += [('getlocal0',), ('getlocal3',), ('getproperty', P('content_array')), ('getlocal', 4), ('callproperty', P('dmIdx'), 2), ('convert_d',), ('setlocal', 5),
                    ('getlocal0',), ('getlocal3',), ('getproperty', P('content_array')), ('getlocal3',), ('callproperty', P('getCurrentMovieClip'), 0), ('callproperty', P('dmIdx'), 2), ('convert_d',), ('setlocal', 6),
                    ('getlocal', 5), ('pushbyte', 0), ('iflt', end), ('getlocal', 6), ('pushbyte', 0), ('iflt', end)] + chain(c, act[1], 7, end) + [
                    ('L', lp), ('label',),
                    ('getlocal', 6), ('getlocal', 5), ('ifnlt', ln),
                    ('getlocal', 7), ('callpropvoid', P(act[2]), 0), ('getlocal', 6), ('increment',), ('convert_d',), ('setlocal', 6), ('jump', lp),
                    ('L', ln), ('label',),
                    ('getlocal', 6), ('getlocal', 5), ('ifngt', lq),
                    ('getlocal', 7), ('callpropvoid', P(act[3]), 0), ('getlocal', 6), ('decrement',), ('convert_d',), ('setlocal', 6), ('jump', ln),
                    ('L', lq)]
        out += [('jump', end), ('L', nx)]
    return out + [('L', end)]


def skills_down(c, g):
    """skills_mc press (CLICK fallback) + the item half's own mouseDown. locals 2 sk / inv 3 li"""
    P = c.P; X, Y = sxy(c); a, b = g('skd'), g('ivd')
    return chain(c, ['mainpanel_mc', 'skills_mc'], 2, a) + [
        ('getlocal2',)] + X + Y + [('pushtrue',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', a),
        ('getlocal0',), ('getlocal2',), ('setproperty', P('dmCkT')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmCkD')),
        ('L', a),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmIvOn'))] + chain(c, ['mainpanel_mc', 'inventory_mc'], 2, b) + [
        ('getlocal2',), ('getproperty', P('localInventory')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', b),
        ('getlocal3',)] + X + Y + [('pushtrue',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', b),
        ('getlocal3',), ('getlocal2',), ('getproperty', P('cursorAccept')), ('setproperty', P('onDoubleClickCallback')),
        ('getlocal3',), ('pushbyte', -1), ('pushbyte', 0), ('callpropvoid', P('mouseDown'), 2),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmIvOn')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmIvT')),
        ('L', b)]


def skills_up(c, g):
    """locals 2 sk / inv 3 li"""
    P = c.P; X, Y = sxy(c); a, b = g('sku'), g('ivu')
    return [
        ('getlocal0',), ('getproperty', P('dmCkT')), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmCkT')),
        ('getlocal2',), ('iffalse', a),
        ('getlocal0',), ('getproperty', P('dmCkSeen')), ('iftrue', a)] + c.now() + [('getlocal0',), ('getproperty', P('dmCkD')), ('subtract',), ('pushshort', TAP_MS), ('ifgt', a),
        ('getlocal2',)] + X + Y + [('pushtrue',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', a),
        ('getlocal0',), ('getlocal2',), ('setproperty', P('dmCkP')),
        ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmCkN')),
        ('L', a),
        ('getlocal0',), ('getproperty', P('dmIvOn')), ('iffalse', b),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmIvOn'))] + chain(c, ['mainpanel_mc', 'inventory_mc'], 2, b) + [
        ('getlocal2',), ('getproperty', P('localInventory')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', b),
        ('getlocal3',), ('pushbyte', -1), ('pushbyte', 0), ('callpropvoid', P('mouseUp'), 2)] + c.now() + [
        ('getlocal0',), ('getproperty', P('dmIvT')), ('subtract',), ('pushshort', TAP_MS), ('ifgt', b),
        ('getlocal3',), ('pushbyte', -1), ('pushbyte', 0), ('callpropvoid', P('mouseClick'), 2),
        ('L', b)]


def party_down(c, g):
    """every larInventory of the panel: onDoubleClickCallback = dmPiDbl; multi-select press: time + place. locals 2 arr 3 n 4 i 5 el 6 li"""
    P = c.P; X, Y = sxy(c); end, loop, nxt, cond, ms = g('pd'), g('pdl'), g('pdn'), g('pdc'), g('pdms')
    return [
        ('getlocal0',), ('getproperty', P('inventoryPanel_mc')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', end),
        ('getlocal2',), ('getproperty', P('inventoryList')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', end),
        ('getlocal2',), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', end),
        ('getlocal2',), ('getproperty', P('length')), ('convert_d',), ('setlocal3',),
        ('pushbyte', 0), ('convert_d',), ('setlocal', 4),
        ('jump', cond),
        ('L', loop), ('label',),
        ('getlocal2',), ('getlocal', 4), ('getproperty', c.late), ('coerce_a',), ('setlocal', 5), ('getlocal', 5), ('iffalse', nxt),
        ('getlocal', 5), ('getproperty', P('localInventory')), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', nxt),
        ('getlocal', 6), ('getlocal0',), ('getproperty', P('dmPiDbl')), ('setproperty', P('onDoubleClickCallback')),
        ('L', nxt), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', cond), ('getlocal', 4), ('getlocal3',), ('iflt', loop),
        ('L', end),
        ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmMsT')),
        ('getlocal0',), ('getproperty', P('inMultiSelect')), ('iffalse', ms),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmMsT')),
        ('getlocal0',)] + X + [('setproperty', P('dmMsX')),
        ('getlocal0',)] + Y + [('setproperty', P('dmMsY')),
        ('L', ms)]


def party_up(c, g):
    """multi-select tap = the pad's A there (setPaintMode true / false = toggle the slot). locals 2 panel 3 cur 4 d"""
    P = c.P; X, Y = sxy(c); end = g('pu')
    def near(v, k):
        return v + [('getlocal0',), ('getproperty', P(k)), ('subtract',), ('convert_d',), ('setlocal', 4),
                    ('getlocal', 4), ('pushbyte', MOVE_PX), ('ifgt', end), ('getlocal', 4), ('pushbyte', -MOVE_PX), ('iflt', end)]
    return [
        ('getlocal0',), ('getproperty', P('dmMsT')), ('iffalse', end),
        ('getlocal0',), ('getproperty', P('inMultiSelect')), ('iffalse', end)] + c.now() + [
        ('getlocal0',), ('getproperty', P('dmMsT')), ('subtract',), ('pushshort', 700), ('ifgt', end)] + near(X, 'dmMsX') + near(Y, 'dmMsY') + [
        ('getlocal0',), ('getproperty', P('inventoryPanel_mc')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', end),
        ('getlocal2',), ('getproperty', P('inventoryList')), ('callproperty', P('getCurrentMovieClip'), 0), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', end),
        ('getlocal3',), ('getproperty', P('isDisabled')), ('iftrue', end),
        ('getlocal3',), ('getproperty', P('localInventory')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', end),
        ('getlocal3',)] + X + Y + [('pushtrue',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', end),
        ('getlocal2',), ('callproperty', P('getCurrentSlot'), 0), ('iffalse', end),
        ('getlocal2',), ('pushtrue',), ('callpropvoid', P('setPaintMode'), 1),
        ('getlocal2',), ('pushfalse',), ('callpropvoid', P('setPaintMode'), 1),
        ('L', end),
        ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmMsT'))]


def patch_any(name, data):
    c = doc_ctx(data); P, S, abc, ref = c.P, c.S, c.abc, c.rbody
    for path, lst, act in TABS[name]:
        assert abc.trait(c.ci, path[0]) is not None, '%s: no %s on the document' % (name, path[0])
        for n in (['selectTab'] if act[0] == 'selectTab' else ['selectMC', 'refreshPos'] if act[0] == 'selectMC' else
                  ['setCurrentTab', 'currentPanel'] if act[0] == 'craft' else [act[1]] if act[0] == 'ei' else [act[2], act[3], 'getCurrentMovieClip']):
            assert n.encode() in abc.strings, '%s: the movie has no %s' % (name, n)
    add_common(c)
    gd, gu = G(), G()
    down, up = HEAD + obj(2) + obj(3) + obj(4) + obj(5) + obj(6), HEAD + obj(2) + obj(3) + obj(4) + num(5) + num(6) + obj(7)
    down += tab_down(c, name, gd); up += tab_up(c, name, gu)
    if name == SKILLS:
        for n in ('skills_mc', 'inventory_mc', 'leftPanelActive', 'setPanel', 'cursorAccept', 'mouseClick', 'onDoubleClickCallback', 'localInventory'):
            assert n.encode() in abc.strings, 'skills panel: no ' + n
        down += skills_down(c, gd); up += skills_up(c, gu)
    if name == PARTY:
        assert abc.trait(c.ci, 'inMultiSelect') is not None and abc.trait(c.ci, 'inventoryPanel_mc') is not None
        pc = abc.class_index('partyInventory_c_fla.panel_1')
        for n in ('setPaintMode', 'useItem', 'getCurrentSlot', 'inventoryList'): assert abc.trait(pc, n) is not None, 'panel_1.' + n
        # dmPiDbl(): the double click of a larInventory: multi-select = nothing (each tap toggles), else the stock useItem
        c.add('dmPiDbl', 0, HEAD + [
            ('getlocal0',), ('getproperty', P('inMultiSelect')), ('iftrue', 'end'),
            ('getlocal0',), ('getproperty', P('inventoryPanel_mc')), ('callpropvoid', P('useItem'), 0),
            ('L', 'end'), ('returnvoid',)], ref)
        down += party_down(c, gd); up += party_up(c, gu)
    c.add('dmT4Down', 1, down + [('returnvoid',)], ref)
    c.add('dmT4Up', 1, up + [('returnvoid',)], ref)
    ticks = ['dmT4Tick']
    reg = [('getlocal1',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmT4Down')), ('callpropvoid', P('addEventListener'), 2),
           ('getlocal1',), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmT4Up')), ('callpropvoid', P('addEventListener'), 2)]
    tick = HEAD + [('getlocal0',), ('callpropvoid', P('dmT4Stage'), 0)]
    if name == SKILLS:
        ME = abc.pub('MouseEvent', 'flash.events')
        c.add('dmT4Clk', 1, HEAD + [('getlocal0',), ('pushtrue',), ('setproperty', P('dmCkSeen')), ('getlocal0',), ('pushnull',), ('setproperty', P('dmCkP')), ('returnvoid',)], ref)
        reg += [('getlocal1',), ('pushstring', S('click')), ('getlocal0',), ('getproperty', P('dmT4Clk')), ('callpropvoid', P('addEventListener'), 2)]
        # no click on the stage by the CLICK_FRAMES-th frame after the release: the stock CLICK closure of skills_mc gets a synthetic one
        tick = HEAD + obj(2) + [('getlocal0',), ('callpropvoid', P('dmT4Stage'), 0),
            ('getlocal0',), ('getproperty', P('dmCkP')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'end'),
            ('getlocal0',), ('getproperty', P('dmCkSeen')), ('iftrue', 'drop'),
            ('getlocal0',), ('getlocal0',), ('getproperty', P('dmCkN')), ('pushbyte', 1), ('add',), ('setproperty', P('dmCkN')),
            ('getlocal0',), ('getproperty', P('dmCkN')), ('pushbyte', CLICK_FRAMES), ('iflt', 'end'),
            ('getlocal0',), ('pushnull',), ('setproperty', P('dmCkP')),
            ('getlocal2',), ('findpropstrict', ME), ('pushstring', S('click')), ('constructprop', ME, 1), ('callpropvoid', P('dispatchEvent'), 1),
            ('jump', 'end'),
            ('L', 'drop'), ('getlocal0',), ('pushnull',), ('setproperty', P('dmCkP')),
            ('L', 'end')]
    c.add('dmT4Stage', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmT4On')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmT4On'))] + reg + [
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmT4Tick', 1, tick + [('returnvoid',)], ref)
    hook_init(c, 'dmT4InitOrig', ['dmT4Stage'], ticks)
    return c


KINDS = {n: [] for n in TABS}
KINDS[PARTY] = ["'mouseDown'", 'dmTbHit', 'selectTab', 'setPaintMode', 'onDoubleClickCallback', 'useItem']
KINDS[SKILLS] = ["'mouseDown'", 'dmTbHit', 'selectMC', 'refreshPos', 'setPanel', "'click'", 'dispatchEvent', 'mouseClick', 'cursorAccept']
KINDS['statsPanel_d.swf'] = ["'mouseDown'", 'dmTbHit', "'selectStatsTab'"]
KINDS['craftPanel_d.swf'] = ["'mouseDown'", 'dmTbHit', "'setCurrentTab'", 'currentPanel']
KINDS['trade_d.swf'] = ["'mouseDown'", 'dmTbHit', 'nextMenuTab', 'previousMenuTab', 'getCurrentMovieClip']
POST = list(KINDS)


def patch(name, data):
    c = patch_any(name, data)
    return c.done(), {'methods': list(c.made), 'moved': dict(c.moved)}


def check(name, src):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(name, src)
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs(); n = len(rep['methods'])
    print('%s (taps4): %d B -> %d B, +%d methods, sha256 %s' % (name, len(src), len(out), n, hashlib.sha256(out).hexdigest()[:16]))
    say(b.ver == a.ver and b.head == a.head and len(a.tags) == len(b.tags) and all(x[0] == y[0] and (k == ta or x[1] == y[1]) for k, (x, y) in enumerate(zip(a.tags, b.tags))),
        'every tag but DoABC #%d is byte-identical (%d tags)' % (ta, len(a.tags)))
    say(B.strings[:len(A.strings)] == A.strings and B.ns[:len(A.ns)] == A.ns and B.nssets == A.nssets and B.mn[:len(A.mn)] == A.mn and B.ints[:len(A.ints)] == A.ints and B.doubles == A.doubles,
        'constant pool only appended: +%d strings, +%d multinames' % (len(B.strings) - len(A.strings), len(B.mn) - len(A.mn)))
    same = all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc, x.method) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc, y.method)
               for x, y in zip(A.bodies, B.bodies))
    say(same and len(B.bodies) == len(A.bodies) + n and len(B.methods) == len(A.methods) + n, 'all %d method bodies of the input byte-identical; %d methods appended' % (len(A.bodies), n))
    oa, ob = A.owners(), B.owners()
    moved = sorted('%s -> %s' % (oa[m].split('::')[-1], ob[m].split('::')[-1]) for m in oa if ob.get(m) != oa[m])
    say(moved == ['onEventInit -> dmT4InitOrig'], 'renamed methods: ' + ', '.join(moved))
    ta_, tb_ = [{(X.mname(k['name']), X.short(t.name)) for k in X.instances for t in k['traits']} for X in (A, B)]
    say(ta_ <= tb_, 'no trait removed (+%d)' % len(tb_ - ta_))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies (stack, scope, locals, branch targets, constant indices)' % len(B.bodies))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips')
    d = '\n'.join('\n'.join(B.dis(B.methods[m].body)) for m in rep['methods'])
    miss = [w for w in KINDS[name] if w not in d]
    say(not miss, 'the added code holds: %s%s' % (', '.join(KINDS[name]), '  MISSING ' + str(miss) if miss else ''))
    # every property path the added code walks exists as a trait somewhere in the movie (a typo would be a silent no-op on the phone)
    names = {X for k in B.instances for X in (B.short(t.name) for t in k['traits'])}
    want = {p for path, lst, act in TABS[name] for p in path + ([lst] if lst else []) + (act[1] if act[0] in ('selectMC', 'craft', 'step') and act[1] else [])}
    if name == SKILLS: want |= {'skills_mc', 'inventory_mc', 'localInventory', 'leftPanelActive', 'onDoubleClickCallback', 'cursorAccept'}
    if name == PARTY: want |= {'inventoryPanel_mc', 'inventoryList', 'localInventory', 'inMultiSelect', 'onDoubleClickCallback', 'isDisabled'}
    lost = sorted(w for w in want if w not in names)
    say(not lost, 'every property the added code walks is a trait of the movie: %s%s' % (', '.join(sorted(want)), '  MISSING ' + str(lost) if lost else ''))
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
    print('FAILED: %d' % bad if bad else 'taps4: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
