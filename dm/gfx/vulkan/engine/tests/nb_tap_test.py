# [DOS2] NB-04: every NVN entry point the NB frame IR depends on has its tap (docs/dos2/NATIVE_BACKEND_PLAN.md §1 TAP / NB-04).
# A source check, not a run: the tap only works inside the game. An entry point counts as tapped when its body has an NBT(...) line or calls a
# helper whose body has one (bind_handles / bind_handles_n / bind_state / wt_register / h_nvn*Downsample*). New entry points of the guarded
# families (Draw*, Clear*, Copy*, Dispatch*, Bind texture / image, Downsample*) fail the check until they get a tap.
# NB-09: also the OP_X_NB_DRAW capture (see the block at the end). NB-13a: the state shadow hooks. NB-15: the DM_NB_FOREIGN counter hooks.
# NB-21: the frame job and the hand-off of the translation queue (DM_NB_OWN; the block before the verdict, with mutations of hle_nvn.c).
# Run: python dm/gfx/vulkan/engine/tests/nb_tap_test.py
from pathlib import Path
import re, sys

root = Path(__file__).resolve().parents[4]
src = (root / 'gfx/nvn/hle_nvn.c').read_text(encoding='utf-8')
inc = (root / 'gfx/nvn/nvn_nb_tap.inc').read_text(encoding='utf-8')

def bodies(text):
    """name -> body of every NVN(name, ...) { ... } and static function in text (brace matched, comments / strings stripped)."""
    clean = re.sub(r'/\*.*?\*/|//[^\n]*', ' ', text, flags=re.S)
    clean = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', clean)
    clean = re.sub(r"'(?:\\.|[^'\\\n])'", "' '", clean)
    out = {}
    for m in re.finditer(r'^(?:NVN\((\w+),|static [^;{()]*?\b(\w+)\s*\()', clean, flags=re.M):
        name = m.group(1) or m.group(2)
        i = clean.find('{', m.end())
        semi = clean.find(';', m.end())
        if i < 0 or (0 <= semi < i and not m.group(1)): continue   # a prototype
        depth, j = 0, i
        while j < len(clean):
            if clean[j] == '{': depth += 1
            elif clean[j] == '}':
                depth -= 1
                if depth == 0: break
            j += 1
        out.setdefault(name, clean[i:j + 1])
    return out

b = bodies(src)
helpers = {n for n, body in b.items() if not n.startswith('nvn') and 'NBT(' in body}
def tapped(name, seen=()):
    body = b.get(name)
    if body is None: return None
    if 'NBT(' in body: return True
    if len(seen) >= 3: return False
    for h in re.findall(r'\b(\w+)\s*\(', body):
        if h in helpers: return True
        nxt = h[2:] if h.startswith('h_nvn') else h   # h_nvnX = the entry point X called directly; else a static helper
        if nxt != name and nxt not in seen and nxt in b and tapped(nxt, seen + (name,)): return True
    return False

required = ['nvnCommandBuffer' + n for n in (
    'SetRenderTargets ClearColor ClearColori ClearColorui ClearDepthStencil ClearTexture ClearTexturei ClearTextureui ClearBuffer '
    'CopyBufferToBuffer CopyBufferToTexture CopyTextureToBuffer CopyTextureToTexture Downsample DownsampleTextureView TiledDownsample '
    'TiledDownsampleTextureView BindTexture BindTextures BindSeparateTexture BindSeparateTextures BindImage BindImages BindProgram '
    'BindBlendState BindColorState BindChannelMaskState BindMultisampleState BindPolygonState BindDepthStencilState BindVertexAttribState '
    'BindVertexStreamState BindVertexBuffer BindVertexBuffers BindUniformBuffer BindUniformBuffers BindStorageBuffer BindStorageBuffers '
    'DrawArrays DrawArraysInstanced DrawElements DrawElementsBaseVertex DrawElementsInstanced DrawArraysIndirect DrawElementsIndirect '
    'MultiDrawArraysIndirectCount MultiDrawElementsIndirectCount DrawTexture DispatchCompute DispatchComputeIndirect Barrier ReportCounter '
    'SetScissor EndRecording').split()] + ['nvnQueueSubmitCommands', 'nvnQueuePresentTexture', 'nvnTexturePoolRegisterTexture',
    'nvnTexturePoolRegisterImage']
family = re.compile(r'^nvnCommandBuffer(Draw|MultiDraw|Clear|Copy(?!Commands)|Dispatch|Downsample|TiledDownsample|Bind(Texture|Image|SeparateTexture)|SetRenderTargets)')
names = sorted(set(required) | {n for n in b if family.match(n)})
bad = []
for n in names:
    t = tapped(n)
    if t is None: bad.append(n + ': entry point not found')
    elif not t: bad.append(n + ': no NBT(...) tap')
# The tap must stay inert when DM_NB is off and outside DOS2 builds.
if '#define NBT(x) do { if (__builtin_expect(g_dm_nb != 0, 0))' not in inc: bad.append('nvn_nb_tap.inc: NBT is not gated on g_dm_nb')
if not re.search(r'#else\s*\n#define NBT\(x\) \(\(void\)0\)', inc): bad.append('nvn_nb_tap.inc: non-DOS2 NBT is not a no-op')
if '#include "nvn_nb_tap.inc"' not in src: bad.append('hle_nvn.c does not include nvn_nb_tap.inc')
# A tap never replaces the record: each NBT line in an entry point is followed by the original REC / rec / RECN / cmd_alloc / x_push.
for n in names:
    body = b.get(n) or ''
    if 'NBT(' in body and n.startswith('nvnCommandBuffer') and not re.search(r'\b(REC|RECN|rec|cmd_alloc|bind_handles\w*|bind_state|h_nvn\w+)\s*\(', body):
        bad.append(n + ': tap without the NVN record')
# NB-05: every tap function that changes the IR's passes writes the NB records (nbt_sync) after the IR call; the records bypass the
# DM_NATIVE_DRAW buffer (back-patched by address); the backend knows every OP_X_NB_* record and the front end patches before the queue.
ib = bodies(inc)
for n, body in ib.items():
    if n.startswith('nbt_') and n not in ('nbt_sync', 'nbt_present') and re.search(r'\bnb_(pass_begin|clear|op|draw)\s*\(', body):
        last = max(body.rfind(k) for k in ('nb_pass_begin(', 'nb_clear(', 'nb_op(', 'nb_draw('))
        if body.find('nbt_sync(', last) < 0: bad.append(n + ': changes the IR without nbt_sync after it (NB records)')
if not re.search(r'nb_rec_alloc[^{;]*\{[^}]*nd_flush\(\)', src, flags=re.S): bad.append('hle_nvn.c nb_rec_alloc: does not bypass the DM_NATIVE_DRAW buffer')
eng = (root / 'gfx/vulkan/engine/xbg_game.cpp').read_text(encoding='utf-8')
for op in ('OP_X_NB_FRAME', 'OP_X_NB_PASS', 'OP_X_NB_PASS_END'):
    if 'case %s:' % op not in eng: bad.append('xbg_game.cpp execute: no case ' + op)
fe = (root / 'host/hle_dos2_nb.c').read_text(encoding='utf-8')
m = re.search(r'void dm_nb_submit\(void\) \{(.*?)\n\}', fe, flags=re.S)
if not m or m.group(1).count('nb_ir_plan();') < 2: bad.append('hle_dos2_nb.c: dm_nb_submit does not plan / patch before the queue')
# NB-09 (DM_NB_OPS 36 / 37): the modelled calls go to the draw's shadow only AFTER the NB-04 tap saw them, and keep their NVN record as
# the fallback; any other record inside the bracket flushes the shadow first (cmd_alloc, before the DM_NATIVE_DRAW buffer); NB records and
# the OP_X_NATIVE_DRAW wrapper are not 'foreign'; wt_scan reads the record's binds; the backend unpacks it through the very helpers the
# bind cases of execute use; outside DOS2 the capture is a constant 0.
for n in ['nvnCommandBuffer' + x for x in 'DrawArrays DrawElements DrawElementsBaseVertex DrawElementsInstanced BindUniformBuffer BindUniformBuffers'.split()] + ['bind_handles']:
    body = b.get(n) or ''
    i, j = body.find('NBT('), body.find('NBD_TAKE(c)')
    if j < 0: bad.append(n + ': no NB-09 capture (NBD_TAKE)')
    elif i < 0 or j < i: bad.append(n + ': NB-09 capture before the NB-04 tap')
    elif not re.search(r'\b(REC|RECN)\s*\(', body[j:]): bad.append(n + ': NB-09 capture without the NVN record as fallback')
