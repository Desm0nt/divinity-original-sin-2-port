"""Scan every member of every LSPK package (non-audio) and sdk.raw for GLSLCoutput (0x19866891) / NVN control (0x98761234) magics
outside .bshd files. Usage: scan_shader_magic.py <Data dir> > report"""
import sys, os, glob, zlib, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lspk import Pak
G, C = b'\x91\x68\x86\x19', b'\x34\x12\x76\x98'
data = sys.argv[1]
for f in ['exefs-update/sdk.raw', 'exefs-update/main.raw']:
    d = open(f, 'rb').read(); print(f, 'glslc', d.count(G), 'control', d.count(C), flush=True)
skip = ('Voice', 'Video', 'SharedSounds', 'SharedDLCSounds')
for f in sorted(glob.glob(data + '/*.pak') + glob.glob(data + '/Localization/*.pak') + glob.glob(data + '/Localization/*/*.pak')):
    b = os.path.basename(f)
    if b.startswith(skip) or b[:-4].split('_')[-1].isdigit(): continue
    try: p = Pak(f)
    except Exception as e: print('ERR', f, e); continue
    n = hits = 0
    for e in p.entries:
        if e[0].endswith('.bshd'): continue
        try: d = p.read(e)
        except Exception as ex: print('  read err', e[0], ex); continue
        n += 1
        g, c = d.count(G), d.count(C)
        if g or c: hits += 1; print('  HIT', b, e[0], 'glslc', g, 'control', c, flush=True)
    print(b, 'members', n, 'with magic', hits, flush=True)
