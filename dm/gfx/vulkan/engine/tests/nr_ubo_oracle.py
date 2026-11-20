#!/usr/bin/env python3
# NR-09U (DM_NR_UBO): the model of dm/gfx/nr/nr_ubo.h against the game's own machine code.
#
# The two uniform-block functions every engine draw runs before its texture flush - 0x926f90(r) (global blocks, under r's read lock)
# and 0x925740(r) (the shader's blocks of stages 0 / 1 / 5) - are run from the user's own main.raw (update v786432, build 4979b200;
# never in the repository) in the Unicorn emulator, in that order as the draw functions call them, on random renderer / device / ring
# states, together with their lock helpers 0x9156a0 / 0x918050 and the PLT stubs of nn::os::GetCurrentThread / YieldThread. The engine's
# nvnCommandBufferBindUniformBuffer pointer (main+0x2bbfa80) points at a stub that logs every call with its five arguments. The same states
# go through the model (nr_ubo_test --oracle) and the two results must be identical: the calls in order with every argument, every byte
# of the renderer / shader record / blocks / list / shadows arena, the device's frame / ctx / ring head / ring lock / bases, and the ring
# (CRC-32 of the window at the head and of the wrap window at 0). A case the model refuses must be one the generator made bad on purpose
# (a flag) or one where the machine code faults.
# Static checks: the code hashes of NRU_CODE_TABLE, the slot name of NRU_SLOT_BIND by the engine's loader (0x934f00), the PLT stubs
# 0x1bf45a0 / 0x1bf4590 = YieldThread / GetCurrentThread (their GOT slots by the dynamic symbol table), the draw functions' calls of the
# two functions (0x928efc / 0x928f04, 0x92902c / 0x929034), the waits' call sites.
#
#   python nr_ubo_oracle.py --main <main.raw> --exe <nr_ubo_test executable> [--cases 4000] [--mutations --cc <clang>]
# Exit 0 = PASS, 1 = FAIL, 77 = skipped (no main.raw / no unicorn / no capstone).
import argparse, os, random, re, struct, subprocess, sys, tempfile, zlib

GA, A1, DEVA, DEVN, RINGA = 0x10000000, 0x3000, 0x20000000, 0x26000, 0x40000000
RINGN = 0x1380000 + 0x20000
O_R, O_S, O_ARR, ARR_N, O_LIST, O_GBLK, GBLK_N, O_SH = 0, 0x1800, 0x1840, 6, 0x1cc0, 0x1d00, 8, 0x1f00
STUB, STACK, SENT = 0x30000000, 0x50000000, 0x60000000
FN_G, FN_S, SLOT_BIND, GOT_CUR, GOT_YIELD = 0x926f90, 0x925740, 0x2bbfa80, 0x2b04fd0, 0x2b04fd8
D_FRAME, D_CTX, D_HEAD, D_CPU, D_GPU, D_LOCK = 0x51f0, 0x5248, 0x25a60, 0x25a98, 0x25aa0, 0x25aa8
R_DEV, R_GLOCK, R_GOWNER, R_GLIST, R_GCOUNT, R_SHADER = 0xb70, 0xbe0, 0xbe4, 0xbf8, 0xc00, 0x17e0
B_STAMP, B_GPU, B_CPU, B_SRC, B_SIZE, B_DIRTY, B_SLOT, BLOCK = 0, 0x10, 0x18, 0x20, 0x30, 0x34, 0x35, 0x40
THR = 0x7777a5a50000   # what the GetCurrentThread stub returns (the engine compares its low 32 bits with r+0xbe4)
WIN = 0x10000
M64 = (1 << 64) - 1
HDR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'nr', 'nr_ubo.h')


def fnv(b):
    h = 0xcbf29ce484222325
    for c in b: h = ((h ^ c) * 0x100000001b3) & M64
    return h


def header(): return open(HDR, encoding='utf-8').read()


