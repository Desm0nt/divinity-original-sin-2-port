"""[DOS2] UI layout of a SWF movie: where every named instance sits (timeline) and how AS3 moves / sizes / anchors it (code).

Input of docs/dos2/TOUCH_LAYOUT.md: the iPad touch layout (`*_t.swf`) measured against the PC movie (and the console `_c` one) so the layout
can be rebuilt on the Switch engine's PC movies. Static only (no player). Only NUMBERS are taken from iPad files (docs/dos2/TOUCH_ART.md).

Timeline part (built on tools/dos2_swf_graft.py):
  stage size, frame rate, frame count, document class (SymbolClass id 0), root frame labels; every instance on the root timeline and inside
  the sprites it places (recursive, depth limit): path, depth, character id, kind, symbol class, matrix decomposed (x, y, scaleX, scaleY,
  rotation), bounds in the character's own px and in STAGE px (matrices composed from the root; clip layers intersect what they mask).
  Characters imported from a library movie (ImportAssets2: LSClasses.swf, uiElements_t.swf ...) are resolved in the movie's folder
  (or --lib dirs) through the library's ExportAssets / SymbolClass.

Code part (built on tools/dos2_abc.py): a small stack decompiler (linear, no control-flow structuring; branches stay `if (..) goto L`) that
turns method bodies into statements, so layout formulas read as code: `this.x = (stage.stageWidth - 120) / 2`. `code` prints, per class,
the methods that touch layout (x / y / width / height / scale / stage size / setAnchor ...); `ei` lists every ExternalInterface.call with
its literal arguments (setAnchor / registerAnchorId ...). Notation: `(a ?: b)` = the value of one of two branches that join (a ternary
whose condition is the `if` just before), `a || b` / `a && b` = short-circuit, `arg1..` = parameters, `l<n>` = locals, `fn#<n>` = a closure.

  python -I tools/dos2_layout.py info <movie.swf> [--depth N] [--all] [--frame N] [--lib DIR]...
        named instances (with --all: unnamed too) as an indented tree; --symbol <class | id> walks that symbol instead of the root;
        --all-frames adds instances placed only on later frames (marked @fN)
  python -I tools/dos2_layout.py pair <t.swf> <pc.swf> [<c.swf>] [--depth N] [--lib DIR]...
        instances matched by named path (then by unique leaf name): position / stage bounds / scale in both, the difference
  python -I tools/dos2_layout.py code <movie.swf> [--class REGEX] [--method REGEX] [--full] [--all-methods]
        decompiled layout code (default: methods with layout statements; --full: whole bodies; --all-methods: every method)
  python -I tools/dos2_layout.py dis  <movie.swf> <Class::method>    the decompiled body of one method (short class name is fine)
  python -I tools/dos2_layout.py ei   <movie.swf>...                 ExternalInterface.call sites with literal args
  python -I tools/dos2_layout.py consts <movie.swf>                  class const / var traits with numeric / string defaults
  python -I tools/dos2_layout.py summary <movie.swf>...              the engine-facing contract: root `layout` / uiScaling / keepInScreen ...
        assignments and const / var defaults, and the UIObject EI calls (setAnchor, registerAnchorId, setMcSize, setPosition, keepUIinScreen
        ...) with literal args. The engine side (iPad UIObject 0x1004a9480 reads these vars, OnResize 0x1004aa918 applies the layout mode):
        docs/dos2/TOUCH_LAYOUT.md section 1
  python -I tools/dos2_layout.py codediff <a.swf> <b.swf> [--class REGEX] [--layout] [--with-lib]
        methods matched by owner (the `<movie>_t_fla.` / `_fla.` prefix ignored): only in a / only in b, and a statement diff of every
        changed method (--layout: only diffs that touch layout statements)
  (code skips the shared LS_Classes / tween library unless --with-lib or --class is given)
  python -I tools/dos2_layout.py json <out_dir> <movie.swf>... [--depth N]   one <name>.json per movie: timeline + layout code + EI
"""
import json, math, os, re, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dos2_abc as A
import dos2_swf_graft as G

T_EXPORT, T_IMPORT, T_IMPORT2, T_FILEATTR, T_SCENE = 56, 57, 71, 69, 86
IDENT = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


# ================================================================ matrices (a, b, c, d, tx, ty) in px; x' = a x + c y + tx, y' = b x + d y + ty
def mat_of(raw):
    if raw is None: return IDENT
    m = G.Bits(raw, 0).matrix(); return (m[0], m[1], m[2], m[3], m[4] / 20.0, m[5] / 20.0)


def mul(P, C):
    """P applied after C"""
    return (P[0] * C[0] + P[2] * C[1], P[1] * C[0] + P[3] * C[1], P[0] * C[2] + P[2] * C[3], P[1] * C[2] + P[3] * C[3],
            P[0] * C[4] + P[2] * C[5] + P[4], P[1] * C[4] + P[3] * C[5] + P[5])


def decompose(m):
    """-> x, y, scaleX, scaleY, rotation deg (Flash convention: scaleY negative when the matrix mirrors)"""
    a, b, c, d, tx, ty = m
    sx = math.hypot(a, b); sy = math.hypot(c, d)
    if a * d - b * c < 0: sy = -sy
    return tx, ty, sx, sy, math.degrees(math.atan2(b, a))


def box_through(m, b):
    if not b: return None
    pts = [(m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]) for x in (b[0], b[2]) for y in (b[1], b[3])]
    return (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))


def union(a, b):
    if not a: return b
    if not b: return a
    return (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))


def inter(a, b):
    if not a or not b: return None
    r = (max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3]))
    return r if r[0] < r[2] and r[1] < r[3] else None


