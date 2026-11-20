"""Divinity: Original Sin 2 DE: mobile "Light Shadows" resolve (CascadedPCF_<N>x<S> fragment stage) as native bundles (DM_GFX_NATIVE_SHADERS=1).

tools/dos2_pcf/CascadedPCF_mobile.frag is compiled with glslc once per (key, mode) and wrapped like tools/dos2_native_shaders.py does:
the translated bundle's header, binding table, input list and c1 bytes unchanged, only the SPIR-V replaced, so the host's head check
(xbg_game.cpp namespace native: head equal byte for byte) accepts it. The GLSL declares the translated bundle's own descriptor bindings
(4 / 22 / 28 / 34 / 10 / 16), so nothing is renumbered; the entry-point interface is checked against the translated SPIR-V.
Modes (subfolders of --out):  exact  = 9 point taps + manual compare (Switch result);  mobile = 4 textureGather taps (2x2 PCF each);
ipad16 = 16 textureGather taps on the iPad Poisson disc;  dref = mobile's 4 taps as hardware depth compares (sampler2DShadow: the
host turns compare LESS + linear filter on for that binding, PERF-X19 T11). Each folder: native/<key>.xgs, native.xgspack (only these keys) and, when
work/shaders-native/native exists, with-materials/native.xgspack (these keys + the material bundles of dos2_native_shaders.py).
Also writes <out>/stats.tsv (instruction / texture-op counts translated vs native).
Usage: dos2_pcf_mobile.py [--bundles work/shaders-vk] [--out work/shaders-native-mobile] [--materials work/shaders-native/native]
"""
import argparse, os, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'dm' / 'gfx' / 'tools'))
from dos2_native_shaders import Spv, read_bundle, write_bundle, tool, WORK
import xgs_pack

SRC = HERE / 'dos2_pcf' / 'CascadedPCF_mobile.frag'
# key -> (shader, cascades, atlas texels = 2 x cascade size)
KEYS = {'1372db843749e1d3': ('CascadedPCF_3x768', 3, 1536), 'a758e42af8853343': ('CascadedPCF_2x1024', 2, 2048),
        '2a221ee245d55aab': ('CascadedPCF_3x512', 3, 1024)}
MODES = {'exact': 0, 'mobile': 1, 'ipad16': 2, 'dref': 3}
# per pixel: (taps in one cascade, texels compared per tap); a pixel in a fade band samples two cascades
TAPS = {'translated': (9, 1), 'exact': (9, 1), 'mobile': (4, 4), 'ipad16': (16, 4), 'dref': (4, 4)}
TEX_OPS = {87: 'Sample', 88: 'SampleExplLod', 89: 'SampleDref', 90: 'SampleDrefExplLod', 95: 'Fetch', 96: 'Gather', 97: 'DrefGather'}


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: raise SystemExit('%s failed:\n%s' % (Path(cmd[0]).name, (r.stderr or r.stdout).strip()))


def tex_ops(spv):
    w = Spv(spv).w; out, i = {}, 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if op in TEX_OPS: out[TEX_OPS[op]] = out.get(TEX_OPS[op], 0) + 1
        i += n
    return ' '.join('%s=%d' % kv for kv in sorted(out.items()))


def opt_count(spv, tmp):
    a, b = Path(tmp) / 't.spv', Path(tmp) / 'to.spv'
    a.write_bytes(spv); run([tool('spirv-opt'), '-O', str(a), '-o', str(b)])
    return Spv(b.read_bytes()).count


def check_iface(nat, tr):
    n, t = Spv(nat), Spv(tr)
    for sc, what in ((1, 'inputs'), (3, 'outputs')):
        if n.io(sc) != t.io(sc): raise SystemExit('%s differ: native %s translated %s' % (what, n.io(sc), t.io(sc)))
    tb = {(s, b) for _, k, s, b, _ in t.descriptors()}
    extra = {(s, b) for _, k, s, b, _ in n.descriptors()} - tb
    if extra: raise SystemExit('native descriptors not in the translated module: %s' % sorted(extra))


# ------------------------------------------------------------------------------------------------- equivalence (NR-04, PCF_MODE 0)
QUAD = ((0.0, 0.0), (0.0, 0.5), (0.5, 0.0))


