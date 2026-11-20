#!/usr/bin/env python3
"""[NR-16] Read the DM_NR_CPU / DM_NVN_PROF=2 / DM_SLEEPSTAT lines of a DOS2 phone log and answer the questions of the steps that wait for
them (docs/dos2/nr/NR-16.md, audit docs/dos2/nr/AUDIT-REMAINING.md):

  NR-23  HLE NVN time on the game's recording thread a frame (go if >= 1.5 ms)
  NR-24  gates a frame, their cost, the global lock G (contention), the main thread's poll + yield (go if gates >= 10k a frame or G
         contended >= 5 % with >= 50 us of waiting a frame, or the main thread spends >= 10 % of its time yielding)
  NR-21  pipeline binds that sorting the order-free (depth-only) passes would save, at 1.4-1.8 us of GPU each on Turnip (go if >= 0.3 ms)
  NR-17  where each heavy thread ran (cluster shares), the guest names to give the job role (DM_CPU_JOB_NAMES)
  A/B    two or more logs (e.g. DM_NB_MX=1 vs 0, DM_GFX_PRESENT_THREAD=1 vs 0): fps, GPU busy / gaps, the translation thread's CPU and
         waits, the worker's CPU, the whole process's CPU a frame - per log, 90 Hz phase windows (88.5-91 fps) left out.

Usage:  python tools/dos2_cpuprof.py run.dm.log                  one run: the answers
        python tools/dos2_cpuprof.py --ab A.dm.log B.dm.log [...] A/B table (first log = reference)
        python tools/dos2_cpuprof.py --self-test
Medians over the 10 s windows with fps >= --min-fps (default 30: menus / loading screens out).
"""
import argparse
import re
import statistics
import sys

NUM = r'(-?[\d.]+)'


def med(v):
    v = [x for x in v if x is not None]
    return statistics.median(v) if v else None


def fmt(x, nd=2):
    return '-' if x is None else ('%.' + str(nd) + 'f') % x


def in_90hz(fps):
    return fps is not None and 88.5 < fps < 91.0


class Run:
    def __init__(self, name):
        self.name = name
        self.threads = []   # (fps, process_ms, rq_ms, letters, [entries])
        self.cores = []     # list of {letter: (busy_pct, cores, ghz, cap)}
        self.hle = []       # dict
        self.svc = []       # (fps-less) dict
        self.nvn = []       # dict
        self.nvn_top = []   # list of (name, calls, ns, ms)
        self.gfx = []       # dict
        self.passes = []    # dict
        self.sig = []       # raw text
        self.perf = []      # (fps, gpu, draws)
        self.sleep = []     # (handle, yield/s, host %)
        self.mx = []        # (ran, cpu, waited)
        self.roles = None
        self.probe = None
        self.nvn_cal = None
        self.nocall = []    # NR-23 DM_NR_NOCALL=2: dict(calls, share, total, switches, same)
        self.nocall_cost = []   # dict(thunk, direct, switch, saved, wrap_all)
        self.nocall_bad = None  # (flushes compared, bad, draws compared, bad)
        self.nr24 = []      # [NR-24] dict per 10 s: the short clock paths, the arbiter buckets, the yield waits
        self.nr24_probe = None   # (mode, full gate ns, short entry ns)
        self.lc_cal = []    # NR-28 DM_NR_LEANCALL calibration: dict(cpu, ghz, old, lean, saved, cycles)
        self.lc = []        # NR-28 report lines: dict(mode, calls, saved_ms, cpu, saved_ns, slow_ms, bad)

    def ok(self, fps, min_fps):
        return fps is not None and fps >= min_fps and not in_90hz(fps)


ENTRY = re.compile(r'^(?P<label>.+?) (?P<tid>\d+) (?P<ms>[\d.]+) \((?P<wait>[\d.]+)\) \[(?P<cl>[\d ]*)\]$')


