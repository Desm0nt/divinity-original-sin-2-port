// [DOS2] VK-NATIVE stage 3, draw part (dm_vkn_draw_register): the constant / texture flushes and the draws of the engine's renderer
// on the native backend (dm/gfx/vkn/vkn_draw.c). Mode 1 (native) only; mode 2 has no site here and stays exactly as it was.
// main = main.raw of update v786432, build 4979b200; r = the renderer ([main+0x2bbdc68]); dev = [r+0xb70], frame = [dev+0x51f0].
// Facts from the decompilations of 0x925740 0x926f90 0x924730 0x9249f0 0x9242c0 0x928e80 0x928fb0 0x9290f0 0x9284e0 0x92c3c0 0x9279a0
// 0x928be0 0x93e5f0 (cases 0x12 0x13 0x23 0x24 0x25, the alpha reference) 0x921250 0x918050 0x1bf4590 and the disassembly of 0x928fb0
// (docs/dos2/nr/VK-NATIVE-MAP-2.md sections 1, 2, 4, 5).
//
// Replaced functions (none calls its original: every one of them records NVN commands):
//   0x926f90 FlushGlobalConstants(r)  reader lock r+0xbe0 (skipped when the owner r+0xbe4 is the calling thread, 0x1bf4590); the
//            block pointers [r+0xbf8], count ([r+0xc00] >> 24) & 0xffffff; a block that is dirty (+0x34) or stale (+0 != frame) is
//            copied (+0x30 bytes from the shadow +0x20) into OUR ring (vkn_ring_alloc: the engine's own rule) and announced to the
//            stages 0, 1, 2 at its binding index (+0x35, no "< 8" check here, as the original); record: +0x10 pseudo address,
//            +0x18 CPU pointer in our ring, +0x34 = 0, +0 = frame (the high-water fields stay: the original leaves them).
//   0x925740 FlushShaderConstants(r)  record [r+0x17e0] (0 = nothing); its block arrays +0x20 / +0x28 / +0x30 (vertex / fragment /
//            compute), u8 counts +0x38 / +0x39 / +0x3a, records of 0x40. Upload: binding index < 8 and (dirty or stale), the same
//            copy, then +0x2c = +0x28, +0x28 = 0. Bind pass: EVERY block with +0x18 != 0 and index < 8 is announced again (the
//            original rebinds them on every call: an earlier shader's block at the same slot must not stay), stage 0 / 1 / 2.
//   0x924730 FlushTextures(r)  the original binds the dirty slot range of each stage and resets the ranges; the draw path reads the
//            slot arrays at the draw, so only the resets remain: texture ranges r+0x15dc/dd, +0x1664/65, +0x16ec/ed and sampler
//            ranges +0x1444, +0x14cc, +0x1554 (u16 0xff20 = min 0x20, max -1), exactly as written there.
//   0x9249f0 BindImageByName(r, hash, u64 *texture)  stamps the texture record (r+0 table, 0x28, +8 = frame) and keeps the texture
//            for every stage whose slot (resource node +0x10 / +0x11 / +0x12, s8) is >= 0 (persists like NVN's image bindings).
//   0x9242c0 BindStorageBufferByName(r, hash, handle)  stamps the record (0x921250 = the r+0x300 table, 0x58: +0 = frame) and keeps
//            (address +0x18, size [+0xc] * [+8]) per stage slot: no DOS2 bundle reads storage buffers (vkn_shader.h), counted only.
//   0x928e80 DrawIndexed(r, w1 first, w2 count), 0x928fb0 DrawIndexedInstanced(r, -, w2 instances, -, w4 count; first index
//            [r+0x17d0], disassembly: w2 -> instanceCount, w4 -> count), 0x9290f0 DispatchCompute(r, x, y, z): the common tail
//            (dm_vkn_draw_tail_pre / _post) around vkn_draw_*; the vkn_draw_in comes from dm_vkn_draw_fill. Return void.
// The renderer cache (dm_vkn_draw_fill):
//   +0x17d8 shader handle (BindShader 0x9283a0 compares with it; +0x17e0 its record), +0x16f8 blend, +0x1708 depth-stencil, +0x1710
//   stencil reference (s32), +0x16f0 raster, +0x1740 vertex format, +0x1748 / +0x1750 / +0x1758 vertex buffers with the offsets
//   NVN bound them at in +0x1788 / +0x1790 / +0x1798 (0x92c3c0), +0x17c8 index buffer, +0x1714 primitive (engine 1..7), +0x1868
//   alpha reference (float; >= 0 = alpha test GREATER, NVN 5, in both color states of 0x9284e0), texture-pool ids +0x1558 / +0x15e0
//   / +0x1668 and sampler-pool ids +0x13c0 / +0x1448 / +0x14d0 (32 s32 each), viewport {w, h, x, y} (s32) at +0x171c (packet 0x12
//   stores +4 w, +8 h, +0xc x, +0x10 y there and calls SetViewport(x, y, w, h)), scissor {left, top, right, bottom} (s32) at
//   +0x172c (packet 0x13: SetScissor(l, t, r - l, b - t)).
//   Handles the engine zeroes WITHOUT an NVN call (NVN keeps what it had): 0x9284e0 with blend 0 only updates the cache (also
//   SetRenderTargets 0x9279a0 zeroes +0x16f8 to force the next apply); 0x92c3c0 with depth-stencil 0 / raster 0 updates the cache
//   (and the reference) and binds nothing. So a zero field resolves to the last non-zero one seen at a draw (depth-stencil with its
//   reference). The default blend handle [main+0x2bbdb70]: nvnBlendStateSetDefaults + nvnColorStateSetDefaults = blending off on
//   every target: passed as blend 0 (vkn_draw: blend off).
//   The NVN scissor is set by both kinds of events: every viewport set (packet 0x12, 0x9279a0, 0x928be0, 0x93d9f0, ...) also sets
//   the scissor to the viewport rectangle without touching the scissor cache; packet 0x13 sets the scissor cache and NVN's scissor.
//   Both skip NVN when the cache already holds the values. So the effective scissor is the cache field that changed last: compared
//   with the previous draw's copies (both changed between two draws: the scissor wins, the usual order; counted) - only when the
//   exec part's dm_vkn_exec_scissor (exact: it sees the events in order) is not in the build.
// Conditional rendering (r+0x185c / +0x1860, SetRenderEnableConditional in packet 0x15): the fields are kept exactly; the draw
// itself is always recorded (no query in the native path yet).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_buf.h"
#include "../gfx/vkn/vkn_draw.h"
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_draw.h"

