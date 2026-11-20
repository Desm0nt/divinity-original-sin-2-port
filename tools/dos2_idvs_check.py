"""T2 DM_NR_IDVS: offline proof and cost of the engine's PointSize strip (dm/gfx/vulkan/engine/xbg_spirv_idvs.cpp) on a whole shader
pack. Nothing ships from here; it runs the production C++ through its offline runner (tests/idvs_scan.cpp, built on the host).

Every vertex shader of the pack gets the phone's load-time rewrites first (--chain nftcpu, as tools/dos2_xlq_check.py), so "before" is
what a Mali phone compiles today; with --pairs the NR-32 trim of each (vs, fs) pair is applied first too (DM_NR_XLQ=1, the Mali default).
  1. structure  the runner's own check (xbg_spirv_idvs_check) of every strip;
  2. spirv-val  --target-env vulkan1.1 on every module before and after (NDK r29 SPIRV-Tools);
  3. equiv      tools/spv_interp.py (the NR-27 fuzzing interpreter) runs both on the same random inputs and uniform-block contents and
                compares every output: Location outputs in full, the gl_PerVertex block member by member except PointSize (gl_Position
                bit for bit); --negative N also compares PointSize on N modules and expects a difference there (the comparison is not blind);
  4. malioc     Arm Mali Offline Compiler per variant (IDVS cores: position / varying; Bifrost without IDVS: one "Main" variant) before /
                after per core; with --heat (dos2_shader_heat.py --json) and --pairs, weighted by vertices a frame.

usage: dos2_idvs_check.py --pack <shaders.xgspack> --out <new dir> [--scan <idvs_scan exe> | --cxx clang++] [--pairs pairs.tsv]
           --spirv-tools <dir with spirv-val> [--malioc <malioc exe>] [--cores Mali-G52,Mali-G57,Mali-G78,Mali-G715]
           [--heat heat.json] [--runs 2] [--negative 16] [--threads 12] [--limit N] [--json out.json]
Findings with numbers: docs/dos2/nr/NR-IDVS.md."""
import argparse, collections, concurrent.futures as cf, glob, json, os, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'dm', 'gfx', 'vulkan', 'engine')
sys.path.insert(0, HERE)
import dos2_ubovec_check as uv  # noqa: E402  (make_fuzz, ubo_bytes, exe)


def build_scan(cxx, out):
    dst = uv.exe(os.path.join(out, 'idvs_scan'))
    src = [os.path.join(ENGINE, 'tests', 'idvs_scan.cpp')] + [os.path.join(ENGINE, f) for f in (
        'xbg_spirv_idvs.cpp', 'xbg_spirv_vsout.cpp', 'xbg_spirv_ubovec.cpp', 'xbg_spirv_ldc.cpp', 'xbg_spirv_temps.cpp', 'xbg_spirv_peep.cpp')]
    r = subprocess.run([cxx, '-O2', '-std=c++17', '-D_CRT_SECURE_NO_WARNINGS', '-Wno-deprecated-declarations'] + src + ['-o', dst],
                       capture_output=True, text=True)
    if r.returncode: sys.exit('build of idvs_scan failed:\n' + r.stderr[-3000:])
    return dst


def ps_layout(m):
    """var id -> None (a PointSize variable: skipped) or the PointSize member index of a gl_PerVertex block (-1: not a block with it)"""
    out = {}
    for v, (pt, sc) in m.vars.items():
        if sc != 3: continue
        if m.dec.get(v, {}).get(11, [None])[0] == 1: out[v] = None; continue
        st = m.types[pt][1][1]; mem = -1
        for (s, k), d in getattr(m, 'mdec', {}).items():
            if s == st and d.get(11, [None])[0] == 1: mem = k
        out[v] = mem
    return out


_FZ = None


