// [DOS2] VK-NATIVE stage 4: conditional rendering and visibility queries (contract and rules: vkn_query.h). No game addresses.
// Stage 5.1 (O1 / O2 of docs/dos2/nr/VK-OCCLUSION-ALT.md): the oracle mode 4 with its object census, and DM_VK_NATIVE_GBUF_LE.
#ifdef DM_GAME_DOS2
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkn_query.h"
#include "vkn_frame.h"
#include "vkn_fx.h"
#include "vkn_state.h"
#include "../nr/nr_occ.h"
#include "vkn_occ.h"   // [stage 5 technique A] modes 5 / 6
#include "vkn_occb.h"  // [stage 5 technique B] modes 7 / 8
#include "vkn_occz.h"  // [stage 5 technique Z] modes 10 .. 13

#define Q_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark
// The front end (android/vulkan_present.cpp) enabled VK_EXT_conditional_rendering on the device (asked for with DM_VK_NATIVE_COND 1 / 3).
extern int dm_vk_cond_render __attribute__((weak));
// The backend's shader library (vkn_shlib.h; C++, not in the host tests): fragment facts of its shader objects.
extern int vkn_shlib_on(void) __attribute__((weak));
extern int vkn_shlib_fs_facts(const void *shader) __attribute__((weak));

// Bracket states (see vkn_query.h). Q_NOEXT: drew something on a device without the extension - no Vulkan query is made then (only
// the count mode needs the result without predicates), the consumers are drawn. Q_NOQ: tracked without a query by design (mode 0 with
// GBUF_LE, mode 4 between oracle recordings).
enum { Q_NONE = 0, Q_PENDING, Q_ACTIVE, Q_CLOSED, Q_COPIED, Q_EMPTY, Q_VISIBLE, Q_NOEXT, Q_NOQ };
enum { MAP = 8192, SPARE = VKN_Q_RECS, REDS = 16, RETIRE = 3 };   // RETIRE = vkn.h's retirement delay (frames)
enum { R_NESTED, R_UNMATCHED, R_FULL, R_SETUP, R_RECS, R_LE_TABLE, R_N };
enum { KINDS = 16, KIND_OTHER = KINDS - 1, BUCKETS = 6, NO_BIN = 0xff };

typedef struct q_rec {
    uint64_t key; int32_t q; uint32_t seen, done; uint8_t st;
    uint8_t closed;                  // its end report came while it was the open pair (not nested, not cut by the recording end)
    uint8_t skip;                    // mode 4: its key's last verdict is "no sample": its draws are skipped
    uint8_t cap;                     // GBUF_LE: g_cap[] holds its first draw as the pre-pass finally drew it
    uint64_t okey;                   // object key (modes 2 / 4; 0 = none)
} q_rec;
typedef struct q_slot {
    VkQueryPool pool;
    uint64_t frame;                  // vkn_frame() of its last recording
    uint32_t nq, copied, dirty;      // queries begun / copied in the current use; queries to reset before the next use
    uint32_t ncond; int used, read;  // read: its verdicts were folded in (mode 4 reads an oracle as soon as it retired)
    uint16_t qrec[VKN_Q_CAP];        // query -> record of the current use
    uint16_t cond[VKN_Q_CONDS];      // queries whose predicate judged a draw (read back when the slot is reused)
    uint8_t cbin[VKN_Q_CONDS];       // census bin of that draw (pass kind << 3 | size bucket), NO_BIN = none
    uint64_t qkey[VKN_Q_CAP];        // object key of the query's pair when its result is a complete verdict, 0 = none
} q_slot;

static int g_mode, g_setup, g_broken, g_gpu, g_buf_cond;
static PFN_vkCmdBeginConditionalRenderingEXT g_pbegin;
static PFN_vkCmdEndConditionalRenderingEXT g_pend;
static VkBuffer g_pbuf; static VkDeviceMemory g_pmem; static volatile uint32_t *g_pred;   // VKN_Q_SLOTS x VKN_Q_CAP predicate words
static q_slot g_slot[VKN_Q_SLOTS]; static int g_cur = -1, g_next;
static q_rec g_rec[VKN_Q_RECS + 1]; static uint32_t g_nrec;   // [SPARE]: "visible" for keys beyond the record table
static struct { uint64_t key; uint32_t gen; uint16_t rec; } g_map[MAP]; static uint32_t g_gen = 1;
static int g_open = -1;                         // record of the open bracket (between its two reports), -1 = none
static uint64_t g_cond_key; static int g_cond_set;
static int g_pred_on, g_cond_active; static int32_t g_list_q = -1; static uint32_t g_pred_off;   // the draw being recorded
static vkn_query_stats g_st; static uint32_t g_tick;
static uint8_t g_red[R_N];
#define QRED(i, ...) do { if (g_red[i] < REDS) { g_red[i]++; vkn_logf(Q_RED "query: " __VA_ARGS__); } } while (0)
// CPU time: one call in 16 is timed and counted 16 times (two clock reads per call would cost more than the calls themselves).
#define T_BEGIN const int t_on_ = (++g_tick & 15) == 0; const uint64_t t0_ = t_on_ ? vkn_now_ns() : 0
#define T_END do { if (t_on_) g_st.cpu_ns += 16 * (vkn_now_ns() - t0_); } while (0)

// ---- stage 5.1: what the recording being made does (set at its begin) and the draw being made ----
static int g_track;                             // brackets are recorded at all (mode != 0, or GBUF_LE asked for)
static int g_rec_noq;                           // this recording's brackets get no query (Q_NOQ)
static int g_count_now;                         // this recording counts like mode 2 (mode 2; mode 4's oracle recording)
static int g_o1_skip;                           // mode 4 between oracles (presented): draws of hidden keys are skipped
static uint32_t g_oracle_n; static int g_opend = -1;   // mode 4: presented recordings so far; slot of the oracle awaiting its readback
static int32_t g_dcond = -1;                    // the draw: -1 unconditional, -2 its condition names no pair here, else the record
static uint8_t g_list_bin = NO_BIN;             // the draw's census bin (set by the O1 function)
static nro_hist g_hist;                         // object key -> verdicts of the read frames (nr_occ.h)
static struct { uint64_t key; uint32_t gen, n; } g_mm[MAP];   // draw hash -> pairs with it in this recording (occurrence numbers)
static vkn_query_occ_stats g_os, g_os_prev; static uint64_t g_os_frame;
static struct { vkn_tex *c, *d; char name[44]; } g_kind[KINDS]; static uint32_t g_nkind; static int g_lastk = -1;

int vkn_query_mode(void) { return g_mode; }
int vkn_query_idle(void) { return !g_mode && !g_track; }   // [rt2 R7] no bracket, predicate or count can touch a draw (vkn_sort.h)
int vkn_query_gpu(void) { return g_gpu; }
void vkn_query_stats_get(vkn_query_stats *s) { if (s) *s = g_st; }
void vkn_query_occ_stats_get(vkn_query_occ_stats *s) { if (s) *s = g_os; }

