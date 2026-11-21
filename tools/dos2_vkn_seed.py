#!/usr/bin/env python3
"""DOS2 VK-SEED: the portable pipeline seed of the NATIVE backend (DM_VK_NATIVE=1; docs/dos2/nr/VK-SEED.md; runtime dm/gfx/vkn/vkn_seed.c).

The backend's own warm-up list (vkn-pipeline-list.bin, dm/gfx/vkn/vkn_warm.c, list version 2) keys every pipeline by content that is
already stable across runs: the stage program keys (16 hex digits = a hash of a game shader's control + code), the engine's argument
tuples of blend / depth-stencil / raster, the engine's vertex format elements, primitive, masks, the pass formats (VkFormats), the
technique bits and recipes. Only a few fields are the RECORDING DEVICE's choice, and the header binds the file to that device (vendor,
device, driver, pipelineCacheUUID). `build` turns one or more such lists (several phones / scenes / runs) into one device-independent
seed (dos2-vkn-seed.bin, shipped in the APK):
  * header: no device binding - the sources' vendor / device ids are listed for the record only;
  * the vertex retype code (vkn_draw.c retype_code: "#vi", on everywhere but Adreno) -> 0: the reading device makes it again by its own
    rule from the shader and the vertex format (vkn_warm_retype_code); likewise the first word of a "mat" vertex-module recipe (its
    `code`, the same retype code);
  * depth formats: a list recorded on a device WITHOUT D24 depth attachments (stock Xclipse; DM_GFX_SIM_GPU=xclipse / nod24 runs) holds
    D32F / D32S8 stand-ins (vkn_tex.c) that may as well be the engine's own D32 targets: such a record is kept with the D24 format and
    the flag EITHER (pad3 bit 0): the reader builds the device's form of both (a D24 device builds D24 and D32, a device without D24
    builds its stand-in once);
  * colour formats, samples, everything else: as recorded (the engine's formats; a format the reader cannot render to is refused there).
Records are deduplicated on that canonical content (two devices' records of one draw are one seed record) and kept in first-seen order
(the order the game first drew them: the warm-up builds them in that order). Each record keeps a checksum (fnv64 of the record, as the
list does) - the reader drops a torn or foreign one.

Content (`audit` proves it): program keys (hex), Vulkan enums / formats, engine argument bytes, vertex element bytes, masks, piece
names and their recipe words. No shader code, no SPIR-V, no game file bytes, no device-specific data.

  build --list F[:nod24|:d24] [F ...] --out SEED     the seed (+ a report); a list's D24 capability defaults by its vendor
  dump SEED|LIST                                     header + per-record summary counts
  audit SEED                                         re-check every record of a seed (shape, keys, pads, sums)
  cover --seed S --list L[:nod24] [...]              how many records of a device list the seed has (the coverage a fresh install gets
                                                     for the scenes that list saw), by the reader's matching rule
  selftest                                           round trip on made-up records (no files)
"""
import argparse, collections, struct, sys
from pathlib import Path

LIST_MAGIC = b'VKNPLST\x00'
SEED_MAGIC = b'VKNSEED\x00'
SEED_VERSION = 1
REC = 296          # vkn_warm_rec, list version 2 (vkn_warm.h; _Static_assert in vkn_warm.c)
REC1 = 232         # list version 1 (stage 4): the first 224 bytes + their sum
HDR_LIST = 48      # whdr: magic, version, rec_size, vendor, device, driver, pad, uuid[16]
HDR_SEED = 64
SUM_AT = 288
# field offsets of vkn_warm_rec (checked by the C side: vkn_seed.c _Static_asserts the same numbers)
O_KEY0, O_KEY1, O_RETYPE, O_AREF, O_AFUNC, O_COLOR, O_DEPTH = 0, 16, 32, 40, 44, 48, 80
O_PRIM, O_SREF, O_FLAGS, O_NVEL = 84, 85, 86, 87
O_BLEND, O_DS, O_RASTER, O_NCOLOR, O_SAMPLES, O_PAD0, O_WMASK = 88, 94, 102, 107, 108, 109, 110
O_VSIN, O_PAD1, O_VEL, O_PAD2 = 118, 120, 122, 218
O_FX, O_RKIND, O_FSALT, O_PAD3, O_RLEN, O_PIECE, O_RARG = 224, 225, 226, 227, 228, 230, 240
F_BLEND, F_DS, F_RASTER, F_VFMT, F_FS = 1, 2, 4, 8, 16
R_NONE, R_FX, R_VS = 0, 1, 2
P_EITHER = 1                                       # pad3 bit 0 of a seed record: depth D24 or its D32 stand-in (source without D24)
VK_X8D24, VK_D32F, VK_D24S8, VK_D32S8 = 125, 126, 129, 130
D24_OF = {VK_D32F: VK_X8D24, VK_D32S8: VK_D24S8}  # stand-in -> the engine format it may stand in for
NOD24_VENDORS = {0x144D: 'Samsung Xclipse (stock driver)', 0x1002: 'AMD / RADV (Xclipse)'}
HEX = set(b'0123456789abcdefABCDEF')


