"""DOS2 VK-NATIVE P10 (docs/dos2/nr/VK-P10.md): input-attachment twins of the fragment modules of the merged tile pass
G-buffer -> LinearizeDepth -> CascadedPCF -> fused lighting, built into the backend (dm/android/shaders/tile_*.frag.spv.inc +
tile_twins.inc: no setup run), each proven equal to the module it replaces, bit for bit.

A twin = the same GLSL as the module it replaces, compiled with -DOC=<mask>: the textures the chain itself produces become subpassInput
(read by subpassLoad at the fragment's own pixel), the render-scale slow path is compiled out (valid only at render scale 1). Without OC
every source compiles to the bytes it compiled to before (checked against the pack with --pack).
    tile_linz                    LinearizeDepth 'x' (OC=1): tDepth (binding 10, index 0); equals texture(tDepth, TexCoord0.xy) only at the
                                 own pixel's centre with a NEAREST sampler (the host proves it: VKN_TILE_C_CENTER)
    tile_pcf_<N>x<S>_<x|m|d>     CascadedPCF_<N>x<S>, pack variants x / m / d (OC=1): LinearDepth (binding 10, index 0); the atlas a sampler
    tile_light_<f|g>             the fused light of the pack ('f' Sky inside Light, 'g' + DeferredFog on output 1), OC=0xFBF: the Light
                                 part's tG0 tNormal tDepth tEmissive tBase tAniso tShadow (bindings 10 16 22 28 34 40 58, indices 0..5, 7) and
                                 the Sky part's own lfG0 lfDepth lfNormal lfBase (bindings 200 206 212 224, indices 8..11): 11 inputs
    tile_light_<f|g>s            the same with OC=0x10BF: the Sky part reads the Light part's four inputs (7 inputs; valid only when
                                 bindings 200 / 206 / 212 / 224 hold the images of bindings 10 / 22 / 16 / 34: the host checks)
    tile_light_lcap<N>[t]_<f|g>[s]   the same two forms of the built-in capped fused modules (DM_VK_NATIVE_LCAP=2, tools/dos2_light_cap_fused.py)
Proof (tools/spv_interp.py, OpImageRead of a subpass input = the attachment's texel at the fragment's own pixel): on the random invocations
of the module's own generator at render scale 1 the twin's outputs equal the replaced module's bit for bit; the modelled read moved one
pixel must be seen (mutation); spirv-val; the twin's inputs / outputs / descriptors against the replaced module's; the subpass bindings
are exactly the table. The backend (vkn_shlib_named.cpp vkn_shlib_named_tile) hands a twin out only for the exact module it was proven
against (FNV-1a 64 of that module's SPIR-V, written into tile_twins.inc).
Usage: dos2_tile_shaders.py --bundles <dos2-port>/work/shaders-vk [--pack <native-nr.xgspack>] [--n 200] [--check]
    --check: build and prove, write nothing; fail when the files on disk differ from the build
"""
import argparse, struct, sys, tempfile
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'dm' / 'gfx' / 'tools'))
from dos2_native_shaders import Spv, read_bundle, tool, WORK
from dos2_pcf_mobile import run
import dos2_pcf_mobile as PCF
import dos2_post_mobile as PM
import dos2_light_mobile as LM
import dos2_light_cap as LC
import dos2_light_cap_fused as LCF
import dos2_lightfuse as LF
import dos2_onchip_shaders as OCS
import spv_interp as SI

OUT = HERE.parent / 'dm' / 'android' / 'shaders'
LINZ_KEY = 'f23c09d198b55dc5'
C_SCALE1, C_CENTER, C_SHARE = 1, 2, 4            # vkn_shlib_named.h VKN_TILE_C_*
LIGHT_IN = {10: 0, 16: 1, 22: 2, 28: 3, 34: 4, 40: 5, 58: 7}
SKY_IN = {200: 8, 206: 9, 212: 10, 224: 11}
SHARE = {200: 10, 206: 22, 212: 16, 224: 34}    # Sky binding -> the Light binding the 's' form reads instead
OC_OWN, OC_SHARE = 0xFBF, 0x10BF


def fnv64(b):
    x = 0xcbf29ce484222325
    for c in b: x = ((x ^ c) * 0x100000001b3) & 0xffffffffffffffff
    return x


def glslc(src, out, defines, inc=None):
    cmd = [tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', *defines]
    if inc: cmd += ['-I', str(inc)]
    run(cmd + ['-o', str(out), str(src)]); run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(out)])
    return Path(out).read_bytes()


