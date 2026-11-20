"""Divinity: Original Sin 2 DE: static check of everything the engine object registry (NR-01, DM_NR, dm/host/hle_dos2_nr.c) relies on.

Reads the game's main (work/exefs-update/main.raw + main.symbols.json, build id checked) with python + capstone + unicorn, no Ghidra.
The source files are the single truth: the hook table is parsed out of dm/host/hle_dos2_nr.c, the engine format table out of
dm/gfx/nr/nr_reg.c.
  1. Hook sites (k_nr_site): the 4 prologue words of every site against main.raw, none PC-relative, the word before the start is a
     function boundary, no direct branch anywhere in .text lands in the 16-byte patch window (start+4..start+15), and - because every
     hook forwards x0-x7, d0-d7 and 12 stack slots but NOT x8 nor the upper halves of v0-v7 - no path from the entry reads x8 or a
     vector register v0-v7 before writing it (a call reached with x8 unwritten counts as a read).
  2. The addresses the hook file documents but does not hook (0x919c20 leaf switch, 0x91eea0 adrp in the prologue) are confirmed
     unhookable, so nobody adds them later by mistake.
  3. Engine format table (63 rows): NVN format and sRGB NVN format of each engine format = 0x919c20(fmt, srgb) run in unicorn
     (jump table 0x1f5051c); name / channels / bytes / compressed flag = 0x91eea0(out, fmt) (jump table 0x1f5058c).
  4. Small tables the DM_NR=2 checks embed: sampler min filter (0x1f585e8), and the image format pairs of CreateTextureFromImage are
     listed for the eye.
  5. Engine opcode 55 (resource transition): its handler returns packet length 0x20 on every path that returns.
  6. NR-02 (DM_NR_KEY, dm/host/hle_dos2_nr_key.inc): the hook of the texture flush 0x924730 (in item 1 with the others); every
     state-cache store / read the draw key relies on, instruction by instruction; the .inc's offsets equal the checked ones; every call
     of 0x924730 in main is in k_nk_site, and the first NVN call after each site is the draw (or dispatch / nothing) its kind says.
  7. Optional (--so work/build-android/libdm.so): no hook in the built library tail-calls the trampoline or stores into its incoming
     argument area (that would write into the game's caller frame).
Exit code 1 on any failure.
Usage: dos2_nr_check.py [--main work/exefs-update/main.raw] [--so work/build-android/libdm.so]
"""
import argparse, re, struct, sys
from pathlib import Path
import capstone

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dos2_nb_check as nb   # Main (build id check), branch, pcrel, inbound, jump_table

FMT_NVN, FMT_NVN_TABLE, FMT_INFO, FMT_INFO_TABLE, ASSERT_FN = 0x919c20, 0x1f5051c, 0x91eea0, 0x1f5058c, 0x80f560
SAMPLER_MIN_TABLE, NOT_HOOKABLE = 0x1f585e8, ((0x919c20, 'engine format -> NVN format'), (0x91eea0, 'GetFormatInfo'))
ARG_REGS = ['x%d' % i for i in range(8)]


