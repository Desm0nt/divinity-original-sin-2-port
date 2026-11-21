// [DOS2] VK-NATIVE stage 5.1 piece A0, measurement (vkn_prof.h: the switch, what is measured and how). No game addresses, no engine
// record offsets. State is the recording thread's (vkn_frame.h); the only cross-thread entry is vkn_prof_gpu_request (an atomic flag).
// GPU regions: query pool of VKN_PROF_SLOTS * QPF timestamps, region k = command buffer k of vkn_frame's ring (k < 3; the off-frame
// buffer is never sampled). Region layout: [0] top of the recording, [1] its bottom, [2 + 2i] / [3 + 2i] before / after pass i.
// Lifetime: the pool is made on the first sampled frame and lives as long as the device (as vkn_frame's own pool); a region is reset by
// the recording that writes it (vkCmdResetQueryPool at its top: the buffer's previous submission finished, its fence was waited for)
// and read in slot_done of that buffer, so no region is ever read or reset while the GPU may write it.
#ifdef DM_GAME_DOS2
#include "vkn_prof.h"
#include "vkn_buf.h"
#include "vkn_draw.h"
#include "vkn_frame.h"
#include "vkn_pass.h"
#include "vkn_ui.h"
#include "vkn_tex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PR_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"

enum { VKN_PROF_SLOTS = 4, MAXP = 384, QPF = 2 + 2 * MAXP, NAME = 96, DRAW_PERIOD = 128, TOP_KINDS = 24 };
static const char *const k_part[VKN_PROF_PARTS] = {"flush", "fill", "state", "fx", "buffers", "tex+set", "pass", "pipeline", "viewport", "draw-cmd", "tail"};
// [rt P0] the fine parts (vkn_prof.h): name, coarse part, 1 = only a driver call
static const struct { const char *name; uint8_t coarse, drv; } k_fine[VKN_PROF_FINE] = {
    {"tex lookups", VKN_PROF_TEX, 0}, {"pass sample", VKN_PROF_TEX, 0}, {"tex rest", VKN_PROF_TEX, 0}, {"set key", VKN_PROF_TEX, 0},
    {"set find", VKN_PROF_TEX, 0}, {"vkCmdBindDescriptorSets", VKN_PROF_TEX, 1}, {"pipe key", VKN_PROF_PIPE, 0}, {"pipe find", VKN_PROF_PIPE, 0},
    {"vkCmdBindPipeline", VKN_PROF_PIPE, 1}, {"buf lookups", VKN_PROF_BUF, 0}, {"vkCmdBind*Buffer(s)", VKN_PROF_BUF, 1}, {"vkCmdSetViewport/Scissor", VKN_PROF_VIEW, 1},
    {"query pre", VKN_PROF_CMD, 0}, {"vkCmdDraw*", VKN_PROF_CMD, 1}};
int vkn_prof_lvl3;

vkn_prof_hot vkn_prof_h __attribute__((aligned(64))) = {.left = 0xffffffffu};
int vkn_prof_gpu_on, vkn_prof_gpu_cur;

typedef struct pent { char name[NAME]; uint32_t w, h; uint8_t ended; uint64_t draws0, draws; } pent;
typedef struct kind { const pent *e; uint32_t n; uint64_t draws; double ms; } kind;
// One ring slot's sampled recording: passes recorded (np; `over` beyond MAXP untimed), its frame number, draws in it.
typedef struct pslot { int pending; uint32_t np, over, forced_end; uint64_t frame, draws0, draws; pent *p; } pslot;

static struct {
    int level, ts_tried, ts_ok, req;
    VkDevice dev; VkQueryPool qp; double period_ns; uint64_t mask;
    uint64_t interval_ns, next_ns, init_t;
    pslot s[VKN_PROF_SLOTS];
    int cur, open;                                 // sampled slot of the open recording; a pass of it is open (its begin was written)
    uint64_t q[QPF]; kind k[MAXP];
    uint64_t gpu_samples, gpu_lost, gpu_unready;    // GPU frames read / lost (submit failed) / results not available
    double last_gpu_ms; uint64_t last_gpu_frame, last_gpu_t;
    // draw parts (since init; the census takes deltas)
    uint64_t acc[VKN_PROF_PARTS], cnt[VKN_PROF_PARTS], hits, samples, unended, t0;
    uint64_t facc[VKN_PROF_FINE], fcnt[VKN_PROF_FINE], dcalls;   // [rt P0] level 3
    double timer_ns;
    // the host's frame times
    uint64_t fc_n, fc_begin, fc_rec, fc_end;
} g = {.cur = -1};

