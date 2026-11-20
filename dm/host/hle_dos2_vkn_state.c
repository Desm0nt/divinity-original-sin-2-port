// [DOS2] VK-NATIVE front end, state part (DM_VK_NATIVE 1 and 2): the engine's state-object and vertex-format events go to
// dm/gfx/vkn/vkn_state.c. main = main.raw of update v786432, build 4979b200; r = [main+0x2bbdc68]. Arguments: VK-NATIVE-MAP-1.md §2.
// Every hook calls the function it sits on first (in both modes: until stage 4 the engine's own record is still built by it, and the
// engine reads it), then hands the plain arguments to the backend:
//   0x922530 CreateBlendState(r, srcRGB, dstRGB, srcA, dstA, alphaOp, flags) -> handle
//   0x922d10 CreateDepthStencilState(r, depthTest, depthWrite, depthFunc, stencilEnable, stencilMask, stencilFunc, depthFailOp;
//            stack: passOp) -> handle
//   0x923360 CreateRasterState(r, cull, fill, flagA, flagB, polygonOffset) -> handle
//   0x917100 CreateVertexFormat(r, elems, count, -, u64 *out): returns nothing; *out = the handle, an EXISTING one for equal elements
//            (byte compare of 6 x count under the table lock), so the put is idempotent
//   0x923c20 / 0x923e60 / 0x924080 DestroyBlend / DepthStencil / RasterState(r, handle by value)
// The state arguments are bytes in the engine (it compares and indexes with the low byte), so only the low byte is passed on.
// The polygon offset values (0x93d9f0: SetPolygonOffsetClamp(-2.5, -5.0, 0) every frame) are the backend's defaults; the frame
// piece may call vkn_state_set_depth_bias when it owns 0x93d9f0.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_state.h"
#include "hle_dos2_vkn.h"

static vkn_fn g_o_bs, g_o_ds, g_o_rs, g_o_vf, g_o_dbs, g_o_dds, g_o_drs;

VKN_HOOK vkn_h_bs(VKN_A) {   // CreateBlendState
    const uint64_t h = g_o_bs(VKN_F);
    if (h) vkn_blend_put(h, (uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)a4, (uint8_t)a5, (uint32_t)a6);
    return h;
}
VKN_HOOK vkn_h_ds(VKN_A) {   // CreateDepthStencilState (the 9th argument, passOp, is the first stack slot)
    const uint64_t h = g_o_ds(VKN_F);
    if (h) vkn_ds_put(h, (uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)a4, (uint8_t)a5, (uint8_t)a6, (uint8_t)a7, (uint8_t)s0);
    return h;
}
VKN_HOOK vkn_h_rs(VKN_A) {   // CreateRasterState
    const uint64_t h = g_o_rs(VKN_F);
    if (h) vkn_raster_put(h, (uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)a4, (uint32_t)a5);
    return h;
}
VKN_HOOK vkn_h_vf(VKN_A) {   // CreateVertexFormat: elements copied before the call (caller memory), the handle read after it
    uint8_t el[255 * 6]; const uint32_t n = (uint32_t)a2;
    const int ok = n <= 255 && (!n || a1);   // the engine keeps the count in a byte: more is not a real format
    if (ok && n) memcpy(el, (const void *)(uintptr_t)a1, (size_t)n * 6);
    const uint64_t rc = g_o_vf(VKN_F);
    uint64_t h = 0;
    if (a4) memcpy(&h, (const void *)(uintptr_t)a4, 8);
    if (h && ok) vkn_vfmt_put(h, el, n);
    else if (h) { static int noted; if (!noted++) vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91state: vertex format of %u elements: not taken", n); }
    return rc;
}
VKN_HOOK vkn_h_dbs(VKN_A) { const uint64_t rc = g_o_dbs(VKN_F); vkn_blend_kill(a1); return rc; }
VKN_HOOK vkn_h_dds(VKN_A) { const uint64_t rc = g_o_dds(VKN_F); vkn_ds_kill(a1); return rc; }
VKN_HOOK vkn_h_drs(VKN_A) { const uint64_t rc = g_o_drs(VKN_F); vkn_raster_kill(a1); return rc; }

static void vkn_state_census(void) {
    vkn_state_stats s; vkn_state_stats_get(&s);
    vkn_logf("state: blend %u ids / %u live, depth-stencil %u / %u, raster %u / %u, vertex formats %u / %u (%llu puts, %llu destroys, %llu rejected); "
             "passes %u (%u compatibility classes), framebuffers %u (%llu retired); pipelines %u + %u compute (hits %llu, built %llu, failed %llu, "
             "rewrite asks %llu, build %.1f ms)",
             s.blend_ids, s.blend_live, s.ds_ids, s.ds_live, s.raster_ids, s.raster_live, s.vfmt_ids, s.vfmt_live, (unsigned long long)s.puts,
             (unsigned long long)s.kills, (unsigned long long)s.bad, s.passes, s.compat, s.framebuffers, (unsigned long long)s.fb_retired, s.pipelines,
             s.compute, (unsigned long long)s.hits, (unsigned long long)s.misses, (unsigned long long)s.failures, (unsigned long long)s.rewrites,
             s.build_ns / 1e6);
}

void dm_vkn_state_register(void);
void dm_vkn_state_register(void) {
    // First 4 instructions of each site (python <scratchpad>/vk/re/pro.py; none PC-relative, checked with capstone).
    static const uint32_t k_bs[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa90367fa, 0xa9045ff8, 0xa90557f6};
    static const uint32_t k_ds[4] = {0xd10243ff /* sub sp,sp,#0x90 */, 0xa9036ffc, 0xa90467fa, 0xa9055ff8};
    static const uint32_t k_rs[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa901efff, 0xa90367fa, 0xa9045ff8};
    static const uint32_t k_vf[4] = {0xd10183ff /* sub sp,sp,#0x60 */, 0xa9025ff8, 0xa90357f6, 0xa9044ff4};
    static const uint32_t k_dbs[4] = {0xd100c3ff /* sub sp,sp,#0x30 */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    static const uint32_t k_dds[4] = {0xf81e0ff3 /* str x19,[sp,#-0x20]! */, 0xa9017bfd, 0x910043fd, 0x911b0013 /* add x19,x0,#0x6c0 */};
    static const uint32_t k_drs[4] = {0xd100c3ff /* sub sp,sp,#0x30 */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    dm_vkn_site(0x922530, k_bs, "CreateBlendState", (void *)vkn_h_bs, &g_o_bs, VKN_M_BOTH);
    dm_vkn_site(0x922d10, k_ds, "CreateDepthStencilState", (void *)vkn_h_ds, &g_o_ds, VKN_M_BOTH);
    dm_vkn_site(0x923360, k_rs, "CreateRasterState", (void *)vkn_h_rs, &g_o_rs, VKN_M_BOTH);
    dm_vkn_site(0x917100, k_vf, "CreateVertexFormat", (void *)vkn_h_vf, &g_o_vf, VKN_M_BOTH);
    dm_vkn_site(0x923c20, k_dbs, "DestroyBlendState", (void *)vkn_h_dbs, &g_o_dbs, VKN_M_BOTH);
    dm_vkn_site(0x923e60, k_dds, "DestroyDepthStencilState", (void *)vkn_h_dds, &g_o_dds, VKN_M_BOTH);
    dm_vkn_site(0x924080, k_drs, "DestroyRasterState", (void *)vkn_h_drs, &g_o_drs, VKN_M_BOTH);
    dm_vkn_census(vkn_state_census);
}
#endif
