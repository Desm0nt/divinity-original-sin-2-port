"""[NR-28] DM_NR_LEANCALL: the lean NVN call path in an AArch64 emulator (unicorn), and what one call costs on each path.

Part A (always): the real nvn_entry.S + nvn_thunks.S (asm_elf.py, then the NDK's clang, linked statically with a stub data file at a fixed
address). Every case starts with x0..x28 / q0..q31 at distinct patterns, x29 = the guest's frame, x30 = a return sentinel, tpidr_el0 =
the thread pointer of the case, sp on a guest stack (or on the big stack). Checks:
  - lean, switch: the body runs on top - 32 of the thread's big stack with x0-x8, x12-x15, x18-x28 and q0-q31 as the caller left them,
    x16 = the slot index, x29 = a frame record {the guest's x29, the guest's return address, the guest's sp}; afterwards sp, x29,
    x19-x28, d8-d15 come back, x0 / x1 / q0-q3 are the body's; nothing below the guest's sp is written (the old wrapper writes 240 bytes);
    only those 32 bytes of the big stack are written; the plain thunk counted the call once (the stadd thunk too).
  - lean, in place (sp already on the big stack, every edge of the 1 MiB range): a tail call, sp / x29 / x30 untouched.
  - lean, passed on (no entry, another thread's entry, an entry one slot off): _nvn_bigstack_entry is reached with every argument
    register, x16 = the slot address, sp and x30 as the thunk left them; the old wrapper then runs the body (both answers of
    nvn_bigstack_top: another stack / in place).
  - the hash: 1500 thread pointers (random, 1 MiB + 16 KiB apart as bionic maps threads, page-aligned) - the entry at the slot
    nvn_lc_hash (C, mirrored here) names is the one the assembly reads.
  - DM_NVN_PROF=2 under the lean entry: nvn_prof2_entry gets the index, the untimed path reaches the body untouched, the timed one adds
    one call to nvn_p2_acc[i].
  - instruction / memory-operation counts of each path (thunk + entry, the body excluded; the old path without nvn_bigstack_top).
Part B (--so <libdm.so>): the same paths through the real libdm.so - thunks, both entries, the compiled nvn_bigstack_top with its TLS
descriptor call (bionic's resolver for a dlopen'ed library modelled: the static one, 2 instructions, and the dynamic one, 17) and the
no-op body; instructions by class and a cost model per CPU class.
--mutations: 10 mutations of _nvn_lean_entry (range edge, hash, thread test, index, frame, x8, tail call), part A must fail on each.
Usage: python nr_leancall_test.py --ndk <ndk bin dir> [--so <libdm.so>] [--mutations] [--keep <dir>]   (needs: pip unicorn, pyelftools, capstone)"""
import argparse, os, random, struct, subprocess, sys, tempfile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_INSN, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as C
import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
DM = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..'))
NFUNC = 523
TABN = 4096
BIGSTACK = 1 << 20
DATA_C = r'''
#include <stdint.h>
typedef struct { uint64_t cnt; void *fn; } nvn_slot_t;
nvn_slot_t nvn_slot[%d];
void *nvn_real[%d], *nvn_real_big[%d];
uint64_t nvn_prof_ticks[%d];
uint64_t nvn_p2_tp, nvn_p2_acc[%d][2];
struct { uint64_t tp, top; } nvn_lc_tab[%d] __attribute__((aligned(64)));
void *nvn_bigstack_top(uint64_t sp) { (void)sp; return 0; }
uint64_t nvn_unimpl_c(void *s, uint64_t a, uint64_t b, uint64_t c) { (void)s; (void)a; (void)b; (void)c; return 0; }
void nvn_trace_c(uint64_t i, uint64_t a, uint64_t b, uint64_t c, uint64_t r) { (void)i; (void)a; (void)b; (void)c; (void)r; }
uint64_t mach_absolute_time(void) { return 0; }
''' % (NFUNC, NFUNC, NFUNC, NFUNC, NFUNC, TABN)

def lc_hash(tp):   # nvn_lc_hash in nvn_leancall.inc
    t = tp >> 12; t ^= t >> 10; t ^= t >> 20; return t & (TABN - 1)

def build(ndk, out, entry_src=None):
    clang = os.path.join(ndk, 'clang.exe' if os.name == 'nt' else 'clang')
    srcs = []
    for n in ('nvn_entry.S', 'nvn_thunks.S'):
        d = os.path.join(out, 'elf_' + n)
        src = entry_src if (n == 'nvn_entry.S' and entry_src) else os.path.join(DM, 'gfx', 'nvn', n)
        subprocess.check_call([sys.executable, os.path.join(DM, 'android', 'tools', 'asm_elf.py'), src, d]); srcs.append(d)
    data = os.path.join(out, 'lc_data.c'); open(data, 'w').write(DATA_C); srcs.append(data)
    elf = os.path.join(out, 'lc.elf')
    subprocess.check_call([clang, '--target=aarch64-linux-android29', '-O1', '-nostdlib', '-static', '-fno-pic', '-fuse-ld=lld',
                           '-Wl,-e,nvn_lean_entry', '-Wl,--image-base=0x400000', '-o', elf] + srcs)
    return elf

