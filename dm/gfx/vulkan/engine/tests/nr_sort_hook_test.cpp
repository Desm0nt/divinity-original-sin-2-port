// [DOS2] NR-21 host test: the segment boundary of DM_NR_SORT is every vkCmd* wrapper of ../xbg_nb_mt.inc. With NBMT_PRE defined as in
// xbg_game.cpp (the command buffer with waiting draws -> nrsort::cut), the production wrappers run against the mock Vulkan of
// nb_mt_mock.h in DM_NB_MT modes 0 (direct), 1 (packets replayed by the caller) and 3 (worker): for each of the 35 wrapped commands
// (pNext variants included), nbmt::end and nbmt::sync, the hook fires once with that command's op, BEFORE the command, and the draws it
// records (through the same wrappers, as nrsort::flush does) land in the command buffer ahead of the command; a command on another
// command buffer does not fire it; the hook does not fire again for its own draws (pend is cleared first).
// Build (Vulkan headers: a copy of the NDK sysroot's usr/include/vulkan + vk_video):
//   clang++ -std=c++17 -O2 -Wall -Wextra -I<vk> nr_sort_hook_test.cpp -o nr_sort_hook_test && ./nr_sort_hook_test
#include <vulkan/vulkan_core.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>

static std::vector<std::string> g_lines;
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void xbg_logf(const char *fmt, ...) { char t[2048]; va_list a; va_start(a, fmt); vsnprintf(t, sizeof t, fmt, a); va_end(a); g_lines.push_back(t); }
#include "nb_mt_mock.h"

// the hook of xbg_game.cpp, with nrsort replaced by a recorder
namespace nrsort { std::atomic<VkCommandBuffer> pend{VK_NULL_HANDLE}; void cut(VkCommandBuffer c, unsigned op); }
#define NBMT_PRE(c, op) do { if (__builtin_expect((c) == nrsort::pend.load(std::memory_order_relaxed), 0) && (c)) nrsort::cut((c), (op)); } while (0)
#define NBMT_REAL(f) mock_##f
#include "../xbg_nb_mt.inc"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static std::vector<unsigned> g_ops;
static unsigned g_draws = 2;   // draws a segment "records"
void nrsort::cut(VkCommandBuffer c, unsigned op) {
    if (pend.load(std::memory_order_relaxed) != c) return;
    pend.store(VK_NULL_HANDLE, std::memory_order_relaxed);   // as nrsort::flush: cleared first, the segment's own commands do not re-enter
    g_ops.push_back(op);
    for (unsigned i = 0; i < g_draws; i++) { vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, VkPipeline(uintptr_t(0xF00D0 + i))); vkCmdDraw(c, 0xF1u, i, 7, 9); }
}

