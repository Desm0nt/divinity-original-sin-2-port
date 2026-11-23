"""[DOS2] UI mode TOUCH: the PC tutorial window made for a touch screen (owner 23.11: "the game keeps popping PC tutorials about keys,
F-keys and so on; on the iPad there were none, they get in the way; and on the iPad a tip closes by a tap on the whole window, not by the
tiny X - finish it like the iPad"). Static only: nothing here was played or run on a phone.

The touch mode runs the PC UI (mode byte main+0x2bb8918 = 0), so type 0x37 plays the player's PC GUI/tutorialBox.swf and the engine takes
the PC text of each tutorial. Two halves:
  host   the tutorials whose text names a key, the mouse or a pad button are not queued at all while the HUD part of the mode is TOUCH
         (dm/host/hle_dos2_touch_tuts.c: InsertTutorial main+0x126a0d0 hooked; the facts and the 89 keys: hle_dos2_touch_tuts.h). The table
         is Public/Shared/Localization/TutorialMessages.lsb (Shared.pak; TranslatedStringKeys: key -> text handle). The PC UI shows the
         base key, the console UI key + "_Controller" when it exists (main+0xdc15f0). The iPad's copy has 50 more keys, key + "_Touch",
         with touch wording only its own localization has (handles the Switch english.xml / russian.xml do not have): the release carries
         no iPad text, so they cannot be used. The rule works on the SHIPPED texts (the table's inline English is older: three keys name a
         key only in english.xml and the other languages): a key token ([IE_...], [LeftStickX]) in any of the 14 Switch languages, a mouse
         act (click, double click, mouse, cursor, arrow keys, keyboard) in English or Russian; KEEP below lists the hits that stay shown.
         45 PC keys + 44 console keys (the PC UI never asks for a *_Controller key, but the client names some itself).
  movie  tutorialBox_p.swf = the player's PC tutorialBox.swf with seven methods wrapped (the originals kept as dmTb*Orig) and three tap
         handlers added: a tap (press and release on the same window) anywhere on a non-modal pop-up or on the pointer bubble = its X
         (xcloseBtnFunc: ExternalInterface cancelNonModal, the PC path), anywhere on the modal box = its OK (fadeOutModal -> OkPressed).
         Taps on the window's own controls (the X, OK, the "Hide tutorials" check box) are theirs as before. A window closes once: a second
         tap while it fades out does nothing (the engine would otherwise get a second cancelNonModal / OkPressed for the next tutorial).
  k_touch  type 0x37 -> tutorialBox_p (dm/host/hle_dos2_touchui.c; touch set only - SWITCH / HYBRID keep the stock movie).
Why the host and not the movie for the keyboard half: the movie only gets the finished text (the key tokens already replaced by the
player's bindings, in the game's language) and has to answer the engine for a window it would not show; the engine's queue sees the key.

  python -I tools/dos2_ptutorial.py check <Switch Game.pak | dir of the PC movies> [main.raw] [--ipad <iPad .app/Data dir>]
         every static check; --ipad also compares the key set with the iPad's _Touch twins (counts only, no iPad text is used)
  python -I tools/dos2_ptutorial.py build <src> <out dir>     tutorialBox_p.swf
  python -I tools/dos2_ptutorial.py list [Shared.pak]         the keys left out in touch, with their PC text (English)
Hook: tools/dos2_pchud_touch.py calls install() at its end; build() then also returns tutorialBox_p.swf and check() runs check() here
(so the `touch` set of tools/dos2_touchui_pak.py packs it).
"""
import hashlib, html, os, re, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, verify_all
import dos2_swf_graft as gr

ROOT = Path(__file__).resolve().parent.parent
GUI = 'Public/Game/GUI/'
SRC, OUT = 'tutorialBox.swf', 'tutorialBox_p.swf'
DOC = 'tutorialBox_fla.MainTimeline'
TUT_LSB = 'Public/Shared/Localization/TutorialMessages.lsb'
HDR = ROOT / 'dm' / 'host' / 'hle_dos2_touch_tuts.h'

# the rule for "keyboard-only": a key (any but *_Title and TUT_CAT_*) whose text names a key, the mouse or a pad button in the game's
# shipped texts - a key token ([IE_...], [LeftStickX]) in any of the 14 Switch languages (or the table's own English), a mouse act in the
# English or Russian text - and the keys it hits wrongly
TOKEN = re.compile(r'\[(IE_\w+|\w*Stick\w*)\]')
MOUSE = re.compile(r'(?i)\b(double[- ]?click|right[- ]click|left[- ]click|click\w*|mouse|cursor|arrow keys|keyboard)\b')
MOUSE_RU = re.compile(r'(?i)щ[её]лк\w*|мыш\w*|курсор\w*|клавиш\w*')
KEEP = {'TUT_PressurePlates': '"they make a clicking sound": the plate clicks, not the mouse',
        'TUT_Combat': 'the combat rules; only the Japanese text adds "or press [IE_UIEndTurn]"',
        'TUT_TradeIntro': 'on-screen buttons of the trade window (Russian / German "click the scales" = a tap); the iPad shows this PC text too (no _Touch twin)'}
