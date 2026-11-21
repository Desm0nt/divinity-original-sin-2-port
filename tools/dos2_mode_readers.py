"""[DOS2] Every reader of the engine's input mode byte main+0x2bb8918 in main.raw (build 4979b200), for base A
(DM_DOS2_TOUCH_BASE=console: dm/host/hle_dos2_touch_base.c, docs/dos2/TOUCH_MODES_PLAN.md "Основа A: касание на консольном режиме движка").

The byte is 1 for the controller UI (SWITCH / HYBRID) and 0 for the keyboard-and-mouse UI (TOUCH). Base A must give TOUCH the console
INPUT mode without changing what the PC windows and the TOUCH gestures run, so the question is: which readers are the input layer and
which are UI / game code? This tool finds every `ldrb w, [x, #0x918]` whose base register holds the page main+0x2bb8000 (an `adrp`
straight to it, or a copy of such a register), the indirect reads (`add x, x, #0x918`), any store, and groups them:

  input   the listener filter of the input manager's dispatch (main+0x843f80: two copies of "who gets this event") - the ONLY readers
          between the raw records and the listeners: the pad poll, InjectInput, ProcessInputEvent, the bindings' reader have none
  engine  the other readers below main+0xd00000 (UI manager: focus of the PC UI, a mouse-only helper, "is a controller active"; the client's
          own input listener main+0xc05c80; the window's letterbox main+0xc0b524)
  game    everything from main+0xd00000 up (eoc): the per-player UI set builder main+0x10c07ac, window type choices next to CreateUIObject
          main+0xaebbf0, the PC / console text tables (main+0x2a32xxx: key names in hints), the classes' own branches

and checks what the host relies on (exit 1 if not): the input group is exactly main+0x8440b0 and main+0x844248 (= TB_PIN_A / TB_PIN_B of
hle_dos2_touch_base.c), there is no store to the byte but the NX input init main+0x8a8eb8, neither pinned word uses x18.

  python tools/dos2_mode_readers.py <main.raw> [--list]      --list: every reader, with the instructions that use the value
"""
import collections, re, struct, sys
from pathlib import Path
import capstone

PAGE, OFF, TEXT_END, GAME = 0x2bb8000, 0x918, 0x1bf7000, 0xd00000
FILTER = (0x843f80, 0x844530)        # the dispatch that holds both copies of the listener filter
INPUT_LAYER = (0x840000, 0x8b0000)   # input manager + NX input (filter, InjectInput 0x847690, ProcessInputEvent 0x845350, the pad poll 0x8a7530)
CREATE = 0xaebbf0                    # CreateUIObject(mgr, layer, type, flags, x4, player)


