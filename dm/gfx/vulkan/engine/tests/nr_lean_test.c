// [DOS2] NR-13 / NR-26 host test: DM_NR_LEAN (dm/gfx/nr/nr_lean.h, the NB-13a shadow dm/gfx/nvn/nvn_nb_state.h; docs/dos2/nr/NR-13.md).
// - the switches: parse, defaults, out-of-range values, the A/B phase (off N / on 2N / off N), NRL_STATE eligibility, NRL_UBO rule;
// - the fallback rule of NRL_STATE, as a model of the whole stream: random state call sequences where the capture is
//     (a) on all the time (DM_NR_LEAN=0), (b) never taken (DM_NR_LEAN=1: every call a plain record), (c) paused and resumed at random
//     "frame" boundaries (DM_NR_LEAN_AB: flush at the pause, nbs_resume at the return),
//   executed by a backend model (plain records one by one, OP_X_NB_STATE blocks DEF / REF / NOID) against the same calls applied
//   directly: at EVERY draw - served by the NR lane or left to the old path makes no difference, both read this state - the backend state
//   must be identical in all three;
// - mutation: a resume that keeps the shadow's memory of what it emitted (no nbs_resume) must be caught by the same check;
// - the model numbers of NR-13.md §3: records / bytes the backend replays with and without the capture, and the shadow + dictionary
//   time against the plain copy (PC nanoseconds; printed, not asserted).
// Build + run (from this directory):
//   clang -std=c11 -O2 -Wall -Wextra -DDM_GAME_DOS2 -D_CRT_SECURE_NO_WARNINGS -I../../../nvn/include nr_lean_test.c -o nr_lean_test && ./nr_lean_test
#include "../../../nvn/nvn_obj.h"
#include "../../../nr/nr_lean.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int fails; static uint64_t checks;
#define CHECK(x) do { checks++; if (!(x)) { if (fails < 20) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
static uint64_t rnd_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 7; rnd_state ^= rnd_state << 17; return rnd_state; }
static uint64_t now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

// ---- a record as hle_nvn.c rec() writes it (same helpers as nb_state_test.c) ----
typedef struct { uint8_t b[NBS_REC_MAX]; uint32_t size; } recbuf;
static recbuf mk(uint32_t op, const void *pl, size_t n) {
    recbuf r; memset(&r, 0, sizeof r); r.size = (uint32_t)((8 + n + 7) & ~(size_t)7);
    const uint32_t h[2] = {op, r.size}; memcpy(r.b, h, 8); memcpy(r.b + 8, pl, n); return r;
}
static recbuf r_blend(int target, int src) { p_bind_state s; memset(&s, 0, sizeof s); blend_st b; memset(&b, 0, sizeof b); b.target = (uint64_t)target; b.src_rgb = (uint64_t)src; memcpy(s.bytes, &b, sizeof b); return mk(OP_BIND_STATE, &s, sizeof s); }
static recbuf r_state(int kind, uint32_t v) { p_bind_state s; memset(&s, 0, sizeof s); s.kind = kind; memcpy(s.bytes, &v, 4); return mk(OP_BIND_STATE, &s, sizeof s); }
static recbuf r_stencil(int face, int which, int v) { p_stencil s = {face, which == 0 ? v : 0, which == 1 ? v : 0, which == 2 ? v : 0, which}; return mk(OP_STENCIL, &s, sizeof s); }
static recbuf r_prog(uintptr_t p, int stages) { p_bind_program b; memset(&b, 0, sizeof b); b.prog = (const prog_t *)p; b.stages = stages; return mk(OP_BIND_PROGRAM, &b, sizeof b); }
static recbuf r_vbuf(int first, int n, uint64_t a) { p_bind_vbuf b; memset(&b, 0, sizeof b); b.first = first; b.n = n; for (int i = 0; i < n; i++) { b.r[i].address = a + (uint64_t)i * 0x100; b.r[i].size = 64; } return mk(OP_BIND_VBUF, &b, offsetof(p_bind_vbuf, r) + sizeof(NVNbufferRange) * (size_t)n); }
static recbuf r_vattr(int n, uint32_t seed) { p_bind_vattr b; memset(&b, 0, sizeof b); b.n = n; for (int i = 0; i < n; i++) b.st[i] = seed + (uint32_t)i; return mk(OP_BIND_VATTR, &b, offsetof(p_bind_vattr, st) + 4 * (size_t)n); }
static recbuf r_vstream(int n, uint32_t seed) { p_bind_vstream b; memset(&b, 0, sizeof b); b.n = n; for (int i = 0; i < n; i++) { b.st[i].stride = seed + (uint32_t)i; b.st[i].divisor = 0; } return mk(OP_BIND_VSTREAM, &b, offsetof(p_bind_vstream, st) + sizeof(vs_st) * (size_t)n); }
static recbuf r_vp(float x) { p_viewports v; memset(&v, 0, sizeof v); v.first = 0; v.n = 1; v.v[0][0] = x; v.v[0][2] = 100; v.v[0][3] = 50; return mk(OP_VIEWPORTS, &v, offsetof(p_viewports, v) + sizeof v.v[0]); }
static recbuf r_scissor(int x) { p_scissor s = {x, 0, 64, 32}; return mk(OP_SCISSOR, &s, sizeof s); }
static recbuf r_drange(float n) { p_depth_range d = {n, 1.f}; return mk(OP_DEPTH_RANGE, &d, sizeof d); }
static recbuf r_poff(float f) { p_poly_offset o = {f, 2.f, 0.f}; return mk(OP_POLY_OFFSET, &o, sizeof o); }
static recbuf r_bcolor(float r) { p_blend_color c = {{r, 0, 0, 1}}; return mk(OP_BLEND_COLOR, &c, sizeof c); }
static recbuf r_aref(float r) { p_alpha_ref a = {r, 0}; return mk(OP_X_ALPHA_REF, &a, sizeof a); }

