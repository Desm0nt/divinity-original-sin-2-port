// [DOS2] NR-02: draw identity, counting mode (DM_NR_KEY, default 0; docs/dos2/nr/NR-02.md). Included by hle_dos2_nr.c after its hooks:
// uses the registry (g_reg, g_nr_mu), nr_peek, NR_HOOK / NR_A / NR_F, nr_rc_pre. main = main.raw of update v786432, build 4979b200.
//   DM_NR_KEY=0 / unset  nothing (no hook, no allocation; the NVN tap lines return at once)
//   DM_NR_KEY=1          needs DM_NR=1 or 2. A hook on the renderer's texture flush 0x924730 - every engine draw path calls it right before
//                        its NVN draw call - reads the renderer's state cache and counts per draw: the native key (shader, blend,
//                        depth-stencil, raster, vertex format handles, blend mask word, primitive, alpha test, RT format set; nr_key.h),
//                        whether it is eligible for a native path and why not, the texture set (texture + sampler pool ids of the vertex
//                        and fragment slots, hashed only when a dirty range says they changed), whether the draw ran inside an engine
//                        command list (ExecuteRenderCommand depth on this thread) or outside the lists, and the call site. The NVN draw
//                        that follows is matched to it (any other NVN draw = UI / direct NVN). "[nr] draw keys ..." every 10 s;
//                        with DM_NR_DUMP=1 nr_keys.tsv, nr_texsets.tsv, nr_rtfmts.tsv, nr_names.tsv, nr_draw_sites.tsv.
//   DM_NR_KEY=2          the same + checks (red lines, first 16, all counted): the NVN programs bound on the command buffer at the draw =
//                        the programs the registry holds for the shader handle in the state cache; the engine's shader record (r+0x17e0)
//                        holds them too; texture slots never change under a clean dirty range (the premise of a texture-set cache); the
//                        pack key of each program at its first draw = the key computed at nvnProgramSetShaders.
// There is no DM_NR_KEY=3: nothing is executed differently in NR-02, so there is nothing to poison.
// State cache of the renderer r (main 0x93e6ac code 21 / 0x9283a0 / 0x9284e0 / 0x9279a0 / 0x924730 / 0x9243d0; dos2_nr_check.py checks
// each store): +0x16f0 raster handle, +0x16f8 blend handle, +0x1700 blend mask word, +0x1708 depth-stencil handle (+0x1710 stencil ref,
// dynamic), +0x1714 engine primitive byte, +0x1740 vertex format handle, +0x17d8 shader handle (+0x17e0 its record: +8 VS, +0x10 FS
// program), +0x17e8 + 8 i colour targets 0..7, +0x1828 depth target, +0x1868 alpha reference (test on when >= 0); texture slots: ids
// +0x1558 / +0x15e0 (vertex / fragment, 32 x i32, -1 empty), sampler ids +0x13c0 / +0x1448, dirty ranges (i8 min, i8 max; clean = 32, -1)
// textures +0x15dc / +0x1664, samplers +0x1444 / +0x14cc. The flush itself resets the ranges; a flush without a draw (code 33, compute)
// carries "dirty" to the next draw of the thread. Pool id of a texture: u32 at NVN texture object + 0xc0 (+0xc4.. its layer / cube views),
// stored by CreateTexture right after nvnTexturePoolRegisterTexture.
// Locks: g_nk_mu around the key tables; lock order g_nk_mu -> g_nr_mu (the registry hooks never hold g_nr_mu when they call in here).
// The state cache is read directly: the hook runs at the entry of 0x924730 on the thread of the draw, which reads the same fields itself.
static nr_keys *g_nk;
static pthread_mutex_t g_nk_mu = PTHREAD_MUTEX_INITIALIZER;
static nr_fn g_o_tf;
static uint64_t g_nk_base;   // main's load address
enum { NK_TF = 0x924730 };
// Every call of the texture flush in main (a bl at `bl`; the return address is bl + 4) and what follows it on that path.
static const struct nk_site { uint32_t bl; uint8_t kind; } k_nk_site[] = {
    {0x928f0c, NR_DS_LIST_IDX},       // 0x928e80 DrawElements, only from the code-36 handler
    {0x92903c, NR_DS_IDX_INST},       // 0x928fb0 DrawElementsInstanced (code 37 and 0xb7e020 directly)
    {0x929140, NR_DS_COMPUTE},        // 0x9290f0 DispatchCompute (code 43)
    {0x92f9c4, NR_DS_FULL}, {0x92ff60, NR_DS_FULL}, {0x930570, NR_DS_FULL}, {0x930be4, NR_DS_FULL}, {0x9311b4, NR_DS_FULL},   // codes 38..42
    {0x93f0f4, NR_DS_LIST_ARRAYS},    // ExecuteRenderCommand code 35, DrawArrays inline
    {0x93fa28, NR_DS_FLUSH},          // code 33: the flush alone
    {0x941010, NR_DS_DIRECT_ARRAYS},  // 0x940f90 DrawArrays when the list records nothing (list +0x30 == 0)
    {0xaab224, NR_DS_DIRECT_IDX}, {0xabcf38, NR_DS_DIRECT_IDX}, {0xabd3ec, NR_DS_DIRECT_IDX}, {0xabe5a0, NR_DS_DIRECT_IDX},   // the dual-mode
    {0xabe8c0, NR_DS_DIRECT_IDX}, {0xabf138, NR_DS_DIRECT_IDX}, {0xabf844, NR_DS_DIRECT_IDX}, {0xaf31d0, NR_DS_DIRECT_IDX},   // functions' own
    {0xb8964c, NR_DS_DIRECT_IDX}, {0xb89830, NR_DS_DIRECT_IDX}, {0x1241144, NR_DS_DIRECT_IDX}, {0x1335d40, NR_DS_DIRECT_IDX}, // DrawElements
    {0xb0ae90, NR_DS_DIRECT_ARRAYS}, {0xd1b2ec, NR_DS_DIRECT_ARRAYS},                                                         // (vtable only)
};
enum { NK_SITES = sizeof k_nk_site / sizeof k_nk_site[0] };
enum { NK_R_RASTER = 0x16f0, NK_R_BLEND = 0x16f8, NK_R_BLEND_ARG = 0x1700, NK_R_DEPTH = 0x1708, NK_R_PRIM = 0x1714, NK_R_VFORMAT = 0x1740,
       NK_R_SHADER = 0x17d8, NK_R_SHADER_REC = 0x17e0, NK_R_COLOR = 0x17e8, NK_R_DEPTH_RT = 0x1828, NK_R_ALPHA_REF = 0x1868,
       NK_R_TEX_VS = 0x1558, NK_R_TEX_FS = 0x15e0, NK_R_SMP_VS = 0x13c0, NK_R_SMP_FS = 0x1448 };
