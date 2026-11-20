// [DOS2] NR-09R (DM_NR_DIRECT, docs/dos2/nr/NR-09R.md): host test of dm/gfx/nr/nr_direct.h - the parts of the direct engine draw that
// decide what is NOT done, each against a reference that does everything.
//   1 the tail word       NRD_X_* in the spare word of an OP_X_NR_DRAW tail; the old readers of the record are not disturbed
//   2 NRDP_TEXF           the binding mirror: a stream with the "same handle" binds left out leaves the backend's tables exactly as the
//                         full stream does at every draw - for any submission order, replays, nested handles, another thread writing
//                         into the recording, binds outside brackets; the =2 flags pass the backend's check; mutations are caught
//   3 NRDP_TAP            the tap memo: the frame IR (nb_plan.c, the real one) built with skipped walks equals the IR built with every
//                         walk; the =2 check never fires; mutations of the signature / the learn rule are caught
//   4 NRDP_IDENT          the lean identity returns the key id / why of the shared table for every draw
//   5 cost model          x86 ns of the old and the new per-draw work of parts 3 and 4 (information only)
// Build: clang -std=c11 -O2 -Wall -Wextra nr_direct_test.c ../../../nb/nb_plan.c -o nr_direct_test
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../../nvn/nvn_nb_draw.h"
#include "../../../nb/nb_ir.h"
#include "../../../nr/nr_direct.h"

static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_rng = 0x1234567887654321ull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 16); }
static uint32_t rn(uint32_t n) { return rnd() % n; }

// ================= 1. the tail word =================
static void test_tail(void) {
    for (int it = 0; it < 4000; it++) {
        nbd_shadow s; memset(&s, 0, sizeof s);
        const unsigned nb = rn(30);
        for (unsigned i = 0; i < nb; i++) nbd_add(&s, rn(4), (int)rn(6), (int)rn(16), rnd(), rnd());
        s.h.draw = NBD_DRAW_IDX; s.h.count = (int32_t)rn(1000); s.h.flags |= NBD_F_NR;
        uint8_t rec[sizeof(nbd_shadow) + 64]; memset(rec, 0xcc, sizeof rec);
        const uint32_t n = nbd_pack(&s, rec, sizeof rec), size = n + (uint32_t)sizeof(nrd_tail);
        const uint32_t key = rnd() | 1, ts = rnd(), why = rn(256), site = rn(25);
        CHECK(nrd_tail_put(rec, size, key, ts, why, site), "tail put");
        nbd_hdr h; const uint8_t *b = NULL; nrd_tail t;
        CHECK(!nbd_view(rec, size, &h, &b) && nrd_tail_get(rec, size, &h, &t) && t._pad == 0, "a tail without the word reads 0");
        const uint32_t filt = rn(400), would = rnd() & 0xffffff;
        const uint32_t x1 = nrd_x_filt(NRD_X_DIRECT, filt), x2 = nrd_x_chk(rn(2) ? NRD_X_DIRECT : 0, would);
        CHECK(nrd_x_filtered(x1) == (filt > 255 ? 255 : filt) && (x1 & NRD_X_DIRECT) && !nrd_x_would(x1), "filt word %#x", x1);
        CHECK(nrd_x_would(x2) == would && !nrd_x_filtered(x2), "check word %#x", x2);
        const uint32_t x = rn(2) ? x1 : x2;
        uint8_t before[sizeof rec]; memcpy(before, rec, sizeof rec);
        nrd_tail_ext_put(rec, size, x);
        CHECK(!memcmp(rec, before, nrd_tail_off(&h) + offsetof(nrd_tail, _pad)) && !memcmp(rec + size, before + size, sizeof rec - size), "the word alone is written");
        nbd_shadow u; nrd_tail t2;
        CHECK(!nbd_unpack(rec, size, &u) && !memcmp(u.bind, s.bind, sizeof(nbd_bind) * nb) && u.h.count == s.h.count, "the old unpack takes the record");
        CHECK(nrd_tail_get(rec, size, &h, &t2) && t2._pad == x && t2.key == key && t2.texset == ts && t2.why == why && t2.site == site && t2.ver == NRD_VER, "tail fields kept");
        nrd_tail_ext_put(rec, n, 0xdeadbeef);   // a record without room for a tail: nothing written
        CHECK(nrd_tail_get(rec, size, &h, &t2) && t2._pad == x, "short record untouched");
    }
}