// ---- backend model: the fields execute() sets (xbg_game.cpp), same semantics ----
typedef struct {
    uint64_t blend[8]; uint32_t color, mask, ms, poly, ds; int32_t sten[3][2]; float bias[3], bcol[4], aref;
    uint32_t nattr, attr[16]; vs_st stream[16]; NVNbufferRange vb[16]; uintptr_t shader[6];
    float vp[4], dmin, dmax; int32_t sc[4];
} model;
static uint64_t g_applied, g_applied_bytes;
static void apply(model *m, const uint8_t *rec) {
    uint32_t op, size; memcpy(&op, rec, 4); memcpy(&size, rec + 4, 4); const uint8_t *p = rec + 8;
    g_applied++; g_applied_bytes += size;
    switch (op) {
    case OP_BIND_STATE: { p_bind_state a; memcpy(&a, p, sizeof a);
        if (a.kind == 0) { blend_st b; memcpy(&b, a.bytes, sizeof b); memcpy(&m->blend[b.target], a.bytes, sizeof b); }
        else { uint32_t v; memcpy(&v, a.bytes, 4); if (a.kind == 1) m->color = v; else if (a.kind == 2) m->mask = v; else if (a.kind == 3) m->ms = v; else if (a.kind == 4) m->poly = v; else m->ds = v; }
        break; }
    case OP_STENCIL: { p_stencil a; memcpy(&a, p, sizeof a); for (int i = 0; i < 2; i++) if (a.face & (1 << i)) m->sten[a.which][i] = a.which == 0 ? a.value_mask : a.which == 1 ? a.mask : a.ref; break; }
    case OP_POLY_OFFSET: { p_poly_offset a; memcpy(&a, p, sizeof a); m->bias[0] = a.factor; m->bias[1] = a.units; m->bias[2] = a.clamp; break; }
    case OP_BLEND_COLOR: memcpy(m->bcol, p, 16); break;
    case OP_X_ALPHA_REF: memcpy(&m->aref, p, 4); break;
    case OP_BIND_VATTR: { p_bind_vattr a; memset(&a, 0, sizeof a); memcpy(&a, p, size - 8); m->nattr = (uint32_t)a.n; memset(m->attr, 0, sizeof m->attr); memcpy(m->attr, a.st, 4 * (size_t)a.n); break; }
    case OP_BIND_VSTREAM: { p_bind_vstream a; memset(&a, 0, sizeof a); memcpy(&a, p, size - 8); memcpy(m->stream + a.first, a.st, sizeof(vs_st) * (size_t)a.n); break; }
    case OP_BIND_VBUF: { p_bind_vbuf a; memset(&a, 0, sizeof a); memcpy(&a, p, size - 8); memcpy(m->vb + a.first, a.r, sizeof(NVNbufferRange) * (size_t)a.n); break; }
    case OP_BIND_PROGRAM: { p_bind_program a; memcpy(&a, p, sizeof a); for (int s = 0; s < 6; s++) if (a.stages & (1 << s)) m->shader[s] = a.prog ? (uintptr_t)a.prog ^ (uintptr_t)s : 0; break; }
    case OP_VIEWPORTS: { p_viewports a; memset(&a, 0, sizeof a); memcpy(&a, p, size - 8); memcpy(m->vp, a.v[0], 16); break; }
    case OP_SCISSOR: memcpy(m->sc, p, 16); break;
    case OP_DEPTH_RANGE: memcpy(&m->dmin, p, 4); memcpy(&m->dmax, p + 4, 4); break;
    default: break;
    }
}
static void apply_content(model *m, const uint8_t *c) {
    for (uint32_t off = 0;;) { uint32_t op, size; memcpy(&op, c + off, 4); memcpy(&size, c + off + 4, 4); if (!op) return; apply(m, c + off); off += size; }
}
// the backend's DEF table (xbg_nb_state.inc nbstate::defs); the content is copied because the test reuses its record buffer
typedef struct { uint32_t epoch, bytes; uint8_t c[NBS_SUB_BYTES]; } bdef;
static bdef g_defs[NBS_KINDS][NBS_MAX_IDS];
static int g_miss;
static void be_exec(model *m, const uint8_t *p, uint32_t n) {
    nbs_hdr h; nbs_view v[NBS_KINDS];
    const int nb = nbs_parse(p, n, &h, v);
    CHECK(nb > 0);
    for (int i = 0; i < nb; i++) {
        const nbs_view *b = &v[i]; const uint8_t *c = b->content;
        if (b->b.how == NBS_B_REF) { bdef *d = &g_defs[b->b.kind][b->b.id]; if (d->epoch != h.epoch) { g_miss++; continue; } c = d->c; }
        else if (b->b.how == NBS_B_DEF) { bdef *d = &g_defs[b->b.kind][b->b.id]; d->epoch = h.epoch; d->bytes = b->b.bytes; memcpy(d->c, c, b->b.bytes); }
        apply_content(m, c);
    }
}

