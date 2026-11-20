#!/usr/bin/env python3
# NR-NATIVE step D half c (DM_NRN_PKT): the packet models of dm/gfx/nr/nr_pkt.h against the game's own machine code.
#
# ExecuteRenderCommand main+0x93e5f0 is run from the user's own main.raw (update v786432, build 4979b200; never in the repository) in
# the Unicorn emulator with the opcodes the models stand for - 0x0f (texture: 0x9243d0), 0x1c (parameter: 0x924bb0), 0x11, 0x39,
# 0x3a - on random renderer states, packets, shader descriptions, texture records and shadow blocks. The same states go through the
# model (nr_pkt_test --oracle) and the two must agree in every byte of guest memory afterwards and in the length the handler
# returns (the list walker advances by it). The emulator also proves what the model assumes about the rest: no byte outside the
# arena and the stack is written and no NVN pointer is called.
# It also prints / checks the code hashes of NRPK_CODE_TABLE (the front end refuses to arm DM_NRN_PKT on any other code).
#
#   python nr_pkt_oracle.py --main <main.raw> --exe <nr_pkt_test executable> [--cases 4000] [--wsl]
# Exit 0 = PASS, 1 = FAIL, 77 = skipped (no main.raw / no unicorn).
import argparse, os, random, re, struct, subprocess, sys, tempfile

GA, ARENA, STACK, SENT = 0x10000000, 0x10000, 0x40000000, 0x50000000
RC, P15, P28, GDEF = 0x93e5f0, 0x9243d0, 0x924bb0, 0x2bbda90
A_R, A_LIST, A_PKT, A_DESC, A_RECP, A_TREC, A_PAGES, A_PAGE0, A_BLK, A_DATA = 0, 0x2000, 0x3000, 0x4000, 0x4100, 0x4200, 0x5000, 0x5100, 0x6000, 0x7000
TEX, SMP = (0x1558, 0x15e0, 0x1668), (0x13c0, 0x1448, 0x14d0)
M64 = (1 << 64) - 1
CODE = [(P15, 0x924730, 'packet 0x0f handler 0x9243d0'), (P28, 0x925130, 'packet 0x1c handler 0x924bb0'),
        (0xa49c30, 0xa49ed4, 'the list walker 0xa49c30 (the only caller of ExecuteRenderCommand)')]


def fnv(b):
    h = 0xcbf29ce484222325
    for c in b: h = ((h ^ c) * 0x100000001b3) & M64
    return h


