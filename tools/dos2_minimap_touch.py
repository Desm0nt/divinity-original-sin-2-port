"""[DOS2] Tap on the minimap opens the map, as on the iPad (minimap_t: a tap on the map -> openMap). minimap_c.swf -> minimap_d.swf.

Owner 15.11 (pack 2z): "how else than '-' to open the map?". The console minimap (minimap_c, UI type 0x3c, 357x500 anchored top right) is display
only: no listener at all (docs/dos2/TOUCH_UI_AUDIT.md, minimap section, verdict B). This adds one: a press on map_mc (the drawn map, frame and
tension ring) dims it, the release of the stage on map_mc calls ExternalInterface dmPadBtn(27) = the pad's '-' (controller_1.json ToggleMap <-
c:back; host: hle_dos2_touch.c dt_ei_padbtn), the same as the map button of bottomBar_d's column (tools/dos2_hud_touch.py).
ABC: 3 methods appended to minimap_c_fla.MainTimeline, onEventInit re-pointed to a wrapper (the original kept as dmMmInitOrig); no original body,
no original tag changes. The host routes the mouse to type 0x3c (dm/host/hle_dos2_touchui_route.c group hud) and swaps the path
(hle_dos2_touchui.c k_hint). Static only, not run on a phone.

Tactical camera button (owner 15.11, branch dos2-dpadbtns: "every game function needs a visible element"; the pad's d-pad up =
ToggleTacticalCamera, Engine.pak PlayerProfiles/Eoc/controller_1.json): the iPad's minimap_t has tacticalCamera_mc (a cmnToggleButton: its
plate cmnToggleButtonBgMc frame 1 + ico_tacticalCamera centred in iconContainer_mc) left of its ring. That art is grafted in (tools/dos2_swf_graft.py,
iPad minimap_t of the owner's IPA) as dmVisTac_mc at the iPad's offset from the ring's centre (minimap_t: ring centre 1247,187, button 980.5,104.5;
minimap_c: ring centre 177,177 -> button at document x -89.5, y 94.5). That is left of minimap_c's 357 px stage, and Iggy draws a movie only inside
its own rect (pack 3i), so the frame grows by MM_DX px to the left (357x500 -> 457x500, the anchor stays topRight) and onEventInit moves every
child of the document right by MM_DX: every stock element stays where it was on the screen (the host's minimap geometry follows the rect's right side,
hle_dos2_touch.c dt_minimap_fit). Press dims the button, the stage's release on it = ExternalInterface dmPadBtn(21) = the pad's d-pad up.
Without the iPad movie (patch(data) with ip None) the movie is the old one: tap on the map only, stock frame.

Pack 4a on the phone (branch dos2-dpad2): the tactical button did not react, and neither did the map tap ever (runs t3r..t3w: every map
tap was the host's fallback "minimap tap at .. (no UI object took the press)"). The minimap UI object (type 0x3c) gets mouse MOVES through the
routing hook but never a mouse BUTTON in Iggy: in every DM_DBG run the "Iggy button #" lines name bottomBar_d / playerInfo_d / dmColumn_d /
the menus, never minimap_d. So no Flash listener of this movie can fire. The tap is the host's: the button's HUD rect is reported as
"btn21" (tools/dos2_hudrect_touch.py) and a world tap inside it presses d-pad up (hle_dos2_touch.c dt_hint_tap). The listeners below stay
(harmless; if a press ever reaches them the host's 250 ms pulse guard drops the second press).
Owner (pack 4a): the button sits TAC_DX px further right than the iPad's offset, next to the ring's left edge (window centre ~1957,170 on
2400x1080), not under the on-screen pad buttons.

  python -I tools/dos2_minimap_touch.py build <Switch Game.pak | minimap_c.swf> <out.swf> [<iPad Game.pak | dir with minimap_t.swf>]
  python -I tools/dos2_minimap_touch.py check <Switch Game.pak | minimap_c.swf> [<iPad Game.pak | dir with minimap_t.swf>]
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, NS_PUBLIC
import dos2_swf_graft as gr
from dos2_column_touch import rect_bytes

MOVIE = 'minimap_d.swf'   # same length as minimap_c.swf: the creator's path is rewritten in place (hle_dos2_touchui.c k_hint, type 0x3c)
SRC = 'Public/Game/GUI/minimap_c.swf'
MAP_CODE = 27             # LS_Classes.controllerHelper BTN_* code of the pad's '-' (hle_dos2_touch.c dt_btn_of_hint: 27 = DM_BTN_MINUS)
TAC_CODE = 21             # the pad's d-pad up = ToggleTacticalCamera (dt_btn_of_hint: 21 = DM_BTN_UP)
IPAD = 'minimap_t.swf'
MM_W, MM_H = 357, 500     # minimap_c's stage (the host's DT_RING_R 308 = the ring's right side in it)
MM_DX = 100               # px the frame grows to the left (the iPad offset put the button's left side at stage x ~10; kept: the phone showed this frame right)
TAC_DX = 72               # owner 15.11 (pack 4a): the button this many document px right of the iPad's offset = its right side on the ring's left edge
                          # (stage 82.5..149.5, ring's left side ~146): window centre ~1957 on 2400x1080 (the owner's circle: ~1960, 170)


def source(path):
    if os.path.isdir(path): return (Path(path) / 'minimap_c.swf').read_bytes()
    data = open(path, 'rb').read()
    if data[:3] in (b'CWS', b'FWS', b'ZWS'): return data
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == SRC]
    if not e: sys.exit('%s: no %s' % (path, SRC))
    return pak.read(e[0])


def ipad(path):
    """minimap_t.swf of the owner's iPad Game.pak (or a directory / the file itself)"""
    if os.path.isdir(path): return (Path(path) / IPAD).read_bytes()
    data = open(path, 'rb').read()
    if data[:3] in (b'CWS', b'FWS', b'ZWS'): return data
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == 'Public/Game/GUI/' + IPAD]
    if not e: sys.exit('%s: no %s' % (path, IPAD))
    return pak.read(e[0])


