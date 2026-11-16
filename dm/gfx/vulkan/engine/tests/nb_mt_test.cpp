// [DOS2] NB-20b host test: DM_NB_MT - the production wrappers and switches of ../xbg_nb_mt.inc and the queue of dm/gfx/nb/nb_mt.h
// against a mock Vulkan (a "command buffer" is a byte log; every mock vkCmd* appends its name id, its scalar arguments and the contents
// of its arrays, field by field, written independently of the packet code).
// - identity: random command programs (every wrapped command, arrays of 0..17 elements and oversize ones, null arrays, pNext chains,
//   commands on a second command buffer, drains in the middle, submissions refused for a shared pool / A/B / stop + restart) give the
//   byte-identical log per command buffer in modes 1, 2, 3 as in mode 0 (direct calls), for several queue shapes (tiny chunks, hand-over
//   every 16 packets, a queue of 2 chunks with a slow driver);
// - accounting: packets written = packets replayed by the worker + by the translate thread; nothing queued after nbmt::end;
// - mode 2: no finding on a clean run; a chunk changed after hand-over is reported (hash before replay);
// - stall paths taken: queue full, help at the submit, waits; no memory leak / race: run under the sanitizers.
// Build (Vulkan headers: any vulkan/ + vk_video/ include tree, e.g. a copy of the NDK sysroot's usr/include/vulkan and vk_video):
//   g++ -std=c++17 -O1 -g -Wall -Wextra -pthread -I<vk> nb_mt_test.cpp -o nb_mt_test                    && ./nb_mt_test
//   g++ -std=c++17 -O1 -g -fsanitize=thread -pthread -I<vk> nb_mt_test.cpp -o nb_mt_tsan                && ./nb_mt_tsan
//   g++ -std=c++17 -O1 -g -fsanitize=address,undefined -pthread -I<vk> nb_mt_test.cpp -o nb_mt_asan     && ./nb_mt_asan
//   clang++ -std=c++17 -O2 -Wall -Wextra -I<vk> nb_mt_test.cpp -o nb_mt_test.exe                        (Windows host, no sanitizer)
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

#include "nb_mt_mock.h"   // the mock Vulkan (shared with nb_mx_test.cpp)

#define NBMT_REAL(f) mock_##f
#include "../xbg_nb_mt.inc"

// ---------------- the random program ----------------
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Rng { uint64_t s; uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); } uint32_t upto(uint32_t n) { return n ? next() % n : 0; } };
template <class T> static T H(Rng &r) { return T(uintptr_t(0x1000 + (r.next() & 0xfffff) * 16)); }   // a fake handle
template <class T> static void fill(Rng &r, T &v) { uint8_t *p = reinterpret_cast<uint8_t *>(&v); for (size_t i = 0; i < sizeof v; i++) p[i] = uint8_t(r.next()); }
static uint32_t count(Rng &r) { const uint32_t k = r.upto(100); return k < 15 ? 0 : k < 60 ? 1 : k < 97 ? 2 + r.upto(16) : 3000 + r.upto(3000); }   // some arrays larger than a chunk

struct Cfg { int mode; uint32_t flush, chunk_kb, queue; int slow; bool safe_only; };
struct Out { std::vector<std::vector<uint8_t>> logs; uint64_t calls = 0; };

