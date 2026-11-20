#!/usr/bin/env python3
"""[DOS2] NR-FRAME S2F: setup's native-nr.xgspack check (tools/setup_dos2.py step_nrpack) accepts packs with and without the OPTIONAL S2F
variants (dos2_nr_sh_pack.OC_OPTIONAL: 5.ClusteredLightingWithSSAO, 5.DeferredFog) and still refuses a pack that misses a required bundle or
holds an unknown one. The rule is replayed on key lists (no pack build); with a pack path argument its real key list is checked too.
Usage: nrpack_optional_test.py [native-nr.xgspack ...]"""
import struct, sys
from pathlib import Path
TOOLS = Path(__file__).resolve().parents[5] / 'tools'
sys.path.insert(0, str(TOOLS))
import dos2_nr_sh_pack as NRP

ok = True
def check(c, m):
    global ok
    print(('PASS ' if c else 'FAIL ') + m); ok &= bool(c)

ENTRIES, OC_VARIANTS, OC_OPTIONAL = NRP.ENTRIES, NRP.OC_VARIANTS, getattr(NRP, 'OC_OPTIONAL', {})
want = sorted([f'{v}.{e[0] if len(e[0]) + 2 < 32 else e[1]}' for e in ENTRIES for v in e[3]] + [f'{v}.{e[0]}' for e in ENTRIES for v, _ in OC_VARIANTS.get(e[0], [])])
optional = sorted(f'{v}.{e[0]}' for e in ENTRIES for v, _ in OC_OPTIONAL.get(e[0], []))
def setup_accepts(have):   # tools/setup_dos2.py step_nrpack, the same expression
    return sorted(k for k in have if k not in optional) == want

src = (TOOLS / 'setup_dos2.py').read_text(encoding='utf-8')
check("if sorted(k for k in have if k not in optional) != want: raise Fail(" in src and "OC_OPTIONAL = getattr(NRP, 'OC_OPTIONAL', {})" in src,
      'setup_dos2.py step_nrpack uses this rule')
check(optional == ['5.ClusteredLightingWithSSAO', '5.DeferredFog'], 'the optional bundles are the S2F variants (%s)' % optional)
check(not set(optional) & set(want), 'no optional bundle is required')
check(setup_accepts(want), 'a pack without the S2F variants is accepted')
check(setup_accepts(sorted(want + optional)), 'a pack with both S2F variants is accepted')
check(setup_accepts(sorted(want + optional[:1])), 'a pack with one of them is accepted')
check(not setup_accepts(want[1:]), 'a pack missing a required bundle is refused')
check(not setup_accepts(sorted(want + ['9.Unknown'])), 'a pack with an unknown bundle is refused')
pk = (TOOLS / 'dos2_nr_sh_pack.py').read_text(encoding='utf-8')
check("except BaseException as ex:\n                    if not optional: raise" in pk, 'the pack builder leaves a failing optional variant out instead of failing')
for p in sys.argv[1:]:
    data = Path(p).read_bytes(); magic, ver, n, _ = struct.unpack_from('<4I', data, 0)
    have = sorted(struct.unpack_from('<32s', data, 16 + 56 * i)[0].rstrip(b'\0').decode() for i in range(n)) if magic == 0x50534758 and ver == 1 else []
    check(setup_accepts(have), '%s (%d bundles, S2F variants %s) is accepted' % (p, len(have), [k for k in have if k in optional]))
print('nrpack_optional_test: ' + ('all passed' if ok else 'FAILED'))
sys.exit(0 if ok else 1)
