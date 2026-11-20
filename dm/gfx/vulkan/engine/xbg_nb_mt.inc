// [DOS2] NB-20b DM_NB_MT: the Vulkan recording of a submission on a worker thread (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-20b findings";
// queue, order and waiting rules in dm/gfx/nb/nb_mt.h - the host test tests/nb_mt_test.cpp runs this file and that header against a mock
// Vulkan, also under ThreadSanitizer). Included by xbg_game.cpp at file scope, before the first vkCmd* of the translation unit.
//
// The cut is the vkCmd* boundary: the translate thread still takes every decision (pass begins / ends, barriers, layouts, uploads, the
// pipeline, descriptor and snapshot work - NB-20a classes S and L stay serial, nothing of the draw path's state is shared with the worker),
// but each vkCmd* on the recording slot's command buffer becomes a packet (arguments and arrays copied), and the worker makes the calls.
// The command buffer gets the same calls with the same arguments in the same order; it and its command pool are used by one thread at a
// time. Everything is in the command buffer before vkEndCommandBuffer (nbmt::end waits on a condition variable; while the worker is
// behind, the translate thread replays the remaining chunks itself instead of sleeping).
//   DM_NB_MT   0  off: every wrapper is one never-taken compare and the direct call (no thread, no packets)
//              1  packets, replayed by the translate thread at every hand-over (the cost of the packet layer alone, no thread)
//              2  worker + check: every chunk's bytes hashed at hand-over and before / after the replay, hand-over order, packet walk
//              3  worker
//   DM_NB_MT_FLUSH     256   packets per hand-over (16..65536)
//   DM_NB_MT_CHUNK_KB  64    chunk size (16..1024); a larger packet gets a chunk of its own
//   DM_NB_MT_QUEUE     64    chunks queued at most (2..1024): past it the translate thread replays chunks itself ("queue full")
//   DM_NB_MT_CPUS      0     hex CPU mask of the worker; 0 = every CPU (the mask inherited from the pinned translate thread is dropped)
// A submission is recorded directly (counted by reason) when the slot has no command pool of its own (the pool would be shared with the
// other slot), in the off phase of the in-process A/B (gfx-ab-target = nbmt), or when the worker could not be started.
// A call with a pNext chain is not copied: the queue is drained and the call made directly (counted).
// Line "[nb] mt ..." every 10 s while submissions come.
//
// Engine hooks (tests/source_structure_test.py, block NB-20b, keeps them in place): nbmt::init() at the switches, nbmt::begin() right after
// vkBeginCommandBuffer(s.cmd) in recording_slot(), nbmt::end() right before vkEndCommandBuffer(s.cmd) in submit_slots(), nbmt::stop() in
// destroy_slots(); the four commands called through function pointers (conditional rendering, indirect count) go through nbmt::P*.
// Every vkCmd* name used by the translation unit must be in the #define list at the end of this file.
#include "../../nb/nb_mt.h"
#include <algorithm>
#include <tuple>
#include <new>
#if defined(__linux__)
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#endif
#ifndef NBMT_ENDRP_HOOK
#define NBMT_ENDRP_HOOK() ((void)0)   // [safe-mode] xbg_game.cpp defines it (DM_GFX_FAKE_GPU_CRASH=<s>:rec)
#endif
#ifndef NBMT_REAL
#define NBMT_REAL(f) ::f
#endif
#ifndef NBMT_PRE
#define NBMT_PRE(c, op) ((void)0)   // [NR-21] xbg_game.cpp defines it (DM_NR_SORT: queued draws are recorded before any other command); the host test keeps it empty
#endif
namespace {
namespace nbmt {
using nbmtq::Pkt;
enum Op : uint16_t { OP_BeginRenderPass = 1, OP_EndRenderPass, OP_BindPipeline, OP_BindDescriptorSets, OP_BindVertexBuffers, OP_BindIndexBuffer,
    OP_PushConstants, OP_SetViewport, OP_SetScissor, OP_Draw, OP_DrawIndexed, OP_DrawIndirect, OP_DrawIndexedIndirect, OP_DrawIndirectCount,
    OP_DrawIndexedIndirectCount, OP_Dispatch, OP_DispatchIndirect, OP_ClearAttachments, OP_ClearColorImage, OP_ClearDepthStencilImage,
    OP_CopyBuffer, OP_CopyBufferToImage, OP_CopyImageToBuffer, OP_CopyImage, OP_BlitImage, OP_ResolveImage, OP_FillBuffer, OP_PipelineBarrier,
    OP_BeginQuery, OP_EndQuery, OP_ResetQueryPool, OP_WriteTimestamp, OP_CopyQueryPoolResults, OP_BeginConditionalRendering,
    OP_EndConditionalRendering, OP_NextSubpass, OP_COUNT };
nbmtq::Queue q;
VkCommandBuffer cur = VK_NULL_HANDLE;   // the command buffer being recorded through packets (null: every wrapper calls directly)
int mode = 0;
unsigned long cpus = 0;
uint64_t report_ns = 0, pnext_calls = 0;
void (*on_packet)(uint16_t op) = nullptr;   // DM_NB_MX_PROF (xbg_nb_mx.inc): every packet written, by op

inline bool go(VkCommandBuffer c) { return __builtin_expect(c == cur, 0) && c != VK_NULL_HANDLE; }
// Room for packet P plus `extra` bytes of array copies; nullptr = no memory: the queue is drained, the caller calls directly.
template <class P> inline P *open(uint16_t op, size_t extra, uint8_t *&tail) {
    Pkt *k = q.open(uint32_t(nbmtq::al8(sizeof(P)) + extra));
    if (!k) { q.sync(nbmtq::NBMT_SY_ALLOC); return nullptr; }
    k->op = op; tail = reinterpret_cast<uint8_t *>(k) + nbmtq::al8(sizeof(P));
    if (__builtin_expect(on_packet != nullptr, 0)) on_packet(op);
    return reinterpret_cast<P *>(k);
}
template <class T> inline size_t asz(const T *s, uint32_t n) { return s && n ? nbmtq::al8(size_t(n) * sizeof(T)) : 0; }
template <class T> inline const T *dup(uint8_t *&tail, const T *s, uint32_t n) {
    if (!s || !n) return nullptr;
    T *d = reinterpret_cast<T *>(tail); memcpy(static_cast<void *>(d), s, size_t(n) * sizeof(T)); tail += nbmtq::al8(size_t(n) * sizeof(T));
    return d;
}
inline void pnext_sync() { ++pnext_calls; q.sync(nbmtq::NBMT_SY_PNEXT); }
template <class T> inline bool chained(const T *a, uint32_t n) { for (uint32_t i = 0; a && i < n; i++) if (a[i].pNext) return true; return false; }

// [DOS2] NR-ZERO Z5: the packet layouts the chunk builder (Build, below) writes too and same_stream() reads - named here, the
// wrappers alias them (the same members in the same order as the local structs they replace).
template <class... A> struct PodP { Pkt k; std::tuple<A...> v; };
struct PBindSets { Pkt k; VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, nd; const VkDescriptorSet *sets; const uint32_t *dyn; };
struct PBindVB { Pkt k; uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o; };
struct PViewport { Pkt k; uint32_t first, n; const VkViewport *v; };
struct PScissor { Pkt k; uint32_t first, n; const VkRect2D *v; };
// A command whose arguments are all values: the packet is the argument tuple.
template <auto F, uint16_t OP, class... A> inline void pod(VkCommandBuffer c, A... a) {
    NBMT_PRE(c, OP);
    using P = PodP<A...>; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP, 0, t)) {
        new (&p->v) std::tuple<A...>(a...);
        p->k.run = [](void *cb, const Pkt *k) { std::apply([cb](const A &...x) { F(VkCommandBuffer(cb), x...); }, reinterpret_cast<const P *>(k)->v); };
        q.close(&p->k); return; }
    F(c, a...);
}
// The same for a command reached through a function pointer (extension entry points resolved at init).
template <uint16_t OP, class Fn, class... A> inline void podf(Fn f, VkCommandBuffer c, A... a) {
    NBMT_PRE(c, OP);
    struct P { Pkt k; Fn f; std::tuple<A...> v; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP, 0, t)) {
        p->f = f; new (&p->v) std::tuple<A...>(a...);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); std::apply([cb, p](const A &...x) { p->f(VkCommandBuffer(cb), x...); }, p->v); };
        q.close(&p->k); return; }
    f(c, a...);
}