#define DR_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark, a literal of its own

enum {
    R_DEV = 0xb70, DEV_FRAME = 0x51f0,
    R_TEX_TAB = 0x0, TEX_REC = 0x28, R_SSBO_TAB = 0x300, SSBO_REC = 0x58,
    R_GLOCK = 0xbe0, R_GOWNER = 0xbe4, R_GLIST = 0xbf8, R_GCOUNT = 0xc00,
    R_SREC = 0x17e0, R_SHADER = 0x17d8,
    SREC_BLOCKS = 0x20, SREC_COUNTS = 0x38, SREC_RES = 0x48,
    B_STAMP = 0x00, B_VA = 0x10, B_CPU = 0x18, B_SHADOW = 0x20, B_HW = 0x28, B_HW_PREV = 0x2c, B_SIZE = 0x30, B_DIRTY = 0x34, B_BIND = 0x35, B_REC = 0x40,
    R_TEX_VS = 0x1558, R_TEX_FS = 0x15e0, R_TEX_CS = 0x1668, R_SMP_VS = 0x13c0, R_SMP_FS = 0x1448, R_SMP_CS = 0x14d0,
    R_TDIRTY_VS = 0x15dc, R_TDIRTY_FS = 0x1664, R_TDIRTY_CS = 0x16ec, R_SDIRTY_VS = 0x1444, R_SDIRTY_FS = 0x14cc, R_SDIRTY_CS = 0x1554,
    R_RASTER = 0x16f0, R_BLEND = 0x16f8, R_DS = 0x1708, R_SREF = 0x1710, R_PRIM = 0x1714, R_VIEWPORT = 0x171c, R_SCISSOR = 0x172c,
    R_VFMT = 0x1740, R_VB0 = 0x1748, R_VBOFF0 = 0x1788, R_IB = 0x17c8, R_FIRST = 0x17d0,
    R_RENDER_EN = 0x185c, R_RENDER_EN_PREV = 0x185d, R_RENDER_QUERY = 0x1860, R_ALPHA = 0x1868,
    M_DEFAULT_BLEND = 0x2bbdb70, M_TID = 0x1bf4590,
    RES_SLOTS = 0x10,                 // resource node payload: s8 slot per stage at +0x10 / +0x11 / +0x12
    WR_LOCK = 0x100000,               // the engine's reader-writer word: readers count low 20 bits, a writer adds 0x100000
    IMG_SLOTS = 32, REDS = 16,
};
// Our pseudo GPU address of a constant block in our ring (record +0x10): non-zero like the engine's, tag 0xb1 in bits 48..55 (not
// vkn_buf's 0xb0, so vkn_buf_resolve rejects it), the ring offset below. Nothing of the engine dereferences it in mode 1.
#define DR_UBO_VA 0x00b1000000000000ull

