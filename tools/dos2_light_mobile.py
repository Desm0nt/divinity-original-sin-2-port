"""Divinity: Original Sin 2 DE: ALU-lighter full-screen lighting fragment stages (DeferredSky, ClusteredLightingWithSSAO) as native bundles.

Same method as tools/dos2_post_mobile.py: tools/dos2_post/<name>_mobile.frag -> glslc -O (vulkan1.1) -> spirv-val -> interface check against
the translated module -> bundle = translated head byte for byte + new SPIR-V (DM_GFX_NATIVE_SHADERS=1 swaps it in).
Equivalence: tools/spv_interp.py runs the translated module (spirv-opt -O; the raw one on every 8th invocation) and the native one on the same
random inputs and compares the outputs bit for bit. Inputs: G-buffer textures with every shading model / FXEmissive flag, sky depth 1,
render scales 1 (3/4 of the runs) and random scales; for the lighting stage also a random light set (c13: 48 lights, pos / 1/range incl. 0 /
colour / cone), a cluster grid (c12: offset + point | spot << 16 counts per cluster, up to 8 + 4 lights) and the byte index list (c11), sized
like the game's (64 KiB banks), world positions around the lights so that the range / cone / back-face branches all run.
Counts: executed instructions per pixel (render scale 1) on the same random inputs and on fixed scenarios (no lights, 1 point, 1 spot,
8 point + 4 spot; standard and anisotropic shading model).
Output (default work/shaders-native-mobile): light/native/<key>.xgs, light/native.xgspack (these keys), light/all/native.xgspack
(3 shadow keys of mobile/native + 5 post keys of post/native + these) = copy --pack-copy (default packs/2a/native.xgspack.light-mobile);
light/stats.tsv, light/scenarios.tsv, light/equivalence.txt.
Usage: dos2_light_mobile.py [--bundles work/shaders-vk] [--out work/shaders-native-mobile] [--n 400] [--only Sky] [--no-test]
"""
import argparse, shutil, struct, sys, tempfile
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'dm' / 'gfx' / 'tools'))
from dos2_native_shaders import Spv, read_bundle, write_bundle, tool, WORK
from dos2_pcf_mobile import run, check_iface, tex_ops
from dos2_post_mobile import cbuf, SHADERS as POST_KEYS, SHADOW_KEYS
import spv_interp as SI
import xgs_pack

SRC = HERE / 'dos2_post'
SHADERS = {
    '640a4c497bcc748b': ('DeferredSky', 'DeferredSky_mobile.frag'),
    'd236b8d5b7ffad3c': ('ClusteredLightingWithSSAO', 'ClusteredLightingWithSSAO_mobile.frag'),
}
FW, FH = 1800, 806          # frame (gl_FragCoord range, so the 128-pixel cluster tiles vary)
NL = 48                     # lights in c13


class BigTex(SI.Texture):
    """logical w x h texture backed by a small prime-sized random table (fetch only), out of bounds = 0"""
    def __init__(self, table, w, h):
        SI.Texture.__init__(self, table); self.lw, self.lh = w, h

    def fetch(self, x, y):
        if not (0 <= x < self.lw and 0 <= y < self.lh): return [SI.F(0)] * 4
        return [SI.F(v) for v in self.d[y % self.h, x % self.w]]


def table(rng, lo=0.0, hi=1.0, special=None, shape=(23, 37)):
    d = rng.uniform(lo, hi, shape + (4,)).astype(np.float32)
    if special is not None:
        m = rng.random(shape + (4,)) < 0.25; d[m] = rng.choice(special, m.sum())
    return d


def support(scales):
    b = bytearray(1392); struct.pack_into('<4f', b, 144, 1 / FW, 1 / FH, 0, 0); struct.pack_into('<4f', b, 160, FW, FH, 0, 0)
    for i in range(73): struct.pack_into('<f', b, 192 + 16 * i, 1.0)
    for i, s in enumerate(scales): struct.pack_into('<f', b, 192 + 16 * i, s)
    return bytes(b)


def scales(rng, general):
    if not general: return (1.0,) * 10
    return tuple(float(rng.choice([1.0, 1.0, 0.5, 2.0, -2.0, -1.5])) for _ in range(10))


def gbuf0(rng, models=(0, 0, 0, 1, 2, 3)):
    """t_E: roughness, reflectance, metal, flags = (FXEmissive + 2 * model) / 255"""
    d = table(rng)
    d[..., 2] = np.where(rng.random(d.shape[:2]) < 0.4, rng.choice([0.0, 1.0], d.shape[:2]), d[..., 2])
    d[..., 3] = (rng.choice([0, 1, 0, 0], d.shape[:2]) + 2 * rng.choice(models, d.shape[:2])) / 255.0
    return d