N_KEYS, N_BASE, N_OFF, N_PC = 426, 178, 89, 45

# trait -> our method (the original is kept as the name in the third column) - one table for patch() and run_py()
REPOINT = [('onEventInit', 'dmTbInit', 'dmTbInitOrig', 0), ('fadeInModal', 'dmTbFadeIn', 'dmTbFadeInOrig', 5),
           ('fadeOutModal', 'dmTbFadeOut', 'dmTbFadeOutOrig', 0), ('fadeInNonModalPointer', 'dmTbPtrIn', 'dmTbPtrInOrig', 4),
           ('fadeOutNonModalPointer', 'dmTbPtrOut', 'dmTbPtrOutOrig', 0), ('addNonModalBox', 'dmTbAddBox', 'dmTbAddBoxOrig', 4),
           ('removeNonModalBox', 'dmTbRemoveBox', 'dmTbRemoveBoxOrig', 1)]
ADDED = ['dmTbDown', 'dmTbIn', 'dmTbModalUp', 'dmTbPointerUp', 'dmTbBoxUp']

# The wrappers and tap handlers in the Python subset tools/dos2_touchkit.py kc_compile turns into AVM2 (the same text runs in Python: run_py)
CODE = '''
def dmTbInit():
    this.dmTbInitOrig()
    if this.dmTbOn:
        return
    this.dmTbOn = True
    b = this.tutorialBox_mc
    if b:
        b.addEventListener('mouseDown', this.dmTbDown)
        b.addEventListener('mouseUp', this.dmTbModalUp)
    p = this.nonModalPointer_mc
    if p:
        p.addEventListener('mouseDown', this.dmTbDown)
        p.addEventListener('mouseUp', this.dmTbPointerUp)

def dmTbDown(e):
    this.dmTbPress = e.currentTarget

def dmTbIn(t, o):
    if not o:
        return False
    while t:
        if t is o:
            return True
        t = t.parent
    return False

def dmTbFadeIn(a, b, c, d, f):
    this.dmTbOut = False
    this.dmTbFadeInOrig(a, b, c, d, f)

def dmTbFadeOut():
    if this.dmTbOut:
        return
    this.dmTbOut = True
    this.dmTbFadeOutOrig()

def dmTbModalUp(e):
    b = e.currentTarget
    if not (this.dmTbPress is b):
        return
    this.dmTbPress = None
    if not this.modalBoxVisible or this.dmTbOut:
        return
    t = e.target
    if this.dmTbIn(t, b.ok_mc) or this.dmTbIn(t, b.checkbox_mc):
        return
    this.fadeOutModal()

def dmTbPtrIn(a, b, c, d):
    this.dmTbPtrOpen = True
    this.dmTbPtrInOrig(a, b, c, d)

def dmTbPtrOut():
    this.dmTbPtrOpen = False
    this.dmTbPtrOutOrig()

def dmTbPointerUp(e):
    p = e.currentTarget
    if not (this.dmTbPress is p):
        return
    this.dmTbPress = None
    if not this.dmTbPtrOpen or not p.visible:
        return
    if this.dmTbIn(e.target, p.close_mc):
        return
    this.xcloseBtnFunc()

def dmTbAddBox(a, b, c, d):
    this.dmTbAddBoxOrig(a, b, c, d)
    l = this.nonModalList
    if not l:
        return
    m = l.getElementByString('idStr', a)
    if not m:
        return
    m.dmTbClosed = False
    if m.dmTbArmed:
        return
    m.dmTbArmed = True
    m.addEventListener('mouseDown', this.dmTbDown)
    m.addEventListener('mouseUp', this.dmTbBoxUp)

def dmTbRemoveBox(a):
    l = this.nonModalList
    if l and l.length > 0:
        m = l.getAt(0)
        if a != '':
            m = l.getElementByString('idStr', a)
        if m:
            m.dmTbClosed = True
    this.dmTbRemoveBoxOrig(a)

def dmTbBoxUp(e):
    m = e.currentTarget
    if not (this.dmTbPress is m):
        return
    this.dmTbPress = None
    if m.dmTbClosed:
        return
    if this.dmTbIn(e.target, m.close_mc):
        return
    m.dmTbClosed = True
    this.xcloseBtnFunc()
'''


