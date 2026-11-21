"""[DOS2] UI mode TOUCH: the PC tooltip window as ONE self-contained movie, tooltip_p.swf (docs/dos2/TOUCH_MODES_PLAN.md
"Подсказки через свой tooltip_p"). Static only: nothing here was played or run on a phone.

Why: the PC tooltip window (type 0x2c, GUI/tooltip.swf) imports the Flash library tooltipHelper_kb.swf. The Switch's library loader
(main+0x10cf360) never registers it, so Iggy made no player for the movie (no root, the crash main+0x111e6ec, 21.11). Registering it
globally at the UI start-up (d5013e4) broke console movies (Options -> Sound / Game, crash in Iggy main+0x3c56b4): a registered library is
seen by every movie, and tooltipHelper_kb reuses the console tooltipHelper.swf names (tt_*) with other methods and brings PC versions of
classes that 37+ console movies embed (TweensyZero, TweensyTimelineZero, easing.Sine, LS_Classes.scrollbar_text).
So this module builds tooltip_p.swf from the player's OWN Switch files (GUI/tooltip.swf + GUI/tooltipHelper_kb.swf of Game.pak; nothing
from the IPA) with everything it took from the library INSIDE the movie, under names no console movie or library uses:

  names    every class of the result that a console library could also define is renamed - the packages LS_Classes, fl.motion.easing,
           com.flashdynamix.motion -> dmTip.<package>, the library's top-level tt_* classes and IggyTween -> dmTip_<name>. The rename is
           done in the constant pools only (string entries equal to an old name, its package, or "<package>:<Class>" protected /
           private namespace names), so the bytecode of both movies stays byte for byte; the string literals the code hands to
           getDefinitionByName ("tt_tooltipObj", "tt_handle" ...) and the PlaceObject3 class names ("tt_tooltipObj") follow, as do the
           SymbolClass names. Safe because no trait and no display-list instance carries one of those names (checked).
  classes  tooltip.swf keeps its own DoABC block (24 classes); the library's DoABC tags (one class each, dependency order) follow it,
           without the 14 classes tooltip.swf already defines (same API, checked: one consistent set, tooltip.swf's) and without the
           library's test MainTimeline. Every class is defined once in the movie.
  symbols  the library's symbol characters (tt_* sprites, their shapes, bitmaps, texts) are copied with tools/dos2_swf_graft.py (ids
           remapped, timelines kept); a second SymbolClass tag binds them to their (renamed) classes.
  imports  the import of tooltipHelper_kb.swf is gone; the movie imports fonts_en.swf (as before) and the library's own two imports,
           uiElements.swf and LSClasses.swf (button icons, SkillSchoolIcons, ...), all three registered by the Switch's loader. No
           global registration anywhere: the console movies see exactly the libraries of the hybrid.
  host     dm/host/hle_dos2_touchui.c k_touch {0x2c, 0, "tooltip_p", "GUI/tooltip.swf"}; dm/host/hle_dos2_touchui_hud.c makes the window
           only when type 0x2c plays GUI/tooltip_p.swf (the "root before [+0x88]" guard and the init stub for a rootless object stay).

  python -I tools/dos2_ptooltip.py check <Switch Game.pak | dir of the Switch GUI movies> [main.raw]   every static check
  python -I tools/dos2_ptooltip.py build <src> <out dir>                                             tooltip_p.swf
  python -I tools/dos2_ptooltip.py names <src>                                                       the rename table
Hook: tools/dos2_pchud_touch.py calls install() at its end; build() then also returns tooltip_p.swf and check() runs check() here (so the
`touch` set of tools/dos2_touchui_pak.py packs it).
"""
import hashlib, os, re, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dos2_abc as A
from dos2_abc import Swf, verify_all, MN_QNAME, MN_QNAMEA, MN_MULTI, MN_MULTIA, MN_TYPE
import dos2_swf_graft as gr

