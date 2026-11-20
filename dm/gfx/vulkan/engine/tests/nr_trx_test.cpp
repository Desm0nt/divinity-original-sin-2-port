// [DOS2] NR-TRX host test + micro-benchmarks (docs/dos2/nr/NR-TRX.md): the translator hot-path changes against the code they replace.
//  A  DM_NR_TRX_ZC     nr_trx.h nrt_ix (sorted index of live chunks + last chunk) == zc::holder()'s scan of every table slot, on random
//                      tables (slots reused, retired holes, adjacent chunks of one pool, overlapping chunks -> ok 0) and every boundary;
//                      bench: a Fort Joy table (22 live 64 MB chunks in 6 pools, 34 slots; play-1118 "chunks=22 retired=30"), two lookups
//                      a draw (vertex + index) with the cache disturbed between draws as the rest of a draw does.
//  B  DM_NR_TRX_STICKY nrt_wrset (written byte ranges, merged) == a byte bitmap of every replay write: "meets" exact, newest serial >= the
//                      true one, ranges sorted / disjoint / not touching, coarse past NRT_WR_MAX; bench: add / meets cost.
//  C  DM_NR_TRX_GRP    nb_pipe.h nbp_refresh with the recent-id compare == without (the same group id on every draw, mode 2 finds 0),
//                      mutation (an MRU that ignores a dictionary restart) caught; bench: Fort Joy group sizes (vertex format 192 B, blend
//                      80, depth-stencil ~32 ...), 2.76 changed + 0.95 unchanged groups re-read a draw (play-1118 "[nb] pipe" line).
//  D  NB-MT packets    ../xbg_nb_mt.inc against the mock Vulkan: translate-thread ns a packet of the served-draw mix (pipeline 0.56, set
//                      0.79, vertex 0.91, index 0.53, draw 1 a draw; play-1118 "[nr] draw" line) in mode 0 (direct) / 1 (packets replayed
//                      inline) / 3 (worker: producer side only).
//  E  DM_NR_TRX_STICKY stable written ranges + the replay-write guard (NR-TRX section 8): nrt_sv_conflict == the list of every serve;
//                      nrt_sticky_class == byte bitmaps; a replay model with the engine's two frame slots in flight (serves, replay CPU
//                      writes, submits, in-order GPU ends, fence waits, gpu_sync, chunk retire): 0 stale in-place reads, every guard
//                      mutation caught; bench: note / conflict cost.
// Build (Vulkan headers as nb_mt_test): clang++ -std=c++17 -O2 -Wall -Wextra -I<vk> nr_trx_test.cpp -o nr_trx_test.exe
#include <vulkan/vulkan_core.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <random>

static std::vector<std::string> g_lines;
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void xbg_logf(const char *fmt, ...) { char t[2048]; va_list a; va_start(a, fmt); vsnprintf(t, sizeof t, fmt, a); va_end(a); g_lines.push_back(t); }
#include "nb_mt_mock.h"
#define NBMT_REAL(f) mock_##f
#include "../xbg_nb_mt.inc"
#include "../../../nr/nr_trx.h"
#include "../../../nb/nb_pipe.h"