static const uint16_t k_nk_dirty[4] = {0x15dc, 0x1664, 0x1444, 0x14cc};   // (min, max) byte pairs: textures VS / FS, samplers VS / FS

typedef struct nk_stat {   // under g_nk_mu
    uint64_t calls, kind[NR_DS_N][2], site[NK_SITES][2];   // [.][0 inside an engine command list, 1 outside the lists]
    uint64_t draws, eligible, why[NR_NE_BITS], new_keys, new_ts;
    uint64_t dirty, changed, ts_rt, ts_unknown, ts_none;
    uint64_t chk_draws, bad_record, bad_clean;
} nk_stat;
static nk_stat g_nks, g_nks_w;   // running totals, the copy at the last report
static struct { uint64_t nvn_engine, nvn_other, pend_lost, chk_bound, bad_bound, unbound, keys_checked, bad_packkey, bad_lines; } g_nka;   // atomics
static __thread struct {
    int rc, pend, carry, site, ncb;
    uint64_t r, ts_h; uint32_t ts_id; uint16_t rtf;
    uint64_t rt[NR_RT_COLORS + 1];
    uint64_t shader, exp[2]; char key[2][17];             // the pending draw (DM_NR_KEY=2)
    struct { const void *cb; uint64_t p[3]; } cb[4];      // programs bound per command buffer on this thread (DM_NR_KEY=2)
    // NR-09 (DM_NR_DRAW): the identity of the pending engine draw (idp, set by the flush hook) becomes the identity of the NVN draw that
    // follows it (idn, moved by dm_nr_tap_draw - the first statement of every NVN draw entry) and is taken once by dm_nr_draw_ident.
    int idp, idn; uint32_t id_key, id_ts, id_why, id_site;
} t_nk;
static int g_nr_draw;   // NR-09: DM_NR_DRAW 1 / 2 as armed by dm_dos2_nr_install (the hook hands identities over)
// NR-09, DM_NR_DRAW without DM_NR_KEY (g_nr_key == 3, "identity only"): the last keys of this thread, so that a repeated key costs a
// compare and no lock. Direct mapped by the key's hash; ids are dense and never reused (nr_key.h), `why` is fixed at the key's first sight.
enum { NK_LEAN = 64 };
static __thread struct { nr_dkey k; uint32_t id, why; } t_nkl[NK_LEAN];
static struct { uint64_t draws, key_hit, key_lock, ts_same, ts_lock, rt_lock, taken, untaken; } g_nki;   // atomics (lean path + hand-over)
static struct { uint64_t p; } g_nk_seen[4096];            // programs whose pack key was checked at a draw (g_nk_mu)

static int nk_mode(void) {
    const char *e = getenv("DM_NR_KEY");
    const int v = e ? atoi(e) : 0;
    if (v == 1 || v == 2) return v;
    if (v) LOG("【红】[nr] DM_NR_KEY=%s: only 0 (off), 1 (count) and 2 (count + checks) exist - off", e);
    return 0;
}
static int nk_site(uint64_t lr) {   // index of the call site, -1 = not a known caller
    static __thread int last = -1;
    const uint64_t bl = lr - g_nk_base - 4;
    if (last >= 0 && k_nk_site[last].bl == bl) return last;
    for (int i = 0; i < NK_SITES; i++) if (k_nk_site[i].bl == bl) return last = i;
    return -1;
}
static void nk_red(const char *what, const char *detail) {
    if (__atomic_fetch_add(&g_nka.bad_lines, 1, __ATOMIC_RELAXED) < 16) LOG("【红】[nr] key check: %s - %s", what, detail);
}

// ---- the registry side: pool ids of a texture the engine created (from nr_tex_commit, g_nr_mu NOT held) ----
static void nk_texture(uint64_t handle, uint64_t obj, unsigned views) {
    uint32_t id[64]; const unsigned n = 1 + (views > 63 ? 63 : views);
    if (!nr_peek(id, obj + 0xc0, (size_t)n * 4)) return;
    pthread_mutex_lock(&g_nk_mu);
    for (unsigned i = 0; i < n; i++) if (id[i] && id[i] < (1u << 20)) nr_texid_put(g_nk, id[i], handle);
    pthread_mutex_unlock(&g_nk_mu);
}

