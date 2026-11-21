// [DOS2] VK-NATIVE stage 2, part "textures and samplers" (dm_vkn_tex_register; the core is hle_dos2_vkn.c): every engine texture,
// render target, window texture and sampler gets its native Vulkan object (dm/gfx/vkn/vkn_tex.c) from the ENGINE's create call, in
// both modes (1 and 2): each hook calls the original first - it fills the engine's record and still does its NVN bookkeeping - and
// then builds the Vulkan object from the call's arguments and the engine record. main = main.raw of update v786432, build 4979b200;
// r = [main+0x2bbdc68]. Engine facts: docs/dos2/nr/VK-NATIVE-MAP-1.md sections 1.6, 2, 3.2, 4, 5 and MAP-3 (window, pool ids).
//   0x91e2a0 CreateTexture(r, handle, void **initData, w3 engine format, w4 w, w5 h, w6 depth, w7 mips; STACK s0 byte, s1 layers,
//            s2 MSAA code, s3 u16 flags, s4 u8 usage, s5 NVN flags (-1 = own pick), s6 NVN format (0 = by engine format), s7 A*)
//   0x91f2e0 CreateTextureFromImage(r, handle, image*, w3 bit0 sRGB)   (image: +0 engine format, +4 flags (bit1 cube), +8 NVN format
//            or -1, +0xc packaged layout (8 bytes; int -1 = linear data), +0x18 w, +0x20 h, +0x28 depth, +0x38 mips, +0x40 layers (i64),
//            +0x48 CPU pointer of its storage, +0x50 u32 storage size)
//   0x93c3a0 window textures (window: w +0x18, h +0x1c, u16 MSAA code +0x20, records at +0xf8 / +0x120, handles +0x68 / +0x70,
//            engine format +0x148)
//   0x91d630 DestroyTextureIfOld(dev, record (live or the pending copy), force) -> 1 = finalized now: the texture dies by its object
//   0x920460 ReduceTextureMips(r, handle, level): a temporary texture by CreateTexture, level-tail copy, records swapped
//   0x921e40 CreateSampler(r, filter, wrapU, wrapV, wrapW, maxAniso, compare) -> handle; 0x9239f0 DestroySampler(r, handle)
// Texture object (0x108, record +0): +0xc0 pool id of the whole texture, +0xc4..+0xdc per-layer / per-face pool ids (-1 none),
// +0x100 the default sampler's pool id. Sampler record (r+0x540, 0x20): +0x10 its pool id.
// Data pointers: initData (CreateTexture) and the image's level data (linear images) are read inside the hook, right after the
// original returned - the caller frees them after the call; packaged storage is the image's own memory (kept by the texture). The
// hooks run on the engine's loader threads and on the render thread (0x920460 from the window's begin): vkn_tex locks its tables.
// Stage 4 (mode 1, hle_dos2_vkn_create.c, DM_VK_NATIVE_CREATE): the originals these hooks call no longer reach HLE NVN - their
// builder / Initialize / pool registration / view / getter / WriteTexels entries are answered by the create part's stubs in the
// game's NVN table (same in-place object layout, no translator side effects); WriteTexels copies nothing, the data reaches the
// native image here from the engine arguments. The engine's own bookkeeping (pool-id bitmap, A at object +0xe0, records) is unchanged.
// Stage 4 (both modes, devices without BC and DM_GFX_SIM_GPU=mali / powervr): vkn_tex looks packaged BC1..BC3 2D textures up in the
// setup ASTC pack (vkn_astc.h; DM_VK_NATIVE_ASTC=0 = always decode), then the GPU routes (vkn_bcgpu.h: GPU decode + ASTC 4x4 encode,
// GPU decode to RGBA8), then the CPU decode; the census adds the lines "tex BC:" (textures and MB by storage, time on the creating
// threads, why the pack did not take them) and "tex BC gpu:" (the device's decisions, self-tests, GPU work recorded).
// Shared render-target storage (both modes): 0x913170 (renderer init, the only caller of 0x914bb0) reserves one piece of pool-3 memory
// per storage key (0x914bb0(r, &key, format, w, h): a 2D one-level description, its storage size / alignment, 0x932900 -> record A in
// the map r+0x18c8[key]); the map r+0x18d8 sends several target names to one key. The target pool 0x912700 sets r+0x18e8 = name and
// flag 0x2000 when the name is in r+0x18d8, and 0x91e2a0 then copies the key's record A into the texture object (+0xe0, 0x20 bytes)
// instead of allocating: SetStorage(A+0x18 block +0x50, A+8 offset). So every texture of one key is the same memory on the Switch
// (0x91d630 does not free A under flag 0x2000; the reservation is never re-made, also not on a window resize 0x912f70: the new
// generation lands on the same memory). The CreateTexture hook passes (block, offset) of such a texture as its storage identity to
// vkn_tex_create_shared, which makes textures of one identity and one shape ONE image (VK-NATIVE.md 3.3; the save-game thumbnail
// reads ScreenshotBackBuffer, which only the scaled back buffer of the same key is drawn into). Non-shared textures never take part:
// 0x932900 never hands out live memory twice, and the reservations are never freed.
// Not covered here: texture updates after creation (UI WriteTexels / CopyBufferToTexture, packets 0x2c / 0x2d: stage 3).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_tex.h"
#include "../gfx/vkn/vkn_astc.h"
#include "../gfx/vkn/vkn_bcgpu.h"
#include "../gfx/vkn/vkn_rt.h"   // [stage 5.5] the recording thread
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_stack.h"

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
static vkn_fn g_o_ct, g_o_cti, g_o_win, g_o_dt, g_o_red, g_o_cs, g_o_ds;
static uint32_t g_fail_logged, g_sfail_logged;