def parse(path, name=None):
    r = Run(name or path)
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if '[cpuprof]' not in line and 'NVN PERF v=1' not in line and '[sleepstat]' not in line and '[nb] mx (DM_NB_MX' not in line and '[nr] nocall' not in line and '[nvn] leancall' not in line:
                continue
            m = re.search(r'\[cpuprof\] threads \(' + NUM + r' s, ' + NUM + r' frames = ' + NUM + r' fps; [^)]*\): process ' + NUM + r' ms CPU a frame \(' + NUM +
                          r' cores\), run-queue wait ' + NUM + r' ms a frame; per thread ms a frame \(wait\) \[%(\w*)\]:(.*)$', line)
            if m:
                ents = []
                for part in m.group(8).split(';'):
                    e = ENTRY.match(part.strip())
                    if e:
                        ents.append(dict(label=e['label'], tid=int(e['tid']), ms=float(e['ms']), wait=float(e['wait']), cl=[float(x) for x in e['cl'].split()]))
                r.threads.append(dict(fps=float(m.group(3)), proc=float(m.group(4)), cores=float(m.group(5)), rq=float(m.group(6)), letters=m.group(7), ents=ents))
                continue
            m = re.search(r'\[cpuprof\] cores \([\d.]+ s\):(.*)$', line)
            if m:
                d = {}
                for e in re.finditer(r' (\w) (\d+) cpus busy (?:' + NUM + r'% \(of \d+ cores: ' + NUM + r'\)|- \(/proc/stat not readable\)), ' + NUM + r' GHz avg, cap ' + NUM + r' GHz;', m.group(1)):
                    d[e.group(1)] = dict(n=int(e.group(2)), busy=float(e.group(3)) if e.group(3) else None, cores=float(e.group(4)) if e.group(4) else None, ghz=float(e.group(5)), cap=float(e.group(6)))
                r.cores.append(d)
                continue
            m = re.search(r'\[cpuprof\] hle \([\d.]+ s\): gates ' + NUM + r' a frame \(svc ' + NUM + r', clock ' + NUM + r', obs ' + NUM + r'\) = ' + NUM +
                          r' k/s; their frames at the probe\'s ' + NUM + r' ns \+ the clock\'s ' + NUM + r' ns = ' + NUM + r' ms a frame; by thread a frame:(.*)$', line)
            if m:
                byt = [(e.group(1), int(e.group(2)), float(e.group(3))) for e in re.finditer(r' (.+?) (\d+) ' + NUM + ';', m.group(9))]
                r.hle.append(dict(gates=float(m.group(1)), svc=float(m.group(2)), clock=float(m.group(3)), obs=float(m.group(4)), kps=float(m.group(5)),
                                  probe=float(m.group(6)), clock_ns=float(m.group(7)), ms=float(m.group(8)), byt=byt))
                continue
            m = re.search(r'\[cpuprof\] hle svc \([^)]*\):(.*)\| G: ' + NUM + r' takes a frame, ' + NUM + r'% found it held, waited ' + NUM +
                          r' us a frame \(longest ' + NUM + r' us\), held ' + NUM + r' us a frame', line)
            if m:
                sv = [(int(e.group(1), 16), e.group(2), float(e.group(3)), float(e.group(4)), float(e.group(5))) for e in
                      re.finditer(r' 0x([0-9a-f]+) ([\w-]+\*?) ' + NUM + ' x ' + NUM + ' = ' + NUM + ';', m.group(1))]
                r.svc.append(dict(svc=sv, takes=float(m.group(2)), held_pct=float(m.group(3)), wait_us=float(m.group(4)), longest_us=float(m.group(5)), hold_us=float(m.group(6))))
                continue
            m = re.search(r'\[cpuprof\] nvn \(DM_NVN_PROF=2, thread (\d+), [^)]*?' + NUM + r' submissions a frame\): NVN ' + NUM + r' ms an interval in ' + NUM +
                          r' calls \(' + NUM + r' ns a call, the wrapper\'s ' + NUM + r' ns taken out\) \+ nvnQueueSubmitCommands ' + NUM + r' ms; the thread\'s CPU ' + NUM +
                          r' ms, wall ' + NUM + r' ms in the interval: NVN = ' + NUM + '% of its CPU(?:; of the NVN time ' + NUM + ' ms in calls that wait)?', line)
            if m:
                r.nvn.append(dict(tid=int(m.group(1)), per_frame=float(m.group(2)), ms=float(m.group(3)), calls=float(m.group(4)), ns=float(m.group(5)), floor=float(m.group(6)),
                                  sub=float(m.group(7)), cpu=float(m.group(8)), wall=float(m.group(9)), pct=float(m.group(10)), waits=float(m.group(11)) if m.group(11) else 0.0))
                continue
            m = re.search(r'\[cpuprof\] nvn top \([^)]*\):(.*)$', line)
            if m:
                r.nvn_top.append([(e.group(1), float(e.group(2)), float(e.group(3)), float(e.group(4))) for e in re.finditer(r' (\w+) ' + NUM + ' x ' + NUM + ' ns = ' + NUM + ' ms;', m.group(1))])
                continue
            m = re.search(r'\[cpuprof\] DM_NVN_PROF=2: .*?untimed call ' + NUM + r' ns, timed call ' + NUM + r' ns, ' + NUM + ' ns of it inside', line)
            if m:
                r.nvn_cal = dict(untimed=float(m.group(1)), timed=float(m.group(2)), floor=float(m.group(3)))
                continue
            m = re.search(r'\[nr\] nocall, what =1 would not call through the thunks: .*?: ' + NUM + r' NVN calls a frame \(' + NUM + r'% of the ' + NUM + r' above\) for ' + NUM +
                          r' stack switches;.*?already held that handle this frame ' + NUM + ' a frame', line)
            if m:
                r.nocall.append(dict(calls=float(m.group(1)), share=float(m.group(2)), total=float(m.group(3)), switches=float(m.group(4)), same=float(m.group(5))))
                continue
            m = re.search(r'\[nr\] nocall cost model \([^)]*\): an NVN call through thunk \+ big stack ' + NUM + r' ns, its body called directly ' + NUM + r' ns, one stack switch ' + NUM +
                          r' ns: =1 would save about ' + NUM + r' ms a frame .*?cost ' + NUM + ' ms in the', line)
            if m:
                r.nocall_cost.append(dict(thunk=float(m.group(1)), direct=float(m.group(2)), switch=float(m.group(3)), saved=float(m.group(4)), wrap_all=float(m.group(5))))
                continue
            if '[nvn] leancall calibration' in line:   # NR-28
                for m in re.finditer(r' cpu(\d+) ' + NUM + r' GHz: old ' + NUM + r' ns .*?, lean ' + NUM + r' ns .*?; saved ' + NUM + r' ns = ' + NUM + r' cycles;', line):
                    r.lc_cal.append(dict(cpu=int(m.group(1)), ghz=float(m.group(2)), old=float(m.group(3)), lean=float(m.group(4)), saved=float(m.group(5)), cycles=float(m.group(6))))
                continue
            m = re.search(r'\[nvn\] leancall \(DM_NR_LEANCALL=(\d), [^)]*\): ' + NUM + r' (?:NVN calls a frame through the lean entry|calls a frame compared)', line)
            if m:
                d = dict(mode=int(m.group(1)), calls=float(m.group(2)), saved_ms=None, cpu=None, saved_ns=None, slow_ms=None, bad=None)
                e = re.search(r'saved (?:by =1 )?' + NUM + r' ms a frame on cpu(\d+) \(' + NUM + r' ns a call\), ' + NUM + r' ms on the slowest calibrated core', line)
                if e: d.update(saved_ms=float(e.group(1)), cpu=int(e.group(2)), saved_ns=float(e.group(3)), slow_ms=float(e.group(4)))
                e = re.search(r'bad (\d+) \(total (\d+)\)', line)
                if e: d['bad'] = int(e.group(2))
                r.lc.append(d)
                continue
            m = re.search(r'\[nr\] nocall check \([^)]*\): (\d+) texture flushes compared, bad (\d+) .*?; (\d+) draws 36 / 37 compared, bad (\d+)', line)
            if m:
                r.nocall_bad = tuple(int(m.group(k)) for k in range(1, 5))
                continue
            m = re.search(r'\[cpuprof\] nr24 probe \(DM_NR_GATE_TICK=(\d+)\): a clock gate through the full gate ' + NUM + r' ns, through the short entry ' + NUM + ' ns', line)
            if m:
                r.nr24_probe = (int(m.group(1)), float(m.group(2)), float(m.group(3)))
                continue
            m = re.search(r'\[cpuprof\] nr24 \([\d.]+ s; a frame\): DM_NR_GATE_TICK=(\d+) clock reads by the short entry ' + NUM + r", main's GetSystemTick on the host " + NUM +
                          r', full clock gates ' + NUM + r'; DM_NR_GATE_ARB=(\d+) bucket takes ' + NUM + r' \(' + NUM + r'% found held\), wakes ' + NUM + r' \(futex ' + NUM +
                          r'\), moved onto a lock ' + NUM + r', red (\d+); DM_NR_GATE_YIELD=(\d+) waits ' + NUM + r' \(' + NUM + r' woken by progress, ' + NUM + r' timed out, ' + NUM +
                          r' us each\), would wait ' + NUM, line)
            if m:
                g = m.groups()
                r.nr24.append(dict(tick=int(g[0]), fast=float(g[1]), fn=float(g[2]), full=float(g[3]), arb=int(g[4]), takes=float(g[5]), held=float(g[6]), wakes=float(g[7]),
                                   fwakes=float(g[8]), moves=float(g[9]), red=int(g[10]), yld=int(g[11]), yw=float(g[12]), yw_woke=float(g[13]), yw_tmo=float(g[14]), yw_us=float(g[15]),
                                   yw_would=float(g[16])))
                continue
            m = re.search(r'\[cpuprof\] gate probe: one gate round trip .*? ' + NUM + r' ns; the clock gate\'s own guest_ns\(\) ' + NUM + ' ns', line)
            if m:
                r.probe = (float(m.group(1)), float(m.group(2)))
                continue
            m = re.search(r'\[cpuprof\] gfx \([\d.]+ s, ' + NUM + r' frames = ' + NUM + r' fps; DM_NB_MX=(\d+) DM_GFX_PRESENT_THREAD=(\d+) DM_NB_MT=(\d+)\): translation thread a frame: submit wall ' +
                          NUM + ' ms CPU ' + NUM + r' ms \(waited ' + NUM + r'\), present wall ' + NUM + ' ms CPU ' + NUM + r' ms \(waited ' + NUM + r'\), ' + NUM +
                          r' submissions a frame; MX worker CPU ' + NUM + r' ms a frame; GPU a frame: engine submissions ' + NUM + r' ms, between them ' + NUM + ' ms', line)
            if m:
                g = [float(x) for x in m.groups()]
                r.gfx.append(dict(fps=g[1], mx=int(g[2]), pt=int(g[3]), mt=int(g[4]), sub_wall=g[5], sub_cpu=g[6], sub_wait=g[7], pres_wall=g[8], pres_cpu=g[9], pres_wait=g[10],
                                  subs=g[11], mx_cpu=g[12], gpu=g[13], gap=g[14]))
                continue
            m = re.search(r'\[cpuprof\] passes \([\d.]+ s, a frame\): ' + NUM + ' passes with draws, ' + NUM + ' draws, ' + NUM + r' pipeline binds \(' + NUM +
                          r' if every pass were sorted by pipeline\), ' + NUM + ' set binds, ' + NUM + r' draws counting samples; order-free \([^)]*\): ' + NUM + ' passes, ' +
                          NUM + ' draws, ' + NUM + ' binds -> ' + NUM + ' sorted = ' + NUM + ' binds saved a frame', line)
            if m:
                g = [float(x) for x in m.groups()]
                r.passes.append(dict(passes=g[0], draws=g[1], binds=g[2], sorted=g[3], sets=g[4], counting=g[5], fpasses=g[6], fdraws=g[7], fbinds=g[8], fsorted=g[9], saved=g[10]))
                continue
            m = re.search(r'\[cpuprof\] passes by signature [^:]*:(.*)$', line)
            if m:
                r.sig.append(m.group(1).strip())
                continue
            m = re.search(r'\[cpuprof\] roles (.*)$', line)
            if m:
                r.roles = m.group(1) if not m.group(1).startswith('(cont.)') else (r.roles or '') + m.group(1)[8:]
                continue
            m = re.search(r'NVN PERF v=1 .*? fps=' + NUM + r' .*?gpu_ms=' + NUM + r' .*?draws=' + NUM, line)
            if m:
                r.perf.append((float(m.group(1)), float(m.group(2)), float(m.group(3))))
                continue
            m = re.search(r'\[sleepstat\] tid \d+ handle (0x[0-9a-f]+): per s yield ' + NUM + ' <20us ' + NUM + ' <200us ' + NUM + ' <2ms ' + NUM + ' longer ' + NUM +
                          r'; host time in them ' + NUM + '%', line)
            if m:
                r.sleep.append((m.group(1), float(m.group(2)), float(m.group(7))))
                continue
            m = re.search(r'worker ran ' + NUM + r' ms(?: \(CPU ' + NUM + r'\))?, the translation thread waited ' + NUM, line)
            if m:
                r.mx.append((float(m.group(1)), float(m.group(2)) if m.group(2) else None, float(m.group(3))))
    return r


