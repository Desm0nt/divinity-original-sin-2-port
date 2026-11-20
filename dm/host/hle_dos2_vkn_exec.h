// [DOS2] VK-NATIVE stage 3, packets and state functions (hle_dos2_vkn_exec.c): what the other host parts may call. Mode 1 only
// (in mode 2 nothing of it is installed and these functions are not called by this part). Every function runs on the thread that
// executes the engine's command lists (the renderer's state cache is that thread's); r = the renderer ([main+0x2bbdc68]).
#ifndef DM_HLE_DOS2_VKN_EXEC_H
#define DM_HLE_DOS2_VKN_EXEC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// main.c host_preload, the first line of the DOS2 block: with DM_VK_NATIVE=1 every switch of the old native / NB / NR / NRN layers
// is forced to 0 (their installers then do nothing; see the table in hle_dos2_vkn_exec.c).
void dm_dos2_vkn_pre(void);

// The scissor the NVN stream would have at a draw, as x, y, w, h. The engine keeps two caches: viewport r+0x171c {w, h, x, y} and
// scissor r+0x172c {left, top, right, bottom}; but EVERY viewport setter (packet 0x12, SetRenderTargets 0x9279a0, the depth clear
// 0x928be0, 0xab50e0 / 0xab5390 / 0xb15090 ...) also calls nvnCommandBufferSetScissor with the viewport rect WITHOUT writing r+0x172c,
// and only packet 0x13 writes both. So the scissor in effect is the viewport rect when a viewport change came after the last scissor
// change, else the scissor cache. Returns 1 = the viewport rect, 0 = the scissor cache. The draw part takes vkn_draw_in.scissor here.
int dm_vkn_exec_scissor(uint64_t r, int32_t out[4]);
// A viewport setter outside this file changed r+0x171c (the pass part after 0x9279a0 / 0x928be0 wrote it): the scissor is now the
// viewport rect. Without the call the rule above still holds unless the viewport returns to the very rect it had at the last scissor
// change (A -> B -> A).
void dm_vkn_exec_viewport_note(void);

// The engine's state functions, NVN-free (they write exactly the renderer cache fields and frame stamps the originals write), for the
// later waves (immediate draws 0x92f730..0x930e90, the stage functions' immediate branches):
void dm_vkn_exec_shader(uint64_t r, uint64_t shader);                  // BindShader 0x9283a0(r, &shader)
void dm_vkn_exec_blend(uint64_t r, uint64_t blend, uint32_t mask);     // ApplyBlend 0x9284e0(r, &blend, mask)
void dm_vkn_exec_depth(uint64_t r, uint64_t ds, uint32_t stencil_ref); // the depth-stencil part of packets 0x08 / 0x15
void dm_vkn_exec_raster(uint64_t r, uint64_t raster);                  // packet 0x07, SetRasterState 0x93ffc0's immediate branch
void dm_vkn_exec_vfmt(uint64_t r, uint64_t vfmt);                      // packet 0x0a
void dm_vkn_exec_vb(uint64_t r, int slot, uint64_t vb, uint64_t off);  // packet 0x0b (slot 0..7)
void dm_vkn_exec_ib(uint64_t r, uint64_t ib);                          // packet 0x0c
void dm_vkn_exec_state(uint64_t r, uint64_t block);                    // ApplyDrawState 0x92c3c0(r, block): packet 0x15 from +8 on
// DrawArrays the way packet 0x23 does it: the conditional-render fields r+0x185c / 0x185d / 0x1860 as the original, the draw itself
// (with the draw-time flushes) by the draw part's dm_vkn_draw_arrays. 0 = recorded, 1 = dropped.
int dm_vkn_exec_draw_arrays(uint64_t r, uint32_t first, uint32_t count);

#ifdef __cplusplus
}
#endif
#endif