// ================= 2. NRDP_TEXF: the binding mirror =================
enum { M_ST = 2, M_SL = 6, M_H = 3, REC_MAX = 400, NREC = 24 };
typedef struct { uint8_t op, kind, stage, slot, would; uint64_t h; int call; } mrec;   // op 0 bind, 1 draw, 2 call
typedef struct { mrec r[2][REC_MAX]; int n[2]; } mrecording;                          // [0] the full stream, [1] the filtered one
typedef struct { uint64_t t[NRDM_KINDS][M_ST][M_SL]; } betab;
static mrecording g_recs[NREC];
static uint64_t g_snap[2][200000]; static int g_nsnap[2];
static unsigned long long g_would_bad, g_would_n, g_filtered, g_binds;
// The backend: the records of recording k of stream s in order, into tables t. =2: a flagged bind must find its handle in the table.
static void be_exec(betab *t, int k, int s, int depth) {
    const mrecording *R = &g_recs[k];
    for (int i = 0; i < R->n[s]; i++) {
        const mrec *r = &R->r[s][i];
        if (r->op == 0) {
            if (s == 0 && r->would) { g_would_n++; if (t->t[r->kind][r->stage][r->slot] != r->h) g_would_bad++; }
            t->t[r->kind][r->stage][r->slot] = r->h;
        } else if (r->op == 1) {
            uint64_t x = 0; for (unsigned q = 0; q < sizeof t->t / 8; q++) x = nrd_mix(x ^ ((const uint64_t *)t->t)[q]);
            if (g_nsnap[s] < 200000) g_snap[s][g_nsnap[s]++] = x;
        } else if (depth < 2) be_exec(t, r->call, s, depth + 1);
    }
}
// One session: recordings written by the "engine thread" (and now and then another thread) into two command buffers, then executed in
// a random order with replays. mut: 1 = no reset at BeginRecording, 2 = nested handles do not forget, 0 = the real thing.
static int mirror_session(int mut) {
    static nrd_mirror m; memset(&m, 0, sizeof m);
    static const int cbs[2] = {11, 22}; const int me = 1, other = 2;
    int cur[2] = {-1, -1}, nrec = 0;
    memset(g_recs, 0, sizeof g_recs);
    int c = 0;
    for (int step = 0; step < 900 && nrec <= NREC; step++) {
        if (rn(30) == 0) c ^= 1;   // the thread mostly stays with one command buffer (a frame), now and then it writes into the other
        const int ev = (int)rn(100);
        if (cur[c] < 0) {   // BeginRecording
            if (nrec == NREC) break;
            cur[c] = nrec++;
            if (mut != 1) nrdm_begin(&m, &cbs[c]);
            continue;
        }
        mrecording *R = &g_recs[cur[c]];
        if (R->n[0] >= REC_MAX - 8 || ev < 3) { cur[c] = -1; continue; }   // EndRecording
        if (ev < 70) {   // a bind call
            const unsigned kind = rn(10) < 8 ? NRDM_TEX : rn(2) ? NRDM_SAMP : NRDM_IMG;
            const int stage = (int)rn(M_ST), n = rn(10) < 8 ? 1 : 2, first = (int)rn(M_SL - 1), by_other = rn(60) == 0, in = !by_other && rn(10) < 7;
            uint64_t h[2] = {1 + rn(M_H), 1 + rn(M_H)};
            const int same = nrdm_bind(&m, &cbs[c], by_other ? (const void *)&other : (const void *)&me, in, kind, stage, first, n, h);
            const int filt = nrdm_filter(same, in, kind, n);
            g_binds += (unsigned)n;
            for (int i = 0; i < n; i++) {
                mrec r = {0, (uint8_t)kind, (uint8_t)stage, (uint8_t)(first + i), (uint8_t)filt, h[i], 0};
                R->r[0][R->n[0]++] = r;
                if (!filt) R->r[1][R->n[1]++] = r; else g_filtered++;
            }
        } else if (ev < 95) { mrec r = {1, 0, 0, 0, 0, 0, 0}; R->r[0][R->n[0]++] = r; R->r[1][R->n[1]++] = r; }
        else if (cur[c] > 0) {   // CallCommands / CopyCommands of an earlier, finished recording
            int k = (int)rn((uint32_t)cur[c]); if (k == cur[0] || k == cur[1]) continue;
            mrec r = {2, 0, 0, 0, 0, 0, k}; R->r[0][R->n[0]++] = r; R->r[1][R->n[1]++] = r;
            if (mut != 2) nrdm_nested(&m, &cbs[c]);
        }
    }
    // the backend: any order, any replay, from any table state - the same for both streams
    betab init; for (unsigned q = 0; q < sizeof init.t / 8; q++) ((uint64_t *)init.t)[q] = 100 + rn(4);
    int order[3 * NREC], no = 0;
    for (int i = 0; i < 3 * NREC; i++) order[no++] = (int)rn((uint32_t)(nrec ? nrec : 1));
    for (int s = 0; s < 2; s++) { betab t = init; g_nsnap[s] = 0; for (int i = 0; i < no; i++) be_exec(&t, order[i], s, 0); }
    int bad = g_nsnap[0] != g_nsnap[1];
    for (int i = 0; i < g_nsnap[0] && !bad; i++) bad = g_snap[0][i] != g_snap[1][i];
    return bad;
}
static void test_mirror(void) {
    int bad = 0; g_would_bad = g_would_n = g_filtered = g_binds = 0;
    for (int it = 0; it < 1500; it++) bad += mirror_session(0);
    CHECK(!bad, "filtered stream: the backend's tables differ at a draw in %d of 1500 sessions", bad);
    CHECK(g_would_n > 20000 && !g_would_bad, "=2 flags: %llu of %llu flagged binds did not find their handle in the table", g_would_bad, g_would_n);
    CHECK(g_filtered * 40 > g_binds, "the sessions filter enough to mean something (%llu of %llu binds)", g_filtered, g_binds);
    printf("  mirror: 1500 sessions, %llu binds, %llu not sent (%.1f%%), %llu flagged binds checked by the backend model\n", g_binds, g_filtered, 100. * (double)g_filtered / (double)g_binds, g_would_n);
    int m1 = 0, m2 = 0; unsigned long long wb = 0;
    g_would_bad = 0; for (int it = 0; it < 300; it++) m1 += mirror_session(1); wb = g_would_bad;
    CHECK(m1 > 0 && wb > 0, "mutation 'no reset at BeginRecording' must show (tables %d sessions, =2 flags %llu)", m1, wb);
    g_would_bad = 0; for (int it = 0; it < 300; it++) m2 += mirror_session(2);
    CHECK(m2 > 0 && g_would_bad > 0, "mutation 'a nested handle does not forget' must show (tables %d sessions, =2 flags %llu)", m2, g_would_bad);
    printf("  mirror mutations caught: no reset at BeginRecording in %d of 300 sessions, nested handle in %d of 300\n", m1, m2);
    // the rule itself
    static nrd_mirror m; memset(&m, 0, sizeof m); int a = 1, b = 2, t1 = 3, t2 = 4; uint64_t h = 77, g = 78;
    CHECK(!nrdm_bind(&m, &a, &t1, 0, NRDM_TEX, 0, 3, 1, &h), "nothing known before the first bracket");
    CHECK(m.cb == NULL, "a bind outside brackets adopts nothing");
    CHECK(!nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 3, 1, &h) && nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 3, 1, &h) == 1, "second bind of the handle is the same");
    CHECK(nrdm_bind(&m, &a, &t1, 0, NRDM_TEX, 0, 3, 1, &h) == 1 && !nrdm_filter(1, 0, NRDM_TEX, 1), "outside the bracket: known, never filtered");
    CHECK(!nrdm_bind(&m, &a, &t1, 1, NRDM_SAMP, 0, 3, 1, &h) && !nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 1, 3, 1, &h), "kinds and stages are apart");
    CHECK(!nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 3, 1, &g) && !nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 3, 1, &h), "a different handle in between");
    CHECK(!nrdm_bind(&m, &b, &t1, 0, NRDM_TEX, 0, 3, 1, &g) && nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 3, 1, &h) == 1, "another command buffer does not disturb");
    CHECK(!nrdm_bind(&m, &a, &t2, 0, NRDM_TEX, 0, 5, 1, &g) && m.cb == NULL, "another thread in the tracked recording: forgotten");
    CHECK(!nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 3, 1, &h), "nothing is the same after that");
    nrdm_begin(&m, &a); CHECK(!nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 3, 1, &h), "nothing is the same in a new recording");
    CHECK(!nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 9, 3, 1, &h) && !nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 200, 1, &h) && !nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 200, 1, &h), "outside the table: never the same");
    uint64_t two[2] = {5, 5};
    CHECK(!nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 10, 2, two) && !nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 10, 2, two) && nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 11, 1, two) == 1, "a call of several handles is noted, not filtered");
    m.epoch = 0xffffffffu; nrdm_reset(&m);
    CHECK(m.epoch == 1 && !nrdm_bind(&m, &a, &t1, 1, NRDM_TEX, 0, 11, 1, two), "the epoch wraps to a clean table");
}

