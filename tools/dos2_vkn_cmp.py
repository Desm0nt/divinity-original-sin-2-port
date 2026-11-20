"""Divinity: Original Sin 2 DE, VK-NATIVE (stage 5.1 A5): compare two DM_VK_NATIVE_DUMP target dumps.

The dump (dm/gfx/vkn/vkn_pass.c, "debug aid"): DM_VK_NATIVE_DUMP=k1[,k2,...] writes the colour targets of the k-th heavy presented frame
(40 passes or more), each blitted (level 0, layer 0, NEAREST) to 480x216 RGBA8 and saved without alpha as a binary PPM
    <DM_VK_NATIVE_DUMP_DIR | DM_RUNTIME_DIR | dm>/vkn_<k>_<nn>_<handle hex>_<w>x<h>_f<VkFormat>_c<class>.ppm
(nn = order of first use in that frame; DM_VK_NATIVE_DUMP_OFF=1 dumps off-frame recordings as k = 9001...). Pull the directory of
each run (the files of one k are one frame) and compare them: run A (e.g. techniques off) against run B (techniques on).

Targets are matched by (k, handle, size, format, class); a target whose handle differs between the runs is matched by (k, nn, size,
format, class) when that is unique. Per target: equal / differ, the max and mean absolute difference per channel (R G B), the number
of pixels with any channel difference above --tol, and their bounding box. The table is sorted by the number of differing pixels.
Exit code: 0 = no pixel differs by more than --tol (and at most --max-pixels such pixels per target) and no target is missing;
1 = a difference (or a target present in one run only, unless --allow-missing); 2 = usage / input error.

Usage:
  dos2_vkn_cmp.py A B [--k K | --ka KA --kb KB] [--tol N] [--max-pixels N] [--allow-missing] [--diff-dir DIR [--diff-format png|ppm]]
  dos2_vkn_cmp.py --selftest
A / B: a dump directory or one .ppm file each. Without --k: every k found in both runs; if each run holds exactly one k, those two.
--diff-dir: one picture per differing target (<name>.diff.png): run A in grey, differing pixels in red (brighter = bigger difference).
Python standard library only (numpy is used when it is installed: faster on many targets).
"""
import argparse, os, re, struct, sys, tempfile, zlib

try:
    import numpy as np  # optional
except Exception:  # pragma: no cover
    np = None

NAME = re.compile(r'^vkn_(\d+)_(\d+)_([0-9a-fA-F]+)_(\d+)x(\d+)_f(\d+)_c(\d+)\.ppm$')


def read_ppm(path):
    """(w, h, rgb bytes) of a binary PPM (P6, maxval 255; '#' comments allowed in the header)."""
    with open(path, 'rb') as f:
        data = f.read()
    fields, i = [], 0
    while len(fields) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if i < len(data) and data[i:i + 1] == b'#':
            while i < len(data) and data[i:i + 1] not in (b'\n', b'\r'):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace() and data[j:j + 1] != b'#':
            j += 1
        if j == i:
            raise ValueError('%s: truncated header' % path)
        fields.append(data[i:j]); i = j
    if fields[0] != b'P6':
        raise ValueError('%s: not a binary PPM (P6)' % path)
    w, h, mx = int(fields[1]), int(fields[2]), int(fields[3])
    if mx != 255:
        raise ValueError('%s: maxval %d (only 255)' % (path, mx))
    i += 1  # the single whitespace after maxval
    px = data[i:i + w * h * 3]
    if len(px) != w * h * 3:
        raise ValueError('%s: %d pixel bytes, %d expected' % (path, len(px), w * h * 3))
    return w, h, px


def write_ppm(path, w, h, rgb):
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h)); f.write(bytes(rgb))


def write_png(path, w, h, rgb):
    """A minimal PNG encoder: 8-bit RGB, filter 0 on every row, one IDAT (zlib of the standard library)."""
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0); raw += rgb[y * w * 3:(y + 1) * w * 3]
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def scan(path):
    """{(k, nn, handle, w, h, fmt, cls): file} of a dump directory or a single file."""
    files = [path] if os.path.isfile(path) else [os.path.join(path, n) for n in sorted(os.listdir(path))]
    out = {}
    for p in files:
        m = NAME.match(os.path.basename(p))
        if not m:
            if os.path.isfile(path):  # a single file of another name: k 0, nn 0
                out[(0, 0, '0', 0, 0, 0, 0)] = p
            continue
        k, nn, hd, w, h, f, c = m.groups()
        out[(int(k), int(nn), hd.lower(), int(w), int(h), int(f), int(c))] = p
    return out


