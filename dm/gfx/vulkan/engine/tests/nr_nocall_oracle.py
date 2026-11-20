#!/usr/bin/env python3
# NR-23 (DM_NR_NOCALL): the model of dm/gfx/nr/nr_nocall.h against the game's own machine code.
#
# The three functions the model stands for - the texture flush main+0x924730 and the draw functions main+0x928e80 / +0x928fb0 - are run
# from the user's own main.raw (update v786432, build 4979b200; never in the repository) in the Unicorn emulator on random renderer
# states. The engine's NVN pointers point at stubs that log every call with its arguments; the two uniform-block functions are stubbed
# (the model does not stand for them; the front end calls the engine's). The same states go through the model (nr_nocall_test --oracle)
# and the two results must be identical: the NVN calls in order with every argument, and every byte of guest memory afterwards.
# It also checks what the front end relies on: the code hashes of NRNC_CODE_TABLE, the five NVN pointer slots (names from the engine's
# loader), the index size table, the return addresses of the two flush calls, and - with --nvn-entry-obj - the machine code of
# nvn_on_bigstack (nvn_entry.S) in the emulator: the call runs on the other stack, the caller's stack and frame come back.
#
#   python nr_nocall_oracle.py --main <main.raw> --exe <nr_nocall_test executable> [--cases 3000] [--nvn-entry-obj <nvn_entry.S.o> --objdump <llvm-objdump>]
# Exit 0 = PASS, 1 = FAIL, 77 = skipped (no main.raw / no unicorn / no capstone).
import argparse, os, random, re, struct, subprocess, sys, tempfile

GA, ARENA = 0x10000000, 0x10000
A_R, A_DEV, A_CTX1, A_CTX2, A_PAGES, A_GENS, A_GENPG, A_RECPG = 0, 0x2000, 0x9000, 0x9800, 0xa000, 0xa100, 0xa200, 0xb000
STUB, STACK, SENT = 0x30000000, 0x40000000, 0x50000000
FLUSH, DRAW, DRAWI, UBO1, UBO2, ISZ = 0x924730, 0x928e80, 0x928fb0, 0x926f90, 0x925740, 0x1f5865c
SLOTS = [(0x2bbeee8, 'nvnDeviceGetTextureHandle'), (0x2bbfab0, 'nvnCommandBufferBindTexture'), (0x2bbfb20, 'nvnCommandBufferDrawElements'),
         (0x2bbfb38, 'nvnCommandBufferDrawElementsInstanced'), (0x2bbfcd0, 'nvnCommandBufferSetRenderEnable')]
STAGES = [(0, 0x1558, 0x13c0, 0x15dc, 0x1444), (1, 0x15e0, 0x1448, 0x1664, 0x14cc), (5, 0x1668, 0x14d0, 0x16ec, 0x1554)]
M64 = (1 << 64) - 1


def sx32(v): v &= 0xffffffff; return v - (1 << 32) if v & 0x80000000 else v
def u64(v): return v & M64


def fnv(b):
    h = 0xcbf29ce484222325
    for c in b: h = ((h ^ c) * 0x100000001b3) & M64
    return h


def static_checks(main, header):
    code = re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)ull, "([^"]+)"\}', header)
    assert len(code) == 3, 'NRNC_CODE_TABLE has three rows'
    for off, end, h, name in code:
        off, end, h = int(off, 16), int(end, 16), int(h, 16)
        got = fnv(main[off + 16:end])
        assert got == h, 'hash of %s (main+%#x..%#x) is %#x, the header says %#x' % (name, off, end, got, h)
    # the engine's loader (main+0x934f00...): which NVN name each pointer slot gets
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    pg, x1, names = {}, None, {}
    for i in md.disasm(main[0x934000:0x939000], 0x934000):
        if i.mnemonic == 'adrp': r, v = i.op_str.split(', #'); pg[r] = int(v, 16)
        elif i.mnemonic == 'add' and i.op_str.startswith('x1, x1, #'): x1 = pg.get('x1', 0) + int(i.op_str.split('#')[1], 16)
        elif i.mnemonic == 'str' and i.op_str.startswith('x0, [x8, #') and x1:
            names[pg.get('x8', 0) + int(i.op_str.split('#')[1].rstrip(']'), 16)] = main[x1:main.index(b'\0', x1)].decode('latin1'); x1 = None
    for slot, name in SLOTS: assert names.get(slot) == name, 'slot main+%#x is %s, expected %s' % (slot, names.get(slot), name)
    hs = dict(re.findall(r'NRNC_SLOT_(\w+) = (0x[0-9a-f]+)', header))
    assert [int(hs[k], 16) for k in ('HANDLE', 'BIND', 'DRAW', 'DRAWI', 'REN')] == [s for s, _ in SLOTS], 'NRNC_SLOT_* of the header'
    # the two flush calls of the draw functions and their return addresses
    for bl, lr in ((0x928f0c, 'NRNC_LR_DRAW = 0x928f10'), (0x92903c, 'NRNC_LR_DRAWI = 0x929040')):
        w = struct.unpack_from('<I', main, bl)[0]; o = w & 0x3ffffff
        assert w >> 26 == 0x25 and bl + ((o - (1 << 26) if o & (1 << 25) else o) << 2) == FLUSH and lr in header, 'bl 0x924730 at main+%#x' % bl
    assert struct.unpack_from('<8i', main, ISZ) == (1, 2, 4, 4, 4, 4, 2, 2) and 'NRNC_ISZ_TABLE = 0x1f5865c' in header, 'index size table'
    return len(names)


