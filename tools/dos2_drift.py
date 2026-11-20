#!/usr/bin/env python3
# [DOS2] NR-DRIFT (docs/dos2/nr/NR-DRIFT.md): "after a location starts we lose 10-15 fps for good" - read a phone log and say where
# the frames went: the DRS climb after a load, a slope at a fixed resolution, a GPU clock change (the probe of DM_NR_GPUPROBE), or a
# size that only grows ("[nr] growth").
#
#   python tools/dos2_drift.py <log> [<log> ...] [--csv out.csv] [--all-sizes]
#     <log>: logcat (*.log, stream.log), the port's own lines (*.dm.log) or Download/.../logs/boot.log (no timestamps: the time is the
#            sum of the perf windows' wall_ms)
#   python tools/dos2_drift.py --self-test
#
# Output per process (pid) and per scene segment (a segment = the windows between two load screens; a load screen = < 64 draws a
# frame, as nr_drift.h): duration, level, fps / GPU ms of the first and the last 10 s, the DRS height over the segment and how long
# it stayed under 100 %, the plateau (windows at 100 % DRS with draws within 15 % of the segment median) with its GPU-ms slope per
# minute, and the verdict. Then the growth lines: every size first -> last and whether it only grew, the probe's ALU index and copy
# GB/s first -> last, DRS / thermal / RSS / CPU MHz. Same-level segments of one process are compared (plateau GPU ms at a similar draw
# count): the "come back to the same place" test.
import math
import re
import sys

T = re.compile(r'^(\d\d-\d\d) (\d\d):(\d\d):(\d\d\.\d+)')
PID = re.compile(r'^\d\d-\d\d \d\d:\d\d:\d\d\.\d+\s+(\d+)\s|\(\s*(\d+)\)')
V = re.compile(r'NVN PERF v=1 .*?frames=(\d+) wall_ms=([\d.]+) fps=([\d.]+) submit_cpu_ms=([\d.]+) present_cpu_ms=([\d.]+) gpu_ms=([-\d.]+)'
               r'.*? draws=([\d.]+)')
DRS = re.compile(r'\[quality\] GPU frame time \(10 s\): the game reads ([\d.]+) ms \(min ([\d.]+), max ([\d.]+).*?DRS current \d+x\d+ = ([\d.]+) %'
                 r'.*?target ([\d.]+) ms')
LOC = re.compile(r'\[nr\] heat: location (\S+) -> (\S+)')
GROWTH = re.compile(r'\[nr\] growth \(.*')
SETTLE = re.compile(r'\[quality\] DRS settle .*')
LOAD_DRAWS = 64


def parse_growth(line):
    """the '[nr] growth' line -> dict of sections: sizes / since / gpu / drs / thermal / mem / cpu / cost"""
    out = {}
    body = line[line.index('[nr] growth'):]
    m = re.search(r'level (\S+)\): sizes(.*?)(;|$)', body)
    if m:
        out['level'] = m.group(1)
        out['sizes'] = {k: float(v) for k, v in re.findall(r' ([a-z_]+)=([-\d.]+)', m.group(2))}
    m = re.search(r'; since load(.*?)(;|$)', body)
    if m:
        out['since'] = {k: float(v) for k, v in re.findall(r' ([a-z_]+)([+-][\d.]+)', m.group(1))}
    m = re.search(r'; gpu alu (\?\s)?([-\d.]+) \(([-\d.]+) ns/iter.*?copy ([-\d.]+) us ([-\d.]+) GB/s index (\?\s)?([-\d.]+)', body)
    if m:
        out['alu_index'] = float(m.group(2)); out['ns_iter'] = float(m.group(3)); out['copy_gbs'] = float(m.group(5)); out['copy_index'] = float(m.group(7))
    m = re.search(r'; drs ([-\d.]+) %', body)
    if m: out['drs'] = float(m.group(1))
    m = re.search(r'; thermal (-?\d+) headroom ([-\w.]+)', body)
    if m:
        out['thermal'] = int(m.group(1))
        try: out['headroom'] = float(m.group(2))
        except ValueError: out['headroom'] = float('nan')
    m = re.search(r'; mem rss ([-\d.]+) MB', body)
    if m: out['rss'] = float(m.group(1))
    m = re.search(r'; cpu MHz(.*?)(;|$)', body)
    if m: out['cpu'] = {k: float(v) for k, v in re.findall(r' (\S+) (-?\d+)', m.group(1))}
    m = re.search(r'walks retire cleanup ([\d.]+) us', body)
    if m: out['walk_us'] = float(m.group(1))
    m = re.search(r'; cost (\d+) us', body)
    if m: out['cost_us'] = float(m.group(1))
    return out


