"""[DOS2] Touch for the console list menus that have no init / btnList of the main menu: the pause menu (gameMenu_c.swf -> gameMenu_d.swf, type 0x4d)
and the option list of the message box (msgBox_c.swf -> msgBox_d.swf, types 0x4b / 0x4c). docs/dos2/TOUCH_CONSOLE_PLAN.md, "Пауза и msgBox".

Same recipe as tools/dos2_mainmenu_touch.py (mainMenu_d), other class and fields:
  gameMenu_c  class gameMenu_c_fla.MenuMC_1 (= root.gameMenu_mc): list `verticalList` (LS_Classes.listDisplay) of Menu_button (text_txt, id,
              isEnabled; a disabled one is added with selectable = false); pad A = IE UIAccept -> MainTimeline.executeSelected ->
              MenuMC_1.executeSelected -> verticalList.m_CurrentSelection.buttonAction() (EI buttonPressed(id) + sound). The menu is (re)built and
              shown by MenuMC_1.openMenu: the stage listeners are added from there (once).
  msgBox_c    class msgBox_c_fla.popup_mc_2 (= root.popup_mc): option list `hlist` (LS_Classes.horizontalList, in optionsHolder_mc) of Menu_button;
              pad A = IE UIAccept -> MainTimeline.executeSelected (when hlist has options) -> popup_mc_2.executeSelected -> buttonAction of the
              selection (or "MultiSelectionDone" with the multi selector). Options are laid out by popup_mc_2.doneAdding: listeners from there.
              Its button hints (A/B/X/Y) have only stub mouse listeners (TOUCH_UI_AUDIT §1.5), so msgBox_d also gets tools/dos2_hint_touch.py:
              setHintIcon wrapper (tap on a hint = EI dmPadBtn(code)) + the document's "IE UIAccept after a stage mouse event" guard.
Behaviour added (appended methods only, no original method body changed):
  hit test per element in its OWN coordinates (el.mouseX / el.mouseY against its text field's box + a margin: works for the vertical list and for
  the horizontal one, whose elements sit in row holders); only visible + selectable elements of a visible list in a visible menu count;
    move, no button  -> that item becomes the selection through the stock list.selectMC (arrow, EI menuButtonOver, sound) - the pad's look;
    down             -> selection + pressed look (label 2 px down, alpha 0.6);
    move while down  -> leaving the item cancels the press;
    up on the item   -> the ORIGINAL executeSelected (kept as tuExecOrig) = exactly what the pad's A runs for that selection; tap outside = nothing.
  executeSelected (the A path) is wrapped: for 700 ms after a mouse event it does nothing (a tap does not also "accept" the focused item);
  when it runs it stamps tuFired, and a release within 700 ms of that does not run it again. Time only, no state can stick. The pad path
  (onEventUp / onEventDown bodies, moveCursor, the original executeSelected, listDisplay next / previous / select / selectMC) is byte-identical.

  dialog_c    (dialog_d.swf, types 0x42 / 0x65) has its own touch code (drag over the answers = move the selection, release = run it), but a tap
              runs the HIGHLIGHTED answer, not the tapped one: patch_dialog adds "press = select the answer under the finger" (see there).

  python -I tools/dos2_list_touch.py build <Switch Game.pak | x_c.swf> <out dir | out.swf> [gameMenu msgBox dialog]
  python -I tools/dos2_list_touch.py check <Switch Game.pak | x_c.swf> [gameMenu msgBox dialog]        static verification, exit 1 on any failure
"""
import hashlib, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, OPS, MN_MULTIL, NS_PUBLIC
import dos2_hint_touch

