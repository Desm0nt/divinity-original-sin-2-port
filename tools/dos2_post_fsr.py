"""Divinity: Original Sin 2 DE, N6 DM_HACK_FSR (docs/dos2/nr/NR-HACKS.md): build and prove the PPFXAA + FSR 1 EASU module.

1. Build: tools/dos2_post/PPFXAA_fsr.frag -> glslc (vulkan1.1, -O) -> spirv-val -> interface check against the translated PPFXAA
   (work/shaders-vk/691768aa08545d95.xgs: inputs / outputs equal, descriptors a subset) -> dm/android/shaders/ppfxaa_fsr.frag.spv.inc
   (the backend wraps it with the player's own translated head at load time, xbg_nr_sh.inc fsr_apply: no setup run).
2. Proof 1:1 (the 3D at 100 % and the HUD portraits): tools/spv_interp.py runs the translated PPFXAA (spirv-opt -O) and this module on the
   random cases of tools/dos2_post_mobile.py (PPFXAA: noise, straight edges, NaN texels / coordinates, odd constants) with the derivatives of
   a 1:1 stretch (m.dpdx = 1 / width): the outputs must be equal bit for bit.
3. Proof EASU (stretch ratio < 1): the module against an independent numpy FsrEasuF that reads the 12 texels by index (no gather corner
   arithmetic), on random images and positions at ratios 0.5 .. 0.9 incl. the drawn-part edge (clamp): max |difference| must stay < 2e-6.
4. --images: offline before / after on the owner's screenshots (src/work/logs t4.png / light1.png, not in git): a crop box-downscaled to
   the ratio (a stand-in for the low-resolution render: no aliasing of its own - the real low render is sharper and jaggier), then upscaled
   bilinear (today's stretch) / EASU / EASU + RCAS 0.5 stops (the gear's medium sharpening); PSNR against the full-resolution crop and side
   by side PNGs into --out (default the scratch folder given, else work/fsr-shots).
Usage: dos2_post_fsr.py [--n 400] [--no-build] [--images DIR_OF_PNG --out DIR]
"""
import argparse, struct, subprocess, sys, tempfile
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from dos2_native_shaders import Spv, read_bundle, tool, WORK
from dos2_pcf_mobile import run, check_iface
import spv_interp as SI
import dos2_post_mobile as PM

class DModule(SI.Module):
    """spv_interp.Module + OpDPdx / OpDPdy (and Fine / Coarse) = the constants m.dpdx / m.dpdy: the derivatives of a varying that is linear
    across the draw (the full-screen triangle's TexCoord0). Kept here, not in spv_interp.py: that file is part of setup's post-pack tools
    hash (a change would make every player re-run setup)."""
    dpdx = dpdy = None

    def _exec(self, env, op, a):
        if op in (207, 208, 210, 211, 213, 214):
            d = self.dpdx if op in (207, 210, 213) else self.dpdy
            if d is None: raise RuntimeError('OpDPdx / OpDPdy without m.dpdx / m.dpdy')
            x = self._get(env, a[2]); env[a[1]] = [SI.F(d) for _ in x] if isinstance(x, list) else SI.F(d); return
        return super()._exec(env, op, a)


KEY = '691768aa08545d95'
SRC = HERE / 'dos2_post' / 'PPFXAA_fsr.frag'
INC = HERE.parent / 'dm' / 'android' / 'shaders' / 'ppfxaa_fsr.frag.spv.inc'
F = np.float32


INC_H = HERE.parent / 'dm' / 'android' / 'shaders' / 'ppfxaa_fsrh.frag.spv.inc'


