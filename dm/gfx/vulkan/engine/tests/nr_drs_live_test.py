#!/usr/bin/env python3
# [DOS2] VK-DRS-FSR (docs/dos2/nr/VK-DRS-FSR.md): the game's own dynamic-resolution controller (main.raw 0xc05da8 .. 0xc05ef8 with its
# step-down tail 0xc0652c .. 0xc06620 and step-up 0xc06458) and SetSize (0xb21880 .. 0xb218f8) run in Unicorn, patched exactly as
# dm/host/hle_dos2_quality.c patches them (the words are read from that file: the target adrp / ldr pair, the threshold ldr, the ceiling
# cave and its jump, the SetSize fmov), with our data page {target, step-up threshold, ceiling factor}. Around it a Python copy of the
# monitor thread's per-tick work (target / ceiling factor; dq_live_keep after a live call) and of dm_dos2_res3d_live. A frame = one
# controller run with the GPU ms of a cost model (fixed part + part x drawn area, optional noise / spikes), 60 frames a second.
# Checks:
#   exact     the cave in with factor 1 (the new default: it was only installed for a ceiling < 100) gives the same sizes, frame for frame,
#             as the unpatched code (no cave), on a noisy load with steps down and up;
#   steps     the controller's steps: up = 1.25 % of the window per side after 1 s of predicted headroom, down = one jump to the size its
#             model (0.5 + 0.5 x area) says fits, after two evaluations over target - 0.5 ms, never below the floor;
#   live      the gear's row switched while running (each choice of the 22.11 report): boot fixed 50 % -> dynamic (floor 75, 60 fps) ->
#             fixed 100 -> fixed 67 -> dynamic (floor 60) with a window change (SetSize of the other Switch mode) -> fixed 83: each lands within
#             a monitor tick (fixed) or stays inside [floor, 100] and settles near the target (dynamic); the floor holds under a GPU spike.
#   python nr_drs_live_test.py [--main <main.raw>] [-v]      Exit 0 PASS, 1 FAIL, 77 skipped (no unicorn / no main.raw).
import argparse, math, os, random, re, struct, sys
try:
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL
    from unicorn.arm64_const import *
except Exception:
    print('SKIPPED: no unicorn'); sys.exit(77)

HERE = os.path.dirname(os.path.abspath(__file__))
QC = os.path.join(HERE, '..', '..', '..', '..', 'host', 'hle_dos2_quality.c')
q = open(QC, encoding='utf-8').read()
def enum(name): return int(re.search(r'\b' + name + r' = (0x[0-9a-f]+)', q).group(1), 16)
SETSIZE, GATE, DRS_PTR, REND_PTR, GPU_MS, TGT_ADRP, THR_FMOV, CLAMP = (enum(n) for n in ('DQ_SETSIZE', 'DQ_GATE', 'DQ_DRS_PTR', 'DQ_RENDERER_PTR', 'DQ_GPU_MS',
                                                                                    'DQ_TGT_ADRP', 'DQ_THR_FMOV', 'DQ_CLAMP'))
def words(after, n):   # the first n 0x........ words after a marker in quality.c
    s = q[q.index(after):]
    return [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8})u?\b', s)][:n]
