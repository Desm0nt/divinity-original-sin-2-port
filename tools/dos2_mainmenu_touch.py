"""[DOS2] mainMenu_d.swf = the Switch console main menu (mainMenu_c.swf, Switch Game.pak) with real touch (docs/dos2/TOUCH_UI_PLAN.md §9, variant D).

Look: untouched (no tag of the movie but DoABC changes; no display object is added, so the layout is the console one).
Behaviour added (all in class mainMenu_c_fla.mainMenu_mc_1, appended methods only; no original method body is changed):
  stage mouseMove / mouseDown / mouseUp listeners (added once, from init) -> the item under the pointer is found by geometry (the list's
  mouseX / mouseY against every visible selectable element: its row +- half the spacing, its label +- 120 px), because a console Menu_button is
  a bare text field (no background to hit);
    move, no button  -> that item becomes the selection through the stock listDisplay.selectMC (arrow, info text, EI buttonOver, sound);
    down             -> selection + pressed look (label 2 px down, alpha 0.6);
    move while down  -> leaving the item cancels the press;
    up on the item   -> its own buttonFunction() (EI buttonPressed(id)), or EI disabledButtonPressed(id) as executeSelected does.
  executeSelected (what `IE UIAccept` runs: the pad's A, and on the phone also a mouse click) is wrapped: for 700 ms after a mouse button
  event (and after a mouse move, once one button event has reached the movie) it does nothing, so a tap cannot ALSO confirm the focused
  item; when it does run it stamps the time and a release within 700 ms of that does not press again (covers the other order of the two
  events). No state can stick: the guard is time only. The pad path (onEventUp / onEventDown, moveCursor, setSelection, the original
  executeSelected body) is byte-identical.
How: the traits `init` and `executeSelected` are re-pointed to new wrapper methods and the original methods stay under the names
`tuInitOrig` / `tuExecOrig`; state lives in dynamic properties (the class is not sealed).

  python -I tools/dos2_mainmenu_touch.py build <mainMenu_c.swf | Switch Game.pak> <out mainMenu_d.swf>
  python -I tools/dos2_mainmenu_touch.py check <mainMenu_c.swf | Switch Game.pak> [<built mainMenu_d.swf>]     static verification, exit 1 on any failure
"""
import hashlib, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Abc, Trait, asm, verify_all, OPS, MN_MULTIL, NS_PUBLIC

CLS = 'mainMenu_c_fla.mainMenu_mc_1'
MOVIE = 'mainMenu_d.swf'          # same length as mainMenu_c.swf: the creator's path is rewritten in place (hle_dos2_touchui.c)
SRC = 'Public/Game/GUI/mainMenu_c.swf'
GUARD_MS = 700
DEBOUNCE_MS = 250   # pack 3f (owner: guard only against a repeated click): executeSelected and a tap's release share tuFired; the second of the
                    # two within DEBOUNCE_MS does nothing. The old rule (no accept for 700 ms after any mouse event) ate real A presses.
NEW = ['tuActive', 'tuHit', 'tuOver', 'tuUnpress', 'tuMove', 'tuPress', 'tuRelease']
PAD_PATH = ['mainMenu_c_fla.MainTimeline::onEventUp', 'mainMenu_c_fla.MainTimeline::onEventDown', 'mainMenu_c_fla.MainTimeline::executeSelected',
            'mainMenu_c_fla.MainTimeline::moveCursor', 'mainMenu_c_fla.MainTimeline::setSelection', CLS + '::moveCursor', CLS + '::setSelection',
            CLS + '::setSelectedIndex', CLS + '::setSelectedItem', CLS + '::onMoveSelection', 'Menu_button::selectElement', 'Menu_button::buttonFunction',
            'LS_Classes.listDisplay::selectMC', 'LS_Classes.listDisplay::select', 'LS_Classes.listDisplay::next', 'LS_Classes.listDisplay::previous']


