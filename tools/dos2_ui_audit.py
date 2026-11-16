"""DOS2 UI movies: who listens to what (mouse / touch / custom events) and which ExternalInterface call each handler ends in.

Input of docs/dos2/TOUCH_UI_AUDIT.md. Static only (ABC), built on tools/dos2_abc.py.

  python -I tools/dos2_ui_audit.py one  <file.swf> [--lib]         one movie, readable (--lib: also the shared LS_Classes.* widgets)
  python -I tools/dos2_ui_audit.py all  <dir> <out dir> [glob]     one <name>.txt per movie
  python -I tools/dos2_ui_audit.py pair <t.swf> <c.swf>            EI names: only in the first / only in the second / in both (with arg counts)
  python -I tools/dos2_ui_audit.py dead <main.raw> <swf>...        EI names of a movie that are not a substring of the engine binary

Per movie:
  events     strings "IE ..." pushed by the movie (the `events` array the engine subscribes it to: pad events for `_c`, gestures for `_t`)
  listeners  Class: EVENT->handler {EI reached from the handler through methods of the same class}; [STUB] = the handler body calls nothing
             and sets nothing (a leftover listener: the element does NOT react); target.callback=handler lines are the touch-button wiring of `_t`
  pad        for every "IE ..." case compared in onEventDown / onEventUp of a class: nothing more than its presence (see `dis` for the body)
  EI         name/argc <- Class::method
"""
import sys, os, glob, fnmatch
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dos2_abc as A

import re
CB = re.compile(r'callback|Func$|Fnc$|func$|^on[A-Z].*(Press|Click|Tap|Up|Down|Select|Change)', re.I)
CALL = {'callproperty', 'callpropvoid', 'callproplex', 'callsuper', 'callsupervoid'}


def scan(abc):
    own = abc.owners(); out = {}
    for b in abc.bodies:
        who = own.get(b.method, 'fn#%d' % b.method)
        try: ins = abc.ins(b.code)
        except ValueError: continue
        tok = []   # (kind, text, argc)
        for p0, op, ops, _ in ins:
            nm, fmt = A.OPS[op][0], A.OPS[op][1]
            if nm == 'pushstring': tok.append(('str', abc.S(ops[0]), 0))
            elif nm in ('getlex', 'findpropstrict', 'findproperty'): tok.append(('lex', abc.short(ops[0]), 0))
            elif nm == 'getproperty': tok.append(('prop', abc.short(ops[0]), 0))
            elif nm == 'newfunction': tok.append(('fn', 'fn#%d' % ops[0], 0))
            elif nm in CALL: tok.append(('call', abc.short(ops[0]), ops[1]))
            elif nm in ('setproperty', 'initproperty'): tok.append(('set', abc.short(ops[0]), 0))
            elif nm in ('getlocal0', 'getlocal1', 'getlocal2', 'getlocal3', 'getlocal'): tok.append(('loc', '', 0))
            else: tok.append(('x', nm, 0))
        rec = {'cb': [], 'listen': [], 'ei': [], 'calls': set(), 'ie': set(), 'fns': set()}
        for n, (k, t, a) in enumerate(tok):
            if k == 'str' and t.startswith('IE '): rec['ie'].add(t[3:])
            if k == 'fn': rec['fns'].add(t)
            if k == 'set' and CB.search(t) and n >= 1:
                # <target>.<cb> = this.<handler>   |   <target>.callbackFunctionName = "EI name"
                pv = tok[n - 1]; tg = '?'
                for m in range(n - 2, max(n - 6, -1), -1):
                    if tok[m][0] == 'prop': tg = tok[m][1]; break
                    if tok[m][0] == 'set': break
                if pv[0] == 'prop': rec['cb'].append((tg, t, pv[1]))
                elif pv[0] == 'str': rec['cb'].append((tg, t, '"%s"' % pv[1]))
                elif pv[0] == 'fn': rec['cb'].append((tg, t, pv[1]))
            if k != 'call': continue
            if t in ('initialize', 'init') and a >= 2 and tok[n - 1][0] in ('prop', 'fn'):
                tg = '?'
                for m in range(n - 2, max(n - 10, -1), -1):
                    if tok[m][0] == 'prop' and tok[m][1].endswith(('_mc', '_btn', 'Btn')): tg = tok[m][1]; break
                rec['cb'].append((tg, t + '()', tok[n - 1][1]))
            rec['calls'].add(t)
            if t in ('addEventListener', 'removeEventListener'):
                # arguments: event, handler[, useCapture, priority, weak]; walk back
                h = None; ev = None; m = n - 1
                while m >= 0 and n - m < 16:
                    kk, tt, _ = tok[m]
                    if h is None:
                        if kk in ('prop', 'fn'): h = tt
                        elif kk == 'call': h = tt + '()'
                    else:
                        if kk == 'prop' and m > 0 and tok[m - 1][0] == 'lex': ev = tok[m - 1][1] + '.' + tt; break
                        if kk == 'str': ev = '"%s"' % tt; break
                        if kk == 'prop': ev = '.' + tt; break
                    m -= 1
                if t == 'addEventListener': rec['listen'].append((ev or '?', h or '?'))
            if t == 'call':
                m = n - 1; name = None; lex = False
                while m >= 0 and n - m < 60:
                    if tok[m][0] == 'lex' and tok[m][1] == 'ExternalInterface': lex = True; break
                    m -= 1
                if lex:
                    name = tok[m + 1][1] if m + 1 < n and tok[m + 1][0] == 'str' else '<dynamic>'
                    rec['ei'].append((name, a - 1))
        rec['stub'] = not any(k in ('call', 'set') for k, _, _ in tok)
        out[who] = rec
    return out