K_CLAMP = words('static const uint32_t k_clamp[4]', 4)
K_TGT = words('static const uint32_t k_tgt[3]', 3)
K_THR = words('static const uint32_t k_thr[2]', 2)
CAVE_HEAD = words('const uint32_t w[14] = {', 4)        # ldr x16,#40 / ldr s22,[x16,#8] / fmul s7 / fmul s2
CAVE_TAIL = words('k_clamp[3],\n                            0x58000090', 2) if False else [0x58000090, 0xd61f0200]
assert 'k_clamp[0], k_clamp[1], k_clamp[2], k_clamp[3],\n                            0x58000090 /* ldr x16,#16 (back) */, 0xd61f0200 /* br x16 */,' in q, 'the cave layout changed: update this test'
JUMP = words('const uint32_t j[4] = {', 2)               # ldr x16,#8 / br x16
LDR_S18 = int(re.search(r'0x([0-9a-f]{8}) /\* ldr s18,\[x11\] \*/', q).group(1), 16)
LDR_S5 = int(re.search(r'dq_poke1\(m, DQ_THR_FMOV, 0x([0-9a-f]{8}) /\* ldr s5,\[x11,#4\] \*/', q).group(1), 16)
assert 'if (pg) g_cave = !dq_cave_install(m);' in q, 'VK-DRS-FSR: the cave is installed whenever our page is'
NH = open(os.path.join(HERE, '..', '..', '..', 'nr', 'nr_drift.h'), encoding='utf-8').read()   # NR-DRIFT DM_DOS2_DRS_SPIKE cave (quality.c dq_spike_install)
def nconst(name): return int(re.search(name + r' = (0x[0-9a-f]+|\d+)', NH).group(1), 0)
_b = NH[NH.index('nrd_spike_cave[NRD_SPIKE_CODE_WORDS] = {'):NH.index('};   // + lit_page')]
SPIKE_CAVE = [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8}),', _b)]
SPIKE_SITE, SPIKE_BACK = nconst('NRD_SPIKE_SITE'), nconst('NRD_SPIKE_BACK')
assert len(SPIKE_CAVE) == nconst('NRD_SPIKE_CODE_WORDS')
_m = re.search(r'const char \*sp = getenv\("DM_DOS2_DRS_SPIKE"\); int sm = sp && \*sp \? atoi\(sp\) : (\d);', q)
SPIKE_DEF = {0: 0, 1: 1 | 2 | 4, 2: 2 | 4}[int(_m.group(1))]   # the cave's mode bits of DM_DOS2_DRS_SPIKE unset (apply | count | min of 3)

f2u = lambda f: struct.unpack('<I', struct.pack('<f', f))[0]
u2f = lambda u: struct.unpack('<f', struct.pack('<I', u & 0xffffffff))[0]
f32 = lambda x: u2f(f2u(x))

def imm8(i):   # quality.c dq_imm8
    a, b, cd, f = i >> 7 & 1, i >> 6 & 1, i >> 4 & 3, i & 15
    return u2f(a << 31 | (b ^ 1) << 30 | ((0x1f << 25) if b else 0) | cd << 23 | f << 19)
def imm8_floor(want):   # the largest immediate not above want (quality.c with DM_HACK_SCALE), else the nearest
    best, bi = 0, 0x68
    for i in range(256):
        v = imm8(i)
        if 0 < v <= want + 1e-6 and v > best: best, bi = v, i
    return bi, best

A_MAIN, MAIN_SIZE = 0, 0x3400000
A_PAGE, A_CAVE, A_OBJ, A_STACK, A_SPIKE = 0x3800000, 0x3804000, 0x3808000, 0x3810000, 0x3820000
A_DRS, A_REND, A_CTX = A_OBJ, A_OBJ + 0x100, A_OBJ + 0x2000

