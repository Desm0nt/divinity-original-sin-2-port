"""DOS2 NB-18b-3: fused post-tail fragment shaders + the "fused" native bundle format (dm/gfx/nb/nb_fused.h), offline only.

A fused shader runs in the pass of the LAST shader of a chain (the host) and computes the elided passes inline, so the plan can skip
them (NB-18b-4a/4b). It is made from the translated modules themselves (work/shaders-vk/<key>.xgs, spirv-opt -O) with tools/spv_splice.py:
the host's read of the elided pass's output becomes a call of the elided shader (its inputs set per pixel), so the arithmetic is the
translated one back to back. No GLSL rewrite: a hand port of EoCSight (~600 instructions) could not be kept bit exact.

  F1   PPApplyLUT o EoCSight                host EoCSight 91eb2d97 (LUT pass elided). Sight samples Base 1:1 at TexCoord0 = the pixel
                                            centre, LUT fetches at gl_FragCoord: the fused LUT runs at gl_FragCoord. The UNORM8 round trip
                                            of the elided RGBA8 target is emulated (RoundEven(sat(x)*255)/255) -> bit exact vs two passes.
  F2   PPVignette o CombineUI_NoFetch       host CombineUI_NoFetch 98186c22 (Vignette pass elided): the vignette is applied to the bilinear
                                            sample of its input at the combine's coordinate (TexCoord1 := the same uv; needs the vignette
                                            VS VPtoRTScaleBias = (1,1,0,0)). Not exact: bilinear(vig(x)) vs vig(bilinear(x)) + no RGBA8 step.
  F3   CombineUI_NoFetch, UI texel = 0      "3D only" first draw of the UI-direct final pass (plan п. 3b / 18b-7): bit exact vs the combine
                                            with a transparent UI texel.
  F3V  F3 o Vignette                        3D only + vignette (b with vignette): as F2.
  F1A  FXAA' o PPApplyLUT o EoCSight        (NB-18b-8, route A) host EoCSight; the LUT pass is elided and so is the console's FXAA pass that
       F1AS / F1AR = other luma proxies     follows Sight (plan: hle_dos2_nb_post.c / nb_post_aa.h; backend: its reader samples Sight's target).
                                            FXAA' = the translated PPFXAA 691768aa rebuilt to run BEFORE the LUT: its analysis taps (2 gathers
                                            + up to 9 G-only bilinear taps) read the pre-LUT HDR image through a luma proxy
                                            (tools/dos2_post/fused_a/luma.glsl), the centre colour is LUT(M), the last tap is
                                            mix(LUT(M), LUT(N), w) (blend.frag); its c1 literals are baked in, its target size comes from
                                            textureSize. The HDR extra binding carries NB_FX_E_LINEAR. Sight then runs on that colour at M.
                                            Not exact by design: see eq_F1A (control build, composition, decisions against the chain).
  F4 / F4V  (AA off) LUT -> Sight -> [Vignette] -> Combine in one pass: NOT built here - see the plan doc (Sight's view ray TexCoord1 is
                                            a per-vertex frustum corner, its corner order is not known offline).
Each fused bundle: work/shaders-native-post/fused/<name>.<hostkey>.xgs (format XGF1, nb_fused.h) + fused.xgspack (xgs_pack v1),
stats.tsv, equivalence.txt. Checks: spirv-val vulkan1.1, interface (inputs / outputs by location equal to the host's translated module,
builtins listed, descriptors = host's ⊆ + the extra table), equivalence with tools/spv_interp.py (sequence of translated shaders on
whole small images vs the fused one per pixel).
Usage: dos2_post_fuse.py [--bundles work/shaders-vk] [--out work/shaders-native-post] [--n 60] [--only F1]
"""
import argparse, shutil, struct, subprocess, sys, tempfile
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent / 'dm' / 'gfx' / 'tools'))
from dos2_native_shaders import Spv, read_bundle, tool, WORK
import spv_interp as SI
import spv_splice
import xgs_pack

K = {'LUT': '921728a5bbb1c71b', 'Sight': '91eb2d97a304709a', 'Vig': '43e8e8ed2b1df6c2', 'Comb': '98186c22a7209861', 'FXAA': '691768aa08545d95'}
VS = {'Vig': '1bb8aad5b05ae002', 'Sight': 'd52cf8500f1e95d3'}
XGF_MAGIC, XGS_MAGIC = 0x31464758, 0x31534758
F_EXACT, F_Q8, F_APPROX = 1, 2, 4
E_LINEAR = 1                        # nb_fused.h NB_FX_E_LINEAR
FUSED_A = HERE / 'dos2_post' / 'fused_a'
# bundle name -> (luma.glsl LUMA, text). F1A = the default (DM_NB_POST_LUMA=0): linear light never lost an edge the chain anti-aliases on
# any stand-in LUT (eq_dec / eq_luts) and costs nothing; F1AS (=1) / F1AR (=2) are the alternatives for the screenshot A/B.
LUMA = {'F1A': (1, 'g (linear light)'), 'F1AS': (0, 'sqrt(g)'), 'F1AR': (2, 'sqrt(g / (g + 1))')}
SRC_FUSED = 0xF0
REQ_UBO_EQ, REQ_SAME_SIZE, REQ_VP_MAP = 1, 2, 3   # nb_fused.h NB_FX_R_*
STAGE_VS, STAGE_FS = 0, 1


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, **kw)
    if r.returncode: raise SystemExit('%s failed:\n%s' % (Path(cmd[0]).name, (r.stderr or r.stdout).decode(errors='replace').strip()))
    return r.stdout


class Tools:
    def __init__(self, tmp): self.tmp = Path(tmp); self.n = 0

    def _f(self, ext): self.n += 1; return self.tmp / ('m%d.%s' % (self.n, ext))

    def opt(self, spv):
        a, b = self._f('spv'), self._f('spv'); a.write_bytes(spv); run([tool('spirv-opt'), '-O', str(a), '-o', str(b)]); return b.read_bytes()

    def dis(self, spv):
        a = self._f('spv'); a.write_bytes(spv)
        return run([tool('spirv-dis'), '--raw-id', '--no-header', '--no-indent', str(a)]).decode()

    def asm(self, text):
        a, b = self._f('spvasm'), self._f('spv'); a.write_text(text)
        run([tool('spirv-as'), '--target-env', 'spv1.3', str(a), '-o', str(b)]); return b.read_bytes()

    def val(self, spv):
        a = self._f('spv'); a.write_bytes(spv); run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(a)])

    def glsl(self, name, defs=()):
        """tools/dos2_post/fused_a/<name>.frag -> SPIR-V (glslc, vulkan1.1) -> spirv-opt -O"""
        b = self._f('spv')
        run([tool('glslc'), '-fshader-stage=frag', '--target-env=vulkan1.1', '-I', str(FUSED_A)] + ['-D' + d for d in defs] +
            [str(FUSED_A / (name + '.frag')), '-o', str(b)])
        return self.opt(b.read_bytes())


# ---------------------------------------------------------------------------------------------------------------------- recipes
def next_binding(tr):
    return max(b[0] for b in tr['bindings']) + 6


