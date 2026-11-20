// [DOS2] NR-ZERO Z5 host test of the whole path (docs/dos2/nr/NR-ZERO.md section 22): the game thread writes the packets of the draws
// it has values for, the translation thread takes them draw by draw. dm/gfx/nr/nr_z5p.h (the published templates), nr_z5r.h (the
// run protocol with a verdict per draw), nbmtq::Queue (spare chunks, splice), the real wrappers of xbg_nb_mt.inc against the mock
// Vulkan of nb_mt_test.cpp (a "command buffer" is a byte log of the calls made on it). Two real threads.
//
// The world. A frame is a recording: engine draw records of SITES (the words a game thread has: key id, draw arguments, index buffer,
// vertex stream 0, the uniform addresses it bound - new ones every draw), other commands, pass changes. What the tail of a draw binds
// is the translation thread's knowledge (the event's resolved values): it changes under the game thread's feet - a site gets
// another pipeline or set, the constant ring moves, a mesh moves to another buffer, the viewport of a pass changes. Things only the
// translation thread sees: a draw its prologue drops, a draw after which another pipeline is left bound ("foreign"), a bind cache
// reset without a command ("silent"), commands the game thread did not count on, recordings never executed or recorded directly.
//   TODAY    every executed recording recorded by the translation thread alone, direct calls (mode 0): the reference logs.
//   APPLY    (DM_NR_Z5=1) the game thread runs ahead: slot of the record from its site words, the slot's template from the table,
//            values from the template + its own words, packets into a spare chunk, the values into the side chunk, the mark and the
//            slot into the record. The translation thread executes every record, asks Reader::draw at the command section; VALID:
//            nothing recorded, the real cache gets the draw's values; RECORD: recorded as today and the slot's template published.
//            NBMT_PRE (the hook of every wrapper) hands the held chunk over before anything else is written. The logs must be
//            byte-identical to TODAY's in modes 1 / 2 / 3 and several queue shapes; every spare chunk comes back.
//   VERIFY   (DM_NR_Z5=2) everything recorded by the translation thread; every draw =1 would have taken is compared packet for
//            packet with the game thread's chunk: bad 0, logs TODAY's.
//   SCRIPTS  one thread, exact outcomes draw by draw (all valid, a wrong value, a silent cache move, a command in between, a
//            dropped draw, alternating pipelines).
// The mutation check: built with -DNRZ5R_MUTATE=k (k = 1..14, nr_z5r.h) or -DNRZ5R_TEST_DROP=k (1 after a valid draw the real bind
// cache is not set to the draw's values, 2 the wrappers' hook does not hand the held chunk over, 3 a dropped draw does not cut its
// run) this test must FAIL.
// Build (Vulkan headers: any vulkan/ + vk_video/ include tree, e.g. the NDK sysroot's usr/include), from the repository root:
//   clang++ -std=c++17 -O2 -Wall -Wextra -I<vk> dm/gfx/vulkan/engine/tests/nr_z5r_test.cpp -o nr_z5r_test && ./nr_z5r_test
//   g++ -std=c++17 -O1 -g -fsanitize=thread -pthread -I<vk> dm/gfx/vulkan/engine/tests/nr_z5r_test.cpp -o nr_z5r_tsan && ./nr_z5r_tsan
#include <vulkan/vulkan_core.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef NRZ5R_TEST_DROP
#define NRZ5R_TEST_DROP 0
#endif

static std::vector<std::string> g_lines;
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void xbg_logf(const char *fmt, ...) { char t[2048]; va_list a; va_start(a, fmt); vsnprintf(t, sizeof t, fmt, a); va_end(a); g_lines.push_back(t); }
#include "nb_mt_mock.h"
#define NBMT_REAL(f) mock_##f
static void (*g_pre)() = nullptr;   // the engine's NBMT_PRE: before a wrapper writes anything (and at nbmt::end / sync)
#define NBMT_PRE(c, op) do { (void)(c); (void)(op); if (g_pre) g_pre(); } while (0)
#include "../xbg_nb_mt.inc"
#include "../../../nr/nr_z5p.h"
#include "../../../nr/nr_z5r.h"

static long fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
struct Rng { uint64_t s; uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); } uint32_t upto(uint32_t n) { return n ? next() % n : 0; } };
template <class H> static H hnd(uint64_t x) { return H(uintptr_t(x)); }

// ---------------- the world ----------------
enum { K_DRAW = 0, K_OTHER = 1, K_PASS = 2, K_SILENT = 3 };
struct Ev {
    int kind; bool foreign, dropped;
    // the record (what the game thread has)
    nrz5_site site; int32_t count, instances, first, base_vertex, base_instance; uint64_t ub[2][8];
    // what the translation thread resolves when it executes the record
    nrz5_vals v; uint32_t offs[40], first_index, k; uint8_t st[40], sl[40];
};
struct Site { uint32_t key; bool indexed; int32_t itype, count, instances, first, base_vertex, base_instance; uint64_t ibuf, vb0;
              uint64_t pipe, layout, set; uint32_t noff; uint8_t st[40], sl[40]; uint16_t used; uint64_t vb[16], vbo[16], ib, ibo; uint32_t it, first_index, k; };