def windows(r, min_fps):
    """The 10 s windows to judge: by the threads line's fps (the gfx line's for the translator lines)"""
    th = [t for t in r.threads if r.ok(t['fps'], min_fps)]
    gf = [g for g in r.gfx if r.ok(g['fps'], min_fps)]
    return th, gf


def role_ms(th, key):
    """median ms a frame of the thread whose label carries the role (rec, main, pres) or the comm (nvn-translate, dm-present...)"""
    v = []
    for t in th:
        x = [e['ms'] for e in t['ents'] if key in e['label'].split(',') or e['label'].startswith(key + ',') or e['label'] == key or e['label'].endswith(key)
             or (key in ('rec', 'main', 'pres') and key + ',' in e['label'])]
        v.append(sum(x) if x else 0.0)
    return med(v)


def answers(r, min_fps=30.0, out=print):
    th, gf = windows(r, min_fps)
    out('== %s: %d threads windows, %d translator windows judged (fps >= %.0f, 90 Hz phase out); %d NVN PERF windows' % (r.name, len(th), len(gf), min_fps, len(r.perf)))
    fps = med([t['fps'] for t in th]) or med([g['fps'] for g in gf])
    out('fps %s, process CPU %s ms a frame (%s cores), run-queue wait %s ms a frame' % (fmt(fps, 1), fmt(med([t['proc'] for t in th])), fmt(med([t['cores'] for t in th])), fmt(med([t['rq'] for t in th]))))
    # where the CPU goes, by role
    rec, main, tr = role_ms(th, 'rec'), role_ms(th, 'main'), role_ms(th, 'nvn-translate')
    out('threads a frame: rec (NVN submitter) %s ms, main %s ms, nvn-translate %s ms, dm-nbmt %s, dm-nbx0 %s, dm-present %s' %
        (fmt(rec), fmt(main), fmt(tr), fmt(role_ms(th, 'dm-nbmt')), fmt(role_ms(th, 'dm-nbx0')), fmt(role_ms(th, 'dm-present'))))
    # NR-23
    nv = [n for n in r.nvn]
    if nv:
        nvn_ms = med([n['ms'] - n['waits'] + n['sub'] for n in nv]); per = med([n['per_frame'] for n in nv]) or 1.0
        nvf = nvn_ms * per if nvn_ms is not None else None
        out('NR-23: HLE NVN on the recording thread %s ms a frame without the waiting calls (%s ms waited in them; %s calls an interval, %s ns a call; %s %% of that thread\'s CPU %s ms an interval) -> %s' %
            (fmt(nvf, 3), fmt(med([n['waits'] for n in nv]), 3), fmt(med([n['calls'] for n in nv]), 0), fmt(med([n['ns'] for n in nv]), 0), fmt(med([n['pct'] for n in nv]), 0), fmt(med([n['cpu'] for n in nv])),
             'GO (>= 1.5 ms)' if nvf is not None and nvf >= 1.5 else 'NO-GO (< 1.5 ms): the cost is not in the NVN calls'))
        if r.nvn_top:
            agg = {}
            for top in r.nvn_top:
                for name, c, ns, ms in top:
                    agg.setdefault(name, []).append((c, ns, ms))
            best = sorted(agg.items(), key=lambda kv: -med([x[2] for x in kv[1]]))[:8]
            out('       top NVN functions (an interval): ' + '; '.join('%s %s x %s ns = %s ms' % (k, fmt(med([x[0] for x in v]), 0), fmt(med([x[1] for x in v]), 0), fmt(med([x[2] for x in v]), 3)) for k, v in best))
        if r.nvn_cal:
            out('       wrapper calibration: untimed %.1f ns, timed %.1f ns a call (floor %.1f ns taken out)' % (r.nvn_cal['untimed'], r.nvn_cal['timed'], r.nvn_cal['floor']))
    else:
        out('NR-23: no "[cpuprof] nvn" line (run with DM_NVN_PROF=2)')
    if r.nocall:
        nc = r.nocall; cost = r.nocall_cost
        saved = med([c['saved'] for c in cost]) if cost else None
        out('NR-23 (DM_NR_NOCALL=2): the cut (texture flush + draw call) covers %s of %s NVN calls a frame (%s %%) for %s stack switches; binds that repeat the slot\'s handle %s a frame' %
            (fmt(med([n['calls'] for n in nc]), 0), fmt(med([n['total'] for n in nc]), 0), fmt(med([n['share'] for n in nc]), 1), fmt(med([n['switches'] for n in nc]), 0), fmt(med([n['same'] for n in nc]), 1)))
        if cost:
            out('       thunk + big stack %s ns a call (body direct %s ns), one stack switch %s ns: =1 saves about %s ms a frame; the wrapper of ALL calls costs %s ms a frame -> %s' %
                (fmt(med([c['thunk'] for c in cost]), 1), fmt(med([c['direct'] for c in cost]), 1), fmt(med([c['switch'] for c in cost]), 1), fmt(saved, 3), fmt(med([c['wrap_all'] for c in cost]), 3),
                 'GO for =1 (>= 0.15 ms)' if saved is not None and saved >= 0.15 else 'NO-GO for =1 (< 0.15 ms a frame): the calls that can be cut cleanly are too few / too cheap'))
        if r.nocall_bad:
            out('       model check: %d flushes compared, %d bad; %d draws compared, %d bad%s' % (r.nocall_bad + ('' if not (r.nocall_bad[1] or r.nocall_bad[3]) else '  <-- do NOT run =1',)))
    # NR-28
    if r.lc_cal or r.lc:
        if r.lc_cal:
            out('NR-28 (DM_NR_LEANCALL) calibration, ns a call old -> lean (saved, cycles): ' +
                '; '.join('cpu%d %.2f GHz %.1f -> %.1f (%.1f, %.0f)' % (c['cpu'], c['ghz'], c['old'], c['lean'], c['saved'], c['cycles']) for c in r.lc_cal))
        done = [x for x in r.lc if x['saved_ms'] is not None]
        if done:
            mode = done[-1]['mode']; bad = max([x['bad'] or 0 for x in done]); sv = med([x['saved_ms'] for x in done])
            verdict = ('do NOT run =1 (the lean entry disagrees with the wrapper)' if bad else
                       'GO for =1 (no semantic change; >= 0.05 ms a frame)' if sv >= 0.05 else 'marginal (< 0.05 ms a frame)')
            out("NR-28 (DM_NR_LEANCALL=%d): %s calls a frame, saved %s ms a frame on the recording thread's cpu (%s ns a call), %s ms on the slowest core%s -> %s" %
                (mode, fmt(med([x['calls'] for x in done]), 0), fmt(sv, 3), fmt(med([x['saved_ns'] for x in done]), 1), fmt(med([x['slow_ms'] for x in done]), 3),
                 '; check: %d bad' % bad if mode == 2 else '', verdict))
        elif r.lc:
            out('NR-28 (DM_NR_LEANCALL=%d): %s calls a frame, no calibration yet (it runs after 600 frames)' % (r.lc[-1]['mode'], fmt(med([x['calls'] for x in r.lc]), 0)))
    # NR-24
    if r.hle:
        hl = r.hle[len(r.hle) // 4:] or r.hle   # skip the first quarter (menu)
        gates, cost = med([h['gates'] for h in hl]), med([h['ms'] for h in hl])
        clock = med([h['clock'] for h in hl]); svc = med([h['svc'] for h in hl])
        sv = r.svc[len(r.svc) // 4:] or r.svc
        held, wait, hold = med([s['held_pct'] for s in sv]), med([s['wait_us'] for s in sv]), med([s['hold_us'] for s in sv])
        main_sleep = [s for s in r.sleep if s[0] == '0x1001']
        y = med([s[1] for s in main_sleep]); yh = med([s[2] for s in main_sleep])
        go_a = gates is not None and gates >= 10000
        go_b = held is not None and wait is not None and held >= 5 and wait >= 50
        go_c = yh is not None and yh >= 10
        out('NR-24: gates %s a frame (svc %s, clock %s) costing %s ms a frame in gate frames (probe %s ns); G: %s takes a frame, %s %% found it held, waited %s us / held %s us a frame; '
            'main thread yields %s / s, %s %% of its time in sleeps / yields' % (fmt(gates, 0), fmt(svc, 0), fmt(clock, 0), fmt(cost, 3), fmt(r.probe[0] if r.probe else None, 1),
                                                                              fmt(med([s['takes'] for s in sv]), 0), fmt(held, 1), fmt(wait, 1), fmt(hold, 1), fmt(y, 0), fmt(yh, 1)))
        out('       (a) clock stub %s, (b) per-address arbiter %s, (c) bounded wait for the main poll %s' %
            ('GO' if go_a else 'no-go', 'GO' if go_b else 'no-go', 'GO' if go_c else ('no-go' if yh is not None else '? (run with DM_SLEEPSTAT=1)')))
        if sv and sv[-1]['svc']:
            agg = {}
            for s in sv:
                for n, name, c, us, ms in s['svc']:
                    agg.setdefault((n, name), []).append((c, us, ms))
            best = sorted(agg.items(), key=lambda kv: -med([x[0] for x in kv[1]]))[:8]
            out('       SVCs a frame: ' + '; '.join('0x%02x %s %s x %s us' % (k[0], k[1], fmt(med([x[0] for x in v]), 1), fmt(med([x[1] for x in v]), 1)) for k, v in best))
    else:
        out('NR-24: no "[cpuprof] hle" line (run with DM_NR_CPU=1)')
    if r.nr24:
        n4 = r.nr24[len(r.nr24) // 4:] or r.nr24
        z = n4[-1]
        out('NR-24 switches: DM_NR_GATE_TICK=%d clock reads a frame: short entry %s, main\'s GetSystemTick on the host %s, full clock gates %s%s; DM_NR_GATE_ARB=%d bucket takes %s a frame '
            '(%s %% found held), wakes %s (futex %s), moved onto a lock %s, red %d; DM_NR_GATE_YIELD=%d waits %s a frame (%s woken by progress, %s timed out, %s us each), would wait %s'
            % (z['tick'], fmt(med([x['fast'] for x in n4]), 0), fmt(med([x['fn'] for x in n4]), 0), fmt(med([x['full'] for x in n4]), 0),
               ('; probe: full gate %.1f ns, short entry %.1f ns' % r.nr24_probe[1:]) if r.nr24_probe else '', z['arb'], fmt(med([x['takes'] for x in n4]), 0), fmt(med([x['held'] for x in n4]), 1),
               fmt(med([x['wakes'] for x in n4]), 0), fmt(med([x['fwakes'] for x in n4]), 0), fmt(med([x['moves'] for x in n4]), 0), z['red'], z['yld'], fmt(med([x['yw'] for x in n4]), 0),
               fmt(med([x['yw_woke'] for x in n4]), 0), fmt(med([x['yw_tmo'] for x in n4]), 0), fmt(med([x['yw_us'] for x in n4]), 1), fmt(med([x['yw_would'] for x in n4]), 0)))
        if z['red']:
            out('       DM_NR_GATE_ARB=2 reported %d invariant violations: read the [nr24] red lines before any default change' % z['red'])
    # NR-21
    pa = r.passes[len(r.passes) // 4:] or r.passes
    if pa:
        saved = med([p['saved'] for p in pa])
        lo, hi = (saved * 1.4 / 1000, saved * 1.8 / 1000) if saved is not None else (None, None)
        out('NR-21: %s passes with %s draws a frame, %s pipeline binds (%s if sorted); order-free passes %s with %s draws: %s binds -> %s sorted = %s saved a frame = %s-%s ms GPU '
            'at 1.4-1.8 us a switch -> %s' % (fmt(med([p['passes'] for p in pa]), 1), fmt(med([p['draws'] for p in pa]), 0), fmt(med([p['binds'] for p in pa]), 0),
                                             fmt(med([p['sorted'] for p in pa]), 0), fmt(med([p['fpasses'] for p in pa]), 1), fmt(med([p['fdraws'] for p in pa]), 0),
                                             fmt(med([p['fbinds'] for p in pa]), 0), fmt(med([p['fsorted'] for p in pa]), 0), fmt(saved, 0), fmt(lo), fmt(hi),
                                             'GO (>= 0.3 ms)' if lo is not None and lo >= 0.3 else 'NO-GO / marginal (< 0.3 ms at the low end)'))
        if r.sig:
            out('       by signature (last window): ' + r.sig[-1])
    else:
        out('NR-21: no "[cpuprof] passes" line')
    # NR-17
    if th:
        letters = th[-1]['letters']
        agg = {}
        for t in th:
            for e in t['ents']:
                agg.setdefault((e['label'], e['tid']), []).append(e)
        heavy = sorted(agg.items(), key=lambda kv: -med([e['ms'] for e in kv[1]]))[:12]
        out('NR-17: heavy threads (ms a frame, run-queue wait, share by cluster %s):' % letters)
        jobs, crit = set(), set()
        for (label, tid), es in heavy:
            ms = med([e['ms'] for e in es])
            cl = [med([e['cl'][k] for e in es if len(e['cl']) > k]) for k in range(len(letters))]
            out('       %-44s %6d %6s ms (%s) %s' % (label, tid, fmt(ms), fmt(med([e['wait'] for e in es])), ' '.join('%s%s' % (letters[k], fmt(cl[k], 0)) for k in range(len(letters)))))
            m = re.match(r'(?:[\w,]*,)?g:(.+?)\(p\d+\)$', label)
            roles = set(label.split(',')[:-1])
            if m and roles & {'rec', 'pres'} and 'main' not in roles:
                crit.add(m.group(1))
            elif m and not roles & {'main', 'rec', 'pres'} and ms is not None and ms >= 0.5:
                jobs.add(re.sub(r'[\s_-]*\d+$', '', m.group(1)))
        if crit:
            out('       the game\'s render / present thread besides main (a critical role like main, not a job): ' + ','.join(sorted(crit)))
        if jobs:
            out('       guest names of heavy non-main game threads (candidates for DM_CPU_JOB_NAMES): ' + ','.join(sorted(jobs)))
        if r.roles:
            out('       guest threads (tid handle name priority core mask entry): ' + r.roles[:2400])
        if r.cores:
            cs = r.cores[len(r.cores) // 4:] or r.cores
            out('       clusters: ' + '; '.join('%s busy %s %% at %s GHz (cap %s)' % (L, fmt(med([c[L]['busy'] for c in cs if L in c]), 0), fmt(med([c[L]['ghz'] for c in cs if L in c])),
                                                                                    fmt(med([c[L]['cap'] for c in cs if L in c]))) for L in letters))
    return dict(fps=fps)


def ab(runs, min_fps=30.0, out=print):
    """A/B table: the first run is the reference; medians of the judged windows"""
    rows = [('fps (NVN PERF, unlimited phase)', lambda r: med([p[0] for p in r.perf if p[2] > 500 and r.ok(p[0], min_fps)]), 1),
            ('GPU busy ms a frame (NVN PERF gpu_ms)', lambda r: med([p[1] for p in r.perf if p[2] > 500 and r.ok(p[0], min_fps)]), 2),
            ('GPU engine submissions ms (cpuprof gfx)', lambda r: med([g['gpu'] for g in windows(r, min_fps)[1]]), 2),
            ('GPU gaps ms a frame (blits + idle)', lambda r: med([g['gap'] for g in windows(r, min_fps)[1]]), 2),
            ('translation submit wall ms', lambda r: med([g['sub_wall'] for g in windows(r, min_fps)[1]]), 2),
            ('translation submit CPU ms', lambda r: med([g['sub_cpu'] for g in windows(r, min_fps)[1]]), 2),
            ('translation present wall ms', lambda r: med([g['pres_wall'] for g in windows(r, min_fps)[1]]), 2),
            ('translation present CPU ms', lambda r: med([g['pres_cpu'] for g in windows(r, min_fps)[1]]), 2),
            ('MX worker CPU ms a frame', lambda r: med([g['mx_cpu'] for g in windows(r, min_fps)[1]]), 2),
            ('[nb] mx handoff ms (waited - ran)', lambda r: med([m[2] - m[0] for m in r.mx if m[0] > 3]), 2),
            ('nvn-translate CPU ms (schedstat)', lambda r: role_ms(windows(r, min_fps)[0], 'nvn-translate'), 2),
            ('dm-nbx0 CPU ms', lambda r: role_ms(windows(r, min_fps)[0], 'dm-nbx0'), 2),
            ('dm-present CPU ms', lambda r: role_ms(windows(r, min_fps)[0], 'dm-present'), 2),
            ('dm-nbmt CPU ms', lambda r: role_ms(windows(r, min_fps)[0], 'dm-nbmt'), 2),
            ('rec thread CPU ms', lambda r: role_ms(windows(r, min_fps)[0], 'rec'), 2),
            ('process CPU ms a frame', lambda r: med([t['proc'] for t in windows(r, min_fps)[0]]), 2),
            ('run-queue wait ms a frame', lambda r: med([t['rq'] for t in windows(r, min_fps)[0]]), 2),
            ('[NR-24] main thread CPU ms', lambda r: role_ms(windows(r, min_fps)[0], 'main'), 2),
            ('[NR-24] full clock gates a frame', lambda r: med([h['clock'] for h in r.hle]), 0),
            ('[NR-24] G takes a frame', lambda r: med([x['takes'] for x in r.svc]), 0),
            ('[NR-24] G waited us a frame', lambda r: med([x['wait_us'] for x in r.svc]), 1),
            ('[NR-24] yield waits a frame', lambda r: med([x['yw'] for x in r.nr24]), 0)]
    names = [r.name for r in runs]
    out('%-42s' % 'metric (median of judged windows)' + ''.join('%14s' % n[-14:] for n in names) + ''.join('%12s' % ('d ' + n[-9:]) for n in names[1:]))
    res = {}
    for label, fn, nd in rows:
        vals = [fn(r) for r in runs]
        res[label] = vals
        d = ['-' if (v is None or vals[0] is None) else ('%+.' + str(nd) + 'f') % (v - vals[0]) for v in vals[1:]]
        out('%-42s' % label + ''.join('%14s' % fmt(v, nd) for v in vals) + ''.join('%12s' % x for x in d))
    out('verdict rule: on a GPU-bound phone fps = 1000 / (GPU busy + gaps); a switch that leaves both unchanged but adds process CPU costs heat only;'
        ' one that lowers the translation thread\'s wall + CPU is what a CPU-bound phone gains. Windows of the 90 Hz phase are left out.')
    return res


def self_test():
    import os
    import tempfile
    # lines in the exact formats of hle_cpuprof.c / nvn_cpuprof.inc / xbg_cpuprof.inc / xbg_nb_mx.inc
    th = ('[cpuprof] threads (10.0 s, %d frames = %.1f fps; schedstat): process %.2f ms CPU a frame (%.2f cores), run-queue wait %.2f ms a frame; per thread ms a frame (wait) [%%LMBX]:'
          ' rec,g:RenderThread(p16) 10936 %.2f (0.10) [0 5 90 5]; pres,main,g:main(p44) 10934 4.95 (0.20) [0 0 100 0]; nvn-translate 11197 6.10 (0.05) [0 0 0 100];'
          ' g:JobWorker 3(p20) 11000 2.10 (0.30) [60 40 0 0]; g:JobWorker 4(p20) 11001 1.90 (0.30) [70 30 0 0]; dm-nbmt 11203 1.20 (0.01) [0 10 90 0];'
          ' dm-nbx0 11210 %.2f (0.01) [0 0 0 100]; dm-cpuprof 12000 0.05 (0.00) [100 0 0 0]; 40 more threads 1.00')
    lines = []
    for k, fps in enumerate([60.0, 77.2, 77.4, 89.8, 77.0, 76.9]):
        lines.append('11-17 01:42:%02d.000 1 2 I dm : [dm] ' % k + th % (int(fps * 10), fps, 31.4, 2.43, 0.9, 5.30, 4.0))
        lines.append('11-17 01:42:%02d.000 1 2 I dm : [dm] [cpuprof] cores (10.0 s): L 2 cpus busy 23%% (of 2 cores: 0.46), 1.10 GHz avg, cap 2.27 GHz; M 2 cpus busy 30%% (of 2 cores: 0.60), 2.10 GHz avg, cap 2.96 GHz;'
                     ' B 3 cpus busy 55%% (of 3 cores: 1.65), 2.40 GHz avg, cap 3.15 GHz; X 1 cpus busy 80%% (of 1 cores: 0.80), 2.90 GHz avg, cap 3.30 GHz;' % k)
        lines.append('[dm] [cpuprof] hle (10.0 s): gates 4321 a frame (svc 321, clock 3900, obs 100) = 333 k/s; their frames at the probe\'s 85 ns + the clock\'s 30 ns = 0.484 ms a frame; '
                     'by thread a frame: pres,main,g:main(p44) 10934 3000; g:JobWorker 3(p20) 11000 800;')
        lines.append('[dm] [cpuprof] hle svc (10.0 s; a frame: count x us = ms, * = blocks by design): 0x0b SleepThread* 120.0 x 30.1 = 3.612; 0x1a ArbitrateLock* 12.0 x 4.0 = 0.048; '
                     '0x1b ArbitrateUnlock 12.0 x 0.8 = 0.010; | G: 600 takes a frame, 2.5% found it held, waited 12.0 us a frame (longest 300.0 us), held 180.0 us a frame')
        lines.append('[dm] [cpuprof] nvn (DM_NVN_PROF=2, thread 10936, 1 submission interval in 16 timed, 48 timed in 10 s, 1.00 submissions a frame): NVN 2.100 ms an interval in 14650 calls '
                     '(143 ns a call, the wrapper\'s 40 ns taken out) + nvnQueueSubmitCommands 0.050 ms; the thread\'s CPU 5.300 ms, wall 12.900 ms in the interval: NVN = 41% of its CPU; '
                     'of the NVN time 0.100 ms in calls that wait (SyncWait / AcquireTexture / QueueFinish / EventWait / Read-WriteTexels)')
        lines.append('[dm] [cpuprof] nvn top (an interval, calls x ns = ms): CommandBufferDrawElements 1150 x 540 ns = 0.621 ms; CommandBufferBindTexture 4000 x 80 ns = 0.320 ms;')
        lines.append('I dm-vulkan: [cpuprof] gfx (10.0 s, %d frames = %.1f fps; DM_NB_MX=1 DM_GFX_PRESENT_THREAD=1 DM_NB_MT=3): translation thread a frame: submit wall 12.400 ms CPU 4.100 ms '
                     '(waited 8.300), present wall 0.360 ms CPU 0.300 ms (waited 0.060), 1.00 submissions a frame; MX worker CPU 4.000 ms a frame; GPU a frame: engine submissions 12.040 ms, '
                     'between them 0.800 ms (present blits + idle; 0 overlapped, 0 pauses over 200 ms); 1.00 retired submissions a frame' % (int(fps * 10), fps))
        lines.append('I dm-vulkan: [cpuprof] passes (10.0 s, a frame): 46.0 passes with draws, 1155 draws, 600 pipeline binds (380 if every pass were sorted by pipeline), 798 set binds, '
                     '263 draws counting samples; order-free (depth only, one LESS / LEQUAL, no colour, no stencil): 3.0 passes, 520 draws, 300 binds -> 120 sorted = 180 binds saved a frame')
        lines.append('I dm-vulkan: [cpuprof] passes by signature (a frame: passes/draws/binds/distinct/sets, F = order-free passes): 2160x972 c0d 1.0/340/200/40/300 F1.0;')
        lines.append('I dm-vulkan: NVN PERF v=1 sync=0 frames=60 wall_ms=777.0 fps=%.2f submit_cpu_ms=12.4 present_cpu_ms=0.36 gpu_ms=12.01 gpu_samples=60 submits=1.00 draws=1154.8' % fps)
        lines.append('[dm] [sleepstat] tid 10934 handle 0x1001: per s yield 3000 <20us 10 <200us 0 <2ms 0 longer 0; host time in them 12.5%, back-offs 4; last asked 0 ns from main+0x15ee898')
        lines.append('I dm-vulkan: [nb] mx (DM_NB_MX=1, last 10 s): submissions on the worker 772, on the translation thread 0 (a/b off 0, nested 0, no worker 0); per submission: context moved both ways in 0.013 ms (27 variables, 43692 bytes), worker ran 7.20 ms (CPU 4.00), the translation thread waited 7.30 ms')
    lines.append('[dm] [nr] nocall, what =1 would not call through the thunks: 1100.0 texture flushes a frame (983.0 in draws 36 / 37) with 1000.0 binds = 2000 calls '
                 '(GetTextureHandle + BindTexture), 983.0 draw calls, 0.00 SetRenderEnable: 2983 NVN calls a frame (20.4% of the 14650 above) for 990 stack switches; '
                 'left to the engine: flushes with a range outside the slots 0.00, with no command buffer 0.00, from an unknown call site 0.00, draws without an '
                 'index buffer record 0.00 a frame; binds whose slot already held that handle this frame 250.0 a frame (25.0% of the binds; =1 still makes them)')
    lines.append('[dm] [nr] nocall cost model (calibrated on the recording thread): an NVN call through thunk + big stack 20.0 ns, its body called directly 2.0 ns, one stack '
                 'switch 9.0 ns: =1 would save about 0.045 ms a frame (2983 calls x 18.0 ns - 990 switches x 9.0 ns); all 14650 calls a frame cost 0.264 ms in the '
                 'thunk + wrapper alone (the bodies: DM_NVN_PROF=2, [cpuprof] nvn top)')
    lines.append('[dm] [nr] nocall check (DM_NR_NOCALL=2, totals): 70000 texture flushes compared, bad 0 (bind count 0, a bind 0, ranges not reset 0, other NVN calls inside 0); '
                 '60000 draws 36 / 37 compared, bad 0 (the draw call 0, SetRenderEnable 0, the flag bytes 0)')
    lines.append('[dm] [cpuprof] gate probe: one gate round trip (dm_gate frame of 800 bytes, lookup, dispatch, back) 85.0 ns; the clock gate\'s own guest_ns() 30.0 ns')
    lines.append('[dm] [cpuprof] nr24 probe (DM_NR_GATE_TICK=1): a clock gate through the full gate 120.5 ns, through the short entry 9.5 ns')
    for k in range(4):
        lines.append('[dm] [cpuprof] nr24 (10.0 s; a frame): DM_NR_GATE_TICK=1 clock reads by the short entry 2900, main\'s GetSystemTick on the host 1000, full clock gates 3; '
                     'DM_NR_GATE_ARB=1 bucket takes 400 (1.5% found held), wakes 120 (futex 90), moved onto a lock 4, red 0; '
                     'DM_NR_GATE_YIELD=1 waits 30 (20 woken by progress, 10 timed out, 45.0 us each), would wait 0')
    lines.append('[dm] [cpuprof] DM_NVN_PROF=2: 431 NVN slots timed on the submitting thread, 1 submission interval in 16; calibration through CommandBufferPopDebugGroup: untimed call 9.0 ns, timed call 60.0 ns, 40.0 ns of it inside the stamps (taken out per call); cntvct 52.08 ns a tick')
    lines.append('[dm] [nvn] leancall calibration (DM_NR_LEANCALL=2, slot CommandBufferPopDebugGroup, 4096 calls a path, best of 5, a tight loop: warm caches and predictors): '
                 'cpu0 2.00 GHz: old 40.0 ns (thunk stadd 8.0 + wrapper 32.0), lean 12.0 ns (plain thunk 4.0 + entry 8.0), in place old 30.0 / lean 4.0, body 1.0; saved 28.0 ns = 56 cycles; '
                 'cpu7 3.30 GHz: old 12.0 ns (thunk stadd 3.0 + wrapper 9.0), lean 4.0 ns (plain thunk 1.0 + entry 3.0), in place old 9.0 / lean 1.5, body 0.3; saved 8.0 ns = 26 cycles;')
    lines.append('[dm] [nvn] leancall (DM_NR_LEANCALL=2, 10 s, 600 frames): 14600 calls a frame compared (14000 on this submitting thread), the lean entry would take the big stack in '
                 'place 3.0 a frame and pass on 0.0; bad 0 (total 0); counters: wrapper 8760000, thunks (atomic) 8760010, plain counter 8759990 (lost 10 = 0.0001% when threads race); '
                 'saved by =1 0.117 ms a frame on cpu7 (8.0 ns a call), 0.409 ms on the slowest calibrated core')
    d = tempfile.mkdtemp()
    a = os.path.join(d, 'a.dm.log'); open(a, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    b = os.path.join(d, 'b.dm.log')
    open(b, 'w', encoding='utf-8').write('\n'.join(x.replace('DM_NB_MX=1', 'DM_NB_MX=0').replace('MX worker CPU 4.000', 'MX worker CPU 0.000').replace('submit wall 12.400 ms CPU 4.100', 'submit wall 12.300 ms CPU 7.900')
                                                   .replace('fps=77.20', 'fps=79.20').replace('fps=77.40', 'fps=79.40').replace('fps=77.00', 'fps=79.00')
                                                   .replace('dm-nbx0 11210 4.00', 'dm-nbx0 11210 0.00') for x in lines if '[nb] mx' not in x) + '\n')
    r = parse(a, 'a')
    assert len(r.threads) == 6 and len(r.threads[0]['ents']) == 8, (len(r.threads), r.threads[0]['ents'])
    assert r.threads[1]['ents'][0]['label'] == 'rec,g:RenderThread(p16)' and r.threads[1]['ents'][0]['cl'] == [0, 5, 90, 5], r.threads[1]['ents'][0]
    assert r.threads[0]['letters'] == 'LMBX' and r.cores[0]['X']['busy'] == 80
    assert r.hle[0]['gates'] == 4321 and r.hle[0]['byt'][0][1] == 10934 and r.svc[0]['svc'][0][0] == 0x0b and r.svc[0]['held_pct'] == 2.5
    assert r.nvn[0]['ms'] == 2.1 and r.nvn[0]['calls'] == 14650 and r.nvn_top[0][0][0] == 'CommandBufferDrawElements' and r.nvn_cal['floor'] == 40
    assert r.gfx[0]['mx'] == 1 and r.gfx[0]['sub_cpu'] == 4.1 and r.gfx[0]['gap'] == 0.8 and r.passes[0]['saved'] == 180
    assert r.perf[0][0] == 60.0 and r.sleep[0] == ('0x1001', 3000.0, 12.5) and r.mx[0] == (7.2, 4.0, 7.3) and r.probe == (85.0, 30.0)
    assert r.nocall[0] == dict(calls=2983, share=20.4, total=14650, switches=990, same=250) and r.nocall_cost[0]['saved'] == 0.045 and r.nocall_bad == (70000, 0, 60000, 0), (r.nocall, r.nocall_cost, r.nocall_bad)
    assert r.nr24_probe == (1, 120.5, 9.5) and len(r.nr24) == 4 and r.nr24[0]['fast'] == 2900 and r.nr24[0]['held'] == 1.5 and r.nr24[0]['yw_us'] == 45.0, (r.nr24_probe, r.nr24[:1])
    assert len(r.lc_cal) == 2 and r.lc_cal[1] == dict(cpu=7, ghz=3.3, old=12.0, lean=4.0, saved=8.0, cycles=26.0) and r.lc[0]['saved_ms'] == 0.117 and r.lc[0]['bad'] == 0 \
        and r.lc[0]['calls'] == 14600, (r.lc_cal, r.lc)
    th_, gf_ = windows(r, 30)
    assert len(th_) == 5 and len(gf_) == 5, 'the 89.8 fps (90 Hz) window is left out'
    got = []
    answers(r, out=got.append)
    text = '\n'.join(got)
    assert 'NR-23: HLE NVN on the recording thread 2.050 ms a frame without the waiting calls (0.100 ms waited' in text and 'GO (>= 1.5 ms)' in text, text
    assert '(a) clock stub no-go, (b) per-address arbiter no-go, (c) bounded wait for the main poll GO' in text, text
    assert '180 saved a frame = 0.25-0.32 ms GPU' in text and 'NO-GO / marginal' in text, text
    assert 'candidates for DM_CPU_JOB_NAMES): JobWorker' + chr(10) in text + chr(10) and 'besides main (a critical role like main, not a job): RenderThread' in text, text
    assert 'rec (NVN submitter) 5.30 ms, main 4.95 ms, nvn-translate 6.10 ms' in text, text
    assert "NR-24 switches: DM_NR_GATE_TICK=1 clock reads a frame: short entry 2900, main's GetSystemTick on the host 1000, full clock gates 3; probe: full gate 120.5 ns, short entry 9.5 ns" in text, text
    assert 'NR-28 (DM_NR_LEANCALL=2): 14600 calls a frame, saved 0.117 ms a frame' in text and 'GO for =1' in text and 'cpu7 3.30 GHz 12.0 -> 4.0 (8.0, 26)' in text, text
    got = []
    res = ab([parse(a, 'mx1'), parse(b, 'mx0')], out=got.append)
    assert res['translation submit CPU ms'] == [4.1, 7.9] and res['MX worker CPU ms a frame'] == [4.0, 0.0] and res['dm-nbx0 CPU ms'][1] == 0.0, res
    assert res['fps (NVN PERF, unlimited phase)'] == [77.0, 79.0], res['fps (NVN PERF, unlimited phase)']
    assert abs(res['[nb] mx handoff ms (waited - ran)'][0] - 0.1) < 1e-9 and res['[nb] mx handoff ms (waited - ran)'][1] is None
    assert res['dm-nbx0 CPU ms'] == [4.0, 0.0] and res['process CPU ms a frame'] == [31.4, 31.4], res
    # the C sources still print what this parser reads (fragments of the format strings)
    import pathlib
    dm = pathlib.Path(__file__).resolve().parents[1] / 'dm'
    want = {'host/hle_cpuprof.c': ['process %.2f ms CPU a frame (%.2f cores), run-queue wait %.2f ms a frame; per thread ms a frame (wait) [%%', ' %s %d %.2f (%.2f) [',
                                   'cpus busy %.0f%% (of %d cores: %.2f), %.2f GHz avg, cap %.2f GHz;', 'cpus busy - (/proc/stat not readable), %.2f GHz avg, cap %.2f GHz;', 'a frame (svc %.0f, clock %.0f, obs %.0f) = %.0f k/s; their frames at the probe\'s %.0f ns + the clock\'s %.0f ns = %.3f ms a frame; by thread a frame:',
                                   ' 0x%02x %s%s %.1f x %.1f = %.3f;', ' | G: %.0f takes a frame, %.1f%% found it held, waited %.1f us a frame (longest %.1f us), held %.1f us a frame',
                                   'one gate round trip (dm_gate frame of 800 bytes, lookup, dispatch, back) %.1f ns; the clock gate\'s own guest_ns() %.1f ns',
                                   '[cpuprof] nr24 probe (DM_NR_GATE_TICK=%d): a clock gate through the full gate %.1f ns, through the short entry %.1f ns',
                                   '"[cpuprof] nr24 (%.1f s; a frame): DM_NR_GATE_TICK=%d clock reads by the short entry %.0f, main\'s GetSystemTick on the host %.0f, full clock gates %.0f; "',
                                   '"DM_NR_GATE_ARB=%d bucket takes %.0f (%.1f%% found held), wakes %.0f (futex %.0f), moved onto a lock %.0f, red %llu; "',
                                   '"DM_NR_GATE_YIELD=%d waits %.0f (%.0f woken by progress, %.0f timed out, %.1f us each), would wait %.0f"'],
            'gfx/nvn/nvn_cpuprof.inc': ['%.2f submissions a frame): NVN %.3f ms an interval "', '"in %.0f calls (%.0f ns a call, the wrapper\'s %.0f ns taken out) + nvnQueueSubmitCommands %.3f ms; the thread\'s CPU %.3f ms, wall %.3f ms in the "',
                                        '"interval: NVN = %.0f%% of its CPU; of the NVN time %.3f ms in calls that wait', ' %s %.0f x %.0f ns = %.3f ms;', 'untimed call %.1f ns, timed call %.1f ns, %.1f ns of it inside the stamps'],
            'gfx/vulkan/engine/xbg_cpuprof.inc': ['fps; DM_NB_MX=%d DM_GFX_PRESENT_THREAD=%d DM_NB_MT=%d): translation thread a frame: submit wall %.3f ms CPU %.3f ms "',
                                                  '"(waited %.3f), present wall %.3f ms CPU %.3f ms (waited %.3f), %.2f submissions a frame; MX worker CPU %.3f ms a frame; GPU a frame: engine submissions %.3f ms, "',
                                                  '"between them %.3f ms (present blits', ' passes with draws, %.0f draws, %.0f pipeline binds (%.0f if every pass were sorted by pipeline), %.0f set binds, %.0f draws counting samples; "',
                                                  '%.1f passes, %.0f draws, %.0f binds -> %.0f sorted = %.0f binds saved a frame'],
            'gfx/vulkan/engine/xbg_nb_mx.inc': ['worker ran %.2f ms%s, the translation thread waited %.2f ms', '" (CPU %.2f)"'],
            'gfx/nvn/nvn_leancall.inc': ['" cpu%d %.2f GHz: old %.1f ns (thunk %s %.1f + wrapper %.1f), lean %.1f ns (plain thunk %.1f + entry %.1f%s), "',
                                         '"in place old %.1f / lean %.1f, body %.1f; saved %.1f ns = %.0f cycles;"', '"%.3f ms a frame on cpu%d (%.1f ns a call), %.3f ms on the slowest calibrated core"',
                                         '[nvn] leancall (DM_NR_LEANCALL=1, %.0f s, %.0f frames): %.0f NVN calls a frame through the lean entry;',
                                         '[nvn] leancall (DM_NR_LEANCALL=2, %.0f s, %.0f frames): %.0f calls a frame compared (', 'bad %llu (total %llu)', 'saved by =1 %s', 'stacks taken over %llu; saved %s'],
            'host/hle_dos2_nr_nocall.inc': ['%.0f NVN calls a frame (%.1f%% of the %.0f above) for %.0f stack switches; "', 'binds whose slot already held that handle this frame %.1f a frame',
                                            'an NVN call through thunk + big stack %.1f ns, its body called directly %.1f ns, one stack "', '"switch %.1f ns: =1 would save about %.3f ms a frame (',
                                            'all %.0f calls a frame cost %.3f ms in the "', '%llu texture flushes compared, bad %llu (', '"%llu draws 36 / 37 compared, bad %llu (']}
    for f, frags in want.items():
        p = dm / f
        if not p.exists():
            continue   # the tool copied without the tree
        src = p.read_text(encoding='utf-8')
        for fr in frags:
            assert fr in src, 'the format in %s changed: %r' % (f, fr)
    print('PASS: dos2_cpuprof self-test (parse every line kind, 90 Hz windows out, NR-17/21/23/24 answers, A/B table; the C formats still match)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='*')
    ap.add_argument('--ab', action='store_true', help='A/B table of the logs (first = reference)')
    ap.add_argument('--min-fps', type=float, default=30.0)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return 0
    if not a.logs:
        ap.print_help()
        return 2
    runs = [parse(p) for p in a.logs]
    if a.ab:
        ab(runs, a.min_fps)
    else:
        for r in runs:
            answers(r, a.min_fps)
    return 0


if __name__ == '__main__':
    sys.exit(main())
