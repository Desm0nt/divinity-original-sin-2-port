"""DOS2 NR-04 (DM_NR_SH): the engine shaders by name, built on the player's machine -> <shaders>/native-nr.xgspack (setup stage 8).

Every bundle = the player's own translated bundle of that engine shader (header, binding table, inputs, c1: byte for byte, read from
dm/shaders/shaders.xgspack) + the SPIR-V of this port's own GLSL rewrite of its fragment stage (tools/dos2_post/<Name>_mobile.frag,
tools/dos2_pcf/CascadedPCF_mobile.frag) compiled with glslc -O (vulkan1.1) and checked: spirv-val, entry-point interface equal to the
translated module (input / output locations), descriptors a subset of the translated binding table; the classes that have one also run
tools/spv_interp.py on translated and rewrite with the same seeded random inputs (outputs equal bit for bit, else the tool fails). Nothing
of the game goes into the APK: the host (xbg_game.cpp namespace nrsh, dm/gfx/nr/nr_sh.c) only names the bundles and checks them again.
Pack keys "<variant>.<engine name>" ("<variant>.<fragment key>" when the name is longer than 29): x = exact (post / light: proven bit for bit; shadows: 9 point taps, the Switch algorithm, no
interpreter proof: compared on the same inputs and reported - equal within ~1e-4, a tap compare at ref ~ depth can flip), m = mobile
(shadows only: 4 textureGather taps, a smoother edge - another look), d = dref (shadows only, PERF-X19 T11: mobile's 4 taps as hardware
depth compares, sampler2DShadow; the host sets compare LESS + linear on that binding; compared with m on the same inputs, reported).

  dos2_nr_sh_pack.py --pack dm/shaders/shaders.xgspack --out DIR [--n 40] [--programs work/shaders-jobs/programs.tsv]   what setup runs
  dos2_nr_sh_pack.py --bundles work/shaders-vk --out DIR [--ref packs/2a/native.xgspack.light-mobile ...]
      --ref: every bundle of the reference packs (keyed by the translated key, tools/dos2_*_mobile.py) whose key / variant is ours must
             carry the same SPIR-V bytes (the packs run on the phone 14.11 / 16.11: gp2, c3nat1)
Output: DIR/native-nr.xgspack, DIR/nr_sh.tsv (one row per bundle), DIR/equivalence.txt. Exit 1 on any failed check.
"""
import argparse, hashlib, re, shutil, struct, subprocess, sys, tempfile, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'dm' / 'gfx' / 'tools'))

