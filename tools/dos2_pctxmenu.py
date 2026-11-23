"""[DOS2] UI mode TOUCH: "Description" / "Описание" in an item's context menu (owner 23.11, playing on the phone: "in the inventory a tap on an
item gives the context menu; the description would be better shown by a separate item 'Описание' in the context menu, if possible").
Static only: nothing here was played or run on a phone.

How the PC menu works (GUI/contextMenu.swf, UI types 0xa / 0xb; the item menu is 0xb: main+0x10cc3d0 takes it from the player record's node
+0x48 in mode 0): an item window's openContextMenu makes the engine invoke clearButtons, set `buttonArr` (7 values per entry: id, actionID,
clickSound, -, text, disabled, legal), invoke updateButtons (the movie builds the list from buttonArr: addButton -> windowsMenu_mc.addEntry, a
WEntry each), later open. A tap on an entry = WEntry.pressedButton = ExternalInterface buttonPressed(id, actionID, handle); a tap outside =
close() = menuClosed. The iPad's contextMenu_t has the same code (no entry of its own): there the description is the ShortLongPress. So ours
is new, the iPad's way of reaching it is not copied (nothing of the IPA is used).

contextMenu_p.swf = the player's PC contextMenu.swf with three methods re-pointed (the originals kept as dmCtx*Orig; no original body or tag
changed):
  MainTimeline.setTitle(t, b)  setTitle(SENTINEL, true) from the host (dm/host/hle_dos2_touch_ctxdesc.c: it sends it on an item window's
                               openContextMenu, queued before the class's own clearButtons / updateButtons; setTitle is the class's invoke 4,
                               registered at main+0x12ccfcc) only notes the time; any other title goes to the original
  MainTimeline.updateButtons() a fill within WIN_MS of that note gets one more entry at the BOTTOM of buttonArr (the engine's entries keep their
                               places, ids and actions): id = actionID = ENTRY_ID, text DESC_RU when any of the engine's texts has a Cyrillic
                               letter (the game's language), else DESC_EN; the note is used once. Then the original builds the list.
  WEntry.pressedButton()       our entry: the menu closes itself (MainTimeline.close: its menuClosed goes first), then ExternalInterface
                               dmCtxDesc(1) - the host shows that item's tooltip (the hover's showItemTooltip). Every other entry: the original
                               (buttonPressed to the engine).
k_touch maps type 0xb to contextMenu_p (dm/host/hle_dos2_touchui.c; touch set only - 0xa, SWITCH and HYBRID keep the stock movie).

  python -I tools/dos2_pctxmenu.py check <Switch Game.pak | dir of the PC movies> [main.raw]   every static check
  python -I tools/dos2_pctxmenu.py build <src> <out dir>                                       contextMenu_p.swf
Hook: tools/dos2_pchud_touch.py calls install() at its end; build() then also returns contextMenu_p.swf and check() runs check() here (so the
`touch` set of tools/dos2_touchui_pak.py packs it).
"""
import hashlib, os, re, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, MN_MULTIL, NS_PUBLIC
import dos2_swf_graft as gr

ROOT = Path(__file__).resolve().parent.parent
HDR = ROOT / 'dm' / 'host' / 'hle_dos2_touch_ctxdesc.h'
HOST = ROOT / 'dm' / 'host' / 'hle_dos2_touch_ctxdesc.c'
GUI = 'Public/Game/GUI/'
SRC, OUT = 'contextMenu.swf', 'contextMenu_p.swf'
DOC, ENTRY = 'contextMenu_fla.MainTimeline', 'WEntry'
ITEM_MOVIES = ['partyInventory.swf', 'characterSheet.swf', 'containerInventory.swf']   # the item windows whose calls the host reads
NS_AS3 = 'http://adobe.com/AS3/2006/builtin'
DESC_EN, DESC_RU = 'Description', 'Описание'
WIN_MS = 2000
CYR = (0x400, 0x4ff)


def host_consts():
    """the numbers shared with the host (hle_dos2_touch_ctxdesc.h)"""
    s = HDR.read_text(encoding='utf-8')
    def d(n): return re.search(r'#define %s "([^"]*)"' % n, s).group(1)
    def e(n): return int(re.search(r'\b%s = (0x[0-9a-f]+|\d+)' % n, s).group(1), 0)
    return {'EI_NAME': d('CD_EI_NAME'), 'SENTINEL': d('CD_SENTINEL'), 'ENTRY_ID': e('CD_ENTRY_ID'), 'WIN_MS': e('CD_WIN_MS'), 'T_MENU': e('CD_T_MENU')}
H = host_consts()