XREG = [C.UC_ARM64_REG_X0 + i for i in range(29)] + [C.UC_ARM64_REG_X29, C.UC_ARM64_REG_X30]
QREG = [C.UC_ARM64_REG_Q0 + i for i in range(32)]
def xr(n): return XREG[n]
GUEST, GSP, BIGM, BODY, SENT = 0x80000000, 0x80010000, 0x90000000, 0xA0000000, 0xB0000000
TOP = BIGM + 0x4000 + BIGSTACK       # the top of the big stack (the mapping: 16 KiB guard + 1 MiB, as nvn_bigstack_top maps it)
GFP = 0x6666777788889999
RET0, RET1 = 0x1234567812345678, 0x9abcdef09abcdef0
def xpat(k): return 0x1000000000000000 | (k << 32) | (k * 0x01010101)
def qpat(k): return (0x5a5a000000000000 | k) << 64 | (0xa5a50000 + k) * 0x100000001
def qret(k): return (0xfeed0000 + k) << 96 | 0x1111 * (k + 1)

class Emu:
    def __init__(self, elf_path):
        f = ELFFile(open(elf_path, 'rb'))
        self.sym = {s.name: s['st_value'] for s in f.get_section_by_name('.symtab').iter_symbols() if s.name}
        uc = self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.ctl_set_cpu_model(C.UC_CPU_ARM64_MAX)   # LSE (stadd) for the atomic thunks
        lo = min(s['p_vaddr'] for s in f.iter_segments() if s['p_type'] == 'PT_LOAD') & ~0xFFF
        hi = max(s['p_vaddr'] + s['p_memsz'] for s in f.iter_segments() if s['p_type'] == 'PT_LOAD')
        uc.mem_map(lo, (hi - lo + 0xFFFF) & ~0xFFFF)
        for s in f.iter_segments():
            if s['p_type'] == 'PT_LOAD': uc.mem_write(s['p_vaddr'], s.data())
        uc.mem_map(GUEST, 0x20000); uc.mem_map(BIGM, 0x120000); uc.mem_map(BODY, 0x1000); uc.mem_map(SENT, 0x1000)
        ret = struct.pack('<I', 0xd65f03c0)
        uc.mem_write(BODY, ret); uc.mem_write(SENT, ret)
        self.cnt = 0; self.trace = None; self.body = []; self.at_big = []; self.top_ret = 0; self.top_args = []; self.writes = []
        uc.hook_add(UC_HOOK_CODE, self._on_body, begin=BODY, end=BODY)
        uc.hook_add(UC_HOOK_CODE, self._on_big, begin=self.sym['nvn_bigstack_entry'], end=self.sym['nvn_bigstack_entry'])
        uc.hook_add(UC_HOOK_CODE, self._on_top, begin=self.sym['nvn_bigstack_top'], end=self.sym['nvn_bigstack_top'])
        uc.hook_add(UC_HOOK_CODE, self._on_any)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        uc.hook_add(UC_HOOK_INSN, self._on_mrs, None, 1, 0, C.UC_ARM64_INS_MRS)
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    def _on_mrs(self, uc, reg, cp, ud):
        if (cp.crn, cp.crm, cp.op0, cp.op1, cp.op2) == (14, 0, 3, 3, 2):   # cntvct_el0
            self.cnt += 7; uc.reg_write(reg, self.cnt); return True
        return False
    def _on_write(self, uc, access, addr, size, value, ud): self.writes.append((addr, size))
    def _on_any(self, uc, addr, size, ud):
        if self.trace is not None: self.trace.append(addr)
    def regs(self):
        uc = self.uc
        return {'x': [uc.reg_read(r) for r in XREG], 'q': [uc.reg_read(r) for r in QREG], 'sp': uc.reg_read(C.UC_ARM64_REG_SP)}
    def _on_body(self, uc, addr, size, ud):
        r = self.regs(); r['frame'] = struct.unpack('<3Q', bytes(uc.mem_read(r['sp'], 24))) if BIGM <= r['sp'] - 32 < BIGM + 0x120000 or GUEST <= r['sp'] < GUEST + 0x20000 - 24 else None
        self.body.append(r)
        uc.reg_write(xr(0), RET0); uc.reg_write(xr(1), RET1)
        for k in range(2, 18): uc.reg_write(xr(k), 0xdead000000000000 | k)    # the body may clobber every caller-saved register (not x18)
        for k in range(4): uc.reg_write(QREG[k], qret(k))
        for k in list(range(4, 8)) + list(range(16, 32)): uc.reg_write(QREG[k], 0xbad0 + k)
    def _on_big(self, uc, addr, size, ud): self.at_big.append(self.regs())
    def _on_top(self, uc, addr, size, ud):   # the C function: clobbers its caller-saved registers, answers top_ret, returns
        self.top_args.append(uc.reg_read(xr(0)))
        for k in range(1, 18): uc.reg_write(xr(k), 0xc0c0000000000000 | k)
        for k in list(range(0, 8)) + list(range(16, 32)): uc.reg_write(QREG[k], 0xc1c1 + k)
        uc.reg_write(xr(0), self.top_ret); uc.reg_write(C.UC_ARM64_REG_PC, uc.reg_read(xr(30)))

    def setup(self, tp, sp=GSP, tab=(), slot=7, real_big=BODY, real=0, p2tp=0, top_ret=0):
        uc = self.uc; s = self.sym
        uc.mem_write(s['nvn_lc_tab'], b'\0' * 16 * TABN)
        for (i, etp, etop) in tab: uc.mem_write(s['nvn_lc_tab'] + 16 * i, struct.pack('<QQ', etp, etop))
        uc.mem_write(s['nvn_slot'] + 16 * slot, struct.pack('<QQ', 41, s['nvn_lean_entry']))
        uc.mem_write(s['nvn_real_big'] + 8 * slot, struct.pack('<Q', real_big))
        uc.mem_write(s['nvn_real'] + 8 * slot, struct.pack('<Q', real))
        uc.mem_write(s['nvn_p2_tp'], struct.pack('<Q', p2tp)); uc.mem_write(s['nvn_p2_acc'] + 16 * slot, b'\0' * 16)
        uc.mem_write(GUEST, b'\xa5' * 0x20000); uc.mem_write(BIGM, b'\x3c' * 0x120000)
        for k in range(29): uc.reg_write(xr(k), xpat(k))
        uc.reg_write(xr(29), GFP); uc.reg_write(xr(30), SENT)
        for k in range(32): uc.reg_write(QREG[k], qpat(k))
        uc.reg_write(C.UC_ARM64_REG_SP, sp); uc.reg_write(C.UC_ARM64_REG_TPIDR_EL0, tp); uc.reg_write(C.UC_ARM64_REG_NZCV, 0x60000000)
        self.body = []; self.at_big = []; self.top_args = []; self.top_ret = top_ret; self.writes = []; self.trace = []
    def run(self, start, x16=None):
        if x16 is not None: self.uc.reg_write(xr(16), x16)
        self.uc.emu_start(start, SENT, count=4000)
        return self.uc.reg_read(C.UC_ARM64_REG_PC) == SENT
    def cnt_of(self, slot): return struct.unpack('<Q', bytes(self.uc.mem_read(self.sym['nvn_slot'] + 16 * slot, 8)))[0]
    def classify(self, addrs, skip=()):
        k = {'instructions': 0, 'loads': 0, 'stores': 0, 'atomics': 0, 'indirect branches': 0, 'calls': 0, 'system register reads': 0}
        for a in addrs:
            if a in skip: continue
            w = bytes(self.uc.mem_read(a, 4)); i = next(self.md.disasm(w, a)); m = i.mnemonic
            k['instructions'] += 1
            if m in ('stadd', 'ldadd', 'ldxr', 'stxr', 'cas', 'casal', 'swp'): k['atomics'] += 1
            elif m.startswith(('ldr', 'ldp', 'ldur')): k['loads'] += 1
            elif m.startswith(('str', 'stp', 'stur')): k['stores'] += 1
            if m in ('br', 'blr'): k['indirect branches'] += 1
            if m in ('bl', 'blr'): k['calls'] += 1
            if m == 'mrs': k['system register reads'] += 1
        return k


