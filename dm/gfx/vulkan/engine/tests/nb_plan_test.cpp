// [dos2-nb] NB-02: host test of the NB frame IR and the tiler load/store planner (dm/gfx/nb). Synthetic frames: clear-only pass,
// depth without a reader, cross-frame reader, A-B-A as is, mid-frame submission, iPad contract, transient candidates, dump/parse.
// Build (Windows clang + MSVC libs, or WSL g++):
//   clang -std=c11 -c ../../../nb/nb_plan.c -D_CRT_SECURE_NO_WARNINGS && clang++ -std=c++17 -Wall -D_CRT_SECURE_NO_WARNINGS nb_plan_test.cpp nb_plan.o -o nb_plan_test
//   gcc -std=c11 -c ../../../nb/nb_plan.c && g++ -std=c++17 -Wall nb_plan_test.cpp nb_plan.o -o nb_plan_test
// Golden frames: nb_plan_test <dump.txt> parses an IR text dump (DM_NB_DUMP=1), plans it with both rules and prints the plan.
// NB-16: test_merge (synthetic aa / aba / bans / chains / order tail) and, per golden frame, the merge planner as dumped (untracked),
// with hazards assumed tracked (upper bound) and with HOIST, each checked by merge_oracle (dependencies, write order, RAW sources).
// NB-07: test_slotmask (a model of the tap's draw reads with the backend's slot masks, nvn_nb_tap.inc nbt_reads / nbt_mask): a bound
// slot the program does not sample no longer makes a false STORE nor a false 'touches-a'; the golden dumps were recorded without masks.
#include "../../../nb/nb_ir.h"
#include "../../../nb/nb_slotmask.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <tuple>
#include <algorithm>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

static nb_frame F, G;
static nb_plan_out O, O2;
static nb_persist P;
static const nb_rules EXACT = { NB_RULES_EXACT, 0, 0 }, FRESH = { NB_RULES_EXACT, NB_RULEF_FRESH, 0 }, IPAD = { NB_RULES_IPAD, 0, 0 };
static const uint32_t BLACK[4] = { 0, 0, 0, 0x3f800000u };
enum : uint64_t { A = 0x1000, B = 0x2000, D = 0x3000, T = 0x4000, H = 0x5000, R = 0x6000, Z = 0x7000, E = 0x8000, S = 0x9000 };

static void pass(nb_frame *f, uint64_t c0, uint64_t ds) {
    nb_att a[2]; int n = 0;
    if (c0) { a[n].image = c0; a[n].slot = 0; n++; }
    if (ds) { a[n].image = ds; a[n].slot = NB_SLOT_DS; n++; }
    nb_pass_begin(f, a, n);
}
static void clear_rt(nb_frame *f, uint64_t image, unsigned asp) { nb_clear(f, image, asp, NB_CLEAR_FULL | NB_CLEAR_RT, BLACK, 1.0f, 0); }
static void draws(nb_frame *f, int n) { for (int i = 0; i < n; i++) nb_draw(f, 0); }
static void plan(const nb_rules &r, nb_persist *p = nullptr) { CHECK(nb_plan(&F, p, &r, &O) == 0); }
static std::string dump(const nb_frame *f, const nb_plan_out *o) {
    size_t n = nb_dump_str(f, o, nullptr, 0);
    std::string s(n + 1, '\0');
    nb_dump_str(f, o, &s[0], s.size());
    s.resize(n);
    return s;
}

static void test_clear_only_pass() {
    // SetRenderTargets(A, D) + Clear both, no draw; SetRenderTargets(A, D) again and draw: the clear-only pass is not executed.
    nb_frame_reset(&F, 1);
    pass(&F, A, D); clear_rt(&F, A, NB_ASP_COLOR); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL);
    pass(&F, A, D); draws(&F, 5);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.node[0].flags == NB_NP_CLEAR_ONLY);
    CHECK(O.node[1].flags == NB_NP_EXEC);
    CHECK(O.clr[0].fate == NB_CLR_FOLDED && O.clr[0].into == 1);
    CHECK(O.clr[1].fate == NB_CLR_FOLDED && O.clr[1].into == 1);
    CHECK(O.node[1].load[0] == NB_LOAD_CLEAR && O.node[1].clear[0] == 0);
    CHECK(O.node[1].load[1] == NB_LOAD_CLEAR && O.node[1].clear[1] == 1);
    CHECK(O.node[1].store[0] == NB_STORE);              // colour without a later reader is kept (both rules)
    CHECK(O.node[1].store[1] == NB_STORE_DONT_CARE);    // depth without a reader, not xread
    CHECK(O.st.folded == 2 && O.st.standalone == 0 && O.st.clear_only == 1);

    // Nothing uses A after its clear-only pass: a plain clear at its place.
    nb_frame_reset(&F, 2);
    pass(&F, A, 0); clear_rt(&F, A, NB_ASP_COLOR);
    pass(&F, B, 0); draws(&F, 1);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.clr[0].fate == NB_CLR_STANDALONE);
    CHECK(O.node[1].load[0] == NB_LOAD);                // B: no history, exact rule without FRESH loads

    // A sampled before the next pass on A: the clear must happen there; the later pass on A loads it.
    nb_frame_reset(&F, 3);
    pass(&F, A, 0); clear_rt(&F, A, NB_ASP_COLOR);
    pass(&F, B, 0); draws(&F, 1); nb_read(&F, -1, A);
    pass(&F, A, 0); draws(&F, 1);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.clr[0].fate == NB_CLR_STANDALONE);
    CHECK(O.node[2].load[0] == NB_LOAD);

    // ClearTexture (texture clear) of the bound target closes the pass; the next draw reopens the same set and absorbs the clear.
    nb_frame_reset(&F, 4);
    pass(&F, A, 0); draws(&F, 2);
    nb_clear(&F, A, NB_ASP_COLOR, NB_CLEAR_FULL, BLACK, 0, 0);
    draws(&F, 3);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(F.nnode == 3 && F.node[0].end == NB_END_CLEAR && (F.node[1].kind & 0xff) == NB_OP_CLEAR && F.node[2].kind == NB_NODE_PASS);
    CHECK(F.node[2].att[0].image == A && F.node[2].draws == 3);
    CHECK(O.clr[0].fate == NB_CLR_FOLDED && O.clr[0].into == 2 && O.node[2].load[0] == NB_LOAD_CLEAR);
    CHECK(O.node[0].store[0] == NB_STORE_DONT_CARE);    // colour fully overwritten by the clear before any read

    // A full copy into A after its pending clear: the clear is dead.
    nb_frame_reset(&F, 5);
    nb_clear(&F, A, NB_ASP_COLOR, NB_CLEAR_FULL, BLACK, 0, 0);
    { uint64_t s = B, d = A; nb_op(&F, NB_OP_COPY | NB_OPF_DST_FULL, &s, 1, &d, 1); }
    pass(&F, A, 0); draws(&F, 1);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.clr[0].fate == NB_CLR_DEAD && O.st.dead == 1);
    CHECK(O.node[2].load[0] == NB_LOAD);

    // Partial clear: never folded, executes, keeps old texels.
    nb_frame_reset(&F, 6);
    nb_clear(&F, A, NB_ASP_COLOR, 0, BLACK, 0, 0);
    pass(&F, A, 0); draws(&F, 1);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.clr[0].fate == NB_CLR_STANDALONE && O.node[1].load[0] == NB_LOAD);

    // Empty pass (SetRenderTargets, nothing): skipped.
    nb_frame_reset(&F, 7);
    pass(&F, A, D); pass(&F, B, 0); draws(&F, 1); nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.node[0].flags == NB_NP_EMPTY && O.st.empty == 1 && O.st.exec == 1);
}

static void test_depth_readers() {
    // Scene depth D: cleared, drawn; no reader -> DONT_CARE. Sampled later -> STORE. Copied -> STORE. Loaded by the next pass -> STORE.
    for (int variant = 0; variant < 4; variant++) {
        nb_frame_reset(&F, 10 + variant);
        pass(&F, A, D); clear_rt(&F, A, NB_ASP_COLOR); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL); draws(&F, 10);
        if (variant == 1) { pass(&F, B, 0); draws(&F, 1); nb_read(&F, -1, D); }
        if (variant == 2) { uint64_t s = D, d = T; nb_op(&F, NB_OP_COPY | NB_OPF_DST_FULL, &s, 1, &d, 1); }
        if (variant == 3) { pass(&F, B, D); draws(&F, 1); }
        nb_submit(&F, 1);
        plan(EXACT);
        CHECK(O.node[0].load[1] == NB_LOAD_CLEAR);
        CHECK(O.node[0].store[1] == (variant == 0 ? NB_STORE_DONT_CARE : NB_STORE));
        if (variant == 3) CHECK(O.node[1].load[1] == NB_LOAD && O.node[1].store[1] == NB_STORE_DONT_CARE);
    }
    // Depth fully overwritten (cleared again) by the next pass before any reader: DONT_CARE in both rules.
    nb_frame_reset(&F, 14);
    pass(&F, A, D); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL); draws(&F, 2);
    pass(&F, B, D); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL); draws(&F, 2); nb_read(&F, -1, A);
    nb_submit(&F, 1);
    plan(EXACT); CHECK(O.node[0].store[1] == NB_STORE_DONT_CARE);
    plan(IPAD);  CHECK(O.node[0].store[1] == NB_STORE_DONT_CARE);
    // Depth-only clear of a depth+stencil image is not full: the pass loads (no CLEAR/LOAD mix between aspects, FN-16/17).
    nb_frame_reset(&F, 15);
    pass(&F, A, D); nb_clear(&F, D, NB_ASP_DEPTH, NB_CLEAR_RT, BLACK, 1.0f, 0); draws(&F, 2);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.node[0].load[1] == NB_LOAD && O.clr[0].fate == NB_CLR_INPASS);
}

