"""DOS2 NR-LIGHTFUSE (docs/dos2/nr/NR-LIGHTFUSE.md): the fused DeferredSky -> ClusteredLightingWithSSAO fragment shader ("f." variant of
ClusteredLightingWithSSAO in native-nr.xgspack, setup stage 8 via tools/dos2_nr_sh_pack.py) and its proof on tools/spv_interp.py.

The chain it replaces, per pixel: DeferredSky (native, = translated bit for bit) writes Sp = q(spec) and Df = q(diff) (R11G11B10F stores);
ClusteredLightingWithSSAO reads Sp at its own pixel as tAmbient and the blender (ONE, ONE, ADD) writes Df = q(q(diff) + light). The fused
shader computes Sky's outputs itself and returns q(diff) + light (the host's Df was cleared to 0 by the Sky pass, so the blender gives
q(0 + that)). q = round to nearest even into the unsigned 11 / 10-bit float (lightfuse_sky.glsl, q11 below). Proof: on random G-buffers /
lights / IBL (the generators of tools/dos2_light_mobile.py, Sky reading the same G-buffer), at render scale 1 (the host's value):
  fused output == q(diff) + light of the chain, bit for bit (so q(fused) == q(chain) for any rounding of the final store), and
  q11(fused) == q11(chain) as the GPU stores it under round-to-nearest-even. A GPU that rounds the stores of Sp / Df differently (Vulkan
  leaves it to the implementation) differs by at most one step of the format there (6 / 5 mantissa bits): the documented bound.
  Negative control: the chain without the Sp store rounding must differ (the test sees the quantization).
  compile_fused(c1x, c1y, out) / descriptors(spv) / prove(...) are used by dos2_nr_sh_pack.py; standalone:
  dos2_lightfuse.py --bundles work/shaders-vk [--n 200]
"""
import argparse, struct, sys, tempfile
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
SRC = HERE / 'dos2_post'
SKY_KEY, LIGHT_KEY = '640a4c497bcc748b', 'd236b8d5b7ffad3c'
# fused binding -> (DeferredSky's binding it is fed from, kind); the host's spare slots: textures 120 + i, uniform slots 5 / 3 (Sky's own)
EXTRAS = {200: (10, 'tex'), 206: (16, 'tex'), 212: (22, 'tex'), 218: (28, 'tex'), 224: (34, 'tex'), 230: (40, 'tex'), 236: (46, 'tex'),
          242: (52, 'tex'), 248: (64, 'ubo'), 254: (70, 'ubo')}
F32 = np.float32


def f2u(x): return struct.unpack('<I', struct.pack('<f', float(x)))[0]
def u2f(u): return F32(struct.unpack('<f', struct.pack('<I', u & 0xffffffff))[0])


def q1(x, drop, dscale, maxv):
    """lightfuse_sky.glsl lf_q: RNE store into an unsigned float with (23 - drop) mantissa bits, exponent bias 15"""
    x = F32(x)
    if np.isnan(x): return x
    if not x > 0: return F32(0.0)
    if np.isinf(x): return x
    if x < F32(6.10351562e-05): return F32(np.round(F32(x * F32(dscale))) / F32(dscale))
    u = f2u(x); r = u2f(((u + ((1 << (drop - 1)) - 1) + ((u >> drop) & 1)) & ~((1 << drop) - 1)) & 0xffffffff)
    return u2f(0x7f800000) if r > F32(maxv) else r


def q11(v): return [q1(v[0], 17, 1048576.0, 65024.0), q1(v[1], 17, 1048576.0, 65024.0), q1(v[2], 18, 524288.0, 64512.0)]


def compile_frag(out, c1x=None, c1y=None, fused=True, fog=False):
    from dos2_pcf_mobile import run
    from dos2_native_shaders import tool
    cmd = [tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', '-I', str(SRC), '-o', str(out)]
    if fused:
        cmd.append('-DLIGHTFUSE')
        if fog: cmd.append('-DLFFOG')
        if c1x is not None: cmd += ['-DSKY_C1X=0x%08xu' % c1x, '-DSKY_C1Y=0x%08xu' % c1y]
    run(cmd + [str(SRC / 'ClusteredLightingWithSSAO_mobile.frag')])
    run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(out)])
    return Path(out).read_bytes()


def sky_c1(raw):
    """bits of c1[0].x / .y of the player's DeferredSky bundle (XGS1: head, binding table, inputs, then c1)"""
    h = struct.unpack_from('<8I', raw, 0); o = 32 + h[3] * 32 + h[4] * 4
    if h[5] < 8: raise SystemExit('DeferredSky bundle: no c1 data')
    return struct.unpack_from('<2I', raw, o)