# trait -> our method (the original kept as the name in the third column): class, trait, ours, keep, parameters
REPOINT = [(DOC, 'setTitle', 'dmCtxTitle', 'dmCtxTitleOrig', 2), (DOC, 'updateButtons', 'dmCtxFill', 'dmCtxFillOrig', 0),
           (ENTRY, 'pressedButton', 'dmCtxPress', 'dmCtxPressOrig', 0)]

# the wrappers in the Python subset tools/dos2_touchkit.py kc_compile turns into AVM2 (the same text runs in Python: run_py)
CODE_DOC = '''
def dmCtxTitle(t, b):
    if t == SENTINEL:
        this.dmCtxT = getTimer()
        return
    this.dmCtxTitleOrig(t, b)

def dmCtxFill():
    a = this.buttonArr
    t = this.dmCtxT
    if t and a and getTimer() - t < WIN_MS:
        this.dmCtxT = 0
        k = a.length
        if k >= 7:
            ru = False
            i = 4
            while i < k:
                s = String(a[i])
                j = 0
                n = s.length
                while j < n:
                    c = s.charCodeAt(j)
                    if c >= CYR0 and c <= CYR1:
                        ru = True
                        j = n
                    j = j + 1
                i = i + 7
            txt = DESC_EN
            if ru:
                txt = DESC_RU
            a[k] = ENTRY_ID
            a[k + 1] = ENTRY_ID
            a[k + 2] = True
            a[k + 3] = ''
            a[k + 4] = txt
            a[k + 5] = False
            a[k + 6] = True
    this.dmCtxFillOrig()
'''
CODE_ENTRY = '''
def dmCtxPress():
    if this.id == ENTRY_ID:
        r = this.root
        if r:
            r.close()
        EI.call(EI_NAME, 1)
        return
    this.dmCtxPressOrig()
'''


def env():
    return {'SENTINEL': ('const', H['SENTINEL']), 'WIN_MS': ('const', H['WIN_MS']), 'ENTRY_ID': ('const', H['ENTRY_ID']), 'EI_NAME': ('const', H['EI_NAME']),
            'DESC_EN': ('const', DESC_EN), 'DESC_RU': ('const', DESC_RU), 'CYR0': ('const', CYR[0]), 'CYR1': ('const', CYR[1])}


def patch(data):
    """contextMenu.swf -> (contextMenu_p.swf, {method: assembler lines})"""
    import dos2_touchkit as tk
    swf = Swf(data); (tag, pre, abc), = swf.abcs()
    t0 = abc.trait(abc.class_index(DOC), 'updateButtons'); ns = abc.mn[t0.name][1]
    assert abc.ns[ns] == (NS_PUBLIC, abc.string('')), 'updateButtons is not public'
    nsset = [ns]; abc.nssets.append(nsset); late = len(abc.mn); abc.mn.append((MN_MULTIL, len(abc.nssets) - 1))   # a[i]
    full = {'EI': ('lex', abc.pub('ExternalInterface', 'flash.external')), 'String': ('call', abc.pub('String')), 'getTimer': ('call', abc.pub('getTimer', 'flash.utils'))}
    full.update(env())
    made = {}
    made.update(tk.kc_compile(abc, ns, late, full, CODE_DOC)); made.update(tk.kc_compile(abc, ns, late, full, CODE_ENTRY))
    as3 = abc.qname(abc.namespace(8, NS_AS3), 'charCodeAt'); pub = abc.qname(ns, 'charCodeAt')   # a String method: in the AS3 namespace, as tools/dos2_ptouch_tips.py
    out = {}
    for cls, trait, mine, keep, n in REPOINT:
        ci = abc.class_index(cls); inst = abc.instances[ci]
        assert not inst['flags'] & 1, '%s is sealed (dynamic properties needed)' % cls
        t = abc.trait(ci, trait); assert t is not None and t.kind & 15 == 1, '%s::%s' % (cls, trait)
        assert abc.trait(ci, keep) is None, 'already patched: %s::%s' % (cls, keep)
        orig = abc.methods[t.b]; np, rest, lines = made[mine]
        assert np == n == len(orig.params) and not rest, '%s::%s: %d parameters' % (cls, trait, n)
        lines = [(l[0], as3) + l[2:] if len(l) > 1 and l[1] == pub and l[0].startswith('callprop') else l for l in lines]
        m = abc.add_method('dm_' + mine, list(orig.params), orig.ret, asm(lines), 64, 64, orig.body.init_scope, orig.body.init_scope + 1)
        if orig.flags & 0x08: abc.methods[m].flags |= 0x08; abc.methods[m].options = list(orig.options)   # setTitle(t, b = false)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        old = t.b; t.b = m; inst['traits'].append(Trait(abc.qname(ns, keep), 1, 0, old))
        out[mine] = lines
    swf.tags[tag][1] = pre + abc.write()
    return swf.write(), out


