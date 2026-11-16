"""[DOS2] The touch UI's own art: what the HUD / column / panel passes draw into the console movies instead of the iPad build's graphics
(owner 16.11: TouchUI.pak is built from the player's Switch dump alone; no iPad file, and neither the repository nor the APK holds Larian art).

Three kinds of element (docs/dos2/TOUCH_ART.md has the inventory):
  png   our own drawings, assets/touchui/<name>.png (original art made for this port, RGBA, drawn at 2x the stage size: PX = 0.5 stage px per
        PNG px). Each goes in as a DefineBitsLossless2 + a DefineShape filled with it (clipped, smoothed), its box 0,0 .. w/2,h/2 (or centred).
  sw    a symbol of the player's own Switch Game.pak, ONLY where the iPad used the very same drawing (proved per element in TOUCH_ART.md:
        same bitmaps, renders equal); grafted at setup time from the dump (tools/dos2_swf_graft.py), never shipped.
  vec   pure geometry drawn here (the hot bar row / fold arrows: a flat solid arrow, as the iPad's own vector one).

  python -I tools/dos2_touchart.py check            every asset of ASSETS is there, RGBA, of the size the passes expect
"""
import hashlib, struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dos2_swf_graft as gr

ROOT = Path(__file__).resolve().parent.parent
ASSET_DIR = ROOT / 'assets' / 'touchui'
PX = 0.5   # stage px per PNG px
# name -> (w, h) PNG px; the passes rely on these sizes (layout = the iPad's)
ASSETS = {
    'plate_round_copper': (148, 148), 'plate_round_steel': (138, 138), 'glyph_cancel': (80, 80), 'icon_menu': (80, 80), 'icon_tactical': (104, 108),
    'plate_square_copper': (148, 148), 'plate_endturn': (414, 88), 'icon_sneak': (92, 92), 'icon_sheath': (92, 92),
    'btn_character': (210, 210), 'ico_inventory': (118, 108), 'ico_charsheet': (118, 108), 'ico_skills': (118, 108), 'ico_craft': (118, 108),
    'ico_journal': (118, 108), 'icon_map': (118, 108),
    'pan_close': (100, 100), 'pan_helmet': (128, 128), 'pan_list': (128, 128), 'pan_multi': (128, 128), 'pan_plus': (68, 68), 'pan_minus': (68, 68),
    'pan_balance': (130, 170), 'pan_info': (80, 56),
}
ARROW_RGB = (150, 141, 120)   # the hot bar arrows' flat khaki (the iPad's own arrow is a vector of this colour)


# ---------------------------------------------------------------- PNG (numpy only: the setup has no Pillow)
def read_png(path):
    """8-bit RGBA / RGB, non-interlaced PNG -> numpy uint8 HxWx4"""
    import numpy as np
    d = Path(path).read_bytes(); assert d[:8] == b'\x89PNG\r\n\x1a\n', '%s: not a PNG' % path
    p = 8; idat = []; w = h = None
    while p < len(d):
        n, t = struct.unpack_from('>I4s', d, p); c = d[p + 8:p + 8 + n]; p += 12 + n
        if t == b'IHDR':
            w, h, depth, ct, _, _, il = struct.unpack('>IIBBBBB', c)
            assert depth == 8 and ct in (2, 6) and not il, '%s: want 8-bit RGB(A), not interlaced' % path
            ch = 4 if ct == 6 else 3
        elif t == b'IDAT': idat.append(c)
        elif t == b'IEND': break
    raw = np.frombuffer(zlib.decompress(b''.join(idat)), np.uint8).reshape(h, 1 + w * ch)
    out = np.zeros((h, w * ch), np.uint8); prev = np.zeros(w * ch, np.uint8)
    for y in range(h):
        f, line = raw[y, 0], raw[y, 1:]
        if f == 0: cur = line.copy()
        elif f == 2: cur = (line.astype(np.uint16) + prev).astype(np.uint8)
        elif f == 1:
            cur = line.reshape(w, ch).copy()
            cur = np.cumsum(cur, 0, dtype=np.uint8).reshape(-1)
        else:
            cur = np.zeros(w * ch, np.uint8); L = line.astype(np.int32); P = prev.astype(np.int32)
            for x in range(w * ch):
                a = int(cur[x - ch]) if x >= ch else 0; b = int(P[x]); c = int(P[x - ch]) if x >= ch else 0
                if f == 3: v = (a + b) >> 1
                else:
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    v = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[x] = (L[x] + v) & 255
        out[y] = cur; prev = cur
    img = out.reshape(h, w, ch)
    if ch == 3: img = np.concatenate([img, np.full((h, w, 1), 255, np.uint8)], -1)
    return img


