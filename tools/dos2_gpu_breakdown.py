#!/usr/bin/env python3
"""DOS2 GPU breakdown of one frame: per-pass GPU time (NVN GPASS lines, DM_GFX_PASS_PROF) or, without them, a DRAM
traffic proxy from the pass graph (NVN PGRAPH lines, DM_GFX_PASS_GRAPH=1). docs/dos2/GPU_BREAKDOWN.md.

  python tools/dos2_gpu_breakdown.py work/logs/p2b.log                 # PGRAPH proxy, every sampled frame (one line per frame)
  python tools/dos2_gpu_breakdown.py work/logs/p2b.log --frame 2509    # PGRAPH proxy table of one frame
  python tools/dos2_gpu_breakdown.py work/logs/gp1.log --gpass         # GPASS: time per pass and per group, every profiled frame

Groups: portrait (80x100 mini pipeline), ui (Iggy layer), iggy-filter (Iggy filter ping-pong), prepass, gbuffer, shadow,
shadow-resolve, lighting (sky + clustered + fog + fade), particles(+chain), bloom/lum, post-tail, combine, clear.
Proxy = bytes written by the attachments over the whole pass area (sysmem: no tile load/store; draws write DRAM directly) +
1:1 reads of render targets produced earlier in the frame. Draw coverage is unknown in PGRAPH: Iggy filter quads, UI and
geometry passes are counted as full-area x 1 (upper bound for the filters, lower bound for overdraw-heavy geometry).
"""
import argparse, collections, re, sys

BPP = {0x01: 1, 0x05: 2, 0x06: 2, 0x11: 4, 0x25: 4, 0x29: 8, 0x32: 2, 0x33: 4, 0x34: 4, 0x35: 4, 0x36: 8, 0x3d: 4, 0x3f: 4}
NAMES = {   # ps program key prefix (PGRAPH fs= / GPASS 4:<key>) -> EngineShaders name (work/shaders-jobs/jobs.tsv)
    '9c8e09d06f45': 'FadePattern(depth)', 'f23c09d198b5': 'LinearizeDepth', '640a4c497bcc': 'DeferredSky',
    'd236b8d5b7ff': 'ClusteredLightingWithSSAO', 'd8aeec640d46': 'DeferredFog', '62d683cc4995': 'FadeBlending',
    'cbe8649300e9': 'PPDownSampleLog2', '7b3210191abd': 'PPDefault', 'f3fd8e101578': 'PPLuminanceAccum',
    '921728a5bbb1': 'PPApplyLUT', '691768aa0854': 'PPFXAA', '91eb2d97a304': 'EoCSight', '43e8e8ed2b1d': 'PPVignette',
    '98186c22a720': 'CombineUI_NoFetch', '1372db843749': 'CascadedPCF_3x768', 'd99f1fdb55bb': 'Iggy(main+0x2ace788)',
    'b5afd82fadff': 'Iggy-fill(main+0x2a32fa0)', '5e26ce730b83': 'Iggy-blur(main+0x2a507c8)',
    'facf8294': 'DownsampleDepthMax', '34348860': 'CrossBilateralUpsample',
    '74fe9ae2066d': 'bloom-down', '0e3fc07b320d': 'bloom-blur', '4e290c9cf116': 'bloom-up',
}

def name_of(fs):
    for k, v in NAMES.items():
        if fs.startswith(k) or k.startswith(fs[:8]) and fs[:8] == k[:8]:
            return v
    return fs

PASS = re.compile(r'NVN PGRAPH f=(\d+) p(\d+) s\d+ L\w+ (\d+)x(\d+) att=\[([^\]]*)\] d=(\d+) fs=(\S+) in=\[([^\]]*)\]')
IMGS = re.compile(r'NVN PGRAPH f=(\d+) imgs (.*)')

def group(p, w, h, names, draws, fs_name, att_imgs, imgs):
    if w * h <= 80 * 100 or (w <= 256 and h <= 256 and fs_name in ('PPDownSampleLog2', 'PPDefault', 'PPLuminanceAccum')):
        return 'portrait/lum-small'
    if fs_name == '-':
        return 'clear-only'
    if fs_name.startswith('Iggy-'):
        return 'iggy-filter'
    if fs_name.startswith('Iggy'):
        return 'ui'
    if fs_name.startswith('bloom'):
        return 'bloom'
    if w == h and w >= 1024:
        return 'shadow'
    if fs_name in ('FadePattern(depth)',) and draws > 10:
        return 'prepass'
    if fs_name in ('DeferredSky', 'ClusteredLightingWithSSAO', 'DeferredFog', 'FadeBlending'):
        return 'lighting'
    if fs_name == 'CascadedPCF_3x768':
        return 'shadow-resolve'
    if fs_name in ('PPApplyLUT', 'EoCSight', 'PPFXAA', 'PPVignette', 'LinearizeDepth'):
        return 'post-tail' if fs_name != 'LinearizeDepth' else 'lin-depth'
    if fs_name == 'CombineUI_NoFetch':
        return 'combine'
    if len(att_imgs) >= 4:
        return 'gbuffer'
    if draws <= 8 and len(att_imgs) == 2:
        return 'particles/forward'
    return 'other'

