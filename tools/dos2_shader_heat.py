#!/usr/bin/env python3
"""NR-HEAT: rank DOS2's shaders and passes by GPU time, overall and per location, from the players' / owner's logs.

Input (any mix, auto-detected):
  shader-heat.tsv / shader-heat.1.tsv  the DM_NR_HEAT log (Download/DivinityOriginalSin2Mobile/logs; format in dm/gfx/nr/nr_heat.h)
  *.log / *.dm.log                      a boot.log / dm.log with NVN GPASS lines (DM_GFX_PASS_PROF runs): each GPASS frame becomes a sample;
                                        the location is taken from "[nr] heat: location A -> B" lines when the log has them, else the file name
Optional:
  --programs work/shaders-jobs/programs.tsv   engine / material names of the keys (setup writes it; the keys are the same for every player)
  --pack work/shaders-vk/shaders.xgspack      (or a directory of <key>.xgs) static SPIR-V cost of each key: instructions, ALU, transcendental,
                                              texture, branches, loops, dynamic-index UBO loads, bitcasts, live SSA values (straight-line estimate)
Ranking: a pass's GPU time (two timestamps outside the render pass) is split among the programs that drew in it - by draw count
('draw ms'), by vertices ('vert ms'), and, with --pack, by draws x (FS + VS static cost) ('est ms', the default order: "pass time x share of
draws x cost"). Times are per sampled frame (mean over the level's sampled frames). Inside a pass nothing is timed (tilers bin the whole pass).

usage: dos2_shader_heat.py <files...> [--programs P] [--pack K] [--top 30] [--level NAME] [--min-passes 0] [--json out.json] [--relabel NAME@FROM-TO]
                           [--keys-out top_fs.txt] [--self-test]
"""
import argparse, collections, glob, json, math, os, re, struct, sys

GPASS = re.compile(r'NVN GPASS frame=(\d+) slot=\d+ ring=\d+ i=\d+ kind=(\w+) us=(\d+)(?: (\d+)x(\d+) att=(\d+) fmt=(\d+) draws=(\d+) verts=(\d+) '
                   r'first=(\S+) last=(\S+))?(?: cs=(\S+))?')
SUMMARY = re.compile(r'NVN GPASS frame=(\d+) .* summary .* span_us=(-?\d+)')
LOCATION = re.compile(r'\[nr\] heat: location (.+?) -> (.+?) \(')
FUSED = re.compile(r'^F\d+[A-Z]?\.')   # NB-18 fused post bundles: F1.<key>, F1A.<key>, F2.<key> ...
NR_SH = re.compile(r'\{"(\w+)",\s*"([0-9a-f]{16})"')
HERE = os.path.dirname(os.path.abspath(__file__))


class Row:
    __slots__ = ('frames', 'passes', 'draws', 'verts', 'us_d', 'us_v', 'us_p', 'us_c')

    def __init__(self): self.frames = self.passes = self.draws = self.verts = 0; self.us_d = self.us_v = self.us_p = self.us_c = 0.0

    def add(self, f, p, d, v, ud, uv, up, uc):
        self.frames += f; self.passes += p; self.draws += d; self.verts += v; self.us_d += ud; self.us_v += uv; self.us_p += up; self.us_c += uc


COST_FIELDS = ('inst', 'alu', 'trans', 'tex', 'branch', 'loops', 'ubo', 'ubo_dyn', 'bitcast', 'cost')


class Level:
    def __init__(self):
        self.frames = 0; self.passes = 0; self.draws = 0; self.span = 0.0; self.pass_us = 0.0; self.compute_us = 0.0
        self.rows = collections.defaultdict(Row)   # (kind, wxh, fmt, att, vs, fs) -> Row
        self.fps = []; self.sources = set()


class Data:
    def __init__(self):
        self.levels = collections.defaultdict(Level)
        self.src = {}            # key -> x / nr / f
        self.stage = {}; self.words = {}
        self.dcost = {}          # key -> static counts of the SPIR-V the device ran (S lines)
        self.cost_kind = set()   # how the 'est' column was made: device (A us_cost), pack (GPASS input), draws
        self.headers = []; self.locations = []; self.warnings = []


