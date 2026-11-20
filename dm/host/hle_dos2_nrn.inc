// [DOS2] NR-NATIVE: the draw as the engine holds it, above NVN (DM_NRN / DM_NRN_<PASS>, default 0; docs/dos2/nr/NR-NATIVE.md). Included by
// hle_dos2_nr.c ahead of its hooks (they call NRN_NOTE). main = main.raw of update v786432, build 4979b200.
//   every DM_NRN_<PASS> = 0 / unset   nothing: no word is read from the renderer, the draw record is byte for byte the old one
//                                     (nvn_nb_draw.inc pays one load).
//   DM_NRN_<PASS> = 2                 an engine draw of that pass (SHADOW / PREPASS / GBUF / LIGHT / POST / OTHER) carries its CARD in the
//                                     draw record: one word of the renderer's own state cache at the draw - the engine's mesh (vertex
//                                     format, vertex / index buffer handles), material (shader, state objects, texture and sampler
//                                     slots) and pass (target handles) - dm/gfx/nr/nr_nrn.h nrn_card_of. The backend (xbg_nr_nrn.inc)
//                                     notes what the translation path recorded for the card and classes every difference.
//   DM_NRN_<PASS> = 1                 apply (step B): the same record; the backend executes a draw whose card has a row from the row.
//                                     The game thread also asks the backend's published table whether the card has a row and says
//                                     so in the tail (NRN_T_READY) - a count for the short record, nothing is written differently.
//   DM_NRN = v                        the value of every pass that has no switch of its own.
//   DM_NRN_WORDS                      what the card reads besides the fixed state words (nr_nrn.h NRN_W_*; default 3 = texture + sampler slots).
// The pass comes from the names the engine gives its targets: every resource transition (opcode 55, the shared ExecuteRenderCommand
// chain) carries the handle and "<Stage>::<Resource>"; the class of the name is kept per handle (nrn_rtmap) and the draw asks it for
// its colour target 0 and its depth target (r+0x17e8 / r+0x1828).
// Needs DM_NR=1|2 and DM_NR_DRAW=1|2 (the identity hook is where the renderer of the draw is known; the tail follows the NR-09 tail).
#include "../gfx/nr/nr_nrn.h"

void nvn_nrn_enable(int on);   // nvn_nb_draw.inc: the draw record asks dm_nrn_draw_card
static uint8_t g_nrn_mode[NRN_P_N];
static int g_nrn_any;                       // a pass is on (read by every engine draw: one load when 0)
static unsigned g_nrn_words = NRN_W_DEFAULT;
static nrn_rtmap g_nrn_rt;
static __thread uint64_t t_nrn_r;           // the renderer of the engine draw whose identity was just taken (this thread)
static struct { uint64_t name; uint32_t cls; } g_nrn_names[64];   // name pointer -> class (the thread that executes the lists)
static uint32_t g_nrn_seq[NRN_P_N];         // atomics: cards written per pass, the record's sequence number
static struct { uint64_t cards[NRN_P_N], off[NRN_P_N], no_r, ns, timed, rc55, named[NRN_RC_N], ready[NRN_P_N], oth_c0[NRN_RC_N], oth_ds[NRN_RC_N]; } g_nrnc;   // atomics
// step B part 2: the short record. nrn_pre (from the direct draw of NR-09R, after the identity, before the flush and the draw are
// recorded) takes the card of the draw from the renderer; when the pass is at 1, DM_NRN_SHORT is not 0 and the backend has published
// the card (it holds the image of the card), the draw is written SHORT (dm/gfx/nvn/nvn_nrn.inc nvn_nrn_short: the waiting state
// records are dropped, the texture binds of the flush are not recorded). dm_nrn_draw_card then hands the same card to the record.
void nvn_nrn_short(int on); void nvn_nrn_short_enable(int on); void nvn_nrn_short_stats(uint64_t out[7]);
static int g_nrn_short;                         // DM_NRN_SHORT as armed (1 unless set to 0; only with a pass at 1)
static __thread struct { uint64_t card; uint8_t valid, pass, shrt, ready; } t_nrn_pre;
static uint64_t g_nrn_pre_differs;              // atomic: =2 only - the card taken before the flush is not the card at the draw record
const void *xbg_nrn_pub(void) __attribute__((weak));   // the backend's published table of cards that have a row (xbg_nr_nrn.inc)
static const nrn_pub *g_nrn_pub;
static int nrn_on(void) { return g_nrn_any; }
// [DOS2] NR-NATIVE step D (hle_dos2_nrn_d.inc, DM_NRN_LIST; 0 = off: nrn_pre pays one load): the note of the draw from list construction
static int g_nrnd;
static const void *nrnd_take(void); static int nrnd_pass(const void *note, const uint8_t *R); static uint64_t nrnd_card(const void *note, const uint8_t *R);
static void nrnd_skip(const void *note); static void nrnd_report(uint64_t frames);
// [DOS2] NR-NATIVE step D half b (DM_NRN_REC=2): the clock at the parts of a direct draw's big-stack call (hle_dos2_nr_direct.inc nrd_job)
static int g_nrnd_rec;
static __thread uint64_t t_nrnd_tj[6]; static __thread int t_nrnd_tj_on;
static void nrnd_tj_done(void);
#define NRND_TJ0(timed) do { if (__builtin_expect(g_nrnd_rec == 2, 0) && (timed)) { t_nrnd_tj_on = 1; t_nrnd_tj[0] = nr_now(); } } while (0)
#define NRND_TJ(k) do { if (__builtin_expect(t_nrnd_tj_on, 0)) t_nrnd_tj[k] = nr_now(); } while (0)
#define NRND_TJ5() do { if (__builtin_expect(t_nrnd_tj_on, 0)) { t_nrnd_tj[5] = nr_now(); t_nrnd_tj_on = 0; nrnd_tj_done(); } } while (0)
#define NRN_NOTE(r) do { if (__builtin_expect(g_nrn_any, 0)) t_nrn_r = (r); } while (0)

