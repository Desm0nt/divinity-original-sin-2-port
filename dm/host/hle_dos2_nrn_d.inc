// [DOS2] NR-NATIVE step D (DM_NRN_LIST, default 0; docs/dos2/nr/NR-NATIVE-D.md): the card and the pass of an engine draw from LIST
// CONSTRUCTION. Included by hle_dos2_nr.c right after hle_dos2_nrn.inc (uses its g_nrn_words / g_nrn_rt / g_nrn_mode, NR_HOOK / NR_A /
// nr_fn, NR_INC, nr_now, dn_hook / dn_words_ok). The model and the note are in dm/gfx/nr/nr_nrn_d.h (pure C, host tests
// tests/nr_nrn_d_test.c and tests/nr_nrn_d_tsan_test.cpp). main = main.raw of update v786432, build 4979b200.
//   DM_NRN_LIST=0 / unset  nothing: no hook goes in, the lists are byte for byte the engine's, nrn_pre pays one load.
//   DM_NRN_LIST=2          verify. The wrappers of the engine's "draw yourself" functions (method 23 of the draw objects: the common
//                          0x90cbf0 behind 0x90e1f0, and 0xb7e020 / 0xb89330 / 0xb87470 / 0xc6cc50 / 0xc7f590 / 0xc82c80) put one
//                          packet of our own (opcode 0x40, 0x38 bytes) in front of the group the function appends and fill it when
//                          the function has returned: the pass of the context, the group's draw packet, the hashes of the group.
//                          Three more hooks bracket the builders whose context names a pass: 0xb15090 (a sun cascade's list),
//                          0xb1be00 (the pre-pass task), 0xb1c580 (the G-buffer stage's list). At execution the shared
//                          ExecuteRenderCommand hook gives the packet to nrnd_exec_note (the engine's switch never sees it). Every
//                          draw still hashes the renderer's cache for its card and takes its pass from the names of its targets;
//                          the note's pass and the chain's card (nr_nrn_d.h) are compared with them: BAD = red line (first 16).
//   DM_NRN_LIST=1          apply. The note's pass is the pass of the draw (when the context names one); the card comes from the
//                          memo of the chain when it answers (no hash of the cache); 1 draw in 64 is hashed all the same and
//                          compared: a difference is a red line and the memo answers nothing for the rest of the run.
// Needs DM_NRN_<PASS> (a card is taken only for a pass that is on), DM_NR_DIRECT=1 (the card is taken in the direct draw) and the
// shared ExecuteRenderCommand hook. Threads: the wrappers run on the engine's list-building threads and touch only the list of their
// context argument (one task builds a list) and thread-locals; the run and the memo belong to the thread that executes lists.
#include "../gfx/nr/nr_nrn_d.h"

int dn_install_rc_note(uint64_t (*note)(uint64_t list, uint64_t pkt));   // hle_dos2_native.c
enum { NRND_ALLOC = 0x941720, NRND_AUDIT = 64 };
enum { NRND_B_NONE, NRND_B_CASCADE, NRND_B_PREPASS, NRND_B_GBUF };
// g_nrnd (the mode as armed) is declared by hle_dos2_nrn.inc: nrn_pre reads it.
static int g_nrnd_memo_off;                      // atomic: the audit of =1 found a wrong card - the memo answers nothing any more
static uint64_t g_nrnd_base, g_nrnd_size;
static uint64_t g_nrnd_epoch;                    // atomic: textures created + destroyed (a texture packet names a texture record)
typedef uint64_t (*nrnd_alloc_fn)(uint64_t list, uint64_t size, uint64_t align);
static nrnd_alloc_fn g_nrnd_alloc;
static __thread struct { uint8_t kind, sub; } t_nrnd_build;   // the builder this thread is inside (NRND_B_*; saved / restored by the bracket hooks)
static __thread nrnd_run t_nrnd;
static __thread nrnd_memo *t_nrnd_memo;
static __thread uint32_t t_nrnd_n;
enum { NRNDC_NOTES, NRNDC_NOLIST, NRNDC_PART, NRNDC_NODRAW, NRNDC_EXEC, NRNDC_DRAWS, NRNDC_NONOTE, NRNDC_PASS_NOTE, NRNDC_PASS_SAME, NRNDC_PASS_BAD, NRNDC_CHAIN,
       NRNDC_HIT, NRNDC_LEARN, NRNDC_SAME, NRNDC_BAD, NRNDC_AUDIT, NRNDC_NS_MEMO, NRNDC_T_MEMO, NRNDC_NS_FULL, NRNDC_T_FULL, NRNDC_SCAN_NS, NRNDC_SCAN_T, NRNDC_N };
