"""[DOS2] UI mode TOUCH: the loading screen's tips for a touch screen (owner 21.11: "why does the loading screen talk about left Shift and
holding Ctrl? Bring the iPad's tips; if they cannot be enabled without the IPA, write them in our own words with the same meaning").
docs/dos2/TOUCH_MODES_PLAN.md "Подсказки экрана загрузки в касании". Static only: nothing here was played or run on a phone.

How the engine picks a tip (main.raw 4979b200; the host half is dm/host/hle_dos2_touch_tips.c, its facts in hle_dos2_touch_tips.h): the
table is Public/Game/Hints/Hints.lsx (HintKey, Description handle, Keyboard, Controller); the loading screen (type 0x17,
GUI/loadingScreen.swf) takes a random entry every HintDuration (5) seconds - mode byte main+0x2bb8918 = 0 (the PC UI the touch mode runs)
-> an entry with Keyboard = 1, else Controller = 1 - replaces its [IE_*] tokens by the bindings of the mode (keyboard: "Left Shift",
"Ctrl" ...) and hands the text to the movie's setInfoText (invoke index 1, the only call of that index in the class). The iPad's
Hints.lsx has a third flag, TouchScreen: 38 general tips + 17 touch-only ones, no keyboard tip. Its touch-only texts exist only in the
iPad's localization (handles the Switch english.xml / russian.xml do not have), so they cannot be switched on: the release carries no
iPad file or text (docs/dos2/TOUCH_ART.md). So:
  host   while the HUD part of the mode is TOUCH the keyboard-only entries (hle_dos2_touch_tips.h k_tt_off, 14 keys) leave the PC pick:
         their Keyboard byte is cleared in the loaded table, as the iPad leaves them out of its TouchScreen set. The 38 general tips stay,
         in the game's language.
  movie  loadingScreen_p.swf = the player's PC loadingScreen.swf with MainTimeline::setInfoText wrapped (the original kept as dmTipOrig):
         the 1st, 3rd, 5th ... call shows one of OUR touch tips (TIPS below: our own words for every iPad touch tip that our gestures do,
         and touch versions of the keyboard tips; English, Russian when the engine's text has a Cyrillic letter: the game's language
         decides), rotating from a random start; the 2nd, 4th ... show the engine's tip as it came.
  k_touch  type 0x17 -> loadingScreen_p (dm/host/hle_dos2_touchui.c; touch set only - SWITCH / HYBRID keep the stock movie).
Why not the console set (mode byte != 0): its entries carry pad tokens too ([RightStickX], [RightStickY], [IE_DestructionToggle],
[IE_ToggleTacticalCamera]) and the byte switches the whole UI.

  python -I tools/dos2_ptouch_tips.py check <Switch Game.pak | dir of the PC movies> [main.raw] [--ipad <iPad .app/Data dir>]
         every static check; --ipad also proves no tip repeats the iPad's wording (5-word runs against its english.xml / russian.xml)
  python -I tools/dos2_ptouch_tips.py build <src> <out dir>     loadingScreen_p.swf
  python -I tools/dos2_ptouch_tips.py list                      the tips (EN / RU) with what each stands for
Hook: tools/dos2_pchud_touch.py calls install() at its end; build() then also returns loadingScreen_p.swf and check() runs check() here
(so the `touch` set of tools/dos2_touchui_pak.py packs it).
"""
import hashlib, os, re, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, verify_all, asm, MN_MULTIL, NS_PUBLIC
import dos2_swf_graft as gr

ROOT = Path(__file__).resolve().parent.parent
GUI = 'Public/Game/GUI/'
SRC, OUT = 'loadingScreen.swf', 'loadingScreen_p.swf'
DOC = 'loadingScreen_fla.MainTimeline'
NS_AS3 = 'http://adobe.com/AS3/2006/builtin'   # String methods (the movie's own code calls toLowerCase / substr in it)
MAX_LEN = 150                                  # the engine's longest tip is ~140 characters: the hint box fits that