def read(path):
    """-> {pid: {'w': [windows], 'drs': [(t, pct, max_ms, target)], 'loc': [(t, level)], 'growth': [(t, dict)], 'settle': [(t, text)]}}"""
    procs = {}
    cum = {}
    with open(path, errors='replace') as f:
        for line in f:
            m = T.match(line)
            pm = PID.search(line)
            pid = (pm.group(1) or pm.group(2)) if pm else 'boot'
            p = procs.setdefault(pid, {'w': [], 'drs': [], 'loc': [], 'growth': [], 'settle': []})
            t = int(m.group(2)) * 3600 + int(m.group(3)) * 60 + float(m.group(4)) if m else None
            if t is None:
                t = cum.get(pid, 0.0)
            v = V.search(line)
            if v:
                fr, wall, fps, sc, pc, g, dr = v.groups()
                if m is None:
                    cum[pid] = cum.get(pid, 0.0) + float(wall) / 1000.0; t = cum[pid]
                p['w'].append({'t': t, 'frames': int(fr), 'wall': float(wall), 'fps': float(fps), 'sub': float(sc), 'pres': float(pc),
                               'gpu': float(g), 'draws': float(dr)})
                continue
            d = DRS.search(line)
            if d:
                p['drs'].append((t, float(d.group(4)), float(d.group(3)), float(d.group(5)))); continue
            lo = LOC.search(line)
            if lo:
                p['loc'].append((t, lo.group(2))); continue
            if '[nr] growth (' in line:
                p['growth'].append((t, parse_growth(line))); continue
            s = SETTLE.search(line)
            if s:
                p['settle'].append((t, s.group(0)))
    return {k: v for k, v in procs.items() if v['w']}


def drs_at(p, t):
    """the last DRS height logged at or before t (the 10-s line), None when unknown"""
    best = None
    for (tt, pct, mx, tg) in p['drs']:
        if tt <= t + 0.01: best = pct
        else: break
    return best


def level_at(p, t):
    best = '-'
    for (tt, lv) in p['loc']:
        if tt <= t + 0.01: best = lv
        else: break
    return best


def segments(p):
    segs, cur = [], []
    for w in p['w']:
        if w['draws'] < LOAD_DRAWS:
            if len(cur) >= 5: segs.append(cur)
            cur = []
        else:
            cur.append(w)
    if len(cur) >= 5: segs.append(cur)
    return segs


def wavg(ws, key):
    fr = sum(w['frames'] for w in ws)
    return sum(w[key] * w['frames'] for w in ws) / fr if fr else float('nan')


def fps_of(ws):
    wall = sum(w['wall'] for w in ws)
    return sum(w['frames'] for w in ws) * 1000.0 / wall if wall else float('nan')


def slope(xs, ys):
    n = len(xs)
    if n < 3: return float('nan')
    mx, my = sum(xs) / n, sum(ys) / n
    vx = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / vx if vx else float('nan')


