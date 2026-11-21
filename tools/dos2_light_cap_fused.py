"""Divinity: Original Sin 2 DE, VK-NATIVE stage 5.4 G1, DM_VK_NATIVE_LCAP=2 (dm/gfx/vkn/vkn_hacks.h): the light cap of N6 DM_HACK_LIGHTS
inside the fused DeferredSky -> ClusteredLightingWithSSAO module of NR-LIGHTFUSE, so that the cap and LIGHTFUSE run together in mode 1.

tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag + the LCAP text patch of tools/dos2_light_cap.py (capped_source), compiled with
-DLIGHTFUSE [-DLFFOG] -DLCAP=2 / 4 and the player's DeferredSky c1 bits (SKY_C1X / SKY_C1Y, read from its bundle as the pack tool does)
-> dm/android/shaders/light_lcap<N>_<f|g>.frag.spv.inc. The backend (vkn_shlib_named.cpp) puts them behind the player's translated Light
head only when the player's Sky c1 equals the bits named in the .inc's first line (else the DM_VK_NATIVE_LCAP=1 rule: no fusion).
Proofs on tools/spv_interp.py (tools/dos2_lightfuse.py prove): the capped fused module == Sky -> q11 store -> the CAPPED Light
(light_lcap<N>: = the 'x' Light on a prefix of the cluster's list, tools/dos2_light_cap.py) -> blend, bit for bit, before and after the
R11G11B10F store; the 'g' form also: output 1 == DeferredFog of the stored Df. Control: how often the cap changed the Light's output on
the same generator (a proof whose cases never reach N lights would not see the cap): the lightfuse generator with the cluster list of
dos2_light_cap.py (0..12 points, 0..4 spots), so both proofs run on lists the cap cuts.
[gpu2] Also the per-light threshold cap (DM_VK_NATIVE_LCAP_SEL=1, the default; tools/dos2_light_cap.py thr_source) the same way:
light_lcap<N>t_<f|g>.frag.spv.inc, the same proof against its own capped chain; its control runs with sun and ambient on.
Usage: dos2_light_cap_fused.py [--bundles <dos2-port>/work/shaders-vk] [--n 200]
"""
import argparse, sys, tempfile
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from dos2_native_shaders import read_bundle, tool, WORK
from dos2_pcf_mobile import run
import dos2_light_cap as LC
import dos2_lightfuse as LF
import dos2_light_mobile as LM
import spv_interp as SI

OUT = HERE.parent / 'dm' / 'android' / 'shaders'


def compile_capped(tmp, name, defines, text=None):
    src = Path(tmp) / 'ClusteredLightingWithSSAO_lcap.frag'
    src.write_text(LC.capped_source() if text is None else text, encoding='utf-8')
    spv = Path(tmp) / (name + '.spv')
    run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', *defines, '-I', str(LF.SRC), '-o', str(spv), str(src)])
    run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(spv)])
    return spv.read_bytes()


def write_inc(data, path, cap, fog, c1x, c1y, what='the LCAP text patch of tools/dos2_light_cap.py', extra=''):
    import struct
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    lines = ['// [DOS2] stage 5.4 DM_VK_NATIVE_LCAP=2: SKY_C1 0x%08x 0x%08x - SPIR-V of tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag + %s, '
             '-DLIGHTFUSE%s -DLCAP=%d%s (glslc --target-env=vulkan1.1 -O), made by tools/dos2_light_cap_fused.py'
             % (c1x, c1y, what, ' -DLFFOG' if fog else '', cap, extra)]
    for i in range(0, len(w), 8): lines.append(', '.join('0x%08x' % x for x in w[i:i + 8]) + ',')
    path.write_text('\n'.join(lines) + '\n')


_case = LM.case
def case_many(name, rng, flag, scen):
    """the lightfuse generator's case with the cluster list of tools/dos2_light_cap.py: every pixel reads one list of 0..12 points and
    0..4 spots (random lights of the case's table), so the cap has lights to cut (the plain generator rarely passes 4)"""
    np_, ns_ = int(rng.integers(0, 13)), int(rng.integers(0, 5))
    if scen is None: scen = (np_, ns_, int(rng.choice([0, 0, 2])))   # (dos2_light_cap.py prove: lights placed to light the pixel)
    inp, ub, tx = _case(name, rng, flag, scen)
    c12, c11 = LC.shared_list(rng, np_, ns_); ub = dict(ub); ub[64], ub[70] = c12, bytes(c11)
    return inp, ub, tx


