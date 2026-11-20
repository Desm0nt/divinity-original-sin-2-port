// NR-10 host test: dm/gfx/nr/nr_rp.h (DM_NR_RP: render-pass visibility points, the barrier before an IR pass, fresh attachments).
// gcc -std=c11 -O2 -Wall -Wextra dm/gfx/vulkan/engine/tests/nr_rp_test.c -o nr_rp_test && ./nr_rp_test [nb_ir_*.txt ...]
//
// A model of the host's image-hazard bookkeeping (xbg_game.cpp: write_seq, barrier_seq, frag_seq, Image::last_write / prior_write /
// storage_write, the op points of NB-08 (always on here, as on the phone), prepare_read, the pre-pass barrier of xbg_nb.inc, pass open /
// continue / end) in three worlds on the same event script: OLD (DM_NR_RP=0), NARROW (=1), CHECK (=2: old barriers + the shadow).
//   (1) soundness against an oracle of Vulkan visibility, per image SUBRESOURCE (two each): up to which write it is visible to every
//       stage / to the fragment stages. Moved by full / fragment barriers, by an op's after-barrier (whole image) and by the end of a
//       render pass (dep[1]: the attached subresources only). A read with no barrier must see every write of the image. OLD must be
//       sound too (it checks the oracle and the model);
//   (2) CHECK executes exactly what OLD executes;
//   (3) CHECK's shadow predicts NARROW's decision at every read and at every pre-pass barrier, except in the wake of a pass split
//       that only one of the two worlds has (these random streams split passes far more often than a frame does): < 0.1 % of the events;
//   (4) mutations of the rules are caught by the oracle: stamping a pass that did not begin clean, an all-stage point under the tile
//       policy, a render-pass point that covers a storage write.
// With IR dumps of the phone (work/logs/*-dumps/nb_ir_*.txt) as arguments: the frame is replayed in OLD and NARROW, tile policy off
// and on, and the barriers of a steady-state frame are printed (reads are taken as the first draw's, fragment stage).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../nr/nr_rp.h"
#include "../../../nb/nb_opbar.h"

enum { NI = 64, NR_IMG = 10, NA = 6, FRAG = 0, VERT = 1 };   // NI: image slots (replay), NR_IMG: images of the random streams
enum { MUT_NOCLEAN = 1, MUT_TILEFULL = 2, MUT_STORAGE = 4 };
typedef struct { uint64_t last_write, prior_write, storage_write, opvis_full, opvis_frag, nrvis_full, nrvis_frag; } Img;
typedef struct {
    int nr, tile, mut;                                  // nr: 0 old, 1 narrow, 2 check
    uint64_t write_seq, barrier_seq, frag_seq, sh_b, sh_f; int mute;
    Img im[NI];
    int open, natt, att[NA], sub[NA], clean, clean_f;   // the open pass
    uint64_t full, frag, read_bar, pb_bar, splits, unsound, stamps, dirty;
    int last_dec;                                       // decision of the last read / pre-pass barrier in the NARROW sense (NR_PB_*)
    uint64_t oclk, ow[NI][2], oall[NI][2], ofr[NI][2];  // oracle
} W;
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

