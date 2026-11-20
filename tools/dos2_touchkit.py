"""[DOS2] dmTouch: one touch kit for every touchable UI movie (docs/dos2/TOUCH_UNIFIED.md: 3.2 the kit, 3.3 the maps, 5 steps M0 / M1 / M2,
6 checks). Static only: nothing made here was played or run on a phone when it was written.

The kit's code is ONE source (KIT_CODE below) in the Python subset that tools/dos2_cc_touch.py already turns into AVM2, extended here
(kc_compile: lists, subscript stores, `*rest` parameters, float constants, `%`). The same text runs in Python (Runner: a display tree built
from the SWF's placements, the host's events by hand) - the executor of section 6 - so hit tests, the report and the action table are
checked without a Flash player.

What the kit adds to a movie (append-only: no original body changes, only traits re-pointed with the originals kept under dmK* names):
  frame1           -> the original, then the kit's input event names appended to `events` ("IE dmTap" ... "IE dmSet"; the host gives them a
                      node: hle_dos2_touchui_ie.c dm_dos2_touchui_ie_extra)
  onEventInit      -> the original, then dmKSetup: the map (dmKMap), the report tick (enterFrame, every 10 frames: ExternalInterface
                      "dmKRects"("W,H|id,x,y,w,h,flags;...") of the visible elements, stage px; again at least every 3rd tick)
  onEventDown/Up   -> an "IE dm*" event is the kit's (dmKEv); any other goes to the original as it came (none in the movie = false)
  dmKEv            -> down: the element under the stage mouse (dmKFind: map order = priority, exact box first, then the nearest box grown to
                      the host's least hit size); up: the verb (tap / dtap / slong / long / hold / scroll) through dmKAct = the table the map
                      makes - only after the host's "IE dmSet" act = 1 (DM_DOS2_TOUCH_KIT=1); before that it only traces
                      (ExternalInterface "dmKitTrace"(event, down, value, mouse x, y, element, acted)).
Actions of a map (section 3.3): host:panel(n) / side(n) / pad(n) / key(n) / map / menu = ExternalInterface dmAct(code, n, 0) (codes of
dm/host/hle_dos2_touch_kit.h); ei:Name(args) = that ExternalInterface; call:path.method(args) (path '' = the document) = an AS3 method;
ie:IE Name = the movie's original onEventDown(i) + onEventUp(i) (its own pad path); hold:IE Name = down / up apart; toggle:prop =
this.prop = !this.prop; scroll:path = that scrollbar's scrollTo(scrolledY - px); native = nothing (the host's mouse does it: the element is
reported with 't'); none. Arguments: numbers, 'strings', @i (the child index of a 'c' element), @id (the hit object's id).
pad:IE Name (the engine's event by the mode's transport, K6) is not built yet: a map with it is refused.

  python -I tools/dos2_touchkit.py m0    <Switch Game.pak | sw_gui dir> <out dir>    the M0 probe movies: dmColumn_d.swf + dmSide_p.swf as the
        packs build them + the kit's names in `events` + onEventDown / onEventUp that only trace (no map, no action)
  python -I tools/dos2_touchkit.py kit   <Switch Game.pak | sw_gui dir> <out dir>    the M2 pilot: the same two movies with the full kit and their
        maps (tools/dos2_touchmap/dmSide_p.py, dmColumn_d.py); their old press code stays (it never gets a press while the kit is on)
  python -I tools/dos2_touchkit.py check <Switch Game.pak | sw_gui dir> [--main main.raw] [--write-golden]   every static check: the kit
        compiles and verifies, the M0 probes, the maps (paths, screen verbs, actions vs the movie's ABC and main.raw), the Runner's runs
        against tools/dos2_touchmap/golden/<movie>.txt
  python -I tools/dos2_touchkit.py run   <Switch Game.pak | sw_gui dir> <map name> <gesture>...   one Runner session, printed: gestures
        "tap:X,Y" "dtap:X,Y" "slong:X,Y" "long:X,Y" "hold:X,Y" "set:KEY,N" "tick" (stage px)
Hook: tools/dos2_touchui_pak.py --kit-m0 / --kit replace the packed 0x8d movies with these (the default packs are not touched).
"""
import ast, hashlib, os, re, struct, sys, tempfile, textwrap
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, OPS, MN_MULTIL, NS_PUBLIC
import dos2_swf_graft as gr

ROOT = Path(__file__).resolve().parent.parent
KIT_H = ROOT / 'dm' / 'host' / 'hle_dos2_touch_kit.h'
GOLDEN = Path(__file__).resolve().parent / 'dos2_touchmap' / 'golden'


# ---------------------------------------------------------------- the host's numbers (one source: hle_dos2_touch_kit.h)
def host_consts():
    s = KIT_H.read_text(encoding='utf-8')
    names = re.findall(r'"(IE dm\w+)"', re.search(r'dk_ev_name\[DK_EV_N\] = \{(.*?)\};', s).group(1))
    acts = re.findall(r'"(\w+|\?)"', re.search(r'dk_act_name\[DK_A_N\] = \{(.*?)\};', s).group(1))
    sets = {k.lower(): int(v) for k, v in re.findall(r'DK_SET_(\w+) = (\d+)', s)}
    flags = re.findall(r"DK_F_\w+ = \d+ /\* (\w): ", s)   # the report letters
    return names, {a: i for i, a in enumerate(acts) if a != '?'}, sets, flags
EV_NAMES, ACT_CODE, SET_KEY, _ = host_consts()
VERB_OF = {'IE dmTap': 'tap', 'IE dmDTap': 'dtap', 'IE dmSLong': 'slong', 'IE dmLong': 'long', 'IE dmHold': 'hold', 'IE dmScroll': 'scroll'}