// ---- ExecuteRenderCommand: the opcode-55 handler of NR-01 + the list depth of this thread ----
static void nk_rc_pre(uint64_t list, unsigned op, uint64_t pkt) { t_nk.rc++; nr_rc_pre(list, op, pkt); }
static void nk_rc_post(uint64_t list, unsigned op, uint64_t pkt, uint64_t len) { if (t_nk.rc > 0) t_nk.rc--; nrnev_rc_post(list, op, pkt, len); /* [DOS2] NR-NATIVE step E */ }
static const dn_rc_handler k_nk_rc = {nk_rc_pre, nk_rc_post};

// ---- NR-09: the identity only (g_nr_key == 3: DM_NR_DRAW without DM_NR_KEY). The same reads and tables as nk_draw below, without the
// census: a key equal to one of the thread's last keys and a clean texture range take no lock.
static void nk_ident(uint64_t r, uint64_t lr) {
    const int si = nk_site(lr), kind = si >= 0 ? k_nk_site[si].kind : NR_DS_UNKNOWN;
    const uint8_t *R = (const uint8_t *)(uintptr_t)r;
    int dirty = 0;
    for (int i = 0; i < 4; i++) if ((int8_t)R[k_nk_dirty[i]] <= (int8_t)R[k_nk_dirty[i] + 1]) dirty = 1;
    if (t_nk.r != r) { t_nk.r = r; t_nk.ts_id = 0; t_nk.ts_h = 0; t_nk.rtf = 0; dirty = 1; }
    dirty |= t_nk.carry;
    if (!nr_dsite_is_draw(kind)) { t_nk.carry = dirty; return; }
    t_nk.carry = 0;
    nr_dkey k; memset(&k, 0, sizeof k);
    memcpy(&k.shader, R + NK_R_SHADER, 8); memcpy(&k.blend, R + NK_R_BLEND, 8); memcpy(&k.blend_arg, R + NK_R_BLEND_ARG, 4);
    memcpy(&k.depth, R + NK_R_DEPTH, 8); memcpy(&k.raster, R + NK_R_RASTER, 8); memcpy(&k.vformat, R + NK_R_VFORMAT, 8);
    float aref; memcpy(&aref, R + NK_R_ALPHA_REF, 4);
    k.prim = R[NK_R_PRIM]; k.alpha_test = aref >= 0.f;
    uint64_t rt[NR_RT_COLORS + 1]; memcpy(rt, R + NK_R_COLOR, 8 * NR_RT_COLORS); memcpy(&rt[NR_RT_COLORS], R + NK_R_DEPTH_RT, 8);
    if (!t_nk.rtf || memcmp(rt, t_nk.rt, sizeof rt)) {
        pthread_mutex_lock(&g_nk_mu); pthread_mutex_lock(&g_nr_mu);
        t_nk.rtf = nr_rtfmt_note(g_nk, g_reg, rt, rt[NR_RT_COLORS]);
        pthread_mutex_unlock(&g_nr_mu); pthread_mutex_unlock(&g_nk_mu);
        memcpy(t_nk.rt, rt, sizeof rt); NR_INC(g_nki.rt_lock);
    }
    k.rtfmt = t_nk.rtf;
    const unsigned slot = (unsigned)(nr_dkey_hash(&k) >> 20) & (NK_LEAN - 1);
    uint32_t id, why;
    if (t_nkl[slot].id && !memcmp(&t_nkl[slot].k, &k, sizeof k)) { id = t_nkl[slot].id; why = t_nkl[slot].why; NR_INC(g_nki.key_hit); }
    else {
        int fresh = 0;
        pthread_mutex_lock(&g_nk_mu);
        const nr_kent *e = nr_key_note(g_nk, NULL, &k, &fresh);
        if (!e && fresh < 0) { pthread_mutex_lock(&g_nr_mu); e = nr_key_note(g_nk, g_reg, &k, &fresh); pthread_mutex_unlock(&g_nr_mu); }
        id = e ? e->id : 0; why = e ? e->why : NR_NE_LOST;
        pthread_mutex_unlock(&g_nk_mu);
        t_nkl[slot].k = k; t_nkl[slot].id = id; t_nkl[slot].why = why; NR_INC(g_nki.key_lock);
    }
    if (kind == NR_DS_FULL) why |= NR_NE_FULL;
    if (dirty) {
        int32_t tex[NR_TS_STAGES][NR_TS_SLOTS], smp[NR_TS_STAGES][NR_TS_SLOTS];
        memcpy(tex[0], R + NK_R_TEX_VS, sizeof tex[0]); memcpy(tex[1], R + NK_R_TEX_FS, sizeof tex[1]);
        memcpy(smp[0], R + NK_R_SMP_VS, sizeof smp[0]); memcpy(smp[1], R + NK_R_SMP_FS, sizeof smp[1]);
        const uint64_t h = nr_texset_hash(tex, smp);
        if (h == t_nk.ts_h && t_nk.ts_id) NR_INC(g_nki.ts_same);
        else {
            int tf = 0;
            pthread_mutex_lock(&g_nk_mu);
            const nr_texset *ts = nr_texset_note(g_nk, NULL, h, tex, &tf);
            if (!ts && tf < 0) { pthread_mutex_lock(&g_nr_mu); ts = nr_texset_note(g_nk, g_reg, h, tex, &tf); pthread_mutex_unlock(&g_nr_mu); }
            t_nk.ts_id = ts ? ts->id : 0; t_nk.ts_h = h;
            pthread_mutex_unlock(&g_nk_mu);
            NR_INC(g_nki.ts_lock);
        }
    }
    NR_INC(g_nki.draws);
    t_nk.idp = 1; t_nk.id_key = id; t_nk.id_ts = t_nk.ts_id; t_nk.id_why = why; t_nk.id_site = si >= 0 ? (uint32_t)si : 0xffu; NRN_NOTE(r);
}
// NR-09: the identity of the NVN draw being recorded on this thread (nvn_nb_draw.inc nbd_draw, inside the engine's draw bracket, after
// dm_nr_tap_draw). 1 = taken (once); 0 = this NVN draw followed no engine flush on this thread: the record stays a plain OP_X_NB_DRAW.
int dm_nr_draw_ident(uint32_t *key, uint32_t *texset, unsigned *why, unsigned *site) {
    if (!g_nr_draw || !t_nk.idn) { if (g_nr_draw) NR_INC(g_nki.untaken); return 0; }
    t_nk.idn = 0;
    *key = t_nk.id_key; *texset = t_nk.id_ts; *why = t_nk.id_why; *site = t_nk.id_site;
    NR_INC(g_nki.taken);
    return 1;
}