ca = b.get('cmd_alloc') or ''
if 'NBD_FOREIGN(o)' not in ca or ca.find('NBD_FOREIGN(o)') > ca.find('t_nd.mode'): bad.append('hle_nvn.c cmd_alloc: no NBD_FOREIGN before the DM_NATIVE_DRAW buffer')
for n in ('nb_rec_alloc', 'nd_flush'):
    if not re.search(r'NBD_QUIET\(p = cmd_alloc\(', b.get(n) or ''): bad.append('hle_nvn.c %s: its record is counted as foreign (no NBD_QUIET)' % n)
if 'OP_X_NB_DRAW' not in (b.get('wt_scan') or ''): bad.append('hle_nvn.c wt_scan: OP_X_NB_DRAW binds not scanned')
if '#include "nvn_nb_draw.inc"' not in src or src.find('#include "nvn_nb_draw.inc"') < src.find('#define RECN('): bad.append('hle_nvn.c: nvn_nb_draw.inc not included after RECN')
if not re.search(r'#else\s*\n(?:#define [^\n]*\n)*#define NBD_TAKE\(c\) 0\n', inc): bad.append('nvn_nb_tap.inc: non-DOS2 NBD_TAKE is not 0')
dinc = (root / 'gfx/nvn/nvn_nb_draw.inc').read_text(encoding='utf-8')
if not ('REC(c, OP_BIND_UBO, p_bind_buf)' in dinc and 'RECN(c, op, p_bind_handles, h, 1)' in dinc): bad.append('nvn_nb_draw.inc: the flush does not write the replaced bind records')
if 'case OP_X_NB_DRAW:nbdraw::exec(p,n)' not in eng: bad.append('xbg_game.cpp execute: no case OP_X_NB_DRAW')
if 'case OP_BIND_UBO:case OP_BIND_SSBO:exec_bind_buf(' not in eng or 'case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:exec_bind_handles(' not in eng:
    bad.append('xbg_game.cpp execute: the bind cases do not use exec_bind_buf / exec_bind_handles (the NB-09 unpack would drift)')
nbinc = (root / 'gfx/vulkan/engine/xbg_nb.inc').read_text(encoding='utf-8')
m = re.search(r'namespace nbdraw\{\r?\n(.*?)\n\}', nbinc, flags=re.S)   # the body (not the forward declaration of tick)
if not m or 'exec_bind_buf(' not in m.group(1) or 'exec_bind_handles(' not in m.group(1) or 'nbd_unpack(' not in m.group(1): bad.append('xbg_nb.inc nbdraw: not the faithful unpack (DM_NB_DRAW_UNPACK=1)')
# NB-10: the default path applies the record (nbd_view + nbd_apply, one generation bump per stage); NBD_F_CHECK records are compared only
if not m or 'nbd_view(' not in m.group(1) or 'nbd_apply(' not in m.group(1) or 'nbd_compare(' not in m.group(1) or 'NBD_F_CHECK' not in m.group(1):
    bad.append('xbg_nb.inc nbdraw: no NB-10 apply / check')
if eng.count('if(nbdraw::pend)nbdraw::seen(') != 2: bad.append('xbg_game.cpp execute: OP_DRAW / OP_DRAW_IDX do not feed the DM_NB_OPS_CHECK draw comparison')
if 'nvn_nb_draw_check(chk)' not in fe or 'DM_NB_OPS_CHECK' not in fe: bad.append('hle_dos2_nb.c: DM_NB_OPS_CHECK not passed to the tap')
if not ('t_nbd_mirror = 1; return 0;' in dinc and 'if (t_nbd_mirror) { t_nbd_mirror = 0; return; }' in dinc and 'h->flags |= NBD_F_CHECK' in dinc):
    bad.append('nvn_nb_draw.inc: DM_NB_OPS_CHECK does not write both the old records and a check-only record')
if 'dn_install_draw(&k_nb_draw)' not in fe: bad.append('hle_dos2_nb.c: DM_NB_OPS does not register the draw bracket')
# NB-13a (DM_NB_OPS state bits): the state shadow hooks sit where every record passes: rec() captures first (before cmd_alloc) and marks a
# plain state record after it is allocated; cmd_alloc writes the pending state before the NB-09 foreign flush and before the DM_NATIVE_DRAW
# buffer; EndRecording ends the epoch after its OP_END; the shadow's own records are not foreign (NBD_QUIET); every state entry point goes
# through rec() (REC / RECN / bind_state); the backend executes OP_X_NB_STATE blocks with execute() itself; the FE brackets every packet.
rc = b.get('rec') or ''
if not (0 <= rc.find('NBS_REC(c, op, payload)') < rc.find('cmd_alloc(')): bad.append('hle_nvn.c rec: no NBS_REC before cmd_alloc')
if not (0 <= rc.find('cmd_alloc(') < rc.find('NBS_RAW(op, p + 8')): bad.append('hle_nvn.c rec: no NBS_RAW after cmd_alloc')
if not (0 <= ca.find('NBS_BEFORE(o)') < ca.find('NBD_FOREIGN(o)')): bad.append('hle_nvn.c cmd_alloc: NBS_BEFORE missing or after NBD_FOREIGN')
er = b.get('nvnCommandBufferEndRecording') or ''
if not (0 <= er.find('cmd_alloc(') < er.find('NBS_END(o)')): bad.append('hle_nvn.c EndRecording: no NBS_END after its OP_END')
for n in ['nvnCommandBuffer' + x for x in ('SetViewports SetScissor SetScissors SetDepthRange SetDepthRanges BindProgram BindVertexAttribState '
          'BindVertexStreamState BindVertexBuffers BindVertexBuffer SetStencilValueMask SetStencilMask SetStencilRef SetPolygonOffsetClamp '
          'SetBlendColor SetAlphaRef').split()] + ['bind_state']:
    body = b.get(n)
    if body is None: bad.append(n + ': state entry point not found'); continue
    if not re.search(r'\b(REC|RECN)\s*\(', body) or 'cmd_alloc(' in body: bad.append(n + ': state record not written through rec() (the NB-13a shadow would miss it)')
sinc = (root / 'gfx/nvn/nvn_nb_state.inc').read_text(encoding='utf-8')
if sinc.count('NBD_QUIET(') < 2: bad.append('nvn_nb_state.inc: the shadow records are not NBD_QUIET (they would flush an NB-09 draw)')
if not inc.rstrip().endswith('#include "nvn_nb_state.inc"   // [DOS2] DM_NB_OPS state bits (NB-13a): the state shadow, OP_X_NB_STATE (NBS_* hooks; empty outside DOS2)\n'
                             '#include "nvn_nb_key.inc"     // [DOS2] DM_NB_RELOC (NB-14): the key frame of every IR pass, OP_X_NB_KEY (empty outside DOS2)\n'
                             '#include "nvn_nb_order.inc"   // [DOS2] DM_NB_ORDER (NB-17a): the order tail of OP_X_NB_FRAME (empty outside DOS2)\n'
                             '#include "nvn_nb_own.inc"     // [DOS2] DM_NB_OWN (NB-21): the frame job handed to the backend with its submit (empty outside DOS2)'):
    bad.append('nvn_nb_tap.inc: nvn_nb_state.inc + nvn_nb_key.inc + nvn_nb_order.inc + nvn_nb_own.inc not included at the end (in this order)')