def rotation(rng):
    q, _ = np.linalg.qr(rng.normal(size=(3, 3)))
    return q.astype(np.float32)


def unit(rng):
    v = rng.normal(size=3); return v / np.linalg.norm(v)


def lights(rng, cam, spread):
    c13 = bytearray(65536)
    for i in range(NL):
        pos = cam + rng.uniform(-spread, spread, 3)
        r = float(rng.choice([rng.uniform(2, 25), rng.uniform(2, 25), rng.uniform(0.5, 4), 0.0]))
        ir = 1.0 / r if r else 0.0
        col = rng.uniform(0, 4, 3)
        co = rng.uniform(-0.2, 0.97); ci = min(co + rng.uniform(0.0, 0.2), 1.0)
        struct.pack_into('<12f', c13, 48 * i, *pos, ir, *col, co, *(unit(rng) * rng.uniform(0.5, 2)), ci)
    return bytes(c13)


def clusters(rng, maxp, maxs, fixed=None):
    """c12: per cluster (offset into the c11 byte list, points | spots << 16); c11: light index bytes"""
    n = 16384 // 2
    if fixed is None:
        cnt = rng.integers(0, maxp + 1, n) | (rng.integers(0, maxs + 1, n) << 16)
    else:
        cnt = np.full(n, fixed[0] | (fixed[1] << 16))
    off = rng.integers(0, 65536 - 32, n)
    c12 = np.stack([off, cnt], 1).astype(np.uint32).tobytes()
    c11 = rng.integers(0, NL, 65536).astype(np.uint8).tobytes()
    return c12, c11


def case(name, rng, general, scen=None):
    """(inputs, ubos, textures) of one random invocation; scen = (points, spots, model) for the fixed scenarios"""
    fc = (rng.integers(0, FW) + 0.5, rng.integers(0, FH) + 0.5, 0.5, 1.0)
    sb = support(scales(rng, general))
    models = (0, 0, 0, 1, 2, 3) if scen is None else (scen[2],)
    g0 = gbuf0(rng, models)
    depth = table(rng, 0.02, 1.0, special=None if scen else [1.0, 1.0, 0.5])
    exposure = table(rng, 0.2, 3.0, special=[0.0], shape=(1, 1))
    nrm = table(rng, 0.05, 0.95)
    k1 = (3.5554, float(rng.choice([0.02, 0.05, 0.3])), float(rng.uniform(0.2, 1.0)), 1e-4)
    if name == 'DeferredSky':
        cube = lambda: SI.CubeTexture(rng.uniform(0, 3, (6, 4, 4, 4)).astype(np.float32))
        rot = rotation(rng)
        c8 = {i: tuple(rot[i]) + (0.0,) for i in range(3)}
        tx = {10: BigTex(g0, FW, FH), 16: BigTex(depth, FW, FH), 22: BigTex(nrm, FW, FH), 28: cube(),
              34: BigTex(table(rng), FW, FH), 40: BigTex(exposure, 1, 1), 46: SI.Texture(table(rng, shape=(8, 8))), 52: cube()}
        ub = {4: sb, 58: cbuf({0: k1}), 64: cbuf(c8), 70: cbuf({2: (0.0, float(rng.uniform(0, 8)), float(rng.uniform(0, 2)), 0.0)})}
        ray = tuple(rng.uniform(-30, 30, 3)) + (0.0,)
        return {'FragCoord': fc, 2: ray}, ub, tx
    if name == 'ClusteredLightingWithSSAO':
        cam = np.array([5.0, 15.0, -3.0])
        rot = rotation(rng)
        c4 = {0: (0, 0, 0, float(rng.choice([-1.0, 0.5]))), 1: tuple(rot[0]) + (0.0,), 2: tuple(rot[1]) + (0.0,), 3: tuple(rot[2]) + (0.0,)}
        far = float(rng.uniform(100, 400))
        c15 = {12: tuple(cam) + (1.0,), 13: (far, 0, 0, 0), 14: (float(rng.uniform(4, 8)), float(rng.uniform(-2, 4)), 15.0, 7.0)}
        sun = unit(rng)
        # fixed scenarios isolate the lights: no sun colour, ambient or emissive, so a 1-ulp change in a light term reaches the output
        c16 = {i: (float(sun[i]), 0.0, float(rng.uniform(0, 3)) if scen is None else 0.0, 0.0) for i in range(3)}
        c16[7] = (0.0, float(rng.choice([0.0, 1.0])), 0.0, 0.0)
        kd = float(rng.choice([0.318309873, 1.0])) if scen is None else float(rng.choice([0.0, 0.318309873]))   # kd 0: specular only
        k = {0: k1, 1: (1e-6, 1.0, kd, 1e-4), 2: (2.0 / 4095.0, float(rng.uniform(-0.9, 0.9)), 1e-3, 0.0)}
        if scen is None: c12, c11 = clusters(rng, 8, 4)
        else: c12, c11 = clusters(rng, 0, 0, scen[:2])
        aniso = table(rng)
        lo, hi = (0.0, 2.0) if scen is None else (0.0, 0.0)
        if scen is not None: g0[..., 3] = (2 * scen[2]) / 255.0; g0[..., 0] = rng.uniform(0.4, 1.0, g0.shape[:2])   # wide lobes
        tx = {10: BigTex(g0, FW, FH), 16: BigTex(nrm, FW, FH), 22: BigTex(depth, FW, FH), 28: BigTex(table(rng, lo, hi), FW, FH),
              34: BigTex(table(rng), FW, FH), 40: BigTex(aniso, FW, FH), 46: BigTex(exposure, 1, 1), 52: BigTex(table(rng, lo, hi), FW, FH),
              58: BigTex(table(rng, special=[0.0, 1.0]), FW, FH)}
        ub = {4: sb, 64: c12, 70: c11, 76: lights(rng, cam, 22.0), 82: cbuf(c16), 88: cbuf(k), 94: cbuf(c4), 100: cbuf(c15)}
        ray = tuple(rng.uniform(-30, 30, 3)) + (0.0,)
        return {'FragCoord': fc, 2: ray}, ub, tx
    raise KeyError(name)


