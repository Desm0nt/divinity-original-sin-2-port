"""[DOS2] A small software renderer for SWF characters (static look only), to SEE what the touch UI passes put into a movie.

Draws frame 1 of a sprite / a shape the way the Flash player would, closely enough to compare art side by side: shapes (DefineShape 1..4:
straight and curved edges, fill styles solid / linear / radial / focal gradients / bitmaps clipped or repeated, line styles as plain strokes),
bitmaps (DefineBitsLossless 1 / 2, DefineBits + JPEGTables, DefineBitsJPEG 2 / 3 / 4 with alpha), sprites (their frame-1 display list, matrices,
colour transforms, clip layers as masks). Text fields are drawn as a faint box (fonts live in the engine). No filters / blend modes.
Fills use the even-odd rule over the edges of each fill style, 4x4 supersampled.

  python -I tools/dos2_swf_render.py <movie.swf | pak:inner/path.swf> <id | symbol | root> <out.png> [--scale s] [--frame n] [--bg r,g,b]

Library: render(swf, what, scale=1.0, frame=1) -> (PIL RGBA image, (x0, y0) of its top left in the character's px space).
"""
import io, math, struct, sys, zlib
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf
import dos2_swf_graft as gr

SS = 4   # supersampling per axis


def _rgba(r, ver):
    if ver >= 3: return (r.u8(), r.u8(), r.u8(), r.u8())
    return (r.u8(), r.u8(), r.u8(), 255)


def _fill(r, ver):
    t = r.u8()
    if t == 0x00: return ('solid', _rgba(r, ver))
    if t in (0x10, 0x12, 0x13):
        m = r.matrix(); r.align(); b = r.u8(); n = b & 15; stops = []
        for _ in range(n): stops.append((r.u8(), _rgba(r, ver)))
        foc = 0.0
        if t == 0x13: foc = struct.unpack_from('<h', r.b, r.p)[0] / 256.0; r.p += 2
        return ('lin' if t == 0x10 else 'rad', m, stops, b >> 6 & 3, foc)
    if t in (0x40, 0x41, 0x42, 0x43):
        bid = r.u16(); m = r.matrix()
        return ('bmp', bid, m, t in (0x41, 0x43), t in (0x40, 0x41))   # clipped, smoothed
    raise ValueError('fill %#x' % t)


def _styles(r, ver):
    fills, lines = [], []
    n = r.u8()
    if n == 0xff and ver >= 2: n = r.u16()
    for _ in range(n): fills.append(_fill(r, ver))
    n = r.u8()
    if n == 0xff: n = r.u16()
    for _ in range(n):
        w = r.u16()
        if ver < 4: lines.append((w, ('solid', _rgba(r, ver))))
        else:
            f0 = r.u8(); f1 = r.u8(); join = f0 >> 4 & 3; has_fill = f0 >> 3 & 1
            if join == 2: r.u16()
            lines.append((w, _fill(r, ver) if has_fill else ('solid', _rgba(r, 4))))
    return fills, lines


