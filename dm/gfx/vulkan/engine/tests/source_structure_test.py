# C/C++ 分隔符门：捕获手工三方合并的结构截断，不替代编译器。
# C/C++ delimiter gate: catch structural truncation during manual three-way merges; not a compiler replacement.
from pathlib import Path
import re
root=Path(__file__).resolve().parents[4]
def first_branch(s):   # keep only the first branch of #if / #else (both branches would count a brace twice)
    out=[];skip=[]
    for ln in s.split('\n'):
        t=ln.strip()
        if t.startswith('#if'):skip.append(bool(skip and skip[-1]));out.append('');continue
        if t.startswith('#el') and skip:skip[-1]=True;out.append('');continue
        if t.startswith('#endif') and skip:skip.pop();out.append('');continue
        out.append('' if (skip and skip[-1]) else ln)
    return '\n'.join(out)
def delimiter_gate():   # -> names of files whose delimiters do not balance (NB-20a: runs after the draw-path check, same verdict)
    bad=[]
    for name in ['gfx/vulkan/engine/xbg_game.cpp','gfx/nvn/hle_nvn.c','android/vulkan_present.cpp']:
        s=(root/name).read_text(encoding='utf-8')
        s=re.sub(r"""/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'""", "", s,flags=re.S)
        s=first_branch(s)   # NB-20a: #if / #else branches counted once
        stack=[];ok=True
        for ch in s:
            if ch in '({[':stack.append(ch)
            elif ch in ')}]':
                if not stack or stack.pop()!={')':'(',']':'[','}':'{'}[ch]:ok=False;break
        if not ok or stack:bad.append(name);print('FAIL: delimiter gate: %s (%d left open)'%(name,len(stack)))
    if not bad:print('PASS: C/C++ balanced delimiters for engine, NVN and Android presenter (not a compile)')
    return bad

# NB-20a (docs/dos2/NATIVE_BACKEND_PLAN.md, "NB-20a findings"): every namespace-scope variable and every `static` local of the
# translate-thread draw path, by class, for multi-threaded pass recording (DM_NB_MT, NB-20b..d). Name-based grep, not a parser:
# a NEW name in the draw path fails here -> classify it in the NB-20a table and add it below (a name that left the path only prints).
#   C = per-recording-context (moves into the worker's context)   K = counter (per-context block, summed at the pass join)
#   L = shared cache / lock / atomic                              S = serial: pass / barrier / layout / slot decisions (plan pre-pass)
#   R = read-only after init (switches, device facts)              D = diagnostic (DM_NB_MT stays serial while armed)
#   N = name collision: a global of another namespace whose name the path uses for a local / member
NB20A_DRAW_GLOBALS = {
 'C': 'command_pool cx_sh draw_timing nvn_stage P cand',   # not moved yet (cx_sh / draw_timing: diagnostics; P / cand: nbdesc); the rest: NB20B_CTX below
 'K': 'alpha_draws alpha_refused alpha_variants async_skips bad barrier_by binds_skipped changed compile_ns create_max_ns create_ns '
      'creates depth_bounds_dropped depth_feedback_reads depth_idle_passes desc_abandoned desc_chained desc_memo_hits desc_memo_misses '
      'desc_memo_texel_fail desc_memo_why desc_ns dirty_pages fallbacks fb_creates fb_hits hits key_diff lazy_storage_saved '
      'memo_front_hits memo_simg_hits mid_chunk_ns mismatches missing_program_skips msaa_passes noload_passes noload_why noout_masked '
      'offset_changed_at offset_changes pass_end_by pass_end_lines pass_fast_hits pending_misses perf pipe_draw_fast_hits pipe_memo_hits '
      'pipe_memo_misses pipe_pod_hits pipe_pod_misses pipeline_counts pipeline_fast_hits pipeline_ns read_break_why reversed_depth_draws '
      'set_bind_offsets set_binds set_binds_offsets_only skipped_draws snap_front_hits stats texel_hits tile_war_barriers xmemo_hits '
      'xview_races s bytes draws hist idx inst multi plain u8 store_kept',   # + DM_GFX_VTX_STATS counters (VERTEX.md)
 'L': 'any built count exhausted failed gskip_n id_keys ids images_epoch lazy_info memo_check_gen memo_mod_gen memo_pinned modules mu '
      'mutex nregions persistent_framebuffers pipeline_cache pipeline_fast pipeline_objects pipeline_tick pipelines pipelines_unsaved '
      'regions render_pass_cache snapshot_epoch tracked',
 'S': 'barrier_seq bi_dc counter_epoch cur cur_ok desc_serial0 epochs frag_seq frame last_pass lazy lazy_ok lazy_pending open_occlusion '
      'inside open_pass passes_since_full pclear_n pending_reports pending_writebacks slot_index slot_rec_begin slot_serial slots storage_dirty '
      'storage_image_draw storage_unbarriered write_seq cmd index cur_at lazy_at',   # + DM_GFX_SAMPLES_TS=4 shared stamp of the open pass (samplets::cmd / index, VERTEX.md)
 'R': 'bind_cache_enabled cfg depth16_as_d24 depth_bias_x2 desc_memo_enabled draw_detail draw_reads_on enabled gpu_driver_id img_fast '
      'lazy_clear_on lazy_storage max_regions missing_program nb_rp_mode no_occlusion_queries noload_enabled noout_mask_on '
      'occlusion_batch_enabled persistent_fb_enabled pipe_memo pipeline_fast_enabled pipeline_object_limit properties read_frag_only '
      'read_preflight readback_cached serial slot_pools_enabled snap_no_sync store_hist_mode texel_memo_enabled tile_barrier_on '
      'tile_frag_stages timestamp_bits xmemo_enabled xview_guard hist_on',
 'D': 'mode active armed on bi_kind bi_store capture draw_log_fp draw_log_n end_img end_line end_why eng_mask last_attachment_trace last_writer '
      'late notes open pass_prof_draws pass_prof_enabled pass_prof_frame pass_prof_next pass_prof_period pass_prof_queries '
      'pass_prof_query_limit pass_prof_skipped_slots pass_prof_slot_limit pass_prof_slots pass_prof_until pass_prof_verts passes pend '
      'pipe_stats pipeline provenance_file pstat_logged pstat_use sub told in_op mute sh_b sh_f',   # + NB-08 DM_NB_OPBARRIER=2 shadow (opbar::)
 'N': 'guard layout line ok pool result sets slot t0 uint8_t valid want why state',   # state: astccache::state (the State of the draw path is a context variable, NB20B_CTX)
}
# static locals of draw(prim, first, count, ...): C scratch vectors, D log-once counters, R one-time env parse, N scanner artefact
NB20A_DRAW_STATICS = {'C': 'attachments attributes bindings fast fkey stages', 'D': 'n noted notes told unb', 'R': 'skip_init skip_sh', 'N': 'uint32_t'}
NB20A_PATH = ('draw create_pass descriptors descriptors_stable prepare_read prepare_attachment mark_storage_images begin_open '
              'end_open_pass full_barrier snapshot stream_buffer first_draw_covers nbrp_begin lazy_clear_open lazy_clear_begin '
              'occlusion_query pass_prof_draw pipeline_lookup recording_slot pipe_memo_fill get_image').split()


