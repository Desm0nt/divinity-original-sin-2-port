"""[DOS2] Static self-test of dm/host/hle_dos2_touchui.c against main.raw (docs/dos2/TOUCH_UI_PLAN.md, TU-02): every site the host compares
at run time is read out of the C source (the enum offsets, the k_* word arrays, the site table) and compared with the file; no site may
use x18 (the loader rewrites those instructions, such a word never matches on the phone); the two rodata literals are checked; the type
table from tools/dos2_ui_types.py must give GUI/mainMenu_c.swf for types 0x48 and 0x57; the string allocation rule of the registrar
((len + 16) & ~15) says whether each substitution is written in place; optionally a TouchUI.pak is checked for the movies.

  python tools/dos2_touchui_check.py <main.raw> [TouchUI.pak]
"""
import re, struct, subprocess, sys
from pathlib import Path
import capstone

root = Path(__file__).resolve().parent.parent
src = ''.join((root / 'dm/host' / f).read_text(encoding='utf-8') for f in ('hle_dos2_touchui.c', 'hle_dos2_touchui_ei.c', 'hle_dos2_touchui_hud.c', 'hle_dos2_touchui_ie.c', 'hle_dos2_touchui_route.c', 'hle_dos2_touch_world.c', 'hle_dos2_touchui_vis.c', 'hle_dos2_touch_gaps.h', 'hle_dos2_touch_gaps.c', 'hle_dos2_touch_kit.c'))   # TU-06 / TU-07 sites too; the dmTouch kit's (DK_)
main = open(sys.argv[1], 'rb').read()
enum = {k: int(v, 16) for k, v in re.findall(r'\b((?:T[UGW]|EI|HU|IE|RT|VS|DK)_[A-Z0-9_]+) = (0x[0-9a-f]+)', src)}
arr = {}
for name, n, body in re.findall(r'static const uint32_t (k_\w+)\[(\d+)\] = \{(.*?)\};', src, re.S):
    words = [int(w, 16) for w in re.findall(r'0x[0-9a-f]{8}', re.sub(r'/\*.*?\*/', '', body, flags=re.S))]
    assert len(words) == int(n), (name, len(words), n); arr[name] = words
sites = re.findall(r'\{((?:T[UGW]|EI|HU|IE|RT|VS|DK)_[A-Z0-9_]+), (\d+), (k_\w+)\}', src) + [('TU_IGGY_NOMETHOD', '7', 'k_iggy_nomethod')]
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); bad = 0
for off, n, k in sites:
    o = enum[off]; want = arr[k]; got = list(struct.unpack_from('<%dI' % int(n), main, o))
    x18 = [i.mnemonic + ' ' + i.op_str for i in md.disasm(main[o:o + 4 * int(n)], o) if re.search(r'\b[xw]18\b', i.op_str)]
    ok = got == want and int(n) == len(want) and not x18
    bad += not ok
    print('%-18s main+%#9x %2s words  %s%s' % (off, o, n, 'ok' if ok else 'DIFFERS', '  uses x18: %s' % x18 if x18 else ''))
for off, lit in (('TU_PATH_C', b'GUI/mainMenu_c.swf\0'), ('TU_PAK_GLOB', b'*.pak\0')):
    ok = main[enum[off]:enum[off] + len(lit)] == lit; bad += not ok
    print('%-18s main+%#9x literal %r  %s' % (off, enum[off], lit[:-1].decode(), 'ok' if ok else 'DIFFERS'))
tab = subprocess.run([sys.executable, str(root / 'tools/dos2_ui_types.py'), 'sw', sys.argv[1]], capture_output=True, text=True).stdout
types = {int(m.group(2), 16): (m.group(1), int(m.group(3), 16)) for m in re.finditer(r'^(GUI/\S+)\s+id (0x[0-9a-f]+)\s+creator (0x[0-9a-f]+)', tab, re.M)}
# the default table k_def (TU-04): every type must have the tail it expects; a `_c` id with a PC twin (TU-05) must share the twin's creator
# function (one class) and the twin must be the PC movie of the same name
# three tables since 15.11 (console touch): k_def = the default (main menu D only), k_ipad = DM_DOS2_TOUCHUI_SET=ipad, k_hint = DM_DOS2_TOUCHUI_SET=hints
tabs = {}
for tname, body in re.findall(r'static const tu_def (k_\w+)\[\] = \{(.*?)\n\};', src, re.S):
    tabs[tname] = [(int(a, 16), int(b, 16), mv, ex) for a, b, mv, ex in re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+|0), "(\w+)", "([^"]+)"\}', body)]
assert sorted(tabs) == ['k_def', 'k_hint', 'k_ipad', 'k_touch'] and len(tabs['k_touch']) >= 6 and all(d[2].endswith('_p') for d in tabs['k_touch']) and [d[2] for d in tabs['k_def']] == ['mainMenu_d', 'mainMenu_d'] and len(tabs['k_ipad']) >= 25 and len(tabs['k_hint']) >= 8, {k: len(v) for k, v in tabs.items()}
assert not any(d[2].endswith('_t') for d in tabs['k_def'] + tabs['k_hint'] + tabs['k_touch']), 'an iPad movie in the default / hints / touch table'
block = [int(x, 16) for x in re.search(r'static const uint16_t k_block\[\] = \{(.*?)\};', src).group(1).split(',')]
for t in block:   # every blocker is a registered menu type
    ok = t in types; bad += not ok
    if not ok: print('k_block type %#x is not registered  UNEXPECTED' % t)