// [safe-mode] NBMT_ENDRP_HOOK (xbg_game.cpp: the DM_GFX_FAKE_GPU_CRASH=<s>:rec test) runs where the call is made: in the packet on the worker,
// or on the caller when the command is direct. Empty in the host tests.
template <auto F> inline void end_rp_call(VkCommandBuffer c) { NBMT_ENDRP_HOOK(); F(c); }
inline void CmdEndRenderPass(VkCommandBuffer c) { pod<end_rp_call<NBMT_REAL(vkCmdEndRenderPass)>, OP_EndRenderPass>(c); }
inline void CmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipeline pl) { pod<NBMT_REAL(vkCmdBindPipeline), OP_BindPipeline>(c, bp, pl); }
inline void CmdNextSubpass(VkCommandBuffer c, VkSubpassContents sc) { pod<NBMT_REAL(vkCmdNextSubpass), OP_NextSubpass>(c, sc); }   // [DOS2] NR-ONCHIP
inline void CmdBindIndexBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkIndexType t) { pod<NBMT_REAL(vkCmdBindIndexBuffer), OP_BindIndexBuffer>(c, b, o, t); }
inline void CmdDraw(VkCommandBuffer c, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) { pod<NBMT_REAL(vkCmdDraw), OP_Draw>(c, vc, ic, fv, fi); }
inline void CmdDrawIndexed(VkCommandBuffer c, uint32_t n, uint32_t ic, uint32_t first, int32_t vo, uint32_t fi) { pod<NBMT_REAL(vkCmdDrawIndexed), OP_DrawIndexed>(c, n, ic, first, vo, fi); }
inline void CmdDrawIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride) { pod<NBMT_REAL(vkCmdDrawIndirect), OP_DrawIndirect>(c, b, o, n, stride); }
inline void CmdDrawIndexedIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride) { pod<NBMT_REAL(vkCmdDrawIndexedIndirect), OP_DrawIndexedIndirect>(c, b, o, n, stride); }
inline void CmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) { pod<NBMT_REAL(vkCmdDispatch), OP_Dispatch>(c, x, y, z); }
inline void CmdDispatchIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o) { pod<NBMT_REAL(vkCmdDispatchIndirect), OP_DispatchIndirect>(c, b, o); }
inline void CmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize n, uint32_t v) { pod<NBMT_REAL(vkCmdFillBuffer), OP_FillBuffer>(c, b, o, n, v); }
inline void CmdBeginQuery(VkCommandBuffer c, VkQueryPool p, uint32_t i, VkQueryControlFlags f) { pod<NBMT_REAL(vkCmdBeginQuery), OP_BeginQuery>(c, p, i, f); }
inline void CmdEndQuery(VkCommandBuffer c, VkQueryPool p, uint32_t i) { pod<NBMT_REAL(vkCmdEndQuery), OP_EndQuery>(c, p, i); }
inline void CmdResetQueryPool(VkCommandBuffer c, VkQueryPool p, uint32_t first, uint32_t n) { pod<NBMT_REAL(vkCmdResetQueryPool), OP_ResetQueryPool>(c, p, first, n); }
inline void CmdWriteTimestamp(VkCommandBuffer c, VkPipelineStageFlagBits st, VkQueryPool p, uint32_t i) { pod<NBMT_REAL(vkCmdWriteTimestamp), OP_WriteTimestamp>(c, st, p, i); }
inline void CmdCopyQueryPoolResults(VkCommandBuffer c, VkQueryPool p, uint32_t first, uint32_t n, VkBuffer b, VkDeviceSize o, VkDeviceSize stride, VkQueryResultFlags f) {
    pod<NBMT_REAL(vkCmdCopyQueryPoolResults), OP_CopyQueryPoolResults>(c, p, first, n, b, o, stride, f); }