# ================================================================ movie + libraries
class Mov:
    """a SWF with its character table, symbol names, imports resolved lazily in library movies"""
    _cache = {}

    def __init__(s, path, libdirs=()):
        s.path = str(path); s.name = os.path.basename(s.path); s.libdirs = [os.path.dirname(s.path)] + list(libdirs)
        data = open(s.path, 'rb').read(); s.swf = A.Swf(data); s.mv = G.Movie(s.swf)
        r = G.Bits(s.swf.head, 0); x0, x1, y0, y1 = r.rect(); r.align(); p = r.p
        s.stage = ((x1 - x0) / 20.0, (y1 - y0) / 20.0); s.stage_origin = (x0 / 20.0, y0 / 20.0)
        s.fps = struct.unpack_from('<H', s.swf.head, p)[0] / 256.0; s.nframes = struct.unpack_from('<H', s.swf.head, p + 2)[0]
        s.version = s.swf.ver; s.exports = {}; s.labels = []
        n = 1
        for code, d, _ in s.swf.tags:
            if code == T_EXPORT:
                cnt = struct.unpack_from('<H', d, 0)[0]; q = 2
                for _ in range(cnt):
                    cid = struct.unpack_from('<H', d, q)[0]; nm, q = G.cstr(d, q + 2); s.exports[nm.decode('utf-8', 'replace')] = cid
            elif code == T_IMPORT:   # old ImportAssets: url, count, (id, name)
                url, q = G.cstr(d, 0); cnt = struct.unpack_from('<H', d, q)[0]; q += 2
                for _ in range(cnt):
                    cid = struct.unpack_from('<H', d, q)[0]; nm, q = G.cstr(d, q + 2); s.mv.imports[cid] = (url, nm)
            elif code == G.T_SHOW: n += 1
            elif code == G.T_LABEL: s.labels.append((n, G.cstr(d, 0)[0].decode('utf-8', 'replace')))
        s.doc = s.mv.classes.get(0)
        s._bounds = {}

    @classmethod
    def load(cls, path, libdirs=()):
        k = os.path.normcase(os.path.abspath(path))
        if k not in cls._cache: cls._cache[k] = Mov(path, libdirs)
        return cls._cache[k]

    def resolve_import(s, cid):
        """(library Mov, its character id) for an imported character, or (None, None)"""
        url, nm = s.mv.imports[cid]; url = url.decode('utf-8', 'replace'); nm = nm.decode('utf-8', 'replace')
        base = os.path.basename(url.replace('\\', '/'))
        for d in s.libdirs:
            for cand in (base, base.replace('.gfx', '.swf')):
                f = os.path.join(d, cand)
                if os.path.isfile(f):
                    lib = Mov.load(f, s.libdirs[1:])
                    if nm in lib.exports: return lib, lib.exports[nm]
                    if nm in lib.mv.byname: return lib, lib.mv.byname[nm]
                    return lib, None
        return None, None

    def char(s, cid):
        """(Mov that defines it, id there, tag code or None)"""
        if cid in s.mv.imports:
            lib, lid = s.resolve_import(cid)
            if lib and lid is not None and lid in lib.mv.defs: return lib, lid, lib.swf.tags[lib.mv.defs[lid]][0]
            return None, None, None
        if cid in s.mv.defs: return s, cid, s.swf.tags[s.mv.defs[cid]][0]
        return None, None, None

    def symbol(s, cid):
        if cid in s.mv.imports:
            url, nm = s.mv.imports[cid]; return '%s@%s' % (nm.decode('utf-8', 'replace'), os.path.basename(url.decode('utf-8', 'replace')))
        return s.mv.classes.get(cid, '')

    def kind(s, cid):
        m, i, code = s.char(cid)
        return G.KIND.get(code, str(code)) if code is not None else ('import?' if cid in s.mv.imports else '?')

    def display(s, cid, frame=1):
        """display list {depth: rec} of a sprite (or the root when cid is None) at the end of <frame>"""
        if cid is None:
            body = b''.join(G.tag_bytes(code, d) for code, d, _ in s.swf.tags if code in (G.T_PLACE, G.T_PLACE2, G.T_PLACE3, G.T_REMOVE, G.T_REMOVE2, G.T_SHOW, G.T_END))
            return G.frame_state(b'\0\0\0\0' + body, frame)
        code, d, _ = s.swf.tags[s.mv.defs[cid]]
        return G.frame_state(d, frame)

    def frames(s, cid):
        if cid is None: return s.nframes
        code, d, _ = s.swf.tags[s.mv.defs[cid]]
        return G.frames_of(d) if code == G.T_SPRITE else 1

    def bounds(s, cid, frame=1, _stack=()):
        """local bounds (px) of a character, imports resolved, clip layers intersect the depths they mask"""
        key = (cid, frame)
        if key in s._bounds: return s._bounds[key]
        m, i, code = s.char(cid)
        if m is None: return None
        if m is not s: return m.bounds(i, 1, _stack)
        d = s.swf.tags[s.mv.defs[cid]][1]
        if code in G.T_SHAPES or code in G.T_TEXTS or code == G.T_EDIT or code in G.T_MORPHS:
            x0, x1, y0, y1 = G.Bits(d, 2).rect(); b = (x0 / 20.0, y0 / 20.0, x1 / 20.0, y1 / 20.0)
        elif code == G.T_SPRITE and cid not in _stack:
            b = s.list_bounds(s.display(cid, frame), _stack + (cid,))
        else: b = None
        s._bounds[key] = b
        return b

    def list_bounds(s, dl, _stack=()):
        box = None; clips = []
        for depth in sorted(dl):
            rec = dl[depth]
            if rec['char'] is None: continue
            b = box_through(mat_of(rec['matrix']), s.bounds(rec['char'], 1, _stack))
            if rec['clip'] is not None:
                clips.append((depth, struct.unpack_from('<H', rec['clip'], 0)[0], b)); continue
            for d0, d1, cb in clips:
                if d0 < depth <= d1: b = inter(b, cb)
            box = union(box, b)
        return box