def adrp(w, pc):
    if (w & 0x9f000000) != 0x90000000: return None
    imm = (((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)
    if imm & (1 << 20): imm -= 1 << 21
    return (pc & ~0xfff) + (imm << 12), w & 31


def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read(); words = struct.unpack_from('<%dI' % (TEXT_END // 4), data, 0)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.detail = True
    one = lambda i: next(md.disasm(data[i * 4:i * 4 + 4], i * 4), None)

    def source(i, reg, depth=0):
        """what the base register of the read at word i holds: 'page' (adrp to the page, maybe through register copies), else a note"""
        for j in range(i - 1, max(i - 3000, 0), -1):
            a = adrp(words[j], j * 4)
            if a and a[1] == reg: return 'page' if a[0] == PAGE else 'other page'
            ins = one(j)
            if ins is None: continue
            try: wr = {ins.reg_name(r) for r in ins.regs_access()[1]}
            except capstone.CsError: continue
            if 'x%d' % reg in wr or 'w%d' % reg in wr:
                m = re.fullmatch(r'x%d, x(\d+)' % reg, ins.op_str) if ins.mnemonic == 'mov' else None
                if m and depth < 4: return source(j, int(m.group(1)), depth + 1)
                # a callee-saved register restored / a loop counter met first: the linear walk back left the path the read is on
                return 'unresolved (%s %s at %#x)' % (ins.mnemonic, ins.op_str, j * 4)
        return 'unresolved'

    readers, doubt, stores, indirect = [], [], [], []
    for i, w in enumerate(words):
        if (w & 0xfffffc00) == 0x39646000:                      # ldrb wT, [xN, #0x918]
            s = source(i, (w >> 5) & 31)
            (readers if s == 'page' else doubt).append((i * 4, s))
        elif (w & 0xfffffc00) == 0x39246000 and source(i, (w >> 5) & 31) == 'page': stores.append(i * 4)   # strb wT, [xN, #0x918]
        elif (w & 0xfffffc00) == 0x91246000 and source(i, (w >> 5) & 31) == 'page': indirect.append(i * 4)  # add xD, xN, #0x918
    kind = collections.Counter(); grp = collections.Counter(); rows = []
    for off, _ in readers + doubt:
        nxt = list(md.disasm(data[off + 4:off + 4 + 4 * 48], off + 4))
        use = ' | '.join('%s %s' % (x.mnemonic, x.op_str) for x in nxt[:3])
        g = 'input' if FILTER[0] <= off < FILTER[1] else 'engine' if off < GAME else 'game'
        tabs = [x for x in nxt[:6] if x.mnemonic == 'adrp' and x.op_str.endswith('#0x2a32000')]
        make = [x for x in nxt if x.mnemonic == 'bl' and x.op_str == '#%#x' % CREATE]
        sel = [x for x in nxt[:12] if x.mnemonic in ('csel', 'csinc', 'cset', 'csetm')]
        k = 'text table (PC / console strings)' if tabs else 'UI object made after it (window type / flags choice)' if make and sel else \
            'value select (csel / cset)' if sel else 'branch (cbz / cbnz / cmp)'
        grp[g] += 1; kind[k] += g == 'game'; rows.append((off, g, k, use))
    bad = 0

    def t(ok, what):
        nonlocal bad
        bad += not ok; print('%s  %s' % ('ok    ' if ok else 'FAILED', what))
    print('main+%#x: %d read(s) `ldrb w,[x,#%#x]` through an adrp to main+%#x, %d more where the walk back does not reach the adrp (callee-saved register / loop: '
          'counted as readers), %d indirect (add x,x,#%#x), %d store(s)' % (PAGE + OFF, len(readers), OFF, PAGE, len(doubt), len(indirect), OFF, len(stores)))
    print('groups: input %d, engine (below main+%#x) %d, game %d' % (grp['input'], GAME, grp['engine'], grp['game']))
    for k, n in kind.most_common(): print('  game: %4d  %s' % (n, k))
    print('engine-level readers:')
    for off, g, k, use in rows:
        if g != 'game': print('  main+%#9x  %-6s %s' % (off, g, use))
    for off in indirect: print('  main+%#9x  indirect: %s' % (off, ' | '.join('%s %s' % (x.mnemonic, x.op_str) for x in md.disasm(data[off:off + 12], off))))
    inp = sorted(off for off, g, _, _ in rows if g == 'input')
    t(inp == [0x8440b0, 0x844248], 'the listener filter reads the byte at exactly main+0x8440b0 and main+0x844248 (found: %s)' % ' '.join('%#x' % o for o in inp))
    lay = sorted(off for off, _, _, _ in rows if INPUT_LAYER[0] <= off < INPUT_LAYER[1])
    t(lay == inp, 'no other reader in the input manager / NX input code main+%#x..%#x (InjectInput, ProcessInputEvent, the bindings' reader main+0x84ebd0, the pad poll)' % INPUT_LAYER)
    t(not stores and not [o for o in indirect if o < GAME], 'no store to the byte through its page and no indirect access in engine code (the NX input init main+0x8a8eb8 writes it through its struct: %s)'
      % ('strb w19,[x8,#0x30]' if words[0x8a8eb8 // 4] == 0x3900c113 else 'WORD DIFFERS'))
    t(words[0x8a8eb8 // 4] == 0x3900c113, 'main+0x8a8eb8 is `strb w19,[x8,#0x30]` (the only writer; UI mode TOUCH stores 0 there, hle_dos2_touch.c)')
    for off in (0x8440b0, 0x844248):
        ins = one(off // 4); pin = 0x52800020 | (words[off // 4] & 31); p = next(md.disasm(struct.pack('<I', pin), off))
        t(not re.search(r'\b[xw]18\b', ins.op_str) and p.mnemonic == 'mov' and p.op_str.endswith('#1'), 'main+%#x %s %s -> %#010x %s %s (no x18)' % (off, ins.mnemonic, ins.op_str, pin, p.mnemonic, p.op_str))
    c = Path(__file__).resolve().parent.parent / 'dm' / 'host' / 'hle_dos2_touch_base.c'
    if c.exists():
        src = c.read_text(encoding='utf-8')
        t('TB_PIN_A = 0x8440b0' in src and 'TB_PIN_B = 0x844248' in src and 'TB_MODE_PAGE = %#x' % PAGE in src, 'hle_dos2_touch_base.c pins the same two words (TB_PIN_A / TB_PIN_B) and checks the same page')
    if '--list' in sys.argv:
        for off, g, k, use in rows: print('main+%#9x  %-6s %-52s %s' % (off, g, k, use))
        for off, s in doubt: print('main+%#9x  walk back: %s' % (off, s))
    print('FAILED' if bad else 'all checks passed')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
