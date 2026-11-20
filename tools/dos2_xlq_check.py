"""NR-32 DM_NR_XLQ: offline proof and cost of the engine's dead-varying trim (dm/gfx/vulkan/engine/xbg_spirv_vsout.cpp) on a whole shader
pack. Nothing ships from here; it runs the production C++ through its offline runner (tests/vsout_scan.cpp, built on the host).

Pairs: every (vertex, fragment) program of setup's programs.tsv (work/shaders-jobs), plus --pairs files (vs<TAB>fs a line, e.g. the pairs of
an NR-HEAT log). Both stages get the phone's load-time rewrites first (--chain nftcpu = NAN_MINMAX + NAN_SAT, LDC_FOLD, FORWARD_TEMPS,
LDC_CANON, F2I_NOTRUNC, NR-27 UBOVEC), so "before" is what a Mali phone compiles today. Per distinct (vertex shader, fragment read mask):
  1. structure  the runner's own check (xbg_spirv_vsout_check) of every trim;
  2. spirv-val  --target-env vulkan1.1 on the module before and after (NDK r29 SPIRV-Tools);
  3. equiv      tools/spv_interp.py (the NR-27 fuzzing interpreter of dos2_ubovec_check.py) runs both on the same random inputs and
                uniform-block contents and compares the built-in outputs (gl_Position ...) in full and every Location output component
                the fragment shader reads; --negative N also compares a dropped component on N modules and expects a difference there
                (the comparison sees the stores it lets go);
  4. malioc     Arm Mali Offline Compiler cycles before / after per core (position + varying variants summed on IDVS cores); with
                --heat (an NR-HEAT shader-heat.tsv ranking as json from dos2_shader_heat.py --json) also weighted by vertices a frame.

usage: dos2_xlq_check.py --pack <shaders.xgspack> --out <new dir> [--programs work/shaders-jobs/programs.tsv] [--pairs extra.tsv ...]
           [--scan <vsout_scan exe> | --cxx clang++] --spirv-tools <dir with spirv-val> [--malioc <malioc exe>]
           [--cores Mali-G52,Mali-G57,Mali-G715,Mali-G720] [--heat heat.json] [--runs 6] [--negative 24] [--threads 14] [--limit N]
Findings with numbers: docs/dos2/nr/NR-32.md."""
import argparse, collections, concurrent.futures as cf, glob, json, os, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'dm', 'gfx', 'vulkan', 'engine')
sys.path.insert(0, HERE)
import dos2_ubovec_check as uv  # noqa: E402  (make_fuzz, ubo_bytes, malioc, bound, exe)


def build_scan(cxx, out):
    dst = uv.exe(os.path.join(out, 'vsout_scan'))
    src = [os.path.join(ENGINE, 'tests', 'vsout_scan.cpp')] + [os.path.join(ENGINE, f) for f in (
        'xbg_spirv_vsout.cpp', 'xbg_spirv_ubovec.cpp', 'xbg_spirv_ldc.cpp', 'xbg_spirv_temps.cpp', 'xbg_spirv_peep.cpp')]
    r = subprocess.run([cxx, '-O2', '-std=c++17', '-D_CRT_SECURE_NO_WARNINGS', '-Wno-deprecated-declarations'] + src + ['-o', dst],
                       capture_output=True, text=True)
    if r.returncode: sys.exit('build of vsout_scan failed:\n' + r.stderr[-3000:])
    return dst


def masks_of(m):
    """var id -> (location, first component) of the Location outputs of a spv_interp module"""
    out = {}
    for v, (pt, sc) in m.vars.items():
        d = m.dec.get(v, {})
        if sc == 3 and 30 in d: out[v] = (d[30][0], d.get(31, [0])[0])
    return out


def pick(val, loc, first, keep, inverse=False):
    """the components of an output value whose mask bit is in keep (or, inverse, not in keep)"""
    vals = val if isinstance(val, list) else [val]; r = []
    for k, x in enumerate(vals):
        bit = 1 << (4 * loc + first + k)
        if bool(keep & bit) != inverse: r.append((k, repr(x)))
    return r


_FZ = None


