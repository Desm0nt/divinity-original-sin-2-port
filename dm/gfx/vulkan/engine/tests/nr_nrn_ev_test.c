// [DOS2] NR-NATIVE step E (DM_NRN_EV, docs/dos2/nr/NR-NATIVE-E.md): host test of dm/gfx/nr/nr_nrn_ev.h - the event journal, the coverage,
// the handle epochs, the ring and the verdicts of the verify.
//   1 units        the journal (a write raises every range it touches, the huge-range path, a slot never shrinks), the coverage against
//                  a plain list (overlap eviction, a reader that meets a writer, the full table), handles (fresh epochs, generations),
//                  the ring (the wrap rule of nr_ubo.h)
//   2 equivalence  a model of the engine - buffers created with or without data over a pool whose ranges are handed out again,
//                  destroyed, updated by GPU copy (announced when the record is written, the bytes written when the "translation
//                  thread" executes it), copied into on the CPU, a scratch buffer written with no event, neighbours that share a page
//                  and are written with no event - against a model of the page tracker (a generation a page) and the truth (the
//                  bytes). Two version sources of every ask of a range:
//                    the events: one (range, epoch) never stands for two contents; a range not wholly inside a live non-scratch
//                    buffer has no epoch; the verify of =2 over the same stream reports 0 BAD, and "neighbour" exactly for the unannounced
//                    writes to other bytes of a shared page;
//                    the game thread's handle epoch: one (handle, epoch) never stands for two contents AS RECORDED.
//   3 the verify catches it   the same model with a write into a covered buffer that no event announces: BAD is reported.
// Mutations -DNRNE_MUTATE=1..10 (nr_nrn_ev.h) - each must fail this test.
// Build: clang -std=c11 -O2 -Wall -Wextra nr_nrn_ev_test.c -o nr_nrn_ev_test
#include <stdio.h>
#include <stdlib.h>
#include "../../../nr/nr_nrn_ev.h"

static unsigned long long g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { if (g_fail++ < 20) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 16); }
static uint32_t rn(uint32_t n) { return n ? rnd() % n : 0; }

