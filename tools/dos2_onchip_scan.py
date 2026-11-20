"""DOS2 NR-ONCHIP: which deferred passes could run as one render pass with subpasses (input attachments), and what that saves on a tiler.

Reads NB IR dumps (nb_ir_*_p*.txt, DM_NB=1 dump; grammar: tools/dos2_nb_check.py) and, optionally, a dm log (boot.log) for the driver's
on-chip features. Per frame it finds the deferred chain the engine draws every 3D frame:
    G-buffer (>= 3 colour + depth, many draws) -> LinearizeDepth -> [shadow cascades into the atlas] -> CascadedPCF_* -> DeferredSky ->
    ClusteredLightingWithSSAO
(fragment keys below; every read of an earlier chain output by these shaders is a texelFetch at the fragment's own pixel - see
tools/dos2_post/*_mobile.frag fast path, render scale 1 = what the host writes) and models the DRAM traffic of a tile GPU:
    load  = attachment loaded at pass begin (plan 'load'), store = stored at pass end (plan 'store'), read = a full-size sampled read of a
    chain image by a chain pass (each texel once). Clears cost nothing. 1 MB = 1e6 bytes. No framebuffer compression (AFBC / PVRIC / UBWC
    would scale every number by roughly 0.5-0.7 for G-buffer data).
Variants (a variant is only reported when its passes are all present in the frame, back to back except for passes it may skip):
    S3  = CascadedPCF -> DeferredSky -> ClusteredLighting as 3 subpasses (back to back in the stream; only the lighting stage reads
          input attachments: shadow mask + specular IBL; no G-buffer pipeline changes)
    S2  = DeferredSky -> ClusteredLighting (frames without the shadow resolve, c4[0].w == 0)
    G   = G-buffer -> LinearizeDepth -> CascadedPCF -> DeferredSky -> ClusteredLighting as one render pass: needs the shadow-map passes
          between LinearizeDepth and CascadedPCF moved before the G-buffer pass (they read neither) and every G-buffer pipeline built for
          subpass 0 of the merged pass
Liveness after the chain: the next event of an image in the dump: read / copy source / pass load -> live; full clear / pass clear / dc -> dead;
none (end of the part) -> 'open' (counted live, i.e. stored, in the 'safe' column; dead in the 'dead-open' column).
Usage: dos2_onchip_scan.py <dump or dir> ... [--log boot.log] [--frames] [--w W --h H]
"""
import argparse, re, sys
from pathlib import Path

LIN = {'f23c09d198b55dc5'}
PCF = {'1372db843749e1d3', 'a758e42af8853343', '2a221ee245d55aab'}
SKY = {'640a4c497bcc748b'}
LIGHT = {'d236b8d5b7ffad3c'}
NAMES = {**{k: 'LinearizeDepth' for k in LIN}, **{k: 'CascadedPCF' for k in PCF}, **{k: 'DeferredSky' for k in SKY},
         **{k: 'ClusteredLighting' for k in LIGHT}}
BPP = {0x1: 1, 0x2: 1, 0x5: 2, 0x6: 2, 0x7: 2, 0xA: 4, 0xB: 4, 0xD: 2, 0x11: 4, 0x12: 4, 0x16: 8, 0x25: 4, 0x26: 4, 0x29: 8, 0x2A: 8,
       0x2E: 16, 0x32: 2, 0x33: 4, 0x34: 4, 0x35: 4, 0x36: 8, 0x37: 4, 0x38: 4, 0x3D: 4, 0x3F: 4, 0x40: 4}
# effective DRAM bandwidth (GB/s) a full-screen pass sees, per class (assumptions, see docs/dos2/nr/NR-ONCHIP.md section 4)
CLASSES = [('Adreno 750 Turnip TU_DEBUG=sysmem (Mi 14)', None), ('Mali-G57 MC2 (LPDDR4X x32)', 11.0), ('Mali-G615/G715 (LPDDR5/5X x64)', 35.0),
           ('PowerVR GE8320 / BXM (LPDDR4X x32)', 8.0), ('PowerVR DXT (Tensor G5, LPDDR5X)', 40.0),
           ('Adreno 6xx/7xx stock driver, GMEM (Snapdragon 7)', 20.0),
           ('Adreno 750 if Turnip ran this one pass in GMEM (driver change, minus the binning pass)', 50.0)]


class Pass:
    def __init__(self, idx, atts):
        self.idx, self.atts = idx, atts            # atts: {slot: addr}
        self.draws, self.fs, self.reads, self.ld, self.st, self.kind = 0, None, [], {}, {}, None

    def key(self): return self.fs if self.draws == 1 else None