def _nb20a_strip(s):
    bs = chr(92); out = []; i = 0; n = len(s)
    while i < n:
        c = s[i]
        if s.startswith('//', i):
            j = s.find('\n', i); j = n if j < 0 else j; out.append(' ' * (j - i)); i = j
        elif s.startswith('/*', i):
            j = s.find('*/', i + 2); j = n if j < 0 else j + 2; out.append(re.sub(r'[^\n]', ' ', s[i:j])); i = j
        elif c in '"\'' and not (c == "'" and i and s[i - 1].isalnum()):
            j = i + 1
            while j < n and s[j] != c:
                j += 2 if s[j] == bs else 1
            out.append(c + ' ' * (j - i - 1) + c); i = j + 1
        elif c == '#' and s[s.rfind('\n', 0, i) + 1:i].strip() == '':
            j = i
            while True:
                e = s.find('\n', j); e = n if e < 0 else e
                if s[e - 1:e] == bs: j = e + 1; continue
                break
            out.append(re.sub(r'[^\n]', ' ', s[i:e])); i = e
        else:
            out.append(c); i += 1
    return ''.join(out)


def _nb20a_scan(text):
    """-> (namespace-scope variable names, {function name: [bodies]}) of one translation unit (both #if branches are seen)."""
    s = _nb20a_strip(text); stack = []; st0 = 0; paren = 0; names = set(); bodies = {}
    fn_head = re.compile(r'([A-Za-z_~]\w*)\s*\(([^;{}]*)\)\s*(?:const)?\s*(?:noexcept)?\s*$')
    top = lambda: all(k == 'ns' for k in stack)
    for i, c in enumerate(s):
        if c == '(': paren += 1
        elif c == ')': paren -= 1
        elif c == '{' and paren == 0:
            head = s[st0:i]
            if top() and re.search(r'\bnamespace\s*\w*\s*$', head): stack.append('ns'); st0 = i + 1
            elif top():
                h = head.strip(); m = fn_head.search(h)
                if m and not re.match(r'(template\s*<[^>]*>\s*)?(struct|class|union|enum)\b', h) and not h.endswith('='):
                    stack.append(('fn', m.group(1), i))
                else: stack.append(('other', '', i))
            else: stack.append('x')
        elif c == '}' and paren == 0:
            k = stack.pop()
            if k == 'ns': st0 = i + 1
            elif top() and k[0] == 'fn': bodies.setdefault(k[1], []).append(s[k[2]:i + 1]); st0 = i + 1
        elif c == ';' and paren == 0 and top():
            t = re.sub(r'\s+', ' ', s[st0:i]).strip(); st0 = i + 1
            if not t or re.match(r'(using|typedef|template|friend|static_assert|namespace)\b', t): continue
            if re.match(r'(struct|class|union)\b[^{]*\{', t): t = re.sub(r'^.*\}\s*', '', t)
            d = 0; parts = ['']
            for ch in t:
                d += ch in '({[<'; d -= ch in ')}]>'
                if ch == ',' and d == 0: parts.append('')
                else: parts[-1] += ch
            if re.search(r'\w\s*\(.*\)\s*(const|noexcept)?\s*$', parts[0].split('=')[0].strip()): continue  # prototype
            if re.match(r'(struct|class|enum|union)\s+\w+\s*$', parts[0].strip()): continue
            for pt in parts:
                q = re.sub(r'\{.*$', '', re.sub(r'\[[^\]]*\]', '', pt.split('=')[0])).strip()
                m = re.search(r'([A-Za-z_]\w*)$', q)
                if m: names.add(m.group(1))
    return names, bodies


