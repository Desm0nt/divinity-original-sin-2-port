// [DOS2] NB-20b (ctx) host test: DM_NB_MX - the production ../xbg_nb_mx.inc (context accessor, continuation, segment profile) and
// dm/gfx/nb/nb_mx.h (registry, worker, classes, model), with ../xbg_nb_mt.inc and the mock Vulkan of nb_mt_test.cpp underneath.
// - registry: scalars, arrays, structs, vectors, map iterators, a 24 KB array; a slot registered twice / out of range is refused;
//   ctx_load / ctx_store round trip; a worker instance is apart from the main one until it is stored;
// - nbx::get: the main instance on every thread while no window is open; inside a window the worker's own instance on the worker and
//   still the main one on the poster and on a third thread;
// - continuation (DM_NB_MX=1): a "submission" that reads and writes every kind of context variable, records vkCmd* packets and calls
//   itself again (nested) gives the same variables and the same command-buffer log as with DM_NB_MX=0, over thousands of
//   submissions with the worker stopped and restarted, with the A/B phase off in between; it ran on another thread;
// - profile: every segment class from a packet sequence (unit) and through the real wrappers + nbx::mark (the "[nb] mx prof" line);
//   the two-phase model against hand-computed cases.
// Build (as nb_mt_test.cpp; <vk> = a vulkan/ + vk_video/ include tree):
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-missing-field-initializers -pthread -I<vk> nb_mx_test.cpp -o nb_mx_test && ./nb_mx_test
//   ... -fsanitize=thread ... / ... -fsanitize=address,undefined ...
#include <vulkan/vulkan_core.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static std::vector<std::string> g_lines;
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void xbg_logf(const char *fmt, ...) { char t[4096]; va_list a; va_start(a, fmt); vsnprintf(t, sizeof t, fmt, a); va_end(a); g_lines.push_back(t); }
#include "nb_mt_mock.h"
#define NBMT_REAL(f) mock_##f
#include "../xbg_nb_mt.inc"
#include "../xbg_nb_mx.inc"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void put_env(const char *k, const char *v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

// ---- context variables declared the way xbg_game.cpp declares them (the slots of the engine's list, test types) ----
struct Big { uint64_t serial = 0, epoch = 0; void *im = nullptr; };
struct Padded { uint8_t a = 1; uint64_t b = 2; uint16_t c = 3; std::vector<int> v; };
typedef std::unordered_map<int, int> Map;
static Map shared_map;
static uint64_t tex_gen_m0[6] = {};
#define tex_gen nbx::get(tex_gen_m0, nbx::K_tex_gen)
static unsigned cur_op_m0 = 7;
#define cur_op nbx::get(cur_op_m0, nbx::K_cur_op)
static std::vector<void *> draw_reads_m0;
#define draw_reads nbx::get(draw_reads_m0, nbx::K_draw_reads)
static Big img_front_m0[1024];
#define img_front nbx::get(img_front_m0, nbx::K_img_front)
static Map::iterator memo_front_it_m0;
#define memo_front_it nbx::get(memo_front_it_m0, nbx::K_memo_front_it)
static Padded state_m0;
#define state nbx::get(state_m0, nbx::K_state)
static int filler[nbx::K_COUNT];