def parse_shape(d, ver):
    """-> list of (kind 'fill'|'line', style, [segments ((x0, y0), (x1, y1)) in twips])"""
    r = gr.Bits(d, 2); r.rect()
    if ver == 4: r.rect(); r.u8()
    fills, lines = _styles(r, ver)
    nf, nl = r.ub(4), r.ub(4)
    paths = {}   # (kind, index into the global style list) -> segments
    allf, alll = list(fills), list(lines); fb, lb = 0, 0
    x = y = 0; f0 = f1 = ln = 0
    def add(seg):
        for k, idx in (('fill', f0), ('fill', f1)):
            if idx: paths.setdefault(('fill', fb + idx - 1), []).append(seg)
        if ln: paths.setdefault(('line', lb + ln - 1), []).append(seg)
    while True:
        if r.ub(1) == 0:
            flags = r.ub(5)
            if flags == 0: break
            if flags & 1: n = r.ub(5); x, y = r.sb(n), r.sb(n)
            if flags & 2: f0 = r.ub(nf)
            if flags & 4: f1 = r.ub(nf)
            if flags & 8: ln = r.ub(nl)
            if flags & 16:
                fb, lb = len(allf), len(alll)
                nfs, nls = _styles(r, ver); allf += nfs; alll += nls
                nf, nl = r.ub(4), r.ub(4)
        else:
            if r.ub(1):   # straight
                n = r.ub(4) + 2
                if r.ub(1): dx, dy = r.sb(n), r.sb(n)
                else:
                    if r.ub(1): dx, dy = 0, r.sb(n)
                    else: dx, dy = r.sb(n), 0
                add(((x, y), (x + dx, y + dy))); x += dx; y += dy
            else:
                n = r.ub(4) + 2
                cx, cy = x + r.sb(n), y + r.sb(n); ax, ay = cx + r.sb(n), cy + r.sb(n)
                steps = 10; px, py = x, y
                for k in range(1, steps + 1):
                    t = k / float(steps)
                    qx = (1 - t) ** 2 * x + 2 * (1 - t) * t * cx + t * t * ax; qy = (1 - t) ** 2 * y + 2 * (1 - t) * t * cy + t * t * ay
                    add(((px, py), (qx, qy))); px, py = qx, qy
                x, y = ax, ay
    out = []
    for (kind, idx), segs in paths.items():
        out.append((kind, allf[idx] if kind == 'fill' else alll[idx], segs))
    out.sort(key=lambda t: t[0] == 'line')   # fills under lines
    return out


class Ctx:
    def __init__(s, swf, mv=None):
        s.swf = swf; s.mv = mv or gr.Movie(swf); s.bmp = {}; s.jt = None; s.shapes = {}
        for code, d, _ in swf.tags:
            if code == 8: s.jt = d

    def bitmap(s, bid):
        """-> float32 HxWx4 premultiplied RGBA 0..1"""
        if bid in s.bmp: return s.bmp[bid]
        from PIL import Image
        code, d, _ = s.swf.tags[s.mv.defs[bid]]
        if code in (20, 36):
            fmt, w, h = d[2], struct.unpack_from('<H', d, 3)[0], struct.unpack_from('<H', d, 5)[0]
            if fmt == 5:
                raw = np.frombuffer(zlib.decompress(d[7:]), np.uint8)[:w * h * 4].reshape(h, w, 4).astype(np.float32) / 255.0
                a, rgb = raw[..., 0:1], raw[..., 1:4]
                if code == 20: a = np.ones_like(a)
                img = np.concatenate([rgb, a], -1)
            elif fmt == 3:
                n = d[7] + 1; raw = zlib.decompress(d[8:]); cs = 4 if code == 36 else 3
                pal = np.frombuffer(raw[:n * cs], np.uint8).reshape(n, cs).astype(np.float32) / 255.0
                pitch = (w + 3) & ~3; idx = np.frombuffer(raw[n * cs:], np.uint8)[:pitch * h].reshape(h, pitch)[:, :w]
                p = pal[idx]
                img = np.concatenate([p[..., :3] * (p[..., 3:4] if cs == 4 else 1), p[..., 3:4] if cs == 4 else np.ones_like(p[..., :1])], -1)
            else: raise ValueError('lossless format %d' % fmt)
        else:
            if code == 6: data = (s.jt[:-2] + d[4:]) if s.jt and len(s.jt) > 4 else d[2:]
            elif code == 21: data = d[2:]
            else:
                off = struct.unpack_from('<I', d, 2)[0]; p0 = 6 + (2 if code == 90 else 0); data = d[p0:p0 + off]; adata = d[p0 + off:]
            if data[:4] == bytes((0xff, 0xd9, 0xff, 0xd8)): data = data[4:]
            im = Image.open(io.BytesIO(data)).convert('RGBA'); arr = np.asarray(im).astype(np.float32) / 255.0
            if code in (35, 90) and adata and data[:2] == b'\xff\xd8':
                a = np.frombuffer(zlib.decompress(adata), np.uint8)[:arr.shape[0] * arr.shape[1]].reshape(arr.shape[:2]).astype(np.float32) / 255.0
                arr = np.concatenate([arr[..., :3] * a[..., None], a[..., None]], -1)   # JPEG3 colour is premultiplied by Flash convention
                arr[..., :3] = np.minimum(arr[..., :3], a[..., None])
            else: arr = np.concatenate([arr[..., :3] * arr[..., 3:4], arr[..., 3:4]], -1)
            img = arr
        s.bmp[bid] = img
        return img