def equiv_one(job):
    """(tag, before, after, runs, negative) -> dict"""
    global _FZ
    tag, pb, pa, runs, negative = job
    if _FZ is None: _FZ = uv.make_fuzz()
    Fuzz, np = _FZ
    try:
        mb, ma = Fuzz(open(pb, 'rb').read()), Fuzz(open(pa, 'rb').read()); mb.setup(set()); ma.setup(set())
        lay = ps_layout(mb); diff = None; seen_ps = False; n = 0; members = 0
        for r in range(runs):
            seed = zlib.crc32(tag.encode()) + r * 7919; profile = r % 4; rng = np.random.default_rng(seed)
            ubos = [uv.ubo_bytes(np, rng, profile if r % 8 < 4 else 3) for _ in range(4)]
            eb, _, _ = mb.run2(seed, profile, ubos, 20000); OB = {v: mb._copy(mb.mem[v]) for v, (pt, sc) in mb.vars.items() if sc == 3}
            ea, _, _ = ma.run2(seed, profile, ubos, 20000); OA = {v: ma._copy(ma.mem[v]) for v, (pt, sc) in ma.vars.items() if sc == 3}; n += 1
            if eb == 'cut' or ea == 'cut': continue
            if eb != ea: diff = 'run %d: end %s / %s' % (r, eb, ea); break
            for v, val in OB.items():
                if v not in OA: diff = 'run %d: output %d gone' % (r, v); break
                k = lay.get(v, -1)
                if k is None:   # a PointSize variable
                    if negative and repr(OA[v]) != repr(val): seen_ps = True
                    continue
                if k >= 0 and isinstance(val, list):
                    for i, x in enumerate(val):
                        if i == k:
                            if negative and repr(OA[v][i]) != repr(x): seen_ps = True
                            continue
                        members += 1
                        if repr(OA[v][i]) != repr(x): diff = 'run %d: output %d member %d differs: %s / %s' % (r, v, i, repr(x)[:80], repr(OA[v][i])[:80]); break
                    if diff: break
                    continue
                members += 1
                if repr(OA[v]) != repr(val): diff = 'run %d: output %d differs' % (r, v); break
            if diff: break
        return dict(tag=tag, runs=n, diff=diff, seen_ps=seen_ps, members=members)
    except Exception as e:
        return dict(tag=tag, runs=0, diff=None, error='%s: %s' % (type(e).__name__, e))


def malioc_vs(mexe, path, core):
    """per variant (Position / Varying on IDVS, Main without IDVS): bound cycles (longest path, else total), LS, arith, registers"""
    r = subprocess.run([mexe, '-c', core, '--vulkan', '--vertex', '--format', 'json', path], capture_output=True, text=True)
    try: sh = json.loads(r.stdout)['shaders'][0]
    except Exception: return dict(error=(r.stdout + r.stderr)[-300:])
    if sh.get('errors'): return dict(error=str(sh['errors'])[:300])
    d = {}
    for v in sh['variants']:
        perf = v['performance']; P = {p['name']: p['value'] for p in v['properties']}
        c = perf.get('longest_path_cycles')
        if not c or c['cycle_count'][0] is None: c = perf.get('total_cycles')
        cyc = dict(zip(perf['pipelines'], [x or 0 for x in c['cycle_count']]))
        a = cyc.get('arith_total', cyc.get('arithmetic', 0))
        d[v['name']] = dict(bound=max([a] + [x for k, x in cyc.items() if not k.startswith('arith')]), ls=cyc.get('load_store', 0), arith=a,
                            wr=P.get('work_registers_used') or 0, spill=bool(P.get('has_stack_spilling')))
    return d


def split(m):
    """(binning cost: Position, or Main when IDVS is off; varying cost; 'idvs' / 'main')"""
    if 'Position' in m: return m['Position']['bound'], m.get('Varying', {}).get('bound', 0.0), 'idvs'
    return m['Main']['bound'], 0.0, 'main'


