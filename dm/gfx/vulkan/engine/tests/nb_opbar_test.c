// NB-08 host test: dm/gfx/nb/nb_opbar.h (DM_NB_OPBARRIER read-barrier bookkeeping of narrowed copies / clears).
// clang -std=c11 -O2 -Wall -Wextra dm/gfx/vulkan/engine/tests/nb_opbar_test.c -o nb_opbar_test && ./nb_opbar_test
//
// A model of the host's image-hazard bookkeeping (xbg_game.cpp: write_seq, barrier_seq, frag_seq, Image::last_write / storage_write,
// prepare_read, full_barrier / frag_barrier, Commands vs OpCommands, the slot-begin barrier) in three worlds run on the same event
// script: OLD (DM_NB_OPBARRIER=0), NARROW (=1) and CHECK (=2: old barriers executed + the shadow). Checked:
//   (1) soundness of NARROW against an oracle of Vulkan visibility (per image: up to which write it is visible to all stages / to the
//       fragment stages, moved by full / fragment barriers and by an op's own after-barrier): whenever NARROW records no barrier
//       before a read, the oracle says the read sees every write it must see;
//   (2) CHECK executes exactly what OLD executes (same barrier decision at every read);
//   (3) CHECK's shadow predicts NARROW's decision at every read (the =2 numbers are the =1 behaviour);
//   (4) the counters: NARROW's read barriers = OLD's + CHECK's EXTRA - CHECK's SAVED (exact), NARROW's "caused" <= EXTRA (lower bound).
#include "../../../nb/nb_opbar.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

enum { NIMG = 8 };
typedef struct { uint64_t last_write, storage_write, vis_full, vis_frag, opw; } img_t;
typedef struct {
    int mode, tile;
    uint64_t write_seq, barrier_seq, frag_seq, sh_b, sh_f, skip_seq;
    unsigned in_op; int mute;
    img_t im[NIMG];
    uint64_t full_barriers, frag_barriers, read_barriers, caused, extra, saved, narrowed_ops;
} world;

static void w_init(world *w, int mode, int tile) { memset(w, 0, sizeof *w); w->mode = mode; w->tile = tile; w->write_seq = 1; }
// full_barrier / frag_barrier / the slot-begin barrier of recording_slot (sets barrier_seq only)
static void w_full(world *w) {
    w->barrier_seq = w->frag_seq = w->write_seq; ++w->full_barriers;
    if (w->mode == 2 && !w->in_op && !w->mute) w->sh_b = w->sh_f = w->write_seq;
}
static void w_frag(world *w) {
    w->frag_seq = w->write_seq; ++w->frag_barriers;
    if (w->mode == 2 && !w->in_op && !w->mute) w->sh_f = w->write_seq;
}
static void w_slot(world *w) { w->barrier_seq = w->write_seq; if (w->mode == 2) w->sh_b = w->write_seq; }
static void w_pass_write(world *w, int i) { w->im[i].last_write = ++w->write_seq; }          // prepare_attachment
static void w_storage_write(world *w, int i) { w->im[i].storage_write = ++w->write_seq; }    // mark_storage_images
// OpCommands: ops on images a (source or the cleared image) and b (destination, -1 = none)
static void w_op(world *w, int a, int b) {
    const int narrow = w->mode == 1; int imgs[2] = {a, b}; const int n = b >= 0 ? 2 : 1;
    if (w->mode) ++w->in_op;
    if (!narrow) w_full(w);
    for (int k = 0; k < n; k++) {
        img_t *x = &w->im[imgs[k]]; const uint64_t seq = ++w->write_seq;
        if (w->mode == 1) x->last_write = seq; else if (w->mode == 2) x->opw = seq;
        if (w->mode) nb_ob_stamp(&x->vis_full, &x->vis_frag, seq, w->tile);
    }
    if (!narrow) w_full(w);
    if (narrow) { w->skip_seq = w->write_seq; ++w->narrowed_ops; }
    if (w->mode) --w->in_op;
}
// prepare_read (other = layout not GENERAL / attachment of the open pass: not modelled, 0). Returns 1 when a barrier is recorded.
static int w_read(world *w, int i, int frag, int as_storage, int *narrow_says) {
    img_t *x = &w->im[i];
    uint64_t seen = frag ? w->frag_seq : w->barrier_seq;
    if (w->mode == 1) seen = nb_ob_seen(seen, frag ? x->vis_frag : x->vis_full);
    const int written = nb_ob_written(x->last_write, x->storage_write, as_storage, seen);
    if (narrow_says) *narrow_says = written;
    if (w->mode == 1 && nb_ob_caused(written, x->last_write, x->storage_write, as_storage, w->skip_seq)) ++w->caused;
    if (w->mode == 2) {
        const uint64_t lw = x->last_write > x->opw ? x->last_write : x->opw;
        const int wn = nb_ob_written(lw, x->storage_write, as_storage, nb_ob_seen(frag ? w->sh_f : w->sh_b, frag ? x->vis_frag : x->vis_full));
        if (narrow_says) *narrow_says = wn;
        const int r = nb_ob_shadow_read(wn, written, 0);
        if (r == NB_OB_EXTRA) { ++w->extra; if (frag) w->sh_f = w->write_seq; else w->sh_b = w->sh_f = w->write_seq; }
        else if (r == NB_OB_SAVED) { ++w->saved; w->mute = 1; }
    }
    if (written) { ++w->read_barriers; if (frag) w_frag(w); else w_full(w); }
    w->mute = 0;
    return written;
}