def pair(a, b, ka, kb):
    """Matched (name, file A, file B) and the names present in one run only."""
    A = {key: p for key, p in a.items() if key[0] == ka}
    B = {key: p for key, p in b.items() if key[0] == kb}
    by_h = {(key[2],) + key[3:]: key for key in B}
    used, pairs, only_a = set(), [], []
    rest = []
    for key in sorted(A):
        kk = by_h.get((key[2],) + key[3:])
        if kk is not None and kk not in used:
            used.add(kk); pairs.append((key, A[key], B[kk]))
        else:
            rest.append(key)
    for key in rest:  # handles differ between the runs: the same order of first use, size, format, class (only when unique)
        cand = [kk for kk in B if kk not in used and kk[1] == key[1] and kk[3:] == key[3:]]
        if len(cand) == 1:
            used.add(cand[0]); pairs.append((key, A[key], B[cand[0]]))
        else:
            only_a.append(key)
    only_b = [kk for kk in sorted(B) if kk not in used]
    return pairs, only_a, only_b


def diff(pa, pb, tol):
    """The difference of two pictures: dict (or None when the sizes differ)."""
    wa, ha, xa = read_ppm(pa); wb, hb, xb = read_ppm(pb)
    if (wa, ha) != (wb, hb):
        return None
    w, h = wa, ha
    if np is not None:
        a = np.frombuffer(xa, np.uint8).reshape(h, w, 3).astype(np.int16); b = np.frombuffer(xb, np.uint8).reshape(h, w, 3).astype(np.int16)
        d = np.abs(a - b); bad = (d > tol).any(axis=2); n = int(bad.sum())
        mx = [int(d[:, :, c].max()) for c in range(3)]; mean = [float(d[:, :, c].mean()) for c in range(3)]
        box = None
        if n:
            ys, xs = np.nonzero(bad); box = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
        dm = d.max(axis=2).astype(np.uint8).tobytes()
        mask = bad.astype(np.uint8).tobytes()
    else:
        mx, tot, n, box = [0, 0, 0], [0, 0, 0], 0, None
        dm, mask = bytearray(w * h), bytearray(w * h)
        x0 = y0 = 1 << 30; x1 = y1 = -1
        for i in range(w * h):
            o = 3 * i; m = 0
            for c in range(3):
                v = xa[o + c] - xb[o + c]
                if v < 0:
                    v = -v
                tot[c] += v
                if v > mx[c]:
                    mx[c] = v
                if v > m:
                    m = v
            dm[i] = m
            if m > tol:
                n += 1; mask[i] = 1; x, y = i % w, i // w
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
        mean = [t / float(w * h) for t in tot]
        if n:
            box = (x0, y0, x1, y1)
    return {'w': w, 'h': h, 'max': mx, 'mean': mean, 'pixels': n, 'box': box, 'a': xa, 'dmax': bytes(dm), 'mask': bytes(mask)}


def diff_picture(r):
    """Run A in grey (dimmed), differing pixels in red (brighter = bigger difference)."""
    w, h, a, dm, mask = r['w'], r['h'], r['a'], r['dmax'], r['mask']
    out = bytearray(w * h * 3)
    for i in range(w * h):
        if mask[i]:
            out[3 * i] = min(255, 96 + dm[i] * 4); out[3 * i + 1] = 0; out[3 * i + 2] = 0
        else:
            g = (a[3 * i] * 3 + a[3 * i + 1] * 6 + a[3 * i + 2]) // 40
            out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = g
    return out


def name_of(key):
    return 'vkn_%d_%02d_%s_%dx%d_f%d_c%d' % key