def fnv64(b):
    h = 1469598103934665603
    for c in b:
        h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def u32(b, o): return struct.unpack_from('<I', b, o)[0]


def piece(r): return bytes(r[O_PIECE:O_PIECE + 10]).split(b'\0')[0].decode('latin1')


def rec_sum(r): return fnv64(bytes(r[:SUM_AT]))


def set_sum(r): struct.pack_into('<Q', r, SUM_AT, rec_sum(r))


def ident(r):
    """The portable identity of a seed record (the C reader's seed_ident): fnv of the record with its seed flags cleared."""
    x = bytearray(r[:SUM_AT]); x[O_PAD3] = 0
    return fnv64(bytes(x))


class Bad(Exception):
    pass


def check_shape(r):
    """The checks the reader makes (vkn_warm.c list_load + vkn_seed.c): a record a device could have written."""
    if r[O_NVEL] > 16 or r[O_NCOLOR] > 8 or struct.unpack_from('<H', r, O_RLEN)[0] > 48:
        raise Bad('shape')
    if r[O_PAD0] or bytes(r[O_PAD1:O_PAD1 + 2]) != b'\0\0' or bytes(r[O_PAD2:O_PAD2 + 6]) != b'\0' * 6:
        raise Bad('pad')
    if r[O_RKIND] not in (R_NONE, R_FX, R_VS) or r[O_FX] & ~3 or r[O_FSALT] > 1:
        raise Bad('technique')
    k0, k1 = bytes(r[O_KEY0:O_KEY0 + 16]), bytes(r[O_KEY1:O_KEY1 + 16])
    if not all(c in HEX for c in k0):
        raise Bad('key')
    if r[O_FLAGS] & F_FS:
        if not all(c in HEX for c in k1):
            raise Bad('key')
    elif k1 != b'\0' * 16:
        raise Bad('key')
    if r[O_RKIND] and not piece(r):
        raise Bad('technique')
    for i in range(8):
        f = u32(r, O_COLOR + 4 * i)
        if f and (f >= 124 and f <= 130):
            raise Bad('format')   # a depth format in a colour slot
    d = u32(r, O_DEPTH)
    if d and not (124 <= d <= 130):
        raise Bad('format')


def read_list(path):
    b = Path(path).read_bytes()
    if len(b) < HDR_LIST or b[:8] != LIST_MAGIC:
        raise SystemExit(f'{path}: not a vkn-pipeline-list.bin (magic)')
    ver, rs, ven, dev, drv = struct.unpack_from('<5I', b, 8)
    if (ver, rs) not in ((2, REC), (1, REC1)):
        raise SystemExit(f'{path}: list version {ver} / record size {rs}: not 1/232 or 2/296')
    out, bad = [], collections.Counter()
    n = (len(b) - HDR_LIST) // rs
    for i in range(n):
        s = b[HDR_LIST + i * rs:HDR_LIST + (i + 1) * rs]
        r = bytearray(REC)
        if ver == 1:
            r[:224] = s[:224]
            if struct.unpack_from('<Q', s, 224)[0] != fnv64(s[:224]):
                bad['sum'] += 1; continue
            set_sum(r)
        else:
            r[:] = s
            if struct.unpack_from('<Q', r, SUM_AT)[0] != rec_sum(r):
                bad['sum'] += 1; continue
        try:
            check_shape(r)
        except Bad as e:
            bad[str(e)] += 1; continue
        out.append(r)
    if (len(b) - HDR_LIST) % rs:
        bad['torn tail'] += 1
    return dict(path=str(path), version=ver, vendor=ven, device=dev, driver=drv, recs=out, bad=bad)