static uint64_t draws_now(void) {   // draws recorded so far (engine draws + UI draws: both go into passes)
    vkn_draw_stats d; vkn_draw_stats_get(&d);
    vkn_ui_stats u; vkn_ui_stats_get(&u);
    return d.draws + u.draws;
}

double vkn_prof_timer_ns(void) { return g.timer_ns; }   // [rt2 F1] (vkn_fx.c: subtracted from the per-function times)
void vkn_prof_init(int level) {
    g.level = level < 0 ? 0 : level > 3 ? 3 : level;
    vkn_prof_lvl3 = g.level == 3;   // [rt P0]
    vkn_buf_lockprof = vkn_tex_lockprof = g.level == 3;
    g.interval_ns = g.level >= 2 ? 2000000000ull : 20000000000ull;
    const uint64_t now = vkn_now_ns();
    g.next_ns = now + g.interval_ns; g.init_t = now;
    vkn_prof_h.on = 0; vkn_prof_h.left = g.level ? DRAW_PERIOD : 0xffffffffu;
    vkn_prof_gpu_on = g.level > 0; vkn_prof_gpu_cur = 0;
    // the cost of one timer read (subtracted per probe from the part times)
    const uint64_t a = vkn_now_ns();
    for (int i = 0; i < 64; i++) (void)vkn_now_ns();
    g.timer_ns = (double)(vkn_now_ns() - a) / 65.0;
    if (g.level)
        vkn_logf("prof: DM_VK_NATIVE_PROF=%d: draw parts on 1 of %d draws (timer read %.0f ns, subtracted), a GPU-sampled frame every %llu s, per-pass list %s",
                 g.level, DRAW_PERIOD, g.timer_ns, (unsigned long long)(g.interval_ns / 1000000000ull), g.level >= 2 ? "complete" : "top 24 kinds");
    if (g.level == 3) vkn_logf("prof: DM_VK_NATIVE_PROF=3: + the split (draw by part 3, fx by function, rt by record, pipeline changes, locks)");
    else vkn_logf("prof: DM_VK_NATIVE_PROF=0: no measurement lines");
}
int vkn_prof_level(void) { return g.level; }
void vkn_prof_gpu_request(void) { __atomic_store_n(&g.req, 1, __ATOMIC_RELEASE); }
void vkn_prof_frame_cpu(uint64_t begin_ns, uint64_t record_ns, uint64_t end_ns) {
    if (!g.level) return;
    g.fc_n++; g.fc_begin += begin_ns; g.fc_rec += record_ns; g.fc_end += end_ns;
}

// ---- GPU timestamps ----
// First sampled frame: the queue family's timestampValidBits decides (0 = no timestamps on this queue: one line, no GPU lines ever).
static int pool_ready(void) {
    if (g.ts_ok) return 1;
    if (g.ts_tried) return 0;
    const vkn_dev *d = vkn_device();
    if (!d) return 0;   // (not tried yet: the next frame asks again)
    g.ts_tried = 1;
    uint32_t n = 0; VkQueueFamilyProperties qf[16];
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &n, NULL);
    if (n > 16) n = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &n, qf);
    const uint32_t bits = d->family < n ? qf[d->family].timestampValidBits : 0;
    if (!bits || !(d->props.limits.timestampPeriod > 0)) {
        vkn_logf("prof: queue family %u has timestampValidBits %u (period %g ns): no GPU time by pass on this device", d->family, bits, (double)d->props.limits.timestampPeriod);
        vkn_prof_gpu_on = 0; return 0;
    }
    for (int i = 0; i < VKN_PROF_SLOTS; i++) {
        g.s[i].p = (pent *)calloc(MAXP, sizeof(pent));
        if (!g.s[i].p) { vkn_logf(PR_RED "prof: no memory for the pass records: no GPU time by pass"); vkn_prof_gpu_on = 0; return 0; }
    }
    const VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = VKN_PROF_SLOTS * QPF};
    const VkResult r = vkCreateQueryPool(d->dev, &qi, NULL, &g.qp);
    if (r != VK_SUCCESS) { vkn_logf(PR_RED "prof: vkCreateQueryPool of %u timestamps failed (VkResult %d): no GPU time by pass", qi.queryCount, (int)r); g.qp = VK_NULL_HANDLE; vkn_prof_gpu_on = 0; return 0; }
    g.dev = d->dev; g.period_ns = d->props.limits.timestampPeriod;
    g.mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    g.ts_ok = 1;
    vkn_logf("prof: GPU timestamps: %u valid bits, %.3f ns a tick, %u passes a frame at most", bits, g.period_ns, MAXP);
    return 1;
}
static double tick_ms(uint64_t a, uint64_t b) { return (double)((b - a) & g.mask) * g.period_ns / 1e6; }