def patch(data):
    """tutorialBox.swf -> tutorialBox_p.swf"""
    import dos2_touchkit as tk
    swf = Swf(data); d = tk.Doc(swf, 'dmTbInitOrig'); abc = d.abc
    made = d.compile(CODE, {})
    for trait, mine, keep, n in REPOINT:
        t = abc.trait(d.ci, trait); assert t is not None and t.kind & 15 == 1, trait
        assert len(abc.methods[t.b].params) == n == len(abc.methods[made[mine]].params), '%s: %d parameters' % (trait, n)
        assert d.repoint(trait, mine, keep), trait
    d.add(*ADDED)
    d.done()
    return swf.write()


# ---------------------------------------------------------------- the same source in Python (taps on a display tree built by hand)
class _O:
    """a display object / dynamic object: unknown properties read as None (AS3: undefined)"""
    def __init__(s, name, parent=None, **kw):
        s._n, s.parent, s._l = name, parent, {}
        s.visible = True
        s.__dict__.update(kw)
    def __getattr__(s, n): return None
    def __bool__(s): return True
    def __repr__(s): return s._n
    def addEventListener(s, ev, fn): s._l.setdefault(ev, []).append(fn)


class _Ev:
    def __init__(s, cur, target): s.currentTarget, s.target = cur, target


class _List(_O):
    def __init__(s): super().__init__('nonModalList'); s.items = []
    @property
    def length(s): return len(s.items)
    def getAt(s, i): return s.items[int(i)] if 0 <= int(i) < len(s.items) else None
    def getElementByString(s, k, v): return next((m for m in s.items if getattr(m, k) == v), None)


class _Doc(_O):
    """the movie's document with the PC bodies the wrappers keep (what they do to the engine goes to .log)"""
    def __init__(s, fns):
        super().__init__('root'); s._fns = fns; s.log = []; s.modalBoxVisible = False
        s.tutorialBox_mc = b = _O('tutorialBox_mc', s)
        b.text_txt = _O('text_txt', b); b.ok_mc = _O('ok_mc', b); b.checkbox_mc = _O('checkbox_mc', b); _O('box', b.checkbox_mc)
        b.ok_mc.text_txt = _O('ok_text', b.ok_mc)
        s.nonModalPointer_mc = p = _O('nonModalPointer_mc', s, visible=False)
        p.text_txt = _O('ptr_text', p); p.close_mc = _O('ptr_close', p)
        s.nonModalHolder_mc = _O('nonModalHolder_mc', s); s.nonModalList = _List()
        s.world = _O('world', s)
    def __getattr__(s, n):
        mine = {t: m for t, m, _, _ in REPOINT}
        if n in mine: return s._fns[mine[n]]
        if n in s._fns: return s._fns[n]
        return None
    # the PC bodies (kept as dmTb*Orig)
    def dmTbInitOrig(s): s.log.append('init')
    def dmTbFadeInOrig(s, *a): s.modalBoxVisible = True; s.log.append('modal in')
    def dmTbFadeOutOrig(s): s.log.append('fadeOutModal')   # its tween ends in fadeOutModalDone -> OkPressed (done() below)
    def dmTbPtrInOrig(s, *a): s.nonModalPointer_mc.visible = True; s.log.append('pointer in')
    def dmTbPtrOutOrig(s): s.log.append('pointer out')
    def dmTbAddBoxOrig(s, idstr, *a):
        lst = s.nonModalList; m = lst.getElementByString('idStr', idstr)
        if not m:
            lst.items = []; m = _O('popup_' + idstr, s.nonModalHolder_mc, idStr=idstr); m.text_txt = _O('text_' + idstr, m); m.close_mc = _O('x_' + idstr, m)
            lst.items.append(m)
        s.log.append('box %s' % idstr)
    def dmTbRemoveBoxOrig(s, idstr): s.log.append('remove %s' % idstr)
    def xcloseBtnFunc(s): s.log.append('cancelNonModal'); s.fadeOutNonModalPointer()
    def done(s): s.modalBoxVisible = False; s.log.append('OkPressed')


def run_py():
    """the movie's code over a display tree -> (document, tap(target), press(down on, up on))"""
    fns = {}
    doc = _Doc(fns)
    ns = {'this': doc, 'None': None, 'True': True, 'False': False}
    exec(CODE, ns)
    fns.update({k: v for k, v in ns.items() if k.startswith('dmTb')})
    def press(down, up):   # mouse down on one object, up on another: each event bubbles from its target to the root
        for ev, tg in (('mouseDown', down), ('mouseUp', up)):
            t = tg
            while t:
                for fn in list(t._l.get(ev, [])): fn(_Ev(t, tg))
                t = t.parent
    return doc, (lambda tg: press(tg, tg)), press


# ---------------------------------------------------------------- sources, hook
def sources(path):
    if os.path.isdir(path): return {SRC: (Path(path) / SRC).read_bytes()}
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == GUI + SRC]
    if not e: sys.exit('%s: no %s' % (path, SRC))
    return {SRC: pak.read(e[0])}


def make(src): return {OUT: patch(src[SRC])}