static uint64_t g_nrndc[NRNDC_N], g_nrnd_kind[NRND_K_N], g_nrnd_pm[NRN_P_N][NRN_P_N];   // atomics; pm[note's pass][pass by targets]
static unsigned g_nrnd_red;
void dm_nrnd_tex_epoch(void);
void dm_nrnd_tex_epoch(void) { __atomic_fetch_add(&g_nrnd_epoch, 1, __ATOMIC_RELAXED); }

// ---- list construction (any thread) ----
typedef struct { uint8_t *note; uint64_t list, chunk; uint32_t at, cnt, seq; uint64_t t0; } nrnd_open;   // cnt / seq: [DOS2] NR-NATIVE step D half c
static inline uint64_t nrnd_r64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static inline uint32_t nrnd_r32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
#include "hle_dos2_nrn_c.inc"   // [DOS2] NR-NATIVE step D half c: DM_NRN_PKT (one packet instead of the group; nothing while it is 0)
// Before the engine's function: the note packet in front of the group. 0 = no note (the list calls NVN at once, no memory).
static int nrnd_begin(nrnd_open *o, int kind, uint64_t obj, uint64_t ctx) {
    const uint64_t list = ctx ? nrnd_r64(ctx) : 0;
    if (!list || !*(const uint8_t *)(uintptr_t)(list + 0x30)) { NR_INC(g_nrndc[NRNDC_NOLIST]); return 0; }   // list +0x30: "record packets" (else the functions call NVN directly)
    const uint32_t cflags = nrnd_r32(ctx + 8), cmask = nrnd_r32(ctx + 0xc);
    const unsigned b = t_nrnd_build.kind;
    o->t0 = !(t_nrnd_n++ & 63) ? nr_now() : 0;
    uint8_t *p = (uint8_t *)(uintptr_t)g_nrnd_alloc(list, NRND_NOTE_BYTES, NRND_NOTE_ALIGN);   // the engine's AllocCommand: the packet is the list's
    if (!p) return 0;
    nrnd_note n;
    nrnd_note_init(&n, kind, cflags, cmask, b == NRND_B_CASCADE ? (uint32_t)NRND_VT_CASCADE : b ? (uint32_t)NRND_VT_GBUF_STAGE : 0u, t_nrnd_build.sub, obj);
    memcpy(p, &n, sizeof n);   // flags 0: not filled yet
    o->note = p; o->list = list; o->chunk = nrnd_r64(list + 8); o->at = nrnd_r32(o->chunk + 0xc);
    o->cnt = nrnd_r32(o->chunk + 0x10); o->seq = t_nrnd_n;   // [DOS2] NR-NATIVE step D half c: the chunk's packet count with the note in it
    return 1;
}
// After it: the packets it appended, from the byte after the note to the list's cursor (chunk: +0 data, +0xc used, +0x18 next).
static void nrnd_end(const nrnd_open *o) {
    nrnd_seg seg[8]; unsigned ns = 0; int whole = 1;
    const uint64_t cur = nrnd_r64(o->list + 8);
    uint64_t c = o->chunk; uint32_t at = o->at;
    for (;;) {
        if (ns == 8 || !c) { whole = 0; break; }
        seg[ns].base = (const uint8_t *)(uintptr_t)nrnd_r64(c); seg[ns].at = at; seg[ns].end = nrnd_r32(c + 0xc); ns++;
        if (c == cur) break;
        c = nrnd_r64(c + 0x18); at = 0;
    }
    nrnd_note n; memcpy(&n, o->note, sizeof n);
    if (whole) nrnd_scan(&n, seg, ns); else n.flags = NRND_F_DONE;
    if (__builtin_expect(g_nrnk, 0)) nrnk_built(&n, o->chunk, o->cnt, whole ? ns : 0, o->seq);   // [DOS2] NR-NATIVE step D half c: the group becomes the note's payload (=1) / would (=2)
    memcpy(o->note, &n, sizeof n);
    NR_INC(g_nrndc[NRNDC_NOTES]); NR_INC(g_nrnd_kind[n.kind < NRND_K_N ? n.kind : 0]);
    if (!(n.flags & NRND_F_FULL)) NR_INC(g_nrndc[NRNDC_PART]); else if (!(n.flags & NRND_F_DRAW)) NR_INC(g_nrndc[NRNDC_NODRAW]);
    if (o->t0) { __atomic_fetch_add(&g_nrndc[NRNDC_SCAN_NS], nr_now() - o->t0, __ATOMIC_RELAXED); NR_INC(g_nrndc[NRNDC_SCAN_T]); }
}
static nr_fn g_o_d23a, g_o_d23b, g_o_d23c, g_o_d23d, g_o_d23e, g_o_d23f, g_o_d23g, g_o_dcas, g_o_dpre, g_o_dgb;
// 0x90cbf0(queue entry, context): the object is the entry's first word. The others: (object, queue entry, context).
#define NRND_WRAP(name, orig, kind, OBJ, CTX) NR_HOOK name(NR_A) { \
    nrnd_open o_; const int on_ = nrnd_begin(&o_, (kind), (OBJ), (CTX)); \
    const uint64_t v = orig(NR_F); \
    if (on_) nrnd_end(&o_); \
    return v; }