# Keep in step with dm/gfx/nr/nr_sh.c nr_sh_table (source_structure_test.py compares them):
# (name, fragment key, class, variants, proven, colour attachments, NVN format of colour 0)
ENTRIES = [
    ('FadeBlending', '62d683cc49953ad6', 'post', 'x', 1, 1, 0x3f),
    ('EoCFog', 'e94ea226e689801c', 'post', 'x', 1, 1, 0x25),
    ('EoCOutlineCombine', '010472595b342188', 'post', 'x', 1, 1, 0x25),
    ('LinearizeDepth', 'f23c09d198b55dc5', 'post', 'x', 1, 1, 0x06),
    ('PPDownSampleLog2', 'cbe8649300e9c8cd', 'post', 'x', 1, 1, 0x3f),
    ('DeferredSky', '640a4c497bcc748b', 'light', 'x', 1, 2, 0x3f),
    ('ClusteredLightingWithSSAO', 'd236b8d5b7ffad3c', 'light', 'x', 1, 1, 0x3f),
    ('CascadedPCF_3x768', '1372db843749e1d3', 'shadow', 'xmd', 0, 1, 0x01),
    ('CascadedPCF_2x1024', 'a758e42af8853343', 'shadow', 'xmd', 0, 1, 0x01),
    ('CascadedPCF_3x512', '2a221ee245d55aab', 'shadow', 'xmd', 0, 1, 0x01),
    # NR-33A (engine post)
    ('DeferredFog', 'd8aeec640d465275', 'post', 'x', 1, 1, 0x3f),
    ('PPBloomUpsampleAndBlur', '4e290c9cf1162fe9', 'post', 'x', 1, 1, 0x3f),
    ('CombineUI_NoFetch', '98186c22a7209861', 'post', 'x', 1, 1, 0x25),
    ('PPBloomScaleBiasDownsampleAndBlur', '74fe9ae2066d53da', 'post', 'x', 1, 1, 0x3f),
    ('PPBloomDownsampleAndBlur', '0e3fc07b320db1e9', 'post', 'x', 1, 1, 0x3f),
    ('PPFXAA', '691768aa08545d95', 'post', 'x', 1, 1, 0x25),
    # end NR-33A (engine post)
]
PCF_DEFS = {'1372db843749e1d3': (3, 1536), 'a758e42af8853343': (2, 2048), '2a221ee245d55aab': (3, 1024)}   # cascades, atlas texels
PCF_MODE = {'x': 0, 'm': 1, 'd': 3}
# NR-ONCHIP (docs/dos2/nr/NR-ONCHIP.md §3 / §8): input-attachment variants, compiled from the same .frag with -DOC=<mask> (texture n with
# bit n set -> subpassInput, input_attachment_index n). Pack key "<v>.<name>" (nr_sh_pack_key); the host (xbg_nr_onchip.inc) uses them only
# inside its merged render pass. 2 = light.s2 (binding 52 tAmbient = Sky out 0, index 6), 3 = light.s3 (+ binding 58 tShadow, index 7).
# Checked here: spirv-val, the interface, exactly the mask's bindings as subpass inputs, and with --n > 0 the interpreter proof of
# tools/dos2_onchip_shaders.py (variant == translated == NR-04 native, bit for bit).
OC_VARIANTS = {'ClusteredLightingWithSSAO': [('2', 0x40), ('3', 0xC0)]}
# NR-LIGHTFUSE (docs/dos2/nr/NR-LIGHTFUSE.md): "f.ClusteredLightingWithSSAO" = DeferredSky computed inside the lighting stage
# (ClusteredLightingWithSSAO_mobile.frag -DLIGHTFUSE + dos2_post/lightfuse_sky.glsl, Sky's c1[0].xy baked from the player's DeferredSky
# bundle). Its extra bindings 200..254 (tools/dos2_lightfuse.py EXTRAS) are not in the translated table: the host (xbg_nr_lightfuse.inc)
# adds them to its twin. Checked here: spirv-val, interface, descriptors = the Light table + exactly the extras, and with --n > 0 the
# interpreter proof dos2_lightfuse.prove (fused == Sky -> R11G11B10F store -> Light -> ONE / ONE blend, bit for bit under RNE stores).
LF_VARIANTS = {'ClusteredLightingWithSSAO': 'fg'}   # f = Sky inside Light; g = f + DeferredFog of the stored Df on out 1 (DM_NR_LIGHTFUSE_FOG)
# Both are OPTIONAL: a variant that fails to build or to prove is left out of the pack (equivalence.txt says why) and setup accepts the pack
# with or without them; the host then keeps the separate passes (a red line at the first chain).
# NR-FRAME S2F (docs/dos2/nr/NR-FRAME.md §10): the Fog tail's variants - 5.ClusteredLightingWithSSAO (OC 0x48: tEmissive binding 28 index 3 +
# tAmbient 52 index 6) and 5.DeferredFog (OC 0x02: tScene binding 16 index 1). OPTIONAL: a variant that does not build or prove is left out
# of the pack with a note (the host then keeps plain S2); setup (step_nrpack) accepts packs with or without them.
OC_OPTIONAL = {'ClusteredLightingWithSSAO': [('5', 0x48)], 'DeferredFog': [('5', 0x02)]}
BIT = re.compile(r'native == translated bit for bit: (\d+) / (\d+); raw translated == spirv-opt -O translated: (\d+) / (\d+)')