def make_case(rng, kind):
    a = bytearray(rng.getrandbits(8 * ARENA).to_bytes(ARENA, 'little'))
    def w64(o, v): struct.pack_into('<Q', a, o, v & M64)
    def w32(o, v): struct.pack_into('<I', a, o, v & 0xffffffff)
    w64(A_R + 0xb70, GA + A_DEV); w64(A_DEV + 0x5248, GA + A_CTX1)
    elig = True
    for _, tex, smp, rg, _s in STAGES:
        m = rng.randrange(8)
        lo, hi = sorted((rng.randrange(32), rng.randrange(32)))
        if m == 0: lo, hi = 0x20, 0xff
        elif m == 1: lo, hi = rng.randrange(32), rng.randrange(32)        # may be empty (lo > hi)
        elif m == 2 and kind == 0 and rng.randrange(4) == 0: hi = 32 + rng.randrange(90); elig = False   # past the slots: left to the engine
        a[A_R + rg], a[A_R + rg + 1] = lo & 0xff, hi & 0xff
        for i in range(32):
            if rng.randrange(3) == 0: w32(A_R + tex + 4 * i, 0xffffffff)
            if rng.randrange(5) == 0: w32(A_R + smp + 4 * i, rng.randrange(1 << 12))
            if rng.randrange(4) == 0: w32(A_R + tex + 4 * i, rng.randrange(1 << 20))
    per, page = 1 + rng.randrange(16), rng.randrange(4); off = rng.randrange(per); idx = page * per + off
    gen = 1 + rng.randrange(0x3ffffe)
    w32(A_R + 0x250, per); w64(A_R + 0x240, GA + A_PAGES); w64(A_R + 0x248, GA + A_GENS)
    for p in range(4): w64(A_PAGES + 8 * p, GA + A_RECPG + 0x800 * p); w64(A_GENS + 8 * p, GA + A_GENPG + 0x100 * p)
    bad_ib = kind != 0 and rng.randrange(12) == 0
    w32(A_GENPG + 0x100 * page + 4 * off, gen ^ 1 if bad_ib else gen)
    w64(A_R + 0x17c8, rng.getrandbits(10) << 54 | gen << 32 | idx)
    w32(A_RECPG + 0x800 * page + 0x58 * off + 0x24, rng.randrange(8))
    a[A_R + 0x185c] = rng.choice((0, 0, 1, 1, rng.randrange(256))); a[A_R + 0x185d] = rng.choice((0, 0, 1, 1, rng.randrange(256)))
    if rng.randrange(2): w64(A_R + 0x17e0, 0)
    if kind != 0 and bad_ib: elig = False
    args = (sx32(rng.getrandbits(32)), sx32(rng.getrandbits(32)), sx32(rng.getrandbits(32)))
    return dict(kind=kind, a1=args[0], a2=args[1], a4=args[2], swap=rng.randrange(2), arena=a, elig=elig)