def proxy(path, want):
    frames = collections.defaultdict(list); imgs = collections.defaultdict(dict)
    for line in open(path, errors='replace'):
        m = PASS.search(line)
        if m:
            f, p, w, h, att, d, fs, inp = m.groups()
            frames[int(f)].append((int(p), int(w), int(h), att, int(d), fs, inp)); continue
        m = IMGS.search(line)
        if m:
            for tok in m.group(2).split():
                k, v = tok.split('=', 1); parts = v.split(':')
                imgs[int(m.group(1))][k] = (int(parts[1], 16), *map(int, parts[2].split('x')))
    out = []
    for f in sorted(frames):
        if want is not None and f != want:
            continue
        rows = []; tot = collections.Counter(); im = imgs[f]
        for p, w, h, att, d, fs, inp in frames[f]:
            atts = [a.strip() for a in att.split(',') if a.strip()]
            ids = [re.match(r'(#\d+)', a).group(1) for a in atts]
            wr = 0
            for a, i in zip(atts, ids):
                fmt = im.get(i, (0x25, w, h))[0]
                wr += w * h * BPP.get(fmt, 4)
            rd = 0
            for r in inp.split(','):
                r = r.strip()
                mm = re.match(r'(#\d+)<p\d+', r)
                if mm and mm.group(1) in im:
                    fmt, iw, ih = im[mm.group(1)]
                    rd += min(iw * ih, w * h) * BPP.get(fmt, 4)
            fsn = name_of(fs) if fs != '-' else '-'
            g = group(p, w, h, fsn, d, fsn, ids, im)
            mb = (wr + rd) / 1e6
            rows.append((p, f'{w}x{h}', d, fsn, g, wr / 1e6, rd / 1e6)); tot[g] += mb
        out.append((f, rows, tot))
    return out

GP = re.compile(r'NVN GPASS frame=(\d+) slot=(\d+) ring=\d+ i=(\d+) kind=(\w+) us=(\d+) (\d+)x(\d+) att=(\d+) fmt=(\d+) draws=(\d+) verts=(\d+) first=(\S+) last=(\S+)')
GS = re.compile(r'NVN GPASS frame=(\d+) slot=(\d+) ring=\d+ summary .*pass_us=(\d+) compute_us=(\d+) span_us=(-?\d+)')

def gpass(path):
    frames = collections.defaultdict(list); spans = {}
    for line in open(path, errors='replace'):
        m = GP.search(line)
        if m:
            f, s, i, kind, us, w, h, att, fmt, d, v, first, last = m.groups()
            ps = re.search(r'4:([0-9a-f]{16})', first); fsn = name_of(ps.group(1)[:12]) if ps else '-'
            rp = re.search(r' rep=(\d+)', line)   # counter reports recorded in the pass (VERTEX.md; absent in older logs)
            frames[int(f)].append((int(i), int(us), int(w), int(h), int(att), int(fmt), int(d), fsn, int(rp.group(1)) if rp else -1)); continue
        m = GS.search(line)
        if m:
            spans[int(m.group(1))] = (int(m.group(3)), int(m.group(5)))
    for f in sorted(frames):
        tot = collections.Counter(); rows = sorted(frames[f])
        for i, us, w, h, att, fmt, d, fsn, rp in rows:
            g = group(i, w, h, fsn, d, fsn, [0] * att, {}); tot[g] += us
        pu, span = spans.get(f, (sum(r[1] for r in rows), -1))
        print(f'frame {f}: passes={len(rows)} pass_us={pu} span_us={span} | ' +
              ' '.join(f'{g}={us/1000:.2f}' for g, us in tot.most_common()))
        for i, us, w, h, att, fmt, d, fsn, rp in sorted(rows, key=lambda r: -r[1])[:12]:
            per = f' {us / d:4.1f} us/draw' if d else ''
            print(f'   i={i:3d} {us:6d} us {w}x{h} att={att} fmt={fmt:#x} draws={d}{per}' + (f' rep={rp}' if rp >= 0 else '') + f' {fsn}')

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('log'); ap.add_argument('--frame', type=int); ap.add_argument('--gpass', action='store_true')
    a = ap.parse_args()
    if a.gpass:
        return gpass(a.log)
    for f, rows, tot in proxy(a.log, a.frame):
        if a.frame is None:
            print(f'frame {f}: passes={len(rows)} MB={sum(tot.values()):.0f} | ' + ' '.join(f'{g}={mb:.0f}' for g, mb in tot.most_common()))
            continue
        print(f'frame {f}: {len(rows)} passes, proxy {sum(tot.values()):.0f} MB')
        for p, sz, d, fsn, g, wr, rd in rows:
            print(f'  p{p:<3d} {sz:>10s} d={d:<4d} {g:<18s} {fsn:<28s} write={wr:6.1f} read_rt={rd:6.1f} MB')
        print('  by group: ' + ', '.join(f'{g} {mb:.0f} MB' for g, mb in tot.most_common()))

if __name__ == '__main__':
    sys.exit(main())
