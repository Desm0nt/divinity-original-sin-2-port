#!/usr/bin/env python3
"""[NR-16] Map a simpleperf profile of the DOS2 port onto the guest modules (docs/dos2/nr/NR-16.md, "simpleperf").

The guest's main / sdk images live in anonymous memory (dm/host/loader.c copies them there), so simpleperf sees their samples as
"unknown" symbols at absolute addresses. The run's own log has the bases ("[dm] main: base 0x6f417e1000 size 0x335c000"); with them
every address becomes main+0x... / sdk+0x..., and with a function list (tools/ghidra_export_functions.py on our own headless Ghidra
project) the containing function's name.

Input: the text of the NDK's simpleperf/report_sample.py (perf-script format: one header line per sample, then "\t<ip> <symbol> (<dso>)"
lines, the first one the sample's own pc, the rest the call chain), and the dm.log of the same run.

Usage:
  python tools/dos2_perf_map.py --log run.dm.log --samples samples.txt [--funcs main=main_funcs.tsv --funcs sdk=sdk_funcs.tsv]
                                [--thread 10936] [--top 40] [--bucket 0x100]
  python tools/dos2_perf_map.py --self-test
Output: self samples by place (module or library), by thread x place, the top functions by self samples (host symbols as simpleperf
named them, guest ones as main+0x... / names) and by inclusive samples (anywhere in the call chain).
"""
import argparse
import bisect
import collections
import re
import sys


def read_bases(log):
    bases = {}
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r'\[dm\] (\w+): base (0x[0-9a-f]+) size (0x[0-9a-f]+)', line)
            if m:
                bases[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))   # the last boot of the log wins
    return bases


def read_funcs(spec):
    """main=path: lines '<offset hex>\\t<name>' (offset from the module base); returns {module: (sorted starts, names)}"""
    out = {}
    for s in spec or []:
        mod, path = s.split('=', 1)
        rows = []
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('#') or not line.strip():
                    continue
                a, _, name = line.rstrip('\n').partition('\t')
                rows.append((int(a, 16), name or a))
        rows.sort()
        out[mod] = ([r[0] for r in rows], [r[1] for r in rows])
    return out


class Mapper:
    def __init__(self, bases, funcs, bucket):
        self.mods = sorted((b, b + s, n) for n, (b, s) in bases.items())
        self.funcs = funcs
        self.bucket = bucket

    def place(self, ip, sym, dso):
        """-> (place, function key)"""
        for lo, hi, name in self.mods:
            if lo <= ip < hi:
                off = ip - lo
                f = self.funcs.get(name)
                if f:
                    i = bisect.bisect_right(f[0], off) - 1
                    if i >= 0:
                        return name, '%s:%s' % (name, f[1][i])
                return name, '%s+0x%x' % (name, off & ~(self.bucket - 1))
        base = dso.rsplit('/', 1)[-1] if dso else '?'
        if base.startswith('[kernel') or dso.startswith('[kernel'):
            base = '[kernel]'
        return base, '%s:%s' % (base, sym if sym and sym != 'unknown' else '0x%x' % ip)


def parse_samples(path):
    """yield (comm, tid, [(ip, sym, dso), ...]) - frame 0 = the sample's pc"""
    head = re.compile(r'^(?P<comm>.*?)\t(?P<pid>\d+)/(?P<tid>\d+) ')
    frame = re.compile(r'^\t\s*(?P<ip>[0-9a-f]+) (?P<sym>.*) \((?P<dso>[^()]*)\)\s*$')
    cur = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                continue
            m = head.match(line)
            if m:
                if cur:
                    yield cur
                cur = (m.group('comm'), int(m.group('tid')), [])
                continue
            m = frame.match(line)
            if m and cur:
                cur[2].append((int(m.group('ip'), 16), m.group('sym'), m.group('dso')))
            elif not line.strip() and cur:
                yield cur
                cur = None
    if cur:
        yield cur


def report(mapper, samples, thread=None, top=40, out=print):
    by_place, by_tp, self_f, incl_f, comm = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter(), {}
    n = 0
    for c, tid, frames in samples:
        if thread and tid != thread or not frames:
            continue
        n += 1
        comm[tid] = c
        p, fn = mapper.place(*frames[0])
        by_place[p] += 1; by_tp[(tid, p)] += 1; self_f[fn] += 1
        for k in {mapper.place(*fr)[1] for fr in frames}:
            incl_f[k] += 1
    if not n:
        out('no samples%s' % (' of thread %d' % thread if thread else ''))
        return {}
    out('%d samples%s' % (n, ' of thread %d' % thread if thread else ''))
    out('self by place: ' + '; '.join('%s %.1f%%' % (p, 100.0 * v / n) for p, v in by_place.most_common(12)))
    tids = collections.Counter()
    for (tid, p), v in by_tp.items():
        tids[tid] += v
    for tid, v in tids.most_common(10):
        places = sorted(((p, c) for (t, p), c in by_tp.items() if t == tid), key=lambda x: -x[1])[:5]
        out('  thread %d %-16s %.1f%%: %s' % (tid, comm.get(tid, '?'), 100.0 * v / n, ', '.join('%s %.1f%%' % (p, 100.0 * c / n) for p, c in places)))
    out('top self:')
    for fn, v in self_f.most_common(top):
        out('  %6.2f%%  %s' % (100.0 * v / n, fn))
    out('top inclusive (in the call chain):')
    for fn, v in incl_f.most_common(top):
        out('  %6.2f%%  %s' % (100.0 * v / n, fn))
    return dict(n=n, by_place=by_place, self=self_f, incl=incl_f)