_host = None
def install(host):
    """called at the end of tools/dos2_pchud_touch.py"""
    global _host
    _host = host
    h_sources, h_build, h_check = host.sources, host.build, host.check
    def sources_(path):
        s = h_sources(path); s.update(sources(path)); return s
    def build_(src):
        out = h_build(src); out.update(make(src)); return out
    def check_(src): return h_check(src) + check(src)
    host.sources, host.build, host.check = sources_, build_, check_


# ---------------------------------------------------------------- data
def _romfs(rel):
    """a file next to the Switch Game.pak setup gave (romfs/Data/<rel>: the player's game folder), else the dev tree's work/romfs/Data/<rel>"""
    try:
        import dos2_pchud_touch
        sw = getattr(dos2_pchud_touch, 'SW_PAK', None)
    except ImportError: sw = None
    if sw and Path(sw).is_file() and (Path(sw).parent / rel).exists(): return Path(sw).parent / rel
    return _find('work/romfs/Data/' + rel)


def _find(rel):
    for d in Path(__file__).resolve().parents:
        p = d / rel
        if p.exists(): return p
    return None


def parse_lsb(d):
    """an LSB (binary LSX) file -> [(region, (node name, {attribute: value}, [children]))]; strings decoded, TranslatedString = (text, handle)"""
    p = 0x28
    n, = struct.unpack_from('<I', d, p); p += 4
    names = {}
    for _ in range(n):
        ln, = struct.unpack_from('<I', d, p); p += 4
        s = d[p:p + ln].decode('utf-8', 'replace').rstrip('\0'); p += ln
        i, = struct.unpack_from('<I', d, p); p += 4
        names[i] = s
    rc, = struct.unpack_from('<I', d, p); p += 4
    regs = []
    for _ in range(rc):
        k, o = struct.unpack_from('<II', d, p); p += 8; regs.append((names[k], o))
    def rstr(p):
        ln, = struct.unpack_from('<I', d, p); p += 4
        return d[p:p + ln].decode('utf-8', 'replace').rstrip('\0'), p + ln
    def node(p):
        nm, na, nc = struct.unpack_from('<III', d, p); p += 12
        attrs = {}
        for _ in range(na):
            an, at = struct.unpack_from('<II', d, p); p += 8
            if at in (20, 21, 22, 23, 29, 30): v, p = rstr(p)
            elif at == 28: v, p = rstr(p); h, p = rstr(p); v = (v, h)
            elif at in (1, 19, 27): v = d[p]; p += 1
            elif at in (2, 3): v = struct.unpack_from('<h' if at == 2 else '<H', d, p)[0]; p += 2
            elif at in (4, 5, 6): v = struct.unpack_from({4: '<i', 5: '<I', 6: '<f'}[at], d, p)[0]; p += 4
            elif at in (24, 26, 32): v = struct.unpack_from('<q', d, p)[0]; p += 8
            elif at == 31: v = d[p:p + 16].hex(); p += 16
            else: raise ValueError('LSB attribute type %d at %#x' % (at, p))
            attrs[names[an]] = v
        kids = []
        for _ in range(nc):
            k, p = node(p); kids.append(k)
        return (names[nm], attrs, kids), p
    return [(rn, node(o)[0]) for rn, o in regs]


def tut_table(pak_path):
    """TutorialMessages.lsb of a pak -> {key: (English text, handle)} or None"""
    import lspk
    pak = lspk.Pak(str(pak_path)); e = [e for e in pak.entries if e[0] == TUT_LSB]
    if not e: return None
    (_, (_, _, kids)), = parse_lsb(pak.read(e[0]))
    return {a['UUID']: tuple(a['Content']) for _, a, _ in kids}


def base_keys(table): return {k: v for k, v in table.items() if not re.search(r'_(Title|Controller|Touch)$', k) and not k.startswith('TUT_CAT_')}


def shown_keys(table): return {k: v for k, v in table.items() if not k.endswith('_Title') and not k.startswith('TUT_CAT_')}


def loc_texts(table, loc_dir):
    """the shipped texts of the table's keys: {handle: {language: text}} from <loc_dir>/English.pak and <loc_dir>/<Language>/<Language>.pak"""
    import lspk
    hs = {v[1] for v in table.values()}; out = {}
    paks = [Path(loc_dir) / 'English.pak'] + sorted(p for p in Path(loc_dir).glob('*/*.pak') if p.stem == p.parent.name)
    for p in paks:
        if not p.exists(): continue
        pak = lspk.Pak(str(p))
        for e in pak.entries:
            if e[0].endswith('.xml') and '/Gender/' not in e[0]:
                t = pak.read(e).decode('utf-8', 'replace')
                for h, v in re.findall(r'<content contentuid="([^"]+)"[^>]*>(.*?)</content>', t, re.S):
                    if h in hs: out.setdefault(h, {})[p.stem] = html.unescape(v)
    return out