static nbs_state S;
static uint8_t out[NBS_PAYLOAD_MAX];

// ---- 1. the switches ----
static void test_switches(void) {
    nr_lean_cfg c;
    CHECK(nr_lean_parse(NULL, NULL, NULL, &c) == 0 && c.mode == 0 && c.parts == 0 && c.ab == 0);              // default: off, nothing armed
    CHECK(nr_lean_parse("", "", "", &c) == 0 && c.mode == 0 && c.parts == 0);
    CHECK(nr_lean_parse("0", "3", "300", &c) == 0 && c.mode == 0 && c.parts == 0 && c.ab == 0);              // 0 wins over the others
    CHECK(nr_lean_parse("1", NULL, NULL, &c) == 0 && c.mode == 1 && c.parts == NRL_ALL && c.ab == 0);
    CHECK(nr_lean_parse("1", "1", "300", &c) == 0 && c.mode == 1 && c.parts == NRL_STATE && c.ab == 300);
    CHECK(nr_lean_parse("1", "0x2", NULL, &c) == 0 && c.parts == NRL_UBO);
    CHECK(nr_lean_parse("2", NULL, "300", &c) == 0 && c.mode == 2 && c.parts == NRL_ALL && c.ab == 0);        // no A/B at 2: nothing is skipped
    CHECK(nr_lean_parse("3", NULL, NULL, &c) == NRL_BAD_MODE && c.mode == 0 && c.parts == 0);                // 3 (poison) does not exist: off
    CHECK(nr_lean_parse("-1", NULL, NULL, &c) == NRL_BAD_MODE && c.mode == 0);
    CHECK(nr_lean_parse("1", "8", NULL, &c) == NRL_BAD_PARTS && c.mode == 1 && c.parts == NRL_ALL);
    CHECK(nr_lean_parse("1", NULL, "-5", &c) == NRL_BAD_AB && c.ab == 0);
    CHECK(nr_lean_state_why(1, 0, 0) == NRL_ST_OK);
    CHECK(nr_lean_state_why(0, 0, 0) == NRL_ST_NOBITS && nr_lean_state_why(1, 1, 0) == NRL_ST_RELOC && nr_lean_state_why(1, 0, 3) == NRL_ST_ORDER);
    CHECK(nr_lean_state_why(1, 2, 2) == NRL_ST_RELOC);
    for (int w = 0; w < 4; w++) CHECK(nr_lean_state_name(w)[0] != '?');
    CHECK(nr_lean_ubo_skip(1, 1) == 1 && nr_lean_ubo_skip(1, 0) == 0 && nr_lean_ubo_skip(0, 1) == 0 && nr_lean_ubo_skip(0, 0) == 0);
    // ABBA: off N, on 2N, off N; equal frames in both phases over every 4N window; n = 0 = always on
    for (unsigned n = 1; n <= 300; n += 13) {
        uint64_t on = 0, changes = 0; int prev = nr_lean_ab_on(0, n); CHECK(prev == 0);
        for (uint64_t f = 0; f < 8ull * n; f++) { const int p = nr_lean_ab_on(f, n); on += (uint64_t)p; if (p != prev) changes++; prev = p; }
        CHECK(on == 4ull * n); CHECK(changes == 4);   // 0 -> 1 at N, 1 -> 0 at 3N, twice (the phases of two windows touch: off N + off N)
        CHECK(nr_lean_ab_on(n, n) == 1 && nr_lean_ab_on(3ull * n - 1, n) == 1 && nr_lean_ab_on(3ull * n, n) == 0 && nr_lean_ab_on(4ull * n, n) == 0);
    }
    for (uint64_t f = 0; f < 100; f++) CHECK(nr_lean_ab_on(f, 0) == 1);
}