# ---------------- input: the heat file ----------------
def relabel_of(spec):
    """[NR-BN] --relabel NAME@FROM-TO: blocks (B) / sampled frames (F) whose t_s (seconds since start) is in [FROM, TO] count as level NAME.
    For logs whose location vote missed the real level (18.11 play: Fort Joy logged as TUT_Tutorial_A until the map was opened)."""
    out = []
    for x in spec or []:
        name, _, rng = x.partition('@'); lo, _, hi = rng.partition('-')
        out.append((name, float(lo or 0), float(hi or 1e18)))
    return out


def read_heat(path, D, min_passes, relabel=()):
    level_of_block = None
    def lv_at(name, t_s):
        for n, lo, hi in relabel:
            if lo <= t_s <= hi: return n
        return name
    skip = False
    for ln, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
        f = line.rstrip('\n').split('\t')
        t = f[0]
        try:
            if t == 'H': D.headers.append((os.path.basename(path), dict(x.split('=', 1) for x in f[1:] if '=' in x)))
            elif t == 'L': D.locations.append((os.path.basename(path), f[3], float(f[2]), f[5] if len(f) > 5 else '-'))
            elif t == 'S':
                D.src[f[1]] = f[4]; D.stage[f[1]] = int(f[2]); D.words[f[1]] = int(f[3])
                if len(f) >= 15 and f[5] != '-': D.dcost[f[1]] = dict(zip(COST_FIELDS, map(int, f[5:15])))
            elif t == 'F':
                L = D.levels[lv_at(f[3], float(f[2]))]
                if float(f[12]) > 0: L.fps.append(float(f[12]))
            elif t == 'B':
                level_of_block = lv_at(f[3], float(f[2])); L = D.levels[level_of_block]; L.sources.add(os.path.basename(path))
                passes = int(f[5]); frames = int(f[4])
                skip = min_passes and frames and passes / frames < min_passes
                if skip: continue
                L.frames += frames; L.passes += passes; L.draws += int(f[6]); L.span += float(f[7]); L.pass_us += float(f[8]); L.compute_us += float(f[9])
            elif t == 'A':
                if level_of_block is None or skip: continue
                k = (f[1], f[2], f[3], f[4], f[5], f[6])
                uc = float(f[14]) if len(f) > 14 else float(f[11])
                D.cost_kind.add('device' if len(f) > 14 else 'draws')
                D.levels[level_of_block].rows[k].add(int(f[7]), int(f[8]), int(f[9]), int(f[10]), float(f[11]), float(f[12]), float(f[13]), uc)
        except (IndexError, ValueError) as e:
            D.warnings.append('%s:%d: %s (%s)' % (path, ln, t, e))