class Game:
    def __init__(self, raw, cave, spike=0):
        uc = self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(A_MAIN, MAIN_SIZE, UC_PROT_ALL); uc.mem_write(A_MAIN, raw)
        for a, n in ((A_PAGE, 0x4000), (A_CAVE, 0x4000), (A_OBJ, 0x8000), (A_STACK, 0x10000), (A_SPIKE, 0x4000)): uc.mem_map(a, n, UC_PROT_ALL)
        uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)
        rd = lambda a: struct.unpack('<I', bytes(uc.mem_read(a, 4)))[0]
        assert [rd(CLAMP + 4 * i) for i in range(4)] == K_CLAMP and [rd(TGT_ADRP - 4 + 4 * i) for i in range(3)] == K_TGT and \
            [rd(THR_FMOV - 4 + 4 * i) for i in range(2)] == K_THR, 'main.raw is not build 4979b200'
        # quality.c: the target pair (adrp x11 page; ldr s18,[x11]) and the threshold (ldr s5,[x11,#4])
        dp = (A_PAGE >> 12) - (TGT_ADRP >> 12); imm = dp & 0x1fffff
        adrp = 0x9000000b | (imm & 3) << 29 | (imm >> 2) << 5
        uc.mem_write(TGT_ADRP, struct.pack('<2I', adrp, LDR_S18)); uc.mem_write(THR_FMOV, struct.pack('<I', LDR_S5))
        if cave:
            back = CLAMP + 16
            w = CAVE_HEAD + K_CLAMP + CAVE_TAIL + [A_PAGE & 0xffffffff, A_PAGE >> 32, back & 0xffffffff, back >> 32]
            uc.mem_write(A_CAVE, struct.pack('<14I', *w))
            uc.mem_write(CLAMP, struct.pack('<4I', JUMP[0], JUMP[1], A_CAVE & 0xffffffff, A_CAVE >> 32))
        if spike:   # DM_DOS2_DRS_SPIKE (nrd_spike_init: mode word, history NaN, counters 0)
            lit = SPIKE_CAVE + [A_PAGE & 0xffffffff, A_PAGE >> 32, SPIKE_BACK & 0xffffffff, SPIKE_BACK >> 32]
            uc.mem_write(A_SPIKE, struct.pack('<%dI' % len(lit), *lit))
            uc.mem_write(A_PAGE + 12, struct.pack('<6I', spike, 0x7fc00000, 0x7fc00000, 0, 0, 0))
            uc.mem_write(SPIKE_SITE, struct.pack('<4I', JUMP[0], JUMP[1], A_SPIKE & 0xffffffff, A_SPIKE >> 32))
        uc.mem_write(DRS_PTR, struct.pack('<Q', A_DRS)); uc.mem_write(REND_PTR, struct.pack('<Q', A_REND))
        self.page(31.5, 30.0, 1.0)
    def page(self, t, h, c): self.uc.mem_write(A_PAGE, struct.pack('<3f', t, h, c))
    def floor_fmov(self, pct):   # quality.c mode 2: SetSize fmov s2,#min (0x1e201002 | i8 << 13)
        i8, got = imm8_floor(pct / 100.0)
        self.uc.mem_write(SETSIZE + 0x10, struct.pack('<I', 0x1e201002 | i8 << 13)); return got
    def set_size(self, w, h):   # the real SetSize up to its call of 0x1bf6600 (every store done by then)
        uc = self.uc; uc.reg_write(UC_ARM64_REG_X0, A_DRS); uc.reg_write(UC_ARM64_REG_X1, w); uc.reg_write(UC_ARM64_REG_X2, h)
        uc.reg_write(UC_ARM64_REG_SP, A_STACK + 0x8000); uc.emu_start(SETSIZE, SETSIZE + 0x78)
    def obj(self):
        b = bytes(self.uc.mem_read(A_DRS, 0x50))
        mw, mh, nw, nh = struct.unpack_from('<4i', b, 8); cw, ch = struct.unpack_from('<2f', b, 0x28); fr, dn = struct.unpack_from('<2i', b, 0x38)
        return dict(max_w=mw, max_h=mh, min_w=nw, min_h=nh, cur_w=cw, cur_h=ch, frames=fr, downs=dn, up_ms=struct.unpack_from('<f', b, 0x44)[0])
    def write_obj(self, **kv):
        offs = {'min_w': (0x10, '<i'), 'min_h': (0x14, '<i'), 'cur_w': (0x28, '<f'), 'cur_h': (0x2c, '<f')}
        for k, v in kv.items(): o, f = offs[k]; self.uc.mem_write(A_DRS + o, struct.pack(f, v))
    def frame(self, gpu_ms, dt=1 / 60.0):
        uc = self.uc
        uc.mem_write(A_REND + GPU_MS, struct.pack('<f', gpu_ms)); uc.mem_write(A_CTX + 8, struct.pack('<f', dt))
        uc.reg_write(UC_ARM64_REG_X19, A_CTX); uc.reg_write(UC_ARM64_REG_SP, A_STACK + 0x8000)
        uc.emu_start(GATE - 8, GATE + (0xc05ef8 - GATE))   # 0xc05da8 (DRS pointer load) .. 0xc05ef8 (the controller's end)