static void test_cross_frame_reader() {
    // H: written in frame N, sampled at the start of frame N+1 (history). Frame 20 drops it (nothing known yet), frame 21 learns
    // (read before the first write), from then on it is stored.
    nb_persist_reset(&P);
    nb_frame_reset(&F, 20);
    pass(&F, A, H); clear_rt(&F, H, NB_ASP_DEPTH); draws(&F, 3);
    nb_submit(&F, 1);
    plan(EXACT, &P);
    CHECK(O.node[0].store[1] == NB_STORE_DONT_CARE);
    CHECK(nb_persist_find(&P, H) && !nb_persist_find(&P, H)->xread);
    nb_frame_reset(&F, 21);
    pass(&F, B, 0); draws(&F, 1); nb_read(&F, -1, H);
    pass(&F, A, H); clear_rt(&F, H, NB_ASP_DEPTH); draws(&F, 3);
    nb_submit(&F, 1);
    plan(EXACT, &P);
    CHECK(O.st.xread_new == 2 && nb_persist_find(&P, H)->xread);   // H sampled first; B loaded before its first write
    CHECK(O.node[1].store[1] == NB_STORE);
    nb_frame_reset(&F, 22);
    pass(&F, A, H); clear_rt(&F, H, NB_ASP_DEPTH); draws(&F, 3);
    nb_submit(&F, 1);
    plan(EXACT, &P);
    CHECK(O.node[0].store[1] == NB_STORE);
    // Re-created image: history forgotten.
    nb_persist_forget(&P, H);
    CHECK(!nb_persist_find(&P, H)->xread);

    // Depth reused without a clear in the next frame (loaded before written): learned as xread on the load.
    nb_persist_reset(&P);
    nb_frame_reset(&F, 30);
    pass(&F, A, Z); draws(&F, 1);
    nb_submit(&F, 1);
    plan(EXACT, &P);
    CHECK(O.node[0].load[1] == NB_LOAD && O.node[0].store[1] == NB_STORE && nb_persist_find(&P, Z)->xread);

    // FRESH (plan's original exact rule): not written in this frame and not xread -> DONT_CARE; xread -> LOAD.
    nb_persist_reset(&P);
    nb_frame_reset(&F, 31);
    pass(&F, A, 0); draws(&F, 1);
    nb_submit(&F, 1);
    plan(FRESH, &P);
    CHECK(O.node[0].load[0] == NB_LOAD_DONT_CARE);
    nb_frame_reset(&F, 32);
    pass(&F, B, 0); draws(&F, 1); nb_read(&F, -1, A);
    pass(&F, A, 0); draws(&F, 1);
    nb_submit(&F, 1);
    plan(FRESH, &P);
    CHECK(nb_persist_find(&P, A)->xread && O.node[1].load[0] == NB_LOAD);
    // Proven cover of the first draw: DONT_CARE without FRESH; a feedback read forbids it.
    nb_frame_reset(&F, 33);
    pass(&F, A, D); nb_draw(&F, 1u << 0); draws(&F, 2);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.node[0].load[0] == NB_LOAD_DONT_CARE && O.node[0].load[1] == NB_LOAD);
    nb_frame_reset(&F, 34);
    pass(&F, A, 0); nb_draw(&F, 1u << 0); nb_read(&F, -1, A);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.node[0].load[0] == NB_LOAD && (O.node[0].flags & NB_NP_FEEDBACK));
    CHECK(O.node[0].cover_dc == 0);                     // NB-05: feedback - no DONT_CARE even with a coverage proof
    // NB-05 cover_dc: colour LOADs that a coverage proof of the BE would turn into DONT_CARE; never depth, a clear or a proven cover.
    nb_frame_reset(&F, 35);
    pass(&F, A, D); draws(&F, 2);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.node[0].load[0] == NB_LOAD && O.node[0].load[1] == NB_LOAD && O.node[0].cover_dc == 1u);
    plan(IPAD);
    CHECK(O.node[0].cover_dc == 0);
}

static void test_aba() {
    // G-buffer G, light L reads G, G again: no merge (NB-16), A2 loads, A1 stores.
    const uint64_t Gb = 0xa000, L = 0xb000;
    nb_frame_reset(&F, 40);
    pass(&F, Gb, D); clear_rt(&F, Gb, NB_ASP_COLOR); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL); draws(&F, 50);
    pass(&F, L, 0); draws(&F, 4); nb_read(&F, -1, Gb); nb_read(&F, -1, D);
    pass(&F, Gb, D); draws(&F, 20);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.st.exec == 3 && O.st.passes == 3);
    CHECK(O.node[0].store[0] == NB_STORE && O.node[0].store[1] == NB_STORE);
    CHECK(O.node[2].load[0] == NB_LOAD && O.node[2].load[1] == NB_LOAD);
    CHECK(O.node[2].store[1] == NB_STORE_DONT_CARE);
    CHECK(O.node[1].load[0] == NB_LOAD);                // L: no history
    plan(IPAD);
    CHECK(O.node[1].load[0] == NB_LOAD_DONT_CARE);      // iPad: L not written in this frame
    CHECK(O.node[2].load[0] == NB_LOAD && O.node[2].store[0] == NB_STORE);
}

static void test_mid_submit() {
    // ReadTexels in the middle of frame 50: submission 1 ends with a readback; submission 2 continues on R and Z.
    nb_persist_reset(&P);
    nb_frame_reset(&F, 50);
    pass(&F, R, Z); clear_rt(&F, R, NB_ASP_COLOR); clear_rt(&F, Z, NB_ASP_DEPTH | NB_ASP_STENCIL); draws(&F, 8);
    { uint64_t s = R; nb_op(&F, NB_OP_READBACK, &s, 1, nullptr, 0); }
    nb_submit(&F, 0);
    plan(EXACT, &P);
    CHECK(F.node[0].end == NB_END_OP);
    CHECK(O.node[0].store[0] == NB_STORE);
    CHECK(O.node[0].store[1] == NB_STORE);              // readers after a mid-frame submission are unknown: keep
    nb_frame_reset(&F, 50);
    pass(&F, R, Z); draws(&F, 4);
    { uint64_t s = R; nb_op(&F, NB_OP_PRESENT, &s, 1, nullptr, 0); }
    nb_submit(&F, 1);
    plan(EXACT, &P);
    CHECK(O.node[0].load[0] == NB_LOAD && O.node[0].load[1] == NB_LOAD);   // written by submission 1 of this frame
    CHECK(O.node[0].store[0] == NB_STORE && O.node[0].store[1] == NB_STORE_DONT_CARE);
    plan(IPAD, &P);
    CHECK(O.node[0].load[0] == NB_LOAD && O.node[0].load[1] == NB_LOAD);
    // Next frame, iPad contract: not written yet in frame 51 -> DONT_CARE.
    nb_frame_reset(&F, 51);
    pass(&F, R, Z); draws(&F, 4);
    nb_submit(&F, 1);
    plan(IPAD, &P);
    CHECK(O.node[0].load[0] == NB_LOAD_DONT_CARE && O.node[0].load[1] == NB_LOAD_DONT_CARE);
}

static void test_colour_overwrite() {
    // T drawn, then cleared and drawn again before any reader: exact drops the first store, iPad always stores colour.
    nb_frame_reset(&F, 60);
    pass(&F, T, 0); draws(&F, 2);
    pass(&F, T, 0); clear_rt(&F, T, NB_ASP_COLOR); draws(&F, 2);
    nb_submit(&F, 1);
    plan(EXACT); CHECK(O.node[0].store[0] == NB_STORE_DONT_CARE && O.node[1].load[0] == NB_LOAD_CLEAR);
    plan(IPAD);  CHECK(O.node[0].store[0] == NB_STORE);
}