static std::thread::id g_poster;
static std::atomic<int> g_other_thread{0};
static int depth_seen = 0;
static uint64_t h_fake[2] = {11, 22};
static Log g_cb;
// One "submission": context reads and writes, packets on the recorded command buffer, a nested submit.
static void run_sub(int n, const uint64_t *h, uint64_t &budget) {
    if (std::this_thread::get_id() != g_poster) ++g_other_thread;
    for (int i = 0; i < n; i++) budget += h[i];
    for (int k = 0; k < 6; k++) tex_gen[k] += uint64_t(k + 1) * budget;
    cur_op = cur_op * 31 + unsigned(budget);
    draw_reads.push_back(reinterpret_cast<void *>(uintptr_t(cur_op)));
    if (draw_reads.size() > 40) draw_reads.erase(draw_reads.begin(), draw_reads.begin() + 20);
    img_front[cur_op & 1023].serial += cur_op; img_front[(cur_op >> 3) & 1023].epoch ^= budget;
    shared_map[int(cur_op & 63)] += 1; memo_front_it = shared_map.find(int(cur_op & 63));
    state.b += memo_front_it->second; state.c = uint16_t(state.c + 1); state.v.push_back(int(state.b)); if (state.v.size() > 9) state.v.clear();
    VkCommandBuffer c = reinterpret_cast<VkCommandBuffer>(&g_cb);
    vkCmdDraw(c, cur_op, uint32_t(state.b), uint32_t(tex_gen[3]), uint32_t(draw_reads.size()));
    VkViewport vp{float(state.c), 0, 1, 1, 0, 1}; vkCmdSetViewport(c, 0, 1, &vp);
    if (!depth_seen) { ++depth_seen; uint64_t b2 = 0; if (!nbx::submit(1, h_fake, b2, false, run_sub)) run_sub(1, h_fake, b2); --depth_seen; }   // inside a window: refused (nested), run here
}
struct Snap { uint64_t tg[6]; unsigned op; std::vector<void *> dr; uint64_t sum; Padded st; int it_key; std::vector<uint8_t> log; uint64_t budget; };
static Snap snap(uint64_t budget) {
    Snap s; memcpy(s.tg, tex_gen_m0, sizeof s.tg); s.op = cur_op_m0; s.dr = draw_reads_m0; s.sum = 0; for (auto &b : img_front_m0) s.sum += b.serial * 3 + b.epoch;
    s.st = state_m0; s.it_key = memo_front_it_m0 == Map::iterator() ? -1 : memo_front_it_m0->first; s.log = g_cb.b; s.budget = budget; return s;
}
static bool same(const Snap &a, const Snap &b) {
    return !memcmp(a.tg, b.tg, sizeof a.tg) && a.op == b.op && a.dr == b.dr && a.sum == b.sum && a.st.a == b.st.a && a.st.b == b.st.b && a.st.c == b.st.c && a.st.v == b.st.v &&
           a.it_key == b.it_key && a.log == b.log && a.budget == b.budget;
}
static void reset_vars() {
    memset(tex_gen_m0, 0, sizeof tex_gen_m0); cur_op_m0 = 7; draw_reads_m0.clear(); for (auto &b : img_front_m0) b = Big{}; shared_map.clear(); memo_front_it_m0 = Map::iterator(); state_m0 = Padded{};
    g_cb.b.clear(); g_cb.calls = 0;
}
static Snap run_mode(int mx, int mt, unsigned subs) {
    char e[8]; snprintf(e, sizeof e, "%d", mx); put_env("DM_NB_MX", e); snprintf(e, sizeof e, "%d", mt); put_env("DM_NB_MT", e);
    nbmt::init(); nbx::init(); nbx::prof_init(); reset_vars(); g_other_thread = 0; g_poster = std::this_thread::get_id();
    VkCommandBuffer c = reinterpret_cast<VkCommandBuffer>(&g_cb); uint64_t total = 0;
    for (unsigned s = 0; s < subs; s++) {
        const bool ab_off = s % 9 == 4;
        nbmt::begin(c, true, false);
        uint64_t budget = s;
        if (!nbx::submit(2, h_fake, budget, ab_off, run_sub)) run_sub(2, h_fake, budget);
        CHECK(!nbx::par.load() && !nbx::window, "no window left open");
        nbmt::end(c);
        total += budget;
        if (s % 211 == 210) { nbx::stop(); nbmt::stop(); }   // both workers end and start again
    }
    nbx::stop(); nbmt::stop();
    return snap(total);
}