def nb20a_check():
    names, bodies = _nb20a_scan((root / 'gfx/vulkan/engine/xbg_game.cpp').read_text(encoding='utf-8'))
    gone = [f for f in NB20A_PATH if f not in bodies]
    assert not gone, 'NB-20a: draw-path functions not found (renamed? update NB20A_PATH): %s' % gone
    used = set(); statics = set()
    for f in NB20A_PATH:
        for b in bodies[f]:
            used |= {m.group(1) for m in re.finditer(r'(?<![\w.>])([A-Za-z_]\w*)', b) if m.group(1) in names}
            if f == 'draw' and len(b) > 20000:   # draw(prim, first, count, ...), not pgraph::draw()
                statics |= {m.group(1) for m in re.finditer(r'\bstatic\s+[\w:<>,\s\*&]+?\b([A-Za-z_]\w*)\s*(?=[=;{\[,])', b)}
    for what, table, found in (('global', NB20A_DRAW_GLOBALS, used), ('static local of draw()', NB20A_DRAW_STATICS, statics)):
        known = {}
        for cls, v in table.items():
            for nm in v.split():
                assert nm not in known, 'NB-20a: %s listed twice (%s, %s)' % (nm, known[nm], cls); known[nm] = cls
        new = sorted(found - set(known))
        assert not new, ('NB-20a: new %s on the translate-thread draw path: %s -> classify it (C/K/L/S/R/D/N) in '
                         'docs/dos2/NATIVE_BACKEND_PLAN.md "NB-20a findings" and in NB20A_DRAW_* here' % (what, ' '.join(new)))
        left = sorted(set(known) - found)
        if left: print('note: NB-20a %s no longer on the draw path (drop from the table): %s' % (what, ' '.join(left)))
    counts = {c: len(v.split()) for c, v in NB20A_DRAW_GLOBALS.items()}
    print('PASS: NB-20a draw-path globals classified: %d (%s), draw() static locals: %d'
          % (len(used), ' '.join('%s=%d' % kv for kv in counts.items()), len(statics)))


# NB-19 (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-19 findings"): DM_NB_TRANSIENT wiring. Every hook in xbg_game.cpp is gated by nbtr::mode
# (off = one test, output unchanged); every non-attachment use of an image reaches nbtr::touch before it needs other usage; every host
# pass begin path goes through lrz_touch_before (nbtr::begin); the planned paths call nbtr::decide right before it.
def nb19_check():
    s = (root / 'gfx/vulkan/engine/xbg_game.cpp').read_text(encoding='utf-8')
    inc = (root / 'gfx/vulkan/engine/xbg_nb_transient.inc').read_text(encoding='utf-8')
    names, bodies = _nb20a_scan(s)
    calls = list(re.finditer(r'nbtr::(touch|begin|decide|frame|gone)\(', s))
    assert len(calls) >= 9, 'NB-19: nbtr hooks missing (%d)' % len(calls)
    for m in calls:
        line = s[s.rfind('\n', 0, m.start()) + 1:s.find('\n', m.start())]
        assert 'nbtr::mode>0' in line or 'nbtr.transient' in line, 'NB-19: ungated hook: %s' % line.strip()[:160]
    tr = s[s.find('void transition(VkCommandBuffer cmd,VkImageLayout next'):][:900]
    assert 'nbtr::touch(*this,NB_TRF_XFER)' in tr and tr.find('nbtr::touch') < tr.find('b.oldLayout=layout'), \
        'NB-19: the transition hook must run before the barrier reads layout / image'
    vw = s[s.find('VkImageView view(const view_t *v,bool attachment=false'):][:400]
    assert 'if(!attachment && nbtr::mode>0)nbtr::touch(*this,NB_TRF_READ)' in vw, 'NB-19: view hook (sampled / storage view)'
    for fn, hook in (('prepare_read', 'nbtr::touch(im,NB_TRF_READ)'), ('upgrade_mutable', 'nbtr::touch(im,NB_TRF_XFER)'),
                     ('lrz_touch_before', 'nbtr::begin(p)'), ('nbrp_begin', 'nbtr::decide(p,t.dc,store)'),
                     ('nbrp_lazy_begin', 'nbtr::decide(lazy.pass,')):
        b = bodies.get(fn, [''])[0]
        assert hook in b, 'NB-19: %s lacks %s' % (fn, hook)
        if fn.startswith('nbrp'):
            assert b.find('nbtr::decide') < b.find('lrz_touch_before('), 'NB-19: decide() must precede lrz_touch_before in %s' % fn
    code = _nb20a_strip(s)
    begins = len(re.findall(r'vkCmdBeginRenderPass\(', code))
    lrz = len(re.findall(r'\blrz_touch_before\((?!const)', code))
    assert lrz == 6 and begins == 7, ('NB-19: host pass begin paths changed (%d vkCmdBeginRenderPass, %d lrz_touch_before calls): a new '
                                      'begin path must call lrz_touch_before (nbtr::begin) - the ClearTexture view pass is the 7th' % (begins, lrz))
    assert 'nbtr::init();' in s and 'getenv("DM_NB_TRANSIENT")' in inc and 'TRANSIENT_ATTACHMENT_BIT' in inc and 'LAZILY_ALLOCATED_BIT' in inc
    print('PASS: NB-19 DM_NB_TRANSIENT hooks gated (%d calls); transition / view / prepare_read / upgrade_mutable touch; %d host pass '
          'begins through lrz_touch_before' % (len(calls), lrz))