static void test_transient() {
    // E: depth cleared every frame, never sampled or copied -> candidate after 8 frames; S: same but sampled once -> never.
    nb_persist_reset(&P);
    for (int fr = 0; fr < 10; fr++) {
        nb_frame_reset(&F, 100 + fr);
        pass(&F, A, E); clear_rt(&F, A, NB_ASP_COLOR); clear_rt(&F, E, NB_ASP_DEPTH | NB_ASP_STENCIL); draws(&F, 3);
        pass(&F, B, S); clear_rt(&F, S, NB_ASP_DEPTH | NB_ASP_STENCIL); draws(&F, 3);
        if (fr == 2) { pass(&F, T, 0); draws(&F, 1); nb_read(&F, -1, S); }
        { uint64_t s = A; nb_op(&F, NB_OP_PRESENT, &s, 1, nullptr, 0); }
        nb_submit(&F, 1);
        plan(EXACT, &P);
        int hasE = 0, hasS = 0;
        for (int i = 0; i < O.ntransient; i++) { hasE |= O.transient[i] == E; hasS |= O.transient[i] == S; }
        CHECK(hasE == (fr >= 7));
        CHECK(!hasS);
    }
    plan(IPAD, &P);
    CHECK(O.ntransient == 0);
}

static void test_dump_roundtrip() {
    nb_frame_reset(&F, 77);
    pass(&F, A, D); clear_rt(&F, A, NB_ASP_COLOR); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL);
    pass(&F, A, D); nb_draw(&F, 1); draws(&F, 3); nb_read(&F, -1, T); nb_read(&F, -1, H);
    nb_clear(&F, A, NB_ASP_COLOR, NB_CLEAR_RT, BLACK, 0, 0);            // in-pass clear after draws
    { uint64_t s[2] = { A, D }, d = B; nb_op(&F, NB_OP_BLIT, s, 2, &d, 1); }
    draws(&F, 2);                                                      // reopened A, D
    nb_clear(&F, B, NB_ASP_COLOR, NB_CLEAR_FULL, BLACK, 0.5f, 7);
    { uint64_t d = S; nb_op(&F, NB_OP_COMPUTE, nullptr, 0, &d, 1); }
    nb_op(&F, NB_OP_BARRIER, nullptr, 0, nullptr, 0);
    pass(&F, B, 0); draws(&F, 1); nb_read(&F, -1, S);
    { uint64_t s = B; nb_op(&F, NB_OP_PRESENT, &s, 1, nullptr, 0); }
    nb_submit(&F, 1);
    plan(EXACT);
    std::string a = dump(&F, nullptr), ap = dump(&F, &O);
    CHECK(nb_parse(ap.c_str(), &G) == 0);                // plan comments are ignored
    std::string b = dump(&G, nullptr);
    CHECK(a == b);
    if (a != b) printf("--- original\n%s--- parsed\n%s", a.c_str(), b.c_str());
    CHECK(nb_plan(&G, nullptr, &EXACT, &O2) == 0);
    CHECK(dump(&G, &O2) == ap);
    CHECK(F.nnode == G.nnode && F.nref == G.nref && F.nclear == G.nclear);
    CHECK(nb_parse("nb frame 1\npass c0=0x10\nbogus\n", &G) == 3);
    CHECK(nb_parse("pass c0=0x10\n", &G) == 1);
    // nb_dump to a FILE: same text.
    FILE *tf = tmpfile();
    if (tf) {
        nb_dump(&F, &O, tf); long n = ftell(tf); rewind(tf);
        std::string t((size_t)n, '\0'); size_t got = fread(&t[0], 1, (size_t)n, tf); fclose(tf);
        t.resize(got);
        CHECK(t == ap);
    }
    // Truncated buffer: still NUL-terminated, returns the full length.
    char small[16]; size_t need = nb_dump_str(&F, nullptr, small, sizeof small);
    CHECK(need == a.size() && strlen(small) == sizeof small - 1);
    printf("%s", ap.c_str());
}

static void test_limits() {
    nb_frame_reset(&F, 90);
    for (int i = 0; i < NB_MAX_NODES + 10; i++) { pass(&F, A + (uint64_t)i, 0); nb_draw(&F, 0); }
    CHECK(F.nnode == NB_MAX_NODES && F.overflow > 0);
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.st.exec == NB_MAX_NODES);
    nb_frame_reset(&F, 91);
    nb_draw(&F, 0);                                      // no RT set yet
    CHECK(F.stray == 1 && F.nnode == 0);
    CHECK(nb_read(&F, 3, A) < 0);
}

// 16.11 (ab7rp3, DOS2_PORT.md "store-hist"): a submission that lost records must not discard any store. The scene depth D is drawn in
// pass 0; its reader (the lighting) comes after the node table is full, so the IR never sees it: before the fix D got DONT_CARE.
static void test_lost_records() {
    for (int variant = 0; variant < 3; variant++) {
        nb_frame_reset(&F, 95 + variant);
        pass(&F, A, D); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL); draws(&F, 4);
        if (variant == 1) for (int i = 0; F.nnode < NB_MAX_NODES; i++) { pass(&F, T + (uint64_t)i, 0); nb_draw(&F, 0); }
        if (variant == 1) { pass(&F, B, 0); nb_draw(&F, 0); nb_read(&F, -1, D); }   // the reader past the cap: lost (overflow)
        if (variant == 2) { nb_pass_end(&F, NB_END_OP); nb_read(&F, -1, D); }           // a read with no pass open: lost (stray)
        nb_submit(&F, 1);
        plan(EXACT);
        if (variant == 0) CHECK(O.node[0].store[1] == NB_STORE_DONT_CARE && O.st.store_lost == 0);   // complete IR: nothing reads D
        else CHECK((variant == 1 ? F.overflow > 0 : F.stray > 0) && O.node[0].store[1] == NB_STORE && O.st.store_lost >= 1);
        if (variant) CHECK(dump(&F, &O).find("kept as STORE (lost records)") != std::string::npos);
        int dc = 0; for (int i = 0; i < F.nnode; i++) for (int k = 0; k < F.node[i].natt; k++) dc += O.node[i].store[k] == NB_STORE_DONT_CARE;
        CHECK(variant == 0 || dc == 0);
    }
}

// NB-05b: barriers inside a pass. A barrier with the pass open is kept (nbar, no node); a later read of an attachment of the pass ends it
// at the barrier (op node between the two passes); a read of anything else does not; a barrier with no pass open is an op node.
static void test_barrier_kept() {
    nb_frame_reset(&F, 100);
    pass(&F, A, D); clear_rt(&F, A, NB_ASP_COLOR); draws(&F, 3);
    CHECK(nb_barrier(&F) == 0 && F.open && F.node[0].nbar == 1 && F.bar_pending);
    CHECK(nb_barrier_hazard(&F, T) == 0 && F.open);      // reads T (not an attachment): the pass goes on
    nb_read(&F, -1, T); draws(&F, 2);
    CHECK(F.nnode == 1 && F.node[0].draws == 5);
    CHECK(nb_barrier_hazard(&F, A) == 1 && !F.open);     // reads its own colour after the barrier: ends at the barrier
    CHECK(F.nnode == 2 && F.node[0].end == NB_END_BARRIER && (F.node[1].kind & 0xff) == NB_OP_BARRIER && F.bar_split == 1);
    nb_draw(&F, 0); nb_read(&F, -1, A);                 // the draw reopens A+D
    CHECK(F.nnode == 3 && F.node[2].kind == NB_NODE_PASS && F.node[2].natt == 2 && !F.bar_pending);
    CHECK(nb_barrier_hazard(&F, A) == 0);                // no barrier since: feedback stays in the pass
    pass(&F, B, 0); CHECK(!F.bar_pending);
    const uint64_t b = B, h = H;
    nb_op(&F, NB_OP_COPY, &b, 1, &h, 1);
    CHECK(nb_barrier(&F) == 5 && (F.node[5].kind & 0xff) == NB_OP_BARRIER);   // no pass open: an op node as before
    nb_submit(&F, 1);
    plan(EXACT);
    CHECK(O.node[0].store[0] == NB_STORE && O.node[2].load[0] == NB_LOAD);
    std::string a = dump(&F, nullptr);
    CHECK(a.find("barriers 1\n") != std::string::npos);
    CHECK(nb_parse(a.c_str(), &G) == 0 && dump(&G, nullptr) == a && G.node[0].nbar == 1);
}