// ---- oracle ----
static void o_write(W *w, int i, int s) { w->ow[i][s] = ++w->oclk; }
static void o_full(W *w) { for (int i = 0; i < NI; i++) for (int s = 0; s < 2; s++) w->oall[i][s] = w->ofr[i][s] = w->ow[i][s]; }
static void o_frag(W *w) { for (int i = 0; i < NI; i++) for (int s = 0; s < 2; s++) w->ofr[i][s] = w->ow[i][s]; }
static void o_vis(W *w, int i, int s) { w->ofr[i][s] = w->ow[i][s]; if (!w->tile) w->oall[i][s] = w->ow[i][s]; }
static int o_sees(const W *w, int i, int stage) {
    for (int s = 0; s < 2; s++) if ((stage == FRAG ? w->ofr[i][s] : w->oall[i][s]) < w->ow[i][s]) return 0;
    return 1;
}
// ---- host ----
static void h_full(W *w) { w->barrier_seq = w->frag_seq = w->write_seq; w->full++; if (w->nr == 2 && !w->mute) w->sh_b = w->sh_f = w->write_seq; o_full(w); }
static void h_frag(W *w) { w->frag_seq = w->write_seq; w->frag++; if (w->nr == 2 && !w->mute) w->sh_f = w->write_seq; o_frag(w); }
static uint64_t gb(const W *w) { return w->nr == 2 ? w->sh_b : w->barrier_seq; }   // the narrow world's points (nrrp::gb / gf)
static uint64_t gf(const W *w) { return w->nr == 2 ? w->sh_f : w->frag_seq; }
static uint64_t seen_full(const W *w, const Img *m) { return nb_ob_seen(gb(w), m->opvis_full); }
static uint64_t seen_frag(const W *w, const Img *m) { return nb_ob_seen(gf(w), m->opvis_frag); }
static int n_written(const W *w, const Img *m, int fo, uint64_t rp) {   // nr_rp_written with the rules' mutations
    const uint64_t seen = fo ? seen_frag(w, m) : seen_full(w, m);
    if (w->mut & MUT_STORAGE) return m->last_write > nr_rp_max(seen, rp) || m->storage_write > nr_rp_max(seen, rp);
    return nr_rp_written(m->last_write, m->storage_write, 0, seen, rp);
}
static void end_pass(W *w) {
    if (!w->open) return;
    for (int k = 0; k < w->natt; k++) o_vis(w, w->att[k], w->sub[k]);   // dep[1] of the pass: its attachment writes
    if (w->nr) {
        const int cf = (w->mut & MUT_NOCLEAN) ? 1 : w->clean_f, cb = (w->mut & MUT_NOCLEAN) ? 1 : w->clean;
        if (cf) { w->stamps++; for (int k = 0; k < w->natt; k++) { Img *m = &w->im[w->att[k]];
            nr_rp_pass_end(&m->nrvis_full, &m->nrvis_frag, m->last_write, cb, cf, (w->mut & MUT_TILEFULL) ? 0 : w->tile); } }
        else w->dirty++;
    }
    w->open = 0;
}
static int in_open(const W *w, int i) { if (w->open) for (int k = 0; k < w->natt; k++) if (w->att[k] == i) return 1; return 0; }
// prepare_read of a sampled image (xbg_game.cpp), then the oracle
static void h_read(W *w, int i, int stage) {
    Img *m = &w->im[i]; const int fo = w->tile && stage == FRAG, io = in_open(w, i);
    uint64_t seen = fo ? w->frag_seq : w->barrier_seq; seen = nb_ob_seen(seen, fo ? m->opvis_frag : m->opvis_full);
    const int written = w->nr == 1 ? n_written(w, m, fo, fo ? m->nrvis_frag : m->nrvis_full) : (m->last_write > seen || m->storage_write > seen);
    int dec = (written || io) ? (fo ? NR_PB_FRAG : NR_PB_FULL) : NR_PB_NONE;
    if (w->nr == 2) {   // nrrp_read
        uint64_t rp = fo ? m->nrvis_frag : m->nrvis_full;
        if (io && (fo ? w->clean_f : w->clean && !w->tile)) rp = nr_rp_max(rp, m->last_write);
        const int narrow = n_written(w, m, fo, rp), old = written || io;
        dec = narrow ? (fo ? NR_PB_FRAG : NR_PB_FULL) : NR_PB_NONE;
        if (old && !narrow) w->mute = 1;
        else if (narrow && !old) {   // the narrow path barriers here: an open pass ends (stamped if it began clean) and begins again behind the barrier
            if (w->open) { if (w->clean_f) for (int k = 0; k < w->natt; k++) { Img *a = &w->im[w->att[k]]; nr_rp_pass_end(&a->nrvis_full, &a->nrvis_frag, a->last_write, w->clean, w->clean_f, w->tile); }
                w->clean_f = 1; if (!fo) w->clean = 1; }
            if (fo) w->sh_f = w->write_seq; else w->sh_b = w->sh_f = w->write_seq; }
    }
    if (written || io) {
        end_pass(w);
        int bar = 1;
        if (w->nr == 1 && io) bar = n_written(w, m, fo, fo ? m->nrvis_frag : m->nrvis_full);
        if (bar) { if (fo) h_frag(w); else h_full(w); w->read_bar++; }
        if (w->nr == 1) dec = bar ? (fo ? NR_PB_FRAG : NR_PB_FULL) : NR_PB_NONE;
    }
    w->mute = 0; w->last_dec = dec;
    if (!o_sees(w, i, stage)) w->unsound++;
}
static void set_clean(W *w) {
    w->clean = w->clean_f = 1;
    for (int k = 0; k < w->natt; k++) { const Img *m = &w->im[w->att[k]];
        const uint64_t sw = (w->mut & MUT_STORAGE) ? 0 : m->storage_write;
        if (!nr_rp_clean(m->prior_write, sw, seen_full(w, m), m->nrvis_full)) w->clean = 0;
        if (!nr_rp_clean(m->prior_write, sw, seen_frag(w, m), m->nrvis_frag)) w->clean_f = 0; }
}
// draw() opening a pass on (att, sub)[n]; rd[nrd] = the header's reads when the IR pass has NB_NP_PREBAR (nrd < 0: none)
static void h_open(W *w, int n, const int *att, const int *sub, int nrd, const int *rd, int frag_first) {
    for (int k = 0; k < n; k++) { Img *m = &w->im[att[k]]; m->prior_write = m->last_write; m->last_write = ++w->write_seq; }   // create_pass: prepare_attachment
    end_pass(w);
    w->natt = n; for (int k = 0; k < n; k++) { w->att[k] = att[k]; w->sub[k] = sub[k]; }
    w->last_dec = -1;
    if (nrd >= 0) {   // nbrp::prebar
        int af = 0, afr = 0;
        for (int j = 0; j < nrd; j++) { const Img *m = &w->im[rd[j]]; int own = 0; for (int k = 0; k < n; k++) if (att[k] == rd[j]) own = 1; if (own) continue;
            if (n_written(w, m, 0, m->nrvis_full)) af = 1;
            if (n_written(w, m, 1, m->nrvis_frag)) afr = 1; }
        const int narrow = nr_rp_prebar(af, afr, 0, w->tile, frag_first), old = w->write_seq > w->barrier_seq ? NR_PB_FULL : NR_PB_NONE;
        w->last_dec = narrow;
        if (w->nr == 1) {
            if (narrow) { if (narrow == NR_PB_FRAG) h_frag(w); else h_full(w); for (int k = 0; k < n; k++) w->im[att[k]].last_write = ++w->write_seq; w->pb_bar++; }
        } else {
            if (w->nr == 2) { const int c = nr_rp_compare(old, narrow);
                if (c == NR_CMP_MORE) { if (narrow == NR_PB_FULL) w->sh_b = w->sh_f = w->write_seq; else w->sh_f = w->write_seq; }
                else if (c != NR_CMP_SAME) { w->mute = 1; if (c == NR_CMP_FRAG) w->sh_f = w->write_seq; } }
            if (old) { h_full(w); for (int k = 0; k < n; k++) w->im[att[k]].last_write = ++w->write_seq; w->pb_bar++; }
            w->mute = 0;
        }
    }
    w->open = 1; if (w->nr) set_clean(w);
    for (int k = 0; k < n; k++) o_write(w, att[k], sub[k]);
}
// a later draw of the open pass that samples image i (not an attachment of the pass)
static void h_draw_read(W *w, int i, int stage) {
    int n = w->natt, att[NA], sub[NA]; memcpy(att, w->att, sizeof att); memcpy(sub, w->sub, sizeof sub);
    const int was = w->open;
    h_read(w, i, stage);
    if (!was) return;
    if (!w->open) { const int d = w->last_dec; w->splits++; h_open(w, n, att, sub, -1, NULL, 0); w->last_dec = d; }   // the split pass begins again (LOAD), no pre-pass barrier
    else for (int k = 0; k < n; k++) { Img *m = &w->im[att[k]]; m->prior_write = m->last_write; m->last_write = ++w->write_seq; }   // pass_fast
}
static void h_op(W *w, int i) {   // OpCommands, DM_NB_OPBARRIER=1: a copy / clear of the whole image, barriers on it only
    Img *m = &w->im[i]; end_pass(w);
    m->prior_write = m->last_write; m->last_write = ++w->write_seq; nb_ob_stamp(&m->opvis_full, &m->opvis_frag, m->last_write, w->tile);
    o_write(w, i, 0); o_write(w, i, 1); o_vis(w, i, 0); o_vis(w, i, 1);
}
static void h_storage(W *w, int i) { w->im[i].storage_write = ++w->write_seq; o_write(w, i, 0); }   // a draw / dispatch wrote it as a storage image
static void h_raw(W *w, int i) { Img *m = &w->im[i]; end_pass(w); m->prior_write = m->last_write; m->last_write = ++w->write_seq; o_write(w, i, 1); }   // a write of ANOTHER subresource nothing made visible

