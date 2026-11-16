// NB-12 host test: dm/gfx/nb/nb_hit.h (DM_NB_UBO ring ranges, DM_GFX_HIT_FAST hit-path images) + a rough micro benchmark.
// clang -std=c11 -O2 -Wall -Wextra dm/gfx/vulkan/engine/tests/nb_hit_test.c -o nb_hit_test && ./nb_hit_test
#include "../../../nb/nb_hit.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t rng = 0x12345678abcdefull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec * 1e9 + t.tv_nsec; }

static nb_ring_tab ring;
static nb_hit_tab hits;

static void ring_units(void) {
    memset(&ring, 0, sizeof ring);
    nb_ring_note(&ring, 0x10000100, 256, 7);
    CHECK(nb_ring_find(&ring, 0x10000100, 256, 7));
    CHECK(nb_ring_find(&ring, 0x10000100, 64, 7));          // a shorter copy of the bound range
    CHECK(!nb_ring_find(&ring, 0x10000100, 512, 7));        // more than the bind covers
    CHECK(!nb_ring_find(&ring, 0x10000100, 256, 8));        // the next submission: not known
    CHECK(!nb_ring_find(&ring, 0x10000200, 16, 7));         // never bound
    nb_ring_note(&ring, 0x10000100, 1024, 7);               // the same range rebound bigger
    CHECK(nb_ring_find(&ring, 0x10000100, 1024, 7));
    nb_ring_note(&ring, 0, 64, 7); nb_ring_note(&ring, 0x20000000, 0, 7);
    CHECK(!nb_ring_find(&ring, 0, 64, 7) && !nb_ring_find(&ring, 0x20000000, 16, 7));
}
// Model of the engine's constant ring (main 0x925740 / 0x926f90): blocks rebuilt into new ranges (bump, align 256, wrap), all blocks of
// the program bound by each draw record; other (non-ring) uniform ranges bound by old bind records. Every range nb_ring_find accepts
// must be one a record of the same submission bound, and must not have been rewritten since that bind.
static void ring_stream(void) {
    enum { BLOCKS = 48, RING = 0x1380000 };
    struct { uint64_t addr, size, gen; } blk[BLOCKS];
    static uint32_t written_gen[RING / 256];   // generation of the last write of each 256-byte ring granule
    memset(&ring, 0, sizeof ring); memset(written_gen, 0, sizeof written_gen);
    uint64_t head = 0, gen = 1, epoch = 1, accepted = 0, rejected = 0, lookups = 0; const uint64_t base = 0x40000000;
    for (int b = 0; b < BLOCKS; b++) { blk[b].addr = 0; blk[b].size = 64 + 64 * (rnd() % 16); blk[b].gen = 0; }
    for (int draw = 0; draw < 400000; draw++) {
        if (draw % 700 == 0) ++epoch;                               // a submission
        const int first = (int)(rnd() % (BLOCKS - 6)), n = 1 + (int)(rnd() % 6);
        for (int b = first; b < first + n; b++) {
            if (!blk[b].addr || rnd() % 3 == 0) {                   // dirty / new frame: a new ring range, written once
                head = (head + 255) & ~255ull; if (head + blk[b].size > RING) head = 0;
                blk[b].addr = base + head; blk[b].gen = ++gen;
                for (uint64_t g = head / 256; g <= (head + blk[b].size - 1) / 256; g++) written_gen[g] = (uint32_t)gen;
                head += blk[b].size;
            }
            nb_ring_note(&ring, blk[b].addr, blk[b].size, epoch);   // the draw record binds every block of the program
        }
        for (int k = 0; k < 4; k++) {                               // uniform_snapshot calls of the draw: ring blocks or foreign ranges
            ++lookups;
            if (rnd() % 4 == 0) { const uint64_t a = 0x80000000ull + (rnd() % 4096) * 256; CHECK(!nb_ring_find(&ring, a, 64, epoch)); continue; }
            const int b = first + (int)(rnd() % n);
            nb_ring_ent *e = nb_ring_find(&ring, blk[b].addr, blk[b].size, epoch);
            if (e) { ++accepted; const uint64_t o = blk[b].addr - base;
                for (uint64_t g = o / 256; g <= (o + blk[b].size - 1) / 256; g++) CHECK(written_gen[g] <= blk[b].gen); }   // not rewritten since
            else ++rejected;
        }
    }
    printf("ring stream: %llu lookups, ring ranges accepted %llu, forgotten by collisions %llu\n", (unsigned long long)lookups,
           (unsigned long long)accepted, (unsigned long long)rejected);
    CHECK(accepted > rejected * 20);
}
static void view_units(void) {
    uint8_t sticky[64] = {0}; uint64_t off = 0;
    CHECK(nb_ubo_view(0x1000100, 256, 0x1000000, 64 * 4096, 64, sticky, &off) == NBU_DIRECT && off == 0x100);
    CHECK(nb_ubo_view(0x1000110, 256, 0x1000000, 64 * 4096, 64, sticky, &off) == NBU_ALIGN);
    CHECK(nb_ubo_view(0x0fff000, 256, 0x1000000, 64 * 4096, 64, sticky, &off) == NBU_CHUNK);
    CHECK(nb_ubo_view(0x1000000 + 64 * 4096 - 128, 256, 0x1000000, 64 * 4096, 64, sticky, &off) == NBU_CHUNK);   // runs past the chunk
    CHECK(nb_ubo_view(0x1000100, 256, 0, 0, 64, sticky, &off) == NBU_CHUNK);
    sticky[1] = 1;
    CHECK(nb_ubo_view(0x1000f80, 256, 0x1000000, 64 * 4096, 64, sticky, &off) == NBU_STICKY);   // crosses into the sticky page
    CHECK(nb_ubo_view(0x1000e00, 256, 0x1000000, 64 * 4096, 64, sticky, &off) == NBU_DIRECT);
}
// ---- hit fast: a model of the backend state the full path (get_image with the read preflight + prepare_read) reads ----
typedef struct { int layout_general, sampled_ever, uploaded, gpu_written; uint64_t last_write, storage_write, checked; } MImg;
typedef struct { uint64_t sub, pend, barrier_seq, write_seq, images_epoch; int frag, open_has[8], hooks, front_ok[8]; int pend_overlaps[8]; } MState;
static MImg img[8]; static MState st; static int tex_id[8];
static nb_hit_in in_for(int i, int storage) {
    nb_hit_in x; memset(&x, 0, sizeof x);
    x.tex = &tex_id[i]; x.im = &img[i]; x.serial = 100 + i; x.mem = 0x1000 * (i + 1); x.size = 0x1000;
    x.sub = st.sub; x.pend = st.pend; x.storage = storage; x.hooks = st.hooks; x.front_ok = st.front_ok[i];
    x.checked = img[i].checked == st.sub; x.has_contents = img[i].uploaded || img[i].gpu_written; x.general = img[i].layout_general;
    x.last_write = img[i].last_write; x.storage_write = img[i].storage_write; x.seen = st.barrier_seq; x.sampled_ever = img[i].sampled_ever;
    x.in_open = st.open_has[i];
    return x;
}
// The full path; returns 1 when it did anything besides slot_use (resolve, content check / upload, front refill, pass end, barrier).
static int full_path(int i) {
    int did = 0;
    if (st.pend_overlaps[i]) { st.pend_overlaps[i] = 0; ++st.pend; did = 1; }               // cpu_access resolved pending results
    if (!st.front_ok[i]) { st.front_ok[i] = 1; did = 1; }                                    // get_image_slow lookup refills the front
    if (img[i].checked != st.sub || !(img[i].uploaded || img[i].gpu_written)) { img[i].checked = st.sub; img[i].uploaded = 1; did = 1; }
    const int as_storage = !img[i].sampled_ever;
    const int written = img[i].last_write > st.barrier_seq || (!as_storage && img[i].storage_write > st.barrier_seq);
    if (!img[i].layout_general || written || st.open_has[i]) {                               // end the pass, transition, barrier
        for (int k = 0; k < 8; k++) st.open_has[k] = 0;
        img[i].layout_general = 1; st.barrier_seq = st.write_seq; did = 1;
    }
    return did;
}
static void hit_units(void) {
    memset(&hits, 0, sizeof hits); memset(img, 0, sizeof img); memset(&st, 0, sizeof st);
    st.sub = 5; st.pend = 1; st.write_seq = 10; st.barrier_seq = 10;
    for (int i = 0; i < 8; i++) { img[i].layout_general = 1; img[i].uploaded = 1; img[i].checked = 5; img[i].last_write = 3; img[i].sampled_ever = 1; st.front_ok[i] = 1; }
    nb_hit_in x = in_for(0, 0);
    CHECK(nb_hit_skip(&hits, &x) == NBH_NEW);
    nb_hit_note(&hits, &x); x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_SKIP);
    x = in_for(0, 1); CHECK(nb_hit_skip(&hits, &x) == NBH_STORAGE);
    st.hooks = 1; x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_HOOK); st.hooks = 0;
    st.pend = 2; x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_PEND); st.pend = 1;
    st.sub = 6; img[0].checked = 6; x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_SUB); st.sub = 5; img[0].checked = 5;
    st.front_ok[0] = 0; x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_FRONT); st.front_ok[0] = 1;
    img[0].uploaded = 0; x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_CHECK); img[0].uploaded = 1;
    img[0].layout_general = 0; x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_LAYOUT); img[0].layout_general = 1;
    img[0].last_write = 11; x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_WRITTEN); img[0].last_write = 3;
    img[0].storage_write = 11; x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_WRITTEN);         // sampled: storage writes count
    img[0].sampled_ever = 0; x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_SKIP);              // as storage: they do not (prepare_read)
    img[0].sampled_ever = 1; img[0].storage_write = 0;
    st.open_has[0] = 1; x = in_for(0, 0); CHECK(nb_hit_skip(&hits, &x) == NBH_OPEN); st.open_has[0] = 0;
    nb_hit_in y = in_for(0, 0); y.mem += 0x100; CHECK(nb_hit_skip(&hits, &y) == NBH_NEW);              // registration moved
    y = in_for(0, 0); y.im = &img[1]; CHECK(nb_hit_skip(&hits, &y) == NBH_NEW);                         // another Image
}
// Random streams: whenever the decision says skip, the full path must be a no-op.
static void hit_stream(void) {
    uint64_t skips = 0, full = 0, why[NBH_N] = {0};
    for (int round = 0; round < 2000; round++) {
        memset(&hits, 0, sizeof hits); memset(img, 0, sizeof img); memset(&st, 0, sizeof st);
        st.sub = 1; st.pend = 1; st.write_seq = 1;
        for (int i = 0; i < 8; i++) { img[i].layout_general = rnd() & 1; img[i].sampled_ever = (rnd() % 4) != 0; }
        for (int op = 0; op < 300; op++) {
            const unsigned r = (unsigned)(rnd() % 100);
            if (r < 3) ++st.sub;                                                          // submission
            else if (r < 6) ++st.pend;                                                    // a pending list changed
            else if (r < 8) st.pend_overlaps[rnd() % 8] = 1, ++st.pend;                   // a new pending result over a texture (bumps too)
            else if (r < 13) img[rnd() % 8].last_write = ++st.write_seq;                  // attachment write
            else if (r < 15) img[rnd() % 8].storage_write = ++st.write_seq;
            else if (r < 18) st.barrier_seq = st.write_seq;                               // a barrier elsewhere
            else if (r < 21) { for (int k = 0; k < 8; k++) st.open_has[k] = (rnd() % 5) == 0; }   // a new pass
            else if (r < 22) { for (int k = 0; k < 8; k++) st.front_ok[k] = 0; }          // images_epoch moved
            else if (r < 23) img[rnd() % 8].uploaded = 0;                                 // a CPU write invalidated the contents
            else if (r < 24) img[rnd() % 8].layout_general = 0;                           // a transfer took it out of GENERAL
            else if (r < 25) st.hooks = !st.hooks;
            else {                                                                        // a memo hit reads 1..4 images
                const int n = 1 + (int)(rnd() % 4);
                for (int k = 0; k < n; k++) {
                    const int i = (int)(rnd() % 8); const int storage = (rnd() % 16) == 0;
                    nb_hit_in x = in_for(i, storage); const int w = nb_hit_skip(&hits, &x); ++why[w];
                    if (w == NBH_SKIP) { ++skips; CHECK(full_path(i) == 0); continue; }   // the full path changed nothing
                    ++full; full_path(i);
                    if (!storage && !st.hooks) { nb_hit_in z = in_for(i, 0); nb_hit_note(&hits, &z); }
                }
            }
        }
    }
    printf("hit stream: %llu images, skipped %llu (%.0f %%); full: new %llu sub %llu pend %llu front %llu check %llu layout %llu written %llu open %llu storage %llu hook %llu\n",
           (unsigned long long)(skips + full), (unsigned long long)skips, 100.0 * skips / (double)(skips + full), (unsigned long long)why[NBH_NEW],
           (unsigned long long)why[NBH_SUB], (unsigned long long)why[NBH_PEND], (unsigned long long)why[NBH_FRONT], (unsigned long long)why[NBH_CHECK],
           (unsigned long long)why[NBH_LAYOUT], (unsigned long long)why[NBH_WRITTEN], (unsigned long long)why[NBH_OPEN], (unsigned long long)why[NBH_STORAGE],
           (unsigned long long)why[NBH_HOOK]);
    CHECK(skips > 0);
}
// ---- rough micro benchmark (x86 host; the phone line gives the real numbers) ----
typedef struct { uint64_t addr, bytes; } Rep;
static volatile uint64_t sink;
static void bench(void) {
    enum { N = 200000, REPORTS = 454 };
    static uint8_t guest[1 << 20], arena[1 << 22]; static Rep reps[REPORTS]; static uint8_t texrec[256], texcopy[256];
    for (int i = 0; i < (int)sizeof guest; i++) guest[i] = (uint8_t)rnd();
    for (int i = 0; i < REPORTS; i++) { reps[i].addr = 0x90000000ull + 16 * (uint64_t)i; reps[i].bytes = 16; }
    memset(&ring, 0, sizeof ring);
    for (int i = 0; i < 1024; i++) nb_ring_note(&ring, (uint64_t)(uintptr_t)guest + 256 * (uint64_t)i, 640, 1);
    uint8_t sticky[256] = {0};
    // (a) ring range in place: find + view decision
    double t0 = now_ns(); uint64_t acc = 0;
    for (int k = 0; k < N; k++) { const uint64_t a = (uint64_t)(uintptr_t)guest + 256 * (uint64_t)(k & 1023); nb_ring_ent *e = nb_ring_find(&ring, a, 600, 1);
        uint64_t off = 0; if (e && nb_ubo_view(a, 608, (uint64_t)(uintptr_t)guest, sizeof guest, 64, sticky, &off) == NBU_DIRECT) acc += off; }
    double t1 = now_ns(); sink = acc;
    // (b) snapshot miss model: pending scan (empty) + copy 600 B into a bump arena + zero pad (the std::map insert not modelled: cheaper than the phone)
    size_t at = 0;
    for (int k = 0; k < N; k++) { const uint8_t *src = guest + 256 * (k & 1023); if (at + 608 > sizeof arena) at = 0; memcpy(arena + at, src, 600); memset(arena + at + 600, 0, 8); at += 640; }
    double t2 = now_ns(); sink = arena[123];
    // (c) hit-path image, full path model: tex_t copy + cpu_access over 454 pending reports (why 9) + front lookup
    for (int k = 0; k < N; k++) { memcpy(texcopy, texrec, sizeof texcopy); const uint64_t a = 0x10000000ull + 4096 * (uint64_t)(k & 63), n = 4096; int hit = 0;
        for (int r = 0; r < REPORTS; r++) if (a < reps[r].addr + reps[r].bytes && reps[r].addr < a + n) { hit = 1; break; }
        acc += hit + texcopy[k & 255]; }
    double t3 = now_ns(); sink = acc;
    // (d) the hit-fast decision
    memset(&hits, 0, sizeof hits); memset(img, 0, sizeof img); memset(&st, 0, sizeof st); st.sub = 1; st.pend = 1;
    for (int i = 0; i < 8; i++) { img[i].layout_general = 1; img[i].uploaded = 1; img[i].checked = 1; img[i].sampled_ever = 1; st.front_ok[i] = 1; nb_hit_in z = in_for(i, 0); nb_hit_note(&hits, &z); }
    for (int k = 0; k < N; k++) { nb_hit_in x = in_for(k & 7, 0); acc += nb_hit_skip(&hits, &x); }
    double t4 = now_ns(); sink = acc;
    printf("bench (x86, ns per call): ring in-place decision %.1f vs snapshot copy of 600 B %.1f; hit image decision %.1f vs preflight scan of %d reports + tex copy %.1f\n",
           (t1 - t0) / N, (t2 - t1) / N, (t4 - t3) / N, REPORTS, (t3 - t2) / N);
}
int main(void) {
    ring_units(); ring_stream(); view_units(); hit_units(); hit_stream(); bench();
    if (fails) { printf("FAIL: %d\n", fails); return 1; }
    printf("PASS: nb_hit_test (ring note / find, in-place view, hit-fast decision vs full-path model)\n");
    return 0;
}
