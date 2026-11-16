// [dos2-nb] NB-07: host test of the "program -> sampled slots" table (dm/gfx/nb/nb_slotmask.h): the backend publishes from its
// translation thread(s) while the front end reads on the game's render thread. Checks: basic insert / lookup / republish / key 0 / probe
// limit (full), then a stress run: writers publish keys (each key's masks are one of two patterns derived from the key, republished over
// and over so the seqlock is hit) while readers look keys up: every hit must be exactly one of the key's two patterns (never a torn mix),
// a key published once must stay found once it was found, and every key is found after the writers joined.
// Build (WSL g++ / clang): g++ -std=c++17 -O2 -Wall -pthread nb_slotmask_test.cpp -o nb_slotmask_test
#include "../../../nb/nb_slotmask.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

static nb_sm_table T;
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return x; }
static nb_sm_masks pattern(uint64_t key, int alt) {
    nb_sm_masks m;
    for (int s = 0; s < NB_SM_STAGES; s++) { m.tex[s] = mix(key * 16 + s * 2 + alt); m.img[s] = mix(key * 16 + s * 2 + 1 + alt * 7) | 1; }
    return m;
}
static bool same(const nb_sm_masks &a, const nb_sm_masks &b) { return !memcmp(&a, &b, sizeof a); }

static void test_basic() {
    memset(&T, 0, sizeof T);
    nb_sm_masks r{}, m = pattern(5, 0);
    CHECK(nb_sm_lookup(&T, 5, &r) == 0);
    CHECK(nb_sm_publish(&T, 0, &m) == 0 && T.n == 0);
    CHECK(nb_sm_publish(&T, 5, &m) == 1 && T.n == 1);
    CHECK(nb_sm_lookup(&T, 5, &r) == 1 && same(r, m));
    m = pattern(5, 1);
    CHECK(nb_sm_publish(&T, 5, &m) == 1 && T.n == 1 && T.writes == 2);
    CHECK(nb_sm_lookup(&T, 5, &r) == 1 && same(r, m));
    CHECK(nb_sm_lookup(&T, 6, &r) == 0 && nb_sm_lookup(nullptr, 5, &r) == 0 && nb_sm_lookup(&T, 0, &r) == 0);
    // Keys with one home slot: NB_SM_PROBE fit, the next one is dropped (counted) and stays unknown, the others stay found.
    memset(&T, 0, sizeof T);
    std::vector<uint64_t> keys;
    for (uint64_t k = 1; keys.size() < NB_SM_PROBE + 1; k++) if (nb_sm_home(k) == 100) keys.push_back(k);
    for (size_t i = 0; i < keys.size(); i++) { m = pattern(keys[i], 0); CHECK(nb_sm_publish(&T, keys[i], &m) == (i < NB_SM_PROBE)); }
    CHECK(T.n == NB_SM_PROBE && T.full == 1);
    for (size_t i = 0; i < keys.size(); i++) CHECK(nb_sm_lookup(&T, keys[i], &r) == (i < NB_SM_PROBE) && (i >= NB_SM_PROBE || same(r, pattern(keys[i], 0))));
    // The front end's take: the rebound slots or all, & mask; the rest stays pending.
    uint64_t d = 0x30; CHECK(nb_sm_take(&d, 0, 0x10) == 0x10 && d == 0x20);
    d = 0; CHECK(nb_sm_take(&d, 1, 0x3) == 0x3 && d == ~0x3ull);
    d = 0x5; CHECK(nb_sm_take(&d, 0, NB_SM_ALL) == 0x5 && d == 0);
}

// once = 1: the real pattern (the backend publishes a program once, when program_shaders resolves it): the writers split the keys; a key
// found once must stay found (lost = 0). once = 0: both writers republish every key ROUNDS times with alternating patterns (seqlock
// contention); a reader that keeps meeting a writer inside gives up after NB_SM_RETRY tries and says "unknown" (= conservative, counted
// as lost, allowed); torn reads must still be 0.
static void stress(int once) {
    memset(&T, 0, sizeof T);
    enum { KEYS = 6000, WRITERS = 2, READERS = 2 };
    const int rounds = once ? 1 : 40;
    std::atomic<int> done{0};
    std::atomic<uint64_t> torn{0}, lost{0}, hits{0}, misses{0};
    std::vector<std::thread> th;
    for (int w = 0; w < WRITERS; w++) th.emplace_back([&, w] {
        for (int r = 0; r < rounds; r++)
            for (uint64_t i = 0; i < KEYS; i++) {
                const uint64_t n = w & 1 ? KEYS - i : i + 1;   // opposite orders
                if (once && (n & 1) != (uint64_t)w) continue;  // once: writer 0 the even keys, writer 1 the odd ones
                const uint64_t k = n * 0x10001ull;
                const nb_sm_masks m = pattern(k, once ? 0 : (int)((r + w) & 1));
                nb_sm_publish(&T, k, &m);
            }
        done.fetch_add(1);
    });
    for (int rd = 0; rd < READERS; rd++) th.emplace_back([&, rd] {
        std::vector<char> seen(KEYS + 1, 0);
        uint64_t x = 12345 + rd;
        while (done.load() < WRITERS) {
            x = mix(x); const uint64_t i = x % KEYS + 1, k = i * 0x10001ull;
            nb_sm_masks r;
            if (nb_sm_lookup(&T, k, &r)) {
                hits.fetch_add(1, std::memory_order_relaxed); seen[i] = 1;
                if (!same(r, pattern(k, 0)) && !(!once && same(r, pattern(k, 1)))) torn.fetch_add(1);
            } else {
                misses.fetch_add(1, std::memory_order_relaxed);
                if (seen[i]) lost.fetch_add(1);
            }
        }
    });
    for (auto &t : th) t.join();
    int missing = 0;
    for (uint64_t i = 1; i <= KEYS; i++) { nb_sm_masks r; const uint64_t k = i * 0x10001ull; if (!nb_sm_lookup(&T, k, &r) || !(same(r, pattern(k, 0)) || same(r, pattern(k, 1)))) missing++; }
    const uint32_t writes = once ? (uint32_t)KEYS : (uint32_t)KEYS * WRITERS * (uint32_t)rounds;
    printf("# stress %s: %d keys, %d writers x %d rounds, %d readers: %llu hits, %llu misses, torn %llu, lost %llu, missing after %d, n=%u full=%u writes=%u\n",
           once ? "publish once" : "republish", (int)KEYS, (int)WRITERS, rounds, (int)READERS, (unsigned long long)hits.load(), (unsigned long long)misses.load(),
           (unsigned long long)torn.load(), (unsigned long long)lost.load(), missing, T.n, T.full, T.writes);
    CHECK(torn.load() == 0 && missing == 0 && T.n == KEYS && T.full == 0 && T.writes == writes);
    if (once) CHECK(lost.load() == 0);
}

int main() {
    test_basic();
    for (int i = 0; i < 4; i++) { stress(1); stress(0); }
    printf(failures ? "nb_slotmask_test: %d FAILED\n" : "nb_slotmask_test: all passed\n", failures);
    return failures ? 1 : 0;
}