// ---- 1 units ----
static void test_units(void) {
    nrne_j *j = (nrne_j *)calloc(1, sizeof *j);
    // a write raises the version of every range that shares a page with it, and of no range that shares no slot
    for (int it = 0; it < 4000; it++) {
        const uint64_t a = 0x40000000ull + rn(1 << 26), n = 1 + rn(it & 1 ? 1 << 20 : 5000);
        const uint64_t qa = a + (uint64_t)rn((uint32_t)n), qn = 1 + rn((uint32_t)(a + n - qa));   // a range inside the write
        const uint64_t before = nrne_j_ver(j, qa, qn), before_all = nrne_j_ver(j, a, n);
        const uint64_t s = nrne_j_write(j, a, n);
        CHECK(s > before && s > before_all);
        CHECK(nrne_j_ver(j, qa, qn) == s);
        CHECK(nrne_j_ver(j, a, n) == s);
        CHECK(nrne_j_ver(j, a + n - 1, 1) == s && nrne_j_ver(j, a, 1) == s);          // the first and the last byte
        const uint64_t wide = nrne_j_ver(j, a > 4096 ? a - 4096 : 0, n + 8192);       // a range around it
        CHECK(wide == s);
    }
    { const uint64_t s = nrne_j_write(j, 0x100000000ull, 1ull << 30);                  // 1 GB: every slot
      CHECK(nrne_j_ver(j, 0x100000000ull + 12345, 77) == s && nrne_j_ver(j, 0x7000, 1) == s); }
    { uint64_t slot = 0; nrne_max(&slot, 5); nrne_max(&slot, 3); CHECK(slot == 5); nrne_max(&slot, 9); CHECK(slot == 9); }   // a late writer with an older serial
    CHECK(nrne_j_ver(j, 0x5000, 0) == 0);
    free(j);

    // the coverage against a plain list
    nrne_cov *c = (nrne_cov *)calloc(1, sizeof *c);
    enum { M = 600 };
    static struct { uint64_t lo, hi, meta; int live; } ref[M];
    int nref = 0;
    for (int it = 0; it < 30000; it++) {
        const uint32_t op = rn(10);
        if (op < 5 && nref < M) {
            const uint64_t lo = 0x1000000ull + (uint64_t)rn(4000) * 0x400, hi = lo + 0x100 + (uint64_t)rn(64) * 0x100, meta = nrne_meta((int)rn(2), (int)rn(3), ((uint64_t)(1 + rn(1000)) << 32) | (uint32_t)it);
            for (int k = 0; k < nref; k++) if (ref[k].live && ref[k].lo < hi && ref[k].hi > lo) ref[k].live = 0;   // what it overlaps is dead
            CHECK(nrne_cov_add(c, lo, hi, meta) == 1);
            int k = 0; while (k < nref && ref[k].live) k++;
            if (k == nref) nref++;
            ref[k].lo = lo; ref[k].hi = hi; ref[k].meta = meta; ref[k].live = 1;
        } else if (op < 7 && nref) {
            const int k = (int)rn((uint32_t)nref);
            const int was = ref[k].live;
            const int did = rn(2) ? nrne_cov_del(c, ref[k].lo, ref[k].meta) : nrne_cov_del_handle(c, ref[k].meta);
            CHECK(did == was); ref[k].live = 0;
        } else {
            const uint64_t a = 0x1000000ull + rn(4000 * 0x400 + 0x8000), n = 1 + rn(0x900);
            nrne_ent e; const int got = nrne_cov_find(c, a, n, &e);
            int want = -1;
            for (int k = 0; k < nref; k++) if (ref[k].live && a >= ref[k].lo && a + n <= ref[k].hi) want = k;
            CHECK(got == (want >= 0));
            if (got && want >= 0) CHECK(e.lo == ref[want].lo && e.hi == ref[want].hi && e.meta == ref[want].meta);
        }
        if (!(it & 1023)) { uint64_t live = 0; for (int k = 0; k < nref; k++) live += (uint64_t)ref[k].live; CHECK(c->n == live); for (uint32_t i = 1; i < c->n; i++) CHECK(c->e[i - 1].hi <= c->e[i].lo); }
    }
    // a reader that meets a writer is not answered
    { int k = 0; while (k < nref && !ref[k].live) k++;
      if (k < nref) { nrne_ent e; CHECK(nrne_cov_find(c, ref[k].lo, 1, &e) == 1); c->seq++; CHECK(nrne_cov_find(c, ref[k].lo, 1, &e) == 0); c->seq++; CHECK(nrne_cov_find(c, ref[k].lo, 1, &e) == 1); } }
    // a new buffer over a dead entry whose destroy was never seen: the dead entry goes
    { memset(c, 0, sizeof *c);
      CHECK(nrne_cov_add(c, 0x2000, 0x6000, nrne_meta(0, 0, 1ull << 32 | 1)));
      CHECK(nrne_cov_add(c, 0x5000, 0x5800, nrne_meta(0, 0, 1ull << 32 | 2)));
      nrne_ent e; CHECK(nrne_cov_find(c, 0x2000, 0x100, &e) == 0); CHECK(nrne_cov_find(c, 0x5000, 0x100, &e) == 1 && e.hi == 0x5800); CHECK(c->n == 1); }
    // the full table: the range is simply not covered
    { memset(c, 0, sizeof *c);
      for (uint32_t i = 0; i < NRNE_CAP; i++) CHECK(nrne_cov_add(c, 0x100000ull + i * 0x200ull, 0x100000ull + i * 0x200ull + 0x100, nrne_meta(1, 0, 1ull << 32 | i)));
      CHECK(nrne_cov_add(c, 0x90000000ull, 0x90000100ull, nrne_meta(1, 0, 2ull << 32)) == 0);
      nrne_ent e; CHECK(nrne_cov_find(c, 0x90000000ull, 0x10, &e) == 0); CHECK(nrne_cov_find(c, 0x100000ull + 77 * 0x200ull, 0x100, &e) == 1); CHECK(c->full == 1); }
    free(c);

    // handles
    nrn_ev *v = (nrn_ev *)calloc(1, sizeof *v);
    const uint64_t h1 = 5ull << 32 | 100, h2 = 6ull << 32 | 100, h3 = 5ull << 32 | (100 + NRNE_H);   // h2: the index again with another generation; h3: another index of the slot
    CHECK(nrn_ev_buf_epoch(v, NRNE_K_VB, h1) == 0);
    const uint64_t s1 = nrn_ev_created(v, NRNE_K_VB, NRNE_C_STATIC, h1, 0x10000, 0x800);
    CHECK(s1 && nrn_ev_buf_epoch(v, NRNE_K_VB, h1) == s1 && nrn_ev_buf_epoch(v, NRNE_K_IB, h1) == 0 && nrn_ev_buf_epoch(v, NRNE_K_VB, h2) == 0);
    CHECK(nrn_ev_buf_epoch(v, NRNE_K_VB, 100) == 0);                                            // generation 0 = no object
    const uint64_t u1 = nrn_ev_updated(v, NRNE_K_VB, h1, 0x100, 0x40, 0);
    CHECK(u1 > s1 && nrn_ev_buf_epoch(v, NRNE_K_VB, h1) == u1);
    CHECK(nrn_ev_range_epoch(v, 0x10000, 0x800, NULL, NULL) != 0);
    { const uint64_t e0 = nrn_ev_range_epoch(v, 0x10000, 0x800, NULL, NULL); nrn_ev_updated(v, NRNE_K_VB, h1, 0, 0, 0);   // a GPU copy announced: the range's journal waits for the backend
      CHECK(nrn_ev_range_epoch(v, 0x10000, 0x800, NULL, NULL) == e0); nrn_ev_wrote(v, 0x10100, 0x40); CHECK(nrn_ev_range_epoch(v, 0x10000, 0x800, NULL, NULL) != e0); }
    CHECK(nrn_ev_updated(v, NRNE_K_VB, h2, 0, 0, 1) == 0);                                      // an unknown handle
    const uint64_t s2 = nrn_ev_created(v, NRNE_K_VB, NRNE_C_DYNAMIC, h2, 0x10000, 0x800);        // the index made again (no destroy seen)
    CHECK(s2 > u1 && nrn_ev_buf_epoch(v, NRNE_K_VB, h2) == s2 && nrn_ev_buf_epoch(v, NRNE_K_VB, h1) == 0);
    const uint64_t s3 = nrn_ev_created(v, NRNE_K_VB, NRNE_C_STATIC, h3, 0x20000, 0x800);         // pushes h2 out of the slot
    CHECK(nrn_ev_buf_epoch(v, NRNE_K_VB, h3) == s3 && nrn_ev_buf_epoch(v, NRNE_K_VB, h2) == 0);
    CHECK(nrn_ev_range_epoch(v, 0x10000, 0x800, NULL, NULL) != 0);                               // still covered: its buffer lives
    nrn_ev_destroyed(v, NRNE_K_VB, h2);                                                         // the handle table no longer knows h2: the coverage must still lose it
    CHECK(nrn_ev_range_epoch(v, 0x10000, 0x800, NULL, NULL) == 0);
    CHECK(nrn_ev_range_epoch(v, 0x20000, 0x800, NULL, NULL) != 0);
    nrn_ev_destroyed(v, NRNE_K_VB, h3);
    CHECK(nrn_ev_range_epoch(v, 0x20000, 0x800, NULL, NULL) == 0 && nrn_ev_buf_epoch(v, NRNE_K_VB, h3) == 0);
    // textures: by handle only
    const uint64_t t1 = 9ull << 32 | 7;
    CHECK(nrn_ev_tex_epoch(v, t1) == 0);
    const uint64_t ts = nrn_ev_created(v, NRNE_K_TEX, NRNE_C_STATIC, t1, 0, 0);
    CHECK(nrn_ev_tex_epoch(v, t1) == ts && v->cov.n == 0);
    const uint64_t tw = nrn_ev_updated(v, NRNE_K_TEX, t1, 0, 0, 0);
    CHECK(tw > ts && nrn_ev_tex_epoch(v, t1) == tw);
    // the ring: the head as nr_ubo.h moves it (aligned up, an allocation that does not fit starts at 0)
    { uint32_t head = 0; uint64_t wraps = 0;
      for (int i = 0; i < 200000; i++) {
          const uint32_t size = 64 + rn(6000);
          uint32_t h = (head + 0xffu) & 0xffffff00u;
          if ((uint64_t)h + size > 0x137ffffu) { h = 0; wraps++; }
          head = h + size;
          if (!(i % 3)) nrne_ring_note(&v->ring, head);          // sampled at some of the allocations
          else if (h == 0) nrne_ring_note(&v->ring, head);       // (the sampler sees every wrap: a wrap takes far longer than a sample)
      }
      CHECK(wraps > 20 && nrn_ev_ring_epoch(v) == wraps); }
    free(v);
}