def build_one(tr, inc, defines):
    with tempfile.TemporaryDirectory() as tmp:
        spv = Path(tmp) / 'fsr.spv'
        run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', *defines, '-I', str(SRC.parent), '-o', str(spv), str(SRC)])
        run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(spv)])
        data = spv.read_bytes()
    check_iface(data, tr['spirv'])
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    lines = ['// [DOS2] N6 DM_HACK_FSR: SPIR-V of tools/dos2_post/PPFXAA_fsr.frag (glslc --target-env=vulkan1.1 -O %s), made by tools/dos2_post_fsr.py'
             % ' '.join(defines)]
    for i in range(0, len(w), 8): lines.append(', '.join('0x%08x' % x for x in w[i:i + 8]) + ',')
    inc.write_text('\n'.join(lines) + '\n')
    print('built %s: %d words (translated PPFXAA %d words), interface = the translated one' % (inc.name, len(w), len(tr['spirv']) // 4))
    return data


def build(bundles):
    """-> (fp32 module, half module (-DEASU_H), the translated PPFXAA after spirv-opt -O)"""
    tr = read_bundle(Path(bundles) / (KEY + '.xgs'))
    data = build_one(tr, INC, []); data_h = build_one(tr, INC_H, ['-DEASU_H'])
    with tempfile.TemporaryDirectory() as tmp:
        to, too = Path(tmp) / 'tr.spv', Path(tmp) / 'tro.spv'
        to.write_bytes(tr['spirv']); run([tool('spirv-opt'), '-O', str(to), '-o', str(too)])
        tr_opt = too.read_bytes()
    return data, data_h, tr_opt


# ---------------------------------------------------------------------------------------------------------------- reference EASU
def u2f(u): return np.array([u], dtype=np.uint32).view(np.float32)[0]
def f2u(f): return int(np.array([f], dtype=np.float32).view(np.uint32)[0])
def rcp_lo(a): return u2f((0x7ef07ebb - f2u(a)) & 0xffffffff)
def rsq_lo(a): return u2f((0x5f347d74 - (f2u(a) >> 1)) & 0xffffffff)


def easu_ref(img, tc, ratio):
    """img [h, w, >=3] float32; tc = TexCoord0.xy; ratio = drawn part / size. FsrEasuF with texels read by index, the 4 gather corners
    clamped to the drawn part as the module does (clamp of the corner point to 1 .. lim - 1)."""
    h, w = img.shape[:2]; size = np.array([w, h], F)
    lim = np.maximum(np.floor(F(ratio) * size + F(0.5)), F(2))
    pp = np.array(tc, F) * size - F(0.5); fp = np.floor(pp); pp = (pp - fp).astype(F)

    # the 12 texels by their offsets from fp (b c / e f g h / i j k l / n o); each belongs to one of the module's 4 gathers, whose corner
    # point the module clamps to 1 .. lim - 1 - the same shift moves the group's texels here (edge rows / columns of the drawn part only)
    groups = {'b': (1, -1), 'c': (1, -1), 'i': (0, 1), 'j': (0, 1), 'f': (0, 1), 'e': (0, 1), 'k': (2, 1), 'l': (2, 1), 'h': (2, 1), 'g': (2, 1),
              'o': (1, 3), 'n': (1, 3)}
    offs = {'b': (0, -1), 'c': (1, -1), 'i': (-1, 1), 'j': (0, 1), 'f': (0, 0), 'e': (-1, 0), 'k': (1, 1), 'l': (2, 1), 'h': (2, 0), 'g': (1, 0),
            'o': (1, 2), 'n': (0, 2)}
    T = {}
    for k, o in offs.items():
        q = fp + groups[k]; sh = np.clip(q, 1, lim - 1) - q
        x, y = int(fp[0] + o[0] + sh[0]), int(fp[1] + o[1] + sh[1])
        T[k] = img[min(max(y, 0), h - 1), min(max(x, 0), w - 1), :3].astype(F)
    L = {k: F(v[2] * F(0.5) + (v[0] * F(0.5) + v[1])) for k, v in T.items()}
    dirv = [F(0), F(0)]; ln = [F(0)]

    def setf(wt, A, B, C, D, E):
        dc, cb = F(D - C), F(C - B); lenX = rcp_lo(F(max(abs(dc), abs(cb)))); dirX = F(D - B)
        dirv[0] = F(dirv[0] + F(dirX * wt)); lenX = F(min(max(F(abs(dirX) * lenX), F(0)), F(1))); lenX = F(lenX * lenX); ln[0] = F(ln[0] + F(lenX * wt))
        ec, ca = F(E - C), F(C - A); lenY = rcp_lo(F(max(abs(ec), abs(ca)))); dirY = F(E - A)
        dirv[1] = F(dirv[1] + F(dirY * wt)); lenY = F(min(max(F(abs(dirY) * lenY), F(0)), F(1))); lenY = F(lenY * lenY); ln[0] = F(ln[0] + F(lenY * wt))
    px, py = pp
    setf(F(F(1 - px) * F(1 - py)), L['b'], L['e'], L['f'], L['g'], L['j'])
    setf(F(px * F(1 - py)), L['c'], L['f'], L['g'], L['h'], L['k'])
    setf(F(F(1 - px) * py), L['f'], L['i'], L['j'], L['k'], L['n'])
    setf(F(px * py), L['g'], L['j'], L['k'], L['l'], L['o'])
    dx, dy = dirv; dirR = F(F(dx * dx) + F(dy * dy)); zro = dirR < F(1.0 / 32768.0)
    dirR = F(1) if zro else rsq_lo(dirR); dx = F(1) if zro else dx
    dx, dy = F(dx * dirR), F(dy * dirR)
    l = F(ln[0] * F(0.5)); l = F(l * l)
    stretch = F(F(F(dx * dx) + F(dy * dy)) * rcp_lo(F(max(abs(dx), abs(dy)))))
    len2 = (F(F(1) + F(F(stretch - F(1)) * l)), F(F(1) + F(F(-0.5) * l)))
    lob = F(F(0.5) + F(F((1.0 / 4.0 - 0.04) - 0.5) * l)); clp = rcp_lo(lob)
    mn = np.minimum(np.minimum(T['f'], np.minimum(T['g'], T['j'])), T['k']); mx = np.maximum(np.maximum(T['f'], np.maximum(T['g'], T['j'])), T['k'])
    aC = np.zeros(3, F); aW = F(0)
    for name, off in (('b', (0, -1)), ('c', (1, -1)), ('i', (-1, 1)), ('j', (0, 1)), ('f', (0, 0)), ('e', (-1, 0)), ('k', (1, 1)),
                      ('l', (2, 1)), ('h', (2, 0)), ('g', (1, 0)), ('o', (1, 2)), ('n', (0, 2))):
        ox, oy = F(F(off[0]) - px), F(F(off[1]) - py)
        vx = F(F(F(ox * dx) + F(oy * dy)) * len2[0]); vy = F(F(F(ox * F(-dy)) + F(oy * dx)) * len2[1])
        d2 = min(F(F(vx * vx) + F(vy * vy)), clp)
        wB = F(F(F(2.0 / 5.0) * d2) - F(1)); wA = F(F(lob * d2) - F(1)); wB = F(wB * wB); wA = F(wA * wA)
        wB = F(F(F(25.0 / 16.0) * wB) - F(25.0 / 16.0 - 1.0)); wt = F(wB * wA)
        aC = (aC + T[name] * wt).astype(F); aW = F(aW + wt)
    return np.minimum(mx, np.maximum(mn, aC * F(F(1) / aW)))


def easu_ref_h(img, tc, ratio, dt=np.float16):
    """FsrEasuH as PPFXAA_fsr.frag -DEASU_H computes it (pairs, exact reciprocals, edge-length floor 2^-14), every value rounded to dt after
    each operation (np.float16: the bound of mediump on a GPU with fp16 ALUs; np.float32: what the interpreter runs). Positions float32."""
    D = dt; h, w = img.shape[:2]; size = np.array([w, h], F)
    lim = np.maximum(np.floor(F(ratio) * size + F(0.5)), F(2))
    pp = np.array(tc, F) * size - F(0.5); fp = np.floor(pp); ppp = (pp - fp).astype(D)
    groups = {'b': (1, -1), 'c': (1, -1), 'i': (0, 1), 'j': (0, 1), 'f': (0, 1), 'e': (0, 1), 'k': (2, 1), 'l': (2, 1), 'h': (2, 1), 'g': (2, 1),
              'o': (1, 3), 'n': (1, 3)}
    offs = {'b': (0, -1), 'c': (1, -1), 'i': (-1, 1), 'j': (0, 1), 'f': (0, 0), 'e': (-1, 0), 'k': (1, 1), 'l': (2, 1), 'h': (2, 0), 'g': (1, 0),
            'o': (1, 2), 'n': (0, 2)}
    T = {}
    for k, o in offs.items():
        q = fp + groups[k]; sh = np.clip(q, 1, lim - 1) - q
        x, y = int(fp[0] + o[0] + sh[0]), int(fp[1] + o[1] + sh[1])
        T[k] = img[min(max(y, 0), h - 1), min(max(x, 0), w - 1), :3].astype(D)
    with np.errstate(all='ignore'):
        L = {k: D(v[2] * D(0.5) + D(v[0] * D(0.5) + v[1])) for k, v in T.items()}
        st = {'dx': np.zeros(2, D), 'dy': np.zeros(2, D), 'ln': np.zeros(2, D)}

        def setH(wt, A, B, C, Dd, E):
            for (p, q), key in (((Dd, B), 'dx'), ((E, A), 'dy')):
                lo, hi = (C - (B if key == 'dx' else A)).astype(D), (Dd - C if key == 'dx' else E - C).astype(D)
                ln_ = (D(1) / np.maximum(np.maximum(abs(hi), abs(lo)), D(1.0 / 16384.0))).astype(D)
                dd_ = (p - q).astype(D)
                st[key] = (st[key] + (dd_ * wt).astype(D)).astype(D)
                ln_ = np.clip((abs(dd_) * ln_).astype(D), 0, 1).astype(D); ln_ = (ln_ * ln_).astype(D)
                st['ln'] = (st['ln'] + (ln_ * wt).astype(D)).astype(D)
        P = lambda a, b: np.array([L[a], L[b]], D)
        wx = (np.array([1, 0], D) + np.array([-ppp[0], ppp[0]], D)).astype(D)
        setH((wx * D(D(1) - ppp[1])).astype(D), P('b', 'c'), P('e', 'f'), P('f', 'g'), P('g', 'h'), P('j', 'k'))
        setH((wx * ppp[1]).astype(D), P('f', 'g'), P('i', 'j'), P('j', 'k'), P('k', 'l'), P('n', 'o'))
        dx, dy = D(st['dx'][0] + st['dx'][1]), D(st['dy'][0] + st['dy'][1]); ln = D(st['ln'][0] + st['ln'][1])
        dirR = D(D(dx * dx) + D(dy * dy)); zro = dirR < D(1.0 / 32768.0)
        dirR = D(1) if zro else D(D(1) / D(np.sqrt(dirR))); dx = D(1) if zro else dx
        dx, dy = D(dx * dirR), D(dy * dirR)
        l = D(ln * D(0.5)); l = D(l * l)
        stretch = D(D(D(dx * dx) + D(dy * dy)) / D(max(abs(dx), abs(dy))))
        len2 = (D(D(1) + D(D(stretch - D(1)) * l)), D(D(1) - D(D(0.5) * l)))
        lob = D(D(0.5) + D(D((1.0 / 4.0 - 0.04) - 0.5) * l)); clp = D(D(1) / lob)
        mn = np.minimum(np.minimum(T['f'], T['g']), np.minimum(T['j'], T['k'])); mx = np.maximum(np.maximum(T['f'], T['g']), np.maximum(T['j'], T['k']))
        aC = np.zeros(3, D); aW = D(0)
        for name, off in (('b', (0, -1)), ('c', (1, -1)), ('i', (-1, 1)), ('j', (0, 1)), ('f', (0, 0)), ('e', (-1, 0)), ('k', (1, 1)),
                          ('l', (2, 1)), ('h', (2, 0)), ('g', (1, 0)), ('o', (1, 2)), ('n', (0, 2))):
            ox, oy = D(D(off[0]) - ppp[0]), D(D(off[1]) - ppp[1])
            vx = D(D(D(ox * dx) + D(oy * dy)) * len2[0]); vy = D(D(D(ox * D(-dy)) + D(oy * dx)) * len2[1])
            d2 = min(D(D(vx * vx) + D(vy * vy)), clp)
            wB = D(D(D(2.0 / 5.0) * d2) - D(1)); wA = D(D(lob * d2) - D(1)); wB = D(wB * wB); wA = D(wA * wA)
            wB = D(D(D(25.0 / 16.0) * wB) - D(25.0 / 16.0 - 1.0)); wt = D(wB * wA)
            aC = (aC + (T[name] * wt).astype(D)).astype(D); aW = D(aW + wt)
        return np.minimum(mx, np.maximum(mn, (aC * D(D(1) / aW)).astype(D))).astype(np.float32)


def prove_easu_h(nat_h, n):
    """the half module (the interpreter runs it in fp32) against easu_ref_h in fp32: the FsrEasuH form transcribed right"""
    rng = np.random.default_rng(0xfa5f)
    m = DModule(nat_h); W, H = 24, 16; worst = 0.0; bad = 0
    for it in range(n):
        r = float(rng.choice([0.5, 0.6667, 0.75, 0.85, 0.9])); d = rng.uniform(0, 1, (H, W, 4)).astype(F)
        if it % 2:
            yy, xx = np.mgrid[0:H, 0:W].astype(F); a = rng.uniform(0, np.pi); lv = rng.uniform(0, 1, (2, 4)).astype(F)
            side = (np.cos(a) * (xx - rng.uniform(0, W * r)) + np.sin(a) * (yy - rng.uniform(0, H * r))) > 0
            d = np.where(side[..., None], lv[0], lv[1]).astype(F)
        ox, oy = int(rng.integers(0, W)), int(rng.integers(0, H))
        if rng.random() < 0.2: ox = W - 1
        tc = (F((ox + 0.5) / W * r), F((oy + 0.5) / H * r)); m.dpdx, m.dpdy = r / W, r / H
        c1 = struct.pack('<2fi', 1 / 12, 0.5, 1) + bytes(244)
        out = m.run({'FragCoord': (ox + 0.5, oy + 0.5, 0.5, 1.0), 1: (tc[0], tc[1], 0.0, 1.0)}, {16: PM.cbuf({13: (0, 0, W, H)}), 22: c1},
                    {10: SI.Texture(d, 'linear')})
        diff = float(np.max(np.abs(np.array(out[0][:3], F) - easu_ref_h(d, tc, r, np.float32))))
        worst = max(worst, diff)
        if not diff < 2e-6:
            bad += 1
            if bad <= 3: print('  MISMATCH EASU-H r=%.3f px=(%d,%d) diff %.3g' % (r, ox, oy, diff))
    print('EASU half module (run in fp32): %d / %d within 2e-6 of the numpy FsrEasuH form (worst %.3g)' % (n - bad, n, worst))
    return bad


def error_bound_h(src_dir, n):
    """fp16 FsrEasuH (numpy float16 after every operation) against fp32 FsrEasuF on the owner's screenshots (8-bit like the game's RGBA8
    target), random positions; -> errors in 8-bit steps"""
    from PIL import Image
    rng = np.random.default_rng(0x16); errs = []
    imgs = [np.asarray(Image.open(Path(src_dir) / f).convert('RGBA'), dtype=np.float32) / 255.0 for f in ('t4.png', 'light1.png') if (Path(src_dir) / f).exists()]
    if not imgs: print('  (no screenshots: error bound skipped)'); return None
    for it in range(n):
        img = imgs[it % len(imgs)]
        y0, x0 = int(rng.integers(100, img.shape[0] - 140)), int(rng.integers(300, img.shape[1] - 400))
        crop = img[y0:y0 + 32, x0:x0 + 48]; Hh, Ww = crop.shape[:2]
        tc = (F(rng.uniform(4, Ww - 4) / Ww), F(rng.uniform(4, Hh - 4) / Hh))
        f32, h32, h16 = easu_ref(crop, tc, 1.0), easu_ref_h(crop, tc, 1.0, np.float32), easu_ref_h(crop, tc, 1.0, np.float16)
        errs.append((float(np.max(np.abs(h16 - h32))), float(np.max(np.abs(h16 - f32))), float(np.max(np.abs(h32 - f32)))))
    e = np.array(errs) * 255
    for k, what in enumerate(('fp16 vs the same form in fp32 (precision)', 'fp16 FsrEasuH vs fp32 FsrEasuF (all)',
                              'fp32 FsrEasuH form vs FsrEasuF (exact vs approximate reciprocals)')):
        print('EASU %s, %d screenshot positions: max %.2f, p99 %.2f, mean %.3f 8-bit steps' % (what, n, e[:, k].max(), np.percentile(e[:, k], 99), e[:, k].mean()))
    return e


def prove_11(nat, tr_opt, n):
    rng = np.random.default_rng(0x691768aa)
    mt, mn = SI.Module(tr_opt), DModule(nat)
    bad = 0; ex = [[], []]
    for it in range(n):
        inp, ubo, tx = PM.case('PPFXAA', rng, False)
        img = tx[10]; mn.dpdx, mn.dpdy = 1.0 / img.w, 1.0 / img.h
        a, b = mt.run(inp, ubo, tx), mn.run(inp, ubo, tx)
        ex[0].append(mt.executed); ex[1].append(mn.executed)
        if not PM.same(a, b):
            bad += 1
            if bad <= 3: print('  MISMATCH 1:1 in=%s translated=%s fsr=%s' % (inp, a, b))
    print('1:1 (FXAA path): %d random invocations, fsr module == translated PPFXAA bit for bit: %d / %d; executed/pixel %.1f vs %.1f'
          % (n, n - bad, n, np.mean(ex[0]), np.mean(ex[1])))
    return bad


def prove_easu(nat, n):
    rng = np.random.default_rng(0xfa5e)
    m = DModule(nat); W, H = 24, 16
    worst = 0.0; bad = 0; ex = []
    for it in range(n):
        r = float(rng.choice([0.5, 0.6667, 0.75, 0.85, 0.9]))
        kind = rng.integers(0, 3)
        if kind == 0: d = rng.uniform(0, 1, (H, W, 4)).astype(F)
        else:
            yy, xx = np.mgrid[0:H, 0:W].astype(F); a = rng.uniform(0, np.pi); lv = rng.uniform(0, 1, (2, 4)).astype(F)
            side = (np.cos(a) * (xx - rng.uniform(0, W * r)) + np.sin(a) * (yy - rng.uniform(0, H * r))) > 0
            d = np.where(side[..., None], lv[0], lv[1]).astype(F)
            if kind == 2: d = (d + rng.uniform(-0.03, 0.03, d.shape)).astype(F)
        tex = SI.Texture(d, 'linear')
        ox, oy = int(rng.integers(0, W)), int(rng.integers(0, H))
        if rng.random() < 0.2: ox = W - 1 - int(rng.integers(0, 2))   # the drawn part's right / bottom edge: the clamp
        if rng.random() < 0.2: oy = H - 1 - int(rng.integers(0, 2))
        tc = (F((ox + 0.5) / W * r), F((oy + 0.5) / H * r))
        m.dpdx, m.dpdy = r / W, r / H
        c1 = struct.pack('<2fi', 1 / 12, 0.5, 1) + bytes(244)
        out = m.run({'FragCoord': (ox + 0.5, oy + 0.5, 0.5, 1.0), 1: (tc[0], tc[1], 0.0, 1.0)}, {16: PM.cbuf({13: (0, 0, W, H)}), 22: c1}, {10: tex})
        ex.append(m.executed)
        ref = easu_ref(d, tc, r)
        diff = float(np.max(np.abs(np.array(out[0][:3], F) - ref)))
        worst = max(worst, diff)
        if not (diff < 2e-6) or out[0][3] != 1.0:
            bad += 1
            if bad <= 3: print('  MISMATCH EASU r=%.3f px=(%d,%d) module=%s ref=%s' % (r, ox, oy, out[0][:3], ref))
    print('EASU path: %d random invocations (ratios 0.5 .. 0.9, edges, noise / steps), module vs numpy FsrEasuF by texel index: %d / %d within 2e-6 '
          '(worst %.3g); executed/pixel %.1f' % (n, n - bad, n, worst, np.mean(ex)))
    return bad


# ---------------------------------------------------------------------------------------------------------------- offline images
def easu_image(lo, out_w, out_h):
    """vectorised FsrEasuF of the whole output (same formulas as easu_ref; the input = the drawn part, so no clamp needed)"""
    h, w = lo.shape[:2]
    ox, oy = np.meshgrid(np.arange(out_w, dtype=F), np.arange(out_h, dtype=F))
    ppx = (ox + F(0.5)) * F(w / out_w) - F(0.5); ppy = (oy + F(0.5)) * F(h / out_h) - F(0.5)
    fx, fy = np.floor(ppx), np.floor(ppy); px, py = (ppx - fx).astype(F), (ppy - fy).astype(F)
    fx, fy = fx.astype(int), fy.astype(int)
    def t(dx, dy): return lo[np.clip(fy + dy, 0, h - 1), np.clip(fx + dx, 0, w - 1), :3]
    T = {'b': t(0, -1), 'c': t(1, -1), 'i': t(-1, 1), 'j': t(0, 1), 'f': t(0, 0), 'e': t(-1, 0), 'k': t(1, 1), 'l': t(2, 1), 'h': t(2, 0),
         'g': t(1, 0), 'o': t(1, 2), 'n': t(0, 2)}
    L = {k: v[..., 2] * F(0.5) + (v[..., 0] * F(0.5) + v[..., 1]) for k, v in T.items()}
    def rcp(a): return (np.uint32(0x7ef07ebb) - a.astype(F).view(np.uint32)).view(F)
    def rsq(a): return (np.uint32(0x5f347d74) - (a.astype(F).view(np.uint32) >> 1)).view(F)
    dx = np.zeros_like(px); dy = np.zeros_like(px); ln = np.zeros_like(px)
    with np.errstate(all='ignore'):
        for wt, (A, B, C, D, E) in (((1 - px) * (1 - py), 'befgj'), (px * (1 - py), 'cfghk'), ((1 - px) * py, 'fijkn'), (px * py, 'gjklo')):
            A, B, C, D, E = (L[k] for k in (A, B, C, D, E))
            lenX = rcp(np.maximum(abs(D - C), abs(C - B))); dirX = D - B; dx += dirX * wt
            lenX = np.clip(abs(dirX) * lenX, 0, 1) ** 2; ln += lenX * wt
            lenY = rcp(np.maximum(abs(E - C), abs(C - A))); dirY = E - A; dy += dirY * wt
            lenY = np.clip(abs(dirY) * lenY, 0, 1) ** 2; ln += lenY * wt
        dirR = dx * dx + dy * dy; zro = dirR < F(1.0 / 32768.0)
        dirR = np.where(zro, F(1), rsq(dirR)); dx = np.where(zro, F(1), dx); dx, dy = dx * dirR, dy * dirR
        l = (ln * F(0.5)) ** 2
        stretch = (dx * dx + dy * dy) * rcp(np.maximum(abs(dx), abs(dy)))
        l2x, l2y = F(1) + (stretch - F(1)) * l, F(1) - F(0.5) * l
        lob = F(0.5) + F((1.0 / 4.0 - 0.04) - 0.5) * l; clp = rcp(lob)
        aC = np.zeros(px.shape + (3,), F); aW = np.zeros_like(px)
        for name, off in (('b', (0, -1)), ('c', (1, -1)), ('i', (-1, 1)), ('j', (0, 1)), ('f', (0, 0)), ('e', (-1, 0)), ('k', (1, 1)),
                          ('l', (2, 1)), ('h', (2, 0)), ('g', (1, 0)), ('o', (1, 2)), ('n', (0, 2))):
            ox_, oy_ = off[0] - px, off[1] - py
            vx = (ox_ * dx + oy_ * dy) * l2x; vy = (ox_ * -dy + oy_ * dx) * l2y
            d2 = np.minimum(vx * vx + vy * vy, clp)
            wB = (F(0.4) * d2 - 1) ** 2; wA = (lob * d2 - 1) ** 2; wB = F(25.0 / 16.0) * wB - F(25.0 / 16.0 - 1.0)
            wt = wB * wA; aC += T[name] * wt[..., None]; aW += wt
        mn = np.minimum(np.minimum(T['f'], np.minimum(T['g'], T['j'])), T['k']); mx = np.maximum(np.maximum(T['f'], np.maximum(T['g'], T['j'])), T['k'])
        return np.minimum(mx, np.maximum(mn, aC / aW[..., None]))


def rcas(img, stops):
    """dm/android/shaders/rcas.comp (sharpness = exp2(-stops))"""
    p = np.pad(img, ((1, 1), (1, 1), (0, 0)), mode='edge')
    b, d, e, f, h = p[:-2, 1:-1], p[1:-1, :-2], p[1:-1, 1:-1], p[1:-1, 2:], p[2:, 1:-1]
    mn4 = np.minimum(np.minimum(b, d), np.minimum(f, h)); mx4 = np.maximum(np.maximum(b, d), np.maximum(f, h))
    hitMin = np.minimum(mn4, e) / np.maximum(4 * mx4, 1e-5); hitMax = (1 - np.maximum(mx4, e)) / np.minimum(4 * mn4 - 4, -1e-5)
    lobeRGB = np.maximum(-hitMin, hitMax)
    lobe = np.maximum(-(0.25 - 1.0 / 16.0), np.minimum(lobeRGB.max(-1), 0.0)) * (2.0 ** -stops)
    c = (lobe[..., None] * (b + d + f + h) + e) / (4 * lobe[..., None] + 1)
    return np.clip(c, 0, 1)


def images(src_dir, out_dir):
    from PIL import Image
    out = Path(out_dir); out.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, box in (('t4.png', (300, 120, 1500, 720)), ('light1.png', (600, 300, 1800, 900))):
        p = Path(src_dir) / name
        if not p.exists(): print('  (no %s)' % p); continue
        full = np.asarray(Image.open(p).convert('RGB').crop(box), dtype=np.float32) / 255.0
        H, W = full.shape[:2]
        for r in (0.85, 0.75, 0.67):
            lw, lh = int(round(W * r)), int(round(H * r))
            lo = np.asarray(Image.fromarray((full * 255 + 0.5).astype(np.uint8)).resize((lw, lh), Image.BOX), dtype=np.float32) / 255.0
            bil = np.asarray(Image.fromarray((lo * 255 + 0.5).astype(np.uint8)).resize((W, H), Image.BILINEAR), dtype=np.float32) / 255.0
            ea = easu_image(lo, W, H); ear = rcas(ea, 0.5); bilr = rcas(bil, 0.5)
            def psnr(a): return 10 * np.log10(1.0 / max(float(np.mean((a - full) ** 2)), 1e-12))
            rows.append('%s %3d %%: PSNR vs full res: bilinear %.2f dB, bilinear + RCAS 0.5 %.2f dB, EASU %.2f dB, EASU + RCAS 0.5 %.2f dB'
                        % (name, int(r * 100), psnr(bil), psnr(bilr), psnr(ea), psnr(ear)))
            cw = 420; ch = 300; x0, y0 = W // 2 - cw // 2, H // 2 - ch // 2
            tiles = [full, bil, ear]
            strip = np.concatenate([t_[y0:y0 + ch, x0:x0 + cw] for t_ in tiles], axis=1)
            Image.fromarray((np.clip(strip, 0, 1) * 255 + 0.5).astype(np.uint8)).resize((cw * 3 * 2, ch * 2), Image.NEAREST).save(
                out / ('fsr_%s_%d.png' % (Path(name).stem, int(r * 100))))
    (out / 'fsr_psnr.txt').write_text('\n'.join(rows) + '\n')
    print('\n'.join(rows)); print('side by side (full | bilinear today | EASU + RCAS 0.5), x2 nearest crops: %s' % out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--n', type=int, default=400)
    ap.add_argument('--images', default='')
    ap.add_argument('--out', default=str(WORK / 'fsr-shots'))
    a = ap.parse_args()
    nat, nat_h, tr_opt = build(a.bundles)
    bad = prove_11(nat, tr_opt, a.n) + prove_easu(nat, a.n)
    print('half module (-DEASU_H):'); bad += prove_11(nat_h, tr_opt, a.n) + prove_easu_h(nat_h, a.n)
    if a.images: error_bound_h(a.images, a.n * 5); images(a.images, a.out)
    if bad: raise SystemExit('%d failures' % bad)


if __name__ == '__main__':
    main()