static uint32_t g_rng;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
typedef struct { uint64_t full, frag, read_bar, pb_bar, splits, unsound, stamps, dirty, diff_exec, diff_shadow, events; } Res;

// One script in the three worlds. raw: include writes nothing makes visible (the hole the clean rule closes).
static void run(uint32_t seed, int tile, int n, int raw, int mut, Res *o, Res *nw, Res *ck) {
    static W w[3]; memset(w, 0, sizeof w);
    for (int k = 0; k < 3; k++) { w[k].nr = k; w[k].tile = tile; w[k].mut = k == 1 ? mut : 0; }
    uint64_t diff_exec = 0, diff_shadow = 0;
    g_rng = seed ? seed : 1;
    for (int e = 0; e < n; e++) {
        const uint32_t r = rnd() % 100;
        if (r < 40) {   // a new pass: the first draw's reads, then the pass (with a pre-pass barrier for 2 of 3)
            int na = 1 + (int)(rnd() % 2), att[NA], sub[NA], rd[4], nrd = (int)(rnd() % 3), st[4], dec1[4] = {0}, pb1 = 0;
            att[0] = (int)(rnd() % NR_IMG); att[1] = (att[0] + 1 + (int)(rnd() % (NR_IMG - 1))) % NR_IMG; sub[0] = (int)(rnd() % 2); sub[1] = (int)(rnd() % 2);
            for (int j = 0; j < nrd; j++) { do rd[j] = (int)(rnd() % NR_IMG); while (rd[j] == att[0] || (na > 1 && rd[j] == att[1])); st[j] = rnd() % 5 ? FRAG : VERT; }
            const int pre = rnd() % 3 != 0, extra = (int)(rnd() % NR_IMG), ff = nrd ? st[0] == FRAG : 1;
            int hrd[5], nh = 0; for (int j = 0; j < nrd; j++) hrd[nh++] = rd[j]; hrd[nh++] = extra;   // the header also lists a later draw's read
            for (int k = 0; k < 3; k++) {
                for (int j = 0; j < nrd; j++) { h_read(&w[k], rd[j], st[j]); if (k == 1) dec1[j] = w[k].last_dec; if (k == 2 && dec1[j] != w[k].last_dec) diff_shadow++; }
                h_open(&w[k], na, att, sub, pre ? nh : -1, hrd, ff);
                if (k == 1) pb1 = w[k].last_dec;
                if (k == 2 && pre && pb1 != w[k].last_dec) diff_shadow++;
            }
        } else if (r < 70) {   // a later draw of the open pass reads something
            const int i = (int)(rnd() % NR_IMG), stg = rnd() % 5 ? FRAG : VERT; int d1 = 0;
            for (int k = 0; k < 3; k++) { if (in_open(&w[k], i)) continue; h_draw_read(&w[k], i, stg); if (k == 1) d1 = w[k].last_dec; if (k == 2 && d1 != w[k].last_dec) diff_shadow++; }
        } else if (r < 82) { const int i = (int)(rnd() % NR_IMG); for (int k = 0; k < 3; k++) h_op(&w[k], i); }
        else if (r < 88) { const int i = (int)(rnd() % NR_IMG); for (int k = 0; k < 3; k++) if (!in_open(&w[k], i)) h_storage(&w[k], i); }
        else if (r < 92 && raw) { const int i = (int)(rnd() % NR_IMG); for (int k = 0; k < 3; k++) h_raw(&w[k], i); }
        else if (r < 96) { for (int k = 0; k < 3; k++) { end_pass(&w[k]); h_full(&w[k]); } }   // submission boundary
        else for (int k = 0; k < 3; k++) end_pass(&w[k]);
        if (w[0].full != w[2].full || w[0].frag != w[2].frag || w[0].open != w[2].open) diff_exec++;
    }
    Res *out[3] = {o, nw, ck};
    for (int k = 0; k < 3; k++) { Res *q = out[k]; q->full += w[k].full; q->frag += w[k].frag; q->read_bar += w[k].read_bar; q->pb_bar += w[k].pb_bar; q->splits += w[k].splits;
        q->unsound += w[k].unsound; q->stamps += w[k].stamps; q->dirty += w[k].dirty; q->events += (uint64_t)n; }
    ck->diff_exec += diff_exec; ck->diff_shadow += diff_shadow;
}

