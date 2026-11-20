// [DOS2] NR-HEAT engine side (xbg_nr_heat.inc) against stand-ins of the engine globals it reads: sampling cadence, the per-pass
// program bookkeeping (draw / close), the hand-over, the writer thread (location changes, blocks, S lines with the device's static cost,
// P lines of DM_NR_HEAT=2), the file. Linux / WSL (the writer uses pthread names and setpriority):
//   g++ -std=c++17 -O1 -g -Wall -Wextra -pthread -I<vulkan headers> nr_heat_writer_test.cpp ../../../nr/nr_heat.c -o nr_heat_writer_test
// The produced file is left in $TMPDIR/nr_heat_writer_test/shader-heat.tsv (tools/dos2_shader_heat.py reads it).
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static std::mutex g_logmu; static std::vector<std::string> g_log;
void xbg_logf(const char *f, ...) { char t[4096]; va_list a; va_start(a, f); vsnprintf(t, sizeof t, f, a); va_end(a); std::lock_guard<std::mutex> l(g_logmu); g_log.push_back(t); }

// ---- stand-ins of what the .inc reads from xbg_game.cpp ----
struct Shader { std::string key; };
struct Slot {
    struct PassRec {
        uint32_t q0 = 0, q1 = 0, w = 0, h = 0, att = 0, fmt = 0, draws0 = 0, draws1 = 0, x = 0, y = 0, z = 0; uint64_t verts0 = 0, verts1 = 0, rep0 = 0, rep1 = 0;
        bool compute = false, complete = false, mixed = false, dz = false; std::string first, last;
        struct HeatProg { const void *vs, *fs; uint32_t draws; uint64_t verts; };
        std::vector<HeatProg> heat; uint32_t hcur = UINT32_MAX; uint64_t hd0 = 0, hv0 = 0;
    };
    uint32_t prof_used = 0, prof_skipped = 0; uint64_t prof_frame = 0;
};
struct Cfg { const char *shader_directory = "/nonexistent/shaders"; } cfg;
VkPhysicalDeviceProperties properties{};
uint32_t timestamp_bits = 64; bool pass_prof_enabled = false;
uint64_t pass_prof_draws = 0, pass_prof_verts = 0;
uint64_t timing_clock() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

static std::mutex g_lvmu; static std::string g_level = "-", g_prev = "-"; static int g_gen = 0;
extern "C" int dm_dos2_heat_level(char *cur, size_t n, char *prev, size_t pn, double *score_kb) {
    std::lock_guard<std::mutex> l(g_lvmu); snprintf(cur, n, "%s", g_level.c_str()); snprintf(prev, pn, "%s", g_prev.c_str()); if (score_kb) *score_kb = 1234; return g_gen;
}
static void set_level(const char *s) { std::lock_guard<std::mutex> l(g_lvmu); g_prev = g_level; g_level = s; g_gen++; }

#include "../xbg_nr_heat.inc"

// a small module: one texture sample, one transcendental (cost 4 + 4 + alu)
static std::vector<uint32_t> module(int samples) {
    std::vector<uint32_t> w = {0x07230203, 0x10000, 0, 100, 0};
    auto ins = [&](uint32_t op, std::initializer_list<uint32_t> a) { w.push_back(uint32_t((a.size() + 1) << 16 | op)); w.insert(w.end(), a); };
    w.push_back(6u << 16 | 11); w.push_back(20); { uint32_t imp[4] = {0}; memcpy(imp, "GLSL.std.450", 12); w.insert(w.end(), imp, imp + 4); }
    ins(54, {1, 2, 0, 3}); ins(248, {4});
    for (int i = 0; i < samples; i++) ins(87, {11, uint32_t(50 + i), 90, 74});
    ins(129, {10, 75, 72, 73}); ins(12, {10, 76, 20, 31, 75});
    w.push_back(1u << 16 | 253); w.push_back(1u << 16 | 56);
    return w;
}

static Shader vs1{"1111111111111111"}, fs1{"aaaaaaaaaaaaaaaa"}, fs2{"bbbbbbbbbbbbbbbb"}, fused{"F1A.cccccccccccccccc"}, cs{"dddddddddddddddd"};

