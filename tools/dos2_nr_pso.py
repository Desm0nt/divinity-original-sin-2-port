"""DOS2 NR-07: the iPad build's pipeline-state catalogue (ShaderCache.pak, 3970 .pso) as a list of Switch shader pack key pairs, and the
coverage of a device's warm-up list (files/dm/vk-pipeline-list.bin) against it. Names and keys only: nothing of the iPad files is
read beyond the .pso file NAMES in the package index, nothing is written into the port's data.

The iPad engine precompiles one .pso per (material shader x pipeline state) it met while Larian played the game through
("Platform/iOS/shadercache/<path>_Metal.bshd_<16 hex>.pso"); the Switch .bshd of the same material is "<path>_NX.bshd" (IPA_RECON.md §4:
5445 of 5448 Switch materials have a Metal twin; _SHA is iPad-only). work/shaders-jobs/programs.tsv names the pack keys of every Switch
.bshd (vs / ps = xbg_shader_program_key: the keys of the host's bundles and of the warm-up records). So the catalogue says which
(VS, FS) key pairs the whole game draws with and how many state variants each has - the pipelines a device has to build sooner or later.
A device's warm-up list holds the pipelines it built so far (one record per create-info): its pairs / the catalogue's = how much of the
game a boot warm-up already covers, i.e. how many pipelines are still built in a frame the first time an unseen area loads.

  (default)            catalogue summary: .pso files, materials, mapped to Switch keys, distinct pairs, variants per material, by variant kind
  --list F [F ...]     coverage of these vk-pipeline-list.bin files (records, distinct pairs, in the catalogue / engine or UI shaders outside it)
  --out F              the catalogue as TSV: vs, ps, switch path, iPad state variants (keys and names only)
Usage: dos2_nr_pso.py [--pak <ShaderCache.pak>] [--programs work/shaders-jobs/programs.tsv] [--list vk-pipeline-list.bin ...] [--out F]
"""
import argparse, collections, re, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lspk  # noqa: E402

PSO = re.compile(r'^Platform/iOS/shadercache/(.+)_Metal\.bshd_([0-9A-Fa-f]{1,16})\.pso$')   # the hash is printed without leading zeros
KIND = re.compile(r'_(DEF|DEP|FOR|SHA)$')
MAGIC, MAX_STAGES, MAX_KEY, MAX_WORDS = 0x57504C58, 5, 256, 4096


def catalogue(pak):
    """{switch .bshd path: set of iPad variant hashes} from the package index (file names only)."""
    out = collections.defaultdict(set)
    for e in lspk.Pak(pak).entries:
        m = PSO.match(e[0].replace('\\', '/'))
        if m: out[m.group(1) + '_NX.bshd'].add(m.group(2).lower())
    return out


def programs(path):
    """{.bshd path (without the pak): (vs, ps)}"""
    keys = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines()[1:]:
        f = line.split('\t')
        if len(f) < 5 or ':' not in f[0]: continue
        keys[f[0].split(':', 1)[1]] = (f[1], f[3])
    return keys


def warm_records(data):
    """[stage keys] of each record of a vk-pipeline-list.bin (the engine's warm::start reader; stops at the first bad record)."""
    i, out = 0, []
    def u32():
        nonlocal i
        if i + 4 > len(data): return None
        v = struct.unpack_from('<I', data, i)[0]; i += 4; return v
    while i < len(data):
        magic, ns = u32(), u32()
        if magic != MAGIC or ns is None or ns > MAX_STAGES: break
        keys = []
        for _ in range(ns):
            n = u32()
            if n is None or n > MAX_KEY or i + n > len(data): return out
            keys.append(data[i:i + n].decode('ascii', 'replace')); i += n
        count = u32()
        if count is None or count > MAX_WORDS or i + count * 4 > len(data): break
        i += count * 4
        out.append(keys)
    return out


def base(k):
    return k.split('#', 1)[0].lower()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pak', default=str(ROOT.parent / 'work' / 'ipa' / 'Payload' / 'Divinity Original Sin 2.app' / 'Data' / 'ShaderCache.pak'))
    ap.add_argument('--programs', default=str(ROOT.parent / 'work' / 'shaders-jobs' / 'programs.tsv'))
    ap.add_argument('--list', nargs='*', default=[])
    ap.add_argument('--out', default='')
    a = ap.parse_args()
    cat, keys = catalogue(a.pak), programs(a.programs)
    nfiles = sum(len(v) for v in cat.values())
    kinds = collections.Counter()
    mapped, pairs, unmapped = {}, collections.defaultdict(int), collections.Counter()
    for path, var in cat.items():
        m = KIND.search(path[:-len('_NX.bshd')])
        kind = m.group(1) if m else 'other'
        kinds[kind] += len(var)
        k = keys.get(path)
        if not k or k[0] == '-' or k[1] == '-': unmapped[kind] += 1; continue
        mapped[path] = k; pairs[k] += len(var)
    hist = collections.Counter(min(len(v), 9) for v in cat.values())
    print(f'iPad ShaderCache.pak: {nfiles} distinct (material, state) .pso over {len(cat)} materials; by variant kind: ' + ', '.join(f'{k} {n}' for k, n in sorted(kinds.items())))
    print(f'state variants per material: ' + ', '.join(f'{k if k < 9 else "9+"}: {n}' for k, n in sorted(hist.items())))
    print(f'mapped to Switch pack keys (programs.tsv): {len(mapped)} materials, {len(pairs)} distinct (VS, FS) pairs, {sum(pairs.values())} iPad pipelines; '
          f'not mapped: ' + (', '.join(f'{k} {n}' for k, n in sorted(unmapped.items())) or 'none') + ' (_SHA is iPad-only: the Switch draws shadows with _DEP)')
    if a.out:
        with open(a.out, 'w', encoding='utf-8', newline='\n') as o:
            o.write('vs\tps\tswitch_path\tipad_variants\n')
            for path, k in sorted(mapped.items()): o.write(f'{k[0]}\t{k[1]}\t{path}\t{len(cat[path])}\n')
        print(f'catalogue: {a.out} ({len(mapped)} rows)')
    allpairs = {(base(v), base(p)) for v, p in pairs}
    allkeys = {base(x) for v, p in pairs for x in (v, p)}
    for f in a.list:
        recs = warm_records(Path(f).read_bytes())
        rp = collections.Counter()
        for r in recs:
            ks = [base(k) for k in r]
            rp[tuple(ks[:2]) if len(ks) >= 2 else tuple(ks)] += 1
        inside = {p for p in rp if len(p) == 2 and p in allpairs}
        outside_engine = sum(n for p, n in rp.items() if not (len(p) == 2 and p in allpairs) and not any(k in allkeys for k in p))
        print(f'{f}: {len(recs)} records, {len(rp)} distinct stage pairs; {len(inside)} of the catalogue\'s {len(allpairs)} material pairs covered '
              f'({100.0 * len(inside) / max(1, len(allpairs)):.1f} %, {sum(rp[p] for p in inside)} records); records outside the catalogue {sum(rp.values()) - sum(rp[p] for p in inside)} '
              f'(engine / UI / particle shaders the iPad cache does not list: {outside_engine}); iPad pipelines of the uncovered pairs: '
              f'{sum(n for (v, p), n in pairs.items() if (base(v), base(p)) not in inside)} (built in a frame the first time their area loads, unless async-skippable)')


if __name__ == '__main__':
    main()