static void one_command(Rng &r, VkCommandBuffer c, bool safe_only) {
    const uint32_t k = safe_only ? (r.upto(2) ? 10u : 3u) : 1 + r.upto(35);
    switch (k) {
    case 1: { VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; VkBaseInStructure chain{}; if (r.upto(40) == 0) b.pNext = &chain;
        b.renderPass = H<VkRenderPass>(r); b.framebuffer = H<VkFramebuffer>(r); fill(r, b.renderArea); const uint32_t n = r.upto(10); std::vector<VkClearValue> cv(n + 1); for (auto &v : cv) fill(r, v);
        b.clearValueCount = n; b.pClearValues = r.upto(8) || n ? cv.data() : nullptr; vkCmdBeginRenderPass(c, &b, r.upto(2) ? VK_SUBPASS_CONTENTS_INLINE : VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS); break; }
    case 2: vkCmdEndRenderPass(c); break;
    case 3: vkCmdBindPipeline(c, r.upto(2) ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE, H<VkPipeline>(r)); break;
    case 4: { const uint32_t n = 1 + r.upto(3), nd = r.upto(9); std::vector<VkDescriptorSet> s(n); for (auto &v : s) v = H<VkDescriptorSet>(r); std::vector<uint32_t> d(nd + 1); for (auto &v : d) v = r.next();
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, H<VkPipelineLayout>(r), r.upto(3), n, s.data(), nd, nd ? d.data() : nullptr); break; }
    case 5: { const uint32_t n = 1 + r.upto(16); std::vector<VkBuffer> b(n); std::vector<VkDeviceSize> o(n); for (uint32_t i = 0; i < n; i++) { b[i] = H<VkBuffer>(r); o[i] = r.next(); }
        vkCmdBindVertexBuffers(c, r.upto(8), n, b.data(), o.data()); break; }
    case 6: vkCmdBindIndexBuffer(c, H<VkBuffer>(r), VkDeviceSize(r.next()) << 8, r.upto(2) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32); break;
    case 7: { const uint32_t n = r.upto(129); std::vector<uint8_t> v(n + 1); for (auto &x : v) x = uint8_t(r.next()); vkCmdPushConstants(c, H<VkPipelineLayout>(r), r.next(), r.upto(64), n, v.data()); break; }
    case 8: { const uint32_t n = 1 + r.upto(4); std::vector<VkViewport> v(n); for (auto &x : v) fill(r, x); vkCmdSetViewport(c, r.upto(2), n, v.data()); break; }
    case 9: { const uint32_t n = 1 + r.upto(4); std::vector<VkRect2D> v(n); for (auto &x : v) fill(r, x); vkCmdSetScissor(c, r.upto(2), n, v.data()); break; }
    case 10: vkCmdDraw(c, r.next(), r.next(), r.next(), r.next()); break;
    case 11: vkCmdDrawIndexed(c, r.next(), r.next(), r.next(), int32_t(r.next()), r.next()); break;
    case 12: vkCmdDrawIndirect(c, H<VkBuffer>(r), r.next(), r.next(), r.next()); break;
    case 13: vkCmdDrawIndexedIndirect(c, H<VkBuffer>(r), r.next(), r.next(), r.next()); break;
    case 14: nbmt::PDrawIndirectCount(mock_DrawIndirectCount, c, H<VkBuffer>(r), r.next(), H<VkBuffer>(r), r.next(), r.next(), r.next()); break;
    case 15: nbmt::PDrawIndexedIndirectCount(mock_DrawIndexedIndirectCount, c, H<VkBuffer>(r), r.next(), H<VkBuffer>(r), r.next(), r.next(), r.next()); break;
    case 16: vkCmdDispatch(c, r.next(), r.next(), r.next()); break;
    case 17: vkCmdDispatchIndirect(c, H<VkBuffer>(r), r.next()); break;
    case 18: { const uint32_t na = r.upto(10), nr = r.upto(4); std::vector<VkClearAttachment> a(na + 1); std::vector<VkClearRect> rc(nr + 1); for (auto &x : a) fill(r, x); for (auto &x : rc) fill(r, x);
        vkCmdClearAttachments(c, na, a.data(), nr, rc.data()); break; }
    case 19: { VkClearColorValue v; fill(r, v); const uint32_t n = count(r) % 40; std::vector<VkImageSubresourceRange> rg(n + 1); for (auto &x : rg) fill(r, x);
        vkCmdClearColorImage(c, H<VkImage>(r), VK_IMAGE_LAYOUT_GENERAL, &v, n, rg.data()); break; }
    case 20: { VkClearDepthStencilValue v; fill(r, v); const uint32_t n = 1 + r.upto(3); std::vector<VkImageSubresourceRange> rg(n); for (auto &x : rg) fill(r, x);
        vkCmdClearDepthStencilImage(c, H<VkImage>(r), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, n, rg.data()); break; }
    case 21: { const uint32_t n = count(r); std::vector<VkBufferCopy> rg(n + 1); for (auto &x : rg) fill(r, x); vkCmdCopyBuffer(c, H<VkBuffer>(r), H<VkBuffer>(r), n, n || r.upto(2) ? rg.data() : nullptr); break; }
    case 22: { const uint32_t n = count(r); std::vector<VkBufferImageCopy> rg(n + 1); for (auto &x : rg) fill(r, x); vkCmdCopyBufferToImage(c, H<VkBuffer>(r), H<VkImage>(r), VK_IMAGE_LAYOUT_GENERAL, n, rg.data()); break; }
    case 23: { const uint32_t n = count(r); std::vector<VkBufferImageCopy> rg(n + 1); for (auto &x : rg) fill(r, x); vkCmdCopyImageToBuffer(c, H<VkImage>(r), VK_IMAGE_LAYOUT_GENERAL, H<VkBuffer>(r), n, rg.data()); break; }
    case 24: { const uint32_t n = 1 + r.upto(17); std::vector<VkImageCopy> rg(n); for (auto &x : rg) fill(r, x); vkCmdCopyImage(c, H<VkImage>(r), VK_IMAGE_LAYOUT_GENERAL, H<VkImage>(r), VK_IMAGE_LAYOUT_GENERAL, n, rg.data()); break; }
    case 25: { const uint32_t n = 1 + r.upto(3); std::vector<VkImageBlit> rg(n); for (auto &x : rg) fill(r, x);
        vkCmdBlitImage(c, H<VkImage>(r), VK_IMAGE_LAYOUT_GENERAL, H<VkImage>(r), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, rg.data(), r.upto(2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST); break; }
    case 26: { VkImageResolve rg; fill(r, rg); vkCmdResolveImage(c, H<VkImage>(r), VK_IMAGE_LAYOUT_GENERAL, H<VkImage>(r), VK_IMAGE_LAYOUT_GENERAL, 1, &rg); break; }
    case 27: vkCmdFillBuffer(c, H<VkBuffer>(r), r.next(), r.next(), r.next()); break;
    case 28: { const uint32_t nm = r.upto(3), nb = r.upto(3), ni = r.upto(20); std::vector<VkMemoryBarrier> m(nm + 1); std::vector<VkBufferMemoryBarrier> b(nb + 1); std::vector<VkImageMemoryBarrier> im(ni + 1);
        VkBaseInStructure chain{};
        for (auto &x : m) { x = VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; x.srcAccessMask = r.next(); x.dstAccessMask = r.next(); }
        for (auto &x : b) { x = VkBufferMemoryBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; x.srcAccessMask = r.next(); x.dstAccessMask = r.next(); x.srcQueueFamilyIndex = r.next(); x.dstQueueFamilyIndex = r.next(); x.buffer = H<VkBuffer>(r); x.offset = r.next(); x.size = r.next(); }
        for (auto &x : im) { x = VkImageMemoryBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; x.srcAccessMask = r.next(); x.dstAccessMask = r.next(); x.oldLayout = VkImageLayout(r.upto(8)); x.newLayout = VkImageLayout(r.upto(8));
            x.srcQueueFamilyIndex = r.next(); x.dstQueueFamilyIndex = r.next(); x.image = H<VkImage>(r); fill(r, x.subresourceRange); }
        if (ni && r.upto(30) == 0) im[r.upto(ni)].pNext = &chain;
        if (nm && r.upto(60) == 0) m[0].pNext = &chain;
        vkCmdPipelineBarrier(c, r.next(), r.next(), r.upto(2), nm, nm ? m.data() : nullptr, nb, nb ? b.data() : nullptr, ni, ni ? im.data() : nullptr); break; }
    case 29: vkCmdBeginQuery(c, H<VkQueryPool>(r), r.next(), r.upto(2)); break;
    case 30: vkCmdEndQuery(c, H<VkQueryPool>(r), r.next()); break;
    case 31: vkCmdResetQueryPool(c, H<VkQueryPool>(r), r.next(), r.next()); break;
    case 32: vkCmdWriteTimestamp(c, r.upto(2) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, H<VkQueryPool>(r), r.next()); break;
    case 33: vkCmdCopyQueryPoolResults(c, H<VkQueryPool>(r), r.next(), r.next(), H<VkBuffer>(r), r.next(), 8, r.upto(4)); break;
    case 34: { VkConditionalRenderingBeginInfoEXT b{VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT}; VkBaseInStructure chain{}; if (r.upto(20) == 0) b.pNext = &chain;
        b.buffer = H<VkBuffer>(r); b.offset = r.next(); b.flags = r.upto(2); nbmt::PBeginConditionalRendering(mock_BeginCond, c, &b); break; }
    default: nbmt::PEndConditionalRendering(mock_EndCond, c); break;
    }
}