if 'case OP_X_NB_STATE:nbstate::exec(p,n,depth,budget)' not in eng or '#include "xbg_nb_state.inc"' not in eng: bad.append('xbg_game.cpp: no OP_X_NB_STATE case / include')
sbe = (root / 'gfx/vulkan/engine/xbg_nb_state.inc').read_text(encoding='utf-8')
if 'execute(c,depth+1,budget)' not in sbe or 'nbs_parse(' not in sbe: bad.append('xbg_nb_state.inc: blocks not executed by execute() after nbs_parse')
if 'nvn_nb_state_begin(' not in fe or 'nvn_nb_state_end()' not in fe or '{nb_rc_pre, nb_rc_post}' not in fe: bad.append('hle_dos2_nb.c: no state bracket on the packets')
# NB-11 (DM_NB_DESC): the reuse is sound only if every writer of the binding tables state.tex / samp / img / ub / sb moves tex_gen / buf_gen
# (dm/gfx/nb/nb_desc.h): direct element writes in the engine sources must have a bump on the same or the next line; the bind cases and the
# record apply bump; the decision runs around nbd_apply (pre before, post after, candidate cleared after the draw); descriptors() takes the
# entry only after the guard and commits on its hit / insert; the state reset drops the previous entry.
elem = r'\[(?:[^\[\]]|\[[^\[\]]*\])*\]'
wr = re.compile(r'state\.(?:tex|samp|img|ub|sb)' + elem + elem + r'(?:\.\w+)?\s*=(?!=)')
for f in sorted((root / 'gfx/vulkan/engine').glob('xbg_*')):
    if f.suffix not in ('.cpp', '.inc'): continue
    text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), f.read_text(encoding='utf-8'), flags=re.S)
    lines = [re.sub(r'//.*', '', l) for l in text.split('\n')]
    for i, l in enumerate(lines):
        if wr.search(l) and not re.search(r'\+\+(?:tex|buf)_gen\[', l + (lines[i + 1] if i + 1 < len(lines) else '')):
            bad.append('%s:%d: binding table written without a tex_gen / buf_gen bump (NB-11 DM_NB_DESC would reuse a stale set)' % (f.name, i + 1))
for fn in ('void exec_bind_buf(', 'void exec_bind_handles('):
    k = eng.find(fn); line = eng[k:eng.find('\n', k)]
    if k < 0 or not re.search(r'\+\+(?:tex|buf)_gen\[a\.stage\]', line): bad.append('xbg_game.cpp %s): no generation bump' % fn[5:-1])
ap = nbinc[nbinc.find('void apply('):nbinc.find('void unpack(')]
if not (0 <= ap.find('nb_desc_pre(') < ap.find('nbd_apply(') < ap.find('++tex_gen[') < ap.find('nb_desc_post(') < ap.find('do_draw(h);') < ap.find('nbdesc::cand=false;')):
    bad.append('xbg_nb.inc nbdraw::apply: NB-11 decision not around nbd_apply / the draw')
dk = eng.find('void descriptors(Descriptors &out'); de = eng.find('void descriptors_build(Descriptors &out,Commands &commands,bool compute){', dk); dsc = eng[dk:de]
if not (0 <= dsc.find('nbreuse=nbdesc::guard()') < dsc.find('hit=nbdesc::P.it') and dsc.count('nbdesc::commit(') == 2 and 'key=nbdesc::P.key' in dsc):
    bad.append('xbg_game.cpp descriptors(): NB-11 guard / reuse / commit / fallback key missing')
if 'state=State{};nbdesc::P.valid=false;' not in eng: bad.append('xbg_game.cpp: the state reset does not drop the NB-11 previous entry')
# NB-12 (DM_GFX_HIT_FAST): a hit-path image skips get_image's cpu_access only while the pending lists are as when it was checked:
# every statement that changes pending_writebacks / pending_reports (push_back / erase / clear / swap / move) bumps hit_pend_gen on that line.
hitinc = (root / 'gfx/vulkan/engine/xbg_nb_hit.inc').read_text(encoding='utf-8')
for i, l in enumerate(eng.split('\n')):
    if re.search(r'pending_(?:writebacks|reports)\.(?:push_back|emplace_back|erase|clear|swap|insert|resize|pop_back)\(|=std::move\(pending_(?:writebacks|reports)\)', l) and '++hit_pend_gen' not in l:
        bad.append('xbg_game.cpp:%d: pending list changed without ++hit_pend_gen (NB-12 DM_GFX_HIT_FAST would skip a needed cpu_access)' % (i + 1))
dsc2 = dsc[dsc.find('if(hit!=desc_memo.end()){DescMemo &m=hit->second;'):]
if not (0 <= dsc2.find('hitfast::before(ti,hfk,hfp))continue;') < dsc2.find('Image &im=get_image(ti.first,true);') < dsc2.find('hitfast::after(ti,im,hfk,hfp);')):
    bad.append('xbg_game.cpp descriptors(): NB-12 hit-fast hooks not around the hit-path get_image / prepare_read')
us = eng[eng.find('Buffer uniform_snapshot(uint64_t address,size_t copy){'):]
if not us.split('\n')[1].lstrip().startswith('if(nbubo::mode>0 && !nbubo::busy)'):
    bad.append('xbg_game.cpp uniform_snapshot: NB-12 nbubo::fast is not its first step')
if not (0 <= ap.find('nbd_apply(') < ap.find('nbubo::note(h,b)') < ap.find('do_draw(h);')):
    bad.append('xbg_nb.inc nbdraw::apply: NB-12 ring note not between nbd_apply and the draw')
if 'nb_ring_note(&ring,x.a,x.b,snapshot_epoch)' not in hitinc or 'nb_ring_find(&ring,address,copy,snapshot_epoch)' not in hitinc:
    bad.append('xbg_nb_hit.inc: NB-12 ring ranges not keyed by the submission (snapshot_epoch)')
# NB-15 (DM_NB_FOREIGN=1, default 0): count only. cmd_alloc files every record after the NB-13a / NB-09 flushes and before the DM_NATIVE_DRAW
# buffer; rec adds the draw records after cmd_alloc; every op a draw entry point writes is a draw for the counter; the counters never write a
# record; the walk checks an lr before it follows the saved fp; the front end reads the switch as "1" only and brackets every packet.
if not (0 <= ca.find('NBD_FOREIGN(o)') < ca.find('NBF_REC(o, size)') < ca.find('t_nd.mode')): bad.append('hle_nvn.c cmd_alloc: NBF_REC not after NBD_FOREIGN / before the DM_NATIVE_DRAW buffer')
if not (0 <= rc.find('cmd_alloc(') < rc.find('NBF_DRAW(op)')): bad.append('hle_nvn.c rec: no NBF_DRAW after cmd_alloc')
if '#define NBF_REC(o, size) do { if (__builtin_expect(g_nbf_on, 0))' not in inc or '#define NBF_DRAW(op) do { if (__builtin_expect(g_nbf_on, 0))' not in inc:
    bad.append('nvn_nb_tap.inc: NBF_REC / NBF_DRAW not gated on g_nbf_on')
if not re.search(r'#define NBF_REC\(o, size\) \(\(void\)0\)\n#define NBF_DRAW\(op\) \(\(void\)0\)\n#endif', inc): bad.append('nvn_nb_tap.inc: non-DOS2 NBF_* are not no-ops')
for n in ('nbf_rec', 'nbf_draw', 'nbf_caller', 'nbf_site_of'):
    body = ib.get(n)
    if body is None: bad.append('nvn_nb_tap.inc: %s missing' % n); continue
    if re.search(r'\b(rec|REC|RECN|cmd_alloc|nb_rec_alloc|put_hdr|nbd_flush|nbs_flush)\s*\(', body): bad.append('nvn_nb_tap.inc %s: NB-15 counters write a record' % n)
nd = ib.get('nbf_draw') or ''
drawops = set()
for n in names:
    if n.startswith('nvnCommandBuffer') and 'nbt_draw(' in (b.get(n) or ''):
        drawops |= set(re.findall(r'\bRECN?\(c, (OP_\w+)', b[n]))
for o in sorted(drawops | {'OP_X_NB_DRAW'}):
    if o not in nd: bad.append('nvn_nb_tap.inc nbf_draw: draw record %s not counted (NB-15)' % o)