def cuts(light_x, light_cap, n, seed, ref=False):
    """cases of the lightfuse generator in which the capped Light differs from the full one (ref: sun and ambient on - the threshold
    cap measures each light against them, so it drops nothing in the scenarios that zero them)"""
    rng = np.random.default_rng(seed); mx, mc = SI.Module(light_x), SI.Module(light_cap); k = 0
    for it in range(n):
        scen = None if it % 4 else LM.SCENARIOS[(it // 4) % len(LM.SCENARIOS)][1]
        inp, ub, tx = LM.case('ClusteredLightingWithSSAO', rng, False, scen)
        if ref: ub, tx = LC.with_ref(rng, ub, tx)
        if not LM.same(mx.run(inp, ub, tx), mc.run(inp, ub, tx)): k += 1
    return k


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--n', type=int, default=200)
    a = ap.parse_args()
    raw = (Path(a.bundles) / (LF.SKY_KEY + '.xgs')).read_bytes()
    c1x, c1y = LF.sky_c1(raw)
    tr = read_bundle(Path(a.bundles) / (LF.LIGHT_KEY + '.xgs'))
    light_tab = [b[0] for b in tr['bindings']]
    fails = 0; out = []
    with tempfile.TemporaryDirectory() as tmp:
        run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', '-I', str(LF.SRC), '-o', str(Path(tmp) / 's.spv'), str(LF.SRC / 'DeferredSky_mobile.frag')])
        run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', '-I', str(LF.SRC), '-o', str(Path(tmp) / 'd.spv'), str(LF.SRC / 'DeferredFog_mobile.frag')])
        sky, fog = (Path(tmp) / 's.spv').read_bytes(), (Path(tmp) / 'd.spv').read_bytes()
        LM.case = case_many   # (prove() and cuts() draw their cases from it)
        light_x = LF.compile_frag(Path(tmp) / 'x.spv', fused=False)
        c1 = ['-DSKY_C1X=0x%08xu' % c1x, '-DSKY_C1Y=0x%08xu' % c1y]
        # [gpu2] two selections: the first N (DM_VK_NATIVE_LCAP_SEL=0: light_lcap<N>_<f|g>) and the per-light threshold (SEL=1, the
        # default: light_lcap<N>t_<f|g>, tools/dos2_light_cap.py thr_source)
        for sel in ('first', 'thr'):
            for cap in (2, 4):
                if sel == 'first': text, dl, tag, what, ex = None, ['-DLCAP=%d' % cap], '', 'the LCAP text patch of tools/dos2_light_cap.py', ''
                else:
                    text, tau = LC.thr_source(cap); dl = LC.thr_defines(cap); tag = 't'
                    what, ex = 'the threshold text patch of tools/dos2_light_cap.py (thr_source)', ' -DLTHR=%r' % tau
                light_cap = compile_capped(tmp, 'l%s%d' % (tag, cap), dl, text)
                k = cuts(light_x, light_cap, a.n, 0x5400 + cap, ref=sel == 'thr')
                out.append('LCAP=%d %s: the cap changed the Light output in %d of %d generator cases (control)' % (cap, sel, k, a.n))
                if not k: fails += 1
                for fogv in (False, True):
                    fused = compile_capped(tmp, 'f%s%d%d' % (tag, cap, fogv), ['-DLIGHTFUSE'] + (['-DLFFOG'] if fogv else []) + dl + c1, text)
                    binds = LF.check_descriptors(fused, light_tab)
                    lines, f = LF.prove(sky, light_cap, fused, a.n, 0x5410 + cap * 2 + int(fogv), c1x, c1y, fog if fogv else None)
                    fails += f
                    name = 'light_lcap%d%s_%s.frag.spv.inc' % (cap, tag, 'g' if fogv else 'f')
                    write_inc(fused, OUT / name, cap, fogv, c1x, c1y, what, ex)
                    out.append('built %s: %d words, bindings %s (Sky c1 %08x %08x); against the capped chain:' % (name, len(fused) // 4, binds, c1x, c1y))
                    out += ['  ' + l for l in lines]
    print('\n'.join(out))
    print('PASS' if not fails else 'FAILED (%d)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