def write_png(path, img):
    """numpy uint8 HxWx4 -> PNG (filter Sub on every row: read_png's fast path)"""
    import numpy as np
    h, w, _ = img.shape; a = img.astype(np.int16)
    sub = a.copy(); sub[:, 1:] = (a[:, 1:] - a[:, :-1]) % 256
    rows = b''.join(b'\x01' + sub[y].astype(np.uint8).tobytes() for y in range(h))
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def asset(name):
    p = ASSET_DIR / (name + '.png')
    if not p.exists(): sys.exit('%s: missing (our touch UI art; assets/touchui/README.md)' % p)
    img = read_png(p); want = ASSETS.get(name)
    assert want is None or (img.shape[1], img.shape[0]) == want, '%s: %dx%d, the passes expect %dx%d' % (p, img.shape[1], img.shape[0], want[0], want[1])
    return img


# ---------------------------------------------------------------- SWF tags
def _nb(*v): return max(2, max(abs(int(x)).bit_length() for x in v) + 1)


def _rect_bits(x0, x1, y0, y1):
    n = _nb(x0, x1, y0, y1); return gr.wbits([(n, 5), (x0, n), (x1, n), (y0, n), (y1, n)])


def _poly_records(pts):
    """shape records: move to pts[0] with FillStyle1 = 1, straight edges through every point back to the start, end"""
    rec = []; x0, y0 = pts[0]; n = _nb(x0, y0)
    rec += [(0, 1), (0, 1), (0, 1), (1, 1), (0, 1), (1, 1), (n, 5), (x0, n), (y0, n), (1, 1)]   # style change: fill1 + move, 1 fill bit
    cx, cy = x0, y0
    for x, y in pts[1:] + [pts[0]]:
        dx, dy = x - cx, y - cy; cx, cy = x, y
        if not dx and not dy: continue
        n = _nb(dx, dy)
        rec += [(1, 1), (1, 1), (n - 2, 4), (1, 1), (dx, n), (dy, n)]   # straight, general line
    rec += [(0, 1), (0, 5)]
    return gr.wbits(rec)