# Our tips. `from` = what each stands for: iPad TouchScreen-only hint keys (meaning only, our wording, adapted to what OUR touch gestures do:
# docs/dos2/TOUCH_MODES_PLAN.md section 2 and "Implemented: touch gestures") and Switch Keyboard-only keys (their touch version).
TIPS = [
    ('info', ['HINT_LONG_PRESS_UI_HINT'],
     'Rest a finger on a button or an item for a moment to read what it is.',
     'Задержите палец на кнопке или предмете, чтобы прочитать, что это.'),
    ('zoom', ['HINT_PINCH_CAMERA_ZOOM', 'HINT_CAM_ZOOM'],
     'Pinch or spread two fingers to move the camera away or closer.',
     'Сведите или разведите два пальца, чтобы отдалить или приблизить камеру.'),
    ('pan', ['HINT_DRAG_CAMERA'],
     'Look around: slide one finger across the world to move the camera.',
     'Осмотритесь: проведите пальцем по миру, чтобы сдвинуть камеру.'),
    ('rotate', ['HINT_TWO_FINGER_ROTATE_CAMERA', 'HINT_CAM_ROTATE'],
     'Turn two fingers on the screen like a dial to rotate the camera.',
     'Поверните два пальца на экране, как ручку, чтобы вращать камеру.'),
    ('menu', ['HINT_LONG_PRESS_CONTEXT_MENU'],
     'A long press on an item opens its menu with everything you can do with it.',
     'Долгое нажатие на предмет открывает его меню со всеми действиями.'),
    ('use', ['HINT_DOUBLE_TAP_ITEM_USE'],
     'In the inventory, double tap an item to use it: eat, drink a potion, put on a sword.',
     'В инвентаре дважды коснитесь предмета, чтобы применить его: съесть, выпить зелье, надеть меч.'),
    ('track', ['HINT_DOUBLE_TAP_MARKER_ON_MAP'],
     'On the map, tap a marker to follow it; tap it again to stop following.',
     'На карте коснитесь метки, чтобы следовать к ней; коснитесь ещё раз, чтобы перестать.'),
    ('mark', ['HINT_TOUCH_AND_HOLD_TO_CREATE_MARKER'],
     'Tap an empty spot on the map to put your own marker there; a long press on it removes it.',
     'Коснитесь пустого места на карте, чтобы поставить свою метку; долгое нажатие на неё убирает её.'),
    ('mapzoom', ['HINT_PINCH_TO_ZOOM_MAP'],
     'Pinch two fingers on the map to change its scale.',
     'Щипок двумя пальцами на карте меняет её масштаб.'),
    ('aim', ['HINT_DRAG_TO_AIM_SPELLS'],
     'After picking a skill, tap where it should go, then press the circle with the tick. The cross cancels it.',
     'Выбрав навык, коснитесь места, куда его направить, и нажмите круг с галочкой. Крестик отменяет.'),
    ('decks', ['HINT_SWIPE_HOTBAR_TO_CHANGE_DECKS'],
     'Swipe along the hotbar to flip through your skill decks.',
     'Проведите пальцем вдоль панели быстрого доступа, чтобы листать колоды навыков.'),
    ('surface', ['HINT_TOUCH_AND_HOLD_SURFACES'],
     'Rest a finger on a surface for a moment to find out what it is.',
     'Задержите палец на поверхности, чтобы узнать, что это за поверхность.'),
    ('labels', ['HINT_TAP_WITH_TWO_FINGERS_TO_SHOW_PICKABLES'],
     'A two-finger tap labels the items nearby that you can take; another one hides the labels.',
     'Коснитесь двумя пальцами, чтобы подписать предметы рядом, которые можно взять; ещё одно касание прячет подписи.'),
    ('scroll', ['HINT_SCROLL_VERTICAL_LISTS_WITH_TWO_FINGERS'],
     'Move two fingers up or down to scroll a long list.',
     'Ведите двумя пальцами вверх или вниз, чтобы прокрутить длинный список.'),
    ('center', ['HINT_CAM_HOME'],
     'Double tap a portrait to bring the camera back to that character.',
     'Дважды коснитесь портрета, чтобы вернуть камеру к этому персонажу.'),
    ('traps', ['HINT_TRAPS'],
     'To cross a trapped area, turn on sneaking from the hotbar actions and bring the camera close with a pinch.',
     'Чтобы пройти через ловушки, включите режим скрытности в действиях панели быстрого доступа и приблизьте камеру щипком.'),
    ('destroy', ['HINT_CTRL'],
     'A long press on a barrel, a chair or even a friend offers Attack: break lifeless things or strike the unsuspecting.',
     'Долгое нажатие на бочку, стул или даже союзника предложит «Атаковать»: ломайте вещи или бейте ничего не подозревающих.'),
    ('tactical', ['HINT_TACTICALVIEW'],
     'In combat, swipe down with three fingers for the tactical view and up to leave it.',
     'В бою проведите тремя пальцами вниз, чтобы включить тактический обзор, и вверх, чтобы выйти из него.'),
    ('outline', ['HINT_OUTLINE'],
     'Losing sight of characters in a fight? Turn on character outlines in the Options.',
     'Теряете персонажей из виду в бою? Включите их выделение в параметрах.'),
    ('examine', ['HINT_PERCEPTION'],
     'To examine someone, long press them and pick Examine. The higher your Loremaster, the more you learn.',
     'Чтобы осмотреть персонажа, долго нажмите на него и выберите «Осмотреть». Чем выше Знание легенд, тем больше узнаете.'),
    ('pockets', ['HINT_PICKPOCKET'],
     'While sneaking, tap a townsperson to go through their pockets.',
     'В режиме скрытности коснитесь горожанина, чтобы обшарить его карманы.'),
]
# iPad touch-only tips without a tip of ours, and why (the check proves every iPad touch key is in TIPS or here)
NO_TIP = {'HINT_TURN_OFF_MAGNIFYING_GLASS': 'no magnifying glass in our touch mode',
          'HINT_USE_MAGNIFYING_GLASS': 'no magnifying glass in our touch mode',
          'HINT_TOUCH_MOVE_OBJECTS': 'a one-finger slide in the world moves the camera here; moving objects by dragging is not a gesture of ours'}
