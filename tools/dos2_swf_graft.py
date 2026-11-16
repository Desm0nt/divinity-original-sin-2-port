"""[DOS2] SWF tag-level graft: copy a symbol tree (a sprite / shape / text with everything it draws with) from one movie into another.

Owner rule 15.11: where the iPad has an element to tap and the console movie has none, the element is drawn into the console movie itself
(docs/dos2/TOUCH_VISIBLE.md). This tool moves the VISUALS: the definition tags of a character and of everything it depends on
(DefineSprite -> the characters its timeline places; DefineShape 1..4 / DefineMorphShape 1..2 -> the bitmaps of its fill and line styles;
DefineEditText / DefineText 1..2 -> fonts; DefineButton2 -> its state characters; ImportAssets2 entries the tree uses; plus the tags that
hang on a copied character: CSMTextSettings, DefineScalingGrid, DefineFontAlignZones, DefineFontName), with every character id remapped to
ids the target does not use. Nothing of the target is rewritten: its own tags stay byte for byte and in order, the new tags are only
INSERTED (definitions before the first root control tag, root PlaceObject2 tags before the first ShowFrame of the root timeline).

The AS3 classes of the source are NOT copied (an iPad class needs the iPad's class library and its engine class): a grafted sprite is a plain
MovieClip. Because of that a multi-frame sprite would loop its frames (the class's stop() is gone), so by default every copied sprite is
FROZEN: its timeline is cut to the display list of one frame (frame 1, or `frames={source id: n}`), 1 frame long. The target's ABC wires the
click: a placed instance is found with getChildByName(name) (tools/dos2_hud_touch.py is the first user).

Library:
    g = Graft(dst: dos2_abc.Swf, src: dos2_abc.Swf)
    new_id = g.copy(src_id_or_symbol_name, frames={}, freeze=True)   # the tree; returns the id in the target
    new_id = g.sprite([(char id in the target, depth, (x, y) or matrix bytes, name or None), ...])   # a new 1-frame sprite made of placed characters
    g.place(new_id, x, y, name, scale=1.0)                            # root timeline, frame 1, on top (depth = highest used + 1)
    out = g.finish()                                                  # the tags are inserted; dst.write() gives the movie
    bounds(swf, id, frame=1) -> (xmin, ymin, xmax, ymax) in px        # of a shape / sprite frame (sprites: union of the placed characters)
    check(before: bytes, after: bytes) -> list of problems            # original tags kept + order, ids unique, every use after its definition

  python -I tools/dos2_swf_graft.py list <movie.swf>                               characters: id, kind, symbol class, frames, dependencies
  python -I tools/dos2_swf_graft.py tree <movie.swf> <id | symbol> [frame]         the placement tree of a sprite (depth, id, name, x, y)
  python -I tools/dos2_swf_graft.py graft <src.swf> <id | symbol>[@frame] <dst.swf> <out.swf> [--place x,y[,name]] [--keep-frames]
  python -I tools/dos2_swf_graft.py selftest <src.swf> <id | symbol> <dst.swf>     graft, re-read, compare the tree copied back with the source
  python -I tools/dos2_swf_graft.py png <movie.swf> <id> <out.png>                 a DefineBitsLossless 1/2 bitmap as PNG (to look at an icon)
"""
import struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dos2_abc

T_END, T_SHOW, T_PLACE, T_REMOVE, T_PLACE2, T_REMOVE2, T_PLACE3, T_SPRITE, T_LABEL = 0, 1, 4, 5, 26, 28, 70, 39, 43
T_SHAPES = {2: 1, 22: 2, 32: 3, 83: 4}
T_MORPHS = {46: 1, 84: 2}
T_BITMAPS = (6, 21, 35, 90, 20, 36)
T_FONTS = (10, 48, 75, 91)
T_TEXTS = {11: 1, 33: 2}
T_EDIT, T_BUTTON2, T_IMPORT2, T_SYMCLASS, T_ABC, T_CSM, T_GRID, T_ZONES, T_FONTNAME = 37, 34, 71, 76, 82, 74, 78, 73, 88
T_OTHERDEF = (14, 60, 87)   # sound, video stream, binary data: id first, no dependencies
DEFS = set(T_SHAPES) | set(T_MORPHS) | set(T_BITMAPS) | set(T_FONTS) | set(T_TEXTS) | {T_EDIT, T_BUTTON2, T_SPRITE} | set(T_OTHERDEF)
HANG = (T_CSM, T_GRID, T_ZONES, T_FONTNAME)   # tags about a character: its id first
KIND = {39: 'sprite', 37: 'edittext', 34: 'button2', 71: 'import'}
for _k in T_SHAPES: KIND[_k] = 'shape%d' % T_SHAPES[_k]
for _k in T_MORPHS: KIND[_k] = 'morph%d' % T_MORPHS[_k]
for _k in T_BITMAPS: KIND[_k] = 'bitmap'
for _k in T_FONTS: KIND[_k] = 'font'
for _k in T_TEXTS: KIND[_k] = 'text%d' % T_TEXTS[_k]