def _inv(m):
    a, b, c, d, tx, ty = m; det = a * d - b * c
    if abs(det) < 1e-12: return None
    ia, ib, ic, id_ = d / det, -b / det, -c / det, a / det
    return (ia, ib, ic, id_, -(ia * tx + ic * ty), -(ib * tx + id_ * ty))


def _mul(p, q):
    """p after q: x' = p(q(x)); matrices (a, b, c, d, tx, ty) with x' = a x + c y + tx, y' = b x + d y + ty"""
    a, b, c, d, tx, ty = q; A, B, C, D, TX, TY = p
    return (A * a + C * b, B * a + D * b, A * c + C * d, B * c + D * d, A * tx + C * ty + TX, B * tx + D * ty + TY)


def _cover(segs, m, W, H):
    """even-odd coverage (H*SS x W*SS bool) of the edge list (twips) under matrix m (twips -> canvas px)"""
    if not segs: return None
    P = np.array(segs, np.float64).reshape(-1, 4) / 20.0
    a, b, c, d, tx, ty = m
    x0 = (a * P[:, 0] + c * P[:, 1] + tx) * SS; y0 = (b * P[:, 0] + d * P[:, 1] + ty) * SS
    x1 = (a * P[:, 2] + c * P[:, 3] + tx) * SS; y1 = (b * P[:, 2] + d * P[:, 3] + ty) * SS
    Hs, Ws = H * SS, W * SS
    mask = np.zeros((Hs, Ws), np.bool_)
    keep = y0 != y1
    x0, y0, x1, y1 = x0[keep], y0[keep], x1[keep], y1[keep]
    if not len(x0): return None
    ylo, yhi = np.minimum(y0, y1), np.maximum(y0, y1)
    rows = np.arange(Hs) + 0.5
    acc = np.zeros((Hs, Ws + 1), np.int32)
    for k in range(len(x0)):
        r0 = max(0, int(math.ceil(ylo[k] - 0.5))); r1 = min(Hs - 1, int(math.ceil(yhi[k] - 0.5)) - 1)
        if r1 < r0: continue
        yy = rows[r0:r1 + 1]
        xx = x0[k] + (yy - y0[k]) * (x1[k] - x0[k]) / (y1[k] - y0[k])
        xi = np.clip(np.ceil(xx - 0.5).astype(np.int64), 0, Ws)
        np.add.at(acc, (np.arange(r0, r1 + 1), xi), 1)
    return (np.cumsum(acc, 1)[:, :Ws] & 1).astype(np.bool_)


def _down(mask):
    H, W = mask.shape[0] // SS, mask.shape[1] // SS
    return mask.reshape(H, SS, W, SS).mean((1, 3), dtype=np.float32)