// Engine helpers called as they are (pure functions over the image / format; the guest image is host memory).
typedef uint64_t (*img_level_fn)(uint64_t img, int64_t level, int64_t layer);          // 0x941b00: CPU pointer of (level, layer) or 0
typedef uint64_t (*img_size_fn)(uint64_t img, uint64_t level0, int64_t nlevels, uint32_t fmt);   // 0x943050: bytes of nlevels levels
typedef void (*fmt_info_fn)(uint64_t out[4], int efmt);                                  // 0x91eea0: {name, components, bytes, compressed}
static void *eng(uint32_t off) { return dm_vkn_base() + off; }

static uint32_t msaa_samples(uint32_t code) {   // the engine's table at 0x1f586e0 ({2,1,4,1,1,1,8} by code - 2)
    static const uint8_t k[7] = {2, 1, 4, 1, 1, 1, 8};
    return code - 2u < 7u ? k[code - 2u] : 1u;
}
static void obj_ids(uint64_t obj, vkn_tex_desc *d) {
    d->pool_id = vkn_rd32(obj + 0xc0);
    for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) d->layer_ids[i] = vkn_rd32(obj + 0xc4 + 4ull * i);
    d->sampler_id = vkn_rd32(obj + 0x100);
    d->cookie = obj;
}
static void tex_failed(const char *what, uint64_t handle, const vkn_tex_desc *d, const char *why) {
    if (__atomic_fetch_add(&g_fail_logged, 1, __ATOMIC_RELAXED) >= 16) return;
    vkn_logf(RED "tex: %s %#llx failed (%s): engine fmt %u NVN %#x target %u %ux%ux%u mips %u layers %u samples %u class %u sRGB %u pool %#x", what,
             (unsigned long long)handle, why ? why : "?", d->efmt, d->nvn_fmt, d->nvn_target, d->w, d->h, d->depth, d->mips, d->layers, d->samples, d->cls, d->srgb, d->pool_id);
}

// ---- shared render-target storage ----
static uint64_t g_sh_seen, g_sh_norec;
static uint32_t g_sh_red;
// An engine name (a string object: the value points to its characters, the reference count sits at -0x10, 0x912700) as text for logs.
static void name_str(uint64_t v, char out[32]) {
    int i = 0;
    if (v >= 0x10000)
        for (; i < 31; i++) { const uint8_t c = vkn_rd8(v + (uint64_t)i); if (!c) break; if (c < 0x20 || c > 0x7e) { i = 0; break; } out[i] = (char)c; }
    if (i) out[i] = 0; else snprintf(out, 32, "%#llx", (unsigned long long)v);
}
// The storage key of a target name: the map r+0x18d8 as 0x91e2a0's shared path walks it (u32 count r+0x18dc, buckets r+0x18e0; hash =
// the name value, 0 -> 0x1f77c8c, modulo the count; a bucket holds the first node; node +0 next, +8 name, +0x10 storage key). 0 = none.
static uint64_t shared_key(uint64_t r, uint64_t name) {
    const uint32_t n = vkn_rd32(r + 0x18dc); const uint64_t b = vkn_rd64(r + 0x18e0);
    if (!n || !b) return 0;
    uint64_t p = vkn_rd64(b + ((name ? name : 0x1f77c8c) % n) * 8);
    for (int i = 0; p && i < 256; i++, p = vkn_rd64(p)) if (vkn_rd64(p + 8) == name) return vkn_rd64(p + 0x10);
    return 0;
}

