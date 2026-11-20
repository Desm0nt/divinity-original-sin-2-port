// [DOS2] NR-ZERO Z5 host test of the second writer (dm/gfx/nr/nr_z5w.h, nbmtq::Queue::take_spare / splice_spare of dm/gfx/nb/nb_mt.h;
// docs/dos2/nr/NR-ZERO.md section 18): two real threads - a WRITER (the game thread's place: it walks the recordings ahead, marks the
// draw records and writes the tails of the runs into spare chunks of the queue) and the TRANSLATION thread (it executes the recordings,
// takes or declines every run). Against the mock Vulkan of nb_mt_test.cpp (a "command buffer" is a byte log of the calls made on it).
//   TODAY    every recording recorded by the translation thread alone, direct calls (mode 0): the reference logs.
//   RUNS     the writer runs ahead (up to three recordings); the translation thread records head and unmarked draws itself, splices
//            a run's chunk at its MARK_SPLICE record when the bind cache holds what the run knew and its own verdict is yes, skips
//            the covered records, copies the run's cache words back; a declined run is recorded draw by draw. Things the writer cannot
//            see happen on the translation thread: a draw that leaves another pipeline bound ("foreign": the old tail finished it),
//            verdicts that say no (1 in 8), recordings that are never executed (with and without Reader::abandon), everything written ahead thrown away (Reader::drop), a recording recorded
//            by direct calls. The logs must be byte-identical to TODAY's in modes 1 / 2 / 3 and several queue shapes; mode 2 of the
//            queue: no finding; every spare chunk comes back; every packet replayed once.
//   VERIFY   (the shape of DM_NR_Z5=2 with a second writer) the translation thread records everything itself and compares its own
//            packets of every tail draw with the writer's chunk: same packets, the chunk ends where the tail ends; the logs are TODAY's.
// The mutation check: built with -DNRZ5W_MUTATE=k (k = 1..6, nr_z5w.h), -DNRZ5W_TEST_DROP=k (1 the run's cache words are not copied
// back after a chunk, 2 the writer does not end its run at a record that is not an engine draw) this test must FAIL. (nr_z5.h's own
// mutations, -DNRZ5_MUTATE=k, are nr_z5_test.cpp's; 10 of the 11 fail here too.)
// Build (Vulkan headers: any vulkan/ + vk_video/ include tree, e.g. the NDK sysroot's usr/include), from the repository root:
//   clang++ -std=c++17 -O2 -Wall -Wextra -I<vk> dm/gfx/vulkan/engine/tests/nr_z5w_test.cpp -o nr_z5w_test && ./nr_z5w_test
//   g++ -std=c++17 -O1 -g -fsanitize=thread -pthread -I<vk> dm/gfx/vulkan/engine/tests/nr_z5w_test.cpp -o nr_z5w_tsan && ./nr_z5w_tsan
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
#ifndef NRZ5W_TEST_DROP
#define NRZ5W_TEST_DROP 0
#endif

static std::vector<std::string> g_lines;
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void xbg_logf(const char *fmt, ...) { char t[2048]; va_list a; va_start(a, fmt); vsnprintf(t, sizeof t, fmt, a); va_end(a); g_lines.push_back(t); }
#include "nb_mt_mock.h"
#define NBMT_REAL(f) mock_##f
#include "../xbg_nb_mt.inc"
#include "../../../nr/nr_z5w.h"

static long fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
struct Rng { uint64_t s; uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); } uint32_t upto(uint32_t n) { return n ? next() % n : 0; } };
template <class H> static H hnd(uint64_t x) { return H(uintptr_t(x)); }