cw = ib.get('nbf_caller') or ''
if not (0 <= cw.find('g_nbf_main_sz') < cw.find('module_of(') < cw.find('fp = nfp')): bad.append('nvn_nb_tap.inc nbf_caller: lr not checked (main, other module) before the saved fp is followed')
if 'atoi(e) == 1' not in fe or 'getenv("DM_NB_FOREIGN")' not in fe: bad.append('hle_dos2_nb.c: DM_NB_FOREIGN is not "on only when 1"')
for n in ('nb_rc_pre', 'nb_rc_post'):
    if 'nvn_nb_foreign_pkt(' not in (bodies(fe).get(n) or ''): bad.append('hle_dos2_nb.c %s: no NB-15 packet bracket' % n)
if 'nb_foreign_report(f)' not in (bodies(fe).get('nb_report') or ''): bad.append('hle_dos2_nb.c nb_report: no NB-15 line')
# NB-13b (DM_NB_PIPE, default 0): the memo keys on group ids (dm/gfx/nb/nb_pipe.h), so it is sound only while every writer of a group's
# State bytes marks that group. Writers: execute() cases (marked by nbpipe::mark before the switch: NB-13a's kind table for the state ops +
# the misc ops), the state reset (nbpipe::reset) and the pre-pass depth override inside draw() (not keyed: look() gets ppg.on). Any other
# writer fails here. The hooks: look before pipe_memo_fill in the fast block, store at both memo stores, clear / erase with the POD map.
pinc = (root / 'gfx/vulkan/engine/xbg_nb_pipe.inc').read_text(encoding='utf-8')
st_h = (root / 'gfx/nvn/nvn_nb_state.h').read_text(encoding='utf-8')
objh = (root / 'gfx/nvn/nvn_obj.h').read_text(encoding='utf-8')
G_OF = {'blend': 'BLEND', 'blend_color': 'BLEND', 'color': 'COLOR', 'mask': 'COLOR', 'ms': 'COLOR', 'alpha_ref': 'COLOR', 'poly': 'POLY', 'bias': 'POLY',
        'ds': 'DS', 'stencil_compare': 'DS', 'stencil_write': 'DS', 'stencil_ref': 'DS', 'attrib': 'VFMT', 'stream': 'VFMT',
        'patch_size': 'MISC', 'bounds': 'MISC', 'restart': 'MISC', 'depth_clamp': 'MISC', 'rast_discard': 'MISC'}
fld = '|'.join(sorted(G_OF, key=len, reverse=True))
wr13 = re.compile(r'(?:\bstate\.(' + fld + r')\b(?:\[[^\]]*\])*(?:\.\w+)?\s*=(?!=)|memcpy\(&?state\.(' + fld + r')\b|xbg_vertex_attributes_replace\(state\.(' + fld + r')\b)')
op_groups = {}   # NB-13a nbs_op_kinds: op -> kinds
ok_sw = st_h[st_h.find('static inline unsigned nbs_op_kinds('):st_h.find('// Kind and key of a written record')]
for m in re.finditer(r'((?:case OP_\w+:\s*)+)return ([^;]+);', ok_sw):
    for o in re.findall(r'case (OP_\w+):', m.group(1)): op_groups[o] = set(re.findall(r'NBS_K_(\w+)', m.group(2)))
mk = pinc[pinc.find('inline void mark('):pinc.find('void tick(')]
for o in re.findall(r'case (OP_\w+):', mk[mk.find('switch(op)'):]): op_groups.setdefault(o, set()).add('MISC')
ext = objh[objh.find('OP_X_PATCH_SIZE = OP_COUNT_'):]
ext_names = re.findall(r'^\s*(OP_X_\w+)', ext, flags=re.M)
alias = {'OP_COUNT_': ext_names[0]}; alias.update({'OP_COUNT_+%d' % i: n for i, n in enumerate(ext_names)})
ex = eng[eng.find('void execute(const uint8_t *pc,unsigned depth,uint64_t &budget){'):]
ex = ex[:ex.find('\n}\n')]
if not (0 <= ex.find('if(nbpipe::mode)nbpipe::mark(h.op,p,n);') < ex.find('switch(h.op){')): bad.append('xbg_game.cpp execute: no NB-13b mark before the switch')
seen13 = 0
for l in ex.split('\n'):
    labels = re.findall(r'case (OP_\w+(?:\+\d+)?):', l)
    for m in wr13.finditer(l):
        f = next(x for x in m.groups() if x); seen13 += 1
        if not labels: bad.append('xbg_game.cpp execute: a write of state.%s outside a case line (NB-13b cannot see it)' % f); continue
        for o in labels:
            o2 = alias.get(o, o)
            if G_OF[f] not in op_groups.get(o2, set()): bad.append('xbg_game.cpp execute %s writes state.%s (%s group) but nbpipe::mark does not mark that group' % (o, f, G_OF[f]))
if seen13 < 12: bad.append('xbg_game.cpp execute: NB-13b writer scan found only %d state writes (scanner broken?)' % seen13)
exk = eng.find('void execute(const uint8_t *pc,unsigned depth,uint64_t &budget){'); exe = exk + len(ex)
for f in sorted((root / 'gfx/vulkan/engine').glob('xbg_*')):
    if f.suffix not in ('.cpp', '.inc'): continue
    text = f.read_text(encoding='utf-8'); pos = 0
    for i, l in enumerate(text.split('\n')):
        at = pos; pos += len(l) + 1
        code = re.sub(r'//.*', '', l)
        if not wr13.search(code): continue
        if f.name == 'xbg_game.cpp' and exk <= at < exe: continue   # execute(): checked above
        if 'state=State{}' in code and 'nbpipe::reset();' in code: continue
        if f.name == 'xbg_game.cpp' and re.search(r'struct Guard\{|g\.on=true;g\.saved=state\.ds;', code): continue   # pre-pass override (draw-local, not keyed)
        bad.append('%s:%d: a pipeline-state group written outside execute() (NB-13b DM_NB_PIPE would key a stale id)' % (f.name, i + 1))
fb = eng[eng.find('if(draw_fast && pipe_memo && (pipe_memo_valid || pipe_pod_on())'):eng.find('static std::vector<VkVertexInputAttributeDescription> attributes;')]
if not (0 <= fb.find('state.restart_eff=re;}') < fb.find('nbpipe::look(') < fb.find('if(!fast_hit){pipe_memo_fill(')) or 'ppg.on,draw_fast,bindings,zero_binding)' not in fb:
    bad.append('xbg_game.cpp draw(): NB-13b look not between restart_eff and pipe_memo_fill (or without the pre-pass flag)')
if not (0 <= fb.find('fast_hit=true;') < fb.find('if(nbp_cur)nbpipe::store(hit_pipe,bindings,zero_binding);')): bad.append('xbg_game.cpp draw(): no NB-13b store at the fast-block hit')
if 'pipe_pod_store(pipe_memo_next,pipeline.pipeline);if(nbp_cur)nbpipe::store(pipeline.pipeline,bindings,zero_binding);}' not in eng: bad.append('xbg_game.cpp draw(): no NB-13b store at the slow-path memo store')
if eng.count('nbpipe::store(') != 2: bad.append('xbg_game.cpp: NB-13b store count != 2')
for l in eng.split('\n'):
    if 'pipeline_fast.clear()' in l and 'nbpipe::clear();' not in l: bad.append('xbg_game.cpp: a pipeline cache clear without nbpipe::clear (NB-13b): ' + l.strip()[:60])