// NB-05c: NB_NP_PREBAR. The P2 "split" class (DOS2 ping-pong blur, heavy scene nodes 66 / 68 / 70 / 80 / 82 / 84): pass X writes P,
// an empty pass, pass Y (c0 = Q) draws twice and samples P from its 2nd draw. The host begins Y at draw 1 and ends it at draw 2's read
// (prepare_read why 2: P written since its last barrier). The IR keeps Y whole and flags it: the barrier goes before Y begins.
static void test_prebar() {
    nb_frame_reset(&F, 200);
    const uint64_t P = A, Q = B;
    pass(&F, P, D); clear_rt(&F, P, NB_ASP_COLOR); draws(&F, 3);          // 0: writes P (+ D)
    pass(&F, R, D);                                                      // 1: empty
    pass(&F, Q, D); draws(&F, 2); nb_barrier(&F); nb_read(&F, -1, P);    // 2: reads P (written by 0)
    pass(&F, R, D);                                                      // 3: empty
    pass(&F, P, D); draws(&F, 2); nb_read(&F, -1, Q);                    // 4: reads Q (written by 2, after the barrier point of 2)
    pass(&F, Z, 0); draws(&F, 1); nb_read(&F, -1, T); nb_read(&F, -1, Z);   // 5: T never written, Z its own attachment (feedback)
    pass(&F, E, 0); draws(&F, 1); nb_read(&F, -1, D);                    // 6: D written by 4 (after 4's barrier point)
    pass(&F, H, 0); draws(&F, 1); nb_read(&F, -1, Q);                    // 7: Q not written since the barrier point of 6
    const uint64_t s = H, d = S;
    nb_op(&F, NB_OP_COMPUTE, &s, 1, &d, 1);                              // 8: writes S
    pass(&F, Z, 0); draws(&F, 1); nb_read(&F, -1, S);                    // 9: reads the compute output
    nb_submit(&F, 1);
    plan(EXACT);
    const bool want[] = { false, false, true, false, true, false, true, false, false, true };
    for (int i = 0; i < 10; i++) CHECK(((O.node[i].flags & NB_NP_PREBAR) != 0) == want[i]);
    CHECK(O.st.prebar == 4 && (O.node[2].flags & NB_NP_EXEC) && F.node[2].draws == 2 && F.node[2].nbar == 1 && F.nnode == 10);
    const std::string t = dump(&F, &O);
    CHECK(t.find("# prebar 2\n") != std::string::npos && t.find("# prebar 9\n") != std::string::npos && t.find("prebar=4\n") != std::string::npos);
    CHECK(nb_parse(t.c_str(), &G) == 0 && dump(&G, nullptr) == dump(&F, nullptr));   // "# prebar" lines are plan comments
}

// NB-05c golden oracle, independent of the planner's bookkeeping: an exec pass that samples (not as its own attachment) an image written in
// this submission by an earlier non-empty pass (attachment) or op (dst / standalone clear, taken at its own node) after the last flagged pass must carry NB_NP_PREBAR, and no
// other pass may. Returns the passes that break it; *cand = flagged passes with 2+ draws (where the host splits if the read is not in draw 1).
static int prebar_oracle(const nb_frame *f, const nb_plan_out *o, int *flagged, int *cand) {
    std::vector<std::pair<uint64_t, int>> wr;   // image, node of its last write
    int last_bar = -1, bad = 0; *flagged = 0; *cand = 0;
    for (int i = 0; i < f->nnode; i++) {
        const nb_node *n = &f->node[i]; const nb_ref *refs = &f->ref[n->ref_first];
        auto written_after = [&](uint64_t im) { for (auto &w : wr) if (w.first == im && w.second > last_bar) return true; return false; };
        auto note = [&](uint64_t im) { for (auto &w : wr) if (w.first == im) { w.second = i; return; } wr.push_back({ im, i }); };
        if (n->kind == NB_NODE_PASS) {
            if (!n->draws) { for (int c = 0; c < n->nclear; c++) if (o->clr[n->clear_first + c].fate == NB_CLR_STANDALONE) note(f->clr[n->clear_first + c].image); continue; }
            bool need = false;
            for (int r = 0; r < n->nref; r++) {
                bool own = false; for (int k = 0; k < n->natt; k++) own |= n->att[k].image == refs[r].image;
                if (own) continue;
                need |= written_after(refs[r].image);
            }
            const bool has = (o->node[i].flags & NB_NP_PREBAR) != 0;
            if (has != need) { bad++; if (getenv("NB_GOLDEN_VERBOSE")) printf("# prebar oracle: node %d plan %d oracle %d\n", i, has, need); }
            if (has) { (*flagged)++; if (n->draws >= 2) (*cand)++; }
            for (int k = 0; k < n->natt; k++) note(n->att[k].image);
            if (has) last_bar = i - 1;   // the barrier sits before this pass: its own writes come after it
        } else if ((n->kind & 0xff) == NB_OP_CLEAR) {
            if (o->clr[n->clear_first].fate == NB_CLR_STANDALONE) note(f->clr[n->clear_first].image);
        } else {
            for (int r = 0; r < n->nref; r++) if (refs[r].role == NB_REF_DST) note(refs[r].image);
        }
    }
    return bad;
}

// Golden replay with the NB-05b barrier rule: the P2 dumps were taken with "a barrier ends the pass". Rebuild the frame through the
// builder: a pass ended at a barrier stays open over the barrier ops after it, and the next pass with the same attachment set continues
// it unless it reads one of its attachments (then nb_barrier_hazard splits it there). Upper bound: the dump cannot tell a draw that
// reopened the set from an explicit SetRenderTargets of the same set (the tap keeps the latter a new pass).
static void replay_keep_barriers(const nb_frame *s, nb_frame *d) {
    nb_frame_reset(d, s->frame_no);
    for (int i = 0; i < s->nnode; i++) {
        const nb_node *n = &s->node[i];
        const nb_ref *refs = &s->ref[n->ref_first];
        if (n->kind == NB_NODE_PASS) {
            bool cont = d->open && d->bar_pending && d->node[d->nnode - 1].natt == n->natt;
            for (int k = 0; cont && k < n->natt; k++) cont = d->node[d->nnode - 1].att[k].image == n->att[k].image && d->node[d->nnode - 1].att[k].slot == n->att[k].slot;
            if (cont) for (int r = 0; r < n->nref && d->open; r++) nb_barrier_hazard(d, refs[r].image);
            if (!cont || !d->open) nb_pass_begin(d, n->att, n->natt);
            for (int c = 0; c < n->nclear; c++) { const nb_clear_rec *x = &s->clr[n->clear_first + c]; if (!x->after_draw) nb_clear(d, x->image, x->aspects, x->flags, x->color, x->depth, x->stencil); }
            for (uint32_t k = 0; k < n->draws; k++) nb_draw(d, k ? 0 : n->cover);
            for (int c = 0; c < n->nclear; c++) { const nb_clear_rec *x = &s->clr[n->clear_first + c]; if (x->after_draw) nb_clear(d, x->image, x->aspects, x->flags, x->color, x->depth, x->stencil); }
            for (int r = 0; r < n->nref; r++) nb_read(d, -1, refs[r].image);
            for (int b = 0; b < n->nbar; b++) nb_barrier(d);
            if (n->end != NB_END_BARRIER && n->end != NB_END_OPEN) nb_pass_end(d, n->end);
        } else if ((n->kind & 0xff) == NB_OP_BARRIER) {
            nb_barrier(d);
        } else if ((n->kind & 0xff) == NB_OP_CLEAR) {
            const nb_clear_rec *x = &s->clr[n->clear_first]; nb_clear(d, x->image, x->aspects, x->flags, x->color, x->depth, x->stencil);
        } else {
            uint64_t src[64], dst[64]; int ns = 0, nd = 0;
            for (int r = 0; r < n->nref; r++) { if (refs[r].role == NB_REF_SRC && ns < 64) src[ns++] = refs[r].image; else if (refs[r].role == NB_REF_DST && nd < 64) dst[nd++] = refs[r].image; }
            nb_op(d, n->kind, src, ns, dst, nd);
        }
    }
    nb_submit(d, s->final);
}

/* ---------------- NB-16: merge planner ---------------- */
static nb_merge_out M;
static const nb_merge_rules MERGE_ON = { NB_MERGE_ON, 0 }, MERGE_OFF = { NB_MERGE_OFF, 0 }, MERGE_HOIST = { NB_MERGE_ON, NB_MERGEF_HOIST };