// ---------------- the program (as nr_z5_test.cpp) ----------------
struct Ev { int kind; bool foreign; nrz5_vals v; uint32_t offs[40]; };   // kind 0: an engine draw (foreign: the translation thread leaves another pipeline bound after it); 1: another command; 2: one that starts a pass
static void make_draw(Rng &r, Ev &e) {
    static const uint16_t useds[6] = {0x1, 0x3, 0x7, 0x5, 0x13, 0x8003};
    static const uint32_t noffs[6] = {0, 2, 5, 5, 8, 33};
    nrz5_vals &v = e.v; memset(&v, 0, sizeof v);
    v.pipe = 0x1000 + r.upto(5); v.layout = 0x2000 + r.upto(2); v.set = 0x3000 + r.upto(3);
    v.noff = noffs[r.upto(r.upto(12) ? 5 : 6)]; for (uint32_t i = 0; i < v.noff; i++) e.offs[i] = 256 * r.upto(i < 2 ? 3 : 1 + r.upto(2)); v.off = e.offs;
    { VkViewport vp{0, 0, float(640 << r.upto(2)), 360, 0, 1}; memcpy(v.vp, &vp, sizeof vp); VkRect2D sc{{0, 0}, {uint32_t(640 << r.upto(2)), 360}}; memcpy(v.sc, &sc, sizeof sc); }
    v.used = useds[r.upto(r.upto(6) ? 3 : 6)];
    for (uint32_t i = 0; i < 16; i++) if (v.used >> i & 1) { v.vb[i] = 0x4000 + r.upto(3); v.vbo[i] = 64 * r.upto(2); }
    v.indexed = r.upto(8) != 0; v.ib = 0x5000 + r.upto(2); v.ibo = 2 * r.upto(2); v.it = r.upto(3) ? uint32_t(VK_INDEX_TYPE_UINT16) : uint32_t(VK_INDEX_TYPE_UINT32);
    v.count = 3 + r.upto(3000); v.instances = 1 + r.upto(2); v.first = r.upto(5000); v.first_instance = r.upto(2); v.vertex_offset = int32_t(r.upto(4000)) - 100;
    e.foreign = r.upto(40) == 0;
}
static std::vector<Ev> program(uint64_t seed, unsigned n) {
    Rng r{seed * 0x9E3779B97F4A7C15ull + 7}; std::vector<Ev> p(n);
    for (auto &e : p) { const uint32_t k = r.upto(100); e.kind = k < 94 ? 0 : k < 98 ? 1 : 2; e.foreign = false; if (!e.kind) make_draw(r, e); else { memset(&e.v, 0, sizeof e.v); e.v.count = r.next(); } }
    for (auto &e : p) e.v.off = e.offs;
    return p;
}
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
// A draw the translation thread records itself. foreign: what the writer cannot see - after this draw another pipeline is bound.
static void record(VkCommandBuffer c, BindCache &bc, const Ev &e) {
    ref_tail(c, bc, e.v);
    if (e.foreign) { const VkPipeline p = hnd<VkPipeline>(0x1999); nbmt::CmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p); bc.pipe = p; }
}
static void other(VkCommandBuffer c, BindCache &bc, const Ev &e) {
    if (e.kind == 2) { nbmt::CmdEndRenderPass(c); VkRenderPassBeginInfo b{}; b.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; b.renderPass = hnd<VkRenderPass>(e.v.count & 7); b.renderArea.extent = {640, 360};
        nbmt::CmdBeginRenderPass(c, &b, VK_SUBPASS_CONTENTS_INLINE); bc = BindCache{}; }
    else if (e.v.count & 1) nbmt::CmdFillBuffer(c, hnd<VkBuffer>(0x77), e.v.count & 0xff0, 16, e.v.count);
    else { VkClearAttachment a{}; a.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; VkClearRect rc{}; rc.rect.extent = {uint32_t(e.v.count & 255), 4}; rc.layerCount = 1; nbmt::CmdClearAttachments(c, 1, &a, 1, &rc); }
}
static nrz5_cache conv(const BindCache &bc) {
    nrz5_cache c; nrz5_cache_unknown(&c); c.known = NRZ5_K_PIPE | NRZ5_K_SET | NRZ5_K_VP | NRZ5_K_SC | NRZ5_K_IB; c.known_vb = 0xffff;
    c.pipe = uint64_t(uintptr_t(bc.pipe)); c.layout = uint64_t(uintptr_t(bc.layout)); c.set = uint64_t(uintptr_t(bc.set)); c.noff = bc.noff; memcpy(c.off, bc.off, sizeof c.off);
    c.has_vp = bc.has_vp; c.has_sc = bc.has_sc; memcpy(c.vp, &bc.vp, sizeof bc.vp); memcpy(c.sc, &bc.sc, sizeof bc.sc);
    for (int i = 0; i < 16; i++) { c.vb[i] = uint64_t(uintptr_t(bc.vb[i])); c.vbo[i] = bc.vbo[i]; }
    c.ib = uint64_t(uintptr_t(bc.ib)); c.ibo = bc.ibo; c.it = uint32_t(bc.it); return c;
}
static void back(BindCache &bc, const nrz5_cache &m) {   // after a chunk: the run's known fields are the real cache's
    if (m.known & NRZ5_K_PIPE) bc.pipe = hnd<VkPipeline>(m.pipe);
    if (m.known & NRZ5_K_SET) { bc.noff = m.noff; if (m.noff != UINT32_MAX) { bc.layout = hnd<VkPipelineLayout>(m.layout); bc.set = hnd<VkDescriptorSet>(m.set); if (m.noff) memcpy(bc.off, m.off, size_t(m.noff) * 4); } }
    if (m.known & NRZ5_K_VP) { memcpy(&bc.vp, m.vp, sizeof bc.vp); bc.has_vp = true; }
    if (m.known & NRZ5_K_SC) { memcpy(&bc.sc, m.sc, sizeof bc.sc); bc.has_sc = true; }
    for (int i = 0; i < 16; i++) if (m.known_vb >> i & 1) { bc.vb[i] = hnd<VkBuffer>(m.vb[i]); bc.vbo[i] = m.vbo[i]; }
    if (m.known & NRZ5_K_IB) { bc.ib = hnd<VkBuffer>(m.ib); bc.ibo = m.ibo; bc.it = VkIndexType(m.it); }
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
    { const uint32_t n = (32u << 20) / nbmt::q.chunk_bytes; nbmt::q.spare_max = n < 8 ? 8 : n > 1024 ? 1024 : n; }   // (the 1 MB shape: 32 chunks - the writer often finds none)
}
static void finish() {
    g_st = nbmt::q.st; { std::lock_guard<std::mutex> l(nbmt::q.mu); g_ws = nbmt::q.ws; }
    nbmt::stop(); g_slow.store(0); nbmt::q.st = nbmtq::Stats{}; { std::lock_guard<std::mutex> l(nbmt::q.mu); nbmt::q.ws = nbmtq::WorkerStats{}; CHECK(!nbmt::q.running && !nbmt::q.free_list && !nbmt::q.head, "stop: worker gone, chunk memory returned"); }
}
static std::vector<uint8_t> today(const std::vector<Ev> &p) {
    Log lg; VkCommandBuffer c = reinterpret_cast<VkCommandBuffer>(&lg); nbmt::report_ns = nbmtq::now_ns(); nbmt::begin(c, true, false);
    BindCache bc;
    for (const Ev &e : p) { if (e.kind) other(c, bc, e); else record(c, bc, e); }
    nbmt::end(c); return std::move(lg.b);
}