// Oracle: Vulkan visibility of each image's writes (the newest write sequence of each kind and up to which write it is visible).
typedef struct { uint64_t seq, last_write, storage_write, vis_all[NIMG], vis_frag[NIMG]; uint64_t lw[NIMG], sw[NIMG]; int tile; } oracle;
static void o_full(oracle *o) { for (int i = 0; i < NIMG; i++) { uint64_t m = o->lw[i] > o->sw[i] ? o->lw[i] : o->sw[i]; o->vis_all[i] = o->vis_frag[i] = m; } }
static void o_frag(oracle *o) { for (int i = 0; i < NIMG; i++) { uint64_t m = o->lw[i] > o->sw[i] ? o->lw[i] : o->sw[i]; o->vis_frag[i] = m; } }
static void o_op(oracle *o, int a, int b) {   // before: ALL -> TRANSFER (op images); the op writes; after: TRANSFER -> frag (tile) / ALL
    int imgs[2] = {a, b}; const int n = b >= 0 ? 2 : 1;
    for (int k = 0; k < n; k++) { const int i = imgs[k]; o->lw[i] = ++o->seq; uint64_t m = o->lw[i] > o->sw[i] ? o->lw[i] : o->sw[i];
        o->vis_frag[i] = m; if (!o->tile) o->vis_all[i] = m; }
}
static int o_needs(const oracle *o, int i, int frag, int as_storage) {
    const uint64_t v = frag ? o->vis_frag[i] : o->vis_all[i];
    return o->lw[i] > v || (!as_storage && o->sw[i] > v);
}

static void units(void) {
    for (int tile = 0; tile < 2; tile++) {
        world old, nar, chk; w_init(&old, 0, tile); w_init(&nar, 1, tile); w_init(&chk, 2, tile);
        world *all[3] = {&old, &nar, &chk};
        int n0, n1, n2;
        // (a) a pass writes 0; a copy 1 -> 2; a draw samples 0: the old copy barrier covered it, the narrow path barriers now.
        for (int k = 0; k < 3; k++) { w_pass_write(all[k], 0); w_op(all[k], 1, 2); }
        CHECK(!w_read(&old, 0, 1, 0, &n0) && w_read(&nar, 0, 1, 0, &n1) && !w_read(&chk, 0, 1, 0, &n2) && n1 == n2);
        CHECK(nar.caused == 1 && chk.extra == 1 && old.full_barriers == 2 && nar.full_barriers == 0 && nar.frag_barriers == 1);
        // (b) a fragment read of the copy's destination needs nothing in any world.
        for (int k = 0; k < 3; k++) w_op(all[k], 1, 2);
        CHECK(!w_read(&old, 2, 1, 0, 0) && !w_read(&nar, 2, 1, 0, 0) && !w_read(&chk, 2, 1, 0, &n2) && !n2);
        // (c) a vertex / compute read of it: a barrier only on a tiler (after-barrier waits in the fragment stages only).
        for (int k = 0; k < 3; k++) w_op(all[k], 1, 2);
        CHECK(w_read(&nar, 2, 0, 0, 0) == tile && !w_read(&old, 2, 0, 0, 0) && !w_read(&chk, 2, 0, 0, &n2) && n2 == tile);
        // (d) the source too (its layout transition is a write): a vertex read of 1 after a copy from it.
        for (int k = 0; k < 3; k++) w_op(all[k], 1, 3);
        CHECK(w_read(&nar, 1, 0, 0, 0) == tile && !w_read(&chk, 1, 0, 0, &n2) && n2 == tile);
        // (e) a pass writes the destination after the op: every world barriers its read.
        for (int k = 0; k < 3; k++) { w_op(all[k], 1, 2); w_pass_write(all[k], 2); }
        CHECK(w_read(&old, 2, 1, 0, 0) && w_read(&nar, 2, 1, 0, 0) && w_read(&chk, 2, 1, 0, 0));
        // (f) a storage read ignores earlier storage writes (as the host does); a sampled read does not.
        for (int k = 0; k < 3; k++) { w_storage_write(all[k], 4); w_op(all[k], 5, -1); }
        CHECK(!w_read(&nar, 4, 1, 1, 0) && w_read(&nar, 4, 1, 0, 0));
    }
}

