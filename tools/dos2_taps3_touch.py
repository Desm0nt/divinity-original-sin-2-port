"""[DOS2] Third tap pass (branch dos2-taps3, owner run tw3): a further pass over the built `_d` movies, appended methods only (every method body of
the input stays byte for byte, every tag but DoABC too). docs/dos2/TOUCH_CONSOLE_PLAN.md section 9.

Why: tw2 showed that the mouseDown / mouseUp listeners tools/dos2_hint_touch.py hangs on the hint elements never fire on the phone, while stage
listeners + geometry do (pause menu, option lists). So everything here hangs on the STAGE and finds its target by geometry at tap time, from
the live display objects (hint bars that bounce / scroll as a marquee, lists that scroll: the rects are computed when the finger lands).

1. HINTS by geometry (HINTS: saveLoad_d optionsSettings_d mods_d mainMenu_d characterCreation_d = every movie still on the element-listener
   hint patch; msgBox_d / tutorialBox_d have their own pass in tools/dos2_taps_touch.py). The document class gets:
     dmHgWalk  depth-first walk of the visible display tree (alpha > 0; a child clipped by a mask / scrollRect that the finger is outside of is
               skipped with its subtree) for LS_Classes.controllerBtnHint instances (subclasses too: mainMenu BtnHintBlah) whose icon_mc carries
               tuBtn = the pad code (set by the tools/dos2_hint_touch.py setHintIcon wrapper: so exactly the hints the old patch knew);
     dmHgEl    the hint's button: its parent when that is a small element (<= 6 children, holds a TextField label, not taller than 150 px:
               controllerBtnElement, BtnHintMC), else the bare icon (an icon inline in a text: buttonHintTextHelper, e.g. "(A) Begin" of the
               difficulty panel, "(Y) / (A)" of the origin panel) whose box is stretched to the right by EXT icon heights over its label;
     dmHgBox   the box: union of the children's getBounds(element) + margin (computed from the children, so the plate never feeds back);
     dmHgDown / dmHgUp  stage mouseDown / mouseUp: press and release on the same hint within TAP_MS = ExternalInterface dmPadBtn(tuBtn) - the
               host presses that pad button (hle_dos2_touch.c, A is delayed past the tap-is-not-accept guards), i.e. the movie's own pad path.
               The press dims the button; the old element listener (if it ever fires) is disarmed (icon_mc.tuT = 0), so no double press;
     dmHgPlate a plate behind every hint (Graphics of the element: gold frame + dark fill under its children; a bare inline icon gets a gold frame
               only, so the text next to it stays readable), refreshed every PLATE_FRAMES frames from an enterFrame tick, redrawn only when the
               box changed. Hook: onEventInit re-pointed to a wrapper (original kept as dmHgInitOrig); the tick retries the stage listeners.
2. OPTIONS ROWS (optionsSettings_d, all four tabs share the movie: types 0x58..0x5b): stage geometry over mainMenu_mc.list.content_array:
     press on a row     = the stock list selection (list.selectMC) - the pad's look and tooltip;
     slider (SliderComp) press / drag on the bar = value from the finger over slider_mc.m_bg_mc (min .. max, snapped to snapInterval), set through
                        the stock slider_mc.value + SliderComp.onChange (amount text, EI menuSliderID = what the pad's left / right does), only
                        when the snapped value changes; release = the stock SliderComp.onhandleUp (EI sliderHandleUp, the PC mouse path);
     selector / checkbox (SelectorMC; a checkbox is a SelectorMC with isCheckBox) release on the left half of the row = selList.previous, right
                        half = selList.next (+ the stock click sound), same as handleEvent("IE UILeft" / "IE UIRight");
     button (Menu_button: "apply", "controls" ...) release on it = buttonPressed + buttonReleased (EI buttonPressed(id)) - the A path.
     a vertical drag over the rows moves the stock selection one row per ROW_PX (finger up = list.next, down = list.previous), the list
                        scrolls to it as with the pad; a release after such a drag (or DRAG_PX away from the press) runs nothing.
3. CHARACTER CREATION step tabs (characterCreation_d, header_mc.stepTabs): tap on a tab = go to that step, one pad step (the cc tool's tcPad =
   IE UICreationTabNext / Prev = EI nextStep / previousStep) at a time until it is the current tab (add_cc_tabs). The origin panel's
   "(Y) origin info" / "(A) listen to the story" and the bottom bar "(+) start / (L) controls / (B) main menu" are hints: part 1.
4. SKILLS PANEL (inventorySkillPanel_d): a no-op mouseDown listener on mainpanel_mc, so a press on the panel is the panel's, not the world's
   (patch_skills). Panel tabs by tap and skill select / use by tap: not done in this pass.
5. HINT BAR by its own list (optionsSettings_d mainMenu_mc.buttonHint_mc, mainMenu_d mainMenu_mc.buttonHintBar_mc; add_hintbar, branch
   dos2-splash): owner, pack 3b - the options "(X) Default (B) Cancel (A) Accept" bar did not answer taps. Stage mouseX / mouseY +
   element.getBounds(stage) over the bar's content_array (row +- HB_PADY = 40 px), pad code = controllerBtnElement.iconId; press + release on
   the same element = EI dmPadBtn(iconId). Shares part 1's dmHgF stamp (no double press). Traced (HB_TRACE codes, see add_hintbar).
6. TITLE SPLASH (mainMenu_d, add_splash): "Press (A) to start" - a press anywhere while the splash is up = EI dmPadBtn(2), the pad's A;
   listener from frame1 / onEventInit / an enterFrame retry; traced (SP_TRACE codes).
   Both pack-3c fixes are wrapped early (hook_init early=True: our listeners before the original onEventInit body).

  python -I tools/dos2_taps3_touch.py check <dir with the built _d movies>      static verification of every movie of KINDS present there
  python -I tools/dos2_taps3_touch.py build <in.swf> <out.swf>
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, OPS
from dos2_taps_touch import Ctx, HEAD, obj, num

HINTS = ('saveLoad_d.swf', 'optionsSettings_d.swf', 'mods_d.swf', 'mainMenu_d.swf', 'characterCreation_d.swf')
PAD, EXT, TAP_MS, AGAIN_MS, PLATE_FRAMES, DRAG_PX, ROW_PX = 8, 4, 1500, 400, 15, 40, 48
AGAIN_SPLASH_MS = 1500
HB_A_MS = 100            # dmPadBtn(code, delay): a hint bar press in 100 ms (was 750: owner 3e, 'Accept' felt dead)
SPLASH_A_MS = 60          # dmPadBtn(2, delay): the splash's close is no guarded accept, so A almost at once (owner 3e: the tap felt dead)
# mainMenu_d splash trace: EI dmPadBtn(code) with codes that are no pad button -> the host logs `[touch] dmPadBtn(<code>): no pad button ...`
HB_PADX, HB_PADY, HB_TRACES = 8, 40, 3
# hint bar trace (add_hintbar), same channel as SP_TRACE
HB_TRACE = {'press on a hint': 950, 'press: no bar on screen': 951, 'press: bar up, no hint under the finger': 952, 'release: too late': 953,
            'release: off the pressed hint': 954, 'release: part 1 just pressed': 955, 'release: no iconId': 956, 'release: pad button': 957,
            'stage listeners added': 960}
SP_TRACE = {'tap on the splash: A': 900, 'again too soon': 901, 'no splashScreen_mc': 911, 'splash hidden': 912, 'splash closing': 913,
            'stage listener added': 930}
GOLD, DARK = 0xB89B5E, 0x15110D


def doc_ctx(data):
    c = Ctx(data, None)
    c.ref_init = c.ref('onEventInit'); c.rbody = c.abc.methods[c.ref_init.b].body
    assert not c.abc.methods[c.ref_init.b].params, 'onEventInit takes parameters'
    return c


def lt(c, a, b, lbl):            # if a < b goto lbl (a, b: lists that push a number)
    return a + b + [('iflt', lbl)]


# ---------------------------------------------------------------- 1. hints by geometry
def tut_off_ops(c, loc):
    """icon.tuT = 0 only where icon has a tuT (the dos2_hint_touch.py hint class is dynamic; the options bar's controllerBtnElement.icon_mc is
    sealed: an unguarded write throws a ReferenceError and aborts the handler - pack 3d run t3d, options 'Accept' never pressed)."""
    lbl = 'tuX%d' % loc
    return [('pushstring', c.S('tuT')), ('getlocal', loc), ('in',), ('iffalse', lbl),
            ('getlocal', loc), ('pushbyte', 0), ('setproperty', c.P('tuT')), ('L', lbl)]


def add_hints(c):
    P, S, abc, ref = c.P, c.S, c.abc, c.rbody
    tut_off = lambda loc: tut_off_ops(c, loc)
    HINT = abc.pub('controllerBtnHint', 'LS_Classes'); TF = abc.pub('TextField', 'flash.text'); DOC = abc.pub('DisplayObjectContainer', 'flash.display')
    assert b'controllerBtnHint' in abc.strings and c.late is not None
    hc = abc.class_index('LS_Classes.controllerBtnHint')
    assert abc.trait(hc, 'tuHintOrig') is not None, 'not a movie of tools/dos2_hint_touch.py (no tuHintOrig)'
    A = lambda loc, k: [('getlocal', loc), ('pushbyte', k), ('getproperty', c.late)]
    # dmHgEl(h): the hint's button. locals 1 h 2 p 3 n 4 i 5 ch
    c.add('dmHgEl', 1, HEAD + obj(2) + num(3) + num(4) + obj(5) + [
        ('getlocal1',), ('getproperty', P('parent')), ('coerce_a',), ('setlocal2',),
        ('getlocal2',), ('iffalse', 'self'),
        ('getlocal2',), ('getlex', DOC), ('istypelate',), ('iffalse', 'self'),
        ('getlocal2',), ('getproperty', P('height')), ('pushshort', 150), ('ifgt', 'self'),
        ('getlocal2',), ('getproperty', P('numChildren')), ('convert_d',), ('setlocal3',),
        ('getlocal3',), ('pushbyte', 6), ('ifgt', 'self'),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal2',), ('getlocal', 4), ('callproperty', P('getChildAt'), 1), ('coerce_a',), ('setlocal', 5),
        ('getlocal', 5), ('getlex', TF), ('istypelate',), ('iffalse', 'next'),
        ('getlocal2',), ('returnvalue',),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('L', 'self'), ('getlocal1',), ('returnvalue',)], ref)
    # dmHgBox(el, h): [x0, y0, x1, y1] in el space or null. locals 1 el 2 h 3 n 4 i 5 ch 6 r 7 x0 8 y0 9 x1 10 y1 11 any 12 v 13 pad
    def mm(loc, val, op, lbl):   # loc = val if val <op> loc
        return val + [('convert_d',), ('setlocal', 12), ('getlocal', 12), ('getlocal', loc), (op, lbl), ('getlocal', 12), ('setlocal', loc), ('L', lbl)]
    r = lambda f: [('getlocal', 6), ('getproperty', P(f))]
    c.add('dmHgBox', 2, HEAD + obj(3) + num(4) + obj(5) + obj(6) + num(7) + num(8) + num(9) + num(10) + num(11) + num(12) + num(13) + [
        ('pushbyte', PAD), ('convert_d',), ('setlocal', 13),
        ('getlocal1',), ('getlocal2',), ('ifstrictne', 'kids'),
        # bare inline icon: the icon's box, stretched to the right over its label
        ('getlocal2',), ('getproperty', P('icon_mc')), ('coerce_a',), ('setlocal', 5), ('getlocal', 5), ('iffalse', 'none'),
        ('getlocal', 5), ('getlocal2',), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'none'),
        ] + r('x') + [('convert_d',), ('setlocal', 7)] + r('y') + [('convert_d',), ('setlocal', 8)] +
        r('x') + r('width') + [('add',), ('convert_d',), ('setlocal', 9)] + r('y') + r('height') + [('add',), ('convert_d',), ('setlocal', 10),
        ('getlocal', 10), ('getlocal', 8), ('subtract',), ('pushbyte', 4), ('divide',), ('convert_d',), ('setlocal', 13),
        ('getlocal', 9), ('getlocal', 10), ('getlocal', 8), ('subtract',), ('pushbyte', EXT), ('multiply',), ('add',), ('convert_d',), ('setlocal', 9),
        ('jump', 'pad'),
        ('L', 'kids'),
        ('getlocal1',), ('getproperty', P('numChildren')), ('convert_d',), ('setlocal3',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal1',), ('getlocal', 4), ('callproperty', P('getChildAt'), 1), ('coerce_a',), ('setlocal', 5),
        ('getlocal', 5), ('iffalse', 'next'), ('getlocal', 5), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 5), ('getlocal1',), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'next'),
        ] + r('width') + [('pushbyte', 0), ('ifngt', 'next'),
        ('getlocal', 11), ('iftrue', 'merge'),
        ] + r('x') + [('convert_d',), ('setlocal', 7)] + r('y') + [('convert_d',), ('setlocal', 8)] +
        r('x') + r('width') + [('add',), ('convert_d',), ('setlocal', 9)] + r('y') + r('height') + [('add',), ('convert_d',), ('setlocal', 10),
        ('pushbyte', 1), ('convert_d',), ('setlocal', 11), ('jump', 'next'),
        ('L', 'merge')] + mm(7, r('x'), 'ifnlt', 'm1') + mm(8, r('y'), 'ifnlt', 'm2') + mm(9, r('x') + r('width') + [('add',)], 'ifngt', 'm3') +
        mm(10, r('y') + r('height') + [('add',)], 'ifngt', 'm4') + [
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('getlocal', 11), ('iffalse', 'none'),
        ('L', 'pad'),
        ('getlocal', 7), ('getlocal', 13), ('subtract',), ('getlocal', 8), ('getlocal', 13), ('subtract',),
        ('getlocal', 9), ('getlocal', 13), ('add',), ('getlocal', 10), ('getlocal', 13), ('add',), ('newarray', 4), ('returnvalue',),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], ref)
    # dmHgIn(o, box): the finger inside box (o's own coordinates)
    c.add('dmHgIn', 2, HEAD + num(3) + [
        ('getlocal2',), ('iffalse', 'no'),
        ('getlocal1',), ('getproperty', P('mouseX')), ('convert_d',), ('setlocal3',),
        ('getlocal3',)] + A(2, 0) + [('ifnge', 'no'), ('getlocal3',)] + A(2, 2) + [('ifnle', 'no'),
        ('getlocal1',), ('getproperty', P('mouseY')), ('convert_d',), ('setlocal3',),
        ('getlocal3',)] + A(2, 1) + [('ifnge', 'no'), ('getlocal3',)] + A(2, 3) + [('ifnle', 'no'),
        ('pushtrue',), ('returnvalue',),
        ('L', 'no'), ('pushfalse',), ('returnvalue',)], ref)
    # dmHgClip(ch): false when ch is clipped (mask / scrollRect) and the finger is outside the clip. locals 1 ch 2 m 3 r
    rr = lambda f: [('getlocal3',), ('getproperty', P(f))]
    def inside(o, fail):
        return [('getlocal', o), ('getproperty', P('mouseX'))] + rr('x') + [('ifnge', fail),
                ('getlocal', o), ('getproperty', P('mouseX'))] + rr('x') + rr('width') + [('add',), ('ifnle', fail),
                ('getlocal', o), ('getproperty', P('mouseY'))] + rr('y') + [('ifnge', fail),
                ('getlocal', o), ('getproperty', P('mouseY'))] + rr('y') + rr('height') + [('add',), ('ifnle', fail)]
    c.add('dmHgClip', 1, HEAD + obj(2) + obj(3) + [
        ('getlocal1',), ('getproperty', P('mask')), ('coerce_a',), ('setlocal2',),
        ('getlocal2',), ('iffalse', 'sr'),
        ('getlocal2',), ('getlocal2',), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'sr')] + inside(2, 'no') + [
        ('L', 'sr'),
        ('getlocal1',), ('getproperty', P('scrollRect')), ('coerce_a',), ('setlocal3',),
        ('getlocal3',), ('iffalse', 'yes')] + inside(1, 'no') + [
        ('L', 'yes'), ('pushtrue',), ('returnvalue',),
        ('L', 'no'), ('pushfalse',), ('returnvalue',)], ref)
    # dmHgWalk(c, mode): mode 0 = hit test (sets dmHgFound), 1 = plates. locals 1 c 2 mode 3 n 4 i 5 ch 6 ic 7 el
    c.add('dmHgWalk', 2, HEAD + num(3) + num(4) + obj(5) + obj(6) + obj(7) + [
        ('getlocal1',), ('getproperty', P('numChildren')), ('convert_d',), ('setlocal3',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal1',), ('getlocal', 4), ('callproperty', P('getChildAt'), 1), ('coerce_a',), ('setlocal', 5),
        ('getlocal', 5), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('alpha')), ('pushbyte', 0), ('ifngt', 'next'),
        ('getlocal2',), ('iftrue', 'noclip'),
        ('getlocal0',), ('getlocal', 5), ('callproperty', P('dmHgClip'), 1), ('iffalse', 'next'),
        ('L', 'noclip'),
        ('getlocal', 5), ('getlex', HINT), ('istypelate',), ('iffalse', 'cont'),
        ('getlocal', 5), ('getproperty', P('icon_mc')), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'next'),
        ('getlocal', 6), ('getproperty', P('tuBtn')), ('pushundefined',), ('ifstricteq', 'next'),
        ('getlocal0',), ('getlocal', 5), ('callproperty', P('dmHgEl'), 1), ('coerce_a',), ('setlocal', 7),
        ('getlocal2',), ('iftrue', 'plate'),
        ('getlocal0',), ('getlocal', 7), ('getlocal0',), ('getlocal', 7), ('getlocal', 5), ('callproperty', P('dmHgBox'), 2), ('callproperty', P('dmHgIn'), 2), ('iffalse', 'next'),
        ('getlocal0',), ('getlocal', 5), ('setproperty', P('dmHgFound')), ('jump', 'next'),
        ('L', 'plate'), ('getlocal0',), ('getlocal', 7), ('getlocal', 5), ('callpropvoid', P('dmHgPlate'), 2), ('jump', 'next'),
        ('L', 'cont'),
        ('getlocal', 5), ('getlex', DOC), ('istypelate',), ('iffalse', 'next'),
        ('getlocal0',), ('getlocal', 5), ('getlocal2',), ('callpropvoid', P('dmHgWalk'), 2),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('returnvoid',)], ref)
    # dmHgPlate(el, h). locals 1 el 2 h 3 box 4 g 5 ic 6 x0 7 y0 8 w 9 hh 10 t
    def rect(x, y, w, h): return [('getlocal', 4)] + x + y + w + h + [('callpropvoid', P('drawRect'), 4)]
    L = lambda n: [('getlocal', n)]
    fill = lambda col: [('getlocal', 4), ('pushint', c.icon(col)), ('pushbyte', 9), ('pushbyte', 10), ('divide',), ('callpropvoid', P('beginFill'), 2)]
    endf = [('getlocal', 4), ('callpropvoid', P('endFill'), 0)]
    same = []
    for k, (loc, nm) in enumerate(((6, 'dmP0'), (7, 'dmP1'), (8, 'dmP2'), (9, 'dmP3'))):
        same += [('getlocal', 5), ('getproperty', P(nm)), ('getlocal', loc), ('ifne', 'draw')]
    store = []
    for loc, nm in ((6, 'dmP0'), (7, 'dmP1'), (8, 'dmP2'), (9, 'dmP3')): store += [('getlocal', 5), ('getlocal', loc), ('setproperty', P(nm))]
    c.add('dmHgPlate', 2, HEAD + obj(3) + obj(4) + obj(5) + num(6) + num(7) + num(8) + num(9) + num(10) + [
        ('getlocal0',), ('getlocal1',), ('getlocal2',), ('callproperty', P('dmHgBox'), 2), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end'),
        ('getlocal2',), ('getproperty', P('icon_mc')), ('coerce_a',), ('setlocal', 5), ('getlocal', 5), ('iffalse', 'end')] +
        A(3, 0) + [('convert_d',), ('setlocal', 6)] + A(3, 1) + [('convert_d',), ('setlocal', 7)] +
        A(3, 2) + L(6) + [('subtract',), ('convert_d',), ('setlocal', 8)] + A(3, 3) + L(7) + [('subtract',), ('convert_d',), ('setlocal', 9)] +
        same + [('jump', 'end'), ('L', 'draw')] + store + [
        ('getlocal1',), ('getproperty', P('graphics')), ('coerce_a',), ('setlocal', 4), ('getlocal', 4), ('iffalse', 'end'),
        ('getlocal', 4), ('callpropvoid', P('clear'), 0),
        ('getlocal1',), ('getlocal2',), ('ifstrictne', 'full'),
        ('getlocal', 9), ('pushbyte', 12), ('divide',), ('convert_d',), ('setlocal', 10)] + fill(GOLD) +
        rect(L(6), L(7), L(8), L(10)) + rect(L(6), L(7) + L(9) + [('add',)] + L(10) + [('subtract',)], L(8), L(10)) +
        rect(L(6), L(7), L(10), L(9)) + rect(L(6) + L(8) + [('add',)] + L(10) + [('subtract',)], L(7), L(10), L(9)) + endf + [('returnvoid',),
        ('L', 'full')] + fill(GOLD) + rect(L(6), L(7), L(8), L(9)) + endf + fill(DARK) +
        rect(L(6) + [('pushbyte', 2), ('add',)], L(7) + [('pushbyte', 2), ('add',)], L(8) + [('pushbyte', 4), ('subtract',)], L(9) + [('pushbyte', 4), ('subtract',)]) + endf + [
        ('L', 'end'), ('returnvoid',)], ref)
    # dmHgFind(): the hint under the finger or null
    c.add('dmHgFind', 0, HEAD + [
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmHgFound')),
        ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal0',), ('getlocal0',), ('pushbyte', 0), ('callpropvoid', P('dmHgWalk'), 2),
        ('getlocal0',), ('getproperty', P('dmHgFound')), ('returnvalue',),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], ref)
    # dmHgDown(e) / dmHgUp(e). locals 1 e 2 h 3 el 4 ic
    c.add('dmHgDown', 1, HEAD + obj(2) + obj(3) + obj(4) + [
        ('getlocal0',), ('callproperty', P('dmHgFind'), 0), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('getlocal2',), ('setproperty', P('dmHgH')),
        ('getlocal2',), ('iffalse', 'end'),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmHgT')),
        ] + ([('getlocal0',), ('pushnull',), ('setproperty', P('tcEv'))] if abc.trait(c.ci, 'tcPad') is not None else []) + [   # cc: the row under the hint does not also get "accept"
        ('getlocal2',), ('getproperty', P('icon_mc')), ('coerce_a',), ('setlocal', 4),
        ('getlocal', 4), ('iffalse', 'nt0')] + tut_off(4) + [('L', 'nt0'),   # disarm the element listener of tools/dos2_hint_touch.py (guarded)
        ('getlocal0',), ('getlocal2',), ('callproperty', P('dmHgEl'), 1), ('coerce_a',), ('setlocal3',),
        ('getlocal', 4), ('getlocal3',), ('getproperty', P('alpha')), ('setproperty', P('dmA')),
        ('getlocal3',), ('pushbyte', 6), ('pushbyte', 10), ('divide',), ('setproperty', P('alpha')),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmHgUp', 1, HEAD + obj(2) + obj(3) + obj(4) + [
        ('getlocal0',), ('getproperty', P('dmHgH')), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmHgH')),
        ('getlocal2',), ('iffalse', 'end'),
        ('getlocal2',), ('getproperty', P('icon_mc')), ('coerce_a',), ('setlocal', 4),
        ('getlocal', 4), ('iffalse', 'nt1')] + tut_off(4) + [('L', 'nt1'),
        ('getlocal0',), ('getlocal2',), ('callproperty', P('dmHgEl'), 1), ('coerce_a',), ('setlocal3',),
        ('getlocal3',), ('getlocal', 4), ('getproperty', P('dmA')), ('setproperty', P('alpha')),
        ] + c.now() + [('getlocal0',), ('getproperty', P('dmHgT')), ('subtract',), ('pushshort', TAP_MS), ('ifgt', 'end'),
        ('getlocal0',), ('callproperty', P('dmHgFind'), 0), ('getlocal2',), ('ifstrictne', 'end'),
        ] + c.now() + [('getlocal0',), ('getproperty', P('dmHgF')), ('subtract',), ('pushshort', AGAIN_MS), ('iflt', 'end'),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmHgF')),
        ] + c.ei(2, ('pushstring', S('dmPadBtn')), ('getlocal', 4), ('getproperty', P('tuBtn'))) + [
        ('L', 'end'), ('returnvoid',)], ref)
    # dmHgTick(e): plates every PLATE_FRAMES frames; (re)tries the stage listeners
    c.add('dmHgTick', 1, HEAD + num(2) + [
        ('getlocal0',), ('callpropvoid', P('dmHgStage'), 0),
        ('getlocal0',), ('getproperty', P('dmHgN')), ('pushbyte', 1), ('add',), ('convert_d',), ('setlocal2',),
        ('getlocal0',), ('getlocal2',), ('setproperty', P('dmHgN')),
        ('getlocal2',), ('pushbyte', PLATE_FRAMES), ('iflt', 'end'),
        ('getlocal0',), ('pushbyte', 0), ('setproperty', P('dmHgN')),
        ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal0',), ('pushbyte', 1), ('callpropvoid', P('dmHgWalk'), 2),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmHgStage', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmHgOn')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmHgOn')),
        ('getlocal1',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmHgDown')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal1',), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmHgUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)], ref)
    return ['dmHgStage', 'dmHgTick']


def hook_init(c, keep, calls, ticks, first=(), early=False):
    """onEventInit -> first() + original (kept as <keep>) + calls(); an enterFrame listener per tick method, added once.
    early: calls and tick listeners BEFORE the original body (a throw in it cannot keep them out; mainMenu_d / optionsSettings_d, pack 3c)"""
    P, S = c.P, c.S
    flag = keep + 'On'
    orig = [('getlocal0',), ('callpropvoid', P(keep), 0)]
    mine = sum(([('getlocal0',), ('callpropvoid', P(m), 0)] for m in calls), [])
    once = [('getlocal0',), ('getproperty', P(flag)), ('iftrue', 'end'), ('getlocal0',), ('pushtrue',), ('setproperty', P(flag))] + sum(
        ([('getlocal0',), ('pushstring', S('enterFrame')), ('getlocal0',), ('getproperty', P(t)), ('callpropvoid', P('addEventListener'), 2)] for t in ticks), []) + [
        ('L', 'end')]
    pre = sum(([('getlocal0',), ('callpropvoid', P(m), 0)] for m in first), [])
    body = (lambda n: pre + mine + once + orig + [('returnvoid',)]) if early else (lambda n: pre + orig + mine + once + [('returnvoid',)])
    c.wrap('onEventInit', keep, body)


def patch_hints_only(data):
    c = doc_ctx(data); st = add_hints(c)
    hook_init(c, 'dmHgInitOrig', [st[0]], [st[1]])
    return c


# ---------------------------------------------------------------- 2. options rows and sliders
def add_options(c):
    P, S, abc, ref = c.P, c.S, c.abc, c.rbody
    assert abc.mname(c.inst['name']) == 'optionsSettings_c_fla.MainTimeline'
    ov = abc.class_index('optionsSettings_c_fla.overview_mc_1'); assert abc.trait(ov, 'list') is not None
    SL, SE, MB = abc.pub('SliderComp'), abc.pub('SelectorMC'), abc.pub('Menu_button')
    for cls, ns in (('SliderComp', ('slider_mc', 'onChange', 'onhandleUp')), ('SelectorMC', ('selList',)), ('Menu_button', ('buttonPressed', 'buttonReleased'))):
        k = abc.class_index(cls)
        for n in ns: assert abc.trait(k, n) is not None, cls + '.' + n
    ls = abc.class_index('LS_Classes.LSSlider')
    for n in ('value', 'minimum', 'maximum', 'snapInterval', 'm_bg_mc', 'disabled'): assert abc.trait(ls, n) is not None, 'LSSlider.' + n
    for n in ('menuSliderID', 'sliderHandleUp', 'buttonPressed', 'UI_Generic_Click'): assert n.encode() in abc.strings, n
    # dmOsRow(): the row under the finger or null. locals 1 list 2 arr 3 n 4 i 5 el 6 r
    rr = lambda f: [('getlocal', 6), ('getproperty', P(f))]
    c.add('dmOsRow', 0, HEAD + obj(1) + obj(2) + num(3) + num(4) + obj(5) + obj(6) + [
        ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal0',), ('getproperty', P('mainMenu_mc')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('list')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('visible')), ('iffalse', 'none'),
        ('getlocal0',), ('getlocal1',), ('callproperty', P('dmOsClip'), 1), ('iffalse', 'none'),
        ('getlocal1',), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'none'),
        ('getlocal2',), ('getproperty', P('length')), ('convert_d',), ('setlocal3',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal2',), ('getlocal', 4), ('getproperty', c.late), ('coerce_a',), ('setlocal', 5),
        ('getlocal', 5), ('iffalse', 'next'), ('getlocal', 5), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 5), ('getlocal', 5), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseY'))] + rr('y') + [('ifnge', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseY'))] + rr('y') + rr('height') + [('add',), ('ifnle', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseX'))] + rr('x') + [('ifnge', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseX'))] + rr('x') + rr('width') + [('add',), ('ifnle', 'next'),
        ('getlocal', 5), ('returnvalue',),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], ref)
    # dmOsClip(o): the finger inside the list's visible window (its scrollRect / mask, if any) - rows scrolled out are not hit
    c.add('dmOsClip', 1, HEAD + obj(2) + [
        ('getlocal0',), ('getlocal1',), ('callproperty', P('dmHgClip'), 1), ('iffalse', 'no'),
        ('getlocal1',), ('getproperty', P('containerContent_mc')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'yes'),
        ('getlocal0',), ('getlocal2',), ('callproperty', P('dmHgClip'), 1), ('iffalse', 'no'),
        ('L', 'yes'), ('pushtrue',), ('returnvalue',),
        ('L', 'no'), ('pushfalse',), ('returnvalue',)], ref)
    # dmOsSlide(row): value from the finger over the slider's bar; the stock onChange only when the snapped value changes.
    # locals 1 row 2 s 3 bg 4 r 5 f 6 v 7 min 8 max 9 step
    c.add('dmOsSlide', 1, HEAD + obj(2) + obj(3) + obj(4) + num(5) + num(6) + num(7) + num(8) + num(9) + [
        ('getlocal1',), ('getproperty', P('slider_mc')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'end'),
        ('getlocal2',), ('getproperty', P('disabled')), ('iftrue', 'end'),
        ('getlocal2',), ('getproperty', P('m_bg_mc')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end'),
        ('getlocal3',), ('getlocal3',), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 4), ('getlocal', 4), ('iffalse', 'end'),
        ('getlocal', 4), ('getproperty', P('width')), ('pushbyte', 1), ('ifngt', 'end'),
        ('getlocal3',), ('getproperty', P('mouseX')), ('getlocal', 4), ('getproperty', P('x')), ('subtract',), ('getlocal', 4), ('getproperty', P('width')), ('divide',), ('convert_d',), ('setlocal', 5),
        ('getlocal', 5), ('pushbyte', 0), ('ifnlt', 'lo'), ('pushbyte', 0), ('convert_d',), ('setlocal', 5), ('L', 'lo'),
        ('getlocal', 5), ('pushbyte', 1), ('ifngt', 'hi'), ('pushbyte', 1), ('convert_d',), ('setlocal', 5), ('L', 'hi'),
        ('getlocal2',), ('getproperty', P('minimum')), ('convert_d',), ('setlocal', 7),
        ('getlocal2',), ('getproperty', P('maximum')), ('convert_d',), ('setlocal', 8),
        ('getlocal', 7), ('getlocal', 8), ('getlocal', 7), ('subtract',), ('getlocal', 5), ('multiply',), ('add',), ('convert_d',), ('setlocal', 6),
        ('getlocal2',), ('getproperty', P('snapInterval')), ('convert_d',), ('setlocal', 9),
        ('getlocal', 9), ('pushbyte', 0), ('ifngt', 'set'),                       # snap: min + round((v - min) / step) * step
        ('getlocal', 7), ('getlex', abc.pub('Math')), ('getlocal', 6), ('getlocal', 7), ('subtract',), ('getlocal', 9), ('divide',), ('callproperty', P('round'), 1),
        ('getlocal', 9), ('multiply',), ('add',), ('convert_d',), ('setlocal', 6),
        ('getlocal', 6), ('getlocal', 8), ('ifngt', 'set'), ('getlocal', 8), ('setlocal', 6),
        ('L', 'set'),
        ('getlocal2',), ('getproperty', P('value')), ('getlocal', 6), ('ifeq', 'end'),
        ('getlocal2',), ('getlocal', 6), ('setproperty', P('value')),
        ('getlocal1',), ('pushnull',), ('callpropvoid', P('onChange'), 1),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmOsMoved')),
        ('L', 'end'), ('returnvoid',)], ref)
    # dmOsDown(e) / dmOsMove(e) / dmOsUp(e). locals 1 e 2 row 3 list
    c.add('dmOsDown', 1, HEAD + obj(2) + obj(3) + [
        ('getlocal0',), ('callproperty', P('dmOsRow'), 0), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('getlocal2',), ('setproperty', P('dmOsR')),
        ('getlocal2',), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('mouseY')), ('setproperty', P('dmOsY')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmOsMoved')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmOsDrag')),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmOsScr')),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('mouseY')), ('setproperty', P('dmOsY0')),
        ('getlocal0',), ('getproperty', P('mainMenu_mc')), ('getproperty', P('list')), ('coerce_a',), ('setlocal3',),
        ('getlocal3',), ('callproperty', P('getCurrentMovieClip'), 0), ('getlocal2',), ('ifstricteq', 'sel'),
        ('getlocal3',), ('getlocal2',), ('pushfalse',), ('callpropvoid', P('selectMC'), 2),   # the stock selection (pad look, tooltip)
        ('L', 'sel'),
        ('getlocal2',), ('getlex', SL), ('istypelate',), ('iffalse', 'end'),
        ('getlocal2',), ('getproperty', P('slider_mc')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end'),
        ('getlocal3',), ('getproperty', P('mouseY')), ('pushbyte', -24), ('ifnge', 'end'),   # on the bar's band (slider_mc space)
        ('getlocal3',), ('getproperty', P('mouseY')), ('getlocal3',), ('getproperty', P('height')), ('pushbyte', 24), ('add',), ('ifnle', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmOsDrag')),
        ('getlocal0',), ('getlocal2',), ('callpropvoid', P('dmOsSlide'), 1),
        ('L', 'end'), ('returnvoid',)], ref)
    # a drag over the rows moves the stock selection one row per ROW_PX (finger up = next), the list scrolls to it as with the pad
    lst = [('getlocal0',), ('getproperty', P('mainMenu_mc')), ('getproperty', P('list'))]
    c.add('dmOsMove', 1, HEAD + obj(2) + num(3) + [
        ('getlocal0',), ('getproperty', P('dmOsR')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('dmOsDrag')), ('iffalse', 'rows'),
        ('getlocal0',), ('getlocal2',), ('callpropvoid', P('dmOsSlide'), 1), ('returnvoid',),
        ('L', 'rows'),
        ('getlocal0',), ('getproperty', P('mouseY')), ('getlocal0',), ('getproperty', P('dmOsY')), ('subtract',), ('convert_d',), ('setlocal3',),
        ('getlocal3',), ('pushbyte', -ROW_PX), ('ifnle', 'down')] + lst + [('callpropvoid', P('next'), 0),
        ('getlocal0',), ('getproperty', P('dmOsY')), ('pushbyte', ROW_PX), ('subtract',), ('jump', 'moved'),
        ('L', 'down'),
        ('getlocal3',), ('pushbyte', ROW_PX), ('ifnge', 'end')] + lst + [('callpropvoid', P('previous'), 0),
        ('getlocal0',), ('getproperty', P('dmOsY')), ('pushbyte', ROW_PX), ('add',),
        ('L', 'moved'), ('convert_d',), ('setlocal3',), ('getlocal0',), ('getlocal3',), ('setproperty', P('dmOsY')),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmOsScr')),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmOsUp', 1, HEAD + obj(2) + num(3) + [
        ('getlocal0',), ('getproperty', P('dmOsR')), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmOsR')),
        ('getlocal2',), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('dmOsDrag')), ('iffalse', 'nodrag'),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmOsDrag')),
        ('getlocal0',), ('getlocal2',), ('callpropvoid', P('dmOsSlide'), 1),
        ('getlocal2',), ('pushnull',), ('callpropvoid', P('onhandleUp'), 1), ('returnvoid',),     # the stock release of the slider handle
        ('L', 'nodrag'),
        ('getlocal0',), ('getproperty', P('dmOsScr')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('mouseY')), ('getlocal0',), ('getproperty', P('dmOsY0')), ('subtract',), ('convert_d',), ('setlocal3',),
        ('getlocal3',), ('pushbyte', DRAG_PX), ('ifgt', 'end'), ('getlocal3',), ('pushbyte', -DRAG_PX), ('iflt', 'end'),
        ('getlocal0',), ('callproperty', P('dmOsRow'), 0), ('getlocal2',), ('ifstrictne', 'end'),
        ('getlocal0',), ('callproperty', P('dmHgFind'), 0), ('iftrue', 'end'),            # a hint over the row has its own tap
        ('getlocal2',), ('getlex', SE), ('istypelate',), ('iffalse', 'btn'),
        ('getlocal2',), ('getproperty', P('enabled')), ('pushfalse',), ('ifstricteq', 'end'),
        ('getlocal2',), ('getproperty', P('selList')), ('iffalse', 'end'),
        ('getlocal2',), ('getproperty', P('mouseX')), ('getlocal2',), ('getproperty', P('width')), ('pushbyte', 2), ('divide',), ('ifnlt', 'right'),
        ('getlocal2',), ('getproperty', P('selList')), ('callpropvoid', P('previous'), 0), ('jump', 'snd'),
        ('L', 'right'), ('getlocal2',), ('getproperty', P('selList')), ('callpropvoid', P('next'), 0),
        ('L', 'snd')] + c.ei(2, ('pushstring', S('PlaySound')), ('pushstring', S('UI_Generic_Click'))) + [('returnvoid',),
        ('L', 'btn'),
        ('getlocal2',), ('getlex', MB), ('istypelate',), ('iffalse', 'end'),
        ('getlocal2',), ('pushnull',), ('callpropvoid', P('buttonPressed'), 1),
        ('getlocal2',), ('pushnull',), ('callpropvoid', P('buttonReleased'), 1),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmOsStage', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmOsOn')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmOsOn')),
        ('getlocal1',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmOsDown')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal1',), ('pushstring', S('mouseMove')), ('getlocal0',), ('getproperty', P('dmOsMove')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal1',), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmOsUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmOsTick', 1, HEAD + [('getlocal0',), ('callpropvoid', P('dmOsStage'), 0), ('returnvoid',)], ref)
    return ['dmOsStage', 'dmOsTick']


def patch_options(data):
    c = doc_ctx(data); st = add_hints(c); so = add_options(c); hb = add_hintbar(c, ('mainMenu_mc', 'buttonHint_mc'))
    hook_init(c, 'dmHgInitOrig', [st[0], so[0], hb[0]], [st[1], so[1], hb[1]], early=True)
    return c


# ---------------------------------------------------------------- 5. the bottom hint bar by its own list (options, main menu) + the title splash
def add_hintbar(c, chain):
    """The bottom hint bar (mainMenu_mc.<bar>, an LS_Classes.controllerContainer: "(X) Default (B) Cancel (A) Accept") by its own element list.
    Owner (pack 3b): the options hint bar never answered a tap although the sliders did - the part-1 walk never reached a dmPadBtn there.
    v1 (pack 3c, run tw6) used element.hitTestPoint(stage.mouseX, stage.mouseY +- 24) and did not fire either; the taps landed 25..35 px above
    the bar's centre. v2: the options rows' way (they work in the same movie): every element's box in its own space (element.getBounds(element)) against
    element.mouseX / mouseY; x range +- HB_PADX, the row generous: top - HB_PADY .. bottom + HB_PADY. The bar is this.<chain>
    (options mainMenu_mc.buttonHint_mc, main menu mainMenu_mc.buttonHintBar_mc, character creation btnHints_mc.btnHints). Pad code = controllerBtnElement.iconId (written by the stock setBtnHint). Press and release on the same
    element within TAP_MS = EI dmPadBtn(iconId). Shares part 1's stamp dmHgF (no double press).
    Trace (first HB_TRACES presses): EI dmPadBtn with codes that are no pad button, logged by the host as `dmPadBtn(<n>): no pad button`:
    HB_TRACE codes, then for a miss the geometry in the bar's own space: 20000 + finger y, 30000 + bar top, 40000 + bar bottom,
    50000 + finger x, 60000 + bar left, 70000 + bar right."""
    P, S, abc, ref = c.P, c.S, c.abc, c.rbody
    tut_off = lambda loc: tut_off_ops(c, loc)
    hc = abc.class_index('LS_Classes.controllerBtnElement')
    for n in ('iconId', 'icon_mc'): assert abc.trait(hc, n) is not None, 'controllerBtnElement.' + n
    assert abc.trait(abc.class_index('LS_Classes.listDisplay'), 'content_array') is not None and c.late is not None
    T = lambda code: c.ei(2, ('pushstring', S('dmPadBtn')), ('pushshort', code))
    MATH = abc.pub('Math')
    rnd = lambda val: [('getlex', MATH)] + val + [('callproperty', P('round'), 1)]
    tv = lambda base, val: [('getlex', c.EI), ('pushstring', S('dmPadBtn')), ('pushint', c.icon(base))] + rnd(val) + [('add',), ('callpropvoid', P('call'), 2)]
    # dmHbBar(): the bar MovieClip (this.<chain...>) when it and every holder on the way are on screen, else null. locals 1 mc
    step = lambda name: [('getlocal1',), ('getproperty', P(name)), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'none'),
                         ('getlocal1',), ('getproperty', P('visible')), ('iffalse', 'none'),
                         ('getlocal1',), ('getproperty', P('alpha')), ('pushbyte', 0), ('ifngt', 'none')]
    c.add('dmHbBar', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'none'), ('getlocal0',), ('coerce_a',), ('setlocal1',)] + sum((step(n) for n in chain), []) + [
        ('getlocal1',), ('returnvalue',),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], ref)
    # dmHbFind(): the hint element under the finger. locals 1 bar 2 arr 3 n 4 i 5 el 6 stage 7 x 8 y 9 r
    r = lambda f: [('getlocal', 9), ('getproperty', P(f))]
    c.add('dmHbFind', 0, HEAD + obj(1) + obj(2) + num(3) + num(4) + obj(5) + obj(6) + num(7) + num(8) + obj(9) + [
        ('getlocal0',), ('callproperty', P('dmHbBar'), 0), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'none'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'none'),
        ('getlocal', 6), ('getproperty', P('mouseX')), ('convert_d',), ('setlocal', 7),
        ('getlocal', 6), ('getproperty', P('mouseY')), ('convert_d',), ('setlocal', 8),
        ('getlocal1',), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'none'),
        ('getlocal2',), ('getproperty', P('length')), ('convert_d',), ('setlocal3',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal2',), ('getlocal', 4), ('getproperty', c.late), ('coerce_a',), ('setlocal', 5),
        ('getlocal', 5), ('iffalse', 'next'), ('getlocal', 5), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 5), ('getlocal', 5), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 9), ('getlocal', 9), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseX'))] + r('x') + [('pushbyte', HB_PADX), ('subtract',), ('ifnge', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseX'))] + r('x') + r('width') + [('add',), ('pushbyte', HB_PADX), ('add',), ('ifnle', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseY'))] + r('y') + [('pushbyte', HB_PADY), ('subtract',), ('ifnge', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseY'))] + r('y') + r('height') + [('add',), ('pushbyte', HB_PADY), ('add',), ('ifnle', 'next'),
        ('getlocal', 5), ('returnvalue',),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], ref)
    # dmHbTr(): may this press still be traced (first HB_TRACES)
    c.add('dmHbTr', 0, HEAD + [
        ('getlocal0',), ('getproperty', P('dmHbN')), ('pushbyte', HB_TRACES), ('ifge', 'no'),
        ('pushtrue',), ('returnvalue',), ('L', 'no'), ('pushfalse',), ('returnvalue',)], ref)
    # dmHbDown(e). locals 1 e 2 bar 3 r 4 st
    rr = lambda f: [('getlocal3',), ('getproperty', P(f))]
    c.add('dmHbDown', 1, HEAD + obj(2) + obj(3) + obj(4) + [
        ('getlocal0',), ('getlocal0',), ('callproperty', P('dmHbFind'), 0), ('setproperty', P('dmHbE')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmHbT')),
        ('getlocal0',), ('callproperty', P('dmHbTr'), 0), ('iffalse', 'end'),
        ('getlocal0',), ('callproperty', P('dmHbBar'), 0), ('coerce_a',), ('setlocal2',),
        ('getlocal2',), ('iftrue', 'bar')] + T(HB_TRACE['press: no bar on screen']) + [('jump', 'cnt'),
        ('L', 'bar'),
        ('getlocal0',), ('getproperty', P('dmHbE')), ('iffalse', 'miss')] + T(HB_TRACE['press on a hint']) + [('jump', 'cnt'),
        ('L', 'miss')] + T(HB_TRACE['press: bar up, no hint under the finger']) + [
        ('getlocal2',), ('getlocal2',), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal3',)] +
        tv(20000, [('getlocal2',), ('getproperty', P('mouseY'))]) + tv(30000, rr('y')) + tv(40000, rr('y') + rr('height') + [('add',)]) +
        tv(50000, [('getlocal2',), ('getproperty', P('mouseX'))]) + tv(60000, rr('x')) + tv(70000, rr('x') + rr('width') + [('add',)]) + [
        ('L', 'cnt'),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmHbN')), ('pushbyte', 1), ('add',), ('setproperty', P('dmHbN')),
        ('L', 'end'), ('returnvoid',)], ref)
    # dmHbUp(e). locals 1 e 2 el 3 icon 4 tr
    why = lambda k: [('getlocal', 4), ('iffalse', 'end')] + T(HB_TRACE[k]) + [('jump', 'end')]
    c.add('dmHbUp', 1, HEAD + obj(2) + obj(3) + [('pushfalse',), ('setlocal', 4),
        ('getlocal0',), ('getproperty', P('dmHbE')), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmHbE')),
        ('getlocal2',), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('dmHbN')), ('pushbyte', HB_TRACES + 1), ('ifgt', 'go'), ('pushtrue',), ('setlocal', 4),
        ('L', 'go'),
        ] + c.now() + [('getlocal0',), ('getproperty', P('dmHbT')), ('subtract',), ('pushshort', TAP_MS), ('ifngt', 'k1')] + why('release: too late') + [
        ('L', 'k1'), ('getlocal0',), ('callproperty', P('dmHbFind'), 0), ('getlocal2',), ('ifstricteq', 'k2')] + why('release: off the pressed hint') + [
        ('L', 'k2')] + c.now() + [('getlocal0',), ('getproperty', P('dmHgF')), ('subtract',), ('pushshort', AGAIN_MS), ('ifnlt', 'k3')] + why('release: part 1 just pressed') + [
        ('L', 'k3'), ('getlocal2',), ('getproperty', P('iconId')), ('pushundefined',), ('ifstrictne', 'k4')] + why('release: no iconId') + [
        ('L', 'k4'),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmHgF')),
        ('getlocal', 4), ('iffalse', 'fire')] + T(HB_TRACE['release: pad button']) + [
        ('L', 'fire'), ('getlocal0',)] + c.now() + [('setproperty', P('tuPadA')),   # the movie's accept guard lets this A through (dos2_hint_touch / mainmenu PADA_MS)
        ] + c.ei(3, ('pushstring', S('dmPadBtn')), ('getlocal2',), ('getproperty', P('iconId')), ('pushbyte', HB_A_MS)) + [
        # pack 3d run t3d: the options bar's icon_mc is a sealed class without tuT -> the old unguarded write threw a ReferenceError
        # before the press (dmHgF stamped, no dmPadBtn). Now: press first, then disarm the old listener only where the property exists.
        ('getlocal2',), ('getproperty', P('icon_mc')), ('coerce_a',), ('setlocal3',), ('getlocal3',), ('iffalse', 'end')] + tut_off(3) + [
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmHbStage', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmHbOn')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmHbOn')),
        ('getlocal1',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmHbDown')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal1',), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmHbUp')), ('callpropvoid', P('addEventListener'), 2),
        ] + T(HB_TRACE['stage listeners added']) + [
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmHbTick', 1, HEAD + [('getlocal0',), ('callpropvoid', P('dmHbStage'), 0), ('returnvoid',)], ref)
    return ['dmHbStage', 'dmHbTick']


def add_splash(c):
    """mainMenu_d title screen "Press (A) to start" (owner, pack 3b run tw5: taps reached the movie, the splash never advanced). The splash is
    closed by the movie's onEventUp on IE UIAccept (splashScreen_mc visible -> closeSplashScreen) and the game itself waits for that A; a tap is
    no UIAccept any more (tools/dos2_mainmenu_touch.py / dos2_hint_touch.py guards, the route's tap-is-not-accept rule) and the splash text is no
    controllerBtnHint, so nothing pressed A. Now: a finger down anywhere while the splash is up (splashScreen_mc visible, not closingSplash)
    = EI dmPadBtn(2) = the pad's A (the host presses it 750 ms later, past the movie's 700 ms guard): the stock pad path closes the splash and the
    game goes on. At most one A per AGAIN_SPLASH_MS. The menu list does not also take the tap (its touch code ignores a press made while the
    splash is up: tuActive). The P2 "press to join" text (setPressToJoin) has no meaning on a phone: not tappable.
    Pack 3c (run tw6): no `pad button` line at all -> v2: the stage listener is hung from every entry the movie has (frame1, onEventInit
    BEFORE the original body, an enterFrame tick that retries until the stage exists), the state test is only "visible and not closing", and
    every decision is traced through the host's own log of dmPadBtn with a code that is no pad button (`[touch] dmPadBtn(9xx): no pad button
    for that hint code`, first 40 lines of that kind): SP_TRACE."""
    P, S, abc, ref = c.P, c.S, c.abc, c.rbody
    assert abc.mname(c.inst['name']) == 'mainMenu_c_fla.MainTimeline'
    for n in ('splashScreen_mc', 'closingSplash', 'closeSplashScreen', 'frame1'): assert abc.trait(c.ci, n) is not None, n
    T = lambda code: c.ei(2, ('pushstring', S('dmPadBtn')), ('pushshort', code))
    # dmSpWhy(): 0 = the splash is up and waits, else the SP_TRACE code of the reason. locals 1 mc
    c.add('dmSpWhy', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('splashScreen_mc')), ('coerce_a',), ('setlocal1',),
        ('getlocal1',), ('iftrue', 'a'), ('pushshort', SP_TRACE['no splashScreen_mc']), ('returnvalue',),
        ('L', 'a'), ('getlocal1',), ('getproperty', P('visible')), ('iftrue', 'b'), ('pushshort', SP_TRACE['splash hidden']), ('returnvalue',),
        ('L', 'b'), ('getlocal0',), ('getproperty', P('closingSplash')), ('iffalse', 'c'), ('pushshort', SP_TRACE['splash closing']), ('returnvalue',),
        ('L', 'c'), ('pushbyte', 0), ('returnvalue',)], ref)
    # dmSpDown(e): the press itself asks for A (no release needed). locals 1 e 2 why
    c.add('dmSpDown', 1, HEAD + num(2) + [
        ('getlocal0',), ('callproperty', P('dmSpWhy'), 0), ('convert_d',), ('setlocal2',),
        ('getlocal2',), ('pushbyte', 0), ('ifeq', 'on'),
        ('getlocal0',), ('getproperty', P('dmSpN')), ('pushbyte', 6), ('ifge', 'end'),        # off: trace the first 6 refusals only
        ('getlocal0',), ('getlocal0',), ('getproperty', P('dmSpN')), ('pushbyte', 1), ('add',), ('setproperty', P('dmSpN')),
        ] + c.ei(2, ('pushstring', S('dmPadBtn')), ('getlocal2',)) + [('returnvoid',),
        ('L', 'on'),
        ] + c.now() + [('getlocal0',), ('getproperty', P('dmSpF')), ('subtract',), ('pushshort', AGAIN_SPLASH_MS), ('ifnlt', 'fire'),
        ] + T(SP_TRACE['again too soon']) + [('returnvoid',),
        ('L', 'fire'),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmSpF')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmHgF')),
        ] + T(SP_TRACE['tap on the splash: A']) + c.ei(3, ('pushstring', S('dmPadBtn')), ('pushbyte', 2), ('pushbyte', SPLASH_A_MS)) + [   # pack 3f: A in 60 ms, not 750
        ('L', 'end'), ('returnvoid',)], ref)
    # dmSpStage(): the stage listener, once (traced); dmSpTick(e): retries it every frame
    c.add('dmSpStage', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmSpOn')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmSpOn')),
        ('getlocal1',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmSpDown')), ('callpropvoid', P('addEventListener'), 2),
        ] + T(SP_TRACE['stage listener added']) + [
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmSpTick', 1, HEAD + [('getlocal0',), ('callpropvoid', P('dmSpStage'), 0), ('returnvoid',)], ref)
    # dmSpArm(): the enterFrame tick on, once (from frame1 and from onEventInit before its original body)
    c.add('dmSpArm', 0, HEAD + [
        ('getlocal0',), ('getproperty', P('dmSpArmed')), ('iftrue', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmSpArmed')),
        ('getlocal0',), ('pushstring', S('enterFrame')), ('getlocal0',), ('getproperty', P('dmSpTick')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)], ref)   # no EI and no stage work here: frame1 runs inside the constructor; the tick does it
    # frame1 (the timeline's first frame script, run from the constructor): original, then arm
    t = abc.trait(c.ci, 'frame1'); orig = abc.methods[t.b]; assert t.kind & 15 == 1 and not orig.params, 'frame1'
    m = c.add('frame1', 0, HEAD + [('getlocal0',), ('callpropvoid', P('dmSpFrame1Orig'), 0),
                                   ('getlocal0',), ('callpropvoid', P('dmSpArm'), 0), ('returnvoid',)], orig.body, like=orig, trait=False)
    old = t.b; t.b = m; c.inst['traits'].append(Trait(P('dmSpFrame1Orig'), 1, 0, old)); c.moved['dmSpFrame1Orig'] = ('frame1', old)
    return ['dmSpArm', None]


def patch_mainmenu(data):
    c = doc_ctx(data); sp = add_splash(c); st = add_hints(c); hb = add_hintbar(c, ('mainMenu_mc', 'buttonHintBar_mc'))
    hook_init(c, 'dmHgInitOrig', [st[0], hb[0]], [st[1], hb[1]], first=[sp[0]], early=True)
    return c


# ---------------------------------------------------------------- 3. character creation: tap on a step tab = go to that step
def add_cc_tabs(c):
    """tools/dos2_cc_touch.py only knows "a tab left / right of the current one = one step" (tcStep) and tw3 showed no tab switch at all.
    Here, the goal way: release on the tab pressed (its own box by getBounds, live) = goal; an enterFrame tick runs the pad path (the cc tool's
    tcPad: the original onEventDown / onEventUp of IE UICreationTabNext / Prev = EI nextStep / previousStep) one step at a time towards the goal
    until the current tab of header_mc.stepTabs is the goal (waits for the step to land: 400 ms after a change, at most 900 ms; gives up after 4 s).
    If the cc tool's own single step also fires, the goal logic absorbs it (it only ever steps towards the goal)."""
    P, S, abc, ref = c.P, c.S, c.abc, c.rbody
    assert abc.mname(c.inst['name']) == 'characterCreation_c_fla.MainTimeline'
    assert abc.trait(c.ci, 'tcPad') is not None, 'not the characterCreation_d of tools/dos2_cc_touch.py (no tcPad)'
    assert abc.trait(abc.class_index('characterCreation_c_fla.header_mc_2'), 'stepTabs') is not None
    for n in ('IE UICreationTabNext', 'IE UICreationTabPrev'): assert n.encode() in abc.strings, n
    lst = lambda loc: [('getlocal0',), ('getproperty', P('header_mc')), ('coerce_a',), ('setlocal', loc), ('getlocal', loc), ('iffalse', 'none'),
                       ('getlocal', loc), ('getproperty', P('visible')), ('iffalse', 'none'),
                       ('getlocal', loc), ('getproperty', P('stepTabs')), ('coerce_a',), ('setlocal', loc), ('getlocal', loc), ('iffalse', 'none'),
                       ('getlocal', loc), ('getproperty', P('visible')), ('iffalse', 'none')]
    rr = lambda f: [('getlocal', 6), ('getproperty', P(f))]
    # dmTsHit(): the step tab under the finger. locals 1 list 2 arr 3 n 4 i 5 el 6 r
    c.add('dmTsHit', 0, HEAD + obj(1) + obj(2) + num(3) + num(4) + obj(5) + obj(6) + [
        ('getlocal0',), ('getproperty', P('visible')), ('iffalse', 'none')] + lst(1) + [
        ('getlocal1',), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'none'),
        ('getlocal2',), ('getproperty', P('length')), ('convert_d',), ('setlocal3',),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal2',), ('getlocal', 4), ('getproperty', c.late), ('coerce_a',), ('setlocal', 5),
        ('getlocal', 5), ('iffalse', 'next'), ('getlocal', 5), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 5), ('getlocal', 5), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal', 6), ('getlocal', 6), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseX'))] + rr('x') + [('pushbyte', 10), ('subtract',), ('ifnge', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseX'))] + rr('x') + rr('width') + [('add',), ('pushbyte', 10), ('add',), ('ifnle', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseY'))] + rr('y') + [('pushbyte', 20), ('subtract',), ('ifnge', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseY'))] + rr('y') + rr('height') + [('add',), ('pushbyte', 20), ('add',), ('ifnle', 'next'),
        ('getlocal', 5), ('returnvalue',),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal3',), ('iflt', 'loop'),
        ('L', 'none'), ('pushnull',), ('returnvalue',)], ref)
    c.add('dmTsDown', 1, HEAD + [('getlocal0',), ('getlocal0',), ('callproperty', P('dmTsHit'), 0), ('setproperty', P('dmTsP')), ('returnvoid',)], ref)
    # dmTsUp(e). locals 1 e 2 t 3 list
    c.add('dmTsUp', 1, HEAD + obj(2) + obj(3) + [
        ('getlocal0',), ('getproperty', P('dmTsP')), ('coerce_a',), ('setlocal2',),
        ('getlocal0',), ('pushnull',), ('setproperty', P('dmTsP')),
        ('getlocal2',), ('iffalse', 'none'),
        ('getlocal0',), ('callproperty', P('dmTsHit'), 0), ('getlocal2',), ('ifstrictne', 'none')] + lst(3) + [
        ('getlocal0',), ('getlocal2',), ('setproperty', P('dmTsGoal')),
        ('getlocal0',), ('getlocal3',), ('callproperty', P('getCurrentMovieClip'), 0), ('setproperty', P('dmTsCur')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmTsT')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmTsF')),
        ('L', 'none'), ('returnvoid',)], ref)
    # dmTsTick(e). locals 1 e 2 g 3 list 4 cur 5 dt
    c.add('dmTsTick', 1, HEAD + obj(2) + obj(3) + obj(4) + num(5) + [
        ('getlocal0',), ('callpropvoid', P('dmTsStage'), 0),
        ('getlocal0',), ('getproperty', P('dmTsGoal')), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'end')] + lst(3) + [
        ('getlocal3',), ('callproperty', P('getCurrentMovieClip'), 0), ('coerce_a',), ('setlocal', 4),
        ('getlocal', 4), ('iffalse', 'none'), ('getlocal', 4), ('getlocal2',), ('ifstricteq', 'none'),
        ] + c.now() + [('getlocal0',), ('getproperty', P('dmTsT')), ('subtract',), ('pushshort', 4000), ('ifgt', 'none'),
        ] + c.now() + [('getlocal0',), ('getproperty', P('dmTsF')), ('subtract',), ('convert_d',), ('setlocal', 5),
        ('getlocal', 5), ('pushshort', 900), ('ifgt', 'fire'),
        ('getlocal', 5), ('pushshort', 400), ('ifngt', 'end'),
        ('getlocal', 4), ('getlocal0',), ('getproperty', P('dmTsCur')), ('ifstricteq', 'end'),
        ('L', 'fire'),
        ('getlocal0',), ('getlocal', 4), ('setproperty', P('dmTsCur')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmTsF')),
        ('getlocal2',), ('getproperty', P('list_pos')), ('getlocal', 4), ('getproperty', P('list_pos')), ('ifngt', 'prev'),
        ('getlocal0',), ('pushstring', S('IE UICreationTabNext')), ('callpropvoid', P('tcPad'), 1), ('returnvoid',),
        ('L', 'prev'),
        ('getlocal0',), ('pushstring', S('IE UICreationTabPrev')), ('callpropvoid', P('tcPad'), 1), ('returnvoid',),
        ('L', 'none'), ('getlocal0',), ('pushnull',), ('setproperty', P('dmTsGoal')),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmTsStage', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmTsOn')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmTsOn')),
        ('getlocal1',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmTsDown')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal1',), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmTsUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)], ref)
    return ['dmTsStage', 'dmTsTick']


def patch_cc(data):
    c = doc_ctx(data); st = add_hints(c); sc = add_cc_tabs(c); hb = add_hintbar(c, ('btnHints_mc', 'btnHints'))
    hook_init(c, 'dmHgInitOrig', [st[0], sc[0], hb[0]], [st[1], sc[1], hb[1]], early=True)
    return c


# ---------------------------------------------------------------- 4. skills panel: the press belongs to the panel
def patch_skills(data):
    """tw2: every press on inventorySkillPanel (0x3e) was "handled 0" (the skill cells have only CLICK listeners; a press nobody listens to is
    not taken by UI and goes on to the world). A no-op mouseDown listener on mainpanel_mc (the whole panel) makes the press the panel's.
    Tap = select / use of a skill by geometry is NOT done here (open; see docs/dos2/TOUCH_CONSOLE_PLAN.md section 9)."""
    c = doc_ctx(data); P, S, abc, ref = c.P, c.S, c.abc, c.rbody
    assert abc.mname(c.inst['name']) == 'inventorySkillPanel_c_fla.MainTimeline' and abc.trait(c.ci, 'mainpanel_mc') is not None
    c.add('dmSkNop', 1, HEAD + [('returnvoid',)], ref)
    c.add('dmSkHook', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmSkOn')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('mainpanel_mc')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmSkOn')),
        ('getlocal1',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmSkNop')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)], ref)
    c.wrap('onEventInit', 'dmSkInitOrig', lambda n: [('getlocal0',), ('callpropvoid', P('dmSkInitOrig'), 0), ('getlocal0',), ('callpropvoid', P('dmSkHook'), 0), ('returnvoid',)])
    return c


# ---------------------------------------------------------------- 7. Options -> Controls: the "B Back" hint of buttonLayoutNX
BL_PADX, BL_PADY = 160, 40   # px around the "Back" label (close_txt) in its own space: the B icon stands next to it


def add_blback(c):
    """buttonLayoutNX_c (type 0x5f, Options -> Controls: the pad layout picture + "(B) Back"; owner 21.11: its Back took no tap in hybrid or
    touch). It has no LS_Classes.controllerBtnHint (its B is a plain symbol buttonB_1 next to the label close_txt), so parts 1 / 5 find nothing
    there. A stage press and release within TAP_MS, both on close_txt's box (its own getBounds against close_txt.mouseX / mouseY, + BL_PADX
    left / right for the icon, + BL_PADY up / down) = EI dmPadBtn(1): the pad's B (hybrid: the npad B; touch: its keyboard twin escape), which
    the movie's own onEventUp turns into "IE UICancel" -> PlaySound UI_Gen_Back + requestCloseUI, the pad's path."""
    P, S, abc, ref = c.P, c.S, c.abc, c.rbody
    assert abc.mname(c.inst['name']) == 'buttonLayoutNX_c_fla.MainTimeline' and abc.trait(c.ci, 'close_txt') is not None
    r = lambda f: [('getlocal2',), ('getproperty', P(f))]
    c.add('dmBlHit', 0, HEAD + obj(1) + obj(2) + num(3) + num(4) + [
        ('getlocal0',), ('getproperty', P('close_txt')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'no'),
        ('getlocal1',), ('getproperty', P('visible')), ('iffalse', 'no'),
        ('getlocal1',), ('getlocal1',), ('callproperty', P('getBounds'), 1), ('coerce_a',), ('setlocal2',), ('getlocal2',), ('iffalse', 'no'),
        ('getlocal1',), ('getproperty', P('mouseX')), ('convert_d',), ('setlocal3',),
        ('getlocal1',), ('getproperty', P('mouseY')), ('convert_d',), ('setlocal', 4),
        ('getlocal3',)] + r('x') + [('pushshort', BL_PADX), ('subtract',), ('ifnge', 'no'),
        ('getlocal3',)] + r('x') + r('width') + [('add',), ('pushshort', BL_PADX), ('add',), ('ifnle', 'no'),
        ('getlocal', 4)] + r('y') + [('pushbyte', BL_PADY), ('subtract',), ('ifnge', 'no'),
        ('getlocal', 4)] + r('y') + r('height') + [('add',), ('pushbyte', BL_PADY), ('add',), ('ifnle', 'no'),
        ('pushtrue',), ('returnvalue',),
        ('L', 'no'), ('pushfalse',), ('returnvalue',)], ref)
    c.add('dmBlDown', 1, HEAD + [
        ('getlocal0',), ('getlocal0',), ('callproperty', P('dmBlHit'), 0), ('setproperty', P('dmBlP')),
        ('getlocal0',)] + c.now() + [('setproperty', P('dmBlT')), ('returnvoid',)], ref)
    c.add('dmBlUp', 1, HEAD + [
        ('getlocal0',), ('getproperty', P('dmBlP')), ('iffalse', 'end'),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('dmBlP'))] + c.now() + [
        ('getlocal0',), ('getproperty', P('dmBlT')), ('subtract',), ('pushshort', TAP_MS), ('ifngt', 'k1'), ('jump', 'end'),
        ('L', 'k1'), ('getlocal0',), ('callproperty', P('dmBlHit'), 0), ('iffalse', 'end')] + c.ei(2, ('pushstring', S('dmPadBtn')), ('pushbyte', 1)) + [
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmBlStage', 0, HEAD + obj(1) + [
        ('getlocal0',), ('getproperty', P('dmBlOn')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal1',), ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('dmBlOn')),
        ('getlocal1',), ('pushstring', S('mouseDown')), ('getlocal0',), ('getproperty', P('dmBlDown')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal1',), ('pushstring', S('mouseUp')), ('getlocal0',), ('getproperty', P('dmBlUp')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)], ref)
    c.add('dmBlTick', 1, HEAD + [('getlocal0',), ('callpropvoid', P('dmBlStage'), 0), ('returnvoid',)], ref)
    return ['dmBlStage', 'dmBlTick']


def patch_bl(data):
    c = doc_ctx(data); st = add_blback(c)
    hook_init(c, 'dmBlInitOrig', [st[0]], [st[1]], early=True)
    return c


KINDS = {'inventorySkillPanel_d.swf': (patch_skills, ["'mouseDown'", 'mainpanel_mc', 'dmSkNop']),
         'saveLoad_d.swf': (patch_hints_only, ["'dmPadBtn'", 'dmHgWalk', 'istypelate', 'drawRect', "'enterFrame'"]),
         'optionsSettings_d.swf': (patch_options, ["'dmPadBtn'", 'dmHgWalk', 'onChange', 'onhandleUp', 'selectMC', 'previous', 'buttonReleased', "'mouseMove'",
                                                   'dmHbFind', 'getBounds', 'buttonHint_mc', 'iconId']),
         'mods_d.swf': (patch_hints_only, ["'dmPadBtn'", 'dmHgWalk', 'drawRect']),
         'mainMenu_d.swf': (patch_mainmenu, ["'dmPadBtn'", 'dmHgWalk', 'drawRect', 'dmHbFind', 'getBounds', 'buttonHintBar_mc', 'iconId',
                                             'dmSpWhy', 'dmSpFrame1Orig', 'splashScreen_mc', 'closingSplash', "'enterFrame'"]),
         'buttonLayoutNX_d.swf': (patch_bl, ["'dmPadBtn'", 'close_txt', 'getBounds', "'mouseUp'", "'enterFrame'"]),
         'characterCreation_d.swf': (patch_cc, ["'dmPadBtn'", 'dmHgWalk', 'drawRect', "'IE UICreationTabNext'", 'tcPad', 'list_pos', 'dmHbFind', 'btnHints_mc', 'iconId'])}
POST = list(KINDS)


def patch(name, data):
    c = KINDS[name][0](data)
    return c.done(), {'methods': list(c.made), 'moved': dict(c.moved), 'names': getattr(c, 'names', ())}


def check(name, src):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(name, src)
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs(); n = len(rep['methods'])
    print('%s (taps3): %d B -> %d B, +%d methods, sha256 %s' % (name, len(src), len(out), n, hashlib.sha256(out).hexdigest()[:16]))
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
    ta_, tb_ = [{(X.mname(c['name']), X.short(t.name)) for c in X.instances for t in c['traits']} for X in (A, B)]
    say(ta_ <= tb_, 'no trait removed (+%d)' % len(tb_ - ta_))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies (stack, scope, locals, branch targets, constant indices)' % len(B.bodies))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips')
    d = '\n'.join('\n'.join(B.dis(B.methods[m].body)) for m in rep['methods'])
    miss = [w for w in KINDS[name][1] if w not in d]
    say(not miss, 'the added code holds: %s%s' % (', '.join(KINDS[name][1]), '  MISSING ' + str(miss) if miss else ''))
    return bad


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    if a[0] == 'build':
        if len(a) < 3: sys.exit(__doc__)
        name = os.path.basename(a[2])
        if name not in KINDS: sys.exit('output name must be one of: ' + ' '.join(KINDS))
        out, rep = patch(name, open(a[1], 'rb').read()); open(a[2], 'wb').write(out)
        print('%s: %d B, sha256 %s' % (a[2], len(out), hashlib.sha256(out).hexdigest())); return
    bad = 0
    for name in POST:
        p = Path(a[1]) / name
        if p.exists(): bad += check(name, p.read_bytes())
    print('FAILED: %d' % bad if bad else 'taps3: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
