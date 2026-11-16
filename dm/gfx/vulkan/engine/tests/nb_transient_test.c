// NB-19 host test: dm/gfx/nb/nb_transient.h (DM_NB_TRANSIENT, transient attachments) + golden replay of IR dumps.
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nb_transient_test.c dm/gfx/nb/nb_plan.c -o nb_transient_test
//   ./nb_transient_test                      unit cases of the decision
//   ./nb_transient_test <dump.txt> ...       + each IR dump (DM_NB_DUMP=1) replayed 12 frames through nb_plan (exact rules, persistent
//                                            state) and turned into the backend's frame flags the way xbg_nb_transient.inc gathers them:
//                                            an executed pass = a planned host pass (load_ok = plan load CLEAR / DONT_CARE; a cover_dc
//                                            LOAD counts as LOAD), its reads (and a read of its own attachment) = READ, op src / dst and a
//                                            standalone clear = XFER. Prints the reasons of the last frame per attachment image.
#include "../../../nb/nb_ir.h"
#include "../../../nb/nb_transient.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int judge(nb_tr_img *s, int apply, int *act) { return nb_tr_judge(s, NB_TRS_OK, 8, apply, act); }
static void clean_frame(nb_tr_img *s, uint32_t fr) { nb_tr_pass(s, fr, 1, 1, 1); }

