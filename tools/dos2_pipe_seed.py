#!/usr/bin/env python3
"""DOS2 NR-31: the portable pipeline warm-up seed (DM_NR_SEED; docs/dos2/nr/NR-31.md; runtime dm/gfx/nr/nr_seed.{h,c}).

A device's warm-up list (files/dm/vk-pipeline-list.bin, xbg_game.cpp namespace warm: one record per graphics pipeline it built =
stage shader pack keys + the exact create-info key) holds that DEVICE's create infos. `build` turns a pulled list into a seed:
every record is decoded and checked against the shapes draw() writes (anything else is refused), its stage keys and module ids
are checked, duplicates go, and the SOURCE device's policy (tile barrier, depth stand-ins, dynamic-UBO limit, NOOUT, vertex
retype / fetch support, optional features: from its dm.log with --log, else a preset) goes into the header. On a device the
runtime (DM_NR_SEED=1) re-targets each record to its own policy (nr_seed.c nrs_expand) and builds it in the background before
gameplay; a record it cannot re-target exactly is refused there, never built. A draw takes a warm pipeline only on an exact key
match, so a wrong record costs a build, never a different picture.

Content (`audit` proves it for a file): shader pack keys (16 hex digits = a 64-bit hash of a game shader's control + code, plus
"#at<func>_<bits>" alpha-test variants) and fixed-function numbers (Vulkan enums / formats / strides / offsets / masks / blend
constants). No shader code, no SPIR-V, no game file bytes - every word is a decoded field of a known create-info slot.

  build --list F [F ...] --out SEED [--log dm.log [dm.log ...] | --policy turnip] [--bundles DIR] [--post-pack P]   the seed (+ report)
  audit SEED                       re-decode every byte of a seed: what it holds, by class (the legal check)
  dump SEED                        header policy + per record: stages, pass formats, vertex formats
  diff A B                         two lists: records with the same base stage keys, the fields that differ (what a policy changes)
  policy --log dm.log              the device policy read from a dm.log
  pso --list F [--pak P --programs T]   iPad .pso catalogue (tools/dos2_nr_pso.py) against a list: pairs a template-based seed could add
--bundles: a directory of <key>.xgs bundles (work/shaders-vk): the layout of every record is re-derived from the bundles' binding
tables under the source policy (the same model the runtime checks the seed with); a recorded layout must hold the bundles' bindings in
their order (binding, count, stage), its uniform buffers static or dynamic - which ones were dynamic is the recording device's rule
(dynamic-UBO limit, NR-08 split, NR-08b read-only, an older build), never game state, so the seed carries the model's layout.
--post-pack: the fused post bundles (DM_NB_POST "F1." / "F1A." ... stage keys; work/shaders-native-post-a/fused.xgspack): their binding
tables as xbg_nb_post.inc compose() rewrites them.

NR-31b - a list holds records of several policies (one phone, many runs: SIM mali / powervr / ubdyn8 runs, older builds). The first
--log is the seed's source policy (the device's own, e.g. the Mi 14 with every current default); further --log files are the other
policies the list was recorded under (their dm.logs). Each record is matched to the policy whose render-pass dependencies it carries,
re-targeted to the source policy exactly (dependencies, layout, vertex variant, NOOUT masks of fragment shaders with outputs; what the
recording device could not record - a feature it lacks - stays as recorded, as nsd_expand does) and deduplicated on the result: two
records of one draw recorded under two policies are one seed record ("duplicate (re-targeted)"). A NOOUT record of a fragment shader
without outputs has lost the game's colour write masks: it is kept only when the same draw was recorded with masks (then a duplicate),
else left out as "noout" (its masks are unknown; such shaders write no colour anyway).
"""
import argparse, collections, json, re, struct, sys
from pathlib import Path

REC_MAGIC, SEED_MAGIC, SEED_VERSION = 0x57504C58, 0x31535044, 1
POLICY_FIELDS = ['tile_barrier', 'd24s8_as_d32s8', 'x8d24_as_d32', 'depth16_as_d24', 'bias_x2', 'ubo_dynamic', 'ubo_dyn_limit', 'dyn_partial',
                 'noout', 'retype', 'alpha_test', 'features', 'unmodelled'] + [f'vtx_unsup{i}' for i in range(8)]
F_NONSOLID, F_DCLAMP, F_BCLAMP, F_DBOUNDS, F_LOGICOP, F_ALL = 1, 2, 4, 8, 16, 31
# the Mi 14 Ultra (Adreno 750, Turnip, dm.log 17.11): every optional feature, native depth formats, D16 as D16, maxUBdyn 16, retype / NOOUT /
# tile barrier off, alpha test on (DOS2), every vertex format fetchable
PRESETS = {'turnip': dict(tile_barrier=0, d24s8_as_d32s8=0, x8d24_as_d32=0, depth16_as_d24=0, bias_x2=0, ubo_dynamic=1, ubo_dyn_limit=16, dyn_partial=1,
                          noout=0, retype=0, alpha_test=1, features=F_ALL, unmodelled=0, vtx_unsup=[0] * 8, split=1, ro=0)}
# split / ro (NR-08 DM_NR_DESC_SPLIT, NR-08b DM_NR_UBODYN read-only rule) are layout rules of a device, not header words: a seed's source
# layouts are the plain model's (the runtime checks them with it), the destination applies its own rules (nsd_layout)
KEY = re.compile(r'^[0-9a-fA-F]{16}(#at[1-9][0-9]{0,2}_[0-9a-f]{8}|#at0_[0-9a-f]{8}|#vi[0-9a-f]{8}(c[0-9a-f]{8})?)?$')
# a fused post stage of the backend (DM_NB_POST: xbg_nb_post.inc fused_name, nr_seed.c nsd_key_parse): "F1." "F2." "F1A." "F1AS." "F1AR." + host key
FUSED = re.compile(r'^F[12][A-Z]{0,2}\.[0-9a-f]{16}$')
REASONS = ['ok', 'parse', 'big', 'shape', 'key', 'module-id', 'variant', 'shader', 'facts', 'layout', 'descriptor-limit', 'depth-ambiguous',
           'attachment-format', 'vertex-format', 'limit', 'noout', 'dependencies', 'policy', 'vertex-ambiguous', 'split']


def key_ok(k):
    return KEY.match(k) is not None or FUSED.match(k) is not None


def bundle_key(k):
    """The bundle a stage key names: the base key, or the whole fused key (its own bundle in the post pack)."""
    return k if FUSED.match(k) else k.split('#')[0]


def fnv(s):   # warm::fnv
    h = 1469598103934665603
    for c in s.encode('ascii'):
        h ^= c; h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h | (1 << 63)


