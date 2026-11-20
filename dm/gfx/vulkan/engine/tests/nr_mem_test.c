// [DOS2] NR-29 host test: dm/gfx/nr/nr_mem.h (DM_NR_MEM) - RAM classes, the budget split over every on-mask, "only lowers", the
// trim tiers of every onTrimMemory level, the pressure poll, the trim gate and the /proc/meminfo parser; prints the caps per class
// against the old defaults (the numbers of docs/dos2/nr/NR-29.md).
// Build (Windows LLVM or any C11 compiler):
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nr_mem_test.c dm/gfx/nr/nr_mem.c -o nr_mem_test && ./nr_mem_test
#include "../../../nr/nr_mem.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static long checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { if (++fails < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define MB (1ull << 20)
#define GIB (1ull << 30)
static const unsigned ALL = (1u << NRM_N) - 1;

// The old defaults of each cache for a RAM size (hle_dos2_nr.c nrt_start, xbg_nr_buf.inc init, xbg_game.cpp astccache, warm-up)
static uint64_t old_default(int w, uint64_t ram) {
    uint64_t blob = ram / 64 >> 20; blob = blob < 32 ? 32 : blob > 128 ? 128 : blob;
    switch (w) {
    case NRM_TEXBLOB: return blob * MB;
    case NRM_TEXSTREAM: return 2 * blob * MB;
    case NRM_BUFSTATIC: return 192 * MB;
    case NRM_RING: return 20 * MB;
    case NRM_ASTCQ: return 96 * MB;
    default: return UINT64_MAX;   // the warm-up SPIR-V had no cap
    }
}

static void tiers(void) {
    CHECK(nrm_tier(0) == NRM_T_BIG);
    CHECK(nrm_tier(3 * GIB + 700 * MB) == NRM_T_4GB);      // a 4 GB phone
    CHECK(nrm_tier(19 * GIB / 4 - 1) == NRM_T_4GB);
    CHECK(nrm_tier(19 * GIB / 4) == NRM_T_6GB);
    CHECK(nrm_tier(5 * GIB + 512 * MB) == NRM_T_6GB);      // a 6 GB phone (MemTotal ~5.3-5.7 GiB)
    CHECK(nrm_tier(27 * GIB / 4 - 1) == NRM_T_6GB);
    CHECK(nrm_tier(27 * GIB / 4) == NRM_T_8GB);
    CHECK(nrm_tier(7 * GIB + 400 * MB) == NRM_T_8GB);      // an 8 GB phone
    CHECK(nrm_tier(10 * GIB - 1) == NRM_T_8GB);
    CHECK(nrm_tier(10 * GIB) == NRM_T_BIG);
    CHECK(nrm_tier(11 * GIB) == NRM_T_BIG);                // 12 GB
    CHECK(nrm_tier(15 * GIB) == NRM_T_BIG);                // the Mi 14 Ultra (16 GB)
    CHECK(nrm_tier_budget(NRM_T_4GB) == 96 * MB && nrm_tier_budget(NRM_T_6GB) == 192 * MB && nrm_tier_budget(NRM_T_8GB) == 384 * MB);
    CHECK(nrm_tier_budget(NRM_T_BIG) == 0 && nrm_tier_budget(-1) == 0 && nrm_tier_budget(99) == 0);
    for (int t = 0; t < NRM_TIERS; t++) CHECK(strcmp(nrm_tier_name(t), "?") != 0);
    CHECK(!strcmp(nrm_tier_name(7), "?") && !strcmp(nrm_name(-1), "?") && !strcmp(nrm_name(NRM_N), "?"));
}

