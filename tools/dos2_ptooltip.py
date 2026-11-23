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
def build(tt, kb, diag=True):
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
    rep['diag'] = diag_wrap(S) if diag else []
    return S.write(compress=True), rep


# ---------------------------------------------------------------- diagnostics (phone 23.11: the engine calls addTooltip x54 / addFormattedTooltip x7
# but the movie never reaches its own ExternalInterface calls after them - no keepUIinScreen, no setTooltipSize: the code throws inside, and
# Iggy reports nothing). Each method below is wrapped: the engine (or the frame) calls the wrapper under the old name, the wrapper calls the
# original (kept as dmOrig_<name>) inside try / catch; a throw is reported as ExternalInterface dmTipErr_<name>(String(error)) (the host's
# DM_DOS2_TOUCHUI_EI log shows its text: "TypeError: Error #1009 ..." / "ReferenceError ..."), a return as dmTipOk_<name>() - then returns.
DIAG = ('frame1', 'addTooltip', 'addFormattedTooltip', 'INTshowTooltip', 'showFormattedTooltipAfterPos')
DIAG_STATE = ('tooltip_mc', 'tooltip_array', 'formatTooltip', 'compareTooltip', 'offhandTooltip', 'defaultTooltip', 'statusTooltip', 'tf', 'compareShowTimer', 'events')
def diag_wrap(S):
    for n, pre, abc in S.abcs():
        try: ci = abc.class_index('tooltip_fla.MainTimeline')
        except KeyError: continue
        ei = abc.pub('ExternalInterface', 'flash.external'); call = abc.pub('call'); done = []
        for name in DIAG:
            t = abc.trait(ci, name)
            if t is None or t.kind & 15 != 1: continue
            om = abc.methods[t.b]; ob = om.body; np_ = len(om.params)
            keep = A.Trait(abc.pub('dmOrig_' + name), t.kind & 0x0f, 0, t.b)   # the original, under a new public name (no override / final bits)
            orig = keep.name
            head = A.asm([('getlocal0',), ('pushscope',)])
            body = A.asm([('getlocal0',)] + [('getlocal', k) for k in range(1, np_ + 1)] + [('callpropvoid', orig, np_)])
            ok = A.asm([('getlex', ei), ('pushstring', abc.string('dmTipOk_' + name)), ('callpropvoid', call, 1), ('returnvoid',)])
            # the report: String(e), e.message, e.errorID, then for each member of DIAG_STATE whether it is null (1) - frame1 makes them (run 23.11b:
            # every addTooltip / addFormattedTooltip threw a bare "TypeError": a member the frame script never made is the suspect)
            st = []
            for m in DIAG_STATE:
                tr = abc.trait(ci, m)
                st += [('getlocal0',), ('getproperty', tr.name if tr else abc.pub(m)), ('pushnull',), ('equals',), ('convert_i',)]
            ex = A.asm([('setlocal', np_ + 1), ('getlocal0',), ('pushscope',), ('getlex', ei), ('pushstring', abc.string('dmTipErr_' + name)),
                        ('getlocal', np_ + 1), ('convert_s',), ('getlocal', np_ + 1), ('getproperty', abc.pub('message')), ('convert_s',),
                        ('getlocal', np_ + 1), ('getproperty', abc.pub('errorID')), ('convert_s',)] + st +
                       [('callpropvoid', call, 4 + len(DIAG_STATE)), ('returnvoid',)])
            f, to = len(head), len(head) + len(body)
            wb = abc.add_method('dmDiag_' + name, om.params, om.ret, head + body + ok + ex, max(8 + len(DIAG_STATE), np_ + 2), np_ + 2, ob.init_scope, ob.init_scope + 1)
            wm = abc.methods[wb]
            if om.flags & 0x08: wm.flags |= 0x08; wm.options = list(om.options)
            abc.methods[wb].body.exc = [(f, to, to + len(ok), 0, 0)]
            abc.instances[ci]['traits'].append(keep); t.b = wb; done.append(name)
        bad = verify_all(abc)
        assert not bad, bad
        S.tags[n][1] = pre + abc.write()
        return done
    raise AssertionError('no tooltip_fla.MainTimeline')


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


