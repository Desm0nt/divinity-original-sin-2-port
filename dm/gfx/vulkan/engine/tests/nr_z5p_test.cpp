// [DOS2] NR-ZERO Z5 host test of the published templates (dm/gfx/nr/nr_z5p.h; docs/dos2/nr/NR-ZERO.md section 22): what the
// translation thread publishes for the draws it resolved and what the game thread makes of it.
//   ROUND TRIP   a draw described by nrz5_tmpl_make and read back by nrz5p_vals with the same record words gives the draw's own
//                values, field for field (random draws: indexed / plain, 0..8 dynamic offsets, sparse vertex bindings, folds);
//                with other uniform addresses the offsets move by exactly the address difference; a draw a template cannot hold
//                (9 offsets, 9 bindings, an offset above its address, an unknown slot) never predicts.
//   MODEL        the table against a plain model (an array of the last template put in each slot): random puts / gets, versions.
//   NO PREDICTION another site in the slot, an unknown uniform address, an offset past 32 bits.
//   TWO THREADS  one writer republishes slots with self-checking templates (every word is a function of one seed) while readers
//                copy them: a read that returns a template must return ONE published template, never a mix; versions only grow.
// The mutation check: built with -DNRZ5P_MUTATE=k (k = 1..9, nr_z5p.h) this test must FAIL.
// Build, from the repository root:
//   clang++ -std=c++17 -O2 -Wall -Wextra dm/gfx/vulkan/engine/tests/nr_z5p_test.cpp -o nr_z5p_test && ./nr_z5p_test
//   g++ -std=c++17 -O1 -g -fsanitize=thread -pthread dm/gfx/vulkan/engine/tests/nr_z5p_test.cpp -o nr_z5p_tsan && ./nr_z5p_tsan
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include "../../../nr/nr_z5p.h"

static long fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
struct Rng { uint64_t s; uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); } uint32_t upto(uint32_t n) { return n ? next() % n : 0; } };

struct Draw { nrz5_site site; nrz5_vals v; uint32_t offs[40]; uint32_t first_index, k; uint64_t ub[6][16]; uint8_t st[40], sl[40];
              int32_t count, instances, first, base_vertex, base_instance; };
