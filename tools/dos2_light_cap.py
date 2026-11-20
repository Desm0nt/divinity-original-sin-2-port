"""Divinity: Original Sin 2 DE, N6 DM_HACK_LIGHTS (docs/dos2/nr/NR-HACKS.md): build and prove the light-cap variants of the lighting pass.

tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag (the NR-04 'x' rewrite, proven bit for bit against the translated module by
tools/dos2_light_mobile.py) compiled with -DLCAP=2 / 4 -> dm/android/shaders/light_lcap<N>.frag.spv.inc (the backend wraps them with the
player's translated head, xbg_nr_sh.inc hack_apply). Proofs with tools/spv_interp.py on tools/dos2_light_mobile.py's cases:
  1. LCAP=99 (never reached) == the 'x' module bit for bit (the changed loops alone change nothing);
  2. LCAP=N == the 'x' module run on a prefix of the cluster's list (points, then spots; the list rewritten), bit for bit - the prefix that
     ends at the N-th light that lights the pixel: the variant is exactly "the first N contributing lights";
  3. cost: executed instructions per lit pixel for 0..8 points / 0..4 spots (the 'x' module), the per-light increment.
Usage: dos2_light_cap.py [--bundles work/shaders-vk] [--n 200]
"""
import argparse, struct, tempfile, sys
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from dos2_native_shaders import read_bundle, tool, WORK
from dos2_pcf_mobile import run, check_iface
import spv_interp as SI
import dos2_light_mobile as LM

KEY = 'd236b8d5b7ffad3c'
SRC = HERE / 'dos2_post' / 'ClusteredLightingWithSSAO_mobile.frag'
OUT = HERE.parent / 'dm' / 'android' / 'shaders'


CAP_AT = '    uint p = oc.x, np = oc.y & 65535u, ns = uint(int(oc.y) >> 16);\n'
CAP_ADD = ('#ifdef LCAP\n'
           '    // [DOS2] N6 DM_HACK_LIGHTS: the first LCAP lights that light this pixel (in range, facing it; points, then spots, in the list\'s\n'
           '    // order) - after the LCAP-th the loops stop; lights out of range cost only their range test, as before\n'
           '    const uint p0 = p, npo = np;\n'
           '    uint lit = 0u;\n'
           '#endif\n')
LOOP_P = '        for (uint i = 0u; i < np; i++, p++) {\n'
LOOP_S = '        for (uint i = 0u; i < ns; i++, p++) {\n'
SPOT_ADD = '#ifdef LCAP\n        p = p0 + npo;   // the spots start after all the cluster\'s points (the point loop may stop early)\n#endif\n'
HIT = '                    diff = fma(dc * col, vec3(kd), diff);\n'


def capped_source():
    """the 'x' rewrite with the cap inserted (text patch at build time: the *_mobile.frag files feed setup's NR-04 pack stamp, so the
    source itself stays untouched): a counter of contributing lights, the loops stop at LCAP, the spot loop restarts at its own list"""
    s = SRC.read_text(encoding='utf-8')
    assert s.count(CAP_AT) == 1 and s.count(LOOP_P) == 2 and s.count(LOOP_S) == 2 and s.count(HIT) == 4, \
        'ClusteredLightingWithSSAO_mobile.frag changed: update the LCAP anchors'
    s = s.replace(CAP_AT, CAP_AT + CAP_ADD, 1)
    s = s.replace(LOOP_P, '#ifdef LCAP\n        for (uint i = 0u; i < np && lit < uint(LCAP); i++, p++) {\n#else\n' + LOOP_P + '#endif\n')
    s = s.replace(LOOP_S, SPOT_ADD + '#ifdef LCAP\n        for (uint i = 0u; i < ns && lit < uint(LCAP); i++, p++) {\n#else\n' + LOOP_S + '#endif\n')
    s = s.replace(HIT, HIT + '#ifdef LCAP\n                    lit++;\n#endif\n')
    return s


def compile_(defines, tr):
    with tempfile.TemporaryDirectory() as tmp:
        spv = Path(tmp) / 'l.spv'; src = Path(tmp) / 'ClusteredLightingWithSSAO_lcap.frag'
        src.write_text(capped_source(), encoding='utf-8')
        run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', *defines, '-I', str(SRC.parent), '-o', str(spv), str(src)])
        run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(spv)])
        data = spv.read_bytes()
    check_iface(data, tr['spirv'])
    return data