void vkn_prof_rec_begin_(VkCommandBuffer cb, int slot, int presented) {
    vkn_prof_gpu_cur = 0; g.cur = -1; g.open = 0;
    if (slot < 0 || slot >= VKN_PROF_SLOTS) return;
    if (g.s[slot].pending) { g.s[slot].pending = 0; g.gpu_lost++; }   // its fence was never seen signalled: a recording lost at submit
    if (!presented || !cb) return;
    const uint64_t now = vkn_now_ns();
    if (now < g.next_ns && !__atomic_load_n(&g.req, __ATOMIC_ACQUIRE)) return;
    if (!pool_ready()) return;
    __atomic_store_n(&g.req, 0, __ATOMIC_RELEASE);
    g.next_ns = now + g.interval_ns;
    const uint32_t base = (uint32_t)slot * QPF;
    vkCmdResetQueryPool(cb, g.qp, base, QPF);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qp, base);   // after everything before it on the queue: this frame only
    g.s[slot].np = 0; g.s[slot].over = 0; g.s[slot].forced_end = 0; g.s[slot].frame = vkn_frame(); g.s[slot].draws0 = draws_now();
    g.cur = slot; vkn_prof_gpu_cur = 1;
}

void vkn_prof_pass_begin_(VkCommandBuffer cb, vkn_tex *const *color, uint32_t ncolor, vkn_tex *depth, uint32_t w, uint32_t h) {
    if (g.cur < 0 || !cb) return;
    pslot *s = &g.s[g.cur];
    if (g.open) {   // the previous pass has no end yet (vkn_pass always ends it first: should not happen): its end written here, every query is written
        pent *o = &s->p[s->np - 1];
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qp, (uint32_t)g.cur * QPF + 3 + 2 * (s->np - 1));
        o->ended = 2; o->draws = draws_now() - o->draws0; s->forced_end++; g.open = 0;
    }
    if (s->np >= MAXP) { s->over++; return; }
    pent *e = &s->p[s->np];
    const vkn_tex *c = NULL; uint32_t nc = 0;
    for (uint32_t i = 0; i < ncolor && color; i++) if (color[i]) { if (!c) c = color[i]; nc++; }
    char cn[40], dn[40];
    if (c) { if (c->sname[0] && strcmp(c->sname, "?")) snprintf(cn, sizeof cn, "%s", c->sname); else snprintf(cn, sizeof cn, "tex#%04llx", (unsigned long long)(c->handle & 0xffff)); }
    if (depth) { if (depth->sname[0] && strcmp(depth->sname, "?")) snprintf(dn, sizeof dn, "%s", depth->sname); else snprintf(dn, sizeof dn, "tex#%04llx", (unsigned long long)(depth->handle & 0xffff)); }
    if (c && nc > 1) snprintf(e->name, NAME, "%s(+%u)%s%s", cn, nc - 1, depth ? " +" : "", depth ? dn : "");
    else if (c) snprintf(e->name, NAME, "%s%s%s", cn, depth ? " +" : "", depth ? dn : "");
    else snprintf(e->name, NAME, "depth-only %s", depth ? dn : "?");
    e->w = w; e->h = h; e->ended = 0; e->draws0 = draws_now(); e->draws = 0;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qp, (uint32_t)g.cur * QPF + 2 + 2 * s->np);   // the work before the pass done
    s->np++; g.open = 1;
}
void vkn_prof_pass_end_(VkCommandBuffer cb) {
    if (g.cur < 0 || !g.open || !cb) return;
    pslot *s = &g.s[g.cur];
    pent *e = &s->p[s->np - 1];
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qp, (uint32_t)g.cur * QPF + 3 + 2 * (s->np - 1));   // the pass done
    e->ended = 1; e->draws = draws_now() - e->draws0; g.open = 0;
}
void vkn_prof_rec_close_(VkCommandBuffer cb, int slot) {
    vkn_prof_gpu_cur = 0;
    if (g.cur < 0 || g.cur != slot || !cb) { g.cur = -1; g.open = 0; return; }
    pslot *s = &g.s[slot];
    if (g.open) {   // a pass still open at the close (vkn_pass ends it in its close listener first: should not happen): its end is written here
        pent *e = &s->p[s->np - 1];
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qp, (uint32_t)slot * QPF + 3 + 2 * (s->np - 1));
        e->ended = 2; e->draws = draws_now() - e->draws0; s->forced_end++; g.open = 0;
    }
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qp, (uint32_t)slot * QPF + 1);
    s->draws = draws_now() - s->draws0;
    s->pending = 1; g.cur = -1;
}