# ================================================================ timeline walk
def walk(mov, depth_limit=4, frame=1, all_frames=False, start=None):
    """[{path, npath, level, depth, char, kind, symbol, x, y, sx, sy, rot, alpha?, local, stage, frame, name}] root first, depth first"""
    out = []

    def rec_list(m, cid):
        """[(rec, first frame)] for a sprite / root: frame <frame>, plus (all_frames) depths first seen on later frames"""
        dl = m.display(cid, frame); got = [(dl[k], frame) for k in sorted(dl)]
        if all_frames:
            seen = {(r['depth'], r['char'], r['name']) for r, _ in got}
            for f in range(1, m.frames(cid) + 1):
                if f == frame: continue
                for k, r in sorted(m.display(cid, f).items()):
                    t = (r['depth'], r['char'], r['name'])
                    if t not in seen and r['char'] is not None: seen.add(t); got.append((r, f))
        return got

    def go(m, cid, parent_m, path, npath, level, stack):
        for r, f in rec_list(m, cid):
            if r['char'] is None: continue
            name = r['name'][:-1].decode('utf-8', 'replace') if r['name'] else None
            lm = mat_of(r['matrix']); sm = mul(parent_m, lm)
            x, y, sx, sy, rot = decompose(lm)
            dm, did, code = m.char(r['char'])
            local = m.bounds(r['char'])
            seg = name if name else '[d%d]' % r['depth']
            e = dict(path=(path + '.' if path else '') + seg, npath=(npath + '.' if npath and name else npath) + (name or ''), level=level,
                     depth=r['depth'], char=r['char'], kind=m.kind(r['char']), symbol=m.symbol(r['char']), name=name,
                     x=round(x, 2), y=round(y, 2), sx=round(sx, 4), sy=round(sy, 4), rot=round(rot, 2),
                     local=[round(v, 1) for v in local] if local else None,
                     stage=[round(v, 1) for v in box_through(sm, local)] if local else None,
                     origin=[round(sm[4], 1), round(sm[5], 1)], frame=f, mask=r['clip'] is not None,
                     movie=dm.name if dm is not None and dm is not m else None)
            out.append(e)
            if code == G.T_SPRITE and level + 1 < depth_limit and (dm.name, did) not in stack:
                go(dm, did, sm, e['path'], e['npath'], level + 1, stack | {(dm.name, did)})
    go(mov, start, IDENT, '', '', 0, frozenset())
    return out


def fmt_box(b): return '%.0f,%.0f..%.0f,%.0f (%.0fx%.0f)' % (b[0], b[1], b[2], b[3], b[2] - b[0], b[3] - b[1]) if b else '-'


def head(mov):
    w, h = mov.stage
    s = '%s: stage %gx%g%s, %g fps, %d frames, SWF %d, document class %s' % (mov.name, w, h, ' origin %g,%g' % mov.stage_origin if any(mov.stage_origin) else '',
                                                                          mov.fps, mov.nframes, mov.version, mov.doc or '-')
    if mov.labels: s += '\n  root labels: ' + ' '.join('%d:%s' % l for l in mov.labels)
    return s


def cmd_info(args, depth, show_all, frame, libs, all_frames, sym=None):
    mov = Mov.load(args[0], libs); print(head(mov))
    start = mov.mv.resolve(sym) if sym else None
    if start is not None: print('  symbol %s = #%d, %d frames, local bounds %s' % (sym, start, mov.frames(start), fmt_box(mov.bounds(start))))
    for e in walk(mov, depth, frame, all_frames, start):
        if not show_all and not e['name']: continue
        sc = '' if e['sx'] == 1 and e['sy'] == 1 else ' scale %g,%g' % (e['sx'], e['sy'])
        rt = ' rot %g' % e['rot'] if e['rot'] else ''
        print('%s%-28s d%-4d #%-5d %-9s %-40s at %7.1f,%7.1f%s%s  stage %s%s%s' % (
            '  ' * e['level'], e['name'] or '-', e['depth'], e['char'], e['kind'], (e['symbol'] or '')[:40], e['x'], e['y'], sc, rt,
            fmt_box(e['stage']), ' [mask]' if e['mask'] else '', ' @f%d' % e['frame'] if e['frame'] != frame else ''))


def index(entries):
    by = {}; leaf = {}
    for e in entries:
        if not e['name']: continue
        by.setdefault(e['npath'], e)
        leaf.setdefault(e['name'], []).append(e)
    return by, leaf