def canon(r, nod24):
    """A device record -> its seed record (canonical content; vkn_seed.c canon_rec is the same rule)."""
    x = bytearray(r)
    struct.pack_into('<Q', x, O_RETYPE, 0)
    if x[O_RKIND] == R_VS and piece(x) == 'mat' and struct.unpack_from('<H', x, O_RLEN)[0] == 24:
        struct.pack_into('<Q', x, O_RARG, 0)   # the recipe's retype code
    x[O_PAD3] = 0
    d = u32(x, O_DEPTH)
    if nod24 and d in D24_OF:
        struct.pack_into('<I', x, O_DEPTH, D24_OF[d]); x[O_PAD3] |= P_EITHER
    set_sum(x)
    return x


def list_arg(a):
    for opt in ('nod24', 'd24'):   # (a Windows path keeps its drive letter's colon)
        if a.endswith(':' + opt): return a[:-len(opt) - 1], opt
    return a, ''


def nod24_of(L, opt):
    if opt == 'nod24': return True
    if opt == 'd24': return False
    return L['vendor'] in NOD24_VENDORS


def build_seed(lists):
    """lists: [(list dict, nod24)] -> (records in first-seen order, report)."""
    seen, recs = {}, []
    rep = collections.Counter()
    for L, nod24 in lists:
        for r in L['recs']:
            x = canon(r, nod24)
            k = ident(x)
            if k in seen:
                y = recs[seen[k]]
                if (x[O_PAD3] & P_EITHER) and not (y[O_PAD3] & P_EITHER):
                    rep['duplicate (an exact D24 record wins over an EITHER one)'] += 1
                elif (y[O_PAD3] & P_EITHER) and not (x[O_PAD3] & P_EITHER):
                    recs[seen[k]] = x; rep['duplicate (the exact D24 record replaced an EITHER one)'] += 1
                else:
                    rep['duplicate'] += 1
                continue
            # an exact D32 record and an EITHER record of its D24 form describe the same D32 build on a D24-less reader: keep both
            seen[k] = len(recs); recs.append(x)
            rep['kept'] += 1
    return recs, rep


def seed_bytes(recs, lists):
    body = b''.join(bytes(r) for r in recs)
    hid = fnv64(body)
    src = [(L['vendor'], L['device']) for L, _ in lists][:3]
    h = bytearray(HDR_SEED)
    h[:8] = SEED_MAGIC
    struct.pack_into('<4IQ2I', h, 8, SEED_VERSION, REC, len(recs), len(lists), hid, 2, 0)
    for i, (v, d) in enumerate(src):
        struct.pack_into('<2I', h, 40 + 8 * i, v, d)
    return bytes(h) + body, hid


def read_seed(path):
    b = Path(path).read_bytes()
    if len(b) < HDR_SEED or b[:8] != SEED_MAGIC:
        raise SystemExit(f'{path}: not a dos2-vkn-seed.bin (magic)')
    ver, rs, n, nsrc, hid, lv, fl = struct.unpack_from('<4IQ2I', b, 8)
    if ver != SEED_VERSION or rs != REC or len(b) != HDR_SEED + n * REC:
        raise SystemExit(f'{path}: seed version {ver}, record size {rs}, {n} records, {len(b)} bytes: not this tool\'s seed')
    if fnv64(b[HDR_SEED:]) != hid:
        raise SystemExit(f'{path}: the body does not match the header id (damaged)')
    recs = [bytearray(b[HDR_SEED + i * REC:HDR_SEED + (i + 1) * REC]) for i in range(n)]
    src = [struct.unpack_from('<2I', b, 40 + 8 * i) for i in range(min(nsrc, 3))]
    return dict(id=hid, recs=recs, nsrc=nsrc, src=src)