if 'pipe_pod.erase(f);else ++f;nbpipe::erase(old);}' not in eng: bad.append('xbg_game.cpp pipe_pod_erase: no nbpipe::erase')
if '#include "xbg_nb_pipe.inc"' not in eng: bad.append('xbg_game.cpp: xbg_nb_pipe.inc not included')
# NB-14 (DM_NB_RELOC 1 / 2, default 0): the key frame. nbt_sync calls the key hooks right after the FRAME / PASS records (gated on g_nbk_on: off =
# no record written, the stream unchanged); OP_X_NB_KEY is the last op (no op value moved); the key is written only through nbt_rec (straight
# into the IR's command memory) and committed after the copy; the backend snapshots at every FRAME (nbchk::frame), executes the units with
# execute() itself, puts the live state back at =2, and has the case in execute(); the front end accepts only 1 / 2 and reports every 10 s.
# And: every execute() case that writes the backend state is modelled by the key (list / bind unit, nested walk) or explicitly not part of it.
kinc = (root / 'gfx/nvn/nvn_nb_key.inc').read_text(encoding='utf-8'); kh = (root / 'gfx/nvn/nvn_nb_key.h').read_text(encoding='utf-8')
kbe = (root / 'gfx/vulkan/engine/xbg_nb_key.inc').read_text(encoding='utf-8'); obj = (root / 'gfx/nvn/nvn_obj.h').read_text(encoding='utf-8')
sy = ib.get('nbt_sync') or ''
fr_i, pa_i = sy.find('g_nbt.frec = fr;'), sy.find('g_nbt.em_open = open;')
if not (0 <= fr_i < sy.find('if (g_nbk_on) nbk_frame();') < pa_i < sy.find('if (g_nbk_on) nbk_pass(open);')):
    bad.append('nvn_nb_tap.inc nbt_sync: NB-14 hooks not right after the FRAME / PASS records or not gated on g_nbk_on')
if not re.search(r'OP_X_NB_KEY,[^\n]*\n\s*OP_X_END_', obj): bad.append('nvn_obj.h: OP_X_NB_KEY is not the last op (an op value before OP_X_END_ moved)')
kb = bodies(kinc)
pk = kb.get('nbk_pass') or ''
if not (0 <= pk.find('nbk_walk(') < pk.find('nbk_pack(') < pk.find('nbt_rec(OP_X_NB_KEY, n)') < pk.find('memcpy(p, g_nbk_out, n); nbk_commit(&g_nbk, p, def_off);')):
    bad.append('nvn_nb_key.inc nbk_pass: walk -> pack -> nbt_rec(OP_X_NB_KEY) -> copy + commit order broken')
for n, body in kb.items():
    if re.search(r'\b(rec|REC|RECN|cmd_alloc|nb_rec_alloc|put_hdr)\s*\(', body): bad.append('nvn_nb_key.inc %s: writes a record outside nbt_rec' % n)
if 'void nvn_nb_key_enable(void) { g_nbk_on = 1; }' not in kinc: bad.append('nvn_nb_key.inc: enable does not set g_nbk_on')
if 'nbk_reset(&g_nbk);' not in (kb.get('nbk_frame') or ''): bad.append('nvn_nb_key.inc nbk_frame: the mirror is not emptied at FRAME')
if 'void frame(const p_nb_frame &f){nbkey::frame(f.seq);' not in nbinc or not nbinc.rstrip().endswith('#include "xbg_nb_key.inc"   // [DOS2] NB-14: DM_NB_RELOC, OP_X_NB_KEY (namespace nbkey: base at FRAME, check / set at every pass, start() for NB-17a)'):
    bad.append('xbg_nb.inc: no NB-14 base snapshot at FRAME / xbg_nb_key.inc not included at the end')
if 'case OP_X_NB_KEY:nbkey::exec(p,n,depth,budget);break;' not in eng: bad.append('xbg_game.cpp execute: no OP_X_NB_KEY case')
if not ('execute(v[i].content,depth+1,budget)' in kbe and 'nbk_parse(' in kbe): bad.append('xbg_nb_key.inc: units not executed by execute() after nbk_parse')
ex = kbe[kbe.find('void exec('):kbe.find('[[maybe_unused]] bool start(')]
if not (0 <= ex.find('memcpy(&live,&state,sizeof(State))') < ex.find('build(v,nu,depth,budget)') < ex.find('memcpy(tex_gen,tg,sizeof tg);memcpy(buf_gen,bg,sizeof bg);') < ex.find('if(mode==2){memcpy(&state,&live,sizeof(State));missing_program=live_missing;')):
    bad.append('xbg_nb_key.inc exec: =2 does not put the live state and generations back after the build')
if 'getenv("DM_NB_RELOC")' not in fe or 'r == 1 || r == 2' not in fe or 'nb_key_report(f)' not in (bodies(fe).get('nb_report') or ''):
    bad.append('hle_dos2_nb.c: DM_NB_RELOC not "1 / 2 only" or no NB-14 line')
if '(prog ? 0u : 0x8000u)' not in (root / 'gfx/nvn/nvn_nb_state.h').read_text(encoding='utf-8'):
    bad.append('nvn_nb_state.h nbs_kind: a NULL BindProgram shares the key of a non-NULL one (missing_program would be lost by the replay)')
exb = eng[eng.find('void execute(const uint8_t *pc,unsigned depth,uint64_t &budget){'):]
exb = exb[:exb.find('\n}\n')]
alias = {'OP_COUNT_': 'OP_X_PATCH_SIZE', 'OP_COUNT_+5': 'OP_X_DEPTH_BOUNDS'}
handled = set(re.findall(r'\b(OP_[A-Z0-9_]+)\b', kh[kh.find('static inline int nbk_list_of('):kh.find('// Units a record')] + kh[kh.find('static inline int nbk_bind_unit('):kh.find('static inline int nbk_unit_of(')]))
handled |= set(re.findall(r'case (OP_[A-Z0-9_]+)', (root / 'gfx/nvn/nvn_nb_state.h').read_text(encoding='utf-8').split('static inline int nbs_kind(')[1].split('// ---- shadow')[0]))
handled |= {'OP_CALL', 'OP_X_NATIVE_DRAW', 'OP_X_NB_STATE', 'OP_X_NB_DRAW'}   # nested walk / OP_X_NB_DRAW binds
nkeyed = 0
not_key = {'OP_RESET_COUNTER'}   # count_samples / samples_passed: counter epochs (passes with counters are not reordered, NB-16)
for chunk in re.split(r'\n        case ', exb)[1:]:
    labels = re.findall(r'^(OP_[A-Z0-9_]+(?:\+\d+)?):', chunk) + re.findall(r'case (OP_[A-Z0-9_]+(?:\+\d+)?):', chunk.split('\n')[0])
    code = chunk[:chunk.find('\n        case ') if '\n        case ' in chunk else len(chunk)]   # the whole case (incl. #if lines)
    if not re.search(r'\bstate\.\w+(?:\[[^\]]*\]|\.\w+)*\s*=(?!=)|memcpy\(&?state\.|exec_bind_(?:buf|handles)\(|xbg_vertex_attributes_replace\(state\.', code): continue
    for l in labels:
        op = alias.get(l, l)
        nkeyed += 1
        if op not in handled and op not in not_key: bad.append('xbg_game.cpp execute case %s writes the state but the NB-14 key frame does not model it' % l)
if nkeyed < 28: bad.append('xbg_game.cpp execute: only %d state-writing cases found (the NB-14 coverage scan is broken)' % nkeyed)
# NB-17a / NB-17b (DM_NB_ORDER 1 / 2 / 3, default 0): the order tail and the run by segments. Off = nothing reserved or written (the FRAME
# record keeps its size, every hook gated on g_nbo_on); the tail is filled only through nvn_nb_order_patch, after nvn_nb_tap_patch and
# before the queue, and writes no record; the backend: the stop compare is the first statement of execute()'s loop (before the budget, so
# a run by segments counts the same records), the handle's OP_END is left to the execute() that met the FRAME, nbchk::frame comes before
# nbord::frame; check mode never runs; a start from a key and the put-back reset NB-13b's groups (the state is written directly); and every
# op the scan calls "state-only" is an execute() case that only writes the state (a draw / clear / copy / CPU-visible case in the list fails).
oinc = (root / 'gfx/nvn/nvn_nb_order.inc').read_text(encoding='utf-8'); obe = (root / 'gfx/vulkan/engine/xbg_nb_order.inc').read_text(encoding='utf-8')
oh = (root / 'gfx/nb/nb_order.h').read_text(encoding='utf-8')
if not (0 <= sy.find('const uint32_t fsz = g_nbo_on ? nbo_frame_size() : (uint32_t)sizeof(p_nb_frame);') < sy.find('nbt_rec(OP_X_NB_FRAME, fsz)') < sy.find('g_nbt.frec = fr;')
        < sy.find('if (g_nbo_on) nbo_frame(fr, fsz);') < sy.find('g_nbt.em_open = open;') < sy.find('if (g_nbo_on) nbo_pass();')):
    bad.append('nvn_nb_tap.inc nbt_sync: NB-17a hooks missing, out of place or not gated on g_nbo_on (off must keep the FRAME record size)')
