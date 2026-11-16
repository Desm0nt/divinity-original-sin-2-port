"""Divinity: Original Sin 2 DE: mobile "Light Shadows" resolve (CascadedPCF_<N>x<S> fragment stage) as native bundles (DM_GFX_NATIVE_SHADERS=1).

tools/dos2_pcf/CascadedPCF_mobile.frag is compiled with glslc once per (key, mode) and wrapped like tools/dos2_native_shaders.py does:
the translated bundle's header, binding table, input list and c1 bytes unchanged, only the SPIR-V replaced, so the host's head check
(xbg_game.cpp namespace native: head equal byte for byte) accepts it. The GLSL declares the translated bundle's own descriptor bindings
(4 / 22 / 28 / 34 / 10 / 16), so nothing is renumbered; the entry-point interface is checked against the translated SPIR-V.
Modes (subfolders of --out):  exact  = 9 point taps + manual compare (Switch result);  mobile = 4 textureGather taps (2x2 PCF each);
ipad16 = 16 textureGather taps on the iPad Poisson disc. Each folder: native/<key>.xgs, native.xgspack (only these keys) and, when
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
MODES = {'exact': 0, 'mobile': 1, 'ipad16': 2}
# per pixel: (taps in one cascade, texels compared per tap); a pixel in a fade band samples two cascades
TAPS = {'translated': (9, 1), 'exact': (9, 1), 'mobile': (4, 4), 'ipad16': (16, 4)}
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
