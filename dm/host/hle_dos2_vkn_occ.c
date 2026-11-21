// [DOS2] VK-NATIVE stage 5 technique A, piece A-1 (docs/dos2/nr/VK-OCCLUSION-ALT.md 4.3, section 8): the engine object's world bounds
// and the pair -> object link, handed to dm/gfx/vkn/vkn_occ.c. Registered only for DM_VK_NATIVE=1 with DM_VK_NATIVE_COND 5 / 6.
// main = main.raw of update v786432, build 4979b200; r = [main+0x2bbdc68], dev = [r+0xb70]. Facts (decompilations of 0xb0c570,
// 0xb0df20, 0xb0ea00, 0x8e40b0, 0x8e3710, 0x8e6dd0, 0x8e60f0, 0x90cbb0, 0xb238e0, 0xb867d0, 0xa4b400, 0xa4be10, 0xa4c030, 0xa4b090
// and the disassembly of 0xb0c570..0xb0c620):
//   The visitor. 0xb0df20 builds a render stage (0x38c0 bytes, vtable 0x299af58): +0x20 = 0xb0c570 (the "object passed culling"
//     callback), +0x28 = 0xb0db10, +0x30 = 0xb0dd00, an embedded object at +0x3850 (vtable 0x299b060) with +0x3860 = the stage. Its
//     slot 4, 0xb0ea00, copies the culling query (0x1a8 bytes) to stage+0x36a0: query+0x10 = the culling camera ([stage+0x38] or
//     [[stage+0x10]+0x60]), +0x30 = the stage, +0x40 the extra planes (count byte +0x180), +0x24 the query type (1 = frustum); then
//     it starts the job 0x8e40b0 (stage+0x3680) on the job system: 0x8e3710 walks the scene's quad tree (node box +0x94..+0xa8, 4
//     children) and calls each node list's slot 0x118 (job threads, in parallel: hence the atomic allocation below).
//   The per-object test. The list functions 0x90cbb0 / 0xb238e0 / 0xb867d0 test the object with 0x8e6dd0(obj, query) and then call
//     (*(query+0x30)+0x20)(query+0x30, obj, [obj+0x210]) = 0xb0c570(stage, obj, x2). 0x8e6dd0 reads the object's bounding sphere
//     (centre obj+0x13c / 0x140 / 0x144, radius obj+0x148) against the query's planes and the camera's six planes (camera+0x220,
//     p- / n-vertex index tables +0x280..), and its AXIS-ALIGNED BOX {min x, y, z at obj+0x14c..0x154, max at obj+0x158..0x160} against
//     the camera's 8 frustum corners (camera+0x310..0x36c) by separating axes (0x8e60f0: the box corners are (p[0] or p[3], p[1] or
//     p[4], p[2] or p[5])). Both in world space (the corners and planes are). So the box at obj+0x14c is the bound the engine culls the
//     object's draws with in this frame: the premise "the box covers what the draws rasterise" is the engine's own (else objects would
//     vanish at the screen edge), checked by DM_VK_NATIVE_COND=6.
//   The camera: +0xe0 view, +0x120 its inverse, +0x160 projection, +0x1e0 view-projection (0xa4b400: proj x view through 0x8e2970,
//     then 0xa4c030 derives the planes at +0x220 from it: plane(y + w) = {e1 + e3, e5 + e7, e9 + e11, e13 + e15}, i.e. clip[c] =
//     sum_r p[r] * e[4 r + c], row 3 = w), +0x90 = "matrices stale" (the getters recompute while set).
//   The allocation (0xb0c5c0..0xb0c61c): flags w24 = [obj+0x1d8]; bits 19 / 12 / 13 / 14 return / branch away before it; then
//     (flags & ~0x40000) != 0 and bit 18: k = ldaxr/stlxr fetch_add [dev+0x25ac0]; k < 0x800: entry = [dev+0x25ab0 + 8 * [dev+0x31e0]]
//     + 16 k, entry+8 = [dev+0x51f0] (frame), obj+0x268 = entry, obj+0x270 = frame; k >= 0x800: obj+0x268 = 0, obj+0x270 = -1. The
//     pair packets 0x3d / 0x3e carry the entry (vkn_query_begin's key), the consumers take obj+0x268 when obj+0x270 is this frame.
// The hook: 0xb0c570 itself (job threads; the allocation site has no function boundary): the original first, then for an object that
// took a pair, the entry / frame it wrote, the box, the camera's view-projection -> vkn_occ_gather (lock-free, projected there). Two
// stages may gather one object concurrently: the entry read back may be the other's (vkn_occ_gather detects a second write of one
// entry in a frame); entry+8 must hold the frame. Flagged DM_VKN_SF_ENC: only engine reads and a lock-free table write, no sync with
// the recording thread (it may run on the engine thread when the job system runs a job there).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_occ.h"
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_stack.h"