def build_console_lib(tt):
    """owner 23.11 ("why does the hybrid mode show tooltips fine? borrow from there"): the PC tooltip.swf as it is, its one import
    tooltipHelper_kb.swf (never registered on the Switch) swapped for the console library tooltipHelper.swf that the Switch's loader
    registers for the console tooltip (the hybrid's, which works). Every library class tooltip.swf names (tt_tooltipObj + 8 looked up by
    getDefinitionByName) is in tooltipHelper.swf with the same traits; only LSTooltipClass / tt_groupHolder / tt_EquipHeader bodies differ
    (pad button hints). The grafted single movie (build above, the 21.11 route) threw TypeError on every call on the phone (run 23.11b)."""
    T = Swf(tt); n = 0
    for t in T.tags:
        if t[0] == T_IMPORT2 and import_url(t[1]) == LIB:
            t[1] = CONSOLE_LIB.encode() + t[1][t[1].index(b'\0'):]; n += 1
    assert n == 1, 'tooltip.swf imports %s once' % LIB
    if TOUCH_FIT: touch_fit(T)
    return T.write(compress=True)
CONSOLE_LIB = 'tooltipHelper.swf'
ROUTE = 'console_lib'   # what the pak gets: 'console_lib' (above) or 'graft' (build: one self-contained movie, with the diag wrappers)


# ---------------------------------------------------------------- the size and the body (phone run 23.11, 6.7" 2400 x 1080, after the console_lib route)
# (a) a hot bar skill tooltip ran off the bottom of the screen, (b) an inventory item tooltip showed its header, ONE line of its body and its
# footer, low and partly off the screen. Causes (static, docs/dos2/TOUCH_MODES_PLAN.md "Подсказки касания: размер, тело, удержание"):
#   size  the engine places the tooltip window itself: setTooltipSize(w, h) (main+0x111e490) stores w, h and runs the placement main+0x110e2c0,
#         which works in screen px = w, h x s2 with s2 = S x screen height / 1080 (S = the "UI scale" setting while the mode byte is 0) - no class
#         scale in it. The host gave type 0x2c the class scale c = 1.3 (the window drawn 30 % bigger than the engine believed): a hot bar
#         tooltip is put at y = H - (h + 158) x s2 (the hot bar branch, requester type 0x28) and any other one is clamped to y + h x s2 <= H,
#         so the drawn one ended 0.3 h lower, under the screen's edge.
#   body  the console library's tt_bgMid.setBgWSubsections(h, maxH + tooltipPaddingB, list) sets the body's height to min(h, maxH + 26) (the
#         keyboard library's sets h): the console panels give their tooltip a maxH, tooltip.swf never does (0), so every tooltip with
#         sub-sections (most items) got a 26 px body = one line (LSTooltipClass.resetBackground, scaleH = true, hasSubSections).
# Fix in the movie (the host: class scale 1 for type 0x2c, so the engine's numbers ARE the drawn ones - hle_dos2_touchui_hud.c):
#   addFormattedTooltip  the original, after maxH = TIP_MAXH on the three LSTooltipClass objects (formatTooltip / compareTooltip /
#                        offhandTooltip .tooltip_mc): the body keeps its full height (no cap, so no scrolling a finger cannot do)
#   checkTooltipBoundaries(w, h, x, y)  (the one place that sends setTooltipSize) the window's own scale f = dmTipF (the host's factor, the
#                        hybrid's console tooltip size: 1.3 on a phone), lowered so f x h <= dmTipMaxH and f x w <= dmTipMaxW (the screen in
#                        stage px less the hot bar branch's 158 and margins), never under 1/2; tooltip_mc scaled by f and the ENGINE told
#                        f x w, f x h - so its placement keeps the whole tooltip on the screen. h of an empty / simple-text tooltip is
#                        tooltip_mc.height (already scaled): divided back first. EI dmTipFit(f, w, h) for the host's log.
#   setAnchor(v)         (an invoke name the tooltip class registers, absent from tooltip.swf) the host's numbers: 1000000 + 1000 f, 2000000 +
#                        max height, 3000000 + max width (stage px); anything else is ignored. EI dmTipSet(f, max h, max w) answers.
# Nothing told (an old host): f = 1, no fit - tooltip.swf's own behaviour.
TOUCH_FIT = True
TIP_MAXH = 100000
TIP_CMD = 1000000
TIP_EI = ('dmTipFit', 'dmTipSet')


