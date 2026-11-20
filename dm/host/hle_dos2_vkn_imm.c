// [DOS2] VK-NATIVE stage 3 wave 2, immediate draws (dm_vkn_imm_register; DM_VK_NATIVE=1 only): the engine's five "draw from memory"
// functions without NVN. main = main.raw of update v786432, build 4979b200; r = the renderer ([main+0x2bbdc68]); dev = [r+0xb70],
// frame stamp = [dev+0x51f0]. Facts from the decompilations / disassembly of 0x92f730 0x92fc70 0x930210 0x930910 0x930e90 0x928dd0
// 0x930850 0x925130 0x924610 0x9243d0 0x8e2970, their callers 0x93f95c (executor, opcode 0x28) 0x93ff20 (0x29) 0x93fc1c (0x2a)
// 0x93fea0 (0x26) 0x93fee0 (0x27) 0xa7ec10 0x941630, and the renderer constructor 0x914bb0 (0x915054..0x915110)
// (docs/dos2/nr/VK-NATIVE-MAP-2.md 5.1, MAP-3 opcodes 0x26..0x2a).
//
// Each original copies its vertices into the engine's dynamic vertex buffer [r+0x8d0] (created by the constructor: 0x100000 bytes,
// usage 2; in mode 1 a native block, the record's +0x10 CPU pointer is the block's: hle_dos2_vkn_buf.c) at the ring head u32 r+0x8d8
// (head + bytes > 0x100000 -> head = 0, stored; head += bytes after the copy; never reset elsewhere but in the constructor), binds
// shader / constants / states / format through the renderer's cache and draws INLINE with nvnCommandBufferDrawArrays(r+0x1718, 0, n)
// (0x930210: DrawElements). Here the same engine-visible work, in the original's order:
//   shader      BindShader 0x9283a0 -> dm_vkn_exec_shader (the exec part: same cache fields, record stamp);
//   constants   the engine's own SetShaderConstant 0x925130(record [r+0x17e0], name hash, data, bytes) - NVN-free (compare + copy into
//               the block's shadow, dirty byte +0x34, high-water +0x28): called directly; the camera product 0x8e2970 (leaf math) and
//               0x930850 (transpose + 0x925130 of name [0x2bbffd0], 0x40 bytes) likewise; names: [0x2bbff90] scaleBias (r+0x930, 16),
//               [0x2bbff98] color (16), [0x2bbffb8] 8 bytes, [0x2bbffd0] the 4x4 matrix;
//   texture     0x924610(r, [0x2bbffc0], texture, 0xffffffff) - NVN-free (it and 0x9243d0 only write the slot arrays and the record's
//               frame stamp): called directly, unless neither the texture nor the fallback r+0x8a8 is live (the original would write
//               through a null record: skipped, counted);
//   states      raster: the default [0x2bbdc18] (0x930210: packet +0x78) -> dm_vkn_exec_raster; blend (mask -1) -> dm_vkn_exec_blend;
//               depth-stencil with reference 0 -> dm_vkn_exec_depth; vertex format r+0x910 / 0x918 / 0x920 / 0x928 -> dm_vkn_exec_vfmt;
//               stream 0 = (r+0x8d0, head) -> dm_vkn_exec_vb (r+0x1748 / r+0x1788 exactly as packet 0x0b writes them: the draw
//               part's dm_vkn_draw_fill reads them); primitive u8 r+0x1714 + NVN mode r+0x1718 (table 0x1f58640, 0 outside 1..7);
//   draw        dm_vkn_draw_tail_pre / dm_vkn_draw_fill / vkn_draw_arrays (indexed: vkn_draw_indexed) / dm_vkn_draw_tail_post: the
//               draw part's common tail (flushes when a shader record is bound, texture flush, conditional-render fields).
// Primitives 2 / 4 / 7 (fan, quads, line loop) are rewritten by vkn_draw.c itself.
//
// 0x930210 (opcode 0x2a only): the indices (packet +0x58, +0x60 count x +0x64 bytes) go into the dynamic index buffer [r+0x8e0]
// (constructor: 0x100000 indices of 4 bytes, NVN type u32 in record +0x24) through 0x928dd0: ring head u32 r+0x8e8, reset when
// head + bytes > 0x100000 (bytes, although the buffer has 4 MiB), returns the old head = the BYTE offset of the copy. The original
// then writes the index record's +0x28 = packet +0x64 (no generation check), r+0x17c8 = [r+0x8e0] and r+0x17d0 = that byte offset
// (when either differs; stamp +0x18), and calls nvnCommandBufferDrawElements(r+0x1718, record +0x24, count, record +8) - the BASE
// address (disassembly 0x930580..0x930598: x4 = [rec+8], nothing added). The open point of MAP-2 (section 0, item 9) is settled:
// NVN draws the indices at the START of the ring with the buffer's own type (u32), not the ones just copied (unless head was 0) and
// not with the packet's index size. This file draws exactly that (first index 0); the census counts the draws whose copy landed
// elsewhere and those whose index size is not 4 (both mean the Switch shows other indices than the caller gave). No writer of opcode
// 0x2a exists in main (strb / strh opcode scan): the path looks unreachable in DOS2.
//
// Who reaches the family (bl / b scan of main, opcode writers by the movz + strb / strh [x0] pattern after AllocCommand 0x941720):
//   0x92f730  executor opcode 0x28 (0x93f95c) and 0xa7eac0 (packet `strh 0x0328` = opcode 0x28 primitive 3, or the immediate tail call
//             0xa7ec10 with primitive 3, 4 vertices, blend [0x2bbdb38], depth-stencil [0x2bbdba0], scaleBias r+0x930 = {2, -2, -1, 1});
//   0x92fc70  opcode 0x29 (0x93ff20) and 0x941550's immediate tail call (0x941630); 0x941550 is called by 0xb20410 0xb20468 0xb207f4
//             0xb20854 with the primitive from a debug-draw entry (+0x34): not a constant;
//   0x930210 / 0x930910 / 0x930e90  only the executor (opcodes 0x2a / 0x26 / 0x27); no writer of these opcodes was found.
// Primitive rewrites (vkn_draw.c) reachable from here: only through the debug-draw entries' primitive (unknown statically); 0xa7eac0
// draws a strip. The primitive of the ordinary draws (opcode 0x0d, writers e.g. 0xabe1ac / 0xb23cbc; packet 0x15 +0x58, 0x92c2c0 = 1)
// comes from object fields too, so which of fan / quads / loop DOS2 uses is for the census (vkn_draw_rw_stats) to show. Iggy's quads
// (GDraw 0x1bf1428) do not go through the renderer's primitive at all (UI wave).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdio.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_buf.h"
#include "../gfx/vkn/vkn_draw.h"
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_draw.h"
#include "hle_dos2_vkn_exec.h"