def movie(path):
    swf = A.Swf(open(path, 'rb').read()); res = {}; sup = {}
    for _, _, abc in swf.abcs():
        res.update(scan(abc))
        for i in abc.instances: sup[abc.mname(i['name'])] = abc.mname(i['super'])
    return res, sup, swf.symbols()


def reach(res, cls, meth, seen=None):
    """EI names reachable from Class::meth through same-class methods and closures"""
    seen = seen if seen is not None else set(); out = []
    key = cls + '::' + meth if not meth.startswith('fn#') else meth
    if key in seen or key not in res: return out
    seen.add(key); r = res[key]
    out += [e[0] for e in r['ei']]
    for c in r['calls']: out += reach(res, cls, c, seen)
    for f in r['fns']: out += reach(res, cls, f, seen)
    return out


NOISE = {'PlaySound', 'UIAssert', 'inputFocus', 'inputFocusLost', 'didUse2FingerScrolling'}


def report(path, lib=False):
    res, sup, syms = movie(path); L = []
    ie = sorted(set().union(*[r['ie'] for r in res.values()])) if res else []
    L.append('== %s  (%d classes, %d symbol classes)' % (os.path.basename(path), len(sup), len(syms)))
    L.append('events: ' + ' '.join(ie))
    bycls = {}
    for who, r in res.items():
        cls, _, m = who.partition('::')
        bycls.setdefault(cls, []).append((m, r))
    L.append('listeners:')
    libs = []
    for cls in sorted(bycls):
        items = []
        for m, r in bycls[cls]:
            for tg, cbn, h in r['cb']:
                if h.startswith('"'): items.append('%s.%s=EI %s' % (tg, cbn, h)); continue
                ei = sorted(set(reach(res, cls, h)) - NOISE)
                items.append('%s.%s=%s%s' % (tg, cbn, h, ' {%s}' % ','.join(ei) if ei else ''))
            for ev, h in r['listen']:
                ei = sorted(set(reach(res, cls, h.rstrip('()'))) - NOISE)
                hk = h if h.startswith('fn#') else cls + '::' + h
                if hk in res and res[hk]['stub']: h += '[STUB]'
                items.append('%s->%s%s' % (ev.replace('MouseEvent.', 'M.').replace('TouchEvent.', 'T.').replace('Event.', 'E.'), h, ' {%s}' % ','.join(ei) if ei else ''))
        if not items: continue
        items = sorted(set(items))
        if cls.startswith(('LS_Classes.', 'fl.', 'Iggy')) and not lib:
            libs.append(cls.split('.')[-1]); continue
        L.append('  %s (%s): %s' % (cls, sup.get(cls.rstrip('$'), '?').split('.')[-1], '; '.join(items)))
    if libs: L.append('  lib widgets with listeners: ' + ' '.join(sorted(set(libs))))
    L.append('pad cases:')
    for cls in sorted(bycls):
        for m, r in bycls[cls]:
            if r['ie'] and m not in ('frame1',):
                ei = sorted(set(reach(res, cls, m)) - NOISE)
                L.append('  %s::%s: %s%s' % (cls, m, ' '.join(sorted(r['ie'])), ' {%s}' % ','.join(ei) if ei else ''))
    L.append('EI:')
    ei = {}
    for who, r in res.items():
        for name, argc in r['ei']: ei.setdefault(name, {}).setdefault(argc, set()).add(who)
    for name in sorted(ei):
        if name in NOISE: continue
        for argc, whos in sorted(ei[name].items()):
            w = sorted(whos); L.append('  %s/%d <- %s%s' % (name, argc, ', '.join(w[:4]), ' ...' if len(w) > 4 else ''))
    return '\n'.join(L), ei