static void test_units(void) {
    uint64_t vf = 0, vr = 0;
    nr_rp_pass_end(&vf, &vr, 10, 1, 1, 0); CHECK(vf == 10 && vr == 10);          // no tile policy: every stage
    nr_rp_pass_end(&vf, &vr, 7, 1, 1, 0); CHECK(vf == 10 && vr == 10);           // points only move forward
    vf = vr = 0; nr_rp_pass_end(&vf, &vr, 10, 1, 1, 1); CHECK(vf == 0 && vr == 10);   // tile: the fragment stages only
    vf = vr = 0; nr_rp_pass_end(&vf, &vr, 10, 0, 1, 0); CHECK(vf == 0 && vr == 10);   // not clean against the all-stage points
    vf = vr = 0; nr_rp_pass_end(&vf, &vr, 10, 1, 0, 0); CHECK(vf == 10 && vr == 0);
    CHECK(nr_rp_clean(5, 0, 5, 0) && nr_rp_clean(5, 0, 0, 5) && !nr_rp_clean(6, 0, 5, 5));
    CHECK(!nr_rp_clean(5, 9, 5, 20));                                             // a pending storage write: never clean by a render-pass point
    CHECK(!nr_rp_written(10, 0, 0, 3, 10) && nr_rp_written(11, 0, 0, 3, 10) && !nr_rp_written(11, 0, 0, 11, 0));
    CHECK(nr_rp_written(4, 12, 0, 3, 20) && !nr_rp_written(4, 12, 1, 3, 20));     // storage write: only the global / op point, and not for a storage read
    CHECK(nr_rp_prebar(1, 1, 0, 0, 1) == NR_PB_FULL && nr_rp_prebar(0, 1, 0, 0, 1) == NR_PB_NONE);   // no tile policy: the all-stage test decides
    CHECK(nr_rp_prebar(1, 1, 0, 1, 1) == NR_PB_FRAG && nr_rp_prebar(1, 0, 0, 1, 1) == NR_PB_NONE);   // tile, fragment-only first draw
    CHECK(nr_rp_prebar(1, 0, 0, 1, 0) == NR_PB_FULL && nr_rp_prebar(0, 0, 0, 1, 0) == NR_PB_NONE);   // tile, a pre-fragment reader
    CHECK(nr_rp_prebar(0, 0, 1, 1, 1) == NR_PB_FULL && nr_rp_prebar(0, 0, 1, 0, 0) == NR_PB_FULL);   // unknown reads: the old rule
    CHECK(nr_rp_compare(NR_PB_FULL, NR_PB_FULL) == NR_CMP_SAME && nr_rp_compare(NR_PB_FULL, NR_PB_NONE) == NR_CMP_DROP);
    CHECK(nr_rp_compare(NR_PB_FULL, NR_PB_FRAG) == NR_CMP_FRAG && nr_rp_compare(NR_PB_NONE, NR_PB_FRAG) == NR_CMP_MORE && nr_rp_compare(NR_PB_FRAG, NR_PB_FULL) == NR_CMP_MORE);
    CHECK(nr_rp_fresh(1, 0, 0) && !nr_rp_fresh(0, 0, 0) && !nr_rp_fresh(1, 1, 0) && !nr_rp_fresh(1, 0, 1));
    CHECK(nr_rp_bytes(2160, 972, 0) == 2160ull * 972 * 4 && nr_rp_bytes(16, 16, 8) == 2048);
    // the phone's pattern: pass A renders X, pass B samples X and renders Y, pass C samples Y (rd2: 26 read + 38 pre-pass barriers a frame)
    for (int tile = 0; tile < 2; tile++) for (int nr = 0; nr < 2; nr++) {
        W w; memset(&w, 0, sizeof w); w.nr = nr; w.tile = tile;
        const int X = 0, Y = 1, Z = 2, s0 = 0; int rd[1];
        h_open(&w, 1, &X, &s0, -1, NULL, 1);
        h_read(&w, X, FRAG); rd[0] = X; h_open(&w, 1, &Y, &s0, 1, rd, 1);
        h_read(&w, Y, FRAG); rd[0] = Y; h_open(&w, 1, &Z, &s0, 1, rd, 1);
        end_pass(&w);
        CHECK(!w.unsound);
        if (!nr) CHECK(w.full + w.frag == 4 && w.pb_bar == 2 && w.read_bar == 2);   // old: a read barrier and a full pre-pass barrier per pass
        else CHECK(w.full + w.frag == 0 && w.stamps == 3);                           // narrow: the passes' own dependencies
        if (!nr && tile) CHECK(w.frag == 2 && w.full == 2);                           // old tile: the pre-pass barriers are FULL
        // a vertex-stage read of Y afterwards: every stage on a non-tile GPU already, a full barrier under the tile policy
        h_read(&w, Y, VERT); CHECK(!w.unsound);
        if (nr) CHECK(w.full == (tile ? 1u : 0u));
    }
}