# ---------------------------------------------------------------- the kit (Python subset; kc_compile -> AVM2, Runner -> Python)
KIT_CODE = r'''
def dmKF1():
    this.dmKF1Orig()
    this.dmKNames()

def dmKInit():
    this.dmKInitOrig()
    this.dmKSetup()

def dmKSetup():
    if this.dmKOn: return
    this.dmKOn = True
    this.dmKNames()
    this.dmKActOn = False
    this.dmKTr = TRACE
    this.dmKLeast = 0
    this.dmKMode = -1
    this.dmKN = 0
    this.dmKAge = 0
    this.dmKLast = ''
    this.dmKCur = -1
    this.dmKCi = -1
    this.dmKV = 0
    this.dmKMap()
    this.addEventListener('enterFrame', this.dmKTick)

def dmKPath(parts):
    # an instance by its path of names from the document ([] = the document)
    o = this
    i = 0
    n = parts.length
    while i < n:
        if not o: return None
        o = o.getChildByName(parts[i])
        i = i + 1
    return o

def dmKVis(o):
    # visible on the screen: it and every parent up to the document, alpha above 0
    while o:
        if not o.visible: return False
        if o.alpha <= 0: return False
        if o == this: return True
        o = o.parent
    return False

def dmKIn(o, sx, sy, pad, g):
    # the stage point inside o's stage box grown by pad, and to at least g around its centre; this.dmKD = the squared distance to the centre
    r = o.getBounds(this.stage)
    x0 = r.x - pad
    y0 = r.y - pad
    x1 = r.x + r.width + pad
    y1 = r.y + r.height + pad
    if x1 - x0 < g:
        c = (x0 + x1) / 2
        x0 = c - g / 2
        x1 = c + g / 2
    if y1 - y0 < g:
        c = (y0 + y1) / 2
        y0 = c - g / 2
        y1 = c + g / 2
    if sx < x0 or sx > x1: return False
    if sy < y0 or sy > y1: return False
    c = (x0 + x1) / 2 - sx
    d = (y0 + y1) / 2 - sy
    this.dmKD = c * c + d * d
    return True

def dmKHit(i, sx, sy, g):
    # the object of map entry i under the point (a 'c' entry: the child, this.dmKCi = its index), or None
    o = this.dmKPath(this.dmKP[i])
    if not o: return None
    if not this.dmKVis(o): return None
    if this.dmKK[i] == 'c':
        j = o.numChildren - 1
        while j >= 0:
            c = o.getChildAt(j)
            if c.visible and this.dmKIn(c, sx, sy, this.dmKPad[i], g):
                this.dmKCi = j
                return c
            j = j - 1
        return None
    if not this.dmKIn(o, sx, sy, this.dmKPad[i], g): return None
    this.dmKCi = -1
    return o

def dmKFind():
    # the map entry under the stage mouse: its own box first (map order = priority, top first), else the nearest box grown to the least size
    this.dmKHitEl = None
    st = this.stage
    if not st: return -1
    sx = st.mouseX
    sy = st.mouseY
    n = this.dmKP.length
    i = 0
    while i < n:
        if this.dmKK[i] != 'w':
            el = this.dmKHit(i, sx, sy, 0)
            if el:
                this.dmKHitEl = el
                return i
        i = i + 1
    best = -1
    bd = 0
    bc = -1
    i = 0
    while i < n:
        if this.dmKK[i] != 'w':
            el = this.dmKHit(i, sx, sy, this.dmKLeast)
            if el:
                if best < 0 or this.dmKD < bd:
                    best = i
                    bd = this.dmKD
                    bc = this.dmKCi
                    this.dmKHitEl = el
        i = i + 1
    this.dmKCi = bc
    return best

def dmKDown(i, *rest):
    if this.dmKIs(this.events[i]): return this.dmKEv(this.events[i], True, rest)
    if HAS_DOWN: return this.dmKDownOrig.apply(this, [i].concat(rest))
    return False

def dmKUp(i, *rest):
    if this.dmKIs(this.events[i]): return this.dmKEv(this.events[i], False, rest)
    if HAS_UP: return this.dmKUpOrig.apply(this, [i].concat(rest))
    return False

def dmKEv(nm, down, rest):
    v = 0
    if rest.length > 1: v = rest[1]
    if nm == 'IE dmSet':
        if not down: this.dmKSet(v)
        return True
    if down:
        if nm == 'IE dmHold' and this.dmKCur >= 0 and this.dmKHeld: return True
        this.dmKCur = this.dmKFind()
        this.dmKCurEl = this.dmKHitEl
        this.dmKCurCi = this.dmKCi
        if nm == 'IE dmHold':
            this.dmKHeld = True
            if this.dmKCur >= 0 and this.dmKActOn: this.dmKAct(this.dmKCur, 'hold', this.dmKCurEl, this.dmKCurCi, True)
        return True
    i = this.dmKCur
    verb = 'tap'
    if nm == 'IE dmDTap': verb = 'dtap'
    if nm == 'IE dmSLong': verb = 'slong'
    if nm == 'IE dmLong': verb = 'long'
    if nm == 'IE dmHold': verb = 'hold'
    if nm == 'IE dmScroll': verb = 'scroll'
    this.dmKV = v
    this.dmKHeld = False
    acted = 0
    if i >= 0 and this.dmKActOn: acted = this.dmKAct(i, verb, this.dmKCurEl, this.dmKCurCi, False)
    if this.dmKTr:
        hit = ''
        if i >= 0: hit = this.dmKId[i]
        st = this.stage
        mx = -1
        my = -1
        if st:
            mx = st.mouseX
            my = st.mouseY
        EI.call('dmKitTrace', nm, down, v, mx, my, hit, acted)
    return True

def dmKSet(v):
    # the host's settings: key * 1000000 + number (hle_dos2_touch_kit.h DK_SET_*)
    k = Math.floor(v / 1000000)
    n = v - k * 1000000
    if k == SET_ACT: this.dmKActOn = n > 0
    if k == SET_PAD: this.dmKLeast = n
    if k == SET_MODE: this.dmKMode = n

def dmKPad1(nm):
    # the movie's own pad path for an input event: its original onEventDown(i) + onEventUp(i)
    a = this.events
    n = a.length
    i = 0
    while i < n:
        if a[i] == nm:
            if HAS_DOWN: this.dmKDownOrig(i)
            if HAS_UP: this.dmKUpOrig(i)
            return 1
        i = i + 1
    return 0

def dmKHold1(nm, down):
    a = this.events
    n = a.length
    i = 0
    while i < n:
        if a[i] == nm:
            if down:
                if HAS_DOWN: this.dmKDownOrig(i)
            else:
                if HAS_UP: this.dmKUpOrig(i)
            return 1
        i = i + 1
    return 0

def dmKReport():
    # "W,H|id,x,y,w,h,flags;..." of the visible elements (stage px, the authored frame W x H)
    st = this.stage
    s = HEAD
    if not st: return s
    if not this.dmKVis(this): return s
    n = this.dmKP.length
    i = 0
    sep = ''
    while i < n:
        o = this.dmKPath(this.dmKP[i])
        if o and this.dmKVis(o):
            r = o.getBounds(st)
            if r.width > 0 and r.height > 0:
                p = this.dmKPad[i]
                s = s + sep + this.dmKId[i] + ',' + String(Math.round(r.x - p)) + ',' + String(Math.round(r.y - p)) + ',' + String(Math.round(r.width + 2 * p)) + ',' + String(Math.round(r.height + 2 * p)) + ',' + this.dmKF[i]
                sep = ';'
        i = i + 1
    return s

def dmKTick(e):
    this.dmKN = this.dmKN + 1
    if this.dmKN < 10: return
    this.dmKN = 0
    s = this.dmKReport()
    this.dmKAge = this.dmKAge + 1
    if s == this.dmKLast and this.dmKAge < 3: return
    this.dmKAge = 0
    this.dmKLast = s
    EI.call('dmKRects', s)
'''

# M0: the probe of TOUCH_UNIFIED section 5 - only the names and a trace (does an addressed "IE dmTap" reach onEventUp?)
M0_CODE = r'''
def dmM0F1():
    this.dmM0F1Orig()
    this.dmM0Names()

def dmM0Down(i, *rest):
    if this.dmM0Ev(i, True, rest): return True
    if HAS_DOWN: return this.dmM0DownOrig.apply(this, [i].concat(rest))
    return False

def dmM0Up(i, *rest):
    if this.dmM0Ev(i, False, rest): return True
    if HAS_UP: return this.dmM0UpOrig.apply(this, [i].concat(rest))
    return False

def dmM0Ev(i, down, rest):
    nm = this.events[i]
    if not this.dmM0Is(nm): return False
    v = 0
    if rest.length > 1: v = rest[1]
    st = this.stage
    mx = -1
    my = -1
    if st:
        mx = st.mouseX
        my = st.mouseY
    hit = ''
    names = BUTTONS
    k = 0
    n = names.length
    while k < n:
        c = this.getChildByName(names[k])
        if c and c.visible and c.hitTestPoint(mx, my, False): hit = names[k]
        k = k + 1
    EI.call('dmKitTrace', nm, down, v, mx, my, hit, -1)
    return True
'''


