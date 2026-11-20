// [DOS2] VK-NATIVE stage 4, part "texture read-back and video frames" (dm_vkn_read_register; DM_VK_NATIVE=1 only). main = main.raw
// of update v786432, build 4979b200; r = the renderer ([main+0x2bbdc68]); texture table at r+0 (0x28-byte records: +0 the texture
// object = NVNtexture (0xc0) + the engine's tail, = the vkn_tex cookie; +0x10 engine format); the game's NVN table at main+0x2bbee98.
// Facts: decompilations of 0x9291c0 0x9296d0 0x929880 0xa66bc0 0x92ccb0 0xbe74c0 0xbe79a0 0x1083b10 0xb05150 0xb05270 0xd1ada0 0xd1bbe0
// 0xd1bc80 0xd1b8a0 (scratchpad s4read/re/out), docs/dos2/nr/VK-NATIVE-MAP-3.md section 7.1.
//
// 1. READ-BACK (DM_VK_NATIVE_READ, default 1 in mode 1; 0 = the stage-3 "no data" behaviour).
//   The three engine readers all do, for a texture handle (r, x1): record + object check, w = nvnTextureGetWidth, h = GetHeight,
//   (0x9291c0 / 0x9296d0: fmt = GetFormat), bpp = 0x91eea0(record +0x10).bytes, buffer = 0x83cc00(w * h * bpp), and when
//   (nvnTextureGetFlags & 8) == 0 (not COMPRESSIBLE) nvnTextureReadTexels(object, NULL view, region {0, 0, 0, w, h, 1}, buffer) through
//   the NVN table slot 222 (main+0x2bbf588) - tight rows, row 0 first. Then engine code only (no NVN):
//     0x9291c0(r, h, path string, ..., w6 out w, s0 out h) file: 0xbe74c0 rescale (fmt 0x25 RGBA8 -> 4 channels, 0x19 RGB8 -> 3, any
//              other format: 0x80f560 + a trap), then 0xbe7500 writes the file (by extension); returns 1 / 0.
//     0x9296d0(r, h, stream, ...) encode: 0xbe79a0 (same two formats, else the trap) into a new buffer, both buffers written to the
//              stream 0x81a280; returns 1 / 0.
//     0x929880(r, h, {ptr, u32 size, u32 w, u32 h} out) raw: fills the descriptor (the requester owns the buffer); returns 1.
//   The only caller is the frame function 0xa66bc0 (step 3 of MAP-3 7.1), once per queued request ([0x2bbb260]+0x78, 0x50-byte entries
//   built by 0xb05150 (file: +0x18 path, +0x28 / +0x2c size) or 0xb05270 (stream +0x10)); it renders the window into a
//   "ScreenshotBackBuffer" target, copies it (0x920b00, the pass part: vkn_pass_copy, scaled when the request has its own size) into
//   a read texture made by 0x92ccb0(0, engine format 4 RGBA8 -> NVN 0x25, w, h, ..., flags 4, usage 8) = CreateTexture (the texture
//   part makes its vkn_tex), ends the off-frame recording (main window: EndFrame(win, 0) = the frame part's vkn_rec_submit(1); another
//   window: nothing submits it - its EndRecording / Submit / SyncWait are the command part's sinks), then calls one reader. The save
//   thumbnail is a FILE request: 0x1083b10 (callers 0x105b6f0 / 0x105c860, the save code [inferred]) queues 0xb05150(window, path,
//   480, 270): read texture 480 x 270 RGBA8 -> 0x9291c0.
//   Here: the slot 222 function (dm_vkn_read_slot hands it to the command part's repointing loop) reads the backend's image of the
//   object (vkn_tex_by_cookie) with vkn_read_texels: into the open off-frame recording when one is open, else its own; submit + wait;
//   bytes as NVN returns them. The three readers stay ENGINE code (rescale / encode / file / stream run on the bytes we give), behind
//   pre-check hooks that keep the stage-3 "return 0 = no data" (what they return for a stale handle; 0xa66bc0 ignores the result) when
//   any of these fails, with a red line: the slot is not repointed (the command part's wiring missing), the object is unknown to the
//   backend, the format is not readable (vkn_read_supported) or its NVN texel size differs from the engine's bpp (the buffer size),
//   for 0x9291c0 / 0x9296d0 the NVN format is not 0x25 / 0x19 (the engine would trap), a presented recording is open, or it is not
//   the recording thread. The readers' own NVN calls besides slot 222 are object queries (GetWidth / GetHeight / GetFormat / GetFlags)
//   that HLE NVN answers from its texture object - DEPENDENCY on the creation piece (stage 4, in parallel): those four queries must
//   keep answering the engine's values for objects whose creation it replaces.
//
// 2. VIDEO FRAMES: the movie player 0xd1ada0 copies its decoded planes into the plane textures' mapped storage ([object+0xe0],
//   stride from nvnTextureGetStride) before its own draw. Stage 4 (copies): that is the storage mirror's job (hle_dos2_vkn_glue.c,
//   which hooks 0xd1ada0 for the planes' row counts and mirrors each plane's rows once, before the movie's draw). This file's own
//   plane upload (a second copy of the same rows from the decoder's buffers) and DM_VK_NATIVE_VIDEO are gone.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_frame.h"
#include "../gfx/vkn/vkn_read.h"
#include "../gfx/vkn/vkn_tex.h"
#include "../gfx/nvn/nvn_obj.h"   // view_t: the HLE layout of NVNtextureView in guest memory (a view argument of ReadTexels)
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_stack.h"

