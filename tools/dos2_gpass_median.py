"""DOS2 GPASS medians (N6 NR-HACKS cost table): per pass (shader name / hash, size, attachments, ordinal) and per group, median us over
the 3D frames of an NVN GPASS log (DM_GFX_PASS_PROF). Usage: dos2_gpass_median.py <log> [min passes per frame, default 29]"""
import re, sys, collections, statistics
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import dos2_gpu_breakdown as B
GP = B.GP; GS = B.GS
def load(path):
    frames = collections.defaultdict(list); spans = {}
    for line in open(path, errors='replace'):
        m = GP.search(line)
        if m:
            f, s, i, kind, us, w, h, att, fmt, d, v, first, last = m.groups()
            ps = re.search(r'4:(?:F1A\.)?([0-9a-f]{16})', first); fsn = B.name_of(ps.group(1)[:12]) if ps else '-'
            if 'F1A.' in first: fsn = 'F1A(LUT+Sight)'
            frames[int(f)].append((int(i), int(us), int(w), int(h), int(att), int(fmt), int(d), int(v), fsn))
            continue
        m = GS.search(line)
        if m: spans[int(m.group(1))] = (int(m.group(3)), int(m.group(5)))
    return frames, spans
def main():
    path = sys.argv[1]; minp = int(sys.argv[2]) if len(sys.argv) > 2 else 29
    frames, spans = load(path)
    camp = [f for f in frames if len(frames[f]) >= minp and any(r[2] * r[3] >= 1000 * 500 and r[2] != 2400 for r in frames[f])]
    print(f'{path}: {len(camp)} 3D frames of {len(frames)}; span median {statistics.median([spans[f][1] for f in camp if f in spans]) if camp else 0}')
    per = collections.defaultdict(list); grp = collections.defaultdict(list)
    for f in camp:
        g = collections.Counter(); seen = collections.Counter()
        for i, us, w, h, att, fmt, d, v, fsn in frames[f]:
            key = (fsn, f'{w}x{h}', att)
            seen[key] += 1
            per[(key, seen[key])].append((us, d, v))
            g[B.group(i, w, h, fsn, d, fsn, [0] * att, {})] += us
        for k, us in g.items(): grp[k].append(us)
    tot = 0
    rows = []
    for (key, n), L in per.items():
        if len(L) < len(camp) * 0.5: continue
        us = statistics.median([x[0] for x in L]); d = statistics.median([x[1] for x in L]); v = statistics.median([x[2] for x in L])
        rows.append((us, key, n, d, v, len(L)))
    for us, key, n, d, v, cnt in sorted(rows, reverse=True):
        tot += us
        print(f'  {us:7.0f} us  {key[0]:<28s} {key[1]:>10s} att={key[2]} #{n} draws={d:.0f} verts={v:.0f} (in {cnt})')
    print(f'  sum of medians {tot:.0f} us')
    for k, L in sorted(grp.items(), key=lambda kv: -statistics.median(kv[1])):
        print(f'  group {k:<20s} {statistics.median(L)/1000:6.2f} ms (in {len(L)})')
main()