def make_case(rng):
    a = bytearray(rng.getrandbits(8 * ARENA).to_bytes(ARENA, 'little'))
    def w64(o, v): struct.pack_into('<Q', a, o, v & M64)
    def w32(o, v): struct.pack_into('<I', a, o, v & 0xffffffff)
    def w16(o, v): struct.pack_into('<H', a, o, v & 0xffff)
    op = rng.choice((0x0f, 0x0f, 0x0f, 0x1c, 0x1c, 0x1c, 0x11, 0x39, 0x3a))
    w64(A_LIST + 0x38, GA + A_R)
    a[A_PKT] = op
    per = rng.choice((0, 1, 3, 16)); gdef = rng.randrange(max(1, per) * 4)
    w32(A_R + 0x550, per); w64(A_R + 0x540, GA + A_PAGES)
    for p in range(4): w64(A_PAGES + 8 * p, GA + A_PAGE0 + 0x200 * p)
    for st in range(3):   # ids the tables hold: small values so that "the same id" happens; ranges anywhere
        for i in range(32):
            w32(A_R + TEX[st] + 4 * i, rng.randrange(6) - 1); w32(A_R + SMP[st] + 4 * i, rng.randrange(6) - 1)
    if op == 0x0f:
        w64(A_PKT + 8, 0 if rng.randrange(12) == 0 else GA + A_DESC); w64(A_PKT + 0x10, 0 if rng.randrange(12) == 0 else GA + A_RECP)
        w32(A_PKT + 0x18, rng.choice((-1, -1, 0, 1, 2, 3)))
        for k in range(3): a[A_DESC + k] = rng.choice((0xff, 0x80, rng.randrange(32), rng.randrange(32), rng.randrange(8)))
        w32(A_DESC + 4, rng.choice((-1, -5, rng.randrange(5))))
        w64(A_RECP, GA + A_TREC)
        w32(A_TREC + 0xc0, rng.randrange(5))
        for v in range(4): w32(A_TREC + 0xc0 + 4 * (v + 1), rng.choice((-1, rng.randrange(5))))
        w32(A_TREC + 0x100, rng.choice((-1, -7, rng.randrange(5))))
        for p in range(4):
            for e in range(16): w32(A_PAGE0 + 0x200 * p + 0x20 * e + 0x10, rng.randrange(5))
    elif op == 0x1c:
        n = rng.choice((0, 1, 2, 3, 4, 7, 8, 12, 16, 31, 32, 33, 48, 63, 64, 65, 100, 192, 600))
        w32(A_PKT + 0x28, n)
        alias = rng.randrange(10) == 0
        for k in range(3):
            blk = A_BLK + 0x100 * (0 if alias and k == 2 else k)
            w64(A_PKT + 8 + 8 * k, 0 if rng.randrange(5) == 0 else GA + blk)
            off = rng.choice((0xffff, rng.randrange(0, 0x400, 4), rng.randrange(0x400)))
            w16(A_PKT + 0x20 + 2 * k, off)
            w64(A_BLK + 0x100 * k + 0x20, GA + A_DATA + 0x1000 * k); w32(A_BLK + 0x100 * k + 0x28, rng.choice((0, rng.randrange(0x800), 0x7ff)))
            a[A_BLK + 0x100 * k + 0x34] = rng.randrange(2)
            if off != 0xffff and rng.randrange(3):   # the block already holds the values (all of them, or all but one byte)
                a[A_DATA + 0x1000 * k + off:A_DATA + 0x1000 * k + off + n] = a[A_PKT + 0x30:A_PKT + 0x30 + n]
                if n and rng.randrange(2): a[A_DATA + 0x1000 * k + off + rng.randrange(n)] ^= 1 << rng.randrange(8)
    return dict(op=op, gdef=gdef, arena=a)