// ---- the draw: the entry of the texture flush ----
static void nk_draw(uint64_t r, uint64_t lr) {
    if (g_nr_key == 3) {
        if (g_nrd_lean == 1) { nrd_ident_lr(r, lr); return; }   // NR-09R DM_NR_DIRECT=1: the same key id without a lock (hle_dos2_nr_direct.inc)
        nk_ident(r, lr);
        if (__builtin_expect(g_nrd_lean == 2, 0)) nrd_ident_cmp(r, lr);   // NR-09R DM_NR_DIRECT=2: the lean identity against this one
        return;
    }
    const int si = nk_site(lr), kind = si >= 0 ? k_nk_site[si].kind : NR_DS_UNKNOWN, out = t_nk.rc <= 0;
    const uint8_t *R = (const uint8_t *)(uintptr_t)r;
    int dirty = 0;
    for (int i = 0; i < 4; i++) if ((int8_t)R[k_nk_dirty[i]] <= (int8_t)R[k_nk_dirty[i] + 1]) dirty = 1;
    if (t_nk.r != r) { t_nk.r = r; t_nk.ts_id = 0; t_nk.ts_h = 0; t_nk.rtf = 0; dirty = 1; }
    dirty |= t_nk.carry;
    if (!nr_dsite_is_draw(kind)) {   // code 33 / compute: no NVN draw follows; the dirty ranges it consumes go to the next draw
        t_nk.carry = dirty;
        pthread_mutex_lock(&g_nk_mu);
        g_nks.calls++; g_nks.kind[kind][out]++; if (si >= 0) g_nks.site[si][out]++;
        pthread_mutex_unlock(&g_nk_mu);
        return;
    }
    t_nk.carry = 0;
    if (t_nk.pend) NR_INC(g_nka.pend_lost);   // the previous engine draw of this thread reached no NVN draw
    nr_dkey k; memset(&k, 0, sizeof k);
    memcpy(&k.shader, R + NK_R_SHADER, 8); memcpy(&k.blend, R + NK_R_BLEND, 8); memcpy(&k.blend_arg, R + NK_R_BLEND_ARG, 4);
    memcpy(&k.depth, R + NK_R_DEPTH, 8); memcpy(&k.raster, R + NK_R_RASTER, 8); memcpy(&k.vformat, R + NK_R_VFORMAT, 8);
    float aref; memcpy(&aref, R + NK_R_ALPHA_REF, 4);
    k.prim = R[NK_R_PRIM]; k.alpha_test = aref >= 0.f;
    uint64_t rt[NR_RT_COLORS + 1]; memcpy(rt, R + NK_R_COLOR, 8 * NR_RT_COLORS); memcpy(&rt[NR_RT_COLORS], R + NK_R_DEPTH_RT, 8);
    int32_t tex[NR_TS_STAGES][NR_TS_SLOTS], smp[NR_TS_STAGES][NR_TS_SLOTS]; uint64_t h = 0;
    if (dirty || g_nr_key == 2) {
        memcpy(tex[0], R + NK_R_TEX_VS, sizeof tex[0]); memcpy(tex[1], R + NK_R_TEX_FS, sizeof tex[1]);
        memcpy(smp[0], R + NK_R_SMP_VS, sizeof smp[0]); memcpy(smp[1], R + NK_R_SMP_FS, sizeof smp[1]);
        h = nr_texset_hash(tex, smp);
    }
    const uint64_t old_h = t_nk.ts_h;
    const int bad_clean = !dirty && g_nr_key == 2 && old_h && h != old_h;   // counted, then taken as a change so the set stays right
    if (bad_clean) dirty = 1;
    uint64_t rec[3] = {0, 0, 0}; int have_rec = 0;
    if (g_nr_key == 2) { uint64_t ra; memcpy(&ra, R + NK_R_SHADER_REC, 8); have_rec = ra && nr_peek(rec, ra, sizeof rec); }
    char sname[64]; sname[0] = 0;

    pthread_mutex_lock(&g_nk_mu);
    if (!t_nk.rtf || memcmp(rt, t_nk.rt, sizeof rt)) {
        pthread_mutex_lock(&g_nr_mu); t_nk.rtf = nr_rtfmt_note(g_nk, g_reg, rt, rt[NR_RT_COLORS]); pthread_mutex_unlock(&g_nr_mu);
        memcpy(t_nk.rt, rt, sizeof rt);
    }
    k.rtfmt = t_nk.rtf;
    int fresh = 0;
    const nr_kent *e = nr_key_note(g_nk, NULL, &k, &fresh);
    if (!e && fresh < 0) { pthread_mutex_lock(&g_nr_mu); e = nr_key_note(g_nk, g_reg, &k, &fresh); pthread_mutex_unlock(&g_nr_mu); }
    unsigned why = e ? e->why : NR_NE_LOST;
    if (kind == NR_DS_FULL) why |= NR_NE_FULL;
    nk_stat *s = &g_nks;
    s->calls++; s->draws++; s->kind[kind][out]++; if (si >= 0) s->site[si][out]++;
    if (fresh == 1) s->new_keys++;
    if (!why) s->eligible++;
    for (int b = 0; b < NR_NE_BITS; b++) if (why >> b & 1) s->why[b]++;
    const nr_texset *ts = NULL;
    if (dirty) {
        s->dirty++;
        int tf = 0;
        ts = nr_texset_note(g_nk, NULL, h, tex, &tf);
        if (!ts && tf < 0) { pthread_mutex_lock(&g_nr_mu); ts = nr_texset_note(g_nk, g_reg, h, tex, &tf); pthread_mutex_unlock(&g_nr_mu); }
        if (tf == 1) s->new_ts++;
        const uint32_t id = ts ? ts->id : 0;
        if (id != t_nk.ts_id || !id) s->changed++;
        t_nk.ts_id = id; t_nk.ts_h = h;
    } else {
        nr_texset_count(g_nk, t_nk.ts_id);
        ts = nr_texset_get(g_nk, t_nk.ts_id);
    }
    if (!ts) s->ts_none++;
    else { if (ts->nrt) s->ts_rt++; if (ts->nunknown) s->ts_unknown++; }
    if (g_nr_key == 2) {
        s->chk_draws++;
        if (bad_clean) s->bad_clean++;
        pthread_mutex_lock(&g_nr_mu);
        const nr_shader *sh = nr_shader_get(g_reg, k.shader);
        t_nk.exp[0] = sh ? sh->prog[NR_STAGE_VS] : 0; t_nk.exp[1] = sh ? sh->prog[NR_STAGE_FS] : 0;
        memcpy(t_nk.key[0], sh ? sh->key[NR_STAGE_VS] : "", sh ? 17 : 1); memcpy(t_nk.key[1], sh ? sh->key[NR_STAGE_FS] : "", sh ? 17 : 1);
        if (sh) snprintf(sname, sizeof sname, "%s", sh->name[0] ? sh->name : "(unnamed)");
        pthread_mutex_unlock(&g_nr_mu);
        if (sh && have_rec && (rec[1] != t_nk.exp[0] || rec[2] != t_nk.exp[1])) s->bad_record++;
        else have_rec = 0;   // nothing to report
    }
    const uint32_t id_key = e ? e->id : 0;   // NR-09 (read under g_nk_mu: the entry does not move, the copy is for after the unlock)
    pthread_mutex_unlock(&g_nk_mu);
    t_nk.pend = 1; t_nk.shader = k.shader; t_nk.site = si;
    if (g_nr_draw) { t_nk.idp = 1; t_nk.id_key = id_key; t_nk.id_ts = t_nk.ts_id; t_nk.id_why = why; t_nk.id_site = si >= 0 ? (uint32_t)si : 0xffu; NRN_NOTE(r); }
    if (g_nr_key == 2) {
        char d[256];
        if (bad_clean) {
            snprintf(d, sizeof d, "renderer %#llx, %s draw at main+%#x: the texture / sampler slots changed with every dirty range clean (set hash %016llx -> %016llx)",
                     (unsigned long long)r, nr_dsite_name(kind), si >= 0 ? k_nk_site[si].bl : 0u, (unsigned long long)old_h, (unsigned long long)h);
            nk_red("clean range", d); }
        if (have_rec) {
            snprintf(d, sizeof d, "shader %#llx '%s': engine record VS %#llx FS %#llx, registry VS %#llx FS %#llx", (unsigned long long)k.shader, sname,
                     (unsigned long long)rec[1], (unsigned long long)rec[2], (unsigned long long)t_nk.exp[0], (unsigned long long)t_nk.exp[1]);
            nk_red("shader record", d); }
    }
}
NR_HOOK nr_h_tf(NR_A) {   // the renderer's texture flush (r): every engine draw path calls it right before its NVN draw call
    nk_draw(a0, (uint64_t)(uintptr_t)__builtin_return_address(0));   // the hook is jumped to: x30 is still the game's return address
    return g_o_tf(NR_F);
}