def main():
    cmd = sys.argv[1]
    if cmd == 'one':
        print(report(sys.argv[2], '--lib' in sys.argv)[0])
    elif cmd == 'all':
        d, o = sys.argv[2], sys.argv[3]; pat = sys.argv[4] if len(sys.argv) > 4 else '*.swf'; os.makedirs(o, exist_ok=True)
        for f in sorted(glob.glob(os.path.join(d, '*.swf'))):
            if not fnmatch.fnmatch(os.path.basename(f), pat): continue
            try: txt = report(f)[0]
            except Exception as e: txt = '== %s: %r' % (f, e)
            open(os.path.join(o, os.path.basename(f)[:-4] + '.txt'), 'w', encoding='utf-8').write(txt + '\n')
    elif cmd == 'dead':
        # EI names of each movie that do not occur anywhere in the engine binary (no handler: the call is ignored by this engine)
        binary = open(sys.argv[2], 'rb').read()
        for f in sys.argv[3:]:
            _, ei = report(f)
            dead = sorted(n for n in ei if n not in NOISE and n != '<dynamic>' and n.encode() not in binary)
            print('%s: %d EI, not in the binary (%d): %s' % (os.path.basename(f), len(ei), len(dead), ' '.join(dead)))
    elif cmd == 'pair':
        _, a = report(sys.argv[2]); _, b = report(sys.argv[3])
        f = lambda e, k: '%s/%s' % (k, ','.join(str(x) for x in sorted(e[k])))
        for title, keys, src in (('only in %s' % os.path.basename(sys.argv[2]), sorted(set(a) - set(b) - NOISE), a),
                                 ('only in %s' % os.path.basename(sys.argv[3]), sorted(set(b) - set(a) - NOISE), b)):
            print('%s (%d): %s' % (title, len(keys), ' '.join(f(src, k) for k in keys)))
        both = sorted((set(a) & set(b)) - NOISE)
        print('both (%d): %s' % (len(both), ' '.join(k if set(a[k]) == set(b[k]) else '%s[%s|%s]' % (k, ','.join(map(str, sorted(a[k]))), ','.join(map(str, sorted(b[k])))) for k in both)))


if __name__ == '__main__':
    main()