// Opcode 55 (from nr_rc_pre): the handle is the resource the engine calls `name`.
static void nrn_rc55(uint64_t handle, uint64_t name) {
    if (!name) return;
    const unsigned i = (unsigned)(name >> 3) & 63u;
    int cls;
    if (g_nrn_names[i].name == name) cls = (int)g_nrn_names[i].cls;
    else {
        char text[64]; text[0] = 0;
        const module_t *m = g_nr_main; const uint64_t lo = (uint64_t)(uintptr_t)m->base, hi = lo + m->size;
        if (name >= lo && name < hi) { size_t k = 0; const char *s = (const char *)(uintptr_t)name; while (k < sizeof text - 1 && name + k < hi && s[k]) { text[k] = s[k]; k++; } text[k] = 0; }
        else nr_peek_text(text, sizeof text, name, -1);
        cls = nrn_name_class(text);
        g_nrn_names[i].cls = (uint32_t)cls; g_nrn_names[i].name = name;
    }
    NR_INC(g_nrnc.rc55); NR_INC(g_nrnc.named[cls]);
    if (cls != NRN_RC_NONE) nrn_rt_put(&g_nrn_rt, handle, cls);
}

// The card of the engine draw whose record is being written (nvn_nb_draw.inc nbd_draw, same thread, right after the identity was
// taken). 1 = *card / *pass / *seq are the tail; 0 = no tail (the pass is off, or no renderer was noted for this draw).
// From nrd_draw_take (this thread; R = the renderer of the draw about to be recorded directly).
static void nrn_pre(const uint8_t *R) {
    t_nrn_pre.valid = 0;
    const int ld = __builtin_expect(g_nrnd, 0);                              // [DOS2] NR-NATIVE step D: the pass and the card from the note of list construction
    const void *note = ld ? nrnd_take() : NULL;                               // [DOS2] NR-NATIVE step D
    const int p = ld ? nrnd_pass(note, R) : nrn_pass_of_r(R, &g_nrn_rt);      // [DOS2] NR-NATIVE step D
    if (!g_nrn_mode[p]) { if (ld) nrnd_skip(note); return; }                  // [DOS2] NR-NATIVE step D
    t_nrn_pre.card = ld ? nrnd_card(note, R) : nrn_card_of(R, g_nrn_words); t_nrn_pre.pass = (uint8_t)p; t_nrn_pre.valid = 1;   // [DOS2] NR-NATIVE step D
    t_nrn_pre.ready = g_nrn_pub && nrn_pub_ready(g_nrn_pub, t_nrn_pre.card);
    t_nrn_pre.shrt = g_nrn_short && g_nrn_mode[p] == 1 && t_nrn_pre.ready;
    if (t_nrn_pre.shrt) nvn_nrn_short(1);
}
#define NRN_PRE(R) do { if (__builtin_expect(g_nrn_any, 0)) nrn_pre(R); } while (0)
int nvn_nrn_short_left(void);   // nvn_nrn.inc: 1 = a short flag no draw record took (cleared)
#define NRN_POST() do { if (__builtin_expect(g_nrn_short, 0) && nvn_nrn_short_left()) LOG("【红】[nr] native: a draw was decided short and no draw record was written - its state records and texture binds are lost"); } while (0)
int dm_nrn_draw_card(uint64_t *card, unsigned *pass, uint32_t *seq, unsigned *rsv) {
    const uint64_t r = t_nrn_r;
    t_nrn_r = 0;
    if (!r) { NR_INC(g_nrnc.no_r); t_nrn_pre.valid = 0; return 0; }
    const uint8_t *R = (const uint8_t *)(uintptr_t)r;
    const int pre = t_nrn_pre.valid; t_nrn_pre.valid = 0;
    const int p = pre ? t_nrn_pre.pass : nrn_pass_of_r(R, &g_nrn_rt);
    if (!g_nrn_mode[p]) { NR_INC(g_nrnc.off[p]); return 0; }
    const uint32_t n = __atomic_add_fetch(&g_nrn_seq[p], 1, __ATOMIC_RELAXED);
    const int timed = (n & 63u) == 0;
    const uint64_t t0 = timed ? nr_now() : 0;
    *pass = (unsigned)p; *seq = n;
    if (pre) {   // the card nrn_pre took before the flush (=2: the flush must not have changed a byte the card reads)
        *card = t_nrn_pre.card; *rsv = t_nrn_pre.ready ? NRN_T_READY : 0u;
        if (g_nrn_mode[p] == 2 && nrn_card_of(R, g_nrn_words) != *card) NR_INC(g_nrn_pre_differs);
    } else { *card = nrn_card_of(R, g_nrn_words); *rsv = g_nrn_pub && nrn_pub_ready(g_nrn_pub, *card) ? NRN_T_READY : 0u; }
    if (*rsv) NR_INC(g_nrnc.ready[p]);
    if (p == NRN_P_OTHER) {   // what the targets of an unclassed draw are called (the line of the report)
        uint64_t c0, ds; memcpy(&c0, R + NRN_R_RT, 8); memcpy(&ds, R + NRN_R_DEPTH_RT, 8);
        NR_INC(g_nrnc.oth_c0[nrn_rt_get(&g_nrn_rt, c0)]); NR_INC(g_nrnc.oth_ds[nrn_rt_get(&g_nrn_rt, ds)]);
    }
    if (timed) { __atomic_fetch_add(&g_nrnc.ns, nr_now() - t0, __ATOMIC_RELAXED); NR_INC(g_nrnc.timed); }
    NR_INC(g_nrnc.cards[p]);
    return 1;
}

