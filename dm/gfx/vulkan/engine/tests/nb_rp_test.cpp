// [dos2-nb] NB-06: host test of the header -> host pass decision (dm/gfx/nb/nb_rp.h, used by xbg_nb.inc nbrp under DM_NB_RP).
// Synthetic headers (attachment order, mismatches, bounds) and headers filled from real nb_plan output the way nvn_nb_tap_patch fills them.
// Build (Windows clang + MSVC libs, or WSL g++), from this folder:
//   clang -std=c11 -c ../../../nb/nb_plan.c -D_CRT_SECURE_NO_WARNINGS && clang++ -std=c++17 -Wall -D_CRT_SECURE_NO_WARNINGS nb_rp_test.cpp nb_plan.o -o nb_rp_test
//   gcc -std=c11 -c ../../../nb/nb_plan.c && g++ -std=c++17 -Wall nb_rp_test.cpp nb_plan.o -o nb_rp_test
#include "../../../nb/nb_rp.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

// The header fields nb_rp_map reads (nvn_obj.h p_nb_pass without the reads).
struct Hdr { uint8_t natt = 0, plan_ok = 1, flags = 0; uint64_t att[NB_MAX_ATT] = {}; uint8_t load[NB_MAX_ATT] = {}, store[NB_MAX_ATT] = {}; uint16_t cover_dc = 0; };
static int map(const Hdr &h, unsigned n, const uint64_t *att, nb_rp_dec *d) {
    return nb_rp_map(h.plan_ok, h.natt, h.att, h.load, h.store, h.cover_dc, h.flags, n, att, d);
}
// As nvn_nb_tap_patch: attachments of IR node i, the plan's load / store / flags / cover_dc.
static Hdr from_plan(const nb_frame *f, const nb_plan_out *o, int i) {
    Hdr h; const nb_node *n = &f->node[i]; h.natt = n->natt;
    for (int k = 0; k < n->natt; k++) { h.att[k] = n->att[k].image; h.load[k] = o->node[i].load[k]; h.store[k] = o->node[i].store[k]; }
    h.flags = o->node[i].flags; h.cover_dc = o->node[i].cover_dc; return h;
}

static nb_frame F; static nb_plan_out O;
static const nb_rules EXACT = { NB_RULES_EXACT, 0, 0 };
static const uint32_t BLACK[4] = { 0, 0, 0, 0x3f800000u };
enum : uint64_t { A = 0x1000, B = 0x2000, D = 0x3000, T = 0x4000 };
static void pass(uint64_t c0, uint64_t ds) {
    nb_att a[2]; int n = 0;
    if (c0) { a[n].image = c0; a[n].slot = 0; n++; }
    if (ds) { a[n].image = ds; a[n].slot = NB_SLOT_DS; n++; }
    nb_pass_begin(&F, a, n);
}
static void clear_rt(uint64_t image, unsigned asp) { nb_clear(&F, image, asp, NB_CLEAR_FULL | NB_CLEAR_RT, BLACK, 1.0f, 0); }

static void test_map() {
    Hdr h; h.natt = 3; h.att[0] = A; h.att[1] = B; h.att[2] = D;
    h.load[0] = NB_LOAD_CLEAR; h.load[1] = NB_LOAD; h.load[2] = NB_LOAD_DONT_CARE;
    h.store[0] = NB_STORE; h.store[1] = NB_STORE; h.store[2] = NB_STORE_DONT_CARE; h.cover_dc = 1u << 1; h.flags = NB_NP_EXEC | NB_NP_PREBAR;
    nb_rp_dec d;
    // Host order B, A, D: masks follow the host index.
    const uint64_t host[3] = { B, A, D };
    CHECK(map(h, 3, host, &d) == NB_RP_OK);
    CHECK(d.n == 3 && d.clear == 2u && d.dc == 4u && d.cover == 1u && d.store_dc == 4u && d.flags == (NB_NP_EXEC | NB_NP_PREBAR));
    // Same order.
    const uint64_t same[3] = { A, B, D };
    CHECK(map(h, 3, same, &d) == NB_RP_OK && d.clear == 1u && d.cover == 2u && d.dc == 4u);
    // Fallbacks: no plan, other count, an image the header lacks, a duplicate host attachment, null image, more than 9.
    Hdr np = h; np.plan_ok = 0; CHECK(map(np, 3, host, &d) == NB_RP_NO_PLAN);
    CHECK(map(h, 2, host, &d) == NB_RP_ATT);
    const uint64_t other[3] = { A, B, T }; CHECK(map(h, 3, other, &d) == NB_RP_ATT);
    const uint64_t dup[3] = { A, A, D }; CHECK(map(h, 3, dup, &d) == NB_RP_ATT);
    const uint64_t null3[3] = { A, 0, D }; CHECK(map(h, 3, null3, &d) == NB_RP_ATT);
    Hdr hd = h; hd.att[1] = A; const uint64_t hdup[3] = { A, B, D }; CHECK(map(hd, 3, hdup, &d) == NB_RP_ATT);   // the header names A twice
    uint64_t ten[10]; for (int i = 0; i < 10; i++) ten[i] = 0x100 * (i + 1);
    Hdr big; big.natt = 200; for (int i = 0; i < NB_MAX_ATT; i++) big.att[i] = ten[i];   // natt beyond the record: clamped, never read past 9
    CHECK(map(big, 10, ten, &d) == NB_RP_ATT);
    CHECK(map(big, 9, ten, &d) == NB_RP_OK && d.n == 9);
    Hdr none; none.natt = 0; CHECK(map(none, 0, ten, &d) == NB_RP_OK && d.n == 0);   // a pass without attachments
}