// ---- CreateTexture ----
static void ct_post(uint64_t r, uint64_t handle, uint64_t data, uint32_t efmt, uint32_t w, uint32_t h, uint32_t depth, uint16_t flags, uint32_t msaa_code,
                    uint32_t layers_arg, uint32_t nvn_ov, uint64_t name) {
    const uint64_t rec = vkn_rec(r, 0x000, 0x28, handle); if (!rec) return;
    const uint64_t obj = vkn_rd64(rec); if (!obj) return;
    vkn_tex_desc d = {0};
    d.efmt = efmt; d.nvn_fmt = nvn_ov; d.srgb = flags & 1; d.storage = (flags & 0xc00) != 0;
    d.cls = flags & 0x300 ? VKN_TC_DEPTH : flags & 0x60 ? VKN_TC_COLOR : VKN_TC_SAMPLED;
    d.mips = vkn_rd8(rec + 0x18); d.layers = vkn_rd8(rec + 0x1b); if (!d.layers) d.layers = layers_arg ? layers_arg : 1;
    d.samples = msaa_samples(msaa_code);
    // the engine's target pick (0x91e2a0): 1D / 2D / 3D by which sizes are given, + array, MSAA, cube; any other combination is 2D
    const int cube = (flags & 2) != 0, ms = msaa_code != 1, arr = d.layers != 1;
    const unsigned key = (unsigned)(w && !depth && !h) | (unsigned)(w && h && !depth) << 1 | (unsigned)(w && h && depth) << 2 | (unsigned)arr << 4 | (unsigned)ms << 5 | (unsigned)cube << 3;
    switch (key) {
    case 0x01: d.nvn_target = 0; break; case 0x04: d.nvn_target = 2; break; case 0x0a: d.nvn_target = 8; break; case 0x11: d.nvn_target = 3; break;
    case 0x12: d.nvn_target = 4; break; case 0x1a: d.nvn_target = 9; break; case 0x22: d.nvn_target = 5; break; case 0x32: d.nvn_target = 6; break;
    default: d.nvn_target = 1; break;
    }
    const uint32_t nvn_depth = w && h && depth ? depth : 1;   // the builder's size z (the engine forces 1 unless all three are given)
    d.w = w; d.h = h; d.depth = d.nvn_target == 2 ? nvn_depth : 1;
    obj_ids(obj, &d);
    // initial data (not for targets): layer (texture depth) -> level -> face (6 for a cube), each w_m x h_m, the source advancing by
    // w_m * h_m * the engine's bytes per pixel (0x91eea0 +0x10; for block formats that is bytes per block - the engine's own rule)
    vkn_tex_region *reg = NULL; uint32_t nreg = 0;
    if (!(flags & 0x360) && data && d.mips) {
        uint64_t fi[4] = {0, 0, 0, 0}; ((fmt_info_fn)eng(0x91eea0))(fi, (int)efmt);
        const uint64_t bpp = fi[2]; const uint32_t faces = cube ? 6 : 1;
        reg = (vkn_tex_region *)malloc(sizeof *reg * (size_t)nvn_depth * d.mips * faces);
        uint64_t p = data;
        for (uint32_t l = 0; reg && l < nvn_depth; l++)
            for (uint32_t m = 0; m < d.mips; m++) {
                const uint32_t wm = w >> m ? w >> m : 1, hm = h >> m ? h >> m : 1;
                for (uint32_t f = 0; f < faces; f++, p += (uint64_t)wm * hm * bpp) {
                    if (d.nvn_target == 2 && l >= (depth >> m ? depth >> m : 1)) continue;   // a 3D slice beyond this level's depth
                    reg[nreg++] = (vkn_tex_region){.level = m, .layer = d.nvn_target == 2 ? 0 : l + f, .z = d.nvn_target == 2 ? l : 0, .w = wm, .h = hm, .d = 1,
                                                   .src = (const void *)(uintptr_t)p};
                }
            }
    }
    const vkn_tex_src src = {.kind = nreg ? VKN_SRC_REGIONS : VKN_SRC_NONE, .reg = reg, .nreg = nreg};
    const char *why = NULL;
    // flag 0x2000 (record +0x14, 0x91e2a0): the memory is the storage key's reservation, copied into A at the object's +0xe0 (+0xe8
    // offset in the pool, +0xf0 u32 size, +0xf6 u8 pool kind = valid, +0xf8 pool block): (block, offset) names the memory
    vkn_tex_share sh, *shp = NULL; char nm[32], km[32];
    // DM_VK_NATIVE_SHARED=0: every target its own image, as before this part (phone bisect).
    static int sh_on = -1;
    if (sh_on < 0) { const char *e = getenv("DM_VK_NATIVE_SHARED"); sh_on = !(e && *e == '0'); }
    if ((flags & 0x2000) && sh_on) {
        __atomic_add_fetch(&g_sh_seen, 1, __ATOMIC_RELAXED);
        const uint64_t blk = vkn_rd64(obj + 0xf8), off = vkn_rd64(obj + 0xe8);
        name_str(name, nm); name_str(shared_key(r, name), km);
        if (vkn_rd8(obj + 0xf6) && blk) {
            memset(&sh, 0, sizeof sh);
            sh.block = blk; sh.offset = off; sh.bytes = vkn_rd32(obj + 0xf0); sh.name = nm; sh.key = km;
            shp = &sh;
        } else {
            __atomic_add_fetch(&g_sh_norec, 1, __ATOMIC_RELAXED);
            if (__atomic_fetch_add(&g_sh_red, 1, __ATOMIC_RELAXED) < 16)
                vkn_logf(RED "tex: shared target %#llx (%s, key %s, %ux%u engine fmt %u) has no storage record (A +0x16 %u, block %#llx): own image", (unsigned long long)handle,
                         nm, km, w, h, efmt, (unsigned)vkn_rd8(obj + 0xf6), (unsigned long long)blk);
        }
    }
    if (!(shp ? vkn_tex_create_shared(handle, &d, &src, shp, &why) : vkn_tex_create(handle, &d, &src, &why)))
        tex_failed(data && nreg ? "CreateTexture (with data)" : "CreateTexture", handle, &d, why);
    free(reg);
}
VKN_HOOK vkn_h_ct(VKN_A) {
    const uint64_t data = a2 ? vkn_rd64(a2) : 0;   // *initData
    const uint64_t name = vkn_rd64(a0 + 0x18e8);    // the target name 0x912700 set (read by the shared path of 0x91e2a0)
    const uint64_t v = g_o_ct(VKN_F);
    ct_post(a0, a1, data, (uint32_t)a3, (uint32_t)a4, (uint32_t)a5, (uint32_t)a6, (uint16_t)s3, (uint32_t)s2, (uint32_t)s1, (uint32_t)s6, name);
    return v;
}