def names_code(prefix):
    """the kit's names pushed into `events` once, and the 'is it ours' test (both from hle_dos2_touch_kit.h)"""
    c = 'def %sNames():\n    if this.%sNamed: return\n    ev = this.events\n    if not ev: return\n    this.%sNamed = True\n' % (prefix, prefix, prefix)
    c += ''.join("    ev.push('%s')\n" % n for n in EV_NAMES)
    c += '\ndef %sIs(nm):\n' % prefix + ''.join("    if nm == '%s': return True\n" % n for n in EV_NAMES) + '    return False\n'
    return c


# ---------------------------------------------------------------- the compiler (tools/dos2_cc_touch.py cc_compile, extended)
def kc_compile(abc, ns, late, env, code):
    """code -> {name: (parameters, has *rest, assembler lines)}. env: name -> ('lex', mn) | ('call', mn) | ('const', value)"""
    P = lambda name: abc.qname(ns, name)
    S = abc.string
    out = {}
    def dbl(v):
        b = struct.pack('<d', float(v))
        if b in abc.doubles[1:]: return abc.doubles.index(b, 1)
        abc.doubles.append(b); return len(abc.doubles) - 1
    for fn in ast.parse(textwrap.dedent(code)).body:
        params = [a.arg for a in fn.args.args]
        rest = fn.args.vararg.arg if fn.args.vararg else None
        reg = {n: i + 1 for i, n in enumerate(params)}
        if rest: reg[rest] = len(params) + 1
        for node in ast.walk(fn):
            if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store) and node.id not in reg: reg[node.id] = len(reg) + 1
        L = [('getlocal0',), ('pushscope',)]
        for n, r in reg.items():
            if n not in params and n != rest: L += [('pushnull',), ('coerce_a',), ('setlocal', r)]
        nlab = [0]
        def lab():
            nlab[0] += 1; return '%s_%d' % (fn.name, nlab[0])

        def const(v):
            if v is None: L.append(('pushnull',))
            elif v is True: L.append(('pushtrue',))
            elif v is False: L.append(('pushfalse',))
            elif isinstance(v, str): L.append(('pushstring', S(v)))
            elif isinstance(v, int) and -128 <= v < 128: L.append(('pushbyte', v & 0xff))
            elif isinstance(v, int) and 0 <= v < 32768: L.append(('pushshort', v))
            elif isinstance(v, (int, float)): L.append(('pushdouble', dbl(v)))
            else: raise ValueError('constant %r' % (v,))

        def expr(e):
            if isinstance(e, ast.Constant): const(e.value)
            elif isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.USub) and isinstance(e.operand, ast.Constant): const(-e.operand.value)
            elif isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.USub): expr(e.operand); L.append(('negate',))
            elif isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.Not): expr(e.operand); L.append(('not',))
            elif isinstance(e, ast.Name):
                if e.id == 'this': L.append(('getlocal0',))
                elif e.id in reg: L.append(('getlocal', reg[e.id]))
                elif e.id in env and env[e.id][0] == 'lex': L.append(('getlex', env[e.id][1]))
                elif e.id in env and env[e.id][0] == 'const': const(env[e.id][1])
                elif e.id in env and env[e.id][0] == 'list': expr(ast.List([ast.Constant(x) for x in env[e.id][1]], ast.Load()))
                else: raise ValueError('name %s in %s' % (e.id, fn.name))
            elif isinstance(e, ast.Attribute): expr(e.value); L.append(('getproperty', P(e.attr)))
            elif isinstance(e, ast.Subscript): expr(e.value); expr(e.slice); L.append(('getproperty', late))
            elif isinstance(e, ast.List):
                for x in e.elts: expr(x)
                L.append(('newarray', len(e.elts)))
            elif isinstance(e, ast.BinOp):
                expr(e.left); expr(e.right)
                L.append(({ast.Add: 'add', ast.Sub: 'subtract', ast.Mult: 'multiply', ast.Div: 'divide', ast.Mod: 'modulo'}[type(e.op)],))
            elif isinstance(e, ast.Compare):
                assert len(e.ops) == 1, ast.dump(e)
                expr(e.left); expr(e.comparators[0]); op = type(e.ops[0])
                L.append(({ast.Lt: 'lessthan', ast.LtE: 'lessequals', ast.Gt: 'greaterthan', ast.GtE: 'greaterequals', ast.Eq: 'equals', ast.NotEq: 'equals',
                           ast.Is: 'strictequals', ast.IsNot: 'strictequals'}[op],))
                if op in (ast.NotEq, ast.IsNot): L.append(('not',))
            elif isinstance(e, ast.Call): call(e, False)
            else: raise ValueError('%s: %s' % (fn.name, ast.dump(e)))

        def call(e, void):
            f = e.func
            if isinstance(f, ast.Name):
                kind, mn = env[f.id]; assert kind == 'call', f.id
                L.append(('findpropstrict', mn))
                for a in e.args: expr(a)
                L.append(('callproperty', mn, len(e.args)))
                if void: L.append(('pop',))
            else:
                expr(f.value)
                for a in e.args: expr(a)
                L.append(('callpropvoid' if void else 'callproperty', P(f.attr), len(e.args)))

        def jfalse(e, target):
            if isinstance(e, ast.BoolOp) and isinstance(e.op, ast.And):
                for v in e.values: jfalse(v, target)
            elif isinstance(e, ast.BoolOp) and isinstance(e.op, ast.Or):
                yes = lab()
                for v in e.values[:-1]: jtrue(v, yes)
                jfalse(e.values[-1], target); L.append(('L', yes))
            elif isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.Not): jtrue(e.operand, target)
            else: expr(e); L.append(('iffalse', target))

        def jtrue(e, target):
            if isinstance(e, ast.BoolOp) and isinstance(e.op, ast.Or):
                for v in e.values: jtrue(v, target)
            elif isinstance(e, ast.BoolOp) and isinstance(e.op, ast.And):
                no = lab()
                for v in e.values[:-1]: jfalse(v, no)
                jtrue(e.values[-1], target); L.append(('L', no))
            elif isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.Not): jfalse(e.operand, target)
            else: expr(e); L.append(('iftrue', target))

        def store(t, value):
            if isinstance(t, ast.Name): expr(value); L.extend([('coerce_a',), ('setlocal', reg[t.id])])
            elif isinstance(t, ast.Attribute): expr(t.value); expr(value); L.append(('setproperty', P(t.attr)))
            elif isinstance(t, ast.Subscript): expr(t.value); expr(t.slice); expr(value); L.append(('setproperty', late))
            else: raise ValueError(ast.dump(t))

        def block(body):
            for s in body:
                if isinstance(s, ast.Assign):
                    t, = s.targets; store(t, s.value)
                elif isinstance(s, ast.AugAssign):
                    load = ast.Name(s.target.id, ast.Load()) if isinstance(s.target, ast.Name) else ast.Attribute(s.target.value, s.target.attr, ast.Load())
                    store(s.target, ast.BinOp(load, s.op, s.value))
                elif isinstance(s, ast.Expr):
                    if isinstance(s.value, ast.Constant): continue
                    assert isinstance(s.value, ast.Call), ast.dump(s); call(s.value, True)
                elif isinstance(s, ast.Return):
                    if s.value is None: L.append(('returnvoid',))
                    else: expr(s.value); L.append(('returnvalue',))
                elif isinstance(s, ast.If):
                    no = lab(); jfalse(s.test, no); block(s.body)
                    if s.orelse:
                        end = lab(); L.append(('jump', end)); L.append(('L', no)); block(s.orelse); L.append(('L', end))
                    else: L.append(('L', no))
                elif isinstance(s, ast.While):
                    top, cond = lab(), lab()
                    L.append(('jump', cond)); L.append(('L', top)); L.append(('label',)); block(s.body); L.append(('L', cond)); jtrue(s.test, top)
                elif isinstance(s, ast.Pass): pass
                else: raise ValueError('%s: %s' % (fn.name, ast.dump(s)))

        block(fn.body)
        L.append(('returnvoid',))
        out[fn.name] = (len(params), bool(rest), L)
    return out


