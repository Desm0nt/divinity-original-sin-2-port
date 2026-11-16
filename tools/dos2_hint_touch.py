"""[DOS2] Button hints of a console movie as touch buttons: <name>_c.swf -> <name>_d.swf (docs/dos2/TOUCH_CONSOLE_PLAN.md, console touch, owner 15.11).

Look: untouched (only the DoABC tag changes, no display object is added). Two additions, appended methods only, no original body changed:

   Many console movies already carry mouse code of their own (the PC game in controller mode: msgBox_c's hints call EI ButtonPressed on mouse down,
   bottomBar_c's slots EI SlotPressed, contextMenu_c's entries ...; listed in the plan): those parts need no patch, only an open mouse filter and
   addition 2. OWN_HINTS = movies whose hints are such buttons already (no wrapper there).
1. HINTS. Every console movie draws its button hints ("(A) Select  (B) Back") with the shared class LS_Classes.controllerBtnHint; its
   setHintIcon(button, highlight) gets the LS_Classes.controllerHelper BTN_* code of the pad button (B 1, A 2, X 3, Y 4, LT 5, RT 6, d-pad 21..24,
   Back 27, Start 28, stick presses 29 / 30, LB 31, RB 32). The trait setHintIcon is re-pointed to a wrapper: it notes the code, adds mouseDown /
   mouseUp listeners once - on the hint's parent when that is a small element (icon + label, at most 6 children: the label is part of the
   button), else on the icon itself - and calls the original (kept as tuHintOrig). A press and a release on the hint within 800 ms call
   ExternalInterface "dmPadBtn"(code); the host (hle_dos2_touch.c) holds that npad button for a few polls: the same path as the on-screen pad, so
   the game does what the pad button does on that screen. The class is sealed, so the state lives on its icon_mc (a plain MovieClip: dynamic).
2. TAP IS NOT ACCEPT (not for a movie that already has the main menu's guard, tools/dos2_mainmenu_touch.py). On the phone a mouse click also
   reaches a console movie as its input event "IE UIAccept" (the pad's A). The document class's onEventUp / onEventDown are wrapped: an
   "IE UIAccept" within 400 ms of a mouse event of the stage (move / down / up, stamped by listeners added once from onEventInit or the first
   input event) is swallowed (returns true = handled); everything else goes to the original (kept as tuEvUpOrig / tuEvDownOrig). The pad's A is
   untouched (no mouse event near it); the A a hint tap asks for is pressed by the host 750 ms after the tap, past this guard and the main menu's.

  python -I tools/dos2_hint_touch.py build <x_c.swf | Switch Game.pak> <out dir | out.swf> [movie ...]   (with a package: the movies to make, default HINT_SET)
  python -I tools/dos2_hint_touch.py check <x_c.swf | Switch Game.pak> [movie ...]                       static verification, exit 1 on any failure
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, OPS, MN_MULTIL, NS_PUBLIC

HINT_CLS = 'LS_Classes.controllerBtnHint'
GUI = 'Public/Game/GUI/'
HINT_SET = ['msgBox', 'saveLoad', 'optionsSettings', 'mods', 'characterCreation', 'tutorialBox']   # hle_dos2_touchui.c k_hint (DM_DOS2_TOUCHUI_SET=hints)
OWN_HINTS = {'msgBox_c.swf'}   # movies whose hints already take the mouse themselves (msgBox_c popup_mc::addBtnHint: MOUSE_DOWN -> onButtonDown -> EI ButtonPressed): guard only
TAP_MS, GUARD_MS = 800, 400
DEBOUNCE_MS = 250   # pack 3f (owner 3e: "guard against a repeated click; A must register only when the tap hit the right place, then no other guard
                    # is needed"): the old guard dropped every IE UIAccept for 400 ms after ANY mouse event, so repeated taps on 'Accept' ate the A
                    # the hint asked for. A tap makes no UIAccept on the phone (hle_dos2_touchui_route.c drops those: 0 in runs t3d / t3e), so the
                    # guard now only drops a second accept within DEBOUNCE_MS of the last one (per handler: down and up of one press both pass).
EI_NAME = 'dmPadBtn'


def out_name(name): return name[:-6] + '_d.swf' if name.endswith('_c.swf') else name[:-4] + '_d.swf'


def patch(data, guard=None, hints=True):
    """movie bytes -> (patched bytes, report). guard: None = add the accept guard unless the movie has the main menu's (tuExecOrig); hints=False: no hint taps."""
    swf = Swf(data); (tag, pre, abc), = swf.abcs()
    rep = {'hint': [], 'guard': [], 'notes': []}
    S = abc.string
    getTimer, EI = abc.pub('getTimer', 'flash.utils'), abc.pub('ExternalInterface', 'flash.external')
    HEAD = [('getlocal0',), ('pushscope',)]
    def now(): return [('findpropstrict', getTimer), ('callproperty', getTimer, 0)]

    def add(ci, name, nparams, lines, ref, like=None):
        m = abc.add_method('tu_' + name, [0] * nparams, 0, asm(lines), 64, 64, ref.init_scope, ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        if like is not None:   # same parameter types / optional values as the method whose name it takes
            abc.methods[m].params = list(like.params); abc.methods[m].ret = like.ret
            if like.flags & 0x08: abc.methods[m].flags |= 0x08; abc.methods[m].options = list(like.options)
        return m

    # ---- 1. the hints
    try: hi = abc.class_index(HINT_CLS)
    except KeyError: hi = None
    if not hints: hi = None; rep['notes'].append('hints: the own mouse handlers of the movie (not wrapped)')
    if hi is not None:
        inst = abc.instances[hi]; t = abc.trait(hi, 'setHintIcon')
        assert t is not None and t.kind & 15 == 1, 'setHintIcon is not a method'
        assert abc.trait(hi, 'tuHintOrig') is None, 'already patched'
        orig = abc.methods[t.b]; assert len(orig.params) == 2, 'setHintIcon takes %d parameters' % len(orig.params)
        ns = abc.mn[t.name][1]; assert abc.ns[ns] == (NS_PUBLIC, S('')), 'setHintIcon is not public'
        P = lambda name: abc.qname(ns, name)
        ref = orig.body
        icon = [('getlocal0',), ('getproperty', P('icon_mc'))]
        code_set = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 3), ('pushnull',), ('coerce_a',), ('setlocal', 4)] + icon + [('coerce_a',), ('setlocal', 3),
            ('getlocal', 3), ('iffalse', 'orig'),
            ('getlocal', 3), ('getlocal1',), ('setproperty', P('tuBtn')),
            ('getlocal', 3), ('getproperty', P('tuOn')), ('iftrue', 'orig'),
            ('getlocal', 3), ('pushtrue',), ('setproperty', P('tuOn')),
            ('getlocal0',), ('getproperty', P('parent')), ('coerce_a',), ('setlocal', 4),
            ('getlocal', 4), ('iffalse', 'self'),
            ('getlocal', 4), ('getproperty', P('numChildren')), ('pushbyte', 6), ('ifgt', 'self'),
            ('jump', 'reg'),
            ('L', 'self'), ('getlocal0',), ('coerce_a',), ('setlocal', 4),
            ('L', 'reg'),
            ('getlocal', 4), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('tuDown')), ('callpropvoid', P('addEventListener'), 2),
            ('getlocal', 4), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('tuUp')), ('callpropvoid', P('addEventListener'), 2),
            ('L', 'orig'), ('getlocal0',), ('getlocal1',), ('getlocal2',), ('callpropvoid', P('tuHintOrig'), 2), ('returnvoid',)]
        code_down = HEAD + icon + now() + [('setproperty', P('tuT')), ('returnvoid',)]
        code_up = HEAD + now() + icon + [('getproperty', P('tuT')), ('subtract',), ('pushshort', TAP_MS), ('ifnlt', 'end')] + icon + [('pushbyte', 0), ('setproperty', P('tuT')),
            ('getlex', EI), ('pushstring', S(EI_NAME))] + icon + [('getproperty', P('tuBtn')), ('callpropvoid', P('call'), 2),
            ('L', 'end'), ('returnvoid',)]
        m_set = add(hi, 'setHintIcon', 2, code_set, ref, like=orig); m_down = add(hi, 'tuDown', 1, code_down, ref); m_up = add(hi, 'tuUp', 1, code_up, ref)
        old = t.b; t.b = m_set
        inst['traits'] += [Trait(P('tuHintOrig'), 1, 0, old), Trait(P('tuDown'), 1, 0, m_down), Trait(P('tuUp'), 1, 0, m_up)]
        rep['hint'] = [m_set, m_down, m_up]; rep['hint_orig'] = old
    elif hints: rep['notes'].append('no %s in this movie: no hint taps' % HINT_CLS)

    # ---- 2. a tap is not "accept"
    docs = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']).endswith('.MainTimeline')]
    has_mm = any(abc.trait(i, 'tuExecOrig') is not None for i in range(len(abc.instances)))
    if guard is None: guard = not has_mm
    if guard and len(docs) == 1 and not abc.instances[docs[0]]['flags'] & 1:
        ci = docs[0]; inst = abc.instances[ci]
        tu, td, ti = abc.trait(ci, 'onEventUp'), abc.trait(ci, 'onEventDown'), abc.trait(ci, 'onEventInit')
        if tu is None and td is None: rep['notes'].append('the document class has no onEventUp / onEventDown: no accept guard')
        else:
            first = tu or td; ns = abc.mn[first.name][1]; assert abc.ns[ns] == (NS_PUBLIC, S('')), 'onEventUp is not public'
            P = lambda name: abc.qname(ns, name)
            late = None
            for tr in (tu, td):
                if tr is None or late is not None: continue
                for _, op, ops, _ in abc.ins(abc.methods[tr.b].body.code):
                    if OPS[op][0] == 'getproperty' and abc.mn[ops[0]][0] == MN_MULTIL: late = ops[0]; break
            assert late is not None, 'no events[index] read in onEventUp / onEventDown'
            assert S('IE UIAccept') and b'IE UIAccept' in abc.strings, 'no IE UIAccept'
            ref = abc.methods[first.b].body
            code_stamp = HEAD + [('getlocal0',)] + now() + [('setproperty', P('tuLast')), ('returnvoid',)]
            code_hook = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1),
                ('getlocal0',), ('getproperty', P('tuHooked')), ('iftrue', 'end'),
                ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 1),
                ('getlocal', 1), ('iffalse', 'end'),
                ('getlocal0',), ('pushtrue',), ('setproperty', P('tuHooked'))]
            for ev in ('mouseMove', 'mouseDown', 'mouseUp'):
                code_hook += [('getlocal', 1), ('pushstring', S(ev)), ('getlocal0',), ('getproperty', P('tuStamp')), ('callpropvoid', P('addEventListener'), 2)]
            code_hook += [('L', 'end'), ('returnvoid',)]
            m_stamp = add(ci, 'tuStamp', 1, code_stamp, ref); m_hook = add(ci, 'tuHook', 0, code_hook, ref)
            inst['traits'] += [Trait(P('tuStamp'), 1, 0, m_stamp), Trait(P('tuHook'), 1, 0, m_hook)]
            rep['guard'] = [m_stamp, m_hook]; rep['moved'] = {}
            for tr, keep in ((tu, 'tuEvUpOrig'), (td, 'tuEvDownOrig')):
                if tr is None: continue
                orig = abc.methods[tr.b]; n = len(orig.params); assert n >= 1 and tr.kind & 15 == 1
                code = HEAD + [('getlocal0',), ('callpropvoid', P('tuHook'), 0),
                    ('getlocal0',), ('getproperty', P('events')), ('getlocal1',), ('getproperty', late), ('pushstring', S('IE UIAccept')), ('ifstrictne', 'go')] + now() + [
                    ('getlocal0',), ('getproperty', P(keep + 'T')), ('subtract',), ('pushshort', DEBOUNCE_MS), ('ifnlt', 'ok'),
                    ('pushtrue',), ('returnvalue',),                                   # a second accept within DEBOUNCE_MS: a double fire, dropped
                    ('L', 'ok'), ('getlocal0',)] + now() + [('setproperty', P(keep + 'T')),
                    ('L', 'go'), ('getlocal0',)] + [('getlocal', k + 1) for k in range(n)] + [('callproperty', P(keep), n), ('returnvalue',)]
                m = add(ci, abc.short(tr.name), n, code, ref, like=orig); abc.methods[m].ret = 0
                old = tr.b; tr.b = m; inst['traits'].append(Trait(P(keep), 1, 0, old)); rep['guard'].append(m); rep['moved'][keep] = old
            if ti is not None and ti.kind & 15 == 1 and not abc.methods[ti.b].params:
                code = HEAD + [('getlocal0',), ('callpropvoid', P('tuInitOrig2'), 0), ('getlocal0',), ('callpropvoid', P('tuHook'), 0), ('returnvoid',)]
                m = add(ci, 'onEventInit', 0, code, abc.methods[ti.b].body); old = ti.b; ti.b = m
                inst['traits'].append(Trait(P('tuInitOrig2'), 1, 0, old)); rep['guard'].append(m); rep['moved']['tuInitOrig2'] = old
    elif guard: rep['notes'].append('document class not found or sealed: no accept guard')
    else: rep['notes'].append('accept guard: the main menu\'s own (tuExecOrig)' if has_mm else 'accept guard not asked')
    swf.tags[tag][1] = pre + abc.write()
    return swf.write(), rep