// ---- CreateTextureFromImage ----
static uint32_t image_nvn(int32_t f, int srgb, uint32_t efmt) {   // 0x91f2e0: the image's NVN format with the sRGB pairs swapped
    switch (f) {
    case 0x25: return srgb ? 0x38 : 0x25;
    case 0x41: case 0x45: return srgb ? 0x45 : 0x41;
    case 0x42: case 0x46: return srgb ? 0x46 : 0x42;
    case 0x43: case 0x47: return srgb ? 0x47 : 0x43;
    case 0x44: case 0x48: return srgb ? 0x48 : 0x44;
    case 0x6d: return srgb ? 0x37 : 0x6d;
    case -1: { uint32_t n = 0; vkn_tex_engine_format(efmt, srgb, &n); return n; }
    default: return (uint32_t)f;
    }
}
VKN_HOOK vkn_h_cti(VKN_A) {
    const uint64_t img = a2;
    const uint64_t v = g_o_cti(VKN_F);   // (the image stays valid until the caller frees it after this call)
    if (!img) return v;
    const uint64_t rec = vkn_rec(a0, 0x000, 0x28, a1); const uint64_t obj = rec ? vkn_rd64(rec) : 0;
    if (!obj) return v;
    const uint32_t efmt = vkn_rd32(img), iflags = vkn_rd32(img + 4); const int32_t inv = (int32_t)vkn_rd32(img + 8), lay = (int32_t)vkn_rd32(img + 0xc);
    const int64_t w = (int64_t)vkn_rd64(img + 0x18), h = (int64_t)vkn_rd64(img + 0x20), dp = (int64_t)vkn_rd64(img + 0x28), mips = (int64_t)vkn_rd64(img + 0x38),
                  layers = (int64_t)vkn_rd64(img + 0x40);
    const int cube = (iflags & 2) != 0, srgb = (int)(a3 & 1);
    vkn_tex_desc d = {0};
    d.efmt = efmt; d.srgb = (uint8_t)srgb; d.cls = VKN_TC_SAMPLED; d.samples = 1;
    d.nvn_fmt = image_nvn(inv, srgb, efmt);
    d.nvn_target = cube ? 8 : dp >= 2 ? 2 : (dp == 1 && h == 1) ? 0 : 1;
    d.w = (uint32_t)w; d.h = (uint32_t)h; d.depth = d.nvn_target == 2 ? (uint32_t)dp : 1; d.mips = (uint32_t)(mips > 0 ? mips : 1); d.layers = 1;
    obj_ids(obj, &d);
    const uint32_t nvn_depth = (uint32_t)(layers > dp ? layers : dp);   // SetSize3D(w, h, max(layers, depth))
    vkn_tex_src src = {.kind = VKN_SRC_NONE};
    vkn_tex_region reg[16 * 6]; uint32_t nreg = 0;
    if (lay != -1) {   // packaged: Switch block-linear storage in the image's own memory (no WriteTexels on the engine side)
        src.kind = VKN_SRC_PACKAGED; src.storage = (const void *)(uintptr_t)vkn_rd64(img + 0x48); src.storage_bytes = vkn_rd32(img + 0x50);
        src.pkg_block_h = (int)vkn_rd8(img + 0xc) + 1; src.nvn_depth = nvn_depth ? nvn_depth : 1;
    } else {
        // linear: level pointers by 0x941b00 (layer 0: a 2D / 3D / cube image has one layer, 0x941b00 strides whole chains by layer);
        // cube faces 1..5 at k * (0x943050(image, 0, mips, format) / 6). [deviation] a 3D level is uploaded whole (the engine writes
        // slice 0 of each level only: region depth 1).
        const uint64_t fstride = cube ? ((img_size_fn)eng(0x943050))(img, 0, (int64_t)d.mips, efmt) / 6 : 0;
        for (uint32_t m = 0; m < d.mips && m < 16; m++) {
            const uint64_t p = ((img_level_fn)eng(0x941b00))(img, m, 0);
            if (!p) continue;
            const uint32_t wm = d.w >> m ? d.w >> m : 1, hm = d.h >> m ? d.h >> m : 1, dm = d.depth >> m ? d.depth >> m : 1;
            for (uint32_t k = 0; k < (cube ? 6u : 1u); k++)
                reg[nreg++] = (vkn_tex_region){.level = m, .layer = k, .z = 0, .w = wm, .h = hm, .d = d.nvn_target == 2 ? dm : 1, .src = (const void *)(uintptr_t)(p + k * fstride)};
        }
        if (nreg) { src.kind = VKN_SRC_REGIONS; src.reg = reg; src.nreg = nreg; }
    }
    const char *why = NULL;
    if (!vkn_tex_create(a1, &d, &src, &why)) tex_failed(lay != -1 ? "CreateTextureFromImage (packaged)" : "CreateTextureFromImage (linear)", a1, &d, why);
    return v;
}