class Emu:
    def __init__(self, main):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
        self.uc = uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x924000, 0x6000); uc.mem_write(0x924000, main[0x924000:0x92a000])
        uc.mem_map(0x1f58000, 0x1000); uc.mem_write(0x1f58000, main[0x1f58000:0x1f59000])
        uc.mem_map(0x2bbe000, 0x2000)
        for i, (slot, _) in enumerate(SLOTS): uc.mem_write(slot, struct.pack('<Q', STUB + 4 * i))
        ret = struct.pack('<I', 0xd65f03c0)
        uc.mem_map(STUB, 0x1000); uc.mem_write(STUB, ret * 16)
        uc.mem_write(UBO1, ret); uc.mem_write(UBO2, ret)          # the uniform-block functions: stubs that return at once
        uc.mem_map(GA, ARENA); uc.mem_map(STACK, 0x10000); uc.mem_map(SENT, 0x1000); uc.mem_write(SENT, ret)
        uc.hook_add(UC_HOOK_CODE, self.on_stub, begin=STUB, end=STUB + 4 * len(SLOTS) - 1)
        uc.hook_add(UC_HOOK_CODE, self.on_ubo, begin=UBO2, end=UBO2)
        uc.hook_add(UC_HOOK_CODE, self.on_ubo, begin=UBO1, end=UBO1)
        uc.hook_add(UC_HOOK_MEM_INVALID, self.on_bad)
        self.calls, self.fault, self.swap = [], None, 0

    def reg(self, n):
        from unicorn import arm64_const as c
        return self.uc.reg_read(getattr(c, 'UC_ARM64_REG_X%d' % n))

    def on_stub(self, uc, addr, size, user):
        from unicorn import arm64_const as c
        i = (addr - STUB) // 4; x = [self.reg(k) for k in range(8)]
        if i == 0: uc.reg_write(c.UC_ARM64_REG_X0, u64(sx32(x[1]) | (sx32(x[2]) << 20)))   # hle_nvn.c nvnDeviceGetTextureHandle
        self.calls.append((i, x))

    def on_ubo(self, uc, addr, size, user):
        self.calls.append((5 if addr == UBO1 else 6, [self.reg(0)]))
        if addr == UBO1 and self.swap: uc.mem_write(GA + A_DEV + 0x5248, struct.pack('<Q', GA + A_CTX2))

    def on_bad(self, uc, access, addr, size, value, user):
        self.fault = addr; return False

    def run(self, case):
        from unicorn import arm64_const as c, UcError
        uc = self.uc
        uc.mem_write(GA, bytes(case['arena'])); self.calls, self.fault, self.swap = [], None, case['swap']
        for k in range(31): uc.reg_write(getattr(c, 'UC_ARM64_REG_X%d' % k), 0x7777000000000000 + k)   # junk in every register
        uc.reg_write(c.UC_ARM64_REG_SP, STACK + 0x8000); uc.reg_write(c.UC_ARM64_REG_X30, SENT)
        uc.reg_write(c.UC_ARM64_REG_X0, GA + A_R)
        fn = FLUSH
        if case['kind'] == 36: fn = DRAW; uc.reg_write(c.UC_ARM64_REG_X1, case['a1'] & 0xffffffff); uc.reg_write(c.UC_ARM64_REG_X2, case['a2'] & 0xffffffff)
        if case['kind'] == 37: fn = DRAWI; uc.reg_write(c.UC_ARM64_REG_X2, case['a2'] & 0xffffffff); uc.reg_write(c.UC_ARM64_REG_X4, case['a4'] & 0xffffffff)
        try: uc.emu_start(fn, SENT, count=200000)
        except UcError as e: self.fault = self.fault if self.fault is not None else str(e)
        assert self.fault is None, 'the machine code faulted at %s (kind %d)' % (self.fault if isinstance(self.fault, str) else hex(self.fault), case['kind'])
        assert uc.reg_read(c.UC_ARM64_REG_PC) == SENT and uc.reg_read(c.UC_ARM64_REG_SP) == STACK + 0x8000, 'the function did not return'
        for k in range(19, 30): assert uc.reg_read(getattr(c, 'UC_ARM64_REG_X%d' % k)) == 0x7777000000000000 + k, 'callee-saved x%d' % k
        # the log as the model's calls
        out, i, dev8 = [], 0, GA + A_DEV + 8
        while i < len(self.calls):
            k, x = self.calls[i]
            if k == 0:
                assert i + 1 < len(self.calls) and self.calls[i + 1][0] == 1, 'GetTextureHandle not followed by BindTexture'
                y = self.calls[i + 1][1]
                assert x[0] == dev8 and y[3] == u64(sx32(x[1]) | (sx32(x[2]) << 20)), 'handle argument'
                out.append((1, y[0], y[1] & 0xffffffff, y[2] & 0xffffffff, u64(sx32(x[1])), u64(sx32(x[2])), 0, 0)); i += 2; continue
            assert k != 1, 'BindTexture without GetTextureHandle'
            if k == 4: assert x[1] & 0xffffffff == 1; out.append((2, x[0], 1, 0, 0, 0, 0, 0))
            elif k == 2: out.append((3, x[0], u64(sx32(x[1])), x[2] & 0xffffffff, u64(sx32(x[3])), x[4], u64(-1), 0))
            elif k == 3:
                assert x[5] & 0xffffffff == 0 and x[6] & 0xffffffff == 0, 'base vertex / base instance'
                out.append((3, x[0], u64(sx32(x[1])), x[2] & 0xffffffff, u64(sx32(x[3])), x[4], u64(sx32(x[7])), 0))
            else: out.append((4 if k == 5 else 5, x[0], 0, 0, 0, 0, 0, 0))
            i += 1
        return out, bytes(uc.mem_read(GA, ARENA))