// ---- the NVN side (hle_nvn.c tap lines, any thread; only while g_dm_nr != 0) ----
void dm_nr_tap_bind(const void *cb, const void *prog, int stages) {
    if (g_nr_key != 2) return;
    int i = 0;
    while (i < t_nk.ncb && t_nk.cb[i].cb != cb) i++;
    if (i == t_nk.ncb) { if (t_nk.ncb < 4) t_nk.ncb++; else { memmove(&t_nk.cb[0], &t_nk.cb[1], 3 * sizeof t_nk.cb[0]); i = 3; } t_nk.cb[i].cb = cb; t_nk.cb[i].p[0] = t_nk.cb[i].p[1] = t_nk.cb[i].p[2] = 0; }
    const uint64_t p = (uint64_t)(uintptr_t)prog;
    if (stages & 1) t_nk.cb[i].p[0] = p;
    if (stages & 2) t_nk.cb[i].p[1] = p;
    if (stages & 0x20) t_nk.cb[i].p[2] = p;
}
static int nk_seen(uint64_t p) {   // 1 = this program's key was already checked (g_nk_mu held); marks it
    for (unsigned i = (unsigned)(p >> 4) & 4095, n = 0; n < 4096; i = (i + 1) & 4095, n++) {
        if (g_nk_seen[i].p == p) return 1;
        if (!g_nk_seen[i].p) { g_nk_seen[i].p = p; return 0; }
    }
    return 1;   // table full: no more key checks
}
void dm_nr_tap_draw(const void *cb) {
    if (!g_nr_key) return;
    t_nk.idn = t_nk.idp; t_nk.idp = 0;   // NR-09: the identity belongs to this NVN draw only
    if (g_nr_key == 3) return;
    if (!t_nk.pend) { NR_INC(g_nka.nvn_other); return; }
    t_nk.pend = 0; NR_INC(g_nka.nvn_engine);
    if (g_nr_key != 2) return;
    int i = 0;
    while (i < t_nk.ncb && t_nk.cb[i].cb != cb) i++;
    char d[300];
    if (i == t_nk.ncb) NR_INC(g_nka.unbound);   // nothing bound on this command buffer by this thread (bound elsewhere / before the tap)
    else {
        NR_INC(g_nka.chk_bound);
        if (t_nk.cb[i].p[0] != t_nk.exp[0] || t_nk.cb[i].p[1] != t_nk.exp[1]) {
            NR_INC(g_nka.bad_bound);
            snprintf(d, sizeof d, "shader %#llx at main+%#x: bound VS %#llx FS %#llx, the handle's programs VS %#llx FS %#llx", (unsigned long long)t_nk.shader,
                     t_nk.site >= 0 ? k_nk_site[t_nk.site].bl : 0u, (unsigned long long)t_nk.cb[i].p[0], (unsigned long long)t_nk.cb[i].p[1],
                     (unsigned long long)t_nk.exp[0], (unsigned long long)t_nk.exp[1]);
            nk_red("programs bound at the draw", d);
        }
    }
    for (int st = 0; st < 2; st++) {
        const uint64_t p = t_nk.exp[st];
        if (!p) continue;
        pthread_mutex_lock(&g_nk_mu); const int seen = nk_seen(p); pthread_mutex_unlock(&g_nk_mu);
        if (seen) continue;
        char now[17]; dm_nr_prog_key((const void *)(uintptr_t)p, now);
        NR_INC(g_nka.keys_checked);
        if (strcmp(now, t_nk.key[st])) {
            NR_INC(g_nka.bad_packkey);
            snprintf(d, sizeof d, "%s program %#llx of shader %#llx: key '%s' at nvnProgramSetShaders, '%s' at its first draw", st ? "FS" : "VS",
                     (unsigned long long)p, (unsigned long long)t_nk.shader, t_nk.key[st], now);
            nk_red("pack key", d);
        }
    }
}

