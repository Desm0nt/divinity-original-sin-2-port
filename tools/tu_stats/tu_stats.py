"""N1 (docs/dos2/nr/NR-NSHADER.md): Adreno 750 compiler statistics offline, from the real Turnip compiler (host build, fake kgsl).

  tu_stats.py run <jobs.tsv> [--out stats.tsv]        jobs: name <tab> vs <tab> fs [<tab> colour VkFormat numbers, space separated]
                                                      vs / fs = a 16-hex key of work/shaders-vk (translated bundle), a path to a .spv
                                                      or to an .xgs bundle; fs '-' = vertex only
  tu_stats.py build                                   (re)build tu_stats + fakekgsl.so in WSL (needs the host Turnip, see below)
Environment / defaults: WSL distro Ubuntu-22.04; driver $HOME/n1mesa-build/src/freedreno/vulkan/libvulkan_freedreno.so (the turnip27
Mesa tree mesa-full built for x86_64 Linux, kgsl kmd: scratchpad recipe host_turnip.sh in the doc); IR3_SHADER_DEBUG etc. pass through.
Output TSV: name, stage, then the driver's statistics (Instruction_Count, Max_Waves_Per_Core, Registers_used, NOPs_Count, ...).
"""
import argparse, os, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
WORK = Path(os.environ.get('DOS2_WORK', str(HERE.parents[2] / 'work')))
DISTRO = os.environ.get('TU_STATS_DISTRO', 'Ubuntu-22.04')
DRIVER = os.environ.get('TU_STATS_DRIVER', '$HOME/n1mesa-build/src/freedreno/vulkan/libvulkan_freedreno.so')
TOOLS = os.environ.get('TU_STATS_TOOLS', '$HOME/n1tools')


def wsl_path(p):
    p = str(Path(p).resolve()).replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:] if len(p) > 1 and p[1] == ':' else p


def pack_entry(path, key):
    import struct
    d = Path(path).read_bytes(); m, v, n, _ = struct.unpack_from('<4I', d, 0)
    for i in range(n):
        k, o, sz, _, _ = struct.unpack_from('<32sQIIQ', d, 16 + i * 56)
        if k.rstrip(b'\0').decode() == key: return d[o:o + sz]
    raise SystemExit('%s: no key %s' % (path, key))


def bundle_spirv(b):
    """SPIR-V of an XGS1 bundle or of the XGS1 bundle inside an XGF1 (fused, dm/gfx/nb/nb_fused.h) one"""
    import struct
    o = b.find(struct.pack('<I', 0x31534758))
    if o < 0: raise SystemExit('no XGS1 bundle')
    size = struct.unpack_from('<8I', b, o)[6]
    s = b.find(struct.pack('<I', 0x07230203), o)
    return b[s:s + size]


def spirv_of(ref, tmp, tag):
    """ref -> path of a .spv file in tmp. ref: 16-hex key of work/shaders-vk, a .spv, an .xgs / XGF1 file, or pack:<xgspack>:<key>"""
    from dos2_native_shaders import read_bundle
    if ref.startswith('pack:'):
        path, key = ref[5:].rsplit(':', 1)
        out = Path(tmp) / (tag + '.spv'); out.write_bytes(bundle_spirv(pack_entry(path, key))); return out
    p = Path(ref)
    if len(ref) == 16 and all(c in '0123456789abcdef' for c in ref):
        p = WORK / 'shaders-vk' / (ref + '.xgs')
    if p.suffix == '.spv':
        return p
    b = read_bundle(p)
    out = Path(tmp) / (tag + '.spv'); out.write_bytes(b['spirv']); return out


