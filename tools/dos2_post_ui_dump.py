"""DOS2 NB-18a: dump the post tail / UI combine shaders of EngineShaders.pak for reading (no build, no phone).

For each shader name: GLSLC reflection of the Switch .bshd (uniform blocks, uniforms with offsets, samplers with bindings, in / out)
and the translated module work/shaders-vk/<key>.xgs (bundle bindings, spirv-opt -O + spirv-dis text, texture-op counts).
Output: <out>/<name>.<stage>.txt. Keys come from the .bshd itself (dos2_native_shaders.bshd_programs).
Usage: dos2_post_ui_dump.py [--pak work/romfs/Data/EngineShaders.pak] [--out <dir>] [names...]
"""
import argparse, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from dos2_native_shaders import bshd_programs, read_bundle, tool, WORK, Spv
sys.path.insert(0, str(HERE.parent.parent / 'tools'))

DEFAULT = ['CombineUI', 'CombineUI_NoFetch', 'CombineUI_BicubicFilter', 'CombineUI_HDR', 'PPApplyLUT', 'EoCSight', 'EoCSight_HDR',
           'PPFXAA', 'PPVignette', 'PPVignette_HDR', 'FadeBlending', 'EoCOutlineCombine', 'EoCOutlineCombine_HDR']
TEX_OPS = {87: 'Sample', 88: 'SampleExplLod', 89: 'SampleDref', 90: 'SampleDrefExplLod', 95: 'Fetch', 96: 'Gather', 97: 'DrefGather'}


def pak_file(pak, name):
    sys.path.insert(0, str(HERE.parent.parent / 'tools'))
    import lspk
    p = lspk.Pak(pak)
    for e in p.entries:
        if e[0] == name: return p.read(e)
    raise SystemExit('not in pak: ' + name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pak', default=str(WORK / 'romfs' / 'Data' / 'EngineShaders.pak'))
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--out', required=True)
    ap.add_argument('names', nargs='*', default=DEFAULT)
    a = ap.parse_args()
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name in a.names:
            raw = pak_file(a.pak, 'Shaders/NX/%s.bshd' % name)
            for stage, key, refl in bshd_programs(raw):
                lines = ['# %s %s key %s' % (name, stage, key)]
                for sec in ('blocks', 'unif', 'in', 'out'):
                    for n, w in (refl or {}).get(sec, []):
                        lines.append('%-6s %-40s %s' % (sec, n, ' '.join(str(x) for x in w[:14])))
                b = Path(a.bundles) / (key + '.xgs')
                if b.exists():
                    tr = read_bundle(b)
                    lines.append('# bundle bindings: ' + ' | '.join(' '.join(str(x) for x in bb) for bb in tr['bindings']))
                    s, o = Path(tmp) / 'a.spv', Path(tmp) / 'b.spv'
                    s.write_bytes(tr['spirv'])
                    subprocess.run([tool('spirv-opt'), '-O', str(s), '-o', str(o)], check=True)
                    w = Spv(o.read_bytes()).w; ops, i = {}, 5
                    while i < len(w):
                        n, op = w[i] >> 16, w[i] & 0xffff
                        if op in TEX_OPS: ops[TEX_OPS[op]] = ops.get(TEX_OPS[op], 0) + 1
                        i += n
                    lines.append('# texture ops (opt): ' + str(ops))
                    lines.append(subprocess.run([tool('spirv-dis'), '--no-header', str(o)], capture_output=True, text=True).stdout)
                else:
                    lines.append('# no translated bundle ' + str(b))
                (out / ('%s.%s.txt' % (name, stage))).write_text('\n'.join(lines) + '\n')
                print(name, stage, key, 'bundle' if b.exists() else 'NO BUNDLE')


if __name__ == '__main__':
    main()