class Bits:
    """MSB-first bit reader over bytes (SWF RECT / MATRIX / shape records)"""
    def __init__(s, b, p=0): s.b = b; s.p = p; s.bit = 0
    def ub(s, n):
        v = 0
        for _ in range(n):
            v = v << 1 | (s.b[s.p] >> (7 - s.bit)) & 1; s.bit += 1
            if s.bit == 8: s.bit = 0; s.p += 1
        return v
    def sb(s, n):
        v = s.ub(n)
        return v - (1 << n) if n and v >> (n - 1) else v
    def align(s):
        if s.bit: s.bit = 0; s.p += 1
    def u8(s): s.align(); v = s.b[s.p]; s.p += 1; return v
    def u16(s): s.align(); v = struct.unpack_from('<H', s.b, s.p)[0]; s.p += 2; return v
    def rect(s):
        s.align(); n = s.ub(5); v = [s.sb(n) for _ in range(4)]; s.align(); return v   # xmin, xmax, ymin, ymax (twips)
    def matrix(s):
        s.align(); m = [1.0, 0.0, 0.0, 1.0, 0, 0]
        if s.ub(1): n = s.ub(5); m[0] = s.sb(n) / 65536.0; m[3] = s.sb(n) / 65536.0
        if s.ub(1): n = s.ub(5); m[1] = s.sb(n) / 65536.0; m[2] = s.sb(n) / 65536.0   # rotate skew 0 (b), rotate skew 1 (c)
        n = s.ub(5); m[4] = s.sb(n); m[5] = s.sb(n); s.align(); return m           # a, b, c, d, tx, ty (twips)
    def cxform(s, alpha=True):
        s.align(); ha = s.ub(1); hm = s.ub(1); n = s.ub(4)
        for _ in range((ha + hm) * (4 if alpha else 3)): s.sb(n)
        s.align()