NRND_WRAP(nr_h_d23a, g_o_d23a, NRND_K_COMMON, a0 ? nrnd_r64(a0) : 0, a1)
NRND_WRAP(nr_h_d23b, g_o_d23b, NRND_K_INST, a0, a2)
NRND_WRAP(nr_h_d23c, g_o_d23c, NRND_K_B89330, a0, a2)
NRND_WRAP(nr_h_d23d, g_o_d23d, NRND_K_B87470, a0, a2)
NRND_WRAP(nr_h_d23e, g_o_d23e, NRND_K_C6CC50, a0, a2)
NRND_WRAP(nr_h_d23f, g_o_d23f, NRND_K_C7F590, a0, a2)
NRND_WRAP(nr_h_d23g, g_o_d23g, NRND_K_C82C80, a0, a2)
// The builders whose context names a pass. A thread that waits for a job group inside one may run another builder's task, so the
// bracket is saved and put back.
#define NRND_BRACKET(name, orig, KIND, SUB) NR_HOOK name(NR_A) { \
    const uint8_t bk_ = t_nrnd_build.kind, bs_ = t_nrnd_build.sub; \
    t_nrnd_build.kind = (KIND); t_nrnd_build.sub = (uint8_t)(SUB); \
    const uint64_t v = orig(NR_F); \
    t_nrnd_build.kind = bk_; t_nrnd_build.sub = bs_; \
    return v; }
