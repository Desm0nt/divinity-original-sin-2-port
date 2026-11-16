"""AVM2 ABC reader / WRITER / disassembler / tiny assembler + SWF tag rewriter for the DOS2 UI movies (no external tools).

tools/dos2_swf_abc.py only reads the contract of a movie; this module keeps the whole ABC block as a model that serialises back
byte for byte (checked by `roundtrip`), so a movie can be patched: constants / methods / bodies / traits are only APPENDED, so every
index the original code uses stays valid and no original method body is re-linked.

  python -I tools/dos2_abc.py roundtrip <file.swf>                    parse + write every DoABC tag, compare bytes; write + re-read the SWF
  python -I tools/dos2_abc.py classes   <file.swf>                    classes, their traits
  python -I tools/dos2_abc.py dis       <file.swf> <Class>[::method]  disassembly (class name may be the short name)
  python -I tools/dos2_abc.py verify    <file.swf>                    stack / scope / local / branch-target check of EVERY method body
"""
import struct, sys, zlib, lzma

# ---------------------------------------------------------------- opcodes: name, operand format, pops, pushes  (None = depends on operand)
# operand letters: u = u30, b = u8, j = s24 branch, S = lookupswitch, m = multiname index (u30), a = arg count (u30), D = debug
OPS = {
    0x01: ('bkpt', '', 0, 0), 0x02: ('nop', '', 0, 0), 0x03: ('throw', '', 1, 0), 0x04: ('getsuper', 'm', 1, 1), 0x05: ('setsuper', 'm', 2, 0),
    0x06: ('dxns', 'u', 0, 0), 0x07: ('dxnslate', '', 1, 0), 0x08: ('kill', 'u', 0, 0), 0x09: ('label', '', 0, 0),
    0x0c: ('ifnlt', 'j', 2, 0), 0x0d: ('ifnle', 'j', 2, 0), 0x0e: ('ifngt', 'j', 2, 0), 0x0f: ('ifnge', 'j', 2, 0), 0x10: ('jump', 'j', 0, 0),
    0x11: ('iftrue', 'j', 1, 0), 0x12: ('iffalse', 'j', 1, 0), 0x13: ('ifeq', 'j', 2, 0), 0x14: ('ifne', 'j', 2, 0), 0x15: ('iflt', 'j', 2, 0),
    0x16: ('ifle', 'j', 2, 0), 0x17: ('ifgt', 'j', 2, 0), 0x18: ('ifge', 'j', 2, 0), 0x19: ('ifstricteq', 'j', 2, 0), 0x1a: ('ifstrictne', 'j', 2, 0),
    0x1b: ('lookupswitch', 'S', 1, 0), 0x1c: ('pushwith', '', 1, 0), 0x1d: ('popscope', '', 0, 0), 0x1e: ('nextname', '', 2, 1),
    0x1f: ('hasnext', '', 2, 1), 0x20: ('pushnull', '', 0, 1), 0x21: ('pushundefined', '', 0, 1), 0x23: ('nextvalue', '', 2, 1),
    0x24: ('pushbyte', 'b', 0, 1), 0x25: ('pushshort', 'u', 0, 1), 0x26: ('pushtrue', '', 0, 1), 0x27: ('pushfalse', '', 0, 1),
    0x28: ('pushnan', '', 0, 1), 0x29: ('pop', '', 1, 0), 0x2a: ('dup', '', 1, 2), 0x2b: ('swap', '', 2, 2), 0x2c: ('pushstring', 'u', 0, 1),
    0x2d: ('pushint', 'u', 0, 1), 0x2e: ('pushuint', 'u', 0, 1), 0x2f: ('pushdouble', 'u', 0, 1), 0x30: ('pushscope', '', 1, 0),
    0x31: ('pushnamespace', 'u', 0, 1), 0x32: ('hasnext2', 'uu', 0, 1),
    0x40: ('newfunction', 'u', 0, 1), 0x41: ('call', 'a', None, 1), 0x42: ('construct', 'a', None, 1), 0x43: ('callmethod', 'ua', None, 1),
    0x44: ('callstatic', 'ua', None, 1), 0x45: ('callsuper', 'ma', None, 1), 0x46: ('callproperty', 'ma', None, 1), 0x47: ('returnvoid', '', 0, 0),
    0x48: ('returnvalue', '', 1, 0), 0x49: ('constructsuper', 'a', None, 0), 0x4a: ('constructprop', 'ma', None, 1), 0x4c: ('callproplex', 'ma', None, 1),
    0x4e: ('callsupervoid', 'ma', None, 0), 0x4f: ('callpropvoid', 'ma', None, 0), 0x53: ('applytype', 'a', None, 1),
    0x55: ('newobject', 'a', None, 1), 0x56: ('newarray', 'a', None, 1), 0x57: ('newactivation', '', 0, 1), 0x58: ('newclass', 'u', 1, 1),
    0x59: ('getdescendants', 'm', 1, 1), 0x5a: ('newcatch', 'u', 0, 1), 0x5d: ('findpropstrict', 'm', 0, 1), 0x5e: ('findproperty', 'm', 0, 1),
    0x5f: ('finddef', 'm', 0, 1), 0x60: ('getlex', 'm', 0, 1), 0x61: ('setproperty', 'm', 2, 0), 0x62: ('getlocal', 'u', 0, 1),
    0x63: ('setlocal', 'u', 1, 0), 0x64: ('getglobalscope', '', 0, 1), 0x65: ('getscopeobject', 'b', 0, 1), 0x66: ('getproperty', 'm', 1, 1),
    0x68: ('initproperty', 'm', 2, 0), 0x6a: ('deleteproperty', 'm', 1, 1), 0x6c: ('getslot', 'u', 1, 1), 0x6d: ('setslot', 'u', 2, 0),
    0x6e: ('getglobalslot', 'u', 0, 1), 0x6f: ('setglobalslot', 'u', 1, 0), 0x70: ('convert_s', '', 1, 1), 0x71: ('esc_xelem', '', 1, 1),
    0x72: ('esc_xattr', '', 1, 1), 0x73: ('convert_i', '', 1, 1), 0x74: ('convert_u', '', 1, 1), 0x75: ('convert_d', '', 1, 1),
    0x76: ('convert_b', '', 1, 1), 0x77: ('convert_o', '', 1, 1), 0x78: ('checkfilter', '', 1, 1), 0x80: ('coerce', 'm', 1, 1),
    0x82: ('coerce_a', '', 1, 1), 0x85: ('coerce_s', '', 1, 1), 0x86: ('astype', 'm', 1, 1), 0x87: ('astypelate', '', 2, 1),
    0x90: ('negate', '', 1, 1), 0x91: ('increment', '', 1, 1), 0x92: ('inclocal', 'u', 0, 0), 0x93: ('decrement', '', 1, 1),
    0x94: ('declocal', 'u', 0, 0), 0x95: ('typeof', '', 1, 1), 0x96: ('not', '', 1, 1), 0x97: ('bitnot', '', 1, 1),
    0xa0: ('add', '', 2, 1), 0xa1: ('subtract', '', 2, 1), 0xa2: ('multiply', '', 2, 1), 0xa3: ('divide', '', 2, 1), 0xa4: ('modulo', '', 2, 1),
    0xa5: ('lshift', '', 2, 1), 0xa6: ('rshift', '', 2, 1), 0xa7: ('urshift', '', 2, 1), 0xa8: ('bitand', '', 2, 1), 0xa9: ('bitor', '', 2, 1),
    0xaa: ('bitxor', '', 2, 1), 0xab: ('equals', '', 2, 1), 0xac: ('strictequals', '', 2, 1), 0xad: ('lessthan', '', 2, 1),
    0xae: ('lessequals', '', 2, 1), 0xaf: ('greaterthan', '', 2, 1), 0xb0: ('greaterequals', '', 2, 1), 0xb1: ('instanceof', '', 2, 1),
    0xb2: ('istype', 'm', 1, 1), 0xb3: ('istypelate', '', 2, 1), 0xb4: ('in', '', 2, 1),
    0xc0: ('increment_i', '', 1, 1), 0xc1: ('decrement_i', '', 1, 1), 0xc2: ('inclocal_i', 'u', 0, 0), 0xc3: ('declocal_i', 'u', 0, 0),
    0xc4: ('negate_i', '', 1, 1), 0xc5: ('add_i', '', 2, 1), 0xc6: ('subtract_i', '', 2, 1), 0xc7: ('multiply_i', '', 2, 1),
    0xd0: ('getlocal0', '', 0, 1), 0xd1: ('getlocal1', '', 0, 1), 0xd2: ('getlocal2', '', 0, 1), 0xd3: ('getlocal3', '', 0, 1),
    0xd4: ('setlocal0', '', 1, 0), 0xd5: ('setlocal1', '', 1, 0), 0xd6: ('setlocal2', '', 1, 0), 0xd7: ('setlocal3', '', 1, 0),
    0xef: ('debug', 'D', 0, 0), 0xf0: ('debugline', 'u', 0, 0), 0xf1: ('debugfile', 'u', 0, 0),
}
BYNAME = {v[0]: k for k, v in OPS.items()}
MN_QNAME, MN_QNAMEA, MN_RTQ, MN_RTQA, MN_RTQL, MN_RTQLA, MN_MULTI, MN_MULTIA, MN_MULTIL, MN_MULTILA, MN_TYPE = 7, 0xd, 0xf, 0x10, 0x11, 0x12, 9, 0xe, 0x1b, 0x1c, 0x1d
NS_PUBLIC, NS_PRIVATE, NS_PKGINT, NS_PROT = 0x16, 0x05, 0x17, 0x18