def case(key, rng, general, c1):
    """(inputs, ubos, textures) of one random invocation of CascadedPCF_<N>x<S>: a world point p = depth * ray + view position and, per
    cascade, a projection that puts p near (or outside) its atlas quadrant, so the walk, the edge / far fades, the blend into the next
    cascade, the last-cascade fade and the quadrant clamp of the taps all run; atlas depths random (point and bilinear samplers)."""
    import numpy as np
    import spv_interp as SI
    from dos2_post_mobile import support, cbuf, scales, W, H
    nc = KEYS[key][1]
    fc = (rng.integers(0, W) + 0.5, rng.integers(0, H) + 0.5, 0.5, 1.0)
    depth = float(rng.choice([rng.uniform(0.5, 80.0), rng.uniform(0.5, 80.0), 0.0, 1e4]))
    lin = np.full((H, W, 4), depth, dtype=np.float32) * rng.uniform(0.99, 1.01, (H, W, 1)).astype(np.float32)
    ray = rng.uniform(-1.5, 1.5, 3); vp = rng.uniform(-60, 60, 3)
    p = depth * ray + vp
    lp = (rng.uniform(0.2, 4.0), rng.uniform(0, 1), float(rng.choice([0.05, 0.1, 0.2, 0.5, 1.0])), rng.uniform(0.0, 2.0))
    far = sorted(rng.uniform(5, 200, 3)); wid = sorted(rng.uniform(8, 160, 3))
    k = {0: lp, 1: tuple(far) + (0.0,), 2: tuple(wid) + (0.0,)}
    for i in range(nc):
        r = rng.random()
        if r < 0.15: t = rng.uniform(-1.0, 2.0, 2)                                    # far outside: weight 0
        elif r < 0.45: t = np.array(QUAD[i]) + rng.choice([-0.01, 0.0, 0.002, 0.25, 0.497, 0.5, 0.51], 2) + rng.uniform(-0.003, 0.003, 2)
        else: t = np.array(QUAD[i]) + rng.uniform(-0.03, 0.53, 2)                     # inside, edge fades
        z = float(rng.choice([rng.uniform(-0.1, 1.1), rng.uniform(0.9, 1.05), rng.uniform(0.0, 0.5)]))
        R = rng.normal(0, 1, (3, 3)) * rng.uniform(0.002, 0.05)
        w3 = np.array([0.0, 0.0, 0.0, 1.0]) if rng.random() < 0.7 else np.append(rng.normal(0, 0.002, 3), rng.uniform(0.5, 2.0))
        wp = float(w3[:3] @ p + w3[3]) or 1.0
        tgt = np.array([t[0], t[1], z])
        rows = [np.append(R[j], tgt[j] - R[j] @ p) * wp for j in range(3)]
        for j, row in enumerate(rows): k[23 + 4 * i + j] = tuple(row)
        k[23 + 4 * i + 3] = tuple(w3)
    atlas = SI.Texture(rng.uniform(-0.05, 1.05, (48, 48, 4)).astype(np.float32), 'linear' if rng.random() < 0.5 else 'nearest')
    ubos = {4: support(scales(rng, general)), 22: cbuf({12: tuple(vp) + (1.0,)}), 28: cbuf(k, 40), 34: c1}
    return {'FragCoord': fc, 1: tuple(ray) + (0.0,)}, ubos, {10: SI.Texture(lin, 'nearest'), 16: atlas}


def equivalence(key, tr_opt, tr_raw, nat, c1, n, seed):
    """(report lines, per-pixel stats, failures): translated (spirv-opt -O; the raw one on every 8th case) and exact on the same inputs"""
    import numpy as np
    import spv_interp as SI
    from dos2_post_mobile import same
    name = KEYS[key][0]
    rng = np.random.default_rng(seed)
    mods = {'translated': SI.Module(tr_opt), 'native': SI.Module(nat)}
    raw = SI.Module(tr_raw)
    bad, raw_bad, nraw, lit, dmax, r8 = [], 0, 0, 0, 0.0, 0
    stats = {k: [[], []] for k in mods}
    for it in range(n):
        general = it >= (3 * n) // 4
        inp, ubo, tx = case(key, rng, general, c1)
        res = {}
        for k, m in mods.items():
            res[k] = m.run(inp, ubo, tx)
            if not general: stats[k][0].append(m.executed); stats[k][1].append(m.tex_ops)
        if res['translated'] and float(res['translated'][0][0]) not in (0.0, 1.0): lit += 1
        if not same(res['translated'], res['native']):
            bad.append((inp, res['translated'], res['native'], general))
            t, v = float(res['translated'][0][0]) if res['translated'] else 0.0, float(res['native'][0][0]) if res['native'] else 0.0
            dmax = max(dmax, abs(t - v)); r8 += round(min(max(t, 0.0), 1.0) * 255) != round(min(max(v, 0.0), 1.0) * 255)
        if it % 8 == 0:
            nraw += 1
            if not same(raw.run(inp, ubo, tx), res['translated']): raw_bad += 1
    lines = ['%s: %d random invocations (%d with render scale 1, %d with random scales; %d with a partial shadow), native == translated bit '
             'for bit: %d / %d; raw translated == spirv-opt -O translated: %d / %d' % (name, n, (3 * n) // 4, n - (3 * n) // 4, lit,
                                                                                    n - len(bad), n, nraw - raw_bad, nraw),
             '  %s: largest difference %.3g; in the pass R8_UNORM target (round to 1/255) %d of %d differ' % (name, dmax, r8, n)]
    for inp, t, nn, g in bad[:5]:
        lines.append('  MISMATCH general=%s in=%s translated=%s native=%s' % (g, inp, t, nn))
    st = {k: (float(np.mean(v[0])), float(np.mean(v[1])), int(np.max(v[0])), int(np.max(v[1]))) for k, v in stats.items()}
    return lines, st, len(bad) + raw_bad