# Switch keyboard-only keys (hle_dos2_touch_tips.h k_tt_off) without a touch version, and why
NO_TWIN = {'HINT_STACKS': 'splitting a stack needs a held key; no gesture for it',
           'HINT_QUEUE': 'queued orders need a held key; no gesture for it',
           'HINT_ELEMWEAPON': 'a weapon strike on the ground needs a held key + click; no gesture for it',
           'HINT_EDGEPANNING': 'no screen edge panning on a phone',
           'HINT_THEFT': 'the red cursor has no touch counterpart (no cursor)'}
# words a touch tip must never use (keys, mouse); the engine's tips that use them are the ones the host takes out
KEYWORDS = re.compile(r'(?i)\b(shift|ctrl|alt|click|mouse|cursor|keyboard|key|hover|scroll wheel)\b|\[|клавиш|щ[её]лк|мыш|курсор')
CYR = re.compile('[Ѐ-ӿ]')

# The wrapper in the Python subset tools/dos2_touchkit.py kc_compile turns into AVM2 (the same text runs in Python: run_py below)
CODE = '''
def dmTipSet(t):
    n = this.dmTipN
    if not n:
        n = 0
    this.dmTipN = n + 1
    if n % 2 == 1:
        this.dmTipOrig(t)
        return
    s = String(t)
    ru = False
    i = 0
    k = s.length
    while i < k:
        c = s.charCodeAt(i)
        if c >= 1024 and c <= 1279:
            ru = True
            i = k
        i = i + 1
    j = this.dmTipI
    if j == None:
        j = Math.floor(Math.random() * NTIPS)
    this.dmTipI = j + 1
    if ru:
        tips = TIPS_RU
    else:
        tips = TIPS_EN
    this.dmTipOrig(tips[j % NTIPS])
'''


def env():
    return {'NTIPS': ('const', len(TIPS)), 'TIPS_EN': ('list', [t[2] for t in TIPS]), 'TIPS_RU': ('list', [t[3] for t in TIPS])}


def patch(data):
    """loadingScreen.swf -> (loadingScreen_p.swf, the assembler lines of dmTipSet)"""
    import dos2_touchkit as tk
    swf = Swf(data); d = tk.Doc(swf, 'dmTipOrig'); abc = d.abc
    assert d.has('setInfoText'), 'MainTimeline::setInfoText'
    full = {'EI': ('lex', d.EI), 'Math': ('lex', abc.pub('Math')), 'String': ('call', abc.pub('String'))}; full.update(env())
    made = tk.kc_compile(abc, d.ns, d.late, full, CODE)
    np, rest, lines = made['dmTipSet']; assert np == 1 and not rest
    as3 = abc.qname(abc.namespace(8, NS_AS3), 'charCodeAt'); pub = d.P('charCodeAt')   # a String method: called in the AS3 namespace, as the movie's own code does
    lines = [(l[0], as3) + l[2:] if len(l) > 1 and l[1] == pub and l[0].startswith('callprop') else l for l in lines]
    ti = abc.trait(d.ci, 'setInfoText'); orig = abc.methods[ti.b]
    m = abc.add_method('dk_dmTipSet', list(orig.params), orig.ret, asm(lines), 64, 64, d.ref.init_scope, d.ref.init_scope + 1)
    b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
    d.made['dmTipSet'] = m
    d.repoint('setInfoText', 'dmTipSet', 'dmTipOrig')
    d.done()
    return swf.write(), lines