// ---- replay of an IR dump (DM_NB_DUMP text) ----
typedef struct { uint64_t key; } Key;
static Key g_keys[NI]; static int g_nkeys;   // images that are rendered into or cleared; a texture that is only read never needs a barrier
static int key_ix(uint64_t k, int create) { for (int i = 0; i < g_nkeys; i++) if (g_keys[i].key == k) return i; if (create && g_nkeys < NI) { g_keys[g_nkeys].key = k; return g_nkeys++; } return -1; }
typedef struct { int n, att[NA], nrd, rd[40], exec, prebar; } RPass;
typedef struct { int kind; RPass p; int img; } REv;   // kind 0 pass, 1 op on img
static int load_dump(const char *path, REv *ev, int cap) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char line[512]; int n = 0, cur = -1; uint64_t pend_clear[32]; int npc = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "pass", 4) && (line[4] == ' ' || line[4] == '\n' || line[4] == '\r')) {
            if (n >= cap) break;
            cur = n++; memset(&ev[cur], 0, sizeof ev[cur]);
            for (char *q = line; (q = strstr(q, "=0x")); q += 3) { const int ix = key_ix(strtoull(q + 1, NULL, 16), 1); if (ix >= 0 && ev[cur].p.n < NA) ev[cur].p.att[ev[cur].p.n++] = ix; }
        } else if (!strncmp(line, "read 0x", 7) && cur >= 0) { const int ix = key_ix(strtoull(line + 5, NULL, 16), 0); if (ix >= 0 && ev[cur].p.nrd < 40) ev[cur].p.rd[ev[cur].p.nrd++] = ix; }
        else if (!strncmp(line, "# plan ", 7) && cur >= 0) { if (strstr(line, " exec")) ev[cur].p.exec = 1; }
        else if (!strncmp(line, "# prebar ", 9) && cur >= 0) ev[cur].p.prebar = 1;
        else if (!strncmp(line, "clear 0x", 8) && strstr(line, "rt=0")) { if (npc < 32) pend_clear[npc++] = strtoull(line + 6, NULL, 16); }
        else if (!strncmp(line, "# clear ", 8) && strstr(line, "standalone") && npc) {   // an image clear the plan does not fold: an op
            if (n < cap) { const int ix = key_ix(pend_clear[npc - 1], 1); if (ix >= 0) { memset(&ev[n], 0, sizeof ev[n]); ev[n].kind = 1; ev[n].img = ix; n++; } } }
    }
    fclose(f); return n;
}
static void replay(const char *path) {
    static REv ev[512]; g_nkeys = 0;
    load_dump(path, ev, 512);   // first walk: the written images get the model's slots
    const int n = load_dump(path, ev, 512); if (n < 0) { printf("  %s: cannot open\n", path); return; }
    int exec = 0; for (int i = 0; i < n; i++) if (!ev[i].kind && ev[i].p.exec) exec++;
    printf("  %s: %d exec passes, %d tracked images\n", path, exec, g_nkeys);
    for (int tile = 0; tile < 2; tile++) for (int nr = 0; nr < 2; nr++) {
        W w; memset(&w, 0, sizeof w); w.nr = nr; w.tile = tile; uint64_t f0 = 0, g0 = 0, r0 = 0, p0 = 0;
        for (int it = 0; it < 3; it++) {
            if (it == 2) { f0 = w.full; g0 = w.frag; r0 = w.read_bar; p0 = w.pb_bar; }
            for (int i = 0; i < n; i++) {
                if (ev[i].kind) { h_op(&w, ev[i].img); continue; }
                const RPass *p = &ev[i].p; if (!p->exec || !p->n) continue;
                int sub[NA] = {0, 0, 0};
                for (int j = 0; j < p->nrd; j++) { int own = 0; for (int k = 0; k < p->n; k++) if (p->att[k] == p->rd[j]) own = 1; if (!own) h_read(&w, p->rd[j], FRAG); }
                h_open(&w, p->n, p->att, sub, p->prebar ? p->nrd : -1, p->rd, 1);
            }
            end_pass(&w); w.full--; h_full(&w);   // the next submission begins behind a barrier (not counted)
        }
        printf("    %-8s %-6s: barriers a frame %2llu full + %2llu fragment (read %llu, before a pass %llu)%s\n", tile ? "tile" : "no tile", nr ? "narrow" : "old",
               (unsigned long long)(w.full - f0), (unsigned long long)(w.frag - g0), (unsigned long long)(w.read_bar - r0), (unsigned long long)(w.pb_bar - p0), w.unsound ? "  UNSOUND" : "");
        CHECK(!w.unsound);
    }
}

