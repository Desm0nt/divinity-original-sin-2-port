// [DOS2] NR-08 host test: dm/gfx/nr/nr_desc.h (DM_NR_DESC) - the plan of a shader set, the split of its uniform blocks under a dynamic
// limit, the key table, the read-hazard rule, and the set recycler against a model of the backend's two frame slots: every set a draw
// bound must still hold, when that draw's submission retires, exactly what the draw needed (texture key + static tuple).
// With a bundle directory it also builds the plan of every vertex + fragment pair of the game and prints what NR-08 can serve per
// device class (16 dynamic uniform buffers: Adreno; 8: Mali / Xclipse; none: PowerVR policy).
// Build (Windows LLVM or any C11 compiler), from the repository root:
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nr_desc_test.c dm/gfx/nr/nr_desc.c -o nr_desc_test
//   ./nr_desc_test [<work/shaders-vk> <work/shaders-jobs/programs.tsv>]
#include "../../../nr/nr_desc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
static long checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { if (++fails < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec * 1e9 + t.tv_nsec; }

// The engine's dyn_mode_for + UBO_DYN (xbg_game.cpp) for one shader set: 1 all uniform buffers dynamic, 2 only the per-draw ones
// (game blocks, driver blocks), 0 none; 3 = NR-08 split (`split` set): the game blocks picked by nr_dsplit.
static unsigned dyn_mode(const nr_dstage st[NRD_STAGES], unsigned limit, int dynamic_on, int split, uint8_t *dyn[NRD_STAGES], const uint32_t chg[6][16]) {
    unsigned all = 0, vary = 0, mode;
    for (unsigned s = 0; s < NRD_STAGES; s++) if (st[s].present) for (uint32_t i = 0; i < st[s].n; i++) {
        const nr_dbind *b = &st[s].b[i];
        if (b->type != NRD_T_UBO) continue;
        all += b->count;
        if (b->source == NRD_S_UNIFORM || b->source == NRD_S_DRIVER0 || b->source == NRD_S_DRIVER2) vary += b->count;
    }
    if (!dynamic_on) mode = 0;
    else mode = all && all <= limit ? 1 : (vary && vary <= limit ? 2 : 0);
    const nr_dbind *pick[NRD_MAX_UBO]; uint32_t c[NRD_MAX_UBO]; uint8_t d[NRD_MAX_UBO]; unsigned n = 0; int ok = 1;
    if (!mode && dynamic_on && split && vary > limit) {
        for (unsigned s = 0; s < NRD_STAGES && ok; s++) if (st[s].present) for (uint32_t i = 0; i < st[s].n; i++) {
            const nr_dbind *b = &st[s].b[i];
            if (b->type != NRD_T_UBO) continue;
            if (b->source == NRD_S_DRIVER0 || b->source == NRD_S_DRIVER2) { ok = 0; break; }
            if (b->source != NRD_S_UNIFORM) continue;
            if (b->count != 1 || b->slot >= 16 || n >= NRD_MAX_UBO) { ok = 0; break; }
            pick[n] = b; c[n] = chg ? chg[st[s].ns][b->slot] : 0; n++;
        }
        if (ok && n > limit) { nr_dsplit(c, n, limit, d); mode = 3; }
    }
    for (unsigned s = 0; s < NRD_STAGES; s++) if (st[s].present) for (uint32_t i = 0; i < st[s].n; i++) {
        const nr_dbind *b = &st[s].b[i]; int v = 0;
        if (b->type == NRD_T_UBO) {
            if (mode == 1) v = 1;
            else if (mode == 2) v = b->source == NRD_S_UNIFORM || b->source == NRD_S_DRIVER0 || b->source == NRD_S_DRIVER2;
            else if (mode == 3) for (unsigned k = 0; k < n; k++) if (pick[k] == b) v = d[k];
        }
        dyn[s][i] = (uint8_t)v;
    }
    return mode;
}

// ---------------- plan ----------------
static nr_dbind B(uint32_t binding, uint32_t type, uint32_t bytes, uint32_t source, uint32_t slot, uint32_t samp) {
    nr_dbind b = {binding, type, 1, bytes, source, slot, samp, 0};
    return b;
}
static void plan_units(void) {
    // a DOS2-like pair: VS (stage 0, NVN stage 0): 3 game blocks + c1 + support; FS (stage 4, NVN stage 1): 4 game blocks, support, 3 textures
    nr_dbind vs[] = {B(0, 6, 0x90, 2, 0, 0), B(6, 6, 0x40, 2, 1, 0), B(12, 6, 0x200, 2, 2, 0), B(18, 6, 64, 4, 0, 0), B(24, 6, 1392, 1, 0, 0)};
    nr_dbind fs[] = {B(4, 6, 0x90, 2, 0, 0), B(10, 6, 0x100, 2, 3, 0), B(16, 6, 0x30, 2, 4, 0), B(22, 6, 0, 2, 5, 0), B(28, 6, 1392, 1, 0, 0),
                     B(34, 1, 0, 7, 0, 0xffffffffu), B(40, 1, 0, 7, 1, 1), B(46, 1, 0, 7, 7, 0xffffffffu)};
    uint8_t dv[8] = {0}, df[8] = {0}; uint8_t *dyn[NRD_STAGES] = {dv, dv, dv, dv, df};
    nr_dstage st[NRD_STAGES]; memset(st, 0, sizeof st);
    st[0].b = vs; st[0].n = 5; st[0].ns = 0; st[0].present = 1; st[0].dyn = dv;
    st[4].b = fs; st[4].n = 8; st[4].ns = 1; st[4].present = 1; st[4].dyn = df;
    nr_dplan p;
    // Adreno: 16 dynamic -> mode 1, 10 dynamic offsets in binding order
    CHECK(dyn_mode(st, 16, 1, 0, dyn, NULL) == 1);
    CHECK(nr_dplan_build(&p, st) == NRD_OK && p.nrow == 13 && p.nubo == 7 && p.ndyn == 10 && p.nstat == 0 && p.nimg == 3);
    CHECK(p.tex_stages == 2 && p.ubo_stages == 3);
    {   // binding order: 0 4 6 10 12 16 18 22 24 28 -> ranks; rows are in stage / table order
        static const uint8_t want[10] = {0, 2, 4, 6, 8, 1, 3, 5, 7, 9};
        for (unsigned i = 0; i < 10; i++) CHECK(p.row[i].dyn_at == want[i]);
        CHECK(p.row[3].kind == NRD_K_FIXED_DYN && p.row[4].kind == NRD_K_FIXED_DYN && p.row[0].kind == NRD_K_RING_DYN && p.row[8].bytes == 0);
        CHECK(p.row[10].kind == NRD_K_IMAGE && p.row[10].samp == 0xff && p.row[11].samp == 1 && p.row[12].slot == 7 && p.row[12].ns == 1 && p.row[12].dyn_at == 0xff);
        for (unsigned i = 0; i < p.nubo; i++) CHECK(p.row[p.ubo_row[i]].source == NRD_S_UNIFORM);
        for (unsigned i = 0; i < p.nimg; i++) CHECK(p.row[p.img_row[i]].kind == NRD_K_IMAGE);
    }
    // Mali: 8 dynamic. all = 10 > 8, per-draw = 7 <= 8 -> mode 2: support / c1 static
    CHECK(dyn_mode(st, 8, 1, 0, dyn, NULL) == 2);
    CHECK(nr_dplan_build(&p, st) == NRD_OK && p.ndyn == 7 && p.nstat == 0 && p.row[3].kind == NRD_K_FIXED_STAT && p.row[9].kind == NRD_K_FIXED_STAT);
    // PowerVR policy: no dynamic buffers -> every game block a static tuple word
    CHECK(dyn_mode(st, 8, 0, 1, dyn, NULL) == 0);
    CHECK(nr_dplan_build(&p, st) == NRD_OK && p.ndyn == 0 && p.nstat == 7 && p.nubo == 7);
    for (unsigned i = 0; i < p.nubo; i++) CHECK(p.row[p.ubo_row[i]].kind == NRD_K_RING_STAT && p.row[p.ubo_row[i]].stat_at == i);
    // a pair with 10 game blocks on a device with 8: the old layout = all static (mode 0); the split keeps the 8 most-changing dynamic
    nr_dbind fs10[] = {B(4, 6, 0x90, 2, 0, 0), B(10, 6, 0x100, 2, 3, 0), B(16, 6, 0x30, 2, 4, 0), B(22, 6, 0x10, 2, 5, 0), B(28, 6, 0x10, 2, 6, 0),
                       B(34, 6, 0x10, 2, 7, 0), B(40, 6, 0x10, 2, 8, 0), B(46, 1, 0, 7, 0, 0xffffffffu)};
    st[4].b = fs10; st[4].n = 8;
    CHECK(dyn_mode(st, 8, 1, 0, dyn, NULL) == 0);
    CHECK(nr_dplan_build(&p, st) == NRD_OK && p.nubo == 10 && p.nstat == 10 && p.ndyn == 0);
    uint32_t chg[6][16]; memset(chg, 0, sizeof chg);
    chg[0][0] = 5; chg[0][1] = 900; chg[0][2] = 880; chg[1][0] = 5; chg[1][3] = 400; chg[1][4] = 30; chg[1][5] = 870; chg[1][6] = 860; chg[1][7] = 850; chg[1][8] = 840;
    CHECK(dyn_mode(st, 8, 1, 1, dyn, chg) == 3);
    CHECK(nr_dplan_build(&p, st) == NRD_OK && p.nubo == 10 && p.nstat == 2 && p.ndyn == 8);
    CHECK(p.row[0].kind == NRD_K_RING_STAT && p.row[5].kind == NRD_K_RING_STAT && p.row[0].stat_at == 0 && p.row[5].stat_at == 1);   // the two per-frame blocks (5 changes)
    CHECK(p.row[3].kind == NRD_K_FIXED_STAT && p.row[4].kind == NRD_K_FIXED_STAT);
    // not eligible
    nr_dbind bad1[] = {B(0, 6, 0x90, 2, 0, 0), B(6, 7, 0, 3, 0, 0)};                 // storage buffer
    nr_dbind bad2[] = {B(0, 6, 0x1000, 5, 0, 0)};                                      // driver block
    nr_dbind bad3[] = {B(0, 1, 0, 7, 0, 0xffffffffu)}; bad3[0].count = 4;             // texture array
    nr_dbind bad4[] = {B(0, 1, 0, 8, 0, 0xffffffffu)};                                 // image source
    nr_dbind bad5[] = {B(0, 6, 0x90, 2, 16, 0)};                                       // block slot out of range
    nr_dbind bad6[] = {B(0, 3, 0, 8, 0, 0)};                                           // storage image
    memset(st, 0, sizeof st); st[4].present = 1; st[4].ns = 1; st[4].dyn = df;
    st[4].b = bad1; st[4].n = 2; CHECK(nr_dplan_build(&p, st) == NRD_NE_TYPE);
    st[4].b = bad2; st[4].n = 1; CHECK(nr_dplan_build(&p, st) == NRD_NE_DRIVER);
    st[4].b = bad3; CHECK(nr_dplan_build(&p, st) == NRD_NE_ARRAY);
    st[4].b = bad4; CHECK(nr_dplan_build(&p, st) == NRD_NE_SOURCE);
    st[4].b = bad5; CHECK(nr_dplan_build(&p, st) == NRD_NE_SLOT);
    st[4].b = bad6; CHECK(nr_dplan_build(&p, st) == NRD_NE_TYPE);
    memset(st, 0, sizeof st); CHECK(nr_dplan_build(&p, st) == NRD_NE_NOSHADER);
    {   // too many rows
        static nr_dbind many[NRD_MAX_IMG + 1]; for (unsigned i = 0; i <= NRD_MAX_IMG; i++) many[i] = B(i * 6, 1, 0, 7, i, 0xffffffffu);
        static uint8_t dm[NRD_MAX_IMG + 1];
        st[4].present = 1; st[4].ns = 1; st[4].dyn = dm; st[4].b = many; st[4].n = NRD_MAX_IMG + 1; CHECK(nr_dplan_build(&p, st) == NRD_NE_ROWS);
        st[4].n = NRD_MAX_IMG; CHECK(nr_dplan_build(&p, st) == NRD_OK && p.nimg == NRD_MAX_IMG);
    }
    for (int w = 0; w < NRD_NE_N; w++) CHECK(nr_dne_name(w)[0] != '?');
    {   // device limits: 3 textures in the fragment stage, 5 / 5 uniform buffers a stage, 10 dynamic (the first pair, all dynamic)
        nr_dbind v2[] = {B(0, 6, 0x90, 2, 0, 0), B(6, 6, 0x40, 2, 1, 0), B(12, 6, 0x200, 2, 2, 0), B(18, 6, 64, 4, 0, 0), B(24, 6, 1392, 1, 0, 0)};
        nr_dbind f2[] = {B(4, 6, 0x90, 2, 0, 0), B(10, 6, 0x100, 2, 3, 0), B(16, 6, 0x30, 2, 4, 0), B(22, 6, 0, 2, 5, 0), B(28, 6, 1392, 1, 0, 0),
                         B(34, 1, 0, 7, 0, 0xffffffffu), B(40, 1, 0, 7, 1, 1), B(46, 1, 0, 7, 7, 0xffffffffu)};
        uint8_t a5[5] = {1, 1, 1, 1, 1}, a8[8] = {1, 1, 1, 1, 1, 0, 0, 0};
        memset(st, 0, sizeof st);
        st[0].b = v2; st[0].n = 5; st[0].ns = 0; st[0].present = 1; st[0].dyn = a5;
        st[4].b = f2; st[4].n = 8; st[4].ns = 1; st[4].present = 1; st[4].dyn = a8;
        CHECK(nr_dplan_build(&p, st) == NRD_OK && p.ndyn == 10);
        CHECK(nr_dplan_limits(&p, 16, 12, 16) == NRD_OK && nr_dplan_limits(&p, 3, 5, 10) == NRD_OK);
        CHECK(nr_dplan_limits(&p, 2, 12, 16) == NRD_NE_LIMIT && p.why == NRD_NE_LIMIT);
        CHECK(nr_dplan_build(&p, st) == NRD_OK && nr_dplan_limits(&p, 16, 4, 16) == NRD_NE_LIMIT);
        CHECK(nr_dplan_build(&p, st) == NRD_OK && nr_dplan_limits(&p, 16, 12, 9) == NRD_NE_LIMIT);
        p.why = NRD_NE_TYPE; CHECK(nr_dplan_limits(&p, 0, 0, 0) == NRD_NE_TYPE);   // an ineligible plan keeps its reason
    }
    // nr_dsplit: ties go to the lower index; limit >= n keeps all dynamic
    uint32_t c4[4] = {7, 7, 7, 7}; uint8_t d4[4];
    CHECK(nr_dsplit(c4, 4, 2, d4) == 2 && d4[0] && d4[1] && !d4[2] && !d4[3]);
    CHECK(nr_dsplit(c4, 4, 4, d4) == 0 && nr_dsplit(c4, 4, 9, d4) == 0 && nr_dsplit(c4, 4, 0, d4) == 4);
    uint32_t c5[5] = {1, 50, 2, 50, 3};
    uint8_t d5[5];
    CHECK(nr_dsplit(c5, 5, 3, d5) == 2 && !d5[0] && d5[1] && !d5[2] && d5[3] && d5[4]);
}

// ---------------- hazard rule ----------------
static void hazard_units(void) {
    nr_dimg_in x; memset(&x, 0, sizeof x);
    x.checked = x.uploaded = x.general = 1;
    CHECK(nr_dimg_skip(&x) == NRD_I_SKIP);
    x.checked = 0; CHECK(nr_dimg_skip(&x) == NRD_I_FIRST); x.checked = 1;
    x.uploaded = 0; CHECK(nr_dimg_skip(&x) == NRD_I_FIRST); x.uploaded = 1;
    x.general = 0; CHECK(nr_dimg_skip(&x) == NRD_I_LAYOUT); x.general = 1;
    x.gpu_written = 1; CHECK(nr_dimg_skip(&x) == NRD_I_RT); x.gpu_written = 0;
    x.last_write = 9; CHECK(nr_dimg_skip(&x) == NRD_I_RT); x.last_write = 0;
    x.storage_write = 9; CHECK(nr_dimg_skip(&x) == NRD_I_RT); x.storage_write = 0;
    x.op_write = 9; CHECK(nr_dimg_skip(&x) == NRD_I_RT); x.op_write = 0;
    x.hook = 1; CHECK(nr_dimg_skip(&x) == NRD_I_HOOK); x.hook = 0;
    // every combination: SKIP only for an uploaded, checked, never GPU-written image in the read layout with no hook
    for (unsigned m = 0; m < 512; m++) {
        x.last_write = m & 1; x.storage_write = m >> 1 & 1; x.op_write = m >> 2 & 1; x.checked = m >> 3 & 1; x.uploaded = m >> 4 & 1; x.gpu_written = m >> 5 & 1;
        x.general = m >> 6 & 1; x.hook = m >> 7 & 1;
        const int want = (m & 0x1ff & ~0x100u) == (8 | 16 | 64);
        CHECK((nr_dimg_skip(&x) == NRD_I_SKIP) == want);
    }
}

// ---------------- key table ----------------
static void table_units(void) {
    nr_dtab t;
    CHECK(nr_dtab_init(&t, 1) == -1);
    CHECK(nr_dtab_init(&t, 300) == 0 && t.max == 300 && t.cap >= 600);
    uint64_t key[NRD_KEY_WORDS];
    static uint64_t keys[300][NRD_KEY_WORDS]; static unsigned nk[300];
    for (unsigned i = 0; i < 300; i++) {
        nk[i] = NRD_KEY_FIXED + 2 * (unsigned)(rnd() % (NRD_MAX_IMG + 1));
        for (unsigned w = 0; w < nk[i]; w++) keys[i][w] = rnd() % 7 ? rnd() : 0;
        keys[i][0] = i;   // distinct
        const uint64_t h = nr_dkey_hash64(keys[i], nk[i]);
        CHECK(nr_dtab_find(&t, h, keys[i], nk[i]) == -1);
        CHECK(nr_dtab_add(&t, h, keys[i], nk[i]) == (int32_t)i);
    }
    CHECK(t.n == 300);
    memcpy(key, keys[0], sizeof key);
    CHECK(nr_dtab_add(&t, 1, key, nk[0]) == -1);   // full
    for (unsigned i = 0; i < 300; i++) {
        const uint64_t h = nr_dkey_hash64(keys[i], nk[i]);
        CHECK(nr_dtab_find(&t, h, keys[i], nk[i]) == (int32_t)i);
        memcpy(key, keys[i], sizeof key); key[nk[i] - 1] ^= 1;   // one word differs
        CHECK(nr_dtab_find(&t, nr_dkey_hash64(key, nk[i]), key, nk[i]) == -1);
        CHECK(nr_dtab_find(&t, h, key, nk[i]) == -1);            // the same hash, another key: words are compared
        if (nk[i] > NRD_KEY_FIXED) CHECK(nr_dtab_find(&t, h, keys[i], nk[i] - 2) == -1);   // a prefix is another key
    }
    // equal hashes on purpose (the table trusts the words, not the hash)
    nr_dtab_clear(&t); CHECK(t.n == 0);
    for (unsigned i = 0; i < 200; i++) CHECK(nr_dtab_add(&t, 42, keys[i], nk[i]) == (int32_t)i);
    for (unsigned i = 0; i < 200; i++) CHECK(nr_dtab_find(&t, 42, keys[i], nk[i]) == (int32_t)i);
    CHECK(nr_dtab_find(&t, 42, keys[250], nk[250]) == -1);
    nr_dtab_clear(&t);
    CHECK(nr_dtab_find(&t, 42, keys[0], nk[0]) == -1);
    nr_dtab_free(&t);
    CHECK(nr_dtab_find(&t, 42, keys[0], nk[0]) == -1 && nr_dtab_add(&t, 42, keys[0], nk[0]) == -1);
    // the hash spreads keys that differ in one handle
    {   enum { N = 4096 }; static uint64_t hs[N]; unsigned coll = 0;
        uint64_t k[13]; for (unsigned w = 0; w < 13; w++) k[w] = rnd();
        for (unsigned i = 0; i < N; i++) { k[11] = 0x100000 + i; hs[i] = nr_dkey_hash64(k, 13); }
        static uint8_t seen[1 << 16];
        for (unsigned i = 0; i < N; i++) { const unsigned b = (unsigned)hs[i] & 0xffff; if (seen[b]) coll++; seen[b] = 1; }
        CHECK(coll < N / 8);   // ~ N^2 / (2 * 65536) = 128 expected
    }
}

// ---------------- recycler: units ----------------
static void fifo_units(void) {
    nr_dfifo f; nr_dfifo_init(&f);
    CHECK(nr_dfifo_take(&f, 5, NRD_AGE_SAFE) == -1);
    const int32_t a = nr_dfifo_push(&f, 100, 5), b = nr_dfifo_push(&f, 101, 5), c = nr_dfifo_push(&f, 102, 6);
    CHECK(a == 0 && b == 1 && c == 2 && f.n == 3 && f.node[a].gen == 1);
    CHECK(nr_dfifo_take(&f, 5, NRD_AGE_SAFE) == -1 && nr_dfifo_take(&f, 6, NRD_AGE_SAFE) == -1);    // serial 5 is recorded / flying
    CHECK(nr_dfifo_use(&f, a, 1, 6) == 1);                                // a bound again at 6: goes behind b
    CHECK(nr_dfifo_take(&f, 7, NRD_AGE_SAFE) == b && f.node[b].gen == 2 && f.node[b].stamp == 7 && f.node[b].set == 101);
    CHECK(nr_dfifo_use(&f, b, 1, 7) == 0);                                // the old holder of b lost it
    CHECK(nr_dfifo_use(&f, b, 2, 7) == 1);
    CHECK(nr_dfifo_take(&f, 7, NRD_AGE_SAFE) == -1);                                    // c and a were bound at 6
    CHECK(nr_dfifo_take(&f, 8, NRD_AGE_SAFE) == c && nr_dfifo_take(&f, 8, NRD_AGE_SAFE) == a && nr_dfifo_take(&f, 8, NRD_AGE_SAFE) == -1);
    CHECK(nr_dfifo_take(&f, 9, NRD_AGE_SAFE) == b);
    CHECK(nr_dfifo_use(&f, -1, 1, 9) == 0 && nr_dfifo_use(&f, 77, 1, 9) == 0);
    for (int i = 0; i < 500; i++) CHECK(nr_dfifo_push(&f, 1000 + (uint64_t)i, 10) == 3 + i);   // grows
    CHECK(f.n == 503 && f.node[2].set == 102);
    nr_dfifo_free(&f); CHECK(f.n == 0 && nr_dfifo_take(&f, 99, NRD_AGE_SAFE) == -1);
}

// ---------------- recycler + entries against the frame-slot model ----------------
// The engine side's set choice (xbg_nr_desc.inc serve): an entry keeps its two last sets by static tuple; a miss rewrites a set taken
// from the recycler. Model of the GPU: a submission `serial` executes while serials serial and serial + 1 are recorded; when serial + 2
// starts recording it has retired. At that moment every draw of it must find its set holding (entry, tuple) as at the bind.
enum { M_ENT = 40, M_SETS = 20000, M_DRAWS = 400 };
typedef struct { int32_t node; uint32_t gen; uint64_t set; uint32_t tuple; } mvar;
typedef struct { mvar v[2]; int mru; } ment;
typedef struct { uint64_t set; uint32_t ent, tuple; } mdraw;
static struct { uint32_t ent, tuple; int written; } g_set[M_SETS];   // contents of each set now
static uint64_t g_nsets;
static long g_rewrites, g_hits, g_wrong, g_early;
static void frame_model(unsigned nstat_mode, unsigned frames, unsigned tuple_span) {
    // nstat_mode 0: no static tuple (texture part only: tuple always 0); 1: a tuple that rarely changes (split); 2: changes every draw (no dynamic buffers)
    nr_dfifo f; nr_dfifo_init(&f);
    static ment ent[M_ENT]; memset(ent, 0, sizeof ent);
    for (unsigned i = 0; i < M_ENT; i++) ent[i].v[0].node = ent[i].v[1].node = -1;
    static mdraw sub[3][M_DRAWS]; unsigned nsub[3] = {0, 0, 0};
    uint32_t tuple = 1; g_nsets = 0; memset(g_set, 0, sizeof g_set);
    for (uint32_t serial = 10; serial < 10 + frames; serial++) {
        // serial - 2 retires now: its draws read their sets one last time
        const unsigned old = (serial + 1) % 3;   // the ring slot serial - 2 used
        for (unsigned i = 0; i < nsub[old]; i++) { const mdraw *d = &sub[old][i]; if (g_set[d->set].ent != d->ent || g_set[d->set].tuple != d->tuple) g_wrong++; }
        // and serial - 1 is flying: nothing of it may have been rewritten either (checked when it retires); record `serial`
        const unsigned cur = serial % 3; nsub[cur] = 0;
        const unsigned ndraw = 50 + (unsigned)(rnd() % (M_DRAWS - 50));
        for (unsigned k = 0; k < ndraw; k++) {
            const uint32_t e = (uint32_t)(rnd() % M_ENT);
            if (nstat_mode == 0) tuple = 0;
            else if (nstat_mode == 2 || !(rnd() % tuple_span)) tuple++;
            ment *en = &ent[e]; mvar *v = NULL;
            for (int q = 0; q < 2 && !v; q++) { mvar *c = &en->v[q]; if (c->node >= 0 && c->tuple == tuple && nr_dfifo_use(&f, c->node, c->gen, serial)) { v = c; en->mru = q; g_hits++; } }
            if (!v) {
                mvar *c = &en->v[nstat_mode ? en->mru ^ 1 : 0];
                int32_t n = nr_dfifo_take(&f, serial, nstat_mode ? NRD_AGE_SAFE : NRD_AGE_IDLE);
                if (n < 0) { CHECK(g_nsets < M_SETS); n = nr_dfifo_push(&f, g_nsets++, serial); }
                else if (g_set[f.node[n].set].written + 2 > (int)serial && g_set[f.node[n].set].written) g_early++;   // rewritten while a reader could be alive
                c->node = n; c->gen = f.node[n].gen; c->set = f.node[n].set; c->tuple = tuple;
                g_set[c->set].ent = e; g_set[c->set].tuple = tuple; g_set[c->set].written = (int)serial; g_rewrites++;
                en->mru = (int)(c - en->v); v = c;
            }
            g_set[v->set].written = (int)serial;   // last bound
            CHECK(g_set[v->set].ent == e && g_set[v->set].tuple == tuple);
            sub[cur][nsub[cur]].set = v->set; sub[cur][nsub[cur]].ent = e; sub[cur][nsub[cur]].tuple = tuple; nsub[cur]++;
        }
    }
    nr_dfifo_free(&f);
}
static void fifo_model(void) {
    g_rewrites = g_hits = g_wrong = g_early = 0;
    frame_model(0, 400, 1);
    CHECK(g_wrong == 0 && g_early == 0);
    CHECK(g_nsets == M_ENT);                 // texture part only: one set per entry for good, never rewritten
    CHECK(g_rewrites == M_ENT);
    const long h0 = g_hits;
    g_rewrites = g_hits = g_wrong = g_early = 0;
    frame_model(1, 400, 200);                // split: a static tuple that changes about once a frame (the per-frame blocks get a new ring address)
    CHECK(g_wrong == 0 && g_early == 0);
    printf("  model split:   %ld draws reuse a set, %ld set writes, %llu sets\n", g_hits, g_rewrites, (unsigned long long)g_nsets);
    CHECK(g_hits > 4 * g_rewrites);          // each entry rewrites its set once per tuple (a per-frame block moves once a frame), not once per draw
    g_rewrites = g_hits = g_wrong = g_early = 0;
    frame_model(2, 300, 1);                  // no dynamic buffers: the tuple moves every draw -> one write a draw, sets bounded by 3 submissions
    CHECK(g_wrong == 0 && g_early == 0 && g_hits == 0);
    printf("  model static:  %ld set writes (one a draw), %llu sets (<= 3 submissions of draws)\n", g_rewrites, (unsigned long long)g_nsets);
    CHECK(g_nsets <= 3 * M_DRAWS);
    printf("  model dynamic: %ld draws reuse a set, %d sets written once\n", h0, M_ENT);
}

// ---------------- micro benchmark ----------------
static void bench(void) {
    nr_dtab t; if (nr_dtab_init(&t, 4096)) return;
    static uint64_t keys[2000][21];
    for (unsigned i = 0; i < 2000; i++) { for (unsigned w = 0; w < 21; w++) keys[i][w] = rnd(); nr_dtab_add(&t, nr_dkey_hash64(keys[i], 21), keys[i], 21); }
    volatile int64_t sink = 0; const unsigned N = 4000000;
    double t0 = now_ns();
    for (unsigned i = 0; i < N; i++) { const uint64_t *k = keys[(i * 7919u) % 2000]; sink += nr_dtab_find(&t, nr_dkey_hash64(k, 21), k, 21); }
    const double find_ns = (now_ns() - t0) / N;
    nr_dfifo f; nr_dfifo_init(&f); for (unsigned i = 0; i < 3000; i++) nr_dfifo_push(&f, i, 1);
    t0 = now_ns();
    for (unsigned i = 0; i < N; i++) { const uint32_t serial = 3 + i / 1000; int32_t n = nr_dfifo_take(&f, serial, NRD_AGE_SAFE); if (n < 0) n = nr_dfifo_push(&f, i, serial); sink += n; }
    const double take_ns = (now_ns() - t0) / N;
    printf("  bench (this PC): key hash + find (5 textures, 21 words) %.1f ns, recycler take %.1f ns (%llu sets)\n", find_ns, take_ns, (unsigned long long)f.n);
    nr_dfifo_free(&f); nr_dtab_free(&t); (void)sink;
}

// ---------------- the game's bundles ----------------
typedef struct { char key[20]; uint32_t stage, n; nr_dbind *b; } bundle;
static bundle *g_b; static unsigned g_nb;
static int cmp_bundle(const void *a, const void *b) { return strcmp(((const bundle *)a)->key, ((const bundle *)b)->key); }
static const bundle *find_bundle(const char *key) { bundle k; memset(&k, 0, sizeof k); snprintf(k.key, sizeof k.key, "%s", key); return (const bundle *)bsearch(&k, g_b, g_nb, sizeof *g_b, cmp_bundle); }
#ifdef _WIN32
#include <io.h>
static void list_dir(const char *dir, void (*fn)(const char *dir, const char *name)) {
    char pat[1024]; snprintf(pat, sizeof pat, "%s/*.xgs", dir);
    struct _finddata_t fd; intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return;
    do fn(dir, fd.name); while (!_findnext(h, &fd));
    _findclose(h);
}
#else
#include <dirent.h>
static void list_dir(const char *dir, void (*fn)(const char *dir, const char *name)) {
    DIR *d = opendir(dir); if (!d) return;
    for (struct dirent *e; (e = readdir(d));) { const size_t n = strlen(e->d_name); if (n > 4 && !strcmp(e->d_name + n - 4, ".xgs")) fn(dir, e->d_name); }
    closedir(d);
}
#endif
static void load_bundle(const char *dir, const char *name) {
    char path[1200]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *fp = fopen(path, "rb"); if (!fp) return;
    uint32_t h[8];
    if (fread(h, 4, 8, fp) == 8 && h[0] == 0x31534758 && h[1] == 1 && h[3] <= 256) {
        bundle *b = &g_b[g_nb]; memset(b, 0, sizeof *b);
        snprintf(b->key, sizeof b->key, "%.*s", (int)(strlen(name) - 4 < 19 ? strlen(name) - 4 : 19), name);
        b->stage = h[2]; b->n = h[3]; b->b = (nr_dbind *)malloc(sizeof(nr_dbind) * (h[3] ? h[3] : 1));
        if (b->b && fread(b->b, sizeof(nr_dbind), h[3], fp) == h[3]) g_nb++;
    }
    fclose(fp);
}
static void bundles(const char *dir, const char *programs) {
    g_b = (bundle *)calloc(20000, sizeof *g_b); if (!g_b) return;
    list_dir(dir, load_bundle);
    qsort(g_b, g_nb, sizeof *g_b, cmp_bundle);
    FILE *fp = fopen(programs, "rb");
    if (!fp || !g_nb) { printf("  bundles: %u read from %s, programs %s: skipped\n", g_nb, dir, fp ? "ok" : "not found"); if (fp) fclose(fp); return; }
    static const struct { const char *name; unsigned limit; int on, split; } dev[4] = {
        {"16 dynamic (Adreno)", 16, 1, 0}, {"8 dynamic, old layouts (Mali / Xclipse, DM_NR_DESC_SPLIT=0)", 8, 1, 0},
        {"8 dynamic + split (Mali / Xclipse)", 8, 1, 1}, {"no dynamic buffers (PowerVR policy)", 8, 0, 0}};
    unsigned pairs = 0, missing = 0, elig[4] = {0}, ne[NRD_NE_N] = {0}, mode_n[4][4], stat_h[4][NRD_MAX_UBO + 1], rows_max = 0, img_max = 0, ubo_max = 0, dyn_max[4] = {0};
    unsigned long img_sum = 0, ubo_sum = 0; unsigned img16 = 0, stage_ub12 = 0;
    memset(mode_n, 0, sizeof mode_n); memset(stat_h, 0, sizeof stat_h);
    char line[2048]; int first = 1;
    while (fgets(line, sizeof line, fp)) {
        if (first) { first = 0; continue; }
        char *col[6] = {0}; unsigned nc = 0; char *q = line;
        while (nc < 6) { col[nc++] = q; char *tab = strchr(q, '\t'); if (!tab) break; *tab = 0; q = tab + 1; }
        if (nc < 5 || !strcmp(col[1], "-") || !strcmp(col[3], "-")) continue;
        const bundle *vs = find_bundle(col[1]), *ps = find_bundle(col[3]);
        if (!vs || !ps) { missing++; continue; }
        pairs++;
        static uint8_t dv[256], df[256]; uint8_t *dyn[NRD_STAGES] = {dv, dv, dv, dv, df};
        nr_dstage st[NRD_STAGES]; memset(st, 0, sizeof st);
        st[0].b = vs->b; st[0].n = vs->n; st[0].ns = 0; st[0].present = 1; st[0].dyn = dv;
        st[4].b = ps->b; st[4].n = ps->n; st[4].ns = 1; st[4].present = 1; st[4].dyn = df;
        for (unsigned d = 0; d < 4; d++) {
            const unsigned mode = dyn_mode(st, dev[d].limit, dev[d].on, dev[d].split, dyn, NULL);
            nr_dplan p; const int why = nr_dplan_build(&p, st);
            if (d == 0 && !why) {   // Vulkan's required minimums per stage: 16 sampled images, 12 uniform buffers
                unsigned su[2] = {0, 0}, si[2] = {0, 0};
                for (unsigned i = 0; i < p.nrow; i++) { if (p.row[i].kind == NRD_K_IMAGE) si[p.row[i].ns & 1]++; else su[p.row[i].ns & 1]++; }
                if (si[0] > 16 || si[1] > 16) img16++;
                if (su[0] > 12 || su[1] > 12) stage_ub12++;
            }
            if (d == 0) { ne[why]++; if (!why) { img_sum += p.nimg; ubo_sum += p.nubo; if (p.nrow > rows_max) rows_max = p.nrow; if (p.nimg > img_max) img_max = p.nimg; if (p.nubo > ubo_max) ubo_max = p.nubo; } }
            if (why) continue;
            elig[d]++; mode_n[d][mode]++; stat_h[d][p.nstat]++;
            if (p.ndyn > dyn_max[d]) dyn_max[d] = p.ndyn;
            CHECK(p.ndyn <= dev[d].limit || !dev[d].on);
            CHECK(p.nubo == p.nstat + (unsigned)(p.ndyn - (mode == 1 ? p.nrow - p.nimg - p.nubo : 0)));
        }
    }
    fclose(fp);
    printf("  bundles: %u; vertex + fragment pairs %u (%u without a bundle); NR-08 eligible %u (%.1f%%); not: type %u, source %u, driver %u, array %u, slot %u, rows %u\n",
           g_nb, pairs, missing, elig[0], 100. * elig[0] / (pairs ? pairs : 1), ne[NRD_NE_TYPE], ne[NRD_NE_SOURCE], ne[NRD_NE_DRIVER], ne[NRD_NE_ARRAY], ne[NRD_NE_SLOT], ne[NRD_NE_ROWS]);
    printf("  eligible pairs: %.1f textures, %.1f ring blocks on average; at most %u descriptors, %u textures, %u ring blocks; a stage over Vulkan's minimum limits: > 16 textures %u pairs, > 12 uniform buffers %u pairs\n",
           elig[0] ? (double)img_sum / elig[0] : 0., elig[0] ? (double)ubo_sum / elig[0] : 0., rows_max, img_max, ubo_max, img16, stage_ub12);
    for (unsigned d = 0; d < 4; d++) {
        printf("  %-62s all dynamic %u, per-draw dynamic %u, split %u, all static %u; at most %u dynamic offsets; static tuple words:", dev[d].name,
               mode_n[d][1], mode_n[d][2], mode_n[d][3], mode_n[d][0], dyn_max[d]);
        for (unsigned k = 0; k <= NRD_MAX_UBO; k++) if (stat_h[d][k]) printf(" %u:%u", k, stat_h[d][k]);
        printf("\n");
    }
}

int main(int argc, char **argv) {
    plan_units(); hazard_units(); table_units(); fifo_units(); fifo_model(); bench();
    if (argc >= 3) bundles(argv[1], argv[2]);
    printf("%s: %ld checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