def sites():
    """The hook tables: k_nr_site of hle_dos2_nr.c (NR-01) and k_nr_key_site of hle_dos2_nr_key.inc (NR-02, DM_NR_KEY)."""
    out = []
    for f, table in (('dm/host/hle_dos2_nr.c', 'k_nr_site[] = {'), ('dm/host/hle_dos2_nr_key.inc', 'k_nr_key_site[] = {'),
                     ('dm/host/hle_dos2_nrn_ev.inc', 'k_nrnev_site[] = {'), ('dm/host/hle_dos2_nrn_d.inc', 'k_nrnd_site[] = {')):   # NR-NATIVE step E (DM_NRN_EV), step D (the draw-yourself wrappers and the builder brackets)
        s = (ROOT / f).read_text(encoding='utf-8')
        body = s[s.index(table):]
        body = re.sub(r' */\*.*?\*/', '', body[:body.index('\n};')])   # the instruction comments hold commas
        got = [(int(m.group(1), 16), [int(m.group(k), 16) for k in range(2, 6)], m.group(6), m.group(7))
               for m in re.finditer(r'\{(0x[0-9a-f]+), \{(0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\}, "([^"]+)", \(void \*\)(\w+), &(\w+)\}', body)]
        rows = body.count('(void *)nr_h_')
        if len(got) != rows or not got: sys.exit('%s %s: parsed %d of %d rows' % (f, table, len(got), rows))
        out += got
    # [DOS2] VK-NATIVE: every row dm_vkn_site(0x..., k_x, "name", (void *)hook, &orig, modes) of dm/host/hle_dos2_vkn*.c with its words
    # `static const uint32_t k_x[4] = {...}` (same file)
    for f in sorted((ROOT / 'dm/host').glob('hle_dos2_vkn*.c')):
        s = re.sub(r' */\*.*?\*/', '', f.read_text(encoding='utf-8'))
        words = {m.group(1): [int(m.group(k), 16) for k in range(2, 6)]
                 for m in re.finditer(r'static const uint32_t (k_\w+)\[4\] = \{(0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\};', s)}
        got = [(int(m.group(1), 16), words.get(m.group(2)), m.group(3), m.group(4))
               for m in re.finditer(r'dm_vkn_site\((0x[0-9a-f]+), (k_\w+), "([^"]+)", \(void \*\)(\w+), &\w+, [^;\n]*\);', s)]
        rows = s.count('    dm_vkn_site(0x')
        if len(got) != rows or any(g[1] is None for g in got): sys.exit('%s: parsed %d of %d dm_vkn_site rows (or a row without its words)' % (f.name, len(got), rows))
        out += got
    return out


