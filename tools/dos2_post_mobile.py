"""Divinity: Original Sin 2 DE: ALU-lighter full-screen post fragment stages as native bundles (DM_GFX_NATIVE_SHADERS=1).

Same method as tools/dos2_pcf_mobile.py (shadows): tools/dos2_post/<name>_mobile.frag is compiled with glslc, validated with spirv-val
(vulkan1.1), its entry-point interface checked against the translated module (inputs / outputs equal, descriptors a subset of the
translated bindings) and wrapped with the translated bundle's head byte for byte (header, binding table, inputs, c1) + the new SPIR-V.
Equivalence: tools/spv_interp.py (scalar SPIR-V interpreter) runs the translated module (spirv-opt -O, and the raw one on a subset)
and the native module on the same random inputs (textures, cbuffers, fragment coordinates / varyings, render scales 1 and non-1) and
compares the outputs bit for bit; it also counts executed instructions and texture instructions per pixel.
Output (default work/shaders-native-mobile): post/native/<key>.xgs, post/native.xgspack (post keys only), post/with-shadows/native.xgspack
(post + the 3 shadow keys of mobile/native), post/stats.tsv, post/equivalence.txt; and --pack-copy (default packs/2a/native.xgspack.post-mobile)
= a copy of post/with-shadows/native.xgspack.
Usage: dos2_post_mobile.py [--bundles work/shaders-vk] [--out work/shaders-native-mobile] [--n 400] [--only Fade] [--no-test]
"""
import argparse, random, shutil, struct, subprocess, sys, tempfile
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'dm' / 'gfx' / 'tools'))
from dos2_native_shaders import Spv, read_bundle, write_bundle, tool, WORK
from dos2_pcf_mobile import run, check_iface, opt_count, tex_ops
import spv_interp as SI
import xgs_pack

SRC = HERE / 'dos2_post'
# key -> (shader, source, Switch static samples / instr. as in TILE_SHADER_CANDIDATES, iPad)
SHADERS = {
    '62d683cc49953ad6': ('FadeBlending', 'FadeBlending_mobile.frag'),
    'e94ea226e689801c': ('EoCFog', 'EoCFog_mobile.frag'),
    '010472595b342188': ('EoCOutlineCombine', 'EoCOutlineCombine_mobile.frag'),
    'f23c09d198b55dc5': ('LinearizeDepth', 'LinearizeDepth_mobile.frag'),
    'cbe8649300e9c8cd': ('PPDownSampleLog2', 'PPDownSampleLog2_mobile.frag'),
    # NR-33A (engine post)
    'd8aeec640d465275': ('DeferredFog', 'DeferredFog_mobile.frag'),
    '4e290c9cf1162fe9': ('PPBloomUpsampleAndBlur', 'PPBloomUpsampleAndBlur_mobile.frag'),
    '98186c22a7209861': ('CombineUI_NoFetch', 'CombineUI_NoFetch_mobile.frag'),
    '74fe9ae2066d53da': ('PPBloomScaleBiasDownsampleAndBlur', 'PPBloomScaleBiasDownsampleAndBlur_mobile.frag'),
    '0e3fc07b320db1e9': ('PPBloomDownsampleAndBlur', 'PPBloomDownsampleAndBlur_mobile.frag'),
    '691768aa08545d95': ('PPFXAA', 'PPFXAA_mobile.frag'),
    # end NR-33A (engine post)
}
SHADOW_KEYS = ('1372db843749e1d3', 'a758e42af8853343', '2a221ee245d55aab')
W, H = 24, 16


# ---------------------------------------------------------------------------------------------------------------------- random inputs
def support(scales):
    b = bytearray(1392); struct.pack_into('<4f', b, 144, 1 / W, 1 / H, 0, 0); struct.pack_into('<4f', b, 160, W, H, 0, 0)
    for i in range(73): struct.pack_into('<f', b, 192 + 16 * i, 1.0)
    for i, s in enumerate(scales): struct.pack_into('<f', b, 192 + 16 * i, s)
    return bytes(b)


def cbuf(vec4s, n=16):
    b = bytearray(16 * n)
    for i, v in vec4s.items(): struct.pack_into('<4f', b, 16 * i, *v)
    return bytes(b)


def tex(rng, lo=0.0, hi=1.0, alpha=None, special=None):
    d = rng.uniform(lo, hi, (H, W, 4)).astype(np.float32)
    if alpha is not None: d[..., 3] = rng.choice(alpha, (H, W))
    if special is not None:
        m = rng.random((H, W, 4)) < 0.25; d[m] = rng.choice(special, m.sum())
    return SI.Texture(d, 'linear')