def _paint(ctx, style, m, W, H):
    """colour of a fill style at each canvas px: float32 HxWx4 premultiplied"""
    kind = style[0]
    if kind == 'solid':
        r, g, b, a = [v / 255.0 for v in style[1]]
        out = np.empty((H, W, 4), np.float32); out[...] = (r * a, g * a, b * a, a); return out
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float64) + 0.5
    if kind == 'bmp':
        _, bid, bm, clip, smooth = style
        if bid == 0xffff or bid not in ctx.mv.defs: return np.zeros((H, W, 4), np.float32)
        img = ctx.bitmap(bid); bh, bw = img.shape[:2]
        full = _mul(m, (bm[0], bm[1], bm[2], bm[3], bm[4], bm[5]))   # bitmap px -> shape twips (bm) -> canvas px (m: twips -> canvas)
        inv = _inv(full)
        if inv is None: return np.zeros((H, W, 4), np.float32)
        ia, ib, ic, id_, itx, ity = inv
        u = ia * xs + ic * ys + itx; v = ib * xs + id_ * ys + ity
        if clip: u = np.clip(u, 0, bw - 1e-3); v = np.clip(v, 0, bh - 1e-3)
        else: u = np.mod(u, bw); v = np.mod(v, bh)
        if smooth:
            u0 = np.clip(np.floor(u - 0.5), 0, bw - 1).astype(np.int64); v0 = np.clip(np.floor(v - 0.5), 0, bh - 1).astype(np.int64)
            u1 = np.minimum(u0 + 1, bw - 1); v1 = np.minimum(v0 + 1, bh - 1)
            fu = np.clip(u - 0.5 - u0, 0, 1)[..., None].astype(np.float32); fv = np.clip(v - 0.5 - v0, 0, 1)[..., None].astype(np.float32)
            return (img[v0, u0] * (1 - fu) * (1 - fv) + img[v0, u1] * fu * (1 - fv) + img[v1, u0] * (1 - fu) * fv + img[v1, u1] * fu * fv).astype(np.float32)
        return img[v.astype(np.int64), u.astype(np.int64)]
    _, gm, stops, spread, foc = style
    full = _mul(m, gm); inv = _inv(full)
    if inv is None or not stops: return np.zeros((H, W, 4), np.float32)
    ia, ib, ic, id_, itx, ity = inv
    u = (ia * xs + ic * ys + itx) / 16384.0; v = (ib * xs + id_ * ys + ity) / 16384.0
    t = (u + 1) / 2.0 if kind == 'lin' else np.sqrt(u * u + v * v)
    t = np.clip(t, 0, 1) * 255.0
    pos = np.array([p for p, _ in stops], np.float64); cols = np.array([[c[0] / 255.0 * c[3] / 255.0, c[1] / 255.0 * c[3] / 255.0, c[2] / 255.0 * c[3] / 255.0, c[3] / 255.0] for _, c in stops])
    out = np.stack([np.interp(t, pos, cols[:, k]) for k in range(4)], -1)
    return out.astype(np.float32)


def _stroke(segs, width_twips, m, W, H):
    from PIL import Image, ImageDraw
    sc = math.sqrt(abs(m[0] * m[3] - m[1] * m[2])) or 1.0
    w = max(1.0, width_twips / 20.0 * sc) * SS
    im = Image.new('L', (W * SS, H * SS), 0); dr = ImageDraw.Draw(im)
    a, b, c, d, tx, ty = m
    for (p, q) in segs:
        X0 = (a * p[0] / 20.0 + c * p[1] / 20.0 + tx) * SS; Y0 = (b * p[0] / 20.0 + d * p[1] / 20.0 + ty) * SS
        X1 = (a * q[0] / 20.0 + c * q[1] / 20.0 + tx) * SS; Y1 = (b * q[0] / 20.0 + d * q[1] / 20.0 + ty) * SS
        dr.line([(X0, Y0), (X1, Y1)], fill=255, width=int(round(w)))
        rr = w / 2.0; dr.ellipse([X1 - rr, Y1 - rr, X1 + rr, Y1 + rr], fill=255)
    return np.asarray(im) > 127


def _cx(rec):
    """colour transform of a placement -> (mult rgba, add rgba) (add in 0..1)"""
    if not rec.get('cx'): return None
    r = gr.Bits(rec['cx'], 0); ha = r.ub(1); hm = r.ub(1); n = r.ub(4)
    mul = [1.0] * 4; add = [0.0] * 4
    if hm: mul = [r.sb(n) / 256.0 for _ in range(4)]
    if ha: add = [r.sb(n) / 255.0 for _ in range(4)]
    return mul, add


def _over(dst, src):
    dst *= (1 - src[..., 3:4]); dst += src


