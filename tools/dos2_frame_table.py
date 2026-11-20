#!/usr/bin/env python3
"""DOS2 NR-FRAME: the real frame as a table (docs/dos2/nr/NR-FRAME.md) and the lossless restructure candidates it holds.

Inputs (all optional except the dump):
  dump           an NB IR dump (DM_NB_DUMP=1: nb_ir_*_p*.txt; grammar tools/dos2_nb_check.py) - pass order, attachments by storage
                 address, the planner's load / store per attachment, folded clears, sampled reads, draws, first fragment shader
  --pipes F      a warm-up list (files/dm/vk-pipeline-list.bin): per fragment shader the colour formats, blend, write mask and the
                 depth / stencil state the device built (tools/dos2_pipe_seed.py decode) - what the cover proof (NR-10b) sees
  --heat F       a shader-heat.tsv with P rows (DM_NR_HEAT=2): median GPU us per (fragment shader, size), and the pass size
Model (labelled as such in the output):
  tiler   bytes = load (plan LOAD) + store (plan STORE) of every attachment at its real bytes per pixel (no AFBC / UBWC; x0.5-0.7 with it)
  sysmem  (Turnip TU_DEBUG=sysmem, the Mi 14) load / store ops move nothing; a CLEAR load op is a 2D-engine clear of the render area
          (tu_clear_sysmem_attachment: blit + CCU clean / invalidate + WFI), a draw writes its attachments (area x bpp, full-screen
          passes), a sampled read reads the texture (area x bpp of the image, once)
Candidates (each is reported with its pass, attachment and MB; "exact" = the rule is a proof, not a heuristic):
  CLRDC   a folded CLEAR of a colour attachment whose pass's first draw is one exact full-screen program (cover-provable) that writes
          every channel of the format without blending: CLEAR -> DONT_CARE (sysmem: one clear blit + WFI less; tiler: ~0)
  FMASK   a plan LOAD inside cover_dc that NR-10b refuses only because the write mask is not 0xF although it covers every channel the
          format has (R11G11B10F 0x7, R8 / R16 0x1, RG16 0x3): load DONT_CARE with the format-aware mask rule (tiler: the load)
  DSIDLE  a depth-stencil attached to a pass whose draws (by the pipeline list) neither test, write nor stencil: detach (tiler: DS load
          + store of the pass); NR-UIX does this for Iggy passes only
  DEADST  a store STORE whose image's next event is a full clear / CLEAR / DONT_CARE load (should be DONT_CARE; tiler: the store)
Usage: dos2_frame_table.py DUMP [--pipes vk-pipeline-list.bin] [--heat shader-heat.tsv] [--md] [--all DIR]   |   --self-test
"""
import argparse, collections, re, statistics, sys
from pathlib import Path

NAMES = {
    'f23c09d198b55dc5': 'LinearizeDepth', '640a4c497bcc748b': 'DeferredSky', 'd236b8d5b7ffad3c': 'ClusteredLightingWithSSAO',
    'd8aeec640d465275': 'DeferredFog', '62d683cc49953ad6': 'FadeBlending', '1372db843749e1d3': 'CascadedPCF_3x768',
    '2a221ee245d55aab': 'CascadedPCF_3x512', 'a758e42af8853343': 'CascadedPCF_2x1024', 'cbe8649300e9c8cd': 'PPDownSampleLog2',
    '7b3210191abd90cd': 'PPDefault', 'f3fd8e1015789f30': 'LuminanceAccum', '74fe9ae2066d53da': 'PPBloomScaleBiasDownsampleAndBlur',
    '0e3fc07b320db1e9': 'PPBloomDownsampleAndBlur', '4e290c9cf1162fe9': 'PPBloomUpsampleAndBlur', '921728a5bbb1c71b': 'PPApplyLUT',
    '91eb2d97a304709a': 'EoCSight', 'e94ea226e689801c': 'EoCFog', '691768aa08545d95': 'PPFXAA', '98186c22a7209861': 'CombineUI_NoFetch',
    '343488601475d44d': 'CrossBilateralUpsample', 'facf8294de39f566': 'DepthDownsample(FragDepth)', '010472595b342188': 'EoCOutlineCombine',
    '43e8e8ed': 'PPVignette', 'd99f1fdb55bbafd0': 'Iggy', 'b5afd82fadffd7db': 'Iggy fill', '5e26ce730b83399a': 'Iggy blur',
    '9c8e09d06f456fdf': 'FadePattern (depth pre-pass)', 'eaaa50c5ddbfd31b': 'G-buffer material',
}
# NVN format -> (bytes per pixel, components); depth formats marked by name
NVN = {0x1: (1, 1, 'R8'), 0x5: (2, 1, 'R16F'), 0x6: (2, 1, 'R16'), 0xA: (4, 1, 'R32F'), 0xD: (2, 2, 'RG8'), 0x11: (4, 2, 'RG16F'),
       0x12: (4, 2, 'RG16'), 0x25: (4, 4, 'RGBA8'), 0x29: (8, 4, 'RGBA16F'), 0x2E: (16, 4, 'RGBA32F'), 0x32: (2, 0, 'D16'),
       0x33: (4, 0, 'X8D24'), 0x34: (4, 0, 'D32F'), 0x35: (4, 0, 'D24S8'), 0x36: (8, 0, 'D32FS8'), 0x37: (4, 4, 'RGBA8_SRGB'),
       0x3D: (4, 4, 'RGB10A2'), 0x3F: (4, 3, 'R11G11B10F'), 0x40: (4, 3, 'RGB9E5')}