def _xy(rec):
    m = gr.Bits(rec['matrix'], 0).matrix() if rec['matrix'] else [1.0, 0.0, 0.0, 1.0, 0, 0]
    assert abs(m[0] - 1) < 1e-3 and abs(m[3] - 1) < 1e-3 and not m[1] and not m[2], 'scaled / rotated placement'
    return m[4] / 20.0, m[5] / 20.0


def _root(swf, name):
    for code, d, _ in swf.tags:
        if code in (4, 26, 70):
            r = gr.place_parse(code, d)
            if r['name'] == name.encode() + b'\0': return r
    raise KeyError('no root instance ' + name)


def _child(swf, mv, cid, name):
    code, d, _ = swf.tags[mv.defs[cid]]
    for r in gr.frame_state(d, 1).values():
        if r['name'] == name.encode() + b'\0': return r
    raise KeyError('no %s in character %d' % (name, cid))


def ring_centre(swf):
    """the minimap ring's centre in document px: map_mc -> mapHolder_mc -> mask_mc (the round map mask) + the mask's box centre"""
    mv = gr.Movie(swf); mm = _root(swf, 'map_mc'); h = _child(swf, mv, mm['char'], 'mapHolder_mc'); k = _child(swf, mv, h['char'], 'mask_mc')
    b = gr.bounds(swf, k['char'], 1, mv)
    (x0, y0), (x1, y1), (x2, y2) = _xy(mm), _xy(h), _xy(k)
    return x0 + x1 + x2 + (b[0] + b[2]) / 2.0, y0 + y1 + y2 + (b[1] + b[3]) / 2.0


def graft_tac(swf, ip):
    """the iPad's tactical camera button -> dmVisTac_mc. Returns (box (x0, y0, x1, y1) in document px, before the MM_DX shift; report)"""
    t = Swf(ip); tm = gr.Movie(t)
    tc = _root(t, 'tacticalCamera_mc'); tx, ty = _xy(tc)
    icx, icy = ring_centre(t); ccx, ccy = ring_centre(swf)
    bg = _child(t, tm, tc['char'], 'bg_mc')['char']; ic = _xy(_child(t, tm, tc['char'], 'iconContainer_mc'))
    g = gr.Graft(swf, t)
    nb = g.copy(bg); ni = g.copy('ico_tacticalCamera')   # frozen to frame 1: the plate's / the icon's common state
    b = gr.bounds(t, tm.resolve('ico_tacticalCamera'))
    btn = g.sprite([(nb, 1, (0, 0), None), (ni, 2, (ic[0] - (b[0] + b[2]) / 2.0, ic[1] - (b[1] + b[3]) / 2.0), None)])   # setIcon: centred in iconContainer_mc
    x, y = ccx + (tx - icx) + TAC_DX, ccy + (ty - icy)
    g.place(btn, x, y, 'dmVisTac_mc'); frozen, defs = len(g.frozen), len(g.new_defs); g.finish()
    w = gr.bounds(t, bg)
    return (x + w[0], y + w[1], x + w[2], y + w[3]), {'ipad ring': (icx, icy), 'ipad button': (tx, ty), 'ring': (ccx, ccy), 'frozen': frozen, 'defs': defs}