def part_a(elf):
    E = Emu(elf); s = E.sym; fails = []; nchk = [0]
    def check(c, msg):
        nchk[0] += 1
        if not c: fails.append(msg); print('FAIL', msg)
    slot = 7; saddr = s['nvn_slot'] + 16 * slot
    lean_lo, lean_hi = s['nvn_lean_entry'], s['nvn_lean_entry'] + 0x84
    TP = 0x7a1b2c3d4000 + 0x10
    h = lc_hash(TP)
    counts = {}
    def args_intact(r, what, x16=None, skip_x=(9, 10, 11, 16, 17), old=False):
        # old = the body under the old wrapper: only x0-x7 / q0-q7 (the reloaded arguments) and the callee-saved x19-x28 hold; x8, x9-x15
        # and q16-q31 are whatever nvn_bigstack_top (a C call) left - the lean entry keeps every register but its five scratch ones
        for k in range(29):
            if k in skip_x or (old and 8 <= k <= 18): continue
            check(r['x'][k] == xpat(k), '%s: x%d intact (%#x)' % (what, k, r['x'][k]))
        for k in range(32):
            if old and k >= 8 and not (8 <= k < 16): continue
            if old and 8 <= k < 16: check(r['q'][k] & (2**64 - 1) == qpat(k) & (2**64 - 1), '%s: d%d intact' % (what, k)); continue
            check(r['q'][k] == qpat(k), '%s: q%d intact' % (what, k))
        if x16 is not None: check(r['x'][16] == x16, '%s: x16 = %#x (got %#x)' % (what, x16, r['x'][16]))
    def after_ok(what):
        uc = E.uc
        check(uc.reg_read(C.UC_ARM64_REG_SP) == E.sp0, '%s: sp comes back' % what)
        check(uc.reg_read(xr(29)) == GFP, '%s: x29 comes back' % what)
        for k in range(19, 29): check(uc.reg_read(xr(k)) == xpat(k), '%s: x%d comes back' % (what, k))
        for k in range(8, 16): check(uc.reg_read(QREG[k]) & (2**64 - 1) == qpat(k) & (2**64 - 1), '%s: d%d comes back' % (what, k))
        check(uc.reg_read(xr(0)) == RET0 and uc.reg_read(xr(1)) == RET1, '%s: x0 / x1 = the body\'s' % what)
        for k in range(4): check(uc.reg_read(QREG[k]) == qret(k), '%s: q%d = the body\'s' % (what, k))

    # --- lean, switch, from the plain thunk and from the stadd thunk ---
    for th in ('nvnth80_%d' % slot, 'nvnth_%d' % slot, None):
        what = 'lean switch via %s' % (th or 'the entry')
        E.setup(TP, tab=[(h, TP, TOP)]); E.sp0 = GSP
        ok = E.run(s[th] if th else s['nvn_lean_entry'], None if th else saddr)
        check(ok, what + ': returns'); check(len(E.body) == 1 and not E.at_big, what + ': the body once, not the old wrapper')
        if E.body:
            r = E.body[0]; args_intact(r, what, x16=slot)
            check(r['sp'] == TOP - 32, what + ': the body on top - 32 of the big stack (%#x)' % r['sp'])
            check(r['x'][29] == TOP - 32 and r['frame'] == (GFP, SENT, GSP), what + ': frame record {guest x29, guest x30, guest sp}')
            check(lean_lo <= r['x'][30] < lean_hi, what + ': the body returns into the lean entry')
        after_ok(what)
        check(all(not (GUEST <= a < GUEST + 0x20000) for a, n in E.writes), what + ': nothing written on the guest stack')
        check(all(TOP - 32 <= a and a + n <= TOP for a, n in E.writes if BIGM <= a < BIGM + 0x120000), what + ': only the 32-byte frame on the big stack')
        if th: check(E.cnt_of(slot) == 42, what + ': the thunk counted the call once')
        k = E.classify(E.trace, skip=(BODY,)); counts['lean, stack switch' + (' (%s thunk)' % ('plain' if '80' in th else 'stadd') if th else ' (entry only)')] = k

    # --- lean, in place: every edge of the range ---
    for sp, inplace in ((TOP, 1), (TOP - 0x1000, 1), (TOP - BIGSTACK, 1), (TOP - BIGSTACK - 16, 0), (TOP + 16, 0)):
        what = 'lean at sp = top %+#x' % (sp - TOP)
        E.setup(TP, sp=sp, tab=[(h, TP, TOP)]); E.sp0 = sp
        check(E.run(s['nvnth80_%d' % slot]), what + ': returns'); check(len(E.body) == 1 and not E.at_big, what + ': the body once')
        if E.body:
            r = E.body[0]; args_intact(r, what, x16=slot)
            if inplace:
                check(r['sp'] == sp and r['x'][30] == SENT and r['x'][29] == GFP, what + ': in place, a tail call (sp, x29, x30 untouched)')
                check(not [1 for a, n in E.writes if not (s['nvn_slot'] <= a < s['nvn_slot'] + 16 * NFUNC)], what + ': nothing written but the counter')
            else: check(r['sp'] == TOP - 32, what + ': outside the range: the switch')
        after_ok(what)
        if sp == TOP - 0x1000: counts['lean, in place (plain thunk)'] = E.classify(E.trace, skip=(BODY,))

    # --- lean passes on: no entry / another thread's / one slot off; then the old wrapper with both answers of nvn_bigstack_top ---
    for tab, why in (([], 'no entry'), ([(h, TP + 0x104000, TOP)], 'another thread\'s entry'), ([((h + 1) % TABN, TP, TOP)], 'the entry one slot off')):
        for top_ret in (TOP, 0):
            what = 'lean passes on (%s), old wrapper %s' % (why, 'switches' if top_ret else 'in place')
            sp = GSP if top_ret else TOP - 0x2000
            E.setup(TP, sp=sp, tab=tab, top_ret=top_ret); E.sp0 = sp
            check(E.run(s['nvnth80_%d' % slot]), what + ': returns')
            check(len(E.at_big) == 1, what + ': reaches _nvn_bigstack_entry once')
            if E.at_big:
                r = E.at_big[0]; args_intact(r, what + ' at the old entry', x16=saddr)
                check(r['sp'] == sp and r['x'][30] == SENT and r['x'][29] == GFP, what + ': sp / x29 / x30 as the thunk left them')
            check(len(E.body) == 1, what + ': the body once')
            if E.body:
                r = E.body[0]; args_intact(r, what + ' at the body', x16=slot, old=True)
                check(r['sp'] == (TOP if top_ret else sp - 240), what + ': the body where the old wrapper puts it')
                check(E.top_args == [sp - 240], what + ': nvn_bigstack_top gets the wrapper\'s sp')
            after_ok(what)
            if why == 'no entry' and top_ret: counts['old wrapper, stack switch (plain thunk + lean test, nvn_bigstack_top excluded)'] = E.classify(E.trace, skip=(BODY,))

    # --- the old path alone: the stadd thunk -> _nvn_bigstack_entry (as the slots are with DM_NR_LEANCALL 0 / 2) ---
    E.setup(TP, top_ret=TOP); E.sp0 = GSP
    E.uc.mem_write(saddr, struct.pack('<QQ', 41, s['nvn_bigstack_entry']))
    check(E.run(s['nvnth_%d' % slot]) and len(E.body) == 1 and E.body[0]['sp'] == TOP, 'old path: the body on the big stack')
    after_ok('old path')
    check(len([1 for a, n in E.writes if GUEST <= a < GUEST + 0x20000]) > 0, 'old path: writes its frame on the guest stack (sanity of the write check)')
    counts['old: stadd thunk + big-stack wrapper (nvn_bigstack_top excluded)'] = E.classify(E.trace, skip=(BODY,))

    # --- the hash: random and bionic-like thread pointers ---
    rnd = random.Random(28); tps = [rnd.randrange(1 << 36, 1 << 47) & ~0xF for _ in range(1000)]
    base = 0x7b00000000 + 0xd50; tps += [base + k * 0x104000 for k in range(250)] + [base + k * 0x101000 for k in range(250)]
    hit = miss = 0; hs = set()
    for tp in tps:
        hh = lc_hash(tp); hs.add(hh)
        E.setup(tp, tab=[(hh, tp, TOP)]); E.sp0 = GSP; E.trace = None
        E.run(s['nvnth80_%d' % slot]); hit += len(E.body) == 1 and not E.at_big and E.body[0]['sp'] == TOP - 32
        E.setup(tp, tab=[((hh + 1) % TABN, tp, TOP)]); E.trace = None
        E.run(s['nvnth80_%d' % slot]); miss += len(E.at_big) == 1
    check(hit == len(tps) and miss == len(tps), 'hash: the assembly reads the entry nvn_lc_hash names (%d / %d hits, %d / %d misses one slot off)' % (hit, len(tps), miss, len(tps)))
    spaced = [lc_hash(base + k * 0x104000) for k in range(64)]
    check(len(set(spaced)) == 64, 'hash: 64 threads 1 MiB + 16 KiB apart get 64 different slots (%d)' % len(set(spaced)))

    # --- DM_NVN_PROF=2 under the lean entry ---
    for p2tp in (0x5555000, TP):
        what = 'DM_NVN_PROF=2 %s' % ('timed' if p2tp == TP else 'untimed')
        E.setup(TP, tab=[(h, TP, TOP)], real_big=s['nvn_prof2_entry'], real=BODY, p2tp=p2tp); E.sp0 = GSP
        check(E.run(s['nvnth80_%d' % slot]) and len(E.body) == 1, what + ': the body once')
        if E.body:
            r = E.body[0]
            args_intact(r, what, skip_x=(9, 10, 11, 12, 16, 17) if p2tp == TP else (9, 10, 11, 16, 17))
            check(r['sp'] == (TOP - 32 if p2tp != TP else TOP - 64), what + ': on the big stack (sp %#x)' % r['sp'])
        acc = struct.unpack('<QQ', bytes(E.uc.mem_read(s['nvn_p2_acc'] + 16 * slot, 16)))
        check(acc[1] == (1 if p2tp == TP else 0), what + ': nvn_p2_acc[i] calls = %d' % acc[1])
        after_ok(what)
    return fails, nchk[0], counts