# NB-20b (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-20b findings"): DM_NB_MT records the slot's command buffer on a worker by turning every
# vkCmd* of the translation unit into a packet (xbg_nb_mt.inc). That only holds while NO call reaches the command buffer around the
# wrappers: a vkCmd* name without a wrapper, a command called through a function pointer, the command buffer handed to another file, or
# a begin / end of the slot's command buffer outside the nbmt bracket would write into it while the worker does.
# The draw path's globals (NB-20a table above) are untouched by NB-20b: the worker reads none of them (it gets copies of the arguments).
def nb20b_check():
    eng = root / 'gfx/vulkan/engine'
    s = (eng / 'xbg_game.cpp').read_text(encoding='utf-8')
    mt = (eng / 'xbg_nb_mt.inc').read_text(encoding='utf-8')
    wrapped = set(re.findall(r'^#define (vkCmd\w+) nbmt::Cmd\w+\s*$', mt, flags=re.M))
    assert len(wrapped) >= 31, 'NB-20b: wrapper list of xbg_nb_mt.inc not found (%d)' % len(wrapped)
    for w in sorted(wrapped):
        assert re.search(r'#define %s nbmt::C%s\s*$' % (w, w[3:]), mt, flags=re.M), 'NB-20b: %s is mapped to another wrapper' % w
        body = mt[mt.find('inline void C%s(VkCommandBuffer c' % w[3:]):]
        assert body.startswith('inline void C') and 'NBMT_REAL(%s)' % w in body[:body.find('\n}') + 2 if '{ pod<' not in body[:400] else body.find('\n') + 1], \
            'NB-20b: wrapper of %s does not reach the real %s' % (w, w)
    code = _nb20a_strip(s)
    inc_at = s.find('#include "xbg_nb_mt.inc"')
    first = re.search(r'\bvkCmd\w+', code)
    assert 0 < inc_at < first.start(), 'NB-20b: xbg_nb_mt.inc must be included before the first vkCmd* of xbg_game.cpp'
    used = set(re.findall(r'\bvkCmd\w+', code))
    files = ['xbg_game.cpp']
    for f in sorted(eng.glob('*.inc')):
        if f.name == 'xbg_nb_mt.inc': continue
        t = f.read_text(encoding='utf-8')   # every engine .inc is part of this translation unit (some through another .inc)
        used |= set(re.findall(r'vkCmd\w+', _nb20a_strip(t))); files.append(f.name)
        at = s.find('#include "%s"' % f.name)
        assert at < 0 or at > inc_at, 'NB-20b: %s is included before xbg_nb_mt.inc' % f.name
    missing = sorted(used - wrapped)
    assert not missing, ('NB-20b: vkCmd* without a wrapper in xbg_nb_mt.inc (with DM_NB_MT it would be written into the command buffer '
                         'while the worker records): %s' % ' '.join(missing))
    assert '#undef vkCmd' not in s, 'NB-20b: a vkCmd* wrapper is undefined in xbg_game.cpp'
    # commands reached through function pointers: declared PFN_vkCmd* variables may only be passed to nbmt::P*, never called
    ptrs = set(re.findall(r'PFN_vkCmd\w+\s+(\w+)\s*=\s*nullptr', code))
    assert {'pbegin', 'pend', 'pfn_draw_indexed_indirect_count', 'pfn_draw_indirect_count'} <= ptrs, 'NB-20b: PFN_vkCmd* variables moved (%s)' % sorted(ptrs)
    for nm in sorted(ptrs):
        m = re.search(r'(?<![\w])(?:\w+\s*\.\s*)?%s\s*\(' % nm, code)
        assert not m, 'NB-20b: %s is called directly (use the nbmt::P* wrapper): %s' % (nm, code[max(0, m.start() - 40):m.end() + 20].strip())
    assert len(re.findall(r'nbmt::P(?:Begin|End)ConditionalRendering\(|nbmt::PDraw(?:Indexed)?IndirectCount\(', code)) == 4, 'NB-20b: the four pointer commands go through nbmt::P*'
    # the bracket: one begin and one end of the slot's command buffer, each with its hook right next to it
    names, bodies = _nb20a_scan(s)
    assert len(re.findall(r'vkBeginCommandBuffer\(s\.cmd\b', code)) == 1 and len(re.findall(r'vkEndCommandBuffer\(s\.cmd\b', code)) == 1, \
        'NB-20b: the slot command buffer is begun / ended in more than one place'
    rs = bodies['recording_slot'][0]; b0 = rs.find('vkBeginCommandBuffer(s.cmd'); h0 = rs.find('nbmt::begin(s.cmd,')
    assert 0 < b0 < h0 and not re.search(r'\bvkCmd\w+', rs[b0:h0]), 'NB-20b: nbmt::begin must follow vkBeginCommandBuffer(s.cmd) at once'
    assert 's.pool!=VK_NULL_HANDLE' in rs[h0:h0 + 80] and 'ab_off(' in rs[h0:h0 + 80], 'NB-20b: nbmt::begin needs the own-pool and A/B conditions'
    ss = bodies['submit_slots'][0]; h1 = ss.find('nbmt::end(s.cmd)'); e1 = ss.find('vkEndCommandBuffer(s.cmd')
    assert 0 < h1 < e1 and not re.search(r'\bvkCmd\w+', ss[h1:e1]), 'NB-20b: nbmt::end must be the last thing before vkEndCommandBuffer(s.cmd)'
    assert 'nbmt::stop()' in bodies['destroy_slots'][0] and s.count('nbmt::init();') == 1, 'NB-20b: nbmt::stop in destroy_slots / nbmt::init at the switches'
    # the command buffer never leaves the translation unit, and the worker takes nothing of the engine: no engine name in the queue header
    for h in sorted(eng.glob('*.h')):
        assert 'VkCommandBuffer' not in h.read_text(encoding='utf-8', errors='replace'), 'NB-20b: %s takes a command buffer (another file could record into the slot)' % h.name
    core = (root / 'gfx/nb/nb_mt.h').read_text(encoding='utf-8')
    assert 'vulkan' not in re.sub(r'//[^\n]*', '', core).lower() and 'cv_idle.wait(' in core and 'cv_work.wait(' in core and not re.search(r'\byield\b|sleep_for|usleep', core), \
        'NB-20b: nb_mt.h stays engine-free and waits on condition variables only'
    for sw in ('DM_NB_MT', 'DM_NB_MT_FLUSH', 'DM_NB_MT_CHUNK_KB', 'DM_NB_MT_QUEUE', 'DM_NB_MT_CPUS'):
        assert '"%s"' % sw in mt, 'NB-20b: switch %s' % sw
    assert re.search(r'num\("DM_NB_MT", 0, 0, 3\)', mt), 'NB-20b: DM_NB_MT defaults to 0'
    print('PASS: NB-20b DM_NB_MT: %d vkCmd* names of %s all wrapped (%d wrappers), %d pointer commands through nbmt::P*, begin / end / stop / init '
          'hooks in place, no command buffer in engine headers' % (len(used), ' + '.join(files[:1]) + ' + %d .inc' % (len(files) - 1), len(wrapped), len(ptrs)))
