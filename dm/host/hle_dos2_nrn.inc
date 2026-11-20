// [DOS2] NR-NATIVE: the draw as the engine holds it, above NVN (DM_NRN / DM_NRN_<PASS>, default 0; docs/dos2/nr/NR-NATIVE.md). Included by
// hle_dos2_nr.c ahead of its hooks (they call NRN_NOTE). main = main.raw of update v786432, build 4979b200.
//   every DM_NRN_<PASS> = 0 / unset   nothing: no word is read from the renderer, the draw record is byte for byte the old one
//                                     (nvn_nb_draw.inc pays one load).
//   DM_NRN_<PASS> = 2                 an engine draw of that pass (SHADOW / PREPASS / GBUF / LIGHT / POST / OTHER) carries its CARD in the
//                                     draw record: one word of the renderer's own state cache at the draw - the engine's mesh (vertex
//                                     format, vertex / index buffer handles), material (shader, state objects, texture and sampler
//                                     slots) and pass (target handles) - dm/gfx/nr/nr_nrn.h nrn_card_of. The backend (xbg_nr_nrn.inc)
//                                     notes what the translation path recorded for the card and classes every difference.
//   DM_NRN_<PASS> = 1                 refused (red line, the pass is off): the replay of a draw from its card is not built.
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
static struct { uint64_t cards[NRN_P_N], off[NRN_P_N], no_r, ns, timed, rc55, named[NRN_RC_N]; } g_nrnc;   // atomics
static int nrn_on(void) { return g_nrn_any; }
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
int dm_nrn_draw_card(uint64_t *card, unsigned *pass, uint32_t *seq) {
    const uint64_t r = t_nrn_r;
    t_nrn_r = 0;
    if (!r) { NR_INC(g_nrnc.no_r); return 0; }
    const uint8_t *R = (const uint8_t *)(uintptr_t)r;
    const int p = nrn_pass_of_r(R, &g_nrn_rt);
    if (!g_nrn_mode[p]) { NR_INC(g_nrnc.off[p]); return 0; }
    const uint32_t n = __atomic_add_fetch(&g_nrn_seq[p], 1, __ATOMIC_RELAXED);
    const int timed = (n & 63u) == 0;
    const uint64_t t0 = timed ? nr_now() : 0;
    *card = nrn_card_of(R, g_nrn_words); *pass = (unsigned)p; *seq = n;
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
    for (int p = 0; p < NRN_P_N; p++) {
        if (mode[p] == 1) {
            LOG("【红】[nr] native: DM_NRN_%s=1 is refused - the replay of a draw from its card is not built (docs/dos2/nr/NR-NATIVE.md section 4): the pass is off, use =2", nrn_pass_name(p));
            mode[p] = 0;
        }
        if (mode[p]) { if (on[0]) strncat(on, " ", sizeof on - strlen(on) - 1); strncat(on, nrn_pass_name(p), sizeof on - strlen(on) - 1); }
    }
    if (!on[0]) return;
    if (!draw_on) { LOG("【红】[nr] native: DM_NRN (%s) needs DM_NR_DRAW=1|2 (the identity hook and the NR-09 tail of the draw record): off", on); return; }
    { const char *w = getenv("DM_NRN_WORDS"); if (w && *w) g_nrn_words = (unsigned)atoi(w) & 63u; }
    memcpy(g_nrn_mode, mode, sizeof g_nrn_mode);
    nvn_nrn_enable(1);
    __atomic_store_n(&g_nrn_any, 1, __ATOMIC_RELEASE);
    LOG("[nr] native (NR-NATIVE, front end): passes %s at 2 (verify) - every engine draw of those passes writes its card (one word of the renderer's state cache: "
        "raster / blend / depth-stencil / vertex format / vertex and index buffer / shader / target handles, stencil ref, primitive, alpha ref%s%s%s%s%s%s; DM_NRN_WORDS=%u) "
        "and its pass after the NR-09 tail of the draw record; nothing else is written or executed differently",
        on, g_nrn_words & NRN_W_TEX ? ", texture slots" : "", g_nrn_words & NRN_W_SMP ? ", sampler slots" : "", g_nrn_words & NRN_W_VBDIRTY ? ", +0x1788 stream words" : "",
        g_nrn_words & NRN_W_IBFIRST ? ", first index" : "", g_nrn_words & NRN_W_REN ? ", conditional render" : "", g_nrn_words & NRN_W_SHREC ? ", shader record" : "", g_nrn_words);
}
// From nr_report (every 10 s).
static void nrn_report(uint64_t frames) {
    if (!g_nrn_any) return;
    uint64_t c[NRN_P_N], o[NRN_P_N], nm[NRN_RC_N], total = 0;
    for (int p = 0; p < NRN_P_N; p++) { c[p] = __atomic_exchange_n(&g_nrnc.cards[p], 0, __ATOMIC_RELAXED); o[p] = __atomic_exchange_n(&g_nrnc.off[p], 0, __ATOMIC_RELAXED); total += c[p]; }
    for (int k = 0; k < NRN_RC_N; k++) nm[k] = __atomic_exchange_n(&g_nrnc.named[k], 0, __ATOMIC_RELAXED);
    const uint64_t no_r = __atomic_exchange_n(&g_nrnc.no_r, 0, __ATOMIC_RELAXED), ns = __atomic_exchange_n(&g_nrnc.ns, 0, __ATOMIC_RELAXED),
                   tn = __atomic_exchange_n(&g_nrnc.timed, 0, __ATOMIC_RELAXED), rc = __atomic_exchange_n(&g_nrnc.rc55, 0, __ATOMIC_RELAXED);
    const double f = frames ? (double)frames : 1.;
    LOG("[nr] native cards (front end, last 10 s): %.0f a frame - SHADOW %.0f PREPASS %.0f GBUF %.0f LIGHT %.0f POST %.0f OTHER %.0f; engine draws of a pass that is off: "
        "%.0f / %.0f / %.0f / %.0f / %.0f / %.0f a frame; draw records without a renderer in hand %llu; %llu ns a card (1 in 64, %llu timed); resource transitions %llu "
        "(names: shadow %llu, g-buffer %llu, hdr %llu, ldr %llu, aux %llu, other %llu, unreadable %llu)",
        total / f, c[0] / f, c[1] / f, c[2] / f, c[3] / f, c[4] / f, c[5] / f, o[0] / f, o[1] / f, o[2] / f, o[3] / f, o[4] / f, o[5] / f, (unsigned long long)no_r,
        (unsigned long long)(tn ? ns / tn : 0), (unsigned long long)tn, (unsigned long long)rc, (unsigned long long)nm[NRN_RC_SHADOW], (unsigned long long)nm[NRN_RC_GBUF],
        (unsigned long long)nm[NRN_RC_HDR], (unsigned long long)nm[NRN_RC_LDR], (unsigned long long)nm[NRN_RC_AUX], (unsigned long long)nm[NRN_RC_NAMED], (unsigned long long)nm[NRN_RC_NONE]);
}
