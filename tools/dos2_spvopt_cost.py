#!/usr/bin/env python3
"""NR-25: does spirv-opt -O / -Os make DOS2's translated shaders cheaper for a GPU? Offline evidence, nothing ships from here.

For the modules a frame actually draws (GPASS lines of a DM_GFX_PASS_PROF run give each FS / VS its GPU time; "NVN shader <key> stage=N
loaded" lines give the working set) and optionally a random sample of the pack: SPIR-V from shaders.xgspack -> raw / spirv-opt -O / -Os
(the NDK r29 SPIRV-Tools v2022.4, the same library the on-device DM_GFX_SPV_OPT links) -> spirv-val (vulkan1.1) -> Arm Mali Offline
Compiler (malioc, the Mali driver's own compiler; free Arm Performance Studio, extract the MSI with `msiexec /a`, no install) per core:
cycles per pipeline (arith / load_store / varying / texture) of the longest path (total for modules with loops), the bound pipeline,
work / uniform registers, stack spilling. With --native it also measures a native pack (DM_NR_SH, NR-04) under the same keys.

usage: dos2_spvopt_cost.py --pack <shaders.xgspack> --logs <dir with gp1.log gp2.log rc1.log> --malioc <malioc.exe>
                           --spirv-tools <dir with spirv-opt / spirv-val> [--cores Mali-G57,Mali-G52,Mali-G715,Mali-G720]
                           [--random 300] [--native <native pack>] [--json out.json] [--top 30] [--threads 12]
Findings with numbers: docs/dos2/nr/NR-25.md."""
import argparse, collections, concurrent.futures as cf, json, os, random, re, statistics, struct, subprocess, sys, tempfile

PIPES = ('arith_total', 'load_store', 'varying', 'texture')
STAGE = {0: 'vert', 4: 'frag', 5: 'comp'}
GPASS = re.compile(r'NVN GPASS frame=(\d+) .* i=(\d+) kind=(\w+) us=(\d+) (\d+)x(\d+) att=(\d+) fmt=(\d+) draws=(\d+) verts=(\d+) '
                   r'first=(\S+) last=(\S+)')
LOADED = re.compile(r'NVN shader ([0-9a-f]{16}) stage=(\d) loaded')


def read_pack(path):
    data = open(path, 'rb').read()
    magic, ver, n, _ = struct.unpack_from('<4I', data, 0)
    if magic != 0x50534758 or ver != 1: sys.exit(f'{path}: not an xgspack v1')
    idx = {}
    for i in range(n):
        k, o, sz, _, _ = struct.unpack_from('<32sQIIQ', data, 16 + i * 56)
        idx[k.rstrip(b'\0').decode()] = (o, sz)
    return data, idx


def bundle_spv(data, idx, key):
    """XGS1 bundle: 8 u32 head (stage = word 2, bindings = 3, vertex inputs = 4, c1 bytes = 5, SPIR-V bytes = 6), bindings 32 B each,
    inputs 4 B each, c1, SPIR-V."""
    o, sz = idx[key]; b = data[o:o + sz]; h = struct.unpack_from('<8I', b, 0)
    off = 32 + h[3] * 32 + h[4] * 4 + h[5]
    return h[2], b[off:off + h[6]] if h[6] else b[off:]


def weights(logs, min_passes=60):
    """GPU us per profiled frame for each FS / VS key: a pass's time goes to the FS (VS) of its first and last draw, split evenly; only
    frames with >= min_passes passes (the campaign, not the menu / loading)."""
    fs, vs, frames = collections.Counter(), collections.Counter(), 0
    for name in ('gp1.log', 'gp2.log'):
        p = os.path.join(logs, name)
        if not os.path.isfile(p): continue
        per = collections.defaultdict(list)
        for line in open(p, encoding='utf-8', errors='replace'):
            m = GPASS.search(line)
            if m: per[m.group(1)].append(m)
        for ms in per.values():
            if len(ms) < min_passes: continue
            frames += 1
            for m in ms:
                if not int(m.group(9)): continue
                ks = [dict(x.split(':', 1) for x in s.split(';') if ':' in x) for s in (m.group(11), m.group(12))]
                for st, C in (('4', fs), ('0', vs)):
                    u = {d[st] for d in ks if st in d}
                    for k in u: C[k] += int(m.group(4)) / len(u)
    seen = {'4': set(), '0': set()}
    for name in ('rc1.log', 'gp1.log', 'gp2.log'):
        p = os.path.join(logs, name)
        if not os.path.isfile(p): continue
        for line in open(p, encoding='utf-8', errors='replace'):
            m = LOADED.search(line)
            if m and m.group(2) in seen: seen[m.group(2)].add(m.group(1))
    n = max(frames, 1)
    return frames, {k: v / n for k, v in fs.items()}, {k: v / n for k, v in vs.items()}, seen['4'], seen['0']


