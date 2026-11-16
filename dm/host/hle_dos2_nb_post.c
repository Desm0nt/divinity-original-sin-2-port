// [DOS2] NB-18b-4a: the post-tail fusion plan in the NB front end (DM_NB_POST=1/2/3, default 0; docs/dos2/NATIVE_BACKEND_PLAN.md
// "NB-18b-4 findings"). Needs DM_NB=1 or 2 (the frame IR). Parts:
//   - the BE -> FE program table (dm/gfx/nb/nb_post.h): the backend publishes prog_t serial -> fs key + sampled slots per stage once it
//     resolved the program; the NVN tap reads it at each draw (nvn_nb_tap.inc nbt_prog_draw -> nb_draw_prog). Open addressing, one
//     writer under a mutex, lock-free readers (serial stored last with release, never rewritten).
//   - dm_nb_post_plan: nb_post_plan (dm/gfx/nb/nb_plan.c) on every planned submission; the PASS headers then carry NB_NP_ELIDE /
//     NB_NP_FUSE (nvn_nb_tap_patch copies the flags). Sizes from the tap's RT image table. The backend decides per frame what it does
//     with the flags (xbg_nb_post.inc: 2 = check only, 1 = elide + fused draw, 3 = 1 + poison of the elided target).
//   - one line "[nb] post plan ..." every 10 s.
// DM_NB_POST unset / 0: nothing here runs (the tap's program hook is one not-taken branch).
// NB-18b-8 (route A): DM_NB_POST_AA=1 (default 0; needs DM_NB_POST) runs nb_post_aa_plan (dm/gfx/nb/nb_post_aa.h) after nb_post_plan:
// the F1 pair becomes F1A (FXAA on the pre-LUT image inside Sight's draw) and the console's FXAA pass after Sight is elided, per
// submission; the console's FXAA stays on in the engine, so a refused submission keeps its anti-aliasing. One more line
// "[nb] post plan aa ..." every 10 s.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/nb/nb_post.h"
#include "../gfx/nb/nb_post_aa.h"
#include "../gfx/nvn/nvn_nb_tap.h"

int g_dm_nb_post;
enum { NBP_CAP = 16384 };
static nb_post_prog g_tab[NBP_CAP]; static uint64_t g_key[NBP_CAP]; static uint32_t g_count, g_full;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t nbp_hash(uint64_t s) { s ^= s >> 31; s *= 0x9E3779B97F4A7C15ull; return (uint32_t)(s >> 40) & (NBP_CAP - 1); }

void dm_nb_post_publish(const nb_post_prog *p) {
    if (!p || !p->serial) return;
    pthread_mutex_lock(&g_mu);
    uint32_t h = nbp_hash(p->serial);
    for (int i = 0; i < 64; i++, h = (h + 1) & (NBP_CAP - 1)) {
        const uint64_t k = __atomic_load_n(&g_key[h], __ATOMIC_RELAXED);
        if (k == p->serial) break;
        if (!k) { g_tab[h] = *p; __atomic_store_n(&g_key[h], p->serial, __ATOMIC_RELEASE); g_count++; pthread_mutex_unlock(&g_mu); return; }
    }
    if (__atomic_load_n(&g_key[h], __ATOMIC_RELAXED) != p->serial) g_full++;
    pthread_mutex_unlock(&g_mu);
}
const nb_post_prog *dm_nb_post_prog(uint64_t serial) {
    if (!serial) return NULL;
    uint32_t h = nbp_hash(serial);
    for (int i = 0; i < 64; i++, h = (h + 1) & (NBP_CAP - 1)) {
        const uint64_t k = __atomic_load_n(&g_key[h], __ATOMIC_ACQUIRE);
        if (k == serial) return &g_tab[h];
        if (!k) return NULL;
    }
    return NULL;
}

