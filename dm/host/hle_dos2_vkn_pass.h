// [DOS2] VK-NATIVE stage 3, pass part of the front end (hle_dos2_vkn_pass.c): what the other parts call. Mode 1 only.
#ifndef DM_HLE_DOS2_VKN_PASS_H
#define DM_HLE_DOS2_VKN_PASS_H
#include <stdint.h>

// Region clear of packet 0x04 (0x93e5f0): engine texture handle (generation checked; unknown = dropped and counted), layer (-1 -> 0),
// colour, region {x, y, w, h} of level 0 (NVN ClearTexture(region, mask 0xf): independent of the bound targets).
void dm_vkn_pass_clear_rect(uint64_t tex_handle, int32_t layer, const float rgba[4], int32_t x, int32_t y, int32_t w, int32_t h);

// NVN's viewport / scissor as the replaced SetRenderTargets 0x9279a0 and ClearDepthStencil 0x928be0 leave them: both call
// nvnCommandBufferSetViewport AND SetScissor(0, 0, w, h) - only when the viewport cache r+0x171c..0x1728 differs from that rectangle
// (it is then rewritten); the scissor cache r+0x172c (packet 0x13's) is NOT written. So after such a call the scissor NVN applies is
// the full rectangle while r+0x172c still holds the old one: whoever derives the draw's scissor must know. The serial changes with
// every such call (0 = none yet); xywh gets its rectangle. dm_vkn_on_nvn_viewport_scissor (weak: defined by the part that tracks the
// effective NVN viewport / scissor, if any) is called at the same moment.
uint32_t dm_vkn_pass_nvn_vpsc(int32_t xywh[4]);
void dm_vkn_on_nvn_viewport_scissor(int32_t x, int32_t y, int32_t w, int32_t h) __attribute__((weak));

#endif