# Vulkan format -> components (the pipeline list holds Vulkan formats)
VK_COMP = {9: 1, 70: 1, 76: 1, 100: 1, 16: 2, 77: 2, 83: 2, 37: 4, 43: 4, 44: 4, 50: 4, 64: 4, 97: 4, 109: 4, 122: 3, 123: 3}
VK_NAME = {9: 'R8', 70: 'R16', 76: 'R16F', 83: 'RG16F', 37: 'RGBA8', 97: 'RGBA16F', 122: 'R11G11B10F', 124: 'D16', 126: 'D32F', 129: 'D24S8'}


def name(fs):
    if not fs: return '-'
    return NAMES.get(fs, NAMES.get(fs[:8], fs[:12]))


class P:
    def __init__(self, i, atts):
        self.i, self.atts = i, atts          # [(slot, addr)]
        self.draws, self.fs, self.exact, self.reads, self.ld, self.st = 0, None, False, [], {}, {}
        self.kind, self.cover_dc, self.folded, self.plan = None, 0, [], None


def parse(path):
    """-> events [('pass', P) | ('clear', addr, full) | ('op', kind)], images {addr: (w, h, fmt)}, stats line"""
    ev, img, cur, last, clears, stats = [], {}, None, None, [], ''
    for line in Path(path).read_text(errors='replace').splitlines():
        t = line.split()
        if not t: continue
        if t[0] == 'pass':
            atts = [(k, int(v, 16)) for k, v in (x.split('=') for x in t[1:] if '=' in x)]
            cur = P(len(ev), atts); ev.append(('pass', cur)); continue
        if cur is not None:
            if t[0] == 'draws': cur.draws = int(t[1]); continue
            if t[0].startswith('fs=') and cur.fs is None: cur.fs = t[0][3:]; cur.exact = 'exact' in t[1:]; continue
            if t[0] == 'read': cur.reads.append(int(t[1], 16)); continue
            if t[0] == 'end': last, cur = cur, None; continue
            if t[0] == 'clear':
                clears.append(len(ev)); ev.append(('clear', int(t[1], 16), 'full=1' in line, 'in-pass', True)); continue
            continue
        if line.startswith('# plan') and last is not None:
            last.plan = int(t[2]); last.kind = t[3] if len(t) > 3 else None
            for x in t[4:]:
                m = re.match(r'(c\d|ds):(\w+)/(\w+)', x)
                if m: last.ld[m.group(1)], last.st[m.group(1)] = m.group(2), m.group(3)
                m = re.match(r'cover_dc=0x([0-9a-f]+)', x)
                if m: last.cover_dc = int(m.group(1), 16)
            last = None; continue
        if line.startswith('# clear') and 'folded into' in line:
            m = re.match(r'# clear (\d+) folded into (\d+)', line)
            if m and clears: e = ev[clears[-1]]; ev[clears[-1]] = e[:3] + ('folded:%s' % m.group(2), e[4])
            continue
        if line.startswith('# image'):
            m = re.match(r'# image (0x[0-9a-f]+) (\d+)x(\d+) fmt=(0x[0-9a-f]+)', line)
            if m: img[int(m.group(1), 16)] = (int(m.group(2)), int(m.group(3)), int(m.group(4), 16))
            continue
        if line.startswith('# stats'): stats = line[2:]; continue
        if t[0] == 'clear': clears.append(len(ev)); ev.append(('clear', int(t[1], 16), 'full=1' in line, 'op', False)); continue
        if t[0] == 'op': ev.append(('op', t[1])); continue
    # folded clears -> the pass that took them (by plan number)
    byplan = {e[1].plan: e[1] for e in ev if e[0] == 'pass' and e[1].plan is not None}
    for e in ev:
        if e[0] == 'clear' and e[3].startswith('folded:'):
            p = byplan.get(int(e[3][7:]))
            if p: p.folded.append(e[1])
    return ev, img, stats


