"""[NR-24] entry.S _dm_gate_fast under an AArch64 emulator (unicorn): the real assembly, built with the NDK's clang (asm_elf.py, then
linked with a stub data file at a fixed address), run against a gate table, a tick block and a counter value this script sets (the
mrs cntvct_el0 is hooked). Each case starts with every x0..x30 at a distinct pattern, NZCV set, sp pointing at the gate block's frame
([sp] = the guest's x29) and stops at the return point (fast) or at _dm_gate (passed on). Checks: the value in xN, every other register,
sp and NZCV as they came in, x29 = the guest's; a passed-on gate reaches _dm_gate with every register and sp untouched.
Cases: every destination x0..x28 (x18 passes on), x29 / x30 pass on, SVC / obs / probe kinds pass on, an unknown return point passes
on, a hash collision chain, a frozen block (valid 0), no block, a counter read before c0 (clamped to base), a 26 MHz scale (128-bit),
the DM_NR_CPU count by register, a block published between the counter read and the re-check (passes on).
Usage: python t_nr24_fastgate.py [--ndk <ndk bin dir>] [--keep <dir>]   (needs: pip unicorn, pyelftools; prints PASS / FAIL)"""
import argparse, os, struct, subprocess, sys, tempfile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_INSN
from unicorn.arm64_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_C = r'''
#include <stdint.h>
typedef struct { uint64_t ret; int kind, arg; uint64_t orig; } gate_t;
gate_t dm_gate_tab[4096];
void *dm_tick_cur;
void (*dm_gate_prof)(int, int, uint64_t);
uint64_t dm_tick_fast_n[32];
uint64_t gate_dispatch(uint64_t r, uint64_t *x) { (void)r; (void)x; return 0; }
uint64_t unimpl_c(unsigned i, uint64_t *x) { (void)i; (void)x; return 0; }
void dm_thread_returned(void) {}
'''

def build(ndk, out):
    src = os.path.join(HERE, '..', 'entry.S')
    elf_s = os.path.join(out, 'entry_elf.S'); data = os.path.join(out, 'fg_data.c'); elf = os.path.join(out, 'fg.elf')
    subprocess.check_call([sys.executable, os.path.join(HERE, '..', '..', 'android', 'tools', 'asm_elf.py'), src, elf_s])
    open(data, 'w').write(DATA_C)
    clang = os.path.join(ndk, 'clang.exe' if os.name == 'nt' else 'clang')
    subprocess.check_call([clang, '--target=aarch64-linux-android29', '-O1', '-nostdlib', '-static', '-fno-pic', '-fuse-ld=lld',
                           '-Wl,-e,dm_gate_fast', '-Wl,--image-base=0x400000', '-o', elf, elf_s, data])
    return elf

def gate_h(a): return (((a >> 2) * 0x9E3779B97F4A7C15) & (2**64 - 1)) >> 52