ob = bodies(oinc)
for n, body in ob.items():
    if re.search(r'\b(rec|REC|RECN|cmd_alloc|nb_rec_alloc|nbt_rec|put_hdr)\s*\(', body): bad.append('nvn_nb_order.inc %s: writes a record (the tail lives in the FRAME record only)' % n)
if 'void nvn_nb_order_enable(int mode) { g_nbo_on = mode; }' not in oinc: bad.append('nvn_nb_order.inc: enable does not set g_nbo_on')
pp = oinc[oinc.find('void nvn_nb_order_patch('):]
if not (0 <= pp.find('if (!g_nbo_on || !pl || (const uint8_t *)g_nbt.frec != pl) return;') < pp.find('nbo_build(') < pp.find('(uint8_t *)r - 8') < pp.find('nbo_fill(')):
    bad.append('nvn_nb_order.inc nvn_nb_order_patch: guard -> nbo_build -> PASS record addresses -> nbo_fill order broken')
pl = fe[fe.find('static void nb_ir_plan(void) {'):]; pl = pl[:pl.find('\n}\n')]
if not (0 <= pl.find('nb_order_plan(f, &tracked)') < pl.find('nvn_nb_tap_patch(') < pl.find('if (g_nbo) nvn_nb_order_patch(f, mo, !rc, tracked);')):
    bad.append('hle_dos2_nb.c nb_ir_plan: the order tail is not filled right after nvn_nb_tap_patch (before the submit is queued)')
if 'getenv("DM_NB_ORDER")' not in fe or 'r >= 1 && r <= 3' not in fe or 'nb_order_report(f)' not in (bodies(fe).get('nb_report') or ''):
    bad.append('hle_dos2_nb.c: DM_NB_ORDER not "1 / 2 / 3 only" or no NB-17a line')
if 'if (g_nbo < 2 || g_dm_nb_post) return NULL;' not in fe: bad.append('hle_dos2_nb.c nb_order_plan: the merge planner runs at DM_NB_ORDER=1 or with DM_NB_POST')
if not re.search(r'for\(;;\)\{if\(pc==nbord::stop\)return;[^\n]*\n\s*need\(budget\+\+<10000000', exb): bad.append('xbg_game.cpp execute: the NB-17a stop compare is not the first statement of the loop')
if 'case OP_END:if(nbord::in_run && depth==nbord::run_depth){nbord::end_pc=pc;return;}' not in exb: bad.append('xbg_game.cpp execute: OP_END inside a run by segments is not left to the outer execute()')
fc = exb[exb.find('case OP_X_NB_FRAME:'):]; fc = fc[:fc.find('\n        case ')]
if not (0 <= fc.find('nbchk::frame(payload<p_nb_frame>(p,n));') < fc.find('if(nbord::mode && nbord::frame(p,n,pc+h.size,depth,budget,go)){pc=go;continue;}')):
    bad.append('xbg_game.cpp execute: OP_X_NB_FRAME does not hand the part to nbord::frame after nbchk::frame')
if not (0 <= eng.find('#include "xbg_nb_order.inc"') < eng.find('void execute(const uint8_t *pc,unsigned depth,uint64_t &budget){')): bad.append('xbg_game.cpp: xbg_nb_order.inc not included before execute()')
fr = obe[obe.find('bool frame('):]
if not (0 <= fr.find('if(mode==2){') < fr.find('if(!ok)return false;') < fr.find('if(mode!=3){++s.r_mode;return false;}') < fr.find('nbstate::defs[') < fr.find('resume=nbo_run(')):
    bad.append('xbg_nb_order.inc frame: check mode / a failed verification / another order at =1 can reach the run, or the DEF blocks are not registered before it')
if 'nbkey::start(st->key,st->keyn,x.depth,*x.budget))return 0;' not in obe or not re.search(r'int cb_start\([^\n]*\n[^\n]*\n\s*nbpipe::reset\(\);', obe):
    bad.append('xbg_nb_order.inc cb_start: no nbpipe::reset() after the state came from a key (NB-13b would keep stale group ids)')
cp = obe[obe.find('void cb_put('):obe.find('// nbrp (xbg_nb.inc)')]
if not ('memcpy(&state,&kept,sizeof(State))' in cp and '++tex_gen[st];++buf_gen[st];' in cp and 'nbdesc::P.valid=false;' in cp and 'nbpipe::reset();' in cp):
    bad.append('xbg_nb_order.inc cb_put: the put-back does not move the generations / drop the NB-11 entry / reset the NB-13b groups')
if 'bool usable(const uint8_t *p,size_t n,uint32_t seq,int node)' not in kbe or 'return nbkey::usable(p,n,seq,node)?1:0;' not in obe:
    bad.append('xbg_nb_key.inc / xbg_nb_order.inc: the scan does not ask nbkey::usable for a complete key')
if 'if(nbord::in_run){p_nb_pass t;if(nbord::unit_last(*h,t)){nb_rp_dec d2{};if(map(p,t,d2)==NB_RP_OK)cur.store_dc&=d2.store_dc;else cur.store_dc=0;}}' not in nbinc:
    bad.append('xbg_nb.inc nbrp::open: a unit does not take its stores from its last pass (DONT_CARE only where both headers say so)')
pure = set(re.findall(r'case (OP_[A-Z0-9_]+(?:\+\d+)?):', obe[obe.find('bool pure_op('):obe.find('// Bytes of command memory readable')]))
no_effect = {'OP_BEGIN', 'OP_X_NB_PASS', 'OP_X_NB_PASS_END', 'OP_X_NB_KEY', 'OP_X_NB_STATE', 'OP_X_ENGINE_MARK', 'OP_X_FENCE_SYNC'}   # checks / nested state records / nothing
impure_call = re.compile(r'\b(draw|draw_texture|dispatch|clear_attachment|clear_texture|copy_texture|resolve_texture|gpu_buffer_copy|cpu_access|invalidate_cpu_range|submit_slots|'
                         r'complete_all|end_occlusion_query|apply_event|execute_indirect|execute_draw_call|full_barrier|end_open_pass)\s*\(|condr::|counter_epoch|pending_reports|pending_events')
def pure_bad(pure_ops):
    out = []; seen = set()
    for chunk in re.split(r'\n        case ', exb)[1:]:
        labels = re.findall(r'^(OP_[A-Z0-9_]+(?:\+\d+)?):', chunk) + re.findall(r'case (OP_[A-Z0-9_]+(?:\+\d+)?):', chunk.split('\n')[0])
        for l in labels:
            if l not in pure_ops: continue
            seen.add(l)
            code = re.sub(r'//[^\n]*', '', chunk)
            writes = re.search(r'\bstate\.\w+(?:\[[^\]]*\]|\.\w+)*\s*=(?!=)|memcpy\(&?state\.|exec_bind_(?:buf|handles)\(|xbg_vertex_attributes_replace\(state\.', code)
            if impure_call.search(code): out.append('%s: its execute() case does more than write the state' % l)
            elif not writes and l not in no_effect: out.append('%s: its execute() case does not write the state and is not a known no-effect record' % l)
    return out + ['%s: no execute() case' % l for l in sorted(pure_ops - seen)]