def next_use(ev, start, addr):
    """the next event of addr after event index start: read / load / clear / dc / open"""
    later = False   # in-pass clears right after `start` belong to its own pass (an RT clear inside it), not to what comes next
    for e in ev[start + 1:]:
        if e[0] == 'clear' and e[1] == addr and (later or not e[4]): return 'clear' if e[2] else 'part-clear'
        if e[0] == 'pass':
            later = True
            p = e[1]
            if addr in p.reads: return 'read'
            for s, a in p.atts:
                if a == addr:
                    if not p.draws and p.kind in ('empty', None): continue      # an IR pass the plan drops
                    l = p.ld.get(s, 'load')
                    return {'load': 'load', 'clear': 'CLEAR', 'dc': 'dc'}.get(l, l)
    return 'open'


def load_pipes(path):
    """{fs key (16 hex): [facts]} from a warm-up list; facts = dict(cols=[vk formats], blend=[(en, mask)], dt, dw, st, ds=fmt)"""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import dos2_pipe_seed as S
    recs, _ = S.records(Path(path).read_bytes())
    out = collections.defaultdict(list)
    for keys, w, _raw in recs:
        try: p, _ = S.decode(w)
        except S.Bad: continue
        fs = keys[-1] if len(keys) >= 2 else None
        if not fs: continue
        base = fs.split('.')[-1][:16] if '.' in fs else fs[:16]
        f = dict(cols=[p['att'][a]['format'] if a != 0xFFFFFFFF else None for a, _l in p['col']],
                 blend=[(b[0], b[7]) for b in p['blend']], dt=p['ds']['depth_test'], dw=p['ds']['depth_write'], st=p['ds']['stencil_test'],
                 ds=p['att'][p['depth'][0]]['format'] if p['depth'] else None, fused=fs if '.' in fs else None)
        if f not in out[base]: out[base].append(f)
    return out


def load_heat(path):
    """{(fs, WxH): median us}, {fs: WxH most common}"""
    us, size = collections.defaultdict(list), collections.defaultdict(collections.Counter)
    for line in Path(path).read_text(errors='replace').splitlines():
        c = line.split('\t')
        if c[0] != 'P' or len(c) < 11: continue
        progs = c[10].split(',')
        fs = progs[0].split('/')[1] if progs and '/' in progs[0] else None
        if not fs: continue
        fs = fs.split('.')[-1][:16]
        us[(fs, c[5])].append(int(c[4])); size[fs][c[5]] += 1
    return {k: statistics.median(v) for k, v in us.items()}, dict(size)


def full_write(f):
    """every colour attachment written in full (no blend, mask = every channel of its format)"""
    for fmt, (en, mask) in zip(f['cols'], f['blend']):
        if fmt is None: continue
        need = (1 << VK_COMP.get(fmt, 4)) - 1
        if en or (mask & need) != need: return False
    return True


def mask_short(f):
    """full except for a mask that is 0xF only if the format had an alpha (the NR-10b rule refuses it today)"""
    return full_write(f) and any(fmt is not None and (mask & 15) != 15 for fmt, (en, mask) in zip(f['cols'], f['blend']))