int main(int argc, char **argv) {
    test_units();
    for (int tile = 0; tile < 2; tile++) for (int raw = 0; raw < 2; raw++) {
        Res o = {0}, nw = {0}, ck = {0};
        for (uint32_t seed = 1; seed <= 4; seed++) run(seed * 7919u, tile, 200000, raw, 0, &o, &nw, &ck);
        printf("stream tile=%d raw=%d: %llu events; barriers old %llu full + %llu frag, narrow %llu + %llu (read %llu -> %llu, before a pass %llu -> %llu); pass splits old %llu, narrow %llu; "
               "passes stamped %llu, not clean %llu; unsound old %llu narrow %llu; check != old %llu, shadow != narrow %llu\n", tile, raw, (unsigned long long)o.events,
               (unsigned long long)o.full, (unsigned long long)o.frag, (unsigned long long)nw.full, (unsigned long long)nw.frag, (unsigned long long)o.read_bar, (unsigned long long)nw.read_bar,
               (unsigned long long)o.pb_bar, (unsigned long long)nw.pb_bar, (unsigned long long)o.splits, (unsigned long long)nw.splits, (unsigned long long)nw.stamps, (unsigned long long)nw.dirty,
               (unsigned long long)o.unsound, (unsigned long long)nw.unsound, (unsigned long long)ck.diff_exec, (unsigned long long)ck.diff_shadow);
        CHECK(o.unsound == 0); CHECK(nw.unsound == 0); CHECK(ck.unsound == 0); CHECK(ck.diff_exec == 0);
        CHECK(ck.diff_shadow * 1000 < ck.events);   // exact except after a pass split only one of the worlds has (the shadow keeps one pass structure)
        CHECK(nw.full + nw.frag < o.full + o.frag);
    }
    // mutations: each must be caught by the oracle on the same streams
    { Res o = {0}, nw = {0}, ck = {0}; run(7919u, 0, 200000, 1, MUT_NOCLEAN, &o, &nw, &ck); printf("mutation 'stamp without the clean test': unsound %llu\n", (unsigned long long)nw.unsound); CHECK(nw.unsound > 0); }
    { Res o = {0}, nw = {0}, ck = {0}; run(7919u, 1, 200000, 0, MUT_TILEFULL, &o, &nw, &ck); printf("mutation 'all-stage point under the tile policy': unsound %llu\n", (unsigned long long)nw.unsound); CHECK(nw.unsound > 0); }
    { Res o = {0}, nw = {0}, ck = {0}; run(7919u, 0, 200000, 0, MUT_STORAGE, &o, &nw, &ck); printf("mutation 'render-pass point covers a storage write': unsound %llu\n", (unsigned long long)nw.unsound); CHECK(nw.unsound > 0); }
    if (argc > 1) { printf("replay of IR dumps (reads as the first draw's, fragment stage; 3rd repetition of the frame):\n"); for (int i = 1; i < argc; i++) replay(argv[i]); }
    printf(g_fail ? "nr_rp_test: FAILED\n" : "nr_rp_test: all passed\n");
    return g_fail;
}