static int fails = 0; static uint64_t checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { if (++fails < 30) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t now_ns() { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static volatile uint64_t g_sink = 0;
static void setenv_(const char *k, const char *v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

// ============ A: the chunk index ============
namespace A {
struct Chunk { uintptr_t lo = 0; size_t n = 0; uintptr_t owner = 0; uint64_t gen = 0; bool live = false; void *buffer{}; void *memory{}; void *ahb = nullptr;
               std::vector<uint8_t> sticky; nrt_wrset wr{}; };   // the engine's zc::Chunk layout (VkBuffer / VkDeviceMemory are pointer-sized)
enum { MAX = 512 };
Chunk table[MAX]; int count = 0; uint64_t next_gen = 1; uint64_t ver = 1, ix_ver = 0; nrt_ix ix;
Chunk *scan(uint64_t a) {   // zc::holder_scan, verbatim
    int n = count; Chunk *best = nullptr;
    for (int i = 0; i < n; i++) { Chunk &c = table[i]; if (c.live && a >= c.lo && a - c.lo < c.n && (!best || c.gen > best->gen)) best = &c; }
    return best;
}
Chunk *indexed(uint64_t a) {   // zc::holder_ix without the counters
    if (ix_ver != ver) { ix_ver = ver; uint64_t lo[MAX], sz[MAX]; uint8_t live[MAX];
        for (int i = 0; i < count; i++) { lo[i] = table[i].lo; sz[i] = table[i].n; live[i] = table[i].live; } nrt_ix_build(&ix, lo, sz, live, count); }
    if (!ix.ok) return scan(a);
    const int s = nrt_ix_find(&ix, a); return s >= 0 ? &table[s] : nullptr;
}
void put(uintptr_t lo, size_t n) { int i = 0; while (i < count && table[i].live) ++i; Chunk &t = table[i]; t.lo = lo; t.n = n; t.gen = next_gen++; t.live = true; if (i == count) ++count; ++ver; }
void retire(int i) { table[i].live = false; ++ver; }
void reset() { for (int i = 0; i < MAX; i++) table[i] = Chunk{}; count = 0; ++ver; }

void exact(std::mt19937_64 &R) {
    uint64_t compared = 0, overlap_tables = 0;
    for (int t = 0; t < 3000; t++) {
        reset();
        const bool overlap = t % 10 == 9;
        // pools of 1..8 adjacent 64 MB chunks (the last one shorter), spread over a 64 GB range, some retired, slots reused
        const int pools = 1 + int(R() % 12);
        for (int p = 0; p < pools && count < 400; p++) {
            uintptr_t base = (uintptr_t)(0x1000000000ull + (R() % 1024) * (64ull << 20)) + (R() % 4) * 4096;
            const int k = 1 + int(R() % 8);
            for (int c = 0; c < k; c++) { const size_t n = c + 1 < k ? (64u << 20) : size_t(4096 * (1 + R() % 16384)); bool clash = false;
                for (int i = 0; i < count; i++) if (table[i].live && base < table[i].lo + table[i].n && table[i].lo < base + n) clash = true;
                if (!clash || overlap) put(base, n);
                base += n; }
            if (R() % 3 == 0 && count) retire(int(R() % count));
        }
        bool any_overlap = false;
        for (int i = 0; i < count; i++) for (int j = i + 1; j < count; j++) if (table[i].live && table[j].live && table[i].lo < table[j].lo + table[j].n && table[j].lo < table[i].lo + table[i].n) any_overlap = true;
        indexed(0); CHECK(ix.ok == !any_overlap, "table %d: overlap detection %d vs %d", t, ix.ok, int(any_overlap)); overlap_tables += any_overlap;
        for (int q = 0; q < 1500; q++) {
            uint64_t a; const int i = count ? int(R() % count) : 0; const Chunk &c = table[i];
            switch (R() % 6) { case 0: a = c.lo; break; case 1: a = c.lo + c.n - 1; break; case 2: a = c.lo + c.n; break; case 3: a = c.lo ? c.lo - 1 : 0; break;
                case 4: a = c.lo + R() % (c.n ? c.n : 1); break; default: a = 0x1000000000ull + R() % (80ull << 30); }
            Chunk *x = scan(a), *y = indexed(a); ++compared;
            CHECK(x == y, "table %d address %#llx: scan %p index %p", t, (unsigned long long)a, (void *)x, (void *)y);
        }
    }
    printf("A exact: %llu lookups on 3000 tables (%llu with overlapping live chunks -> the scan): index == scan\n", (unsigned long long)compared, (unsigned long long)overlap_tables);
}
void bench() {
    reset(); std::mt19937_64 R(7);
    // Fort Joy: 22 live chunks (725 MB) in 6 pools + 12 retired slots in between (34 slots), as play-1118 "chunks=22 retired=30" with reuse
    const int per_pool[6] = {6, 5, 4, 3, 2, 2}; uintptr_t base = 0x10be000000ull;
    auto dead = [](uintptr_t lo) { Chunk &t = table[count++]; t.lo = lo; t.n = 64u << 20; t.gen = next_gen++; t.live = false; ++ver; };   // a retired slot
    for (int p = 0; p < 6; p++) { for (int c = 0; c < per_pool[p]; c++) { put(base, 64u << 20); base += 64u << 20; if (R() % 2 && count < 34) dead(base + (1ull << 36)); } base += 7ull << 30; }
    while (count < 34) dead(0x7f00000000ull + uintptr_t(count) * (1ull << 26));
    std::vector<Chunk *> live; for (int i = 0; i < count; i++) if (table[i].live) live.push_back(&table[i]);
    // 2600 meshes, each with a vertex and an index range somewhere in the live chunks; draws walk them in runs (material order) or at random
    struct Mesh { uint64_t vb, ib; }; std::vector<Mesh> mesh(2600);
    for (auto &m : mesh) { const Chunk *a = live[R() % live.size()], *b = R() % 4 ? a : live[R() % live.size()]; m.vb = a->lo + R() % a->n; m.ib = b->lo + R() % b->n; }
    std::vector<uint8_t> junk(4u << 20); for (auto &x : junk) x = uint8_t(R());
    const int draws = 100000; std::vector<uint32_t> order(draws), lines(draws * 96);
    for (int pass = 0; pass < 2; pass++) {
        for (int d = 0; d < draws; d++) order[d] = pass == 0 ? uint32_t(R() % mesh.size()) : uint32_t((d / 3 + (d % 3 ? 0 : uint32_t(R() % 7))) % mesh.size());
        for (auto &l : lines) l = uint32_t(R() % (junk.size() / 64));
        double best[3] = {1e18, 1e18, 1e18};
        for (int rep = 0; rep < 3; rep++) {
            for (int v = 0; v < 3; v++) {   // 0: disturbance only, 1: scan, 2: index
                uint64_t acc = 0; const uint64_t t0 = now_ns();
                for (int d = 0; d < draws; d++) {
                    for (int k = 0; k < 96; k++) acc += junk[size_t(lines[size_t(d) * 96 + k]) * 64];   // the rest of a draw: ~6 KB of other data
                    const Mesh &m = mesh[order[d]];
                    if (v == 1) { acc += uintptr_t(scan(m.vb)); acc += uintptr_t(scan(m.ib)); }
                    else if (v == 2) { acc += uintptr_t(indexed(m.vb)); acc += uintptr_t(indexed(m.ib)); }
                }
                const double ns = double(now_ns() - t0) / draws; g_sink += acc; if (ns < best[v]) best[v] = ns;
            }
        }
        printf("A bench (%s mesh order, 34 slots / 22 live): a lookup: scan %.1f ns, index %.1f ns (%.1f -> %.1f ns a draw of 2 lookups)\n", pass ? "material-run" : "random",
               (best[1] - best[0]) / 2, (best[2] - best[0]) / 2, best[1] - best[0], best[2] - best[0]);
    }
}
}

// ============ B: written byte ranges ============
namespace B {
void invariants(const nrt_wrset &s, const char *where) {
    for (uint32_t i = 0; i < s.n; i++) { CHECK(s.v[i].lo < s.v[i].hi, "%s: empty range", where); if (i + 1 < s.n) CHECK(s.v[i].hi < s.v[i + 1].lo, "%s: ranges %u / %u overlap or touch", where, i, i + 1); }
}
void exact(std::mt19937_64 &R) {
    const uint32_t N = 4u << 20; std::vector<uint8_t> bit(N); std::vector<uint32_t> ser(N);
    uint64_t queries = 0, meets = 0;
    for (int t = 0; t < 16; t++) {
        nrt_wrset s{}; std::fill(bit.begin(), bit.end(), 0); std::fill(ser.begin(), ser.end(), 0);
        const int marks = 50 + int(R() % 2000);
        for (int m = 0; m < marks; m++) {
            const uint32_t len = R() % 8 ? 16 + uint32_t(R() % 256) : 1 + uint32_t(R() % 70000);
            uint32_t a = uint32_t(R() % N); if (R() % 3 == 0) a &= ~4095u; if (a + len > N) a = N - len;
            const uint32_t serial = uint32_t(m + 1);
            CHECK(nrt_wr_add(&s, a, a + len, serial) == 0, "add went coarse at %u ranges", s.n);
            for (uint32_t i = a; i < a + len; i++) { bit[i] = 1; ser[i] = serial; }
            if (m % 97 == 0) invariants(s, "after add");
            for (int q = 0; q < 3; q++) {
                const uint32_t ql = 1 + uint32_t(R() % (R() % 16 ? 4096 : 90000)); uint32_t qa = uint32_t(R() % N); if (qa + ql > N) qa = N - ql;
                bool ref = false; uint32_t refn = 0; for (uint32_t i = qa; i < qa + ql; i++) if (bit[i]) { ref = true; if (ser[i] > refn) refn = ser[i]; }
                uint64_t nw = 0; const int got = nrt_wr_meets(&s, qa, qa + ql, &nw); ++queries; meets += got;
                CHECK(got == int(ref), "query [%u,+%u): set %d, bytes %d", qa, ql, got, int(ref));
                if (got && ref) CHECK(nw >= refn, "newest serial %llu below the byte's %u", (unsigned long long)nw, refn);
                CHECK(nrt_wr_meets(&s, qa, qa + ql, nullptr) == got, "meets without newest differs");
            }
        }
        invariants(s, "end");
        nrt_wr_free(&s);
    }
    // coarse past the cap
    nrt_wrset s{}; int went = 0; for (uint32_t i = 0; i < NRT_WR_MAX + 10; i++) went += nrt_wr_add(&s, i * 4, i * 4 + 1, 1);
    CHECK(went == 1 && s.coarse && nrt_wr_meets(&s, 1, 2, nullptr) == 1, "coarse past the cap (went %d, coarse %d)", went, s.coarse); nrt_wr_free(&s);
    printf("B exact: %llu queries (%llu meet a written byte) on 16 random write histories: meets == byte bitmap, newest serial >= the bytes', coarse past %d ranges\n",
           (unsigned long long)queries, (unsigned long long)meets, int(NRT_WR_MAX));
}
void bench() {
    std::mt19937_64 R(3); nrt_wrset s{};
    const int n = 2000; const uint64_t t0 = now_ns();
    for (int i = 0; i < n; i++) { const uint32_t a = uint32_t(R() % (60u << 20)); nrt_wr_add(&s, a, a + 64 + uint32_t(R() % 192), uint64_t(i)); }
    const double add_ns = double(now_ns() - t0) / n;
    uint64_t acc = 0; const int q = 2000000; const uint64_t t1 = now_ns();
    for (int i = 0; i < q; i++) { const uint32_t a = uint32_t(R() % (60u << 20)); uint64_t nw; acc += nrt_wr_meets(&s, a, a + 2048, &nw); }
    const double meet_ns = double(now_ns() - t1) / q; g_sink += acc;
    printf("B bench: %u ranges in a chunk: add %.1f ns, meets (2 KB view) %.1f ns\n", s.n, add_ns, meet_ns);
    nrt_wr_free(&s);
}
}

// ============ C: state-group refresh (nb_pipe.h nbp_refresh): what it costs a draw, and two exact speed-ups tried ============
// (1) recent ids compared before the hash (MRU of 4) and (2) a 4-lane hash instead of the one dependent multiply chain. Both give the
// same ids by construction (the dictionary interns by memcmp; ids are handed out in first-seen order, the hash only picks probe slots).
namespace C {
enum { S_VFMT = 0, S_BLEND = 192, S_COLOR = 256, S_POLY = 276, S_DS = 280, S_BC = 296, S_STEN = 324, S_MISC = 360, S_ALPHA = 392, S_END = 400 };
static nbp_span sp(size_t a, size_t b, size_t c = 0, size_t d = 0) { nbp_span s{}; s.off[0] = uint16_t(a); s.len[0] = uint16_t(b - a); s.off[1] = uint16_t(c); s.len[1] = uint16_t(d - c); return s; }
static void spans(nbp_span out[NBP_GROUPS]) {   // sizes as the engine's State (nvn_obj.h): vertex format 192 B, blend 80, colour 24, polygon 16, depth-stencil 44, misc 32
    out[NBP_G_BLEND] = sp(S_BLEND, S_COLOR, S_BC, S_BC + 16); out[NBP_G_COLOR] = sp(S_COLOR, S_POLY, S_ALPHA, S_END); out[NBP_G_POLY] = sp(S_POLY, S_DS, S_BC + 16, S_STEN);
    out[NBP_G_DS] = sp(S_DS, S_BC, S_STEN, S_MISC); out[NBP_G_VFMT] = sp(S_VFMT, S_BLEND); out[NBP_G_MISC] = sp(S_MISC, S_ALPHA);
}
static uint64_t hash1(const uint8_t *p, uint32_t n) { return nbp_hash(p, n); }
static uint64_t hash4(const uint8_t *p, uint32_t n) {   // 4 independent lanes of 8-byte words
    uint64_t a = 0x9E3779B97F4A7C15ull ^ n, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull; uint32_t i = 0;
    for (; i + 32 <= n; i += 32) { uint64_t w[4]; memcpy(w, p + i, 32); a = (a ^ w[0]) * 0xff51afd7ed558ccdull; b = (b ^ w[1]) * 0xff51afd7ed558ccdull; c = (c ^ w[2]) * 0xff51afd7ed558ccdull; d = (d ^ w[3]) * 0xff51afd7ed558ccdull; a ^= a >> 29; b ^= b >> 29; c ^= c >> 29; d ^= d >> 29; }
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); a = (a ^ w) * 0xff51afd7ed558ccdull; a ^= a >> 29; }
    for (; i < n; i++) a = (a ^ p[i]) * 0x100000001b3ull;
    uint64_t h = a ^ (b * 0x9E3779B97F4A7C15ull) ^ (c * 0xC2B2AE3D27D4EB4Full) ^ (d * 0x165667B19E3779F9ull);
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33; return h;
}
// nbp_dict_get / nbp_refresh of the header with the hash as a parameter, + the MRU variant (m = null: off)
typedef uint64_t (*HashFn)(const uint8_t *, uint32_t);
static int dict_get(nbp_dict *d, const uint8_t *p, int *fresh, HashFn H) {
    const uint64_t h = H(p, d->size); uint32_t k = (uint32_t)(h >> 40) & (NBP_DHASH - 1);
    for (;; k = (k + 1) & (NBP_DHASH - 1)) { const uint16_t t = d->tab[k]; if (!t) break; if (d->id[t - 1].h == h && !memcmp(d->arena + (size_t)(t - 1) * d->size, p, d->size)) { *fresh = 0; return t - 1; } }
    if (d->nid >= NBP_IDS) return -1;
    d->id[d->nid].h = h; memcpy(d->arena + (size_t)d->nid * d->size, p, d->size); d->tab[k] = (uint16_t)(++d->nid); *fresh = 1; return (int)d->nid - 1;
}
struct Mru { uint16_t n[NBP_GROUPS], v[NBP_GROUPS][4]; };
static void refresh(nbp_groups *g, const uint8_t *base, HashFn H, Mru *m) {
    for (uint32_t msk = g->dirty; msk; msk &= msk - 1) {
        const int k = __builtin_ctz(msk); g->st[NBP_S_REFRESH]++;
        if ((g->valid >> k & 1) && nbp_group_same(g, k, base)) { g->st[NBP_S_SAME]++; continue; }
        const nbp_span *s = &g->sp[k]; uint32_t at = 0;
        for (int i = 0; i < NBP_SPANS; i++) { memcpy(g->cur[k] + at, base + s->off[i], s->len[i]); at += s->len[i]; }
        int fresh = 0, id = -1;
        if (m) for (unsigned r = 0; r < m->n[k]; r++) if (!memcmp(g->d[k].arena + (size_t)m->v[k][r] * g->d[k].size, g->cur[k], g->d[k].size)) { id = m->v[k][r]; break; }
        if (id < 0) id = dict_get(&g->d[k], g->cur[k], &fresh, H);
        if (m) { unsigned r = 0; while (r < m->n[k] && m->v[k][r] != id) r++; if (r == m->n[k] && m->n[k] < 4) m->n[k]++; if (r >= m->n[k]) r = m->n[k] - 1; for (; r > 0; r--) m->v[k][r] = m->v[k][r - 1]; m->v[k][0] = uint16_t(id); }
        if (fresh) g->st[NBP_S_NEWID]++;
        g->gid[k] = (uint32_t)id; g->valid |= 1u << k;
    }
    g->dirty = 0;
}
struct Mat { uint8_t v[NBP_GROUPS]; };
struct Run { std::vector<uint32_t> gid; double ns; uint64_t refresh, same; };
// variant: 0 writes only, 1 the header's nbp_refresh, 2 the copy with nbp_hash (= 1), 3 + MRU, 4 4-lane hash
static Run run(int variant, const std::vector<Mat> &mats, const std::vector<uint16_t> &order, const std::vector<uint8_t> &extra, const std::vector<std::vector<uint8_t>> &bytes, bool record) {
    nbp_span s[NBP_GROUPS]; spans(s);
    static nbp_groups G; nbp_groups_init(&G, s); Mru mru{};
    std::vector<uint8_t> st(S_END + 64, 0); Run r{}; if (record) r.gid.reserve(order.size() * NBP_GROUPS);
    int cur[NBP_GROUPS]; for (int g = 0; g < NBP_GROUPS; g++) cur[g] = -1;
    const uint64_t t0 = now_ns();
    for (size_t d = 0; d < order.size(); d++) {
        const Mat &m = mats[order[d]];
        for (int g = 0; g < NBP_GROUPS; g++) if (cur[g] != m.v[g]) {   // the state record's bytes into the state (execute())
            const uint8_t *b = bytes[size_t(g) * 16 + m.v[g]].data(); uint32_t at = 0;
            for (int i = 0; i < NBP_SPANS; i++) { memcpy(st.data() + s[g].off[i], b + at, s[g].len[i]); at += s[g].len[i]; }
            cur[g] = m.v[g]; G.dirty |= 1u << g; }
        G.dirty |= extra[d];
        if (variant == 1) nbp_refresh(&G, st.data()); else if (variant == 2) refresh(&G, st.data(), hash1, nullptr);
        else if (variant == 3) refresh(&G, st.data(), hash1, &mru); else if (variant == 4) refresh(&G, st.data(), hash4, nullptr); else G.dirty = 0;
        if (record) for (int g = 0; g < NBP_GROUPS; g++) r.gid.push_back(G.gid[g]); else g_sink += G.gid[NBP_G_VFMT];
    }
    r.ns = double(now_ns() - t0) / double(order.size()); r.refresh = G.st[NBP_S_REFRESH]; r.same = G.st[NBP_S_SAME];
    nbp_groups_free(&G); return r;
}
void test_and_bench() {
    std::mt19937_64 R(11);
    const unsigned var[NBP_GROUPS] = {6, 4, 3, 6, 10, 2}; std::vector<Mat> mats(70);   // 70 materials over 6 / 4 / 3 / 6 / 10 / 2 group variants
    for (auto &m : mats) for (int g = 0; g < NBP_GROUPS; g++) m.v[g] = uint8_t(R() % var[g]);
    nbp_span s[NBP_GROUPS]; spans(s); std::vector<std::vector<uint8_t>> bytes(NBP_GROUPS * 16);
    for (int g = 0; g < NBP_GROUPS; g++) for (int v = 0; v < 16; v++) { auto &b = bytes[size_t(g) * 16 + v]; b.resize(nbp_gsize(&s[g])); for (size_t j = 0; j < b.size(); j++) b[j] = uint8_t(j % 3 ? 0 : R()); }   // mostly zero, as State
    const size_t draws = 600000;
    for (int pat = 0; pat < 2; pat++) {
        std::vector<uint16_t> order(draws); std::vector<uint8_t> extra(draws);
        for (size_t d = 0; d < draws; d++) {
            order[d] = pat == 0 ? uint16_t(R() % mats.size()) : uint16_t(((d / 6) * 7 + (R() % 3 == 0 ? R() % 5 : 0)) % mats.size());   // random / material runs
            extra[d] = uint8_t((R() % 100 < 95) ? (1u << (R() % NBP_GROUPS)) : 0);   // a record that rewrote a group with its bytes (0.95 a draw)
        }
        const Run a = run(1, mats, order, extra, bytes, true);
        for (int v = 2; v <= 4; v++) { const Run b = run(v, mats, order, extra, bytes, true); CHECK(a.gid == b.gid, "pattern %d variant %d: group ids differ from nbp_refresh", pat, v); }
        double best[5] = {1e18, 1e18, 1e18, 1e18, 1e18};
        for (int rep = 0; rep < 5; rep++) for (int v = 0; v <= 4; v++) best[v] = std::min(best[v], run(v, mats, order, extra, bytes, false).ns);
        printf("C %s order: %.2f groups re-read a draw (%.2f changed); refresh a draw: header %.1f ns (copy %.1f), + recent ids %.1f, 4-lane hash %.1f (ids equal in all)\n",
               pat ? "material-run" : "random", double(a.refresh) / draws, double(a.refresh - a.same) / draws, best[1] - best[0], best[2] - best[0], best[3] - best[0], best[4] - best[0]);
    }
}
}

// ============ D: NB-MT packets on the translate thread ============
namespace D {
void bench() {
    const int mode_list[3] = {0, 1, 3};
    for (int mi = 0; mi < 3; mi++) {
        char m[4]; snprintf(m, sizeof m, "%d", mode_list[mi]); setenv_("DM_NB_MT", m); setenv_("DM_NB_MT_FLUSH", "256"); setenv_("DM_NB_MT_CHUNK_KB", "64"); setenv_("DM_NB_MT_QUEUE", "64");
        nbmt::init();
        std::mt19937 R(5); double best = 1e18; uint64_t packets = 0;
        VkDescriptorSet set = (VkDescriptorSet)(uintptr_t)0x1234; VkBuffer vb = (VkBuffer)(uintptr_t)0x55, ib = (VkBuffer)(uintptr_t)0x66;
        for (int rep = 0; rep < 3; rep++) {
            Log log; log.b.reserve(64u << 20); VkCommandBuffer cb = reinterpret_cast<VkCommandBuffer>(&log);
            nbmt::begin(cb, true, false);
            uint64_t n = 0; const int draws = 20000; const uint64_t t0 = now_ns();
            for (int d = 0; d < draws; d++) {
                const unsigned r = R() % 100;
                if (r < 56) { nbmt::CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)(0x100 + (d & 7))); ++n; }
                if (r % 100 < 79) { const uint32_t off[5] = {uint32_t(d) * 256u, 0, uint32_t(d) * 512u, 4096, uint32_t(d) * 64u}; nbmt::CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipelineLayout)(uintptr_t)0x77, 0, 1, &set, 5, off); ++n; }
                if ((r * 7) % 100 < 91) { const VkDeviceSize o = VkDeviceSize(d) * 48; nbmt::CmdBindVertexBuffers(cb, 0, 1, &vb, &o); ++n; }
                if ((r * 13) % 100 < 53) { nbmt::CmdBindIndexBuffer(cb, ib, 0, VK_INDEX_TYPE_UINT16); ++n; }
                nbmt::CmdDrawIndexed(cb, 300, 1, uint32_t(d) * 3u, int32_t(d), 0); ++n;
            }
            const double ns = double(now_ns() - t0) / double(n);   // the translate thread up to the submission's end (mode 3: producer only)
            nbmt::end(cb); if (ns < best) best = ns; packets = n;
            g_sink += log.b.size();
        }
        printf("D NB-MT DM_NB_MT=%d: %.1f ns a vkCmd* on the translate thread (served-draw mix, %.2f packets a draw; the mock's own logging included in all three)\n",
               mode_list[mi], best, double(packets) / 20000.0);
        nbmt::stop();
    }
}
}

