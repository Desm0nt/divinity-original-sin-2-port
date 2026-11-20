// [DOS2] NB-13a: the state shadow of the NVN tap (docs/dos2/NATIVE_BACKEND_PLAN.md NB-13a; layout and dedup in nvn_nb_state.h). Included
// at the end of nvn_nb_tap.inc (inside hle_nvn.c, before cmd_alloc / rec). Hooks, one line each in hle_nvn.c:
//   rec()          NBS_REC first: inside a taken state bracket a state record is captured (rec returns a slot of the shadow, the entry
//                  point writes its payload there; the record is filed at the next tap call); otherwise what is pending goes out first.
//                  NBS_RAW last: a state record written plainly makes its kind "unknown" (read once its payload is written).
//   cmd_alloc()    NBS_BEFORE first: before any record of the shadow's command buffer the dirty state goes out as one OP_X_NB_STATE.
//   EndRecording   NBS_END after its OP_END: the epoch (dictionaries) of that command buffer ends.
// The bracket comes from the front end (hle_dos2_nb.c: ExecuteRenderCommand pre / post, DM_NB_OPS state bits): nvn_nb_state_begin(taken)
// / _end nest per packet. One thread owns the shadow (the first that enters a taken bracket, the engine's render thread); a state record
// on another thread only marks its kinds unknown (atomically, consumed by the owner). g_nbs_on == 0 (no state bit): every hook is a load
// and a not-taken branch, the command stream is byte for byte the old one.
#ifdef DM_GAME_DOS2
enum { NBSS_BRACKETS, NBSS_TAKEN, NBSS_OTHER_THREAD, NBSS_CAPTURED, NBSS_CAPTURED_BYTES, NBSS_RECORDS, NBSS_BYTES, NBSS_DEF, NBSS_REF,
       NBSS_NOID, NBSS_SAME, NBSS_RAW, NBSS_XRAW, NBSS_UNKNOWN, NBSS_FALLBACK, NBSS_EPOCHS, NBSS_KINDS };
_Static_assert(NBSS_KINDS == NBSS_N, "nvn_nb_tap.h NBSS_N");
static int g_nbs_on, g_nbs_owned, g_nbs_ready;   // g_nbs_ready: nvn_nb_state_enable ran (NR-13: nvn_nb_state_pause turns g_nbs_on off / on)
static __thread int t_nbs_own, t_nbs_emit, t_nbs_depth; static __thread uint8_t t_nbs_in[16];
static nbs_state g_nbs;                 // owner thread only
static cmd_t *g_nbs_cb;                 // the command buffer of the shadow / epoch
static uint32_t g_nbs_epoch_next = 1;
static unsigned g_nbs_xunk;             // atomic: kinds touched by plain state records on other threads
static struct { int active; cmd_t *cb; uint32_t size; uint8_t rec[NBS_REC_MAX]; } g_nbs_pend;   // the last captured record (payload being written)
static struct { int active; uint32_t op, n; const uint8_t *pl; } g_nbs_raw;                          // the last plain state record (payload being written)
static uint8_t g_nbs_out[NBS_PAYLOAD_MAX];
static uint64_t g_nbss[NBSS_KINDS];
static uint64_t g_nbs_st_seen[NBS_S_N];
static void nbss_add(int k, uint64_t n) { __atomic_fetch_add(&g_nbss[k], n, __ATOMIC_RELAXED); }
static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload);   // hle_nvn.c