def equiv_one(job):
    """(tag, before, after, keep, runs, negative) -> dict"""
    global _FZ
    tag, pb, pa, keep, runs, negative = job
    if _FZ is None: _FZ = uv.make_fuzz()
    Fuzz, np = _FZ
    try:
        mb, ma = Fuzz(open(pb, 'rb').read()), Fuzz(open(pa, 'rb').read()); mb.setup(set()); ma.setup(set())
        lb = masks_of(mb); diff = None; seen_drop = False; n = 0
        for r in range(runs):
            seed = zlib.crc32(tag.encode()) + r * 7919; profile = r % 4; rng = np.random.default_rng(seed)
            ubos = [uv.ubo_bytes(np, rng, profile if r % 8 < 4 else 3) for _ in range(4)]
            eb, _, _ = mb.run2(seed, profile, ubos, 20000); OB = {v: mb._copy(mb.mem[v]) for v, (pt, sc) in mb.vars.items() if sc == 3}
            ea, _, _ = ma.run2(seed, profile, ubos, 20000); OA = {v: ma._copy(ma.mem[v]) for v, (pt, sc) in ma.vars.items() if sc == 3}; n += 1
            if eb == 'cut' or ea == 'cut': continue
            if eb != ea: diff = 'run %d: end %s / %s' % (r, eb, ea); break
            for v, val in OB.items():
                if v in lb:
                    loc, first = lb[v]; want = pick(val, loc, first, keep)
                    got = pick(OA[v], loc, first, keep) if v in OA else []
                    if want and want != got: diff = 'run %d: output loc %d differs: %s / %s' % (r, loc, want[:4], got[:4]); break
                    if negative and v in OA and pick(val, loc, first, keep, True) != pick(OA[v], loc, first, keep, True): seen_drop = True
                    if negative and v not in OA and any(x not in (repr(np.float32(0)), '0', '0.0') for _, x in pick(val, loc, first, keep, True)): seen_drop = True
                elif v not in OA or repr(OA[v]) != repr(val): diff = 'run %d: built-in output %d differs' % (r, v); break
            if diff: break
        return dict(tag=tag, runs=n, diff=diff, seen_drop=seen_drop)
    except Exception as e:
        return dict(tag=tag, runs=0, diff=None, error='%s: %s' % (type(e).__name__, e))


def malioc_vs(mexe, path, core):
    """a vertex shader's cycles: every variant malioc reports summed (IDVS cores: position + varying shader; Bifrost: one), longest
    path where malioc gives one (else total); bound = per variant the largest pipeline, summed"""
    r = subprocess.run([mexe, '-c', core, '--vulkan', '--vertex', '--format', 'json', path], capture_output=True, text=True)
    try: sh = json.loads(r.stdout)['shaders'][0]
    except Exception: return dict(error=(r.stdout + r.stderr)[-300:])
    if sh.get('errors'): return dict(error=str(sh['errors'])[:300])
    d = dict(bound=0.0, ls=0.0, arith=0.0, wr=0, spill=False)
    for v in sh['variants']:
        perf = v['performance']; P = {p['name']: p['value'] for p in v['properties']}
        c = perf.get('longest_path_cycles')
        if not c or c['cycle_count'][0] is None: c = perf.get('total_cycles')
        cyc = dict(zip(perf['pipelines'], [x or 0 for x in c['cycle_count']]))
        d['bound'] += max(cyc.values()); d['ls'] += cyc.get('load_store', 0); d['arith'] += cyc.get('arith_total', cyc.get('arithmetic', 0))
        d['wr'] = max(d['wr'], P.get('work_registers_used') or 0); d['spill'] |= bool(P.get('has_stack_spilling'))
    return d