def run(jobs, out=None, env=None):
    rows = []
    with tempfile.TemporaryDirectory(dir=os.environ.get('TU_STATS_TMP')) as tmp:
        man = []
        for i, j in enumerate(jobs):
            name, vs, fs = j[0], j[1], j[2]
            fmts = j[3] if len(j) > 3 else ''
            v = spirv_of(vs, tmp, 'v%d' % i)
            f = '-' if fs == '-' else wsl_path(spirv_of(fs, tmp, 'f%d' % i))
            man.append('%s %s %s %s' % (name, wsl_path(v), f, fmts))
        (Path(tmp) / 'manifest.txt').write_text('\n'.join(man) + '\n')
        envs = ' '.join('%s=%s' % kv for kv in (env or {}).items())
        cmd = 'MESA_SHADER_CACHE_DISABLE=1 TU_DEBUG=${TU_DEBUG:-} %s LD_PRELOAD=%s/fakekgsl.so %s/tu_stats %s %s' % (
            envs, TOOLS, TOOLS, DRIVER, wsl_path(Path(tmp) / 'manifest.txt'))
        r = subprocess.run(['wsl', '-d', DISTRO, '-e', 'bash', '-c', cmd], capture_output=True, text=True,
                           env=dict(os.environ, MSYS_NO_PATHCONV='1'))
        if r.returncode:
            sys.stderr.write(r.stderr[-4000:]); raise SystemExit('tu_stats failed (%d)' % r.returncode)
        for line in r.stdout.splitlines():
            if line.startswith('ERR '):
                rows.append({'name': line.split()[1], 'stage': 'ERR', 'err': line}); continue
            f = line.split('\t')
            if len(f) < 2: continue
            d = {'name': f[0], 'stage': f[1]}
            for kv in f[2:]:
                k, _, v = kv.partition('='); d[k] = v
            rows.append(d)
        if env and env.get('IR3_SHADER_DEBUG'):
            sys.stderr.write(r.stderr)
    if out:
        keys = []
        for d in rows:
            for k in d:
                if k not in keys: keys.append(k)
        with open(out, 'w') as fo:
            fo.write('\t'.join(keys) + '\n')
            for d in rows: fo.write('\t'.join(str(d.get(k, '')) for k in keys) + '\n')
    return rows


def threads(d):
    try: return int(d['Max_Waves_Per_Core']) * int(d['Subgroup_size'])
    except (KeyError, ValueError): return '?'


def selftest():
    """the translated PPFXAA (691768aa08545d95) with its post VS (28b24f7288e2abe4): both stages compile, FS statistics present"""
    rows = run([['selftest', '28b24f7288e2abe4', '691768aa08545d95', '37']])
    fs = [d for d in rows if d['stage'] == 'FS']
    ok = len(fs) == 1 and int(fs[0].get('Instruction_Count', 0)) > 100 and int(fs[0].get('Subgroup_size', 0)) in (64, 128)
    print('tu_stats selftest %s: %s' % ('PASS' if ok else 'FAIL', fs[0] if fs else rows))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['run', 'selftest'])
    ap.add_argument('jobs', nargs='?')
    ap.add_argument('--out')
    a = ap.parse_args()
    if a.cmd == 'selftest': sys.exit(selftest())
    jobs = [l.rstrip('\n').split('\t') for l in open(a.jobs) if l.strip() and not l.startswith('#')]
    rows = run(jobs, a.out)
    for d in rows:
        if d['stage'] == 'ERR': print(d['err']); continue
        print('%-40s %-9s inst %-5s regs %-3s half %-3s waves %-3s x %-3s = threads %-5s nops %-4s sy %-3s ss %-3s' % (
            d['name'], d['stage'][:9], d.get('Instruction_Count', '?'), d.get('Registers_used', '?'), d.get('Half-registers_used', '?'),
            d.get('Max_Waves_Per_Core', '?'), d.get('Subgroup_size', '?'), threads(d), d.get('NOPs_Count', '?'),
            d.get('Instructions_with_SY_sync_bit', '?'), d.get('Instructions_with_SS_sync_bit', '?')))


if __name__ == '__main__':
    main()