GUI = 'Public/Game/GUI/'
GUARD_MS = 700
DEBOUNCE_MS = 250   # pack 3f (owner): only a repeated-click guard (tuFired), no 'no accept for 700 ms after a mouse event'
NEW = ['tuActive', 'tuHit', 'tuOver', 'tuUnpress', 'tuMove', 'tuPress', 'tuRelease']
CFG = {
    'gameMenu': dict(src='gameMenu_c.swf', out='gameMenu_d.swf', cls='gameMenu_c_fla.MenuMC_1', list='verticalList', hook='openMenu', padx=120, pady=6,
                     active=[['visible'], ['verticalList'], ['verticalList', 'visible']], hints=False,
                     pad=['gameMenu_c_fla.MainTimeline::onEventUp', 'gameMenu_c_fla.MainTimeline::onEventDown', 'gameMenu_c_fla.MainTimeline::executeSelected',
                          'gameMenu_c_fla.MenuMC_1::moveCursor', 'gameMenu_c_fla.MenuMC_1::setListLoopable', 'gameMenu_c_fla.MenuMC_1::onMoveSelection',
                          'gameMenu_c_fla.MenuMC_1::closeMenu', 'Menu_button::buttonAction', 'Menu_button::selectElement']),
    'msgBox': dict(src='msgBox_c.swf', out='msgBox_d.swf', cls='msgBox_c_fla.popup_mc_2', list='hlist', hook='doneAdding', padx=30, pady=16,
                   active=[['visible'], ['hlist'], ['hlist', 'visible'], ['optionsHolder_mc'], ['optionsHolder_mc', 'visible']], hints=True,
                   pad=['msgBox_c_fla.MainTimeline::executeSelected', 'msgBox_c_fla.popup_mc_2::moveCursor', 'msgBox_c_fla.popup_mc_2::onMoveSelectionOption',
                        'msgBox_c_fla.popup_mc_2::selectMultiSelectorOption', 'msgBox_c_fla.popup_mc_2::addOption', 'Menu_button::buttonAction', 'Menu_button::selectElement']),
    'dialog': dict(src='dialog_c.swf', out='dialog_d.swf'),   # patch_dialog: own recipe (types 0x42 / 0x65)
}
LIST_PAD = ['LS_Classes.listDisplay::selectMC', 'LS_Classes.listDisplay::select', 'LS_Classes.listDisplay::next', 'LS_Classes.listDisplay::previous']


