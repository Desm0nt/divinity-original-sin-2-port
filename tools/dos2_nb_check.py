"""Divinity: Original Sin 2 DE: static check of everything the native render backend front end (NB, docs/dos2/NATIVE_BACKEND_PLAN.md) relies on.

Reads the game's main (work/exefs-update/main.raw + main.symbols.json, build id checked) with python + capstone, no Ghidra:
  1. NVN slot table: the game calls NVN through global pointer slots filled by its loader (adrp/add x1 = "nvn..." name,
     str x0,[xN,#imm] = slot). Every `ldr xT,[xN,#imm]` whose base register was last written by an adrp to a slot page is a use
     (backward trace, as dm/gfx/tools/nvn_slots.py). dm/gfx/nvn/nvn_slots.tsv belongs to another game (3.3.94012), so the DOS2 slots
     are rebuilt here.
  2. Hook points (plan NB-01 list): word before the start (function boundary: ret / udf / b / br), first 4 instructions with any
     PC-relative operand flagged, and every direct branch in .text that lands inside the 16-byte patch window (start+4..start+15).
  3. ExecuteRenderCommand 0x93e5f0: the u16 jump table and its base are read from the dispatch code itself and compared with the plan
     (0x1f50716 / 0x93e648, 64 codes). Per code: handler, static packet length (last x0 write on each path to the common epilogue,
     a hint like NATIVE_PLAN 1.5), direct callees, and which NVN slots are reachable from the handler through direct calls and tail
     calls up to --depth levels (default 3), each with the smallest depth it is reached at. `blr` / `br xN` are not followed and counted.
  4. Plan expectations (NATIVE_PLAN 1.4 NVN names and handlers, 1.5 packet lengths) against the result; differences are printed.

Output: docs/dos2/nb_opcodes.tsv (one row per code) and a text report on stdout (hook prologues, table check, TAP points per code,
differences). Exit code 1 if a hook prologue check fails or the jump table differs from the plan.
Usage: dos2_nb_check.py [--main work/exefs-update/main.raw] [--out docs/dos2/nb_opcodes.tsv] [--depth 3]
"""
import argparse, collections, json, struct, sys
from pathlib import Path
import numpy as np
import capstone

ROOT = Path(__file__).resolve().parent.parent
BUILD_ID = '4979b200d53bb282e0d11871613286da'

# Plan constants (NATIVE_PLAN 1.2, 1.5; NATIVE_BACKEND_PLAN NB-01).
ERC, ERC_TABLE, ERC_BASE, ERC_EPI, ERC_N = 0x93e5f0, 0x1f50716, 0x93e648, 0x93e660, 64
HOOKS = [
    (0xa49c30, 'ExecuteCommandList', 'hook'), (0x93e5f0, 'ExecuteRenderCommand', 'hook'), (0x9215f0, 'BeginFrame', 'hook'),
    (0x93ccd0, 'EndFrame', 'hook'), (0x928e80, 'draw indexed (code 36)', 'hook'), (0x928fb0, 'draw instanced (code 37)', 'hook'),
    (0x9279a0, 'SetRenderTargets (code 22)', 'entry'), (0x928be0, 'RT + depth clear (code 5)', 'entry'),
    (0x928ab0, 'ClearTexture (code 3)', 'entry'), (0x920b00, 'CopyTextureToTexture (code 52)', 'entry'),
    (0x9290f0, 'DispatchCompute (code 43)', 'entry'), (0x925740, 'UBO flush -> BindUniformBuffer', 'entry'),
    (0x924730, 'texture flush -> BindTexture', 'entry'), (0x9283a0, 'BindProgram (codes 9 / 21)', 'entry'),
]
# NATIVE_PLAN 1.4: code -> (handler or None, callee or None, NVN names expected somewhere under the handler). Short names drop
# the nvnCommandBuffer prefix. None = the plan gives no single address for this code.
P14 = {}
def _p(codes, handler, callee, nvn):
    for c in codes: P14[c] = (handler, callee, nvn)