// ---- key -> record of the current recording (open addressing; a generation per recording empties it) ----
static uint32_t slot_of(uint64_t key) { return (uint32_t)(((key >> 4) * 0x9e3779b97f4a7c15ull) >> 51) & (MAP - 1); }
static int map_get(uint64_t key) {
    for (uint32_t i = slot_of(key), n = 0; n < MAP; i = (i + 1) & (MAP - 1), n++) {
        if (g_map[i].gen != g_gen) return -1;
        if (g_map[i].key == key) return g_map[i].rec;
    }
    return -1;
}
static void map_put(uint64_t key, uint32_t rec) {   // records <= VKN_Q_RECS + 1 < MAP / 2: always room
    for (uint32_t i = slot_of(key), n = 0; n < MAP; i = (i + 1) & (MAP - 1), n++)
        if (g_map[i].gen != g_gen || g_map[i].key == key) { g_map[i].gen = g_gen; g_map[i].key = key; g_map[i].rec = (uint16_t)rec; return; }
}
// Occurrence number of a draw hash among this recording's pairs (0 for the first), counted up. One key per pair: < MAP / 2 entries.
static uint32_t mm_next(uint64_t h) {
    for (uint32_t i = slot_of(h ^ (h >> 29)), n = 0; n < MAP; i = (i + 1) & (MAP - 1), n++) {
        if (g_mm[i].gen != g_gen) { g_mm[i].gen = g_gen; g_mm[i].key = h; g_mm[i].n = 1; return 0; }
        if (g_mm[i].key == h) return g_mm[i].n++;
    }
    return 0xffffffffu;
}

// The extension's entry points (vkGetDeviceProcAddr is in the trampoline table; the two EXT names are not): only when the front end
// enabled the extension on the device, and only for a predicate buffer created with the conditional-rendering usage.
static void probe(const vkn_dev *d) {
    g_pbegin = NULL; g_pend = NULL; g_gpu = 0;
    if (!(&dm_vk_cond_render && dm_vk_cond_render) || (g_setup > 1 && !g_buf_cond)) return;
    g_pbegin = (PFN_vkCmdBeginConditionalRenderingEXT)(void *)vkGetDeviceProcAddr(d->dev, "vkCmdBeginConditionalRenderingEXT");
    g_pend = (PFN_vkCmdEndConditionalRenderingEXT)(void *)vkGetDeviceProcAddr(d->dev, "vkCmdEndConditionalRenderingEXT");
    g_gpu = g_pbegin && g_pend;
}
static const char *mode_text(int m) {
    return m == VKN_COND_ON ? "on" : m == VKN_COND_COUNT ? "count only, everything drawn" : m == VKN_COND_POISON ? "poison: predicates inverted"
         : m == VKN_COND_A ? "technique A: carried verdicts with proof, CPU skips, learning queries only (vkn_occ.h)"   // [stage 5 technique A]
         : m == VKN_COND_A_VERIFY ? "technique A VERIFY: a query on every pair, nothing skipped, A's decisions checked against them"
         : m == VKN_COND_B ? "technique B: group queries and group conditions (vkn_occb.h)"   // [stage 5 technique B]
         : m == VKN_COND_B_VERIFY ? "technique B VERIFY: per-member queries, captured G-buffer draws queried and drawn, B's group decisions checked"
         : m == VKN_COND_Z ? "technique Z count only: depth snapshot + engine boxes decided on the engine thread, everything drawn (vkn_occz.h)"   // [stage 5 technique Z]
         : m == VKN_COND_Z_VERIFY ? "technique Z VERIFY: queries on every pair + Z's self-check, nothing skipped"
         : m == VKN_COND_Z_ON ? "technique Z: the worker skips the pairs the engine thread decided hidden"
         : m == VKN_COND_Z_POISON ? "technique Z POISON: trusted VISIBLE objects skipped (they vanish)"
         : m == VKN_COND_AB ? "techniques A + B: A's CPU skips first, B's group queries and conditions for the rest"
         : m == VKN_COND_ORACLE ? "ORACLE: INEXACT, for measurements while standing only, never for players (a query on every pair once in 64 frames, "
                                  "the draws of objects last seen hidden skipped on the CPU in between)" : "off";
}
// [stage 5 technique B] mode 7 without the extension or on a tile family: mode 0 (vkn_occb_gate prints the line); once the device is known
static void b_gate(void) {
    if ((g_mode == VKN_COND_B || g_mode == VKN_COND_AB) && vkn_occb_gate(VKN_COND_B, g_gpu) != VKN_COND_B) {
        g_mode = g_mode == VKN_COND_AB ? VKN_COND_A : VKN_COND_OFF; vkn_occb_init(0, -1); g_track = g_mode || vkn_query_le_asked();   // (9: A alone)
        if (g_mode) vkn_logf("query: DM_VK_NATIVE_COND=9 -> 5 (technique A alone)");
    }
}
// ---- setup: pools, predicate words, the extension's entry points (first presented recording with a device) ----
static int setup(void) {
    g_setup = 1;
    const vkn_dev *d = vkn_device();
    if (!d) { g_setup = 0; return -1; }   // (tried again at the next recording)
    probe(d);
    g_buf_cond = g_gpu;
    for (int i = 0; i < VKN_Q_SLOTS; i++) {
        const VkQueryPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = VKN_Q_CAP};
        if (vkCreateQueryPool(d->dev, &ci, NULL, &g_slot[i].pool) != VK_SUCCESS) goto fail;
        g_slot[i].dirty = VKN_Q_CAP;   // a new pool's queries are undefined until reset
    }
    {
        const VkDeviceSize bytes = (VkDeviceSize)VKN_Q_SLOTS * VKN_Q_CAP * 4u;
        const VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | (g_gpu ? VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT : 0)};
        if (vkCreateBuffer(d->dev, &bi, NULL, &g_pbuf) != VK_SUCCESS) goto fail;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d->dev, g_pbuf, &mr);
        // host-visible and coherent: the predicates of a retired slot are read for the census without a map flush
        const int mt = vkn_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) goto fail;
        const VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
        void *p = NULL;
        if (vkAllocateMemory(d->dev, &ai, NULL, &g_pmem) != VK_SUCCESS || vkBindBufferMemory(d->dev, g_pbuf, g_pmem, 0) != VK_SUCCESS ||
            vkMapMemory(d->dev, g_pmem, 0, bytes, 0, &p) != VK_SUCCESS || !p) goto fail;
        g_pred = (volatile uint32_t *)p;
        for (uint32_t i = 0; i < VKN_Q_SLOTS * VKN_Q_CAP; i++) g_pred[i] = 1;   // (never read before a copy wrote it; 1 = "visible")
    }
    g_setup = 2;
    b_gate();   // [stage 5 technique B]
    vkn_logf("query: DM_VK_NATIVE_COND=%d (%s): %d slots x %d occlusion queries, predicates %s", g_mode, mode_text(g_mode), VKN_Q_SLOTS, VKN_Q_CAP,
             g_gpu ? "on the GPU (VK_EXT_conditional_rendering)" : "not available (VK_EXT_conditional_rendering not enabled): only empty brackets are skipped");
    return 0;
fail:
    g_broken = 1; g_st.setup_fail++;
    QRED(R_SETUP, "query pools / predicate buffer could not be created: conditional rendering stays off (everything drawn)");
    return -1;
}

// ---- census bins (modes 2 / 4): pass kind by target set, size bucket by index count x instances ----
static int kind_of(vkn_tex *c, vkn_tex *d) {
    if (g_lastk >= 0 && g_kind[g_lastk].c == c && g_kind[g_lastk].d == d) return g_lastk;
    for (uint32_t i = 0; i < g_nkind; i++) if (g_kind[i].c == c && g_kind[i].d == d) return g_lastk = (int)i;
    if (g_nkind >= KIND_OTHER) return KIND_OTHER;
    const uint32_t k = g_nkind++;
    g_kind[k].c = c; g_kind[k].d = d;   // (the names are copied now: a texture may die before the census prints them)
    snprintf(g_kind[k].name, sizeof g_kind[k].name, "%.20s+%.20s", c && c->sname[0] ? c->sname : c ? "?" : "-", d && d->sname[0] ? d->sname : d ? "?" : "-");
    return g_lastk = (int)k;
}
static int bucket_of(uint64_t n) { return n <= 64 ? 0 : n <= 256 ? 1 : n <= 1024 ? 2 : n <= 4096 ? 3 : n <= 16384 ? 4 : 5; }
static void bin_add(uint64_t *k, uint64_t *b, uint8_t bin) { if (bin != NO_BIN) { k[bin >> 3 & (KINDS - 1)]++; b[(bin & 7) < BUCKETS ? bin & 7 : BUCKETS - 1]++; } }