for w in pure_bad(pure): bad.append('xbg_nb_order.inc pure_op ' + w)
if len(pure) < 30: bad.append('xbg_nb_order.inc pure_op: only %d ops found (the NB-17a scan of the list is broken)' % len(pure))
for mut in ('OP_UPDATE_UB', 'OP_DRAW', 'OP_CLEAR_COLOR', 'OP_RESET_COUNTER', 'OP_X_RENDER_COND', 'OP_COPY_T2T', 'OP_BARRIER'):
    if not pure_bad(pure | {mut}): bad.append('nb_tap_test: the NB-17a state-only check would accept %s (mutation not caught)' % mut)
if 'enum { NBO_PASS_SEQ_OFF = 0, NBO_PASS_NODE_OFF = 4 };' not in oh or 'offsetof(p_nb_pass,seq)==NBO_PASS_SEQ_OFF && offsetof(p_nb_pass,node)==NBO_PASS_NODE_OFF' not in obe:
    bad.append('nb_order.h / xbg_nb_order.inc: the PASS record offsets the scan reads are not asserted against p_nb_pass')
# NB-21 (DM_NB_OWN 1 / 2, default 0): the frame job and the hand-off. Off = the old queue exactly (both call sites fall through to
# x_push, nothing is noted in the tap); at any setting no record is written (the job travels with the queue job). The queue: the final
# submit of a frame is always queued at once (never held back, so no waiter knows about it) - at =1 as an open XJ_OWN job; its present is
# attached to the queued job under the queue lock only while that job is the newest and not closed; the translation thread closes the job
# under the lock after the submission, reads it again and runs submit -> submit number published -> present -> frame number; the backend
# gets the frame job before the submission is executed and the pool slot is released after it. The model of these rules is
# tests/nb_own_test.c; here the real call sites are checked, and each rule removed from the source text must fail (mutations).
winc = (root / 'gfx/nvn/nvn_nb_own.inc').read_text(encoding='utf-8'); wbe = (root / 'gfx/vulkan/engine/xbg_nb_own.inc').read_text(encoding='utf-8')
kinc = (root / 'gfx/nvn/nvn_nb_key.inc').read_text(encoding='utf-8')
for n, body in bodies(winc).items():
    if re.search(r'\b(rec|REC|RECN|cmd_alloc|nb_rec_alloc|nbt_rec|put_hdr)\s*\(', body): bad.append('nvn_nb_own.inc %s: writes a record (DM_NB_OWN must not change the stream)' % n)
if not (0 <= sy.find('if (g_nbo_on) nbo_frame(fr, fsz);') < sy.find('if (g_nbw_on) nbw_frame();') < sy.find('g_nbt.em_open = open;') < sy.find('if (g_nbw_on) nbw_pass(open);')
        < sy.find('if (g_nbk_on) nbk_pass(open);')):
    bad.append('nvn_nb_tap.inc nbt_sync: NB-21 hooks missing, not gated on g_nbw_on, or nbw_pass after nbk_pass (the key address would be dropped)')
if 'if (g_nbw_on) nbw_key(node, p);' not in kinc: bad.append('nvn_nb_key.inc nbk_pass: the key address is not noted for the frame job (gated on g_nbw_on)')
if 'void nvn_nb_own_enable(int mode) { g_nbw_on = mode; }' not in winc or not re.search(r'void nvn_nb_own_build\([^\n]*\n\s*t_nbw_ready = NULL;\n\s*if \(!g_nbw_on\) return;', winc):
    bad.append('nvn_nb_own.inc: enable does not set g_nbw_on, or build is not gated on it / does not drop a job left from an earlier plan')
if 'g_nbo.last = 0;' not in oinc or not (0 <= oinc.find('nbo_fill(pl, g_nbo_ent, ns,') < oinc.find('g_nbo.last = ns;')):
    bad.append('nvn_nb_order.inc nvn_nb_order_patch: g_nbo.last (the valid tail the frame job copies) not reset / not set after nbo_fill')
ds = fe[fe.find('void dm_nb_submit(void) {'):]; ds = ds[:ds.find('\n}\n')]
if ds.count('nb_ir_plan();') != 2 or ds.count('nvn_nb_own_build(&g_irf, !g_ir_rc, g_ir_part);') != 2 or not (0 <= ds.rfind('nb_ir_plan();') < ds.rfind('if (g_nbw) nvn_nb_own_build(') < ds.rfind('nb_ir_finish();')):
    bad.append('hle_dos2_nb.c dm_nb_submit: the frame job is not built after each plan (and before nb_ir_finish resets the tap)')
eh = fe[fe.find('static uint64_t nb_end_hook('):]; eh = eh[:eh.find('\n}\n')]
if not (0 <= eh.find('nvn_nb_own_arm((int)(a1 & 1));') < eh.find('g_end_orig(w, a1, a2, a3, a4, a5)') < eh.find('if (g_nbw) nvn_nb_own_end();')):
    bad.append('hle_dos2_nb.c nb_end_hook: EndFrame is not armed by its present bit before the original / not ended after it')
if 'getenv("DM_NB_OWN")' not in fe or 'if (r == 1 || r == 2) {\n            g_nbw = r; nvn_nb_own_enable(r);' not in fe or 'nb_own_report(f)' not in (bodies(fe).get('nb_report') or ''):
    bad.append('hle_dos2_nb.c: DM_NB_OWN not "1 / 2 only" or no NB-21 line')
def own_bad(text):
    out = []; q = bodies(text); clean = re.sub(r'//[^\n]*', '', text)
    def body(n): return q.get(n) or ''
    def seq(s, *parts):
        at = -1
        for p in parts:
            at2 = s.find(p, at + 1)
            if at2 < 0: return False
            at = at2
        return True
    for n in ('x_own_submit', 'x_own_present'):
        if not re.match(r'\{\s*if \(!g_nbw_on\) return 0;', body(n)): out.append('%s: not off at DM_NB_OWN=0' % n)
    if 'if (!XOWN_SUBMIT(&j)) x_push(&j);' not in clean: out.append('nvnQueueSubmitCommands: the submit does not fall through to x_push')
    if not seq(clean, 'own_job = XOWN_PRESENT(&j, g_frame);', 'o->ticket[idx] = own_job ? own_job : x_push(&j); o->done[idx] = x_fence();', 'if (!own_job) { xjob j = {XJ_FRAME};'):
        out.append('nvnQueuePresentTexture: the present / frame number are not queued the old way when not attached (or as well as attached), or the ticket is not the open job')
    if '#define XOWN_SUBMIT(j) 0' not in clean or '#define XOWN_PRESENT(j, frame) 0' not in clean or '#define XOWN_RUN(j) ((void)0)' not in clean:
        out.append('the hand-off is not compiled out of the other games')
    for n in ('x_wait', 'x_drain', 'x_wait_sub', 'x_thread'):
        if 'g_xown' in body(n) or 'XOWN' in body(n): out.append('%s: knows about the frame job (nothing is held back, so no waiter may depend on it)' % n)
    if not seq(body('x_push'), 'pthread_mutex_lock(&g_xmu);', 'if (j->kind == XJ_SUBMIT || j->kind == XJ_OWN) j->sub = atomic_fetch_add(&g_x_subs, 1) + 1;', 'uint64_t job = ++g_xq_in;',
               'if (j->kind == XJ_OWN) nbw_hand_opened(&g_xown.h, job, j->sub, 0);', 'g_xq[job % XQ] = *j;'):
        out.append('x_push: an XJ_OWN job gets no submit number, or the rule does not learn its job number under the queue lock before the job is visible')
    sb = body('x_own_submit')
    if not seq(sb, 'j->own = nbw_take(j->h, j->n);', 'pthread_once(&g_x_once, x_start);', 'nbw_hand_submit(&g_xown.h, t_xown_armed, j->own != NULL, g_x_sync);',
               'if (open && !virt) { j->kind = XJ_OWN; j->parts = NBW_P_SUBMIT; }', 'const uint64_t job = x_push(j);', 'if (open && virt) {', 'nbw_hand_opened(&g_xown.h, job, j->sub, 1);', 'return 1;') \
            or sb.count('return') != 2 or sb.count('x_push(j)') != 1:
        out.append('x_own_submit: the submit must always be queued at once (x_push on every path), as XJ_OWN only at =1, counted as open at =2')
    pb = body('x_own_present')
    if not seq(pb, 'pthread_mutex_lock(&g_xmu);', 'xjob *q = &g_xq[g_xown.h.job % XQ];', 'const int done = g_xown.h.virt ? g_xsub_done >= g_xown.h.sub : (q->parts & NBW_P_CLOSED) != 0;',
               'if (nbw_hand_present(&g_xown.h, t_xown_armed, g_xq_in, done)) {', 'q->parts |= NBW_P_PRESENT | NBW_P_FRAME;', 'job = g_xown.h.job;', 'pthread_mutex_unlock(&g_xmu);'):
        out.append('x_own_present: not "under the queue lock: newest job and not closed -> attach to the queued job, its number is the ticket"')
    rb = body('x_run_own')
    if not seq(rb, 's.kind = XJ_SUBMIT; x_run(&s, job);', 'pthread_mutex_lock(&g_xmu);', 'xjob *q = &g_xq[job % XQ];', 'q->parts |= NBW_P_CLOSED; s = *q;', 'g_xsub_done = j->sub; pthread_cond_broadcast(&g_xcv);',
               'pthread_mutex_unlock(&g_xmu);', 'if (s.parts & NBW_P_PRESENT) { s.kind = XJ_PRESENT; x_run(&s, job); }', 'if (s.parts & NBW_P_FRAME) { s.kind = XJ_FRAME; x_run(&s, job); }'):
        out.append('x_run_own: not submit -> (under the lock) closed, the queued job read again, submit number published -> present -> frame number')
    xr = body('x_run')
    if not seq(xr, 'if (j->kind == XJ_OWN) { x_run_own(j, job); return; }', 'XOWN_RUN(j); x_submit(j->h, j->n, j->sub, j->host); XOWN_DONE(j); free(j->h);'):
        out.append('x_run: the frame job is not given to the backend before its submission and released after it')
    if not seq(clean, '#define XOWN_RUN(j)', 'mtl_nb_own((j)->own, (j)->h, (j)->n);', '#define XOWN_DONE(j)', 'mtl_nb_own(NULL, NULL, 0); nbw_release((j)->own);'):
        out.append('XOWN_RUN / XOWN_DONE: begin with the handles, end, then release the pool slot')
    if 'if (j.kind == XJ_SUBMIT || j.kind == XJ_OWN) { g_xsub_done = j.sub;' not in clean: out.append('x_thread: an XJ_OWN job does not publish its submit number')
    en = clean[clean.find('void nvn_nb_own_end(void) {'):]; en = en[:en.find('\n}\n')]
    if not seq(en, 't_xown_armed = 0;', 'nbw_hand_end(&g_xown.h);'): out.append('nvn_nb_own_end: the open job of this EndFrame is not forgotten when it returns')
    if 'void nvn_nb_own_arm(int on) { t_xown_armed = on && g_nbw_on; }' not in clean: out.append('nvn_nb_own_arm: not per thread / not gated')
    return out
