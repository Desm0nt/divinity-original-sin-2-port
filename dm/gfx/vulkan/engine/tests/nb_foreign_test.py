# [DOS2] NB-15 host test: the DM_NB_FOREIGN counters of nvn_nb_tap.inc (docs/dos2/NATIVE_BACKEND_PLAN.md NB-15).
# The NB-15 block (from "// ---- NB-15" to the NBF_DRAW macro) is cut out of the tap verbatim, compiled with stubs for the few hle_nvn.c
# names it uses (t_nbd, module_of, the OP_* codes) and run: classes for every context (NB records, foreign in a taken draw, taken packet,
# opcode off the mask, nested packets, callback 1 / 2, UI block, direct, unknown caller, another module, other thread), the frame walk to
# the "game" caller through AArch64 frame records (synthetic: the block's __builtin_frame_address(0) is replaced by a chain the test builds,
# because x64 Windows frames are not laid out as {fp, lr} pairs; the real arm64 prologues are checked by disassembly, findings), the draw stash surviving NB's own records, the site table (same key, full table), the
# stats window (copy + zero), and that nothing is counted while the switch is off.
# Needs a host clang (C:/Program Files/LLVM/bin/clang.exe or clang on PATH). Run: python dm/gfx/vulkan/engine/tests/nb_foreign_test.py
from pathlib import Path
import os, re, shutil, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[4]
inc = (root / 'gfx/nvn/nvn_nb_tap.inc').read_text(encoding='utf-8')
m = re.search(r'(// ---- NB-15 .*?\n#define NBF_DRAW\(op\)[^\n]*\n)', inc, flags=re.S)
if not m:
    print('FAIL: nvn_nb_tap.inc has no NB-15 block'); sys.exit(1)
block = m.group(1)
if block.count('__builtin_frame_address(0)') != 1:
    print('FAIL: nbf_caller does not start the walk at its own frame record'); sys.exit(1)
block = block.replace('__builtin_frame_address(0)', 'test_frame()')
cc = shutil.which('clang') or next((p for p in ('C:/Program Files/LLVM/bin/clang.exe',) if os.path.exists(p)), None)
if not cc:
    print('SKIP: no host clang'); sys.exit(0)

test = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvn_nb_tap.h"
enum { OP_BIND_UBO = 1, OP_DRAW, OP_DRAW_IDX, OP_X_DRAW_CALL, OP_X_DRAW_INDIRECT, OP_X_MULTI_DRAW_INDIRECT, OP_X_DRAW_TEX, OP_X_NB_DRAW };
static __thread struct { int in, emitting; } t_nbd;
static const uint8_t *g_other_lo; static size_t g_other_sz;   // "another guest module"
static const void *module_of(const void *p) { return (size_t)((const uint8_t *)p - g_other_lo) < g_other_sz ? p : NULL; }
// AArch64 frame records {saved fp, saved lr}: fr[0] = nbf_caller's own, then host frames, then the one whose lr is the game's call site
static uintptr_t fr[16][2] __attribute__((aligned(16)));
static void *test_frame(void) { return fr[0]; }
#define MAIN_LO ((const uint8_t *)(uintptr_t)0x10000000u)
#define HOST_LR ((uintptr_t)0x7f0000001000ull)
static void chain(int host, uintptr_t game_lr) {   // host frames, then the game's call site (game_lr 0 = a chain of host frames only)
    memset(fr, 0, sizeof fr);
    for (int i = 0; i < 15; i++) { fr[i][0] = (uintptr_t)fr[i + 1]; fr[i][1] = i < host || !game_lr ? HOST_LR : game_lr; }
}
@BLOCK@
static int fails;
#define CHECK(c) do { if (!(c)) { printf("check failed line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static volatile int sink;
static void guest_call(uint32_t size) { NBF_REC(0, size); sink++; }    // a record (cmd_alloc)
static void guest_draw(uint32_t op) { NBF_REC(0, 24); NBF_DRAW(op); sink++; }   // a draw record (cmd_alloc, then rec)
static nbf_stats st;
static void take(void) { nvn_nb_foreign_stats(&st); }
int main(void) {
    const uint8_t *lo = MAIN_LO; const size_t sz = 0x1000000; const uint32_t SITE = 0x93cd6c;
    chain(3, (uintptr_t)lo + SITE);
    // off: nothing counted
    guest_call(16); nvn_nb_foreign_pkt(22, 0); guest_call(16); nvn_nb_foreign_pkt(-1, 0);
    take(); CHECK(st.rec[NBF_OFF_MASK] == 0 && st.rec[NBF_DIRECT] == 0 && st.rec[NBF_THREAD] == 0);
    nvn_nb_foreign_enable(lo, sz, 0, 0);   // "main" = the two guest functions; no UI range yet
    t_nbf.rt = 0;
    guest_call(16); take(); CHECK(st.rec[NBF_THREAD] == 1 && st.bytes[NBF_THREAD] == 16);   // never ran a packet: other thread
    guest_draw(OP_DRAW); take(); CHECK(st.drw[NBF_THREAD] == 1 && st.rec[NBF_THREAD] == 1);
    nvn_nb_foreign_pkt(22, 0); nvn_nb_foreign_pkt(-1, 0);   // now this is the render thread
    // outside packets: the walk finds the game's call site
    guest_call(16); guest_call(16); take();
    CHECK(st.rec[NBF_DIRECT] == 2 && st.walk_failed == 0 && st.other_module == 0);
    int ns = 0; uint32_t off = 0;
    for (int s = 0; s < NBF_SITES; s++) if (st.site[s].used) { ns++; off = st.site[s].off; CHECK(st.site[s].rec == 2 && st.site[s].op == NBF_OP_NONE); }
    CHECK(ns == 1 && off == SITE);
    chain(2, ((uintptr_t)lo + SITE) | 0x00ab000000000000ull); guest_call(8); take(); CHECK(st.rec[NBF_DIRECT] == 1);   // PAC / tag bits stripped
    for (int s = 0; s < NBF_SITES; s++) if (st.site[s].used) CHECK(st.site[s].off == SITE);
    chain(3, (uintptr_t)lo + SITE); fr[1][0] = (uintptr_t)fr[0]; guest_call(8); take();   // a saved fp that does not go up: stop
    CHECK(st.rec[NBF_OTHER] == 1 && st.walk_failed == 1);
    chain(3, (uintptr_t)lo + SITE); fr[1][0] = (uintptr_t)fr[2] + 4; guest_call(8); take(); CHECK(st.walk_failed == 1);   // misaligned fp
    chain(12, (uintptr_t)lo + SITE); guest_call(8); take(); CHECK(st.walk_failed == 1 && st.rec[NBF_OTHER] == 1);       // deeper than 10
    chain(9, (uintptr_t)lo + SITE); guest_call(8); take(); CHECK(st.rec[NBF_DIRECT] == 1);                                  // 10th record ok
    chain(3, (uintptr_t)lo + SITE);
    // UI block: the same call site inside the UI range
    nvn_nb_foreign_enable(lo, sz, off, off + 1); guest_call(8); take(); CHECK(st.rec[NBF_UI] == 1 && st.rec[NBF_DIRECT] == 0);
    nvn_nb_foreign_enable(lo, sz, 0, 0);
    // packets: off the mask, taken, nested, callbacks
    nvn_nb_foreign_pkt(22, 0); guest_call(32); guest_draw(OP_DRAW_IDX); nvn_nb_foreign_pkt(-1, 0); take();
    CHECK(st.rec[NBF_OFF_MASK] == 2 && st.drw[NBF_OFF_MASK] == 1 && st.op_rec[22] == 2 && st.op_drw[22] == 1 && st.op_bytes[22] == 56);
    for (int s = 0; s < NBF_SITES; s++) CHECK(!st.site[s].used);   // no walk inside an ordinary packet
    nvn_nb_foreign_pkt(9, 1); guest_call(8); nvn_nb_foreign_pkt(5, 0); guest_call(8); nvn_nb_foreign_pkt(-1, 0); guest_call(8); nvn_nb_foreign_pkt(-1, 0); take();
    CHECK(st.rec[NBF_TAKEN_PKT] == 2 && st.rec[NBF_OFF_MASK] == 1 && st.op_rec[9] == 2 && st.op_rec[5] == 1);
    nvn_nb_foreign_pkt(1, 0); guest_draw(OP_X_DRAW_INDIRECT); nvn_nb_foreign_pkt(-1, 0); take();
    CHECK(st.rec[NBF_CALLBACK] == 1 && st.drw[NBF_CALLBACK] == 1 && st.op_rec[1] == 1);
    ns = 0; for (int s = 0; s < NBF_SITES; s++) if (st.site[s].used) { ns++; CHECK(st.site[s].op == 1 && st.site[s].drw == 1); }
    CHECK(ns == 1);
    // a taken draw: foreign records; NB's own records do not move the draw stash
    t_nbd.in = 1; guest_draw(OP_X_DRAW_CALL); t_nbd.in = 0; take(); CHECK(st.rec[NBF_DRAW_FOREIGN] == 1 && st.drw[NBF_DRAW_FOREIGN] == 1);
    nvn_nb_foreign_pkt(22, 0);
    NBF_REC(0, 24);                                 // the draw record of the packet ...
    t_nbd.emitting = 1; NBF_REC(0, 64); NBF_DRAW(OP_X_NB_DRAW); t_nbd.emitting = 0;   // ... a flush / NB draw in between ...
    NBF_DRAW(OP_DRAW); NBF_DRAW(OP_BIND_UBO);       // ... the draw keeps the class of its own record; a non-draw op is no draw
    nvn_nb_foreign_pkt(-1, 0); take();
    CHECK(st.rec[NBF_NB] == 1 && st.bytes[NBF_NB] == 64 && st.drw[NBF_NB] == 1 && st.drw[NBF_OFF_MASK] == 1 && st.op_drw[22] == 1);
    // no caller in main: walk stopped / another module
    chain(0, 0); guest_call(8); take(); CHECK(st.rec[NBF_OTHER] == 1 && st.walk_failed == 1 && st.other_module == 0);
    chain(3, (uintptr_t)lo + SITE); g_other_lo = lo + SITE - 4; g_other_sz = 64; nvn_nb_foreign_enable(lo + 0x2000000, sz, 0, 0);   // lr in another module
    guest_call(8); take(); CHECK(st.rec[NBF_OTHER] == 1 && st.other_module == 1 && st.walk_failed == 0);
    g_other_sz = 0; nvn_nb_foreign_enable(lo, sz, 0, 0);
    // site table: same key -> same slot, full -> -1 and counted
    int a = nbf_site_of(0x1234, 0xff), b2 = nbf_site_of(0x1234, 0xff), c = nbf_site_of(0x1234, 1);
    CHECK(a >= 0 && a == b2 && c >= 0 && c != a);
    int full = 0; for (uint32_t i = 0; i < 2000; i++) if (nbf_site_of(0x100000 + 4 * i, 0xff) < 0) full++;
    take(); CHECK(full > 0 && st.site_full == (uint64_t)full);
    take(); CHECK(st.site_full == 0 && st.rec[NBF_DIRECT] == 0);   // the window starts empty
    if (fails) { printf("FAIL: %d checks\n", fails); return 1; }
    printf("ok\n"); return 0;
}
'''.replace('@BLOCK@', block)
bad = []
with tempfile.TemporaryDirectory() as d:
    c = Path(d) / 'nb_foreign_t.c'; exe = Path(d) / ('nb_foreign_t' + ('.exe' if os.name == 'nt' else ''))
    c.write_text(test, encoding='utf-8')
    r = subprocess.run([cc, '-std=gnu11', '-O1', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-unused-parameter',
                        '-I', str(root / 'gfx/nvn'), str(c), '-o', str(exe)], capture_output=True, text=True)
    warn = [l for l in (r.stdout + r.stderr).splitlines() if 'warning:' in l or 'error:' in l]
    if r.returncode: print('FAIL: compile\n' + r.stdout + r.stderr); sys.exit(1)
    if warn: bad += warn
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    if r.returncode or 'ok' not in r.stdout: bad.append('run: ' + (r.stdout + r.stderr).strip())
if bad:
    print('FAIL: NB-15 foreign counters\n  ' + '\n  '.join(bad)); sys.exit(1)
print('PASS: NB-15 DM_NB_FOREIGN: classes (NB / taken draw / taken packet / off mask / nested / callback / UI / direct / unknown / '
      'another module / other thread), frame walk to the caller, draw stash across NB records, site table, stats window, off = nothing counted')