def _doc_kit():
    """tools/dos2_pchud_touch.py's Doc / HEAD / num (the host module when installed, else imported)"""
    if _host is not None and hasattr(_host, 'Doc'): return _host
    import dos2_pchud_touch as ph
    return ph


def touch_fit(T):
    """tooltip.swf's document class patched in place (methods appended, two traits re-pointed with the originals kept, one method added)"""
    ph = _doc_kit(); d = ph.Doc(T); P, HEAD, num = d.P, ph.HEAD, ph.num
    half = [('pushbyte', 1), ('pushbyte', 2), ('divide',)]
    # addFormattedTooltip(x, y, show): locals n + 1 an object
    def aft(n):
        o = n + 1; c = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', o)]
        for k, p in enumerate(('formatTooltip', 'compareTooltip', 'offhandTooltip')):
            c += d.get(p) + [('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', 'x%d' % k),
                             ('getlocal', o), ('getproperty', P('tooltip_mc')), ('coerce_a',), ('setlocal', o), ('getlocal', o), ('iffalse', 'x%d' % k),
                             ('getlocal', o), ('pushint', d.abc_int(TIP_MAXH)), ('setproperty', P('maxH')), ('L', 'x%d' % k)]
        return c + d.call_orig('dmTipAftOrig', n) + [('returnvoid',)]
    d.wrap('addFormattedTooltip', 'dmTipAftOrig', aft)
    # checkTooltipBoundaries(w, h, x, y): locals 5 f, 6 h0, 7 s, 8 a limit, 9 tooltip_mc
    def ctb(n):
        assert n == 4, 'checkTooltipBoundaries(w, h, x, y)'
        F, H0, S_, LIM, MC = 5, 6, 7, 8, 9
        c = HEAD + [('pushbyte', 1), ('convert_d',), ('setlocal', F), ('getlocal2',), ('convert_d',), ('setlocal', H0), ('pushbyte', 1), ('convert_d',), ('setlocal', S_),
                    ('pushbyte', 0), ('convert_d',), ('setlocal', LIM), ('pushnull',), ('coerce_a',), ('setlocal', MC)]
        c += d.get('tooltip_mc') + [('coerce_a',), ('setlocal', MC), ('getlocal', MC), ('iffalse', 'plain')]
        # f = the host's factor when it is a positive number
        c += d.get('dmTipF') + [('convert_d',), ('setlocal', LIM), ('getlocal', LIM), ('pushbyte', 0), ('ifngt', 'nf'), ('getlocal', LIM), ('setlocal', F), ('L', 'nf')]
        # h0: unscaled (getTooltipHeight measures tf.tooltip_mc when there is one, else tooltip_mc.height = already x its scale)
        c += [('getlocal', MC), ('getproperty', P('scaleY')), ('convert_d',), ('setlocal', S_), ('getlocal', S_), ('pushbyte', 0), ('ifgt', 'sok'),
              ('pushbyte', 1), ('convert_d',), ('setlocal', S_), ('L', 'sok')]
        c += d.get('tf') + [('iffalse', 'disp')] + d.get('tf', 'tooltip_mc') + [('iftrue', 'fmt'),
              ('L', 'disp'), ('getlocal', H0), ('getlocal', S_), ('divide',), ('convert_d',), ('setlocal', H0), ('L', 'fmt')]
        # the fit: f x h0 <= dmTipMaxH, f x w <= dmTipMaxW (positive numbers only)
        for k, (prop, size) in enumerate((('dmTipMaxH', [('getlocal', H0)]), ('dmTipMaxW', [('getlocal1',), ('convert_d',)]))):
            c += d.get(prop) + [('convert_d',), ('setlocal', LIM), ('getlocal', LIM), ('pushbyte', 0), ('ifngt', 'nl%d' % k)] + size + [('pushbyte', 0), ('ifngt', 'nl%d' % k)] + \
                 size + [('getlocal', F), ('multiply',), ('getlocal', LIM), ('ifngt', 'nl%d' % k), ('getlocal', LIM)] + size + [('divide',), ('convert_d',), ('setlocal', F), ('L', 'nl%d' % k)]
        c += [('getlocal', F)] + half + [('ifnlt', 'fmin')] + half + [('convert_d',), ('setlocal', F), ('L', 'fmin')]
        c += [('getlocal', MC), ('getlocal', F), ('setproperty', P('scaleX')), ('getlocal', MC), ('getlocal', F), ('setproperty', P('scaleY'))]
        c += d.ei('dmTipFit', [('getlocal', F)], [('getlocal1',)], [('getlocal', H0)])
        c += [('getlocal0',), ('getlocal1',), ('getlocal', F), ('multiply',), ('getlocal', H0), ('getlocal', F), ('multiply',), ('getlocal3',), ('getlocal', 4),
              ('callpropvoid', P('dmTipCtbOrig'), 4), ('returnvoid',)]
        c += [('L', 'plain')] + d.call_orig('dmTipCtbOrig', n) + [('returnvoid',)]
        return c
    d.wrap('checkTooltipBoundaries', 'dmTipCtbOrig', ctb)
    # setAnchor(v = null, ...rest): locals 1 v, 2 rest, 3 the number, 4 the command
    big = lambda v: [('pushint', d.abc_int(v))]
    c = HEAD + [('pushbyte', 0), ('convert_d',), ('setlocal', 3), ('pushbyte', 0), ('convert_i',), ('setlocal', 4),
                ('getlocal1',), ('convert_d',), ('setlocal', 3), ('getlocal', 3)] + big(TIP_CMD) + [('ifnge', 'end'), ('getlocal', 3)] + big(4 * TIP_CMD) + [('ifnlt', 'end'),
                ('getlocal', 3)] + big(TIP_CMD) + [('divide',), ('convert_i',), ('setlocal', 4),
                ('getlocal', 3), ('getlocal', 4)] + big(TIP_CMD) + [('multiply',), ('subtract',), ('convert_d',), ('setlocal', 3),
                ('getlocal', 4), ('pushbyte', 1), ('ifne', 'c2'), ('getlocal0',), ('getlocal', 3)] + num(1000) + [('divide',), ('setproperty', P('dmTipF')), ('jump', 'ack'),
                ('L', 'c2'), ('getlocal', 4), ('pushbyte', 2), ('ifne', 'c3'), ('getlocal0',), ('getlocal', 3), ('setproperty', P('dmTipMaxH')), ('jump', 'ack'),
                ('L', 'c3'), ('getlocal', 4), ('pushbyte', 3), ('ifne', 'end'), ('getlocal0',), ('getlocal', 3), ('setproperty', P('dmTipMaxW')),
                ('L', 'ack')] + d.ei('dmTipSet', d.get('dmTipF'), d.get('dmTipMaxH'), d.get('dmTipMaxW')) + [('L', 'end'), ('returnvoid',)]
    assert d.abc.trait(d.ci, 'setAnchor') is None, 'tooltip.swf has no setAnchor of its own'
    m = d.add('setAnchor', 1, c)
    mm = d.abc.methods[m]; mm.flags |= 0x08 | 0x04; mm.options = [(0x0c, 0x0c)]   # HAS_OPTIONAL (v = null) | NEED_REST: any argument count the engine might use
    b = mm.body; st, sc, loc = d.abc.verify(b); b.local_count = max(loc, 3)
    d.inst['traits'].append(ph.Trait(P('setAnchor'), 1, 0, m))
    bad = verify_all(d.abc)
    assert not bad, bad
    d.done()
    return d.made


