#!/usr/bin/env python3
"""NR-27 DM_NR_UBOVEC: offline proof and cost of the engine's uniform-read merge (dm/gfx/vulkan/engine/xbg_spirv_ubovec.cpp) on a whole
shader pack. Nothing ships from here; it runs the production C++ through its offline runner (tests/ubovec_scan.cpp, built on the host).

For every module of the pack the rewrite changes (after the load-time clean-ups a phone applies first: --chain ftc = DM_GFX_LDC_FOLD,
FORWARD_TEMPS, LDC_CANON):
  1. structure  the runner's own check (xbg_spirv_ubovec_check) of every rewrite;
  2. spirv-val  --target-env vulkan1.1 on the module before and after (NDK r29 SPIRV-Tools);
  3. equiv      tools/spv_interp.py runs both on the same random inputs and uniform-block contents (several value profiles) and compares
                every output variable, the discard, and the value of every rewritten read each time it executes; instructions the
                interpreter does not know (texture reads, derivatives ...) return the same pseudo-random value in both runs, keyed by
                the instruction and its operand values. Reported per module: runs, rewritten reads executed at least once (coverage).
                The first --runs runs follow the shader's own branches; if reads are still not covered (a material that discards
                early, a light loop of zero lights), further runs are steered: half of the conditional branches are decided by a
                seeded coin (never into a block that ends in OpKill), the same way in both modules. A steered run tests the same
                equality in states the shader may never reach; the rewrite's proof uses no branch condition, so it must hold there too.
                --negative N also runs N modules rewritten with the poison switch and expects a difference (the comparison can see one);
  4. malioc     Arm Mali Offline Compiler cycles per pipeline before / after on each core (free Arm Performance Studio; extract the MSI
                with `msiexec /a`, no install). With --logs (gp1.log / gp2.log of a DM_GFX_PASS_PROF run) also weighted by GPU time.

usage: dos2_ubovec_check.py --pack <shaders.xgspack> --out <new dir> [--scan <ubovec_scan exe> | --cxx <clang++ / g++>]
           --spirv-tools <dir with spirv-val> [--malioc <malioc exe>] [--cores Mali-G52,Mali-G57,Mali-G715] [--stages 4,0]
           [--chain ftc] [--runs 4] [--blocks 6000] [--negative 12] [--logs <dir>] [--threads 12] [--limit N] [--json out.json]
Findings with numbers: docs/dos2/nr/NR-27.md."""
import argparse, collections, concurrent.futures as cf, json, os, random, re, struct, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'dm', 'gfx', 'vulkan', 'engine')
PIPES = ('arith', 'load_store', 'varying', 'texture')   # arith = arith_total (Valhall / 5th gen) or arithmetic (Bifrost)
STAGE = {0: 'vertex', 4: 'fragment', 5: 'compute'}
GPASS = re.compile(r'NVN GPASS frame=(\d+) .* i=(\d+) kind=(\w+) us=(\d+) (\d+)x(\d+) att=(\d+) fmt=(\d+) draws=(\d+) verts=(\d+) '
                   r'first=(\S+) last=(\S+)')


def exe(p): return p + ('.exe' if os.name == 'nt' and not p.lower().endswith('.exe') else '')


def build_scan(cxx, out):
    dst = exe(os.path.join(out, 'ubovec_scan'))
    src = [os.path.join(ENGINE, 'tests', 'ubovec_scan.cpp')] + [os.path.join(ENGINE, f) for f in
                                                                 ('xbg_spirv_ubovec.cpp', 'xbg_spirv_ldc.cpp', 'xbg_spirv_temps.cpp')]
    r = subprocess.run([cxx, '-O2', '-std=c++17', '-D_CRT_SECURE_NO_WARNINGS', '-Wno-deprecated-declarations'] + src + ['-o', dst],
                       capture_output=True, text=True)
    if r.returncode: sys.exit('building ubovec_scan failed:\n' + r.stderr[-2000:])
    return dst


# ------------------------------------------------------------------------------------------------------------ equivalence (spv_interp)
class Cut(Exception): pass