def bitmap_tag(g, img):
    """DefineBitsLossless2 (premultiplied ARGB) of an RGBA image into Graft g; returns its id"""
    import numpy as np
    h, w, _ = img.shape; a = img[..., 3:4].astype(np.uint16)
    argb = np.concatenate([img[..., 3:4], (img[..., :3].astype(np.uint16) * a // 255).astype(np.uint8)], -1)
    bid = g._alloc(); g.new_defs.append([36, struct.pack('<HBHH', bid, 5, w, h) + zlib.compress(argb.tobytes(), 9)]); g._made[bid] = len(g.new_defs) - 1
    return bid


def bitmap_shape(g, img, centre=False, scale=PX):
    """a DefineShape: a rectangle w*scale x h*scale px filled with the image (clipped, smoothed: fill 0x41). Returns (id, box px)."""
    h, w, _ = img.shape; W, H = w * scale, h * scale
    ox, oy = (-W / 2.0, -H / 2.0) if centre else (0.0, 0.0)
    bid = bitmap_tag(g, img)
    t = [int(round(v * 20)) for v in (ox, oy, ox + W, oy + H)]
    fill = b'\x01\x41' + struct.pack('<H', bid) + gr.matrix_bytes(ox, oy, 20.0 * scale, 20.0 * scale)
    pts = [(t[0], t[1]), (t[2], t[1]), (t[2], t[3]), (t[0], t[3])]
    sid = g._alloc(); g.new_defs.append([2, struct.pack('<H', sid) + _rect_bits(t[0], t[2], t[1], t[3]) + fill + b'\x00' + bytes([0x10]) + _poly_records(pts)])
    g._made[sid] = len(g.new_defs) - 1
    return sid, (ox, oy, ox + W, oy + H)


def poly_shape(g, pts_px, rgb):
    """a DefineShape: one solid polygon (px points). Returns (id, box px)."""
    t = [(int(round(x * 20)), int(round(y * 20))) for x, y in pts_px]
    xs, ys = [p[0] for p in t], [p[1] for p in t]
    fill = b'\x01\x00' + bytes(rgb)
    sid = g._alloc(); g.new_defs.append([2, struct.pack('<H', sid) + _rect_bits(min(xs), max(xs), min(ys), max(ys)) + fill + b'\x00' + bytes([0x10]) + _poly_records(t)])
    g._made[sid] = len(g.new_defs) - 1
    return sid, (min(xs) / 20.0, min(ys) / 20.0, max(xs) / 20.0, max(ys) / 20.0)


# an arrow pointing left, centred near 0,0, about the size of the iPad hot bar arrow (88 x 85 px): our own outline, a flat fill
ARROW_LEFT = [(-44, 0), (-5, -41), (8, -29), (-16, -7.5), (44, -7.5), (44, 7.5), (-16, 7.5), (8, 29), (-5, 41)]


def arrow_shape(g, right=False):
    pts = [(-x, y) for x, y in ARROW_LEFT] if right else ARROW_LEFT
    if right: pts = pts[:1] + pts[1:][::-1]   # keep the winding (fill on the right of the path)
    return poly_shape(g, pts, ARROW_RGB)


def edit_text(g, box, html, font_class='$Title', height=28, align=2):
    """a DefineEditText (read-only, no select, HTML, outlines, font by class name: the engine's font library, as the game's own labels).
    box = (x0, y0, x1, y1) px. Returns its id."""
    t = [int(round(v * 20)) for v in box]
    cid = g._alloc()
    body = struct.pack('<H', cid) + _rect_bits(t[0], t[2], t[1], t[3]) + bytes([0x8c, 0xb3]) + font_class.encode() + b'\0' + struct.pack('<H', height * 20) + \
        b'\xff\xff\xff\xff' + struct.pack('<BHHHh', align, 0, 0, 0, 40) + b'\0' + html.encode() + b'\0'
    g.new_defs.append([37, body]); g._made[cid] = len(g.new_defs) - 1
    return cid


class Art:
    """element factory over one Graft of the target movie. png(name) / sw(...) / arrow(...) return (character id, its box px)."""
    def __init__(self, g, sw=None):
        self.g = g; self.sw = sw; self._png = {}; self._sw = {}

    def png(self, name, centre=False):
        k = (name, centre)
        if k not in self._png: self._png[k] = bitmap_shape(self.g, asset(name), centre)
        return self._png[k]

    def arrow(self, right=False):
        k = ('arrow', right)
        if k not in self._png: self._png[k] = arrow_shape(self.g, right)
        return self._png[k]


def check():
    bad = 0
    for n, (w, h) in sorted(ASSETS.items()):
        p = ASSET_DIR / (n + '.png')
        try:
            img = read_png(p); ok = (img.shape[1], img.shape[0]) == (w, h) and img[..., 3].min() == 0
            print('%-4s %-22s %4dx%-4d sha256 %s' % ('ok' if ok else 'FAIL', n, img.shape[1], img.shape[0], hashlib.sha256(p.read_bytes()).hexdigest()[:16]))
        except Exception as e: ok = False; print('FAIL %-22s %s' % (n, e))
        bad += not ok
    return bad


if __name__ == '__main__':
    if sys.argv[1:] == ['check']: sys.exit(1 if check() else 0)
    sys.exit(__doc__)
