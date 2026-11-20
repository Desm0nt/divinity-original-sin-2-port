"""[DOS2] TouchUI.pak: the iPad touch movies (`*_t.swf`) as an extra LSPK v13 package for the Switch engine (docs/dos2/TOUCH_UI_PLAN.md, TU-01).

The engine mounts every `*.pak` it finds in rom:/Data (FindFiles "*.pak", main+0x82fea8), so the file goes next to Game.pak. The package is
written the way Larian's own Switch packages are (checked against Data/Game.pak): files sorted by name (case ignored) from offset 0, each padded to 64 bytes, every entry
LZ4 block compressed with flags 0x42 and crc = CRC-32 of the bytes on disk; then the file list (u32 count + one LZ4 block of 280-byte
entries); then the 40-byte header (version 13, list offset, list size, parts 1, flags 0, priority 0, md5 zero, header size, "LSPK").
`--store` writes the entries uncompressed instead (flags 0, uncompressed size 0), for an A/B run only.

  python tools/dos2_touchui_pak.py <set> [--src <iPad Game.pak | directory of extracted *.swf>] [--out <dir>] [--store] [--name TouchUI.pak]
  sets: console (THE DEFAULT since 15.11, owner: console look with touch; only mainMenu_d.swf, no iPad file, no --src needed),
        hints (console + msgBox_d saveLoad_d optionsSettings_d mods_d characterCreation_d tutorialBox_d: tools/dos2_hint_touch.py, for DM_DOS2_TOUCHUI_SET=hints);
               + bottomBar_d = the console HUD with options / tactical camera / hot bar arrows / cancel / end turn / sneak / weapon buttons and dmColumn_d =
               the panel column + map (tools/dos2_hud_touch.py, tools/dos2_column_touch.py; --no-vis = without), laid out as the iPad's, in our
               own art (assets/touchui, tools/dos2_touchart.py; owner 16.11: no iPad file needed, docs/dos2/TOUCH_ART.md);
               + minimap_d = tap on the minimap opens the map (tools/dos2_minimap_touch.py; --no-minimap = without);
               + partyInventory_d equipmentPanel_d statsPanel_d inventorySkillPanel_d journal_d craftPanel_d trade_d reward_d examine_d journal_csp_d = the
               panels with a button row as the iPad's (tools/dos2_panel_touch.py; --no-panels = without);
               characterCreation_d = hints + taps on its own elements, tools/dos2_cc_touch.py; gameMenu_d (pause menu), msgBox_d (option list +
               hints) and dialog_d (tap = that answer): tools/dos2_list_touch.py;
               + tools/dos2_taps_touch.py over msgBox_d / tutorialBox_d / bottomBar_d and playerInfo_d (prompts, hold A, hot bar slots, portraits; --no-taps = without)
               + tools/dos2_taps3_touch.py over saveLoad_d optionsSettings_d mods_d mainMenu_d characterCreation_d inventorySkillPanel_d (hints by stage
               geometry, option rows / sliders, creation step tabs, skills panel press; --no-taps3 = without)
               + tools/dos2_taps4_touch.py over partyInventory_d inventorySkillPanel_d statsPanel_d craftPanel_d trade_d (tabs by tap, skill select / use,
               the skills panel's item half, inventory multi-select tap = toggle; --no-taps4 = without); journal_csp_d (0x8c) = journal_csp.swf + the row),
               + tools/dos2_taps5_touch.py over saveLoad_d bottomBar_d partyInventory_d inventorySkillPanel_d equipmentPanel_d (save / load rows,
               hot bar tooltip by long press, end turn by hold, item menu by long press, equipment taps; --no-taps5 = without),
               + tools/dos2_hudrect_touch.py over bottomBar_d / minimap_d / playerInfo_d (ExternalInterface dmHudRects: the HUD elements' real
               stage bounds for the floating sticks; --no-hudrect = without), and overhead_d = overhead.swf + that pass (the "(X) Actions..." hint
               over the selected target as rect "hint3": a world tap there = X; --no-ohint = without);
        touch (hints + the HUD of UI mode TOUCH made from the player's PC movies: hotBar_p minimap_p statusConsole_p playerInfo_p combatLog_p dmSide_p,
               tools/dos2_pchud_touch.py, docs/dos2/TOUCH_MODES_PLAN.md "Implemented: touch HUD"; every `hints` movie is byte for byte the same),
        --kit-m0 / --kit (hints / touch sets; docs/dos2/TOUCH_UNIFIED.md M0 / M2): the type 0x8d movies (dmColumn_d, dmSide_p) get the M0 probe
               (tools/dos2_touchkit.py m0_patch: the kit's input event names + a trace) or the whole dmTouch kit with their maps; without them
               every movie is what it was,
        menu1 (mainMenu_t only), menu (11 menus + uiElements_t; no mainMenu_t: the owner keeps mainMenu_c, 15.11), hud, all (every registered `_t` movie, no journal_t), list (print the sets)
  every set but menu1 gets mainMenu_d.swf = the Switch mainMenu_c.swf with touch handlers (variant D, tools/dos2_mainmenu_touch.py) and tappable
  button hints (tools/dos2_hint_touch.py), made from
  --sw <Switch Game.pak | mainMenu_c.swf> (default ../work/romfs/Data/Game.pak); --no-d leaves it out; --no-hints = without the hint patch (the
  mainMenu_d of pack 2u byte for byte: the fallback if the hints misbehave on the phone).

Default source: ../work/ipa/Payload/*/Data/Game.pak (the owner's IPA), default output ../work/touchui/<set>/TouchUI.pak. The result is read
back with tools/lspk.py and every file compared byte for byte with the source. Larian's files: never into the APK or the repository.
"""
import glob, os, struct, sys, zlib
from pathlib import Path
import lz4.block
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lspk, dos2_pchud_touch, dos2_column_touch, dos2_minimap_touch, dos2_mainmenu_touch, dos2_hint_touch, dos2_cc_touch, dos2_list_touch, dos2_hud_touch, dos2_panel_touch, dos2_taps_touch, dos2_taps3_touch, dos2_taps4_touch, dos2_taps5_touch, dos2_hudrect_touch

