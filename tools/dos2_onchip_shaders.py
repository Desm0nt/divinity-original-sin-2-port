"""DOS2 NR-ONCHIP: input-attachment variants of the native deferred lighting stages, proven equal to the translated modules.

A variant is tools/dos2_post/<stage>_mobile.frag compiled with -DOC=<mask>: texture n with bit n set becomes a subpassInput
(input_attachment_index n) read by subpassLoad, the render-scale slow path is compiled out (a variant runs only at render scale 1, what
the host writes). With OC undefined the same file compiles to the NR-04 bundle byte for byte (checked here too).
Variants (docs/dos2/nr/NR-ONCHIP.md section 3: which chain needs which):
    light.s2 0x40  ClusteredLightingWithSSAO, tAmbient (DeferredSky out 0) from the merged pass        (S2: Sky -> Light)
    light.s3 0xC0  + tShadow (CascadedPCF out)                                                          (S3: PCF -> Sky -> Light)
    light.g  0xFF  + the five G-buffer targets and the linear depth                                     (G: whole chain)
    sky.g    0x0F  DeferredSky, the G-buffer targets and the linear depth                               (G)
    light.s2f 0x48 ClusteredLightingWithSSAO, tAmbient + tEmissive (G-buffer c0, which the fog subpass   (S2F: Sky -> Light -> Fog,
                   then writes: c0 must be an input attachment of the merged pass, not a texture)       NR-FRAME.md §8)
    fog.s2f  0x02  DeferredFog, tScene (= Df the light subpass wrote) from the merged pass              (S2F)
Proof (tools/spv_interp.py, which models OpImageRead of a subpass input the way Vulkan defines it: the attachment's texel at the
fragment's own pixel): on the same random invocations as tools/dos2_light_mobile.py (render scale 1; every shading model, sky, lights,
clusters, shadow on / off) the variant's outputs equal the translated module's (spirv-opt -O) and the NR-04 native module's bit for bit;
fixed light scenarios too. Sensitivity: the same run with the modelled input read moved one pixel must mismatch (mutation).
Usage: dos2_onchip_shaders.py [--bundles <player's translated bundles dir>] [--n 300] [--out <dir for the .spv>]
"""
import argparse, struct, sys, tempfile
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'dm' / 'gfx' / 'tools'))
from dos2_native_shaders import Spv, read_bundle, tool, WORK
from dos2_pcf_mobile import run
import dos2_light_mobile as LM
import spv_interp as SI

SRC = HERE / 'dos2_post'
VARIANTS = [('light.s2', 'd236b8d5b7ffad3c', 'ClusteredLightingWithSSAO', 0x40),
            ('light.s3', 'd236b8d5b7ffad3c', 'ClusteredLightingWithSSAO', 0xC0),
            ('light.g', 'd236b8d5b7ffad3c', 'ClusteredLightingWithSSAO', 0xFF),
            ('sky.g', '640a4c497bcc748b', 'DeferredSky', 0x0F),
            ('light.s2f', 'd236b8d5b7ffad3c', 'ClusteredLightingWithSSAO', 0x48),
            ('fog.s2f', 'd8aeec640d465275', 'DeferredFog', 0x02)]
SRCS = {'ClusteredLightingWithSSAO': 'ClusteredLightingWithSSAO_mobile.frag', 'DeferredSky': 'DeferredSky_mobile.frag', 'DeferredFog': 'DeferredFog_mobile.frag'}
# input_attachment_index -> binding, as declared in the .frag files
INPUTS = {'ClusteredLightingWithSSAO': [10, 16, 22, 28, 34, 40, 52, 58], 'DeferredSky': [10, 16, 22, 34], 'DeferredFog': [10, 16]}
import dos2_post_mobile as PM   # the post shaders' random invocations (DeferredFog)
def gen(stage):
    """(case(rng), same) of a stage's random invocations at render scale 1"""
    if stage == 'DeferredFog': return (lambda rng, scen=None: PM.case(stage, rng, False)), PM.same
    return (lambda rng, scen=None: LM.case(stage, rng, False, scen) if scen is not None else LM.case(stage, rng, False)), LM.same


def compile_frag(src, out, oc=None):
    cmd = [tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', '-I', str(SRC)]
    if oc is not None: cmd.append('-DOC=%#x' % oc)
    run(cmd + ['-o', str(out), str(SRC / src)]); run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(out)])
    return Path(out).read_bytes()


