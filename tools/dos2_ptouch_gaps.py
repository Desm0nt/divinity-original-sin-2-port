"""[DOS2] UI mode TOUCH: iPad functions whose ExternalInterface (EI) the Switch engine's PC class does not handle - the working equivalents
in the `_p` movies (docs/dos2/TOUCH_MODES_PLAN.md "Implemented: iPad functions without a Switch EI - equivalents"; the host half is
dm/host/hle_dos2_touch_gaps.c). Static only: nothing here was played or run on a phone.

How a gap is told from a working call: an EI name works only when the OnFunctionCalled chain of the window's engine class compares it -
vtable slot 0 (main.raw 4979b200), its string compares (strcmp main+0x1bf4600 / +0x1bf43d0), then the functions it branches to (sub-handlers
such as main+0x13f5860 "offerJunk" of trade, base classes 0x10a5750 / 0xae2780). Some creators construct a derived class inline: their
vtable is the one the CREATOR stores (playerInfo 0x26: main+0x29eb768, dialog 0xe: main+0x29eb5b0), not the constructor's. A name that is a
string elsewhere in main is not enough; a name that is no string at all is dead for sure. ENGINE below is checked against main.raw.

Movie patches (applied AFTER the movie's own pass: tools/dos2_ppanel_trade.py builds trade_p / containerInventory_p; this module re-points
methods of the built movie, every body it replaces stays callable under a dmGap* name):
  trade_p                 trade_1.onRightDown -> nothing (the PC armed "right mouse up on the window = cancel": the touch long press is a
                          right click, so a long press anywhere on the trade window CLOSED the trade - the iPad has no such thing);
                          trade_1.onContainerRightClick -> nothing (on an item the PC sent itemRightClick - not in main.raw at all; on an empty
                          cell it sent cancel = close). The iPad's long press in trade is the tooltip: the host's short long press (hover)
                          already shows it, and a long press stays the tooltip.
  containerInventory_p    inventory_1.onContainerRightUp -> the original, then onContextMenuInputUp(): the long press opens the item's context
                          menu through the EI the container classes DO handle (openContextMenu(handle, x, y): types 9 and 0x25), instead of
                          rightClickItem (not in main.raw) - also when the engine does not deliver the "IE ContextMenu" input event to the
                          movie in mode 0 (the PC route). onContextMenuInputUp is guarded: a second call within CTX_GUARD_MS (the other route
                          of the same long press) does nothing.

  python -I tools/dos2_ptouch_gaps.py check <Switch Game.pak | dir of the PC movies> [main.raw]
  python -I tools/dos2_ptouch_gaps.py build <src> <out dir>          the patched trade_p / containerInventory_p only
Hook: tools/dos2_pchud_touch.py calls install() at its end (after tools/dos2_ppanel_trade.py's); its build() then returns the patched movies
and its check() runs check() here too (so the `touch` set of tools/dos2_touchui_pak.py gets them).
"""
import hashlib, os, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all
import dos2_swf_graft as gr
import dos2_ppanel_trade as ppt

CTX_GUARD_MS = 400   # one long press = at most one context menu (the movie's right mouse up and the engine's IE ContextMenu may both come)

# ---- engine facts (main.raw 4979b200 offsets)
ENGINE = [
    # (what, creator that stores the vtable or None, vtable, its slot 0 = OnFunctionCalled, names its chain compares, names it must NOT compare)
    ('trade 0x2e', None, 0x29f1260, 0x1349640, ['itemClick', 'acceptOffer', 'clearOffers', 'balanceClicked', 'cancel', 'offerJunk'], []),
    ('container 0x09', None, 0x29ee470, 0x12c4b60, ['openContextMenu', 'doubleClickItem', 'takeAll'], []),
    ('container 0x25 (pickpocket)', None, 0x29f27e0, 0x13aee00, ['openContextMenu', 'doubleClickItem'], []),
    ('playerInfo 0x26', 0x10b5f00, 0x29eb768, 0x1237b10, ['charSel', 'centerCamOnCharacter', 'piAddToGroupUnder', 'piDetachUnder'], ['openCharInventory']),
    ('dialog 0x0e', 0x10b4be0, 0x29eb5b0, 0x1237140, ['toggleInv', 'QuestionPressed', 'TradeButtonPressed', 'StopListening'], []),
    ('PC hot bar 0x28 (the column / inventory route of the host)', None, 0x29eb1b0, 0x122fdb0, ['hotbarBtnPressed'], []),
]
NOT_IN_MAIN = ['itemRightClick', 'itemDoubleClick', 'splitItem', 'rightClickItem', 'middleClickItem']   # the PC movies' calls the patches replace
STRCMP = (0x1bf4600, 0x1bf43d0)               # the two string compares the handlers call (imports)