GUI = 'Public/Game/GUI/'
MENU = ['msgBox', 'gameMenu', 'saveLoad', 'optionsSettings', 'optionsInput', 'loadingScreen', 'userProfile', 'characterCreation',
        'connectionMenu', 'serverlist', 'tutorialBox']
HUD = ['hotBar', 'minimap', 'playerInfo', 'statusConsole', 'combatLog', 'combatTurn']
SETS = {'console': [], 'hints': [], 'touch': [],   # no iPad movie: made from the Switch package only
        'menu1': ['mainMenu_t.swf'],
        'menu': [m + '_t.swf' for m in MENU] + ['uiElements_t.swf'],
        'hud': [m + '_t.swf' for m in MENU + HUD] + ['uiElements_t.swf'],
        'all': None}   # every *_t.swf of the source but journal_t.swf (not registered on the iPad) and the language variants (*_t_CN / _JA)
ALIGN, ENTRY, FLAGS_LZ4 = 64, 280, 0x42


def source(src):
    """name -> bytes getter for the movies of the source (the iPad Game.pak or a directory)"""
    if os.path.isdir(src):
        return {p.name: p.read_bytes for p in Path(src).glob('*.swf')}
    pak = lspk.Pak(src)
    return {e[0][len(GUI):]: (lambda e=e: pak.read(e)) for e in pak.entries if e[0].startswith(GUI) and e[0].endswith('.swf') and '/' not in e[0][len(GUI):]}


