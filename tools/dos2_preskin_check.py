"""NR-PRESKIN (DM_NR_PRESKIN, docs/dos2/nr/NR-PRESKIN.md): the bit-exact proof of the pre-skinning rewrite on the player's pack.

dm/gfx/vulkan/engine/tests/nr_preskin_test (the production xbg_spirv_preskin.cpp, built for WSL) writes, per skinning vertex shader of a key
list, the module as the phone loads it (<key>.orig.spv), the consumer (.cons.spv), the slice (.slice.spv), the plan (.plan.txt) and, when
another plan's record holds all of its values, the consumer for that record (.sup.spv / .sup.txt). This script
  1. validates every module with spirv-val (--target-env vulkan1.1, the NDK's);
  2. runs the interpreter (tools/spv_interp.py: SPIR-V semantics, every float op rounded to float32 on its own) per module and --n random
     vertices: original(inputs) -> outputs O; slice(inputs, gl_VertexIndex = v) -> record words R (the storage buffer at v * m + c);
     consumer(inputs, new Location groups = R) -> O'. O' must equal O bit for bit, every output (gl_Position and every varying);
  3. superset records: P's .sup consumer fed with the OTHER plan's slice record (same inputs) must give P's original outputs - the proof
     that a value hash shared by two modules is the same value (the runtime reuses records across vertex shaders on it).
The other plan's slice gets the same uniform blocks by data key (source << 16 | NVN slot: the block sits at another binding number in another
module), random data for a block only it reads. Inputs: Location 15 (bone indices: the translator bitcasts them to int and multiplies by 48) = float words of ints 0..127; Location 14
weights in [0, 1]; others uniform in [-2, 2]; every uniform block random float32 in [-2, 2] (bones, world matrix, view-projection).
Exit 1 on any spirv-val error or difference.

  dos2_preskin_check.py --dir OUT [--n 6] [--keys K] [--val PATH-TO-spirv-val]
  dos2_preskin_check.py --dir OUT --malioc PATH-TO-malioc [--core Mali-G57] [--tsv T]   cycles (load/store bound) of original / consumer
      (position and varying variants) and slice per module, averaged by record class (<= 4 words: depth passes; wider: G-buffer / FX)
"""
import argparse, glob, os, random, struct, subprocess, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import numpy as np
import spv_interp as SI

def default_val():
    """spirv-val: $SPIRV_VAL, else the NDK's shader-tools ($ANDROID_NDK_HOME, or the newest ndk/ under $ANDROID_SDK_ROOT / $ANDROID_HOME), else PATH"""
    if os.environ.get('SPIRV_VAL'): return os.environ['SPIRV_VAL']
    roots = [os.environ.get('ANDROID_NDK_HOME', '')] + sorted(glob.glob(os.path.join(os.environ.get('ANDROID_SDK_ROOT', os.environ.get('ANDROID_HOME', '')), 'ndk', '*')), reverse=True)
    for r in roots:
        for c in glob.glob(os.path.join(r, 'shader-tools', '*', 'spirv-val*')) if r else []: return c
    return 'spirv-val'
VAL = default_val()


def plan_of(path):
    p = {}
    for line in Path(path).read_text().split('\n'):
        if not line.strip(): continue
        k, *v = line.split(); p[k] = v
    return p


def bits(v):
    if isinstance(v, dict): return {k: bits(x) for k, x in v.items()}
    if isinstance(v, list): return [bits(x) for x in v]
    if isinstance(v, (bool, np.bool_)): return bool(v)
    if isinstance(v, int): return v & 0xffffffff
    return SI.f2u(v)


def locs_of(m):
    out = {}
    for v, (pt, sc) in m.vars.items():
        d = m.dec.get(v, {})
        if sc == 1 and 30 in d: out[d[30][0]] = m.types[pt][1][1]
    return out


def ubo_bindings(m):
    return sorted({m.dec.get(v, {}).get(33, [None])[0] for v, (pt, sc) in m.vars.items() if sc == 2})