// function-pointer commands (xbg_game.cpp calls these four through nbmt::P*)
inline void PDrawIndirectCount(PFN_vkCmdDrawIndirectCountKHR f, VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co, uint32_t n, uint32_t stride) {
    podf<OP_DrawIndirectCount>(f, c, b, o, cb, co, n, stride); }
inline void PDrawIndexedIndirectCount(PFN_vkCmdDrawIndexedIndirectCountKHR f, VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co, uint32_t n, uint32_t stride) {
    podf<OP_DrawIndexedIndirectCount>(f, c, b, o, cb, co, n, stride); }
inline void PEndConditionalRendering(PFN_vkCmdEndConditionalRenderingEXT f, VkCommandBuffer c) { podf<OP_EndConditionalRendering>(f, c); }
inline void PBeginConditionalRendering(PFN_vkCmdBeginConditionalRenderingEXT f, VkCommandBuffer c, const VkConditionalRenderingBeginInfoEXT *bi) {
    NBMT_PRE(c, OP_BeginConditionalRendering);
    struct P { Pkt k; PFN_vkCmdBeginConditionalRenderingEXT f; VkConditionalRenderingBeginInfoEXT bi; }; uint8_t *t;
    if (go(c)) { if (bi->pNext) pnext_sync(); else if (P *p = open<P>(OP_BeginConditionalRendering, 0, t)) {
        p->f = f; p->bi = *bi;
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); p->f(VkCommandBuffer(cb), &p->bi); };
        q.close(&p->k); return; } }
    f(c, bi);
}

