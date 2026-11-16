"""Minimal SWF / AVM2 ABC reader for the DOS2 UI movies (no external tools).

For every .swf it lists: the document class, the methods and slots of the document class (= what the engine may invoke / set),
and every ExternalInterface.call("name", ...) name with the class::method that makes the call (= what the movie asks of the engine).

  python -I tools/dos2_swf_abc.py dump  <file.swf>              one movie, readable
  python -I tools/dos2_swf_abc.py json  <dir> <out.json>        every *.swf in <dir>
  python -I tools/dos2_swf_abc.py diff  <a.swf> <b.swf>         contract difference (EI names, root methods)
"""
import sys, os, struct, zlib, lzma, json


def swf_body(data):
    sig = data[:3]
    if sig == b'FWS': return data[8:]
    if sig == b'CWS': return zlib.decompress(data[8:])
    if sig == b'ZWS':
        props = data[12:17]
        d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[lzma._decode_filter_properties(lzma.FILTER_LZMA1, props)])
        return d.decompress(data[17:], struct.unpack_from('<I', data, 4)[0] - 8)
    raise ValueError('not a swf: %r' % sig)


def swf_tags(body):
    nbits = body[0] >> 3
    p = (5 + 4 * nbits + 7) // 8 + 4
    while p < len(body):
        h = struct.unpack_from('<H', body, p)[0]; p += 2
        code, ln = h >> 6, h & 0x3f
        if ln == 0x3f: ln = struct.unpack_from('<I', body, p)[0]; p += 4
        yield code, body[p:p + ln]
        p += ln
        if code == 0: break


class R:
    def __init__(s, b, p=0): s.b = b; s.p = p
    def u8(s): v = s.b[s.p]; s.p += 1; return v
    def u30(s):
        v = 0; sh = 0
        while True:
            c = s.b[s.p]; s.p += 1; v |= (c & 0x7f) << sh; sh += 7
            if not c & 0x80 or sh >= 35: return v & 0xffffffff
    def s24(s): v = s.b[s.p] | s.b[s.p + 1] << 8 | s.b[s.p + 2] << 16; s.p += 3; return v - (1 << 24) if v & 0x800000 else v


U30_1 = {0x04, 0x05, 0x06, 0x08, 0x25, 0x2c, 0x2d, 0x2e, 0x2f, 0x31, 0x40, 0x41, 0x42, 0x49, 0x53, 0x55, 0x56, 0x58, 0x59, 0x5a, 0x5d, 0x5e,
         0x5f, 0x60, 0x61, 0x62, 0x63, 0x66, 0x67, 0x68, 0x6a, 0x6c, 0x6d, 0x6e, 0x6f, 0x80, 0x86, 0xb2, 0x92, 0x94, 0xc2, 0xc3, 0xf0, 0xf1, 0xf2}
U30_2 = {0x32, 0x43, 0x44, 0x45, 0x46, 0x4a, 0x4c, 0x4e, 0x4f}
BR = set(range(0x0c, 0x1b))
CALLS = {0x46, 0x4f, 0x4c}


class Abc:
    def __init__(s, b):
        r = R(b, 4)
        n = r.u30()
        for _ in range(max(n - 1, 0)): r.u30()
        n = r.u30()
        for _ in range(max(n - 1, 0)): r.u30()
        n = r.u30(); r.p += 8 * max(n - 1, 0)
        n = r.u30(); s.str = ['']
        for _ in range(max(n - 1, 0)):
            l = r.u30(); s.str.append(b[r.p:r.p + l].decode('utf-8', 'replace')); r.p += l
        n = r.u30(); s.ns = ['']
        for _ in range(max(n - 1, 0)): r.u8(); s.ns.append(s.str[r.u30()])
        n = r.u30()
        for _ in range(max(n - 1, 0)):
            for _ in range(r.u30()): r.u30()
        n = r.u30(); s.mn = [('', '')]
        for _ in range(max(n - 1, 0)):
            k = r.u8()
            if k in (0x07, 0x0d): ns = s.ns[r.u30()]; s.mn.append((ns, s.str[r.u30()]))
            elif k in (0x0f, 0x10): s.mn.append(('', s.str[r.u30()]))
            elif k in (0x11, 0x12): s.mn.append(('', '*'))
            elif k in (0x09, 0x0e): nm = s.str[r.u30()]; r.u30(); s.mn.append(('', nm))
            elif k in (0x1b, 0x1c): r.u30(); s.mn.append(('', '*'))
            elif k == 0x1d:
                q = r.u30()
                for _ in range(r.u30()): r.u30()
                s.mn.append(('', 'T<%d>' % q))
            else: raise ValueError('multiname kind %#x' % k)
        n = r.u30(); s.mnames = []
        for _ in range(n):
            pc = r.u30(); r.u30()
            for _ in range(pc): r.u30()
            nm = s.str[r.u30()]; fl = r.u8()
            if fl & 0x08:
                for _ in range(r.u30()): r.u30(); r.u8()
            if fl & 0x80:
                for _ in range(pc): r.u30()
            s.mnames.append((nm, pc))
        for _ in range(r.u30()):
            r.u30(); c = r.u30()
            for _ in range(2 * c): r.u30()
        nc = r.u30(); s.classes = []; s.owner = {}
        for _ in range(nc):
            name = s.mn[r.u30()]; sup = s.mn[r.u30()]; fl = r.u8()
            if fl & 0x08: r.u30()
            for _ in range(r.u30()): r.u30()
            ii = r.u30(); cname = (name[0] + '.' if name[0] else '') + name[1]
            s.owner[ii] = cname + '::<init>'
            c = {'name': cname, 'super': sup[1], 'methods': {}, 'slots': []}
            s.traits(r, c, cname); s.classes.append(c)
        for i in range(nc):
            ci = r.u30(); s.owner[ci] = s.classes[i]['name'] + '::<cinit>'
            s.traits(r, {'methods': {}, 'slots': []}, s.classes[i]['name'] + '$')
        for _ in range(r.u30()):
            r.u30(); s.traits(r, {'methods': {}, 'slots': []}, 'script')
        s.ei = {}; s.strs_used = set()
        for _ in range(r.u30()):
            m = r.u30(); r.u30(); r.u30(); r.u30(); r.u30(); cl = r.u30(); code = b[r.p:r.p + cl]; r.p += cl
            for _ in range(r.u30()):
                for _ in range(5): r.u30()
            s.traits(r, {'methods': {}, 'slots': []}, 'act')
            s.scan(code, s.owner.get(m, 'fn#%d' % m))

    def traits(s, r, c, cname):
        for _ in range(r.u30()):
            nm = s.mn[r.u30()][1]; k = r.u8(); kind = k & 15
            if kind in (0, 6):
                r.u30(); r.u30()
                if r.u30(): r.u8()
                c['slots'].append(nm)
            elif kind in (1, 2, 3):
                r.u30(); m = r.u30(); s.owner[m] = cname + '::' + nm
                if kind == 1: c['methods'][nm] = s.mnames[m][1]
                else: c['slots'].append(nm)
            else: r.u30(); r.u30()
            if k & 0x40:
                for _ in range(r.u30()): r.u30()

    def scan(s, code, who):
        r = R(code); pend = False; first = None; n = len(code)
        try:
            while r.p < n:
                op = r.u8()
                if op in U30_1:
                    v = r.u30()
                    if op == 0x2c:
                        s.strs_used.add(s.str[v])
                        if pend and first is None: first = s.str[v]
                    elif op in (0x60, 0x5d, 0x5e, 0x66) and s.mn[v][1] == 'ExternalInterface': pend = True; first = None
                elif op in U30_2:
                    v = r.u30(); r.u30()
                    if op in CALLS and pend and s.mn[v][1] == 'call':
                        s.ei.setdefault(first if first is not None else '<dynamic>', set()).add(who); pend = False
                elif op in BR: r.s24()
                elif op == 0x1b:
                    r.s24(); c = r.u30()
                    for _ in range(c + 1): r.s24()
                elif op in (0x24, 0x65): r.u8()
                elif op == 0xef: r.u8(); r.u30(); r.u8(); r.u30()
        except IndexError:
            pass


