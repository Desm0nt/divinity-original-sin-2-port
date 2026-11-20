// [DOS2] NR-ZERO Z5 host test (docs/dos2/nr/NR-ZERO.md section 14): the run assembler (dm/gfx/nr/nr_z5.h), the chunk builder and the
// stream comparison of ../xbg_nb_mt.inc, and the hand-over at a position (nbmtq::Queue::splice of dm/gfx/nb/nb_mt.h), against the mock
// Vulkan of nb_mt_test.cpp (a "command buffer" is a byte log of the calls made on it).
//   TODAY   a random program - engine draws (pipelines, sets with 0..33 dynamic offsets, viewports, scissors, vertex bindings and index
//           buffers from small pools, so that binds repeat and are dropped) and other commands (passes, clears; some reset the bind
//           cache) - is recorded the way the translation thread records it today: every draw by ref_tail(), a copy of the command
//           section of nrdraw::lane_tail (xbg_nr_draw.inc) over the real bind cache, through the wrappers.
//   SHADOW  while TODAY runs (packet modes), every draw that reads only fields its run already set is also written by nrz5_emit over the
//           run's model cache into a chunk of its own: same_stream() must find the same packets as the wrappers just wrote, and the
//           known fields of the model must be the real cache's after every draw (what DM_NR_Z5=2 checks on the phone).
//   RUNS    the same program recorded the Z5 way: each run of consecutive draws = head draws by ref_tail (they read a field the run
//           does not know yet), then the tail written by nrz5_emit into a chunk taken from the queue and handed over by splice() at
//           the position of its first draw; the real cache gets the model's known fields. A full chunk or a draw that reads a new
//           unknown field ends the run.
//   The command logs of RUNS in modes 1 / 2 / 3 (several queue shapes) are byte-identical to TODAY's direct calls (mode 0): the same
//   commands with the same arguments in the same order - passes and barriers included, since everything else goes through the same
//   wrappers at the same positions. Mode 2: no finding (hash, hand-over order, walk). Every packet written is replayed once.
// The mutation check: built with -DNRZ5_MUTATE=k (k = 1..11, nr_z5.h) or -DNRZ5_TEST_DROP=k (k = 1 the model's cache is not copied back
// after a chunk, 2 the chunk is handed over at the END of the run instead of at its first tail draw's position, 3 a draw that reads an
// unknown field is written ahead anyway) this test must FAIL.
// Build (Vulkan headers: any vulkan/ + vk_video/ include tree, e.g. the NDK sysroot's usr/include), from the repository root:
//   clang++ -std=c++17 -O2 -Wall -Wextra -I<vk> dm/gfx/vulkan/engine/tests/nr_z5_test.cpp -o nr_z5_test && ./nr_z5_test
//   g++ -std=c++17 -O1 -g -fsanitize=thread -pthread -I<vk> dm/gfx/vulkan/engine/tests/nr_z5_test.cpp -o nr_z5_tsan && ./nr_z5_tsan
#include <vulkan/vulkan_core.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>
#ifndef NRZ5_TEST_DROP
#define NRZ5_TEST_DROP 0
#endif

static std::vector<std::string> g_lines;
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void xbg_logf(const char *fmt, ...) { char t[2048]; va_list a; va_start(a, fmt); vsnprintf(t, sizeof t, fmt, a); va_end(a); g_lines.push_back(t); }
#include "nb_mt_mock.h"
#define NBMT_REAL(f) mock_##f
#include "../xbg_nb_mt.inc"
#include "../../../nr/nr_z5.h"

static long fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
struct Rng { uint64_t s; uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); } uint32_t upto(uint32_t n) { return n ? next() % n : 0; } };
template <class H> static H hnd(uint64_t x) { return H(uintptr_t(x)); }

// ---------------- the program ----------------
struct Ev { int kind; nrz5_vals v; uint32_t offs[40]; };   // kind 0: an engine draw; 1: another command; 2: another command that starts a pass (the bind cache is reset)
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
}
static std::vector<Ev> program(uint64_t seed, unsigned n) {
    Rng r{seed * 0x9E3779B97F4A7C15ull + 7}; std::vector<Ev> p(n);
    for (auto &e : p) { const uint32_t k = r.upto(100); e.kind = k < 88 ? 0 : k < 96 ? 1 : 2; if (!e.kind) make_draw(r, e); else { memset(&e.v, 0, sizeof e.v); e.v.count = r.next(); } }
    for (auto &e : p) e.v.off = e.offs;   // (the vector does not move any more)
    return p;
}