def compare(a_path, b_path, k=None, ka=None, kb=None, tol=0, max_pixels=0, allow_missing=False, diff_dir=None, diff_format='png', out=sys.stdout):
    a, b = scan(a_path), scan(b_path)
    if not a or not b:
        print('no dump files in %s' % (a_path if not a else b_path), file=out); return 2, []
    kas, kbs = sorted({key[0] for key in a}), sorted({key[0] for key in b})
    if k is not None:
        plan = [(k, k)]
    elif ka is not None or kb is not None:
        plan = [(ka if ka is not None else kas[0], kb if kb is not None else kbs[0])]
    else:
        common = [x for x in kas if x in kbs]
        plan = [(x, x) for x in common] if common else ([(kas[0], kbs[0])] if len(kas) == 1 and len(kbs) == 1 else [])
    if not plan:
        print('runs hold different dump numbers (A %s, B %s): name them with --ka / --kb' % (kas, kbs), file=out); return 2, []
    rows, rc = [], 0
    for (xa, xb) in plan:
        pairs, only_a, only_b = pair(a, b, xa, xb)
        for key, pa, pb in pairs:
            r = diff(pa, pb, tol)
            if r is None:
                rows.append((key, None)); rc = 1; continue
            rows.append((key, r))
            if r['pixels'] > max_pixels:
                rc = 1
                if diff_dir:
                    os.makedirs(diff_dir, exist_ok=True)
                    pic = diff_picture(r); base = os.path.join(diff_dir, name_of(key) + '.diff.' + diff_format)
                    (write_png if diff_format == 'png' else write_ppm)(base, r['w'], r['h'], pic)
        for key in only_a:
            rows.append((key, 'only A')); rc = rc or (0 if allow_missing else 1)
        for key in only_b:
            rows.append((key, 'only B')); rc = rc or (0 if allow_missing else 1)
    def order(row):
        r = row[1]
        return (-(r['pixels']) if isinstance(r, dict) else -10 ** 9, name_of(row[0]))
    rows.sort(key=order)
    print('%-52s %-7s %8s %-13s %-20s %s' % ('target (A)', 'result', 'pixels', 'max R G B', 'mean R G B', 'box x0,y0-x1,y1'), file=out)
    for key, r in rows:
        if r is None:
            print('%-52s %-7s' % (name_of(key), 'SIZE'), file=out)
        elif isinstance(r, str):
            print('%-52s %-7s' % (name_of(key), r.upper()), file=out)
        else:
            res = 'equal' if r['pixels'] == 0 and max(r['max']) == 0 else ('within' if r['pixels'] <= max_pixels else 'DIFFER')
            box = '%d,%d-%d,%d' % r['box'] if r['box'] else '-'
            print('%-52s %-7s %8d %-13s %-20s %s' % (name_of(key), res, r['pixels'], '%d %d %d' % tuple(r['max']), '%.2f %.2f %.2f' % tuple(r['mean']), box), file=out)
    nd = sum(1 for _, r in rows if not isinstance(r, dict) or r['pixels'] > max_pixels)
    print('%d targets compared, %d differ / missing (tolerance %d, pixels allowed %d)%s' % (len(rows), nd, tol, max_pixels, '' if np is not None else ' [no numpy]'), file=out)
    return rc, rows