// ---- window textures (two colour targets, engine handles at window +0x68 / +0x70) ----
VKN_HOOK vkn_h_win(VKN_A) {
    const uint64_t win = a0;
    const uint64_t old0 = vkn_rd64(win + 0x68), old1 = vkn_rd64(win + 0x70);
    const uint64_t v = g_o_win(VKN_F);
    const uint64_t hd[2] = {vkn_rd64(win + 0x68), vkn_rd64(win + 0x70)}, ob[2] = {vkn_rd64(win + 0xf8), vkn_rd64(win + 0x120)};
    if (old0 && old0 != hd[0] && old0 != hd[1]) vkn_tex_destroy_handle(old0);   // the engine replaced its window textures
    if (old1 && old1 != hd[0] && old1 != hd[1]) vkn_tex_destroy_handle(old1);
    for (int i = 0; i < 2; i++) {
        if (!hd[i] || !ob[i]) continue;
        vkn_tex_desc d = {0};
        d.efmt = vkn_rd32(win + 0x148); d.cls = VKN_TC_WINDOW; d.nvn_target = 1; d.w = vkn_rd32(win + 0x18); d.h = vkn_rd32(win + 0x1c); d.depth = 1; d.mips = 1;
        d.layers = 1; const uint32_t code = vkn_rd16(win + 0x20); d.samples = code > 15 ? 8 : msaa_samples(code);   // (0x93c3a0: codes above 15 -> 8)
        obj_ids(ob[i], &d);
        for (int k = 0; k < VKN_TEX_LAYER_IDS; k++) d.layer_ids[k] = ~0u;   // (the window object sets no per-layer ids)
        const char *why = NULL;
        if (!vkn_tex_create(hd[i], &d, NULL, &why)) tex_failed("window texture", hd[i], &d, why);
    }
    return v;
}