// one command of each kind: k = nbmt op (1..35); pnext: the variant with a pNext chain where the wrapper has one
static void command(unsigned k, VkCommandBuffer c, bool pnext) {
    VkBaseInStructure chain{}; const VkBuffer b = VkBuffer(uintptr_t(0x100)); const VkImage im = VkImage(uintptr_t(0x200)); const VkQueryPool qp = VkQueryPool(uintptr_t(0x300));
    const VkPipelineLayout lay = VkPipelineLayout(uintptr_t(0x400)); VkDeviceSize o = 16; uint32_t dyn[2] = {1, 2}; VkDescriptorSet set = VkDescriptorSet(uintptr_t(0x500));
    VkViewport vp{0, 0, 8, 8, 0, 1}; VkRect2D sc{{0, 0}, {8, 8}}; VkClearAttachment ca{}; VkClearRect cr{}; VkClearColorValue cv{}; VkClearDepthStencilValue dv{};
    VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; VkBufferCopy bc{0, 0, 4}; VkBufferImageCopy bic{}; VkImageCopy icp{}; VkImageBlit ib{}; VkImageResolve ir{};
    switch (k) {
    case 1: { VkClearValue cl{}; VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; bi.pNext = pnext ? &chain : nullptr; bi.clearValueCount = 1; bi.pClearValues = &cl;
        vkCmdBeginRenderPass(c, &bi, VK_SUBPASS_CONTENTS_INLINE); break; }
    case 2: vkCmdEndRenderPass(c); break;
    case 3: vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, VkPipeline(uintptr_t(0x600))); break;
    case 4: vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &set, 2, dyn); break;
    case 5: vkCmdBindVertexBuffers(c, 0, 1, &b, &o); break;
    case 6: vkCmdBindIndexBuffer(c, b, 0, VK_INDEX_TYPE_UINT16); break;
    case 7: vkCmdPushConstants(c, lay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, dyn); break;
    case 8: vkCmdSetViewport(c, 0, 1, &vp); break;
    case 9: vkCmdSetScissor(c, 0, 1, &sc); break;
    case 10: vkCmdDraw(c, 3, 1, 0, 0); break;
    case 11: vkCmdDrawIndexed(c, 3, 1, 0, 0, 0); break;
    case 12: vkCmdDrawIndirect(c, b, 0, 1, 16); break;
    case 13: vkCmdDrawIndexedIndirect(c, b, 0, 1, 20); break;
    case 14: nbmt::PDrawIndirectCount(mock_DrawIndirectCount, c, b, 0, b, 64, 4, 16); break;
    case 15: nbmt::PDrawIndexedIndirectCount(mock_DrawIndexedIndirectCount, c, b, 0, b, 64, 4, 20); break;
    case 16: vkCmdDispatch(c, 1, 1, 1); break;
    case 17: vkCmdDispatchIndirect(c, b, 0); break;
    case 18: vkCmdClearAttachments(c, 1, &ca, 1, &cr); break;
    case 19: vkCmdClearColorImage(c, im, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rg); break;
    case 20: vkCmdClearDepthStencilImage(c, im, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &rg); break;
    case 21: vkCmdCopyBuffer(c, b, b, 1, &bc); break;
    case 22: vkCmdCopyBufferToImage(c, b, im, VK_IMAGE_LAYOUT_GENERAL, 1, &bic); break;
    case 23: vkCmdCopyImageToBuffer(c, im, VK_IMAGE_LAYOUT_GENERAL, b, 1, &bic); break;
    case 24: vkCmdCopyImage(c, im, VK_IMAGE_LAYOUT_GENERAL, im, VK_IMAGE_LAYOUT_GENERAL, 1, &icp); break;
    case 25: vkCmdBlitImage(c, im, VK_IMAGE_LAYOUT_GENERAL, im, VK_IMAGE_LAYOUT_GENERAL, 1, &ib, VK_FILTER_NEAREST); break;
    case 26: vkCmdResolveImage(c, im, VK_IMAGE_LAYOUT_GENERAL, im, VK_IMAGE_LAYOUT_GENERAL, 1, &ir); break;
    case 27: vkCmdFillBuffer(c, b, 0, 4, 0); break;
    case 28: { VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; m.pNext = pnext ? &chain : nullptr; vkCmdPipelineBarrier(c, 1, 1, 0, 1, &m, 0, nullptr, 0, nullptr); break; }
    case 29: vkCmdBeginQuery(c, qp, 0, 0); break;
    case 30: vkCmdEndQuery(c, qp, 0); break;
    case 31: vkCmdResetQueryPool(c, qp, 0, 2); break;
    case 32: vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0); break;
    case 33: vkCmdCopyQueryPoolResults(c, qp, 0, 1, b, 0, 8, 0); break;
    case 34: { VkConditionalRenderingBeginInfoEXT bi{VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT}; bi.pNext = pnext ? &chain : nullptr; bi.buffer = b;
        nbmt::PBeginConditionalRendering(mock_BeginCond, c, &bi); break; }
    case 35: nbmt::PEndConditionalRendering(mock_EndCond, c); break;
    }
}
static uint32_t id_at(const Log &l, size_t at) { uint32_t v; memcpy(&v, l.b.data() + at, 4); return v; }
// the command ids of a log, in order (the mock heads are 0xC0DE00xx; argument bytes never start a scan here: we walk by known sizes
// of the marker pair and read the next head)
static void setenv_mode(int m) { char e[16]; snprintf(e, sizeof e, "%d", m);
#if defined(_WIN32)
    _putenv_s("DM_NB_MT", e);
#else
    setenv("DM_NB_MT", e, 1);
#endif
}