def bl_target(main, at):
    w = struct.unpack_from('<I', main, at)[0]; o = w & 0x3ffffff
    return at + ((o - (1 << 26) if o & (1 << 25) else o) << 2) if w >> 26 == 0x25 else None


def static_checks(main, h):
    code = re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)ull, "([^"]+)"\}', h)
    assert len(code) == 4, 'NRU_CODE_TABLE has four rows'
    for off, end, hv, name in code:
        got = fnv(main[int(off, 16) + 16:int(end, 16)])
        assert got == int(hv, 16), 'hash of %s (main+%s..%s) is %#x, the header says %s' % (name, off, end, got, hv)
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    pg, x1, names = {}, None, {}
    for i in md.disasm(main[0x934000:0x939000], 0x934000):   # the engine's NVN loader: which name each pointer slot gets
        if i.mnemonic == 'adrp': r, v = i.op_str.split(', #'); pg[r] = int(v, 16)
        elif i.mnemonic == 'add' and i.op_str.startswith('x1, x1, #'): x1 = pg.get('x1', 0) + int(i.op_str.split('#')[1], 16)
        elif i.mnemonic == 'str' and i.op_str.startswith('x0, [x8, #') and x1:
            names[pg.get('x8', 0) + int(i.op_str.split('#')[1].rstrip(']'), 16)] = main[x1:main.index(b'\0', x1)].decode('latin1'); x1 = None
    assert names.get(SLOT_BIND) == 'nvnCommandBufferBindUniformBuffer', 'slot main+%#x is %s' % (SLOT_BIND, names.get(SLOT_BIND))
    assert 'NRU_SLOT_BIND = 0x2bbfa80' in h
    # PLT stubs -> GOT -> dynamic symbol names
    mod0 = struct.unpack_from('<I', main, 4)[0]; assert main[mod0:mod0 + 4] == b'MOD0'
    dyn = mod0 + struct.unpack_from('<i', main, mod0 + 4)[0]; tags = {}
    for i in range(0, 0x1000, 16):
        t, v = struct.unpack_from('<QQ', main, dyn + i)
        if t == 0: break
        tags[t] = v
    symtab, strtab, jmprel, pltsz = tags[6], tags[5], tags[23], tags[2]
    def got_name(got):
        for k in range(0, pltsz, 24):
            off, info, _ = struct.unpack_from('<QQQ', main, jmprel + k)
            if off == got:
                nm = struct.unpack_from('<I', main, symtab + 24 * (info >> 32))[0]
                return main[strtab + nm:main.index(b'\0', strtab + nm)].decode()
    for plt, got, want in ((0x1bf45a0, GOT_YIELD, '_ZN2nn2os11YieldThreadEv'), (0x1bf4590, GOT_CUR, '_ZN2nn2os16GetCurrentThreadEv')):
        adrp, ldr = struct.unpack_from('<II', main, plt)
        assert got_name(got) == want, 'GOT %#x is %s' % (got, got_name(got))
        insn = list(md.disasm(main[plt:plt + 16], plt))
        assert insn[0].mnemonic == 'adrp' and insn[1].mnemonic == 'ldr' and int(insn[0].op_str.split('#')[1], 16) + int(insn[1].op_str.split('#')[1].rstrip(']'), 16) == got, 'PLT %#x' % plt
    for g, s in ((0x928efc, 0x928f04), (0x92902c, 0x929034)):
        assert bl_target(main, g) == FN_G and bl_target(main, s) == FN_S, 'the draw functions call 0x926f90 then 0x925740 (%#x / %#x)' % (g, s)
    return len(names)