def wbits(fields):
    """[(value, bit count)] -> bytes, MSB first, zero padded"""
    acc = 0; n = 0
    for v, k in fields: acc = acc << k | (v & ((1 << k) - 1)); n += k
    pad = -n % 8
    return (acc << pad).to_bytes((n + pad) // 8, 'big')


def matrix_bytes(x, y, sx=1.0, sy=1.0):
    """a translate (px) + optional scale MATRIX"""
    f = []
    if sx != 1.0 or sy != 1.0:
        a, d = int(round(sx * 65536)), int(round(sy * 65536)); n = max(a.bit_length(), d.bit_length()) + 1
        f += [(1, 1), (n, 5), (a, n), (d, n)]
    else: f.append((0, 1))
    f.append((0, 1))
    tx, ty = int(round(x * 20)), int(round(y * 20)); n = max(abs(tx).bit_length(), abs(ty).bit_length()) + 1
    f += [(n, 5), (tx, n), (ty, n)]
    return wbits(f)


def cstr(b, p):
    e = b.index(b'\0', p); return b[p:e], e + 1


def subtags(d, p=4):
    """the tags of a DefineSprite payload: [(code, payload)]"""
    out = []
    while p < len(d):
        h = struct.unpack_from('<H', d, p)[0]; p += 2; code, ln = h >> 6, h & 0x3f
        if ln == 0x3f: ln = struct.unpack_from('<I', d, p)[0]; p += 4
        out.append((code, d[p:p + ln])); p += ln
        if code == T_END: break
    return out


def tag_bytes(code, d, force_long=False):
    if force_long or len(d) >= 0x3f: return struct.pack('<HI', code << 6 | 0x3f, len(d)) + d
    return struct.pack('<H', code << 6 | len(d)) + d


# ---------------------------------------------------------------- where the character ids of a tag are (byte offsets of u16 fields)
def _fill_style(r, ver, out, morph=False):
    t = r.u8()
    if t == 0x00: r.p += (8 if morph else 4 if ver >= 3 else 3)
    elif t in (0x10, 0x12, 0x13):
        r.matrix()
        if morph:
            r.matrix(); n = r.u8() & 15; r.p += n * 10
        else:
            n = r.u8() & 15; r.p += n * (5 if ver >= 3 else 4)
            if t == 0x13: r.p += 2
    elif t in (0x40, 0x41, 0x42, 0x43):
        out.append(r.p); r.p += 2; r.matrix()
        if morph: r.matrix()
    else: raise ValueError('fill style type %#x' % t)


def _styles(r, ver, out):
    n = r.u8()
    if n == 0xff and ver >= 2: n = r.u16()
    for _ in range(n): _fill_style(r, ver, out)
    n = r.u8()
    if n == 0xff: n = r.u16()
    for _ in range(n):
        if ver < 4: r.p += 2 + (4 if ver >= 3 else 3)
        else:
            r.p += 2; fl0 = r.b[r.p]; r.p += 2; join = fl0 >> 4 & 3; has_fill = fl0 >> 3 & 1   # LINESTYLE2: width, then StartCap 2 | Join 2 | HasFill 1 | ... (first byte), 2 flag bytes
            if join == 2: r.p += 2
            if has_fill: _fill_style(r, ver, out)
            else: r.p += 4


def shape_ids(d, ver):
    """offsets of the bitmap ids of a DefineShape<ver> payload (fill styles, line fills, styles of StateNewStyles records)"""
    out = []; r = Bits(d, 2); r.rect()
    if ver == 4: r.rect(); r.p += 1
    _styles(r, ver, out)
    r.align(); fb = r.ub(4); lb = r.ub(4)
    while True:
        if r.ub(1):   # edge
            if r.ub(1):   # straight
                n = r.ub(4) + 2
                if r.ub(1): r.sb(n); r.sb(n)
                else: r.ub(1); r.sb(n)
            else: n = r.ub(4) + 2; [r.sb(n) for _ in range(4)]
            continue
        fl = r.ub(5)
        if not fl: break
        if fl & 1: n = r.ub(5); r.sb(n); r.sb(n)
        if fl & 2: r.ub(fb)
        if fl & 4: r.ub(fb)
        if fl & 8: r.ub(lb)
        if fl & 16:
            r.align(); _styles(r, ver, out); r.align(); fb = r.ub(4); lb = r.ub(4)
    return out


def morph_ids(d, ver):
    out = []; r = Bits(d, 2); r.rect(); r.rect()
    if ver == 2: r.rect(); r.rect(); r.p += 1
    r.p += 4
    n = r.u8()
    if n == 0xff: n = r.u16()
    for _ in range(n): _fill_style(r, 3, out, morph=True)
    n = r.u8()
    if n == 0xff: n = r.u16()
    for _ in range(n):
        if ver == 1: r.p += 4 + 8
        else:
            r.p += 4; fl0 = r.b[r.p]; r.p += 2; join = fl0 >> 4 & 3; has_fill = fl0 >> 3 & 1
            if join == 2: r.p += 2
            if has_fill: _fill_style(r, 3, out, morph=True)
            else: r.p += 8
    return out


def text_ids(d, ver):
    out = []; r = Bits(d, 2); r.rect(); r.matrix(); gb = r.u8(); ab = r.u8()
    while True:
        fl = r.u8()
        if not fl: break
        if fl & 8: out.append(r.p); r.p += 2
        if fl & 4: r.p += 4 if ver == 2 else 3
        if fl & 1: r.p += 2
        if fl & 2: r.p += 2
        if fl & 8: r.p += 2
        n = r.u8(); r.ub((gb + ab) * n); r.align()
    return out


def place_ids(code, d):
    """offsets of the character id of a PlaceObject / 2 / 3 / RemoveObject payload"""
    if code in (T_PLACE, T_REMOVE): return [0]
    if code == T_PLACE2: return [3] if d[0] & 2 else []
    if code == T_PLACE3:
        p = 4
        if d[1] & 8: p = cstr(d, p)[1]   # class name: only with HasClassName (HasImage + HasCharacter carries none in these movies: `06 10 depth id matrix`)
        return [p] if d[0] & 2 else []
    return []


def ids_of(code, d):
    """(offset of the tag's own id or None, offsets of the ids it USES). A sprite's uses are returned as (sub tag index, offset) pairs by sprite_uses."""
    if code in T_SHAPES: return 0, shape_ids(d, T_SHAPES[code])
    if code in T_MORPHS: return 0, morph_ids(d, T_MORPHS[code])
    if code in T_TEXTS: return 0, text_ids(d, T_TEXTS[code])
    if code == T_EDIT:
        r = Bits(d, 2); r.rect(); f = r.b[r.p]
        return 0, ([r.p + 2] if f & 1 else [])
    if code == T_BUTTON2:
        out = []; r = Bits(d, 5)
        while True:
            fl = r.u8()
            if not fl: break
            if fl & 0x10: raise ValueError('DefineButton2 with a filter list: not handled')
            out.append(r.p); r.p += 4; r.matrix(); r.cxform()
            if fl & 0x20: r.p += 1
        return 0, out
    if code in DEFS or code in HANG: return 0, []
    return None, []


def sprite_uses(d):
    """[(position of the u16 in the sprite payload)] of every character id its timeline tags use"""
    out = []; p = 4
    while p < len(d):
        h = struct.unpack_from('<H', d, p)[0]; p += 2; code, ln = h >> 6, h & 0x3f
        if ln == 0x3f: ln = struct.unpack_from('<I', d, p)[0]; p += 4
        out += [p + o for o in place_ids(code, d[p:p + ln])]
        p += ln
        if code == T_END: break
    return out


# ---------------------------------------------------------------- a movie's characters
class Movie:
    def __init__(s, swf):
        s.swf = swf; s.defs = {}; s.hang = {}; s.imports = {}; s.classes = {}
        for n, (code, d, _) in enumerate(swf.tags):
            if code in DEFS: s.defs[struct.unpack_from('<H', d, 0)[0]] = n
            elif code in HANG: s.hang.setdefault(struct.unpack_from('<H', d, 0)[0], []).append(n)
            elif code == T_IMPORT2:
                url, p = cstr(d, 0); p += 2; cnt = struct.unpack_from('<H', d, p)[0]; p += 2
                for _ in range(cnt):
                    cid = struct.unpack_from('<H', d, p)[0]; name, p = cstr(d, p + 2); s.imports[cid] = (url, name)
        for name, cid in swf.symbols().items(): s.classes.setdefault(cid, name)
        s.byname = {v: k for k, v in s.classes.items()}

    def resolve(s, what):
        if isinstance(what, int): return what
        if what.isdigit(): return int(what)
        hit = [c for c, n in s.classes.items() if n == what or n.split('.')[-1] == what]
        if len(hit) != 1: raise KeyError('%d symbols named %s' % (len(hit), what))
        return hit[0]

    def uses(s, cid):
        if cid in s.imports or cid not in s.defs: return []
        code, d, _ = s.swf.tags[s.defs[cid]]
        if code == T_SPRITE: return [struct.unpack_from('<H', d, o)[0] for o in sprite_uses(d)]
        return [struct.unpack_from('<H', d, o)[0] for o in ids_of(code, d)[1]]

    def tree(s, cid):
        """every character the tree of cid needs, dependencies first, cid last"""
        out = []; seen = set()
        def walk(c):
            if c in seen or c == 0xffff: return
            seen.add(c)
            for u in s.uses(c): walk(u)
            out.append(c)
        walk(cid)
        return out

    def used_ids(s):
        out = set(s.defs) | set(s.imports) | set(s.classes)
        return out

    def root_depths(s):
        out = set()
        for code, d, _ in s.swf.tags:
            if code == T_PLACE: out.add(struct.unpack_from('<H', d, 2)[0])
            elif code == T_PLACE2: out.add(struct.unpack_from('<H', d, 1)[0])
            elif code == T_PLACE3: out.add(struct.unpack_from('<H', d, 2)[0])
        return out


# ---------------------------------------------------------------- PlaceObject2 / 3 as a record (to cut a sprite to one frame)
def place_parse(code, d):
    """-> dict(depth, char, matrix, cx, ratio, name, clip, rest3) with raw bytes for matrix / cx; None for fields not present"""
    if code == T_PLACE:
        r = Bits(d, 4); r.matrix(); return dict(depth=struct.unpack_from('<H', d, 2)[0], char=struct.unpack_from('<H', d, 0)[0], matrix=d[4:r.p], cx=None, ratio=None,
                                                 name=None, clip=None, cls=None, f2=0, rest3=b'', move=False, v3=False, actions=None)
    f = d[0]; v3 = code == T_PLACE3; f2 = d[1] if v3 else 0; p = 2 if v3 else 1
    rec = dict(depth=struct.unpack_from('<H', d, p)[0], char=None, matrix=None, cx=None, ratio=None, name=None, clip=None, cls=None, f2=f2, rest3=b'', move=bool(f & 1), v3=v3, actions=None)
    p += 2
    if v3 and f2 & 8: e = cstr(d, p)[1]; rec['cls'] = d[p:e]; p = e
    if f & 2: rec['char'] = struct.unpack_from('<H', d, p)[0]; p += 2
    if f & 4: r = Bits(d, p); r.matrix(); rec['matrix'] = d[p:r.p]; p = r.p
    if f & 8: r = Bits(d, p); r.cxform(); rec['cx'] = d[p:r.p]; p = r.p
    if f & 16: rec['ratio'] = d[p:p + 2]; p += 2
    if f & 32: e = cstr(d, p)[1]; rec['name'] = d[p:e]; p = e
    if f & 64: rec['clip'] = d[p:p + 2]; p += 2
    if f & 128: rec['actions'] = d[p:]; p = len(d)   # clip actions (AS1/2 only): kept raw, last
    else: rec['rest3'] = d[p:]                        # PlaceObject3: filter list, blend mode, cache as bitmap, visible, background
    return rec


def place_build(rec):
    """a full (non-move) PlaceObject2 / 3 payload from a record"""
    f = (2 if rec['char'] is not None else 0) | (4 if rec['matrix'] is not None else 0) | (8 if rec['cx'] is not None else 0) | (16 if rec['ratio'] is not None else 0) | \
        (32 if rec['name'] is not None else 0) | (64 if rec['clip'] is not None else 0) | (128 if rec['actions'] is not None else 0)
    o = bytes([f]) + (bytes([rec['f2']]) if rec['v3'] else b'') + struct.pack('<H', rec['depth'])
    if rec['v3'] and rec['cls'] is not None: o += rec['cls']
    if rec['char'] is not None: o += struct.pack('<H', rec['char'])
    for k in ('matrix', 'cx', 'ratio', 'name', 'clip'):
        if rec[k] is not None: o += rec[k]
    o += rec['actions'] if rec['actions'] is not None else rec['rest3']
    return (T_PLACE3 if rec['v3'] else T_PLACE2), o


def frame_state(d, frame=1):
    """the display list of a sprite payload at the end of frame <frame>: {depth: record}"""
    dl = {}; n = 1
    for code, t in subtags(d):
        if code == T_SHOW:
            if n >= frame: break
            n += 1
        elif code in (T_PLACE, T_PLACE2, T_PLACE3):
            rec = place_parse(code, t); old = dl.get(rec['depth'])
            if rec['move'] and old:
                if rec['char'] is None:   # modify the character at this depth
                    for k in ('matrix', 'cx', 'ratio', 'name', 'clip'):
                        if rec[k] is None: rec[k] = old[k]
                    rec['char'] = old['char']
                    if rec['v3'] and not rec['rest3'] and not (rec['f2'] & 7): rec['rest3'] = old['rest3']; rec['f2'] = old['f2']; rec['cls'] = old['cls']
                    elif not rec['v3'] and old['v3']: rec['v3'] = True; rec['f2'] = old['f2']; rec['rest3'] = old['rest3']; rec['cls'] = old['cls']
                # move + character = replace: the new record stands as it is (missing matrix = identity)
            rec['move'] = False; dl[rec['depth']] = rec
        elif code == T_REMOVE2: dl.pop(struct.unpack_from('<H', t, 0)[0], None)
        elif code == T_REMOVE: dl.pop(struct.unpack_from('<H', t, 2)[0], None)
        elif code == T_END: break
    return dl


def freeze(d, frame=1):
    """a sprite payload cut to the display list of one frame (1 frame, no labels, no actions)"""
    dl = frame_state(d, frame); o = d[:2] + struct.pack('<H', 1)
    for depth in sorted(dl): o += tag_bytes(*place_build(dl[depth]))
    return o + tag_bytes(T_SHOW, b'') + tag_bytes(T_END, b'')


def frames_of(d): return struct.unpack_from('<H', d, 2)[0]


def bounds(swf, cid, frame=1, _mv=None):
    """(xmin, ymin, xmax, ymax) px of a shape / text / sprite frame in its own space; None when nothing is drawn. Scale / translate only for sprites
    (a rotated child's box is taken through its full matrix corners)."""
    mv = _mv or Movie(swf)
    if cid not in mv.defs: return None
    code, d, _ = swf.tags[mv.defs[cid]]
    if code in T_SHAPES or code in T_TEXTS or code == T_EDIT or code in T_MORPHS:
        x0, x1, y0, y1 = Bits(d, 2).rect(); return (x0 / 20.0, y0 / 20.0, x1 / 20.0, y1 / 20.0)
    if code != T_SPRITE: return None
    box = None
    for rec in frame_state(d, frame).values():
        if rec['char'] is None or rec['clip'] is not None: continue
        b = bounds(swf, rec['char'], 1, mv)
        if not b: continue
        m = Bits(rec['matrix'], 0).matrix() if rec['matrix'] else [1.0, 0.0, 0.0, 1.0, 0, 0]
        pts = [(m[0] * x + m[2] * y + m[4] / 20.0, m[1] * x + m[3] * y + m[5] / 20.0) for x in (b[0], b[2]) for y in (b[1], b[3])]
        bb = (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))
        box = bb if not box else (min(box[0], bb[0]), min(box[1], bb[1]), max(box[2], bb[2]), max(box[3], bb[3]))
    return box