ROOT = Path(__file__).resolve().parent.parent
GUI = 'Public/Game/GUI/'
SRC, LIB, OUT = 'tooltip.swf', 'tooltipHelper_kb.swf', 'tooltip_p.swf'
DOC, LIB_DOC = 'tooltip_fla.MainTimeline', 'tooltipHelper_kb_fla.MainTimeline'
PKGS = ('LS_Classes', 'fl.motion.easing', 'com.flashdynamix.motion')   # packages a console movie / library also defines
PRE_PKG, PRE_TOP = 'dmTip.', 'dmTip_'
TOP_EXTRA = ('IggyTween',)            # + every top-level tt_* class of the library
# the libraries the Switch's loader main+0x10cf360 registers (docs/dos2/TOUCH_MODES_PLAN.md "Подсказки (tooltip) в касании")
REGISTERED = ('fonts_en.swf', 'LSClasses.swf', 'uiElements.swf', 'tooltipHelper.swf', 'texture_lib.swf', 'containerInventory_lib.swf', 'consoleHints_c.swf')
IMPORTS = ('fonts_en.swf', 'uiElements.swf', 'LSClasses.swf')   # tooltip.swf's fonts_en + the library's own imports
BUILTIN_PKGS = ('flash.', 'http://adobe.com/AS3/2006/builtin', '__AS3__.vec', 'adobe.utils')
# the tooltip UI class of main.raw: vtable, its init (vt+0x70) and its invoke registrar (vt+0xe8, `ldr x8,[x8,#0x1a8]; adrp/add x2; mov w1,#i; blr x8`)
VT, VT_INIT, VT_INVOKES = 0x1d4cf70, 0x111e690, 0x111e780
T_IMPORT2, T_SYMCLASS, T_ABC, T_SPRITE, T_PLACE3 = 71, 76, 82, 39, 70


# ---------------------------------------------------------------- names
def class_names(abc): return [abc.mname(i['name']) for i in abc.instances]


class Renamer:
    def __init__(s, top):
        s.top = set(top)
    def full(s, name):
        """a class / symbol name -> its name in tooltip_p (unchanged when it is not one of ours)"""
        if name in s.top: return PRE_TOP + name
        pkg, _, c = name.rpartition('.')
        if pkg in PKGS: return PRE_PKG + pkg + '.' + c
        return name
    def string(s, t):
        """an ABC string entry -> its new text or None"""
        if t in s.top: return PRE_TOP + t
        if t in PKGS: return PRE_PKG + t
        m = re.fullmatch(r'(.+?)([:.])(\w+)', t)
        if m and m.group(1) in PKGS: return PRE_PKG + t
        return None
    def abc(s, abc):
        n = 0
        for i in range(1, len(abc.strings)):
            new = s.string(abc.strings[i].decode('utf-8', 'replace'))
            if new is not None: abc.strings[i] = new.encode('utf-8'); n += 1
        return n


def top_names(kb):
    out = set(TOP_EXTRA)
    for _, _, abc in kb.abcs():
        for nm in class_names(abc):
            if '.' not in nm and nm.startswith('tt_'): out.add(nm)
    return out


# ---------------------------------------------------------------- tag helpers
def symclass_entries(d):
    n = struct.unpack_from('<H', d, 0)[0]; p = 2; out = []
    for _ in range(n):
        cid = struct.unpack_from('<H', d, p)[0]; e = d.index(b'\0', p + 2); out.append((cid, d[p + 2:e].decode())); p = e + 1
    return out


def symclass_bytes(entries): return struct.pack('<H', len(entries)) + b''.join(struct.pack('<H', c) + n.encode() + b'\0' for c, n in entries)


def import_url(d): return d[:d.index(b'\0')].decode()


def import_bytes(url): return url.encode() + b'\0' + b'\x01\x00' + struct.pack('<H', 0)