class World:
    def __init__(self, rng):
        self.rng = rng; self.flags = set()
        self.a = bytearray(rng.getrandbits(8 * A1).to_bytes(A1, 'little'))

    def w64(self, o, v): struct.pack_into('<Q', self.a, o, v & M64)
    def w32(self, o, v): struct.pack_into('<I', self.a, o, v & 0xffffffff)

    def block(self, o, frame, shader):
        rng = self.rng
        self.w64(o + B_STAMP, frame if rng.randrange(3) else (frame - 1 - rng.randrange(3)))
        self.a[o + B_DIRTY] = 0 if rng.randrange(3) else rng.randrange(1, 256)
        self.a[o + B_SLOT] = rng.randrange(8) if rng.randrange(6) else rng.randrange(8, 256)
        self.w32(o + B_SIZE, 16 * rng.randrange(17) if rng.randrange(8) else rng.randrange(0x100))
        self.w64(o + B_SRC, GA + O_SH + rng.randrange(A1 - O_SH - 0x110))
        if shader and rng.randrange(5) == 0: self.w64(o + B_CPU, 0)

    def build(self):
        rng, f = self.rng, self.flags
        bad = lambda p: rng.random() < p
        frame = rng.getrandbits(64)
        hk = rng.randrange(8)
        head = 0x137ff00 + rng.randrange(0x100) if hk == 0 else 0xffffff00 + rng.randrange(0x100) if hk == 1 else 0 if hk == 2 else \
            rng.randrange(0x1370000) if hk < 7 else rng.randrange(1 << 32)
        self.dev = dict(frame=frame, ctx=0x7100000000 + (rng.getrandbits(12) << 4), head=head, lock=0, cpu=RINGA, gpu=0x8000000000 + (rng.getrandbits(24) << 8))
        if bad(0.02): self.dev['ctx'] = 0; f.add('noctx')
        if bad(0.02): self.dev['cpu'] = 0; f.add('noring')
        self.w64(O_R + R_DEV, DEVA)
        if bad(0.015): self.w64(O_R + R_DEV, 0); f.add('nodev')
        self.w32(O_R + R_GLOCK, rng.randrange(4))   # other readers in it
        ow = rng.random()
        self.w32(O_R + R_GOWNER, 0xffffffff if ow < 0.86 else THR if ow < 0.93 else rng.getrandbits(32))
        if ow >= 0.86: f.add('owner')
        ng = rng.randrange(5)
        if bad(0.015): ng = 17; f.add('count')
        self.w64(O_R + R_GCOUNT, (rng.getrandbits(64) & ~(0xffffff << 24)) | (ng << 24))
        self.w64(O_R + R_GLIST, GA + O_LIST)
        if bad(0.015): self.w64(O_R + R_GLIST, 0); f.add('nulllist')
        for i in range(GBLK_N): self.block(O_GBLK + BLOCK * i, frame, 0)
        for i in range(min(ng, 8)): self.w64(O_LIST + 8 * i, GA + O_GBLK + BLOCK * rng.randrange(GBLK_N))
        self.w64(O_R + R_SHADER, GA + O_S if rng.randrange(10) else 0)
        for st in range(3):
            self.a[O_S + 0x38 + st] = rng.randrange(ARR_N + 1)
            self.w64(O_S + 0x20 + 8 * st, GA + O_ARR + st * ARR_N * BLOCK)
            for i in range(ARR_N): self.block(O_ARR + (st * ARR_N + i) * BLOCK, frame, 1)
        if bad(0.015): self.a[O_S + 0x38 + rng.randrange(3)] = 17 + rng.randrange(20); f.add('count')
        # the bad features (each flagged: the model may refuse such a case)
        if bad(0.015): self.w64(O_S + 0x20 + 8 * rng.randrange(3), 0); f.add('nullarr')
        if ng and bad(0.015): self.w64(O_LIST + 8 * rng.randrange(min(ng, 8)), 0); f.add('nullblk')
        if bad(0.015): self.w32(self.anyblock() + B_SIZE, 0x10001 + rng.randrange(0x1000)); f.add('size')
        if bad(0.015): self.w64(self.anyblock() + B_SRC, 0); f.add('nullsrc')
        if bad(0.015): self.w64(self.anyblock() + B_SRC, RINGA + rng.randrange(0x20000)); f.add('srcring')
        if bad(0.015): self.w64(O_S + 0x20 + 8 * rng.randrange(3), GA + O_S + rng.randrange(0x30)); f.add('arrhdr')
        if bad(0.015): self.w64(O_S + 0x28, GA + O_ARR + BLOCK * rng.randrange(1, 4)); f.add('arrarr')
        if ng and bad(0.02): self.w64(O_LIST + 8 * rng.randrange(min(ng, 8)), GA + O_ARR + BLOCK * rng.randrange(3 * ARR_N)); f.add('gshader')
        if ng and bad(0.02): self.w64(O_LIST + 8 * rng.randrange(min(ng, 8)), GA + O_GBLK + 8 * rng.randrange(1, 8 * (GBLK_N - 1))); f.add('ghalf')
        if bad(0.02): self.w64(self.anyblock() + B_SRC, GA + O_GBLK + rng.randrange(GBLK_N * BLOCK)); f.add('srcg')
        if ng and bad(0.015): self.w64(O_LIST + 8 * rng.randrange(min(ng, 8)), GA + O_LIST - 0x20 + 8 * rng.randrange(8)); f.add('glist')

    def anyblock(self):
        return O_ARR + BLOCK * self.rng.randrange(3 * ARR_N) if self.rng.randrange(2) else O_GBLK + BLOCK * self.rng.randrange(GBLK_N)

    def devbytes(self):
        d = self.dev
        return struct.pack('<QQIIQQ', d['frame'], d['ctx'], d['head'], d['lock'], d['cpu'], d['gpu'])


