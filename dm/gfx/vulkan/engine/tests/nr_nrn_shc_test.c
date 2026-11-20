// [DOS2] NR-NATIVE shadows, DM_NRN_SHC (docs/dos2/nr/NR-NATIVE.md section 11): host test of dm/gfx/nr/nr_nrn_shc.h - the tape of a cascade
// draw's structure hash against the chain hash() of NR-SHCACHE folds directly.
//   (1) equivalence: random draws (a model of what hash() reads: words and small structures, some of them long, some tapes longer than
//       the scratch), every one hashed twice - the direct chain (nrshc_mix / nrshc_bytes call by call, the NVN path of today) and the
//       tape under the draw's card: the two values are equal for every draw, whatever the table holds.
//   (2) a scene: 600 cards drawn for 300 frames; some inputs of some draws change (one byte of a structure, one word, the length of a
//       structure, the number of items); the tape must answer "equal" exactly when no input changed since the card's last draw, and
//       its value must be the direct chain's in both cases; two cards that share a slot push each other out and stay exact.
//   (3) the pieces: a draw without a card, a tape of exactly the kept capacity, one byte over, a string longer than NRNT_BIG.
// Build: clang -std=c11 -O2 -Wall -Wextra nr_nrn_shc_test.c -o nr_nrn_shc_test        (mutations: -DNRNT_MUTATE=1, 3, 4, 5, 6, expect exit code 1)
#include <stdio.h>
#include "../../../nr/nr_shc.h"
#include "../../../nr/nr_nrn_shc.h"

static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 12) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_seed = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// A draw as hash() reads it: items in call order.
enum { ITEMS = 96, BLOB = 6000 };
typedef struct { uint8_t kind; uint16_t len; uint32_t off; uint64_t w; } item;
typedef struct { uint32_t n; item it[ITEMS]; uint8_t blob[BLOB]; uint32_t used; } draw;
static void draw_make(draw *d, unsigned items, int big) {
    d->n = 0; d->used = 0;
    for (unsigned i = 0; i < items && d->n < ITEMS; i++) {
        item *t = &d->it[d->n];
        if (rn(3)) { t->kind = 0; t->w = rnd(); }
        else { unsigned len = big && !rn(8) ? 4097 + rn(600) : rn(3) ? 4 + rn(60) : rn(400);
            if (d->used + len > BLOB) continue;
            t->kind = 1; t->len = (uint16_t)len; t->off = d->used; for (unsigned k = 0; k < len; k++) d->blob[d->used + k] = (uint8_t)rnd(); d->used += len; }
        d->n++;
    }
}
static uint64_t direct(const draw *d) {   // hash(): the chain call by call
    uint64_t h = 0x53484331;
    for (uint32_t i = 0; i < d->n; i++) h = d->it[i].kind ? nrshc_bytes(h, d->blob + d->it[i].off, d->it[i].len) : nrshc_mix(h, d->it[i].w);
    return h;
}
static uint64_t taped(nrnt *t, const draw *d, uint64_t card) {   // hash_ser(): the same calls onto the tape
    nrnt_begin(t);
    for (uint32_t i = 0; i < d->n; i++) { if (d->it[i].kind) nrnt_bytes(t, d->blob + d->it[i].off, d->it[i].len); else nrnt_word(t, d->it[i].w); }
    return nrnt_end(t, card);
}
static uint32_t tape_bytes(const draw *d) { uint32_t n = 0; for (uint32_t i = 0; i < d->n; i++) n += d->it[i].kind ? 3u + d->it[i].len : 9u; return n; }

static void test_equiv(nrnt *t) {
    static draw d;
    for (int i = 0; i < 40000; i++) {
        draw_make(&d, 1 + rn(i % 50 == 0 ? ITEMS : 70), i % 7 == 0);
        const uint64_t card = rn(4) ? (rnd() | 1) : 0, a = direct(&d);
        CHECK(taped(t, &d, card) == a, "equivalence: the tape's hash is the chain's (draw %d, %u items, %u tape bytes)", i, d.n, tape_bytes(&d));
        CHECK(taped(t, &d, card) == a, "equivalence: the same draw again (answered from the kept tape or hashed)");
        if (g_fail > 12) return;
    }
}