# 16.11 store-hist (docs/dos2/DOS2_PORT.md "store-hist: учёт выброшенного store"): every read / overwrite note is gated by hist_on (or sits
# in a function only called under it); both planned begin paths drop the store DONT_CARE of banned images before the twin is made, and the
# plan's discards are marked with the IR pass (nb_seq / nb_node) for the VIOLATION line.
def storehist_check():
    s = (root / 'gfx/vulkan/engine/xbg_game.cpp').read_text(encoding='utf-8')
    names, bodies = _nb20a_scan(s)
    inside = ('hist_note_read', 'hist_note_overwrite', 'hist_begin', 'store_hist_mask', 'hist_keep')
    nl = chr(10); spans = []
    for f in inside:   # the bookkeeping helpers themselves (called only under hist_on): from their definition to the next top-level line
        d = re.search(r'\n(?:void|uint32_t|unsigned) ' + f + r'\(', s); assert d, 'store-hist: %s not defined' % f
        e = re.search(r'\n(?![ \t])', s[d.end():]); spans.append((d.start(), d.end() + (e.start() if e else 0)))
    calls = 0
    for m in re.finditer(r'\b(hist_note_read|hist_note_overwrite|hist_begin)\(', s):
        line = s[s.rfind(nl, 0, m.start()) + 1:s.find(nl, m.start())]
        if re.match(r'\s*(void|struct Image;)', line): continue   # definitions / prototypes
        if any(x <= m.start() < y for x, y in spans): continue
        calls += 1
        assert 'hist_on' in line, 'store-hist: ungated bookkeeping: %s' % line.strip()[:160]
    assert calls >= 10, 'store-hist: bookkeeping hooks missing (%d)' % calls
    for fn in ('nbrp_begin', 'nbrp_lazy_begin'):
        b = bodies.get(fn, [''])[0]
        assert 'nb_rp_keep_store(&t,hist_keep(' in b and b.find('nb_rp_keep_store') < b.find('lazy_twin('), 'store-hist: %s keeps banned stores before the twin' % fn
        assert 'open_pass.nb_seq=' in b and "hist_begin(" in b, 'store-hist: %s marks / judges like the heuristic' % fn
    assert 'xbg_hist_discarded(' in bodies['store_hist_pass_end'][0] and 'hist_on=store_hist_mode!=0 || nb_rp_mode==1 || nb_rp_mode==3' in s
    assert 'store_hist_mode)hist_note_read' not in s and 'hist_dead' not in s, 'store-hist: old fields / gates left'
    print('PASS: store-hist bookkeeping gated by hist_on (%d hooks);' % calls + ' planned begins keep banned stores; plan discards marked with the IR pass')