static void split_all_masks(void) {
    static const uint64_t floors[NRM_N] = {16 * MB, 32 * MB, 32 * MB, 0, 16 * MB, 4 * MB};
    const uint64_t rams[] = {3 * GIB + 700 * MB, 5 * GIB + 512 * MB, 7 * GIB + 400 * MB, 11 * GIB, 15 * GIB, 0};
    for (size_t r = 0; r < sizeof rams / sizeof rams[0]; r++)
        for (unsigned on = 0; on <= ALL; on++) {
            nrm_plan p; nrm_plan_make(&p, rams[r], on);
            CHECK(p.ram == rams[r] && p.on == on && p.tier == nrm_tier(rams[r]) && p.budget == nrm_tier_budget(p.tier));
            CHECK(p.cap[NRM_RING] == 0);   // never capped
            uint64_t sum = 0, fsum = 0;
            for (int i = 0; i < NRM_N; i++) {
                if (!p.budget || !(on >> i & 1)) { CHECK(p.cap[i] == 0); continue; }   // no cap: off caches and the 12+ GB class
                if (i == NRM_RING) continue;
                CHECK(p.cap[i] >= floors[i]);        // a floor each
                CHECK(p.cap[i] % MB == 0);
                sum += p.cap[i]; fsum += floors[i];
            }
            if (!p.budget) continue;
            const uint64_t ring = on >> NRM_RING & 1 ? NRM_RING_RESERVE : 0;
            const uint64_t left = p.budget > ring ? p.budget - ring : 0;
            CHECK(p.floored == (fsum > left));
            if (!p.floored) CHECK(sum + ring <= p.budget);   // the caps together stay inside the class budget
            else CHECK(sum <= left + fsum);   // each cap = max(share, floor) and the shares fit in left
            // only lowers: what a cache uses is never above its old default
            for (int i = 0; i < NRM_N; i++) CHECK(nrm_apply(&p, i, old_default(i, rams[r])) <= old_default(i, rams[r]));
        }
}

static void split_numbers(void) {
    nrm_plan p;
    nrm_plan_make(&p, 7 * GIB + 400 * MB, ALL);   // 8 GB, everything on: (384 - 20) / 13 = 28 MB a weight
    CHECK(p.cap[NRM_TEXBLOB] == 56 * MB && p.cap[NRM_TEXSTREAM] == 112 * MB && p.cap[NRM_BUFSTATIC] == 112 * MB && p.cap[NRM_ASTCQ] == 56 * MB && p.cap[NRM_PIPE] == 28 * MB);
    CHECK(!p.floored);
    nrm_plan_make(&p, 5 * GIB + 512 * MB, ALL);   // 6 GB: 172 / 13 = 13.2 MB a weight
    CHECK(p.cap[NRM_TEXBLOB] == 26 * MB && p.cap[NRM_TEXSTREAM] == 52 * MB && p.cap[NRM_BUFSTATIC] == 52 * MB && p.cap[NRM_ASTCQ] == 26 * MB && p.cap[NRM_PIPE] == 13 * MB);
    nrm_plan_make(&p, 3 * GIB + 700 * MB, ALL);   // 4 GB: every cache at its floor
    CHECK(p.floored && p.cap[NRM_TEXBLOB] == 16 * MB && p.cap[NRM_TEXSTREAM] == 32 * MB && p.cap[NRM_BUFSTATIC] == 32 * MB && p.cap[NRM_ASTCQ] == 16 * MB);
    // the default build (NR-05 / 06 / 22 off): only the ASTC queue and the warm-up SPIR-V share the class budget
    nrm_plan_make(&p, 5 * GIB + 512 * MB, 1u << NRM_ASTCQ | 1u << NRM_PIPE);
    CHECK(p.cap[NRM_ASTCQ] == 128 * MB && p.cap[NRM_PIPE] == 64 * MB);
    CHECK(nrm_apply(&p, NRM_ASTCQ, 96 * MB) == 96 * MB);   // above the old default: the default stays
    CHECK(nrm_apply(&p, NRM_PIPE, UINT64_MAX) == 64 * MB); // no cap before: the share
    CHECK(nrm_apply(&p, NRM_TEXBLOB, 77 * MB) == 77 * MB); // off: untouched
    CHECK(nrm_apply(NULL, NRM_TEXBLOB, 5) == 5 && nrm_apply(&p, -1, 5) == 5 && nrm_apply(&p, NRM_N, 5) == 5);
    nrm_plan_make(&p, 15 * GIB, ALL);   // the Mi 14 Ultra: nothing changes
    for (int i = 0; i < NRM_N; i++) CHECK(nrm_apply(&p, i, old_default(i, 15 * GIB)) == old_default(i, 15 * GIB));
    // the ring alone: reserved, no cap, nothing else
    nrm_plan_make(&p, 7 * GIB, 1u << NRM_RING);
    for (int i = 0; i < NRM_N; i++) CHECK(p.cap[i] == 0);
}