# Mutations of _nvn_lean_entry that part A must catch (each one builds and runs the whole part A)
MUTATIONS = [
    ("b.ls Llc_here", "b.lo Llc_here", "in place only below the range's top edge"),
    ("and x9, x9, #4095                // NVN_LC_TAB - 1", "and x9, x9, #2047", "a hash of 11 bits"),
    ("eor x9, x9, x9, lsr #20", "eor x9, x9, x9, lsr #21", "another fold of the hash"),
    ("cmp x9, x17\n    b.ne Llc_slow", "cmp x9, x9\n    b.ne Llc_slow", "any entry taken as this thread's"),
    ("    lsr x16, x16, #4                 // slot address -> index\n    adrp x9, _nvn_real_big@PAGE", "    adrp x9, _nvn_real_big@PAGE", "no slot index"),
    ("str x9, [x10, #16]               // the caller's sp", "str x9, [x10, #24]", "the caller's sp saved off the frame"),
    ("stp x29, x30, [x10, #-32]!", "stp x29, x30, [x10, #-16]!", "a 16-byte frame (the sp slot above the top)"),
    ("    mov sp, x10\n    mov x29, sp\n", "    mov sp, x10\n", "no frame pointer"),
    ("sub x11, x10, x9\n    cmp x11,", "sub x8, x10, x9\n    cmp x8,", "x8 (the indirect result register) as scratch"),
    ("Llc_here:\n    br x17", "Llc_here:\n    blr x17\n    ret", "in place with a call (lr lost)"),
]