class R:
    def __init__(s, b, p=0): s.b = b; s.p = p
    def u8(s): v = s.b[s.p]; s.p += 1; return v
    def u30(s):
        v = 0; sh = 0
        while True:
            c = s.b[s.p]; s.p += 1; v |= (c & 0x7f) << sh; sh += 7
            if not c & 0x80 or sh >= 35: return v & 0xffffffff
    def s24(s): v = s.b[s.p] | s.b[s.p + 1] << 8 | s.b[s.p + 2] << 16; s.p += 3; return v - (1 << 24) if v & 0x800000 else v
    def take(s, n): v = bytes(s.b[s.p:s.p + n]); assert len(v) == n; s.p += n; return v


def u30(v):
    v &= 0xffffffff; o = bytearray()
    while True:
        c = v & 0x7f; v >>= 7
        if v: o.append(c | 0x80)
        else: o.append(c); return bytes(o)


def s24(v): return struct.pack('<i', v)[:3]


class Trait:
    __slots__ = ('name', 'kind', 'a', 'b', 'vindex', 'vkind', 'meta')
    def __init__(s, name, kind, a, b, vindex=0, vkind=0, meta=None): s.name = name; s.kind = kind; s.a = a; s.b = b; s.vindex = vindex; s.vkind = vkind; s.meta = meta or []
    def __eq__(s, o): return isinstance(o, Trait) and (s.name, s.kind, s.a, s.b, s.vindex, s.vkind, s.meta) == (o.name, o.kind, o.a, o.b, o.vindex, o.vkind, o.meta)

    @staticmethod
    def read(r):
        name = r.u30(); k = r.u8(); t = k & 15
        if t in (0, 6):
            a = r.u30(); b = r.u30(); vi = r.u30(); vk = r.u8() if vi else 0; tr = Trait(name, k, a, b, vi, vk)
        elif t in (1, 2, 3, 4, 5): tr = Trait(name, k, r.u30(), r.u30())
        else: raise ValueError('trait kind %d' % t)
        if k & 0x40: tr.meta = [r.u30() for _ in range(r.u30())]
        return tr

    def write(s):
        o = u30(s.name) + bytes([s.kind]) + u30(s.a) + u30(s.b)
        if s.kind & 15 in (0, 6):
            o += u30(s.vindex)
            if s.vindex: o += bytes([s.vkind])
        if s.kind & 0x40: o += u30(len(s.meta)) + b''.join(u30(m) for m in s.meta)
        return o