inline void CmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo *b, VkSubpassContents sc) {
    NBMT_PRE(c, OP_BeginRenderPass);
    struct P { Pkt k; VkRenderPassBeginInfo b; VkSubpassContents sc; }; uint8_t *t;
    if (go(c)) { if (b->pNext) pnext_sync(); else if (P *p = open<P>(OP_BeginRenderPass, asz(b->pClearValues, b->clearValueCount), t)) {
        p->b = *b; p->sc = sc; p->b.pClearValues = dup(t, b->pClearValues, b->clearValueCount);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdBeginRenderPass)(VkCommandBuffer(cb), &p->b, p->sc); };
        q.close(&p->k); return; } }
    NBMT_REAL(vkCmdBeginRenderPass)(c, b, sc);
}
inline void CmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t nd, const uint32_t *dyn) {
    NBMT_PRE(c, OP_BindDescriptorSets);
    using P = PBindSets; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_BindDescriptorSets, asz(sets, n) + asz(dyn, nd), t)) {
        p->bp = bp; p->l = l; p->first = first; p->n = n; p->nd = nd; p->sets = dup(t, sets, n); p->dyn = dup(t, dyn, nd);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdBindDescriptorSets)(VkCommandBuffer(cb), p->bp, p->l, p->first, p->n, p->sets, p->nd, p->dyn); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdBindDescriptorSets)(c, bp, l, first, n, sets, nd, dyn);
}
inline void CmdBindVertexBuffers(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    NBMT_PRE(c, OP_BindVertexBuffers);
    using P = PBindVB; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_BindVertexBuffers, asz(b, n) + asz(o, n), t)) {
        p->first = first; p->n = n; p->b = dup(t, b, n); p->o = dup(t, o, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdBindVertexBuffers)(VkCommandBuffer(cb), p->first, p->n, p->b, p->o); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdBindVertexBuffers)(c, first, n, b, o);
}
inline void CmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    NBMT_PRE(c, OP_PushConstants);
    struct P { Pkt k; VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, size; const uint8_t *v; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_PushConstants, asz(static_cast<const uint8_t *>(v), size), t)) {
        p->l = l; p->st = st; p->off = off; p->size = size; p->v = dup(t, static_cast<const uint8_t *>(v), size);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdPushConstants)(VkCommandBuffer(cb), p->l, p->st, p->off, p->size, p->v); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdPushConstants)(c, l, st, off, size, v);
}
inline void CmdSetViewport(VkCommandBuffer c, uint32_t first, uint32_t n, const VkViewport *v) {
    NBMT_PRE(c, OP_SetViewport);
    using P = PViewport; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_SetViewport, asz(v, n), t)) {
        p->first = first; p->n = n; p->v = dup(t, v, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdSetViewport)(VkCommandBuffer(cb), p->first, p->n, p->v); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdSetViewport)(c, first, n, v);
}
inline void CmdSetScissor(VkCommandBuffer c, uint32_t first, uint32_t n, const VkRect2D *v) {
    NBMT_PRE(c, OP_SetScissor);
    using P = PScissor; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_SetScissor, asz(v, n), t)) {
        p->first = first; p->n = n; p->v = dup(t, v, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdSetScissor)(VkCommandBuffer(cb), p->first, p->n, p->v); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdSetScissor)(c, first, n, v);
}
inline void CmdClearAttachments(VkCommandBuffer c, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r) {
    NBMT_PRE(c, OP_ClearAttachments);
    struct P { Pkt k; uint32_t na, nr; const VkClearAttachment *a; const VkClearRect *r; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_ClearAttachments, asz(a, na) + asz(r, nr), t)) {
        p->na = na; p->nr = nr; p->a = dup(t, a, na); p->r = dup(t, r, nr);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdClearAttachments)(VkCommandBuffer(cb), p->na, p->a, p->nr, p->r); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdClearAttachments)(c, na, a, nr, r);
}
inline void CmdClearColorImage(VkCommandBuffer c, VkImage im, VkImageLayout lay, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    NBMT_PRE(c, OP_ClearColorImage);
    struct P { Pkt k; VkImage im; VkImageLayout lay; VkClearColorValue v; uint32_t n; const VkImageSubresourceRange *r; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_ClearColorImage, asz(r, n), t)) {
        p->im = im; p->lay = lay; p->v = *v; p->n = n; p->r = dup(t, r, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdClearColorImage)(VkCommandBuffer(cb), p->im, p->lay, &p->v, p->n, p->r); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdClearColorImage)(c, im, lay, v, n, r);
}
inline void CmdClearDepthStencilImage(VkCommandBuffer c, VkImage im, VkImageLayout lay, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    NBMT_PRE(c, OP_ClearDepthStencilImage);
    struct P { Pkt k; VkImage im; VkImageLayout lay; VkClearDepthStencilValue v; uint32_t n; const VkImageSubresourceRange *r; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_ClearDepthStencilImage, asz(r, n), t)) {
        p->im = im; p->lay = lay; p->v = *v; p->n = n; p->r = dup(t, r, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdClearDepthStencilImage)(VkCommandBuffer(cb), p->im, p->lay, &p->v, p->n, p->r); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdClearDepthStencilImage)(c, im, lay, v, n, r);
}
inline void CmdCopyBuffer(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) {
    NBMT_PRE(c, OP_CopyBuffer);
    struct P { Pkt k; VkBuffer s, d; uint32_t n; const VkBufferCopy *r; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_CopyBuffer, asz(r, n), t)) {
        p->s = s; p->d = d; p->n = n; p->r = dup(t, r, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdCopyBuffer)(VkCommandBuffer(cb), p->s, p->d, p->n, p->r); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdCopyBuffer)(c, s, d, n, r);
}
inline void CmdCopyBufferToImage(VkCommandBuffer c, VkBuffer s, VkImage d, VkImageLayout dl, uint32_t n, const VkBufferImageCopy *r) {
    NBMT_PRE(c, OP_CopyBufferToImage);
    struct P { Pkt k; VkBuffer s; VkImage d; VkImageLayout dl; uint32_t n; const VkBufferImageCopy *r; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_CopyBufferToImage, asz(r, n), t)) {
        p->s = s; p->d = d; p->dl = dl; p->n = n; p->r = dup(t, r, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdCopyBufferToImage)(VkCommandBuffer(cb), p->s, p->d, p->dl, p->n, p->r); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdCopyBufferToImage)(c, s, d, dl, n, r);
}
inline void CmdCopyImageToBuffer(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n, const VkBufferImageCopy *r) {
    NBMT_PRE(c, OP_CopyImageToBuffer);
    struct P { Pkt k; VkImage s; VkImageLayout sl; VkBuffer d; uint32_t n; const VkBufferImageCopy *r; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_CopyImageToBuffer, asz(r, n), t)) {
        p->s = s; p->sl = sl; p->d = d; p->n = n; p->r = dup(t, r, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdCopyImageToBuffer)(VkCommandBuffer(cb), p->s, p->sl, p->d, p->n, p->r); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdCopyImageToBuffer)(c, s, sl, d, n, r);
}
inline void CmdCopyImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    NBMT_PRE(c, OP_CopyImage);
    struct P { Pkt k; VkImage s, d; VkImageLayout sl, dl; uint32_t n; const VkImageCopy *r; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_CopyImage, asz(r, n), t)) {
        p->s = s; p->sl = sl; p->d = d; p->dl = dl; p->n = n; p->r = dup(t, r, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdCopyImage)(VkCommandBuffer(cb), p->s, p->sl, p->d, p->dl, p->n, p->r); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdCopyImage)(c, s, sl, d, dl, n, r);
}
inline void CmdBlitImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    NBMT_PRE(c, OP_BlitImage);
    struct P { Pkt k; VkImage s, d; VkImageLayout sl, dl; uint32_t n; VkFilter f; const VkImageBlit *r; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_BlitImage, asz(r, n), t)) {
        p->s = s; p->sl = sl; p->d = d; p->dl = dl; p->n = n; p->f = f; p->r = dup(t, r, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdBlitImage)(VkCommandBuffer(cb), p->s, p->sl, p->d, p->dl, p->n, p->r, p->f); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdBlitImage)(c, s, sl, d, dl, n, r, f);
}
inline void CmdResolveImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
    NBMT_PRE(c, OP_ResolveImage);
    struct P { Pkt k; VkImage s, d; VkImageLayout sl, dl; uint32_t n; const VkImageResolve *r; }; uint8_t *t;
    if (go(c)) if (P *p = open<P>(OP_ResolveImage, asz(r, n), t)) {
        p->s = s; p->sl = sl; p->d = d; p->dl = dl; p->n = n; p->r = dup(t, r, n);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdResolveImage)(VkCommandBuffer(cb), p->s, p->sl, p->d, p->dl, p->n, p->r); };
        q.close(&p->k); return; }
    NBMT_REAL(vkCmdResolveImage)(c, s, sl, d, dl, n, r);
}
inline void CmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags df, uint32_t nm, const VkMemoryBarrier *m,
                               uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    NBMT_PRE(c, OP_PipelineBarrier);
    struct P { Pkt k; VkPipelineStageFlags src, dst; VkDependencyFlags df; uint32_t nm, nb, ni; const VkMemoryBarrier *m; const VkBufferMemoryBarrier *b; const VkImageMemoryBarrier *i; }; uint8_t *t;
    if (go(c)) { if (chained(m, nm) || chained(b, nb) || chained(i, ni)) pnext_sync(); else if (P *p = open<P>(OP_PipelineBarrier, asz(m, nm) + asz(b, nb) + asz(i, ni), t)) {
        p->src = src; p->dst = dst; p->df = df; p->nm = nm; p->nb = nb; p->ni = ni; p->m = dup(t, m, nm); p->b = dup(t, b, nb); p->i = dup(t, i, ni);
        p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdPipelineBarrier)(VkCommandBuffer(cb), p->src, p->dst, p->df, p->nm, p->m, p->nb, p->b, p->ni, p->i); };
        q.close(&p->k); return; } }
    NBMT_REAL(vkCmdPipelineBarrier)(c, src, dst, df, nm, m, nb, b, ni, i);
}