def rand_inputs(rng, locs, m):
    inp = {}
    for l, t in locs.items():
        op, a = m.types[t]; n = a[1] if op == 23 else 1; ct = m.types[a[0]][0] if op == 23 else op
        if l == 15: vals = [struct.unpack('<f', struct.pack('<I', rng.randrange(0, 128)))[0] for _ in range(n)]
        elif l == 14: vals = [rng.choice([0.0, 1.0, rng.random()]) for _ in range(n)]
        else: vals = [rng.uniform(-2, 2) for _ in range(n)]
        if ct == 21: vals = [rng.randrange(0, 128) if l == 15 else rng.randrange(0, 1 << 16) for _ in range(n)]
        inp[l] = vals
    return inp


def record(slice_m, inputs, ubos, vi, mm):
    inp = dict(inputs); inp[('bi', 42)] = vi
    slice_m.run(inp, ubos=dict(ubos))
    rec = {}
    for key, val in slice_m.ssbo.items():
        idx = key[-1]
        if idx // mm == vi: rec[idx % mm] = val & 0xffffffff
    if len(rec) != mm: raise RuntimeError('the slice stored %d of %d words' % (len(rec), mm))
    return [rec[c] for c in range(mm)]


def feed(inputs, groups, rec):
    inp = dict(inputs)
    for loc, first, width in groups:
        w = rec[first:first + width]; inp[loc] = w if width > 1 else w[0]
    return inp


def malioc_cycles(exe, core, spv):
    import json, shutil, tempfile
    d = tempfile.mkdtemp(); f = os.path.join(d, 'x.vert.spv'); shutil.copy(spv, f)
    r = subprocess.run([exe, '-c', core, '--vulkan', '--format', 'json', f], capture_output=True, text=True); shutil.rmtree(d, ignore_errors=True)
    try: j = json.loads(r.stdout)
    except Exception: return None
    out = {}
    for sh in j.get('shaders', []):
        for v in sh.get('variants', []):
            cyc = v.get('performance', {}).get('total_cycles', {}).get('cycle_count', [])
            out[v.get('name', '?')] = max(cyc) if cyc else None
    return out


