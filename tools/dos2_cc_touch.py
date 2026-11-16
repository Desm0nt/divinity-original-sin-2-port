"""[DOS2] characterCreation_d.swf = the Switch console character creation (characterCreation_c.swf, Switch Game.pak) with real touch
(docs/dos2/TOUCH_UI_AUDIT.md section 2, verdict B; owner's priority 15.11).

Look: untouched (only the DoABC tag changes, no display object is added). Made in two layers:
  1. tools/dos2_hint_touch.py (as before): the button hints are pad buttons (EI dmPadBtn), "IE UIAccept" within 400 ms of a mouse event is swallowed;
     it keeps the ORIGINAL onEventDown / onEventUp as tuEvDownOrig / tuEvUpOrig.
  2. this file: methods appended to the document class characterCreation_c_fla.MainTimeline (not sealed: the state is dynamic properties tc*).
     Stage mouseDown / mouseMove / mouseUp listeners (added once, from onEventInit or the first input event). A press finds the element under the
     pointer by geometry (mouseX / mouseY of the parents against x / y / width / height; rows of a list by the list's own arithmetic, scrollRect.y
     included), moves the stock selection there (listDisplay.selectMC, the same call the pad's up / down ends in) and dims the pressed part
     (alpha 0.6). A release without a drag (30 px) then runs the PAD PATH itself: the original onEventDown(i) + onEventUp(i) with the index of the
     input event the pad would send, so every action is the console movie's own code and its own ExternalInterface call:
        header   leftBtn_mc / rightBtn_mc               IE UICreationTabPrev / Next      -> EI previousStep / nextStep
                 a step tab left / right of the current one: one step that way (not on the class-edit panels)
                 name field (text)                      IE UIEditCharacter               -> inputField.selectElement, EI editName(true); a press anywhere
                                                                                            else while it is edited = deselectElement (EI handleTextInput)
        origins / appearance   a selector row: selected; its left part / arrowL_mc = IE UILeft, right part / arrowR_mc = IE UIRight -> EI selectOption/3
                 female_mc / male_mc (the one that is not active)  IE UIToggleEquipment  -> EI setGender/1
                 armor_mc / helmet_mc                   IE UIToggleEquipment             -> toggleState, EI setArmourState/1 (the console's 3-state cycle)
        class    the class name: left / right of it = IE UILeft / UIRight -> EI prevPreset / nextPreset (first leaves the edit list as the pad's Up does:
                 EI canDoClassEdit(false)); an edit row: first tap selects (EI canDoClassEdit(true) as the pad's Down), a tap on the selected row =
                 IE UIAccept -> EI editClass/1
        class-edit panels   LB_mc / RB_mc, a tab left / right of the current one: IE UICreationTabPrev / Next -> EI prevClassEdit / nextClassEdit
        attributes   row selected; plus_mc = IE UIRight -> EI plusAttribute/1, min_mc = IE UILeft -> EI minAttribute/1
        abilities    group + row selected; plus_mc / min_mc -> EI plusAbility/2 / minAbility/2
        skills       row selected + IE UIAccept -> skillEntry.toggleSkill (EI toggleSkill/1)
        talents / tags   row selected (content or racial list); a content row + IE UIAccept -> EI toggleTalent/1 / toggleTag/2
        instruments  row selected + IE UIAccept -> EI setInstrument/1
        next / previous step, start, back, lore, tooltip: the button hints (layer 1) or the header buttons above
     Lists scroll by drag: a vertical drag (14 px) that starts in the panel scrolls the panel's scrollList (abilities, skills, talents, tags) through
     its own scrollbar.scrollTo; a drag never acts as a tap. The scrollbar itself already has mouse code (handle, arrows).
     The paper doll (stock stage listener -> EI rotateCharacter) is untouched: a press left of the panel is not ours.
  No double fire: onEventDown / onEventUp get a second wrapper - "IE UIAccept" within 700 ms of a mouse button event (and of a move once a button
  event was seen) is swallowed; when an accept does run its time is kept and a release within 700 ms of it does nothing (the other order). The
  taps call the ORIGINAL bodies (tuEvDownOrig / tuEvUpOrig), so the guards do not eat them. Time only: nothing can stick.
  The pad path is byte-identical: no original method body changes, only the traits onEventDown / onEventUp / onEventInit are re-pointed.

The touch code is written in a Python-syntax subset and compiled to AVM2 here (cc_compile): this.a.b, calls, if / while, locals, + - * /, compares.

  python -I tools/dos2_cc_touch.py build <characterCreation_c.swf | Switch Game.pak> <out characterCreation_d.swf>
  python -I tools/dos2_cc_touch.py check <characterCreation_c.swf | Switch Game.pak> [<built characterCreation_d.swf>]   static verification, exit 1 on any failure
"""
import ast, hashlib, re, sys, textwrap
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dos2_abc import Swf, Trait, asm, verify_all, OPS, MN_MULTIL, NS_PUBLIC
import dos2_hint_touch