// ---- slots ----
static void readback(q_slot *s, int si) {   // the slot's last recording finished (RETIRE rule): the GPU's verdicts on its draws
    if (s->read) return;
    s->read = 1;
    for (uint32_t i = 0; i < s->ncond; i++) {
        const uint32_t v = g_pred[(uint32_t)si * VKN_Q_CAP + s->cond[i]];
        g_st.results_read++;
        if (v) g_st.gpu_drawn++; else { g_st.gpu_culled++; bin_add(g_os.cull_k, g_os.cull_b, s->cbin[i]); }
    }
    s->ncond = 0;
    if (vkn_cond_is_a(g_mode)) for (uint32_t q = 0; q < s->copied; q++) vkn_occ_q_result(si, q, g_pred[(uint32_t)si * VKN_Q_CAP + q]);   // [stage 5 technique A]
    if (vkn_cond_is_b(g_mode)) for (uint32_t q = 0; q < s->copied; q++) vkn_occb_q_result(si, q, g_pred[(uint32_t)si * VKN_Q_CAP + q]);   // [stage 5 technique B]
    if (g_mode == VKN_COND_Z_VERIFY) for (uint32_t q = 0; q < s->copied; q++) vkn_occz_q_result(si, q, g_pred[(uint32_t)si * VKN_Q_CAP + q]);   // [stage 5 technique Z]
    // object verdicts (modes 2 / 4): one serial of the history per read recording
    uint32_t keyed = 0;
    for (uint32_t q = 0; q < s->nq; q++) keyed += s->qkey[q] != 0;
    if (!keyed) return;
    // [stage 5.1 A5 fix] verdicts older than the newest folded ones are not folded: a slot whose recording was keyed in another mode and
    // is read only when the oracle takes it again (mode 2 -> 4) would otherwise become "the last read" and void the newer verdicts
    // (host test: the frames after the third oracle lost every CPU skip)
    static uint64_t fold_frame; static int fold_any;
    if (fold_any && s->frame < fold_frame) return;
    fold_frame = s->frame; fold_any = 1;
    nro_hist_begin(&g_hist); g_os.read_frames++;
    for (uint32_t q = 0; q < s->nq; q++) {
        const uint64_t k = s->qkey[q]; if (!k) continue;
        const int hidden = g_pred[(uint32_t)si * VKN_Q_CAP + q] == 0;
        const nro_hent *e = nro_hist_get(&g_hist, k);
        g_os.read_pairs++; g_os.read_hidden += (uint64_t)hidden;
        if (e && e->last + 1 == g_hist.serial) { g_os.read_stable++; if (e->count == 1 && (e->streak > 0) != hidden) g_os.read_changed++; }
        nro_hist_put(&g_hist, k, 0, hidden);
    }
}
static void on_begin(void) {
    g_cur = -1; g_open = -1; g_cond_set = 0; g_pred_on = 0; g_cond_active = 0; g_list_q = -1; g_nrec = 0;
    g_rec_noq = !g_mode; g_count_now = 0; g_o1_skip = 0; g_dcond = -1;
    if (++g_gen == 0) { memset(g_map, 0, sizeof g_map); memset(g_mm, 0, sizeof g_mm); g_gen = 1; }
    if (!g_mode || g_broken || !vkn_rec_presented()) return;
    if (vkn_cond_is_z(g_mode) && g_mode != VKN_COND_Z_VERIFY) { g_rec_noq = 1; return; }   // [stage 5 technique Z] pairs tracked, no query
    if (!g_setup && setup()) return;
    if (g_broken) return;
    if (!g_mode) { g_rec_noq = 1; return; }   // [stage 5 technique B] mode 7 fell back to 0 at setup
    T_BEGIN;
    if (g_mode == VKN_COND_ORACLE) {
        if (g_opend >= 0 && g_slot[g_opend].frame + RETIRE <= vkn_frame()) { readback(&g_slot[g_opend], g_opend); g_opend = -1; }
        if (g_oracle_n++ % VKN_ORACLE_PERIOD) { g_rec_noq = 1; g_o1_skip = 1; T_END; return; }   // between oracles: no query, skips
    }
    if (g_mode >= VKN_COND_A)   // [stage 5 technique A / B] every retired slot's verdicts now (3 recordings old), not at its reuse
        for (int i = 0; i < VKN_Q_SLOTS; i++) if (g_slot[i].used && !g_slot[i].read && g_slot[i].frame + RETIRE <= vkn_frame()) readback(&g_slot[i], i);
    q_slot *s = &g_slot[g_next];
    if (s->used && s->frame + RETIRE > vkn_frame()) {   // (its recording may still run)
        g_st.slots_busy++;
        if (g_mode == VKN_COND_ORACLE) { g_os.oracle_lost++; g_oracle_n--; g_rec_noq = 1; g_o1_skip = 1; }   // the oracle comes next time
        T_END; return;
    }
    if (s->used) readback(s, g_next);
    VkCommandBuffer cb = vkn_cb();
    if (s->dirty) vkCmdResetQueryPool(cb, s->pool, 0, s->dirty);
    s->nq = s->copied = s->dirty = 0; s->ncond = 0; s->used = 1; s->read = 0; s->frame = vkn_frame();
    if (g_mode == VKN_COND_ORACLE) { g_opend = g_next; g_os.oracle_frames++; }
    g_count_now = g_mode == VKN_COND_COUNT || g_mode == VKN_COND_ORACLE || g_mode >= VKN_COND_A;   // [stage 5 technique A / B] 5 .. 8 count; [Z] 11
    if (vkn_cond_is_a(g_mode)) vkn_occ_q_slot_taken(g_next);
    if (vkn_cond_is_b(g_mode)) vkn_occb_q_slot_taken(g_next);   // [stage 5 technique B]
    g_cur = g_next; g_next = (g_next + 1) % VKN_Q_SLOTS; g_st.slots_taken++;
    T_END;
}
static void end_active(VkCommandBuffer cb, q_rec *r) { vkCmdEndQuery(cb, g_slot[g_cur].pool, (uint32_t)r->q); }
// Recording close. A pair still open is unusable; its query (if begun) ends here when this listener runs before the pass module's
// (still inside the pass: legal), else the pass end already ended it. g_cur stays: a pass ended after this listener still copies.
static void on_close(void) {
    if (g_open >= 0) {
        q_rec *r = &g_rec[g_open];
        g_st.open_at_close++;
        if (r->st == Q_ACTIVE) end_active(vkn_cb(), r);
        r->st = Q_VISIBLE;
    }
    g_open = -1; g_cond_set = 0;
}
static void occ_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user);
// [stage 5 technique A] after this module's own listeners: the slot this recording took (g_cur), the recording's end
static void occ_begin(void) {
    if (vkn_cond_is_z(g_mode)) vkn_occz_q_rec_begin();   // [stage 5 technique Z]
    if (vkn_cond_is_a(g_mode)) vkn_occ_q_rec_begin(!g_broken && vkn_rec_presented(), g_cur);
    if (vkn_cond_is_b(g_mode)) vkn_occb_q_rec_begin(!g_broken && vkn_rec_presented(), g_cur);   // [stage 5 technique B]
}
static void occ_close(void) {
    if (vkn_cond_is_a(g_mode)) vkn_occ_q_rec_close();
    if (vkn_cond_is_b(g_mode)) vkn_occb_q_rec_close();   // [stage 5 technique B]
}
static void listeners(void) {
    static int done;
    if (!done) { done = 1; vkn_rec_on_begin(on_begin); vkn_rec_on_close(on_close); vkn_rec_on_begin(occ_begin); vkn_rec_on_close(occ_close); }   // [stage 5 technique A] occ_*
}
void vkn_query_init(int mode) {
    g_mode = mode < 0 || mode >= VKN_COND_MODES_Z ? VKN_COND_OFF : mode;   // [stage 5 technique A / B] 5 .. 9 too; [Z] 10 .. 13
    if (g_setup > 1 && !g_broken && vkn_device()) probe(vkn_device());   // (a second call: the host test switches modes / the extension)
    if (g_mode) listeners();
    if (g_mode == VKN_COND_COUNT || g_mode == VKN_COND_ORACLE) {   // O1: object keys, census bins, mode 4's skips
        static int reg;
        if (!reg) reg = !vkn_fx_register("occ-oracle", occ_fx, NULL, 50);
    }
    if (g_mode == VKN_COND_ORACLE)
        vkn_logf("query: DM_VK_NATIVE_COND=4 = %s", mode_text(g_mode));
    {   // [stage 5 technique A] the model (vkn_occ.c) and its budget
        const char *b = getenv("DM_VK_NATIVE_COND_B");
        vkn_occ_init(vkn_cond_is_a(g_mode) ? (g_mode == VKN_COND_AB ? VKN_COND_A : g_mode) : 0, b && *b ? atoi(b) : -1);
        vkn_occb_init(vkn_cond_is_b(g_mode) ? (g_mode == VKN_COND_AB ? VKN_COND_B : g_mode) : 0, b && *b ? atoi(b) : -1);   // [stage 5 technique B] (9: A as 5, B as 7)
        if (g_mode >= VKN_COND_A) vkn_logf("query: DM_VK_NATIVE_COND=%d = %s", g_mode, mode_text(g_mode));
    }
    vkn_occz_init(vkn_cond_is_z(g_mode) ? g_mode : 0);   // [stage 5 technique Z]
    const char *le = getenv("DM_VK_NATIVE_GBUF_LE");
    vkn_query_le_init(le && *le ? atoi(le) : 0);
    g_track = g_mode || vkn_query_le_asked();
    if (g_setup > 1 && !g_broken && vkn_device()) b_gate();   // [stage 5 technique B] (set up already: the gate now)
}