def cmd_pair(args, depth, libs):
    movs = [Mov.load(a, libs) for a in args]
    for m in movs: print(head(m))
    ents = [walk(m, depth) for m in movs]; idx = [index(e) for e in ents]
    t_by, t_leaf = idx[0]
    print('\n%-44s | %-34s | %-40s | %s' % ('path in ' + movs[0].name, movs[0].name + ' at / stage', 'counterpart at / stage', 'd(x,y) of stage box / size ratio'))
    used = [set() for _ in movs]
    for e in ents[0]:
        if not e['name']: continue
        line = '%s%-*s | %-34s' % ('  ' * e['level'], 44 - 2 * e['level'], e['npath'][-(44 - 2 * e['level']):], '%.0f,%.0f %s' % (e['x'], e['y'], fmt_box(e['stage'])))
        for k in range(1, len(movs)):
            by, leaf = idx[k]; o = by.get(e['npath']); how = ''
            if o is None and len(leaf.get(e['name'], [])) == 1 and len(t_leaf.get(e['name'], [])) == 1: o = leaf[e['name']][0]; how = '~'
            if o is None: line += ' | %-40s |' % ('(none in %s)' % movs[k].name); continue
            used[k].add(o['npath'])
            dd = ''
            if e['stage'] and o['stage']:
                ow, oh = o['stage'][2] - o['stage'][0], o['stage'][3] - o['stage'][1]; tw, th = e['stage'][2] - e['stage'][0], e['stage'][3] - e['stage'][1]
                dd = 'd %+.0f,%+.0f  x%.2f,%.2f' % (e['stage'][0] - o['stage'][0], e['stage'][1] - o['stage'][1], tw / ow if ow else 0, th / oh if oh else 0)
            line += ' | %-40s | %s' % ('%s%.0f,%.0f %s%s' % (how, o['x'], o['y'], fmt_box(o['stage']), '' if how == '' else ' ' + o['npath'][-24:]), dd)
        print(line)
    for k in range(1, len(movs)):
        rest = [e for e in ents[k] if e['name'] and e['npath'] not in used[k]]
        if rest:
            print('\nonly in %s (%d):' % (movs[k].name, len(rest)))
            for e in rest: print('  %-60s %.0f,%.0f %s' % (e['npath'], e['x'], e['y'], fmt_box(e['stage'])))


# ================================================================ ABC: a linear stack decompiler
BIN = {'add': '+', 'subtract': '-', 'multiply': '*', 'divide': '/', 'modulo': '%', 'lshift': '<<', 'rshift': '>>', 'urshift': '>>>',
       'bitand': '&', 'bitor': '|', 'bitxor': '^', 'equals': '==', 'strictequals': '===', 'lessthan': '<', 'lessequals': '<=',
       'greaterthan': '>', 'greaterequals': '>=', 'instanceof': 'instanceof', 'in': 'in', 'add_i': '+', 'subtract_i': '-', 'multiply_i': '*',
       'istypelate': 'is', 'astypelate': 'as'}
IFS = {'ifeq': '==', 'ifne': '!=', 'iflt': '<', 'ifle': '<=', 'ifgt': '>', 'ifge': '>=', 'ifstricteq': '===', 'ifstrictne': '!==',
       'ifnlt': '!<', 'ifnle': '!<=', 'ifngt': '!>', 'ifnge': '!>='}
CONV = {'convert_s', 'convert_i', 'convert_u', 'convert_d', 'convert_b', 'convert_o', 'coerce_a', 'coerce_s', 'coerce', 'checkfilter', 'esc_xelem', 'esc_xattr'}


def _num(v):
    if isinstance(v, float) and v == int(v) and abs(v) < 1e15: return str(int(v))
    return repr(v)