def draw(ctx, cid, m, canvas, cx=None, frame=1, depth=0):
    """composite character cid with matrix m (char px -> canvas px) onto canvas (HxWx4 premultiplied)"""
    if depth > 40 or cid not in ctx.mv.defs: return
    H, W = canvas.shape[:2]
    code, d, _ = ctx.swf.tags[ctx.mv.defs[cid]]
    if code in gr.T_SHAPES:
        if cid not in ctx.shapes: ctx.shapes[cid] = parse_shape(d, gr.T_SHAPES[code])
        mt = (m[0], m[1], m[2], m[3], m[4], m[5])   # px -> canvas; shape coords twips: _cover divides by 20
        for kind, style, segs in ctx.shapes[cid]:
            if kind == 'fill':
                cov = _cover(segs, mt, W, H)
                if cov is None: continue
                cov = _down(cov)
                st = style
                if st[0] == 'bmp': pm = (m[0] / 20.0, m[1] / 20.0, m[2] / 20.0, m[3] / 20.0, m[4], m[5])   # twips -> canvas
                else: pm = (m[0] / 20.0, m[1] / 20.0, m[2] / 20.0, m[3] / 20.0, m[4], m[5])
                col = _paint(ctx, st, pm, W, H)
            else:
                w, st = style
                if w == 0: continue
                cov = _stroke(segs, w, mt, W, H); cov = _down(cov)
                col = _paint(ctx, st, (m[0] / 20.0, m[1] / 20.0, m[2] / 20.0, m[3] / 20.0, m[4], m[5]), W, H)
            src = col * cov[..., None]
            if cx: src = _apply_cx(src, cx)
            _over(canvas, src)
        return
    if code in gr.T_BITMAPS:
        img = ctx.bitmap(cid); bh, bw = img.shape[:2]
        _over(canvas, _paint(ctx, ('bmp', cid, (20.0, 0, 0, 20.0, 0, 0), True, True), (m[0] / 20.0, m[1] / 20.0, m[2] / 20.0, m[3] / 20.0, m[4], m[5]), W, H)
              * _down(_cover([((0, 0), (bw * 20, 0)), ((bw * 20, 0), (bw * 20, bh * 20)), ((bw * 20, bh * 20), (0, bh * 20)), ((0, bh * 20), (0, 0))], m, W, H))[..., None])
        return
    if code == gr.T_EDIT or code in gr.T_TEXTS:
        x0, x1, y0, y1 = gr.Bits(d, 2).rect()
        box = [((x0, y0), (x1, y0)), ((x1, y0), (x1, y1)), ((x1, y1), (x0, y1)), ((x0, y1), (x0, y0))]
        cov = _cover(box, m, W, H)
        if cov is not None: _over(canvas, (np.array([0.25, 0.25, 0.3, 0.25], np.float32) * _down(cov)[..., None]).astype(np.float32))
        return
    if code == gr.T_SPRITE:
        dl = gr.frame_state(d, frame if depth == 0 else 1)
        clips = []   # (until depth, mask)
        for dep in sorted(dl):
            rec = dl[dep]
            if rec['char'] is None: continue
            mm = gr.Bits(rec['matrix'], 0).matrix() if rec['matrix'] else [1.0, 0.0, 0.0, 1.0, 0, 0]
            child = _mul(m, (mm[0], mm[1], mm[2], mm[3], mm[4] / 20.0, mm[5] / 20.0))
            ccx = _cx(rec); ccx = _comb(cx, ccx)
            if rec['clip'] is not None:
                layer = np.zeros_like(canvas); draw(ctx, rec['char'], child, layer, None, 1, depth + 1)
                clips.append((struct.unpack_from('<H', rec['clip'], 0)[0], layer[..., 3:4].copy())); continue
            clips = [c for c in clips if c[0] >= dep]
            if clips:
                layer = np.zeros_like(canvas); draw(ctx, rec['char'], child, layer, ccx, 1, depth + 1)
                for _, mk in clips: layer *= np.clip(mk, 0, 1)
                _over(canvas, layer)
            else: draw(ctx, rec['char'], child, canvas, ccx, 1, depth + 1)


def _comb(a, b):
    if a is None: return b
    if b is None: return a
    return [x * y for x, y in zip(a[0], b[0])], [a[1][k] + b[1][k] * a[0][k] for k in range(4)]


def _apply_cx(src, cx):
    mul, add = cx
    a = src[..., 3:4]
    rgb = np.divide(src[..., :3], a, out=np.zeros_like(src[..., :3]), where=a > 1e-6)
    rgb = np.clip(rgb * np.array(mul[:3], np.float32) + np.array(add[:3], np.float32), 0, 1)
    na = np.clip(a * mul[3] + add[3] * (a > 0), 0, 1)
    return np.concatenate([rgb * na, na], -1).astype(np.float32)