# ---------------------------------------------------------------- maps
def load_map(name):
    import importlib
    return importlib.import_module('dos2_touchmap.' + name).MOVIE
def load_screens():
    import importlib
    return importlib.import_module('dos2_touchmap.screens').SCREEN
MAPS = ['dmSide_p', 'dmColumn_d']   # the M2 pilot (type 0x8d in both modes)
ACT_RE = re.compile(r'^(host|ei|call|ie|hold|toggle|scroll|pad):(.*)$')


def _args(text):
    """'3, 'x', @i' -> python values / ('@', name)"""
    out = []
    for a in [x.strip() for x in text.split(',') if x.strip()]:
        if a.startswith('@'): out.append(('@', a[1:]))
        elif a[0] in '\'"': out.append(a[1:-1])
        else: out.append(float(a) if '.' in a else int(a))
    return out


def parse_action(a):
    """an action string -> (kind, payload); raises ValueError for a malformed one"""
    if a in ('native', 'none'): return (a, None)
    m = ACT_RE.match(a)
    if not m: raise ValueError('action %r' % a)
    k, rest = m.groups()
    if k == 'host':
        mm = re.match(r'^(\w+)(?:\((-?\d+)\))?$', rest)
        if not mm or mm.group(1) not in ACT_CODE: raise ValueError('host verb %r (known: %s)' % (rest, ' '.join(ACT_CODE)))
        return ('host', (ACT_CODE[mm.group(1)], int(mm.group(2) or 0)))
    if k in ('ei', 'call'):
        mm = re.match(r'^([\w.]*?)\.?(\w+)\((.*)\)$', rest) if k == 'call' else re.match(r'^()(\w+)\((.*)\)$', rest)
        if not mm: raise ValueError('%s action %r' % (k, rest))
        path = [p for p in mm.group(1).split('.') if p] if k == 'call' else []
        return (k, (path, mm.group(2), _args(mm.group(3))))
    if k in ('ie', 'hold'):
        if not rest.startswith('IE '): raise ValueError('%s needs an "IE Name": %r' % (k, rest))
        return (k, rest)
    if k == 'toggle':
        if not re.match(r'^\w+$', rest): raise ValueError('toggle:%r' % rest)
        return (k, rest)
    if k == 'scroll': return (k, [p for p in rest.split('.') if p])
    if k == 'pad': raise ValueError('pad:%s - the engine-event transport of K6 is not built yet (M1): use ie: / call: / host:' % rest)
    raise ValueError(a)


def plan(mv, screens):
    """the map -> [(id, path list, kind, flags, pad, {gesture: [actions]})] + problems (the screen's every meaning covered)"""
    bad = []; out = []
    sc = screens.get(mv['screen'])
    if sc is None: return [], ['screen %r is not in screens.py' % mv['screen']]
    for (eid, path, kind, role, pad, acts) in mv['elements']:
        if kind not in ('e', 'w', 'c'): bad.append('%s: kind %r' % (eid, kind)); continue
        if role not in sc: bad.append('%s: role %r is not a role of screen %r' % (eid, role, mv['screen'])); continue
        g2a = {}
        for gesture, meaning in sc[role].items():
            if meaning not in acts: bad.append('%s: the screen gives %s the meaning %r, the map has no action for it' % (eid, gesture, meaning)); continue
            al = acts[meaning] if isinstance(acts[meaning], list) else [acts[meaning]]
            try: g2a[gesture] = [parse_action(a) for a in al]
            except ValueError as e: bad.append('%s: %s' % (eid, e))
        extra = set(acts) - set(sc[role].values())
        if extra: bad.append('%s: actions for meanings the screen does not have: %s' % (eid, ' '.join(sorted(extra))))
        fl = 'w' if kind == 'w' else 'e'
        if any(a[0] == 'native' for a in g2a.get('tap', [])): fl += 't'
        if 'hold' in g2a: fl += 'h'
        if any(a[0] == 'scroll' for v in g2a.values() for a in v): fl += 's'
        if mv.get('modal'): fl += 'm'
        out.append((eid, [p for p in path.split('.') if p], kind, fl, int(pad), g2a))
    return out, bad


def act_code(entries):
    """dmKMap() and dmKAct(i, verb, el, ci, down) in the kit's subset, from a planned map"""
    def lit(x): return repr(x)
    c = 'def dmKMap():\n'
    c += '    this.dmKP = [%s]\n' % ', '.join('[%s]' % ', '.join(lit(p) for p in e[1]) for e in entries)
    c += '    this.dmKK = [%s]\n' % ', '.join(lit(e[2]) for e in entries)
    c += '    this.dmKF = [%s]\n' % ', '.join(lit(e[3]) for e in entries)
    c += '    this.dmKPad = [%s]\n' % ', '.join(str(e[4]) for e in entries)
    c += '    this.dmKId = [%s]\n' % ', '.join(lit(e[0]) for e in entries)
    c += '\ndef dmKAct(i, verb, el, ci, down):\n'
    def arg(a):
        if isinstance(a, tuple) and a[0] == '@': return {'i': 'ci', 'id': 'el.id'}.get(a[1], 'None')
        return lit(a)
    for k, e in enumerate(entries):
        if not e[5]: continue
        c += '    if i == %d:\n' % k
        for g, acts in e[5].items():
            c += "        if verb == '%s':\n" % g
            ret = 1; body = []
            for kind, p in acts:
                if kind == 'host': body.append("EI.call('dmAct', %d, %d, 0)" % p)
                elif kind == 'ei': body.append('EI.call(%s)' % ', '.join([lit(p[1])] + [arg(a) for a in p[2]]))
                elif kind == 'call':
                    if p[0]: body += ['o = this.dmKPath([%s])' % ', '.join(lit(x) for x in p[0]), 'if o: o.%s(%s)' % (p[1], ', '.join(arg(a) for a in p[2]))]
                    else: body.append('this.%s(%s)' % (p[1], ', '.join(arg(a) for a in p[2])))
                elif kind == 'ie': body.append('this.dmKPad1(%s)' % lit(p))
                elif kind == 'hold': body.append('this.dmKHold1(%s, down)' % lit(p))
                elif kind == 'toggle': body.append('this.%s = not this.%s' % (p, p))
                elif kind == 'scroll': body += ['sb = this.dmKPath([%s])' % ', '.join(lit(x) for x in p), 'if sb: sb.scrollTo(sb.scrolledY - this.dmKV, False, False)']
                else: ret = 0   # native / none: the kit does nothing itself
            c += ''.join('            %s\n' % b for b in body) + '            return %d\n' % ret
    c += '    return 0\n'
    return c


