// [NR-16] CPU attribution of the HLE NVN layer on the game's recording thread (docs/dos2/nr/NR-16.md). Included by hle_nvn.c right
// before nvnQueueSubmitCommands; nvn_lookup calls nvn_p2_install once, after the big-stack wrapper is in.
//
//   DM_NVN_PROF  0 / unset  nothing (the slots are untouched)
//                1          the old whole-run timing wrapper (installed by a constructor, i.e. only when the variable is in the process
//                           environment at load time: on Android dm/env comes later, so =1 never took effect there - and with the
//                           D3-lineage big-stack wrapper its slot index convention would not match; kept as it was for the Mac host)
//                2          sampled timing of the thread that submits (the game's recording thread): every implemented slot but
//                           nvnQueuePresentTexture goes through nvn_prof2_entry (nvn_entry.S). On every other thread, and on that one
//                           outside its window, the entry is a compare of tpidr_el0 and a jump on (a few ns). One submission interval
//                           in DM_NVN_PROF_EVERY (16) is a window: from the end of one nvnQueueSubmitCommands to the end of the next,
//                           every NVN call of that thread is timed with cntvct_el0 (19.2 MHz on Android) and counted; the thread's own
//                           CPU time and the wall time of the interval are taken at both ends. The fixed cost of a timed call inside the
//                           stamps (measured at install through a no-op slot) is taken out per call.
//   DM_NVN_PROF_EVERY  1..1024 (16): one timed submission interval in N.
// Line "[cpuprof] nvn ..." + "[cpuprof] nvn top ..." every 10 s from the recording thread; the install and calibration once.
// Cost (Mi 14 class, 14.6k calls a frame): untimed ~3 ns a call = ~0.04 ms a frame; a timed interval ~+1 ms, 1 in 16 = ~0.07 ms a frame.
extern void nvn_prof2_entry(void);
uint64_t nvn_p2_tp;                   // tpidr_el0 of the thread whose window is open, else 0 (nvn_prof2_entry compares it)
uint64_t nvn_p2_acc[NVN_NFUNC][2];    // {cntvct ticks, calls} of that thread in the open window; written by that thread only
static int g_p2_on, g_p2_every = 16, g_p2_isub = -1, g_p2_ipres = -1;
static uint8_t g_p2_waits[NVN_NFUNC];   // slots that wait by design (GPU / display): their time is reported apart from the HLE work
static double g_p2_ns_tick = 1.0, g_p2_floor_ticks;   // ns per cntvct tick; the fixed cost of one timed call inside its stamps
static uint64_t nvn_p2_frames(void);
static inline uint64_t p2_ticks(void) {
#if defined(__aarch64__)
    uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v;
#else
    return 0;
#endif
}
static inline uint64_t p2_tp(void) {
#if defined(__aarch64__)
    return (uint64_t)(uintptr_t)__builtin_thread_pointer();   // tpidr_el0: the host's thread pointer on guest threads too (the guest's is a key slot)
#else
    return 0;
#endif
}
static uint64_t p2_thread_cpu_ns(void) { struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void nvn_nop(void);
#if defined(DM_GAME_ETERNAL) || defined(DM_GAME_XC2) || defined(DM_GAME_D3)
#define P2_BIG 1
#endif
// The slot's effective implementation (under the big-stack wrapper when it is in)
static void *p2_impl(unsigned i) {
#ifdef P2_BIG
    if (NVN_BIG_FN(nvn_slot[i].fn)) return nvn_real_big[i];   // the big-stack wrapper or NR-28's lean entry
#endif
    return nvn_slot[i].fn;
}
static void nvn_p2_install(void) {
    static int done; if (done) return; done = 1;
    const char *e = getenv("DM_NVN_PROF"); if (!e || atoi(e) != 2) return;
#if !defined(__aarch64__) || defined(__APPLE__)
    LOG("[cpuprof] DM_NVN_PROF=2 needs an aarch64 Linux / Android host (cntvct_el0): off"); return;
#endif
    if (getenv("DM_NVN_TRACE")) { LOG("[cpuprof] DM_NVN_PROF=2 with DM_NVN_TRACE: the trace wrapper is in, sampled timing off"); return; }
    if ((e = getenv("DM_NVN_PROF_EVERY")) && *e) { int v = atoi(e); g_p2_every = v < 1 ? 1 : v > 1024 ? 1024 : v; }
#if defined(__aarch64__)
    { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); g_p2_ns_tick = f ? 1e9 / (double)f : 1.0; }
#endif
    int nop = -1; unsigned n = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) {
        if (!strcmp(nvn_names[i], "nvnQueueSubmitCommands")) g_p2_isub = (int)i;
        if (!strcmp(nvn_names[i], "nvnQueuePresentTexture")) { g_p2_ipres = (int)i; continue; }   // present waits for the display: not HLE work
        if (nvn_slot[i].fn == (void *)nvn_unimpl_entry) continue;
        g_p2_waits[i] = !strcmp(nvn_names[i], "nvnSyncWait") || !strcmp(nvn_names[i], "nvnWindowAcquireTexture") || !strcmp(nvn_names[i], "nvnQueueFinish") ||
                        !strcmp(nvn_names[i], "nvnEventWait") || !strcmp(nvn_names[i], "nvnTextureReadTexels") || !strcmp(nvn_names[i], "nvnTextureWriteTexels");
        if (nop < 0 && p2_impl(i) == (void *)nvn_nop) nop = (int)i;
#ifdef P2_BIG
        if (NVN_BIG_FN(nvn_slot[i].fn)) { nvn_real[i] = nvn_real_big[i]; nvn_real_big[i] = (void *)nvn_prof2_entry; n++; continue; }   // under it (or under NR-28's lean entry): x16 = slot index
#endif
        nvn_real[i] = nvn_slot[i].fn; nvn_slot[i].fn = (void *)nvn_prof2_entry; n++;   // x16 = slot address
    }
    // Calibration through a no-op slot's own thunk (its call counter gets 2 x 2048 more): the timed path's stamps-to-stamps floor, and
    // the wall cost of a timed and of an untimed call (thunk + big stack + this entry + the no-op)
    double untimed = 0, timed = 0;
    if (nop >= 0) {
        void (*f)(void) = (void (*)(void))nvn_thunk_tab()[nop];   // the table nvn_lookup hands out (NR-28: plain with DM_NR_LEANCALL=1)
        for (int k = 0; k < 256; k++) f();
        uint64_t a = now_ns(); for (int k = 0; k < 2048; k++) f(); untimed = (double)(now_ns() - a) / 2048;
        nvn_p2_acc[nop][0] = nvn_p2_acc[nop][1] = 0; nvn_p2_tp = p2_tp();
        a = now_ns(); for (int k = 0; k < 2048; k++) f(); timed = (double)(now_ns() - a) / 2048;
        nvn_p2_tp = 0;
        g_p2_floor_ticks = nvn_p2_acc[nop][1] ? (double)nvn_p2_acc[nop][0] / (double)nvn_p2_acc[nop][1] : 0;
        memset(nvn_p2_acc, 0, sizeof nvn_p2_acc);
    }
    g_p2_on = 1;
    LOG("[cpuprof] DM_NVN_PROF=2: %u NVN slots timed on the submitting thread, 1 submission interval in %d; calibration through %s: "
        "untimed call %.1f ns, timed call %.1f ns, %.1f ns of it inside the stamps (taken out per call); cntvct %.2f ns a tick",
        n, g_p2_every, nop >= 0 ? nvn_names[nop] + 3 : "(no no-op slot: floor 0)", untimed, timed, g_p2_floor_ticks * g_p2_ns_tick, g_p2_ns_tick);
}
// The followed thread and the windows (all touched by the submitting thread; another thread submitting while it is active is ignored)
static uint64_t g_p2_follow, g_p2_last_ns, g_p2_k, g_p2_rep_ns, g_p2_rep_frames, g_p2_subs, g_p2_open_cpu, g_p2_open_wall;
static int g_p2_tid;
static uint64_t g_p2_wn, g_p2_wcpu, g_p2_wwall, g_p2_wsub, g_p2_wfn[NVN_NFUNC][2];
// At the start of nvnQueueSubmitCommands: a stamp when this thread's window is open (the submit itself is HLE work of the window)
static inline uint64_t nvn_p2_enter(void) { return g_p2_on && nvn_p2_tp && nvn_p2_tp == p2_tp() ? p2_ticks() : 0; }
static void nvn_p2_report(uint64_t now) {
    const double sec = (double)(now - g_p2_rep_ns) / 1e9; const uint64_t fr = nvn_p2_frames(), frames = fr - g_p2_rep_frames;
    if (g_p2_wn) {
        const double wn = (double)g_p2_wn, fl = g_p2_floor_ticks;
        double calls = 0, ticks = 0, wticks = 0;
        for (unsigned i = 0; i < NVN_NFUNC; i++) { calls += (double)g_p2_wfn[i][1]; double t = (double)g_p2_wfn[i][0] - fl * (double)g_p2_wfn[i][1]; ticks += t > 0 ? t : 0; if (g_p2_waits[i] && t > 0) wticks += t; }
        const double nvn_ms = ticks * g_p2_ns_tick / 1e6 / wn, sub_ms = (double)g_p2_wsub * g_p2_ns_tick / 1e6 / wn, cpu_ms = (double)g_p2_wcpu / 1e6 / wn,
                     wall_ms = (double)g_p2_wwall * g_p2_ns_tick / 1e6 / wn, per_frame = frames ? (double)g_p2_subs / (double)frames : 0;
        LOG("[cpuprof] nvn (DM_NVN_PROF=2, thread %d, 1 submission interval in %d timed, %llu timed in %.0f s, %.2f submissions a frame): NVN %.3f ms an interval "
            "in %.0f calls (%.0f ns a call, the wrapper's %.0f ns taken out) + nvnQueueSubmitCommands %.3f ms; the thread's CPU %.3f ms, wall %.3f ms in the "
            "interval: NVN = %.0f%% of its CPU; of the NVN time %.3f ms in calls that wait (SyncWait / AcquireTexture / QueueFinish / EventWait / Read-WriteTexels)",
            g_p2_tid, g_p2_every, (unsigned long long)g_p2_wn, sec, per_frame, nvn_ms, calls / wn, calls ? ticks * g_p2_ns_tick / calls : 0, fl * g_p2_ns_tick,
            sub_ms, cpu_ms, wall_ms, cpu_ms > 0 ? 100.0 * (nvn_ms + sub_ms) / cpu_ms : 0, wticks * g_p2_ns_tick / 1e6 / wn);
        char line[1400]; size_t o = 0; line[0] = 0;
        for (int k = 0; k < 12 && o < sizeof line - 120; k++) {
            int best = -1; double bt = 0;
            for (unsigned i = 0; i < NVN_NFUNC; i++) { double t = (double)g_p2_wfn[i][0] - fl * (double)g_p2_wfn[i][1]; if (g_p2_wfn[i][1] && t > bt) { bt = t; best = (int)i; } }
            if (best < 0) break;
            const double c = (double)g_p2_wfn[best][1] / wn;
            o += (size_t)snprintf(line + o, sizeof line - o, " %s %.0f x %.0f ns = %.3f ms;", nvn_names[best] + 3, c, bt * g_p2_ns_tick / (double)g_p2_wfn[best][1], bt * g_p2_ns_tick / 1e6 / wn);
            g_p2_wfn[best][0] = 0; g_p2_wfn[best][1] = 0;
        }
        LOG("[cpuprof] nvn top (an interval, calls x ns = ms):%s", line);
    } else LOG("[cpuprof] nvn (DM_NVN_PROF=2): no timed interval in %.0f s (%llu submissions on thread %d)", sec, (unsigned long long)g_p2_subs, g_p2_tid);
    g_p2_wn = g_p2_wcpu = g_p2_wwall = g_p2_wsub = g_p2_subs = 0; memset(g_p2_wfn, 0, sizeof g_p2_wfn);
    g_p2_rep_ns = now; g_p2_rep_frames = fr;
}
// At the end of nvnQueueSubmitCommands: close this thread's open window, open the next one every g_p2_every submissions
static void nvn_p2_leave(uint64_t t0) {
    if (!g_p2_on) return;
    const uint64_t tp = p2_tp(), now = now_ns();
    if (g_p2_follow != tp) {
        if (g_p2_follow && now - g_p2_last_ns < 2000000000ull) return;   // another thread submits while the followed one is active
        if (g_p2_follow) LOG("[cpuprof] nvn: the submitting thread is now %d (was %d)", (int)gettid(), g_p2_tid);
        g_p2_follow = tp; g_p2_tid = (int)gettid(); g_p2_k = 0; nvn_p2_tp = 0;
        if (!g_p2_rep_ns) { g_p2_rep_ns = now; g_p2_rep_frames = nvn_p2_frames(); }
    }
    g_p2_last_ns = now; g_p2_subs++;
    if (nvn_p2_tp == tp) {
        nvn_p2_tp = 0;
        const uint64_t w = p2_ticks();
        for (unsigned i = 0; i < NVN_NFUNC; i++) if (nvn_p2_acc[i][1] && (int)i != g_p2_isub) { g_p2_wfn[i][0] += nvn_p2_acc[i][0]; g_p2_wfn[i][1] += nvn_p2_acc[i][1]; }
        g_p2_wsub += t0 ? w - t0 : 0; g_p2_wcpu += p2_thread_cpu_ns() - g_p2_open_cpu; g_p2_wwall += w - g_p2_open_wall; g_p2_wn++;
    }
    if (now - g_p2_rep_ns >= 10000000000ull) nvn_p2_report(now);
    if (++g_p2_k % (uint64_t)g_p2_every == 0) {
        memset(nvn_p2_acc, 0, sizeof nvn_p2_acc);
        g_p2_open_cpu = p2_thread_cpu_ns(); g_p2_open_wall = p2_ticks(); nvn_p2_tp = tp;
    }
}
// [NR-16] DM_NR_CPU (dm/host/hle_cpuprof.c): who presents and who submits, for the role names of the [cpuprof] threads line
static _Atomic int g_cpf_present_tid, g_cpf_submit_tid;
static int g_cpf_on = -1;
static inline int cpf_nvn_on(void) { if (__builtin_expect(g_cpf_on < 0, 0)) { const char *e = getenv("DM_NR_CPU"); g_cpf_on = e && atoi(e) > 0; } return g_cpf_on; }
void dm_nvn_cpuprof_tids(int *present_tid, int *submit_tid) { *present_tid = atomic_load(&g_cpf_present_tid); *submit_tid = atomic_load(&g_cpf_submit_tid); }
