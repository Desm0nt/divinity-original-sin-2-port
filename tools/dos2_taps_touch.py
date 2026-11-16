"""[DOS2] Taps that pack 2y dropped (phone run tw2): a second pass over the built `_d` movies + playerInfo.swf -> playerInfo_d.swf.
docs/dos2/TOUCH_CONSOLE_PLAN.md section 8. Appended methods only: every method body of the input stays byte for byte, every tag but DoABC too.

What tw2 showed (work/logs/tw2.log) and what is added:
  msgBox_d      the A/B/X/Y prompts of every confirmation (types 0x4b / 0x4c: quit, character creation ...) were tapped six times, the movie
                got the presses, no ExternalInterface call followed: the listeners tools/dos2_hint_touch.py hangs on the hint elements never
                fired on a phone (not one "dmPadBtn" in tw1 / tw2). Now: stage mouseDown / mouseUp (the way the option list and the pause menu
                work, which did fire), hit test per hint element of popup_mc.list in its own coordinates (icon .. label + margin); a press and a
                release on the same hint = what the pad button does in onEventUp: ExternalInterface ButtonPressed(hint.option, currentDevice),
                or the stock executeSelected for the A hint of a box with an option list. Each hint gets a plate drawn behind it (Graphics of
                the element: gold frame, dark fill), so it reads as a button; a press dims it.
  tutorialBox_d "hold A": the movie's own MOUSE_DOWN / MOUSE_UP listeners call onEventDown(0) / onEventUp(0) (pressedConfirm / releasedConfirm),
                but (1) the accept guard of dos2_hint_touch swallows exactly these calls (IE UIAccept right after a mouse event), (2) a tap is a
                press and a release in one poll: the hold never fills. Now a press on the tutorial box calls the kept original onEventDown(0)
                and an ENTER_FRAME tick keeps the hold alive (presses again if something released it) until holdToSkip_mc.done, then calls the
                original onEventUp(0) = the confirm. A tap on a non modal tutorial pointer = ExternalInterface cancelNonModal (the pad's B).
  bottomBar_d   hot bar slots: ten taps on slots became world clicks ("taken by no UI object"): a slot has only CLICK / ROLL_OVER listeners
                and a press nobody listens to is not "taken"; the stock touch code needs the bar activated by "openedFromTouch" (no such
                string in the Switch main) and then a double click. Now: mouseDown on slotsHolder_mc (the press is the HUD's), stage mouseUp
                on the same slot within 700 ms = ExternalInterface SlotPressed(slot, isEnabled): the call the pad's A makes (main+0x11f1024
                has no "bar active" test); an empty slot does nothing.
  playerInfo_d  portraits: the press is taken, the release never reaches the stock MOUSE_UP listener on frame_mc ("handled 0" on every
                release): no charSel. Now onDown also listens to the stage's mouseUp and runs the stock onIconUp when it did not run
                (charSel = select); a second tap on the same portrait within 500 ms = ExternalInterface dmPanel(1) (the character sheet,
                hle_dos2_touchui_vis.c).

  python -I tools/dos2_taps_touch.py check <dir with the built _d movies> <Switch Game.pak | playerInfo.swf>
  python -I tools/dos2_taps_touch.py build <in.swf> <out.swf>          (the movie is recognised by its classes)
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, OPS, MN_MULTIL, NS_PUBLIC

GUI = 'Public/Game/GUI/'
PI_SRC, PI_OUT = 'playerInfo.swf', 'playerInfo_d.swf'
POST = ('msgBox_d.swf', 'tutorialBox_d.swf', 'bottomBar_d.swf')   # built by the other tools, patched again here
PADX, PADY, MINH = 8, 8, 24            # hint plate / hit box: margin around icon + label, least half height (px, element space)
GOLD, DARK = 0xB89B5E, 0x15110D
HINT_AGAIN_MS, SLOT_TAP_MS, SLOT_AGAIN_MS, PI_DOUBLE_MS, TUT_FRAMES = 500, 700, 600, 500, 900
BTN_A = 2                               # LS_Classes.controllerHelper.BTN_A


class Ctx:
    def __init__(s, data, cls):
        s.swf = Swf(data); (s.tag, s.pre, s.abc), = s.swf.abcs(); abc = s.abc
        s.ci = abc.class_index(cls) if cls else [i for i, c in enumerate(abc.instances) if abc.mname(c['name']).endswith('.MainTimeline')][0]
        s.inst = abc.instances[s.ci]; assert not s.inst['flags'] & 1, 'sealed class'
        s.made = []; s.moved = {}
        s.getTimer, s.EI = abc.pub('getTimer', 'flash.utils'), abc.pub('ExternalInterface', 'flash.external')
        s.ns = abc.namespace(NS_PUBLIC, '')
        s.late = None
        for b in abc.bodies:
            try: ins = abc.ins(b.code)
            except ValueError: continue
            for _, op, ops, _ in ins:
                if OPS[op][0] == 'getproperty' and abc.mn[ops[0]][0] == MN_MULTIL: s.late = ops[0]; break
            if s.late is not None: break

    def P(s, name): return s.abc.qname(s.ns, name)
    def S(s, text): return s.abc.string(text)
    def now(s): return [('findpropstrict', s.getTimer), ('callproperty', s.getTimer, 0)]
    def ei(s, n, *ins): return [('getlex', s.EI)] + list(ins) + [('callpropvoid', s.P('call'), n)]   # n values pushed by ins
    def icon(s, v):
        if v not in s.abc.ints[1:]: s.abc.ints.append(v)
        return s.abc.ints.index(v, 1)

    def ref(s, name):
        t = s.abc.trait(s.ci, name); assert t is not None and t.kind & 15 == 1, '%s is not a method' % name
        assert s.abc.ns[s.abc.mn[t.name][1]] == (NS_PUBLIC, s.S('')), '%s is not public' % name
        return t

    def add(s, name, nparams, lines, ref, like=None, trait=True):
        abc = s.abc
        assert abc.trait(s.ci, name) is None or not trait, 'already patched: ' + name
        m = abc.add_method('dm_' + name, [0] * nparams, 0, asm(lines), 64, 64, ref.init_scope, ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        if like is not None:
            abc.methods[m].params = list(like.params); abc.methods[m].ret = like.ret
            if like.flags & 0x08: abc.methods[m].flags |= 0x08; abc.methods[m].options = list(like.options)
        if trait: s.inst['traits'].append(Trait(s.P(name), 1, 0, m))
        s.made.append(m)
        return m

    def wrap(s, name, keep, body):
        """re-point the trait <name> to a wrapper; the method it had stays reachable as <keep>. body(n params) -> lines after the head."""
        abc = s.abc; t = s.ref(name); orig = abc.methods[t.b]; n = len(orig.params)
        assert abc.trait(s.ci, keep) is None, 'already patched: ' + keep
        m = s.add(name, n, [('getlocal0',), ('pushscope',)] + body(n), orig.body, like=orig, trait=False)
        old = t.b; t.b = m; s.inst['traits'].append(Trait(s.P(keep), 1, 0, old)); s.moved[keep] = (name, old)
        return m

    def done(s):
        s.swf.tags[s.tag][1] = s.pre + s.abc.write()
        return s.swf.write()


HEAD = [('getlocal0',), ('pushscope',)]
def obj(n): return [('pushnull',), ('coerce_a',), ('setlocal', n)]
def num(n, v=0): return [('pushbyte', v), ('convert_d',), ('setlocal', n)]


# ---------------------------------------------------------------- msgBox_d: the A / B / X / Y prompts
def patch_msgbox(data):
    c = Ctx(data, 'msgBox_c_fla.popup_mc_2'); P, S = c.P, c.S; abc = c.abc
    assert abc.trait(c.ci, 'tuExecOrig') is not None, 'not the msgBox_d of tools/dos2_list_touch.py'
    assert c.late is not None
    ref = abc.methods[c.ref('addBtnHint').b].body; assert len(abc.methods[c.ref('addBtnHint').b].params) == 4
    doc = [i for i, k in enumerate(abc.instances) if abc.mname(k['name']).endswith('.MainTimeline')][0]
    assert abc.trait(doc, 'currentDevice') is not None and b'ButtonPressed' in abc.strings
    hc = abc.class_index('LS_Classes.controllerBtnElement')
    for n in ('icon_mc', 'text_txt', 'iconId'): assert abc.trait(hc, n) is not None, 'controllerBtnElement.' + n
    # locals of the two walkers: 1 list 2 array 3 n 4 i 5 el 6 icon 7 label 8 graphics / mx 9 my 10 x0 11 x1 12 half height
    def walk(visible_only, inner):
        return HEAD + obj(1) + obj(2) + num(3) + num(4) + obj(5) + obj(6) + obj(7) + obj(8) + num(9) + num(10) + num(11) + num(12) + ([
            ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'none'),
            ('getlocal0',), ('getproperty', P('stage')), ('iffalse', 'none')] if visible_only else []) + [
            ('getlocal0',), ('getproperty', P('list')), ('coerce_a',), ('setlocal', 1),
            ('getlocal', 1), ('iffalse', 'none'),
            ('getlocal', 1), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal', 2),
            ('getlocal', 2), ('iffalse', 'none'),
            ('getlocal', 2), ('getproperty', P('length')), ('convert_d',), ('setlocal', 3),
            ('jump', 'cond'),
            ('L', 'loop'), ('label',),
            ('getlocal', 2), ('getlocal', 4), ('getproperty', c.late), ('coerce_a',), ('setlocal', 5),
            ('getlocal', 5), ('iffalse', 'next'),
            ('getlocal', 5), ('getproperty', P('icon_mc')), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'next'),
            ('getlocal', 5), ('getproperty', P('text_txt')), ('coerce_a',), ('setlocal', 7), ('getlocal', 7), ('iffalse', 'next'),
            ('getlocal', 6), ('getproperty', P('x')), ('pushbyte', PADX), ('subtract',), ('convert_d',), ('setlocal', 10),
            ('getlocal', 7), ('getproperty', P('x')), ('getlocal', 7), ('getproperty', P('width')), ('add',), ('pushbyte', PADX), ('add',), ('convert_d',), ('setlocal', 11),
            ('getlocal', 6), ('getproperty', P('height')), ('pushbyte', 2), ('divide',), ('pushbyte', PADY), ('add',), ('convert_d',), ('setlocal', 12),
            ('getlocal', 12), ('pushbyte', MINH), ('ifge', 'hok'), ('pushbyte', MINH), ('convert_d',), ('setlocal', 12), ('L', 'hok'),
            ] + inner + [
            ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
            ('L', 'cond'), ('getlocal', 4), ('getlocal', 3), ('iflt', 'loop'),
            ('L', 'none'), ('pushnull',), ('returnvalue',)]
    hit = walk(True, [
        ('getlocal', 5), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseX')), ('convert_d',), ('setlocal', 9),
        ('getlocal', 9), ('getlocal', 10), ('ifnge', 'next'),
        ('getlocal', 9), ('getlocal', 11), ('ifnle', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseY')), ('convert_d',), ('setlocal', 9),
        ('getlocal', 9), ('getlocal', 12), ('negate',), ('ifnge', 'next'),
        ('getlocal', 9), ('getlocal', 12), ('ifnle', 'next'),
        ('getlocal', 5), ('returnvalue',)])
    def rect(inset):
        return [('getlocal', 8), ('getlocal', 10), ('pushbyte', inset), ('add',), ('getlocal', 12), ('negate',), ('pushbyte', inset), ('add',),
                ('getlocal', 11), ('getlocal', 10), ('subtract',), ('pushbyte', 2 * inset), ('subtract',),
                ('getlocal', 12), ('pushbyte', 2), ('multiply',), ('pushbyte', 2 * inset), ('subtract',), ('callpropvoid', P('drawRect'), 4)]
    plate = walk(False, [
        ('getlocal', 5), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', 8), ('getlocal', 8), ('iffalse', 'next'),
        ('getlocal', 8), ('callpropvoid', P('clear'), 0),
        ('getlocal', 8), ('pushint', c.icon(GOLD)), ('pushbyte', 9), ('pushbyte', 10), ('divide',), ('callpropvoid', P('beginFill'), 2)] + rect(0) + [
        ('getlocal', 8), ('callpropvoid', P('endFill'), 0),
        ('getlocal', 8), ('pushint', c.icon(DARK)), ('pushbyte', 9), ('pushbyte', 10), ('divide',), ('callpropvoid', P('beginFill'), 2)] + rect(2) + [
        ('getlocal', 8), ('callpropvoid', P('endFill'), 0)])
    c.add('dmHintHit', 0, hit, ref); c.add('dmPlate', 0, plate, ref)
    c.add('dmHintDown', 1, HEAD + obj(2) + [
        ('getlocal0',), ('callproperty', P('dmHintHit'), 0), ('coerce_a',), ('setlocal', 2),
        ('getlocal0',), ('getlocal', 2), ('setproperty', P('dmHintEl')),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', 2), ('getproperty', P('alpha')), ('setproperty', P('dmHintA')),
        ('getlocal', 2), ('pushbyte', 6), ('pushbyte', 10), ('divide',), ('setproperty', P('alpha')),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmHintUp', 1, HEAD + obj(2) + [
        ('getlocal0',), ('getproperty', P('dmHintEl')), ('coerce_a',), ('setlocal', 2),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmHintEl')),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('getlocal0',), ('getproperty', P('dmHintA')), ('setproperty', P('alpha')),
        ('getlocal0',), ('callproperty', P('dmHintHit'), 0), ('getlocal', 2), ('ifne', 'end'),
        ] + c.now() + [('getlocal0',), ('getproperty', P('dmHintT')), ('subtract',), ('pushshort', HINT_AGAIN_MS), ('iflt', 'end'),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmHintT')),
        ('getlocal', 2), ('getproperty', P('iconId')), ('pushbyte', BTN_A), ('ifne', 'ei'),      # the A prompt of a box with options = accept the selected option
        ('getlocal0',), ('getproperty', P('hlist')), ('iffalse', 'ei'),
        ('getlocal0',), ('getproperty', P('hlist')), ('getproperty', P('length')), ('pushbyte', 0), ('ifngt', 'ei'),
        ('getlocal0',), ('callpropvoid', P('tuExecOrig'), 0), ('returnvoid',),
        ('L', 'ei')] + c.ei(3, ('pushstring', S('ButtonPressed')), ('getlocal', 2), ('getproperty', P('option')),
                           ('getlocal0',), ('getproperty', P('parent')), ('getproperty', P('currentDevice'))) + [
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmHintHook', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmHintOn')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 1), ('getlocal', 1), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmHintOn')),
        ('getlocal', 1), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmHintDown')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal', 1), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmHintUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)], ref)
    c.wrap('addBtnHint', 'dmBtnHintOrig', lambda n: [('getlocal0',)] + [('getlocal', k + 1) for k in range(n)] + [('callpropvoid', P('dmBtnHintOrig'), n),
        ('getlocal0',), ('callpropvoid', P('dmHintHook'), 0), ('getlocal0',), ('callpropvoid', P('dmPlate'), 0), ('returnvoid',)])
    return c


# ---------------------------------------------------------------- tutorialBox_d: a tap holds A
def patch_tutorial(data):
    c = Ctx(data, None); P, S = c.P, c.S; abc = c.abc
    assert abc.mname(c.inst['name']) == 'tutorialBox_c_fla.MainTimeline'
    DN = 'tuEvDownOrig' if abc.trait(c.ci, 'tuEvDownOrig') is not None else 'onEventDown'
    UP = 'tuEvUpOrig' if abc.trait(c.ci, 'tuEvUpOrig') is not None else 'onEventUp'
    hook = 'tuHook' if abc.trait(c.ci, 'tuHook') is not None else 'onEventInit'   # tuHook runs from onEventInit and from every input event
    ref = abc.methods[c.ref(UP).b].body
    for cls in ('tutorialBox_c_fla.modalPopup_13', 'tutorialBox_c_fla.modalPointer_21'):
        k = abc.class_index(cls)
        for n in ('holdToSkip_mc', 'pressedConfirm', 'releasedConfirm'): assert abc.trait(k, n) is not None, cls + '.' + n
    k = abc.class_index('tutorialBox_c_fla.holdToSkip_16')
    for n in ('done', 'started', 'confirmAnim'): assert abc.trait(k, n) is not None, 'holdToSkip.' + n
    f1 = [t for t in c.inst['traits'] if abc.short(t.name).endswith('frame1') and t.kind & 15 == 1][0]
    ev = [abc.S(ops[0]) for _, op, ops, _ in abc.ins(abc.methods[f1.b].body.code) if OPS[op][0] == 'pushstring' and abc.S(ops[0]).startswith('IE ')]
    assert ev and ev[0] == 'IE UIAccept', 'events[0] is not IE UIAccept: %r' % ev[:2]
    c.add('dmTutDown', 1, HEAD + obj(2) + [
        ('getlocal0',), ('getproperty', P('dmTutOn')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('modalBoxVisible')), ('iftrue', 'hold'),
        ('getlocal0',), ('getproperty', P('modalPointerVisible')), ('iftrue', 'hold'),
        ('getlocal0',), ('getproperty', P('nonModalPointer_mc')), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'end'), ('getlocal', 2), ('getproperty', P('visible')), ('iffalse', 'end'),
        ] + c.ei(1, ('pushstring', S('cancelNonModal'))) + [('returnvoid',),
        ('L', 'hold'),
        ('getlocal0',), ('pushbyte', 0), ('callpropvoid', P(DN), 1),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmTutOn')),
        ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmTutN')),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmTutTick', 1, HEAD + obj(2) + obj(3) + [
        ('getlocal0',), ('getproperty', P('dmTutOn')), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmTutN')), ('increment',), ('setproperty', P('dmTutN')),
        ('getlocal0',), ('getproperty', P('dmTutN')), ('pushshort', TUT_FRAMES), ('ifgt', 'off'),
        ('getlocal0',), ('getproperty', P('modalBoxVisible')), ('iffalse', 'ptr'),
        ('getlocal0',), ('getproperty', P('tutorialBox_mc')), ('coerce_a',), ('setlocal', 2), ('jump', 'have'),
        ('L', 'ptr'), ('getlocal0',), ('getproperty', P('modalPointerVisible')), ('iffalse', 'off'),
        ('getlocal0',), ('getproperty', P('modalPointer_mc')), ('coerce_a',), ('setlocal', 2),
        ('L', 'have'), ('getlocal', 2), ('iffalse', 'off'),
        ('getlocal', 2), ('getproperty', P('holdToSkip_mc')), ('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'off'),
        ('getlocal', 3), ('getproperty', P('done')), ('iffalse', 'notdone'),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmTutOn')),
        ('getlocal0',), ('pushbyte', 0), ('callpropvoid', P(UP), 1), ('returnvoid',),      # the hold is full: release = confirm
        ('L', 'notdone'),
        ('getlocal', 3), ('getproperty', P('confirmAnim')), ('iftrue', 'end'),
        ('getlocal', 3), ('getproperty', P('started')), ('iftrue', 'end'),
        ('getlocal', 2), ('callpropvoid', P('pressedConfirm'), 0), ('returnvoid',),         # something released it (the tap's own mouse up): press again
        ('L', 'off'), ('getlocal0',), ('pushfalse',), ('setproperty', P('dmTutOn')),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmTutHook', 0, HEAD + [
        ('getlocal0',), ('getproperty', P('dmTutHooked')), ('iftrue', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmTutHooked')),
        ('getlocal0',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmTutDown')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal0',), ('pushstring', S('enterFrame')), ('getlocal0',), ('getproperty', P('dmTutTick')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)], ref)
    c.wrap(hook, 'dmTutHookOrig', lambda n: [('getlocal0',)] + [('getlocal', k + 1) for k in range(n)] + [('callpropvoid', P('dmTutHookOrig'), n),
        ('getlocal0',), ('callpropvoid', P('dmTutHook'), 0), ('returnvoid',)])
    c.names = (DN, UP, hook)
    return c


# ---------------------------------------------------------------- bottomBar_d: hot bar slots
def patch_bottombar(data):
    c = Ctx(data, None); P, S = c.P, c.S; abc = c.abc
    assert abc.mname(c.inst['name']) == 'bottomBar_c_fla.MainTimeline'
    hook = 'dmVisHook' if abc.trait(c.ci, 'dmVisHook') is not None else 'onEventInit'   # dmVisHook (tools/dos2_hud_touch.py) runs from onEventInit and setActiveInCombat
    ref = abc.methods[c.ref('onEventUp').b].body
    sh = abc.class_index('bottomBar_c_fla.slotsHolder_11')
    for n in ('getSlotOnXY', 'getSlot', 'selectSlot', 'cellHeight'): assert abc.trait(sh, n) is not None, 'slotsHolder.' + n
    for n in ('active', 'isDragging', 'bottombar_mc'): assert abc.trait(c.ci, n) is not None, n
    for n in ('SlotPressed', 'PlaySound', 'UI_Generic_Click'): assert n.encode() in abc.strings, n
    holder = lambda loc, fail: [('getlocal0',), ('getproperty', P('bottombar_mc')), ('coerce_a',), ('setlocal', loc), ('getlocal', loc), ('iffalse', fail),
                                ('getlocal', loc), ('getproperty', P('slotsHolder_mc')), ('coerce_a',), ('setlocal', loc), ('getlocal', loc), ('iffalse', fail)]
    # dmSlotAt(): the slot under the finger (slotsHolder's own coordinates, as its mouseMoveOrDown) or 255
    c.add('dmSlotAt', 0, HEAD + obj(1) + num(3) + [
        ('getlocal0',), ('getproperty', P('stage')), ('iffalse', 'none')] + holder(1, 'none') + [
        ('getlocal', 1), ('getproperty', P('mouseX')), ('pushbyte', 0), ('iflt', 'none'),
        ('getlocal', 1), ('getproperty', P('mouseY')), ('pushbyte', 0), ('iflt', 'none'),
        ('getlocal', 1), ('getproperty', P('mouseY')), ('getlocal', 1), ('getproperty', P('cellHeight')), ('ifgt', 'none'),
        ('getlocal', 1), ('getlocal', 1), ('getproperty', P('mouseX')), ('getlocal', 1), ('getproperty', P('mouseY')), ('callproperty', P('getSlotOnXY'), 2), ('convert_d',), ('setlocal', 3),
        ('getlocal', 1), ('getlocal', 3), ('callproperty', P('getSlot'), 1), ('iffalse', 'none'),
        ('getlocal', 3), ('returnvalue',),
        ('L', 'none'), ('pushshort', 255), ('returnvalue',)], ref)
    c.add('dmSlotDown', 1, HEAD + obj(2) + [
        ('getlocal0',), ('getlocal0',), ('callproperty', P('dmSlotAt'), 0), ('setproperty', P('dmSlotN')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmSlotT')),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmSlotUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmSlotUp', 1, HEAD + num(2) + obj(3) + obj(4) + [
        ('getlocal0',), ('getproperty', P('dmSlotN')), ('convert_d',), ('setlocal', 2),
        ('getlocal0',), ('pushshort', 255), ('setproperty', P('dmSlotN')),
        ('getlocal', 2), ('pushshort', 255), ('ifeq', 'end'),
        ] + c.now() + [('getlocal0',), ('getproperty', P('dmSlotT')), ('subtract',), ('pushshort', SLOT_TAP_MS), ('ifgt', 'end'),      # a hold is the stock drag
        ('getlocal0',), ('getproperty', P('isDragging')), ('iftrue', 'end'),
        ('getlocal0',), ('callproperty', P('dmSlotAt'), 0), ('getlocal', 2), ('ifne', 'end'),
        ] + c.now() + [('getlocal0',), ('getproperty', P('dmSlotF')), ('subtract',), ('pushshort', SLOT_AGAIN_MS), ('iflt', 'end')] + holder(3, 'end') + [
        ('getlocal', 3), ('getlocal', 2), ('callproperty', P('getSlot'), 1), ('coerce_a',), ('setlocal', 4), ('getlocal', 4), ('iffalse', 'end'),
        ('getlocal', 4), ('getproperty', P('inUse')), ('iffalse', 'end'),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmSlotF')),
        ('getlocal0',), ('getproperty', P('active')), ('iffalse', 'press'),
        ('getlocal', 3), ('getlocal', 2), ('pushfalse',), ('callpropvoid', P('selectSlot'), 2),
        ('L', 'press')] + c.ei(2, ('pushstring', S('PlaySound')), ('pushstring', S('UI_Generic_Click'))) +
        c.ei(3, ('pushstring', S('SlotPressed')), ('getlocal', 2), ('getlocal', 4), ('getproperty', P('isEnabled'))) + [
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmSlotHook', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmSlotOn')), ('iftrue', 'end')] + holder(1, 'end') + [
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmSlotOn')),
        ('getlocal0',), ('pushshort', 255), ('setproperty', P('dmSlotN')),
        ('getlocal', 1), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmSlotDown')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)], ref)
    c.wrap(hook, 'dmSlotHookOrig', lambda n: [('getlocal0',)] + [('getlocal', k + 1) for k in range(n)] + [('callpropvoid', P('dmSlotHookOrig'), n),
        ('getlocal0',), ('callpropvoid', P('dmSlotHook'), 0), ('returnvoid',)])
    c.names = (hook,)
    return c


# ---------------------------------------------------------------- playerInfo_d: portraits
def patch_playerinfo(data):
    c = Ctx(data, 'playerInfo'); P, S = c.P, c.S; abc = c.abc
    assert abc.mname(c.inst['name']) == 'playerInfo'
    for n in ('frame_mc', 'hadDown', 'onIconUp'): assert abc.trait(c.ci, n) is not None, n
    assert b'charSel' in abc.strings
    ref = abc.methods[c.ref('onDown').b].body; assert len(abc.methods[c.ref('onDown').b].params) == 1
    # dmPiUp(e): the stage's release after a press on this portrait. State lives on frame_mc (a plain MovieClip).
    c.add('dmPiUp', 1, HEAD + obj(2) + obj(3) + [
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmPiUp')), ('callpropvoid', P('removeEventListener'), 2),
        ('getlocal0',), ('getproperty', P('frame_mc')), ('coerce_a',), ('setlocal', 3), ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('hadDown')), ('iffalse', 'tap'),          # the stock MOUSE_UP of frame_mc did not select: run it (charSel)
        ('getlocal0',), ('getlocal1',), ('callpropvoid', P('onIconUp'), 1),
        ('L', 'tap'),
        ('getlocal', 3), ('getlocal', 2), ('getproperty', P('mouseX')), ('getlocal', 2), ('getproperty', P('mouseY')), ('pushfalse',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('root')), ('getproperty', P('isDragging')), ('iftrue', 'end'),
        ] + c.now() + [('getlocal', 3), ('getproperty', P('dmPiT')), ('subtract',), ('pushshort', PI_DOUBLE_MS), ('ifnlt', 'single'),
        ('getlocal', 3), ('pushbyte', 0), ('setproperty', P('dmPiT')),
        ] + c.ei(2, ('pushstring', S('dmPanel')), ('pushbyte', 1)) + [('returnvoid',),     # second tap: the character sheet of the (now selected) character
        ('L', 'single'), ('getlocal', 3)] + c.now() + [('setproperty', P('dmPiT')),
        ('L', 'end'), ('returnvoid',)], ref)
    c.wrap('onDown', 'dmPiDownOrig', lambda n: obj(2) + [('getlocal0',), ('getlocal1',), ('callpropvoid', P('dmPiDownOrig'), 1),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 2), ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmPiUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)])
    return c


KINDS = {'msgBox_d.swf': (patch_msgbox, ["'ButtonPressed'", 'tuExecOrig', 'drawRect', 'dmHintHit']),
         'tutorialBox_d.swf': (patch_tutorial, ['pressedConfirm', "'cancelNonModal'", 'holdToSkip_mc', "'enterFrame'"]),
         'bottomBar_d.swf': (patch_bottombar, ["'SlotPressed'", 'getSlotOnXY', 'dmSlotAt']),
         PI_OUT: (patch_playerinfo, ["'dmPanel'", 'onIconUp', 'hadDown'])}


def patch(name, data):
    """name = the output movie's name (KINDS); returns (bytes, report)"""
    c = KINDS[name][0](data)
    return c.done(), {'methods': list(c.made), 'moved': dict(c.moved), 'names': getattr(c, 'names', ())}