static void test_twin() {
    nb_rp_dec d{}; d.n = 3; d.clear = 1u; d.dc = 4u; d.cover = 2u; d.store_dc = 4u;
    // The host holds the clear of attachment 0 (colour bit 0): CLEAR, store DONT_CARE of 2, load DONT_CARE of 2; cover only when proven.
    nb_rp_twin_out t = nb_rp_twin(&d, 1u, 0);
    CHECK(t.mask == (1ull | (1ull << 20) | (1ull << 34)) && t.dc == 4u && t.clear_missing == 0 && t.clear_extra == 0);
    t = nb_rp_twin(&d, 1u, 1);
    CHECK(t.mask == (1ull | (1ull << 20) | (1ull << 33) | (1ull << 34)) && t.dc == 6u);
    // Plan CLEAR without a pending host value: LOAD (the host executed that clear already), counted.
    t = nb_rp_twin(&d, 0u, 0);
    CHECK(!(t.mask & 3ull) && t.clear_missing == 1);
    // A host clear the plan does not fold is still folded (the clear must happen), and wins over DONT_CARE.
    t = nb_rp_twin(&d, 1u | (3u << 4), 1);   // depth + stencil clear on attachment 2
    CHECK(t.clear_extra == 1 && (t.mask & (3ull << 4)) && !(t.mask & (1ull << 34)) && !(t.dc & 4u));
    // Nothing decided: the pass as created.
    nb_rp_dec z{}; z.n = 2; t = nb_rp_twin(&z, 0, 1); CHECK(t.mask == 0 && t.dc == 0);
}

// Headers from the planner: the clear-only pass is dropped, the next pass loads CLEAR, depth without a reader stores DONT_CARE.
static void test_from_plan() {
    nb_frame_reset(&F, 1);
    pass(A, D); clear_rt(A, NB_ASP_COLOR); clear_rt(D, NB_ASP_DEPTH | NB_ASP_STENCIL);
    pass(A, D); nb_draw(&F, 0); nb_draw(&F, 0);
    pass(B, 0); nb_read(&F, -1, A); nb_draw(&F, 0);
    nb_submit(&F, 1);
    CHECK(nb_plan(&F, nullptr, &EXACT, &O) == 0);
    nb_rp_dec d;
    Hdr h0 = from_plan(&F, &O, 0); const uint64_t ad[2] = { A, D };
    CHECK(map(h0, 2, ad, &d) == NB_RP_OK && (d.flags & NB_NP_CLEAR_ONLY) && d.clear == 0 && d.store_dc == 0);   // dropped pass: L / S if run
    Hdr h1 = from_plan(&F, &O, 1); const uint64_t da[2] = { D, A };                                              // host lists depth first
    CHECK(map(h1, 2, da, &d) == NB_RP_OK && d.clear == 3u && d.store_dc == 1u && (d.flags & NB_NP_EXEC));
    nb_rp_twin_out t = nb_rp_twin(&d, 1u | 3u << 2, 0);   // the host's lazy pass holds both clears (colour at 1, depth+stencil at 0)
    CHECK(t.clear_missing == 0 && t.clear_extra == 0 && (t.mask >> 18 & 0x1ff) == 1u && t.dc == 0);
    Hdr h2 = from_plan(&F, &O, 2); const uint64_t b[1] = { B };
    CHECK(map(h2, 1, b, &d) == NB_RP_OK && (d.flags & NB_NP_PREBAR) && d.clear == 0 && d.dc == 0);   // reads A written by pass 1: barrier before
    const uint64_t wrong[1] = { A }; CHECK(map(h2, 1, wrong, &d) == NB_RP_ATT);
}

// 16.11 store-hist: a host attachment the host banned after a violation keeps STORE whatever the plan says (nb_rp_keep_store).
static void test_keep_store() {
    nb_rp_dec d{}; d.n = 3; d.store_dc = 5u; d.clear = 1u;
    nb_rp_twin_out t = nb_rp_twin(&d, 1u, 0);
    CHECK((t.mask >> 18 & 0x1ff) == 5u);
    nb_rp_twin_out u = t; CHECK(nb_rp_keep_store(&u, 0) == 0 && u.mask == t.mask);                  // nothing banned: unchanged
    u = t; CHECK(nb_rp_keep_store(&u, 4u) == 1 && (u.mask >> 18 & 0x1ff) == 1u && (u.mask & 3ull));   // attachment 2 kept, clear stays
    u = t; CHECK(nb_rp_keep_store(&u, 2u) == 0 && u.mask == t.mask);                                 // a STORE attachment: nothing to keep
    u = t; CHECK(nb_rp_keep_store(&u, 0x1ffu) == 2 && !(u.mask >> 18 & 0x1ff) && u.dc == t.dc);
}

int main() {
    test_map(); test_twin(); test_from_plan(); test_keep_store();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("nb_rp_test: all passed\n"); return 0;
}