// ---------------- [DOS2] NR-ZERO Z5: a chunk built apart from the queue ----------------
// The eight commands of a recorded engine draw (nrdraw::lane_tail) as packets written into a chunk of its own instead of the queue's
// current one: the same layouts, the same arguments, array copies inside the chunk (a packet points into its own chunk: a chunk is
// handed over whole - nbmtq::Queue::splice - never copied). false = no room (the chunk is full): the builder ends its run there.
struct Build { nbmtq::Chunk *c = nullptr; };
template <class P> inline P *bopen(Build &b, uint16_t op, size_t extra, uint8_t *&tail) {
    const size_t bytes = nbmtq::al8(sizeof(P)) + extra;
    if (!b.c || b.c->cap - b.c->used < bytes) return nullptr;
    Pkt *k = reinterpret_cast<Pkt *>(b.c->data() + b.c->used);
    k->size = uint32_t(bytes); k->pad = 0; k->op = op; tail = reinterpret_cast<uint8_t *>(k) + nbmtq::al8(sizeof(P));
    return reinterpret_cast<P *>(k);
}
inline void bclose(Build &b, Pkt *k) { b.c->used += k->size; ++b.c->cmds; }
template <auto F, uint16_t OP, class... A> inline bool bpod(Build &b, A... a) {
    using P = PodP<A...>; uint8_t *t; P *p = bopen<P>(b, OP, 0, t); if (!p) return false;
    new (&p->v) std::tuple<A...>(a...);
    p->k.run = [](void *cb, const Pkt *k) { std::apply([cb](const A &...x) { F(VkCommandBuffer(cb), x...); }, reinterpret_cast<const P *>(k)->v); };
    bclose(b, &p->k); return true;
}
inline bool BBindPipeline(Build &b, VkPipelineBindPoint bp, VkPipeline pl) { return bpod<NBMT_REAL(vkCmdBindPipeline), OP_BindPipeline>(b, bp, pl); }
inline bool BBindIndexBuffer(Build &b, VkBuffer buf, VkDeviceSize o, VkIndexType t) { return bpod<NBMT_REAL(vkCmdBindIndexBuffer), OP_BindIndexBuffer>(b, buf, o, t); }
inline bool BDraw(Build &b, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) { return bpod<NBMT_REAL(vkCmdDraw), OP_Draw>(b, vc, ic, fv, fi); }
inline bool BDrawIndexed(Build &b, uint32_t n, uint32_t ic, uint32_t first, int32_t vo, uint32_t fi) { return bpod<NBMT_REAL(vkCmdDrawIndexed), OP_DrawIndexed>(b, n, ic, first, vo, fi); }
inline bool BBindDescriptorSets(Build &b, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t nd, const uint32_t *dyn) {
    using P = PBindSets; uint8_t *t; P *p = bopen<P>(b, OP_BindDescriptorSets, asz(sets, n) + asz(dyn, nd), t); if (!p) return false;
    p->bp = bp; p->l = l; p->first = first; p->n = n; p->nd = nd; p->sets = dup(t, sets, n); p->dyn = dup(t, dyn, nd);
    p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdBindDescriptorSets)(VkCommandBuffer(cb), p->bp, p->l, p->first, p->n, p->sets, p->nd, p->dyn); };
    bclose(b, &p->k); return true;
}
inline bool BBindVertexBuffers(Build &b, uint32_t first, uint32_t n, const VkBuffer *buf, const VkDeviceSize *o) {
    using P = PBindVB; uint8_t *t; P *p = bopen<P>(b, OP_BindVertexBuffers, asz(buf, n) + asz(o, n), t); if (!p) return false;
    p->first = first; p->n = n; p->b = dup(t, buf, n); p->o = dup(t, o, n);
    p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdBindVertexBuffers)(VkCommandBuffer(cb), p->first, p->n, p->b, p->o); };
    bclose(b, &p->k); return true;
}
inline bool BSetViewport(Build &b, uint32_t first, uint32_t n, const VkViewport *v) {
    using P = PViewport; uint8_t *t; P *p = bopen<P>(b, OP_SetViewport, asz(v, n), t); if (!p) return false;
    p->first = first; p->n = n; p->v = dup(t, v, n);
    p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdSetViewport)(VkCommandBuffer(cb), p->first, p->n, p->v); };
    bclose(b, &p->k); return true;
}
inline bool BSetScissor(Build &b, uint32_t first, uint32_t n, const VkRect2D *v) {
    using P = PScissor; uint8_t *t; P *p = bopen<P>(b, OP_SetScissor, asz(v, n), t); if (!p) return false;
    p->first = first; p->n = n; p->v = dup(t, v, n);
    p->k.run = [](void *cb, const Pkt *k) { const P *p = reinterpret_cast<const P *>(k); NBMT_REAL(vkCmdSetScissor)(VkCommandBuffer(cb), p->first, p->n, p->v); };
    bclose(b, &p->k); return true;
}
// Two packet streams of those eight commands make the same calls: the same operations in the same order with the same arguments and the
// same array contents. (Raw bytes cannot be compared: a packet holds pointers into its own chunk, the address of its thunk, and padding.)
// false: *at = index of the first packet that differs (or that is not one of the eight); a stream that ends early differs at its end.
inline bool same_stream(const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb, uint32_t *at) {
    uint32_t ia = 0, ib = 0, n = 0;
    auto arr = [](const void *x, const void *y, size_t bytes) { return (!x && !y) || (x && y && !memcmp(x, y, bytes)); };
    for (; ia < na && ib < nb; ++n) {
        const Pkt *x = reinterpret_cast<const Pkt *>(a + ia), *y = reinterpret_cast<const Pkt *>(b + ib); bool ok = x->op == y->op && x->size >= sizeof(Pkt) && y->size >= sizeof(Pkt);
        if (ok) switch (x->op) {
        case OP_BindPipeline: { using P = PodP<VkPipelineBindPoint, VkPipeline>; ok = reinterpret_cast<const P *>(x)->v == reinterpret_cast<const P *>(y)->v; break; }
        case OP_BindIndexBuffer: { using P = PodP<VkBuffer, VkDeviceSize, VkIndexType>; ok = reinterpret_cast<const P *>(x)->v == reinterpret_cast<const P *>(y)->v; break; }
        case OP_Draw: { using P = PodP<uint32_t, uint32_t, uint32_t, uint32_t>; ok = reinterpret_cast<const P *>(x)->v == reinterpret_cast<const P *>(y)->v; break; }
        case OP_DrawIndexed: { using P = PodP<uint32_t, uint32_t, uint32_t, int32_t, uint32_t>; ok = reinterpret_cast<const P *>(x)->v == reinterpret_cast<const P *>(y)->v; break; }
        case OP_BindDescriptorSets: { const PBindSets *p = reinterpret_cast<const PBindSets *>(x), *r = reinterpret_cast<const PBindSets *>(y);
            ok = p->bp == r->bp && p->l == r->l && p->first == r->first && p->n == r->n && p->nd == r->nd && arr(p->sets, r->sets, size_t(p->n) * sizeof(VkDescriptorSet)) && arr(p->dyn, r->dyn, size_t(p->nd) * 4); break; }
        case OP_BindVertexBuffers: { const PBindVB *p = reinterpret_cast<const PBindVB *>(x), *r = reinterpret_cast<const PBindVB *>(y);
            ok = p->first == r->first && p->n == r->n && arr(p->b, r->b, size_t(p->n) * sizeof(VkBuffer)) && arr(p->o, r->o, size_t(p->n) * sizeof(VkDeviceSize)); break; }
        case OP_SetViewport: { const PViewport *p = reinterpret_cast<const PViewport *>(x), *r = reinterpret_cast<const PViewport *>(y); ok = p->first == r->first && p->n == r->n && arr(p->v, r->v, size_t(p->n) * sizeof(VkViewport)); break; }
        case OP_SetScissor: { const PScissor *p = reinterpret_cast<const PScissor *>(x), *r = reinterpret_cast<const PScissor *>(y); ok = p->first == r->first && p->n == r->n && arr(p->v, r->v, size_t(p->n) * sizeof(VkRect2D)); break; }
        default: ok = false; }
        if (!ok) { if (at) *at = n; return false; }
        ia += x->size; ib += y->size;
    }
    if (ia != na || ib != nb) { if (at) *at = n; return false; }
    return true;
}
// The sink of nrz5_emit (dm/gfx/nr/nr_z5.h: handles as words, viewport / scissor as bytes) that writes into a Build.
struct Z5Sink { Build &b;
    template <class H> static H h(uint64_t x) { return H(uintptr_t(x)); }
    bool pipeline(uint64_t p) { return BBindPipeline(b, VK_PIPELINE_BIND_POINT_GRAPHICS, h<VkPipeline>(p)); }
    bool sets(uint64_t l, uint64_t s, uint32_t n, const uint32_t *off) { const VkDescriptorSet ds = h<VkDescriptorSet>(s); return BBindDescriptorSets(b, VK_PIPELINE_BIND_POINT_GRAPHICS, h<VkPipelineLayout>(l), 0, 1, &ds, n, off); }
    bool viewport(const uint8_t *vp) { VkViewport v; memcpy(&v, vp, sizeof v); return BSetViewport(b, 0, 1, &v); }
    bool scissor(const uint8_t *sc) { VkRect2D v; memcpy(&v, sc, sizeof v); return BSetScissor(b, 0, 1, &v); }
    bool vertex(uint32_t first, uint32_t n, const uint64_t *buf, const uint64_t *off) { VkBuffer bb[16]; VkDeviceSize oo[16]; if (n > 16) return false;
        for (uint32_t i = 0; i < n; i++) { bb[i] = h<VkBuffer>(buf[i]); oo[i] = off[i]; } return BBindVertexBuffers(b, first, n, bb, oo); }
    bool index(uint64_t buf, uint64_t off, uint32_t type) { return BBindIndexBuffer(b, h<VkBuffer>(buf), off, VkIndexType(type)); }
    bool draw_indexed(uint32_t n, uint32_t ic, uint32_t first, int32_t vo, uint32_t fi) { return BDrawIndexed(b, n, ic, first, vo, fi); }
    bool draw(uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) { return BDraw(b, vc, ic, fv, fi); }
};
static_assert(sizeof(VkViewport) == 24 && sizeof(VkRect2D) == 16, "Z5Sink: the bytes nr_z5.h carries");
// ---------------- engine side: switches, the submission bracket, the report ----------------
void worker_start() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "dm-nbmt");
    // The translate thread is pinned to the fastest cores and a new thread inherits that mask (KB PERF-D07): the worker must not sit on
    // the translate thread's core. Every configured CPU unless DM_NB_MT_CPUS names a mask; the scheduler (and ADPF: the busiest threads
    // of the process join the hint session by themselves, perf_android.c) places it.
    cpu_set_t set; CPU_ZERO(&set); const long n = sysconf(_SC_NPROCESSORS_CONF);
    for (long c = 0; c < (n > 0 ? n : 8) && c < 64; c++) if (!cpus || (cpus >> c & 1)) CPU_SET(int(c), &set);
    sched_setaffinity(0, sizeof set, &set);
