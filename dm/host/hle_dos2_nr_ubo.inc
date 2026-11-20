// [DOS2] NR-09U: the engine's uniform blocks of a direct draw as native code (DM_NR_UBO, default 0; docs/dos2/nr/NR-09U.md). Included by
// hle_dos2_nr.c after hle_dos2_nr_nocall.inc and before hle_dos2_nr_direct.inc (whose nrd_draw_take calls nru_take). main = main.raw of
// update v786432, build 4979b200.
//   DM_NR_UBO=0 / unset  nothing: nrd_draw_take calls the engine's 0x926f90(r) + 0x925740(r) as before; the NUB line of the
//                        BindUniformBuffer body is a load and a not-taken branch.
//   DM_NR_UBO=1          needs DM_NR_DIRECT=1 (its native draw handler is where the two functions are called). For a direct draw the two
//                        functions do not run as machine code: the model of dm/gfx/nr/nr_ubo.h (proven bit-exact against the machine code by
//                        tests/nr_ubo_oracle.py) rebuilds the dirty / stale blocks into the device's constant ring with the engine's own lock
//                        protocol on the same words (r's read lock, the ring's writer lock, the head's fetch-add - other threads that use the
//                        ring meet the very lock), copies the shadows and writes the blocks' fields; its BindUniformBuffer calls go through the
//                        body of the entry point at the start of the direct draw's one big-stack call (nrd_job), before the texture flush - no
//                        thunk, no wrapper, no stack switch of their own. The ring NR-06 hands to the backend in place gets the very bytes.
//                        DM_NR_UBO_AB=1: draws alternate in blocks of 64 between the model and the engine's two functions (both timed: the
//                        '[nr] ubo' line gives ns a draw for each, the model's including its calls through the body).
//   DM_NR_UBO=2          the engine's two functions run; the model's simulation runs first on the same state (no store, no lock) and predicts
//                        every block's 0x40 bytes, the ring head, the bytes each rebuild copies and the BindUniformBuffer calls; after the engine
//                        the four are compared (red lines, first 16 + totals every 10 s). Another thread's allocation between the two moves the
//                        head: counted as raced, the offsets are not compared then.
//   There is no =3: =1 makes the stores and the calls the engine made; nothing is left stale that a poison could show.
// A draw the model refuses (nr_ubo.h: no device / context / ring, r's lock owned by a thread, too many blocks, a null pointer, a block size
// over 64 KB, a store that would land on a word the model reads once) runs the engine's two functions, untouched: decided before any store
// (but r's read lock, given back the engine's way).
#include "../gfx/nr/nr_ubo.h"
#include "../gfx/nr/nr_direct.h"   // NRDP_DRAW
#include "../gfx/nvn/nvn_nr_ubo.h"

static int g_nru_want, g_nru_ab;
static int g_nru_live;   // atomic: 0 = not looked at yet, 1 = the engine's BindUniformBuffer pointer is our thunk, -1 = not (never native)
static uint64_t g_nru_base;
enum { NRUC_DRAWS, NRUC_NATIVE, NRUC_ENGINE_AB, NRUC_CALLS, NRUC_REB_G, NRUC_REB_S, NRUC_WAITS, NRUC_YIELD, NRUC_NOTLIVE, NRUC_TIMED, NRUC_NS,
       NRUC_TIMED_E, NRUC_NS_E, NRUC_CMP, NRUC_RACED, NRUC_BAD, NRUC_BAD_BLOCK, NRUC_BAD_HEAD, NRUC_BAD_RING, NRUC_BAD_CALLS, NRUC_DIRTY_F, NRUC_SAME, NRUC_REFUSE,
       NRUC_N = NRUC_REFUSE + NRU_WHY_N };
static uint64_t g_nruc[NRUC_N];   // atomics, fed from the thread's counters once in 64 draws
static __thread struct { uint32_t n; int timed; uint64_t t_model; uint64_t c[NRUC_N]; nru_run *run; } t_nru;
static unsigned g_nru_red;
static uint64_t g_nru_tot[4];     // =2 totals: compared, raced, bad, simulation refused (atomics)