def scales(rng, general):
    if not general: return (1.0, 1.0, 1.0, 1.0, 1.0)
    return tuple(float(rng.choice([1.0, 0.5, 2.0, -2.0, -1.5])) for _ in range(5))


def case(name, rng, general):
    """(inputs, ubos, textures) of one random invocation"""
    fc = (rng.integers(0, W) + 0.5, rng.integers(0, H) + 0.5, 0.5, 1.0)
    uv = (rng.uniform(-0.1, 1.1), rng.uniform(-0.1, 1.1), 0.0, 1.0)
    sb = support(scales(rng, general))
    if name == 'FadeBlending':
        wh = (float(rng.choice([W, W - 3])), float(rng.choice([H, H - 2])))
        return ({'FragCoord': fc}, {4: sb, 22: cbuf({13: (0, 0) + wh})},
                {10: tex(rng, alpha=[0.0, 0.0, 1.0, 1.0, 0.25, 0.5, 0.8]), 16: tex(rng, 0.0, 4.0)})
    if name == 'EoCFog':
        k = {0: (rng.uniform(0.5, 4), rng.uniform(0.5, 4), rng.uniform(0.5, 4), float(rng.choice([-0.3, 0.0, 0.4]))),
             1: (rng.uniform(0.5, 4), rng.uniform(0.5, 4), rng.uniform(0.5, 4), rng.uniform(-50, 300)),
             2: (rng.uniform(-3, 3), rng.uniform(-3, 3), 0, 0)}
        ray = (rng.uniform(-2, 2), rng.uniform(-2, 2), rng.uniform(-2, 2), 0.0)
        return ({'FragCoord': fc, 1: uv, 2: ray}, {34: cbuf(k), 40: cbuf({12: (rng.uniform(-5, 5), 3, rng.uniform(-5, 5), 1)})},
                {10: tex(rng, 0.0, 1.0, special=[1.0, 1.0, 0.999]), 16: tex(rng, 0.0, 6.0), 22: tex(rng), 28: tex(rng)})
    if name == 'EoCOutlineCombine':
        c1 = struct.pack('<f', 0.15) + bytes(252)
        return ({'FragCoord': fc, 1: uv}, {4: sb, 28: c1},
                {10: tex(rng, alpha=[0.0, 0.0, 1.0, 0.3, 0.7]), 16: tex(rng, 0, 3), 22: tex(rng, 0, 2, alpha=[0.0, 1.0, 0.4, 0.9])})
    if name == 'LinearizeDepth':
        return ({'FragCoord': fc, 1: uv}, {16: cbuf({0: (rng.uniform(1.0, 1.2), rng.uniform(0.01, 2), 0, 0)})},
                {10: tex(rng, 0.0, 1.0, special=[0.0, 1.0, 0.5])})
    if name == 'PPDownSampleLog2':
        return ({'FragCoord': fc, 1: uv}, {}, {10: tex(rng, 0.0, 8.0, special=[0.0, 2.0, 2.0000002, np.inf, -np.inf, np.nan, 1e38, -1.0])})
    # NR-33A (engine post)
    uvn = (float(rng.choice([uv[0], uv[0], uv[0], np.nan])), float(rng.choice([uv[1], uv[1], uv[1], np.nan])), 0.0, 1.0)
    if name == 'DeferredFog':
        # K4..6 = fog colour x (far), y (near) per channel; z / w = distance start (K4) and end (K5), equal ends -> 1/0;
        # K6.z / K6.w = height base / range (0 -> 1/0)
        zs, ws = rng.uniform(0, 20), rng.uniform(0, 20)
        ze, we = float(rng.choice([zs, zs + rng.uniform(0.1, 60)])), float(rng.choice([ws, ws + rng.uniform(0.1, 60)]))
        k = {4: (rng.uniform(0, 2), rng.uniform(0, 2), zs, ws), 5: (rng.uniform(0, 2), rng.uniform(0, 2), ze, we),
             6: (rng.uniform(0, 2), rng.uniform(0, 2), rng.uniform(-5, 5), float(rng.choice([0.0, rng.uniform(-10, 10), rng.uniform(-10, 10)])))}
        ray = (rng.uniform(-30, 30), rng.uniform(-30, 30), rng.uniform(-30, 30), 0.0)
        return ({'FragCoord': fc, 2: ray}, {4: sb, 22: cbuf(k), 28: cbuf({12: (0, rng.uniform(-5, 5), 0, 1)})},
                {10: tex(rng, 0.0, 1.0, special=[0.0, 1.0, np.nan, np.inf]), 16: tex(rng, 0.0, 6.0, special=[0.0, np.nan])})
    if name == 'PPBloomUpsampleAndBlur':
        wh = (float(rng.choice([W, W / 2, rng.uniform(1, 64)])), float(rng.choice([H, H / 2, rng.uniform(1, 64)])))
        return ({1: uvn}, {22: cbuf({13: (0, 0) + wh}), 28: cbuf({0: (rng.uniform(0, 2), 0, 0, 0)})},
                {10: tex(rng, 0.0, 4.0), 16: tex(rng, 0.0, 4.0)})
    if name == 'CombineUI_NoFetch':
        th = float(rng.choice([0.001, 0.001, 0.001, rng.uniform(0, 1)]))   # c1[0].x of the bundle = 0.001
        c1 = struct.pack('<f', th) + bytes(252)
        off = (float(rng.choice([0.0, 0.0, 0.5, 3.0, -2.0])), float(rng.choice([0.0, 0.0, 0.5, 2.0])), 0, 0)
        g = float(rng.choice([2.2, 1.0, 0.4545, rng.uniform(0.3, 3)]))
        ui = tex(rng, -0.2, 1.3, alpha=[0.0, 0.0005, 0.001, 0.3, 0.7, 1.0, 1.0, np.nan], special=[0.0, 1.0, np.nan])
        return ({'FragCoord': fc, 1: uvn}, {4: sb, 22: cbuf({0: off, 1: (0, g, 0, 0)}), 28: c1},
                {10: ui, 16: tex(rng, 0.0, 1.5)})
    if name in ('PPBloomScaleBiasDownsampleAndBlur', 'PPBloomDownsampleAndBlur'):
        wh = (float(rng.choice([W, W / 2, rng.uniform(1, 64)])), float(rng.choice([H, H / 2, rng.uniform(1, 64)])))
        src = tex(rng, 0.0, 4.0, special=[0.0, np.nan, np.inf])
        if name == 'PPBloomDownsampleAndBlur': return ({1: uvn}, {16: cbuf({13: (0, 0) + wh})}, {10: src})
        w = (0.59, 0.11) if rng.random() < 0.75 else (rng.uniform(0, 1), rng.uniform(0, 1))   # c1[0].xy of the bundle = 0.59, 0.11
        c1 = struct.pack('<2f', *w) + bytes(248)
        return ({1: uvn}, {16: cbuf({13: (0, 0) + wh}), 22: c1, 28: cbuf({0: (rng.uniform(0, 2), float(rng.choice([0.0, rng.uniform(0.1, 4)])), 0, 0)})},
                {10: src})
    if name == 'PPFXAA':
        kind = rng.integers(0, 4)
        if kind == 0: img = tex(rng, special=[0.0, 1.0, np.nan])                     # noise: short spans, NaN texels
        else:                                                                        # straight edges at random angles: long spans
            yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
            a = rng.uniform(0, np.pi); lvl = rng.uniform(0, 1, 2)
            side = (np.cos(a) * (xx - rng.uniform(0, W)) + np.sin(a) * (yy - rng.uniform(0, H))) > 0
            d = np.where(side[..., None], lvl[0], lvl[1]).repeat(4, 2) + rng.uniform(-0.02, 0.02, (H, W, 4)) * (kind == 1)
            if kind == 3: d[rng.random((H, W)) < 0.04] = np.float32(rng.choice([0.0, 1.0]))
            img = SI.Texture(d.astype(np.float32), 'linear')
        px, py = rng.integers(0, W) + 0.5, rng.integers(0, H) + 0.5
        wh = (float(W), float(H)) if rng.random() < 0.8 else (rng.uniform(8, 64), rng.uniform(8, 64))
        u = (px / W, py / H) if rng.random() < 0.85 else (rng.uniform(-0.1, 1.1), rng.uniform(-0.1, 1.1))
        u = (float(rng.choice([u[0]] * 15 + [np.nan])), float(rng.choice([u[1]] * 15 + [np.nan])), 0.0, 1.0)
        k = (1 / 12, 0.5) if rng.random() < 0.8 else (rng.uniform(0, 0.3), rng.uniform(0, 1))   # c1 of the bundle: 1/12, 0.5, int 1
        c1 = struct.pack('<2fi', k[0], k[1], int(rng.choice([1, 1, 1, 0, 2]))) + bytes(244)
        return ({'FragCoord': (px, py, 0.5, 1.0), 1: u}, {16: cbuf({13: (0, 0) + wh}), 22: c1}, {10: img})
    # end NR-33A (engine post)
    raise KeyError(name)