// A draw as the translation thread resolved it. shape: 0 anything a template holds, 1 nine offsets, 2 nine bindings.
static void make(Rng &r, Draw &d, int shape) {
    memset(&d, 0, sizeof d); nrz5_vals &v = d.v;
    const bool indexed = r.upto(6) != 0;
    d.count = int32_t(3 + r.upto(9000)); d.instances = indexed ? int32_t(1 + r.upto(3)) : 1; d.first = indexed ? 0 : int32_t(r.upto(5000));
    d.base_vertex = indexed ? int32_t(r.upto(70000)) - 100 : 0; d.base_instance = indexed ? int32_t(r.upto(3)) : 0;
    const uint64_t ibuf = indexed ? 0x7100000000ull + 2 * r.upto(100000) : 0, vb0 = 0x7200000000ull + 16 * r.upto(100000);
    nrz5_site_make(&d.site, 1 + r.upto(4000), indexed ? NRZ5S_IDX : NRZ5S_ARRAYS, 4, indexed ? 1 + int32_t(r.upto(2)) : 0, d.count, d.base_vertex, d.base_instance, d.instances, d.first, ibuf, vb0);
    v.pipe = 0x1000 + r.upto(500); v.layout = 0x2000 + r.upto(20); v.set = 0x3000 + r.upto(300);
    v.noff = shape == 1 ? 9 : r.upto(NRZ5T_MAX_DYN + 1); v.off = d.offs;
    for (uint32_t i = 0; i < v.noff; i++) {   // each offset from its own uniform slot (distinct slots: a slot holds one address)
        d.st[i] = uint8_t(i % 6); d.sl[i] = uint8_t(i / 6 + r.upto(2) * 8);
        const uint64_t base = 0x7300000000ull + (uint64_t(r.upto(4)) << 24); d.offs[i] = 256 * r.upto(1 << 16);
        d.ub[d.st[i]][d.sl[i]] = base + d.offs[i];
    }
    for (int i = 0; i < 24; i++) v.vp[i] = uint8_t(r.next()); for (int i = 0; i < 16; i++) v.sc[i] = uint8_t(r.next());
    const unsigned nvb = shape == 2 ? 9 : 1 + r.upto(NRZ5T_MAX_VB);
    while (true) { unsigned n = 0; for (uint32_t m = v.used; m; m &= m - 1) ++n; if (n >= nvb) break; v.used = uint16_t(v.used | 1u << r.upto(16)); }
    for (uint32_t i = 0; i < 16; i++) if (v.used >> i & 1) { v.vb[i] = 0x4000 + r.upto(3000); v.vbo[i] = 4 * r.upto(1 << 20); }
    d.k = r.upto(3) ? 0 : r.upto(1000); d.first_index = indexed ? r.upto(60000) : 0;
    v.indexed = indexed; v.count = uint32_t(d.count);
    if (indexed) { v.ib = 0x5000 + r.upto(300); v.ibo = r.upto(4); v.it = r.upto(2); v.instances = uint32_t(d.instances); v.first = d.first_index; v.vertex_offset = int32_t(uint32_t(d.base_vertex) + d.k); v.first_instance = uint32_t(d.base_instance); }
    else { v.instances = 1; v.first = uint32_t(d.first) + d.k; v.first_instance = 0; }
}
static void tmpl_of(const Draw &d, nrz5_tmpl *t) {
    nrz5_tmpl_make(t, &d.site, &d.v, d.first_index, d.k, [&](uint32_t i, unsigned *st, unsigned *sl, uint64_t *a) { *st = d.st[i]; *sl = d.sl[i]; *a = d.ub[d.st[i]][d.sl[i]]; return true; });
}
static bool same_vals(const nrz5_vals &a, const nrz5_vals &b) {
    if (a.pipe != b.pipe || a.layout != b.layout || a.set != b.set || a.noff != b.noff || (a.noff && memcmp(a.off, b.off, size_t(a.noff) * 4))) return false;
    if (memcmp(a.vp, b.vp, sizeof a.vp) || memcmp(a.sc, b.sc, sizeof a.sc) || a.used != b.used) return false;
    for (uint32_t i = 0; i < 16; i++) if (a.used >> i & 1) if (a.vb[i] != b.vb[i] || a.vbo[i] != b.vbo[i]) return false;
    if (a.indexed != b.indexed || a.count != b.count || a.instances != b.instances || a.first != b.first || a.first_instance != b.first_instance) return false;
    if (a.indexed && (a.ib != b.ib || a.ibo != b.ibo || a.it != b.it || a.vertex_offset != b.vertex_offset)) return false;
    return true;
}

