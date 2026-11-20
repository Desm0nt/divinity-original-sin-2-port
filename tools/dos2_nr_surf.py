"""DOS2 NR-33C (DM_NR_SH_SURF): the surface materials' fragment shaders with a wave-coherent early out, built on the player's machine
-> <shaders>/native-nr33c.xgspack (setup stage 8). Nothing of the game is in git / the APK: this tool transforms the player's own
translated SPIR-V (dm/shaders/shaders.xgspack) by structure, the host (dm/gfx/vulkan/engine/xbg_nr_surf.inc) only reads the result.

Why: DOS2's surfaces (puddles of water / blood / oil / poison / fire, cloud shadows, rain) are box / grid meshes over the ground. Their
translated fragment shaders kill the fragments outside the box / facing away / outside the surface cells in the first ~10 % of the code,
then run 600-1400 instructions with 12-22 texture reads (and a point-light loop for FlowingWater). A kill does not stop a wave on Turnip
(ir3_legalize.c kill_sched: "kill doesn't actually kill the thread", the lanes go on as helpers; only loops get a jump to the end), nor
on Mali / PowerVR unless the whole quad / warp is dead and the compiler places the test early: every killed lane still pays the rest.
The forward (alpha-blended) ones also run the whole lighting for fragments whose alpha is exactly 0 (FClamp / FMax(0, ..) of the cell
coverage, the depth fade): blended with SRC_ALPHA / ONE_MINUS_SRC_ALPHA they leave the target as it was.

What (all structural, on the spirv-opt -O form of the player's module; no constant, operation or order of the shader's own math changes):
  at a point P of the top-level code (the merge block of a top-level "if (c) kill", or after the alpha of colour 0 is known)
      live = !(every top-level kill condition before P) [&& !(alpha dead)]
      if (subgroupAny(live)) { the rest of the shader, unchanged } else { kill }
  The branch is uniform in the subgroup, so derivatives / implicit-LOD reads inside it see the same quads as before; a wave only skips
  the rest when none of its lanes can write anything. Live lanes run exactly the original instructions. Variants (pack keys):
    k.<fs key>  exact, no gate: kill points only (every lane it skips was killed already)
    a.<fs key>  exact for a blended draw: + alpha == +-0 is dead (the host takes it only when colour 0 blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA,
                ADD, dst alpha factor ONE or ONE_MINUS_SRC_ALPHA, depth / stencil writes off, no alpha to coverage, no logic op); differs from
                the game only where the game would blend inf / NaN colour with alpha 0 (NaN there, the old pixel here)
    m.<fs key>  "mobile": + 0 <= alpha < 1/255 is dead (same gate): an edge with alpha below one 8-bit step is not drawn - owner-visible
    p.<fs key>  poison: as m, but a wave it would skip draws magenta where alpha is dead (shows where the early out fires)
  Proof (every bundle, seeded): spirv-val; tools/spv_interp.py runs translated and variant on the same random inputs twice - subgroupAny =
  this lane (a lone wave) and subgroupAny = true (a live neighbour): with a live neighbour every output must equal the translated one bit
  for bit (a kill too); alone, the variant may only skip a lane the translated one kills (k) or leaves with alpha dead (a / m).
  Record "sr.<fs key>": XSR1, FNV-1a 64 of the translated SPIR-V, flags (variants), counts (dumped by --list).

  dos2_nr_surf.py --pack dm/shaders/shaders.xgspack --programs work/shaders-jobs/programs.tsv --out DIR [--n 48] [--malioc EXE]
Output: DIR/native-nr33c.xgspack, DIR/nr_surf.tsv, DIR/proof.txt. Exit 1 on any failed check.
"""
import argparse, hashlib, random, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'dm' / 'gfx' / 'tools'))
import spv_interp as SI  # noqa: E402

# The programs (name in the game's packages, Materials:Public/Game/Assets/Materials/...): the surface system + the FX the owner's
# Fort Joy log ranks (NR-HEAT, play-1118). Keys come from programs.tsv of the player's own packages, never from this list.
SURF_DIR = 'Materials:Public/Game/Assets/Materials/Surfaces/'
FX_NAMES = ['RS3_FX_Surface_Fire_Ground_01_ST_FOR_NX', 'RS3_FX_OverlayMaterial_Rain_01_ST_FOR_NX', 'NAT_Water_Ocean_infinite_A_PBR_ST_FOR_NX',
            'RS3_FX_Particles_Emissive_02_ST_FOR_NX']
T_MOBILE = 1.0 / 255.0
SMALL = [0.0, 1.0 / 255, 2.0 / 255, 3.0 / 255, 0.25, 0.5, 1.0, 2.0, 3.0]
MIN_REST = 0.30      # a point is kept when at least this share of the shader's instructions follows it
MIN_STEP = 0.12      #   and it skips this much more than the previous point
MAX_POINTS = 3