static void stream(int tile, int seed) {
    world old, nar, chk; w_init(&old, 0, tile); w_init(&nar, 1, tile); w_init(&chk, 2, tile);
    world *all[3] = {&old, &nar, &chk};
    oracle o; memset(&o, 0, sizeof o); o.seq = 1; o.tile = tile;
    rng = 0x9e3779b97f4a7c15ull ^ (uint64_t)seed * 0x2545F4914F6CDD1Dull;
    uint64_t reads = 0, unsound = 0, old_vs_chk = 0, shadow_vs_nar = 0, over = 0;
    for (int ev = 0; ev < 200000; ev++) {
        const unsigned r = (unsigned)(rnd() % 100); const int i = (int)(rnd() % NIMG); int j = (int)(rnd() % NIMG); if (j == i) j = (i + 1) % NIMG;
        if (r < 25) { for (int k = 0; k < 3; k++) w_pass_write(all[k], i); o.lw[i] = ++o.seq; }
        else if (r < 30) { for (int k = 0; k < 3; k++) w_storage_write(all[k], i); o.sw[i] = ++o.seq; }
        else if (r < 40) { const int b = rnd() & 1 ? j : -1; for (int k = 0; k < 3; k++) w_op(all[k], i, b); o_op(&o, i, b); }
        else if (r < 42) { for (int k = 0; k < 3; k++) w_slot(all[k]); o_full(&o); }
        else {
            const int frag = tile ? (int)(rnd() & 1) : 0, st = (rnd() % 8) == 0;   // without TILE_BARRIER every read is a full one
            int d0, d1, d2; const int b0 = w_read(&old, i, frag, st, &d0), b1 = w_read(&nar, i, frag, st, &d1), b2 = w_read(&chk, i, frag, st, &d2);
            ++reads;(void)d0;
            const int need = o_needs(&o, i, frag, st);
            if (!b1 && need) ++unsound;
            if (b1 && !need) ++over;
            if (b1) { if (frag) o_frag(&o); else o_full(&o); }
            if (b0 != b2) ++old_vs_chk;
            if (d1 != d2) ++shadow_vs_nar;
        }
    }
    CHECK(unsound == 0); CHECK(old_vs_chk == 0); CHECK(shadow_vs_nar == 0);
    CHECK(nar.read_barriers == old.read_barriers + chk.extra - chk.saved);   // the =2 numbers are exact
    CHECK(nar.caused <= chk.extra);   // =1 sees only the reads the LAST narrowed op would have covered: a lower bound
    CHECK(old.full_barriers == chk.full_barriers);
    printf("stream tile=%d seed=%d: %llu reads; narrow: %llu ops narrowed (%llu full barriers not recorded), read barriers %llu "
           "(old %llu, caused %llu, check extra %llu / saved %llu), oracle-unneeded barriers %llu, unsound %llu, old!=check %llu, shadow!=narrow %llu\n",
           tile, seed, (unsigned long long)reads, (unsigned long long)nar.narrowed_ops, (unsigned long long)(2 * nar.narrowed_ops),
           (unsigned long long)nar.read_barriers, (unsigned long long)old.read_barriers, (unsigned long long)nar.caused,
           (unsigned long long)chk.extra, (unsigned long long)chk.saved, (unsigned long long)over, (unsigned long long)unsound,
           (unsigned long long)old_vs_chk, (unsigned long long)shadow_vs_nar);
}

// Mutation: the stamp ignoring tile (after-barrier claimed visible to all stages on a tiler) must be caught as unsound.
static void mutation(void) {
    world nar; w_init(&nar, 1, 1); oracle o; memset(&o, 0, sizeof o); o.seq = 1; o.tile = 1;
    w_op(&nar, 1, 2); o_op(&o, 1, 2);
    nar.im[2].vis_full = nar.im[2].vis_frag;   // the bug
    int d; const int b = w_read(&nar, 2, 0, 0, &d);
    CHECK(!b && o_needs(&o, 2, 0, 0));          // the oracle sees the missing barrier
}

int main(void) {
    units();
    for (int tile = 0; tile < 2; tile++) for (int seed = 1; seed <= 4; seed++) stream(tile, seed);
    mutation();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("all passed\n");
    return 0;
}