# ---------------------------------------------------------------- the monitor thread's part and the live call (quality.c, copied)
class Host:
    def __init__(self, g, user_pct, target_ms, floor_boot):
        self.g, self.user, self.ceil_lad = g, user_pct, 100
        self.live_on, self.live_floor = 0, 0
        self.set_target(target_ms)
    def set_target(self, ms): self.tq = (f32(ms), f32(ms * 30.0 / 31.5))
    def eff(self): return min(self.user, self.ceil_lad)
    def tick(self):   # dq_monitor: the ceiling factor every tick (g_cave), then dq_live_keep after a live call
        self.g.page(self.tq[0], self.tq[1], f32(self.eff() / 100.0))
        if self.live_on: self.keep()
    def keep(self):   # dq_live_keep
        d = self.g.obj()
        if d['max_w'] <= 0: return
        fl = self.live_floor
        if 50 <= fl <= 100:
            mw, mh = int(f32(d['max_w'] * f32(fl / 100.0))), int(f32(d['max_h'] * f32(fl / 100.0)))
            if (d['min_w'], d['min_h']) != (mw, mh): self.g.write_obj(min_w=mw, min_h=mh); d['min_w'], d['min_h'] = mw, mh
        c = f32(self.eff() / 100.0); cw, ch = f32(d['max_w'] * c), f32(d['max_h'] * c)
        lw, lh = min(float(d['min_w']), cw), min(float(d['min_h']), ch)
        uw, uh = d['cur_w'], d['cur_h']; nw, nh = uw, uh
        if uh > ch + 0.5 or uw > cw + 0.5: nw, nh = cw, ch
        elif uh < lh - 0.5 or uw < lw - 0.5: nw, nh = lw, lh
        if (nw, nh) != (uw, uh): self.g.write_obj(cur_w=nw, cur_h=nh)
    def live(self, fixed_pct, fps, floor_pct):   # dm_dos2_res3d_live
        if fixed_pct > 0: fl = ce = max(50, min(100, fixed_pct)); self.set_target(100.0)
        else:
            fl, ce = max(50, min(99, floor_pct)), 100
            self.set_target(1000.0 / fps * 0.95)
        self.user, self.live_floor, self.live_on = ce, fl, 1
        self.g.page(self.tq[0], self.tq[1], f32(self.eff() / 100.0))

def cost(d, base, per, noise=0.0, rng=None):   # GPU ms of a frame at the current size: fixed part + part x drawn area
    a = d['cur_w'] * d['cur_h'] / float(d['max_w'] * d['max_h'])
    return f32(base + per * a + (rng.gauss(0, noise) if rng else 0.0))

def pct(d): return 100.0 * d['cur_h'] / d['max_h']