# ---------------------------------------------------------------- movie patches
def _raw_wrap(c, name, keep, lines):
    """trait `name` of Cls c -> a new body (raw lines, parameters as the original); the original kept as public `keep`"""
    abc = c.mv.abc; t = abc.trait(c.ci, name); assert t is not None and t.kind & 15 == 1, '%s::%s' % (c.name, name)
    assert abc.trait(c.ci, keep) is None, 'already patched: %s::%s' % (c.name, keep)
    orig = abc.methods[t.b]; m = c.add(name, len(orig.params), lines, like=orig)
    old = t.b; t.b = m; c.inst['traits'].append(Trait(c.P(keep), 1, 0, old))


def trade(data):
    """built trade_p.swf -> trade_p.swf with the long press neutral (no close, no dead call)"""
    mv = ppt.Movie(data); t = ppt.Cls(mv, 'trade_fla.trade_1')
    t.wrap('onRightDown', 'dmGapRdOrig', lambda b: None)             # never arms closeUIOnClick (right mouse up = cancel)
    t.wrap('onContainerRightClick', 'dmGapRcOrig', lambda b: None)   # no itemRightClick (dead) / cancel (close)
    return mv.done()


def container(data):
    """built containerInventory_p.swf -> long press = openContextMenu (handled by both container classes), at most once per press"""
    mv = ppt.Movie(data); inv = ppt.Cls(mv, 'containerInventory_fla.inventory_1'); P = mv.P
    gt = mv.abc.pub('getTimer', 'flash.utils')
    # onContextMenuInputUp(): Boolean - guarded; locals: 1 = now
    _raw_wrap(inv, 'onContextMenuInputUp', 'dmGapCtxOrig', [
        ('getlocal0',), ('pushscope',), ('pushbyte', 0), ('convert_d',), ('setlocal', 1),
        ('findpropstrict', gt), ('callproperty', gt, 0), ('convert_d',), ('setlocal', 1),
        ('getlocal', 1), ('getlocal0',), ('getproperty', P('dmCtxT')), ('subtract',), ('pushshort', CTX_GUARD_MS), ('iflt', 'skip'),
        ('getlocal0',), ('callproperty', P('dmGapCtxOrig'), 0), ('convert_b',), ('dup',), ('iffalse', 'ret'),
        ('getlocal0',), ('getlocal', 1), ('setproperty', P('dmCtxT')),
        ('L', 'ret'), ('returnvalue',),
        ('L', 'skip'), ('pushfalse',), ('returnvalue',)])
    # onContainerRightUp(e): the original (its listener bookkeeping), then the context menu the engine handles
    def ru(b):
        b.orig(); b.emit(('getlocal0',), ('callproperty', b.P('onContextMenuInputUp'), 0), ('pop',))
    inv.wrap('onContainerRightUp', 'dmGapRuOrig', ru)
    return mv.done()


PATCH = {'trade_p.swf': trade, 'containerInventory_p.swf': container}


def build(made):
    """{name: bytes} of the built movies -> the same dict with PATCH applied"""
    out = dict(made)
    for n, f in PATCH.items():
        if n in out: out[n] = f(out[n])
    return out


_host = None
def install(host):
    """called at the end of tools/dos2_pchud_touch.py (after tools/dos2_ppanel_trade.py's install)"""
    global _host
    _host = host
    h_build, h_check = host.build, host.check
    def build_(src): return build(h_build(src))
    def check_(src): return h_check(src) + check(src)
    host.build, host.check = build_, check_