def make(src): return {OUT: build_console_lib(src[SRC]) if ROUTE == 'console_lib' else build(src[SRC], src[LIB])[0]}
def dis_names(abc): return {abc.short(t.name) for i in abc.instances for t in i['traits']}


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


def check_fit(src, T0, C, main_raw=None):
    """the touch fit of the console_lib route (touch_fit above): append-only, the wrappers' contract, the causes it answers, the host's side"""
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    (_, _, a0), = T0.abcs(); (_, _, a1), = C.abcs()
    if not TOUCH_FIT: say(False, 'TOUCH_FIT is off: the tooltip keeps its 26 px body and the engine places it by a size it does not draw'); return bad
    ci0, ci1 = a0.class_index(DOC), a1.class_index(DOC)
    tr = lambda a, ci, n: a.trait(ci, n)
    say(len(a1.methods) == len(a0.methods) + 3 and len(a1.bodies) == len(a0.bodies) + 3 and all(a1.bodies[i].code == a0.bodies[i].code for i in range(len(a0.bodies))),
        'touch fit: 3 methods appended (addFormattedTooltip / checkTooltipBoundaries wrappers, setAnchor), every body of tooltip.swf byte for byte')
    ok = True
    for name, keep in (('addFormattedTooltip', 'dmTipAftOrig'), ('checkTooltipBoundaries', 'dmTipCtbOrig')):
        t0, t1, k1 = tr(a0, ci0, name), tr(a1, ci1, name), tr(a1, ci1, keep)
        ok &= bool(t0 and t1 and k1) and k1.b == t0.b and t1.b >= len(a0.methods)
    sa = tr(a1, ci1, 'setAnchor')
    ok &= sa is not None and sa.b >= len(a0.methods) and a1.methods[sa.b].flags & 0x0c == 0x0c and tr(a0, ci0, 'setAnchor') is None
    say(ok, 'traits: addFormattedTooltip / checkTooltipBoundaries -> the wrappers (originals kept as dmTipAftOrig / dmTipCtbOrig); setAnchor(v = null, ...rest) added')
    vb = verify_all(a1)
    say(not vb, 'ABC verifies after the fit (%d bodies)%s' % (len(a1.bodies), '' if not vb else ': %s' % vb[:3]))
    own = a1.owners(); dis = lambda m: '\n'.join(a1.dis(a1.methods[m].body))
    send = sorted({own.get(b.method, '?') for b in a1.bodies if "'setTooltipSize'" in '\n'.join(a1.dis(b))})
    callers = sorted({own.get(b.method, '?') for b in a1.bodies if 'checkTooltipBoundaries' in '\n'.join(a1.dis(b)) and b.method < len(a0.methods)})
    say(send == ['tooltip_fla.MainTimeline::dmTipCtbOrig'] and set(callers) >= {'tooltip_fla.MainTimeline::' + n for n in ('dmTipAftOrig', 'addTooltip', 'setWindow')},
        'setTooltipSize is sent only by the original checkTooltipBoundaries (now dmTipCtbOrig), every caller of it (%s) names checkTooltipBoundaries = the wrapper' % ', '.join(c.split('::')[-1] for c in callers))
    w = dis(tr(a1, ci1, 'checkTooltipBoundaries').b)
    say(all(x in w for x in ('scaleX', 'scaleY', 'dmTipF', 'dmTipMaxH', 'dmTipMaxW', "'dmTipFit'", 'callpropvoid     dmTipCtbOrig, 4')) and w.count('multiply') >= 4,
        'checkTooltipBoundaries wrapper: tooltip_mc scaled by f = min(dmTipF, dmTipMaxH / h, dmTipMaxW / w) (>= 1/2), the engine told f x w, f x h; EI dmTipFit for the log')
    w = dis(tr(a1, ci1, 'addFormattedTooltip').b)
    say(w.count('setproperty      maxH') == 3 and w.count('pushint          %d' % TIP_MAXH) == 3 and 'dmTipAftOrig' in w,
        'addFormattedTooltip wrapper: maxH = %d on formatTooltip / compareTooltip / offhandTooltip .tooltip_mc before the original' % TIP_MAXH)
    w = dis(sa.b)
    say(all('pushint          %d' % v in w for v in (TIP_CMD, 4 * TIP_CMD)) and all(x in w for x in ('dmTipF', 'dmTipMaxH', 'dmTipMaxW', "'dmTipSet'")),
        'setAnchor(v): %d + 1000 f / %d + max h / %d + max w (stage px) for the host, anything else ignored; EI dmTipSet answers' % (TIP_CMD, 2 * TIP_CMD, 3 * TIP_CMD))
    # the cause of the one-line body: the console library caps the body at maxH + padding, the keyboard library does not
    def cap(lib):
        sw = Swf(src[lib]) if lib in src else None
        if sw is None: return None
        for _, _, a in sw.abcs():
            o = a.owners()
            for b in a.bodies:
                if o.get(b.method, '').endswith('::setBgWSubsections'):
                    lines = a.dis(b); hs = [k for k, l in enumerate(lines) if l.split()[1:3] == ['setproperty', 'height'] and 'bg_mc' in '\n'.join(lines[max(0, k - 12):k])]
                    return any('ifnlt' in l for k in hs for l in lines[max(0, k - 10):k])
        return None
    cc, ck = cap(CONSOLE_LIB), cap(LIB)
    say(cc is True and ck is False, 'the cause: %s tt_bgMid.setBgWSubsections sets the body to min(h, maxH + padding) (%s), %s sets h (%s) - tooltip.swf never sets maxH (0): a 26 px body'
        % (CONSOLE_LIB, cc, LIB, ck))
    b = None
    for p in [main_raw, _find('work/exefs-update/main.raw')]:
        if p and os.path.exists(str(p)): b = open(str(p), 'rb').read(); break
    if b is not None:
        inv = invokes(b)
        say(not isinstance(inv, str) and 'setAnchor' in [n for _, n in inv], 'the tooltip class registers the invoke setAnchor (the host\'s carrier)')
    h = _host_src('hle_dos2_touchui_hud.c'); hh = _host_src('hle_dos2_touchui_hud.h')
    if h is None or hh is None: print('skip dm/host not found next to tools/: host fit checks skipped')
    else:
        say(all('"%s"' % n in h for n in TIP_EI) and '"setAnchor"' in h and 'HU_TIP_CMD = %d' % TIP_CMD in hh,
            'host: hle_dos2_touchui_hud.c takes EI %s (log) and sends setAnchor(HU_TIP_CMD + ...) (hle_dos2_touchui_hud.h HU_TIP_CMD = %d)' % (' / '.join(TIP_EI), TIP_CMD))
        say('HU_TIP_HB = 158' in hh, 'host: the hot bar branch\'s 158 stage px (main+0x110e3bc add w14,w11,#0x9e) in the max height (hle_dos2_touchui_hud.h HU_TIP_HB)')
    return bad


