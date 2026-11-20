"""[DOS2] HUD element rects for the floating sticks (pack 3f+, branch dos2-hudrect). Owner 15.11 (pack 3e): "binding to the screen edges is bad -
what about a 16:9 phone, a 4:3 tablet? Get the real position of the objects in pixels and compute the real place and size".

D3PadView (Java) must not spawn a floating stick where the finger lands on a tappable HUD element (hot bar, the iPad button column, the menu
button, minimap, portraits). Pack 3d/3e guessed those places as fractions of the screen (HUD_RECTS_DEFAULT); here the movies themselves say
where their elements are:

  every RX_FRAMES frames an enterFrame tick builds one string
      "<stage.stageWidth>,<stage.stageHeight>|<id>,<x>,<y>,<w>,<h>;<id>,..."
  (x..h = Math.round of element.getBounds(stage): stage space of that movie; an element counts only when it and its holder are visible with
  alpha > 0; a container's box is the union of its visible children, so hidden parts do not grow it) and calls
      ExternalInterface "dmHudRects"(that string)
  when it changed, and at least every KEEP ticks (keep-alive: the host forgets a movie's rects 1.5 s after its last call, so a movie that stops
  ticking - hidden / destroyed - stops blocking the stick).
  The host (dm/host/hle_dos2_touch.c dt_ei_hudrects) maps stage -> window px with the FlashPlayer's origin (+0x10 / +0x14, the offset Iggy's
  mouse move subtracts) and the movie's scale, and hands the rects to Java (DmActivity.nativeDos2HudRects).

  bottomBar_d   hotbar = bottombar_mc (union of its visible children), and every grafted button of tools/dos2_hud_touch.py by name
                (dmVis0..5_mc, dmVisMap_mc, dmVisOpt_mc, dmVisPrev_mc, dmVisNext_mc, dmVisCancel_mc, dmVisEnd_mc, dmVisFoldR_mc, dmVisFoldL_mc,
                dmVisSneak_mc, dmVisDestr_mc); hint4 = bottombar_mc.yHint_mc ("(Y) quick access" under the hot bar)
  minimap_d     minimap = map_mc; btn21 = dmVisTac_mc (the tactical camera button, tools/dos2_minimap_touch.py: the minimap object never gets a
                mouse button in Iggy, so the host presses d-pad up for a world tap inside this rect, pack 4b)
  playerInfo_d  pi<i> = every visible child of container_mc (the portraits)
                dos2-touch6: btn<iconId> = every button info over the hot bar (bottombar_mc.btnList: "(A) Move  (X) Actions"), hold1 = the
                B one in combat and endTurnHint_mc / holdEndTurnHint_mc ("Hold (B) to end the turn": a finger held there holds B, dt_hold_hint)
  overhead_d    hint3 = texts_mc.actionInfo_txt of every visible selection info (charHolder_mc.<CharHolder>.sInfo, the "(X) Actions..." line under
                "(A) <action>" over the selected target; overhead.swf = UI type 5, no mouse at all: tools/dos2_touchui_pak.py ships it as overhead_d.swf,
                hle_dos2_touchui.c maps the type). Every RX[movie] frames (10: the line comes and goes with the selection).
  An id "hint<BTN code>" is a button hint: besides keeping the sticks off, a world tap inside it is that pad button (hle_dos2_touch.c dt_hint_tap,
  branch dos2-dpadbtns; the movie gets no press of its own for it). "btn<BTN code>" is the same for a button that is always there (its rect
  counts as long as the movie's rects live, not only DT_HINT_TTL; branch dos2-dpad2).

Appended methods only (every method body of the input stays byte for byte, every tag but DoABC too); onEventInit re-pointed to a wrapper
(original kept as dmHrInitOrig).

  python -I tools/dos2_hudrect_touch.py check <dir with the built _d movies>
  python -I tools/dos2_hudrect_touch.py build <in.swf> <out.swf>
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, verify_all
from dos2_taps_touch import HEAD, obj, num
from dos2_taps3_touch import doc_ctx, hook_init
import dos2_hud_touch

RX_FRAMES, KEEP = 30, 4      # owner 15.11 (weak devices: no needless polling): a measure every 30 frames (~0.5 s); unchanged rects re-sent every 4th (~2 s, host TTL 5 s)
OVERHEAD = 'overhead_d.swf'  # GUI/overhead.swf (UI type 5) + this pass only
RX = {OVERHEAD: 10}          # frames between two measures where not RX_FRAMES (host: a hint rect counts DT_HINT_TTL ms after its last call)
BOX_MAX = 3000          # a child's box larger than this (stage px) is a sentinel / hidden layer, not a HUD element (pack 3h)
EI_NAME = 'dmHudRects'

GRAFTED = [n for n in [n for n, _, _ in dos2_hud_touch.COLUMN] + [n for n, _ in dos2_hud_touch.EXTRA] if n not in dos2_hud_touch.NOT_HERE]
import dos2_column_touch
COLUMN_MOVIE = dos2_column_touch.COLUMN_ALL + [n for n, _, _ in dos2_column_touch.FOLDS]   # the column's own movie (owner 15.11)
# movie -> [(kind, id, document property or grafted child name)]; kind 'prop' = this.<name>, 'child' = this.getChildByName(name),
# 'each' = every child of this.<name> (id + index)
WHAT = {
    'bottomBar_d.swf': [('prop', 'hotbar', 'bottombar_mc')] + [('child', n, n) for n in GRAFTED] + [('path', 'hint4', 'bottombar_mc.yHint_mc')] +
                       # dos2-touch6 (owner 18.11, combat): the button infos over the hot bar ("(A) Move  (X) Actions": bottombar_mc.btnList, BtnInfo.iconId)
                       # as "btn<code>" (a tap = that button), the one with the B icon in combat and the end-turn hints as "hold1" (held = B held: end the turn)
                       [('binfo', 'btn', 'bottombar_mc'), ('path', 'hold1', 'endTurnHint_mc'), ('path', 'hold1', 'holdEndTurnHint_mc')],
    dos2_column_touch.MOVIE: [('child', n, n) for n in COLUMN_MOVIE],
    'minimap_d.swf': [('prop', 'minimap', 'map_mc')],   # pack 4c: the tactical camera moved to bottomBar_d (dmVisTac_mc there, reported with the grafted buttons)   # pack 4b: btn21 = d-pad up by the host (minimap_d gets no Iggy button)   # pack 3o: map_mc minus its map layer (dmHrBox skips mapHolder_mc) = the ring; frame_mc was only the N ornament   # pack 3h: the ring; map_mc as a whole held a hidden map layer (x 1963..3406 y 37..1470, run t3g)
    'playerInfo_d.swf': [('each', 'pi', 'container_mc')],
    OVERHEAD: [('ohint', 'hint3', 'charHolder_mc')],   # 'ohint': dmHrOh walks the selection infos (sInfo.texts_mc.actionInfo_txt)
}
POST = list(WHAT)


def overhead_source(path):
    """GUI/overhead.swf of the Switch Game.pak (or the file itself): the input of overhead_d.swf"""
    data = open(path, 'rb').read()
    if data[:3] in (b'CWS', b'FWS', b'ZWS'): return data
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == 'Public/Game/GUI/overhead.swf']
    if not e: sys.exit('%s: no Public/Game/GUI/overhead.swf' % path)
    return pak.read(e[0])


class G:
    def __init__(s): s.n = 0
    def __call__(s, base): s.n += 1; return '%s_%d' % (base, s.n)


def patch_any(name, data):
    c = doc_ctx(data); P, S, abc, ref = c.P, c.S, c.abc, c.rbody
    for kind, _, n in WHAT[name]:
        if kind in ('prop', 'each', 'path', 'ohint', 'binfo'): assert abc.trait(c.ci, n.split('.')[0]) is not None, '%s: no %s on the document' % (name, n)
        else: assert n.encode() in abc.strings, '%s: no grafted %s' % (name, n)
        if kind == 'ohint':
            ch, hp = abc.class_index('CharHolder'), abc.class_index('HPBar')
            assert abc.trait(ch, 'sInfo') is not None and abc.trait(hp, 'texts_mc') is not None and b'actionInfo_txt' in abc.strings, '%s: no sInfo / texts_mc / actionInfo_txt' % name
    MATH = abc.pub('Math')
    rnd = lambda val: [('getlex', MATH)] + val + [('callproperty', P('round'), 1)]
    # dmHrInit(): counters (an undefined property + 1 would be NaN for ever)
    c.add('dmHrInit', 0, HEAD + [('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmHrN')), ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmHrK')),
        ('getlocal0',), ('pushstring', S('')), ('setproperty', P('dmHrLast')), ('getlocal0',), ('pushstring', S('')), ('setproperty', P('dmHrS')),
        ('returnvoid',)], ref)
    # dmHrBox(mc): the union of mc's visible children's getBounds(stage), or mc.getBounds(stage) (no children / none visible); null without stage.
    # locals 1 mc 2 r 3 n 4 i 5 ch 6 st
    # pack 3h (run t3g): an empty / never-drawn child reports a sentinel box (+-1e20) and blew the hot bar up to the whole screen width: a
    # child's box counts only when 0 < w, h <= BOX_MAX. locals 7 b
    ok = lambda lbl: [('getlocal', 7), ('getproperty', P('width')), ('pushbyte', 0), ('ifngt', lbl), ('getlocal', 7), ('getproperty', P('width')), ('pushshort', BOX_MAX), ('ifngt', lbl + 'w'),
                      ('jump', lbl), ('L', lbl + 'w'), ('getlocal', 7), ('getproperty', P('height')), ('pushbyte', 0), ('ifngt', lbl), ('getlocal', 7), ('getproperty', P('height')),
                      ('pushshort', BOX_MAX), ('ifgt', lbl)]
    c.add('dmHrBox', 1, HEAD + obj(2) + num(3) + num(4) + obj(5) + obj(6) + obj(7) + [
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'none'),
        ('pushstring', S('numChildren')), ('getlocal1',), ('in',), ('iffalse', 'self'),     # Shape / Bitmap are sealed: no numChildren to read
        ('getlocal1',), ('getproperty', P('numChildren')), ('convert_d',), ('setlocal3',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal1',), ('getlocal', 4), ('callproperty', P('getChildAt'), 1), ('coerce_a',), ('setlocal', 5),
        ('getlocal', 5), ('iffalse', 'next'), ('getlocal', 5), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('alpha')), ('pushbyte', 0), ('ifngt', 'next'),
        # pack 3o: the minimap's map layer (map_mc.mapHolder_mc, far bigger than the ring) is no HUD element: the ring = map_mc without it
        ('getlocal', 5), ('getproperty', P('name')), ('pushstring', S('mapHolder_mc')), ('ifstricteq', 'next'),
        ('getlocal', 5), ('getlocal', 6), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 7), ('getlocal', 7), ('iffalse', 'next')] + ok('next') + [
        ('getlocal2',), ('iftrue', 'uni'),
        ('getlocal', 7), ('setlocal2',), ('jump', 'next'),
        ('L', 'uni'), ('getlocal2',), ('getlocal', 7), ('callproperty', P('union'), 1), ('coerce_a',), ('setlocal2',),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('getlocal2',), ('iftrue', 'done'),
        ('L', 'self'), ('getlocal1',), ('getlocal', 6), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal2',),
        ('L', 'done'), ('getlocal2',), ('returnvalue',),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], ref)
    # dmHrAdd(id, mc): append "id,x,y,w,h;" to dmHrS when mc is visible (alpha > 0) and has a box. locals 1 id 2 mc 3 r
    rr = lambda f: [('getlocal3',), ('getproperty', P(f))]
    field = lambda f: [('pushstring', S(',')), ('add',)] + rnd(rr(f)) + [('add',)]
    c.add('dmHrAdd', 2, HEAD + obj(3) + [
        ('getlocal2',), ('iffalse', 'end'), ('getlocal2',), ('getproperty', P('visible')), ('iffalse', 'end'),
        ('getlocal2',), ('getproperty', P('alpha')), ('pushbyte', 0), ('ifngt', 'end'),
        ('getlocal0',), ('getlocal2',), ('callproperty', P('dmHrBox'), 1), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end')] +
        rr('width') + [('pushbyte', 0), ('ifngt', 'end')] + rr('height') + [('pushbyte', 0), ('ifngt', 'end'),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmHrS')), ('getlocal1',), ('add',)] + field('x') + field('y') + field('width') + field('height') + [
        ('pushstring', S(';')), ('add',), ('setproperty', P('dmHrS')),
        ('L', 'end'), ('returnvoid',)], ref)
    # dmHrEach(prefix, mc): dmHrAdd(prefix + i, child i) for every child of a visible mc. locals 1 prefix 2 mc 3 n 4 i
    c.add('dmHrEach', 2, HEAD + num(3) + num(4) + [
        ('getlocal2',), ('iffalse', 'end'), ('getlocal2',), ('getproperty', P('visible')), ('iffalse', 'end'),
        ('pushstring', S('numChildren')), ('getlocal2',), ('in',), ('iffalse', 'end'),
        ('getlocal2',), ('getproperty', P('numChildren')), ('convert_d',), ('setlocal3',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal0',), ('getlocal1',), ('getlocal', 4), ('add',), ('getlocal2',), ('getlocal', 4), ('callproperty', P('getChildAt'), 1), ('callpropvoid', P('dmHrAdd'), 2),
        ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('L', 'end'), ('returnvoid',)], ref)
    if any(kind == 'ohint' for kind, _, _ in WHAT[name]):
        # dmHrOh(id, holder): overhead_d - for every visible CharHolder child of holder whose selection info (sInfo, in its ohSHolder_mc) is shown:
        # dmHrAdd(id, sInfo.texts_mc.actionInfo_txt) (dmHrAdd skips it when hidden / empty box). locals 1 id 2 holder 3 n 4 i 5 c 6 s 7 t
        nxt = lambda loc: [('getlocal', loc), ('iffalse', 'next'), ('getlocal', loc), ('getproperty', P('visible')), ('iffalse', 'next')]
        c.add('dmHrOh', 2, HEAD + num(3) + num(4) + obj(5) + obj(6) + obj(7) + [
            ('getlocal2',), ('iffalse', 'end'), ('getlocal2',), ('getproperty', P('visible')), ('iffalse', 'end'),
            ('getlocal2',), ('getproperty', P('numChildren')), ('convert_d',), ('setlocal3',),
            ('jump', 'cond'),
            ('L', 'loop'), ('label',),
            ('getlocal2',), ('getlocal', 4), ('callproperty', P('getChildAt'), 1), ('coerce_a',), ('setlocal', 5)] + nxt(5) + [
            ('pushstring', S('sInfo')), ('getlocal', 5), ('in',), ('iffalse', 'next'),
            ('getlocal', 5), ('getproperty', P('sInfo')), ('coerce_a',), ('setlocal', 6)] + nxt(6) + [
            ('getlocal', 6), ('getproperty', P('alpha')), ('pushbyte', 0), ('ifngt', 'next'),
            ('getlocal', 6), ('getproperty', P('parent')), ('coerce_a',), ('setlocal', 7)] + nxt(7) + [
            ('getlocal', 6), ('getproperty', P('texts_mc')), ('coerce_a',), ('setlocal', 7)] + nxt(7) + [
            ('getlocal0',), ('getlocal1',), ('getlocal', 7), ('getproperty', P('actionInfo_txt')), ('callpropvoid', P('dmHrAdd'), 2),
            ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
            ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
            ('L', 'end'), ('returnvoid',)], ref)
    if any(kind == 'binfo' for kind, _, _ in WHAT[name]):
        assert abc.trait(abc.class_index('bottomBar_c_fla.bottombarMC_1'), 'btnList') is not None and abc.trait(c.ci, 'inActiveCombat') is not None and b'iconId' in abc.strings
        # dmHrBi(prefix, bb): every visible element of bb.btnList.content_array as prefix + iconId ("hold1" for the B one in combat).
        # locals 1 prefix 2 bb 3 arr 4 n 5 i 6 el 7 id
        c.add('dmHrBi', 2, HEAD + obj(3) + num(4) + num(5) + obj(6) + obj(7) + [
            ('getlocal2',), ('iffalse', 'end'), ('getlocal2',), ('getproperty', P('btnList')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end'),
            ('getlocal3',), ('getproperty', P('visible')), ('iffalse', 'end'),
            ('getlocal3',), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end'),
            ('getlocal3',), ('getproperty', P('length')), ('convert_d',), ('setlocal', 4),
            ('jump', 'cond'),
            ('L', 'loop'), ('label',),
            ('getlocal3',), ('getlocal', 5), ('getproperty', c.late), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'next'),
            ('getlocal', 6), ('getproperty', P('iconId')), ('pushundefined',), ('ifstricteq', 'next'),
            ('getlocal1',), ('getlocal', 6), ('getproperty', P('iconId')), ('add',), ('coerce_a',), ('setlocal', 7),
            ('getlocal', 6), ('getproperty', P('iconId')), ('pushbyte', 1), ('ifne', 'add'),
            ('getlocal0',), ('getproperty', P('inActiveCombat')), ('iffalse', 'add'),
            ('pushstring', S('hold1')), ('coerce_a',), ('setlocal', 7),
            ('L', 'add'), ('getlocal0',), ('getlocal', 7), ('getlocal', 6), ('callpropvoid', P('dmHrAdd'), 2),
            ('L', 'next'), ('getlocal', 5), ('increment',), ('convert_d',), ('setlocal', 5),
            ('L', 'cond'), ('getlocal', 5), ('getlocal', 4), ('iflt', 'loop'),
            ('L', 'end'), ('returnvoid',)], ref)
    body = []
    for kind, ident, n in WHAT[name]:
        if kind == 'prop': body += [('getlocal0',), ('pushstring', S(ident)), ('getlocal0',), ('getproperty', P(n)), ('callpropvoid', P('dmHrAdd'), 2)]
        elif kind == 'path':   # a.b.c: each step must exist (null -> dmHrAdd skips)
            steps = n.split('.'); body += [('getlocal0',), ('pushstring', S(ident)), ('getlocal0',), ('getproperty', P(steps[0]))]
            for k, st in enumerate(steps[1:]): body += [('dup',), ('iffalse', 'pn%s%d' % (ident, k)), ('getproperty', P(st)), ('L', 'pn%s%d' % (ident, k))]
            body += [('callpropvoid', P('dmHrAdd'), 2)]
        elif kind == 'child': body += [('getlocal0',), ('pushstring', S(ident)), ('getlocal0',), ('pushstring', S(n)), ('callproperty', P('getChildByName'), 1),
                                       ('callpropvoid', P('dmHrAdd'), 2)]
        elif kind == 'ohint': body += [('getlocal0',), ('pushstring', S(ident)), ('getlocal0',), ('getproperty', P(n)), ('callpropvoid', P('dmHrOh'), 2)]
        elif kind == 'binfo': body += [('getlocal0',), ('pushstring', S(ident)), ('getlocal0',), ('getproperty', P(n)), ('callpropvoid', P('dmHrBi'), 2)]
        else: body += [('getlocal0',), ('pushstring', S(ident)), ('getlocal0',), ('getproperty', P(n)), ('callpropvoid', P('dmHrEach'), 2)]
    # dmHrTick(e). locals 1 e 2 st
    c.add('dmHrTick', 1, HEAD + obj(2) + [
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmHrN')), ('pushbyte', 1), ('add',), ('setproperty', P('dmHrN')),
        ('getlocal0',), ('getproperty', P('dmHrN')), ('pushbyte', RX.get(name, RX_FRAMES)), ('iflt', 'end'),
        ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmHrN')),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmHrK')), ('pushbyte', 1), ('add',), ('setproperty', P('dmHrK')),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'end'),
        ('getlocal0',), ('pushstring', S('')), ('getlocal2',), ('getproperty', P('stageWidth')), ('add',), ('pushstring', S(',')), ('add',),
        ('getlocal2',), ('getproperty', P('stageHeight')), ('add',), ('pushstring', S('|')), ('add',), ('setproperty', P('dmHrS')),
        ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'send')] + body + [
        ('L', 'send'),
        ('getlocal0',), ('getproperty', P('dmHrS')), ('getlocal0',), ('getproperty', P('dmHrLast')), ('ifstrictne', 'go'),
        ('getlocal0',), ('getproperty', P('dmHrK')), ('pushbyte', KEEP), ('iflt', 'end'),
        ('L', 'go'),
        ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmHrK')),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmHrS')), ('setproperty', P('dmHrLast')),
        ] + c.ei(2, ('pushstring', S(EI_NAME)), ('getlocal0',), ('getproperty', P('dmHrS'))) + [
        ('L', 'end'), ('returnvoid',)], ref)
    hook_init(c, 'dmHrInitOrig', [], ['dmHrTick'], first=['dmHrInit'])
    return c


def patch(name, data):
    c = patch_any(name, data)
    return c.done(), {'methods': list(c.made), 'moved': dict(c.moved)}


def check(name, src):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(name, src)
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs(); n = len(rep['methods'])
    print('%s (hudrect): %d B -> %d B, +%d methods, sha256 %s' % (name, len(src), len(out), n, hashlib.sha256(out).hexdigest()[:16]))
    say(b.ver == a.ver and b.head == a.head and len(a.tags) == len(b.tags) and all(x[0] == y[0] and (k == ta or x[1] == y[1]) for k, (x, y) in enumerate(zip(a.tags, b.tags))),
        'every tag but DoABC #%d is byte-identical (%d tags)' % (ta, len(a.tags)))
    say(B.strings[:len(A.strings)] == A.strings and B.ns[:len(A.ns)] == A.ns and B.nssets == A.nssets and B.mn[:len(A.mn)] == A.mn and B.ints[:len(A.ints)] == A.ints and B.doubles == A.doubles,
        'constant pool only appended: +%d strings, +%d multinames' % (len(B.strings) - len(A.strings), len(B.mn) - len(A.mn)))
    same = all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc, x.method) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc, y.method)
               for x, y in zip(A.bodies, B.bodies))
    say(same and len(B.bodies) == len(A.bodies) + n and len(B.methods) == len(A.methods) + n, 'all %d method bodies of the input byte-identical; %d methods appended' % (len(A.bodies), n))
    oa, ob = A.owners(), B.owners()
    moved = sorted('%s -> %s' % (oa[m].split('::')[-1], ob[m].split('::')[-1]) for m in oa if ob.get(m) != oa[m])
    say(moved == ['onEventInit -> dmHrInitOrig'], 'renamed methods: ' + ', '.join(moved))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies' % len(B.bodies))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips')
    d = '\n'.join('\n'.join(B.dis(B.methods[m].body)) for m in rep['methods'])
    want = ["'%s'" % EI_NAME, 'getBounds', 'union', 'dmHrAdd'] + ["'%s'" % i for _, i, _ in WHAT[name]]
    miss = [w for w in want if w not in d]
    say(not miss, 'the added code holds: %s%s' % (', '.join(want), '  MISSING ' + str(miss) if miss else ''))
    names = {X for k in B.instances for X in (B.short(t.name) for t in k['traits'])}
    lost = sorted(n for kind, _, n in WHAT[name] if kind != 'child' and any(x not in names for x in n.split('.'))) + sorted(n for kind, _, n in WHAT[name] if kind == 'child' and n.encode() not in B.strings)
    say(not lost, 'every element the code reads exists in the movie (%d)%s' % (len(WHAT[name]), '  MISSING ' + str(lost) if lost else ''))
    return bad


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    if a[0] == 'build':
        if len(a) < 3: sys.exit(__doc__)
        name = os.path.basename(a[2])
        if name not in WHAT: sys.exit('output name must be one of: ' + ' '.join(WHAT))
        out, rep = patch(name, open(a[1], 'rb').read()); open(a[2], 'wb').write(out)
        print('%s: %d B, sha256 %s' % (a[2], len(out), hashlib.sha256(out).hexdigest())); return
    bad = 0
    for name in POST:
        p = Path(a[1]) / name
        if p.exists(): bad += check(name, p.read_bytes())
    print('FAILED: %d' % bad if bad else 'hudrect: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