#define IM_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark, a literal of its own

enum {
    R_DEV = 0xb70, DEV_FRAME = 0x51f0,
    T_VB = 0x180, E_VB = 0x68, T_IB = 0x240, E_IB = 0x58, T_TEX = 0x0, E_TEX = 0x28,
    VB_CPU = 0x10, IB_VA = 0x08, IB_CPU = 0x10, IB_LAST = 0x18, IB_TYPE = 0x24, IB_ISIZE = 0x28,
    R_CAM = 0x888,                                    // camera block: +0x80 / +0xc0 / +0x100 matrices
    R_FALLBACK_TEX = 0x8a8,                           // 0x924610's fallback texture (the white 16x16 of 0x92ed30)
    R_DYN_VB = 0x8d0, R_DYN_VB_HEAD = 0x8d8, R_DYN_IB = 0x8e0, R_DYN_IB_HEAD = 0x8e8,
    R_SH_2D = 0x8f0, R_SH_3D = 0x8f8, R_SH_2DT = 0x900, R_SH_3DT = 0x908,
    R_VF_2D = 0x910, R_VF_3D = 0x918, R_VF_2DT = 0x920, R_VF_3DT = 0x928,
    R_SCALEBIAS = 0x930,
    R_PRIM = 0x1714, R_NVN_PRIM = 0x1718, R_IB = 0x17c8, R_IB_FIRST = 0x17d0, R_SREC = 0x17e0,
    M_RASTER_DEFAULT = 0x2bbdc18, M_PRIM_TAB = 0x1f58640,
    M_N_SCALEBIAS = 0x2bbff90, M_N_COLOR = 0x2bbff98, M_N_CONST8 = 0x2bbffb8, M_N_TEXTURE = 0x2bbffc0,
    F_SET_CONST = 0x925130, F_SET_TEX = 0x924610, F_MATMUL = 0x8e2970, F_MVP = 0x930850,
    RING = 0x100000,                                  // both rings' wrap limit (0x92f730.. / 0x928dd0: "0x100000 < head + bytes")
    REDS = 16,
};
enum { FN_2D, FN_3D, FN_IDX, FN_2DT, FN_3DT, FN_N };   // 0x92f730 0x92fc70 0x930210 0x930910 0x930e90
static const char *const k_fn[FN_N] = {"0x92f730 2D", "0x92fc70 3D", "0x930210 indexed", "0x930910 2D textured", "0x930e90 3D textured"};