NORES = {0, 2, 3, 4, 5, 6, 8, 10, 14, 15, 16, 17, 39, 56, 62, 63, 71, 72, 74, 75, 99, 218, 219, 224, 225, 246, 247, 249, 250, 251, 252, 253,
         254, 255, 256, 257, 317, 330, 331, 332, 4416, 5380, 5632, 5633}
IDONLY = {7, 11, 73, 248} | set(range(19, 39))
TEX = {87, 88, 89, 90, 95, 96, 97}
PURE_LOADS = {1, 2, 0}   # storage classes UniformConstant 0, Input 1, Uniform 2: loads of these may move


class Mod:
    """a SPIR-V module as (header, [(op, operands)]); one function expected (spirv-opt -O inlines)"""
    def __init__(self, b):
        w = struct.unpack('<%dI' % (len(b) // 4), b)
        assert w[0] == 0x07230203, 'not SPIR-V'
        self.hdr = list(w[:5]); self.ins = []; i = 5
        while i < len(w):
            n = w[i] >> 16; op = w[i] & 0xffff
            if not n: raise ValueError('bad word count')
            self.ins.append((op, list(w[i + 1:i + n]))); i += n

    def words(self):
        out = list(self.hdr)
        for op, a in self.ins: out += [((len(a) + 1) << 16) | op] + a
        return struct.pack('<%dI' % len(out), *out)

    def new_id(self):
        self.hdr[3] += 1; return self.hdr[3] - 1


def res(op, a):
    if op in NORES: return None
    if op in IDONLY: return a[0]
    return a[1] if len(a) > 1 else None


def uses(op, a):
    """ids an instruction reads (conservative: a literal equal to an id counts too, which only makes slices bigger)"""
    if op in NORES:
        if op == 250: return [a[0]]
        if op == 251: return [a[0]]
        if op in (62, 63): return a[:2]
        return []
    if op in IDONLY: return []
    r = a[2:]
    if op == 12: return a[4:]                       # OpExtInst: set, literal instruction
    if op == 81: return a[2:3]                      # OpCompositeExtract: indices are literals
    if op == 82: return a[2:4]                      # OpCompositeInsert
    if op == 79: return a[2:4]                      # OpVectorShuffle
    if op == 245: return a[2::2]                    # OpPhi values (parents are labels)
    return r


class Fn:
    def __init__(self, m):
        self.m = m
        f0 = next(k for k, (op, a) in enumerate(m.ins) if op == 54)
        if sum(1 for op, a in m.ins if op == 54) != 1: raise ValueError('more than one function')
        self.f0 = f0
        self.head = m.ins[f0]
        self.blocks, self.order, cur = {}, [], None
        for op, a in m.ins[f0 + 1:]:
            if op == 56: break
            if op == 248: cur = a[0]; self.blocks[cur] = []; self.order.append(cur); continue
            if cur is None: raise ValueError('instruction before the first label')
            self.blocks[cur].append((op, a))
        self.defs, self.blk_of = {}, {}
        for b in self.order:
            for k, (op, a) in enumerate(self.blocks[b]):
                r = res(op, a)
                if r is not None: self.defs[r] = (b, k)
        self.var_sc = {a[1]: a[2] for op, a in m.ins[:f0] if op == 59}
        self.types = {a[0]: (op, a[1:]) for op, a in m.ins[:f0] if 19 <= op <= 39}
        self.consts = {a[1]: (a[0], a[2:]) for op, a in m.ins[:f0] if op == 43}

    def instr(self, i):
        b, k = self.defs[i]; return self.blocks[b][k]

    def term(self, b):
        ins = self.blocks[b]; t = ins[-1]
        mg = ins[-2] if len(ins) >= 2 and ins[-2][0] in (246, 247) else None
        return mg, t

    def chain(self):
        """the top-level blocks in execution order (entry, then the merge block of every top-level construct)"""
        out, b, seen = [], self.order[0], set()
        while b is not None and b not in seen:
            seen.add(b); out.append(b)
            mg, (op, a) = self.term(b)
            if op in (253, 254, 255, 252): break
            if mg is not None: b = mg[1][0]
            elif op == 249: b = a[0]
            else: return None   # an unstructured top level: not handled
        return out

    def cost(self, b):
        n = t = 0
        for op, a in self.blocks[b]:
            if op in (245, 246, 247, 249, 250, 251, 8, 317): continue
            n += 1; t += op in TEX
        return n, t


def kill_block(fn, b):
    ins = fn.blocks.get(b)
    return ins is not None and len(ins) == 1 and ins[0][0] == 252


def analyse(m):
    """-> dict: chain, kill points [(merge block, cond, kill_if_true)], alpha (value id, point block or None), costs"""
    fn = Fn(m); ch = fn.chain()
    if not ch: raise ValueError('top level is not a chain of constructs')
    pos = {b: i for i, b in enumerate(fn.order)}
    total = [0, 0]
    for b in fn.order:
        n, t = fn.cost(b); total[0] += n; total[1] += t
    # remaining cost from the start of each block in layout order
    rest = {}; acc = [0, 0]
    for b in reversed(fn.order):
        n, t = fn.cost(b); acc = [acc[0] + n, acc[1] + t]; rest[b] = tuple(acc)
    kills = []   # (header block, cond, kill_if_true, merge)
    for b in ch:
        mg, (op, a) = fn.term(b)
        if op == 250 and mg is not None and mg[0] == 247:
            M = mg[1][0]
            if kill_block(fn, a[1]) and a[2] == M: kills.append((b, a[0], True, M))
            elif kill_block(fn, a[2]) and a[1] == M: kills.append((b, a[0], False, M))
    # colour 0 alpha: the one store into component 3 of the Location 0 output, in a top-level block
    deco = {}
    for op, a in m.ins[:fn.f0]:
        if op == 71 and a[1] == 30: deco[a[0]] = a[2]
    out0 = [v for v, l in deco.items() if l == 0 and fn.var_sc.get(v) == 3]
    alpha = None
    if len(out0) == 1:
        o = out0[0]; ch_comp = {}
        for b in fn.order:
            for op, a in fn.blocks[b]:
                if op == 65 and a[2] == o and len(a) == 4 and a[3] in fn.consts: ch_comp[a[1]] = fn.consts[a[3]][1][0]
        st = [(b, a) for b in fn.order for op, a in fn.blocks[b] if op == 62 and (a[0] == o or a[0] in ch_comp)]
        st3 = [(b, a) for b, a in st if ch_comp.get(a[0]) == 3]
        whole = [(b, a) for b, a in st if a[0] == o]
        if len(st3) == 1 and not whole and st3[0][0] in ch: alpha = st3[0][1][1]
    apoint = None; moved = []
    if alpha is not None:
        sl, stk = set(), [alpha]
        while stk:
            x = stk.pop()
            if x in sl or x not in fn.defs: continue
            sl.add(x); op, a = fn.instr(x); stk += [u for u in uses(op, a) if u in fn.defs]
        chs = set(ch)
        last = max((ch.index(M) for _, _, _, M in kills), default=0)   # after the top-level kills: a killed wave skips the alpha too
        for C in ch[last:]:
            if any(o == 246 for o, _ in fn.blocks[C]): continue      # never split a loop header
            after = [x for x in sl if pos[fn.defs[x][0]] >= pos[C]]
            ok = True
            for x in after:
                b, k = fn.defs[x]; op, a = fn.blocks[b][k]
                if b not in chs or op == 245: ok = False; break
                if op == 61:
                    base = a[2]
                    while base in fn.defs and fn.instr(base)[0] in (65, 66): base = fn.instr(base)[1][2]
                    if fn.var_sc.get(base) not in PURE_LOADS: ok = False; break
                if op in (59, 57, 62, 63) or op in NORES: ok = False; break
            if ok:
                apoint = C; moved = sorted(after, key=lambda x: (pos[fn.defs[x][0]], fn.defs[x][1])); break
    return dict(fn=fn, chain=ch, kills=kills, alpha=alpha, apoint=apoint, moved=moved, total=total, rest=rest, pos=pos)


def plan(an, variant):
    """the points (block, kind) the variant gets, in chain order: the last kill of every cluster of top-level kills (kills with less
    than MIN_STEP of the shader between them), when at least MIN_REST of the shader follows; the alpha point (a / m / p)"""
    total = an['total'][0]; rest = an['rest']; fn = an['fn']
    order = {b: i for i, b in enumerate(an['chain'])}
    ms = sorted({M for _, _, _, M in an['kills'] if not any(o == 246 for o, _ in fn.blocks[M])}, key=lambda b: order[b])
    groups = []
    for M in ms:
        if groups and rest[groups[-1][-1]][0] - rest[M][0] < MIN_STEP * total: groups[-1].append(M)
        else: groups.append([M])
    pts = [(g[-1], 'k') for g in groups if rest[g[-1]][0] >= MIN_REST * total][:MAX_POINTS]
    if variant in 'amp' and an['apoint'] is not None and rest[an['apoint']][0] - len(an['moved']) >= MIN_REST * total:   # what a dead wave still skips
        A = an['apoint']; pts = [p for p in pts if p[0] != A] + [(A, 'a')]
        # a kill point right before the alpha point (little work between) is folded into it
        pts = sorted(pts, key=lambda x: order[x[0]])
        out = list(pts)
        pts = [p for i, p in enumerate(out) if not (p[1] == 'k' and i + 1 < len(out) and out[i + 1][1] == 'a'
                                                    and rest[p[0]][0] - rest[out[i + 1][0]][0] < MIN_STEP * total)]
    return pts


def build(spirv, variant):
    """the variant module (bytes) and its plan, or (None, reason)"""
    m = Mod(spirv); an = analyse(m); fn = an['fn']
    pts = plan(an, variant)
    if variant in 'amp' and not any(k == 'a' for _, k in pts): return None, 'no alpha point', an, pts
    if not pts: return None, 'no point worth a branch', an, pts
    g = m.ins[:fn.f0]
    # ---- ids we need: bool, float, uint, constants
    def find_type(op, *ops):
        for o, a in g:
            if o == op and a[1:] == list(ops): return a[0]
        return None
    extra = []
    tbool = find_type(20)
    if tbool is None: tbool = m.new_id(); extra.append((20, [tbool]))
    tf = find_type(22, 32)
    tu = find_type(21, 32, 0)
    if tu is None: tu = m.new_id(); extra.append((21, [tu, 32, 0]))
    def const(t, bits):
        for o, a in g + extra:
            if o == 43 and a[0] == t and a[2:] == [bits]: return a[1]
        i = m.new_id(); extra.append((43, [t, i, bits])); return i
    c3 = const(tu, 3)
    f0 = const(tf, 0) if tf else None
    fT = const(tf, struct.unpack('<I', struct.pack('<f', T_MOBILE))[0]) if tf and variant in 'mp' else None
    mag = None
    if variant == 'p':
        tv4 = find_type(23, tf, 4); f1 = const(tf, 0x3f800000)
        mag = m.new_id(); extra.append((44, [tv4, mag, f1, f0, f1, f1]))
    out0 = None
    if variant == 'p':
        for op, a in g:
            if op == 71 and a[1] == 30 and a[2] == 0 and fn.var_sc.get(a[0]) == 3: out0 = a[0]
    # ---- rebuild the function body
    blocks = {b: list(ins) for b, ins in fn.blocks.items()}; order = list(fn.order)
    moved = set(an['moved']) if any(k == 'a' for _, k in pts) else set()
    if moved:   # take the moved instructions out of their blocks (OpSampledImage / OpImage are cloned: their uses must stay in-block)
        clone = {}
        for b in order:
            keep = []
            for op, a in blocks[b]:
                r = res(op, a)
                if r in moved and op not in (86, 100): continue
                keep.append((op, a))
            blocks[b] = keep
    unreach = []; reparent = {}
    kills_by_merge = {}
    for hb, c, kt, M in an['kills']: kills_by_merge.setdefault(M, []).append((c, kt))
    chain_pos = {b: i for i, b in enumerate(an['chain'])}
    for C, kind in pts:
        conds = [kc for M, lst in kills_by_merge.items() if chain_pos[M] <= chain_pos[C] for kc in lst]
        alpha_on = any(k == 'a' for P, k in pts if chain_pos[P] <= chain_pos[C])
        ins = blocks[C]; nphi = 0
        while nphi < len(ins) and ins[nphi][0] == 245: nphi += 1
        head = ins[:nphi]
        if kind == 'a' and moved:
            remap = {}
            for x in an['moved']:
                op, a = fn.instr(x); a = list(a)
                if op in (86, 100):
                    n = m.new_id(); remap[x] = n; a[1] = n
                a = [a[0], a[1]] + [remap.get(u, u) for u in a[2:]] if res(op, a) is not None and op not in IDONLY else a
                head.append((op, a))
            # uses of a cloned OpSampledImage inside the moved set point at the clone (done above); the originals stay for the rest
        dead = None
        for c, kt in conds:
            d = c
            if not kt: d = m.new_id(); head.append((168, [tbool, d, c]))
            if dead is None: dead = d
            else: n = m.new_id(); head.append((166, [tbool, n, dead, d])); dead = n
        if alpha_on:
            av = an['alpha']
            if variant == 'a':
                ad = m.new_id(); head.append((180, [tbool, ad, av, f0]))                       # FOrdEqual alpha, 0 (+-0)
            else:
                ge = m.new_id(); lt = m.new_id(); ad = m.new_id()
                head += [(190, [tbool, ge, av, f0]), (184, [tbool, lt, av, fT]), (167, [tbool, ad, ge, lt])]
            if dead is None: dead = ad
            else: n = m.new_id(); head.append((166, [tbool, n, dead, ad])); dead = n
        if dead is None: continue
        live = m.new_id(); anyv = m.new_id(); R = m.new_id(); D = m.new_id(); U = m.new_id()
        head += [(168, [tbool, live, dead]), (334, [tbool, anyv, c3, live]), (247, [U, 0]), (250, [anyv, R, D])]
        blocks[C] = head
        blocks[R] = ins[nphi:]
        if variant == 'p' and alpha_on:
            # a skipped wave draws magenta (its killed lanes stay killed: they never get here, or their output is discarded)
            blocks[D] = [(62, [out0, mag]), (253, [])]
        else:
            blocks[D] = [(252, [])]
        order.insert(order.index(C) + 1, R); order.insert(order.index(R) + 1, D)
        blocks[U] = [(255, [])]; unreach.append(U)
        reparent[C] = R
        # phis elsewhere that name C as the predecessor now come from R (the block that ends with C's old terminator)
        for b in order:
            blocks[b] = [(op, [reparent.get(x, x) if (op == 245 and k >= 2 and k % 2 == 1) else x for k, x in enumerate(a)]) if op == 245 else (op, a)
                         for op, a in blocks[b]]
    order += unreach
    # ---- emit
    caps = {a[0] for op, a in g if op == 17}
    newg = []
    for op, a in g:
        newg.append((op, a))
    ci = max(k for k, (op, a) in enumerate(newg) if op == 17)
    for cap in (61, 62):
        if cap not in caps: ci += 1; newg.insert(ci, (17, [cap]))
    newg += extra
    body = [fn.head]
    for b in order: body.append((248, [b])); body += blocks[b]
    body.append((56, []))
    tail = []
    k = fn.f0 + 1
    while m.ins[k][0] != 56: k += 1
    tail = m.ins[k + 1:]
    m.ins = newg + body + tail
    if m.hdr[1] < 0x00010300: m.hdr[1] = 0x00010300
    return m.words(), None, an, pts


# ------------------------------------------------------------------------------------------------------------------- proof
def fuzz_case(m, rng, r):
    ubo_b = sorted({m.dec.get(v, {}).get(33, [None])[0] for v, (pt, sc) in m.vars.items() if sc == 2})
    tex_b = sorted({m.dec.get(v, {}).get(33, [None])[0] for v, (pt, sc) in m.vars.items() if sc == 0})
    ins = [m.dec[v][30][0] for v, (pt, sc) in m.vars.items() if sc == 1 and 30 in m.dec.get(v, {})]
    lo, hi = [(-4, 4), (0, 2), (-1, 1), (0.25, 4), (0.5, 8), (-0.5, 2)][r % 6]
    ubos = {}
    for b in ubo_b:   # floats; in 2 of 3 cases 60 % of the words are small integers or light-list words (a count in the high half, an
        arr = np.array([rng.uniform(lo, hi) for _ in range(1024)], dtype=np.float32).view(np.uint32)   # index in the low half)
        if r % 5 == 4:
            arr = np.array([rng.choice(SMALL) for _ in range(1024)], dtype=np.float32).view(np.uint32)
            for i in range(1024):
                if rng.random() < 0.4: arr[i] = rng.randrange(0, 3) << 16 | rng.randrange(0, 64)
        elif r % 3:
            for i in range(1024):
                if rng.random() < 0.6: arr[i] = (rng.randrange(0, 4) << 16 | rng.randrange(0, 64)) if r % 3 == 2 else rng.randrange(0, 4)
        ubos[b] = arr.tobytes() + b'\0' * (65536 - 4096)
    # texels: uniform values, exact zeros (coverage / masks) and words the shader compares with (its float constants, words of the
    # uniform buffers of this case: the surface-type id of a cell is compared for equality with the material's)
    pal = [F for F in (getattr(m, '_fpal', None) or [])]
    if not pal:
        pal = sorted({float(v) for v in m.consts.values() if isinstance(v, (float, np.floating)) and np.isfinite(v)})[:48]; m._fpal = pal or [0.0]
    for b in ubo_b:
        u = np.frombuffer(ubos[b][:4096], dtype=np.float32)
        pal = pal + [float(u[rng.randrange(1024)]) for _ in range(12)]
    tex = {}
    for b in tex_b:
        if r % 5 == 4:
            tex[b] = SI.Texture(np.array([[[rng.choice(SMALL) for _ in range(4)] for _ in range(4)] for _ in range(4)], dtype=np.float32),
                                filt='nearest'); continue
        z = rng.random() < 0.25; pp = rng.random() < 0.6
        tex[b] = SI.Texture(np.array([[[0.0 if z and rng.random() < 0.5 else (rng.choice(pal) if pp and rng.random() < 0.5 else rng.uniform(0, 1))
                                        for _ in range(4)] for _ in range(4)] for _ in range(4)], dtype=np.float32), filt='nearest' if rng.random() < 0.5 else 'linear')
    s = [0.5, 1.0, 3.0, 0.1, 0.25][r % 5]
    inputs = {'FragCoord': (rng.uniform(0, 2160), rng.uniform(0, 972), rng.uniform(0, 1), 1.0)}
    for l in ins: inputs[l] = tuple(rng.uniform(-s, s) for _ in range(4))
    return inputs, ubos, tex


def bits(v):
    if v is None: return None
    return {k: [SI.f2u(x) if not isinstance(x, (bool, int)) else int(x) for x in (vv if isinstance(vv, list) else [vv])] for k, vv in v.items()}


def neutral(spirv, bound):
    """the module with every original "if (c) kill" turned into "if (c) {}" (kill blocks with an id below bound -> branch to the merge
    of their selection): the code after a kill runs for every input, so random inputs exercise all of it"""
    m = Mod(spirv); fn = Fn(m); merge_of = {}
    for b in fn.order:
        mg, (op, a) = fn.term(b)
        if op == 250 and mg is not None and mg[0] == 247:
            for t in a[1:3]: merge_of[t] = mg[1][0]
    body = [fn.head]
    for b in fn.order:
        ins = fn.blocks[b]
        if b < bound and len(ins) == 1 and ins[0][0] == 252 and b in merge_of: ins = [(249, [merge_of[b]])]
        body.append((248, [b])); body += ins
    body.append((56, []))
    k = fn.f0 + 1
    while m.ins[k][0] != 56: k += 1
    m.ins = m.ins[:fn.f0] + body + m.ins[k + 1:]
    return m.words()


def prove(tr, var, variant, n, seed):
    """-> (errors, stats) on n seeded cases. Per case: T = translated, V = variant; T0 / V0 = the same with the translated shader's
    kills disarmed (neutral): the code after them runs for every input.
      V0 with a live neighbour == T0 bit for bit (the rest of the shader is the translated code, whatever the inputs);
      V  with a live neighbour == T  bit for bit (kills included);
      V  alone: == T, or V skips the lane where T kills it (k / a / m / p) or T leaves alpha dead (a: +-0; m / p: 0 <= a < 1/255)"""
    bound = struct.unpack_from('<I', tr, 12)[0]
    t0, v0 = neutral(tr, bound), neutral(var, bound)
    M = {k: SI.Module(x) for k, x in (('T', tr), ('V', var), ('T0', t0), ('V0', v0))}
    for x in M.values(): x.max_steps = 60000
    rng = random.Random(seed)
    err = []; st = dict(d_cases=0, d_skipped=0, d_killed=0, d_adead=0, d_exec_tr=0, d_exec_var=0, runaway=0, cases=0, live=0, killed=0, adead=0, skipped=0, tr_exec=0, var_exec_lone=0)
    def dead_alpha(o):
        al = o.get(0, [None] * 4)[3] if o else None
        if al is None or not np.isfinite(al): return False
        return float(al) == 0.0 if variant == 'a' else (0.0 <= float(al) < T_MOBILE if variant in 'mp' else False)
    for r in range(n):
        inp, ubo, tx = fuzz_case(M['T'], rng, r)
        try:
            M['V0'].any_all = True; M['V'].any_all = True
            t0r = M['T0'].run(inp, ubo, tx); v0r = M['V0'].run(inp, ubo, tx)
            tr_ = M['T'].run(inp, ubo, tx); et = M['T'].executed; vr = M['V'].run(inp, ubo, tx)
            M['V'].any_all = False; vl = M['V'].run(inp, ubo, tx); ev = M['V'].executed
            M['V0'].any_all = False; v0l = M['V0'].run(inp, ubo, tx); e0 = M['V0'].executed; e0t = M['T0'].executed
        except RuntimeError: st['runaway'] += 1; continue
        # disarmed, alone: the variant may only skip where a translated kill fires or the alpha is dead
        st['d_cases'] += 1; st['d_exec_tr'] += e0t; st['d_exec_var'] += e0
        if bits(v0l) != bits(t0r):
            st['d_skipped'] += 1
            if tr_ is None: st['d_killed'] += 1
            elif variant != 'k' and dead_alpha(t0r) and (v0l is None or variant == 'p'): st['d_adead'] += 1
            else: err.append('case %d: kills disarmed, alone: the variant skips a lane the translated shader draws' % r)
        st['cases'] += 1; st['tr_exec'] += et; st['var_exec_lone'] += ev
        if bits(t0r) != bits(v0r): err.append('case %d: kills disarmed, a live neighbour: the variant differs' % r)
        if bits(tr_) != bits(vr): err.append('case %d: a live neighbour: the variant differs (%s / %s)' % (r, bits(tr_), bits(vr)))
        if tr_ is None: st['killed'] += 1
        else: st['live'] += 1
        if bits(vl) == bits(tr_): continue
        st['skipped'] += 1
        if tr_ is None: continue
        if variant != 'k' and dead_alpha(tr_) and (vl is None or variant == 'p'): st['adead'] += 1; continue
        err.append('case %d: alone the variant skips / changes a lane the translated shader draws (%s / %s)' % (r, bits(tr_), bits(vl)))
    # the disarmed pair also counts how often the alpha is dead after the kills (fuzz inputs: no statement about the game)
    return err, st


# ------------------------------------------------------------------------------------------------------------------- pack
def tool(name):
    from dos2_native_shaders import tool as t
    return t(name)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: raise SystemExit('%s failed:\n%s%s' % (' '.join(map(str, cmd)), r.stdout[-1500:], r.stderr[-1500:]))
    return r.stdout


def fnv64(b):
    x = 0xcbf29ce484222325
    for c in b: x = ((x ^ c) * 0x100000001b3) & 0xffffffffffffffff
    return x


def bundle_parts(b):
    _, _, stage, nb, ni, c1, sp, _ = struct.unpack_from('<8I', b, 0)
    o = 32 + nb * 32 + ni * 4 + c1
    return stage, b[:o], b[o:o + sp]


def with_spirv(head, spv):
    h = bytearray(head); struct.pack_into('<I', h, 24, len(spv)); return bytes(h) + spv


def malioc_fs(exe, path, core):
    import json
    r = subprocess.run([exe, '-c', core, '--vulkan', '--fragment', '--format', 'json', str(path)], capture_output=True, text=True)
    try:
        sh = json.loads(r.stdout)['shaders'][0]; v = sh['variants'][0]; pf = v['performance']
    except Exception: return None
    def cyc(k):
        c = pf.get(k)
        if not c or not c.get('cycle_count'): return None
        d = {k: (x or 0.0) for k, x in zip(pf['pipelines'], c['cycle_count'])}; return max(d.get('arith_total', 0), d.get('load_store', 0), d.get('texture', 0), d.get('varying', 0)), d.get('texture', 0)
    return dict(total=cyc('total_cycles'), short=cyc('shortest_path_cycles'), long=cyc('longest_path_cycles'))


def programs(path):
    out = []
    for line in Path(path).read_text(encoding='utf-8').splitlines()[1:]:
        f = line.split('\t')
        if len(f) < 4 or f[3] == '-': continue
        name = f[0].rsplit('/', 1)[-1].replace('.bshd', '')
        if (f[0].startswith(SURF_DIR) and (name.endswith('_FOR_NX') or name.endswith('_DEF_NX'))) or name in FX_NAMES: out.append((name, f[3]))
    return out


def one(job):
    """one program: (rows, proof lines, {pack key: bytes}, failures)"""
    name, key, b, n, mexe, cores, keep = job
    rows, proof, files, fails = [], [], {}, 0
    stg, head, spv = bundle_parts(b)
    if stg != 4: return rows, proof, files, fails
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        src = tmp / (key + '.spv'); src.write_bytes(spv); opt = tmp / (key + '.o.spv')
        run([tool('spirv-opt'), '-O', str(src), '-o', str(opt)]); ob = opt.read_bytes()
        flags = 0
        # G-buffer programs (_DEF_NX) write several attachments with their own alphas: the kill points only
        for v in ('k' if name.endswith('_DEF_NX') else 'kamp'):
            try: res_, why, an, pts = build(ob, v)
            except (ValueError, KeyError, StopIteration) as e: res_, why, an, pts = None, 'not handled: %s' % e, None, []
            if res_ is None:
                proof.append('%s %s %s: no variant (%s)' % (name, key, v, why)); continue
            vp = tmp / ('%s.%s.spv' % (key, v)); vp.write_bytes(res_)
            run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(vp)])
            err, st = prove(ob, res_, v, n, int(key[:8], 16) ^ ord(v))
            if v in 'amp' and not err:
                # the alpha point alone, on the module with its kills disarmed: every random case reaches it, so the skip on a dead
                # alpha (and only there) is exercised; same transform, same proof
                t0 = neutral(ob, struct.unpack_from('<I', ob, 12)[0]); r2 = build(t0, v)[0]
                if r2 is not None:
                    e2, s2 = prove(t0, r2, v, n, int(key[:8], 16) ^ ord(v) ^ 0x55)
                    err += ['(alpha point, kills disarmed) ' + e for e in e2]
                    proof.append('%s %s %s: alpha point with the kills disarmed: %d cases bit for bit with a live neighbour; alone %d skipped, '
                                 'all with alpha dead (%d); executed a case %.0f -> %.0f' % (name, key, v, s2['cases'], s2['d_skipped'],
                                 s2['d_adead'], s2['d_exec_tr'] / max(1, s2['d_cases']), s2['d_exec_var'] / max(1, s2['d_cases'])))
            if err: fails += 1; proof += ['FAILED %s %s %s: %s' % (name, key, v, e) for e in err[:4]]; continue
            files['%s.%s' % (v, key)] = with_spirv(head, res_); flags |= 1 << 'kamp'.index(v)
            if keep: files['spv/%s.%s.spv' % (key, v)] = res_; files['spv/%s.o.spv' % key] = ob
            total = an['total']; first = an['rest'][pts[0][0]][0]
            mc = ''
            if mexe:
                parts = []
                for c in cores:
                    x, y = malioc_fs(mexe, opt, c), malioc_fs(mexe, vp, c)
                    f = lambda d, k: ('%.2f' % d[k][0]) if d and d.get(k) else '-'
                    parts.append('%s %s/%s -> %s/%s' % (c[5:], f(x, 'total'), f(x, 'short'), f(y, 'total'), f(y, 'short')))
                mc = '\t' + '; '.join(parts)
            dc = max(1, st['d_cases'])
            rows.append('%s\t%s\t%s\t%s\t%d\t%d\t%.0f%%\t%d/%d bit for bit (%d runaway)\t%d/%d/%d/%d of %d\t%.0f/%.0f%s' % (
                name, key, v, ','.join(k for _, k in pts), total[0], total[1], 100.0 * first / max(1, total[0]), st['cases'], st['cases'],
                st['runaway'], st['d_cases'] - st['d_skipped'], st['d_killed'], st['d_adead'], st['d_skipped'], st['d_cases'],
                st['d_exec_tr'] / dc, st['d_exec_var'] / dc, mc))
        if flags: files['sr.%s' % key] = struct.pack('<4sQII', b'XSR1', fnv64(spv), flags, 0)
    return rows, proof, files, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pack', required=True); ap.add_argument('--programs', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--n', type=int, default=16); ap.add_argument('--malioc', default=''); ap.add_argument('--cores', default='Mali-G57,Mali-G715')
    ap.add_argument('--only', default=''); ap.add_argument('--keep-spv', action='store_true'); ap.add_argument('--jobs', type=int, default=0)
    a = ap.parse_args()
    import concurrent.futures as cf, os
    from dos2_nr_sh_pack import pack_read
    import xgs_pack
    progs = [p for p in programs(a.programs) if a.only in p[0]]
    tr = pack_read(a.pack, {k for _, k in progs})
    out = Path(a.out); stage = out / 'bundles'
    if stage.exists(): shutil.rmtree(stage)
    stage.mkdir(parents=True)
    rows = ['name\tfs_key\tvariant\tpoints\tinstr\ttex\trest_after_first\tproof\tdisarmed alone: same/skipped (killed, alpha dead)/skipped of '
            'cases\texec a case disarmed: translated/variant alone' + ('\tmalioc ' + ' '.join(a.cores.split(',')) if a.malioc else '')]
    proof, fails, made = [], 0, 0
    jobs = [(name, key, tr[key], a.n, a.malioc, a.cores.split(','), a.keep_spv) for name, key in progs if key in tr]
    proof += ['%s %s: not in the pack' % (name, key) for name, key in progs if key not in tr]
    with cf.ProcessPoolExecutor(a.jobs or max(1, (os.cpu_count() or 2) - 1)) as ex:
        for r, p, files, f in ex.map(one, jobs):
            rows += r; proof += p; fails += f
            for k, b in files.items():
                if k.startswith('spv/'): (out / 'spv').mkdir(exist_ok=True); (out / k).write_bytes(b); continue
                (stage / (k + '.xgs')).write_bytes(b); made += not k.startswith('sr.')
    pack = out / 'native-nr33c.xgspack'
    xgs_pack.build(str(stage), str(pack))
    shutil.rmtree(stage)
    (out / 'nr_surf.tsv').write_text('\n'.join(rows) + '\n', encoding='utf-8')
    (out / 'proof.txt').write_text('\n'.join(proof) + '\n', encoding='utf-8')
    print('%d rows (nr_surf.tsv), %d proof lines (proof.txt), %d failed' % (len(rows) - 1, len(proof), fails))
    for l in proof:
        if l.startswith('FAILED'): print(l)
    print('%s: %d variant bundles, %d programs, sha256 %s' % (pack, made, len(jobs), hashlib.sha256(pack.read_bytes()).hexdigest()))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
