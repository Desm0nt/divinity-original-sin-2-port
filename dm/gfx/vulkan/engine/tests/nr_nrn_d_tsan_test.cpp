// [DOS2] NR-NATIVE step D (docs/dos2/nr/NR-NATIVE-D.md): the note of list construction under threads. The engine builds its lists on
// worker threads - one task a list - while other tasks build other lists and textures are made and destroyed; the lists are executed
// later on ONE thread, after the engine waited for the builders' job group (the join below). What must hold:
//   - a builder writes only into its own list (the note packet included) and its thread-locals: no data race with another builder,
//     with the texture events (the epoch is an atomic) or with the executing thread;
//   - a builder that is inside one bracket and runs another builder's task (the engine's wait runs tasks) restores its bracket;
//   - the executing thread finds every note filled, and the card the memo answers is the card of the cache.
// Build (WSL): g++ -std=c++17 -O1 -g -fsanitize=thread nr_nrn_d_tsan_test.cpp -o nr_nrn_d_tsan_test -lpthread   (also without the sanitizer)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include "../../../nr/nr_nrn_d.h"

static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 12) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
struct Chunk { std::vector<uint8_t> raw; uint8_t *data; uint32_t cap, used = 0, count = 0; };
struct List { Chunk c; int pass; uint32_t cflags, stage; std::vector<const uint8_t *> draws; };
static thread_local struct { uint8_t kind, sub; } t_build;
static std::atomic<uint64_t> g_epoch{0};
static std::atomic<bool> g_stop{false};
static uint8_t *alloc(List &l, uint32_t size, uint32_t align) {
    Chunk &c = l.c; uint32_t pad = ((align + c.used - 1) & ~(align - 1)) - c.used; if (!pad) pad = align;
    if (c.used + pad + size > c.cap) return nullptr;
    c.data[c.used] = uint8_t(align); uint8_t *p = c.data + c.used + pad; c.used += pad + size; c.count++; return p;
}
static void build(List &l, unsigned seed, int depth);
// the wrapper (hle_dos2_nrn_d.inc nrnd_begin / nrnd_end) around a draw-yourself function
static void draw_yourself(List &l, unsigned m) {
    uint8_t *np = alloc(l, NRND_NOTE_BYTES, NRND_NOTE_ALIGN); if (!np) return;
    nrnd_note n; nrnd_note_init(&n, NRND_K_COMMON, l.cflags, 0x10, t_build.kind == 1 ? uint32_t(NRND_VT_CASCADE) : t_build.kind ? uint32_t(NRND_VT_GBUF_STAGE) : 0u, t_build.sub, m);
    memcpy(np, &n, sizeof n);
    const uint32_t at0 = l.c.used;
    uint8_t *p = alloc(l, 0x70, 8); if (!p) return;
    memset(p, 0, 0x70); p[0] = NRND_PK_STATE; uint64_t sh = 0x100000000ull + m % 7, vb = 0x600000000ull + m % 11; memcpy(p + 8, &sh, 8); memcpy(p + 0x38, &vb, 8);
    if (m % 3) { p = alloc(l, 0x20, 8); if (!p) return; memset(p, 0, 0x20); p[0] = NRND_PK_TEX; uint64_t d = m % 5, r = m % 13; memcpy(p + 8, &d, 8); memcpy(p + 0x10, &r, 8); }
    p = alloc(l, 0x14, 4); if (!p) return; memset(p, 0, 0x14); p[0] = NRND_PK_DRAW; l.draws.push_back(p);
    nrnd_seg s = {l.c.data, at0, l.c.used};
    memcpy(&n, np, sizeof n); nrnd_scan(&n, &s, 1); memcpy(np, &n, sizeof n);
}
static std::vector<List> *g_lists;
static void build(List &l, unsigned seed, int depth) {
    const uint8_t k0 = t_build.kind, s0 = t_build.sub;
    t_build.kind = l.pass == NRN_P_SHADOW ? 1 : l.pass == NRN_P_PREPASS ? 2 : 3; t_build.sub = uint8_t(seed & 3);
    for (unsigned i = 0; i < 400; i++) {
        draw_yourself(l, seed * 31 + i * 7);
        if (depth == 0 && i == 200 && (seed & 1)) {   // the wait of this builder runs another builder's task on this thread
            List &o = (*g_lists)[8 + seed / 2]; build(o, seed + 100, 1); }   // (one task a list: thread t owns list t and, when odd, list 8 + t / 2)
    }
    t_build.kind = k0; t_build.sub = s0;
}