def run(g, h, secs, base, per, noise=0.0, rng=None, spikes=(), trace=None):
    for f in range(int(secs * 60)):
        if f % 15 == 0: h.tick()
        d = g.obj(); ms = cost(d, base, per, noise, rng)
        if f in spikes: ms = f32(80.0)
        g.frame(ms)
        if trace is not None: trace.append((pct(g.obj()), ms))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--main', default=os.path.join(HERE, '..', '..', '..', '..', '..', '..', 'work', 'exefs-update', 'main.raw'))
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    if not os.path.isfile(a.main):
        alt = os.path.join(HERE, '..', '..', '..', '..', '..', '..', '..', 'work', 'exefs-update', 'main.raw')   # a worktree beside src: <port>/work
        if os.path.isfile(alt): a.main = alt
        else: print('SKIPPED: no main.raw (%s)' % a.main); sys.exit(77)
    raw = open(a.main, 'rb').read()
    fails = []
    def check(c, what):
        if not c: fails.append(what); print('  FAIL', what)
    W, H = 2160, 972

    # ---- exact: factor 1 cave == no cave, frame for frame, on a noisy dynamic run (steps down and up happen)
    seqs = []
    for cave in (0, 1):
        g = Game(raw, cave); g.floor_fmov(75); g.set_size(W, H); hst = Host(g, 100, 15.83, 75)
        tr = []; run(g, hst, 40, 5.0, 14.0, 0.3, random.Random(7), spikes=(600, 601, 1500, 1503), trace=tr); seqs.append(tr)
    downs = sum(1 for i in range(1, len(seqs[1])) if seqs[1][i][0] < seqs[1][i - 1][0] - 1e-3)
    ups = sum(1 for i in range(1, len(seqs[1])) if seqs[1][i][0] > seqs[1][i - 1][0] + 1e-3)
    check(seqs[0] == seqs[1], 'exact: the factor-1 cave changes nothing (%d frames, %d steps down, %d up)' % (len(seqs[1]), downs, ups))
    check(downs >= 2 and ups >= 5, 'exact: the run steps both ways (%d down, %d up)' % (downs, ups))
    print('exact: cave factor 1 == no cave over %d frames (%d steps down, %d steps up): %s' % (len(seqs[1]), downs, ups, seqs[0] == seqs[1]))

    # ---- steps: up 1.25 % per second of headroom, down one jump to the model's size, never below the floor
    g = Game(raw, 1); g.floor_fmov(75); g.set_size(W, H); hst = Host(g, 100, 15.83, 75)
    hst.tick(); g.write_obj(cur_w=W * 0.8, cur_h=H * 0.8)
    tr = []; run(g, hst, 6, 2.0, 4.0, trace=tr)   # cheap: steps up
    ups = [round(tr[i][0] - tr[i - 1][0], 3) for i in range(1, len(tr)) if tr[i][0] > tr[i - 1][0] + 1e-3]
    gaps = [i for i in range(1, len(tr)) if tr[i][0] > tr[i - 1][0] + 1e-3]
    check(ups and all(abs(u - 1.25) < 0.01 for u in ups[:-1]), 'steps: up by 1.25 %% (%s)' % ups[:6])
    check(len(gaps) >= 3 and all(55 <= gaps[i] - gaps[i - 1] <= 66 for i in range(1, len(gaps))), 'steps: one step up a second (%s)' % gaps[:6])
    d0 = g.obj(); tr = []; run(g, hst, 2, 6.0, 20.0, trace=tr)   # 26 ms at 100 %: over target
    lands = sorted(set(round(x[0], 2) for x in tr), reverse=True)
    jumps = sum(1 for i in range(1, len(tr)) if tr[i][0] < tr[i - 1][0] - 1e-3)
    check(jumps <= 2 and tr[-1][0] < pct(d0) - 2.4, 'steps: down in one or two jumps (%d, %.2f -> %.2f %%)' % (jumps, pct(d0), tr[-1][0]))
    check(min(x[0] for x in tr) >= 75.0 - 1e-3, 'steps: never below the floor (%.2f)' % min(x[0] for x in tr))
    print('steps: up %s %% once a second (frames %s); down from %.1f %% at 26 ms -> %s %%' % (ups[:4], [gaps[i] - gaps[i - 1] for i in range(1, min(len(gaps), 5))],
                                                                                         pct(d0), lands[:4]))

    # ---- one slow frame, and frame-time jitter (the climb needs 1 s of frames that all predict a step up under the threshold)
    for spike in (0, 1 | 2 | 4):
        g = Game(raw, 1, spike); g.floor_fmov(75); g.set_size(W, H); hst = Host(g, 100, 15.83, 75); hst.tick(); g.write_obj(cur_w=W * 0.9, cur_h=H * 0.9)
        tr = []; run(g, hst, 3, 4.0, 10.0, spikes=(60,), trace=tr)
        one = min(x[0] for x in tr)
        g.write_obj(cur_w=W * 0.9, cur_h=H * 0.9); tr = []; run(g, hst, 3, 4.0, 10.0, spikes=(60, 61), trace=tr)
        two = min(x[0] for x in tr)
        res = []
        for sd in (0.0, 0.4, 0.8, 1.5):
            g.write_obj(cur_w=W * 0.75, cur_h=H * 0.75); tr = []; run(g, hst, 30, 4.0, 18.0, sd, random.Random(11), trace=tr)
            res.append((sd, tr[-1][0], sum(x[1] for x in tr[-600:]) / 600))
        print('spike filter %s: one 80 ms frame at 90 %% -> lowest %.2f %%, two in a row -> %.2f %%; climb from 75 %% for 30 s on 4 + 18 x area ms with jitter sd 0 / 0.4 / 0.8 / 1.5 ms -> %s'
              % ('off' if not spike else 'on (min of 3)', one, two, ', '.join('%.2f %% (GPU %.2f)' % (r[1], r[2]) for r in res)))
        check(min(r[1] for r in res) >= 75.0 - 1e-3, 'jitter: never below the floor')
        check(one > 89.9, 'one slow frame keeps the size (%.2f)' % one)
        if spike: check(two > 89.9 and res[2][1] > 77.0, 'spike filter: two slow frames keep the size (%.2f), it climbs under 0.8 ms jitter (%.2f)' % (two, res[2][1]))

    # ---- live: the owner's case and every switch of the row
    g = Game(raw, 1, SPIKE_DEF); got = g.floor_fmov(50); g.set_size(W, H)
    hst = Host(g, 50, 100.0, 50)   # start env: fixed 50 % (DM_DOS2_DRS=off + DM_HACK_SCALE=50: min = ceiling = 50, 100 ms)
    tr = []; run(g, hst, 3, 3.0, 12.0, trace=tr)
    check(abs(tr[-1][0] - 50.0) < 0.2, 'live: boot fixed 50 %% lands (%.2f)' % tr[-1][0])
    print('live (DM_DOS2_DRS_SPIKE default bits %d): boot fixed 50 %% (fmov %.3f) -> %.2f %% after 3 s' % (SPIKE_DEF, got, tr[-1][0]))
    steps = [('dynamic floor 75, 60 fps', (0, 60, 75), 'dyn', 75), ('fixed 100', (100, 60, 75), 'fix', 100), ('fixed 67', (67, 60, 75), 'fix', 67),
             ('dynamic floor 60, 60 fps + window change', (0, 60, 60), 'dyn', 60), ('fixed 83', (83, 60, 60), 'fix', 83)]
    for name, args, kind, want in steps:
        hst.live(*args)
        if 'window' in name: g.set_size(1728, 778)   # the other Switch mode: SetSize writes its boot fmov floor (50 %) and current = max again
        tr = []; run(g, hst, 12, 4.0, 18.0, 0.8, random.Random(len(name)), spikes=(400, 405), trace=tr)
        first = tr[16][0] if len(tr) > 16 else tr[-1][0]   # after one monitor tick
        lo, hi = min(x[0] for x in tr[16:]), max(x[0] for x in tr[16:])
        d = g.obj()
        if kind == 'fix':
            check(abs(first - want) < 0.2 and lo > want - 0.2 and hi < want + 0.2, 'live %s: lands within a tick and holds (%.2f, %.2f..%.2f)' % (name, first, lo, hi))
        else:
            tail = [x[1] for x in tr[-180:]]
            check(lo >= want - 0.2 and hi <= 100.0 + 1e-3, 'live %s: inside [floor, 100] (%.2f..%.2f)' % (name, lo, hi))
            check(abs(d['min_h'] - int(d['max_h'] * want / 100.0)) <= 1, 'live %s: floor in the object (%d of %d)' % (name, d['min_h'], d['max_h']))
            check(sum(tail) / len(tail) < 15.83 + 0.6, 'live %s: settles at the target (mean GPU %.2f ms over the last 3 s)' % (name, sum(tail) / len(tail)))
        print('live: %-42s after a tick %6.2f %%, then %6.2f .. %6.2f %%, end %6.2f %% (%dx%d of %dx%d), mean GPU last 3 s %.2f ms' %
              (name, first, lo, hi, pct(d), d['cur_w'], d['cur_h'], d['max_w'], d['max_h'], sum(x[1] for x in tr[-180:]) / 180))
    if fails: print('FAIL: %d' % len(fails)); sys.exit(1)
    print('PASS: VK-DRS-FSR live 3D resolution: the game controller in Unicorn, patched as quality.c (exact with the factor-1 cave, steps, every live switch)')

if __name__ == '__main__':
    main()