def descriptors(spv):
    """{binding: set} of the module's descriptor variables"""
    w = struct.unpack('<%dI' % (len(spv) // 4), spv); i = 5; ds, bd = {}, {}
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if op == 71 and w[i + 2] == 34: ds[w[i + 1]] = w[i + 3]
        if op == 71 and w[i + 2] == 33: bd[w[i + 1]] = w[i + 3]
        i += n
    return {bd[k]: ds.get(k, 0) for k in bd}


def check_descriptors(spv, light_table):
    """the fused module's bindings: every one in the translated Light table or one of EXTRAS (else SystemExit)"""
    allowed = set(light_table) | set(EXTRAS)
    got = descriptors(spv)
    bad = sorted(b for b in got if b not in allowed or got[b] != 0)
    if bad: raise SystemExit('fused: bindings %s are neither the Light table %s nor the extras' % (bad, sorted(light_table)))
    miss = sorted(set(EXTRAS) - set(got))
    if miss: raise SystemExit('fused: extras %s not declared (layout drift)' % miss)
    return sorted(got)


def same(a, b): return all(f2u(x) == f2u(y) or (np.isnan(x) and np.isnan(y)) for x, y in zip(a, b))


def prove(sky_nat, light_nat, fused, n, seed, c1x, c1y, fog_nat=None):
    """(report lines, failures). fog_nat: the native DeferredFog; then fused is the g variant and its out 1 must equal DeferredFog of the
    stored Df (R11G11B10F, RNE) bit for bit"""
    import dos2_light_mobile as L
    import spv_interp as SI
    from dos2_post_mobile import cbuf
    S, Lm, Fm = SI.Module(sky_nat), SI.Module(light_nat), SI.Module(fused)
    Gm = SI.Module(fog_nat) if fog_nat else None
    eq_fog = ctl_fog = 0
    rng = np.random.default_rng(seed)
    k1 = (float(u2f(c1x)), float(u2f(c1y)), 0.0, 0.0)
    eq_pre = eq_q = ctl = lit = 0; bad = []
    ex = {'sky': [], 'light': [], 'fused': []}
    for it in range(n):
        scen = None if it % 4 else L.SCENARIOS[(it // 4) % len(L.SCENARIOS)][1]
        inp, ub, tx = L.case('ClusteredLightingWithSSAO', rng, False, scen)
        if Gm:   # fog parameters in c16[4..6] (the Light generator leaves them 0): start / end distances, heights, colours
            c16 = bytearray(ub[82]); z0 = float(rng.uniform(0, 40)); w0 = float(rng.uniform(0, 30))
            for r, (z, w) in enumerate(((z0, w0), (z0 + float(rng.uniform(1, 80)), w0 + float(rng.uniform(1, 60))), (float(rng.uniform(-5, 20)), float(rng.choice([-1, 1]) * rng.uniform(0.5, 30))))):
                struct.pack_into('<4f', c16, 16 * (4 + r), float(rng.uniform(0, 2)), float(rng.uniform(0, 2)), z, w)
            ub = dict(ub); ub[82] = bytes(c16)
        cube = lambda: SI.CubeTexture(rng.uniform(0, 3, (6, 4, 4, 4)).astype(np.float32))
        rot = L.rotation(rng)
        c8 = cbuf({i: tuple(rot[i]) + (0.0,) for i in range(3)})
        c6 = cbuf({2: (0.0, float(rng.uniform(0, 8)), float(rng.uniform(0, 2)), 0.0)})
        stx = {10: tx[10], 16: tx[22], 22: tx[16], 28: cube(), 34: tx[34], 40: tx[46], 46: SI.Texture(L.table(rng, shape=(8, 8))), 52: cube()}
        sub = {4: ub[4], 58: cbuf({0: k1}), 64: c8, 70: c6}
        so = S.run(inp, sub, stx); ex['sky'].append(S.executed)
        spec, diff = so[0][:3], so[1][:3]
        t2 = dict(tx); t2[52] = L.BigTex(np.array([[list(q11(spec)) + [0.0]]], np.float32), L.FW, L.FH)
        lo = Lm.run(inp, ub, t2)[0]; ex['light'].append(Lm.executed)
        want_pre = [F32(a + b) for a, b in zip(q11(diff), lo[:3])]
        chain = q11(want_pre)
        ftx = dict(tx); fub = dict(ub)
        for b, (sb, kind) in EXTRAS.items():
            if kind == 'tex': ftx[b] = stx[sb]
            else: fub[b] = sub[sb]
        fr = Fm.run(inp, fub, ftx); fo = fr[0]; ex['fused'].append(Fm.executed)
        if Gm:
            stored = chain + [F32(1.0)]   # the Df texel Fog reads: R11G11B10F (alpha 1)
            gtx = {10: tx[22], 16: L.BigTex(np.array([[stored]], np.float32), L.FW, L.FH)}
            go = Gm.run(inp, {4: ub[4], 22: ub[82], 28: ub[100]}, gtx)[0]
            if 1 in fr and same(fr[1], go): eq_fog += 1
            g2 = Gm.run(inp, {4: ub[4], 22: ub[82], 28: ub[100]}, {10: tx[22], 16: L.BigTex(np.array([[list(want_pre) + [1.0]]], np.float32), L.FW, L.FH)})[0]
            if not same(g2, go): ctl_fog += 1   # control: Fog over the unrounded Df
            if not (1 in fr and same(fr[1], go)) and len(bad) < 4: bad.append((it, list(go), list(fr.get(1, []))))
        if same(fo[:3], want_pre) and same([fo[3]], [lo[3]]): eq_pre += 1
        else: bad.append((it, want_pre, fo))
        if same(q11(fo[:3]), chain): eq_q += 1
        t3 = dict(tx); t3[52] = L.BigTex(np.array([[list(spec) + [0.0]]], np.float32), L.FW, L.FH)   # control: Sp not rounded
        lo3 = Lm.run(inp, ub, t3)[0]
        if not same([F32(a + b) for a, b in zip(q11(diff), lo3[:3])], fo[:3]): ctl += 1
        if any(float(x) != 0.0 for x in spec): lit += 1
    lines = ['NR-LIGHTFUSE fused == Sky -> q11 -> Light -> blend: %d random invocations (%d lit by Sky), output before the store bit for bit %d / %d, '
             'stored (RNE) %d / %d; control (chain without the Sp store rounding) differs in %d of the %d lit; executed per pixel: Sky %.0f + '
             'Light %.0f, fused %.0f' % (n, lit, eq_pre, n, eq_q, n, ctl, lit, np.mean(ex['sky']), np.mean(ex['light']), np.mean(ex['fused']))]
    for it, w, f in bad[:4]: lines.append('  MISMATCH #%d chain %s fused %s' % (it, [float(x) for x in w], [float(x) for x in f]))
    if Gm:
        lines.append('NR-LIGHTFUSE fog (variant g): out 1 == DeferredFog of the stored Df, bit for bit %d / %d; control (Fog over the unrounded Df) differs in %d' % (eq_fog, n, ctl_fog))
    fails = (n - eq_pre) + (n - eq_q) + (0 if ctl > 0 or lit == 0 else 1) + ((n - eq_fog + (0 if ctl_fog else 1)) if Gm else 0)
    return lines, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', required=True)
    ap.add_argument('--n', type=int, default=200)
    a = ap.parse_args()
    raw = (Path(a.bundles) / (SKY_KEY + '.xgs')).read_bytes()
    c1x, c1y = sky_c1(raw)
    from dos2_native_shaders import read_bundle
    light_tab = [b[0] for b in read_bundle(Path(a.bundles) / (LIGHT_KEY + '.xgs'))['bindings']]
    from dos2_pcf_mobile import run
    from dos2_native_shaders import tool
    with tempfile.TemporaryDirectory() as tmp:
        fused = compile_frag(Path(tmp) / 'f.spv', c1x, c1y)
        print('fused: %d bytes, bindings %s (Sky c1 %08x %08x)' % (len(fused), check_descriptors(fused, light_tab), c1x, c1y))
        light = compile_frag(Path(tmp) / 'l.spv', fused=False)
        run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', '-I', str(SRC), '-o', str(Path(tmp) / 's.spv'), str(SRC / 'DeferredSky_mobile.frag')])
        lines, fails = prove((Path(tmp) / 's.spv').read_bytes(), light, fused, a.n, 1119, c1x, c1y)
        fogged = compile_frag(Path(tmp) / 'g.spv', c1x, c1y, fog=True)
        print('fog variant: %d bytes, bindings %s' % (len(fogged), check_descriptors(fogged, light_tab)))
        run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', '-I', str(SRC), '-o', str(Path(tmp) / 'd.spv'), str(SRC / 'DeferredFog_mobile.frag')])
        l2, f2 = prove((Path(tmp) / 's.spv').read_bytes(), light, fogged, a.n, 1120, c1x, c1y, (Path(tmp) / 'd.spv').read_bytes())
        lines += l2; fails += f2
    print('\n'.join(lines))
    print('PASS' if not fails else 'FAILED (%d)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