def head_hash(b):
    """FNV-1a 64 of the bundle head without header word 6 (dm/gfx/nr/nr_sh.c nr_sh_head_hash)"""
    _, _, _, nb, ni, c1, sp, _ = struct.unpack_from('<8I', b, 0)
    h = 32 + nb * 32 + ni * 4 + c1
    x = 0xcbf29ce484222325
    for i in range(h):
        if 24 <= i < 28: continue
        x = ((x ^ b[i]) * 0x100000001b3) & 0xffffffffffffffff
    return x


def pack_read(path, keys):
    """{key: bundle bytes} of keys from an xgspack v1 (CRC checked)"""
    data = Path(path).read_bytes()
    magic, ver, n, _ = struct.unpack_from('<4I', data, 0)
    if magic != 0x50534758 or ver != 1: raise SystemExit('%s: not an xgspack v1' % path)
    got = {}
    for i in range(n):
        k, o, size, _, crc = struct.unpack_from('<32sQIIQ', data, 16 + 56 * i)
        k = k.rstrip(b'\0').decode('ascii', 'replace')
        if k in keys:
            b = data[o:o + size]
            if len(b) != size or zlib.crc32(b) != crc: raise SystemExit('%s: bundle %s damaged' % (path, k))
            got[k] = b
    return got


