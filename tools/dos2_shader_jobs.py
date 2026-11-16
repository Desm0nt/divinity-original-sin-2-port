"""Divinity: Original Sin 2 DE (Switch) shader jobs for the offline translator (dmsh_xlate via dm/gfx/tools/dmsh_vk_xc2.py).

Larian keeps every NVN program in its LSPK v13 packages (RomFS Data/*.pak, read with tools/lspk.py): Shaders/NX/*.bshd in EngineShaders.pak
and <material>_<pass>_NX.bshd in Engine / Materials / CommunityMods.pak (6158 files on update v786432). A .bshd is
    +0 u32 big-endian: 0x80000000 | uncompressed size, +4 zlib stream ->
    container: ffffffff 01 02 01 18 ... then one bare GLSLCoutput per stage (magic 0x19866891, total size at +0x44), vertex first:
    6126 files vs + ps, 5 compute (one cs), 27 empty (no program).
Each GLSLCoutput has one GPU-code section, the same layout TL2's PROGRAMS/*.NVN use (tl2_shader_jobs.glslc_stage). A program = the stages
of one .bshd. The NX shader GLSL sources (*_NX.shd, GLSL 450 + GL_NV_gpu_shader5) ship next to them - reference only.

Depth: right after device init the game calls nvnDeviceSetDepthMode(dev, 1 = NEAR_IS_ZERO) and nvnDeviceSetWindowOriginMode(dev, 1)
unconditionally (main 0x938574 / 0x93858c, proc pointer slots main+0x2bbef18 / +0x2bbef10, update v786432 build 4979b200), so vertex shaders
get {"depth_zero": true} (DOS2_DEPTH_ZERO=0 turns it off).
Outputs as tl2_shader_jobs.py (rec/, jobs.tsv, programs.tsv, shader_map.tsv, shader_facts.tsv, xfb.tsv, report.txt), same KEY.
Usage: dos2_shader_jobs.py [Data dir with the .pak files] [out dir]
"""
import collections, json, os, shutil, struct, sys, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / 'tools'))   # dos2-port/tools/lspk.py
from d3_shader_jobs import host_key, xfb_layout, CTRL, GFX, SPH_STAGE
from tl2_shader_jobs import glslc_stage
from lspk import Pak

ROOT = HERE.parent
PAKS = ('EngineShaders', 'Engine', 'Materials', 'CommunityMods')
MAGIC = b'\x91\x68\x86\x19'


