// [DOS2] NR-NATIVE step B part 2 (docs/dos2/nr/NR-NATIVE.md section 9): host test of the SHORT record of an engine draw.
// The NVN layer's half is the engine's own file, compiled here with stubs: dm/gfx/nvn/nvn_nrn.inc (the waiting state records, the
// short decision's effect, the mask of what was left out). The masks, the rows and the publication are dm/gfx/nr/nr_nrn.h.
//
// The model. An ENGINE with a state cache (program, depth-stencil, polygon, two blend targets, three vertex-buffer slots, texture
// slots of two stages) sends a field to NVN only when its cache says NVN does not hold it (the filter of the Switch build); a UI
// draws at the end of every frame with its own state, part of which it sets every frame (the engine's fields it clobbers) and part
// only once (fields the engine never writes); after the UI the engine marks its cache dirty. The record stream of a frame is
// executed by the BACKEND after the frame was written (the publication the front end reads is one frame old, short records are in
// flight when the backend changes its tables).
//   path A   every state record and every texture bind is written and applied: the reference.
//   path B   the same engine through the short record: state records wait (nvn_nrn.inc), a draw whose card is published is written
//            short, the backend puts the left-out fields into its State from the image of the card.
// What must hold: EVERY draw - engine and UI - sees in path B exactly the state it sees in path A, in every field it uses; the image
// of a card never differs from what a long record of the card wrote; a short record always finds its image.
// Mutations (-DSHORT_MUTATE=k, the test must FAIL): 1 the backend writes the whole image (not only what was left out), 2 a waiting
// state record is not written before another record, 3 the front end writes short without asking the publication, 4 the image is
// taken before the record's texture binds landed, 5 the left-out texture slots are not named in the record, 6 a short draw does not
// drop the waiting state records (they are written after it). Also -DNRN_MUTATE=14 / 15 (nr_nrn.h).
// Build: clang -std=c11 -O2 -Wall -Wextra nr_nrn_short_test.c -o nr_nrn_short_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../nr/nr_nrn.h"
#ifndef SHORT_MUTATE
#define SHORT_MUTATE 0
#endif
static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 12) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---- the record stream of path B and the stubs nvn_nrn.inc is compiled against ----
enum { OP_X_NR_STATE = 1, OP_BIND_TEX = 2, OP_DRAW = 3, OP_FOREIGN = 4, OP_UIDRAW = 5 };
typedef struct { int recording; } NVNcommandBuffer;
static uint8_t g_stream[1 << 20]; static size_t g_len;
static void nrnp_flush(void);
static __thread struct { void *cb; uint32_t n, busy; uint32_t len[4]; uint8_t b[4][704]; } t_nrnp_decl_check;   // (the layout nvn_nrn.inc declares; unused)
#define NRNP_RECORDING(c) ((c) && (c)->recording)
#define NVN_NRN_PART 1
#include "../../../nvn/nvn_nrn.inc"
#undef NVN_NRN_PART
static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload) {   // = hle_nvn.c rec() -> cmd_alloc: what waits goes out first
    (void)c;
    if (SHORT_MUTATE != 2) NRNP_FLUSH(c);
    const uint32_t n = (uint32_t)((payload + 7) & ~(size_t)7);
    memcpy(g_stream + g_len, &op, 4); memcpy(g_stream + g_len + 4, &n, 4);
    void *p = g_stream + g_len + 8; memset(p, 0, n); g_len += 8 + n;
    return p;
}
#define NVN_NRN_PART 2
#include "../../../nvn/nvn_nrn.inc"
#undef NVN_NRN_PART