def check(src, main_raw=None, all_dir=None):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    print('-- tools/dos2_ptooltip.py: the PC tooltip window as one movie (tooltip_p.swf)')
    tt, kb = src[SRC], src[LIB]
    after, rep = build(tt, kb, diag=False); rn = rep['rn']   # the checks below are about the movie itself; the diag wrappers: last check
    print('%s + %s -> %s: %d B + %d B -> %d B, sha256 %s' % (SRC, LIB, OUT, len(tt), len(kb), len(after), hashlib.sha256(after).hexdigest()[:16]))
    P = Swf(after); T0 = Swf(tt); K0 = Swf(kb)
    say(Swf(P.write()).body() == P.body(), 'the movie parses and round-trips')
    d, drep = build(tt, kb); D = Swf(d); dabc = [abc for _, _, abc in D.abcs() if 'tooltip_fla.MainTimeline' in class_names(abc)][0]
    say(build(tt, kb)[0] == d, 'the graft build is deterministic')
    cl = build_console_lib(tt); C = Swf(cl)
    say(make(src)[OUT] == (cl if ROUTE == 'console_lib' else d), 'the packed movie = route %s' % ROUTE)
    say([import_url(t[1]) for t in C.tags if t[0] == T_IMPORT2] == [import_url(t[1]).replace(LIB, CONSOLE_LIB) for t in T0.tags if t[0] == T_IMPORT2]
        and CONSOLE_LIB in REGISTERED and [t for t in C.tags if t[0] not in (T_IMPORT2, T_ABC)] == [t for t in T0.tags if t[0] not in (T_IMPORT2, T_ABC)],
        'console_lib route: tooltip.swf byte for byte but its import %s -> %s (registered by the Switch loader) and its DoABC (the touch fit below)' % (LIB, CONSOLE_LIB))
    bad += check_fit(src, T0, C, main_raw)
    say(drep['diag'] == list(DIAG) and not verify_all(dabc) and all('dmOrig_' + m in dis_names(dabc) for m in DIAG),
        'diag wrappers (phone 23.11, the movie throws silently): %s -> try / catch around dmOrig_<name>, EI dmTipErr_<name>(error) / dmTipOk_<name>' % ', '.join(drep['diag']))
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