NRND_BRACKET(nr_h_dcas, g_o_dcas, NRND_B_CASCADE, a0 ? nrnd_r64(a0 + 0xb8) + 2 * nrnd_r64(a0 + 0xb0) : 0)   // 0xb15090(cascade stage, ...): the tile's row + 2 x column
NRND_BRACKET(nr_h_dpre, g_o_dpre, NRND_B_PREPASS, 0)                                                         // 0xb1be00(the pre-pass task)
NRND_BRACKET(nr_h_dgb, g_o_dgb, NRND_B_GBUF, 0)                                                              // 0xb1c580(DeferredBufferStage, ...)
static const struct nrnd_site { uint32_t off; uint32_t pro[4]; const char *name; void *hook; nr_fn *orig; } k_nrnd_site[] = {
    {0x90cbf0, {0xd10343ff /* sub sp,sp,#0xd0 */, 0xa90867fa, 0xa9095ff8, 0xa90a57f6}, "draw yourself (common)", (void *)nr_h_d23a, &g_o_d23a},
    {0xb7e020, {0xd10343ff /* sub sp,sp,#0xd0 */, 0xa90867fa, 0xa9095ff8, 0xa90a57f6}, "draw yourself (instanced mesh)", (void *)nr_h_d23b, &g_o_d23b},
    {0xb89330, {0xd10303ff /* sub sp,sp,#0xc0 */, 0xa90767fa, 0xa9085ff8, 0xa90957f6}, "draw yourself (0xb89330)", (void *)nr_h_d23c, &g_o_d23c},
    {0xb87470, {0xd102c3ff /* sub sp,sp,#0xb0 */, 0xa9075ff8, 0xa90857f6, 0xa9094ff4}, "draw yourself (0xb87470)", (void *)nr_h_d23d, &g_o_d23d},
    {0xc6cc50, {0xd10303ff /* sub sp,sp,#0xc0 */, 0xf9003bf9, 0xa9085ff8, 0xa90957f6}, "draw yourself (0xc6cc50)", (void *)nr_h_d23e, &g_o_d23e},
    {0xc7f590, {0xd102c3ff /* sub sp,sp,#0xb0 */, 0xf9003bf7, 0xa90857f6, 0xa9094ff4}, "draw yourself (0xc7f590)", (void *)nr_h_d23f, &g_o_d23f},
    {0xc82c80, {0xd10383ff /* sub sp,sp,#0xe0 */, 0xa90967fa, 0xa90a5ff8, 0xa90b57f6}, "draw yourself (0xc82c80)", (void *)nr_h_d23g, &g_o_d23g},
    {0xb15090, {0xd10483ff /* sub sp,sp,#0x120 */, 0xa90c6ffc, 0xa90d67fa, 0xa90e5ff8}, "cascade list builder", (void *)nr_h_dcas, &g_o_dcas},
    {0xb1be00, {0xd102c3ff /* sub sp,sp,#0xb0 */, 0xf9003bf7, 0xa90857f6, 0xa9094ff4}, "pre-pass list builder", (void *)nr_h_dpre, &g_o_dpre},
    {0xb1c580, {0xd103c3ff /* sub sp,sp,#0xf0 */, 0xfd003bea, 0x6d0823e9, 0xa9096ffc}, "G-buffer stage list builder", (void *)nr_h_dgb, &g_o_dgb},
};
enum { NRND_SITES = sizeof k_nrnd_site / sizeof k_nrnd_site[0] };

