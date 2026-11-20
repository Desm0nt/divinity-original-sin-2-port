"""DOS2 NR-02: the NR descriptor layout of the translated shader bundles, kept in the bundle header's spare word (offline).

A bundle (<key>.xgs, dm/gfx/vulkan/engine/xbg_game.h) starts with xbg_shader_header {magic 'XGS1', version 1, stage, binding_count,
input_count, c1_bytes, spirv_bytes, reserved}. The reader (xbg_game.cpp load_shader_key) checks every word but `reserved` and the exact
extent, so the NR layout can only live in `reserved` (a trailing section would fail "Shader bundle extent mismatch"). The word is
computed from the bundle's own binding table exactly as dm/gfx/nr/nr_key.c nr_layout_word does (the C unit test nr_key_test.cpp reads
the --tsv output of this tool back and compares bundle by bundle):
  set 0 = the game's uniform blocks (source 2) in table order, each a dynamic-offset UBO into the constant ring;
  set 1 = the textures (source 7) in table order.
  bits 28..31 0xA, 24..27 version 1, 20..23 uniform blocks (15 = 15+), 14..19 textures (63 = 63+), 0..7 flags:
  1 c1 (the bundle's constant bank), 2 support buffer, 4 driver banks, 8 storage / image, 16 arrays, 32 other source, 64 fits
  (no storage / image / array / other and <= 8 blocks), 128 clamped.
Modes:
  (default) census: per stage how many bundles fit, block / texture count histograms, the flags; per program (vertex + fragment pair of
            work/shaders-jobs/programs.tsv) the summed blocks against the Vulkan limit maxDescriptorSetUniformBuffersDynamic (8 = the
            spec minimum, what Mali / PowerVR report; Adreno reports more).
  --tsv F   key, stage, word, blocks, textures, flags of every bundle
  --write   stamp the word into `reserved` of every bundle of --bundles in place (reserved must be 0 or already that word); a pack
            (xgs_pack.py) has to be rebuilt afterwards. --check: only compare, exit 1 on a bundle whose word is missing or differs.
Usage: dos2_nr_layout.py [--bundles work/shaders-vk] [--programs work/shaders-jobs/programs.tsv] [--tsv out.tsv] [--write | --check] [--limit N]
"""
import argparse, collections, os, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAGIC = 0x31534758
STAGES = {0: 'vs', 1: 'tcs', 2: 'tes', 3: 'gs', 4: 'fs', 5: 'cs'}
F_C1, F_SUPPORT, F_DRIVER, F_STORAGE, F_ARRAYS, F_OTHER, F_FITS, F_CLAMPED = 1, 2, 4, 8, 16, 32, 64, 128
FLAG_NAMES = ['c1', 'support', 'driver', 'storage', 'arrays', 'other', 'fits', 'clamped']
MAX_UBO = 8


def layout_word(bindings):
    """bindings: list of 8-tuples (binding, type, count, bytes, source, slot, sampler_slot, flags) -> u32 (nr_key.c nr_layout_word)"""
    ubo = tex = fl = 0
    for b in bindings:
        count, src = b[2], b[4]
        if src == 2:
            ubo += 1
            if count > 1: fl |= F_ARRAYS
        elif src == 7:
            tex += 1
            if count > 1: fl |= F_ARRAYS
        elif src == 4: fl |= F_C1
        elif src == 1: fl |= F_SUPPORT
        elif src in (5, 6): fl |= F_DRIVER
        elif src in (3, 8, 10, 11): fl |= F_STORAGE
        else: fl |= F_OTHER
    if not fl & (F_STORAGE | F_ARRAYS | F_OTHER) and ubo <= MAX_UBO: fl |= F_FITS
    if ubo > 15: ubo, fl = 15, fl | F_CLAMPED
    if tex > 63: tex, fl = 63, fl | F_CLAMPED
    return 0xA << 28 | 1 << 24 | ubo << 20 | tex << 14 | fl


def read(path):
    b = Path(path).read_bytes()
    if len(b) < 32: return None
    h = struct.unpack_from('<8I', b, 0)
    if h[0] != MAGIC or h[1] != 1 or h[3] > 256: return None
    n = h[3]
    binds = [struct.unpack_from('<8I', b, 32 + 32 * i) for i in range(n)]
    return h, binds