static nbmtq::Stats g_st; static nbmtq::WorkerStats g_ws; static uint64_t g_pnext;
static void fold() {   // totals of a run (report() resets the windows; here the windows are summed)
    const nbmtq::Stats &s = nbmt::q.st;
    g_st.subs += s.subs; g_st.cmds += s.cmds; g_st.chunks += s.chunks; g_st.big += s.big; g_st.full += s.full; g_st.waits += s.waits; g_st.help_chunks += s.help_chunks; g_st.help_cmds += s.help_cmds;
    g_st.alloc_fail += s.alloc_fail; for (int i = 0; i < nbmtq::NBMT_SY_KINDS; i++) g_st.sync[i] += s.sync[i]; for (int i = 0; i < nbmtq::NBMT_FB_KINDS; i++) g_st.direct[i] += s.direct[i];
    if (s.depth_max > g_st.depth_max) g_st.depth_max = s.depth_max;
    nbmt::q.st = nbmtq::Stats{};
    std::lock_guard<std::mutex> l(nbmt::q.mu); const nbmtq::WorkerStats &w = nbmt::q.ws;
    g_ws.chunks += w.chunks; g_ws.cmds += w.cmds; g_ws.checked += w.checked; g_ws.sleeps += w.sleeps; for (int i = 0; i < nbmtq::NBMT_BAD_KINDS; i++) g_ws.bad[i] += w.bad[i];
    nbmt::q.ws = nbmtq::WorkerStats{};
    g_pnext += nbmt::pnext_calls; nbmt::pnext_calls = 0;
}

