// [DOS2] NR-GMEM DM_GFX_PASS_GMEM (docs/dos2/nr/NR-GMEM.md, rule and mechanism in dm/gfx/nr/nr_gmem.h): our own GMEM / sysmem choice per
// render pass on Adreno + Turnip. Included by xbg_game.cpp right after xbg_nb_mt.inc, so every vkCreateRenderPass / vkCmdBeginRenderPass /
// vkCmdEndRenderPass of the translation unit goes through namespace pgm (the macros at the end), whatever path made the pass (create_pass,
// lazy-clear twins, NB twins, NR-ONCHIP's M, the clear / copy passes): one funnel, nothing to forget.
//   DM_GFX_PASS_GMEM   0  off (default): nothing is recorded or counted, Turnip stays as dm/env says (TU_DEBUG=sysmem unless set).
//                      2  count: each pass is judged when it ends; "NVN PERF pass-gmem" (per window) says what =1 would put in GMEM and
//                         why the rest stay in sysmem, "NVN PERF pass-gmem-top" (every DM_GFX_PASS_GMEM_TOP windows) the passes by
//                         signature (size, attachments, load / store / clear bytes, draws, first fragment shader key, Turnip-model bytes).
//                         Turnip is not touched (TU_DEBUG=sysmem): the picture and the timing are those of 0.
//                      1  apply: vk_driver.c loads Turnip with TU_DEBUG=gmem (unless dm/env sets TU_DEBUG) and gives every render pass a
//                         VERTEX -> VERTEX self-dependency; each pass the rule keeps in sysmem gets that barrier right before its
//                         vkCmdEndRenderPass (Turnip: rp.disable_gmem). Needs Turnip (driverID MESA_TURNIP) and dm_vk_pass_gmem_state() = 1,
//                         else it counts only (red line once). Boot only: TU_DEBUG is read when the driver loads.
//   DM_GFX_PASS_GMEM_DRAWS 5, _MIN_DRAWS 1, _ATTS 3, _PIXELS 600000, _BW 0 (1 = Turnip's bandwidth model must favour GMEM too), _TOP 10.
// Passes recorded outside this translation unit (dm/android/vulkan_present.cpp present pass, xbg_vulkan.cpp's 2D readback pass) call
// dm_vk_pass_end_sysmem() themselves: they stay in sysmem.
#include "../../nr/nr_gmem.h"
#include <mutex>
#include <unordered_map>
#include <string>
extern "C" __attribute__((weak)) int dm_vk_pass_gmem_state(void);   // dm/android/vk_driver.c: 1 = TU_DEBUG without sysmem + self-dependencies installed
namespace {
namespace pgm {
int mode = 0;                 // DM_GFX_PASS_GMEM
bool apply = false;           // mode 1 and the driver side is ready (Turnip, TU_DEBUG=gmem or empty, self-dependencies)
nrgm_cfg cfg = nrgm_cfg_default();
unsigned top_every = 10, windows = 0;
// What vkCreateRenderPass of this translation unit made (any thread may create; the begin lookup takes the lock too: ~70 a frame).
struct RpInfo { uint32_t subpasses = 0, atts = 0, cpp_load = 0, cpp_store = 0, cpp_clear = 0, depth_cpp = 0, stencil_cpp = 0; uint8_t color_cpp[8] {}; };
std::mutex rp_lock;
std::unordered_map<VkRenderPass, RpInfo> rps;
// One open pass per command buffer (the translation thread keeps one; the clear / copy passes use their own Commands).
struct Open { VkCommandBuffer cmd = VK_NULL_HANDLE; VkRenderPass rp = VK_NULL_HANDLE; bool known = false, lf = false; RpInfo info; nrgm_pass p {}; char ps[12] {}; };   // lf: [DOS2] NR-LIGHTFUSE keeps it in sysmem
Open open_[4];
// open_ and the counters: one lock (uncontended: the translation thread; DM_NB_MX windows may run translation on worker threads).
std::mutex st_lock;
// per window
uint64_t n_pass = 0, n_gmem = 0, n_barrier = 0, why_n[NRGM_W_N] {}, gmem_draws = 0, sys_bytes = 0, gmem_bytes = 0, all_sys_bytes = 0;
struct Agg { uint32_t w, h, atts, ld, st, cl, draws_max; uint64_t n, draws; int why; char ps[12]; uint64_t sys, gm; };
std::unordered_map<uint64_t, Agg> top;


inline Open *find(VkCommandBuffer c) { for (auto &o : open_) if (o.cmd == c) return &o; return nullptr; }
inline Open *slot(VkCommandBuffer c) {
    if (Open *o = find(c)) return o;
    for (auto &o : open_) if (!o.cmd) return &o;
    return &open_[0];   // more than 4 command buffers with an open pass: never in this engine; the oldest state is dropped
}

inline uint32_t env_u(const char *n, uint32_t d) { const char *e = getenv(n); return e && *e ? uint32_t(strtoul(e, nullptr, 0)) : d; }
void init(uint32_t driver_id) {
    const char *e = getenv("DM_GFX_PASS_GMEM"); mode = e && *e ? atoi(e) : 0; if (mode < 0 || mode >= NRGM_MODES) mode = 0;
    cfg.max_draws = env_u("DM_GFX_PASS_GMEM_DRAWS", cfg.max_draws); cfg.min_draws = env_u("DM_GFX_PASS_GMEM_MIN_DRAWS", cfg.min_draws);
    cfg.max_atts = env_u("DM_GFX_PASS_GMEM_ATTS", cfg.max_atts); cfg.min_pixels = env_u("DM_GFX_PASS_GMEM_PIXELS", uint32_t(cfg.min_pixels));
    cfg.bw = int(env_u("DM_GFX_PASS_GMEM_BW", 0)); top_every = env_u("DM_GFX_PASS_GMEM_TOP", 10);
    cfg.sub2 = env_u("DM_GFX_PASS_GMEM_M", 0) == 1u;   // [DOS2] NR-FRAME: NR-ONCHIP's M (Sky -> Light subpasses) may go GMEM (docs/dos2/nr/NR-FRAME.md)
    const int drv = dm_vk_pass_gmem_state ? dm_vk_pass_gmem_state() : -1;
    apply = mode == NRGM_APPLY && drv == 1 && driver_id == 18u /* VK_DRIVER_ID_MESA_TURNIP */;
    if (!mode) return;
    const char *tu = getenv("TU_DEBUG");
    xbg_logf("%sNVN DM_GFX_PASS_GMEM=%d (%s) draws %u..%u atts<=%u pixels>=%llu bw=%d M=%d; driverID=%u TU_DEBUG=%s driver side=%d%s",
             mode == NRGM_APPLY && !apply ? "【红】" : "", mode, apply ? "apply: passes the rule keeps in sysmem get the VERTEX barrier" : "count only",
             cfg.min_draws, cfg.max_draws, cfg.max_atts, (unsigned long long)cfg.min_pixels, cfg.bw, cfg.sub2, driver_id, tu ? tu : "(unset)", drv,
             mode == NRGM_APPLY && !apply ? ": =1 needs Turnip loaded with TU_DEBUG without sysmem (vk_driver.c sets gmem when dm/env has no TU_DEBUG)" : "");
}

// vkCreateRenderPass of the translation unit: the pass is created as asked (the driver loader adds the self-dependencies), its
// load / store / clear bytes are remembered for the begin.
VkResult create_rp(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    const VkResult r = ::vkCreateRenderPass(d, ci, a, out);
    if (r != VK_SUCCESS || !mode || !ci) return r;
    RpInfo i; i.subpasses = ci->subpassCount; i.atts = ci->attachmentCount;
    for (uint32_t k = 0; k < ci->attachmentCount; k++) {
        const VkAttachmentDescription &x = ci->pAttachments[k]; const uint32_t cpp = nrgm_cpp(uint32_t(x.format));
        const bool ds = x.format >= VK_FORMAT_D16_UNORM && x.format <= VK_FORMAT_D32_SFLOAT_S8_UINT;
        const bool st = x.format == VK_FORMAT_S8_UINT || x.format == VK_FORMAT_D16_UNORM_S8_UINT || x.format == VK_FORMAT_D24_UNORM_S8_UINT || x.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        // Turnip (tu_render_pass_bandwidth_config): load / store / clear of the attachment, depth and stencil aspects together
        const bool ld = x.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || (st && x.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
        const bool sto = x.storeOp == VK_ATTACHMENT_STORE_OP_STORE || (st && x.stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE);
        const bool clr = x.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || (st && x.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
        if (ld) i.cpp_load += cpp; if (sto) i.cpp_store += cpp; if (clr) i.cpp_clear += cpp;
        if (ds) { i.stencil_cpp = st ? 1u : 0u; i.depth_cpp = st && cpp > 1 ? cpp - 1u : cpp; if (x.format == VK_FORMAT_D24_UNORM_S8_UINT) i.depth_cpp = 4; }
    }
    if (ci->subpassCount) { const VkSubpassDescription &s = ci->pSubpasses[0];
        for (uint32_t k = 0; k < s.colorAttachmentCount && k < 8; k++) { const uint32_t at = s.pColorAttachments[k].attachment;
            if (at != VK_ATTACHMENT_UNUSED && at < ci->attachmentCount) i.color_cpp[k] = uint8_t(nrgm_cpp(uint32_t(ci->pAttachments[at].format))); } }
    std::lock_guard<std::mutex> g(rp_lock); rps[*out] = i;
    return r;
}
void begin_rp(VkCommandBuffer c, const VkRenderPassBeginInfo *b, VkSubpassContents sc) {
    if (mode) {
        std::lock_guard<std::mutex> lk(st_lock);
        Open &o = *slot(c); o = Open {}; o.cmd = c; o.rp = b->renderPass;
        { std::lock_guard<std::mutex> g(rp_lock); auto it = rps.find(b->renderPass); if (it != rps.end()) { o.known = true; o.info = it->second; } }
        o.p.w = b->renderArea.extent.width; o.p.h = b->renderArea.extent.height; o.p.layers = 1; o.p.subpasses = o.known ? o.info.subpasses : 0;
        o.p.atts = o.info.atts; o.p.cpp_load = o.info.cpp_load; o.p.cpp_store = o.info.cpp_store; o.p.cpp_clear = o.info.cpp_clear;
    }
    nbmt::CmdBeginRenderPass(c, b, sc);
}
// draw() / nr_draw on the open pass of command buffer c (begun): one more draw and its bytes per covered sample.
inline void note_draw(VkCommandBuffer c, uint32_t active_colors, uint32_t blend_en, uint32_t write_mask, bool depth, bool dt, bool dw, bool st, const char *ps) {
    if (!mode) return;
    std::lock_guard<std::mutex> lk(st_lock);
    Open *op = find(c); if (!op) return;
    Open &o = *op; uint32_t written = 0;
    for (unsigned i = 0; i < 8; i++) if ((active_colors >> i & 1u) && ((write_mask >> (4 * i)) & 15u)) written |= 1u << i;
    o.p.draw_bytes += nrgm_draw_bytes(o.info.color_cpp, written, blend_en, depth ? o.info.depth_cpp : 0, depth ? o.info.stencil_cpp : 0, depth && dt, depth && dw, depth && st);
    if (depth && (dw || st)) o.p.depth_written = 1;
    if (!o.p.draws++ && ps) snprintf(o.ps, sizeof o.ps, "%.8s", ps);
}
// [DOS2] NR-LIGHTFUSE (xbg_nr_lightfuse.inc): a pass of a fused Sky -> Light chain (Sky's draw-less pass, the fused Light pass) stays in sysmem
// (reason "other" in the window counts; the lightfuse line counts them): GMEM would move the same bytes for these one-draw passes.
inline void note_lightfuse(VkCommandBuffer c) { if (!mode) return; std::lock_guard<std::mutex> lk(st_lock); if (Open *o = find(c)) o->lf = true; }
inline void note_storage(VkCommandBuffer c) { if (!mode) return; std::lock_guard<std::mutex> lk(st_lock); if (Open *o = find(c)) o->p.storage = 1; }      // a draw wrote storage buffers / images
inline void note_depth_read(VkCommandBuffer c) { if (!mode) return; std::lock_guard<std::mutex> lk(st_lock); if (Open *o = find(c)) o->p.depth_read = 1; } // prepare_read: a draw samples the open pass's depth
void end_rp(VkCommandBuffer c) {
    bool sysmem = false;
    if (mode) {
        std::lock_guard<std::mutex> lk(st_lock);
        Open *o = find(c);
        if (o) {
            const int why = o->known && !o->lf ? nrgm_decide(&cfg, &o->p) : NRGM_W_OTHER;
            uint64_t s = 0, g = 0; nrgm_bytes(&o->p, &s, &g);
            ++n_pass; ++why_n[why]; all_sys_bytes += s;
            if (why == NRGM_W_GMEM) { ++n_gmem; gmem_draws += o->p.draws; sys_bytes += s; gmem_bytes += g; }
            else if (apply) { sysmem = true; ++n_barrier; }
            if (top_every) {
                const uint64_t k = (uint64_t(uintptr_t(o->rp)) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(o->p.w) << 40) ^ (uint64_t(o->p.h) << 20) ^ uint64_t(why) ^ (uint64_t(std::min(o->p.draws, 63u)) << 4)
                                   ^ (uint64_t(uint8_t(o->ps[0])) << 12) ^ (uint64_t(uint8_t(o->ps[1])) << 32) ^ (uint64_t(uint8_t(o->ps[7])) << 52);
                Agg &a = top[k];
                if (!a.n) { a.w = o->p.w; a.h = o->p.h; a.atts = o->p.atts; a.ld = o->p.cpp_load; a.st = o->p.cpp_store; a.cl = o->p.cpp_clear; a.why = why; memcpy(a.ps, o->ps, sizeof a.ps); }
                ++a.n; a.draws += o->p.draws; a.draws_max = std::max(a.draws_max, o->p.draws); a.sys += s; a.gm += g;
                if (top.size() > 4096) top.clear();
            }
            *o = Open {};
        }
    }
    if (sysmem) nbmt::CmdPipelineBarrier(c, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);   // Turnip: rp.disable_gmem
    nbmt::CmdEndRenderPass(c);
}
// NVN PERF window (perf_frame): per frame.
void report(double f) {
    if (!mode) return;
    std::lock_guard<std::mutex> lk(st_lock);
    char why[256]; int at = 0;
    for (int i = 1; i < NRGM_W_N; i++) at += snprintf(why + at, sizeof why - at, "%s%s=%.1f", i > 1 ? " " : "", nrgm_why_name[i], why_n[i] / f);
    xbg_logf("NVN PERF pass-gmem mode=%d apply=%d passes=%.1f gmem=%.1f (%s) barriers=%.1f gmem_draws=%.1f sysmem: %s; turnip-model MB picked sys/gmem=%.2f/%.2f all-sys=%.2f",
             mode, int(apply), n_pass / f, n_gmem / f, apply ? "in GMEM" : "would be GMEM", n_barrier / f, gmem_draws / f, why, sys_bytes / f / 1048576.0,
             gmem_bytes / f / 1048576.0, all_sys_bytes / f / 1048576.0);
    n_pass = n_gmem = n_barrier = gmem_draws = sys_bytes = gmem_bytes = all_sys_bytes = 0; for (auto &w : why_n) w = 0;
    if (!top_every || ++windows % top_every) return;
    std::vector<const Agg *> v; v.reserve(top.size()); for (auto &e : top) v.push_back(&e.second);
    std::sort(v.begin(), v.end(), [](const Agg *a, const Agg *b) {   // GMEM picks first, then the most frequent passes of any reason
        const bool ga = a->why == NRGM_W_GMEM, gb = b->why == NRGM_W_GMEM; return ga != gb ? ga : a->n > b->n; });
    const double tf = f * top_every; unsigned shown = 0;
    for (const Agg *a : v) {
        if (shown++ >= 16) break;
        xbg_logf("NVN PERF pass-gmem-top %ux%u att=%u ld/st/clr=%u/%u/%u B/px draws=%.1f(max %u) ps=%s n=%.2f/frame -> %s turnip-model KB sys/gmem=%.0f/%.0f",
                 a->w, a->h, a->atts, a->ld, a->st, a->cl, a->n ? double(a->draws) / a->n : 0.0, a->draws_max, a->ps[0] ? a->ps : "-", a->n / tf,
                 nrgm_why_name[a->why], a->n ? a->sys / 1024.0 / a->n : 0.0, a->n ? a->gm / 1024.0 / a->n : 0.0);
    }
    top.clear();
}
}  // namespace pgm
}  // namespace
#undef vkCmdBeginRenderPass
#undef vkCmdEndRenderPass
#define vkCmdBeginRenderPass pgm::begin_rp
#define vkCmdEndRenderPass pgm::end_rp
#define vkCreateRenderPass pgm::create_rp