def fuse(T, mods, trs, host, steps):
    """steps: [(site binding, producer name or None, tag, inputs, quant, const)] applied in order to the host's module.
    Returns (spirv, extras [(vk binding, entry of the producer's translated table, producer name)])."""
    text = T.dis(mods[host]); nb = next_binding(trs[host]); extras = []
    for site, prod, tag, inputs, quant, const in steps:
        bind_map = {}
        if prod:
            for b in trs[prod]['bindings']:
                bind_map[b[0]] = nb; extras.append((nb, b, prod)); nb += 6
        text, info = spv_splice.splice(text, T.dis(mods[prod]) if prod else None, site, tag, bind_map, inputs, quant, const)
        text = T.dis(T.opt(T.asm(text)))
    spv = T.opt(T.asm(text)); T.val(spv)
    used = {b for _, k, s_, b, _ in Spv(spv).descriptors()}
    return spv, [e for e in extras if e[0] in used], info      # producer bindings spirv-opt removed are not fed


RECIPES = {
    # name: (host, elided chain, steps, flags, requirements)
    'F1': ('Sight', ['LUT'], [(16, 'LUT', 'lut', {'FragCoord': 'fragcoord'}, True, None)], F_EXACT | F_Q8,
           [(0, STAGE_VS, REQ_VP_MAP, 'Sight VPtoRTScaleBias maps its viewport onto the LUT target 1:1', None),
            (1, STAGE_FS, REQ_SAME_SIZE, 'LUT RT0 == Sight RT0', None)]),   # NB-18b-4 phone fix 16.11: was UBO_EQ (1,1,0,0)
    'F2': ('Comb', ['Vig'], [(16, 'Vig', 'vig', {1: 'coord', 2: 'coord'}, False, None)], F_APPROX,
           [(1, STAGE_VS, REQ_UBO_EQ, 'Vignette VPtoRTScaleBias', (1, 1, 0, 0)), (1, STAGE_FS, REQ_SAME_SIZE, 'Vignette RT0 == its Base', None)]),
    # NB-18b-8 route A: built by fuse_a (FXAA' o LUT inside Sight), same requirements as F1; one bundle per luma proxy (LUMA)
    'F1A': ('Sight', ['LUT'], None, F_APPROX | F_Q8,
            [(0, STAGE_VS, REQ_VP_MAP, 'Sight VPtoRTScaleBias maps its viewport onto the LUT target 1:1', None), (1, STAGE_FS, REQ_SAME_SIZE, 'LUT RT0 == Sight RT0', None)]),
    'F1AS': ('Sight', ['LUT'], None, F_APPROX | F_Q8,
             [(0, STAGE_VS, REQ_VP_MAP, 'Sight VPtoRTScaleBias maps its viewport onto the LUT target 1:1', None), (1, STAGE_FS, REQ_SAME_SIZE, 'LUT RT0 == Sight RT0', None)]),
    'F1AR': ('Sight', ['LUT'], None, F_APPROX | F_Q8,
             [(0, STAGE_VS, REQ_VP_MAP, 'Sight VPtoRTScaleBias maps its viewport onto the LUT target 1:1', None), (1, STAGE_FS, REQ_SAME_SIZE, 'LUT RT0 == Sight RT0', None)]),
    'F3': ('Comb', [], [(10, None, 'ui', None, False, (0.0, 0.0, 0.0, 0.0))], F_EXACT, []),
    'F3V': ('Comb', ['Vig'], [(10, None, 'ui', None, False, (0.0, 0.0, 0.0, 0.0)),
                             (16, 'Vig', 'vig', {1: 'coord', 2: 'coord'}, False, None)], F_APPROX,
            [(1, STAGE_VS, REQ_UBO_EQ, 'Vignette VPtoRTScaleBias', (1, 1, 0, 0)), (1, STAGE_FS, REQ_SAME_SIZE, 'Vignette RT0 == its Base', None)]),
}


# ---------------------------------------------------------------------------------------------------------------------- route A (F1A)
def fxaa_prepare(text, c1, base_to):
    """The translated PPFXAA (spirv-opt -O, --raw-id text) made ready for the splices of fxaa_pre:
       - its Base (binding 10) moves to binding base_to (the HDR image with a linear clamp sampler);
       - the c1 literals (fp_c1[0].xyz: subpixel 1/12, 0.5, an int) become constants; the target size fp_c15[13].zw becomes textureSize;
       - every read of Base is classed by what the code uses of it: 'luma' (only G, never the output: 2 gathers + the edge-search taps)
         or 'colour' (reaches the output through the OpBitcast chain: the centre and the last tap). The centre is both: its G also
         feeds the analysis, so the read is doubled - the copy is a luma read.
    Returns (text, luma {result id: 'gather' | 'sample'}, colour [result ids in code order])."""
    M = spv_splice.Module(text); B = spv_splice.Builder(M, 'fx')
    base, c15v, c1v = M.var_by_binding(10), M.var_by_binding(16), M.var_by_binding(22)
    if None in (base, c15v, c1v) or len(M.funcs) != 1: raise SystemExit('PPFXAA: not the module this recipe was written for')
    for i in M.annot:
        if i.op == 'OpDecorate' and i.args[0] == base and i.args[1] == 'Binding': i.args[2] = str(base_to)
    fn = M.funcs[0]
    uses = {}
    for i in fn:
        for a in i.args:
            if a.startswith('%'): uses.setdefault(a, []).append(i)
    ival = lambda x: int(M.defs[x].args[1])
    simg = M.defs[M.defs[base].args[0]].args[1]; img = M.defs[simg].args[0]
    f32, i32 = B.f32(), B.decl('OpTypeInt', '32', '1'); v2i = B.decl('OpTypeVector', i32, '2'); i0 = B.decl('OpConstant', i32, '0')
    chains = {i.res: (i.args[1], [ival(x) for x in i.args[2:]]) for i in fn if i.op == 'OpAccessChain' and i.args[1] in (c15v, c1v)}
    bloads = {i.res for i in fn if i.op == 'OpLoad' and i.args[1] == base}
    luma, colour, out, n_c1, n_size = {}, [], [], 0, 0
    for i in fn:
        if i.op == 'OpLoad' and i.args[1] in chains:
            var, idx = chains[i.args[1]]
            if var == c1v:
                if idx[:2] != [0, 0]: raise SystemExit('PPFXAA: c1 index %s' % idx)
                bits = struct.unpack_from('<I', c1, 4 * idx[2])[0]; us = uses.get(i.res, [])
                if us and all(u.op == 'OpBitcast' for u in us):          # read as an int: constant-fold the bitcasts
                    for u in us: u.op = 'OpCopyObject'; u.args = [u.args[0], B.decl('OpConstant', u.args[0], str(bits))]
                    n_c1 += 1; continue
                out.append(spv_splice.Ins(i.res, 'OpCopyObject', [f32, B.decl('OpConstant', f32, '%.9g' % struct.unpack('<f', struct.pack('<I', bits))[0])]))
                n_c1 += 1; continue
            if idx[:2] != [0, 13] or idx[2] not in (2, 3): raise SystemExit('PPFXAA: c15 index %s' % idx)
            a, b, c, d = (B.fresh('sz') for _ in range(4))
            out += [spv_splice.Ins(a, 'OpLoad', [simg, base]), spv_splice.Ins(b, 'OpImage', [img, a]),
                    spv_splice.Ins(c, 'OpImageQuerySizeLod', [v2i, b, i0]), spv_splice.Ins(d, 'OpCompositeExtract', [i32, c, str(idx[2] - 2)]),
                    spv_splice.Ins(i.res, 'OpConvertSToF', [f32, d])]
            n_size += 1; continue
        out.append(i)
        if i.op not in spv_splice.IMAGE_READS or i.args[1] not in bloads: continue
        ex = uses.get(i.res, [])
        if any(u.op != 'OpCompositeExtract' for u in ex): raise SystemExit('PPFXAA: a read is used whole')
        if i.op == 'OpImageGather': luma[i.res] = 'gather'; continue
        if not any(v.op == 'OpBitcast' for u in ex for v in uses.get(u.res, [])):
            if any(u.args[2] != '1' for u in ex): raise SystemExit('PPFXAA: a luma tap reads more than G')
            luma[i.res] = 'sample'; continue
        colour.append(i.res)
        both = [u for u in ex if any(v.op != 'OpBitcast' for v in uses.get(u.res, []))]
        if both:
            if any(u.args[2] != '1' for u in both): raise SystemExit('PPFXAA: the analysis reads more than G of a colour tap')
            r2 = B.fresh('lr'); out.append(spv_splice.Ins(r2, i.op, list(i.args))); luma[r2] = 'sample'
            for u in both:
                x2 = B.fresh('lx'); out.append(spv_splice.Ins(x2, 'OpCompositeExtract', [u.args[0], r2, '1']))
                for v in uses.get(u.res, []):
                    if v.op != 'OpBitcast': v.args = [x2 if a == u.res else a for a in v.args]
    M.funcs[0] = out; M.glob = spv_splice._order(M.glob)
    if n_c1 != 3 or n_size != 2 or len(colour) != 2 or sum(k == 'gather' for k in luma.values()) != 2:
        raise SystemExit('PPFXAA: unexpected shape (c1 loads %d, size loads %d, colour reads %d, luma %s)' % (n_c1, n_size, len(colour), luma))
    return M.text(), luma, colour