# ---------------------------------------------------------------- the graft
class Graft:
    def __init__(s, dst, src=None):
        s.dst = dst; s.dm = Movie(dst); s.src = src; s.sm = Movie(src) if src else None
        s.next = max(s.dm.used_ids() | {0}) + 1
        s.map = {}            # source id -> target id
        s.new_defs = []       # [code, payload] in dependency order
        s.new_imports = []    # (url, [(id, name)])
        s.new_places = []     # root PlaceObject2 payloads
        s.dropped = []        # (source id, class name): class links of the source that are not carried over
        s.frozen = []         # (source id, frames it had, frame kept)
        s.depth = max(s.dm.root_depths() | {0})
        s._made = {}          # ids defined by this graft (target id -> index in new_defs), for bounds of composed sprites

    def _alloc(s):
        v = s.next; s.next += 1
        if v >= 0xffff: raise ValueError('out of character ids')
        return v

    def copy(s, what, frames=None, freeze_sprites=True):
        cid = s.sm.resolve(what); frames = frames or {}
        for c in s.sm.tree(cid):
            if c in s.map: continue
            if c in s.sm.imports:
                url, name = s.sm.imports[c]
                same = [k for k, v in s.dm.imports.items() if v == (url, name)]   # the target already imports this asset: use its character
                if same: s.map[c] = same[0]; continue
                s.map[c] = s._alloc(); s.dm.imports[s.map[c]] = (url, name)
                for u, lst in s.new_imports:
                    if u == url: lst.append((s.map[c], name)); break
                else: s.new_imports.append((url, [(s.map[c], name)]))
                continue
            if c not in s.sm.defs: raise KeyError('character %d is used by the tree but not defined in the source' % c)
            code, d, _ = s.src.tags[s.sm.defs[c]]
            s.map[c] = s._alloc()
            if c in s.sm.classes: s.dropped.append((c, s.sm.classes[c]))
            if code == T_SPRITE and freeze_sprites and (frames_of(d) > 1 or c in frames):
                s.frozen.append((c, frames_of(d), frames.get(c, 1))); d = freeze(d, frames.get(c, 1))
            s.new_defs.append([code, s._remap(code, d, c)])
            s._made[s.map[c]] = len(s.new_defs) - 1
            for n in s.sm.hang.get(c, []):
                hc, hd, _ = s.src.tags[n]; s.new_defs.append([hc, struct.pack('<H', s.map[c]) + hd[2:]])
        return s.map[cid]

    def _remap(s, code, d, own):
        b = bytearray(d); struct.pack_into('<H', b, 0, s.map[own])
        offs = sprite_uses(d) if code == T_SPRITE else ids_of(code, d)[1]
        for o in offs:
            v = struct.unpack_from('<H', d, o)[0]
            if v != 0xffff: struct.pack_into('<H', b, o, s.map[v])
        return bytes(b)

    def sprite(s, children):
        """a new 1-frame DefineSprite: children = [(target char id, depth, (x, y) | (x, y, scale) | MATRIX bytes, name | None)]"""
        cid = s._alloc(); o = struct.pack('<HH', cid, 1)
        for ch, depth, m, name in children:
            mb = m if isinstance(m, (bytes, bytearray)) else matrix_bytes(m[0], m[1], *(m[2:] * 2 if len(m) == 3 else m[2:]))
            rec = dict(depth=depth, char=ch, matrix=bytes(mb), cx=None, ratio=None, name=(name.encode() + b'\0') if name else None, clip=None, cls=None, f2=0, rest3=b'', move=False, v3=False, actions=None)
            o += tag_bytes(*place_build(rec))
        s.new_defs.append([T_SPRITE, o + tag_bytes(T_SHOW, b'') + tag_bytes(T_END, b'')]); s._made[cid] = len(s.new_defs) - 1
        return cid

    def place(s, cid, x, y, name, scale=1.0, depth=None):
        if depth is None: s.depth += 1; depth = s.depth
        rec = dict(depth=depth, char=cid, matrix=matrix_bytes(x, y, scale, scale), cx=None, ratio=None, name=name.encode() + b'\0', clip=None, cls=None, f2=0, rest3=b'', move=False, v3=False, actions=None)
        s.new_places.append(place_build(rec)[1]); return depth

    def preview(s):
        """a Swf with the new definitions appended (not for writing): lets bounds() see the grafted characters before finish()"""
        tmp = dos2_abc.Swf(s.dst.write(compress=False)); k = len(tmp.tags) - 1
        tmp.tags[k:k] = [[c, d, False] for c, d in s.new_defs]
        return tmp

    def finish(s):
        """insert the new tags into the target; returns it"""
        tags = s.dst.tags
        ctl = [n for n, t in enumerate(tags) if t[0] in (T_PLACE, T_PLACE2, T_PLACE3, T_ABC, 72, T_SYMCLASS, T_SHOW, T_REMOVE, T_REMOVE2)]
        at = ctl[0] if ctl else len(tags) - 1
        new = [[T_IMPORT2, url + b'\0' + b'\x01\x00' + struct.pack('<H', len(lst)) + b''.join(struct.pack('<H', c) + n + b'\0' for c, n in lst), False] for url, lst in s.new_imports]
        new += [[c, d, False] for c, d in s.new_defs]
        tags[at:at] = new
        show = next(n for n, t in enumerate(tags) if t[0] == T_SHOW)   # the first frame of the root timeline
        stop = [n for n, t in enumerate(tags[:show]) if t[0] in (T_ABC, 72, T_SYMCLASS)]
        at = stop[0] if stop else show
        tags[at:at] = [[T_PLACE2, d, False] for d in s.new_places]
        return s.dst