static void round_trip() {
    Rng r{0x1234567};
    unsigned pred = 0, moved = 0, with_k = 0;
    for (int n = 0; n < 200000; n++) {
        Draw d; make(r, d, 0); nrz5_tmpl t; tmpl_of(d, &t);
        CHECK(t.flags & NRZ5T_F_OK, "a draw a template holds is described");
        nrz5_vals v; uint32_t off[NRZ5T_MAX_DYN];
        auto ub = [&](unsigned st, unsigned sl) { return d.ub[st][sl]; };
        const bool ok = nrz5p_vals(&t, &d.site, d.count, d.instances, d.first, d.base_vertex, d.base_instance, ub, &v, off);
        CHECK(ok && same_vals(v, d.v), "the template of a draw gives the draw's own values (draw %d)", n);
        if (ok) ++pred; if (d.k) ++with_k;
        // the same draw with its uniform blocks somewhere else: each offset moves by its block's move
        if (d.v.noff) { Draw e = d; e.v.off = e.offs; const uint32_t i = r.upto(d.v.noff); const uint32_t mv = 256 * (1 + r.upto(100));
            e.ub[e.st[i]][e.sl[i]] += mv; e.offs[i] += mv;
            auto ub2 = [&](unsigned st, unsigned sl) { return e.ub[st][sl]; };
            const bool ok2 = nrz5p_vals(&t, &e.site, e.count, e.instances, e.first, e.base_vertex, e.base_instance, ub2, &v, off);
            CHECK(ok2 && same_vals(v, e.v) && !same_vals(v, d.v), "a moved uniform block moves its offset by as much"); ++moved;
            // an unknown address: no prediction
            e.ub[e.st[i]][e.sl[i]] = 0;
            CHECK(!nrz5p_vals(&t, &e.site, e.count, e.instances, e.first, e.base_vertex, e.base_instance, ub2, &v, off), "an unknown uniform address: no prediction");
            // an address 4 GiB above: the offset does not fit
            e.ub[e.st[i]][e.sl[i]] = d.ub[d.st[i]][d.sl[i]] + (1ull << 32);
            CHECK(!nrz5p_vals(&t, &e.site, e.count, e.instances, e.first, e.base_vertex, e.base_instance, ub2, &v, off), "an offset past 32 bits: no prediction");
            // an address below the template's base
            e.ub[e.st[i]][e.sl[i]] = t.delta[i] ? t.delta[i] - 1 : 0;
            CHECK(!nrz5p_vals(&t, &e.site, e.count, e.instances, e.first, e.base_vertex, e.base_instance, ub2, &v, off), "an address below the block's base: no prediction"); }
        // another site in the slot
        { nrz5_site s2 = d.site; s2.w[r.upto(NRZ5T_SITE_WORDS)] ^= 1ull << r.upto(40);
          CHECK(!nrz5p_vals(&t, &s2, d.count, d.instances, d.first, d.base_vertex, d.base_instance, ub, &v, off), "the template of another site: no prediction"); }
    }
    CHECK(pred == 200000 && moved > 100000 && with_k > 30000, "round trip: %u predicted, %u moved, %u folded", pred, moved, with_k);
    // what a template cannot hold
    for (int n = 0; n < 2000; n++) { Draw d; make(r, d, 1 + n % 2); nrz5_tmpl t; tmpl_of(d, &t); nrz5_vals v; uint32_t off[40];
        CHECK(!(t.flags & NRZ5T_F_OK) && !memcmp(t.site, d.site.w, sizeof t.site), "nine offsets / nine bindings: described as 'no prediction' under its site");
        CHECK(!nrz5p_vals(&t, &d.site, d.count, d.instances, d.first, d.base_vertex, d.base_instance, [&](unsigned st, unsigned sl) { return d.ub[st][sl]; }, &v, off), "and never predicted"); }
    for (int n = 0; n < 2000; n++) { Draw d; do make(r, d, 0); while (!d.v.noff); nrz5_tmpl t; const uint32_t bad = r.upto(d.v.noff);
        nrz5_tmpl_make(&t, &d.site, &d.v, d.first_index, d.k, [&](uint32_t i, unsigned *st, unsigned *sl, uint64_t *a) { *st = d.st[i]; *sl = d.sl[i]; *a = d.ub[d.st[i]][d.sl[i]];
            if (i == bad) { if (n & 1) return false; *a = d.offs[i] ? d.offs[i] - 1 : 0; } return true; });
        CHECK(!(t.flags & NRZ5T_F_OK) && !t.pipe && !t.noff, "an offset whose slot is unknown / above its address: no template"); }
    // equal draws give equal bytes (the writer's "already there" test compares words)
    { Draw d; make(r, d, 0); nrz5_tmpl a, b; memset(&a, 0xa5, sizeof a); memset(&b, 0x5a, sizeof b); tmpl_of(d, &a); tmpl_of(d, &b); CHECK(!memcmp(&a, &b, sizeof a), "a template is a function of the draw"); }
    // site words
    { nrz5_site a, b; nrz5_site_make(&a, 7, NRZ5S_IDX, 4, 1, 300, 5, 0, 1, 0, 0x7100000000ull, 0x7200000000ull);
      const int32_t args[9][7] = {{4,1,300,5,0,1,0},{5,1,300,5,0,1,0},{4,2,300,5,0,1,0},{4,1,301,5,0,1,0},{4,1,300,6,0,1,0},{4,1,300,5,1,1,0},{4,1,300,5,0,2,0},{4,1,300,5,0,1,1},{4,1,300,5,0,1,0}};
      for (int i = 0; i < 8; i++) { nrz5_site_make(&b, 7, NRZ5S_IDX, args[i][0], args[i][1], args[i][2], args[i][3], args[i][4], args[i][5], args[i][6], 0x7100000000ull, 0x7200000000ull);
          CHECK((i == 0) == !memcmp(&a, &b, sizeof a), "site words: argument %d", i); }
      nrz5_site_make(&b, 8, NRZ5S_IDX, 4, 1, 300, 5, 0, 1, 0, 0x7100000000ull, 0x7200000000ull); CHECK(memcmp(&a, &b, sizeof a), "site words: key id");
      nrz5_site_make(&b, 7, NRZ5S_ARRAYS, 4, 1, 300, 5, 0, 1, 0, 0x7100000000ull, 0x7200000000ull); CHECK(memcmp(&a, &b, sizeof a), "site words: draw kind");
      nrz5_site_make(&b, 7, NRZ5S_IDX, 4, 1, 300, 5, 0, 1, 0, 0x7100000002ull, 0x7200000000ull); CHECK(memcmp(&a, &b, sizeof a), "site words: index buffer");
      nrz5_site_make(&b, 7, NRZ5S_IDX, 4, 1, 300, 5, 0, 1, 0, 0x7100000000ull, 0x7200000010ull); CHECK(memcmp(&a, &b, sizeof a), "site words: vertex stream 0");
      CHECK(nrz5_site_hash(&a, 0) != nrz5_site_hash(&a, 1), "the occurrence moves the slot"); }
}