def same(a, b):
    """outputs of two runs ({location: vec4} or None = discard) equal bit for bit (any NaN == any NaN)"""
    if a is None or b is None: return a is b
    if set(a) != set(b): return False
    return all(struct.pack('<f', x) == struct.pack('<f', y) or (np.isnan(x) and np.isnan(y)) for o in a for x, y in zip(a[o], b[o]))


def equivalence(name, tr_opt, tr_raw, nat, n, seed):
    rng = np.random.default_rng(seed)
    mods = {'translated': SI.Module(tr_opt), 'native': SI.Module(nat)}
    raw = SI.Module(tr_raw)
    bad, raw_bad, nraw = [], 0, 0
    stats = {k: [[], []] for k in mods}
    for it in range(n):
        general = it >= (3 * n) // 4
        inp, ubo, tx = case(name, rng, general)
        res = {}
        for k, m in mods.items():
            res[k] = m.run(inp, ubo, tx)
            if not general: stats[k][0].append(m.executed); stats[k][1].append(m.tex_ops)
        if not same(res['translated'], res['native']):
            bad.append((inp, res['translated'], res['native'], general))
        if it % 8 == 0:
            nraw += 1; r = raw.run(inp, ubo, tx)
            if not same(r, res['translated']): raw_bad += 1
    lines = ['%s: %d random invocations (%d with render scale 1, %d with random scales), native == translated bit for bit: %d / %d;'
             ' raw translated == spirv-opt -O translated: %d / %d' % (name, n, (3 * n) // 4, n - (3 * n) // 4, n - len(bad), n,
                                                                       nraw - raw_bad, nraw)]
    for inp, t, nn, g in bad[:5]:
        lines.append('  MISMATCH general=%s in=%s translated=%s native=%s' % (g, inp, t, nn))
    st = {k: (float(np.mean(v[0])), float(np.mean(v[1])), int(np.max(v[0])), int(np.max(v[1]))) for k, v in stats.items()}
    return lines, st, len(bad) + raw_bad


SCENARIOS = [('no lights, model 0', (0, 0, 0)), ('1 point, model 0', (1, 0, 0)), ('1 spot, model 0', (0, 1, 0)),
             ('8 point + 4 spot, model 0', (8, 4, 0)), ('no lights, aniso', (0, 0, 2)), ('1 point, aniso', (1, 0, 2)),
             ('1 spot, aniso', (0, 1, 2)), ('8 point + 4 spot, aniso', (8, 4, 2))]