// ---- the state both ends talk about ----
enum { VB = 4, TS = 2, TSLOT = 8 };   // vb[3] and tex[1][7] are the UI's alone
typedef struct { uint64_t prog, ds, poly, blend[2], vb[VB], tex[TS][TSLOT]; } st_t;
enum { H_DS = 1, H_POLY = 16 };       // nr3_hdr have bits (NR3_H_DS, NR3_H_POLY)
// A state record with the layout nrn_emask_of_state reads: nr3_hdr, prog, ds, poly, blend[target], vb[slot] {value, 0}.
typedef struct { int nprog; unsigned have, vbm, blendm; uint64_t prog, ds, poly, blend[2], vb[VB]; } srec;
static uint32_t srec_size(const srec *r) { return 16 + 8u * (unsigned)r->nprog + ((r->have & H_DS) ? 8u : 0u) + ((r->have & H_POLY) ? 8u : 0u) + 8u * (unsigned)__builtin_popcount(r->blendm) + 16u * (unsigned)__builtin_popcount(r->vbm); }
static void srec_put(const srec *r, uint8_t *d) {
    memset(d, 0, 16); d[0] = 1; d[2] = (uint8_t)r->nprog; { uint16_t h = (uint16_t)r->have, v = (uint16_t)r->vbm; memcpy(d + 4, &h, 2); memcpy(d + 6, &v, 2); } d[8] = (uint8_t)r->blendm; d[10] = r->nprog ? 1 : 0;
    uint8_t *s = d + 16;
    if (r->nprog) { memcpy(s, &r->prog, 8); s += 8; }
    if (r->have & H_DS) { memcpy(s, &r->ds, 8); s += 8; }
    if (r->have & H_POLY) { memcpy(s, &r->poly, 8); s += 8; }
    for (int t = 0; t < 2; t++) if (r->blendm >> t & 1) { memcpy(s, &r->blend[t], 8); s += 8; }
    for (int k = 0; k < VB; k++) if (r->vbm >> k & 1) { memcpy(s, &r->vb[k], 8); memset(s + 8, 0, 8); s += 16; }
}
static void srec_apply(st_t *S, const uint8_t *d) {
    const int nprog = d[2]; uint16_t have, vbm; memcpy(&have, d + 4, 2); memcpy(&vbm, d + 6, 2); const unsigned blendm = d[8];
    const uint8_t *s = d + 16;
    if (nprog) { memcpy(&S->prog, s, 8); s += 8; }
    if (have & H_DS) { memcpy(&S->ds, s, 8); s += 8; }
    if (have & H_POLY) { memcpy(&S->poly, s, 8); s += 8; }
    for (int t = 0; t < 2; t++) if (blendm >> t & 1) { memcpy(&S->blend[t], s, 8); s += 8; }
    for (int k = 0; k < VB; k++) if (vbm >> k & 1) { memcpy(&S->vb[k], s, 8); s += 16; }
}
// The image written into State under the mask of a short record (the backend's img_walk with put = true).
static void image_put(st_t *S, const st_t *I, const nrn_emask *m, const uint32_t tm[TS]) {
    if (SHORT_MUTATE == 1) { *S = *I; return; }
    if (m->stages & 1) S->prog = I->prog;
    if (m->have & H_DS) S->ds = I->ds;
    if (m->have & H_POLY) S->poly = I->poly;
    for (int t = 0; t < 2; t++) if (m->blend >> t & 1) S->blend[t] = I->blend[t];
    for (int k = 0; k < VB; k++) if (m->vb >> k & 1) S->vb[k] = I->vb[k];
    for (int st = 0; st < TS; st++) for (int s = 0; s < TSLOT; s++) if (tm[st] >> s & 1) S->tex[st][s] = I->tex[st][s];
}
static int image_same(const st_t *S, const st_t *I, const nrn_emask *m, const uint32_t tm[TS]) {
    if ((m->stages & 1) && S->prog != I->prog) return 0;
    if ((m->have & H_DS) && S->ds != I->ds) return 0;
    if ((m->have & H_POLY) && S->poly != I->poly) return 0;
    for (int t = 0; t < 2; t++) if ((m->blend >> t & 1) && S->blend[t] != I->blend[t]) return 0;
    for (int k = 0; k < VB; k++) if ((m->vb >> k & 1) && S->vb[k] != I->vb[k]) return 0;
    for (int st = 0; st < TS; st++) for (int s = 0; s < TSLOT; s++) if ((tm[st] >> s & 1) && S->tex[st][s] != I->tex[st][s]) return 0;
    return 1;
}