// ---- destroy: by the texture object of the record (live record or the pending copy) ----
void dm_vkn_glue_forget(uint64_t obj) __attribute__((weak));   // hle_dos2_vkn_glue.c
VKN_HOOK vkn_h_dt(VKN_A) {
    const uint64_t obj = a1 ? vkn_rd64(a1) : 0;   // before: the original clears the record
    const uint64_t v = g_o_dt(VKN_F);
    if ((v & 1) && obj) {   // (audit U3: its storage note goes too)
        // [stage 5.5] the drain 0x91ad20 calls this for every pending texture, every frame: the sync only when one really goes
        static vkn_rt_site site = VKN_RT_SITE("DestroyTextureIfOld (destroyed)");
        vkn_rt_own_begin(&site);
        if (dm_vkn_glue_forget) dm_vkn_glue_forget(obj);
        vkn_tex_destroy_cookie(obj);
        vkn_rt_own_end();
    }
    return v;
}

// ---- mip reduction: the handle's record now holds the smaller texture the call made under a temporary handle ----
VKN_HOOK vkn_h_red(VKN_A) {
    const uint64_t rec0 = vkn_rec(a0, 0x000, 0x28, a1), old = rec0 ? vkn_rd64(rec0) : 0;
    const uint64_t v = g_o_red(VKN_F);
    const uint64_t rec1 = vkn_rec(a0, 0x000, 0x28, a1), now = rec1 ? vkn_rd64(rec1) : 0;
    if (old && now && old != now && vkn_tex_swap_reduced(a1, old, now) == -1) {
        static uint32_t told; if (told++ < 8) vkn_logf(RED "tex: mip reduction of %#llx: texture %s unknown", (unsigned long long)a1, vkn_tex_by_cookie(now) ? "before" : "after");
    }
    return v;   // (deferred calls - r+0x18c0 clear - only queue: nothing changed)
}

// ---- samplers ----
VKN_HOOK vkn_h_cs(VKN_A) {
    const uint64_t v = g_o_cs(VKN_F);
    const uint64_t rec = vkn_rec(a0, 0x540, 0x20, v);
    if (!rec) return v;
    const uint32_t id = vkn_rd32(rec + 0x10);
    if (vkn_sampler_create(v, id, (uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)a4, (uint32_t)a5, (uint8_t)a6) && __atomic_fetch_add(&g_sfail_logged, 1, __ATOMIC_RELAXED) < 16)
        vkn_logf(RED "tex: CreateSampler %#llx failed: filter %u wrap %u/%u/%u aniso %u compare %u pool %#x", (unsigned long long)v, (unsigned)(uint8_t)a1, (unsigned)(uint8_t)a2,
                 (unsigned)(uint8_t)a3, (unsigned)(uint8_t)a4, (unsigned)a5, (unsigned)(uint8_t)a6, id);
    return v;
}
VKN_HOOK vkn_h_ds(VKN_A) {
    const uint64_t v = g_o_ds(VKN_F);
    vkn_sampler_destroy(a1);
    return v;
}