def mutation_check(ndk, out):
    src = open(os.path.join(DM, 'gfx', 'nvn', 'nvn_entry.S'), encoding='utf-8').read()
    caught = 0
    for k, (a, b, why) in enumerate(MUTATIONS):
        assert src.count(a) == 1, 'mutation %d: the text is not in nvn_entry.S: %r' % (k, a)
        m = os.path.join(out, 'mut%d_nvn_entry.S' % k); open(m, 'w', encoding='utf-8').write(src.replace(a, b))
        d = os.path.join(out, 'mut%d' % k); os.makedirs(d, exist_ok=True)
        import io, contextlib
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf): f, n, _ = part_a(build(ndk, d, m))
            ok = bool(f)
        except (UcError, AssertionError, KeyError, IndexError, StopIteration) as e: ok = True; f = [repr(e)]
        caught += ok
        print('  mutation %d (%s): %s' % (k, why, 'caught (%d checks fail)' % len(f) if ok else 'NOT CAUGHT'))
    return caught, len(MUTATIONS)


def show_counts(title, counts):
    print(title)
    cols = ['instructions', 'loads', 'stores', 'atomics', 'indirect branches', 'calls', 'system register reads']
    print('  %-78s %s' % ('path', ' '.join('%6s' % c.split()[0][:6] for c in cols)))
    for k, v in counts.items(): print('  %-78s %s' % (k, ' '.join('%6d' % v[c] for c in cols)))


