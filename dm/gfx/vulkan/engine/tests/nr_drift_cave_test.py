#!/usr/bin/env python3
# [DOS2] NR-DRIFT DM_DOS2_DRS_SPIKE (docs/dos2/nr/NR-DRIFT.md section 8): the machine-code cave of dm/gfx/nr/nr_drift.h (nrd_spike_cave)
# run in Unicorn against its C model nrd_spike_step (nr_drift_test --spike, when --exe is given) and a Python copy of it, on random
# sample sequences in every mode. Per call: s16 (the GPU ms the controller then uses) bit for bit, the page counters / history, and the
# four instructions the cave moved (x9 = x20 + 0x28, s0 = [x20 + 0x28], w11 / w12 = [x20 + 8] / [x20 + 0xc]); every other general and
# vector register must be unchanged except x16, x17, s22..s25 (and the flags). With --main the four replaced words at main+0xc05dcc are
# checked against nrd_spike_orig and, in mode 0, the cave's results equal the original four instructions run from main.raw.
#   python nr_drift_cave_test.py [--exe <nr_drift_test>] [--main <main.raw>] [--cases 3000]
# Exit 0 PASS, 1 FAIL, 77 skipped (no unicorn).
import argparse, os, random, re, struct, subprocess, sys
try:
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UcError
    from unicorn.arm64_const import *
except Exception:
    print('SKIPPED: no unicorn'); sys.exit(77)

HDR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'nr', 'nr_drift.h')
h = open(HDR, encoding='utf-8').read()
def const(name): return int(re.search(name + r' = (0x[0-9a-f]+|\d+)', h).group(1), 0)
blk = h[h.index('nrd_spike_cave[NRD_SPIKE_CODE_WORDS] = {'):h.index('};   // + lit_page')]
CAVE = [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8}),', blk)]
oblk = h[h.index('nrd_spike_orig[4] = {'):]
ORIG = [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8})', oblk[:oblk.index('};')])]
CODE_WORDS, LIT_PAGE, LIT_BACK = const('NRD_SPIKE_CODE_WORDS'), const('NRD_SPIKE_LIT_PAGE'), const('NRD_SPIKE_LIT_BACK')
PG = {k: const('NRD_PG_' + k) for k in ('TARGET', 'MODE', 'H1', 'H2', 'SUPP', 'OVER')}
assert len(CAVE) == CODE_WORDS and len(ORIG) == 4

A_CAVE, A_PAGE, A_REND, A_DRS, A_BACK, A_ORIG = 0x100000, 0x200000, 0x300000, 0x400000, 0x500000, 0x600000
f2u = lambda f: struct.unpack('<I', struct.pack('<f', f))[0]
u2f = lambda u: struct.unpack('<f', struct.pack('<I', u & 0xffffffff))[0]
def f32(x): return u2f(f2u(x))
def fminnm(a, b):
    if a != a: return b
    if b != b: return a
    return a if a < b else b

def model(pg, raw_u):   # the Python copy of nrd_spike_step (float32 arithmetic)
    raw = u2f(raw_u); mode = pg[PG['MODE']]
    if not mode: return raw_u
    h1 = pg[PG['H1']]
    f = fminnm(raw, u2f(pg[PG['H1']]))
    if mode & 4: f = fminnm(f, u2f(pg[PG['H2']]))
    pg[PG['H2']] = h1; pg[PG['H1']] = raw_u
    t = u2f(pg[PG['TARGET']])
    if f32(t - raw) < 0.5:
        pg[PG['OVER']] += 1
        if not (f32(t - f) < 0.5): pg[PG['SUPP']] += 1
    return f2u(f) if mode & 1 else raw_u

GP = [getattr(sys.modules['unicorn.arm64_const'], 'UC_ARM64_REG_X%d' % i) for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]
VQ = [getattr(sys.modules['unicorn.arm64_const'], 'UC_ARM64_REG_Q%d' % i) for i in range(32)]

def mk():
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for a in (A_CAVE, A_PAGE, A_REND, A_DRS, A_BACK, A_ORIG): uc.mem_map(a, 0x10000)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)   # FP / SIMD on
    words = CAVE + [A_PAGE & 0xffffffff, A_PAGE >> 32, (A_BACK) & 0xffffffff, A_BACK >> 32]
    uc.mem_write(A_CAVE, struct.pack('<%dI' % len(words), *words))
    uc.mem_write(A_ORIG, struct.pack('<4I', *ORIG))
    return uc