def patch_list(data, cfg):
    """console movie bytes -> (bytes with the list touch, report)"""
    swf = Swf(data); (tag, pre, abc), = swf.abcs()
    CLS = cfg['cls']; ci = abc.class_index(CLS); inst = abc.instances[ci]
    assert not inst['flags'] & 1, 'the class is sealed: dynamic properties would not work'
    t_hook, t_exec = abc.trait(ci, cfg['hook']), abc.trait(ci, 'executeSelected')
    assert t_hook and t_exec and t_hook.kind & 15 == 1 and t_exec.kind & 15 == 1
    ns = abc.mn[t_exec.name][1]; assert abc.ns[ns] == (NS_PUBLIC, abc.string('')), 'executeSelected is not public'
    assert abc.mn[t_hook.name][1] == ns
    for n in NEW + ['tuHookOrig', 'tuExecOrig']: assert abc.trait(ci, n) is None, 'already patched'
    assert not abc.methods[t_exec.b].params, 'executeSelected takes parameters'
    P = lambda name: abc.qname(ns, name)
    getTimer = abc.pub('getTimer', 'flash.utils')
    own = abc.owners(); by = {v: k for k, v in own.items()}
    late = [ops[0] for _, op, ops, _ in abc.ins(abc.methods[by['LS_Classes.listDisplay::positionElements']].body.code)
            if OPS[op][0] == 'getproperty' and abc.mn[ops[0]][0] == MN_MULTIL][0]      # the compiler's own a[i] name
    ref = abc.methods[t_exec.b].body
    L = cfg['list']; PX, PY = cfg['padx'], cfg['pady']; assert 0 < PX < 128 and 0 < PY < 128

    def now(): return [('findpropstrict', getTimer), ('callproperty', getTimer, 0)]
    def stamp(): return [('getlocal0',)] + now() + [('setproperty', P('tuLast'))]
    def num(n, v=0): return [('pushbyte', v), ('convert_d',), ('setlocal', n)]
    def obj(n): return [('pushnull',), ('coerce_a',), ('setlocal', n)]
    HEAD = [('getlocal0',), ('pushscope',)]

    code = {}
    act = []
    for chain in [['stage']] + cfg['active']:
        act += [('getlocal0',)] + [('getproperty', P(p)) for p in chain] + [('iffalse', 'no')]
    code['tuActive'] = (0, HEAD + act + [('pushtrue',), ('returnvalue',), ('L', 'no'), ('pushfalse',), ('returnvalue',)])
    # tuHit(): locals 1 list 2 array 3 n 4 i 5 el 6 label 7 mx 8 my 9 x0 10 y0 11 x1 12 y1
    code['tuHit'] = (0, HEAD + obj(1) + obj(2) + num(3) + num(4) + obj(5) + obj(6) + num(7) + num(8) + num(9) + num(10) + num(11) + num(12) + [
        ('getlocal0',), ('getproperty', P(L)), ('coerce_a',), ('setlocal', 1),
        ('getlocal', 1), ('iffalse', 'none'),
        ('getlocal', 1), ('getproperty', P('content_array')), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'none'),
        ('getlocal', 2), ('getproperty', P('length')), ('convert_d',), ('setlocal', 3),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal', 2), ('getlocal', 4), ('getproperty', late), ('coerce_a',), ('setlocal', 5),
        ('getlocal', 5), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('selectable')), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('text_txt')), ('coerce_a',), ('setlocal', 6),
        ('getlocal', 6), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('mouseX')), ('convert_d',), ('setlocal', 7),
        ('getlocal', 5), ('getproperty', P('mouseY')), ('convert_d',), ('setlocal', 8),
        ('getlocal', 6), ('getproperty', P('x')), ('pushbyte', PX), ('subtract',), ('convert_d',), ('setlocal', 9),
        ('getlocal', 6), ('getproperty', P('y')), ('pushbyte', PY), ('subtract',), ('convert_d',), ('setlocal', 10),
        ('getlocal', 6), ('getproperty', P('x')), ('getlocal', 6), ('getproperty', P('width')), ('add',), ('pushbyte', PX), ('add',), ('convert_d',), ('setlocal', 11),
        ('getlocal', 6), ('getproperty', P('y')), ('getlocal', 6), ('getproperty', P('height')), ('add',), ('pushbyte', PY), ('add',), ('convert_d',), ('setlocal', 12),
        ('getlocal', 7), ('getlocal', 9), ('ifnge', 'next'),      # mx >= x0
        ('getlocal', 7), ('getlocal', 11), ('ifnle', 'next'),     # mx <= x1
        ('getlocal', 8), ('getlocal', 10), ('ifnge', 'next'),     # my >= y0
        ('getlocal', 8), ('getlocal', 12), ('ifnle', 'next'),     # my <= y1
        ('getlocal', 5), ('returnvalue',),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal', 3), ('iflt', 'loop'),
        ('L', 'none'), ('pushnull',), ('returnvalue',)])
    # tuOver(el): hover = the stock selection (CHANGE -> arrow; selectElement -> EI menuButtonOver, sound)
    code['tuOver'] = (1, HEAD + [
        ('getlocal1',), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P(L)), ('callproperty', P('getCurrentMovieClip'), 0), ('getlocal1',), ('ifeq', 'end'),
        ('getlocal0',), ('getproperty', P(L)), ('getlocal1',), ('pushfalse',), ('callpropvoid', P('selectMC'), 2),
        ('L', 'end'), ('returnvoid',)])
    code['tuUnpress'] = (0, HEAD + obj(1) + obj(2) + [
        ('getlocal0',), ('getproperty', P('tuItem')), ('coerce_a',), ('setlocal', 1),
        ('getlocal0',), ('pushnull',), ('setproperty', P('tuItem')),
        ('getlocal', 1), ('iffalse', 'end'),
        ('getlocal', 1), ('getproperty', P('text_txt')), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal', 2), ('getlocal0',), ('getproperty', P('tuY')), ('setproperty', P('y')),
        ('getlocal', 2), ('pushbyte', 1), ('setproperty', P('alpha')),
        ('L', 'end'), ('returnvoid',)])
    code['tuMove'] = (1, HEAD + [('getlocal0',), ('getproperty', P('tuSeen')), ('iffalse', 'unseen')] + stamp() + [
        ('L', 'unseen'),
        ('getlocal0',), ('getproperty', P('tuDown')), ('iffalse', 'hover'),
        ('getlocal0',), ('getproperty', P('tuItem')), ('iffalse', 'end'),
        ('getlocal0',), ('callproperty', P('tuActive'), 0), ('iffalse', 'cancel'),
        ('getlocal0',), ('callproperty', P('tuHit'), 0), ('getlocal0',), ('getproperty', P('tuItem')), ('ifeq', 'end'),
        ('L', 'cancel'), ('getlocal0',), ('callpropvoid', P('tuUnpress'), 0), ('jump', 'end'),
        ('L', 'hover'),
        ('getlocal0',), ('callproperty', P('tuActive'), 0), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal0',), ('callproperty', P('tuHit'), 0), ('callpropvoid', P('tuOver'), 1),
        ('L', 'end'), ('returnvoid',)])
    code['tuPress'] = (1, HEAD + obj(2) + obj(3) + stamp() + [
        ('getlocal0',), ('callpropvoid', P('tuUnpress'), 0),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('tuDown')),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('tuSeen')),
        ('getlocal0',), ('callproperty', P('tuActive'), 0), ('iffalse', 'end'),
        ('getlocal0',), ('callproperty', P('tuHit'), 0), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', 2), ('callpropvoid', P('tuOver'), 1),
        ('getlocal0',), ('getlocal', 2), ('setproperty', P('tuItem')),
        ('getlocal', 2), ('getproperty', P('text_txt')), ('coerce_a',), ('setlocal', 3),
        ('getlocal', 3), ('iffalse', 'end'),
        ('getlocal0',), ('getlocal', 3), ('getproperty', P('y')), ('setproperty', P('tuY')),
        ('getlocal', 3), ('getlocal', 3), ('getproperty', P('y')), ('pushbyte', 2), ('add',), ('setproperty', P('y')),
        ('getlocal', 3), ('pushbyte', 6), ('pushbyte', 10), ('divide',), ('setproperty', P('alpha')),
        ('L', 'end'), ('returnvoid',)])
    code['tuRelease'] = (1, HEAD + obj(2) + stamp() + [
        ('getlocal0',), ('pushfalse',), ('setproperty', P('tuDown')),
        ('getlocal0',), ('getproperty', P('tuItem')), ('coerce_a',), ('setlocal', 2),
        ('getlocal0',), ('callpropvoid', P('tuUnpress'), 0),
        ('getlocal', 2), ('iffalse', 'end'),
        ('getlocal0',), ('callproperty', P('tuActive'), 0), ('iffalse', 'end'),
        ('getlocal0',), ('callproperty', P('tuHit'), 0), ('getlocal', 2), ('ifne', 'end'),      # released somewhere else: no action
        ] + now() + [('getlocal0',), ('getproperty', P('tuFired')), ('subtract',), ('pushshort', DEBOUNCE_MS), ('iflt', 'end'),   # just ran (double fire)
        ('getlocal0',), ('getproperty', P(L)), ('getproperty', P('m_CurrentSelection')), ('getlocal', 2), ('ifne', 'end'),   # the pad path acts on the selection
        ('getlocal0',)] + now() + [('setproperty', P('tuFired')),
        ('getlocal0',), ('callpropvoid', P('tuExecOrig'), 0),
        ('L', 'end'), ('returnvoid',)])
    code['executeSelected'] = (0, HEAD + now() + [
        ('getlocal0',), ('getproperty', P('tuFired')), ('subtract',), ('pushshort', DEBOUNCE_MS), ('iflt', 'end'),   # a double fire (pack 3f: no guard after mouse events)
        ('getlocal0',)] + now() + [('setproperty', P('tuFired')),
        ('getlocal0',), ('callpropvoid', P('tuExecOrig'), 0),
        ('L', 'end'), ('returnvoid',)])
    horig = abc.methods[t_hook.b]; nh = len(horig.params)
    code['hook'] = (nh, HEAD + obj(nh + 1) + [('getlocal0',)] + [('getlocal', k + 1) for k in range(nh)] + [('callpropvoid', P('tuHookOrig'), nh),
        ('getlocal0',), ('getproperty', P('tuHooked')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', nh + 1),
        ('getlocal', nh + 1), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('tuHooked')),
        ('getlocal', nh + 1), ('pushstring', abc.string('mouseMove')), ('getlocal0',), ('getproperty', P('tuMove')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal', nh + 1), ('pushstring', abc.string('mouseDown')), ('getlocal0',), ('getproperty', P('tuPress')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal', nh + 1), ('pushstring', abc.string('mouseUp')), ('getlocal0',), ('getproperty', P('tuRelease')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)])

    made = {}
    for name, (nparams, lines) in code.items():
        m = abc.add_method('tu_' + name, [0] * nparams, 0, asm(lines), 64, 64, ref.init_scope, ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        made[name] = m
    mh = abc.methods[made['hook']]; mh.params = list(horig.params); mh.ret = horig.ret
    if horig.flags & 0x08: mh.flags |= 0x08; mh.options = list(horig.options)
    old_hook, old_exec = t_hook.b, t_exec.b
    t_hook.b = made['hook']; t_exec.b = made['executeSelected']
    inst['traits'] += [Trait(P('tuHookOrig'), 1, 0, old_hook), Trait(P('tuExecOrig'), 1, 0, old_exec)]
    for n in NEW: inst['traits'].append(Trait(P(n), 1, 0, made[n]))
    swf.tags[tag][1] = pre + abc.write()
    return swf.write(), {'methods': made, 'orig': {cfg['hook']: old_hook, 'executeSelected': old_exec}}


DLG = 'dialog_c_fla.Dialog_1'
DLG_NEW = ['tuDlgHit', 'tuDlgPress', 'tuDlgRelease']
DLG_MOVE = 40          # px of finger travel (stage) after which a release is a drag, not a tap


def patch_dialog(data):
    """dialog_c.swf -> dialog_d.swf. dialog_c's own touch code (Dialog_1 frame1: stage MOUSE_MOVE over the answer list moves the selection every 50 px,
    MOUSE_UP without such a move -> executeSelected) runs the HIGHLIGHTED answer, not the one under the finger. Added: a stage mouseDown that selects
    the answer under the finger through the stock Dialog_1.select(i) (highlight, EI selectedId, sound - the pad's own path), and a stage mouseUp
    that, when the finger is still on that answer and moved < 40 px, runs the stock executeSelected (EI QuestionPressed). The stock MOUSE_UP
    listener was added first, so it runs first; executeSelected is one-shot per answer set (selectionMade), so the answer is never sent twice."""
    swf = Swf(data); (tag, pre, abc), = swf.abcs()
    ci = abc.class_index(DLG); inst = abc.instances[ci]
    assert not inst['flags'] & 1
    t_hook, t_sel, t_exec = abc.trait(ci, 'addAnswersDone'), abc.trait(ci, 'select'), abc.trait(ci, 'executeSelected')
    assert t_hook and t_sel and t_exec and not abc.methods[t_hook.b].params and not abc.methods[t_exec.b].params
    ns = abc.mn[t_hook.name][1]; assert abc.ns[ns] == (NS_PUBLIC, abc.string(''))
    for n in DLG_NEW + ['tuHookOrig']: assert abc.trait(ci, n) is None, 'already patched'
    P = lambda name: abc.qname(ns, name)
    own = abc.owners(); by = {v: k for k, v in own.items()}
    late = [ops[0] for _, op, ops, _ in abc.ins(abc.methods[by[DLG + '::highlightAnswer']].body.code)
            if OPS[op][0] == 'getproperty' and abc.mn[ops[0]][0] == MN_MULTIL][0]
    ref = abc.methods[t_exec.b].body
    HEAD = [('getlocal0',), ('pushscope',)]
    def num(n, v=0): return [('pushbyte', v), ('convert_d',), ('setlocal', n)]
    def obj(n): return [('pushnull',), ('coerce_a',), ('setlocal', n)]
    code = {}
    # tuDlgHit(): index of the answer under the finger, -1 none.  locals 1 holder 2 array 3 n 4 i 5 el 6 mx 7 my
    code['tuDlgHit'] = (0, HEAD + obj(1) + obj(2) + num(3) + num(4) + obj(5) + num(6) + num(7) + [
        ('getlocal0',), ('getproperty', P('dialogActive')), ('iffalse', 'none'),
        ('getlocal0',), ('getproperty', P('stage')), ('iffalse', 'none'),
        ('getlocal0',), ('getproperty', P('answerList')), ('iffalse', 'none'),
        ('getlocal0',), ('getproperty', P('cAnswerHolder')), ('coerce_a',), ('setlocal', 1),
        ('getlocal', 1), ('iffalse', 'none'),
        ('getlocal', 1), ('getproperty', P('answer_array')), ('coerce_a',), ('setlocal', 2),
        ('getlocal', 2), ('iffalse', 'none'),
        ('getlocal0',), ('getproperty', P('stage')), ('getproperty', P('mouseX')), ('convert_d',), ('setlocal', 6),
        ('getlocal0',), ('getproperty', P('stage')), ('getproperty', P('mouseY')), ('convert_d',), ('setlocal', 7),
        ('getlocal0',), ('getproperty', P('answerList')), ('getlocal', 6), ('getlocal', 7), ('pushfalse',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'none'),
        ('getlocal', 2), ('getproperty', P('length')), ('convert_d',), ('setlocal', 3),
        ('jump', 'cond'),
        ('L', 'loop'), ('label',),
        ('getlocal', 2), ('getlocal', 4), ('getproperty', late), ('coerce_a',), ('setlocal', 5),
        ('getlocal', 5), ('iffalse', 'next'),
        ('getlocal', 5), ('getproperty', P('visible')), ('iffalse', 'next'),
        ('getlocal', 5), ('getlocal', 6), ('getlocal', 7), ('pushfalse',), ('callproperty', P('hitTestPoint'), 3), ('iffalse', 'next'),
        ('getlocal', 4), ('returnvalue',),
        ('L', 'next'), ('getlocal', 4), ('increment',), ('convert_d',), ('setlocal', 4),
        ('L', 'cond'), ('getlocal', 4), ('getlocal', 3), ('iflt', 'loop'),
        ('L', 'none'), ('pushbyte', -1 & 0xff), ('returnvalue',)])
    code['tuDlgPress'] = (1, HEAD + num(2) + [
        ('getlocal0',), ('pushfalse',), ('setproperty', P('tuArmed')),
        ('getlocal0',), ('callproperty', P('tuDlgHit'), 0), ('convert_d',), ('setlocal', 2),
        ('getlocal', 2), ('pushbyte', 0), ('ifnge', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('tuArmed')),
        ('getlocal0',), ('getlocal', 2), ('setproperty', P('tuAns')),
        ('getlocal0',), ('getlocal0',), ('getproperty', P('stage')), ('getproperty', P('mouseY')), ('setproperty', P('tuY')),
        ('getlocal0',), ('getproperty', P('cAnswerHolder')), ('getproperty', P('currentSelection')), ('getlocal', 2), ('ifeq', 'end'),
        ('getlocal0',), ('getlocal', 2), ('callpropvoid', P('select'), 1),
        ('L', 'end'), ('returnvoid',)])
    code['tuDlgRelease'] = (1, HEAD + num(2) + [
        ('getlocal0',), ('getproperty', P('tuArmed')), ('iffalse', 'end'),
        ('getlocal0',), ('pushfalse',), ('setproperty', P('tuArmed')),
        ('getlocal0',), ('getproperty', P('stage')), ('iffalse', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('getproperty', P('mouseY')), ('getlocal0',), ('getproperty', P('tuY')), ('subtract',), ('convert_d',), ('setlocal', 2),
        ('getlocal', 2), ('getlocal', 2), ('multiply',), ('pushshort', DLG_MOVE * DLG_MOVE), ('ifnlt', 'end'),      # dragged: the stock scroll / select took it
        ('getlocal0',), ('callproperty', P('tuDlgHit'), 0), ('getlocal0',), ('getproperty', P('tuAns')), ('ifne', 'end'),   # released elsewhere: nothing
        ('getlocal0',), ('getproperty', P('cAnswerHolder')), ('getproperty', P('currentSelection')), ('getlocal0',), ('getproperty', P('tuAns')), ('ifne', 'end'),
        ('getlocal0',), ('callpropvoid', P('executeSelected'), 0),                # no-op when the stock MOUSE_UP already sent it (selectionMade)
        ('L', 'end'), ('returnvoid',)])
    code['hook'] = (0, HEAD + obj(1) + [('getlocal0',), ('callpropvoid', P('tuHookOrig'), 0),
        ('getlocal0',), ('getproperty', P('tuHooked')), ('iftrue', 'end'),
        ('getlocal0',), ('getproperty', P('stage')), ('coerce_a',), ('setlocal', 1),
        ('getlocal', 1), ('iffalse', 'end'),
        ('getlocal0',), ('pushtrue',), ('setproperty', P('tuHooked')),
        ('getlocal', 1), ('pushstring', abc.string('mouseDown')), ('getlocal0',), ('getproperty', P('tuDlgPress')), ('callpropvoid', P('addEventListener'), 2),
        ('getlocal', 1), ('pushstring', abc.string('mouseUp')), ('getlocal0',), ('getproperty', P('tuDlgRelease')), ('callpropvoid', P('addEventListener'), 2),
        ('L', 'end'), ('returnvoid',)])
    made = {}
    for name, (nparams, lines) in code.items():
        m = abc.add_method('tu_' + name, [0] * nparams, 0, asm(lines), 64, 64, ref.init_scope, ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        made[name] = m
    old = t_hook.b; t_hook.b = made['hook']
    inst['traits'].append(Trait(P('tuHookOrig'), 1, 0, old))
    for n in DLG_NEW: inst['traits'].append(Trait(P(n), 1, 0, made[n]))
    swf.tags[tag][1] = pre + abc.write()
    return swf.write(), {'methods': made, 'orig': {'addAnswersDone': old}}


def check_dialog(src, built=None):
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch_dialog(src)
    if built is not None: say(built == out, 'the built movie is exactly what the patcher makes from this source')
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs()
    print('dialog_c.swf -> dialog_d.swf: %d B -> %d B, sha256 %s' % (len(src), len(out), hashlib.sha256(out).hexdigest()))
    say(b.ver == a.ver and b.head == a.head and len(a.tags) == len(b.tags) and all(x[0] == y[0] and (n == ta or x[1] == y[1]) for n, (x, y) in enumerate(zip(a.tags, b.tags))),
        'header and every tag but DoABC #%d byte-identical (%d tags)' % (ta, len(a.tags)))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips')
    say(B.strings[:len(A.strings)] == A.strings and B.ns[:len(A.ns)] == A.ns and B.nssets == A.nssets and B.mn[:len(A.mn)] == A.mn and B.ints == A.ints and B.doubles == A.doubles,
        'constant pool only appended')
    same = all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc) and x.method == y.method
               for x, y in zip(A.bodies, B.bodies))
    say(same and len(B.bodies) == len(A.bodies) + 4 and len(B.methods) == len(A.methods) + 4, 'all %d original method bodies byte-identical; 4 methods appended' % len(A.bodies))
    oa, ob = A.owners(), B.owners(); ia, ib = {v: k for k, v in oa.items()}, {v: k for k, v in ob.items()}
    pad = [DLG + '::' + n for n in ('select', 'executeSelected', 'executeSelection', 'moveSelection', 'highlightAnswer', 'chooseAnswer', 'frame1')] + \
          ['dialog_c_fla.MainTimeline::onEventUp', 'dialog_c_fla.MainTimeline::onEventDown']
    say(all(ia.get(n) == ib.get(n) and n in ia for n in pad), 'pad path and the stock touch code bound to their original methods: ' + ', '.join(n.split('::')[1] for n in pad))
    moved = sorted('%s -> %s' % (oa[m], ob[m]) for m in oa if ob.get(m) != oa[m])
    say(moved == ['%s::addAnswersDone -> %s::tuHookOrig' % (DLG, DLG)] and all(n in ib for n in ia), 'renamed: addAnswersDone -> tuHookOrig; no name lost')
    ci = B.class_index(DLG)
    say(all(c1 == c2 for n, (c1, c2) in enumerate(zip(A.instances, B.instances)) if n != ci) and A.classes == B.classes and A.scripts == B.scripts
        and len(B.instances[ci]['traits']) == len(A.instances[ci]['traits']) + 4, 'only class %s changes: +4 method traits' % DLG)
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies' % len(B.bodies))
    dis = lambda n: '\n'.join(B.dis(B.methods[ib[DLG + '::' + n]].body))
    say("'mouseDown'" in dis('addAnswersDone') and "'mouseUp'" in dis('addAnswersDone') and 'tuHookOrig' in dis('addAnswersDone'), 'addAnswersDone = original + stage mouseDown / mouseUp (once)')
    say('select' in dis('tuDlgPress') and 'executeSelected' in dis('tuDlgRelease') and 'hitTestPoint' in dis('tuDlgHit'),
        'press: stock select(answer under the finger); release on it (< %d px): stock executeSelected' % DLG_MOVE)
    return bad


def patch(data, which):
    """<which>_c.swf bytes -> (<which>_d.swf bytes, report). msgBox: + tools/dos2_hint_touch.py (hint taps and the document's accept guard)."""
    if which == 'dialog': return patch_dialog(data)
    cfg = CFG[which]
    out, rep = patch_list(data, cfg)
    if cfg['hints']:
        out, hrep = dos2_hint_touch.patch(out, guard=True, hints=True); rep['hints'] = hrep
    return out, rep


def check(which, src, built=None):
    """static verification; returns the number of failures"""
    if which == 'dialog': return check_dialog(src, built)
    cfg = CFG[which]; CLS = cfg['cls']
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    out, rep = patch(src, which)
    nh = len(rep['hints']['hint']) + len(rep['hints']['guard']) if 'hints' in rep else 0
    nnew = 9 + nh
    if built is not None: say(built == out, 'the built movie is exactly what the patcher makes from this source (%d B)' % len(built))
    a, b = Swf(src), Swf(out); (ta, pa, A), = a.abcs(); (tb, pb, B), = b.abcs()
    print('%s -> %s: %d B -> %d B, sha256 %s' % (cfg['src'], cfg['out'], len(src), len(out), hashlib.sha256(out).hexdigest()))
    say(b.sig == b'CWS' and b.ver == a.ver and b.head == a.head, 'SWF header: version %d, frame rectangle / rate unchanged' % b.ver)
    say(len(a.tags) == len(b.tags) and all(x[0] == y[0] and (n == ta or x[1] == y[1]) for n, (x, y) in enumerate(zip(a.tags, b.tags))),
        'every tag but DoABC #%d is byte-identical (%d tags: the console look)' % (ta, len(a.tags)))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips (SWF and ABC rewritten byte-identical)')
    say(B.strings[:len(A.strings)] == A.strings and B.ns[:len(A.ns)] == A.ns and B.nssets == A.nssets and B.mn[:len(A.mn)] == A.mn and B.ints == A.ints
        and B.doubles == A.doubles, 'constant pool only appended: +%d strings, +%d namespaces, +%d multinames' % (len(B.strings) - len(A.strings), len(B.ns) - len(A.ns), len(B.mn) - len(A.mn)))
    same = all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc)
               and x.method == y.method for x, y in zip(A.bodies, B.bodies))
    say(same and len(B.bodies) == len(A.bodies) + nnew and len(B.methods) == len(A.methods) + nnew,
        'all %d original method bodies byte-identical; %d methods appended (none removed)' % (len(A.bodies), nnew))
    oa, ob = A.owners(), B.owners(); ia, ib = {v: k for k, v in oa.items()}, {v: k for k, v in ob.items()}
    pad = cfg['pad'] + LIST_PAD
    say(all(ia[n] == ib[n] for n in pad), 'pad path still bound to its original methods: ' + ', '.join(n.split('::')[1] for n in pad))
    say(all(n in ib for n in ia), 'every original method name still resolves (no trait removed)')
    moved = sorted('%s -> %s' % (oa[m], ob[m]) for m in oa if ob.get(m) != oa[m])
    want = sorted(['%s::%s -> %s::tuHookOrig' % (CLS, cfg['hook'], CLS), '%s::executeSelected -> %s::tuExecOrig' % (CLS, CLS)]
                  + (['%s -> %s' % (n.replace('::tuEvUpOrig', '::onEventUp').replace('::tuEvDownOrig', '::onEventDown').replace('::tuInitOrig2', '::onEventInit')
                                    .replace('::tuHintOrig', '::setHintIcon'), n) for n in sorted(ob.values()) if n.split('::')[-1] in ('tuEvUpOrig', 'tuEvDownOrig', 'tuInitOrig2', 'tuHintOrig')]))
    say(moved == want, 'renamed methods: ' + '; '.join(m.replace(CLS + '::', '') for m in moved))
    ci = B.class_index(CLS)
    others = [n for n in range(len(A.instances)) if n != ci and A.instances[n] != B.instances[n]]
    say(len(B.instances[ci]['traits']) == len(A.instances[ci]['traits']) + 9 and A.classes == B.classes and A.scripts == B.scripts
        and all(B.mname(B.instances[n]['name']) in (dos2_hint_touch.HINT_CLS, '%s_fla.MainTimeline' % cfg['src'][:-4]) for n in others) and (cfg['hints'] or not others),
        '%s: +9 method traits, no slot, flags %#x (not sealed)%s' % (CLS, B.instances[ci]['flags'], ('; also changed: ' + ', '.join(B.mname(B.instances[n]['name']) for n in others)) if others else '; no other class changes'))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies (stack depth at every join, scope, locals, branch targets, constant indices)' % len(B.bodies))
    for n in sorted(rep['methods']):
        bd = B.methods[rep['methods'][n]].body; st, sc, loc = B.verify(bd)
        say((st, loc) == (bd.max_stack, bd.local_count), '  %-16s %3d B  max_stack %d  locals %d  scope %d..%d (exact)' % (n, len(bd.code), bd.max_stack, bd.local_count, bd.init_scope, bd.max_scope))
    dis = lambda n: '\n'.join(B.dis(B.methods[ib[CLS + '::' + n]].body))
    say('tuExecOrig' in dis('executeSelected') and 'pushshort        %d' % DEBOUNCE_MS in dis('executeSelected') and 'tuFired' in dis('executeSelected') and 'tuFired' in dis('tuRelease'),
        'no double fire: executeSelected and a tap release share tuFired, the second within %d ms does nothing' % DEBOUNCE_MS)
    say(all(("'%s'" % e) in dis(cfg['hook']) for e in ('mouseMove', 'mouseDown', 'mouseUp')) and 'tuHookOrig' in dis(cfg['hook']),
        '%s = original + stage mouseMove / mouseDown / mouseUp listeners (once)' % cfg['hook'])
    d = dis('tuRelease')
    say('tuExecOrig' in d and 'm_CurrentSelection' in d and 'tuHit' in d, 'release on the pressed item (still the selection) -> the original executeSelected = the pad A path')
    say('selectMC' in dis('tuOver') and 'mouseX' in dis('tuHit') and 'selectable' in dis('tuHit'), 'hover / press select through %s.selectMC; hit = visible + selectable items, own coordinates' % cfg['list'])
    if cfg['hints']:
        h = rep['hints']
        say(len(h['hint']) == 3 and len(h['guard']) >= 4, 'hint taps (controllerBtnHint.setHintIcon wrapped -> EI dmPadBtn) + document accept guard (%s)' % ', '.join(sorted(h.get('moved', {}))))
    return bad