CLS = 'characterCreation_c_fla.MainTimeline'
MOVIE = 'characterCreation_d.swf'      # same length as characterCreation_c.swf: the creator's path is rewritten in place (hle_dos2_touchui.c)
SRC = 'Public/Game/GUI/characterCreation_c.swf'
GUARD_MS = 700
DEBOUNCE_MS = 250   # pack 3f (owner: only a repeated-click guard; A registers only from a tap on the right place): a second accept / tap within
                    # 250 ms of the last one is dropped; no more 'no accept for 700 ms after a mouse event' (it ate the hint bar's A)
WRAP = {'onEventDown': 'tcEvDownPrev', 'onEventUp': 'tcEvUpPrev', 'onEventInit': 'tcInitPrev'}     # trait -> the name the previous method keeps
PAD_PATH = ['characterCreation_c_fla.characterCreationPanel_5::delegateDown', 'characterCreation_c_fla.characterCreationPanel_5::delegateUp',
            'characterCreation_c_fla.basePanelMC_11::delegateDown', 'selector::delegateDown', 'characterCreation_c_fla.attributesMC_22::delegateDown',
            'characterCreation_c_fla.abilitiesMC_23::delegateDown', 'characterCreation_c_fla.skillsMC_24::delegateDown',
            'characterCreation_c_fla.talentsMC_25::delegateDown', 'characterCreation_c_fla.tagsMC_29::delegateDown',
            'characterCreation_c_fla.tagsTalentsPanel_26::delegateDown', 'characterCreation_c_fla.classMC_19::delegateDown',
            'characterCreation_c_fla.instrumentsMC_30::delegateDown', 'inputField::selectElement', 'inputField::deselectElement',
            'optionSelection::nextEl', 'optionSelection::previousEl', 'skillEntry::toggleSkill', 'characterCreation_c_fla.armorHelmetVisSelector_16::toggleState',
            'LS_Classes.listDisplay::selectMC', 'LS_Classes.scrollList::selectMC', 'LS_Classes.listDisplay::select', 'LS_Classes.scrollbar::scrollTo',
            CLS + '::PaperDollHit', CLS + '::setPanel', CLS + '::selectOption']
# every ExternalInterface name a tap can end in (all are calls of the ORIGINAL bodies, reached through the pad path)
EI_TAPS = ['selectOption', 'setGender', 'setArmourState', 'nextStep', 'previousStep', 'nextPreset', 'prevPreset', 'editClass', 'nextClassEdit', 'prevClassEdit',
           'plusAttribute', 'minAttribute', 'plusAbility', 'minAbility', 'toggleSkill', 'toggleTalent', 'toggleTag', 'setInstrument', 'editName', 'handleTextInput']