struct World {
    Rng r; std::vector<Site> sites; std::vector<uint32_t> order; std::vector<uint8_t> evs; uint64_t ring_guest = 0x7300000000ull, ring_delta = 0x7300000000ull - 4096; uint32_t head = 0, vpgen = 0;
    void new_site(Site &s) {
        static const uint16_t useds[6] = {0x1, 0x3, 0x7, 0x5, 0x13, 0x8003};
        static const uint32_t noffs[7] = {0, 2, 4, 5, 5, 8, 9};   // (9: more than a template holds - never predicted)
        memset(&s, 0, sizeof s); s.key = 1 + r.upto(3000); s.indexed = r.upto(8) != 0; s.itype = s.indexed ? 1 + int32_t(r.upto(2)) : 0;
        s.count = int32_t(3 + r.upto(3000)); s.instances = s.indexed ? 1 + int32_t(r.upto(2)) : 1; s.first = s.indexed ? 0 : int32_t(r.upto(5000));
        s.base_vertex = s.indexed ? int32_t(r.upto(4000)) - 100 : 0; s.base_instance = s.indexed ? int32_t(r.upto(2)) : 0;
        s.ibuf = s.indexed ? 0x7100000000ull + 2 * r.upto(5000) : 0; s.vb0 = 0x7200000000ull + 16 * r.upto(5000);
        s.pipe = 0x1000 + r.upto(5); s.layout = 0x2000 + r.upto(2); s.set = 0x3000 + r.upto(3);
        s.noff = noffs[r.upto(r.upto(10) ? 6 : 7)]; for (uint32_t i = 0; i < s.noff; i++) { s.st[i] = uint8_t(i & 1); s.sl[i] = uint8_t(i / 2); }
        s.used = useds[r.upto(r.upto(6) ? 3 : 6)];
        for (uint32_t i = 0; i < 16; i++) if (s.used >> i & 1) { s.vb[i] = 0x4000 + r.upto(3); s.vbo[i] = 64 * r.upto(2); }
        s.ib = 0x5000 + r.upto(2); s.ibo = 2 * r.upto(2); s.it = r.upto(3) ? uint32_t(VK_INDEX_TYPE_UINT16) : uint32_t(VK_INDEX_TYPE_UINT32);
        s.first_index = s.indexed ? r.upto(5000) : 0; s.k = r.upto(3) ? 0 : r.upto(100);
    }
    explicit World(uint64_t seed) : r{seed * 0x9E3779B97F4A7C15ull + 7} {
        sites.resize(160); for (auto &s : sites) new_site(s);
        for (uint32_t i = 0; i < 400; i++) order.push_back(r.upto(5) ? i % 160 : r.upto(160));   // a frame: sites in a stable order, some twice
        for (uint32_t i = 0; i < 400; i++) evs.push_back(uint8_t(r.upto(100) < 2 ? (r.upto(3) ? K_PASS : K_OTHER) : 0));   // the frame's own passes and other commands, at the same places every frame
    }
    // The next frame. Between frames the translation thread's knowledge moves a little.
    std::vector<Ev> frame() {
        for (int n = 0; n < 3; n++) { Site &s = sites[r.upto(uint32_t(sites.size()))]; const uint32_t w = r.upto(6);
            if (w == 0) s.pipe = 0x1000 + r.upto(5); else if (w == 1) s.set = 0x3000 + r.upto(3); else if (w == 2) { const uint32_t i = uint32_t(__builtin_ctz(s.used)); s.vb[i] = 0x4000 + r.upto(3); }
            else if (w == 3) s.vbo[uint32_t(__builtin_ctz(s.used))] = 64 * r.upto(2); else if (w == 4) s.ibo = 2 * r.upto(2); else s.first_index = s.indexed ? r.upto(5000) : 0; }
        if (!r.upto(40)) ring_delta -= 256 * (1 + r.upto(4));   // the constant ring is served from another offset
        if (!r.upto(50)) ++vpgen;                               // the render size changed
        if (!r.upto(6)) order[r.upto(uint32_t(order.size()))] = r.upto(uint32_t(sites.size()));
        if (!r.upto(20)) new_site(sites[r.upto(uint32_t(sites.size()))]);
        std::vector<Ev> p; uint32_t pass = 0;
        for (size_t i = 0; i < order.size(); i++) {
            const uint32_t k = r.upto(1000);   // (and now and then one that was not there the frame before)
            const int ek = evs[i] ? int(evs[i]) : k < 2 ? K_OTHER : k < 3 ? K_PASS : k < 6 ? K_SILENT : 0;
            if (ek) { Ev e; memset(&e, 0, sizeof e); e.kind = ek; e.count = int32_t(r.next() & 0x7fffffff); if (evs[i] == K_PASS) ++pass; p.push_back(e); }   // (the viewport follows the frame's own passes)
            if (!r.upto(60)) continue;   // culled this frame
            const Site &s = sites[order[i]]; Ev e; memset(&e, 0, sizeof e); e.kind = K_DRAW; e.foreign = r.upto(50) == 0; e.dropped = r.upto(70) == 0;
            e.count = s.count; e.instances = s.instances; e.first = s.first; e.base_vertex = s.base_vertex; e.base_instance = s.base_instance;
            nrz5_site_make(&e.site, s.key, s.indexed ? NRZ5S_IDX : NRZ5S_ARRAYS, 4, s.itype, s.count, s.base_vertex, s.base_instance, s.instances, s.first, s.ibuf, s.vb0);
            nrz5_vals &v = e.v; v.pipe = s.pipe; v.layout = s.layout; v.set = s.set; v.noff = s.noff;
            for (uint32_t j = 0; j < s.noff; j++) { e.st[j] = s.st[j]; e.sl[j] = s.sl[j]; head = (head + 256 * (1 + r.upto(3))) & 0xffffff; e.ub[s.st[j]][s.sl[j]] = ring_guest + head; e.offs[j] = uint32_t(ring_guest + head - ring_delta); }
            { const uint32_t w = 640u << ((pass + vpgen) & 1); VkViewport vp{0, 0, float(w), 360, 0, 1}; memcpy(v.vp, &vp, sizeof vp); VkRect2D sc{{0, 0}, {w, 360}}; memcpy(v.sc, &sc, sizeof sc); }
            v.used = s.used; for (uint32_t j = 0; j < 16; j++) { v.vb[j] = s.vb[j]; v.vbo[j] = s.vbo[j]; }
            v.indexed = s.indexed; v.count = uint32_t(s.count); e.first_index = s.first_index; e.k = s.k;
            if (s.indexed) { v.ib = s.ib; v.ibo = s.ibo; v.it = s.it; v.instances = uint32_t(s.instances); v.first = s.first_index; v.vertex_offset = int32_t(uint32_t(s.base_vertex) + s.k); v.first_instance = uint32_t(s.base_instance); }
            else { v.instances = 1; v.first = uint32_t(s.first) + s.k; v.first_instance = 0; }
            p.push_back(e);
        }
        for (auto &e : p) e.v.off = e.offs;
        return p;
    }
};