// Independent oracle of the merged order (own access model, written apart from nb_plan.c): a permutation; no two dependent nodes
// swapped (RAW / WAR / WAW on an image, or a fence on either side); every image's write sequence and every read's last writer the same
// as in submission order; a continuation sits right after the previous pass of its unit, same set, no hazards / feedback / CLEAR load;
// identity when off; PREBAR of the identity order = nb_plan's. Returns the number of violations (printed with NB_GOLDEN_VERBOSE).
static int merge_oracle(const nb_frame *f, const nb_plan_out *o, const nb_merge_out *m, int mode) {
    const int n = f->nnode; int bad = 0;
    auto fail = [&](const char *what, int a, int b) { bad++; if (getenv("NB_GOLDEN_VERBOSE")) printf("# merge oracle: %s (%d, %d)\n", what, a, b); };
    if (m->nnode != n) { fail("nnode", m->nnode, n); return bad; }
    std::vector<int> pos(n, -1);
    for (int x = 0; x < n; x++) { int i = m->order[x]; if (i < 0 || i >= n || pos[i] >= 0) { fail("not a permutation", x, i); return bad; } pos[i] = x; }
    for (int i = 0; i < n; i++) if (m->pos[i] != pos[i]) fail("pos", i, m->pos[i]);
    std::vector<std::vector<uint64_t>> W(n), R(n); std::vector<int> fence(n, 0);
    for (int i = 0; i < n; i++) {
        const nb_node *d = &f->node[i]; const nb_ref *refs = &f->ref[d->ref_first]; const int k = d->kind & 0xff;
        if (k == NB_NODE_PASS) {
            if (d->hz) { fence[i] = 1; continue; }
            if (o->node[i].flags & NB_NP_EXEC) { for (int a = 0; a < d->natt; a++) { W[i].push_back(d->att[a].image); R[i].push_back(d->att[a].image); }
                                                 for (int r = 0; r < d->nref; r++) R[i].push_back(refs[r].image); }
            else for (int c = 0; c < d->nclear; c++) W[i].push_back(f->clr[d->clear_first + c].image);
        } else if (k == NB_OP_CLEAR) W[i].push_back(f->clr[d->clear_first].image);
        else if (k == NB_OP_BARRIER) {}
        else if ((k == NB_OP_COPY || k == NB_OP_BLIT || k == NB_OP_RESOLVE) && d->nref)
            for (int r = 0; r < d->nref; r++) (refs[r].role == NB_REF_DST ? W[i] : R[i]).push_back(refs[r].image);
        else fence[i] = 1;
    }
    auto has = [](const std::vector<uint64_t> &v, uint64_t x) { for (auto y : v) if (y == x) return true; return false; };
    for (int x = 0; x < n; x++) for (int y = x + 1; y < n; y++) {
        bool dep = fence[x] || fence[y];
        for (auto im : W[x]) dep = dep || has(W[y], im) || has(R[y], im);
        for (auto im : R[x]) dep = dep || has(W[y], im);
        if (dep && pos[x] > pos[y]) fail("dependency reordered", x, y);
    }
    // Per image: write sequence and last writer of each read, submission order vs merged order.
    auto replay = [&](bool merged, std::vector<std::pair<uint64_t, int>> &writes, std::vector<std::tuple<int, uint64_t, int>> &rd) {
        std::vector<std::pair<uint64_t, int>> last;
        for (int x = 0; x < n; x++) {
            const int i = merged ? m->order[x] : x;
            for (auto im : R[i]) { int w = -1; for (auto &l : last) if (l.first == im) w = l.second; rd.push_back(std::make_tuple(i, im, w)); }
            for (auto im : W[i]) { writes.push_back({ im, i }); bool hit = false; for (auto &l : last) if (l.first == im) { l.second = i; hit = true; } if (!hit) last.push_back({ im, i }); }
        }
    };
    std::vector<std::pair<uint64_t, int>> w0, w1; std::vector<std::tuple<int, uint64_t, int>> r0, r1;
    replay(false, w0, r0); replay(true, w1, r1);
    auto seq = [](std::vector<std::pair<uint64_t, int>> v, uint64_t im) { std::vector<int> s; for (auto &e : v) if (e.first == im) s.push_back(e.second); return s; };
    for (auto &e : w0) if (seq(w0, e.first) != seq(w1, e.first)) { fail("image write order", e.second, 0); break; }
    std::sort(r0.begin(), r0.end()); std::sort(r1.begin(), r1.end());
    if (r0 != r1) fail("read-after-write source", (int)r0.size(), (int)r1.size());
    int conts = 0;
    for (int j = 0; j < n; j++) {
        const int i = m->cont[j];
        if (i < 0) { if (m->head[j] != j) fail("head without cont", j, m->head[j]); continue; }
        conts++;
        const nb_node *a = &f->node[i], *b = &f->node[j];
        if (pos[j] != pos[i] + 1) fail("continuation not adjacent", i, j);
        if (a->kind != NB_NODE_PASS || b->kind != NB_NODE_PASS || !(o->node[i].flags & NB_NP_EXEC) || !(o->node[j].flags & NB_NP_EXEC)) fail("cont of a non-pass", i, j);
        if (a->hz || b->hz) fail("cont with a hazard", i, j);
        if ((o->node[i].flags | o->node[j].flags) & NB_NP_FEEDBACK) fail("cont with feedback", i, j);
        bool same = a->natt == b->natt;
        for (int k = 0; same && k < b->natt; k++) { bool hit = false; for (int q = 0; q < a->natt; q++) hit |= a->att[q].image == b->att[k].image && a->att[q].slot == b->att[k].slot; same = hit; }
        if (!same) fail("cont of another set", i, j);
        for (int k = 0; k < b->natt; k++) if (o->node[j].load[k] == NB_LOAD_CLEAR) fail("cont with a CLEAR load", i, j);
        if (m->head[j] != m->head[i]) fail("head", i, j);
        if (m->prebar[j]) fail("prebar on a continuation", i, j);
    }
    if (conts != m->st.merged) fail("merged count", conts, m->st.merged);
    if (mode != NB_MERGE_ON) {
        for (int x = 0; x < n; x++) if (m->order[x] != x) { fail("off: not identity", x, m->order[x]); break; }
        if (conts) fail("off: links", conts, 0);
        for (int i = 0; i < n; i++) if (f->node[i].kind == NB_NODE_PASS && (o->node[i].flags & NB_NP_EXEC) && !!m->prebar[i] != !!(o->node[i].flags & NB_NP_PREBAR))
            fail("off: prebar differs from the plan", i, m->prebar[i]);
    }
    return bad;
}
static void merge(const nb_merge_rules &r) { CHECK(nb_merge(&F, &O, &r, &M) == 0); CHECK(merge_oracle(&F, &O, &M, r.mode) == 0); }
static std::vector<int> order_of() { return std::vector<int>(M.order, M.order + M.nnode); }