enum { O_FLAGS = 0x1d8, O_ENTRY = 0x268, O_FRAME = 0x270, O_BOX = 0x14c, ST_CAMERA = 0x36b0, CAM_STALE = 0x90, CAM_VP = 0x1e0,
       R_DEV = 0xb70, DEV_HALF0 = 0x25ab0, DEV_HALF1 = 0x25ab8, DEV_FRAME = 0x51f0 };
static vkn_fn g_o_gather;
static uint64_t g_calls, g_took, g_noentry, g_badframe, g_stale_cam, g_nocam;

VKN_HOOK oc_h_gather(VKN_A) {   // 0xb0c570(stage, obj, x2): void
    const uint32_t fl = a1 ? vkn_rd32(a1 + O_FLAGS) : 0;
    const uint64_t rc = g_o_gather(VKN_F);
    __atomic_add_fetch(&g_calls, 1, __ATOMIC_RELAXED);
    if (!a0 || !a1 || (fl & (1u << 19 | 1u << 12 | 1u << 13 | 1u << 14)) || !(fl & 0x40000u) || !(fl & ~0x40000u)) return rc;
    __atomic_add_fetch(&g_took, 1, __ATOMIC_RELAXED);
    const uint64_t entry = vkn_rd64(a1 + O_ENTRY), fr = vkn_rd64(a1 + O_FRAME);
    if (!entry) { __atomic_add_fetch(&g_noentry, 1, __ATOMIC_RELAXED); return rc; }   // beyond 2048 pairs this frame
    if (vkn_rd64(entry + 8) != fr) { __atomic_add_fetch(&g_badframe, 1, __ATOMIC_RELAXED); return rc; }
    const uint64_t r = vkn_renderer(), dev = r ? vkn_rd64(r + R_DEV) : 0;
    if (dev) vkn_occ_gather_bases(vkn_rd64(dev + DEV_HALF0), vkn_rd64(dev + DEV_HALF1));
    float box[6], vp[16]; memcpy(box, (const void *)(uintptr_t)(a1 + O_BOX), sizeof box);
    const uint64_t cam = vkn_rd64(a0 + ST_CAMERA);
    const float *v = NULL;
    if (!cam) __atomic_add_fetch(&g_nocam, 1, __ATOMIC_RELAXED);
    else if (vkn_rd8(cam + CAM_STALE)) __atomic_add_fetch(&g_stale_cam, 1, __ATOMIC_RELAXED);   // matrices not recomputed yet: unknown
    else { memcpy(vp, (const void *)(uintptr_t)(cam + CAM_VP), sizeof vp); v = vp; }
    (void)vkn_occ_gather(entry, fr, a1, box, v);
    return rc;
}
static void oc_census(void) {
    vkn_logf("occ-a gather: calls %llu, objects that took a pair %llu, beyond 2048 %llu, entry frame mismatch %llu, no camera %llu, camera stale %llu",
             (unsigned long long)g_calls, (unsigned long long)g_took, (unsigned long long)g_noentry, (unsigned long long)g_badframe,
             (unsigned long long)g_nocam, (unsigned long long)g_stale_cam);
}
void dm_vkn_occ_register(void);
void dm_vkn_occ_register(void) {
    if (vkn_mode() != VKN_NATIVE) return;
    const char *qc = getenv("DM_VK_NATIVE_COND");
    const int cond = qc && *qc ? atoi(qc) : 0;
    if (cond != 5 && cond != 6) return;
    // First 4 instructions (python <scratchpad>/vk/re/pro.py b0c570; none PC-relative, checked with capstone).
    static const uint32_t k_gather[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa9026ffc, 0xa90367fa, 0xa9045ff8};
    dm_vkn_stack_flag((void *)oc_h_gather, DM_VKN_SF_ENC);
    dm_vkn_site(0xb0c570, k_gather, "ObjectGather", (void *)oc_h_gather, &g_o_gather, VKN_M_NATIVE);
    dm_vkn_census(oc_census);
}
#endif