// ---- execution (the thread that executes lists) ----
// The note packet, from the shared ExecuteRenderCommand hook (the engine's handler does not run). Returns its length.
static uint64_t nrnd_exec_note(uint64_t list, uint64_t pkt) {
    nrnd_run_note(&t_nrnd, list, (const nrnd_note *)(uintptr_t)pkt);
    NR_INC(g_nrndc[NRNDC_EXEC]);
    if (__builtin_expect(g_nrnk, 0)) return nrnk_note(list, pkt);   // [DOS2] NR-NATIVE step D half c: a merged group is run here; the length covers it
    return NRND_NOTE_BYTES;
}
// Every other packet, from the opcode-55 handler's first line (nr_rc_pre: it runs before the engine's handler or the native one).
static inline void nrnd_pkt(unsigned op, uint64_t pkt) { nrnd_run_pkt(&t_nrnd, op, pkt); }
#define NRND_PKT(op, pkt) do { if (__builtin_expect(g_nrnd, 0)) nrnd_pkt((op), (pkt)); } while (0)
// nrn_pre: the note of the draw being recorded directly (NULL: none).
static const void *nrnd_take(void) {
    const nrnd_note *n = nrnd_run_take(&t_nrnd);
    NR_INC(g_nrndc[NRNDC_DRAWS]);
    if (!n) NR_INC(g_nrndc[NRNDC_NONOTE]);
    return n;
}
// The pass of the draw: the note's when its context names one (=1), the class of the targets otherwise; =2 compares the two.
static int nrnd_pass(const void *note, const uint8_t *R) {
    const nrnd_note *n = (const nrnd_note *)note;
    const int pn = n ? nrnd_pass_of(n->cflags, n->stage) : -1;
    if (pn >= 0 && g_nrnd == 1) { NR_INC(g_nrndc[NRNDC_PASS_NOTE]); return pn; }
    const int pt = nrn_pass_of_r(R, &g_nrn_rt);
    if (pn >= 0) {
        NR_INC(g_nrndc[NRNDC_PASS_NOTE]); NR_INC(g_nrnd_pm[pn][pt]);
        if (pn == pt) NR_INC(g_nrndc[NRNDC_PASS_SAME]);
        else { NR_INC(g_nrndc[NRNDC_PASS_BAD]);   // not an error of the note: the names of the targets may class the pass otherwise (the line says which)
            if (__atomic_fetch_add(&g_nrnd_red, 1, __ATOMIC_RELAXED) < 16)
                LOG("[nr] native list: the context of a draw says pass %s (flags %#x, mask %#x, cascade %u), the names of its targets say %s", nrn_pass_name(pn), n->cflags,
                    (unsigned)n->cmask, (unsigned)n->sub, nrn_pass_name(pt)); }
    }
    return pt;
}
// The card of the draw (its pass is on). =2: the cache's hash, with the chain's prediction compared; =1: the memo's answer when the
// chain holds and the memo has the key, else the hash (which teaches the memo).
static uint64_t nrnd_card(const void *note, const uint8_t *R) {
    const nrnd_note *n = (const nrnd_note *)note;
    nrnd_memo *m = t_nrnd_memo;
    if (__builtin_expect(!m, 0)) m = t_nrnd_memo = (nrnd_memo *)calloc(1, sizeof *m);
    const int off = !m || __atomic_load_n(&g_nrnd_memo_off, __ATOMIC_RELAXED);
    const uint64_t key = off ? 0 : nrnd_chain_key(&t_nrnd, n, __atomic_load_n(&g_nrnd_epoch, __ATOMIC_RELAXED));
    const uint32_t k = t_nrnd_n++;
    const int timed = !(k & 63);
    const uint64_t t0 = timed ? nr_now() : 0;
    uint64_t pred = key ? nrnd_memo_get(m, key) : 0, card;
    if (key) NR_INC(g_nrndc[NRNDC_CHAIN]);
    if (pred) NR_INC(g_nrndc[NRNDC_HIT]);
    const int audit = pred && g_nrnd == 1 && !(k % NRND_AUDIT);
    if (pred && g_nrnd == 1 && !audit) {
        card = pred;
        if (timed) { __atomic_fetch_add(&g_nrndc[NRNDC_NS_MEMO], nr_now() - t0, __ATOMIC_RELAXED); NR_INC(g_nrndc[NRNDC_T_MEMO]); }
    } else {
        card = nrn_card_of(R, g_nrn_words);
        if (timed) { __atomic_fetch_add(&g_nrndc[NRNDC_NS_FULL], nr_now() - t0, __ATOMIC_RELAXED); NR_INC(g_nrndc[NRNDC_T_FULL]); }
        if (pred) {
            if (audit) NR_INC(g_nrndc[NRNDC_AUDIT]);
            if (pred == card) NR_INC(g_nrndc[NRNDC_SAME]);
            else {
                NR_INC(g_nrndc[NRNDC_BAD]);
                if (g_nrnd == 1) __atomic_store_n(&g_nrnd_memo_off, 1, __ATOMIC_RELAXED);
                if (__atomic_fetch_add(&g_nrnd_red, 1, __ATOMIC_RELAXED) < 16)
                    LOG("【红】[nr] native list: the chain of groups did not decide the card of a draw - memo %016llx, the renderer's cache %016llx (object %#llx, context flags %#x, "
                        "%u packets before the draw)%s", (unsigned long long)pred, (unsigned long long)card, (unsigned long long)n->obj, n->cflags, n->npre,
                        g_nrnd == 1 ? ": the memo answers nothing any more (every card is hashed from the cache)" : "");
            }
        } else if (key) NR_INC(g_nrndc[NRNDC_LEARN]);
        if (key) nrnd_memo_put(m, key, card);
    }
    nrnd_run_drawn(&t_nrnd, n, card);
    return card;
}
// The draw's pass is off: no card was taken, the chain stops.
static void nrnd_skip(const void *note) { nrnd_run_drawn(&t_nrnd, (const nrnd_note *)note, 0); }