CODE = r'''
def tcHook():
    if this.tcHooked: return
    st = this.stage
    if not st: return
    this.tcHooked = True
    st.addEventListener('mouseDown', this.tcDown)
    st.addEventListener('mouseMove', this.tcMove)
    st.addEventListener('mouseUp', this.tcUp)

def tcIn(o, px, py):
    # the pointer (in the coordinates of o's parent) is inside o's box grown by px / py
    if not o: return False
    par = o.parent
    if not par: return False
    if not o.visible: return False
    x = par.mouseX - o.x
    y = par.mouseY - o.y
    if x < 0 - px: return False
    if x > o.width + px: return False
    if y < 0 - py: return False
    if y > o.height + py: return False
    return True

def tcPick(lst, px, py):
    # the visible element of a list whose box (grown) holds the pointer: tabs
    if not lst: return None
    a = lst.content_array
    if not a: return None
    n = a.length
    i = 0
    while i < n:
        el = a[i]
        if this.tcIn(el, px, py): return el
        i = i + 1
    return None

def tcRowAt(lst, y):
    # the visible element of a vertical list whose band holds y (content coordinates)
    a = lst.content_array
    if not a: return None
    n = a.length
    sp = lst.EL_SPACING
    if not sp > 0: sp = 0
    i = 0
    while i < n:
        el = a[i]
        if el and el.visible:
            top = el.y
            if y >= top and y < top + lst.getElementHeight(el) + sp: return el
        i = i + 1
    return None

def tcRow(lst):
    # the element under the pointer; this.tcY = the pointer's y in the content (the scroll offset of a scrollList added by hand)
    if not lst: return None
    cm = lst.container_mc
    cn = lst.containerContent_mc
    if not cm or not cn: return None
    y = lst.mouseY - cm.y - cn.y
    r = cn.scrollRect
    if r:
        if y < 0: return None
        if y > r.height: return None
        y = y + r.y
    this.tcY = y
    return this.tcRowAt(lst, y)

def tcPM(row):
    # plus_mc / min_mc of a row under the pointer -> the pad's right / left
    pl = row.plus_mc
    mi = row.min_mc
    x = row.mouseX
    inP = False
    inM = False
    if pl and pl.visible and x >= pl.x - 45 and x <= pl.x + pl.width + 45: inP = True
    if mi and mi.visible and x >= mi.x - 45 and x <= mi.x + mi.width + 45: inM = True
    if inP and inM:
        dp = x - pl.x
        if dp < 0: dp = 0 - dp
        dm = x - mi.x
        if dm < 0: dm = 0 - dm
        if dp <= dm: inM = False
        else: inP = False
    if inP:
        this.tcBtn = pl
        return 'IE UIRight'
    if inM:
        this.tcBtn = mi
        return 'IE UILeft'
    return None

def tcStep(lst, px, py):
    # a tab of a horizontal list left / right of the current one -> one step that way
    t = this.tcPick(lst, px, py)
    if not t: return False
    cur = lst.getCurrentMovieClip()
    if not cur: return False
    if t == cur: return False
    this.tcBtn = t
    if t.list_pos > cur.list_pos: this.tcEv = 'IE UICreationTabNext'
    else: this.tcEv = 'IE UICreationTabPrev'
    return True

def tcFind():
    h = this.header_mc
    cc = this.CCPanel_mc
    p = this.currentPanel
    if not h or not cc: return
    nm = h.name_mc
    if nm:
        on = this.tcIn(nm.text_txt, 80, 30)
        if nm.isSelected:
            if not on: nm.deselectElement()
            return
        if on and nm.visible and nm.isEnabled:
            this.tcEv = 'IE UIEditCharacter'
            this.tcBtn = nm.text_txt
            return
    lb = h.leftBtn_mc
    if this.tcIn(lb, 40, 40):
        this.tcEv = 'IE UICreationTabPrev'
        this.tcBtn = lb
        return
    rb = h.rightBtn_mc
    if this.tcIn(rb, 40, 40):
        this.tcEv = 'IE UICreationTabNext'
        this.tcBtn = rb
        return
    det = cc.isDetailsPanel(p)
    if not det:
        if this.tcStep(h.stepTabs, 10, 20): return
    if this.mouseX < cc.x: return
    if this.mouseY >= this.btnHints_mc.y: return
    pa = cc.panelArray
    if not pa: return
    pan = pa[p]
    if not pan: return
    if not pan.visible: return
    base = pan.base_mc
    if det and base:
        if this.tcIn(base.LB_mc, 50, 40):
            this.tcEv = 'IE UICreationTabPrev'
            this.tcBtn = base.LB_mc
            return
        if this.tcIn(base.RB_mc, 50, 40):
            this.tcEv = 'IE UICreationTabNext'
            this.tcBtn = base.RB_mc
            return
        if this.tcStep(base.classEditTabList, 6, 16): return
    if p == 0 or p == 1:
        if not base: return
        if p == 0:
            g = pan.genderSelector_mc
            if g and g.visible:
                f = None
                if this.tcIn(g.female_mc, 30, 30): f = g.female_mc
                elif this.tcIn(g.male_mc, 30, 30): f = g.male_mc
                if f:
                    this.tcBtn = f
                    if f.currentFrame != 2: this.tcEv = 'IE UIToggleEquipment'
                    return
        else:
            q = pan.equip_mc
            if q and q.visible:
                f = None
                if this.tcIn(q.armor_mc, 30, 30): f = q.armor_mc
                elif this.tcIn(q.helmet_mc, 30, 30): f = q.helmet_mc
                if f:
                    this.tcBtn = f
                    this.tcEv = 'IE UIToggleEquipment'
                    return
        lst = base.contentHolder
        row = this.tcRow(lst)
        if not row: return
        if not row.selectable: return
        lst.selectMC(row, False)
        s = row.selectionMC
        if row.isSelector and s and s.arrowL_mc and s.arrowR_mc:
            x = s.mouseX
            if x <= s.arrowL_mc.x + 70:
                this.tcEv = 'IE UILeft'
                this.tcBtn = s.arrowL_mc
            elif x >= s.arrowR_mc.x - 70:
                this.tcEv = 'IE UIRight'
                this.tcBtn = s.arrowR_mc
        return
    if p == 2:
        cs = pan.classSelection_mc
        lst = pan.classEditList
        if not cs or not lst: return
        row = this.tcRow(lst)
        if row:
            was = False
            if not cs.visible:
                if lst.getCurrentMovieClip() == row: was = True
            if cs.visible:
                cs.visible = False
                EI.call('canDoClassEdit', True)
            lst.selectMC(row, False)
            this.tcBtn = row
            if was: this.tcEv = 'IE UIAccept'
            return
        t = pan.currentClass_txt
        if not t: return
        y = pan.mouseY - t.y - t.height / 2
        if y < -70: return
        if y > 70: return
        if not cs.visible:
            cs.visible = True
            lst.clearSelection()
            EI.call('canDoClassEdit', False)
        x = pan.mouseX - t.x - t.width / 2
        this.tcBtn = t
        if x < -60: this.tcEv = 'IE UILeft'
        elif x > 60: this.tcEv = 'IE UIRight'
        return
    if p == 3:
        lst = pan.attrList
        row = this.tcRow(lst)
        if not row: return
        lst.selectMC(row, False)
        pan.delegateDown('tc')
        this.tcEv = this.tcPM(row)
        return
    if p == 4 or p == 5:
        lst = pan.abilityGroupList
        this.tcSL = lst
        g = this.tcRow(lst)
        if not g: return
        al = g.abilityList
        if not al: return
        row = this.tcRowAt(al, this.tcY - g.y - g.listContainer_mc.y - al.y - al.container_mc.y - al.containerContent_mc.y)
        if not row: return
        cur = lst.getCurrentMovieClip()
        if cur and cur != g: cur.clear()
        lst.selectMC(g, False)
        al.selectMC(row, False)
        pan.delegateDown('tc', 0)
        this.tcEv = this.tcPM(row)
        return
    if p == 6:
        lst = pan.skillList
        this.tcSL = lst
        row = this.tcRow(lst)
        if not row: return
        lst.selectMC(row, False)
        pan.delegateDown('tc')
        this.tcBtn = row
        this.tcEv = 'IE UIAccept'
        return
    if p == 7 or p == 8:
        tp = pan.talents_mc
        if p == 8: tp = pan.tags_mc
        if not tp: return
        lst = tp.contentList
        this.tcSL = lst
        row = this.tcRow(lst)
        if not row:
            lst = tp.racialList
            row = this.tcRow(lst)
        if not row: return
        cur = tp.currentList
        if cur and cur != lst: cur.clearSelection()
        tp.currentList = lst
        lst.selectMC(row, False)
        tp.delegateDown('tc')
        this.tcBtn = row
        if lst == tp.contentList: this.tcEv = 'IE UIAccept'
        return
    if p == 9:
        lst = pan.instrumentList
        row = this.tcRow(lst)
        if not row: return
        lst.selectMC(row, False)
        this.tcBtn = row
        this.tcEv = 'IE UIAccept'
        return

def tcDown(e):
    this.tcLast = getTimer()
    this.tcSeen = True
    this.tcIsDown = True
    this.tcDrag = False
    this.tcX0 = this.mouseX
    this.tcY0 = this.mouseY
    this.tcEv = None
    this.tcSL = None
    b = this.tcBtn
    if b: b.alpha = 1
    this.tcBtn = None
    if not this.visible: return
    if this.waiting_mc and this.waiting_mc.visible: return
    if this.letterT_mc and this.letterT_mc.visible: return
    this.tcFind()
    sl = this.tcSL
    if sl:
        this.tcSM = sl.mouseY
        this.tcSY = sl.m_scrollbar_mc.scrolledY
    b = this.tcBtn
    if b: b.alpha = 6 / 10

def tcMove(e):
    if this.tcSeen: this.tcLast = getTimer()
    if not this.tcIsDown: return
    sl = this.tcSL
    if not sl: return
    d = sl.mouseY - this.tcSM
    if not this.tcDrag:
        if d > 14 or d < -14:
            this.tcDrag = True
            b = this.tcBtn
            if b: b.alpha = 1
    if this.tcDrag: sl.m_scrollbar_mc.scrollTo(this.tcSY - d, False, False)

def tcUp(e):
    now = getTimer()
    this.tcLast = now
    dn = this.tcIsDown
    this.tcIsDown = False
    b = this.tcBtn
    this.tcBtn = None
    if b: b.alpha = 1
    ev = this.tcEv
    this.tcEv = None
    this.tcSL = None
    if not dn: return
    if not ev: return
    if this.tcDrag: return
    d = this.mouseX - this.tcX0
    if d > 30 or d < -30: return
    d = this.mouseY - this.tcY0
    if d > 30 or d < -30: return
    if now - this.tcFired < DEBOUNCE: return
    this.tcFired = now
    this.tcPad(ev)

def tcPad(ev):
    # the pad path itself: the original onEventDown + onEventUp with the index of that input event
    a = this.events
    if not a: return
    n = a.length
    i = 0
    while i < n:
        if a[i] == ev:
            this.tuEvDownOrig(i, 0)
            this.tuEvUpOrig(i, 0)
            return
        i = i + 1

def onEventDown(i, j):
    this.tcHook()
    if this.events[i] == 'IE UIAccept':
        t = getTimer()
        if t - this.tcFired < DEBOUNCE: return True
        this.tcFired = t
    return this.tcEvDownPrev(i, j)

def onEventUp(i, j):
    this.tcHook()
    return this.tcEvUpPrev(i, j)

def onEventInit():
    this.tcInitPrev()
    this.tcHook()
'''
NEW = [n for n in re.findall(r'^def (\w+)', CODE, re.M) if n not in WRAP]


