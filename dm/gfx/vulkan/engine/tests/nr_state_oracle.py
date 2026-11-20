#!/usr/bin/env python3
# NR-09S (DM_NR_STATE): the model of dm/gfx/nr/nr_state.h against the game's own machine code.
#
# ExecuteRenderCommand main+0x93e5f0 is run with opcode 21 from the user's own main.raw (update v786432, build 4979b200; never in the
# repository) in the Unicorn emulator on random renderer states and packets: the dispatcher, the opcode-21 handler 0x93e6ac, SetShader
# 0x9283a0 and SetBlend 0x9284e0 as they are. The engine's 19 NVN pointer slots point at stubs that log every call with its arguments.
# The same states go through the model (nr_state_test --oracle) and the two results must be identical: the NVN calls in order with every
# argument (the engine's two stack objects of the default-blend path stand for the model's NRST_LOCAL_*), the returned packet length,
# and every byte of guest memory afterwards. A case the model leaves to the engine must be one where the machine code faults or hands a
# bad pointer to an NVN call (or the case has a null table pointer / no recording context, which the model refuses by rule).
# Static checks: the code hashes and the slot names of NRST_CODE_TABLE / NRST_SLOTS (names from the engine's loader 0x934f00), the jump
# table entry of opcode 21, the primitive table.
#
#   python nr_state_oracle.py --main <main.raw> --exe <nr_state_test executable> [--cases 4000] [--write-hashes]
#                             [--mutations --cc <clang>]   (8 mutations of the model, each rebuilt from a copy: every one must FAIL)
# Exit 0 = PASS, 1 = FAIL, 77 = skipped (no main.raw / no unicorn / no capstone).
import argparse, os, random, re, struct, subprocess, sys, tempfile

GA, ARENA = 0x10000000, 0x18000
A_LIST, A_R, A_PKT, A_CNT, A_DEV, A_CTX, A_TAB, TAB_STRIDE = 0, 0x1000, 0x3000, 0x3100, 0x4000, 0x9400, 0xa000, 0x1400
STUB, STACK, SENT = 0x30000000, 0x40000000, 0x50000000
ERC, SPECIAL, PRIM, JT = 0x93e5f0, 0x2bbdb70, 0x1f58640, 0x1f50716
TABS = [(0xc0, 0x60), (0x180, 0x68), (0x240, 0x58), (0x480, 0x80), (0x600, 0x20), (0x6c0, 0x20), (0x780, 0x18)]   # shader VB IB vfmt blend DS raster
T_SHADER, T_VB, T_IB, T_VFMT, T_BLEND, T_DS, T_RASTER = range(7)
M64 = (1 << 64) - 1
HDR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'nr', 'nr_state.h')
# NRST_F_* (1-based) -> how the stub's registers map to an nrst_op {fn, i, j, f, a, p, q}
F_PROGRAM, F_DS, F_SMASK, F_SVMASK, F_SREF, F_POLY, F_BLEND_DEF, F_BLEND, F_COLOR_DEF, F_COLOR_ATEST, F_COLOR, F_BLEND_TARGET, F_COLOR_BLEND, \
    F_VATTR, F_VSTREAM, F_VB, F_REN, F_REN_COND, F_ALPHA_REF = range(1, 20)


def fnv(b):
    h = 0xcbf29ce484222325
    for c in b: h = ((h ^ c) * 0x100000001b3) & M64
    return h


def header():
    return open(HDR, encoding='utf-8').read()


def loader_names(main):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    pg, x1, names = {}, None, {}
    for i in md.disasm(main[0x934000:0x939000], 0x934000):
        if i.mnemonic == 'adrp': r, v = i.op_str.split(', #'); pg[r] = int(v, 16)
        elif i.mnemonic == 'add' and i.op_str.startswith('x1, x1, #'): x1 = pg.get('x1', 0) + int(i.op_str.split('#')[1], 16)
        elif i.mnemonic == 'str' and i.op_str.startswith('x0, [x8, #') and x1:
            names[pg.get('x8', 0) + int(i.op_str.split('#')[1].rstrip(']'), 16)] = main[x1:main.index(b'\0', x1)].decode('latin1'); x1 = None
    return names