def sprite_place3(d, fn):
    """a DefineSprite payload with every PlaceObject3 class name passed through fn (bytes without NUL -> bytes); (payload, changed)"""
    out = [d[:4]]; p = 4; changed = 0
    while p < len(d):
        h = struct.unpack_from('<H', d, p)[0]; hp = p; p += 2; code, ln = h >> 6, h & 0x3f; lng = ln == 0x3f
        if lng: ln = struct.unpack_from('<I', d, p)[0]; p += 4
        body = d[p:p + ln]; p += ln
        if code == T_PLACE3 and body[1] & 8:
            e = body.index(b'\0', 4); old = body[4:e]; new = fn(old)
            if new != old:
                body = body[:4] + new + body[e:]; changed += 1
                out.append(struct.pack('<HI', code << 6 | 0x3f, len(body)) + body if lng or len(body) >= 0x3f else struct.pack('<H', code << 6 | len(body)) + body)
                if code == 0: break
                continue
        out.append(d[hp:p])
        if code == 0: break
    return b''.join(out) + d[p:], changed


def place3_classes(swf):
    """[(sprite id, class name)] of every PlaceObject3 with a class name (sprites and the root timeline)"""
    out = []
    for code, d, _ in swf.tags:
        tags = [(struct.unpack_from('<H', d, 0)[0], c, b) for c, b in gr.subtags(d)] if code == T_SPRITE else [(0, code, d)]
        for owner, c, b in tags:
            if c == T_PLACE3 and b[1] & 8: out.append((owner, b[4:b.index(b'\0', 4)].decode()))
    return out


def instance_names(swf):
    out = set()
    for code, d, _ in swf.tags:
        tags = gr.subtags(d) if code == T_SPRITE else [(code, d)]
        for c, b in tags:
            if c in (26, 70):
                r = gr.place_parse(c, b)
                if r.get('name'): out.add(r['name'].rstrip(b'\0').decode('utf-8', 'replace'))
    return out


# ---------------------------------------------------------------- the build
def build(tt, kb):
    """tooltip.swf + tooltipHelper_kb.swf bytes -> (tooltip_p.swf bytes, report)"""
    T = Swf(tt); K = Swf(kb); rn = Renamer(top_names(K)); rep = dict(strings=0, place3=0, dropped=[], kept=[], symbols=0)
    # stage 1: tooltip.swf itself - its ABC / SymbolClass / PlaceObject3 class names renamed, the library import swapped for the library's imports
    (n0, pre0, a0), = T.abcs()
    rep['strings'] += rn.abc(a0); T.tags[n0][1] = pre0 + a0.write()
    for t in T.tags:
        if t[0] == T_SPRITE:
            t[1], c = sprite_place3(t[1], lambda b: rn.full(b.decode()).encode()); rep['place3'] += c
        elif t[0] == T_SYMCLASS: t[1] = symclass_bytes([(c, rn.full(n)) for c, n in symclass_entries(t[1])])
    imps = [k for k, t in enumerate(T.tags) if t[0] == T_IMPORT2]
    have = [import_url(T.tags[k][1]) for k in imps]
    assert LIB in have and all(not struct.unpack_from('<H', T.tags[k][1], T.tags[k][1].index(b'\0') + 3)[0] for k in imps), 'tooltip.swf imports: %s' % have
    at = imps[have.index(LIB)]
    T.tags[at:at + 1] = [[T_IMPORT2, import_bytes(u), False] for u in IMPORTS if u not in have]
    stage1 = T.write(compress=False)
    # stage 2: the library's symbols grafted (timelines kept), its classes appended as their own DoABC tags, a SymbolClass for them
    S = Swf(stage1); g = gr.Graft(S, K)
    syms = sorted(((cid, nm) for nm, cid in K.symbols().items() if cid), key=lambda x: x[0])
    for cid, _ in syms: g.copy(cid, freeze_sprites=False)
    for nd in g.new_defs:
        if nd[0] == T_SPRITE: nd[1], c = sprite_place3(nd[1], lambda b: rn.full(b.decode()).encode()); rep['place3'] += c
    g.finish()
    defined = set(class_names(a0)); new_abc = []
    for n, pre, abc in K.abcs():
        rep['strings'] += rn.abc(abc); names = class_names(abc)
        if names == [LIB_DOC]: rep['dropped'].append((LIB_DOC, 'the library\'s own test timeline')); continue
        if all(x in defined for x in names): rep['dropped'].append((names[0], 'defined by tooltip.swf')); continue
        assert not any(x in defined for x in names), names
        defined |= set(names); rep['kept'].append(names[0])
        new_abc.append([T_ABC, pre[:4] + names[0].replace('.', '/').encode() + b'\0' + abc.write(), True])
    k = next(i for i, t in enumerate(S.tags) if t[0] == T_ABC)
    S.tags[k + 1:k + 1] = new_abc
    ent = [(g.map[cid], rn.full(nm)) for cid, nm in syms]; rep['symbols'] = len(ent)
    k = next(i for i, t in enumerate(S.tags) if t[0] == T_SYMCLASS)
    S.tags[k:k] = [[T_SYMCLASS, symclass_bytes(ent), True]]
    rep['stage1'] = stage1; rep['rn'] = rn; rep['map'] = dict(g.map)
    return S.write(compress=True), rep