static void nbs_epoch(void) {
    uint32_t e = g_nbs_epoch_next++; if (!e) e = g_nbs_epoch_next++;
    nbs_new_epoch(&g_nbs, e); nbss_add(NBSS_EPOCHS, 1);
}
// The dirty kinds as one OP_X_NB_STATE into the shadow's command buffer (not a foreign record of an NB-09 draw).
static void nbs_write(void) {
    if (!g_nbs.dirty || !g_nbs_cb) return;
    const uint32_t n = nbs_pack(&g_nbs, g_nbs_out, sizeof g_nbs_out);
    for (int i = 0; i < NBS_S_N; i++) {   // pack counters -> the 10 s line
        const uint64_t d = g_nbs.st[i] - g_nbs_st_seen[i]; g_nbs_st_seen[i] = g_nbs.st[i];
        static const int map[NBS_S_N] = {NBSS_DEF, NBSS_REF, NBSS_NOID, NBSS_SAME, NBSS_UNKNOWN};
        if (d) nbss_add(map[i], d);
    }
    if (!n) return;
    t_nbs_emit = 1;
    uint8_t *p; NBD_QUIET(p = (uint8_t *)rec((NVNcommandBuffer *)g_nbs_cb, OP_X_NB_STATE, n));
    t_nbs_emit = 0;
    memcpy(p, g_nbs_out, n);
    nbss_add(NBSS_RECORDS, 1); nbss_add(NBSS_BYTES, (n + 8 + 7) & ~7u);
}
static void nbs_unknown_k(unsigned mask) {
    nbs_unknown(&g_nbs, mask);
    const uint64_t d = g_nbs.st[NBS_S_UNKNOWN] - g_nbs_st_seen[NBS_S_UNKNOWN]; g_nbs_st_seen[NBS_S_UNKNOWN] = g_nbs.st[NBS_S_UNKNOWN];
    if (d) nbss_add(NBSS_UNKNOWN, d);
}
// File the record captured last (its payload is complete now: no other tap call came in between) / mark the kind of the last plain one.
static void nbs_settle(void) {
    if (g_nbs_raw.active) {
        g_nbs_raw.active = 0;
        uint32_t key; const int k = nbs_kind(g_nbs_raw.op, g_nbs_raw.pl, g_nbs_raw.n, &key);
        nbs_unknown_k(k >= 0 ? 1u << k : nbs_op_kinds(g_nbs_raw.op));
    }
    if (!g_nbs_pend.active) return;
    g_nbs_pend.active = 0;
    if (nbs_capture(&g_nbs, g_nbs_pend.rec, g_nbs_pend.size) >= 0) return;
    // not modelled / does not fit: what is pending goes out, then the record as it would have been written, and its kinds become unknown
    nbss_add(NBSS_FALLBACK, 1);
    nbs_write();
    const uint32_t op = nbs_rd32(g_nbs_pend.rec), pn = g_nbs_pend.size - 8;
    t_nbs_emit = 1;
    uint8_t *q; NBD_QUIET(q = (uint8_t *)rec((NVNcommandBuffer *)g_nbs_pend.cb, op, pn));
    t_nbs_emit = 0;
    memcpy(q, g_nbs_pend.rec + 8, pn);
    uint32_t key; const int k = nbs_kind(op, g_nbs_pend.rec + 8, pn, &key);
    nbs_unknown_k(k >= 0 ? 1u << k : nbs_op_kinds(op));
}
static void nbs_flush(void) {
    nbs_settle();
    nbs_write();
    const unsigned x = __atomic_exchange_n(&g_nbs_xunk, 0u, __ATOMIC_RELAXED);
    if (x) nbs_unknown_k(x);
}
// rec() entry: the capture slot, or NULL (the record is written as before).
static void *nbs_rec(NVNcommandBuffer *c, uint32_t op, size_t payload) {
    if (t_nbs_emit) return NULL;
    const unsigned kinds = nbs_op_kinds(op);
    if (!t_nbs_own) { if (kinds) { __atomic_fetch_or(&g_nbs_xunk, kinds, __ATOMIC_RELAXED); nbss_add(NBSS_XRAW, 1); } return NULL; }
    nbs_settle();
    if (!kinds) return NULL;   // not a state record: NBS_BEFORE in cmd_alloc writes what is pending first
    const uint32_t size = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    cmd_t *o = (cmd_t *)c;   // = CB(c), defined later in hle_nvn.c
    if (t_nbs_depth > 0 && t_nbs_depth <= (int)sizeof t_nbs_in && t_nbs_in[t_nbs_depth - 1] && size <= NBS_REC_MAX) {
        if (o != g_nbs_cb) { nbs_flush(); g_nbs_cb = o; nbs_epoch(); }   // the shadow follows the command buffer; a new epoch
        g_nbs_pend.active = 1; g_nbs_pend.cb = o; g_nbs_pend.size = size;
        memset(g_nbs_pend.rec, 0, size);
        const uint32_t hdr[2] = {op, size}; memcpy(g_nbs_pend.rec, hdr, 8);
        nbss_add(NBSS_CAPTURED, 1); nbss_add(NBSS_CAPTURED_BYTES, size);
        return g_nbs_pend.rec + 8;
    }
    nbs_flush();   // a plain state record (outside a taken bracket): the shadow's state goes out before it, its kind is marked by NBS_RAW
    return NULL;
}
static void nbs_raw(uint32_t op, const uint8_t *pl, uint32_t n) {
    if (t_nbs_emit || !t_nbs_own || !nbs_op_kinds(op)) return;
    g_nbs_raw.active = 1; g_nbs_raw.op = op; g_nbs_raw.n = n; g_nbs_raw.pl = pl; nbss_add(NBSS_RAW, 1);
}
static __thread int t_nbs_time; static __thread uint64_t t_nbs_ns;   // [DOS2] NR-CPU: a timed direct draw (nvn_nrd_draw) asks for the state block's share
static void nbs_before(cmd_t *o) {
    if (t_nbs_emit || !t_nbs_own) return;
    const uint64_t t0 = __builtin_expect(t_nbs_time, 0) ? now_ns() : 0;
    nbs_settle();
    if (o == g_nbs_cb && (g_nbs.dirty || __atomic_load_n(&g_nbs_xunk, __ATOMIC_RELAXED))) nbs_flush();
    if (t0) t_nbs_ns += now_ns() - t0;
}
static void nbs_end(cmd_t *o) {
    if (t_nbs_emit || !t_nbs_own || o != g_nbs_cb) return;
    nbs_flush();   // nothing is pending here (the OP_END went through NBS_BEFORE)
    g_nbs_cb = NULL; nbs_epoch();
}
void nvn_nb_state_enable(void);
void nvn_nb_state_begin(int taken);
void nvn_nb_state_end(void);
void nvn_nb_state_stats(uint64_t out[NBSS_KINDS]);
void nvn_nb_state_enable(void) { nbs_init(&g_nbs, 0); nbs_epoch(); g_nbs_ready = 1; g_nbs_on = 1; }
void nvn_nb_state_begin(int taken) {
    if (!g_nbs_on) return;
    if (taken) {
        nbss_add(NBSS_BRACKETS, 1);
        if (!t_nbs_own) {
            int z = 0;
            if (__atomic_compare_exchange_n(&g_nbs_owned, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) t_nbs_own = 1;
            else { nbss_add(NBSS_OTHER_THREAD, 1); taken = 0; }
        }
        if (taken) nbss_add(NBSS_TAKEN, 1);
    }
    if (t_nbs_own) nbs_settle();
    if (t_nbs_depth < (int)sizeof t_nbs_in) t_nbs_in[t_nbs_depth] = (uint8_t)(taken != 0);
    t_nbs_depth++;
}
void nvn_nb_state_end(void) {
    if (!g_nbs_on || t_nbs_depth <= 0) return;
    if (t_nbs_own) nbs_settle();   // the last call of the packet is filed (it stays pending until the next record)
    t_nbs_depth--;
}
// [DOS2] NR-13 DM_NR_LEAN_AB (dm/host/hle_dos2_nb.c, the render thread at EndFrame): the capture off (1) / on again (0) at a frame
// boundary, so that one run measures both. Off: what the shadow holds goes out first (as at EndRecording), then every hook is a load and a
// not-taken branch again and the state packets write plain NVN state records. On: nbs_resume (every kind unknown, a new epoch).
// 1 = done (or already so); 0 = not now (a packet is open on this thread, or the shadow belongs to another thread): ask at the next frame.
int nvn_nb_state_pause(int off);
int nvn_nb_state_pause(int off) {
    if (!g_nbs_ready || t_nbs_depth) return 0;
    if (__atomic_load_n(&g_nbs_owned, __ATOMIC_ACQUIRE) && !t_nbs_own) return 0;
    if (off) {
        if (!g_nbs_on) return 1;
        if (t_nbs_own) nbs_flush();
        g_nbs_on = 0;
    } else {
        if (g_nbs_on) return 1;
        g_nbs_pend.active = 0; g_nbs_raw.active = 0; g_nbs_cb = NULL;
        (void)__atomic_exchange_n(&g_nbs_xunk, 0u, __ATOMIC_RELAXED);
        { uint32_t e = g_nbs_epoch_next++; if (!e) e = g_nbs_epoch_next++; nbs_resume(&g_nbs, e); nbss_add(NBSS_EPOCHS, 1); }
        g_nbs_on = 1;
    }
    return 1;
}
// [DOS2] NR-09S3 (DM_NR_STATE=4, nvn_nr_state3.inc): one OP_X_NR_STATE record carries the state of an opcode-21 packet instead of the
// state records the shadow would have captured. nbs_ext_before: what the shadow holds goes out first (as before a plain state record, in
// whichever command buffer it belongs to). nbs_ext_after: the kinds the record wrote are unknown to the shadow (as after a plain record):
// the next captured call of such a kind is emitted again with only the entries set since. Owner thread only; another thread marks them
// for the owner (g_nbs_xunk), like a plain record there.
static void nbs_ext_before(void) {   // = nbs_flush, without its atomic exchange when nothing is marked
    if (t_nbs_emit || !t_nbs_own) return;
    nbs_settle();
    if (g_nbs.dirty || __atomic_load_n(&g_nbs_xunk, __ATOMIC_RELAXED)) nbs_flush();
}
static void nbs_ext_after(unsigned kinds) {
    if (t_nbs_emit || !kinds) return;
    if (!t_nbs_own) { __atomic_fetch_or(&g_nbs_xunk, kinds, __ATOMIC_RELAXED); nbss_add(NBSS_XRAW, 1); return; }
    nbs_settle(); nbs_unknown_k(kinds);
}
void nvn_nb_state_stats(uint64_t out[NBSS_KINDS]) { for (int i = 0; i < NBSS_KINDS; i++) out[i] = __atomic_exchange_n(&g_nbss[i], 0, __ATOMIC_RELAXED); }
#define NBS_REC(c, op, payload) do { if (__builtin_expect(g_nbs_on, 0)) { void *nbs_q_ = nbs_rec((c), (op), (payload)); if (nbs_q_) return nbs_q_; } } while (0)
#define NBS_RAW(op, pl, n) do { if (__builtin_expect(g_nbs_on, 0)) nbs_raw((op), (pl), (n)); } while (0)
#define NBS_BEFORE(o) do { if (__builtin_expect(g_nbs_on, 0)) nbs_before(o); } while (0)
#define NBS_STATE_TIMED 1   // [NR-CPU] t_nbs_time / t_nbs_ns exist (nvn_nr_direct.inc)
#define NBS_END(o) do { if (__builtin_expect(g_nbs_on, 0)) nbs_end(o); } while (0)
#define NBS_EXT_BEFORE() do { if (__builtin_expect(g_nbs_on, 0)) nbs_ext_before(); } while (0)            // [DOS2] NR-09S3
#define NBS_EXT_AFTER(k) do { if (__builtin_expect(g_nbs_on, 0)) nbs_ext_after(k); } while (0)            // [DOS2] NR-09S3
#else
#define NBS_EXT_BEFORE() ((void)0)
#define NBS_EXT_AFTER(k) ((void)0)
#define NBS_REC(c, op, payload) ((void)0)
#define NBS_RAW(op, pl, n) ((void)0)
#define NBS_BEFORE(o) ((void)0)
#define NBS_END(o) ((void)0)
#endif