_p([0, 2, 53, 57, 58], None, None, [])
_p([1], 0x93fc9c, None, ['BindImage', 'BindStorageBuffer', 'SetRenderEnable', 'BindPolygonState'])
_p([3], 0x93f268, 0x928ab0, ['ClearTexture']); _p([4], 0x93f70c, None, ['ClearTexture'])
_p([5], 0x93f630, 0x928be0, ['SetRenderTargets', 'SetViewport', 'SetScissor', 'ClearDepthStencil'])
_p([6], 0x93f468, 0x9284e0, ['BindBlendState', 'BindColorState']); _p([7], 0x93f480, None, ['BindPolygonState'])
_p([8], 0x93f504, None, ['BindDepthStencilState']); _p([63], 0x93ecac, None, ['BindDepthStencilState'])
_p([9], 0x93f454, 0x9283a0, ['BindProgram'])
_p([10], 0x93eb44, None, ['BindVertexAttribState', 'BindVertexStreamState']); _p([11], 0x93ebf0, None, ['BindVertexBuffer'])
_p([15], 0x93e684, 0x9243d0, ['BindTexture']); _p([28], 0x93e648, 0x924bb0, ['BindTexture']); _p([33], 0x93fa24, 0x924730, ['BindTexture'])
_p([18], 0x93f17c, None, ['SetViewport']); _p([19], 0x93faa8, None, ['SetScissor'])
_p([21], 0x93e6ac, None, ['SetRenderTargets', 'BindVertexBuffer']); _p([22], 0x93efa8, 0x9279a0, ['SetRenderTargets'])
_p([25], 0x93f290, 0x925130, ['BindUniformBuffer'])
_p([27], 0x93f430, None, ['SetRenderEnable']); _p([48], 0x93ef48, None, ['SetRenderEnable'])
_p([31], 0x93fa48, 0x91db40, ['nvnBufferFinalize']); _p([32], 0x93fa34, 0x91d890, ['nvnBufferFinalize'])
_p([35], 0x93f0c0, None, ['DrawArrays', 'BindTexture'])
_p([36], 0x93e870, 0x928e80, ['SetRenderEnableConditional', 'DrawElements'])
_p([37], 0x93eaa4, 0x928fb0, ['DrawElementsInstanced'])
_p([40], 0x93f944, 0x92f730, ['DrawArrays']); _p([42], 0x93fc14, 0x930210, ['DrawElements'])
_p([43], 0x93f968, 0x9290f0, ['DispatchCompute']); _p([52], 0x93f654, 0x920b00, ['CopyTextureToTexture'])
_p([55], 0x93edac, None, ['Barrier', 'SetRenderEnableConditional']); _p([61], 0x93e8d8, None, ['ReportCounter', 'BindVertexBuffer'])
_p([44, 46, 47, 56, 59], None, 0x1bf44e0, [])
# NATIVE_PLAN 1.5 packet lengths ('var' = variable).
P15 = dict(enumerate([
    'var', 'var', 0x20, 0x30, 0x40, 0x20, 0x18, 0x10, 0x18, 0x10, 0x10, 0x18, 0x10, 2, 0x18, 0x20, 0x18, 0x18, 0x14, 0x14, 0x14, 0x70,
    'var', 0x98, 0x4c, 0x10, 0x10, 1, 'var', 0x20, 0x20, 0x10, 0x10, 1, 1, 0xc, 0x14, 0x1c, 0x50, 0x90, 0x40, 0x40, 0xa0, 0x10, 'var',
    0x18, 'var', 0x18, 0x10, 'var', 0x10, 0x18, 0x18, 0x28, 0x14, 'var', 0xc, 2, 2, 0xc, 0x18, 0x10, 0x10, 0x10]))