static void unit(void) {
    int act;
    // 1. depth cleared by the load op and dropped every frame: STREAK for 7 frames, candidate on the 8th; count mode never creates.
    {   nb_tr_img s; memset(&s, 0, sizeof s);
        for (uint32_t f = 0; f < 7; f++) { clean_frame(&s, f); CHECK(judge(&s, 1, &act) == NB_TRR_STREAK && act == NB_TRA_NONE); }
        nb_tr_img c = s;
        clean_frame(&s, 7); CHECK(judge(&s, 1, &act) == NB_TRR_CAND && act == NB_TRA_CREATE && s.transient);
        clean_frame(&s, 8); CHECK(judge(&s, 1, &act) == NB_TRR_CAND && act == NB_TRA_KEEP && s.transient);
        clean_frame(&c, 7); CHECK(judge(&c, 0, &act) == NB_TRR_CAND && act == NB_TRA_NONE && !c.transient);
        clean_frame(&c, 8); CHECK(judge(&c, 0, &act) == NB_TRR_CAND && act == NB_TRA_NONE && !c.transient);
    }
    // 2. several passes a frame, each self-contained: still clean; one pass that loads breaks it (LOAD) and resets the streak.
    {   nb_tr_img s; memset(&s, 0, sizeof s);
        for (uint32_t f = 0; f < 5; f++) { clean_frame(&s, f); clean_frame(&s, f); CHECK(judge(&s, 1, &act) == NB_TRR_STREAK); }
        CHECK(s.streak == 5);
        nb_tr_pass(&s, 5, 1, 1, 1); nb_tr_pass(&s, 5, 1, 0, 1);
        CHECK(judge(&s, 1, &act) == NB_TRR_LOAD && s.streak == 0 && act == NB_TRA_NONE);
        nb_tr_pass(&s, 6, 1, 1, 0); CHECK(judge(&s, 1, &act) == NB_TRR_STORE);
        nb_tr_pass(&s, 7, 0, 1, 1); CHECK(judge(&s, 1, &act) == NB_TRR_UNPLANNED);   // heuristics pass: not proven, whatever its ops
        nb_tr_pass(&s, 8, 0, 0, 0); nb_tr_pass(&s, 8, 1, 0, 0); CHECK(judge(&s, 1, &act) == NB_TRR_UNPLANNED);   // first reason wins
    }
    // 3. a read anywhere in its life is sticky (READ before XFER before the pass reasons), even in a frame without that read.
    {   nb_tr_img s; memset(&s, 0, sizeof s);
        CHECK(nb_tr_touch(&s, 0, NB_TRF_XFER) == 0);
        clean_frame(&s, 0); CHECK(judge(&s, 1, &act) == NB_TRR_XFER);
        clean_frame(&s, 1); CHECK(judge(&s, 1, &act) == NB_TRR_XFER);
        nb_tr_touch(&s, 2, NB_TRF_READ); nb_tr_pass(&s, 2, 0, 0, 0); CHECK(judge(&s, 1, &act) == NB_TRR_READ);
        for (uint32_t f = 3; f < 20; f++) { clean_frame(&s, f); CHECK(judge(&s, 1, &act) == NB_TRR_READ && act == NB_TRA_NONE); }
        CHECK(s.ever == (NB_TRF_READ | NB_TRF_XFER) && !s.transient);
    }
    // 4. transient image read: touch says re-create now, banned for good (BANNED before every other reason).
    {   nb_tr_img s; memset(&s, 0, sizeof s);
        for (uint32_t f = 0; f < 8; f++) { clean_frame(&s, f); judge(&s, 1, &act); }
        CHECK(s.transient && act == NB_TRA_CREATE);
        CHECK(nb_tr_touch(&s, 8, NB_TRF_READ) == 1 && !s.transient && s.banned);
        CHECK(nb_tr_touch(&s, 8, NB_TRF_READ) == 0);   // once
        clean_frame(&s, 8); CHECK(judge(&s, 1, &act) == NB_TRR_BANNED && act == NB_TRA_NONE);
        for (uint32_t f = 9; f < 30; f++) { clean_frame(&s, f); CHECK(judge(&s, 1, &act) == NB_TRR_BANNED && !s.transient); }
    }
    // 5. transient image loaded / stored / begun by heuristics: valid Vulkan, judged at the frame end -> REVERT + ban.
    for (int k = 0; k < 3; k++) {
        nb_tr_img s; memset(&s, 0, sizeof s);
        for (uint32_t f = 0; f < 8; f++) { clean_frame(&s, f); judge(&s, 1, &act); }
        CHECK(s.transient);
        nb_tr_pass(&s, 8, k != 2, k != 0, k != 1);
        const int r = judge(&s, 1, &act);
        CHECK(act == NB_TRA_REVERT && !s.transient && s.banned);
        CHECK(r == (k == 0 ? NB_TRR_LOAD : k == 1 ? NB_TRR_STORE : NB_TRR_UNPLANNED));
    }
    // 6. a transient image not used in a frame stays transient; frames without it do not count towards the streak.
    {   nb_tr_img s; memset(&s, 0, sizeof s);
        for (uint32_t f = 0; f < 8; f++) { clean_frame(&s, f); judge(&s, 1, &act); }
        nb_tr_begin_frame(&s, 9); CHECK(judge(&s, 1, &act) == NB_TRR_CAND && act == NB_TRA_NONE && s.transient);
        nb_tr_img t; memset(&t, 0, sizeof t);
        for (uint32_t f = 0; f < 4; f++) { clean_frame(&t, 2 * f); judge(&t, 1, &act); }
        CHECK(t.streak == 4 && !t.transient);
    }
    // 7. shape: the first refusal in order; a misfit image is SHAPE before READ.
    CHECK(nb_tr_shape(1, 1, 1, 0, 1) == NB_TRS_OK);
    CHECK(nb_tr_shape(0, 4, 2, 1, 0) == NB_TRS_TYPE);
    CHECK(nb_tr_shape(1, 4, 2, 1, 0) == NB_TRS_LEVELS);
    CHECK(nb_tr_shape(1, 1, 6, 1, 0) == NB_TRS_LAYERS);
    CHECK(nb_tr_shape(1, 1, 1, 1, 0) == NB_TRS_ALIAS);
    CHECK(nb_tr_shape(1, 1, 1, 0, 0) == NB_TRS_FORMAT);
    {   nb_tr_img s; memset(&s, 0, sizeof s); nb_tr_touch(&s, 0, NB_TRF_READ); clean_frame(&s, 0);
        CHECK(nb_tr_judge(&s, NB_TRS_LEVELS, 8, 1, &act) == NB_TRR_SHAPE); }
    // 8. one listing per image and frame; flags reset with the frame; min_frames <= 0 = 8.
    {   nb_tr_img s; memset(&s, 0, sizeof s);
        CHECK(nb_tr_pass(&s, 3, 1, 1, 1) == 1 && nb_tr_pass(&s, 3, 1, 1, 1) == 0 && nb_tr_touch(&s, 3, NB_TRF_XFER) == 0);
        CHECK(nb_tr_pass(&s, 4, 1, 1, 1) == 1 && s.frame_flags == NB_TRF_ATT);
        nb_tr_img u; memset(&u, 0, sizeof u);
        for (uint32_t f = 0; f < 8; f++) { clean_frame(&u, f); nb_tr_judge(&u, NB_TRS_OK, 0, 1, &act); }
        CHECK(u.transient);
    }
    // 9. random model: the decision never makes an image transient that was read / copied or had a non-clean judged frame within
    //    its last 8 judged frames, and an image is transient only while no such event happened since its creation.
    {   uint64_t r = 0x9e3779b97f4a7c15ull;
        for (int img = 0; img < 2000; img++) {
            nb_tr_img s; memset(&s, 0, sizeof s);
            int clean_run = 0, dirty_ever = 0, bad_since = 0;
            for (uint32_t f = 0; f < 200; f++) {
                r ^= r << 13; r ^= r >> 7; r ^= r << 17;
                const unsigned x = (unsigned)(r >> 40) & 1023;
                int used = x < 900, ev = (x % 97) == 0 ? NB_TRF_READ : (x % 89) == 0 ? NB_TRF_XFER : 0;
                int planned = (x % 53) != 0, lok = (x % 41) != 0, sdc = (x % 37) != 0;
                if (img & 1) { planned = lok = sdc = 1; if (ev && (x & 7)) ev = 0; }
                const int was = s.transient;
                if (ev) { const int now = nb_tr_touch(&s, f, (unsigned)ev); CHECK(now == was); dirty_ever = 1; if (was) bad_since = 1; }
                if (used) nb_tr_pass(&s, f, planned, lok, sdc);
                if (!used && !s.transient && !ev) continue;   // the backend lists only images it saw this frame
                nb_tr_judge(&s, NB_TRS_OK, 8, 1, &act);
                const int clean = used && planned && lok && sdc && !dirty_ever;
                if (used) clean_run = clean ? clean_run + 1 : 0;
                if (s.transient) { CHECK(!dirty_ever && clean_run >= 8 && !bad_since); }
                if (act == NB_TRA_CREATE) CHECK(clean_run >= 8);
                if (used && !clean && was && !s.banned) CHECK(0);
            }
        }
    }
}