def static_checks(main, h, write):
    code = re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)ull, "([^"]+)"\}', h)
    assert len(code) == 4, 'NRST_CODE_TABLE has four rows'
    new = h
    for off, end, hv, name in code:
        got = fnv(main[int(off, 16) + 16:int(end, 16)])
        if write: new = new.replace('{%s, %s, %sull, "%s"}' % (off, end, hv, name), '{%s, %s, %#xull, "%s"}' % (off, end, got, name))
        else: assert got == int(hv, 16), 'hash of %s (main+%s..%s) is %#x, the header says %s' % (name, off, end, got, hv)
    if write and new != h: open(HDR, 'w', encoding='utf-8', newline='\n').write(new); print('nr_state.h: code hashes written')
    slots = re.findall(r'\{(0x2bb[0-9a-f]+), "(nvn\w+)"\}', h)
    assert len(slots) == 19, 'NRST_SLOTS has 19 rows'
    names = loader_names(main)
    for s, n in slots: assert names.get(int(s, 16)) == n, 'slot main+%s is %s, the header says %s' % (s, names.get(int(s, 16)), n)
    e = struct.unpack_from('<H', main, JT + 2 * 21)[0]
    assert 0x93e648 + 4 * e == 0x93e6ac, 'jump table entry of opcode 21 -> %#x' % (0x93e648 + 4 * e)
    assert main[0x93e5f0:0x93e600] == struct.pack('<4I', 0xd10503ff, 0xfd006be8, 0xa90e6ffc, 0xa90f67fa), 'ExecuteRenderCommand prologue'
    return [int(s, 16) for s, _ in slots], struct.unpack_from('<7i', main, PRIM)