# ---------------------------------------------------------------- ABC side
class Doc:
    """the document class of a movie (methods appended, traits re-pointed with the originals kept), as tools/dos2_pchud_touch.py Doc"""
    def __init__(s, swf, guard):
        s.swf = swf; (s.tag, s.pre, s.abc), = swf.abcs(); abc = s.abc
        docs = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']).endswith('.MainTimeline')]
        assert len(docs) == 1 and not abc.instances[docs[0]]['flags'] & 1, 'document class not found or sealed (the kit keeps dynamic properties)'
        s.ci = docs[0]; s.inst = abc.instances[s.ci]
        ti = abc.trait(s.ci, 'onEventInit'); assert ti is not None and ti.kind & 15 == 1, 'onEventInit()'
        s.ns = abc.mn[ti.name][1]; assert abc.ns[s.ns] == (NS_PUBLIC, abc.string('')), 'onEventInit is not public'
        s.ref = abc.methods[ti.b].body
        assert abc.trait(s.ci, guard) is None, 'already patched (%s)' % guard
        s.EI = abc.pub('ExternalInterface', 'flash.external'); s.made = {}
        nsset = [s.ns]; abc.nssets.append(nsset); s.late = len(abc.mn); abc.mn.append((MN_MULTIL, len(abc.nssets) - 1))   # events[i]
    def P(s, name): return s.abc.qname(s.ns, name)
    def compile(s, code, env):
        abc = s.abc
        full = {'EI': ('lex', s.EI), 'Math': ('lex', abc.pub('Math')), 'String': ('call', abc.pub('String'))}; full.update(env)
        for name, (np, rest, lines) in kc_compile(abc, s.ns, s.late, full, code).items():
            m = abc.add_method('dk_' + name, [0] * np, 0, asm(lines), 64, 64, s.ref.init_scope, s.ref.init_scope + 1)
            if rest: abc.methods[m].flags |= 0x04   # NEED_REST: any number of arguments after the first (the engine passes index, player, value)
            b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
            s.made[name] = m
        return s.made
    def has(s, name):
        t = s.abc.trait(s.ci, name); return t is not None and t.kind & 15 == 1
    def repoint(s, trait, method, keep):
        """trait (re-pointed to our method; the original kept as `keep`) or a new public trait when the movie has none"""
        t = s.abc.trait(s.ci, trait)
        if t is not None:
            assert t.kind & 15 == 1, trait
            old = t.b; t.b = s.made[method]; s.inst['traits'].append(Trait(s.P(keep), 1, 0, old)); return True
        s.inst['traits'].append(Trait(s.P(trait), 1, 0, s.made[method])); return False
    def add(s, *names):
        for n in names: s.inst['traits'].append(Trait(s.P(n), 1, 0, s.made[n]))
    def done(s): s.swf.tags[s.tag][1] = s.pre + s.abc.write()


def frame_size(swf):
    r = gr.Bits(swf.head, 0); x0, x1, y0, y1 = r.rect()
    return int(round((x1 - x0) / 20.0)), int(round((y1 - y0) / 20.0))


def orig_events(data):
    """the `events` the movie's frame1 makes (pushstring ... newarray / Array(...) -> initproperty events)"""
    (tag, pre, abc), = Swf(data).abcs()
    ci = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']).endswith('.MainTimeline')][0]
    t = abc.trait(ci, 'dmKF1Orig') or abc.trait(ci, 'dmM0F1Orig') or abc.trait(ci, 'frame1')   # a patched movie keeps its original frame1 under these
    if t is None: return []
    strs = []; got = []
    for _, op, ops, _ in abc.ins(abc.methods[t.b].body.code):
        nm = OPS[op][0]
        if nm == 'pushstring': strs.append(abc.S(ops[0]))
        elif nm == 'newarray': got = strs[len(strs) - ops[0]:] if ops[0] else []; strs = strs[:len(strs) - ops[0]] if ops[0] else strs
        elif nm == 'constructprop' and abc.short(ops[0]) == 'Array': got = strs[len(strs) - ops[1]:] if ops[1] else []
        elif nm == 'initproperty' and abc.short(ops[0]) == 'events': return list(got)
    return []


def m0_patch(data, buttons):
    """the M0 probe: the kit's names in `events`, onEventDown / onEventUp that trace a kit event (and pass any other to the original)"""
    swf = Swf(data); d = Doc(swf, 'dmM0F1')
    assert d.has('frame1'), 'frame1 (where the movie fills `events`)'
    env = {'HAS_DOWN': ('const', int(d.has('onEventDown'))), 'HAS_UP': ('const', int(d.has('onEventUp'))), 'BUTTONS': ('list', list(buttons))}
    d.compile(M0_CODE + '\n' + names_code('dmM0'), env)
    d.repoint('frame1', 'dmM0F1', 'dmM0F1Orig')
    d.repoint('onEventDown', 'dmM0Down', 'dmM0DownOrig'); d.repoint('onEventUp', 'dmM0Up', 'dmM0UpOrig')
    d.add('dmM0Ev', 'dmM0Names', 'dmM0Is')
    d.done()
    return swf.write(), {'methods': dict(d.made)}


def kit_source(entries, data, trace=1):
    """the kit's whole source for one movie + its compile constants"""
    swf = Swf(data); fw, fh = frame_size(swf)
    return KIT_CODE + '\n' + names_code('dmK') + '\n' + act_code(entries), {'HEAD': ('const', '%d,%d|' % (fw, fh)), 'TRACE': ('const', int(trace)),
        'SET_ACT': ('const', SET_KEY['act']), 'SET_PAD': ('const', SET_KEY['pad']), 'SET_MODE': ('const', SET_KEY['mode'])}


def kit_patch(data, mv, screens=None, trace=1):
    """the kit + this movie's map -> (bytes, report)"""
    entries, bad = plan(mv, screens or load_screens())
    if bad: raise ValueError('%s: map refused: %s' % (mv['movie'], '; '.join(bad)))
    code, env = kit_source(entries, data, trace)
    swf = Swf(data); d = Doc(swf, 'dmKF1')
    assert d.has('frame1'), 'frame1 (where the movie fills `events`)'
    env.update({'HAS_DOWN': ('const', int(d.has('onEventDown'))), 'HAS_UP': ('const', int(d.has('onEventUp')))})
    d.compile(code, env)
    d.repoint('frame1', 'dmKF1', 'dmKF1Orig'); d.repoint('onEventInit', 'dmKInit', 'dmKInitOrig')
    d.repoint('onEventDown', 'dmKDown', 'dmKDownOrig'); d.repoint('onEventUp', 'dmKUp', 'dmKUpOrig')
    d.add(*[n for n in d.made if n not in ('dmKF1', 'dmKInit', 'dmKDown', 'dmKUp')])
    d.done()
    return swf.write(), {'methods': dict(d.made), 'entries': entries, 'code': code, 'env': env}


# ---------------------------------------------------------------- the 0x8d movies as the packs build them
def sources(path):
    """watermark.swf of the Switch package / sw_gui directory"""
    if os.path.isdir(path): return (Path(path) / 'watermark.swf').read_bytes()
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == 'Public/Game/GUI/watermark.swf']
    if not e: sys.exit('%s: no watermark.swf' % path)
    return pak.read(e[0])


def base_movies(wm):
    """{'dmColumn_d.swf': bytes, 'dmSide_p.swf': bytes} exactly as tools/dos2_touchui_pak.py packs them, and the button names of each"""
    import dos2_column_touch as ct, dos2_pchud_touch as pt, dos2_hudrect_touch as hr
    col = hr.patch(ct.MOVIE, ct.patch(wm)[0])[0]   # the pack's chain: the column, then its HUD rects (tools/dos2_hudrect_touch.py POST)
    side = pt.side(wm)[0]
    return {'dmColumn_d.swf': col, 'dmSide_p.swf': side}, BUTTONS


def _buttons():
    import dos2_column_touch as ct, dos2_pchud_touch as pt
    return {'dmColumn_d.swf': list(ct.PLACE_ALL), 'dmSide_p.swf': [s[0] for s in pt.SIDE]}
BUTTONS = _buttons()


