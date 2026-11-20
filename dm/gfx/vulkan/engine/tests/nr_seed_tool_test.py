#!/usr/bin/env python3
"""NR-31: tools/dos2_pipe_seed.py against the runtime core (dm/gfx/nr/nr_seed.c, through nr_seed_test.c's modes).

  python nr_seed_tool_test.py [--bin nr_seed_test(.exe)] [--tl2] [--dos2]
Builds nr_seed_test with clang when --bin is not given (PATH or C:/Program Files/LLVM/bin). Checks:
  - the C unit / model / fuzz test passes;
  - a synthetic Turnip list (the C forward model of draw()) -> `build --bundles` (layouts re-derived from the bundle headers) -> `audit`
    (every byte accounted for) -> the C reader expands every seed record to its own policy = the list's record byte for byte;
  - a damaged record in a list is left out with its reason (the tool keeps the records before it, as the engine does);
  - a seed with one flipped body byte is refused by both readers (id);
  - --tl2: the real TL2 Mi 14 lists (same engine, tl2-port...): seed + audit, and the Turnip list re-targeted to SIM mali
    reproduces every record of the SIM-mali list (nr_seed_test --real).
  - --dos2 (NR-31b): the DOS2 phone list of 17.11 (Mi 14, native runs + SIM mali / powervr / ubdyn8 runs + a safe-mode ladder, every
    current default: NR-04 / 06 / 08 / 08b / 09 / 10 / 20, NB): `build` with the runs' dm.logs leaves out only duplicates, `audit` accounts
    for every byte, and the seed re-targeted to each of those policies (nr_seed_test --real-seed) gives back every list record byte for
    byte except the NR-08 split ones (refused by design).
"""
import argparse, os, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]
TOOL = ROOT / 'tools' / 'dos2_pipe_seed.py'
PORTS = Path(os.environ.get('DM_PORTS_DIR', ROOT.parent))   # the folder that holds the tl2-port / dos2-port trees
TL2_LIST = (PORTS / 'tl2-port/work/save-backups/20261116-1822/vk-pipeline-list.bin')
TL2_MALI = (PORTS / 'tl2-port-repo/work/warm-mali/vk-pipeline-list-mali-sim.bin')
TL2_BUNDLES = (PORTS / 'tl2-port/work/shaders-vk')
DOS2 = (PORTS / 'dos2-port')
DOS2_LIST, DOS2_LOGS = DOS2 / 'work/seed/vk-pipeline-list.bin', DOS2 / 'src/work/logs'
DOS2_BUNDLES, DOS2_POST = DOS2 / 'work/shaders-vk', DOS2 / 'work/shaders-native-post-a/fused.xgspack'
# the source (Mi 14, current defaults) first, then the runs that wrote other records into the same list
DOS2_RUNS = ['nr13_f6a', 'nr15_g4b', 'nr12_pvr', 'nr12_pvr_nr08b', 'nr11_dir2m', 'nr7_c5a', 'nr14_ladder']


def run(*cmd, ok=True):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    out = r.stdout + r.stderr
    if ok and r.returncode: print(out); raise SystemExit(f'FAIL: {" ".join(map(str, cmd))} -> {r.returncode}')
    return r.returncode, out