def reasons(table, loc):
    """key -> (languages with a key token, mouse words EN, mouse words RU) for every key the rule hits (KEEP included)"""
    out = {}
    for k, (e, h) in shown_keys(table).items():
        d = loc.get(h, {})
        tl = [l for l, s in sorted(d.items()) if TOKEN.search(s)] + (['lsb'] if TOKEN.search(e) else [])
        me = [m.group(0) for s in (e, d.get('English', '')) for m in [MOUSE.search(s)] if m]
        mr = [m.group(0) for m in [MOUSE_RU.search(d.get('Russian', ''))] if m]
        if tl or me or mr: out[k] = (tl, me, mr)
    return out


def keyboard_only(table, loc): return [k for k in shown_keys(table) if k in reasons(table, loc) and k not in KEEP]


def host_keys():
    s = HDR.read_text(encoding='utf-8')
    return re.findall(r'"(TUT_\w+)"', s.split('k_tu_off[]')[1].split('};')[0])


def host_words(name):
    s = HDR.read_text(encoding='utf-8')
    return [int(x, 16) for x in re.findall(r'0x[0-9a-f]+', s.split(name)[1].split('};')[0])]


def _loc(pak_path, handles):
    import lspk
    pak = lspk.Pak(str(pak_path)); out = {}
    for e in pak.entries:
        if re.search(r'/(english|russian)\.xml$', e[0]) and '/Gender/' not in e[0]:
            t = pak.read(e).decode('utf-8')
            for h, v in re.findall(r'<content contentuid="([^"]+)"[^>]*>(.*?)</content>', t, re.S):
                if h in handles: out[h] = html.unescape(v)
    return out