static void test_merge() {
    const uint64_t Gb = 0xa000, L = 0xb000, X = 0xc000, Y = 0xd000;
    // aa: A, unrelated copy X -> Y, barrier, A again: one unit, the copy and the barrier go before it.
    nb_frame_reset(&F, 60); nb_track(&F, NB_TRACK_MERGE);
    pass(&F, A, D); clear_rt(&F, A, NB_ASP_COLOR); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL); draws(&F, 5);
    { uint64_t s = X, d = Y; nb_op(&F, NB_OP_COPY | NB_OPF_DST_FULL, &s, 1, &d, 1); }
    nb_barrier(&F);
    pass(&F, A, D); draws(&F, 3);
    nb_submit(&F, 1);
    plan(EXACT);
    merge(MERGE_ON);
    CHECK(M.st.cand_aa == 1 && M.st.aa == 1 && M.st.merged == 1 && M.cont[3] == 0 && M.head[3] == 0);
    CHECK((order_of() == std::vector<int>{ 1, 2, 0, 3 }));
    CHECK(M.rec[0].kind == NB_MK_AA && M.rec[0].why == NB_MB_MERGED && M.rec[0].before == 2 && M.rec[0].after == 0);
    merge(MERGE_OFF);                                   // identity, analysis kept
    CHECK(M.st.aa == 1 && M.st.merged == 0 && (order_of() == std::vector<int>{ 0, 1, 2, 3 }) && M.cont[3] == -1);
    { // order tail: pack / parse, identity and merged
        unsigned char buf[2048]; uint16_t e[NB_MAX_NODES]; uint32_t off[4] = { 0, 640, 700, 760 }, off2[4]; nb_merge_tail_hdr h;
        size_t nbytes = nb_merge_tail_pack(&M, 77, nullptr, buf, sizeof buf);
        CHECK(nbytes == nb_merge_tail_size(4, 0) && nbytes == 12 + 8);
        CHECK(nb_merge_tail_parse(buf, nbytes, e, NB_MAX_NODES, nullptr, &h) == 4 && h.seq == 77 && h.ncont == 0 && e[0] == 0 && e[3] == 3);
        merge(MERGE_ON);
        nbytes = nb_merge_tail_pack(&M, 78, off, buf, sizeof buf);
        CHECK(nbytes == nb_merge_tail_size(4, 1) && nbytes == 12 + 8 + 16);
        CHECK(nb_merge_tail_parse(buf, nbytes, e, NB_MAX_NODES, off2, &h) == 4 && h.ncont == 1 && (h.flags & NB_MTF_MERGED) && (h.flags & NB_MTF_OFFSETS));
        CHECK(e[0] == 1 && e[1] == 2 && e[2] == 0 && e[3] == (3 | NB_MT_CONT) && off2[1] == 640);
        CHECK(nb_merge_tail_pack(&M, 78, off, buf, 10) == 0);
        buf[0] = 2; CHECK(nb_merge_tail_parse(buf, nbytes, e, NB_MAX_NODES, nullptr, nullptr) < 0); buf[0] = 1;
        uint16_t dup = 1; memcpy(buf + 12, &dup, 2); memcpy(buf + 14, &dup, 2); CHECK(nb_merge_tail_parse(buf, nbytes, e, NB_MAX_NODES, nullptr, nullptr) < 0);
    }
    // Not tracked (every P2 dump): blocked, unless the analysis assumes tracking.
    F.track = 0; plan(EXACT); merge(MERGE_ON);
    CHECK(M.st.blocked[NB_MB_UNTRACKED] == 1 && M.st.merged == 0);
    { nb_merge_rules r = { NB_MERGE_ON, NB_MERGEF_ASSUME_TRACKED }; merge(r); CHECK(M.st.merged == 1); }
    { nb_merge_rules r = { NB_MERGE_ON, NB_MERGEF_ABA | NB_MERGEF_ASSUME_TRACKED }; merge(r); CHECK(M.st.blocked[NB_MB_KIND_OFF] == 1); }

    // aa blocked: the op between reads A's attachment (it would have to be both after A1 and before A2).
    nb_frame_reset(&F, 61); nb_track(&F, NB_TRACK_MERGE);
    pass(&F, A, D); draws(&F, 5);
    { uint64_t s = A, d = Y; nb_op(&F, NB_OP_COPY | NB_OPF_DST_FULL, &s, 1, &d, 1); }
    pass(&F, A, D); draws(&F, 3);
    nb_submit(&F, 1); plan(EXACT); merge(MERGE_ON);
    CHECK(M.st.blocked[NB_MB_TOUCHES_A] == 1 && M.rec[0].culprit == 1 && M.st.merged == 0);

    // aba exact: G-buffer, an unrelated pass L (reads T), G again: L goes before, G1 G2 one unit.
    nb_frame_reset(&F, 62); nb_track(&F, NB_TRACK_MERGE);
    pass(&F, Gb, D); clear_rt(&F, Gb, NB_ASP_COLOR); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL); draws(&F, 50);
    pass(&F, L, 0); draws(&F, 4); nb_read(&F, -1, T);
    pass(&F, Gb, D); draws(&F, 20);
    nb_submit(&F, 1); plan(EXACT); merge(MERGE_ON);
    CHECK(M.st.cand_aba == 1 && M.st.exact == 1 && (order_of() == std::vector<int>{ 1, 0, 2 }) && M.cont[2] == 0);
    // ... L samples what G1 never touches but G1 samples X and L writes X: L must stay after the unit (WAR): G1 G2 L.
    nb_frame_reset(&F, 63); nb_track(&F, NB_TRACK_MERGE);
    pass(&F, Gb, D); draws(&F, 50); nb_read(&F, -1, X);
    pass(&F, X, 0); draws(&F, 4);
    pass(&F, Gb, D); draws(&F, 20);
    nb_submit(&F, 1); plan(EXACT); merge(MERGE_ON);
    CHECK(M.st.exact == 1 && (order_of() == std::vector<int>{ 0, 2, 1 }) && M.rec[0].after == 1);
    // ... the test_aba frame: L reads G: blocked.
    nb_frame_reset(&F, 64); nb_track(&F, NB_TRACK_MERGE);
    pass(&F, Gb, D); draws(&F, 50);
    pass(&F, L, 0); draws(&F, 4); nb_read(&F, -1, Gb); nb_read(&F, -1, D);
    pass(&F, Gb, D); draws(&F, 20);
    nb_submit(&F, 1); plan(EXACT); merge(MERGE_ON);
    CHECK(M.st.blocked[NB_MB_TOUCHES_A] == 1 && M.st.merged == 0);
    // ... A2 reads L's output: strict "exact" blocks; HOIST moves L before G1.
    nb_frame_reset(&F, 65); nb_track(&F, NB_TRACK_MERGE);
    pass(&F, Gb, D); draws(&F, 50);
    pass(&F, L, 0); draws(&F, 4); nb_read(&F, -1, T);
    pass(&F, Gb, D); draws(&F, 20); nb_read(&F, -1, L);
    nb_submit(&F, 1); plan(EXACT);
    CHECK(O.node[2].flags & NB_NP_PREBAR);
    merge(MERGE_ON);
    CHECK(M.st.blocked[NB_MB_A2_READS_B] == 1 && M.rec[0].culprit == 1);
    merge(MERGE_HOIST);
    CHECK(M.st.exact == 1 && (order_of() == std::vector<int>{ 1, 0, 2 }) && M.prebar[0] == 1 && M.prebar[2] == 0 && M.st.prebar == 1);
    // ... a compute / buffer copy / fence / hazard pass between: blocked; a hazard on A2; feedback; A2 cleared.
    for (int v = 0; v < 7; v++) {
        nb_frame_reset(&F, 66 + v); nb_track(&F, NB_TRACK_MERGE);
        pass(&F, Gb, D); draws(&F, 50);
        if (v == 5) nb_read(&F, -1, Gb);
        if (v == 0) nb_op(&F, NB_OP_COMPUTE, nullptr, 0, nullptr, 0);
        if (v == 1) nb_op(&F, NB_OP_COPY, nullptr, 0, nullptr, 0);
        if (v == 2) nb_hazard(&F, NB_HZ_COND);       // closes nothing: marks G1
        pass(&F, L, 0); draws(&F, 4); nb_read(&F, -1, T);
        if (v == 3) nb_hazard(&F, NB_HZ_COUNTER);
        nb_pass_end(&F, NB_END_RT);
        if (v == 4) nb_hazard(&F, NB_HZ_UBO);        // between passes: a fence node
        pass(&F, Gb, D);
        if (v == 6) clear_rt(&F, Gb, NB_ASP_COLOR);
        draws(&F, 20);
        nb_submit(&F, 1); plan(EXACT); merge(MERGE_ON);
        const int want[7] = { NB_MB_FENCE, NB_MB_FENCE, NB_MB_COND, NB_MB_COUNTER, NB_MB_UBO, NB_MB_FEEDBACK, NB_MB_A2_CLEAR };
        CHECK(M.nrec == 1 && M.rec[0].why == want[v] && M.st.merged == 0);
        if (v == 4) CHECK((F.node[2].kind & 0xff) == NB_OP_FENCE && F.node[2].hz == NB_HZ_UBO);
    }
    // Chain A B A C A -> B C A A A (one unit of three), and a merged unit stays atomic when a later candidate moves nodes around it.
    nb_frame_reset(&F, 75); nb_track(&F, NB_TRACK_MERGE);
    pass(&F, A, D); draws(&F, 2);
    pass(&F, B, 0); draws(&F, 1);
    pass(&F, A, D); draws(&F, 2);
    pass(&F, S, 0); draws(&F, 1);
    pass(&F, A, D); draws(&F, 2);
    nb_submit(&F, 1); plan(EXACT); merge(MERGE_ON);
    CHECK(M.st.exact == 2 && M.st.merged == 2 && (order_of() == std::vector<int>{ 1, 3, 0, 2, 4 }) && M.cont[2] == 0 && M.cont[4] == 2 && M.head[4] == 0 && M.st.units == 3);
    nb_frame_reset(&F, 76); nb_track(&F, NB_TRACK_MERGE);
    pass(&F, Gb, 0); draws(&F, 2);                   // 0
    pass(&F, B, 0); draws(&F, 1); nb_read(&F, -1, X); // 1: B (reads X)
    pass(&F, B, 0); draws(&F, 1);                    // 2: B again -> unit {1, 2}
    pass(&F, X, 0); draws(&F, 1); nb_read(&F, -1, Y); // 3: writes X: after B (WAR); G1 does not care
    pass(&F, Gb, 0); draws(&F, 2);                   // 4: Gb again: {1,2} goes before, 3 is free -> before
    nb_submit(&F, 1); plan(EXACT); merge(MERGE_ON);
    CHECK(M.st.merged == 2 && M.cont[2] == 1 && M.cont[4] == 0 && (order_of() == std::vector<int>{ 1, 2, 3, 0, 4 }));
    // Clear-only / empty passes of the set are never A1 / A2 (here A2 absorbs the clear-only pass's clear: a2-clear).
    nb_frame_reset(&F, 77); nb_track(&F, NB_TRACK_MERGE);
    pass(&F, A, D); draws(&F, 2);
    pass(&F, A, D);                                  // empty
    pass(&F, A, D); clear_rt(&F, A, NB_ASP_COLOR);  // clear-only (folds into the next)
    pass(&F, A, D); draws(&F, 2);
    nb_submit(&F, 1); plan(EXACT); merge(MERGE_ON);
    CHECK(M.nrec == 1 && M.rec[0].node == 3 && M.rec[0].prev == 0 && M.rec[0].why == NB_MB_A2_CLEAR);
    // Dump / parse of hazards and tracking.
    nb_frame_reset(&F, 78); nb_track(&F, NB_TRACK_HAZARDS);
    pass(&F, A, 0); draws(&F, 1); nb_hazard(&F, NB_HZ_COUNTER | NB_HZ_SSBO);
    nb_pass_end(&F, NB_END_RT); nb_hazard(&F, NB_HZ_CPU); nb_hazard(&F, NB_HZ_COND);
    pass(&F, B, 0); draws(&F, 1);
    nb_submit(&F, 1);
    { const std::string t = dump(&F, nullptr);
      CHECK(t.find("track hazards\n") != std::string::npos && t.find("hazard counter ssbo\n") != std::string::npos && t.find("hazard cond cpu\n") != std::string::npos);
      CHECK(nb_parse(t.c_str(), &G) == 0 && dump(&G, nullptr) == t && G.nnode == 3 && G.node[1].hz == (NB_HZ_CPU | NB_HZ_COND) && G.track == NB_TRACK_HAZARDS); }
    { char b[256]; plan(EXACT); merge(MERGE_ON); nb_merge_summary(&M, b, sizeof b); CHECK(!strncmp(b, "aa=0/0 aba=0 exact=0 blocked()=0", 32)); }
}