def cc_compile(abc, ns, late, env):
    """CODE -> {name: (number of parameters, assembler lines)}. env: global name -> ('lex', multiname) | ('call', multiname) | ('const', int)"""
    P = lambda name: abc.qname(ns, name)
    S = abc.string
    out = {}
    for fn in ast.parse(textwrap.dedent(CODE)).body:
        params = [a.arg for a in fn.args.args]
        reg = {n: i + 1 for i, n in enumerate(params)}
        for node in ast.walk(fn):
            if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store) and node.id not in reg: reg[node.id] = len(reg) + 1
        L = [('getlocal0',), ('pushscope',)]
        for n, r in reg.items():
            if n not in params: L += [('pushnull',), ('coerce_a',), ('setlocal', r)]
        nlab = [0]
        def lab():
            nlab[0] += 1; return '%s_%d' % (fn.name, nlab[0])

        def expr(e):
            if isinstance(e, ast.Constant):
                v = e.value
                if v is None: L.append(('pushnull',))
                elif v is True: L.append(('pushtrue',))
                elif v is False: L.append(('pushfalse',))
                elif isinstance(v, str): L.append(('pushstring', S(v)))
                elif isinstance(v, int) and -128 <= v < 128: L.append(('pushbyte', v))
                elif isinstance(v, int) and 0 <= v < 32768: L.append(('pushshort', v))
                else: raise ValueError('constant %r' % (v,))
            elif isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.USub) and isinstance(e.operand, ast.Constant): expr(ast.Constant(-e.operand.value))
            elif isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.Not): expr(e.operand); L.append(('not',))
            elif isinstance(e, ast.Name):
                if e.id == 'this': L.append(('getlocal0',))
                elif e.id in reg: L.append(('getlocal', reg[e.id]))
                elif e.id in env and env[e.id][0] == 'lex': L.append(('getlex', env[e.id][1]))
                elif e.id in env and env[e.id][0] == 'const': expr(ast.Constant(env[e.id][1]))
                else: raise ValueError('name %s in %s' % (e.id, fn.name))
            elif isinstance(e, ast.Attribute): expr(e.value); L.append(('getproperty', P(e.attr)))
            elif isinstance(e, ast.Subscript): expr(e.value); expr(e.slice); L.append(('getproperty', late))
            elif isinstance(e, ast.BinOp):
                expr(e.left); expr(e.right)
                L.append(({ast.Add: 'add', ast.Sub: 'subtract', ast.Mult: 'multiply', ast.Div: 'divide'}[type(e.op)],))
            elif isinstance(e, ast.Compare):
                assert len(e.ops) == 1
                expr(e.left); expr(e.comparators[0]); op = type(e.ops[0])
                L.append(({ast.Lt: 'lessthan', ast.LtE: 'lessequals', ast.Gt: 'greaterthan', ast.GtE: 'greaterequals', ast.Eq: 'equals', ast.NotEq: 'equals'}[op],))
                if op is ast.NotEq: L.append(('not',))
            elif isinstance(e, ast.Call): call(e, False)
            else: raise ValueError(ast.dump(e))

        def call(e, void):
            f = e.func
            if isinstance(f, ast.Name):
                kind, mn = env[f.id]; assert kind == 'call'
                L.append(('findpropstrict', mn))
                for a in e.args: expr(a)
                L.append(('callproperty', mn, len(e.args)))
                if void: L.append(('pop',))
            else:
                expr(f.value)
                for a in e.args: expr(a)
                L.append(('callpropvoid' if void else 'callproperty', P(f.attr), len(e.args)))

        def jfalse(e, target):
            if isinstance(e, ast.BoolOp) and isinstance(e.op, ast.And):
                for v in e.values: jfalse(v, target)
            elif isinstance(e, ast.BoolOp) and isinstance(e.op, ast.Or):
                yes = lab()
                for v in e.values[:-1]: jtrue(v, yes)
                jfalse(e.values[-1], target); L.append(('L', yes))
            elif isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.Not): jtrue(e.operand, target)
            else: expr(e); L.append(('iffalse', target))

        def jtrue(e, target):
            if isinstance(e, ast.BoolOp) and isinstance(e.op, ast.Or):
                for v in e.values: jtrue(v, target)
            elif isinstance(e, ast.BoolOp) and isinstance(e.op, ast.And):
                no = lab()
                for v in e.values[:-1]: jfalse(v, no)
                jtrue(e.values[-1], target); L.append(('L', no))
            elif isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.Not): jfalse(e.operand, target)
            else: expr(e); L.append(('iftrue', target))

        def block(body):
            for s in body:
                if isinstance(s, ast.Assign):
                    t, = s.targets
                    if isinstance(t, ast.Name): expr(s.value); L.extend([('coerce_a',), ('setlocal', reg[t.id])])
                    elif isinstance(t, ast.Attribute): expr(t.value); expr(s.value); L.append(('setproperty', P(t.attr)))
                    else: raise ValueError(ast.dump(t))
                elif isinstance(s, ast.Expr):
                    if isinstance(s.value, ast.Constant): continue
                    assert isinstance(s.value, ast.Call); call(s.value, True)
                elif isinstance(s, ast.Return):
                    if s.value is None: L.append(('returnvoid',))
                    else: expr(s.value); L.append(('returnvalue',))
                elif isinstance(s, ast.If):
                    no = lab(); jfalse(s.test, no); block(s.body)
                    if s.orelse:
                        end = lab(); L.append(('jump', end)); L.append(('L', no)); block(s.orelse); L.append(('L', end))
                    else: L.append(('L', no))
                elif isinstance(s, ast.While):
                    top, cond = lab(), lab()
                    L.append(('jump', cond)); L.append(('L', top)); L.append(('label',)); block(s.body); L.append(('L', cond)); jtrue(s.test, top)
                else: raise ValueError(ast.dump(s))

        block(fn.body)
        L.append(('returnvoid',))
        out[fn.name] = (len(params), L)
    return out