def malioc_one(job):
    mexe, tag, pb, pa, cores = job
    return tag, {c: (malioc_vs(mexe, pb, c), malioc_vs(mexe, pa, c)) for c in cores}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pack', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--programs', default=os.path.join(HERE, '..', '..', 'work', 'shaders-jobs', 'programs.tsv'))
    ap.add_argument('--pairs', nargs='*', default=[]); ap.add_argument('--scan'); ap.add_argument('--cxx'); ap.add_argument('--chain', default='nftcpu')
    ap.add_argument('--spirv-tools'); ap.add_argument('--malioc'); ap.add_argument('--cores', default='Mali-G52,Mali-G57,Mali-G715,Mali-G720')
    ap.add_argument('--heat'); ap.add_argument('--runs', type=int, default=6); ap.add_argument('--negative', type=int, default=24)
    ap.add_argument('--threads', type=int, default=14); ap.add_argument('--limit', type=int, default=0); ap.add_argument('--json')
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
    pairs = set()
    for l in open(a.programs, encoding='utf-8'):
        x = l.rstrip('\n').split('\t')
        if len(x) >= 5 and x[0] != 'label' and x[1] != '-' and x[3] != '-' and x[2] == '-': pairs.add((x[1], x[3]))
    for p in a.pairs:
        for l in open(p): x = l.split(); pairs.add((x[0], x[1])) if len(x) >= 2 and x[0] != 'vs' else None
    pf = os.path.join(a.out, 'pairs_in.tsv'); open(pf, 'w').write(''.join('%s\t%s\n' % p for p in sorted(pairs)))
    scan = a.scan or build_scan(a.cxx or 'clang++', a.out)
    r = subprocess.run([scan, a.pack, pf, a.out, a.chain, 'write'], capture_output=True, text=True)
    print('structure:', r.stdout.strip(), ('\n' + r.stderr.strip()) if r.stderr.strip() else ''); res = dict(scan=r.stdout.strip(), scan_rc=r.returncode)
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(a.out, 'pairs.tsv'))][1:]
    tags = sorted({os.path.basename(f)[:-len('.after.vert.spv')] for f in glob.glob(os.path.join(a.out, '*.after.vert.spv'))})
    if a.limit: tags = tags[:a.limit]
    B = lambda t: os.path.join(a.out, t + '.before.vert.spv'); A = lambda t: os.path.join(a.out, t + '.after.vert.spv')
    if a.spirv_tools:
        val = uv.exe(os.path.join(a.spirv_tools, 'spirv-val'))
        def v1(t): return t, [subprocess.run([val, '--target-env', 'vulkan1.1', p], capture_output=True).returncode for p in (B(t), A(t))]
        with cf.ThreadPoolExecutor(a.threads) as ex: vr = list(ex.map(v1, tags))
        bad = [t for t, rc in vr if rc[1] and not rc[0]]; res['spirv_val'] = dict(modules=len(tags), before_bad=sum(1 for _, rc in vr if rc[0]), after_bad=len(bad))
        print('spirv-val: %d modules, before refused %d, after refused %d (new refusals %d)' % (len(tags), res['spirv_val']['before_bad'], sum(1 for _, rc in vr if rc[1]), len(bad)))
    # equivalence: keep = the read mask in the tag (<vs>.<read hex>)
    jobs = [(t, B(t), A(t), int(t.split('.')[1], 16), a.runs, i < a.negative) for i, t in enumerate(tags)]
    with cf.ProcessPoolExecutor(a.threads) as ex: er = list(ex.map(equiv_one, jobs, chunksize=8))
    diffs = [e for e in er if e.get('diff')]; errs = [e for e in er if e.get('error')]; neg = [e for e in er[:a.negative]]
    res['equiv'] = dict(modules=len(er), runs=sum(e['runs'] for e in er), differ=len(diffs), errors=len(errs),
                        negative=len(neg), negative_seen=sum(1 for e in neg if e.get('seen_drop')))
    print('equiv: %d modules, %d run pairs, %d differ, %d interpreter errors; negative control: dropped components seen in %d of %d'
          % (len(er), res['equiv']['runs'], len(diffs), len(errs), res['equiv']['negative_seen'], len(neg)))
    for e in diffs[:8]: print('  DIFF', e['tag'], e['diff'])
    for e in errs[:4]: print('  ERROR', e['tag'], e['error'][:200])
    if a.malioc:
        cores = a.cores.split(',')
        with cf.ThreadPoolExecutor(a.threads) as ex: mr = dict(ex.map(malioc_one, [(a.malioc, t, B(t), A(t), cores) for t in tags]))
        W = collections.Counter()
        if a.heat:
            h = json.load(open(a.heat))
            for lv in h['levels'].values():
                for x in lv['rows']:
                    if x.get('vs') and x.get('fs'): W[(x['vs'], x['fs'])] += x['verts'] / max(1, lv['frames'])
        read_of = {(x[0], x[1]): x[3] for x in rows}
        res['malioc'] = {}
        for c in cores:
            s = collections.Counter(); better = worse = errn = 0
            for t, d in mr.items():
                x, y = d[c]
                if 'error' in x or 'error' in y: errn += 1; continue
                bx, by = x['bound'], y['bound']; s['b'] += bx; s['a'] += by; s['lsb'] += x['ls']; s['lsa'] += y['ls']; s['spb'] += x['spill']; s['spa'] += y['spill']; better += by < bx; worse += by > bx
            for (vs, fs), v in W.items():
                t = '%s.%s' % (vs, read_of.get((vs, fs), ''))
                if t in mr and 'error' not in mr[t][c][0] and 'error' not in mr[t][c][1]: s['wb'] += v * mr[t][c][0]['bound']; s['wa'] += v * mr[t][c][1]['bound']
            res['malioc'][c] = dict(s, better=better, worse=worse, errors=errn)
            print('malioc %s: bound cycles %.0f -> %.0f (%+.1f%%), better %d worse %d errors %d%s' % (
                c, s['b'], s['a'], 100 * (s['a'] - s['b']) / max(1e-9, s['b']), better, worse, errn,
                ' | weighted by vertices a frame %.2fM -> %.2fM (%+.1f%%)' % (s['wb'] / 1e6, s['wa'] / 1e6, 100 * (s['wa'] - s['wb']) / s['wb']) if s['wb'] else ''))
    if a.json: json.dump(res, open(a.json, 'w'), indent=1)
    ok = r.returncode == 0 and not diffs and (not a.spirv_tools or not res['spirv_val']['after_bad']) and res['equiv']['negative_seen'] == res['equiv']['negative']
    print('RESULT', 'PASS' if ok else 'FAIL'); return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
