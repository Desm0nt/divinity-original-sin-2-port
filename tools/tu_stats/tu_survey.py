"""N1 (docs/dos2/nr/NR-NSHADER.md): rank the shaders of a heat log by GPU cost with BOTH real compilers - the Adreno 750 one (Turnip ir3,
offline through tu_stats + fakekgsl) and Arm's malioc (Mali) - on the SPIR-V the phone really compiles (the load-time chain of
xbg_game.cpp tidy_rewrite, phone_chain), and with the port's native rewrite where one replaces the translated module (NR-04 / NR-33A
pack, NB-18 fused pack).

  tu_survey.py --heat-json out.json [--level FJ_FortJoy_Main] [--native-pack <native-nr.xgspack>] [--fused-pack <fused.xgspack>]
               [--malioc <malioc.exe>] [--cores Mali-G57,Mali-G78] [--out survey.tsv] [--top 40] [--work DIR]
  out.json = tools/dos2_shader_heat.py ... --json out.json (levels -> rows of pass signature, vs, fs, draws, verts, est_ms).
Columns: est_ms (the heat tool's device split), est_a (the same pass time split by draws x Adreno issue slots instead of SPIR-V size),
Adreno FS / VS instructions (incl. nops), registers, threads (max waves x subgroup size), Mali cycles (longest path bound, all variants
summed for VS) and work registers. Needs: WSL with the host Turnip build and $HOME/n1tools/{tu_stats,fakekgsl.so,phone_chain}
(tools/tu_stats/build_tools.sh, host_turnip.sh).
"""
import argparse, collections, json, os, struct, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent))
import tu_stats as T

PACK = T.WORK / 'shaders-vk' / 'shaders.xgspack'
NR_NAMES = {}   # fs key -> engine name (dm/gfx/nr/nr_sh.c table via tools/dos2_nr_sh_pack.py ENTRIES)
try:
    import dos2_nr_sh_pack as NP
    for e in NP.ENTRIES: NR_NAMES[e[1]] = e[0]
except Exception:
    pass