// ============ E: sticky pages, stable written ranges + the replay-write guard (frames in flight) ============
// E1 nrt_sv_conflict == a plain list of every noted serve (serial in [floor, now], same slot + gen, overlapping), ring wrap, slot reuse.
// E2 nrt_sticky_class == byte bitmaps (written bytes with their last serial, hot bytes).
// E3 a replay model with the engine's two frame slots (zc::trx_floor / trx_guard / trx_sticky_ok / mark as in xbg_game.cpp): draws serve
//    views (in place or copy), replay CPU writes land after the guard, submissions go in flight, the GPU finishes them in order at random
//    times, fences are waited (wait_fence), gpu_sync. Every in-place view a draw reads is checked: no byte of it may be written between the
//    serve and the GPU's end of that submission. 0 stale reads with the guard; each mutation of the guard must give stale reads.
// E4 bench: note / conflict cost at Fort Joy rates.
namespace E {
static nrt_svring *ring_new() { nrt_svring *r = (nrt_svring *)calloc(1, sizeof(nrt_svring)); return r; }
static void ring_del(nrt_svring *r) { nrt_sv_free(r); free(r); }

void sv_exact(std::mt19937_64 &R) {
    nrt_svring *r = ring_new();
    struct Ref { uint64_t serial; int slot; uint64_t gen; uint32_t lo, hi; };
    std::vector<Ref> ref; uint64_t queries = 0, conf = 0; uint64_t gens[8] = {1, 2, 3, 4, 5, 6, 7, 8}; uint64_t next_gen = 9;
    uint64_t now = 2;
    for (int step = 0; step < 60000; step++) {
        const int op = int(R() % 100);
        if (op < 3) { ++now; continue; }                                           // a submission
        if (op < 4) { const int s = int(R() % 8); gens[s] = next_gen++; continue; } // slot reused by a new chunk
        const int slot = int(R() % 8); const uint32_t span = (R() % 4) ? (1u << 20) : (70u << 20);   // offsets past 64 MB share the last block
        if (op < 70) {
            const uint32_t len = 1 + uint32_t(R() % (R() % 8 ? 8192 : 300000)); const uint32_t lo = uint32_t(R() % (span - len));
            nrt_sv_note(r, now, slot, gens[slot], lo, lo + len); ref.push_back({now, slot, gens[slot], lo, lo + len});
            if (R() % 4 == 0) { nrt_sv_note(r, now, slot, gens[slot], lo, lo + len); ref.push_back({now, slot, gens[slot], lo, lo + len}); }   // a repeat
        } else {
            const uint32_t len = 1 + uint32_t(R() % (R() % 8 ? 4096 : 200000)); const uint32_t a = uint32_t(R() % (span - len));
            const uint64_t floor = now - std::min<uint64_t>(now, uint64_t(R() % 4));   // the engine's floor is now - 2 .. now; the ring holds 4
            bool want = false; uint64_t wv = 0;
            for (const Ref &e : ref) if (e.serial >= floor && e.serial <= now && e.slot == slot && e.gen == gens[slot] && e.lo < a + len && e.hi > a) { want = true; wv = std::max(wv, e.serial); }
            uint64_t v = 0; const int got = nrt_sv_conflict(r, floor, now, slot, gens[slot], a, a + len, &v); ++queries; conf += got;
            CHECK(got == int(want), "E1 conflict [%u,+%u) slot %d floor %llu now %llu: ring %d, list %d", a, len, slot, (unsigned long long)floor, (unsigned long long)now, got, int(want));
            if (got && want) CHECK(v == wv, "E1 latest submission %llu, list %llu", (unsigned long long)v, (unsigned long long)wv);
        }
        if (ref.size() > 20000) ref.erase(std::remove_if(ref.begin(), ref.end(), [&](const Ref &e) { return e.serial + 4 <= now; }), ref.end());
    }
    // "over" (an entry not stored): every write to a touched block conflicts
    nrt_svring *o = ring_new(); nrt_sv_note(o, 5, 1, 7, 100, 200); o->g[5 % NRT_SV_RING].over = 1;
    CHECK(nrt_sv_conflict(o, 4, 5, 1, 7, 60000, 60001, nullptr) == 1, "E1 over: a write to a touched block conflicts");
    CHECK(nrt_sv_conflict(o, 4, 5, 1, 7, 70000, 70001, nullptr) == 0, "E1 over: an untouched block does not");
    CHECK(nrt_sv_conflict(o, 6, 6, 1, 7, 100, 101, nullptr) == 0, "E1 a finished submission does not");
    ring_del(o);
    printf("E1 exact: %llu write queries (%llu meet a served view) against the list of every serve: equal, latest submission equal\n", (unsigned long long)queries, (unsigned long long)conf);
    ring_del(r);
}

void class_exact(std::mt19937_64 &R) {
    const uint32_t N = 1u << 20; std::vector<uint64_t> ser(N); std::vector<uint8_t> hot(N);
    uint64_t q = 0, k[NRT_CLS_N] = {};
    for (int t = 0; t < 8; t++) {
        nrt_wrset wr{}, ho{}; std::fill(ser.begin(), ser.end(), 0); std::fill(hot.begin(), hot.end(), 0);
        uint64_t now = 1; const uint64_t age = 1 + R() % 9;
        for (int m = 0; m < 1500; m++) {
            if (R() % 5 == 0) ++now;
            const uint32_t len = R() % 6 ? 16 + uint32_t(R() % 512) : 1 + uint32_t(R() % 40000); uint32_t a = uint32_t(R() % N); if (a + len > N) a = N - len;
            nrt_wr_add(&wr, a, a + len, now); for (uint32_t i = a; i < a + len; i++) ser[i] = now;
            if (R() % 10 == 0) { nrt_wr_add(&ho, a, a + len, now); for (uint32_t i = a; i < a + len; i++) hot[i] = 1; }
            for (int j = 0; j < 4; j++) {
                const uint32_t ql = 1 + uint32_t(R() % 20000); uint32_t qa = uint32_t(R() % N); if (qa + ql > N) qa = N - ql;
                bool any = false, anyhot = false; uint64_t nw = 0; for (uint32_t i = qa; i < qa + ql; i++) if (ser[i]) { any = true; nw = std::max(nw, ser[i]); if (hot[i]) anyhot = true; }
                const int got = nrt_sticky_class(&wr, &ho, qa, qa + ql, now, age); ++q; ++k[got];
                // merged ranges keep the latest serial of what they merged: a view can only look younger than its bytes (copy: safe side)
                if (!any) CHECK(got == NRT_CLS_NEVER, "E2 never-written view got class %d", got);
                else if (nw + age > now) CHECK(got == NRT_CLS_RECENT, "E2 recently written view got class %d", got);
                else if (anyhot) CHECK(got == NRT_CLS_HOT || got == NRT_CLS_RECENT, "E2 hot view got class %d", got);
                else CHECK(got != NRT_CLS_NEVER && got != NRT_CLS_HOT, "E2 stable view got class %d", got);
                if (got == NRT_CLS_STABLE) CHECK(any && !anyhot && nw + age <= now, "E2 STABLE only when every met byte is old and none hot");
            }
        }
        nrt_wr_free(&wr); nrt_wr_free(&ho);
    }
    printf("E2 exact: %llu views: never %llu, stable %llu, recent %llu, hot %llu; STABLE only when every met written byte is >= age old and not hot\n",
           (unsigned long long)q, (unsigned long long)k[NRT_CLS_NEVER], (unsigned long long)k[NRT_CLS_STABLE], (unsigned long long)k[NRT_CLS_RECENT], (unsigned long long)k[NRT_CLS_HOT]);
}

// ---- E3: the replay model ----
enum Mut { M_NONE = 0, M_NO_GUARD, M_FLOOR_UP, M_SYNC_AS_WAIT, M_NO_NOTE, M_NO_HOT_AGE1, M_N };
static const char *mut_name[M_N] = {"guard", "no guard", "floor one too high (slot_serial-2 ignored)", "recording submission only waited, not submitted", "serves not noted", "no hot + age 1"};
struct Model {
    static const int K = 4; static const uint32_t CH = 1u << 20;   // 4 chunks of 1 MB (16 blocks of 64 KB)
    int mut; uint64_t age; std::mt19937_64 R; bool base_guard = false;   // base_guard: DM_NR_TRX_STICKY=3 (base-path views noted in svb and guarded)
    struct Sl { bool recording = false, pending = false; uint64_t serial = 0; } sl[2];
    uint64_t slot_serial = 2, gpu_done = 1, submitted = 1; int slot_index = 0;
    struct Ch { uint64_t gen = 0; std::vector<uint32_t> ver; std::vector<uint8_t> sticky; nrt_wrset wr{}, hot{}; std::vector<std::pair<uint32_t, uint32_t>> mesh; } ch[K];
    uint64_t next_gen = 1, wid = 0;
    nrt_svring *sv, *svb;
    struct Read { uint64_t serial; int c; uint64_t gen; uint32_t lo, hi; uint32_t ver; bool base; };
    std::vector<Read> reads;
    uint64_t serves = 0, inplace = 0, copies = 0, cls[NRT_CLS_N] = {}, writes = 0, stale = 0, base_stale = 0, syncs = 0, waits = 0, busy = 0, hot_adds = 0, demote_bad = 0, completes = 0;
    Model(int m, uint64_t a, uint64_t seed) : mut(m), age(a), R(seed) { sv = ring_new(); svb = ring_new(); for (int i = 0; i < K; i++) fresh(i); }
    ~Model() { ring_del(sv); ring_del(svb); for (auto &c : ch) { nrt_wr_free(&c.wr); nrt_wr_free(&c.hot); } }
    void fresh(int i) {   // map a new chunk into slot i (a retired chunk's in-flight reads keep reading its old AHB: they can no longer go stale)
        Ch &c = ch[i]; c.gen = next_gen++; c.ver.assign(CH, 0); c.sticky.assign(CH / 4096, 0); nrt_wr_free(&c.wr); nrt_wr_free(&c.hot); c.mesh.clear();
        uint32_t at = uint32_t(R() % 4096);
        while (at < CH - 70000) { const uint32_t len = R() % 5 ? 64 + uint32_t(R() % 6000) : 8192 + uint32_t(R() % 60000); c.mesh.push_back({at, at + len}); at += len + (R() % 3 ? 0 : uint32_t(R() % 3000)); }
        reads.erase(std::remove_if(reads.begin(), reads.end(), [&](const Read &r) { return r.c == i; }), reads.end());
    }
    uint32_t maxver(const Ch &c, uint32_t lo, uint32_t hi) { uint32_t m = 0; for (uint32_t x = lo; x < hi; x++) m = std::max(m, c.ver[x]); return m; }
    void gpu_finish(uint64_t upto) {   // the GPU ends submissions in order: their reads happened; check them once more by version
        if (upto > submitted) upto = submitted;
        if (upto <= gpu_done) return;
        gpu_done = upto; ++completes;
        for (size_t i = 0; i < reads.size();) { const Read &r = reads[i]; if (r.serial <= gpu_done) { if (!r.base && maxver(ch[r.c], r.lo, r.hi) != r.ver) ++stale; reads[i] = reads.back(); reads.pop_back(); } else ++i; }
    }
    void retire(Sl &s) { if (!s.pending) return; gpu_finish(s.serial); s.pending = false; }
    void recording_slot() { Sl &s = sl[slot_index]; if (!s.recording) { retire(s); s.recording = true; s.serial = slot_serial; } }
    void submit(bool wait) {
        Sl &s = sl[slot_index];
        if (s.recording) { s.recording = false; s.pending = true; submitted = s.serial; slot_index ^= 1; ++slot_serial; }
        if (wait) { retire(sl[slot_index]); retire(sl[slot_index ^ 1]); }
    }
    void wait_fence() { Sl &older = sl[slot_index].pending ? sl[slot_index] : sl[slot_index ^ 1]; retire(older); }
    uint64_t floor_() { uint64_t f = slot_serial; if (sl[slot_index ^ 1].pending) f = slot_serial - 1; if (sl[slot_index].pending) f = slot_serial - 2; return mut == M_FLOOR_UP ? f + 1 : f; }
    // zc::view for a vertex / index view [lo, hi) of chunk i (the page loop + trx_sticky_ok)
    void serve(int i, uint32_t lo, uint32_t hi) {
        Ch &c = ch[i]; ++serves; bool sticky = false;
        for (uint32_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) if (c.sticky[pg]) { sticky = true; break; }
        if (!sticky) { ++inplace; if (base_guard) nrt_sv_note(svb, slot_serial, i, c.gen, lo, hi); reads.push_back({slot_serial, i, c.gen, lo, hi, maxver(c, lo, hi), !base_guard}); return; }   // the base zero-copy path (guarded only at 3)
        const int k = nrt_sticky_class(&c.wr, &c.hot, lo, hi, slot_serial, age); ++cls[k];
        if (k != NRT_CLS_NEVER && k != NRT_CLS_STABLE) { ++copies; return; }
        if (mut != M_NO_NOTE) nrt_sv_note(sv, slot_serial, i, c.gen, lo, hi);
        ++inplace; reads.push_back({slot_serial, i, c.gen, lo, hi, maxver(c, lo, hi), false});
    }
    // zc::mark (trx_guard first) + the write itself
    void write(int i, uint32_t a, uint32_t b) {
        Ch &c = ch[i]; ++writes;
        if (mut != M_NO_GUARD) {
            uint64_t v = 0; int hit = nrt_sv_conflict(sv, floor_(), slot_serial, i, c.gen, a, b, &v);
            if (base_guard) { uint64_t bv = 0; if (nrt_sv_conflict(svb, floor_(), slot_serial, i, c.gen, a, b, &bv)) { hit = 1; v = std::max(v, bv); } }
            if (hit) {
                if (mut != M_NO_HOT_AGE1) { nrt_wr_add(&c.hot, a, b, slot_serial); ++hot_adds; }
                const int plan = nrt_guard_plan(hit, v, slot_serial);
                if (plan == NRT_GUARD_SYNC && mut != M_SYNC_AS_WAIT) { ++syncs; submit(true); }
                else { ++waits; Sl &old = sl[slot_index], &prev = sl[slot_index ^ 1]; const bool need_prev = v + 1 >= slot_serial;
                    if ((old.pending && old.serial > gpu_done) || (need_prev && prev.pending && prev.serial > gpu_done)) ++busy;
                    if (old.pending) retire(old); if (need_prev && prev.pending) retire(prev); }
            }
        }
        for (uint32_t pg = a >> 12; pg <= (b - 1) >> 12; pg++) c.sticky[pg] = 1;
        nrt_wr_add(&c.wr, a, b, slot_serial);
        ++wid; for (uint32_t x = a; x < b; x++) c.ver[x] = uint32_t(wid);
        // the GPU must not see it in any read not finished yet
        for (const Read &r : reads) if (r.c == i && r.gen == c.gen && r.serial > gpu_done && r.lo < b && r.hi > a) { if (r.base) ++base_stale; else ++stale; }
        reads.erase(std::remove_if(reads.begin(), reads.end(), [&](const Read &r) { return r.c == i && r.serial > gpu_done && r.lo < b && r.hi > a; }), reads.end());   // counted once
        // a write onto a view demotes it at once: the next serve of those bytes copies
        const int k = nrt_sticky_class(&c.wr, &c.hot, a, b, slot_serial, age); if (k == NRT_CLS_NEVER || k == NRT_CLS_STABLE) ++demote_bad;
    }
    void run(int steps) {
        for (int s = 0; s < steps; s++) {
            const int op = int(R() % 1000);
            if (op < 560) {   // a draw: its vertex / index views (recording_slot before or after the snapshot, as the old tail / lane do)
                const bool before = R() % 2; if (before) recording_slot();
                const int i = int(R() % K); Ch &c = ch[i];
                uint32_t lo, hi; if (R() % 8) { const auto &m = c.mesh[size_t(R() % c.mesh.size())]; lo = m.first; hi = m.second; } else { const uint32_t len = 16 + uint32_t(R() % 9000); lo = uint32_t(R() % (CH - len)); hi = lo + len; }
                serve(i, lo, hi); if (!before) recording_slot();
            } else if (op < 700) {   // a replay CPU write (COPY_B2B / UPDATE_UB / fill): onto a mesh (an upload / rewrite), next to one, or anywhere
                const int i = int(R() % K); Ch &c = ch[i]; uint32_t a, b; const int w = int(R() % 10);
                if (w < 4) { const auto &m = c.mesh[size_t(R() % std::min<size_t>(c.mesh.size(), 40))]; a = m.first; b = m.second; }       // a small set of meshes the game rewrites
                else if (w < 6) { const auto &m = c.mesh[size_t(R() % c.mesh.size())]; a = m.first; b = m.second; if (R() % 2) { const uint32_t cut = (b - a) / 2; a += cut; } }
                else if (w < 8) { const auto &m = c.mesh[size_t(R() % c.mesh.size())]; a = m.second; b = std::min(CH, a + 16 + uint32_t(R() % 256)); }   // the neighbour bytes on the page
                else { const uint32_t len = 16 + uint32_t(R() % 3000); a = uint32_t(R() % (CH - len)); b = a + len; }
                write(i, a, b);
            } else if (op < 760) submit(false);
            else if (op < 880) gpu_finish(gpu_done + 1);
            else if (op < 890) wait_fence();
            else if (op < 892) submit(true);
            else if (op < 893) fresh(int(R() % K));
        }
        submit(true);
    }
};

void model() {
    const uint64_t ages[3] = {8, 2, 1};
    for (uint64_t age : ages) {
        Model m(M_NONE, age, 1000 + age); m.run(250000);
        CHECK(m.stale == 0, "E3 age %llu: %llu stale in-place reads with the guard", (unsigned long long)age, (unsigned long long)m.stale);
        CHECK(m.demote_bad == 0, "E3 age %llu: %llu writes left their bytes servable", (unsigned long long)age, (unsigned long long)m.demote_bad);
        CHECK(m.cls[NRT_CLS_STABLE] > 1000 && m.syncs + m.waits > 0, "E3 age %llu: the model reaches the new class (%llu) and the guard (%llu)", (unsigned long long)age, (unsigned long long)m.cls[NRT_CLS_STABLE], (unsigned long long)(m.syncs + m.waits));
        printf("E3 model age %llu: %llu draws' views (%llu in place, %llu copied; sticky-page classes never %llu / stable %llu / recent %llu / hot %llu), %llu replay writes, "
               "guard: %llu submit+wait, %llu waits (%llu on a busy fence), %llu hot ranges; GPU ends %llu; stale in-place reads %llu (base zero-copy path, first write onto a never-written page: %llu)\n",
               (unsigned long long)age, (unsigned long long)m.serves, (unsigned long long)m.inplace, (unsigned long long)m.copies, (unsigned long long)m.cls[NRT_CLS_NEVER], (unsigned long long)m.cls[NRT_CLS_STABLE],
               (unsigned long long)m.cls[NRT_CLS_RECENT], (unsigned long long)m.cls[NRT_CLS_HOT], (unsigned long long)m.writes, (unsigned long long)m.syncs, (unsigned long long)m.waits, (unsigned long long)m.busy,
               (unsigned long long)m.hot_adds, (unsigned long long)m.completes, (unsigned long long)m.stale, (unsigned long long)m.base_stale);
    }
    {   // DM_NR_TRX_STICKY=3: the base path's in-place views are guarded too -> no stale read at all
        Model m(M_NONE, 8, 4242); m.base_guard = true; m.run(250000);
        CHECK(m.stale == 0 && m.base_stale == 0, "E3 STICKY=3: %llu stale reads", (unsigned long long)m.stale);
        printf("E3 model STICKY=3 (base views guarded too): %llu views (%llu in place), %llu writes, guard %llu submit+wait / %llu waits (%llu busy); stale in-place reads %llu (base path included)\n",
               (unsigned long long)m.serves, (unsigned long long)m.inplace, (unsigned long long)m.writes, (unsigned long long)m.syncs, (unsigned long long)m.waits, (unsigned long long)m.busy, (unsigned long long)m.stale);
    }
    for (int mu = 1; mu < M_N; mu++) {
        uint64_t st = 0; for (int r = 0; r < 3 && !st; r++) { Model m(mu, mu == M_NO_HOT_AGE1 ? 1 : 8, 77 + mu * 13 + r); m.run(250000); st += m.stale; }
        if (mu == M_NO_HOT_AGE1) { CHECK(st == 0, "E3 mutation '%s' is still exact (hot only limits the waits): %llu stale", mut_name[mu], (unsigned long long)st); printf("E3 variant '%s': %llu stale (hot is a cost limit, not part of the proof)\n", mut_name[mu], (unsigned long long)st); }
        else { CHECK(st > 0, "E3 mutation '%s' not caught", mut_name[mu]); printf("E3 mutation '%s': %llu stale reads (caught)\n", mut_name[mu], (unsigned long long)st); }
    }
}

void bench() {
    std::mt19937_64 R(9); nrt_svring *r = ring_new();
    // Fort Joy: ~300 (walking, nrB_tex) .. ~1300 (play-1118 heavy) in-place sticky serves a frame over 22 chunks, ~25 .. 76 replay writes a frame
    const int frames = 400, serves = 1300, writes = 76; std::vector<std::pair<uint32_t, uint32_t>> mesh; for (int i = 0; i < 3000; i++) { const uint32_t lo = uint32_t(R() % (60u << 20)); mesh.push_back({lo, lo + 64 + uint32_t(R() % 8000)}); }
    uint64_t tn = 0, tc = 0, hits = 0;
    for (int f = 0; f < frames; f++) {
        const uint64_t now = 10 + uint64_t(f);
        const uint64_t t0 = now_ns(); for (int s = 0; s < serves; s++) { const auto &m = mesh[size_t(R() % mesh.size())]; nrt_sv_note(r, now, int(R() % 22), 1, m.first, m.second); } tn += now_ns() - t0;
        const uint64_t t1 = now_ns(); for (int w = 0; w < writes; w++) { const uint32_t a = uint32_t(R() % (60u << 20)); uint64_t v; hits += nrt_sv_conflict(r, now - 2, now, int(R() % 22), 1, a, a + 6000, &v); } tc += now_ns() - t1;
    }
    printf("E4 bench: note %.1f ns a serve, conflict %.1f ns a write (%d serves + %d writes a frame, 3 submissions in flight; %llu writes met a block with serves)\n",
           double(tn) / (double(frames) * serves), double(tc) / (double(frames) * writes), serves, writes, (unsigned long long)hits);
    ring_del(r);
}
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::mt19937_64 R(20261118);
    A::exact(R); A::bench();
    B::exact(R); B::bench();
    C::test_and_bench();
    D::bench();
    E::sv_exact(R); E::class_exact(R); E::model(); E::bench();
    printf("%s: nr_trx_test %llu checks, %d failed\n", fails ? "FAIL" : "PASS", (unsigned long long)checks, fails);
    return fails ? 1 : 0;
}