def check(name, src):
    """static verification of one movie; returns the number of failures"""
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(src, hints=name not in OWN_HINTS)
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs()
    nnew = len(rep['hint']) + len(rep['guard'])
    print('%s -> %s: %d B -> %d B, +%d methods, sha256 %s%s' % (name, out_name(name), len(src), len(out), nnew, hashlib.sha256(out).hexdigest()[:16],
                                                               ''.join('; ' + n for n in rep['notes'])))
    say(rep['hint'] != [] or name in OWN_HINTS, 'the movie has %s: setHintIcon wrapped' % HINT_CLS if rep['hint'] else 'hints keep the own mouse handlers of the movie')
    say(b.ver == a.ver and b.head == a.head and len(a.tags) == len(b.tags) and all(x[0] == y[0] and (n == ta or x[1] == y[1]) for n, (x, y) in enumerate(zip(a.tags, b.tags))),
        'every tag but DoABC #%d is byte-identical (%d tags: the console look)' % (ta, len(a.tags)))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips')
    say(B.strings[:len(A.strings)] == A.strings and B.ns[:len(A.ns)] == A.ns and B.nssets == A.nssets and B.mn[:len(A.mn)] == A.mn and B.ints == A.ints and B.doubles == A.doubles,
        'constant pool only appended: +%d strings, +%d namespaces, +%d multinames' % (len(B.strings) - len(A.strings), len(B.ns) - len(A.ns), len(B.mn) - len(A.mn)))
    same = all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc) and x.method == y.method
               for x, y in zip(A.bodies, B.bodies))
    say(same and len(B.bodies) == len(A.bodies) + nnew and len(B.methods) == len(A.methods) + nnew, 'all %d original method bodies byte-identical; %d methods appended' % (len(A.bodies), nnew))
    oa, ob = A.owners(), B.owners()
    moved = sorted('%s -> %s' % (oa[m].split('::')[-1], ob[m].split('::')[-1]) for m in oa if ob.get(m) != oa[m])
    want = (['setHintIcon -> tuHintOrig'] if rep['hint'] else []) + sorted({'tuEvUpOrig': 'onEventUp -> tuEvUpOrig', 'tuEvDownOrig': 'onEventDown -> tuEvDownOrig',
                                                                           'tuInitOrig2': 'onEventInit -> tuInitOrig2'}[k] for k in rep.get('moved', {}))
    say(sorted(moved) == sorted(want), 'renamed methods: ' + ', '.join(moved))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies (stack, scope, locals, branch targets, constant indices)' % len(B.bodies))
    ib = {v: k for k, v in ob.items()}
    if rep['hint']:
        d = '\n'.join(B.dis(B.methods[ib[HINT_CLS + '::tuUp']].body))
        say("'%s'" % EI_NAME in d and 'pushshort        %d' % TAP_MS in d, 'hint release within %d ms of its press -> ExternalInterface %s(BTN code)' % (TAP_MS, EI_NAME))
    if rep['guard']:
        doc = [B.mname(c['name']) for c in B.instances if B.mname(c['name']).endswith('.MainTimeline')][0]
        for n in ('onEventUp', 'onEventDown'):
            if doc + '::' + n not in ib: continue
            d = '\n'.join(B.dis(B.methods[ib[doc + '::' + n]].body))
            say("'IE UIAccept'" in d and 'pushshort        %d' % DEBOUNCE_MS in d, '%s: a second "IE UIAccept" within %d ms of the last one is dropped (double fire), the rest goes to the original' % (n, DEBOUNCE_MS))
    return bad