def summary(recs):
    c = collections.Counter()
    for r in recs:
        c['records'] += 1
        c['technique: ' + ('piece object' if r[O_RKIND] == R_FX else 'piece vertex module' if r[O_RKIND] == R_VS else
                          'fx bits' if r[O_FX] else 'named fs' if r[O_FSALT] else 'plain')] += 1
        if r[O_RKIND]: c['piece ' + piece(r)] += 1
        c['depth %d' % u32(r, O_DEPTH)] += 1
        if r[O_PAD3] & P_EITHER: c['EITHER (D24 or its D32 stand-in)'] += 1
        if struct.unpack_from('<Q', r, O_RETYPE)[0]: c['retype code set'] += 1
        if struct.unpack_from('<I', r, O_AFUNC)[0]: c['alpha-test variant'] += 1
    c['shader key pairs'] = len({bytes(r[:32]) for r in recs})
    return c


def cmd_build(a):
    lists = []
    for arg in a.list:
        p, opt = list_arg(arg)
        L = read_list(p)
        nd = nod24_of(L, opt)
        lists.append((L, nd))
        print(f'{p}: list version {L["version"]}, vendor {L["vendor"]:#x} device {L["device"]:#x} driver {L["driver"]:#x}: '
              f'{len(L["recs"])} records (dropped: {dict(L["bad"]) or "none"}); depth: {"no D24 (stand-ins read as EITHER)" if nd else "D24 native (formats as recorded)"}')
    recs, rep = build_seed(lists)
    data, hid = seed_bytes(recs, lists)
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_bytes(data)
    print(f'{a.out}: seed {hid:016x}, {len(recs)} records, {len(data)} bytes; {dict(rep)}')
    for k, v in sorted(summary(recs).items()): print(f'  {k}: {v}')


def cmd_dump(a):
    b = Path(a.file).read_bytes()
    if b[:8] == SEED_MAGIC:
        S = read_seed(a.file)
        print(f'seed {S["id"]:016x}: {len(S["recs"])} records from {S["nsrc"]} list(s): ' + ', '.join(f'{v:#x}/{d:#x}' for v, d in S['src']))
        recs = S['recs']
    else:
        L = read_list(a.file)
        print(f'list version {L["version"]}: vendor {L["vendor"]:#x} device {L["device"]:#x}: {len(L["recs"])} records, dropped {dict(L["bad"])}')
        recs = L['recs']
    for k, v in sorted(summary(recs).items()): print(f'  {k}: {v}')


def cmd_audit(a):
    S = read_seed(a.file)
    bad = collections.Counter()
    for r in S['recs']:
        if struct.unpack_from('<Q', r, SUM_AT)[0] != rec_sum(r): bad['sum'] += 1; continue
        try: check_shape(r)
        except Bad as e: bad[str(e)] += 1; continue
        if struct.unpack_from('<Q', r, O_RETYPE)[0]: bad['retype not canonical'] += 1
        if r[O_PAD3] & ~P_EITHER: bad['seed flags'] += 1
        if (r[O_PAD3] & P_EITHER) and u32(r, O_DEPTH) not in (VK_X8D24, VK_D24S8): bad['EITHER without a D24 format'] += 1
    print(f'{a.file}: seed {S["id"]:016x}, {len(S["recs"])} records: ' + (f'BAD {dict(bad)}' if bad else
          'every record is program keys + Vulkan enums / formats + engine argument / vertex element bytes + masks + piece recipe words; no code'))
    return 1 if bad else 0


def seed_index(recs):
    """The reader's lookup set: identity of each record, and of the D32 form of an EITHER record."""
    s = set()
    for r in recs:
        s.add(ident(r))
        if r[O_PAD3] & P_EITHER:
            x = bytearray(r); d = u32(x, O_DEPTH)
            struct.pack_into('<I', x, O_DEPTH, VK_D32F if d == VK_X8D24 else VK_D32S8); s.add(ident(x))
    return s