def parse(path):
    """-> list of events: ('pass', Pass) / ('clear', addr, full) / ('op', kind, src, dst); images {addr: (w, h, fmt)}"""
    ev, img, cur, last = [], {}, None, None
    for line in Path(path).read_text(errors='replace').splitlines():
        t = line.split()
        if not t: continue
        if t[0] == 'pass':
            cur = Pass(len(ev), {k: int(v, 16) for k, v in (x.split('=') for x in t[1:] if '=' in x)}); ev.append(('pass', cur)); continue
        if cur is not None:
            if t[0] == 'draws': cur.draws = int(t[1]); continue
            if t[0].startswith('fs=') and cur.fs is None: cur.fs = t[0][3:]; continue
            if t[0] == 'read': cur.reads.append(int(t[1], 16)); continue
            if t[0] == 'end': last, cur = cur, None; continue
            if t[0] == 'clear':                      # a clear inside the pass (RT clear): a write, no traffic
                ev.append(('clear', int(t[1], 16), 'full=1' in line)); continue
            continue
        if line.startswith('# plan') and last is not None:
            last.kind = t[3] if len(t) > 3 else None
            for x in t[4:]:
                m = re.match(r'(c\d|ds):(\w+)/(\w+)', x)
                if m: last.ld[m.group(1)], last.st[m.group(1)] = m.group(2), m.group(3)
            last = None; continue
        if line.startswith('# image'):
            m = re.match(r'# image (0x[0-9a-f]+) (\d+)x(\d+) fmt=(0x[0-9a-f]+)', line)
            if m: img[int(m.group(1), 16)] = (int(m.group(2)), int(m.group(3)), int(m.group(4), 16))
            continue
        if t[0] == 'clear': ev.append(('clear', int(t[1], 16), 'full=1' in line)); continue
        if t[0] == 'op':
            kv = dict(x.split('=') for x in t[2:] if '=' in x)
            ev.append(('op', t[1], int(kv['src'], 16) if 'src' in kv else None, int(kv['dst'], 16) if 'dst' in kv else None))
    return ev, img


def nbytes(addr, img, px):
    fmt = img.get(addr, (0, 0, 0x25))[2]
    return BPP.get(fmt, 4) * px


def liveness(ev, start, addr):
    """next event of addr after event index start: 'live' / 'dead' / 'open'"""
    for e in ev[start + 1:]:
        if e[0] == 'clear' and e[1] == addr: return 'dead' if e[2] else 'live'
        if e[0] == 'op' and addr in (e[2], e[3]): return 'live' if e[2] == addr or e[1] != 'copy' else 'dead'
        if e[0] == 'pass':
            p = e[1]
            if addr in p.reads: return 'live'
            for s, a in p.atts.items():
                if a == addr:
                    l = p.ld.get(s, 'load')
                    if l == 'load': return 'live' if p.draws else 'open-skip'
                    return 'dead'
    return 'open'


def model(ev, img, chain, px):
    """traffic (MB) of the chain's passes as separate passes and merged; chain = list of event indices of passes (stream order)"""
    passes = [ev[i][1] for i in chain]
    written = set()
    sep = merged_safe = merged_dead = 0.0
    first, lastw = {}, {}
    for p in passes:
        for s, a in p.atts.items():
            first.setdefault(a, (p, s)); lastw[a] = (p, s)
    for p in passes:
        for s, a in p.atts.items():
            b = nbytes(a, img, px)
            if p.ld.get(s) == 'load': sep += b
            if p.st.get(s, 'store') == 'store': sep += b
        for r in set(p.reads):
            if r in written: sep += nbytes(r, img, px)            # merged: an input attachment, no DRAM read
        written.update(p.atts.values())
    live = {}
    for a, (p, s) in first.items():
        b = nbytes(a, img, px)
        if p.ld.get(s) == 'load': merged_safe += b; merged_dead += b
        lv = liveness(ev, chain[-1], a); live[a] = lv
        lp, ls = lastw[a]
        if lp.st.get(ls, 'store') == 'store':
            if lv in ('live', 'open', 'open-skip'): merged_safe += b
            if lv == 'live': merged_dead += b
    return sep / 1e6, merged_safe / 1e6, merged_dead / 1e6, live


def chains(ev):
    """the frame's deferred chain: indices of G-buffer, LinearizeDepth, the passes between, PCF, Sky, Light"""
    out = {}
    ps = [(i, e[1]) for i, e in enumerate(ev) if e[0] == 'pass' and e[1].draws]
    for n, (i, p) in enumerate(ps):
        k = p.key()
        if k in LIGHT:
            out['light'] = i
            if n and ps[n - 1][1].key() in SKY:
                out['sky'] = ps[n - 1][0]
                if n > 1 and ps[n - 2][1].key() in PCF: out['pcf'] = ps[n - 2][0]
            break
    if 'light' not in out: return out
    for i, p in ps:
        if i >= out['light']: break
        if p.key() in LIN: out['lin'] = i
        if sum(1 for s in p.atts if s.startswith('c')) >= 3 and 'ds' in p.atts and p.draws > 8: out['gbuf'] = i
    return out


def between(ev, a, b):
    """passes with draws and stand-alone ops strictly between event indices a and b"""
    return [e for e in ev[a + 1:b] if (e[0] == 'pass' and e[1].draws) or e[0] == 'op']