static void print_table(void) {
    const uint64_t rams[] = {5 * GIB + 512 * MB, 7 * GIB + 400 * MB, 11 * GIB};
    printf("caps with NR-05 + 06 + 22 + ASTC + warm-up on (old -> new MB):\n");
    for (size_t r = 0; r < 3; r++) {
        nrm_plan p; nrm_plan_make(&p, rams[r], ALL);
        uint64_t o = 0, n = 0;
        printf("  %-13s budget %4llu:", nrm_tier_name(p.tier), (unsigned long long)(p.budget / MB));
        for (int i = 0; i < NRM_N; i++) {
            if (i == NRM_RING) continue;
            const uint64_t od = old_default(i, rams[r]), nd = nrm_apply(&p, i, od);
            if (od != UINT64_MAX) { o += od; n += nd; printf(" %s %llu->%llu", i == NRM_TEXBLOB ? "blob" : i == NRM_TEXSTREAM ? "stream" : i == NRM_BUFSTATIC ? "static" : "astcq",
                                                               (unsigned long long)(od / MB), (unsigned long long)(nd / MB)); }
            else printf(" spirv inf->%llu", nd == UINT64_MAX ? 0ull : (unsigned long long)(nd / MB));
        }
        printf("; sum %llu -> %llu MB (-%llu) + ring 20\n", (unsigned long long)(o / MB), (unsigned long long)(n / MB), (unsigned long long)((o - n) / MB));
    }
}

static void trim_levels(void) {
    CHECK(nrm_trim_tier(0) == NRM_TRIM_NONE && nrm_trim_tier(4) == NRM_TRIM_NONE && nrm_trim_tier(-1) == NRM_TRIM_NONE);
    CHECK(nrm_trim_tier(5) == NRM_TRIM_1);
    CHECK(nrm_trim_tier(10) == NRM_TRIM_2);
    CHECK(nrm_trim_tier(15) == NRM_TRIM_3);
    CHECK(nrm_trim_tier(20) == NRM_TRIM_2);
    CHECK(nrm_trim_tier(40) == NRM_TRIM_3 && nrm_trim_tier(60) == NRM_TRIM_3 && nrm_trim_tier(80) == NRM_TRIM_3 && nrm_trim_tier(1000) == NRM_TRIM_3);
    CHECK(nrm_trim_tier(7) == NRM_TRIM_1 && nrm_trim_tier(12) == NRM_TRIM_2 && nrm_trim_tier(30) == NRM_TRIM_2);
}

static void pressure(void) {
    const uint64_t ram = 8 * GIB;   // 4 % = 327.7 MB, 8 % = 655.4 MB
    CHECK(nrm_pressure(0, ram, 0) == NRM_TRIM_NONE && nrm_pressure(100 * MB, 0, 0) == NRM_TRIM_NONE);
    CHECK(nrm_pressure(2 * GIB, ram, 0) == NRM_TRIM_NONE);
    CHECK(nrm_pressure(600 * MB, ram, 0) == NRM_TRIM_1);
    CHECK(nrm_pressure(300 * MB, ram, 0) == NRM_TRIM_2);
    CHECK(nrm_pressure(ram / 25, ram, 0) == NRM_TRIM_2 && nrm_pressure(ram / 25 + 1, ram, 0) == NRM_TRIM_1);
    // an ActivityManager threshold above the fractions raises the marks (2 x / 1.25 x it)
    CHECK(nrm_pressure(900 * MB, ram, 500 * MB) == NRM_TRIM_1);
    CHECK(nrm_pressure(600 * MB, ram, 500 * MB) == NRM_TRIM_1 && nrm_pressure(480 * MB, ram, 500 * MB) == NRM_TRIM_2);   // 1.25 x 500 = 625 MB, capped at 6 % = 491.5
    CHECK(nrm_pressure(400 * MB, ram / 2, 400 * MB) == NRM_TRIM_1 && nrm_pressure(240 * MB, ram / 2, 400 * MB) == NRM_TRIM_2);   // 4 GiB: marks 245.8 / 491.5 MB
    CHECK(nrm_pressure(1100 * MB, ram, 500 * MB) == NRM_TRIM_NONE);
    // a huge threshold raises them at most to 6 % / 12 % (491.5 / 983 MB of 8 GiB)
    CHECK(nrm_pressure(1000 * MB, ram, 3 * GIB) == NRM_TRIM_NONE);
    CHECK(nrm_pressure(980 * MB, ram, 3 * GIB) == NRM_TRIM_1);
    CHECK(nrm_pressure(500 * MB, ram, 3 * GIB) == NRM_TRIM_1 && nrm_pressure(490 * MB, ram, 3 * GIB) == NRM_TRIM_2);
    // a small threshold does not lower them
    CHECK(nrm_pressure(600 * MB, ram, 10 * MB) == NRM_TRIM_1);
    for (uint64_t a = MB; a < 4 * GIB; a += 37 * MB) CHECK(nrm_pressure(a, ram, 300 * MB) != NRM_TRIM_3);   // never 3 from the poll
}