def bshd_stages(d, where, bad):
    """[(stage, control, code)] of one .bshd file"""
    h, = struct.unpack_from('>I', d, 0)
    if not h >> 31: bad.append(f'{where}: header {h:#x} without the 0x80000000 flag'); return []
    u = zlib.decompress(d[4:])
    if len(u) != h & 0x7fffffff: bad.append(f'{where}: {len(u):#x} bytes, header says {h & 0x7fffffff:#x}'); return []
    out, i = [], 0
    while True:
        i = u.find(MAGIC, i)
        if i < 0: break
        tot, = struct.unpack_from('<I', u, i + 0x44)
        r = glslc_stage(u[i:i + tot], f'{where}@{i:#x}', bad)
        if r: out.append(r)
        i += max(tot, 4)
    return out


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT.parent / 'work' / 'romfs' / 'Data'
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT.parent / 'work' / 'shaders-jobs'
    if (out / 'rec').exists(): shutil.rmtree(out / 'rec')
    (out / 'rec').mkdir(parents=True)
    meta, where, bad, rep, xfbs, progs, nfiles, empty = {}, collections.defaultdict(list), [], collections.Counter(), {}, [], 0, []
    for pk in PAKS:
        pak = Pak(src / f'{pk}.pak')
        for e in pak.entries:
            if not e[0].endswith('.bshd'): continue
            nfiles += 1
            label = f'{pk}:{e[0]}'
            sts = bshd_stages(pak.read(e), label, bad)
            if not sts: empty.append(label); continue
            keys = {}
            for st, ctl, code in sts:
                k = host_key(ctl, code)
                if st in keys: bad.append(f'{label}: two {st} stages'); continue
                keys[st] = k
                where[k].append(label)
                rec = bytes(0x38) + code + ctl
                if k in meta:
                    if (out / 'rec' / f'{k}.rec').read_bytes() != rec: bad.append(f'{k}: same key, different bytes ({label})')
                    continue
                (out / 'rec' / f'{k}.rec').write_bytes(rec)
                c1o, c1t = struct.unpack_from('<2I', ctl, 0x700)
                xfb = st == 'vs' and any(struct.unpack_from('<4H', ctl, 0x6dc))
                if xfb: xfbs[k] = xfb_layout(ctl)
                own, = struct.unpack_from('<I', ctl, 0x20)
                meta[k] = {'stage': st, 'code': len(code), 'ctrl': len(ctl), 'kl': own if 0x760 <= own < 0x880 else 0x880, 'c1': c1t - c1o,
                           'src': label, 'xfb': xfb, 'progs': 0, 'next': set()}
                rep[f'{st} control {len(ctl):#x}'] += 1
                if st == 'cs':   # compute local size / shared bytes (control +0x748 / +0x754, TL2 lesson: without them LocalSize is 1,1,1)
                    lc = list(struct.unpack_from('<3I', ctl, 0x748)); sh, = struct.unpack_from('<I', ctl, 0x754)
                    if not all(1 <= x <= 1024 for x in lc) or lc[0] * lc[1] * lc[2] > 1024 or sh > 49152: bad.append(f'{label}: local {lc} shared {sh}')
                    meta[k]['facts'] = {'local': lc, 'shared': (sh + 15) // 16 * 16}
            if 'cs' in keys: progs.append((label, '-', '-', '-', keys['cs']))
            elif 'vs' in keys and 'ps' in keys: progs.append((label, keys['vs'], keys.get('gs', '-'), keys['ps'], '-'))
            else: bad.append(f'{label}: stages {sorted(keys)}')
    # [main] 75 GLSLCoutput stages built into main itself (engine / UI / logo programs; 9 of the first run's 13 missing keys): pairs are
    # consecutive vs -> ps, other stages stand alone. [missing] records the host dumped for keys it did not find (xbg_game.cpp, DOS2:
    # <shaders>/../missing/<key>.rec = 0x38 zero bytes + code + control)
    extra = []
    raw = Path(os.environ.get('DOS2_MAIN_RAW', ROOT.parent / 'work' / 'exefs-update' / 'main.raw'))
    if raw.is_file():
        d = raw.read_bytes(); i = 0
        while True:
            i = d.find(MAGIC, i)
            if i < 0: break
            tot, = struct.unpack_from('<I', d, i + 0x44)
            r = glslc_stage(d[i:i + tot], f'main+{i:#x}', bad)
            if r: extra.append((f'main+{i:#x}', r))
            i += max(tot, 4)
    mdir = Path(os.environ.get('DOS2_MISSING', ROOT.parent / 'work' / 'missing'))
    for f in sorted(mdir.glob('*.rec')) if mdir.is_dir() else []:
        rec = f.read_bytes(); ci = rec.find(CTRL, 0x38)
        if ci < 0: bad.append(f'{f.name}: no control'); continue
        code, ctl = rec[0x38:ci], rec[ci:]
        nc, = struct.unpack_from('<I', ctl, 0x704); code = code[:nc]
        st = 'cs' if code[:4] != GFX else SPH_STAGE.get(struct.unpack_from('<I', code, 0x30)[0] >> 10 & 0xf, '?')
        extra.append((f'missing:{f.stem}', (st, ctl, code)))
    prev = None
    for label, (st, ctl, code) in extra:
        k = host_key(ctl, code)
        where[k].append(label)
        if k not in meta:
            rec = bytes(0x38) + code + ctl
            (out / 'rec' / f'{k}.rec').write_bytes(rec)
            c1o, c1t = struct.unpack_from('<2I', ctl, 0x700)
            own, = struct.unpack_from('<I', ctl, 0x20)
            meta[k] = {'stage': st, 'code': len(code), 'ctrl': len(ctl), 'kl': own if 0x760 <= own < 0x880 else 0x880, 'c1': c1t - c1o,
                       'src': label, 'xfb': False, 'progs': 0, 'next': set()}
            rep[f'{st} control {len(ctl):#x} (extra)'] += 1
            if st == 'cs':
                lc = list(struct.unpack_from('<3I', ctl, 0x748)); sh, = struct.unpack_from('<I', ctl, 0x754)
                meta[k]['facts'] = {'local': lc, 'shared': (sh + 15) // 16 * 16}
        if st == 'ps' and prev and prev[0] == 'vs' and label.startswith('main+'): progs.append((f'{prev[1]}+{label}', prev[2], '-', k, '-')); prev = None
        else: prev = (st, label, k)
    for label, vs, gs, ps, cs in progs:
        for x in (vs, gs, ps, cs):
            if x != '-': meta[x]['progs'] += 1
        if vs != '-': meta[vs]['next'].add(ps)
    with open(out / 'programs.tsv', 'w', newline='\n', encoding='utf-8') as fp:
        fp.write('label\tvs\tgs\tps\tcs\n' + ''.join('\t'.join(r) + '\n' for r in progs))
    with open(out / 'jobs.tsv', 'w', newline='\n', encoding='utf-8') as fp:
        fp.write('key\tstage\tcode_bytes\tctrl_bytes\tctl_key_bytes\tc1_bytes\txfb\tprograms\tnext\tsource\n')
        for k in sorted(meta):
            v = meta[k]
            fp.write(f"{k}\t{v['stage']}\t{v['code']}\t{v['ctrl']}\t{v['kl']}\t{v['c1']}\t{int(v['xfb'])}\t{v['progs']}\t{','.join(sorted(v['next'])) or '-'}\t{v['src']}\n")
    with open(out / 'shader_map.tsv', 'w', newline='\n', encoding='utf-8') as fp:
        fp.write('key\tstage\tfiles\n')
        for k in sorted(meta): fp.write(f"{k}\t{meta[k]['stage']}\t{' | '.join(where[k])}\n")
    dz = os.environ.get('DOS2_DEPTH_ZERO', '1') != '0'
    with open(out / 'shader_facts.tsv', 'w', newline='\n') as fp:
        fp.write('# DOS2 translation-query facts (tools/dos2_shader_jobs.py): key <TAB> facts JSON merged into the translator job;\n'
                 '# depth_zero: the game runs NVN_DEPTH_MODE_NEAR_IS_ZERO (main 0x938574), so vertex shaders keep z unchanged;\n'
                 '# local / shared: compute local size and shared bytes from the control section (+0x748 / +0x754)\n')
        for k in sorted(meta):
            f = dict(meta[k].get('facts') or {})
            if dz and meta[k]['stage'] in ('vs', 'gs'): f['depth_zero'] = True
            if f: fp.write(k + '\t' + json.dumps(f, separators=(',', ':')) + '\n')
    with open(out / 'xfb.tsv', 'w', newline='\n') as fp:
        fp.write('# vertex shaders whose control section declares transform feedback: key, buffer, stride, location:components...\n')
        for k in sorted(xfbs):
            for b, stride, o in xfbs[k]: fp.write(f'{k}\t{b}\t{stride}\t{" ".join(f"{l}:{c}" for l, c in o)}\n')
    sc = collections.Counter(v['stage'] for v in meta.values())
    lines = [f'source: {src} ({nfiles} .bshd files in {", ".join(PAKS)}; {len(empty)} without a program)',
             f'unique keys: {len(meta)} ({", ".join(f"{s} {n}" for s, n in sorted(sc.items()))}); problems: {len(bad)}',
             f'programs: {len(progs)}; keys in no program: {sum(not v["progs"] for v in meta.values())}',
             f'transform feedback: {" ".join(sorted(xfbs)) or "none"}',
             'depth_zero facts: ' + ('on' if dz else 'OFF (DOS2_DEPTH_ZERO=0)')] + \
            [f'  {n:5d}  {k}' for k, n in sorted(rep.items())] + [f'  PROBLEM {b}' for b in bad]
    (out / 'report.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    (out / 'empty.txt').write_text('\n'.join(empty) + '\n', encoding='utf-8')
    print('\n'.join(lines[:40]))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