def fnv64(b):
    h = 1469598103934665603
    for c in b:
        h ^= c; h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def records(data, off=0):
    """[(keys, words, raw bytes)] of a list (warm::start's reader: stops at the first bad record); and whether it ended cleanly."""
    i, out = off, []
    def u32():
        nonlocal i
        if i + 4 > len(data): return None
        v = struct.unpack_from('<I', data, i)[0]; i += 4; return v
    while i < len(data):
        start = i
        magic, ns = u32(), u32()
        if magic != REC_MAGIC or ns is None or ns > 5: return out, False
        keys = []
        for _ in range(ns):
            n = u32()
            if n is None or n > 256 or i + n > len(data): return out, False
            keys.append(data[i:i + n].decode('ascii', 'replace')); i += n
        count = u32()
        if count is None or count > 4096 or i + count * 4 > len(data): return out, False
        words = list(struct.unpack_from('<%dI' % count, data, i)); i += count * 4
        out.append((keys, words, data[start:i]))
    return out, True


class Bad(Exception):
    def __init__(self, reason, field=''): super().__init__(reason); self.reason, self.field = reason, field


def decode(w):
    """The record's fields: dict + [(name, value)] per word (nr_seed.c nrs_parse; warm::build's reader)."""
    i, names = 0, []
    def u(name):
        nonlocal i
        if i >= len(w): raise Bad('parse', name)
        names.append(name); i += 1; return w[i - 1]
    p = {}
    if u('version') != 1: raise Bad('parse', 'version')
    ln = u('layout.words'); end = i + ln
    p['l_flags'], nlb = u('layout.set.flags'), u('layout.set.bindings')
    if nlb > 1280: raise Bad('big', 'layout')
    p['lb'] = [(u('layout.binding'), u('layout.binding.type'), u('layout.binding.count'), u('layout.binding.stages')) for _ in range(nlb)]
    p['pl_flags'], p['sets'], npush = u('layout.flags'), u('layout.sets'), u('layout.push')
    if npush > 16: raise Bad('parse', 'layout.push')
    p['push'] = [(u('layout.push.stages'), u('layout.push.offset'), u('layout.push.size')) for _ in range(npush)]
    if i != end: raise Bad('parse', 'layout.words')
    pn = u('pass.words'); end = i + pn
    p['r_flags'], natt = u('pass.flags'), u('pass.attachments')
    if natt > 9: raise Bad('parse', 'pass.attachments')
    p['att'] = [dict(flags=u('pass.att.flags'), format=u('pass.att.format'), samples=u('pass.att.samples'), load=u('pass.att.load'), store=u('pass.att.store'),
                     sload=u('pass.att.stencil_load'), sstore=u('pass.att.stencil_store'), init=u('pass.att.initial_layout'), fin=u('pass.att.final_layout')) for _ in range(natt)]
    p['s_flags'], p['s_bind'], ncol = u('pass.subpass.flags'), u('pass.subpass.bind_point'), u('pass.colors')
    if ncol > 8: raise Bad('parse', 'pass.colors')
    p['col'] = [(u('pass.color.attachment'), u('pass.color.layout')) for _ in range(ncol)]
    p['depth'] = (u('pass.depth.attachment'), u('pass.depth.layout')) if u('pass.has_depth') else None
    nd = u('pass.dependencies')
    if nd > 8: raise Bad('parse', 'pass.dependencies')
    p['dep'] = [tuple(u('pass.dep.' + k) for k in ('src', 'dst', 'src_stage', 'dst_stage', 'src_access', 'dst_access', 'flags')) for _ in range(nd)]
    if i != end: raise Bad('parse', 'pass.words')
    p['flags'], p['subpass'], ns = u('ci.flags'), u('ci.subpass'), u('ci.stages')
    if ns > 5: raise Bad('parse', 'ci.stages')
    p['st'] = []
    for _ in range(ns):
        fl, stg = u('stage.flags'), u('stage.stage'); lo, hi = u('stage.module'), u('stage.module'); p['st'].append((fl, stg, lo | hi << 32))
    p['vi_flags'], nvb = u('vi.flags'), u('vi.bindings')
    if nvb > 32: raise Bad('parse', 'vi.bindings')
    p['vb'] = [(u('vi.binding'), u('vi.binding.stride'), u('vi.binding.rate')) for _ in range(nvb)]
    nva = u('vi.attributes')
    if nva > 32: raise Bad('parse', 'vi.attributes')
    p['va'] = [(u('vi.attr.location'), u('vi.attr.binding'), u('vi.attr.format'), u('vi.attr.offset')) for _ in range(nva)]
    p['ia'] = (u('ia.flags'), u('ia.topology'), u('ia.restart'))
    p['tess'] = (u('tess.flags'), u('tess.patch')) if u('tess.present') else None
    vpf, nvp, nsc, hv, hs = u('vp.flags'), u('vp.viewports'), u('vp.scissors'), u('vp.static_viewports'), u('vp.static_scissors')
    if nvp > 16 or nsc > 16: raise Bad('parse', 'vp')
    p['vp'] = (vpf, nvp, nsc, [u('vp.viewport') for _ in range(6 * nvp)] if hv else None, [u('vp.scissor') for _ in range(4 * nsc)] if hs else None)
    p['rs'] = dict((k, u('rs.' + k)) for k in ('flags', 'depth_clamp', 'discard', 'polygon_mode', 'cull', 'front_face', 'depth_bias', 'depth_bias_constant',
                                                 'depth_bias_clamp', 'depth_bias_slope', 'line_width'))
    p['ms'] = dict((k, u('ms.' + k)) for k in ('flags', 'samples', 'sample_shading', 'min_sample_shading', 'alpha_to_coverage', 'alpha_to_one', 'sample_mask'))
    if p['ms']['sample_mask']:
        if p['ms']['samples'] > 64: raise Bad('parse', 'ms.samples')
        p['ms_mask'] = [u('ms.sample_mask.word') for _ in range((p['ms']['samples'] + 31) // 32)]
    p['ds'] = dict((k, u('ds.' + k)) for k in ('flags', 'depth_test', 'depth_write', 'depth_compare', 'depth_bounds', 'stencil_test'))
    st = ('fail', 'pass', 'depth_fail', 'compare', 'compare_mask', 'write_mask', 'reference')
    p['sf'] = tuple(u('ds.stencil.' + k) for k in st); p['sb'] = tuple(u('ds.stencil.' + k) for k in st)
    p['db'] = (u('ds.min_depth_bounds'), u('ds.max_depth_bounds'))
    p['cb'] = (u('cb.flags'), u('cb.logic_op_enable'), u('cb.logic_op')); nb = u('cb.attachments')
    if nb > 8: raise Bad('parse', 'cb.attachments')
    p['blend'] = [tuple(u('cb.' + k) for k in ('blend_enable', 'src_color', 'dst_color', 'color_op', 'src_alpha', 'dst_alpha', 'alpha_op', 'write_mask')) for _ in range(nb)]
    p['bconst'] = [u('cb.blend_constants') for _ in range(4)]
    p['dy_flags'], nd = u('dy.flags'), u('dy.states')
    if nd > 64: raise Bad('parse', 'dy.states')
    p['dyn'] = [u('dy.state') for _ in range(nd)]
    if i != len(w): raise Bad('parse', 'trailing words')
    return p, names


def deps(tile):   # create_pass(), nr_seed.c nrs_pass_deps
    d0 = [0xFFFFFFFF, 0, 0x10000, 0x8000, 0x10000, 0x780, 0]; d1 = [0, 0xFFFFFFFF, 0x8000, 0x10000, 0x500, 0x18000, 0]
    if tile: d0[3], d1[3] = 0x780, 0x1780
    return [tuple(d0), tuple(d1)]


STAGE_INDEX = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4}


def shape(p, keys, pol):
    """nr_seed.c nrs_shape + the key / id / variant / dependency / depth checks (what the runtime refuses whatever the destination)."""
    def need(c, reason, field):
        if not c: raise Bad(reason, field)
    S = (1, 2, 4, 8, 16, 32, 64)
    need(p['l_flags'] == 0 and p['pl_flags'] == 0 and p['sets'] == 1 and not p['push'], 'shape', 'layout')
    for b in p['lb']: need(b[1] <= 8 and 1 <= b[2] <= 128 and b[3] in STAGE_INDEX, 'shape', 'layout.binding')
    need(p['r_flags'] == 0 and p['att'] and p['s_flags'] == 0 and p['s_bind'] == 0 and len(p['dep']) == 2, 'shape', 'pass')
    for a in p['att']:
        need(a['flags'] == 0 and a['samples'] in S, 'shape', 'pass.att')
        need(a['load'] == a['store'] == a['sload'] == a['sstore'] == 0, 'shape', 'pass.att.ops')
        need(a['init'] == a['fin'] == 1, 'shape', 'pass.att.layout')
    used = set()
    for att, lay in p['col']:
        need(lay == 1, 'shape', 'pass.color.layout')
        if att == 0xFFFFFFFF: continue
        need(att < len(p['att']) and att not in used, 'shape', 'pass.color.attachment'); used.add(att)
    if p['depth']:
        need(p['depth'][1] == 1 and p['depth'][0] < len(p['att']) and p['depth'][0] not in used, 'shape', 'pass.depth'); used.add(p['depth'][0])
    need(used == set(range(len(p['att']))), 'shape', 'pass.attachments')
    need(p['flags'] == 0 and p['subpass'] == 0 and len(p['st']) >= 1 and len(p['st']) == len(keys), 'shape', 'ci')
    last, idx = -1, []
    for fl, stg, _ in p['st']:
        s = STAGE_INDEX.get(stg, -1); need(fl == 0 and s > last, 'shape', 'stage'); last = s; idx.append(s)
    need(idx[0] == 0, 'shape', 'stage.vertex')
    need((1 in idx) == (2 in idx) == (p['tess'] is not None) and (p['tess'] is None or p['tess'][0] == 0), 'shape', 'tess')
    need(p['vi_flags'] == 0 and len(p['vb']) <= 16 and len(p['va']) <= 16, 'shape', 'vi')
    for k, b in enumerate(p['vb']): need(b[0] < 16 and (k == 0 or b[0] > p['vb'][k - 1][0]) and b[2] <= 1, 'shape', 'vi.binding')
    locs = set()
    for a in p['va']:
        need(a[0] < 16 and any(b[0] == a[1] for b in p['vb']) and a[2] != 0 and a[0] not in locs, 'shape', 'vi.attr'); locs.add(a[0])
    need(p['ia'][0] == 0 and p['ia'][1] <= 10 and p['ia'][2] <= 1, 'shape', 'ia')
    need(p['vp'][:3] == (0, 1, 1) and p['vp'][3] is None and p['vp'][4] is None, 'shape', 'vp')
    r = p['rs']
    need(r['flags'] == 0 and r['depth_clamp'] <= 1 and r['discard'] <= 1 and r['polygon_mode'] <= 2 and r['cull'] <= 3 and r['front_face'] <= 1 and r['depth_bias'] <= 1
         and r['line_width'] == 0x3F800000, 'shape', 'rs')
    m = p['ms']
    need(m['flags'] == 0 and m['samples'] in S and m['sample_shading'] == 0 and m['min_sample_shading'] == 0 and m['alpha_to_coverage'] <= 1 and m['alpha_to_one'] == 0
         and not m['sample_mask'], 'shape', 'ms')
    for a in p['att']: need(a['samples'] == m['samples'], 'shape', 'ms.samples')
    d = p['ds']
    need(d['flags'] == 0 and d['depth_test'] <= 1 and d['depth_write'] <= 1 and d['depth_compare'] <= 7 and d['depth_bounds'] <= 1 and d['stencil_test'] <= 1
         and all(x <= 7 for x in p['sf'][:4] + p['sb'][:4]), 'shape', 'ds')
    need(p['cb'][0] == 0 and p['cb'][1] <= 1 and p['cb'][2] <= 15 and len(p['blend']) == len(p['col']), 'shape', 'cb')
    for b in p['blend']: need(b[0] <= 1 and all(x <= 18 for x in (b[1], b[2], b[4], b[5])) and b[3] <= 4 and b[6] <= 4 and b[7] <= 15, 'shape', 'cb.attachment')
    need(p['dy_flags'] == 0 and p['dyn'] == [0, 1], 'shape', 'dy')
    for k, (fl, stg, mid) in zip(keys, p['st']):
        need(key_ok(k), 'key', 'stage key')
        need(fnv(k) == mid, 'module-id', 'stage.module')
        if FUSED.match(k): need(STAGE_INDEX[stg] == 4, 'variant', 'fused variant')
        if '#at' in k: need(STAGE_INDEX[stg] == 4, 'variant', 'alpha variant')
    if pol is None: return
    for k, (fl, stg, mid) in zip(keys, p['st']):
        if '#vi' in k: need(STAGE_INDEX[stg] == 0 and pol['retype'] and len(k) == 27, 'variant', 'vertex variant')
    need(p['dep'] == deps(pol['tile_barrier']), 'dependencies', 'pass.dependencies')
    if p['depth']:
        f = p['att'][p['depth'][0]]['format']
        need(not ((f == 125 and pol['depth16_as_d24']) or (f == 126 and pol['x8d24_as_d32']) or (f == 130 and pol['d24s8_as_d32s8'])), 'depth-ambiguous', 'pass.depth.format')
    for a in p['va']:
        for g in range(1, 256):
            if pol['vtx_unsup'][g >> 5] >> (g & 31) & 1 and WIDEN.get(g) == a[2]: raise Bad('vertex-ambiguous', 'vi.attr.format')


WIDEN = {23: 37, 24: 38, 27: 41, 28: 42, 84: 91, 85: 92, 88: 95, 89: 96, 90: 97, 104: 107, 105: 108, 106: 109}   # xbg_vertex_widen


XGS_MAGIC, XGF_MAGIC, PACK_MAGIC = 0x31534758, 0x31464758, 0x50534758
SRC_SUPPORT, SRC_UNIFORM, SRC_DRIVER0, SRC_DRIVER2, SRC_TEXTURE = 1, 2, 5, 6, 7
NB_FX_K_SUPPORT, NB_FX_K_CBUF, NB_FX_K_TEX, SPARE_TEX, SPARE_UBO = 1, 2, 7, 120, 15   # nb_fused.h, xbg_nb_post.inc


def fused_bundle(blob):
    """The XGS1 bundle of an XGF1 fused bundle with its extra bindings' source / slot rewritten as xbg_nb_post.inc compose() does; None if not one."""
    if len(blob) < 112 or struct.unpack_from('<2I', blob, 0) != (XGF_MAGIC, 1): return None
    n_extra, n_req, bsize = struct.unpack_from('<3I', blob, 16)
    o = 112 + 24 * n_extra + 32 * n_req
    if o + bsize != len(blob) or bsize < 32: return None
    b = bytearray(blob[o:o + bsize]); h = struct.unpack_from('<8I', b, 0)
    if h[0] != XGS_MAGIC or h[1] != 1 or h[3] < n_extra: return None
    host, nt, nu = h[3] - n_extra, 0, 0
    for i in range(n_extra):
        kind = blob[112 + 24 * i + 14]
        if kind == NB_FX_K_TEX: src, slot = SRC_TEXTURE, SPARE_TEX + nt; nt += 1
        elif kind == NB_FX_K_CBUF: src, slot = SRC_UNIFORM, SPARE_UBO - nu; nu += 1
        else: src, slot = SRC_SUPPORT, 0
        struct.pack_into('<2I', b, 32 + 32 * (host + i) + 16, src, slot)
    return bytes(b)


class Bundles:
    """Bundle headers (binding tables, inputs) and fragment facts by stage key: <dir>/<key>.xgs, fused keys from the post pack."""
    def __init__(self, bdir, post_pack=''):
        self.dir, self.cache, self.post = Path(bdir) if bdir else None, {}, {}
        if post_pack and Path(post_pack).exists():
            d = Path(post_pack).read_bytes(); magic, ver, n, _ = struct.unpack_from('<4I', d, 0)
            if magic == PACK_MAGIC and ver == 1:
                for i in range(n):
                    k, o, sz, _, _ = struct.unpack_from('<32sQIIQ', d, 16 + 56 * i)
                    fb = fused_bundle(d[o:o + sz])
                    if fb: self.post[k.rstrip(b'\0').decode()] = fb

    def raw(self, key):
        if FUSED.match(key): return self.post.get(key)
        if not self.dir: return None
        f = self.dir / (key.lower() + '.xgs')
        return f.read_bytes() if f.exists() else None

    def get(self, key):
        """(stage, [(binding, type, count, source, slot)], fragment (outputs, sample_mask) or None); None = no bundle."""
        if key in self.cache: return self.cache[key]
        b, r = self.raw(key), None
        if b and len(b) >= 32:
            h = struct.unpack_from('<8I', b, 0)
            if h[0] == XGS_MAGIC and h[1] == 1 and h[3] <= 256 and len(b) >= 32 + 32 * h[3]:
                binds = [(lambda x: (x[0], x[1], x[2], x[4], x[5]))(struct.unpack_from('<8I', b, 32 + 32 * i)) for i in range(h[3])]
                r = (h[2], binds, fs_facts(b, h) if h[2] == 4 else None)
        self.cache[key] = r
        return r


def fs_facts(b, h):
    """xbg_spirv_raster_facts for a fragment shader: (output locations mask, SampleMask output) from the bundle's SPIR-V; None if unreadable."""
    off = 32 + 32 * h[3] + 4 * h[4] + h[5]
    if h[6] % 4 or off + h[6] > len(b) or h[6] < 20: return None
    w = struct.unpack_from('<%dI' % (h[6] // 4), b, off)
    if w[0] != 0x07230203: return None
    loc, sm, outv, i = {}, set(), [], 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if not n or i + n > len(w): return None
        if op == 71 and n >= 4:   # OpDecorate: Location / BuiltIn SampleMask
            if w[i + 2] == 30: loc[w[i + 1]] = w[i + 3]
            if w[i + 2] == 11 and w[i + 3] == 20: sm.add(w[i + 1])
        if op == 59 and n >= 4 and w[i + 3] == 3: outv.append(w[i + 2])   # OpVariable ... Output
        i += n
    mask = 0
    for v in outv:
        if v in loc and loc[v] < 32: mask |= 1 << loc[v]
    return mask, any(v in sm for v in outv)


def split_eligible(shaders, lim):   # xbg_nr_desc.inc split_pick: true = dyn_mode_for returns 3 (dynamic blocks picked by run-time counts)
    n = 0
    for _, bs in shaders:
        for b in bs:
            if b[1] != 6: continue
            if b[3] in (SRC_DRIVER0, SRC_DRIVER2): return False
            if b[3] != SRC_UNIFORM: continue
            if b[2] != 1 or b[4] >= 16 or n >= 16: return False
            n += 1
    return lim > 0 and n > lim


def layout_of(shaders, pol):   # descriptors(): dyn_mode_for, UBO_DYN, the key loop (nr_seed.c nsd_layout); shaders = [(stage, bindings)]
    allc = sum(b[2] for _, bs in shaders for b in bs if b[1] == 6)
    vary = sum(b[2] for _, bs in shaders for b in bs if b[1] == 6 and b[3] in (2, 5, 6))
    lim = pol['ubo_dyn_limit']
    if pol['ubo_dynamic'] and pol.get('split') and vary > lim and not (allc and allc <= lim) and split_eligible(shaders, lim):
        raise Bad('split', 'NR-08 split layout (dynamic blocks picked at run time)')
    mode = 0 if not pol['ubo_dynamic'] else 1 if allc and allc <= lim else 2 if vary and vary <= lim and pol['dyn_partial'] else 0
    out = []
    for stage, bs in shaders:
        for b in bs:
            dyn = b[1] == 6 and (mode == 1 or (mode == 2 and b[3] in (2, 5, 6)))
            out.append((b[0], 8 if dyn else b[1], b[2], 1 << stage))
    return out, allc, mode


def policy_from_log(text):
    """The device's policy from its dm.log (the lines xbg_game.cpp writes at start-up)."""
    p = dict(PRESETS['turnip']); p['vtx_unsup'] = [0] * 8; seen = []
    def grab(rx):
        m = re.search(rx, text); return m
    if (m := grab(r'tile pass policy.*?DM_GFX_TILE_BARRIER=(\d)')): p['tile_barrier'] = int(m.group(1)); seen.append('tile')
    if (m := grab(r'NVN depth formats: D24S8 (\S+).*?X8_D24 (\S+)')): p['d24s8_as_d32s8'] = int(m.group(1) != 'native'); p['x8d24_as_d32'] = int(m.group(2) != 'native'); seen.append('depth')
    if (m := grab(r'NVN depth16 stored as (\S+)')): p['depth16_as_d24'] = int(m.group(1) == 'X8_D24'); seen.append('d16')
    if (m := grab(r'depth_bias_x2=(\d)')): p['bias_x2'] = int(m.group(1)); seen.append('bias')
    if (m := grab(r'NVN gpu policy:.*?ubo_dynamic=(\d) noout_mask=(\d)')): p['ubo_dynamic'] = int(m.group(1)); p['noout'] = int(m.group(2)); seen.append('policy')
    if (m := grab(r'NVN caps limits .*?ubDyn=(\d+)')): p['ubo_dyn_limit'] = int(m.group(1)); seen.append('ubDyn')
    if (m := grab(r'DM_GFX_VTX_RETYPE=(\d)')): p['retype'] = int(m.group(1)); seen.append('retype')
    if (m := grab(r'DM_GFX_ALPHA_TEST=(\d)')): p['alpha_test'] = int(m.group(1)); seen.append('alpha')
    p['split'] = int(bool(re.search(r'\[nr\] DM_NR_DESC=1 \(NR-08: on\).*?DM_NR_DESC_SPLIT=1', text))); seen.append('split')
    p['ro'] = int(bool(re.search(r'\[nr\] DM_NR_UBODYN=\d \(NR-08b: on;', text))); seen.append('ro')
    if (m := grab(r'NVN caps features (.*)')):
        f = dict(re.findall(r'(\w+)=(\d)', m.group(1))); feat = 0
        for name, bit in (('fillModeNonSolid', F_NONSOLID), ('depthClamp', F_DCLAMP), ('depthBounds', F_DBOUNDS), ('logicOp', F_LOGICOP)):
            if f.get(name) == '1': feat |= bit
        feat |= F_BCLAMP if 'no-depth-bias-clamp' not in text else 0   # the fallbacks line names a missing depthBiasClamp
        p['features'] = feat; seen.append('features')
    for line in re.findall(r'NVN caps formats (.*)', text):   # "<vkformat>:<optimal>/<buffer>": VERTEX_BUFFER = 0x40
        for fmt, buf in re.findall(r'(\d+):0x[0-9a-f]+/0x([0-9a-f]+)', line):
            f = int(fmt)
            if f < 256 and not (int(buf, 16) & 0x40): p['vtx_unsup'][f >> 5] |= 1 << (f & 31)
        seen.append('formats')
    return p, sorted(set(seen))


def policies_from_log(text):
    """[(policy, seen)] per boot of a dm.log (a run may hold several: a safe-mode ladder restarts with other switches); a boot starts at its
    "NVN gpu family=" line."""
    at = [m.start() for m in re.finditer(r'NVN gpu family=', text)]
    if len(at) <= 1: return [policy_from_log(text)]
    return [policy_from_log(text[a:b]) for a, b in zip(at, at[1:] + [len(text)])]


POLICY_KEYS = POLICY_FIELDS[:13] + ['vtx_unsup', 'split', 'ro']


def policy_words(p):
    return [p[k] for k in POLICY_FIELDS[:13]] + list(p['vtx_unsup'])


def policy_text(p):
    feats = '+'.join(n for n, b in (('nonsolid', 1), ('dclamp', 2), ('bclamp', 4), ('dbounds', 8), ('logicop', 16)) if p['features'] & b) or 'none'
    nun = sum(bin(x).count('1') for x in p['vtx_unsup'])
    return (f"tile_barrier={p['tile_barrier']} d24s8->d32s8={p['d24s8_as_d32s8']} x8d24->d32={p['x8d24_as_d32']} d16->x8d24={p['depth16_as_d24']} bias_x2={p['bias_x2']} "
            f"ubo_dynamic={p['ubo_dynamic']} limit={p['ubo_dyn_limit']} partial={p['dyn_partial']} noout={p['noout']} retype={p['retype']} alpha_test={p['alpha_test']} "
            f"features={feats} vertex formats without fetch={nun}" + (f" split={p.get('split', 0)} read-only={p.get('ro', 0)}" if 'split' in p else ''))


def record_bytes(keys, w):   # warm::record's format
    out = struct.pack('<2I', REC_MAGIC, len(keys))
    for k in keys: kb = k.encode('ascii'); out += struct.pack('<I', len(kb)) + kb
    return out + struct.pack('<I', len(w)) + struct.pack('<%dI' % len(w), *w)


def stage_shaders(keys, p, B):
    sh = []
    for k, (_, stg, _) in zip(keys, p['st']):
        r = B.get(bundle_key(k))
        if r is None: raise Bad('shader', 'bundle ' + bundle_key(k))
        if r[0] != STAGE_INDEX[stg]: raise Bad('shader', 'bundle stage')
        sh.append(r)
    return sh


def layout_fits(rec, sh):
    """The recorded layout holds the bundles' bindings in order, uniform buffers static or dynamic (any device's rule)."""
    raw = [b for _, bs, _ in sh for b in bs]
    stages = [1 << st for st, bs, _ in sh for _ in bs]
    if len(rec) != len(raw): return False
    for r, b, s in zip(rec, raw, stages):
        if (r[0], r[2], r[3]) != (b[0], b[2], s): return False
        if r[1] != b[1] and not (b[1] == 6 and r[1] == 8): return False
    return True


DEPTH_KEYS = ('d24s8_as_d32s8', 'x8d24_as_d32', 'depth16_as_d24')


def depth_back(pol, f):   # nr_seed.c depth_back / depth_fwd
    return {125: 0 if pol['depth16_as_d24'] else 125, 126: 0 if pol['x8d24_as_d32'] else 126, 130: 0 if pol['d24s8_as_d32s8'] else 130}.get(f, f)


def depth_fwd(pol, f):
    return {124: 125 if pol['depth16_as_d24'] else 124, 125: 126 if pol['x8d24_as_d32'] else 125, 129: 130 if pol['d24s8_as_d32s8'] else 129}.get(f, f)


def f32(b): return struct.unpack('<f', struct.pack('<I', b))[0]
def b32(f): return struct.unpack('<I', struct.pack('<f', f))[0]


def retarget(keys, w, p, names, src, dst, B):
    """One record from the policy it was recorded under (src) to the seed's source policy (dst), the inverse of what src changed and
    the forward of what dst does (nsd_expand's derivations). -> (keys, words, notes); notes: 'mask-unknown' (src NOOUT masked the colour
    writes of a fragment shader without outputs), 'relayout' (the recorded layout is not dst's model), 'lossy' (dst has a feature src lacks:
    its field stays as recorded)."""
    at = collections.defaultdict(list)
    for i, n in enumerate(names): at[n].append(i)
    w, notes = list(w), set()
    sh = stage_shaders(keys, p, B)
    if not layout_fits(p['lb'], sh): raise Bad('layout', 'bindings')
    lay, _, _ = layout_of([(s, bs) for s, bs, _ in sh], dst)
    for i, b in zip(at['layout.binding.type'], lay): w[i] = b[1]
    if [tuple(x) for x in lay] != [tuple(x) for x in p['lb']]: notes.add('relayout')
    dw = [v for d in deps(dst['tile_barrier']) for v in d]
    dep_at = [i for i, n in enumerate(names) if n.startswith('pass.dep.')]
    for i, v in zip(dep_at, dw): w[i] = v
    # depth stand-ins
    d16 = False
    if p['depth']:
        fi = at['pass.att.format'][p['depth'][0]]; f = w[fi]
        if any(src[k] != dst[k] for k in DEPTH_KEYS):
            g = depth_back(src, f)
            if not g: raise Bad('depth-ambiguous', 'pass.depth.format')
            w[fi] = depth_fwd(dst, g); d16 = g == 124
        else: d16 = f == 124 or (f == 125 and src['depth16_as_d24'])
    # vertex formats: the same fetch support keeps them; otherwise only formats neither side could have widened
    if src['vtx_unsup'] != dst['vtx_unsup']:
        for i in at['vi.attr.format']:
            f = w[i]
            if any(src['vtx_unsup'][g >> 5] >> (g & 31) & 1 and WIDEN.get(g) == f for g in range(1, 256)): raise Bad('vertex-ambiguous', 'vi.attr.format')
            if f < 256 and dst['vtx_unsup'][f >> 5] >> (f & 31) & 1: raise Bad('vertex-format', 'vi.attr.format')
    # stage keys: the vertex variant (retype) of dst
    keys = list(keys)
    if '#vi' in keys[0]:
        if '#vi' in keys[0] and len(keys[0]) != 27: raise Bad('variant', 'vertex conversion variant')
        if not dst['retype']: keys[0] = keys[0].split('#')[0]
        elif src['vtx_unsup'] != dst['vtx_unsup']: raise Bad('variant', 'vertex variant')
    elif dst['retype'] and not src['retype']: raise Bad('variant', 'retype (input classes unknown here)')
    for s, (k, i) in enumerate(zip(keys, at['stage.module'][0::2])):
        m = fnv(k); w[i], w[i + 1] = m & 0xFFFFFFFF, m >> 32
    # rasterizer / depth / blend features, depth-bias scale
    for feat, field, zero in ((F_NONSOLID, 'rs.polygon_mode', 0), (F_DCLAMP, 'rs.depth_clamp', 0), (F_DBOUNDS, 'ds.depth_bounds', 0), (F_LOGICOP, 'cb.logic_op_enable', 0)):
        if not dst['features'] & feat and w[at[field][0]]:
            w[at[field][0]] = zero
            if feat == F_LOGICOP: w[at['cb.logic_op'][0]] = 0
        elif dst['features'] & feat and not src['features'] & feat: notes.add('lossy')
    if not dst['features'] & F_BCLAMP and f32(w[at['rs.depth_bias_clamp'][0]]) != 0.0: w[at['rs.depth_bias_clamp'][0]] = 0
    ss = (256.0 if src['depth16_as_d24'] and d16 else 1.0) * (2.0 if src['bias_x2'] else 1.0)
    ds = (256.0 if dst['depth16_as_d24'] and d16 else 1.0) * (2.0 if dst['bias_x2'] else 1.0)
    if ss != ds: i = at['rs.depth_bias_constant'][0]; w[i] = b32(f32(w[i]) / ss * ds)
    # NOOUT masks
    if p['blend'] and src['noout'] != dst['noout']:
        fs = next((r for r, (_, stg, _) in zip(sh, p['st']) if STAGE_INDEX[stg] == 4), None)
        facts = fs[2] if fs else (0, False)
        if facts is None: raise Bad('facts', 'fragment outputs')
        if not facts[0] and not facts[1]:
            if src['noout']: notes.add('mask-unknown')
            else:
                for i in at['cb.write_mask']: w[i] = 0
    return keys, w, notes


def cmd_build(a):
    logs, pols = a.log or [], []
    for path in logs:
        boots = policies_from_log(Path(path).read_text(encoding='utf-8', errors='replace'))
        for b, (q, seen) in enumerate(boots):
            if any(all(q[k] == o[k] for k in POLICY_KEYS) for o in pols): continue
            q['name'] = Path(path).name + (f' boot {b + 1}' if len(boots) > 1 else ''); pols.append(q)
            print(f'{"source policy" if len(pols) == 1 else "also recorded under"} {q["name"]} ({", ".join(seen)})\n  ' + policy_text(q))
    if not logs: pols = [dict(PRESETS[a.policy], name='preset ' + a.policy)]; print(f'source policy: preset {a.policy}\n  ' + policy_text(pols[0]))
    if a.policy_json: pols[0].update(json.loads(Path(a.policy_json).read_text()))
    pol = pols[0]
    if pol.get('ro'): raise SystemExit('the source policy runs NR-08b read-only layouts (a PowerVR-class log): take the seed from a device without them')
    B = Bundles(a.bundles, a.post_pack) if a.bundles else None
    if B and a.post_pack: print(f'  fused post bundles: {len(B.post)} from {a.post_pack}')
    why, fields, seen, n_in = collections.Counter(), collections.Counter(), set(), 0
    out, canon, unknown = [], {}, []   # canonical record bytes in order; bytes -> index; mask-unknown records
    via, notes_n = collections.Counter(), collections.Counter()
    for path in a.list:
        recs, clean = records(Path(path).read_bytes())
        print(f'{path}: {len(recs)} records' + ('' if clean else ' (stopped at a damaged record, as the engine does)'))
        for keys, w, raw in recs:
            n_in += 1
            if raw in seen: why['duplicate'] += 1; continue
            seen.add(raw)
            try:
                p, names = decode(w); shape(p, keys, None)
                if not B:   # no bundles: only records of the source policy as they are
                    shape(p, keys, pol); out.append(raw); canon[raw] = len(out) - 1; via[pol['name']] += 1; continue
                cand = [q for q in pols if p['dep'] == deps(q['tile_barrier'])]
                if not cand: raise Bad('dependencies', 'pass.dependencies (no --log policy has them: add the dm.log of the run that recorded it)')
                results, last = {}, None
                for q in cand:
                    try:
                        shape(p, keys, q); k2, w2, nt = retarget(keys, w, p, names, q, pol, B)
                        results.setdefault(record_bytes(k2, w2), (q, k2, w2, nt))
                    except Bad as e: last = e
                if not results: raise last
                if len(results) > 1: raise Bad('policy', 'the policies it may come from re-target it differently')
                rb, (q, k2, w2, nt) = next(iter(results.items()))
            except Bad as e:
                why[e.reason] += 1; fields[f'{e.reason}: {e.field}'] += 1; continue
            for n in nt: notes_n[n] += 1
            if 'mask-unknown' in nt: unknown.append((rb, k2, w2, q)); continue
            if rb in canon: why['duplicate (re-targeted)'] += 1; continue
            canon[rb] = len(out); out.append(rb); via[q['name']] += 1
    if unknown:   # NOOUT records of shaders without outputs: the same draw recorded with its masks makes them duplicates
        def wild(k2, w2):
            _, nm = decode(w2); return (tuple(k2), tuple(0 if n == 'cb.write_mask' else v for n, v in zip(nm, w2)))
        known = {wild(*(lambda r: (r[0], r[1]))(parse_rec(rb))) for rb in out}
        for rb, k2, w2, q in unknown:
            if wild(k2, w2) in known: why['duplicate (re-targeted)'] += 1
            else: why['noout'] += 1; fields['noout: cb.write_mask (masked by the recording device, the game masks unknown)'] += 1
    body = b''.join(out); kept = len(out)
    pw = policy_words(dict(pol, unmodelled=0))
    hdr = struct.pack('<8I', SEED_MAGIC, SEED_VERSION, 8 + len(pw), kept, 0, 0, 0, len(pw)) + struct.pack('<%dI' % len(pw), *pw)
    sid = fnv64(body)
    hdr = hdr[:20] + struct.pack('<2I', sid & 0xFFFFFFFF, sid >> 32) + hdr[28:]
    Path(a.out).write_bytes(hdr + body)
    print(f'{a.out}: {kept} records of {n_in} ({len(hdr) + len(body)} bytes, id {sid:016x}); left out: ' +
          (', '.join(f'{k} {v}' for k, v in why.most_common()) or 'nothing'))
    for k, v in fields.most_common(12): print(f'  refused {v:5d}  {k}')
    if B:
        print('  kept records by the first policy that gives them: ' + ', '.join(f'{k} {v}' for k, v in via.most_common()))
        print(f'  re-targeted: layouts re-derived (recorded under another uniform-buffer rule / build) {notes_n["relayout"]}, fields the recording '
              f'device could not record (a feature it lacks; kept as recorded) {notes_n["lossy"]}, NOOUT masks unknown {notes_n["mask-unknown"]}')
        bad = 0   # every kept record = what the runtime's source check expects (nsd_parse / nsd_shape / layout model / dependencies)
        for rb in out:
            k2, w2 = parse_rec(rb); p2, _ = decode(w2)
            try: shape(p2, k2, pol); bad += [tuple(x) for x in layout_of([(s, bs) for s, bs, _ in stage_shaders(k2, p2, B)], pol)[0]] != [tuple(x) for x in p2['lb']]
            except Bad: bad += 1
        print(f'  self-check: {kept - bad} of {kept} kept records have the shape, dependencies and layout the runtime checks a seed with')
        if bad: return 1
    return 0 if kept else 1


def parse_rec(rb):
    recs, _ = records(rb); return recs[0][0], recs[0][1]


def read_seed(path):
    b = Path(path).read_bytes()
    if len(b) < 32: raise SystemExit(f'{path}: too short')
    magic, ver, hw, nrec, flags, idlo, idhi, pw = struct.unpack_from('<8I', b, 0)
    if magic != SEED_MAGIC or ver != SEED_VERSION or pw != len(POLICY_FIELDS) or hw != 8 + pw: raise SystemExit(f'{path}: not a version-{SEED_VERSION} seed')
    words = struct.unpack_from('<%dI' % pw, b, 32)
    pol = dict(zip(POLICY_FIELDS[:13], words[:13])); pol['vtx_unsup'] = list(words[13:])
    body = b[4 * hw:]
    if fnv64(body) != (idlo | idhi << 32): raise SystemExit(f'{path}: damaged (id differs)')
    recs, clean = records(body)
    if not clean or len(recs) != nrec: raise SystemExit(f'{path}: damaged records')
    return pol, recs, len(b), 4 * hw


def cmd_audit(a):
    pol, recs, size, hlen = read_seed(a.seed)
    cls = collections.Counter(); bad = 0
    for keys, w, raw in recs:
        try:
            p, names = decode(w); shape(p, keys, pol)
        except Bad as e:
            bad += 1; print(f'  record refused: {e.reason} {e.field}'); continue
        cls['record framing (magic, counts, key lengths)'] += 4 * (3 + len(keys))
        cls['shader pack keys (hash identity: 16 hex + alpha variant / fused post prefix)'] += sum(len(k) for k in keys)
        for n in names:
            c = ('module ids (warm::fnv of the keys)' if n == 'stage.module' else
                 'formats (Vulkan enums)' if n.endswith('format') else
                 'counts / lengths of the create-info pieces' if n in ('layout.words', 'pass.words') or n.endswith(('bindings', 'attributes', 'attachments', 'colors', 'stages', 'states', 'dependencies')) else
                 'fixed-function state words')
            cls[c] += 4
    total = sum(cls.values()) + hlen
    print(f'{a.seed}: {size} bytes = header {hlen} (magic, version, record count, id, source policy: {len(POLICY_FIELDS)} numbers) + {len(recs)} records')
    for k, v in cls.most_common(): print(f'  {v:9d} B  {k}')
    print(f'  every byte accounted for: {total == size}; records refused by the decoder: {bad}')
    print('  no shader code / SPIR-V / game file bytes: each word above is a decoded field of a Vulkan create-info slot or a key string')
    return 0 if total == size and not bad else 1


def cmd_dump(a):
    pol, recs, size, hlen = read_seed(a.seed)
    print(f'{a.seed}: {len(recs)} records; source policy: {policy_text(pol)}')
    for i, (keys, w, _) in enumerate(recs[:a.limit]):
        p, _ = decode(w)
        att = ','.join(str(x['format']) for x in p['att'])
        print(f'{i:5d} {"+".join(keys)} att[{att}] samples {p["ms"]["samples"]} vfmt[{",".join(str(v[2]) for v in p["va"])}] layout {len(p["lb"])} bindings')
    return 0


def cmd_diff(a):
    ra, _ = records(Path(a.a).read_bytes()); rb, _ = records(Path(a.b).read_bytes())
    base = lambda ks: tuple(k.split('#')[0] for k in ks)
    ib = collections.defaultdict(list)
    for keys, w, _ in rb: ib[base(keys)].append((keys, w))
    diffs, keydiff, pairs, same, nolen = collections.Counter(), collections.Counter(), 0, 0, 0
    for keys, w, _ in ra:
        for kb, wb in ib.get(base(keys), [])[:1]:
            pairs += 1
            if kb != keys: keydiff['stage key variants differ'] += 1
            if w == wb: same += 1; continue
            if len(w) != len(wb): nolen += 1; continue
            _, names = decode(w)
            for n in sorted({names[i] for i in range(len(w)) if w[i] != wb[i]}): diffs[n] += 1
    print(f'{a.a}: {len(ra)} records, {a.b}: {len(rb)}; {pairs} share their base stage keys: {same} identical, {nolen} of another length; fields that differ:')
    for k, v in (diffs + keydiff).most_common(): print(f'  {v:5d}  {k}')
    return 0


def cmd_policy(a):
    boots = policies_from_log(Path(a.log).read_text(encoding='utf-8', errors='replace'))
    if not 0 <= a.boot < len(boots): raise SystemExit(f'{a.log}: {len(boots)} boot(s)')
    pol, seen = boots[a.boot]
    if a.words:   # the 21 header words + split + read-only (nr_seed_test --real-seed POLICY)
        print(' '.join(str(v) for v in policy_words(pol) + [pol['split'], pol['ro']])); return 0
    print(f'{a.log}' + (f' boot {a.boot + 1} of {len(boots)}' if len(boots) > 1 else '') + f' ({", ".join(seen)}): {policy_text(pol)}')
    print(json.dumps({k: pol[k] for k in POLICY_FIELDS[:13]} | {'vtx_unsup': pol['vtx_unsup']}))
    return 0


def cmd_pso(a):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import dos2_nr_pso as pso
    cat, keys = pso.catalogue(a.pak), pso.programs(a.programs)
    pairs = collections.Counter()
    for path, var in cat.items():
        k = keys.get(path)
        if k and k[0] != '-' and k[1] != '-': pairs[(k[0].lower(), k[1].lower())] += len(var)
    recs, _ = records(Path(a.list).read_bytes())
    have = collections.defaultdict(list)
    for keys_, w, _ in recs:
        b = [k.split('#')[0].lower() for k in keys_]
        if len(b) >= 2: have[(b[0], b[-1])].append(w)
    kind = {}
    for path, var in cat.items():
        k = keys.get(path)
        if k and k[0] != '-' and k[1] != '-':
            m = pso.KIND.search(path[:-len('_NX.bshd')]); kind[(k[0].lower(), k[1].lower())] = m.group(1) if m else 'other'
    def inputs(vs):   # the VS bundle's input locations (header), the part of the vertex layout a template must share
        f = Path(a.bundles) / (vs + '.xgs')
        if not f.exists(): return None
        h = struct.unpack_from('<8I', f.read_bytes()[:32]); b = f.read_bytes()[32 + 32 * h[3]:32 + 32 * h[3] + 4 * h[4]]
        return tuple(sorted(struct.unpack_from('<%dI' % h[4], b))) if h[4] else ()
    by_vs = collections.defaultdict(int); templ = collections.Counter()
    for (vs, fs) in have:
        by_vs[vs] += 1
        if (vs, fs) in kind: templ[(kind[(vs, fs)], inputs(vs))] += 1
    unc = [(p, n) for p, n in pairs.items() if p not in have]
    tvs = [(p, n) for p, n in unc if by_vs.get(p[0])]
    tk = [(p, n) for p, n in unc if templ.get((kind.get(p), inputs(p[0])))]
    nvs = len({v for v, f in pairs})
    print(f'iPad catalogue: {len(pairs)} (VS, FS) pairs ({nvs} distinct VS: a material has its own pair), {sum(pairs.values())} pipelines; the list: {len(have)} pairs, '
          f'of them in the catalogue {sum(1 for p in pairs if p in have)}')
    print(f'uncovered pairs {len(unc)} ({sum(n for _, n in unc)} iPad pipelines); with the same VS in the list {len(tvs)}; with a listed material of the same kind '
          f'(DEF / DEP / FOR) and the same vertex input locations {len(tk)} ({sum(n for _, n in tk)} iPad pipelines) - a template would GUESS their blend, depth, '
          f'raster, render-target and vertex formats from it; .pso names carry no state')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build'); b.add_argument('--list', nargs='+', required=True); b.add_argument('--out', required=True)
    b.add_argument('--log', nargs='+', default=[]); b.add_argument('--policy', default='turnip', choices=sorted(PRESETS)); b.add_argument('--policy-json', default='')
    b.add_argument('--bundles', default=''); b.add_argument('--post-pack', default='')
    s = sub.add_parser('audit'); s.add_argument('seed')
    d = sub.add_parser('dump'); d.add_argument('seed'); d.add_argument('--limit', type=int, default=40)
    f = sub.add_parser('diff'); f.add_argument('a'); f.add_argument('b')
    p = sub.add_parser('policy'); p.add_argument('--log', required=True); p.add_argument('--words', action='store_true'); p.add_argument('--boot', type=int, default=0)
    root = Path(__file__).resolve().parent.parent
    o = sub.add_parser('pso'); o.add_argument('--list', required=True)
    o.add_argument('--pak', default=str(root.parent / 'work' / 'ipa' / 'Payload' / 'Divinity Original Sin 2.app' / 'Data' / 'ShaderCache.pak'))
    o.add_argument('--programs', default=str(root.parent / 'work' / 'shaders-jobs' / 'programs.tsv'))
    o.add_argument('--bundles', default=str(root.parent / 'work' / 'shaders-vk'))
    a = ap.parse_args()
    return {'build': cmd_build, 'audit': cmd_audit, 'dump': cmd_dump, 'diff': cmd_diff, 'policy': cmd_policy, 'pso': cmd_pso}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