#define RB_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark, a literal of its own

// ---- what other parts call (weak on their side) ----
// int dm_vkn_read_slot(unsigned slot_index, void **fn): 1 = this part replaces that slot of the game's NVN table (*fn = its entry,
//     a host-stack stub, to store into main+0x2bbee98 + 8 * slot_index when the table is filled); 0 = not this part's slot (or read-back
//     is off). Today: slot 222 nvnTextureReadTexels. The command part's repointing loop (0x938330's hook) calls it for every slot.
int dm_vkn_read_slot(unsigned slot_index, void **fn);
void dm_vkn_read_register(void);

// ---- engine facts ----
enum {
    M_TABLE = 0x2bbee98, S_READ_TEXELS = 222,            // nvnTextureReadTexels = main+0x2bbf588
    TEX_TAB = 0x000, TEX_REC = 0x28, REC_OBJ = 0x00, REC_EFMT = 0x10,
    FN_FMT_INFO = 0x91eea0,                              // 0x91eea0(out[4], engine format): {name, components, bytes, compressed}
    NVN_RGBA8 = 0x25, NVN_RGB8 = 0x19,                   // the only formats 0xbe74c0 / 0xbe79a0 accept (else 0x80f560 + trap)
};
typedef void (*fmt_info_fn)(uint64_t out[4], int efmt);
static void *eng(uint32_t off) { return dm_vkn_base() + off; }

// ---- counters (census) and red lines ----
enum { F_FILE, F_ENC, F_RAW, F_OTHER, F_N };
static const char *const k_fname[F_N] = {"SaveTextureToFile", "EncodeTexture", "ReadTextureTexels", "other"};
enum { N_OFF, N_SLOT, N_TEX, N_FMT, N_BPP, N_ENCFMT, N_BUSY, N_THREAD, N_N };   // "no data" reasons of the pre-checks
static const char *const k_nname[N_N] = {"switched off", "slot 222 not repointed", "texture unknown to the backend", "format not readable",
                                         "NVN texel size != engine bpp", "format the engine's encoder traps on", "presented recording open", "not the recording thread"};
enum { RL_NODATA, RL_SLOT, RL_N };
static struct {
    uint64_t calls[F_N], served[F_N], nodata[N_N];
    uint64_t regions[F_N], bytes[F_N], wait_ns[F_N], slot_fail[F_N], slot_unknown, slot_view;
    uint32_t red[RL_N];
} g_rs;
static int red_ok(int k) { return g_rs.red[k]++ < 16; }

