"""[DOS2] The iPad button column as its own UI movie, anchored by the game to the right screen edge (owner 15.11, packs 3i..3o: the column
grafted into bottomBar_d could not reach the edge - Iggy draws a movie only inside its FlashPlayer rect, bottomBar_c is a centred 1920x1080
"fixed" frame - and fillVFit left-aligned the hot bar). This movie = GUI/watermark.swf (2 KB, a document class with no engine logic beyond
onEventInit's anchor; its UI type 0x8d is never created on the console) with:
  - the SWF frame of minimap_c (357 px wide; height FRAME_H) and the same anchor as the minimap (setAnchor topRight / screen / topRight), so the
    game places it like the minimap: stage x 308 = the ring's right side (window 2336 on the 2400x1080 phone, minimap scale 1.3);
  - the column, our own art (assets/touchui via tools/dos2_touchart.py, owner 16.11; laid out as the iPad's rightSideButtons_t: a round
    character button, square plates with the panel icons, the map), scaled by SCALE, right side on x RIGHT, from y TOP (just under the ring) down;
  - the column's tap code: press dims, the stage's release on the same button -> ExternalInterface dmPanel(id) (0..5, hle_dos2_touchui_vis.c),
    dmPadBtn(27) = the map ('-'), the fold handles; the state shown / folded / hidden by polling ExternalInterface dmColumnPoll(state) - the
    host answers through the invoke "addWatermarkText"(100 + state) (a name the engine registered for this class; a Number there is ours,
    a String goes to the original).
The host (hle_dos2_touchui.c) maps type 0x8d -> GUI/dmColumn_d.swf, creates it next to bottomBar_c and shows / hides it with bottomBar.

  python -I tools/dos2_column_touch.py build <Switch Game.pak | watermark.swf> <out.swf>
  python -I tools/dos2_column_touch.py check <Switch Game.pak | watermark.swf>
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, NS_PUBLIC
import dos2_swf_graft as gr
import dos2_touchart as ta

MOVIE = 'dmColumn_d.swf'
SRC = 'Public/Game/GUI/watermark.swf'
TYPE = 0x8d                      # watermark.swf's UI type (creator main+0x10b9520: a plain UIObject + 0x20 bytes)
FRAME_W, FRAME_H = 357, 1080     # pack 3s: the full height (the host puts the column 1.5 buttons under the ring: 7 buttons need ~470 px below it)
RIGHT, TOP = 308, 345            # minimap_c stage: the ring's right side (measured 15.11) / just under the ring (its bottom ~335)
SCALE, STEP = 0.97, 78           # owner 15.11 (3s): icons 10-15 % bigger; 74 px iPad buttons at 0.97 every 78 stage px (was 0.85 / 68)
BTN = 74
COLUMN = [('dmVis1_mc', 1, 'btn_character'), ('dmVis5_mc', 5, 'ico_charsheet'), ('dmVis0_mc', 0, 'ico_inventory'), ('dmVis2_mc', 2, 'ico_skills'), ('dmVis3_mc', 3, 'ico_craft'),
          ('dmVis4_mc', 4, 'ico_journal'), ('dmVisMap_mc', 14, 'icon_map')]   # (instance, code, art in assets/touchui)
FOLD_SCALE = 0.6 * SCALE
FOLD_PAD = 40                    # pack 3t: invisible hit margin around a fold arrow, its own px (x FOLD_SCALE ~ 20 window px)
FOLDS = [('dmVisFoldR_mc', True, (RIGHT - BTN * SCALE - 22, TOP + BTN * SCALE / 2.0)),   # fold: left of the helmet
         ('dmVisFoldL_mc', False, (RIGHT - 16, TOP + BTN * SCALE / 2.0))]                  # unfold: at the ring's right side, while folded
COLUMN_ALL = [n for n, _, _ in COLUMN]
PLACE_ALL = COLUMN_ALL + [n for n, _, _ in FOLDS]   # pack 3s: what dmColX / dmColY move (the host places the column by the real minimap)
COL_POLL = 60
INVOKE = 'addWatermarkText'      # the host's channel into this movie (hle_dos2_touchui_vis.c)


def _read(path, name, gui=True):
    if os.path.isdir(path): return (Path(path) / name).read_bytes()
    data = open(path, 'rb').read()
    if data[:3] in (b'CWS', b'FWS', b'ZWS'): return data
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == 'Public/Game/GUI/' + name]
    if not e: sys.exit('%s: no %s' % (path, name))
    return pak.read(e[0])


def source(path): return _read(path, 'watermark.swf')


def rect_bytes(w, h):
    """SWF RECT 0..w, 0..h (px) in twips"""
    vals = [0, w * 20, 0, h * 20]; nb = max(v.bit_length() for v in vals) + 1
    bits = format(nb, '05b') + ''.join(format(v, '0%db' % nb) for v in vals)
    bits += '0' * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def graft(swf):
    """our own art (assets/touchui via tools/dos2_touchart.py, owner 16.11), laid out as the iPad column"""
    rep = {'placed': [], 'frozen': 0, 'defs': 0}
    g = gr.Graft(swf); art = ta.Art(g)
    plate, pb = art.png('plate_square_copper')                   # 74 x 74 px
    x = RIGHT - BTN * SCALE
    for k, (name, code, icon) in enumerate(COLUMN):
        if icon == 'btn_character':                              # the round character button, 105 px drawn, shown BTN wide (as the iPad's)
            c, cb = art.png(icon); btn = g.sprite([(c, 1, (0, 0, BTN / (cb[2] - cb[0])), None)])
        else:
            ic, ib = art.png(icon)
            btn = g.sprite([(plate, 1, (0, 0), None), (ic, 2, (BTN / 2.0 - (ib[2] - ib[0]) / 2.0, BTN / 2.0 - (ib[3] - ib[1]) / 2.0), None)])
        y = TOP + STEP * k
        g.place(btn, x, y, name, SCALE); rep['placed'].append((name, code, x, y, x + BTN * SCALE, y + BTN * SCALE))
    for name, right, (cx, cy) in FOLDS:
        c, b = art.arrow(right); k = FOLD_SCALE
        px, py = cx - (b[0] + b[2]) / 2.0 * k, cy - (b[1] + b[3]) / 2.0 * k; g.place(c, px, py, name, k)
        rep['placed'].append((name, 16, px + b[0] * k, py + b[1] * k, px + b[2] * k, py + b[3] * k))
    rep['frozen'] += len(g.frozen); g.finish(); rep['defs'] += len(g.new_defs)
    swf.head = rect_bytes(FRAME_W, FRAME_H) + swf.head[-4:]   # the frame of the "fixed" layout (keeps frame rate / count)
    return rep


def wire(swf, rep):
    (tag, pre, abc), = swf.abcs(); S = abc.string
    docs = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']).endswith('.MainTimeline')]
    assert len(docs) == 1 and not abc.instances[docs[0]]['flags'] & 1, 'document class not found or sealed'
    ci = docs[0]; inst = abc.instances[ci]
    ti, tw = abc.trait(ci, 'onEventInit'), abc.trait(ci, INVOKE)
    assert ti is not None and ti.kind & 15 == 1 and not abc.methods[ti.b].params, 'onEventInit()'
    assert tw is not None and tw.kind & 15 == 1, INVOKE
    assert abc.trait(ci, 'dmVisHook') is None, 'already patched'
    ns = abc.mn[ti.name][1]; assert abc.ns[ns] == (NS_PUBLIC, S('')), 'onEventInit is not public'
    P = lambda name: abc.qname(ns, name)
    EI = abc.pub('ExternalInterface', 'flash.external')
    HEAD = [('getlocal0',), ('pushscope',)]
    ref = abc.methods[ti.b].body

    def add(name, nparams, lines, like=None):
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 64, 64, ref.init_scope, ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        if like is not None:
            abc.methods[m].ret = like.ret
            if like.flags & 0x08: abc.methods[m].flags |= 0x08; abc.methods[m].options = list(like.options)
        return m

    def child(name, loc): return [('getlocal0',), ('pushstring', S(name)), ('callproperty', P('getChildByName'), 1), ('coerce_a',), ('setlocal', loc)]
    def ei(*args): return [('getlex', EI)] + list(args) + [('callpropvoid', P('call'), len(args))]
    names = [n for n, _, _ in COLUMN] + [n for n, _, _ in FOLDS]
    codes = [c for _, c, _ in COLUMN] + [16, 16]

    # dmVisHook(): once - every button gets its action code and a press listener; the column state poll starts
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('pushnull',), ('coerce_a',), ('setlocal', 5), ('pushnull',), ('coerce_a',), ('setlocal', 6),
        ('getlocal0',), ('getproperty', P('dmVisOn')), ('iftrue', 'end'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmVisOn'))]
    r6 = lambda f: [('getlocal', 6), ('getproperty', P(f))]
    for k, (name, c) in enumerate(zip(names, codes)):
        code += child(name, 1) + [('getlocal', 1), ('iffalse', 'n%d' % k),
            ('getlocal', 1), ('pushbyte', c), ('setproperty', P('dmCode')),
            ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseChildren')),
            ('getlocal', 1), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmVisDown')), ('callpropvoid', P('addEventListener'), 2)]
        if c == 16:   # pack 3t (owner: "the fold arrow does not work"; run t3s: taps at its edge missed the thin line art): an invisible fill FOLD_PAD
            # (local px) around the arrow is its hit area (a 0-alpha fill counts for hitTestPoint / mouseDown)
            code += [('getlocal', 1), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', 5),
                ('getlocal', 1), ('getlocal', 1), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 6),
                ('getlocal', 5), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', P('beginFill'), 2),
                ('getlocal', 5)] + r6('x') + [('pushbyte', FOLD_PAD), ('subtract',)] + r6('y') + [('pushbyte', FOLD_PAD), ('subtract',)] +                 r6('width') + [('pushbyte', 2 * FOLD_PAD), ('add',)] + r6('height') + [('pushbyte', 2 * FOLD_PAD), ('add',), ('callpropvoid', P('drawRect'), 4),
                ('getlocal', 5), ('callpropvoid', P('endFill'), 0)]
        code += [('L', 'n%d' % k)]
    code += [('getlocal0',), ('pushbyte', -1), ('setproperty', P('dmColState')), ('getlocal0',), ('pushfalse',), ('setproperty', P('dmColFold')),
             ('getlocal0',), ('pushbyte', COL_POLL - 1), ('setproperty', P('dmColN')), ('getlocal0',), ('callpropvoid', P('dmColApply'), 0),
             ('getlocal0',), ('pushstring', S('enterFrame')), ('getlocal0',), ('getproperty', P('dmColTick')), ('callpropvoid', P('addEventListener'), 2),
             ('L', 'end'), ('returnvoid',)]
    m_hook = add('dmVisHook', 0, code)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
        ('getlocal1',), ('getproperty', P('currentTarget')), ('coerce_a',), ('setlocal', 2),
        ('getlocal0',), ('getlocal', 2), ('setproperty', P('dmVisB')),
        ('getlocal', 2), ('pushbyte', 6), ('pushbyte', 10), ('divide',), ('setproperty', P('alpha')),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal', 3), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmVisUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)]
    m_down = add('dmVisDown', 1, code)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3), ('pushbyte', 0), ('coerce_a',), ('setlocal', 4),
        ('getlocal0',), ('getproperty', P('dmVisB')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmVisB')),
        ('getlocal', 2), ('pushbyte', 1), ('setproperty', P('alpha')),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal', 2), ('getlocal', 3), ('getproperty', P('mouseX')), ('getlocal', 3), ('getproperty', P('mouseY')), ('pushfalse',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'end'),
        ('getlocal', 2), ('getproperty', P('dmCode')), ('coerce_a',), ('setlocal', 4),
        ('getlocal', 4), ('pushbyte', 10), ('ifnlt', 'c14'),
        ] + ei(('pushstring', S('dmPanel')), ('getlocal', 4)) + [('returnvoid',),
        ('L', 'c14'), ('getlocal', 4), ('pushbyte', 14), ('ifne', 'c16'),
        ] + ei(('pushstring', S('dmPadBtn')), ('pushbyte', 27)) + [('returnvoid',),
        ('L', 'c16'), ('getlocal', 4), ('pushbyte', 16), ('ifne', 'end'),
        ] + ei(('pushstring', S('PlaySound')), ('pushstring', S('UI_Generic_Click'))) + [
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmColFold')), ('not',), ('setproperty', P('dmColFold')),
        ('getlocal0',), ('callpropvoid', P('dmColApply'), 0),
        ('L', 'end'), ('returnvoid',)]
    m_up = add('dmVisUp', 1, code)
    # dmColApply(): buttons + fold arrow visible = shown and not folded; the unfold arrow = shown and folded
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('pushfalse',), ('setlocal', 2), ('pushfalse',), ('setlocal', 3),
        ('getlocal0',), ('getproperty', P('dmColState')), ('pushbyte', 2), ('equals',), ('not',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'nf'), ('getlocal0',), ('getproperty', P('dmColFold')), ('not',), ('setlocal', 3), ('L', 'nf')]
    for k, name in enumerate(COLUMN_ALL + ['dmVisFoldR_mc', 'dmVisFoldL_mc']):
        code += child(name, 1) + [('getlocal', 1), ('iffalse', 'a%d' % k), ('getlocal', 1)]
        if name == 'dmVisFoldL_mc': code += [('pushfalse',), ('getlocal', 2), ('iffalse', 'b%d' % k), ('pop',), ('getlocal', 3), ('not',), ('L', 'b%d' % k)]
        else: code += [('getlocal', 3)]
        code += [('setproperty', P('visible')), ('L', 'a%d' % k)]
    code += [('returnvoid',)]
    m_apply = add('dmColApply', 0, code)
    code = HEAD + [('getlocal0',), ('getlocal1',), ('convert_i',), ('setproperty', P('dmColState')),
        ('getlocal1',), ('pushbyte', 1), ('ifne', 's0'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmColFold')), ('jump', 'ap'),
        ('L', 's0'), ('getlocal1',), ('pushbyte', 0), ('ifne', 'ap'), ('getlocal0',), ('pushfalse',), ('setproperty', P('dmColFold')),
        ('L', 'ap'), ('getlocal0',), ('callpropvoid', P('dmColApply'), 0), ('returnvoid',)]
    m_col = add('dmColumn', 1, code)
    code = HEAD + [('getlocal0',), ('getlocal0',), ('getproperty', P('dmColN')), ('increment',), ('setproperty', P('dmColN')),
        ('getlocal0',), ('getproperty', P('dmColN')), ('pushbyte', COL_POLL), ('ifnge', 'end'),
        ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmColN')),
        ('getlex', EI), ('pushstring', S('dmColumnPoll')), ('getlocal0',), ('getproperty', P('dmColState')),
        ('getlocal0',), ('getproperty', P('dmColRx')), ('getlocal0',), ('getproperty', P('dmColTy')), ('callpropvoid', P('call'), 4),
        ('L', 'end'), ('returnvoid',)]
    m_tick = add('dmColTick', 1, code)
    # pack 3s (run t3r: this class is not scaled 1.3 like the minimap, so fixed stage numbers put the column 15 px right of the ring and over its
    # bottom): the host places it by the minimap's real rect. dmColX(r): the column's right side at stage x r; dmColY(t): its top at stage y t.
    # Each element moves from the x / y it had in the movie (kept as dmPx<k> / dmPy<k>). locals 1 v 2 d 3 mc
    def mover(prop, base, key, keep):
        c = HEAD + [('pushbyte', 0), ('convert_d',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
                    ('getlocal0',), ('getlocal1',), ('setproperty', P(keep)),
                    ('getlocal1',), ('pushshort', base), ('subtract',), ('convert_d',), ('setlocal', 2)]
        for k, nm in enumerate(PLACE_ALL):
            c += child(nm, 3) + [('getlocal', 3), ('iffalse', 'm%d' % k),
                ('getlocal0',), ('getproperty', P(key + str(k))), ('pushundefined',), ('ifstrictne', 'n%d' % k),
                ('getlocal0',), ('getlocal', 3), ('getproperty', P(prop)), ('setproperty', P(key + str(k))),
                ('L', 'n%d' % k), ('getlocal', 3), ('getlocal0',), ('getproperty', P(key + str(k))), ('getlocal', 2), ('add',), ('setproperty', P(prop)),
                ('L', 'm%d' % k)]
        return c + [('returnvoid',)]
    m_px = add('dmColX', 1, mover('x', RIGHT, 'dmPx', 'dmColRx'))
    m_py = add('dmColY', 1, mover('y', TOP, 'dmPy', 'dmColTy'))
    # onEventInit(): the minimap's anchor (topRight of the screen) for this movie's own id, then the hook. The original registered
    # "watermark" at the screen's centre; it is kept as dmColInitOrig and not called.
    code = HEAD + ei(('pushstring', S('registerAnchorId')), ('pushstring', S('dmColumn'))) + ei(
        ('pushstring', S('setAnchor')), ('pushstring', S('topRight')), ('pushstring', S('screen')), ('pushstring', S('topRight'))) + [
        ('getlocal0',), ('callpropvoid', P('dmVisHook'), 0), ('returnvoid',)]
    m_init = add('onEventInit', 0, code, like=abc.methods[ti.b])
    # addWatermarkText(a, ...): a Number = the host's column state (100 + state); anything else -> the original
    orig = abc.methods[tw.b]; n = len(orig.params)
    code = HEAD + [('getlocal1',), ('typeof',), ('pushstring', S('number')), ('ifne', 'orig'),
        ('getlocal1',), ('pushshort', 20000), ('ifnge', 'nx'),
        ('getlocal0',), ('getlocal1',), ('pushshort', 20000), ('subtract',), ('callpropvoid', P('dmColY'), 1), ('returnvoid',),
        ('L', 'nx'), ('getlocal1',), ('pushshort', 10000), ('ifnge', 'ns'),
        ('getlocal0',), ('getlocal1',), ('pushshort', 10000), ('subtract',), ('callpropvoid', P('dmColX'), 1), ('returnvoid',),
        ('L', 'ns'),
        ('getlocal0',), ('getlocal1',), ('pushbyte', 100), ('subtract',), ('callpropvoid', P('dmColumn'), 1), ('returnvoid',),
        ('L', 'orig'), ('getlocal0',)] + [('getlocal', k + 1) for k in range(n)] + [('callpropvoid', P('dmColWmOrig'), n), ('returnvoid',)]
    m_wm = add(INVOKE, n, code, like=orig)
    inst['traits'] += [Trait(P('dmVisHook'), 1, 0, m_hook), Trait(P('dmVisDown'), 1, 0, m_down), Trait(P('dmVisUp'), 1, 0, m_up),
                       Trait(P('dmColApply'), 1, 0, m_apply), Trait(P('dmColumn'), 1, 0, m_col), Trait(P('dmColTick'), 1, 0, m_tick),
                       Trait(P('dmColX'), 1, 0, m_px), Trait(P('dmColY'), 1, 0, m_py)]
    old_i = ti.b; ti.b = m_init; inst['traits'].append(Trait(P('dmColInitOrig'), 1, 0, old_i))
    old_w = tw.b; tw.b = m_wm; inst['traits'].append(Trait(P('dmColWmOrig'), 1, 0, old_w))
    rep['methods'] = [m_hook, m_down, m_up, m_apply, m_col, m_tick, m_px, m_py, m_init, m_wm]
    swf.tags[tag][1] = pre + abc.write()


def patch(data):
    swf = Swf(data); rep = graft(swf); wire(swf, rep)
    return swf.write(), rep


def check(src):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(src)
    b = Swf(out); (tb, pb, B), = b.abcs()
    print('%s -> %s: %d B -> %d B, +%d definition tags, +%d root instances, +%d methods, sha256 %s' % (
        SRC.split('/')[-1], MOVIE, len(src), len(out), rep['defs'], len(rep['placed']), len(rep['methods']), hashlib.sha256(out).hexdigest()[:16]))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies' % len(B.bodies))
    say(Swf(Swf(out).write()).body() == b.body(), 'the movie parses and round-trips')
    g = gr.check(src, out)
    say(g == ['header / tail differ'], 'graft: every original tag of watermark.swf kept, ids unique, uses after definitions (only the frame in the header changed)%s' % ('' if g == ['header / tail differ'] else ': ' + str(g)))
    say(b.head[:-4] == rect_bytes(FRAME_W, FRAME_H), 'frame %dx%d (the minimap\'s width)' % (FRAME_W, FRAME_H))
    names = {gr.place_parse(c, t)['name'] for c, t, _ in b.tags if c in (4, 26, 70)}
    say(all((n.encode() + b'\0') in names for n in COLUMN_ALL + [f for f, _, _ in FOLDS]), 'all %d buttons on the root timeline by name' % len(rep['placed']))
    inside = all(0 <= p[2] and p[4] <= FRAME_W and 0 <= p[3] and p[5] <= FRAME_H for p in rep['placed'])
    right = max(p[4] for p in rep['placed'] if p[0] in COLUMN_ALL)
    say(inside and abs(right - RIGHT) < 0.5, 'inside the frame, the column\'s right side at stage x %.1f (the ring: %d)' % (right, RIGHT))
    ob = B.owners(); ib = {v: k for k, v in ob.items()}; doc = [B.mname(c['name']) for c in B.instances if B.mname(c['name']).endswith('.MainTimeline')][0]
    d = '\n'.join(B.dis(B.methods[ib[doc + '::onEventInit']].body))
    say("'registerAnchorId'" in d and "'topRight'" in d and "'screen'" in d and 'dmVisHook' in d, 'onEventInit: anchor topRight / screen / topRight, then the hook')
    d = '\n'.join(B.dis(B.methods[ib[doc + '::dmVisUp']].body))
    say("'dmPanel'" in d and "'dmPadBtn'" in d and 'pushbyte         27' in d and 'hitTestPoint' in d, 'release on the pressed button -> dmPanel(id) / dmPadBtn(27) map / fold')
    d = '\n'.join(B.dis(B.methods[ib[doc + '::' + INVOKE]].body))
    say('dmColumn' in d and 'dmColWmOrig' in d, '%s(Number) -> dmColumn(n - 100), else the original' % INVOKE)
    return bad


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == 'build':
        out, rep = patch(source(a[1])); open(a[2], 'wb').write(out); print('%s: %d B' % (a[2], len(out))); return
    if len(a) == 2 and a[0] == 'check': sys.exit(1 if check(source(a[1])) else 0)
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