def bigstack_check(obj, objdump):
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, arm64_const as c
    text = subprocess.run([objdump, '-d', '--disassemble-symbols=nvn_on_bigstack', obj], capture_output=True, text=True, check=True).stdout
    words = [int(m.group(1), 16) for m in re.finditer(r'^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s', text, re.M)]
    assert 10 <= len(words) <= 24 and words[-1] == 0xd65f03c0, 'nvn_on_bigstack not found in %s' % obj
    bl = [i for i, w in enumerate(words) if w >> 26 == 0x25]
    assert len(bl) == 1, 'one bl (nvn_bigstack_top)'
    CODE, TOPFN, FN, BIG = 0x60000000, 0x60001000, 0x60002000, 0x70000000
    words[bl[0]] = 0x94000000 | ((TOPFN - (CODE + 4 * bl[0])) >> 2)
    ret = struct.pack('<I', 0xd65f03c0)
    for top in (BIG + 0x10000, 0):   # the big stack is elsewhere / the caller already runs on it
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(CODE, 0x3000); uc.mem_write(CODE, b''.join(struct.pack('<I', w) for w in words)); uc.mem_write(TOPFN, ret); uc.mem_write(FN, ret)
        uc.mem_map(STACK, 0x10000); uc.mem_map(BIG, 0x11000); uc.mem_map(SENT, 0x1000); uc.mem_write(SENT, ret)
        seen = {}
        def on_top(u, addr, size, user):
            seen['top_arg'] = u.reg_read(c.UC_ARM64_REG_X0); u.reg_write(c.UC_ARM64_REG_X0, top)
            for k in range(1, 18): u.reg_write(getattr(c, 'UC_ARM64_REG_X%d' % k), 0xdead0000 + k)    # a C function clobbers these
        def on_fn(u, addr, size, user):
            seen['fn'] = seen.get('fn', 0) + 1; seen['sp'] = u.reg_read(c.UC_ARM64_REG_SP); seen['arg'] = u.reg_read(c.UC_ARM64_REG_X0)
            u.reg_write(c.UC_ARM64_REG_X0, 0xabcdef0123)
            for k in range(1, 18): u.reg_write(getattr(c, 'UC_ARM64_REG_X%d' % k), 0xbeef0000 + k)
        uc.hook_add(UC_HOOK_CODE, on_top, begin=TOPFN, end=TOPFN); uc.hook_add(UC_HOOK_CODE, on_fn, begin=FN, end=FN)
        sp0 = STACK + 0x8000
        uc.reg_write(c.UC_ARM64_REG_SP, sp0); uc.reg_write(c.UC_ARM64_REG_X29, 0x1111222233334444); uc.reg_write(c.UC_ARM64_REG_X30, SENT)
        uc.reg_write(c.UC_ARM64_REG_X0, FN); uc.reg_write(c.UC_ARM64_REG_X1, 0x5151515151)
        uc.emu_start(CODE, SENT, count=1000)
        assert uc.reg_read(c.UC_ARM64_REG_PC) == SENT, 'nvn_on_bigstack did not return'
        assert seen.get('fn') == 1 and seen['arg'] == 0x5151515151, 'fn(arg) called once'
        assert seen['top_arg'] == sp0 - 32, 'nvn_bigstack_top gets the stack pointer'
        assert seen['sp'] == (top if top else sp0 - 32), 'fn runs on %s' % ('the big stack' if top else 'the stack it was called on')
        assert seen['sp'] % 16 == 0, 'stack alignment'
        assert uc.reg_read(c.UC_ARM64_REG_SP) == sp0 and uc.reg_read(c.UC_ARM64_REG_X29) == 0x1111222233334444, 'the stack and frame of the caller come back'
        assert uc.reg_read(c.UC_ARM64_REG_X0) == 0xabcdef0123, 'the return value of fn'
    return len(words)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--main', default=os.environ.get('DM_DOS2_MAIN', ''))
    ap.add_argument('--exe', default='')
    ap.add_argument('--cases', type=int, default=3000)
    ap.add_argument('--seed', type=int, default=23)
    ap.add_argument('--nvn-entry-obj', default=''); ap.add_argument('--objdump', default='llvm-objdump')
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    header = open(os.path.join(here, '..', '..', '..', 'nr', 'nr_nocall.h'), encoding='utf-8').read()
    try:
        import unicorn, capstone  # noqa: F401
    except ImportError as e:
        print('SKIP: nr_nocall_oracle: %s' % e); return 77
    big = ''
    if a.nvn_entry_obj:
        n = bigstack_check(a.nvn_entry_obj, a.objdump)
        big = '; nvn_on_bigstack (%d instructions of %s) switches to the big stack / stays in place and restores the caller' % (n, os.path.basename(a.nvn_entry_obj))
    if not a.main or not os.path.isfile(a.main):
        print('SKIP: nr_nocall_oracle: no main.raw (--main or DM_DOS2_MAIN)%s' % big); return 77
    data = open(a.main, 'rb').read()
    nslots = static_checks(data, header)
    if not a.exe or not os.path.isfile(a.exe):
        print('SKIP: nr_nocall_oracle: static checks pass (3 code hashes, %d NVN slots named), no --exe to run the model%s' % (nslots, big)); return 77
    rng = random.Random(a.seed)
    cases = [make_case(rng, (0, 36, 37)[i % 3]) for i in range(a.cases)]
    with tempfile.TemporaryDirectory() as d:
        fin, fout = os.path.join(d, 'in.bin'), os.path.join(d, 'out.bin')
        with open(fin, 'wb') as f:
            f.write(struct.pack('<II', 0x434e524e, len(cases))); f.write(data[ISZ:ISZ + 32])
            for c in cases: f.write(struct.pack('<8i', c['kind'], c['a1'], c['a2'], c['a4'], c['swap'], 0, 0, 0)); f.write(bytes(c['arena']))
        subprocess.run([a.exe, '--oracle', fin, fout], check=True, stdout=subprocess.DEVNULL)
        res = open(fout, 'rb').read()
    emu = Emu(data)
    pos = 4; assert struct.unpack_from('<I', res, 0)[0] == len(cases)
    ran = left = ncalls = nbind = nren = 0
    for i, c in enumerate(cases):
        r, n = struct.unpack_from('<II', res, pos); pos += 8
        calls = [struct.unpack_from('<8Q', res, pos + 64 * k) for k in range(n)]; pos += 64 * n
        arena = res[pos:pos + ARENA]; pos += ARENA
        assert bool(r) == c['elig'], 'case %d (kind %d): the model %s, expected %s' % (i, c['kind'], 'ran' if r else 'left it to the engine', 'eligible' if c['elig'] else 'not eligible')
        if not r:
            assert arena == bytes(c['arena']), 'case %d: a case left to the engine was written to' % i
            left += 1; continue
        want, mem = emu.run(c)
        assert list(map(tuple, calls)) == want, 'case %d (kind %d): NVN calls differ\n  machine code: %s\n  model:        %s' % (i, c['kind'], want, calls)
        if mem != arena:
            bad = [hex(k) for k in range(ARENA) if mem[k] != arena[k]][:8]
            raise AssertionError('case %d (kind %d): guest memory differs at arena offsets %s' % (i, c['kind'], bad))
        ran += 1; ncalls += len(want); nbind += sum(1 for w in want if w[0] == 1); nren += sum(1 for w in want if w[0] == 2)
    assert pos == len(res)
    print('PASS: nr_nocall_oracle: %d states run through the machine code of main+0x924730 / 0x928e80 / 0x928fb0 and through the model: the same %d NVN calls '
          '(%d BindTexture, %d SetRenderEnable) with every argument, the same guest memory byte for byte; %d states the model leaves to the engine untouched; '
          '3 code hashes, %d NVN slots named%s' % (ran, ncalls, nbind, nren, left, nslots, big))
    return 0


if __name__ == '__main__':
    try: sys.exit(main())
    except AssertionError as e:
        print('FAIL: nr_nocall_oracle: %s' % e); sys.exit(1)