def host_movies(tables=('k_def', 'k_hint')):
    """GUI/<name>.swf of the host's default and hints tables (dm/host/hle_dos2_touchui.c k_def + k_hint), or None without the source"""
    import re
    c = Path(__file__).resolve().parent.parent / 'dm' / 'host' / 'hle_dos2_touchui.c'
    if not c.exists(): return None
    src = c.read_text(encoding='utf-8'); out = set()
    for tname in tables:
        m = re.search(r'static const tu_def %s\[\] = \{(.*?)\n\};' % tname, src, re.S)
        out |= {GUI + mv + '.swf' for mv in re.findall(r'\{0x[0-9a-f]+, (?:0x[0-9a-f]+|0), "(\w+)", "[^"]+"\}', m.group(1))}
    return out


def host_missing(names, tables=('k_def', 'k_hint')):
    want = host_movies(tables)
    return None if want is None else sorted(n.split('/')[-1] for n in want - set(names))


def write_pak(path, files, store=False, priority=0):
    """files: [(package path, bytes)] in the order given. Returns the entries written."""
    entries = []
    with open(path, 'wb') as f:
        for name, data in files:
            nb = name.encode('utf-8'); assert len(nb) < 256, name
            disk = data if store else lz4.block.compress(data, mode='high_compression', store_size=False)
            off = f.tell(); assert off % ALIGN == 0
            f.write(disk); f.write(b'\0' * (-len(disk) % ALIGN))
            entries.append(struct.pack('<256sIIIIII', nb, off, len(disk), 0 if store else len(data), 0, 0 if store else FLAGS_LZ4, zlib.crc32(disk)))
        tab = b''.join(entries)
        lst = struct.pack('<I', len(entries)) + lz4.block.compress(tab, mode='high_compression', store_size=False)
        lo = f.tell()
        f.write(lst)
        f.write(struct.pack('<IIIHBB16sI4s', 13, lo, len(lst), 1, 0, priority, b'\0' * 16, 40, b'LSPK'))
    return entries