// ---- the engine's reports ----
void vkn_query_begin(uint64_t key) {
    g_st.begins++;
    if (!g_track || !key) return;
    T_BEGIN;
    if (g_open >= 0) {   // nested / a begin without its end: the open bracket is unusable (one occlusion query at a time)
        q_rec *o = &g_rec[g_open];
        if (o->st == Q_ACTIVE) end_active(vkn_cb(), o);
        if (o->st != Q_VISIBLE) o->st = Q_VISIBLE;
        if (vkn_cond_is_a(g_mode)) vkn_occ_q_pair_broken((uint32_t)g_open);   // [stage 5 technique A]
        g_st.nested++;
        QRED(R_NESTED, "report pair %#llx begins inside the open pair %#llx: the open one counts as visible", (unsigned long long)key, (unsigned long long)o->key);
        g_open = -1;
    }
    if (g_nrec >= VKN_Q_RECS) {
        g_st.pool_full++;
        QRED(R_RECS, "more than %d report pairs in one recording: pair %#llx counts as visible", VKN_Q_RECS, (unsigned long long)key);
        g_rec[SPARE].st = Q_VISIBLE; g_rec[SPARE].key = 0; g_rec[SPARE].okey = 0; g_rec[SPARE].skip = 0; g_rec[SPARE].closed = 0; g_rec[SPARE].cap = 0;
        map_put(key, SPARE);
        T_END; return;
    }
    const uint32_t i = g_nrec++;
    q_rec *r = &g_rec[i];
    r->key = key; r->q = -1; r->seen = r->done = 0; r->closed = r->skip = r->cap = 0; r->okey = 0;
    if (g_rec_noq) r->st = Q_NOQ;
    else if (g_cur < 0) { r->st = Q_VISIBLE; g_st.no_slot++; } else r->st = Q_PENDING;
    if (vkn_cond_is_a(g_mode)) { const int want = vkn_occ_q_pair_begin(i, key); if (r->st == Q_PENDING && !want) r->st = Q_NOQ; }   // [stage 5 technique A] learning only
    if (vkn_cond_is_b(g_mode)) { if (r->st == Q_PENDING && !vkn_cond_is_a(g_mode)) r->st = Q_NOQ; vkn_occb_q_pair_begin(i); }   // [stage 5 technique B] queries chosen at the first draw (9: A's learning stays)
    map_put(key, i);
    g_open = (int)i;
    T_END;
}
void vkn_query_end(uint64_t key) {
    g_st.ends++;
    if (!g_track || !key) return;
    T_BEGIN;
    if (g_open < 0 && map_get(key) == SPARE) { T_END; return; }   // (a pair beyond the record table: counted at its begin)
    if (g_open < 0 || g_rec[g_open].key != key) {
        g_st.unmatched++;
        QRED(R_UNMATCHED, "report pair %#llx ends without its begin (open pair %#llx)", (unsigned long long)key, g_open >= 0 ? (unsigned long long)g_rec[g_open].key : 0ull);
        T_END; return;
    }
    q_rec *r = &g_rec[g_open]; g_open = -1; r->closed = 1;
    const int complete = r->seen == r->done;   // every draw that entered the draw path inside the pair was recorded (or skipped by its own condition)
    if (r->st == Q_ACTIVE) {
        end_active(vkn_cb(), r);
        if (complete) r->st = Q_CLOSED; else { r->st = Q_VISIBLE; g_st.lost++; }
    } else if (r->st == Q_PENDING) {
        if (complete) { r->st = Q_EMPTY; g_st.empty++; } else { r->st = Q_VISIBLE; g_st.lost++; }
    } else if (r->st == Q_NOQ && g_mode && !r->seen) { r->st = Q_EMPTY; g_st.empty++; }   // mode 4 between oracles: exact, as mode 1
    T_END;
}
void vkn_query_cond(uint64_t key) { g_cond_key = key; g_cond_set = key != 0; }
void vkn_query_dispatch(void) {
    if (vkn_cond_is_b(g_mode)) vkn_occb_q_dispatch();   // [stage 5 technique B] (a safe point)
    if (g_cond_set) { g_cond_set = 0; g_st.cond_dispatch++; }
}