def fxaa_pre(T, mods, trs, luma_mode, colour='LUT', binds=None):
    """FXAA' as a fragment module of its own: input location 1 = uv (the pixel centre), gl_FragCoord; output 0 = the anti-aliased colour
    AFTER the colour producer (PPApplyLUT with the RGBA8 round trip; 'ident' for the control build). binds = the final vk bindings of
    the producer's table entries {producer binding: vk}; the HDR image (producer binding 10) is FXAA's own Base, linear sampler."""
    binds = binds or {4: 58, 10: 64, 16: 70, 22: 76}
    if binds[10] != 64: raise SystemExit('fused_a/*.frag declare the HDR image at binding 64')
    text, luma, colours = fxaa_prepare(T.dis(mods['FXAA']), c1_of(trs['FXAA']), binds[10])
    if luma_mode != 1:                     # LUMA 1 = linear light: the taps are used as they are
        for kind, src, tag in (('gather', 'proxy_4', 'p4'), ('sample', 'proxy_g', 'pg')):
            ids = {r for r, k in luma.items() if k == kind}
            text, _ = spv_splice.splice(text, T.dis(T.glsl(src, ['LUMA=%d' % luma_mode])), None, tag, sites=[(binds[10], ids, {0: 'read'}, False)])
    text, _ = spv_splice.splice(text, T.dis(T.glsl('blend')), None, 'bl', sites=[(binds[10], {colours[1]}, {0: 'coord', 1: ('value', colours[0]), 'FragCoord': 'fragcoord'}, False)])
    prod = T.dis(mods['LUT']) if colour == 'LUT' else T.dis(T.glsl('ident'))
    q = colour == 'LUT'
    text, info = spv_splice.splice(text, prod, None, 'lut', bind_map=binds if colour == 'LUT' else None, sites=[
        (binds[10], {colours[0]}, {'FragCoord': 'fragcoord'}, q), (206, None, {'FragCoord': 'coord'}, q)])
    if binds[10] not in info['shared']: raise SystemExit("FXAA': the colour producer did not share the HDR binding")
    spv = T.opt(T.asm(text)); T.val(spv)
    left = sorted(b for _, k, s_, b, _ in Spv(spv).descriptors())
    want = sorted(binds.values()) if colour == 'LUT' else [binds[10]]
    if left != want: raise SystemExit("FXAA': descriptors %s, expected %s" % (left, want))
    return spv


def fuse_a(T, mods, trs, name):
    """F1A: EoCSight's read of Base (binding 16) = FXAA'(uv = that read's coordinate, gl_FragCoord). Returns (spirv, extras, FXAA' spirv)."""
    nb = next_binding(trs['Sight']); binds, extras = {}, []
    for b in trs['LUT']['bindings']:
        binds[b[0]] = nb; extras.append((nb, b, 'LUT', E_LINEAR if b[0] == 10 else 0)); nb += 6
    pre = fxaa_pre(T, mods, trs, LUMA[name][0], 'LUT', binds)
    text, _ = spv_splice.splice(T.dis(mods['Sight']), T.dis(pre), 16, 'fa', None, {1: 'coord', 'FragCoord': 'fragcoord'}, False, None)
    spv = T.opt(T.asm(text)); T.val(spv)
    used = {b for _, k, s_, b, _ in Spv(spv).descriptors()}
    return spv, [e for e in extras if e[0] in used], pre


def pack_xgf(name, recipe, trs, spv, extras):
    """XGF1 = header + host / elided keys + extra-binding table + requirements + an XGS1 bundle (host head, binding table extended)"""
    host, chain, _, flags, reqs = recipe
    tr = trs[host]; elided = [K[c] for c in chain]
    ext = b''
    for vk, b, prod, *ef in extras:
        binding, typ, count, size, source, slot, samp, bflags = b
        ext += struct.pack('<IIIBBBBII', vk, typ, size, chain.index(prod) + 1, STAGE_FS, source, ef[0] if ef else 0, slot, bflags)
    req = b''
    for pas, stage, kind, _, val in reqs:
        if kind == REQ_UBO_EQ: req += struct.pack('<BBBBIII4f', pas, stage, kind, 0, 0, 0, 0, *val)   # c-buffer slot 0, offset 0
        else: req += struct.pack('<BBBBIII4f', pas, stage, kind, 0, 0, 0, 0, 0, 0, 0, 0)            # SAME_SIZE / VP_MAP (c-buffer slot 0, offset 0)
    # embedded XGS1: host header with nb += extras, the host's bindings + extras (source SRC_FUSED, slot = table index), inputs, c1
    raw = tr['raw']; h = list(struct.unpack_from('<8I', raw, 0)); nb0, ni, c1 = h[3], h[4], h[5]
    binds = raw[32:32 + nb0 * 32]
    for i, (vk, b, prod, *ef) in enumerate(extras):
        binds += struct.pack('<8I', vk, b[1], b[2], b[3], SRC_FUSED, i, b[6], b[7])
    h[3] = nb0 + len(extras); h[6] = len(spv)
    o = 32 + nb0 * 32
    bundle = struct.pack('<8I', *h) + binds + raw[o:o + ni * 4 + c1] + spv
    keys = K[host].encode().ljust(16, b'\0')[:16] + b''.join(k.encode()[:16] for k in elided).ljust(64, b'\0')
    head = struct.pack('<8I', XGF_MAGIC, 1, flags, len(chain), len(extras), len(reqs), len(bundle), 0)
    return head + keys + ext + req + bundle


