// [DOS2] NB-20b host tests: the mock Vulkan of nb_mt_test.cpp / nb_mx_test.cpp. A "command buffer" is a byte log: every mock vkCmd*
// appends its name id, its scalar arguments and the contents of its arrays, field by field (written independently of the packet code).
// Include after <vulkan/vulkan_core.h>, <atomic>, <vector>; then `#define NBMT_REAL(f) mock_##f` and include ../xbg_nb_mt.inc.
#ifndef NB_MT_MOCK_H
#define NB_MT_MOCK_H
// ---------------- mock Vulkan ----------------
struct Log { std::vector<uint8_t> b; uint64_t calls = 0; };
static Log &L(VkCommandBuffer c) { return *reinterpret_cast<Log *>(c); }
static std::atomic<int> g_slow{0};   // > 0: every g_slow-th mock call burns time (a slow driver)
static std::atomic<uint64_t> g_mock_calls{0};
template <class T> static void put(Log &l, const T &v) { const uint8_t *p = reinterpret_cast<const uint8_t *>(&v); l.b.insert(l.b.end(), p, p + sizeof v); }
static void putn(Log &l, const void *p, size_t n) { const uint8_t *q = static_cast<const uint8_t *>(p); l.b.insert(l.b.end(), q, q + n); }
static Log &head(VkCommandBuffer c, uint32_t id) {
    Log &l = L(c); ++l.calls; put(l, uint32_t(0xC0DE0000u | id));
    const int s = g_slow.load(std::memory_order_relaxed);
    if (s > 0 && g_mock_calls.fetch_add(1, std::memory_order_relaxed) % uint64_t(s) == 0) { volatile uint64_t x = 0; for (int i = 0; i < 20000; i++) x = x + uint64_t(i); }
    return l;
}
static void mock_vkCmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo *b, VkSubpassContents sc) {
    Log &l = head(c, 1); put(l, b->sType); put(l, uint8_t(b->pNext != nullptr)); put(l, b->renderPass); put(l, b->framebuffer); put(l, b->renderArea); put(l, b->clearValueCount);
    for (uint32_t i = 0; i < b->clearValueCount; i++) putn(l, &b->pClearValues[i], sizeof(VkClearValue));
    put(l, sc);
}
static void mock_vkCmdEndRenderPass(VkCommandBuffer c) { head(c, 2); }
static void mock_vkCmdNextSubpass(VkCommandBuffer c, VkSubpassContents sc) { Log &l = head(c, 40); put(l, sc); }   // NR-ONCHIP
static void mock_vkCmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipeline p) { Log &l = head(c, 3); put(l, bp); put(l, p); }
static void mock_vkCmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout lay, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    Log &l = head(c, 4); put(l, bp); put(l, lay); put(l, first); put(l, n); for (uint32_t i = 0; i < n; i++) put(l, s[i]); put(l, nd); for (uint32_t i = 0; i < nd; i++) put(l, d[i]);
}
static void mock_vkCmdBindVertexBuffers(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    Log &l = head(c, 5); put(l, first); put(l, n); for (uint32_t i = 0; i < n; i++) { put(l, b[i]); put(l, o[i]); }
}
static void mock_vkCmdBindIndexBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkIndexType t) { Log &l = head(c, 6); put(l, b); put(l, o); put(l, t); }
static void mock_vkCmdPushConstants(VkCommandBuffer c, VkPipelineLayout lay, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    Log &l = head(c, 7); put(l, lay); put(l, st); put(l, off); put(l, size); putn(l, v, size);
}
static void mock_vkCmdSetViewport(VkCommandBuffer c, uint32_t first, uint32_t n, const VkViewport *v) { Log &l = head(c, 8); put(l, first); put(l, n); for (uint32_t i = 0; i < n; i++) putn(l, &v[i], sizeof v[i]); }
static void mock_vkCmdSetScissor(VkCommandBuffer c, uint32_t first, uint32_t n, const VkRect2D *v) { Log &l = head(c, 9); put(l, first); put(l, n); for (uint32_t i = 0; i < n; i++) putn(l, &v[i], sizeof v[i]); }
static void mock_vkCmdDraw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e) { Log &l = head(c, 10); put(l, a); put(l, b); put(l, d); put(l, e); }
static void mock_vkCmdDrawIndexed(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, int32_t e, uint32_t f) { Log &l = head(c, 11); put(l, a); put(l, b); put(l, d); put(l, e); put(l, f); }
static void mock_vkCmdDrawIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { Log &l = head(c, 12); put(l, b); put(l, o); put(l, n); put(l, s); }
static void mock_vkCmdDrawIndexedIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { Log &l = head(c, 13); put(l, b); put(l, o); put(l, n); put(l, s); }
static void mock_DrawIndirectCount(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co, uint32_t n, uint32_t s) { Log &l = head(c, 14); put(l, b); put(l, o); put(l, cb); put(l, co); put(l, n); put(l, s); }
static void mock_DrawIndexedIndirectCount(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co, uint32_t n, uint32_t s) { Log &l = head(c, 15); put(l, b); put(l, o); put(l, cb); put(l, co); put(l, n); put(l, s); }
static void mock_vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) { Log &l = head(c, 16); put(l, x); put(l, y); put(l, z); }
static void mock_vkCmdDispatchIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o) { Log &l = head(c, 17); put(l, b); put(l, o); }
static void mock_vkCmdClearAttachments(VkCommandBuffer c, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r) {
    Log &l = head(c, 18); put(l, na); for (uint32_t i = 0; i < na; i++) { put(l, a[i].aspectMask); put(l, a[i].colorAttachment); putn(l, &a[i].clearValue, sizeof(VkClearValue)); }
    put(l, nr); for (uint32_t i = 0; i < nr; i++) { put(l, r[i].rect); put(l, r[i].baseArrayLayer); put(l, r[i].layerCount); }
}
static void ranges(Log &l, uint32_t n, const VkImageSubresourceRange *r) { put(l, n); for (uint32_t i = 0; i < n; i++) { put(l, r[i].aspectMask); put(l, r[i].baseMipLevel); put(l, r[i].levelCount); put(l, r[i].baseArrayLayer); put(l, r[i].layerCount); } }
static void mock_vkCmdClearColorImage(VkCommandBuffer c, VkImage im, VkImageLayout lay, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    Log &l = head(c, 19); put(l, im); put(l, lay); putn(l, v, sizeof *v); ranges(l, n, r);
}
static void mock_vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage im, VkImageLayout lay, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    Log &l = head(c, 20); put(l, im); put(l, lay); put(l, v->depth); put(l, v->stencil); ranges(l, n, r);
}
static void mock_vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) {
    Log &l = head(c, 21); put(l, s); put(l, d); put(l, n); for (uint32_t i = 0; i < n; i++) { put(l, r[i].srcOffset); put(l, r[i].dstOffset); put(l, r[i].size); }
}
static void bic(Log &l, uint32_t n, const VkBufferImageCopy *r) {
    put(l, n); for (uint32_t i = 0; i < n; i++) { put(l, r[i].bufferOffset); put(l, r[i].bufferRowLength); put(l, r[i].bufferImageHeight); put(l, r[i].imageSubresource.aspectMask);
        put(l, r[i].imageSubresource.mipLevel); put(l, r[i].imageSubresource.baseArrayLayer); put(l, r[i].imageSubresource.layerCount); put(l, r[i].imageOffset); put(l, r[i].imageExtent); }
}
static void mock_vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer s, VkImage d, VkImageLayout dl, uint32_t n, const VkBufferImageCopy *r) { Log &l = head(c, 22); put(l, s); put(l, d); put(l, dl); bic(l, n, r); }
static void mock_vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n, const VkBufferImageCopy *r) { Log &l = head(c, 23); put(l, s); put(l, sl); put(l, d); bic(l, n, r); }
static void sub(Log &l, const VkImageSubresourceLayers &s) { put(l, s.aspectMask); put(l, s.mipLevel); put(l, s.baseArrayLayer); put(l, s.layerCount); }
static void mock_vkCmdCopyImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    Log &l = head(c, 24); put(l, s); put(l, sl); put(l, d); put(l, dl); put(l, n);
    for (uint32_t i = 0; i < n; i++) { sub(l, r[i].srcSubresource); put(l, r[i].srcOffset); sub(l, r[i].dstSubresource); put(l, r[i].dstOffset); put(l, r[i].extent); }
}
static void mock_vkCmdBlitImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    Log &l = head(c, 25); put(l, s); put(l, sl); put(l, d); put(l, dl); put(l, n);
    for (uint32_t i = 0; i < n; i++) { sub(l, r[i].srcSubresource); put(l, r[i].srcOffsets[0]); put(l, r[i].srcOffsets[1]); sub(l, r[i].dstSubresource); put(l, r[i].dstOffsets[0]); put(l, r[i].dstOffsets[1]); }
    put(l, f);
}
static void mock_vkCmdResolveImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
    Log &l = head(c, 26); put(l, s); put(l, sl); put(l, d); put(l, dl); put(l, n);
    for (uint32_t i = 0; i < n; i++) { sub(l, r[i].srcSubresource); put(l, r[i].srcOffset); sub(l, r[i].dstSubresource); put(l, r[i].dstOffset); put(l, r[i].extent); }
}
static void mock_vkCmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize n, uint32_t v) { Log &l = head(c, 27); put(l, b); put(l, o); put(l, n); put(l, v); }
static void mock_vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags df, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb,
                                      const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    Log &l = head(c, 28); put(l, src); put(l, dst); put(l, df); put(l, nm);
    for (uint32_t i = 0; i < nm; i++) { put(l, m[i].sType); put(l, uint8_t(m[i].pNext != nullptr)); put(l, m[i].srcAccessMask); put(l, m[i].dstAccessMask); }
    put(l, nb);
    for (uint32_t i = 0; i < nb; i++) { put(l, b[i].sType); put(l, uint8_t(b[i].pNext != nullptr)); put(l, b[i].srcAccessMask); put(l, b[i].dstAccessMask); put(l, b[i].srcQueueFamilyIndex);
        put(l, b[i].dstQueueFamilyIndex); put(l, b[i].buffer); put(l, b[i].offset); put(l, b[i].size); }
    put(l, ni);
    for (uint32_t i = 0; i < ni; i++) { put(l, im[i].sType); put(l, uint8_t(im[i].pNext != nullptr)); put(l, im[i].srcAccessMask); put(l, im[i].dstAccessMask); put(l, im[i].oldLayout); put(l, im[i].newLayout);
        put(l, im[i].srcQueueFamilyIndex); put(l, im[i].dstQueueFamilyIndex); put(l, im[i].image); ranges(l, 1, &im[i].subresourceRange); }
}
static void mock_vkCmdBeginQuery(VkCommandBuffer c, VkQueryPool p, uint32_t i, VkQueryControlFlags f) { Log &l = head(c, 29); put(l, p); put(l, i); put(l, f); }
static void mock_vkCmdEndQuery(VkCommandBuffer c, VkQueryPool p, uint32_t i) { Log &l = head(c, 30); put(l, p); put(l, i); }
static void mock_vkCmdResetQueryPool(VkCommandBuffer c, VkQueryPool p, uint32_t f, uint32_t n) { Log &l = head(c, 31); put(l, p); put(l, f); put(l, n); }
static void mock_vkCmdWriteTimestamp(VkCommandBuffer c, VkPipelineStageFlagBits s, VkQueryPool p, uint32_t i) { Log &l = head(c, 32); put(l, s); put(l, p); put(l, i); }
static void mock_vkCmdCopyQueryPoolResults(VkCommandBuffer c, VkQueryPool p, uint32_t f, uint32_t n, VkBuffer b, VkDeviceSize o, VkDeviceSize s, VkQueryResultFlags fl) {
    Log &l = head(c, 33); put(l, p); put(l, f); put(l, n); put(l, b); put(l, o); put(l, s); put(l, fl);
}
static void mock_BeginCond(VkCommandBuffer c, const VkConditionalRenderingBeginInfoEXT *b) { Log &l = head(c, 34); put(l, b->sType); put(l, uint8_t(b->pNext != nullptr)); put(l, b->buffer); put(l, b->offset); put(l, b->flags); }
static void mock_EndCond(VkCommandBuffer c) { head(c, 35); }

#endif
