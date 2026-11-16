// [DOS2] NB-14: the key frame of an IR pass in the NVN tap (docs/dos2/NATIVE_BACKEND_PLAN.md NB-14; layout, mirror, walk and pack in
// nvn_nb_key.h). Included at the end of nvn_nb_tap.inc (inside hle_nvn.c, after nvn_nb_state.inc). Two calls from nbt_sync (render thread,
// the IR's command buffer): nbk_frame() right after OP_X_NB_FRAME (the mirror empties, the walk starts after that record), nbk_pass(node)
// right after OP_X_NB_PASS (walk the records written since, pack, write OP_X_NB_KEY straight after the PASS record). Both are behind
// g_nbk_on (DM_NB_RELOC 1 / 2, set by hle_dos2_nb.c at install): at 0 nothing is walked or written, the stream is byte for byte the old one.
// Cost: a walk of the records of each pass once (header + switch per record, state records copied into the mirror), a pack per pass; the
// line '[nb] key' every 10 s gives records walked, bytes and the measured time (1 in 16 passes timed).
#ifdef DM_GAME_DOS2
enum { NBKS_KEYS, NBKS_BYTES, NBKS_UNITS, NBKS_DEF, NBKS_REF, NBKS_WALKED, NBKS_NESTED, NBKS_BLOCKS, NBKS_NBDRAW, NBKS_NOANCHOR,
       NBKS_LOST_END, NBKS_LOST_BAD, NBKS_LOST_GUARD, NBKS_PARTIAL, NBKS_UNRESOLVED, NBKS_OVER, NBKS_BADREC, NBKS_BETWEEN, NBKS_NOREC,
       NBKS_TIMED, NBKS_NS, NBKS_FRAMES, NBKS_KINDS };
_Static_assert(NBKS_KINDS == NBKS_N, "nvn_nb_tap.h NBKS_N");
static nbk_mirror g_nbk;          // render thread only (the tap's thread)
static nbk_defs g_nbk_defs;       // OP_X_NB_STATE DEF blocks the walk saw (BSS: pages touched only when used)
static struct { const cmd_t *cb; const uint8_t *begin, *pos; uint32_t seq; int lost; } g_nbkw;
static uint8_t g_nbk_out[NBK_PAYLOAD_MAX];
static uint64_t g_nbks[NBKS_KINDS], g_nbk_seen[NBK_S_N];
static uint32_t g_nbk_tick;

static uint64_t nbk_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
// An OP_X_NB_STATE REF whose DEF lies before the walk start (before FRAME): the NB-13a dictionary of the current epoch holds the same bytes.
static const uint8_t *nbk_resolve(unsigned kind, unsigned id, uint32_t epoch, uint32_t *bytes) {
    if (!g_nbs_on || !t_nbs_own || kind >= NBS_KINDS || g_nbs.epoch != epoch || id >= g_nbs.d[kind].nid) return NULL;
    const nbs_did *e = &g_nbs.d[kind].id[id];
    *bytes = e->size; return g_nbs.d[kind].arena + e->off;
}
static void nbk_take_walk(void) {   // walk counters -> the 10 s line
    static const int map[NBK_S_N] = {NBKS_WALKED, NBKS_NESTED, NBKS_BLOCKS, NBKS_NBDRAW, NBKS_UNRESOLVED, NBKS_OVER, NBKS_BADREC};
    for (int i = 0; i < NBK_S_N; i++) { g_nbks[map[i]] += g_nbk.st[i] - g_nbk_seen[i]; g_nbk_seen[i] = g_nbk.st[i]; }
}
static void nbk_frame(void) {
    const cmd_t *o = (const cmd_t *)g_nbt.cb;
    nbk_reset(&g_nbk);
    g_nbkw.cb = o; g_nbkw.begin = o ? o->begin : NULL; g_nbkw.pos = o ? o->cur : NULL; g_nbkw.seq = g_nbt.seq; g_nbkw.lost = 0;
    g_nbks[NBKS_FRAMES]++;
}
static void nbk_pass(int node) {
    const cmd_t *o = (const cmd_t *)g_nbt.cb;
    const int timed = !(g_nbk_tick++ & 15); const uint64_t t0 = timed ? nbk_ns() : 0;
    const uint64_t w0 = g_nbk.st[NBK_S_WALKED];
    if (!o || !g_nbkw.pos || g_nbkw.cb != o || g_nbkw.begin != o->begin || g_nbkw.seq != g_nbt.seq) {   // no FRAME anchor in this recording
        if (!g_nbkw.lost) g_nbks[NBKS_NOANCHOR]++;
        g_nbkw.lost = 1;
    } else if (!g_nbkw.lost) {
        const int r = nbk_walk(&g_nbk, &g_nbk_defs, nbk_resolve, &g_nbkw.pos, o->cur);
        if (r != NBK_W_OK) { g_nbkw.lost = 1; g_nbks[r == NBK_W_END ? NBKS_LOST_END : r == NBK_W_GUARD ? NBKS_LOST_GUARD : NBKS_LOST_BAD]++; }
    }
    uint32_t def_off[NBK_UNITS]; unsigned nd, nr;
    const uint32_t n = nbk_pack(&g_nbk, g_nbt.seq, node, (uint32_t)(g_nbk.st[NBK_S_WALKED] - w0), g_nbkw.lost, g_nbk_out, sizeof g_nbk_out, def_off, &nd, &nr);
    uint8_t *p = n ? (uint8_t *)nbt_rec(OP_X_NB_KEY, n) : NULL;   // straight after the PASS record (nothing pending: the PASS flushed it)
    if (!p) g_nbks[NBKS_NOREC]++;
    else {
        memcpy(p, g_nbk_out, n); nbk_commit(&g_nbk, p, def_off);
        if (g_nbw_on) nbw_key(node, p);   // NB-21: the frame job carries the key's address
        g_nbks[NBKS_KEYS]++; g_nbks[NBKS_BYTES] += (n + 8 + 7) & ~7u; g_nbks[NBKS_UNITS] += nd + nr; g_nbks[NBKS_DEF] += nd; g_nbks[NBKS_REF] += nr;
        if (g_nbk.lost && !g_nbkw.lost) g_nbks[NBKS_PARTIAL]++;
    }
    if (!g_nbkw.lost && o && g_nbkw.pos) {   // past the KEY record; anything else written between PASS and KEY is filed (expected: none)
        const uint64_t w1 = g_nbk.st[NBK_S_WALKED];
        const int r = nbk_walk(&g_nbk, &g_nbk_defs, nbk_resolve, &g_nbkw.pos, o->cur);
        if (r != NBK_W_OK) { g_nbkw.lost = 1; g_nbks[NBKS_LOST_BAD]++; }
        const uint64_t extra = g_nbk.st[NBK_S_WALKED] - w1;
        if (extra > (p ? 1u : 0u)) g_nbks[NBKS_BETWEEN] += extra - (p ? 1u : 0u);
    }
    nbk_take_walk();
    if (timed) { g_nbks[NBKS_TIMED]++; g_nbks[NBKS_NS] += nbk_ns() - t0; }
}
void nvn_nb_key_enable(void);
void nvn_nb_key_stats(uint64_t out[NBKS_KINDS]);
void nvn_nb_key_enable(void) { g_nbk_on = 1; }
void nvn_nb_key_stats(uint64_t out[NBKS_KINDS]) { memcpy(out, g_nbks, sizeof g_nbks); memset(g_nbks, 0, sizeof g_nbks); }   // render thread (EndFrame)
#endif
