// [DOS2] NR-NATIVE step E (docs/dos2/nr/NR-NATIVE-E.md): resources by engine events - the host half. DM_NRN_EV (default 0). Included by
// hle_dos2_nr.c right after its hook shape (NR_HOOK / NR_A), ahead of its hooks (they call the NRNEV_* lines). main = main.raw of update
// v786432, build 4979b200; r = the renderer.
//   DM_NRN_EV=0 / unset   nothing: no table, the three hooks below are not installed, every NRNEV_* line is one load and a branch.
//   DM_NRN_EV=1 | 2       every place the engine writes the bytes of a vertex / index buffer, or announces that it will, is an EVENT into
//                         one table (dm/gfx/nr/nr_nrn_ev.h): the journal by guest range (read by the translation thread:
//                         xbg_nr_nrn_ev.inc - 2 compares it with the page tracker, 1 takes it instead), the epochs by engine handle
//                         (for the game thread), the constant ring's wrap count. The engine's functions run unchanged.
// The events (addresses read in the binary, see the doc):
//   creation   CreateVertexBuffer 0x917630 / CreateIndexBuffer 0x9179d0 (NR-01's hooks, after the original: the initial data is in)
//   destroy    0x91db40 / 0x91d890 (NR-01's hooks)
//   GPU copy   0x9218c0 (r, update): update +0 handle, +8 kind (0 vertex, 1 index, 2 another table), +0xc offset, +0x28 size - the
//              record of nvnCommandBufferCopyBufferToBuffer is being written; the handle's epoch moves now (a draw recorded after it reads
//              the new bytes), the journal of the range moves when the backend executes the copy (it journals its own writes)
//   CPU copy   opcode 0x2f (vertex -> vertex, destination handle at +0x10), opcode 0x33 (index -> index, destination at +0x10) through
//              the shared ExecuteRenderCommand chain, before AND after the memcpy; the same two as immediate calls 0x9412f0 / 0x9414f0
//              (list +0x30 == 0: the list does not record), hooked
//   scratch    opcodes 0x26..0x2a (immediate-mode draws) and every draw packet: [r+0x8d0] / [r+0x8e0] are marked "written outside every event"
//   texture    creation (NR-01's commit) and opcode 0x2c (rows copied into the texture's mapped memory, handle at +8): the handle's epoch
//   ring       head [dev+0x25a60] sampled at every engine draw packet: lower than the last one = the ring wrapped
#include "../gfx/nr/nr_nrn_ev.h"

static int g_nrnev;                                   // DM_NRN_EV as armed: 0 / 1 / 2 (read by every NRNEV_* line)
static nrn_ev *g_nrnev_t;
static pthread_mutex_t g_nrnev_mu = PTHREAD_MUTEX_INITIALIZER;   // the one writer of the coverage at a time
enum { NRNEVC_CREATED, NRNEVC_NOMEM, NRNEVC_DESTROYED, NRNEVC_GPU, NRNEVC_GPU_UNKNOWN, NRNEVC_GPU_OTHER, NRNEVC_CPU, NRNEVC_CPU_UNKNOWN, NRNEVC_CPU_IMM, NRNEVC_SCRATCH,
       NRNEVC_TEX_NEW, NRNEVC_TEX_WRITE, NRNEVC_N };
static uint64_t g_nrnevc[NRNEVC_N];                   // atomics
static uint64_t g_nrnev_scr[2];                       // the scratch handles already marked (atomics)
#define NRNEV_ON() __builtin_expect(g_nrnev, 0)
// the backend's half reads and writes the same object (xbg_nr_nrn_ev.inc); NULL while the switch is off
const void *dm_nrn_ev_table(void);
const void *dm_nrn_ev_table(void) { return __atomic_load_n(&g_nrnev, __ATOMIC_ACQUIRE) ? g_nrnev_t : NULL; }

static inline uint64_t nrnev_r64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static inline uint32_t nrnev_r32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }

// A vertex / index buffer the engine call created (kind NRS_KIND_*; b: the call's bracket - the NVN buffer it initialised is the storage).
static void nrnev_created(int kind, uint64_t handle, unsigned usage, int has_init, const nr_bracket *b) {
    if (!b || !b->nbuf || !b->buf_mem || !b->buf_size) { NR_INC(g_nrnevc[NRNEVC_NOMEM]); return; }
    const int cls = usage == NR_USAGE_DYNAMIC ? NRNE_C_DYNAMIC : has_init ? NRNE_C_STATIC : NRNE_C_NOINIT;
    pthread_mutex_lock(&g_nrnev_mu);
    nrn_ev_created(g_nrnev_t, kind == NRS_KIND_VB ? NRNE_K_VB : NRNE_K_IB, cls, handle, b->buf_mem, b->buf_size);
    pthread_mutex_unlock(&g_nrnev_mu);
    NR_INC(g_nrnevc[NRNEVC_CREATED]);
}
static void nrnev_destroyed(int kind, uint64_t handle) {
    pthread_mutex_lock(&g_nrnev_mu);
    const uint64_t s = nrn_ev_destroyed(g_nrnev_t, kind == NRS_KIND_VB ? NRNE_K_VB : NRNE_K_IB, handle);
    pthread_mutex_unlock(&g_nrnev_mu);
    (void)s; NR_INC(g_nrnevc[NRNEVC_DESTROYED]);
}
static void nrnev_tex(uint64_t handle) { nrn_ev_created(g_nrnev_t, NRNE_K_TEX, NRNE_C_STATIC, handle, 0, 0); NR_INC(g_nrnevc[NRNEVC_TEX_NEW]); }
#define NRNEV_CREATED(kind, handle, usage, has_init, b) do { if (NRNEV_ON()) nrnev_created(kind, handle, usage, has_init, b); } while (0)
#define NRNEV_DESTROYED(kind, handle) do { if (NRNEV_ON()) nrnev_destroyed(kind, handle); } while (0)
#define NRNEV_TEX(handle) do { if (NRNEV_ON()) nrnev_tex(handle); } while (0)

static void nrnev_cpu(int kind, uint64_t handle, int imm) {   // a memcpy into the buffer's mapped pointer starts / has ended
    if (nrn_ev_updated(g_nrnev_t, kind, handle, 0, 0, 1)) { NR_INC(g_nrnevc[imm ? NRNEVC_CPU_IMM : NRNEVC_CPU]); return; }
    pthread_mutex_lock(&g_nrnev_mu);   // the handle table lost the handle (another one of its slot): its range from the coverage
    const uint64_t s = nrn_ev_updated_slow(g_nrnev_t, kind, handle);
    pthread_mutex_unlock(&g_nrnev_mu);
    NR_INC(g_nrnevc[s ? (imm ? NRNEVC_CPU_IMM : NRNEVC_CPU) : NRNEVC_CPU_UNKNOWN]);
}
static void nrnev_scratch(uint64_t r) {
    for (int k = 0; k < 2; k++) {
        const uint64_t h = nrnev_r64(r + (k ? 0x8e0u : 0x8d0u));
        if (nrne_h_none(h) || __atomic_load_n(&g_nrnev_scr[k], __ATOMIC_RELAXED) == h) continue;
        pthread_mutex_lock(&g_nrnev_mu);
        nrn_ev_scratch(g_nrnev_t, NRNE_K_VB, h); nrn_ev_scratch(g_nrnev_t, NRNE_K_IB, h);
        pthread_mutex_unlock(&g_nrnev_mu);
        __atomic_store_n(&g_nrnev_scr[k], h, __ATOMIC_RELAXED);
        NR_INC(g_nrnevc[NRNEVC_SCRATCH]);
    }
}
// The shared ExecuteRenderCommand chain (from nr_rc_pre / the post handlers; list +0x38 = the renderer). post: after the original.
static void nrnev_rc(uint64_t list, unsigned op, uint64_t pkt, int post) {
    switch (op) {
    case 0x2f: nrnev_cpu(NRNE_K_VB, nrnev_r64(pkt + 0x10), 0); break;
    case 0x33: nrnev_cpu(NRNE_K_IB, nrnev_r64(pkt + 0x10), 0); break;
    case 0x2c: if (nrn_ev_updated(g_nrnev_t, NRNE_K_TEX, nrnev_r64(pkt + 8), 0, 0, 0)) NR_INC(g_nrnevc[NRNEVC_TEX_WRITE]); break;
    case 0x26: case 0x27: case 0x28: case 0x29: case 0x2a: if (!post) { const uint64_t r = nrnev_r64(list + 0x38); if (r) nrnev_scratch(r); } break;
    case 0x23: case 0x24: case 0x25:
        // (the scratch handles are looked at here too: every ask of the backend follows a draw packet, so the scratch buffer is marked before
        // the first ask whatever path the first immediate-mode draw took)
        if (!post) { const uint64_t r = nrnev_r64(list + 0x38), dev = r ? nrnev_r64(r + 0xb70) : 0; if (r) nrnev_scratch(r); if (dev) nrne_ring_note(&g_nrnev_t->ring, nrnev_r32(dev + 0x25a60)); }
        break;
    default: break;
    }
}
#define NRNEV_RC(list, op, pkt) do { if (NRNEV_ON() && (op) - 0x23u <= 0x10u) nrnev_rc(list, op, pkt, 0); } while (0)
static void nrnev_rc_post(uint64_t list, unsigned op, uint64_t pkt, uint64_t len) { (void)len; if (NRNEV_ON() && (op == 0x2f || op == 0x33 || op == 0x2c)) nrnev_rc(list, op, pkt, 1); }

