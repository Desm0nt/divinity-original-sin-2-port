"""Larian LSPK v13 package reader (Divinity: Original Sin 2 DE, Switch RomFS Data/*.pak).
Layout (header at the end): file end - 8 = u32 header size + "LSPK"; header = u32 version 13, u32 file list offset, u32 file list
size, u16 parts, u8 flags, u8 priority, 16 bytes md5. File list = u32 count + LZ4 block of count x 280-byte entries: name[256],
u32 offset, u32 size on disk, u32 uncompressed size (0 = stored), u32 archive part, u32 flags (low nibble 0 none / 1 zlib / 2 lz4), u32 crc.
Usage: lspk.py list <pak> [glob]          -> path, size, stored size, method
       lspk.py extract <pak> <dest> [glob]
"""
import fnmatch, os, struct, sys, zlib
from pathlib import Path
import lz4.block, lz4.frame


class Pak:
    def __init__(self, path):
        self.path = Path(path)
        self.f = open(path, 'rb')
        size = os.path.getsize(path)
        self.f.seek(size - 8)
        hsize, magic = struct.unpack('<I4s', self.f.read(8))
        assert magic == b'LSPK', (path, magic)
        self.f.seek(size - hsize)
        ver, lo, ls, self.parts, self.flags, self.prio = struct.unpack_from('<IIIHBB', self.f.read(hsize))
        assert ver == 13, (path, ver)
        self.f.seek(lo)
        raw = self.f.read(ls)
        n = struct.unpack_from('<I', raw)[0]
        tab = lz4.block.decompress(raw[4:], uncompressed_size=n * 280)
        self.entries = []
        for i in range(n):
            name, off, disk, usize, part, fl, crc = struct.unpack_from('<256sIIIIII', tab, i * 280)
            self.entries.append((name.split(b'\0')[0].decode('utf-8', 'replace'), off, disk, usize, part, fl, crc))

    def part_file(self, part):
        if part == 0: return self.f
        return open(self.path.with_name(f'{self.path.stem}_{part}{self.path.suffix}'), 'rb')

    def read(self, e):
        name, off, disk, usize, part, fl, crc = e
        f = self.part_file(part)
        f.seek(off)
        b = f.read(disk)
        m = fl & 0xf
        if m == 0 or usize == 0: return b
        if m == 1: return zlib.decompress(b)
        if m == 2:
            try: return lz4.block.decompress(b, uncompressed_size=usize)
            except Exception: return lz4.frame.decompress(b)
        raise ValueError(f'{name}: method {fl:#x}')


def main():
    if len(sys.argv) < 3: sys.exit(__doc__)
    cmd, pak = sys.argv[1], Pak(sys.argv[2])
    if cmd == 'list':
        pat = sys.argv[3] if len(sys.argv) > 3 else '*'
        for e in pak.entries:
            if fnmatch.fnmatch(e[0], pat): print(f'{e[0]}\t{e[3] or e[2]}\t{e[2]}\t{e[5] & 0xf}\tpart{e[4]}')
    elif cmd == 'extract':
        dest = Path(sys.argv[3]); pat = sys.argv[4] if len(sys.argv) > 4 else '*'
        for e in pak.entries:
            if not fnmatch.fnmatch(e[0], pat): continue
            out = dest / e[0]; out.parent.mkdir(parents=True, exist_ok=True); out.write_bytes(pak.read(e))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