// ---- the draw path ----
int vkn_query_draw_enter(void) {
    if (vkn_cond_is_b(g_mode)) { if (vkn_occb_replaying()) return 0; vkn_occb_q_enter(); }   // [stage 5 technique B] B's own replays: not the engine's
    g_pred_on = 0; g_list_q = -1; g_dcond = -1; g_list_bin = NO_BIN;
    if (!g_track) { if (g_cond_set) { g_cond_set = 0; g_st.conds++; } return 0; }
    if (g_open >= 0) g_rec[g_open].seen++;
    if (!g_cond_set) return 0;
    T_BEGIN;
    const uint64_t key = g_cond_key; g_cond_set = 0;
    g_st.conds++;
    int skip = 0;
    const int ri = map_get(key);
    g_dcond = ri >= 0 && ri != SPARE ? ri : -2;   // (the effect functions judge the draw by its pair)
    if (!g_mode) {}   // tracked for GBUF_LE only: drawn as always
    else if (g_rec_noq) g_os.cond_noq++;   // mode 4 between oracles: drawn unless the O1 function skips it
    else if (g_cur < 0) g_st.drawn_no_slot++;
    else if (ri < 0) g_st.drawn_no_query++;
    else {
        const q_rec *r = &g_rec[ri];
        switch (r->st) {
        case Q_EMPTY:   // no draw between the reports: zero samples, the Switch skips it too (exact on the CPU)
            if (g_mode == VKN_COND_ON || g_mode == VKN_COND_A || g_mode == VKN_COND_B || g_mode == VKN_COND_AB) { skip = 1; g_st.cond_cpu_skip++; }   // [stage 5 technique A / B] 5, 7, 9 as 1
            else if (g_count_now) { g_st.cond_count++; g_st.count_empty++; }
            else g_st.drawn_poison_empty++;
            break;
        case Q_COPIED:
            if (g_count_now) { g_list_q = r->q; g_st.cond_count++; }
            else if (g_gpu) { g_pred_on = 1; g_list_q = r->q; g_pred_off = ((uint32_t)g_cur * VKN_Q_CAP + (uint32_t)r->q) * 4u; g_st.cond_gpu++; }
            else g_st.drawn_no_ext++;
            break;
        case Q_CLOSED: g_st.drawn_same_pass++; break;   // its pass is still open: no predicate yet
        case Q_PENDING: case Q_ACTIVE: g_st.drawn_open++; break;
        case Q_NOEXT: g_st.drawn_no_ext++; break;
        case Q_NOQ: g_os.cond_noq++; break;   // [stage 5 technique A] mode 5: no query (drawn unless vkn_occ.c proves it)
        default: g_st.drawn_unusable++; break;
        }
    }
    if (skip && g_open >= 0) g_rec[g_open].done++;
    T_END;
    return skip;
}
void vkn_query_draw_pre(VkCommandBuffer cb) {
    if (!g_track) return;
    if (vkn_cond_is_b(g_mode) && vkn_occb_replaying()) return;   // [stage 5 technique B]
    if (vkn_cond_is_a(g_mode)) vkn_occ_q_draw_pre();   // [stage 5 technique A] the draw is recorded
    if (vkn_cond_is_b(g_mode)) vkn_occb_q_draw_pre_test();   // [stage 5 technique B] (the host test's sample hook)
    if (g_open >= 0) {
        q_rec *r = &g_rec[g_open];
        if (r->st == Q_PENDING) {   // the first recorded draw of the pair: the pass is open now, the query begins in it
            q_slot *s = &g_slot[g_cur];
            if (!g_gpu && !g_count_now) r->st = Q_NOEXT;   // (no predicate could use it: no query, not empty)
            else if (s->nq >= VKN_Q_CAP) { r->st = Q_VISIBLE; g_st.pool_full++; QRED(R_FULL, "more than %d queries in one recording: pair %#llx counts as visible", VKN_Q_CAP, (unsigned long long)r->key); }
            else { r->q = (int32_t)s->nq; s->qrec[s->nq] = (uint16_t)g_open; s->qkey[s->nq] = 0; s->nq++; s->dirty = s->nq; vkCmdBeginQuery(cb, s->pool, (uint32_t)r->q, 0); r->st = Q_ACTIVE; g_st.queries++; }
        }
        r->done++;
    }
    if (g_list_q >= 0 && g_cur >= 0) {
        q_slot *s = &g_slot[g_cur];
        if (s->ncond < VKN_Q_CONDS) { s->cbin[s->ncond] = g_list_bin; s->cond[s->ncond++] = (uint16_t)g_list_q; } else g_st.results_lost++;
    }
    g_list_q = -1;
    if (g_pred_on) {
        const VkConditionalRenderingBeginInfoEXT bi = {.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT, .buffer = g_pbuf, .offset = g_pred_off,
            .flags = g_mode == VKN_COND_POISON ? VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT : 0};
        g_pbegin(cb, &bi);
        g_cond_active = 1;
    }
}
void vkn_query_draw_post(VkCommandBuffer cb) {
    if (vkn_cond_is_b(g_mode)) { if (vkn_occb_replaying()) return; vkn_occb_q_draw_post(); }   // [stage 5 technique B]
    if (g_cond_active) { g_pend(cb); g_cond_active = 0; }
    g_pred_on = 0;
}

// ---- pass boundaries ----
void vkn_query_pass_closing(VkCommandBuffer cb) {
    if (!g_mode) return;
    if (g_cond_active) { g_pend(cb); g_cond_active = 0; }   // (not reached: begin / end surround one draw command)
    if (vkn_cond_is_b(g_mode) && vkn_occb_replaying()) return;   // [stage 5 technique B] (a replay ended the pass: B counts it)
    if (g_open >= 0 && g_cur >= 0) {
        q_rec *r = &g_rec[g_open];
        if (r->st == Q_ACTIVE) {   // a query must end in the pass instance it began in: the pair's later draws may pass samples elsewhere
            end_active(cb, r);
            r->st = Q_VISIBLE; g_st.broken++;
        }
    }
    if (vkn_cond_is_b(g_mode)) vkn_occb_pass_closing(cb);   // [stage 5 technique B] the captured draws of this pass instance, before it ends
}
void vkn_query_pass_closed(VkCommandBuffer cb) {
    if (!g_mode || g_cur < 0) return;
    q_slot *s = &g_slot[g_cur];
    if (s->copied >= s->nq) return;
    if (vkn_cond_is_b(g_mode) && vkn_occb_q_active()) return;   // [stage 5 technique B] B's query is still open (a replay broke the pass): copied later, never waited on
    T_BEGIN;
    // every query begun so far has ended (a pair open at this pass end was ended just before it): copy their results, one word each
    const uint32_t first = s->copied, n = s->nq - first;
    vkCmdCopyQueryPoolResults(cb, s->pool, first, n, g_pbuf, ((VkDeviceSize)g_cur * VKN_Q_CAP + first) * 4u, 4, VK_QUERY_RESULT_WAIT_BIT);
    const VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = g_gpu ? VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT | VK_ACCESS_HOST_READ_BIT : VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, g_gpu ? VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT | VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    for (uint32_t q = first; q < s->nq; q++) {
        q_rec *r = &g_rec[s->qrec[q]];
        if (r->st == Q_CLOSED && r->q == (int32_t)q) r->st = Q_COPIED;
        s->qkey[q] = r->st == Q_COPIED && r->q == (int32_t)q ? r->okey : 0;   // a verdict of the whole pair, or none
        if (vkn_cond_is_a(g_mode)) vkn_occ_q_copied(g_cur, q, s->qrec[q], r->st == Q_COPIED && r->q == (int32_t)q);   // [stage 5 technique A]
        if (vkn_cond_is_b(g_mode)) vkn_occb_q_copied(g_cur, q, s->qrec[q], r->st == Q_COPIED && r->q == (int32_t)q);   // [stage 5 technique B]
        if (g_mode == VKN_COND_Z_VERIFY) vkn_occz_q_copied(g_cur, q, r->key, r->st == Q_COPIED && r->q == (int32_t)q);   // [stage 5 technique Z]
    }
    s->copied = s->nq; g_st.copies++; g_st.copied += n;
    T_END;
}