def malioc_main(a, keys):
    import collections
    rows = []; agg = collections.defaultdict(lambda: collections.defaultdict(list))
    for k in keys:
        p = plan_of(os.path.join(a.dir, k + '.plan.txt')); mm = int(p['m'][0]); cls = 'depth (<= 4 words)' if mm <= 4 else 'wider'
        r = {s_: malioc_cycles(a.malioc, a.core, os.path.join(a.dir, k + s_ + '.spv')) for s_ in ('.orig', '.cons', '.slice')}
        if not all(r.values()): continue
        pv = lambda d: (d.get('Position', d.get('Main')), d.get('Varying'))
        op, ov = pv(r['.orig']); cp, cv = pv(r['.cons']); sp, _ = pv(r['.slice'])
        rows.append('%s\t%d\t%s\t%s\t%s\t%s\t%s' % (k, mm, op, ov, cp, cv, sp))
        for name, val in (('orig position', op), ('orig varying', ov), ('cons position', cp), ('cons varying', cv), ('slice', sp)):
            if val is not None: agg[cls][name].append(val)
    for cls, d in agg.items():
        print('%s %s: %d modules; mean cycles %s' % (a.core, cls, len(d['slice']), ', '.join('%s %.2f' % (n, sum(v) / len(v)) for n, v in d.items())))
    if a.tsv: Path(a.tsv).write_text('vs\tm\torig_pos\torig_var\tcons_pos\tcons_var\tslice\n' + '\n'.join(rows) + '\n')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dir', required=True); ap.add_argument('--n', type=int, default=6); ap.add_argument('--val', default=VAL)
    ap.add_argument('--keys'); ap.add_argument('--no-val', action='store_true')
    ap.add_argument('--malioc'); ap.add_argument('--core', default='Mali-G57'); ap.add_argument('--tsv')
    a = ap.parse_args()
    keys = sorted(Path(p).name[:-len('.plan.txt')] for p in glob.glob(os.path.join(a.dir, '*.plan.txt')))
    if a.keys: want = set(Path(a.keys).read_text().split()); keys = [k for k in keys if k in want]
    if a.malioc: return malioc_main(a, keys)
    bad = val_bad = runs = sup_runs = 0; errs = []
    mods = {}
    for k in keys:
        files = [os.path.join(a.dir, k + s) for s in ('.orig.spv', '.cons.spv', '.slice.spv', '.sup.spv') if os.path.exists(os.path.join(a.dir, k + s))]
        if not a.no_val:
            for f in files:
                r = subprocess.run([a.val, '--target-env', 'vulkan1.1', f], capture_output=True, text=True)
                if r.returncode: val_bad += 1; errs.append('spirv-val %s: %s' % (os.path.basename(f), (r.stdout + r.stderr).strip()[:300]))
        mods[k] = {s: SI.Module(Path(os.path.join(a.dir, k + s)).read_bytes()) for s in ('.orig.spv', '.cons.spv', '.slice.spv')}
        if os.path.exists(os.path.join(a.dir, k + '.sup.spv')): mods[k]['.sup.spv'] = SI.Module(Path(os.path.join(a.dir, k + '.sup.spv')).read_bytes())
    for k in keys:
        p = plan_of(os.path.join(a.dir, k + '.plan.txt')); mm = int(p['m'][0])
        o, c, s = mods[k]['.orig.spv'], mods[k]['.cons.spv'], mods[k]['.slice.spv']
        # own layout (xbg_spirv_preskin_layout with pos = null): groups of min(4, m - j) words at the plan's new locations, in order
        free_new = [int(x) for x in p['locs']]; groups = []; j = 0
        for loc in free_new: w = min(4, mm - j); groups.append((loc, j, w)); j += w
        if j != mm: raise SystemExit('%s: the plan locations do not cover the record' % k)
        ub = ubo_bindings(o)
        for t in range(a.n):
            rng = random.Random(hash(k) & 0xffffffff ^ (t * 7919))
            ubos = {b: np.array([rng.uniform(-2, 2) for _ in range(4096)], dtype=np.float32).tobytes() for b in ub}
            inputs = rand_inputs(rng, locs_of(o), o)
            vi = rng.randrange(0, 4096)
            O = bits(o.run(inputs, ubos=dict(ubos)))
            R = record(s, inputs, ubos, vi, mm)
            O2 = bits(c.run(feed(inputs, groups, R), ubos=dict(ubos)))
            runs += 1
            if O != O2: bad += 1; errs.append('%s run %d: consumer(slice) differs from the original' % (k, t))
            if '.sup.spv' in mods[k]:
                sp = plan_of(os.path.join(a.dir, k + '.sup.txt')); other = sp['record'][0]; om = int(sp['record'][1])
                sg = [tuple(int(x) for x in g.split(':')) for g in sp['groups']]
                os_ = mods[other]['.slice.spv']
                inp2 = dict(inputs)
                for l, tt in locs_of(mods[other]['.orig.spv']).items():
                    if l not in inp2: inp2.update({l: v for l2, v in rand_inputs(rng, {l: tt}, mods[other]['.orig.spv']).items()})
                # the other module's uniform blocks by data key (source << 16 | NVN slot): the same block may sit at another binding there
                kb = {key: int(b) for b, key in (x.split(':') for x in p['ubos'])}
                ub2 = {}
                for x in plan_of(os.path.join(a.dir, other + '.plan.txt'))['ubos']:
                    b2, key = x.split(':'); ub2[int(b2)] = ubos.get(kb.get(key), np.array([rng.uniform(-2, 2) for _ in range(4096)], dtype=np.float32).tobytes())
                R2 = record(os_, inp2, ub2, vi, om)
                O3 = bits(mods[k]['.sup.spv'].run(feed(inputs, sg, R2), ubos=dict(ubos)))
                sup_runs += 1
                if O != O3: bad += 1; errs.append('%s run %d: superset consumer with %s\'s record differs' % (k, t, other))
    for e in errs[:30]: print(e)
    print('dos2_preskin_check: %d modules, spirv-val failures %d, interpreter runs %d (own record) + %d (another plan\'s record), differences %d' %
          (len(keys), val_bad, runs, sup_runs, bad))
    return 1 if bad or val_bad else 0


if __name__ == '__main__':
    sys.exit(main())
