"""NR-10b: the position programs of xbg_spirv_pos.cpp against an independent run of the same vertex modules (tools/spv_interp.py).

  nr_cover_test --census <work/shaders-vk> programs.txt     (C++: every vertex bundle through the analysis; one accepted program a line)
  python nr_cover_census.py <work/shaders-vk> programs.txt [shader_map.tsv | -] [spv dir]
  (N5: with spv dir, the module of each key is <spv dir>/<key>.spv - the final words nr_cover_chain_test wrote after the load-time
  clean-ups and DM_NR_UBOVEC - instead of the bundle's own SPIR-V)

For each accepted shader: 24 random draws of its inputs and of the bytes of its uniform blocks; the program (evaluated here, float32,
the same operations as nr_cv_eval) must give the position the interpreter computes for the whole shader, bit for bit when the program is
"exact", within 1e-5 relative otherwise (the shader's Fma is fused in the interpreter and a * b + c in the program). Exit 1 on a mismatch.
"""
import struct, sys, random
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[5] / 'tools'))
from spv_interp import Module, F

OPS = ('const', 'input', 'ubo', 'add', 'sub', 'mul', 'div', 'neg', 'fma', 'abs', 'min', 'max')


def spirv_of(path):
    b = Path(path).read_bytes()
    magic, ver, stage, nb, ni, c1, sp, _ = struct.unpack_from('<8I', b, 0)
    assert magic == 0x31534758
    o = 32 + nb * 32 + ni * 4 + c1
    return b[o:o + sp]


def run_prog(ins, out, inputs, ubos):
    v = []
    with np.errstate(all='ignore'):
        for op, a, b, c, k in ins:
            if op == 'const': v.append(F(struct.unpack('<f', struct.pack('<I', k))[0]))
            elif op == 'input': v.append(F(inputs[a][b]))
            elif op == 'ubo': v.append(F(struct.unpack_from('<f', ubos[a], b)[0]))
            elif op == 'add': v.append(F(v[a] + v[b]))
            elif op == 'sub': v.append(F(v[a] - v[b]))
            elif op == 'mul': v.append(F(v[a] * v[b]))
            elif op == 'div': v.append(F(v[a] / v[b]))
            elif op == 'neg': v.append(F(-v[a]))
            elif op == 'fma': v.append(F(F(v[a] * v[b]) + v[c]))
            elif op == 'abs': v.append(F(abs(v[a])))
            elif op == 'min': v.append(v[a] if v[a] < v[b] else v[b])
            elif op == 'max': v.append(v[a] if v[a] > v[b] else v[b])
    return [v[i] for i in out]


def position(m):
    """the interpreter's gl_Position after run(): the Output variable (or block member) that is BuiltIn Position"""
    for v, (pt, sc) in m.vars.items():
        if sc != 3: continue
        if m.dec.get(v, {}).get(11, [None])[0] == 0: return m.mem[v]
        t = m.types[pt][1][1]
        if m.types[t][0] == 30:
            for k in range(len(m.types[t][1])):
                if m.mdec.get((t, k), {}).get(11, [None])[0] == 0: return m.mem[v][k]
    raise ValueError('no position')


def main():
    bundles, progs = Path(sys.argv[1]), Path(sys.argv[2])
    names = {}; spv = Path(sys.argv[4]) if len(sys.argv) > 4 else None
    if len(sys.argv) > 3 and sys.argv[3] != '-':
        for l in open(sys.argv[3], encoding='utf-8'):
            p = l.rstrip('\n').split('\t')
            if len(p) >= 3: names[p[0]] = p[2]
    rnd = random.Random(1710); bad = 0; n = 0; exact = 0; worst = 0.0
    for line in progs.read_text().splitlines():
        p = line.split(); key = p[0]; kv = dict(x.split('=') for x in p[1:5])
        out = [int(x) for x in kv['out'].split(',')]
        ins = []
        for x in p[5:]:
            op, a, b, c, k = x.split(':'); ins.append((op, int(a), int(b), int(c), int(k, 16)))
        m = Module((spv / (key + '.spv')).read_bytes() if spv else spirv_of(bundles / (key + '.xgs'))); n += 1; exact += kv['exact'] == '1'
        for trial in range(24):
            inputs = {loc: [F(rnd.uniform(-2, 2)) for _ in range(4)] for loc in range(16)}
            ubos = {bnd: struct.pack('<64f', *[rnd.uniform(-2, 2) for _ in range(64)]) + bytes(65536 - 256) for bnd in range(16)}
            with np.errstate(all='ignore'):
                m.run(inputs=inputs, ubos=ubos); ref = [F(x) for x in position(m)]
            got = run_prog(ins, out, inputs, ubos)
            for c in range(4):
                if kv['exact'] == '1': ok = got[c].tobytes() == ref[c].tobytes()
                else:
                    err = abs(float(got[c]) - float(ref[c])) / max(1.0, abs(float(ref[c]))); worst = max(worst, err); ok = err <= 1e-5
                if not ok:
                    bad += 1
                    if bad <= 8: print('MISMATCH %s (%s) component %d: program %r, shader %r' % (key, names.get(key, '?'), c, got[c], ref[c]))
    print('nr_cover_census: %d programs (%d exact) x 24 random draws against spv_interp: %d mismatches; worst relative error of the computed ones %.2e'
          % (n, exact, bad, worst))
    return 1 if bad or not n else 0


if __name__ == '__main__':
    sys.exit(main())