static nb_post_hist g_hist; static nb_post_out g_out;
static struct { uint64_t parts, fused[NB_FXK_KINDS], why[NB_FXK_KINDS][NB_PW_KINDS], later_cover, assumed, bad, exact_passes, passes_fs; } g_pp;
static int g_aa; static nb_post_aa_state g_aast; static nb_post_aa_out g_aao;
static struct { uint64_t f1a, fxaa, copy, copy_kept, nostep, plain, readers, op_reads, bad, unrelated, carried, why[NB_PAW_KINDS]; } g_pa;
// NB-18b-9: a pair left plain names its nodes (3 lines per reason): which pass read Sight's target first, which FXAA pass refused it.
static void nbp_node_str(const nb_frame *f, int q, char *b, size_t cap) {
    if (q < 0 || q >= f->nnode) { snprintf(b, cap, "none"); return; }
    const nb_node *m = &f->node[q];
    if (m->kind != NB_NODE_PASS) { snprintf(b, cap, "node %d op %s", q, nb_kind_name(m->kind & 0xff)); return; }
    uint32_t w = 0, h = 0; int fmt = 0; const uint64_t t = m->natt ? m->att[0].image : 0;
    if (t) nvn_nb_tap_image(t, &w, &h, &fmt);
    int o = snprintf(b, cap, "node %d pass fs %016llx%s%s, %u draws, %d targets, first %#llx %ux%u, samples", q, (unsigned long long)m->fs,
                     (m->fsx & NB_FSX_EXACT) ? " exact" : " NOT exact", (m->fsx & NB_FSX_MIXED) ? " mixed" : "", m->draws, m->natt, (unsigned long long)t, w, h);
    for (int i = 0; i < m->nsr && o > 0 && o < (int)cap - 40; i++) {
        uint32_t sw = 0, sh = 0; nvn_nb_tap_image(m->sread[i], &sw, &sh, &fmt);
        o += snprintf(b + o, cap - (size_t)o, " %#llx %ux%u", (unsigned long long)m->sread[i], sw, sh);
    }
}
static void nbp_aa_diag(const nb_frame *f) {
    static unsigned told[NB_PAW_KINDS];
    const int w = g_aao.d_why;
    if (g_aao.d_host < 0 || w >= NB_PAW_KINDS || told[w] >= 3) return;
    told[w]++;
    char a[420], b[420], c[420];
    nbp_node_str(f, g_aao.d_host, a, sizeof a); nbp_node_str(f, g_aao.d_first, b, sizeof b); nbp_node_str(f, g_aao.d_other, c, sizeof c);
    LOG("[nb] post plan aa: frame %u left as F1 + the console's FXAA (%s): Sight = %s; first reader of its target = %s; the FXAA pass that refused it = %s",
        f->frame_no, nb_post_aa_why_name(w), a, b, c);
}
static int nbp_size(void *ctx, uint64_t image, uint32_t *w, uint32_t *h) { (void)ctx; int fmt; return nvn_nb_tap_image(image, w, h, &fmt); }