def scan(files, w, h, show):
    px = w * h
    tot = {'S3': [], 'S2': [], 'G': []}; nfr = 0; missing = {}
    for f in files:
        ev, img = parse(f); c = chains(ev)
        if 'light' not in c: continue
        nfr += 1; row = [Path(f).name]
        if 'pcf' in c:
            sep, ms, md, live = model(ev, img, [c['pcf'], c['sky'], c['light']], px)
            tot['S3'].append((sep, ms, md)); row.append(f"S3 {sep:.1f} -> {ms:.1f} / {md:.1f} MB")
            if show: row.append('  after: ' + ', '.join(f"{a:#x} {v}" for a, v in live.items()))
        elif 'sky' in c:
            sep, ms, md, live = model(ev, img, [c['sky'], c['light']], px)
            tot['S2'].append((sep, ms, md)); row.append(f"S2 {sep:.1f} -> {ms:.1f} / {md:.1f} MB")
        else: missing['sky not right before the lighting'] = missing.get('sky not right before the lighting', 0) + 1
        if 'gbuf' in c and 'lin' in c and 'sky' in c:
            mem = [c['gbuf'], c['lin']] + ([c['pcf']] if 'pcf' in c else []) + [c['sky'], c['light']]
            moved = [e for e in between(ev, c['lin'], c.get('pcf', c['sky']))]
            gap = [e for e in between(ev, c['gbuf'], c['lin'])]
            sep, ms, md, live = model(ev, img, mem, px)
            tot['G'].append((sep, ms, md))
            row.append(f"G {sep:.1f} -> {ms:.1f} / {md:.1f} MB (passes to move before the G-buffer: {sum(1 for e in moved if e[0] == 'pass')}, "
                       f"draws {sum(e[1].draws for e in moved if e[0] == 'pass')}, ops {sum(1 for e in moved if e[0] == 'op')}; "
                       f"between G-buffer and LinearizeDepth: {len(gap)})")
            if show: row.append('  after: ' + ', '.join(f"{a:#x} {v}" for a, v in live.items()))
        if show: print(' | '.join(row))
    print(f"frames with the lighting pass: {nfr} of {len(files)}" + (f"; {missing}" if missing else ''))
    for k, v in tot.items():
        if not v: continue
        n = len(v); sep, ms, md = (sum(x[i] for x in v) / n for i in range(3))
        print(f"{k:3s} in {n} frames: separate passes {sep:.1f} MB/frame -> one render pass {ms:.1f} (open images stored) / "
              f"{md:.1f} (open images dropped): saves {sep - ms:.1f} .. {sep - md:.1f} MB/frame at {w}x{h}")
        for name, bw in CLASSES:
            if bw is None: print(f"      {name}: 0 ms (no tile memory in use: the merged pass runs in sysmem, same DRAM traffic)"); continue
            print(f"      {name}: {(sep - ms) / bw:.2f} .. {(sep - md) / bw:.2f} ms raw, x0.5-0.7 with framebuffer compression")


def device(log):
    txt = Path(log).read_text(errors='replace')
    ext = set()
    for m in re.finditer(r'Device extensions \[\d+\]: (.*)', txt): ext.update(m.group(1).split())
    want = ['VK_KHR_dynamic_rendering', 'VK_KHR_dynamic_rendering_local_read', 'VK_EXT_rasterization_order_attachment_access',
            'VK_ARM_rasterization_order_attachment_access', 'VK_EXT_attachment_feedback_loop_layout', 'VK_EXT_load_store_op_none',
            'VK_KHR_load_store_op_none', 'VK_QCOM_render_pass_shader_resolve', 'VK_QCOM_tile_properties', 'VK_EXT_shader_tile_image']
    g = re.search(r'gpu 0: "([^"]+)"', txt); mem = re.search(r'gpu 0 memory: (.*)', txt); tu = re.search(r'TU_DEBUG=(\S*)', txt)
    print(f"device: {g.group(1) if g else '?'}; TU_DEBUG={tu.group(1) if tu else '(not set by the port)'}")
    for x in want: print(f"  {x}: {'yes' if x in ext else 'no'}")
    if mem:
        lazy = [t for t in re.findall(r't\d+=h\d+/(0x[0-9a-f]+)', mem.group(1)) if int(t, 16) & 0x10]
        print(f"  memory types: {mem.group(1).strip()} -> lazily allocated (transient attachments): {'yes ' + ','.join(lazy) if lazy else 'no'}")
    if tu and 'sysmem' in tu.group(1):
        print("  Turnip renders every pass in sysmem (TU_DEBUG=sysmem, android/vk_driver.c): subpasses / transient attachments save no DRAM "
              "traffic on this device; tile memory is only used with TU_DEBUG unset (autotune) or gmem")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='*'); ap.add_argument('--log'); ap.add_argument('--frames', action='store_true')
    ap.add_argument('--w', type=int, default=2160); ap.add_argument('--h', type=int, default=972)
    a = ap.parse_args()
    if a.log: device(a.log)
    files = []
    for p in a.paths:
        p = Path(p); files += sorted(p.glob('nb_ir_*_p*.txt')) if p.is_dir() else [p]
    if files: scan(files, a.w, a.h, a.frames)
    elif not a.log: ap.error('no dumps and no --log')


if __name__ == '__main__':
    main()