static void model() {
    nrz5p::Table T; CHECK(T.init(1000), "init"); CHECK(T.n == 512, "slots rounded down to a power of two (%u)", T.n);
    std::vector<nrz5_tmpl> last(T.n); std::vector<uint32_t> ver(T.n, 0);
    Rng r{99}; std::vector<Draw> pool(64); for (auto &d : pool) make(r, d, 0);
    uint64_t puts = 0, same = 0, gets = 0, empty = 0;
    for (int n = 0; n < 300000; n++) {
        const uint32_t i = r.upto(64);   // a few slots: puts repeat
        if (r.upto(3)) { nrz5_tmpl t; tmpl_of(pool[r.upto(8) ? i : r.upto(64)], &t); if (!r.upto(50)) t.vb_at[NRZ5T_MAX_VB - 1] ^= 1;   // (a template that differs in its last word only)
            const bool want = !ver[i] || memcmp(&last[i], &t, sizeof t); const bool wrote = T.put(i, &t);
            CHECK(wrote == want, "put writes iff the slot holds other words"); if (wrote) { last[i] = t; ver[i] += 2; ++puts; } else ++same; }
        else { nrz5_tmpl t; uint32_t v = 0; const bool ok = T.get(i, &t, &v); ++gets;
            if (!ver[i]) { CHECK(!ok, "an empty slot gives nothing"); ++empty; } else CHECK(ok && v == ver[i] && !memcmp(&t, &last[i], sizeof t), "get gives the last template put and its version"); }
    }
    CHECK(puts > 1000 && same > 1000 && gets > 1000 && T.puts == puts && T.same == same, "model: %llu puts, %llu already there, %llu gets (%llu empty)", (unsigned long long)puts, (unsigned long long)same, (unsigned long long)gets, (unsigned long long)empty);
    { const Draw &d = pool[0]; CHECK(T.slot_of(&d.site, 0) < T.n && T.slot_of(&d.site, 0) == T.slot_of(&d.site, 0), "slot_of"); }
    T.destroy();
}