# ---------------------------------------------------------------- the same source in Python (rotation, language, engine tips kept)
class _Str(str):
    @property
    def length(s): return len(s)
    def charCodeAt(s, i): return ord(s[int(i)])


class _This:
    def __init__(s): s.shown = []
    def __getattr__(s, n): return None
    def dmTipOrig(s, t): s.shown.append(t)


def run_py(texts, seed=0.37):
    """the movie's dmTipSet over these engine texts -> what the movie shows"""
    import math, types
    this = _This()
    ns = {'this': this, 'String': lambda t: _Str('null' if t is None else t), 'Math': types.SimpleNamespace(floor=math.floor, random=lambda: seed),
          'NTIPS': len(TIPS), 'TIPS_EN': [t[2] for t in TIPS], 'TIPS_RU': [t[3] for t in TIPS], 'None': None, 'True': True, 'False': False}
    exec(CODE, ns)
    for t in texts: ns['dmTipSet'](t)
    return this.shown


# ---------------------------------------------------------------- sources, hook
def sources(path):
    if os.path.isdir(path): return {SRC: (Path(path) / SRC).read_bytes()}
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == GUI + SRC]
    if not e: sys.exit('%s: no %s' % (path, SRC))
    return {SRC: pak.read(e[0])}


def make(src): return {OUT: patch(src[SRC])[0]}


_host = None
def install(host):
    """called at the end of tools/dos2_pchud_touch.py (after tools/dos2_ptouch_gaps.py's install)"""
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


def _hints_lsx(path):
    """Switch Public/Game/Hints/Hints.lsx: [(key, handle, keyboard, controller, touchscreen or None)]"""
    import lspk
    pak = lspk.Pak(str(path)); e = [e for e in pak.entries if e[0] == 'Public/Game/Hints/Hints.lsx']
    if not e: return None
    t = pak.read(e[0]).decode('utf-8'); out = []
    for n in re.findall(r'<node id ?="Hint">(.*?)</node>', t, re.S):
        a = {k: (v, h) for k, v, h in re.findall(r'id="(\w+)" value="([^"]*)"(?: handle="([^"]*)")?', n)}
        out.append((a['HintKey'][0], a['Description'][1], a['Keyboard'][0] == '1', a['Controller'][0] == '1',
                    (a['TouchScreen'][0] == '1') if 'TouchScreen' in a else None, a['Description'][0]))
    return out


def _c_off():
    src = (ROOT / 'dm' / 'host' / 'hle_dos2_touch_tips.h').read_text(encoding='utf-8')
    return re.findall(r'"(HINT_\w+)"', src.split('k_tt_off[]')[1].split('};')[0])


def _loc_texts(pak_path, handles):
    import lspk, html
    pak = lspk.Pak(str(pak_path)); out = []
    for e in pak.entries:
        if re.search(r'/(english|russian)\.xml$', e[0]) and '/Gender/' not in e[0]:
            t = pak.read(e).decode('utf-8')
            for h, v in re.findall(r'<content contentuid="([^"]+)"[^>]*>(.*?)</content>', t, re.S):
                if h in handles: out.append(html.unescape(v))
    return out


def _words(t): return re.findall(r'\w+', t.lower())