def analyse(dump, pipes=None, heat=None, sizes=None):
    ev, img, stats = parse(dump)
    rows, cand = [], collections.defaultdict(list)
    for i, e in enumerate(ev):
        if e[0] != 'pass': continue
        p = e[1]
        if not p.draws: continue
        w, h = (0, 0)
        if sizes and p.fs and p.fs in sizes:   # the heat's pass size for this shader: the image table's size when the heat saw it, else its most common
            tab = ['%dx%d' % img[a][:2] for _s, a in p.atts if a in img]
            seen = [z for z in tab if z in sizes[p.fs]]
            w, h = map(int, (seen[0] if seen else sizes[p.fs].most_common(1)[0][0]).split('x'))
        atts = []
        for s, a in p.atts:
            iw, ih, fmt = img.get(a, (0, 0, 0x25))
            bpp = NVN.get(fmt, (4, 4, hex(fmt)))[0]
            aw, ah = (w, h) if w else (iw, ih)
            atts.append(dict(slot=s, addr=a, fmt=fmt, bpp=bpp, w=aw, h=ah, ld=p.ld.get(s, '?'), st=p.st.get(s, '?'),
                             nxt=next_use(ev, i, a), folded=a in p.folded))
        pw, ph = (w, h) if w else (min((x['w'] for x in atts), default=0), min((x['h'] for x in atts), default=0))
        mb = lambda x: x['w'] * x['h'] * x['bpp'] / 1e6
        tiler = sum(mb(x) for x in atts if x['ld'] == 'load') + sum(mb(x) for x in atts if x['st'] == 'store')
        sysclear = sum(mb(x) for x in atts if x['ld'] == 'clear')
        facts = (pipes or {}).get(p.fs or '', [])
        # sysmem model: every colour attachment written once over the pass area (x2 when the pipeline blends: the destination is read),
        # depth read for the test / written, plus each sampled frame image (an image of the IR table >= 64x64) read once in full
        f0 = facts[0] if facts else None
        blend = [b[0] for b in f0['blend']] if f0 else []
        sysw = 0.0; ci = 0
        for x in atts:
            if x['slot'] == 'ds':
                if f0 and (f0['dt'] or f0['st']): sysw += pw * ph * x['bpp'] / 1e6
                if f0 and f0['dw']: sysw += pw * ph * x['bpp'] / 1e6
            else:
                sysw += pw * ph * x['bpp'] * (2 if ci < len(blend) and blend[ci] else 1) / 1e6; ci += 1
        sysr = 0.0
        for a in set(p.reads):
            if a in img and img[a][0] >= 64 and img[a][1] >= 64:
                iw, ih, fmt = img[a]; sysr += iw * ih * NVN.get(fmt, (4, 4, ''))[0] / 1e6
        us = heat.get((p.fs, '%dx%d' % (pw, ph))) if heat and p.fs else None
        rows.append(dict(i=i, plan=p.plan, fs=p.fs, name=name(p.fs), draws=p.draws, exact=p.exact, w=pw, h=ph, atts=atts, tiler=tiler,
                         sysclear=sysclear, sysw=sysw, sysr=sysr, reads=len(set(p.reads)), facts=facts, us=us, cover_dc=p.cover_dc))
        # candidates
        one = p.draws == 1 and p.exact
        if one and facts and all(full_write(f) for f in facts):
            for x in atts:
                if x['slot'] != 'ds' and x['ld'] == 'clear':
                    cand['CLRDC'].append((p.plan, name(p.fs), x['w'], x['h'], NVN.get(x['fmt'], (4, 4, hex(x['fmt'])))[2], mb(x)))
        if one and facts and all(mask_short(f) for f in facts):
            for k, x in enumerate(atts):
                if x['slot'] != 'ds' and x['ld'] == 'load' and (p.cover_dc >> k & 1):
                    cand['FMASK'].append((p.plan, name(p.fs), x['w'], x['h'], NVN.get(x['fmt'], (4, 4, hex(x['fmt'])))[2], mb(x)))
        if facts and any(x['slot'] == 'ds' for x in atts) and all(f['ds'] is not None and not (f['dt'] or f['dw'] or f['st']) for f in facts):
            d = [x for x in atts if x['slot'] == 'ds'][0]
            cand['DSIDLE'].append((p.plan, name(p.fs), d['w'], d['h'], 'D24S8', (mb(d) if d['ld'] == 'load' else 0) + (mb(d) if d['st'] == 'store' else 0)))
        for x in atts:
            if x['st'] == 'store' and x['nxt'] in ('clear', 'CLEAR', 'dc'):
                cand['DEADST'].append((p.plan, name(p.fs), x['w'], x['h'], NVN.get(x['fmt'], (4, 4, hex(x['fmt'])))[2], mb(x)))
    return rows, cand, stats