// ---------------- the translation thread's own recording: the command section of nrdraw::lane_tail over the real bind cache ----------------
struct BindCache { VkPipeline pipe = VK_NULL_HANDLE; VkPipelineLayout layout = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t noff = UINT32_MAX, off[32]{}; bool has_vp = false, has_sc = false; VkViewport vp{}; VkRect2D sc{}; VkBuffer vb[16]{}; VkDeviceSize vbo[16]{};
    VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize ibo = 0; VkIndexType it = VK_INDEX_TYPE_MAX_ENUM; };
static void ref_tail(VkCommandBuffer cmd, BindCache &bc, const nrz5_vals &v) {
    struct { VkPipeline pipe; uint16_t used; VkBuffer vb[16]; uint64_t vbo[16]; VkBuffer ib; uint64_t ibo; VkIndexType it; VkViewport vp; VkRect2D sc; } H{};
    H.pipe = hnd<VkPipeline>(v.pipe); H.used = v.used; for (int i = 0; i < 16; i++) { H.vb[i] = hnd<VkBuffer>(v.vb[i]); H.vbo[i] = v.vbo[i]; }
    H.ib = hnd<VkBuffer>(v.ib); H.ibo = v.ibo; H.it = VkIndexType(v.it); memcpy(&H.vp, v.vp, sizeof H.vp); memcpy(&H.sc, v.sc, sizeof H.sc);
    struct { VkPipelineLayout layout; VkDescriptorSet set; uint32_t no; const uint32_t *off; } ds{hnd<VkPipelineLayout>(v.layout), hnd<VkDescriptorSet>(v.set), v.noff, v.off};
    struct { VkCommandBuffer command; } c{cmd};
    // (the lines of lane_tail, without its counters)
    if(bc.pipe!=H.pipe){nbmt::CmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,H.pipe);bc.pipe=H.pipe;}
    {const uint32_t no=ds.no;
     if(bc.layout!=ds.layout || bc.set!=ds.set || bc.noff!=no || no>32 || (no && memcmp(bc.off,ds.off,no*4))){
        nbmt::CmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,ds.layout,0,1,&ds.set,no,ds.off);
        if(no<=32){bc.layout=ds.layout;bc.set=ds.set;bc.noff=no;if(no)memcpy(bc.off,ds.off,no*4);}else bc.noff=UINT32_MAX;}}
    if(!bc.has_vp || memcmp(&bc.vp,&H.vp,sizeof H.vp)){nbmt::CmdSetViewport(c.command,0,1,&H.vp);bc.vp=H.vp;bc.has_vp=true;}
    if(!bc.has_sc || memcmp(&bc.sc,&H.sc,sizeof H.sc)){nbmt::CmdSetScissor(c.command,0,1,&H.sc);bc.sc=H.sc;bc.has_sc=true;}
    {unsigned changed=0;for(uint32_t m=H.used;m;m&=m-1){const uint32_t i=uint32_t(__builtin_ctz(m));if(bc.vb[i]!=H.vb[i] || bc.vbo[i]!=H.vbo[i])changed|=1u<<i;}
     if(changed){uint8_t f[8],k[8];const unsigned runs=nr_vb_runs(changed,H.used,f,k);
        for(unsigned r=0;r<runs;r++){VkDeviceSize o[16];for(unsigned j=0;j<k[r];j++){o[j]=H.vbo[f[r]+j];bc.vb[f[r]+j]=H.vb[f[r]+j];bc.vbo[f[r]+j]=H.vbo[f[r]+j];}
            nbmt::CmdBindVertexBuffers(c.command,f[r],k[r],&H.vb[f[r]],o);}}}
    if(v.indexed){
        if(bc.ib!=H.ib || bc.ibo!=H.ibo || bc.it!=H.it){nbmt::CmdBindIndexBuffer(c.command,H.ib,H.ibo,H.it);bc.ib=H.ib;bc.ibo=H.ibo;bc.it=H.it;}
        nbmt::CmdDrawIndexed(c.command,v.count,v.instances,v.first,v.vertex_offset,v.first_instance);}
    else nbmt::CmdDraw(c.command,v.count,v.instances,v.first,v.first_instance);
}
// What the tail leaves in the bind cache after a draw with these values - without the commands (a VALID draw).
static void leave(BindCache &bc, const nrz5_vals &v) {
    bc.pipe = hnd<VkPipeline>(v.pipe);
    if (v.noff <= 32) { bc.layout = hnd<VkPipelineLayout>(v.layout); bc.set = hnd<VkDescriptorSet>(v.set); bc.noff = v.noff; if (v.noff) memcpy(bc.off, v.off, size_t(v.noff) * 4); } else bc.noff = UINT32_MAX;
    memcpy(&bc.vp, v.vp, sizeof bc.vp); bc.has_vp = true; memcpy(&bc.sc, v.sc, sizeof bc.sc); bc.has_sc = true;
    for (uint32_t m = v.used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); bc.vb[i] = hnd<VkBuffer>(v.vb[i]); bc.vbo[i] = v.vbo[i]; }
    if (v.indexed) { bc.ib = hnd<VkBuffer>(v.ib); bc.ibo = v.ibo; bc.it = VkIndexType(v.it); }
}
static void foreign_bind(VkCommandBuffer c, BindCache &bc) { const VkPipeline p = hnd<VkPipeline>(0x1999); nbmt::CmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p); bc.pipe = p; }
static void other(VkCommandBuffer c, BindCache &bc, const Ev &e) {
    if (e.kind == K_SILENT) { bc = BindCache{}; return; }   // the cache is reset and nothing is written
    if (e.kind == K_PASS) { nbmt::CmdEndRenderPass(c); VkRenderPassBeginInfo b{}; b.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; b.renderPass = hnd<VkRenderPass>(e.count & 7); b.renderArea.extent = {640, 360};
        nbmt::CmdBeginRenderPass(c, &b, VK_SUBPASS_CONTENTS_INLINE); bc = BindCache{}; }
    else if (e.count & 1) nbmt::CmdFillBuffer(c, hnd<VkBuffer>(0x77), e.count & 0xff0, 16, uint32_t(e.count));
    else { VkClearAttachment a{}; a.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; VkClearRect rc{}; rc.rect.extent = {uint32_t(e.count & 255), 4}; rc.layerCount = 1; nbmt::CmdClearAttachments(c, 1, &a, 1, &rc); }
}
static nrz5_cache conv(const BindCache &bc) {
    nrz5_cache c; nrz5_cache_unknown(&c); c.known = NRZ5_K_PIPE | NRZ5_K_SET | NRZ5_K_VP | NRZ5_K_SC | NRZ5_K_IB; c.known_vb = 0xffff;
    c.pipe = uint64_t(uintptr_t(bc.pipe)); c.layout = uint64_t(uintptr_t(bc.layout)); c.set = uint64_t(uintptr_t(bc.set)); c.noff = bc.noff; memcpy(c.off, bc.off, sizeof c.off);
    c.has_vp = bc.has_vp; c.has_sc = bc.has_sc; memcpy(c.vp, &bc.vp, sizeof bc.vp); memcpy(c.sc, &bc.sc, sizeof bc.sc);
    for (int i = 0; i < 16; i++) { c.vb[i] = uint64_t(uintptr_t(bc.vb[i])); c.vbo[i] = bc.vbo[i]; }
    c.ib = uint64_t(uintptr_t(bc.ib)); c.ibo = bc.ibo; c.it = uint32_t(bc.it); return c;
}
static bool same_vals(const nrz5_vals &a, const nrz5_vals &b) {
    if (a.pipe != b.pipe || a.layout != b.layout || a.set != b.set || a.noff != b.noff || (a.noff && memcmp(a.off, b.off, size_t(a.noff) * 4))) return false;
    if (memcmp(a.vp, b.vp, sizeof a.vp) || memcmp(a.sc, b.sc, sizeof a.sc) || a.used != b.used) return false;
    for (uint32_t i = 0; i < 16; i++) if (a.used >> i & 1) if (a.vb[i] != b.vb[i] || a.vbo[i] != b.vbo[i]) return false;
    if (a.indexed != b.indexed || a.count != b.count || a.instances != b.instances || a.first != b.first || a.first_instance != b.first_instance) return false;
    if (a.indexed && (a.ib != b.ib || a.ibo != b.ibo || a.it != b.it || a.vertex_offset != b.vertex_offset)) return false;
    return true;
}