def pack(made, what):
    """tools/dos2_touchui_pak.py --kit-m0 / --kit: the packed type 0x8d movies (whatever passes made them) -> the M0 probe / the kit with its
    map; returns the names replaced"""
    done = []
    for name in sorted(made):
        if name not in BUTTONS: continue
        if what == 'm0': made[name] = m0_patch(made[name], BUTTONS[name])[0]
        else: made[name] = kit_patch(made[name], load_map(name[:-4]))[0]
        done.append(name)
    return done


def m0_movies(wm):
    base, btn = base_movies(wm)
    return {n: m0_patch(d, btn[n])[0] for n, d in base.items()}


def kit_movies(wm, trace=1):
    base, _ = base_movies(wm)
    out = {}
    for name in MAPS:
        mv = load_map(name); out[mv['movie']] = kit_patch(base[mv['movie']], mv, trace=trace)[0]
    return out


# ---------------------------------------------------------------- the Runner (section 6: the kit's source run in Python on the SWF's display tree)
class JSArray(list):
    @property
    def length(s): return len(s)
    def __bool__(s): return True   # an AS3 Array is an object: true even when empty
    def push(s, *v): s.extend(v); return len(s)
    def concat(s, *o):
        r = JSArray(s)
        for x in o: r.extend(x if isinstance(x, list) else [x])
        return r
    def __getitem__(s, i):
        i = int(i)
        return list.__getitem__(s, i) if 0 <= i < len(s) else None
    def __setitem__(s, i, v):
        i = int(i)
        while len(s) <= i: s.append(None)
        list.__setitem__(s, i, v)


def js_string(v):
    if isinstance(v, bool): return 'true' if v else 'false'
    if isinstance(v, float) and v == int(v): return str(int(v))
    if isinstance(v, float): return ('%.2f' % v).rstrip('0').rstrip('.')   # the Runner's print only (the kit's report rounds itself)
    return 'null' if v is None else str(v)


class Rect:
    def __init__(s, x, y, w, h): s.x, s.y, s.width, s.height = x, y, w, h


class Calls:
    """a method nobody defines: recorded (the actions the kit makes on the movie's own objects)"""
    def __init__(s, log, who, name): s.log, s.who, s.name = log, who, name
    def __call__(s, *a): s.log.append('call %s.%s(%s)' % (s.who, s.name, ', '.join(js_string(x) if not isinstance(x, str) else repr(x) for x in a))); return None
    def apply(s, this, args): return s(*args)


class Node:
    def __init__(s, log, name, path, x, y, sx, sy, local):
        d = s.__dict__
        d.update(_log=log, name=name, _path=path, x=x, y=y, scaleX=sx, scaleY=sy, _local=local, visible=True, alpha=1.0, parent=None, _kids=[])
    def __getattr__(s, n):
        if n.startswith('_'): raise AttributeError(n)
        return Calls(s._log, s._path or 'root', n)
    @property
    def numChildren(s): return len(s._kids)
    def getChildAt(s, j): return s._kids[int(j)] if 0 <= int(j) < len(s._kids) else None
    def getChildByName(s, n):
        for c in s._kids:
            if c.name == n: return c
        return None
    def _mat(s):
        if s.parent is None or isinstance(s.parent, Stage): return (s.scaleX, s.scaleY, s.x, s.y) if s.parent is not None else (1.0, 1.0, 0.0, 0.0)
        a, d, tx, ty = s.parent._mat()
        return (a * s.scaleX, d * s.scaleY, tx + a * s.x, ty + d * s.y)
    def getBounds(s, target):
        if not s._local: return Rect(0, 0, 0, 0)
        a, d, tx, ty = s._mat()
        x0, y0, x1, y1 = s._local
        xs = sorted([tx + a * x0, tx + a * x1]); ys = sorted([ty + d * y0, ty + d * y1])
        return Rect(xs[0], ys[0], xs[1] - xs[0], ys[1] - ys[0])
    def hitTestPoint(s, x, y, shape):
        r = s.getBounds(None); return r.x <= x <= r.x + r.width and r.y <= y <= r.y + r.height


class Stage:
    def __init__(s, w, h): s.mouseX = s.mouseY = 0.0; s.stageWidth = w; s.stageHeight = h


class Root(Node):
    """the document: dynamic properties (unset = None, as AS3's undefined), the kit's functions, the originals and the movie's own methods
    recorded (the Runner does not run AVM2)"""
    def __init__(s, log, fns, origs, methods):
        Node.__init__(s, log, 'root', '', 0, 0, 1, 1, None)
        s.__dict__.update(_fns=fns, _origs=origs, _meth=set(methods), _listen={})
    def __getattr__(s, n):
        if n.startswith('_'): raise AttributeError(n)
        if n in s._fns: return s._fns[n]
        if n in s._origs: return Calls(s._log, 'orig', s._origs[n])
        if n in s._meth: return Calls(s._log, 'root', n)
        return None
    def addEventListener(s, ev, fn): s._listen.setdefault(ev, []).append(fn)