# ---------------------------------------------------------------- the same source in Python (a menu filled by hand)
class _Str(str):
    @property
    def length(s): return len(s)
    def charCodeAt(s, i): return ord(s[int(i)])


class _Arr(list):
    @property
    def length(s): return len(s)
    def __setitem__(s, i, v):
        i = int(i)
        while len(s) <= i: s.append(None)
        list.__setitem__(s, i, v)


class _Obj:
    def __getattr__(s, n): return None


def run_py(arr, title=None, dt=0, press=None):
    """the engine's fill `arr` (buttonArr) after setTitle(title) dt ms earlier, then a press on the entry with id `press` ->
    (the list the movie builds: [(id, text)], the log of what reached the engine / the host)"""
    clock = [1000]; log = []
    doc = _Obj(); entry = _Obj()
    def get_timer(): return clock[0]
    def ui_title(t, b): log.append('title %s' % t)
    def fill_orig():
        a = doc.buttonArr; doc.built = [(a[i], a[i + 4]) for i in range(0, len(a), 7)]; doc.buttonArr = _Arr()
    def close(): log.append('menuClosed')
    ei = type('EI', (), {'call': staticmethod(lambda *a: log.append('EI %s' % ', '.join(map(str, a))))})
    def press_orig(): log.append('buttonPressed %s' % entry.id)
    ns = {'this': doc, 'getTimer': get_timer, 'String': lambda v: _Str('null' if v is None else v), 'EI': ei, 'None': None, 'True': True, 'False': False}
    ns.update({k: v[1] for k, v in env().items()})
    exec(CODE_DOC, ns); exec(CODE_ENTRY, ns)
    doc.dmCtxTitleOrig, doc.dmCtxFillOrig, doc.close = ui_title, fill_orig, close
    if title is not None: ns['this'] = doc; ns['dmCtxTitle'](title, True)
    clock[0] += dt
    doc.buttonArr = _Arr(arr); ns['dmCtxFill']()
    if press is not None:
        ns['this'] = entry; entry.id = press; entry.root = doc; entry.dmCtxPressOrig = press_orig; ns['dmCtxPress']()
    return doc.built, log


# ---------------------------------------------------------------- sources, hook
def sources(path):
    need = [SRC] + ITEM_MOVIES
    if os.path.isdir(path): return {n: (Path(path) / n).read_bytes() for n in need if (Path(path) / n).exists()}
    import lspk
    pak = lspk.Pak(path); by = {e[0]: e for e in pak.entries}
    if GUI + SRC not in by: sys.exit('%s: no %s' % (path, SRC))
    return {n: pak.read(by[GUI + n]) for n in need if GUI + n in by}


def make(src): return {OUT: patch(src[SRC])[0]}


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


# ---------------------------------------------------------------- checks
def _ei_calls(data):
    """the movie's ExternalInterface calls: [(name, number of arguments)] (a constant name before the call)"""
    s = Swf(data); (_, _, A), = s.abcs(); out = []
    for m in A.methods:
        if not m.body: continue
        L = A.dis(m.body)
        for i, l in enumerate(L):
            g = re.search(r"pushstring\s+'(\w+)'", l)
            if not g or i == 0 or 'ExternalInterface' not in L[i - 1]: continue
            j = i
            while j < len(L) and not re.search(r'callpropvoid\s+call, (\d+)', L[j]): j += 1
            if j < len(L): out.append((g.group(1), int(re.search(r'call, (\d+)', L[j]).group(1)) - 1))
    return out