class Emu:
    def __init__(self, main):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_INVALID
        self.uc = uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        size = 0x3000000
        uc.mem_map(0, size); uc.mem_write(0, main[:size])
        uc.mem_map(GA, ARENA); uc.mem_map(STACK, 0x10000); uc.mem_map(SENT, 0x1000); uc.mem_write(SENT, struct.pack('<I', 0xd65f03c0))
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write); uc.hook_add(UC_HOOK_MEM_INVALID, self.on_bad)
        self.stray, self.fault = [], None

    def on_write(self, uc, access, addr, size, value, user):
        if not (GA <= addr < GA + ARENA or STACK <= addr < STACK + 0x10000): self.stray.append(addr)

    def on_bad(self, uc, access, addr, size, value, user):
        self.fault = addr; return False

    def run(self, case):
        from unicorn import arm64_const as c, UcError
        uc = self.uc
        uc.mem_write(GA, bytes(case['arena'])); uc.mem_write(GDEF, struct.pack('<I', case['gdef'])); self.stray, self.fault = [], None
        for k in range(31): uc.reg_write(getattr(c, 'UC_ARM64_REG_X%d' % k), 0x7777000000000000 + k)
        uc.reg_write(c.UC_ARM64_REG_SP, STACK + 0x8000); uc.reg_write(c.UC_ARM64_REG_X30, SENT)
        uc.reg_write(c.UC_ARM64_REG_X0, GA + A_LIST); uc.reg_write(c.UC_ARM64_REG_X1, case['op']); uc.reg_write(c.UC_ARM64_REG_X2, GA + A_PKT)
        try: uc.emu_start(RC, SENT, count=400000)
        except UcError as e: self.fault = self.fault if self.fault is not None else str(e)
        assert self.fault is None, 'the machine code faulted at %s (opcode %#x)' % (self.fault if isinstance(self.fault, str) else hex(self.fault), case['op'])
        assert uc.reg_read(c.UC_ARM64_REG_PC) == SENT and uc.reg_read(c.UC_ARM64_REG_SP) == STACK + 0x8000, 'the function did not return'
        assert not self.stray, 'opcode %#x wrote outside the renderer / the blocks / the stack: %s' % (case['op'], ', '.join(hex(x) for x in self.stray[:4]))
        return uc.reg_read(c.UC_ARM64_REG_X0), bytes(uc.mem_read(GA, ARENA))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--main'); ap.add_argument('--exe'); ap.add_argument('--cases', type=int, default=4000); ap.add_argument('--seed', type=int, default=20261120)
    ap.add_argument('--wsl', action='store_true', help='the executable is a Linux one: run it through wsl')
    a = ap.parse_args()
    if not a.main or not os.path.exists(a.main): print('SKIP: no main.raw'); return 77
    try: import unicorn  # noqa
    except ImportError: print('SKIP: no unicorn'); return 77
    main_raw = open(a.main, 'rb').read()
    header = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'nr', 'nr_pkt.h'), encoding='utf-8').read()
    front = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'host', 'hle_dos2_nrn_c.inc'), encoding='utf-8').read()
    for off, end, name in CODE:
        h = fnv(main_raw[off:end]); row = '{%#x, %#x, 0x%016xull, "%s"}' % (off, end, h, name)
        print('code: ' + row)
        assert row in front, 'NRPK_CODE_TABLE of hle_dos2_nrn_c.inc lacks the row ' + row
    assert 'NRPK_GDEF = 0x2bbda90' in header
    # the walker: ExecuteRenderCommand has exactly one caller in the text (a bl inside 0xa49c30) and no pointer to it in the data
    text = main_raw[:0x1e80000]; callers = []
    for at in range(0, len(text) - 3, 4):
        w = struct.unpack_from('<I', text, at)[0]
        if w >> 26 in (0x25, 0x05):
            imm = w & 0x3ffffff; imm -= (imm & 0x2000000) << 1
            if at + 4 * imm == RC: callers.append(at)
    assert callers == [0xa49cb0], 'callers of ExecuteRenderCommand: %s' % [hex(x) for x in callers]
    assert struct.pack('<Q', RC) not in main_raw, 'a pointer to ExecuteRenderCommand in the image'
    rng = random.Random(a.seed); emu = Emu(main_raw)
    cases = [make_case(rng) for _ in range(a.cases)]
    with tempfile.TemporaryDirectory() as d:
        fc, fe = os.path.join(d, 'cases.bin'), os.path.join(d, 'expect.bin')
        with open(fc, 'wb') as f:
            f.write(struct.pack('<I', len(cases)))
            for c in cases: f.write(struct.pack('<IIII', c['op'], A_PKT, c['gdef'], 0)); f.write(bytes(c['arena']))
        per_op = {}
        with open(fe, 'wb') as f:
            for c in cases:
                ret, mem = emu.run(c); per_op[c['op']] = per_op.get(c['op'], 0) + (mem != bytes(c['arena']))
                f.write(struct.pack('<Q', ret)); f.write(mem)
        assert per_op.get(0x11, 0) == 0 and per_op.get(0x39, 0) == 0 and per_op.get(0x3a, 0) == 0, 'opcode 0x11 / 0x39 / 0x3a changed guest memory'
        assert per_op.get(0x0f, 0) > a.cases // 20 and per_op.get(0x1c, 0) > a.cases // 20, 'coverage: too few cases changed memory'
        def wp(p):
            p = os.path.abspath(p).replace('\\', '/')
            return '/mnt/' + p[0].lower() + p[2:] if a.wsl else p
        cmd = (['wsl', '-e'] if a.wsl else []) + [a.exe if a.wsl else os.path.abspath(a.exe), '--oracle', wp(fc), wp(fe), hex(GA)]
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        sys.stdout.write(r.stdout[-2000:])
        if r.returncode != 0: print('FAIL: the model differs from the machine code (%d cases)' % a.cases); return 1
    print('PASS: %d cases - ExecuteRenderCommand 0x93e5f0 with opcodes 0x0f / 0x1c / 0x11 / 0x39 / 0x3a in the emulator and the model leave the same bytes and '
          'return the same length; cases that changed memory: 0x0f %d, 0x1c %d; nothing written outside the arena' % (a.cases, per_op.get(0x0f, 0), per_op.get(0x1c, 0)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
