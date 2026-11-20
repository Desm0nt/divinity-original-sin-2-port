// [dos2-nr05fix] NR-05 fix: the engine's start decision (nr_tex.h nrt_engine_decide, used by xbg_game.cpp nrtex::init / poll) against
// the order the phone showed on 17.11: the engine's device init (Vulkan thread) ran ~190 ms BEFORE the front end's install set
// g_dm_nr_tex (nr16_tex1 / ts1 / ts2), so the old "read the mode once at device init" gave 0 for good (keys 0 / taken 0).
// Threads: "device init" (decides once; pending if not decided), "host install" (sets the mode, then publishes the flag with release,
// then lets the guest start), "translator" (waits for the guest, then polls at each new image while pending, as get_image_slow does).
// Checks for every order (init before / after / during the install) and every DM_NR_TEX 0..3 (+ out of range): the engine ends with the
// host's mode, it is decided before the first "material texture" of the guest, the old rule reproduces the failure, no front end -> 0,
// a front end without the flag -> its value at once (old behaviour).
// Build (Windows clang 21 or WSL g++), from this folder: clang++ -std=c++17 -O2 -Wall -Wextra nr_tex_start_test.cpp -o nr_tex_start_test
extern "C" {
#include "../../../nr/nr_tex.h"
}
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

static int checks = 0, failed = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failed; if (failed <= 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

struct Run { int mode_seen, old_mode, decided_at_first_texture, polls; };

// One boot. delay_*: microseconds before the device init / the install start. host: the DM_NR_TEX the install decides.
static Run boot(int host, unsigned delay_init_us, unsigned delay_install_us, bool flag) {
    int g_mode = 0, g_set = 0;   // the front end's g_dm_nr_tex / g_dm_nr_tex_set (plain ints, as in hle_dos2_nr.c)
    std::atomic<int> guest{0}, init_done{0};
    int mode = -1, old_mode = -1; bool pending = false;
    Run r{};
    std::thread dev([&] {   // the Vulkan thread: device init
        std::this_thread::sleep_for(std::chrono::microseconds(delay_init_us));
        old_mode = __atomic_load_n(&g_mode, __ATOMIC_ACQUIRE);   // the rule before the fix: read once here
        old_mode = old_mode >= NRT_ON && old_mode <= NRT_KEEP ? old_mode : 0;
        int m = 0;
        if (!nrt_engine_decide(&g_mode, flag ? &g_set : nullptr, &m)) pending = true; else mode = m;
        init_done.store(1, std::memory_order_release);
    });
    std::thread hostt([&] {   // host_preload: dm_dos2_nr_install (nrt_start sets the mode; the cleanup publishes the flag)
        std::this_thread::sleep_for(std::chrono::microseconds(delay_install_us));
        if (host) __atomic_store_n(&g_mode, host, __ATOMIC_RELAXED);   // only nrt_start writes it (DM_NR_TEX 1..3 there; out of range values tested via the engine's clamp)
        if (flag) __atomic_store_n(&g_set, 1, __ATOMIC_RELEASE);
        guest.store(1, std::memory_order_release);   // guest code runs only after host_preload
    });
    dev.join();   // the translator starts after the device init (as in the engine)
    std::thread tr([&] {
        while (!init_done.load(std::memory_order_acquire)) std::this_thread::yield();
        while (!guest.load(std::memory_order_acquire)) {   // images before guest code (none in DOS2, but poll anyway)
            if (pending) { int m = 0; ++r.polls; if (nrt_engine_decide(&g_mode, flag ? &g_set : nullptr, &m)) { pending = false; mode = m; } }
            std::this_thread::yield();
        }
        // the guest's first material texture reaches get_image_slow: poll first
        if (pending) { int m = 0; ++r.polls; if (nrt_engine_decide(&g_mode, flag ? &g_set : nullptr, &m)) { pending = false; mode = m; } }
        r.decided_at_first_texture = !pending;
    });
    tr.join(); hostt.join();
    r.mode_seen = mode; r.old_mode = old_mode;
    return r;
}

int main() {
    // 1. no front end: decided at once, mode 0; a value out of range is off
    { int m = 7; CHECK(nrt_engine_decide(nullptr, nullptr, &m) == 1 && m == 0, "no front end: decided, mode 0 (got %d)", m); }
    { int hm = 9, hs = 1, m = 7; CHECK(nrt_engine_decide(&hm, &hs, &m) == 1 && m == 0, "DM_NR_TEX=9: off (got %d)", m); }
    { int hm = -1, hs = 1, m = 7; CHECK(nrt_engine_decide(&hm, &hs, &m) == 1 && m == 0, "DM_NR_TEX=-1: off (got %d)", m); }
    // 2. not published yet: not decided, whatever the mode reads; published: the mode
    for (int v = 0; v <= 3; v++) {
        int hm = v, hs = 0, m = 7;
        CHECK(nrt_engine_decide(&hm, &hs, &m) == 0 && m == 0, "flag 0, mode %d: not decided", v);
        hs = 1; CHECK(nrt_engine_decide(&hm, &hs, &m) == 1 && m == v, "flag 1, mode %d: decided %d", v, m);
        int hm2 = v, m2 = 7; CHECK(nrt_engine_decide(&hm2, nullptr, &m2) == 1 && m2 == v, "a front end without the flag, mode %d: as it is (%d)", v, m2);
    }
    // 3. the phone's order (init ~190 ms before the install; scaled to 19 ms): the old rule stays off, the fix starts
    for (int v = 1; v <= 3; v++) {
        const Run r = boot(v, 0, 19000, true);
        CHECK(r.old_mode == 0, "17.11 order, DM_NR_TEX=%d: the old rule reads 0 at device init (got %d)", v, r.old_mode);
        CHECK(r.mode_seen == v && r.decided_at_first_texture && r.polls >= 1, "17.11 order, DM_NR_TEX=%d: started late with %d (polls %d)", v, r.mode_seen, r.polls);
    }
    // 4. the install first: decided at device init (no poll), as before
    for (int v = 0; v <= 3; v++) {
        const Run r = boot(v, 19000, 0, true);
        CHECK(r.mode_seen == v && r.polls == 0 && r.old_mode == v, "install first, DM_NR_TEX=%d: mode %d at init, polls %d", v, r.mode_seen, r.polls);
    }
    // 5. DM_NR_TEX=0 in the late order: the first poll after the install decides 0 and stops polling (nothing starts)
    { const Run r = boot(0, 0, 19000, true); CHECK(r.mode_seen == 0 && r.decided_at_first_texture, "DM_NR_TEX=0 late: decided 0 (got %d)", r.mode_seen); }
    // 6. random interleavings: always the host's mode by the guest's first texture
    std::mt19937 rng(17112026u); int late = 0;
    for (int i = 0; i < 600; i++) {
        const int v = int(rng() % 4); const unsigned a = rng() % 400, b = rng() % 400;
        const Run r = boot(v, a, b, true);
        CHECK(r.mode_seen == v && r.decided_at_first_texture, "random %d (init +%u us, install +%u us): mode %d of %d", i, a, b, r.mode_seen, v);
        if (r.polls) ++late;
    }
    printf("random boots: %d of 600 decided late (polled)\n", late);
    printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed);
    return failed ? 1 : 0;
}