def movie(path):
    body = swf_body(open(path, 'rb').read())
    doc = None; abcs = []
    for code, d in swf_tags(body):
        if code == 82:
            p = d.index(b'\0', 4) + 1; abcs.append(Abc(d[p:]))
        elif code == 72: abcs.append(Abc(d))
        elif code == 76:
            n = struct.unpack_from('<H', d, 0)[0]; p = 2
            for _ in range(n):
                cid = struct.unpack_from('<H', d, p)[0]; e = d.index(b'\0', p + 2); nm = d[p + 2:e].decode(); p = e + 1
                if cid == 0: doc = nm
    out = {'doc': doc, 'ei': {}, 'root_methods': {}, 'root_slots': [], 'classes': [], 'events': [], 'strings': set()}
    for a in abcs:
        for k, v in a.ei.items(): out['ei'].setdefault(k, set()).update(v)
        for c in a.classes:
            out['classes'].append(c['name'])
            if doc and c['name'] == doc: out['root_methods'].update(c['methods']); out['root_slots'] += c['slots']
        out['events'] += sorted(x for x in a.strs_used if x.startswith('IE '))
        out['strings'] |= a.strs_used
    out['ei'] = {k: sorted(v) for k, v in sorted(out['ei'].items())}
    out['strings'] = sorted(x for x in out['strings'] if 0 < len(x) < 48)
    return out


def main():
    cmd = sys.argv[1]
    if cmd == 'dump':
        m = movie(sys.argv[2]); print('document class:', m['doc']); print('classes (%d):' % len(m['classes']), ' '.join(m['classes']))
        print('root methods (%d):' % len(m['root_methods']), ' '.join('%s/%d' % kv for kv in sorted(m['root_methods'].items())))
        print('root slots:', ' '.join(m['root_slots'])); print('input events:', ' '.join(m['events']))
        print('ExternalInterface.call names (%d):' % len(m['ei']))
        for k, v in m['ei'].items(): print('  %-34s %s' % (k, ', '.join(v[:4]) + (' ...' if len(v) > 4 else '')))
    elif cmd == 'json':
        res = {}
        for f in sorted(os.listdir(sys.argv[2])):
            if f.lower().endswith('.swf'):
                p = os.path.join(sys.argv[2], f)
                try: res[f] = movie(p); res[f]['size'] = os.path.getsize(p)
                except Exception as e: res[f] = {'error': repr(e), 'size': os.path.getsize(p)}
        json.dump(res, open(sys.argv[3], 'w'), indent=1)
        print(len(res), 'movies ->', sys.argv[3], '| errors:', [(k, v['error']) for k, v in res.items() if 'error' in v])
    elif cmd == 'diff':
        a, b = movie(sys.argv[2]), movie(sys.argv[3])
        for key in ('ei', 'root_methods'):
            sa, sb = set(a[key]), set(b[key])
            print(key, 'only in A:', ' '.join(sorted(sa - sb))); print(key, 'only in B:', ' '.join(sorted(sb - sa))); print(key, 'common:', len(sa & sb))


if __name__ == '__main__':
    main()