int main() {
    std::vector<List> lists(12);
    g_lists = &lists;
    for (unsigned i = 0; i < lists.size(); i++) { List &l = lists[i]; l.c.cap = 1 << 18; l.c.raw.assign(l.c.cap + 0x40, uint8_t(0xa5 + i)); l.c.data = l.c.raw.data() + (0x40 - (uintptr_t(l.c.raw.data()) & 0x3f));
        l.pass = i % 3 == 0 ? NRN_P_SHADOW : i % 3 == 1 ? NRN_P_PREPASS : NRN_P_GBUF; l.cflags = l.pass == NRN_P_SHADOW ? 2 : l.pass == NRN_P_PREPASS ? 0x40000 : 1;
        l.draws.reserve(1024); }
    std::thread ev([] { while (!g_stop.load(std::memory_order_relaxed)) { g_epoch.fetch_add(1, std::memory_order_relaxed); std::this_thread::yield(); } });
    std::vector<std::thread> th;
    std::vector<char> ok(8, 0);
    for (unsigned t = 0; t < 8; t++) th.emplace_back([&lists, &ok, t] { const bool a = t_build.kind == 0; build(lists[t], t, 0); ok[t] = a && t_build.kind == 0 && t_build.sub == 0; });
    for (auto &x : th) x.join();   // the engine's wait for the job group
    for (unsigned t = 0; t < 8; t++) CHECK(ok[t], "builder %u did not put its bracket back", t);
    g_stop = true; ev.join();
    // execution on one thread
    static nrnd_memo memo; nrnd_run run{}; uint8_t R[0x1900]; memset(R, 0, sizeof R);
    unsigned long long draws = 0, hits = 0, named = 0;
    const uint64_t epoch = g_epoch.load();
    for (int rep = 0; rep < 3; rep++) for (List &l : lists) {
        const uint8_t *p = l.c.data; unsigned di = 0;
        for (uint32_t k = 0; k < l.c.count; k++) {
            const uintptr_t a = *p, r = uintptr_t(p) % a; p += r ? a - r : a;
            const unsigned op = p[0];
            if (op == NRND_OP) { nrnd_note n; memcpy(&n, p, sizeof n); CHECK(n.flags & NRND_F_DONE, "a note the builder did not fill");
                nrnd_run_note(&run, uint64_t(uintptr_t(&l)), reinterpret_cast<const nrnd_note *>(p)); p += NRND_NOTE_BYTES; continue; }
            nrnd_run_pkt(&run, op, uint64_t(uintptr_t(p)));
            if (op == NRND_PK_STATE) { memcpy(R + NRN_R_SHADER, p + 8, 8); memcpy(R + NRN_R_VB, p + 0x38, 8); p += 0x70; }
            else if (op == NRND_PK_TEX) { uint64_t d, r2; memcpy(&d, p + 8, 8); memcpy(&r2, p + 0x10, 8); const uint32_t id = uint32_t(r2 * 17 + 1); memcpy(R + NRN_R_TEX_FS + 4 * (d % 32), &id, 4); p += 0x20; }
            else { CHECK(op == NRND_PK_DRAW && di < l.draws.size() && l.draws[di] == p, "the draw packets"); di++;
                const nrnd_note *n = nrnd_run_take(&run); CHECK(n != nullptr, "a draw without its note"); draws++;
                if (n) { const int pn = nrnd_pass_of(n->cflags, n->stage); if (pn >= 0) { named++; CHECK(pn == l.pass, "pass %d by the note of a list of pass %d", pn, l.pass); } }
                const uint64_t card = nrn_card_of(R, NRN_W_DEFAULT), key = nrnd_chain_key(&run, n, epoch);
                if (key) { const uint64_t pred = nrnd_memo_get(&memo, key); if (pred) { hits++; CHECK(pred == card, "memo %016llx, cache %016llx", (unsigned long long)pred, (unsigned long long)card); } nrnd_memo_put(&memo, key, card); }
                nrnd_run_drawn(&run, n, card); p += 0x14; }
        }
    }
    CHECK(draws > 10000 && hits * 2 > draws && named == draws, "draws %llu, memo hits %llu, passes named %llu", draws, hits, named);
    printf("%s nr_nrn_d_tsan_test: %llu checks, %llu failed; %llu draws, memo answered %llu, epoch events %llu\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail, draws, hits, (unsigned long long)epoch);
    return g_fail ? 1 : 0;
}