// ---- census ----
static void tex_census(void) {
    vkn_tex_stats s; vkn_tex_get_stats(&s);
    const double mb = 1048576.0;
    vkn_logf("tex: alive sampled %u / colour %u / depth %u / window %u (%u waiting for their destroy), images %.1f / %.1f / %.1f / %.1f MB, created %llu destroyed %llu, "
             "uploads %llu (%.1f MB, %.0f ms CPU), deswizzled %llu, BC decoded %llu, RGB widened %llu, mip swaps %llu; samplers %u (made %llu); failed: textures %llu, samplers %llu",
             s.alive[VKN_TC_SAMPLED], s.alive[VKN_TC_COLOR], s.alive[VKN_TC_DEPTH], s.alive[VKN_TC_WINDOW], s.pending, s.bytes[VKN_TC_SAMPLED] / mb, s.bytes[VKN_TC_COLOR] / mb,
             s.bytes[VKN_TC_DEPTH] / mb, s.bytes[VKN_TC_WINDOW] / mb, (unsigned long long)s.created, (unsigned long long)s.destroyed, (unsigned long long)s.uploads, s.upload_bytes / mb,
             s.upload_ns / 1e6, (unsigned long long)s.deswizzled, (unsigned long long)s.decoded, (unsigned long long)s.widened, (unsigned long long)s.swaps, s.samplers,
             (unsigned long long)s.samplers_made, (unsigned long long)s.failed, (unsigned long long)s.samplers_failed);
    // stage 4: BC textures by storage (native BC / ASTC from the setup pack / GPU decode + ASTC 4x4 encode / GPU decode to RGBA8 / CPU
    // decode): alive (MB), made, creation time on the creating threads, staged MB; why the pack did not take the others; the pack
    uint64_t bcm = 0; for (int k = 0; k < VKN_BC_N; k++) bcm += s.bc_made[k];
    if (bcm) {
        vkn_astc_stats a; vkn_astc_get_stats(&a);
        static const char *const k_state[4] = {"not opened", "ready", "none", "unreadable"};
        static const char *const k_kind[VKN_BC_N] = {"native", "ASTC pack", "CPU-decoded", "GPU ASTC", "GPU RGBA8"};
        char kinds[640]; size_t at = 0; kinds[0] = 0;
        for (int k = 0; k < VKN_BC_N && at + 120 < sizeof kinds; k++)
            at += (size_t)snprintf(kinds + at, sizeof kinds - at, "%s%s %u (%.1f MB) made %llu, %.0f ms, staged %.1f MB", k ? " / " : "", k_kind[k], s.bc_alive[k],
                                   s.bc_bytes[k] / mb, (unsigned long long)s.bc_made[k], s.bc_ns[k] / 1e6, s.bc_upload[k] / mb);
        vkn_logf("tex BC: alive %s; not from the pack because: no pack %llu, "
                 "BC4-BC7 %llu, not packaged %llu, not 2D one layer %llu, pack has no such shape %llu / bytes %llu, block size %llu; mip tails re-made %llu; "
                 "pack %s (%u entries, %.1f MB): lookups %llu, hits %llu, hashed %.1f MB in %.1f ms, read %llu (%.1f MB in %.1f ms), read errors %llu, "
                 "zero blocks fixed %llu; pack path %.1f ms",
                 kinds, (unsigned long long)s.dec_why[VKN_DW_OFF], (unsigned long long)s.dec_why[VKN_DW_FMT],
                 (unsigned long long)s.dec_why[VKN_DW_SRC], (unsigned long long)s.dec_why[VKN_DW_SHAPE], (unsigned long long)s.dec_why[VKN_DW_MISS_SHAPE],
                 (unsigned long long)s.dec_why[VKN_DW_MISS_KEY], (unsigned long long)s.dec_why[VKN_DW_BLOCK], (unsigned long long)s.pack_tail,
                 a.state >= 0 && a.state < 4 ? k_state[a.state] : "?", a.entries, a.pack_bytes / mb, (unsigned long long)a.lookups, (unsigned long long)a.hits,
                 a.hash_bytes / mb, a.hash_ns / 1e6, (unsigned long long)a.reads, a.read_bytes / mb, a.read_ns / 1e6, (unsigned long long)a.read_fails,
                 (unsigned long long)a.zero_fixed, s.pack_ns / 1e6);
        vkn_bcgpu_stats g; vkn_bcgpu_get_stats(&g);
        if (g.ready) {
            static const char *const k_why[VKN_BCGW_N] = {"DM_GFX_BC_DECODE=cpu", "no doors", "BC4/BC5/BC6H", "3D/1D/MS", "device", "pipeline/self-test"};
            static const char *const k_kept[VKN_BCAK_N] = {"DM_GFX_BC_ASTC=0", "auto on Qualcomm", "ASTC features", "encoder", "family", "shape", "image props"};
            char why[256], kept[256]; size_t wa = 0, ka = 0; why[0] = kept[0] = 0;
            for (int k = 0; k < VKN_BCGW_N; k++) if (g.why[k]) wa += (size_t)snprintf(why + wa, sizeof why - wa, " %s %llu", k_why[k], (unsigned long long)g.why[k]);
            for (int k = 0; k < VKN_BCAK_N; k++) if (g.kept[k]) ka += (size_t)snprintf(kept + ka, sizeof kept - ka, " %s %llu", k_kept[k], (unsigned long long)g.kept[k]);
            vkn_logf("tex BC gpu: decode %s (self-test ok %#x bad %#x by NVN format - 0x41), ASTC encode %s%s%s; routed ASTC %llu / RGBA8 %llu / CPU %llu (CPU because:%s; "
                     "RGBA8 because:%s); jobs %llu, batches %llu (decode %llu, encode %llu dispatches), blocks %llu, BC in %.1f MB, copies %llu, recorded %llu; "
                     "job build %.1f ms, device set-up + self-tests %.1f ms; scratch %.1f MB",
                     g.ready > 0 ? "on" : "off", g.kat_ok, g.kat_bad, g.astc > 0 ? "on" : g.astc < 0 ? "off" : "undecided", g.line[0] ? ": " : "", g.line,
                     (unsigned long long)g.routed[VKN_BCG_ASTC], (unsigned long long)g.routed[VKN_BCG_RGBA8], (unsigned long long)g.routed[VKN_BCG_NONE],
                     wa ? why : " -", ka ? kept : " -", (unsigned long long)g.jobs, (unsigned long long)g.batches, (unsigned long long)g.dec_dispatch,
                     (unsigned long long)g.enc_dispatch, (unsigned long long)g.blocks, g.in_bytes / mb, (unsigned long long)g.copies, (unsigned long long)g.recorded,
                     g.build_ns / 1e6, g.prep_ns / 1e6, g.scratch_bytes / mb);
        }
    }
    // shared render-target storage: identities with an image now, images on them, engine textures attached to another one's image now;
    // so far: shared creations seen, without a storage record, new images, attached, kept separate by reason, names gone, hand-overs
    if (g_sh_seen)
        vkn_logf("tex shared: storages %u, images %u, aliases attached now %u; creations %llu (no record %llu): new image %llu, attached %llu, kept separate "
                 "(size %llu, format %llu, shape %llu, usage %llu, data %llu, views %llu); names destroyed %llu, owner hand-overs %llu",
                 s.sh_storages, s.sh_images, s.sh_aliases, (unsigned long long)g_sh_seen, (unsigned long long)g_sh_norec, (unsigned long long)s.sh_made,
                 (unsigned long long)s.sh_attached, (unsigned long long)s.sh_kept[VKN_AL_SIZE], (unsigned long long)s.sh_kept[VKN_AL_FORMAT],
                 (unsigned long long)s.sh_kept[VKN_AL_SHAPE], (unsigned long long)s.sh_kept[VKN_AL_USAGE], (unsigned long long)s.sh_kept[VKN_AL_DATA],
                 (unsigned long long)s.sh_kept[VKN_AL_VIEWS], (unsigned long long)s.sh_detached, (unsigned long long)s.sh_handover);
    if (s.failed || s.samplers_failed) vkn_logf(RED "tex: %llu texture / %llu sampler creations failed so far (the first 16 of each are logged)", (unsigned long long)s.failed,
                                                (unsigned long long)s.samplers_failed);
}

