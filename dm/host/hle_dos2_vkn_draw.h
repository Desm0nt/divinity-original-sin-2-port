// [DOS2] VK-NATIVE stage 3, draw part of the front end (hle_dos2_vkn_draw.c): what other parts call (mode 1 only).
#ifndef DM_HLE_DOS2_VKN_DRAW_H
#define DM_HLE_DOS2_VKN_DRAW_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct vkn_draw_in;

// Packet 0x23 of the command-list executor 0x93e5f0 (DrawArrays: first = packet +4, count = packet +8): the engine's common draw tail
// (constant and texture flushes, conditional-render fields) around vkn_draw_arrays. Returns vkn_draw_arrays' result (0 recorded).
int dm_vkn_draw_arrays(uint32_t first, uint32_t count);
// Fills a vkn_draw_in from the renderer cache r = [main+0x2bbdc68] (shader, states, vertex format / buffers, index buffer, primitive,
// alpha reference, texture / sampler slot arrays, viewport, effective scissor). Call it once per draw: it tracks what NVN would
// have bound for cache fields the engine zeroes without an NVN call (blend / depth-stencil / raster handle 0) and which of
// viewport / scissor set the NVN scissor last.
void dm_vkn_draw_fill(struct vkn_draw_in *in);
// The engine's common draw tail, split around the draw command (0x928e80 / 0x928fb0 / 0x9290f0 and packet 0x23 all have it):
// pre = FlushGlobalConstants + FlushShaderConstants when a shader record is bound, FlushTextures, the conditional-render reset of
// r+0x185c / r+0x185d / r+0x1860; post = r+0x185d = r+0x185c. For the immediate-draw part, which has the same tail.
void dm_vkn_draw_tail_pre(uint64_t r);
void dm_vkn_draw_tail_post(uint64_t r);

#ifdef __cplusplus
}
#endif
#endif