// Golden: image sizes from the dump's "# image 0x... WxH fmt=0x.." comments (NVN formats), bytes per texel of the formats DOS2 uses.
struct img_info { uint64_t id; unsigned w, h, fmt; };
static std::vector<img_info> parse_images(const std::string &t) {
    std::vector<img_info> v; size_t a = 0;
    while ((a = t.find("# image 0x", a)) != std::string::npos) { img_info e{}; unsigned long long id = 0;
        if (sscanf(t.c_str() + a, "# image 0x%llx %ux%u fmt=0x%x", &id, &e.w, &e.h, &e.fmt) == 4) { e.id = id; v.push_back(e); } a += 8; }
    return v;
}
static unsigned fmt_bytes(unsigned fmt) {
    switch (fmt) { case 0x1: case 0x3: case 0x31: return 1; case 0x5: case 0x6: case 0xd: case 0x32: return 2;
                   case 0x29: case 0x16: case 0x36: return 8; case 0x2e: return 16; default: return 4; }
}
struct merge_numbers { int full_exact = 0, full_gbuf = 0; double mb = 0; std::string sets; };
static merge_numbers merge_value(const nb_frame *f, const nb_plan_out *o, const nb_merge_out *m, const std::vector<img_info> &imgs) {
    merge_numbers r; double maxa = 0;
    for (auto &e : imgs) maxa = std::max(maxa, double(e.w) * e.h);
    for (int k = 0; k < m->nrec; k++) {
        const nb_merge_rec &rc = m->rec[k];
        if (rc.why != NB_MB_MERGED) continue;
        const nb_node *b = &f->node[rc.node], *a = &f->node[rc.prev];
        bool full = false; int colours = 0; double bytes = 0;
        for (int i = 0; i < b->natt; i++) {
            const img_info *e = nullptr; for (auto &x : imgs) if (x.id == b->att[i].image) e = &x;
            if (b->att[i].slot != NB_SLOT_DS) colours++;
            if (!e) continue;
            full |= double(e->w) * e->h >= 0.5 * maxa;
            int ia = -1; for (int q = 0; q < a->natt; q++) if (a->att[q].image == b->att[i].image) ia = q;
            const double sz = double(e->w) * e->h * fmt_bytes(e->fmt);
            bytes += (ia >= 0 && o->node[rc.prev].store[ia] == NB_STORE ? sz : 0) + (o->node[rc.node].load[i] == NB_LOAD ? sz : 0);
        }
        r.mb += bytes / 1e6;
        if (rc.kind == NB_MK_ABA && full) { r.full_exact++; if (colours >= 2) r.full_gbuf++; }
        char t[96]; snprintf(t, sizeof t, " %s%d<-%d(%datt,%.1fMB)", rc.kind == NB_MK_AA ? "aa:" : "aba:", rc.node, rc.prev, b->natt, bytes / 1e6); r.sets += t;
    }
    return r;
}
static int golden_merge(const char *path, const char *tag, const nb_frame *f, const nb_plan_out *o, const std::vector<img_info> &imgs) {
    static nb_merge_out m;
    const nb_merge_rules plain = { NB_MERGE_ON, 0 }, strict = { NB_MERGE_ON, NB_MERGEF_ASSUME_TRACKED },
                         hoist = { NB_MERGE_ON, NB_MERGEF_ASSUME_TRACKED | NB_MERGEF_HOIST }, off = { NB_MERGE_OFF, NB_MERGEF_ASSUME_TRACKED };
    char line[512];
    int bad = 0;
    CHECK(nb_merge(f, o, &plain, &m) == 0); bad += merge_oracle(f, o, &m, NB_MERGE_ON);
    CHECK(m.st.merged == 0);                           // P2 dumps carry no "track" line: nothing may merge
    nb_merge_summary(&m, line, sizeof line);
    printf("# golden %s NB-16 %s, as dumped (untracked): %s\n", path, tag, line);
    CHECK(nb_merge(f, o, &off, &m) == 0); bad += merge_oracle(f, o, &m, NB_MERGE_OFF);
    CHECK(nb_merge(f, o, &strict, &m) == 0); bad += merge_oracle(f, o, &m, NB_MERGE_ON);
    nb_merge_summary(&m, line, sizeof line);
    merge_numbers s = merge_value(f, o, &m, imgs);
    printf("# golden %s NB-16 %s, hazards assumed tracked: %s; full-screen exact %d (G-buffer, 2+ colours: %d), DRAM saved %.1f MB:%s\n",
           path, tag, line, s.full_exact, s.full_gbuf, s.mb, s.sets.c_str());
    if (getenv("NB_GOLDEN_VERBOSE")) for (int k = 0; k < m.nrec; k++) printf("#   cand %d<-%d (head %d) %s %s between %d culprit %d\n", m.rec[k].node, m.rec[k].prev,
        m.rec[k].head, m.rec[k].kind ? "aba" : "aa", nb_merge_why_name(m.rec[k].why), m.rec[k].between, m.rec[k].culprit);
    CHECK(nb_merge(f, o, &hoist, &m) == 0); bad += merge_oracle(f, o, &m, NB_MERGE_ON);
    nb_merge_summary(&m, line, sizeof line);
    s = merge_value(f, o, &m, imgs);
    printf("# golden %s NB-16 %s, + HOIST: %s; full-screen exact %d (G-buffer %d), DRAM saved %.1f MB:%s\n", path, tag, line, s.full_exact, s.full_gbuf, s.mb, s.sets.c_str());
    printf("# golden %s NB-16 %s: merge oracle violations %d\n", path, tag, bad);
    CHECK(bad == 0);
    return bad;
}

static int run_golden(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("cannot open %s\n", path); return 2; }
    std::string text; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
    fclose(fp);
    int line = nb_parse(text.c_str(), &F);
    if (line) { printf("%s: parse error at line %d\n", path, line); return 1; }
    // Golden check: the exact-rule plan re-made on the host = the plan the phone wrote (its "# plan" / "# clear" lines). The phone had the
    // cross-frame history (nb_persist); the host replays one frame without it, so lines that need history may differ (counted).
    plan(EXACT);
    const std::string mine = dump(&F, &O);
    auto plan_lines = [](const std::string &t) { std::vector<std::string> v; size_t a = 0;
        while (a < t.size()) { size_t e = t.find('\n', a); if (e == std::string::npos) e = t.size(); std::string l = t.substr(a, e - a); a = e + 1;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.compare(0, 7, "# plan ") || !l.compare(0, 8, "# clear ")) v.push_back(l); }
        return v; };
    const std::vector<std::string> want = plan_lines(text), got = plan_lines(mine);
    size_t same = 0; for (size_t i = 0; i < want.size() && i < got.size(); i++) same += want[i] == got[i];
    if (getenv("NB_GOLDEN_VERBOSE")) for (size_t i = 0; i < want.size() && i < got.size(); i++) if (want[i] != got[i]) printf("# differs: phone '%s' host '%s'\n", want[i].c_str(), got[i].c_str());
    const nb_plan_stats st0 = O.st;
    replay_keep_barriers(&F, &G);
    CHECK(nb_plan(&G, nullptr, &EXACT, &O2) == 0);
    const nb_plan_stats &st1 = O2.st;
    int bar_end0 = 0, bar_end1 = 0;
    for (int i = 0; i < F.nnode; i++) bar_end0 += F.node[i].kind == NB_NODE_PASS && F.node[i].end == NB_END_BARRIER;
    for (int i = 0; i < G.nnode; i++) bar_end1 += G.node[i].kind == NB_NODE_PASS && G.node[i].end == NB_END_BARRIER;
    printf("# golden %s: plan lines phone %zu host %zu equal %zu; passes %d (exec %d empty %d clear-only %d, ended by a barrier %d) -> "
           "barriers kept %u: passes %d (exec %d empty %d clear-only %d, ended by a barrier %d, split by an attachment read %u); "
           "load L/C %d/%d -> %d/%d, store S/D %d/%d -> %d/%d, standalone clears %d -> %d\n",
           path, want.size(), got.size(), same, st0.passes, st0.exec, st0.empty, st0.clear_only, bar_end0,
           G.bar_kept, st1.passes, st1.exec, st1.empty, st1.clear_only, bar_end1, G.bar_split,
           st0.load_load, st0.load_clear, st1.load_load, st1.load_clear, st0.store_store, st0.store_dc, st1.store_store, st1.store_dc,
           st0.standalone, st1.standalone);
    int flagged = 0, cand = 0;
    const int bad = prebar_oracle(&F, &O, &flagged, &cand);
    CHECK(bad == 0 && flagged == st0.prebar);
    printf("# golden %s: NB-05c pre-pass barriers %d (passes of 2+ draws among them, the host's split candidates: %d); oracle mismatches %d\n",
           path, flagged, cand, bad);
    const std::vector<img_info> imgs = parse_images(text);
    golden_merge(path, "as dumped", &F, &O, imgs);
    golden_merge(path, "barriers kept", &G, &O2, imgs);
    if (getenv("NB_GOLDEN_VERBOSE")) { printf("# rules exact\n%s", mine.c_str()); printf("# rules exact, barriers kept\n%s", dump(&G, &O2).c_str());
        plan(IPAD); printf("# rules ipad\n%s", dump(&F, &O).c_str()); }
    return failures ? 1 : 0;
}