// ---------------- TODAY: the command section of nrdraw::lane_tail over the real bind cache ----------------
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
static void other(VkCommandBuffer c, BindCache &bc, const Ev &e) {
    if (e.kind == 2) { nbmt::CmdEndRenderPass(c); VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; b.renderPass = hnd<VkRenderPass>(e.v.count & 7); b.renderArea.extent = {640, 360};
        nbmt::CmdBeginRenderPass(c, &b, VK_SUBPASS_CONTENTS_INLINE); bc = BindCache{}; }
    else if (e.v.count & 1) nbmt::CmdFillBuffer(c, hnd<VkBuffer>(0x77), e.v.count & 0xff0, 16, e.v.count);
    else { VkClearAttachment a{}; a.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; VkClearRect rc{}; rc.rect.extent = {uint32_t(e.v.count & 255), 4}; rc.layerCount = 1; nbmt::CmdClearAttachments(c, 1, &a, 1, &rc); }
}
// the real cache <-> the words of nr_z5.h
static nrz5_cache conv(const BindCache &bc) {
    nrz5_cache c; nrz5_cache_unknown(&c); c.known = NRZ5_K_PIPE | NRZ5_K_SET | NRZ5_K_VP | NRZ5_K_SC | NRZ5_K_IB; c.known_vb = 0xffff;
    c.pipe = uint64_t(uintptr_t(bc.pipe)); c.layout = uint64_t(uintptr_t(bc.layout)); c.set = uint64_t(uintptr_t(bc.set)); c.noff = bc.noff; memcpy(c.off, bc.off, sizeof c.off);
    c.has_vp = bc.has_vp; c.has_sc = bc.has_sc; memcpy(c.vp, &bc.vp, sizeof bc.vp); memcpy(c.sc, &bc.sc, sizeof bc.sc);
    for (int i = 0; i < 16; i++) { c.vb[i] = uint64_t(uintptr_t(bc.vb[i])); c.vbo[i] = bc.vbo[i]; }
    c.ib = uint64_t(uintptr_t(bc.ib)); c.ibo = bc.ibo; c.it = uint32_t(bc.it); return c;
}
static void back(BindCache &bc, const nrz5_cache &m) {   // after a chunk: the model's known fields are the real cache's
    if (m.known & NRZ5_K_PIPE) bc.pipe = hnd<VkPipeline>(m.pipe);
    if (m.known & NRZ5_K_SET) { bc.noff = m.noff; if (m.noff != UINT32_MAX) { bc.layout = hnd<VkPipelineLayout>(m.layout); bc.set = hnd<VkDescriptorSet>(m.set); if (m.noff) memcpy(bc.off, m.off, size_t(m.noff) * 4); } }
    if (m.known & NRZ5_K_VP) { memcpy(&bc.vp, m.vp, sizeof bc.vp); bc.has_vp = true; }
    if (m.known & NRZ5_K_SC) { memcpy(&bc.sc, m.sc, sizeof bc.sc); bc.has_sc = true; }
    for (int i = 0; i < 16; i++) if (m.known_vb >> i & 1) { bc.vb[i] = hnd<VkBuffer>(m.vb[i]); bc.vbo[i] = m.vbo[i]; }
    if (m.known & NRZ5_K_IB) { bc.ib = hnd<VkBuffer>(m.ib); bc.ibo = m.ibo; bc.it = VkIndexType(m.it); }
}