static int kind_cmp(const void *a, const void *b) {
    const double x = ((const kind *)a)->ms, y = ((const kind *)b)->ms;
    return x < y ? 1 : x > y ? -1 : 0;
}
// The fence of ring slot `slot` signalled: its timestamps are final (read without a wait), the lines are printed.
void vkn_prof_slot_done_(int slot) {
    if (slot < 0 || slot >= VKN_PROF_SLOTS || !g.s[slot].pending || !g.ts_ok) return;
    pslot *s = &g.s[slot];
    s->pending = 0;
    const uint32_t n = 2 + 2 * s->np, base = (uint32_t)slot * QPF;
    const VkResult r = vkGetQueryPoolResults(g.dev, g.qp, base, n, sizeof(uint64_t) * n, g.q, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) {
        if (g.gpu_unready++ < 16) vkn_logf(PR_RED "prof: timestamps of the sampled frame %llu not available after its fence (VkResult %d): no GPU lines for it", (unsigned long long)s->frame, (int)r);
        return;
    }
    g.gpu_samples++;
    const double frame_ms = tick_ms(g.q[0], g.q[1]);
    uint32_t nk = 0; double sum = 0; uint64_t pdraws = 0;
    for (uint32_t i = 0; i < s->np; i++) {
        const pent *e = &s->p[i];
        const double ms = tick_ms(g.q[2 + 2 * i], g.q[3 + 2 * i]);
        sum += ms; pdraws += e->draws;
        uint32_t k = 0;
        for (; k < nk; k++) if (g.k[k].e->w == e->w && g.k[k].e->h == e->h && !strcmp(g.k[k].e->name, e->name)) break;
        if (k == nk) { g.k[nk].e = e; g.k[nk].n = 0; g.k[nk].draws = 0; g.k[nk].ms = 0; nk++; }
        g.k[k].n++; g.k[k].draws += e->draws; g.k[k].ms += ms;
    }
    qsort(g.k, nk, sizeof g.k[0], kind_cmp);
    g.last_gpu_ms = frame_ms; g.last_gpu_frame = s->frame; g.last_gpu_t = vkn_now_ns();
    char note[96]; note[0] = 0;
    if (s->over || s->forced_end) snprintf(note, sizeof note, "; %u passes over %u untimed (in outside), %u passes ended late", s->over, MAXP, s->forced_end);
    vkn_logf("gpu by pass (frame %llu, sample %llu, every %llu s): frame %.2f ms = passes %.2f ms (%u passes in %u kinds, %llu of %llu draws) + outside passes %.2f ms "
             "(uploads, barriers, copies, clears, dispatches)%s",
             (unsigned long long)s->frame, (unsigned long long)g.gpu_samples, (unsigned long long)(g.interval_ns / 1000000000ull), frame_ms, sum, s->np, nk,
             (unsigned long long)pdraws, (unsigned long long)s->draws, frame_ms - sum, note);
    const uint32_t top = g.level >= 2 ? nk : nk < TOP_KINDS ? nk : TOP_KINDS;
    for (uint32_t k = 0; k < top; k++)
        vkn_logf("gpu by pass: %2u. %6.3f ms %5.1f %%  %s %ux%u  x%u, %llu draws", k + 1, g.k[k].ms, frame_ms > 0 ? 100.0 * g.k[k].ms / frame_ms : 0.0,
                 g.k[k].e->name, g.k[k].e->w, g.k[k].e->h, g.k[k].n, (unsigned long long)g.k[k].draws);
    if (top < nk) {
        double ms = 0; uint32_t np = 0; uint64_t dr = 0;
        for (uint32_t k = top; k < nk; k++) { ms += g.k[k].ms; np += g.k[k].n; dr += g.k[k].draws; }
        vkn_logf("gpu by pass: the rest: %6.3f ms %5.1f %% in %u more kinds (%u passes, %llu draws)", ms, frame_ms > 0 ? 100.0 * ms / frame_ms : 0.0, nk - top, np, (unsigned long long)dr);
    }
}