// NB-07: the tap's draw reads (nvn_nb_tap.inc nbt_program / nbt_mask / nbt_reads), images bound directly by slot (graphics stages 0-4).
static nb_sm_table SMT;
struct TapModel {
    uint64_t tex[NB_SM_STAGES][64] = {}, dirty[NB_SM_STAGES] = {}, prog[NB_SM_STAGES] = {}; int sm = 1, rd_node = -1; uint32_t rd_gen = 0, gen = 0;
    void bind(int st, int s, uint64_t im) { if (tex[st][s] != im) { tex[st][s] = im; dirty[st] |= 1ull << s; } }
    void program(uint64_t serial, int stages) { for (int st = 0; st < NB_SM_STAGES; st++) if (stages >> st & 1) prog[st] = serial; }
    uint64_t mask(int st) const {
        nb_sm_masks m;
        if (!sm || !prog[st] || !nb_sm_lookup(&SMT, prog[st], &m)) return NB_SM_ALL;
        return m.tex[st];
    }
    void draw(nb_frame *f) {
        nb_draw(f, 0);
        if (!f->open) return;
        const int node = f->nnode - 1, all = node != rd_node || rd_gen != gen;
        for (int st = 0; st < 5; st++) {
            uint64_t m = nb_sm_take(&dirty[st], all, mask(st));
            while (m) { const int s = __builtin_ctzll(m); m &= m - 1; if (tex[st][s]) nb_read(f, -1, tex[st][s]); }
        }
        rd_node = node; rd_gen = gen;
    }
    void reset() { gen++; }   // nvn_nb_tap_reset: the next draw takes every bound slot again
};
static int reads_of(const nb_frame *f, int node, uint64_t im) {
    int n = 0; const nb_node *x = &f->node[node];
    for (int j = 0; j < x->nref; j++) n += f->ref[x->ref_first + j].image == im;
    return n;
}
static void test_slotmask() {
    const uint64_t Gb = 0xa000, L = 0xb000, X0 = 0xe100, X1 = 0xe200, PG = 101, PL = 102, PL2 = 103, PU = 104;   // programs = prog_t serials
    enum { VS = 0, FS = 1, GFX = 0x1f };
    memset(&SMT, 0, sizeof SMT);
    { nb_sm_masks m{}; m.tex[FS] = 1ull << 0 | 1ull << 1; CHECK(nb_sm_publish(&SMT, PG, &m) == 1); }   // G-buffer: FS slots 0, 1
    { nb_sm_masks m{}; m.tex[FS] = 1ull << 0; CHECK(nb_sm_publish(&SMT, PL, &m) == 1); }                // L: FS slot 0 only
    { nb_sm_masks m{}; m.tex[FS] = 1ull << 0 | 1ull << 5; m.tex[VS] = 0; CHECK(nb_sm_publish(&SMT, PL2, &m) == 1); }   // L2: FS 0 and 5
    // PU never published: unknown = every bound slot (conservative).
    for (int sm = 0; sm < 2; sm++) {
        // (1) false 'touches-a': G-buffer, a pass L that samples T while Gb / D are still bound in slots 5 / 6 (the G-buffer's own textures
        // from an earlier pass, never sampled by L's program), G-buffer again.
        TapModel t; t.sm = sm;
        nb_frame_reset(&F, 70); nb_track(&F, NB_TRACK_MERGE);
        t.program(PG, GFX); t.bind(FS, 0, X0); t.bind(FS, 1, X1); t.bind(FS, 5, Gb); t.bind(FS, 6, D);
        pass(&F, Gb, D); clear_rt(&F, Gb, NB_ASP_COLOR); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL); for (int i = 0; i < 50; i++) t.draw(&F);
        t.program(PL, GFX); t.bind(FS, 0, T);
        pass(&F, L, 0); for (int i = 0; i < 4; i++) t.draw(&F);
        t.program(PG, GFX);
        pass(&F, Gb, D); for (int i = 0; i < 20; i++) t.draw(&F);
        nb_submit(&F, 1); plan(EXACT); merge(MERGE_ON);
        if (!sm) {   // before NB-07: L "reads" Gb and D, the G-buffer pass reads its own attachments (feedback)
            CHECK(reads_of(&F, 1, Gb) == 1 && reads_of(&F, 1, D) == 1 && reads_of(&F, 0, Gb) == 1);
            CHECK(M.st.merged == 0 && M.st.exact == 0 && (M.st.blocked[NB_MB_TOUCHES_A] + M.st.blocked[NB_MB_FEEDBACK]) == 1);
        } else {     // masks: L reads only T, the G-buffer pass only X0 / X1 -> aba exact, L before the unit
            CHECK(reads_of(&F, 1, Gb) == 0 && reads_of(&F, 1, D) == 0 && reads_of(&F, 1, T) == 1 && reads_of(&F, 0, Gb) == 0 && reads_of(&F, 0, X0) == 1);
            CHECK(M.st.cand_aba == 1 && M.st.exact == 1 && M.st.merged == 1 && (order_of() == std::vector<int>{ 1, 0, 2 }) && M.cont[2] == 0);
        }
        // (2) false STORE: depth D of pass A has no reader except a bound-but-unsampled slot of the next pass.
        TapModel u; u.sm = sm;
        nb_frame_reset(&F, 71);
        u.program(PG, GFX); u.bind(FS, 0, X0);
        pass(&F, A, D); clear_rt(&F, D, NB_ASP_DEPTH | NB_ASP_STENCIL); for (int i = 0; i < 10; i++) u.draw(&F);
        u.program(PL, GFX); u.bind(FS, 0, A); u.bind(FS, 6, D);
        pass(&F, B, 0); for (int i = 0; i < 3; i++) u.draw(&F);
        nb_submit(&F, 1); plan(EXACT);
        CHECK(reads_of(&F, 1, A) == 1);
        if (!sm) CHECK(reads_of(&F, 1, D) == 1 && O.node[0].store[1] == NB_STORE);              // false STORE of D
        else     CHECK(reads_of(&F, 1, D) == 0 && O.node[0].store[1] == NB_STORE_DONT_CARE);    // depth without a reader
    }
    // (3) a slot not sampled by the first program of a pass stays pending: a later program of the same pass that samples it (no rebind)
    // still records it; an unknown program (never published) records every bound slot; a reset (new IR) takes every slot again.
    TapModel v;
    nb_frame_reset(&F, 72);
    v.program(PL, GFX); v.bind(FS, 0, T); v.bind(FS, 5, X0); v.bind(FS, 6, X1);
    pass(&F, B, 0); v.draw(&F);
    CHECK(reads_of(&F, 0, T) == 1 && reads_of(&F, 0, X0) == 0 && reads_of(&F, 0, X1) == 0);
    v.program(PL2, GFX); v.draw(&F);
    CHECK(reads_of(&F, 0, X0) == 1 && reads_of(&F, 0, X1) == 0);
    v.program(PU, GFX); v.draw(&F);
    CHECK(reads_of(&F, 0, X1) == 1);
    v.program(PL, GFX); pass(&F, H, 0); v.draw(&F);   // new pass: every bound slot again, masked
    CHECK(reads_of(&F, 1, T) == 1 && reads_of(&F, 1, X0) == 0 && reads_of(&F, 1, X1) == 0);
    // (4) the table: key 0 is never stored, a republish replaces the masks, lookups of other keys stay unknown.
    { nb_sm_masks m{}, r{}; CHECK(nb_sm_publish(&SMT, 0, &m) == 0); m.tex[FS] = 7; CHECK(nb_sm_publish(&SMT, PL, &m) == 1);
      CHECK(nb_sm_lookup(&SMT, PL, &r) == 1 && r.tex[FS] == 7 && r.tex[VS] == 0); CHECK(nb_sm_lookup(&SMT, 999, &r) == 0); CHECK(SMT.n == 3); }
}

int main(int argc, char **argv) {
    if (argc > 1) return run_golden(argv[1]);
    test_clear_only_pass();
    test_depth_readers();
    test_cross_frame_reader();
    test_aba();
    test_mid_submit();
    test_colour_overwrite();
    test_transient();
    test_dump_roundtrip();
    test_limits();
    test_lost_records();
    test_barrier_kept();
    test_prebar();
    test_merge();
    test_slotmask();
    printf(failures ? "nb_plan_test: %d FAILED\n" : "nb_plan_test: all passed\n", failures);
    return failures ? 1 : 0;
}