static inline void wr64(uint64_t p, uint64_t v) { memcpy((void *)(uintptr_t)p, &v, 8); }
static inline void wr32(uint64_t p, uint32_t v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static inline void wr16(uint64_t p, uint16_t v) { memcpy((void *)(uintptr_t)p, &v, 2); }
static inline void wr8(uint64_t p, uint8_t v) { *(uint8_t *)(uintptr_t)p = v; }
static inline uint64_t frame_of(uint64_t r) { return vkn_rd64(vkn_rd64(r + R_DEV) + DEV_FRAME); }

static vkn_fn g_o_fg, g_o_fs, g_o_ft, g_o_img, g_o_ssbo, g_o_di, g_o_dii, g_o_dc;   // never called (replaced functions)

static struct {
    uint64_t fg, fs, ft, g_up, s_up, s_ann, ring_bytes, ring_fail, null_block, bad_ptr, gl_slot, tex_binds_orig;
    uint64_t img_binds, img_gfx, img_bad, ssbo_binds, ssbo_bad, no_record, cond_resets;
    uint64_t blend_zero, blend_default, ds_zero, raster_zero, scissor_vp, scissor_both;
    uint64_t di, dii, dc, da, dropped;
} g_c;
static uint8_t g_red[8];
#define DR_REDLN(i, ...) do { if (g_red[i] < REDS) { g_red[i]++; vkn_logf(DR_RED "draw host: " __VA_ARGS__); } } while (0)

// ---- the constant flushes ----
typedef int (*tid_fn)(void);
static int engine_tid(void) { return ((tid_fn)(void *)(dm_vkn_base() + M_TID))(); }   // the SDK thread id thunk the original calls
// Reader side of the engine's word r+0xbe0 (0x926f90 / 0x918050): wait while a writer holds it, then count one reader.
static void rlock(uint64_t w) {
    uint32_t *p = (uint32_t *)(uintptr_t)w;
    for (;;) {
        while (__atomic_load_n(p, __ATOMIC_ACQUIRE) >= WR_LOCK) sched_yield();
        const uint32_t v = __atomic_add_fetch(p, 1, __ATOMIC_ACQ_REL);
        if (v < WR_LOCK) return;
        __atomic_sub_fetch(p, 1, __ATOMIC_ACQ_REL);
    }
}
static void runlock(uint64_t w) { __atomic_sub_fetch((uint32_t *)(uintptr_t)w, 1, __ATOMIC_ACQ_REL); }

// Copies a block into our ring and writes the record as the original does. shader: the shader-block variant (high-water fields).
// Returns the ring offset, or -1 (nothing written: the block stays dirty / stale and is tried again at the next flush).
static int64_t upload(uint64_t b, uint64_t frame, int shader) {
    const uint32_t size = vkn_rd32(b + B_SIZE); const uint64_t shadow = vkn_rd64(b + B_SHADOW);
    uint8_t *cpu = (uint8_t *)vkn_ring_cpu(); uint32_t off = 0;
    if (!shadow && size) { g_c.null_block++; DR_REDLN(0, "constant block %#llx (%u bytes, binding %u) has no shadow copy: not uploaded", (unsigned long long)b, size, vkn_rd8(b + B_BIND)); return -1; }
    if (!cpu || vkn_ring_alloc(size, &off)) { g_c.ring_fail++; DR_REDLN(1, "constant ring: no room / no ring for %u bytes (block %#llx)", size, (unsigned long long)b); return -1; }
    if (size) memcpy(cpu + off, (const void *)(uintptr_t)shadow, size);
    wr64(b + B_VA, DR_UBO_VA | off);
    wr64(b + B_CPU, (uint64_t)(uintptr_t)(cpu + off));
    if (shader) { const uint32_t hw = vkn_rd32(b + B_HW); wr32(b + B_HW, 0); wr32(b + B_HW_PREV, hw); }
    wr8(b + B_DIRTY, 0);
    wr64(b + B_STAMP, frame);
    g_c.ring_bytes += size;
    return off;
}
static void flush_globals(uint64_t r) {
    g_c.fg++;
    const int32_t owner = (int32_t)vkn_rd32(r + R_GOWNER);
    const int lock = owner == -1 || owner != engine_tid();
    if (lock) rlock(r + R_GLOCK);
    const uint64_t n = (vkn_rd64(r + R_GCOUNT) >> 24) & 0xffffff, list = vkn_rd64(r + R_GLIST), frame = frame_of(r);
    for (uint64_t i = 0; list && i < n; i++) {
        const uint64_t b = vkn_rd64(list + 8 * i);
        if (!b) { g_c.null_block++; DR_REDLN(0, "global constant list entry %llu of %llu is NULL", (unsigned long long)i, (unsigned long long)n); continue; }
        if (!vkn_rd8(b + B_DIRTY) && vkn_rd64(b + B_STAMP) == frame) continue;
        const int64_t off = upload(b, frame, 0);
        if (off < 0) continue;
        g_c.g_up++;
        const uint32_t slot = vkn_rd8(b + B_BIND), size = vkn_rd32(b + B_SIZE);
        if (slot >= VKN_UBO_SLOTS) { g_c.gl_slot++; DR_REDLN(2, "global constant block %#llx: binding index %u >= %d", (unsigned long long)b, slot, VKN_UBO_SLOTS); continue; }
        for (int st = 0; st < 3; st++) vkn_draw_ubo(st, slot, (uint32_t)off, size);   // NVN stages 0, 1, 5 at once (0x926f90)
    }
    if (lock) runlock(r + R_GLOCK);
}
static void flush_shader(uint64_t r) {
    const uint64_t rec = vkn_rd64(r + R_SREC);
    if (!rec) return;
    g_c.fs++;
    const uint64_t frame = frame_of(r);
    for (int k = 0; k < 3; k++) {   // upload pass: vertex, fragment, compute lists
        const uint64_t arr = vkn_rd64(rec + SREC_BLOCKS + 8 * k); const uint32_t cnt = vkn_rd8(rec + SREC_COUNTS + k);
        if (!arr && cnt) { g_c.null_block++; DR_REDLN(0, "shader record %#llx: list %d has %u blocks and no array", (unsigned long long)rec, k, cnt); continue; }
        for (uint32_t i = 0; i < cnt; i++) {
            const uint64_t b = arr + (uint64_t)B_REC * i;
            if (vkn_rd8(b + B_BIND) < 8 && (vkn_rd8(b + B_DIRTY) || vkn_rd64(b + B_STAMP) != frame) && upload(b, frame, 1) >= 0) g_c.s_up++;
        }
    }
    const uint8_t *cpu = (const uint8_t *)vkn_ring_cpu();
    for (int k = 0; k < 3; k++) {   // bind pass: every uploaded block of the current shader, at its own stage
        const uint64_t arr = vkn_rd64(rec + SREC_BLOCKS + 8 * k); const uint32_t cnt = arr ? vkn_rd8(rec + SREC_COUNTS + k) : 0;
        for (uint32_t i = 0; i < cnt; i++) {
            const uint64_t b = arr + (uint64_t)B_REC * i, p = vkn_rd64(b + B_CPU); const uint32_t slot = vkn_rd8(b + B_BIND), size = vkn_rd32(b + B_SIZE);
            if (!p || slot >= 8) continue;
            const uint64_t c = (uint64_t)(uintptr_t)cpu;
            if (!cpu || p < c || p - c + size > VKN_RING_BYTES) {
                g_c.bad_ptr++;
                DR_REDLN(3, "shader block %#llx (stage list %d, binding %u): CPU pointer %#llx is not in our ring %#llx (not announced)", (unsigned long long)b, k, slot, (unsigned long long)p, (unsigned long long)c);
                continue;
            }
            vkn_draw_ubo(k, slot, (uint32_t)(p - c), size);
            g_c.s_ann++;
        }
    }
}
// 0x924730: only the resets (and a count of the binds the original would have issued: slots in the dirty ranges holding a texture).
static void flush_textures(uint64_t r) {
    g_c.ft++;
    static const uint32_t dirty[3] = {R_TDIRTY_VS, R_TDIRTY_FS, R_TDIRTY_CS}, ids[3] = {R_TEX_VS, R_TEX_FS, R_TEX_CS}, sdirty[3] = {R_SDIRTY_VS, R_SDIRTY_FS, R_SDIRTY_CS};
    for (int k = 0; k < 3; k++) {
        const int8_t lo = (int8_t)vkn_rd8(r + dirty[k]), hi = (int8_t)vkn_rd8(r + dirty[k] + 1);
        for (int s = lo < 0 ? 0 : lo; s <= hi && s < 32; s++) g_c.tex_binds_orig += (int32_t)vkn_rd32(r + ids[k] + 4u * s) != -1;
        wr16(r + dirty[k], 0xff20);    // min 0x20, max -1 (the vertex / fragment ranges as two byte stores, the compute one as u16)
        wr16(r + sdirty[k], 0xff20);
    }
}
void dm_vkn_draw_tail_pre(uint64_t r) {
    if (vkn_rd64(r + R_SREC)) { flush_globals(r); flush_shader(r); }
    flush_textures(r);
    if (vkn_rd8(r + R_RENDER_EN_PREV) && vkn_rd8(r + R_RENDER_EN)) {   // nvnCommandBufferSetRenderEnable(cb, 1) dropped
        wr8(r + R_RENDER_EN, 0); wr8(r + R_RENDER_EN_PREV, 0); wr64(r + R_RENDER_QUERY, 0);
        g_c.cond_resets++;
    }
}
void dm_vkn_draw_tail_post(uint64_t r) { wr8(r + R_RENDER_EN_PREV, vkn_rd8(r + R_RENDER_EN)); }

// ---- the shader record's resource table (record +0x48): buckets +4 (count) / +8 (array); key 0 hashes as 0x1f77c8c; node
// {next, key, payload from +0x10} ----
static uint64_t res_node(uint64_t rec, uint64_t hash) {
    const uint64_t t = rec ? vkn_rd64(rec + SREC_RES) : 0;
    if (!t) return 0;
    const uint32_t nb = vkn_rd32(t + 4); const uint64_t arr = vkn_rd64(t + 8);
    if (!nb || !arr) return 0;
    const uint64_t k = hash ? hash : 0x1f77c8c;
    for (uint64_t n = vkn_rd64(arr + 8 * (k % nb)); n; n = vkn_rd64(n)) if (vkn_rd64(n + 8) == hash) return n;
    return 0;
}

// ---- images and storage buffers (kept per stage slot: they persist like NVN's bindings) ----
static uint64_t g_img[3][IMG_SLOTS];
static struct { uint64_t addr; uint32_t size; } g_ssbo[3][IMG_SLOTS];
VKN_HOOK h_bind_image(VKN_A) {   // 0x9249f0(r, hash, u64 *texture)
    VKN_UNUSED;
    const uint64_t r = a0, rec = vkn_rd64(r + R_SREC);
    if (!rec) return 0;
    const uint64_t h = a2 ? vkn_rd64(a2) : 0, trec = vkn_rec(r, R_TEX_TAB, TEX_REC, h);
    if (!trec) { g_c.img_bad++; DR_REDLN(4, "BindImageByName: texture handle %#llx is not live (the original would fault): ignored", (unsigned long long)h); return 0; }
    wr64(trec + 8, frame_of(r));
    const uint64_t n = res_node(rec, a1);
    if (!n) return 0;
    for (int k = 0; k < 3; k++) {
        const int8_t slot = (int8_t)vkn_rd8(n + RES_SLOTS + k);
        if (slot < 0) continue;
        if (slot >= IMG_SLOTS) { g_c.img_bad++; DR_REDLN(4, "BindImageByName: stage %d image slot %d >= %d", k, slot, IMG_SLOTS); continue; }
        g_img[k][slot] = h; g_c.img_binds++;
        if (k < 2) g_c.img_gfx++;
    }
    return 0;
}
VKN_HOOK h_bind_ssbo(VKN_A) {    // 0x9242c0(r, hash, handle)
    VKN_UNUSED;
    const uint64_t r = a0, srec = vkn_rec(r, R_SSBO_TAB, SSBO_REC, a2);   // = 0x921250(r + 0x300, handle)
    if (!srec) { g_c.ssbo_bad++; DR_REDLN(5, "BindStorageBufferByName: handle %#llx is not live (the original would fault): ignored", (unsigned long long)a2); return 0; }
    wr64(srec, frame_of(r));
    const uint64_t rec = vkn_rd64(r + R_SREC);
    if (!rec) { g_c.ssbo_bad++; DR_REDLN(5, "BindStorageBufferByName without a shader record (the original would fault): ignored"); return 0; }
    const uint64_t n = res_node(rec, a1);
    if (!n) return 0;
    for (int k = 0; k < 3; k++) {
        const int8_t slot = (int8_t)vkn_rd8(n + RES_SLOTS + k);
        if (slot < 0) continue;
        if (slot >= IMG_SLOTS) { g_c.ssbo_bad++; continue; }
        g_ssbo[k][slot].addr = vkn_rd64(srec + 0x18); g_ssbo[k][slot].size = vkn_rd32(srec + 0xc) * vkn_rd32(srec + 8);
        g_c.ssbo_binds++;
    }
    return 0;
}

// ---- the renderer cache -> vkn_draw_in ----
// The exec part (hle_dos2_vkn_exec.h, built in parallel) tracks the scissor in effect from the events themselves; weak: until the
// merge (or without that part) the change detection below stands in.
int dm_vkn_exec_scissor(uint64_t r, int32_t out[4]) __attribute__((weak));
static uint64_t g_last_blend, g_last_ds, g_last_raster; static uint8_t g_last_sref;
static int32_t g_prev_vp[4], g_prev_sc[4]; static int g_prev_ok, g_sc_from_vp = 1;
void dm_vkn_draw_fill(vkn_draw_in *in) {
    const uint64_t r = vkn_renderer();
    memset(in, 0, sizeof *in);
    if (!r) return;
    in->shader = vkn_rd64(r + R_SHADER);
    // blend: 0 = what NVN still has (the last applied one); the default handle = NVN defaults = blending off
    uint64_t bl = vkn_rd64(r + R_BLEND);
    if (!bl) { bl = g_last_blend; g_c.blend_zero++; } else g_last_blend = bl;
    const uint64_t def = vkn_main64(M_DEFAULT_BLEND);
    if (bl && bl == def) { bl = 0; g_c.blend_default++; }
    in->blend = bl;
    uint64_t ds = vkn_rd64(r + R_DS); uint8_t sref = (uint8_t)vkn_rd32(r + R_SREF);
    if (!ds) { ds = g_last_ds; sref = g_last_sref; g_c.ds_zero++; } else { g_last_ds = ds; g_last_sref = sref; }
    in->ds = ds; in->stencil_ref = sref;
    uint64_t rs = vkn_rd64(r + R_RASTER);
    if (!rs) { rs = g_last_raster; g_c.raster_zero++; } else g_last_raster = rs;
    in->raster = rs;
    in->vfmt = vkn_rd64(r + R_VFMT);
    for (int i = 0; i < 3; i++) { in->vb[i] = vkn_rd64(r + R_VB0 + 8u * i); in->vb_off[i] = (uint32_t)vkn_rd64(r + R_VBOFF0 + 8u * i); }
    in->ib = vkn_rd64(r + R_IB);
    in->prim = vkn_rd8(r + R_PRIM);
    uint32_t a = vkn_rd32(r + R_ALPHA); memcpy(&in->alpha_ref, &a, 4);
    in->tex[0] = (const int32_t *)(uintptr_t)(r + R_TEX_VS); in->tex[1] = (const int32_t *)(uintptr_t)(r + R_TEX_FS); in->tex[2] = (const int32_t *)(uintptr_t)(r + R_TEX_CS);
    in->smp[0] = (const int32_t *)(uintptr_t)(r + R_SMP_VS); in->smp[1] = (const int32_t *)(uintptr_t)(r + R_SMP_FS); in->smp[2] = (const int32_t *)(uintptr_t)(r + R_SMP_CS);
    int32_t vp[4], sc[4];   // vp {w, h, x, y}; sc {l, t, r, b}
    for (int i = 0; i < 4; i++) { vp[i] = (int32_t)vkn_rd32(r + R_VIEWPORT + 4u * i); sc[i] = (int32_t)vkn_rd32(r + R_SCISSOR + 4u * i); }
    in->viewport[0] = (float)vp[2]; in->viewport[1] = (float)vp[3]; in->viewport[2] = (float)vp[0]; in->viewport[3] = (float)vp[1];
    if (dm_vkn_exec_scissor) {   // the exec part sees every viewport / scissor event in order: exact
        if (dm_vkn_exec_scissor(r, in->scissor)) g_c.scissor_vp++;
        return;
    }
    if (g_prev_ok) {   // without it: the cache field that changed since the previous draw
        const int vch = memcmp(vp, g_prev_vp, sizeof vp) != 0, sch = memcmp(sc, g_prev_sc, sizeof sc) != 0;
        if (sch) g_sc_from_vp = 0; else if (vch) g_sc_from_vp = 1;
        if (sch && vch) g_c.scissor_both++;
    }
    memcpy(g_prev_vp, vp, sizeof vp); memcpy(g_prev_sc, sc, sizeof sc); g_prev_ok = 1;
    if (g_sc_from_vp) { in->scissor[0] = vp[2]; in->scissor[1] = vp[3]; in->scissor[2] = vp[0]; in->scissor[3] = vp[1]; g_c.scissor_vp++; }
    else { in->scissor[0] = sc[0]; in->scissor[1] = sc[1]; in->scissor[2] = sc[2] - sc[0]; in->scissor[3] = sc[3] - sc[1]; }
}

// ---- the draws ----
VKN_HOOK h_flush_globals(VKN_A) { VKN_UNUSED; flush_globals(a0); return 0; }   // 0x926f90(r)
VKN_HOOK h_flush_shader(VKN_A) { VKN_UNUSED; flush_shader(a0); return 0; }     // 0x925740(r)
VKN_HOOK h_flush_textures(VKN_A) { VKN_UNUSED; flush_textures(a0); return 0; } // 0x924730(r)
VKN_HOOK h_draw_indexed(VKN_A) {   // 0x928e80(r, w1 first, w2 count)
    VKN_UNUSED;
    const uint64_t r = a0;
    dm_vkn_draw_tail_pre(r);
    vkn_draw_in in; dm_vkn_draw_fill(&in);
    g_c.di++; g_c.dropped += vkn_draw_indexed(&in, (uint32_t)a1, (uint32_t)a2, 1) != 0;
    dm_vkn_draw_tail_post(r);
    return 0;
}
VKN_HOOK h_draw_indexed_inst(VKN_A) {   // 0x928fb0(r, -, w2 instances, -, w4 count); first index [r+0x17d0]
    VKN_UNUSED;
    const uint64_t r = a0;
    dm_vkn_draw_tail_pre(r);
    vkn_draw_in in; dm_vkn_draw_fill(&in);
    g_c.dii++; g_c.dropped += vkn_draw_indexed(&in, (uint32_t)vkn_rd64(r + R_FIRST), (uint32_t)a4, (uint32_t)a2) != 0;
    dm_vkn_draw_tail_post(r);
    return 0;
}
VKN_HOOK h_dispatch(VKN_A) {   // 0x9290f0(r, w1 x, w2 y, w3 z)
    VKN_UNUSED;
    const uint64_t r = a0;
    dm_vkn_draw_tail_pre(r);
    vkn_draw_in in; dm_vkn_draw_fill(&in);
    vkn_img_bind img[IMG_SLOTS]; uint32_t n = 0;
    for (uint32_t s = 0; s < IMG_SLOTS; s++) if (g_img[2][s]) { img[n].slot = s; img[n].tex = g_img[2][s]; n++; }
    g_c.dc++; g_c.dropped += vkn_draw_dispatch(&in, img, n, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3) != 0;
    dm_vkn_draw_tail_post(r);
    return 0;
}
int dm_vkn_draw_arrays(uint32_t first, uint32_t count) {
    const uint64_t r = vkn_renderer();
    if (!r) return 1;
    dm_vkn_draw_tail_pre(r);
    vkn_draw_in in; dm_vkn_draw_fill(&in);
    const int rc = vkn_draw_arrays(&in, first, count);
    g_c.da++; g_c.dropped += rc != 0;
    dm_vkn_draw_tail_post(r);
    return rc;
}

// ---- census ----
static uint64_t g_census_frame, g_census_bytes;
static void draw_census(void) {
    vkn_draw_stats s; vkn_draw_stats2 x; vkn_draw_stats_get(&s); vkn_draw_stats2_get(&x);
    const uint64_t f = vkn_frame(), df = f - g_census_frame, db = g_c.ring_bytes - g_census_bytes;
    g_census_frame = f; g_census_bytes = g_c.ring_bytes;
    vkn_logf("draw: %llu recorded (indexed %llu, arrays %llu), %llu dispatches; engine calls indexed %llu / instanced %llu / arrays %llu / dispatch %llu, %llu dropped "
             "(shader %llu, state %llu, pipeline %llu, pass %llu, primitive %llu, vb %llu, ib %llu, binding %llu, viewport %llu, variants %llu, set %llu, no recording %llu); "
             "empty scissor %llu; binds: pipeline %llu, set %llu, vb %llu, ib %llu, viewport %llu; sets built %llu, cache hits %llu, flushes %llu, pools %llu, key words %llu",
             (unsigned long long)s.draws, (unsigned long long)s.indexed, (unsigned long long)s.arrays, (unsigned long long)s.dispatches,
             (unsigned long long)g_c.di, (unsigned long long)g_c.dii, (unsigned long long)g_c.da, (unsigned long long)g_c.dc, (unsigned long long)g_c.dropped,
             (unsigned long long)s.dropped_shader, (unsigned long long)s.dropped_state, (unsigned long long)s.dropped_pipe, (unsigned long long)s.dropped_pass, (unsigned long long)s.dropped_prim,
             (unsigned long long)x.dropped_vb, (unsigned long long)x.dropped_ib, (unsigned long long)x.dropped_bind, (unsigned long long)x.dropped_viewport, (unsigned long long)x.dropped_variant,
             (unsigned long long)x.dropped_set, (unsigned long long)x.dropped_cb, (unsigned long long)x.scissor_empty,
             (unsigned long long)s.pipe_binds, (unsigned long long)s.set_binds, (unsigned long long)s.vb_binds, (unsigned long long)s.ib_binds, (unsigned long long)s.viewport_sets,
             (unsigned long long)s.set_builds, (unsigned long long)s.set_cache, (unsigned long long)x.forget_all, (unsigned long long)x.pools, (unsigned long long)x.set_bytes_words);
    vkn_logf("draw: textures feedback %llu, unknown %llu, empty %llu, view type mismatch %llu, no placeholder %llu; samplers unknown %llu; uniform zero block %llu, announced %llu (bad %llu); "
             "flushes global %llu / shader %llu / texture %llu (NVN texture binds the original would make %llu), uploads global %llu / shader %llu, re-announced %llu; "
             "ring %.1f KB a frame (%llu frames), ring failures %llu, bad blocks %llu / pointers %llu / global slots %llu; images %llu (graphics %llu, bad %llu), storage buffers %llu (bad %llu); "
             "conditional resets %llu; resolved: blend 0 %llu, default blend %llu, depth-stencil 0 %llu, raster 0 %llu; scissor from viewport %llu, both changed %llu",
             (unsigned long long)s.feedback_tex, (unsigned long long)s.unknown_tex, (unsigned long long)x.empty_tex, (unsigned long long)x.view_mismatch, (unsigned long long)x.placeholder_fail,
             (unsigned long long)s.unknown_smp, (unsigned long long)x.zero_ubo, (unsigned long long)x.ubo_announce, (unsigned long long)x.ubo_bad,
             (unsigned long long)g_c.fg, (unsigned long long)g_c.fs, (unsigned long long)g_c.ft, (unsigned long long)g_c.tex_binds_orig, (unsigned long long)g_c.g_up, (unsigned long long)g_c.s_up,
             (unsigned long long)g_c.s_ann, df ? db / 1024.0 / (double)df : 0.0, (unsigned long long)df, (unsigned long long)g_c.ring_fail, (unsigned long long)g_c.null_block,
             (unsigned long long)g_c.bad_ptr, (unsigned long long)g_c.gl_slot, (unsigned long long)g_c.img_binds, (unsigned long long)g_c.img_gfx, (unsigned long long)g_c.img_bad,
             (unsigned long long)g_c.ssbo_binds, (unsigned long long)g_c.ssbo_bad, (unsigned long long)g_c.cond_resets, (unsigned long long)g_c.blend_zero, (unsigned long long)g_c.blend_default,
             (unsigned long long)g_c.ds_zero, (unsigned long long)g_c.raster_zero, (unsigned long long)g_c.scissor_vp, (unsigned long long)g_c.scissor_both);
}

void dm_vkn_draw_register(void);
void dm_vkn_draw_register(void) {
    // First 4 instructions of each site (python <scratchpad>/vk/re/pro.py; none PC-relative, checked with capstone).
    static const uint32_t k_fg[4] = {0xd101c3ff /* sub sp,sp,#0x70 */, 0xa9016ffc, 0xa90267fa, 0xa9035ff8};
    static const uint32_t k_fs[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa9026ffc, 0xa90367fa, 0xa9045ff8};
    static const uint32_t k_ft[4] = {0xa9ba6ffc /* stp x28,x27,[sp,#-0x60]! */, 0xa90167fa, 0xa9025ff8, 0xa90357f6};
    static const uint32_t k_img[4] = {0xa9bc5ff8 /* stp x24,x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    static const uint32_t k_ssbo[4] = {0xf81d0ff5 /* str x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    static const uint32_t k_di[4] = {0xa9bc5ff8 /* stp x24,x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    static const uint32_t k_dii[4] = {0xa9bc5ff8 /* stp x24,x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    static const uint32_t k_dc[4] = {0xa9bc5ff8 /* stp x24,x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    dm_vkn_site(0x926f90, k_fg, "FlushGlobalConstants", (void *)h_flush_globals, &g_o_fg, VKN_M_NATIVE);
    dm_vkn_site(0x925740, k_fs, "FlushShaderConstants", (void *)h_flush_shader, &g_o_fs, VKN_M_NATIVE);
    dm_vkn_site(0x924730, k_ft, "FlushTextures", (void *)h_flush_textures, &g_o_ft, VKN_M_NATIVE);
    dm_vkn_site(0x9249f0, k_img, "BindImageByName", (void *)h_bind_image, &g_o_img, VKN_M_NATIVE);
    dm_vkn_site(0x9242c0, k_ssbo, "BindStorageBufferByName", (void *)h_bind_ssbo, &g_o_ssbo, VKN_M_NATIVE);
    dm_vkn_site(0x928e80, k_di, "DrawIndexed", (void *)h_draw_indexed, &g_o_di, VKN_M_NATIVE);
    dm_vkn_site(0x928fb0, k_dii, "DrawIndexedInstanced", (void *)h_draw_indexed_inst, &g_o_dii, VKN_M_NATIVE);
    dm_vkn_site(0x9290f0, k_dc, "DispatchCompute", (void *)h_dispatch, &g_o_dc, VKN_M_NATIVE);
    if (vkn_mode() != VKN_NATIVE) return;   // mode 2: no site of this part goes in (VKN_M_NATIVE), nothing else either
    vkn_draw_install();
    dm_vkn_census(draw_census);
}
#endif