// ---- draw probes ----
void vkn_prof_draw_slow(void) {
    vkn_prof_hot *h = &vkn_prof_h;
    if (h->on) { h->on = 0; h->on3 = 0; g.unended++; }   // the previous sampled draw left without vkn_prof_draw_end (its parts are kept)
    if (h->left) return;
    if (!g.level) { h->left = 0xffffffffu; return; }
    h->left = DRAW_PERIOD; g.hits++;
    g.samples++; h->on = 1; h->on3 = g.level == 3; g.t0 = h->last = vkn_now_ns();
}
void vkn_prof_mark_f_slow(int f) {   // [rt P0]
    const uint64_t t = vkn_now_ns();
    if ((unsigned)f >= VKN_PROF_FINE) f = VKN_PROF_F_TEX_REST;
    const uint64_t d = t - vkn_prof_h.last;
    g.facc[f] += d; g.fcnt[f]++; g.acc[k_fine[f].coarse] += d; g.cnt[k_fine[f].coarse]++;
    vkn_prof_h.last = t;
}
void vkn_prof_driver_calls(uint32_t n) { if (vkn_prof_h.on3) g.dcalls += n; }
void vkn_prof_mark_slow(int part) {
    const uint64_t t = vkn_now_ns();
    if ((unsigned)part >= VKN_PROF_PARTS) part = VKN_PROF_TAIL;
    g.acc[part] += t - vkn_prof_h.last; g.cnt[part]++;
    vkn_prof_h.last = t;
}
void vkn_prof_end_slow(void) { vkn_prof_mark_slow(VKN_PROF_TAIL); vkn_prof_h.on = 0; vkn_prof_h.on3 = 0; }