# ---------------------------------------------------------------- checks
def _main(main_raw=None):
    for p in [main_raw] + [d / 'work' / 'exefs-update' / 'main.raw' for d in Path(__file__).resolve().parents]:
        if p and os.path.exists(str(p)): return open(str(p), 'rb').read()
    return None


def _rel(b):
    rel = {}
    for o in range(0x1bf7060, 0x1bf7060 + 0x231d38, 0x18):
        off, info, add = struct.unpack_from('<QQq', b, o)
        if info & 0xffffffff == 0x403: rel[off] = add
    return rel


def _end(b, f):
    """first ret / b past the furthest branch target inside the function"""
    e = f; far = f
    while e < f + 0x8000:
        x = struct.unpack_from('<I', b, e)[0]; t = None
        if x & 0xff000010 == 0x54000000 or x & 0x7e000000 == 0x34000000:
            o = (x >> 5) & 0x7ffff; o = o - (1 << 19) if o & (1 << 18) else o; t = e + 4 * o
        elif x & 0x7e000000 == 0x36000000:
            o = (x >> 5) & 0x3fff; o = o - (1 << 14) if o & (1 << 13) else o; t = e + 4 * o
        elif x & 0xfc000000 == 0x14000000:
            o = x & 0x3ffffff; o = o - (1 << 26) if o & (1 << 25) else o; t = e + 4 * o
            if not (f <= t < f + 0x8000) and e >= far: return e + 4
        if t is not None and f <= t < f + 0x8000: far = max(far, t)
        if x == 0xd65f03c0 and e >= far: return e + 4
        e += 4
    return e


def _scan(b, f, e):
    """one function: C strings it builds with adrp + add (within 32 instructions), whether it calls a string compare, the functions it
    branches to (bl / b out of [f, e))"""
    page = {}; out = set(); cmp_ = False; outs = []
    for pc in range(f, e, 4):
        x = struct.unpack_from('<I', b, pc)[0]
        if x & 0x9f000000 == 0x90000000:
            imm = ((x >> 5) & 0x7ffff) << 2 | (x >> 29) & 3
            if imm & (1 << 20): imm -= 1 << 21
            page[x & 31] = ((pc & ~0xfff) + (imm << 12), pc)
        elif x & 0xffc00000 == 0x91000000:
            p = page.get((x >> 5) & 31)
            if p and pc - p[1] < 0x80:
                a = p[0] + ((x >> 10) & 0xfff); z = b.find(b'\0', a, a + 64)
                out.add(a)
                if z > a: out.add(b[a:z].decode('latin1'))
        elif x & 0xfc000000 in (0x94000000, 0x14000000):
            o = x & 0x3ffffff; o = o - (1 << 26) if o & (1 << 25) else o; t = pc + 4 * o
            if t in STRCMP: cmp_ = True
            elif not (f <= t < e) and 0x800000 <= t < 0x1e00000: outs.append(t)
    return out, cmp_, outs


def _chain(b, f, depth=4):
    """the names an OnFunctionCalled chain compares: the function, then every function it branches to that compares strings too
    (sub-handlers, base classes), depth-limited"""
    names = set(); seen = {f}; todo = [(f, 0)]
    while todo:
        g, d = todo.pop()
        got, cmp_, outs = _scan(b, g, _end(b, g))
        if g != f and not cmp_: continue
        names |= {n for n in got if isinstance(n, str)}
        if d < depth: todo += [(t, d + 1) for t in outs if t not in seen and not seen.add(t)]
    return names


def check_engine(b, say):
    rel = _rel(b)
    for what, creator, vt, fn, has, hasnt in ENGINE:
        got = rel.get(vt)
        stored = creator is None or vt in _scan(b, creator, _end(b, creator))[0]
        say(got == fn and stored, '%s: vtable main+%#x%s, slot 0 = main+%#x (OnFunctionCalled)' % (what, vt, ' stored by its creator main+%#x' % creator if creator else '', got or 0))
        names = _chain(b, fn)
        say(all(n in names for n in has), '%s: its chain compares %s' % (what, ' '.join(has)))
        if hasnt: say(not any(n in names for n in hasnt), '%s: its chain (sub-handlers, bases) compares none of %s' % (what, ' '.join(hasnt)))
    miss = [n for n in NOT_IN_MAIN if b'\0' + n.encode() + b'\0' in b]
    say(not miss, 'not a string of main.raw at all (dead on every class): %s%s' % (' '.join(NOT_IN_MAIN), '' if not miss else ' - FOUND: ' + ' '.join(miss)))


