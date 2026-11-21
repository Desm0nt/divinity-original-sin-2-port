"""[DOS2] The merged bindings file of base A (DM_DOS2_TOUCH_BASE=console; dm/host/hle_dos2_touch_base.c,
docs/dos2/TOUCH_MODES_PLAN.md "Основа A: касание на консольном режиме движка").

UI mode TOUCH loads PlayerProfiles/Eoc/keyboard_1.json (camera w a s d, delete / end, pageup / pagedown, escape, f5, o, lalt, lshift ...): the
pad has no binding there. On the console input base the pad's records pass the engine's listener filter again, so TOUCH needs BOTH files:
the keyboard's for its gestures and the pad's (Eoc/controller_1.json: UIAccept <- c:a, UICancel <- c:b, UIUp ... ) for hint taps of the
console menus and a physical gamepad. The engine loads ONE file per player from the path string main+0x1ebfc35 (main+0xc05ab0), so the
two are merged at setup, from the player's own Engine.pak, into TouchUI.pak as

    PlayerProfiles/Eoc/dmtouch_1.json = for every action the keyboard file's inputs, then the controller file's inputs it does not have yet;
                                        actions only the controller file has follow, in its order.

Nothing is removed and nothing is rebound: with no pad input the game sees the keyboard file's bindings unchanged.

  python tools/dos2_touch_bind.py check <Engine.pak | dir with PlayerProfiles/Eoc>     static checks of the merge, exit 1 on any failure
  python tools/dos2_touch_bind.py merge <Engine.pak | dir> <out.json>                  write the merged file
  python tools/dos2_touch_bind.py show  <Engine.pak | dir>                             actions that got inputs from both files
Larian's files: never into the APK or the repository (the merged file is made on the player's machine and goes into TouchUI.pak only).
"""
import json, os, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
KEYS, PAD, OUT = 'PlayerProfiles/Eoc/keyboard_1.json', 'PlayerProfiles/Eoc/controller_1.json', 'PlayerProfiles/Eoc/dmtouch_1.json'
PATH_ROOM = len(PAD)   # the engine's string is ".../controller_1.json": the new path is written over it (hle_dos2_touch_base.h has the same assert)
# what the TOUCH gestures and the hint taps rely on (hle_dos2_touch.c "UI mode TOUCH", dt_pad_key, dt_btn_of_hint): must survive the merge
NEED = {'CameraForward': 'key:w', 'CameraBackward': 'key:s', 'CameraLeft': 'key:a', 'CameraRight': 'key:d', 'CameraRotateLeft': 'key:delete',
        'CameraRotateRight': 'key:end', 'CameraZoomIn': 'key:pageup', 'CameraZoomOut': 'key:pagedown', 'ToggleInGameMenu': 'key:escape',
        'UICancel': 'key:escape', 'QuickSave': 'key:f5', 'ToggleTacticalCamera': 'key:o', 'ShowWorldTooltips': 'key:lalt', 'ShowSneakCones': 'key:lshift',
        'UIAccept': 'key:return', 'ToggleMap': 'key:m'}
NEED_PAD = {'UIAccept': 'c:a', 'UICancel': 'c:b', 'UIBack': 'c:b', 'UIUp': 'c:dpad_up', 'UIDown': 'c:dpad_down', 'UILeft': 'c:dpad_left', 'UIRight': 'c:dpad_right',
            'ToggleInGameMenu': 'c:start', 'ToggleMap': 'c:back', 'UITabPrev': 'c:leftshoulder', 'UITabNext': 'c:rightshoulder', 'UIDelete': 'c:y',
            'ControllerContextMenu': 'c:x', 'PanelSelect': 'c:righttrigger', 'PartyManagement': 'c:lefttrigger'}


def sources(src):
    """(keyboard_1.json bytes, controller_1.json bytes) from an Engine.pak or a directory that holds PlayerProfiles/Eoc"""
    if os.path.isdir(src):
        return tuple((Path(src) / n).read_bytes() for n in (KEYS, PAD))
    import lspk
    pak = lspk.Pak(src); got = {e[0]: pak.read(e) for e in pak.entries if e[0] in (KEYS, PAD)}
    pak.f.close()
    miss = [n for n in (KEYS, PAD) if n not in got]
    if miss: raise SystemExit('%s has no %s' % (src, ' / '.join(miss)))
    return got[KEYS], got[PAD]


def parse(data, what):
    """the file as an ordered {action: [inputs]}; a repeated action or a non-list value is an error (the merge must not guess)"""
    seen = []
    def pairs(p):
        for k, _ in p: seen.append(k)
        return dict(p)
    d = json.loads(data.decode('utf-8-sig'), object_pairs_hook=pairs)
    dup = sorted({k for k in seen if seen.count(k) > 1})
    if dup: raise SystemExit('%s: action(s) listed twice: %s' % (what, ' '.join(dup)))
    for k, v in d.items():
        if not isinstance(v, list) or not all(isinstance(x, str) for x in v): raise SystemExit('%s: %s is not a list of input strings' % (what, k))
    return d