// every word of a self-checking template is a function of (seed, index)
static uint64_t wword(uint64_t seed, int j) { uint64_t h = (seed + 1) * 0x9E3779B97F4A7C15ull + uint64_t(j) * 0xC2B2AE3D27D4EB4Full; h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; return h ^ h >> 29; }
static void two_threads() {
    static nrz5p::Table T; CHECK(T.init(16), "init");
    std::atomic<bool> stop{false}; std::atomic<uint64_t> good{0}, torn{0}, back{0}, none{0};
    const unsigned rounds =
#if defined(__SANITIZE_THREAD__)
        60000;
#else
        600000;
#endif
    auto reader = [&](unsigned id) { Rng r{id + 77}; uint32_t seen[16] = {};
        while (!stop.load(std::memory_order_relaxed)) { const uint32_t i = r.upto(16); nrz5_tmpl t; uint32_t v = 0;
            if (!T.get(i, &t, &v)) { none.fetch_add(1, std::memory_order_relaxed); continue; }
            uint64_t w[NRZ5T_WORDS]; memcpy(w, &t, sizeof w); bool ok = true; for (int j = 1; j < NRZ5T_WORDS && ok; j++) ok = w[j] == wword(w[0], j);
            if (ok) good.fetch_add(1, std::memory_order_relaxed); else torn.fetch_add(1, std::memory_order_relaxed);
            if (v < seen[i] || (v & 1)) back.fetch_add(1, std::memory_order_relaxed); seen[i] = v; } };
    std::thread r1(reader, 1), r2(reader, 2);
    { Rng r{5}; for (unsigned n = 0; n < rounds; n++) { uint64_t w[NRZ5T_WORDS]; w[0] = n; for (int j = 1; j < NRZ5T_WORDS; j++) w[j] = wword(n, j); nrz5_tmpl t; memcpy(&t, w, sizeof t); T.put(r.upto(16), &t); } }
    stop.store(true); r1.join(); r2.join();
    CHECK(torn.load() == 0 && back.load() == 0 && good.load() > 1000, "two readers against the writer: %llu whole templates, %llu torn, %llu versions out of order, %llu reads thrown away",
          (unsigned long long)good.load(), (unsigned long long)torn.load(), (unsigned long long)back.load(), (unsigned long long)none.load());
    // a slot that is being written gives nothing; one written meanwhile gives nothing (the version says so)
    { nrz5_tmpl t; uint32_t v = 0; nrz5p::Slot &x = T.s[3]; const uint32_t v0 = x.ver.load();
      x.ver.store(v0 + 1); CHECK(!T.get(3, &t, &v), "a slot being written gives nothing"); x.ver.store(v0 + 2); CHECK(v0 == 0 || T.get(3, &t, &v), "and gives its template again when the write is over"); }
    printf("     two threads: %llu whole reads, %llu thrown away\n", (unsigned long long)good.load(), (unsigned long long)none.load());
}
// The reader's two version loads, step by step (what a second thread would do between them, done here in one thread).
static void interleave() {
    static nrz5p::Table T; CHECK(T.init(4), "init"); Rng r{3}; Draw a, b; make(r, a, 0); make(r, b, 0); nrz5_tmpl ta, tb; tmpl_of(a, &ta); tmpl_of(b, &tb);
    T.put(1, &ta);
    // a writer that stops in the middle: the version is odd, half the words are b's
    nrz5p::Slot &x = T.s[1]; uint64_t wb[NRZ5T_WORDS]; memcpy(wb, &tb, sizeof wb); const uint32_t v = x.ver.load();
    x.ver.store(v + 1); for (int j = 0; j < NRZ5T_WORDS / 2; j++) x.w[j].store(wb[j]);
    nrz5_tmpl t; uint32_t gv = 0; CHECK(!T.get(1, &t, &gv), "a half-written slot is not read");
    for (int j = NRZ5T_WORDS / 2; j < NRZ5T_WORDS; j++) x.w[j].store(wb[j]); x.ver.store(v + 2);
    CHECK(T.get(1, &t, &gv) && gv == v + 2 && !memcmp(&t, &tb, sizeof t), "the finished write is read whole");
    // put() itself leaves an even, larger version and makes it odd on the way (the mutation of the writer shows here)
    struct Spy { static bool odd_seen(nrz5p::Table &T2, uint32_t i, const nrz5_tmpl *t2) { const uint32_t v0 = T2.s[i].ver.load(); std::atomic<bool> go{true}, saw{false};
        std::thread th([&] { while (go.load(std::memory_order_relaxed)) if (T2.s[i].ver.load(std::memory_order_relaxed) & 1) saw.store(true); });
        for (int n = 0; n < 400000 && !saw.load(); n++) { nrz5_tmpl u = *t2; u.pipe = 0x9000 + n; T2.put(i, &u); }
        go.store(false); th.join(); return saw.load() && T2.s[i].ver.load() > v0 && !(T2.s[i].ver.load() & 1); } };
    CHECK(Spy::odd_seen(T, 2, &ta), "a put passes through an odd version and ends on an even one");
}

int main() {
    round_trip(); model(); interleave(); two_threads();
    printf("nr_z5p_test: %ld checks, %ld failed -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