// ---- census (every 10 s, from the host's census): deltas since the last call ----
void vkn_prof_census(void) {
    if (!g.level) return;
    static struct {
        uint64_t acc[VKN_PROF_PARTS], cnt[VKN_PROF_PARTS], calls, samples, unended, fc_n, fc_begin, fc_rec, fc_end, frames, t;
        uint64_t facc[VKN_PROF_FINE], fcnt[VKN_PROF_FINE], dcalls; vkn_draw_prof3 p3; uint64_t lk[4];
        vkn_draw_stats d; vkn_pass_stats p; vkn_ui_stats u; uint64_t ring;
    } o;
    const uint64_t now = vkn_now_ns();
    const uint64_t calls = g.hits * DRAW_PERIOD + (DRAW_PERIOD - (vkn_prof_h.left <= DRAW_PERIOD ? vkn_prof_h.left : DRAW_PERIOD));
    vkn_frame_stats f; vkn_frame_stats_get(&f);
    vkn_draw_stats d; vkn_draw_stats_get(&d);
    vkn_pass_stats p; vkn_pass_stats_get(&p);
    vkn_ui_stats u; vkn_ui_stats_get(&u);
    vkn_buf_stats b; vkn_buf_stats_get(&b);
    const uint64_t frames = f.frames - o.frames, dcalls = calls - o.calls, dsamp = g.samples - o.samples;
    const double fr = frames ? (double)frames : 1.0, secs = (now - (o.t ? o.t : g.init_t)) / 1e9;
    // draw by part
    double part[VKN_PROF_PARTS], per = 0;
    for (int i = 0; i < VKN_PROF_PARTS; i++) {
        const double a = (double)(g.acc[i] - o.acc[i]) - (double)(g.cnt[i] - o.cnt[i]) * g.timer_ns;
        part[i] = dsamp && a > 0 ? a / (double)dsamp / 1000.0 : 0.0;   // us a draw
        per += part[i];
    }
    const double cpf = (double)dcalls / fr, draw_ms = per * cpf / 1000.0;
    char pl[400]; int m = 0; pl[0] = 0;
    for (int i = 0; i < VKN_PROF_PARTS && m < (int)sizeof pl - 32; i++) m += snprintf(pl + m, sizeof pl - (size_t)m, "%s%s %.2f", i ? ", " : "", k_part[i], part[i]);
    vkn_logf("draw by part (%.1f s, 1 of %d draws: %llu sampled of %llu calls, %llu unended): per draw %.2f us = %s us; per frame %.2f ms (%.0f calls a frame, %llu frames); "
             "timer %.0f ns a probe subtracted",
             secs, DRAW_PERIOD, (unsigned long long)dsamp, (unsigned long long)dcalls, (unsigned long long)(g.unended - o.unended), per, pl, draw_ms, cpf,
             (unsigned long long)frames, g.timer_ns);
    if (g.level == 3) {   // [rt P0] the split
        char fl[900]; int k = 0; fl[0] = 0; double drv = 0, fine_sum = 0;
        for (int i = 0; i < VKN_PROF_FINE && k < (int)sizeof fl - 48; i++) {
            const double a = (double)(g.facc[i] - o.facc[i]) - (double)(g.fcnt[i] - o.fcnt[i]) * g.timer_ns;
            const double us = dsamp && a > 0 ? a / (double)dsamp / 1000.0 : 0.0;
            fine_sum += us; if (k_fine[i].drv) drv += us;
            k += snprintf(fl + k, sizeof fl - (size_t)k, "%s%s %.3f", i ? ", " : "", k_fine[i].name, us);
        }
        const double dc = dsamp ? (double)(g.dcalls - o.dcalls) / (double)dsamp : 0.0;
        vkn_logf("draw by part 3 (us a draw call, the same samples): %s; driver %.2f ms a frame (%.1f vkCmd calls a draw call, %.0f a frame), ours %.2f ms a frame (fine parts %.2f us of %.2f)",
                 fl, drv * cpf / 1000.0, dc, dc * cpf, (per - drv) * cpf / 1000.0, fine_sum, per);
        vkn_draw_prof3 p3; vkn_draw_prof3_get(&p3);
        const double nch = (double)(p3.changes - o.p3.changes);
#define P3(x) (nch > 0 ? 100.0 * (double)(p3.x - o.p3.x) / nch : 0.0)
        vkn_logf("pipeline changes: %.0f a frame (%.0f binds); field differs in %% of changes: shader %.0f, variants %.0f, vertex format %.0f, depth-stencil %.0f, raster %.0f, "
                 "blend %.0f, stencil ref %.0f, write masks %.0f, primitive %.0f, pass %.0f, effect bits %.0f, own vertex module %.0f; only dynamic-state candidates (ds / raster / stencil ref / "
                 "blend) %.0f %%, only ds / raster / stencil ref %.0f %%", nch / fr, (double)(d.pipe_binds - o.d.pipe_binds) / fr,
                 P3(f[0]), P3(f[1]), P3(f[2]), P3(f[3]), P3(f[4]), P3(f[5]), P3(f[6]), P3(f[7]), P3(f[8]), P3(f[9]), P3(f[10]), P3(f[11]), P3(dyn_only), P3(dyn_only_nb));
#undef P3
        uint64_t lk[4] = {0, 0, 0, 0}; vkn_buf_lock_stats(&lk[0], &lk[1]); vkn_tex_lock_stats(&lk[2], &lk[3]);
        vkn_logf("locks (lookups of the draw path, any thread): buffers %.0f a frame, contended %.2f a frame; textures / samplers %.0f a frame, contended %.2f a frame",
                 (double)(lk[0] - o.lk[0]) / fr, (double)(lk[1] - o.lk[1]) / fr, (double)(lk[2] - o.lk[2]) / fr, (double)(lk[3] - o.lk[3]) / fr);
        memcpy(o.lk, lk, sizeof lk); o.p3 = p3;
    }
    memcpy(o.facc, g.facc, sizeof o.facc); memcpy(o.fcnt, g.fcnt, sizeof o.fcnt); o.dcalls = g.dcalls;
    // backend measured
    const uint64_t fn = g.fc_n - o.fc_n; const double fcn = fn ? (double)fn : 1.0;
    const double rec_ms = (double)(g.fc_rec - o.fc_rec) / fcn / 1e6, beg_ms = (double)(g.fc_begin - o.fc_begin) / fcn / 1e6, end_ms = (double)(g.fc_end - o.fc_end) / fcn / 1e6;
    uint64_t gb = 0, ge = 0; const int have_ring = vkn_frame_gpu_ns(&gb, &ge) && ge >= gb;
    char gpu[160];
    if (g.gpu_samples) snprintf(gpu, sizeof gpu, "GPU %.2f ms (sampled frame %llu, %.0f s ago)", g.last_gpu_ms, (unsigned long long)g.last_gpu_frame, (now - g.last_gpu_t) / 1e9);
    else snprintf(gpu, sizeof gpu, "GPU not sampled yet%s", g.ts_tried && !g.ts_ok ? " (no timestamps)" : "");
    if (have_ring) { const size_t l = strlen(gpu); snprintf(gpu + l, sizeof gpu - l, ", newest frame %.2f ms", (ge - gb) / 1e6); }
#define PF(x) ((double)(d.x - o.d.x) / fr)
    vkn_logf("backend measured (%.1f s): frames %llu (%.1f fps); engine thread: recording %.2f ms a frame (BeginFrame %.2f, EndFrame %.2f), backend draw calls %.2f ms "
             "(sampled, %.0f %% of the recording); %s; a frame: draws %.0f (+ UI %.0f, dispatches %.0f), passes %.1f, binds pipeline %.0f / set %.0f / vb %.0f / ib %.0f, "
             "sets built %.0f, constant ring %.0f KB (UI %.0f KB); GPU samples %llu (lost %llu, unready %llu)",
             secs, (unsigned long long)frames, secs > 0 ? frames / secs : 0.0, rec_ms, beg_ms, end_ms, draw_ms, rec_ms > 0 ? 100.0 * draw_ms / rec_ms : 0.0, gpu,
             PF(draws), (double)(u.draws - o.u.draws) / fr, PF(dispatches), (double)(p.passes - o.p.passes) / fr, PF(pipe_binds), PF(set_binds), PF(vb_binds), PF(ib_binds),
             PF(set_builds), (double)(b.ring_bytes - o.ring) / fr / 1024.0, (double)(u.ring_bytes - o.u.ring_bytes) / fr / 1024.0,
             (unsigned long long)g.gpu_samples, (unsigned long long)g.gpu_lost, (unsigned long long)g.gpu_unready);
#undef PF
    memcpy(o.acc, g.acc, sizeof o.acc); memcpy(o.cnt, g.cnt, sizeof o.cnt);
    o.calls = calls; o.samples = g.samples; o.unended = g.unended; o.fc_n = g.fc_n; o.fc_begin = g.fc_begin; o.fc_rec = g.fc_rec; o.fc_end = g.fc_end;
    o.frames = f.frames; o.t = now; o.d = d; o.p = p; o.u = u; o.ring = b.ring_bytes;
}
#endif
