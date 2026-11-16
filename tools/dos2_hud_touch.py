"""[DOS2] On-screen HUD buttons drawn into the console HUD: bottomBar_c.swf -> bottomBar_d.swf (docs/dos2/TOUCH_VISIBLE.md, owner 15.11:
"no element to tap = not acceptable"; the radial menu / pad buttons are not an answer where the iPad has a button).

Art (owner 16.11): our own drawings, assets/touchui/*.png via tools/dos2_touchart.py, laid out as the iPad's touch HUD (no iPad file, no
game icon is read; inventory and proofs: docs/dos2/TOUCH_ART.md). Every original tag of bottomBar_c stays byte for byte, only DoABC grows.
  button            art (assets/touchui)                         tap
  dmVisOpt_mc       plate_round_copper + icon_menu, 74 px          dmPadBtn(28) = pad '+' (ToggleInGameMenu), left of the minimap ring (dmOptPlace)
  dmVisTac_mc       plate_round_copper + icon_tactical             dmPadBtn(21) = d-pad up (ToggleTacticalCamera), TAC_GAP under the options button
  dmVisPrev/Next_mc vector arrows (flat khaki), x ARROW_SCALE      ExternalInterface prevHotbar / nextHotbar (+ PlaySound)
  dmVisCancel_mc    plate_round_steel + glyph_cancel, 69 px        dmPadBtn(1) = pad B
  dmVisEnd_mc       plate_endturn + a "$Title" text field          ExternalInterface endTurn; shown only while setActiveInCombat(true)
  dmVisSneak_mc     plate_round_steel + icon_sneak                 dmPadBtn(22) = d-pad down (ToggleSneak)
  dmVisDestr_mc     plate_round_steel + icon_sheath                dmPadBtn(24) = d-pad right (DestructionToggle = weapon drawn)
  cancel / sneak / destruction BTN_SCALE larger, on the cancel button's centre line, DPAD_GAP apart, DPAD_PAD px invisible hit margin;
  "(Y) quick access" under the hot bar (bottombar_mc.yHint_mc, bottomBar_c's own hint text): its own mouseDown listener + an invisible fill
      YHINT_PAD px around its text (dmYPad) -> dmPadBtn(4) = Y.
The panel column (helmet, 5 panels, map, fold arrows) lives in its own movie dmColumn_d (tools/dos2_column_touch.py); its state code stays here.
ABC: methods appended to the document class, onEventInit and setActiveInCombat re-pointed to wrappers (originals kept as dmVisInitOrig /
dmVisCombatOrig); a press dims the button (alpha 0.6), the release of the stage on the same button runs the action. No original body changes.

  python -I tools/dos2_hud_touch.py build <Switch Game.pak | bottomBar_c.swf> <out.swf>
  python -I tools/dos2_hud_touch.py check <Switch Game.pak | bottomBar_c.swf> [bottomBar_d.swf]
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, NS_PUBLIC, OPS, u30
import dos2_swf_graft as gr
import dos2_touchart as ta

MOVIE = 'bottomBar_d.swf'      # same length as bottomBar_c.swf: the creator's path is rewritten in place (hle_dos2_touchui.c k_hint, type 0x3b)
SRC = 'Public/Game/GUI/bottomBar_c.swf'
COL_X, COL_Y, COL_STEP, BTN = 1348 + (1920 - 1440), 422, 80, 74   # the iPad column, anchored to the right edge of a 1920 stage
# (instance, action code, what) top to bottom; codes 0..5 = panel ids, 10 / 11 hot bar row -/+, 12 cancel (pad B), 13 end turn
COLUMN = [('dmVis1_mc', 1, 'helmet'), ('dmVis5_mc', 5, 'ico_charsheet'), ('dmVis0_mc', 0, 'ico_inventory'), ('dmVis2_mc', 2, 'ico_skills'), ('dmVis3_mc', 3, 'ico_craft'),
          ('dmVis4_mc', 4, 'ico_journal')]
EXTRA = [('dmVisPrev_mc', 10), ('dmVisNext_mc', 11), ('dmVisCancel_mc', 12), ('dmVisEnd_mc', 13), ('dmVisMap_mc', 14), ('dmVisOpt_mc', 15), ('dmVisFoldR_mc', 16), ('dmVisFoldL_mc', 16),
         ('dmVisSneak_mc', 18), ('dmVisDestr_mc', 19), ('dmVisTac_mc', 20)]
# codes 17..19 (branch dos2-dpadbtns): 17 the Y hint (bottombar_mc.yHint_mc) -> dmPadBtn(4); 18 sneak -> dmPadBtn(22) d-pad down; 19 destruction ->
# dmPadBtn(24) d-pad right (hle_dos2_touch.c dt_btn_of_hint)
PAD_OF = {17: 4, 18: 22, 19: 24, 20: 21}   # pack 4c: 20 = the tactical camera (d-pad up), here next to the options button
TAC_GAP = 10   # pack 4c: the tactical camera button this many px under the options button, the same width
PLATE_PARTS = (b'select_mc\0', b'bg_inner_mc\0', b'bg_middle_mc\0', b'bg_outer_mc\0')   # cancelBtn without its cross (the unnamed last layer)
DPAD_ICON, DPAD_PAD, YHINT_PAD = 42, 10, 12            # borrowed icon's larger side on the 69 px plate; invisible hit margins (own px)
# pack 4b (owner after 4a): cancel / sneak / destruction 18 % larger, uniformly; sneak and destruction on the cancel button's centre line
# (CANCEL_XY y), right of it in that order, DPAD_GAP stage px between the scaled plates; the destruction button with its hit margin stays
# inside the 1920 stage (x <= 1920 = window 2160 on 2400x1080). The centres follow from the plate's size (graft: dpad_centres).
BTN_SCALE, DPAD_GAP = 1.18, 26
# map (owner 15.11, pack 2z: "how else than '-' to open the map / '+' for the menu? compare with the iPad"): the iPad column's ico_map button under the
# journal (next slot of the column) -> dmPadBtn(27) = pad '-' (controller_1.json ToggleMap <- c:back); the iPad's btnOptions -> dmPadBtn(28) = pad '+'
# (ToggleInGameMenu <- c:start). btnOptions sits top right on the iPad (1348,20 of 1440); our top right is the minimap (minimap_c 357x500 anchored
# topRight: stage x 1563..1920, y 0..~450) and the gear, so it goes just left of the minimap, at the top.
MAP_XY = (COL_X, COL_Y + COL_STEP * len(COLUMN))
OPT_XY = (1480, 20)
# fold handle (the iPad's setRolledIn): a small hot bar arrow; right arrow left of the helmet = fold the column, left arrow at the screen edge = unfold
FOLD_SCALE = 0.6
FOLDR_C, FOLDL_C = (COL_X - 30, COL_Y + BTN / 2.0), (1920 - 28, COL_Y + BTN / 2.0)
COLUMN_ALL = [n for n, _, _ in COLUMN] + ['dmVisMap_mc']   # what folds / hides together
# owner 15.11 (packs 3i..3o): the column lives in its own movie now, anchored by the game to the right edge under the minimap
# (tools/dos2_column_touch.py, dmColumn_d.swf); bottomBar_d keeps only the hot bar arrows / cancel / end turn / options. True = the old graft.
COLUMN_HERE = False
NOT_HERE = set() if COLUMN_HERE else set(COLUMN_ALL + ['dmVisFoldR_mc', 'dmVisFoldL_mc'])
EDGE_ALL = COLUMN_ALL + ['dmVisFoldR_mc', 'dmVisFoldL_mc', 'dmVisOpt_mc']   # what follows the window's right edge (dmColEdge)
PREV_XY, NEXT_XY, ARROW_SCALE = (351, 963), (1571, 963), 0.8   # the slots of bottomBar_c: stage x 406..1516, y 926..1000 (bottombar_mc 960,871 + slotsHolder_mc -554,55 x1.2)
CANCEL_XY = (1650, 963)                                         # centre of the 68 px round button
END_XY = (1310, 826)                                            # top left of the 207x44 plate: above the health / armour bars (stage y 877..920), right of the AP circles
COL_POLL = 60                  # frames between two dmColumnPoll calls (about a second; owner 15.11: no needless polling on weak devices)
PANEL_NAMES = ['Inventory', 'Character', 'Skills', 'Crafting', 'Journal', 'Equipment']


def _read(path, name):
    if os.path.isdir(path): return (Path(path) / name).read_bytes()
    data = open(path, 'rb').read()
    if data[:3] in (b'CWS', b'FWS', b'ZWS'): return data
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == 'Public/Game/GUI/' + name]
    if not e: sys.exit('%s: no %s' % (path, name))
    return pak.read(e[0])


def source(path): return _read(path, 'bottomBar_c.swf')


def dpad_centres(pb):
    """centres (stage px) of the sneak / destruction plates (own box pb, scaled BTN_SCALE): on the cancel button's centre line, right of it,
    DPAD_GAP apart (the cancel button is the same 69 px plate)"""
    w = (pb[2] - pb[0]) * BTN_SCALE
    sx = CANCEL_XY[0] + w + DPAD_GAP
    return (sx, CANCEL_XY[1]), (sx + w + DPAD_GAP, CANCEL_XY[1])


def graft(swf):
    """the visuals (owner 16.11: our own art, tools/dos2_touchart.py - no iPad file, no game icon); returns the report"""
    assert not COLUMN_HERE, 'the column lives in dmColumn_d (tools/dos2_column_touch.py)'
    rep = {'placed': [], 'frozen': 0, 'defs': 0, 'dropped': [], 'icons': {}}
    g = gr.Graft(swf); art = ta.Art(g)
    # options (pack 3x: left of the minimap ring, top): the round bronze plate + the menu bars, 74 px as the iPad's btnOptions
    rp, rb = art.png('plate_round_copper'); b = rb
    def on_round(icon):
        ic, ib = art.png(icon)
        return g.sprite([(rp, 1, (0, 0), None), (ic, 2, ((rb[2] - (ib[2] - ib[0])) / 2.0, (rb[3] - (ib[3] - ib[1])) / 2.0), None)])
    g.place(on_round('icon_menu'), OPT_XY[0], OPT_XY[1], 'dmVisOpt_mc')
    rep['placed'].append(('dmVisOpt_mc', 15, OPT_XY[0] + b[0], OPT_XY[1] + b[1], OPT_XY[0] + b[2], OPT_XY[1] + b[3]))
    # pack 4c: the tactical camera (d-pad up) under the options button, the same plate and width; dmOptPlace moves both
    tx, ty = OPT_XY[0], OPT_XY[1] + b[3] + TAC_GAP
    g.place(on_round('icon_tactical'), tx, ty, 'dmVisTac_mc')
    rep['placed'].append(('dmVisTac_mc', 20, tx + b[0], ty + b[1], tx + b[2], ty + b[3])); rep['tac_off'] = tx - OPT_XY[0]
    # hot bar row arrows (vector, the iPad's flat khaki)
    for name, right, (x, y) in (('dmVisPrev_mc', False, PREV_XY), ('dmVisNext_mc', True, NEXT_XY)):
        c, ab = art.arrow(right); g.place(c, x, y, name, ARROW_SCALE)
        rep['placed'].append((name, 11 if right else 10, x + ab[0] * ARROW_SCALE, y + ab[1] * ARROW_SCALE, x + ab[2] * ARROW_SCALE, y + ab[3] * ARROW_SCALE))
    # cancel: the round steel plate + the cancel cross, centred on 0,0 (scaled BTN_SCALE about its centre)
    k = BTN_SCALE
    sp, pb = art.png('plate_round_steel', centre=True)
    def on_steel(icon):
        ic, ib = art.png(icon, centre=True)
        return g.sprite([(sp, 1, (0, 0), None), (ic, 2, (0, 0), None)])
    cx, cy = CANCEL_XY; g.place(on_steel('glyph_cancel'), cx, cy, 'dmVisCancel_mc', k)
    rep['placed'].append(('dmVisCancel_mc', 12, cx + pb[0] * k, cy + pb[1] * k, cx + pb[2] * k, cy + pb[3] * k))
    # end turn: our plate + a text field "End turn" in the game's $Title font (shown only in combat)
    ep, eb = art.png('plate_endturn')
    lab = ta.edit_text(g, (5, 0, 202, 32), '<p align="center"><font size="28" color="#ffffff" letterSpacing="0.00" kerning="1">End turn</font></p>')
    g.place(g.sprite([(ep, 1, (0, 0), None), (lab, 3, (0, 3), 'label_txt')]), END_XY[0], END_XY[1], 'dmVisEnd_mc')
    rep['placed'].append(('dmVisEnd_mc', 13, END_XY[0] + eb[0], END_XY[1] + eb[1], END_XY[0] + eb[2], END_XY[1] + eb[3]))
    # sneak / destruction (d-pad down / right): the steel plate + our sneak / sheathe icons, right of the cancel button on its centre line
    sneak_xy, destr_xy = dpad_centres(pb); rep['centres'] = (sneak_xy, destr_xy)
    for name, code, icon, xy in (('dmVisSneak_mc', 18, 'icon_sneak', sneak_xy), ('dmVisDestr_mc', 19, 'icon_sheath', destr_xy)):
        g.place(on_steel(icon), xy[0], xy[1], name, k)
        rep['placed'].append((name, code, xy[0] + pb[0] * k, xy[1] + pb[1] * k, xy[0] + pb[2] * k, xy[1] + pb[3] * k)); rep['icons'][name] = icon
    rep['frozen'] += len(g.frozen); rep['dropped'] += g.dropped; g.finish(); rep['defs'] += len(g.new_defs)
    return rep


def wire(swf, rep):
    """the ABC part: listeners on the placed instances"""
    (tag, pre, abc), = swf.abcs(); S = abc.string
    docs = [i for i, c in enumerate(abc.instances) if abc.mname(c['name']).endswith('.MainTimeline')]
    assert len(docs) == 1 and not abc.instances[docs[0]]['flags'] & 1, 'document class not found or sealed'
    ci = docs[0]; inst = abc.instances[ci]
    ti, tc = abc.trait(ci, 'onEventInit'), abc.trait(ci, 'setActiveInCombat')
    assert ti is not None and ti.kind & 15 == 1 and not abc.methods[ti.b].params, 'onEventInit()'
    assert tc is not None and tc.kind & 15 == 1 and len(abc.methods[tc.b].params) == 1, 'setActiveInCombat(Boolean)'
    assert abc.trait(ci, 'dmVisInitOrig') is None, 'already patched'
    for n in ('prevHotbar', 'nextHotbar', 'endTurn', 'PlaySound', 'UI_Generic_Click'): assert n.encode() in abc.strings, 'the movie has no "%s"' % n
    yi = abc.class_index('bottomBar_c_fla.yHint_6')   # the Y hint gets dmCode written on it: its class must be dynamic
    assert not abc.instances[yi]['flags'] & 1 and abc.trait(yi, 'expandY_holder') is not None, 'yHint_6 sealed / no expandY_holder'
    assert abc.trait(abc.class_index('bottomBar_c_fla.bottombarMC_1'), 'yHint_mc') is not None, 'bottombar_mc has no yHint_mc'
    ns = abc.mn[ti.name][1]; assert abc.ns[ns] == (NS_PUBLIC, S('')), 'onEventInit is not public'
    P = lambda name: abc.qname(ns, name)
    EI = abc.pub('ExternalInterface', 'flash.external')
    HEAD = [('getlocal0',), ('pushscope',)]
    ref = abc.methods[ti.b].body

    def add(name, nparams, lines, like=None):
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 64, 64, ref.init_scope, ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        if like is not None:
            abc.methods[m].params = list(like.params); abc.methods[m].ret = like.ret
            if like.flags & 0x08: abc.methods[m].flags |= 0x08; abc.methods[m].options = list(like.options)
        return m

    def child(name, loc): return [('getlocal0',), ('pushstring', S(name)), ('callproperty', P('getChildByName'), 1), ('coerce_a',), ('setlocal', loc)]
    def ei(*args): return [('getlex', EI)] + list(args) + [('callpropvoid', P('call'), len(args))]

    # dmVisHook(): once; every placed button gets its action code and a press listener; the end-turn plate starts hidden.
    # locals 1 mc, 5 graphics, 6 its bounds (the hit margin of the d-pad buttons)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('pushnull',), ('coerce_a',), ('setlocal', 5), ('pushnull',), ('coerce_a',), ('setlocal', 6),
                   ('getlocal0',), ('getproperty', P('dmVisOn')), ('iftrue', 'end'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmVisOn'))]
    r6 = lambda f: [('getlocal', 6), ('getproperty', P(f))]
    for k, (name, c) in enumerate([(n, c) for n, c, _ in COLUMN] + EXTRA):
        code += child(name, 1) + [('getlocal', 1), ('iffalse', 'n%d' % k),
            ('getlocal', 1), ('pushbyte', c), ('setproperty', P('dmCode')),
            ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseChildren')),
            ('getlocal', 1), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmVisDown')), ('callpropvoid', P('addEventListener'), 2)]
        if name == 'dmVisEnd_mc': code += [('getlocal', 1), ('pushfalse',), ('setproperty', P('visible'))]
        if c in (18, 19):   # an invisible fill DPAD_PAD (own px) around the plate is its hit area too (a 0-alpha fill counts for mouseDown / hitTestPoint)
            code += [('getlocal', 1), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', 5),
                ('getlocal', 1), ('getlocal', 1), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 6),
                ('getlocal', 5), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', P('beginFill'), 2),
                ('getlocal', 5)] + r6('x') + [('pushbyte', DPAD_PAD), ('subtract',)] + r6('y') + [('pushbyte', DPAD_PAD), ('subtract',)] + \
                r6('width') + [('pushbyte', 2 * DPAD_PAD), ('add',)] + r6('height') + [('pushbyte', 2 * DPAD_PAD), ('add',), ('callpropvoid', P('drawRect'), 4),
                ('getlocal', 5), ('callpropvoid', P('endFill'), 0)]
        code += [('L', 'n%d' % k)]
    # the Y hint under the hot bar (bottombar_mc.yHint_mc, code 17): its own press listener; its hit fill comes from dmYPad (now and every COL_POLL frames)
    code += [('getlocal0',), ('getproperty', P('bottombar_mc')), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'ny'),
        ('getlocal', 1), ('getproperty', P('yHint_mc')), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'ny'),
        ('getlocal', 1), ('pushbyte', 17), ('setproperty', P('dmCode')),
        ('getlocal', 1), ('pushfalse',), ('setproperty', P('mouseChildren')),
        ('getlocal', 1), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmVisDown')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal0',), ('callpropvoid', P('dmYPad'), 0),
        ('L', 'ny')]
    # the column state (shown / folded / hidden): -1 = no word from the host yet (shown); the host's state comes by polling (dmColTick)
    code += [('getlocal0',), ('pushbyte', -1), ('setproperty', P('dmColState')), ('getlocal0',), ('pushfalse',), ('setproperty', P('dmColFold')),
             ('getlocal0',), ('pushbyte', COL_POLL - 1), ('setproperty', P('dmColN')), ('getlocal0',), ('callpropvoid', P('dmColApply'), 0),
             ('getlocal0',), ('pushstring', S('enterFrame')), ('getlocal0',), ('getproperty', P('dmColTick')), ('callpropvoid', P('addEventListener'), 2)]
    code += [('L', 'end'), ('returnvoid',)]
    m_hook = add('dmVisHook', 0, code)
    # dmVisDown(e): remember the button, dim it; the release is taken from the stage (a finger may leave the button)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
        ('getlocal1',), ('getproperty', P('currentTarget')), ('coerce_a',), ('setlocal', 2),
        ('getlocal0',), ('getlocal', 2), ('setproperty', P('dmVisB')),
        ('getlocal0',), ('getlocal', 2), ('getproperty', P('alpha')), ('setproperty', P('dmVisA')),   # dos2-dpadbtns: the release puts back what it was (the Y hint is the game's)
        ('getlocal', 2), ('pushbyte', 6), ('pushbyte', 10), ('divide',), ('setproperty', P('alpha')),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 3),
        ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal', 3), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmVisUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)]
    m_down = add('dmVisDown', 1, code)
    # dmVisUp(e): the stage's release; on the pressed button = its action
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3), ('pushbyte', 0), ('coerce_a',), ('setlocal', 4),
        ('getlocal0',), ('getproperty', P('dmVisB')), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmVisB')),
        ('getlocal', 2), ('getlocal0',), ('getproperty', P('dmVisA')), ('setproperty', P('alpha')),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 3),
        ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal', 2), ('getlocal', 3), ('getproperty', P('mouseX')), ('getlocal', 3), ('getproperty', P('mouseY')), ('pushfalse',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'end'),
        ('getlocal', 2), ('getproperty', P('dmCode')), ('coerce_a',), ('setlocal', 4),
        ('getlocal', 4), ('pushbyte', 10), ('ifnlt', 'bar'),
        ] + ei(('pushstring', S('dmPanel')), ('getlocal', 4)) + [('returnvoid',),
        ('L', 'bar'), ('getlocal', 4), ('pushbyte', 10), ('ifne', 'c11'),
        ] + ei(('pushstring', S('PlaySound')), ('pushstring', S('UI_Generic_Click'))) + ei(('pushstring', S('prevHotbar')),) + [('returnvoid',),
        ('L', 'c11'), ('getlocal', 4), ('pushbyte', 11), ('ifne', 'c12'),
        ] + ei(('pushstring', S('PlaySound')), ('pushstring', S('UI_Generic_Click'))) + ei(('pushstring', S('nextHotbar')),) + [('returnvoid',),
        ('L', 'c12'), ('getlocal', 4), ('pushbyte', 12), ('ifne', 'c13'),
        ] + ei(('pushstring', S('dmPadBtn')), ('pushbyte', 1)) + [('returnvoid',),
        ('L', 'c13'), ('getlocal', 4), ('pushbyte', 13), ('ifne', 'c14'),
        ('getlocal0',), ('getproperty', P('inActiveCombat')), ('iffalse', 'end'),
        ] + ei(('pushstring', S('endTurn')),) + [('returnvoid',),
        ('L', 'c14'), ('getlocal', 4), ('pushbyte', 14), ('ifne', 'c15'),
        ] + ei(('pushstring', S('dmPadBtn')), ('pushbyte', 27)) + [('returnvoid',),
        ('L', 'c15'), ('getlocal', 4), ('pushbyte', 15), ('ifne', 'c16'),
        ] + ei(('pushstring', S('dmPadBtn')), ('pushbyte', 28)) + [('returnvoid',),
        ('L', 'c16'), ('getlocal', 4), ('pushbyte', 16), ('ifne', 'c17'),
        ] + ei(('pushstring', S('PlaySound')), ('pushstring', S('UI_Generic_Click'))) + [
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmColFold')), ('not',), ('setproperty', P('dmColFold')),
        ('getlocal0',), ('callpropvoid', P('dmColApply'), 0), ('returnvoid',)]
    for c in sorted(PAD_OF):   # dos2-dpadbtns: the Y hint, sneak, destruction = that pad button (hle_dos2_touch.c dt_ei_padbtn)
        code += [('L', 'c%d' % c), ('getlocal', 4), ('pushbyte', c), ('ifne', 'c%d' % (c + 1) if c + 1 in PAD_OF else 'end')] + \
                ei(('pushstring', S('dmPadBtn')), ('pushbyte', PAD_OF[c])) + [('returnvoid',)]
    code += [('L', 'end'), ('returnvoid',)]
    m_up = add('dmVisUp', 1, code)
    # dmYPad(): the Y hint's hit area = an invisible fill YHINT_PAD px around its text's box (expandY_holder in yHint_mc's space: computed from the child,
    # so the fill never feeds back); redrawn only when that box moved / changed width. locals 1 mc 2 holder 3 box 4 graphics
    r3 = lambda f: [('getlocal', 3), ('getproperty', P(f))]
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('pushnull',), ('coerce_a',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
        ('pushnull',), ('coerce_a',), ('setlocal', 4),
        ('getlocal0',), ('getproperty', P('bottombar_mc')), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'end'),
        ('getlocal', 1), ('getproperty', P('yHint_mc')), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'end'),
        ('getlocal', 1), ('getproperty', P('expandY_holder')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('getlocal', 1), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'end')] + \
        r3('width') + [('getlocal0',), ('getproperty', P('dmYW')), ('ifne', 'draw')] + r3('x') + [('getlocal0',), ('getproperty', P('dmYX')), ('ifeq', 'end'),
        ('L', 'draw'), ('getlocal0',)] + r3('width') + [('setproperty', P('dmYW')), ('getlocal0',)] + r3('x') + [('setproperty', P('dmYX')),
        ('getlocal', 1), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', 4),
        ('getlocal', 4), ('callpropvoid', P('clear'), 0),
        ('getlocal', 4), ('pushbyte', 0), ('pushbyte', 0), ('callpropvoid', P('beginFill'), 2),
        ('getlocal', 4)] + r3('x') + [('pushbyte', YHINT_PAD), ('subtract',)] + r3('y') + [('pushbyte', YHINT_PAD), ('subtract',)] + \
        r3('width') + [('pushbyte', 2 * YHINT_PAD), ('add',)] + r3('height') + [('pushbyte', 2 * YHINT_PAD), ('add',), ('callpropvoid', P('drawRect'), 4),
        ('getlocal', 4), ('callpropvoid', P('endFill'), 0),
        ('L', 'end'), ('returnvoid',)]
    m_ypad = add('dmYPad', 0, code)
    # dmColApply(): column (6 panels + map) visible = not hidden and not folded; handles: right arrow while the column is out, left arrow (at the
    # screen edge) while folded; the options button follows "hidden" only (folding keeps the menu reachable)
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 1), ('pushfalse',), ('setlocal', 2), ('pushfalse',), ('setlocal', 3),
        ('getlocal0',), ('getproperty', P('dmColState')), ('pushbyte', 2), ('equals',), ('not',), ('setlocal', 2),   # 2 = shown at all
        ('getlocal', 2), ('iffalse', 'nf'), ('getlocal0',), ('getproperty', P('dmColFold')), ('not',), ('setlocal', 3), ('L', 'nf')]   # 3 = column out
    for k, name in enumerate(COLUMN_ALL + ['dmVisFoldR_mc', 'dmVisFoldL_mc', 'dmVisOpt_mc']):
        code += child(name, 1) + [('getlocal', 1), ('iffalse', 'a%d' % k), ('getlocal', 1)]
        if name == 'dmVisFoldL_mc': code += [('pushfalse',), ('getlocal', 2), ('iffalse', 'b%d' % k), ('pop',), ('getlocal', 3), ('not',), ('L', 'b%d' % k)]
        else: code += [('getlocal', 3 if name in COLUMN_ALL or name == 'dmVisFoldR_mc' else 2)]
        code += [('setproperty', P('visible')), ('L', 'a%d' % k)]
    code += [('returnvoid',)]
    m_apply = add('dmColApply', 0, code)
    # dmColumn(s): the host's state 0 shown / 1 folded / 2 hidden; shown / folded reset the finger's fold
    code = HEAD + [('getlocal0',), ('getlocal1',), ('convert_i',), ('setproperty', P('dmColState')),
        ('getlocal1',), ('pushbyte', 1), ('ifne', 's0'), ('getlocal0',), ('pushtrue',), ('setproperty', P('dmColFold')), ('jump', 'ap'),
        ('L', 's0'), ('getlocal1',), ('pushbyte', 0), ('ifne', 'ap'), ('getlocal0',), ('pushfalse',), ('setproperty', P('dmColFold')),
        ('L', 'ap'), ('getlocal0',), ('callpropvoid', P('dmColApply'), 0), ('returnvoid',)]
    m_col = add('dmColumn', 1, code)
    # dmColTick(e): every COL_POLL frames ExternalInterface dmColumnPoll(the state the movie has); the host answers through setActiveInCombat(100 + state)
    code = HEAD + [('getlocal0',), ('getlocal0',), ('getproperty', P('dmColN')), ('increment',), ('setproperty', P('dmColN')),
        ('getlocal0',), ('getproperty', P('dmColN')), ('pushbyte', COL_POLL), ('ifnge', 'end'),
        ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmColN')),
        ('getlocal0',), ('callpropvoid', P('dmYPad'), 0),   # dos2-dpadbtns: the Y hint's hit fill follows its text (cheap: redrawn only on a change)
        ('getlex', EI), ('pushstring', S('dmColumnPoll')), ('getlocal0',), ('getproperty', P('dmColState')), ('getlocal0',), ('getproperty', P('dmEdge')),
        ('getlocal0',), ('getproperty', P('dmColR')), ('getlocal0',), ('getproperty', P('dmOptX')), ('callpropvoid', P('call'), 5),
        ('L', 'end'), ('returnvoid',)]
    m_tick = add('dmColTick', 1, code)
    # dmColEdge(e): pack 3h (owner: "move them to the edge, so for everyone they sit along the screen edge"): e = the window's right edge in this
    # movie's stage px (host, hle_dos2_touchui_vis.c: (window w - origin x) / scale; 1920 when the stage fills the width). Everything anchored to
    # the right of the 1920 stage (the column, its map button, the fold arrows, the options button left of the minimap - the minimap itself is
    # anchored to the real edge by the game) moves by e - 1920 from its place in the movie. locals 1 e, 2 dx, 3 mc
    MATHL = abc.pub('Math')
    code = HEAD + [('pushbyte', 0), ('convert_d',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
        ('getlocal0',), ('getlocal1',), ('setproperty', P('dmEdge')),
        # pack 3n: with layout fillVFit (set_layout) the stage is the whole window and starts at its left edge: the 1920 design is centred again
        # by moving the document by half the extra width (e - 1920; 0 with the stock "fixed" layout, where the host sends e = 1920)
        ('getlocal0',), ('getlocal1',), ('pushshort', 1920), ('subtract',), ('setproperty', P('x')),
        ('getlocal0',), ('pushbyte', -1), ('setproperty', P('dmColR')),
        ('getlocal0',), ('pushbyte', -1), ('setproperty', P('dmOptX')),   # pack 3x: the host places the options button again after an edge change
        ('getlocal1',), ('pushshort', 1920), ('subtract',), ('convert_d',), ('setlocal', 2)]
    for k, name in enumerate(EDGE_ALL):
        code += child(name, 3) + [('getlocal', 3), ('iffalse', 'e%d' % k),
            ('getlocal0',), ('getproperty', P('dmEx%d' % k)), ('pushundefined',), ('ifstrictne', 'h%d' % k),
            ('getlocal0',), ('getlocal', 3), ('getproperty', P('x')), ('setproperty', P('dmEx%d' % k)),
            # pack 3j (run t3i: the column moved to x 2308 and vanished): Iggy draws a movie only inside its own 1920-wide rect, so an element moves
            # at most until its right side meets the stage's right edge: x = min(x0 + dx, 1920 - width)
            ('L', 'h%d' % k), ('getlocal', 3), ('getlex', MATHL), ('getlocal0',), ('getproperty', P('dmEx%d' % k)), ('getlocal', 2), ('add',),
            ('getlocal1',), ('getlocal', 3), ('getproperty', P('width')), ('subtract',), ('callproperty', P('min'), 2), ('setproperty', P('x')),   # clamp: x <= e - width (pack 3k: e = the visible edge the host sends)
            ('L', 'e%d' % k)]
    code += [('returnvoid',)]
    m_edge = add('dmColEdge', 1, code)
    # dmOptPlace(x): pack 3x (owner's sketch: the options button just left of the minimap ring, at the top): x in document px from the host
    # (the minimap's real rect), y stays. locals 1 x 2 mc
    toff = int(round(rep.get('tac_off', 0)))
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2), ('getlocal0',), ('getlocal1',), ('setproperty', P('dmOptX'))] + child('dmVisOpt_mc', 2) + [
        ('getlocal', 2), ('iffalse', 'tac'), ('getlocal', 2), ('getlocal1',), ('setproperty', P('x')), ('L', 'tac')] + child('dmVisTac_mc', 2) + [
        ('getlocal', 2), ('iffalse', 'end'), ('getlocal', 2), ('getlocal1',), ('pushshort', toff), ('add',), ('setproperty', P('x')),   # pack 4c: the tactical camera under it
        ('L', 'end'), ('returnvoid',)]
    m_opt = add('dmOptPlace', 1, code)
    # dmColRight(c): pack 3o (owner's sketch: the column's right side on one line with the minimap ring's right side): c = that line in document
    # px (host: the ring's live rect); the column, its map button and both fold arrows move so the column's right side (COL_X + BTN in the movie)
    # lands on c; the options button stays with dmColEdge. Uses the x the elements had in the movie (dmEx<k>, stored by dmColEdge / here).
    code = HEAD + [('pushbyte', 0), ('convert_d',), ('setlocal', 2), ('pushnull',), ('coerce_a',), ('setlocal', 3),
        ('getlocal0',), ('getlocal1',), ('setproperty', P('dmColR')),
        ('getlocal1',), ('pushshort', COL_X + BTN), ('subtract',), ('convert_d',), ('setlocal', 2)]
    for k, name in enumerate(EDGE_ALL):
        if name == 'dmVisOpt_mc': continue
        code += child(name, 3) + [('getlocal', 3), ('iffalse', 'r%d' % k),
            ('getlocal0',), ('getproperty', P('dmEx%d' % k)), ('pushundefined',), ('ifstrictne', 'q%d' % k),
            ('getlocal0',), ('getlocal', 3), ('getproperty', P('x')), ('setproperty', P('dmEx%d' % k)),
            ('L', 'q%d' % k), ('getlocal', 3), ('getlocal0',), ('getproperty', P('dmEx%d' % k)), ('getlocal', 2), ('add',), ('setproperty', P('x')),
            ('L', 'r%d' % k)]
    code += [('returnvoid',)]
    m_right = add('dmColRight', 1, code)
    inst['traits'] += [Trait(P('dmVisHook'), 1, 0, m_hook), Trait(P('dmVisDown'), 1, 0, m_down), Trait(P('dmVisUp'), 1, 0, m_up),
                       Trait(P('dmColApply'), 1, 0, m_apply), Trait(P('dmColumn'), 1, 0, m_col), Trait(P('dmColTick'), 1, 0, m_tick),
                       Trait(P('dmColEdge'), 1, 0, m_edge), Trait(P('dmColRight'), 1, 0, m_right), Trait(P('dmOptPlace'), 1, 0, m_opt),
                       Trait(P('dmYPad'), 1, 0, m_ypad)]
    # onEventInit(): the original, then the hook
    code = HEAD + [('getlocal0',), ('callpropvoid', P('dmVisInitOrig'), 0), ('getlocal0',), ('callpropvoid', P('dmVisHook'), 0), ('returnvoid',)]
    m = add('onEventInit', 0, code, like=abc.methods[ti.b]); old_i = ti.b; ti.b = m; inst['traits'].append(Trait(P('dmVisInitOrig'), 1, 0, old_i))
    # setActiveInCombat(b): the original, then the end-turn plate follows it (the hook too: a movie whose onEventInit was not called yet).
    # The host's column state rides on this name (an invoke needs a name the engine registered for the movie, hle_dos2_touchui_ei.c): a Number
    # >= 100 is dmColumn(n - 100) and never reaches the original (the engine sends a Boolean). The parameter is untyped for that.
    code = HEAD + [('pushnull',), ('coerce_a',), ('setlocal', 2),
        ('getlocal1',), ('typeof',), ('pushstring', S('number')), ('ifne', 'orig'), ('getlocal1',), ('pushbyte', 100), ('ifnge', 'orig'),
        ('getlocal0',), ('callpropvoid', P('dmVisHook'), 0),
        ('getlocal1',), ('pushshort', 30000), ('ifnge', 'edg'),
        ('getlocal0',), ('getlocal1',), ('pushshort', 30000), ('subtract',), ('callpropvoid', P('dmColRight'), 1), ('returnvoid',),
        ('L', 'edg'), ('getlocal1',), ('pushshort', 20000), ('ifnge', 'edg2'),
        ('getlocal0',), ('getlocal1',), ('pushshort', 20000), ('subtract',), ('callpropvoid', P('dmOptPlace'), 1), ('returnvoid',),
        ('L', 'edg2'), ('getlocal1',), ('pushshort', 10000), ('ifnge', 'col'),
        ('getlocal0',), ('getlocal1',), ('pushshort', 10000), ('subtract',), ('callpropvoid', P('dmColEdge'), 1), ('returnvoid',),
        ('L', 'col'), ('getlocal0',), ('getlocal1',), ('pushbyte', 100), ('subtract',), ('callpropvoid', P('dmColumn'), 1), ('returnvoid',),
        ('L', 'orig'), ('getlocal0',), ('getlocal1',), ('convert_b',), ('callpropvoid', P('dmVisCombatOrig'), 1),
        ('getlocal0',), ('callpropvoid', P('dmVisHook'), 0)] + child('dmVisEnd_mc', 2) + [
        ('getlocal', 2), ('iffalse', 'end'), ('getlocal', 2), ('getlocal1',), ('convert_b',), ('setproperty', P('visible')),
        ('L', 'end'), ('returnvoid',)]
    m2 = add('setActiveInCombat', 1, code, like=abc.methods[tc.b]); abc.methods[m2].params = [0]; old_c = tc.b; tc.b = m2; inst['traits'].append(Trait(P('dmVisCombatOrig'), 1, 0, old_c))
    rep['methods'] = [m_hook, m_down, m_up, m_ypad, m_apply, m_col, m_tick, m_edge, m_right, m_opt, m, m2]; rep['moved'] = {'dmVisInitOrig': old_i, 'dmVisCombatOrig': old_c}
    swf.tags[tag][1] = pre + abc.write()


def set_layout(data, value):
    """pack 3l (owner 15.11: the column must sit at the real screen edge, "like the minimap"): bottomBar_c declares layout = "fixed" (a 1920x1080
    frame centred in the window: Iggy draws it only inside x 240..2160 on a 2400x1080 window, read from its FlashPlayer +0x10..+0x1c). The game's
    full-width movies (overhead, worldTooltip, overviewMap, tutorialBox) say "fillVFit". This rewrites the u30 operand of the one
    `pushstring 'fixed'` that feeds `initproperty layout` in the document class (same operand length or it refuses). Returns new bytes."""
    swf = Swf(data); (tag, pre, abc), = swf.abcs()
    hits = []
    for b in abc.bodies:
        ins = abc.ins(b.code)
        for k in range(1, len(ins)):
            off, op, ops, size = ins[k]
            if OPS[op][0] in ('initproperty', 'setproperty') and abc.short(ops[0]) == 'layout':
                poff, pop, pops, psize = ins[k - 1]
                if OPS[pop][0] == 'pushstring' and abc.S(pops[0]) == 'fixed': hits.append((b, poff, psize, pops[0]))
    assert len(hits) == 1, 'layout = "fixed" found %d times' % len(hits)
    b, poff, psize, old = hits[0]
    idx = abc.string(value); enc = bytes([0x2c]) + u30(idx)
    assert len(enc) == psize, 'operand length differs (%d vs %d)' % (len(enc), psize)
    b.code = b.code[:poff] + enc + b.code[poff + psize:]
    swf.tags[tag][1] = pre + abc.write()
    return swf.write()


def patch(data):
    """bottomBar_c bytes -> (bottomBar_d bytes, report)"""
    swf = Swf(data); rep = graft(swf); wire(swf, rep)
    return swf.write(), rep


def check(src, built=None):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(src)
    if built is not None: say(built == out, 'the given %s is what this tool builds from these sources (%d B)' % (MOVIE, len(built)))
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs()
    print('%s -> %s: %d B -> %d B, +%d definition tags, +%d root instances, %d sprites frozen to one frame, +%d methods, sha256 %s' % (
        SRC.split('/')[-1], MOVIE, len(src), len(out), rep['defs'], len(rep['placed']), rep['frozen'], len(rep['methods']), hashlib.sha256(out).hexdigest()[:16]))
    say(not gr.check(src, out), 'graft: every original tag of bottomBar_c byte for byte and in order (DoABC aside), new ids unique, every use after its definition, re-read equal')
    say(not [1 for c, d, _ in b.tags if c == gr.T_IMPORT2][len([1 for c, d, _ in a.tags if c == gr.T_IMPORT2]):] and rep['frozen'] == 0 and not rep['dropped'],
        'own art only: no symbol of another movie copied (no import, nothing frozen, no class link dropped)')
    say(B.strings[:len(A.strings)] == A.strings and B.ns[:len(A.ns)] == A.ns and B.nssets == A.nssets and B.mn[:len(A.mn)] == A.mn and B.ints == A.ints and B.doubles == A.doubles,
        'constant pool only appended: +%d strings, +%d multinames' % (len(B.strings) - len(A.strings), len(B.mn) - len(A.mn)))
    same = all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc, x.method) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc, y.method)
               for x, y in zip(A.bodies, B.bodies))
    n = len(rep['methods'])
    say(same and len(B.bodies) == len(A.bodies) + n and len(B.methods) == len(A.methods) + n, 'all %d original method bodies byte-identical; %d methods appended' % (len(A.bodies), n))
    oa, ob = A.owners(), B.owners()
    moved = sorted('%s -> %s' % (oa[m].split('::')[-1], ob[m].split('::')[-1]) for m in oa if ob.get(m) != oa[m])
    say(moved == ['onEventInit -> dmVisInitOrig', 'setActiveInCombat -> dmVisCombatOrig'], 'renamed methods: ' + ', '.join(moved))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies' % len(B.bodies))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips')
    ib = {v: k for k, v in ob.items()}; doc = [B.mname(c['name']) for c in B.instances if B.mname(c['name']).endswith('.MainTimeline')][0]
    d = '\n'.join(B.dis(B.methods[ib[doc + '::dmVisUp']].body))
    say(all("'%s'" % s in d for s in ('dmPanel', 'prevHotbar', 'nextHotbar', 'dmPadBtn', 'endTurn')) and 'hitTestPoint' in d and 'pushbyte         27' in d and 'pushbyte         28' in d and 'dmColApply' in d,
        'release on the pressed button -> dmPanel(id) / prevHotbar / nextHotbar / dmPadBtn(1) / endTurn / dmPadBtn(27) map / dmPadBtn(28) menu / fold handle')
    say(all('pushbyte         %d' % v in d for v in PAD_OF.values()) and 'dmVisA' in d,
        'dos2-dpadbtns: release on the Y hint / sneak / destruction -> dmPadBtn(4) Y / dmPadBtn(22) d-pad down / dmPadBtn(24) d-pad right; the alpha the press found is put back')
    h = '\n'.join(B.dis(B.methods[ib[doc + '::dmVisHook']].body)); y = '\n'.join(B.dis(B.methods[ib[doc + '::dmYPad']].body))
    say('yHint_mc' in h and 'dmYPad' in h and h.count('drawRect') == 2 and 'expandY_holder' in y and 'drawRect' in y and 'dmYPad' in '\n'.join(B.dis(B.methods[ib[doc + '::dmColTick']].body)),
        'dmVisHook: the Y hint gets code 17 + a press listener, the d-pad buttons a %d px hit fill; dmYPad (hook + every %d frames) fills %d px around the hint\'s text' % (DPAD_PAD, COL_POLL, YHINT_PAD))
    d2 = '\n'.join(B.dis(B.methods[ib[doc + '::setActiveInCombat']].body))
    say('callpropvoid     dmColumn, 1' in d2 and 'dmVisCombatOrig' in d2 and B.methods[ib[doc + '::setActiveInCombat']].params == [0] and
        "'dmColumnPoll'" in '\n'.join(B.dis(B.methods[ib[doc + '::dmColTick']].body)),
        'column state: dmColTick -> ExternalInterface dmColumnPoll(state); setActiveInCombat(Number >= 100) -> dmColumn(n - 100), a Boolean -> the original')
    names = {gr.place_parse(c, t)['name'] for c, t, _ in b.tags if c in (4, 26, 70)}
    say(all((nm.encode() + b'\0') in names for nm, _, _ in COLUMN if nm not in NOT_HERE) and all((nm.encode() + b'\0') in names for nm, _ in EXTRA if nm not in NOT_HERE), 'all %d instances are on the root timeline by name' % len(rep['placed']))
    boxes = rep['placed']; out = COLUMN_ALL + ['dmVisFoldR_mc']; excl = lambda a, b: 'dmVisFoldL_mc' in (a, b) and (a in out or b in out)   # the edge handle shows only while those are hidden
    clash = [(p[0], q[0]) for i, p in enumerate(boxes) for q in boxes[i + 1:] if p[2] < q[4] and q[2] < p[4] and p[3] < q[5] and q[3] < p[5] and not excl(p[0], q[0])]
    inside = all(0 <= p[2] and p[4] <= 1920 and 0 <= p[3] and p[5] <= 1080 for p in boxes)
    say(not clash and inside, 'the new elements are inside the 1920x1080 stage and do not overlap each other%s' % (': ' + str(clash) if clash else ''))
    slots = (406, 926, 1516, 1000)
    say(not [p[0] for p in boxes if p[2] < slots[2] and slots[0] < p[4] and p[3] < slots[3] and slots[1] < p[5]], 'none covers the hot bar slots (stage %d,%d..%d,%d)' % slots)
    bx = {p[0]: p for p in boxes}; trio = [bx[n] for n in ('dmVisCancel_mc', 'dmVisSneak_mc', 'dmVisDestr_mc')]
    cyy = [(p[3] + p[5]) / 2.0 for p in trio]; sz = [(p[4] - p[2], p[5] - p[3]) for p in trio]; m = DPAD_PAD * BTN_SCALE
    say(max(cyy) - min(cyy) < 0.01 and all(abs(w - sz[0][0]) < 0.01 and abs(h - sz[0][1]) < 0.01 for w, h in sz) and abs(sz[0][0] - 69 * BTN_SCALE) < 0.6
        and trio[0][4] < trio[1][2] and trio[1][4] < trio[2][2] and trio[2][4] + m <= 1920,
        'pack 4b: cancel / sneak / destruction %.0f %% larger (%.1f px), vertical centres equal (y %.1f), in that order left to right, gaps %.1f / %.1f; '
        'the destruction button with its %.1f px hit margin ends at stage x %.1f <= 1920 (window %.0f on 2400x1080)' % (
            (BTN_SCALE - 1) * 100, sz[0][0], cyy[0], trio[1][2] - trio[0][4], trio[2][2] - trio[1][4], m, trio[2][4] + m, trio[2][4] + m + 240))
    print('       icons: %s' % '; '.join('%s = %s' % kv for kv in sorted(rep['icons'].items())))
    say(len([1 for c, d, _ in b.tags if c == 36]) - len([1 for c, d, _ in a.tags if c == 36]) == 8, 'our art went in as 8 DefineBitsLossless2 (round bronze + steel plates, menu bars, tactical, cross, end turn plate, sneak, sheathe)')
    for p in boxes: print('       %-15s code %2d  stage %7.1f,%6.1f .. %7.1f,%6.1f  window %7.1f,%6.1f .. %7.1f,%6.1f%s' % (p[0], p[1], p[2], p[3], p[4], p[5], p[2] + 240, p[3], p[4] + 240, p[5],
                                                                                           '  panel ' + PANEL_NAMES[p[1]] if p[1] < 6 else ''))
    return bad


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    src = source(a[1])
    if a[0] == 'build':
        if len(a) < 3: sys.exit(__doc__)
        out, rep = patch(src); os.makedirs(os.path.dirname(os.path.abspath(a[2])), exist_ok=True); open(a[2], 'wb').write(out)
        print('%s: %d B, sha256 %s' % (a[2], len(out), hashlib.sha256(out).hexdigest())); return
    bad = check(src, open(a[2], 'rb').read() if len(a) > 2 else None)
    print('FAILED: %d' % bad if bad else 'HUD touch elements: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
