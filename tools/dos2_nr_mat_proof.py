"""NR-33B (DM_NR_MAT) proof over the player's translated pack: the vertex-output pruning (dm/gfx/nr/nr_mat.c nr_mat_prune) on every
distinct (vertex shader, fragment input mask) pair of the material programs, and the empty fragment shader.

For each program of programs.tsv with both stages (default: *_DEP_NX and *_DEF_NX; --all every one) and each keep mask the host can use
(the fragment shader's Location inputs; 0 = the empty fragment shader of a depth-only draw), the C pruning (run through the CLI of
dm/gfx/vulkan/engine/tests/nr_mat_test.c) must give a module that spirv-val accepts and that, on tools/spv_interp.py with seeded random
vertex inputs / uniforms, writes gl_Position (and every kept output) bit for bit as the translated one. Static counts (instructions after
spirv-opt -O, what a driver sees at best) are reported per family for the doc.

  dos2_nr_mat_proof.py --pack work/shaders-vk/shaders.xgspack --programs work/shaders-jobs/programs.tsv --tool nr_mat_test.exe --out DIR
       [--n 4] [--limit 0] [--all]
Output: DIR/proof.tsv (one row per distinct pair), DIR/proof.txt (summary). Exit 1 on any mismatch / invalid module.
"""
import argparse, collections, hashlib, random, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import numpy as np
import spv_interp as SI
from dos2_nr_mat import pack_index, bundle_parts
from dos2_native_shaders import tool