// ---- half b: DM_NRN_REC (the composer is the NVN half's: dm/gfx/nvn/nvn_nrn_d.inc) ----
void nvn_nrn_rec_enable(int mode); void nvn_nrn_rec_stats(uint64_t out[3]);
static uint64_t g_nrnd_tj[6];   // atomics: ns of stack switch in / uniform-buffer bodies / texture binds of the flush / the draw (tap + record) / stack switch out; timed draws
static void nrnd_tj_done(void) {
    for (int k = 0; k < 5; k++) __atomic_fetch_add(&g_nrnd_tj[k], t_nrnd_tj[k + 1] - t_nrnd_tj[k], __ATOMIC_RELAXED);
    NR_INC(g_nrnd_tj[5]);
}
static void nrnd_rec_report(uint64_t frames) {
    if (!g_nrnd_rec) return;
    uint64_t s[3], t[6]; nvn_nrn_rec_stats(s);
    for (int k = 0; k < 6; k++) t[k] = __atomic_exchange_n(&g_nrnd_tj[k], 0, __ATOMIC_RELAXED);
    const double f = frames ? (double)frames : 1.; const uint64_t n = t[5] ? t[5] : 1;
    LOG("[nr] native rec (DM_NRN_REC=%d, step D, last 10 s): records of card draws composed directly %.0f a frame; verify: composed next to the NVN half's record and compared byte "
        "for byte %llu, DIFFER %llu; the big-stack call of a direct draw by part, ns (1 in 64, %llu timed; =2 only): onto the big stack %llu, the uniform-buffer bodies %llu, the "
        "texture binds of the flush %llu, the draw (tap + record) %llu, back %llu",
        g_nrnd_rec, s[0] / f, (unsigned long long)s[1], (unsigned long long)s[2], (unsigned long long)t[5], (unsigned long long)(t[0] / n), (unsigned long long)(t[1] / n),
        (unsigned long long)(t[2] / n), (unsigned long long)(t[3] / n), (unsigned long long)(t[4] / n));
}
static void nrnd_rec_arm(void) {
    const char *e = getenv("DM_NRN_REC");
    const int v = e && *e ? atoi(e) : 0;   // 0 on every device unless asked (the backend's policy function xbg_nrn_rec_default says the same)
    if (!v) return;
    if (v != 1 && v != 2) { LOG("【红】[nr] native rec: DM_NRN_REC=%s is not 0 / 1 / 2: off", e); return; }
    if (!nrn_on()) { LOG("【红】[nr] native rec: DM_NRN_REC=%d needs a DM_NRN_<PASS> switch at 1 or 2 (only a record with a card is composed): off", v); return; }
    { const char *z = getenv("DM_NR_Z5"); if (z && atoi(z)) { LOG("[nr] native rec: DM_NR_Z5=%s patches the record's spare words after it is written - DM_NRN_REC off", z); return; } }
    nvn_nrn_rec_enable(v); g_nrnd_rec = v;
    LOG("[nr] native rec (NR-NATIVE step D, DM_NRN_REC=%d): %s", v, v == 1 ? "the payload of a card draw's record (header, binds, NR-09 tail, card tail, short ext) is written by the "
        "composer in one pass; the recorder's own code around it is unchanged" : "verify - every card draw's record is also composed directly and compared byte for byte with the "
        "one the NVN half wrote; the parts of a direct draw's big-stack call are timed");
}
// From nrn_report (every 10 s).
static void nrnd_report(uint64_t frames) {
    nrnd_rec_report(frames);
    nrnk_report(frames);   // [DOS2] NR-NATIVE step D half c
    if (!g_nrnd) return;
    uint64_t c[NRNDC_N], kd[NRND_K_N];
    for (int i = 0; i < NRNDC_N; i++) c[i] = __atomic_exchange_n(&g_nrndc[i], 0, __ATOMIC_RELAXED);
    for (int i = 0; i < NRND_K_N; i++) kd[i] = __atomic_exchange_n(&g_nrnd_kind[i], 0, __ATOMIC_RELAXED);
    const double f = frames ? (double)frames : 1.;
    char pm[400]; int o = 0; pm[0] = 0;
    for (int a = 0; a < NRN_P_N; a++) for (int b = 0; b < NRN_P_N; b++) {
        const uint64_t v = __atomic_exchange_n(&g_nrnd_pm[a][b], 0, __ATOMIC_RELAXED);
        if (v && o < (int)sizeof pm - 48) o += snprintf(pm + o, sizeof pm - (size_t)o, "%s%s->%s %.0f", o ? ", " : "", nrn_pass_name(a), nrn_pass_name(b), (double)v / f);
    }
    LOG("[nr] native list (DM_NRN_LIST=%d, step D, last 10 s): notes written at list construction %.0f a frame (common %.0f, instanced %.0f, 0xb89330 %.0f, 0xb87470 %.0f, 0xc6cc50 %.0f, "
        "0xc7f590 %.0f, 0xc82c80 %.0f; group not scanned to its end %.0f, no single draw packet %.0f; lists that call NVN at once %.0f), %llu ns a note on the building thread (1 in 64); "
        "executed %.0f a frame; direct draws that asked %.0f a frame, without a note %.0f; pass named by the context %.0f a frame (same as the target names %.0f, other %.0f%s%s); "
        "card: the chain held for %.0f a frame, the memo answered %.0f (%.1f%% of the draws that asked), taught %.0f; compared with the cache's hash %llu (audits of =1: %llu), "
        "same %llu, BAD %llu; ns a card (1 in 64): from the memo %llu (%llu), hashed %llu (%llu)%s",
        g_nrnd, c[NRNDC_NOTES] / f, kd[0] / f, kd[1] / f, kd[2] / f, kd[3] / f, kd[4] / f, kd[5] / f, kd[6] / f, c[NRNDC_PART] / f, c[NRNDC_NODRAW] / f, c[NRNDC_NOLIST] / f,
        (unsigned long long)(c[NRNDC_SCAN_T] ? c[NRNDC_SCAN_NS] / c[NRNDC_SCAN_T] : 0), c[NRNDC_EXEC] / f, c[NRNDC_DRAWS] / f, c[NRNDC_NONOTE] / f, c[NRNDC_PASS_NOTE] / f,
        c[NRNDC_PASS_SAME] / f, c[NRNDC_PASS_BAD] / f, pm[0] ? "; note->targets a frame: " : "", pm, c[NRNDC_CHAIN] / f, c[NRNDC_HIT] / f,
        c[NRNDC_DRAWS] ? 100. * (double)c[NRNDC_HIT] / (double)c[NRNDC_DRAWS] : 0., c[NRNDC_LEARN] / f, (unsigned long long)(c[NRNDC_SAME] + c[NRNDC_BAD]),
        (unsigned long long)c[NRNDC_AUDIT], (unsigned long long)c[NRNDC_SAME], (unsigned long long)c[NRNDC_BAD],
        (unsigned long long)(c[NRNDC_T_MEMO] ? c[NRNDC_NS_MEMO] / c[NRNDC_T_MEMO] : 0), (unsigned long long)c[NRNDC_T_MEMO],
        (unsigned long long)(c[NRNDC_T_FULL] ? c[NRNDC_NS_FULL] / c[NRNDC_T_FULL] : 0), (unsigned long long)c[NRNDC_T_FULL],
        __atomic_load_n(&g_nrnd_memo_off, __ATOMIC_RELAXED) ? "; THE MEMO IS OFF (an audit found a wrong card)" : "");
}