SKY, LIGHT, FOG = '640a4c497bcc748b', 'd236b8d5b7ffad3c', 'd8aeec640d465275'
def s2f_census(dump, tot):
    """NR-FRAME 8: Sky -> Light -> Fog back to back (only empty passes / clears of Fog's target between), Fog = 1 attachment c0 that
    Light samples (G-buffer emissive) and Sky does not, Fog samples Df (Light's target) and not c0; counts into tot"""
    ev, _img, _ = parse(dump)
    ps = [(i, e[1]) for i, e in enumerate(ev) if e[0] == 'pass' and e[1].draws]
    for n, (i, p) in enumerate(ps):
        if p.fs != LIGHT: continue
        tot['light'] += 1
        if n == 0 or ps[n - 1][1].fs != SKY: tot['no Sky before'] += 1; continue
        if n + 1 >= len(ps) or ps[n + 1][1].fs != FOG: tot['next is not Fog'] += 1; continue
        S, L, F = ps[n - 1][1], p, ps[n + 1][1]
        df, sp = L.atts[0][1], S.atts[0][1]; c0 = F.atts[0][1] if F.atts else None
        between = [e for e in ev[i + 1:ps[n + 1][0]] if not (e[0] == 'pass' and not e[1].draws)]
        cond = [('Fog has one attachment', len(F.atts) == 1), ('Fog samples Df', df in F.reads), ('Light samples c0', c0 in L.reads),
                ('Sky does not sample c0', c0 not in S.reads), ('Fog does not sample c0', c0 not in F.reads), ('c0 is not Sp / Df', c0 not in (sp, df)),
                ('only clears of c0 between', all(e[0] == 'clear' and e[1] == c0 for e in between)), ('Fog one draw', F.draws == 1)]
        bad = [k for k, v in cond if not v]
        if bad: tot['refused: ' + bad[0]] += 1
        else: tot['S2F'] += 1; tot['  with a clear of c0 between'] += int(bool(between))


def fmt_rows(rows, md=False):
    out = []
    hdr = ['#', 'plan', 'pass (first FS)', 'draws', 'size', 'attachments (slot fmt load/store -> next use)', 'tiler MB', 'sysmem clear MB', 'sysmem write+read MB', 'heat us', 'pipe']
    if md: out.append('| ' + ' | '.join(hdr) + ' |'); out.append('|' + '---|' * len(hdr))
    for n, r in enumerate(rows):
        a = ', '.join('%s %s %s/%s%s->%s' % (x['slot'], NVN.get(x['fmt'], (4, 4, hex(x['fmt'])))[2], x['ld'], x['st'], '(f)' if x['folded'] else '', x['nxt']) for x in r['atts'])
        pf = ';'.join('%s%s' % ('+'.join(VK_NAME.get(c, str(c)) for c in f['cols'] if c is not None) or 'noc',
                                ' blend' if any(b[0] for b in f['blend']) else '' ) + (' dt' if f['dt'] else '') + (' dw' if f['dw'] else '') +
                      (' st' if f['st'] else '') + (' ds-idle' if f['ds'] is not None and not (f['dt'] or f['dw'] or f['st']) else '')
                      for f in r['facts'][:2]) or '-'
        cells = [str(n), str(r['plan']), r['name'] + ('' if r['exact'] or r['draws'] > 1 else ' (mixed)'), str(r['draws']), '%dx%d' % (r['w'], r['h']), a,
                 '%.1f' % r['tiler'], '%.1f' % r['sysclear'] if r['sysclear'] else '', '%.1f + %.1f' % (r['sysw'], r['sysr']) if r['facts'] else '?', '%.0f' % r['us'] if r['us'] else '', pf]
        out.append(('| ' + ' | '.join(cells) + ' |') if md else '  '.join(cells))
    return out


def fmt_cand(cand):
    out = []
    for k in ('CLRDC', 'FMASK', 'DSIDLE', 'DEADST'):
        v = cand.get(k, [])
        out.append('%s: %d a frame, %.1f MB  %s' % (k, len(v), sum(x[5] for x in v),
                   '; '.join('plan %s %s %dx%d %s %.1f MB' % x for x in v[:12])))
    return out