struct Cfg { int mode; unsigned flush, chunk_kb, queue; int slow; };
static nbmtq::WorkerStats g_ws; static nbmtq::Stats g_st;
static void setup(const Cfg &cfg) {
    char e[4][64]; snprintf(e[0], 64, "%d", cfg.mode); snprintf(e[1], 64, "%u", cfg.flush); snprintf(e[2], 64, "%u", cfg.chunk_kb); snprintf(e[3], 64, "%u", cfg.queue);
#if defined(_WIN32)
    _putenv_s("DM_NB_MT", e[0]); _putenv_s("DM_NB_MT_FLUSH", e[1]); _putenv_s("DM_NB_MT_CHUNK_KB", e[2]); _putenv_s("DM_NB_MT_QUEUE", e[3]);
#else
    setenv("DM_NB_MT", e[0], 1); setenv("DM_NB_MT_FLUSH", e[1], 1); setenv("DM_NB_MT_CHUNK_KB", e[2], 1); setenv("DM_NB_MT_QUEUE", e[3], 1);
#endif
    nbmt::init(); g_slow.store(cfg.slow); g_ws = nbmtq::WorkerStats{}; g_st = nbmtq::Stats{};
    { const uint32_t n = (32u << 20) / nbmt::q.chunk_bytes; nbmt::q.spare_max = n < 8 ? 8 : n > 1024 ? 1024 : n; }
}
static void finish() {
    g_st = nbmt::q.st; { std::lock_guard<std::mutex> l(nbmt::q.mu); g_ws = nbmt::q.ws; }
    nbmt::stop(); g_slow.store(0); nbmt::q.st = nbmtq::Stats{}; { std::lock_guard<std::mutex> l(nbmt::q.mu); nbmt::q.ws = nbmtq::WorkerStats{}; CHECK(!nbmt::q.running && !nbmt::q.free_list && !nbmt::q.head, "stop: worker gone, chunk memory returned"); }
}
static std::vector<uint8_t> today(const std::vector<Ev> &p) {
    Log lg; VkCommandBuffer c = reinterpret_cast<VkCommandBuffer>(&lg); nbmt::report_ns = nbmtq::now_ns(); nbmt::begin(c, true, false);
    BindCache bc;
    for (const Ev &e : p) { if (e.kind) { other(c, bc, e); continue; } if (e.dropped) continue; ref_tail(c, bc, e.v); if (e.foreign) foreign_bind(c, bc); }
    nbmt::end(c); return std::move(lg.b);
}