static Out run(uint64_t seed, const Cfg &cfg, unsigned submissions, unsigned per_sub, bool corrupt = false) {
    char e[4][64]; snprintf(e[0], 64, "%d", cfg.mode); snprintf(e[1], 64, "%u", cfg.flush); snprintf(e[2], 64, "%u", cfg.chunk_kb); snprintf(e[3], 64, "%u", cfg.queue);
#if defined(_WIN32)
    _putenv_s("DM_NB_MT", e[0]); _putenv_s("DM_NB_MT_FLUSH", e[1]); _putenv_s("DM_NB_MT_CHUNK_KB", e[2]); _putenv_s("DM_NB_MT_QUEUE", e[3]);
#else
    setenv("DM_NB_MT", e[0], 1); setenv("DM_NB_MT_FLUSH", e[1], 1); setenv("DM_NB_MT_CHUNK_KB", e[2], 1); setenv("DM_NB_MT_QUEUE", e[3], 1);
#endif
    nbmt::init(); nbmt::q.test_corrupt = corrupt; g_slow.store(cfg.slow); g_st = nbmtq::Stats{}; g_ws = nbmtq::WorkerStats{}; g_pnext = 0;
    CHECK(nbmt::mode == cfg.mode && nbmt::q.flush_cmds == (cfg.flush < 16 ? 16u : cfg.flush), "switches read");
    Rng r{seed * 0x9E3779B97F4A7C15ull + 1}; Out out;
    for (unsigned s = 0; s < submissions; s++) {
        Log a, b; VkCommandBuffer ca = reinterpret_cast<VkCommandBuffer>(&a), cb = reinterpret_cast<VkCommandBuffer>(&b);
        const uint32_t kind = r.upto(40); const bool own_pool = kind != 0, ab_off = kind == 1;
        nbmt::report_ns = nbmtq::now_ns();                     // the 10 s line stays out of the run (it resets the window the checks below sum)
        nbmt::begin(ca, own_pool, ab_off);
        CHECK((nbmt::cur == ca) == (cfg.mode != 0 && own_pool && !ab_off), "begin: packets exactly when the mode, the pool and the A/B phase allow");
        const unsigned n = per_sub / 2 + r.upto(per_sub);
        for (unsigned i = 0; i < n; i++) {
            const uint32_t w = r.upto(200);
            if (w == 0) nbmt::sync(ca);                       // a caller that drains
            else if (w == 1) nbmt::sync(cb);                  // not the recorded buffer: nothing happens
            else one_command(r, w < 12 ? cb : ca, cfg.safe_only);
        }
        nbmt::end(ca);
        CHECK(nbmt::cur == VK_NULL_HANDLE, "end: no command buffer left in packet mode");
        { std::lock_guard<std::mutex> l(nbmt::q.mu); CHECK(!nbmt::q.head && !nbmt::q.busy && !nbmt::q.queued, "end: queue empty, nobody inside a chunk"); }
        for (unsigned i = r.upto(4); i; i--) one_command(r, ca, cfg.safe_only);   // after the end: direct (submit_slots still writes nothing, but the wrappers must pass through)
        nbmt::end(cb);                                         // an end for a buffer that was never begun: nothing
        out.calls += a.calls + b.calls; out.logs.push_back(std::move(a.b)); out.logs.push_back(std::move(b.b));
        if (r.upto(150) == 0) { fold(); nbmt::stop(); }        // the worker ends and is started again by the next begin
    }
    fold(); nbmt::stop(); g_slow.store(0); nbmt::q.test_corrupt = false;
    { std::lock_guard<std::mutex> l(nbmt::q.mu); CHECK(!nbmt::q.running && !nbmt::q.free_list && !nbmt::q.head, "stop: worker gone, chunk memory returned"); }
    return out;
}
static bool same(const Out &a, const Out &b, const char *what) {
    if (a.logs.size() != b.logs.size() || a.calls != b.calls) { CHECK(false, "%s: %zu logs / %llu calls against %zu / %llu", what, b.logs.size(), (unsigned long long)b.calls, a.logs.size(), (unsigned long long)a.calls); return false; }
    for (size_t i = 0; i < a.logs.size(); i++) if (a.logs[i] != b.logs[i]) { CHECK(false, "%s: command buffer log %zu differs (%zu / %zu bytes)", what, i, b.logs[i].size(), a.logs[i].size()); return false; }
    return true;
}