def write_inc(data, path, what):
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    lines = ['// [DOS2] N6 DM_HACK_LIGHTS: SPIR-V of tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag + the LCAP text patch of tools/dos2_light_cap.py %s (glslc --target-env=vulkan1.1 -O), '
             'made by tools/dos2_light_cap.py' % what]
    for i in range(0, len(w), 8): lines.append(', '.join('0x%08x' % x for x in w[i:i + 8]) + ',')
    path.write_text('\n'.join(lines) + '\n')


def shared_list(rng, np_, ns_):
    """c12 with every cluster at offset 0 and the same counts, c11 = random light indices: any pixel reads the same list"""
    n = 16384 // 2
    c12 = np.stack([np.zeros(n, np.uint32), np.full(n, np_ | (ns_ << 16), np.uint32)], 1).astype(np.uint32).tobytes()
    c11 = bytearray(rng.integers(0, LM.NL, 65536).astype(np.uint8).tobytes())
    return c12, c11


def prefix(c11, np_, ns_, k):
    """the cluster cut to the first k entries of (points, then spots): the list rewritten, counts lowered"""
    kp = min(np_, k); ks = min(ns_, k - kp)
    b = bytearray(c11); b[0:kp] = c11[0:kp]; b[kp:kp + ks] = c11[np_:np_ + ks]
    n = 16384 // 2
    c12 = np.stack([np.zeros(n, np.uint32), np.full(n, kp | (ks << 16), np.uint32)], 1).astype(np.uint32).tobytes()
    return c12, bytes(b)


def prove(x, var, big, cap, n, seed):
    """1. LCAP=99 (never reached) == x bit for bit: the loop change itself changes nothing;
    2. LCAP=cap == x on some prefix of the list (the prefix that ends at the cap-th contributing light), bit for bit; also counted: how often
       the cap cut anything (the variant differs from x on the whole list)"""
    rng = np.random.default_rng(seed)
    mx, mv, mb = SI.Module(x), SI.Module(var), SI.Module(big)
    same_big = pref = cut = 0
    for it in range(n):
        np_, ns_ = int(rng.integers(0, 13)), int(rng.integers(0, 5))
        model = int(rng.choice([0, 0, 2]))
        inp, ub, tx = LM.case('ClusteredLightingWithSSAO', rng, it % 4 == 3, (np_, ns_, model))
        c12, c11 = shared_list(rng, np_, ns_); ub[64], ub[70] = c12, bytes(c11)
        full = mx.run(inp, ub, tx); v = mv.run(inp, ub, tx)
        same_big += LM.same(mb.run(inp, ub, tx), full)
        if not LM.same(v, full): cut += 1
        ok = False
        for k in range(np_ + ns_ + 1):
            ub2 = dict(ub); ub2[64], ub2[70] = prefix(c11, np_, ns_, k)
            if LM.same(mx.run(inp, ub2, tx), v): ok = True; break
        pref += ok
    print('LCAP=%d: LCAP=99 == x bit for bit %d / %d; variant == x on a prefix of the list %d / %d (the cap cut something in %d)'
          % (cap, same_big, n, pref, n, cut))
    return (n - same_big) + (n - pref)


def cost(x, n=40):
    rng = np.random.default_rng(3); m = SI.Module(x); rows = []
    for np_, ns_ in ((0, 0), (1, 0), (2, 0), (4, 0), (8, 0), (0, 1), (0, 2), (0, 4), (4, 2), (8, 4)):
        ex = []
        for it in range(n):
            inp, ub, tx = LM.case('ClusteredLightingWithSSAO', rng, False, (np_, ns_, 0))
            c12, c11 = shared_list(rng, np_, ns_); ub[64], ub[70] = c12, bytes(c11)
            m.run(inp, ub, tx); ex.append(m.executed)
        rows.append((np_, ns_, float(np.mean(ex))))
    base = rows[0][2]
    for np_, ns_, e in rows: print('  cost x module: %d point + %d spot in the cluster: %.0f instructions / lit pixel (+%.0f over no lights)' % (np_, ns_, e, e - base))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--n', type=int, default=200)
    a = ap.parse_args()
    tr = read_bundle(Path(a.bundles) / (KEY + '.xgs'))
    x = compile_([], tr); bad = 0
    big = compile_(['-DLCAP=99'], tr)
    for cap in (2, 4):
        v = compile_(['-DLCAP=%d' % cap], tr)
        write_inc(v, OUT / ('light_lcap%d.frag.spv.inc' % cap), '-DLCAP=%d' % cap)
        print('built light_lcap%d.frag.spv.inc: %d words (x %d words), interface = the translated one' % (cap, len(v) // 4, len(x) // 4))
        bad += prove(x, v, big, cap, a.n, 0x11c0 + cap)
    cost(x)
    if bad: raise SystemExit('%d failures' % bad)


if __name__ == '__main__':
    main()
