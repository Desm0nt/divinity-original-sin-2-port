"""Divinity: Original Sin 2 DE: native SPIR-V bundles compiled from the game's own NX GLSL sources (DM_GFX_NATIVE_SHADERS).

Every NVN stage is translated offline from Maxwell code (tools/dos2_shader_jobs.py -> dm/gfx/tools/dmsh_vk_xc2.py -> work/shaders-vk/<key>.xgs).
Next to 558 of the 6006 material .bshd files the packages also ship the material's GLSL source <material>_<pass>_NX.shd (GLSL 450 +
GL_NV_gpu_shader5, "[Vertex shader]" / "[Fragment shader]"). This tool compiles those stages with glslc and writes, for each stage whose
interface maps 1:1 onto the translated bundle of the same key, an .xgs with the translated bundle's header, binding table, vertex input
list and c1 bytes unchanged and only the SPIR-V replaced (descriptor bindings renumbered onto the bundle's table), validated with spirv-val.

Headers: the game ships no .shdh at all (no Shaders/ folder in any .pak). tools/dos2_nx_headers/ holds reconstructions:
  GlobalConstants_NX / _PS_NX  from the GLSLC reflection sections of the .bshd (block names, NVN bindings, offsets, types, row-major),
  NX/CommonHelpers (clip, GetTangentFrame) and NX/PBR (GBufferData, EncodeGBufferData)  from the translated Maxwell code,
  NX/ImageBasedLightingHelpers, Exposure, ClusteredLightingHelpers and PBR's forward-lighting helpers: not reconstructed, so the forward-lit
  stages that call them fail to compile and keep their translated bundles.
Pairing: a native vertex stage is written only when every program that uses its key also gets a native fragment stage (translated
fragment stages declare their inputs from the Maxwell attribute masks; mixing a narrower native VS output with them is not safe). A native
fragment stage needs its inputs among the outputs of every vertex stage it is paired with.

Output (default work/shaders-native): native/<key>.xgs, native.xgspack (dm/gfx/tools/xgs_pack.py format, one optional file to push as
<shaders>/native.xgspack), report.txt, stages.tsv (key, stage, status, reason, instruction counts translated / native).
Usage: dos2_native_shaders.py [--data romfs/Data] [--bundles work/shaders-vk] [--out work/shaders-native] [--only <material substring>]
"""
import argparse, collections, os, re, struct, subprocess, sys, tempfile, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from d3_shader_jobs import host_key
from tl2_shader_jobs import glslc_stage
from lspk import Pak

ROOT = HERE.parent
WORK = ROOT.parent / 'work'
NDK_TOOLS = Path(os.environ.get('DOS2_SHADER_TOOLS', str(Path(os.environ.get('ANDROID_HOME', '.')) / 'ndk/29.0.14206865/shader-tools/windows-x86_64')))
HEADERS = HERE / 'dos2_nx_headers'
PAKS = ('Engine', 'Materials', 'CommunityMods')
MAGIC = b'\x91\x68\x86\x19'
SAMPLER_SHIFT = 64          # sampler bindings are compiled at 64 + NVN texture binding so they never share a number with a block
SRC_UNIFORM, SRC_TEXTURE = 2, 7
VK_COMBINED, VK_UBO = 1, 6
GLSLC_STAGE = {0: 'vs', 1: 'ps'}   # GLSLC reflection stage index order: vertex, fragment, geometry, tess control, tess eval, compute


def tool(name):
    p = NDK_TOOLS / (name + '.exe')
    return str(p if p.exists() else NDK_TOOLS / name)