static void gate(void) {
    nrm_gate g; memset(&g, 0, sizeof g);
    const uint64_t s = 1000000000ull, gap = 30 * s;
    CHECK(!nrm_gate_pass(&g, NRM_TRIM_NONE, s, gap) && !nrm_gate_pass(&g, NRM_TRIM_N, s, gap));
    CHECK(nrm_gate_pass(&g, NRM_TRIM_1, 10 * s, gap));
    CHECK(!nrm_gate_pass(&g, NRM_TRIM_1, 20 * s, gap));   // the same tier within the gap
    CHECK(nrm_gate_pass(&g, NRM_TRIM_2, 21 * s, gap));    // a higher tier at once
    CHECK(!nrm_gate_pass(&g, NRM_TRIM_1, 45 * s, gap));   // tier 1 gap passed, but tier 2 (which includes it) ran 24 s ago
    CHECK(!nrm_gate_pass(&g, NRM_TRIM_2, 45 * s, gap));
    CHECK(nrm_gate_pass(&g, NRM_TRIM_3, 46 * s, gap));
    CHECK(!nrm_gate_pass(&g, NRM_TRIM_2, 60 * s, gap));
    CHECK(nrm_gate_pass(&g, NRM_TRIM_1, 77 * s, gap));    // everything older than 30 s
    CHECK(nrm_gate_pass(&g, NRM_TRIM_3, 78 * s, gap));    // a lower tier that just ran does not block a higher one (tier 3 last 32 s ago)
    CHECK(!nrm_gate_pass(&g, NRM_TRIM_1, 90 * s, gap));   // ... which then covers it
}

static void meminfo(void) {
    const char *t = "MemTotal:        7745660 kB\nMemFree:          212340 kB\nMemAvailable:    1873012 kB\nBuffers:            3036 kB\n"
                    "SwapTotal:       4194300 kB\nMemAvailableX:  1 kB\nOdd:42\n";
    CHECK(nrm_meminfo(t, "MemTotal") == 7745660ull << 10);
    CHECK(nrm_meminfo(t, "MemAvailable") == 1873012ull << 10);   // not MemAvailableX, and the key at a line start only
    CHECK(nrm_meminfo(t, "MemFree") == 212340ull << 10);
    CHECK(nrm_meminfo(t, "Odd") == 42);
    CHECK(nrm_meminfo(t, "Cached") == 0 && nrm_meminfo(t, "") == 0 && nrm_meminfo(NULL, "MemTotal") == 0 && nrm_meminfo(t, NULL) == 0);
    CHECK(nrm_meminfo("XMemTotal: 5 kB\n", "MemTotal") == 0);
    CHECK(nrm_tier(nrm_meminfo(t, "MemTotal")) == NRM_T_8GB);
}

int main(void) {
    tiers(); split_all_masks(); split_numbers(); trim_levels(); pressure(); gate(); meminfo();
    print_table();
    printf("%s: %ld checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