def rtraits(r): return [Trait.read(r) for _ in range(r.u30())]
def wtraits(ts): return u30(len(ts)) + b''.join(t.write() for t in ts)


class Method:
    def __init__(s): s.params = []; s.ret = 0; s.name = 0; s.flags = 0; s.options = []; s.pnames = []; s.body = None


class Body:
    def __init__(s): s.method = 0; s.max_stack = 0; s.local_count = 0; s.init_scope = 0; s.max_scope = 0; s.code = b''; s.exc = []; s.traits = []


class Abc:
    def __init__(s, b):
        r = R(b); s.version = r.take(4)
        n = r.u30(); s.ints = [0] + [r.u30() for _ in range(max(n - 1, 0))]
        n = r.u30(); s.uints = [0] + [r.u30() for _ in range(max(n - 1, 0))]
        n = r.u30(); s.doubles = [b''] + [r.take(8) for _ in range(max(n - 1, 0))]
        n = r.u30(); s.strings = [b''] + [r.take(r.u30()) for _ in range(max(n - 1, 0))]
        n = r.u30(); s.ns = [(0, 0)] + [(r.u8(), r.u30()) for _ in range(max(n - 1, 0))]
        n = r.u30(); s.nssets = [[]] + [[r.u30() for _ in range(r.u30())] for _ in range(max(n - 1, 0))]
        n = r.u30(); s.mn = [(0,)]
        for _ in range(max(n - 1, 0)):
            k = r.u8()
            if k in (MN_QNAME, MN_QNAMEA, MN_MULTI, MN_MULTIA): s.mn.append((k, r.u30(), r.u30()))   # QName: ns, name; Multiname: name, nsset
            elif k in (MN_RTQ, MN_RTQA, MN_MULTIL, MN_MULTILA): s.mn.append((k, r.u30()))
            elif k in (MN_RTQL, MN_RTQLA): s.mn.append((k,))
            elif k == MN_TYPE: q = r.u30(); s.mn.append((k, q, tuple(r.u30() for _ in range(r.u30()))))
            else: raise ValueError('multiname kind %#x' % k)
        s.methods = []
        for _ in range(r.u30()):
            m = Method(); pc = r.u30(); m.ret = r.u30(); m.params = [r.u30() for _ in range(pc)]; m.name = r.u30(); m.flags = r.u8()
            if m.flags & 0x08: m.options = [(r.u30(), r.u8()) for _ in range(r.u30())]
            if m.flags & 0x80: m.pnames = [r.u30() for _ in range(pc)]
            s.methods.append(m)
        s.metadata = []
        for _ in range(r.u30()):
            nm = r.u30(); c = r.u30(); s.metadata.append((nm, [r.u30() for _ in range(c)], [r.u30() for _ in range(c)]))
        nc = r.u30(); s.instances = []; s.classes = []
        for _ in range(nc):
            i = {'name': r.u30(), 'super': r.u30(), 'flags': r.u8()}
            i['protns'] = r.u30() if i['flags'] & 0x08 else 0
            i['ifaces'] = [r.u30() for _ in range(r.u30())]; i['iinit'] = r.u30(); i['traits'] = rtraits(r); s.instances.append(i)
        for _ in range(nc): s.classes.append({'cinit': r.u30(), 'traits': rtraits(r)})
        s.scripts = [{'init': r.u30(), 'traits': rtraits(r)} for _ in range(r.u30())]
        s.bodies = []
        for _ in range(r.u30()):
            b_ = Body(); b_.method = r.u30(); b_.max_stack = r.u30(); b_.local_count = r.u30(); b_.init_scope = r.u30(); b_.max_scope = r.u30()
            b_.code = r.take(r.u30()); b_.exc = [tuple(r.u30() for _ in range(5)) for _ in range(r.u30())]; b_.traits = rtraits(r)
            s.bodies.append(b_); s.methods[b_.method].body = b_
        assert r.p == len(b), ('trailing bytes in ABC', r.p, len(b))

    def write(s):
        o = [s.version]
        o += [u30(len(s.ints) if len(s.ints) > 1 else 0)] + [u30(v) for v in s.ints[1:]]
        o += [u30(len(s.uints) if len(s.uints) > 1 else 0)] + [u30(v) for v in s.uints[1:]]
        o += [u30(len(s.doubles) if len(s.doubles) > 1 else 0)] + s.doubles[1:]
        o += [u30(len(s.strings) if len(s.strings) > 1 else 0)] + [u30(len(v)) + v for v in s.strings[1:]]
        o += [u30(len(s.ns) if len(s.ns) > 1 else 0)] + [bytes([k]) + u30(n) for k, n in s.ns[1:]]
        o += [u30(len(s.nssets) if len(s.nssets) > 1 else 0)] + [u30(len(v)) + b''.join(u30(x) for x in v) for v in s.nssets[1:]]
        o.append(u30(len(s.mn) if len(s.mn) > 1 else 0))
        for m in s.mn[1:]:
            if m[0] == MN_TYPE: o.append(bytes([m[0]]) + u30(m[1]) + u30(len(m[2])) + b''.join(u30(x) for x in m[2]))
            else: o.append(bytes([m[0]]) + b''.join(u30(x) for x in m[1:]))
        o.append(u30(len(s.methods)))
        for m in s.methods:
            o.append(u30(len(m.params)) + u30(m.ret) + b''.join(u30(p) for p in m.params) + u30(m.name) + bytes([m.flags]))
            if m.flags & 0x08: o.append(u30(len(m.options)) + b''.join(u30(v) + bytes([k]) for v, k in m.options))
            if m.flags & 0x80: o.append(b''.join(u30(p) for p in m.pnames))
        o.append(u30(len(s.metadata)))
        for nm, ks, vs in s.metadata: o.append(u30(nm) + u30(len(ks)) + b''.join(u30(x) for x in ks) + b''.join(u30(x) for x in vs))
        o.append(u30(len(s.instances)))
        for i in s.instances:
            o.append(u30(i['name']) + u30(i['super']) + bytes([i['flags']]) + (u30(i['protns']) if i['flags'] & 0x08 else b'')
                     + u30(len(i['ifaces'])) + b''.join(u30(x) for x in i['ifaces']) + u30(i['iinit']) + wtraits(i['traits']))
        for c in s.classes: o.append(u30(c['cinit']) + wtraits(c['traits']))
        o.append(u30(len(s.scripts)))
        for c in s.scripts: o.append(u30(c['init']) + wtraits(c['traits']))
        o.append(u30(len(s.bodies)))
        for b_ in s.bodies:
            o.append(u30(b_.method) + u30(b_.max_stack) + u30(b_.local_count) + u30(b_.init_scope) + u30(b_.max_scope) + u30(len(b_.code)) + b_.code
                     + u30(len(b_.exc)) + b''.join(b''.join(u30(x) for x in e) for e in b_.exc) + wtraits(b_.traits))
        return b''.join(o)

    # ---- names
    def S(s, i): return s.strings[i].decode('utf-8', 'replace')
    def nsname(s, i): return s.S(s.ns[i][1]) if i else '*'
    def mname(s, i):
        if not i: return '*'
        m = s.mn[i]; k = m[0]
        if k in (MN_QNAME, MN_QNAMEA):
            ns = s.nsname(m[1]); kind = s.ns[m[1]][0] if m[1] else 0
            pre = {NS_PRIVATE: 'private:', NS_PROT: 'protected:', NS_PKGINT: 'internal:'}.get(kind, '')
            return pre + (ns + '.' if ns and ns != '*' else '') + s.S(m[2])
        if k in (MN_MULTI, MN_MULTIA): return s.S(m[1])
        if k in (MN_RTQ, MN_RTQA): return 'rt:' + s.S(m[1])
        if k in (MN_MULTIL, MN_MULTILA): return '[late]'
        if k in (MN_RTQL, MN_RTQLA): return '[rtlate]'
        return s.mname(m[1]) + '.<' + ','.join(s.mname(x) for x in m[2]) + '>'
    def short(s, i):
        m = s.mn[i] if i else (0,)
        if m[0] in (MN_QNAME, MN_QNAMEA): return s.S(m[2])
        return s.mname(i)

    # ---- append-only constant pool helpers (an existing entry is reused)
    def string(s, text):
        b = text.encode('utf-8')
        if b in s.strings[1:]: return s.strings.index(b, 1)
        s.strings.append(b); return len(s.strings) - 1
    def namespace(s, kind, text):
        e = (kind, s.string(text))
        if e in s.ns[1:]: return s.ns.index(e, 1)
        s.ns.append(e); return len(s.ns) - 1
    def qname(s, ns_index, text):
        e = (MN_QNAME, ns_index, s.string(text))
        if e in s.mn[1:]: return s.mn.index(e, 1)
        s.mn.append(e); return len(s.mn) - 1
    def pub(s, text, package=''): return s.qname(s.namespace(NS_PUBLIC, package), text)
    def find_qname(s, text, package=None, kinds=None):
        """indices of every QName with this local name (optionally in this package / of these namespace kinds)"""
        out = []
        for i, m in enumerate(s.mn):
            if i and m[0] == MN_QNAME and s.S(m[2]) == text and (package is None or s.nsname(m[1]) == package) and (kinds is None or s.ns[m[1]][0] in kinds):
                out.append(i)
        return out
    def class_index(s, name):
        hit = [i for i, c in enumerate(s.instances) if s.mname(c['name']) == name or s.short(c['name']) == name]
        if len(hit) != 1: raise KeyError('%d classes named %s' % (len(hit), name))
        return hit[0]
    def trait(s, ci, name, static=False):
        ts = (s.classes if static else s.instances)[ci]['traits']
        hit = [t for t in ts if s.short(t.name) == name]
        return hit[0] if hit else None

    def add_method(s, name, params, ret, code, max_stack, local_count, init_scope, max_scope):
        m = Method(); m.params = list(params); m.ret = ret; m.name = s.string(name); s.methods.append(m)
        b = Body(); b.method = len(s.methods) - 1; b.max_stack = max_stack; b.local_count = local_count; b.init_scope = init_scope; b.max_scope = max_scope
        b.code = code; s.bodies.append(b); m.body = b
        return b.method

    # ---- disassembly
    def ins(s, code):
        """[(offset, opcode, operands, size)]; a branch operand is the absolute target offset"""
        r = R(code); out = []
        while r.p < len(code):
            p0 = r.p; op = r.u8()
            if op not in OPS: raise ValueError('unknown opcode %#x at %d' % (op, p0))
            ops = []
            for f in OPS[op][1]:
                if f in 'uma': ops.append(r.u30())
                elif f == 'b': ops.append(r.u8())
                elif f == 'j': ops.append(r.s24() + r.p)
                elif f == 'S':
                    d = r.s24(); c = r.u30(); ops = [p0 + d, [p0 + r.s24() for _ in range(c + 1)]]
                elif f == 'D': ops = [r.u8(), r.u30(), r.u8(), r.u30()]
            out.append((p0, op, ops, r.p - p0))
        return out

    def dis(s, body):
        lines = []
        for p0, op, ops, _ in s.ins(body.code):
            nm, fmt = OPS[op][0], OPS[op][1]; txt = []
            for f, v in zip(fmt, ops):
                if f == 'm': txt.append(s.mname(v))
                elif f == 'j': txt.append('L%d' % v)
                elif f == 'u' and nm == 'pushstring': txt.append(repr(s.S(v)))
                elif f == 'u' and nm == 'pushint': txt.append(str(struct.unpack('<i', struct.pack('<I', s.ints[v]))[0]))
                elif f == 'u' and nm == 'pushdouble': txt.append(repr(struct.unpack('<d', s.doubles[v])[0]))
                elif f == 'u' and nm == 'newfunction': txt.append('fn#%d' % v)
                elif f == 'u' and nm == 'newclass': txt.append(s.mname(s.instances[v]['name']))
                else: txt.append(str(v))
            if fmt == 'S': txt = ['default L%d' % ops[0]] + ['L%d' % x for x in ops[1]]
            lines.append('%5d  %-16s %s' % (p0, nm, ', '.join(txt)))
        return lines

    # ---- verification of one body: every path gives the same stack / scope depth at a join, nothing underflows, limits hold
    def mn_runtime(s, i):
        k = s.mn[i][0] if i else 0
        return {MN_RTQ: 1, MN_RTQA: 1, MN_MULTIL: 1, MN_MULTILA: 1, MN_RTQL: 2, MN_RTQLA: 2}.get(k, 0)

    def effect(s, op, ops):
        nm, fmt, pop, push = OPS[op]
        if pop is None:
            argc = ops[-1]
            pop = {'call': argc + 2, 'construct': argc + 1, 'callmethod': argc + 1, 'callstatic': argc + 1, 'constructsuper': argc + 1,
                   'applytype': argc + 1, 'newobject': 2 * argc, 'newarray': argc}.get(nm, argc + 1)
        if 'm' in fmt: pop += s.mn_runtime(ops[0])
        return pop, push

    def verify(s, body):
        """returns (max stack reached, max scope reached, highest local used + 1) or raises ValueError"""
        code = body.code; ins = s.ins(code); at = {i[0]: n for n, i in enumerate(ins)}; end = len(code)
        nparams = len(s.methods[body.method].params); fl = s.methods[body.method].flags
        nloc = 1 + nparams + (1 if fl & 0x05 else 0)
        seen = {}; work = [(0, 0, 0)]
        for e in body.exc:
            if e[2] not in at: raise ValueError('exception target %d is not an instruction' % e[2])
            work.append((e[2], 1, 0))
        mst = msc = 0
        while work:
            pc, st, sc = work.pop()
            while True:
                if pc == end: raise ValueError('control falls off the end of the code')
                if pc not in at: raise ValueError('offset %d is not an instruction start' % pc)
                if pc in seen:
                    if seen[pc] != (st, sc): raise ValueError('offset %d reached with stack/scope %s and %s' % (pc, seen[pc], (st, sc)))
                    break
                seen[pc] = (st, sc)
                _, op, ops, size = ins[at[pc]]; nm = OPS[op][0]
                pop, push = s.effect(op, ops)
                if st < pop: raise ValueError('stack underflow at %d (%s): have %d, need %d' % (pc, nm, st, pop))
                st += push - pop; mst = max(mst, st)
                if nm in ('pushscope', 'pushwith'): sc += 1; msc = max(msc, sc)
                elif nm == 'popscope':
                    sc -= 1
                    if sc < 0: raise ValueError('scope underflow at %d' % pc)
                elif nm == 'getscopeobject' and ops[0] >= sc: raise ValueError('getscopeobject %d at %d with %d scopes' % (ops[0], pc, sc))
                loc = {'getlocal0': 0, 'getlocal1': 1, 'getlocal2': 2, 'getlocal3': 3, 'setlocal0': 0, 'setlocal1': 1, 'setlocal2': 2, 'setlocal3': 3}.get(nm)
                if nm in ('getlocal', 'setlocal', 'kill', 'inclocal', 'declocal', 'inclocal_i', 'declocal_i'): loc = ops[0]
                if nm == 'hasnext2': loc = max(ops)
                if nm == 'debug': loc = None
                if loc is not None: nloc = max(nloc, loc + 1)
                for f, v in zip(OPS[op][1], ops):
                    if f == 'm' and not 0 < v < len(s.mn): raise ValueError('multiname %d out of range at %d' % (v, pc))
                    if f == 'u' and nm == 'pushstring' and not 0 < v < len(s.strings): raise ValueError('string %d out of range at %d' % (v, pc))
                if nm in ('returnvoid', 'returnvalue', 'throw'): break
                if nm == 'lookupswitch':
                    for t in [ops[0]] + ops[1]: work.append((t, st, sc))
                    break
                if nm == 'jump': pc = ops[0]; continue
                if OPS[op][1] == 'j': work.append((ops[0], st, sc))
                pc += size
        if mst > body.max_stack: raise ValueError('max_stack %d but the code reaches %d' % (body.max_stack, mst))
        if body.init_scope + msc > body.max_scope: raise ValueError('max_scope_depth %d but the code reaches %d + %d' % (body.max_scope, body.init_scope, msc))
        if nloc > body.local_count: raise ValueError('local_count %d but the code uses %d' % (body.local_count, nloc))
        return mst, msc, nloc

    def owners(s):
        """method index -> readable owner"""
        own = {}
        for ci, (i, c) in enumerate(zip(s.instances, s.classes)):
            cn = s.mname(i['name']); own[i['iinit']] = cn + '::<init>'; own[c['cinit']] = cn + '::<cinit>'
            for ts, sep in ((i['traits'], '::'), (c['traits'], '$::')):
                for t in ts:
                    if t.kind & 15 in (1, 2, 3): own[t.b] = cn + sep + {1: '', 2: 'get ', 3: 'set '}[t.kind & 15] + s.short(t.name)
        for n, sc in enumerate(s.scripts): own[sc['init']] = 'script%d::<init>' % n
        return own