def dref_vs_mobile(key, mob, dref, c1, n, seed):
    """(report lines, failures): the dref variant (hardware compare + bilinear, spv_interp Module._dref) against mobile (textureGather +
    manual compare + fract() weights) on the same random inputs, with an atlas of the real size (ATLAS texels: the shader's weights
    use that constant) holding depths in [0, 1] (a UNORM atlas). Equal up to float rounding of the weights; a tap whose 2x2 footprint
    the gather's fixed-point footprint choice (8 sub-texel bits) puts one texel over differs there with ~0 weight."""
    import numpy as np
    import spv_interp as SI
    name, nc, atlas = KEYS[key][0], KEYS[key][1], KEYS[key][2]
    rng = np.random.default_rng(seed)
    big = rng.uniform(0.0, 1.0, (atlas, atlas, 4)).astype(np.float32)
    big[rng.random((atlas, atlas)) < 0.3] = 1.0                                                       # cleared (far) texels
    tex = SI.Texture(big, 'linear')
    mods = {'mobile': SI.Module(mob), 'dref': SI.Module(dref)}
    dmax, r8, lit, cnt = 0.0, 0, 0, {k: [[], []] for k in mods}
    for it in range(n):
        inp, ubo, tx = case(key, rng, False, c1)
        tx[16] = tex
        res = {}
        for k, m in mods.items():
            res[k] = m.run(inp, ubo, tx); cnt[k][0].append(m.executed); cnt[k][1].append(m.tex_ops)
        a, b = float(res['mobile'][0][0]), float(res['dref'][0][0])
        lit += 0.0 < a < 1.0
        dmax = max(dmax, abs(a - b)); r8 += round(min(max(a, 0.0), 1.0) * 255) != round(min(max(b, 0.0), 1.0) * 255)
    st = {k: (float(np.mean(v[0])), float(np.mean(v[1]))) for k, v in cnt.items()}
    lines = ['%s: dref == mobile on %d random invocations (%d with a partial shadow, atlas %d x %d): largest difference %.3g, R8 %d of %d differ; '
             'executed / texture ops per pixel: mobile %.0f / %.1f, dref %.0f / %.1f' % (name, n, lit, atlas, atlas, dmax, r8, n,
             st['mobile'][0], st['mobile'][1], st['dref'][0], st['dref'][1])]
    return lines, int(dmax > 1e-3)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--out', default=str(WORK / 'shaders-native-mobile'))
    ap.add_argument('--materials', default=str(WORK / 'shaders-native' / 'native'))
    a = ap.parse_args()
    out, mats = Path(a.out), Path(a.materials)
    rows = ['key\tshader\tmode\tinstr_opt\ttexture_ops_static\ttaps_per_pixel(seam)\ttexels_compared(seam)\tspirv_bytes']
    with tempfile.TemporaryDirectory() as tmp:
        for key, (name, nc, atlas) in KEYS.items():
            tr = read_bundle(Path(a.bundles) / (key + '.xgs'))
            t, c = TAPS['translated']
            rows.append('%s\t%s\ttranslated\t%d\t%s\t%d (%d)\t%d (%d)\t%d' % (key, name, opt_count(tr['spirv'], tmp), tex_ops(tr['spirv']),
                                                                         t, 2 * t, t * c, 2 * t * c, len(tr['spirv'])))
            for mode, m in MODES.items():
                spv = Path(tmp) / ('%s_%s.spv' % (key, mode))
                run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-DNCASC=%d' % nc, '-DATLAS=%d.0' % atlas, '-DPCF_MODE=%d' % m,
                     '-fshader-stage=frag', '-o', str(spv), str(SRC)])
                run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(spv)])
                data = spv.read_bytes()
                check_iface(data, tr['spirv'])
                d = out / mode / 'native'; d.mkdir(parents=True, exist_ok=True)
                (d / (key + '.xgs')).write_bytes(write_bundle(tr, data))
                t, c = TAPS[mode]
                rows.append('%s\t%s\t%s\t%d\t%s\t%d (%d)\t%d (%d)\t%d' % (key, name, mode, Spv(data).count, tex_ops(data), t, 2 * t,
                                                                         t * c, 2 * t * c, len(data)))
    for mode in MODES:
        d = out / mode
        xgs_pack.build(str(d / 'native'), str(d / 'native.xgspack'))
        if mats.is_dir():
            w = d / 'with-materials'; staging = w / 'all'
            if staging.exists(): shutil.rmtree(staging)
            staging.mkdir(parents=True)
            for f in mats.glob('*.xgs'):
                if f.stem not in KEYS: shutil.copyfile(f, staging / f.name)
            for f in (d / 'native').glob('*.xgs'): shutil.copyfile(f, staging / f.name)
            xgs_pack.build(str(staging), str(w / 'native.xgspack'))
            shutil.rmtree(staging)
    (out / 'stats.tsv').write_text('\n'.join(rows) + '\n')
    print('\n'.join(rows))


if __name__ == '__main__':
    main()
