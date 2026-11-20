// [DOS2] NR-NATIVE step B (docs/dos2/nr/NR-NATIVE.md section 8): the publication of the cards that have a row (dm/gfx/nr/nr_nrn.h
// nrn_pub) under threads - one writer (the translation thread: set, bump, the wrap of the 16-bit generation) and readers (the game
// thread's nrn_pub_ready). What must hold while the writer runs:
//   - a card that was never published is never ready (48 bits of the card are compared, whatever word the slot holds);
//   - a card the writer retired with a bump and did not publish again is not ready for a reader that starts after the bump returned;
//   - a card published and not retired since is ready for a reader that starts after the set returned.
// Build (WSL): g++ -std=c++17 -O1 -g -fsanitize=thread nr_nrn_pub_test.cpp -o nr_nrn_pub_test -lpthread     (also without the sanitizer)
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "../../../nr/nr_nrn.h"

static nrn_pub g_pub;
static std::atomic<uint64_t> g_phase{0};      // even: stable; the writer makes it odd while it changes what phase means
static std::atomic<bool> g_stop{false};
static std::atomic<uint64_t> g_fail{0}, g_checks{0};
static uint64_t mixc(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; return (x ^ (x >> 33)) | 1; }
enum { CARDS = 512 };
static uint64_t card_pub(unsigned i) { return mixc(0x1000 + i); }          // published in the phases whose bit 1 is set
static uint64_t card_never(unsigned i) { return mixc(0x900000 + i); }      // never published

static void reader(unsigned seed) {
    uint64_t n = 0, bad = 0; unsigned x = seed;
    while (!g_stop.load(std::memory_order_relaxed)) {
        x = x * 1664525u + 1013904223u;
        const unsigned i = (x >> 8) % CARDS;
        if (nrn_pub_ready(&g_pub, card_never(i))) ++bad;
        const uint64_t p0 = g_phase.load(std::memory_order_acquire);
        const int r = nrn_pub_ready(&g_pub, card_pub(i));
        const uint64_t p1 = g_phase.load(std::memory_order_acquire);
        if (p0 == p1 && !(p0 & 1)) {                // the writer did nothing in between
            const bool want = (p0 & 2) != 0;        // phase 2 (mod 4): every card_pub is published; phase 0: every one is retired
            // two published cards may share a slot: only the last one set is ready - so "published" is checked for slots of one card only
            if (!want && r) ++bad;
            if (want && !r) { bool alone = true; for (unsigned j = 0; j < CARDS && alone; j++) if (j != i && nrn_pub_slot(card_pub(j)) == nrn_pub_slot(card_pub(i))) alone = false; if (alone) ++bad; }
        }
        n += 2;
    }
    g_checks += n; g_fail += bad;
}
int main() {
    std::vector<std::thread> t;
    for (unsigned k = 0; k < 3; k++) t.emplace_back(reader, 77u + k);
    for (unsigned round = 0; round < 70000; round++) {          // > 65535 bumps: the generation wraps
        g_phase.fetch_add(1, std::memory_order_release);        // odd: changing
        for (unsigned i = 0; i < CARDS; i++) nrn_pub_set(&g_pub, card_pub(i));
        g_phase.fetch_add(1, std::memory_order_release);        // 2 (mod 4): every card_pub is published
        if (!(round & 255)) std::this_thread::yield();
        g_phase.fetch_add(1, std::memory_order_release);        // odd: changing
        nrn_pub_bump(&g_pub);
        g_phase.fetch_add(1, std::memory_order_release);        // 0 (mod 4): every one is retired
    }
    g_stop = true;
    for (auto &x : t) x.join();
    // single-threaded end state
    uint64_t bad = g_fail.load();
    for (unsigned i = 0; i < CARDS; i++) if (nrn_pub_ready(&g_pub, card_pub(i)) || nrn_pub_ready(&g_pub, card_never(i))) ++bad;
    nrn_pub_set(&g_pub, card_pub(5)); if (!nrn_pub_ready(&g_pub, card_pub(5))) ++bad;
    printf("nr_nrn_pub_test: %llu reads, %llu failed, generation %llu: %s\n", (unsigned long long)g_checks.load(), (unsigned long long)bad, (unsigned long long)g_pub.gen, bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