# ---------------------------------------------------------------- tiny assembler: asm(abc, [('getlocal0',), ('iffalse', 'L1'), ('label:', 'L1'), ...])
def asm(lines):
    """lines: (mnemonic, operand ints...) ; a branch takes a label name; ('L', name) defines a label. Returns code bytes."""
    out = bytearray(); fix = []; lab = {}
    for ln in lines:
        if ln[0] == 'L': lab[ln[1]] = len(out); continue
        op = BYNAME[ln[0]]; fmt = OPS[op][1]; out.append(op); assert len(ln) - 1 == len(fmt), ln
        for f, v in zip(fmt, ln[1:]):
            if f in 'uma': out += u30(v)
            elif f == 'b': out.append(v & 0xff)
            elif f == 'j': fix.append((len(out), v)); out += b'\0\0\0'
            else: raise ValueError(ln)
    for pos, name in fix: out[pos:pos + 3] = s24(lab[name] - (pos + 3))
    return bytes(out)


# ---------------------------------------------------------------- SWF container
class Swf:
    def __init__(s, data):
        s.sig = data[:3]; s.ver = data[3]
        if s.sig == b'FWS': body = data[8:]
        elif s.sig == b'CWS': body = zlib.decompress(data[8:])
        elif s.sig == b'ZWS':
            d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[lzma._decode_filter_properties(lzma.FILTER_LZMA1, data[12:17])])
            body = d.decompress(data[17:], struct.unpack_from('<I', data, 4)[0] - 8)
        else: raise ValueError('not a swf: %r' % s.sig)
        assert len(body) + 8 == struct.unpack_from('<I', data, 4)[0], 'SWF length field'
        nbits = body[0] >> 3; p = (5 + 4 * nbits + 7) // 8 + 4
        s.head = body[:p]; s.tags = []   # [code, payload, long header?]
        while p < len(body):
            h = struct.unpack_from('<H', body, p)[0]; p += 2; code, ln = h >> 6, h & 0x3f; lng = ln == 0x3f
            if lng: ln = struct.unpack_from('<I', body, p)[0]; p += 4
            s.tags.append([code, body[p:p + ln], lng]); p += ln
            if code == 0: break
        s.tail = body[p:]

    def body(s):
        o = [s.head]
        for code, d, lng in s.tags:
            if lng or len(d) >= 0x3f: o.append(struct.pack('<HI', code << 6 | 0x3f, len(d)))
            else: o.append(struct.pack('<H', code << 6 | len(d)))
            o.append(d)
        return b''.join(o) + s.tail

    def write(s, compress=True):
        b = s.body()
        if compress: return b'CWS' + bytes([s.ver]) + struct.pack('<I', len(b) + 8) + zlib.compress(b, 9)
        return b'FWS' + bytes([s.ver]) + struct.pack('<I', len(b) + 8) + b

    def abcs(s):
        """[(tag index, prefix bytes before the ABC block, Abc)]"""
        out = []
        for n, (code, d, _) in enumerate(s.tags):
            if code == 82: p = d.index(b'\0', 4) + 1; out.append((n, d[:p], Abc(d[p:])))
            elif code == 72: out.append((n, b'', Abc(d)))
        return out

    def symbols(s):
        """SymbolClass: class name -> character id"""
        out = {}
        for code, d, _ in s.tags:
            if code == 76:
                n = struct.unpack_from('<H', d, 0)[0]; p = 2
                for _ in range(n):
                    cid = struct.unpack_from('<H', d, p)[0]; e = d.index(b'\0', p + 2); out[d[p + 2:e].decode()] = cid; p = e + 1
        return out