print('k_block: %d console menu types, all registered: %s' % (len(block), ' '.join('%#x' % t for t in block)))
for tid, pc, movie, expect in tabs['k_def'] + tabs['k_ipad'] + tabs['k_hint']:
    old, cr = types.get(tid, (None, None)); ok = old == expect; note = ''
    if pc:
        tw = types.get(pc, (None, None)); same = tw[1] == cr and tw[0] == expect.replace('_c.swf', '.swf')
        ok = ok and same; note = ' -> PC twin %#x %s, creator %s' % (pc, tw[0], 'shared %#x' % cr if same else 'DIFFERENT')
    bad += not ok
    print('type %#5x: %-28s -> GUI/%s.swf%s  %s' % (tid, old, movie, note, 'ok' if ok else 'UNEXPECTED (want %s)' % expect))
if len(sys.argv) > 2:
    sys.path.insert(0, str(root / 'tools')); import lspk
    names = {e[0] for e in lspk.Pak(sys.argv[2]).entries}
    want = sorted({'Public/Game/GUI/%s.swf' % mv for _, _, mv, _ in tabs['k_def']})
    miss = [w for w in want if w not in names]; bad += bool(miss)
    print('%s: %d file(s), %d movie(s) of the default table %s' % (sys.argv[2], len(names), len(want), 'all present' if not miss else 'MISSING: ' + ' '.join(miss)))
    for tname, env in (('k_ipad', 'ipad'), ('k_hint', 'hints')):   # informative: which experiment sets this package can serve
        w2 = sorted({'Public/Game/GUI/%s.swf' % mv for _, _, mv, _ in tabs[tname]}); m2 = [w for w in w2 if w not in names]
        print('  DM_DOS2_TOUCHUI_SET=%s: %s' % (env, 'served by this package' if not m2 else 'not in this package (%d of %d movies missing)' % (len(m2), len(w2))))
# TU-06: the vtable slots the host relies on (relocations of main: .rela.dyn R_AARCH64_RELATIVE), FlashPlayer main+0x29c3ba0, UIObject base main+0x2999a18
rel = {}
for o in range(0x1bf7060, 0x1bf7060 + 0x231d38, 0x18):
    off, info, add = struct.unpack_from('<QQq', main, o)
    if info & 0xffffffff == 0x403: rel[off] = add
for what, slot, want in (('FlashPlayer vt+0x58 Invoke0', 0x29c3ba0 + 0x58, enum['EI_INV0']), ('FlashPlayer vt+0x88 Invoke6', 0x29c3ba0 + 0x88, enum['EI_INV6']),
                         ('FlashPlayer vt+0x90 InvokeArgs', 0x29c3ba0 + 0x90, enum['EI_INVN']), ('FlashPlayer vt+0x130 EI drain', 0x29c3ba0 + 0x130, enum['EI_DRAIN']),
                         ('UIObject vt[0] OnFunctionCalled', 0x2999a18, enum['EI_BASE']),
                         ('panelSelect_c vt[0] OnFunctionCalled', enum['VS_PS_VT'], enum['VS_OPENPANEL']),
                         ('playerInfo 0x26 vt[0] (touch gaps)', enum['TG_PI_VT'], enum['TG_PI_CALLED'])):
    ok = rel.get(slot) == want; bad += not ok
    print('%-34s main+%#x -> %#x  %s' % (what, slot, rel.get(slot, -1), 'ok' if ok else 'DIFFERS'))
for a in ('EI_DRAIN', 'EI_INV0', 'EI_INV1', 'EI_INV2', 'EI_INV3', 'EI_INV4', 'EI_INV5', 'EI_INV6', 'EI_INVN', 'HU_CREATE_ALL', 'HU_DESTROY_ALL', 'IE_REG', 'RT_DISPATCH'):   # hooked prologues: no PC-relative instruction in the 4 moved words
    pcrel = [i.mnemonic for i in md.disasm(main[enum[a]:enum[a] + 16], enum[a]) if i.mnemonic in ('adr', 'adrp', 'b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or i.mnemonic.startswith('b.') or (i.mnemonic.startswith('ldr') and '#' in i.op_str and '[' not in i.op_str)]
    bad += bool(pcrel); print('%-18s prologue moved to a trampoline: %s' % (a, 'PC-RELATIVE ' + str(pcrel) if pcrel else 'position independent, ok'))
ok = main[0x1f52d87:0x1f52d87 + 6] == bytes(int(x, 16) for x in re.search(r'k_vs_kinds\[VS_PANELS\] = \{(.*?)\}', src).group(1).split(','))   # the radial menu's panel id table (hle_dos2_touchui_vis.c)
bad += not ok; print('panel id table     main+0x1f52d87 6 bytes  %s' % ('ok' if ok else 'DIFFERS'))
# the column state rides on the invoke name "setActiveInCombat" (hle_dos2_touchui_vis.c vs_ei_colpoll; the engine must know that name for bottomBar)
ok = b'\x00setActiveInCombat\x00' in main and '"setActiveInCombat"' in src; bad += not ok
print('invoke name        "setActiveInCombat" in main.raw (the column state of bottomBar_d)  %s' % ('ok' if ok else 'MISSING'))
print('FAILED' if bad else 'all sites match main.raw')
sys.exit(1 if bad else 0)