def wsl(cmd):
    r = subprocess.run(['wsl', '-d', T.DISTRO, '-e', 'bash', '-c', cmd], capture_output=True, text=True, env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    if r.returncode: raise SystemExit('wsl: %s\n%s' % (cmd[:200], r.stderr[-2000:]))
    return r.stdout


def chain(keys, out, letters):
    out.mkdir(parents=True, exist_ok=True)
    kf = out / 'keys.txt'; kf.write_text('\n'.join(sorted(keys)) + '\n')
    print(wsl('%s/phone_chain %s %s %s %s' % (T.TOOLS, T.wsl_path(PACK), T.wsl_path(kf), T.wsl_path(out), letters)).strip())


def malioc(exe, path, stage, core):
    r = subprocess.run([exe, '-c', core, '--vulkan', '--' + stage, '--format', 'json', str(path)], capture_output=True, text=True)
    try:
        sh = json.loads(r.stdout)['shaders'][0]
    except Exception:
        return None
    cyc, regs = 0.0, 0
    for v in sh['variants']:
        pf = v['performance']; lp = pf.get('longest_path_cycles') or {}
        c = lp.get('cycle_count') if lp.get('cycle_count') and lp['cycle_count'][0] else pf['total_cycles']['cycle_count']
        d = dict(zip(pf['pipelines'], c))
        cyc += max(d.get(k) or 0 for k in ('arith_total', 'load_store', 'texture', 'varying'))
        regs = max(regs, {p['name']: p['value'] for p in v.get('properties', [])}.get('work_registers_used', 0))
    return round(cyc, 2), regs


def native_ref(fs, native_pack, fused_pack):
    """the pack bundle that replaces fs on the phone, or None"""
    if fs.startswith('F1') or fs.startswith('F2') or fs.startswith('F3'):
        return 'pack:%s:%s' % (fused_pack, fs) if fused_pack else None
    n = NR_NAMES.get(fs)
    if not n or not native_pack: return None
    key = 'x.' + (n if len(n) <= 29 else fs)
    return 'pack:%s:%s' % (native_pack, key)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--heat-json', required=True)
    ap.add_argument('--level', default='FJ_FortJoy_Main')
    ap.add_argument('--native-pack', default='')
    ap.add_argument('--fused-pack', default='')
    ap.add_argument('--malioc', default='')
    ap.add_argument('--cores', default='Mali-G57')
    ap.add_argument('--out', default='')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--work', default='')
    a = ap.parse_args()
    d = json.load(open(a.heat_json))
    L = d['levels'][a.level] if a.level != '*' else {'rows': d['overall'], 'frames': None}
    rows, nf = L['rows'], L['frames']
    names = {k: (v[0] if v else '?') for k, v in d.get('names', {}).items()}
    names.update({k: n for k, n in NR_NAMES.items()})
    work = Path(a.work) if a.work else Path(tempfile.mkdtemp(prefix='tu_survey_', dir=os.environ.get('TU_STATS_TMP')))
    have = lambda k: (T.WORK / 'shaders-vk' / (k + '.xgs')).exists()
    keys = {r['vs'] for r in rows if have(r['vs'])} | {r['fs'] for r in rows if have(r['fs'])}
    chain(keys, work / 'adreno', 'nftcp')
    if a.malioc: chain(keys, work / 'mali', 'nftcpu')
    jobs, seen, rowjob = [], {}, []
    for r in rows:
        if not have(r['vs']) or not (have(r['fs']) or native_ref(r['fs'], a.native_pack, a.fused_pack)): rowjob.append(None); continue
        depth = r['fmt'] in ('124', '129') and r['att'] == '1'
        att = int(r['att'])
        fm = '0' if depth else ' '.join([r['fmt']] + ['37'] * (att - 2 if att == 5 else att - 1)) if att > 1 else r['fmt']
        fsref = native_ref(r['fs'], a.native_pack, a.fused_pack) or str(work / 'adreno' / (r['fs'] + '.spv'))
        k = (r['vs'], fsref, fm)
        if k not in seen:
            seen[k] = 'j%d' % len(jobs); jobs.append([seen[k], str(work / 'adreno' / (r['vs'] + '.spv')), fsref, fm])
        rowjob.append(seen[k])
    st = collections.defaultdict(dict)
    for x in T.run(jobs):
        st[x['name']][x['stage']] = x
    mal = {}
    if a.malioc:
        cores = a.cores.split(',')
        want = {}
        for r in rows:
            nref = native_ref(r['fs'], a.native_pack, a.fused_pack)
            if nref: want[('fragment', r['fs'])] = T.spirv_of(nref, str(work), 'n_' + r['fs'].replace('.', '_'))
            elif (work / 'mali' / (r['fs'] + '.spv')).exists(): want[('fragment', r['fs'])] = work / 'mali' / (r['fs'] + '.spv')
            if (work / 'mali' / (r['vs'] + '.spv')).exists(): want[('vertex', r['vs'])] = work / 'mali' / (r['vs'] + '.spv')
        items = [(k, p, c) for k, p in want.items() for c in cores]
        with ThreadPoolExecutor(8) as ex:
            for (k, p, c), v in zip(items, ex.map(lambda t: malioc(a.malioc, t[1], t[0][0], t[2]), items)): mal[(k, c)] = v
    # est_a: each pass signature's time split by draws x (Adreno FS + VS issue slots)
    g = lambda s, k: int(s.get(k, 0) or 0)
    sig_ms, sig_w = collections.defaultdict(float), collections.defaultdict(float)
    for r, j in zip(rows, rowjob):
        sig = (r['kind'], r['wxh'], r['fmt'], r['att'])
        if r['verts'] > 6 * max(1, r['passes']): sig_ms[sig] = max(sig_ms[sig], r['pass_ms'])   # geometry passes (full-screen draws keep est_ms)
        if j and r['verts'] > 6 * max(1, r['passes']): sig_w[sig] += r['draws'] * (g(st[j].get('FS', {}), 'Instruction_Count') + g(st[j].get('VS', {}), 'Instruction_Count'))
    out = []
    for r, j in zip(rows, rowjob):
        sig = (r['kind'], r['wxh'], r['fmt'], r['att'])
        s = st.get(j, {}) if j else {}
        fs, vs = s.get('FS', {}), s.get('VS', {})
        w = r['draws'] * (g(fs, 'Instruction_Count') + g(vs, 'Instruction_Count'))
        o = dict(fs=r['fs'], vs=r['vs'], name=names.get(r['fs'], names.get(r['fs'].split('.', 1)[-1], '?')), pas='%s f%s a%s' % (r['wxh'], r['fmt'], r['att']),
                 native='yes' if native_ref(r['fs'], a.native_pack, a.fused_pack) else '', est_ms=round(r['est_ms'], 4),
                 est_a=round(r['est_ms'], 4) if r['verts'] <= 6 * max(1, r['passes']) else round(sig_ms[sig] * w / sig_w[sig], 4) if sig_w[sig] else '', draws_f=round(r['draws'] / (nf or r['frames']), 1),
                 verts_f=round(r['verts'] / (nf or r['frames'])), a_fs_inst=g(fs, 'Instruction_Count'), a_fs_nops=g(fs, 'NOPs_Count'),
                 a_fs_regs=g(fs, 'Registers_used'), a_fs_threads=g(fs, 'Max_Waves_Per_Core') * g(fs, 'Subgroup_size'), a_vs_inst=g(vs, 'Instruction_Count'),
                 a_vs_threads=g(vs, 'Max_Waves_Per_Core') * g(vs, 'Subgroup_size'))
        for c in (a.cores.split(',') if a.malioc else []):
            m = mal.get((('fragment', r['fs']), c)); v = mal.get((('vertex', r['vs']), c))
            o['%s_fs' % c] = m[0] if m else ''; o['%s_fs_regs' % c] = m[1] if m else ''; o['%s_vs' % c] = v[0] if v else ''
        out.append(o)
    out.sort(key=lambda o: -o['est_ms'])
    if a.out:
        keys_ = list(out[0].keys())
        with open(a.out, 'w') as f:
            f.write('\t'.join(keys_) + '\n')
            for o in out: f.write('\t'.join(str(o.get(k, '')) for k in keys_) + '\n')
    for o in out[:a.top]:
        mm = ' '.join('%s %s/%sr vs %s' % (c.replace('Mali-', ''), o.get(c + '_fs'), o.get(c + '_fs_regs'), o.get(c + '_vs')) for c in (a.cores.split(',') if a.malioc else []))
        print('%6.3f %6s %-17s %-40s %-18s %3s d/f %6.1f | A750 FS %4d (nop %3d) r%2d t%4d VS %4d | %s' % (
            o['est_ms'], o['est_a'], o['fs'][:17], o['name'][:40], o['pas'][:18], 'nat' if o['native'] else '', o['draws_f'],
            o['a_fs_inst'], o['a_fs_nops'], o['a_fs_regs'], o['a_fs_threads'], o['a_vs_inst'], mm))
    print('work dir:', work)


if __name__ == '__main__':
    main()