# NB-20b ctx (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-20b-ctx findings"): the class C globals moved into the recording context. Each is
# read through `#define <name> nbx::get(<name>_m0, nbx::K_<name>)` (xbg_nb_mx.inc): the main instance on every thread unless a window is
# open and the thread is a worker. The list here, the slot enum, the defines and the registrations must stay in step, nobody may name a
# main instance directly, and a pure header read after the defines must be pre-included by xbg_nb_mx.inc (its own identifiers would
# meet the macros).
NB20B_CTX = ('bind_cache buf_gen cur_op desc_memo_dyn desc_memo_images desc_memo_local desc_memo_recording desc_memo_texel draw_reads '
             'draw_reads_overflow g_ui_moff g_ui_moff_addr img_front in_descriptors memo_dyn_ok memo_front_gen memo_front_it memo_front_key '
             'memo_front_slot pipe_memo_key pipe_memo_next pipe_memo_pipeline pipe_memo_valid snap_site state storage_images_bound tex_gen').split()
def nb20b_ctx_check():
    eng = root / 'gfx/vulkan/engine'
    s = (eng / 'xbg_game.cpp').read_text(encoding='utf-8')
    mx = (eng / 'xbg_nb_mx.inc').read_text(encoding='utf-8')
    known = set()
    for c, v in NB20A_DRAW_GLOBALS.items():
        if c != 'N': known |= set(v.split())
    both = sorted(set(NB20B_CTX) & known)
    assert not both, 'NB-20b ctx: listed in the NB-20a table and as a context variable: %s' % both
    m = re.search(r'enum Slot \{(.*?)\};', mx, flags=re.S)
    slots = [t.split('=')[0].strip() for t in m.group(1).split(',')]
    assert slots[-1] == 'K_COUNT' and sorted(x[2:] for x in slots[:-1]) == sorted(NB20B_CTX), \
        'NB-20b ctx: the slot enum of xbg_nb_mx.inc and NB20B_CTX differ: %s' % sorted(set(x[2:] for x in slots[:-1]) ^ set(NB20B_CTX))
    code = _nb20a_strip(s)   # no comments, strings or preprocessor lines
    first_def = len(s)
    for n in NB20B_CTX:
        d = '#define %s nbx::get(%s_m0,nbx::K_%s)' % (n, n, n)
        assert s.count(d) == 1, 'NB-20b ctx: %s needs exactly one `%s`' % (n, d)
        first_def = min(first_def, s.find(d))
        assert len(re.findall(r'nbx::reg\(nbx::K_%s,&%s_m0, *' % (n, n), code)) == 1, 'NB-20b ctx: %s is not registered once' % n
        uses = len(re.findall(r'\b%s_m0\b' % n, code))
        assert uses == (3 if n == 'in_descriptors' else 2), ('NB-20b ctx: the main instance %s_m0 is named %d times outside its declaration and registration '
                                                              '(use %s: a worker has its own)' % (n, uses, n))
        assert '#undef %s' % n not in s, 'NB-20b ctx: %s is undefined somewhere' % n
    assert s.find('#include "xbg_nb_mx.inc"') > s.find('#include "xbg_nb_mt.inc"') > 0 and s.find('#include "xbg_nb_mx.inc"') < first_def, 'NB-20b ctx: xbg_nb_mx.inc right after xbg_nb_mt.inc, before the defines'
    pre = set(re.findall(r'^#include "([^"]+)"', mx, flags=re.M))
    late = set(re.findall(r'^#include "((?:\.\./\.\./nb/|xbg_)[^"]+\.h)"', s[first_def:], flags=re.M))
    for f in sorted(eng.glob('*.inc')):
        if f.name in ('xbg_nb_mt.inc', 'xbg_nb_mx.inc'): continue
        late |= set(re.findall(r'^#include "((?:\.\./\.\./nb/|xbg_)[^"]+\.h)"', f.read_text(encoding='utf-8'), flags=re.M))
    missing = sorted(h for h in late if h not in pre)
    assert not missing, 'NB-20b ctx: headers read after the context defines but not pre-included by xbg_nb_mx.inc: %s' % missing
    names, bodies = _nb20a_scan(s)
    assert 'nbx::submit(n,handles,budget,ab_off(' in bodies['xbg_submit_now'][0] and s.count('nbx::init();') == 1 and 'nbx::stop();' in bodies['destroy_slots'][0], \
        'NB-20b ctx: nbx::submit in xbg_submit_now / nbx::init at the switches / nbx::stop in destroy_slots'
    core = (root / 'gfx/nb/nb_mx.h').read_text(encoding='utf-8')
    assert 'vulkan' not in re.sub(r'//[^\n]*', '', core).lower() and not re.search(r'\byield\b|sleep_for|usleep', core), 'NB-20b ctx: nb_mx.h stays engine-free, waits on condition variables only'
    assert re.search(r'num\("DM_NB_MX", 0, 0, 1\)', mx) and re.search(r'num\("DM_NB_MX_PROF", 0, 0, 1\)', mx), 'NB-20b ctx: DM_NB_MX / DM_NB_MX_PROF default to 0'
    print('PASS: NB-20b ctx: %d context variables (slot enum = defines = registrations, no direct use of a main instance), %d late headers '
          'pre-included, submit / init / stop hooks in place' % (len(NB20B_CTX), len(late)))