def windows(ring, h0):
    s = min(h0 & 0xffffff00, RINGN); n = WIN if s + WIN <= RINGN else RINGN - s
    return zlib.crc32(bytes(ring[0:WIN])), zlib.crc32(bytes(ring[s:s + n]))


class Emu:
    def __init__(self, main):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
        self.uc = uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for lo, hi in ((0x915000, 0x919000), (0x925000, 0x928000), (0x1bf4000, 0x1bf5000)):
            uc.mem_map(lo, hi - lo); uc.mem_write(lo, main[lo:hi])
        uc.mem_map(0x2b04000, 0x1000); uc.mem_write(0x2b04000, main[0x2b04000:0x2b05000])
        uc.mem_map(0x2bbf000, 0x1000)
        uc.mem_write(SLOT_BIND, struct.pack('<Q', STUB)); uc.mem_write(GOT_CUR, struct.pack('<Q', STUB + 4)); uc.mem_write(GOT_YIELD, struct.pack('<Q', STUB + 8))
        ret = struct.pack('<I', 0xd65f03c0)
        uc.mem_map(STUB, 0x1000); uc.mem_write(STUB, ret * 4)
        uc.mem_map(GA, 0x4000); uc.mem_map(DEVA, DEVN); uc.mem_map(RINGA, (RINGN + 0xfff) & ~0xfff)
        uc.mem_map(STACK, 0x10000); uc.mem_map(SENT, 0x1000); uc.mem_write(SENT, ret)
        uc.hook_add(UC_HOOK_CODE, self.on_stub, begin=STUB, end=STUB + 11)
        uc.hook_add(UC_HOOK_MEM_INVALID, self.on_bad)
        self.calls, self.fault, self.yields = [], None, 0

    def on_stub(self, uc, addr, size, user):
        from unicorn import arm64_const as c
        if addr == STUB: self.calls.append(tuple(uc.reg_read(getattr(c, 'UC_ARM64_REG_X%d' % k)) for k in range(5)))
        elif addr == STUB + 4: uc.reg_write(c.UC_ARM64_REG_X0, THR)
        else: self.yields += 1

    def on_bad(self, uc, access, addr, size, value, user):
        self.fault = addr; return False

    def call(self, fn, r):
        from unicorn import arm64_const as c, UcError
        uc = self.uc
        for k in range(31): uc.reg_write(getattr(c, 'UC_ARM64_REG_X%d' % k), 0x7777000000000000 + k)
        uc.reg_write(c.UC_ARM64_REG_SP, STACK + 0x8000); uc.reg_write(c.UC_ARM64_REG_X30, SENT); uc.reg_write(c.UC_ARM64_REG_X0, r)
        try: uc.emu_start(fn, SENT, count=3000000)
        except UcError as e: self.fault = self.fault if self.fault is not None else str(e)
        if self.fault is not None: return False
        assert uc.reg_read(c.UC_ARM64_REG_PC) == SENT and uc.reg_read(c.UC_ARM64_REG_SP) == STACK + 0x8000, '%#x did not return (spin?)' % fn
        for k in range(19, 30): assert uc.reg_read(getattr(c, 'UC_ARM64_REG_X%d' % k)) == 0x7777000000000000 + k, 'callee-saved x%d' % k
        return True

    def run(self, w):
        uc = self.uc
        uc.mem_write(GA, bytes(w.a))
        d = w.dev
        uc.mem_write(DEVA + D_FRAME, struct.pack('<Q', d['frame'])); uc.mem_write(DEVA + D_CTX, struct.pack('<Q', d['ctx']))
        uc.mem_write(DEVA + D_HEAD, struct.pack('<I', d['head'])); uc.mem_write(DEVA + D_LOCK, struct.pack('<I', d['lock']))
        uc.mem_write(DEVA + D_CPU, struct.pack('<Q', d['cpu'])); uc.mem_write(DEVA + D_GPU, struct.pack('<Q', d['gpu']))
        self.calls, self.fault, self.yields = [], None, 0
        ok = self.call(FN_G, GA + O_R) and self.call(FN_S, GA + O_R)
        s = min(d['head'] & 0xffffff00, RINGN); n = WIN if s + WIN <= RINGN else RINGN - s
        ring0, ring1 = bytes(uc.mem_read(RINGA, WIN)), bytes(uc.mem_read(RINGA + s, n))
        uc.mem_write(RINGA, b'\0' * WIN); uc.mem_write(RINGA + s, b'\0' * n)
        if not ok: return None
        assert self.yields == 0, 'YieldThread was called in a single-threaded case'
        dv = bytes(uc.mem_read(DEVA + D_FRAME, 8)) + bytes(uc.mem_read(DEVA + D_CTX, 8)) + bytes(uc.mem_read(DEVA + D_HEAD, 4)) + bytes(uc.mem_read(DEVA + D_LOCK, 4)) + \
            bytes(uc.mem_read(DEVA + D_CPU, 8)) + bytes(uc.mem_read(DEVA + D_GPU, 8))
        calls = [(x0, x1 & 0xffffffff, x2 & 0xffffffff, x3, x4) for x0, x1, x2, x3, x4 in self.calls]
        return calls, bytes(uc.mem_read(GA, A1)), dv, (zlib.crc32(ring0), zlib.crc32(ring1))