# ---- NR-02 (DM_NR_KEY): the state cache fields the draw key reads, the call sites of the texture flush, the pool id ----
TEX_FLUSH = 0x924730
KEY_INC = 'dm/host/hle_dos2_nr_key.inc'
# (address, instruction, what it proves): stores of the state cache by the engine's own bind code, reads of the flush, pool id stores.
KEY_INS = [
    (0x9283c0, 'str x8, [x0, #0x17d8]', 'SetShader 0x9283a0 stores the shader handle at r+0x17d8'),
    (0x928408, 'str x20, [x0, #0x17e0]', '... and its record at r+0x17e0'),
    (0x928418, 'ldr x1, [x20, #8]', '... record +8 = the program bound for stage bits 1 (vertex)'),
    (0x928444, 'ldr x1, [x20, #0x10]', '... record +0x10 = the program bound for stage bits 2 (fragment)'),
    (0x928548, 'str x9, [x19, #0x16f8]', 'SetBlend 0x9284e0 stores the blend handle at r+0x16f8'),
    (0x92854c, 'str w2, [x19, #0x1700]', '... and its mask word at r+0x1700'),
    (0x93ecf4, 'str x9, [x20, #0x1708]', 'code 21 stores the depth-stencil handle at r+0x1708'),
    (0x93ecf8, 'str w21, [x20, #0x1710]', '... and the stencil reference at r+0x1710 (dynamic, not in the key)'),
    (0x93e9a4, 'str x8, [x20, #0x16f0]', 'code 21 stores the raster (polygon state) handle at r+0x16f0'),
    (0x93f498, 'str x9, [x8, #0x16f0]', 'code 7 stores it too'),
    (0x93e908, 'str x8, [x20, #0x1740]', 'code 21 stores the vertex format handle at r+0x1740'),
    (0x93eb58, 'str x8, [x20, #0x1740]', 'code 10 stores it too'),
    (0x93e724, 'mov w21, #0x1714', 'code 21: x21 = r+0x1714 ...'),
    (0x93e7c4, 'add x21, x20, x21', '...'),
    (0x93f218, 'strb w8, [x21]', '... the engine primitive byte at r+0x1714'),
    (0x93f2c0, 'str w8, [x20, #0x1718]', '... and its NVN primitive at r+0x1718'),
    (0x927b3c, 'str x8, [x19, #0x17e8]', 'SetRenderTargets 0x9279a0 stores colour target 0 at r+0x17e8'),
    (0x927d08, 'str x20, [x19, #0x1828]', '... and the depth target at r+0x1828'),
    (0x93f150, 'str s8, [x20, #0x1868]', 'code 21 stores the alpha reference at r+0x1868'),
    (0x9285a4, 'fcmp s0, #0.0', 'SetBlend: alpha test by the sign of r+0x1868 ...'),
    (0x9285a8, 'b.ge #0x9287ec', '... on when >= 0'),
    (0x92475c, 'mov w8, #0x15dc', 'the flush: vertex texture dirty range min at r+0x15dc ...'),
    (0x924760, 'mov w9, #0x15dd', '... max at r+0x15dd'),
    (0x924778, 'cmp w8, w26', '... min <= max = dirty'),
    (0x92477c, 'b.le #0x9248fc', '...'),
    (0x92474c, 'mov w8, #0x1664', 'fragment texture dirty range at r+0x1664 / +0x1665'),
    (0x924780, 'mov w8, #0x20', 'clean = 32 ...'),
    (0x924788, 'mov w8, #0xff', '... and -1'),
    (0x924794, 'strh w24, [x19, #0x1444]', 'the flush resets the vertex sampler range r+0x1444'),
    (0x924864, 'strh w24, [x19, #0x14cc]', '... and the fragment one r+0x14cc'),
    (0x924918, 'ldr w1, [x8, #0x1558]', 'vertex texture ids at r+0x1558 + 4 slot'),
    (0x924970, 'ldr w2, [x8, #0x13c0]', 'vertex sampler ids at r+0x13c0 + 4 slot'),
    (0x9247b0, 'ldr w1, [x8, #0x15e0]', 'fragment texture ids at r+0x15e0 + 4 slot'),
    (0x9247cc, 'ldr w2, [x8, #0x1448]', 'fragment sampler ids at r+0x1448 + 4 slot'),
    (0x9243ec, 'ldr w8, [x11, #0xc0]', 'the slot writer 0x9243d0 takes the id at NVN texture object + 0xc0'),
    (0x9244fc, 'mov w13, #0x1558', '... writes vertex texture ids'),
    (0x92441c, 'mov w13, #0x15e0', '... fragment texture ids'),
    (0x92445c, 'mov w13, #0x1448', '... fragment sampler ids'),
    (0x924468, 'mov w12, #0x14cd', '... and widens the fragment sampler range'),
    (0x91e9a0, 'str w22, [x2, #0xc0]', 'CreateTexture stores the pool id at NVN texture object + 0xc0'),
    (0x91eb24, 'str w23, [x8, #0xc0]', '... the view ids after it (+0xc4 + 4 i)'),
    (0x91e9e8, 'mov w8, #6', '... 6 views for a cube, else the layer count'),
    (0x91f63c, 'str w21, [x8, #0xc0]', 'CreateTextureFromImage stores the pool id at +0xc0'),
    (0x93c6ac, 'str w23, [x8, #0xc0]', 'window textures: pool id at +0xc0'),
    (0x93c960, 'str w20, [x8, #0xc0]', '...'),
]
KEY_R = {'NK_R_RASTER': 0x16f0, 'NK_R_BLEND': 0x16f8, 'NK_R_BLEND_ARG': 0x1700, 'NK_R_DEPTH': 0x1708, 'NK_R_PRIM': 0x1714, 'NK_R_VFORMAT': 0x1740,
         'NK_R_SHADER': 0x17d8, 'NK_R_SHADER_REC': 0x17e0, 'NK_R_COLOR': 0x17e8, 'NK_R_DEPTH_RT': 0x1828, 'NK_R_ALPHA_REF': 0x1868,
         'NK_R_TEX_VS': 0x1558, 'NK_R_TEX_FS': 0x15e0, 'NK_R_SMP_VS': 0x13c0, 'NK_R_SMP_FS': 0x1448}
DRAW_KINDS = {'NR_DS_LIST_IDX': ('DrawElements',), 'NR_DS_IDX_INST': ('DrawElementsInstanced',), 'NR_DS_FULL': ('DrawElements', 'DrawArrays'),
              'NR_DS_LIST_ARRAYS': ('DrawArrays',), 'NR_DS_DIRECT_IDX': ('DrawElements',), 'NR_DS_DIRECT_ARRAYS': ('DrawArrays',)}


def key_sites():
    s = (ROOT / KEY_INC).read_text(encoding='utf-8')
    body = s[s.index('k_nk_site[] = {'):]
    body = body[:body.index('\n};')]
    return [(int(a, 16), k) for a, k in re.findall(r'\{(0x[0-9a-f]+), (NR_DS_\w+)\}', body)]