# ---------------- Part B: the real libdm.so ----------------
def part_b(so, ndk, out):
    clang = os.path.join(ndk, 'clang.exe' if os.name == 'nt' else 'clang')
    res_s = os.path.join(out, 'res.S')
    open(res_s, 'w').write('''    .text
    .globl res_static
res_static:
    ldr x0, [x0, #8]
    ret
    .globl res_dynamic
res_dynamic:                   // bionic's fast path (model): x0 = the descriptor, returns the tp-relative address
    stp x19, x20, [sp, #-32]!
    stp x21, x22, [sp, #16]
    ldr x19, [x0, #8]          // the resolver argument {generation, module index, offset}
    mrs x1, tpidr_el0
    ldr x20, [x1]              // the DTV (tp[TLS_SLOT_DTV])
    ldr x21, [x19]             // the argument's generation
    ldr x22, [x20]             // the DTV's generation
    cmp x21, x22
    b.hi 1f
    ldr x21, [x19, #8]         // module index
    add x21, x20, x21, lsl #3
    ldr x21, [x21, #8]         // the module's TLS block
    cbz x21, 1f
    ldr x22, [x19, #16]        // the offset
    add x0, x21, x22
    sub x0, x0, x1
1:  ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #32
    ret
''')
    res_o = os.path.join(out, 'res.o')
    subprocess.check_call([clang, '--target=aarch64-linux-android29', '-c', res_s, '-o', res_o])
    rf = ELFFile(open(res_o, 'rb')); rtext = rf.get_section_by_name('.text').data()
    rsym = {x.name: x['st_value'] for x in rf.get_section_by_name('.symtab').iter_symbols() if x.name}

    f = ELFFile(open(so, 'rb'))
    sym = {}
    for sec in (f.get_section_by_name('.symtab'), f.get_section_by_name('.dynsym')):
        if sec:
            for x in sec.iter_symbols():
                if x.name and x['st_value'] and x.name not in sym: sym[x.name] = x['st_value']
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM); uc.ctl_set_cpu_model(C.UC_CPU_ARM64_MAX)
    BASE = 0x10000000
    hi = max(s['p_vaddr'] + s['p_memsz'] for s in f.iter_segments() if s['p_type'] == 'PT_LOAD')
    uc.mem_map(BASE, (hi + 0xFFFF) & ~0xFFFF)
    for s in f.iter_segments():
        if s['p_type'] == 'PT_LOAD': uc.mem_write(BASE + s['p_vaddr'], s.data())
    A = lambda n: BASE + sym[n]
    RES, TPM, TLSB, ARG = 0xC0000000, 0xC1000000, 0xC2000000, 0xC3000000
    for m in (RES, TPM, TLSB, ARG): uc.mem_map(m, 0x10000)
    uc.mem_write(RES, rtext)
    uc.mem_map(GUEST, 0x20000); uc.mem_map(BIGM, 0x120000); uc.mem_map(SENT, 0x1000); uc.mem_write(SENT, struct.pack('<I', 0xd65f03c0))
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.detail = False
    # nvn_bigstack_top's TLS descriptor: the adrp / ldr x1 pair names the GOT slot
    code = bytes(uc.mem_read(A('nvn_bigstack_top'), 0x100)); ins = list(md.disasm(code, A('nvn_bigstack_top')))
    got = None
    for a, b in zip(ins, ins[1:]):
        if a.mnemonic == 'adrp' and a.op_str.startswith('x0') and b.mnemonic == 'ldr' and b.op_str.startswith('x1, [x0'):
            page = int(a.op_str.split('#')[1], 16); off = int(b.op_str.split('#')[1].rstrip(']'), 16) if '#' in b.op_str else 0
            got = page + off; break
    assert got, 'no TLS descriptor in nvn_bigstack_top'
    TP = TPM + 0x100
    T_BIG_OFF = 0x40            # where t_big lives in the TLS block (model)
    # model: tp + x0 = TLSB + T_BIG_OFF
    uc.mem_write(TP, struct.pack('<Q', ARG + 0x800))                      # tp[0] = the DTV
    uc.mem_write(ARG + 0x800, struct.pack('<QQ', 5, TLSB))               # dtv {generation 5, modules[0] = the block}
    uc.mem_write(ARG, struct.pack('<QQQ', 3, 0, T_BIG_OFF))              # arg {generation 3, module index 0, offset}
    uc.mem_write(TLSB + T_BIG_OFF, struct.pack('<Q', BIGM + 0x4000))     # t_big: this thread has its big stack
    # a slot whose body is nvn_nop
    slot = 7; saddr = A('nvn_slot') + 16 * slot
    uc.mem_write(A('nvn_real_big') + 8 * slot, struct.pack('<Q', A('nvn_nop')))
    tab = A('nvn_lc_tab')
    results = {}
    def run(label, start, entry, resolver, lean_mode, sp=GSP, x16=None):
        uc.mem_write(got, struct.pack('<QQ', RES + rsym[resolver], (TLSB + T_BIG_OFF - TP) if resolver == 'res_static' else ARG))
        uc.mem_write(saddr, struct.pack('<QQ', 0, A(entry)))
        uc.mem_write(A('g_lc_mode'), struct.pack('<I', lean_mode))
        uc.mem_write(tab, b'\0' * 16 * TABN); uc.mem_write(tab + 16 * lc_hash(TP), struct.pack('<QQ', TP, TOP))
        uc.reg_write(C.UC_ARM64_REG_TPIDR_EL0, TP); uc.reg_write(C.UC_ARM64_REG_SP, sp); uc.reg_write(xr(30), SENT); uc.reg_write(xr(29), GFP)
        if x16 is not None: uc.reg_write(xr(16), x16)
        tr = []
        hk = uc.hook_add(UC_HOOK_CODE, lambda u, a, n, d: tr.append(a))
        try: uc.emu_start(start, SENT, count=5000)
        except UcError as e: raise AssertionError('%s: %s after %s' % (label, e, ' '.join('%#x' % (a - BASE if a >= BASE and a < RES else a) for a in tr[-8:])))
        uc.hook_del(hk)
        assert uc.reg_read(C.UC_ARM64_REG_PC) == SENT and uc.reg_read(C.UC_ARM64_REG_SP) == sp, label
        k = {'instructions': 0, 'loads': 0, 'stores': 0, 'atomics': 0, 'indirect branches': 0, 'calls': 0, 'system register reads': 0}
        body = A('nvn_nop')
        for a in tr:
            if a == body: continue
            i = next(md.disasm(bytes(uc.mem_read(a, 4)), a)); m = i.mnemonic
            k['instructions'] += 1
            if m in ('stadd', 'ldadd', 'ldxr', 'stxr', 'cas', 'casal', 'swp'): k['atomics'] += 1
            elif m.startswith(('ldr', 'ldp', 'ldur')): k['loads'] += 1
            elif m.startswith(('str', 'stp', 'stur')): k['stores'] += 1
            if m in ('br', 'blr'): k['indirect branches'] += 1
            if m in ('bl', 'blr'): k['calls'] += 1
            if m == 'mrs': k['system register reads'] += 1
        results[label] = k
    th, th80 = A('nvnth_%d' % slot), A('nvnth80_%d' % slot)
    for res, rn in (('res_static', 'static TLS resolver'), ('res_dynamic', 'dynamic TLS resolver')):
        run('old (=0): stadd thunk + wrapper + nvn_bigstack_top, %s' % rn, th, 'nvn_bigstack_entry', res, 0)
    run('old, already on the big stack (dynamic TLS resolver)', th, 'nvn_bigstack_entry', 'res_dynamic', 0, sp=TOP - 0x2000)
    run('lean (=1): plain thunk + lean entry, stack switch', th80, 'nvn_lean_entry', 'res_dynamic', 1)
    run('lean (=1): plain thunk + lean entry, in place', th80, 'nvn_lean_entry', 'res_dynamic', 1, sp=TOP - 0x2000)
    run('lean entry alone (stadd thunk kept), stack switch', th, 'nvn_lean_entry', 'res_dynamic', 1)
    return results


