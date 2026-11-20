// [DOS2] VK-NATIVE stage 3, render passes (vkn_pass.c): the engine's "set render targets / clear / copy" events as Vulkan render
// passes. Design: docs/dos2/nr/VK-NATIVE.md section 4.3 (the rule is the iPad Metal backend's):
//   - a pass is opened LAZILY, by the first draw after the targets changed (vkn_pass_ready);
//   - a clear that arrives while no pass is open is remembered on the texture and becomes loadOp CLEAR of the next pass that has the
//     texture as an attachment; an attachment is LOADed only if it was written in this frame (or carries data from an earlier frame
//     that the engine did not clear: see "stale" below), else DONT_CARE; a remembered clear whose texture is used otherwise first
//     (sampled, copied, presented) is executed as a command of its own (vkCmdClearColorImage / vkCmdClearDepthStencilImage);
//   - a clear that arrives while a pass with that attachment is open: vkCmdClearAttachments;
//   - before a texture is sampled or copied its layout is made right: if it is an attachment of the open pass the pass ends first
//     (and reopens with LOAD at the next draw).
// "stale": a target not written in this frame and not cleared may still be read by the engine on purpose (history buffers, the
// shadow atlas kept across frames): its content is kept (LOAD) unless the texture was never written at all. So DONT_CARE is used
// only for never-written targets; the tile-GPU refinement (DONT_CARE for targets fully covered by the first draw) is stage 4.
// Layout bookkeeping: vkn_tex.layout is the truth; this module is the only one that changes it between vkn_rec_begin and submit
// (uploads set it before). Single-threaded (the recording thread). No game addresses.
#ifndef DM_VKN_PASS_H
#define DM_VKN_PASS_H
#include "vkn.h"
#include "vkn_tex.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_PASS_COLORS = 8 };
// The engine set its targets (SetRenderTargets 0x9279a0 semantics): n colour targets (a NULL entry = that slot unbound), layer and
// mip of each (already defaulted to 0), depth target (NULL = none). Ends an open pass if the set differs. Nothing is recorded yet.
void vkn_pass_targets(uint32_t n, vkn_tex *const color[VKN_PASS_COLORS], const uint32_t layer[VKN_PASS_COLORS], const uint32_t mip[VKN_PASS_COLORS], vkn_tex *depth);
// Depth-only target set by the engine's depth clear (0x928be0 binds (0 colours, depth) when depth differs from the bound one).
// Clears. rect NULL = whole level. mode of the depth clear: bit 0 depth, bit 1 stencil.
void vkn_pass_clear_color(vkn_tex *t, uint32_t layer, const float rgba[4], const int32_t rect[4]);
void vkn_pass_clear_depth(vkn_tex *t, int what, float depth, uint32_t stencil);

// What a draw needs to know about the pass it is drawn in.
typedef struct vkn_pass_info {
    VkRenderPass pass; uint16_t compat;          // vkn_state.h render pass + its compatibility id (pipeline key)
    uint32_t ncolor, width, height, samples;
    VkColorComponentFlags write_mask[VKN_PASS_COLORS];   // by target format (vkn_write_mask_*): 0 for an unbound slot
    uint8_t has_depth, has_stencil;
    VkFormat depth_format;
} vkn_pass_info;
// Makes the pass of the current targets open in vkn_cb() (begins it if needed, with the load ops of the rule above). 0 = ok and
// *info filled; -1 = no targets / no command buffer / failure (the draw must be dropped).
int vkn_pass_ready(vkn_pass_info *info);
// A draw is about to sample `t` (called for every texture of the draw BEFORE vkn_pass_ready): transitions it to
// SHADER_READ_ONLY_OPTIMAL if needed (ending the open pass when that is required, executing a remembered clear). Returns 0 = ok,
// 1 = `t` is an attachment of the current target set (a feedback loop: the caller binds its placeholder texture instead).
int vkn_pass_sample(vkn_tex *t);
// A compute dispatch is about to use `t` as a storage image: ends the pass, layout GENERAL.
void vkn_pass_storage(vkn_tex *t);
void vkn_pass_end(void);                         // ends the open pass, if any (copies, submit; registered as a close listener of vkn_frame)
// The engine's texture copy (0x920b00 semantics): formats equal; both 1 level: one copy of min(depth) layers; else for i < dst
// levels: src level (src levels - dst levels + i) -> dst level i.
void vkn_pass_copy(vkn_tex *dst, vkn_tex *src);
// Transition for a use outside this module (present: TRANSFER_SRC_OPTIMAL; read-back). Ends the pass if `t` is in it.
void vkn_pass_layout(vkn_tex *t, VkImageLayout to);

typedef struct vkn_pass_stats { uint64_t passes, load, clear, dontcare, clears_folded, clears_cmd, clears_att, breaks_sample, feedback, copies, transitions; } vkn_pass_stats;
void vkn_pass_stats_get(vkn_pass_stats *s);

#ifdef __cplusplus
}
#endif
#endif