def calls_to(m, target):
    """Every bl / b in main's text whose target is `target`."""
    import numpy as np
    w = m.w.astype(np.int64)
    imm = w & 0x3ffffff
    imm = np.where(imm & 0x2000000, imm - 0x4000000, imm)
    tgt = np.arange(m.N, dtype=np.int64) * 4 + imm * 4
    hit = ((w & 0x7c000000) == 0x14000000) & (tgt == target)   # b (0x14) and bl (0x94)
    return [int(i) * 4 for i in np.nonzero(hit)[0]]


def first_nvn_after(m, use, pc, limit=48):
    """The first NVN slot read after pc on the straight path (following b), or None."""
    for _ in range(limit):
        if pc in use: return use[pc]
        k, t = nb.branch(m.u32(pc), pc)
        if k == 'ret': return None
        pc = t if k == 'b' else pc + 4
    return None


def formats():
    s = (ROOT / 'dm/gfx/nr/nr_reg.c').read_text(encoding='utf-8')
    body = s[s.index('const nr_fmt_row nr_formats[NR_FMT_N] = {'):]
    body = body[:body.index('\n};')]
    rows = [(m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4)), int(m.group(5)), int(m.group(6)))
            for m in re.finditer(r'\{"([^"]+)", (0x[0-9a-f]+), (0x[0-9a-f]+), (\d+), (\d+), (\d+)\},', body)]
    return rows


def fp_index(name):
    """(index, is_vector) of an FP / SIMD register name, or None."""
    m = re.fullmatch(r'([bhsdqv])(\d+)', name)
    return (int(m.group(2)), m.group(1) in 'qv') if m else None


def early_reads(m, start, limit=600, depth=0, fp=True):
    """Reads of x8 / v0-v7 before a write on any path from start (DFS over direct branches). A call reached with x8 unwritten hands
    the caller's x8 on: an import (C++ API called without an indirect-result pointer) cannot use it, a function of main is walked
    the same way (x8 only, 3 levels), an indirect call is reported."""
    found, seen, work = [], set(), [(start, False, frozenset() if fp else frozenset(range(8)))]
    n = 0
    while work and n < limit:
        pc, x8w, fpw = work.pop()
        while n < limit:
            if (pc, x8w, fpw) in seen: break
            seen.add((pc, x8w, fpw)); n += 1
            i = m.dis(pc); x = m.u32(pc)
            if i is None: break
            k, t = nb.branch(x, pc)
            rd, wr = i.regs_access()
            rd = [i.reg_name(r) for r in rd]; wr = [i.reg_name(r) for r in wr]
            if not x8w and any(r in ('x8', 'w8') for r in rd): found.append('0x%x reads x8: %s %s' % (pc, i.mnemonic, i.op_str))
            for r in rd:
                f = fp_index(r)
                if f and f[0] < 8 and f[0] not in fpw:
                    found.append('0x%x reads %s%s: %s %s' % (pc, r, ' (VECTOR: not forwarded)' if f[1] else ' (scalar: forwarded as d%d)' % f[0], i.mnemonic, i.op_str))
            if any(r in ('x8', 'w8') for r in wr): x8w = True
            fpw = fpw | frozenset(f[0] for f in (fp_index(r) for r in wr) if f and f[0] < 8)
            if k in ('bl', 'blr'):
                if not x8w:
                    if k == 'blr': found.append('0x%x indirect call with x8 never written (x8 may be handed on): %s %s' % (pc, i.mnemonic, i.op_str))
                    elif not m.plt_name(t):
                        if depth >= 3: found.append('0x%x call chain with x8 never written deeper than 3: bl 0x%x' % (pc, t))
                        else: found += ['via bl 0x%x at 0x%x: %s' % (t, pc, e) for e in early_reads(m, t, limit, depth + 1, False)[0]]
                x8w = True; fpw = frozenset(range(8))   # caller-saved: dead after the call
            if x8w and len(fpw) == 8: break               # nothing left to find on this path
            if k in ('ret', 'br', 'stop'): break
            if k == 'b': pc = t; continue
            if k == 'cond': work.append((t, x8w, fpw))
            pc += 4
    return found, n