def _ei_names(abc):
    import dos2_ui_audit as au
    return {n for rec in au.scan(abc).values() for n, _ in rec['ei'] if n != '<dynamic>'}


def check(src, main_raw=None):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    print('-- tools/dos2_ptouch_gaps.py: iPad functions without a Switch EI')
    b = _main(main_raw)
    if b is None: print('FAIL main.raw not found'); return 1
    check_engine(b, say)
    made = ppt.build(src); out = build(made)
    for o in PATCH:
        data, before, after = src[ppt.SRC[o]], made[o], out[o]
        print('%s: %d B -> %d B, sha256 %s' % (o, len(before), len(after), hashlib.sha256(after).hexdigest()[:16]))
        s = Swf(after); (_, _, Bc), = s.abcs(); (_, _, B0), = Swf(before).abcs()
        errs = verify_all(Bc)
        for who, why in errs: print('       %s: %s' % (who, why))
        say(not errs, 'ABC verifies: %d method bodies' % len(Bc.bodies))
        say(Swf(Swf(after).write()).body() == s.body(), 'the movie parses and round-trips')
        g = gr.check(data, after, quiet=True); say(g == ['header / tail differ'], 'every original tag of the PC movie kept%s' % ('' if g == ['header / tail differ'] else ': ' + str(g)))
        e0, e1 = _ei_names(B0), _ei_names(Bc)
        say(e1 <= e0, 'no new ExternalInterface name (only fewer reachable): %s' % (' '.join(sorted(e1 - e0)) or 'none new'))
        own = {v: k for k, v in Bc.owners().items()}
        def dis(cls, m): return '\n'.join(Bc.dis(Bc.methods[own[cls + '::' + m]].body)) if cls + '::' + m in own else ''
        if o == 'trade_p.swf':
            c = 'trade_fla.trade_1'
            for m, keep in (('onRightDown', 'dmGapRdOrig'), ('onContainerRightClick', 'dmGapRcOrig')):
                t = dis(c, m)
                say(bool(t) and 'callprop' not in t and 'ExternalInterface' not in t and Bc.trait(Bc.class_index(c), keep) is not None,
                    '%s::%s does nothing now: no cancel (close), no itemRightClick (the PC body kept as %s)' % (c, m, keep))
            say(Bc.trait(Bc.class_index(c), 'dmF1Orig') is not None and Bc.trait(Bc.class_index(c), 'dmUpOrig') is not None,
                'the passes of tools/dos2_ppanel_trade.py (dmF1Orig layout, dmUpOrig double tap) are still under it')
        else:
            c = 'containerInventory_fla.inventory_1'
            t = dis(c, 'onContextMenuInputUp'); say('dmGapCtxOrig' in t and 'getTimer' in t and str(CTX_GUARD_MS) in t and 'returnvalue' in t,
                                                    'inventory_1.onContextMenuInputUp: the original (openContextMenu) at most once in %d ms, Boolean kept' % CTX_GUARD_MS)
            t = dis(c, 'onContainerRightUp'); say('dmGapRuOrig' in t and 'onContextMenuInputUp' in t, 'inventory_1.onContainerRightUp: the original, then onContextMenuInputUp (long press = context menu)')
            say('openContextMenu' in e1 and b'\0openContextMenu\0' in b, 'openContextMenu is in the movie and in main.raw (handled by both container classes, see above)')
    return bad


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == 'check': sys.exit(1 if check(ppt.sources(a[1]), a[2] if len(a) > 2 else None) else 0)
    if len(a) == 3 and a[0] == 'build':
        out = Path(a[2]); out.mkdir(parents=True, exist_ok=True); src = ppt.sources(a[1])
        for n, d in build(ppt.build(src)).items():
            if n in PATCH: (out / n).write_bytes(d); print('%s: %d B' % (out / n, len(d)))
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