// ---- the engine ----
typedef struct { uint64_t vb[3]; int streams; } mesh_t;
typedef struct { uint64_t prog, ds, poly, blend[2], tex[TS][6]; int nt[TS]; } mat_t;
enum { MESHES = 24, MATS = 40 };
static mesh_t g_mesh[MESHES]; static mat_t g_mat[MATS];
static st_t g_cache; static st_t g_dirty;   // dirty: nonzero = NVN may not hold the cache's value (after the UI)
static uint64_t used_hash(const st_t *S, const mesh_t *me, const mat_t *ma) {   // the fields an engine draw reads
    uint64_t h = nrn_mix(nrn_mix(nrn_mix(1, S->prog), S->ds), S->poly); h = nrn_mix(nrn_mix(h, S->blend[0]), S->blend[1]);
    for (int k = 0; k < me->streams; k++) h = nrn_mix(h, S->vb[k]);
    for (int st = 0; st < TS; st++) for (int s = 0; s < ma->nt[st]; s++) h = nrn_mix(h, S->tex[st][s]);
    return h;
}
static uint64_t ui_hash(const st_t *S) { return nrn_mix(nrn_mix(nrn_mix(nrn_mix(nrn_mix(nrn_mix(2, S->prog), S->vb[0]), S->vb[3]), S->tex[1][0]), S->tex[1][7]), S->blend[0]); }

enum { MAXDRAW = 1 << 16 };
static uint64_t g_want[MAXDRAW]; static uint32_t g_ndraw;   // path A: what each draw saw
static st_t A;                                               // path A's State
static NVNcommandBuffer g_cb = {1};
static nrn_pub g_pub; static int g_short_on = 1;
static unsigned long long n_short, n_long, n_state_dropped, n_tex_dropped, n_noimg, n_imgbad, n_ready_late, n_culled;

// One engine draw (cull = the packet is executed but the draw is not made: its state record waits, the next record writes it).
static void engine_draw(const mesh_t *me, const mat_t *ma, int cull) {
    // the opcode-21 packet: every field it carries goes through the filter
    srec r; memset(&r, 0, sizeof r);
#define FIELD(cachef, dirtyf, val, emit) if (g_cache.cachef != (val) || g_dirty.dirtyf) { g_cache.cachef = (val); g_dirty.dirtyf = 0; emit; }
    FIELD(prog, prog, ma->prog, (r.nprog = 1, r.prog = ma->prog))
    FIELD(ds, ds, ma->ds, (r.have |= H_DS, r.ds = ma->ds))
    FIELD(poly, poly, ma->poly, (r.have |= H_POLY, r.poly = ma->poly))
    for (int t = 0; t < 2; t++) FIELD(blend[t], blend[t], ma->blend[t], (r.blendm |= 1u << t, r.blend[t] = ma->blend[t]))
    for (int k = 0; k < me->streams; k++) FIELD(vb[k], vb[k], me->vb[k], (r.vbm |= 1u << k, r.vb[k] = me->vb[k]))
    uint8_t buf[256];
    if (r.nprog || r.have || r.blendm || r.vbm) {
        srec_put(&r, buf); srec_apply(&A, buf);                       // path A: written and applied at once
        uint8_t *w = nrnp_room(&g_cb, srec_size(&r));                  // path B: it waits for its draw
        if (w) srec_put(&r, w); else srec_put(&r, rec(&g_cb, OP_X_NR_STATE, srec_size(&r)));
    }
    // the material's textures into the cache; the flush binds what NVN may not hold
    struct { uint8_t st, slot; uint64_t v; } bind[TS * 6]; unsigned nb = 0;
    for (int st = 0; st < TS; st++) for (int s = 0; s < ma->nt[st]; s++)
        if (g_cache.tex[st][s] != ma->tex[st][s] || g_dirty.tex[st][s]) { g_cache.tex[st][s] = ma->tex[st][s]; g_dirty.tex[st][s] = 0; bind[nb].st = (uint8_t)st; bind[nb].slot = (uint8_t)s; bind[nb].v = ma->tex[st][s]; nb++; }
    if (cull) { for (unsigned i = 0; i < nb; i++) g_dirty.tex[bind[i].st][bind[i].slot] = 1; n_culled++; return; }   // (no flush ran: the slots stay to be bound)
    // the card: the whole cache
    const uint64_t card = nrn_mix_bytes(7, (const uint8_t *)&g_cache, sizeof g_cache) | 1;
    const int ready = nrn_pub_ready(&g_pub, card), shrt = g_short_on && (ready || SHORT_MUTATE == 3);
    if (SHORT_MUTATE == 6 && shrt) { t_nrn_short = 1; memset(&t_nrn_sx, 0, sizeof t_nrn_sx); t_nrn_sx.st[0] = t_nrn_sx.st[1] = 0xff; }
    else if (shrt) nvn_nrn_short(1);
    uint8_t rb[512]; uint32_t nbr = 0;
    for (unsigned i = 0; i < nb; i++) {
        A.tex[bind[i].st][bind[i].slot] = bind[i].v;
        if (NRN_SHORT_BIND(OP_BIND_TEX, bind[i].st, bind[i].slot, 1)) continue;
        rb[nbr * 16] = bind[i].st; rb[nbr * 16 + 1] = bind[i].slot; memcpy(rb + nbr * 16 + 8, &bind[i].v, 8); nbr++;
    }
    const uint32_t id = g_ndraw++; g_want[id] = used_hash(&A, me, ma);
    // the draw record: card, flags, id, the mesh / material for the check, binds, the ext of a short record
    const int sh = t_nrn_short; nrn_short_ext sx = t_nrn_sx;
    if (SHORT_MUTATE == 5) sx.tm[0] = sx.tm[1] = 0;
    uint8_t *p = (uint8_t *)rec(&g_cb, OP_DRAW, 32 + 16 * nbr + (sh ? sizeof sx : 0));
    t_nrn_short = 0;
    memcpy(p, &card, 8); { uint32_t f = (uint32_t)sh | (uint32_t)ready << 1; memcpy(p + 8, &f, 4); memcpy(p + 12, &id, 4); memcpy(p + 16, &nbr, 4); }
    { uint16_t mi = (uint16_t)(me - g_mesh), ti = (uint16_t)(ma - g_mat); memcpy(p + 20, &mi, 2); memcpy(p + 22, &ti, 2); }
    memcpy(p + 32, rb, 16 * nbr);
    if (sh) memcpy(p + 32 + 16 * nbr, &sx, sizeof sx);
}
// The UI at the end of a frame: its own records, its draw, then the engine learns that NVN's state is not its cache any more.
static void foreign(int field, uint64_t v) {
    uint8_t *p = (uint8_t *)rec(&g_cb, OP_FOREIGN, 16); memcpy(p, &field, 4); memcpy(p + 8, &v, 8);
    switch (field) { case 0: A.prog = v; break; case 1: A.vb[0] = v; break; case 2: A.tex[1][0] = v; break; case 3: A.blend[0] = v; break; case 4: A.vb[3] = v; break; default: A.tex[1][7] = v; }
}
static void ui(int frame) {
    foreign(0, 0x5500 + (unsigned)(frame & 3)); foreign(1, 0x5600); foreign(2, 0x5700 + (unsigned)(frame & 1)); foreign(3, 0x5800);
    if (frame == 0) { foreign(4, 0x5900); foreign(5, 0x5a00); }   // set once: the engine never touches them
    const uint32_t id = g_ndraw++; g_want[id] = ui_hash(&A);
    memcpy(rec(&g_cb, OP_UIDRAW, 8), &id, 4);
    g_dirty.prog = g_dirty.ds = g_dirty.poly = g_dirty.blend[0] = g_dirty.blend[1] = 1;
    for (int k = 0; k < 3; k++) g_dirty.vb[k] = 1;
    for (int st = 0; st < TS; st++) for (int s = 0; s < 6; s++) g_dirty.tex[st][s] = 1;
}