MUTATIONS = [
    ('uint32_t h = (nru_r32(D + NRU_D_HEAD) + 0xffu) & 0xffffff00u;', 'uint32_t h = (nru_r32(D + NRU_D_HEAD) + 0x100u) & 0xffffff00u;'),
    ('for (int k = 0; k < 3; k++) nru_put(s, cb, k_nru_stage[k], B);', 'for (int k = 0; k < 3; k++) nru_put(s, cb, k_nru_stage[2 - k], B);'),
    ('if (nru_r64(B + NRU_B_CPU) && B[NRU_B_SLOT] <= 7) nru_put', 'if (nru_r64(B + NRU_B_CPU) && B[NRU_B_SLOT] < 7) nru_put'),
    ('nru_w32(B + NRU_B_W28, 0); nru_w32(B + NRU_B_W2C, w);', 'nru_w32(B + NRU_B_W28, 0); nru_w32(B + NRU_B_W2C, 0);'),
    ('if (B[NRU_B_DIRTY] || nru_r64(B + NRU_B_STAMP) != nru_r64(D + NRU_D_FRAME)) {\n                nru_rebuild(s, b, D, 0);', 'if (B[NRU_B_DIRTY]) {\n                nru_rebuild(s, b, D, 0);'),
    ('nru_w64(B + NRU_B_GPU, gpu); nru_w64(B + NRU_B_CPU, cpu);', 'nru_w64(B + NRU_B_GPU, cpu); nru_w64(B + NRU_B_CPU, gpu);'),
    ('if (nru_r64(B + NRU_B_CPU) && B[NRU_B_SLOT] <= 7) nru_put', 'if (B[NRU_B_SLOT] <= 7) nru_put'),
    ('if (B[NRU_B_SLOT] <= 7 && (B[NRU_B_DIRTY]', 'if (B[NRU_B_SLOT] <= 8 && (B[NRU_B_DIRTY]'),
    ('if ((uint64_t)h + size > NRU_RING_LAST) h = 0;\n        __atomic_store_n', 'if ((uint64_t)h + size > NRU_RING_LAST + 0x100) h = 0;\n        __atomic_store_n'),
    ('for (int st = 0; st < 3; st++) for (unsigned i = 0; i < cnt[st]; i++) {   // phase 2', 'for (int st = 2; st >= 0; st--) for (unsigned i = 0; i < cnt[st]; i++) {   // phase 2'),
]