static int g_read_on;
static void *g_slot_stub;      // the host-stack stub of rd_slot (made at registration; NULL = read-back off)
static int g_fn = F_OTHER;     // which reader is running (the recording thread only; set around the original)

// ---- slot 222: nvnTextureReadTexels(const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *{x, y, z, w, h, d}, void *) ----
VKN_HOOK rd_slot(VKN_A) {
    VKN_UNUSED;
    const int f = g_fn;
    const uint64_t obj = a0, view = a1, region = a2, dst = a3;
    vkn_tex *t = obj ? vkn_tex_by_cookie(obj) : NULL;
    if (!t || !region || !dst) {
        g_rs.slot_unknown++;
        if (red_ok(RL_SLOT)) vkn_logf(RB_RED "read: ReadTexels(object %#llx, region %#llx, buffer %#llx) from %s: %s: buffer left as it is", (unsigned long long)obj,
                                       (unsigned long long)region, (unsigned long long)dst, k_fname[f], t ? "null argument" : "object unknown to the backend");
        return 0;
    }
    uint32_t level = 0, layer = 0;
    if (view) {   // (the engine passes NULL; a view's base level / first layer as HLE NVN keeps them)
        const view_t *vw = (const view_t *)(uintptr_t)view;
        if (vw->set_levels) level = (uint32_t)vw->base_level;
        if (vw->set_layers) layer = (uint32_t)vw->min_layer;
        g_rs.slot_view++;
    }
    const int32_t x = (int32_t)vkn_rd32(region), y = (int32_t)vkn_rd32(region + 4), z = (int32_t)vkn_rd32(region + 8);
    const int32_t w = (int32_t)vkn_rd32(region + 12), h = (int32_t)vkn_rd32(region + 16), d = (int32_t)vkn_rd32(region + 20);
    const uint32_t nb = vkn_read_texel_bytes(t);
    const uint64_t img = (uint64_t)(w > 0 ? w : 0) * (uint64_t)(h > 0 ? h : 0) * nb;   // ReadTexels' tight image stride
    int rc = w > 0 && h > 0 && d > 0 && z >= 0 && nb ? VKN_READ_OK : VKN_READ_E_RECT;
    uint64_t ns = 0;
    for (int32_t k = 0; rc == VKN_READ_OK && k < d; k++) {   // (d slices / layers from z, one image each)
        uint64_t one = 0;
        rc = vkn_read_texels(t, level, layer + (uint32_t)z + (uint32_t)k, x, y, (uint32_t)w, (uint32_t)h, (void *)(uintptr_t)(dst + (uint64_t)k * img), 0, &one);
        ns += one;
    }
    g_rs.wait_ns[f] += ns;
    if (rc == VKN_READ_OK) { g_rs.regions[f]++; g_rs.bytes[f] += img * (uint64_t)d; return 0; }
    g_rs.slot_fail[f]++;
    // Black, not the allocator's garbage - only for the three readers, whose buffer size (w * h * engine bpp) the pre-check matched.
    const int zero = f != F_OTHER && nb && w > 0 && h > 0 && d > 0;
    if (zero) memset((void *)(uintptr_t)dst, 0, (size_t)(img * (uint64_t)d));
    if (red_ok(RL_SLOT)) vkn_logf(RB_RED "read: ReadTexels of texture %#llx (%ux%u NVN %#x) region %d,%d,%d %dx%dx%d from %s failed (%d): %s", (unsigned long long)t->handle,
                                   t->w, t->h, t->nvn_fmt, x, y, z, w, h, d, k_fname[f], rc, zero ? "zeros returned" : "buffer left as it is");
    return 0;
}

int dm_vkn_read_slot(unsigned slot_index, void **fn) {
    if (slot_index != S_READ_TEXELS || !g_slot_stub || !fn) return 0;
    *fn = g_slot_stub;
    return 1;
}
static int slot_in(void) {
    const uint64_t cur = __atomic_load_n((const uint64_t *)(const void *)(dm_vkn_base() + M_TABLE + 8u * S_READ_TEXELS), __ATOMIC_ACQUIRE);
    return g_slot_stub && cur == (uint64_t)(uintptr_t)g_slot_stub;
}

