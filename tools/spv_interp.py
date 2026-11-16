"""Tiny scalar SPIR-V fragment-shader interpreter (one invocation at a time), for offline equivalence tests of native bundles.

Covers the subset that the DOS2 translated post shaders (after spirv-opt -O) and glslc -O output use: Logical addressing,
Function / Private / Input / Output / Uniform / UniformConstant / PushConstant storage, structured control flow incl. loops,
OpPhi, float32 arithmetic (numpy float32, IEEE single), 32-bit ints (stored unsigned, sign-interpreted per op), GLSL.std.450
subset, OpImageFetch / OpImageSample*Lod / OpImageGather on CPU textures (bilinear or nearest, clamp-to-edge; 2D and CubeTexture;
an explicit lod / bias != 0 scales the result by 1 + lod / 8, so a wrong lod is a mismatch; Texture3D: trilinear, lod 0, for the
3D LUT of PPApplyLUT), OpKill.
Uniform blocks are read from raw bytes with the module's Offset / ArrayStride / MatrixStride (row-major honoured) decorations.

After run(): m.executed = instructions executed (merges / OpLine not counted), m.tex_ops = texture instructions executed.
Usage:  m = Module(spirv_bytes); out = m.run(inputs={'FragCoord': (x, y, z, w), 1: (..)}, ubos={binding: bytes},
                                            textures={binding: Texture(...)}, push=bytes)  ->  {location: [floats]} or None (discard)
"""
import math, struct
import numpy as np

F = np.float32
M32 = 0xffffffff


def f2u(f): return struct.unpack('<I', struct.pack('<f', float(f)))[0]
def u2f(u): return F(struct.unpack('<f', struct.pack('<I', u & M32))[0])
def s32(u): u &= M32; return u - (1 << 32) if u & 0x80000000 else u