def source(path):
    data = open(path, 'rb').read()
    if data[:3] in (b'CWS', b'FWS', b'ZWS'): return data
    import lspk
    pak = lspk.Pak(path); e = [e for e in pak.entries if e[0] == SRC]
    if not e: sys.exit('%s: no %s' % (path, SRC))
    return pak.read(e[0])


def patch(data):
    """characterCreation_c.swf bytes -> (characterCreation_d.swf bytes, report dict)"""
    mid = dos2_hint_touch.patch(data)[0]                         # layer 1: hints + the 400 ms accept guard, originals kept as tuEvDownOrig / tuEvUpOrig
    swf = Swf(mid); (tag, pre, abc), = swf.abcs()
    ci = abc.class_index(CLS); inst = abc.instances[ci]
    assert not inst['flags'] & 1, 'the class is sealed: dynamic properties would not work'
    for n in ('tuEvDownOrig', 'tuEvUpOrig'): assert abc.trait(ci, n) is not None, 'layer 1 did not keep %s' % n
    tr = {n: abc.trait(ci, n) for n in WRAP}
    for n, t in tr.items(): assert t is not None and t.kind & 15 == 1, n
    for n in NEW + list(WRAP.values()): assert abc.trait(ci, n) is None, 'already patched'
    ns = abc.mn[tr['onEventDown'].name][1]; assert abc.ns[ns] == (NS_PUBLIC, abc.string('')), 'onEventDown is not public'
    P = lambda name: abc.qname(ns, name)
    late = [ops[0] for _, op, ops, _ in abc.ins(abc.methods[abc.trait(ci, 'tuEvDownOrig').b].body.code) if OPS[op][0] == 'getproperty' and abc.mn[ops[0]][0] == MN_MULTIL][0]
    env = {'EI': ('lex', abc.pub('ExternalInterface', 'flash.external')), 'getTimer': ('call', abc.pub('getTimer', 'flash.utils')), 'GUARD': ('const', GUARD_MS), 'DEBOUNCE': ('const', DEBOUNCE_MS)}
    ref = abc.methods[abc.trait(ci, 'tuEvDownOrig').b].body      # scope depths of an instance method of this class
    made = {}
    for name, (nparams, lines) in cc_compile(abc, ns, late, env).items():
        m = abc.add_method('tc_' + name, [0] * nparams, 0, asm(lines), 64, 64, ref.init_scope, ref.init_scope + 1)
        b = abc.methods[m].body; st, sc, loc = abc.verify(b); b.max_stack = st; b.local_count = loc; abc.verify(b)
        made[name] = m
    orig = {}
    for n, keep in WRAP.items():
        like = abc.methods[tr[n].b]; new = abc.methods[made[n]]
        assert len(like.params) == len(new.params), n
        new.params = list(like.params)                           # same parameter types as the method whose name it takes
        if like.flags & 0x08: new.flags |= 0x08; new.options = list(like.options)
        orig[n] = tr[n].b; tr[n].b = made[n]
        inst['traits'].append(Trait(P(keep), 1, 0, orig[n]))
    for n in NEW: inst['traits'].append(Trait(P(n), 1, 0, made[n]))
    swf.tags[tag][1] = pre + abc.write()
    return swf.write(), {'methods': made, 'prev': orig, 'mid': mid}