// ---- 2. the fallback rule: three streams, one state at every draw ----
static recbuf random_call(void) {
    const int k = (int)(rnd() % 13); const int v = (int)(rnd() % 3);   // few values: repeats and REF happen
    switch (k) {
    case 0: return r_blend((int)(rnd() % 3), v);
    case 1: return r_state(1 + (int)(rnd() % 3), (uint32_t)v);
    case 2: return r_state(4, (uint32_t)v);
    case 3: return r_state(5, (uint32_t)v);
    case 4: return r_stencil(1 + (int)(rnd() % 3), (int)(rnd() % 3), v);
    case 5: return rnd() & 1 ? r_vattr(1 + (int)(rnd() % 4), (uint32_t)v) : r_vstream(1 + (int)(rnd() % 4), (uint32_t)v);
    case 6: return r_vbuf((int)(rnd() % 3), 1 + (int)(rnd() % 3), 0x10000u * (uint64_t)v);
    case 7: return r_prog(0x1000u * (uintptr_t)(1 + v), rnd() & 1 ? 0x1f : 0x11);
    case 8: return r_vp((float)v);
    case 9: return r_scissor(v);
    case 10: return r_drange((float)v);
    case 11: return rnd() & 1 ? r_poff((float)v) : r_bcolor((float)v);
    default: return r_aref((float)v);
    }
}
static void fe_flush(model *be) { const uint32_t n = nbs_pack(&S, out, sizeof out); if (n) be_exec(be, out, n); }
static void fe_plain(model *be, const recbuf *c) {   // nvn_nb_state.inc: what is pending first, the record itself, its kind unknown
    fe_flush(be); apply(be, c->b);
    uint32_t key; const int k = nbs_kind(*(const uint32_t *)c->b, c->b + 8, c->size - 8, &key);
    if (k >= 0) nbs_unknown(&S, 1u << k);
}
// mode: 0 = capture always on, 1 = never taken, 2 = paused / resumed at frame boundaries; mutate: the resume skips nbs_resume
static uint64_t run_stream(int mode, int mutate, uint64_t *draws_out, uint64_t *pauses_out) {
    memset(g_defs, 0, sizeof g_defs);
    uint64_t draws = 0, bad = 0, pauses = 0;
    for (int run = 0; run < 24; run++) {
        model ref, be; memset(&ref, 0, sizeof ref); memset(&be, 0, sizeof be);
        uint32_t epoch = 100 + (uint32_t)run * 100000; nbs_init(&S, epoch);
        int on = mode != 1;   // the capture
        const int plain_pct = run % 3 == 0 ? 0 : run % 3 == 1 ? 10 : 45;   // plain state records outside taken packets (1 397 a frame on the phone)
        for (int step = 0; step < 6000; step++) {
            const uint64_t r = rnd() % 100;
            if (r < 14) {   // a draw - NR lane or old path, both read the backend state now
                if (on) fe_flush(&be);
                draws++; if (memcmp(&ref, &be, sizeof ref)) bad++;
            } else if (r < 15) {   // EndRecording: the epoch ends
                if (on) { fe_flush(&be); nbs_new_epoch(&S, ++epoch); }
            } else if (r < 17 && mode == 2) {   // a frame boundary of the A/B: the phase may change
                if (on) { fe_flush(&be); on = 0; pauses++; }                          // nvn_nb_state_pause(1)
                else { if (!mutate) nbs_resume(&S, ++epoch); on = 1; }                // nvn_nb_state_pause(0)
            } else {
                recbuf c = random_call();
                apply(&ref, c.b);
                if (!on) apply(&be, c.b);                                              // the plain NVN record, executed as it comes
                else if ((int)(rnd() % 100) < plain_pct) fe_plain(&be, &c);
                else if (nbs_capture(&S, c.b, c.size) < 0) fe_plain(&be, &c);         // does not fit: the fallback of nvn_nb_state.inc
            }
        }
    }
    *draws_out = draws; if (pauses_out) *pauses_out = pauses;
    return bad;
}
static void test_fallback_rule(void) {
    uint64_t d, p;
    g_miss = 0; uint64_t bad = run_stream(0, 0, &d, NULL);
    printf("stream A (capture on, DM_NR_LEAN=0): %llu draws, %llu see another state than the direct calls\n", (unsigned long long)d, (unsigned long long)bad);
    CHECK(bad == 0 && g_miss == 0 && d > 10000);
    bad = run_stream(1, 0, &d, NULL);
    printf("stream B (state packets not taken, DM_NR_LEAN=1): %llu draws, %llu differ\n", (unsigned long long)d, (unsigned long long)bad);
    CHECK(bad == 0 && g_miss == 0 && d > 10000);
    bad = run_stream(2, 0, &d, &p);
    printf("stream C (paused / resumed %llu times, DM_NR_LEAN_AB): %llu draws, %llu differ, REF without DEF %d\n", (unsigned long long)p, (unsigned long long)d, (unsigned long long)bad, g_miss);
    CHECK(bad == 0 && g_miss == 0 && p > 500);
    g_miss = 0; bad = run_stream(2, 1, &d, &p);
    printf("mutation (resume without nbs_resume): %llu of %llu draws see a stale state, REF without DEF %d - caught\n", (unsigned long long)bad, (unsigned long long)d, g_miss);
    CHECK(bad > 0);
    // nbs_resume itself: nothing held, nothing remembered as emitted, a new epoch, nothing dirty
    nbs_init(&S, 5); { recbuf c = r_blend(0, 1); CHECK(nbs_capture(&S, c.b, c.size) >= 0); } CHECK(nbs_pack(&S, out, sizeof out) > 0);
    { recbuf c = r_scissor(2); CHECK(nbs_capture(&S, c.b, c.size) >= 0); }
    nbs_resume(&S, 9);
    CHECK(S.epoch == 9 && S.dirty == 0); for (int k = 0; k < NBS_KINDS; k++) CHECK(S.s[k].n == 0 && S.s[k].used == 0 && S.emitted[k] == -1 && S.d[k].nid == 0);
    CHECK(nbs_pack(&S, out, sizeof out) == 0);
    { recbuf c = r_blend(0, 1); CHECK(nbs_capture(&S, c.b, c.size) >= 0); const uint32_t n = nbs_pack(&S, out, sizeof out); nbs_hdr h; nbs_view v[NBS_KINDS];
      CHECK(n > 0 && nbs_parse(out, n, &h, v) == 1 && v[0].b.how == NBS_B_DEF && h.epoch == 9); }   // the same content again: a DEF of the new epoch, not a REF
}