struct Cfg { int mode; unsigned flush, chunk_kb, queue; int slow; };
struct Tot { uint64_t draws = 0, head = 0, tail = 0, runs = 0, chunks = 0, sh_cmp = 0, sh_skip = 0, full = 0; } tot;
static nbmtq::WorkerStats g_ws; static nbmtq::Stats g_st;
static void setup(const Cfg &cfg) {
    char e[4][64]; snprintf(e[0], 64, "%d", cfg.mode); snprintf(e[1], 64, "%u", cfg.flush); snprintf(e[2], 64, "%u", cfg.chunk_kb); snprintf(e[3], 64, "%u", cfg.queue);
#if defined(_WIN32)
    _putenv_s("DM_NB_MT", e[0]); _putenv_s("DM_NB_MT_FLUSH", e[1]); _putenv_s("DM_NB_MT_CHUNK_KB", e[2]); _putenv_s("DM_NB_MT_QUEUE", e[3]);
#else
    setenv("DM_NB_MT", e[0], 1); setenv("DM_NB_MT_FLUSH", e[1], 1); setenv("DM_NB_MT_CHUNK_KB", e[2], 1); setenv("DM_NB_MT_QUEUE", e[3], 1);
#endif
    nbmt::init(); g_slow.store(cfg.slow); g_ws = nbmtq::WorkerStats{}; g_st = nbmtq::Stats{}; tot = Tot{};
}
static void finish() {
    g_st = nbmt::q.st; { std::lock_guard<std::mutex> l(nbmt::q.mu); g_ws = nbmt::q.ws; }
    nbmt::stop(); g_slow.store(0); nbmt::q.st = nbmtq::Stats{}; { std::lock_guard<std::mutex> l(nbmt::q.mu); nbmt::q.ws = nbmtq::WorkerStats{}; CHECK(!nbmt::q.running && !nbmt::q.free_list && !nbmt::q.head, "stop: worker gone, chunk memory returned"); }
}
// One submission of the program, the way of today; shadow: the DM_NR_Z5=2 comparison alongside.
static std::vector<uint8_t> today(const std::vector<Ev> &p, bool shadow) {
    Log lg; VkCommandBuffer c = reinterpret_cast<VkCommandBuffer>(&lg); nbmt::report_ns = nbmtq::now_ns(); nbmt::begin(c, true, false);
    BindCache bc; nrz5_cache model; nrz5_cache_unknown(&model);
    nbmtq::Chunk *scratch = static_cast<nbmtq::Chunk *>(malloc(sizeof(nbmtq::Chunk) + 8192)); scratch->cap = 8192;
    for (const Ev &e : p) {
        if (e.kind) { other(c, bc, e); nrz5_cache_unknown(&model); continue; }   // any other command ends the run
        if (!shadow || nbmt::cur != c) { ref_tail(c, bc, e.v); continue; }
        const bool ahead = !nrz5_reads_unknown(&model, &e.v);
        nbmtq::Chunk *c0 = nbmt::q.cur; const uint32_t u0 = c0 ? c0->used : 0; const uint64_t ch0 = nbmt::q.st.chunks;
        nrz5_cache before = model;
        ref_tail(c, bc, e.v);
        if (!ahead) nrz5_after(&model, &e.v);
        else {
            scratch->used = scratch->cmds = 0; nbmt::Build b{scratch}; nbmt::Z5Sink sink{b}; model = before;
            const unsigned n = nrz5_emit(sink, &model, &e.v); CHECK(n > 0, "shadow: the draw fits the scratch chunk");
            if (c0 && c0 == nbmt::q.cur && ch0 == nbmt::q.st.chunks) { uint32_t at = 0; ++tot.sh_cmp;
                CHECK(nbmt::same_stream(c0->data() + u0, c0->used - u0, scratch->data(), scratch->used, &at), "shadow: the packets written ahead are the wrappers' (packet %u)", at);
                if (scratch->used) { uint32_t at2 = 0; reinterpret_cast<nbmt::Pkt *>(scratch->data())->op ^= 1; CHECK(!nbmt::same_stream(c0->data() + u0, c0->used - u0, scratch->data(), scratch->used, &at2) && at2 == 0, "same_stream sees another operation"); }
                CHECK(!nbmt::same_stream(c0->data() + u0, c0->used - u0, scratch->data(), 0, &at), "same_stream sees a stream that ends early"); }
            else ++tot.sh_skip;   // a hand-over inside the draw: its packets are no longer the producer's to read
        }
        const nrz5_cache real = conv(bc); const int d = nrz5_cache_diff(&model, &real); CHECK(d == 0, "shadow: the model's known fields are the real cache's (field %d)", d);
    }
    free(scratch); nbmt::end(c); return std::move(lg.b);
}
// The same submission with the tails of the runs written ahead and handed over by splice().
static std::vector<uint8_t> runs(const std::vector<Ev> &p, uint64_t seed) {
    Log lg; VkCommandBuffer c = reinterpret_cast<VkCommandBuffer>(&lg); nbmt::report_ns = nbmtq::now_ns(); nbmt::begin(c, true, false);
    BindCache bc; Rng r{seed + 99};
    size_t i = 0;
    while (i < p.size()) {
        if (p[i].kind) { other(c, bc, p[i]); ++i; continue; }
        if (nbmt::cur != c) { ref_tail(c, bc, p[i].v); ++i; continue; }
        size_t end = i; const size_t cap = 1 + r.upto(40); while (end < p.size() && !p[end].kind && end - i < cap) ++end;   // the run: consecutive draws (a random cap: a run may end anywhere)
        ++tot.runs;
        nrz5_cache model; nrz5_cache_unknown(&model);
        while (i < end && NRZ5_TEST_DROP != 3 && nrz5_reads_unknown(&model, &p[i].v)) { ref_tail(c, bc, p[i].v); nrz5_after(&model, &p[i].v); ++tot.head; ++i; }
        if (i == end) continue;
        nbmt::Build b{nbmt::q.take(nbmt::q.chunk_bytes)}; CHECK(b.c != nullptr, "a chunk for the run"); if (!b.c) break;
        const uint32_t cap0 = b.c->cap; if (r.upto(6) == 0) b.c->cap = 600 + r.upto(600);   // sometimes a chunk with little room left: the run ends where it is full
        nbmt::Z5Sink sink{b}; size_t j = i;
        std::vector<const Ev *> late;
        for (; j < end; j++) {
            if (NRZ5_TEST_DROP != 3 && nrz5_reads_unknown(&model, &p[j].v)) break;                 // a new unknown field after the tail began: the run ends
            const nrz5_cache saved = model; const uint32_t u = b.c->used, cm = b.c->cmds;
            if (!nrz5_emit(sink, &model, &p[j].v)) { model = saved; b.c->used = u; b.c->cmds = cm; ++tot.full; break; }   // the chunk is full: the run ends before this draw
            ++tot.tail;
        }
        if (j == i) { b.c->cap = cap0; nbmt::q.give_back(b.c); ref_tail(c, bc, p[i].v); ++tot.head; ++i; continue; }   // not even one draw fits (never with these sizes) / the first one reads an unknown field
        if (NRZ5_TEST_DROP == 2) { for (size_t k = j; k < end; k++) late.push_back(&p[k]); }
        if (NRZ5_TEST_DROP == 2 && !late.empty()) { for (const Ev *e : late) ref_tail(c, bc, e->v); b.c->cap = cap0; nbmt::q.splice(b.c); ++tot.chunks; i = end; continue; }
        b.c->cap = cap0; nbmt::q.splice(b.c); ++tot.chunks;
        if (NRZ5_TEST_DROP != 1) back(bc, model);
        i = j;   // what is left of the run starts the next one (head first)
    }
    nbmt::end(c); return std::move(lg.b);
}