def scenarios(tr_opt, nat, n=60, seed=7):
    """executed instructions per pixel (render scale 1, lit pixels) for fixed light counts; also re-checks equality"""
    rows, fails = [], 0
    mods = {'translated': SI.Module(tr_opt), 'native': SI.Module(nat)}
    for label, scen in SCENARIOS:
        rng = np.random.default_rng(seed)
        ex = {k: [] for k in mods}; tx = {k: [] for k in mods}
        for _ in range(n):
            inp, ubo, t = case('ClusteredLightingWithSSAO', rng, False, scen)
            r = {}
            for k, m in mods.items():
                r[k] = m.run(inp, ubo, t); ex[k].append(m.executed); tx[k].append(m.tex_ops)
            if not same(r['translated'], r['native']): fails += 1
        rows.append('%s\t%.1f\t%.1f\t%.2f\t%.2f' % (label, np.mean(ex['translated']), np.mean(ex['native']), np.mean(tx['translated']),
                                                   np.mean(tx['native'])))
    return rows, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--out', default=str(WORK / 'shaders-native-mobile'))
    ap.add_argument('--pack-copy', default=str(HERE.parent.parent / 'packs' / '2a' / 'native.xgspack.light-mobile'))
    ap.add_argument('--n', type=int, default=400)
    ap.add_argument('--only', default='')
    ap.add_argument('--no-test', action='store_true')
    a = ap.parse_args()
    root = Path(a.out); out = root / 'light'; nd = out / 'native'
    if nd.exists(): shutil.rmtree(nd)
    nd.mkdir(parents=True)
    rows = ['key\tshader\tinstr_opt translated\tinstr_opt native\texec/pixel translated (max)\texec/pixel native (max)\t'
            'tex/pixel translated (max)\ttex/pixel native (max)\ttexture_ops_static translated\ttexture_ops_static native\tspirv_bytes tr/nat']
    eq, fails, scen_rows = [], 0, []
    with tempfile.TemporaryDirectory() as tmp:
        for key, (name, src) in SHADERS.items():
            if a.only and a.only not in name: continue
            tr = read_bundle(Path(a.bundles) / (key + '.xgs'))
            spv = Path(tmp) / (key + '.spv')
            run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', '-I', str(SRC), '-o', str(spv), str(SRC / src)])
            run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(spv)])
            data = spv.read_bytes()
            check_iface(data, tr['spirv'])
            (nd / (key + '.xgs')).write_bytes(write_bundle(tr, data))
            to = Path(tmp) / (key + '.tr.spv'); too = Path(tmp) / (key + '.tro.spv')
            to.write_bytes(tr['spirv']); run([tool('spirv-opt'), '-O', str(to), '-o', str(too)])
            tr_opt = too.read_bytes()
            st = {'translated': (0, 0, 0, 0), 'native': (0, 0, 0, 0)}
            if not a.no_test:
                lines, st, nf = equivalence(name, tr_opt, tr['spirv'], data, a.n, int(key[:8], 16))
                eq += lines; fails += nf; print('\n'.join(lines))
                if name == 'ClusteredLightingWithSSAO':
                    scen_rows, sf = scenarios(tr_opt, data)
                    fails += sf
                    eq.append('%s: fixed light-count scenarios (%d x 60 lit pixels): %s' % (name, len(SCENARIOS),
                                                                                          'all equal' if not sf else '%d MISMATCHES' % sf))
            rows.append('%s\t%s\t%d\t%d\t%.1f (%d)\t%.1f (%d)\t%.2f (%d)\t%.2f (%d)\t%s\t%s\t%d/%d' % (
                key, name, Spv(tr_opt).count, Spv(data).count, st['translated'][0], st['translated'][2], st['native'][0],
                st['native'][2], st['translated'][1], st['translated'][3], st['native'][1], st['native'][3], tex_ops(tr['spirv']),
                tex_ops(data), len(tr['spirv']), len(data)))
    xgs_pack.build(str(nd), str(out / 'native.xgspack'))
    staging = out / 'all' / 'stage'
    if staging.exists(): shutil.rmtree(staging)
    staging.mkdir(parents=True)
    for k in SHADOW_KEYS:
        f = root / 'mobile' / 'native' / (k + '.xgs')
        if f.exists(): shutil.copyfile(f, staging / f.name)
    for k in POST_KEYS:
        f = root / 'post' / 'native' / (k + '.xgs')
        if f.exists(): shutil.copyfile(f, staging / f.name)
    for f in nd.glob('*.xgs'): shutil.copyfile(f, staging / f.name)
    nkeys = len(list(staging.glob('*.xgs')))
    xgs_pack.build(str(staging), str(out / 'all' / 'native.xgspack'))
    if a.pack_copy:
        Path(a.pack_copy).parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(out / 'all' / 'native.xgspack', a.pack_copy)
    shutil.rmtree(staging)
    (out / 'stats.tsv').write_text('\n'.join(rows) + '\n')
    if scen_rows:
        (out / 'scenarios.tsv').write_text('scenario (ClusteredLightingWithSSAO, lit pixels, render scale 1)\texec/pixel translated\t'
                                           'exec/pixel native\ttex/pixel translated\ttex/pixel native\n' + '\n'.join(scen_rows) + '\n')
        print('\n'.join(scen_rows))
    if eq: (out / 'equivalence.txt').write_text('\n'.join(eq) + '\n')
    print('\n'.join(rows))
    print('pack: %s (%d keys, %d bytes)' % (out / 'all' / 'native.xgspack', nkeys, (out / 'all' / 'native.xgspack').stat().st_size))
    if fails: raise SystemExit('%d equivalence failures' % fails)


if __name__ == '__main__':
    main()