// ---------------- the writer thread ----------------
struct Recording { size_t s; std::vector<uint32_t> marks; };
struct Handoff { std::mutex mu; std::condition_variable cv; std::deque<Recording> q; bool done = false; };
static nrz5w::Ring g_ring;
static nrz5w::WStats writer_thread(const std::vector<std::vector<Ev>> &prog, Handoff &h, uint32_t chunk_cap_seed) {
    nrz5w::Writer W; W.q = &nbmt::q; W.ring = &g_ring; Rng r{chunk_cap_seed + 5}; W.min_tails = 1 + chunk_cap_seed % 3;   // (1..3: runs published without a chunk happen)
    uint32_t limit = ~0u;   // sometimes a chunk with little room: the run ends where it is full (the room is cut only inside emit, while the chunk is the writer's alone)
    auto emit = [&](nbmtq::Chunk *ch, nrz5_cache *m, const nrz5_vals *v) { const uint32_t cap0 = ch->cap; if (limit < cap0) ch->cap = limit; nbmt::Build b{ch}; nbmt::Z5Sink sink{b}; const unsigned n = nrz5_emit(sink, m, v); ch->cap = cap0; return n; };
    for (size_t s = 0; s < prog.size(); s++) {
        Recording rec; rec.s = s; rec.marks.resize(prog[s].size());
        for (size_t i = 0; i < prog[s].size(); i++) {
            const Ev &e = prog[s][i];
            if (e.kind) { if (NRZ5W_TEST_DROP != 2 || e.kind == 2) W.end_run(); rec.marks[i] = 0; continue; }
            if (!W.cur.chunk) limit = r.upto(6) == 0 ? 400 + r.upto(700) : ~0u;   // (the next tail's chunk)
            rec.marks[i] = W.draw(e.v, emit);
        }
        W.end_run();
        std::unique_lock<std::mutex> l(h.mu); h.cv.wait(l, [&] { return h.q.size() < 3; }); h.q.push_back(std::move(rec)); h.cv.notify_all();
    }
    { std::lock_guard<std::mutex> l(h.mu); h.done = true; } h.cv.notify_all();
    return W.st;
}
struct Tot { uint64_t draws = 0, recorded = 0, abandoned = 0, direct = 0; nrz5w::RStats r; nrz5w::WStats w; } tot;
static uint64_t g_abandoned_all = 0;   // over every shape (one shape of 60 recordings may have none: 4 runs of the test in 24 on a loaded host)
// All recordings with the writer ahead. verify: nothing is taken, the chunks are compared. Returns the logs (an abandoned recording: empty).
static std::vector<std::vector<uint8_t>> with_writer(const std::vector<std::vector<Ev>> &prog, uint64_t seed, bool verify, std::vector<int> &executed, const int *fates = nullptr) {
    std::vector<std::vector<uint8_t>> logs(prog.size()); executed.assign(prog.size(), 0);
    Handoff h; nrz5w::WStats ws; tot = Tot{};
    std::thread wt([&] { ws = writer_thread(prog, h, uint32_t(seed)); });
    nrz5w::Reader R; R.q = &nbmt::q; R.ring = &g_ring; Rng r{seed * 31 + 3};
    for (;;) {
        Recording rec;
        { std::unique_lock<std::mutex> l(h.mu); h.cv.wait(l, [&] { return !h.q.empty() || h.done; }); if (h.q.empty()) break; rec = std::move(h.q.front()); h.q.pop_front(); h.cv.notify_all(); }
        const std::vector<Ev> &p = prog[rec.s];
        const uint32_t fate = fates ? uint32_t(fates[rec.s]) : verify ? 9 : r.upto(14);
        if (fate == 0) { for (uint32_t mw : rec.marks) R.abandon(mw); ++tot.abandoned; continue; }   // never executed; the reader is told
        if (fate == 1) { ++tot.abandoned; continue; }                   // never executed and nobody tells the reader: the ids say so at the next run
        if (fate == 3 && !r.upto(4)) R.drop();                          // everything written ahead so far is thrown away (a device loss, a shutdown path): the records are recorded as today
        const bool direct = fate == 2; if (direct) ++tot.direct;        // recorded by direct calls (no pool for the worker: nbmt::begin's fallback)
        executed[rec.s] = 1;
        Log lg; VkCommandBuffer c = reinterpret_cast<VkCommandBuffer>(&lg); nbmt::report_ns = nbmtq::now_ns(); nbmt::begin(c, !direct, false);
        BindCache bc;
        for (size_t i = 0; i < p.size(); i++) {
            const Ev &e = p[i];
            if (e.kind) { other(c, bc, e); continue; }
            ++tot.draws;
            if (nbmt::cur != c) { R.abandon(rec.marks[i]); record(c, bc, e); ++tot.recorded; continue; }   // this recording goes by direct calls: nothing written ahead is used
            auto real = [&] { return conv(bc); };
            // the translation thread's own verdict on a run: none of its tail draws does more than its commands, and "the stamps stand" (1 in 8 do not)
            auto ok = [&](const nrz5w::Run &run) { for (uint32_t k = 0; k < run.tails && i + k < p.size(); k++) if (p[i + k].foreign) return false; return verify || fates || r.upto(8) != 0; };
            if (verify) {
                R.verify_begin(rec.marks[i], real, ok);
                nbmtq::Chunk *c0 = nbmt::q.cur; const uint32_t u0 = c0 ? c0->used : 0; const uint64_t ch0 = nbmt::q.st.chunks;
                record(c, bc, e); ++tot.recorded;
                nbmtq::Chunk *cc = c0 ? c0 : nbmt::q.cur; const uint32_t from = c0 ? u0 : 0; const bool readable = cc && cc == nbmt::q.cur && ch0 == nbmt::q.st.chunks;
                R.verify_end(rec.marks[i], readable ? cc->data() + from : nullptr, readable ? cc->used - from : 0,
                             [](const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb) { uint32_t at = 0; return nbmt::same_stream(a, na, b, nb, &at); },
                             [](uint16_t op) { return op == nbmt::OP_Draw || op == nbmt::OP_DrawIndexed; });
                continue;
            }
            nrz5_cache after;
            const int a = R.draw(rec.marks[i], real, ok, &after);
            if (a == nrz5w::Reader::RECORD) { record(c, bc, e); ++tot.recorded; }
            else if (a == nrz5w::Reader::SPLICED) { if (NRZ5W_TEST_DROP != 1) back(bc, after); }
        }
        nbmt::end(c); logs[rec.s] = std::move(lg.b);
        if (verify) CHECK(!R.v_on, "verify: a run ends inside its recording");
    }
    wt.join(); R.drop();
    tot.r = R.st; tot.w = ws;
    CHECK(nbmt::q.spare_out.load() == 0, "every spare chunk came back (%u out)", nbmt::q.spare_out.load());
    CHECK(g_ring.front() == nullptr, "the ring is empty");
    return logs;
}