def malioc_one(job):
    mexe, tag, pb, pa, cores = job
    return tag, {c: (malioc_vs(mexe, pb, c), malioc_vs(mexe, pa, c)) for c in cores}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pack', required=True); ap.add_argument('--out', required=True); ap.add_argument('--pairs')
    ap.add_argument('--scan'); ap.add_argument('--cxx'); ap.add_argument('--chain', default='nftcpu')
    ap.add_argument('--spirv-tools'); ap.add_argument('--malioc'); ap.add_argument('--cores', default='Mali-G52,Mali-G57,Mali-G78,Mali-G715')
    ap.add_argument('--heat'); ap.add_argument('--runs', type=int, default=2); ap.add_argument('--negative', type=int, default=16)
    ap.add_argument('--threads', type=int, default=12); ap.add_argument('--limit', type=int, default=0); ap.add_argument('--json')
    ap.add_argument('--malioc-pairs-only', action='store_true', help='malioc only the pair modules (the --pairs set), not every pack VS')
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
    scan = a.scan or build_scan(a.cxx or 'clang++', a.out)
    cmd = [scan, a.pack, a.out, a.chain, 'write'] + ([a.pairs] if a.pairs else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    print('structure:', r.stdout.strip(), ('\n' + r.stderr.strip()[-2000:]) if r.stderr.strip() else ''); res = dict(scan=r.stdout.strip(), scan_rc=r.returncode)
    tags = sorted({os.path.basename(f)[:-len('.after.vert.spv')] for f in glob.glob(os.path.join(a.out, '*.after.vert.spv'))})
    if a.limit: tags = tags[:a.limit]
    B = lambda t: os.path.join(a.out, t + '.before.vert.spv'); A = lambda t: os.path.join(a.out, t + '.after.vert.spv')
    if a.spirv_tools:
        val = uv.exe(os.path.join(a.spirv_tools, 'spirv-val'))
        def v1(t): return t, [subprocess.run([val, '--target-env', 'vulkan1.1', p], capture_output=True).returncode for p in (B(t), A(t))]
        with cf.ThreadPoolExecutor(a.threads) as ex: vr = list(ex.map(v1, tags))
        bad = [t for t, rc in vr if rc[1] and not rc[0]]
        res['spirv_val'] = dict(modules=len(tags), before_bad=sum(1 for _, rc in vr if rc[0]), after_bad=sum(1 for _, rc in vr if rc[1]), new_bad=len(bad))
        print('spirv-val: %d modules, before refused %d, after refused %d (new refusals %d)' % (len(tags), res['spirv_val']['before_bad'], res['spirv_val']['after_bad'], len(bad)))
    jobs = [(t, B(t), A(t), a.runs, i < a.negative) for i, t in enumerate(tags)]
    with cf.ProcessPoolExecutor(a.threads) as ex: er = list(ex.map(equiv_one, jobs, chunksize=8))
    diffs = [e for e in er if e.get('diff')]; errs = [e for e in er if e.get('error')]; neg = er[:a.negative]
    res['equiv'] = dict(modules=len(er), runs=sum(e['runs'] for e in er), differ=len(diffs), errors=len(errs), members=sum(e.get('members', 0) for e in er),
                        negative=len(neg), negative_seen=sum(1 for e in neg if e.get('seen_ps')))
    print('equiv: %d modules, %d run pairs, %d differ, %d interpreter errors, %d output values compared; negative control: PointSize differs in %d of %d'
          % (len(er), res['equiv']['runs'], len(diffs), len(errs), res['equiv']['members'], res['equiv']['negative_seen'], len(neg)))
    for e in diffs[:8]: print('  DIFF', e['tag'], e['diff'])
    for e in errs[:4]: print('  ERROR', e['tag'], e['error'][:200])
    if a.malioc:
        cores = a.cores.split(',')
        mt = [t for t in tags if '.' in t] if a.malioc_pairs_only else tags
        with cf.ThreadPoolExecutor(a.threads) as ex: mr = dict(ex.map(malioc_one, [(a.malioc, t, B(t), A(t), cores) for t in mt]))
        W = collections.Counter()
        if a.heat and a.pairs:
            sys.path.insert(0, ENGINE)
            read_of = {}
            for l in open(os.path.join(a.out, 'pair_reads.tsv')) if os.path.exists(os.path.join(a.out, 'pair_reads.tsv')) else []:
                x = l.split(); read_of[(x[0], x[1])] = x[2]
            h = json.load(open(a.heat))
            for lv in h['levels'].values():
                for x in lv['rows']:
                    if x.get('vs') and x.get('fs') and (x['vs'], x['fs']) in read_of: W['%s.%s' % (x['vs'], read_of[(x['vs'], x['fs'])])] += x['verts'] / max(1, lv['frames'])
        res['malioc'] = {}
        for c in cores:
            s = collections.Counter(); modes = collections.Counter()
            for t, d in mr.items():
                x, y = d[c]
                if 'error' in x or 'error' in y: s['err'] += 1; continue
                bx, by = split(x), split(y); modes['%s->%s' % (bx[2], by[2])] += 1
                s['pb'] += bx[0]; s['pa'] += by[0]; s['vb'] += bx[1]; s['va'] += by[1]; s['better'] += by[0] < bx[0]; s['worse'] += by[0] > bx[0]
                s['vworse'] += by[1] > bx[1] and bx[2] == by[2]
                w = W.get(t, 0)
                if w: s['wpb'] += w * bx[0]; s['wpa'] += w * by[0]; s['wvb'] += w * bx[1]; s['wva'] += w * by[1]
            res['malioc'][c] = dict(s, modes=dict(modes))
            print('malioc %s: %s | binning (position / main) cycles %.0f -> %.0f (%+.1f%%), varying %.0f -> %.0f, position better %d worse %d, varying worse %d, errors %d%s' % (
                c, dict(modes), s['pb'], s['pa'], 100 * (s['pa'] - s['pb']) / max(1e-9, s['pb']), s['vb'], s['va'], s['better'], s['worse'], s['vworse'], s['err'],
                ' | weighted by vertices a frame: binning %.2fM -> %.2fM (%+.1f%%), varying %.2fM -> %.2fM' % (
                    s['wpb'] / 1e6, s['wpa'] / 1e6, 100 * (s['wpa'] - s['wpb']) / max(1e-9, s['wpb']), s['wvb'] / 1e6, s['wva'] / 1e6) if s['wpb'] else ''))
        if a.json: json.dump(dict(res=res, per=mr, W=W), open(a.json + '.malioc.json', 'w'))
    if a.json: json.dump(res, open(a.json, 'w'), indent=1)
    ok = r.returncode == 0 and not diffs and not errs and (not a.spirv_tools or not res['spirv_val']['new_bad']) and res['equiv']['negative_seen'] == res['equiv']['negative']
    print('RESULT', 'PASS' if ok else 'FAIL'); return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