/* ---------------- golden replay ---------------- */
static nb_frame F;
static nb_plan_out O;
static nb_persist P;
typedef struct gimg { uint64_t id; nb_tr_img s; int ds, listed, why, att_frames; } gimg;
static gimg G[4096]; static int ng;
static gimg *gfind(uint64_t id) {
    for (int i = 0; i < ng; i++) if (G[i].id == id) return &G[i];
    if (ng >= (int)(sizeof G / sizeof G[0])) return NULL;
    memset(&G[ng], 0, sizeof G[ng]); G[ng].id = id; G[ng].why = -1; return &G[ng++];
}
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = (char *)malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f); fclose(f); b[got] = 0; return b;
}
static void touch(uint32_t fr, uint64_t id, unsigned kind) { gimg *g = gfind(id); if (g) nb_tr_touch(&g->s, fr, kind); }
static int golden(const char *path, int *tot_cand, int *tot_att) {
    char *text = slurp(path); if (!text) { printf("cannot read %s\n", path); return -1; }
    const int bad = nb_parse(text, &F); free(text);
    if (bad) { printf("%s: parse error at line %d\n", path, bad); return -1; }
    nb_persist_reset(&P); ng = 0;
    const nb_rules ex = { NB_RULES_EXACT, 0, 0 };
    const int frames = 12; int ntr = 0; const uint32_t base = F.frame_no;
    for (int it = 0; it < frames; it++) {
        const uint32_t fr = base + (uint32_t)it;
        nb_frame *f = &F; f->frame_no = fr;
        if (nb_plan(f, &P, &ex, &O) != 0) { printf("%s: plan failed\n", path); return -1; }
        ntr = O.ntransient;
        for (int ni = 0; ni < f->nnode; ni++) {
            const nb_node *n = &f->node[ni]; const nb_node_plan *np = &O.node[ni]; const nb_ref *refs = &f->ref[n->ref_first];
            if (n->kind == NB_NODE_PASS) {
                if (np->flags & NB_NP_EXEC) {
                    for (int k = 0; k < n->natt; k++) {
                        gimg *g = gfind(n->att[k].image); if (!g) continue;
                        if (n->att[k].slot == NB_SLOT_DS) g->ds = 1;
                        nb_tr_pass(&g->s, fr, 1, np->load[k] != NB_LOAD, np->store[k] == NB_STORE_DONT_CARE);
                    }
                    for (int i = 0; i < n->nref; i++) touch(fr, refs[i].image, NB_TRF_READ);   // own attachment too (feedback)
                }
                for (int i = 0; i < n->nclear; i++) if (O.clr[n->clear_first + i].fate == NB_CLR_STANDALONE) touch(fr, f->clr[n->clear_first + i].image, NB_TRF_XFER);
            } else {
                for (int i = 0; i < n->nref; i++) touch(fr, refs[i].image, NB_TRF_XFER);
                if ((n->kind & 0xff) == NB_OP_CLEAR && n->nclear && O.clr[n->clear_first].fate == NB_CLR_STANDALONE) touch(fr, f->clr[n->clear_first].image, NB_TRF_XFER);
            }
        }
        for (int i = 0; i < ng; i++) {
            gimg *g = &G[i]; if (g->s.frame != fr || !(g->s.frame_flags & NB_TRF_ATT)) { if (it == frames - 1) g->listed = 0; continue; }
            int act; const int why = nb_tr_judge(&g->s, NB_TRS_OK, 8, 1, &act);
            g->att_frames++;
            if (it == frames - 1) { g->listed = 1; g->why = why; }
        }
    }
    int by[NB_TRR_KINDS][2]; memset(by, 0, sizeof by); int att = 0;
    for (int i = 0; i < ng; i++) if (G[i].listed) { by[G[i].why][G[i].ds]++; att++; }
    printf("%s: frame %u x%d, %d attachment images (planner transient list %d): ", path, base, frames, att, ntr);
    for (int r = 0; r < NB_TRR_KINDS; r++) if (by[r][0] || by[r][1]) printf("%s %d (ds %d) ", nb_tr_reason_name(r), by[r][0] + by[r][1], by[r][1]);
    printf("\n");
    for (int i = 0; i < ng; i++) if (G[i].listed && G[i].ds) printf("    ds %#llx: %s (streak %u)\n", (unsigned long long)G[i].id, nb_tr_reason_name(G[i].why), G[i].s.streak);
    // Cross-check: a candidate of the decision is one of the planner's own transient list (the planner's rule is not stricter on these).
    for (int i = 0; i < ng; i++) if (G[i].listed && G[i].why == NB_TRR_CAND) {
        int in = 0; for (int t = 0; t < O.ntransient; t++) if (O.transient[t] == G[i].id) in = 1;
        if (!in) printf("    note: candidate %#llx is not in the planner's transient list\n", (unsigned long long)G[i].id);
    }
    *tot_cand += by[NB_TRR_CAND][0] + by[NB_TRR_CAND][1]; *tot_att += att;
    return 0;
}

int main(int argc, char **argv) {
    unit();
    int cand = 0, att = 0, dumps = 0;
    for (int i = 1; i < argc; i++) if (golden(argv[i], &cand, &att) == 0) dumps++; else fails++;
    if (dumps) printf("golden: %d dumps, %d attachment images, %d candidates\n", dumps, att, cand);
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