def check_main(b, say):
    import dos2_ptouch_gaps as pg
    src = HOST.read_text(encoding='utf-8')
    def words(name): return [int(x, 16) for x in re.findall(r'0x[0-9a-f]{8}', re.search(r'%s\[\d+\] = \{(.*?)\};' % name, src, re.S).group(1))]
    def site(name): return int(re.search(r'\b%s = (0x[0-9a-f]+)' % name, src).group(1), 16)
    for arr, off in (('k_cd_reg', 'CD_SITE_REG'), ('k_cd_menu', 'CD_SITE_MENU')):
        w = words(arr); a = site(off)
        say(list(struct.unpack_from('<%dI' % len(w), b, a)) == w, 'host site %s main+%#x: %d words as main.raw' % (arr, a, len(w)))
    reg = pg._scan(b, 0x12ccf40, 0x12cd010)[0]
    names = ['setAnchor', 'clearButtons', 'close', 'open', 'setTitle', 'addButton', 'updateButtons']
    say(all(n in reg for n in names) and site('CD_S_SETTITLE') in reg and b[site('CD_S_SETTITLE'):site('CD_S_SETTITLE') + 9] == b'setTitle\0',
        'the menu class registers its invokes %s (main+0x12ccf40..0x12cd010); setTitle = main+%#x, the one the host sends' % (' '.join(names), site('CD_S_SETTITLE')))
    w = struct.unpack_from('<I', b, 0x10cc430)[0]
    say(w == 0xf940250a,'main+0x10cc3d0 (the item menu getter of the item windows): PC branch reads node +0x48 (the type 0xb cell)')
    base = pg._scan(b, 0x10a5750, pg._end(b, 0x10a5750))[0]
    say('showItemTooltip' in base and 'hideTooltip' in base, 'showItemTooltip / hideTooltip are the game UI base\'s (main+0x10a5750): the host can send it to any item window')
    item = pg._scan(b, 0x13899a0, pg._end(b, 0x13899a0))
    say(0x10cc3d0 in item[2], 'the shared item menu main+0x13899a0 (openContextMenu of the inventory / containers) gets its menu from main+0x10cc3d0')
    miss = [n for n in (H['EI_NAME'], H['SENTINEL']) if b'\0' + n.encode() + b'\0' in b]
    say(not miss, '%s / %s are no strings of main.raw (no engine class takes them)%s' % (H['EI_NAME'], H['SENTINEL'], ' - FOUND: ' + ' '.join(miss) if miss else ''))


