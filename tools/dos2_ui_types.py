"""DOS2 UI type table: (type id, movie path, creator function, object size, constructor, vtables) from the registration function.

  python tools/dos2_ui_types.py ip "<...>/Payload/Divinity Original Sin 2.app/Divinity Original Sin 2" [out.json]
  python tools/dos2_ui_types.py sw <...>/exefs-update/main.raw [out.json]

iPad 3.6.72: registrar 0x101336d00..0x10133fd40, one `bl 0x1004af75c(mgr, id, creator)` per type; creator = {fn, std::string path}.
Switch 3.6.117 (main 4979b200): registrar 0x10a9670..0x10b3a00, the hash insert is inlined (`str wID,[node,#8]`; map at uimgr+0xd8).
Static only (capstone). Addresses: iPad = Mach-O VA, Switch = offsets in main.raw.
"""
import sys, json, capstone

which, binpath = sys.argv[1], sys.argv[2]
b = open(binpath, 'rb').read(); base = 0x100000000 if which == 'ip' else 0
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
if which == 'ip': lo, hi, TEXT, DLO, DHI = 0x101336d00, 0x10133fd40, (0x1000044c0, 0x101c60620), 0x101d9ce30, 0x101e673e8
else: lo, hi, TEXT, DLO, DHI = 0x10a7000, 0x10b3a00, (0, 0x1e00000), 0x2900000, 0x2bb0000


def dis(a, n): return list(md.disasm(b[a - base:a - base + n], a))


def cstr(a):
    o = a - base; e = b.index(b'\0', o); return b[o:e].decode('latin1')


def creator(fn):
    size = None; ctor = None
    for i in dis(fn, 0x180):
        m, o = i.mnemonic, i.op_str
        if m == 'mov' and o.startswith('w0, #' if which == 'ip' else 'w1, #') and size is None: size = int(o.split('#')[1], 16)
        if m == 'bl' and which == 'sw': ctor = int(o[1:], 16)
        if m == 'b': ctor = int(o[1:], 16); break
        if m == 'ret': break
    return size, ctor


def ctorinfo(c):
    if not c: return None, []
    base_ctor = None; vts = []; page = {}
    for i in dis(c, 0x300):
        m, o = i.mnemonic, i.op_str
        if m == 'bl' and base_ctor is None: base_ctor = int(o[1:], 16)
        if m == 'adrp': r, v = o.split(', #'); page[r] = int(v, 16)
        elif m == 'add' and o.count(',') == 2:
            d, s, v = [x.strip() for x in o.split(',')]
            if s in page and v.startswith('#'):
                a = page[s] + int(v[1:], 16)
                if DLO <= a < DHI: vts.append(a)
        elif m == 'adr':
            a = int(o.split('#')[1], 16)
            if DLO <= a < DHI: vts.append(a)
        elif m == 'ret': break
    return base_ctor, vts[:3]


regfn = {}; rows = []; page = {}; path = None; fn = None; lastmov = {}
for i in dis(lo, hi - lo):
    m, o = i.mnemonic, i.op_str
    if m == 'adrp': r, v = o.split(', #'); page[r] = int(v, 16)
    elif m == 'adr':
        r, v = o.split(', #'); a = int(v, 16)
        if TEXT[0] <= a < TEXT[1]: regfn[r] = a
    elif m == 'add' and o.count(',') == 2:
        d, s, v = [x.strip() for x in o.split(',')]
        if s in page and v.startswith('#'):
            a = page[s] + int(v[1:], 16)
            if TEXT[0] <= a < TEXT[1]: regfn[d] = a
            else:
                try:
                    t = cstr(a)
                    if t.endswith('.swf') and t.startswith('GUI/'): path = t; fn = None
                except Exception: pass
    elif m == 'mov' and ', #' in o:
        r, v = o.split(', #'); lastmov[r] = int(v, 16)
    elif m == 'bl' and which == 'ip' and int(o[1:], 16) == 0x1004af75c and path:
        rows.append([path, fn, lastmov.get('w1')]); path = None
    if which == 'ip' and path and m == 'str' and o.endswith('[x0], #8') and o.split(',')[0] in regfn: fn = regfn[o.split(',')[0]]
    if which == 'sw':
        if path and m == 'str' and o.endswith(', [x19]') and o.split(',')[0] in regfn: fn = regfn[o.split(',')[0]]
        if m == 'str' and o.endswith(', [x0, #8]') and o[0] == 'w' and path and fn:
            rows.append([path, fn, lastmov.get(o.split(',')[0])]); path = None
res = []
for path, fn, tid in rows:
    size, ctor = creator(fn) if fn else (None, None)
    bc, vts = ctorinfo(ctor)
    res.append(dict(path=path, id=tid, creator=fn, size=size, ctor=ctor, base_ctor=bc, vts=vts))
    h = lambda v: hex(v) if v is not None else '?'
    print('%-34s id %-5s creator %-12s size %-6s ctor %-12s base %-12s vt %s' % (path, h(tid), h(fn), h(size), h(ctor), h(bc), ' '.join(hex(v) for v in vts)))
if len(sys.argv) > 3: json.dump(res, open(sys.argv[3], 'w'), indent=1)