def model(counts):
    # [model] ns a call on a big out-of-order core (Cortex-X4 / A720 class at ~3 GHz): about 3.5 instructions a cycle on such straight
    # code, + 6 cycles per indirect branch / return pair beyond the first (BTB, return stack), + 12 cycles for an atomic RMW that waits
    # for its cache line; an in-order little core (A520 / A55): about 3x the time (the request's rule; dual issue, every load-use exposed).
    c = counts['instructions'] / 3.5 + 12 * counts['atomics'] + 2 * max(0, counts['indirect branches'] - 1) + 1.0 * counts['calls']
    return c / 3.0   # ns at 3 GHz


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--ndk', default=os.environ.get('NDK_BIN'), help='the NDK clang bin directory (or env NDK_BIN)')
    ap.add_argument('--so', help='a libdm.so built from this tree (part B)'); ap.add_argument('--keep')
    ap.add_argument('--mutations', action='store_true', help='also build 10 mutations of _nvn_lean_entry; part A must fail on each'); a = ap.parse_args()
    if not a.ndk: ap.error('give --ndk <ndk>/toolchains/llvm/prebuilt/<host>/bin or set NDK_BIN')
    out = a.keep or tempfile.mkdtemp(prefix='nr28lc'); os.makedirs(out, exist_ok=True)
    fails, n, counts = part_a(build(a.ndk, out))
    show_counts('Part A (nvn_entry.S + nvn_thunks.S as built; the body excluded):', counts)
    if a.mutations:
        print('Mutations of _nvn_lean_entry:')
        caught, total = mutation_check(a.ndk, out)
        if caught != total: fails.append('mutations: %d of %d caught' % (caught, total)); print('FAIL mutations: %d of %d caught' % (caught, total))
    if a.so:
        res = part_b(a.so, a.ndk, out)
        show_counts('Part B (%s: the real thunks, entries, nvn_bigstack_top and nvn_nop; the body excluded):' % os.path.basename(a.so), res)
        print('  model [m], ns a call at ~3 GHz on a big core (x3 on an in-order A520 / A55):')
        for k, v in res.items(): print('    %-78s %5.1f ns   (little ~%5.1f ns)' % (k, model(v), 3 * model(v)))
        old = res['old (=0): stadd thunk + wrapper + nvn_bigstack_top, dynamic TLS resolver']; lean = res['lean (=1): plain thunk + lean entry, stack switch']
        print('  saved a call: %d instructions, %d loads, %d stores, %d atomics, %d calls; model %.1f ns big / %.1f ns little; x 14.6k calls a frame = %.3f / %.3f ms'
              % (old['instructions'] - lean['instructions'], old['loads'] - lean['loads'], old['stores'] - lean['stores'], old['atomics'] - lean['atomics'],
                 old['calls'] - lean['calls'], model(old) - model(lean), 3 * (model(old) - model(lean)), 14600 * (model(old) - model(lean)) / 1e6,
                 3 * 14600 * (model(old) - model(lean)) / 1e6))
    if fails: print('FAIL: %d of %d checks' % (len(fails), n)); sys.exit(1)
    print('PASS: %d checks (lean switch / in place / passed on, the hash, DM_NVN_PROF=2 under the lean entry)' % n)

if __name__ == '__main__':
    main()
