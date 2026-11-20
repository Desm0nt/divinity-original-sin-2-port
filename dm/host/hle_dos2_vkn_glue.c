// [DOS2] VK-NATIVE stage 3: the few calls between the host parts that were written apart (each part declares the other side weak).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdint.h>
#include "hle_dos2_vkn_exec.h"
// The pass part reset NVN's viewport and scissor to a target's full size (0x9279a0 / 0x928be0 write the viewport cache but not the
// scissor cache): the exec part's "scissor in effect" must see it as a viewport event (hle_dos2_vkn_pass.h declares this weak).
void dm_vkn_on_nvn_viewport_scissor(int32_t x, int32_t y, int32_t w, int32_t h);
void dm_vkn_on_nvn_viewport_scissor(int32_t x, int32_t y, int32_t w, int32_t h) { (void)x; (void)y; (void)w; (void)h; dm_vkn_exec_viewport_note(); }

// ---- engine textures written through their mapped storage (mode 1) ----
// On an immediate command list (list+0x30 == 0: the render thread's own list) the engine updates a texture by copying rows straight
// into the NVN texture's mapped storage ([texture object + 0xe0], row pitch from nvnTextureGetStride): the fog-of-war / sight maps
// (0x14284c0, every frame), 0xff5500, the video planes (0xd1ada0), 0x9410f0. No packet, no renderer function: the native image never
// saw the data (phone runs V3_b..V3_j: the sight maps stayed zero, the post step that multiplies the scene by them wrote black, so
// the 3D scene was black under a correct HUD). Every such site asks nvnTextureGetStride right before its copy, on the render
// thread, and the copy is done before the next draw. So: the game's table slot of nvnTextureGetStride (main+0x2bbf560) is repointed
// to a function that answers through the old entry and notes the texture object; the next draw (dm_vkn_draw_fill) and the end of the
// frame mirror every noted texture from its storage into the native image (vkn_tex_update: recorded in order in the open recording).
#include <string.h>
#include "hle_dos2_vkn.h"
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_tex.h"
enum { STOR_SLOT_OFF = 0x2bbf560, STOR_MAX = 32 };
typedef uint64_t (*stor_fn)(uint64_t);
static stor_fn g_stor_orig;
static __thread struct { uint64_t obj; uint32_t stride; } t_stor[STOR_MAX];
static __thread uint32_t t_nstor;
static uint64_t g_stor_notes, g_stor_updates, g_stor_bytes, g_stor_unknown, g_stor_full;
static uint64_t stor_get_stride(uint64_t tex) {   // nvnTextureGetStride(const NVNtexture *): tex = the engine's texture object
    const uint64_t stride = g_stor_orig ? g_stor_orig(tex) : 0;
    if (tex && vkn_mode() == VKN_NATIVE) {
        uint32_t i = 0;
        while (i < t_nstor && t_stor[i].obj != tex) i++;
        if (i == t_nstor) { if (t_nstor < STOR_MAX) t_nstor++; else { i = STOR_MAX - 1; __atomic_add_fetch(&g_stor_full, 1, __ATOMIC_RELAXED); } }
        t_stor[i].obj = tex; t_stor[i].stride = (uint32_t)stride;
        __atomic_add_fetch(&g_stor_notes, 1, __ATOMIC_RELAXED);
    }
    return stride;
}
void dm_vkn_glue_census(void);
void dm_vkn_glue_repoint(void);
void dm_vkn_glue_repoint(void) {   // after the game filled its NVN table (called by the cmd part's repointing)
    uint64_t *slot = (uint64_t *)(void *)(dm_vkn_base() + STOR_SLOT_OFF);
    const uint64_t cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (!cur || cur == (uint64_t)(uintptr_t)stor_get_stride) return;
    g_stor_orig = (stor_fn)(uintptr_t)cur;
    __atomic_store_n(slot, (uint64_t)(uintptr_t)stor_get_stride, __ATOMIC_RELEASE);
    dm_vkn_census(dm_vkn_glue_census);
    vkn_logf("glue: nvnTextureGetStride (main+%#x) noted: textures the engine writes through their mapped storage are mirrored at the next draw", STOR_SLOT_OFF);
}
void dm_vkn_glue_flush_tex(void);
void dm_vkn_glue_flush_tex(void) {   // render thread: before a draw is built, and at the end of the frame
    if (!t_nstor) return;
    const uint32_t n = t_nstor; t_nstor = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t obj = t_stor[i].obj, src = vkn_rd64(obj + 0xe0);
        vkn_tex *t = vkn_tex_by_cookie(obj);
        if (!t || !src || !t_stor[i].stride) { g_stor_unknown++; continue; }
        if (!vkn_tex_update(t, 0, 0, 0, 0, t->w, t->h, (const void *)(uintptr_t)src, t_stor[i].stride)) { g_stor_updates++; g_stor_bytes += (uint64_t)t_stor[i].stride * t->h; }
        else g_stor_unknown++;
    }
}
void dm_vkn_glue_census(void);
void dm_vkn_glue_census(void) {
    vkn_logf("glue: storage-written textures: %llu notes, %llu mirrored (%.1f MB), %llu not mirrored (unknown texture / no storage / refused), %llu over the note list",
             (unsigned long long)g_stor_notes, (unsigned long long)g_stor_updates, g_stor_bytes / 1048576.0, (unsigned long long)g_stor_unknown, (unsigned long long)g_stor_full);
}
#endif