REGS = [UC_ARM64_REG_X0 + i for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]   # x0..x28 are consecutive in unicorn
def xreg(n): return REGS[n]

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--ndk', default=os.environ.get('NDK_BIN'), help='the NDK clang bin directory (or env NDK_BIN)')
    ap.add_argument('--keep'); a = ap.parse_args()
    if not a.ndk: ap.error('give --ndk <ndk>/toolchains/llvm/prebuilt/<host>/bin or set NDK_BIN')
    out = a.keep or tempfile.mkdtemp(prefix='nr24fg'); os.makedirs(out, exist_ok=True)
    elf_path = build(a.ndk, out)
    f = ELFFile(open(elf_path, 'rb')); sym = {s.name: s['st_value'] for s in f.get_section_by_name('.symtab').iter_symbols() if s.name}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    lo = min(s['p_vaddr'] for s in f.iter_segments() if s['p_type'] == 'PT_LOAD') & ~0xFFF
    hi = max(s['p_vaddr'] + s['p_memsz'] for s in f.iter_segments() if s['p_type'] == 'PT_LOAD')
    size = (hi - lo + 0xFFFF) & ~0xFFFF
    uc.mem_map(lo, size)
    for s in f.iter_segments():
        if s['p_type'] == 'PT_LOAD': uc.mem_write(s['p_vaddr'], s.data())
    STACK, RET_BASE, BLK = 0x80000000, 0x90000000, 0xA0000000
    uc.mem_map(STACK - 0x10000, 0x20000); uc.mem_map(RET_BASE, 0x100000); uc.mem_map(BLK, 0x10000)
    tab, cur, prof, fastn, fast, slow = sym['dm_gate_tab'], sym['dm_tick_cur'], sym['dm_gate_prof'], sym['dm_tick_fast_n'], sym['dm_gate_fast'], sym['dm_gate']
    state = {'cnt': 0, 'mrs_hook': None}

    def on_mrs(uc_, reg, cp_reg, ud):
        # cp_reg: (crn, crm, op0, op1, op2, val); CNTVCT_EL0 = op0 3, op1 3, crn 14, crm 0, op2 2
        if (cp_reg.crn, cp_reg.crm, cp_reg.op0, cp_reg.op1, cp_reg.op2) == (14, 0, 3, 3, 2):
            uc_.reg_write(reg, state['cnt'])
            if state['mrs_hook']: state['mrs_hook']()
            return True
        return False
    uc.hook_add(UC_HOOK_INSN, on_mrs, None, 1, 0, UC_ARM64_INS_MRS)

    fails = []; checks = [0]
    def check(c, msg):
        checks[0] += 1
        if not c: fails.append(msg); print('FAIL', msg)

    def clear_tab():
        uc.mem_write(tab, b'\0' * 24 * 4096)
    def put_gate(ret, kind, arg):
        i = gate_h(ret)
        while struct.unpack('<Q', bytes(uc.mem_read(tab + 24 * i, 8)))[0]: i = (i + 1) & 0xFFF
        uc.mem_write(tab + 24 * i, struct.pack('<QiiQ', ret, kind, arg, 0))
    def set_blk(valid, c0, base, mul, where=BLK):
        uc.mem_write(where, struct.pack('<QQQQQ', valid, c0, base, mul, 1)); uc.mem_write(cur, struct.pack('<Q', where))

    def run(ret, guest_x29=0x2929292929292929):
        pat = {n: 0x1111000000000000 + n * 0x0101010101 for n in range(29)}
        for n in range(29): uc.reg_write(xreg(n), pat[n])
        sp = STACK - 0x100
        uc.mem_write(sp, struct.pack('<QQ', guest_x29, 0x3030303030303030))   # the gate block's frame
        uc.reg_write(UC_ARM64_REG_X29, guest_x29); uc.reg_write(UC_ARM64_REG_X30, ret); uc.reg_write(UC_ARM64_REG_SP, sp)
        nz = 0x60000000; uc.reg_write(UC_ARM64_REG_NZCV, nz)
        uc.mem_write(ret, struct.pack('<I', 0xD503201F))                        # nop at the return point
        stop = {}
        def on_code(uc_, addr, sz, ud):
            if addr in (ret, slow): stop['at'] = addr; uc_.emu_stop()
        h = uc.hook_add(UC_HOOK_CODE, on_code)
        try: uc.emu_start(fast, 0, count=2000)
        except UcError as e: stop['err'] = str(e)
        uc.hook_del(h)
        regs = {n: uc.reg_read(xreg(n)) for n in range(29)}
        return stop, pat, regs, uc.reg_read(UC_ARM64_REG_X29), uc.reg_read(UC_ARM64_REG_X30), uc.reg_read(UC_ARM64_REG_SP), uc.reg_read(UC_ARM64_REG_NZCV) & 0xF0000000, sp, nz

    def expect_fast(name, ret, n, want):
        stop, pat, regs, x29, x30, sp, nzcv, sp0, nz = run(ret)
        check(stop.get('at') == ret, '%s: stopped at %s (want the return point; err %s)' % (name, hex(stop.get('at', 0)), stop.get('err')))
        check(regs[n] == want, '%s: x%d = %#x, want %#x' % (name, n, regs[n], want))
        others = [k for k in range(29) if k != n and regs[k] != pat[k]]
        check(not others, '%s: registers changed: %s' % (name, others))
        check(x29 == 0x2929292929292929 and x30 == ret and sp == sp0 and nzcv == nz, '%s: x29 %#x x30 %#x sp %#x nzcv %#x' % (name, x29, x30, sp, nzcv))
    def expect_slow(name, ret):
        stop, pat, regs, x29, x30, sp, nzcv, sp0, nz = run(ret)
        check(stop.get('at') == slow, '%s: stopped at %s (want _dm_gate; err %s)' % (name, hex(stop.get('at', 0)), stop.get('err')))
        others = [k for k in range(29) if regs[k] != pat[k]]
        check(not others and x29 == 0x2929292929292929 and x30 == ret and sp == sp0 and nzcv == nz, '%s: passed on with changes: regs %s x29 %#x x30 %#x sp %#x' % (name, others, x29, x30, sp))

    clear_tab(); uc.mem_write(prof, b'\0' * 8); uc.mem_write(fastn, b'\0' * 256)
    set_blk(1, 1000, 5_000_000, 0)
    ret = RET_BASE + 0x40
    # every destination register
    for n in range(31):
        r = RET_BASE + 0x1000 + n * 0x40; put_gate(r, 1, n)
        state['cnt'] = 1000 + 777 * (n + 1)
        if n in (18, 29, 30): expect_slow('mrs x%d' % n, r)
        else: expect_fast('mrs x%d (cntpct)' % n, r, n, 5_000_000 + 777 * (n + 1))
    r = RET_BASE + 0x3000; put_gate(r, 2, 21); state['cnt'] = 3000; expect_fast('mrs x21 (cntvct kind)', r, 21, 5_000_000 + 2000)
    for kind, nm in ((0, 'svc'), (3, 'obs'), (4, 'probe')):
        r = RET_BASE + 0x4000 + kind * 0x40; put_gate(r, kind, 0); expect_slow('kind %s' % nm, r)
    expect_slow('unknown return point', RET_BASE + 0x7ff0)
    # a collision chain: three gates whose return points hash to one slot
    base = RET_BASE + 0x10000; h0 = gate_h(base); coll = [base]
    a_ = base + 4
    while len(coll) < 3:
        if gate_h(a_) == h0: coll.append(a_)
        a_ += 4
    for k, c in enumerate(coll): put_gate(c, 1, 3 + k)
    state['cnt'] = 1500
    for k, c in enumerate(coll): expect_fast('collision chain #%d' % k, c, 3 + k, 5_000_000 + 500)
    # frozen, no block, clamp, scale
    r = RET_BASE + 0x5000; put_gate(r, 1, 0)
    set_blk(0, 0, 123, 0); expect_slow('frozen block', r)
    uc.mem_write(cur, b'\0' * 8); expect_slow('no tick block', r)
    set_blk(1, 10_000, 7_000_000, 0); state['cnt'] = 9_000; expect_fast('counter before c0 (clamped)', r, 0, 7_000_000)
    mul = ((19200000 << 32) + 13000000) // 26000000
    set_blk(1, 100, 1_000, mul); d = 1 << 40; state['cnt'] = 100 + d; expect_fast('26 MHz scale, 2^40', r, 0, 1_000 + ((d * mul) >> 32))
    set_blk(1, 100, 1_000, mul); d = 26_000_000; state['cnt'] = 100 + d; expect_fast('26 MHz scale, 1 s', r, 0, 1_000 + ((d * mul) >> 32))
    check(abs(((26_000_000 * mul) >> 32) - 19_200_000) <= 1, 'scale: 1 s of 26 MHz = %d ticks' % ((26_000_000 * mul) >> 32))
    # the DM_NR_CPU count
    uc.mem_write(prof, struct.pack('<Q', 0x1234)); set_blk(1, 0, 0, 0); state['cnt'] = 5
    for _ in range(3): expect_fast('counted read', r, 0, 5)
    check(struct.unpack('<Q', bytes(uc.mem_read(fastn, 8)))[0] == 3, 'DM_NR_CPU count by register: %d (want 3)' % struct.unpack('<Q', bytes(uc.mem_read(fastn, 8)))[0])
    uc.mem_write(prof, b'\0' * 8)
    # a block published between the counter read and the re-check
    set_blk(1, 0, 0, 0)
    state['mrs_hook'] = lambda: uc.mem_write(cur, struct.pack('<Q', BLK + 0x100))
    uc.mem_write(BLK + 0x100, struct.pack('<QQQQQ', 1, 0, 0, 0, 1))
    expect_slow('block changed under the read', r)
    state['mrs_hook'] = None
    print('%s: %d checks, %d failed (entry.S _dm_gate_fast under unicorn, %s)' % ('PASS' if not fails else 'FAIL', checks[0], len(fails), elf_path))
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
