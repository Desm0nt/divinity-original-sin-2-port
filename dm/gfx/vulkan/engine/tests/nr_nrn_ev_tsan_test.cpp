// [DOS2] NR-NATIVE step E (DM_NRN_EV, docs/dos2/nr/NR-NATIVE-E.md): the event table of dm/gfx/nr/nr_nrn_ev.h under threads - the engine's
// threads write (creation / destroy under the host's one mutex; updates and CPU copies with no lock), the translation thread journals
// its own writes and asks ranges, the game thread asks handles. What must hold while everything runs:
//   - a range in memory no buffer ever covered has no epoch;
//   - an answered range was answered from ONE consistent entry (the entry's handle is a function of its bounds here, so an entry read
//     half before and half after a writer moved the array shows) that contains the range - a reader that meets a writer gets no answer;
//   - the journal's version of a range never goes back for one reader; a permanent buffer's handle epoch never goes back and is never 0;
//   - an update of a permanent buffer that returned before a reader's ask began is in that ask's answer.
// Build (WSL): g++ -std=c++17 -O1 -g -fsanitize=thread nr_nrn_ev_tsan_test.cpp -o nr_nrn_ev_tsan_test -lpthread   (also without the sanitizer)
// Mutations -DNRNE_MUTATE=4 (a slot may shrink) and 5 (a reader is answered from a half-moved array) fail it.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "../../../nr/nr_nrn_ev.h"

static nrn_ev *g_v;
static std::mutex g_mu;
static std::atomic<bool> g_stop{false};
static std::atomic<unsigned long long> g_checks{0}, g_fail{0};
#define CHECK(c) do { g_checks.fetch_add(1, std::memory_order_relaxed); if (!(c)) { if (g_fail.fetch_add(1) < 20) printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
enum { NS = 64, NC = 512 };
static const uint64_t S_BASE = 0x10000000ull, C_BASE = 0x40000000ull, N_BASE = 0x70000000ull;   // permanent buffers; churn; never covered
static inline uint64_t s_lo(int i) { return S_BASE + (uint64_t)i * 0x3000 + 0x40; }
static inline uint64_t s_size(int i) { return 0x800 + (uint64_t)(i % 5) * 0x700; }
static inline uint64_t s_handle(int i) { return (uint64_t)(i + 1) << 32 | (uint32_t)i; }
static inline uint64_t c_handle(int idx, uint64_t lo, uint64_t hi) { return (uint64_t)((((lo >> 4) ^ (hi >> 4) * 31u) & 0x3ffffeu) | 1u) << 32 | (uint32_t)(1000 + idx); }
static std::atomic<uint64_t> g_done[NS];   // the serial of the newest update of permanent buffer i that has RETURNED