def make_fuzz():
    sys.path.insert(0, HERE)
    import numpy as np
    import spv_interp as si
    F = si.F
    NORESULT = {63, 99, 218, 219, 220, 221, 224, 225, 228, 4445, 4446}
    UNINTERPRETED = set(range(87, 99)) | set(range(103, 108)) | set(range(207, 216))   # image reads / queries, derivatives

    class Fuzz(si.Module):
        """spv_interp.Module with: inputs of any type, uninterpreted instructions as pseudo-random functions of their operands, a block
        budget instead of the instruction limit, and a trace of the watched result ids."""
        def setup(self, watch): self.watch = watch

        def value(self, t, seed, profile):
            op, a = self.types[t]; r = random.Random(seed)
            def scalar(o):
                if o == 22:
                    k = profile if profile < 3 else r.randrange(3)
                    return F(r.uniform(-2, 2)) if k == 0 else F(r.randrange(0, 6)) if k == 1 else F(r.random())
                if o == 21: return r.randrange(0, 9) if profile != 0 else r.randrange(0, 1 << 32)
                if o == 20: return r.random() < 0.5
                return None
            if op in (20, 21, 22): return scalar(op)
            if op in (23, 24): return [self.value(a[0], seed * 31 + k + 1, profile) for k in range(a[1])]
            if op == 28: return [self.value(a[0], seed * 31 + k + 1, profile) for k in range(self.consts[a[1]])]
            if op == 30: return [self.value(m, seed * 31 + k + 1, profile) for k, m in enumerate(a)]
            return None

        def run2(self, seed, profile, ubos, blocks, steer=False):
            self.ubufs, self.mem, self.tex = {}, {}, {}
            self.coin = random.Random(seed ^ 0x5eed) if steer else None
            if steer and not hasattr(self, 'kills'):
                self.kills = {l for fn in self.funcs.values() for l, blk in fn['blocks'].items() if blk and blk[-1][0] in (252, 4416, 255)}
            self.executed, self.tex_ops, self.trace, self.left, self.profile, self.seed = 0, 0, [], blocks, profile, seed
            outs = []
            for v, (pt, sc) in sorted(self.vars.items()):
                t = self.types[pt][1][1]; d = self.dec.get(v, {})
                if sc in (2, 12): self.ubufs[v] = ubos[d.get(33, [0])[0] % len(ubos)]
                elif sc == 9: self.ubufs[v] = ubos[0]
                elif sc == 0: self.tex[v] = None
                elif sc == 1: self.mem[v] = self.value(t, seed * 1000003 + d.get(30, [d.get(11, [77])[0] + 100])[0], profile)
                else: self.mem[v] = self._null(t)
                if sc == 3: outs.append(v)
            end = 'return'
            try: self._call(self.entry, [])
            except si.Discard: end = 'discard'
            except Cut: end = 'cut'
            return end, [(v, repr(self.mem[v])) for v in outs], self.trace

        def _call(self, fid, args):
            fn = self.funcs[fid]; env = dict(zip(fn['params'], args)); prev, cur = None, fn['order'][0]
            while True:
                self.left -= 1
                if self.left < 0: raise Cut()
                nxt = None
                for op, a in fn['blocks'][cur]:
                    if op == 245:
                        for k in range(2, len(a), 2):
                            if a[k + 1] == prev: env[a[1]] = self._get(env, a[k]); break
                        continue
                    if op in (246, 247, 8, 317): continue
                    if op == 249: nxt = a[0]; break
                    if op == 250:
                        nxt = a[1] if self._get(env, a[0]) else a[2]
                        if self.coin is not None:
                            if self.coin.random() < 0.5: nxt = a[1] if self.coin.random() < 0.5 else a[2]
                            if nxt in self.kills: nxt = a[2] if nxt == a[1] else a[1]
                        break
                    if op == 251:
                        sel = self._get(env, a[0]); nxt = a[1]
                        for k in range(2, len(a), 2):
                            if a[k] == sel: nxt = a[k + 1]; break
                        break
                    if op == 253: return None
                    if op == 254: return self._get(env, a[0])
                    if op in (252, 4416): raise si.Discard()
                    if op == 255: raise Cut()
                    self._exec(env, op, a)
                prev, cur = cur, nxt

        def _exec(self, env, op, a):
            try:
                if op in UNINTERPRETED: raise NotImplementedError
                si.Module._exec(self, env, op, a)
            except (NotImplementedError, KeyError, TypeError, IndexError, ValueError, AttributeError, ZeroDivisionError, OverflowError,
                    struct.error):
                if op in NORESULT or op in (59, 61, 62) or len(a) < 2 or a[0] not in self.types: return
                key = repr((op, a[1], [env.get(x) if x in env else x for x in a[2:]]))
                env[a[1]] = self.value(a[0], zlib.crc32(key.encode()) ^ self.seed, self.profile)
            if (op == 61 or op == 81) and a[1] in self.watch:
                v = env.get(a[1]); self.trace.append((a[1], si.f2u(v) if not isinstance(v, int) else v))
    return Fuzz, np