// ---------------- the game thread ----------------
struct Rec { uint32_t mark = 0, slot = 0; bool pred = false; nrz5_vals pv; uint32_t poff[NRZ5T_MAX_DYN]; };
struct Recording { size_t s; std::vector<Rec> recs; };
struct Handoff { std::mutex mu; std::condition_variable cv; std::deque<Recording> q; bool done = false; };
static nrz5r::Ring g_ring;
static nrz5p::Table g_tab;
struct GStats { uint64_t draws = 0, predicted = 0; nrz5r::WStats w; };
// One engine draw record on the game thread: its slot, the prediction, the packets. occ: this frame's count of records per site hash.
static void game_draw(nrz5r::Writer &W, const Ev &e, Rec &rc, uint8_t *occ, uint32_t limit, GStats &gs) {
    const uint32_t oi = uint32_t(nrz5_site_hash(&e.site, 0) >> 40) & 4095u, o = occ[oi]; if (occ[oi] < 255) ++occ[oi];
    rc.slot = g_tab.slot_of(&e.site, o) + 1; ++gs.draws;
    nrz5_tmpl t; uint32_t ver = 0;
    rc.pred = g_tab.get(rc.slot - 1, &t, &ver) && nrz5p_vals(&t, &e.site, e.count, e.instances, e.first, e.base_vertex, e.base_instance, [&](unsigned st, unsigned sl) { return st < 2 && sl < 8 ? e.ub[st][sl] : uint64_t(0); }, &rc.pv, rc.poff);
    if (!rc.pred) { W.no_values(); rc.mark = 0; return; }
    ++gs.predicted;
    auto emit = [&](nbmtq::Chunk *ch, nrz5_cache *m, const nrz5_vals *v) { const uint32_t cap0 = ch->cap; if (limit < cap0) ch->cap = limit; nbmt::Build b{ch}; nbmt::Z5Sink sink{b}; const unsigned n = nrz5_emit(sink, m, v); ch->cap = cap0; return n; };
    rc.mark = W.draw(rc.pv, emit);
}
static GStats game_thread(const std::vector<std::vector<Ev>> &prog, Handoff &h, uint32_t seed) {
    nrz5r::Writer W; W.q = &nbmt::q; W.ring = &g_ring; Rng r{seed + 5}; W.min_tails = 1 + seed % 3; GStats gs;
    for (size_t s = 0; s < prog.size(); s++) {
        Recording rec; rec.s = s; rec.recs.resize(prog[s].size()); uint8_t occ[4096] = {}; uint32_t limit = ~0u;
        for (size_t i = 0; i < prog[s].size(); i++) {
            const Ev &e = prog[s][i];
            if (e.kind == K_SILENT) continue;                       // (the translation thread's own business)
            if (e.kind) { if (e.kind == K_PASS || r.upto(2)) W.end_run(); continue; }   // (half of the other commands the game thread does not count on: the hook of the wrappers cuts there)
            if (!W.cur.chunk) limit = r.upto(6) == 0 ? 400 + r.upto(700) : ~0u;   // sometimes a chunk with little room: the run ends where it is full
            game_draw(W, e, rec.recs[i], occ, limit, gs);
        }
        W.end_run();
        std::unique_lock<std::mutex> l(h.mu); h.cv.wait(l, [&] { return h.q.size() < 3; }); h.q.push_back(std::move(rec)); h.cv.notify_all();
    }
    { std::lock_guard<std::mutex> l(h.mu); h.done = true; } h.cv.notify_all();
    gs.w = W.st; return gs;
}

// ---------------- the translation thread ----------------
static nrz5r::Reader *g_R = nullptr;
static void pre_hook() { if (NRZ5R_TEST_DROP != 2 && g_R && g_R->holding) g_R->flush(nrz5r::CUT_WRITE); }
static uint64_t sig() { const nbmtq::Stats &q = nbmt::q.st; return q.cmds * 8 + q.sync[nbmtq::NBMT_SY_PNEXT] + q.sync[nbmtq::NBMT_SY_ALLOC] + q.sync[nbmtq::NBMT_SY_CALLER]; }
static bool is_draw(uint16_t op) { return op == nbmt::OP_Draw || op == nbmt::OP_DrawIndexed; }
static void publish(const Ev &e, uint32_t slot) {
    if (!slot) return; nrz5_tmpl t;
    nrz5_tmpl_make(&t, &e.site, &e.v, e.first_index, e.k, [&](uint32_t i, unsigned *st, unsigned *sl, uint64_t *a) { *st = e.st[i]; *sl = e.sl[i]; *a = e.ub[e.st[i]][e.sl[i]]; return true; });
    g_tab.put(slot - 1, &t);
}
struct Tot { uint64_t draws = 0, recorded = 0, valid = 0, abandoned = 0, direct = 0; nrz5r::RStats r; GStats g; } tot;
// One recording on the translation thread. verify: nothing is taken. outcome (optional): 'V' / 'R' / 'D' per event (draws only).
static void execute(nrz5r::Reader &R, const std::vector<Ev> &p, const std::vector<Rec> &recs, VkCommandBuffer c, bool verify, std::string *outcome = nullptr) {
    BindCache bc; bool moved = false; uint64_t sig_valid = 0; bool was_valid = false;
    g_R = verify ? nullptr : &R;
    for (size_t i = 0; i < p.size(); i++) {
        const Ev &e = p[i]; const Rec &rc = recs[i];
        if (e.kind) { other(c, bc, e); if (e.kind == K_SILENT) moved = true; continue; }
        ++tot.draws;
        if (nbmt::cur != c) { R.abandon(rc.mark); if (!e.dropped) { ref_tail(c, bc, e.v); if (e.foreign) foreign_bind(c, bc); ++tot.recorded; } moved = false; continue; }   // this recording goes by direct calls: nothing written ahead is used
        if (e.dropped) {   // the prologue of draw() dropped it: its record never reaches the command section
            if (R.holding) { if (verify) R.verify_miss(); else if (NRZ5R_TEST_DROP != 3) R.flush(nrz5r::CUT_RECORD); } else R.abandon(rc.mark);
            if (outcome) outcome->push_back('D'); moved = false; was_valid = false; continue; }
        auto holds = [&](const nrz5_cache &sh, const nrz5_vals &v) { const nrz5_cache real = conv(bc); return nrz5r::cache_holds(&sh, &real, &v); };
        if (verify) {
            const bool would = R.verify_begin(rc.mark, e.v, holds, is_draw, sig());
            if (would) { CHECK(rc.pred && same_vals(rc.pv, e.v) && !moved, "verify: a draw that would be taken was written from the resolved values and the cache did not move"); ++tot.valid; }
            nbmtq::Chunk *c0 = nbmt::q.cur; const uint32_t u0 = c0 ? c0->used : 0; const uint64_t ch0 = nbmt::q.st.chunks;
            ref_tail(c, bc, e.v); ++tot.recorded;
            nbmtq::Chunk *cc = c0 ? c0 : nbmt::q.cur; const uint32_t from = c0 ? u0 : 0; const bool readable = cc && cc == nbmt::q.cur && ch0 == nbmt::q.st.chunks;
            R.verify_end(readable ? cc->data() + from : nullptr, readable ? cc->used - from : 0, [](const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb) { uint32_t at = 0; return nbmt::same_stream(a, na, b, nb, &at); }, sig());
            if (!would) publish(e, rc.slot);
            if (e.foreign) foreign_bind(c, bc);
            if (outcome) outcome->push_back(would ? 'V' : 'R'); moved = false; continue;
        }
        const bool held = R.holding; const uint64_t s0 = sig();
        const int a = R.draw(rc.mark, e.v, holds, is_draw);
        if (a == nrz5r::Reader::VALID) {
            CHECK(rc.pred && same_vals(rc.pv, e.v) && !moved, "a taken draw was written from the resolved values and the cache did not move");
            if (held && was_valid) CHECK(s0 == sig_valid, "nothing was written between two taken draws of a run");
            if (NRZ5R_TEST_DROP != 1) leave(bc, e.v);
            ++tot.valid; was_valid = true; sig_valid = sig();
        } else { ref_tail(c, bc, e.v); publish(e, rc.slot); ++tot.recorded; was_valid = false; }
        if (was_valid && !R.holding) was_valid = false;   // (the run was complete: its chunk went into the stream)
        if (e.foreign) { foreign_bind(c, bc); was_valid = false; }
        if (outcome) outcome->push_back(a == nrz5r::Reader::VALID ? 'V' : 'R'); moved = false;
    }
}
static std::vector<std::vector<uint8_t>> with_game_thread(const std::vector<std::vector<Ev>> &prog, uint64_t seed, bool verify, std::vector<int> &executed) {
    std::vector<std::vector<uint8_t>> logs(prog.size()); executed.assign(prog.size(), 0);
    Handoff h; GStats gs; tot = Tot{};
    CHECK(g_tab.init(4096), "table"); g_pre = pre_hook;
    std::thread gt([&] { gs = game_thread(prog, h, uint32_t(seed)); });
    nrz5r::Reader R; R.q = &nbmt::q; R.ring = &g_ring; Rng r{seed * 31 + 3};
    for (;;) {
        Recording rec;
        { std::unique_lock<std::mutex> l(h.mu); h.cv.wait(l, [&] { return !h.q.empty() || h.done; }); if (h.q.empty()) break; rec = std::move(h.q.front()); h.q.pop_front(); h.cv.notify_all(); }
        const std::vector<Ev> &p = prog[rec.s];
        const uint32_t fate = verify ? 9 : r.upto(16);
        if (fate == 0) { for (const Rec &x : rec.recs) R.abandon(x.mark); ++tot.abandoned; continue; }   // never executed; the reader is told
        if (fate == 1) { ++tot.abandoned; continue; }                   // never executed and nobody tells the reader: the ids say so at the next run
        if (fate == 3 && !r.upto(4)) R.drop();                          // everything written ahead so far is thrown away
        const bool direct = fate == 2; if (direct) ++tot.direct;        // recorded by direct calls
        executed[rec.s] = 1;
        Log lg; VkCommandBuffer c = reinterpret_cast<VkCommandBuffer>(&lg); nbmt::report_ns = nbmtq::now_ns(); nbmt::begin(c, !direct, false);
        execute(R, p, rec.recs, c, verify);
        nbmt::end(c); logs[rec.s] = std::move(lg.b);
        CHECK(!R.holding, "a run does not outlive its command buffer");
    }
    gt.join(); R.drop(); g_R = nullptr; g_pre = nullptr;
    tot.r = R.st; tot.g = gs;
    CHECK(nbmt::q.spare_out.load() == 0, "every spare chunk came back (%u out)", nbmt::q.spare_out.load());
    CHECK(g_ring.front() == nullptr, "the ring is empty");
    g_tab.destroy();
    return logs;
}

