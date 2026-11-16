"""DOS2 Switch main: every CreateUIObject(mgr, layer, type, flags, x4, player) call site (bl 0xaebbf0) with the type id it passes
and whether the controller-mode byte main+0x2bb8918 is read shortly before (= the site picks a PC or a `_c` type by mode).

  python tools/dos2_ui_sites.py <main.raw> [types.json from dos2_ui_types.py]
"""
import sys, json, struct, capstone
b = open(sys.argv[1], 'rb').read()
names = {}
if len(sys.argv) > 2:
    for r in json.load(open(sys.argv[2])): names.setdefault(r['id'], r['path'][4:])
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
TEXT = 0x1e00000; CREATE = 0xaebbf0
w = struct.unpack_from('<%dI' % (TEXT // 4), b, 0)
for k, x in enumerate(w):
    if x & 0xfc000000 not in (0x94000000, 0x14000000): continue
    off = x & 0x3ffffff
    if off & 0x2000000: off -= 0x4000000
    if k * 4 + off * 4 != CREATE: continue
    pc = k * 4; ins = list(md.disasm(b[pc - 0x80:pc], pc - 0x80)); tid = None; mode = False; page = {}
    for i in ins:
        if i.mnemonic == 'mov' and i.op_str.startswith('w2, #'): tid = int(i.op_str.split('#')[1], 16)
        elif i.mnemonic in ('mov', 'csel', 'ldr', 'ldrh', 'ldrb', 'cset') and i.op_str.startswith('w2,'): tid = i.mnemonic + ' ' + i.op_str
        if i.mnemonic == 'adrp': r, v = i.op_str.split(', #'); page[r] = int(v, 16)
        if i.mnemonic == 'ldrb' and '#0x918]' in i.op_str: mode = True
        if i.mnemonic == 'ldrb' and '#0x30]' in i.op_str: mode = mode or 'maybe'
    t = '%#x %s' % (tid, names.get(tid, '?')) if isinstance(tid, int) else str(tid)
    print('%#x  type %-44s mode byte read nearby: %s' % (pc, t, mode))