# ---------------------------------------------------------------------------------------------------------------------- interface check
def check_iface(fused, host_tr_spv, extras):
    n, t = Spv(fused), Spv(host_tr_spv)
    for sc, what in ((1, 'inputs'), (3, 'outputs')):
        if n.io(sc) != t.io(sc): raise SystemExit('%s differ: fused %s host %s' % (what, n.io(sc), t.io(sc)))
    added = sorted(n.builtins(1) - t.builtins(1))
    tb = {b for _, k, s, b, _ in t.descriptors()} | {e[0] for e in extras}
    extra = {b for _, k, s, b, _ in n.descriptors()} - tb
    if extra: raise SystemExit('fused descriptors outside host + extra table: %s' % sorted(extra))
    return added, sorted(b for _, k, s, b, _ in n.descriptors())


# ---------------------------------------------------------------------------------------------------------------------- equivalence
def support(rng, W, H, general):
    b = bytearray(1392); struct.pack_into('<4f', b, 144, 1 / W, 1 / H, 0, 0); struct.pack_into('<4f', b, 160, W, H, 0, 0)
    for i in range(73): struct.pack_into('<f', b, 192 + 16 * i, 1.0)
    if general:
        for i in range(5): struct.pack_into('<f', b, 192 + 16 * i, float(rng.choice([1.0, 0.5, 2.0, -2.0])))
    return bytes(b)


def cb(vec4s, n=16):
    b = bytearray(16 * n)
    for i, v in vec4s.items(): struct.pack_into('<4f', b, 16 * i, *v)
    return bytes(b)


def rtex(rng, h, w, lo=0.0, hi=1.0, alpha=None, special=None):
    d = rng.uniform(lo, hi, (h, w, 4)).astype(np.float32)
    if alpha is not None: d[..., 3] = rng.choice(alpha, (h, w))
    if special is not None:
        m = rng.random((h, w, 4)) < 0.2; d[m] = rng.choice(special, m.sum())
    return d


def q8(x, decode='div'):
    """UNORM8 store + load: k = RoundEven(sat(x) * 255); decode 'div' = k / 255 (texture unit, correctly rounded), 'mul' = k * (1/255)
    (what the fused shader computes: spirv-opt -O folds its x / 255 into x * 0.00392156886)"""
    with np.errstate(all='ignore'):
        v = np.clip(np.nan_to_num(np.asarray(x, np.float32), nan=0.0), 0, 1)
        k = np.rint(v * np.float32(255)).astype(np.float32)
        return (k / np.float32(255) if decode == 'div' else k * np.float32(1 / 255)).astype(np.float32)


def ulps(a, b):
    ia = np.array([struct.unpack('<i', struct.pack('<f', float(x)))[0] for x in a], np.int64)
    ib = np.array([struct.unpack('<i', struct.pack('<f', float(x)))[0] for x in b], np.int64)
    return int(np.max(np.abs(ia - ib)))


def bits(a): return [struct.pack('<f', float(x)) for x in a]


def same(a, b):
    return a is not None and b is not None and all(x == y or (np.isnan(struct.unpack('<f', x)[0]) and np.isnan(struct.unpack('<f', y)[0]))
                                                   for x, y in zip(bits(a[0]), bits(b[0])))


STATS = {}   # name -> {'fused': [(executed, tex ops)], 'parts': [...]} per output pixel (parts: host + producers x pixel ratio)


def stat(name, kind, ex, tx): STATS.setdefault(name, {}).setdefault(kind, []).append((ex, tx))


def image(mod, W, H, inputs_of, ubos, tex, acc=None):
    out = np.zeros((H, W, 4), np.float32)
    for y in range(H):
        for x in range(W):
            r = mod.run(inputs_of(x, y), ubos, tex); out[y, x] = r[0] if r else 0
            if acc is not None: acc.append((mod.executed, mod.tex_ops))
    return out