// ---------------- scripts: one thread, exact outcomes ----------------
// n draws of one site (pipes[i]: the pipeline resolved for draw i), everything predicted right except where `wrong` says; the
// translation thread's events: silent_before / other_before / dropped (draw indices, -1 none). Returns the outcomes, checks the log.
static std::string script(const char *name, const std::vector<uint64_t> &pipes, int wrong, int silent_before, int other_before, int dropped, int alien = -1) {
    World w(42); std::vector<Ev> base = w.frame(); Ev d{}; for (const Ev &e : base) if (!e.kind && e.v.noff && e.v.noff <= NRZ5T_MAX_DYN) { d = e; break; }
    d.foreign = d.dropped = false;
    std::vector<Ev> p; for (size_t i = 0; i < pipes.size(); i++) {
        if (int(i) == silent_before) { Ev s{}; s.kind = K_SILENT; p.push_back(s); }
        if (int(i) == other_before) { Ev s{}; s.kind = K_OTHER; s.count = 7; p.push_back(s); }
        Ev e = d; e.v.pipe = pipes[i]; e.dropped = int(i) == dropped; p.push_back(e); }
    for (auto &e : p) e.v.off = e.offs;
    setup(Cfg{0, 256, 64, 64, 0}); const std::vector<uint8_t> ref = today(p); finish();
    setup(Cfg{1, 256, 64, 64, 0}); g_pre = pre_hook; tot = Tot{};
    nrz5r::Writer W; W.q = &nbmt::q; W.ring = &g_ring; nrz5r::Reader R; R.q = &nbmt::q; R.ring = &g_ring;
    // the game thread: the values are the resolved ones (a perfect prediction) but for draw `wrong`
    std::vector<Rec> recs(p.size()); { int di = 0; for (size_t i = 0; i < p.size(); i++) { if (p[i].kind) continue; Rec &rc = recs[i]; rc.pred = true; rc.pv = p[i].v; memcpy(rc.poff, p[i].offs, sizeof rc.poff); rc.pv.off = rc.poff;
        if (di == wrong) rc.pv.set ^= 0x100;
        auto emit = [&](nbmtq::Chunk *ch, nrz5_cache *m, const nrz5_vals *v) { nbmt::Build b{ch}; nbmt::Z5Sink sink{b}; return nrz5_emit(sink, m, v); };
        rc.mark = W.draw(rc.pv, emit);
        if (di == alien) rc.mark = nrz5r::mark_word(nrz5r::MARK_COVERED, nrz5r::id_of(rc.mark) + 7);   // this record claims to be covered by ANOTHER run
        ++di; } W.end_run(); }
    Log lg; VkCommandBuffer c = reinterpret_cast<VkCommandBuffer>(&lg); nbmt::report_ns = nbmtq::now_ns(); nbmt::begin(c, true, false);
    std::string out; execute(R, p, recs, c, false, &out); nbmt::end(c);
    CHECK(lg.b == ref, "script %s: the calls are today's (%zu / %zu bytes)", name, lg.b.size(), ref.size());
    R.drop(); g_R = nullptr; g_pre = nullptr; CHECK(nbmt::q.spare_out.load() == 0 && !g_ring.front(), "script %s: chunks back", name);
    finish(); return out;
}