def check(name, src):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(name, src)
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs(); n = len(rep['methods'])
    print('%s (taps): %d B -> %d B, +%d methods, sha256 %s' % (name, len(src), len(out), n, hashlib.sha256(out).hexdigest()[:16]))
    say(b.ver == a.ver and b.head == a.head and len(a.tags) == len(b.tags) and all(x[0] == y[0] and (k == ta or x[1] == y[1]) for k, (x, y) in enumerate(zip(a.tags, b.tags))),
        'every tag but DoABC #%d is byte-identical (%d tags)' % (ta, len(a.tags)))
    say(B.strings[:len(A.strings)] == A.strings and B.ns[:len(A.ns)] == A.ns and B.nssets == A.nssets and B.mn[:len(A.mn)] == A.mn and B.ints[:len(A.ints)] == A.ints and B.doubles == A.doubles,
        'constant pool only appended: +%d strings, +%d multinames, +%d ints' % (len(B.strings) - len(A.strings), len(B.mn) - len(A.mn), len(B.ints) - len(A.ints)))
    same = all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc, x.method) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc, y.method)
               for x, y in zip(A.bodies, B.bodies))
    say(same and len(B.bodies) == len(A.bodies) + n and len(B.methods) == len(A.methods) + n, 'all %d method bodies of the input byte-identical; %d methods appended' % (len(A.bodies), n))
    oa, ob = A.owners(), B.owners()
    moved = sorted('%s -> %s' % (oa[m].split('::')[-1], ob[m].split('::')[-1]) for m in oa if ob.get(m) != oa[m])
    say(moved == sorted('%s -> %s' % (v[0], k) for k, v in rep['moved'].items()), 'renamed methods: ' + ', '.join(moved))
    ta_, tb_ = [{(B.mname(c['name']), B.short(t.name)) for c in X.instances for t in c['traits']} for X in (A, B)]
    say(ta_ <= tb_, 'no trait removed (+%d)' % len(tb_ - ta_))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies (stack, scope, locals, branch targets, constant indices)' % len(B.bodies))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips')
    d = '\n'.join('\n'.join(B.dis(B.methods[m].body)) for m in rep['methods'])
    miss = [w for w in KINDS[name][1] if w not in d]
    say(not miss, 'the added code holds: %s%s' % (', '.join(KINDS[name][1]), '  MISSING ' + str(miss) if miss else ''))
    if rep['names']: print('       hooks / originals used: %s' % ', '.join(rep['names']))
    return bad


def playerinfo_source(path):
    data = open(path, 'rb').read()
    if data[:3] in (b'CWS', b'FWS', b'ZWS'): return data
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == GUI + PI_SRC]
    if not e: sys.exit('%s: no %s%s' % (path, GUI, PI_SRC))
    return pak.read(e[0])


def main():
    a = sys.argv[1:]
    if len(a) < 3 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    if a[0] == 'build':
        data = open(a[1], 'rb').read(); name = os.path.basename(a[2])
        if name not in KINDS: sys.exit('output name must be one of: ' + ' '.join(KINDS))
        out, rep = patch(name, data); open(a[2], 'wb').write(out)
        print('%s: %d B, sha256 %s' % (a[2], len(out), hashlib.sha256(out).hexdigest())); return
    bad = 0
    for name in POST: bad += check(name, (Path(a[1]) / name).read_bytes())
    bad += check(PI_OUT, playerinfo_source(a[2]))
    print('FAILED: %d' % bad if bad else 'tap fixes: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
