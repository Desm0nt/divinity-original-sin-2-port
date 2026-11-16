// [tile] 16.11: host test of the store-hist bookkeeping (xbg_store_hist.h, used by xbg_game.cpp for DM_GFX_STORE_HIST and DM_NB_RP 1 / 3)
// and of the plan's keep mask (nb_rp_keep_store). The sequences are the ones the ab7rp3 phone run raised (docs/dos2/DOS2_PORT.md
// "store-hist: учёт выброшенного store").
// Build, from this folder:  clang++ -std=c++17 -Wall -Wextra store_hist_test.cpp -o store_hist_test   (or g++)
#include "../xbg_store_hist.h"
#include "../../../nb/nb_rp.h"
#include <stdio.h>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)
enum { COL = 1, DEP = 2, STE = 4 };

// The xbg_game.cpp paths in miniature: a pass begin (hist_begin), the heuristic's decision (store_hist_mask), the pass end marks.
static int begin(XbgHist &h, uint64_t frame, uint32_t clear, uint32_t dc, unsigned asp) {
    xbg_hist_attached(h, frame);
    if (xbg_hist_att_overwritten(clear, dc, 0, asp)) { xbg_hist_overwrite(h); return 0; }
    return xbg_hist_read(h);
}
static void test_overwritten() {
    CHECK(xbg_hist_att_overwritten(1u, 0, 0, COL) && !xbg_hist_att_overwritten(0, 0, 0, COL));
    CHECK(xbg_hist_att_overwritten(0, 1u, 0, COL) && xbg_hist_att_overwritten(0, 1u, 0, DEP | STE));   // load DONT_CARE
    CHECK(xbg_hist_att_overwritten(3u, 0, 0, DEP | STE) && !xbg_hist_att_overwritten(1u, 0, 0, DEP | STE));   // D24S8: both aspects
    CHECK(!xbg_hist_att_overwritten(2u, 0, 0, DEP | STE) && xbg_hist_att_overwritten(1u, 0, 0, DEP));
    CHECK(xbg_hist_att_overwritten(3u << 4, 0, 2, DEP | STE) && !xbg_hist_att_overwritten(3u << 4, 0, 1, DEP | STE));
    CHECK(xbg_hist_att_overwritten(0, 4u, 2, COL) && !xbg_hist_att_overwritten(0, 4u, 1, COL));
}
static void test_heuristic() {
    XbgHist h;
    for (uint64_t f = 10; f < 18; f++) { CHECK(begin(h, f, 3u, 0, DEP | STE) == 0); CHECK(!xbg_hist_may_discard(h, f, 8)); }
    CHECK(begin(h, 18, 3u, 0, DEP | STE) == 0 && xbg_hist_may_discard(h, 18, 8));   // 8 frames as an attachment, never read
    xbg_hist_discarded(h, false, XBG_HIST_BY_HIST, 18, 0, -1);
    CHECK(begin(h, 19, 3u, 0, DEP | STE) == 0 && !h.dead && !h.banned);           // cleared next frame: no violation
    xbg_hist_discarded(h, false, XBG_HIST_BY_HIST, 19, 0, -1);
    CHECK(xbg_hist_read(h) == 1 && h.banned && h.read_ever);                       // sampled while discarded
    CHECK(!xbg_hist_may_discard(h, 100, 8));                                       // never again
    XbgHist r; CHECK(begin(r, 1, 0, 0, COL) == 0 && r.read_ever);                   // LOADed from the start: read, never discarded
    for (uint64_t f = 1; f < 50; f++) CHECK(!xbg_hist_may_discard(r, f, 8));
    XbgHist c; xbg_hist_attached(c, 0); for (int i = 0; i < 9; i++) xbg_hist_attached(c, uint64_t(i)); CHECK(!xbg_hist_may_discard(c, 7, 8) && xbg_hist_may_discard(c, 8, 8));
    xbg_hist_discarded(c, true, XBG_HIST_BY_HIST, 8, 0, -1);                       // check mode: decided, nothing discarded
    CHECK(c.vdead && !c.dead && xbg_hist_read(c) == 2 && c.banned);
}
// The ab7rp3 false alarms: the plan discards the main depth at the end of a frame; the next frame clears it through a load-op CLEAR
// (plan CLEAR / host clear twin), an in-pass whole clear or a whole ClearTexture / copy, then lighting samples it. Before the fix only
// a deferred ClearTexture forgot the discard, so the sample was reported (and banned the image). Now: no violation, no ban.
static void test_plan_overwrites() {
    const struct { uint32_t clear, dc; bool pass; } way[] = { { 3u, 0, true }, { 0, 1u, true }, { 0, 0, false } };
    for (const auto &w : way) {
        XbgHist h; xbg_hist_attached(h, 1);
        xbg_hist_discarded(h, false, XBG_HIST_BY_PLAN, 1, 7, 42);
        CHECK(h.dead && h.by == XBG_HIST_BY_PLAN && h.dead_seq == 7 && h.dead_node == 42 && h.dead_frame == 1);
        if (w.pass) CHECK(begin(h, 2, w.clear, w.dc, DEP | STE) == 0);
        else xbg_hist_overwrite(h);   // in-pass whole clear / transfer clear / full copy (xbg_game.cpp hist_note_overwrite)
        CHECK(xbg_hist_read(h) == 0 && !h.banned);
    }
    // A real violation: discarded, then the next pass LOADs it (depth-only clear of D24S8 is not a full overwrite either).
    XbgHist h; xbg_hist_discarded(h, false, XBG_HIST_BY_PLAN, 3, 1, 2);
    CHECK(begin(h, 4, 1u, 0, DEP | STE) == 1 && h.banned);
}
// The plan honours the ban: banned attachments, and attachments this pass LOADs while discarded, keep STORE (xbg_game.cpp hist_keep).
static uint32_t keep(const XbgHist *h, unsigned n, uint32_t clear, uint32_t dc, const unsigned *asp) {
    uint32_t m = 0;
    for (unsigned k = 0; k < n; k++) if (h[k].banned || ((h[k].dead || h[k].vdead) && !xbg_hist_att_overwritten(clear, dc, k, asp[k]))) m |= 1u << k;
    return m;
}
static void test_plan_keep() {
    XbgHist h[3]; const unsigned asp[3] = { COL, COL, DEP | STE };
    nb_rp_dec d{}; d.n = 3; d.store_dc = 4u | 1u;
    nb_rp_twin_out t = nb_rp_twin(&d, 0, 0);
    CHECK(keep(h, 3, 0, 0, asp) == 0 && nb_rp_keep_store(&t, 0) == 0 && (t.mask >> 18 & 7u) == 5u);
    h[2].banned = true;                                     // a violation earlier: the plan's depth DONT_CARE is refused
    CHECK(keep(h, 3, 0, 0, asp) == 4u && nb_rp_keep_store(&t, keep(h, 3, 0, 0, asp)) == 1 && (t.mask >> 18 & 7u) == 1u);
    XbgHist g[3]; xbg_hist_discarded(g[0], false, XBG_HIST_BY_PLAN, 5, 0, 0);
    CHECK(keep(g, 3, 0, 0, asp) == 1u);                     // LOADed while discarded: keep STORE (it is banned right after)
    CHECK(keep(g, 3, 1u, 0, asp) == 0);                     // cleared by the pass: nothing old to keep
}

int main() {
    test_overwritten(); test_heuristic(); test_plan_overwrites(); test_plan_keep();
    if (failures) { printf("store_hist_test: %d failure(s)\n", failures); return 1; }
    printf("store_hist_test: all passed\n"); return 0;
}