def lengths55(m, h):
    """Packet lengths (last mov w0, #imm) on every path of the opcode-55 handler that reaches the common epilogue; a path into the
    engine's assert (no-return) ends there."""
    out, seen, work = set(), set(), [(h, None)]
    while work:
        pc, last = work.pop()
        for _ in range(200):
            if (pc, last) in seen: break
            seen.add((pc, last))
            if pc == nb.ERC_EPI: out.add(last); break
            i = m.dis(pc); x = m.u32(pc)
            k, t = nb.branch(x, pc)
            if i is not None and i.mnemonic == 'mov' and re.match(r'[wx]0, #', i.op_str): last = int(i.op_str.split('#')[1], 0)
            elif k in ('bl', 'blr'):
                if k == 'bl' and t == ASSERT_FN: break
                last = None   # x0 = whatever the call returned
            if k in ('ret', 'br', 'stop'): out.add('%s at 0x%x' % (k, pc)); break
            if k == 'b': pc = t; continue
            if k == 'cond': work.append((t, last))
            pc += 4
    return out


def emu():
    try:
        import unicorn
        from unicorn import arm64_const as uc
    except ImportError:
        sys.exit('pip install unicorn (the engine format table is checked by running the two leaf functions)')
    return unicorn, uc


def run(m, addr, x0, x1, out_mem=False):
    """Run a leaf function of main at addr(x0, x1). Returns ('ret', w0, bytes written at the out buffer) or ('trap', pc)."""
    unicorn, uc = emu()
    mu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    size = (len(m.d) + 0xfff) & ~0xfff
    mu.mem_map(0, size); mu.mem_write(0, m.d)
    STACK, RET, OUT = 0x10000000, 0x10100000, 0x10200000
    mu.mem_map(STACK, 0x300000)
    mu.reg_write(uc.UC_ARM64_REG_SP, STACK + 0x80000); mu.reg_write(uc.UC_ARM64_REG_X30, RET)
    mu.reg_write(uc.UC_ARM64_REG_X0, OUT if out_mem else x0); mu.reg_write(uc.UC_ARM64_REG_X1, x1)
    stop = {}
    def hook(u, address, sz, user):
        if address == ASSERT_FN: stop['trap'] = address; u.emu_stop()
    mu.hook_add(unicorn.UC_HOOK_CODE, hook)
    try: mu.emu_start(addr, RET, count=2000)
    except unicorn.UcError as e: return ('trap', str(e))
    if stop: return ('trap', stop['trap'])
    if mu.reg_read(uc.UC_ARM64_REG_PC) != RET: return ('trap', 'did not return')
    return ('ret', mu.reg_read(uc.UC_ARM64_REG_X0) & 0xffffffff, bytes(mu.mem_read(OUT, 0x20)) if out_mem else b'')