class Dec:
    """decompile one method body into [(offset, statement)]; .ei = [(offset, name, [args])]"""
    def __init__(s, abc, body, params=0):
        s.abc = abc; s.b = body; s.np = params; s.out = []; s.ei = []

    def loc(s, n):
        if n == 0: return 'this'
        return 'arg%d' % n if n <= s.np else 'l%d' % n

    def run(s):
        abc = s.abc; ins = abc.ins(s.b.code); targets = {}
        for p0, op, ops, _ in ins:
            nm = A.OPS[op][0]
            if A.OPS[op][1] == 'j': targets.setdefault(ops[0], None)
            elif nm == 'lookupswitch':
                targets.setdefault(ops[0], None)
                for t in ops[1]: targets.setdefault(t, None)
        for e in s.b.exc: targets.setdefault(e[2], ['(err)'])
        st = []; dead = False; emit = s.out.append
        def pop():
            return st.pop() if st else '?'
        def popn(n):
            v = [pop() for _ in range(n)]; return v[::-1]
        def mname(i):
            return abc.short(i) if i else '*'
        def obj_prop(o, i, rt):
            n = mname(i)
            if rt:   # runtime name parts are on top of the object
                parts = popn(rt); o = pop(); return (o if o not in ('this', '') else '') + '[' + parts[-1] + ']'
            o = o
            if o in ('this', '') or o.startswith('find:'): return n
            return o + '.' + n
        sc = {}; skip_pop = False
        for k, (p0, op, ops, size) in enumerate(ins):
            nm, fmt = A.OPS[op][0], A.OPS[op][1]
            if p0 in sc and st:   # end of a short-circuit `a || b` / `a && b`
                for how, a_ in sc.pop(p0)[::-1]: b_ = st.pop(); st.append('(%s %s %s)' % (a_, how, b_))
                if targets.get(p0) is None: targets.pop(p0, None)
            if p0 in targets:
                snap = targets[p0]
                if dead and snap is not None: st = list(snap)
                elif snap is not None and len(snap) == len(st) and snap != st: st = ['(%s ?: %s)' % (a, b) if a != b else a for a, b in zip(snap, st)]
                emit((p0, 'L%d:' % p0)); dead = False
            if nm in ('debug', 'debugline', 'debugfile', 'label', 'nop', 'bkpt', 'kill', 'pushscope', 'pushwith', 'popscope'):
                if nm in ('pushscope', 'pushwith'): pop()
                continue
            if nm == 'getlocal0': st.append('this')
            elif nm in ('getlocal1', 'getlocal2', 'getlocal3'): st.append(s.loc(int(nm[-1])))
            elif nm == 'getlocal': st.append(s.loc(ops[0]))
            elif nm in ('setlocal0', 'setlocal1', 'setlocal2', 'setlocal3', 'setlocal'):
                n = int(nm[-1]) if nm != 'setlocal' else ops[0]; v = pop(); emit((p0, '%s = %s' % (s.loc(n), v)))
                if k and A.OPS[ins[k - 1][1]][0] == 'dup' and ('(' in v or 'new ' in v):   # dup (+ dup) + setlocal: the copies left are the local
                    i_ = len(st) - 1
                    while i_ >= 0 and st[i_] == v: st[i_] = s.loc(n); i_ -= 1
            elif nm in ('inclocal', 'inclocal_i'): emit((p0, '%s++' % s.loc(ops[0])))
            elif nm in ('declocal', 'declocal_i'): emit((p0, '%s--' % s.loc(ops[0])))
            elif nm == 'pushbyte': st.append(str(ops[0] - 256 if ops[0] > 127 else ops[0]))
            elif nm == 'pushshort': v = ops[0] & 0xffff; st.append(str(v - 65536 if v > 32767 else v))
            elif nm == 'pushint': st.append(str(struct.unpack('<i', struct.pack('<I', abc.ints[ops[0]] & 0xffffffff))[0]))
            elif nm == 'pushuint': st.append(str(abc.uints[ops[0]]))
            elif nm == 'pushdouble': st.append(_num(struct.unpack('<d', abc.doubles[ops[0]])[0]))
            elif nm == 'pushstring': st.append(json.dumps(abc.S(ops[0]), ensure_ascii=False))
            elif nm in ('pushtrue', 'pushfalse', 'pushnull', 'pushundefined', 'pushnan'): st.append({'pushtrue': 'true', 'pushfalse': 'false', 'pushnull': 'null', 'pushundefined': 'undefined', 'pushnan': 'NaN'}[nm])
            elif nm == 'pushnamespace': st.append('ns#%d' % ops[0])
            elif nm in ('getlex',): st.append(mname(ops[0]))
            elif nm in ('findpropstrict', 'findproperty', 'finddef'): popn(abc.mn_runtime(ops[0])); st.append('find:' + mname(ops[0]))
            elif nm == 'getproperty': st.append(obj_prop(None, ops[0], abc.mn_runtime(ops[0])) if abc.mn_runtime(ops[0]) else obj_prop(pop(), ops[0], 0))
            elif nm in ('setproperty', 'initproperty', 'setsuper'):
                v = pop(); rt = abc.mn_runtime(ops[0])
                tgt = obj_prop(None, ops[0], rt) if rt else obj_prop(pop(), ops[0], 0)
                emit((p0, '%s%s = %s' % ('super.' if nm == 'setsuper' else '', tgt, v)))
            elif nm == 'getsuper': rt = abc.mn_runtime(ops[0]); popn(rt); pop(); st.append('super.' + mname(ops[0]))
            elif nm == 'deleteproperty': rt = abc.mn_runtime(ops[0]); st.append('delete ' + (obj_prop(None, ops[0], rt) if rt else obj_prop(pop(), ops[0], 0)))
            elif nm == 'getdescendants': rt = abc.mn_runtime(ops[0]); popn(rt); st.append(pop() + '..' + mname(ops[0]))
            elif nm in ('callproperty', 'callproplex', 'callpropvoid', 'callsuper', 'callsupervoid', 'constructprop'):
                args = popn(ops[1]); rt = abc.mn_runtime(ops[0])
                if rt: f = obj_prop(None, ops[0], rt)
                else: o = pop(); f = obj_prop(o, ops[0], 0)
                if nm.startswith('callsuper'): f = 'super.' + mname(ops[0])
                if f.endswith('ExternalInterface.call') and args and args[0].startswith('"'):
                    s.ei.append((p0, json.loads(args[0]), args[1:]))
                e = ('new ' if nm == 'constructprop' else '') + '%s(%s)' % (f, ', '.join(args))
                if nm in ('callpropvoid', 'callsupervoid'): emit((p0, e))
                else: st.append(e)
            elif nm in ('callmethod', 'callstatic'):
                args = popn(ops[1]); o = pop(); st.append('%s.%s#%d(%s)' % (o, nm[4:], ops[0], ', '.join(args)))
            elif nm == 'call': args = popn(ops[0]); recv = pop(); f = pop(); st.append('%s(%s)' % (f, ', '.join(args)))
            elif nm == 'construct': args = popn(ops[0]); f = pop(); st.append('new %s(%s)' % (f, ', '.join(args)))
            elif nm == 'constructsuper': args = popn(ops[0]); pop(); emit((p0, 'super(%s)' % ', '.join(args)))
            elif nm == 'applytype': args = popn(ops[0]); f = pop(); st.append('%s.<%s>' % (f, ', '.join(args)))
            elif nm == 'newarray': st.append('[%s]' % ', '.join(popn(ops[0])))
            elif nm == 'newobject':
                kv = popn(2 * ops[0]); st.append('{%s}' % ', '.join('%s: %s' % (kv[i], kv[i + 1]) for i in range(0, len(kv), 2)))
            elif nm == 'newfunction': st.append('function#%d' % ops[0])
            elif nm == 'newclass': pop(); st.append('class ' + abc.mname(abc.instances[ops[0]]['name']))
            elif nm == 'newactivation': st.append('activation')
            elif nm == 'newcatch': st.append('catch#%d' % ops[0])
            elif nm == 'getglobalscope': st.append('global')
            elif nm == 'getscopeobject': st.append('scope%d' % ops[0])
            elif nm == 'getslot': st.append('%s.slot%d' % (pop(), ops[0]))
            elif nm == 'setslot': v = pop(); o = pop(); emit((p0, '%s.slot%d = %s' % (o, ops[0], v)))
            elif nm == 'getglobalslot': st.append('global.slot%d' % ops[0])
            elif nm == 'setglobalslot': emit((p0, 'global.slot%d = %s' % (ops[0], pop())))
            elif nm in BIN: b = pop(); a = pop(); st.append('(%s %s %s)' % (a, BIN[nm], b))
            elif nm in ('istype', 'astype'): st.append('(%s %s %s)' % (pop(), 'is' if nm == 'istype' else 'as', mname(ops[0])))
            elif nm in ('negate', 'negate_i'): st.append('-' + pop())
            elif nm == 'not': st.append('!' + pop())
            elif nm == 'bitnot': st.append('~' + pop())
            elif nm in ('increment', 'increment_i'): st.append('(%s + 1)' % pop())
            elif nm in ('decrement', 'decrement_i'): st.append('(%s - 1)' % pop())
            elif nm == 'typeof': st.append('typeof ' + pop())
            elif nm in CONV:
                v = pop(); st.append('int(%s)' % v if nm == 'convert_i' else 'uint(%s)' % v if nm == 'convert_u' else 'Number(%s)' % v if nm == 'convert_d' and not re.match(r'^-?[\d.]+$', v) else v)
            elif nm == 'dup':
                v = pop()
                nx = ins[k + 1] if k + 1 < len(ins) else None; nx2 = ins[k + 2] if k + 2 < len(ins) else None
                if nx and A.OPS[nx[1]][0] in ('iftrue', 'iffalse') and nx2 and A.OPS[nx2[1]][0] == 'pop':
                    sc.setdefault(nx[2][0], []).append(('||' if A.OPS[nx[1]][0] == 'iftrue' else '&&', v)); st.append(v); skip_pop = 2
                else: st += [v, v]
            elif nm == 'swap': b = pop(); a = pop(); st += [b, a]
            elif nm == 'pop' and not skip_pop:
                v = pop()
                if '(' in v: emit((p0, v))
            elif nm in ('hasnext', 'hasnext2'): (popn(2) if nm == 'hasnext' else None); st.append('hasnext(%s)' % (s.loc(ops[0]) if nm == 'hasnext2' else '..'))
            elif nm in ('nextname', 'nextvalue'): i = pop(); o = pop(); st.append('%s(%s, %s)' % (nm, o, i))
            elif nm == 'jump':
                targets[ops[0]] = list(st) if targets.get(ops[0]) is None else targets[ops[0]]; emit((p0, 'goto L%d' % ops[0])); dead = True
            elif nm in ('iftrue', 'iffalse') and skip_pop == 2: skip_pop = 1
            elif nm == 'pop' and skip_pop == 1: pop(); skip_pop = False
            elif nm in ('iftrue', 'iffalse'):
                c = pop(); emit((p0, 'if (%s%s) goto L%d' % ('' if nm == 'iftrue' else '!', c, ops[0])))
                if targets.get(ops[0]) is None: targets[ops[0]] = list(st)
            elif nm in IFS:
                b = pop(); a = pop(); emit((p0, 'if (%s %s %s) goto L%d' % (a, IFS[nm], b, ops[0])))
                if targets.get(ops[0]) is None: targets[ops[0]] = list(st)
            elif nm == 'lookupswitch':
                v = pop(); emit((p0, 'switch (%s) default L%d, cases %s' % (v, ops[0], ' '.join('L%d' % t for t in ops[1]))))
                for t in [ops[0]] + ops[1]:
                    if targets.get(t) is None: targets[t] = list(st)
                dead = True
            elif nm == 'returnvalue': emit((p0, 'return ' + pop())); dead = True
            elif nm == 'returnvoid': emit((p0, 'return')); dead = True
            elif nm == 'throw': emit((p0, 'throw ' + pop())); dead = True
            elif nm == 'dxns' or nm == 'dxnslate': pass
            else: emit((p0, '/* %s %s */' % (nm, ops)))
        return s