# NVN entry points the TAP must see (NATIVE_BACKEND_PLAN 1, TAP row), as short names.
TAP_SET = ['SetRenderTargets', 'ClearColor', 'ClearColori', 'ClearColorui', 'ClearDepthStencil', 'ClearTexture', 'ClearTexturei',
           'ClearTextureui', 'CopyTextureToTexture', 'CopyBufferToTexture', 'CopyTextureToBuffer', 'CopyBufferToBuffer', 'BindTexture',
           'BindTextures', 'BindImage', 'BindStorageBuffer', 'BindProgram', 'BindBlendState', 'BindColorState', 'BindChannelMaskState',
           'BindDepthStencilState', 'BindPolygonState', 'BindMultisampleState', 'BindVertexAttribState', 'BindVertexStreamState',
           'BindVertexBuffer', 'BindVertexBuffers', 'BindUniformBuffer', 'BindUniformBuffers', 'DrawArrays', 'DrawElements',
           'DrawElementsBaseVertex', 'DrawArraysInstanced', 'DrawElementsInstanced', 'DrawArraysIndirect', 'DrawElementsIndirect',
           'DispatchCompute', 'DispatchComputeIndirect', 'Barrier', 'ReportCounter', 'SetRenderEnable', 'SetRenderEnableConditional',
           'SetViewport', 'SetViewports', 'SetScissor', 'SetScissors', 'Downsample', 'ResolveDepthBuffer', 'DiscardColor',
           'DiscardDepthStencil', 'SaveZCullData', 'RestoreZCullData', 'nvnQueueSubmitCommands', 'nvnQueuePresentTexture']


def short(n):
    return n[len('nvnCommandBuffer'):] if n.startswith('nvnCommandBuffer') else n