// ==== stage 5.1 O1: object keys, census bins, mode 4's CPU skips (vkn_fx function, order 50: before every other piece) ====
// Cheap first: a draw that is neither inside a pair nor conditional returns at once. The draw hash is the old path's (xbg_nr_occ.inc
// draw_enter: primitive, count, first, instances, index buffer, vertex streams, shader), taken for a pair's first draw only.
static int hidden_key(uint64_t k) {
    const nro_hent *e = k && g_hist.serial ? nro_hist_get(&g_hist, k) : NULL;
    return e && e->last == g_hist.serial && e->count == 1 && e->streak >= 1;
}
static void occ_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    if (g_open < 0 && g_dcond == -1) return;
    if (g_mode != VKN_COND_COUNT && g_mode != VKN_COND_ORACLE) return;
    if (g_open >= 0) {
        q_rec *r = &g_rec[g_open];
        if (r->seen == 1 && !r->okey) {   // the pair's first draw: its object key
            uint64_t h = nro_mix(0x6f636331ull, (uint64_t)ctx->prim | (uint64_t)ctx->count << 32);
            h = nro_mix(h, (uint64_t)ctx->first | (uint64_t)ctx->instances << 32);
            h = nro_mix(h, ctx->indexed ? in->ib : 0);
            for (int i = 0; i < 3; i++) h = nro_mix(h, in->vb[i] ^ ((uint64_t)in->vb_off[i] << 40));
            h = nro_mix(h, in->shader);
            const uint32_t occ = mm_next(h);
            r->okey = nro_mix(h, occ) | 1u;
            g_os.keyed++; if (occ) g_os.repeated++;
            if (g_o1_skip) r->skip = (uint8_t)hidden_key(r->okey);
        } else if (r->seen == 2) g_os.several++;
        if (g_o1_skip && r->skip) { out->skip = 1; g_os.skip_pre++; g_os.skip_idx += (uint64_t)ctx->count * ctx->instances; return; }
    }
    if (g_dcond == -1) return;
    const uint8_t bin = (uint8_t)(kind_of(ctx->color[0], ctx->depth) << 3 | bucket_of((uint64_t)ctx->count * ctx->instances));
    g_list_bin = bin;
    bin_add(g_os.cond_k, g_os.cond_b, bin);
    if (g_o1_skip && g_dcond >= 0) {
        const q_rec *c = &g_rec[g_dcond];
        const int empty = c->st == Q_EMPTY;
        if (c->skip || empty) {
            out->skip = 1; g_os.skip_cond++; g_os.skip_empty += (uint64_t)empty; g_os.skip_idx += (uint64_t)ctx->count * ctx->instances;
            bin_add(g_os.skip_k, g_os.skip_b, bin);
        }
    }
}

// ==== stage 5.1 O2: DM_VK_NATIVE_GBUF_LE (rule: vkn_query.h) ====
static const char *const k_le_why[VKN_LE_REASONS] = {
    "", "no pair", "pair not closed", "empty pair", "several draws in the pair", "pre-pass draw not recorded", "pre-pass draw not seen whole",
    "other depth target", "depth raised / cleared since", "not the pair's draw", "other vertex stream 0", "other cull / fill / bias",
    "other viewport / wider scissor", "pre-pass depth state", "pre-pass fragment discard / depth / mask / unknown / swapped", "pre-pass alpha test"};
// (a capture of a pre-pass draw records its own refusal with the same reasons: VKN_LE_TARGET, VKN_LE_PRE_*)
enum { DSC_KNOWN = 1, DSC_EQ = 2, DSC_RAISE = 4, DSC_PRE = 8 };
typedef struct le_cap {
    vkn_tex *depth; uint64_t ib, vb0; uint32_t vb0_off, first, count, instances, ev;
    float vp[4]; int32_t sc[4]; uint16_t raster; uint8_t prim, indexed, why;
} le_cap;
static int g_le_req, g_le_on, g_le_gate, g_le_sim_tiler;     // asked; in effect (after the family gate); gate decided; DM_GFX_SIM_GPU tiler model
static int (*g_le_facts)(const vkn_shader *s);
static le_cap g_cap[VKN_Q_RECS];
static uint8_t g_dsc[256], g_rk[256]; static uint16_t g_le_map[256];
static struct { vkn_tex *t; uint32_t ev; } g_dev[8]; static uint32_t g_ndev, g_ev, g_dev_over;   // depth events per target
static vkn_query_le_stats g_ls, g_ls_prev;

int vkn_query_le(void) { return g_le_on; }
int vkn_query_le_asked(void) { return g_le_req != 0; }
void vkn_query_le_test_facts(int (*fn)(const vkn_shader *s)) { g_le_facts = fn; }
void vkn_query_le_stats_get(vkn_query_le_stats *s) { if (s) *s = g_ls; }