def check_so(path, hooks):
    """No hook tail-calls (br) or stores at / above its incoming stack arguments. ELF64 .symtab + capstone."""
    d = Path(path).read_bytes()
    if d[:4] != b'\x7fELF' or d[4] != 2: return ['%s: not an ELF64 file' % path], 0
    shoff, = struct.unpack_from('<Q', d, 0x28); shentsize, shnum = struct.unpack_from('<HH', d, 0x3a)
    sec = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]   # name type flags addr off size link info align entsize
    symtab = next((s for s in sec if s[1] == 2), None)
    if symtab is None: return ['%s: no .symtab (stripped): build tree library expected' % path], 0
    strtab = sec[symtab[6]]
    funcs = {}
    for o in range(symtab[4], symtab[4] + symtab[5], 24):
        name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, o)
        if info & 15 != 2 or not size: continue
        e = d.index(b'\0', strtab[4] + name)
        funcs[d[strtab[4] + name:e].decode('latin1')] = (value, size, shndx)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    bad, n = [], 0
    for h in hooks:
        if h not in funcs: bad.append('%s: not in %s' % (h, path)); continue
        value, size, shndx = funcs[h]
        s = sec[shndx]
        code = d[s[4] + value - s[3]:s[4] + value - s[3] + size]
        ins = list(md.disasm(code, value))
        frame, fp = 0, None
        for i in ins[:24]:   # the prologue: pre-index pushes and sub sp, then x29
            a = re.search(r'\[sp, #-(0x[0-9a-f]+|\d+)\]!', i.op_str)
            if a: frame += int(a.group(1), 0)
            if i.mnemonic == 'sub' and i.op_str.startswith('sp, sp, #'):
                parts = i.op_str.split('#')
                frame += int(parts[1].rstrip(', lsl '), 0) << (12 if 'lsl #12' in i.op_str else 0)
            if fp is None and i.mnemonic in ('add', 'mov') and i.op_str.startswith('x29, sp'):
                c = re.search(r'#(0x[0-9a-f]+|\d+)', i.op_str)
                fp = frame - (int(c.group(1), 0) if c else 0)   # incoming stack arguments start at x29 + fp
        if not frame: bad.append('%s: no stack frame found' % h); continue
        calls = sum(i.mnemonic == 'blr' for i in ins)
        if any(i.mnemonic == 'br' for i in ins): bad.append('%s: indirect tail jump (br) - a sibling call of the trampoline' % h)
        if not calls: bad.append('%s: no indirect call (the trampoline is not called?)' % h)
        for i in ins:
            if not i.mnemonic.startswith('st'): continue
            a = re.search(r'\[sp, #(0x[0-9a-f]+|\d+)\]$', i.op_str); b = re.search(r'\[x29, #(0x[0-9a-f]+|\d+)\]$', i.op_str)
            if (a and int(a.group(1), 0) >= frame) or (b and fp is not None and int(b.group(1), 0) >= fp):
                bad.append('%s: 0x%x stores into the caller frame: %s %s' % (h, i.address, i.mnemonic, i.op_str))
        n += 1
    return bad, n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--main', default=str(ROOT.parent / 'work' / 'exefs-update' / 'main.raw'))
    ap.add_argument('--so', default='')
    a = ap.parse_args()
    m = nb.Main(a.main)
    bad = 0

    hooks = sites()
    print('== %d hook sites of dm/host/hle_dos2_nr.c + hle_dos2_nr_key.inc: prologue words, PC-relative, boundary, inbound branches, x8 / vector argument reads' % len(hooks))
    for addr, words, name, fn in hooks:
        got = [m.u32(addr + 4 * k) for k in range(4)]
        ins = [m.dis(addr + 4 * k) for k in range(4)]
        txt = '; '.join('%s %s' % (i.mnemonic, i.op_str) for i in ins)
        rel = [k for k in range(4) if nb.pcrel(m.u32(addr + 4 * k))]
        pk, _ = nb.branch(m.u32(addr - 4), addr - 4)
        boundary = {'ret': 'ret', 'br': 'br', 'b': 'b', 'stop': 'udf/brk', 'bl': 'bl (no-return call)'}.get(pk, 'NONE')
        inb = nb.inbound(m, addr + 4, addr + 16)
        early, walked = early_reads(m, addr)
        fatal = [e for e in early if 'scalar' not in e]
        ok = got == words and not rel and boundary != 'NONE' and not inb and not fatal
        if not ok: bad += 1
        print('%-4s 0x%07x %-24s %-9s prev=%-18s %s' % ('ok' if ok else 'FAIL', addr, name, fn, boundary, txt))
        if got != words: print('       WORDS differ: main %s, source %s' % (' '.join('%08x' % w for w in got), ' '.join('%08x' % w for w in words)))
        if rel: print('       PC-RELATIVE at +%s' % ','.join(str(4 * k) for k in rel))
        if inb: print('       INBOUND ' + ' '.join('0x%x->0x%x' % h for h in inb[:6]))
        for e in early: print('       %s' % e)
    # (a VK-NATIVE hook - function name vkn_h_* / vb_h_* - may sit on a site an older module hooked: dn_hook_chain chains them)
    vk = [h for h in hooks if h[3].startswith(('vkn_h_', 'vb_h_'))]; old = [h for h in hooks if not h[3].startswith(('vkn_h_', 'vb_h_'))]
    dup = len(old) - len({h[0] for h in old}) + len(vk) - len({h[0] for h in vk})
    if dup: bad += 1; print('FAIL: %d duplicate hook addresses' % dup)

    print('\n== not hookable, as the hook file says')
    for addr, name in NOT_HOOKABLE:
        rel = [k for k in range(4) if nb.pcrel(m.u32(addr + 4 * k))]
        hooked = any(h[0] == addr for h in hooks)
        ok = bool(rel) and not hooked
        if not ok: bad += 1
        print('%-4s 0x%07x %-32s PC-relative / branch in the first 4 instructions at +%s%s' % ('ok' if ok else 'FAIL', addr, name, ','.join(str(4 * k) for k in rel) or 'none',
                                                                                              ' - but it IS in the hook table' if hooked else ''))

    print('\n== engine format table of dm/gfx/nr/nr_reg.c against 0x%x (NVN format) and 0x%x (name, channels, bytes, compressed)' % (FMT_NVN, FMT_INFO))
    rows = formats()
    if len(rows) != 63: bad += 1; print('FAIL: %d rows parsed, 63 expected' % len(rows))
    # the two jump tables are where the plan says: adrp + add in each function
    for fn, table in ((FMT_NVN, FMT_NVN_TABLE), (FMT_INFO, FMT_INFO_TABLE)):
        seen = None; reg = {}
        for pc in range(fn, fn + 0x60, 4):
            x = m.u32(pc)
            if x & 0x9f000000 == 0x90000000: reg[x & 31] = nb.adrp(x, pc)
            elif x & 0xff800000 == 0x91000000 and ((x >> 5) & 31) in reg and seen is None:
                v = reg[(x >> 5) & 31] + ((x >> 10) & 0xfff)
                if v == table: seen = v
        if seen != table: bad += 1
        print('%-4s 0x%x reads its jump table at 0x%x' % ('ok' if seen == table else 'FAIL', fn, table))
    diff = 0
    for f, (name, nvn, nvn_srgb, channels, nbytes, compressed) in enumerate(rows):
        r0, r1 = run(m, FMT_NVN, f, 0), run(m, FMT_NVN, f, 1)
        g0 = r0[1] if r0[0] == 'ret' else 0xffff; g1 = r1[1] if r1[0] == 'ret' else 0xffff
        info = run(m, FMT_INFO, 0, f, out_mem=True)
        if info[0] == 'ret':
            p, ch, by = struct.unpack_from('<QQQ', info[2]); flag = info[2][0x18]
            gname = m.cstr(p) if 0 < p < len(m.d) else '?'
        else: gname, ch, by, flag = '?trap', -1, -1, -1
        ok = (g0, g1, gname, ch, by, flag) == (nvn, nvn_srgb, name, channels, nbytes, compressed)
        if not ok:
            diff += 1
            print('FAIL fmt %2d: main %-8s nvn %#x srgb %#x channels %d bytes %d compressed %d | table %-8s nvn %#x srgb %#x channels %d bytes %d compressed %d'
                  % (f, gname, g0, g1, ch, by, flag, name, nvn, nvn_srgb, channels, nbytes, compressed))
    r63 = run(m, FMT_NVN, 63, 0)
    if r63[0] == 'ret': diff += 1; print('FAIL: 0x%x(63) returns %#x - the enum has more than 63 formats' % (FMT_NVN, r63[1]))
    bad += diff
    traps = [f for f, r in enumerate(rows) if r[1] == 0xffff]
    print('%-4s %d formats: %d differ; engine traps on %s (0xffff in the table); format 63 traps' % ('ok' if not diff else 'FAIL', len(rows), diff, ', '.join('%d %s' % (f, rows[f][0]) for f in traps)))

    print('\n== small tables of the DM_NR=2 checks')
    smin = list(struct.unpack_from('<5I', m.d, SAMPLER_MIN_TABLE))
    ok = smin == [3, 3, 5, 3, 5]
    if not ok: bad += 1
    print('%-4s sampler min filter table 0x%x = %s (nr_check_sampler k_min[1..5] = 3,3,5,3,5)' % ('ok' if ok else 'FAIL', SAMPLER_MIN_TABLE, smin))
    src = (ROOT / 'dm/gfx/nr/nr_reg.c').read_text(encoding='utf-8')
    ok = 'k_min[6] = {2, 3, 3, 5, 3, 5}' in src and 'k_wrap[4] = {1, 7, 6, 5}' in src
    if not ok: bad += 1
    print('%-4s nr_reg.c holds those values (k_min, k_wrap)' % ('ok' if ok else 'FAIL'))

    print('\n== engine opcode 55 (resource transition)')
    jt = nb.jump_table(m)
    if jt['table'] != nb.ERC_TABLE or jt['base'] != nb.ERC_BASE: bad += 1; print('FAIL: ExecuteRenderCommand dispatch differs from the plan')
    h55 = nb.ERC_BASE + 4 * struct.unpack_from('<H', m.d, nb.ERC_TABLE + 2 * 55)[0]
    lens = lengths55(m, h55)
    # the state byte: 0 / 1 run nvnCommandBufferBarrier, 2..12 return at once, anything else asserts (no return)
    ok = lens == {0x20}
    if not ok: bad += 1
    print('%-4s handler 0x%x: packet length on its returning paths = %s (hook file: 0x20 bytes: +1 state, +8 handle, +0x10 name)'
          % ('ok' if ok else 'FAIL', h55, ' | '.join(sorted(hex(v) if isinstance(v, int) else str(v) for v in lens))))
    src = (ROOT / 'dm/host/hle_dos2_nr.c').read_text(encoding='utf-8')
    ok = 'op != 55' in src and 'p + 8, 8' in src and 'p + 0x10, 8' in src and 'p[1]' in src
    if not ok: bad += 1
    print('%-4s the handler reads state +1, handle +8, name +0x10 of opcode 55 only' % ('ok' if ok else 'FAIL'))

    print('\n== NR-02 (DM_NR_KEY): the state cache the draw key reads, the call sites of the texture flush 0x%x, the pool id' % TEX_FLUSH)
    for addr, want, what in KEY_INS:
        i = m.dis(addr); got = '%s %s' % (i.mnemonic, i.op_str) if i else '?'
        ok = got == want
        if not ok: bad += 1
        print('%-4s 0x%07x %-30s %s%s' % ('ok' if ok else 'FAIL', addr, want, what, '' if ok else ' - main has `%s`' % got))
    inc = (ROOT / KEY_INC).read_text(encoding='utf-8')
    nbad = 0
    for name, v in KEY_R.items():
        mm = re.search(r'\b%s = (0x[0-9a-f]+)' % name, inc)
        if not (mm and int(mm.group(1), 16) == v): nbad += 1; print('FAIL %s: %s, the checked field is %#x' % (name, mm.group(1) if mm else 'missing', v))
    if 'k_nk_dirty[4] = {0x15dc, 0x1664, 0x1444, 0x14cc}' not in inc: nbad += 1; print('FAIL k_nk_dirty differs from the checked dirty ranges')
    bad += nbad
    print('%-4s the .inc reads the same %d state-cache offsets and the 4 dirty ranges (NK_R_*, k_nk_dirty)' % ('ok' if not nbad else 'FAIL', len(KEY_R)))
    want = sorted(calls_to(m, TEX_FLUSH)); table = key_sites()
    have = sorted(x for x, _ in table)
    ok = want == have and len(set(have)) == len(have)
    if not ok: bad += 1
    print('%-4s every call of 0x%x in main is in k_nk_site: %d calls, %d rows%s' % ('ok' if ok else 'FAIL', TEX_FLUSH, len(want), len(have),
          '' if ok else ' - missing %s, extra %s' % (['0x%x' % x for x in sorted(set(want) - set(have))], ['0x%x' % x for x in sorted(set(have) - set(want))])))
    _, use, _ = nb.nvn_slots(m)
    for addr, kind in table:
        nxt = first_nvn_after(m, use, addr + 4)
        short = nxt[len('nvnCommandBuffer'):] if nxt and nxt.startswith('nvnCommandBuffer') else nxt
        if kind in DRAW_KINDS: ok = short in DRAW_KINDS[kind]
        elif kind == 'NR_DS_COMPUTE': ok = short == 'DispatchCompute'
        elif kind == 'NR_DS_FLUSH': ok = nxt is None or 'Draw' not in nxt
        else: ok = False
        if not ok: bad += 1
        print('%-4s 0x%07x %-20s next NVN call on the path: %s' % ('ok' if ok else 'FAIL', addr, kind, nxt))
    h33 = nb.ERC_BASE + 4 * struct.unpack_from('<H', m.d, nb.ERC_TABLE + 2 * 33)[0]
    ok = any(x == 0x93fa28 and k == 'NR_DS_FLUSH' for x, k in table) and h33 <= 0x93fa28 < h33 + 16
    if not ok: bad += 1
    print('%-4s 0x93fa28 (flush only) is in the code-33 handler 0x%x' % ('ok' if ok else 'FAIL', h33))

    if a.so:
        print('\n== %s: the hooks never tail-call and never store into the caller frame' % a.so)
        errs, n = check_so(a.so, [h[3] for h in hooks])
        for e in errs: print('FAIL %s' % e)
        bad += len(errs)
        print('%-4s %d of %d hooks checked' % ('ok' if not errs else 'FAIL', n, len(hooks)))

    print('\n%s: %d problem(s)' % ('PASS' if not bad else 'FAIL', bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