int main(int argc, char **argv) {
    const unsigned scale = argc > 1 ? unsigned(atoi(argv[1])) : 1;
    const unsigned frames = 60 * (scale ? scale : 1);
    // ---- scripts ----
    { const uint64_t A = 0x1001, B = 0x1002;
      std::string o;
      o = script("all valid", {A, A, A, A, A, A}, -1, -1, -1, -1); CHECK(o == "RVVVVV", "all valid: %s", o.c_str()); CHECK(tot.valid == 5, "all valid: 5 taken");
      o = script("alternating pipelines", {A, B, A, B, A, B}, -1, -1, -1, -1); CHECK(o == "RVVVVV", "alternating pipelines (the shadow follows the taken draws): %s", o.c_str());
      o = script("a wrong value", {A, A, A, A, A, A}, 3, -1, -1, -1); CHECK(o == "RVVRRR", "a wrong value cuts the run at its draw: %s", o.c_str());
      o = script("a wrong value at the first tail", {A, A, A, A}, 1, -1, -1, -1); CHECK(o == "RRRR", "a wrong first tail draw: nothing taken: %s", o.c_str());
      o = script("a silent cache reset", {A, A, A, A, A, A}, -1, 3, -1, -1); CHECK(o == "RVVRRR", "a cache reset without a command cuts the run: %s", o.c_str());
      o = script("a command in between", {A, A, A, A, A, A}, -1, -1, 4, -1); CHECK(o == "RVVVRR", "a command the game thread did not see cuts the run: %s", o.c_str());
      // (mutation 9 - a covered record of another run taken as the next draw of the run in hand - was caught by nothing: in the two-thread
      // runs a record of another run never meets a held run. Asked here directly.)
      o = script("a record of another run", {A, A, A, A, A, A}, -1, -1, -1, -1, 3); CHECK(o == "RVVRRR", "a covered record of another run ends the run in hand and is recorded: %s", o.c_str());
      o = script("a dropped draw", {A, A, A, A, A, A}, -1, -1, -1, 2); CHECK(o == "RVDRRR", "a dropped draw cuts the run before it: %s", o.c_str()); }
    // ---- two threads ----
    uint64_t packets = 0, spliced = 0, vcmp = 0;
    for (uint64_t seed = 1; seed <= 3; seed++) {
        World w(seed); std::vector<std::vector<Ev>> prog; for (unsigned s = 0; s < frames; s++) { prog.push_back(w.frame()); for (auto &e : prog.back()) e.v.off = e.offs; }
        for (auto &f : prog) for (auto &e : f) e.v.off = e.offs;
        std::vector<std::vector<uint8_t>> ref;
        setup(Cfg{0, 256, 64, 64, 0}); for (auto &p : prog) ref.push_back(today(p)); finish();
        const Cfg shapes[] = {{1, 256, 64, 64, 0}, {2, 256, 64, 64, 0}, {3, 256, 64, 64, 0}, {2, 16, 16, 2, 0}, {3, 16, 16, 2, 7}, {3, 65536, 1024, 1024, 0}};
        for (const Cfg &cf : shapes) {
            char what[96]; snprintf(what, sizeof what, "seed %llu mode %d flush %u chunk %u KB queue %u slow %d", (unsigned long long)seed, cf.mode, cf.flush, cf.chunk_kb, cf.queue, cf.slow);
            std::vector<int> ex;
            // APPLY
            setup(cf); { const auto logs = with_game_thread(prog, seed * 77 + cf.mode, false, ex);
                for (size_t s = 0; s < prog.size(); s++) if (ex[s]) CHECK(logs[s] == ref[s], "%s: with the game thread the calls are today's in today's order (recording %zu: %zu / %zu bytes)", what, s, logs[s].size(), ref[s].size()); }
            const Tot t = tot; finish();
            // (1 MB chunks: 32 spare ones - the game thread mostly finds none. The few runs it does write - 16 in a typical pass - are
            // predicted up to three recordings ahead of the translation thread, and whether any of them survives its first tail draw
            // depends on which thread was faster: "taken > 0" failed in 2 runs of 7 with every log and every chunk count right. The
            // shape checks the protocol under starvation - logs, chunk accounting, below -, not a share.)
            const bool starved = cf.chunk_kb > 64;
            CHECK(starved ? t.valid == t.r.valid && t.r.started + t.g.w.no_chunk + t.g.w.ring_full > 0 : t.valid > 1000 && t.r.complete > 0 && t.r.handed > 0 && t.r.cut[nrz5r::CUT_CACHE] > 0 && t.r.cut[nrz5r::CUT_VALUES] > 0 && t.r.cut[nrz5r::CUT_RECORD] > 0 && t.r.cut[nrz5r::CUT_WRITE] > 0 && t.r.first_no > 0,
                  "%s: taken %llu draws; runs complete %llu, handed over %llu, cut by the cache %llu / values %llu / a record %llu / a write %llu, nothing taken %llu", what, (unsigned long long)t.valid, (unsigned long long)t.r.complete,
                  (unsigned long long)t.r.handed, (unsigned long long)t.r.cut[0], (unsigned long long)t.r.cut[1], (unsigned long long)t.r.cut[3], (unsigned long long)t.r.cut[4], (unsigned long long)t.r.first_no);
            CHECK(t.r.valid == t.valid && g_st.spliced == t.r.handed && g_st.alloc_fail == 0 && t.r.cut[nrz5r::CUT_CHUNK] == 0, "%s: chunks handed over %llu = runs with a taken draw %llu", what, (unsigned long long)g_st.spliced, (unsigned long long)t.r.handed);
            CHECK(t.g.predicted > t.g.draws / 2 && t.g.w.no_values > 0 && t.g.w.heads > 0 && (starved || t.g.w.chunk_full > 0), "%s: the game thread had values for %llu of %llu records", what, (unsigned long long)t.g.predicted, (unsigned long long)t.g.draws);
            if (cf.mode != 1) CHECK(g_ws.cmds + g_st.help_cmds == g_st.cmds && g_ws.chunks + g_st.help_chunks == g_st.chunks, "%s: every packet replayed once", what);
            if (cf.mode == 2) CHECK(g_ws.checked == g_st.chunks && !g_ws.bad[0] && !g_ws.bad[1] && !g_ws.bad[2] && !g_ws.bad[3], "%s: the queue's check is clean (%llu chunks)", what, (unsigned long long)g_ws.checked);
            packets += g_st.cmds; spliced += g_st.spliced_cmds;
            // VERIFY
            setup(cf); { const auto logs = with_game_thread(prog, seed * 177 + cf.mode, true, ex);
                for (size_t s = 0; s < prog.size(); s++) CHECK(ex[s] && logs[s] == ref[s], "%s: verify records today's calls (recording %zu)", what, s); }
            const Tot v = tot; finish();
            CHECK(v.r.v_bad == 0 && v.r.v_same == v.r.v_cmp && v.r.v_cmp + v.r.v_skipped == v.valid && (starved ? v.r.v_cmp > 0 : v.r.v_cmp > 1000 && v.r.cut[nrz5r::CUT_WRITE] > 0 && v.r.cut[nrz5r::CUT_VALUES] > 0), "%s: verify - would take %llu, compared %llu, same %llu, bad %llu", what,
                  (unsigned long long)v.valid, (unsigned long long)v.r.v_cmp, (unsigned long long)v.r.v_same, (unsigned long long)v.r.v_bad);
            vcmp += v.r.v_cmp;
            printf("     values differ at the cut: pipeline %llu, set %llu, offsets %llu, viewport %llu, scissor %llu, vertex %llu, index %llu, arguments %llu\n", (unsigned long long)t.r.diff[1], (unsigned long long)t.r.diff[2],
                   (unsigned long long)t.r.diff[3], (unsigned long long)t.r.diff[4], (unsigned long long)t.r.diff[5], (unsigned long long)t.r.diff[6], (unsigned long long)t.r.diff[7], (unsigned long long)t.r.diff[8]);
            printf("ok   %-56s records %6llu: values for %6llu, heads %5llu tails %6llu in %5llu runs (no chunk %4llu, full %3llu, short %4llu) | taken %6llu (%4.1f%%), runs complete %4llu, cut: cache %4llu values %4llu record %4llu write %4llu, first draw no %4llu, not reached %4llu, abandoned %2llu direct %2llu | verify: %6llu compared\n",
                   what, (unsigned long long)t.g.draws, (unsigned long long)t.g.predicted, (unsigned long long)t.g.w.heads, (unsigned long long)t.g.w.tails, (unsigned long long)t.g.w.runs, (unsigned long long)t.g.w.no_chunk, (unsigned long long)t.g.w.chunk_full,
                   (unsigned long long)t.g.w.short_runs, (unsigned long long)t.valid, t.draws ? 100. * double(t.valid) / double(t.draws) : 0., (unsigned long long)t.r.complete, (unsigned long long)t.r.cut[0], (unsigned long long)t.r.cut[1], (unsigned long long)t.r.cut[3],
                   (unsigned long long)t.r.cut[4], (unsigned long long)t.r.first_no, (unsigned long long)t.r.unreached, (unsigned long long)t.abandoned, (unsigned long long)t.direct, (unsigned long long)v.r.v_cmp);
        }
    }
    // the values record by itself: every field
    { World w(7); std::vector<Ev> f = w.frame(); for (auto &e : f) e.v.off = e.offs; unsigned n = 0;
      for (const Ev &e : f) { if (e.kind) continue; uint8_t buf[1024]; const uint32_t b = nrz5r::pv_put(buf, sizeof buf, &e.v); CHECK(b && b == nrz5r::pv_bytes(&e.v) && !(b & 7) && nrz5r::pv_diff(buf, b, &e.v) == 0, "a values record equals its draw");
          CHECK(nrz5r::pv_put(buf, b - 1, &e.v) == 0 && nrz5r::pv_diff(buf, b - 8, &e.v) == 9, "no room / a cut record");
          uint32_t offs[40]; memcpy(offs, e.offs, sizeof offs); nrz5_vals x = e.v; x.off = offs;
          x = e.v; x.off = offs; x.pipe ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 1, "pipeline"); x = e.v; x.off = offs; x.set ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 2, "set"); x = e.v; x.off = offs; x.layout ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 2, "layout");
          x = e.v; x.off = offs; if (e.v.noff) { offs[e.v.noff - 1] ^= 256; CHECK(nrz5r::pv_diff(buf, b, &x) == 3, "an offset"); offs[e.v.noff - 1] ^= 256; }
          x = e.v; x.off = offs; x.vp[23] ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 4, "viewport"); x = e.v; x.off = offs; x.sc[15] ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 5, "scissor");
          { const uint32_t i = 31u - uint32_t(__builtin_clz(uint32_t(e.v.used))); x = e.v; x.off = offs; x.vb[i] ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 6, "a vertex buffer"); x = e.v; x.off = offs; x.vbo[i] ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 6, "a vertex offset"); }
          if (e.v.indexed) { x = e.v; x.off = offs; x.ibo ^= 2; CHECK(nrz5r::pv_diff(buf, b, &x) == 7, "index offset"); x = e.v; x.off = offs; x.it ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 7, "index type"); x = e.v; x.off = offs; x.vertex_offset ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 8, "base vertex"); }
          x = e.v; x.off = offs; x.count ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 8, "count"); x = e.v; x.off = offs; x.first ^= 1; CHECK(nrz5r::pv_diff(buf, b, &x) == 8, "first"); x = e.v; x.off = offs; x.instances ^= 2; CHECK(nrz5r::pv_diff(buf, b, &x) == 8, "instances");
          ++n; }
      CHECK(n > 100, "values records checked: %u", n);
      CHECK(nrz5r::mark_of(nrz5r::mark_word(nrz5r::MARK_COVERED, 77)) == nrz5r::MARK_COVERED && nrz5r::id_of(nrz5r::mark_word(nrz5r::MARK_SPLICE, nrz5r::ID_MAX)) == uint32_t(nrz5r::ID_MAX) && nrz5r::mark_word(nrz5r::MARK_NONE, 0) == 0, "mark word"); }
    printf("nr_z5r_test: %ld checks, %ld failed, %llu packets (%llu of them in chunks written by the game thread), %llu taken draws compared by verify -> %s\n", checks, fails, (unsigned long long)packets,
           (unsigned long long)spliced, (unsigned long long)vcmp, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