def selftest():
    import io
    fails = []
    def check(c, what):
        if not c:
            fails.append(what); print('FAIL', what)
    W, H = 48, 20
    def pic(seed):
        return bytearray(((x * 7 + y * 13 + seed) & 0xff) for y in range(H) for x in range(W) for _ in range(3))
    with tempfile.TemporaryDirectory() as td:
        A, B = os.path.join(td, 'a'), os.path.join(td, 'b'); os.makedirs(A); os.makedirs(B)
        base = pic(1)
        names = [(600, 0, '100000a01', 1920, 1080, 37, 1), (600, 1, '100000a02', 1920, 1080, 97, 1), (600, 2, '100000a03', 960, 540, 37, 1), (600, 3, '100000a04', 64, 64, 37, 1)]
        for key in names:
            write_ppm(os.path.join(A, name_of(key) + '.ppm'), W, H, base)
        # B: target 0 equal; target 1 three pixels off by 5 / 40; target 2 under another handle (matched by order), equal; target 3 missing;
        # an extra target only in B
        write_ppm(os.path.join(B, name_of(names[0]) + '.ppm'), W, H, base)
        b1 = bytearray(base)
        for (x, y, dv) in ((3, 2, 5), (10, 7, 40), (47, 19, 5)):
            o = 3 * (y * W + x); b1[o] = (b1[o] + dv) & 0xff
        write_ppm(os.path.join(B, name_of(names[1]) + '.ppm'), W, H, b1)
        write_ppm(os.path.join(B, name_of((600, 2, '100000b99', 960, 540, 37, 1)) + '.ppm'), W, H, base)
        write_ppm(os.path.join(B, name_of((600, 9, '100000c00', 32, 32, 37, 1)) + '.ppm'), W, H, base)
        # a header with a comment
        with open(os.path.join(td, 'c.ppm'), 'wb') as f:
            f.write(b'P6\n# made by hand\n%d %d\n255\n' % (W, H)); f.write(bytes(base))
        check(read_ppm(os.path.join(td, 'c.ppm'))[2] == bytes(base), 'ppm header with a comment')
        s = io.StringIO(); rc, rows = compare(A, B, out=s)
        check(rc == 1, 'exit 1 on differences (got %d)' % rc)
        got = {name_of(k): r for k, r in rows}
        r1 = got.get(name_of(names[1]))
        check(isinstance(r1, dict) and r1['pixels'] == 3 and r1['max'] == [40, 0, 0] and r1['box'] == (3, 2, 47, 19), 'differing pixels, max, box: %r' % (r1 and {k: r1[k] for k in ('pixels', 'max', 'box')}))
        check(isinstance(r1, dict) and abs(r1['mean'][0] - 50.0 / (W * H)) < 1e-9, 'mean difference')
        r0 = got.get(name_of(names[0])); check(isinstance(r0, dict) and r0['pixels'] == 0, 'equal target')
        r2 = got.get(name_of(names[2])); check(isinstance(r2, dict) and r2['pixels'] == 0, 'matched by order when the handle differs')
        check(got.get(name_of(names[3])) == 'only A', 'missing in B')
        check(sum(1 for r in got.values() if r == 'only B') == 1, 'extra in B')
        check(name_of(rows[0][0]) in (name_of(names[3]), name_of((600, 9, '100000c00', 32, 32, 37, 1))) and name_of(rows[2][0]) == name_of(names[1]), 'sorted: missing first, then by pixels')
        # tolerance 5: only the 40 counts; allowed pixels 1 + missing allowed: exit 0
        s = io.StringIO(); rc, rows = compare(A, B, tol=5, out=s); got = {name_of(k): r for k, r in rows}
        check(got[name_of(names[1])]['pixels'] == 1, 'tolerance')
        s = io.StringIO(); rc, rows = compare(A, B, tol=5, max_pixels=1, allow_missing=True, out=s); check(rc == 0, 'tolerated: exit 0 (got %d)' % rc)
        # the same runs: exit 0; single files; k selection
        s = io.StringIO(); rc, _ = compare(A, A, out=s); check(rc == 0, 'a run against itself')
        s = io.StringIO(); rc, _ = compare(os.path.join(A, name_of(names[1]) + '.ppm'), os.path.join(B, name_of(names[1]) + '.ppm'), out=s); check(rc == 1, 'two files')
        write_ppm(os.path.join(B, name_of((700, 0, '100000a01', 1920, 1080, 37, 1)) + '.ppm'), W, H, base)
        s = io.StringIO(); rc, rows = compare(A, B, ka=600, kb=700, allow_missing=True, out=s); check(rc == 0 and len(rows) == 4, 'ka / kb (rc %d, %d rows)' % (rc, len(rows)))
        # the difference picture: a valid PNG (chunk CRCs, inflated size) and a PPM
        D = os.path.join(td, 'd'); s = io.StringIO(); compare(A, B, diff_dir=D, out=s)
        pngs = [n for n in os.listdir(D)] if os.path.isdir(D) else []
        check(len(pngs) == 1 and pngs[0].endswith('.diff.png'), 'one difference picture: %r' % pngs)
        if pngs:
            with open(os.path.join(D, pngs[0]), 'rb') as f:
                data = f.read()
            ok = data[:8] == b'\x89PNG\r\n\x1a\n'; i = 8; idat = b''; w = h = 0
            while ok and i < len(data):
                n, t = struct.unpack('>I4s', data[i:i + 8]); body = data[i + 8:i + 8 + n]; crc = struct.unpack('>I', data[i + 8 + n:i + 12 + n])[0]
                ok = ok and crc == zlib.crc32(t + body) & 0xffffffff
                if t == b'IHDR':
                    w, h = struct.unpack('>II', body[:8])
                if t == b'IDAT':
                    idat += body
                i += 12 + n
            raw = zlib.decompress(idat) if ok else b''
            check(ok and (w, h) == (W, H) and len(raw) == H * (1 + 3 * W), 'PNG structure')
            o = 2 * (1 + 3 * W) + 1 + 3 * 3; px = raw[o:o + 3] if raw else b''   # pixel (3, 2): row 2 (filter byte + 3 W), column 3
            check(len(px) == 3 and px[0] > 0 and px[1] == 0 and px[2] == 0, 'a differing pixel is red')
        s = io.StringIO(); D2 = os.path.join(td, 'd2'); compare(A, B, diff_dir=D2, diff_format='ppm', out=s)
        check(os.path.isdir(D2) and read_ppm(os.path.join(D2, os.listdir(D2)[0]))[0] == W, 'PPM difference picture')
        # the pure-python path gives the numpy path's numbers
        global np
        if np is not None:
            keep = np; np = None
            try:
                rp = diff(os.path.join(A, name_of(names[1]) + '.ppm'), os.path.join(B, name_of(names[1]) + '.ppm'), 0)
            finally:
                np = keep
            rn = diff(os.path.join(A, name_of(names[1]) + '.ppm'), os.path.join(B, name_of(names[1]) + '.ppm'), 0)
            check(all(rp[k] == rn[k] for k in ('pixels', 'max', 'box', 'dmax', 'mask')) and all(abs(x - y) < 1e-9 for x, y in zip(rp['mean'], rn['mean'])), 'numpy and pure python agree')
        # sizes that differ
        write_ppm(os.path.join(td, 's.ppm'), W, H - 1, base[:W * (H - 1) * 3])
        s = io.StringIO(); rc, rows = compare(os.path.join(td, 'c.ppm'), os.path.join(td, 's.ppm'), out=s); check(rc == 1 and rows[0][1] is None, 'size mismatch')
    print('dos2_vkn_cmp selftest: %s (%d failures)%s' % ('PASS' if not fails else 'FAIL', len(fails), '' if np is not None else ' [no numpy]'))
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description='compare two DM_VK_NATIVE_DUMP target dumps (dos2_vkn_cmp.py --help: the whole story in the head comment)')
    ap.add_argument('a', nargs='?'); ap.add_argument('b', nargs='?')
    ap.add_argument('--k', type=int); ap.add_argument('--ka', type=int); ap.add_argument('--kb', type=int)
    ap.add_argument('--tol', type=int, default=0, help='a pixel differs when a channel differs by more than this (default 0: exact)')
    ap.add_argument('--max-pixels', type=int, default=0, help='differing pixels allowed per target (default 0)')
    ap.add_argument('--allow-missing', action='store_true', help='a target present in one run only does not fail')
    ap.add_argument('--diff-dir'); ap.add_argument('--diff-format', choices=('png', 'ppm'), default='png')
    ap.add_argument('--selftest', action='store_true')
    o = ap.parse_args()
    if o.selftest:
        return selftest()
    if not o.a or not o.b:
        ap.print_usage(); return 2
    for p in (o.a, o.b):
        if not os.path.exists(p):
            print('%s: not found' % p); return 2
    try:
        rc, _ = compare(o.a, o.b, k=o.k, ka=o.ka, kb=o.kb, tol=o.tol, max_pixels=o.max_pixels, allow_missing=o.allow_missing, diff_dir=o.diff_dir, diff_format=o.diff_format)
    except ValueError as e:
        print(e); return 2
    return rc


if __name__ == '__main__':
    sys.exit(main())
