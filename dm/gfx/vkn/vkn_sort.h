// [DOS2] VK-NATIVE [rt2 R7] DM_VK_NATIVE_SORT: fewer pipeline binds by reordering draws where the order provably does not matter
// (vkn_sort.c; docs/dos2/nr/VK-RT-ANALYSIS.md section 11). No game addresses.
//
// What may be reordered: a run of consecutive engine draws of ONE render pass instance that write only depth with LESS / LESS_OR_EQUAL
// (or write nothing): no colour written (no colour attachment, or every colour write mask 0 - the joined depth pre-pass), depth test on
// with LESS / LEQUAL, no stencil test, no depth bounds, no storage / image bindings in the shader, no query bracket or predicate (the
// query module idle), the engine's own primitive (no index rewrite), not a piece's nested draw, not the pass's first draw (the load
// policy may have taken it as the draw that covers the area). For such a run the final depth of every texel is min(start, z of every
// fragment that survives its own alpha test) whatever the order (LESS and LEQUAL both keep the minimum; ties give the same value), and
// nothing else is written. Blended, colour-writing, GREATER / EQUAL / ALWAYS, stencil, query draws are never moved and end the run.
//
// How: vkn_draw.c routes every graphics bind / dynamic state / draw of its path through the functions below (only while the switch is
// on). The module mirrors what the draw path believes is bound (its g_b) and what the command buffer really holds. A draw the draw path
// marks captured (vkn_sort_cap) becomes a packet: the whole state in effect at its draw (pipeline, layout + set + dynamic offsets, vertex
// buffers, index buffer, viewport, scissor, the draw's numbers). The window is emitted (vkn_sort_flush) before ANY other command reaches
// the command buffer: vkn_cb() flushes (every recorder asks it), vkn_pass flushes before the commands it records on the open pass's
// handle (pass end, clear attachments), vkn_frame before the close, vkn_draw before a draw that is not captured. Emission: packets
// grouped by pipeline in the order of first appearance, each group in recorded order (stable); every bind is compared with what the
// command buffer holds; at the end the state is put back to what the draw path believes (its next draw then binds as before).
//   0  off (the default until the phone's verify run): the draw path calls Vulkan directly, nothing here runs;
//   1  sort: windows reordered;
//   2  verify: windows captured and their sorted order computed and counted, emitted in the recorded order (the command stream equals
//      mode 0: the host test compares the journals; the census line says how many binds sorting would have saved).
// Recording thread only (the recording's owner), no locks.
#ifndef DM_VKN_SORT_H
#define DM_VKN_SORT_H
#include "vkn.h"
#ifdef __cplusplus
extern "C" {
#endif
extern int vkn_sort_on;          // mode != 0 (install time)
extern int vkn_sort_cap;         // the draw being prepared is captured (vkn_draw.c)
extern uint32_t vkn_sort_n;      // packets waiting
void vkn_sort_mode_set(int mode);
int vkn_sort_mode(void);
// the draw path's graphics commands (each: captured into the draw path's view while vkn_sort_cap, else recorded and mirrored)
void vkn_sort_pipe(VkCommandBuffer cb, VkPipeline p);
void vkn_sort_sets(VkCommandBuffer cb, VkPipelineLayout layout, VkDescriptorSet set, uint32_t ndyn, const uint32_t *dyn);
void vkn_sort_vb(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *buf, const VkDeviceSize *off);
void vkn_sort_ib(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type);
void vkn_sort_vp(VkCommandBuffer cb, const VkViewport *vp);
void vkn_sort_sc(VkCommandBuffer cb, const VkRect2D *sc);
void vkn_sort_draw(VkCommandBuffer cb, uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance);
void vkn_sort_draw_idx(VkCommandBuffer cb, uint32_t count, uint32_t instances, uint32_t first, int32_t vertex_offset, uint32_t first_instance);
// the draw path forgot what is bound (a new recording, raw draws of the UI, a driver correction): the window is emitted first
void vkn_sort_forget(void);
// emits the window (no-op when empty); any thread that owns the recording
void vkn_sort_flush(void);
// a captured draw ends before its command (dropped / empty scissor): its binds stay a packet of their own (mode 2 records them in place)
void vkn_sort_nodraw(void);
// the draw path's question after vkn_pass_ready: 1 = this draw may join the window. depth_state: the ds id the pipeline is built with;
// write_masks: the pass info's colour write masks (n = its colour count); pass_begins: vkn_pass_begins() now.
struct vkn_shader;
int vkn_sort_eligible(uint16_t depth_state, uint32_t ncolor, const VkColorComponentFlags *write_masks, int has_depth, const struct vkn_shader *s,
                      int nested, int rewritten, uint64_t pass_begins);
typedef struct vkn_sort_stats {
    uint64_t windows, packets, moved, pipe_rec, pipe_sort, pipe_emit, set_rec, set_emit, restores, full;   // rec / sort: binds in recorded / sorted order; emit: emitted
    uint64_t why_not[8];             // draws not captured, by reason (VKN_SORT_W_*)
} vkn_sort_stats;
enum { VKN_SORT_W_NESTED, VKN_SORT_W_COLOUR, VKN_SORT_W_DEPTH, VKN_SORT_W_STENCIL, VKN_SORT_W_SHADER, VKN_SORT_W_QUERY, VKN_SORT_W_FIRST, VKN_SORT_W_REWRITE };
void vkn_sort_stats_get(vkn_sort_stats *s);
void vkn_sort_census(void);      // one line (the 10 s census)
#ifdef __cplusplus
}
#endif
#endif
