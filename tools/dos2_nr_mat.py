"""DOS2 NR-33B (DM_NR_MAT): material shaders in depth-only passes - the gate records, built on the player's machine (setup stage 8).

Why: 64 % of the depth-only vertices of Fort Joy (shadow atlas 1536x1536 D16, depth pre-pass 2160x972 D24S8; play-1118) are drawn by one
translated fragment shader family (Base_PBR / Base_Tile / RedChannelOnly / SK ..._DEP_NX: 45 programs, one SPIR-V) that writes a "fade"
value to colour 0 while NVN's alpha test (GREATER 0.5) is on. In a pass without colour attachments that value only feeds the alpha test,
and it is the constant 1.0 unless one material uniform (c3[0].x) is exactly 1.0 (the camera-fade dissolve). The host (dm/gfx/nr/nr_mat.c,
xbg_nr_mat.inc) then draws these with an empty fragment shader: no varyings, no texture fetch, no kill -> early depth, LRZ / FPK on.

What this tool proves, per fragment shader key (the translated bundle of the player's own pack):
  a symbolic walk of the SPIR-V (every input, texture and other uniform unknown; uniform words that are only compared with constants are
  kept as symbols) enumerates the paths. A "gate path" ends in OpReturn, is reached only through branches decided by such comparisons
  (no branch on an unknown value) and kills nothing; when Location 0 .w is a known constant at its OpReturn, the record carries it (what
  the host's alpha test compares). Any input that satisfies a gate path's comparisons runs exactly that path, so for such a draw:
  alpha test off -> no fragment is killed and the colour outputs go nowhere (no colour attachment) -> the empty shader is exact;
  alpha test on  -> the fragment passes iff func(w, ref) holds (the host evaluates it) -> exact again.
  Each record is then checked on tools/spv_interp.py with seeded random inputs / textures / uniforms (gate words drawn from the
  comparison constants, their neighbours, 0, 1, NaN): whenever a gate path holds, the run must not kill and must write w.
Records: pack key "dg.<fragment key>", bytes (little endian):
  u32 'XGM1' 0x314d4758, u32 1, u64 FNV-1a 64 of the bundle's SPIR-V bytes, u32 paths, u32 terms,
  per path: u32 w bits, u32 flags (1 = w known), u32 first term, u32 term count;
  per term: u32 bank_word (Maxwell bank << 16 | word, NVN uniform slot = bank - 3), u32 SPIR-V compare opcode (170..191),
            u32 constant bits, u32 flags (1 = the uniform is the left operand, 2 = expected result true).
Nothing of the game goes into the APK: the records live in <shaders>/native-nr33b.xgspack, made here from the player's shaders.xgspack.

  dos2_nr_mat.py --pack dm/shaders/shaders.xgspack --programs work/shaders-jobs/programs.tsv --out DIR [--n 24]   what setup runs
  dos2_nr_mat.py --bundles work/shaders-vk ... (a directory of <key>.xgs instead of the pack); --keys K1,K2 (only these fragment keys)
Output: DIR/native-nr33b.xgspack, DIR/nr_mat.tsv (one row per fragment key), DIR/report.txt. Exit 1 when a record fails its check.
"""
import argparse, collections, copy, hashlib, random, shutil, struct, sys, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'dm' / 'gfx' / 'tools'))
import numpy as np
import spv_interp as SI

MAGIC, VERSION = 0x314d4758, 1
CMP_OPS = set(range(170, 192))
F = np.float32


def fnv64(b):
    x = 0xcbf29ce484222325
    for c in b: x = ((x ^ c) * 0x100000001b3) & 0xffffffffffffffff
    return x


def cmp_raw(op, a, b):
    """SPIR-V comparison on raw 32-bit words (dm/gfx/nr/nr_mat.c nr_mat_cmp): FOrd* false on NaN, FUnord* true on NaN, ints signed / unsigned"""
    a &= 0xffffffff; b &= 0xffffffff
    if 180 <= op <= 191:
        x, y = SI.u2f(a), SI.u2f(b); nan = bool(np.isnan(x) or np.isnan(y))
        k = (op - 180) // 2; unord = (op - 180) % 2
        r = [x == y, x != y, x < y, x > y, x <= y, x >= y][k]
        return bool(r) or nan if unord else bool(r) and not nan
    s = lambda v: v - (1 << 32) if v & 0x80000000 else v
    return {170: a == b, 171: a != b, 172: a > b, 173: s(a) > s(b), 174: a >= b, 175: s(a) >= s(b),
            176: a < b, 177: s(a) < s(b), 178: a <= b, 179: s(a) <= s(b)}[op]