def io_masks(spirv):
    w = struct.unpack('<%dI' % (len(spirv) // 4), spirv); i = 5; loc = {}; var = {}; fn = False
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if op == 71 and w[i + 2] == 30: loc[w[i + 1]] = w[i + 3]
        if op == 54: fn = True
        if op == 59 and not fn: var[w[i + 2]] = w[i + 3]
        i += n
    ins = sum(1 << loc[v] for v, sc in var.items() if sc == 1 and v in loc)
    outs = sum(1 << loc[v] for v, sc in var.items() if sc == 3 and v in loc)
    return ins, outs


def count(spirv, tmp):
    a, b = Path(tmp) / 'c.spv', Path(tmp) / 'co.spv'; a.write_bytes(spirv)
    r = subprocess.run([tool('spirv-opt'), '-O', str(a), '-o', str(b)], capture_output=True)
    if r.returncode: return -1
    w = b.read_bytes(); w = struct.unpack('<%dI' % (len(w) // 4), w); i = 5; n = 0; fn = False
    while i < len(w):
        c, op = w[i] >> 16, w[i] & 0xffff
        if op == 54: fn = True
        if fn and op not in (248, 54, 56, 55): n += 1
        i += c
    return n


def run_vs(m, ins_locs, ubo_bindings, rng_seed):
    rng = random.Random(rng_seed)
    inputs = {l: tuple(rng.uniform(-2, 2) for _ in range(4)) for l in ins_locs}
    inputs[('bi', 42)] = rng.randrange(0, 1 << 16); inputs[('bi', 43)] = rng.randrange(0, 64)
    ubos = {b: np.array([rng.uniform(-2, 2) for _ in range(4096)], dtype=np.float32).tobytes() + b'\0' * (65536 - 16384) for b in ubo_bindings}
    return m.run(inputs, ubos=ubos)


def bits(v):
    if isinstance(v, list): return [bits(x) for x in v]
    if isinstance(v, (float, np.floating)): return SI.f2u(v)
    return v


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pack', required=True); ap.add_argument('--programs', required=True); ap.add_argument('--tool', required=True)
    ap.add_argument('--out', required=True); ap.add_argument('--n', type=int, default=4); ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    data, idx = pack_index(a.pack)
    get = lambda k: data[idx[k][0]:idx[k][0] + idx[k][1]] if k in idx else None
    pairs = collections.OrderedDict()   # (vs sha, keep, empty FS) -> [vs key, fs key, names, family suffix]
    fams = collections.Counter()
    for line in Path(a.programs).read_text(encoding='utf-8').splitlines()[1:]:
        f = line.split('\t')
        if len(f) < 5 or f[1] == '-' or f[3] == '-' or f[2] != '-': continue
        name = f[0].rsplit('/', 1)[-1][:-5]
        if not a.all and not (name.endswith('_DEP_NX') or name.endswith('_DEF_NX')): continue
        vb, fb = get(f[1]), get(f[3])
        if not vb or not fb: continue
        vs, fs = bundle_parts(vb)[2], bundle_parts(fb)[2]
        fin, _ = io_masks(fs)
        for keep, empty in ([(fin, False), (0, True)] if name.endswith('_DEP_NX') else [(fin, False)]):
            k = (hashlib.sha1(vs).hexdigest(), keep, empty)
            if k not in pairs: pairs[k] = [f[1], f[3], [], name.rsplit('_', 2)[-2]]
            pairs[k][2].append(name)
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    rows = ['vs_key\tkeep\toutputs\tpruned\tremoved\tvalid\truns\tequal\tinstr_opt\tinstr_opt_pruned\tprograms']
    tot = collections.Counter(); bad = []
    items = list(pairs.items())[:a.limit] if a.limit else list(pairs.items())
    with tempfile.TemporaryDirectory() as tmp:
        for n_, ((sha, keep, empty), (vk, fk, names, suf)) in enumerate(items):
            vb = get(vk); stage, binds, vs = bundle_parts(vb)
            src = Path(tmp) / 'v.spv'; dst = Path(tmp) / 'p.spv'; src.write_bytes(vs)
            r = subprocess.run([a.tool, 'prune', str(src), '%x' % keep, str(dst)], capture_output=True, text=True).stdout.split()
            _, vo = io_masks(vs)
            if not r or r[0] != 'removed':
                tot['refused %s' % (r[1] if len(r) > 1 else '?')] += 1
                rows.append('%s\t%x\t%x\t-\t%s\t\t\t\t\t\t%s' % (vk, keep, vo, ' '.join(r), ','.join(names[:3]))); continue
            pv = dst.read_bytes()
            val = subprocess.run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(dst)], capture_output=True, text=True)
            ok_val = val.returncode == 0
            if not ok_val: bad.append('%s keep %x: spirv-val: %s' % (vk, keep, val.stderr.strip()[:200]))
            m0, m1 = SI.Module(vs), SI.Module(pv)
            ins = [m0.dec[v][30][0] for v, (pt, sc) in m0.vars.items() if sc == 1 and 30 in m0.dec.get(v, {})]
            ubo = sorted({m0.dec.get(v, {}).get(33, [None])[0] for v, (pt, sc) in m0.vars.items() if sc == 2})
            eq = runs = 0; why = ''
            for k in range(a.n):
                try: o0 = run_vs(m0, ins, ubo, int(vk[:8], 16) + k); o1 = run_vs(m1, ins, ubo, int(vk[:8], 16) + k)
                except (NotImplementedError, KeyError, ValueError, TypeError) as e: why = 'interp: %s' % e; break
                runs += 1
                same = all(bits(o0[key]) == bits(o1[key]) for key in o0 if not isinstance(key, int) or (keep >> key) & 1)
                eq += same
                if not same: bad.append('%s keep %x run %d: kept outputs / gl_Position differ' % (vk, keep, k))
            c0, c1 = count(vs, tmp), count(pv, tmp)
            tot['pruned'] += 1; tot['valid'] += ok_val; tot['runs'] += runs; tot['equal'] += eq; tot['not interpretable'] += bool(why)
            fams[(suf, empty, 'opt')] += c0; fams[(suf, empty, 'opt_p')] += c1; fams[(suf, empty, 'n')] += 1
            fams[(suf, empty, 'slots')] += bin(vo).count('1'); fams[(suf, empty, 'slots_p')] += bin(int(r[3], 16)).count('1')
            rows.append('%s\t%x\t%x\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%s' % (vk, keep, vo, r[3], r[1], ok_val, runs, eq, c0, c1, ','.join(names[:3]) + (' ' + why if why else '')))
            if n_ % 200 == 199: print('  %d / %d pairs' % (n_ + 1, len(items)), flush=True)
    (out / 'proof.tsv').write_text('\n'.join(rows) + '\n', encoding='utf-8')
    rep = ['distinct (vertex shader, keep mask) pairs %d: %s' % (len(items), ', '.join('%s %d' % kv for kv in sorted(tot.items())))]
    for suf in sorted({k[0] for k in fams}):
        for empty in (False, True):
            n = fams[(suf, empty, 'n')]
            if not n: continue
            rep.append('  %s %-24s pairs %4d: VS instructions after spirv-opt -O %6.1f -> %6.1f (-%4.1f %%), Location outputs %4.2f -> %4.2f vec4 slots' % (
                suf, 'keep = FS inputs' if not empty else 'keep = 0 (empty FS)', n, fams[(suf, empty, 'opt')] / n, fams[(suf, empty, 'opt_p')] / n,
                100.0 * (1 - fams[(suf, empty, 'opt_p')] / max(1, fams[(suf, empty, 'opt')])), fams[(suf, empty, 'slots')] / n,
                (fams[(suf, empty, 'slots')] - fams[(suf, empty, 'slots_p')]) / n))
    rep += ['MISMATCH ' + b for b in bad[:20]]
    (out / 'proof.txt').write_text('\n'.join(rep) + '\n', encoding='utf-8'); print('\n'.join(rep))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