# ---------------------------------------------------------------- sources, hook
def _pak_read(path, names):
    import lspk
    pak = lspk.Pak(path); by = {e[0]: e for e in pak.entries}
    miss = [n for n in names if GUI + n not in by]
    if miss: sys.exit('%s: no %s' % (path, ' '.join(miss)))
    return {n: pak.read(by[GUI + n]) for n in names}


def sources(path):
    """the two movies + the registered libraries (for the name checks) of a Switch Game.pak or a directory of its GUI movies"""
    need = [SRC, LIB] + [n for n in REGISTERED]
    if os.path.isdir(path): return {n: (Path(path) / n).read_bytes() for n in need}
    return _pak_read(path, need)


def make(src): return {OUT: build(src[SRC], src[LIB])[0]}


_host = None
def install(host):
    """called at the end of tools/dos2_pchud_touch.py (after tools/dos2_ptouch_tips.py's install)"""
    global _host
    _host = host
    h_sources, h_build, h_check = host.sources, host.build, host.check
    def sources_(path):
        s = h_sources(path); s.update(sources(path)); return s
    def build_(src):
        out = h_build(src); out.update(make(src)); return out
    def check_(src): return h_check(src) + check(src)
    host.sources, host.build, host.check = sources_, build_, check_


# ---------------------------------------------------------------- checks
def _find(rel):
    for d in Path(__file__).resolve().parents:
        p = d / rel
        if p.exists(): return p
    return None


def defs(swf):
    """class names (ABC) | symbol names (SymbolClass, ExportAssets) of a movie"""
    out = set()
    for _, _, abc in swf.abcs(): out |= set(class_names(abc))
    out |= set(swf.symbols())
    for code, d, _ in swf.tags:
        if code == 56:
            n = struct.unpack_from('<H', d, 0)[0]; p = 2
            for _ in range(n): e = d.index(b'\0', p + 2); out.add(d[p + 2:e].decode('utf-8', 'replace')); p = e + 1
    return out


def refs(abc):
    """class references of a block: (kind, name) from supers, interfaces, trait types and the type / lookup operands of the code"""
    out = set()
    def add(i, why):
        if not i: return
        m = abc.mn[i]
        if m[0] in (MN_QNAME, MN_QNAMEA): out.add((why, abc.nsname(m[1]), abc.S(m[2]), abc.ns[m[1]][0]))
        elif m[0] in (MN_MULTI, MN_MULTIA): out.add((why, '<multi>', abc.S(m[1]), 0))
        elif m[0] == MN_TYPE: add(m[1], why); [add(x, why) for x in m[2]]
    for i in abc.instances:
        add(i['super'], 'super'); [add(x, 'iface') for x in i['ifaces']]
    for ts in [i['traits'] for i in abc.instances] + [c['traits'] for c in abc.classes] + [s['traits'] for s in abc.scripts] + [b.traits for b in abc.bodies]:
        for t in ts:
            if t.kind & 15 in (0, 6): add(t.a, 'type')
    for m in abc.methods:
        add(m.ret, 'type'); [add(p, 'type') for p in m.params]
    for b in abc.bodies:
        for _, op, ops, _ in abc.ins(b.code):
            if op in (0x60, 0x5d, 0x5e, 0x5f, 0x80, 0x86, 0xb2, 0x4a): add(ops[0], 'code')
    return out