class Texture:
    """data: numpy float32 array [h, w, 4]; filt 'linear' / 'nearest' for sampling ops; fetch is always exact"""
    def __init__(self, data, filt='linear'):
        self.d = np.asarray(data, dtype=np.float32); self.h, self.w = self.d.shape[:2]; self.filt = filt

    def fetch(self, x, y):
        if not (0 <= x < self.w and 0 <= y < self.h): return [F(0)] * 4          # robust access: out of bounds = 0
        return [F(v) for v in self.d[y, x]]

    def _c(self, x, y): return self.d[min(max(y, 0), self.h - 1), min(max(x, 0), self.w - 1)]

    def sample(self, u, v):
        x, y = float(u) * self.w - 0.5, float(v) * self.h - 0.5
        if self.filt == 'nearest':
            return [F(c) for c in self._c(int(math.floor(x + 0.5)), int(math.floor(y + 0.5)))]
        x0, y0 = math.floor(x), math.floor(y); fx, fy = x - x0, y - y0
        a, b, c, d = self._c(x0, y0), self._c(x0 + 1, y0), self._c(x0, y0 + 1), self._c(x0 + 1, y0 + 1)
        with np.errstate(all='ignore'): r = (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy
        return [F(c) for c in r]

    def sample_c(self, coord, lod=0.0):
        """coordinate list (2D: u, v; 3 components: cube direction) + explicit lod / bias. A non-zero lod scales the bilinear
        result by (1 + lod / 8) so a wrong lod shows up as a mismatch (no real mip chain)."""
        if not all(math.isfinite(float(c)) for c in coord): coord = [F(0) if not math.isfinite(float(c)) else c for c in coord]
        if len(coord) >= 3 and self.cube is not None:
            r = self.cube.face_sample(coord)
        else:
            r = self.sample(coord[0], coord[1])
        if lod != 0:
            k = F(F(1) + F(F(lod) * F(0.125)))
            with np.errstate(all='ignore'): r = [F(c * k) for c in r]
        return r

    cube = None

    def gather(self, u, v, comp):
        # the footprint is chosen in fixed point like a GPU does (8 sub-texel bits): a coordinate at an exact texel centre (FXAA's
        # gathers, NB-18b-8) picks that texel and its +1 neighbours whatever the float rounding of u * w was
        x, y = float(u) * self.w - 0.5, float(v) * self.h - 0.5
        x0, y0 = math.floor(round(x * 256) / 256), math.floor(round(y * 256) / 256)
        return [F(self._c(x0, y0 + 1)[comp]), F(self._c(x0 + 1, y0 + 1)[comp]), F(self._c(x0 + 1, y0)[comp]), F(self._c(x0, y0)[comp])]


class Texture3D:
    """data: numpy float32 array [d, h, w, 4]; trilinear (or nearest) sampling, clamp-to-edge (NB-18b-3: the 3D LUT of PPApplyLUT)"""
    def __init__(self, data, filt='linear'):
        self.d = np.asarray(data, dtype=np.float32); self.dd, self.h, self.w = self.d.shape[:3]; self.filt = filt

    def fetch(self, x, y, z=0):
        if not (0 <= x < self.w and 0 <= y < self.h and 0 <= z < self.dd): return [F(0)] * 4
        return [F(v) for v in self.d[z, y, x]]

    def _c(self, x, y, z):
        return self.d[min(max(z, 0), self.dd - 1), min(max(y, 0), self.h - 1), min(max(x, 0), self.w - 1)]

    def sample(self, u, v, w=0.0):
        x, y, z = float(u) * self.w - 0.5, float(v) * self.h - 0.5, float(w) * self.dd - 0.5
        if self.filt == 'nearest':
            return [F(c) for c in self._c(int(math.floor(x + 0.5)), int(math.floor(y + 0.5)), int(math.floor(z + 0.5)))]
        x0, y0, z0 = math.floor(x), math.floor(y), math.floor(z); fx, fy, fz = x - x0, y - y0, z - z0
        with np.errstate(all='ignore'):
            def pl(zz):
                a, b, c, d = self._c(x0, y0, zz), self._c(x0 + 1, y0, zz), self._c(x0, y0 + 1, zz), self._c(x0 + 1, y0 + 1, zz)
                return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy
            r = pl(z0) * (1 - fz) + pl(z0 + 1) * fz
        return [F(c) for c in r]


class CubeTexture(Texture):
    """six square faces [6, n, n, 4]; sampled by the major axis of the direction (Vulkan face order +x -x +y -y +z -z)"""
    def __init__(self, faces, filt='linear'):
        self.faces = [Texture(f, filt) for f in np.asarray(faces, dtype=np.float32)]
        Texture.__init__(self, self.faces[0].d, filt); self.cube = self

    def face_sample(self, c):
        x, y, z = (float(v) for v in c[:3]); ax, ay, az = abs(x), abs(y), abs(z)
        if ax >= ay and ax >= az: f, sc, tc, ma = (0, -z, -y, ax) if x >= 0 else (1, z, -y, ax)
        elif ay >= az: f, sc, tc, ma = (2, x, z, ay) if y >= 0 else (3, x, -z, ay)
        else: f, sc, tc, ma = (4, x, -y, az) if z >= 0 else (5, -x, -y, az)
        if ma == 0: return self.faces[f].sample(0.5, 0.5)
        return self.faces[f].sample(0.5 * (sc / ma + 1), 0.5 * (tc / ma + 1))


class Discard(Exception): pass


def _str(words):
    b = b''.join(struct.pack('<I', x) for x in words); return b[:b.index(b'\0')].decode()


class Module:
    def __init__(self, data):
        w = struct.unpack('<%dI' % (len(data) // 4), data)
        assert w[0] == 0x07230203
        self.types, self.consts, self.vars, self.dec, self.mdec, self.funcs = {}, {}, {}, {}, {}, {}
        self.ext = {}
        i, fn, blk = 5, None, None
        while i < len(w):
            n, op = w[i] >> 16, w[i] & 0xffff
            a = list(w[i + 1:i + n]); i += n
            if op == 11: self.ext[a[0]] = _str(a[1:])
            elif op == 15: self.entry = a[1]
            elif op == 71: self.dec.setdefault(a[0], {})[a[1]] = a[2:]
            elif op == 72: self.mdec.setdefault((a[0], a[1]), {})[a[2]] = a[3:]
            elif 19 <= op <= 39 or op == 322: self.types[a[0]] = (op, a[1:])
            elif op == 43: self.consts[a[1]] = self._lit(a[0], a[2:])
            elif op in (41, 48): self.consts[a[1]] = True
            elif op in (42, 49): self.consts[a[1]] = False
            elif op in (44, 51): self.consts[a[1]] = [self.consts[c] for c in a[2:]]
            elif op in (46, 1): self.consts[a[1]] = self._null(a[0])                      # OpConstantNull, OpUndef (global)
            elif op == 50: self.consts[a[1]] = self._lit(a[0], a[2:])                        # OpSpecConstant (default)
            elif op == 59 and fn is None: self.vars[a[1]] = (a[0], a[2])
            elif op == 54: fn = {'id': a[1], 'params': [], 'blocks': {}, 'order': []}; self.funcs[a[1]] = fn
            elif op == 55: fn['params'].append(a[1])
            elif op == 56: fn = None
            elif op == 248: blk = a[0]; fn['blocks'][blk] = []; fn['order'].append(blk)
            elif fn is not None and blk is not None: fn['blocks'][blk].append((op, a))

    # ---------------------------------------------------------------- types
    def _lit(self, t, words):
        op, a = self.types[t]
        if op == 22: return u2f(words[0])
        if op == 21: return words[0] & M32
        raise ValueError('literal of type %d' % op)

    def _null(self, t):
        op, a = self.types[t]
        if op == 22: return F(0)
        if op == 21: return 0
        if op == 20: return False
        if op in (23, 24): return [self._null(a[0]) for _ in range(a[1])]
        if op == 28: return [self._null(a[0]) for _ in range(self.consts[a[1]])]
        if op == 30: return [self._null(m) for m in a]
        return None

    def _is_float(self, t):
        op, a = self.types[t]
        while op in (23, 24): op, a = self.types[a[0]]
        return op == 22

    def _signed(self, t):
        op, a = self.types[t]
        while op == 23: op, a = self.types[a[0]]
        return op == 21 and a[1] == 1

    # ---------------------------------------------------------------- uniform memory
    def _read(self, buf, off, t, rowmajor=False, mstride=None):
        op, a = self.types[t]
        if op == 22: return u2f(struct.unpack_from('<I', buf, off)[0]) if off + 4 <= len(buf) else F(0)
        if op == 21: return struct.unpack_from('<I', buf, off)[0] if off + 4 <= len(buf) else 0
        if op == 20: return (struct.unpack_from('<I', buf, off)[0] if off + 4 <= len(buf) else 0) != 0
        if op == 23: return [self._read(buf, off + 4 * k, a[0]) for k in range(a[1])]
        if op == 24:
            ct = self.types[a[0]][1]; rows = ct[1]
            if rowmajor: return [[self._read(buf, off + r * mstride + 4 * c, ct[0]) for r in range(rows)] for c in range(a[1])]
            return [self._read(buf, off + c * mstride, a[0]) for c in range(a[1])]
        if op in (28, 29):
            st = self.dec[t][6][0]; n = self.consts[a[1]] if op == 28 else 0
            return [self._read(buf, off + k * st, a[0]) for k in range(n)]
        if op == 30:
            out = []
            for m, mt in enumerate(a):
                d = self.mdec.get((t, m), {})
                out.append(self._read(buf, off + d[35][0], mt, 4 in d, d.get(7, [16])[0]))
            return out
        raise ValueError('uniform read of type %d' % op)

    def _uload(self, var, path):
        buf = self.ubufs[var]; t = self.types[self.vars[var][0]][1][1]; off = 0; rowmajor, mstride = False, 16
        for idx in path:
            op, a = self.types[t]
            if op == 30:
                d = self.mdec.get((t, idx), {}); off += d[35][0]; rowmajor = 4 in d; mstride = d.get(7, [16])[0]; t = a[idx]
            elif op in (28, 29): off += idx * self.dec[t][6][0]; t = a[0]
            elif op == 24:
                if rowmajor: raise NotImplementedError('row-major column pointer')
                off += idx * mstride; t = a[0]
            elif op == 23: off += 4 * idx; t = a[0]
        return self._read(buf, off, t, rowmajor, mstride)

    # ---------------------------------------------------------------- run
    def run(self, inputs, ubos=None, textures=None, push=b''):
        """inputs: {'FragCoord': vec4, location: vec4}; ubos: {binding: bytes}; textures: {binding: Texture}"""
        self.ubufs, self.mem, self.tex = {}, {}, {}
        self.executed, self.tex_ops = 0, 0
        outs = {}
        for v, (pt, sc) in self.vars.items():
            t = self.types[pt][1][1]; d = self.dec.get(v, {})
            if sc in (2, 12): self.ubufs[v] = (ubos or {}).get(d.get(33, [None])[0], b'')
            elif sc == 9: self.ubufs[v] = push
            elif sc == 0: self.tex[v] = (textures or {}).get(d.get(33, [None])[0])
            elif sc == 1:
                if 11 in d:
                    bi = d[11][0]
                    self.mem[v] = [F(x) for x in inputs['FragCoord']] if bi == 15 else self._null(t)
                else:
                    loc = d[30][0]; val = inputs.get(loc)
                    self.mem[v] = [F(x) for x in val][:self.types[t][1][1]] if val is not None else self._null(t)
            elif sc in (3, 6, 4): self.mem[v] = self._null(t)
            if sc == 3: outs[d.get(30, [0])[0]] = v
        try:
            self._call(self.entry, [])
        except Discard:
            return None
        return {loc: self.mem[v] for loc, v in outs.items()}

    def _get(self, env, i):
        if i in env: return env[i]
        if i in self.consts: return self.consts[i]
        if i in self.vars: return ('ptr', i, ())
        raise KeyError('%%%d' % i)

    def _call(self, fid, args):
        fn = self.funcs[fid]; env = dict(zip(fn['params'], args)); prev, cur = None, fn['order'][0]
        steps = 0
        while True:
            nxt = None
            for op, a in fn['blocks'][cur]:
                steps += 1
                if steps > 2000000: raise RuntimeError('runaway')
                if op == 245:                                                                        # OpPhi
                    for k in range(2, len(a), 2):
                        if a[k + 1] == prev: env[a[1]] = self._get(env, a[k]); break
                    continue
                if op in (246, 247, 8, 317): continue                                              # merges, OpLine, OpNoLine
                self.executed += 1
                if op == 249: nxt = a[0]; break                                                      # OpBranch
                if op == 250: nxt = a[1] if self._get(env, a[0]) else a[2]; break                    # OpBranchConditional
                if op == 251:                                                                        # OpSwitch
                    sel = self._get(env, a[0]); nxt = a[1]
                    for k in range(2, len(a), 2):
                        if a[k] == sel: nxt = a[k + 1]; break
                    break
                if op == 253: return None                                                            # OpReturn
                if op == 254: return self._get(env, a[0])                                            # OpReturnValue
                if op in (252, 4416): raise Discard()                                                # OpKill / OpTerminateInvocation
                self._exec(env, op, a)
            prev, cur = cur, nxt

    def _exec(self, env, op, a):
        g = lambda i: self._get(env, i)
        if op == 59: env[a[1]] = ('ptr', ('fn', a[1]), ()); self.mem[('fn', a[1])] = self._null(self.types[a[0]][1][1]) if len(a) < 4 else g(a[3]); return
        if op == 61:                                                                                 # OpLoad
            p = g(a[2]); env[a[1]] = self._load(p); return
        if op == 62: self._store(g(a[0]), g(a[1])); return                                         # OpStore
        if op in (65, 66):                                                                           # OpAccessChain
            p = g(a[2]); env[a[1]] = ('ptr', p[1], p[2] + tuple(self._idx(g(x)) for x in a[3:])); return
        if op == 57: env[a[1]] = self._call(a[2], [g(x) for x in a[3:]]); return                    # OpFunctionCall
        if op == 1: env[a[1]] = self._null(a[0]); return
        rt, r = a[0], a[1]
        if op == 80: env[r] = self._construct(rt, [g(x) for x in a[2:]]); return
        if op == 81:
            v = g(a[2])
            for k in a[3:]: v = v[k]
            env[r] = v; return
        if op == 82:
            obj = self._copy(g(a[3])); c = obj
            for k in a[4:-1]: c = c[k]
            c[a[-1]] = g(a[2]); env[r] = obj; return
        if op == 79:
            v1, v2 = g(a[2]), g(a[3]); both = list(v1) + list(v2)
            env[r] = [both[k] if k != 0xffffffff else F(0) for k in a[4:]]; return
        if op == 77: env[r] = g(a[2])[self._idx(g(a[3]))]; return
        if op == 78:
            v = list(g(a[2])); v[self._idx(g(a[4]))] = g(a[3]); env[r] = v; return
        if op == 83: env[r] = self._copy(g(a[2])); return                                           # OpCopyObject
        if op == 100: env[r] = g(a[2]); return                                                       # OpImage
        if op == 86: env[r] = g(a[2]); return                                                        # OpSampledImage
        if op in (103, 104):                                                                         # OpImageQuerySizeLod / OpImageQuerySize
            t = g(a[2]); env[r] = [t.w & M32, t.h & M32] if t is not None else [0, 0]; return
        if op == 12: env[r] = self._ext(rt, a[3], [g(x) for x in a[4:]]); return
        if op in (95, 87, 88, 96):
            env[r] = self._image(op, rt, a, g); return
        if op == 169:                                                                                # OpSelect
            c, x, y = g(a[2]), g(a[3]), g(a[4])
            env[r] = [xx if cc else yy for cc, xx, yy in zip(c, x, y)] if isinstance(c, list) else (x if c else y); return
        if op == 148:                                                                                # OpDot
            x, y = g(a[2]), g(a[3]); s = F(0)
            for p, q in zip(x, y): s = F(s + F(p * q))
            env[r] = s; return
        if op == 154: env[r] = any(g(a[2])); return                                                  # OpAny
        if op == 155: env[r] = all(g(a[2])); return                                                  # OpAll
        if op == 142: env[r] = [F(c * g(a[3])) for c in g(a[2])]; return                            # OpVectorTimesScalar
        if op == 144:                                                                                # OpMatrixTimesVector
            m, v = g(a[2]), g(a[3]); n = len(m[0]); out = []
            for row in range(n):
                s = F(0)
                for col in range(len(m)): s = F(s + F(m[col][row] * v[col]))
                out.append(s)
            env[r] = out; return
        args = [g(x) for x in a[2:]]
        env[r] = self._map(op, rt, args)

    def _idx(self, v): return s32(v) if isinstance(v, int) else int(v)

    def _copy(self, v): return [self._copy(x) for x in v] if isinstance(v, list) else v

    def _construct(self, rt, parts):
        op, a = self.types[rt]
        if op == 23:
            out = []
            for p in parts: out.extend(p if isinstance(p, list) else [p])
            return out
        return [self._copy(p) for p in parts]

    def _load(self, p):
        _, root, path = p
        if root in self.ubufs: return self._uload(root, path)
        if root in self.tex: return self.tex[root]
        v = self.mem[root]
        for k in path: v = v[k]
        return self._copy(v)

    def _store(self, p, val):
        _, root, path = p
        if not path: self.mem[root] = self._copy(val); return
        v = self.mem[root]
        for k in path[:-1]: v = v[k]
        v[path[-1]] = self._copy(val)

    def _image(self, op, rt, a, g):
        tex = g(a[2]); coord = g(a[3]); self.tex_ops += 1
        if tex is None: return [F(0)] * 4
        if op == 95:                                                                                 # OpImageFetch
            c = [s32(x) for x in coord]; return tex.fetch(*c[:3])
        mi = 5 if op == 96 else 4; lod = F(0); off = None                                           # image operands (NB-18b-8: offsets)
        if len(a) > mi:
            mask = a[mi]; k = mi + 1
            if mask & 1: lod = g(a[k]); k += 1                                                       # Bias
            if mask & 2: lod = g(a[k]); k += 1                                                       # Lod
            if mask & 4: k += 2                                                                      # Grad
            if mask & 0x18: off = [s32(x) for x in g(a[k])]                                         # ConstOffset / Offset (texels)
        if off and op != 95 and not isinstance(tex, Texture3D):
            coord = [F(F(coord[0]) + F(off[0]) / F(tex.w)), F(F(coord[1]) + F(off[1]) / F(tex.h))] + list(coord[2:])
        if op == 96:                                                                                 # OpImageGather
            return tex.gather(coord[0], coord[1], s32(g(a[4])))
        if isinstance(tex, Texture3D): return tex.sample(coord[0], coord[1], coord[2])   # NB-18b-3 3D LUT, lod 0
        return tex.sample_c(coord if isinstance(coord, list) else [coord], lod)

    def _map(self, op, rt, args):
        vec = isinstance(args[0], list)
        if vec:
            n = len(args[0])
            return [self._scalar(op, rt, [x[k] if isinstance(x, list) else x for x in args]) for k in range(n)]
        return self._scalar(op, rt, args)

    def _scalar(self, op, rt, x):
        with np.errstate(all='ignore'):
            if op == 129: return F(x[0] + x[1])                          # FAdd
            if op == 131: return F(x[0] - x[1])                          # FSub
            if op == 133: return F(x[0] * x[1])                          # FMul
            if op == 136: return F(x[0] / x[1])                          # FDiv
            if op == 127: return F(-x[0])                                # FNegate
            if op == 141:                                                # FMod (sign of y)
                a, b = float(x[0]), float(x[1])
                return F(a - b * math.floor(a / b)) if b != 0 and math.isfinite(a) else F(float('nan'))
            if op == 140: return F(math.fmod(float(x[0]), float(x[1]))) if x[1] != 0 else F(float('nan'))  # FRem
            if op == 128: return (x[0] + x[1]) & M32                     # IAdd
            if op == 130: return (x[0] - x[1]) & M32
            if op == 132: return (x[0] * x[1]) & M32
            if op == 126: return (-x[0]) & M32                           # SNegate
            if op == 135: return (s32(x[0]) // s32(x[1]) if s32(x[0]) * s32(x[1]) >= 0 else -(abs(s32(x[0])) // abs(s32(x[1])))) & M32
            if op == 134: return (x[0] // x[1]) & M32
            if op == 199: return x[0] & x[1]
            if op == 197: return x[0] | x[1]
            if op == 198: return x[0] ^ x[1]
            if op == 200: return (~x[0]) & M32
            if op == 196: return (x[0] << (x[1] & 31)) & M32
            if op == 194: return x[0] >> (x[1] & 31)
            if op == 195: return (s32(x[0]) >> (x[1] & 31)) & M32
            if op == 124:                                                # Bitcast
                if self._is_float(rt): return u2f(x[0]) if isinstance(x[0], int) else x[0]
                return f2u(x[0]) if not isinstance(x[0], int) else x[0]
            if op == 109: return self._ftoi(x[0], False)                 # ConvertFToU
            if op == 110: return self._ftoi(x[0], True)                  # ConvertFToS
            if op == 111: return F(s32(x[0]))                            # ConvertSToF
            if op == 112: return F(x[0] & M32)                           # ConvertUToF
            if op == 113: return x[0]                                    # UConvert (32-bit only)
            if op == 114: return x[0]
            if op == 115: return F(x[0])
            if op == 156: return bool(np.isnan(x[0]))
            if op == 157: return bool(np.isinf(x[0]))
            fo = {180: lambda p, q: p == q, 182: lambda p, q: p != q, 184: lambda p, q: p < q, 186: lambda p, q: p > q,
                  188: lambda p, q: p <= q, 190: lambda p, q: p >= q}
            if op in fo: return bool(fo[op](x[0], x[1])) and not (np.isnan(x[0]) or np.isnan(x[1]))
            fu = {181: lambda p, q: p == q, 183: lambda p, q: p != q, 185: lambda p, q: p < q, 187: lambda p, q: p > q,
                  189: lambda p, q: p <= q, 191: lambda p, q: p >= q}
            if op in fu: return bool(fu[op](x[0], x[1])) or bool(np.isnan(x[0]) or np.isnan(x[1]))
            if op == 170: return x[0] == x[1]
            if op == 171: return x[0] != x[1]
            if op == 172: return x[0] > x[1]
            if op == 173: return s32(x[0]) > s32(x[1])
            if op == 174: return x[0] >= x[1]
            if op == 175: return s32(x[0]) >= s32(x[1])
            if op == 176: return x[0] < x[1]
            if op == 177: return s32(x[0]) < s32(x[1])
            if op == 178: return x[0] <= x[1]
            if op == 179: return s32(x[0]) <= s32(x[1])
            if op == 164: return x[0] == x[1]                            # LogicalEqual
            if op == 165: return x[0] != x[1]
            if op == 166: return x[0] or x[1]
            if op == 167: return x[0] and x[1]
            if op == 168: return not x[0]
        raise NotImplementedError('opcode %d' % op)

    def _ftoi(self, f, signed):
        f = float(f)
        if math.isnan(f): return 0
        t = math.trunc(f) if math.isfinite(f) else (1 << 40 if f > 0 else -(1 << 40))
        if signed: t = max(-(1 << 31), min((1 << 31) - 1, t))
        else: t = max(0, min(M32, t))
        return t & M32

    def _ext(self, rt, inst, x):
        vec = isinstance(x[0], list)
        if inst == 66:                                                        # Length
            s = F(0)
            for c in x[0]: s = F(s + F(c * c))
            return F(np.sqrt(s))
        if inst == 69:                                                        # Normalize
            s = F(0)
            for c in x[0]: s = F(s + F(c * c))
            inv = F(F(1) / F(np.sqrt(s))); return [F(c * inv) for c in x[0]]
        if vec:
            n = len(x[0])
            return [self._ext1(rt, inst, [v[k] if isinstance(v, list) else v for v in x]) for k in range(n)]
        return self._ext1(rt, inst, x)

    def _ext1(self, rt, inst, x):
        with np.errstate(all='ignore'):
            if inst == 3: return F(np.trunc(x[0]))
            if inst == 1: return F(np.rint(x[0]))
            if inst == 2: return F(np.rint(x[0]))
            if inst == 8: return F(np.floor(x[0]))
            if inst == 9: return F(np.ceil(x[0]))
            if inst == 10: return F(x[0] - np.floor(x[0]))
            if inst == 4: return F(abs(x[0]))
            if inst == 5: return (abs(s32(x[0]))) & M32
            if inst == 6: return F(np.sign(x[0]))
            if inst in (37, 79):                                              # FMin / NMin
                a, b = x
                if np.isnan(a): return b
                if np.isnan(b): return a
                return F(min(a, b))
            if inst in (40, 80):
                a, b = x
                if np.isnan(a): return b
                if np.isnan(b): return a
                return F(max(a, b))
            if inst == 38: return min(x[0], x[1])
            if inst == 39: return x[0] if s32(x[0]) < s32(x[1]) else x[1]
            if inst == 41: return max(x[0], x[1])
            if inst == 42: return x[0] if s32(x[0]) > s32(x[1]) else x[1]
            if inst in (43, 81):                                              # FClamp / NClamp
                v = x[0]
                if np.isnan(v): return x[1] if inst == 81 else v
                return F(min(max(v, x[1]), x[2]))
            if inst == 44: return min(max(x[0], x[1]), x[2])
            if inst == 45:
                v = s32(x[0]); return (min(max(v, s32(x[1])), s32(x[2]))) & M32
            if inst == 46: return F(x[0] * (F(1) - x[2]) + x[1] * x[2])     # FMix
            if inst == 48: return F(0) if x[1] < x[0] else F(1)              # Step
            if inst == 49:
                t = F(min(max(F((x[2] - x[0]) / (x[1] - x[0])), 0), 1)); return F(t * t * F(F(3) - F(2) * t))
            if inst == 50: return F(np.float64(x[0]) * np.float64(x[1]) + np.float64(x[2]))   # Fma
            if inst == 26: return F(np.power(x[0], x[1]))
            if inst == 27: return F(np.exp(x[0]))
            if inst == 28: return F(np.log(x[0]))
            if inst == 29: return F(np.exp2(x[0]))
            if inst == 30: return F(np.log2(x[0]))
            if inst == 31: return F(np.sqrt(x[0]))
            if inst == 32: return F(F(1) / F(np.sqrt(x[0])))
            if inst == 13: return F(np.sin(x[0]))
            if inst == 14: return F(np.cos(x[0]))
        raise NotImplementedError('GLSL.std.450 %d' % inst)