def inc_words(path):
    """the words of a .spv.inc (first line = a comment)"""
    return [int(x, 16) for l in Path(path).read_text().splitlines()[1:] for x in l.replace(',', ' ').split()]


def inc_text(data, head):
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    return '\n'.join([head] + [', '.join('0x%08x' % x for x in w[i:i + 8]) + ',' for i in range(0, len(w), 8)]) + '\n'


def bindings(spv): return set(LF.descriptors(spv))


# ------------------------------------------------------------------------------------------------------------------ generators
def linz_case(rng, centre=True):
    """LinearizeDepth with the texture read modelled at the own pixel's centre: a W x H depth texture with a NEAREST sampler and
    TexCoord0.xy = (FragCoord.xy / size) as float32. centre False (control): the generator's free coordinate and a linear sampler."""
    inp, ub, tx = PM.case('LinearizeDepth', rng, False)
    if centre:
        fc = inp['FragCoord']
        inp = dict(inp); inp[1] = (float(np.float32(fc[0] / PM.W)), float(np.float32(fc[1] / PM.H)), 0.0, 1.0)
        tx = {10: SI.Texture(tx[10].d, 'nearest')}
    return inp, ub, tx


def fused_case(rng, it, fog, capped, ref):
    """the invocation of tools/dos2_lightfuse.py prove for the fused module (Sky's resources on the extras; its G-buffer textures are the
    Light part's, as in the frame); capped: the cluster lists of dos2_light_cap_fused.py; ref: sun and ambient on (the threshold cap)"""
    scen = None if it % 4 else LM.SCENARIOS[(it // 4) % len(LM.SCENARIOS)][1]
    inp, ub, tx = (LCF.case_many if capped else LCF._case)('ClusteredLightingWithSSAO', rng, False, scen)
    if ref and it % 2: ub, tx = LC.with_ref(rng, ub, tx)
    if fog:
        c16 = bytearray(ub[82]); z0 = float(rng.uniform(0, 40)); w0 = float(rng.uniform(0, 30))
        for r, (z, w) in enumerate(((z0, w0), (z0 + float(rng.uniform(1, 80)), w0 + float(rng.uniform(1, 60))),
                                    (float(rng.uniform(-5, 20)), float(rng.choice([-1, 1]) * rng.uniform(0.5, 30))))):
            struct.pack_into('<4f', c16, 16 * (4 + r), float(rng.uniform(0, 2)), float(rng.uniform(0, 2)), z, w)
        ub = dict(ub); ub[82] = bytes(c16)
    cube = lambda: SI.CubeTexture(rng.uniform(0, 3, (6, 4, 4, 4)).astype(np.float32))
    rot = LM.rotation(rng)
    stx = {10: tx[10], 16: tx[22], 22: tx[16], 28: cube(), 34: tx[34], 40: tx[46], 46: SI.Texture(LM.table(rng, shape=(8, 8))), 52: cube()}
    sub = {64: PM.cbuf({i: tuple(rot[i]) + (0.0,) for i in range(3)}), 70: PM.cbuf({2: (0.0, float(rng.uniform(0, 8)), float(rng.uniform(0, 2)), 0.0)})}
    ftx = dict(tx); fub = dict(ub)
    for b, (sb, kind) in LF.EXTRAS.items():
        if kind == 'tex': ftx[b] = stx[sb]
        else: fub[b] = sub[sb]
    return inp, fub, ftx


def prove(name, base, twin, case, same, n, seed, extra=None):
    """(lines, failures): twin == base on n invocations, the mutation, optional third module (extra = (label, spv))"""
    mb, mt = SI.Module(base), SI.Module(twin); me = SI.Module(extra[1]) if extra else None
    rng = np.random.default_rng(seed); bad = ebad = 0; reads = 0; lines = []
    for it in range(n):
        inp, ub, tx = case(rng, it)
        rb, rt = mb.run(inp, ub, tx), mt.run(inp, ub, tx); reads += mt.subpass_reads
        if not same(rb, rt):
            bad += 1
            if bad <= 2: lines.append('  MISMATCH %s #%d base=%s twin=%s' % (name, it, rb, rt))
        if me is not None and not same(me.run(inp, ub, tx), rt): ebad += 1
    orig = mt._subpass_read
    def shifted(a, g):
        fc = mt.fragcoord; mt.fragcoord = (fc[0] + 1.0,) + tuple(fc[1:])
        try: return orig(a, g)
        finally: mt.fragcoord = fc
    mt._subpass_read = shifted
    rng = np.random.default_rng(seed + 77); caught = 0; nm = min(n, 60)
    for it in range(nm):
        inp, ub, tx = case(rng, it)
        if not same(mb.run(inp, ub, tx), mt.run(inp, ub, tx)): caught += 1
    lines.insert(0, '%s: twin == the module it replaces, bit for bit: %d / %d%s; input attachment reads %.2f / invocation; mutation (read one '
                 'pixel off) caught in %d / %d' % (name, n - bad, n, '; == %s: %d / %d' % (extra[0], n - ebad, n) if extra else '', reads / max(n, 1), caught, nm))
    return lines, bad + ebad + (0 if caught else 1)


def iface(name, base, twin, want, lose_in=None):
    """interface of the twin against the module it replaces; returns failure lines"""
    b, t = Spv(base), Spv(twin); out = []
    bi, ti = dict(b.io(1)), dict(t.io(1))
    if lose_in is not None and lose_in not in ti: bi.pop(lose_in, None)   # (the coordinate the twin no longer reads)
    if bi != ti: out.append('  INTERFACE %s: inputs %s, the replaced module %s' % (name, ti, bi))
    if b.io(3) != t.io(3): out.append('  INTERFACE %s: outputs %s, the replaced module %s' % (name, t.io(3), b.io(3)))
    if not bindings(twin) <= bindings(base): out.append('  DESCRIPTORS %s: %s not in the replaced module' % (name, sorted(bindings(twin) - bindings(base))))
    sp = OCS.subpass_bindings(twin)
    if sp != want: out.append('  SUBPASS %s: %s, expected %s' % (name, sp, want))
    if len(set(sp.values())) != len(sp) or any(i >= 16 for i in sp.values()): out.append('  SUBPASS %s: indices not unique / >= 16' % name)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--pack', default='', help='a native-nr.xgspack: its bundles must carry the SPIR-V the twins were proven against')
    ap.add_argument('--n', type=int, default=200)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    B = Path(a.bundles); fails = 0; out = []; rows = []; files = {}
    c1x, c1y = LF.sky_c1((B / (LF.SKY_KEY + '.xgs')).read_bytes())
    pack = {}
    if a.pack:
        from dos2_nr_sh_pack import pack_read, spirv_of
        import dos2_nr_sh_pack as NRP
        names = ['%s.%s' % (v, e[0]) for e in NRP.ENTRIES for v in e[3] + 'fg']
        pack = {k: spirv_of(b) for k, b in pack_read(a.pack, set(names)).items()}

    def add(inc, ename, variant, base, twin, want, cond, what, defs, center=0, c1=(0, 0), pk=None):
        nonlocal fails
        if pk and pack:
            if pk not in pack: out.append('  PACK %s: not in %s' % (pk, a.pack)); fails += 1
            elif pack[pk] != base: out.append('  PACK %s: the pack carries another SPIR-V (%d bytes, ours %d)' % (pk, len(pack[pk]), len(base))); fails += 1
            else: out.append('  %s: the pack bundle %s carries exactly the proven module (%d bytes)' % (inc, pk, len(base)))
        order = sorted(want)
        rows.append((inc, ename, variant, fnv64(base), len(base) // 4, order, [want[b] for b in order], cond, center, c1, what))
        files[inc + '.frag.spv.inc'] = inc_text(twin, '// [DOS2] P10 tile twin (input attachments, render scale 1) of %s: %s (glslc --target-env=vulkan1.1 -O); replaces the module '
                                                'of FNV-1a 64 %016x (%d words); made and proven by tools/dos2_tile_shaders.py' % (what, defs, fnv64(base), len(base) // 4))
        out.append('  built %s: %d words; inputs (binding: index) %s' % (inc, len(twin) // 4, ' '.join('%d:%d' % (b, want[b]) for b in order)))

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # ---- LinearizeDepth
        src = PM.SRC / 'LinearizeDepth_mobile.frag'
        base = glslc(src, tmp / 'lz.spv', [], PM.SRC); twin = glslc(src, tmp / 'lzt.spv', ['-DOC=1'], PM.SRC)
        tr = read_bundle(B / (LINZ_KEY + '.xgs')); (tmp / 'tr.spv').write_bytes(tr['spirv'])
        run([tool('spirv-opt'), '-O', str(tmp / 'tr.spv'), '-o', str(tmp / 'tro.spv')])
        lines, f = prove('tile_linz (LinearizeDepth, coordinate = the own pixel centre, NEAREST)', base, twin, lambda rng, it: linz_case(rng), PM.same, a.n, 0x1001,
                         ('translated (spirv-opt -O)', (tmp / 'tro.spv').read_bytes()))
        out += lines; fails += f
        mb, mt = SI.Module(base), SI.Module(twin); rng = np.random.default_rng(0x1002); d = 0
        for it in range(60):
            inp, ub, tx = linz_case(rng, False)
            if not PM.same(mb.run(inp, ub, tx), mt.run(inp, ub, tx)): d += 1
        out.append('  control (free coordinate, linear sampler: the condition VKN_TILE_C_CENTER not met): differs in %d / 60' % d); fails += 0 if d else 1
        bad = iface('tile_linz', base, twin, {10: 0}, lose_in=1); out += bad; fails += len(bad)
        keeps = 1 in dict(Spv(twin).io(1))
        out.append('  tile_linz: fragment input location 1 %s' % ('kept in the interface' if keeps else 'is not declared by the twin (the coordinate is not read)'))
        add('tile_linz', 'LinearizeDepth', '0', base, twin, {10: 0}, C_SCALE1 | C_CENTER, 'LinearizeDepth_mobile.frag (pack x.LinearizeDepth)', '-DOC=1', center=1, pk='x.LinearizeDepth')
        # ---- CascadedPCF
        for key, (ename, nc, atlas) in PCF.KEYS.items():
            if not (B / (key + '.xgs')).is_file(): out.append('  %s: no translated bundle %s' % (ename, key)); fails += 1; continue
            raw = (B / (key + '.xgs')).read_bytes(); trp = read_bundle(B / (key + '.xgs'))
            c1 = trp['head'][len(trp['head']) - struct.unpack_from('<I', raw, 20)[0]:]
            for v, mode in (('x', 0), ('m', 1), ('d', 3)):
                d0 = ['-DNCASC=%d' % nc, '-DATLAS=%d.0' % atlas, '-DPCF_MODE=%d' % mode]
                base = glslc(PCF.SRC, tmp / 'p.spv', d0); twin = glslc(PCF.SRC, tmp / 'pt.spv', d0 + ['-DOC=1'])
                inc = 'tile_pcf_%s_%s' % (ename.split('_')[1], v)
                lines, f = prove('%s (%s, PCF_MODE=%d)' % (inc, ename, mode), base, twin, lambda rng, it, key=key, c1=c1: PCF.case(key, rng, False, c1), PM.same, a.n,
                                 0x2000 + int(key[:4], 16) + mode)
                out += lines; fails += f
                bad = iface(inc, base, twin, {10: 0}); out += bad; fails += len(bad)
                add(inc, ename, '0', base, twin, {10: 0}, C_SCALE1, 'CascadedPCF_mobile.frag (pack %s.%s)' % (v, ename), ' '.join(d0 + ['-DOC=1']), pk='%s.%s' % (v, ename))
        # ---- the fused light: the pack's, then the built-in capped ones
        light_tab = [b[0] for b in read_bundle(B / (LF.LIGHT_KEY + '.xgs'))['bindings']]
        c1d = ['-DSKY_C1X=0x%08xu' % c1x, '-DSKY_C1Y=0x%08xu' % c1y]
        forms = [('', None, [], 'ClusteredLightingWithSSAO_mobile.frag', False, False)]
        for cap in (2, 4):
            forms.append(('lcap%d_' % cap, LC.capped_source(), ['-DLCAP=%d' % cap], 'ClusteredLightingWithSSAO_mobile.frag + the LCAP text patch', True, False))
            text, tau = LC.thr_source(cap)
            forms.append(('lcap%dt_' % cap, text, LC.thr_defines(cap), 'ClusteredLightingWithSSAO_mobile.frag + the threshold text patch', True, True))
        for tag, text, dl, what, capped, ref in forms:
            for fogv in (False, True):
                lv = 'g' if fogv else 'f'
                dfs = ['-DLIGHTFUSE'] + (['-DLFFOG'] if fogv else []) + dl + c1d
                if text is None: src = LF.SRC / 'ClusteredLightingWithSSAO_mobile.frag'
                else: src = tmp / 'ClusteredLightingWithSSAO_lcap.frag'; src.write_text(text, encoding='utf-8')
                base = glslc(src, tmp / 'b.spv', dfs, LF.SRC)
                if capped:   # the module the library hands out is the built-in file: the same words
                    have = inc_words(OUT / ('light_%s%s.frag.spv.inc' % (tag, lv)))
                    if list(struct.unpack('<%dI' % (len(base) // 4), base)) != have:
                        out.append('  BUILT-IN light_%s%s.frag.spv.inc differs from this build of the same source' % (tag, lv)); fails += 1
                LF.check_descriptors(base, light_tab)
                case = lambda rng, it, fogv=fogv, capped=capped, ref=ref: fused_case(rng, it, fogv, capped, ref)
                for sfx, oc, want, cond in (('', OC_OWN, {**LIGHT_IN, **SKY_IN}, C_SCALE1), ('s', OC_SHARE, dict(LIGHT_IN), C_SCALE1 | C_SHARE)):
                    inc = 'tile_light_%s%s%s' % (tag, lv, sfx)
                    twin = glslc(src, tmp / 't.spv', dfs + ['-DOC=%#x' % oc], LF.SRC)
                    lines, f = prove('%s (fused %s%s)' % (inc, lv, ', ' + ' '.join(dl) if dl else ''), base, twin, case, LM.same, a.n, 0x3000 + len(rows))
                    out += lines; fails += f
                    bad = iface(inc, base, twin, want); out += bad; fails += len(bad)
                    if sfx:   # the condition of the shared form is real: other images on the Sky bindings change the result of the replaced module
                        mb, mt = SI.Module(base), SI.Module(twin); rng = np.random.default_rng(0x3900 + len(rows)); d = 0
                        for it in range(40):
                            inp, ub, tx = case(rng, it); tx = dict(tx)
                            for sb_ in SHARE: tx[sb_] = LM.BigTex(LM.table(rng, 0.05, 0.95), LM.FW, LM.FH)
                            if not LM.same(mb.run(inp, ub, tx), mt.run(inp, ub, tx)): d += 1
                        out.append('  control (other images on bindings 200 / 206 / 212 / 224: VKN_TILE_C_SHARE not met): differs in %d / 40' % d); fails += 0 if d else 1
                    add(inc, 'ClusteredLightingWithSSAO', lv.upper() if sfx else lv, base, twin, want, cond,
                        '%s, %s' % (what, 'built-in light_%s%s' % (tag, lv) if capped else 'pack %s.ClusteredLightingWithSSAO' % lv),
                        ' '.join(dfs + ['-DOC=%#x' % oc]), c1=(c1x, c1y), pk=None if capped else '%s.ClusteredLightingWithSSAO' % lv)
    # ---- the table the backend and the host test include
    t = ['// [DOS2] P10 (docs/dos2/nr/VK-P10.md): the input-attachment twins of the merged tile pass, generated by tools/dos2_tile_shaders.py - do not edit.',
         '// One row per twin: the engine shader name, the variant of vkn_shlib_named_tile (0 = the key\'s own module; f / g = the fused light with the',
         '// Sky part\'s own inputs, F / G = with the Sky part reading the Light part\'s inputs), FNV-1a 64 and word count of the SPIR-V the twin was',
         '// proven against (the only module it may replace), the twin, its inputs (binding, input_attachment_index), VKN_TILE_C_* (4 = the shared',
         '// form), the coordinate location (VKN_TILE_C_CENTER), the DeferredSky c1 bits it was compiled with (0 0 = none).',
         'struct vkn_tile_row { const char *name; char variant; uint64_t base_hash; uint32_t base_words; const uint32_t *spv; uint32_t spv_bytes; uint8_t n;',
         '                      uint8_t binding[16], index[16]; uint32_t cond; uint8_t center_loc; uint32_t c1[2]; const char *inc; };']
    for r in rows: t += ['static const uint32_t k_%s[] = {' % r[0], '#include "%s.frag.spv.inc"' % r[0], '};']
    t.append('static const vkn_tile_row k_tile_rows[] = {')
    for inc, ename, variant, h, bw, bs, ix, cond, center, c1, what in rows:
        t.append('    {"%s", %s, 0x%016xull, %d, k_%s, sizeof k_%s, %d, {%s}, {%s}, %du, %d, {0x%08xu, 0x%08xu}, "%s"},'
                 % (ename, "'%s'" % variant if variant != '0' else '0', h, bw, inc, inc, len(bs), ', '.join(map(str, bs)), ', '.join(map(str, ix)), cond, center, c1[0], c1[1], inc))
    t.append('};')
    files['tile_twins.inc'] = '\n'.join(t) + '\n'
    stale = [n for n, s in files.items() if not (OUT / n).is_file() or (OUT / n).read_text().replace('\r\n', '\n') != s]
    if a.check:
        if stale: out.append('  STALE (differ from this build): %s' % ' '.join(stale)); fails += 1
    elif not fails:
        for n, s in files.items(): (OUT / n).write_text(s, newline='\n')
        out.append('wrote %d files to %s (%d changed)' % (len(files), OUT, len(stale)))
    print('\n'.join(out)); print('%d twins; DeferredSky c1 %08x %08x' % (len(rows), c1x, c1y)); print('FAIL %d' % fails if fails else 'PASS')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