def gdn_literals(abc):
    """string literals pushed right before a call of getDefinitionByName"""
    out = set()
    for b in abc.bodies:
        ins = abc.ins(b.code)
        for k, (_, op, ops, _) in enumerate(ins):
            if op in (0x46, 0x4f, 0x4c) and abc.short(ops[0]) == 'getDefinitionByName' and k and ins[k - 1][1] == 0x2c: out.add(abc.S(ins[k - 1][2][0]))
    return out


def api(abc, name):
    """the trait names of a class (private / protected namespace names left out): its API for the 'one consistent set' check"""
    ci = class_names(abc).index(name); i = abc.instances[ci]; c = abc.classes[ci]
    def nm(t): return re.sub(r'^(private|protected|internal):.*\.', r'\1:', abc.mname(t.name)) + '/%d' % (t.kind & 15)
    return abc.mname(i['super']), sorted(nm(t) for t in i['traits']), sorted('$' + nm(t) for t in c['traits'])


def doc_methods(abc):
    ci = class_names(abc).index(DOC)
    return {abc.short(t.name): len(abc.methods[t.b].params) for t in abc.instances[ci]['traits'] if t.kind & 15 == 1}


def invokes(b):
    """the tooltip class's invoke names in main.raw: [(index, name)], or a reason string"""
    import capstone
    q = lambda a: struct.unpack_from('<Q', b, a)[0]
    if q(VT + 0x70) != VT_INIT or q(VT + 0xe8) != VT_INVOKES: return 'vtable main+%#x: +0x70 = %#x, +0xe8 = %#x' % (VT, q(VT + 0x70), q(VT + 0xe8))
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); out = []; x2 = None; via = 0
    for i in md.disasm(b[VT_INVOKES:VT_INVOKES + 0x140], VT_INVOKES):
        o = i.op_str
        if i.mnemonic == 'adrp' and o.startswith('x2,'): x2 = int(o.split('#')[1], 16)
        elif i.mnemonic == 'add' and o.startswith('x2, x2, #'): x2 += int(o.split('#')[1], 16)
        elif i.mnemonic == 'ldr' and o.endswith('#0x1a8]'): via += 1
        elif i.mnemonic == 'mov' and o.startswith('w1, '):
            idx = 0 if o.endswith('wzr') else int(o.split('#')[1], 0)
            out.append((idx, b[x2:b.index(b'\0', x2)].decode()))
        elif i.mnemonic in ('br', 'ret'): break
    if via != len(out) or [k for k, _ in out] != list(range(len(out))): return 'unexpected registrar shape: %s' % out
    return out


def _host_src(name):
    p = ROOT / 'dm' / 'host' / name
    return p.read_text(encoding='utf-8') if p.exists() else None