# ---------------------------------------------------------------- checks
def check(before, after, quiet=False):
    """problems of a graft result against the movie it was made from (both as file bytes). The DoABC tags may differ (the caller patches them);
    everything else of the original must be there byte for byte, in order."""
    a = dos2_abc.Swf(before); b = dos2_abc.Swf(after); bad = []
    if a.head != b.head or a.tail != b.tail or a.ver != b.ver: bad.append('header / tail differ')
    j = 0; extra = []
    for code, d, lng in b.tags:
        if j < len(a.tags) and code == a.tags[j][0] and (d == a.tags[j][1] or code in (T_ABC, 72)): j += 1
        else: extra.append((code, d))
    if j != len(a.tags): bad.append('original tag #%d (code %d) is missing or changed' % (j, a.tags[j][0]))
    defined = set(); mv = Movie(b); n_def = 0
    for code, d, _ in b.tags:
        if code == T_IMPORT2:
            url, p = cstr(d, 0); p += 2; cnt = struct.unpack_from('<H', d, p)[0]; p += 2
            for _ in range(cnt):
                cid = struct.unpack_from('<H', d, p)[0]; p = cstr(d, p + 2)[1]
                if cid in defined: bad.append('imported id %d defined twice' % cid)
                defined.add(cid)
        elif code in DEFS:
            cid = struct.unpack_from('<H', d, 0)[0]; n_def += 1
            uses = [struct.unpack_from('<H', d, o)[0] for o in (sprite_uses(d) if code == T_SPRITE else ids_of(code, d)[1])]
            for u in uses:
                if u != 0xffff and u not in defined: bad.append('character %d (%s) uses %d before its definition' % (cid, KIND.get(code, code), u))
            if cid in defined: bad.append('character id %d defined twice' % cid)
            defined.add(cid)
        elif code in HANG:
            if struct.unpack_from('<H', d, 0)[0] not in defined: bad.append('tag %d about character %d before its definition' % (code, struct.unpack_from('<H', d, 0)[0]))
        elif code in (T_PLACE, T_PLACE2, T_PLACE3):
            for o in place_ids(code, d):
                u = struct.unpack_from('<H', d, o)[0]
                if u not in defined: bad.append('root PlaceObject of character %d before its definition' % u)
    depths = [place_parse(c, d)['depth'] for c, d, _ in b.tags if c in (T_PLACE, T_PLACE2, T_PLACE3) and not (c != T_PLACE and d[0] & 1)]
    f1 = []
    for c, d, _ in b.tags:
        if c == T_SHOW: break
        if c in (T_PLACE, T_PLACE2, T_PLACE3): f1.append(place_parse(c, d)['depth'])
    if len(f1) != len(set(f1)): bad.append('two root placements at one depth in frame 1')
    again = dos2_abc.Swf(b.write())
    if again.body() != b.body(): bad.append('compress + re-read differs')
    if not quiet:
        print('graft check: %d original tags kept byte for byte and in order (DoABC compared by position only), %d new tags (%s), %d characters, ids unique, every use after its definition: %s' % (
            j, len(extra), ', '.join('%d x %s' % (sum(1 for c, _ in extra if c == k), KIND.get(k, 'PlaceObject2' if k == T_PLACE2 else 'tag %d' % k)) for k in sorted({c for c, _ in extra})) or 'none',
            n_def, 'FAILED' if bad else 'ok'))
        for m in bad[:20]: print('  ' + m)
    return bad