// ================= 3. NRDP_TAP: the tap memo against the full walk (the walk of nvn_nb_tap.inc nbt_draw, on the real IR) =================
enum { T_ST = 5, T_SL = 64, T_PROGS = 10, T_IDS = 40 };
// Program serials 1..g_nser (never reused; what was published for a serial stays). T_PROGS of them are in use at a time (g_act); now and
// then one is replaced by a new program (a new serial, nothing published yet).
enum { T_SER = 4096 };
static struct { uint64_t tex[T_ST], img[T_ST]; int pub; } g_pm[T_SER];   // slot masks of the serial (pub: published)
static struct { uint64_t fs; int pub; } g_pp[T_SER];                     // its fs key
static uint32_t g_act[T_PROGS], g_nser;
static int pm_of(uint64_t ser) { return (int)ser; }
static void new_program(int slot) {
    if (g_nser + 1 >= T_SER) return;
    const uint32_t p = ++g_nser; g_act[slot] = p;
    for (int st = 0; st < 2; st++) { g_pm[p].tex[st] = rnd() & 0xff; g_pm[p].img[st] = rn(8) ? 0 : 1; }
    if (rn(12) == 0) g_pm[p].tex[1] = ~(uint64_t)0;   // a shader that addresses textures not by slot
    g_pp[p].fs = 0x1000 + (uint64_t)p;
}
static uint64_t g_reg[T_IDS]; static uint32_t g_writes, g_reggen;              // pool id -> image; publish count; registration generation
typedef struct {
    nb_frame f;
    uint32_t tex[T_ST][T_SL], img[T_ST][T_SL]; uint64_t tdirty[T_ST], idirty[T_ST];
    uint64_t prog[T_ST], ptex[T_ST], pimg[T_ST]; uint8_t pknown[T_ST];
    int rd_node; uint32_t gen, rd_gen;
    int mode, fresh, mut;            // 0 the old walk, 1 skip, 2 walk + check; fresh: masks looked up again after a publish; mut: see test_tap
    uint64_t tabsig; nrd_tapmemo memo; uint32_t mw;
    unsigned long long draws, hits, learned, bad;
} world;
static world *W[4];
static uint64_t w_mask(world *w, int st, int k) {
    if (!w->pknown[st]) {
        const int p = pm_of(w->prog[st]);
        if (!p || !g_pm[p].pub) return ~(uint64_t)0;
        w->pknown[st] = 1; w->ptex[st] = g_pm[p].tex[st]; w->pimg[st] = g_pm[p].img[st];
    }
    return k ? w->pimg[st] : w->ptex[st];
}
static int w_mask_note(world *w) {
    for (int st = 0; st < T_ST; st++) for (int k = 0; k < 2; k++) {
        if (w_mask(w, st, k) != ~(uint64_t)0) continue;
        const uint32_t *tab = k ? w->img[st] : w->tex[st];
        for (int s = 0; s < T_SL; s++) if (tab[s]) return 0;
    }
    return 1;
}
static void w_reads(world *w, int all) {
    for (int st = 0; st < T_ST; st++) for (int k = 0; k < 2; k++) {
        const uint32_t *tab = k ? w->img[st] : w->tex[st]; uint64_t *dirty = k ? &w->idirty[st] : &w->tdirty[st];
        const uint64_t mask = w_mask(w, st, k);
        const uint64_t m0 = all ? ~(uint64_t)0 : *dirty; *dirty = m0 & ~mask;   // nb_sm_take
        uint64_t m = m0 & mask;
        while (m) { const int s = __builtin_ctzll(m); m &= m - 1; if (!tab[s]) continue; const uint64_t im = g_reg[tab[s] - 1]; if (im) nb_read(&w->f, -1, im); }
    }
}
static int w_prog_draw(world *w) {
    uint64_t im[NB_MAX_SREAD + 1]; int n = 0, exact = 1;
    const int fp = pm_of(w->prog[1]), have = fp && g_pp[fp].pub; const uint64_t fs = have ? g_pp[fp].fs : 0;
    for (int st = 0; st < T_ST; st++) {
        uint64_t mt, mi;
        if (!w->pknown[st]) { const int p = pm_of(w->prog[st]); if (!w->prog[st]) { mt = mi = 0; } else if (!p || !g_pm[p].pub) { exact = 0; continue; } else { w->pknown[st] = 1; w->ptex[st] = g_pm[p].tex[st]; w->pimg[st] = g_pm[p].img[st]; mt = w->ptex[st]; mi = w->pimg[st]; } }
        else { mt = w->ptex[st]; mi = w->pimg[st]; }
        if (mt == ~(uint64_t)0 || mi == ~(uint64_t)0) { exact = 0; continue; }
        for (int k = 0; k < 2; k++) {
            uint64_t m = k ? mi : mt; const uint32_t *tab = k ? w->img[st] : w->tex[st];
            while (m) { const int sl = __builtin_ctzll(m); m &= m - 1; if (!tab[sl]) continue; const uint64_t x = g_reg[tab[sl] - 1]; if (!x) { exact = 0; continue; } if (n < NB_MAX_SREAD) im[n++] = x; else exact = 0; }
        }
    }
    nb_draw_prog(&w->f, fs, exact, im, n);
    return exact && (have || !w->prog[1]);
}
static void w_draw(world *w) {
    nb_frame *f = &w->f;
    nb_draw(f, 0);
    if (!f->open) return;
    const int node = f->nnode - 1, all = node != w->rd_node || w->rd_gen != w->gen;
    w->draws++;
    uint64_t sig = 0; int hit = 0; uint8_t fl = 0; uint32_t mw = g_writes;
    int nref = 0, pnref = 0, nsr = 0; uint8_t fsx = 0; uint64_t fs = 0;
    if (w->fresh && mw != w->mw) { w->mw = mw; for (int st = 0; st < T_ST; st++) if (w->prog[st]) w->pknown[st] = 0; }
    if (w->mode) {
        uint64_t pr[5]; memcpy(pr, w->prog, sizeof pr);
        if (w->mut == 1) memset(pr, 0, sizeof pr);                                   // mutation: the programs are not in the signature
        sig = nrd_draw_sig(w->tabsig, pr, w->mut == 3 ? 0 : g_reggen, mw);           // mutation 3: nor the registrations
        hit = !all && nrd_tap_hit(&w->memo, w->mut == 4 ? 0 : node, w->gen, sig, &fl);   // mutation 4: one memo for every pass
        if (hit) {
            w->hits++;
            if (w->mode == 1) return;
            const nb_node *p = &f->node[node]; nref = f->nref; pnref = p->nref; nsr = p->nsr; fsx = p->fsx; fs = p->fs;
        }
    }
    const int known = w_mask_note(w);
    w_reads(w, all);
    const int exact = w_prog_draw(w);
    w->rd_node = node; w->rd_gen = w->gen;
    if (!w->mode) return;
    if (hit) { const nb_node *p = &f->node[node]; if (f->nref != nref || p->nref != pnref || p->nsr != nsr || p->fsx != fsx || p->fs != fs) w->bad++; return; }
    if (((!known || !exact) && w->mut != 2) || g_writes != mw) return;               // mutation 2: remembered although a program is unknown
    nrd_tap_learn(&w->memo, w->mut == 4 ? 0 : node, w->gen, sig, (uint8_t)known); w->learned++;
}
static void w_bind(world *w, int k, int st, int slot, uint32_t id) {
    uint32_t *tab = k ? w->img[st] : w->tex[st]; const uint32_t v = id + 1;
    if (tab[slot] == v) return;
    w->tabsig ^= nrd_slot_sig((unsigned)k, (unsigned)st, (unsigned)slot, tab[slot]) ^ nrd_slot_sig((unsigned)k, (unsigned)st, (unsigned)slot, v);
    tab[slot] = v; *(k ? &w->idirty[st] : &w->tdirty[st]) |= 1ull << slot;
}
static void w_program(world *w, uint64_t ser, int stages) {
    for (int st = 0; st < T_ST; st++) if (stages >> st & 1) { w->prog[st] = ser; w->pknown[st] = !ser; w->ptex[st] = w->pimg[st] = 0; }
}
static void w_reset(world *w) { nb_frame_reset(&w->f, 1); w->gen++; }
// f of b against a from node `from` on: 0 equal; 1 = b has more reads (superset, same draws / sampled / fs); 2 = different.
static int ir_cmp(const nb_frame *a, const nb_frame *b, int from) {
    if (a->nnode != b->nnode || a->open != b->open) return 2;
    int more = 0;
    for (int i = from < 0 ? 0 : from; i < a->nnode; i++) {
        const nb_node *p = &a->node[i], *q = &b->node[i];
        if (p->kind != q->kind || p->draws != q->draws || p->nsr != q->nsr || p->fsx != q->fsx || p->fs != q->fs || memcmp(p->sread, q->sread, sizeof(uint64_t) * p->nsr)) return 2;
        if (q->nref < p->nref) return 2;
        for (int j = 0; j < p->nref; j++) { int k = 0; while (k < q->nref && b->ref[q->ref_first + k].image != a->ref[p->ref_first + j].image) k++; if (k == q->nref) return 2; }
        if (q->nref > p->nref) more = 1;
        else if (!more && memcmp(&a->ref[p->ref_first], &b->ref[q->ref_first], sizeof(nb_ref) * (size_t)p->nref)) return 2;   // same order too
    }
    return more;
}
// One run: the same calls into every world. rereg: 0 = a pool id is registered only while no slot holds it (then the IRs must be
// equal), 1 = at any time (a walk that was not skipped may then record the slot's new image: a superset). republish: masks of a
// published program may be published again with other masks.
static void tap_run(int nworld, int steps, int rereg, int republish, int *diff, int *superset) {
    for (int i = 0; i < nworld; i++) { world *w = W[i]; const int mode = w->mode, fresh = w->fresh, mut = w->mut; memset(w, 0, sizeof *w); w->mode = mode; w->fresh = fresh; w->mut = mut; w->rd_node = -1; w->memo.node = -1; nb_frame_reset(&w->f, 1); }
    memset(g_pm, 0, sizeof g_pm); memset(g_pp, 0, sizeof g_pp); memset(g_reg, 0, sizeof g_reg); g_writes = 0; g_reggen = 0;
    g_nser = 0;
    for (int i = 0; i < T_PROGS; i++) { new_program(i); const uint32_t p = g_act[i]; g_pm[p].pub = rn(4) != 0; g_pp[p].pub = g_pm[p].pub && rn(3) != 0; }
    for (int i = 0; i < T_IDS; i++) g_reg[i] = rn(6) ? 0x1000000ull + (uint64_t)(i * 16) : 0;
    int rt = 0;
    for (int step = 0; step < steps; step++) {
        const int ev = (int)rn(1000);
        if (ev < 520) for (int i = 0; i < nworld; i++) w_draw(W[i]);
        else if (ev < 800) { const int k = rn(12) == 0, st = (int)rn(2), slot = (int)rn(8); const uint32_t id = rn(T_IDS); for (int i = 0; i < nworld; i++) w_bind(W[i], k, st, slot, id); }
        else if (ev < 930) { const uint64_t ser = rn(20) ? g_act[rn(T_PROGS)] : 0; const int stages = rn(3) ? 0x1f : 1 << rn(2); for (int i = 0; i < nworld; i++) w_program(W[i], ser, stages); }
        else if (ev < 960) { nb_att a[2] = {{0x9000000ull + (uint64_t)(++rt % 7) * 256, 0}, {0x9800000ull, NB_SLOT_DS}}; for (int i = 0; i < nworld; i++) nb_pass_begin(&W[i]->f, a, 2); }
        else if (ev < 972) { uint64_t s = 0x9000000ull, d = 0x9100000ull; for (int i = 0; i < nworld; i++) nb_op(&W[i]->f, NB_OP_COPY, &s, 1, &d, 1); }
        else if (ev < 984) { const int slot = (int)rn(T_PROGS); const uint32_t p = g_act[slot];
            if (!g_pm[p].pub) { g_pm[p].pub = 1; g_writes++; }
            else if (!g_pp[p].pub) g_pp[p].pub = 1;
            else if (republish && rn(2)) { g_pm[p].tex[rn(2)] ^= 1ull << rn(8); g_writes++; }
            else new_program(slot); }
        else if (ev < 997) { const uint32_t id = rn(T_IDS); int bound = 0;
            for (int st = 0; st < T_ST && !bound; st++) for (int s = 0; s < T_SL; s++) if (W[0]->tex[st][s] == id + 1 || W[0]->img[st][s] == id + 1) { bound = 1; break; }
            if (rereg || !bound) { g_reg[id] = 0x2000000ull + (uint64_t)rnd() % 64 * 16; g_reggen++; } }
        else { for (int i = 1; i < nworld; i++) if (ir_cmp(&W[0]->f, &W[i]->f, 0) == 2) diff[i]++;   // the whole frame before it goes
               for (int i = 0; i < nworld; i++) w_reset(W[i]); }
        // every step: the nodes that can still change (the open pass and the one before it); every node at the resets above
        for (int i = 1; i < nworld; i++) { const int c = ir_cmp(&W[0]->f, &W[i]->f, W[0]->f.nnode - 2); if (c == 2) diff[i]++; else if (c == 1) superset[i]++; }
        if (W[0]->f.nnode > NB_MAX_NODES - 8 || W[0]->f.nref > NB_MAX_REFS - 64) {
            for (int i = 1; i < nworld; i++) if (ir_cmp(&W[0]->f, &W[i]->f, 0) == 2) diff[i]++;
            for (int i = 0; i < nworld; i++) w_reset(W[i]);
        }
    }
    for (int i = 1; i < nworld; i++) if (ir_cmp(&W[0]->f, &W[i]->f, 0) == 2) diff[i]++;
}
static void test_tap(void) {
    for (int i = 0; i < 4; i++) { W[i] = (world *)calloc(1, sizeof(world)); if (!W[i]) { printf("no memory\n"); exit(2); } }
    int diff[4], sup[4];
    // (a) no registration under a bound id, no republish: old walk = fresh walk = skipping = checking, node for node, read for read
    W[0]->mode = 0; W[0]->fresh = 0; W[1]->mode = 0; W[1]->fresh = 1; W[2]->mode = 1; W[2]->fresh = 1; W[3]->mode = 2; W[3]->fresh = 1;
    for (int i = 0; i < 4; i++) W[i]->mut = 0;
    memset(diff, 0, sizeof diff); memset(sup, 0, sizeof sup);
    unsigned long long draws = 0, hits = 0, bad = 0, learned = 0;
    for (int run = 0; run < 60; run++) { tap_run(4, 20000, 0, 0, diff, sup); draws += W[2]->draws; hits += W[2]->hits; learned += W[2]->learned; bad += W[3]->bad; }
    CHECK(!diff[1] && !sup[1], "masks looked up again after a publish change nothing here (%d / %d steps differ)", diff[1], sup[1]);
    CHECK(!diff[2] && !sup[2], "skipped walks: the IR differs from the full walk's at %d steps (superset at %d)", diff[2], sup[2]);
    CHECK(!diff[3] && !sup[3] && !bad, "=2: the IR differs at %d steps, the check fired %llu times", diff[3], bad);
    CHECK(hits * 12 > draws, "the runs skip enough to mean something (%llu of %llu draws)", hits, draws);
    printf("  tap: 1.2 M steps, %llu draws, %llu walks skipped (%.1f%%), %llu remembered; IR equal to the full walk at every step\n", draws, hits, 100. * (double)hits / (double)draws, learned);
    // (b) registrations at any time + republished masks: against the walk with fresh masks the skipping world never has fewer reads
    W[0]->mode = 0; W[0]->fresh = 1; W[1]->mode = 1; W[1]->fresh = 1; W[2]->mode = 2; W[2]->fresh = 1;
    memset(diff, 0, sizeof diff); memset(sup, 0, sizeof sup); bad = 0;
    for (int run = 0; run < 60; run++) { tap_run(3, 20000, 1, 1, diff, sup); bad += W[2]->bad; }
    CHECK(!diff[1], "registrations / republish: the skipping world lost a read or differs at %d steps", diff[1]);
    CHECK(!diff[2] && !sup[2] && !bad, "registrations / republish, =2: differs at %d steps, check fired %llu", diff[2], bad);
    printf("  tap with registrations under bound ids and republished masks: never fewer reads (more at %d of 1.2 M steps: the slot's new image), =2 clean\n", sup[1]);
    // mutations (world 1 skips with a broken rule, world 2 checks with the same rule: the =2 check must see it too)
    static const char *const mname[5] = {"", "programs not in the signature", "remembered although a program's masks / fs key are unpublished", "registrations not in the signature", "one memo for every pass"};
    for (int mut = 1; mut <= 4; mut++) {
        W[0]->mode = 0; W[0]->fresh = 1; W[0]->mut = 0; W[1]->mode = 1; W[1]->fresh = 1; W[1]->mut = mut; W[2]->mode = 2; W[2]->fresh = 1; W[2]->mut = mut;
        memset(diff, 0, sizeof diff); memset(sup, 0, sizeof sup); bad = 0;
        for (int run = 0; run < 20; run++) { tap_run(3, 20000, 1, 0, diff, sup); bad += W[2]->bad; }
        CHECK(diff[1] > 0, "mutation '%s' must lose a read (differs at %d steps)", mname[mut], diff[1]);
        CHECK(bad > 0, "mutation '%s' must fire the =2 check (%llu)", mname[mut], bad);
        printf("  tap mutation '%s': IR differs at %d steps, =2 check fired %llu times\n", mname[mut], diff[1], bad);
    }
    // signature basics
    uint64_t pr[5] = {1, 2, 0, 0, 0}, pr2[5] = {2, 1, 0, 0, 0};
    CHECK(nrd_draw_sig(5, pr, 0, 0) != nrd_draw_sig(5, pr2, 0, 0) && nrd_draw_sig(5, pr, 1, 0) != nrd_draw_sig(5, pr, 0, 0) && nrd_draw_sig(5, pr, 0, 1) != nrd_draw_sig(5, pr, 0, 0)
          && nrd_draw_sig(5, pr, 0, 1) != nrd_draw_sig(5, pr, 1, 0) && nrd_draw_sig(6, pr, 0, 0) != nrd_draw_sig(5, pr, 0, 0), "signature parts");
    CHECK(nrd_slot_sig(0, 1, 3, 0) == 0 && nrd_slot_sig(0, 1, 3, 9) != nrd_slot_sig(1, 1, 3, 9) && nrd_slot_sig(0, 1, 3, 9) != nrd_slot_sig(0, 0, 3, 9) && nrd_slot_sig(0, 1, 3, 9) != nrd_slot_sig(0, 1, 4, 9), "slot signature");
    uint64_t x = 0;   // the XOR is the table's content, whatever the order of the binds
    x ^= nrd_slot_sig(0, 0, 1, 0) ^ nrd_slot_sig(0, 0, 1, 7); x ^= nrd_slot_sig(0, 1, 2, 0) ^ nrd_slot_sig(0, 1, 2, 9); x ^= nrd_slot_sig(0, 0, 1, 7) ^ nrd_slot_sig(0, 0, 1, 8); x ^= nrd_slot_sig(0, 0, 1, 8) ^ nrd_slot_sig(0, 0, 1, 7);
    CHECK(x == (nrd_slot_sig(0, 0, 1, 7) ^ nrd_slot_sig(0, 1, 2, 9)), "a bind away and back leaves the signature of the same table");
    nrd_tapmemo m; memset(&m, 0, sizeof m); m.node = -1; uint8_t fl = 9;
    CHECK(!nrd_tap_hit(&m, 3, 1, 0x1234, &fl), "empty memo");
    nrd_tap_learn(&m, 3, 1, 0x1234, 1);
    CHECK(nrd_tap_hit(&m, 3, 1, 0x1234, &fl) && fl == 1 && !nrd_tap_hit(&m, 3, 1, 0x1235, &fl), "learned signature");
    CHECK(!nrd_tap_hit(&m, 4, 1, 0x1234, &fl) && !nrd_tap_hit(&m, 3, 1, 0x1234, &fl), "another pass empties the memo");
    nrd_tap_learn(&m, 3, 1, 0x1234, 1); CHECK(!nrd_tap_hit(&m, 3, 2, 0x1234, &fl), "another IR generation empties it");
}