def cmd_cover(a):
    S = read_seed(a.seed); idx = seed_index(S['recs']); keys = {bytes(r[:32]) for r in S['recs']}
    for arg in a.list:
        p, opt = list_arg(arg); L = read_list(p); nd = nod24_of(L, opt)
        had = differ = unknown = 0
        for r in L['recs']:
            x = canon(r, False)
            hit = ident(x) in idx
            if not hit and nd:
                hit = ident(canon(r, True)) in idx
            if hit: had += 1
            elif bytes(r[:32]) in keys: differ += 1
            else: unknown += 1
        n = max(1, len(L['recs']))
        print(f'{p}: {len(L["recs"])} records: the seed has {had} ({100.0 * had / n:.1f} %), same shaders other content {differ}, shaders the seed has no record of {unknown}')


def cmd_selftest(a):
    def mk(depth, retype, nod=False, mat=False):
        r = bytearray(REC)
        r[0:16] = b'a000000000000001'; r[16:32] = b'b000000000000001'; r[O_FLAGS] = F_FS | F_VFMT
        struct.pack_into('<Q', r, O_RETYPE, retype); struct.pack_into('<I', r, O_COLOR, 37); struct.pack_into('<I', r, O_DEPTH, depth)
        r[O_NCOLOR] = 1; r[O_SAMPLES] = 1; r[O_PRIM] = 1; r[O_WMASK] = 15
        if mat:
            r[O_RKIND] = R_VS; r[O_PIECE:O_PIECE + 3] = b'mat'; struct.pack_into('<H', r, O_RLEN, 24); struct.pack_into('<3Q', r, O_RARG, retype, 7, 9)
        set_sum(r); return r
    A = dict(vendor=0x5143, device=1, recs=[mk(VK_D24S8, 0), mk(VK_D32S8, 0), mk(VK_D24S8, 0, mat=True)])
    B = dict(vendor=0x13B5, device=2, recs=[mk(VK_D24S8, 0x30), mk(VK_D24S8, 0x30, mat=True)])            # Mali: retype codes
    C = dict(vendor=0x144D, device=3, recs=[mk(VK_D32S8, 0), mk(VK_D32F, 0)])                           # Xclipse: stand-ins
    recs, rep = build_seed([(A, False), (B, False), (C, True)])
    # B's two = A's once the retype goes; C's D32S8 reads as EITHER D24S8 = A's exact D24S8 (A also has the D32S8 one); C's D32F = new EITHER X8D24
    assert rep['kept'] == 4 and rep['duplicate'] == 2 and rep['duplicate (an exact D24 record wins over an EITHER one)'] == 1, rep
    assert sum(1 for r in recs if r[O_PAD3] & P_EITHER) == 1
    data, hid = seed_bytes(recs, [(A, False), (B, False), (C, True)])
    p = Path(a.tmp or '.') / 'selftest.vseed'; p.write_bytes(data)
    S = read_seed(p); p.unlink()
    assert S['id'] == hid and len(S['recs']) == 4
    idx = seed_index(S['recs'])
    assert ident(canon(mk(VK_D32S8, 0x11), False)) in idx and ident(canon(mk(VK_D24S8, 0x11, mat=True), False)) in idx
    assert ident(canon(mk(VK_D32F, 0), False)) in idx and ident(canon(mk(VK_X8D24, 0), False)) in idx   # the EITHER record, both forms
    print('selftest: ok (retype and mat recipe code canonical, D24-less stand-ins EITHER, dedup across devices, header id)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sp = ap.add_subparsers(dest='cmd', required=True)
    b = sp.add_parser('build'); b.add_argument('--list', nargs='+', required=True); b.add_argument('--out', required=True)
    d = sp.add_parser('dump'); d.add_argument('file')
    u = sp.add_parser('audit'); u.add_argument('file')
    c = sp.add_parser('cover'); c.add_argument('--seed', required=True); c.add_argument('--list', nargs='+', required=True)
    t = sp.add_parser('selftest'); t.add_argument('--tmp')
    a = ap.parse_args()
    return {'build': cmd_build, 'dump': cmd_dump, 'audit': cmd_audit, 'cover': cmd_cover, 'selftest': cmd_selftest}[a.cmd](a) or 0


if __name__ == '__main__':
    sys.exit(main())