def render(swf, what, scale=1.0, frame=1, box=None, pad=2):
    """-> (PIL image, (x0, y0) px of its top left in the character's own space)"""
    from PIL import Image
    mv = gr.Movie(swf)
    if what == 'root':
        b = gr.Bits(swf.body(), 0); n = b.ub(5); r = [b.sb(n) / 20.0 for _ in range(4)]
        box = box or (r[0], r[2], r[1], r[3]); ctx = Ctx(swf, mv)
        W, H = int(math.ceil((box[2] - box[0]) * scale)), int(math.ceil((box[3] - box[1]) * scale))
        canvas = np.zeros((H, W, 4), np.float32)
        dl = {}; n = 1
        for code, d, _ in swf.tags:
            if code == gr.T_SHOW:
                if n >= frame: break
                n += 1
            elif code in (gr.T_PLACE2, gr.T_PLACE3):
                rec = gr.place_parse(code, d)
                if rec['char'] is not None: dl[rec['depth']] = rec
        fake = b''
        for dep in sorted(dl):
            rec = dl[dep]; mm = gr.Bits(rec['matrix'], 0).matrix() if rec['matrix'] else [1.0, 0.0, 0.0, 1.0, 0, 0]
            draw(ctx, rec['char'], (scale * mm[0], scale * mm[1], scale * mm[2], scale * mm[3], scale * (mm[4] / 20.0 - box[0]), scale * (mm[5] / 20.0 - box[1])), canvas, _cx(rec), 1, 1)
        return _img(canvas), (box[0], box[1])
    cid = mv.resolve(what)
    b = box or gr.bounds(swf, cid, frame, mv)
    if not b and cid in mv.defs and swf.tags[mv.defs[cid]][0] in gr.T_BITMAPS:
        im = Ctx(swf, mv).bitmap(cid); b = (0, 0, im.shape[1], im.shape[0])
    b = b or (0, 0, 1, 1)
    b = (b[0] - pad, b[1] - pad, b[2] + pad, b[3] + pad)
    W, H = max(1, int(math.ceil((b[2] - b[0]) * scale))), max(1, int(math.ceil((b[3] - b[1]) * scale)))
    canvas = np.zeros((H, W, 4), np.float32)
    draw(Ctx(swf, mv), cid, (scale, 0, 0, scale, -b[0] * scale, -b[1] * scale), canvas, None, frame)
    return _img(canvas), (b[0], b[1])


def _img(canvas):
    from PIL import Image
    a = canvas[..., 3:4]
    rgb = np.divide(canvas[..., :3], a, out=np.zeros_like(canvas[..., :3]), where=a > 1e-6)
    out = np.concatenate([np.clip(rgb, 0, 1), np.clip(a, 0, 1)], -1)
    return Image.fromarray((out * 255 + 0.5).astype(np.uint8), 'RGBA')


def load(path):
    """a .swf file, or <pak>:<inner path> / <pak>:<name in Public/Game/GUI/>"""
    if ':' in path[2:] and not Path(path).exists():
        k = path.rindex(':'); pak, inner = path[:k], path[k + 1:]
        import lspk
        p = lspk.Pak(pak); want = inner if '/' in inner else 'Public/Game/GUI/' + inner
        e = [e for e in p.entries if e[0] == want]
        if not e: sys.exit('%s: no %s' % (pak, want))
        return Swf(p.read(e[0]))
    return Swf(open(path, 'rb').read())


def main():
    a = sys.argv[1:]
    if len(a) < 3: sys.exit(__doc__)
    opt = lambda k, d: a[a.index(k) + 1] if k in a else d
    swf = load(a[0]); img, org = render(swf, a[1], float(opt('--scale', 1)), int(opt('--frame', 1)))
    bg = opt('--bg', None)
    if bg:
        from PIL import Image
        base = Image.new('RGBA', img.size, tuple(int(v) for v in bg.split(',')) + (255,)); base.alpha_composite(img); img = base
    img.save(a[2]); print('%s: %dx%d, top left %.1f,%.1f' % (a[2], img.size[0], img.size[1], org[0], org[1]))


if __name__ == '__main__':
    main()