// ---- the three readers: pre-checks, then the engine's own function (its only command-stream NVN call is slot 222 = rd_slot) ----
static vkn_fn g_o_file, g_o_enc, g_o_raw;
static vkn_fn *const g_o_rb[3] = {&g_o_file, &g_o_enc, &g_o_raw};
static uint64_t nodata(int f, int why, uint64_t r, uint64_t h, const vkn_tex *t, const char *detail) {
    g_rs.nodata[why]++;
    if (red_ok(RL_NODATA)) vkn_logf(RB_RED "read: %s(r %#llx, texture %#llx, NVN format %#x): no data returned (%s%s%s)", k_fname[f], (unsigned long long)r, (unsigned long long)h,
                                     t ? t->nvn_fmt : 0u, k_nname[why], detail ? ": " : "", detail ? detail : "");
    return 0;
}
static inline __attribute__((always_inline)) uint64_t rb_pre(int f, VKN_A) {   // (inlined into each hook: no tail call there)
    const uint64_t r = a0, h = a1;
    g_rs.calls[f]++;
    if (!g_read_on) return nodata(f, N_OFF, r, h, NULL, NULL);
    const uint64_t rec = r ? vkn_rec(r, TEX_TAB, TEX_REC, h) : 0, obj = rec ? vkn_rd64(rec + REC_OBJ) : 0;
    if (!obj) return (*g_o_rb[f])(VKN_F);   // stale handle / empty record: the original returns 0 before any NVN call
    if (!slot_in()) return nodata(f, N_SLOT, r, h, NULL, NULL);
    vkn_tex *t = vkn_tex_by_cookie(obj);
    if (!t) return nodata(f, N_TEX, r, h, NULL, NULL);
    const char *why = NULL;
    if (!vkn_read_supported(t, &why)) return nodata(f, N_FMT, r, h, t, why);
    uint64_t fi[4] = {0, 0, 0, 0}; ((fmt_info_fn)eng(FN_FMT_INFO))(fi, (int)vkn_rd32(rec + REC_EFMT));
    if (fi[2] != vkn_read_texel_bytes(t)) {
        char s[64]; snprintf(s, sizeof s, "engine format %u: %llu bytes, NVN %#x: %u", vkn_rd32(rec + REC_EFMT), (unsigned long long)fi[2], t->nvn_fmt, vkn_read_texel_bytes(t));
        return nodata(f, N_BPP, r, h, t, s);
    }
    if (f != F_RAW && t->nvn_fmt != NVN_RGBA8 && t->nvn_fmt != NVN_RGB8) return nodata(f, N_ENCFMT, r, h, t, NULL);
    if (vkn_cb() && vkn_rec_presented()) return nodata(f, N_BUSY, r, h, t, NULL);
    if (!vkn_read_now_ok()) return nodata(f, N_THREAD, r, h, t, NULL);
    g_fn = f;
    const uint64_t rc = (*g_o_rb[f])(VKN_F);
    g_fn = F_OTHER;
    g_rs.served[f]++;
    return rc;
}
VKN_HOOK rh_file(VKN_A) { return rb_pre(F_FILE, VKN_F); }   // 0x9291c0
VKN_HOOK rh_enc(VKN_A) { return rb_pre(F_ENC, VKN_F); }     // 0x9296d0
VKN_HOOK rh_raw(VKN_A) { return rb_pre(F_RAW, VKN_F); }     // 0x929880