// ---- census and dump ----
static void nk_dump(void) {   // g_nk_mu held; takes g_nr_mu
    const char *d = getenv("DM_NR_DUMP_DIR");
    if (!d || !*d) d = getenv("DM_RUNTIME_DIR");
    if (!d || !*d) d = "dm";
    char path[1100], tmp[1108]; int rows = 0;
    pthread_mutex_lock(&g_nr_mu);
    for (int w = 0; w <= NR_KTSV_N; w++) {
        snprintf(path, sizeof path, "%s/%s", d, w < NR_KTSV_N ? nr_ktsv_name(w) : "nr_draw_sites.tsv"); snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *o = fopen(tmp, "w");
        if (!o) { LOG("【红】[nr] key dump: cannot write %s", tmp); break; }
        if (w < NR_KTSV_N) { const int n = nr_keys_write_tsv(g_nk, g_reg, w, o); rows += n > 0 ? n : 0; }
        else {
            fprintf(o, "site\tkind\tin_lists\toutside_lists\n");
            for (int i = 0; i < NK_SITES; i++, rows++) fprintf(o, "%#x\t%s\t%llu\t%llu\n", k_nk_site[i].bl, nr_dsite_name(k_nk_site[i].kind),
                                                               (unsigned long long)g_nks.site[i][0], (unsigned long long)g_nks.site[i][1]);
        }
        if (fclose(o) || rename(tmp, path)) { LOG("【红】[nr] key dump: cannot finish %s", path); break; }
    }
    pthread_mutex_unlock(&g_nr_mu);
    LOG("[nr] key dump: %d rows in %s/nr_{keys,texsets,rtfmts,names,draw_sites}.tsv", rows, d);
}
void nvn_nr_draw_stats(uint64_t out[2]) __attribute__((weak));   // nvn_nb_draw.inc: records written with the tail / without an identity
static void nk_draw_report(uint64_t frames) {   // NR-09: the hand-over of identities to the draw records
    static uint64_t w[8];
    const uint64_t c[8] = {NR_GET(g_nki.draws), NR_GET(g_nki.key_hit), NR_GET(g_nki.key_lock), NR_GET(g_nki.ts_same), NR_GET(g_nki.ts_lock), NR_GET(g_nki.rt_lock),
                           NR_GET(g_nki.taken), NR_GET(g_nki.untaken)};
    uint64_t rec[2] = {0, 0};
    if (nvn_nr_draw_stats) nvn_nr_draw_stats(rec);
    const double f = frames ? (double)frames : 1.;
    pthread_mutex_lock(&g_nk_mu);
    const uint32_t nkey = g_nk->nkey, nts = g_nk->nts;
    pthread_mutex_unlock(&g_nk_mu);
    LOG("[nr] draw ident (DM_NR_DRAW=%d, DM_NR_KEY %s): OP_X_NR_DRAW records %.1f a frame (engine draws in the NB bracket without an identity: %.1f a frame, "
        "written as plain OP_X_NB_DRAW); identities taken %.1f a frame, asked for with none pending %.1f; keys %u, texture sets %u%s",
        g_nr_draw, g_nr_key == 3 ? "off: identity only" : g_nr_key == 2 ? "2" : "1", rec[0] / f, rec[1] / f, (c[6] - w[6]) / f, (c[7] - w[7]) / f, nkey, nts,
        g_nr_key == 3 ? "" : " (the census lines above count the same draws)");
    if (g_nr_key == 3)
        LOG("[nr] draw ident, the flush hook (identity only): %.1f engine draws a frame; key from the thread's last keys %.1f%% (table lock %.1f a frame), "
            "texture range dirty with the same set %.1f a frame, set looked up %.1f a frame, RT format set looked up %.1f a frame",
            (c[0] - w[0]) / f, c[0] - w[0] ? 100. * (c[1] - w[1]) / (double)(c[0] - w[0]) : 0., (c[2] - w[2]) / f, (c[3] - w[3]) / f, (c[4] - w[4]) / f, (c[5] - w[5]) / f);
    memcpy(w, c, sizeof w);
}
static void nk_report(uint64_t now, uint64_t frames, int dump) {   // from nr_report (g_nr_rep_mu held, g_nr_mu not)
    (void)now;
    if (g_nr_draw) nk_draw_report(frames);
    if (g_nr_nocall) nc_report(frames);   // NR-23
    if (g_nrd) nrd_report(frames);        // NR-09R
    if (g_nr_key == 3) return;   // NR-09 identity only: no census
    nk_stat c, w;
    pthread_mutex_lock(&g_nk_mu);
    c = g_nks; w = g_nks_w; g_nks_w = g_nks;
    const uint32_t nkey = g_nk->nkey, wkeys = g_nk->win_keys, nts = g_nk->nts, wts = g_nk->win_ts, nrtf = g_nk->nrtf, ntid = g_nk->ntid;
    const uint64_t lost[4] = {g_nk->key_lost, g_nk->ts_lost, g_nk->rtf_lost, g_nk->tid_lost};
    if (dump) nk_dump();
    nr_keys_window(g_nk);
    pthread_mutex_unlock(&g_nk_mu);
    static uint64_t a_eng, a_oth;   // the atomics at the last report (g_nr_rep_mu serialises this function)
    const uint64_t eng = NR_GET(g_nka.nvn_engine), oth = NR_GET(g_nka.nvn_other);
    const double f = frames ? (double)frames : 1.;
    const uint64_t dr = c.draws - w.draws, d1 = dr ? dr : 1;
    uint64_t in = 0, ou = 0;
    for (int k = 0; k < NR_DS_N; k++) if (nr_dsite_is_draw(k)) { in += c.kind[k][0] - w.kind[k][0]; ou += c.kind[k][1] - w.kind[k][1]; }
    char by[600]; int n = 0; by[0] = 0;
    for (int k = 0; k < NR_DS_N && n < (int)sizeof by - 64; k++) {
        const uint64_t v = c.kind[k][0] + c.kind[k][1] - w.kind[k][0] - w.kind[k][1];
        if (v) n += snprintf(by + n, sizeof by - (size_t)n, "%s%s %.1f", n ? ", " : "", nr_dsite_name(k), v / f);
    }
    char why[300]; int m = 0; why[0] = 0;
    for (int b = 0; b < NR_NE_BITS && m < (int)sizeof why - 40; b++) {
        const uint64_t v = c.why[b] - w.why[b];
        if (v) m += snprintf(why + m, sizeof why - (size_t)m, "%s%s %.1f%%", m ? ", " : "", nr_ne_name(b), 100. * v / d1);
    }
    LOG("[nr] draw keys (DM_NR_KEY=%d, %llu frames): engine draws %.1f/frame (in command lists %.1f%%, outside the lists %.1f%%; %s), NVN draws %.1f/frame "
        "(%.1f after an engine draw, %.1f other = UI / direct NVN), engine draws with no NVN draw %llu; keys %u distinct (%u this window, %llu new), "
        "eligible %.1f%% of draws%s%s%s",
        g_nr_key, (unsigned long long)frames, dr / f, 100. * in / d1, 100. * ou / d1, n ? by : "-", (eng - a_eng + oth - a_oth) / f, (eng - a_eng) / f, (oth - a_oth) / f,
        (unsigned long long)NR_GET(g_nka.pend_lost), nkey, wkeys, (unsigned long long)(c.new_keys - w.new_keys), 100. * (c.eligible - w.eligible) / d1,
        m ? " (not: " : "", why, m ? ")" : "");
    LOG("[nr] texture sets (DM_NR_KEY=%d): dirty range at %.1f%% of draws, set changed at %.1f%%, %u distinct (%u this window, %llu new); draws whose set has a "
        "render target %.1f%% (hazard check needed), an id the registry does not know %.1f%%, no set %.1f%%; RT format sets %u, pool ids mapped %u; "
        "flush without a draw %.1f/frame, compute flushes %.1f/frame; lost: keys %llu, sets %llu, rt formats %llu, ids %llu",
        g_nr_key, 100. * (c.dirty - w.dirty) / d1, 100. * (c.changed - w.changed) / d1, nts, wts, (unsigned long long)(c.new_ts - w.new_ts),
        100. * (c.ts_rt - w.ts_rt) / d1, 100. * (c.ts_unknown - w.ts_unknown) / d1, 100. * (c.ts_none - w.ts_none) / d1, nrtf, ntid,
        (c.kind[NR_DS_FLUSH][0] + c.kind[NR_DS_FLUSH][1] - w.kind[NR_DS_FLUSH][0] - w.kind[NR_DS_FLUSH][1]) / f,
        (c.kind[NR_DS_COMPUTE][0] + c.kind[NR_DS_COMPUTE][1] - w.kind[NR_DS_COMPUTE][0] - w.kind[NR_DS_COMPUTE][1]) / f,
        (unsigned long long)lost[0], (unsigned long long)lost[1], (unsigned long long)lost[2], (unsigned long long)lost[3]);
    if (g_nr_key == 2)
        LOG("[nr] key checks (DM_NR_KEY=2): %llu draws; programs bound at the NVN draw = the shader handle's programs: %llu checked, %llu mismatches, "
            "%llu with nothing bound by this thread; engine shader record differs %llu; slots changed under a clean dirty range %llu; pack key at the "
            "first draw = at SetShaders: %llu programs, %llu differ",
            (unsigned long long)c.chk_draws, (unsigned long long)NR_GET(g_nka.chk_bound), (unsigned long long)NR_GET(g_nka.bad_bound),
            (unsigned long long)NR_GET(g_nka.unbound), (unsigned long long)c.bad_record, (unsigned long long)c.bad_clean,
            (unsigned long long)NR_GET(g_nka.keys_checked), (unsigned long long)NR_GET(g_nka.bad_packkey));
    a_eng = eng; a_oth = oth;
}