static uint8_t ds_class(uint16_t id) {
    if (!id) return DSC_KNOWN;   // no depth-stencil state: depth test off
    uint8_t c = g_dsc[id & 255];
    if (c) return c;
    uint8_t a[8]; c = DSC_KNOWN;
    if (!vkn_ds_args(id, a) && a[0]) {   // test, write, func (0 never 1 less 2 equal 3 lequal 4 greater 5 notequal 6 gequal 7 always), stencil on, mask, func
        if (a[2] == 2) c |= DSC_EQ;
        if (a[1] && a[2] >= 4) c |= DSC_RAISE;
        if (a[1] && (a[2] == 1 || a[2] == 3) && (!a[3] || a[5] == 7)) c |= DSC_PRE;
    }
    return g_dsc[id & 255] = c;
}
static uint8_t raster_key(uint16_t id) {   // cull / fill / bias of a raster id (flagA / flagB are never applied); 0 = unknown
    if (!id) return 0x40;   // the default raster state: its own class
    uint8_t k = g_rk[id & 255];
    if (k) return k;
    uint8_t a[5];
    k = vkn_raster_args(id, a) ? 0 : (uint8_t)(0x80 | (a[0] & 3) | (a[1] & 3) << 2 | (a[4] & 1) << 4);
    return g_rk[id & 255] = k;
}
static void dev_bump(vkn_tex *t) {
    g_ls.depth_events++; ++g_ev;
    for (uint32_t i = 0; i < g_ndev; i++) if (g_dev[i].t == t) { g_dev[i].ev = g_ev; return; }
    if (g_ndev < 8) { g_dev[g_ndev].t = t; g_dev[g_ndev++].ev = g_ev; } else g_dev_over = g_ev;   // (a ninth target: every unknown one is dirty now)
}
static uint32_t dev_last(const vkn_tex *t) {
    for (uint32_t i = 0; i < g_ndev; i++) if (g_dev[i].t == t) return g_dev[i].ev;
    return g_dev_over;
}
static int fs_facts(const vkn_shader *s) {
    if (g_le_facts) return g_le_facts(s);
    if (!(vkn_shlib_on && vkn_shlib_on() && vkn_shlib_fs_facts)) return -1;
    return vkn_shlib_fs_facts(s->xs[VKN_SRC_FS]);
}
static void le_gate(void) {
    g_le_gate = 1;
    const vkn_dev *d = vkn_device();
    const uint32_t v = d ? d->props.vendorID : 0;
    const int tiler = v == 0x13B5 || v == 0x1010 || g_le_sim_tiler;
    g_le_on = tiler ? 0 : g_le_req;
    vkn_logf("query: DM_VK_NATIVE_GBUF_LE=%d -> %d (%s): EQUAL -> LEQUAL for the G-buffer draws of objects drawn whole in the pre-pass%s",
             g_le_req, g_le_on, tiler ? "a tile GPU / model: always off there" : g_le_on == 2 ? "count only" : g_le_on ? "on" : "off",
             g_le_on ? "; exactness gate: the target dumps of the phone" : "");
}
// order 1000, every draw that got this far: depth events of its target, and a pair's first draw as the pre-pass finally draws it
static void le_pre_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    if (!g_le_req) return;
    if (!g_le_gate) le_gate();
    if (!g_le_on) return;
    vkn_tex *d = ctx->depth;
    const uint16_t ds = out->ds ? out->ds : ctx->ds;
    if (d && ((ds_class(ds) & DSC_RAISE) || (d->pass_dclr & 1))) dev_bump(d);   // a pending clear runs when this draw's pass begins
    if (g_open < 0 || g_rec[g_open].seen != 1) return;
    q_rec *r = &g_rec[g_open];
    le_cap *c = &g_cap[g_open];
    g_ls.pre_seen++;
    int why = 0;
    const vkn_shader *s = ctx->shader;
    if (!d) why = VKN_LE_TARGET;
    else if (!(ds_class(ds) & DSC_PRE)) why = VKN_LE_PRE_DS;
    else if (out->discard || out->shader || out->vin.vs) why = VKN_LE_PRE_FS;   // primitives discarded / a piece's shader or vertex module (MAT B / XLQ / PRESKIN, as before [stage 5 rest])
    else if (!out->no_fs && s && (s->stages & (1u << VKN_SRC_FS))) {
        if (in->alpha_ref >= 0.0f) why = VKN_LE_PRE_ALPHA;   // the "#at" variant discards
        else { const int f = fs_facts(s); if (f != 0) why = VKN_LE_PRE_FS; }   // -1 unknown, or kill / depth / mask
    }
    c->depth = d; c->ib = ctx->indexed ? in->ib : 0; c->vb0 = in->vb[0]; c->vb0_off = in->vb_off[0];
    c->first = ctx->first; c->count = ctx->count; c->instances = ctx->instances; c->ev = g_ev;
    memcpy(c->vp, in->viewport, sizeof c->vp); memcpy(c->sc, in->scissor, sizeof c->sc);
    c->raster = ctx->raster; c->prim = ctx->prim; c->indexed = ctx->indexed; c->why = (uint8_t)why;
    r->cap = 1;
    if (why) g_ls.pre_refused[why]++;
}
static int le_check(const vkn_draw_in *in, const vkn_fx_ctx *ctx) {
    if (g_dcond < 0) return VKN_LE_NO_PAIR;
    const q_rec *r = &g_rec[g_dcond];
    if (!r->closed) return VKN_LE_OPEN;
    if (!r->seen) return VKN_LE_EMPTY;
    if (r->seen > 1) return VKN_LE_SEVERAL;
    if (r->done != r->seen) return VKN_LE_LOST;
    if (!r->cap) return VKN_LE_NOCAP;
    const le_cap *c = &g_cap[g_dcond];
    if (c->why) return c->why;
    if (ctx->depth != c->depth) return VKN_LE_TARGET;
    if ((ctx->depth->pass_dclr & 1) || dev_last(ctx->depth) > c->ev) return VKN_LE_DEPTH_EVENT;
    if (ctx->indexed != c->indexed || (ctx->indexed && in->ib != c->ib) || ctx->first != c->first || ctx->count != c->count ||
        ctx->instances != c->instances || ctx->prim != c->prim) return VKN_LE_DRAW;
    if (in->vb[0] != c->vb0 || in->vb_off[0] != c->vb0_off) return VKN_LE_STREAM;
    if (ctx->raster != c->raster) { const uint8_t a = raster_key(ctx->raster), b = raster_key(c->raster); if (!a || a != b) return VKN_LE_RASTER; }
    if (memcmp(in->viewport, c->vp, sizeof c->vp)) return VKN_LE_VIEW;
    const int64_t gx = in->scissor[0], gy = in->scissor[1], cx = c->sc[0], cy = c->sc[1];
    if (gx < cx || gy < cy || gx + in->scissor[2] > cx + c->sc[2] || gy + in->scissor[3] > cy + c->sc[3]) return VKN_LE_VIEW;
    return 0;
}
static uint16_t le_swap_id(uint16_t base) {
    uint16_t v = g_le_map[base & 255];
    if (v) return v;
    uint8_t a[8];
    if (vkn_ds_args(base, a)) return 0;
    v = vkn_fx_ds(a[0], a[1], 3, a[3], a[4], a[5], a[6], a[7]);   // the same state, compare LESS_OR_EQUAL
    if (!v) { g_ls.ds_table_full++; QRED(R_LE_TABLE, "gbuf-le: no depth-stencil id for the LEQUAL twin of state %u (table full): drawn with EQUAL", base); return 0; }
    g_ls.twins++;
    return g_le_map[base & 255] = v;
}
// order 250: the swap. Cheap first: an unconditional draw returns at once.
static void le_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    if (g_dcond == -1 || !g_le_on) return;
    g_ls.conds++;
    const uint16_t base = out->ds ? out->ds : ctx->ds;
    if (!(ds_class(base) & DSC_EQ) || !ctx->depth) { g_ls.not_equal++; return; }
    g_ls.cand++;
    const int why = le_check(in, ctx);
    if (why) { g_ls.refused[why]++; return; }
    const uint16_t sw = le_swap_id(base);
    if (!sw) return;
    g_ls.swaps++;
    if (g_le_on == 1) out->ds = sw;
}
void vkn_query_le_init(int value) {
    g_le_req = value == 1 || value == 2 ? value : 0;
    if (!g_mode) g_rec_noq = 1;   // (until the next recording begin sets it)
    g_le_gate = 0; g_le_on = 0;
    const char *sim = getenv("DM_GFX_SIM_GPU");
    g_le_sim_tiler = sim && (strstr(sim, "mali") || strstr(sim, "pvr") || strstr(sim, "powervr"));
    if (g_le_req) {
        static int reg;
        listeners();
        if (!reg) reg = !vkn_fx_register("gbuf-le", le_fx, NULL, 250) && !vkn_fx_register("gbuf-le-pre", le_pre_fx, NULL, 1000);
        if (vkn_device()) le_gate();   // (else at the first draw)
    }
    g_track = g_mode || g_le_req;
}