def analyse_segment(p, seg):
    t0, t1 = seg[0]['t'], seg[-1]['t']
    first = [w for w in seg if 2.0 <= w['t'] - t0 <= 12.0] or seg[:3]   # the first 10 s after the load hitch (its first 2 s skipped)
    last = [w for w in seg if t1 - w['t'] <= 10.0]
    drs = [(tt, pct) for (tt, pct, mx, tg) in p['drs'] if t0 - 1 <= tt <= t1 + 1]
    below = [tt for (tt, pct) in drs if pct < 99.9]
    climb_s = (max(below) - t0) if below else 0.0
    med = sorted(w['draws'] for w in seg)[len(seg) // 2]
    # plateau: from 10 s after the last DRS change seen in the segment (or its start), draws within 15 % of the median
    changes = [drs[i][0] for i in range(1, len(drs)) if abs(drs[i][1] - drs[i - 1][1]) > 0.01]
    prev = [pct for (tt, pct, mx, tg) in p['drs'] if tt < t0 - 1]
    if drs and prev and abs(drs[0][1] - prev[-1]) > 0.01: changes.append(drs[0][0])
    start = max(changes) + 10.0 if changes else t0 + 2.0
    plat = [w for w in seg if w['t'] >= start and abs(w['draws'] - med) <= 0.15 * med]
    sl = slope([w['t'] / 60.0 for w in plat], [w['gpu'] for w in plat]) if len(plat) >= 6 else float('nan')
    r = {'t0': t0, 'dur': t1 - t0, 'level': level_at(p, t0), 'fps_first': fps_of(first), 'fps_last': fps_of(last),
         'gpu_first': wavg(first, 'gpu'), 'gpu_last': wavg(last, 'gpu'), 'draws_med': med, 'drs_min': min((pct for _, pct in drs), default=None),
         'drs_end': drs[-1][1] if drs else None, 'climb_s': climb_s, 'plat_n': len(plat), 'plat_gpu': wavg(plat, 'gpu') if plat else float('nan'),
         'plat_fps': fps_of(plat) if plat else float('nan'), 'plat_slope': sl, 'plat_min': (plat[-1]['t'] - plat[0]['t']) / 60.0 if plat else 0}
    v = []
    if r['drs_min'] is not None and r['drs_min'] < 99.9 and r['fps_first'] > r['fps_last'] * 1.08:
        v.append('DRS: started at %.1f %%, %.0f s under 100 %% - the early fps was at a lower resolution' % (r['drs_min'], climb_s))
    if not math.isnan(sl) and r['plat_min'] >= 1.0:
        if abs(sl) < 0.05 * r['plat_gpu']: v.append('plateau flat (%+.3f ms GPU a minute over %.1f min)' % (sl, r['plat_min']))
        else: v.append('PLATEAU SLOPE %+.3f ms GPU a minute over %.1f min: look at the probe / growth lines' % (sl, r['plat_min']))
    r['verdict'] = '; '.join(v) if v else '-'
    return r


def growth_report(p, all_sizes):
    gl = p['growth']
    if not gl: return ['  growth lines: none (DM_NR_GROWTH off, or an older APK)']
    out = ['  growth lines: %d, %.0f s' % (len(gl), gl[-1][0] - gl[0][0])]
    s0 = gl[0][1].get('sizes', {}); s1 = gl[-1][1].get('sizes', {})
    grew = []
    for k in s1:
        vals = [g.get('sizes', {}).get(k) for _, g in gl if k in g.get('sizes', {})]
        if len(vals) < 2: continue
        mono = all(b >= a for a, b in zip(vals, vals[1:])) and vals[-1] > vals[0]
        if mono or all_sizes:
            grew.append('%s %g -> %g%s' % (k, vals[0], vals[-1], ' (only grew)' if mono else ''))
    out.append('  sizes: ' + (', '.join(grew) if grew else 'nothing only grew'))
    alu = [(t, g['alu_index']) for t, g in gl if g.get('alu_index', -1) > 0]
    cp = [(t, g['copy_gbs']) for t, g in gl if g.get('copy_gbs', -1) > 0]
    if alu: out.append('  probe ALU index %.1f -> %.1f (min %.1f; 100 = the clock of the first run)' % (alu[0][1], alu[-1][1], min(a for _, a in alu)))
    if cp: out.append('  probe copy %.1f -> %.1f GB/s (min %.1f)' % (cp[0][1], cp[-1][1], min(c for _, c in cp)))
    th = [(g.get('thermal'), g.get('headroom')) for _, g in gl if 'thermal' in g]
    if th: out.append('  thermal status %s -> %s, headroom %s -> %s' % (th[0][0], th[-1][0], th[0][1], th[-1][1]))
    rss = [g['rss'] for _, g in gl if 'rss' in g]
    if rss: out.append('  RSS %.0f -> %.0f MB (max %.0f)' % (rss[0], rss[-1], max(rss)))
    w = [g['walk_us'] for _, g in gl if 'walk_us' in g]
    if w: out.append('  retire walk %.1f -> %.1f us a frame (max %.1f)' % (w[0], w[-1], max(w)))
    c = [g['cost_us'] for _, g in gl if 'cost_us' in g]
    if c: out.append('  line cost mean %.0f us, max %.0f us (once a period)' % (sum(c) / len(c), max(c)))
    cpu0 = gl[0][1].get('cpu', {}); cpu1 = gl[-1][1].get('cpu', {})
    if cpu1: out.append('  clocks first -> last: ' + ', '.join('%s %g->%g' % (k, cpu0.get(k, float('nan')), v) for k, v in cpu1.items()))
    return out


def report(path, csv=None, all_sizes=False):
    procs = read(path)
    lines = ['== %s: %d process(es)' % (path, len(procs))]
    rows = []
    for pid, p in procs.items():
        segs = [analyse_segment(p, s) for s in segments(p)]
        lines.append('-- pid %s: %d windows, %d scene segments, %d DRS lines, %d growth lines' % (pid, len(p['w']), len(segs), len(p['drs']), len(p['growth'])))
        for r in segs:
            lines.append('  [%.0f s, %.0f s long, %s, draws ~%.0f] fps first %.1f last %.1f | GPU ms %.2f -> %.2f | DRS min %s end %s | plateau %d windows %.1f fps %.2f ms slope %+.3f ms/min'
                         % (r['t0'], r['dur'], r['level'], r['draws_med'], r['fps_first'], r['fps_last'], r['gpu_first'], r['gpu_last'],
                            '%.1f' % r['drs_min'] if r['drs_min'] is not None else '?', '%.1f' % r['drs_end'] if r['drs_end'] is not None else '?',
                            r['plat_n'], r['plat_fps'], r['plat_gpu'], r['plat_slope']))
            lines.append('      -> ' + r['verdict'])
            rows.append((path, pid, r))
        # the "come back to the same place" test: same level, plateau draws within 10 %
        by = {}
        for r in segs:
            if r['plat_n'] >= 6: by.setdefault(r['level'], []).append(r)
        for lv, rs in by.items():
            if len(rs) < 2: continue
            a = rs[0]
            for b in rs[1:]:
                if abs(b['draws_med'] - a['draws_med']) <= 0.10 * a['draws_med']:
                    lines.append('  same level %s again: plateau %.2f ms / %.1f fps (draws %.0f) -> %.2f ms / %.1f fps (draws %.0f): %+.1f %% GPU'
                                 % (lv, a['plat_gpu'], a['plat_fps'], a['draws_med'], b['plat_gpu'], b['plat_fps'], b['draws_med'], 100.0 * (b['plat_gpu'] / a['plat_gpu'] - 1)))
        lines += growth_report(p, all_sizes)
        for t, s in p['settle'][:6]: lines.append('  ' + s)
    if csv:
        with open(csv, 'w') as f:
            f.write('log,pid,t0,dur,level,draws_med,fps_first,fps_last,gpu_first,gpu_last,drs_min,drs_end,climb_s,plat_n,plat_fps,plat_gpu,plat_slope\n')
            for path_, pid, r in rows:
                f.write(','.join(str(x) for x in (path_, pid, round(r['t0'], 1), round(r['dur'], 1), r['level'], r['draws_med'], round(r['fps_first'], 2),
                        round(r['fps_last'], 2), round(r['gpu_first'], 3), round(r['gpu_last'], 3), r['drs_min'], r['drs_end'], round(r['climb_s'], 1),
                        r['plat_n'], round(r['plat_fps'], 2), round(r['plat_gpu'], 3), r['plat_slope'])) + '\n')
    return lines


def self_test():
    import os
    import tempfile
    d = tempfile.mkdtemp()
    p = os.path.join(d, 'x.dm.log')
    with open(p, 'w') as f:
        t = 0.0
        def ts(t): return '11-19 00:%02d:%06.3f 1234 1300 I dm-vulkan: ' % (int(t // 60), t % 60)
        f.write(ts(t) + '[nr] heat: location - -> FJ_FortJoy_Main (1 KB)\n')
        for i in range(10):   # load screen
            t += 1.0; f.write(ts(t) + 'NVN PERF v=1 sync=0 frames=60 wall_ms=1000.0 fps=60.00 submit_cpu_ms=0.2 present_cpu_ms=0.3 gpu_ms=1.30 gpu_samples=60 submits=1 waits=1 ops=20 folded=1 passes=2.0 draws=13.0 dispatches=0\n')
        h = 75.0
        for i in range(160):  # the DRS climb, then the plateau
            gpu = 11.85 * (0.35 + 0.65 * (h / 100) ** 2); fps = min(120.0, 1000.0 / gpu)
            wall = 60 * 1000.0 / fps; t += wall / 1000.0
            f.write(ts(t) + 'NVN PERF v=1 sync=0 frames=60 wall_ms=%.1f fps=%.2f submit_cpu_ms=%.2f present_cpu_ms=0.3 gpu_ms=%.2f gpu_samples=60 submits=1 waits=1 ops=1200 folded=1 passes=13.0 draws=1250.0 dispatches=0\n' % (wall, fps, gpu, gpu))
            if i % 12 == 0:
                f.write(ts(t) + '[dm] [quality] GPU frame time (10 s): the game reads %.2f ms (min 1.0, max 29.0, 40 samples), the backend measured %.2f ms; DRS current 1x1 = %.1f %% of 2160x972, target 15.83 ms, step up under 15.08 ms\n' % (gpu, gpu, h))
                f.write(ts(t) + '[nr] growth (10 s, frame 1, load 3 s ago, loads 1, level FJ_FortJoy_Main): sizes pipelines=%d images=600 image_mb=255.0; since load pipelines+%d; walks retire cleanup 9.5 us a frame (600 frames); gpu alu 100.0 (5.000 ns/iter, 80 us, n 16384, runs 3, recal 1) copy 110 us 76.2 GB/s index 100.0 (runs 3); drs %.1f %% target 15.83 ms; thermal 0 headroom 0.41; mem rss 1650 MB; cpu MHz p0 1200 p7 2900; cost 140 us (140.0 us a line on average)\n' % (300 + i // 12, i // 12, h))
            h = min(100.0, h + 1.25 * wall / 1000.0)
    out = report(p)
    text = '\n'.join(out)
    assert 'DRS: started at 75.0 %' in text, text
    assert 'plateau flat' in text, text
    assert 'pipelines 300 -> 313 (only grew)' in text, text
    assert 'probe ALU index 100.0 -> 100.0' in text and 'probe copy 76.2 -> 76.2 GB/s' in text, text
    g = parse_growth('[nr] growth (10 s, frame 9, load screen now, loads 2, level -): sizes a=1 b_mb=2.5; since load a+3 b_mb-0.5; gpu alu ? -1.0 (0.000 ns/iter, -1 us, n 4096, runs 0, recal 0) copy -1 us -1.0 GB/s index ? -1.0 (runs 0); thermal -1 headroom nan; mem rss 900 MB; cost 50 us')
    assert g['sizes'] == {'a': 1.0, 'b_mb': 2.5} and g['since'] == {'a': 3.0, 'b_mb': -0.5} and g['alu_index'] == -1.0 and g['rss'] == 900.0 and g['cost_us'] == 50.0, g
    print('dos2_drift.py self-test: PASS (%d report lines)' % len(out))


def main(argv):
    if '--self-test' in argv:
        self_test(); return 0
    csv = None; all_sizes = '--all-sizes' in argv
    if '--csv' in argv: csv = argv[argv.index('--csv') + 1]
    logs = [a for a in argv if not a.startswith('--') and a != csv]
    if not logs:
        print(__doc__ or 'usage: dos2_drift.py <log> ... [--csv out.csv] [--all-sizes] | --self-test'); return 2
    for i, path in enumerate(logs):
        for l in report(path, csv if i == 0 else None, all_sizes): print(l)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