static void engine(unsigned seed) {
    uint64_t r = seed * 0x9E3779B97F4A7C15ull + 1;
    auto rnd = [&r]() { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return (uint32_t)(r >> 20); };
    struct C { uint64_t lo = 0, hi = 0; } own[NC / 2];
    const int base = (seed & 1) ? NC / 2 : 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const uint32_t op = rnd() % 10;
        if (op < 4) {                               // churn: create / destroy over ranges that overlap each other's old places
            const int k = (int)(rnd() % (NC / 2));
            std::lock_guard<std::mutex> l(g_mu);
            if (own[k].lo) { nrn_ev_destroyed(g_v, NRNE_K_IB, c_handle(base + k, own[k].lo, own[k].hi)); own[k].lo = 0; }
            else {
                // each thread keeps to its own half of the churn region (the engine hands a range to one owner at a time)
                const uint64_t lo = C_BASE + (uint64_t)(seed & 1) * 0x8000000ull + (uint64_t)k * 0x4000 + (rnd() % 16) * 0x100, hi = lo + 0x200 + (rnd() % 32) * 0x100;
                nrn_ev_created(g_v, NRNE_K_IB, NRNE_C_DYNAMIC, c_handle(base + k, lo, hi), lo, hi - lo);
                own[k].lo = lo; own[k].hi = hi;
            }
        } else if (op < 8) {                        // a CPU copy into a permanent buffer: no lock
            const int i = (int)(rnd() % NS);
            nrn_ev_updated(g_v, NRNE_K_VB, s_handle(i), 0, 0, 1);
            const uint64_t s = nrn_ev_updated(g_v, NRNE_K_VB, s_handle(i), 0, 0, 1);
            CHECK(s != 0);
            uint64_t c = g_done[i].load();
            while (c < s && !g_done[i].compare_exchange_weak(c, s)) {}
        } else {                                    // a GPU copy announced
            const int i = (int)(rnd() % NS);
            CHECK(nrn_ev_updated(g_v, NRNE_K_VB, s_handle(i), 0x10, 0x40, 0) != 0);
        }
    }
}
static void translator() {
    uint64_t r = 0x1234567;
    auto rnd = [&r]() { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return (uint32_t)(r >> 20); };
    uint64_t last[NS] = {}; unsigned long long answered = 0, asked = 0, churn_ans = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const uint32_t op = rnd() % 8;
        if (op == 0) nrn_ev_wrote(g_v, S_BASE + (rnd() % (NS * 0x3000)), 1 + rnd() % 0x300);      // its own writes into guest memory
        else if (op < 4) {                          // a permanent buffer
            const int i = (int)(rnd() % NS);
            const uint64_t done = g_done[i].load();
            const uint64_t jv = nrne_j_ver(&g_v->j, s_lo(i), s_size(i));
            CHECK(jv >= last[i]); last[i] = jv;      // never back
            CHECK(jv >= done);                       // an update that returned before the ask is in it
            int kind = -1, cls = -1; asked++;
            if (nrn_ev_range_epoch(g_v, s_lo(i) + 0x10, s_size(i) - 0x20, &kind, &cls)) { answered++; CHECK(kind == NRNE_K_VB && cls == NRNE_C_STATIC); }
        } else if (op < 6) {                        // the churn region: an answer comes from one consistent entry
            const uint64_t a = C_BASE + (uint64_t)(rnd() & 1) * 0x8000000ull + (uint64_t)(rnd() % (NC / 2)) * 0x4000 + (rnd() % 0x2000), n = 1 + rnd() % 0x400;
            nrne_ent e;
            if (nrne_cov_find(&g_v->cov, a, n, &e)) {
                churn_ans++;
                CHECK(e.lo <= a && a + n <= e.hi);
                const uint64_t h = nrne_meta_handle(e.meta);
                CHECK(h == (c_handle((int)(uint32_t)h - 1000, e.lo, e.hi) & 0x003fffffffffffffull));
                CHECK(nrne_meta_kind(e.meta) == NRNE_K_IB);
            }
        } else {                                    // never covered
            CHECK(nrn_ev_range_epoch(g_v, N_BASE + rnd() % 0x100000, 1 + rnd() % 0x1000, nullptr, nullptr) == 0);
        }
    }
    CHECK(answered > asked / 20);                   // the readers are answered most of the time (a writer is inside only briefly)
    printf("translation thread: %llu asks of a permanent buffer, %llu answered; %llu answers in the churn region\n", asked, answered, churn_ans);
}
static void game() {
    uint64_t r = 0x7654321, last[NS] = {};
    auto rnd = [&r]() { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return (uint32_t)(r >> 20); };
    while (!g_stop.load(std::memory_order_relaxed)) {
        const int i = (int)(rnd() % NS);
        const uint64_t done = g_done[i].load();
        const uint64_t ep = nrn_ev_buf_epoch(g_v, NRNE_K_VB, s_handle(i));
        CHECK(ep != 0 && ep >= last[i] && ep >= done);
        last[i] = ep;
        CHECK(nrn_ev_buf_epoch(g_v, NRNE_K_VB, (uint64_t)(i + 77) << 32 | (uint32_t)i) == 0);   // another generation of the index
    }
}
int main(int argc, char **argv) {
    const int ms = argc > 1 ? atoi(argv[1]) : 3000;
    g_v = (nrn_ev *)calloc(1, sizeof *g_v);
    if (!g_v) return 2;
    for (int i = 0; i < NS; i++) { const uint64_t s = nrn_ev_created(g_v, NRNE_K_VB, NRNE_C_STATIC, s_handle(i), s_lo(i), s_size(i)); g_done[i].store(s); }
    std::vector<std::thread> t;
    t.emplace_back(engine, 1u); t.emplace_back(engine, 2u); t.emplace_back(translator); t.emplace_back(game);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    g_stop.store(true);
    for (auto &x : t) x.join();
    const unsigned long long c = g_checks.load(), f = g_fail.load();
    printf("%s: %llu checks, %llu failed; %llu events, coverage %llu entries (added %llu, removed %llu, pushed out %llu)\n", f ? "FAIL" : "PASS", c, f,
           (unsigned long long)g_v->j.serial, (unsigned long long)g_v->cov.n, (unsigned long long)g_v->cov.adds, (unsigned long long)g_v->cov.dels, (unsigned long long)g_v->cov.evicted);
    return f ? 1 : 0;
}