def check(src, main_raw=None):
    import dos2_ptouch_gaps as pg
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    print('-- tools/dos2_pctxmenu.py: Description in the item context menu (contextMenu_p)')
    data = src[SRC]; after, lines = patch(data)
    print('%s -> %s: %d B -> %d B, sha256 %s' % (SRC, OUT, len(data), len(after), hashlib.sha256(after).hexdigest()[:16]))
    s = Swf(after); (_, _, Bc), = s.abcs(); (_, _, B0), = Swf(data).abcs()
    errs = verify_all(Bc)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies' % len(Bc.bodies))
    say(Swf(Swf(after).write()).body() == s.body(), 'the movie parses and round-trips')
    g = gr.check(data, after, quiet=True); say(g == [], 'every original tag of the PC movie kept%s' % ('' if g == [] else ': ' + str(g)))
    e0, e1 = pg._ei_names(B0), pg._ei_names(Bc)
    say(e1 == e0 | {H['EI_NAME']} and H['EI_NAME'] not in e0, 'ExternalInterface names: the PC movie\'s + %s only' % H['EI_NAME'])
    keep_ok = True
    for cls, trait, mine, keep, n in REPOINT:
        c0, c1 = B0.class_index(cls), Bc.class_index(cls)
        t0, tk_, tn = B0.trait(c0, trait), Bc.trait(c1, keep), Bc.trait(c1, trait)
        keep_ok &= tk_ is not None and B0.methods[t0.b].body.code == Bc.methods[tk_.b].body.code and tn.b != tk_.b and len(Bc.methods[tn.b].params) == n
    say(keep_ok, 'setTitle / updateButtons / WEntry.pressedButton re-pointed, the PC bodies kept unchanged as dmCtx*Orig')
    own = {v: k for k, v in Bc.owners().items()}
    def dis(c, m): return '\n'.join(Bc.dis(Bc.methods[own[c + '::' + m]].body)) if c + '::' + m in own else ''
    d = dis(DOC, 'setTitle'); say("'%s'" % H['SENTINEL'] in d and 'getTimer' in d and 'dmCtxTitleOrig' in d, 'setTitle("%s", ...) notes the time, any other title -> the original' % H['SENTINEL'])
    d = dis(DOC, 'updateButtons')
    say('dmCtxFillOrig' in d and "'%s'" % DESC_EN in d and "'%s'" % DESC_RU in d and 'charCodeAt' in d and NS_AS3 in d and str(WIN_MS) in d and str(H['ENTRY_ID']) in d,
        'updateButtons: within %d ms of the note one entry (id %d, "%s" / "%s" by a Cyrillic letter in the engine\'s texts) after the engine\'s, then the original' % (WIN_MS, H['ENTRY_ID'], DESC_EN, DESC_RU))
    d = dis(ENTRY, 'pressedButton')
    say('dmCtxPressOrig' in d and "'%s'" % H['EI_NAME'] in d and 'close' in d and 'root' in d and d.index('close') < d.index("'%s'" % H['EI_NAME']),
        'WEntry.pressedButton: our id -> root.close() (menuClosed first), then %s(1); any other -> the original (buttonPressed)' % H['EI_NAME'])
    # behaviour (the same source in Python)
    en = [0, 11, True, '', 'Use', False, True, 1, 12, True, '', 'Drop', False, True]
    ru = [0, 11, True, '', 'Использовать', False, True, 1, 12, True, '', '<font color="#fff">Выбросить</font>', False, True]
    built, log = run_py(en, H['SENTINEL'], 30)
    say(built == [(0, 'Use'), (1, 'Drop'), (H['ENTRY_ID'], DESC_EN)] and log == [], 'run: an item menu (English game) = the engine\'s entries in their order + "%s" at the bottom' % DESC_EN)
    built, log = run_py(ru, H['SENTINEL'], 30)
    say(built == [(0, 'Использовать'), (1, '<font color="#fff">Выбросить</font>'), (H['ENTRY_ID'], DESC_RU)], 'run: Russian game -> "%s"' % DESC_RU)
    built, log = run_py(en)
    say(built == [(0, 'Use'), (1, 'Drop')], 'run: a menu the host did not mark (world, no item window) = the engine\'s entries only')
    built, log = run_py(en, H['SENTINEL'], WIN_MS + 1)
    say(built == [(0, 'Use'), (1, 'Drop')], 'run: a stale note (> %d ms: the engine opened no menu for it) is not used' % WIN_MS)
    built, log = run_py([], H['SENTINEL'], 30)
    say(built == [], 'run: an empty fill gets no lone entry')
    built, log = run_py(en, 'Chest', 0)
    say(log == ['title Chest'] and built == [(0, 'Use'), (1, 'Drop')], 'run: a real title goes to the original')
    built, log = run_py(en, H['SENTINEL'], 30, press=H['ENTRY_ID'])
    say(log == ['menuClosed', 'EI %s, 1' % H['EI_NAME']], 'run: Description pressed -> the menu closes (menuClosed), then %s(1); nothing to the engine\'s buttonPressed' % H['EI_NAME'])
    built, log = run_py(en, H['SENTINEL'], 30, press=1)
    say(log == ['buttonPressed 1'], 'run: an engine entry pressed -> the original buttonPressed')
    # the item windows: openContextMenu with the item third from the end, showItemTooltip with the 7 arguments the host replays
    calls = {n: _ei_calls(src[n]) for n in ITEM_MOVIES if n in src}
    ocm = {n: sorted({k for nm, k in c if nm == 'openContextMenu'}) for n, c in calls.items()}
    tip = {n: sorted({k for nm, k in c if nm == 'showItemTooltip'}) for n, c in calls.items()}
    say(len(calls) == 3 and all(v and set(v) <= {3, 4} for v in ocm.values()) and all(7 in v for v in tip.values()),
        'PC item windows: openContextMenu with %s arguments (item third from the end), showItemTooltip with %s' % (ocm, tip))
    # the host table and the host file
    try:
        tu = (ROOT / 'dm' / 'host' / 'hle_dos2_touchui.c').read_text(encoding='utf-8').split('static const tu_def k_touch[]')[1].split('};')[0]
    except (OSError, IndexError): tu = ''
    say('{0x0b, 0, "contextMenu_p", "GUI/contextMenu.swf"}' in tu and '"contextMenu_p", "GUI/contextMenu.swf"}' in tu and '{0x0a, 0, "contextMenu_p"' not in tu,
        'k_touch maps type 0xb (the item menu) to contextMenu_p; 0xa keeps the stock movie')
    hs = HOST.read_text(encoding='utf-8')
    say('dm_dos2_touchui_ei_watch(cd_watch' in hs and '"setTitle"' in hs and '"showItemTooltip"' in hs and '"openContextMenu"' in hs and 'CD_SENTINEL' in hs and H['T_MENU'] == 0x0b,
        'host (hle_dos2_touch_ctxdesc.c): an EI watcher - openContextMenu -> setTitle("%s"), %s(1) -> showItemTooltip of the item' % (H['SENTINEL'], H['EI_NAME']))
    b = pg._main(main_raw)
    if b is None: say(False, 'main.raw not found')
    else: check_main(b, say)
    return bad


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == 'check': sys.exit(1 if check(sources(a[1]), a[2] if len(a) > 2 else None) else 0)
    if len(a) == 3 and a[0] == 'build':
        out = Path(a[2]); out.mkdir(parents=True, exist_ok=True)
        for n, d in make(sources(a[1])).items(): (out / n).write_bytes(d); print('%s: %d B' % (out / n, len(d)))
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