def check(src, main_raw=None, ipad=None):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    print('-- tools/dos2_ptouch_tips.py: loading screen tips for touch')
    # the tips themselves
    ids = [t[0] for t in TIPS]
    say(len(set(ids)) == len(ids) and len(TIPS) >= 15, '%d tips, ids unique' % len(TIPS))
    say(all(t[2] and t[3] and len(t[2]) <= MAX_LEN and len(t[3]) <= MAX_LEN for t in TIPS),
        'every tip has English and Russian, each <= %d characters (longest %d / %d)' % (MAX_LEN, max(len(t[2]) for t in TIPS), max(len(t[3]) for t in TIPS)))
    say(all(not CYR.search(t[2]) and CYR.search(t[3]) for t in TIPS), 'English tips have no Cyrillic letter, every Russian one has (the movie\'s language test)')
    kw = [(t[0], m.group(0)) for t in TIPS for x in t[2:4] for m in [KEYWORDS.search(x)] if m]
    say(not kw, 'no tip names a key, the mouse, a cursor or a token%s' % ('' if not kw else ': ' + str(kw)))
    off = _c_off()
    froms = {k for t in TIPS for k in t[1]}
    say(len(off) == len(set(off)) and set(off) <= froms | set(NO_TWIN) and set(NO_TWIN) <= set(off) and not set(NO_TWIN) & froms,
        'every keyboard-only key of the host (%d, hle_dos2_touch_tips.h) has a touch tip or a reason (%d without: %s)' % (len(off), len(NO_TWIN), ' '.join(sorted(NO_TWIN))))
    # the engine's table
    gp = Path(src['_game_pak']) if '_game_pak' in src else _romfs('Game.pak')
    rows = _hints_lsx(gp) if gp else None
    if rows is None: print('skip Switch Hints.lsx not found (give the Switch Game.pak as the source): table checks skipped')
    else:
        keys = {r[0]: r for r in rows}
        say(all(k in keys and keys[k][2] for k in off), 'Switch Hints.lsx (%d tips): every host key is there with Keyboard = 1' % len(rows))
        kb = [r for r in rows if r[2]]
        left = [r for r in kb if r[0] not in off]
        say(left and all('[' not in r[5] for r in left), '%d of %d keyboard tips stay in the PC pick, none with a token: %s' % (len(left), len(kb), ' '.join(r[0][5:] for r in left)))
        tok = [r[0] for r in kb if '[' in r[5]]
        say(set(tok) <= set(off), 'every keyboard tip with a key token is a host key: %s' % ' '.join(t[5:] for t in tok))
        con = [r[0] for r in rows if r[3] and '[' in r[5]]
        say(bool(con), 'the console set is not key-free (why the host does not switch to it): %s' % ' '.join(c[5:] for c in con))
    if ipad:
        ip = _hints_lsx(Path(ipad) / 'Game.pak')
        if ip is None: say(False, '--ipad: no Hints.lsx in %s' % ipad)
        else:
            touch_only = [r for r in ip if r[4] and not r[2] and not r[3]]
            mine = {k for t in TIPS for k in t[1]}
            say(all(r[0] in mine or r[0] in NO_TIP for r in touch_only), 'iPad: every touch-only tip (%d) has ours or a reason: %s' % (len(touch_only), ' '.join(sorted(NO_TIP))))
            ipoff = {r[0] for r in ip if r[2] and r[4] is False}
            say(ipoff == set(off), 'iPad: the keyboard tips it leaves out of its TouchScreen set are exactly the host keys (%d)' % len(ipoff))
            handles = {r[1] for r in ip if r[4]}
            texts = _loc_texts(Path(ipad) / 'Localization' / 'English.pak', handles) + _loc_texts(Path(ipad) / 'Localization' / 'Russian' / 'Russian.pak', handles)
            grams = {tuple(w[i:i + 5]) for t in texts for w in [_words(t)] for i in range(len(w) - 4)}
            hit = [(t[0], ' '.join(g)) for t in TIPS for x in t[2:4] for w in [_words(x)] for g in [tuple(w[i:i + 5]) for i in range(len(w) - 4)] if g in grams]
            say(bool(texts) and not hit, 'no tip repeats 5 words in a row of the iPad\'s %d touch-set texts (EN + RU)%s' % (len(texts), '' if not hit else ': ' + str(hit)))
    else: print('skip --ipad not given: the wording check against the iPad texts was not run')
    # the engine facts: the hint goes to setInfoText (invoke index 1), only from the pick
    b = None
    for p in [main_raw, _find('work/exefs-update/main.raw')]:
        if p and os.path.exists(str(p)): b = open(str(p), 'rb').read(); break
    if b is None: say(False, 'main.raw not found')
    else:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        def dis(a, n): return [(i.mnemonic, i.op_str) for i in md.disasm(b[a:a + 4 * n], a)]
        reg = dis(0x114fc18, 3)
        say(reg[0] == ('adrp', 'x2, #0x1efc000') and reg[1] == ('add', 'x2, x2, #0x9df') and reg[2] == ('mov', 'w1, #1') and b[0x1efc9df:0x1efc9eb] == b'setInfoText\0',
            'the loading screen class registers invoke 1 = "setInfoText" (main+0x114fc18)')
        inv = dis(0x114ff0c, 6)
        say(inv[1] == ('ldr', 'x8, [x8, #0x60]') and ('mov', 'w1, #1') in inv and inv[5] == ('blr', 'x8'), 'the hint pick main+0x114fd40 invokes index 1 with the tip (main+0x114ff20)')
        calls = []
        for i in md.disasm(b[0x114f5f0:0x1150800], 0x114f5f0):
            calls.append((i.address, i.mnemonic, i.op_str))
        n1 = [a for k, (a, m, o) in enumerate(calls) if m == 'blr' and any(c[1] == 'ldr' and c[2].endswith('[x8, #0x60]') for c in calls[max(0, k - 14):k])
              and any(c[1] == 'mov' and c[2] == 'w1, #1' for c in calls[max(0, k - 14):k])]
        say(n1 == [0x114ff20], 'no other invoke of index 1 in the class (main+0x114f5f0..0x1150800): every setInfoText is a tip')
        try:
            c = (ROOT / 'dm' / 'host' / 'hle_dos2_touchui.c').read_text(encoding='utf-8')
            ok = '{0x17, 0, "loadingScreen_p", "GUI/loadingScreen.swf"}' in c.split('static const tu_def k_touch[]')[1].split('};')[0]
        except (OSError, IndexError): ok = False
        say(ok, 'k_touch maps type 0x17 (GUI/loadingScreen.swf, tools/dos2_ui_types.py) to loadingScreen_p (touch only)')
    # the movie
    data = src[SRC]; after, lines = patch(data)
    print('%s -> %s: %d B -> %d B, sha256 %s' % (SRC, OUT, len(data), len(after), hashlib.sha256(after).hexdigest()[:16]))
    s = Swf(after); (_, _, Bc), = s.abcs(); (_, _, B0), = Swf(data).abcs()
    errs = verify_all(Bc)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies' % len(Bc.bodies))
    say(Swf(Swf(after).write()).body() == s.body(), 'the movie parses and round-trips')
    g = gr.check(data, after, quiet=True); say(g == [], 'every original tag of the PC movie kept%s' % ('' if g == [] else ': ' + str(g)))
    import dos2_ui_audit as au
    ein = lambda abc: {n for rec in au.scan(abc).values() for n, _ in rec['ei'] if n != '<dynamic>'}
    say(ein(B0) == ein(Bc), 'ExternalInterface names exactly the PC movie\'s')
    own = {v: k for k, v in Bc.owners().items()}
    t = '\n'.join(Bc.dis(Bc.methods[own[DOC + '::setInfoText']].body))
    o = '\n'.join(Bc.dis(Bc.methods[own[DOC + '::dmTipOrig']].body))
    say('dmTipOrig' in t and 'charCodeAt' in t and NS_AS3 in t and 'random' in t and TIPS[0][2] in t and TIPS[-1][3] in t,
        'setInfoText(text) -> dmTipSet: our tip on every 2nd call (language by a Cyrillic letter), else the original (dmTipOrig)')
    say('buttonHintText_mc' in o and 'setText' in o, 'dmTipOrig = the PC body (buttonHintText_mc.setText)')
    # the same source in Python
    en = ['An engine tip in English (sample text).'] * 6
    shown = run_py(en)
    say(shown[1::2] == en[1::2] and all(x in [t[2] for t in TIPS] for x in shown[0::2]) and len(set(shown[0::2])) == 3,
        'run (EN game): ours, engine, ours, engine, ours, engine - three different tips of ours: %s' % ' | '.join(x[:24] for x in shown[0::2]))
    ru = run_py(['Подсказка движка по-русски (пример).'] * 4)
    say(all(x in [t[3] for t in TIPS] for x in ru[0::2]), 'run (RU game): our Russian tips')
    say(run_py([None, None])[0] in [t[2] for t in TIPS], 'run: no text from the engine -> an English tip, no error')
    return bad


def main():
    a = sys.argv[1:]
    ipad = a[a.index('--ipad') + 1] if '--ipad' in a else None
    if ipad: i = a.index('--ipad'); a = a[:i] + a[i + 2:]
    if len(a) >= 2 and a[0] == 'check':
        src = sources(a[1])
        if not os.path.isdir(a[1]): src['_game_pak'] = a[1]
        sys.exit(1 if check(src, a[2] if len(a) > 2 else None, ipad) else 0)
    if len(a) == 3 and a[0] == 'build':
        out = Path(a[2]); out.mkdir(parents=True, exist_ok=True)
        d = make(sources(a[1]))[OUT]; (out / OUT).write_bytes(d); print('%s: %d B' % (out / OUT, len(d))); return
    if a == ['list']:
        for t in TIPS: print('%-9s %-44s\n          EN %s\n          RU %s' % (t[0], ', '.join(t[1]), t[2], t[3]))
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
