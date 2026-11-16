#!/usr/bin/env python3
# Pack every <key>.xgs shader bundle of a directory into one file, <dir>/shaders.xgspack (or the given output), which the engine reads
# with one open instead of one FUSE open per shader (xbg_game.cpp namespace xpack, DM_GFX_SHADER_PACK).
# Format (little endian): u32 magic 'XGSP' (0x50534758), u32 version 1, u32 count, u32 0; count index entries of 56 bytes sorted by key
# (strcmp): char key[32] (NUL-padded), u64 offset, u32 size, u32 0, u64 CRC-32 (zlib) of the bundle bytes; then the bundles, each at its offset
# (16-byte aligned). Usage: xgs_pack.py <shader dir> [output] [--check]
#   --check: read an existing pack back and compare every entry with its file (exit 1 on any difference or missing bundle).
import os, struct, sys, zlib

MAGIC, VERSION, ENT = 0x50534758, 1, struct.Struct('<32sQIIQ')


def crc(b):
    return zlib.crc32(b) & 0xffffffff


def bundles(d):
    keys = sorted(n[:-4] for n in os.listdir(d) if n.endswith('.xgs') and len(n) - 4 < 32)
    return [k for k in keys if os.path.isfile(os.path.join(d, k + '.xgs'))]


def build(d, out):
    keys = bundles(d)
    keys.sort(key=lambda k: k.encode())
    head = 16 + len(keys) * ENT.size
    off = (head + 15) & ~15
    idx, blobs = [], []
    for k in keys:
        b = open(os.path.join(d, k + '.xgs'), 'rb').read()
        idx.append(ENT.pack(k.encode().ljust(32, b'\0'), off, len(b), 0, crc(b)))
        blobs.append((off, b))
        off = (off + len(b) + 15) & ~15
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(struct.pack('<4I', MAGIC, VERSION, len(keys), 0))
        f.write(b''.join(idx))
        for o, b in blobs:
            f.write(b'\0' * (o - f.tell()))
            f.write(b)
    os.replace(tmp, out)
    print('%s: %d bundles, %.1f MB' % (out, len(keys), os.path.getsize(out) / 1048576.0))


def check(d, out):
    data = open(out, 'rb').read()
    magic, ver, n, _ = struct.unpack_from('<4I', data, 0)
    if magic != MAGIC or ver != VERSION:
        print('not an xgspack v1:', out); return 1
    bad, seen = 0, set()
    for i in range(n):
        k, o, sz, _, h = ENT.unpack_from(data, 16 + i * ENT.size)
        k = k.rstrip(b'\0').decode(); seen.add(k)
        f = os.path.join(d, k + '.xgs')
        if not os.path.isfile(f) or open(f, 'rb').read() != data[o:o + sz] or crc(data[o:o + sz]) != h:
            bad += 1; print('differs:', k)
    missing = [k for k in bundles(d) if k not in seen]
    for k in missing[:20]:
        print('not in the pack:', k)
    print('%s: %d entries, %d differ, %d bundles not in the pack' % (out, n, bad, len(missing)))
    return 1 if bad or missing else 0


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        print(__doc__ or 'usage: xgs_pack.py <shader dir> [output] [--check]'); sys.exit(2)
    d = args[0]
    out = args[1] if len(args) > 1 else os.path.join(d, 'shaders.xgspack')
    sys.exit(check(d, out) if '--check' in sys.argv else build(d, out) or 0)