# ---------------------------------------------------------------------------------------------------------------------- .bshd side
def reflection(g):
    """GLSLC reflection section of one GLSLCoutput: {'blocks': [(name, words)], 'unif': ..., 'in': ..., 'out': ...} or None.
    Section header (36 u32 at 0x90 + i * 0x90): [0] size, [1] offset, [2] type (3 = reflection), [11..18] (count, offset) for uniform
    blocks / uniforms / inputs / outputs, [26] string pool offset; entries are 108 / 164 / 92 / 92 bytes, name = (pool offset, length).
    Block words after the name: [10] size, [11] active members, [12] stage mask, [13..18] binding per GLSLC stage.
    Uniform words: [10] type, [11] block, [12] offset, [13] array size, [14] array stride, [15] matrix stride, [16] row major,
    [17] stage mask, [18..23] binding per stage (samplers). Input / output words: [10] type, [11] ?, [12] location."""
    tot, _, ns = struct.unpack_from('<3I', g, 0x44)
    for i in range(ns):
        h = struct.unpack_from('<36I', g, 0x90 + i * 0x90)
        if h[2] != 3: continue
        s = g[h[1]:h[1] + h[0]]; sp = h[26]
        out = {}
        for name, (n, off, esz) in {'blocks': (h[11], h[12], 108), 'unif': (h[13], h[14], 164), 'in': (h[15], h[16], 92),
                                    'out': (h[17], h[18], 92)}.items():
            out[name] = []
            for k in range(n):
                w = struct.unpack_from('<%dI' % (esz // 4), s, off + k * esz)
                out[name].append((s[sp + w[0]:sp + w[0] + w[1]].decode('utf-8', 'replace'), w[10:]))
        return out
    return None


def bshd_programs(d):
    """[(stage, key, reflection)] of one .bshd"""
    h, = struct.unpack_from('>I', d, 0)
    if not h >> 31: return []
    u = zlib.decompress(d[4:])
    out, i, bad = [], 0, []
    while True:
        i = u.find(MAGIC, i)
        if i < 0: break
        tot, = struct.unpack_from('<I', u, i + 0x44)
        g = u[i:i + tot]
        r = glslc_stage(g, '?', bad)
        if r: out.append((r[0], host_key(r[1], r[2]), reflection(g)))
        i += max(tot, 4)
    return out


# ---------------------------------------------------------------------------------------------------------------------- source side
INC = re.compile(r'^\s*#include\s+"([^"]+)"\s*$', re.M)


def split_stages(text):
    parts = re.split(r'^\[(Vertex|Fragment) shader\]\s*$', text, flags=re.M)
    return {('vs' if parts[i] == 'Vertex' else 'ps'): parts[i + 1] for i in range(1, len(parts) - 1, 2)}


def preprocess(src, seen=None):
    """Inline the reconstructed headers, drop GL_NV_gpu_shader5 (the material code uses none of it), make the NVN-style GLSL Vulkan GLSL."""
    seen = set() if seen is None else seen
    def inc(m):
        p = m.group(1).lstrip('/')
        if p in seen: return ''
        seen.add(p)
        f = HEADERS / p
        if not f.exists(): return f'#error missing header {p}\n'
        return preprocess(f.read_text(), seen)
    s = INC.sub(inc, src)
    s = re.sub(r'^\s*#extension\s+GL_NV_gpu_shader5\s*:\s*\w+\s*$', '', s, flags=re.M)
    s = re.sub(r'layout\s*\(\s*location\s*=\s*\d+\s*\)\s*out\s+gl_PerVertex', 'out gl_PerVertex', s)
    s = re.sub(r'layout\s*\(\s*binding\s*=\s*(\d+)\s*\)\s*uniform\s+(sampler\w*|isampler\w*|usampler\w*)',
               lambda m: f'layout(binding = {int(m.group(1)) + SAMPLER_SHIFT}) uniform {m.group(2)}', s)
    return s


def compile_stage(text, stage, tmp):
    """(spirv bytes, error text)"""
    src = Path(tmp) / f'in.{"vert" if stage == "vs" else "frag"}'
    src.write_text(text)
    spv, opt = Path(tmp) / 'a.spv', Path(tmp) / 'b.spv'
    r = subprocess.run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-o', str(spv), str(src)], capture_output=True, text=True)
    if r.returncode: return None, (r.stderr or r.stdout).strip()
    r = subprocess.run([tool('spirv-opt'), '-O', '--remove-unused-interface-variables', str(spv), '-o', str(opt)], capture_output=True, text=True)
    if r.returncode: return None, 'spirv-opt: ' + (r.stderr or r.stdout).strip()
    return opt.read_bytes(), ''


# ---------------------------------------------------------------------------------------------------------------------- SPIR-V
class Spv:
    """Just enough SPIR-V: entry-point interface, decorations, types, descriptor variables; binding operands are rewritable."""
    def __init__(self, data):
        self.w = list(struct.unpack('<%dI' % (len(data) // 4), data))
        assert self.w[0] == 0x07230203
        self.dec = collections.defaultdict(dict); self.dec_at = collections.defaultdict(dict); self.mdec = collections.defaultdict(dict)
        self.types, self.vars, self.iface, self.count, self.consts = {}, {}, [], 0, {}
        i, infn = 5, False
        while i < len(self.w):
            n, op = self.w[i] >> 16, self.w[i] & 0xffff
            a = self.w[i + 1:i + n]
            if op == 15: self.iface = a[2 + (len(_str(a[2:])) // 4 + 1):]                         # OpEntryPoint
            elif op == 71:                                                                         # OpDecorate
                self.dec[a[0]][a[1]] = a[2:]; self.dec_at[a[0]][a[1]] = i + 3
            elif op == 72: self.mdec[(a[0], a[1])][a[2]] = a[3:]                                   # OpMemberDecorate
            elif 19 <= op <= 39: self.types[a[0]] = (op, a[1:])                                    # OpType*
            elif op == 43: self.consts[a[1]] = a[2]                                                # OpConstant
            elif op == 59: self.vars[a[1]] = (a[0], a[2])                                          # OpVariable: type, storage class
            if op == 54: infn = True                                                               # OpFunction .. OpFunctionEnd
            elif op == 56: infn = False
            elif infn and op not in (55, 248, 8, 317, 1):                                          # parameters, labels, lines
                self.count += 1
            i += n

    def pointee(self, v):
        t = self.vars[v][0]; return self.types[t][1][1]

    def shape(self, t):
        """(scalar kind 'f'/'i'/'u'/'b', components) of a variable type (arrays: element)"""
        op, a = self.types[t]
        if op == 28: return self.shape(a[0])                                          # array
        if op == 23: k, c = self.shape(a[0]); return k, c * a[1]                      # vector
        if op == 22: return 'f', 1
        if op == 21: return ('i' if a[1] else 'u'), 1
        if op == 20: return 'b', 1
        if op == 24: k, c = self.shape(a[0]); return k, c * a[1]                      # matrix
        return '?', 0

    def io(self, sc):
        """{location: (kind, components, interpolation)} of the interface variables of storage class sc (1 Input, 3 Output)"""
        out = {}
        for v in self.iface:
            if v not in self.vars or self.vars[v][1] != sc: continue
            d = self.dec.get(v, {})
            if 11 in d: continue                                                      # BuiltIn
            if 30 not in d:
                if self.types[self.pointee(v)][0] == 30: continue                     # gl_PerVertex block
                raise ValueError(f'interface variable %{v} without a location')
            k, c = self.shape(self.pointee(v))
            interp = ''.join(x for x, dd in (('flat', 14), ('noperspective', 13), ('centroid', 16), ('sample', 17)) if dd in d)
            out[d[30][0]] = (k, c, interp)
        return out

    def builtins(self, sc):
        out = set()
        for v in self.iface:
            if v not in self.vars or self.vars[v][1] != sc: continue
            d = self.dec.get(v, {})
            if 11 in d: out.add(d[11][0]); continue
            t = self.pointee(v)
            if self.types[t][0] == 30:
                for (s, m), md in self.mdec.items():
                    if s == t and 11 in md: out.add(md[11][0])
        return out

    def descriptors(self):
        """[(var, kind 'ubo'/'tex'/'?', set, binding, dim)]"""
        out = []
        for v, (t, sc) in self.vars.items():
            if sc not in (0, 2, 12): continue                                         # UniformConstant, Uniform, StorageBuffer
            d = self.dec.get(v, {})
            if 33 not in d: continue
            pt = self.types[t][1][1]
            while self.types[pt][0] == 28: pt = self.types[pt][1][0]
            op, a = self.types[pt]
            if sc == 2 and op == 30 and 2 in self.dec.get(pt, {}): kind, dim = 'ubo', None   # Block
            elif op == 27: kind, dim = 'tex', self.types[a[0]][1][1]                        # OpTypeSampledImage -> image Dim
            else: kind, dim = '?', None
            out.append((v, kind, d.get(34, [0])[0], d[33][0], dim))
        return out

    def set_binding(self, v, binding):
        self.w[self.dec_at[v][33]] = binding
        if 34 in self.dec_at[v]: self.w[self.dec_at[v][34]] = 0

    def bytes(self):
        return struct.pack('<%dI' % len(self.w), *self.w)


def _str(words):
    b = b''.join(struct.pack('<I', x) for x in words)
    return b[:b.index(b'\0')]


# ---------------------------------------------------------------------------------------------------------------------- bundles
def read_bundle(p):
    b = Path(p).read_bytes()
    magic, ver, stage, nb, ni, c1, sp, _ = struct.unpack_from('<8I', b, 0)
    assert magic == 0x31534758 and ver == 1, p
    o = 32
    binds = [list(struct.unpack_from('<8I', b, o + i * 32)) for i in range(nb)]; o += nb * 32
    ins = list(struct.unpack_from('<%dI' % ni, b, o)); o += ni * 4
    return {'raw': b, 'stage': stage, 'bindings': binds, 'inputs': ins, 'head': b[:o + c1], 'spirv': b[o + c1:o + c1 + sp]}


def write_bundle(tr, spirv):
    """translated header / bindings / inputs / c1, native SPIR-V"""
    h = bytearray(tr['head']); struct.pack_into('<I', h, 24, len(spirv))
    return bytes(h) + spirv


def instr_count(data):
    return Spv(data).count


def opt_count(data, tmp):
    """instructions of the translated SPIR-V after the same spirv-opt -O the native one gets (fair comparison)"""
    a, b = Path(tmp) / 't.spv', Path(tmp) / 'to.spv'
    a.write_bytes(data)
    r = subprocess.run([tool('spirv-opt'), '-O', str(a), '-o', str(b)], capture_output=True)
    return instr_count(b.read_bytes()) if not r.returncode else instr_count(data)


# ---------------------------------------------------------------------------------------------------------------------- mapping
def map_stage(nat, tr, stage):
    """Renumber the native descriptors onto the translated bundle's table. '' = ok, else the reason."""
    table = {}
    for b in tr['bindings']:
        binding, typ, count, size, source, slot, samp, flags = b
        if source == SRC_UNIFORM and typ == VK_UBO: table[('ubo', slot)] = b
        elif source == SRC_TEXTURE and typ == VK_COMBINED: table[('tex', slot)] = b
    for v, kind, dset, binding, dim in nat.descriptors():
        if kind == '?': return f'native descriptor %{v} of an unsupported kind'
        nvn = binding - SAMPLER_SHIFT if kind == 'tex' else binding
        b = table.get((kind, nvn))
        if b is None: return f'native {kind} NVN binding {nvn} has no entry in the translated table'
        if kind == 'tex':
            tdim = (b[7] >> 8) & 7 if b[7] >> 31 else None
            want = {1: 1, 3: 3, 2: 2}.get(dim)        # SPIR-V Dim 2D=1, 3D=2, Cube=3; bundle dim bits: see xbg_game.h flags
            if tdim is not None and tdim != want: return f'texture {nvn}: native SPIR-V Dim {dim}, translated shape dim {tdim}'
        nat.set_binding(v, b[0])
    if stage == 'vs':
        ins = nat.io(1)
        if sorted(ins) != sorted(tr['inputs']): return f'vertex inputs native {sorted(ins)} translated {sorted(tr["inputs"])}'
    return ''


def check_io(nat, trs, stage):
    """translated-vs-native interface facts (strings) for the report"""
    tr = Spv(trs)
    notes = []
    if stage == 'vs':
        n, t = nat.io(3), tr.io(3)
        if set(n) != set(t): notes.append(f'outputs native {sorted(n)} translated {sorted(t)}')
        for loc in set(n) & set(t):
            if n[loc][1] != t[loc][1] or n[loc][2] != t[loc][2]: notes.append(f'out {loc}: native {n[loc]} translated {t[loc]}')
        ni, ti = nat.io(1), tr.io(1)
        for loc in set(ni) & set(ti):
            if ni[loc][0] != ti[loc][0]: notes.append(f'in {loc}: native {ni[loc][0]}{ni[loc][1]} translated {ti[loc][0]}{ti[loc][1]}')
    else:
        n, t = nat.io(3), tr.io(3)
        if set(n) != set(t): notes.append(f'colour outputs native {sorted(n)} translated {sorted(t)}')
        ni, ti = nat.io(1), tr.io(1)
        if not set(ni) <= set(ti): notes.append(f'inputs native {sorted(ni)} translated {sorted(ti)}')
        for loc in set(ni) & set(ti):
            if ni[loc][2] != ti[loc][2]: notes.append(f'in {loc} interpolation native "{ni[loc][2]}" translated "{ti[loc][2]}"')
    bn, bt = nat.builtins(3), tr.builtins(3)
    if bn - bt: notes.append(f'builtins written only natively {sorted(bn - bt)}')
    return notes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--data', default=str(WORK / 'romfs' / 'Data'))
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--out', default=str(WORK / 'shaders-native'))
    ap.add_argument('--only', default='')
    ap.add_argument('--keep-glsl', action='store_true', help='write the preprocessed GLSL of every stage to <out>/glsl')
    a = ap.parse_args()
    data, bundles, out = Path(a.data), Path(a.bundles), Path(a.out)
    (out / 'native').mkdir(parents=True, exist_ok=True)
    for f in (out / 'native').glob('*.xgs'): f.unlink()
    if a.keep_glsl: (out / 'glsl').mkdir(exist_ok=True)

    # every program of every .bshd (also those without a source: pairing needs them), and the sources
    progs, sources = [], {}
    for pk in PAKS:
        pak = Pak(data / f'{pk}.pak')
        names = {e[0]: e for e in pak.entries}
        for n, e in names.items():
            if not n.endswith('.bshd'): continue
            sts = bshd_programs(pak.read(e))
            if not sts: continue
            label = f'{pk}:{n}'
            progs.append((label, {st: (k, r) for st, k, r in sts}))
            sn = n[:-5] + '.shd'
            if n.endswith('_NX.bshd') and sn in names and a.only in n: sources[label] = pak.read(names[sn]).decode('utf-8', 'replace')

    status, rows, nat_spv, err_kinds = {}, [], {}, collections.Counter()
    with tempfile.TemporaryDirectory() as tmp:
        for label, stages in progs:
            if label not in sources: continue
            src = split_stages(sources[label])
            for st, (key, refl) in stages.items():
                if key in status: continue
                if st not in src: status[key] = ('nosrc', 'no source section'); continue
                trp = bundles / f'{key}.xgs'
                if not trp.exists(): status[key] = ('notr', 'no translated bundle'); continue
                text = preprocess(src[st])
                if a.keep_glsl: (out / 'glsl' / f'{key}.{st}.glsl').write_text(text)
                spv, err = compile_stage(text, st, tmp)
                if spv is None:
                    m = re.search(r"'([^']+)' : (undeclared identifier|no matching overloaded function found)", err)
                    why = f'{m.group(2)} {m.group(1)}' if m else err.splitlines()[0][:160] if err else 'glslc failed'
                    err_kinds[m.group(1) if m else why] += 1
                    status[key] = ('compile', why); continue
                tr = read_bundle(trp)
                nat = Spv(spv)
                why = map_stage(nat, tr, st)
                if why: status[key] = ('map', why); continue
                notes = check_io(nat, tr['spirv'], st)
                final = nat.bytes()
                (Path(tmp) / 'v.spv').write_bytes(final)
                r = subprocess.run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(Path(tmp) / 'v.spv')], capture_output=True, text=True)
                if r.returncode: status[key] = ('val', (r.stderr or r.stdout).strip().splitlines()[0][:160]); continue
                nat_spv[key] = (st, tr, final, notes)
                status[key] = ('ok', '; '.join(notes))

        # pairing over all programs: a native VS needs a native FS in every program with it; a native FS needs its inputs written by
        # every VS it meets (translated VS outputs: their SPIR-V; native ones: the native SPIR-V)
        drop = {}
        def translated_outputs(k):
            p = bundles / f'{k}.xgs'
            return Spv(read_bundle(p)['spirv']).io(3) if p.exists() else {}
        def outputs(k):
            return Spv(nat_spv[k][2]).io(3) if k in nat_spv else translated_outputs(k)
        changed = True
        while changed:
            changed = False
            for label, stages in progs:
                vs, ps = stages.get('vs', (None,))[0], stages.get('ps', (None,))[0]
                if vs in nat_spv and vs not in drop and (ps not in nat_spv or ps in drop):
                    drop[vs] = f'paired with translated fragment {ps} ({label})'; changed = True
                if ps in nat_spv and ps not in drop and vs:
                    # the native VS (when kept) and the translated VS (the host falls back to it per stage, e.g. a stale native bundle)
                    # must both write every input of the native FS
                    ins = Spv(nat_spv[ps][2]).io(1)
                    for outs in ([outputs(vs)] if vs in nat_spv and vs not in drop else []) + [translated_outputs(vs)]:
                        miss = [l for l in ins if l not in outs or outs[l][1] < ins[l][1]]
                        if miss: drop[ps] = f'inputs {miss} not written by vertex {vs} ({label})'; changed = True; break
        for k, why in drop.items(): status[k] = ('pair', why); del nat_spv[k]

        cnt = collections.Counter(); gain = collections.defaultdict(lambda: [0, 0, 0])
        for k, (st, tr, final, notes) in sorted(nat_spv.items()):
            (out / 'native' / f'{k}.xgs').write_bytes(write_bundle(tr, final))
            t_raw, t_opt, n = instr_count(tr['spirv']), opt_count(tr['spirv'], tmp), instr_count(final)
            g = gain[st]; g[0] += t_raw; g[1] += t_opt; g[2] += n
            rows.append((k, st, 'ok', '; '.join(notes), t_raw, t_opt, n))
        for k, (s, why) in sorted(status.items()):
            cnt[(s, next((st for _, sts in progs for st, (kk, _) in sts.items() if kk == k), '?'))] += 1
            if s != 'ok': rows.append((k, '?', s, why, '', '', ''))

    with open(out / 'stages.tsv', 'w', encoding='utf-8') as f:
        f.write('key\tstage\tstatus\treason_or_notes\ttranslated_instr\ttranslated_opt_instr\tnative_instr\n')
        for r in rows: f.write('\t'.join(map(str, r)) + '\n')
    rep = [f'sources: {len(sources)} .bshd with an _NX.shd; stages considered: {len(status)}']
    for s in ('ok', 'compile', 'map', 'val', 'pair', 'nosrc', 'notr'):
        rep.append(f'{s}: ' + ', '.join(f'{st} {n}' for (ss, st), n in sorted(cnt.items()) if ss == s))
    rep.append('compile failures by first missing symbol: ' + ', '.join(f'{k} {v}' for k, v in err_kinds.most_common(12)))
    for st, (t_raw, t_opt, n) in sorted(gain.items()):
        rep.append(f'{st}: instructions translated {t_raw} (after spirv-opt -O {t_opt}) native {n} = {n / max(t_opt, 1):.2f}x of optimised translated')
    pack = out / 'native.xgspack'
    xp = ROOT / 'dm' / 'gfx' / 'tools' / 'xgs_pack.py'
    if nat_spv and xp.exists():
        r = subprocess.run([sys.executable, str(xp), str(out / 'native'), str(pack)], capture_output=True, text=True)
        rep.append(f'pack: {pack} ({pack.stat().st_size if pack.exists() else 0} bytes) {r.stdout.strip()[-200:]} {r.stderr.strip()[-200:]}')
    (out / 'report.txt').write_text('\n'.join(rep) + '\n')
    print('\n'.join(rep))


if __name__ == '__main__':
    main()
