// [DOS2] NR-NATIVE step D half c (DM_NRN_PKT, docs/dos2/nr/NR-NATIVE-D.md section 13): the merge of a group into its note under threads.
// The engine builds its lists on worker threads, one task a list; the merge (hle_dos2_nrn_c.inc nrnk_built) lowers the packet count of
// the note's chunk and sets a flag in the note - both memory of the builder's own list - and adds to shared counters (atomics). The
// lists are executed later on one thread, after the builders were joined. What must hold: no data race between builders (a builder
// that runs another builder's task included), and the executing thread walks every list to the end of every chunk and executes
// every packet the builders appended, once, in order.
// Build (WSL): g++ -std=c++17 -O1 -g -fsanitize=thread nr_pkt_tsan_test.cpp -o nr_pkt_tsan_test -lpthread   (also without the sanitizer)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include "../../../nr/nr_pkt.h"

static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 12) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
struct Chunk { std::vector<uint8_t> raw; uint8_t *data; uint32_t cap, used = 0, count = 0; };
struct List { std::vector<Chunk> ch; size_t cur = 0; std::vector<uint64_t> sums; uint64_t merged = 0; };
static std::atomic<uint64_t> g_merged{0}, g_yard{0};
static Chunk make_chunk() { Chunk c; c.cap = 1 << 15; c.raw.assign(c.cap + 0x40, 0x5a); c.data = c.raw.data() + (0x40 - (uintptr_t(c.raw.data()) & 0x3f)); return c; }
static uint8_t *alloc(List &l, uint32_t size, uint32_t align) {
    for (;;) {
        Chunk &c = l.ch[l.cur]; const uint32_t r = c.used % align, p = c.used + (r ? align - r : align);
        if (p + size <= c.cap) { c.data[c.used] = uint8_t(align); c.used = p + size; c.count++; return c.data + p; }
        l.cur++;   // the chunks are made ahead (the vector must not move under a pointer)
    }
}
static uint64_t sum_of(const uint8_t *p, uint32_t len) { uint64_t s = 0; for (uint32_t i = 0; i < len; i++) s = s * 131 + p[i]; return s; }
static void put(List &l, unsigned op, uint32_t n28, unsigned m) {
    const uint32_t len = op == NRND_PK_PARAM ? 0x30 + n28 : k_nrnd_len[op];
    uint8_t *p = alloc(l, len, 1u << (m % 5)); for (uint32_t i = 0; i < len; i++) p[i] = uint8_t(m * 7 + i);
    p[0] = uint8_t(op); if (op == NRND_PK_PARAM) memcpy(p + 0x28, &n28, 4);
    l.sums.push_back(sum_of(p, len));
}
static std::vector<List> *g_lists;
static void build(List &l, unsigned seed, int depth) {
    for (unsigned i = 0; i < 300; i++) {
        const unsigned m = seed * 31 + i * 7;
        if (m % 4 == 0) put(l, 0x1b, 0, m);
        uint8_t *np = alloc(l, NRND_NOTE_BYTES, NRND_NOTE_ALIGN);
        const size_t c0 = l.cur; const uint32_t at = l.ch[c0].used, count_note = l.ch[c0].count;
        nrnd_note n; nrnd_note_init(&n, 0, 2, 0, 0, 0, m); memcpy(np, &n, sizeof n);
        put(l, NRND_PK_STATE, 0, m);
        for (unsigned k = m % 4; k; k--) put(l, NRND_PK_PARAM, 16 + 48 * (k & 3), m + k);
        if (m % 3) put(l, NRND_PK_TEX, 0, m);
        put(l, NRND_PK_DRAW, 0, m);
        nrnd_seg seg[4]; unsigned ns = 0;
        for (size_t c = c0; c <= l.cur && ns < 4; c++) { seg[ns].base = l.ch[c].data; seg[ns].at = c == c0 ? at : 0; seg[ns].end = l.ch[c].used; ns++; }
        memcpy(&n, np, sizeof n); nrnd_scan(&n, seg, ns);
        if (nrpk_merge_ok(n.flags & NRND_F_FULL, ns, n.npkt, count_note, l.ch[c0].count)) {
            if (i % 16 == 0) g_yard.fetch_add(1, std::memory_order_relaxed);
            else { l.ch[c0].count -= n.npkt; n.flags |= NRND_F_ONE; l.merged++; g_merged.fetch_add(1, std::memory_order_relaxed); }
        }
        memcpy(np, &n, sizeof n);
        if (depth == 0 && i == 150 && (seed & 1)) build((*g_lists)[8 + seed / 2], seed + 100, 1);   // this builder's wait runs another task
    }
}
struct Exec { const List *l; size_t at; bool bad; };
static uint64_t handler(void *u, unsigned op, uint64_t pkt) {
    Exec *e = static_cast<Exec *>(u); const uint8_t *p = reinterpret_cast<const uint8_t *>(uintptr_t(pkt));
    if (op == NRND_OP) { nrnd_note n; memcpy(&n, p, sizeof n); if (!(n.flags & NRND_F_ONE)) return NRND_NOTE_BYTES; int bad = 0; const uint64_t len = nrpk_group_run(pkt, n.npkt, handler, u, &bad); if (bad) e->bad = true; return len; }
    const uint32_t len = nrnd_pkt_len(p);
    if (!len || e->at >= e->l->sums.size() || e->l->sums[e->at] != sum_of(p, len)) { e->bad = true; return len; }
    e->at++; return len;
}
int main() {
    std::vector<List> lists(12);
    g_lists = &lists;
    for (auto &l : lists) { for (int k = 0; k < 16; k++) l.ch.push_back(make_chunk()); l.sums.reserve(8192); }
    std::atomic<bool> stop{false};
    std::thread ev([&stop] { while (!stop.load(std::memory_order_relaxed)) { g_yard.fetch_add(0, std::memory_order_relaxed); std::this_thread::yield(); } });
    std::vector<std::thread> th;
    for (unsigned t = 0; t < 8; t++) th.emplace_back([&lists, t] { build(lists[t], t, 0); });
    for (auto &x : th) x.join();
    stop = true; ev.join();
    uint64_t merged = 0, pkts = 0;
    for (const List &l : lists) {
        Exec e{&l, 0, false};
        for (size_t c = 0; c <= l.cur; c++) {
            const Chunk &k = l.ch[c]; uint64_t cur = uint64_t(uintptr_t(k.data)); const uint64_t end = cur + k.used;
            for (uint32_t i = 0; i < k.count && !e.bad; i++) {
                const uint64_t a = *reinterpret_cast<const uint8_t *>(uintptr_t(cur)), r = cur % a, p = cur + (r ? a - r : a);
                if (p >= end) { e.bad = true; break; }
                cur = p + handler(&e, *reinterpret_cast<const uint8_t *>(uintptr_t(p)), p);
            }
            CHECK(!e.bad && cur == end, "a chunk was not walked to its end");
        }
        CHECK(e.at == l.sums.size(), "packets executed %zu of %zu", e.at, l.sums.size());
        merged += l.merged; pkts += e.at;
    }
    CHECK(merged == g_merged.load() && merged > 2000 && g_yard.load() > 100, "merged %llu / %llu, yardstick %llu", (unsigned long long)merged, (unsigned long long)g_merged.load(), (unsigned long long)g_yard.load());
    printf("%s nr_pkt_tsan_test: %llu checks, %llu failed; %llu packets executed, %llu groups merged\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail, (unsigned long long)pkts, (unsigned long long)merged);
    return g_fail ? 1 : 0;
}