def ubo_bytes(np, rng, profile, n=1 << 16):
    """n bytes of 32-bit words: 0 = any float bits in [-2, 2), 1 = small integers as floats, 2 = small integers as integers,
    3 = each word one of the three (what a light list looks like: offsets, counts and vectors side by side)."""
    f = rng.uniform(-2, 2, n // 4).astype('<f4').view('<u4'); sf = rng.integers(0, 6, n // 4).astype('<f4').view('<u4')
    si_ = rng.integers(0, 9, n // 4).astype('<u4')
    if profile == 0: return f.tobytes()
    if profile == 1: return sf.tobytes()
    if profile == 2: return si_.tobytes()
    pick = rng.integers(0, 3, n // 4); return np.where(pick == 0, f, np.where(pick == 1, sf, si_)).astype('<u4').tobytes()


_FZ = None


def equiv_one(job):
    """(key, before path, after path, runs, blocks, expect_difference) -> dict"""
    global _FZ
    key, pb, pa, runs, blocks, negative = job
    if _FZ is None: _FZ = make_fuzz()
    Fuzz, np = _FZ
    try:
        b, a = open(pb, 'rb').read(), open(pa, 'rb').read()
        mb, ma = Fuzz(b), Fuzz(a)
        # rewritten reads: OpLoad result ids of `before` that are OpCompositeExtract results in `after`
        loads = {x[1] for fn in mb.funcs.values() for blk in fn['blocks'].values() for op, x in blk if op == 61}
        watch = {x[1] for fn in ma.funcs.values() for blk in fn['blocks'].values() for op, x in blk if op == 81 and x[1] in loads}
        mb.setup(watch); ma.setup(watch)
        seen, diff, ends, n, natural = set(), None, collections.Counter(), 0, 0
        for r in range(runs * 4):
            if r >= runs and len(seen) == len(watch): break
            seed = zlib.crc32(key.encode()) + r * 7919; profile = r % 4
            rng = np.random.default_rng(seed)
            ubos = [ubo_bytes(np, rng, profile if r % 8 < 4 else 3) for _ in range(4)]
            steer = r >= runs
            eb, ob, tb = mb.run2(seed, profile, ubos, blocks, steer); ea, oa, ta = ma.run2(seed, profile, ubos, blocks, steer)
            n += 1; ends[('steered ' if steer else '') + eb] += 1; seen.update(i for i, _ in tb)
            if not steer: natural = len(seen)
            if (eb, ob, tb) != (ea, oa, ta):
                diff = 'run %d (seed %d profile %d): end %s/%s, outputs %s, trace %s' % (
                    r, seed, profile, eb, ea, 'same' if ob == oa else 'DIFFER', 'same' if tb == ta else 'DIFFERS')
                break
        return dict(key=key, runs=n, watch=len(watch), covered=len(seen), natural=natural, diff=diff, ends=dict(ends))
    except Exception as e:                                                       # an interpreter gap is reported, not hidden
        return dict(key=key, runs=0, watch=0, covered=0, natural=0, diff=None, error='%s: %s' % (type(e).__name__, e))


# ------------------------------------------------------------------------------------------------------------ malioc
def malioc(mexe, path, core, stage):
    r = subprocess.run([mexe, '-c', core, '--vulkan', '--' + stage, '--format', 'json', path], capture_output=True, text=True)
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
    """Cycles per pipeline of the longest path; a module with loops has none, then the total (each loop body once)."""
    l = d.get('long'); p = l if l and l.get('load_store') is not None else d['tot']
    q = dict(p); q['arith'] = p.get('arith_total') if p.get('arith_total') is not None else p.get('arithmetic')
    return q


def bound(d): return max((path_of(d).get(k) or 0) for k in PIPES)


def weights(logs, min_passes=60):
    """GPU us per profiled frame for each FS key (NR-25's rule: a pass's time goes to the FS of its first and last draw)."""
    fs, frames = collections.Counter(), 0
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
                u = {d['4'] for d in ks if '4' in d}
                for k in u: fs[k] += int(m.group(4)) / len(u)
    return frames, {k: v / max(frames, 1) for k, v in fs.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pack', required=True); ap.add_argument('--out', required=True); ap.add_argument('--scan'); ap.add_argument('--cxx')
    ap.add_argument('--spirv-tools'); ap.add_argument('--malioc'); ap.add_argument('--cores', default='Mali-G52,Mali-G57,Mali-G715')
    ap.add_argument('--stages', default='4,0'); ap.add_argument('--chain', default='ftc'); ap.add_argument('--runs', type=int, default=4)
    ap.add_argument('--blocks', type=int, default=6000); ap.add_argument('--negative', type=int, default=12); ap.add_argument('--logs')
    ap.add_argument('--threads', type=int, default=12); ap.add_argument('--limit', type=int, default=0); ap.add_argument('--json')
    ap.add_argument('--skip-equiv', action='store_true')
    a = ap.parse_args(); cores = a.cores.split(','); os.makedirs(a.out, exist_ok=True)
    scan = a.scan or build_scan(a.cxx or 'clang++', a.out); res = dict(chain=a.chain, stages={}); bad = 0
    for st in [int(s) for s in a.stages.split(',')]:
        d = os.path.join(a.out, 'stage%d' % st); os.makedirs(d, exist_ok=True); t0 = time.time()
        r = subprocess.run([scan, 'pack', a.pack, d, a.chain, str(st)], capture_output=True, text=True)
        print('== stage %d (%s): %s' % (st, STAGE.get(st, '?'), (r.stdout + r.stderr).strip())); bad += r.returncode != 0
        rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(d, 'modules.tsv'))][1:]
        keys = [x[0] for x in rows if int(x[5])]
        if a.limit: keys = sorted(keys, key=lambda k: zlib.crc32(k.encode()))[:a.limit]
        R = dict(scan=(r.stdout + r.stderr).strip(), changed=len([x for x in rows if int(x[5])]), measured=len(keys),
                 structure_bad=[x[0] for x in rows if x[10] != 'ok'], modules={x[0]: dict(reads=int(x[5]), vec_loads=int(x[6]), dynamic=int(x[7])) for x in rows if int(x[5])})
        res['stages'][st] = R; B = lambda k: os.path.join(d, k + '.before.spv'); A = lambda k: os.path.join(d, k + '.after.spv')
        if a.spirv_tools:
            val = exe(os.path.join(a.spirv_tools, 'spirv-val'))
            def v1(k):
                o = []
                for f in (B(k), A(k)):
                    q = subprocess.run([val, '--target-env', 'vulkan1.1', f], capture_output=True, text=True)
                    o.append('ok' if q.returncode == 0 else (q.stderr or q.stdout).strip()[-200:])
                return k, o
            with cf.ThreadPoolExecutor(a.threads) as ex: V = dict(ex.map(v1, keys))
            R['val_bad_before'] = {k: v[0] for k, v in V.items() if v[0] != 'ok'}; R['val_bad_after'] = {k: v[1] for k, v in V.items() if v[1] != 'ok'}
            new_bad = {k: v for k, v in R['val_bad_after'].items() if k not in R['val_bad_before']}; bad += len(new_bad)
            print('   spirv-val vulkan1.1: %d modules, not ok before %d, after %d (new failures %d)%s' % (
                len(keys), len(R['val_bad_before']), len(R['val_bad_after']), len(new_bad), ''.join('\n      %s %s' % kv for kv in list(new_bad.items())[:5])))
        if not a.skip_equiv:
            jobs = [(k, B(k), A(k), a.runs, a.blocks, False) for k in keys]
            with cf.ProcessPoolExecutor(a.threads) as ex: E = list(ex.map(equiv_one, jobs, chunksize=4))
            diffs = [e for e in E if e.get('diff')]; errs = [e for e in E if e.get('error')]; bad += len(diffs) + len(errs)
            tw, tc, tn = sum(e['watch'] for e in E), sum(e['covered'] for e in E), sum(e['natural'] for e in E)
            R['equiv'] = dict(modules=len(E), runs=sum(e['runs'] for e in E), reads=tw, covered=tc, covered_unsteered=tn, differ=diffs, errors=errs,
                              full=sum(1 for e in E if e['watch'] and e['covered'] == e['watch']),
                              none=[e['key'] for e in E if e['watch'] and not e['covered']],
                              ends=dict(sum((collections.Counter(e.get('ends', {})) for e in E), collections.Counter())))
            print('   equivalence (spv_interp): %d modules, %d run pairs, DIFFER %d, interpreter errors %d; rewritten reads executed %d of %d '
                  '(%.1f %%; %d unsteered), modules fully covered %d, not at all %d; runs ended %s (%.0f s)' % (
                      len(E), R['equiv']['runs'], len(diffs), len(errs), tc, tw, 100.0 * tc / max(tw, 1), tn, R['equiv']['full'], len(R['equiv']['none']),
                      R['equiv']['ends'], time.time() - t0))
            for e in (diffs + errs)[:8]: print('      %s %s' % (e['key'], e.get('diff') or e.get('error')))
            if a.negative:
                nk = sorted(keys, key=lambda k: zlib.crc32(k.encode()))[:a.negative]; nj = []
                for k in nk:
                    p = os.path.join(d, k + '.poison.spv')
                    subprocess.run([scan, 'file', B(k), p, '-', 'poison'], capture_output=True, text=True)   # before.spv already carries the chain
                    nj.append((k, B(k), p, a.runs, a.blocks, True))
                with cf.ProcessPoolExecutor(a.threads) as ex: N = list(ex.map(equiv_one, nj))
                caught = sum(1 for e in N if e.get('diff')); R['negative'] = dict(modules=len(N), caught=caught, missed=[e['key'] for e in N if not e.get('diff')])
                print('   negative control (poison: the next component): difference seen in %d of %d modules' % (caught, len(N)))
        if a.malioc:
            def m1(k): return k, {c: (malioc(a.malioc, B(k), c, STAGE[st]), malioc(a.malioc, A(k), c, STAGE[st])) for c in cores}
            with cf.ThreadPoolExecutor(a.threads) as ex: M = dict(ex.map(m1, keys))
            R['malioc'] = M; W = weights(a.logs)[1] if a.logs and st == 4 else {}
            for c in cores:
                ok = [k for k in keys if 'error' not in M[k][c][0] and 'error' not in M[k][c][1]]; s = collections.Counter(); hist = collections.Counter()
                for k in ok:
                    b_, a_ = M[k][c]
                    for q in PIPES: s['b_' + q] += path_of(b_).get(q) or 0; s['a_' + q] += path_of(a_).get(q) or 0
                    s['b_bound'] += bound(b_); s['a_bound'] += bound(a_); s['b_wr'] += b_['wr'] or 0; s['a_wr'] += a_['wr'] or 0
                    s['b_spill'] += bool(b_['spill']); s['a_spill'] += bool(a_['spill'])
                    hist['cheaper' if bound(a_) < bound(b_) - 1e-6 else 'dearer' if bound(a_) > bound(b_) + 1e-6 else 'same'] += 1
                    hist['ls_down' if (path_of(a_).get('load_store') or 0) < (path_of(b_).get('load_store') or 0) - 1e-6 else
                         'ls_up' if (path_of(a_).get('load_store') or 0) > (path_of(b_).get('load_store') or 0) + 1e-6 else 'ls_same'] += 1
                pc = lambda f: '%+.1f%%' % (100.0 * (s['a_' + f] - s['b_' + f]) / s['b_' + f]) if s['b_' + f] else 'n/a'
                print('   %-10s %d modules (malioc errors %d): bound cycles %s (%.1f -> %.1f a module) | load/store %s (%.1f -> %.1f) arith %s | bound '
                      'cheaper %d same %d dearer %d | load/store down %d same %d up %d | work regs %s, spilling %d -> %d' % (
                          c, len(ok), len(keys) - len(ok), pc('bound'), s['b_bound'] / max(len(ok), 1), s['a_bound'] / max(len(ok), 1), pc('load_store'),
                          s['b_load_store'] / max(len(ok), 1), s['a_load_store'] / max(len(ok), 1), pc('arith'), hist['cheaper'], hist['same'], hist['dearer'],
                          hist['ls_down'], hist['ls_same'], hist['ls_up'], pc('wr'), s['b_spill'], s['a_spill']))
                if W:
                    wk = [k for k in ok if W.get(k)]
                    if wk:
                        tb = sum(bound(M[k][c][0]) * W[k] for k in wk); ta = sum(bound(M[k][c][1]) * W[k] for k in wk)
                        print('      in the profiled frames: %d of them drawn, %.0f us of GPU time a frame (Adreno 750 clock); bound %+.1f%% weighted'
                              % (len(wk), sum(W[k] for k in wk), 100.0 * (ta - tb) / tb))
    if a.json: json.dump(res, open(a.json, 'w'), indent=0)
    print('RESULT: %s' % ('FAILED (%d problems)' % bad if bad else 'ok'))
    return 1 if bad else 0


if __name__ == '__main__': sys.exit(main())