int main(int argc, char **argv) {
    const unsigned scale = argc > 1 ? unsigned(atoi(argv[1])) : 1;
    const unsigned subs = 24 * (scale ? scale : 1), per = 1500;
    uint64_t packets = 0, spliced = 0, sh = 0;
    for (uint64_t seed = 1; seed <= 3; seed++) {
        std::vector<std::vector<Ev>> prog; for (unsigned s = 0; s < subs; s++) prog.push_back(program(seed * 1000 + s, per / 2 + unsigned(seed * 131 + s * 17) % per));
        std::vector<std::vector<uint8_t>> ref;
        setup(Cfg{0, 256, 64, 64, 0}); for (auto &p : prog) ref.push_back(today(p, false)); finish();
        const Cfg shapes[] = {{1, 256, 64, 64, 0}, {2, 256, 64, 64, 0}, {3, 256, 64, 64, 0}, {1, 16, 16, 2, 0}, {2, 16, 16, 2, 0}, {3, 16, 16, 2, 0}, {3, 16, 16, 2, 7}, {2, 32, 16, 4, 3}, {3, 65536, 1024, 1024, 0}};
        for (const Cfg &cf : shapes) {
            char what[96]; snprintf(what, sizeof what, "seed %llu mode %d flush %u chunk %u KB queue %u slow %d", (unsigned long long)seed, cf.mode, cf.flush, cf.chunk_kb, cf.queue, cf.slow);
            // TODAY through packets, with the shadow comparison
            setup(cf); for (size_t s = 0; s < prog.size(); s++) { const auto lg = today(prog[s], true); CHECK(lg == ref[s], "%s: today's packets give today's calls (submission %zu)", what, s); }
            const Tot sh_t = tot; finish(); sh += sh_t.sh_cmp;
            CHECK(sh_t.sh_cmp > 0, "%s: the shadow compared draws", what);
            // RUNS
            setup(cf); for (size_t s = 0; s < prog.size(); s++) { const auto lg = runs(prog[s], seed * 77 + s); CHECK(lg == ref[s], "%s: the runs give today's calls in today's order (submission %zu: %zu / %zu bytes)", what, s, lg.size(), ref[s].size()); }
            const Tot t = tot; finish();
            CHECK(t.chunks > 0 && t.tail > t.head && g_st.spliced == t.chunks, "%s: chunks were handed over (%llu, tail %llu, head %llu)", what, (unsigned long long)t.chunks, (unsigned long long)t.tail, (unsigned long long)t.head);
            CHECK(g_st.alloc_fail == 0, "%s: allocation", what);
            if (cf.mode != 1) CHECK(g_ws.cmds + g_st.help_cmds == g_st.cmds && g_ws.chunks + g_st.help_chunks == g_st.chunks, "%s: every packet replayed once (worker %llu + translate %llu of %llu)", what,
                                    (unsigned long long)g_ws.cmds, (unsigned long long)g_st.help_cmds, (unsigned long long)g_st.cmds);
            if (cf.mode == 2) CHECK(g_ws.checked == g_st.chunks && !g_ws.bad[0] && !g_ws.bad[1] && !g_ws.bad[2] && !g_ws.bad[3], "%s: check clean (%llu chunks)", what, (unsigned long long)g_ws.checked);
            CHECK(t.full > 0, "%s: runs ended by a full chunk", what);
            packets += g_st.cmds; spliced += g_st.spliced_cmds;
            printf("ok   %-58s draws %7llu runs %6llu head %6llu tail %7llu chunks %6llu full %4llu shadow compared %7llu (hand-over inside %llu)\n", what, (unsigned long long)(t.head + t.tail), (unsigned long long)t.runs,
                   (unsigned long long)t.head, (unsigned long long)t.tail, (unsigned long long)t.chunks, (unsigned long long)t.full, (unsigned long long)sh_t.sh_cmp, (unsigned long long)sh_t.sh_skip);
        }
    }
    // the unknown test and the cache words by themselves
    { Ev e; Rng r{5}; make_draw(r, e); e.v.used = 0x5; e.v.indexed = 1; nrz5_cache m; nrz5_cache_unknown(&m);
      CHECK(nrz5_reads_unknown(&m, &e.v), "an empty model knows nothing"); nrz5_after(&m, &e.v); CHECK(!nrz5_reads_unknown(&m, &e.v), "after a draw every field it read is known");
      nrz5_vals w = e.v; w.used = 0x7; CHECK(nrz5_reads_unknown(&m, &w), "a binding the run has not bound"); w = e.v; w.indexed = 0; CHECK(!nrz5_reads_unknown(&m, &w), "a draw without indices reads no index buffer");
      nrz5_cache n2; nrz5_cache_unknown(&n2); w = e.v; w.indexed = 0; nrz5_after(&n2, &w); CHECK(nrz5_reads_unknown(&n2, &e.v), "the index buffer of a run whose draws had none");
      nrz5_cache real = m; CHECK(nrz5_cache_diff(&m, &real) == 0, "same"); real.pipe ^= 1; CHECK(nrz5_cache_diff(&m, &real) == 1, "pipeline"); real = m; real.vbo[2] += 4; CHECK(nrz5_cache_diff(&m, &real) == 5, "stream");
      real = m; real.vbo[1] += 4; CHECK(nrz5_cache_diff(&m, &real) == 0, "a stream the run does not know is not compared"); real = m; real.it ^= 1; CHECK(nrz5_cache_diff(&m, &real) == 6, "index"); }
    printf("nr_z5_test: %ld checks, %ld failed, %llu packets (%llu of them in handed-over chunks), %llu draws compared by the shadow -> %s\n", checks, fails, (unsigned long long)packets, (unsigned long long)spliced,
           (unsigned long long)sh, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