static void nru_count_flush(void) {
    for (int i = 0; i < NRUC_N; i++) if (t_nru.c[i]) { __atomic_fetch_add(&g_nruc[i], t_nru.c[i], __ATOMIC_RELAXED); t_nru.c[i] = 0; }
}
static int nru_verify(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int v = __atomic_load_n(&g_nru_live, __ATOMIC_RELAXED);
    if (!v) {
        const void *th = NULL;
        const int ok = nvn_nru_ready(&th);
        const uint64_t got = nru_r64((const uint8_t *)(uintptr_t)(g_nru_base + NRU_SLOT_BIND));
        v = ok && th && got == (uint64_t)(uintptr_t)th ? 1 : -1;
        if (v > 0) LOG("[nr] ubo: the engine's nvnCommandBufferBindUniformBuffer pointer (main+%#x) is the thunk of the body the native uniform blocks call", NRU_SLOT_BIND);
        else LOG("【红】[nr] ubo: %s (main+%#x holds %#llx, our thunk %p): the uniform blocks stay the engine's machine code", !ok ? "the BindUniformBuffer entry point is not "
                 "implemented by the body the native blocks call" : "the engine's BindUniformBuffer pointer is not our thunk", NRU_SLOT_BIND, (unsigned long long)got, th);
        __atomic_store_n(&g_nru_live, v, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&mu);
    return v > 0;
}
static inline int nru_live(void) {
    const int v = __atomic_load_n(&g_nru_live, __ATOMIC_RELAXED);
    if (__builtin_expect(v != 0, 1)) return v > 0;
    return nru_verify();
}
static void nru_red(const char *what, uint64_t r, const nru_run *s, const nru_call *g, int ng, int at, int bad) {
    if (__atomic_fetch_add(&g_nru_red, 1, __ATOMIC_RELAXED) >= 16) return;
    if (bad & NRU_BAD_CALLS) {
        const nru_call *m = at >= 0 && at < s->ncall ? &s->call[at] : NULL, *e = at >= 0 && at < ng ? &g[at] : NULL;
        LOG("【红】[nr] ubo check: %s - r %#llx, call %d of %d (model) / %d (engine): model stage %u slot %u addr %#llx size %#llx, engine stage %u slot %u addr %#llx size %#llx",
            what, (unsigned long long)r, at, s->ncall, ng, m ? m->stage : 0u, m ? m->slot : 0u, m ? (unsigned long long)m->addr : 0ull, m ? (unsigned long long)m->size : 0ull,
            e ? e->stage : 0u, e ? e->slot : 0u, e ? (unsigned long long)e->addr : 0ull, e ? (unsigned long long)e->size : 0ull);
    } else if (bad & NRU_BAD_BLOCK) LOG("【红】[nr] ubo check: %s - r %#llx: block %#llx (the %d-th the model looked at) is not the bytes the model predicted", what,
                                        (unsigned long long)r, at >= 0 && at < s->nvisit ? (unsigned long long)s->vaddr[at] : 0ull, at);
    else LOG("【红】[nr] ubo check: %s - r %#llx (copy %d: ring %#llx, %u bytes)", what, (unsigned long long)r, at,
             at >= 0 && at < s->ncopy ? (unsigned long long)s->copy[at].dst : 0ull, at >= 0 && at < s->ncopy ? s->copy[at].n : 0u);
}

// nrd_draw_take (hle_dos2_nr_direct.inc), a direct draw with [r+0x17e0] != 0, in place of the engine's fill(r) = 0x926f90 and bind(r) =
// 0x925740. Returns the run whose calls the direct draw's big-stack job makes first (=1, the model ran: the engine's functions must not
// run), or NULL (they ran here: refused, the engine's block of DM_NR_UBO_AB, the pointer not verified, or =2).
static const nru_run *nru_take(uint64_t r, nc_guest_fn fill, nc_guest_fn bind) {
    nru_run *s = t_nru.run;
    if (__builtin_expect(!s, 0)) { s = t_nru.run = (nru_run *)calloc(1, sizeof *s); if (!s) { fill(r); bind(r); return NULL; } }
    const uint32_t n = t_nru.n++;
    const int timed = !(n & 63);
    if (timed) nru_count_flush();
    t_nru.c[NRUC_DRAWS]++; t_nru.timed = 0;
    if (!nru_live()) { t_nru.c[NRUC_NOTLIVE]++; fill(r); bind(r); return NULL; }
    if (g_nru == 1 && g_nru_ab && (n >> 6 & 1)) {   // DM_NR_UBO_AB: this block of 64 is the engine's (timed the same way, calls through the thunks)
        t_nru.c[NRUC_ENGINE_AB]++;
        const uint64_t t0 = timed ? nr_now() : 0;
        fill(r); bind(r);
        if (timed) { t_nru.c[NRUC_TIMED_E]++; t_nru.c[NRUC_NS_E] += nr_now() - t0; }
        return NULL;
    }
    if (g_nru == 1) {
        const uint64_t t0 = timed ? nr_now() : 0;
        s->same_on = g_nrd_ubd == 2;   // NR-UBD=2: count the rebuilds whose shadow equals the previous ring bytes
        if (!nru_draw(r, s, 0)) { t_nru.c[NRUC_REFUSE + (s->why > 0 && s->why < NRU_WHY_N ? s->why : 0)]++; fill(r); bind(r); return NULL; }
        t_nru.c[NRUC_NATIVE]++; t_nru.c[NRUC_CALLS] += (uint64_t)s->ncall; t_nru.c[NRUC_REB_G] += s->reb_g; t_nru.c[NRUC_REB_S] += s->reb_s;
        t_nru.c[NRUC_DIRTY_F] += s->reb_dirty; t_nru.c[NRUC_SAME] += s->reb_same;
        t_nru.c[NRUC_WAITS] += (uint64_t)(s->spin_w + s->spin_d + s->spin_r); t_nru.c[NRUC_YIELD] += s->yields;
        if (timed) { t_nru.timed = 1; t_nru.t_model = nr_now() - t0; }   // nru_job_calls adds the calls' time
        return s;
    }
    // =2: the simulation, then the engine's two functions with their calls captured, then the comparison
    if (!nru_draw(r, s, 1)) {
        t_nru.c[NRUC_REFUSE + (s->why > 0 && s->why < NRU_WHY_N ? s->why : 0)]++; __atomic_fetch_add(&g_nru_tot[3], 1, __ATOMIC_RELAXED);
        fill(r); bind(r); return NULL;
    }
    const uint64_t t0 = timed ? nr_now() : 0;
    nvn_nru_cap_begin();
    fill(r); bind(r);
    const nru_call *got = NULL;
    const int ngot = nvn_nru_cap_end(&got);
    if (timed) { t_nru.c[NRUC_TIMED_E]++; t_nru.c[NRUC_NS_E] += nr_now() - t0; }
    int at = -1, raced = 0;
    const int bad = nru_check(s, r, got, ngot < NRU_CALLS_MAX ? ngot : NRU_CALLS_MAX, &at, &raced) | (ngot > NRU_CALLS_MAX ? NRU_BAD_CALLS : 0);
    t_nru.c[NRUC_CMP]++; __atomic_fetch_add(&g_nru_tot[0], 1, __ATOMIC_RELAXED);
    if (raced) { t_nru.c[NRUC_RACED]++; __atomic_fetch_add(&g_nru_tot[1], 1, __ATOMIC_RELAXED); }
    if (bad) {
        t_nru.c[NRUC_BAD]++; __atomic_fetch_add(&g_nru_tot[2], 1, __ATOMIC_RELAXED);
        if (bad & NRU_BAD_BLOCK) t_nru.c[NRUC_BAD_BLOCK]++;
        if (bad & NRU_BAD_HEAD) t_nru.c[NRUC_BAD_HEAD]++;
        if (bad & NRU_BAD_RING) t_nru.c[NRUC_BAD_RING]++;
        if (bad & NRU_BAD_CALLS) t_nru.c[NRUC_BAD_CALLS]++;
        nru_red(bad & NRU_BAD_CALLS ? "the BindUniformBuffer calls differ" : bad & NRU_BAD_BLOCK ? "a block's bytes differ" : "the ring bytes of a rebuild differ", r, s, got, ngot, at, bad);
    }
    return NULL;
}
// In the direct draw's big-stack job (nrd_job), first: the model's BindUniformBuffer calls through the body.
static void nru_job_calls(const nru_run *s) {
    const uint64_t t0 = t_nru.timed ? nr_now() : 0;
    nvn_nru_exec(s->call, s->ncall);
    if (t_nru.timed) { t_nru.c[NRUC_TIMED]++; t_nru.c[NRUC_NS] += t_nru.t_model + (nr_now() - t0); t_nru.timed = 0; }
}

// ---- the 10 s line (from nrd_report) ----
static void nru_report(uint64_t frames) {
    uint64_t c[NRUC_N];
    for (int i = 0; i < NRUC_N; i++) c[i] = __atomic_exchange_n(&g_nruc[i], 0, __ATOMIC_RELAXED);
    const double f = frames ? (double)frames : 1., tm = c[NRUC_TIMED] ? (double)c[NRUC_TIMED] : 1., te = c[NRUC_TIMED_E] ? (double)c[NRUC_TIMED_E] : 1.;
    char why[400]; int o = 0;
    for (int k = 1; k < NRU_WHY_N && o < (int)sizeof why - 60; k++) if (c[NRUC_REFUSE + k]) o += snprintf(why + o, sizeof why - (size_t)o, "%s%s %.2f", o ? ", " : "", k_nru_why[k], (double)c[NRUC_REFUSE + k] / f);
    if (!o) snprintf(why, sizeof why, "none");
    const double draws = (double)c[NRUC_DRAWS] / f;
    if (g_nru == 1) {
        LOG("[nr] ubo (DM_NR_UBO=1, NR-09U, %llu frames): direct draws with uniform blocks %.1f a frame, native %.1f (the engine's 0x926f90 + 0x925740 not run) with %.1f "
            "BindUniformBuffer calls a frame through the body (%.2f a draw, none through a thunk, no stack switch of their own), blocks rebuilt into the ring a frame: global %.1f, "
            "shader %.1f; left to the engine a frame: %s; pointer not verified %.2f%s; lock waits (another thread held the ring / r's lock) %.2f a frame, YieldThread %llu",
            (unsigned long long)frames, draws, (double)c[NRUC_NATIVE] / f, (double)c[NRUC_CALLS] / f, c[NRUC_NATIVE] ? (double)c[NRUC_CALLS] / (double)c[NRUC_NATIVE] : 0.,
            (double)c[NRUC_REB_G] / f, (double)c[NRUC_REB_S] / f, why, (double)c[NRUC_NOTLIVE] / f, g_nru_ab ? "; DM_NR_UBO_AB: the engine's blocks of 64" : "",
            (double)c[NRUC_WAITS] / f, (unsigned long long)c[NRUC_YIELD]);
        if (g_nrd_ubd == 2)
            LOG("[nr] ubo + ubd (DM_NR_UBO=1, DM_NR_UBD=2): rebuilds of a block dirty again in the frame it was last rebuilt in %.1f a frame, %.1f of them (%.1f%%) with the "
                "shadow equal to the bytes of its previous ring range (counted only: every rebuild copies)", (double)c[NRUC_DIRTY_F] / f, (double)c[NRUC_SAME] / f,
                c[NRUC_DIRTY_F] ? 100. * (double)c[NRUC_SAME] / (double)c[NRUC_DIRTY_F] : 0.);
        const double xn = (double)c[NRUC_NS] / tm, xe = (double)c[NRUC_NS_E] / te;
        if (g_nru_ab && c[NRUC_TIMED_E])
            LOG("[nr] ubo, ns a draw (1 in 64 timed): native (the model + its calls through the body) %.0f (%llu), the engine's two functions with their BindUniformBuffer calls "
                "through the thunks %.0f (%llu): %.1f us a frame saved on this thread at %.1f draws a frame", xn, (unsigned long long)c[NRUC_TIMED], xe, (unsigned long long)c[NRUC_TIMED_E],
                (xe - xn) * draws / 1000., draws);
        else LOG("[nr] ubo, ns a draw (1 in 64 timed): native (the model + its calls through the body) %.0f (%llu); the engine's: DM_NR_UBO_AB=1, or the '[nr] direct, ns a direct "
                 "draw' line of a run without this switch ('the engine's uniform blocks')", xn, (unsigned long long)c[NRUC_TIMED]);
    } else {
        LOG("[nr] ubo check (DM_NR_UBO=2, NR-09U, %llu frames; the engine's two functions run): direct draws with uniform blocks %.1f a frame, compared %.1f (raced %.2f: another "
            "thread allocated in between, offsets not compared), bad %.2f (blocks %.2f, ring bytes %.2f, calls %.2f); not compared (the simulation refused) a frame: %s; totals: %llu "
            "compared, raced %llu, bad %llu, not compared %llu; ns a draw of the engine's two functions with the capture (1 in 64): %.0f",
            (unsigned long long)frames, draws, (double)c[NRUC_CMP] / f, (double)c[NRUC_RACED] / f, (double)c[NRUC_BAD] / f, (double)c[NRUC_BAD_BLOCK] / f,
            (double)c[NRUC_BAD_RING] / f, (double)c[NRUC_BAD_CALLS] / f, why, (unsigned long long)__atomic_load_n(&g_nru_tot[0], __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_nru_tot[1], __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&g_nru_tot[2], __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_nru_tot[3], __ATOMIC_RELAXED), (double)c[NRUC_NS_E] / te);
    }
}

// ---- install (dm_dos2_nr_install; guest code is not running yet) ----
static void nru_arm(module_t *m) {
    const char *e = getenv("DM_NR_UBO");
    const int v = e ? atoi(e) : 0;
    if (!v) return;
    if (v != 1 && v != 2) { LOG("【红】[nr] DM_NR_UBO=%s: only 0 (off), 1 (the uniform blocks of a direct draw natively) and 2 (the engine's + the model, compared) exist - off", e); return; }
    static const nru_code code[NRU_CODE_N] = NRU_CODE_TABLE;
    for (int i = 0; i < NRU_CODE_N; i++) {
        const uint64_t h = m->text_end >= code[i].end ? nru_fnv(m->base + code[i].off + 16, code[i].end - code[i].off - 16) : 0;
        if (h == code[i].fnv) continue;
        LOG("【红】[nr] DM_NR_UBO=%d: main+%#x..%#x (%s) is not the analysed code (hash %016llx, expected %016llx): not DOS2 update v786432 main (build 4979b200) - off",
            v, code[i].off, code[i].end, code[i].name, (unsigned long long)h, (unsigned long long)code[i].fnv);
        return;
    }
    g_nru_base = (uint64_t)(uintptr_t)m->base;
    const char *ab = getenv("DM_NR_UBO_AB");
    g_nru_ab = v == 1 && ab && atoi(ab) == 1;
    g_nru_want = v;
}
// After nrd_install: the direct draw's native handler is where the two functions are called.
static void nru_install(int direct, int direct_parts) {
    const int v = g_nru_want;
    if (!v) return;
    if (direct != 1 || !(direct_parts & NRDP_DRAW)) {
        LOG("【红】[nr] DM_NR_UBO=%d: needs DM_NR_DIRECT=1 with its draw part (its native draw handler calls the uniform-block functions; it is %d, parts %#x) - off", v, direct, direct_parts);
        return;
    }
    nvn_nru_enable(v);
    g_nru = v;   // last: nrd_draw_take reads it
    LOG("[nr] DM_NR_UBO=%d (NR-09U): %s%s", v,
        v == 1 ? "the uniform blocks of a direct draw (main+0x926f90 global + main+0x925740 shader blocks) run as native code: the model rebuilds the blocks into the device's "
                 "constant ring with the engine's lock protocol, its BindUniformBuffer calls go through the body at the start of the draw's big-stack call"
               : "the engine's uniform-block functions run; the model's simulation runs first on the same state and the blocks, the ring head, the copied bytes and the "
                 "BindUniformBuffer calls are compared",
        g_nru_ab ? "; DM_NR_UBO_AB=1: blocks of 64 draws alternate native / engine, both timed" : "");
}