// ---- 2 / 3 the model ----
enum { POOL = 6 << 20, BASE = 0x30000000, MAXB = 300, PAGES = POOL >> 12, GAP = 64 };
static uint8_t *g_mem;                    // the pool's bytes
static uint64_t g_tr[PAGES], g_tr_now;    // the page tracker: generation of the newest write to the page
typedef struct { int live, kind, cls; uint64_t handle, lo, size, rec; } mbuf;   // rec: the content version as RECORDED (what a draw recorded now will read)
static mbuf g_b[MAXB];
static uint32_t g_gen[MAXB];
typedef struct { int b; uint64_t handle; uint32_t off, n; uint8_t fill; } pend;   // a GPU copy written into the stream, not executed yet
static pend g_q[64]; static int g_nq;
static nrn_ev *g_v;
static uint64_t g_fnv(const uint8_t *p, size_t n) { uint64_t h = 0xcbf29ce484222325ull; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull; return h; }
static void mem_write(uint64_t off, uint64_t n, uint8_t fill) {   // any CPU / backend write: the bytes, and the tracker sees the pages
    for (uint64_t i = 0; i < n; i++) g_mem[off + i] = (uint8_t)(fill + i * 7);
    const uint64_t g = ++g_tr_now;
    for (uint64_t p = off >> 12; p <= (off + n - 1) >> 12; p++) g_tr[p] = g;
}
static uint64_t tr_ver(uint64_t off, uint64_t n) { uint64_t g = 0; for (uint64_t p = off >> 12; p <= (off + n - 1) >> 12; p++) if (g_tr[p] > g) g = g_tr[p]; return g; }
// a free range of `size` bytes (first fit over the live buffers, GAP bytes between two: neighbours' bytes on shared pages); -1 none
static int64_t alloc_range(uint64_t size) {
    for (int tries = 0; tries < 40; tries++) {
        const uint64_t lo = (uint64_t)rn(POOL - (uint32_t)size - GAP) & ~15ull;
        int ok = 1;
        for (int i = 0; i < MAXB && ok; i++) if (g_b[i].live && g_b[i].lo < lo + size + GAP && g_b[i].lo + g_b[i].size + GAP > lo) ok = 0;
        if (ok) return (int64_t)lo;
    }
    return -1;
}
// the asks: (range, epoch) -> content; open addressing, never cleared
enum { SEEN = 1 << 20 };
static struct { uint64_t a, n, ep, hash; } *g_seen;
static void seen_check(uint64_t a, uint64_t n, uint64_t ep, uint64_t hash) {
    uint32_t i = (uint32_t)((a * 0x9E3779B97F4A7C15ull ^ n * 0xC2B2AE3D27D4EB4Full ^ ep * 0x165667B19E3779F9ull) >> 40) & (SEEN - 1);
    for (;; i = (i + 1) & (SEEN - 1)) {
        if (!g_seen[i].ep) { g_seen[i].a = a; g_seen[i].n = n; g_seen[i].ep = ep; g_seen[i].hash = hash; return; }
        if (g_seen[i].a == a && g_seen[i].n == n && g_seen[i].ep == ep) { CHECK(g_seen[i].hash == hash); return; }   // one epoch, one content
    }
}
static struct { uint64_t handle, ep, rec; } *g_hseen;   // (handle, epoch) -> the recorded content version
static void hseen_check(uint64_t handle, uint64_t ep, uint64_t rec) {
    uint32_t i = (uint32_t)((handle * 0x9E3779B97F4A7C15ull ^ ep * 0xC2B2AE3D27D4EB4Full) >> 40) & (SEEN - 1);
    for (;; i = (i + 1) & (SEEN - 1)) {
        if (!g_hseen[i].ep) { g_hseen[i].handle = handle; g_hseen[i].ep = ep; g_hseen[i].rec = rec; return; }
        if (g_hseen[i].handle == handle && g_hseen[i].ep == ep) { CHECK(g_hseen[i].rec == rec); return; }
    }
}
static void exec_pending(void) {   // the translation thread executes the copies of the stream, in order
    for (int i = 0; i < g_nq; i++) {
        const mbuf *b = &g_b[g_q[i].b];
        if (!b->live || b->handle != g_q[i].handle) continue;      // (the engine does not destroy a buffer with a copy in flight; the model drops it)
        mem_write(b->lo + g_q[i].off, g_q[i].n, g_q[i].fill);
        nrn_ev_wrote(g_v, BASE + b->lo + g_q[i].off, g_q[i].n);     // snapshot_invalidate
    }
    g_nq = 0;
}
static uint64_t g_vcount[NRNE_V_N], g_neigh_writes, g_rogue_writes;
static nrne_vmemo *g_vm;
// one ask of the translation thread about [off, off + n) of the pool
static void ask(uint64_t off, uint64_t n, int want_cov) {
    int kind = -1, cls = -1;
    const uint64_t ep = nrn_ev_range_epoch(g_v, BASE + off, n, &kind, &cls);
    CHECK((ep != 0) == (want_cov != 0));
    if (!ep) return;
    const uint64_t h = g_fnv(g_mem + off, (size_t)n);
    seen_check(BASE + off, n, ep, h);
    // the verify of =2 over the same ask (the tracker's model always has a version, sometimes not: a page busy)
    nrne_vmemo *m = &g_vm[(uint32_t)((off * 0x9E3779B97F4A7C15ull ^ n) >> 45) & 4095];
    const int tr = rn(16) != 0;
    const int r = nrne_verify(m, BASE + off, n, ep, tr, tr_ver(off, n), 1, tr ? nrne_hash(g_mem + off, (size_t)n) : 0);
    g_vcount[r]++;
}
static void run_model(int rogue, int steps) {
    memset(g_mem, 0, POOL); memset(g_tr, 0, sizeof g_tr); memset(g_b, 0, sizeof g_b); memset(g_v, 0, sizeof *g_v); memset(g_vm, 0, 4096 * sizeof *g_vm);
    memset(g_seen, 0, SEEN * sizeof *g_seen); memset(g_hseen, 0, SEEN * sizeof *g_hseen); memset(g_vcount, 0, sizeof g_vcount);
    g_nq = 0; g_neigh_writes = g_rogue_writes = 0; g_tr_now = 0;
    int scratch = -1;
    for (int step = 0; step < steps; step++) {
        const uint32_t op = rn(100);
        const int i = (int)rn(MAXB);
        mbuf *b = &g_b[i];
        if (op < 14) {                                             // create (a free slot; its index gets a new generation)
            if (b->live) continue;
            const uint64_t size = (rn(4) ? 64 + rn(9000) : 20000 + rn(200000)) & ~15ull;
            const int64_t lo = alloc_range(size);
            if (lo < 0) continue;
            b->kind = (int)rn(2); b->cls = scratch < 0 && step > 50 ? NRNE_C_DYNAMIC : (int)rn(3); b->lo = (uint64_t)lo; b->size = size;
            b->handle = (uint64_t)(++g_gen[i] & 0x3fffff) << 32 | (uint32_t)(i + (rn(2) ? NRNE_H * rn(3) : 0));   // indices that share handle slots too
            if (b->cls != NRNE_C_NOINIT) mem_write(b->lo, size, (uint8_t)rnd());                                  // the initial data, before the event
            const uint64_t before = g_v->j.serial;
            const uint64_t s = nrn_ev_created(g_v, b->kind, b->cls, b->handle, BASE + b->lo, size);
            b->live = 1; b->rec = s;
            CHECK(s > before && nrn_ev_buf_epoch(g_v, b->kind, b->handle) == s);                                  // a fresh epoch, never the dead handle's
            if (scratch < 0 && step > 50 && b->kind == NRNE_K_VB) { scratch = i; nrn_ev_scratch(g_v, NRNE_K_VB, b->handle); nrn_ev_scratch(g_v, NRNE_K_IB, b->handle); }
        } else if (op < 22) {                                      // destroy
            if (!b->live || i == scratch) continue;
            int inflight = 0; for (int k = 0; k < g_nq; k++) if (g_q[k].b == i) inflight = 1;
            if (inflight) continue;
            nrn_ev_destroyed(g_v, b->kind, b->handle);
            b->live = 0;
            CHECK(nrn_ev_range_epoch(g_v, BASE + b->lo, b->size, NULL, NULL) == 0);                               // not covered any more
            CHECK(nrn_ev_buf_epoch(g_v, b->kind, b->handle) == 0);
        } else if (op < 40) {                                      // an update by GPU copy: announced now, executed later
            if (!b->live || i == scratch || g_nq == 64) continue;
            const uint32_t off = rn(3) ? 0 : rn((uint32_t)b->size), n = rn(3) ? (uint32_t)b->size - off : 1 + rn((uint32_t)b->size - off);
            g_q[g_nq].b = i; g_q[g_nq].handle = b->handle; g_q[g_nq].off = off; g_q[g_nq].n = n; g_q[g_nq].fill = (uint8_t)rnd(); g_nq++;
            const uint64_t s = nrn_ev_updated(g_v, b->kind, b->handle, off, n, 0);
            if (s) b->rec = s; else b->rec = ~0ull;               // (a handle pushed out of its slot has no epoch: nothing to check)
        } else if (op < 48) {                                      // a CPU copy into the buffer: event, memcpy, event
            if (!b->live || i == scratch) continue;
            if (!nrn_ev_updated(g_v, b->kind, b->handle, 0, 0, 1)) nrn_ev_updated_slow(g_v, b->kind, b->handle);   // as the host: a handle pushed out of its slot still moves the journal
            mem_write(b->lo, b->size, (uint8_t)rnd());
            uint64_t s = nrn_ev_updated(g_v, b->kind, b->handle, 0, 0, 1);
            if (s) b->rec = s; else { b->rec = ~0ull; s = nrn_ev_updated_slow(g_v, b->kind, b->handle); CHECK(s != 0); }
        } else if (op < 54) {                                      // the scratch buffer: written with no event at all
            if (scratch < 0 || !g_b[scratch].live) continue;
            const mbuf *sb = &g_b[scratch]; const uint32_t off = rn((uint32_t)sb->size - 32);
            mem_write(sb->lo + off, 1 + rn(32), (uint8_t)rnd());
        } else if (op < 60) {                                      // a neighbour: bytes right after a live buffer, on its last page, no event
            if (!b->live) continue;
            mem_write(b->lo + b->size, 1 + rn(GAP - 1), (uint8_t)rnd()); g_neigh_writes++;
        } else if (op < 62 && rogue) {                             // (3) the engine writes a covered buffer outside every hooked place
            if (!b->live || i == scratch || b->size < 8192) continue;
            g_mem[b->lo + 5000] ^= 0x5a; { const uint64_t g = ++g_tr_now; g_tr[(b->lo + 5000) >> 12] = g; } g_rogue_writes++;
        } else if (op < 70) {                                      // the translation thread runs the stream
            exec_pending();
        } else if (op < 92) {                                      // an ask of the translation thread
            if (rogue && g_nq) continue;
            if (b->live) {
                const uint64_t off = rn(2) ? 0 : rn((uint32_t)b->size), n = rn(2) ? b->size - off : 1 + rn((uint32_t)(b->size - off));
                if (rogue) { nrne_vmemo *m = &g_vm[(uint32_t)(((b->lo) * 0x9E3779B97F4A7C15ull ^ b->size) >> 45) & 4095];   // (3): whole buffers, the verify alone
                    int kind, cls; const uint64_t ep = nrn_ev_range_epoch(g_v, BASE + b->lo, b->size, &kind, &cls);
                    if (ep) g_vcount[nrne_verify(m, BASE + b->lo, b->size, ep, 1, tr_ver(b->lo, b->size), 1, nrne_hash(g_mem + b->lo, (size_t)b->size))]++;
                } else ask(b->lo + off, n, i != scratch);
                if (!rogue && rn(4) == 0) ask(b->lo + off, n + (b->size - off - n) + 1 + rn(GAP - 1), 0);          // hangs over the end: not covered
                if (!rogue && b->lo >= 16 && rn(4) == 0) ask(b->lo - 1 - rn(15), 16 + rn(32), 0);                  // begins before it
            } else if (!rogue) {
                const uint64_t off = rn(POOL - 64); int in = 0;
                for (int k = 0; k < MAXB; k++) if (g_b[k].live && off >= g_b[k].lo && off + 16 <= g_b[k].lo + g_b[k].size && k != scratch) in = 1;
                ask(off, 16, in);
            }
        } else {                                                   // an ask of the game thread: the handle's epoch as recorded
            if (!b->live || b->rec == ~0ull) continue;
            const uint64_t ep = nrn_ev_buf_epoch(g_v, b->kind, b->handle);
            if (!ep) continue;                                     // pushed out of its slot
            if (i != scratch) { CHECK(ep == b->rec); hseen_check(b->handle, ep, b->rec); }
        }
    }
    exec_pending();
}