def selftest(src_bytes, what, dst_bytes, quiet=False):
    """graft a tree, write, re-read, and copy the grafted tree BACK into ids of the source: every tag must equal the source tag (sprites: not frozen)"""
    src = dos2_abc.Swf(src_bytes); dst = dos2_abc.Swf(dst_bytes); g = Graft(dst, src); new = g.copy(what, freeze_sprites=False); g.finish(); out = dst.write()
    bad = check(dst_bytes, out, quiet=True)
    back = dos2_abc.Swf(out); bm = Movie(back); sm = Movie(src); inv = {v: k for k, v in g.map.items()}; n = 0
    for c in bm.tree(new):
        if c in bm.imports:
            if bm.imports[c] != sm.imports.get(inv[c]): bad.append('import %d differs' % c)
            continue
        code, d, _ = back.tags[bm.defs[c]]; b = bytearray(d); struct.pack_into('<H', b, 0, inv[c])
        for o in (sprite_uses(d) if code == T_SPRITE else ids_of(code, d)[1]):
            v = struct.unpack_from('<H', d, o)[0]
            if v != 0xffff: struct.pack_into('<H', b, o, inv[v])
        scode, sd, _ = src.tags[sm.defs[inv[c]]]; n += 1
        if (code, bytes(b)) != (scode, sd): bad.append('character %d (source %d) differs after the round trip' % (c, inv[c]))
    if not quiet: print('graft self-test %s: %d characters copied (%d imports), mapped back = source byte for byte, target tags intact: %s' % (what, n, len(g.new_imports), 'FAILED' if bad else 'ok'))
    for m in bad[:20]: print('  ' + m)
    return bad