// ---- 3. model numbers (NR-13.md §3): a frame like the phone's - 982 draws, ~2.7 state calls a draw, ~400 meshes, a few dozen materials ----
enum { DRAWS = 982, FRAMES = 300, MAXCALLS = 8 };
static recbuf g_calls[DRAWS][MAXCALLS]; static int g_ncalls[DRAWS];
static void make_frame(void) {
    uint64_t total = 0;
    int mat = 0;
    for (int d = 0; d < DRAWS; d++) {
        int n = 0;
        const int mesh = (int)(rnd() % 400);
        g_calls[d][n++] = r_vbuf(0, 1 + (mesh & 1), 0x10000000ull + (uint64_t)mesh * 0x4000);            // every draw: its vertex buffers
        if (rnd() % 100 < 35) {   // a material change: program + some of the state groups + vertex format
            mat = (int)(rnd() % 48);
            g_calls[d][n++] = r_prog(0x1000u * (uintptr_t)(1 + mat), 0x11);
            if (rnd() % 100 < 50) g_calls[d][n++] = r_blend(0, mat % 5);
            if (rnd() % 100 < 60) g_calls[d][n++] = r_state(5, (uint32_t)(mat % 7));
            if (rnd() % 100 < 30) g_calls[d][n++] = r_state(4, (uint32_t)(mat % 3));
            if (rnd() % 100 < 60) { g_calls[d][n++] = r_vattr(3 + mat % 3, (uint32_t)(mat % 6)); g_calls[d][n++] = r_vstream(1 + (mat & 1), (uint32_t)(mat % 6)); }
        }
        if (rnd() % 100 < 4) g_calls[d][n++] = r_vp((float)(rnd() % 6));
        g_ncalls[d] = n; total += (uint64_t)n;
    }
    printf("model frame: %d draws, %.2f state calls a draw (phone: 2.72)\n", DRAWS, (double)total / DRAWS);
}
static uint8_t g_stream[1 << 20];
static void test_model(void) {
    make_frame();
    model be; uint64_t t0, fe_cap = 0, fe_plain_ns = 0, be_cap = 0, be_plain = 0, bytes_cap = 0, bytes_plain = 0, rec_cap, rec_plain, rb_cap, rb_plain, blocks = 0, records = 0;
    volatile uint64_t sink = 0;
    // with the capture: the entry point writes its payload into the capture slot, nbs_capture files it, the draw packs the dirty kinds
    memset(&be, 0, sizeof be); memset(g_defs, 0, sizeof g_defs); nbs_init(&S, 1); uint32_t epoch = 1; g_applied = g_applied_bytes = 0;
    static uint32_t rec_off[DRAWS], rec_len[DRAWS];
    for (int f = 0; f < FRAMES; f++) {
        uint32_t used = 0;
        t0 = now_ns();
        for (int d = 0; d < DRAWS; d++) {
            for (int i = 0; i < g_ncalls[d]; i++) { uint8_t slot[NBS_REC_MAX]; const recbuf *c = &g_calls[d][i]; memset(slot, 0, c->size); memcpy(slot, c->b, c->size); nbs_capture(&S, slot, c->size); }
            const uint32_t n = nbs_pack(&S, out, sizeof out);
            rec_off[d] = used; rec_len[d] = n;
            if (n) { memcpy(g_stream + used, out, n); used += (n + 8 + 7) & ~7u; records++; }
        }
        fe_cap += now_ns() - t0; bytes_cap += used;
        t0 = now_ns();
        for (int d = 0; d < DRAWS; d++) if (rec_len[d]) { nbs_hdr h; nbs_view v[NBS_KINDS]; const int nb = nbs_parse(g_stream + rec_off[d], rec_len[d], &h, v); blocks += (uint64_t)(nb > 0 ? nb : 0); be_exec(&be, g_stream + rec_off[d], rec_len[d]); }
        be_cap += now_ns() - t0; sink += be.ds;
        nbs_new_epoch(&S, ++epoch); nbs_new_epoch(&S, ++epoch);   // 2 epochs a frame (phone)
    }
    rec_cap = g_applied; rb_cap = g_applied_bytes;
    // without: every call is its plain record in the stream, executed one by one
    memset(&be, 0, sizeof be); g_applied = g_applied_bytes = 0;
    for (int f = 0; f < FRAMES; f++) {
        uint32_t used = 0;
        t0 = now_ns();
        for (int d = 0; d < DRAWS; d++) for (int i = 0; i < g_ncalls[d]; i++) { const recbuf *c = &g_calls[d][i]; memcpy(g_stream + used, c->b, c->size); used += c->size; }
        fe_plain_ns += now_ns() - t0; bytes_plain += used;
        t0 = now_ns();
        for (uint32_t off = 0; off < used;) { uint32_t size; memcpy(&size, g_stream + off + 4, 4); apply(&be, g_stream + off); off += size; }
        be_plain += now_ns() - t0; sink += be.ds;
    }
    rec_plain = g_applied; rb_plain = g_applied_bytes;
    const double n = (double)FRAMES * DRAWS;
    printf("front end, a draw (PC ns): shadow + dictionary + pack %.0f, plain copy %.0f  -> the capture costs %.0f ns a draw more (x %d draws = %.3f ms a frame on this PC)\n",
           fe_cap / n, fe_plain_ns / n, (fe_cap - (double)fe_plain_ns) / n, DRAWS, (fe_cap - (double)fe_plain_ns) / FRAMES / 1e6);
    printf("stream, a frame: %.1f KB of OP_X_NB_STATE (%.2f records, %.2f blocks a draw) vs %.1f KB of plain records\n", bytes_cap / 1024. / FRAMES, records / n, blocks / n, bytes_plain / 1024. / FRAMES);
    printf("backend, a frame: %.0f state records replayed from blocks (%.1f KB) vs %.0f plain records (%.1f KB): x%.2f records, x%.2f bytes; model apply %.0f vs %.0f ns a draw\n",
           (double)rec_cap / FRAMES, rb_cap / 1024. / FRAMES, (double)rec_plain / FRAMES, rb_plain / 1024. / FRAMES, (double)rec_cap / (double)rec_plain, (double)rb_cap / (double)rb_plain,
           be_cap / n, be_plain / n);
    CHECK(rec_cap >= rec_plain);   // a block replays its whole kind: never fewer records than the calls
    CHECK(sink != 0xdeadbeefdeadbeefull);
}

int main(void) {
    test_switches();
    test_fallback_rule();
    test_model();
    printf("%s: %llu checks, %d failed\n", fails ? "FAIL" : "PASS", (unsigned long long)checks, fails);
    return fails ? 1 : 0;
}