def patch(data, ip=None):
    """minimap_c bytes [+ minimap_t bytes: the tactical camera button] -> (minimap_d bytes, report)"""
    swf = Swf(data)
    tac = graft_tac(swf, ip) if ip is not None else None
    if tac: swf.head = rect_bytes(MM_W + MM_DX, MM_H) + swf.head[-4:]   # the frame grows to the left (the document moves right in dmMmHook)
    (tag, pre, abc), = swf.abcs(); S = abc.string
    docs = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']) == 'minimap_c_fla.MainTimeline']
    assert len(docs) == 1 and not abc.instances[docs[0]]['flags'] & 1, 'document class not found or sealed'
    ci = docs[0]; inst = abc.instances[ci]
    ti = abc.trait(ci, 'onEventInit')
    assert ti is not None and ti.kind & 15 == 1 and not abc.methods[ti.b].params, 'onEventInit()'
    assert abc.trait(ci, 'map_mc') is not None, 'no map_mc'
    assert abc.trait(ci, 'dmMmInitOrig') is None, 'already patched'
    ns = abc.mn[ti.name][1]; assert abc.ns[ns] == (NS_PUBLIC, S('')), 'onEventInit is not public'
    P = lambda name: abc.qname(ns, name)
    EI = abc.pub('ExternalInterface', 'flash.external')
    HEAD = [('getlocal0',), ('pushscope',)]
    ref = abc.methods[ti.b].body

    def add(name, nparams, lines, like=None):
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 64, 64, ref.init_scope, ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        if like is not None: abc.methods[m].params = list(like.params); abc.methods[m].ret = like.ret
        return m

    # dmMmHook(): once; map_mc gets the press listener (its children do not take the mouse: the whole map is one button). With the tactical
    # button: every child of the document moves right by MM_DX (the frame grew by that on the left; not this.x: TOUCH_CONSOLE_PLAN.md 13 - writing
    # the document's x showed no effect in Iggy, moving children does) and dmVisTac_mc gets its press listener. locals 1 mc 2 n 3 i 4 child
    tc = [] if not tac else [('pushbyte', 0), ('convert_d',), ('setlocal', 2), ('pushbyte', 0), ('convert_d',), ('setlocal', 3), ('pushnull',), ('coerce_a',), ('setlocal', 4),
        ('getlocal0',), ('getproperty', P('numChildren')), ('convert_d',), ('setlocal', 2),
        ('jump', 'sc'),
        ('L', 'sl'), ('label',),
        ('getlocal0',), ('getlocal', 3), ('callproperty', P('getChildAt'), 1), ('coerce_a',), ('setlocal', 4), ('getlocal', 4), ('iffalse', 'sn'),
        ('getlocal', 4), ('getlocal', 4), ('getproperty', P('x')), ('pushbyte', MM_DX), ('add',), ('setproperty', P('x')),
        ('L', 'sn'), ('getlocal', 3), ('increment',), ('convert_d',), ('setlocal', 3),
        ('L', 'sc'), ('getlocal', 3), ('getlocal', 2), ('iflt', 'sl'),
        ('getlocal0',), ('pushstring', S('dmVisTac_mc')), ('callproperty', P('getChildByName'), 1), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'nt'),
        ('getlocal1',), ('pushfalse',), ('setproperty', P('mouseChildren')),
        ('getlocal1',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmTcDown')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'nt')]
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal1',),
        ('getlocal0',), ('getproperty', P('dmMmOn')), ('iftrue', 'end'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmMmOn'))] + tc + [
        ('getlocal0',), ('getproperty', P('map_mc')), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('map_mc')), ('pushfalse',), ('setproperty', P('mouseChildren')),
        ('getlocal0',), ('getproperty', P('map_mc')), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmMmDown')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)]
    m_hook = add('dmMmHook', 0, code)
    # dmMmDown(e): dim, wait for the stage's release
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmMmPressed')),
        ('getlocal0',), ('getproperty', P('map_mc')), ('pushbyte', 7), ('pushbyte', 10), ('divide',), ('setproperty', P('alpha')),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmMmUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)]
    m_down = add('dmMmDown', 1, code)
    # dmMmUp(e): released on the map = the pad's '-' (open / close the map)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2),
        ('getlocal0',), ('getproperty', P('dmMmPressed')), ('iffalse', 'end'),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmMmPressed')),
        ('getlocal0',), ('getproperty', P('map_mc')), ('pushbyte', 1), ('setproperty', P('alpha')),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('map_mc')), ('getlocal', 2), ('getproperty', P('mouseX')), ('getlocal', 2), ('getproperty', P('mouseY')), ('pushfalse',),
        ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'end'),
        ('getlex', EI), ('pushstring', S('dmPadBtn')), ('pushbyte', MAP_CODE), ('callpropvoid', P('call'), 2),
        ('L', 'end'), ('returnvoid',)]
    m_up = add('dmMmUp', 1, code)
    made = [m_hook, m_down, m_up]
    inst['traits'] += [Trait(P('dmMmHook'), 1, 0, m_hook), Trait(P('dmMmDown'), 1, 0, m_down), Trait(P('dmMmUp'), 1, 0, m_up)]
    if tac:
        # dmTcDown(e) / dmTcUp(e): the tactical camera button, the same press / release as the map. locals 2 mc 3 stage
        tmc = [('getlocal0',), ('pushstring', S('dmVisTac_mc')), ('callproperty', P('getChildByName'), 1), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end')]
        code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3)] + tmc + [
            ('getlocal0',), ('pushtrue',), ('setproperty', P('dmTcPressed')),
            ('getlocal', 2), ('pushbyte', 6), ('pushbyte', 10), ('divide',), ('setproperty', P('alpha')),
            ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'end'),
            ('getlocal', 3), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmTcUp')), ('callpropvoid', P('addEventListener'), 2),
            ('L', 'end'), ('returnvoid',)]
        m_tdown = add('dmTcDown', 1, code)
        code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
            ('getlocal0',), ('getproperty', P('dmTcPressed')), ('iffalse', 'end'),
            ('getlocal0',), ('pushfalse',), ('setproperty', P('dmTcPressed'))] + tmc + [
            ('getlocal', 2), ('pushbyte', 1), ('setproperty', P('alpha')),
            ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'end'),
            ('getlocal', 2), ('getlocal', 3), ('getproperty', P('mouseX')), ('getlocal', 3), ('getproperty', P('mouseY')), ('pushfalse',),
            ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'end'),
            ('getlex', EI), ('pushstring', S('dmPadBtn')), ('pushbyte', TAC_CODE), ('callpropvoid', P('call'), 2),
            ('L', 'end'), ('returnvoid',)]
        m_tup = add('dmTcUp', 1, code)
        made += [m_tdown, m_tup]
        inst['traits'] += [Trait(P('dmTcDown'), 1, 0, m_tdown), Trait(P('dmTcUp'), 1, 0, m_tup)]
    code = HEAD + [('getlocal0',), ('callpropvoid', P('dmMmInitOrig'), 0), ('getlocal0',), ('callpropvoid', P('dmMmHook'), 0), ('returnvoid',)]
    m = add('onEventInit', 0, code, like=abc.methods[ti.b]); old = ti.b; ti.b = m; inst['traits'].append(Trait(P('dmMmInitOrig'), 1, 0, old))
    swf.tags[tag][1] = pre + abc.write()
    return swf.write(), {'methods': made + [m], 'moved': old, 'tac': tac}