def sources(path, names):
    data = open(path, 'rb').read()
    names = names or list(CFG)
    if data[:3] in (b'CWS', b'FWS', b'ZWS'):
        w = [k for k in CFG if CFG[k]['src'] == os.path.basename(path)]
        if not w: sys.exit('%s: not one of %s' % (path, ', '.join(c['src'] for c in CFG.values())))
        return {w[0]: data}
    import lspk
    pak = lspk.Pak(path); ent = {e[0]: e for e in pak.entries}
    out = {}
    for n in names:
        if n not in CFG: sys.exit('unknown movie %s (want %s)' % (n, ' '.join(CFG)))
        if GUI + CFG[n]['src'] not in ent: sys.exit('%s: no %s%s' % (path, GUI, CFG[n]['src']))
        out[n] = pak.read(ent[GUI + CFG[n]['src']])
    return out


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('build', 'check'): sys.exit(__doc__)
    if a[0] == 'build':
        if len(a) < 3: sys.exit(__doc__)
        src = sources(a[1], a[3:]); dst = a[2]
        for which, data in src.items():
            out, rep = patch(data, which)
            path = dst if dst.endswith('.swf') and len(src) == 1 else os.path.join(dst, CFG[which]['out'])
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True); open(path, 'wb').write(out)
            print('%s: %d B, sha256 %s' % (path, len(out), hashlib.sha256(out).hexdigest()))
        return
    bad = 0
    for which, data in sources(a[1], a[2:]).items(): bad += check(which, data)
    print('FAILED: %d' % bad if bad else 'list touch (pause menu, message box, dialog): all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