// ---- the three hooks of this step (installed only with the switch on) ----
static nr_fn g_o_ev_ub, g_o_ev_cvb, g_o_ev_cib;
NR_HOOK nr_h_ev_ub(NR_A) {   // 0x9218c0 (r, update): the GPU copy of a dynamic update is being recorded
    if (a1) {
        const unsigned kind = *(const uint8_t *)(uintptr_t)(a1 + 8);
        if (kind > 1) NR_INC(g_nrnevc[NRNEVC_GPU_OTHER]);
        else if (nrn_ev_updated(g_nrnev_t, kind ? NRNE_K_IB : NRNE_K_VB, nrnev_r64(a1), nrnev_r32(a1 + 0xc), (uint64_t)(int64_t)(int32_t)nrnev_r32(a1 + 0x28), 0)) NR_INC(g_nrnevc[NRNEVC_GPU]);
        else NR_INC(g_nrnevc[NRNEVC_GPU_UNKNOWN]);
    }
    return g_o_ev_ub(NR_F);
}
NR_HOOK nr_h_ev_cvb(NR_A) {   // 0x9412f0 (list, u64 *src, u64 *dst): opcode 0x2f, or the memcpy at once when the list does not record
    const int imm = a0 && a2 && !*(const uint8_t *)(uintptr_t)(a0 + 0x30);
    const uint64_t h = imm ? nrnev_r64(a2) : 0;
    if (imm) nrnev_cpu(NRNE_K_VB, h, 1);
    const uint64_t v = g_o_ev_cvb(NR_F);
    if (imm) nrnev_cpu(NRNE_K_VB, h, 1);
    return v;
}
NR_HOOK nr_h_ev_cib(NR_A) {   // 0x9414f0 (list, u64 *src, u64 *dst): opcode 0x33, or the memcpy at once
    const int imm = a0 && a2 && !*(const uint8_t *)(uintptr_t)(a0 + 0x30);
    const uint64_t h = imm ? nrnev_r64(a2) : 0;
    if (imm) nrnev_cpu(NRNE_K_IB, h, 1);
    const uint64_t v = g_o_ev_cib(NR_F);
    if (imm) nrnev_cpu(NRNE_K_IB, h, 1);
    return v;
}
static const struct nrnev_site { uint32_t off; uint32_t pro[4]; const char *name; void *hook; nr_fn *orig; } k_nrnev_site[] = {
    {0x9218c0, {0xd10383ff /* sub sp,sp,#0xe0 */, 0xf90053f7, 0xa90b57f6, 0xa90c4ff4}, "buffer update (GPU copy)", (void *)nr_h_ev_ub, &g_o_ev_ub},
    {0x9412f0, {0xa9be4ff4 /* stp x20,x19,[sp,#-0x20]! */, 0xa9017bfd, 0x910043fd, 0x3940c008 /* ldrb w8,[x0,#0x30] */}, "vertex buffer copy", (void *)nr_h_ev_cvb, &g_o_ev_cvb},
    {0x9414f0, {0xa9be4ff4 /* stp x20,x19,[sp,#-0x20]! */, 0xa9017bfd, 0x910043fd, 0x3940c008 /* ldrb w8,[x0,#0x30] */}, "index buffer copy", (void *)nr_h_ev_cib, &g_o_ev_cib},
};