enum { CARDS = 600, FRAMES = 300 };
static draw g_scene[CARDS]; static uint64_t g_card[CARDS]; static uint8_t g_dirty[CARDS]; static int g_slot_last[NRNT_ENT];
static unsigned slot_of(uint64_t key) { return (unsigned)(key ^ key >> 32) & (NRNT_ENT - 1); }
static void test_scene(nrnt *t) {
    for (int c = 0; c < CARDS; c++) { draw_make(&g_scene[c], 10 + rn(40), 0); g_card[c] = ((uint64_t)(c + 1) * 0x9e3779b97f4a7c15ull) | 1; g_dirty[c] = 1; }
    for (int i = 0; i < NRNT_ENT; i++) g_slot_last[i] = -1;   // which card's tape a slot holds (600 cards in 2048 slots: some share one and push each other out)
    int shared = 0;
    uint64_t equal = 0, hashed = 0, clean = 0, clean_equal = 0;
    for (int f = 0; f < FRAMES; f++) {
        for (int c = 0; c < CARDS; c++) {
            draw *d = &g_scene[c];
            if (!rn(25) && d->n) {   // an input of this draw changes
                const unsigned i = rn(d->n), how = rn(4);
                if (d->it[i].kind && d->it[i].len && how == 0) d->blob[d->it[i].off + rn(d->it[i].len)] ^= (uint8_t)(1u << rn(8));
                else if (d->it[i].kind && d->it[i].len > 1 && how == 1) d->it[i].len--;
                else if (!d->it[i].kind) d->it[i].w ^= 1ull << rn(64);
                else if (d->n > 2) d->n--;
                else d->it[i].w ^= 1, d->it[i].kind = 0;
                g_dirty[c] = 1;
            }
            const uint64_t e0 = t->c[NRNT_C_EQUAL], a = direct(d), b = taped(t, d, g_card[c]);
            const int was_equal = t->c[NRNT_C_EQUAL] != e0;
            CHECK(a == b, "scene: the tape's hash is the chain's (frame %d card %d, %s)", f, c, was_equal ? "answered from the kept tape" : "hashed");
            const int keeps = tape_bytes(d) <= NRNT_CAP; const unsigned sl = slot_of(g_card[c]) >> 1;   // (the two ways of a pair)
            if (g_slot_last[sl] >= 0 && g_slot_last[sl] != c) shared++;
            CHECK(!was_equal || (keeps && !g_dirty[c]), "scene: never answered without hashing when an input changed or the tape is not kept (frame %d card %d: kept %d, dirty %d)", f, c, keeps, g_dirty[c]);
            if (keeps && !g_dirty[c]) { clean++; clean_equal += (uint64_t)was_equal; }
            if (keeps) g_slot_last[sl] = c;
            equal += (uint64_t)was_equal; hashed += (uint64_t)!was_equal; g_dirty[c] = 0;
            if (g_fail > 12) return;
        }
    }
    CHECK(clean_equal * 100 >= clean * 93 && shared > 100 && equal > hashed * 3, "scene: an unchanged draw is answered from its tape (%llu of %llu; all draws: %llu equal, %llu hashed; %d draws had another card in their pair of slots)", clean_equal, clean, equal, hashed, shared);
}

static void test_pieces(nrnt *t) {
    static draw d; uint8_t buf[NRNT_CAP + 64]; for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)rnd();
    // exactly the capacity, and one byte over
    for (int over = 0; over < 2; over++) {
        d.n = 1; d.used = 0; d.it[0].kind = 1; d.it[0].off = 0; d.it[0].len = (uint16_t)(NRNT_CAP - 3 + over); memcpy(d.blob, buf, d.it[0].len);
        const uint64_t card = 0x1234567890abcdefull + (uint64_t)over, a = direct(&d), k0 = t->c[NRNT_C_KEPT], l0 = t->c[NRNT_C_LONG];
        CHECK(taped(t, &d, card) == a && taped(t, &d, card) == a, "pieces: a tape of %s the kept capacity hashes as the chain", over ? "one byte over" : "exactly");
        CHECK(over ? t->c[NRNT_C_LONG] == l0 + 2 && t->c[NRNT_C_KEPT] == k0 : t->c[NRNT_C_KEPT] == k0 + 1, "pieces: kept exactly up to the capacity");
    }
    // no card: the key of the first words
    draw_make(&d, 12, 0);
    { const uint64_t n0 = t->c[NRNT_C_NOCARD], e0 = t->c[NRNT_C_EQUAL], a = direct(&d);
      CHECK(taped(t, &d, 0) == a && taped(t, &d, 0) == a && t->c[NRNT_C_NOCARD] == n0 + 2 && (tape_bytes(&d) > NRNT_CAP || t->c[NRNT_C_EQUAL] == e0 + 1), "pieces: a draw without a card is kept under a key of its first words"); }
    // a string longer than NRNT_BIG in the middle, and a tape longer than the scratch
    d.n = 3; d.used = 0; d.it[0].kind = 0; d.it[0].w = 5; d.it[1].kind = 1; d.it[1].off = 0; d.it[1].len = 5000; for (unsigned i = 0; i < 5000; i++) d.blob[i] = (uint8_t)rnd(); d.it[2].kind = 0; d.it[2].w = 9;
    { const uint64_t l0 = t->c[NRNT_C_LONG]; CHECK(taped(t, &d, 77) == direct(&d) && t->c[NRNT_C_LONG] == l0 + 1, "pieces: a long string is folded at once, the tape is not kept"); }
    d.n = ITEMS; d.used = 0; for (unsigned i = 0; i < ITEMS; i++) { d.it[i].kind = 1; d.it[i].off = 0; d.it[i].len = 300; } for (unsigned i = 0; i < 300; i++) d.blob[i] = (uint8_t)rnd();
    { const uint64_t l0 = t->c[NRNT_C_LONG]; CHECK(tape_bytes(&d) > NRNT_SCRATCH && taped(t, &d, 78) == direct(&d) && taped(t, &d, 78) == direct(&d) && t->c[NRNT_C_LONG] == l0 + 2, "pieces: a tape longer than the scratch is folded in parts, never kept"); }
    // an empty tape
    d.n = 0; CHECK(taped(t, &d, 79) == 0x53484331ull && direct(&d) == 0x53484331ull, "pieces: an empty tape is the seed");
    // a word and an 8-byte string are different items
    { draw x; x.n = 1; x.used = 8; x.it[0].kind = 0; x.it[0].w = 0x1122334455667788ull; draw y = x; y.it[0].kind = 1; y.it[0].off = 0; y.it[0].len = 8; memcpy(y.blob, &x.it[0].w, 8);
      CHECK(taped(t, &x, 80) == direct(&x) && taped(t, &y, 80) == direct(&y) && direct(&x) != direct(&y), "pieces: a word and a string of the same bytes are different inputs"); }
}

int main(void) {
    nrnt *t = (nrnt *)malloc(sizeof *t);
    CHECK(t && nrnt_init(t) == 0, "the tape");
    test_equiv(t); test_scene(t); test_pieces(t);
    printf("nr_nrn_shc_test%s: %llu checks, %llu failed: %s\n", NRNT_MUTATE ? " (mutation)" : "", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    nrnt_free(t); free(t);
    return g_fail ? 1 : 0;
}