int main() {
    // the marker pair's bytes, recorded directly
    Log ref; { VkCommandBuffer r = reinterpret_cast<VkCommandBuffer>(&ref); for (unsigned i = 0; i < g_draws; i++) { mock_vkCmdBindPipeline(r, VK_PIPELINE_BIND_POINT_GRAPHICS, VkPipeline(uintptr_t(0xF00D0 + i))); mock_vkCmdDraw(r, 0xF1u, i, 7, 9); } }
    unsigned cases = 0;
    for (int mode : {0, 1, 3}) {
        setenv_mode(mode); nbmt::init(); CHECK(nbmt::mode == mode, "DM_NB_MT=%d read", mode);
        for (unsigned k = 1; k <= 37; k++) for (int pn = 0; pn < 2; pn++) for (int other = 0; other < 2; other++) {
            if (pn && !(k == 1 || k == 28 || k == 34)) continue;
            Log a, b2; VkCommandBuffer ca = reinterpret_cast<VkCommandBuffer>(&a), cb = reinterpret_cast<VkCommandBuffer>(&b2);
            // the command alone, without a segment: its bytes
            Log solo; VkCommandBuffer cs = reinterpret_cast<VkCommandBuffer>(&solo);
            nbmt::begin(cs, true, false); if (k <= 35) command(k, cs, pn); else if (k == 37) nbmt::sync(cs); nbmt::end(cs);
            g_ops.clear();
            nbmt::begin(ca, true, false);
            nrsort::pend.store(other ? cb : ca);          // other: the waiting draws belong to another command buffer
            if (k <= 35) command(k, ca, pn); else if (k == 37) nbmt::sync(ca);
            nbmt::end(ca);                                 // k == 36: nothing but the submission end
            const unsigned want = k <= 35 ? k : k == 36 ? unsigned(nbmt::OP_COUNT) + 1 : unsigned(nbmt::OP_COUNT) + 2;
            if (other) {
                CHECK(g_ops.empty() && a.b == solo.b && nrsort::pend.load() == cb, "mode %d op %u: a command on another command buffer fires nothing", mode, k);
                nrsort::pend.store(VK_NULL_HANDLE);
            } else {
                CHECK(g_ops.size() == 1 && g_ops[0] == want, "mode %d op %u: the hook fires once with the op (got %zu, first %u)", mode, k, g_ops.size(), g_ops.empty() ? 0u : g_ops[0]);
                CHECK(nrsort::pend.load() == VK_NULL_HANDLE, "mode %d op %u: pend cleared", mode, k);
                // the log = the segment's draws, then the command exactly as alone
                std::vector<uint8_t> want_log = ref.b; want_log.insert(want_log.end(), solo.b.begin(), solo.b.end());
                CHECK(a.b == want_log, "mode %d op %u%s: the segment's draws come first, the command after them unchanged (%zu bytes vs %zu)", mode, k, pn ? " (pNext)" : "", a.b.size(), want_log.size());
                if (k <= 35 && !solo.b.empty()) CHECK(id_at(a, ref.b.size()) == (0xC0DE0000u | k), "mode %d op %u: the next head is the command", mode, k);
            }
            ++cases;
        }
        nbmt::stop();
    }
    printf("%s: nr_sort_hook_test, %u cases (35 commands + 3 pNext variants + submission end + sync, own / other command buffer, DM_NB_MT 0 / 1 / 3)\n", fails ? "FAIL" : "all passed", cases);
    return fails ? 1 : 0;
}