def self_test():
    import os
    import tempfile
    d = tempfile.mkdtemp()
    log = os.path.join(d, 'r.dm.log')
    open(log, 'w').write('11-17 01:42:16.528  7918 10934 I dm      : [dm] main: base 0x6f00000000 size 0x335c000\n'
                         '11-17 01:42:16.556  7918 10934 I dm      : [dm] sdk: base 0x6f47a79000 size 0x1320000\n')
    funcs = os.path.join(d, 'main.tsv')
    open(funcs, 'w').write('# offset\tname\n0x15ee000\tRenderTaskGraph_WaitAll\n0x928e80\tDrawBracketBegin\n0x941720\tAllocCommand\n')
    smp = os.path.join(d, 's.txt')
    open(smp, 'w').write('# ========\n# cmdline : /data/local/tmp/simpleperf record\n# ========\n#\n'
                         'org.dos2.port\t7918/10936 [004] 1.000000: 250000 cpu-clock:\n'
                         '\t      6f015ee898 unknown ([anon:dm])\n\t      6f00928e90 unknown ([anon:dm])\n\t      7a00001000 nvn_lookup (/data/app/lib/arm64/libdm.so)\n\n'
                         'org.dos2.port\t7918/10936 [004] 1.000250: 250000 cpu-clock:\n'
                         '\t      6f00941730 unknown ([anon:dm])\n\t      6f00928e90 unknown ([anon:dm])\n\n'
                         'nvn-translate\t7918/11197 [007] 1.000300: 250000 cpu-clock:\n'
                         '\t      7a00002000 draw (/data/app/lib/arm64/libdm.so)\n\n'
                         'org.dos2.port\t7918/10936 [004] 1.000500: 250000 cpu-clock:\n'
                         '\t      6f47a79100 unknown ([anon:dm])\n\t      ffffffc000100000 el0_svc ([kernel.kallsyms])\n\n'
                         'org.dos2.port\t7918/10936 [004] 1.000750: 250000 cpu-clock:\n'
                         '\t      ffffffc000100000 el0_svc ([kernel.kallsyms])\n')
    m = Mapper(read_bases(log), read_funcs(['main=' + funcs]), 0x100)
    got = []
    r = report(m, list(parse_samples(smp)), out=got.append)
    assert r['n'] == 5, r
    assert r['by_place']['main'] == 2 and r['by_place']['libdm.so'] == 1 and r['by_place']['sdk'] == 1 and r['by_place']['[kernel]'] == 1, r['by_place']
    assert r['self']['main:RenderTaskGraph_WaitAll'] == 1 and r['self']['main:AllocCommand'] == 1 and r['self']['sdk+0x100'] == 1 and r['self']['libdm.so:draw'] == 1, r['self']
    assert r['incl']['main:DrawBracketBegin'] == 2 and r['incl']['libdm.so:nvn_lookup'] == 1, r['incl']
    got = []
    r = report(m, list(parse_samples(smp)), thread=11197, out=got.append)
    assert r['n'] == 1 and r['self']['libdm.so:draw'] == 1
    # without function names: buckets of the module offset
    m2 = Mapper(read_bases(log), {}, 0x1000)
    r = report(m2, list(parse_samples(smp)), out=[].append)
    assert r['self']['main+0x15ee000'] == 1 and r['self']['main+0x941000'] == 1, r['self']
    print('PASS: dos2_perf_map self-test (bases from the log, guest addresses to module offsets / function names, host symbols, kernel, threads, inclusive)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--log'); ap.add_argument('--samples')
    ap.add_argument('--funcs', action='append', help='module=path of a "<offset hex>\\t<name>" list (offsets from the module base)')
    ap.add_argument('--thread', type=int); ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--bucket', type=lambda x: int(x, 0), default=0x100, help='offset bucket for guest addresses without a function list')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return 0
    if not (a.log and a.samples):
        ap.print_help()
        return 2
    bases = read_bases(a.log)
    if not bases:
        print('no "[dm] <module>: base ... size ..." line in %s' % a.log)
        return 1
    print('modules: ' + ', '.join('%s 0x%x +0x%x' % (n, b, s) for n, (b, s) in sorted(bases.items(), key=lambda kv: kv[1][0])))
    report(Mapper(bases, read_funcs(a.funcs), a.bucket), parse_samples(a.samples), a.thread, a.top)
    return 0


if __name__ == '__main__':
    sys.exit(main())