// From dm_dos2_nr_install, after nrn_arm (guest code is not running yet). rc_ok: the ExecuteRenderCommand handler of NR is registered.
static void nrnd_arm(module_t *m, int rc_ok) {
    nrnd_rec_arm();
    const char *e = getenv("DM_NRN_LIST");
    const int v = e && *e ? atoi(e) : 0;   // 0 on every device unless asked (the backend's policy function xbg_nrn_list_default says the same)
    if (!v) return;
    if (v != 1 && v != 2) { LOG("【红】[nr] native list: DM_NRN_LIST=%s is not 0 / 1 / 2: off", e); return; }
    if (!nrn_on()) { LOG("【红】[nr] native list: DM_NRN_LIST=%d needs a DM_NRN_<PASS> switch at 1 or 2 (the note serves the card of a pass that is on): off", v); return; }
    if (!rc_ok) { LOG("【红】[nr] native list: DM_NRN_LIST=%d needs the shared ExecuteRenderCommand hook (the engine must never meet the note's opcode): off", v); return; }
    for (int i = 0; i < NRND_SITES; i++)
        if (!dn_words_ok(m, k_nrnd_site[i].off, k_nrnd_site[i].pro)) {
            LOG("【红】[nr] native list: main+%#x (%s) differs: not DOS2 update v786432 main (build 4979b200) - DM_NRN_LIST off, nothing hooked", k_nrnd_site[i].off, k_nrnd_site[i].name); return; }
    { static const uint32_t pro[4] = {0xd10103ff /* sub sp,sp,#0x40 */, 0xf9000bf5, 0xa9024ff4, 0xa9037bfd};
      if (!dn_words_ok(m, NRND_ALLOC, pro)) { LOG("【红】[nr] native list: main+%#x (AllocCommand) differs - DM_NRN_LIST off", NRND_ALLOC); return; } }
    if (dn_install_rc_note(nrnd_exec_note)) { LOG("【红】[nr] native list: the note's opcode could not be registered with the ExecuteRenderCommand hook - DM_NRN_LIST off"); return; }
    g_nrnd_base = (uint64_t)(uintptr_t)m->base; g_nrnd_size = m->size;
    g_nrnd_alloc = (nrnd_alloc_fn)(uintptr_t)(m->base + NRND_ALLOC);
    // The draw wrappers go in LAST and only when every one of them does: a wrapper that is in writes notes, and the hook that takes
    // them is registered above. The brackets first (without them a note only lacks its pass).
    int in = 0;
    for (int i = NRND_SITES - 1; i >= 0; i--) {
        void *o = NULL;
        if (dn_hook(m, k_nrnd_site[i].off, k_nrnd_site[i].pro, k_nrnd_site[i].hook, &o)) { LOG("【红】[nr] native list: main+%#x (%s): mmap / mprotect failed: this hook stays off", k_nrnd_site[i].off, k_nrnd_site[i].name); continue; }
        *k_nrnd_site[i].orig = (nr_fn)o; in++;
    }
    __atomic_store_n(&g_nrnd, v, __ATOMIC_RELEASE);
    nrnk_arm(m, v);   // [DOS2] NR-NATIVE step D half c: DM_NRN_PKT (needs the note)
    LOG("[nr] native list (NR-NATIVE step D, DM_NRN_LIST=%d: %s): %d of %d hooks in - the engine's draw-yourself functions put a note packet (opcode %#x, %d bytes) in front of the "
        "packets they append to a recorded list: the pass of their context, the group's draw packet and the hashes of the group; the card of a direct draw comes from the chain of "
        "groups (card of the previous draw + the packets in between) instead of a hash of the renderer's cache when nothing else ran in between",
        v, v == 1 ? "apply, 1 card in 64 audited against the cache" : "verify - every card is the cache's hash, the chain's card and the context's pass are compared", in, NRND_SITES,
        (unsigned)NRND_OP, (int)NRND_NOTE_BYTES);
}