def spirv_of(b):
    _, _, _, nb, ni, c1, sp, _ = struct.unpack_from('<8I', b, 0)
    o = 32 + nb * 32 + ni * 4 + c1
    return b[o:o + sp]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pack', default='', help='translated shader pack (dm/shaders/shaders.xgspack)')
    ap.add_argument('--bundles', default='', help='directory of translated <key>.xgs (instead of --pack)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--n', type=int, default=40, help='random invocations per equivalence check (0 = none)')
    ap.add_argument('--ref', action='append', default=[], help='reference pack keyed by translated keys (same SPIR-V expected)')
    ap.add_argument('--programs', default='', help='work/shaders-jobs/programs.tsv: the name of every entry must carry its key there (fragment stage)')
    a = ap.parse_args()
    if a.programs and Path(a.programs).is_file():   # the name <-> key table of the game's packages (tools/dos2_shader_jobs.py, NR-02)
        ps = {}
        for line in Path(a.programs).read_text(encoding='utf-8').splitlines()[1:]:
            f = line.split('\t')
            if len(f) >= 4: ps[f[0]] = f[3]
        bad = ['%s: programs.tsv says %s' % (e[0], ps.get('EngineShaders:Shaders/NX/%s.bshd' % e[0], 'nothing')) for e in ENTRIES
               if ps.get('EngineShaders:Shaders/NX/%s.bshd' % e[0]) != e[1]]
        if bad: raise SystemExit('names and keys disagree with the game packages: ' + '; '.join(bad))
        print('names: all %d engine shader names carry their fragment key in %s' % (len(ENTRIES), a.programs))
    from dos2_native_shaders import read_bundle, write_bundle, tool, Spv
    from dos2_pcf_mobile import run, check_iface
    import xgs_pack
    keys = [e[1] for e in ENTRIES]
    if a.pack: tr_bytes = pack_read(a.pack, set(keys))
    elif a.bundles: tr_bytes = {k: (Path(a.bundles) / (k + '.xgs')).read_bytes() for k in keys if (Path(a.bundles) / (k + '.xgs')).is_file()}
    else: raise SystemExit('--pack or --bundles')
    miss = [k for k in keys if k not in tr_bytes]
    if miss: raise SystemExit('translated bundles missing: ' + ', '.join(miss))
    out = Path(a.out); stage = out / 'bundles'
    if stage.exists(): shutil.rmtree(stage)
    stage.mkdir(parents=True)
    rows = ['pack_key\tname\tfs_key\tclass\tproven\thead_fnv64\tspirv_bytes\tspirv_sha256_16\tinputs\toutputs\tequivalence']
    eq, fails = [], 0
    refs = {}
    mob_spv = {}   # T11: key -> the mobile variant's SPIR-V (the dref variant is compared with it; 'm' is built before 'd')
    for r in a.ref:
        for k, b in pack_read(r, set(keys)).items(): refs.setdefault(k, []).append((r, spirv_of(b)))
    with tempfile.TemporaryDirectory() as tmp:
        for name, key, cls, variants, proven, _, _ in ENTRIES:
            raw = tr_bytes[key]
            p = Path(tmp) / (key + '.xgs'); p.write_bytes(raw); tr = read_bundle(p)
            tr_opt = None
            for v in variants:
                spv = Path(tmp) / ('%s_%s.spv' % (key, v))
                if cls == 'shadow':
                    nc, atlas = PCF_DEFS[key]
                    run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-DNCASC=%d' % nc, '-DATLAS=%d.0' % atlas, '-DPCF_MODE=%d' % PCF_MODE[v],
                         '-fshader-stage=frag', '-o', str(spv), str(HERE / 'dos2_pcf' / 'CascadedPCF_mobile.frag')])
                else:
                    run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', '-I', str(HERE / 'dos2_post'), '-o', str(spv),
                         str(HERE / 'dos2_post' / ('%s_mobile.frag' % name))])
                run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(spv)])
                data = spv.read_bytes()
                check_iface(data, tr['spirv'])
                bundle = write_bundle(tr, data)
                pk = '%s.%s' % (v, name if len(name) + 2 < 32 else key)   # NR-33A: too long for an xgspack key -> the fragment key (nr_sh_pack_key)
                assert len(pk) < 32, pk
                (stage / (pk + '.xgs')).write_bytes(bundle)
                res = '-'
                if proven and v == 'x' and a.n > 0:
                    if tr_opt is None:
                        to, too = Path(tmp) / 'tr.spv', Path(tmp) / 'tro.spv'
                        to.write_bytes(tr['spirv']); run([tool('spirv-opt'), '-O', str(to), '-o', str(too)]); tr_opt = too.read_bytes()
                    mod = __import__('dos2_light_mobile' if cls == 'light' else 'dos2_post_mobile')
                    lines, _, _ = mod.equivalence(name, tr_opt, tr['spirv'], data, a.n, int(key[:8], 16))
                    m = BIT.search(lines[0])
                    if not m: raise SystemExit('equivalence: unexpected report %r' % lines[0])
                    good, n, rg, rn = map(int, m.groups())
                    if good != n: fails += 1; lines[0] = 'FAILED ' + lines[0]
                    elif rg != rn:   # spirv-opt's own float folding against the raw translated module: not this rewrite's doing
                        lines.append('  note: the raw translated module differs from its spirv-opt -O form in %d of %d (1-2 ulp, constant folding); '
                                     'the rewrite equals the optimized one, as the driver sees an optimized one too' % (rn - rg, rn))
                    eq += lines; res = '%d/%d' % (good, n)
                elif cls == 'shadow' and v == 'x' and a.n > 0:   # the Switch algorithm: compared, not required bit for bit (reported)
                    if tr_opt is None:
                        to, too = Path(tmp) / 'tr.spv', Path(tmp) / 'tro.spv'
                        to.write_bytes(tr['spirv']); run([tool('spirv-opt'), '-O', str(to), '-o', str(too)]); tr_opt = too.read_bytes()
                    import dos2_pcf_mobile
                    c1 = tr['head'][len(tr['head']) - struct.unpack_from('<I', raw, 20)[0]:]
                    lines, _, _ = dos2_pcf_mobile.equivalence(key, tr_opt, tr['spirv'], data, c1, a.n, int(key[:8], 16))
                    m = BIT.search(lines[0]); eq += lines
                    r8 = re.search(r'largest difference (\S+); .* (\d+) of (\d+) differ', lines[1])
                    res = '%s/%s bit for bit, largest difference %s, R8 %s/%s differ' % (m.group(1), m.group(2), r8.group(1), r8.group(2), r8.group(3)) if m and r8 else '?'
                elif cls == 'shadow' and v == 'd' and a.n > 0 and mob_spv.get(key):   # T11: the hardware compare against mobile (reported)
                    import dos2_pcf_mobile
                    c1 = tr['head'][len(tr['head']) - struct.unpack_from('<I', raw, 20)[0]:]
                    lines, bad = dos2_pcf_mobile.dref_vs_mobile(key, mob_spv[key], data, c1, max(4, a.n // 4), int(key[:8], 16) + 3)
                    eq += lines; fails += bad
                    m = re.search(r'largest difference (\S+), R8 (\d+) of (\d+)', lines[0])
                    res = 'vs mobile: largest difference %s, R8 %s/%s differ' % m.groups() if m else '?'
                elif cls == 'shadow':
                    res = 'none (%s)' % ('Switch algorithm, 9 point taps' if v == 'x' else 'mobile 2x2 PCF: another look' if v == 'm' else
                                         'dref: mobile with hardware compares')
                if cls == 'shadow' and v == 'm': mob_spv[key] = data
                for rp, rs in refs.get(key, []):
                    # the shadow keys of packs/2a: *.shadows-exact = exact; shadows-mobile / post-mobile / light-mobile carry the mobile ones
                    if cls != 'shadow' or v == ('x' if 'exact' in Path(rp).name else 'm'):
                        same = rs == data
                        eq.append('%s: SPIR-V %s the reference %s' % (pk, 'equals' if same else 'DIFFERS FROM', rp))
                        if not same: fails += 1
                n_ = Spv(data)
                rows.append('%s\t%s\t%s\t%s\t%d\t%016x\t%d\t%s\t%s\t%s\t%s' % (pk, name, key, cls, proven, head_hash(bundle), len(data),
                            hashlib.sha256(data).hexdigest()[:16], n_.io(1), n_.io(3), res))
            for v, oc, optional in [(v, oc, 0) for v, oc in OC_VARIANTS.get(name, [])] + [(v, oc, 1) for v, oc in OC_OPTIONAL.get(name, [])]:   # NR-ONCHIP
                import dos2_onchip_shaders as OCS
                pk = '%s.%s' % (v, name); assert len(pk) < 32, pk
                try:
                    data = OCS.compile_frag(OCS.SRCS[name], Path(tmp) / ('%s_oc%s.spv' % (key, v)), oc)
                    check_iface(data, tr['spirv'])
                    sp = OCS.subpass_bindings(data); want = {b: i for i, b in enumerate(OCS.INPUTS[name]) if oc >> i & 1}
                    if sp != want: raise SystemExit('%s OC=%#x: subpass inputs %s, expected %s' % (name, oc, sp, want))
                    bundle = write_bundle(tr, data)
                    res = '-'; bad = 0; lines = []
                    if a.n > 0:
                        if tr_opt is None:
                            to, too = Path(tmp) / 'tr.spv', Path(tmp) / 'tro.spv'
                            to.write_bytes(tr['spirv']); run([tool('spirv-opt'), '-O', str(to), '-o', str(too)]); tr_opt = too.read_bytes()
                        nat = OCS.compile_frag(OCS.SRCS[name], Path(tmp) / ('%s_ocnat.spv' % key))
                        mods = {'translated': OCS.SI.Module(tr_opt), 'native': OCS.SI.Module(nat), 'variant': OCS.SI.Module(data)}
                        lines, bad = OCS.check('oc.' + v, key, name, oc, mods, a.n, 11, 4)
                        res = '%d/%d' % (a.n - min(bad, a.n), a.n) if not bad else 'FAILED %d' % bad
                    if bad and optional: raise SystemExit('the interpreter proof failed (%d)' % bad)
                except BaseException as ex:
                    if not optional: raise
                    eq.append('%s: optional S2F variant left out of the pack (%s); the host keeps plain S2' % (pk, ex)); continue
                eq += lines; fails += 1 if bad else 0
                (stage / (pk + '.xgs')).write_bytes(bundle)
                n_ = Spv(data)
                rows.append('%s\t%s\t%s\t%s\t%d\t%016x\t%d\t%s\t%s\t%s\t%s' % (pk, name, key, 'onchip', proven, head_hash(bundle), len(data),
                            hashlib.sha256(data).hexdigest()[:16], n_.io(1), n_.io(3), res))
            for lv in LF_VARIANTS.get(name, ''):   # NR-LIGHTFUSE (optional)
                import dos2_lightfuse as LF
                pk = '%s.%s' % (lv, name); assert len(pk) < 32, pk
                try:
                    c1x, c1y = LF.sky_c1(tr_bytes[LF.SKY_KEY])
                    data = LF.compile_frag(Path(tmp) / ('%s_lf%s.spv' % (key, lv)), c1x, c1y, fog=lv == 'g')
                    n_ = Spv(data); t_ = Spv(tr['spirv'])
                    if n_.io(1) != t_.io(1): raise SystemExit('inputs differ: %s / %s' % (n_.io(1), t_.io(1)))
                    want_out = dict(t_.io(3)); want_out.update({1: t_.io(3)[0]} if lv == 'g' else {})
                    if n_.io(3) != want_out: raise SystemExit('outputs %s, expected %s' % (n_.io(3), want_out))
                    LF.check_descriptors(data, [b[0] for b in tr['bindings']])
                    bundle = write_bundle(tr, data)
                    res = '-'; lines = []; bad = 0
                    if a.n > 0:
                        sky_spv, light_spv, fog_spv = Path(tmp) / 'lf_sky.spv', Path(tmp) / 'lf_light.spv', Path(tmp) / 'lf_fog.spv'
                        run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', '-I', str(HERE / 'dos2_post'), '-o', str(sky_spv),
                             str(HERE / 'dos2_post' / 'DeferredSky_mobile.frag')])
                        fog_nat = None
                        if lv == 'g':
                            run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', '-I', str(HERE / 'dos2_post'), '-o', str(fog_spv),
                                 str(HERE / 'dos2_post' / 'DeferredFog_mobile.frag')]); fog_nat = fog_spv.read_bytes()
                        lines, bad = LF.prove(sky_spv.read_bytes(), LF.compile_frag(light_spv, fused=False), data, max(a.n, 40), 1119, c1x, c1y, fog_nat)
                        res = 'Sky->Light%s %d/%d' % ('->Fog' if lv == 'g' else '', max(a.n, 40), max(a.n, 40))
                    if bad: raise SystemExit('the interpreter proof failed (%d)' % bad)
                except BaseException as ex:
                    if isinstance(ex, KeyboardInterrupt): raise
                    eq.append('%s: optional NR-LIGHTFUSE variant left out of the pack (%s); the host keeps the separate passes' % (pk, ex)); continue
                eq += lines
                (stage / (pk + '.xgs')).write_bytes(bundle)
                rows.append('%s	%s	%s	%s	%d	%016x	%d	%s	%s	%s	%s' % (pk, name, key, 'lightfuse', 1, head_hash(bundle), len(data),
                            hashlib.sha256(data).hexdigest()[:16], n_.io(1), n_.io(3), res))
    pack = out / 'native-nr.xgspack'
    xgs_pack.build(str(stage), str(pack))
    shutil.rmtree(stage)
    (out / 'nr_sh.tsv').write_text('\n'.join(rows) + '\n', encoding='utf-8')
    (out / 'equivalence.txt').write_text('\n'.join(eq) + '\n', encoding='utf-8')
    print('\n'.join(rows)); print('\n'.join(eq))
    if fails: print('%d failed checks' % fails); return 1
    print('%s: %d bundles' % (pack, len(rows) - 1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
