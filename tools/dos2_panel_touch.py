"""[DOS2] Visible touch buttons in the console PANEL movies: <panel>_c.swf -> <panel>_d.swf (docs/dos2/TOUCH_VISIBLE.md section 5, owner 15.11: every touch
function needs a visible element; the button hints at the bottom of the panels run as a marquee, so a hint tap is no answer there - do it the iPad way).

Each panel gets a row of buttons laid out as the iPad's (every original tag of the console movie stays byte for byte, only DoABC grows) on its root timeline, right-aligned to the panel's right edge, 64 px high, just above /
over the panel's top border. A press dims the button, the release on it runs its action:
    pad N   ExternalInterface "dmPadBtn"(N)  = the host presses that pad button (hle_dos2_touch.c; LS controllerHelper BTN codes: B 1, A 2, X 3, Y 4,
            Back 27, right stick press 30, LB 31, RB 32), so the panel does exactly what the pad does: the engine's input event (Eoc/controller_1.json:
            UICancel c:b, UITabPrev / Next c:leftshoulder / rightshoulder, ControllerContextMenu c:x, UIShowTooltip c:y, UIToggleHelmet /
            UICompareItems / UIToggleMultiselection / UIMarkWares / UITradeSwitchWindow c:rightstick, UIAddPoints c:a, UIRemovePoints c:x,
            UITradeBalance c:x, Combine c:a) reaches onEventDown / onEventUp of the movie and the native class as always;
    ei name ExternalInterface name() of the movie's own class - where the pad needs a HOLD (trade: hold A -> confirmOffer; reward: hold A -> acceptClicked).
Art (owner 16.11, ART below, docs/dos2/TOUCH_ART.md): our own drawings (assets/touchui: close, helmet, list, info, multi, plus, minus, balance) and,
where the iPad's button is the very drawing of a Switch movie, that symbol from the player's dump at setup time (tab arrows = characterSheet.swf
selectorBtnLeft / Right, ok = partyInventory.swf okBtn; frozen to their normal frame, no class).
ABC: 4 methods appended to the document class, onEventInit re-pointed to a wrapper (original kept as dmPanInitOrig). No original body changes.

Static only: positions come from the SWF matrices, nothing was run on a phone.

  python -I tools/dos2_panel_touch.py build <Switch Game.pak | dir> <out dir> [panel ...]
  python -I tools/dos2_panel_touch.py check <Switch Game.pak | dir> [panel ...]
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, NS_PUBLIC
import dos2_swf_graft as gr
import dos2_touchart as ta

H, GAP, RISE = 64, 8, 46          # button height on the stage, gap, how far the row stands above the panel's top edge (clamped to the stage)
# art (owner 16.11, docs/dos2/TOUCH_ART.md): ('png', name) = our own drawing in assets/touchui (tools/dos2_touchart.py);
# ('sw', movie, symbol) = a symbol of the player's Switch Game.pak, only where the iPad's button IS that drawing (same bitmaps, renders equal)
ART = {
    'close': ('png', 'pan_close'), 'prev': ('sw', 'characterSheet.swf', 'selectorBtnLeft'), 'next': ('sw', 'characterSheet.swf', 'selectorBtnRight'),
    'helmet': ('png', 'pan_helmet'), 'list': ('png', 'pan_list'), 'info': ('png', 'pan_info'),
    'multi': ('png', 'pan_multi'), 'plus': ('png', 'pan_plus'), 'minus': ('png', 'pan_minus'),
    'ok': ('sw', 'partyInventory.swf', 'okBtn'), 'balance': ('png', 'pan_balance')}
SWART = sorted({v[1] for v in ART.values() if v[0] == 'sw'})
B, A, X, Y, RS, LB, RB = 1, 2, 3, 4, 30, 31, 32
TABS = [('Prev', 'prev', ('pad', LB), 'UITabPrev'), ('Next', 'next', ('pad', RB), 'UITabNext')]
CLOSE = ('Close', 'close', ('pad', B), 'UICancel')
# panel: UI type ids, the root instance the row hangs on, buttons left to right: (name, art, action, the pad event it stands for)
CFG = {
    'partyInventory': dict(types=[0x8e], anchor='inventoryPanel_mc', btns=TABS + [('Info', 'info', ('pad', Y), 'UIShowTooltip'), ('List', 'list', ('pad', X), 'ControllerContextMenu'),
        ('Multi', 'multi', ('pad', RS), 'UIToggleMultiselection / UICompareItems'), CLOSE]),
    'equipmentPanel': dict(types=[0x40], anchor='mainpanel_mc', btns=[('List', 'list', ('pad', X), 'ControllerContextMenu'), ('Helmet', 'helmet', ('pad', RS), 'UIToggleHelmet'), CLOSE]),
    'statsPanel': dict(types=[0x3f], anchor='mainpanel_mc', btns=TABS + [('Minus', 'minus', ('pad', X), 'UIRemovePoints'), ('Plus', 'plus', ('pad', A), 'UIAddPoints'),
        ('Info', 'info', ('pad', Y), 'UIShowTooltip'), CLOSE]),
    'inventorySkillPanel': dict(types=[0x3e], anchor='mainpanel_mc', btns=TABS + [('Ok', 'ok', ('pad', A), 'UIAccept = cursorAccept: memorize / unmemorize / use the skill, use the item'), ('Info', 'info', ('pad', Y), 'UIShowTooltip'), ('List', 'list', ('pad', X), 'ControllerContextMenu'),
        ('Multi', 'multi', ('pad', RS), 'UIToggleMultiselection / UIMarkWares / UICompareItems'), CLOSE]),
    'journal': dict(types=[0x46], anchor='journal_mc', btns=TABS + [('Ok', 'ok', ('pad', A), 'UIAccept (quest on map / tracking)'), ('Info', 'info', ('pad', Y), 'UIShowTooltip'), CLOSE]),
    # the journal the Switch engine really creates (phone run tw4: "[route] type 0x8c journal_csp.swf"); same document class layout as journal_c
    # (journal_mc), + its sub tabs by a tap from the stage (jtabs: see wire_jtabs)
    'journal_csp': dict(types=[0x8c], src='journal_csp.swf', anchor='journal_mc', jtabs=True,
        btns=TABS + [('Ok', 'ok', ('pad', A), 'UIAccept (quest on map / tracking)'), ('Info', 'info', ('pad', Y), 'UIShowTooltip'), CLOSE]),
    'craftPanel': dict(types=[0x54], anchor='craftPanel_mc', btns=TABS + [('Info', 'info', ('pad', Y), 'UIShowTooltip'), ('List', 'list', ('pad', X), 'ControllerContextMenu'),
        ('Ok', 'ok', ('pad', A), 'Combine / UIAccept'), CLOSE]),
    'trade': dict(types=[0x49], anchor='trade_mc', btns=TABS + [('Info', 'info', ('pad', Y), 'UIShowTooltip'), ('Multi', 'multi', ('pad', RS), 'UITradeSwitchWindow / UICompareItems'),
        ('Balance', 'balance', ('pad', X), 'UITradeBalance'), ('Ok', 'ok', ('ei', 'confirmOffer'), 'hold UIAccept -> confirmOffer'), CLOSE]),
    'reward': dict(types=[0x89], anchor='reward_mc', btns=[('Ok', 'ok', ('ei', 'acceptClicked'), 'hold UIAccept -> acceptClicked')]),
    'examine': dict(types=[0x43], anchor='examine_mc', btns=[('Info', 'info', ('pad', Y), 'UIShowTooltip'), CLOSE]),
}
ORDER = list(CFG)
GUI = 'Public/Game/GUI/'


def src_name(p): return CFG[p].get('src', p + '_c.swf')
def out_name(p): return p + '_d.swf'
def inst(n): return 'dmPan%s_mc' % n


def _read(path, name):
    if os.path.isdir(path): return (Path(path) / name).read_bytes()
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == GUI + name]
    if not e: sys.exit('%s: no %s' % (path, name))
    return pak.read(e[0])


def source(path, panel): return _read(path, src_name(panel))
def swart(path): return {n: _read(path, n) for n in SWART}   # the Switch movies the 'sw' art comes from


def stage(swf):
    b = gr.Bits(swf.body(), 0); n = b.ub(5); r = [b.sb(n) / 20.0 for _ in range(4)]
    return r[1], r[3]


def anchor_box(swf, name):
    mv = gr.Movie(swf)
    for c, t, _ in swf.tags:
        if c in (4, 26, 70):
            rec = gr.place_parse(c, t)
            if rec['name'] == name.encode() + b'\0' and rec['char'] in mv.defs:
                m = gr.Bits(rec['matrix'], 0).matrix() if rec['matrix'] else [1, 0, 0, 1, 0, 0]
                bb = gr.bounds(swf, rec['char'], 1, mv); x, y = m[4] / 20.0, m[5] / 20.0
                return x + bb[0], y + bb[1], x + bb[2], y + bb[3]
    raise KeyError('no root instance %s' % name)


def graft(swf, sm, panel):
    """sm = swart(Switch Game.pak)"""
    cfg = CFG[panel]; rep = {'placed': [], 'frozen': 0, 'defs': 0, 'dropped': []}
    sw, sh = stage(swf); ax0, ay0, ax1, ay1 = anchor_box(swf, cfg['anchor'])
    right = min(ax1, sw); top = max(0.0, ay0 - RISE)
    srcs = {n: Swf(sm[n]) for n in SWART}
    def box_of(art):
        a = ART[art]
        if a[0] == 'png': h, w = ta.asset(a[1]).shape[:2]; return (0.0, 0.0, w * ta.PX, h * ta.PX)
        return gr.bounds(srcs[a[1]], gr.Movie(srcs[a[1]]).resolve(a[2]))
    # sizes first (right to left), then one Graft per source (each Switch movie; our PNGs)
    x = right; todo = {}
    for k in range(len(cfg['btns']) - 1, -1, -1):
        name, art, act, ev = cfg['btns'][k]
        b = box_of(art); s = H / (b[3] - b[1]); w = (b[2] - b[0]) * s
        x -= w; todo.setdefault(ART[art][1] if ART[art][0] == 'sw' else 'png', []).append((k, name, art, x - b[0] * s, top - b[1] * s, s, (x, top, x + w, top + H))); x -= GAP
    for src in SWART + ['png']:
        if src not in todo: continue
        g = gr.Graft(swf, srcs[src] if src != 'png' else None); art = ta.Art(g)
        for k, name, key, px, py, s, box in sorted(todo[src]):
            cid = g.copy(ART[key][2]) if src != 'png' else art.png(ART[key][1])[0]
            g.place(cid, px, py, inst(name), s); rep['placed'].append((k, inst(name)) + box)
        rep['frozen'] += len(g.frozen); rep['dropped'] += g.dropped; g.finish(); rep['defs'] += len(g.new_defs)
    rep['placed'].sort(); rep['stage'] = (sw, sh); rep['anchor'] = (ax0, ay0, ax1, ay1)
    return rep


def wire(swf, rep, panel):
    cfg = CFG[panel]
    (tag, pre, abc), = swf.abcs(); S = abc.string
    docs = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']).endswith('.MainTimeline')]
    assert len(docs) == 1 and not abc.instances[docs[0]]['flags'] & 1, 'document class not found or sealed'
    ci = docs[0]; ins = abc.instances[ci]
    ti = abc.trait(ci, 'onEventInit')
    assert ti is not None and ti.kind & 15 == 1 and not abc.methods[ti.b].params, 'onEventInit()'
    assert abc.trait(ci, 'dmPanInitOrig') is None, 'already patched'
    for name, art, act, ev in cfg['btns']:
        if act[0] == 'ei': assert act[1].encode() in abc.strings, 'the movie has no "%s"' % act[1]
    ns = abc.mn[ti.name][1]; assert abc.ns[ns] == (NS_PUBLIC, S('')), 'onEventInit is not public'
    P = lambda name: abc.qname(ns, name)
    EI = abc.pub('ExternalInterface', 'flash.external')
    HEAD = [('getlocal0',), ('pushscope',)]
    ref = abc.methods[ti.b].body

    def add(name, nparams, lines, like=None):
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 64, 64, ref.init_scope, ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        if like is not None:
            abc.methods[m].params = list(like.params); abc.methods[m].ret = like.ret
            if like.flags & 0x08: abc.methods[m].flags |= 0x08; abc.methods[m].options = list(like.options)
        return m

    def ei(*args): return [('getlex', EI)] + list(args) + [('callpropvoid', P('call'), len(args))]

    # dmPanHook(): once; every placed button gets its index and a press listener
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('getlocal0',), ('getproperty', P('dmPanOn')), ('iftrue', 'end'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmPanOn'))]
    for k, (name, art, act, ev) in enumerate(cfg['btns']):
        code += [('getlocal0',), ('pushstring', S(inst(name))), ('callproperty', P('getChildByName'), 1), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'n%d' % k),
            ('getlocal', 1), ('pushbyte', k), ('setproperty', P('dmCode')),
            ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseChildren')),
            ('getlocal', 1), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmPanDown')), ('callpropvoid', P('addEventListener'), 2),
            ('L', 'n%d' % k)]
    code += [('L', 'end'), ('returnvoid',)]
    m_hook = add('dmPanHook', 0, code)
    # dmPanDown(e): remember the button, dim it; the release is taken from the stage (a finger may leave the button)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
        ('getlocal1',), ('getproperty', P('currentTarget')), ('coerce_a',), ('setlocal', 2),
        ('getlocal0',), ('getlocal', 2), ('setproperty', P('dmPanB')),
        ('getlocal', 2), ('pushbyte', 6), ('pushbyte', 10), ('divide',), ('setproperty', P('alpha')),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 3),
        ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal', 3), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmPanUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)]
    m_down = add('dmPanDown', 1, code)
    # dmPanUp(e): the stage's release; on the pressed button = its action
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3), ('pushbyte', 0), ('coerce_a',), ('setlocal', 4),
        ('getlocal0',), ('getproperty', P('dmPanB')), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmPanB')),
        ('getlocal', 2), ('pushbyte', 1), ('setproperty', P('alpha')),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 3),
        ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal', 2), ('getlocal', 3), ('getproperty', P('mouseX')), ('getlocal', 3), ('getproperty', P('mouseY')), ('pushfalse',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'end'),
        ('getlocal', 2), ('getproperty', P('dmCode')), ('coerce_a',), ('setlocal', 4)]
    for k, (name, art, act, ev) in enumerate(cfg['btns']):
        code += [('getlocal', 4), ('pushbyte', k), ('ifne', 'c%d' % k)]
        code += ei(('pushstring', S('dmPadBtn')), ('pushbyte', act[1])) if act[0] == 'pad' else ei(('pushstring', S(act[1])),)
        code += [('returnvoid',), ('L', 'c%d' % k)]
    code += [('L', 'end'), ('returnvoid',)]
    m_up = add('dmPanUp', 1, code)
    ins['traits'] += [Trait(P('dmPanHook'), 1, 0, m_hook), Trait(P('dmPanDown'), 1, 0, m_down), Trait(P('dmPanUp'), 1, 0, m_up)]
    extra, calls = (wire_jtabs(abc, ins, P, S, add, HEAD) if cfg.get('jtabs') else ([], []))
    code = HEAD + [('getlocal0',), ('callpropvoid', P('dmPanInitOrig'), 0), ('getlocal0',), ('callpropvoid', P('dmPanHook'), 0)] + sum(
        ([('getlocal0',), ('callpropvoid', P(c), 0)] for c in calls), []) + [('returnvoid',)]
    m = add('onEventInit', 0, code, like=abc.methods[ti.b]); old_i = ti.b; ti.b = m; ins['traits'].append(Trait(P('dmPanInitOrig'), 1, 0, old_i))
    rep['methods'] = [m_hook, m_down, m_up] + extra + [m]
    swf.tags[tag][1] = pre + abc.write()


JT_MS, JT_PAD = 1500, 10   # tab tap: press .. release within this, hit margin (px, the tab's own space)


def wire_jtabs(abc, ins, P, S, add, HEAD):
    """journal_csp: its sub tabs by a tap, from the STAGE (element listeners did not fire on the phone, TOUCH_CONSOLE_PLAN.md sections 8-9).
    Journal mode: journal_mc.journalSubTabList (a horizontalList of journalTab); a press and a release on the same visible tab (its getBounds,
    live) = journalSubTabList.selectMC(tab, false): exactly what the pad's next / previous ends in (journalTab.selectElement ->
    selectClickedSubTab(tab, false, true) -> EI selectClickedTab + the click sound). Map mode: journal_mc.mapSubTabList (triHorizontalMenu):
    a tap on its left side list = mapSubTabList.previous(), right side = next() (what onEventUp does for UITabPrev / UITabNext there).
    The stock CLICK closure of a journalTab (if it fires) comes after the stage's mouseUp and finds the tab already selected: no double step.
    Stage listeners are added from an enterFrame tick (the stage may not exist at onEventInit), which removes itself once they are on."""
    from dos2_abc import MN_MULTIL, OPS
    late = None
    for b in abc.bodies:
        try: code = abc.ins(b.code)
        except ValueError: continue
        for _, op, ops, _ in code:
            if OPS[op][0] == 'getproperty' and abc.mn[ops[0]][0] == MN_MULTIL: late = ops[0]; break
        if late is not None: break
    assert late is not None, 'no late-bound getproperty in the movie'
    jm = abc.class_index('journal_csp_fla.journalMC_1')
    for n in ('journalSubTabList', 'mapSubTabList', 'tabHolder_mc', 'selectClickedSubTab'): assert abc.trait(jm, n) is not None, 'journalMC.' + n
    tri = abc.class_index('LS_Classes.triHorizontalMenu')
    for n in ('m_LeftSide', 'm_RightSide'): assert abc.trait(tri, n) is not None, 'triHorizontalMenu.' + n
    assert b'selectClickedTab' in abc.strings
    T = abc.pub('getTimer', 'flash.utils')
    now = [('findpropstrict', T), ('callproperty', T, 0)]
    obj = lambda n: [('pushnull',), ('coerce_a',), ('setlocal', n)]
    num = lambda n: [('pushbyte', 0), ('convert_d',), ('setlocal', n)]
    rr = lambda f: [('getlocal', 2), ('getproperty', P(f))]
    made = []
    def put(name, n, lines):
        m = add(name, n, lines); ins['traits'].append(Trait(P(name), 1, 0, m)); made.append(m)
    # dmJtIn(o): the finger inside o's own bounds (+ margin). locals 1 o 2 r
    put('dmJtIn', 1, HEAD + obj(2) + [
        ('getlocal1',), ('iffalse', 'no'), ('getlocal1',), ('getproperty', P('visible')), ('iffalse', 'no'),
        ('getlocal1',), ('getlocal1',), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'no')] +
        rr('width') + [('pushbyte', 0), ('ifngt', 'no'),
        ('getlocal1',), ('getproperty', P('mouseX'))] + rr('x') + [('pushbyte', JT_PAD), ('subtract',), ('ifnge', 'no'),
        ('getlocal1',), ('getproperty', P('mouseX'))] + rr('x') + rr('width') + [('add',), ('pushbyte', JT_PAD), ('add',), ('ifnle', 'no'),
        ('getlocal1',), ('getproperty', P('mouseY'))] + rr('y') + [('pushbyte', JT_PAD), ('subtract',), ('ifnge', 'no'),
        ('getlocal1',), ('getproperty', P('mouseY'))] + rr('y') + rr('height') + [('add',), ('pushbyte', JT_PAD), ('add',), ('ifnle', 'no'),
        ('pushtrue',), ('returnvalue',),
        ('L', 'no'), ('pushfalse',), ('returnvalue',)])
    # dmJtFind(): the journal tab / the map's left or right side list under the finger, or null. locals 1 jm 2 l 3 arr 4 n 5 i 6 el
    put('dmJtFind', 0, HEAD + obj(1) + obj(2) + obj(3) + num(4) + num(5) + obj(6) + [
        ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal0',), ('getproperty', P('journal_mc')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('journalSubTabList')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'map'),
        ('getlocal2',), ('getproperty', P('visible')), ('iffalse', 'map'),
        ('getlocal1',), ('getproperty', P('tabHolder_mc')), ('getproperty', P('visible')), ('iffalse', 'map'),
        ('getlocal2',), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'map'),
        ('getlocal3',), ('getproperty', P('length')), ('convert_d',), ('setlocal', 4),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal3',), ('getlocal', 5), ('getproperty', late), ('coerce_a',), ('setlocal', 6),
        ('getlocal0',), ('getlocal', 6), ('callproperty', P('dmJtIn'), 1), ('iffalse', 'next'),
        ('getlocal', 6), ('returnvalue',),
        ('L', 'next'), ('getlocal', 5), ('increment',), ('convert_d',), ('setlocal', 5),
        ('L', 'cond'), ('getlocal', 5), ('getlocal', 4), ('iflt', 'loop'),
        ('L', 'map'),
        ('getlocal1',), ('getproperty', P('mapSubTabList')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'none'),
        ('getlocal2',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal2',), ('getproperty', P('m_LeftSide')), ('coerce_a',), ('setlocal', 6),
        ('getlocal0',), ('getlocal', 6), ('callproperty', P('dmJtIn'), 1), ('iffalse', 'right'), ('getlocal', 6), ('returnvalue',),
        ('L', 'right'),
        ('getlocal2',), ('getproperty', P('m_RightSide')), ('coerce_a',), ('setlocal', 6),
        ('getlocal0',), ('getlocal', 6), ('callproperty', P('dmJtIn'), 1), ('iffalse', 'none'), ('getlocal', 6), ('returnvalue',),
        ('L', 'none'), ('pushnull',), ('returnvalue',)])
    put('dmJtDown', 1, HEAD + [('getlocal0',), ('getlocal0',), ('callproperty', P('dmJtFind'), 0), ('setproperty', P('dmJtP')),
        ('getlocal0',)] + now + [('setproperty', P('dmJtT')), ('returnvoid',)])
    # dmJtUp(e). locals 1 e 2 p 3 jm 4 m
    put('dmJtUp', 1, HEAD + obj(2) + obj(3) + obj(4) + [
        ('getlocal0',), ('getproperty', P('dmJtP')), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmJtP')),
        ('getlocal2',), ('iffalse', 'end')] + now + [('getlocal0',), ('getproperty', P('dmJtT')), ('subtract',), ('pushshort', JT_MS), ('ifgt', 'end'),
        ('getlocal0',), ('callproperty', P('dmJtFind'), 0), ('getlocal2',), ('ifstrictne', 'end'),
        ('getlocal0',), ('getproperty', P('journal_mc')), ('coerce_a',), ('setlocal3',),
        ('getlocal3',), ('getproperty', P('mapSubTabList')), ('coerce_a',), ('setlocal', 4),
        ('getlocal2',), ('getlocal', 4), ('getproperty', P('m_LeftSide')), ('ifstrictne', 'r'),
        ('getlocal', 4), ('callpropvoid', P('previous'), 0), ('returnvoid',),
        ('L', 'r'),
        ('getlocal2',), ('getlocal', 4), ('getproperty', P('m_RightSide')), ('ifstrictne', 'tab'),
        ('getlocal', 4), ('callpropvoid', P('next'), 0), ('returnvoid',),
        ('L', 'tab'),
        ('getlocal3',), ('getproperty', P('journalSubTabList')), ('getlocal2',), ('pushfalse',), ('callpropvoid', P('selectMC'), 2),
        ('L', 'end'), ('returnvoid',)])
    # dmJtTick(e): the stage listeners once the stage exists, then the tick goes away. locals 1 e 2 st
    put('dmJtTick', 1, HEAD + obj(2) + [
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'end'),
        ('getlocal2',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmJtDown')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal2',), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmJtUp')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal0',), ('pushstring', S('enterFrame')), ('getlocal0',), ('getproperty', P('dmJtTick')), ('callpropvoid', P('removeEventListener'), 2),
        ('L', 'end'), ('returnvoid',)])
    put('dmJtHook', 0, HEAD + [
        ('getlocal0',), ('getproperty', P('dmJtOn')), ('iftrue', 'end'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmJtOn')),
        ('getlocal0',), ('pushstring', S('enterFrame')), ('getlocal0',), ('getproperty', P('dmJtTick')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)])
    return made, ['dmJtHook']


def patch(data, sm, panel):
    """<panel>_c bytes + swart(Switch Game.pak) -> (<panel>_d bytes, report)"""
    swf = Swf(data); rep = graft(swf, sm, panel); wire(swf, rep, panel)
    return swf.write(), rep


def check(src, sm, panel, built=None):
    bad = 0; cfg = CFG[panel]
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(src, sm, panel)
    if built is not None: say(built == out, 'the given %s is what this tool builds from these sources (%d B)' % (out_name(panel), len(built)))
    a, b = Swf(src), Swf(out); (ta, pa, A_), = a.abcs(); (tb, pb, B_), = b.abcs()
    print('%s -> %s: %d B -> %d B, +%d definition tags, +%d root instances, %d sprites frozen to one frame, +%d methods, sha256 %s' % (
        src_name(panel), out_name(panel), len(src), len(out), rep['defs'], len(rep['placed']), rep['frozen'], len(rep['methods']), hashlib.sha256(out).hexdigest()[:16]))
    say(not gr.check(src, out, quiet=True), 'graft: every original tag byte for byte and in order (DoABC aside), new ids unique, every use after its definition, re-read equal')
    for art in sorted({bt[1] for bt in cfg['btns']}):
        if ART[art][0] != 'sw': continue
        _, mvn, sym = ART[art]
        say(not gr.selftest(sm[mvn], gr.Movie(Swf(sm[mvn])).resolve(sym), src, quiet=True), 'graft round trip Switch %s %s: copied tree mapped back = its tags byte for byte' % (mvn, sym))
    say(B_.strings[:len(A_.strings)] == A_.strings and B_.ns[:len(A_.ns)] == A_.ns and B_.nssets == A_.nssets and B_.mn[:len(A_.mn)] == A_.mn and B_.ints == A_.ints and B_.doubles == A_.doubles,
        'constant pool only appended: +%d strings, +%d multinames' % (len(B_.strings) - len(A_.strings), len(B_.mn) - len(A_.mn)))
    same = all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc, x.method) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc, y.method)
               for x, y in zip(A_.bodies, B_.bodies))
    n = len(rep['methods'])
    say(same and len(B_.bodies) == len(A_.bodies) + n and len(B_.methods) == len(A_.methods) + n, 'all %d original method bodies byte-identical; %d methods appended' % (len(A_.bodies), n))
    oa, ob = A_.owners(), B_.owners()
    moved = sorted('%s -> %s' % (oa[m].split('::')[-1], ob[m].split('::')[-1]) for m in oa if ob.get(m) != oa[m])
    say(moved == ['onEventInit -> dmPanInitOrig'] and len(ob) == len(oa) + n, 'renamed methods: %s; no method removed' % ', '.join(moved))
    ea = {s for s in A_.strings}; say(all(s in set(B_.strings) for s in ea), 'no string (ExternalInterface name) removed')
    errs = verify_all(B_)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies' % len(B_.bodies))
    say(Swf(Swf(out).write()).body() == b.body() and B_.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips')
    ib = {v: k for k, v in ob.items()}; doc = [B_.mname(c['name']) for c in B_.instances if B_.mname(c['name']).endswith('.MainTimeline')][0]
    d = '\n'.join(B_.dis(B_.methods[ib[doc + '::dmPanUp']].body))
    want = {'dmPadBtn' if act[0] == 'pad' else act[1] for _, _, act, _ in cfg['btns']}
    say(all("'%s'" % s in d for s in want) and 'hitTestPoint' in d, 'release on the pressed button -> %s' % ' / '.join(sorted(want)))
    if cfg.get('jtabs'):
        dj = '\n'.join('\n'.join(B_.dis(B_.methods[m].body)) for m in rep['methods'])
        want_j = ['selectMC', 'journalSubTabList', 'm_LeftSide', 'm_RightSide', "'mouseUp'", "'enterFrame'", 'removeEventListener', 'getBounds']
        say(all(w in dj for w in want_j), 'sub tabs by a stage tap: %s' % ', '.join(want_j))
    names = {gr.place_parse(c, t)['name'] for c, t, _ in b.tags if c in (4, 26, 70)}
    say(all((inst(bt[0]).encode() + b'\0') in names for bt in cfg['btns']), 'all %d instances are on the root timeline by name' % len(cfg['btns']))
    boxes = rep['placed']; sw, sh = rep['stage']
    clash = [(p[1], q[1]) for i, p in enumerate(boxes) for q in boxes[i + 1:] if p[2] < q[4] and q[2] < p[4] and p[3] < q[5] and q[3] < p[5]]
    inside = all(-0.01 <= p[2] and p[4] <= sw + 0.01 and -0.01 <= p[3] and p[5] <= sh + 0.01 for p in boxes)
    say(not clash and inside, 'the new elements are inside the %gx%g stage and do not overlap each other%s' % (sw, sh, ': ' + str(clash) if clash else ''))
    print('       panel %s on the stage: %.0f,%.0f .. %.0f,%.0f' % ((cfg['anchor'],) + rep['anchor']))
    for (k, nm, x0, y0, x1, y1), (name, art, act, ev) in zip(boxes, cfg['btns']):
        print('       %-18s %-7s stage %7.1f,%6.1f .. %7.1f,%6.1f  %s -> %s' % (nm, art, x0, y0, x1, y1, 'dmPadBtn(%d)' % act[1] if act[0] == 'pad' else 'EI ' + act[1], ev))
    print('       class links of the Switch movies not carried over (plain MovieClips here): %s' % ', '.join(sorted({n.split('.')[-1] for _, n in rep['dropped']})))
    return bad


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    sm = swart(a[1]); bad = 0
    if a[0] == 'build':
        if len(a) < 3: sys.exit(__doc__)
        os.makedirs(a[2], exist_ok=True)
        for p in a[3:] or ORDER:
            out, rep = patch(source(a[1], p), sm, p); open(os.path.join(a[2], out_name(p)), 'wb').write(out)
            print('%s: %d B, sha256 %s' % (out_name(p), len(out), hashlib.sha256(out).hexdigest()))
        return
    for p in a[2:] or ORDER: bad += check(source(a[1], p), sm, p)
    print('FAILED: %d' % bad if bad else 'panel touch elements: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