def subpass_bindings(spv):
    """bindings of the module's subpass-input variables (OpTypeImage Dim SubpassData) -> input_attachment_index"""
    w = struct.unpack('<%dI' % (len(spv) // 4), spv); i = 5
    img, ptr, var, dec = set(), {}, {}, {}
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff; a = w[i + 1:i + n]
        if op == 25 and a[2] == 6: img.add(a[0])                               # OpTypeImage, Dim SubpassData
        elif op == 32: ptr[a[0]] = a[2]                                          # OpTypePointer
        elif op == 59: var[a[1]] = a[0]                                          # OpVariable
        elif op == 71: dec.setdefault(a[0], {})[a[1]] = a[2] if len(a) > 2 else None   # OpDecorate
        i += n
    return {dec[v].get(33): dec[v].get(43) for v, t in var.items() if ptr.get(t) in img and v in dec}


def check(name, key, stage, oc, mods, n, seed, scen_n):
    """random + scenario invocations: variant == translated == native; returns (lines, failures)"""
    rng = np.random.default_rng(seed); bad = 0; reads = 0; lines = []; case, same = gen(stage)
    for it in range(n):
        inp, ubo, tx = case(rng)
        r = {k: m.run(inp, ubo, tx) for k, m in mods.items()}
        reads += mods['variant'].subpass_reads
        if not (same(r['translated'], r['variant']) and same(r['native'], r['variant'])):
            bad += 1
            if bad <= 3: lines.append('  MISMATCH %s in=%s translated=%s variant=%s' % (name, inp, r['translated'], r['variant']))
    lines.insert(0, '%s (%s, OC=%#04x): %d random invocations at render scale 1, variant == translated == native bit for bit: %d / %d; '
                 'input attachment reads %.2f / invocation' % (name, stage, oc, n, n - bad, n, reads / max(n, 1)))
    if stage == 'ClusteredLightingWithSSAO':
        sb = 0
        for label, scen in LM.SCENARIOS:
            rng = np.random.default_rng(seed + 1)
            for _ in range(scen_n):
                inp, ubo, tx = LM.case(stage, rng, False, scen)
                r = {k: m.run(inp, ubo, tx) for k, m in mods.items()}
                if not (LM.same(r['translated'], r['variant']) and LM.same(r['native'], r['variant'])): sb += 1
        lines.append('  fixed light scenarios (%d x %d): mismatches %d' % (len(LM.SCENARIOS), scen_n, sb)); bad += sb
    return lines, bad


def mutation(stage, var_spv, tr_opt, n, seed):
    """the modelled input read one pixel to the right: the comparison must see it"""
    m = SI.Module(var_spv); orig = m._subpass_read
    def shifted(a, g):
        fc = m.fragcoord; m.fragcoord = (fc[0] + 1.0,) + tuple(fc[1:])
        try: return orig(a, g)
        finally: m.fragcoord = fc
    m._subpass_read = shifted
    t = SI.Module(tr_opt); rng = np.random.default_rng(seed); caught = 0; case, same = gen(stage)
    for _ in range(n):
        inp, ubo, tx = case(rng)
        if not same(t.run(inp, ubo, tx), m.run(inp, ubo, tx)): caught += 1
    return caught


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--n', type=int, default=300); ap.add_argument('--scen', type=int, default=20)
    ap.add_argument('--out', default='')
    a = ap.parse_args()
    fails = 0; out = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp); base = {}
        for stage, src in SRCS.items():
            key = [v[1] for v in VARIANTS if v[2] == stage][0]
            tr = read_bundle(Path(a.bundles) / (key + '.xgs'))
            (tmp / 'tr.spv').write_bytes(tr['spirv']); run([tool('spirv-opt'), '-O', str(tmp / 'tr.spv'), '-o', str(tmp / (stage + '.tro.spv'))])
            nat = compile_frag(src, tmp / (stage + '.nat.spv'))
            base[stage] = ((tmp / (stage + '.tro.spv')).read_bytes(), nat, tr['spirv'])
            out.append('%s: OC undefined compiles to %d bytes (the NR-04 bundle body)' % (stage, len(nat)))
        for name, key, stage, oc in VARIANTS:
            tr_opt, nat, tr_raw = base[stage]
            spv = compile_frag(SRCS[stage], tmp / (name + '.spv'), oc)
            if a.out: Path(a.out).mkdir(parents=True, exist_ok=True); (Path(a.out) / ('%s.%s.spv' % (name, key))).write_bytes(spv)
            sp = subpass_bindings(spv)
            want = {b: i for i, b in enumerate(INPUTS[stage]) if oc >> i & 1}
            if sp != want: out.append('  BINDINGS %s: subpass inputs %s, expected %s' % (name, sp, want)); fails += 1
            n_io, t_io = Spv(spv), Spv(tr_raw)
            if n_io.io(1) != t_io.io(1) or n_io.io(3) != t_io.io(3): out.append('  INTERFACE %s differs' % name); fails += 1
            mods = {'translated': SI.Module(tr_opt), 'native': SI.Module(nat), 'variant': SI.Module(spv)}
            lines, bad = check(name, key, stage, oc, mods, a.n, 11, a.scen)
            out += lines; fails += bad
            out.append('  subpass inputs (binding: input_attachment_index) %s; %d bytes' % (sp, len(spv)))
            caught = mutation(stage, spv, tr_opt, 60, 5)
            out.append('  mutation (input read one pixel off): caught in %d / 60 invocations' % caught)
            if caught == 0: fails += 1
    print('\n'.join(out)); print('FAIL %d' % fails if fails else 'PASS')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