// one profiled submission: pass 0 = 3 draws fs1 + 1 draw fs2 (2400x1080), pass 1 = the fused post (1 draw), a dispatch
static void submit(uint64_t frame) {
    std::vector<Slot::PassRec> recs(3);
    auto drawn = [](Slot::PassRec &r, const Shader *v, const Shader *f, uint64_t verts) { nrheat::draw(r, v, f); pass_prof_draws++; pass_prof_verts += verts; };
    Slot::PassRec &a = recs[0]; a.w = 2400; a.h = 1080; a.fmt = 37; a.att = 2; a.draws0 = uint32_t(pass_prof_draws); a.verts0 = pass_prof_verts;
    drawn(a, &vs1, &fs1, 300); drawn(a, &vs1, &fs1, 300); drawn(a, &vs1, &fs2, 600); drawn(a, &vs1, &fs1, 300);
    nrheat::close(a); a.draws1 = uint32_t(pass_prof_draws); a.verts1 = pass_prof_verts;
    Slot::PassRec &b = recs[1]; b.w = 2160; b.h = 972; b.fmt = 37; b.att = 1; b.draws0 = uint32_t(pass_prof_draws); b.verts0 = pass_prof_verts;
    drawn(b, &vs1, &fused, 3); nrheat::close(b); b.draws1 = uint32_t(pass_prof_draws); b.verts1 = pass_prof_verts;
    Slot::PassRec &c = recs[2]; c.compute = true; c.first = cs.key;
    Slot s; s.prof_frame = frame; s.prof_used = 6;
    nrheat::begin_slot();
    nrheat::pass(a, 800.0); nrheat::pass(b, 200.0); nrheat::pass(c, 50.0);
    nrheat::end_slot(s, 1100.0);
}

static std::vector<std::vector<std::string>> read_rows(const std::string &path) {
    std::vector<std::vector<std::string>> out; FILE *f = fopen(path.c_str(), "rb"); if (!f) return out;
    char line[16384];
    while (fgets(line, sizeof line, f)) {
        std::string l(line); while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        std::vector<std::string> v; size_t p = 0; for (;;) { size_t q = l.find('\t', p); v.push_back(l.substr(p, q == std::string::npos ? q : q - p)); if (q == std::string::npos) break; p = q + 1; }
        out.push_back(v);
    }
    fclose(f); return out;
}