for w in own_bad(src): bad.append('hle_nvn.c NB-21 ' + w)
own_muts = (('    q->parts |= NBW_P_CLOSED; s = *q;\n', '    s = *q;\n'),
            ('    q->parts |= NBW_P_CLOSED; s = *q;\n', '    q->parts |= NBW_P_CLOSED;\n'),
            ('    pthread_mutex_lock(&g_xmu);\n    xjob *q = &g_xq[job % XQ];\n', '    xjob *q = &g_xq[job % XQ];\n'),
            ('nbw_hand_present(&g_xown.h, t_xown_armed, g_xq_in, done)', 'nbw_hand_present(&g_xown.h, t_xown_armed, g_xown.h.job, done)'),
            ('g_xsub_done >= g_xown.h.sub : (q->parts & NBW_P_CLOSED) != 0;', 'g_xsub_done >= g_xown.h.sub : 0;'),
            ('    if (!own_job) { xjob j = {XJ_FRAME};', '    { xjob j = {XJ_FRAME};'),
            ('o->ticket[idx] = own_job ? own_job : x_push(&j);', 'o->ticket[idx] = x_push(&j);'),
            ('XOWN_DONE(j); free(j->h);', 'free(j->h);'),
            ('    if (!g_nbw_on) return 0;\n    j->own = nbw_take(j->h, j->n);', '    j->own = nbw_take(j->h, j->n);'),
            ('    if (j->kind == XJ_SUBMIT || j->kind == XJ_OWN) j->sub = atomic_fetch_add', '    if (j->kind == XJ_SUBMIT) j->sub = atomic_fetch_add'),
            ('        if (j.kind == XJ_SUBMIT || j.kind == XJ_OWN) { g_xsub_done = j.sub;', '        if (j.kind == XJ_SUBMIT) { g_xsub_done = j.sub;'),
            ('static void x_wait(uint64_t job) { pthread_mutex_lock(&g_xmu);', 'static void x_wait(uint64_t job) { pthread_mutex_lock(&g_xmu); if (g_xown.h.open) {}'),
            ('    const uint64_t job = x_push(j);\n    if (open && virt)', '    if (open && !virt) return 1;\n    const uint64_t job = x_push(j);\n    if (open && virt)'))
for a, r in own_muts:
    if src.count(a) != 1: bad.append('nb_tap_test: NB-21 mutation anchor not found once: %r' % a[:60])
    elif not own_bad(src.replace(a, r)): bad.append('nb_tap_test: an NB-21 rule removed from hle_nvn.c is not caught (%r)' % a[:60])
if not (0 <= eng.find('#include "xbg_nb_order.inc"') < eng.find('#include "xbg_nb_own.inc"') < eng.find('void execute(const uint8_t *pc,unsigned depth,uint64_t &budget){')):
    bad.append('xbg_game.cpp: xbg_nb_own.inc not included before execute()')
if 'case OP_X_NB_FRAME:{if(nbown::cur)nbown::frame(pc,p,n);' not in exb or 'case OP_X_NB_PASS:if(nbown::cur)nbown::pass(pc,p,n);' not in exb:
    bad.append('xbg_game.cpp execute: the FRAME / PASS records are not reported to the frame job (behind nbown::cur, null at DM_NB_OWN=0)')
wclean = re.sub(r'//[^\n]*', '', wbe)
if re.search(r'\bstate\.\w+|\bexecute\s*\(|\bdraw\s*\(|begin_pass|end_open_pass', wclean): bad.append('xbg_nb_own.inc: touches the backend state / executes records (NB-21 only compares)')
if not re.search(r'extern "C" void mtl_nb_own\(const void \*job,const uint64_t \*h,int n\)\{\s*if\(job\)nbown::begin\(static_cast<const nbw_job\*>\(job\),h,n\);else nbown::end\(\);\}', wbe):
    bad.append('xbg_nb_own.inc: mtl_nb_own does not begin / end the frame job')
if not (0 <= wbe.find('const int v=nbw_valid(j);') < wbe.find('nbw_chk_begin(&ck,j,h,n);cur=j;')): bad.append('xbg_nb_own.inc begin: a job is used before nbw_valid')
if bad:
    print('FAIL: NB tap coverage\n  ' + '\n  '.join(bad)); sys.exit(1)
print('PASS: NB-04 tap on %d NVN entry points (%d via helpers %s), gated by DM_NB, records unchanged; NB-05 records synced / patched / replayed; '
      'NB-09 draw capture after the tap, NVN record fallback, foreign flush, unpack via the execute helpers; NB-10 apply / check; NB-13a state hooks; NB-11 generation writers / reuse decision; NB-12 pending-list writers / hit-fast + ring hooks; NB-15 foreign counters (%d draw ops); NB-13b group writers / memo hooks; NB-14 key hooks, last op, backend check / restore, every state-writing execute case keyed (%d); NB-17a order tail hooks / run by segments / %d state-only ops checked against execute(); NB-21 frame job / queue hand-off (%d source mutations caught)' % (
    len(names), sum(1 for n in names if 'NBT(' not in (b.get(n) or '')), ', '.join(sorted(helpers)), len(drawops), nkeyed, len(pure), len(own_muts)))