def mutations(a):
    import shutil
    here = os.path.dirname(os.path.abspath(__file__)); caught = 0
    for n, (old, new) in enumerate(MUTATIONS):
        with tempfile.TemporaryDirectory() as td:
            os.makedirs(os.path.join(td, 'a', 'b', 'c')); os.makedirs(os.path.join(td, 'nr'))
            h = header(); assert old in h, 'mutation %d does not apply' % n
            open(os.path.join(td, 'nr', 'nr_ubo.h'), 'w', newline='\n').write(h.replace(old, new, 1))
            src = os.path.join(td, 'a', 'b', 'c', 'nr_ubo_test.c'); shutil.copy(os.path.join(here, 'nr_ubo_test.c'), src)
            exe = os.path.join(td, 'm.exe')
            r = subprocess.run([a.cc, '-std=c11', '-O1', '-w', '-D_CRT_SECURE_NO_WARNINGS', src, '-o', exe], capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
            r = subprocess.run([sys.executable, os.path.abspath(__file__), '--main', a.main, '--exe', exe, '--cases', str(min(a.cases, 1500)), '--seed', str(a.seed + n)],
                               capture_output=True, text=True)
            ok = r.returncode == 1
            caught += ok
            print('mutation %d (%s -> %s): %s' % (n, old.split('\n')[0][:50], new.split('\n')[0][:50], 'caught' if ok else 'NOT caught (rc %d)' % r.returncode))
    print('%s: %d of %d mutations caught' % ('PASS' if caught == len(MUTATIONS) else 'FAIL', caught, len(MUTATIONS)))
    return 0 if caught == len(MUTATIONS) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--main', required=True); ap.add_argument('--exe'); ap.add_argument('--cases', type=int, default=4000)
    ap.add_argument('--seed', type=int, default=0x9e3779b9); ap.add_argument('--mutations', action='store_true'); ap.add_argument('--cc')
    a = ap.parse_args()
    if not os.path.exists(a.main): print('SKIP: no %s (the game file is never in the repository)' % a.main); return 77
    try:
        import unicorn, capstone  # noqa: F401
    except ImportError: print('SKIP: unicorn / capstone missing'); return 77
    main = open(a.main, 'rb').read()
    static_checks(main, header())
    print('static: 4 code hashes, main+%#x = nvnCommandBufferBindUniformBuffer by the loader, PLT YieldThread / GetCurrentThread, the draw functions call 0x926f90 then 0x925740' % SLOT_BIND)
    if a.mutations: return mutations(a)
    rng = random.Random(a.seed); emu = Emu(main)
    worlds, results = [], []
    for n in range(a.cases):
        w = World(rng); w.build(); worlds.append(w); results.append(emu.run(w))
    with tempfile.TemporaryDirectory() as td:
        fin, fout = os.path.join(td, 'in.bin'), os.path.join(td, 'out.bin')
        with open(fin, 'wb') as f:
            f.write(struct.pack('<II', 0x4255524e, len(worlds)))
            for w in worlds: f.write(bytes(w.a)); f.write(w.devbytes())
        r = subprocess.run([a.exe, '--oracle', fin, fout], capture_output=True, text=True)
        if r.returncode: print(r.stdout, r.stderr); return 1
        out = open(fout, 'rb').read()
    pos = 4; n = struct.unpack_from('<I', out, 0)[0]; assert n == len(worlds)
    bad = ran = calls = refused = 0; whys = {}; stages = {}; rebuilt = 0; faults = 0; flagged_ran = 0
    for k, w in enumerate(worlds):
        r_ran, r_why, r_n = struct.unpack_from('<III', out, pos); pos += 12
        mcalls = [struct.unpack_from('<5Q', out, pos + 40 * i) for i in range(r_n)]; pos += 40 * r_n
        ma1 = out[pos:pos + A1]; pos += A1
        mdv = out[pos:pos + 40]; pos += 40
        mcrc = struct.unpack_from('<II', out, pos); pos += 8
        res = results[k]
        if res is None: faults += 1
        if not r_ran:
            refused += 1; whys[r_why] = whys.get(r_why, 0) + 1
            if res is not None and not w.flags:
                bad += 1
                if bad <= 10: print('FAIL case %d: the model refused (why %d) a clean case the machine code ran' % (k, r_why))
            continue
        if res is None:
            bad += 1
            if bad <= 10: print('FAIL case %d: the machine code faulted at %s, the model ran (flags %s)' % (k, emu.fault, sorted(w.flags)))
            continue
        ecalls, ea1, edv, ecrc = res
        ran += 1; calls += len(ecalls); flagged_ran += bool(w.flags)
        for c in ecalls: stages[c[1]] = stages.get(c[1], 0) + 1
        mc = [tuple(c) for c in mcalls]
        if ecalls != mc:
            bad += 1
            if bad <= 10:
                d = next((i for i in range(min(len(ecalls), len(mc))) if ecalls[i] != mc[i]), min(len(ecalls), len(mc)))
                print('FAIL case %d: calls differ at %d (%d machine, %d model): machine %s / model %s' % (k, d, len(ecalls), len(mc),
                      [hex(x) for x in ecalls[d]] if d < len(ecalls) else None, [hex(x) for x in mc[d]] if d < len(mc) else None))
            continue
        if ea1 != ma1:
            bad += 1
            if bad <= 10:
                d = next(i for i in range(A1) if ea1[i] != ma1[i])
                print('FAIL case %d: arena differs at +%#x (machine %02x, model %02x)' % (k, d, ea1[d], ma1[d]))
            continue
        if edv != mdv:
            bad += 1
            if bad <= 10: print('FAIL case %d: device fields differ: machine %s / model %s' % (k, edv.hex(), mdv.hex()))
            continue
        if ecrc != mcrc:
            bad += 1
            if bad <= 10: print('FAIL case %d: ring windows differ (crc machine %08x %08x, model %08x %08x)' % (k, ecrc[0], ecrc[1], mcrc[0], mcrc[1]))
            continue
        rebuilt += struct.unpack_from('<I', edv, 16)[0] != w.dev['head']
    print('cases %d: model ran %d (%d with a flagged feature that did not matter; %d BindUniformBuffer calls: stage 0 %d, 1 %d, 5 %d; head moved in %d), refused %d (%s), '
          'machine code faulted %d' % (len(worlds), ran, flagged_ran, calls, stages.get(0, 0), stages.get(1, 0), stages.get(5, 0), rebuilt, refused,
                                       ', '.join('why %d: %d' % (k2, v) for k2, v in sorted(whys.items())), faults))
    if ran < len(worlds) // 2 or not all(stages.get(s, 0) for s in (0, 1, 5)): print('FAIL: coverage'); bad += 1
    print('PASS' if not bad else 'FAIL (%d)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