typedef void (*set_const_fn)(uint64_t rec, uint64_t name, const void *data, uint32_t bytes);
typedef void (*set_tex_fn)(uint64_t r, uint64_t name, uint64_t tex, uint32_t sampler);
typedef void (*matmul_fn)(float *out, const void *a, const void *b);
typedef void (*mvp_fn)(uint64_t r, const float *m);
static inline void *eng(uint32_t off) { return (void *)(dm_vkn_base() + off); }
static inline void w32(uint64_t p, uint32_t v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static inline void w64(uint64_t p, uint64_t v) { memcpy((void *)(uintptr_t)p, &v, 8); }
static inline void w8(uint64_t p, uint8_t v) { *(uint8_t *)(uintptr_t)p = v; }

// ---- statistics ----
enum { D_NO_R, D_VB, D_VB_BLOCK, D_IB, D_TOO_BIG, D_NO_CAM, D_TEX, D_DRAW, D_IB_OFF, D_IB_SIZE, D_N };
static const char *const k_why[D_N] = {
    [D_NO_R] = "immediate draw with a null renderer",
    [D_VB] = "dynamic vertex buffer r+0x8d0 has no live record / CPU pointer (the original would write through a null record): draw dropped",
    [D_VB_BLOCK] = "dynamic vertex buffer's record CPU pointer is not the native block's (or the block is smaller than the ring): draw dropped",
    [D_IB] = "dynamic index buffer r+0x8e0 has no live record / CPU pointer (0x928dd0 would write through a null record): draw dropped",
    [D_TOO_BIG] = "one immediate draw is bigger than the 1 MiB ring (the original would overrun it): draw dropped",
    [D_NO_CAM] = "camera block r+0x888 is null (the original would read through it): draw dropped",
    [D_TEX] = "neither the texture nor the fallback r+0x8a8 is live (0x924610 would write through a null record): texture not set",
    [D_DRAW] = "the draw part dropped the draw",
    [D_IB_OFF] = "0x930210: indices copied at a non-zero ring offset, NVN (and this draw) reads the ring's start",
    [D_IB_SIZE] = "0x930210: packet index size is not 4, NVN (and this draw) reads them as u32",
};
static struct {
    uint64_t calls[FN_N], verts[FN_N], bytes, vb_wraps, ib_wraps, indices, ib_bytes, tex_sets, d[D_N];
} g_c;
static uint32_t g_red[D_N];
static void red(int k, int fn, uint64_t a, uint64_t b, uint64_t c) {
    g_c.d[k]++;
    if (g_red[k]++ < REDS)
        vkn_logf(IM_RED "imm: %s: %s (values %#llx %#llx %#llx)", fn >= 0 ? k_fn[fn] : "-", k_why[k], (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
}

// ---- engine helpers ----
// The record of a handle in the table at r + tab with the generation check of the machine code (0x92f730: tst / udiv / msub /
// generation compare); NULL page pointers count as "no record" instead of faulting.
static uint64_t im_rec(uint64_t r, uint32_t tab, uint32_t esz, uint64_t h) {
    if (!(h & 0x3fffff00000000ull)) return 0;
    const uint32_t per = vkn_rd32(r + tab + 0x10), idx = (uint32_t)h;
    const uint32_t page = per ? idx / per : 0, off = idx - page * per;
    const uint64_t gp = vkn_rd64(r + tab + 8), gens = gp ? vkn_rd64(gp + 8ull * page) : 0;
    if (!gens || (uint32_t)(h >> 32 & 0x3fffff) != vkn_rd32(gens + 4ull * off)) return 0;
    const uint64_t pp = vkn_rd64(r + tab), base = pp ? vkn_rd64(pp + 8ull * page) : 0;
    return base ? base + (uint64_t)off * esz : 0;
}
// The dynamic vertex ring: checks only (nothing written). 0 = ok, *dst = record +0x10 (the native block's CPU pointer).
static int vb_ring_check(uint64_t r, int fn, uint64_t bytes, uint8_t **dst) {
    const uint64_t h = vkn_rd64(r + R_DYN_VB), rec = im_rec(r, T_VB, E_VB, h);
    const uint64_t cpu = rec ? vkn_rd64(rec + VB_CPU) : 0;
    if (!cpu) { red(D_VB, fn, h, rec, 0); return -1; }
    if (bytes > RING) { red(D_TOO_BIG, fn, bytes, RING, 0); return -1; }
    const vkn_block *b = vkn_buf_get(VKN_BUF_VB, h);
    if (!b || (uint64_t)(uintptr_t)b->cpu != cpu || b->size < RING) { red(D_VB_BLOCK, fn, h, cpu, b ? b->size : 0); return -1; }
    *dst = (uint8_t *)(uintptr_t)cpu;
    return 0;
}
// The copy and the head arithmetic of the originals. Returns the offset the vertices went to (= the stream 0 offset).
static uint32_t vb_ring_put(uint64_t r, int fn, uint8_t *dst, const void *src, uint32_t n, uint32_t stride) {
    const uint32_t bytes = n * stride;
    uint32_t head = vkn_rd32(r + R_DYN_VB_HEAD);
    if (head + bytes > RING) { head = 0; w32(r + R_DYN_VB_HEAD, 0); g_c.vb_wraps++; }
    if (bytes && src) memcpy(dst + head, src, bytes);
    w32(r + R_DYN_VB_HEAD, vkn_rd32(r + R_DYN_VB_HEAD) + bytes);
    g_c.calls[fn]++; g_c.verts[fn] += n; g_c.bytes += bytes;
    return head;
}
static void set_const(uint64_t r, uint32_t name_at, const void *data, uint32_t bytes) {   // 0x925130([r+0x17e0], [main+name_at], ...)
    ((set_const_fn)eng(F_SET_CONST))(vkn_rd64(r + R_SREC), vkn_main64(name_at), data, bytes);
}
static void set_tex(uint64_t r, int fn, uint64_t tex) {   // 0x924610(r, [0x2bbffc0], tex, 0xffffffff)
    if (!im_rec(r, T_TEX, E_TEX, tex) && !im_rec(r, T_TEX, E_TEX, vkn_rd64(r + R_FALLBACK_TEX))) {
        // the original only reaches the null record when the name is in the shader's table; skipping is the same for every other case
        red(D_TEX, fn, tex, vkn_rd64(r + R_FALLBACK_TEX), vkn_rd64(r + R_SREC));
        return;
    }
    ((set_tex_fn)eng(F_SET_TEX))(r, vkn_main64(M_N_TEXTURE), tex, 0xffffffffu);
    g_c.tex_sets++;
}
static void prim(uint64_t r, uint8_t p) {   // r+0x1714 / r+0x1718 as every original (compare with the cache byte first)
    if (vkn_rd8(r + R_PRIM) == p) return;
    w8(r + R_PRIM, p);
    const uint32_t t = (uint32_t)(uint8_t)(p - 1u);
    w32(r + R_NVN_PRIM, t < 7 ? vkn_rd32((uint64_t)(uintptr_t)eng(M_PRIM_TAB) + 4ull * t) : 0u);
}
// The states every function sets after its constants: raster, blend (mask -1), depth-stencil with reference 0, vertex format, stream 0.
static void states(uint64_t r, uint64_t raster, uint64_t blend, uint64_t ds, uint32_t vfmt_at, uint32_t head) {
    dm_vkn_exec_raster(r, raster);
    dm_vkn_exec_blend(r, blend, 0xffffffffu);
    dm_vkn_exec_depth(r, ds, 0);
    dm_vkn_exec_vfmt(r, vkn_rd64(r + vfmt_at));
    dm_vkn_exec_vb(r, 0, vkn_rd64(r + R_DYN_VB), head);
}
// The common tail and the draw: DrawArrays(r+0x1718, 0, n) of the originals.
static void draw_arrays(uint64_t r, uint32_t n) {
    dm_vkn_draw_tail_pre(r);
    vkn_draw_in in; dm_vkn_draw_fill(&in);
    if (vkn_draw_arrays(&in, 0, n)) red(D_DRAW, -1, in.shader, in.prim, n);
    dm_vkn_draw_tail_post(r);
}

// ---- the five functions ----
static vkn_fn g_o_2d, g_o_3d, g_o_idx, g_o_2dt, g_o_3dt;   // never called (each records NVN commands)

// 0x92f730(r, w1 prim, x2 vertices (8 bytes: 2D position), w3 n, x4 blend, x5 depth-stencil, x6 color (16 bytes)); opcode 0x28
// (packet: +1 prim, +8 vertices, +0x10 n, +0x18 blend, +0x20 depth-stencil, color at +0x30) and 0xa7eac0's immediate tail call.
VKN_HOOK h_2d(VKN_A) {
    VKN_UNUSED;
    const uint64_t r = a0; const uint32_t n = (uint32_t)a3;
    if (!r) { red(D_NO_R, FN_2D, 0, 0, 0); return 0; }
    uint8_t *dst;
    if (vb_ring_check(r, FN_2D, (uint64_t)n * 8, &dst)) return 0;
    const uint32_t head = vb_ring_put(r, FN_2D, dst, (const void *)(uintptr_t)a2, n, 8);
    dm_vkn_exec_shader(r, vkn_rd64(r + R_SH_2D));
    set_const(r, M_N_SCALEBIAS, (const void *)(uintptr_t)(r + R_SCALEBIAS), 0x10);
    set_const(r, M_N_COLOR, (const void *)(uintptr_t)a6, 0x10);
    states(r, vkn_main64(M_RASTER_DEFAULT), a4, a5, R_VF_2D, head);
    prim(r, (uint8_t)a1);
    draw_arrays(r, n);
    return 0;
}
// 0x92fc70(r, w1 prim, x2 vertices (12 bytes: 3D position), w3 n, x4 blend, x5 depth-stencil, x6 color); opcode 0x29 (0x93ff20:
// +1 prim, +8 vertices, +0x10 n, +0x18 blend, +0x20 depth-stencil, color at +0x30) and 0x941550's immediate tail call. Matrix:
// 0x8e2970(P, cam+0xc0, cam+0x80), then (after BindShader) the transpose of P under [0x2bbffd0] (0x92fdc4..0x92fe18 = 0x930850).
VKN_HOOK h_3d(VKN_A) {
    VKN_UNUSED;
    const uint64_t r = a0; const uint32_t n = (uint32_t)a3;
    if (!r) { red(D_NO_R, FN_3D, 0, 0, 0); return 0; }
    const uint64_t cam = vkn_rd64(r + R_CAM);
    if (!cam) { red(D_NO_CAM, FN_3D, r, 0, 0); return 0; }
    uint8_t *dst;
    if (vb_ring_check(r, FN_3D, (uint64_t)n * 12, &dst)) return 0;
    const uint32_t head = vb_ring_put(r, FN_3D, dst, (const void *)(uintptr_t)a2, n, 12);
    float p[16];
    ((matmul_fn)eng(F_MATMUL))(p, (const void *)(uintptr_t)(cam + 0xc0), (const void *)(uintptr_t)(cam + 0x80));
    dm_vkn_exec_shader(r, vkn_rd64(r + R_SH_3D));
    ((mvp_fn)eng(F_MVP))(r, p);
    set_const(r, M_N_COLOR, (const void *)(uintptr_t)a6, 0x10);
    states(r, vkn_main64(M_RASTER_DEFAULT), a4, a5, R_VF_3D, head);
    prim(r, (uint8_t)a1);
    draw_arrays(r, n);
    return 0;
}
// 0x930210(r, x1 p) - opcode 0x2a with p = packet + 0x10: +0 world matrix 4x4, +0x40 u8 prim, +0x48 vertices (12 bytes), +0x50 n,
// +0x58 indices, +0x60 index count, +0x64 index size, +0x68 blend, +0x70 depth-stencil, +0x78 raster, +0x80 color.
VKN_HOOK h_idx(VKN_A) {
    VKN_UNUSED;
    const uint64_t r = a0, p = a1;
    if (!r || !p) { red(D_NO_R, FN_IDX, r, p, 0); return 0; }
    const uint32_t n = vkn_rd32(p + 0x50), icount = vkn_rd32(p + 0x60), isize = vkn_rd32(p + 0x64), ibytes = icount * isize;
    const uint64_t cam = vkn_rd64(r + R_CAM);
    if (!cam) { red(D_NO_CAM, FN_IDX, r, 0, 0); return 0; }
    uint8_t *dst;
    if (vb_ring_check(r, FN_IDX, (uint64_t)n * 12, &dst)) return 0;
    const uint64_t ih = vkn_rd64(r + R_DYN_IB), irec = im_rec(r, T_IB, E_IB, ih);
    const uint64_t icpu = irec ? vkn_rd64(irec + IB_CPU) : 0;
    if (!icpu) { red(D_IB, FN_IDX, ih, irec, 0); return 0; }
    if ((uint64_t)icount * isize > RING) { red(D_TOO_BIG, FN_IDX, icount, isize, RING); return 0; }
    const uint32_t head = vb_ring_put(r, FN_IDX, dst, (const void *)(uintptr_t)vkn_rd64(p + 0x48), n, 12);
    // 0x928dd0(r, indices, bytes): the index ring
    uint32_t ioff = vkn_rd32(r + R_DYN_IB_HEAD);
    if (ioff + ibytes > RING) { ioff = 0; w32(r + R_DYN_IB_HEAD, 0); g_c.ib_wraps++; }
    if (ibytes && vkn_rd64(p + 0x58)) memcpy((void *)(uintptr_t)(icpu + ioff), (const void *)(uintptr_t)vkn_rd64(p + 0x58), ibytes);
    w32(r + R_DYN_IB_HEAD, vkn_rd32(r + R_DYN_IB_HEAD) + ibytes);
    g_c.indices += icount; g_c.ib_bytes += ibytes;
    // camera: t1 = cam+0x80 x world, t2 = cam+0xc0 x t1; BindShader; 0x930850(r, t2); color; blend; depth; raster; format; stream 0
    float t1[16], t2[16];
    ((matmul_fn)eng(F_MATMUL))(t1, (const void *)(uintptr_t)(cam + 0x80), (const void *)(uintptr_t)p);
    ((matmul_fn)eng(F_MATMUL))(t2, (const void *)(uintptr_t)(cam + 0xc0), t1);
    dm_vkn_exec_shader(r, vkn_rd64(r + R_SH_3D));
    ((mvp_fn)eng(F_MVP))(r, t2);
    set_const(r, M_N_COLOR, (const void *)(uintptr_t)(p + 0x80), 0x10);
    dm_vkn_exec_blend(r, vkn_rd64(p + 0x68), 0xffffffffu);
    dm_vkn_exec_depth(r, vkn_rd64(p + 0x70), 0);
    dm_vkn_exec_raster(r, vkn_rd64(p + 0x78));
    dm_vkn_exec_vfmt(r, vkn_rd64(r + R_VF_3D));
    dm_vkn_exec_vb(r, 0, vkn_rd64(r + R_DYN_VB), head);
    // the index record (page indexed without a generation check, as 0x9304a8..0x9304cc): +0x28 = packet index size; then r+0x17c8 /
    // r+0x17d0 = (handle, byte offset of the copy) when either differs, the record stamped (+0x18) for a non-zero handle
    w32(irec + IB_ISIZE, isize);
    if (vkn_rd64(r + R_IB) != ih || vkn_rd64(r + R_IB_FIRST) != ioff) {
        if (ih) w64(irec + IB_LAST, vkn_rd64(vkn_rd64(r + R_DEV) + DEV_FRAME));
        w64(r + R_IB, ih); w64(r + R_IB_FIRST, ioff);
    }
    prim(r, vkn_rd8(p + 0x40));
    if (ioff) red(D_IB_OFF, FN_IDX, ioff, icount, vkn_rd32(irec + IB_TYPE));
    if (isize != 4) red(D_IB_SIZE, FN_IDX, isize, icount, vkn_rd32(irec + IB_TYPE));
    // DrawElements(r+0x1718, record +0x24, count, record +8): the base address = first index 0 (see the head comment)
    dm_vkn_draw_tail_pre(r);
    vkn_draw_in in; dm_vkn_draw_fill(&in);
    if (vkn_draw_indexed(&in, 0, icount, 1)) red(D_DRAW, FN_IDX, in.shader, in.prim, icount);
    dm_vkn_draw_tail_post(r);
    return 0;
}
// 0x930910(r, w1 prim, x2 8-byte constant, x3 vertices (0x1c bytes), w4 n, x5 texture, x6 unused, x7 blend, [sp] depth-stencil,
// [sp+8] color) - opcode 0x26 via 0x93fea0 (+1 prim, constant at +4, +0x10 vertices, +0x18 n, +0x20 texture, +0x30 blend, +0x38
// depth-stencil, color at +0x40).
VKN_HOOK h_2dt(VKN_A) {
    VKN_UNUSED;
    const uint64_t r = a0; const uint32_t n = (uint32_t)a4;
    if (!r) { red(D_NO_R, FN_2DT, 0, 0, 0); return 0; }
    uint8_t *dst;
    if (vb_ring_check(r, FN_2DT, (uint64_t)n * 0x1c, &dst)) return 0;
    const uint32_t head = vb_ring_put(r, FN_2DT, dst, (const void *)(uintptr_t)a3, n, 0x1c);
    dm_vkn_exec_shader(r, vkn_rd64(r + R_SH_2DT));
    set_const(r, M_N_SCALEBIAS, (const void *)(uintptr_t)(r + R_SCALEBIAS), 0x10);
    set_const(r, M_N_COLOR, (const void *)(uintptr_t)s1, 0x10);
    set_const(r, M_N_CONST8, (const void *)(uintptr_t)a2, 8);
    set_tex(r, FN_2DT, a5);
    states(r, vkn_main64(M_RASTER_DEFAULT), a7, s0, R_VF_2DT, head);
    prim(r, (uint8_t)a1);
    draw_arrays(r, n);
    return 0;
}
// 0x930e90(r, w1 prim, x2 matrix 4x4, x3 vertices (0x1c bytes), w4 n, x5 texture, x6 unused, x7 blend, [sp] depth-stencil, [sp+8]
// color) - opcode 0x27 via 0x93fee0 (+1 prim, matrix at +0x10, +0x50 vertices, +0x58 n, +0x60 texture, +0x70 blend, +0x78
// depth-stencil, color at +0x80). Matrix (after BindShader, 0x930fd0..0x931050): 0x8e2970(P, cam+0x100, matrix), 0x930850(r, P).
VKN_HOOK h_3dt(VKN_A) {
    VKN_UNUSED;
    const uint64_t r = a0; const uint32_t n = (uint32_t)a4;
    if (!r) { red(D_NO_R, FN_3DT, 0, 0, 0); return 0; }
    const uint64_t cam = vkn_rd64(r + R_CAM);
    if (!cam) { red(D_NO_CAM, FN_3DT, r, 0, 0); return 0; }
    uint8_t *dst;
    if (vb_ring_check(r, FN_3DT, (uint64_t)n * 0x1c, &dst)) return 0;
    const uint32_t head = vb_ring_put(r, FN_3DT, dst, (const void *)(uintptr_t)a3, n, 0x1c);
    dm_vkn_exec_shader(r, vkn_rd64(r + R_SH_3DT));
    float m[16], p[16];
    memcpy(m, (const void *)(uintptr_t)a2, sizeof m);
    ((matmul_fn)eng(F_MATMUL))(p, (const void *)(uintptr_t)(cam + 0x100), m);
    ((mvp_fn)eng(F_MVP))(r, p);
    set_const(r, M_N_COLOR, (const void *)(uintptr_t)s1, 0x10);
    set_tex(r, FN_3DT, a5);
    states(r, vkn_main64(M_RASTER_DEFAULT), a7, s0, R_VF_3DT, head);
    prim(r, (uint8_t)a1);
    draw_arrays(r, n);
    return 0;
}

// ---- census ----
static void im_census(void) {
    vkn_draw_rw_stats w; memset(&w, 0, sizeof w); vkn_draw_rw_stats_get(&w);
    uint64_t calls = 0, verts = 0, drops = 0;
    for (int i = 0; i < FN_N; i++) { calls += g_c.calls[i]; verts += g_c.verts[i]; }
    for (int i = 0; i < D_N; i++) drops += g_c.d[i];
    vkn_logf("imm: %llu immediate draws (2D %llu, 3D %llu, indexed %llu, 2D textured %llu, 3D textured %llu), %llu vertices / %.1f KB copied "
             "(2D %llu, 3D %llu, indexed %llu, 2D tex %llu, 3D tex %llu), vertex ring wraps %llu; indexed: %llu indices / %.1f KB, index ring wraps %llu; "
             "texture sets %llu; rewrites: arrays fan %llu quads %llu loop %llu, indexed fan %llu quads %llu loop %llu (%llu indices, %.1f KB, "
             "blocks %llu, retired %llu, degenerate %llu, dropped %llu); dropped / unexpected %llu",
             (unsigned long long)calls, (unsigned long long)g_c.calls[FN_2D], (unsigned long long)g_c.calls[FN_3D], (unsigned long long)g_c.calls[FN_IDX],
             (unsigned long long)g_c.calls[FN_2DT], (unsigned long long)g_c.calls[FN_3DT], (unsigned long long)verts, g_c.bytes / 1024.0,
             (unsigned long long)g_c.verts[FN_2D], (unsigned long long)g_c.verts[FN_3D], (unsigned long long)g_c.verts[FN_IDX], (unsigned long long)g_c.verts[FN_2DT],
             (unsigned long long)g_c.verts[FN_3DT], (unsigned long long)g_c.vb_wraps, (unsigned long long)g_c.indices, g_c.ib_bytes / 1024.0,
             (unsigned long long)g_c.ib_wraps, (unsigned long long)g_c.tex_sets,
             (unsigned long long)w.draws[0][VKN_RW_FAN], (unsigned long long)w.draws[0][VKN_RW_QUADS], (unsigned long long)w.draws[0][VKN_RW_LOOP],
             (unsigned long long)w.draws[1][VKN_RW_FAN], (unsigned long long)w.draws[1][VKN_RW_QUADS], (unsigned long long)w.draws[1][VKN_RW_LOOP],
             (unsigned long long)w.indices, w.bytes / 1024.0, (unsigned long long)w.blocks, (unsigned long long)w.retired, (unsigned long long)w.degenerate,
             (unsigned long long)w.dropped, (unsigned long long)drops);
    if (!drops) return;
    static const char *const nm[D_N] = {[D_NO_R] = "no-renderer", [D_VB] = "vb-record", [D_VB_BLOCK] = "vb-block", [D_IB] = "ib-record",
        [D_TOO_BIG] = "too-big", [D_NO_CAM] = "no-camera", [D_TEX] = "texture", [D_DRAW] = "draw-dropped", [D_IB_OFF] = "ib-offset-ignored",
        [D_IB_SIZE] = "ib-size-not-4"};
    char line[400]; int k = 0; line[0] = 0;
    for (int i = 0; i < D_N && k < (int)sizeof line - 40; i++)
        if (g_c.d[i]) k += snprintf(line + k, sizeof line - (size_t)k, " %s %llu", nm[i], (unsigned long long)g_c.d[i]);
    vkn_logf(IM_RED "imm: dropped / unexpected:%s", line);
}

// ---- registration ----
void dm_vkn_imm_register(void);
void dm_vkn_imm_register(void) {
    // First 4 instructions of each site (python <scratchpad>/vk/re/pro.py; none PC-relative, checked with capstone).
    static const uint32_t k_2d[4] = {0xf81b0ff9 /* str x25,[sp,#-0x50]! */, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
    static const uint32_t k_3d[4] = {0xd10343ff /* sub sp,sp,#0xd0 */, 0xf90043f9, 0xa9095ff8, 0xa90a57f6};
    static const uint32_t k_idx[4] = {0xd10343ff /* sub sp,sp,#0xd0 */, 0xf90043f9, 0xa9095ff8, 0xa90a57f6};
    static const uint32_t k_2dt[4] = {0xd10183ff /* sub sp,sp,#0x60 */, 0xa90167fa, 0xa9025ff8, 0xa90357f6};
    static const uint32_t k_3dt[4] = {0xd10383ff /* sub sp,sp,#0xe0 */, 0xa90967fa, 0xa90a5ff8, 0xa90b57f6};
    dm_vkn_site(0x92f730, k_2d, "ImmDraw2D", (void *)h_2d, &g_o_2d, VKN_M_NATIVE);
    dm_vkn_site(0x92fc70, k_3d, "ImmDraw3D", (void *)h_3d, &g_o_3d, VKN_M_NATIVE);
    dm_vkn_site(0x930210, k_idx, "ImmDrawIndexed", (void *)h_idx, &g_o_idx, VKN_M_NATIVE);
    dm_vkn_site(0x930910, k_2dt, "ImmDraw2DTextured", (void *)h_2dt, &g_o_2dt, VKN_M_NATIVE);
    dm_vkn_site(0x930e90, k_3dt, "ImmDraw3DTextured", (void *)h_3dt, &g_o_3dt, VKN_M_NATIVE);
    if (vkn_mode() != VKN_NATIVE) return;   // mode 2: no site of this part goes in (VKN_M_NATIVE), no census either
    dm_vkn_census(im_census);
}
#endif