# ---------------- input: GPASS lines of a dm.log ----------------
def read_gpass(path, D, min_passes, costfn=None):
    """Each NVN GPASS frame = one sample. A pass's programs: the distinct (VS, FS) of its first and last draw (all the line tells),
    its draws split evenly among them (the heat file has the real counts); with costfn the pass time is also split by static cost
    (est), as the device does for the heat file."""
    cur_level = 'gpass:' + os.path.basename(path)
    frames = collections.OrderedDict()   # frame -> [level, [pass tuples]]
    spans = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LOCATION.search(line)
        if m: cur_level = m.group(2); continue
        if 'NVN GPASS frame=' not in line or 'boundary' in line: continue
        if ' summary ' in line:
            ms = SUMMARY.search(line)
            if ms: spans[int(ms.group(1))] = spans.get(int(ms.group(1)), 0.0) + max(0.0, float(ms.group(2)))
            continue
        m = GPASS.search(line)
        if not m: continue
        fr = int(m.group(1)); kind = m.group(2); us = float(m.group(3))
        e = frames.setdefault(fr, [cur_level, []])
        if kind == 'compute':
            e[1].append(('c', '0x0', '0', '0', [('-', m.group(12) or '-')], 1, 0, us))
        elif m.group(4):
            progs = []
            for s in (m.group(10), m.group(11)):
                if s.startswith('builtin'): continue
                d = dict(x.split(':', 1) for x in s.split(';') if ':' in x)
                p = (d.get('0', '-'), d.get('4', '-'))
                if p not in progs: progs.append(p)
            e[1].append(('p', '%sx%s' % (m.group(4), m.group(5)), m.group(7), m.group(6), progs, int(m.group(8)), int(m.group(9)), us))
    n = 0
    for fr, (lv, passes) in frames.items():
        if min_passes and sum(1 for p in passes if p[0] == 'p') < min_passes: continue
        L = D.levels[lv]; L.frames += 1; n += 1; L.sources.add(os.path.basename(path)); seen = set(); L.span += spans.get(fr, 0.0)
        for kind, wh, fmt, att, progs, draws, verts, us in passes:
            if kind == 'c': L.compute_us += us
            else: L.passes += 1; L.draws += draws; L.pass_us += us
            plist = progs or [('-', '-')]; m = len(plist)
            costs = [None] * m
            if costfn and progs:
                for j, (vs, fs) in enumerate(plist):
                    cf, cv = costfn(fs), costfn(vs)
                    if cf is not None or cv is not None: costs[j] = (cf or 0) + (cv or 0)
            known = [c for c in costs if c is not None]
            mean = sum(known) / len(known) if known else None
            ws = [(c if c is not None else mean) for c in costs] if known else None
            sw = sum(ws) if ws else 0
            for j, (vs, fs) in enumerate(plist):
                k = (kind, wh, fmt, att, vs, fs)
                uc = us * ws[j] / sw if sw else us / m
                L.rows[k].add(0 if k in seen else 1, 1, draws // m, verts // m, us / m, us / m, us, uc); seen.add(k)
    D.cost_kind.add('pack' if costfn else 'draws')
    return n


def is_heat(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            for i, line in enumerate(f):
                if line[:2] in ('H\t', 'B\t', 'F\t', 'A\t', 'L\t', 'S\t'): return True
                if i > 20: return False
    except OSError:
        return False
    return False


# ---------------- names and static cost ----------------
def load_programs(path):
    names = collections.defaultdict(list)
    if not path or not os.path.isfile(path): return names
    for line in open(path, encoding='utf-8', errors='replace'):
        f = line.rstrip('\n').split('\t')
        if len(f) < 5 or f[0] == 'label': continue
        label = f[0].split(':', 1)[-1]
        label = os.path.splitext(label.rsplit('/', 1)[-1])[0]
        for k in f[1:5]:
            if k != '-' and label not in names[k]: names[k].append(label)
    return names


def load_nr_sh(root):
    p = os.path.join(root, 'dm', 'gfx', 'nr', 'nr_sh.c')
    if not os.path.isfile(p): return {}
    return {k: n for n, k in NR_SH.findall(open(p, encoding='utf-8', errors='replace').read())}


def pack_index(path):
    if not path: return None
    if os.path.isdir(path): return ('dir', path)
    data = open(path, 'rb').read()
    magic, ver, n, _ = struct.unpack_from('<4I', data, 0)
    if magic != 0x50534758 or ver != 1: sys.exit('%s: not an xgspack v1' % path)
    idx = {}
    for i in range(n):
        k, o, sz, _, _ = struct.unpack_from('<32sQIIQ', data, 16 + 56 * i)
        idx[k.rstrip(b'\0').decode('ascii', 'replace')] = (o, sz)
    return ('pack', data, idx)


def bundle(P, key):
    if P is None: return None
    if P[0] == 'dir':
        f = os.path.join(P[1], key + '.xgs')
        return open(f, 'rb').read() if os.path.isfile(f) else None
    o = P[2].get(key)
    return P[1][o[0]:o[0] + o[1]] if o else None


TEX = set(range(87, 99)) | set(range(305, 321))
ALU = set(range(109, 125)) | set(range(126, 153)) | set(range(154, 158)) | set(range(164, 192)) | set(range(194, 206)) | set(range(207, 216)) | {169}
TRANS_EXT = {13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 66, 67, 69}
NO_RESULT = {8, 62, 63, 99, 218, 219, 224, 225, 228, 246, 247, 249, 250, 251, 252, 253, 254, 255, 317, 4416, 5380}


def spirv_cost(spv):
    """Static counts of a SPIR-V module (function bodies only)."""
    w = struct.unpack_from('<%dI' % (len(spv) // 4), spv)
    if len(w) < 5 or w[0] != 0x07230203: return None
    c = dict(inst=0, alu=0, trans=0, tex=0, branch=0, loops=0, ubo=0, ubo_dyn=0, bitcast=0, kill=0, live=0)
    consts, ptr_class = set(), {}
    glsl = set()
    i = 5; in_fn = False; body = []
    while i < len(w):
        n = w[i] >> 16; op = w[i] & 0xffff
        if n == 0 or i + n > len(w): break
        ins = w[i:i + n]
        if op == 11 and n > 2 and bytes(struct.pack('<%dI' % (n - 2), *ins[2:])).startswith(b'GLSL.std.450'): glsl.add(ins[1])
        elif op in (43, 44, 46, 41, 42, 50, 51, 52): consts.add(ins[2])   # OpConstant*, OpSpecConstant*
        elif op == 59: ptr_class[ins[2]] = ins[3]                          # OpVariable: result -> storage class
        elif op in (65, 66) and n > 3:                                       # OpAccessChain: the class of its base, dynamic index?
            ptr_class[ins[2]] = ptr_class.get(ins[3], -1)
            if any(x not in consts for x in ins[4:]): ptr_class[ins[2]] = (ptr_class[ins[2]], 'dyn')
        if op == 54: in_fn = True; body = []
        elif op == 56: in_fn = False; c['live'] = max(c['live'], live_est(body))
        elif in_fn:
            body.append(ins)
            if op not in (248, 8, 317, 247, 249, 59): c['inst'] += 1
            if op in ALU: c['alu'] += 1
            if op == 124: c['bitcast'] += 1
            if op in TEX: c['tex'] += 1
            if op in (250, 251): c['branch'] += 1
            if op == 246: c['loops'] += 1
            if op in (252, 4416, 5380): c['kill'] += 1
            if op == 12 and n > 4:
                if ins[3] in glsl and ins[4] in TRANS_EXT: c['trans'] += 1
                else: c['alu'] += 1
            if op == 61 and n > 3:
                pc = ptr_class.get(ins[3])
                base = pc[0] if isinstance(pc, tuple) else pc
                if base in (2, 9, 12):   # Uniform, PushConstant, StorageBuffer
                    c['ubo'] += 1
                    if isinstance(pc, tuple): c['ubo_dyn'] += 1
        i += n
    c['cost'] = c['alu'] + 4 * c['trans'] + 4 * c['tex'] + 2 * c['ubo_dyn'] + 2 * c['branch']
    return c


def live_est(body):
    """Max SSA values alive at once, walking the function in its written order (no loops / branches modelled): a register proxy."""
    defs, last = {}, {}
    for pos, ins in enumerate(body):
        op = ins[0] & 0xffff; n = len(ins)
        if op == 248: start = 2
        elif op in NO_RESULT: start = 1
        else:
            if n > 2: defs[ins[2]] = pos
            start = 3
        for x in ins[start:]:
            if x in defs: last[x] = pos
    ev = collections.Counter()
    for k, d in defs.items():
        if k in last and last[k] > d: ev[d] += 1; ev[last[k]] -= 1
    cur = best = 0
    for pos in sorted(ev):
        cur += ev[pos]; best = max(best, cur)
    return best


def spirv_of_bundle(b):
    if not b or len(b) < 32: return None
    h = struct.unpack_from('<8I', b, 0)
    off = 32 + h[3] * 32 + h[4] * 4 + h[5]
    return b[off:off + h[6]] if h[6] else b[off:]


# ---------------- report ----------------
def short(names, key, n=46):
    if key in ('-', '', None): return '-'
    k = key.split('.', 1)[-1] if (FUSED.match(key) or key.startswith('x.')) else key
    v = names.get(k) or []
    s = v[0] if v else '?'
    if len(v) > 1: s += ' (+%d)' % (len(v) - 1)
    return s[:n]


def make_cost(D, P, cache):
    """key -> static counts: the pack's bundle (adds live), else what the device counted (S lines), else None."""
    def cost(key):
        if key in ('-', '', None): return None
        if key in cache: return cache[key]
        k = key.split('.', 1)[-1] if FUSED.match(key) else key
        c = spirv_cost(spirv_of_bundle(bundle(P, k)) or b'') if P else None
        if c is None: c = D.dcost.get(key)
        cache[key] = c; return c
    return cost


def analyse(D, cost):
    """Rows -> ms a sampled frame. est = the cost-weighted split made inside each pass (device: A us_cost; GPASS input with a pack:
    read_gpass), else the draw share."""
    out = {}
    for lv, L in D.levels.items():
        if not L.frames: continue
        res = []
        for k, r in L.rows.items():
            res.append(dict(level=lv, kind=k[0], wxh=k[1], fmt=k[2], att=k[3], vs=k[4], fs=k[5], frames=r.frames, passes=r.passes,
                            draws=r.draws, verts=r.verts, draw_ms=r.us_d / L.frames / 1e3, vert_ms=r.us_v / L.frames / 1e3,
                            pass_ms=r.us_p / L.frames / 1e3, est_ms=r.us_c / L.frames / 1e3))
        out[lv] = res
    return out


def fmt_cost(c):
    if not c: return ''
    return 'i%d a%d t%d x%d b%d l%d live%s u%d/%d bc%d' % (c['inst'], c['alu'], c['trans'], c['tex'], c['branch'], c['loops'], c.get('live', '?'),
                                                           c['ubo'], c['ubo_dyn'], c['bitcast'])


def src_of(D, nrsh, key):
    if key in ('-', ''): return ''
    if FUSED.match(key): return 'f'
    if key in nrsh: return 'nr'
    return D.src.get(key, 'x' if D.src else '')


def print_level(title, frames_ms, rows, D, names, cost, nrsh, top, order):
    print('\n== %s ==' % title)
    print(frames_ms)
    sig = collections.defaultdict(lambda: [0.0, 0, collections.Counter()])
    for r in rows:
        s = sig[(r['kind'], r['wxh'], r['fmt'], r['att'])]; s[0] += r['draw_ms']; s[1] += r['draws']; s[2][r['fs']] += r['draw_ms']
    tot = sum(r['draw_ms'] for r in rows) or 1
    print('passes by signature (kind WxH fmt att: ms a sampled frame, share, the FS with the most time):')
    for (kind, wxh, fmt, att), (ms, dr, fsc) in sorted(sig.items(), key=lambda x: -x[1][0])[:12]:
        fs = fsc.most_common(1)[0][0]
        print('  %s %-10s fmt %-3s att %-2s %7.3f ms %5.1f%%  draws %-6d %s %s' % (kind, wxh, fmt, att, ms, 100 * ms / tot, dr, fs, short(names, fs)))
    print('top %d programs by %s (ms a sampled frame; est = pass time x draws x static cost share):' % (top, order))
    print('  %-3s %7s %6s %7s %7s %7s %8s %9s  %-24s %-16s %-4s %-46s %-16s %s' % ('#', 'est_ms', 'share', 'draw_ms', 'vert_ms', 'pass_ms', 'draws/f', 'verts/f',
                                                                                'pass', 'FS / CS', 'src', 'FS name', 'VS', 'FS static cost'))
    for i, r in enumerate(sorted(rows, key=lambda r: -r[order])[:top], 1):
        fr = max(1, r['frames'])
        print('  %-3d %7.3f %5.1f%% %7.3f %7.3f %7.3f %8.1f %9.0f  %-24s %-16s %-4s %-46s %-16s %s' % (
            i, r['est_ms'], 100 * r['est_ms'] / tot, r['draw_ms'], r['vert_ms'], r['pass_ms'], r['draws'] / fr, r['verts'] / fr,
            '%s %s f%s a%s' % (r['kind'], r['wxh'], r['fmt'], r['att']), r['fs'][:16], src_of(D, nrsh, r['fs']), short(names, r['fs']), r['vs'][:16],
            fmt_cost(cost(r['fs'])) if r['fs'] != '-' else ''))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('files', nargs='*')
    ap.add_argument('--programs', default='')
    ap.add_argument('--pack', default='')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--level', default='')
    ap.add_argument('--min-passes', type=int, default=0, help='drop frames (GPASS) / blocks (heat) with fewer passes a frame (menus, loading)')
    ap.add_argument('--json', default='')
    ap.add_argument('--keys-out', default='', help='write the top FS keys (one a line) for other tools (dos2_spvopt_cost.py, malioc)')
    ap.add_argument('--relabel', action='append', default=[], metavar='NAME@FROM-TO',
                    help='heat blocks / frames with t_s in FROM..TO seconds count as level NAME (a log whose location vote missed the level)')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test: return self_test()
    files = []
    for f in a.files: files += sorted(glob.glob(f)) or [f]
    if not files: ap.error('no input files')
    root = os.path.dirname(HERE)
    if not a.programs:
        for c in (os.path.join(root, '..', 'work', 'shaders-jobs', 'programs.tsv'), os.path.join(root, 'work', 'shaders-jobs', 'programs.tsv')):
            if os.path.isfile(c): a.programs = c; break
    D = Data()
    names = load_programs(a.programs)
    nrsh = load_nr_sh(root)
    P = pack_index(a.pack)
    cache = {}
    cost = make_cost(D, P, cache)
    gcost = (lambda k: (cost(k) or {}).get('cost')) if P else None
    for f in files:
        if is_heat(f): read_heat(f, D, a.min_passes, relabel_of(a.relabel))
        else:
            n = read_gpass(f, D, a.min_passes, gcost)
            if not n: D.warnings.append('%s: no NVN GPASS frames' % f)
    res = analyse(D, cost)
    order = 'est_ms' if (D.cost_kind - {'draws'}) else 'draw_ms'
    for w in D.warnings[:10]: print('warning:', w)
    for name, h in D.headers[-3:]: print('%s: run mode=%s period %s s, GPU %s, app %s' % (name, h.get('mode'), h.get('period_s'), h.get('gpu'), h.get('app')))
    if D.locations: print('locations: ' + ' -> '.join('%s (%.0f s)' % (l[1], l[2]) for l in D.locations[-20:]))
    print('names: %s; static cost: %s; est from: %s' % (a.programs or 'none (--programs)', a.pack or 'the device S lines' if D.dcost else a.pack or 'none',
                                                         ', '.join(sorted(D.cost_kind)) or '-'))
    levels = [lv for lv in res if not a.level or lv == a.level]
    allrows, total_frames = [], 0
    for lv in levels: total_frames += D.levels[lv].frames
    # overall: each level's per-frame ms weighted by its sampled frames
    merged = {}
    for lv in levels:
        wf = D.levels[lv].frames / max(1, total_frames)
        for r in res[lv]:
            k = (r['kind'], r['wxh'], r['fmt'], r['att'], r['vs'], r['fs'])
            m = merged.setdefault(k, dict(r, level='*', draw_ms=0.0, vert_ms=0.0, pass_ms=0.0, est_ms=0.0, frames=0, passes=0, draws=0, verts=0))
            for f in ('draw_ms', 'vert_ms', 'pass_ms', 'est_ms'): m[f] += r[f] * wf
            for f in ('frames', 'passes', 'draws', 'verts'): m[f] += r[f]
    allrows = list(merged.values())
    span = sum(D.levels[lv].span for lv in levels) / max(1, total_frames) / 1e3
    pms = sum(D.levels[lv].pass_us + D.levels[lv].compute_us for lv in levels) / max(1, total_frames) / 1e3
    print_level('all locations (%d sampled frames, %d levels)' % (total_frames, len(levels)),
                'a sampled frame: GPU span %.2f ms, passes + dispatches %.2f ms' % (span, pms), allrows, D, names, cost, nrsh, a.top, order)
    for lv in sorted(levels, key=lambda x: -D.levels[x].frames):
        L = D.levels[lv]
        fps = sorted(L.fps)
        print_level('%s (%d sampled frames, from %s)' % (lv, L.frames, ', '.join(sorted(L.sources))),
                    'a sampled frame: GPU span %.2f ms, passes %.2f ms + dispatches %.2f ms, %.0f passes, %.0f draws%s' % (
                        L.span / L.frames / 1e3, L.pass_us / L.frames / 1e3, L.compute_us / L.frames / 1e3, L.passes / L.frames, L.draws / L.frames,
                        ', fps at the samples median %.0f' % fps[len(fps) // 2] if fps else ''), res[lv], D, names, cost, nrsh, min(a.top, 15), order)
    # NR-33 candidates: fragment / compute programs still translated, by summed time
    by_fs = collections.defaultdict(float)
    for r in allrows:
        if r['fs'] != '-': by_fs[r['fs']] += r[order]
    tot = sum(r[order] for r in allrows) or 1
    print('\n== NR-33 candidates: FS / CS by %s over all locations (translated only; nr = NR-04 table, f = fused post) ==' % order)
    cands = []
    for k, ms in sorted(by_fs.items(), key=lambda x: -x[1]):
        s = src_of(D, nrsh, k)
        if s in ('nr', 'f'): continue
        cands.append(k)
        if len(cands) <= a.top:
            print('  %-3d %7.3f ms %5.1f%%  %-16s %-46s %s' % (len(cands), ms, 100 * ms / tot, k[:16], short(names, k), fmt_cost(cost(k))))
    if a.keys_out:
        with open(a.keys_out, 'w') as f: f.write('\n'.join(cands[:a.top]) + '\n')
        print('top %d FS keys -> %s' % (min(a.top, len(cands)), a.keys_out))
    if a.json:
        json.dump(dict(levels={lv: dict(frames=D.levels[lv].frames, rows=res[lv]) for lv in levels}, overall=allrows, candidates=cands[:a.top],
                       costs={k: v for k, v in cache.items() if v}, names={k: names.get(k, []) for k in cands[:a.top]}), open(a.json, 'w'), indent=0)
    return 0


def self_test():
    import tempfile
    d = tempfile.mkdtemp()
    p = os.path.join(d, 'shader-heat.tsv')
    with open(p, 'w') as f:
        f.write('H\tv=1\tunix=1\tmode=1\tperiod_s=20\tgpu=Adreno\tapp=t\n')
        f.write('L\t1\t5\tFJ_FortJoy_Main\t900\t-\n')
        f.write('S\taaaaaaaaaaaaaaaa\t4\t500\tx\t100\t40\t2\t3\t1\t0\t5\t1\t0\t60\nS\t62d683cc49953ad6\t4\t300\tnr\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\n')
        f.write('F\t1\t30\tFJ_FortJoy_Main\t100\t1\t3\t0\t5\t10000\t9000\t0\t60.0\t0\n')
        f.write('B\t1\t300\tFJ_FortJoy_Main\t2\t6\t10\t20000\t18000\t0\n')
        f.write('A\tp\t2400x1080\t37\t2\t1111111111111111\taaaaaaaaaaaaaaaa\t2\t2\t8\t800\t12000\t12000\t16000\t14000\n')
        f.write('A\tp\t2400x1080\t37\t2\t2222222222222222\tbbbbbbbbbbbbbbbb\t2\t2\t2\t200\t4000\t4000\t16000\t2000\n')
        f.write('A\tp\t2160x972\t63\t1\t3333333333333333\t62d683cc49953ad6\t2\t2\t0\t0\t2000\t2000\t2000\t2000\n')
    D = Data(); read_heat(p, D, 0)
    L = D.levels['FJ_FortJoy_Main']
    assert L.frames == 2 and L.passes == 6 and len(L.rows) == 3, (L.frames, L.passes, len(L.rows))
    assert D.dcost['aaaaaaaaaaaaaaaa']['cost'] == 60 and '62d683cc49953ad6' not in D.dcost and D.cost_kind == {'device'}
    res = analyse(D, make_cost(D, None, {}))
    r = {x['fs']: x for x in res['FJ_FortJoy_Main']}
    assert abs(r['aaaaaaaaaaaaaaaa']['draw_ms'] - 6.0) < 1e-9 and abs(r['bbbbbbbbbbbbbbbb']['draw_ms'] - 2.0) < 1e-9
    assert abs(r['aaaaaaaaaaaaaaaa']['est_ms'] - 7.0) < 1e-9   # the device's cost-weighted split (us_cost)
    D3 = Data(); open(p + '.old', 'w').write('B\t1\t1\tX\t1\t1\t1\t1\t1\t0\nA\tp\t1x1\t1\t1\t-\taa\t1\t1\t1\t1\t5\t5\t5\n')
    read_heat(p + '.old', D3, 0); assert D3.levels['X'].rows[('p', '1x1', '1', '1', '-', 'aa')].us_c == 5 and D3.cost_kind == {'draws'}   # no us_cost column
    assert src_of(D, {'62d683cc49953ad6': 1}, '62d683cc49953ad6') == 'nr' and src_of(D, {}, 'F1.x') == 'f'
    g = os.path.join(d, 'x.dm.log')
    with open(g, 'w') as f:
        f.write('I dm-vulkan: NVN GPASS frame=7 slot=0 ring=1 i=0 kind=pass us=100 2400x1080 att=2 fmt=37 draws=4 verts=40 first=0:aa;4:bb last=0:aa;4:cc mixed=1 dz=0 rep=0\n')
        f.write('[dm] [nr] heat: location - -> TUT_Tutorial_A (2000 KB read lately\n')
        f.write('I dm-vulkan: NVN GPASS frame=8 slot=0 ring=1 i=0 kind=compute us=30 cs=dd groups=1,1,1\n')
        f.write('I dm-vulkan: NVN GPASS frame=8 slot=0 ring=1 i=1 kind=pass us=50 64x64 att=1 fmt=6 draws=0 verts=0 first=builtin-clear/no-game-draw last=builtin-clear/no-game-draw mixed=0 dz=0 rep=0\n')
    D2 = Data(); assert read_gpass(g, D2, 0) == 2 and not is_heat(g) and is_heat(p)
    a = D2.levels['gpass:x.dm.log']; t = D2.levels['TUT_Tutorial_A']
    assert a.frames == 1 and len(a.rows) == 2 and abs(a.rows[('p', '2400x1080', '37', '2', 'aa', 'bb')].us_d - 50) < 1e-9
    assert t.frames == 1 and abs(t.compute_us - 30) < 1e-9 and ('p', '64x64', '6', '1', '-', '-') in t.rows
    D4 = Data(); read_gpass(g, D4, 0, {'bb': 30, 'cc': 10, 'aa': 0}.get)   # cost-weighted inside the pass: 100 us -> 75 / 25
    rr = D4.levels['gpass:x.dm.log'].rows
    assert abs(rr[('p', '2400x1080', '37', '2', 'aa', 'bb')].us_c - 75) < 1e-9 and abs(rr[('p', '2400x1080', '37', '2', 'aa', 'cc')].us_c - 25) < 1e-9
    # SPIR-V counter on a hand-made module: OpFunction, a label, an FAdd of two loaded uniforms, a sample, return
    def ins(op, *a): return [((len(a) + 1) << 16) | op] + list(a)
    words = [0x07230203, 0x10000, 0, 100, 0]
    words += ins(59, 50, 51, 2)            # %51 = OpVariable Uniform
    words += ins(43, 10, 60, 0)            # %60 = OpConstant 0
    words += ins(54, 1, 2, 0, 3)           # OpFunction
    words += ins(248, 4)                   # OpLabel
    words += ins(65, 52, 70, 51, 60)       # %70 = OpAccessChain %51 %60 (constant index)
    words += ins(65, 52, 71, 51, 80)       # %71 = OpAccessChain %51 %80 (dynamic)
    words += ins(61, 10, 72, 70)           # %72 = OpLoad %70
    words += ins(61, 10, 73, 71)           # %73 = OpLoad %71
    words += ins(129, 10, 74, 72, 73)      # %74 = OpFAdd
    words += ins(87, 11, 75, 90, 74)       # %75 = OpImageSampleImplicitLod
    words += ins(124, 10, 76, 75)          # OpBitcast
    words += ins(253)                      # OpReturn
    words += ins(56)                       # OpFunctionEnd
    c = spirv_cost(struct.pack('<%dI' % len(words), *words))
    assert c['tex'] == 1 and c['alu'] == 2 and c['ubo'] == 2 and c['ubo_dyn'] == 1 and c['bitcast'] == 1 and c['live'] >= 2, c
    assert c['inst'] == 8, c   # 2 access chains, 2 loads, FAdd, sample, bitcast, return
    print('dos2_shader_heat self-test: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