# NR-01 (engine object registry of the native renderer, DM_NR, default 0): the wiring that keeps "off" byte-identical.
#   - hle_nvn.c: exactly four tap lines, one per entry point (nvnTextureInitialize, nvnProgramSetShaders, nvnSamplerInitialize,
#     nvnBufferInitialize), each an NRT(...) = a test of g_dm_nr; the tap functions only read the host object;
#   - hle_dos2_nr.c: g_dm_nr starts 0 and is set only inside dm_dos2_nr_install, which returns before allocating / hooking anything
#     unless DM_NR is 1 or 2; hooks (dn_hook / dn_install_rc) are installed nowhere else; every hook is NR_HOOK (never a tail call:
#     a sibling call would write its stack arguments into the game's caller frame), calls its own trampoline once with all 28
#     forwarded values, and is listed once in k_nr_site;
#   - main.c calls the install after the NB one, inside the DOS2 block; the registry core stays free of engine / host / game addresses.
def nr01_check():
    def code(text):   # no comments, no strings
        text = re.sub(r'/\*.*?\*/|//[^\n]*', ' ', text, flags=re.S)
        return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    def body(text, head):   # the brace-matched body after the first `head`
        i = text.index(head); i = text.index('{', i); depth, j = 0, i
        while True:
            if text[j] == '{': depth += 1
            elif text[j] == '}':
                depth -= 1
                if not depth: return text[i:j + 1]
            j += 1
    nvn = code((root / 'gfx/nvn/hle_nvn.c').read_text(encoding='utf-8'))
    inc_raw = (root / 'gfx/nvn/nvn_nr_tap.inc').read_text(encoding='utf-8')
    inc = code(inc_raw)
    host_raw = (root / 'host/hle_dos2_nr.c').read_text(encoding='utf-8')
    host = code(host_raw)
    # the tap
    assert (root / 'gfx/nvn/hle_nvn.c').read_text(encoding='utf-8').count('#include "nvn_nr_tap.inc"') == 1, 'NR-01: hle_nvn.c includes nvn_nr_tap.inc once'
    assert nvn.count('NRT(') == 4, 'NR-01: hle_nvn.c has exactly four NRT(...) tap lines, found %d' % nvn.count('NRT(')
    for fn, call in (('nvnTextureInitialize', 'NRT(nrt_texture(o));'), ('nvnProgramSetShaders', 'NRT(nrt_program(o));'), ('nvnSamplerInitialize', 'NRT(nrt_sampler(o));'),
                     ('nvnBufferInitialize', 'NRT(dm_nr_tap_buffer(o, o->size));')):
        b = body(nvn, 'NVN(%s,' % fn)
        assert b.count('NRT(') == 1 and call in b, 'NR-01: %s needs the one tap line `%s`' % (fn, call)
        tail = b[b.index(call) + len(call):]
        assert re.fullmatch(r'\s*return 1;\s*\}', tail), 'NR-01: in %s the tap line is the last statement before `return 1;` (the object is complete, the result unchanged)' % fn
    assert '#define NRT(x) do { if (__builtin_expect(g_dm_nr != 0, 0)) { x; } } while (0)' in inc_raw, 'NR-01: NRT is a guarded line (g_dm_nr != 0)'
    assert re.search(r'#ifdef DM_GAME_DOS2.*#define NRT\(x\) do \{ if.*#else\s*#define NRT\(x\) do \{ \} while \(0\)\s*#endif', inc_raw, flags=re.S), 'NR-01: NRT is empty outside DOS2'
    for fn in ('nrt_texture', 'nrt_program', 'nrt_sampler'):
        b = body(inc, 'static void %s(const ' % fn)
        assert not re.search(r'(?<![=!<>])=(?!=)', b), 'NR-01: %s only reads the host object' % fn
    # the front end
    assert re.search(r'^int g_dm_nr;', host, flags=re.M), 'NR-01: g_dm_nr has no initialiser (0 = off)'
    inst = body(host, 'void dm_dos2_nr_install(void) {')
    gate = 'if (mode != 1 && mode != 2) return;'
    assert 'getenv("DM_NR")' in body(host_raw, 'void dm_dos2_nr_install(void) {') and gate in inst and 'const int mode = e ? atoi(e) : 0;' in inst, 'NR-01: DM_NR unset / 0 / anything but 1, 2 returns at once (default 0)'
    for later in ('nr_reg_new(', 'dn_hook(', 'dn_install_rc(', 'g_dm_nr = mode', 'dn_main('):
        assert later in inst and inst.index(gate) < inst.index(later), 'NR-01: `%s` only after the DM_NR gate' % later
    rest = host.replace(inst, '')
    assert 'dn_hook(' not in rest.replace('int dn_hook(module_t', '') and 'dn_install_rc(' not in rest.replace('int dn_install_rc(const', ''), 'NR-01: hooks are installed only by dm_dos2_nr_install'
    assert len(re.findall(r'\bg_dm_nr\s*=(?!=)', host)) == 2 and len(re.findall(r'\bg_dm_nr\s*=(?!=)', rest)) == 0, 'NR-01: g_dm_nr is set only inside the install'
    assert '#define NR_HOOK static __attribute__((disable_tail_calls)) uint64_t' in host_raw, 'NR-01: NR_HOOK forbids tail calls'
    hooks = re.findall(r'^NR_HOOK (nr_h_\w+)\(NR_A\)', host, flags=re.M)
    assert len(hooks) >= 18 and len(set(hooks)) == len(hooks) and not re.search(r'^static\s+uint64_t\s+nr_h_', host, flags=re.M), 'NR-01: every hook is declared NR_HOOK'
    table = body(host, 'k_nr_site[] =')
    for h in hooks:
        b = body(host, 'NR_HOOK %s(NR_A)' % h)
        orig = 'g_o_' + h[len('nr_h_'):]
        assert b.count(orig + '(NR_F)') == 1 and len(re.findall(r'\bg_o_\w+\(', b)) == 1, 'NR-01: %s calls its own trampoline %s once with the forwarded arguments' % (h, orig)
        assert table.count('(void *)%s, &%s}' % (h, orig)) == 1, 'NR-01: %s is listed once in k_nr_site with %s' % (h, orig)
    assert table.count('(void *)nr_h_') == len(hooks), 'NR-01: k_nr_site rows = hooks'
    assert len(set(re.findall(r'\{(0x[0-9a-f]+), \{', table))) == len(hooks), 'NR-01: one site per hook, no address twice'
    assert 'if (!dn_words_ok(m, s->off, s->pro))' in inst and inst.index('if (!dn_words_ok(m, s->off, s->pro))') < inst.index('dn_hook('), 'NR-01: prologue words are compared before a site is patched'
    assert re.search(r'if \(op != 55 \|\| !pkt\) return;', body(host, 'static void nr_rc_pre(')), 'NR-01: the render-command handler leaves every opcode but 55 at once'
    assert 'malloc(' not in host and 'calloc(' not in host, 'NR-01: the front end allocates nothing itself (nr_reg_new at install only)'
    # main.c
    main_c = (root / 'host/main.c').read_text(encoding='utf-8')
    a, b_ = main_c.find('dm_dos2_nb_install(); }'), main_c.find('{ void dm_dos2_nr_install(void); dm_dos2_nr_install(); }')
    dos2 = main_c.rfind('#ifdef DM_GAME_DOS2', 0, b_)
    assert 0 < a < b_ and main_c.count('dm_dos2_nr_install();') == 1 and dos2 > 0 and '#endif' not in main_c[dos2:b_], 'NR-01: main.c calls dm_dos2_nr_install once, after the NB install, inside the DOS2 block'
    # the registry core: host-testable
    sites = set(re.findall(r'\{(0x[0-9a-f]+), \{', table))
    for name in ('nr_reg.h', 'nr_reg.c'):
        core_raw = (root / 'gfx/nr' / name).read_text(encoding='utf-8'); core = code(core_raw)
        assert 'vulkan' not in core.lower() and 'hle.h' not in core_raw and 'pthread' not in core and 'getenv' not in core, 'NR-01: %s stays free of the engine, the host and the environment' % name
        assert not any(s in core for s in sites), 'NR-01: %s holds no game address' % name
    rows = re.findall(r'^    \{"\w+", 0x[0-9a-f]+, 0x[0-9a-f]+, \d+, \d+, \d+\},$', (root / 'gfx/nr/nr_reg.c').read_text(encoding='utf-8'), flags=re.M)
    assert len(rows) == 63, 'NR-01: the engine format table has 63 rows (found %d)' % len(rows)
    for cm in ('android/CMakeLists.txt', 'harmony/native/CMakeLists.txt'):
        assert '${DM}/gfx/nr/*.c' in (root / cm).read_text(encoding='utf-8'), 'NR-01: %s builds dm/gfx/nr' % cm
    print('PASS: NR-01: 4 guarded tap lines (last statement before return), DM_NR default 0 gate before any hook / allocation, %d hooks (NR_HOOK, own trampoline once, '
          'listed once), opcode 55 only, registry core engine-free, 63 format rows' % len(hooks))


nb20a_check()
nb19_check()
nb20b_check()
nb20b_ctx_check()
storehist_check()
nr01_check()
_bad=delimiter_gate()
assert not _bad,_bad