// ---- the backend of path B ----
static st_t B; static nrn_rows g_rows; static st_t *g_img; static uint8_t *g_has;
static struct { nrn_emask m; uint32_t tex[TS]; } g_w;   // what the engine's records wrote since the previous draw record
static void backend_run(void) {
    for (size_t at = 0; at < g_len;) {
        uint32_t op, n; memcpy(&op, g_stream + at, 4); memcpy(&n, g_stream + at + 4, 4); const uint8_t *p = g_stream + at + 8; at += 8 + n;
        if (op == OP_X_NR_STATE) { srec_apply(&B, p); nrn_emask_of_state(&g_w.m, p, n); continue; }
        if (op == OP_FOREIGN) { int f; uint64_t v; memcpy(&f, p, 4); memcpy(&v, p + 8, 8);
            switch (f) { case 0: B.prog = v; break; case 1: B.vb[0] = v; break; case 2: B.tex[1][0] = v; break; case 3: B.blend[0] = v; break; case 4: B.vb[3] = v; break; default: B.tex[1][7] = v; }
            continue; }
        if (op == OP_UIDRAW) { uint32_t id; memcpy(&id, p, 4); CHECK(ui_hash(&B) == g_want[id], "UI draw %u does not see the state of the reference path", id); continue; }
        uint64_t card; uint32_t f, id, nbr; uint16_t mi, ti; memcpy(&card, p, 8); memcpy(&f, p + 8, 4); memcpy(&id, p + 12, 4); memcpy(&nbr, p + 16, 4); memcpy(&mi, p + 20, 2); memcpy(&ti, p + 22, 2);
        int made = 0; nrn_row *r = nrn_rows_get(&g_rows, card, 0, &made);
        const size_t ri = r ? (size_t)(r - g_rows.e) : 0;
        if (made && r) g_has[ri] = 0;   // (NRN_MUTATE 14: a table that started again hands the slot to another card)
        if (f & 1) {   // short: exactly what was left out, from the image
            nrn_short_ext sx; const int ok = nrn_short_ext_get(p, n, 32 + 16 * nbr, &sx);
            if (!ok || !r || !g_has[ri]) { n_noimg++; CHECK(0, "draw %u: a short record without an image", id); }
            else { uint32_t tm[TS] = {0, 0}; for (int k = 0; k < 2; k++) if (sx.st[k] != 0xff && sx.st[k] < TS) tm[sx.st[k]] |= sx.tm[k];
                image_put(&B, &g_img[ri], &sx.m, tm); n_short++;
                n_state_dropped += (sx.m.have || sx.m.vb || sx.m.blend || sx.m.stages) ? 1 : 0; n_tex_dropped += (unsigned)__builtin_popcount(tm[0]) + (unsigned)__builtin_popcount(tm[1]); }
        } else if (SHORT_MUTATE == 4 && r && !g_has[ri]) { g_img[ri] = B; g_has[ri] = 1; nrn_pub_set(&g_pub, card); }
        for (uint32_t i = 0; i < nbr; i++) { const uint8_t *b = p + 32 + 16 * i; uint64_t v; memcpy(&v, b + 8, 8); B.tex[b[0]][b[1]] = v; g_w.tex[b[0]] |= 1u << b[1]; }
        if (!(f & 1)) {   // long: the State its records left is the image of the card (compared when the card has one)
            n_long++; if (f & 2) n_ready_late++;
            if (r && g_has[ri]) { if (!image_same(&B, &g_img[ri], &g_w.m, g_w.tex)) { n_imgbad++; r->unstable = 1; nrn_pub_drop(&g_pub, card); g_img[ri] = B; } }
            else if (r) { g_img[ri] = B; g_has[ri] = 1; }
            if (r && !r->unstable) nrn_pub_set(&g_pub, card);
        }
        memset(&g_w, 0, sizeof g_w);
        CHECK(used_hash(&B, &g_mesh[mi], &g_mat[ti]) == g_want[id], "engine draw %u (%s) does not see the state of the reference path", id, f & 1 ? "short" : "long");
    }
    g_len = 0;
}

