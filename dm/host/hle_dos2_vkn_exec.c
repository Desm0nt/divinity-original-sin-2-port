// [DOS2] VK-NATIVE stage 3, part "packets and state functions" (dm_vkn_exec_register; DM_VK_NATIVE=1 only): the engine's command
// list executor and renderer state functions without NVN. main = main.raw of update v786432, build 4979b200; r = the renderer
// ([main+0x2bbdc68], = [list+0x38]); dev = [r+0xb70]; frame stamp = [dev+0x51f0]. Facts from the decompilations of 0x93e5f0 (and its
// thunks 0x93fd30 0x93fd70 0x93fda0 0x93fea0 0x93fee0 0x93ff20), 0x9283a0 0x9284e0 0x92c3c0 0x92c130 0x93ffc0 0x940f90 0x93e400
// 0xa7eac0, and the oracle-proven model of opcode 0x15 (dm/gfx/nr/nr_state.h). Map: docs/dos2/nr/VK-NATIVE-MAP-3.md part 6.
//
// Design (docs/dos2/nr/VK-NATIVE-STATUS.md "Этап 3"): the executor ExecuteRenderCommand 0x93e5f0(list, opcode, packet) -> length is NOT
// rewritten. The engine's switch keeps every opcode whose handler is NVN-free or only calls renderer functions other parts replace;
// this file takes (dn_install_rc_take, hle_dos2_native.c) exactly the opcodes whose handler calls NVN INLINE, and replaces the renderer
// state functions with NVN inside. Draw state is not "bound" here: the draw part reads the renderer cache at each draw, so a state
// opcode / function only writes the SAME cache fields (and record frame stamps) the original writes, with the original's
// compare-with-cache rules (other engine code reads the fields and relies on the skips), and nothing else.
//
// ---- The 64 opcodes of 0x93e5f0 (length = what the handler returns; "pass/draw/buf/imm" = the function another part replaces) ----
//   00 data block, u32[+4]+8, skipped            | 20 0x91d890 destroy IB (buf), 0x10        | 40.. default: length 0
//   01 inline callable [+8](pkt+0x18,1,0)+0x18 *  | 21 0x924730 texture flush (draw), 1       |
//   02 callback [+0x10]([+8], u8 [+0x18]), 0x20 * | 22 nothing, 1                            |
//   03 0x928ab0 clear (pass), 0x30                | 23 DrawArrays INLINE -> TAKEN, 0xc        |
//   04 region clear, ClearTexture INLINE -> TAKEN | 24 0x928e80 (draw), 0x14                  |
//      0x40                                       | 25 0x928fb0 (draw), 0x1c                  |
//   05 0x928be0 depth clear (pass), 0x20          | 26 0x93fea0 -> 0x930910 (imm), 0x50       |
//   06 0x9284e0 ApplyBlend (HOOKED here), 0x18    | 27 0x93fee0 -> 0x930e90 (imm), 0x90       |
//   07 raster INLINE -> TAKEN, 0x10               | 28 0x92f730 (imm), 0x40                   |
//   08 depth-stencil + ref INLINE -> TAKEN, 0x18  | 29 0x93ff20 = b 0x92fc70 (imm), 0x40      |
//   09 0x9283a0 BindShader (HOOKED here), 0x10    | 2a 0x930210 (imm), 0xa0                   |
//   0a vertex format INLINE -> TAKEN, 0x10        | 2b 0x9290f0 dispatch (draw), 0x10         |
//   0b vertex buffer + offset INLINE -> TAKEN 0x18| 2c texture rows: nvnTextureGetStride (not a command-buffer call) + memcpy
//   0c index buffer: cache + stamp only, 0x10     |    into the NVN texture's storage [rec0+0xe0]; rows*pitch+0x2f (UI wave)
//   0d primitive: cache only, 2                   | 2d texture "locked" byte = 0, 0x18        |
//   0e 0x93fd30 -> 0x9249f0 BindImage (draw) 0x18 | 2e staged VB update: memcpy into ring r+0x1870 + node r+0x18a0 (no NVN)
//   0f 0x9243d0 texture slot (no NVN), 0x20       | 2f VB -> VB memcpy, 0x18                  |
//   10 0x93fd70 -> 0x9242c0 storage buf (draw)0x18| 30 0x9218c0 staged copy (buf) + 0x921bf0, 0x10
//   11 string release 0x801dd0 only, 0x18         | 31 0x921c60 IB lock (no NVN) + memcpy, size+0x2f
//   12 viewport INLINE (+ SetScissor) -> TAKEN    | 32 0x921dc0 -> 0x9218c0 (buf), 0x10       |
//      0x14                                       | 33 0x921b10 IB copy memcpy, 0x18          |
//   13 scissor INLINE -> TAKEN, 0x14              | 34 0x920b00 texture copy (pass), 0x18     |
//   14 16 bytes -> [r+0x888]+0x150, 0x14          | 35 nothing, 0x28                          |
//   15 full draw state INLINE -> TAKEN, 0x70      | 36 r+0x930..0x93c scale/bias, 0x14        |
//   16 0x9279a0 SetRenderTargets (pass), var.     | 37 barrier: state 0/1 nvnBarrier INLINE -> TAKEN, 0x20
//   17 camera matrices -> [r+0x888], 0x98         | 38 3b nothing, 0xc; 39 3a nothing, 2; 3c nothing, 0x18
//   18 0x92af30 (math only), 0x4c                 | 3d / 3e ReportCounter INLINE -> TAKEN, 0x10
//   19 / 1a 0x925130 constants -> shadow (no NVN) | 3f conditional render INLINE -> TAKEN, 0x10
//   1b 0x927310 (math), 1; 1c 0x924bb0 (copy)     |
//   1d / 1e 0x917630 / 0x9179d0 create (buf)      |   * 01 / 02: the callee is a pointer in the packet: not classifiable
//   1f 0x91db40 destroy VB (buf), 0x10            |     statically; this part counts them (census) but leaves them alone.
// Differences from VK-NATIVE-MAP-3.md part 6 found here: 0x37 state 1 is a barrier too (map: only 0); 0x2c has no command-buffer
// call (TextureGetStride + memcpy into NVN storage: the UI / texture-update wave, not this part, although STATUS lists it as taken);
// 0x29's handler 0x93ff20 is a tail branch to 0x92fc70 (Ghidra shows it inlined); 0x0e / 0x10 / 0x1a go through thunks
// 0x93fd30 / 0x93fd70 / 0x93fda0 (+ a string release); 0x0a binds 3 / 2 / 1 vertex streams by format record +0x1c / +0x18;
// 0x12 sets the NVN scissor to the viewport rect WITHOUT writing the scissor cache (see dm_vkn_exec_scissor); 0x15 rebinds VB1 / VB2
// only for non-zero handles (a zero keeps the old cache entry) and the IB only when non-zero; the IB frame stamp (0x0c / 0x15) has
// no generation check.
//
// ---- Coverage map of stage 3: every function of 0x913000..0x942000 that loads an NVN entry (linear sweep of main.raw for loads
// from the NVN proc table 0x2bbee98..0x2bbfea8: 61 functions, every load inside a known function) ----
//   command-buffer calls:
//     frame   0x9215f0 BeginFrame, 0x93d9f0 per-frame setup, 0x93ccd0 EndFrame (+0x93cef0)
//     pass    0x9279a0 SetRenderTargets, 0x928ab0 clear, 0x928be0 depth clear, 0x920b00 texture copy
//     draw    0x926f90 + 0x925740 constant flushes, 0x924730 texture flush, 0x9249f0 images, 0x9242c0 storage buffers,
//             0x928e80 DrawElements, 0x928fb0 instanced, 0x9290f0 dispatch
//     buf     0x9218c0 staged copy (CopyBufferToBuffer)
//     YOURS   0x93e5f0 (taken opcodes above), 0x9283a0 BindShader, 0x9284e0 ApplyBlend, 0x92c3c0 ApplyDrawState, 0x92c130 format + VB0,
//             0x93ffc0 SetRasterState (immediate branch), 0x940f90 DrawArrays writer (immediate branch)
//     imm     0x92f730 0x92fc70 0x930210 0x930910 0x930e90 (immediate draws, later wave)
//     create  0x939850 context (CommandBufferInitialize / AddCommandMemory, stage 4)
//   no command-buffer call (creation-time / queries / read-back; NVN objects still exist until stage 4):
//     create  0x914bb0 0x917630 0x9179d0 0x9181d0 0x919090 0x91e2a0 0x91f2e0 0x921e40 0x922530 0x922d10 0x923360 0x934150 0x938330
//             0x939120 0x93c3a0; destroy 0x91ad20 0x91ce90 0x91d5c0 0x91d630 0x91d890 0x91db40 0x9239f0 0x923c20 0x923e60 0x924080
//             0x934910; getters 0x920460 0x9282c0 (channel-mask state + TextureGetFormat), 0x9410f0 (TextureGetStride: writer of 0x2c);
//     read-back 0x9291c0 0x9296d0 0x929880 (TextureReadTexels); window 0x93cc20 (WindowSetCrop)
//   outside the renderer (later wave; nvn_by_function.txt): immediate branches of two-mode functions (list+0x30 == 0 -> NVN now, else
//   a packet through AllocCommand 0x941720) and immediate-only stage functions:
//     DrawElements / DrawArrays + SetRenderEnable: 0xaab150 0xabe690 0xabebf0 0xabf1e0 0xaf2ed0 0x1240ee0 (two-mode), 0x1335a30
//       (immediate only), 0xb89330 (+ ReportCounter x2), 0xab9fa0 (+ 17 barriers)
//     full immediate draws (state binds + draw, no packet path): 0xb0aab0 (barriers, SetRenderTargets 0x9279a0, viewport, DrawArrays,
//       0x92c130), 0xd1ada0 (state, viewport, scissor, DrawArrays)
//     state binds: 0xabda50 (+ DrawElements) 0xb23970 0x142c870 (+ conditional render) 0xafa7c0 (polygon x2)
//     viewport + scissor: 0xab50e0 0xab5390 0xb15090 0xb20220 0xb20650 0xb0a880; render targets 0xd1fd70
//     ReportCounter: 0x90cbf0 0xb7e020 0xb87470; barriers: 0xab30e0 0xab46e0 0xab5a00 0xab6e90 0xab71f0 0xac1270 0xac1a80 0xac2de0
//       0xac3260 0xac6070 0xac8d10 0xb1c580 0xb1d6a0 0xd20d40 0x14220b0 0x14227a0 (+ ClearTexture x2) 0x1424560
//     the top-level frame 0xa66bc0 (Barrier, EndRecording, QueueFenceSync / Flush / SubmitCommands, SyncWait): immediate only
//     UI block 0x1bea840..0x1bf4290 (Iggy; UI wave)
//   Can list+0x30 be 0? Yes: 0x93e400 (the only list constructor: renderer at +0x38, +0x30 = 1) is followed in 0xa7eac0 by
//   `strb wzr, [x20, #0x30]` (0xa7ed44) for the second list it builds (kept at object+0x68): an immediate list. The phone confirms
//   it (NR-02 census, work/logs/nr3_r1-dumps / nr4_b2-dumps nr_draw_sites.tsv): draws outside every command list at 0xaab224
//   (0xaab150), 0x1241144 (0x1240ee0), 0xb0ae90 (0xb0aab0), 0xd1b2ec (0xd1ada0), thousands per 10 s; 0x941010 (0x940f90) none seen.
//   So the immediate branches run in mode 1 and need replacements in the later wave.
//
// ---- Stage 4: visibility queries and conditional rendering (facts from the decompilations of 0x93e5f0 cases 0x15 0x23 0x3d 0x3e
// 0x3f, 0x928e80, 0x9215f0, 0x938330, 0xb7e020, 0x90cbf0, 0xb87470, 0xb89330, 0xb23970, 0x142c870 and the disassembly of 0xb0c570;
// docs/dos2/nr/NR-03.md section 1 read the same) ----
//   Counter buffer: device init 0x938330 (0x938edc..0x939030) allocates 0x10000 bytes of CPU table at dev+0x25ab0 (window texture 0)
//     / dev+0x25ab8 (= +0x8000, window texture 1): entry k (16 bytes) = {+0 GPU address [dev+0x25b18] + 0x20 * k, +8 frame}, 2048
//     entries a half; the GPU buffer (0x20000, class 4) is mapped at dev+0x25ac8: two 16-byte counter reports per pair (begin at +0,
//     end at +0x10).
//   BeginFrame 0x9215f0(r, 1): after AcquireTexture, the CPU writes the counter words of this window texture's half through
//     dev+0x25ac8 + (index << 12) * 16: begin.counter = 0, end.counter = -1 for all 2048 pairs, and dev+0x25ac0 (pairs taken) = 0.
//   Allocation 0xb0c5c0..0xb0c61c (inside the per-object gather 0xb0c570(x0, object, x2)): object flags [obj+0x1d8] with bit 18
//     (0x40000) and any other bit: k = atomic fetch_add(dev+0x25ac0, 1) (ldaxr / stlxr); k < 0x800: entry = [dev+0x25ab0 + 8 *
//     [dev+0x31e0]] + 16 k, entry+8 = frame [dev+0x51f0], obj+0x268 = entry, obj+0x270 = frame; k >= 0x800 (0xb0dac0): obj+0x268 = 0,
//     obj+0x270 = -1 (never valid).
//   Users (0xb7e020, same shape in 0x90cbf0 0xb87470 0xb89330 0xb23970 0x142c870): q = obj+0x268 only when obj+0x270 == [dev+0x51f0]
//     (THIS frame), else 0. The pass flags (list param +8): & 0x48002 -> no condition; else the full-state packet 0x15 carries q at
//     +0x60 (immediate list: 0x92c3c0 + SetRenderEnableConditional through the game's NVN table, 0x2bbfcd8). Pass flag bit 18 with q
//     != 0: the object's own draw (0x928fb0 / packet 0x25) is bracketed by packet 0x3d {+8 q} = ReportCounter(cb, 1 = SAMPLES_PASSED,
//     *q) and packet 0x3e = ReportCounter(cb, 1, *q + 0x10) (immediate lists: ReportCounter through table slot 0x2bbfcb8 directly,
//     i.e. hle_dos2_vkn_cmd.c's counted no-op - those pairs are not seen here and their consumers are drawn).
//   Condition: packet 0x15 / 0x3f (and 0xb23970 / 0x142c870 inline) -> SetRenderEnableConditional(cb, 1 = RENDER_IF_NOT_EQUAL, *q):
//     the GPU compares the counter words at *q and *q + 0x10 and renders when they differ, i.e. samples = end - begin != 0; q = 0 ->
//     SetRenderEnable(cb, 1). Cache r+0x185c = (q != 0) / r+0x1860 = q (set only when either differs), r+0x185d = 0. The draw tail
//     (0x928e80 / packet 0x23): r+0x185d && r+0x185c -> SetRenderEnable(1), both 0, r+0x1860 = 0; draw; r+0x185d = r+0x185c. So a
//     condition covers exactly ONE draw (the next one) and at that draw r+0x185c != 0 <=> conditional.
// Answers (a): the condition always names a pair of the SAME frame (both sides check obj+0x270 == frame; a pair address names
//   another object every frame: allocation order, the halves alternate with the window texture). The bracket is drawn by the pass with
//   flag 0x40000 earlier in the same recording (NR-03 phone 17.11, deck: all 263 pairs of a frame closed in ONE pass, 0 consumers in
//   the pair's pass, 682 conditions a frame, the Switch skips 346 draws = 30 % of draws, 24 % of vertices). Compared: begin / end
//   counter words, samples = end - begin; "render" when != 0. A pair never reported this frame still holds {0, -1}: renders. A
//   consumer before its bracket (same frame) therefore renders. The CPU never reads the counters.
// (b) Mode-1 phone run V3_e (20.11, 9 427 frames incl. menus): 4.53 M pairs (packets 0x3d / 0x3e), 4.77 M condition sets with a
//   query (each covers one draw) = 481 pairs / 506 conditional draws a frame on average over the whole run; the Fort Joy census of
//   the brief: ~800 pairs and ~870 conditional draws in a frame of ~2400 draws.
// Native equivalent (dm/gfx/vkn/vkn_query.h, DM_VK_NATIVE_COND): a pair = one occlusion query around the draws recorded between the
//   two packets, results copied into predicate words when the pass ends, the consumer drawn under VK_EXT_conditional_rendering on its
//   word: exact, same frame. Entry points through vkGetDeviceProcAddr (it is in the trampoline table), the device extension by the
//   front end (android/vulkan_present.cpp asks for it with DM_VK_NATIVE=1 and DM_VK_NATIVE_COND 1 / 3).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_query.h"
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_exec.h"
#include "hle_dos2_vkn_stack.h"