def main():
    a = sys.argv[1:]
    if not a or a[0] in ('-h', '--help'): sys.exit(__doc__)
    opt = {'--src': None, '--out': None, '--name': 'TouchUI.pak', '--sw': None}; store = '--store' in a
    for k in opt:
        if k in a: opt[k] = a[a.index(k) + 1]
    which = a[0]
    if which == 'list':
        for k, v in SETS.items(): print(k, ' '.join(v) if v else '(every registered *_t.swf)')
        return
    if which not in SETS: sys.exit(__doc__)
    root = Path(__file__).resolve().parent.parent.parent / 'work'
    need_ipad = SETS[which] is None or bool(SETS[which])   # only the old experiment sets (menu1 / menu / hud / all) pack iPad movies; console / hints never read one
    src = (opt['--src'] or next(iter(glob.glob(str(root / 'ipa' / 'Payload' / '*' / 'Data' / 'Game.pak'))), None)) if need_ipad else None
    if need_ipad and (not src or not os.path.exists(src)): sys.exit('no source: give --src <iPad Game.pak or directory>')
    have = source(src) if need_ipad else {}
    names = SETS[which] if SETS[which] is not None else sorted(n for n in have if n.endswith('_t.swf') and n != 'journal_t.swf')
    miss = [n for n in names if n not in have]
    if miss: sys.exit('missing in %s: %s' % (src, ' '.join(miss)))
    made = {}
    if which != 'menu1' and '--no-d' not in a:   # variant D: the console main menu with touch handlers, patched from the Switch movie
        sw = opt['--sw'] or str(root / 'romfs' / 'Data' / 'Game.pak')
        if not os.path.exists(sw): sys.exit('no Switch source for %s: give --sw <Switch Game.pak or mainMenu_c.swf> (or --no-d)' % dos2_mainmenu_touch.MOVIE)
        srcd = dos2_mainmenu_touch.source(sw)
        if dos2_mainmenu_touch.check(srcd): sys.exit('%s: static checks failed, package not written' % dos2_mainmenu_touch.MOVIE)
        made[dos2_mainmenu_touch.MOVIE] = dos2_mainmenu_touch.patch(srcd)[0]   # --no-hints: exactly the movie of pack 2u (run on the phone)
        if '--no-hints' not in a: made[dos2_mainmenu_touch.MOVIE] = dos2_hint_touch.patch(made[dos2_mainmenu_touch.MOVIE])[0]   # + tappable hints (its own accept guard stays)
        if '--no-hints' not in a and dos2_hint_touch.check(dos2_mainmenu_touch.MOVIE, dos2_mainmenu_touch.patch(srcd)[0]): sys.exit('%s: hint patch checks failed, package not written' % dos2_mainmenu_touch.MOVIE)
        if which in ('hints', 'touch'):
            for name, data in dos2_hint_touch.sources(sw, None).items():
                if dos2_hint_touch.check(name, data): sys.exit('%s: hint patch checks failed, package not written' % name)
                made[dos2_hint_touch.out_name(name)] = dos2_hint_touch.patch(data, hints=name not in dos2_hint_touch.OWN_HINTS)[0]
                if name == dos2_cc_touch.SRC.split('/')[-1]:   # character creation: hints + real touch on its own elements (tools/dos2_cc_touch.py) instead of the hints-only movie
                    if dos2_cc_touch.check(data): sys.exit('%s: static checks failed, package not written' % dos2_cc_touch.MOVIE)
                    made[dos2_cc_touch.MOVIE] = dos2_cc_touch.patch(data)[0]
            # pause menu, message box option list (+ its hints), dialog answers: tools/dos2_list_touch.py (msgBox_d replaces the hints-only one above)
            for lt, data in dos2_list_touch.sources(sw, None).items():
                if dos2_list_touch.check(lt, data): sys.exit("%s: static checks failed, package not written" % dos2_list_touch.CFG[lt]["out"])
                made[dos2_list_touch.CFG[lt]["out"]] = dos2_list_touch.patch(data, lt)[0]
            # the HUD with on-screen buttons laid out as the iPad's, in our own art (tools/dos2_hud_touch.py + assets/touchui, owner 16.11); --no-vis = the stock bottomBar_c
            if '--no-vis' not in a:
                hsrc = dos2_hud_touch.source(sw)
                # the sneak / weapon buttons show the hot bar's own icons, cut from the game's skill atlas (Icons.pak + Shared.pak next to --sw)
                hic = dos2_hud_touch.atlas_icons(sw)
                if hic is None: sys.exit('%s: no skill icon atlas next to %s (Icons.pak / Shared.pak); package not written' % (dos2_hud_touch.MOVIE, sw))
                if dos2_hud_touch.check(hsrc, hic): sys.exit('%s: static checks failed, package not written' % dos2_hud_touch.MOVIE)
                made[dos2_hud_touch.MOVIE] = dos2_hud_touch.patch(hsrc, hic)[0]
                # the button column in its own movie, anchored by the game to the right screen edge under the minimap (owner 15.11; tools/dos2_column_touch.py,
                # host: hle_dos2_touchui.c type 0x8d watermark.swf -> GUI/dmColumn_d.swf, created next to bottomBar_c)
                csrc = dos2_column_touch.source(sw)
                if dos2_column_touch.check(csrc): sys.exit('%s: static checks failed, package not written' % dos2_column_touch.MOVIE)
                made[dos2_column_touch.MOVIE] = dos2_column_touch.patch(csrc)[0]
            # tap on the minimap = the pad's '-' (open the map), as the iPad's minimap_t (tools/dos2_minimap_touch.py); --no-minimap leaves the stock minimap_c
            # (the tactical camera button lives in bottomBar_d since pack 4c; minimap_d gets no art, no iPad file is read)
            if '--no-minimap' not in a:
                msrc = dos2_minimap_touch.source(sw)
                mip = None
                if dos2_minimap_touch.check(msrc, mip): sys.exit('%s: static checks failed, package not written' % dos2_minimap_touch.MOVIE)
                made[dos2_minimap_touch.MOVIE] = dos2_minimap_touch.patch(msrc, mip)[0]
            # the panels with a button row as the iPad's (close, tab arrows, item menu, info, helmet ...: tools/dos2_panel_touch.py); --no-panels leaves the stock `_c` panels
            if '--no-panels' not in a:
                pip = dos2_panel_touch.swart(sw)
                for pn in dos2_panel_touch.ORDER:
                    psrc = dos2_panel_touch.source(sw, pn)
                    if dos2_panel_touch.check(psrc, pip, pn): sys.exit('%s: static checks failed, package not written' % dos2_panel_touch.out_name(pn))
                    made[dos2_panel_touch.out_name(pn)] = dos2_panel_touch.patch(psrc, pip, pn)[0]
            # the taps run tw2 dropped (tools/dos2_taps_touch.py): message box prompts, tutorial "hold A", hot bar slots, portraits (playerInfo_d); --no-taps = without
            if '--no-taps' not in a:
                for n in dos2_taps_touch.POST:
                    if n not in made: continue
                    if dos2_taps_touch.check(n, made[n]): sys.exit('%s: tap fix checks failed, package not written' % n)
                    made[n] = dos2_taps_touch.patch(n, made[n])[0]
                pi = dos2_taps_touch.playerinfo_source(sw)
                if dos2_taps_touch.check(dos2_taps_touch.PI_OUT, pi): sys.exit('%s: tap fix checks failed, package not written' % dos2_taps_touch.PI_OUT)
                made[dos2_taps_touch.PI_OUT] = dos2_taps_touch.patch(dos2_taps_touch.PI_OUT, pi)[0]
            # third pass (tools/dos2_taps3_touch.py): hints by stage geometry, options rows / sliders, character creation step tabs, skills panel press; --no-taps3 = without
            if '--no-taps3' not in a:
                for n in dos2_taps3_touch.POST:
                    if n not in made: continue
                    if dos2_taps3_touch.check(n, made[n]): sys.exit('%s: taps3 checks failed, package not written' % n)
                    made[n] = dos2_taps3_touch.patch(n, made[n])[0]
            # fourth pass (tools/dos2_taps4_touch.py): the panels' own tabs by tap, skills panel select / use / item half, party inventory multi-select; --no-taps4 = without
            if '--no-taps4' not in a:
                for n in dos2_taps4_touch.POST:
                    if n not in made: continue
                    if dos2_taps4_touch.check(n, made[n]): sys.exit('%s: taps4 checks failed, package not written' % n)
                    made[n] = dos2_taps4_touch.patch(n, made[n])[0]
            # fifth pass (tools/dos2_taps5_touch.py, branch dos2-touch6): save / load rows + hint bar, hot bar tooltip by long press, end turn by hold,
            # button infos by tap, item menu by long press (party inventory, skills panel), equipment doll / inventory taps; --no-taps5 = without
            if '--no-taps5' not in a:
                for n in dos2_taps5_touch.POST:
                    if n not in made: continue
                    if dos2_taps5_touch.check(n, made[n]): sys.exit('%s: taps5 checks failed, package not written' % n)
                    made[n] = dos2_taps5_touch.patch(n, made[n])[0]
            # HUD element rects for the floating sticks (tools/dos2_hudrect_touch.py: ExternalInterface dmHudRects with the real stage bounds of the hot bar,
            # the column buttons, minimap, portraits; the host maps them to window px for D3PadView); --no-hudrect = without (the pad falls back to fixed fractions)
            if '--no-hudrect' not in a:
                # branch dos2-dpadbtns: overhead_d.swf = the Switch overhead.swf + this pass only (its "(X) Actions..." line under the selected target reports
                # its rect as "hint3": a world tap there = the pad's X, hle_dos2_touch.c dt_hint_tap); --no-ohint = without (the type keeps overhead.swf)
                if '--no-ohint' not in a: made[dos2_hudrect_touch.OVERHEAD] = dos2_hudrect_touch.overhead_source(sw)
                for n in dos2_hudrect_touch.POST:
                    if n not in made: continue
                    if dos2_hudrect_touch.check(n, made[n]): sys.exit('%s: hudrect checks failed, package not written' % n)
                    made[n] = dos2_hudrect_touch.patch(n, made[n])[0]
            # pack 3l: bottomBar_d layout "fixed" -> "fillVFit" (the whole window width, so the column can reach the real screen edge; dos2_hud_touch.set_layout);
            # --bb-fixed keeps the stock layout
            # owner 15.11 (run t3o: "the skill bar was right in 3j - put it back"): off by default now, --bb-fill turns it on
            if '--bb-fill' in a and dos2_hud_touch.MOVIE in made:
                made[dos2_hud_touch.MOVIE] = dos2_hud_touch.set_layout(made[dos2_hud_touch.MOVIE], 'fillVFit'); print('%s: layout "fixed" -> "fillVFit"' % dos2_hud_touch.MOVIE)
        if which == 'touch':   # UI mode TOUCH: the PC HUD movies laid out as the iPad's + the right side column (tools/dos2_pchud_touch.py), added to the hints set
            psrc = dos2_pchud_touch.sources(sw)
            if dos2_pchud_touch.check(psrc): sys.exit('touch HUD movies: static checks failed, package not written')
            clash = set(dos2_pchud_touch.OUT) & set(made); assert not clash, clash
            made.update(dos2_pchud_touch.build(psrc))
        if which in ('hints', 'touch') and ('--kit-m0' in a or '--kit' in a):   # docs/dos2/TOUCH_UNIFIED.md M0 / M2 pilot (tools/dos2_touchkit.py): only on request
            import dos2_touchkit
            what = 'm0' if '--kit-m0' in a else 'kit'
            for n in dos2_touchkit.pack(made, what):
                print('%s: %s' % (n, 'the M0 probe (the kit\'s names in events + a trace of them)' if what == 'm0' else 'the dmTouch kit + its map (M2 pilot)'))
    files = [(GUI + n, made[n] if n in made else have[n]()) for n in sorted(list(names) + list(made), key=str.lower)]   # Larian's lists are sorted by name, case ignored (Game.pak, Engine.pak)
    for n, d in files:
        if d[:3] not in (b'CWS', b'FWS', b'ZWS'): sys.exit('%s is not a SWF (%r)' % (n, d[:3]))
    if which in ('hints', 'touch') and not [o for o in a if o.startswith('--no-')]:   # self-check: every movie the host maps a UI type to is in the package
        miss = host_missing({n for n, _ in files})
        if miss is None: print('self-check skipped: dm/host/hle_dos2_touchui.c not found next to tools/')
        elif miss: sys.exit('self-check: the host expects %s, not made; package not written' % ' '.join(miss))
        else: print('self-check: all %d movies of the host table k_hint (and k_def) are in the package' % len(host_movies()))
        if which == 'touch':
            miss = host_missing({n for n, _ in files}, ('k_touch',))
            if miss: sys.exit('self-check: the host table k_touch expects %s, not made; package not written' % ' '.join(miss))
            if miss is not None: print('self-check: all %d movies of the host table k_touch are in the package' % len(host_movies(('k_touch',))))
    out = Path(opt['--out'] or root / 'touchui' / which); out.mkdir(parents=True, exist_ok=True)
    path = out / opt['--name']
    write_pak(path, files, store)
    # read back with the reader the port already trusts, compare every byte
    back = lspk.Pak(path); bad = 0
    assert (back.parts, back.flags, back.prio) == (1, 0, 0) and len(back.entries) == len(files)
    for (n, d), e in zip(files, back.entries):
        got = back.read(e); ok = e[0] == n and got == d
        bad += not ok
        print('%-44s %9d B  crc32 %08x  %s' % (n, len(d), zlib.crc32(d), 'ok' if ok else 'MISMATCH'))
    back.f.close()
    total = sum(len(d) for _, d in files)
    print('%s: %d file(s), %d B of movies, package %d B (%.2f MiB), %s; read back with tools/lspk.py: %s' % (
        path, len(files), total, os.path.getsize(path), os.path.getsize(path) / 1048576, 'stored' if store else 'LZ4 0x42 as Game.pak', 'FAILED' if bad else 'all equal'))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