def run(uc, start, end, rnd, raw_u, drs):
    regs = {i: rnd.getrandbits(64) for i in range(31)}
    regs[9] = A_REND; regs[20] = A_DRS
    for i, v in regs.items(): uc.reg_write(GP[i], v)
    vq = [rnd.getrandbits(128) for _ in range(32)]
    for i in range(32): uc.reg_write(VQ[i], vq[i])
    uc.mem_write(A_REND + 0x18f0, struct.pack('<I', raw_u))
    uc.mem_write(A_DRS, drs)
    uc.emu_start(start, end, count=200)
    out = {'pc': uc.reg_read(UC_ARM64_REG_PC), 'x': [uc.reg_read(GP[i]) for i in range(31)], 'q': [uc.reg_read(VQ[i]) for i in range(32)]}
    return regs, vq, out

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--exe'); ap.add_argument('--main'); ap.add_argument('--cases', type=int, default=3000)
    a = ap.parse_args(); rnd = random.Random(19112026); fails = 0; checks = 0
    def check(c, msg):
        nonlocal fails, checks
        checks += 1
        if not c:
            fails += 1
            if fails < 20: print('FAIL', msg)
    if a.main and os.path.isfile(a.main):
        d = open(a.main, 'rb').read(); site = const('NRD_SPIKE_SITE')
        check(list(struct.unpack('<4I', d[site:site + 16])) == ORIG, 'main.raw words at the site = nrd_spike_orig')
        check(const('NRD_SPIKE_BACK') == site + 16, 'back = site + 16')
    for mode, target in ((0, 15.83), (1 | 2, 15.83), (1 | 2 | 4, 31.5), (2, 15.83), (2 | 4, 10.56), (1 | 2 | 4, 100.0)):
        uc = mk(); pg = [0] * 16
        pg[PG['TARGET']] = f2u(target); pg[PG['MODE']] = mode
        pg[PG['H1']] = pg[PG['H2']] = 0x7fc00000
        uc.mem_write(A_PAGE, struct.pack('<16I', *pg))
        seq = []
        for i in range(a.cases):
            r = rnd.random()
            v = f2u(rnd.uniform(5, 14)) if r < 0.7 else f2u(rnd.uniform(14, 60)) if r < 0.95 else rnd.choice([0x7fc00000, f2u(0.0), f2u(target - 0.5), f2u(target - 0.49999)])
            if r > 0.9 and seq: v = seq[-1]   # the renderer did not update the sample
            seq.append(v)
        exe_out = None
        if a.exe:
            p = subprocess.run([a.exe, '--spike', str(mode), repr(target)], input='\n'.join('%08x' % v for v in seq) + '\n', capture_output=True, text=True)
            exe_out = [l.split() for l in p.stdout.splitlines()]
            check(len(exe_out) == len(seq), 'exe answered every sample (mode %d)' % mode)
        for i, raw_u in enumerate(seq):
            drs = bytes(rnd.getrandbits(8) for _ in range(0x50))
            regs, vq, out = run(uc, A_CAVE, A_BACK, rnd, raw_u, drs)
            want = model(pg, raw_u)
            got_s16 = out['q'][16] & 0xffffffff
            ok = out['pc'] == A_BACK and got_s16 == want
            pgm = list(struct.unpack('<16I', uc.mem_read(A_PAGE, 64)))
            ok = ok and pgm[PG['OVER']] == pg[PG['OVER']] and pgm[PG['SUPP']] == pg[PG['SUPP']] and pgm[PG['H1']] == pg[PG['H1']] and pgm[PG['H2']] == pg[PG['H2']]
            if exe_out and i < len(exe_out):
                e = exe_out[i]; ok = ok and int(e[0], 16) == want and int(e[1]) == pg[PG['OVER']] and int(e[2]) == pg[PG['SUPP']]
            # the moved instructions
            ok = ok and out['x'][9] == A_DRS + 0x28 and (out['q'][0] & 0xffffffff) == struct.unpack('<I', drs[0x28:0x2c])[0]
            ok = ok and (out['x'][11] & 0xffffffff) == struct.unpack('<I', drs[8:12])[0] and (out['x'][12] & 0xffffffff) == struct.unpack('<I', drs[12:16])[0]
            for r in range(31):
                if r in (9, 11, 12, 16, 17): continue
                ok = ok and out['x'][r] == regs[r]
            for r in range(32):
                if r in (0, 16, 22, 23, 24, 25): continue
                ok = ok and out['q'][r] == vq[r]
            check(ok, 'mode %d sample %d (%08x): s16 %08x want %08x' % (mode, i, raw_u, got_s16, want))
        if mode == 0:   # mode 0 = the original four instructions, bit for bit
            uc2 = mk()
            for i in range(200):
                raw_u = seq[i]; drs = bytes(rnd.getrandbits(8) for _ in range(0x50)); st = rnd.getstate()
                _, _, o1 = run(uc, A_CAVE, A_BACK, rnd, raw_u, drs); rnd.setstate(st)
                _, _, o2 = run(uc2, A_ORIG, A_ORIG + 16, rnd, raw_u, drs)
                same = all(o1['x'][r] == o2['x'][r] for r in range(31) if r not in (16, 17)) and all(o1['q'][r] == o2['q'][r] for r in range(32) if r not in (22, 23, 24, 25))
                check(same, 'mode 0 = the original instructions (sample %d)' % i)
        print('mode %d: %d samples, over target %d, one-off %d' % (mode, len(seq), pg[PG['OVER']], pg[PG['SUPP']]))
    print('%s: nr_drift_cave_test %d checks, %d failures' % ('PASS' if not fails else 'FAIL', checks, fails))
    return 1 if fails else 0

sys.exit(main())