def verify_all(abc, only=None):
    own = abc.owners(); bad = []
    for b in abc.bodies:
        if only is not None and b.method not in only: continue
        try: abc.verify(b)
        except ValueError as e: bad.append((own.get(b.method, 'fn#%d' % b.method), str(e)))
    return bad


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    data = open(path, 'rb').read(); swf = Swf(data)
    if cmd == 'roundtrip':
        raw = Swf(data); ok = raw.body() == (zlib.decompress(data[8:]) if data[:3] == b'CWS' else data[8:]); bad = not ok
        print('SWF body: %d tags, rebuilt %s' % (len(swf.tags), 'byte-identical' if ok else 'DIFFERS'))
        for n, pre, abc in swf.abcs():
            src = swf.tags[n][1][len(pre):]; out = abc.write(); ok = out == src; bad |= not ok
            print('DoABC tag #%d: %d B, %d strings, %d multinames, %d methods, %d bodies, %d classes: rewritten %s' % (
                n, len(src), len(abc.strings), len(abc.mn), len(abc.methods), len(abc.bodies), len(abc.instances), 'byte-identical' if ok else 'DIFFERS'))
        again = Swf(swf.write()); ok = again.body() == swf.body(); bad |= not ok
        print('compressed again and re-read:', 'equal' if ok else 'DIFFERS')
        sys.exit(1 if bad else 0)
    if cmd == 'verify':
        tot = 0; nbad = 0
        for n, pre, abc in swf.abcs():
            bad = verify_all(abc); tot += len(abc.bodies); nbad += len(bad)
            for who, why in bad: print('  %s: %s' % (who, why))
        print('%s: %d method bodies checked (stack, scope, locals, branch targets, constant indices), %d bad' % (path, tot, nbad))
        sys.exit(1 if nbad else 0)
    for n, pre, abc in swf.abcs():
        own = abc.owners()
        if cmd == 'classes':
            for i, c in zip(abc.instances, abc.classes):
                print('%s extends %s' % (abc.mname(i['name']), abc.mname(i['super'])))
                for ts, tag in ((i['traits'], ''), (c['traits'], 'static ')):
                    for t in ts:
                        k = t.kind & 15
                        if k in (1, 2, 3): print('    %s%s %s/%d' % (tag, {1: 'method', 2: 'getter', 3: 'setter'}[k], abc.mname(t.name), len(abc.methods[t.b].params)))
                        else: print('    %s%s %s : %s' % (tag, {0: 'var', 6: 'const', 4: 'class', 5: 'function'}[k], abc.mname(t.name), abc.mname(t.b) if k in (0, 6) else ''))
        elif cmd == 'dis':
            want = sys.argv[3]; cls, _, meth = want.partition('::')
            for b in abc.bodies:
                who = own.get(b.method, 'fn#%d' % b.method); c, _, m = who.partition('::')
                m = m.replace('get ', '').replace('set ', '')
                if (c == cls or c.split('.')[-1] == cls or c.rstrip('$').split('.')[-1] == cls or who == want) and (not meth or m == meth):
                    mm = abc.methods[b.method]
                    print('\n=== %s (%s) : %s   [method %d, max_stack %d, locals %d, scope %d..%d, %d B]' % (
                        who, ', '.join(abc.mname(p) for p in mm.params), abc.mname(mm.ret), b.method, b.max_stack, b.local_count, b.init_scope, b.max_scope, len(b.code)))
                    print('\n'.join(abc.dis(b)))


if __name__ == '__main__':
    main()