def check(src, ip=None, built=None):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(src, ip)
    if built is not None: say(built == out, 'the given %s is what this tool builds (%d B)' % (MOVIE, len(built)))
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs()
    print('minimap_c.swf -> %s: %d B -> %d B, +%d methods, sha256 %s' % (MOVIE, len(src), len(out), len(rep['methods']), hashlib.sha256(out).hexdigest()[:16]))
    tac = rep['tac']
    if not tac:
        say([t[:2] for i, t in enumerate(a.tags) if i != ta] == [t[:2] for i, t in enumerate(b.tags) if i != tb] and len(a.tags) == len(b.tags), 'every tag but DoABC byte for byte')
    else:
        g = gr.check(src, out, quiet=True)
        say(g == ['header / tail differ'], 'graft: every original tag of minimap_c kept byte for byte and in order (DoABC aside), new ids unique, uses after definitions; '
            'only the frame in the header changed%s' % ('' if g == ['header / tail differ'] else ': ' + str(g)))
        say(b.head[:-4] == rect_bytes(MM_W + MM_DX, MM_H), 'frame %dx%d: minimap_c\'s %dx%d grown %d px to the left (dmMmHook moves the document\'s children right by that)' % (MM_W + MM_DX, MM_H, MM_W, MM_H, MM_DX))
        say(not gr.selftest(ip, gr.Movie(Swf(ip)).resolve('ico_tacticalCamera'), src, quiet=True), 'graft round trip minimap_t ico_tacticalCamera: copied tree mapped back = the iPad tags byte for byte')
        box, r = tac; x0, y0, x1, y1 = box[0] + MM_DX, box[1], box[2] + MM_DX, box[3]
        names = {gr.place_parse(c, t)['name'] for c, t, _ in b.tags if c in (4, 26, 70)}
        say(b'dmVisTac_mc\0' in names and 0 <= x0 and x1 <= MM_W + MM_DX and 0 <= y0 and y1 <= MM_H and x1 < MM_DX + r['ring'][0] - 120,
            'dmVisTac_mc on the root timeline, inside the frame and left of the ring: stage %.1f,%.1f .. %.1f,%.1f (document x %.1f = the iPad offset + TAC_DX %d; iPad ring centre %.0f,%.0f, button %.1f,%.1f; '
            'minimap_c ring centre %.0f,%.0f) = window %.0f..%.0f x %.0f..%.0f on the 2400x1080 phone (scale 1.3, right-anchored)' % (
                x0, y0, x1, y1, box[0], TAC_DX, r['ipad ring'][0], r['ipad ring'][1], r['ipad button'][0], r['ipad button'][1], r['ring'][0], r['ring'][1],
                2400 - (MM_W + MM_DX - x0) * 1.3, 2400 - (MM_W + MM_DX - x1) * 1.3, y0 * 1.3, y1 * 1.3))
    say(B.strings[:len(A.strings)] == A.strings and B.mn[:len(A.mn)] == A.mn and B.ns[:len(A.ns)] == A.ns, 'constant pool only appended')
    same = all(x.code == y.code for x, y in zip(A.bodies, B.bodies)) and len(B.bodies) == len(A.bodies) + len(rep['methods'])
    say(same, 'all %d original method bodies byte-identical; %d appended' % (len(A.bodies), len(rep['methods'])))
    oa, ob = A.owners(), B.owners()
    moved = sorted('%s -> %s' % (oa[m].split('::')[-1], ob[m].split('::')[-1]) for m in oa if ob.get(m) != oa[m])
    say(moved == ['onEventInit -> dmMmInitOrig'], 'renamed methods: ' + ', '.join(moved))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies' % len(B.bodies))
    ib = {v: k for k, v in ob.items()}
    d = '\n'.join(B.dis(B.methods[ib['minimap_c_fla.MainTimeline::dmMmUp']].body))
    say("'dmPadBtn'" in d and 'pushbyte         %d' % MAP_CODE in d and 'hitTestPoint' in d, 'release on map_mc -> ExternalInterface dmPadBtn(%d) = pad minus (ToggleMap)' % MAP_CODE)
    if tac:
        d = '\n'.join(B.dis(B.methods[ib['minimap_c_fla.MainTimeline::dmTcUp']].body)); h = '\n'.join(B.dis(B.methods[ib['minimap_c_fla.MainTimeline::dmMmHook']].body))
        say("'dmPadBtn'" in d and 'pushbyte         %d' % TAC_CODE in d and 'hitTestPoint' in d and 'dmTcDown' in h and 'pushbyte         %d' % MM_DX in h and 'getChildAt' in h,
            'release on dmVisTac_mc -> ExternalInterface dmPadBtn(%d) = d-pad up (ToggleTacticalCamera); dmMmHook moves every document child by %d' % (TAC_CODE, MM_DX))
    say(Swf(Swf(out).write()).body() == b.body(), 'the patched movie parses and round-trips')
    return bad


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    src = source(a[1])
    if a[0] == 'build':
        if len(a) < 3: sys.exit(__doc__)
        out, _ = patch(src, ipad(a[3]) if len(a) > 3 else None); open(a[2], 'wb').write(out); print('%s: %d B, sha256 %s' % (a[2], len(out), hashlib.sha256(out).hexdigest())); return
    bad = check(src, ipad(a[2]) if len(a) > 2 else None)
    print('FAILED: %d' % bad if bad else 'minimap tap: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