// ================= 4. NRDP_IDENT =================
enum { I_KEYS = 4096, I_HASH = 16384 };
static struct { nrd_key k; uint32_t id, why; } g_shared[I_KEYS]; static uint32_t g_nshared, g_sidx[I_HASH]; static unsigned long long g_shared_lookups, g_rt_lookups;
static uint16_t ref_rtfmt(const uint64_t rt[9]) { uint64_t x = 0; for (int i = 0; i < 9; i++) x = nrd_mix(x ^ (rt[i] & 0xff)); return (uint16_t)(1 + x % 9); }   // formats: a function of the handles
static void ref_key(const nrd_key *k, uint32_t *id, uint32_t *why) {   // the shared table (nr_key_note): dense ids, why fixed at first sight
    uint64_t x = 0; for (unsigned q = 0; q < sizeof *k / 8; q++) { uint64_t v; memcpy(&v, (const uint8_t *)k + 8 * q, 8); x = nrd_mix(x ^ v); }
    uint32_t b = (uint32_t)x & (I_HASH - 1);
    for (; g_sidx[b]; b = (b + 1) & (I_HASH - 1)) { const uint32_t i = g_sidx[b] - 1; if (!memcmp(&g_shared[i].k, k, sizeof *k)) { *id = g_shared[i].id; *why = g_shared[i].why; return; } }
    if (g_nshared == I_KEYS) { *id = 0; *why = 128; return; }
    g_sidx[b] = g_nshared + 1;
    g_shared[g_nshared].k = *k; g_shared[g_nshared].id = g_nshared + 1; g_shared[g_nshared].why = (uint32_t)(nrd_mix(k->h[0]) & 0x3f); *id = g_nshared + 1; *why = g_shared[g_nshared].why; g_nshared++;
}
static void lean_ident(nrd_idc *c, const nrd_raw *w, uint32_t *id, uint32_t *why) {   // = hle_dos2_nr_direct.inc nrd_ident_core
    int rt_same = 0;
    if (nrd_ident_same(c, w, id, why, &rt_same)) return;
    uint16_t rtf = c->last_rtf;
    if (!rt_same || !rtf) { rtf = ref_rtfmt(w->rt); g_rt_lookups++; c->rt++; }
    nrd_key k;
    if (nrd_ident_find(c, w, rtf, &k, id, why)) return;
    ref_key(&k, id, why); g_shared_lookups++;
    nrd_ident_put(c, w, rtf, &k, *id, *why);
}
static void test_ident(void) {
    static nrd_idc c; memset(&c, 0, sizeof c); g_nshared = 0; memset(g_sidx, 0, sizeof g_sidx); g_shared_lookups = g_rt_lookups = 0;
    nrd_raw pool[600]; memset(pool, 0, sizeof pool);
    for (int i = 0; i < 600; i++) {
        nrd_raw *w = &pool[i];
        w->shader = 0x100000000ull + rn(150); w->blend = rn(10); w->depth = rn(18); w->raster = rn(10); w->vformat = rn(6);
        const int rts = (int)rn(12); for (int j = 0; j < 9; j++) w->rt[j] = j < 2 || j == 8 ? 0x500 + (uint64_t)(rts * 16 + j) : 0;
        w->blend_arg = rn(3) ? 0xffffffffu : rn(16); w->prim = (uint8_t)rn(3); w->alpha_test = (uint8_t)rn(2);
    }
    unsigned long long n = 0;
    for (int it = 0; it < 400000; it++) {
        const nrd_raw *w = &pool[rn(3) ? rn(600) : rn(40)];
        const int rep = rn(4) ? 1 : 1 + (int)rn(6);   // runs of the same state (instances of one material)
        for (int r = 0; r < rep; r++, n++) {
            uint32_t id = 0, why = 0, rid = 0, rwhy = 0; nrd_key k;
            lean_ident(&c, w, &id, &why);
            nrd_key_of(w, ref_rtfmt(w->rt), &k); ref_key(&k, &rid, &rwhy);
            CHECK(id == rid && why == rwhy && id != 0, "draw %llu: lean id %u why %#x, shared table %u / %#x", n, id, why, rid, rwhy);
        }
    }
    CHECK(c.draws == n && c.same + c.hit + c.miss == n, "counters add up (%llu draws: %llu same, %llu table, %llu shared)", (unsigned long long)c.draws, (unsigned long long)c.same, (unsigned long long)c.hit, (unsigned long long)c.miss);
    CHECK(g_shared_lookups == c.miss && c.miss < n / 20, "the shared table is asked for a key the thread has not seen only (%llu of %llu draws)", g_shared_lookups, n);
    printf("  identity: %llu draws, same as the previous %.1f%%, thread table %.1f%%, shared table %.2f%% (%u keys), RT format looked up %.1f%%\n", n, 100. * (double)c.same / (double)n,
           100. * (double)c.hit / (double)n, 100. * (double)c.miss / (double)n, g_nshared, 100. * (double)g_rt_lookups / (double)n);
    // a full shared table (id 0) is never kept: every such draw asks again, as the old path does
    static nrd_idc d; memset(&d, 0, sizeof d);
    nrd_raw w = pool[0]; nrd_key k; uint32_t id = 5, why = 0; int rt_same = 1;
    CHECK(!nrd_ident_same(&d, &w, &id, &why, &rt_same) && !rt_same, "nothing before the first draw");
    CHECK(!nrd_ident_find(&d, &w, 3, &k, &id, &why), "empty table");
    nrd_ident_put(&d, &w, 3, &k, 0, 128);
    CHECK(!nrd_ident_same(&d, &w, &id, &why, &rt_same) && !nrd_ident_find(&d, &w, 3, &k, &id, &why), "id 0 is not kept");
    nrd_ident_put(&d, &w, 3, &k, 42, 1);
    CHECK(nrd_ident_same(&d, &w, &id, &why, &rt_same) && id == 42 && why == 1, "kept after a real id");
    nrd_raw w2 = w; w2.alpha_test ^= 1;
    CHECK(!nrd_ident_same(&d, &w2, &id, &why, &rt_same) && rt_same && !nrd_ident_find(&d, &w2, 3, &k, &id, &why), "one byte of the state differs: another key, same render targets");
    w2 = w; w2.rt[8] ^= 1;
    CHECK(!nrd_ident_same(&d, &w2, &id, &why, &rt_same) && !rt_same, "a render target differs: the RT format set is looked up again");
    CHECK(sizeof(nrd_raw) == 120 && sizeof(nrd_key) == 56, "layouts (%zu, %zu)", sizeof(nrd_raw), sizeof(nrd_key));
}