// ---- install (from dm_dos2_nr_install, after the registry hooks; guest code is not running yet) ----
static const struct nr_site k_nr_key_site[] = {
    {0x924730, {0xa9ba6ffc /* stp x28,x27,[sp,#-0x60]! */, 0xa90167fa, 0xa9025ff8, 0xa90357f6}, "texture flush", (void *)nr_h_tf, &g_o_tf},
};
static void nk_install(module_t *m, int mode, int rc_ok) {
    g_nk = nr_keys_new();
    if (!g_nk) { LOG("【红】[nr] DM_NR_KEY: no memory for the key tables - off"); return; }
    g_nk_base = (uint64_t)(uintptr_t)m->base;
    const struct nr_site *s = &k_nr_key_site[0];
    void *o = NULL;
    if (!dn_words_ok(m, s->off, s->pro)) { LOG("【红】[nr] main+%#x (%s) differs: not DOS2 update v786432 main (build 4979b200): DM_NR_KEY off", s->off, s->name); nr_keys_free(g_nk); g_nk = NULL; return; }
    g_nr_key = mode;   // before the hook goes in (it reads it)
    if (dn_hook(m, s->off, s->pro, nc_tf_hook(s->hook), &o)) { g_nr_key = 0; LOG("【红】[nr] main+%#x (%s): mmap / mprotect failed: DM_NR_KEY off", s->off, s->name); return; }
    *s->orig = (nr_fn)o;
    if (mode == 3) { LOG("[nr] DM_NR_DRAW without DM_NR_KEY: the renderer's texture flush main+%#x hooked for the draw identity only (key id + texture set id per engine draw, no census, no checks)", NK_TF); return; }
    LOG("[nr] DM_NR_KEY=%d (%s): the renderer's texture flush main+%#x hooked (%d call sites known), the state cache read at every engine draw, the NVN "
        "draw after it matched; command-list depth %s; nothing is drawn differently",
        mode, mode == 2 ? "count + checks" : "count", NK_TF, NK_SITES, rc_ok ? "from the ExecuteRenderCommand chain" : "UNKNOWN (no ExecuteRenderCommand handler): every draw counts as outside the lists");
}
// NR-09 (DM_NR_DRAW 1 / 2, docs/dos2/nr/NR-09.md): the identity of every engine draw rides in its draw record (OP_X_NR_DRAW = the
// OP_X_NB_DRAW of DM_NB_OPS 36 / 37 + nrd_tail). Needs the flush hook: with DM_NR_KEY 1 / 2 the census hands the ids over, without it the
// hook goes in as "identity only". The backend reads DM_NR_DRAW itself (xbg_nr_draw.inc); 1 and 2 differ only there.
void nvn_nr_draw_enable(int on) __attribute__((weak));   // dm/gfx/nvn/nvn_nb_draw.inc
static void nk_draw_install(module_t *m, int rc_ok) {
    const char *e = getenv("DM_NR_DRAW");
    const int v = e ? atoi(e) : 0;
    if (!v) return;
    if (v != 1 && v != 2) { LOG("【红】[nr] DM_NR_DRAW=%s: only 0 (off), 1 (on) and 2 (compute + compare, the old path draws) exist - off", e); return; }
    if (!nvn_nr_draw_enable) { LOG("【红】[nr] DM_NR_DRAW=%d: no record entry point (nvn_nr_draw_enable) - off", v); return; }
    if (!g_nr_key) nk_install(m, 3, rc_ok);
    if (!g_nr_key) { LOG("【红】[nr] DM_NR_DRAW=%d: the texture flush hook is not in (see above) - no identities, every draw stays an OP_X_NB_DRAW", v); return; }
    g_nr_draw = v;
    nvn_nr_draw_enable(1);
    LOG("[nr] DM_NR_DRAW=%d (NR-09): the key id and texture set id of every engine draw ride in its draw record (OP_X_NR_DRAW = OP_X_NB_DRAW + NBD_F_NR + 16 bytes); "
        "needs DM_NB=1 with DM_NB_OPS bits 36 / 37 (the draw bracket); the backend %s", v, v == 2 ? "computes the native draw, lets the old path draw and compares" : "executes the native draw (nr_draw), per-draw fallback to draw()");
}