def build_bin(tmp):
    cc = shutil.which('clang') or (r'C:/Program Files/LLVM/bin/clang.exe' if Path(r'C:/Program Files/LLVM/bin/clang.exe').exists() else None)
    if not cc: raise SystemExit('SKIP: no clang (pass --bin)')
    exe = tmp / ('nr_seed_test.exe' if sys.platform == 'win32' else 'nr_seed_test')
    run(cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-D_CRT_SECURE_NO_WARNINGS', HERE / 'nr_seed_test.c', ROOT / 'dm/gfx/nr/nr_seed.c',
        ROOT / 'dm/gfx/nr/nr_ubodyn.c', '-o', exe)
    return exe


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--bin', default=''); ap.add_argument('--tl2', action='store_true'); ap.add_argument('--dos2', action='store_true')
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix='nr31-'))
    exe = Path(a.bin) if a.bin else build_bin(tmp)
    _, out = run(exe); assert 'PASS (0 failures)' in out, out; print('C test: ' + out.strip().splitlines()[-1])
    lst, bdir, seed = tmp / 'syn.bin', tmp / 'b', tmp / 'syn.seed'; bdir.mkdir()
    run(exe, '--write-list', lst, 600); run(exe, '--write-bundles', bdir)
    nosplit = tmp / 'nosplit.json'; nosplit.write_text('{"split": 0}')   # the C model's Turnip device has no NR-08 split (the preset, the Mi 14 now, has)
    _, out = run(sys.executable, TOOL, 'build', '--list', lst, '--out', seed, '--bundles', bdir, '--policy-json', nosplit)
    assert 'left out: nothing' in out, out
    _, out = run(sys.executable, TOOL, 'audit', seed); assert 'every byte accounted for: True; records refused by the decoder: 0' in out, out
    _, out = run(exe, '--check-seed', seed, lst); assert ' 600 records, 600 expand' in out, out; print('seed round trip: ' + out.strip())
    # a damaged record: everything before it is kept, the rest dropped (as warm::start)
    b = bytearray(lst.read_bytes()); off = 0
    for _ in range(100):
        ns = struct.unpack_from('<I', b, off + 4)[0]; o = off + 8
        for _ in range(ns): o += 4 + struct.unpack_from('<I', b, o)[0]
        off = o + 4 + 4 * struct.unpack_from('<I', b, o)[0]
    struct.pack_into('<I', b, off, 0x12345678); bad = tmp / 'bad.bin'; bad.write_bytes(bytes(b))
    _, out = run(sys.executable, TOOL, 'build', '--list', bad, '--out', tmp / 'bad.seed')
    assert 'stopped at a damaged record' in out and ': 100 records of 100' in out, out
    # one field the draw path never writes (a pass load op): refused as shape
    b = bytearray(lst.read_bytes()); ns = struct.unpack_from('<I', b, 4)[0]; o = 8
    for _ in range(ns): o += 4 + struct.unpack_from('<I', b, o)[0]
    words = o + 4; ln = struct.unpack_from('<I', b, words + 4)[0]; pass_at = words + 8 + 4 * ln   # version, ln, layout, pn
    struct.pack_into('<I', b, pass_at + 4 + 8 + 12, 1)   # pn, flags, natt, att0.flags, format, samples -> loadOp = CLEAR
    sh = tmp / 'shape.bin'; sh.write_bytes(bytes(b))
    _, out = run(sys.executable, TOOL, 'build', '--list', sh, '--out', tmp / 'shape.seed')
    assert 'shape: pass.att.ops' in out and '599 records of 600' in out, out
    # a tampered seed: both readers refuse it
    s = bytearray(seed.read_bytes()); s[-7] ^= 0x40; t = tmp / 'tampered.seed'; t.write_bytes(bytes(s))
    rc, out = run(sys.executable, TOOL, 'audit', t, ok=False); assert rc and 'damaged' in out, out
    rc, out = run(exe, '--check-seed', t, lst, ok=False); assert rc and 'not a seed' in out, out
    print('tool: damaged list record / unknown field / tampered seed handled')
    if a.tl2:
        if not (TL2_LIST.exists() and TL2_MALI.exists() and TL2_BUNDLES.exists()): print('SKIP --tl2: lists not found'); return 0
        _, out = run(sys.executable, TOOL, 'build', '--list', TL2_LIST, '--out', tmp / 'tl2.seed', '--bundles', TL2_BUNDLES); assert 'left out: nothing' in out, out
        _, out = run(sys.executable, TOOL, 'audit', tmp / 'tl2.seed'); assert 'every byte accounted for: True' in out, out
        _, out = run(exe, '--real', TL2_LIST, TL2_MALI, TL2_BUNDLES, 0); print(out.strip())
        assert 'expanded 950, of them exactly a record of the destination list 293' in out, out
    if a.dos2:
        logs = [DOS2_LOGS / f'{r}.dm.log' for r in DOS2_RUNS]
        if not (DOS2_LIST.exists() and DOS2_BUNDLES.exists() and DOS2_POST.exists() and all(l.exists() for l in logs)): print('SKIP --dos2: list / logs / bundles not found'); return 0
        ds = tmp / 'dos2.seed'
        _, out = run(sys.executable, TOOL, 'build', '--list', DOS2_LIST, '--log', *logs, '--bundles', DOS2_BUNDLES, '--post-pack', DOS2_POST, '--out', ds)
        left = out.split('left out: ')[1].splitlines()[0]
        assert all(x.strip().startswith('duplicate') for x in left.split(',')), out
        assert 'self-check: ' in out and out.split('self-check: ')[1].split(' of ')[0] == out.split('self-check: ')[1].split(' of ')[1].split()[0], out
        print('dos2 build: ' + [l for l in out.splitlines() if 'records of' in l][0].split(': ', 1)[1])
        _, out = run(sys.executable, TOOL, 'audit', ds); assert 'every byte accounted for: True; records refused by the decoder: 0' in out, out
        fdir = tmp / 'fused'; fdir.mkdir()
        sys.path.insert(0, str(TOOL.parent)); import dos2_pipe_seed as tool
        for k, b in tool.Bundles('', DOS2_POST).post.items(): (fdir / f'{k}.xgs').write_bytes(b)
        pols = []
        for r, l in zip(DOS2_RUNS, logs):
            nb = len(tool.policies_from_log(l.read_text(encoding='utf-8', errors='replace')))
            for b in range(nb):
                _, w = run(sys.executable, TOOL, 'policy', '--log', l, '--boot', b, '--words'); f = tmp / f'{r}.{b}.pol'; f.write_text(w); pols.append(f)
        _, out = run(exe, '--real-seed', ds, DOS2_LIST, DOS2_BUNDLES, fdir, *pols); print(out.strip())
        first = [l for l in out.splitlines() if 'nr13_f6a.0.pol' in l][0]
        n = int(out.split(', ')[1].split()[0])
        assert f'expanded {n}, of them a record of the list {n}' in first, first   # the Mi 14 itself: every seed record is one of its records
        back = out.split('given back byte for byte by one of the policies: ')[1]
        assert 'other words 0' in back, back
        nsplit = sum(int(x.split('refused split ')[1].split(';')[0].split()[0]) for x in out.splitlines() if 'refused split ' in x)
        assert int(back.split('layout only ')[1].split()[0]) <= 5 and nsplit > 0, back   # only the NR-08 split layouts (ubdyn8 runs) are not given back
    print('PASS: NR-31 tool / core')
    return 0


if __name__ == '__main__':
    sys.exit(main())