static void run(int frames, int combos, uint32_t rows_cap) {
    memset(&g_cache, 0, sizeof g_cache); memset(&g_dirty, 0, sizeof g_dirty); memset(&A, 0, sizeof A); memset(&B, 0, sizeof B); memset(&g_pub, 0, sizeof g_pub); memset(&g_w, 0, sizeof g_w);
    g_ndraw = 0; g_len = 0;
    CHECK(nrn_rows_init(&g_rows, rows_cap) == 0, "rows");
    g_img = (st_t *)calloc(rows_cap, sizeof *g_img); g_has = (uint8_t *)calloc(rows_cap, 1);
    struct { int mesh, mat; } *cb = calloc((size_t)combos, sizeof *cb);
    for (int i = 0; i < combos; i++) { cb[i].mesh = (int)rn(MESHES); cb[i].mat = (int)rn(MATS); }
    for (int f = 0; f < frames; f++) {
        const int nd = 40 + (int)rn(8), base = (f / 50 * 37) % combos;   // the scene changes every 50 frames
        for (int d = 0; d < nd && g_ndraw < MAXDRAW - 2; d++) {
            const int ci = (base + (rn(60) ? d : (int)rn((unsigned)combos))) % combos;   // mostly the same order every frame, sometimes anything
            engine_draw(&g_mesh[cb[ci].mesh], &g_mat[cb[ci].mat], rn(300) == 0);
        }
        ui(f);
        backend_run();   // the frame is executed after it was written
    }
    free(cb); free(g_img); free(g_has); nrn_rows_free(&g_rows);
}
int main(void) {
    for (int i = 0; i < MESHES; i++) { g_mesh[i].streams = 1 + i % 3; for (int k = 0; k < 3; k++) g_mesh[i].vb[k] = 0x1000 + (unsigned)(i * 3 + k) % 30; }
    for (int i = 0; i < MATS; i++) { mat_t *m = &g_mat[i]; m->prog = 0x100 + (unsigned)i % 9; m->ds = 0x200 + (unsigned)i % 3; m->poly = 0x300 + (unsigned)i % 2; m->blend[0] = 0x400 + (unsigned)i % 4; m->blend[1] = 0x480 + (unsigned)i % 2;
        m->nt[0] = i % 3; m->nt[1] = 1 + i % 5; for (int st = 0; st < TS; st++) for (int s = 0; s < 6; s++) m->tex[st][s] = 0x2000 + (unsigned)(i * 7 + st * 3 + s) % 50; }
    g_nrn_defer = 1;
    run(600, 300, 4096);
    CHECK(n_short > 15000 && n_long > 300 && n_state_dropped > 5000 && n_tex_dropped > 5000 && n_culled > 50, "the model exercised the short record (short %llu, long %llu, with dropped state %llu, dropped texture binds %llu, culled %llu)",
          n_short, n_long, n_state_dropped, n_tex_dropped, n_culled);
    CHECK(n_imgbad == 0 && n_noimg == 0, "the image of a card never differs from what a long record wrote (%llu), a short record always finds it (%llu)", n_imgbad, n_noimg);
    const unsigned long long s1 = n_short;
    run(400, 2000, 256);   // more cards than rows: the table fills up, new cards stay long, nothing in flight loses its image
    CHECK(n_noimg == 0 && n_short > s1, "a full table of rows keeps its images (%llu short records without one)", n_noimg);
    { uint64_t ss[NRNPS_N]; nvn_nrn_short_stats(ss);
      CHECK(ss[NRNPS_WAIT] == ss[NRNPS_WRITTEN] + ss[NRNPS_DROPPED] && ss[NRNPS_LOST] == 0 && ss[NRNPS_DROPPED] > 5000, "every state record that waited was written or dropped (%llu = %llu + %llu)",
            (unsigned long long)ss[NRNPS_WAIT], (unsigned long long)ss[NRNPS_WRITTEN], (unsigned long long)ss[NRNPS_DROPPED]); }
    // the mask of a state record, a third stage, the ext
    { srec r; memset(&r, 0, sizeof r); r.nprog = 1; r.have = H_DS | H_POLY; r.blendm = 2; r.vbm = 5; uint8_t b[128]; srec_put(&r, b); nrn_emask m; memset(&m, 0, sizeof m);
      CHECK(nrn_emask_of_state(&m, b, srec_size(&r)) && m.have == (H_DS | H_POLY) && m.vb == 5 && m.blend == 2 && m.stages == 1, "the fields of a state record");
      CHECK(!nrn_emask_of_state(&m, b, 8), "a cut state record names nothing");
      nrn_short_ext x; memset(&x, 0, sizeof x); x.st[0] = x.st[1] = 0xff;
      CHECK(nrn_short_ext_tex(&x, 4, 3) && nrn_short_ext_tex(&x, 0, 31) && nrn_short_ext_tex(&x, 4, 0) && !nrn_short_ext_tex(&x, 2, 1) && !nrn_short_ext_tex(&x, 4, 32) && x.tm[0] == 9 && x.tm[1] == 0x80000000u,
            "two stages of texture slots; a third stage and a slot outside the image are recorded as before");
      uint8_t rb[64]; CHECK(nrn_short_ext_put(rb, sizeof rb, 16, &x) && !nrn_short_ext_put(rb, 32, 16, &x), "the ext needs its room");
      nrn_short_ext y; CHECK(nrn_short_ext_get(rb, sizeof rb, 16, &y) && !memcmp(&x, &y, sizeof x) && !nrn_short_ext_get(rb, 39, 16, &y), "the ext read back; a cut record has none"); }
    (void)t_nrnp_decl_check;
    printf("nr_nrn_short_test%s: %llu checks, %llu failed (short %llu, long %llu, dropped state records' draws %llu, dropped texture binds %llu, ready but long %llu): %s\n",
           SHORT_MUTATE || NRN_MUTATE ? " (mutation)" : "", g_checks, g_fail, n_short, n_long, n_state_dropped, n_tex_dropped, n_ready_late, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
