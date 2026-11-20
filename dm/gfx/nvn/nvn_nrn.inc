// [DOS2] NR-NATIVE step B, part 2 (docs/dos2/nr/NR-NATIVE.md section 9): the SHORT record of an engine draw - the NVN layer's half.
// Included twice by hle_nvn.c (and by the host test tests/nr_nrn_short_test.c with stubs): NVN_NRN_PART 1 right after nvn_nb_tap.inc
// (the state cmd_alloc and bind_handles read), NVN_NRN_PART 2 after nvn_nb_draw.inc (the functions; they need rec()).
//
// With DM_NRN_<PASS>=1 (and DM_NRN_SHORT not 0) the front end (hle_dos2_nrn.inc nrn_pre, from the direct draw of NR-09R) decides, before
// the flush of an engine draw runs, whether the card of the draw is published - the backend holds the IMAGE of the card: what the
// state packets and the texture binds of a draw with that card leave in the backend State. Then the draw is written short:
//   state records   the OP_X_NR_STATE records of the opcode-21 packets since the previous record are not written at once: they wait
//                   here (nrnp_room; at most NRNP_MAX, a record of another command buffer or a full list writes them first). Any other
//                   record written by this thread writes them first, in order (cmd_alloc: NRNP_FLUSH) - so a waiting record is only
//                   ever followed by the draw it was made for. A short draw drops them (nvn_nrn_short(1)) and keeps the union of
//                   the fields they would have written (nrn_emask); a long draw lets its own record write them (the same order as
//                   without the switch).
//   texture binds   a BindTexture of the flush (one handle, slot < NRN_TEX_SLOTS, at most two stages a draw) is not added to the
//                   shadow of the draw; its stage and slot are kept. Its tap, the mirror of NR-09R, the texture-set generation and the
//                   use mark run as before.
//   the record      OP_X_NB_DRAW with the uniform-buffer binds only, the NR-09 tail, the NR-NATIVE tail flagged NRN_T_SHORT and the
//                   nrn_short_ext after it: exactly which fields and which texture slots were left out. The backend writes those,
//                   and only those, from the image.
// Nothing here runs without the switch: nrnp_room is asked only when g_nrn_defer is set, the hooks are one thread-local load.
#if NVN_NRN_PART == 1
enum { NRNP_MAX = 4, NRNP_BYTES = 704 };
enum { NRNPS_WAIT, NRNPS_WRITTEN, NRNPS_DROPPED, NRNPS_TEX, NRNPS_SHORT, NRNPS_NOROOM, NRNPS_LOST, NRNPS_N };
static int g_nrn_defer;                         // armed by the front end before guest code runs (nvn_nrn_short_enable)
static __thread struct { void *cb; uint32_t n, busy; uint32_t len[NRNP_MAX]; uint8_t b[NRNP_MAX][NRNP_BYTES]; } t_nrnp;
static __thread int t_nrn_short;                // the draw being recorded is written short
static __thread nrn_short_ext t_nrn_sx;         // what was left out of it
static uint64_t g_nrnps[NRNPS_N];               // atomics: state records that waited / written (by their long draw or before another record) / dropped by a short draw; texture binds not recorded; short records; records too long to wait; waiting records whose command buffer stopped recording
static void nrnp_flush(void);
static int nrn_short_bind(int st, int slot);
#define NRNP_FLUSH(o) do { if (__builtin_expect(t_nrnp.n, 0) && !t_nrnp.busy) nrnp_flush(); } while (0)
#define NRN_SHORT_BIND(op, st, first, n) (__builtin_expect(t_nrn_short, 0) && (op) == OP_BIND_TEX && (n) == 1 && nrn_short_bind((int)(st), (first)))
#else
// The waiting state records as the records they are, in the order they were made.
static void nrnp_flush(void) {
    const uint32_t n = t_nrnp.n; NVNcommandBuffer *c = (NVNcommandBuffer *)t_nrnp.cb;
    t_nrnp.n = 0;
    if (!NRNP_RECORDING(c)) { __atomic_fetch_add(&g_nrnps[NRNPS_LOST], n, __ATOMIC_RELAXED); return; }   // (cannot be: EndRecording writes a record first - on this thread)
    t_nrnp.busy = 1;
    for (uint32_t i = 0; i < n; i++) memcpy(rec(c, OP_X_NR_STATE, t_nrnp.len[i]), t_nrnp.b[i], t_nrnp.len[i]);
    t_nrnp.busy = 0;
    __atomic_fetch_add(&g_nrnps[NRNPS_WRITTEN], n, __ATOMIC_RELAXED);
}
// Room for a state record of n bytes that waits for its draw (the caller fills it with nr3_put). NULL: write it now (too long; what
// waited is written first, the order stands).
static uint8_t *nrnp_room(NVNcommandBuffer *c, uint32_t n) {
    if (n > NRNP_BYTES) { __atomic_fetch_add(&g_nrnps[NRNPS_NOROOM], 1, __ATOMIC_RELAXED); NRNP_FLUSH(c); return NULL; }
    if (t_nrnp.n && (t_nrnp.cb != (void *)c || t_nrnp.n == NRNP_MAX)) nrnp_flush();
    t_nrnp.cb = c; t_nrnp.len[t_nrnp.n] = n;
    __atomic_fetch_add(&g_nrnps[NRNPS_WAIT], 1, __ATOMIC_RELAXED);
    return t_nrnp.b[t_nrnp.n++];
}
void nvn_nrn_short_enable(int on);
void nvn_nrn_short_enable(int on) { g_nrn_defer = on; }
// From the front end, right before the flush and the draw of an engine draw are recorded (same thread). on: the draw is written short -
// the state records that wait for it are dropped (their fields are kept in the mask), its texture binds are not recorded. off: the
// record of the draw writes them first.
void nvn_nrn_short(int on);
void nvn_nrn_short(int on) {
    t_nrn_short = on;
    if (!on) return;
    memset(&t_nrn_sx, 0, sizeof t_nrn_sx); t_nrn_sx.st[0] = t_nrn_sx.st[1] = 0xff;
    for (uint32_t i = 0; i < t_nrnp.n; i++) nrn_emask_of_state(&t_nrn_sx.m, t_nrnp.b[i], t_nrnp.len[i]);
    if (t_nrnp.n) { __atomic_fetch_add(&g_nrnps[NRNPS_DROPPED], t_nrnp.n, __ATOMIC_RELAXED); t_nrnp.n = 0; }
}
// A texture bind of a short draw: 1 = left out (its slot is in the mask), 0 = record it as before (a third stage, a slot outside the image).
static int nrn_short_bind(int st, int slot) {
    if (!nrn_short_ext_tex(&t_nrn_sx, st, slot)) return 0;
    __atomic_fetch_add(&g_nrnps[NRNPS_TEX], 1, __ATOMIC_RELAXED);
    return 1;
}
int nvn_nrn_short_left(void);
int nvn_nrn_short_left(void) { const int l = t_nrn_short; t_nrn_short = 0; return l; }   // after the draw was recorded: the flag must be gone (nbd_draw took it)
void nvn_nrn_short_stats(uint64_t out[NRNPS_N]);
void nvn_nrn_short_stats(uint64_t out[NRNPS_N]) { for (int i = 0; i < NRNPS_N; i++) out[i] = __atomic_exchange_n(&g_nrnps[i], 0, __ATOMIC_RELAXED); }
#endif
