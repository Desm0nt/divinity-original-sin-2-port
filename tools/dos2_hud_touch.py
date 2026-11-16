"""[DOS2] The iPad's on-screen HUD buttons drawn into the console HUD: bottomBar_c.swf -> bottomBar_d.swf (docs/dos2/TOUCH_VISIBLE.md, owner 15.11:
"no element to tap = not acceptable"; the radial menu / pad buttons are not an answer where the iPad has a button).

What is added (tools/dos2_swf_graft.py: the iPad's own art, sprites frozen to their normal frame, no iPad class; every original tag of
bottomBar_c stays byte for byte, only DoABC grows):
  the panel column of rightSideButtons_t (its place on the iPad: right edge, x 1348 of 1440, y 422, 74 px buttons every 80 px) at the right edge
  of our 1920x1080 HUD stage. Our radial menu (panelSelect_c) has six panels, ids = the engine's (main+0x1219430 fills `panelArray`):
      0 Inventory  1 Character  2 Skills  3 Crafting  4 Journal  5 Equipment
    button            art (iPad)                                         tap
    dmVis1_mc  character   toggleBtnSidePanel bg (helmet), 74 px          ExternalInterface dmPanel(1)
    dmVis5_mc  equipment   cmnTabButton bg + ico_charsheet (armour)       dmPanel(5)
    dmVis0_mc  inventory   cmnTabButton bg + ico_inventory                dmPanel(0)
    dmVis2_mc  skills      cmnTabButton bg + ico_skills                   dmPanel(2)
    dmVis3_mc  crafting    cmnTabButton bg + ico_craft                    dmPanel(3)
    dmVis4_mc  journal     cmnTabButton bg + ico_journal                  dmPanel(4)
  dmPanel is the host's (dm/host/hle_dos2_touchui_vis.c): it hands "openPanel"(id) to the player's panelSelect_c object - the very call the
  radial menu makes when A is pressed on a sector (panelSelect_c closeMenu -> ExternalInterface openPanel -> main+0x1218580 -> 0x121a140).
  hot bar rows: hotBar_t's skill set arrows (btnSkillSetLeftFrames / bntSkillSetRightFrames) left and right of the slots ->
      the movie's own ExternalInterface prevHotbar / nextHotbar (+ PlaySound), i.e. what IE UIHotBarPrev / Next do in onEventDown;
  cancel: statusConsole_t's cancelBtn (round, cross) right of the slots -> dmPadBtn(1) = pad B (cancel the action / close, as the Back gesture);
  end turn: statusConsole_t's endTurnBtn (plate + its "End turn" label) above the bars, shown only while setActiveInCombat(true) ->
      the movie's own ExternalInterface endTurn (what the hold-to-end-turn animation calls when it completes).
  d-pad functions (owner 15.11, branch dos2-dpadbtns: "every game function needs a visible element"; controller_1.json: down = ToggleSneak,
  right = DestructionToggle): two round buttons right of the cancel button, where the owner drew them on the 2400x1080 window (x 1947..2040 and
  2066..2165, y 940..1029 = stage x - 240): the plate of statusConsole_t's cancelBtn (its four layers, without the cross) with
      dmVisSneak_mc  statusConsole_t possessIco (a hooded figure; no sneak icon exists in any movie of either pak)  -> dmPadBtn(22) = d-pad down
      dmVisDestr_mc  playerInfo_t combatIcon frame 1 (crossed swords; no destruction icon exists)                    -> dmPadBtn(24) = d-pad right
  each with an invisible hit margin of DPAD_PAD px (as the column's fold arrows, tools/dos2_column_touch.py FOLD_PAD);
  pack 4b (branch dos2-dpad2, owner after pack 4a): the icons are the ones the hot bar shows for these actions - the engine draws the slots'
      icons itself (bottomBar_c's slotholder_mc.iggy_slots is an Iggy custom-draw clip; no icon is a Flash symbol), from the skill icon atlas
      Ability_Skill_Status_Icons (Icons.pak NVN texture + Shared.pak UV list). The action skills of the hot bar (main.raw: ActionSkillSheathe /
      ActionSkillSneak / ActionSkillFlee / ActionAttackGround with icons Action_Sheath / Action_Sneak / Action_Flee / Action_AttackGround)
      give: sneak = Action_Sneak; destruction = Action_Sheath (the d-pad right state of bottomBar_c's own mode indicator is "_unsheated":
      setModeIndicator(sneaking, unsheated, tactical) -> indicatorEnum down / right / up). Cut out of the atlas when the pak builder has the
      Switch data next to Game.pak (atlas_icons), drawn as a disc ICON_D px on the plate; without it the borrowed iPad art stays.
      The cancel, sneak and destruction buttons are BTN_SCALE larger (uniform), all three on the cancel button's centre line, DPAD_GAP apart;
  "(Y) quick access" under the hot bar (bottombar_mc.yHint_mc, bottomBar_c's own hint: a text, no controllerBtnElement): its own mouseDown
      listener + an invisible fill YHINT_PAD px around its text (redrawn by dmYPad when the text's box changes) -> dmPadBtn(4) = Y.
ABC: methods appended to the document class, onEventInit and setActiveInCombat re-pointed to wrappers (originals kept as dmVisInitOrig /
dmVisCombatOrig); a press dims the button (alpha 0.6), the release of the stage on the same button runs the action. No original body changes.

Static only: positions come from the SWF matrices, nothing was run on a phone.

  python -I tools/dos2_hud_touch.py build <Switch Game.pak | bottomBar_c.swf> <iPad Game.pak | dir with the _t movies> <out.swf>
  python -I tools/dos2_hud_touch.py check <Switch Game.pak | bottomBar_c.swf> <iPad Game.pak | dir> [bottomBar_d.swf]
"""
import hashlib, os, struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, NS_PUBLIC, OPS, u30
import dos2_swf_graft as gr