def check(src, main_raw=None, all_dir=None):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    print('-- tools/dos2_ptooltip.py: the PC tooltip window as one movie (tooltip_p.swf)')
    tt, kb = src[SRC], src[LIB]
    after, rep = build(tt, kb); rn = rep['rn']
    print('%s + %s -> %s: %d B + %d B -> %d B, sha256 %s' % (SRC, LIB, OUT, len(tt), len(kb), len(after), hashlib.sha256(after).hexdigest()[:16]))
    P = Swf(after); T0 = Swf(tt); K0 = Swf(kb)
    say(Swf(P.write()).body() == P.body(), 'the movie parses and round-trips')
    say(make(src)[OUT] == after, 'the build is deterministic')
    g = gr.check(rep['stage1'], after, quiet=True)
    say(g == [], 'graft: every tag of tooltip.swf kept in order, every character id unique and defined before its use%s' % ('' if not g else ': %s' % g[:5]))
    # imports
    urls = [import_url(d) for c, d, _ in P.tags if c == T_IMPORT2]
    nimp = sum(struct.unpack_from('<H', d, d.index(b'\0') + 3)[0] for c, d, _ in P.tags if c == T_IMPORT2)
    say(LIB not in urls and sorted(urls) == sorted(IMPORTS) and set(urls) <= set(REGISTERED) and nimp == 0,
        'imports: %s (all registered by the Switch\'s loader, no character imported by id); no import of %s' % (' '.join(urls), LIB))
    # classes
    blocks = P.abcs(); (_, _, pa), = [x for x in blocks if DOC in class_names(x[2])]
    allc = [c for _, _, abc in blocks for c in class_names(abc)]
    dup = sorted({c for c in allc if allc.count(c) > 1})
    say(not dup, 'every class defined once: %d classes in %d DoABC tags (tooltip.swf 1 + the library\'s %d)%s' % (len(allc), len(blocks), len(blocks) - 1, '' if not dup else ': twice ' + ' '.join(dup)))
    say(len(rep['kept']) + len(rep['dropped']) == len(K0.abcs()), 'the library\'s classes: %d appended, %d left out (%s)' % (
        len(rep['kept']), len(rep['dropped']), ', '.join('%s: %s' % (n, w) for n, w in rep['dropped'][:3]) + (' ...' if len(rep['dropped']) > 3 else '')))
    # one consistent set: every left-out library class has the same API as tooltip.swf's
    (_, _, ta), = T0.abcs(); same = []; diff = []
    kab = {}
    for _, _, a in K0.abcs(): rn.abc(a); kab[class_names(a)[0]] = a   # the library's blocks under the new names
    for n, why in rep['dropped']:
        if n == LIB_DOC: continue
        (same if api(pa, n) == api(kab[n], n) else diff).append(n)
    say(not diff and same, 'the %d classes both movies define have the same super class and trait names (tooltip.swf\'s copies kept)%s' % (len(same), '' if not diff else ': differ ' + ' '.join(diff)))
    # names: nothing old left, nothing shared with a console library
    olds = rn.top | set(PKGS)
    left = []
    for _, _, abc in blocks:
        for i in range(1, len(abc.strings)):
            t = abc.strings[i].decode('utf-8', 'replace')
            if rn.string(t) is not None: left.append(t)
        for k, (kind, nm) in enumerate(abc.ns):
            if k and abc.S(nm) in PKGS: left.append('namespace ' + abc.S(nm))
    sym = [n for c, d, _ in P.tags if c == T_SYMCLASS for _, n in symclass_entries(d)]
    p3 = place3_classes(P)
    left += [n for n in sym if rn.full(n) != n] + [n for _, n in p3 if rn.full(n) != n]
    say(not left, 'no old name left (ABC strings / namespaces, SymbolClass, PlaceObject3): %d strings renamed, %d PlaceObject3 class names, %d library symbols%s' % (
        rep['strings'], rep['place3'], rep['symbols'], '' if not left else ': ' + ' '.join(sorted(set(left))[:8])))
    toks = [t for _, _, abc in blocks for x in abc.strings[1:] for t in [x.decode('utf-8', 'replace')]
            if rn.string(t) is None and any(re.search(r'(?<![\w.])%s(?![\w])' % re.escape(o), t) for o in olds)]
    say(all(not re.search(r'src=|getDefinition', t) for t in toks), 'strings that only MENTION an old name are trace texts (%d): %s' % (len(toks), ' | '.join(t[:40] for t in toks[:3])))
    props = set()
    for sw in (T0, K0):
        for _, _, abc in sw.abcs():
            for ts in [i['traits'] for i in abc.instances] + [c['traits'] for c in abc.classes] + [b.traits for b in abc.bodies]:
                props |= {abc.short(t.name) for t in ts}
        props |= instance_names(sw)
    clash = sorted(props & rn.top)
    say(not clash, 'no trait and no display-list instance is named like a renamed top-level class (the pool rename only renames classes)%s' % ('' if not clash else ': ' + ' '.join(clash)))
    mine = defs(P)
    for lib in REGISTERED:
        if lib not in src: say(False, 'registered library %s not in the source' % lib); continue
        hit = sorted(mine & defs(Swf(src[lib])))
        say(not hit, 'no class / symbol of %s is defined by the registered library %s%s' % (OUT, lib, '' if not hit else ': ' + ' '.join(hit[:8])))
    if all_dir:
        import glob
        other = {}
        for p in sorted(glob.glob(os.path.join(all_dir, '*.swf'))):
            b = os.path.basename(p)
            if b in (SRC, LIB) or b in REGISTERED: continue
            try: d = defs(Swf(Path(p).read_bytes()))
            except Exception: continue
            for n in mine & d: other.setdefault(n, []).append(b)
        renamed = sorted(n for n in other if n.startswith(('dmTip', PRE_TOP)))
        say(not renamed, 'no other movie of %s defines a dmTip name; names shared with non-library movies (never registered, so never seen by tooltip_p): %s' % (
            all_dir, ', '.join('%s (%d)' % (n, len(v)) for n, v in sorted(other.items())) or 'none'))
    # references resolved inside the movie
    own = set(allc); unres = set()
    for _, _, abc in blocks:
        for why, pkg, nm, kind in refs(abc):
            if pkg == '<multi>': full = nm                                   # a multiname: only a dmTip_ top-level class can be checked by name
            elif kind != A.NS_PUBLIC: continue                               # private / protected / internal: members, not classes
            else: full = (pkg + '.' + nm) if pkg else nm
            ours = full.startswith((PRE_PKG, PRE_TOP)) or pkg in ('tooltip_fla', 'tooltipHelper_kb_fla')
            if ours and full not in own: unres.add('%s %s' % (why, full))
    say(not unres, 'every reference to a dmTip / tooltip class (super, interface, slot type, getlex / coerce / construct ...) is defined in the movie%s' % ('' if not unres else ': ' + ', '.join(sorted(unres)[:6])))
    sc = {}
    for c, d, _ in P.tags:
        if c == T_SYMCLASS:
            for cid, n in symclass_entries(d): sc.setdefault(n, []).append(cid)
    chars = gr.Movie(P).defs
    sbad = [n for n, ids in sc.items() if len(ids) != 1 or n not in own or not (ids[0] == 0 or ids[0] in chars)]
    say(not sbad and len(sc) == len(T0.symbols()) + rep['symbols'], 'SymbolClass: %d names (%d of tooltip.swf + %d of the library), each bound once to a defined character and a class of the movie%s' % (
        len(sc), len(T0.symbols()), rep['symbols'], '' if not sbad else ': ' + ' '.join(sbad[:6])))
    pc = [(o, n) for o, n in p3 if n not in mine and not any(n in defs(Swf(src[l])) for l in IMPORTS if l in src)]
    say(not pc, 'PlaceObject3 class names resolve: %s%s' % (', '.join(sorted({n for _, n in p3})), '' if not pc else '; NOT: %s' % pc))
    # getDefinitionByName literals: what resolved for the PC pair resolves for tooltip_p (locally when it was the pair's own)
    libdefs = set()
    for l in IMPORTS:
        if l in src: libdefs |= defs(Swf(src[l]))
    pair = defs(T0) | defs(K0)
    lit0 = set().union(*[gdn_literals(a) for _, _, a in T0.abcs()], *[gdn_literals(a) for _, _, a in K0.abcs()])
    lit1 = set().union(*[gdn_literals(a) for _, _, a in blocks])
    want = {rn.full(x) for x in lit0}
    loc = sorted(x for x in lit0 if x in pair)
    lost = [x for x in lit0 if (x in pair and rn.full(x) not in mine) or (x not in pair and x in libdefs and rn.full(x) != x)]
    nowhere = sorted(x for x in lit0 if x not in pair and x not in libdefs)
    say(lit1 == want and not lost, 'getDefinitionByName literals: %d, the %d the PC pair defines now name local classes (%s); from the imported libraries: %s; defined nowhere (as on the PC): %s' % (
        len(lit1), len(loc), ' '.join(rn.full(x) for x in loc), ' '.join(sorted(x for x in lit0 if x not in pair and x in libdefs)) or 'none', ' '.join(nowhere) or 'none'))
    # ABC
    nb = 0; errs = []
    for _, _, abc in blocks:
        e = verify_all(abc); nb += len(abc.bodies); errs += e
    for who, why in errs[:10]: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies in %d blocks' % (nb, len(blocks)))
    kbody = sum(len(kab[n].bodies) for n in rep['kept'])
    say(nb == len(ta.bodies) + kbody, 'method bodies = tooltip.swf\'s %d + the appended library classes\' %d (code bytes untouched: only pool strings change)' % (len(ta.bodies), kbody))
    say([b.code for b in pa.bodies] == [b.code for b in ta.bodies], 'tooltip.swf\'s own code byte for byte')
    # the engine contract
    import dos2_ui_audit as au
    ein = lambda abcs: {n for abc in abcs for rec in au.scan(abc).values() for n, _ in rec['ei'] if n != '<dynamic>'}
    e0 = ein([ta] + [a for _, _, a in K0.abcs()]); e1 = ein([a for _, _, a in blocks])
    say(e1 <= e0 and ein([ta]) <= e1, 'ExternalInterface names: %s (the PC pair\'s set, none new)' % ' '.join(sorted(e1)))
    say(doc_methods(pa) == doc_methods(ta), 'the document class %s is tooltip.swf\'s (%d methods)' % (DOC, len(doc_methods(pa))))
    b = None
    for p in [main_raw, _find('work/exefs-update/main.raw')]:
        if p and os.path.exists(str(p)): b = open(str(p), 'rb').read(); break
    if b is None: say(False, 'main.raw not found')
    else:
        inv = invokes(b)
        if isinstance(inv, str): say(False, 'the tooltip class\'s invokes: %s' % inv)
        else:
            dm = doc_methods(pa); d0 = doc_methods(ta)
            has = [n for _, n in inv if n in dm]; miss = [n for _, n in inv if n not in dm]
            say([n in dm for _, n in inv] == [n in d0 for _, n in inv] and len(inv) == 10 and 'addTooltip' in has,
                'the tooltip class (vtable main+%#x, invokes registered at main+%#x) names %d invokes; in the movie as in tooltip.swf: %s; not in either (Iggy: no such method): %s' % (
                    VT, VT_INVOKES, len(inv), ' '.join(has), ' '.join(miss)))
        dead = sorted(n for n in e1 if n.encode() + b'\0' not in b)
        say(not dead, 'every ExternalInterface name is a string of main.raw%s' % ('' if not dead else ': NOT ' + ' '.join(dead)))
    # the host
    c = _host_src('hle_dos2_touchui.c'); h = _host_src('hle_dos2_touchui_hud.c')
    if c is None or h is None: print('skip dm/host not found next to tools/: host checks skipped')
    else:
        row = '{0x2c, 0, "tooltip_p", "GUI/tooltip.swf"}'
        say(row in c.split('static const tu_def k_touch[]')[1].split('};')[0], 'k_touch maps type 0x2c (GUI/tooltip.swf) to tooltip_p (touch only)')
        say('"GUI/tooltip_p.swf"' in h, 'hle_dos2_touchui_hud.c makes the window only for GUI/tooltip_p.swf')
        nocom = ''
        for f in sorted((ROOT / 'dm' / 'host').glob('*.[ch]')):
            nocom += re.sub(r'//[^\n]*|/\*.*?\*/', '', f.read_text(encoding='utf-8', errors='replace'), flags=re.S)
        say('tooltipHelper_kb' not in nocom, 'no host code names tooltipHelper_kb (no global library registration anywhere; comments only)')
    return bad


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == 'check':
        sys.exit(1 if check(sources(a[1]), a[2] if len(a) > 2 else None, a[1] if os.path.isdir(a[1]) else None) else 0)
    if len(a) == 3 and a[0] == 'build':
        out = Path(a[2]); out.mkdir(parents=True, exist_ok=True)
        d = make(sources(a[1]))[OUT]; (out / OUT).write_bytes(d); print('%s: %d B' % (out / OUT, len(d))); return
    if len(a) == 2 and a[0] == 'names':
        s = sources(a[1]); _, rep = build(s[SRC], s[LIB]); rn = rep['rn']
        for _, _, abc in Swf(make(s)[OUT]).abcs():
            for n in class_names(abc): print(n)
        print('top-level renamed: %s' % ' '.join(sorted(rn.top)))
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