#define EX_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark, a literal of its own

int dn_install_rc_take(uint64_t mask, int (*take)(uint64_t list, unsigned op, uint64_t pkt, uint64_t *len));   // hle_dos2_native.c
// Other parts (weak: absent until merged). The pass part: a region clear of one layer (level 0) of texture `tex` (engine handle),
// colour rgba, rect x, y, w, h. The draw part: packet 0x23's draw (the draw-time flushes 0x926f90 + 0x925740 when a shader is bound and
// 0x924730, then DrawArrays with primitive r+0x1718), reading the renderer itself; 0 = recorded.
void dm_vkn_pass_clear_rect(uint64_t tex, uint32_t layer, const float rgba[4], int32_t x, int32_t y, int32_t w, int32_t h) __attribute__((weak));
int dm_vkn_draw_arrays(uint32_t first, uint32_t count) __attribute__((weak));

// ---- engine offsets ----
enum {
    L_RECORD = 0x30, L_R = 0x38,                       // list: u8 1 = writes packets, 0 = executes now; the renderer
    R_DEV = 0xb70, DEV_FRAME = 0x51f0, R_CSFLAG = 0xb78,
    T_TEX = 0x0, T_SHADER = 0xc0, T_VB = 0x180, T_IB = 0x240, T_VFMT = 0x480, T_BLEND = 0x600, T_DS = 0x6c0, T_RASTER = 0x780,   // tables
    E_TEX = 0x28, E_SHADER = 0x60, E_VB = 0x68, E_IB = 0x58, E_VFMT = 0x80, E_BLEND = 0x20, E_DS = 0x20, E_RASTER = 0x18,          // records
    VB_LAST = 0x38, IB_LAST = 0x18,
    R_RASTER = 0x16f0, R_BLEND = 0x16f8, R_BLEND_MASK = 0x1700, R_DS = 0x1708, R_REF = 0x1710, R_PRIM = 0x1714, R_NVN_PRIM = 0x1718,
    R_VP = 0x171c /* w, h, x, y */, R_SC = 0x172c /* left, top, right, bottom */, R_VFMT = 0x1740, R_VB = 0x1748, R_VB_OFF = 0x1788,
    R_IB = 0x17c8, R_IB_FIRST = 0x17d0, R_SHADER = 0x17d8, R_SHADER_REC = 0x17e0, R_REN_NEED = 0x185c, R_REN_OFF = 0x185d,
    R_REN_Q = 0x1860, R_ALPHA = 0x1868,
    M_BLEND_DEFAULT = 0x2bbdb70,                       // the engine's default-blend handle (0x9284e0: default blend + colour state)
    M_RASTER_TAB = 0x2bbdc18,                          // 0x93ffc0: the 10 raster handles by index
    M_PRIM_TAB = 0x1f58640,                            // 7 x u32: engine primitive 1..7 -> NVN draw mode
    VB_SLOTS = 8,                                      // r+0x1748..0x1787
};
static inline void w64(uint64_t p, uint64_t v) { memcpy((void *)(uintptr_t)p, &v, 8); }
static inline void w32(uint64_t p, uint32_t v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static inline void w8(uint64_t p, uint8_t v) { *(uint8_t *)(uintptr_t)p = v; }
static inline float rdf(uint64_t p) { float f; memcpy(&f, (const void *)(uintptr_t)p, 4); return f; }
static inline void wf(uint64_t p, float f) { memcpy((void *)(uintptr_t)p, &f, 4); }

// ---- statistics: every drop / unexpected condition has a counter; the first 16 of each print one red line ----
enum {
    // state function calls (hooks)
    S_SHADER, S_SHADER_SKIP, S_BLEND, S_BLEND_SKIP, S_BLEND_DEFAULT, S_STATE, S_FMTVB, S_RASTER_IMM, S_RASTER_LIST, S_ARRAYS_IMM, S_ARRAYS_LIST,
    // packet effects
    S_VIEWPORT, S_SCISSOR, S_CLEAR, S_ARRAYS, S_BARRIER_NVN, S_BARRIER_NOP, S_QUERY_BEGIN, S_QUERY_END, S_COND, S_COND_ON, S_ALPHA,
    // drops / unexpected (red)
    D_NO_R, D_STALE_SHADER, D_STALE_BLEND, D_STALE_DS, D_STALE_RASTER, D_STALE_VFMT, D_STALE_VB, D_TABLE_FAULT, D_NO_DEV, D_VB_SLOT,
    D_CLEAR_TEX, D_CLEAR_NOPASS, D_ARRAYS_NODRAW, D_ARRAYS_DROP, D_BARRIER_BAD, D_QUERY_NULL, D_NULL_ARG,
    S_N
};
static const char *const k_why[S_N] = {
    [D_NO_R] = "packet on a list without a renderer (+0x38 = 0): skipped (the original would crash)",
    [D_STALE_SHADER] = "BindShader: a non-zero shader handle names no record (stale generation): cache written, record pointer 0",
    [D_STALE_BLEND] = "ApplyBlend: a blend handle names no record (the original would read a null record's NVN states)",
    [D_STALE_DS] = "depth-stencil handle names no record (the original binds through a null record)",
    [D_STALE_RASTER] = "raster handle names no record (the original binds through a null record)",
    [D_STALE_VFMT] = "vertex format handle names no record (the original binds through a null record)",
    [D_STALE_VB] = "vertex buffer handle names no live record: cache entry set to 0 as the original does",
    [D_TABLE_FAULT] = "a handle table page / generation pointer is null where the original would fault: treated as no record",
    [D_NO_DEV] = "no device / frame counter (r+0xb70 = 0): frame stamp not written",
    [D_VB_SLOT] = "packet 0x0b with a vertex stream slot outside 0..7: skipped",
    [D_CLEAR_TEX] = "packet 0x04: the texture handle names no record (the original would crash): no clear",
    [D_CLEAR_NOPASS] = "packet 0x04: no pass part (dm_vkn_pass_clear_rect) in this build: clear dropped",
    [D_ARRAYS_NODRAW] = "DrawArrays: no draw part (dm_vkn_draw_arrays) in this build: draw dropped",
    [D_ARRAYS_DROP] = "DrawArrays: the draw part dropped the draw",
    [D_BARRIER_BAD] = "packet 0x37 with a state above 12 (the original aborts through 0x80f560): ignored",
    [D_QUERY_NULL] = "query / conditional-render packet with a null query pointer",
    [D_NULL_ARG] = "state function called with a null renderer / argument pointer: skipped (the original would crash)",
};
static uint64_t g_st[S_N], g_op[64], g_bar[13];
static uint32_t g_red[S_N];
static inline void st(int k) { __atomic_add_fetch(&g_st[k], 1, __ATOMIC_RELAXED); }
static void red(int k, uint64_t a, uint64_t b, uint64_t c) {
    st(k);
    if (__atomic_add_fetch(&g_red[k], 1, __ATOMIC_RELAXED) <= 16)
        vkn_logf(EX_RED "exec: %s (values %#llx %#llx %#llx)", k_why[k], (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
}

// ---- engine table lookup, as the machine code does it (0x9283c4..0x928400; same shape for every table; nr_state.h nrst_lookup) ----
// Returns the record or 0 (no generation bits / stale generation). A null page / generation pointer where the machine code would
// fault: 0 and a counted red line.
static uint64_t ex_lookup(uint64_t r, uint32_t tab, uint32_t esz, uint64_t h) {
    if (!(h & 0x3fffff00000000ull)) return 0;
    const uint32_t per = vkn_rd32(r + tab + 0x10), idx = (uint32_t)h;
    const uint32_t page = per ? idx / per : 0, off = idx - page * per;   // udiv by 0 = 0 on AArch64, as the engine computes it
    const uint64_t gp = vkn_rd64(r + tab + 8), gens = gp ? vkn_rd64(gp + 8ull * page) : 0;
    if (!gens) { red(D_TABLE_FAULT, tab, h, 0); return 0; }
    if ((uint32_t)(h >> 32 & 0x3fffff) != vkn_rd32(gens + 4ull * off)) return 0;
    const uint64_t pp = vkn_rd64(r + tab), base = pp ? vkn_rd64(pp + 8ull * page) : 0;
    if (!base) { red(D_TABLE_FAULT, tab, h, 1); return 0; }
    return base + (uint64_t)off * esz;
}
static int ex_frame(uint64_t r, uint64_t *f) {   // [dev+0x51f0], the stamp every binder writes into the records it touches
    const uint64_t dev = vkn_rd64(r + R_DEV);
    if (!dev) { red(D_NO_DEV, r, 0, 0); return 0; }
    *f = vkn_rd64(dev + DEV_FRAME);
    return 1;
}

// ---- the state functions (NVN-free; renderer fields and stamps exactly as the originals) ----
// BindShader 0x9283a0(r, &h): skip when r+0x17d8 == h; else r+0x17d8 = h, r+0x17e0 = record (table r+0xc0, 0x60 B) or 0; a record
// gets the frame stamp at +0; a record without both a vertex (+8) and a fragment (+0x10) program is a compute shader: u32 r+0xb78 = 1
// (only ever set here; the original then binds +0x18 to the compute stage). NVN: up to three BindProgram calls - none here.
void dm_vkn_exec_shader(uint64_t r, uint64_t h) {
    if (vkn_rd64(r + R_SHADER) == h) { st(S_SHADER_SKIP); return; }
    st(S_SHADER);
    w64(r + R_SHADER, h);
    const uint64_t e = ex_lookup(r, T_SHADER, E_SHADER, h);
    w64(r + R_SHADER_REC, e);
    if (!e) { if (h) red(D_STALE_SHADER, h, 0, 0); return; }
    uint64_t f;
    if (ex_frame(r, &f)) w64(e, f);
    if (!vkn_rd64(e + 8) || !vkn_rd64(e + 0x10)) w32(r + R_CSFLAG, 1);
}
// ApplyBlend 0x9284e0(r, &h, mask): skip when r+0x16f8 != 0 && h == r+0x16f8 && u32 r+0x1700 == mask; else r+0x16f8 = h, r+0x1700 =
// mask. h == 0: nothing more. h == [main+0x2bbdb70]: default blend / colour state on the stack (alpha test 5 when r+0x1868 >= 0).
// Else the record (table r+0x600, 0x20 B): the original also rewrites the record's NVN colour states (+0x10 / +0x14: blend enable
// per bound target r+0x17e8 + 8i) - NVN-only memory, nobody else reads it - and binds them. `mask` is only cached.
void dm_vkn_exec_blend(uint64_t r, uint64_t h, uint32_t mask) {
    const uint64_t c = vkn_rd64(r + R_BLEND);
    if (c && h == c && vkn_rd32(r + R_BLEND_MASK) == mask) { st(S_BLEND_SKIP); return; }
    st(S_BLEND);
    w64(r + R_BLEND, h); w32(r + R_BLEND_MASK, mask);
    if (!h) return;
    if (h == vkn_main64(M_BLEND_DEFAULT)) { st(S_BLEND_DEFAULT); return; }
    if (!ex_lookup(r, T_BLEND, E_BLEND, h)) red(D_STALE_BLEND, h, mask, 0);
}
// Depth-stencil (0x93e6bc / 0x93ecf4): skip when r+0x1708 != 0 && h == r+0x1708 && u32 r+0x1710 == ref; else both stored (a zero
// handle too); h != 0: BindDepthStencilState + SetStencilMask / ValueMask (record +0x10) + SetStencilRef(ref) - none here.
void dm_vkn_exec_depth(uint64_t r, uint64_t h, uint32_t ref) {
    const uint64_t c = vkn_rd64(r + R_DS);
    if (c && h == c && vkn_rd32(r + R_REF) == ref) return;
    w64(r + R_DS, h); w32(r + R_REF, ref);
    if (h && !ex_lookup(r, T_DS, E_DS, h)) red(D_STALE_DS, h, ref, 0);
}
// Raster (packet 0x07, 0x93e6e8, 0x93ffc0): skip when r+0x16f0 != 0 && h == r+0x16f0; else stored; h != 0: BindPolygonState.
void dm_vkn_exec_raster(uint64_t r, uint64_t h) {
    const uint64_t c = vkn_rd64(r + R_RASTER);
    if (c && h == c) return;
    w64(r + R_RASTER, h);
    if (h && !ex_lookup(r, T_RASTER, E_RASTER, h)) red(D_STALE_RASTER, h, 0, 0);
}
// Vertex format (packet 0x0a, 0x93e70c, 0x92c130): skip when r+0x1740 == h; else stored; h != 0: BindVertexAttribState(16, rec+0x20)
// + BindVertexStreamState(rec+0x1c ? 3 : rec+0x18 ? 2 : 1, rec+0x60).
void dm_vkn_exec_vfmt(uint64_t r, uint64_t h) {
    if (vkn_rd64(r + R_VFMT) == h) return;
    w64(r + R_VFMT, h);
    if (h && !ex_lookup(r, T_VFMT, E_VFMT, h)) red(D_STALE_VFMT, h, 0, 0);
}
// Vertex buffer of slot s (packet 0x0b with its offset; the full-state forms with offset 0): skip when r+0x1748[s] == h &&
// r+0x1788[s] == off; else a live record (table r+0x180, 0x68 B, record +0 != 0) gets the frame stamp at +0x38 and the cache takes
// (h, off) [NVN: BindVertexBuffer(s, +8 + off, +0x40 - off)]; any other handle stores (0, 0).
void dm_vkn_exec_vb(uint64_t r, int s, uint64_t h, uint64_t off) {
    if (s < 0 || s >= VB_SLOTS) { red(D_VB_SLOT, (uint64_t)(int64_t)s, h, off); return; }
    if (vkn_rd64(r + R_VB + 8ull * s) == h && vkn_rd64(r + R_VB_OFF + 8ull * s) == off) return;
    uint64_t e = ex_lookup(r, T_VB, E_VB, h);
    if (e && !vkn_rd64(e)) e = 0;
    if (e) { uint64_t f; if (ex_frame(r, &f)) w64(e + VB_LAST, f); }
    else { if (h) red(D_STALE_VB, h, (uint64_t)s, 0); h = 0; off = 0; }
    w64(r + R_VB + 8ull * s, h); w64(r + R_VB_OFF + 8ull * s, off);
}
// Index buffer (packet 0x0c; 0x15 / 0x92c3c0 call it only for h != 0): skip when r+0x17c8 == h && r+0x17d0 == 0; else h != 0 stamps
// the record (table r+0x240, 0x58 B, +0x18) WITHOUT a generation check (the original indexes the page directly), r+0x17c8 = h,
// r+0x17d0 = 0. No NVN call in the original either.
void dm_vkn_exec_ib(uint64_t r, uint64_t h) {
    if (vkn_rd64(r + R_IB) == h && !vkn_rd64(r + R_IB_FIRST)) return;
    if (h) {
        const uint32_t per = vkn_rd32(r + T_IB + 0x10), idx = (uint32_t)h;
        const uint32_t page = per ? idx / per : 0, off = idx - page * per;
        const uint64_t pp = vkn_rd64(r + T_IB), base = pp ? vkn_rd64(pp + 8ull * page) : 0;
        uint64_t f;
        if (!base) red(D_TABLE_FAULT, T_IB, h, 2);
        else if (ex_frame(r, &f)) w64(base + (uint64_t)off * E_IB + IB_LAST, f);
    }
    w64(r + R_IB, h); w64(r + R_IB_FIRST, 0);
}
static void ex_prim(uint64_t r, uint8_t p) {   // r+0x1714 engine primitive, r+0x1718 = table main+0x1f58640[p - 1] (0 outside 1..7)
    if (vkn_rd8(r + R_PRIM) == p) return;
    w8(r + R_PRIM, p);
    const uint32_t t = (uint32_t)p - 1u;
    w32(r + R_NVN_PRIM, t > 6 ? 0u : vkn_rd32((uint64_t)(uintptr_t)(dm_vkn_base() + M_PRIM_TAB) + 4ull * t));
}
// ApplyDrawState 0x92c3c0(r, block) = packet 0x15 from +8: +0 shader, +8 depth-stencil, +0x10 u32 stencil ref, +0x18 raster, +0x20
// blend (mask 0xffffffff), +0x28 vertex format, +0x30 / +0x38 / +0x40 VB0..2 (offset 0; 1 and 2 only when non-zero), +0x48 index
// buffer (only when non-zero), +0x50 u8 primitive. Same order as the original.
void dm_vkn_exec_state(uint64_t r, uint64_t b) {
    dm_vkn_exec_shader(r, vkn_rd64(b));
    dm_vkn_exec_depth(r, vkn_rd64(b + 8), vkn_rd32(b + 0x10));
    dm_vkn_exec_raster(r, vkn_rd64(b + 0x18));
    dm_vkn_exec_blend(r, vkn_rd64(b + 0x20), 0xffffffffu);
    dm_vkn_exec_vfmt(r, vkn_rd64(b + 0x28));
    for (int k = 0; k < 3; k++) {
        const uint64_t h = vkn_rd64(b + 0x30 + 8ull * k);
        if (k && !h) continue;
        dm_vkn_exec_vb(r, k, h, 0);
    }
    const uint64_t ib = vkn_rd64(b + 0x48);
    if (ib) dm_vkn_exec_ib(r, ib);
    ex_prim(r, vkn_rd8(b + 0x50));
}
// DrawArrays (packet 0x23 0x93f0c0, 0x940f90's immediate branch): [flushes] ; if r+0x185d && r+0x185c: SetRenderEnable(1),
// r+0x185c = r+0x185d = 0, r+0x1860 = 0 ; DrawArrays(r+0x1718, first, count) ; r+0x185d = r+0x185c. The flushes and the draw are the
// draw part's (dm_vkn_draw_arrays); the field writes are done here and are idempotent if the draw part repeats them.
int dm_vkn_exec_draw_arrays(uint64_t r, uint32_t first, uint32_t count) {
    st(S_ARRAYS);
    if (vkn_rd8(r + R_REN_OFF) && vkn_rd8(r + R_REN_NEED)) { w8(r + R_REN_NEED, 0); w8(r + R_REN_OFF, 0); w64(r + R_REN_Q, 0); }
    int rc = 1;
    if (!dm_vkn_draw_arrays) red(D_ARRAYS_NODRAW, first, count, 0);
    else if ((rc = dm_vkn_draw_arrays(first, count)) != 0) st(D_ARRAYS_DROP);
    w8(r + R_REN_OFF, vkn_rd8(r + R_REN_NEED));
    return rc;
}

// ---- viewport / scissor in effect (see hle_dos2_vkn_exec.h) ----
// Recording-thread state: the sequence numbers of the last viewport and the last scissor change, and the viewport cache as it was at
// the last scissor change.
static uint64_t g_seq, g_vp_seq, g_sc_seq;
static int32_t g_vp_at_sc[4];
void dm_vkn_exec_viewport_note(void) { g_vp_seq = ++g_seq; }
int dm_vkn_exec_scissor(uint64_t r, int32_t out[4]) {
    int32_t vp[4], sc[4];
    memcpy(vp, (const void *)(uintptr_t)(r + R_VP), 16); memcpy(sc, (const void *)(uintptr_t)(r + R_SC), 16);
    if (g_vp_seq > g_sc_seq || memcmp(vp, g_vp_at_sc, 16)) { out[0] = vp[2]; out[1] = vp[3]; out[2] = vp[0]; out[3] = vp[1]; return 1; }
    out[0] = sc[0]; out[1] = sc[1]; out[2] = sc[2] - sc[0]; out[3] = sc[3] - sc[1];
    return 0;
}

// ---- the taken opcodes ----
static const uint64_t k_ex_ops = 1ull << 0x04 | 1ull << 0x07 | 1ull << 0x08 | 1ull << 0x0a | 1ull << 0x0b | 1ull << 0x12 | 1ull << 0x13 |
                                 1ull << 0x15 | 1ull << 0x23 | 1ull << 0x37 | 1ull << 0x3d | 1ull << 0x3e | 1ull << 0x3f;
static uint64_t ex_len(unsigned op) {   // the lengths of the taken opcodes (fixed for all of them)
    switch (op) {
    case 0x04: return 0x40; case 0x07: return 0x10; case 0x08: return 0x18; case 0x0a: return 0x10; case 0x0b: return 0x18;
    case 0x12: case 0x13: return 0x14; case 0x15: return 0x70; case 0x23: return 0xc; case 0x37: return 0x20;
    default: return 0x10;   // 0x3d 0x3e 0x3f
    }
}
// The two taken opcodes that reach Vulkan (the region clear records a clear / opens a pass, the draw may compile a pipeline) run on
// the host stack: the ExecuteRenderCommand hook (hle_dos2_native.c) calls ex_take on the guest stack (hle_dos2_vkn_stack.c).
typedef struct { uint64_t tex; uint32_t layer; float rgba[4]; int32_t x, y, w, h; } ex_clear_job;
static uint64_t ex_clear_on_stack(void *p) { const ex_clear_job *j = p; dm_vkn_pass_clear_rect(j->tex, j->layer, j->rgba, j->x, j->y, j->w, j->h); return 0; }
typedef struct { uint64_t r; uint32_t first, count; } ex_arrays_job;
static uint64_t ex_arrays_on_stack(void *p) { const ex_arrays_job *j = p; return (uint64_t)(uint32_t)dm_vkn_exec_draw_arrays(j->r, j->first, j->count); }
typedef struct { uint64_t q; int begin; } ex_query_job;
static uint64_t ex_query_on_stack(void *p) { const ex_query_job *j = p; if (j->begin) vkn_query_begin(j->q); else vkn_query_end(j->q); return 0; }
static int ex_take(uint64_t list, unsigned op, uint64_t pkt, uint64_t *len) {
    *len = ex_len(op);
    __atomic_add_fetch(&g_op[op & 63], 1, __ATOMIC_RELAXED);
    const uint64_t r = vkn_rd64(list + L_R);
    if (!r) { red(D_NO_R, list, op, pkt); return 1; }
    switch (op) {
    case 0x04: {   // region clear: texture +8, layer +0x10 (-1 -> 0), rgba +0x20, w +0x30, h +0x34, x +0x38, y +0x3c (0x93e6c0)
        const uint64_t tex = vkn_rd64(pkt + 8);
        int32_t layer = (int32_t)vkn_rd32(pkt + 0x10);
        if (layer == -1) layer = 0;
        float rgba[4]; memcpy(rgba, (const void *)(uintptr_t)(pkt + 0x20), 16);
        const int32_t w = (int32_t)vkn_rd32(pkt + 0x30), h = (int32_t)vkn_rd32(pkt + 0x34), x = (int32_t)vkn_rd32(pkt + 0x38), y = (int32_t)vkn_rd32(pkt + 0x3c);
        if (!vkn_rec(r, T_TEX, E_TEX, tex)) { red(D_CLEAR_TEX, tex, (uint64_t)(uint32_t)layer, (uint64_t)(uint32_t)w << 32 | (uint32_t)h); return 1; }
        if (!dm_vkn_pass_clear_rect) { red(D_CLEAR_NOPASS, tex, 0, 0); return 1; }
        st(S_CLEAR);
        ex_clear_job j = {tex, (uint32_t)layer, {rgba[0], rgba[1], rgba[2], rgba[3]}, x, y, w, h};
        dm_vkn_on_stack(ex_clear_on_stack, &j);
        return 1;
    }
    case 0x07: dm_vkn_exec_raster(r, vkn_rd64(pkt + 8)); return 1;
    case 0x08: dm_vkn_exec_depth(r, vkn_rd64(pkt + 8), vkn_rd32(pkt + 0x10)); return 1;
    case 0x0a: dm_vkn_exec_vfmt(r, vkn_rd64(pkt + 8)); return 1;
    case 0x0b: dm_vkn_exec_vb(r, (int32_t)vkn_rd32(pkt + 4), vkn_rd64(pkt + 8), vkn_rd64(pkt + 0x10)); return 1;
    case 0x12: {   // viewport: w +4, h +8, x +0xc, y +0x10 -> r+0x171c.. {w, h, x, y} unless equal; NVN SetViewport + SetScissor(x, y, w, h)
        int32_t v[4]; memcpy(v, (const void *)(uintptr_t)(pkt + 4), 16);
        if (!memcmp(v, (const void *)(uintptr_t)(r + R_VP), 16)) return 1;
        memcpy((void *)(uintptr_t)(r + R_VP), v, 16);
        st(S_VIEWPORT); dm_vkn_exec_viewport_note();
        return 1;
    }
    case 0x13: {   // scissor: left +4, top +8, right +0xc, bottom +0x10 -> r+0x172c.. unless equal; NVN SetScissor(l, t, r - l, b - t)
        int32_t s[4]; memcpy(s, (const void *)(uintptr_t)(pkt + 4), 16);
        if (!memcmp(s, (const void *)(uintptr_t)(r + R_SC), 16)) return 1;
        memcpy((void *)(uintptr_t)(r + R_SC), s, 16);
        st(S_SCISSOR); g_sc_seq = ++g_seq; memcpy(g_vp_at_sc, (const void *)(uintptr_t)(r + R_VP), 16);
        return 1;
    }
    case 0x15: {   // full draw state (0x93e6ac): the block from +8, then conditional render +0x60, alpha reference +0x68
        dm_vkn_exec_state(r, pkt + 8);
        const uint64_t q = vkn_rd64(pkt + 0x60); const uint8_t need = q != 0;
        // NVN: SetRenderEnable(1) for q == 0, else SetRenderEnableConditional(1, *q). Byte compare as the machine code (nr_state.h).
        if (vkn_rd8(r + R_REN_NEED) != need || vkn_rd64(r + R_REN_Q) != q) { w8(r + R_REN_NEED, need); w64(r + R_REN_Q, q); st(S_COND); if (need) st(S_COND_ON); }
        w8(r + R_REN_OFF, 0);
        const float a = rdf(pkt + 0x68);
        if (!(rdf(r + R_ALPHA) == a)) {   // NaN never equal, as the machine code's fcmp
            // NVN: SetAlphaRef(a) when a >= 0; then the blend is applied again: saved = r+0x16f8, r+0x1868 = a, r+0x16f8 = 0,
            // ApplyBlend(r, &saved, r+0x1700) (the colour state's alpha test follows r+0x1868).
            const uint64_t saved = vkn_rd64(r + R_BLEND);
            wf(r + R_ALPHA, a); w64(r + R_BLEND, 0);
            dm_vkn_exec_blend(r, saved, vkn_rd32(r + R_BLEND_MASK));
            st(S_ALPHA);
        }
        return 1;
    }
    case 0x23: { ex_arrays_job j = {r, vkn_rd32(pkt + 4), vkn_rd32(pkt + 8)}; dm_vkn_on_stack(ex_arrays_on_stack, &j); return 1; }
    case 0x37: {   // resource state u8 +1: 0 / 1 -> nvnCommandBufferBarrier(cb, 2); 2..12 -> nothing; else the engine aborts (0x80f560)
        const uint8_t s = vkn_rd8(pkt + 1);
        if (s <= 12) { __atomic_add_fetch(&g_bar[s], 1, __ATOMIC_RELAXED); st(s <= 1 ? S_BARRIER_NVN : S_BARRIER_NOP); }
        else red(D_BARRIER_BAD, s, vkn_rd64(pkt + 8), 0);
        return 1;   // the pass part tracks layouts itself; a full barrier is not needed between Vulkan passes of one queue here
    }
    case 0x3d: case 0x3e: {   // ReportCounter(cb, 1, **[+8] (+0x10 for the end)): the pair's begin / end -> vkn_query (key = q = [+8])
        const uint64_t q = vkn_rd64(pkt + 8);
        if (!q) { red(D_QUERY_NULL, op, pkt, 0); return 1; }
        st(op == 0x3d ? S_QUERY_BEGIN : S_QUERY_END);
        ex_query_job j = {q, op == 0x3d};
        if (vkn_query_mode()) dm_vkn_on_stack(ex_query_on_stack, &j);   // (may end a Vulkan query)
        else ex_query_on_stack(&j);                                     // (mode 0: counted only, no Vulkan)
        return 1;
    }
    case 0x3f: {   // conditional render: u8 +1 on, query pointer +8 -> r+0x185c / r+0x1860 when either differs; r+0x185d = 0 always
        const uint8_t on = vkn_rd8(pkt + 1); const uint64_t q = vkn_rd64(pkt + 8);
        if (vkn_rd8(r + R_REN_NEED) != on || vkn_rd64(r + R_REN_Q) != q) {
            if (on && !q) red(D_QUERY_NULL, op, pkt, 0);   // the original would read *q
            w8(r + R_REN_NEED, on); w64(r + R_REN_Q, q); st(S_COND); if (on) st(S_COND_ON);
        }
        w8(r + R_REN_OFF, 0);
        return 1;
    }
    }
    return 0;   // not reached: the mask holds only the cases above
}

// ---- the state function hooks (mode 1: the originals are never called, except for their NVN-free packet branches) ----
static vkn_fn g_o_shader, g_o_blend, g_o_state, g_o_fmtvb, g_o_raster, g_o_arrays;
VKN_HOOK ex_h_shader(VKN_A) {   // 0x9283a0 BindShader(r, &shader)
    VKN_UNUSED;
    if (!a0 || !a1) { red(D_NULL_ARG, 0x9283a0, a0, a1); return 0; }
    dm_vkn_exec_shader(a0, vkn_rd64(a1));
    return 0;
}
VKN_HOOK ex_h_blend(VKN_A) {   // 0x9284e0 ApplyBlend(r, &blend, mask)
    VKN_UNUSED;
    if (!a0 || !a1) { red(D_NULL_ARG, 0x9284e0, a0, a1); return 0; }
    dm_vkn_exec_blend(a0, vkn_rd64(a1), (uint32_t)a2);
    return 0;
}
VKN_HOOK ex_h_state(VKN_A) {   // 0x92c3c0 ApplyDrawState(r, block) (x1 = block: 0x92c3d4 mov x20, x1 before bl 0x9283a0)
    VKN_UNUSED;
    if (!a0 || !a1) { red(D_NULL_ARG, 0x92c3c0, a0, a1); return 0; }
    st(S_STATE);
    dm_vkn_exec_state(a0, a1);
    return 0;
}
VKN_HOOK ex_h_fmtvb(VKN_A) {   // 0x92c130(block, r): NOTE the order (0x92c140 ldr x9, [x1, #0x1740]); block +0 format, +8 VB0
    VKN_UNUSED;
    if (!a0 || !a1) { red(D_NULL_ARG, 0x92c130, a0, a1); return 0; }
    st(S_FMTVB);
    dm_vkn_exec_vfmt(a1, vkn_rd64(a0));
    dm_vkn_exec_vb(a1, 0, vkn_rd64(a0 + 8), 0);
    return 0;
}
VKN_HOOK ex_h_raster(VKN_A) {   // 0x93ffc0 SetRasterState(list, u8 index < 10): handle [main+0x2bbdc18 + 8 index]
    const unsigned idx = (unsigned)(a1 & 0xff);
    if (idx > 9) return 0;
    if (!a0) { red(D_NULL_ARG, 0x93ffc0, a0, a1); return 0; }
    if (vkn_rd8(a0 + L_RECORD)) { st(S_RASTER_LIST); return g_o_raster(VKN_F); }   // the packet branch (AllocCommand + opcode 7): NVN-free
    const uint64_t r = vkn_rd64(a0 + L_R);
    if (!r) { red(D_NO_R, a0, 7, 0); return 0; }
    st(S_RASTER_IMM);
    dm_vkn_exec_raster(r, vkn_main64(M_RASTER_TAB + 8u * idx));
    return 0;
}
VKN_HOOK ex_h_arrays(VKN_A) {   // 0x940f90 DrawArrays(list, first, count)
    if (!a0) { red(D_NULL_ARG, 0x940f90, a0, a1); return 0; }
    if (vkn_rd8(a0 + L_RECORD)) { st(S_ARRAYS_LIST); return g_o_arrays(VKN_F); }   // the packet branch (opcode 0x23): NVN-free
    const uint64_t r = vkn_rd64(a0 + L_R);
    if (!r) { red(D_NO_R, a0, 0x23, 0); return 0; }
    st(S_ARRAYS_IMM);
    dm_vkn_exec_draw_arrays(r, (uint32_t)a1, (uint32_t)a2);
    return 0;
}

// ---- stage 4: queries census (vkn_query.h) ----
static vkn_query_stats g_qprev; static uint64_t g_qprev_frame;
static void q_census(void) {
    vkn_query_stats q; vkn_query_stats_get(&q);
    const uint64_t f = vkn_frame(), df = f - g_qprev_frame ? f - g_qprev_frame : 1;
#define QU(x) (unsigned long long)q.x
#define QF(x) (double)(q.x - g_qprev.x) / (double)df
    vkn_logf("query: DM_VK_NATIVE_COND=%d (GPU predicates %s); a frame: pairs %.1f, queries %.1f, conditional draws %.1f -> GPU-predicated %.1f, CPU-skipped %.1f, "
             "counted %.1f, drawn %.1f; GPU verdicts culled %.1f / drawn %.1f; CPU %.3f ms (%llu frames)",
             vkn_query_mode(), vkn_query_gpu() ? "on" : "off", QF(begins), QF(queries), QF(conds), QF(cond_gpu), QF(cond_cpu_skip), QF(cond_count),
             QF(conds) - QF(cond_gpu) - QF(cond_cpu_skip) - QF(cond_count), QF(gpu_culled), QF(gpu_drawn), QF(cpu_ns) / 1e6, (unsigned long long)df);
    vkn_logf("query: pairs begun %llu / ended %llu -> queries %llu, empty %llu, copied %llu in %llu copies; unusable: pass ended inside %llu, lost draws %llu, "
             "nested %llu, open at the recording end %llu, table / pool full %llu, no slot %llu; unmatched ends %llu; conditional draws %llu: GPU %llu, "
             "CPU-skipped %llu, counted %llu (empty %llu); drawn: no pair %llu, pair's pass still open %llu, inside its pair %llu, unusable pair %llu, "
             "no extension %llu, no slot %llu, poison-empty %llu; dispatches with a condition %llu; verdicts read %llu (culled %llu, drawn %llu, lost %llu); "
             "slots taken %llu, busy %llu; setup failures %llu",
             QU(begins), QU(ends), QU(queries), QU(empty), QU(copied), QU(copies), QU(broken), QU(lost), QU(nested), QU(open_at_close), QU(pool_full),
             QU(no_slot), QU(unmatched), QU(conds), QU(cond_gpu), QU(cond_cpu_skip), QU(cond_count), QU(count_empty), QU(drawn_no_query),
             QU(drawn_same_pass), QU(drawn_open), QU(drawn_unusable), QU(drawn_no_ext), QU(drawn_no_slot), QU(drawn_poison_empty), QU(cond_dispatch),
             QU(results_read), QU(gpu_culled), QU(gpu_drawn), QU(results_lost), QU(slots_taken), QU(slots_busy), QU(setup_fail));
#undef QU
#undef QF
    g_qprev = q; g_qprev_frame = f;
    vkn_query_census_occ();   // [stage 5.1 O1 / O2] object census (COND 2 / 4) and DM_VK_NATIVE_GBUF_LE swaps / refusals
}

// ---- census ----
static uint64_t ld(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static void ex_census(void) {
    uint64_t n[64], tot = 0; int top[6] = {-1, -1, -1, -1, -1, -1};
    for (int i = 0; i < 64; i++) { n[i] = ld(&g_op[i]); tot += n[i]; }
    for (int k = 0; k < 6; k++) for (int i = 0; i < 64; i++) {
        int used = 0; for (int j = 0; j < k; j++) used |= top[j] == i;
        if (!used && n[i] && (top[k] < 0 || n[i] > n[top[k]])) top[k] = i;
    }
    char tops[160]; int t = 0; tops[0] = 0;
    for (int k = 0; k < 6 && top[k] >= 0; k++) t += snprintf(tops + t, sizeof tops - (size_t)t, "%s%#x:%llu", k ? " " : "", top[k], (unsigned long long)n[top[k]]);
    uint64_t b212 = 0; for (int s = 2; s <= 12; s++) b212 += ld(&g_bar[s]);
    uint64_t drops = 0; for (int i = D_NO_R; i < S_N; i++) drops += ld(&g_st[i]);
    vkn_logf("exec: %llu packets taken (%s); state fns: shader %llu (+%llu skipped), blend %llu (+%llu skipped, %llu default), draw state %llu, "
             "format+VB0 %llu, raster now %llu / to list %llu, DrawArrays now %llu / to list %llu; draws 0x23 %llu, viewport %llu, scissor %llu, "
             "region clears %llu, alpha %llu; barriers state0 %llu state1 %llu states2-12 %llu; queries begin %llu end %llu, cond render %llu (%llu on); "
             "dropped / unexpected %llu",
             (unsigned long long)tot, t ? tops : "none", (unsigned long long)ld(&g_st[S_SHADER]), (unsigned long long)ld(&g_st[S_SHADER_SKIP]),
             (unsigned long long)ld(&g_st[S_BLEND]), (unsigned long long)ld(&g_st[S_BLEND_SKIP]), (unsigned long long)ld(&g_st[S_BLEND_DEFAULT]),
             (unsigned long long)ld(&g_st[S_STATE]), (unsigned long long)ld(&g_st[S_FMTVB]), (unsigned long long)ld(&g_st[S_RASTER_IMM]),
             (unsigned long long)ld(&g_st[S_RASTER_LIST]), (unsigned long long)ld(&g_st[S_ARRAYS_IMM]), (unsigned long long)ld(&g_st[S_ARRAYS_LIST]),
             (unsigned long long)ld(&g_st[S_ARRAYS]), (unsigned long long)ld(&g_st[S_VIEWPORT]), (unsigned long long)ld(&g_st[S_SCISSOR]),
             (unsigned long long)ld(&g_st[S_CLEAR]), (unsigned long long)ld(&g_st[S_ALPHA]), (unsigned long long)ld(&g_bar[0]),
             (unsigned long long)ld(&g_bar[1]), (unsigned long long)b212, (unsigned long long)ld(&g_st[S_QUERY_BEGIN]),
             (unsigned long long)ld(&g_st[S_QUERY_END]), (unsigned long long)ld(&g_st[S_COND]), (unsigned long long)ld(&g_st[S_COND_ON]),
             (unsigned long long)drops);
    q_census();
    if (!drops) return;
    char line[700]; int k = 0; line[0] = 0;
    static const char *const nm[S_N] = {[D_NO_R] = "no-renderer", [D_STALE_SHADER] = "stale-shader", [D_STALE_BLEND] = "stale-blend",
        [D_STALE_DS] = "stale-ds", [D_STALE_RASTER] = "stale-raster", [D_STALE_VFMT] = "stale-vfmt", [D_STALE_VB] = "stale-vb",
        [D_TABLE_FAULT] = "table-fault", [D_NO_DEV] = "no-dev", [D_VB_SLOT] = "vb-slot", [D_CLEAR_TEX] = "clear-tex", [D_CLEAR_NOPASS] = "clear-nopass",
        [D_ARRAYS_NODRAW] = "arrays-nodraw", [D_ARRAYS_DROP] = "arrays-dropped", [D_BARRIER_BAD] = "barrier-state", [D_QUERY_NULL] = "query-null",
        [D_NULL_ARG] = "null-arg"};
    for (int i = D_NO_R; i < S_N && k < (int)sizeof line - 48; i++) if (ld(&g_st[i])) k += snprintf(line + k, sizeof line - (size_t)k, " %s %llu", nm[i], (unsigned long long)ld(&g_st[i]));
    vkn_logf(EX_RED "exec: dropped / unexpected:%s", line);
}

// ---- registration ----
static void stage5_line(void);   // [stage 5.1 A5] (end of file)
void dm_vkn_exec_register(void);
void dm_vkn_exec_register(void) {
    if (vkn_mode() != VKN_NATIVE) return;   // stage 3 is mode 1 only; mode 2 keeps the engine's executor and functions untouched
    // First 4 instructions of each site (python <scratchpad>/vk/re/pro.py; none PC-relative, checked with capstone).
    static const uint32_t k_shader[4] = {0xf81d0ff5 /* str x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    static const uint32_t k_blend[4] = {0xd10183ff /* sub sp,sp,#0x60 */, 0xf9000bf9, 0xa9025ff8, 0xa90357f6};
    static const uint32_t k_state[4] = {0xf81c0ff7 /* str x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    static const uint32_t k_fmtvb[4] = {0xf81d0ff5 /* str x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    static const uint32_t k_raster[4] = {0xf81e0ff3 /* str x19,[sp,#-0x20]! */, 0xa9017bfd, 0x910043fd, 0x12001c28 /* and w8,w1,#0xff */};
    static const uint32_t k_arrays[4] = {0xf81c0ff7 /* str x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    dm_vkn_site(0x9283a0, k_shader, "BindShader", (void *)ex_h_shader, &g_o_shader, VKN_M_NATIVE);
    dm_vkn_site(0x9284e0, k_blend, "ApplyBlend", (void *)ex_h_blend, &g_o_blend, VKN_M_NATIVE);
    dm_vkn_site(0x92c3c0, k_state, "ApplyDrawState", (void *)ex_h_state, &g_o_state, VKN_M_NATIVE);
    dm_vkn_site(0x92c130, k_fmtvb, "ApplyFormatVB0", (void *)ex_h_fmtvb, &g_o_fmtvb, VKN_M_NATIVE);
    dm_vkn_site(0x93ffc0, k_raster, "SetRasterState", (void *)ex_h_raster, &g_o_raster, VKN_M_NATIVE);
    dm_vkn_site(0x940f90, k_arrays, "ListDrawArrays", (void *)ex_h_arrays, &g_o_arrays, VKN_M_NATIVE);
    // The executor's hook: dn_install_rc_take installs the ExecuteRenderCommand jump itself when no old layer did (with DM_VK_NATIVE=1
    // dm_dos2_vkn_pre turned every one of them off, so this is the only native opcode handler).
    if (dn_install_rc_take(k_ex_ops, ex_take))
        vkn_logf(EX_RED "exec: no ExecuteRenderCommand opcode handler (see the [native] line above): opcodes with inline NVN run the engine's code");
    else vkn_logf("exec: ExecuteRenderCommand opcodes 0x04 0x07 0x08 0x0a 0x0b 0x12 0x13 0x15 0x23 0x37 0x3d 0x3e 0x3f taken; BindShader, ApplyBlend, "
                  "ApplyDrawState, 0x92c130, SetRasterState / ListDrawArrays immediate branches replaced (cache fields only)");
    dm_vkn_census(ex_census);
    dm_vkn_census(stage5_line);   // [stage 5.1 A5] the stage-5 switches in effect, once (end of file)
    // Stage 4: conditional rendering and visibility queries (vkn_query.h). DM_VK_NATIVE_COND 0 (default) = everything drawn, the
    // pairs and conditions only counted (the shipping picture); 1 = on; 2 = count what the GPU would skip; 3 = poison (inverted).
    const char *qc = getenv("DM_VK_NATIVE_COND");
    vkn_query_init(qc && *qc ? atoi(qc) : VKN_COND_OFF);   // (its census lines come with this part's: q_census from ex_census)
}

// ---- mode 1 exclusivity ----
// With DM_VK_NATIVE=1 the old layers must neither hook nor record: their hooks sit on the same engine functions (ExecuteRenderCommand,
// the draw / flush / state functions, BeginFrame / EndFrame, the buffer / texture creates) and assume the NVN stream; their records
// and the translator features that read them expect NVN calls that mode 1 no longer makes. Forced to 0 (setenv overwrite) before any
// installer of the DOS2 block runs and before the Vulkan start (jni_android.c host_thread); app defaults (k_nb_on) and a player's
// dm/env lose to it.
// [stage 5.1 A5] Two kinds of entry since stage 5 (checked 21.11 by the readers of each name, dm/ without tests):
//   H  a host layer (dm/host): its installer hooks engine functions the backend owns - matters in every mode-1 run;
//   T  read by the translator only (dm/gfx/vulkan/engine): in mode 1 the translator is NOT initialised (DM_VK_NATIVE_SHLIB, default 1:
//      the backend's own shader library), so these matter only for the DM_VK_NATIVE_SHLIB=0 fallback, where the translator is
//      initialised library-only and its init / shader loader would read them (phone run V3_f: DM_NR_SH=1 from the family default broke
//      the post chain). Stage 5 ports techniques under NEW names DM_VK_NATIVE_<TECH> (the backend reads only those; their values are
//      printed once by stage5_line below), so the old names stay forced off: with SHLIB=0 the translator would otherwise apply its
//      own version on top of the backend's (a named bundle read twice, a Dref module without the compare sampler, ...).
// Each entry: kind, what it would do in mode 1.
static const struct { const char *name; char kind; } k_pre_off[] = {   // kind: 'H' host, 'T' translator, 'B' both
    {"DM_NATIVE_RC", 'H'},             // H hle_dos2_native.c: ExecuteRenderCommand census hook
    {"DM_NATIVE_DRAW", 'H'},           // H hle_dos2_native.c: draws as one NVN record
    {"DM_GFX_PASS_GRAPH", 'B'},        // H+T pass-graph marks written into the NVN command buffer / their reader
    {"DM_NB", 'H'},                    // H hle_dos2_nb.c: the NB front end (ExecuteCommandList, BeginFrame, EndFrame, the draw functions)
    {"DM_NB_OPS", 'H'},                // H NB-13 state records
    {"DM_NB_OPS_CHECK", 'H'},          // H their check
    {"DM_NB_OWN", 'B'},                // H+T NB own-pass records / the translator's executor of them
    {"DM_NB_FOREIGN", 'H'},            // H foreign-command records
    {"DM_NB_ORDER", 'B'},              // H+T record ordering (also read by the NR direct / NRN layers)
    {"DM_NB_RELOC", 'B'},              // H+T record relocation
    {"DM_NB_DUMP", 'H'},               // H record dumps
    {"DM_NB_MX", 'B'},                 // H+T NB multi-execution
    {"DM_NB_POST", 'H'},               // H hle_dos2_nb_post.c: NB-18 post-process replacement (hooks the post chain)
    {"DM_NB_POST_AA", 'H'},            // H its AA variant
    {"DM_NB_POST_AA_OUTLINE", 'H'},    // H its AA + character outline variant (app default 1)
    {"DM_NR_LEAN", 'H'},               // H the NB lean call path
    {"DM_NR_TAPSET", 'H'},             // H NR-TAPSET hash set of the NB layer (app default 1)
    {"DM_NR", 'H'},                    // H hle_dos2_nr.c: the engine object registry (texture / buffer / pipe events)
    {"DM_NR_TEX", 'H'},                // H NR-05 texture events (implies DM_NR; app default 1)
    {"DM_NR_TEXSTREAM", 'H'},          // H NR-22 texture stream worker (implies DM_NR; app default 1)
    {"DM_NR_BUF", 'B'},                // H+T NR-06 buffers
    {"DM_NR_PIPE", 'B'},               // H+T NR-07 pipe hints
    {"DM_NR_DUMP", 'H'},               // H registry dumps
    {"DM_NR_KEY", 'H'},                // H NR-02 draw keys (hooks the texture flush 0x924730)
    {"DM_NR_DRAW", 'B'},               // H+T NR-09 direct draws
    {"DM_NR_DIRECT", 'B'},             // H+T NR-09R direct records
    {"DM_NR_NOCALL", 'H'},             // H NR-23 no-call path
    {"DM_NR_STATE", 'H'},              // H NR-09S state records (app default 4)
    {"DM_NR_UBO", 'H'},                // H NR-09U uniform allocator (app default 1)
    {"DM_NR_UBD", 'B'},                // H+T NR-UBD repeated uniform binds (app default 1)
    {"DM_NR_Z5", 'B'},                 // H+T NR-ZERO Z5 packets from the game thread
    {"DM_NR_Z8", 'B'},                 // H+T NR-ZERO Z8 submit walk
    {"DM_NRN", 'H'},                   // H NR-NATIVE pass cards, all passes (nr_nrn.h)
    {"DM_NRN_EV", 'B'},                // H+T NR-NATIVE events
    {"DM_NRN_LIST", 'H'},              // H NR-NATIVE list rewriting
    {"DM_NRN_REC", 'H'},               // H NR-NATIVE records
    {"DM_NRN_PKT", 'H'},               // H NR-NATIVE packet models
    {"DM_NRN_NOBODY", 'H'},            // H NR-NATIVE no-body binds
    {"DM_NRN_SHORT", 'H'},             // H NR-NATIVE short path
    {"DM_NRN_LIVE", 'T'},              // T NR-NATIVE live (xbg_nr_z3e.inc)
    {"DM_NRN_SHC", 'T'},               // T NR-NATIVE shadow cache (xbg_nr_shc.inc)
    {"DM_NRN_SHADOW", 'H'},            // H the per-pass NR-NATIVE modes (nr_nrn.h composes "DM_NRN_<PASS>")
    {"DM_NRN_PREPASS", 'H'},           // H "
    {"DM_NRN_GBUF", 'H'},              // H "
    {"DM_NRN_LIGHT", 'H'},             // H "
    {"DM_NRN_POST", 'H'},              // H "
    {"DM_NRN_OTHER", 'H'},             // H "
    {"DM_NB_RP", 'T'},                 // T NB-06 render-pass planner (app default 1)
    {"DM_NB_OPBARRIER", 'T'},          // T NB narrow copy barriers (app default 1)
    {"DM_NB_PIPE", 'T'},               // T NB pipeline cache
    {"DM_NB_DESC", 'T'},               // T NB descriptor cache
    {"DM_NB_UBO", 'T'},                // T NB uniform hits
    {"DM_NB_UIAREA", 'T'},             // T NB UI render area
    {"DM_NB_MT", 'T'},                 // T NB-20b recording worker
    {"DM_NR_DESC", 'T'},               // T NR descriptors / seed policy
    {"DM_NR_HEAT", 'B'},               // H+T pass heat measurement (replaced by DM_VK_NATIVE_PROF)
    {"DM_NR_SH_SURF", 'T'},            // T NR-33C surface bundles (app default 1)
    {"DM_NR_MAT", 'T'},                // T NR-33B empty depth-pass FS (app default 1)
    {"DM_NR_TRX_STICKY", 'T'},         // T translator sticky pages (app default 1)
    {"DM_NR_TRX_ZC", 'T'},             // T translator zero-copy chunk index (app default 1)
    {"DM_NR_SEED", 'T'},               // T NR-31 portable warm-up seed (the backend has vkn_warm)
    {"DM_NR_SH", 'T'},                 // T NR-04 bundles by name (backend: DM_VK_NATIVE_SH)
    {"DM_NR_Z3", 'T'},                 // T NR-ZERO Z3 answer tables
    {"DM_NR_SH_PCF", 'T'},             // T NR-04 shadow class / Dref (backend: DM_VK_NATIVE_SH_PCF)
    {"DM_NR_LIGHTFUSE", 'T'},          // T fused Sky -> Light (backend: DM_VK_NATIVE_LIGHTFUSE, stage 5.1 A4)
    {"DM_NR_LIGHTFUSE_FOG", 'T'},      // T + Fog (backend: DM_VK_NATIVE_LIGHTFUSE_FOG)
    {"DM_NR_ONCHIP", 'T'},             // T NR-ONCHIP Sky -> Light subpasses (not ported: owner 20.11)
    {"DM_NR_ONCHIP_FOG", 'T'},         // T its fog variant
    {"DM_NR_MAT_VS", 'T'},             // T NR-33B trimmed vertex outputs
    {"DM_NR_XLQ", 'T'},                // T NR-32 output trim
    {"DM_HACK_FSR", 'T'},              // T EASU module swap of the stretch pass (stage 5.4 ports it to the backend; until then off in mode 1)
};
// Kept on purpose: DM_NR_MEM (RAM budget, no hook; with NR off it plans only the encoder and warm-up caches), DM_GFX_PRESENT_THREAD
// (the presenter the frame part hands images to), DM_DOS2_* (screen shape, quality / DRS, touch UI: not NVN-stream features), the
// translator's own DM_GFX_* policies (they act on the NVN stream only, which mode 1 leaves empty), DM_HACK_* other than FSR (read by
// the backend's pieces through nr_hacks.h: DM_HACK_SHFAR / _SHPCF / _TIER), and every DM_VK_NATIVE_* switch.
void dm_dos2_vkn_pre(void) {
    const char *e = getenv("DM_VK_NATIVE");
    if (!e || atoi(e) != VKN_NATIVE) return;
    int was_on = 0, was_t = 0;
    const int n = (int)(sizeof k_pre_off / sizeof k_pre_off[0]);
    const char *sl = getenv("DM_VK_NATIVE_SHLIB"); const int translator = sl && *sl && !atoi(sl);   // the SHLIB=0 fallback (vkn_shlib.cpp)
    for (int i = 0; i < n; i++) {
        const char *v = getenv(k_pre_off[i].name);
        if (v && *v && strcmp(v, "0")) { was_on++; was_t += k_pre_off[i].kind == 'T'; }
        setenv(k_pre_off[i].name, "0", 1);
    }
    vkn_logf("DM_VK_NATIVE=1: %d switches of the old native / NB / NR / NRN layers forced to 0 (%d of them were on, %d of those translator-only: %s): no old hook, no old record",
             n, was_on, was_t, translator ? "DM_VK_NATIVE_SHLIB=0, the translator is initialised library-only and would read them" : "the translator is not initialised");
}

// [stage 5.1 A5] One line with the value in effect of every stage-5 switch of the backend, read the way each module reads it (its getter,
// or its getenv with its default), printed once, at the first census of this part (every module has installed and the shader library
// has read its first bundle by then). A module that is not in this build (weak getter absent) prints "n/a".
#include "../gfx/vkn/vkn_draw.h"
#include "../gfx/vkn/vkn_prof.h"
#include "../gfx/vkn/vkn_shlib_named.h"
#include "../gfx/nr/nr_hacks.h"
int vkn_lightfuse_mode(void) __attribute__((weak));      // stage 5.1 A4 (vkn_lightfuse.c): DM_VK_NATIVE_LIGHTFUSE in effect
int vkn_lightfuse_fog(void) __attribute__((weak));       // DM_VK_NATIVE_LIGHTFUSE_FOG in effect
int vkn_shc_active(void) __attribute__((weak));          // vkn_shc.c
void vkn_shlib_named_stats_get(vkn_shlib_named_stats *s) __attribute__((weak));
static void stage5_line(void) {
    static int done; if (done) return; done = 1;
    char lf[16], lff[16], sh[16], pcf[16];
    if (vkn_lightfuse_mode) snprintf(lf, sizeof lf, "%d", vkn_lightfuse_mode()); else snprintf(lf, sizeof lf, "n/a");
    if (vkn_lightfuse_fog) snprintf(lff, sizeof lff, "%d", vkn_lightfuse_fog()); else snprintf(lff, sizeof lff, "n/a");
    vkn_shlib_named_stats ns; memset(&ns, 0, sizeof ns); ns.mode = ns.pcf = -1;
    if (vkn_shlib_named_stats_get) vkn_shlib_named_stats_get(&ns);
    if (ns.mode >= 0) { snprintf(sh, sizeof sh, "%d", ns.mode); snprintf(pcf, sizeof pcf, "%d", ns.pcf); }
    else { snprintf(sh, sizeof sh, "%s", vkn_shlib_named_stats_get ? "not read" : "n/a"); snprintf(pcf, sizeof pcf, "%s", sh); }
    const char *c = getenv("DM_VK_NATIVE_COST");   // hle_dos2_vkn_draw.c
    const uint32_t cost = c && *c ? (uint32_t)strtoul(c, NULL, 0) & VKN_COST_ALL : VKN_COST_ALL;
    const char *s = getenv("DM_VK_NATIVE_SHCACHE");   // vkn_shc_install
    int shc = s && *s ? atoi(s) : 1; if (shc < 0 || shc > 3) shc = 0;
    const char *f = getenv("DM_VK_NATIVE_SHFAR"); char from = 0;
    int far = f && *f ? atoi(f) : nrh_value(NRH_SHFAR, &from); if (far < 0 || far > 2) far = 0;
    vkn_logf("stage 5 switches in effect: DM_VK_NATIVE_COST=%#x DM_VK_NATIVE_SH=%s DM_VK_NATIVE_SH_PCF=%s DM_VK_NATIVE_SHCACHE=%d DM_VK_NATIVE_SHFAR=%d%s "
             "DM_VK_NATIVE_LIGHTFUSE=%s DM_VK_NATIVE_LIGHTFUSE_FOG=%s DM_VK_NATIVE_COND=%d DM_VK_NATIVE_GBUF_LE=%d%s DM_VK_NATIVE_PROF=%d",
             cost, sh, pcf, shc, far, vkn_shc_active ? (vkn_shc_active() ? " (cache module on)" : " (cache module off)") : " (no cache module)", lf, lff,
             vkn_query_mode(), vkn_query_le(), vkn_query_le_asked() && !vkn_query_le() ? " (asked, family gate 0)" : "", vkn_prof_level());
}

#endif