class _Math:
    floor = staticmethod(lambda v: float(int(v // 1)))
    round = staticmethod(lambda v: float(int(v + 0.5) if v >= 0 else -int(-v + 0.5)))


class _EI:
    def __init__(s, log): s.log = log
    def call(s, name, *a): s.log.append('EI %s(%s)' % (name, ', '.join(repr(x) if isinstance(x, str) else js_string(x) for x in a)))


class Runner:
    """one movie: its display tree (tools/dos2_layout.py walk of the built SWF), the kit source run on it; gestures as the host sends them"""
    def __init__(s, built, code, env):
        import dos2_layout as lay
        s.log = []
        with tempfile.TemporaryDirectory() as td:
            f = os.path.join(td, 'movie.swf'); open(f, 'wb').write(built)
            walk = lay.walk(lay.Mov.load(f), 6); fw, fh = lay.Mov.load(f).stage
        s.stage = Stage(fw, fh)
        ns = {'JSArray': JSArray, 'String': js_string, 'Math': _Math, 'EI': _EI(s.log)}
        for k, (kind, v) in env.items():
            ns[k] = JSArray(v) if kind == 'list' else v
        tree = ast.parse(textwrap.dedent(code))
        class Fix(ast.NodeTransformer):   # AS3 arrays: list literals and *rest become JSArray
            def visit_List(self, node):
                self.generic_visit(node); return ast.Call(ast.Name('JSArray', ast.Load()), [node], [])
            def visit_FunctionDef(self, node):
                self.generic_visit(node)
                if node.args.vararg:
                    r = node.args.vararg.arg
                    node.body.insert(0, ast.Assign([ast.Name(r, ast.Store())], ast.Call(ast.Name('JSArray', ast.Load()), [ast.Name(r, ast.Load())], [])))
                return node
        tree = ast.fix_missing_locations(Fix().visit(tree))
        exec(compile(tree, '<kit>', 'exec'), ns)
        fns = {n: ns[n] for n in re.findall(r'^def (\w+)', code, re.M)}
        (_, _, abc), = Swf(built).abcs()
        ci = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']).endswith('.MainTimeline')][0]
        meth = [abc.short(t.name) for t in abc.instances[ci]['traits'] if t.kind & 15 == 1 and abc.short(t.name) not in fns]
        s.root = Root(s.log, fns, {'dmKF1Orig': 'frame1', 'dmKInitOrig': 'onEventInit', 'dmKDownOrig': 'onEventDown', 'dmKUpOrig': 'onEventUp'}, meth)
        ns['this'] = s.root
        s.root.parent = s.stage; s.root.stage = s.stage
        nodes = {'': s.root}
        for e in walk:
            p = e['path']; par = nodes.get(p.rsplit('.', 1)[0] if '.' in p else '')
            if par is None: continue
            n = Node(s.log, e['name'], p, e['x'], e['y'], e['sx'], e['sy'], e['local'])
            n.parent = par; par._kids.append(n); nodes[p] = n
        s.nodes = nodes
        s.root.events = JSArray(orig_events(built))
        fns['dmKF1'](); fns['dmKInit']()
        s.ev = {n: i for i, n in enumerate(s.root.events)}
        s.take()   # the originals frame1 / onEventInit (recorded, not run)
    def node(s, path): return s.nodes.get(path)
    def send(s, name, down, value=1.0):
        s.root.dmKDown(s.ev[name], 0, value) if down else s.root.dmKUp(s.ev[name], 0, value)
    def gesture(s, kind, x=0.0, y=0.0, value=1.0):
        """as the host: the mouse at the finger, then the event's down + up (hold: down at the long press, up at the release)"""
        s.stage.mouseX, s.stage.mouseY = float(x), float(y)
        ev = {'tap': 'IE dmTap', 'dtap': 'IE dmDTap', 'slong': 'IE dmSLong', 'long': 'IE dmLong', 'hold': 'IE dmHold', 'scroll': 'IE dmScroll', 'set': 'IE dmSet'}[kind]
        s.send(ev, True, value); s.send(ev, False, value)
    def tick(s, n=10):
        for _ in range(n):
            for fn in s.root._listen.get('enterFrame', []): fn(None)
    def take(s):
        out = list(s.log); del s.log[:]; return out


def runner_for(name, wm, trace=1):
    base, _ = base_movies(wm)
    mv = load_map(name); built, rep = kit_patch(base[mv['movie']], mv, trace=trace)
    env = dict(rep['env'])
    return Runner(built, rep['code'], env), mv, rep


def scenario(name, wm):
    """the golden run of one map: the report, then every element tapped at its centre (trace only, then acting), a tap off every element, the
    least hit size, a double tap / long press where the screen gives them no meaning"""
    r, mv, rep = runner_for(name, wm)
    out = ['# %s (%s) - tools/dos2_touchkit.py Runner; regenerate: check --write-golden' % (mv['movie'], name)]
    for p in mv.get('hidden_at_start', []):   # what the movie's own onEventInit hides (the Runner does not run AVM2)
        n = r.node(p)
        if n is not None: n.visible = False; out.append('hidden at the start: %s' % p)
    r.tick(10); out += ['report: ' + x for x in r.take()]
    cent = {}
    for eid, path, kind, fl, pad, g2a in rep['entries']:
        n = r.node('.'.join(path))
        if n is None: out.append('MISSING %s %s' % (eid, '.'.join(path))); continue
        if not n.visible: continue
        b = n.getBounds(None); cent[eid] = (b.x + b.width / 2, b.y + b.height / 2)
    for act in (0, 1):
        r.gesture('set', value=SET_KEY['act'] * 1000000 + act); r.take()
        for eid in cent:
            x, y = cent[eid]; r.gesture('tap', x, y)
            out.append('act=%d tap %s at %.0f,%.0f: %s' % (act, eid, x, y, ' | '.join(r.take()) or '-'))
    r.gesture('tap', 2, 2); out.append('tap at 2,2 (no element): %s' % (' | '.join(r.take()) or '-'))
    if cent:
        eid = list(cent)[0]; n = r.node('.'.join(rep['entries'][0][1])); b = n.getBounds(None)
        x, y = b.x - 12, b.y + b.height / 2
        r.gesture('tap', x, y); out.append('tap 12 px left of %s, least 0: %s' % (eid, ' | '.join(r.take()) or '-'))
        r.gesture('set', value=SET_KEY['pad'] * 1000000 + int(b.width + 40)); r.take()
        r.gesture('tap', x, y); out.append('the same tap, least %d px: %s' % (int(b.width + 40), ' | '.join(r.take()) or '-'))
        x, y = cent[eid]
        r.gesture('dtap', x, y); out.append('double tap on %s: %s' % (eid, ' | '.join(r.take()) or '-'))
        r.gesture('long', x, y); out.append('long press on %s: %s' % (eid, ' | '.join(r.take()) or '-'))
    r.tick(10); out += ['report after: ' + x for x in r.take()]
    return out


# ---------------------------------------------------------------- checks
def check_map(mv, built, entries, main_raw, say):
    """section 6: every path is an instance, every ei: name is live (the class chain or the host), every call: method exists with that many
    parameters, every ie: / hold: name is in the movie's `events`, every host: verb is a dmAct code"""
    (tag, pre, abc), = Swf(built).abcs()
    ci = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']).endswith('.MainTimeline')][0]
    import dos2_layout as lay
    with tempfile.TemporaryDirectory() as td:
        f = os.path.join(td, 'm.swf'); open(f, 'wb').write(built); paths = {e['npath'] for e in lay.walk(lay.Mov.load(f), 6)}
    for eid, path, kind, fl, pad, g2a in entries:
        say('.'.join(path) in paths, '%s: %s: instance %s on the timeline' % (mv['movie'], eid, '.'.join(path)))
    host_ei = set(re.findall(r'ei_register\("(\w+)"', ''.join(p.read_text(encoding='utf-8') for p in (ROOT / 'dm' / 'host').glob('hle_dos2_*.c'))))
    chain = None
    if main_raw is not None and mv.get('class_fn'):
        import dos2_ptouch_gaps as pg
        chain = pg._chain(main_raw, mv['class_fn'])
    events = set(orig_events(built))
    for eid, path, kind, fl, pad, g2a in entries:
        for g, acts in g2a.items():
            for k, p in acts:
                if k == 'host': say(p[0] in ACT_CODE.values(), '%s: %s %s: host verb dmAct(%d) known (hle_dos2_touch_kit.h)' % (mv['movie'], eid, g, p[0]))
                elif k == 'ei':
                    live = p[1] in host_ei or (chain is not None and p[1] in chain)
                    say(live, '%s: %s %s: ei:%s is %s' % (mv['movie'], eid, g, p[1], 'a host handler' if p[1] in host_ei else 'compared by the class chain main+%#x' % mv.get('class_fn', 0) if live else 'NOT live (no host handler, not in the class chain)'))
                elif k == 'call':
                    if p[0]: say(True, '%s: %s %s: call:%s.%s (a child object: checked at run time only)' % (mv['movie'], eid, g, '.'.join(p[0]), p[1])); continue
                    t = abc.trait(ci, p[1])
                    say(t is not None and t.kind & 15 == 1 and len(abc.methods[t.b].params) == len(p[2]), '%s: %s %s: call:.%s with %d argument(s) - a method of the document' % (mv['movie'], eid, g, p[1], len(p[2])))
                elif k in ('ie', 'hold'): say(p in events, '%s: %s %s: %s:%s in the movie\'s events' % (mv['movie'], eid, g, k, p))
                elif k == 'toggle': say(True, '%s: %s %s: toggle:%s' % (mv['movie'], eid, g, p))


def check(src, main_path=None, write_golden=False):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    wm = sources(src)
    main_raw = None
    for p in [main_path] + [d / 'work' / 'exefs-update' / 'main.raw' for d in Path(__file__).resolve().parents]:
        if p and os.path.exists(str(p)): main_raw = open(str(p), 'rb').read(); break
    say(main_raw is not None, 'main.raw found (ei: names are checked against the class chains and main\'s strings)')
    h = KIT_H.read_text(encoding='utf-8')
    say(len(EV_NAMES) == 7 and EV_NAMES[0] == 'IE dmTap' and 'DK_EV_TAP = 0x7f10' in h, 'host names: %s (ids 0x7f10.. in hle_dos2_touch_kit.h)' % ', '.join(EV_NAMES))
    say(set(ACT_CODE) == {'panel', 'side', 'pad', 'key', 'map', 'menu'}, 'dmAct verbs from hle_dos2_touch_kit.h: %s' % ', '.join('%s=%d' % kv for kv in sorted(ACT_CODE.items(), key=lambda kv: kv[1])))
    if main_raw is not None:
        miss = [n for n in EV_NAMES if (b'\0' + n[3:].encode() + b'\0') in main_raw]
        say(not miss, 'the kit\'s event names are no input event of the engine (no clash): %s' % ('none of them is a string of main' if not miss else 'CLASH: ' + ' '.join(miss)))
        for n in ('dmAct', 'dmKRects', 'dmKitTrace'):
            say(b'\0' + n.encode() + b'\0' not in main_raw, 'ExternalInterface %s is not a string of main (no class can take it from the host)' % n)
    base, btn = base_movies(wm)
    # M0 probes
    for name, data in base.items():
        out, rep = m0_patch(data, btn[name]); b = Swf(out); (_, _, B), = b.abcs()
        errs = verify_all(B)
        for who, why in errs: print('       %s: %s' % (who, why))
        say(not errs, 'M0 %s: ABC verifies (%d bodies), %d B -> %d B, sha256 %s' % (name, len(B.bodies), len(data), len(out), hashlib.sha256(out).hexdigest()[:16]))
        g = gr.check(data, out, quiet=True)
        say(g == [], 'M0 %s: every tag but the DoABC one byte for byte (%s)' % (name, g or 'same'))
        bci = [i for i, c in enumerate(B.instances) if B.mname(c['name']).endswith('.MainTimeline')][0]
        dis = lambda n: '\n'.join(B.dis(B.methods[B.trait(bci, n).b].body))
        f1 = dis('frame1')
        say('dmM0F1Orig' in f1 and 'dmM0Names' in f1, 'M0 %s: frame1 -> the original, then the names' % name)
        nm = dis('dmM0Names')
        say(all("'%s'" % n in nm for n in EV_NAMES), 'M0 %s: all %d names pushed into events' % (name, len(EV_NAMES)))
        ev = dis('dmM0Ev')
        say("'dmKitTrace'" in ev and 'hitTestPoint' in ev and all("'%s'" % n in ev for n in btn[name]), 'M0 %s: onEventDown / onEventUp trace a kit event with the button under the mouse (%d buttons)' % (name, len(btn[name])))
        for w in ('onEventDown', 'onEventUp'):
            m = B.methods[B.trait(bci, w).b]
            say(m.flags & 0x04 and len(m.params) == 1, 'M0 %s: %s takes (index, ...rest)' % (name, w))
        oe = orig_events(out); say(oe == orig_events(data), 'M0 %s: frame1 of the original still makes events = %r (the names come after it)' % (name, oe))
    # kit + maps
    screens = load_screens()
    for name in MAPS:
        mv = load_map(name); data = base[mv['movie']]
        entries, why = plan(mv, screens)
        say(not why, 'map %s: every meaning of screen %r covered, actions well-formed%s' % (name, mv['screen'], '' if not why else ': ' + '; '.join(why)))
        if why: continue
        out, rep = kit_patch(data, mv, screens)
        b = Swf(out); (_, _, B), = b.abcs()
        errs = verify_all(B)
        for who, wy in errs: print('       %s: %s' % (who, wy))
        say(not errs, 'kit %s: ABC verifies (%d bodies, +%d methods), %d B -> %d B, sha256 %s' % (mv['movie'], len(B.bodies), len(rep['methods']), len(data), len(out), hashlib.sha256(out).hexdigest()[:16]))
        g = gr.check(data, out, quiet=True)
        say(g == [], 'kit %s: every tag but the DoABC one byte for byte (%s)' % (mv['movie'], g or 'same'))
        (_, _, A0), = Swf(data).abcs()
        say(all(B.methods[i].body.code == A0.methods[i].body.code for i in range(len(A0.methods)) if A0.methods[i].body), 'kit %s: every original method body byte for byte (only appended)' % mv['movie'])
        check_map(mv, out, entries, main_raw, say)
        gold = GOLDEN / (name + '.txt'); run = scenario(name, wm)
        if write_golden: GOLDEN.mkdir(parents=True, exist_ok=True); gold.write_text('\n'.join(run) + '\n', encoding='utf-8'); print('     wrote %s' % gold)
        want = gold.read_text(encoding='utf-8').splitlines() if gold.exists() else None
        say(want == run, 'Runner %s: %d lines %s %s' % (name, len(run), 'equal to' if want == run else 'DIFFER from', gold.name))
        if want is not None and want != run:
            for a, c in zip(want + [''] * len(run), run + [''] * len(want)):
                if a != c: print('       want: %s\n       got:  %s' % (a, c)); break
        acts = [l for l in run if l.startswith('act=1 tap')]
        say(acts and all(('EI dmAct(' in l or 'call root.' in l) for l in acts), 'Runner %s: acting, every element\'s tap makes its action' % name)
        say(all('EI dmAct(' not in l and 'call root.dm' not in l for l in run if l.startswith('act=0 tap')), 'Runner %s: before dmSet act = 1 a tap only traces' % name)
    # the refusals work (a map cannot pass with a hole or a dead name)
    side = load_map('dmSide_p')
    holed = dict(side, elements=[side['elements'][0][:5] + ({},)] + side['elements'][1:])
    say(bool(plan(holed, screens)[1]), 'a map without an action for a meaning of its screen is refused')
    for a in ('pad:IE UIAccept', 'host:fly(1)', 'ie:UIAccept', 'call:x', 'jump:1'):
        try: parse_action(a); ok = False
        except ValueError: ok = True
        say(ok, 'action %r is refused' % a)
    dead = dict(side, elements=[side['elements'][0][:5] + ({'open': 'ei:noSuchNameAnywhere()'},)])
    ents, _ = plan(dead, screens); got = []
    check_map(dead, kit_patch(base['dmSide_p.swf'], side, screens)[0], ents, main_raw, lambda ok, text: got.append(ok))
    say(main_raw is None or False in got, 'an ei: name no host handler and no class chain knows is refused')
    if main_raw is not None:
        import dos2_ptouch_gaps as pg
        say('PlaySound' in pg._chain(main_raw, 0xae2780), 'the UIObject base chain main+0xae2780 compares PlaySound (how an ei: name of a map is found live)')
    return bad


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] in ('m0', 'kit'):
        wm = sources(a[1]); made = m0_movies(wm) if a[0] == 'm0' else kit_movies(wm)
        os.makedirs(a[2], exist_ok=True)
        for n, d in made.items(): open(os.path.join(a[2], n), 'wb').write(d); print('%s: %d B' % (os.path.join(a[2], n), len(d)))
        return
    if a and a[0] == 'check' and len(a) >= 2:
        mp = a[a.index('--main') + 1] if '--main' in a else None
        sys.exit(1 if check(a[1], mp, '--write-golden' in a) else 0)
    if len(a) >= 3 and a[0] == 'run':
        r, mv, rep = runner_for(a[2], sources(a[1]))
        for g in a[3:]:
            if g == 'tick': r.tick(10)
            else:
                k, xy = g.split(':', 1); p = [float(v) for v in xy.split(',')]
                if k == 'set': r.gesture('set', value=p[0] * 1000000 + p[1])
                else: r.gesture(k, p[0], p[1])
            print('%-16s %s' % (g, ' | '.join(r.take()) or '-'))
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