// ---- census (from the host's [vkn] query census, every 10 s) ----
static int put_bins(char *o, int n, int cap, const uint64_t *k, const uint64_t *k0, const uint64_t *b, const uint64_t *b0, double df) {
    static const char *const bn[BUCKETS] = {"<=64", "<=256", "<=1K", "<=4K", "<=16K", ">16K"};
    n += snprintf(o + n, (size_t)(cap - n), " by pass [");
    for (uint32_t i = 0; i < KINDS && n < cap - 64; i++) if (k[i] - k0[i]) n += snprintf(o + n, (size_t)(cap - n), "%s %.1f, ", i == KIND_OTHER ? "other" : g_kind[i].name, (double)(k[i] - k0[i]) / df);
    n += snprintf(o + n, (size_t)(cap - n), "] by indices x instances [");
    for (int i = 0; i < BUCKETS && n < cap - 32; i++) n += snprintf(o + n, (size_t)(cap - n), "%s %.1f%s", bn[i], (double)(b[i] - b0[i]) / df, i + 1 < BUCKETS ? ", " : "");
    return n + snprintf(o + n, (size_t)(cap - n), "]");
}
// ==== [stage 5 technique B] what vkn_occb.c needs of this module (recording thread; vkn_query.h) ====
int vkn_query_gpu_pred(void) { return g_mode == VKN_COND_ON || g_mode == VKN_COND_POISON || vkn_cond_is_b(g_mode); }
int vkn_query_b_qalloc(VkQueryPool *pool, uint32_t *q) {
    if (g_cur < 0 || g_broken) return -1;
    q_slot *s = &g_slot[g_cur];
    if (s->nq >= VKN_Q_CAP) { g_st.pool_full++; return -1; }
    *q = s->nq; *pool = s->pool;
    s->qrec[s->nq] = (uint16_t)SPARE; s->qkey[s->nq] = 0; s->nq++; s->dirty = s->nq; g_st.queries++;
    return 0;
}
void vkn_query_b_pred(uint32_t q, VkBuffer *buf, VkDeviceSize *off) {
    *buf = g_pbuf; *off = ((VkDeviceSize)(g_cur < 0 ? 0 : g_cur) * VKN_Q_CAP + q) * 4u;
}
int vkn_query_b_cond(VkCommandBuffer cb, int on, VkBuffer buf, VkDeviceSize off) {
    if (!g_gpu || !g_pbegin || !g_pend || !cb) return -1;
    if (on) {
        const VkConditionalRenderingBeginInfoEXT bi = {.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT, .buffer = buf, .offset = off, .flags = 0};
        g_pbegin(cb, &bi);
    } else g_pend(cb);
    return 0;
}
void vkn_query_b_learn(uint32_t rec) {
    if (rec >= g_nrec || rec >= VKN_Q_RECS) return;
    q_rec *r = &g_rec[rec];
    if (r->st == Q_NOQ && g_cur >= 0 && !g_rec_noq) r->st = Q_PENDING;
}
int vkn_query_b_noq(uint32_t rec) { return rec < g_nrec && rec < VKN_Q_RECS && g_rec[rec].st == Q_NOQ; }
void vkn_query_b_end_active(VkCommandBuffer cb) {
    if (g_open < 0 || g_cur < 0 || !cb) return;
    q_rec *r = &g_rec[g_open];
    if (r->st == Q_ACTIVE) { end_active(cb, r); r->st = Q_VISIBLE; g_st.broken++; }
}
// [stage 5 technique A] what vkn_occ.c reads of the pairs (recording thread)
int vkn_query_occ_open(void) { return g_open; }
uint64_t vkn_query_occ_key(uint32_t rec) { return rec < g_nrec && rec < VKN_Q_RECS ? g_rec[rec].key : 0; }   // [stage 5 technique Z]
int vkn_query_occ_cond(void) { return g_dcond; }
void vkn_query_occ_nrec(uint32_t *n) { *n = g_nrec; }
void vkn_query_occ_skipped(void) { if (g_open >= 0) g_rec[g_open].done++; }
int vkn_query_occ_rec(uint32_t rec, uint32_t *seen, uint32_t *done, int *closed, int *empty) {
    if (rec >= g_nrec) return -1;
    const q_rec *r = &g_rec[rec];
    *seen = r->seen; *done = r->done; *closed = r->closed; *empty = r->st == Q_EMPTY;
    return 0;
}
void vkn_query_census_occ(void) {
    if (vkn_cond_is_z(g_mode)) vkn_occz_census();   // [stage 5 technique Z]
    if (vkn_cond_is_a(g_mode)) vkn_occ_census();   // [stage 5 technique A]
    if (vkn_cond_is_b(g_mode)) vkn_occb_census();  // [stage 5 technique B]
    const uint64_t f = vkn_frame(), df = f > g_os_frame ? f - g_os_frame : 1;
    if (g_mode == VKN_COND_COUNT || g_mode == VKN_COND_ORACLE) {
        const vkn_query_occ_stats *a = &g_os, *p = &g_os_prev;
        const double rf = a->read_frames > p->read_frames ? (double)(a->read_frames - p->read_frames) : 1.0, pr = (double)(a->read_pairs - p->read_pairs);
        char o[2400]; int n = 0; const int cap = (int)sizeof o;
        n += snprintf(o + n, (size_t)(cap - n), "query census: DM_VK_NATIVE_COND=%d%s; a frame: conditional draws", g_mode, g_mode == VKN_COND_ORACLE ? " (ORACLE, INEXACT)" : "");
        n = put_bins(o, n, cap, a->cond_k, p->cond_k, a->cond_b, p->cond_b, (double)df);
        n += snprintf(o + n, (size_t)(cap - n), "; pairs keyed %.1f (repeated draw %.1f, several draws %.1f); read frames %llu: pairs %.1f, hidden %.1f, "
                      "stable keys %.1f %%, verdict changes %.1f; GPU culled a read frame", (double)(a->keyed - p->keyed) / (double)df,
                      (double)(a->repeated - p->repeated) / (double)df, (double)(a->several - p->several) / (double)df,
                      (unsigned long long)(a->read_frames - p->read_frames), pr / rf, (double)(a->read_hidden - p->read_hidden) / rf,
                      pr > 0 ? 100.0 * (double)(a->read_stable - p->read_stable) / pr : 0.0, (double)(a->read_changed - p->read_changed) / rf);
        n = put_bins(o, n, cap, a->cull_k, p->cull_k, a->cull_b, p->cull_b, rf);
        if (g_mode == VKN_COND_ORACLE) {
            n += snprintf(o + n, (size_t)(cap - n), "; oracle recordings %llu (late %llu), CPU-skipped a frame: pre-pass draws %.1f, conditional draws %.1f "
                          "(of empty pairs %.1f), indices %.0f; conditional draws between oracles %.1f; skipped",
                          (unsigned long long)(a->oracle_frames - p->oracle_frames), (unsigned long long)(a->oracle_lost - p->oracle_lost),
                          (double)(a->skip_pre - p->skip_pre) / (double)df, (double)(a->skip_cond - p->skip_cond) / (double)df,
                          (double)(a->skip_empty - p->skip_empty) / (double)df, (double)(a->skip_idx - p->skip_idx) / (double)df,
                          (double)(a->cond_noq - p->cond_noq) / (double)df);
            n = put_bins(o, n, cap, a->skip_k, p->skip_k, a->skip_b, p->skip_b, (double)df);
        }
        (void)n;
        vkn_logf("%s", o);
        g_os_prev = g_os;
    }
    if (g_le_req) {
        const vkn_query_le_stats *a = &g_ls, *p = &g_ls_prev;
        char o[1600]; int n = 0; const int cap = (int)sizeof o;
        n += snprintf(o + n, (size_t)(cap - n), "query gbuf-le: DM_VK_NATIVE_GBUF_LE=%d in effect %d; a frame: conditional draws %.1f (not EQUAL %.1f), EQUAL %.1f -> LEQUAL %.1f%s; refused:",
                      g_le_req, g_le_on, (double)(a->conds - p->conds) / (double)df, (double)(a->not_equal - p->not_equal) / (double)df, (double)(a->cand - p->cand) / (double)df,
                      (double)(a->swaps - p->swaps) / (double)df, g_le_on == 2 ? " (count only: not applied)" : "");
        for (int i = 1; i < VKN_LE_REASONS && n < cap - 96; i++) if (a->refused[i] - p->refused[i]) n += snprintf(o + n, (size_t)(cap - n), " %s %.1f,", k_le_why[i], (double)(a->refused[i] - p->refused[i]) / (double)df);
        n += snprintf(o + n, (size_t)(cap - n), " | pre-pass first draws %.1f, unusable:", (double)(a->pre_seen - p->pre_seen) / (double)df);
        for (int i = 1; i < VKN_LE_REASONS && n < cap - 96; i++) if (a->pre_refused[i] - p->pre_refused[i]) n += snprintf(o + n, (size_t)(cap - n), " %s %.1f,", k_le_why[i], (double)(a->pre_refused[i] - p->pre_refused[i]) / (double)df);
        n += snprintf(o + n, (size_t)(cap - n), " | depth events %.1f; LEQUAL twin states %llu, table full %llu", (double)(a->depth_events - p->depth_events) / (double)df,
                      (unsigned long long)a->twins, (unsigned long long)a->ds_table_full);
        (void)n;
        vkn_logf("%s", o);
        g_ls_prev = g_ls;
    }
    g_os_frame = f;
}
#endif