def eq_F1(I, trs, n, rng, quiet=False, name='F1', flat=False, part=0, outside=None):
    """LUT pass over a whole 8x8 image -> RGBA8 -> Sight at one pixel (translated, -O) vs the fused shader at that pixel.
    name='F1A..', flat=True (NB-18b-8): the HDR image has no edge (one colour +- 1 %), so the console's FXAA pass that follows Sight in
    the chain is the identity and F1A must still equal LUT -> RGBA8 -> Sight bit for bit."""
    W = H = 8; bad = {'mul': 0, 'div': 0}; worst = 0; out_lsb = 0
    P = part or W   # [VK-LOWRES] a lower 3D scale: the chain draws the top-left P x P texels of its 8x8 targets (VP_MAP: TexCoord0 = texel centre)
    outs = []
    for it in range(n):
        general = it >= (3 * n) // 4
        sbL = support(rng, W, H, general); sbS = support(rng, W, H, False)
        hdr = SI.Texture(rtex(rng, H, W, 0.0, 12.0, special=[0.0, 1.0, 1e4, 0.5]))
        if flat: hdr = SI.Texture((rng.uniform(0.02, 6.0, 4) * rng.uniform(0.99, 1.01, (H, W, 4))).astype(np.float32))
        if P < W:   # outside the part: what the frame left there (poison NaN, or the caller's filling for the dependence check)
            d = np.array(hdr.d, np.float32)
            if d is not None:
                fill = outside(rng, d.shape) if outside else np.full(d.shape, np.nan, np.float32)
                m = np.zeros(d.shape[:2], bool); m[P:, :] = True; m[:, P:] = True
                d[m] = fill[m]; hdr = SI.Texture(d)
        lut3 = SI.Texture3D(rng.uniform(0, 1, (4, 4, 4, 4)).astype(np.float32))
        c5L = cb({0: (rng.uniform(0, 0.05), rng.uniform(1, 20), float(rng.choice([0.0, 0.3, 1.0])), 0)})
        acc = []
        lut_img = image(I['LUT'], W, H, lambda x, y: {'FragCoord': (x + .5, y + .5, .5, 1)}, {4: sbL, 22: c5L}, {10: hdr, 16: lut3}, acc)
        if P < W: lut_img[P:, :] = np.nan; lut_img[:, P:] = np.nan   # the LUT pass draws only the part
        lut_ex = np.mean([e for e, _ in acc]), np.mean([t for _, t in acc])
        px, py = int(rng.integers(0, P)), int(rng.integers(0, P))
        uv = ((px + .5) / W, (py + .5) / H, 0.0, 1.0); ray = tuple(rng.uniform(-2, 2, 3)) + (0.0,)
        c8 = {i: tuple(rng.uniform(0.5, 4, 4)) for i in range(4)}
        c8[4] = (float(rng.choice([0.0, 0.6])), float(rng.choice([0.0, 0.7])), 0, 0)
        sub = {40: cb(c8), 46: cb({12: tuple(rng.uniform(-3, 3, 3)) + (1.0,)}), 52: c1_of(trs['Sight']), 4: sbS}
        tx = {10: SI.Texture(rtex(rng, H, W, 0.0, 1.2, special=[1.0, 0.999])),
              22: SI.Texture(rtex(rng, H, W)), 28: SI.Texture(rtex(rng, H, W)), 34: SI.Texture(rtex(rng, H, W))}
        fu_ubo = dict(sub); fu_tx = dict(tx)
        for vk, b, *_ in I[name + 'x']:
            if b[0] in (4, 22): fu_ubo[vk] = {4: sbL, 22: c5L}[b[0]]
            else: fu_tx[vk] = {10: hdr, 16: lut3}[b[0]]
        got = I[name].run({'FragCoord': (px + .5, py + .5, .5, 1), 1: uv, 2: ray}, fu_ubo, fu_tx)
        if part and outside is not None: outs.append((px, py, got)); continue   # eq_part_dep: only the fused result
        if not general and name == 'F1': stat('F1', 'fused', I['F1'].executed, I['F1'].tex_ops)
        for dec in ('mul', 'div'):
            ref = I['Sight'].run({1: uv, 2: ray}, sub, {**tx, 16: SI.Texture(q8(lut_img, dec))})
            if dec == 'mul' and not general and name == 'F1': stat('F1', 'parts', I['Sight'].executed + lut_ex[0], I['Sight'].tex_ops + lut_ex[1])
            if not same(ref, got):
                bad[dec] += 1
                if dec == 'mul' and bad[dec] <= 3 and not quiet: print('  %s MISMATCH' % name, it, general, ref, got)
                if dec == 'div':
                    worst = max(worst, ulps(ref[0], got[0]))
                    out_lsb = max(out_lsb, int(np.max(np.abs(q8(ref[0]) - q8(got[0])) * 255 + 0.5)))
    if part and outside is not None: return outs
    line = ('%s: %d random 8x8 images (%d with render scale 1 in the LUT pass, %d random scales); intermediate RGBA8 '
            'decoded k*(1/255) (= the fused code): fused == LUT pass then Sight pass bit for bit %d / %d; decoded k/255 (texture unit): '
            'equal %d / %d, max %d ulp, max %d LSB after the RGBA8 store of Sight') % (
        ('F1 (LUT o Sight)' if name == 'F1' else name + ' on images without an edge (FXAA = identity)') +
        (' [part %dx%d of 8x8, the texels outside it NaN]' % (P, P) if part else ''),
        n, (3 * n) // 4, n - (3 * n) // 4, n - bad['mul'], n, n - bad['div'], n, worst, out_lsb)
    return line, bad['mul'] + (out_lsb > 1)


def eq_part_dep(I, trs, name, n, P):
    """[VK-LOWRES] F1A on a P x P part: which part pixels depend on the texels outside the part (FXAA's taps reach over the border; the
    console's FXAA at 1:1 - the FSR pre pass - reads the same neighbourhood of its own input). The same scenes twice with two random
    fillings outside the part: a pixel whose result changes depends on them. Returns {distance to the border: (changed, total)}."""
    def runs(seed_out):
        r_out = np.random.default_rng(seed_out)
        return eq_F1(I, trs, n, np.random.default_rng(21), name=name, part=P,
                     outside=lambda rng, shp: r_out.uniform(0.0, 12.0, shp).astype(np.float32))
    a, b = runs(1), runs(2); dist = {}
    for (px, py, ga), (_, _, gb) in zip(a, b):
        d = min(P - 1 - px, P - 1 - py); c, t = dist.get(d, (0, 0))
        dist[d] = (c + (not same(ga, gb)), t + 1)
    return dist


def comb_inputs(rng, Ww, Hw, px, py):
    return {'FragCoord': (px + .5, py + .5, .5, 1), 1: ((px + .5) / Ww, (py + .5) / Hw, 0.0, 1.0)}


_BIG = {}


def big(shape):
    if shape not in _BIG: _BIG[shape] = np.zeros(shape, np.float32)
    a = _BIG[shape]; a[:] = 0; return a


def eq_comb(I, trs, name, n, rng):
    """F2 / F3 / F3V at the real sizes (3D 2160x972 -> window 2400x1080): the reference runs the translated Vignette only on the 2x2
    texels the combine's bilinear sample touches (texel centres, RGBA8 store), then the translated CombineUI_NoFetch; UI zero for F3*"""
    Wl, Hl, Ww, Hw = 2160, 972, 2400, 1080
    vig = name in ('F2', 'F3V'); ui0 = name in ('F3', 'F3V')
    worst, diff, bad_exact = 0, 0, 0
    src_a = big((Hl, Wl, 4)); v_a = np.zeros((Hl, Wl, 4), np.float32); ui_a = np.zeros((Hw, Ww, 4), np.float32)
    for it in range(n):
        px, py = int(rng.integers(0, Ww)), int(rng.integers(0, Hw))
        if it % 5 == 0: px, py = int(rng.choice([0, Ww - 1])), int(rng.choice([0, Hw - 1]))       # edges / corners
        u, v = (px + .5) / Ww, (py + .5) / Hw
        x0, y0 = int(np.floor(np.float32(u) * Wl - .5)), int(np.floor(np.float32(v) * Hl - .5))
        tx = [(min(max(x, 0), Wl - 1), min(max(y, 0), Hl - 1)) for x in (x0, x0 + 1) for y in (y0, y0 + 1)]
        src_a[:] = 0; v_a[:] = 0; ui_a[:] = 0
        for x, y in tx: src_a[y, x] = rng.uniform(0, 1, 4)
        c8v = cb({0: tuple(rng.uniform(0, 1, 3)) + (0,), 1: (rng.uniform(0.2, 1.5), rng.uniform(0.5, 4), 0, 0)})
        sbV = support(rng, Wl, Hl, False); src = SI.Texture(src_a)
        vex = (0, 0)
        if vig:
            for x, y in tx:
                c = ((x + .5) / Wl, (y + .5) / Hl, 0, 1)
                v_a[y, x] = q8(I['Vig'].run({1: c, 2: c}, {4: sbV, 16: c8v}, {10: src})[0])
            r = Wl * Hl / (Ww * Hw); vex = (I['Vig'].executed * r, I['Vig'].tex_ops * r)
            base94 = SI.Texture(v_a)
        else: base94 = src
        a_ = 0.0 if ui0 else float(rng.choice([0.0, 0.0, 1.0, 0.0005, 0.3, 0.8]))
        ui_a[py, px] = tuple(rng.uniform(0, 1, 3) * a_) + (a_,)
        py_ = 1.0 if it % 4 else float(rng.choice([1.0, 2.2, 0.8]))
        ubo = {4: support(rng, Ww, Hw, False), 22: cb({0: (0, 0, 0, 0), 1: (1.0, py_, 1.0, 1.0)}), 28: c1_of(trs['Comb'])}
        inp = comb_inputs(rng, Ww, Hw, px, py)
        ui = SI.Texture(ui_a)
        ref = I['Comb'].run(inp, ubo, {10: ui, 16: base94})
        fu_ubo, fu_tx = dict(ubo), {10: ui}
        for vk, b, prod in I[name + 'x']:
            if b[0] == 4: fu_ubo[vk] = sbV
            elif b[0] == 16: fu_ubo[vk] = c8v
            elif b[0] == 10: fu_tx[vk] = src
        if not vig: fu_tx[16] = base94
        stat(name, 'parts', I['Comb'].executed + vex[0], I['Comb'].tex_ops + vex[1])
        got = I[name].run(inp, fu_ubo, fu_tx)
        stat(name, 'fused', I[name].executed, I[name].tex_ops)
        if not same(ref, got): bad_exact += 1
        d = int(np.max(np.abs(q8(ref[0]) - q8(got[0])) * 255 + 0.5))
        worst = max(worst, d); diff += d > 0
    if vig:
        line = ('%s: %d random pixels at the real sizes (3D %dx%d -> window %dx%d, 1 in 5 on an edge / corner; Params.y = 1 in 3 of 4), '
                'RGBA8 output vs Vignette pass -> RGBA8 -> CombineUI_NoFetch: max %d LSB, %d / %d pixels differ (float bits equal %d / %d)') % (
            name, n, Wl, Hl, Ww, Hw, worst, diff, n, n - bad_exact, n)
        return line, int(worst > 1)
    line = '%s: %d random pixels (real sizes), fused == CombineUI_NoFetch with a zero UI texel bit for bit: %d / %d' % (name, n, n - bad_exact, n)
    return line, bad_exact


# ---------------------------------------------------------------------------------------------------------------------- route A checks
def pq(x):
    """ST.2084 as PPApplyLUT codes it (x = luminance / 10000 nits)"""
    xm = np.power(np.abs(x), 0.159301758)
    return np.power((0.8359375 + 18.8515625 * xm) / (1 + 18.6875 * xm), 78.84375)


def pq_inv(v):
    vp = np.power(np.clip(v, 0, 1), 1 / 78.84375)
    return np.power(np.maximum(vp - 0.8359375, 0) / (18.8515625 - 18.6875 * vp), 1 / 0.159301758)


def synth_lut(kind='filmic', n=32, white=100.0):
    """A stand-in for the game's 3D LUT (its real content is not known offline): PQ-coded input (scene value 1.0 = `white` nits, the
    Params (0, white) of synth_c5) -> a tone curve -> gamma 2.2. Texel i sits at (i + 0.5) / n, as the shader samples it.
    kind: 'filmic' (ACES fit), 'contrast' (filmic + contrast 1.4 around mid grey), 'reinhard' (s / (1 + s)), 'clip' (no tone curve)."""
    v = (np.arange(n) + 0.5) / n; s = pq_inv(v) * 10000.0 / white
    if kind == 'reinhard': t = s / (1 + s)
    elif kind == 'clip': t = np.clip(s, 0, 1)
    else: t = np.clip(s * (2.51 * s + 0.03) / (s * (2.43 * s + 0.59) + 0.14), 0, 1)
    if kind == 'contrast': t = np.clip(0.18 * (t / 0.18) ** 1.4, 0, 1)
    t = t ** (1 / 2.2)
    d = np.zeros((n, n, n, 4), np.float32)
    d[..., 0] = t[None, None, :]; d[..., 1] = t[None, :, None]; d[..., 2] = t[:, None, None]; d[..., 3] = 1
    return SI.Texture3D(d)


def scene(rng, W, H, lo, hi, ratio, noise=0.01):
    """an aliased straight edge between two colours: level log-uniform in [lo, hi], the brighter side `ratio` (log-uniform) times
    the darker; +- noise. Returns (float32 [H, W, 4], level, ratio)."""
    th = rng.uniform(0, np.pi); nx, ny = np.cos(th), np.sin(th); c = rng.uniform(-2, 2)
    yy, xx = np.mgrid[0:H, 0:W]; side = ((xx - W / 2 + .5) * nx + (yy - H / 2 + .5) * ny - c) > 0
    lv = np.exp(rng.uniform(np.log(lo), np.log(hi))); r = np.exp(rng.uniform(np.log(ratio[0]), np.log(ratio[1])))
    a = lv * rng.uniform(0.7, 1.3, 3); b = lv * r * rng.uniform(0.7, 1.3, 3)
    d = np.ones((H, W, 4), np.float32); d[..., :3] = np.where(side[..., None], b, a) * rng.uniform(1 - noise, 1 + noise, (H, W, 3))
    return d, lv, r


def lsb(a, b): return int(np.max(np.abs(q8(a[:3]) - q8(b[:3])) * 255 + 0.5))


def fxaa_ref(I, trs, img, px, py):
    """the translated PPFXAA at one pixel of an RGBA8 image (float32 [H, W, 4], already quantised)"""
    H, W = img.shape[:2]
    return I['FXAA'].run({1: ((px + .5) / W, (py + .5) / H, 0.0, 1.0)}, {16: cb({13: (0, 0, W, H)}, 16), 22: c1_of(trs['FXAA'])}, {10: SI.Texture(img)})[0]


def eq_ctl(I, trs, n, rng):
    """Control build of FXAA' (linear luma, colour producer = the texel itself) against the translated PPFXAA on RGBA8 images with an
    edge: the surgery (read classes, c1 constants, textureSize, the blend of two texels instead of one bilinear tap) kept the code."""
    W = H = 16; tot = eq = edges = worst = 0
    for it in range(n):
        img = q8(np.clip(scene(rng, W, H, 0.05, 0.6, (1.0, 4.0), 0.02)[0], 0, 1))
        for py in range(5, 11):
            for px in range(5, 11):
                ref = fxaa_ref(I, trs, img, px, py)
                got = I['CTL'].run({1: ((px + .5) / W, (py + .5) / H, 0.0, 1.0), 'FragCoord': (px + .5, py + .5, .5, 1)}, {}, {64: SI.Texture(img)})[0]
                d = lsb(ref, got); tot += 1; eq += d == 0; worst = max(worst, d); edges += lsb(ref, img[py, px]) > 0
    line = ("FXAA' control (linear luma, colour = the texel): %d pixels of %d RGBA8 16x16 edge images against the translated PPFXAA: equal "
            "%d / %d after the RGBA8 store, max %d LSB; the FXAA changed %d of them") % (tot, n, eq, tot, worst, edges)
    return line, int(worst > 1 or edges < tot // 20)


def synth_c5(white=100.0, fade=0.0): return cb({0: (0.0, white, fade, 0)})


def eq_dec(I, trs, name, n, rng, lut3):
    """FXAA' with the bundle's luma proxy on the pre-LUT image against the chain LUT pass -> RGBA8 -> translated PPFXAA, per pixel
    around an aliased edge (level 0.02..6 scene units, edge ratio 1.03..6; 1.0 = white of the stand-in LUT). An edge decision = the
    output differs from the un-anti-aliased pixel. Sight is left out on both sides (identity when no sight cone is active; eq_comp
    covers the composition). Returns (lines, totals): one line for all scenes + one per brightness band of the darker side."""
    W = H = 16; sb = support(rng, W, H, False); c5 = synth_c5()
    bands = (('all', 0, 1e9), ('shadows < 0.1', 0, 0.1), ('mid tones 0.1..1', 0.1, 1.0), ('highlights > 1', 1.0, 1e9))
    new = lambda: {'n': 0, 'both': 0, 'none': 0, 'missed': 0, 'extra': 0, 'max': 0, 'sum': 0, 'gt4': 0, 'ex_e': [], 'ex_n': [], 'tx_e': [], 'tx_n': [], 'rex': [], 'rtx': []}
    S = [new() for _ in bands]
    for it in range(n):
        hdr, lv, _ = scene(rng, W, H, 0.02, 6.0, (1.03, 6.0)); ht = SI.Texture(hdr)
        acc = []
        lut_img = q8(image(I['LUT'], W, H, lambda x, y: {'FragCoord': (x + .5, y + .5, .5, 1)}, {4: sb, 22: c5}, {10: ht, 16: lut3}, acc))
        for py in range(5, 11):
            for px in range(5, 11):
                ref = fxaa_ref(I, trs, lut_img, px, py); rex, rtx = I['FXAA'].executed + acc[0][0], I['FXAA'].tex_ops + acc[0][1]
                got = I[name + 'p'].run({1: ((px + .5) / W, (py + .5) / H, 0.0, 1.0), 'FragCoord': (px + .5, py + .5, .5, 1)},
                                        {58: sb, 76: c5}, {64: ht, 70: lut3})[0]
                re_, ge = lsb(ref, lut_img[py, px]) > 0, lsb(got, lut_img[py, px]) > 0; d = lsb(ref, got)
                for st, (_, lo, hi) in zip(S, bands):
                    if not lo <= lv < hi: continue
                    st['n'] += 1; st['both' if re_ and ge else 'none' if not re_ and not ge else 'missed' if re_ else 'extra'] += 1
                    st['max'] = max(st['max'], d); st['sum'] += d; st['gt4'] += d > 4; st['rex'].append(rex); st['rtx'].append(rtx)
                    st['ex_e' if ge else 'ex_n'].append(I[name + 'p'].executed); st['tx_e' if ge else 'tx_n'].append(I[name + 'p'].tex_ops)
    m = lambda v: float(np.mean(v)) if v else 0.0
    st = S[0]
    lines = [("%s (luma %s) decisions: %d pixels of %d HDR edge images, chain = LUT -> RGBA8 -> PPFXAA: edge in both %d, in neither %d, "
              "missed by the proxy %d (%.1f %% of the chain's edges), extra %d (%.1f %% of the pixels); RGBA8 difference mean %.2f LSB, max %d, "
              "> 4 LSB in %d pixels; FXAA' + LUT executes %.0f instr / %.1f taps on a no-edge pixel, %.0f / %.1f on an edge pixel "
              "(chain LUT + FXAA on the same pixels: %.0f / %.1f)") % (
        name, LUMA[name][1], st['n'], n, st['both'], st['none'], st['missed'], 100.0 * st['missed'] / max(1, st['both'] + st['missed']), st['extra'],
        100.0 * st['extra'] / st['n'], st['sum'] / st['n'], st['max'], st['gt4'], m(st['ex_n']), m(st['tx_n']), m(st['ex_e']), m(st['tx_e']),
        m(st['rex']), m(st['rtx']))]
    for st, (bn, _, _) in list(zip(S, bands))[1:]:
        if st['n']: lines.append("  %s, %s: %d pixels, both %d, neither %d, missed %d (%.1f %% of the chain's edges), extra %d (%.1f %% of the pixels), mean %.2f LSB, max %d, > 4 LSB %d" % (
            name, bn, st['n'], st['both'], st['none'], st['missed'], 100.0 * st['missed'] / max(1, st['both'] + st['missed']), st['extra'],
            100.0 * st['extra'] / st['n'], st['sum'] / st['n'], st['max'], st['gt4']))
    return lines, S[0]


def eq_luts(I, trs, name, n):
    """the decisions of eq_dec again on the other stand-in LUTs: how much the choice of the proxy depends on the unknown tone curve"""
    out = []
    for kind in ('contrast', 'reinhard', 'clip'):
        _, st = eq_dec(I, trs, name, n, np.random.default_rng(12), synth_lut(kind))
        out.append('%s: missed %d of %d (%.1f %%), extra %d (%.1f %% of %d pixels), mean %.2f LSB, max %d' % (
            kind, st['missed'], st['both'] + st['missed'], 100.0 * st['missed'] / max(1, st['both'] + st['missed']), st['extra'],
            100.0 * st['extra'] / st['n'], st['n'], st['sum'] / st['n'], st['max']))
    return '  %s (luma %s) on other stand-in LUTs - %s' % (name, LUMA[name][1], '; '.join(out))


def eq_comp(I, trs, name, n, rng, lut3):
    """F1A (FXAA' spliced into EoCSight) against the translated EoCSight reading FXAA''s own output at that texel: bit for bit"""
    W = H = 16; bad = 0; sb = support(rng, W, H, False); c5 = synth_c5(fade=0.25)
    for it in range(n):
        hdr = SI.Texture(scene(rng, W, H, 0.02, 6.0, (1.5, 6.0))[0]); px, py = int(rng.integers(5, 11)), int(rng.integers(5, 11))
        uv = ((px + .5) / W, (py + .5) / H, 0.0, 1.0); ray = tuple(rng.uniform(-2, 2, 3)) + (0.0,)
        c8 = {i: tuple(rng.uniform(0.5, 4, 4)) for i in range(4)}
        c8[4] = (float(rng.choice([0.0, 0.6])), float(rng.choice([0.0, 0.7])), 0, 0)
        sub = {40: cb(c8), 46: cb({12: tuple(rng.uniform(-3, 3, 3)) + (1.0,)}), 52: c1_of(trs['Sight']), 4: sb}
        tx = {10: SI.Texture(rtex(rng, H, W, 0.0, 1.2, special=[1.0, 0.999])),
              22: SI.Texture(rtex(rng, H, W)), 28: SI.Texture(rtex(rng, H, W)), 34: SI.Texture(rtex(rng, H, W))}
        pre = I[name + 'p'].run({1: uv, 'FragCoord': (px + .5, py + .5, .5, 1)}, {58: sb, 76: c5}, {64: hdr, 70: lut3})[0]
        base = np.zeros((H, W, 4), np.float32); base[:] = pre
        ref = I['Sight'].run({1: uv, 2: ray}, sub, {**tx, 16: SI.Texture(base)})
        stat(name, 'sight', I['Sight'].executed, I['Sight'].tex_ops)
        got = I[name].run({'FragCoord': (px + .5, py + .5, .5, 1), 1: uv, 2: ray}, {**sub, 58: sb, 76: c5}, {**tx, 64: hdr, 70: lut3})
        stat(name, 'fused', I[name].executed, I[name].tex_ops)
        if not same(ref, got):
            bad += 1
            if bad <= 3: print('  %s COMPOSITION MISMATCH' % name, it, ref, got)
    return "%s composition: %d edge pixels, fused == translated EoCSight over FXAA''s output bit for bit %d / %d" % (name, n, n - bad, n), bad


def c1_of(tr):
    h = struct.unpack_from('<8I', tr['raw'], 0); o = 32 + h[3] * 32 + h[4] * 4
    return tr['raw'][o:o + h[5]]


# ---------------------------------------------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--out', default=str(WORK / 'shaders-native-post'))
    ap.add_argument('--n', type=int, default=60)
    ap.add_argument('--only', default='')
    ap.add_argument('--part', action='store_true', help='[VK-LOWRES] also prove F1 on a 75 / 50 %% part (outside NaN) and measure '
                    'how far F1A depends on the texels outside the part')
    a = ap.parse_args()
    out = Path(a.out); fd = out / 'fused'
    if fd.exists(): shutil.rmtree(fd)
    fd.mkdir(parents=True)
    trs = {k: read_bundle(Path(a.bundles) / (v + '.xgs')) for k, v in K.items()}
    rows = ['name\thost\telided\tflags\textra bindings\tadded builtins\tinstr_opt fused\tinstr_opt sum of parts\texec/pixel fused\t'
            'exec/pixel parts\ttex/pixel fused\ttex/pixel parts\tspirv bytes\tbundle bytes']
    eq, fails = [], 0
    with tempfile.TemporaryDirectory() as tmp:
        T = Tools(tmp)
        mods = {k: T.opt(tr['spirv']) for k, tr in trs.items()}
        I = {k: SI.Module(m) for k, m in mods.items()}
        lut3 = synth_lut(); ctl_done = False
        for name, rc in RECIPES.items():
            if a.only and a.only != name: continue
            host, chain, steps, flags, reqs = rc
            if name in LUMA: spv, extras, pre = fuse_a(T, mods, trs, name)
            else: spv, extras, info = fuse(T, mods, trs, host, steps)
            added, descs = check_iface(spv, trs[host]['spirv'], extras)
            blob = pack_xgf(name, rc, trs, spv, extras)
            fn = '%s.%s' % (name, K[host])
            (fd / (fn + '.xgs')).write_bytes(blob)
            I[name] = SI.Module(spv); I[name + 'x'] = extras
            rng = np.random.default_rng(int(K[host][:8], 16) ^ len(name))
            if name == 'F1':
                line, nf = eq_F1(I, trs, a.n, rng)
                for P in ((6, 4) if a.part else ()):   # [VK-LOWRES] 75 % / 50 % of the 8x8 targets
                    pl, pn = eq_F1(I, trs, a.n, np.random.default_rng(30 + P), part=P); print(pl); eq.append(pl); fails += pn
            elif name in LUMA:   # NB-18b-8: control build once, then per bundle: no-edge exactness, composition, decisions vs the chain
                I[name + 'p'] = SI.Module(pre); I['FXAA'] = I.get('FXAA') or SI.Module(mods['FXAA']); nf = 0; line = ''
                if not ctl_done:
                    ctl_done = True; I['CTL'] = SI.Module(fxaa_pre(T, mods, trs, 1, 'ident'))
                    cl, cn = eq_ctl(I, trs, max(4, a.n // 6), np.random.default_rng(11)); print(cl); eq.append(cl); fails += cn
                for l2, n2 in (eq_F1(I, trs, max(8, a.n // 3), np.random.default_rng(5), name=name, flat=True),
                               eq_comp(I, trs, name, max(8, a.n // 4), np.random.default_rng(6), lut3)):
                    print(l2); eq.append(l2); nf += n2
                dl, dst = eq_dec(I, trs, name, max(8, a.n * 2), np.random.default_rng(12), lut3)   # the same scenes for every proxy
                line = dl[0]; print(chr(10).join(dl[1:])); eq.extend(dl[1:])
                l3 = eq_luts(I, trs, name, max(8, a.n)); print(l3); eq.append(l3)
                for P in ((6, 4) if a.part and name == 'F1A' else ()):   # [VK-LOWRES] the border band of F1A on a part
                    dd = eq_part_dep(I, trs, name, max(16, a.n), P)
                    pl = '%s on a %dx%d part of 8x8 (75 / 50 %%): part pixels whose result depends on the texels outside the part, by distance to its '                          'right / bottom border: %s' % (name, P, P, ', '.join('%d px: %d / %d' % (d, c, t) for d, (c, t) in sorted(dd.items())))
                    print(pl); eq.append(pl)
                if name == 'F1A' and dst['missed'] * 50 > dst['both'] + dst['missed']: nf += 1      # the default proxy: < 2 % of the chain's edges missed
                sg = STATS[name].pop('sight')          # per pixel of the chain: LUT + PPFXAA (decision scenes) + EoCSight (composition scenes)
                STATS[name]['parts'] = [(np.mean(dst['rex']) + np.mean([e for e, _ in sg]), np.mean(dst['rtx']) + np.mean([t for _, t in sg]))]
            else: line, nf = eq_comb(I, trs, name, a.n * 3, rng)
            print(line); eq.append(line); fails += nf
            if name == 'F1':   # control: the same splice without the RGBA8 emulation must be caught by the bit-exact test
                ctl, _, _ = fuse(T, mods, trs, host, [(16, 'LUT', 'lut', {'FragCoord': 'fragcoord'}, False, None)])
                keep, kst = I['F1'], {k: list(v) for k, v in STATS['F1'].items()}; I['F1'] = SI.Module(ctl)
                cl, cn = eq_F1(I, trs, max(8, a.n // 4), np.random.default_rng(7), quiet=True)
                I['F1'] = keep; STATS['F1'] = kst
                cl = 'control (F1 without the UNORM8 emulation): caught in %d of %d images' % (cn, max(8, a.n // 4))
                print(cl); eq.append(cl)
                if not cn: fails += 1
            # executed instructions / texture ops per output pixel (render scale 1): fused vs the parts (producers x pixel ratio)
            parts = sum(Spv(mods[c]).count for c in chain + [host] + (['FXAA'] if name in LUMA else []))
            st = {k: (np.mean([e for e, _ in v]), np.mean([t for _, t in v])) for k, v in STATS.get(name, {}).items()}
            ex_f, tx_f = st.get('fused', (0, 0)); ex_p, tx_p = st.get('parts', (0, 0))
            rows.append('\t'.join(['%s'] * 6 + ['%d', '%d', '%.1f', '%.1f', '%.2f', '%.2f', '%d', '%d']) % (
                fn, host, '+'.join(chain) or '-', flags, ' '.join('%d<-%s:%s%d%s' % (e[0], e[1][4], 'p', chain.index(e[2]) + 1, 'L' if len(e) > 3 and e[3] else '') for e in extras) or '-',
                ','.join(map(str, added)) or '-', Spv(spv).count, parts, ex_f, ex_p, tx_f, tx_p, len(spv), len(blob)))
    xgs_pack.build(str(fd), str(out / 'fused.xgspack'))
    (out / 'stats.tsv').write_text('\n'.join(rows) + '\n')
    (out / 'equivalence.txt').write_text('\n'.join(eq) + '\n')
    print('\n'.join(rows))
    if fails: raise SystemExit('%d equivalence failures' % fails)


if __name__ == '__main__':
    main()
