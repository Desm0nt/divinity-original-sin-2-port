// [DOS2] VK-NATIVE stage 4: conditional rendering and visibility queries (contract and rules: vkn_query.h). No game addresses.
#ifdef DM_GAME_DOS2
#include <stdlib.h>
#include <string.h>
#include "vkn_query.h"
#include "vkn_frame.h"

#define Q_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark
// The front end (android/vulkan_present.cpp) enabled VK_EXT_conditional_rendering on the device (asked for with DM_VK_NATIVE_COND 1 / 3).
extern int dm_vk_cond_render __attribute__((weak));

// Bracket states (see vkn_query.h). Q_NOEXT: drew something on a device without the extension - no Vulkan query is made then (only
// the count mode needs the result without predicates), the consumers are drawn.
enum { Q_NONE = 0, Q_PENDING, Q_ACTIVE, Q_CLOSED, Q_COPIED, Q_EMPTY, Q_VISIBLE, Q_NOEXT };
enum { MAP = 8192, SPARE = VKN_Q_RECS, REDS = 16, RETIRE = 3 };   // RETIRE = vkn.h's retirement delay (frames)
enum { R_NESTED, R_UNMATCHED, R_FULL, R_SETUP, R_RECS, R_N };

typedef struct q_rec { uint64_t key; int32_t q; uint32_t seen, done; uint8_t st; } q_rec;
typedef struct q_slot {
    VkQueryPool pool;
    uint64_t frame;                  // vkn_frame() of its last recording
    uint32_t nq, copied, dirty;      // queries begun / copied in the current use; queries to reset before the next use
    uint32_t ncond; int used;
    uint16_t qrec[VKN_Q_CAP];        // query -> record of the current use
    uint16_t cond[VKN_Q_CONDS];      // queries whose predicate judged a draw (read back when the slot is reused)
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

int vkn_query_mode(void) { return g_mode; }
int vkn_query_gpu(void) { return g_gpu; }
void vkn_query_stats_get(vkn_query_stats *s) { if (s) *s = g_st; }

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

// The extension's entry points (vkGetDeviceProcAddr is in the trampoline table; the two EXT names are not): only when the front end
// enabled the extension on the device, and only for a predicate buffer created with the conditional-rendering usage.
static void probe(const vkn_dev *d) {
    g_pbegin = NULL; g_pend = NULL; g_gpu = 0;
    if (!(&dm_vk_cond_render && dm_vk_cond_render) || (g_setup > 1 && !g_buf_cond)) return;
    g_pbegin = (PFN_vkCmdBeginConditionalRenderingEXT)(void *)vkGetDeviceProcAddr(d->dev, "vkCmdBeginConditionalRenderingEXT");
    g_pend = (PFN_vkCmdEndConditionalRenderingEXT)(void *)vkGetDeviceProcAddr(d->dev, "vkCmdEndConditionalRenderingEXT");
    g_gpu = g_pbegin && g_pend;
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
    vkn_logf("query: DM_VK_NATIVE_COND=%d (%s): %d slots x %d occlusion queries, predicates %s", g_mode,
             g_mode == VKN_COND_ON ? "on" : g_mode == VKN_COND_COUNT ? "count only, everything drawn" : "poison: predicates inverted", VKN_Q_SLOTS, VKN_Q_CAP,
             g_gpu ? "on the GPU (VK_EXT_conditional_rendering)" : "not available (VK_EXT_conditional_rendering not enabled): only empty brackets are skipped");
    return 0;
fail:
    g_broken = 1; g_st.setup_fail++;
    QRED(R_SETUP, "query pools / predicate buffer could not be created: conditional rendering stays off (everything drawn)");
    return -1;
}

// ---- slots ----
static void readback(q_slot *s, int si) {   // the slot's last recording finished (RETIRE rule): the GPU's verdicts on its draws
    for (uint32_t i = 0; i < s->ncond; i++) {
        const uint32_t v = g_pred[(uint32_t)si * VKN_Q_CAP + s->cond[i]];
        g_st.results_read++;
        if (v) g_st.gpu_drawn++; else g_st.gpu_culled++;
    }
    s->ncond = 0;
}
static void on_begin(void) {
    g_cur = -1; g_open = -1; g_cond_set = 0; g_pred_on = 0; g_cond_active = 0; g_list_q = -1; g_nrec = 0;
    if (++g_gen == 0) { memset(g_map, 0, sizeof g_map); g_gen = 1; }
    if (!g_mode || g_broken || !vkn_rec_presented()) return;
    if (!g_setup && setup()) return;
    if (g_broken) return;
    T_BEGIN;
    q_slot *s = &g_slot[g_next];
    if (s->used && s->frame + RETIRE > vkn_frame()) { g_st.slots_busy++; T_END; return; }   // (its recording may still run)
    if (s->used) readback(s, g_next);
    VkCommandBuffer cb = vkn_cb();
    if (s->dirty) vkCmdResetQueryPool(cb, s->pool, 0, s->dirty);
    s->nq = s->copied = s->dirty = 0; s->ncond = 0; s->used = 1; s->frame = vkn_frame();
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
void vkn_query_init(int mode) {
    g_mode = mode < 0 || mode >= VKN_COND_MODES ? VKN_COND_OFF : mode;
    if (g_setup > 1 && !g_broken && vkn_device()) probe(vkn_device());   // (a second call: the host test switches modes / the extension)
    static int listeners;
    if (!listeners && g_mode) { listeners = 1; vkn_rec_on_begin(on_begin); vkn_rec_on_close(on_close); }
}

// ---- the engine's reports ----
void vkn_query_begin(uint64_t key) {
    g_st.begins++;
    if (!g_mode || !key) return;
    T_BEGIN;
    if (g_open >= 0) {   // nested / a begin without its end: the open bracket is unusable (one occlusion query at a time)
        q_rec *o = &g_rec[g_open];
        if (o->st == Q_ACTIVE) end_active(vkn_cb(), o);
        if (o->st != Q_VISIBLE) o->st = Q_VISIBLE;
        g_st.nested++;
        QRED(R_NESTED, "report pair %#llx begins inside the open pair %#llx: the open one counts as visible", (unsigned long long)key, (unsigned long long)o->key);
        g_open = -1;
    }
    if (g_nrec >= VKN_Q_RECS) {
        g_st.pool_full++;
        QRED(R_RECS, "more than %d report pairs in one recording: pair %#llx counts as visible", VKN_Q_RECS, (unsigned long long)key);
        g_rec[SPARE].st = Q_VISIBLE; g_rec[SPARE].key = 0; map_put(key, SPARE);
        T_END; return;
    }
    const uint32_t i = g_nrec++;
    q_rec *r = &g_rec[i];
    r->key = key; r->q = -1; r->seen = r->done = 0;
    if (g_cur < 0) { r->st = Q_VISIBLE; g_st.no_slot++; } else r->st = Q_PENDING;
    map_put(key, i);
    g_open = (int)i;
    T_END;
}
void vkn_query_end(uint64_t key) {
    g_st.ends++;
    if (!g_mode || !key) return;
    T_BEGIN;
    if (g_open < 0 && map_get(key) == SPARE) { T_END; return; }   // (a pair beyond the record table: counted at its begin)
    if (g_open < 0 || g_rec[g_open].key != key) {
        g_st.unmatched++;
        QRED(R_UNMATCHED, "report pair %#llx ends without its begin (open pair %#llx)", (unsigned long long)key, g_open >= 0 ? (unsigned long long)g_rec[g_open].key : 0ull);
        T_END; return;
    }
    q_rec *r = &g_rec[g_open]; g_open = -1;
    const int complete = r->seen == r->done;   // every draw that entered the draw path inside the pair was recorded (or skipped by its own condition)
    if (r->st == Q_ACTIVE) {
        end_active(vkn_cb(), r);
        if (complete) r->st = Q_CLOSED; else { r->st = Q_VISIBLE; g_st.lost++; }
    } else if (r->st == Q_PENDING) {
        if (complete) { r->st = Q_EMPTY; g_st.empty++; } else { r->st = Q_VISIBLE; g_st.lost++; }
    }
    T_END;
}
void vkn_query_cond(uint64_t key) { g_cond_key = key; g_cond_set = key != 0; }
void vkn_query_dispatch(void) { if (g_cond_set) { g_cond_set = 0; g_st.cond_dispatch++; } }

// ---- the draw path ----
int vkn_query_draw_enter(void) {
    g_pred_on = 0; g_list_q = -1;
    if (!g_mode) { if (g_cond_set) { g_cond_set = 0; g_st.conds++; } return 0; }
    if (g_open >= 0) g_rec[g_open].seen++;
    if (!g_cond_set) return 0;
    T_BEGIN;
    const uint64_t key = g_cond_key; g_cond_set = 0;
    g_st.conds++;
    int skip = 0;
    const int ri = g_cur >= 0 ? map_get(key) : -1;
    if (g_cur < 0) g_st.drawn_no_slot++;
    else if (ri < 0) g_st.drawn_no_query++;
    else {
        const q_rec *r = &g_rec[ri];
        switch (r->st) {
        case Q_EMPTY:   // no draw between the reports: zero samples, the Switch skips it too (exact on the CPU)
            if (g_mode == VKN_COND_ON) { skip = 1; g_st.cond_cpu_skip++; }
            else if (g_mode == VKN_COND_COUNT) { g_st.cond_count++; g_st.count_empty++; }
            else g_st.drawn_poison_empty++;
            break;
        case Q_COPIED:
            if (g_mode == VKN_COND_COUNT) { g_list_q = r->q; g_st.cond_count++; }
            else if (g_gpu) { g_pred_on = 1; g_list_q = r->q; g_pred_off = ((uint32_t)g_cur * VKN_Q_CAP + (uint32_t)r->q) * 4u; g_st.cond_gpu++; }
            else g_st.drawn_no_ext++;
            break;
        case Q_CLOSED: g_st.drawn_same_pass++; break;   // its pass is still open: no predicate yet
        case Q_PENDING: case Q_ACTIVE: g_st.drawn_open++; break;
        case Q_NOEXT: g_st.drawn_no_ext++; break;
        default: g_st.drawn_unusable++; break;
        }
    }
    if (skip && g_open >= 0) g_rec[g_open].done++;
    T_END;
    return skip;
}
void vkn_query_draw_pre(VkCommandBuffer cb) {
    if (!g_mode) return;
    if (g_open >= 0) {
        q_rec *r = &g_rec[g_open];
        if (r->st == Q_PENDING) {   // the first recorded draw of the pair: the pass is open now, the query begins in it
            q_slot *s = &g_slot[g_cur];
            if (!g_gpu && g_mode != VKN_COND_COUNT) r->st = Q_NOEXT;   // (no predicate could use it: no query, not empty)
            else if (s->nq >= VKN_Q_CAP) { r->st = Q_VISIBLE; g_st.pool_full++; QRED(R_FULL, "more than %d queries in one recording: pair %#llx counts as visible", VKN_Q_CAP, (unsigned long long)r->key); }
            else { r->q = (int32_t)s->nq; s->qrec[s->nq] = (uint16_t)g_open; s->nq++; s->dirty = s->nq; vkCmdBeginQuery(cb, s->pool, (uint32_t)r->q, 0); r->st = Q_ACTIVE; g_st.queries++; }
        }
        r->done++;
    }
    if (g_list_q >= 0 && g_cur >= 0) {
        q_slot *s = &g_slot[g_cur];
        if (s->ncond < VKN_Q_CONDS) s->cond[s->ncond++] = (uint16_t)g_list_q; else g_st.results_lost++;
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
    if (g_cond_active) { g_pend(cb); g_cond_active = 0; }
    g_pred_on = 0;
}

// ---- pass boundaries ----
void vkn_query_pass_closing(VkCommandBuffer cb) {
    if (!g_mode) return;
    if (g_cond_active) { g_pend(cb); g_cond_active = 0; }   // (not reached: begin / end surround one draw command)
    if (g_open < 0 || g_cur < 0) return;
    q_rec *r = &g_rec[g_open];
    if (r->st == Q_ACTIVE) {   // a query must end in the pass instance it began in: the pair's later draws may pass samples elsewhere
        end_active(cb, r);
        r->st = Q_VISIBLE; g_st.broken++;
    }
}
void vkn_query_pass_closed(VkCommandBuffer cb) {
    if (!g_mode || g_cur < 0) return;
    q_slot *s = &g_slot[g_cur];
    if (s->copied >= s->nq) return;
    T_BEGIN;
    // every query begun so far has ended (a pair open at this pass end was ended just before it): copy their results, one word each
    const uint32_t first = s->copied, n = s->nq - first;
    vkCmdCopyQueryPoolResults(cb, s->pool, first, n, g_pbuf, ((VkDeviceSize)g_cur * VKN_Q_CAP + first) * 4u, 4, VK_QUERY_RESULT_WAIT_BIT);
    const VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = g_gpu ? VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT | VK_ACCESS_HOST_READ_BIT : VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, g_gpu ? VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT | VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    for (uint32_t q = first; q < s->nq; q++) { q_rec *r = &g_rec[s->qrec[q]]; if (r->st == Q_CLOSED && r->q == (int32_t)q) r->st = Q_COPIED; }
    s->copied = s->nq; g_st.copies++; g_st.copied += n;
    T_END;
}
#endif