// ---- census ----
static void rd_census(void) {
    vkn_read_stats s; vkn_read_stats_get(&s);
    char by[400]; int k = 0; by[0] = 0;
    for (int f = 0; f < F_N; f++) {
        if (!g_rs.calls[f] && !g_rs.regions[f] && !g_rs.slot_fail[f]) continue;
        k += snprintf(by + k, sizeof by - (size_t)k, " %s %llu calls / %llu served / %llu regions / %.1f KB / %.1f ms waited / %llu failed;", k_fname[f],
                      (unsigned long long)g_rs.calls[f], (unsigned long long)g_rs.served[f], (unsigned long long)g_rs.regions[f], g_rs.bytes[f] / 1024.0, g_rs.wait_ns[f] / 1e6,
                      (unsigned long long)g_rs.slot_fail[f]);
        if (k >= (int)sizeof by - 1) break;
    }
    char nd[300]; int m = 0; nd[0] = 0;
    for (int i = 0; i < N_N; i++) if (g_rs.nodata[i] && m < (int)sizeof nd - 64) m += snprintf(nd + m, sizeof nd - (size_t)m, " %s %llu,", k_nname[i], (unsigned long long)g_rs.nodata[i]);
    vkn_logf("read: %s%s; slot: %llu unknown objects, %llu views; backend: %llu reads, %.1f KB, %.1f ms waited (own recordings %llu, engine's %llu, never-written zeros %llu), "
             "errors tex %llu fmt %llu rect %llu busy %llu thread %llu cb %llu mem %llu submit %llu; no data:%s",
             g_read_on ? "on" : "off (DM_VK_NATIVE_READ=0)", k ? by : " no read-backs", (unsigned long long)g_rs.slot_unknown, (unsigned long long)g_rs.slot_view,
             (unsigned long long)s.reads, s.bytes / 1024.0, s.wait_ns / 1e6, (unsigned long long)s.own_rec, (unsigned long long)s.engine_rec, (unsigned long long)s.undefined_zero,
             (unsigned long long)s.e_tex, (unsigned long long)s.e_fmt, (unsigned long long)s.e_rect, (unsigned long long)s.e_busy, (unsigned long long)s.e_thread,
             (unsigned long long)s.e_cb, (unsigned long long)s.e_mem, (unsigned long long)s.e_submit, m ? nd : " none");
}

// ---- registration (install time; idempotent: called from the frame part's register until hle_dos2_vkn.c calls it itself) ----
static int sw_on(const char *name) { const char *e = getenv(name); return !(e && *e == '0'); }
void dm_vkn_read_register(void) {
    static int done;
    if (done || vkn_mode() != VKN_NATIVE) return;   // mode 2: the old path reads and plays (nothing here)
    done = 1;
    g_read_on = sw_on("DM_VK_NATIVE_READ");
    // First 4 instructions of each site (python <scratchpad>/vk/re/pro.py; none PC-relative, checked with capstone).
    static const uint32_t k_rb_file[4] = {0xd10303ff /* sub sp,sp,#0xc0 */, 0xa9066ffc, 0xa90767fa, 0xa9085ff8};
    static const uint32_t k_rb_enc[4] = {0xd102c3ff /* sub sp,sp,#0xb0 */, 0xa90667fa, 0xa9075ff8, 0xa90857f6};
    static const uint32_t k_rb_raw[4] = {0xd10143ff /* sub sp,sp,#0x50 */, 0xa90257f6, 0xa9034ff4, 0xa9047bfd};
    // (off: the hooks stay in and return "no data" - the stage-3 behaviour, so the engine never reaches HLE NVN's ReadTexels)
    dm_vkn_site(0x9291c0, k_rb_file, "SaveTextureToFile", (void *)rh_file, &g_o_file, VKN_M_NATIVE);
    dm_vkn_site(0x9296d0, k_rb_enc, "EncodeTexture", (void *)rh_enc, &g_o_enc, VKN_M_NATIVE);
    dm_vkn_site(0x929880, k_rb_raw, "ReadTextureTexels", (void *)rh_raw, &g_o_raw, VKN_M_NATIVE);
    if (g_read_on) {
        vkn_read_init();
        if (!(g_slot_stub = dm_vkn_stack_stub((void *)rd_slot))) vkn_logf(RB_RED "read: no host-stack stub for the ReadTexels slot: read-back returns no data");
    }
    vkn_logf("read: DM_VK_NATIVE_READ=%d (texture read-back through the ReadTexels slot; movie planes: the storage mirror)", g_read_on);
    dm_vkn_census(rd_census);
}
#endif