class World:
    def __init__(self, rng):
        self.rng = rng
        self.a = bytearray(rng.getrandbits(8 * ARENA).to_bytes(ARENA, 'little'))
        self.flags = set()

    def w64(self, o, v): struct.pack_into('<Q', self.a, o, v & M64)
    def w32(self, o, v): struct.pack_into('<I', self.a, o, v & 0xffffffff)
    def r64(self, o): return struct.unpack_from('<Q', self.a, o)[0]
    def r32(self, o): return struct.unpack_from('<I', self.a, o)[0]

    def build(self):
        rng = self.rng
        self.w64(A_LIST + 0x38, GA + A_R)
        self.w64(A_R + 0xb70, GA + A_DEV); self.w64(A_DEV + 0x5248, GA + A_CTX); self.w64(A_DEV + 0x51f0, rng.getrandbits(40))
        if rng.randrange(150) == 0: self.w64(A_DEV + 0x5248, 0); self.flags.add('noctx')
        for t, (off, esz) in enumerate(TABS):
            T = A_TAB + t * TAB_STRIDE; per = rng.choice((0, 1, 2, 3, 5, 8)) if rng.randrange(40) == 0 else 1 + rng.randrange(8)
            self.w64(A_R + off, GA + T); self.w64(A_R + off + 8, GA + T + 0x20); self.w32(A_R + off + 0x10, per)
            for p in range(4):
                self.w64(T + 8 * p, GA + T + 0x100 + 0x400 * p); self.w64(T + 0x20 + 8 * p, GA + T + 0x40 + 0x20 * p)
                for k in range(8): self.w32(T + 0x40 + 0x20 * p + 4 * k, 1 + rng.randrange(0x3ffffe))
                for k in range(8):
                    e = T + 0x100 + 0x400 * p + k * esz
                    if t == T_SHADER and rng.randrange(4) == 0: self.w64(e + 8 + 8 * rng.randrange(2), 0)
                    if t == T_VB and rng.randrange(6) == 0: self.w64(e, 0)
                    if t == T_VFMT:
                        if rng.randrange(2): self.w32(e + 0x1c, 0)
                        if rng.randrange(2): self.w32(e + 0x18, 0)
                    if t == T_DS: self.a[e + 0x10] = rng.randrange(256)
            if rng.randrange(400) == 0:   # a null table pointer somewhere (the model refuses by rule)
                which = rng.randrange(4)
                if which == 0: self.w64(A_R + off, 0)
                elif which == 1: self.w64(A_R + off + 8, 0)
                elif which == 2: self.w64(T + 8 * rng.randrange(4), 0)
                else: self.w64(T + 0x20 + 8 * rng.randrange(4), 0)
                self.flags.add('nulltab')
        self.special = (rng.getrandbits(48) | 1) if rng.randrange(3) else self.handle(T_BLEND, 3)

    def handle(self, t, kind):
        rng = self.rng; off_t = TABS[t][0]; T = A_TAB + t * TAB_STRIDE
        per = self.r32(A_R + off_t + 0x10); page = rng.randrange(4); off = rng.randrange(per) if per else 0
        idx = page * per + off; gen = self.r32(T + 0x40 + 0x20 * page + 4 * off) if per else 1
        if kind == 0: return 0
        if kind == 1: return idx
        if kind == 2: return rng.getrandbits(10) << 54 | ((gen ^ 5) & 0x3fffff) << 32 | idx
        return rng.getrandbits(10) << 54 | (gen & 0x3fffff) << 32 | idx

    def pick(self, t):
        r = self.rng.randrange(16)
        return self.handle(t, 0 if r == 0 else 1 if r == 1 else 2 if r == 2 else 3)

    def packet(self):
        rng = self.rng
        P, R = A_PKT, A_R
        bl = self.special if rng.randrange(8) == 0 else self.pick(T_BLEND)
        for o, v in ((8, self.pick(T_SHADER)), (0x10, self.pick(T_DS)), (0x20, self.pick(T_RASTER)), (0x28, bl), (0x30, self.pick(T_VFMT))): self.w64(P + o, v)
        self.w32(P + 0x18, rng.randrange(4) if rng.randrange(8) else rng.getrandbits(32))
        for k in range(3): self.w64(P + 0x38 + 8 * k, 0 if k and rng.randrange(2) else self.pick(T_VB))
        per = self.r32(R + 0x240 + 0x10)
        self.w64(P + 0x50, (rng.getrandbits(32) << 32 | rng.randrange(4 * per)) if rng.randrange(4) and per else 0)
        self.a[P + 0x58] = rng.randrange(10) if rng.randrange(6) else rng.randrange(256)
        self.w64(P + 0x60, 0 if rng.randrange(3) else GA + A_CNT)
        fa = rng.choice((-1.0, 0.0, 0.5, -0.0, 0.25, 1.0, float('nan'), -2.5))
        struct.pack_into('<f', self.a, P + 0x68, fa)
        for ro, po in ((0x17d8, 8), (0x1708, 0x10), (0x16f0, 0x20), (0x16f8, 0x28), (0x1740, 0x30), (0x1748, 0x38), (0x1750, 0x40), (0x1758, 0x48), (0x17c8, 0x50), (0x1860, 0x60)):
            if rng.randrange(2): self.a[R + ro:R + ro + 8] = self.a[P + po:P + po + 8]
        if rng.randrange(2): self.a[R + 0x1710:R + 0x1714] = self.a[P + 0x18:P + 0x1c]
        if rng.randrange(2): self.a[R + 0x1714] = self.a[P + 0x58]
        if rng.randrange(2): self.a[R + 0x1868:R + 0x186c] = self.a[P + 0x68:P + 0x6c]
        self.a[R + 0x185c] = rng.choice((0, 1, 0, 1, rng.randrange(256)))
        for k in range(3): self.w64(R + 0x1788 + 8 * k, 0 if rng.randrange(3) else rng.getrandbits(64))
        self.w64(R + 0x17d0, 0 if rng.randrange(3) else rng.getrandbits(64))
        self.w32(R + 0x1700, 0xffffffff if rng.randrange(2) else rng.getrandbits(32))
        for i in range(8): self.w64(R + 0x17e8 + 8 * i, 0 if rng.randrange(2) else rng.getrandbits(64))