LAYOUT_PROPS = r'\.?\b(x|y|width|height|scaleX|scaleY|stageWidth|stageHeight|rotation)\b\s*=|\b(stageWidth|stageHeight|screenWidth|screenHeight|uiScaling|designResolution)\b'
LAYOUT_EI = {'setAnchor', 'registerAnchorId', 'registeranchorId', 'clearAnchor', 'setPosition', 'setMcSize', 'keepUIinScreen', 'setAnchorId',
             'setUIScale', 'setScale', 'resetAnchor', 'setTooltipSize'}
LAYOUT_NAME = re.compile(r'resiz|anchor|layout|position|align|arrange|scal|size|place|reposition|onEventResize|onInit|init|setup|build|updat|center|offset|slot|grid', re.I)


def methods(abc):
    """[(owner, method index, body, params)]"""
    own = abc.owners(); out = []
    for b in abc.bodies:
        out.append((own.get(b.method, 'fn#%d' % b.method), b.method, b, len(abc.methods[b.method].params)))
    return out


def layout_lines(stmts):
    lr = re.compile(LAYOUT_PROPS)
    return [(p, t) for p, t in stmts if lr.search(t) or any('"%s"' % n in t for n in LAYOUT_EI) or t.endswith(':') is False and re.search(r'\b(setAnchor|addChild|removeChild)\b', t)]