# ---------------------------------------------------------------------------------------------------------------- symbolic values
class _Top:
    __slots__ = ()
    def __repr__(self): return 'T'
TOP = _Top()


class Sym:
    """the raw 32-bit word of a uniform: bank_word = Maxwell bank << 16 | word"""
    __slots__ = ('bw',)
    def __init__(self, bw): self.bw = bw
    def __eq__(self, o): return isinstance(o, Sym) and o.bw == self.bw
    def __hash__(self): return hash(('S', self.bw))
    def __repr__(self): return 'U%x:%d' % (self.bw >> 16, self.bw & 0xffff)


class Cond:
    """op(a, b) with one operand a Sym and the other a constant word; neg = logical not of it"""
    __slots__ = ('op', 'a', 'b', 'neg')
    def __init__(self, op, a, b, neg=False): self.op, self.a, self.b, self.neg = op, a, b, neg
    def base(self): return (self.op, self.a, self.b)
    def __repr__(self): return '%s%d(%r,%r)' % ('!' if self.neg else '', self.op, self.a, self.b)


class Abort(Exception): pass
class _Kill(Exception): pass


def conc(v):
    if isinstance(v, list): return all(conc(x) for x in v)
    return not isinstance(v, (_Top, Sym, Cond))


class Abs(SI.Module):
    """symbolic walk of a fragment shader (see the module doc)"""
    def __init__(self, data, slot_of_binding):
        super().__init__(data); self.slot_of = slot_of_binding

    def top(self, t):
        op, a = self.types[t]
        if op in (23, 24): return [self.top(a[0]) for _ in range(a[1])]
        if op == 28: return [self.top(a[0]) for _ in range(self.consts[a[1]])]
        if op == 30: return [self.top(m) for m in a]
        return TOP

    def _word(self, x):
        return SI.f2u(x) if isinstance(x, (float, np.floating)) else (int(x) & 0xffffffff if not isinstance(x, bool) else None)

    # --- overrides of the concrete interpreter
    def _load(self, p):
        if len(p) > 3: return self.top(p[3])                       # a pointer with an unknown index
        _, root, path = p
        if root in self.ubufs: return self._usym(root, path)
        if root in self.tex: return TOP
        v = self.mem[root]
        if v is TOP: return TOP
        for k in path:
            v = v[k]
            if v is TOP: return TOP
        return self._copy(v)

    def _store(self, p, val):
        if len(p) > 3: self.mem[p[1]] = self.top(self.vtype[p[1]]); return
        super()._store(p, val)

    def _usym(self, var, path):
        """a uniform load: 32-bit scalars / vectors of them -> Sym per word, anything else unknown"""
        if self.vars[var][1] != 2: return self.top(self.types[self.vars[var][0]][1][1])
        d = self.dec.get(var, {}); binding = d.get(33, [None])[0]
        slot = self.slot_of.get(binding)
        t = self.types[self.vars[var][0]][1][1]; off = 0
        for idx in path:
            op, a = self.types[t]
            if op == 30: off += self.mdec.get((t, idx), {})[35][0]; t = a[idx]
            elif op in (28, 29): off += idx * self.dec[t][6][0]; t = a[0]
            elif op == 23: off += 4 * idx; t = a[0]
            else: return self.top(t)
        op, a = self.types[t]
        if slot is None or off % 4: return self.top(t)
        bw = lambda o: ((slot + 3) << 16) | (o // 4)
        if op in (21, 22) and a[0] == 32: return Sym(bw(off))
        if op == 23 and self.types[a[0]][0] in (21, 22): return [Sym(bw(off + 4 * k)) for k in range(a[1])]
        return self.top(t)

    def _image(self, op, rt, a, g): return self.top(rt)

    def _ext(self, rt, inst, x):
        if not all(conc(v) for v in x): return self.top(rt)
        return super()._ext(rt, inst, x)

    def _map(self, op, rt, args):
        if all(conc(v) for v in args): return super()._map(op, rt, args)
        if isinstance(args[0], list): return self.top(rt)
        if op in CMP_OPS and len(args) == 2:
            x, y = args
            if isinstance(x, Sym) and conc(y) and self._word(y) is not None: return Cond(op, x, self._word(y))
            if isinstance(y, Sym) and conc(x) and self._word(x) is not None: return Cond(op, self._word(x), y)
            return TOP
        if op == 168 and isinstance(args[0], Cond): c = args[0]; return Cond(c.op, c.a, c.b, not c.neg)   # LogicalNot
        if op == 124 and isinstance(args[0], Sym): return args[0]                                         # Bitcast keeps the word
        if op == 83: return args[0]
        return self.top(rt)

    def _exec(self, env, op, a):
        g = lambda i: self._get(env, i)
        if op in (65, 66):                                                                               # OpAccessChain
            p = g(a[2]); idx = [g(x) for x in a[3:]]
            if len(p) > 3 or not all(conc(i) for i in idx): env[a[1]] = ('ptr', p[1], p[2], self.types[a[0]][1][1]); return
            env[a[1]] = ('ptr', p[1], p[2] + tuple(self._idx(i) for i in idx)); return
        if op == 59:
            env[a[1]] = ('ptr', ('fn', a[1]), ()); self.vtype[('fn', a[1])] = self.types[a[0]][1][1]
            self.mem[('fn', a[1])] = self._null(self.types[a[0]][1][1]) if len(a) < 4 else g(a[3]); return
        if op == 57: raise Abort('function call')
        if op == 169:                                                                                    # OpSelect
            c, x, y = g(a[2]), g(a[3]), g(a[4])
            if conc(c): return super()._exec(env, op, a)
            env[a[1]] = x if conc(x) and conc(y) and x == y else self.top(a[0]); return
        if op in (148, 142, 143, 144, 145, 146, 147, 149, 150, 151, 152, 153):                           # dot / matrix products
            if not all(conc(g(x)) for x in a[2:]): env[a[1]] = self.top(a[0]); return
        if op in (207, 208, 209, 210, 211, 212, 213, 214, 215):                                          # derivatives
            env[a[1]] = self.top(a[0]); return
        if op in (100, 86):
            env[a[1]] = TOP; return
        if op in (103, 104, 105, 106, 107): env[a[1]] = self.top(a[0]); return                           # image queries
        if op in (95, 87, 88, 89, 90, 91, 92, 93, 94, 96, 97, 98, 99):
            if op == 99: raise Abort('image write')
            env[a[1]] = self.top(a[0]); return
        if op == 124 and isinstance(g(a[2]), list) and not conc(g(a[2])): env[a[1]] = self.top(a[0]); return
        try: return super()._exec(env, op, a)
        except (TypeError, NotImplementedError, KeyError, IndexError, AttributeError) as e: raise Abort('opcode %d: %s' % (op, type(e).__name__))

    # --- the walk
    def analyze(self, max_paths=512, max_steps=600000):
        self.ubufs, self.mem, self.tex, self.vtype = {}, {}, {}, {}
        outs = {}
        for v, (pt, sc) in self.vars.items():
            t = self.types[pt][1][1]; d = self.dec.get(v, {}); self.vtype[v] = t
            if sc in (2, 12, 9): self.ubufs[v] = b''
            elif sc == 0: self.tex[v] = None
            elif sc == 1: self.mem[v] = self.top(t)
            elif sc == 3: self.mem[v] = self.top(t)                                        # outputs start undefined
            elif sc in (6, 4): self.mem[v] = self._null(t)
            else: raise Abort('storage class %d' % sc)
            if sc == 3 and 30 in d: outs[d[30][0]] = v
        if len(self.funcs) != 1: raise Abort('%d functions' % len(self.funcs))
        fn = self.funcs[self.entry]
        stack = [dict(env={}, mem=self.mem, prev=None, cur=fn['order'][0], assume={}, terms=[], taint=False)]
        paths, steps = [], 0
        while stack:
            st = stack.pop(); self.mem = st['mem']; env = st['env']; prev, cur = st['prev'], st['cur']
            end = None
            while end is None:
                nxt = None
                for op, a in fn['blocks'][cur]:
                    steps += 1
                    if steps > max_steps: raise Abort('step limit')
                    if op == 245:
                        for k in range(2, len(a), 2):
                            if a[k + 1] == prev: env[a[1]] = self._get(env, a[k]); break
                        continue
                    if op in (246, 247, 8, 317): continue
                    if op == 249: nxt = a[0]; break
                    if op == 250:
                        c = self._get(env, a[0])
                        if isinstance(c, (bool, np.bool_)): nxt = a[1] if c else a[2]; break
                        if isinstance(c, Cond):
                            known = st['assume'].get(c.base())
                            if known is not None: nxt = a[1] if known != c.neg else a[2]; break
                            other = self._fork(st, env, cur, a[2])
                            other['assume'][c.base()] = c.neg; other['terms'].append((c.base(), c.neg))   # base value making !c
                            stack.append(other)
                            st['assume'][c.base()] = not c.neg; st['terms'].append((c.base(), not c.neg))
                            nxt = a[1]; break
                        other = self._fork(st, env, cur, a[2]); other['taint'] = True; stack.append(other)
                        st['taint'] = True; nxt = a[1]; break
                    if op == 251:
                        sel = self._get(env, a[0])
                        if conc(sel):
                            nxt = a[1]
                            for k in range(2, len(a), 2):
                                if a[k] == sel: nxt = a[k + 1]; break
                            break
                        targets = [a[1]] + [a[k + 1] for k in range(2, len(a), 2)]
                        for t in targets[1:]:
                            o = self._fork(st, env, cur, t); o['taint'] = True; stack.append(o)
                        st['taint'] = True; nxt = targets[0]; break
                    if op == 253: end = 'ret'; break
                    if op in (252, 4416, 5380): end = 'kill'; break
                    if op == 254: raise Abort('return value in the entry point')
                    self._exec(env, op, a)
                if end: break
                if nxt is None: raise Abort('block without a terminator')
                prev, cur = cur, nxt
            w = None
            if end == 'ret' and 0 in outs:
                v = self.mem[outs[0]]
                if isinstance(v, list) and len(v) >= 4 and conc(v[3]) and isinstance(v[3], (float, np.floating)): w = SI.f2u(v[3])
            paths.append(dict(end=end, taint=st['taint'], terms=list(st['terms']), w=w))
            if len(paths) > max_paths: raise Abort('path limit')
        return paths

    def _fork(self, st, env, cur, target):
        o = dict(env=dict(env), mem=copy.deepcopy(self.mem), prev=cur, cur=target, assume=dict(st['assume']), terms=list(st['terms']),
                 taint=st['taint'])
        # pointers into the fork's memory keep their (root, path) form, so env can be shared shallowly; values are copied on load
        return o


def fs_facts(words):
    """kill / fragment built-in outputs / side effects (dm/gfx/nr/nr_mat.c nr_mat_scan, the same rules)"""
    kill = side = fragout = False
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        if op in (252, 4416, 5380): kill = True
        if op in (99,) or 227 <= op <= 242 or op in (4417, 5359, 6035): side = True
        if op == 71 and words[i + 2] == 11 and words[i + 3] in (22, 20, 5014): fragout = True
        if op == 72 and words[i + 3] == 11 and words[i + 4] in (22, 20, 5014): fragout = True
        i += n
    return kill, fragout, side


def gate_paths(paths):
    """the usable paths: OpReturn, no branch on an unknown value; terms as record tuples"""
    out = []
    for p in paths:
        if p['end'] != 'ret' or p['taint']: continue
        terms = []
        for (op, a, b), val in p['terms']:
            if isinstance(a, Sym): terms.append((a.bw, op, b, 1 | (2 if val else 0)))
            else: terms.append((b.bw, op, a, 0 | (2 if val else 0)))
        key = (tuple(terms), p['w'])
        if key not in [(tuple(x['terms']), x['w']) for x in out]: out.append(dict(terms=terms, w=p['w']))
    return out


def record(spirv, gp):
    nt = sum(len(p['terms']) for p in gp)
    b = bytearray(struct.pack('<IIQII', MAGIC, VERSION, fnv64(spirv), len(gp), nt))
    first = 0
    for p in gp:
        b += struct.pack('<IIII', p['w'] or 0, 1 if p['w'] is not None else 0, first, len(p['terms'])); first += len(p['terms'])
    for p in gp:
        for bw, op, c, fl in p['terms']: b += struct.pack('<IIII', bw, op, c & 0xffffffff, fl)
    return bytes(b)


def holds(p, word):
    """does gate path p hold for these uniform words (word(bank_word) -> int or None)"""
    for bw, op, c, fl in p['terms']:
        u = word(bw)
        if u is None: return False
        r = cmp_raw(op, u, c) if fl & 1 else cmp_raw(op, c, u)
        if r != bool(fl & 2): return False
    return True


def check(spirv, binding_of_slot, gp, n, seed):
    """seeded random runs on the concrete interpreter: when a gate path holds the shader must not kill and must write w (Location 0 .w)"""
    m = SI.Module(spirv); rng = random.Random(seed)
    words = sorted({t[0] for p in gp for t in p['terms']})
    consts = {bw: [t[2] for p in gp for t in p['terms'] if t[0] == bw] for bw in words}
    ubo_b = sorted({m.dec.get(v, {}).get(33, [None])[0] for v, (pt, sc) in m.vars.items() if sc == 2})
    tex_b = sorted({m.dec.get(v, {}).get(33, [None])[0] for v, (pt, sc) in m.vars.items() if sc == 0})
    ins = [m.dec[v][30][0] for v, (pt, sc) in m.vars.items() if sc == 1 and 30 in m.dec.get(v, {})]
    held = runs = 0; bad = []
    for r in range(n):
        ubos = {}
        for b in ubo_b:
            arr = np.array([rng.uniform(-4, 4) for _ in range(1024)], dtype=np.float32).tobytes() + b'\0' * (65536 - 4096)
            ubos[b] = bytearray(arr)
        for bw in words:
            b = binding_of_slot.get((bw >> 16) - 3)
            if b not in ubos: continue
            c = rng.choice(consts[bw]); cf = SI.u2f(c)
            pick = [c, c + 1, c - 1, 0, 0x3f800000, 0x7fc00000, 0x80000000, SI.f2u(F(rng.uniform(-3, 3))),
                    SI.f2u(np.nextafter(cf, F(np.inf), dtype=np.float32)), SI.f2u(np.nextafter(cf, F(-np.inf), dtype=np.float32))][r % 10 if r < 30 else rng.randrange(10)]
            struct.pack_into('<I', ubos[b], 4 * (bw & 0xffff), pick & 0xffffffff)
        tex = {b: SI.Texture(np.array([[[rng.uniform(0, 1) for _ in range(4)] for _ in range(4)] for _ in range(4)], dtype=np.float32)) for b in tex_b}
        inputs = {'FragCoord': (rng.uniform(0, 2160), rng.uniform(0, 972), rng.uniform(0, 1), 1.0)}
        for l in ins: inputs[l] = tuple(rng.uniform(-3, 3) for _ in range(4))
        try: out = m.run(inputs, ubos={k: bytes(v) for k, v in ubos.items()}, textures=tex)
        except NotImplementedError as e: return 'interpreter: %s' % e, 0, 0
        runs += 1
        word = lambda bw: (struct.unpack_from('<I', ubos[binding_of_slot[(bw >> 16) - 3]], 4 * (bw & 0xffff))[0]
                           if binding_of_slot.get((bw >> 16) - 3) in ubos else None)
        for p in gp:
            if not holds(p, word): continue
            held += 1
            if out is None: bad.append('run %d: gate path %r holds but the shader killed' % (r, p['terms']))
            elif p['w'] is not None and SI.f2u(out[0][3]) != p['w']:
                bad.append('run %d: gate path holds, .w %08x != %08x' % (r, SI.f2u(out[0][3]), p['w']))
            break
    return ('; '.join(bad[:3]) if bad else None), runs, held


def bundle_parts(b):
    _, _, stage, nb, ni, c1, sp, _ = struct.unpack_from('<8I', b, 0)
    binds = [struct.unpack_from('<8I', b, 32 + 32 * i) for i in range(nb)]
    o = 32 + nb * 32 + ni * 4 + c1
    return stage, binds, b[o:o + sp]


def pack_index(path):
    data = Path(path).read_bytes()
    magic, ver, n, _ = struct.unpack_from('<4I', data, 0)
    if magic != 0x50534758 or ver != 1: raise SystemExit('%s: not an xgspack v1' % path)
    idx = {}
    for i in range(n):
        k, o, size, _, crc = struct.unpack_from('<32sQIIQ', data, 16 + 56 * i)
        idx[k.rstrip(b'\0').decode('ascii', 'replace')] = (o, size, crc)
    return data, idx


def analyze_key(b, n_check, seed):
    """one fragment bundle -> (class, record bytes or None, row fields)"""
    stage, binds, spirv = bundle_parts(b)
    if stage != 4: return 'not-fs', None, {}
    w = struct.unpack('<%dI' % (len(spirv) // 4), spirv)
    kill, fragout, side = fs_facts(w)
    cls = 'kill' if kill else 'pure'
    if fragout or side: return cls + ('+fragout' if fragout else '') + ('+side' if side else ''), None, {}
    slot_of = {bd[0]: bd[5] for bd in binds if bd[1] == 6 and bd[4] == 2}      # uniform buffer (type 6, source 2): binding -> NVN slot
    binding_of = {bd[5]: bd[0] for bd in binds if bd[1] == 6 and bd[4] == 2}
    try: paths = Abs(spirv, slot_of).analyze()
    except Abort as e: return cls, None, dict(why=str(e))
    gp = gate_paths(paths)
    useful = [p for p in gp if (kill and True) or p['w'] is not None]
    info = dict(paths=len(paths), kills=sum(p['end'] == 'kill' for p in paths), taint=sum(p['taint'] for p in paths), gates=len(useful))
    if not useful: return cls, None, info
    if not kill and all(not p['terms'] for p in useful) and all(p['w'] is None for p in useful): return cls, None, info
    err, runs, held = check(spirv, binding_of, useful, n_check, seed) if n_check > 0 else (None, 0, 0)
    info.update(runs=runs, held=held, err=err)
    return cls, record(spirv, useful), info


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pack', default='', help='translated shader pack (dm/shaders/shaders.xgspack)')
    ap.add_argument('--bundles', default='', help='directory of translated <key>.xgs (instead of --pack)')
    ap.add_argument('--programs', default='', help='work/shaders-jobs/programs.tsv: the fragment keys of every *_DEP_NX material (default: every FS)')
    ap.add_argument('--keys', default='', help='comma-separated fragment keys (instead of --programs)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--n', type=int, default=24, help='random interpreter runs per record (0 = none)')
    a = ap.parse_args()
    if a.pack: data, idx = pack_index(a.pack); get = lambda k: data[idx[k][0]:idx[k][0] + idx[k][1]] if k in idx else None
    elif a.bundles: get = lambda k: (Path(a.bundles) / (k + '.xgs')).read_bytes() if (Path(a.bundles) / (k + '.xgs')).is_file() else None
    else: raise SystemExit('--pack or --bundles')
    names = collections.defaultdict(list)
    if a.keys: keys = a.keys.split(',')
    elif a.programs:
        for line in Path(a.programs).read_text(encoding='utf-8').splitlines()[1:]:
            f = line.split('\t')
            if len(f) >= 4 and f[0].endswith('_DEP_NX.bshd') and f[3] != '-': names[f[3]].append(f[0].rsplit('/', 1)[-1][:-5])
        keys = sorted(names)
    else: raise SystemExit('--programs or --keys')
    out = Path(a.out); stage = out / 'bundles'
    if stage.exists(): shutil.rmtree(stage)
    stage.mkdir(parents=True)
    rows = ['fs_key\tclass\trecord\tpaths\tkills\ttainted\tgate_paths\truns\theld\tcheck\tprograms']
    by_spv, fails, nrec, cls_n = {}, 0, 0, collections.Counter()
    for k in keys:
        b = get(k)
        if b is None: rows.append('%s\tmissing\t-\t\t\t\t\t\t\t\t' % k); continue
        h = hashlib.sha1(bundle_parts(b)[2]).digest()
        if h not in by_spv: by_spv[h] = analyze_key(b, a.n, int(k[:8], 16))
        cls, rec, info = by_spv[h]
        if rec:   # the same SPIR-V under another key: the record is the same bytes (it hashes the SPIR-V, not the key)
            (stage / ('dg.%s.xgs' % k)).write_bytes(rec); nrec += 1
            if info.get('err'): fails += 1
        cls_n[cls + (' +record' if rec else '')] += 1
        rows.append('%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' % (k, cls, len(rec) if rec else '-', info.get('paths', ''), info.get('kills', ''),
                    info.get('taint', ''), info.get('gates', ''), info.get('runs', ''), info.get('held', ''),
                    'FAILED ' + info['err'] if info.get('err') else info.get('why', 'ok' if rec else ''), ','.join(names.get(k, [])[:4])))
    import xgs_pack
    pack = out / 'native-nr33b.xgspack'
    if nrec: xgs_pack.build(str(stage), str(pack))
    elif pack.exists(): pack.unlink()
    shutil.rmtree(stage)
    (out / 'nr_mat.tsv').write_text('\n'.join(rows) + '\n', encoding='utf-8')
    rep = ['fragment keys %d (distinct SPIR-V %d), gate records %d, failed checks %d' % (len(keys), len(by_spv), nrec, fails)]
    rep += ['  %-24s %d' % (c, n) for c, n in sorted(cls_n.items())]
    (out / 'report.txt').write_text('\n'.join(rep) + '\n', encoding='utf-8')
    print('\n'.join(rep))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