class Emu:
    def __init__(self, main, slots):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
        self.uc = uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for lo, hi in ((0x928000, 0x929000), (0x93e000, 0x940000), (0x1f50000, 0x1f51000), (0x1f58000, 0x1f59000)):
            uc.mem_map(lo, hi - lo); uc.mem_write(lo, main[lo:hi])
        uc.mem_map(0x2bbd000, 0x3000)
        for i, s in enumerate(slots): uc.mem_write(s, struct.pack('<Q', STUB + 4 * i))
        ret = struct.pack('<I', 0xd65f03c0)
        uc.mem_map(STUB, 0x1000); uc.mem_write(STUB, ret * 32)
        uc.mem_map(GA, ARENA); uc.mem_map(STACK, 0x10000); uc.mem_map(SENT, 0x1000); uc.mem_write(SENT, ret)
        uc.hook_add(UC_HOOK_CODE, self.on_stub, begin=STUB, end=STUB + 4 * len(slots) - 1)
        uc.hook_add(UC_HOOK_MEM_INVALID, self.on_bad)
        self.calls, self.fault = [], None

    def on_stub(self, uc, addr, size, user):
        from unicorn import arm64_const as c
        x = [uc.reg_read(getattr(c, 'UC_ARM64_REG_X%d' % k)) for k in range(4)]
        s0 = struct.unpack('<f', struct.pack('<I', uc.reg_read(c.UC_ARM64_REG_S0) & 0xffffffff))[0]
        s0b = uc.reg_read(c.UC_ARM64_REG_S0) & 0xffffffff
        self.calls.append(((addr - STUB) // 4 + 1, x, s0b))

    def on_bad(self, uc, access, addr, size, value, user):
        self.fault = addr; return False

    def run(self, w):
        from unicorn import arm64_const as c, UcError
        uc = self.uc
        uc.mem_write(GA, bytes(w.a)); uc.mem_write(SPECIAL, struct.pack('<Q', w.special)); self.calls, self.fault = [], None
        for k in range(31): uc.reg_write(getattr(c, 'UC_ARM64_REG_X%d' % k), 0x7777000000000000 + k)
        uc.reg_write(c.UC_ARM64_REG_SP, STACK + 0x8000); uc.reg_write(c.UC_ARM64_REG_X30, SENT)
        uc.reg_write(c.UC_ARM64_REG_X0, GA + A_LIST); uc.reg_write(c.UC_ARM64_REG_X1, 0x77770000 | 21); uc.reg_write(c.UC_ARM64_REG_X2, GA + A_PKT)
        try: uc.emu_start(ERC, SENT, count=400000)
        except UcError as e: self.fault = self.fault if self.fault is not None else str(e)
        if self.fault is not None: return None
        assert uc.reg_read(c.UC_ARM64_REG_PC) == SENT and uc.reg_read(c.UC_ARM64_REG_SP) == STACK + 0x8000, 'ExecuteRenderCommand did not return'
        assert uc.reg_read(c.UC_ARM64_REG_X0) == 0x70, 'packet length %#x' % uc.reg_read(c.UC_ARM64_REG_X0)
        for k in range(19, 30): assert uc.reg_read(getattr(c, 'UC_ARM64_REG_X%d' % k)) == 0x7777000000000000 + k, 'callee-saved x%d' % k
        lo, hi = STACK, STACK + 0x10000
        out = []
        for fn, x, s0 in self.calls:
            i32 = lambda v: v & 0xffffffff
            a = p = q = i = j = f = 0
            if fn == F_PROGRAM: a, p, i = x[0], x[1], i32(x[2])
            elif fn in (F_DS, F_POLY, F_BLEND, F_COLOR): a, p = x[0], x[1]
            elif fn in (F_SMASK, F_SVMASK, F_SREF): a, i, j = x[0], i32(x[1]), i32(x[2])
            elif fn in (F_BLEND_DEF, F_COLOR_DEF): p = x[0]
            elif fn in (F_COLOR_ATEST, F_BLEND_TARGET): p, i = x[0], i32(x[1])
            elif fn == F_COLOR_BLEND: p, i, j = x[0], i32(x[1]), i32(x[2])
            elif fn in (F_VATTR, F_VSTREAM): a, i, p = x[0], i32(x[1]), x[2]
            elif fn == F_VB: a, i, p, q = x[0], i32(x[1]), x[2], x[3]
            elif fn == F_REN: a, i = x[0], i32(x[1])
            elif fn == F_REN_COND: a, i, p = x[0], i32(x[1]), x[2]
            elif fn == F_ALPHA_REF: a, f = x[0], s0
            if lo <= p < hi: p = 1 if fn in (F_BLEND_DEF, F_BLEND) else 2   # the engine's stack objects = NRST_LOCAL_BLEND / _COLOR
            out.append((fn, i, j, f, a, p, q))
        return out, bytes(uc.mem_read(GA, ARENA))


MUTATIONS = [
    ('NRST_OP(s, NRST_F_STENCIL_MASK, cb, 0, 0, 3, m);\n            NRST_OP(s, NRST_F_STENCIL_VMASK, cb, 0, 0, 3, m);',
     'NRST_OP(s, NRST_F_STENCIL_VMASK, cb, 0, 0, 3, m);\n            NRST_OP(s, NRST_F_STENCIL_MASK, cb, 0, 0, 3, m);'),
    ('if (k && !h) continue;', 'if (0 && k && !h) continue;'),
    ('if (a >= 0.f) nrst_op_put', 'if (a > 0.f) nrst_op_put'),
    ('? 3 : nrst_r32(E + 0x18) ? 2 : 1', '? 3 : nrst_r32(E + 0x18) ? 2 : 2'),
    ('if (!x->ds || pds != x->ds || x->ref != ref)', 'if (!x->ds || pds != x->ds)'),
    ('(uint64_t)off * 0x58 + 0x18', '(uint64_t)off * 0x58 + 0x10'),
    ('NRST_R_RT + 8 * i) != 0', 'NRST_R_RT + 8 * (i & 6)) != 0'),   # ('!x->blend ||' dropped is an equivalent mutant: same memory, same calls)
    ('if (!e) { x->vb[k] = 0;', 'if (!e) { x->vb[k] = h;'),
]


def mutations(a, main, slots, prim):
    import shutil
    here = os.path.dirname(os.path.abspath(__file__)); caught = 0
    for n, (old, new) in enumerate(MUTATIONS):
        with tempfile.TemporaryDirectory() as td:
            os.makedirs(os.path.join(td, 'a', 'b', 'c')); os.makedirs(os.path.join(td, 'nr'))
            h = header(); assert old in h, 'mutation %d does not apply' % n
            open(os.path.join(td, 'nr', 'nr_state.h'), 'w', newline='\n').write(h.replace(old, new, 1))
            src = os.path.join(td, 'a', 'b', 'c', 'nr_state_test.c'); shutil.copy(os.path.join(here, 'nr_state_test.c'), src)
            exe = os.path.join(td, 'm.exe')
            r = subprocess.run([a.cc, '-std=c11', '-O1', '-w', '-D_CRT_SECURE_NO_WARNINGS', src, '-o', exe], capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
            r = subprocess.run([sys.executable, os.path.abspath(__file__), '--main', a.main, '--exe', exe, '--cases', str(min(a.cases, 1500)), '--seed', str(a.seed + n)],
                               capture_output=True, text=True)
            ok = r.returncode == 1
            caught += ok
            print('mutation %d (%s -> %s): %s' % (n, old.split('\n')[0][:48], new.split('\n')[0][:48], 'caught' if ok else 'NOT caught (rc %d)' % r.returncode))
    print('%s: %d of %d mutations caught' % ('PASS' if caught == len(MUTATIONS) else 'FAIL', caught, len(MUTATIONS)))
    return 0 if caught == len(MUTATIONS) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--main', required=True); ap.add_argument('--exe'); ap.add_argument('--cases', type=int, default=4000)
    ap.add_argument('--seed', type=int, default=0x9e3779b9); ap.add_argument('--write-hashes', action='store_true')
    ap.add_argument('--mutations', action='store_true'); ap.add_argument('--cc')
    a = ap.parse_args()
    if not os.path.exists(a.main): print('SKIP: no %s (the game file is never in the repository)' % a.main); return 77
    try:
        import unicorn, capstone  # noqa: F401
    except ImportError: print('SKIP: unicorn / capstone missing'); return 77
    main = open(a.main, 'rb').read()
    slots, prim = static_checks(main, header(), a.write_hashes)
    if a.write_hashes: return 0
    print('static: 4 code hashes, 19 NVN slots by the loader\'s names, jump table entry 21 -> 0x93e6ac, primitive table %s' % (prim,))
    rng = random.Random(a.seed); emu = Emu(main, slots)
    worlds, results = [], []
    if a.mutations: return mutations(a, main, slots, prim)
    for n in range(a.cases):
        w = World(rng); w.build(); w.packet(); worlds.append(w)
        results.append(emu.run(w))
    with tempfile.TemporaryDirectory() as td:
        fin, fout = os.path.join(td, 'in.bin'), os.path.join(td, 'out.bin')
        with open(fin, 'wb') as f:
            f.write(struct.pack('<II', 0x5453524e, len(worlds))); f.write(struct.pack('<7i', *prim))
            for w in worlds: f.write(struct.pack('<Q', w.special)); f.write(bytes(w.a))
        r = subprocess.run([a.exe, '--oracle', fin, fout], capture_output=True, text=True)
        if r.returncode: print(r.stdout, r.stderr); return 1
        out = open(fout, 'rb').read()
    pos = 4; n = struct.unpack_from('<I', out, 0)[0]; assert n == len(worlds)
    bad = 0; ran = calls = refused = refused_ok = 0; kinds = {}
    for k, w in enumerate(worlds):
        r_ran, r_why, r_n = struct.unpack_from('<III', out, pos); pos += 12
        mops = []
        for i in range(r_n):
            fn, ii, jj, ff, aa, pp, qq = struct.unpack_from('<IiiIQQQ', out, pos); pos += 40
            mops.append((fn, ii & 0xffffffff, jj & 0xffffffff, ff, aa, pp, qq))
        marena = out[pos:pos + ARENA]; pos += ARENA
        res = results[k]
        if not r_ran:
            refused += 1
            ok = res is None or 'nulltab' in w.flags or 'noctx' in w.flags or any((c[4] and c[4] < 0x1000) or (c[5] and 2 < c[5] < 0x1000) for c in res[0])
            if ok: refused_ok += 1
            else:
                bad += 1
                if bad <= 10: print('FAIL case %d: the model refused (why %d) but the machine code ran cleanly: %s' % (k, r_why, res[0][:6]))
            continue
        if res is None:
            bad += 1
            if bad <= 10: print('FAIL case %d: the machine code faulted at %s, the model ran' % (k, emu.fault))
            continue
        eops, earena = res
        ran += 1; calls += len(eops)
        for c in eops: kinds[c[0]] = kinds.get(c[0], 0) + 1
        if eops != mops:
            bad += 1
            if bad <= 10:
                d = next((i for i in range(min(len(eops), len(mops))) if eops[i] != mops[i]), min(len(eops), len(mops)))
                print('FAIL case %d: calls differ at %d (%d machine, %d model): machine %s / model %s' % (k, d, len(eops), len(mops),
                      eops[d] if d < len(eops) else None, mops[d] if d < len(mops) else None))
            continue
        if earena != marena:
            bad += 1
            if bad <= 10:
                d = next(i for i in range(ARENA) if earena[i] != marena[i])
                print('FAIL case %d: guest memory differs at arena+%#x (machine %02x, model %02x)' % (k, d, earena[d], marena[d]))
    print('cases %d: model ran %d (%d NVN calls, every kind: %s), left to the engine %d (%d where the machine code faults / passes a bad pointer / null table / no context)' % (
        len(worlds), ran, calls, ' '.join('%d:%d' % (f, kinds[f]) for f in sorted(kinds)), refused, refused_ok))
    if len(kinds) < 19: print('FAIL: not every NVN call kind was made (%d of 19)' % len(kinds)); bad += 1
    print('PASS' if not bad else 'FAIL (%d)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