def self_test():
    import tempfile, os
    dump = """# DOS2 NB-04 IR dump (DM_NB=1): frame 1 part 0
pass c0=0xa0
draws 1 cover=0x0
fs=f23c09d198b55dc5 exact
end clear
# plan 1 exec c0:clear/store
clear 0xb0 asp=c full=1 rt=0 v=0 d=0 s=0
# clear 0 folded into 3
pass c0=0xb0
draws 1 cover=0x0
fs=d8aeec640d465275 exact
read 0xc0
end rt
# plan 3 exec c0:clear/store
pass c0=0xc0
draws 1 cover=0x0
fs=62d683cc49953ad6 exact
read 0xb0
end rt
# plan 4 exec c0:load/store cover_dc=0x1
pass c0=0xc0 ds=0xd0
draws 1 cover=0x0
fs=343488601475d44d exact
read 0xd0
end rt
# plan 5 exec c0:load/store ds:load/store cover_dc=0x1
clear 0xb0 asp=c full=1 rt=0 v=0 d=0 s=0
# image 0xa0 100x10 fmt=0x6
# image 0xb0 100x10 fmt=0x3f
# image 0xc0 100x10 fmt=0x3f
# image 0xd0 100x10 fmt=0x35
"""
    pipes = {'f23c09d198b55dc5': [dict(cols=[70], blend=[(0, 1)], dt=0, dw=0, st=0, ds=None, fused=None)],
             'd8aeec640d465275': [dict(cols=[122], blend=[(0, 7)], dt=0, dw=0, st=0, ds=None, fused=None)],
             '62d683cc49953ad6': [dict(cols=[122], blend=[(0, 7)], dt=0, dw=0, st=0, ds=None, fused=None)],
             '343488601475d44d': [dict(cols=[122], blend=[(1, 7)], dt=0, dw=0, st=0, ds=129, fused=None)]}
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, 'nb_ir_00_f1_p0.txt'); Path(f).write_text(dump)
        rows, cand, _ = analyse(f, pipes)
    ok = True
    def chk(c, m):
        nonlocal ok
        print(('PASS ' if c else 'FAIL ') + m); ok &= bool(c)
    chk(len(rows) == 4, 'four executed passes')
    chk([x[1] for x in cand['CLRDC']] == ['LinearizeDepth', 'DeferredFog'], 'CLRDC: LinearizeDepth (R16 mask 1) + DeferredFog (folded clear, R11G11B10F mask 7)')
    chk(abs(sum(x[5] for x in cand['CLRDC']) - (100 * 10 * 2 + 100 * 10 * 4) / 1e6) < 1e-9, 'CLRDC MB at real bpp')
    chk([x[1] for x in cand['FMASK']] == ['FadeBlending'], 'FMASK: FadeBlending only (CrossBilateral blends)')
    chk([x[1] for x in cand['DSIDLE']] == ['CrossBilateralUpsample'], 'DSIDLE: CrossBilateralUpsample (no test / write / stencil)')
    chk(rows[1]['atts'][0]['folded'] and rows[1]['atts'][0]['nxt'] == 'read', 'Fog output: folded clear, next use = read by Fade')
    chk(rows[3]['atts'][0]['nxt'] == 'open' and rows[2]['atts'][0]['nxt'] == 'load', 'next use: Fade out -> loaded by the next pass')
    chk(not cand['DEADST'], 'no dead store (0xb0 is cleared after Fade read it, but Fog stores before Fade reads)')
    print('self-test: ' + ('all passed' if ok else 'FAILED'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', nargs='*'); ap.add_argument('--pipes'); ap.add_argument('--heat'); ap.add_argument('--md', action='store_true')
    ap.add_argument('--summary', action='store_true', help='candidates only, one block per dump'); ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--s2f', action='store_true', help='NR-FRAME 8: count the Sky -> Light -> Fog chains (S2F) of the dumps')
    a = ap.parse_args()
    if a.self_test: return self_test()
    pipes = load_pipes(a.pipes) if a.pipes else None
    heat, sizes = load_heat(a.heat) if a.heat else (None, None)
    files = []
    for d in a.dump:
        q = Path(d); files += sorted(q.glob('nb_ir_*_p*.txt')) if q.is_dir() else [q]
    if a.s2f:
        t = collections.Counter()
        for f in files: s2f_census(f, t)
        print('S2F census over %d parts: %s' % (len(files), ', '.join('%s %d' % kv for kv in t.items()))); return 0
    tot = collections.defaultdict(lambda: [0, 0.0]); n3d = 0
    for f in files:
        rows, cand, stats = analyse(f, pipes, heat, sizes)
        if not any(r['name'] == 'ClusteredLightingWithSSAO' for r in rows): continue
        n3d += 1
        print('== %s (%d executed passes; %s)' % (f, len(rows), stats))
        if not a.summary: print('\n'.join(fmt_rows(rows, a.md)))
        print('\n'.join('  ' + x for x in fmt_cand(cand)))
        for k, v in cand.items(): tot[k][0] += len(v); tot[k][1] += sum(x[5] for x in v)
    if n3d: print('== mean over %d 3D parts: ' % n3d + '; '.join('%s %.2f a frame, %.1f MB' % (k, v[0] / n3d, v[1] / n3d) for k, v in sorted(tot.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