# ---------------------------------------------------------------- checks
def check(src, main_raw=None, ipad=None):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    print('-- tools/dos2_ptutorial.py: the tutorial window for touch')
    off = host_keys()
    pc_off = [k for k in off if '_Controller' not in k]
    say(len(off) == len(set(off)) == N_OFF and len(pc_off) == N_PC, 'host list hle_dos2_touch_tuts.h k_tu_off: %d keys, unique (%d PC keys, %d console keys the client may name itself)' % (
        len(off), len(pc_off), len(off) - len(pc_off)))
    # the engine's table and the shipped texts
    sp = Path(src['_shared_pak']) if '_shared_pak' in src else _romfs('Shared.pak')
    tab = tut_table(sp) if sp else None
    ld = _romfs('Localization/English.pak')
    if tab is None or ld is None: say(False, 'Switch Shared.pak with %s or Localization/English.pak not found' % TUT_LSB)
    else:
        base, shown = base_keys(tab), shown_keys(tab)
        loc = loc_texts(tab, ld.parent)
        langs = sorted({l for d in loc.values() for l in d})
        say(len(tab) == N_KEYS and len(base) == N_BASE, 'Switch %s: %d keys, %d base keys (no _Title / _Controller / TUT_CAT_), %d that can be shown' % (TUT_LSB, len(tab), len(base), len(shown)))
        say(len(langs) == 14 and all(len(loc.get(v[1], {})) == 14 for v in shown.values()), 'every one has its text in all %d Switch languages (%s ...)' % (len(langs), ' '.join(langs[:5])))
        why = reasons(tab, loc); kb = keyboard_only(tab, loc)
        same = sorted(kb) == sorted(off)
        say(same, 'the rule (a key token in any language, a mouse act in English / Russian) gives exactly the host list (%d)%s' % (
            len(kb), '' if same else ': rule only %s, host only %s' % (sorted(set(kb) - set(off)), sorted(set(off) - set(kb)))))
        say(all(k in why and k in base for k in KEEP) and all(not TOKEN.search(loc[base[k][1]].get(l, '')) for k in KEEP if k != 'TUT_PressurePlates' for l in ('English', 'Russian')),
            'kept against the rule, none with a key in English / Russian: %s' % '; '.join('%s (%s)' % kv for kv in KEEP.items()))
        lsb_only = [k for k in base if k not in off and TOKEN.search(base[k][0])]
        moved = sorted(k for k in pc_off if not TOKEN.search(base[k][0]) and not MOUSE.search(base[k][0]))
        say(not lsb_only and moved == ['TUT_Equipment', 'TUT_Locked_UseLockpick', 'TUT_Stealing_Noticed'],
            'the table\'s own English is older than the shipped texts: %s need a key only there (english.xml / russian.xml ...)' % ' '.join(moved))
        ru = {k: loc[base[k][1]].get('Russian', '') for k in pc_off}
        ruk = [k for k in pc_off if TOKEN.search(ru[k]) or MOUSE_RU.search(ru[k])]
        say(len(ruk) >= 40, 'Russian (the owner\'s game): %d of the %d PC texts left out name a key or the mouse there' % (len(ruk), len(pc_off)))
        story = {'TUT_Movement', 'TUT_Camera', 'TUT_SaveLoad', 'TUT_Inventory', 'TUT_QuestLog'}
        say(story <= set(off), 'the first tutorials of a new game (%s) are in the host list' % ' '.join(sorted(story)))
        if ipad:
            it = tut_table(Path(ipad) / 'MinData' / 'MinDataMods.pak')
            if it is None: say(False, '--ipad: no %s in %s/MinData/MinDataMods.pak' % (TUT_LSB, ipad))
            else:
                tw = {k[:-6] for k in it if k.endswith('_Touch')}
                say(set(tab) <= set(it) and set(it) - set(tab) == {k + '_Touch' for k in tw}, 'iPad: the Switch table + %d *_Touch keys (touch wording of its own)' % len(tw))
                no_tw = sorted(set(pc_off) - tw)
                say(set(no_tw) <= {'TUT_Character_Creation_Class_Basic', 'TUT_TradeBarter', 'TUT_HoldInHand_ContextMenu', 'TUT_HighlightCharacters', 'TUT_Locked_UseLockpick'},
                    'iPad: %d of our PC keys have a _Touch twin there (it reworded them too); without one: %s' % (len(set(pc_off) & tw), ' '.join(k[4:] for k in no_tw)))
                extra = sorted(k for k in tw - set(off) if k in base)
                say(all(not TOKEN.search(loc[base[k][1]].get(l, '')) for k in extra for l in ('English', 'Russian')),
                    'iPad: %d twins of texts without a key stay shown here as the PC text (%s)' % (len(extra), ' '.join(k[4:] for k in extra)))
        else: print('skip --ipad not given: the comparison with the iPad key set was not run')
    # the engine facts
    b = None
    for p in [main_raw, _find('work/exefs-update/main.raw')]:
        if p and os.path.exists(str(p)): b = open(str(p), 'rb').read(); break
    if b is None: say(False, 'main.raw not found')
    else:
        import capstone, numpy as np
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        def dis(a, n): return [(i.mnemonic, i.op_str) for i in md.disasm(b[a:a + 4 * n], a)]
        words = list(struct.unpack_from('<4I', b, 0x126a0d0))
        say(words == host_words('k_tu_insert_pro'), 'InsertTutorial main+0x126a0d0: the 4 prologue words of the host (no PC-relative one)')
        say(not any(m in ('adr', 'adrp', 'b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz', 'ldr') or m.startswith('b.') for m, _ in dis(0x126a0d0, 4)), 'the 4 words can run in a trampoline')
        w = np.frombuffer(b[:0x1d00000], dtype='<u4').astype(np.int64); pc = np.arange(len(w), dtype=np.int64) * 4
        imm = w & 0x3ffffff; imm = np.where(imm & (1 << 25), imm - (1 << 26), imm); tg = pc + imm * 4
        br = ((w & 0x7c000000) == 0x14000000)
        def callers(t): return sorted(int(x) * 4 for x in np.nonzero(br & (tg == t))[0])
        say(callers(0x126a0d0) == [0x126a088, 0x126b040] and b.find(struct.pack('<Q', 0x126a0d0)) < 0,
            'InsertTutorial is called from AddTutorial main+0x126b040 and the requeue main+0x126a088 only (no pointer to it)')
        say(callers(0x126aed0) == [0x10cedd0] and callers(0x10ced30) == [0xdc13d4], 'AddTutorial <- main+0x10cedd0 (the player\'s box) <- client ShowTutorial main+0xdc13d4 only')
        say(len(callers(0xdc0ec0)) >= 70, 'client ShowTutorial main+0xdc0ec0 has %d call sites (story message + the client\'s own triggers): all go this one way' % len(callers(0xdc0ec0)))
        add = dis(0x126b034, 5)
        say(add[:4] == [('mov', 'x1, sp'), ('mov', 'x0, x19'), ('mov', 'w2, wzr'), ('bl', '#0x126a0d0')] and add[4] == ('mov', 'x0, x25'),
            'AddTutorial: InsertTutorial(box, &entry on its stack, 0), its result unused (main+0x126b034)')
        key = dis(0x126aef8, 2) + dis(0x126af44, 2) + dis(0x126af74, 1)
        say(key[0] == ('mov', 'x25, x2') and key[2] == ('ldr', 'x21, [x25]') and key[3] == ('add', 'x20, x28, #0x10') and key[4] == ('str', 'x21, [sp, #0x10]'),
            'the entry\'s key (+0x10) is the FixedString AddTutorial got by reference (x2)')
        rk = dis(0xdc1604, 6)
        say(rk[0] == ('adrp', 'x8, #0x2bb8000') and rk[1] == ('ldrb', 'w8, [x8, #0x918]') and rk[5] == ('cbz', 'w8, #0xdc1738') and b[0x1ebb575:0x1ebb581] == b'_Controller\0',
            'the key: "_Controller" only when mode byte main+0x2bb8918 != 0; 0 (touch) keeps the base key (main+0xdc1604)')
        inv = {}
        for a in range(0x12650b0, 0x1265270, 0x1c):
            ins = dis(a, 7)
            if len(ins) == 7 and ins[4][0] == 'add' and ins[5][0] == 'mov' and ins[6] == ('blr', 'x8'):
                t = int(ins[3][1].split('#')[1], 16) + int(ins[4][1].split('#')[1], 16); n = 0 if ins[5][1].endswith('wzr') else int(ins[5][1].split('#')[1], 0)
                inv[b[t:b.index(b'\0', t)].decode()] = n
        want = {'fadeInModal': 6, 'addNonModalBox': 7, 'removeNonModalBox': 8, 'fadeInNonModalPointer': 11, 'fadeOutNonModalPointer': 12}
        say(all(inv.get(k) == v for k, v in want.items()) and 'fadeOutModal' not in inv,
            'the box registers invokes %s; fadeOutModal is the movie\'s own' % ', '.join('%s=%d' % kv for kv in sorted(want.items(), key=lambda x: x[1])))
        say(b[0x1eb50a6:0x1eb50b5] == b'cancelNonModal\0' and b[0x1e94884:0x1e9488e] == b'OkPressed\0', 'its ExternalInterface handlers include cancelNonModal and OkPressed (main+0x12642e0, +0x1264308)')
    try:
        c = (ROOT / 'dm' / 'host' / 'hle_dos2_touchui.c').read_text(encoding='utf-8')
        ok = '{0x37, 0, "tutorialBox_p", "GUI/tutorialBox.swf"}' in c.split('static const tu_def k_touch[]')[1].split('};')[0]
    except (OSError, IndexError): ok = False
    say(ok, 'k_touch maps type 0x37 (GUI/tutorialBox.swf) to tutorialBox_p (touch only)')
    try:
        t = (ROOT / 'dm' / 'host' / 'hle_dos2_touch_tips.c').read_text(encoding='utf-8')
        ok = 'dm_dos2_touch_tuts_install();' in t.split('void dm_dos2_touch_tips_install(void) {')[1][:200]
    except (OSError, IndexError): ok = False
    say(ok, 'the hook is installed first thing in dm_dos2_touch_tips_install (main.c host_preload)')
    # the movie
    data = src[SRC]; after = patch(data)
    print('%s -> %s: %d B -> %d B, sha256 %s' % (SRC, OUT, len(data), len(after), hashlib.sha256(after).hexdigest()[:16]))
    s = Swf(after); (_, _, Bc), = s.abcs(); (_, _, B0), = Swf(data).abcs()
    errs = verify_all(Bc)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies' % len(Bc.bodies))
    say(Swf(Swf(after).write()).body() == s.body(), 'the movie parses and round-trips')
    g = gr.check(data, after, quiet=True); say(g == [], 'every original tag of the PC movie kept%s' % ('' if g == [] else ': ' + str(g)))
    import dos2_ui_audit as au
    ein = lambda abc: {n for rec in au.scan(abc).values() for n, _ in rec['ei'] if n != '<dynamic>'}
    say(ein(B0) == ein(Bc), 'ExternalInterface names exactly the PC movie\'s (%s ...)' % ' '.join(sorted(ein(B0))[:6]))
    own = {v: k for k, v in Bc.owners().items()}
    def body(name): return '\n'.join(Bc.dis(Bc.methods[own[DOC + '::' + name]].body))
    traits_ok = all(own.get(DOC + '::' + keep) is not None and keep in body(trait) for trait, _, keep, _ in REPOINT)
    say(traits_ok, '%d traits re-pointed, each original kept and called: %s' % (len(REPOINT), ' '.join(t for t, _, _, _ in REPOINT)))
    say(all(own.get(DOC + '::' + n) is not None for n in ADDED), 'tap handlers added: %s' % ' '.join(ADDED))
    o0 = {v: k for k, v in B0.owners().items()}
    def body0(name): return '\n'.join(B0.dis(B0.methods[o0[DOC + '::' + name]].body))
    xc = body0('xcloseBtnFunc')
    say("'cancelNonModal'" in xc and 'fadeOutNonModalPointer' in xc and body('xcloseBtnFunc') == xc, 'xcloseBtnFunc (the X) = ExternalInterface cancelNonModal + fadeOutNonModalPointer, unchanged')
    say('fadeOutModal' in body0('onEventInit') and 'myFunc' in body0('onEventInit') and "'OkPressed'" in body0('fadeOutModalDone'),
        'the OK button\'s myFunc is this.fadeOutModal (now guarded), its tween ends in OkPressed')
    ab = body0('addNonModalBox')
    say("'idStr'" in ab and 'getElementByString' in ab and 'xcloseBtnFunc' in ab and 'clearElements' in ab,
        'addNonModalBox: one pop-up at a time (clearElements), found again by idStr, its X = xcloseBtnFunc')
    say(not Bc.instances[[i for i, c in enumerate(Bc.instances) if Bc.mname(c['name']) == 'nonModalPopup'][0]]['flags'] & 1, 'nonModalPopup is dynamic (it carries dmTbClosed / dmTbArmed)')
    fm = body0('fadeInModal')
    say('ok_mc' in fm and 'checkbox_mc' in fm and 'modalBoxVisible' in fm, 'the modal box has ok_mc and checkbox_mc (taps on them stay theirs)')
    # the same source in Python
    doc, tap, press = run_py()
    doc.onEventInit(); doc.onEventInit()
    tb, ptr = doc.tutorialBox_mc, doc.nonModalPointer_mc
    say([len(tb._l.get(e, [])) for e in ('mouseDown', 'mouseUp')] == [1, 1] and [len(ptr._l.get(e, [])) for e in ('mouseDown', 'mouseUp')] == [1, 1],
        'run: init twice -> one press and one release listener on the modal box and on the pointer')
    doc.log = []; doc.fadeInModal('t', 'T', 'x', 0, 0); tap(tb.text_txt); tap(tb.text_txt); tap(tb.ok_mc.text_txt); doc.done()
    say(doc.log == ['modal in', 'fadeOutModal', 'OkPressed'], 'run: modal box - a tap on its text = OK once; a second tap and OK while it fades do nothing: %s' % doc.log)
    doc.log = []; doc.fadeInModal('t', 'T', 'x', 0, 0); tap(tb.checkbox_mc.box); press(doc.world, tb.text_txt); tap(doc.world)
    say(doc.log == ['modal in'], 'run: the check box, a drag that ends on the box and a tap beside it do not close it')
    doc.fadeOutModal(); doc.done(); doc.log = []; tap(tb.text_txt)
    say(doc.log == [], 'run: the OK button\'s own path closes it; a tap on the hidden box does nothing')
    doc.log = []; tap(ptr.text_txt); doc.fadeInNonModalPointer('p', 1, 2, 0); tap(ptr.text_txt); tap(ptr.text_txt)
    say(doc.log == ['pointer in', 'cancelNonModal', 'pointer out'], 'run: pointer bubble - a tap before it shows does nothing, one tap = its X once: %s' % doc.log)
    doc.log = []; doc.fadeInNonModalPointer('p', 1, 2, 0); tap(ptr.close_mc); doc.fadeOutNonModalPointer(); tap(ptr.text_txt)
    say(doc.log == ['pointer in', 'pointer out'], 'run: a tap on its X is the X\'s; a tap while the engine fades it out does nothing')
    doc.log = []; doc.addNonModalBox('a', 'x', 'T', 't'); pa = doc.nonModalList.getAt(0); tap(pa.text_txt); tap(pa.text_txt)
    doc.removeNonModalBox('a'); tap(pa.text_txt)
    say(doc.log == ['box a', 'cancelNonModal', 'pointer out', 'remove a'], 'run: pop-up - one tap = its X once, nothing while it goes: %s' % doc.log)
    doc.log = []; doc.addNonModalBox('b', 'x', 'T', 't'); pb = doc.nonModalList.getAt(0); tap(pb.close_mc); doc.removeNonModalBox(''); tap(pb.text_txt)
    doc.addNonModalBox('b', 'x', 'T', 't2'); tap(pb.text_txt)
    say(doc.log == ['box b', 'remove ', 'box b', 'cancelNonModal', 'pointer out'] and [len(pb._l.get(e, [])) for e in ('mouseDown', 'mouseUp')] == [1, 1],
        'run: its X is the X\'s; removed by the engine -> no tap; shown again (same idStr) -> one tap closes it, listeners not doubled')
    return bad


def main():
    a = sys.argv[1:]
    ipad = a[a.index('--ipad') + 1] if '--ipad' in a else None
    if ipad: i = a.index('--ipad'); a = a[:i] + a[i + 2:]
    if len(a) >= 2 and a[0] == 'check':
        src = sources(a[1])
        sys.exit(1 if check(src, a[2] if len(a) > 2 else None, ipad) else 0)
    if len(a) == 3 and a[0] == 'build':
        out = Path(a[2]); out.mkdir(parents=True, exist_ok=True)
        d = make(sources(a[1]))[OUT]; (out / OUT).write_bytes(d); print('%s: %d B' % (out / OUT, len(d))); return
    if a and a[0] == 'list':
        sys.stdout.reconfigure(encoding='utf-8')
        tab = tut_table(a[1] if len(a) > 1 else _romfs('Shared.pak'))
        for k in host_keys(): print('%-36s %s' % (k, re.sub(r'<[^>]+>', '', tab.get(k, ('?',))[0])))
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