int main(int argc, char **argv) {
    const unsigned scale = argc > 1 ? unsigned(atoi(argv[1])) : 1;
    const unsigned subs = 40 * (scale ? scale : 1), per = 1500;
    uint64_t packets = 0, spliced = 0, vcmp = 0;
    for (uint64_t seed = 1; seed <= 3; seed++) {
        std::vector<std::vector<Ev>> prog; for (unsigned s = 0; s < subs; s++) prog.push_back(program(seed * 1000 + s, per / 2 + unsigned(seed * 131 + s * 17) % per));
        std::vector<std::vector<uint8_t>> ref;
        setup(Cfg{0, 256, 64, 64, 0}); for (auto &p : prog) ref.push_back(today(p)); finish();
        const Cfg shapes[] = {{1, 256, 64, 64, 0}, {2, 256, 64, 64, 0}, {3, 256, 64, 64, 0}, {2, 16, 16, 2, 0}, {3, 16, 16, 2, 7}, {3, 65536, 1024, 1024, 0}};
        for (const Cfg &cf : shapes) {
            char what[96]; snprintf(what, sizeof what, "seed %llu mode %d flush %u chunk %u KB queue %u slow %d", (unsigned long long)seed, cf.mode, cf.flush, cf.chunk_kb, cf.queue, cf.slow);
            std::vector<int> ex;
            // RUNS
            setup(cf); { const auto logs = with_writer(prog, seed * 77 + cf.mode, false, ex);
                for (size_t s = 0; s < prog.size(); s++) if (ex[s]) CHECK(logs[s] == ref[s], "%s: with the writer the calls are today's in today's order (recording %zu: %zu / %zu bytes)", what, s, logs[s].size(), ref[s].size()); }
            const Tot t = tot; finish();
            // (1 MB chunks: the writer finds a spare chunk for few runs, so whether one of them belongs to an abandoned recording - "not
            // reached" - depends on which thread was faster: 1 run of the test in ~10 on a loaded host had none. As for "declined by the
            // cache", that path is asked of the other shapes.)
            CHECK(t.r.taken > 0 && t.r.covered > t.r.taken && (t.r.declined_cache > 0 || cf.chunk_kb > 64) && t.r.declined_stamp > 0 && (g_abandoned_all += t.abandoned, true) && (t.r.unreached > 0 || cf.chunk_kb > 64 || !t.abandoned), "%s: runs taken %llu (covered %llu), declined by the cache %llu / the verdict %llu, recordings abandoned %llu, runs not reached %llu",
                  what, (unsigned long long)t.r.taken, (unsigned long long)t.r.covered, (unsigned long long)t.r.declined_cache, (unsigned long long)t.r.declined_stamp, (unsigned long long)t.abandoned, (unsigned long long)t.r.unreached);
            CHECK(t.w.chunk_full > 0 && g_st.spliced == t.r.taken && g_st.alloc_fail == 0, "%s: runs ended by a full chunk %llu; chunks handed over %llu", what, (unsigned long long)t.w.chunk_full, (unsigned long long)g_st.spliced);
            if (cf.mode != 1) CHECK(g_ws.cmds + g_st.help_cmds == g_st.cmds && g_ws.chunks + g_st.help_chunks == g_st.chunks, "%s: every packet replayed once", what);
            if (cf.mode == 2) CHECK(g_ws.checked == g_st.chunks && !g_ws.bad[0] && !g_ws.bad[1] && !g_ws.bad[2] && !g_ws.bad[3], "%s: the queue's check is clean (%llu chunks)", what, (unsigned long long)g_ws.checked);
            packets += g_st.cmds; spliced += g_st.spliced_cmds;
            // VERIFY
            setup(cf); { const auto logs = with_writer(prog, seed * 177 + cf.mode, true, ex);
                for (size_t s = 0; s < prog.size(); s++) CHECK(ex[s] && logs[s] == ref[s], "%s: verify records today's calls (recording %zu)", what, s); }
            const Tot v = tot; finish();
            CHECK(v.r.v_bad == 0 && v.r.v_same == v.r.v_cmp && v.r.v_cmp > 0 && v.r.v_not_comparable > 0, "%s: verify - compared %llu, same %llu, bad %llu, runs not comparable %llu", what, (unsigned long long)v.r.v_cmp,
                  (unsigned long long)v.r.v_same, (unsigned long long)v.r.v_bad, (unsigned long long)v.r.v_not_comparable);
            vcmp += v.r.v_cmp;
            printf("ok   %-56s draws %6llu: writer heads %6llu tails %6llu runs %5llu (no spare chunk %5llu, chunk full %4llu, too short %4llu) | taken %5llu covered %6llu declined cache %4llu verdict %4llu, run no longer in the ring %3llu, not reached %4llu, abandoned %2llu direct %2llu | verify: %6llu draws compared (%llu not readable)\n",
                   what, (unsigned long long)t.w.draws, (unsigned long long)t.w.heads, (unsigned long long)t.w.tails, (unsigned long long)t.w.runs, (unsigned long long)t.w.no_chunk, (unsigned long long)t.w.chunk_full, (unsigned long long)t.w.short_runs, (unsigned long long)t.r.taken, (unsigned long long)t.r.covered,
                   (unsigned long long)t.r.declined_cache, (unsigned long long)t.r.declined_stamp, (unsigned long long)t.r.gone, (unsigned long long)t.r.unreached, (unsigned long long)t.abandoned, (unsigned long long)t.direct,
                   (unsigned long long)v.r.v_cmp, (unsigned long long)v.r.v_skipped);
        }
    }
    // a recording that is never executed and nobody says so, followed by its twin (the same binds, other draw arguments): the twin's
    // records must get the twin's runs - the abandoned recording's runs are older ids at the front of the ring
    { std::vector<std::vector<Ev>> prog; prog.push_back(program(4242, 900)); for (auto &e : prog[0]) e.foreign = false;
      prog.push_back(prog[0]); for (auto &e : prog[1]) { e.v.off = e.offs; if (!e.kind) e.v.count += 1; }
      prog.push_back(prog[0]); for (auto &e : prog[2]) { e.v.off = e.offs; if (!e.kind) e.v.count += 2; }
      std::vector<std::vector<uint8_t>> ref; setup(Cfg{0, 256, 64, 64, 0}); for (auto &p : prog) ref.push_back(today(p)); finish();
      for (int mode = 1; mode <= 3; mode++) { const int fates[3] = {1, 9, 9}; std::vector<int> ex;
          setup(Cfg{mode, 256, 64, 64, 0}); const auto logs = with_writer(prog, 5, false, ex, fates); const Tot t = tot; finish();
          CHECK(!ex[0] && ex[1] && ex[2] && logs[1] == ref[1] && logs[2] == ref[2], "mode %d: after an abandoned recording its twin's calls are the twin's", mode);
          CHECK(t.r.taken > 20 && t.r.unreached > 20 && t.r.gone == 0 && t.r.declined_cache == 0, "mode %d: the twin's runs were taken (%llu), the abandoned ones given back (%llu)", mode, (unsigned long long)t.r.taken, (unsigned long long)t.r.unreached); } }
    // the mark word and the ring by themselves
    { CHECK(nrz5w::mark_of(nrz5w::mark_word(nrz5w::MARK_COVERED, 77)) == nrz5w::MARK_COVERED && nrz5w::id_of(nrz5w::mark_word(nrz5w::MARK_SPLICE, nrz5w::ID_MAX)) == uint32_t(nrz5w::ID_MAX) && nrz5w::mark_word(nrz5w::MARK_NONE, 0) == 0, "mark word");
      static nrz5w::Ring rg; CHECK(rg.front() == nullptr && rg.room(), "empty ring"); nrz5w::Run x; for (uint32_t i = 0; i < nrz5w::Ring::N; i++) { CHECK(rg.room(), "room"); x.id = i + 1; rg.push(x); }
      CHECK(!rg.room() && rg.front() && rg.front()->id == 1, "full ring, first in first out"); rg.pop(); CHECK(rg.room() && rg.front()->id == 2, "pop"); }
    CHECK(g_abandoned_all > 0, "recordings were abandoned in some shape (%llu)", (unsigned long long)g_abandoned_all);
    printf("nr_z5w_test: %ld checks, %ld failed, %llu packets (%llu of them in chunks written by the second thread), %llu tail draws compared by verify -> %s\n", checks, fails, (unsigned long long)packets,
           (unsigned long long)spliced, (unsigned long long)vcmp, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