def flags_text(w):
    return '+'.join(FLAG_NAMES[i] for i in range(8) if w >> i & 1) or '-'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(ROOT.parent / 'work' / 'shaders-vk'))
    ap.add_argument('--programs', default=str(ROOT.parent / 'work' / 'shaders-jobs' / 'programs.tsv'))
    ap.add_argument('--tsv', default='')
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    d = Path(a.bundles)
    names = sorted(n for n in os.listdir(d) if n.endswith('.xgs'))
    if a.limit: names = names[:a.limit]
    rows, bad, per = {}, [], collections.defaultdict(collections.Counter)
    ubo_h, tex_h = collections.defaultdict(collections.Counter), collections.defaultdict(collections.Counter)
    for n in names:
        r = read(d / n)
        if r is None: bad.append(n); continue
        h, binds = r
        w = layout_word(binds)
        st = STAGES.get(h[2], str(h[2]))
        rows[n[:-4]] = (st, w, h[7])
        per[st]['bundles'] += 1
        per[st]['fits'] += bool(w & F_FITS)
        for i, f in enumerate(FLAG_NAMES):
            if w >> i & 1 and f != 'fits': per[st][f] += 1
        ubo_h[st][w >> 20 & 15] += 1; tex_h[st][w >> 14 & 63] += 1
        if a.write and h[7] != w:
            if h[7]: bad.append('%s: reserved %#x is not 0 and not the NR word %#x - left alone' % (n, h[7], w)); continue
            with open(d / n, 'r+b') as f: f.seek(28); f.write(struct.pack('<I', w))
        if a.check and h[7] != w: bad.append('%s: reserved %#x, NR word %#x' % (n, h[7], w))
    print('== %d bundles of %s (%d unreadable)' % (len(rows), d, sum(1 for b in bad if b.endswith('.xgs'))))
    for st in sorted(per, key=lambda s: list(STAGES.values()).index(s) if s in STAGES.values() else 9):
        c = per[st]
        print('%-3s %5d bundles, fit the NR layout %5d (%.1f%%); flags: %s' % (st, c['bundles'], c['fits'], 100. * c['fits'] / max(1, c['bundles']),
              ', '.join('%s %d' % (f, c[f]) for f in FLAG_NAMES if f != 'fits' and c[f]) or '-'))
        print('    uniform blocks: %s' % ' '.join('%d:%d' % kv for kv in sorted(ubo_h[st].items())))
        print('    textures:       %s' % ' '.join('%d:%d' % kv for kv in sorted(tex_h[st].items())))
    p = Path(a.programs)
    if p.is_file():
        pairs = fit = 0; sums = collections.Counter(); miss = 0; why = collections.Counter()
        for line in p.read_text(encoding='utf-8').splitlines()[1:]:
            f = line.split('\t')
            if len(f) < 5 or f[1] == '-' or f[3] == '-': continue
            vs, ps = rows.get(f[1]), rows.get(f[3])
            if not vs or not ps: miss += 1; continue
            pairs += 1
            u = (vs[1] >> 20 & 15) + (ps[1] >> 20 & 15)
            sums[u] += 1
            ok = vs[1] & F_FITS and ps[1] & F_FITS and u <= MAX_UBO
            fit += bool(ok)
            if not ok:
                if not (vs[1] & F_FITS and ps[1] & F_FITS): why['a stage does not fit'] += 1
                else: why['blocks of both stages > %d' % MAX_UBO] += 1
        print('== programs (vertex + fragment of %s): %d pairs (%d without a bundle), fit with <= %d dynamic UBOs per pipeline: %d (%.1f%%); %s'
              % (p, pairs, miss, MAX_UBO, fit, 100. * fit / max(1, pairs), ', '.join('%s %d' % kv for kv in why.items()) or '-'))
        print('   blocks per pipeline: %s' % ' '.join('%d:%d' % kv for kv in sorted(sums.items())))
    if a.tsv:
        with open(a.tsv, 'w', newline='\n') as o:
            o.write('key\tstage\tword\tubo\ttex\tflags\treserved\n')
            for k in sorted(rows):
                st, w, res = rows[k]
                o.write('%s\t%s\t%#010x\t%d\t%d\t%s\t%#x\n' % (k, st, w, w >> 20 & 15, w >> 14 & 63, flags_text(w), res))
        print('wrote %s (%d rows)' % (a.tsv, len(rows)))
    for b in bad[:20]: print('BAD ' + b)
    if a.write: print('stamped the NR layout word into %s (rebuild the .xgspack with dm/gfx/tools/xgs_pack.py)' % d)
    return 1 if (a.check or a.write) and bad else 0


if __name__ == '__main__':
    sys.exit(main())