int main() {
    const char *tmp = getenv("TMPDIR"); std::string dir = std::string(tmp ? tmp : "/tmp") + "/nr_heat_writer_test";
    mkdir(dir.c_str(), 0775); std::string path = dir + "/shader-heat.tsv"; remove(path.c_str()); remove((dir + "/shader-heat.1.tsv").c_str());
    snprintf(properties.deviceName, sizeof properties.deviceName, "Test\tGPU"); properties.vendorID = 0x5143; properties.limits.timestampPeriod = 1;
    // off: nothing
    unsetenv("DM_NR_HEAT"); nrheat::init(); CHECK(nrheat::mode == 0 && nrheat::w == nullptr);
    setenv("DM_NR_HEAT", "7", 1); nrheat::init(); CHECK(nrheat::mode == 0);
    // mode 2 bursts of 3 frames every second, from the start
    setenv("DM_NR_HEAT", "2", 1); setenv("DM_NR_HEAT_DIR", dir.c_str(), 1); setenv("DM_NR_HEAT_S", "1", 1); setenv("DM_NR_HEAT_FIRST_S", "0", 1);
    setenv("DM_NR_HEAT_BURST", "3", 1); setenv("DM_NR_HEAT_FLUSH_S", "10", 1);
    nrheat::init();
    CHECK(nrheat::mode == 2 && nrheat::w && nrheat::dir == dir && nrheat::period_s == 1 && nrheat::burst == 3);
    nrheat::note_shader(vs1.key, 0, module(0), 'x'); nrheat::note_shader(fs1.key, 4, module(2), 'x'); nrheat::note_shader(fs2.key, 4, module(0), 'n');
    nrheat::note_shader(fused.key, 4, module(1), 'f');   // cs: never noted (unknown cost)
    // draw / close bookkeeping
    { Slot::PassRec r; pass_prof_draws = 100; pass_prof_verts = 1000;
      nrheat::draw(r, &vs1, &fs1); pass_prof_draws++; pass_prof_verts += 30;
      nrheat::draw(r, &vs1, &fs1); pass_prof_draws++; pass_prof_verts += 30;
      nrheat::draw(r, &vs1, &fs2); pass_prof_draws++; pass_prof_verts += 60;
      nrheat::draw(r, &vs1, &fs1); pass_prof_draws++; pass_prof_verts += 30; nrheat::close(r);
      CHECK(r.heat.size() == 2 && r.heat[0].draws == 3 && r.heat[0].verts == 90 && r.heat[1].draws == 1 && r.heat[1].verts == 60); }
    set_level("TUT_Tutorial_A");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // the writer sees the location (no block yet: nothing sampled)
    uint64_t frame = 1; int sampled = 0;
    const auto run = [&](double secs) { const uint64_t end = timing_clock() + uint64_t(secs * 1e9);
        while (timing_clock() < end) { if (nrheat::due(frame)) { submit(frame); sampled++; } frame++; std::this_thread::sleep_for(std::chrono::milliseconds(10)); } };
    run(2.2);   // bursts at 0 s, 1 s, 2 s: 3 frames each
    CHECK(sampled >= 6 && sampled <= 9 && sampled % 3 == 0);
    const int first_level = sampled;
    set_level("FJ_FortJoy_Main");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // the writer: the location -> a TUT block
    run(1.1);
    set_level("RC_Main");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    auto rows = read_rows(path);
    CHECK(!rows.empty() && rows[0][0] == "H" && rows[0].size() >= 10);
    if (!rows.empty()) { bool gpu = false; for (auto &f : rows[0]) if (f == "gpu=Test GPU") gpu = true; CHECK(gpu); }   // the tab in the name cleaned
    int L = 0, F = 0, P = 0, S = 0, B = 0, A = 0; std::map<std::string, std::vector<std::string>> s_of; std::vector<std::string> blocks;
    double a_us_d = 0, a_us_c = 0, b_pass = 0, b_comp = 0; std::map<std::string, double> uc_tut, ud_tut; std::string cur;
    for (auto &r : rows) {
        const std::string &t = r[0];
        if (t == "L") L++; else if (t == "F") { F++; CHECK(r.size() == 14 && r[5] == "1" && r[6] == "2" && r[7] == "1" && r[8] == "5" && r[9] == "1100" && r[10] == "1000" && r[11] == "50"); }
        else if (t == "P") { P++; CHECK(r.size() == 11); }
        else if (t == "S") { S++; s_of[r[1]] = r; CHECK(r.size() == 15); }
        else if (t == "B") { B++; blocks.push_back(r[3]); cur = r[3]; CHECK(r.size() == 10); if (cur == "TUT_Tutorial_A") { b_pass += atof(r[8].c_str()); b_comp += atof(r[9].c_str()); CHECK(atoi(r[4].c_str()) == first_level); } }
        else if (t == "A") { A++; CHECK(r.size() == 15); if (cur == "TUT_Tutorial_A") { a_us_d += atof(r[11].c_str()); a_us_c += atof(r[14].c_str()); uc_tut[r[6]] += atof(r[14].c_str()); ud_tut[r[6]] += atof(r[11].c_str()); } }
    }
    CHECK(L == 3);                      // TUT, FJ, RC
    CHECK(F == sampled);                // one F line a sampled frame
    CHECK(P == sampled * 3);            // mode 2: every pass
    CHECK(B == 2 && blocks.size() == 2 && blocks[0] == "TUT_Tutorial_A" && blocks[1] == "FJ_FortJoy_Main");
    CHECK(A == 2 * 4);                  // per block: (vs1, fs1), (vs1, fs2), (vs1, fused), the dispatch
    CHECK(fabs(a_us_d - first_level * 1050.0) < 1e-6 * first_level * 1050 + 2 && fabs(a_us_c - a_us_d) < 4);   // every us of the passes attributed once
    CHECK(fabs(b_pass - first_level * 1000.0) < 1 && fabs(b_comp - first_level * 50.0) < 1);
    // inside pass 0: draw share fs1 3/4, fs2 1/4 of 800 us; cost-weighted: fs1 (2 samples) costs more than fs2 (none)
    CHECK(fabs(ud_tut["aaaaaaaaaaaaaaaa"] - first_level * 600.0) < 2 && fabs(ud_tut["bbbbbbbbbbbbbbbb"] - first_level * 200.0) < 2);
    CHECK(uc_tut["aaaaaaaaaaaaaaaa"] > ud_tut["aaaaaaaaaaaaaaaa"] && uc_tut["bbbbbbbbbbbbbbbb"] < ud_tut["bbbbbbbbbbbbbbbb"]);
    CHECK(fabs(uc_tut["dddddddddddddddd"] - first_level * 50.0) < 2);   // the dispatch keeps its own time (unknown cost)
    // S lines: every noted key of the rows once a file, the device's counts, the source flag; the fused key by its name
    CHECK(S == 4 && s_of.count("1111111111111111") && s_of.count("aaaaaaaaaaaaaaaa") && s_of.count("F1A.cccccccccccccccc") && !s_of.count("dddddddddddddddd"));
    if (s_of.count("aaaaaaaaaaaaaaaa")) { auto &r = s_of["aaaaaaaaaaaaaaaa"]; CHECK(r[2] == "4" && r[4] == "x" && r[8] == "2" && r[7] == "1"); }   // tex 2, trans 1
    if (s_of.count("bbbbbbbbbbbbbbbb")) CHECK(s_of["bbbbbbbbbbbbbbbb"][4] == "nr");
    if (s_of.count("F1A.cccccccccccccccc")) CHECK(s_of["F1A.cccccccccccccccc"][4] == "f");
    // the dispatch's key was never noted (loaded before init, say): no S line, the tool falls back to the pack
    bool started = false; for (auto &l : g_log) if (l.find("[nr] heat DM_NR_HEAT=2") == 0) started = true;
    CHECK(started);
    printf("%s\n", path.c_str());
    if (g_fail) { std::lock_guard<std::mutex> l(g_logmu); for (auto &x : g_log) printf("log: %s\n", x.c_str()); }
    printf("nr_heat_writer_test: %d checks, %d failed (%d frames sampled) - %s\n", g_checks, g_fail, sampled, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
