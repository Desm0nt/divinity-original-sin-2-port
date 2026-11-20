// [DOS2] NR-29 DM_NR_MEM: the budget split and the trim tiers (see nr_mem.h).
#include "nr_mem.h"
#include <stdlib.h>
#include <string.h>

#define MB (1ull << 20)
#define GIB (1ull << 30)

static const char *const k_names[NRM_N] = {"NR-05 texture blobs", "NR-22 prepared images", "NR-06 static copies", "NR-06 ring mirror",
                                           "ASTC encode queue", "warm-up SPIR-V"};
static const unsigned k_weight[NRM_N] = {2, 4, 4, 0, 2, 1};
static const uint64_t k_floor[NRM_N] = {16 * MB, 32 * MB, 32 * MB, 0, 16 * MB, 4 * MB};

const char *nrm_name(int which) { return which >= 0 && which < NRM_N ? k_names[which] : "?"; }
const char *nrm_tier_name(int tier) {
    static const char *const n[NRM_TIERS] = {"4 GB class", "6 GB class", "8 GB class", "12+ GB class"};
    return tier >= 0 && tier < NRM_TIERS ? n[tier] : "?";
}
int nrm_tier(uint64_t ram) {
    if (!ram) return NRM_T_BIG;   // unknown: no cap (the old defaults)
    if (ram < 19 * GIB / 4) return NRM_T_4GB;
    if (ram < 27 * GIB / 4) return NRM_T_6GB;
    if (ram < 10 * GIB) return NRM_T_8GB;
    return NRM_T_BIG;
}
uint64_t nrm_tier_budget(int tier) {
    switch (tier) {
    case NRM_T_4GB: return 96 * MB;
    case NRM_T_6GB: return 192 * MB;
    case NRM_T_8GB: return 384 * MB;
    default: return 0;
    }
}
void nrm_plan_make(nrm_plan *p, uint64_t ram, unsigned on) {
    memset(p, 0, sizeof *p);
    p->ram = ram; p->on = on; p->tier = nrm_tier(ram); p->budget = nrm_tier_budget(p->tier);
    if (!p->budget) return;   // no caps
    uint64_t left = p->budget;
    if (on >> NRM_RING & 1) left = left > NRM_RING_RESERVE ? left - NRM_RING_RESERVE : 0;
    unsigned wsum = 0;
    for (int i = 0; i < NRM_N; i++) if (on >> i & 1) wsum += k_weight[i];
    uint64_t floors = 0;
    for (int i = 0; i < NRM_N; i++) {
        if (!(on >> i & 1) || !k_weight[i]) continue;
        uint64_t share = wsum ? left / wsum * k_weight[i] : 0;
        p->want += share;
        if (share < k_floor[i]) share = k_floor[i];
        p->cap[i] = share / MB * MB;
        floors += k_floor[i];
    }
    p->floored = floors > left;
}
uint64_t nrm_apply(const nrm_plan *p, int which, uint64_t dflt) {
    if (!p || which < 0 || which >= NRM_N || !p->cap[which]) return dflt;
    return dflt < p->cap[which] ? dflt : p->cap[which];
}

int nrm_trim_tier(int level) {
    if (level >= 40) return NRM_TRIM_3;        // BACKGROUND, MODERATE, COMPLETE: the game is not in front
    if (level >= 20) return NRM_TRIM_2;        // UI_HIDDEN
    if (level >= 15) return NRM_TRIM_3;        // RUNNING_CRITICAL
    if (level >= 10) return NRM_TRIM_2;        // RUNNING_LOW
    if (level >= 5) return NRM_TRIM_1;         // RUNNING_MODERATE
    return NRM_TRIM_NONE;
}
int nrm_pressure(uint64_t avail, uint64_t ram, uint64_t threshold) {
    if (!avail || !ram) return NRM_TRIM_NONE;
    uint64_t t2 = ram / 25, t1 = ram / 25 * 2;   // 4 % / 8 %
    if (threshold) {   // raised by the ActivityManager threshold, but at most to 6 % / 12 % (some ROMs report a high one: no trim every 30 s)
        uint64_t a = threshold + threshold / 4, b = threshold * 2;
        if (a > ram / 50 * 3) a = ram / 50 * 3;
        if (b > ram / 25 * 3) b = ram / 25 * 3;
        if (a > t2) t2 = a;
        if (b > t1) t1 = b;
    }
    if (avail <= t2) return NRM_TRIM_2;
    if (avail <= t1) return NRM_TRIM_1;
    return NRM_TRIM_NONE;
}
int nrm_gate_pass(nrm_gate *g, int tier, uint64_t now, uint64_t gap) {
    if (tier <= NRM_TRIM_NONE || tier >= NRM_TRIM_N) return 0;
    for (int k = tier; k < NRM_TRIM_N; k++)   // a trim of this tier or a higher one (which includes it) within gap: not again
        if (g->last[k] && now >= g->last[k] && now - g->last[k] < gap) return 0;
    g->last[tier] = now ? now : 1;
    return 1;
}

uint64_t nrm_usage_bytes(const uint64_t u[NRM_U_N]) {
    uint64_t s = 0;
    for (int i = 0; i < NRM_U_N; i++) if (i != NRM_U_PIPES) s += u[i];
    return s;
}

uint64_t nrm_meminfo(const char *text, const char *key) {
    if (!text || !key || !*key) return 0;
    const size_t n = strlen(key);
    for (const char *s = text; s && *s;) {
        if (!strncmp(s, key, n) && s[n] == ':') {
            char *end = NULL;
            const unsigned long long v = strtoull(s + n + 1, &end, 10);
            while (end && *end == ' ') end++;
            const int kb = end && (end[0] == 'k' || end[0] == 'K') && (end[1] == 'B' || end[1] == 'b');
            return kb ? (uint64_t)v << 10 : (uint64_t)v;
        }
        s = strchr(s, '\n');
        if (s) s++;
    }
    return 0;
}