int main(void) {
    test_units();
    g_mem = (uint8_t *)malloc(POOL); g_v = (nrn_ev *)calloc(1, sizeof *g_v); g_vm = (nrne_vmemo *)calloc(4096, sizeof *g_vm);
    g_seen = calloc(SEEN, sizeof *g_seen); g_hseen = calloc(SEEN, sizeof *g_hseen);
    if (!g_mem || !g_v || !g_vm || !g_seen || !g_hseen) { printf("no memory\n"); return 2; }
    run_model(0, 400000);
    printf("model: asks compared by the verify - same %llu, both %llu, event only %llu, neighbour %llu, unhashed %llu, BAD %llu, first %llu; neighbour writes %llu; covered ranges now %llu\n",
           (unsigned long long)g_vcount[NRNE_V_SAME], (unsigned long long)g_vcount[NRNE_V_BOTH], (unsigned long long)g_vcount[NRNE_V_EVENT], (unsigned long long)g_vcount[NRNE_V_NEIGHBOUR],
           (unsigned long long)g_vcount[NRNE_V_UNHASHED], (unsigned long long)g_vcount[NRNE_V_BAD], (unsigned long long)g_vcount[NRNE_V_FIRST], (unsigned long long)g_neigh_writes,
           (unsigned long long)g_v->cov.n);
    CHECK(g_vcount[NRNE_V_BAD] == 0);                       // every write of a covered buffer was announced
    CHECK(g_vcount[NRNE_V_NEIGHBOUR] > 50);                 // and the unannounced writes to OTHER bytes of a shared page are told apart by the bytes
    CHECK(g_vcount[NRNE_V_SAME] > 1000 && g_vcount[NRNE_V_BOTH] > 100);
    run_model(1, 200000);
    printf("model with unannounced writes into covered buffers: %llu such writes, BAD %llu\n", (unsigned long long)g_rogue_writes, (unsigned long long)g_vcount[NRNE_V_BAD]);
    CHECK(g_rogue_writes > 20 && g_vcount[NRNE_V_BAD] > 0);   // the verify catches a class the engine writes outside the hooks
    printf("%s: %llu checks, %llu failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