// From dm_dos2_nr_install, before the NR-01 hooks go in (guest code is not running yet). 1 = armed.
static int nrnev_arm(module_t *m) {
    const char *e = getenv("DM_NRN_EV");
    const int v = e && *e ? atoi(e) : 0;   // 0 on every device unless asked (the backend's policy function xbg_nrn_ev_default says the same)
    if (!v) return 0;
    if (v != 1 && v != 2) { LOG("【红】[nr] native events: DM_NRN_EV=%s is not 0 / 1 / 2: off", e); return 0; }
    g_nrnev_t = (nrn_ev *)calloc(1, sizeof *g_nrnev_t);
    if (!g_nrnev_t) { LOG("【红】[nr] native events: no memory for the event table (%zu bytes): DM_NRN_EV off", sizeof *g_nrnev_t); return 0; }
    int in = 0;
    for (unsigned i = 0; i < sizeof k_nrnev_site / sizeof k_nrnev_site[0]; i++) {
        const struct nrnev_site *s = &k_nrnev_site[i];
        void *o = NULL;
        if (!dn_words_ok(m, s->off, s->pro) || dn_hook(m, s->off, s->pro, s->hook, &o)) continue;
        *s->orig = (nr_fn)o; in++;
    }
    if (in != (int)(sizeof k_nrnev_site / sizeof k_nrnev_site[0])) {
        // a hook that is in stays in (it forwards to the original); without all three an update could go unannounced: the table stays empty
        LOG("【红】[nr] native events: %d of %zu hooks in (main differs from update v786432 build 4979b200, or mmap / mprotect failed): DM_NRN_EV off", in,
            sizeof k_nrnev_site / sizeof k_nrnev_site[0]);
        return 0;
    }
    __atomic_store_n(&g_nrnev, v, __ATOMIC_RELEASE);
    LOG("[nr] native events (NR-NATIVE step E, front end): DM_NRN_EV=%d - every write the engine makes or orders to a vertex / index buffer is an event (creation, destroy, "
        "the GPU copy of an update main+0x9218c0, the CPU copies of opcodes 0x2f / 0x33 and main+0x9412f0 / +0x9414f0, the immediate-mode scratch buffer [r+0x8d0] marked "
        "as never covered), texture creation and opcode 0x2c by handle, the constant ring's wraps; table %zu KB; the engine's functions run unchanged",
        v, sizeof *g_nrnev_t >> 10);
    return 1;
}
// From nr_report (every 10 s).
static void nrnev_report(uint64_t frames) {
    if (!g_nrnev) return;
    uint64_t c[NRNEVC_N];
    for (int i = 0; i < NRNEVC_N; i++) c[i] = __atomic_exchange_n(&g_nrnevc[i], 0, __ATOMIC_RELAXED);
    const double f = frames ? (double)frames : 1.;
    const nrn_ev *t = g_nrnev_t;
    LOG("[nr] native events (front end, DM_NRN_EV=%d, last 10 s): buffers created %llu (no storage seen %llu), destroyed %llu; updates by GPU copy %.1f a frame (handle not "
        "known %llu, another table %llu); CPU copies into a buffer %.1f a frame (opcodes) + %llu at once (handle not known %llu); scratch buffers marked %llu; textures "
        "created %llu, opcode-0x2c writes %.1f a frame; ring wraps %llu; covered ranges %llu (added %llu, removed %llu, dead entries pushed out %llu, table full %llu); "
        "events %llu in all",
        g_nrnev, (unsigned long long)c[NRNEVC_CREATED], (unsigned long long)c[NRNEVC_NOMEM], (unsigned long long)c[NRNEVC_DESTROYED], (double)c[NRNEVC_GPU] / f,
        (unsigned long long)c[NRNEVC_GPU_UNKNOWN], (unsigned long long)c[NRNEVC_GPU_OTHER], (double)c[NRNEVC_CPU] / f, (unsigned long long)c[NRNEVC_CPU_IMM],
        (unsigned long long)c[NRNEVC_CPU_UNKNOWN], (unsigned long long)c[NRNEVC_SCRATCH], (unsigned long long)c[NRNEVC_TEX_NEW], (double)c[NRNEVC_TEX_WRITE] / f,
        (unsigned long long)nrn_ev_ring_epoch(t), (unsigned long long)NRNE_LOAD(((nrn_ev *)t)->cov.n), (unsigned long long)t->cov.adds, (unsigned long long)t->cov.dels,
        (unsigned long long)t->cov.evicted, (unsigned long long)t->cov.full, (unsigned long long)NRNE_LOAD(((nrn_ev *)t)->j.serial));
}