class Main:
    def __init__(self, path):
        self.d = Path(path).read_bytes()
        sym = json.loads(Path(path).with_name('main.symbols.json').read_text())
        if not sym['nso']['build_id'].startswith(BUILD_ID):
            sys.exit('main build id %s, expected %s' % (sym['nso']['build_id'], BUILD_ID))
        (t0, tn), (r0, rn), (d0, dn) = sym['nso']['segments']
        self.text_end, self.ro = tn, (r0, r0 + rn)
        self.N = tn // 4
        self.w = np.frombuffer(self.d, dtype='<u4', count=self.N)
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.md.detail = True
        self.plt = self._plt()

    def _plt(self):
        """GOT slot -> import name (MOD0 -> .dynamic -> DT_JMPREL / DT_SYMTAB / DT_STRTAB, as tools/plt.py)."""
        b = self.d
        m0 = struct.unpack_from('<I', b, 4)[0]
        dyn = m0 + struct.unpack_from('<i', b, m0 + 4)[0]
        dt = {}
        for i in range(0, 0x400, 16):
            t, v = struct.unpack_from('<QQ', b, dyn + i)
            if t == 0: break
            dt[t] = v
        got = {}
        for o in range(dt[23], dt[23] + dt[2], 24):
            off, info, _ = struct.unpack_from('<QQq', b, o)
            st = struct.unpack_from('<I', b, dt[6] + 24 * (info >> 32))[0]
            got[off] = b[dt[5] + st:b.index(b'\0', dt[5] + st)].decode()
        return got

    def plt_name(self, a):
        """Import name if a is a PLT stub (adrp x16; ldr x17,[x16,#o]; add x16; br x17), else None."""
        if not 0 <= a < self.text_end - 16: return None
        x0, x1, x3 = self.u32(a), self.u32(a + 4), self.u32(a + 12)
        if x0 & 0x9f00001f != 0x90000010 or x1 & 0xffc003ff != 0xf9400211 or x3 != 0xd61f0220: return None
        return self.got_name(adrp(x0, a) + ((x1 >> 10) & 0xfff) * 8)

    def got_name(self, slot): return self.plt.get(slot, '?got_%x' % slot)

    def u32(self, a): return int(self.w[a // 4])

    def dis(self, a): return next(self.md.disasm(self.d[a:a + 4], a), None)

    def cstr(self, a):
        e = self.d.find(b'\0', a, a + 256)
        return self.d[a:e].decode('latin1') if e > 0 else ''


def adrp(x, pc):
    imm = ((x >> 29) & 3) | (((x >> 5) & 0x7ffff) << 2)
    if imm & (1 << 20): imm -= 1 << 21
    return (pc & ~0xfff) + (imm << 12)


def writes(x, r):
    """Whether instruction x writes general register r (approximate, conservative; from dm/gfx/tools/nvn_slots.py)."""
    op0 = (x >> 25) & 0xf
    if x & 0xfc000000 == 0x94000000 or x & 0xfffffc1f == 0xd63f0000: return r <= 18
    if op0 in (0b1000, 0b1001, 0b0101, 0b1101): return (x & 31) == r
    if op0 & 0b0101 == 0b0100:
        L = (x >> 22) & 1
        if (x >> 27) & 7 == 0b101:
            wb = (x >> 23) & 3 in (1, 3)
            return (L and ((x & 31) == r or ((x >> 10) & 31) == r)) or (wb and ((x >> 5) & 31) == r)
        if (x >> 27) & 7 == 0b111 and (x >> 24) & 3 == 0:
            wb = (x >> 21) & 1 == 0 and (x >> 10) & 3 in (1, 3)
            return (L and (x & 31) == r) or (wb and ((x >> 5) & 31) == r)
        if (x >> 27) & 7 == 0b111: return bool(L) and (x & 31) == r
        if (x >> 27) & 7 == 0b011: return (x & 31) == r
    return False


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def branch(x, pc):
    """(kind, target): kind in b, bl, cond, ret, br, blr, stop, None."""
    if x & 0xfc000000 == 0x14000000: return 'b', pc + 4 * sx(x & 0x3ffffff, 26)
    if x & 0xfc000000 == 0x94000000: return 'bl', pc + 4 * sx(x & 0x3ffffff, 26)
    if x & 0xff000010 == 0x54000000: return 'cond', pc + 4 * sx((x >> 5) & 0x7ffff, 19)
    if x & 0x7e000000 == 0x34000000: return 'cond', pc + 4 * sx((x >> 5) & 0x7ffff, 19)
    if x & 0x7e000000 == 0x36000000: return 'cond', pc + 4 * sx((x >> 5) & 0x3fff, 14)
    if x & 0xfffffc1f == 0xd65f0000: return 'ret', None
    if x & 0xfffffc1f == 0xd61f0000: return 'br', None
    if x & 0xfffffc1f == 0xd63f0000: return 'blr', None
    if x == 0 or x & 0xffe0001f == 0xd4200000: return 'stop', None
    return None, None


def pcrel(x):
    """PC-relative instruction (unsafe to copy into a trampoline as is)."""
    k, _ = branch(x, 0)
    return k in ('b', 'bl', 'cond') or x & 0x1f000000 == 0x10000000 or x & 0x3b000000 == 0x18000000  # adr/adrp, ldr literal


def nvn_slots(m):
    """Return (slot address -> NVN name, pc -> NVN name for every slot read)."""
    w = m.w
    idx = np.nonzero((w & 0xffc0001f) == 0x91000001)[0]  # add x1, xN, #imm
    refs = []
    for i in idx:
        x = int(w[i]); rn = (x >> 5) & 31
        for j in range(i - 1, max(i - 5, -1), -1):
            y = int(w[j])
            if y & 0x9f000000 == 0x90000000 and (y & 31) == rn:
                a = adrp(y, j * 4) + ((x >> 10) & 0xfff)
                if m.ro[0] <= a < m.ro[1] and m.d[a:a + 3] == b'nvn': refs.append(int(i))
                break
            if writes(y, rn): break
    if not refs: sys.exit('NVN loader not found')
    lo, hi = min(refs) - 64, max(refs) + 64
    slot, reg, cur = {}, {}, None
    for i in range(lo, hi):
        x = int(w[i]); pc = i * 4
        if x & 0x9f000000 == 0x90000000: reg[x & 31] = adrp(x, pc)
        elif x & 0xffc00000 == 0x91000000 and (x & 31) == 1 and ((x >> 5) & 31) in reg:
            s = m.cstr(reg[(x >> 5) & 31] + ((x >> 10) & 0xfff))
            cur = s if s.startswith('nvn') else cur
        elif x & 0xffc00000 == 0xf9000000 and (x & 31) == 0 and cur and ((x >> 5) & 31) in reg:
            slot[reg[(x >> 5) & 31] + ((x >> 10) & 0xfff) * 8] = cur; cur = None
    offs = np.array(sorted({a & 0xfff for a in slot}), dtype=np.uint32)
    ld = np.nonzero(((w & 0xffc00000) == 0xf9400000) & np.isin(((w >> 10) & 0xfff) * 8, offs))[0]
    use, fallback = {}, [0]
    for i in ld:
        x = int(w[i]); rn = (x >> 5) & 31; off = ((x >> 10) & 0xfff) * 8
        if rn == 31: continue
        rn0, j, add = rn, int(i) - 1, 0
        while j > max(int(i) - 4000, -1):
            y = int(w[j])
            if y == 0xd65f03c0: break
            if writes(y, rn):
                if y & 0x9f000000 == 0x90000000 and (y & 31) == rn:
                    if adrp(y, j * 4) + add + off in slot: use[int(i) * 4] = slot[adrp(y, j * 4) + add + off]
                elif y & 0xff800000 == 0x91000000 and (y & 31) == rn:  # add xN, xM, #imm (lsl 0 / 12): follow xM
                    add += ((y >> 10) & 0xfff) << (12 if y & (1 << 22) else 0); rn = (y >> 5) & 31; j -= 1; continue
                elif y & 0xffe0ffe0 == 0xaa0003e0 and (y & 31) == rn:  # mov xN, xM: follow xM
                    rn = (y >> 16) & 31; j -= 1; continue
                break
            j -= 1
        rt = x & 31
        called = False
        for k in range(int(i) + 1, min(int(i) + 9, m.N)):
            y = int(w[k])
            if y & 0xfffffc1f in (0xd63f0000, 0xd61f0000) and (y >> 5) & 31 == rt: called = True; break
            if writes(y, rt) or branch(y, k * 4)[0]: break
        if int(i) * 4 not in use and called:
            # Fallback: the linear trace hit a write on another CFG path (e.g. an epilogue ldp between a hoisted adrp and this
            # read). Only for a read whose register is called (blr / br) within 8 instructions. Accept an adrp of the same register within 400 instructions whose page + offset is a slot.
            for j in range(int(i) - 1, max(int(i) - 400, -1), -1):
                y = int(w[j])
                if y == 0xd65f03c0 and j < int(i) - 200: break
                if y & 0x9f000000 == 0x90000000 and (y & 31) == rn0 and adrp(y, j * 4) + off in slot:
                    use[int(i) * 4] = slot[adrp(y, j * 4) + off]; fallback[0] += 1; break
    print('slot reads by fallback (adrp on another path): %d' % fallback[0])
    return slot, use, (lo * 4, hi * 4)


class Graph:
    def __init__(self, m, use):
        self.m, self.use = m, use
        w = m.w
        bl = np.nonzero((w & 0xfc000000) == 0x94000000)[0]
        imm = (w[bl] & 0x3ffffff).astype(np.int64)
        imm[imm >= (1 << 25)] -= 1 << 26
        self.fstart = set((bl.astype(np.int64) * 4 + imm * 4).tolist())
        self.cache = {}

    def walk(self, entry, stop=None, limit=30000):
        """CFG of one function (or one switch case if stop = common epilogue). Returns dict(nvn=Counter, calls=set, blr, br, n)."""
        key = (entry, stop)
        if key in self.cache: return self.cache[key]
        m = self.m
        res = dict(nvn=collections.Counter(), calls=set(), blr=0, br=0, n=0, ret=0, switch=0)
        seen, todo = set(), [entry]
        while todo and res['n'] < limit:
            pc = todo.pop()
            while 0 <= pc < m.text_end and pc not in seen:
                seen.add(pc); res['n'] += 1
                if pc in self.use: res['nvn'][self.use[pc]] += 1
                k, t = branch(m.u32(pc), pc)
                if k == 'bl': res['calls'].add(t)
                elif k == 'blr': res['blr'] += 1
                elif k == 'cond': todo.append(t)
                elif k == 'b':
                    if t == stop: break
                    if (t in self.fstart and t != entry) or abs(t - entry) > 0x40000:
                        res['calls'].add(t); break  # tail call
                    pc = t; continue
                elif k == 'br':
                    jt = self.switch(pc)
                    if jt: todo.extend(jt); res['switch'] += 1
                    else: res['br'] += 1
                    break
                elif k in ('ret', 'stop'): res['ret'] += 1; break
                pc += 4
        self.cache[key] = res
        return res

    def switch(self, pc):
        """Targets of a compiler jump table ending at `br` pc: cmp wI,#lim / b.hi; adrp+add xT = table; adr xB = base;
        ldrb|ldrh wE,[xT,xI(,lsl #1)]; add xB, xB, wE, lsl #2; br xB. None if the pattern is not found."""
        m = self.m; lim = tab = base = size = None; regs = {}
        for a in range(pc - 4 * 12, pc, 4):
            x = m.u32(a)
            if x & 0x7f80001f == 0x7100001f: lim = (x >> 10) & 0xfff                       # cmp wN, #imm
            elif x & 0x9f000000 == 0x90000000: regs[x & 31] = adrp(x, a)
            elif x & 0xff800000 == 0x91000000 and ((x >> 5) & 31) in regs: tab = regs[(x >> 5) & 31] + ((x >> 10) & 0xfff)
            elif x & 0x9f000000 == 0x10000000:
                base = a + sx(((x >> 29) & 3) | (((x >> 5) & 0x7ffff) << 2), 21)
            elif x & 0xffe00c00 == 0x38600800: size = 1                                  # ldrb w, [x, x]
            elif x & 0xffe00c00 == 0x78600800: size = 2                                  # ldrh w, [x, x{, lsl #1}]
        if None in (lim, tab, base, size) or not m.ro[0] <= tab < m.ro[1] or lim > 1024: return None
        fmt = '<B' if size == 1 else '<H'
        return sorted({base + 4 * struct.unpack_from(fmt, m.d, tab + size * k)[0] for k in range(lim + 1)})

    def reach(self, entry, stop, depth):
        """NVN name -> smallest depth; callee address -> depth; unresolved blr / br counts."""
        nvn, fn, blr, br, self.sw = {}, {entry: 0}, 0, 0, 0
        level = [(entry, stop)]
        for dep in range(depth + 1):
            nxt = []
            for f, s in level:
                if f != entry and self.m.plt_name(f): continue  # import: named, not walked
                r = self.walk(f, s)
                blr += max(0, r['blr'] - sum(r['nvn'].values())); br += r['br']; self.sw += r['switch']
                for n in r['nvn']: nvn.setdefault(n, dep)
                for c in r['calls']:
                    if c not in fn: fn[c] = dep + 1; nxt.append((c, None))
            level = nxt if dep < depth else []
        return nvn, fn, blr, br


def jump_table(m):
    """Read cmp / table / base from the dispatch code at ERC."""
    got = dict(limit=None, table=None, base=None)
    regs = {}
    for a in range(ERC, ERC + 0x58, 4):
        i = m.dis(a); x = m.u32(a)
        if i is None: continue
        if i.mnemonic == 'cmp' and got['limit'] is None and '#' in i.op_str: got['limit'] = int(i.op_str.split('#')[1], 0)
        if x & 0x9f000000 == 0x90000000: regs[x & 31] = adrp(x, a)
        elif x & 0x9f000000 == 0x10000000:  # adr
            imm = ((x >> 29) & 3) | (((x >> 5) & 0x7ffff) << 2)
            got['base'] = a + sx(imm, 21)
        elif x & 0xff800000 == 0x91000000 and ((x >> 5) & 31) in regs:
            v = regs[(x >> 5) & 31] + ((x >> 10) & 0xfff)
            if m.ro[0] <= v < m.ro[1]: got['table'] = v
    return got


def packet_len(m, h):
    """Last x0/w0 write on each straight path of a case to the epilogue (hint; same method as the earlier oplen walk)."""
    if h == ERC_EPI: return 1, '0x1'  # case = the epilogue itself: w0 = 1 set before the br
    out = set()
    def go(pc, last, seen, dep):
        for _ in range(400):
            if pc in seen: return
            seen.add(pc)
            i = m.dis(pc); x = m.u32(pc)
            k, t = branch(x, pc)
            ops = i.op_str.split(',') if i else []
            if i and ops and ops[0].strip() in ('w0', 'x0') and i.mnemonic not in (
                    'cmp', 'tst', 'str', 'strb', 'strh', 'stp', 'cbz', 'cbnz', 'tbz', 'tbnz', 'cmn', 'ccmp', 'stur'):
                last = '%s %s' % (i.mnemonic, i.op_str)
            if k in ('bl', 'blr'): last = 'call'
            if k == 'b':
                if t == ERC_EPI: out.add(last); return
                pc = t; continue
            if k in ('ret', 'br', 'stop'): out.add(last + '/' + k); return
            if k == 'cond':
                if t == ERC_EPI: out.add(last)
                elif dep < 4: go(t, last, seen, dep + 1)
            pc += 4
    go(h, 'default', set(), 0)
    vals = set()
    for s in out:
        if s.startswith('mov') and '#' in s and s.count(',') == 1 and '/' not in s: vals.add(int(s.split('#')[1], 0))
        elif s == 'default': vals.add(1)
        else: vals.add(s)
    if len(vals) == 1 and isinstance(next(iter(vals)), int): return next(iter(vals)), '0x%x' % next(iter(vals))
    return 'var', ' | '.join(sorted(('0x%x' % v) if isinstance(v, int) else v for v in vals))


def inbound(m, lo, hi):
    """Direct branches (b / bl / b.cond / cbz / tbz) anywhere in .text that land in [lo, hi)."""
    w = m.w; pcs = np.arange(m.N, dtype=np.int64) * 4; hits = []
    for mask, val, sh, bits in ((0x7c000000, 0x14000000, 0, 26), (0xff000010, 0x54000000, 5, 19),
                                (0x7e000000, 0x34000000, 5, 19), (0x7e000000, 0x36000000, 5, 14)):
        sel = np.nonzero((w & mask) == val)[0]
        imm = ((w[sel] >> sh) & ((1 << bits) - 1)).astype(np.int64)
        imm[imm >= (1 << (bits - 1))] -= 1 << bits
        t = pcs[sel] + imm * 4
        for k in np.nonzero((t >= lo) & (t < hi))[0]: hits.append((int(pcs[sel[k]]), int(t[k])))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--main', default=str(ROOT.parent / 'work' / 'exefs-update' / 'main.raw'))
    ap.add_argument('--out', default=str(ROOT / 'docs' / 'dos2' / 'nb_opcodes.tsv'))
    ap.add_argument('--depth', type=int, default=3)
    a = ap.parse_args()
    m = Main(a.main)
    bad = 0
    slot, use, loader = nvn_slots(m)
    print('NVN slots %d (loader 0x%x..0x%x), slot reads %d' % (len(slot), loader[0], loader[1], len(use)))
    g = Graph(m, use)

    print('\n== hook points: boundary, first 4 instructions, inbound branches into start+4..start+15')
    for addr, name, kind in HOOKS:
        prev = m.u32(addr - 4); pk, _ = branch(prev, addr - 4)
        boundary = {'ret': 'ret', 'br': 'br', 'b': 'b', 'stop': 'udf/brk'}.get(pk, 'NONE')
        ins = [m.dis(addr + 4 * k) for k in range(4)]
        txt = '; '.join('%s %s' % (i.mnemonic, i.op_str) for i in ins)
        rel = [k for k in range(4) if pcrel(m.u32(addr + 4 * k))]
        inb = inbound(m, addr + 4, addr + 16)
        ok = boundary != 'NONE' and not rel and not inb
        st = 'ok' if ok else 'FAIL'
        if kind == 'hook' and not ok: bad += 1
        print('%-8s 0x%07x %-32s prev=%-7s %s%s%s' % (st, addr, name, boundary, txt,
              ('  PC-REL at +%s' % ','.join(str(4 * k) for k in rel)) if rel else '',
              ('  INBOUND ' + ' '.join('0x%x->0x%x' % h for h in inb[:4])) if inb else ''))

    print('\n== ExecuteRenderCommand dispatch')
    jt = jump_table(m)
    print('code: cmp #%s, table %s, base %s (plan: cmp #63, 0x%x, 0x%x)' % (
        jt['limit'], hex(jt['table']) if jt['table'] else None, hex(jt['base']) if jt['base'] else None, ERC_TABLE, ERC_BASE))
    if jt['table'] != ERC_TABLE or jt['base'] != ERC_BASE or jt['limit'] not in (63, 64): bad += 1; print('  DIFFERS from plan')
    tab = [struct.unpack_from('<H', m.d, ERC_TABLE + 2 * c)[0] for c in range(ERC_N)]
    hand = [ERC_BASE + 4 * t for t in tab]

    rows, diffs, tap = [], [], collections.defaultdict(set)
    for c in range(ERC_N):
        h = hand[c]
        ln, lnt = packet_len(m, h)
        nvn, fn, blr, br = g.reach(h, ERC_EPI, a.depth)
        direct = ['%s' % (m.plt_name(f) or '0x%x' % f) for f in sorted(f for f, dp in fn.items() if dp == 1)]
        bydep = collections.defaultdict(list)
        for n, dp in sorted(nvn.items()): bydep[dp].append(short(n))
        cb = sorted(short(n) for n in nvn if n.startswith('nvnCommandBuffer') or n.startswith('nvnQueue'))
        taps = sorted(n for n in cb if n in TAP_SET)
        for n in taps: tap[n].add(c)
        ph, pc_, pn = P14.get(c, (None, None, None))
        notes = []
        if ph is not None and ph != h: notes.append('handler plan 0x%x' % ph)
        if pc_ is not None and pc_ not in fn: notes.append('callee 0x%x not reached' % pc_)
        if pn is not None:
            miss = [n for n in pn if not any(short(x) == n or short(x).startswith(n) for x in nvn)]
            if miss: notes.append('plan NVN missing: ' + ','.join(miss))
            if not pn and cb and min(nvn[x] for x in nvn) <= 1: notes.append('plan says no NVN')
        pl = P15.get(c)
        if pl is not None and pl != ln and not (pl == 'var' and ln == 'var'): notes.append('len plan %s' % (pl if pl == 'var' else hex(pl)))
        if notes: diffs.append((c, notes))
        rows.append([str(c), '0x%x' % h, lnt, ' '.join(direct[:8]) + (' +%d' % (len(direct) - 8) if len(direct) > 8 else ''),
                     ','.join(bydep[0]) or '-', ','.join(bydep[1]) or '-', ','.join(bydep[2]) or '-',
                     ','.join(bydep[3]) or '-', ','.join(taps) or '-', str(blr), '%d/%d' % (br, g.sw), str(len(fn) - 1), '; '.join(notes) or '-'])

    hdr = ['code', 'handler', 'len_static', 'direct_calls', 'nvn_d0', 'nvn_d1', 'nvn_d2', 'nvn_d3', 'tap_points', 'blr_other', 'br_open/switch',
           'functions', 'plan_diff']
    out = Path(a.out)
    with out.open('w', encoding='utf-8', newline='\n') as f:
        f.write('# kind: evidence  DOS2 main %s, ExecuteRenderCommand 0x%x codes (tools/dos2_nb_check.py --depth %d)\n' % (
            BUILD_ID[:16], ERC, a.depth))
        f.write('# nvn_dK: NVN slots first reached at call depth K (0 = the case code itself, 1 = its direct callees, ...; tail calls '
                'count as calls; imports named, not walked; blr/br not followed: blr_other = blr not loading an NVN slot; '
                'br_open/switch: register branches left unfollowed (tail calls through a register) / jump tables resolved and followed). Names drop the nvnCommandBuffer prefix. len_static: hint, last x0 write per path.\n')
        f.write('\t'.join(hdr) + '\n')
        for r in rows: f.write('\t'.join(r) + '\n')
    print('wrote %s (%d codes)' % (out, len(rows)))

    print('\n== TAP points: NVN command-buffer entry -> codes reaching it (depth <= %d)' % a.depth)
    for n in sorted(tap): print('  %-28s %s' % (n, ' '.join(str(c) for c in sorted(tap[n]))))
    print('  TAP-set entries reached by no code: ' + ' '.join(n for n in TAP_SET if n not in tap))

    print('\n== differences against NATIVE_PLAN 1.4 / 1.5 (%d codes)' % len(diffs))
    for c, notes in diffs: print('  %2d: %s' % (c, '; '.join(notes)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