int main(int argc, char **argv) {
    const unsigned subs = argc > 1 ? unsigned(atoi(argv[1])) : 3000;
    // ---- registry ----
    { nbxq::Registry r{}; int a = 5; uint64_t arr[6] = {1, 2, 3, 4, 5, 6}; Padded p; p.v = {1, 2, 3}; std::vector<std::string> vs = {"x", "yy"}; Big big[1024]; big[7].serial = 9; Map m; m[3] = 4; Map::iterator it = m.find(3);
      CHECK(nbxq::reg(r, 0, &a, "a") && nbxq::reg(r, 1, &arr, "arr") && nbxq::reg(r, 2, &p, "p") && nbxq::reg(r, 3, &vs, "vs") && nbxq::reg(r, 4, &big, "big") && nbxq::reg(r, 5, &it, "it"), "register");
      CHECK(nbxq::complete(r, 6) && !nbxq::complete(r, 7) && nbxq::ctx_bytes(r) >= sizeof big, "complete / bytes");
      nbxq::Registry bad = r; int z = 0; CHECK(!nbxq::reg(bad, 2, &z, "dup") && !nbxq::complete(bad, 6), "a slot registered twice is refused and poisons the registry");
      nbxq::Registry bad2{}; CHECK(!nbxq::reg(bad2, nbxq::MAX_VARS, &z, "far") && !nbxq::reg(bad2, -1, &z, "neg"), "slot out of range");
      nbxq::Registry gap{}; nbxq::reg(gap, 0, &a, "a"); nbxq::reg(gap, 2, &z, "z"); CHECK(!nbxq::complete(gap, 3), "a missing slot = not complete");
      nbxq::Ctx *c = nbxq::ctx_new(r);
      CHECK(*static_cast<int *>(c->p[0]) == 0 && (*static_cast<uint64_t (*)[6]>(c->p[1]))[5] == 0 && static_cast<Padded *>(c->p[2])->b == 2, "a worker instance starts value-initialised (member defaults kept)");
      nbxq::ctx_load(r, *c);
      CHECK(*static_cast<int *>(c->p[0]) == 5 && (*static_cast<uint64_t (*)[6]>(c->p[1]))[5] == 6 && static_cast<Padded *>(c->p[2])->v.size() == 3 &&
            static_cast<std::vector<std::string> *>(c->p[3])->at(1) == "yy" && (*static_cast<Big (*)[1024]>(c->p[4]))[7].serial == 9 && (*static_cast<Map::iterator *>(c->p[5]))->second == 4, "load: main -> worker");
      *static_cast<int *>(c->p[0]) = 77; (*static_cast<uint64_t (*)[6]>(c->p[1]))[0] = 100; static_cast<Padded *>(c->p[2])->v.push_back(4); static_cast<std::vector<std::string> *>(c->p[3])->clear();
      CHECK(a == 5 && arr[0] == 1 && p.v.size() == 3 && vs.size() == 2, "the worker instance is apart from the main one");
      nbxq::ctx_store(r, *c);
      CHECK(a == 77 && arr[0] == 100 && p.v.size() == 4 && vs.empty() && big[7].serial == 9 && it->second == 4, "store: worker -> main");
      nbxq::ctx_free(r, c); }
    // ---- the engine's slot list, the test's variables ----
    for (int k = 0; k < nbx::K_COUNT; k++) {
        bool ok;
        if (k == nbx::K_tex_gen) ok = nbx::reg(k, &tex_gen_m0, "tex_gen"); else if (k == nbx::K_cur_op) ok = nbx::reg(k, &cur_op_m0, "cur_op"); else if (k == nbx::K_draw_reads) ok = nbx::reg(k, &draw_reads_m0, "draw_reads");
        else if (k == nbx::K_img_front) ok = nbx::reg(k, &img_front_m0, "img_front"); else if (k == nbx::K_memo_front_it) ok = nbx::reg(k, &memo_front_it_m0, "memo_front_it");
        else if (k == nbx::K_state) ok = nbx::reg(k, &state_m0, "state"); else ok = nbx::reg(k, &filler[k], "filler");
        CHECK(ok, "slot %d", k);
    }
    CHECK(nbxq::complete(nbx::registry, nbx::K_COUNT), "the registry is complete");
    // ---- nbx::get ----
    { CHECK(&cur_op == &cur_op_m0 && &state == &state_m0 && &tex_gen == &tex_gen_m0, "no window: the main instance");
      put_env("DM_NB_MX", "1"); nbx::init(); CHECK(nbx::ensure_worker(), "worker starts");
      struct Q { unsigned *w = nullptr, *third = nullptr; bool par_seen = false; } q;
      nbx::par.store(true); unsigned *poster = &cur_op;
      nbx::worker.run([](void *a) { Q &q = *static_cast<Q *>(a); q.w = &cur_op; q.par_seen = nbx::par.load(); }, &q);
      std::thread t([&q] { q.third = &cur_op; }); t.join();
      nbx::par.store(false);
      CHECK(poster == &cur_op_m0 && q.third == &cur_op_m0 && q.w && q.w != &cur_op_m0 && q.par_seen, "window: own instance on the worker only");
      nbx::worker.run([](void *a) { static_cast<Q *>(a)->w = &cur_op; }, &q);
      CHECK(q.w == &cur_op_m0, "window closed: the worker reads the main instance too");
      nbx::stop(); }
    // ---- continuation ----
    for (int mt = 0; mt <= 3; mt += 1) {
        const Snap ref = run_mode(0, mt, subs); CHECK(g_other_thread == 0, "DM_NB_MX=0 stays on the caller's thread");
        const Snap got = run_mode(1, mt, subs);
        CHECK(same(ref, got), "DM_NB_MX=1 (DM_NB_MT=%d): variables and command log equal DM_NB_MX=0 (log %zu / %zu bytes, op %u / %u)", mt, got.log.size(), ref.log.size(), got.op, ref.op);
        CHECK(g_other_thread > int(subs / 2), "DM_NB_MX=1: the submissions ran on the worker (%d)", g_other_thread.load());
        printf("ok   continuation DM_NB_MT=%d: %u submissions, %zu log bytes, %d on the worker\n", mt, subs, got.log.size(), g_other_thread.load());
    }
    { bool line = false; nbx::mode = 1; nbx::st.subs = 3; nbx::report_ns = 1; nbx::tick(nbxq::now_ns() + 20000000000ull); for (const auto &t : g_lines) if (t.find("[nb] mx (DM_NB_MX=1") != std::string::npos && t.find("context moved both ways") != std::string::npos) line = true;
      CHECK(line, "the [nb] mx line"); nbx::mode = 0; }
    // ---- profile: classes (unit) ----
    { using namespace nbxq; struct T { int want; std::vector<int> ev; } cases[] = {
          {NBX_C_EMPTY, {}}, {NBX_C_OPS, {NBX_E_BARRIER, NBX_E_XFER}}, {NBX_C_OPS, {NBX_E_END}}, {NBX_C_CONT, {NBX_E_INPASS, NBX_E_DRAW, NBX_E_DRAW}},
          {NBX_C_CLEAN, {NBX_E_END, NBX_E_BEGIN, NBX_E_INPASS, NBX_E_DRAW}}, {NBX_C_CLEAN, {NBX_E_BEGIN, NBX_E_DRAW, NBX_E_END}}, {NBX_C_CLEAN, {NBX_E_NEUTRAL, NBX_E_BEGIN, NBX_E_DRAW, NBX_E_NEUTRAL}},
          {NBX_C_PRE_BAR, {NBX_E_END, NBX_E_BARRIER, NBX_E_BEGIN, NBX_E_DRAW}}, {NBX_C_PRE_XFER, {NBX_E_BARRIER, NBX_E_XFER, NBX_E_BARRIER, NBX_E_BEGIN, NBX_E_DRAW}},
          {NBX_C_TAIL, {NBX_E_BEGIN, NBX_E_DRAW, NBX_E_END, NBX_E_BARRIER}}, {NBX_C_TAIL, {NBX_E_BEGIN, NBX_E_DRAW, NBX_E_END, NBX_E_XFER}},
          {NBX_C_SPLIT, {NBX_E_BEGIN, NBX_E_DRAW, NBX_E_END, NBX_E_BARRIER, NBX_E_BEGIN, NBX_E_DRAW}}, {NBX_C_SPLIT, {NBX_E_BEGIN, NBX_E_BARRIER, NBX_E_DRAW}},
          {NBX_C_SPLIT, {NBX_E_BEGIN, NBX_E_DRAW, NBX_E_END, NBX_E_DRAW}}, {NBX_C_SPLIT, {NBX_E_BARRIER, NBX_E_DRAW}}};
      for (const T &t : cases) { Seg s; seg_reset(&s); for (int e : t.ev) seg_event(&s, e); CHECK(seg_class(&s) == t.want, "class %s, got %s (%zu events)", class_name(t.want), class_name(seg_class(&s)), t.ev.size()); }
      CHECK(seg_size(0) == NBX_SZ_1 && seg_size(7) == NBX_SZ_1 && seg_size(8) == NBX_SZ_8 && seg_size(31) == NBX_SZ_8 && seg_size(32) == NBX_SZ_32 && seg_size(127) == NBX_SZ_32 && seg_size(128) == NBX_SZ_MORE, "size buckets");
      const uint64_t q[3] = {5, 3, 2};
      CHECK(prof_model(q, 3, 4, 1) == 14 && prof_model(q, 3, 4, 2) == 9 && prof_model(q, 3, 4, 3) == 9 && prof_model(q, 0, 4, 4) == 4, "model: 14 / 9 / 9 / 4");
      const uint64_t q2[5] = {4, 4, 3, 3, 2}; CHECK(prof_model(q2, 5, 0, 2) == 9 && prof_model(q2, 5, 0, 4) == 5 && prof_model(q2, 5, 1, 16) == 5, "model: longest first"); }
    // ---- profile through the wrappers and nbx::mark ----
    { put_env("DM_NB_MX", "0"); put_env("DM_NB_MT", "1"); put_env("DM_NB_MX_PROF", "1"); put_env("DM_NB_MX_MIN", "2"); nbmt::init(); nbx::init(); nbx::prof_init();
      Log cb; VkCommandBuffer c = reinterpret_cast<VkCommandBuffer>(&cb); VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; VkBufferCopy bc{0, 0, 4};
      nbmt::begin(c, true, false);
      vkCmdDraw(c, 1, 1, 0, 0);   // outside a job: not profiled
      nbx::mark(0, -1, 100);
      vkCmdPipelineBarrier(c, 1, 1, 0, 1, &mb, 0, nullptr, 0, nullptr);                                                  // before the first pass
      nbx::mark(1, 3, 100); vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); vkCmdDraw(c, 3, 1, 0, 0); vkCmdDraw(c, 3, 1, 0, 0); vkCmdDraw(c, 3, 1, 0, 0);   // clean, 3 draws, quiet
      nbx::mark(1, 4, 100); vkCmdEndRenderPass(c); vkCmdPipelineBarrier(c, 1, 1, 0, 1, &mb, 0, nullptr, 0, nullptr); vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); vkCmdDraw(c, 3, 1, 0, 0); vkCmdDraw(c, 3, 1, 0, 0);   // pre-barrier, noisy
      nbx::mark(1, 5, 101); vkCmdDraw(c, 3, 1, 0, 0);                                                                     // cont
      nbx::mark(1, 6, 101); vkCmdEndRenderPass(c); vkCmdCopyBuffer(c, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &bc); vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); vkCmdDraw(c, 3, 1, 0, 0);   // pre-xfer
      nbx::mark(1, 7, 101); vkCmdEndRenderPass(c); vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); vkCmdDraw(c, 3, 1, 0, 0); vkCmdEndRenderPass(c); vkCmdCopyBuffer(c, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &bc);   // tail
      nbx::mark(1, 8, 101);                                                                                               // empty
      nbx::mark(2, -1, 101);
      const nbxq::Prof &p = nbx::pf; using namespace nbxq;
      CHECK(p.parts == 1 && p.segs == 6, "one part, six segments (%llu / %llu)", (unsigned long long)p.parts, (unsigned long long)p.segs);
      CHECK(p.n[NBX_C_CLEAN][0] == 1 && p.draws[NBX_C_CLEAN][0] == 3 && p.n[NBX_C_PRE_BAR][0] == 1 && p.n[NBX_C_CONT][0] == 1 && p.n[NBX_C_PRE_XFER][0] == 1 && p.n[NBX_C_TAIL][0] == 1 && p.n[NBX_C_EMPTY][0] == 1, "classes through the wrappers");
      CHECK(p.quiet_n[0] == 1 && p.quiet_draws[0] == 3, "quiet: the clean one only (the pre-barrier one moved the noise sum)");
      CHECK(nbx::nquiet == 1 && nbx::model_parts == 1 && p.ns_prefix > 0, "model input: one quiet segment of >= 2 draws, the prefix timed");
      nbx::prof_ns = 1; nbx::prof_report(nbxq::now_ns() + 20000000000ull);
      bool line = false; for (const auto &t : g_lines) if (t.find("[nb] mx prof") != std::string::npos && t.find("clean 1.0 / 3 draws") != std::string::npos && t.find("T=2") != std::string::npos) line = true;
      CHECK(line, "the [nb] mx prof line");
      nbmt::end(c); nbmt::stop(); nbmt::on_packet = nullptr; }
    printf("%s: nb_mx_test (%zu log lines)\n", fails ? "FAILED" : "all passed", g_lines.size());
    return fails ? 1 : 0;
}