def source(path):
    data = open(path, 'rb').read()
    if data[:3] in (b'CWS', b'FWS', b'ZWS'): return data
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == SRC]
    if not e: sys.exit('%s: no %s' % (path, SRC))
    return pak.read(e[0])


def patch(data):
    """mainMenu_c.swf bytes -> (mainMenu_d.swf bytes, report dict)"""
    swf = Swf(data); (tag, pre, abc), = swf.abcs()
    ci = abc.class_index(CLS); inst = abc.instances[ci]
    assert not inst['flags'] & 1, 'the class is sealed: dynamic properties would not work'
    t_init, t_exec = abc.trait(ci, 'init'), abc.trait(ci, 'executeSelected')
    assert t_init and t_exec and t_init.kind == 1 and t_exec.kind == 1
    ns = abc.mn[t_exec.name][1]; assert abc.ns[ns] == (NS_PUBLIC, abc.string('')), 'executeSelected is not public'
    for n in NEW + ['tuInitOrig', 'tuExecOrig']: assert abc.trait(ci, n) is None, 'already patched'
    P = lambda name: abc.qname(ns, name)                       # public name (an existing multiname is reused)
    one = lambda name, pkg: (lambda h: h[0] if len(h) == 1 else sys.exit('multiname %s.%s: %d' % (pkg, name, len(h))))(abc.find_qname(name, pkg))
    getTimer, MovieClip, EI = one('getTimer', 'flash.utils'), one('MovieClip', 'flash.display'), one('ExternalInterface', 'flash.external')
    own = abc.owners(); by = {v: k for k, v in own.items()}
    late = [ops[0] for _, op, ops, _ in abc.ins(abc.methods[by['LS_Classes.listDisplay::positionElements']].body.code)
            if OPS[op][0] == 'getproperty' and abc.mn[ops[0]][0] == MN_MULTIL][0]      # the compiler's own a[i] name
    ref = abc.methods[t_exec.b].body                             # scope depths of an instance method of this class
    S = abc.string

    def now(): return [('findpropstrict', getTimer), ('callproperty', getTimer, 0)]
    def stamp(): return [('getlocal0',)] + now() + [('setproperty', P('tuLast'))]
    def num(n, v=0): return [('pushbyte', v), ('convert_d',), ('setlocal', n)]
    def obj(n): return [('pushnull',), ('coerce_a',), ('setlocal', n)]
    HEAD = [('getlocal0',), ('pushscope',)]

    code = {}
    # tuActive(): Boolean - the menu is the thing on screen (not the splash screen, not hidden)
    code['tuActive'] = (0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'no'),
        ('getlocal0',), ('getproperty', P('menu_mc')), ('getproperty', P('visible')), ('iffalse', 'no'),
        ('getlocal0',), ('getproperty', P('root')), ('getlex', MovieClip), ('astypelate',), ('coerce_a',), ('setlocal', 1),
        ('getlocal', 1), ('iffalse', 'yes'),
        ('getlocal', 1), ('getproperty', P('splashScreen_mc')), ('iffalse', 'yes'),
        ('getlocal', 1), ('getproperty', P('splashScreen_mc')), ('getproperty', P('visible')), ('iftrue', 'no'),
        ('L', 'yes'), ('pushtrue',), ('returnvalue',),
        ('L', 'no'), ('pushfalse',), ('returnvalue',)])
    # tuHit(): the selectable element under the pointer, or null.  locals: 1 list 2 array 3 container 4 my 5 mx 6 i 7 n 8 el 9 top 10 bottom
    #                                                                       11 half spacing 12 label 13 centre x 14 half width
    code['tuHit'] = (0, HEAD + obj(1) + obj(2) + obj(3) + num(4) + num(5) + num(6) + num(7) + obj(8) + num(9) + num(10) + num(11) + obj(12) + num(13) + num(14) + [
        ('getlocal0',), ('getproperty', P('btnList')), ('coerce_a',), ('setlocal', 1),
        ('getlocal', 1), ('iffalse', 'none'),
        ('getlocal', 1), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 1), ('getproperty', P('containerContent_mc')), ('coerce_a',), ('setlocal', 3),
        ('getlocal', 2), ('iffalse', 'none'), ('getlocal', 3), ('iffalse', 'none'),
        ('getlocal', 3), ('getproperty', P('mouseY')), ('convert_d',), ('setlocal', 4),
        ('getlocal', 3), ('getproperty', P('mouseX')), ('convert_d',), ('setlocal', 5),
        ('getlocal', 1), ('getproperty', P('EL_SPACING')), ('pushbyte', 2), ('divide',), ('convert_d',), ('setlocal', 11),
        # an open drop-down keeps its own mouse handling (larCombo)
        ('getlocal', 1), ('callproperty', P('getCurrentMovieClip'), 0), ('coerce_a',), ('setlocal', 8),
        ('getlocal', 8), ('iffalse', 'scan'),
        ('getlocal', 8), ('getproperty', P('combo_mc')), ('iffalse', 'scan'),
        ('getlocal', 8), ('getproperty', P('combo_mc')), ('getproperty', P('m_isOpen')), ('iffalse', 'scan'),
        ('pushnull',), ('returnvalue',),
        ('L', 'scan'),
        ('getlocal', 2), ('getproperty', P('length')), ('convert_d',), ('setlocal', 7),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal', 2), ('getlocal', 6), ('getproperty', late), ('coerce_a',), ('setlocal', 8),
        ('getlocal', 8), ('iffalse', 'next'),
        ('getlocal', 8), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 8), ('getproperty', P('selectable')), ('iffalse', 'next'),
        ('getlocal', 8), ('getproperty', P('y')), ('getlocal', 11), ('subtract',), ('convert_d',), ('setlocal', 9),
        ('getlocal', 4), ('getlocal', 9), ('ifnge', 'next'),                                   # my >= top
        ('getlocal', 8), ('getproperty', P('y')), ('getlocal', 1), ('getlocal', 8), ('callproperty', P('getElementHeight'), 1), ('add',),
        ('getlocal', 11), ('add',), ('convert_d',), ('setlocal', 10),
        ('getlocal', 4), ('getlocal', 10), ('ifnlt', 'next'),                                  # my < bottom
        ('getlocal', 8), ('getproperty', P('label_txt')), ('coerce_a',), ('setlocal', 12),
        ('getlocal', 12), ('iffalse', 'hit'),                                                  # no label: the row alone decides
        ('getlocal', 8), ('getproperty', P('x')), ('getlocal', 12), ('getproperty', P('x')), ('add',),
        ('getlocal', 12), ('getproperty', P('width')), ('pushbyte', 2), ('divide',), ('add',), ('convert_d',), ('setlocal', 13),
        ('getlocal', 12), ('getproperty', P('width')), ('pushbyte', 2), ('divide',), ('pushbyte', 120), ('add',), ('convert_d',), ('setlocal', 14),
        ('getlocal', 5), ('getlocal', 13), ('getlocal', 14), ('subtract',), ('ifnge', 'next'),  # mx >= centre - half
        ('getlocal', 5), ('getlocal', 13), ('getlocal', 14), ('add',), ('ifnle', 'next'),       # mx <= centre + half
        ('L', 'hit'), ('getlocal', 8), ('returnvalue',),
        ('L', 'next'), ('getlocal', 6), ('increment',), ('convert_d',), ('setlocal', 6),
        ('L', 'cond'), ('getlocal', 6), ('getlocal', 7), ('iflt', 'loop'),
        ('L', 'none'), ('pushnull',), ('returnvalue',)])
    # tuOver(el): hover = the stock selection (arrow, info text, EI buttonOver + sound come from selectMC -> selectElement)
    code['tuOver'] = (1, HEAD + [
        ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('btnList')), ('callproperty', P('getCurrentMovieClip'), 0), ('getlocal1',), ('ifeq', 'end'),
        ('getlocal0',), ('getproperty', P('btnList')), ('getlocal1',), ('pushfalse',), ('callpropvoid', P('selectMC'), 2),
        ('L', 'end'), ('returnvoid',)])
    # tuUnpress(): the pressed look off, nothing is pressed
    code['tuUnpress'] = (0, HEAD + obj(1) + obj(2) + [
        ('getlocal0',), ('getproperty', P('tuItem')), ('coerce_a',), ('setlocal', 1),
        ('getlocal0',), ('pushnull',), ('setproperty', P('tuItem')),
        ('getlocal', 1), ('iffalse', 'end'),
        ('getlocal', 1), ('getproperty', P('label_txt')), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('getlocal0',), ('getproperty', P('tuY')), ('setproperty', P('y')),
        ('getlocal', 2), ('pushbyte', 1), ('setproperty', P('alpha')),
        ('L', 'end'), ('returnvoid',)])
    code['tuMove'] = (1, HEAD + [('getlocal0',), ('getproperty', P('tuSeen')), ('iffalse', 'unseen')] + stamp() + [   # moves count once a button event reached us
        ('L', 'unseen'),
        ('getlocal0',), ('getproperty', P('tuDown')), ('iffalse', 'hover'),
        ('getlocal0',), ('getproperty', P('tuItem')), ('iffalse', 'end'),
        ('getlocal0',), ('callproperty', P('tuActive'), 0), ('iffalse', 'cancel'),
        ('getlocal0',), ('callproperty', P('tuHit'), 0), ('getlocal0',), ('getproperty', P('tuItem')), ('ifeq', 'end'),
        ('L', 'cancel'), ('getlocal0',), ('callpropvoid', P('tuUnpress'), 0), ('jump', 'end'),
        ('L', 'hover'),
        ('getlocal0',), ('callproperty', P('tuActive'), 0), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal0',), ('callproperty', P('tuHit'), 0), ('callpropvoid', P('tuOver'), 1),
        ('L', 'end'), ('returnvoid',)])
    code['tuPress'] = (1, HEAD + obj(2) + obj(3) + stamp() + [
        ('getlocal0',), ('callpropvoid', P('tuUnpress'), 0),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('tuDown')),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('tuSeen')),
        ('getlocal0',), ('callproperty', P('tuActive'), 0), ('iffalse', 'end'),
        ('getlocal0',), ('callproperty', P('tuHit'), 0), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', 2), ('callpropvoid', P('tuOver'), 1),
        ('getlocal0',), ('getlocal', 2), ('setproperty', P('tuItem')),
        ('getlocal', 2), ('getproperty', P('label_txt')), ('coerce_a',), ('setlocal', 3),
        ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', 3), ('getproperty', P('y')), ('setproperty', P('tuY')),
        ('getlocal', 3), ('getlocal', 3), ('getproperty', P('y')), ('pushbyte', 2), ('add',), ('setproperty', P('y')),
        ('getlocal', 3), ('pushbyte', 6), ('pushbyte', 10), ('divide',), ('setproperty', P('alpha')),
        ('L', 'end'), ('returnvoid',)])
    code['tuRelease'] = (1, HEAD + obj(2) + stamp() + [
        ('getlocal0',), ('pushfalse',), ('setproperty', P('tuDown')),
        ('getlocal0',), ('getproperty', P('tuItem')), ('coerce_a',), ('setlocal', 2),
        ('getlocal0',), ('callpropvoid', P('tuUnpress'), 0),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('callproperty', P('tuActive'), 0), ('iffalse', 'end'),
        ('getlocal0',), ('callproperty', P('tuHit'), 0), ('getlocal', 2), ('ifne', 'end'),      # released somewhere else: no action
        ] + now() + [('getlocal0',), ('getproperty', P('tuFired')), ('subtract',), ('pushshort', DEBOUNCE_MS), ('iflt', 'end'),   # just ran (double fire)
        ('getlocal0',)] + now() + [('setproperty', P('tuFired')),
        ('getlocal', 2), ('getproperty', P('enable')), ('iffalse', 'disabled'),
        ('getlocal', 2), ('callpropvoid', P('buttonFunction'), 0), ('jump', 'end'),
        ('L', 'disabled'), ('getlex', EI), ('pushstring', S('disabledButtonPressed')), ('getlocal', 2), ('getproperty', P('id')), ('callpropvoid', P('call'), 2),
        ('L', 'end'), ('returnvoid',)])
    # the wrappers that take the names `executeSelected` and `init`
    code['executeSelected'] = (0, HEAD + now() + [
        ('getlocal0',), ('getproperty', P('tuFired')), ('subtract',), ('pushshort', DEBOUNCE_MS), ('iflt', 'end'),   # a double fire (tap + accept of one press)
        ('getlocal0',)] + now() + [('setproperty', P('tuFired')),
        ('getlocal0',), ('callpropvoid', P('tuExecOrig'), 0),
        ('L', 'end'), ('returnvoid',)])
    code['init'] = (0, HEAD + obj(1) + [
        ('getlocal0',), ('callpropvoid', P('tuInitOrig'), 0),
        ('getlocal0',), ('getproperty', P('tuHooked')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 1),
        ('getlocal', 1), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('tuHooked')),
        ('getlocal', 1), ('pushstring', S('mouseMove')), ('getlocal0',), ('getproperty', P('tuMove')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal', 1), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('tuPress')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal', 1), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('tuRelease')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)])

    made = {}
    for name, (nparams, lines) in code.items():
        m = abc.add_method('tu_' + name, [0] * nparams, 0, asm(lines), 64, 64, ref.init_scope, ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        made[name] = m
    old_init, old_exec = t_init.b, t_exec.b
    t_init.b = made['init']; t_exec.b = made['executeSelected']
    inst['traits'].append(Trait(P('tuInitOrig'), 1, 0, old_init)); inst['traits'].append(Trait(P('tuExecOrig'), 1, 0, old_exec))
    for n in NEW: inst['traits'].append(Trait(P(n), 1, 0, made[n]))
    swf.tags[tag][1] = pre + abc.write()
    out = swf.write()
    return out, {'methods': made, 'orig': {'init': old_init, 'executeSelected': old_exec}, 'abc': len(swf.tags[tag][1]) - len(pre)}


def check(src, built=None):
    """static verification; returns the number of failures"""
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(src)
    if built is not None: say(built == out, 'the built movie is exactly what the patcher makes from this source (%d B, sha256 %s)' % (len(built), hashlib.sha256(built).hexdigest()[:16]))
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs()
    say(b.sig == b'CWS' and b.ver == a.ver and b.head == a.head, 'SWF header: version %d, frame rectangle / rate unchanged' % b.ver)
    say(len(a.tags) == len(b.tags) and all(x[0] == y[0] and (n == ta or x[1] == y[1]) for n, (x, y) in enumerate(zip(a.tags, b.tags))),
        'every tag but DoABC #%d is byte-identical (%d tags: shapes, sprites, fonts, SymbolClass = the console look)' % (ta, len(a.tags)))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips (SWF and ABC rewritten byte-identical)')
    say(B.strings[:len(A.strings)] == A.strings and B.ns[:len(A.ns)] == A.ns and B.nssets == A.nssets and B.mn[:len(A.mn)] == A.mn and B.ints == A.ints
        and B.doubles == A.doubles, 'constant pool only appended: +%d strings, +%d namespaces, +%d multinames' % (len(B.strings) - len(A.strings), len(B.ns) - len(A.ns), len(B.mn) - len(A.mn)))
    same = all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc)
               and x.method == y.method for x, y in zip(A.bodies, B.bodies))
    say(same and len(B.bodies) == len(A.bodies) + 9 and len(B.methods) == len(A.methods) + 9, 'all %d original method bodies byte-identical; 9 methods appended' % len(A.bodies))
    oa, ob = A.owners(), B.owners(); ia, ib = {v: k for k, v in oa.items()}, {v: k for k, v in ob.items()}
    say(all(ia[n] == ib[n] for n in PAD_PATH), 'pad path still bound to its original methods: ' + ', '.join(n.split('::')[1] for n in PAD_PATH[:9]) + ' ...')
    moved = {n: (oa[m], ob[m]) for m, n in ((m, oa[m]) for m in oa) if ob.get(m) != oa[m]}
    say(sorted(v[1] for v in moved.values()) == [CLS + '::tuExecOrig', CLS + '::tuInitOrig'], 'renamed methods: ' + ', '.join('%s -> %s' % (k.split('::')[1], v[1].split('::')[1]) for k, v in sorted(moved.items())))
    ci = B.class_index(CLS)
    say(all(c1 == c2 for n, (c1, c2) in enumerate(zip(A.instances, B.instances)) if n != ci) and A.classes == B.classes and A.scripts == B.scripts
        and len(B.instances[ci]['traits']) == len(A.instances[ci]['traits']) + 9, 'only class %s changes: +9 method traits, no slot, flags %#x (not sealed)' % (CLS, B.instances[ci]['flags']))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies (stack depth at every join, scope, locals, branch targets on instruction starts, constant indices)' % len(B.bodies))
    for n in sorted(rep['methods']):
        bd = B.methods[rep['methods'][n]].body; st, sc, loc = B.verify(bd)
        say((st, loc) == (bd.max_stack, bd.local_count) and bd.init_scope == 10 and bd.max_scope == 11, '  %-16s %3d B  max_stack %d  locals %d  scope %d..%d (exact)' % (
            n, len(bd.code), bd.max_stack, bd.local_count, bd.init_scope, bd.max_scope))
    # the double-fire guard and the three listeners are really there
    dis = lambda n: '\n'.join(B.dis(B.methods[ib[CLS + '::' + n]].body))
    say('tuExecOrig' in dis('executeSelected') and 'pushshort        %d' % DEBOUNCE_MS in dis('executeSelected') and 'tuFired' in dis('executeSelected') and 'tuFired' in dis('tuRelease'),
        'no double fire: executeSelected and a tap release share tuFired, the second within %d ms does nothing (no guard after mouse events)' % DEBOUNCE_MS)
    say(all(("'%s'" % e) in dis('init') for e in ('mouseMove', 'mouseDown', 'mouseUp')) and 'tuInitOrig' in dis('init'), 'init = original + stage mouseMove / mouseDown / mouseUp listeners (once)')
    say('buttonFunction' in dis('tuRelease') and "'disabledButtonPressed'" in dis('tuRelease'), 'release on the pressed item -> its buttonFunction (EI buttonPressed) / EI disabledButtonPressed')
    stubs = [n for n in ('buttonOver', 'buttonOut', 'buttonPressed', 'buttonReleased') if len(B.methods[ib['Menu_button::' + n]].body.code) == 3]
    say(len(stubs) == 4, "Menu_button's own mouseOver / mouseOut / mouseDown listeners stay empty stubs (no second handler)")
    print('%s: %d B -> %s: %d B (ABC %d -> %d B), sha256 %s' % (SRC.split('/')[-1], len(src), MOVIE, len(out), len(a.tags[ta][1]), len(b.tags[tb][1]), hashlib.sha256(out).hexdigest()))
    return bad


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    src = source(a[1])
    if a[0] == 'build':
        out, rep = patch(src); open(a[2], 'wb').write(out)
        print('%s: %d B, sha256 %s' % (a[2], len(out), hashlib.sha256(out).hexdigest()))
        return
    bad = check(src, open(a[2], 'rb').read() if len(a) > 2 else None)
    print('FAILED: %d' % bad if bad else 'mainMenu_d: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
