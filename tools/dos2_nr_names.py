"""DOS2 NR-02: the table shader name <-> shader pack key, offline and against a phone dump (NR-04 replaces engine shaders by name).

Offline: work/shaders-jobs/programs.tsv (tools/dos2_shader_jobs.py) lists every .bshd of the packages with the pack keys of its stages
(label = <pak>:<path>, vs / gs / ps / cs = xbg_shader_program_key of each stage: SHA-256 of the control section + code, the key the host's
bundle files and DM_NR's tap use). Runtime: nr_names.tsv of a DM_NR_KEY=1 + DM_NR_DUMP=1 run (name = the ShaderManager::Get name, path =
LoadShaderFile path, vs_key / fs_key / cs_key from nvnProgramSetShaders).
  (default)        offline table: name (path without the pak and .bshd) -> keys, written with --out; counts per pak
  --dump F         also join a phone dump: every runtime shader is matched by its keys (exact stage pair) and by name; prints
                   matched / key-only / name-only / unknown and every name whose keys differ from the offline ones (a red line each)
Usage: dos2_nr_names.py [--programs work/shaders-jobs/programs.tsv] [--dump nr_names.tsv] [--out names.tsv] [--only EngineShaders]
"""
import argparse, collections, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def offline(path, only):
    rows = []
    for line in Path(path).read_text(encoding='utf-8').splitlines()[1:]:
        f = line.split('\t')
        if len(f) < 5 or ':' not in f[0]: continue
        pak, p = f[0].split(':', 1)
        if only and pak != only: continue
        name = p[:-5] if p.endswith('.bshd') else p
        rows.append({'pak': pak, 'path': p, 'name': name, 'vs': f[1], 'ps': f[3], 'cs': f[4]})
    return rows


def norm(n):
    n = n.replace('\\', '/').strip()
    if n.endswith('.bshd'): n = n[:-5]
    return n.lower()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--programs', default=str(ROOT.parent / 'work' / 'shaders-jobs' / 'programs.tsv'))
    ap.add_argument('--dump', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--only', default='')
    a = ap.parse_args()
    off = offline(a.programs, a.only)
    per = collections.Counter(r['pak'] for r in off)
    print('== offline: %d programs with a name (%s)' % (len(off), ', '.join('%s %d' % kv for kv in sorted(per.items()))))
    if a.out:
        with open(a.out, 'w', newline='\n', encoding='utf-8') as o:
            o.write('name\tpak\tvs_key\tfs_key\tcs_key\n')
            for r in off: o.write('%s\t%s\t%s\t%s\t%s\n' % (r['name'], r['pak'], r['vs'], r['ps'], r['cs']))
        print('wrote %s' % a.out)
    if not a.dump: return 0
    by_pair = {(r['vs'], r['ps'], r['cs']): r for r in off}
    by_name = collections.defaultdict(list)
    for r in off:
        by_name[norm(r['name'])].append(r); by_name[norm(r['name'].rsplit('/', 1)[-1])].append(r)
    got = collections.Counter(); red = []
    lines = Path(a.dump).read_text(encoding='utf-8').splitlines()
    head = lines[0].split('\t')
    for line in lines[1:]:
        f = dict(zip(head, line.split('\t')))
        keys = (f.get('vs_key', '-'), f.get('fs_key', '-'), f.get('cs_key', '-'))
        hit = by_pair.get(keys)
        nm = f.get('name', '-')
        cand = by_name.get(norm(nm), []) if nm != '-' else []
        if hit and (not cand or hit in cand): got['keys and name agree' if cand else 'keys only (no name at runtime)'] += 1
        elif hit: got['keys match another name'] += 1; red.append('%s: keys %s are %s offline' % (nm, '/'.join(keys), hit['name']))
        elif cand: got['name only (keys differ)'] += 1; red.append('%s: runtime keys %s, offline %s' % (nm, '/'.join(keys), '/'.join((cand[0]['vs'], cand[0]['ps'], cand[0]['cs']))))
        else: got['unknown (engine-built / main / missing)'] += 1
    print('== %s: %d runtime shaders: %s' % (a.dump, len(lines) - 1, ', '.join('%s %d' % kv for kv in got.most_common())))
    sys.stdout.flush()
    for r in red[:40]: sys.stdout.buffer.write(('【红】' + r + '\n').encode('utf-8'))
    return 1 if red else 0


if __name__ == '__main__':
    sys.exit(main())