int main(int argc, char **argv) {
    const unsigned scale = argc > 1 ? unsigned(atoi(argv[1])) : 1;   // 0: quick (60 submissions a run), n: 400 n
    const unsigned subs = scale ? 400 * scale : 60, per = 400;
    uint64_t total = 0;
    for (uint64_t seed = 1; seed <= 3; seed++) {
        const Out ref = run(seed, Cfg{0, 256, 64, 64, 0, false}, subs, per);
        CHECK(g_st.subs == 0 && g_st.cmds == 0 && g_ws.chunks == 0, "mode 0: no packet, no chunk, no worker");
        total += ref.calls;
        const Cfg shapes[] = {
            {1, 256, 64, 64, 0, false}, {2, 256, 64, 64, 0, false}, {3, 256, 64, 64, 0, false},
            {1, 16, 16, 2, 0, false},   {2, 16, 16, 2, 0, false},   {3, 16, 16, 2, 0, false},
            {3, 16, 16, 2, 7, false},   {2, 32, 16, 4, 3, false},   {3, 65536, 1024, 1024, 0, false}, {3, 64, 16, 8, 50, false},
        };
        for (const Cfg &c : shapes) {
            char what[96]; snprintf(what, sizeof what, "seed %llu mode %d flush %u chunk %u KB queue %u slow %d", (unsigned long long)seed, c.mode, c.flush, c.chunk_kb, c.queue, c.slow);
            const Out o = run(seed, c, subs, per);
            same(ref, o, what);
            CHECK(g_st.subs > 0 && g_st.cmds > 0, "%s: packets written", what);
            CHECK(g_st.alloc_fail == 0, "%s: allocation", what);
            if (c.mode == 1) CHECK(g_ws.chunks == 0 && g_st.help_chunks == 0, "%s: mode 1 replays at hand-over", what);
            else CHECK(g_ws.cmds + g_st.help_cmds == g_st.cmds && g_ws.chunks + g_st.help_chunks == g_st.chunks, "%s: every packet replayed once (worker %llu + translate %llu of %llu)", what,
                       (unsigned long long)g_ws.cmds, (unsigned long long)g_st.help_cmds, (unsigned long long)g_st.cmds);
            if (c.mode == 2) CHECK(g_ws.checked == g_st.chunks && !g_ws.bad[0] && !g_ws.bad[1] && !g_ws.bad[2] && !g_ws.bad[3], "%s: check clean (%llu chunks)", what, (unsigned long long)g_ws.checked);
            CHECK((c.chunk_kb > 64 || g_st.big > 0) && g_pnext > 0 && g_st.sync[nbmtq::NBMT_SY_PNEXT] == g_pnext && g_st.sync[nbmtq::NBMT_SY_CALLER] > 0, "%s: oversize packets, pNext drains and caller drains happened", what);
            CHECK(g_st.direct[nbmtq::NBMT_FB_THREAD] == 0 && (subs < 400 || (g_st.direct[nbmtq::NBMT_FB_POOL] > 0 && g_st.direct[nbmtq::NBMT_FB_AB] > 0)), "%s: refused submissions counted", what);
            if (c.mode >= 2 && c.slow && c.queue <= 4) CHECK(g_st.full > 0 && g_st.help_chunks + g_st.waits > 0, "%s: a slow driver fills the queue (full %llu, helped %llu, waits %llu)", what, (unsigned long long)g_st.full, (unsigned long long)g_st.help_chunks, (unsigned long long)g_st.waits);
            if (c.mode >= 2) CHECK(g_st.depth_max <= c.queue, "%s: queue bound (%llu)", what, (unsigned long long)g_st.depth_max);
            printf("ok   %-64s packets %9llu chunks %7llu worker %7llu helped %6llu waits %6llu full %5llu big %4llu\n", what, (unsigned long long)g_st.cmds, (unsigned long long)g_st.chunks,
                   (unsigned long long)g_ws.chunks, (unsigned long long)g_st.help_chunks, (unsigned long long)g_st.waits, (unsigned long long)g_st.full, (unsigned long long)g_st.big);
        }
    }
    // mode 2 finds a chunk changed after hand-over (commands whose packets hold no pointer, so the changed byte is harmless to replay)
    { const Out ref = run(9, Cfg{0, 64, 64, 64, 0, true}, 40, 300); (void)ref;
      run(9, Cfg{2, 64, 64, 64, 0, true}, 40, 300, false); CHECK(!g_ws.bad[0] && !g_ws.bad[1] && !g_ws.bad[2] && !g_ws.bad[3] && g_ws.checked > 0, "check: clean run");
      run(9, Cfg{2, 64, 64, 64, 0, true}, 40, 300, true);
      CHECK(g_ws.bad[nbmtq::NBMT_BAD_HASH_PRE] == g_ws.checked && g_ws.checked > 0, "check: every changed chunk reported (%llu of %llu)", (unsigned long long)g_ws.bad[0], (unsigned long long)g_ws.checked);
      bool line = false, warn = false; for (const auto &t : g_lines) { if (t.find("[nb] mt check:") != std::string::npos) line = true; if (t.find("FIRST BAD CHUNK") != std::string::npos) warn = true; }
      nbmt::mode = 2; nbmt::q.st.subs = 1; { std::lock_guard<std::mutex> l(nbmt::q.mu); nbmt::q.ws.bad[0] = 1; nbmt::q.ws.bad_seq = 5; nbmt::q.ws.checked = 3; } nbmt::report(nbmtq::now_ns());
      for (const auto &t : g_lines) { if (t.find("[nb] mt check:") != std::string::npos) line = true; if (t.find("FIRST BAD CHUNK") != std::string::npos) warn = true; }
      CHECK(line && warn, "check: the report line and its warning"); nbmt::mode = 0; }
    bool mt_line = false; for (const auto &t : g_lines) if (t.find("[nb] mt (DM_NB_MT=") != std::string::npos && t.find("queue full") != std::string::npos && t.find("waited") != std::string::npos) mt_line = true;
    CHECK(mt_line, "the 10 s line with waits / stall / drains");
    printf("%s: nb_mt_test, %llu mock calls per reference run set, %zu log lines\n", fails ? "FAILED" : "all passed", (unsigned long long)total, g_lines.size());
    return fails ? 1 : 0;
}