def same(a, b):
    if a is None or b is None: return a is b
    return all(struct.pack('<f', x) == struct.pack('<f', y) or (np.isnan(x) and np.isnan(y)) for x, y in zip(a[0], b[0]))


def equivalence(name, tr_opt, tr_raw, nat, n, seed):
    """returns (report lines, per-pixel stats {variant: (instr mean, tex mean)})"""
    rng = np.random.default_rng(seed)
    mods = {'translated': SI.Module(tr_opt), 'native': SI.Module(nat)}
    raw = SI.Module(tr_raw)
    bad, raw_bad, stats = [], 0, {'translated': [[], []], 'native': [[], []]}
    nraw = 0
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
            nraw += 1
            if not same(raw.run(inp, ubo, tx), res['translated']): raw_bad += 1
    lines = ['%s: %d random invocations (%d with render scale 1, %d with random scales), native == translated bit for bit: %d / %d;'
             ' raw translated == spirv-opt -O translated: %d / %d' % (name, n, (3 * n) // 4, n - (3 * n) // 4, n - len(bad), n,
                                                                       nraw - raw_bad, nraw)]
    for inp, t, nn, g in bad[:5]:
        lines.append('  MISMATCH general=%s in=%s translated=%s native=%s' % (g, inp, t, nn))
    st = {k: (float(np.mean(v[0])), float(np.mean(v[1])), int(np.max(v[0])), int(np.max(v[1]))) for k, v in stats.items()}
    return lines, st, len(bad) + raw_bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--out', default=str(WORK / 'shaders-native-mobile'))
    ap.add_argument('--pack-copy', default=str(HERE.parent.parent / 'packs' / '2a' / 'native.xgspack.post-mobile'))
    ap.add_argument('--n', type=int, default=400)
    ap.add_argument('--only', default='')
    ap.add_argument('--no-test', action='store_true')
    a = ap.parse_args()
    out = Path(a.out) / 'post'; nd = out / 'native'
    if nd.exists(): shutil.rmtree(nd)
    nd.mkdir(parents=True)
    rows = ['key\tshader\tinstr_opt translated\tinstr_opt native\texec/pixel translated (max)\texec/pixel native (max)\t'
            'tex/pixel translated (max)\ttex/pixel native (max)\ttexture_ops_static translated\ttexture_ops_static native\tspirv_bytes tr/nat']
    eq, fails = [], 0
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
            rows.append('%s\t%s\t%d\t%d\t%.1f (%d)\t%.1f (%d)\t%.2f (%d)\t%.2f (%d)\t%s\t%s\t%d/%d' % (
                key, name, Spv(tr_opt).count, Spv(data).count, st['translated'][0], st['translated'][2], st['native'][0],
                st['native'][2], st['translated'][1], st['translated'][3], st['native'][1], st['native'][3], tex_ops(tr['spirv']),
                tex_ops(data), len(tr['spirv']), len(data)))
    xgs_pack.build(str(nd), str(out / 'native.xgspack'))
    shadows = Path(a.out) / 'mobile' / 'native'
    w = out / 'with-shadows'; staging = w / 'all'
    if staging.exists(): shutil.rmtree(staging)
    staging.mkdir(parents=True)
    for k in SHADOW_KEYS:
        if (shadows / (k + '.xgs')).exists(): shutil.copyfile(shadows / (k + '.xgs'), staging / (k + '.xgs'))
    for f in nd.glob('*.xgs'): shutil.copyfile(f, staging / f.name)
    xgs_pack.build(str(staging), str(w / 'native.xgspack'))
    shutil.rmtree(staging)
    if a.pack_copy:
        Path(a.pack_copy).parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(w / 'native.xgspack', a.pack_copy)
    (out / 'stats.tsv').write_text('\n'.join(rows) + '\n')
    if eq: (out / 'equivalence.txt').write_text('\n'.join(eq) + '\n')
    print('\n'.join(rows))
    if fails: raise SystemExit('%d equivalence failures' % fails)


if __name__ == '__main__':
    main()