def ei_calls(abc):
    """ExternalInterface.call names: {name: set of method indices that make the call}"""
    out = {}
    for b in abc.bodies:
        ins = abc.ins(b.code)
        for k, (_, op, ops, _) in enumerate(ins):
            if OPS[op][0] == 'getlex' and abc.mname(ops[0]).endswith('ExternalInterface') and k + 1 < len(ins) and OPS[ins[k + 1][1]][0] == 'pushstring':
                out.setdefault(abc.S(ins[k + 1][2][0]), set()).add(b.method)
    return out


def check(src, built=None):
    """static verification; returns the number of failures"""
    bad = 0
    def say(ok, text):
        nonlocal bad; bad += not ok; print('%-4s %s' % ('ok' if ok else 'FAIL', text))
    bad += dos2_hint_touch.check('characterCreation_c.swf', src)             # layer 1 on its own
    out, rep = patch(src)
    if built is not None: say(built == out, 'the built movie is exactly what the patcher makes from this source (%d B, sha256 %s)' % (len(built), hashlib.sha256(built).hexdigest()[:16]))
    a, m, b = Swf(src), Swf(rep['mid']), Swf(out); (ta, pa, A), = a.abcs(); (tm, pm, M), = m.abcs(); (tb, pb, B), = b.abcs()
    nnew = len(rep['methods'])
    say(b.sig == b'CWS' and b.ver == a.ver and b.head == a.head, 'SWF header: version %d, frame rectangle / rate unchanged' % b.ver)
    say(len(a.tags) == len(b.tags) and all(x[0] == y[0] and (n == ta or x[1] == y[1]) for n, (x, y) in enumerate(zip(a.tags, b.tags))),
        'every tag but DoABC #%d is byte-identical to characterCreation_c (%d tags: shapes, sprites, fonts, SymbolClass = the console look)' % (ta, len(a.tags)))
    say(Swf(Swf(out).write()).body() == b.body() and B.write() == b.tags[tb][1][len(pb):], 'the patched movie parses and round-trips (SWF and ABC rewritten byte-identical)')
    say(B.strings[:len(A.strings)] == A.strings and B.ns[:len(A.ns)] == A.ns and B.nssets == A.nssets and B.mn[:len(A.mn)] == A.mn and B.ints == A.ints
        and B.uints == A.uints and B.doubles == A.doubles, 'constant pool only appended: +%d strings, +%d namespaces, +%d multinames (no int / double added)' % (
            len(B.strings) - len(A.strings), len(B.ns) - len(A.ns), len(B.mn) - len(A.mn)))
    same = lambda X, Y: all(x.code == y.code and (x.max_stack, x.local_count, x.init_scope, x.max_scope, x.exc) == (y.max_stack, y.local_count, y.init_scope, y.max_scope, y.exc)
                            and x.method == y.method for x, y in zip(X.bodies, Y.bodies))
    say(same(A, B) and same(M, B) and len(B.bodies) == len(M.bodies) + nnew and len(B.methods) == len(M.methods) + nnew,
        'all %d original method bodies byte-identical (and the %d of the hint layer); %d methods appended by this layer' % (len(A.bodies), len(M.bodies) - len(A.bodies), nnew))
    oa, om, ob = A.owners(), M.owners(), B.owners(); ia, ib = {v: k for k, v in oa.items()}, {v: k for k, v in ob.items()}
    say(all(ia[n] == ib[n] for n in PAD_PATH), 'pad path still bound to its original methods: ' + ', '.join(sorted({n.split('::')[1] for n in PAD_PATH})))
    say(ib[CLS + '::tuEvDownOrig'] == ia[CLS + '::onEventDown'] and ib[CLS + '::tuEvUpOrig'] == ia[CLS + '::onEventUp'],
        'what a tap runs (tuEvDownOrig / tuEvUpOrig) = the original onEventDown / onEventUp bodies of characterCreation_c')
    moved = sorted('%s -> %s' % (om[k].split('::')[-1], ob[k].split('::')[-1]) for k in om if ob.get(k) != om[k])
    say(moved == sorted('%s -> %s' % kv for kv in WRAP.items()), 'renamed over the hint layer: ' + ', '.join(moved))
    ci = B.class_index(CLS)
    say(all(c1 == c2 for n, (c1, c2) in enumerate(zip(M.instances, B.instances)) if n != ci) and M.classes == B.classes and M.scripts == B.scripts
        and len(B.instances[ci]['traits']) == len(M.instances[ci]['traits']) + nnew and all(t.kind & 15 == 1 for t in B.instances[ci]['traits'][len(M.instances[ci]['traits']):]),
        'only class %s changes: +%d method traits, no slot, flags %#x (not sealed)' % (CLS, nnew, B.instances[ci]['flags']))
    # contract: nothing the engine calls or the movie calls is gone
    ra = {(B.short(t.name), t.kind & 15) for t in A.instances[A.class_index(CLS)]['traits']}; rb = {(B.short(t.name), t.kind & 15) for t in B.instances[ci]['traits']}
    ea, eb = ei_calls(A), ei_calls(B)
    say(ra <= rb and all(len(A.methods[ia[CLS + '::' + n]].params) == len(B.methods[ib[CLS + '::' + n]].params) for n, k in ra if k == 1),
        'contract: all %d root methods / slots of characterCreation_c are in _d with the same parameter counts (+%d: %s)' % (len(ra), len(rb - ra), ' '.join(sorted(n for n, _ in rb - ra))))
    say(all(n in eb and ea[n] <= eb[n] for n in ea), 'contract: all %d ExternalInterface names of characterCreation_c are still called from the same method bodies; added: %s' % (
        len(ea), ', '.join(sorted(set(eb) - set(ea))) or 'none'))
    say(all(n in ea for n in EI_TAPS), 'every action a tap can end in is an ExternalInterface call of the original movie: ' + ' '.join(EI_TAPS))
    errs = verify_all(B)
    for who, why in errs: print('       %s: %s' % (who, why))
    say(not errs, 'ABC verifies: %d method bodies (stack depth at every join, scope, locals, branch targets on instruction starts, constant indices)' % len(B.bodies))
    for n in sorted(rep['methods']):
        bd = B.methods[rep['methods'][n]].body; st, sc, loc = B.verify(bd)
        say((st, loc) == (bd.max_stack, bd.local_count) and bd.init_scope == 10 and bd.max_scope == 11, '  %-12s %4d B  max_stack %d  locals %2d  scope %d..%d (exact)' % (
            n, len(bd.code), bd.max_stack, bd.local_count, bd.init_scope, bd.max_scope))
    dis = lambda n: '\n'.join(B.dis(B.methods[ib[CLS + '::' + n]].body))
    # every property / method name the touch code uses on the movie's objects exists in the original movie (a typo would be a silent no-op)
    known = {A.S(mn[2]) for mn in A.mn[1:] if mn[0] in (7, 0x0d)} | {A.S(mn[1]) for mn in A.mn[1:] if mn[0] in (9, 0x0e)}
    used = set()
    for n in rep['methods']:
        for _, op, ops, _ in B.ins(B.methods[rep['methods'][n]].body.code):
            if 'm' in OPS[op][1] and B.mn[ops[0]][0] == 7: used.add(B.S(B.mn[ops[0]][2]))
    own_names = set(NEW) | set(WRAP.values()) | {'tuEvDownOrig', 'tuEvUpOrig'} | {x for x in used if x.startswith('tc')}
    unknown = sorted(used - known - own_names)
    say(not unknown, 'every name the touch code reads / calls is a name of the original movie (%d names); not in it: %s' % (len(used - own_names), ' '.join(unknown) or 'none'))
    say(all(("'%s'" % e) in dis('tcHook') for e in ('mouseMove', 'mouseDown', 'mouseUp')) and 'tcHooked' in dis('tcHook'), 'stage mouseDown / mouseMove / mouseUp listeners, added once (tcHooked)')
    say('tuEvDownOrig' in dis('tcPad') and 'tuEvUpOrig' in dis('tcPad') and 'tcPad' in dis('tcUp') and 'ExternalInterface' not in dis('tcUp'),
        'a release runs the pad path: tcPad(event) = original onEventDown(i) + onEventUp(i); no ExternalInterface call of its own')
    g = 'pushshort        %d' % DEBOUNCE_MS
    say("'IE UIAccept'" in dis('onEventDown') and g in dis('onEventDown') and 'tcFired' in dis('onEventDown') and all(WRAP[n] in dis(n) for n in ('onEventDown', 'onEventUp'))
        and 'tcFired' in dis('tcUp') and g in dis('tcUp'),
        'no double fire: an accept or tap within %d ms of the last one (tcFired) does nothing; no guard after mouse events (pack 3f)' % DEBOUNCE_MS)
    say('scrollTo' in dis('tcMove') and 'tcDrag' in dis('tcMove') and 'tcDrag' in dis('tcUp'), 'drag: scrollList.m_scrollbar_mc.scrollTo while the finger moves; a drag is not a tap')
    evs = sorted(set(re.findall(r"'(IE \w+)'", dis('tcFind') + dis('tcPM') + dis('tcStep'))))
    A_ev = {A.S(i) for i in range(1, len(A.strings))}
    say(all(e in A_ev for e in evs), 'input events a tap sends (all in the movie\'s own `events`): ' + ' '.join(e[3:] for e in evs))
    print('%s: %d B -> %s: %d B (ABC %d -> %d B), sha256 %s' % (SRC.split('/')[-1], len(src), MOVIE, len(out), len(a.tags[ta][1]), len(b.tags[tb][1]), hashlib.sha256(out).hexdigest()))
    return bad


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('build', 'check') or (a[0] == 'build' and len(a) < 3): sys.exit(__doc__)
    src = source(a[1])
    if a[0] == 'build':
        out, rep = patch(src); open(a[2], 'wb').write(out)
        print('%s: %d B, sha256 %s' % (a[2], len(out), hashlib.sha256(out).hexdigest()))
        return
    bad = check(src, open(a[2], 'rb').read() if len(a) > 2 else None)
    print('FAILED: %d' % bad if bad else 'characterCreation_d: all static checks pass')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