def sources(path, names):
    data = open(path, 'rb').read()
    if data[:3] in (b'CWS', b'FWS', b'ZWS'): return {os.path.basename(path): data}
    import lspk
    pak = lspk.Pak(path); ent = {e[0]: e for e in pak.entries}
    out = {}
    for n in names or HINT_SET:
        fn = n if n.endswith('.swf') else n + '_c.swf'
        if GUI + fn not in ent: sys.exit('%s: no %s%s' % (path, GUI, fn))
        out[fn] = pak.read(ent[GUI + fn])
    return out


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    if a[0] == 'build':
        if len(a) < 3: sys.exit(__doc__)
        src = sources(a[1], a[3:]); dst = a[2]
        for name, data in src.items():
            out, rep = patch(data, hints=name not in OWN_HINTS)
            path = dst if dst.endswith('.swf') and len(src) == 1 else os.path.join(dst, out_name(name))
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True); open(path, 'wb').write(out)
            print('%s: %d B, sha256 %s%s' % (path, len(out), hashlib.sha256(out).hexdigest(), ''.join('; ' + n for n in rep['notes'])))
        return
    bad = 0
    for name, data in sources(a[1], a[2:]).items(): bad += check(name, data)
    print('FAILED: %d' % bad if bad else 'hint touch: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