def decomp_movie(mov):
    """[(owner, statements, ei)] for every body of every DoABC block"""
    res = []
    for n, pre, abc in mov.swf.abcs():
        for who, mi, b, np in methods(abc):
            try: d = Dec(abc, b, np).run(); res.append((who, d.out, d.ei))
            except Exception as ex: res.append((who, [(0, '/* decompile failed: %s */' % ex)], []))
    return res


LIBRARY = re.compile(r'^(LS_Classes\.|IggyTween|larTween|flash\.|fl\.|com\.|mx\.|caurina\.|script\d+::)')

# root variables the engine reads from every movie (strings next to each other in the iPad binary, UIObject / FlashObject:
# "layout" fitHorizontal|fitVertical|fixed|fit|fill|fillVFit, isDragging, isUIMoving, uiScaling, keepCustomInScreen, keepInScreen, ...)
ENGINE_VARS = ('layout', 'uiScaling', 'keepCustomInScreen', 'keepInScreen', 'multiplePlayerSupport', 'sysPanelX', 'sysPanelY', 'sysPanelW',
               'sysPanelH', 'uiLeft', 'uiRight', 'uiTop', 'uiMinHeight', 'uiMinWidth', 'anchorId', 'anchorPos', 'anchorTPos', 'anchorTarget',
               'isUIMoving', 'isDragging')
# ExternalInterface names the engine's UIObject base handles itself (same string block)
ENGINE_EI = ('setMcSize', 'registerAnchorId', 'unregisterAnchorId', 'setAnchor', 'keepUIinScreen', 'clearAnchor', 'setPosition', 'moveWindowStart',
             'startMoveWindow', 'cancelMoveWindow')


def summary(mov, dec=None):
    """engine-facing layout contract: {var: [(owner, value)]}, [(EI name, owner, args)]"""
    dec = dec if dec is not None else decomp_movie(mov); vars_ = {}; eis = []
    vr = re.compile(r'^(?:this\.|root\.|root_mc\.|base\.|parent\.)?(%s) = (.+)$' % '|'.join(ENGINE_VARS))
    for cn, k, name, val in consts(mov):
        if name in ENGINE_VARS and not LIBRARY.match(cn + '::'): vars_.setdefault(name, []).append((cn + ' (' + k + ')', json.dumps(val) if isinstance(val, str) else str(val)))
    for who, stmts, ei in dec:
        if LIBRARY.match(who): continue
        for p, t in stmts:
            m = vr.match(t)
            if m: vars_.setdefault(m.group(1), []).append((who, m.group(2)))
        for p, nm, args in ei:
            if nm in ENGINE_EI or nm.lower() in [e.lower() for e in ENGINE_EI]: eis.append((nm, who, args))
    return vars_, eis


def cmd_summary(paths):
    for path in paths:
        mov = Mov.load(path); v, e = summary(mov)
        print('%-28s stage %gx%g %s' % (mov.name, mov.stage[0], mov.stage[1], ' '.join('%s=%s' % (k, '|'.join(sorted({x[1] for x in v[k]}))) for k in ENGINE_VARS if k in v)))
        for nm, who, args in e: print('    %-18s %s  <- %s' % (nm, ', '.join(args), who))


def cmd_code(path, cls_re, meth_re, full, every, with_lib=False):
    mov = Mov.load(path); print(head(mov))
    cr = re.compile(cls_re) if cls_re else None; mr = re.compile(meth_re) if meth_re else None
    for who, stmts, ei in decomp_movie(mov):
        c, _, m = who.partition('::')
        if not with_lib and not cr and LIBRARY.match(who): continue
        if cr and not cr.search(c): continue
        if mr and not mr.search(m): continue
        lay = layout_lines(stmts)
        if not every and not lay and not (mr or cr and full): continue
        if not every and not lay and not full: continue
        print('\n== %s   (%d statements, %d layout)' % (who, len(stmts), len(lay)))
        for p, t in (stmts if full or every else lay): print('  %5d  %s' % (p, t))


def cmd_dis(path, want):
    mov = Mov.load(path); cls, _, meth = want.partition('::')
    for who, stmts, ei in decomp_movie(mov):
        c, _, m = who.partition('::'); m = m.replace('get ', '').replace('set ', '')
        if (c == cls or c.split('.')[-1] == cls or c.rstrip('$').split('.')[-1] == cls or c.split(':')[-1] == cls) and (not meth or m == meth):
            print('\n== %s' % who)
            for p, t in stmts: print('  %5d  %s' % (p, t))


def cmd_ei(paths):
    for path in paths:
        mov = Mov.load(path); print('== ' + mov.name)
        for who, stmts, ei in decomp_movie(mov):
            for p, name, args in ei: print('  %-26s %-58s (%s)' % (name, who[-58:], ', '.join(args)))


def consts(mov):
    out = []
    for n, pre, abc in mov.swf.abcs():
        for i, c in zip(abc.instances, abc.classes):
            cn = abc.mname(i['name'])
            for ts, tag in ((i['traits'], ''), (c['traits'], 'static ')):
                for t in ts:
                    if t.kind & 15 not in (0, 6) or not t.vkind: continue
                    k = t.vkind
                    v = abc.ints[t.vindex] if k == 3 else abc.uints[t.vindex] if k == 4 else struct.unpack('<d', abc.doubles[t.vindex])[0] if k == 6 else \
                        abc.S(t.vindex) if k == 1 else {0x0a: 'false', 0x0b: 'true', 0x0c: 'null', 0x00: 'undefined'}.get(k, 'k%#x' % k)
                    if k == 3: v = struct.unpack('<i', struct.pack('<I', v & 0xffffffff))[0]
                    out.append((cn, tag + ('const' if t.kind & 15 == 6 else 'var'), abc.short(t.name), v))
    return out