#endif
}
void init() {
    auto num = [](const char *name, long def, long lo, long hi) { const char *e = getenv(name); long v = e && *e ? atol(e) : def; return v < lo ? lo : v > hi ? hi : v; };
    mode = int(num("DM_NB_MT", 0, 0, 3)); q.mode = mode;
    q.flush_cmds = uint32_t(num("DM_NB_MT_FLUSH", 256, 16, 65536));
    q.chunk_bytes = uint32_t(num("DM_NB_MT_CHUNK_KB", 64, 16, 1024)) << 10;
    q.max_queued = uint32_t(num("DM_NB_MT_QUEUE", 64, 2, 1024));
    { const char *e = getenv("DM_NB_MT_CPUS"); cpus = e && *e ? strtoul(e, nullptr, 16) : 0; }
    q.on_worker_start = worker_start;
    { const char *e = getenv("DM_NR_CPU"); q.prof_on = mode && e && atoi(e) > 0; }   // [DOS2] NR-CPU: the replay's time by vkCmd*
    if (mode) xbg_logf("[nb] DM_NB_MT=%d: vkCmd* of the recording slot go through packets, %s (NB-20b; hand-over every %u packets, chunk %u KB, queue %u chunks, worker cpus %#lx)",
        mode, mode == 1 ? "replayed by the translate thread at every hand-over" : mode == 2 ? "recorded by the worker dm-nbmt and checked" : "recorded by the worker dm-nbmt",
        q.flush_cmds, q.chunk_bytes >> 10, q.max_queued, cpus);
}
void report(uint64_t now) {
    nbmtq::Stats s = q.st; q.st = nbmtq::Stats{}; nbmtq::WorkerStats w;
    { std::lock_guard<std::mutex> l(q.mu); w = q.ws; q.ws = nbmtq::WorkerStats{}; }
    const uint64_t direct = s.direct[0] + s.direct[1] + s.direct[2] + s.direct[3], pn = pnext_calls; pnext_calls = 0;
    if (!s.subs && !direct) return;
    const double subs = double(s.subs ? s.subs : 1), ms = 1e-6;
    xbg_logf("[nb] mt (DM_NB_MT=%d, last %.0f s): submissions through packets %llu, direct %llu (shared pool %llu, a/b off %llu, no worker %llu, nested %llu); "
             "per submission: packets %.0f in %.1f chunks (%.1f KB), worker replay %.2f ms (%.1f chunks, slept %.1f times, woken %.1f), translate thread: replay %.2f ms "
             "(%.1f chunks helped, mode 1 inline %.2f ms), waited %.3f ms in %.2f waits (longest %.2f ms); stall: queue full %llu (%.2f ms), deepest queue %llu; "
             "drains: submit %llu, pNext %llu (%llu calls), no memory %llu, caller %llu; chunk allocations %llu (oversize %llu, failed %llu)",
        mode, report_ns ? double(now - report_ns) * 1e-9 : 0.0, (unsigned long long)s.subs, (unsigned long long)direct, (unsigned long long)s.direct[nbmtq::NBMT_FB_POOL],
        (unsigned long long)s.direct[nbmtq::NBMT_FB_AB], (unsigned long long)s.direct[nbmtq::NBMT_FB_THREAD], (unsigned long long)s.direct[nbmtq::NBMT_FB_NESTED],
        double(s.cmds) / subs, double(s.chunks) / subs, double(s.bytes) / subs / 1024.0, double(w.ns) * ms / subs, double(w.chunks) / subs, double(w.sleeps) / subs, double(s.wakes) / subs,
        double(s.help_ns) * ms / subs, double(s.help_chunks) / subs, double(s.inline_ns) * ms / subs, double(s.wait_ns) * ms / subs, double(s.waits) / subs, double(s.wait_max_ns) * ms,
        (unsigned long long)s.full, double(s.full_ns) * ms, (unsigned long long)s.depth_max,
        (unsigned long long)s.sync[nbmtq::NBMT_SY_SUBMIT], (unsigned long long)s.sync[nbmtq::NBMT_SY_PNEXT], (unsigned long long)pn, (unsigned long long)s.sync[nbmtq::NBMT_SY_ALLOC],
        (unsigned long long)s.sync[nbmtq::NBMT_SY_CALLER], (unsigned long long)s.mallocs, (unsigned long long)s.big, (unsigned long long)s.alloc_fail);
    if (mode == 2) xbg_logf("[nb] mt check: chunks %llu, bad %llu (hash before replay %llu, hash after replay %llu, hand-over order %llu, packet walk %llu)%s",
        (unsigned long long)w.checked, (unsigned long long)(w.bad[0] + w.bad[1] + w.bad[2] + w.bad[3]), (unsigned long long)w.bad[nbmtq::NBMT_BAD_HASH_PRE],
        (unsigned long long)w.bad[nbmtq::NBMT_BAD_HASH_POST], (unsigned long long)w.bad[nbmtq::NBMT_BAD_ORDER], (unsigned long long)w.bad[nbmtq::NBMT_BAD_WALK],
        w.bad_seq ? " - FIRST BAD CHUNK SEEN, do not trust DM_NB_MT=3" : "");
    if (q.prof_on) {   // [DOS2] NR-CPU DM_NR_CPU=1: "[nr] cpu mt" - calls a submission x ns a call (1 in 16 timed) = ms a submission, by vkCmd*
        static const char *const names[OP_COUNT] = {"?", "BeginRenderPass", "EndRenderPass", "BindPipeline", "BindDescriptorSets", "BindVertexBuffers", "BindIndexBuffer",
            "PushConstants", "SetViewport", "SetScissor", "Draw", "DrawIndexed", "DrawIndirect", "DrawIndexedIndirect", "DrawIndirectCount", "DrawIndexedIndirectCount",
            "Dispatch", "DispatchIndirect", "ClearAttachments", "ClearColorImage", "ClearDepthStencilImage", "CopyBuffer", "CopyBufferToImage", "CopyImageToBuffer",
            "CopyImage", "BlitImage", "ResolveImage", "FillBuffer", "PipelineBarrier", "BeginQuery", "EndQuery", "ResetQueryPool", "WriteTimestamp",
            "CopyQueryPoolResults", "BeginConditionalRendering", "EndConditionalRendering"};
        struct Row { unsigned op; double calls, ns, ms; } r[OP_COUNT]; unsigned nr = 0; double tot = 0, totc = 0;
        for (unsigned op = 0; op < unsigned(OP_COUNT); op++) {
            const uint64_t c = q.prof.calls[op].exchange(0, std::memory_order_relaxed), t = q.prof.timed[op].exchange(0, std::memory_order_relaxed), ns = q.prof.ns[op].exchange(0, std::memory_order_relaxed);
            if (!c) continue;
            const double per = t ? double(ns) / double(t) : 0.0; r[nr++] = {op, double(c) / subs, per, double(c) * per * 1e-6 / subs}; tot += double(c) * per * 1e-6 / subs; totc += double(c) / subs;
        }
        std::sort(r, r + nr, [](const Row &a, const Row &b) { return a.ms > b.ms; });
        char l[1400]; int at = snprintf(l, sizeof l, "[nr] cpu mt (DM_NR_CPU=1, NR-CPU; 1 in 16 packets timed): replay %.2f ms in %.0f vkCmd* a submission (by the timed sample);", tot, totc);
        for (unsigned i = 0; i < nr && at < int(sizeof l) - 90; i++) at += snprintf(l + at, sizeof l - at, " %s %.1f x %.0f ns = %.2f ms;", names[r[i].op], r[i].calls, r[i].ns, r[i].ms);
        xbg_logf("%s", l);
    }
}
// recording_slot(): the slot's command buffer has just begun. own_pool: the slot has a command pool of its own.
void begin(VkCommandBuffer cmd, bool own_pool, bool ab_is_off) {
    if (!mode) return;
    if (cur) { ++q.st.direct[nbmtq::NBMT_FB_NESTED]; q.end(); cur = VK_NULL_HANDLE; }
    if (!own_pool) { ++q.st.direct[nbmtq::NBMT_FB_POOL]; return; }
    if (ab_is_off) { ++q.st.direct[nbmtq::NBMT_FB_AB]; return; }
    if (!q.start()) { ++q.st.direct[nbmtq::NBMT_FB_THREAD]; return; }
    q.begin(cmd); cur = cmd;
}
// submit_slots(): before vkEndCommandBuffer. Everything is in the command buffer when this returns.
void end(VkCommandBuffer cmd) {
    NBMT_PRE(cmd, OP_COUNT + 1);   // [NR-21] the submission ends: queued draws first (nrsort W_SUBMIT)
    if (!mode) return;
    if (cur && cur == cmd) { q.end(); cur = VK_NULL_HANDLE; }
    const uint64_t now = nbmtq::now_ns();
    if (!report_ns) report_ns = now;
    else if (now - report_ns >= 10000000000ull) { report(now); report_ns = now; }
}
// A caller about to use the command buffer in a way the wrappers do not see: drain first (packets go on afterwards).
inline void sync(VkCommandBuffer cmd) { NBMT_PRE(cmd, OP_COUNT + 2); if (cur && cur == cmd) q.sync(nbmtq::NBMT_SY_CALLER); }   // [NR-21] nrsort W_SYNC
void stop() { if (!mode) return; q.stop(); cur = VK_NULL_HANDLE; q.trim(); }
} // namespace nbmt
} // namespace