def png(swf, cid, path):
    mv = Movie(swf); code, d, _ = swf.tags[mv.defs[cid]]
    if code not in (20, 36): raise ValueError('character %d is not a DefineBitsLossless bitmap' % cid)
    fmt, w, h = d[2], struct.unpack_from('<H', d, 3)[0], struct.unpack_from('<H', d, 5)[0]
    if fmt != 5: raise ValueError('bitmap format %d' % fmt)
    raw = zlib.decompress(d[7:]); rows = []
    for y in range(h):
        row = bytearray(b'\0')
        for x in range(w):
            a, r, g, b = raw[(y * w + x) * 4:(y * w + x) * 4 + 4]
            if code == 20: a = 255
            elif a: r, g, b = min(255, r * 255 // a), min(255, g * 255 // a), min(255, b * 255 // a)   # premultiplied
            row += bytes((r, g, b, a))
        rows.append(bytes(row))
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))
    return w, h


def main():
    a = sys.argv[1:]
    if len(a) < 2: sys.exit(__doc__)
    cmd = a[0]; swf = dos2_abc.Swf(open(a[1], 'rb').read()); mv = Movie(swf)
    if cmd == 'list':
        for cid in sorted(set(mv.defs) | set(mv.imports)):
            if cid in mv.imports: print('%5d import   %s : %s' % (cid, mv.imports[cid][0].decode(), mv.imports[cid][1].decode())); continue
            code, d, _ = swf.tags[mv.defs[cid]]
            print('%5d %-8s %7d B %s%s uses %s' % (cid, KIND.get(code, code), len(d), mv.classes.get(cid, ''), ' frames %d' % frames_of(d) if code == T_SPRITE else '', sorted(set(mv.uses(cid)))))
    elif cmd == 'tree':
        def show(cid, ind, frame):
            code, d, _ = swf.tags[mv.defs[cid]]
            for depth, rec in sorted(frame_state(d, frame).items()):
                m = Bits(rec['matrix'], 0).matrix() if rec['matrix'] else [1, 0, 0, 1, 0, 0]; c = rec['char']
                kind = 'import' if c in mv.imports else KIND.get(swf.tags[mv.defs[c]][0], '?') if c in mv.defs else '?'
                bb = bounds(swf, c, 1, mv)
                print('%sdepth %3d  id %4d %-8s %-28s x %8.2f y %8.2f  scale %.3f %.3f%s%s  %s' % (ind, depth, c, kind, (rec['name'] or b'\0')[:-1].decode() or '-', m[4] / 20.0, m[5] / 20.0, m[0], m[3],
                      ' clip %d' % struct.unpack('<H', rec['clip'])[0] if rec['clip'] else '', ' box %.0f,%.0f..%.0f,%.0f' % bb if bb else '', mv.classes.get(c, '')))
                if c in mv.defs and swf.tags[mv.defs[c]][0] == T_SPRITE: show(c, ind + '    ', 1)
        cid = mv.resolve(a[2]); code, d, _ = swf.tags[mv.defs[cid]]
        print('%d %s %s: %d frame(s), box of frame %s: %s' % (cid, KIND.get(code, code), mv.classes.get(cid, ''), frames_of(d) if code == T_SPRITE else 1, a[3] if len(a) > 3 else 1, bounds(swf, cid, int(a[3]) if len(a) > 3 else 1, mv)))
        if code == T_SPRITE: show(cid, '  ', int(a[3]) if len(a) > 3 else 1)
    elif cmd == 'graft':
        what, _, fr = a[2].partition('@'); dst_bytes = open(a[3], 'rb').read(); dst = dos2_abc.Swf(dst_bytes); g = Graft(dst, swf)
        cid = mv.resolve(what); new = g.copy(cid, frames={cid: int(fr)} if fr else None, freeze_sprites='--keep-frames' not in a)
        if '--place' in a:
            p = a[a.index('--place') + 1].split(','); g.place(new, float(p[0]), float(p[1]), p[2] if len(p) > 2 else 'graft%d_mc' % new)
        g.finish(); out = dst.write(); open(a[4], 'wb').write(out)
        print('%s %s -> character %d of %s: %d definition tag(s), %d import(s), %d sprite(s) frozen, class links not carried: %s' % (
            a[1], a[2], new, a[4], len(g.new_defs), sum(len(l) for _, l in g.new_imports), len(g.frozen), ', '.join(n for _, n in g.dropped) or 'none'))
        sys.exit(1 if check(dst_bytes, out) else 0)
    elif cmd == 'selftest':
        sys.exit(1 if selftest(open(a[1], 'rb').read(), mv.resolve(a[2]), open(a[3], 'rb').read()) else 0)
    elif cmd == 'png':
        print('%s: %dx%d' % ((a[3],) + png(swf, int(a[2]), a[3])))
    else: sys.exit(__doc__)


if __name__ == '__main__':
    main()