void dm_vkn_tex_register(void);
void dm_vkn_create_register(void) __attribute__((weak));   // hle_dos2_vkn_create.c: creation-time NVN entries (mode 1)
void dm_vkn_tex_register(void) {
    if (dm_vkn_create_register) dm_vkn_create_register();
    static const uint32_t k_ct[4] = {0xd105c3ff /* sub sp,sp,#0x170 */, 0xa9116ffc, 0xa91267fa, 0xa9135ff8};
    static const uint32_t k_cti[4] = {0xd10503ff /* sub sp,sp,#0x140 */, 0xa90e6ffc, 0xa90f67fa, 0xa9105ff8};
    static const uint32_t k_win[4] = {0xd10403ff /* sub sp,sp,#0x100 */, 0xa90a6ffc, 0xa90b67fa, 0xa90c5ff8};
    static const uint32_t k_dt[4] = {0xf81e0ff3 /* str x19,[sp,#-0x20]! */, 0xa9017bfd, 0x910043fd, 0xf9400429 /* ldr x9,[x1,#8] */};
    static const uint32_t k_red[4] = {0xd104c3ff /* sub sp,sp,#0x130 */, 0xa90d6ffc, 0xa90e67fa, 0xa90f5ff8};
    static const uint32_t k_cs[4] = {0xd103c3ff /* sub sp,sp,#0xf0 */, 0xa90a67fa, 0xa90b5ff8, 0xa90c57f6};
    static const uint32_t k_ds[4] = {0xf81d0ff5 /* str x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    dm_vkn_site(0x91e2a0, k_ct, "CreateTexture", (void *)vkn_h_ct, &g_o_ct, VKN_M_BOTH);
    dm_vkn_site(0x91f2e0, k_cti, "CreateTextureFromImage", (void *)vkn_h_cti, &g_o_cti, VKN_M_BOTH);
    dm_vkn_site(0x93c3a0, k_win, "window textures", (void *)vkn_h_win, &g_o_win, VKN_M_BOTH);
    dm_vkn_site(0x91d630, k_dt, "DestroyTextureIfOld", (void *)vkn_h_dt, &g_o_dt, VKN_M_BOTH);
    dm_vkn_stack_flag((void *)vkn_h_dt, DM_VKN_SF_ENC);   // [stage 5.5] syncs itself, only for a destroy (the original's NVN entries: the create part's, ENC)
    dm_vkn_site(0x920460, k_red, "ReduceTextureMips", (void *)vkn_h_red, &g_o_red, VKN_M_BOTH);
    dm_vkn_site(0x921e40, k_cs, "CreateSampler", (void *)vkn_h_cs, &g_o_cs, VKN_M_BOTH);
    dm_vkn_site(0x9239f0, k_ds, "DestroySampler", (void *)vkn_h_ds, &g_o_ds, VKN_M_BOTH);
    dm_vkn_census(tex_census);
}
#endif