MOVIE = 'bottomBar_d.swf'      # same length as bottomBar_c.swf: the creator's path is rewritten in place (hle_dos2_touchui.c k_hint, type 0x3b)
SRC = 'Public/Game/GUI/bottomBar_c.swf'
IPAD = ('rightSideButtons_t.swf', 'hotBar_t.swf', 'statusConsole_t.swf', 'playerInfo_t.swf', 'minimap_t.swf')
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
ICON_D = 46                                            # atlas icon: a disc this wide on the 69 px plate (own px; the square icon's corners cut)
ICONS = {'dmVisSneak_mc': 'Action_Sneak', 'dmVisDestr_mc': 'Action_Sheath'}   # hot bar action skill icons (see the docstring)
ATLAS_TEX, ATLAS_UV = 'Public/Shared/Assets/Textures/Icons/Ability_Skill_Status_Icons.dds', 'Public/Shared/GUI/Ability_Skill_Status_Icons.lsx'
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
def ipad(path): return {n: _read(path, n) for n in IPAD}


def _bc3(raw):
    """BC3 blocks -> (blocks, 16, 4) RGBA uint8, pixels row-major in the block (dm/tools/astc_pack.py bc3_decode)"""
    import numpy as np
    b = np.frombuffer(raw, np.uint8).reshape(-1, 16); n = len(b)
    a0 = b[:, 0].astype(np.int32); a1 = b[:, 1].astype(np.int32)
    abits = np.zeros(n, np.uint64)
    for i in range(6): abits |= b[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    k = np.arange(8, dtype=np.int32)
    p8 = np.where(k == 0, a0[:, None], np.where(k == 1, a1[:, None], ((8 - k) * a0[:, None] + (k - 1) * a1[:, None] + 3) // 7))
    p6 = np.where(k == 0, a0[:, None], np.where(k == 1, a1[:, None], np.where(k == 6, 0, np.where(k == 7, 255, ((6 - k) * a0[:, None] + (k - 1) * a1[:, None] + 2) // 5))))
    apal = np.where((a0 > a1)[:, None], p8, p6)
    alpha = np.take_along_axis(apal, ((abits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int64), 1)
    c0 = b[:, 8].astype(np.int32) | (b[:, 9].astype(np.int32) << 8); c1 = b[:, 10].astype(np.int32) | (b[:, 11].astype(np.int32) << 8)
    def rgb(c): r, g, bl = (c >> 11) & 31, (c >> 5) & 63, c & 31; return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2)], -1)
    e0, e1 = rgb(c0), rgb(c1)
    cpal = np.stack([e0, e1, (2 * e0 + e1 + 1) // 3, (e0 + 2 * e1 + 1) // 3], 1)
    cbits = b[:, 12].astype(np.uint32) | (b[:, 13].astype(np.uint32) << 8) | (b[:, 14].astype(np.uint32) << 16) | (b[:, 15].astype(np.uint32) << 24)
    cidx = ((cbits[:, None] >> (np.uint32(2) * np.arange(16, dtype=np.uint32))) & np.uint32(3)).astype(np.int64)
    col = np.take_along_axis(cpal, cidx[:, :, None].repeat(3, 2), 1)
    return np.concatenate([col, alpha[:, :, None]], -1).astype(np.uint8)


def _nvn_tex(d):
    """an NVN texture of the Switch packs ('DFvN' + 'HBvN' blocks; a .dds in the LSPK is this, docs/dos2/DOS2_PORT.md): -> (w, h, fmt, level 0
    linear bytes) or None. Block 2 = info (w, h, format, mips), block 3 = the image (Tegra block-linear: 64-byte x 8-row GOBs, GOB block height
    of level 0 = next power of two of its GOB rows, at most 16)"""
    import numpy as np
    if d[:4] != b'DFvN': return None
    o = struct.unpack_from('<I', d, 4)[0]; info = img = None
    while o + 0x24 <= len(d) and d[o:o + 4] == b'HBvN':
        _, size, off, typ, _, _ = struct.unpack_from('<IQQIII', d, o + 4); p = d[o + off:o + off + size]
        if typ == 2: info = p
        elif typ == 3: img = p
        o += off + size
    if info is None or img is None: return None
    _, _, w, h, _, _, fmt, _, _ = struct.unpack_from('<QIIIIIIII', info, 0)
    bpb, bs = {0x42: (8, 4), 0x43: (16, 4), 0x44: (16, 4), 0x25: (4, 1)}.get(fmt, (0, 0))
    if not bpb: return None
    cd = lambda a, b: (a + b - 1) // b
    wb, hb = cd(w, bs), cd(h, bs); bh = 1
    while bh < cd(hb, 8) and bh < 16: bh *= 2
    pitch = cd(wb * bpb, 64)
    x = np.arange(wb * bpb, dtype=np.int64)[None, :]; y = np.arange(hb, dtype=np.int64)[:, None]
    gob = ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)
    idx = (y // (8 * bh)) * (pitch * bh * 512) + (x // 64) * (bh * 512) + ((y % (8 * bh)) // 8) * 512 + gob
    src = np.frombuffer(img, np.uint8)
    if idx.max() >= len(src): return None
    return w, h, fmt, src[idx].tobytes()


def atlas_icons(sw, names=None):
    """pack 4b: {icon name: (w, h, RGBA bytes)} cut from the game's skill icon atlas - the very pixels the hot bar draws for those actions.
    sw = the Switch Game.pak (or anything in that Data directory): Icons.pak there holds the NVN texture (DFvN, BC3, block-linear: _nvn_tex),
    Shared.pak its UV list. None when they are not there (the caller keeps the borrowed iPad art)."""
    import re
    import numpy as np
    import lspk
    names = list(names or sorted(set(ICONS.values())))
    d = os.path.dirname(os.path.abspath(sw)) if not os.path.isdir(sw) else sw
    ip, sp = os.path.join(d, 'Icons.pak'), os.path.join(d, 'Shared.pak')
    if not (os.path.exists(ip) and os.path.exists(sp)): return None
    def one(pak, name):
        p = lspk.Pak(pak); e = [e for e in p.entries if e[0] == name]
        return p.read(e[0]) if e else None
    tex, uv = one(ip, ATLAS_TEX), one(sp, ATLAS_UV)
    if tex is None or uv is None: return None
    t = _nvn_tex(tex)
    if not t or t[2] != 0x44 or t[0] % 4 or t[1] % 4: return None   # BC3 only (what the Switch atlas is: 1536x1536)
    W, H = t[0], t[1]
    img = _bc3(t[3]).reshape(H // 4, W // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(H, W, 4)
    uv = uv.decode('utf-8', 'replace'); out = {}
    for n in names:
        m = re.search(r'value="%s" type="22" />\s*<attribute id="U1" value="([^"]+)"[^>]*>\s*<attribute id="U2" value="([^"]+)"[^>]*>\s*'
                      r'<attribute id="V1" value="([^"]+)"[^>]*>\s*<attribute id="V2" value="([^"]+)"' % re.escape(n), uv)
        if not m: return None
        u1, u2, v1, v2 = map(float, m.groups())
        x0, y0, x1, y1 = int(round(u1 * W)), int(round(v1 * H)), int(round(u2 * W)), int(round(v2 * H))
        out[n] = (x1 - x0, y1 - y0, np.ascontiguousarray(img[y0:y1, x0:x1]).tobytes())
    return out


def bitmap_disc(g, icon, d):
    """a DefineBitsLossless2 of the icon (premultiplied ARGB) + a DefineShape: a disc d px wide centred on 0,0 (8 quadratic arcs) filled with
    the bitmap stretched over its box (clipped, smoothed: fill 0x41, as the iPad's own icons). Returns (shape id, its box)."""
    import math
    w, h, rgba = icon
    px = bytearray(w * h * 4)
    for i in range(w * h):
        r, gg, b, a = rgba[i * 4:i * 4 + 4]
        px[i * 4:i * 4 + 4] = bytes((a, r * a // 255, gg * a // 255, b * a // 255))
    bid = g._alloc(); g.new_defs.append([36, struct.pack('<HBHH', bid, 5, w, h) + zlib.compress(bytes(px), 9)]); g._made[bid] = len(g.new_defs) - 1
    R = d * 10.0                                         # radius in twips (d px / 2 * 20)
    pts = [(int(round(R * math.cos(k * math.pi / 4))), int(round(R * math.sin(k * math.pi / 4)))) for k in range(9)]
    ctl = [(int(round(R / math.cos(math.pi / 8) * math.cos((k + 0.5) * math.pi / 4))), int(round(R / math.cos(math.pi / 8) * math.sin((k + 0.5) * math.pi / 4)))) for k in range(8)]
    pts[8] = pts[0]
    def nb(*v): return max(2, max(abs(int(x)).bit_length() for x in v) + 1)
    rec = []
    # style change: TypeFlag 0, StateNewStyles 0, StateLineStyle 0, StateFillStyle1 1, StateFillStyle0 0, StateMoveTo 1; move to the start;
    # FillStyle1 = 1 (the right side of the path: the arcs run clockwise on screen)
    n = nb(*pts[0]); rec += [(0, 1), (0, 1), (0, 1), (1, 1), (0, 1), (1, 1), (n, 5), (pts[0][0], n), (pts[0][1], n), (1, 1)]
    for k in range(8):
        dv = (ctl[k][0] - pts[k][0], ctl[k][1] - pts[k][1], pts[k + 1][0] - ctl[k][0], pts[k + 1][1] - ctl[k][1]); n = nb(*dv)
        rec += [(1, 1), (0, 1), (n - 2, 4)] + [(v, n) for v in dv]
    rec += [(0, 1), (0, 5)]
    box = gr.wbits([(nb(int(R), -int(R)) , 5)] + [(v, nb(int(R), -int(R))) for v in (-int(R), int(R), -int(R), int(R))])
    fill = b'\x01\x41' + struct.pack('<H', bid) + gr.matrix_bytes(-d / 2.0, -d / 2.0, 20.0 * d / w, 20.0 * d / h)
    sid = g._alloc(); g.new_defs.append([2, struct.pack('<H', sid) + box + fill + b'\x00' + bytes([0x10]) + gr.wbits(rec)]); g._made[sid] = len(g.new_defs) - 1
    return sid, (-d / 2.0, -d / 2.0, d / 2.0, d / 2.0)


def dpad_centres(pb):
    """centres (stage px) of the sneak / destruction plates (own box pb, scaled BTN_SCALE): on the cancel button's centre line, right of it,
    DPAD_GAP apart (the cancel button is the same 69 px plate)"""
    w = (pb[2] - pb[0]) * BTN_SCALE
    sx = CANCEL_XY[0] + w + DPAD_GAP
    return (sx, CANCEL_XY[1]), (sx + w + DPAD_GAP, CANCEL_XY[1])


def graft(swf, ip, icons=None):
    """the visuals; returns the report. icons = atlas_icons(...) (the hot bar's own icons) or None (the borrowed iPad art)"""
    rep = {'placed': [], 'frozen': 0, 'defs': 0, 'dropped': [], 'icons': {}}
    rsb, hot, stc, pit = (Swf(ip[n]) for n in IPAD[:4])   # (minimap_t, IPAD[4], is read by the tactical camera graft)
    g = gr.Graft(swf, rsb)
    bg = g.copy('cmnTabButtonBg_9')                       # frame 1 = the normal plate
    helmet = g.copy('btnSidePanelBg_6')                   # 105 px round helmet
    for k, (name, code, art) in enumerate(COLUMN if COLUMN_HERE else []):
        if art == 'helmet': btn = g.sprite([(helmet, 1, (0, 0, BTN / 105.0), None)])
        else:
            ico = g.copy(art); b = gr.bounds(rsb, g.sm.resolve(art))   # as the iPad's setButton: the icon centred in iconContainer_mc (37, 37)
            btn = g.sprite([(bg, 1, (0, 0), None), (ico, 2, (37 - (b[0] + b[2]) / 2.0, 37 - (b[1] + b[3]) / 2.0), None)])
        x, y = COL_X, COL_Y + COL_STEP * k
        g.place(btn, x, y, name); rep['placed'].append((name, code, x, y, x + BTN, y + BTN))
    if COLUMN_HERE:
        ico = g.copy('ico_map'); b = gr.bounds(rsb, g.sm.resolve('ico_map'))
        btn = g.sprite([(bg, 1, (0, 0), None), (ico, 2, (37 - (b[0] + b[2]) / 2.0, 37 - (b[1] + b[3]) / 2.0), None)])
        g.place(btn, MAP_XY[0], MAP_XY[1], 'dmVisMap_mc'); rep['placed'].append(('dmVisMap_mc', 14, MAP_XY[0], MAP_XY[1], MAP_XY[0] + BTN, MAP_XY[1] + BTN))
    c = g.copy('btnOptions'); b = gr.bounds(rsb, g.sm.resolve('btnOptions')); g.place(c, OPT_XY[0], OPT_XY[1], 'dmVisOpt_mc')
    rep['placed'].append(('dmVisOpt_mc', 15, OPT_XY[0] + b[0], OPT_XY[1] + b[1], OPT_XY[0] + b[2], OPT_XY[1] + b[3]))
    rep['frozen'] += len(g.frozen); rep['dropped'] += g.dropped; g.finish(); rep['defs'] += len(g.new_defs)
    # pack 4c (owner: "the tactical camera the size of the options button, and pressable like it"): minimap_d gets no mouse buttons, so the
    # iPad's tactical camera button (minimap_t tacticalCamera_mc: plate bg_mc + ico_tacticalCamera in iconContainer_mc) lives here, under the
    # options button, scaled to its width; dmOptPlace moves both. A tap = dmPadBtn(21) = d-pad up = ToggleTacticalCamera.
    if 'minimap_t.swf' in ip:
        import dos2_minimap_touch as mmt
        mt = Swf(ip['minimap_t.swf']); mtm = gr.Movie(mt)
        tc = mmt._root(mt, 'tacticalCamera_mc'); bgc = mmt._child(mt, mtm, tc['char'], 'bg_mc')['char']
        icp = mmt._xy(mmt._child(mt, mtm, tc['char'], 'iconContainer_mc'))
        gt = gr.Graft(swf, mt); nb = gt.copy(bgc); ni = gt.copy('ico_tacticalCamera'); ib = gr.bounds(mt, mtm.resolve('ico_tacticalCamera'))
        btn = gt.sprite([(nb, 1, (0, 0), None), (ni, 2, (icp[0] - (ib[0] + ib[2]) / 2.0, icp[1] - (ib[1] + ib[3]) / 2.0), None)])
        pw = gr.bounds(mt, bgc); ts = (b[2] - b[0]) / float(pw[2] - pw[0])
        tx, ty = OPT_XY[0] + b[0] - pw[0] * ts, OPT_XY[1] + b[3] + TAC_GAP - pw[1] * ts
        gt.place(btn, tx, ty, 'dmVisTac_mc', ts)
        rep['placed'].append(('dmVisTac_mc', 20, tx + pw[0] * ts, ty + pw[1] * ts, tx + pw[2] * ts, ty + pw[3] * ts)); rep['tac_off'] = tx - OPT_XY[0]
        rep['frozen'] += len(gt.frozen); rep['dropped'] += gt.dropped; gt.finish(); rep['defs'] += len(gt.new_defs)
    g = gr.Graft(swf, hot)
    for name, sym, (cx, cy) in ((('dmVisFoldR_mc', 'bntSkillSetRightFrames_62', FOLDR_C), ('dmVisFoldL_mc', 'btnSkillSetLeftFrames_66', FOLDL_C)) if COLUMN_HERE else ()):
        c = g.copy(sym); b = gr.bounds(hot, g.sm.resolve(sym)); k = FOLD_SCALE
        x, y = cx - (b[0] + b[2]) / 2.0 * k, cy - (b[1] + b[3]) / 2.0 * k; g.place(c, x, y, name, k)
        rep['placed'].append((name, 16, x + b[0] * k, y + b[1] * k, x + b[2] * k, y + b[3] * k))
    for name, sym, (x, y) in (('dmVisPrev_mc', 'btnSkillSetLeftFrames_66', PREV_XY), ('dmVisNext_mc', 'bntSkillSetRightFrames_62', NEXT_XY)):
        c = g.copy(sym); b = gr.bounds(hot, g.sm.resolve(sym)); g.place(c, x, y, name, ARROW_SCALE)
        rep['placed'].append((name, 10 if 'Prev' in name else 11, x + b[0] * ARROW_SCALE, y + b[1] * ARROW_SCALE, x + b[2] * ARROW_SCALE, y + b[3] * ARROW_SCALE))
    rep['frozen'] += len(g.frozen); rep['dropped'] += g.dropped; g.finish(); rep['defs'] += len(g.new_defs)
    g = gr.Graft(swf, stc)
    k = BTN_SCALE   # pack 4b: cancel / sneak / destruction scaled about their centres (the plate's art is centred on 0,0)
    c = g.copy('cancelBtn'); b = gr.bounds(stc, g.sm.resolve('cancelBtn'))
    cx, cy = CANCEL_XY[0] - (b[0] + b[2]) / 2.0 * k, CANCEL_XY[1] - (b[1] + b[3]) / 2.0 * k; g.place(c, cx, cy, 'dmVisCancel_mc', k)
    rep['placed'].append(('dmVisCancel_mc', 12, cx + b[0] * k, cy + b[1] * k, cx + b[2] * k, cy + b[3] * k))
    c = g.copy('endTurnBtn'); b = gr.bounds(stc, g.sm.resolve('endTurnBtn')); g.place(c, END_XY[0], END_XY[1], 'dmVisEnd_mc')
    rep['placed'].append(('dmVisEnd_mc', 13, END_XY[0] + b[0], END_XY[1] + b[1], END_XY[0] + b[2], END_XY[1] + b[3]))
    # sneak / destruction (branch dos2-dpadbtns): the cancel button's plate (its named layers, the cross left out) + an icon centred on it
    sm = g.sm; cb = sm.resolve('cancelBtn'); pb = gr.bounds(stc, cb)
    st = gr.frame_state(stc.tags[sm.defs[cb]][1], 1)
    plate = [(g.copy(r['char']), dep, r['matrix'] or gr.matrix_bytes(0, 0)) for dep, r in sorted(st.items()) if r['name'] in PLATE_PARTS]
    assert len(plate) == len(PLATE_PARTS), 'cancelBtn layers: %s' % sorted(r['name'] for r in st.values())
    top = max(d for _, d, _ in plate) + 2
    def on_plate(ico, b, side=DPAD_ICON):   # [(id, depth, matrix, None)] of the plate + the icon (its box b) scaled to `side` and centred on the plate's centre
        s = side / float(max(b[2] - b[0], b[3] - b[1])); px, py = (pb[0] + pb[2]) / 2.0, (pb[1] + pb[3]) / 2.0
        return [(c, dep, m, None) for c, dep, m in plate] + [(ico, top, (px - (b[0] + b[2]) / 2.0 * s, py - (b[1] + b[3]) / 2.0 * s, s), None)]
    sneak_xy, destr_xy = dpad_centres(pb); rep['centres'] = (sneak_xy, destr_xy)
    def put(gg, name, code, btn, xy):   # xy = the plate's centre (stage)
        x, y = xy[0] - (pb[0] + pb[2]) / 2.0 * k, xy[1] - (pb[1] + pb[3]) / 2.0 * k
        gg.place(btn, x, y, name, k); rep['placed'].append((name, code, x + pb[0] * k, y + pb[1] * k, x + pb[2] * k, y + pb[3] * k))
    if icons:   # pack 4b: the hot bar's own icons from the game's atlas, a disc ICON_D px on the plate
        for name, code in (('dmVisSneak_mc', 18), ('dmVisDestr_mc', 19)):
            sid, sb = bitmap_disc(g, icons[ICONS[name]], ICON_D)
            put(g, name, code, g.sprite(on_plate(sid, sb, ICON_D)), sneak_xy if code == 18 else destr_xy)
            rep['icons'][name] = '%s (atlas %dx%d -> disc %d px)' % (ICONS[name], icons[ICONS[name]][0], icons[ICONS[name]][1], ICON_D)
        rep['frozen'] += len(g.frozen); rep['dropped'] += g.dropped; g.finish(); rep['defs'] += len(g.new_defs)
        return rep
    ico = g.copy('possessIco_71'); put(g, 'dmVisSneak_mc', 18, g.sprite(on_plate(ico, gr.bounds(stc, sm.resolve('possessIco_71')))), sneak_xy)
    rep['icons']['dmVisSneak_mc'] = 'statusConsole_t possessIco_71 (borrowed: no game atlas)'
    rep['frozen'] += len(g.frozen); rep['dropped'] += g.dropped; g.finish(); rep['defs'] += len(g.new_defs)
    g2 = gr.Graft(swf, pit)   # the destruction icon lives in playerInfo_t; the plate's ids are the ones the statusConsole_t graft just defined
    ico = g2.copy('combatIcon_59'); put(g2, 'dmVisDestr_mc', 19, g2.sprite(on_plate(ico, gr.bounds(pit, g2.sm.resolve('combatIcon_59')))), destr_xy)
    rep['icons']['dmVisDestr_mc'] = 'playerInfo_t combatIcon_59 (borrowed: no game atlas)'
    rep['frozen'] += len(g2.frozen); rep['dropped'] += g2.dropped; g2.finish(); rep['defs'] += len(g2.new_defs)
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


def patch(data, ip, icons=None):
    """bottomBar_c bytes + {iPad movie name: bytes} [+ atlas_icons()] -> (bottomBar_d bytes, report)"""
    swf = Swf(data); rep = graft(swf, ip, icons); wire(swf, rep)
    return swf.write(), rep


def check(src, ip, built=None, icons=None):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(src, ip, icons)
    if built is not None: say(built == out, 'the given %s is what this tool builds from these sources (%d B)' % (MOVIE, len(built)))
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs()
    print('%s -> %s: %d B -> %d B, +%d definition tags, +%d root instances, %d sprites frozen to one frame, +%d methods, sha256 %s' % (
        SRC.split('/')[-1], MOVIE, len(src), len(out), rep['defs'], len(rep['placed']), rep['frozen'], len(rep['methods']), hashlib.sha256(out).hexdigest()[:16]))
    say(not gr.check(src, out), 'graft: every original tag of bottomBar_c byte for byte and in order (DoABC aside), new ids unique, every use after its definition, re-read equal')
    for n, sym in (('rightSideButtons_t.swf', 'cmnTabButton'), ('rightSideButtons_t.swf', 'toggleBtnSidePanel'), ('hotBar_t.swf', 'btnSkillSetLeftFrames_66'),
                   ('statusConsole_t.swf', 'cancelBtn'), ('statusConsole_t.swf', 'endTurnBtn'), ('statusConsole_t.swf', 'possessIco_71'), ('playerInfo_t.swf', 'combatIcon_59')):
        say(not gr.selftest(ip[n], gr.Movie(Swf(ip[n])).resolve(sym), src, quiet=True), 'graft round trip %s %s: copied tree mapped back = the iPad tags byte for byte' % (n, sym))
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
    if icons:   # the bitmaps went in as DefineBitsLossless2 + a disc shape with a clipped bitmap fill
        say(len([1 for c, d, _ in b.tags if c == 36]) - len([1 for c, d, _ in a.tags if c == 36]) >= 2 and all(n in rep['icons'] for n in ICONS),
            'the hot bar\'s own icons on the d-pad buttons: %s' % ', '.join(sorted(set(ICONS.values()))))
    for p in boxes: print('       %-15s code %2d  stage %7.1f,%6.1f .. %7.1f,%6.1f  window %7.1f,%6.1f .. %7.1f,%6.1f%s' % (p[0], p[1], p[2], p[3], p[4], p[5], p[2] + 240, p[3], p[4] + 240, p[5],
                                                                                           '  panel ' + PANEL_NAMES[p[1]] if p[1] < 6 else ''))
    print('       class links of the iPad not carried over (plain MovieClips here): %s' % ', '.join(sorted({n.split('.')[-1] for _, n in rep['dropped']})))
    return bad


def main():
    a = sys.argv[1:]
    if len(a) < 3 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    src = source(a[1]); ip = ipad(a[2]); icons = atlas_icons(a[1])   # pack 4b: the hot bar icons when Icons.pak / Shared.pak sit next to the Switch Game.pak
    if a[0] == 'build':
        if len(a) < 4: sys.exit(__doc__)
        out, rep = patch(src, ip, icons); os.makedirs(os.path.dirname(os.path.abspath(a[3])), exist_ok=True); open(a[3], 'wb').write(out)
        print('%s: %d B, sha256 %s' % (a[3], len(out), hashlib.sha256(out).hexdigest())); return
    bad = check(src, ip, open(a[3], 'rb').read() if len(a) > 3 else None, icons)
    print('FAILED: %d' % bad if bad else 'HUD touch elements: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