def merge(keys, pad):
    """{action: inputs}: the keyboard file's actions first (its inputs, then the pad's new ones), then the actions only the pad file has"""
    out = {}
    for a, ins in keys.items(): out[a] = list(ins) + [i for i in pad.get(a, []) if i not in ins]
    for a, ins in pad.items():
        if a not in out: out[a] = list(ins)
    return out


def dump(d):
    """in the look of Larian's own files: one action a line, tabs, CRLF"""
    rows = ['\t"%s" : [ %s ]' % (a, ', '.join(json.dumps(i) for i in ins)) if ins else '\t"%s" : [ ]' % a for a, ins in d.items()]
    return ('{\r\n' + ',\r\n'.join(rows) + '\r\n}\r\n').encode('utf-8')


def build(src):
    """the merged file's bytes from an Engine.pak / directory"""
    kb, pb = sources(src)
    return dump(merge(parse(kb, KEYS), parse(pb, PAD)))


def check(src):
    """static checks; returns the number of failures"""
    kb, pb = sources(src); keys, pad = parse(kb, KEYS), parse(pb, PAD); m = merge(keys, pad); data = dump(m); bad = 0
    def t(ok, what):
        nonlocal bad
        bad += not ok; print('%s  %s' % ('ok    ' if ok else 'FAILED', what))
    back = parse(data, OUT)
    t(back == m and list(back) == list(m), 'the written file reads back as the same %d actions in the same order (strict JSON, %d B)' % (len(m), len(data)))
    t(set(m) == set(keys) | set(pad), 'actions = the union of both files (%d keyboard, %d controller, %d in both)' % (len(keys), len(pad), len(set(keys) & set(pad))))
    t(list(m)[:len(keys)] == list(keys), 'the keyboard file\'s actions come first, in its order')
    t(all(m[a][:len(i)] == i for a, i in keys.items()), 'every action starts with the keyboard file\'s inputs, unchanged and in its order')
    t(all(set(i) <= set(m[a]) for a, i in pad.items()), 'every input of the controller file is in its action')
    t(all(set(m[a]) == set(keys.get(a, [])) | set(pad.get(a, [])) for a in m), 'no action has an input neither file gave it')
    t(all(len(set(i)) == len(i) for i in m.values()), 'no input twice in one action')
    nonpad = lambda ins: [i for i in ins if not i.startswith('c:')]
    t(all(nonpad(m[a]) == nonpad(keys.get(a, [])) for a in m), 'keyboard / mouse inputs of every action are exactly the keyboard file\'s (the pad file brings none)')
    t(all(i.startswith('c:') for ins in pad.values() for i in ins), 'the controller file binds pad inputs only (c:...)')
    miss = [a + ' <- ' + i for a, i in NEED.items() if i not in m.get(a, [])]
    t(not miss, 'the keys the TOUCH gestures press are bound: %s' % (', '.join(sorted(NEED.values())) if not miss else 'MISSING ' + '; '.join(miss)))
    miss = [a + ' <- ' + i for a, i in NEED_PAD.items() if i not in m.get(a, [])]
    t(not miss, 'the pad buttons of hint taps / a gamepad are bound (%d checked)%s' % (len(NEED_PAD), '' if not miss else ': MISSING ' + '; '.join(miss)))
    t(len(OUT) <= PATH_ROOM, 'the path %s (%d chars) fits the engine\'s string for %s (%d)' % (OUT, len(OUT), PAD, PATH_ROOM))
    c = Path(__file__).resolve().parent.parent / 'dm' / 'host' / 'hle_dos2_touch_base.h'
    if c.exists(): t('#define TB_BIND_MERGED_PATH "%s"' % OUT in c.read_text(encoding='utf-8'), 'the host points the bindings path at the same name (hle_dos2_touch_base.h TB_BIND_MERGED_PATH)')
    both = [a for a in m if keys.get(a) and pad.get(a)]
    print('%d action(s) have inputs from both files; %d come from the controller file alone' % (len(both), len([a for a in m if a not in keys])))
    return bad


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('check', 'merge', 'show'): sys.exit(__doc__)
    if a[0] == 'check':
        bad = check(a[1]); print('FAILED' if bad else 'all checks passed'); sys.exit(1 if bad else 0)
    if a[0] == 'merge':
        if len(a) < 3: sys.exit(__doc__)
        data = build(a[1]); Path(a[2]).write_bytes(data); print('%s: %d B' % (a[2], len(data))); return
    kb, pb = sources(a[1]); keys, pad = parse(kb, KEYS), parse(pb, PAD)
    for act, ins in merge(keys, pad).items():
        if keys.get(act) and pad.get(act): print('%-28s %s' % (act, '  '.join(ins)))


if __name__ == '__main__':
    main()