// Every vkCmd* the translation unit uses, from here on: the wrapper (direct call unless the command buffer is nbmt::cur).
#define vkCmdBeginRenderPass nbmt::CmdBeginRenderPass
#define vkCmdEndRenderPass nbmt::CmdEndRenderPass
#define vkCmdNextSubpass nbmt::CmdNextSubpass
#define vkCmdBindPipeline nbmt::CmdBindPipeline
#define vkCmdBindDescriptorSets nbmt::CmdBindDescriptorSets
#define vkCmdBindVertexBuffers nbmt::CmdBindVertexBuffers
#define vkCmdBindIndexBuffer nbmt::CmdBindIndexBuffer
#define vkCmdPushConstants nbmt::CmdPushConstants
#define vkCmdSetViewport nbmt::CmdSetViewport
#define vkCmdSetScissor nbmt::CmdSetScissor
#define vkCmdDraw nbmt::CmdDraw
#define vkCmdDrawIndexed nbmt::CmdDrawIndexed
#define vkCmdDrawIndirect nbmt::CmdDrawIndirect
#define vkCmdDrawIndexedIndirect nbmt::CmdDrawIndexedIndirect
#define vkCmdDispatch nbmt::CmdDispatch
#define vkCmdDispatchIndirect nbmt::CmdDispatchIndirect
#define vkCmdClearAttachments nbmt::CmdClearAttachments
#define vkCmdClearColorImage nbmt::CmdClearColorImage
#define vkCmdClearDepthStencilImage nbmt::CmdClearDepthStencilImage
#define vkCmdCopyBuffer nbmt::CmdCopyBuffer
#define vkCmdCopyBufferToImage nbmt::CmdCopyBufferToImage
#define vkCmdCopyImageToBuffer nbmt::CmdCopyImageToBuffer
#define vkCmdCopyImage nbmt::CmdCopyImage
#define vkCmdBlitImage nbmt::CmdBlitImage
#define vkCmdResolveImage nbmt::CmdResolveImage
#define vkCmdFillBuffer nbmt::CmdFillBuffer
#define vkCmdPipelineBarrier nbmt::CmdPipelineBarrier
#define vkCmdBeginQuery nbmt::CmdBeginQuery
#define vkCmdEndQuery nbmt::CmdEndQuery
#define vkCmdResetQueryPool nbmt::CmdResetQueryPool
#define vkCmdWriteTimestamp nbmt::CmdWriteTimestamp
#define vkCmdCopyQueryPoolResults nbmt::CmdCopyQueryPoolResults