// ================= 5. cost model (x86, information only) =================
static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }
static void bench(void) {
    // the tap: 600 draws of one pass, 60 materials, each draw rebinding 3 slots and its program (the engine's code 21 + flush pattern)
    world *w0 = W[0], *w1 = W[1];
    for (int v = 0; v < 2; v++) {
        world *w = v ? w1 : w0; memset(w, 0, sizeof *w); w->mode = v; w->fresh = 1; w->rd_node = -1; w->memo.node = -1; nb_frame_reset(&w->f, 1);
    }
    memset(g_pm, 0, sizeof g_pm); memset(g_pp, 0, sizeof g_pp); g_writes = 1; g_reggen = 0;
    for (int p = 1; p <= T_PROGS; p++) { g_pm[p].tex[0] = 1; g_pm[p].tex[1] = 0x3f; g_pm[p].pub = 1; g_pp[p].fs = (uint64_t)p; g_pp[p].pub = 1; }   // serials 1..T_PROGS
    for (int i = 0; i < T_IDS; i++) g_reg[i] = 0x1000000ull + (uint64_t)i * 16;
    double t[2];
    for (int v = 0; v < 2; v++) {
        world *w = v ? w1 : w0; const double a = now_s(); g_rng = 99;
        for (int frame = 0; frame < 3000; frame++) {
            w_reset(w); nb_att at[2] = {{0x9000000ull, 0}, {0x9800000ull, NB_SLOT_DS}}; nb_pass_begin(&w->f, at, 2);
            for (int d = 0; d < 600; d++) {
                const uint32_t mat = rn(3) ? (uint32_t)(d / 4 % 60) : rn(60);
                w_program(w, 1 + mat % T_PROGS, 0x1f);   /* generation 0 */
                for (int s = 0; s < 3; s++) w_bind(w, 0, 1, s, (mat * 3 + (uint32_t)s) % T_IDS);
                w_draw(w);
            }
        }
        t[v] = now_s() - a;
    }
    printf("  cost model (this PC, not the phone): tap walk per draw %.0f ns, with the memo %.0f ns (%.1f%% of the walks skipped)\n", t[0] / 1.8e6 * 1e9, t[1] / 1.8e6 * 1e9,
           100. * (double)w1->hits / (double)(w1->draws ? w1->draws : 1));
}

int main(void) {
    printf("nr_direct_test (NR-09R, DM_NR_DIRECT)\n");
    test_tail();
    test_mirror();
    test_tap();
    test_ident();
    bench();
    printf("%s: %llu checks, %llu failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