void dm_nb_post_install(void) {
    const char *e = getenv("DM_NB_POST");
    const int m = e && *e ? atoi(e) : 0;
    if (m < 1 || m > 3) return;
    g_dm_nb_post = m;
    LOG("[nb] DM_NB_POST=%d: post-tail fusion plan on (F1 = LUT -> Sight, F2 = Vignette -> CombineUI_NoFetch; the backend %s)", m,
        m == 2 ? "checks only: the chain runs as recorded" : m == 1 ? "elides the first pass and draws the fused shader" : "elides, fuses and poisons the elided target");
    const char *a = getenv("DM_NB_POST_AA");
    if (a && *a) {
        if (a[0] == '1' && !a[1]) {
            g_aa = 1;
            LOG("[nb] DM_NB_POST_AA=1: route A plan on (F1A = FXAA on the pre-LUT image -> LUT -> Sight at Sight's draw; the console's FXAA pass after "
                "Sight is elided per submission and its readers sample Sight's target; a refused submission runs the recorded chain with the console's FXAA)");
        } else if (a[0] != '0' || a[1]) LOG("[nb] DM_NB_POST_AA=%s not understood (0 / 1): off", a);
    }
}
void dm_nb_post_plan(const nb_frame *f, nb_plan_out *po) {
    if (!g_dm_nb_post) return;
    const nb_post_rules r = { 0, 8, nbp_size, NULL };
    g_pp.parts++;
    if (nb_post_plan(f, po, &r, &g_hist, &g_out) < 0) { g_pp.bad++; return; }
    for (int k = 0; k < NB_FXK_KINDS; k++) { g_pp.fused[k] += (uint64_t)g_out.fused[k]; for (int w = 0; w < NB_PW_KINDS; w++) g_pp.why[k][w] += (uint64_t)g_out.why[k][w]; }
    g_pp.later_cover += (uint64_t)g_out.later_cover; g_pp.assumed += (uint64_t)g_out.assumed_cover;
    if (g_aa) {   // NB-18b-8: NB_NP_AA on the F1 pair / the console's FXAA pass / the readers of its target
        if (nb_post_aa_plan(f, po, &g_out, &r, &g_aast, &g_aao) < 0) g_pa.bad++;
        else {
            g_pa.f1a += (uint64_t)g_aao.f1a; g_pa.fxaa += (uint64_t)g_aao.f1a_fxaa; g_pa.copy += (uint64_t)g_aao.f1a_copy; g_pa.copy_kept += (uint64_t)g_aao.f1a_copy_kept;
            g_pa.nostep += (uint64_t)g_aao.f1a_nostep; g_pa.plain += (uint64_t)g_aao.plain; g_pa.readers += (uint64_t)g_aao.readers; g_pa.op_reads += (uint64_t)g_aao.op_reads;
            for (int w = 0; w < NB_PAW_KINDS; w++) g_pa.why[w] += (uint64_t)g_aao.why[w];
            g_pa.unrelated += (uint64_t)g_aao.unrelated_fxaa; g_pa.carried += (uint64_t)g_aao.carried;
            if (g_aao.plain) nbp_aa_diag(f);
        }
    }
    for (int i = 0; i < f->nnode; i++) if (f->node[i].kind == NB_NODE_PASS && (f->node[i].fsx & NB_FSX_ANY)) {
        g_pp.passes_fs++; if (f->node[i].fsx & NB_FSX_EXACT) g_pp.exact_passes++; }
}
void dm_nb_post_report(double fr) {
    if (!g_dm_nb_post) return;
    char w[2][600];
    for (int k = 1; k < NB_FXK_KINDS; k++) {
        int o = 0; w[k - 1][0] = 0;
        for (int i = 1; i < NB_PW_KINDS && o < (int)sizeof w[0] - 80; i++) if (g_pp.why[k][i])
            o += snprintf(w[k - 1] + o, sizeof w[0] - (size_t)o, ", %s %llu", nb_post_why_name(i), (unsigned long long)g_pp.why[k][i]);
    }
    LOG("[nb] post plan DM_NB_POST=%d: %llu submissions planned (bad %llu); fused a frame F1 %.2f%s, F2 %.2f%s; later loads of the intermediate "
        "by cover_dc %llu, by a disproved feedback (assumed covering) %llu; passes with an fs %.1f a frame (exact reads %.1f); program table %u (full %u)",
        g_dm_nb_post, (unsigned long long)g_pp.parts, (unsigned long long)g_pp.bad, g_pp.fused[1] / fr, w[0], g_pp.fused[2] / fr, w[1],
        (unsigned long long)g_pp.later_cover, (unsigned long long)g_pp.assumed, g_pp.passes_fs / fr, g_pp.exact_passes / fr, g_count, g_full);
    memset(&g_pp, 0, sizeof g_pp);
    if (g_aa) {
        char b[700]; int o = 0, bad = 0; b[0] = 0;
        for (int i = NB_PAW_OTHER_FXAA; i < NB_PAW_KINDS && o < (int)sizeof b - 90; i++) if (g_pa.why[i])
            o += snprintf(b + o, sizeof b - (size_t)o, ", %s %llu", nb_post_aa_why_name(i), (unsigned long long)g_pa.why[i]);
        for (int t = 0; t < g_aast.n; t++) bad += g_aast.t[t].bad;
        LOG("[nb] post plan aa DM_NB_POST_AA=1: F1A a frame %.2f (FXAA pass elided %.2f, PPDefault copy elided %.2f / left %.2f, no AA pass after Sight %.2f); "
            "left as F1 + the console's FXAA %.2f; refusals%s; passes told to sample Sight's target %.2f a frame; aliased targets tracked %d "
            "(read by an operation, never aliased again: %d; such reads now %llu); FXAA passes of other chains after Sight (not fed by its target, ignored) "
            "%.2f a frame; fog steps between Sight and the elided AA pass (run as recorded, carry F1A's output) %.2f a frame; bad input %llu",
            g_pa.f1a / fr, g_pa.fxaa / fr, g_pa.copy / fr, g_pa.copy_kept / fr, g_pa.nostep / fr, g_pa.plain / fr, o ? b : ": none", g_pa.readers / fr,
            g_aast.n, bad, (unsigned long long)g_pa.op_reads, g_pa.unrelated / fr, g_pa.carried / fr, (unsigned long long)g_pa.bad);
        memset(&g_pa, 0, sizeof g_pa);
    }
}
#endif