static const char *nrn_getenv(const char *k) { return getenv(k); }
// From dm_dos2_nr_install, before guest code runs. draw_on: DM_NR_DRAW is armed (the tail needs the NR-09 tail).
static void nrn_arm(int draw_on) {
    unsigned bad = 0;
    uint8_t mode[NRN_P_N];
    const unsigned any = nrn_env_modes(nrn_getenv, 0, mode, &bad);   // 0 on every device unless asked (the backend's policy function xbg_nrn_default says the same)
    if (bad) LOG("【红】[nr] native: a DM_NRN switch is not 0 / 1 / 2 (mask %#x: bits 0..5 = SHADOW PREPASS GBUF LIGHT POST OTHER, bit 6 = DM_NRN): those are off", bad);
    if (!any) return;
    char on[96]; on[0] = 0;
    int apply = 0;
    for (int p = 0; p < NRN_P_N; p++) {
        if (mode[p] == 1) apply = 1;
        if (mode[p]) { if (on[0]) strncat(on, " ", sizeof on - strlen(on) - 1); strncat(on, nrn_pass_name(p), sizeof on - strlen(on) - 1); }
    }
    if (!on[0]) return;
    if (!draw_on) { LOG("【红】[nr] native: DM_NRN (%s) needs DM_NR_DRAW=1|2 (the identity hook and the NR-09 tail of the draw record): off", on); return; }
    { const char *w = getenv("DM_NRN_WORDS"); if (w && *w) g_nrn_words = (unsigned)atoi(w) & 63u; }
    memcpy(g_nrn_mode, mode, sizeof g_nrn_mode);
    g_nrn_pub = xbg_nrn_pub ? (const nrn_pub *)xbg_nrn_pub() : NULL;
    { const char *sh = getenv("DM_NRN_SHORT"); g_nrn_short = apply && g_nrn_pub && !(sh && *sh && atoi(sh) == 0); }
    if (g_nrn_short) {   // the image goes into State when the record is executed: only while the backend executes a recording in stream order and nothing else keeps a copy of State
        static const char *const no[] = {"DM_NB_ORDER", "DM_NB_RELOC", "DM_NB_MX", "DM_NR_Z5", "DM_NR_Z8"};
        for (unsigned i = 0; i < sizeof no / sizeof no[0]; i++) { const char *o = getenv(no[i]); if (o && atoi(o)) {
            LOG("[nr] native: %s=%s executes parts of a recording in another order or keeps its own copy of the state - draws are not written short (DM_NRN_SHORT off; the rows still answer)", no[i], o);
            g_nrn_short = 0; break; } }
    }
    if (g_nrn_short) nvn_nrn_short_enable(1);
    nvn_nrn_enable(1);
    __atomic_store_n(&g_nrn_any, 1, __ATOMIC_RELEASE);
    LOG("[nr] native (NR-NATIVE, front end): passes %s (1 = apply, 2 = verify; the record is the same in both%s) - every engine draw of those passes writes its card (one word of the renderer's state cache: "
        "raster / blend / depth-stencil / vertex format / vertex and index buffer / shader / target handles, stencil ref, primitive, alpha ref%s%s%s%s%s%s; DM_NRN_WORDS=%u) "
        "and its pass after the NR-09 tail of the draw record; nothing else is written or executed differently",
        on, apply ? ": the backend executes a draw from its card's row" : "", g_nrn_words & NRN_W_TEX ? ", texture slots" : "", g_nrn_words & NRN_W_SMP ? ", sampler slots" : "", g_nrn_words & NRN_W_VBDIRTY ? ", +0x1788 stream words" : "",
        g_nrn_words & NRN_W_IBFIRST ? ", first index" : "", g_nrn_words & NRN_W_REN ? ", conditional render" : "", g_nrn_words & NRN_W_SHREC ? ", shader record" : "", g_nrn_words);
}
// From nr_report (every 10 s).
static void nrn_report(uint64_t frames) {
    if (!g_nrn_any) return;
    nrnd_report(frames);   // [DOS2] NR-NATIVE step D
    uint64_t c[NRN_P_N], o[NRN_P_N], nm[NRN_RC_N], total = 0;
    for (int p = 0; p < NRN_P_N; p++) { c[p] = __atomic_exchange_n(&g_nrnc.cards[p], 0, __ATOMIC_RELAXED); o[p] = __atomic_exchange_n(&g_nrnc.off[p], 0, __ATOMIC_RELAXED); total += c[p]; }
    for (int k = 0; k < NRN_RC_N; k++) nm[k] = __atomic_exchange_n(&g_nrnc.named[k], 0, __ATOMIC_RELAXED);
    const uint64_t no_r = __atomic_exchange_n(&g_nrnc.no_r, 0, __ATOMIC_RELAXED), ns = __atomic_exchange_n(&g_nrnc.ns, 0, __ATOMIC_RELAXED),
                   tn = __atomic_exchange_n(&g_nrnc.timed, 0, __ATOMIC_RELAXED), rc = __atomic_exchange_n(&g_nrnc.rc55, 0, __ATOMIC_RELAXED);
    const double f = frames ? (double)frames : 1.;
    uint64_t rd[NRN_P_N], oc[NRN_RC_N], od[NRN_RC_N];
    for (int p = 0; p < NRN_P_N; p++) rd[p] = __atomic_exchange_n(&g_nrnc.ready[p], 0, __ATOMIC_RELAXED);
    for (int k = 0; k < NRN_RC_N; k++) { oc[k] = __atomic_exchange_n(&g_nrnc.oth_c0[k], 0, __ATOMIC_RELAXED); od[k] = __atomic_exchange_n(&g_nrnc.oth_ds[k], 0, __ATOMIC_RELAXED); }
    { uint64_t ss[7]; nvn_nrn_short_stats(ss); const uint64_t pd = __atomic_exchange_n(&g_nrn_pre_differs, 0, __ATOMIC_RELAXED);
      if (g_nrn_short || pd)
        LOG("[nr] native short (front end, DM_NRN_SHORT=%d, step B part 2): short records %.0f a frame; state records that waited for their draw %.0f a frame - dropped by a short draw %.0f, "
            "written %.0f; texture binds not recorded %.0f a frame; state records too long to wait %llu; cards taken before the flush that differ at the draw record (=2 only) %llu",
            g_nrn_short, ss[4] / f, ss[0] / f, ss[2] / f, ss[1] / f, ss[3] / f, (unsigned long long)ss[5], (unsigned long long)pd);
      if (pd) LOG("【红】[nr] native: the flush of a draw changed bytes the card reads (%llu draws) - a card taken before the flush is not the card of the record", (unsigned long long)pd); }
    LOG("[nr] native cards (front end, step B): the card had a published row when its record was written - SHADOW %.0f PREPASS %.0f GBUF %.0f LIGHT %.0f POST %.0f OTHER %.0f a frame; "
        "draws classed OTHER by the class of colour target 0 (never named / shadow / g-buffer / hdr / ldr / aux / another name): %llu / %llu / %llu / %llu / %llu / %llu / %llu, "
        "by the class of their depth target: %llu / %llu / %llu / %llu / %llu / %llu / %llu",
        rd[0] / f, rd[1] / f, rd[2] / f, rd[3] / f, rd[4] / f, rd[5] / f,
        (unsigned long long)oc[0], (unsigned long long)oc[1], (unsigned long long)oc[2], (unsigned long long)oc[3], (unsigned long long)oc[4], (unsigned long long)oc[5], (unsigned long long)oc[6],
        (unsigned long long)od[0], (unsigned long long)od[1], (unsigned long long)od[2], (unsigned long long)od[3], (unsigned long long)od[4], (unsigned long long)od[5], (unsigned long long)od[6]);
    LOG("[nr] native cards (front end, last 10 s): %.0f a frame - SHADOW %.0f PREPASS %.0f GBUF %.0f LIGHT %.0f POST %.0f OTHER %.0f; engine draws of a pass that is off: "
        "%.0f / %.0f / %.0f / %.0f / %.0f / %.0f a frame; draw records without a renderer in hand %llu; %llu ns a card (1 in 64, %llu timed); resource transitions %llu "
        "(names: shadow %llu, g-buffer %llu, hdr %llu, ldr %llu, aux %llu, other %llu, unreadable %llu)",
        total / f, c[0] / f, c[1] / f, c[2] / f, c[3] / f, c[4] / f, c[5] / f, o[0] / f, o[1] / f, o[2] / f, o[3] / f, o[4] / f, o[5] / f, (unsigned long long)no_r,
        (unsigned long long)(tn ? ns / tn : 0), (unsigned long long)tn, (unsigned long long)rc, (unsigned long long)nm[NRN_RC_SHADOW], (unsigned long long)nm[NRN_RC_GBUF],
        (unsigned long long)nm[NRN_RC_HDR], (unsigned long long)nm[NRN_RC_LDR], (unsigned long long)nm[NRN_RC_AUX], (unsigned long long)nm[NRN_RC_NAMED], (unsigned long long)nm[NRN_RC_NONE]);
}