def cmd_consts(path):
    mov = Mov.load(path); print(head(mov))
    for cn, k, name, v in consts(mov): print('  %-50s %-12s %-32s = %r' % (cn[-50:], k, name, v))


def _norm_owner(who):
    return re.sub(r'^[A-Za-z0-9]+?(_t|_c)?_fla\.', 'fla.', who)


def _norm_stmt(t):
    t = re.sub(r'\bL\d+\b', 'L', t)
    return re.sub(r'\b[A-Za-z0-9]+?(_t|_c)?_fla\.', 'fla.', t)


def _norm_locals(lines):
    """locals renamed in order of first use (v1, v2 ...), so a shifted register numbering does not show as a change"""
    m = {}
    def f(mo):
        if mo.group(0) not in m: m[mo.group(0)] = 'v%d' % (len(m) + 1)
        return m[mo.group(0)]
    return [re.sub(r'\bl\d+\b', f, t) for t in lines]


def cmd_codediff(a, b, cls_re=None, layout_only=False, with_lib=False, ctx=1):
    """methods matched by owner name (movie prefix of the fla classes ignored); per changed method a short diff of its statements"""
    import difflib
    ma, mb = Mov.load(a), Mov.load(b)
    da = {_norm_owner(w): st for w, st, _ in decomp_movie(ma)}; db = {_norm_owner(w): st for w, st, _ in decomp_movie(mb)}
    cr = re.compile(cls_re) if cls_re else None
    keep = lambda k: (with_lib or not LIBRARY.match(k)) and (not cr or cr.search(k))
    print('%s vs %s' % (ma.name, mb.name))
    only_a = sorted(k for k in da if k not in db and keep(k)); only_b = sorted(k for k in db if k not in da and keep(k))
    print('only in %s (%d): %s' % (ma.name, len(only_a), ' '.join(only_a)))
    print('only in %s (%d): %s' % (mb.name, len(only_b), ' '.join(only_b)))
    for k in sorted(set(da) & set(db)):
        if not keep(k): continue
        x = _norm_locals([_norm_stmt(t) for p_, t in da[k] if not t.endswith(':')]); y = _norm_locals([_norm_stmt(t) for p_, t in db[k] if not t.endswith(':')])
        if x == y: continue
        lines = [l for l in difflib.unified_diff(x, y, lineterm='', n=ctx) if not l.startswith(('---', '+++'))]
        if layout_only and not any(re.search(LAYOUT_PROPS, l) or any('"%s"' % n in l for n in LAYOUT_EI) for l in lines if l[:1] in '+-'): continue
        print('\n== %s' % k)
        for l in lines: print('   ' + l)


def cmd_json(outdir, paths, depth, libs):
    os.makedirs(outdir, exist_ok=True)
    for path in paths:
        mov = Mov.load(path, libs); dec = decomp_movie(mov)
        j = dict(file=mov.name, stage=list(mov.stage), fps=mov.fps, frames=mov.nframes, doc_class=mov.doc, labels=mov.labels,
                 instances=walk(mov, depth),
                 ei=[dict(name=nm, owner=who, offset=p, args=args) for who, st, ei in dec for p, nm, args in ei],
                 layout=[dict(owner=who, lines=['%d  %s' % x for x in layout_lines(st)]) for who, st, ei in dec if layout_lines(st)],
                 consts=[dict(cls=c, kind=k, name=n, value=v) for c, k, n, v in consts(mov)])
        sv, se = summary(mov, dec)
        j['engine_vars'] = {k: [dict(owner=w, value=x) for w, x in v] for k, v in sv.items()}
        j['engine_ei'] = [dict(name=n, owner=w, args=a_) for n, w, a_ in se]
        f = os.path.join(outdir, os.path.splitext(mov.name)[0] + '.json')
        with open(f, 'w', encoding='utf-8') as fh: json.dump(j, fh, ensure_ascii=False, indent=1)
        print('%s: %d instances, %d EI sites, %d layout methods -> %s' % (mov.name, len(j['instances']), len(j['ei']), len(j['layout']), f))


def main():
    a = sys.argv[1:]
    if not a: print(__doc__); return
    def opt(name, default=None, cast=str):
        if name in a:
            i = a.index(name); v = a[i + 1]; del a[i:i + 2]; return cast(v)
        return default
    def flag(name):
        if name in a: a.remove(name); return True
        return False
    libs = []
    while '--lib' in a: libs.append(opt('--lib'))
    depth = opt('--depth', 4, int); frame = opt('--frame', 1, int); show_all = flag('--all'); full = flag('--full'); every = flag('--all-methods')
    allf = flag('--all-frames'); cls_re = opt('--class'); meth_re = opt('--method'); withlib = flag('--with-lib'); symb = opt('--symbol'); flag_lay = flag('--layout')
    cmd = a.pop(0)
    if cmd == 'info': cmd_info(a, depth, show_all, frame, libs, allf, symb)
    elif cmd == 'pair': cmd_pair(a, depth, libs)
    elif cmd == 'code': cmd_code(a[0], cls_re, meth_re, full, every, withlib)
    elif cmd == 'summary': cmd_summary(a)
    elif cmd == 'codediff': cmd_codediff(a[0], a[1], cls_re, flag_lay, withlib)
    elif cmd == 'dis': cmd_dis(a[0], a[1])
    elif cmd == 'ei': cmd_ei(a)
    elif cmd == 'consts': cmd_consts(a[0])
    elif cmd == 'json': cmd_json(a[0], a[1:], depth, libs)
    else: print(__doc__)


if __name__ == '__main__':
    main()