def ninst(spv):
    w = struct.unpack_from("<%dI" % (len(spv) // 4), spv); c, i = 0, 5
    while i < len(w): c += 1; i += max(1, w[i] >> 16)
    return c


def malioc(exe, path, core):
    r = subprocess.run([exe, '-c', core, '--vulkan', '--format', 'json', path], capture_output=True, text=True)
    try: sh = json.loads(r.stdout)['shaders'][0]
    except Exception: return dict(error=(r.stdout + r.stderr)[-300:])
    if sh.get('errors'): return dict(error=str(sh['errors'])[:300])
    v = sh['variants'][0]; P = {p['name']: p['value'] for p in v['properties']}; perf = v['performance']
    d = dict(wr=P.get('work_registers_used'), ur=P.get('uniform_registers_used'), spill=P.get('has_stack_spilling'))
    for k, s in (('total_cycles', 'tot'), ('longest_path_cycles', 'long')):
        c = perf.get(k)
        if c: d[s] = dict(zip(perf['pipelines'], c['cycle_count'])); d[s]['bound'] = c['bound_pipelines']
    return d


def path_of(d):
    l = d.get('long')
    return l if l and l.get('arith_total') is not None else d['tot']


def bound(d):
    p = path_of(d); return max((p.get(k) or 0) for k in PIPES)


def measure(a, data, idx, keys, td, levels=('O', 'Os')):
    opt = os.path.join(a.spirv_tools, 'spirv-opt' + ('.exe' if os.name == 'nt' else ''))
    val = os.path.join(a.spirv_tools, 'spirv-val' + ('.exe' if os.name == 'nt' else ''))

    def one(key):
        st, spv = bundle_spv(data, idx, key); st = STAGE.get(st, 'x%d' % st)
        res = dict(stage=st, words={}, inst={}, val={}, m={})
        base = os.path.join(td, '%s.%d' % (key, id(data) & 0xffff)); files = {'raw': f'{base}.raw.{st}.spv'}
        open(files['raw'], 'wb').write(spv)
        for lvl in levels:
            dst = f'{base}.{lvl}.{st}.spv'
            r = subprocess.run([opt, '-' + lvl, files['raw'], '-o', dst], capture_output=True, text=True)
            if r.returncode: res['val'][lvl] = 'spirv-opt failed: ' + (r.stderr or r.stdout)[-200:]
            else: files[lvl] = dst
        for lvl, f in files.items():
            b = open(f, 'rb').read(); res['words'][lvl] = len(b) // 4; res['inst'][lvl] = ninst(b)
            r = subprocess.run([val, '--target-env', 'vulkan1.1', f], capture_output=True, text=True)
            res['val'][lvl] = 'ok' if r.returncode == 0 else (r.stderr or r.stdout)[-200:]
            res['m'][lvl] = {c: malioc(a.malioc, f, c) for c in a.cores}
        return key, res
    with cf.ThreadPoolExecutor(a.threads) as ex: return dict(ex.map(one, keys))


def usable(r, levels):
    return all(l in r['m'] for l in levels) and all('error' not in c for l in r['m'].values() for c in l.values())


def report(title, R, cores, W=None, top=0):
    for stage, wk in (('frag', 'fs'), ('vert', 'vs')):
        ks = [k for k, r in R.items() if r['stage'] == stage and usable(r, ('raw', 'O', 'Os'))]
        if not ks: continue
        w = {k: (W[wk].get(k, 0.0) if W else 1.0) for k in ks}
        I = {l: sum(R[k]['inst'][l] for k in ks) for l in ('raw', 'O', 'Os')}
        print(f'== {title} {stage}: {len(ks)} modules' + (f' ({sum(1 for k in ks if w[k] > 0)} with GPU time)' if W else ''))
        print('   SPIR-V instructions raw %d -> -O %.2fx, -Os %.2fx; spirv-val not ok: %d' % (
            I['raw'], I['O'] / I['raw'], I['Os'] / I['raw'], sum(1 for k in ks for v in R[k]['val'].values() if v != 'ok')))
        for c in cores:
            S = {}
            for l in ('raw', 'O', 'Os'):
                s = collections.Counter()
                for k in ks:
                    d = R[k]['m'][l][c]; p = path_of(d); b = bound(d)
                    s['bound'] += b; s['w_bound'] += b * w[k]; s['wr'] += d['wr'] or 0; s['spill'] += bool(d['spill'])
                    s['gt32'] += (d['wr'] or 0) > 32
                    for q in PIPES: s[q] += p.get(q) or 0
                S[l] = s
            def pc(l, f): return '%+.1f%%' % (100.0 * (S[l][f] - S['raw'][f]) / S['raw'][f]) if S['raw'][f] else 'n/a'
            dn = sum(1 for k in ks if bound(R[k]['m']['O'][c]) < bound(R[k]['m']['raw'][c]) - 1e-6)
            up = sum(1 for k in ks if bound(R[k]['m']['O'][c]) > bound(R[k]['m']['raw'][c]) + 1e-6)
            print(f'   {c:10s} bound cycles -O {pc("O", "bound")} -Os {pc("Os", "bound")}' +
                  (f' | GPU-time weighted -O {pc("O", "w_bound")} -Os {pc("Os", "w_bound")}' if W else '') +
                  f' | arith {pc("O", "arith_total")} LS {pc("O", "load_store")} tex {pc("O", "texture")} | work regs {pc("O", "wr")}, '
                  f'>32 regs {S["raw"]["gt32"]} -> {S["O"]["gt32"]}, spilling {S["raw"]["spill"]} -> {S["O"]["spill"]} | '
                  f'modules cheaper {dn}, dearer {up}, same {len(ks) - dn - up}')
        if top and W:
            c = cores[0]
            print(f'   top {top} by GPU time ({c}): key us | bound raw -> -O (pipe) | work regs | SPIR-V instructions')
            for k in sorted(ks, key=lambda k: -w[k])[:top]:
                a, o = R[k]['m']['raw'][c], R[k]['m']['O'][c]
                print('   %s %5.0f | %6.2f -> %6.2f %-28s | %2d -> %2d | %5d -> %5d' % (
                    k, w[k], bound(a), bound(o), ','.join(path_of(a)['bound'] or []), a['wr'] or 0, o['wr'] or 0,
                    R[k]['inst']['raw'], R[k]['inst']['O']))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pack', required=True); ap.add_argument('--logs', required=True); ap.add_argument('--malioc', required=True)
    ap.add_argument('--spirv-tools', required=True); ap.add_argument('--cores', default='Mali-G57,Mali-G52,Mali-G715,Mali-G720')
    ap.add_argument('--random', type=int, default=0); ap.add_argument('--native'); ap.add_argument('--json')
    ap.add_argument('--top', type=int, default=30); ap.add_argument('--threads', type=int, default=12)
    a = ap.parse_args(); a.cores = a.cores.split(',')
    data, idx = read_pack(a.pack)
    frames, wfs, wvs, sfs, svs = weights(a.logs)
    W = dict(fs=wfs, vs=wvs)
    keys = sorted(k for k in (set(wfs) | set(wvs) | sfs | svs) if k in idx)
    print(f'{frames} profiled campaign frames; {len(wfs)} FS / {len(wvs)} VS with GPU time; working set {len(keys)} modules of {len(idx)}')
    td = tempfile.mkdtemp(prefix='nr25_')
    R = measure(a, data, idx, keys, td); out = dict(frames=frames, weights=W, working_set=R)
    report('working set', R, a.cores, W, a.top)
    if a.random:
        rng = random.Random(25)
        pool = sorted(k for k in idx if k not in R and bundle_spv(data, idx, k)[0] == 4)
        RR = measure(a, data, idx, rng.sample(pool, min(a.random, len(pool))), td); out['random_fs'] = RR
        report(f'random {len(RR)}', RR, a.cores)
    if a.native:
        nd, ni = read_pack(a.native); nk = sorted(k for k in ni if k in idx)
        N = measure(a, nd, ni, nk, td, levels=('O',)); out['native'] = N
        print(f'== native pack {a.native}: {len(nk)} modules under translated keys ({a.cores[0]} / ...), bound cycles: translated raw -> -O | native')
        for c in a.cores:
            tr = sum(bound(R[k]['m']['raw'][c]) * wfs.get(k, 0) for k in R if R[k]['stage'] == 'frag' and usable(R[k], ('raw', 'O')))
            to = sum(bound(R[k]['m']['O'][c]) * wfs.get(k, 0) for k in R if R[k]['stage'] == 'frag' and usable(R[k], ('raw', 'O')))
            nv = sum((bound(N[k]['m']['raw'][c]) if k in N else bound(R[k]['m']['raw'][c])) * wfs.get(k, 0)
                     for k in R if R[k]['stage'] == 'frag' and usable(R[k], ('raw', 'O')))
            print(f'   {c:10s} GPU-time weighted FS bound: -O {100 * (to - tr) / tr:+.1f}%, native swap {100 * (nv - tr) / tr:+.1f}%')
            for k in nk:
                if k in R:
                    print('      %s w=%5.0f  %6.2f -> %6.2f | native %6.2f' % (k, wfs.get(k, 0), bound(R[k]['m']['raw'][c]),
                                                                         bound(R[k]['m']['O'][c]), bound(N[k]['m']['raw'][c])))
    if a.json: json.dump(out, open(a.json, 'w'), indent=0)


if __name__ == '__main__':
    main()
