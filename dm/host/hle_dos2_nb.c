// [DOS2] NB front end (docs/dos2/NATIVE_BACKEND_PLAN.md, subtask NB-03: the skeleton). main = main.raw of update v786432, build 4979b200.
// Hooks on the render thread of the game:
//   ExecuteCommandList 0xa49c30 (x0 = list, w1 = flags): the engine runs one of its command lists;
//   ExecuteRenderCommand 0x93e5f0: one handler in the shared chain of hle_dos2_native.c (dn_install_rc), the original always runs;
//   BeginFrame 0x9215f0 (x0 = renderer: BeginRecording, AddCommandMemory) and EndFrame 0x93ccd0 (window vtable slot 8: EndRecording,
//   SubmitCommands, PresentTexture). Each is a 16-byte jump + a trampoline (hle_dos2_native.c dn_hook); the prologue words are checked
//   first, all four sites or nothing. None of these functions reads an FP / SIMD argument register before writing it.
//   DM_NB=2  check mode, DM_NB=1 native mode. In NB-03 both only count: lists, packets, BeginFrame per frame, the threads, the recording
//            time BeginFrame -> EndFrame; one line "[nb] frame ..." every 10 s. Later subtasks hang the frame tracker (NB-04) and the
//            planner here; they read g_dm_nb.
//   DM_NB=0 / unset: no hook (default). The hooks of DM_NATIVE_RC / DM_GFX_PASS_GRAPH / DM_NATIVE_DRAW are not touched by DM_NB.
// NB-04 (DM_NB=1 or 2, both only measure): the frame tracker. The NVN tap (dm/gfx/nvn/nvn_nb_tap.inc) builds the IR of dm/gfx/nb/nb_ir.h
//   from the NVN calls of the render thread (the thread that runs BeginFrame / ExecuteCommandList / EndFrame); each submission of the IR's
//   command buffer is planned (nb_plan, exact rules, no FRESH) and counted; the main submit inside EndFrame is final, any other is a
//   mid-frame part (ReadTexels) with the same frame number. One line "[nb] ir ..." every 10 s. NVN records are never changed.
//   DM_NB_DUMP=1: after each 10 s line the next frame (all its parts) is written as text IR + plan (nb_dump; nb_parse reads it back, '#'
//            lines are comments) to <DM_NB_DUMP_DIR | DM_RUNTIME_DIR | dm>/nb_ir_<n>_f<frame>_p<part>.txt, at most 64 files.
// NB-05 (DM_NB=1 or 2): the tap writes OP_X_NB_FRAME / _PASS / _PASS_END records into the IR's command buffer and nb_ir_plan back-patches
//   them (pass facts + plan) at each submission of the IR, before the job is queued - the final part too (inside EndFrame, at its submit;
//   the present joins the IR after the plan). DM_NB=2: the Vulkan backend compares every header with what it did (xbg_nb.inc,
//   lines "[nb] check ..."). NVN records are still never changed.
// NB-09 (DM_NB=1 or 2 + DM_NB_OPS): DM_NB_OPS = bit mask of engine opcodes taken by NB (decimal or 0x..., default 0). Bits 36 / 37 (the
//   draw functions 0x928e80 / 0x928fb0, a handler in the shared draw hook of hle_dos2_native.c): inside the draw the renderer's binds,
//   SetRenderEnable and the draw go to the shadow of the NVN tap and are written as ONE record OP_X_NB_DRAW (dm/gfx/nvn/nvn_nb_draw.inc);
//   any other NVN record inside the draw flushes the shadow as the old records first ("foreign"). The backend unpacks the record into the
//   same state calls + draw (xbg_nb.inc nbdraw, NB-09: a faithful unpack; NB-10 applies it natively). Other bits: not taken yet (logged).
//   One line "[nb] draw ..." every 10 s. Mask bits 36 / 37 clear: nothing changes (no handler, byte-identical command streams).
// NB-13a (DM_NB=1 or 2 + DM_NB_OPS state bits 6 7 8 9 10 11 18 19 21 27 48 63): inside the packet of a taken state opcode (the
//   ExecuteRenderCommand pre / post bracket below) the state records of its NVN calls (blend / color / channel mask / multisample / polygon /
//   depth-stencil states, stencil values, polygon offset, blend colour, alpha ref, vertex attrib / stream state, vertex buffers, program,
//   viewports / scissors / depth ranges) go to the state shadow of the NVN tap (dm/gfx/nvn/nvn_nb_state.inc) and are written as ONE
//   OP_X_NB_STATE before the next other record (draw, NB draw, clear, ...): per sub-state a content-deduplicated block (DEF id + records /
//   REF id / skipped if the backend has that content already). A state record outside a taken packet stays a plain record and makes its
//   sub-state unknown (risk 3). The backend executes the blocks' records in place (xbg_nb_state.inc, NB-13a: faithful; the ids feed the
//   pipeline memo of NB-13b). 27 / 48 / 63 call no state entry point (NB-01: 27 none, 48 CopyBufferToBuffer, 63 SetRenderEnable*): taken
//   but nothing to capture. One line "[nb] state ..." every 10 s. State bits clear: no bracket, byte-identical command streams.
// NB-17a / NB-17b (DM_NB=1 or 2 + DM_NB_ORDER 1 / 2 / 3, default 0): the backend executes the passes of a submission in the order the plan
//   gives. The OP_X_NB_FRAME record gets an order tail (dm/gfx/nb/nb_order.h; nvn_nb_order.inc): the execution order of the passes and the
//   address of each OP_X_NB_PASS record. 1 = the stream order (the backend runs it segment by segment: same records, same order);
//   2 = check: the tail holds the merge planner's order (nb_merge, NB-16), the backend verifies it and executes as recorded;
//   3 = that order is executed (out of the stream order only with DM_NB_RELOC keys). DM_NB_ORDER_ASSUME=1 (debug, with 2 / 3): aa merges
//   planned as if every hazard were tracked; the backend then moves only across segments that hold nothing but state records.
// NB-15 (DM_NB=1 or 2 + DM_NB_FOREIGN=1, default 0): the share of the fallback path. Every record and every draw record of every command
//   buffer is counted by caller class (nvn_nb_tap.h NBF_*: NB itself, foreign in taken draws, taken packets, opcodes off the mask, the
//   callback packets 1 / 2, the UI block main+0x1bea840..0x1bf4290, direct calls from main, unknown caller, other threads), packets by
//   opcode, and the first caller in main of the calls outside packets / in callbacks (top sites). Three lines "[nb] foreign ..." every
//   10 s. Count only: the command stream is the same either way. Decides whether opcodes 35 / 40 / 42 / 61 / 1 are worth taking.
// NB-21 (DM_NB=1 or 2 + DM_NB_OWN 1 / 2, default 0): frame ownership. Each submission of the frame IR gets a frame job (dm/gfx/nb/nb_own.h:
//   the part, the IR skeleton, every segment's PASS / KEY record address and recording in stream order, the execution order) that travels
//   to the backend with the queue job of that submit instead of being found in the stream; the backend compares it with the records it
//   executes ('[nb] own (backend' lines). 1 = the final submit of a frame (inside EndFrame with the present bit) is queued at once as the
//   frame's open job and its present + frame number are attached to it: ONE job of the translation queue (hle_nvn.c XJ_OWN) instead of
//   three; a present that comes after the submission was translated, or behind another job, is queued the old way. 2 = check: the jobs
//   are built and compared, the queue works as before and only counts what =1 would do.
//   Mid-frame parts and submits outside EndFrame stay ordinary submit jobs (with their frame job when they are the IR's). The lists the
//   engine runs outside BeginFrame..EndFrame belong to the frame whose recording they write into; the line counts them by place.
//   DM_NB_OWN never writes or changes a record. One line "[nb] own ..." every 10 s.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../gfx/nvn/nvn_nb_tap.h"
#include "../gfx/nb/nb_slotmask.h"   // NB-07
#include "../gfx/nr/nr_lean.h"       // NR-13 / NR-26: DM_NR_LEAN
nb_sm_table *xbg_nb_slotmask_table(void) __attribute__((weak));   // xbg_nb.inc nbsm (the Vulkan backend)
static int g_nb_slotmask;            // DM_NB_SLOTMASK (default 1; 0 = every bound slot is a read, the pre-NB-07 IR, for A/B)
#include "../gfx/nb/nb_post.h"   // NB-18b-4a: DM_NB_POST (hle_dos2_nb_post.c)
#include "../gfx/nb/nb_onchip.h"   // NR-ONCHIP: DM_NR_ONCHIP chain marks (docs/dos2/nr/NR-ONCHIP.md §8)
// NR-ONCHIP: the backend publishes DM_NR_ONCHIP as it resolved it (env, else the family default) at init; while it is > 0 every planned part
// gets nb_onchip_plan (Sky -> Light / PCF -> Sky -> Light marks in the PASS headers), one line "[nr] onchip plan ..." every 10 s.
static int g_nr_oc, g_nr_oc_fog;   // g_nr_oc_fog: S2F (the Fog tail, NR-FRAME.md §10) as the backend resolved it (dm_nr_onchip_publish_fog)
static nb_oc_out g_oco;
static struct { uint64_t parts, s2, s3, s2f, bad, why[NB_OCW_KINDS], sp_larger, table_differ; int ex_why; uint32_t ex[6]; } g_oc;
// The tap keeps the per-pass attachment sizes only while this is on (nvn_nb_tap_pass_sizes): the size rule uses the sizes each pass was
// bound with. nvn_nb_tap_image (last size per storage) is only the diagnostic "table_differ" now (it refused every chain on the phone).
void dm_nr_onchip_publish(int mode) { __atomic_store_n(&g_nr_oc, mode, __ATOMIC_RELAXED); nvn_nb_tap_pass_sizes(mode > 0); }
void dm_nr_onchip_publish_fog(int on) { __atomic_store_n(&g_nr_oc_fog, on, __ATOMIC_RELAXED); }   // the backend understands tail marks: plan them
static int nr_oc_size(void *ctx, uint64_t image, uint32_t *w, uint32_t *h) { (void)ctx; int fmt; return nvn_nb_tap_image(image, w, h, &fmt); }
static int nr_oc_att_size(void *ctx, int node, int att, uint32_t *w, uint32_t *h) { (void)ctx; return nvn_nb_tap_pass_size(node, att, w, h); }
static void nr_oc_plan(const nb_frame *f, nb_plan_out *po) {
    const nb_oc_rules r = { nr_oc_size, NULL, __atomic_load_n(&g_nr_oc_fog, __ATOMIC_RELAXED) > 0 ? 7u : 0u, nr_oc_att_size };   // bit 2 = S2F only when the backend asks
    g_oc.parts++;
    if (nb_onchip_plan(f, po, &r, &g_oco) < 0) { g_oc.bad++; return; }
    g_oc.s2 += (uint64_t)g_oco.s2; g_oc.s3 += (uint64_t)g_oco.s3; g_oc.s2f += (uint64_t)g_oco.s2f;
    for (int i = 0; i < NB_OCW_KINDS; i++) g_oc.why[i] += (uint64_t)g_oco.why[i];
    g_oc.sp_larger += (uint64_t)g_oco.sp_larger; g_oc.table_differ += (uint64_t)g_oco.table_differ;
    if (g_oco.ex_why && !g_oc.ex_why) { g_oc.ex_why = g_oco.ex_why; memcpy(g_oc.ex, g_oco.ex, sizeof g_oc.ex); }
}
static void nr_oc_report(double fr) {
    const int m = __atomic_load_n(&g_nr_oc, __ATOMIC_RELAXED);
    if (m <= 0 || !g_oc.parts) return;
    char b[700]; int k = 0; b[0] = 0;
    for (int i = NB_OCW_LIGHT_SHAPE; i < NB_OCW_KINDS && k < (int)sizeof b - 120; i++)
        if (g_oc.why[i] && i != NB_OCW_S2F) k += snprintf(b + k, sizeof b - (size_t)k, "%s%s %llu", k ? ", " : "", nb_oc_why_name(i), (unsigned long long)g_oc.why[i]);
    char e[160]; e[0] = 0;   // the first size refusal: the sizes each pass was bound with (0x0 = not known)
    if (g_oc.ex_why) snprintf(e, sizeof e, "; first size refusal: Sky Sp %ux%u, Sky Df %ux%u, Light Df %ux%u", g_oc.ex[0], g_oc.ex[1], g_oc.ex[2], g_oc.ex[3], g_oc.ex[4], g_oc.ex[5]);
    LOG("[nr] onchip plan DM_NR_ONCHIP=%d: %llu parts planned (bad %llu); chains marked a frame S2F %.2f (Fog tail %s), S3 %.2f, S2 %.2f (by per-pass sizes; Sp larger than Df "
        "%llu; the old last-size-per-storage table would have refused %llu of them); not marked / downgraded: %s%s",
        m, (unsigned long long)g_oc.parts, (unsigned long long)g_oc.bad, g_oc.s2f / fr, __atomic_load_n(&g_nr_oc_fog, __ATOMIC_RELAXED) > 0 ? "on" : "off", g_oc.s3 / fr, g_oc.s2 / fr, (unsigned long long)g_oc.sp_larger,
        (unsigned long long)g_oc.table_differ, k ? b : "none", e);
    memset(&g_oc, 0, sizeof g_oc);
}

enum { NB_ECL = 0xa49c30, NB_BEGIN = 0x9215f0, NB_END = 0x93ccd0 };
static const uint32_t k_nb_ecl[4] = {0xd10243ff /* sub sp,sp,#0x90 */, 0xf90023f9 /* str x25,[sp,#0x40] */, 0xa9055ff8, 0xa90657f6};
static const uint32_t k_nb_frame[4] = {0xf81c0ff7 /* str x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};   // BeginFrame and EndFrame

// hle_dos2_native.c
typedef struct dn_rc_handler { void (*pre)(uint64_t list, unsigned op, uint64_t pkt); void (*post)(uint64_t list, unsigned op, uint64_t pkt, uint64_t len); } dn_rc_handler;
module_t *dn_main(void);
int dn_words_ok(module_t *m, uint32_t off, const uint32_t pro[4]);
int dn_hook(module_t *m, uint32_t off, const uint32_t pro[4], void *hook, void **orig);
int dn_install_rc(const dn_rc_handler *h);
typedef struct dn_draw_handler { void (*pre)(uint64_t renderer, unsigned op); void (*post)(uint64_t renderer, unsigned op); } dn_draw_handler;
int dn_install_draw(const dn_draw_handler *h);

int g_dm_nb;   // DM_NB: 0 off, 1 native, 2 check (0 also when a hook site did not match)
typedef uint64_t (*nb_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static nb_fn g_ecl_orig, g_begin_orig, g_end_orig;

static uint64_t nb_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

// Lists are expected to run on one thread (NATIVE_PLAN 1.5); the counters are plain, a second thread only makes them approximate and is
// listed in the report.
enum { NB_TID_LIST = 1, NB_TID_BEGIN = 2, NB_TID_END = 4 };
static struct {
    // this frame (EndFrame to EndFrame)
    uint32_t lists, packets, begins; uint64_t t_begin, rec_ns;
    int in_frame;
    // the report window
    uint64_t t0, frames, w_lists, w_packets, w_begins, w_rec_ns, lists_out, packets_out, nested;
    uint32_t lists_max, packets_max, depth_max;
    int tid[8], ntid; uint8_t kind[8];
} g_nb;
static __thread unsigned nb_depth;   // ExecuteCommandList nesting on this thread

// ---- NB-04 frame tracker (render thread only) ----
static __thread int t_nb_rt;          // this thread runs the frame (set by the hooks below); the NVN tap appends only from it
static nb_frame g_irf; static nb_persist g_irp; static nb_plan_out g_irpo;
static uint32_t g_ir_frame = 1, g_ir_part;
static int g_ir_final_pending, g_nb_in_end, g_nb_dump, g_dump_due, g_dump_files;
static uint64_t g_ir_other_thread;    // atomic: tracked NVN calls made off the render thread
static struct {
    uint64_t parts, finals, mids, no_submit, plan_bad, nodes, passes, draws, refs, clears, ops[NB_OP_KINDS], overflow, stray, plan_ns, bar_kept, bar_split;
    uint64_t exec, empty, clear_only, folded, standalone, dead, ld_clear, ld_dc, ld_load, st_dc, st_store, transient, xread_new, prebar;
    uint64_t st_lost, lost_parts;   // store DONT_CARE kept as STORE because the part lost records (overflow / stray); parts that lost some
    uint64_t note[NBN_KINDS]; uint32_t nodes_max, plan_ns_max;
    uint64_t nb_bytes; uint32_t nb_bytes_max;   // NB-05: OP_X_NB_* record bytes in the command memory (risk 10)
} g_ir;

nb_frame *dm_nb_frame(void) {
    if (!t_nb_rt) { __atomic_fetch_add(&g_ir_other_thread, 1, __ATOMIC_RELAXED); return NULL; }
    return &g_irf;
}
void dm_nb_note(int what, unsigned n) { if (what >= 0 && what < NBN_KINDS) g_ir.note[what] += n; }

static void nb_ir_dump(const nb_frame *f, const nb_plan_out *po) {
    const char *d = getenv("DM_NB_DUMP_DIR");
    if (!d || !*d) d = getenv("DM_RUNTIME_DIR");
    if (!d || !*d) d = "dm";
    char path[1100];
    snprintf(path, sizeof path, "%s/nb_ir_%02d_f%u_p%u.txt", d, g_dump_files, f->frame_no, g_ir_part);
    FILE *o = fopen(path, "w");
    if (!o) { LOG("【红】[nb] dump: cannot write %s - dumps off", path); g_nb_dump = 0; return; }
    fprintf(o, "# DOS2 NB-04 IR dump (DM_NB=%d): frame %u part %u, %d nodes, %d refs, %d clears, overflow %u, stray %u; plan = exact rules\n",
            g_dm_nb, f->frame_no, g_ir_part, f->nnode, f->nref, f->nclear, f->overflow, f->stray);
    nb_dump(f, po, o);
    // RT images: size and NVN format as the tap saw them at SetRenderTargets (comment lines, ignored by nb_parse)
    uint64_t seen[256]; int ns = 0;
    for (int i = 0; i < f->nnode; i++) for (int a = 0; a < f->node[i].natt; a++) {
        const uint64_t im = f->node[i].att[a].image; int k = 0;
        while (k < ns && seen[k] != im) k++;
        if (k < ns || ns >= 256) continue;
        seen[ns++] = im; uint32_t w = 0, h = 0; int fmt = 0;
        if (nvn_nb_tap_image(im, &w, &h, &fmt)) fprintf(o, "# image 0x%llx %ux%u fmt=0x%x\n", (unsigned long long)im, w, h, fmt);
    }
    // NR-ONCHIP (DM_NR_ONCHIP > 0): the size of each attachment as bound when its pass opened. "# image" above is the LAST size of the
    // storage (an alias of another size later in the part overwrites it). "# passsize <node> <att index>=WxH ...", comment lines too.
    for (int i = 0; i < f->nnode; i++) {
        if (f->node[i].kind != NB_NODE_PASS || !f->node[i].natt) continue;
        char t[200]; int k = 0; uint32_t w = 0, h = 0;
        for (int a = 0; a < f->node[i].natt && k < (int)sizeof t - 32; a++)
            if (nvn_nb_tap_pass_size(i, a, &w, &h)) k += snprintf(t + k, sizeof t - (size_t)k, " %d=%ux%u", a, w, h);
        if (k) fprintf(o, "# passsize %d%s\n", i, t);
    }
    fclose(o);
    LOG("[nb] dump: %s (frame %u part %u, %d nodes)", path, f->frame_no, g_ir_part, f->nnode);
}
// A submission of the IR is complete: plan it, count it, back-patch its NB records (NB-05). Runs before the submit is queued: the
// translation thread must not see a record before its plan is in it. nb_ir_finish then dumps and resets.
static int g_ir_rc;
// ---- NB-17a / NB-17b: DM_NB_ORDER ----
static int g_nbo;           // DM_NB_ORDER 1 identity / 2 check / 3 execute the merge planner's order; 0 off
static int g_nbo_assume;    // DM_NB_ORDER_ASSUME=1 (debug): aa merges as if the hazards were tracked
static nb_merge_out g_irm;
static struct { uint64_t parts, bad, cand_aa, cand_aba, aa, exact, merged, moved, units, blocked[NB_MB_KINDS]; } g_nbom;
// ---- NB-21: DM_NB_OWN ----
static int g_nbw;           // DM_NB_OWN 1 (the present and the frame number join the final submit's queue job) / 2 (check: counted only); 0 off
static int g_nbw_n0;        // IR nodes when EndFrame was entered
static struct { uint64_t ends, ends_nopresent, lists_gap, lists_end, nodes_gap, nodes_end; } g_nbwc;
static void nb_own_report(double fr) {
    if (!g_nbw) return;
    uint64_t v[NBW_STATS]; nvn_nb_own_stats(v);
    // v[0..NBWS_N) = nvn_nb_own.inc NBWS_*: built, segments, every key, across recordings, order from the tail, not the stream order, no
    // free job, no FRAME record, a record not written, no plan, taken, stale; then nb_own.h NBW_H_*: final, open, one, late, behind, no
    // present, unarmed, no job, sync, present with no open job; then the queue jobs
    const uint64_t *h = v + NBWS_N; const double b = v[0] ? (double)v[0] : 1.;
    LOG("[nb] own (DM_NB_OWN=%d): %.2f frame jobs a frame (%.1f segments a job; with every key %llu, across recordings %llu, order from the "
        "order tail %llu, not the stream order %llu, without a plan %llu), handed to their submit %llu; not built: no FRAME record %llu, a "
        "record not written %llu, no free job %llu, not this submit's %llu; hand-off: %llu final submits in EndFrame with a job, %s %llu, "
        "one job (submit + present + frame number) %llu, present queued the old way %llu (submission already translated %llu, another job "
        "behind the submit %llu), EndFrame ended without a present %llu; old path: jobs outside EndFrame's present path %llu (EndFrame "
        "without the present bit %llu of %llu), submits there without a job %llu, DM_NVN_SYNC %llu, presents with no open job %llu; queue "
        "jobs a frame %.2f; lists outside BeginFrame..EndFrame a frame: %.2f after EndFrame returned (IR nodes before BeginFrame %.2f), "
        "%.2f inside EndFrame (IR nodes added there before its submit %.2f)",
        g_nbw, v[0] / fr, v[1] / b, (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5],
        (unsigned long long)v[9], (unsigned long long)v[10], (unsigned long long)v[7], (unsigned long long)v[8], (unsigned long long)v[6],
        (unsigned long long)v[11], (unsigned long long)h[0], g_nbw == 1 ? "queued as the open job" : "would be the open job",
        (unsigned long long)h[1], (unsigned long long)h[2], (unsigned long long)(h[3] + h[4]), (unsigned long long)h[3], (unsigned long long)h[4],
        (unsigned long long)h[5], (unsigned long long)h[6], (unsigned long long)g_nbwc.ends_nopresent, (unsigned long long)g_nbwc.ends,
        (unsigned long long)h[7], (unsigned long long)h[8], (unsigned long long)h[9], v[NBWS_N + NBWH_N] / fr,
        g_nbwc.lists_gap / fr, g_nbwc.nodes_gap / fr, g_nbwc.lists_end / fr, g_nbwc.nodes_end / fr);
    memset(&g_nbwc, 0, sizeof g_nbwc);
}
static const nb_merge_out *nb_order_plan(const nb_frame *f, int *tracked) {
    *tracked = 0;
    if (g_nbo < 2 || g_dm_nb_post) return NULL;   // 1: the stream order; DM_NB_POST elides / fuses passes by their place in the stream
    const nb_merge_rules mr = {NB_MERGE_ON, g_nbo_assume ? (unsigned)(NB_MERGEF_AA | NB_MERGEF_ASSUME_TRACKED) : 0u};
    g_nbom.parts++;
    if (nb_merge(f, &g_irpo, &mr, &g_irm)) { g_nbom.bad++; return NULL; }
    const nb_merge_stats *s = &g_irm.st;
    g_nbom.cand_aa += (uint64_t)s->cand_aa; g_nbom.cand_aba += (uint64_t)s->cand_aba; g_nbom.aa += (uint64_t)s->aa; g_nbom.exact += (uint64_t)s->exact;
    g_nbom.merged += (uint64_t)s->merged; g_nbom.moved += (uint64_t)s->moved; g_nbom.units += (uint64_t)s->units;
    for (int i = 0; i < NB_MB_KINDS; i++) g_nbom.blocked[i] += (uint64_t)s->blocked[i];
    *tracked = !g_nbo_assume && (f->track & NB_TRACK_MERGE) == NB_TRACK_MERGE;
    return &g_irm;
}
static void nb_order_report(double fr) {
    if (!g_nbo) return;
    uint64_t v[NBOS_N]; nvn_nb_order_stats(v);
    // order = nvn_nb_order.inc NBOS_*: parts, valid, merged, glue, noplan, over, lost, split, segs, cont, bytes, moved
    const double p = v[0] ? (double)v[0] : 1.;
    char b[400]; int k = 0; b[0] = 0;
    for (int i = 0; i < NB_MB_KINDS && k < (int)sizeof b - 48; i++)
        if (g_nbom.blocked[i]) k += snprintf(b + k, sizeof b - (size_t)k, "%s%s %.1f", k ? ", " : "", nb_merge_why_name(i), g_nbom.blocked[i] / fr);
    LOG("[nb] order (DM_NB_ORDER=%d%s): %.2f order tails a frame (%.0f bytes a part, %.1f passes), valid %llu of %llu (no plan %llu, more "
        "passes than reserved %llu, a record not written %llu, passes in another recording %llu); not the stream order %llu parts (%.1f "
        "passes moved back, %.1f continue the pass before them a frame); planner order not usable by segments %llu; merge planner%s a "
        "frame: candidates aa %.1f aba %.1f, merged aa %.1f exact %.1f, nodes moved %.1f, blocked: %s (failed %llu)",
        g_nbo, g_nbo_assume ? " DM_NB_ORDER_ASSUME=1" : "", v[0] / fr, v[10] / p, v[8] / p, (unsigned long long)v[1], (unsigned long long)v[0],
        (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6], (unsigned long long)v[7], (unsigned long long)v[2],
        v[11] / fr, v[9] / fr, (unsigned long long)v[3], g_nbo < 2 ? " (off at 1)" : g_dm_nb_post ? " (off: DM_NB_POST)" : "",
        g_nbom.cand_aa / fr, g_nbom.cand_aba / fr, g_nbom.aa / fr, g_nbom.exact / fr, g_nbom.moved / fr, k ? b : "-", (unsigned long long)g_nbom.bad);
    memset(&g_nbom, 0, sizeof g_nbom);
}
static void nb_ir_plan(void) {
    nb_frame *f = &g_irf;
    const uint64_t t0 = nb_now();
    const nb_rules rules = {NB_RULES_EXACT, 0, 0};
    const int rc = g_ir_rc = nb_plan(f, &g_irp, &rules, &g_irpo);
    if (!rc && g_dm_nb_post) dm_nb_post_plan(f, &g_irpo);   // NB-18b-4a: NB_NP_ELIDE / NB_NP_FUSE before the headers are patched
    if (!rc && __atomic_load_n(&g_nr_oc, __ATOMIC_RELAXED) > 0) nr_oc_plan(f, &g_irpo);   // NR-ONCHIP: chain marks before the headers are patched
    int tracked = 0;
    const nb_merge_out *mo = g_nbo && !rc ? nb_order_plan(f, &tracked) : NULL;   // NB-17b: after the post plan, before the headers are patched
    const uint32_t bytes = nvn_nb_tap_patch(f, rc ? NULL : &g_irpo, g_ir_part);
    if (g_nbo) nvn_nb_order_patch(f, mo, !rc, tracked);   // NB-17a: the order tail of this part's FRAME record
    g_ir.nb_bytes += bytes; if (bytes > g_ir.nb_bytes_max) g_ir.nb_bytes_max = bytes;
    const uint64_t dt = nb_now() - t0;
    g_ir.parts++; if (f->final) g_ir.finals++; else g_ir.mids++;
    if (rc) g_ir.plan_bad++;
    g_ir.plan_ns += dt; if (dt > g_ir.plan_ns_max) g_ir.plan_ns_max = (uint32_t)(dt > 0xffffffffu ? 0xffffffffu : dt);
    g_ir.nodes += (uint64_t)f->nnode; if ((uint32_t)f->nnode > g_ir.nodes_max) g_ir.nodes_max = (uint32_t)f->nnode;
    g_ir.refs += (uint64_t)f->nref; g_ir.clears += (uint64_t)f->nclear; g_ir.overflow += f->overflow; g_ir.stray += f->stray;
    if (f->overflow || f->stray) g_ir.lost_parts++;
    g_ir.bar_kept += f->bar_kept; g_ir.bar_split += f->bar_split;
    if (!rc) {
        const nb_plan_stats *s = &g_irpo.st;
        g_ir.exec += (uint64_t)s->exec; g_ir.empty += (uint64_t)s->empty; g_ir.clear_only += (uint64_t)s->clear_only; g_ir.folded += (uint64_t)s->folded;
        g_ir.standalone += (uint64_t)s->standalone; g_ir.dead += (uint64_t)s->dead; g_ir.ld_clear += (uint64_t)s->load_clear;
        g_ir.ld_dc += (uint64_t)s->load_dc; g_ir.ld_load += (uint64_t)s->load_load; g_ir.st_dc += (uint64_t)s->store_dc;
        g_ir.st_store += (uint64_t)s->store_store; g_ir.st_lost += (uint64_t)s->store_lost; g_ir.transient += (uint64_t)s->transient; g_ir.xread_new += (uint64_t)s->xread_new;
        g_ir.prebar += (uint64_t)s->prebar;   // NB-05c: passes needing a barrier before they begin (NB_NP_PREBAR)
    }
}
static void nb_ir_finish(void) {
    nb_frame *f = &g_irf; const int rc = g_ir_rc;
    for (int i = 0; i < f->nnode; i++) {   // here: the present of EndFrame is in the IR by now
        const nb_node *n = &f->node[i];
        if (n->kind == NB_NODE_PASS) { g_ir.passes++; g_ir.draws += n->draws; }
        else g_ir.ops[n->kind & 0xff]++;
    }
    if (g_nb_dump && g_dump_due) {
        nb_ir_dump(f, rc ? NULL : &g_irpo);
        if (f->final) { g_dump_due = 0; if (++g_dump_files >= 64) { LOG("[nb] dump: 64 files written - dumps off"); g_nb_dump = 0; } }
    }
    if (f->final) { g_ir_frame++; g_ir_part = 0; } else g_ir_part++;
    nb_frame_reset(f, g_ir_frame);
    nvn_nb_tap_reset();
}
void dm_nb_submit(void) {
    // The final part is planned here too (its records are patched before the job is queued); the present of EndFrame joins its IR after
    // the plan (dump text only: the present no longer feeds the persistent image state, NB-05 findings) and the IR resets after EndFrame.
    // NB-21: the frame job of this submission is built right after the plan (the records are patched), before the tap is reset; the
    // caller (nvnQueueSubmitCommands) takes it for the job it queues next.
    if (g_nb_in_end) {
        if (!g_ir_final_pending) {
            nb_submit(&g_irf, 1); nb_ir_plan(); g_ir_final_pending = 1;
            if (g_nbw) { if (g_irf.nnode > g_nbw_n0) g_nbwc.nodes_end += (uint64_t)(g_irf.nnode - g_nbw_n0); nvn_nb_own_build(&g_irf, !g_ir_rc, g_ir_part); }
        }
        return;
    }
    nb_submit(&g_irf, 0);
    nb_ir_plan();
    if (g_nbw) nvn_nb_own_build(&g_irf, !g_ir_rc, g_ir_part);
    nb_ir_finish();
}
static void nb_ir_report(void) {
    const double fr = g_ir.finals ? (double)g_ir.finals : 1.;
    const uint64_t other_thread = __atomic_exchange_n(&g_ir_other_thread, 0, __ATOMIC_RELAXED);
    LOG("[nb] ir: %llu frames + %llu mid-frame parts, %llu frames without an IR submit in EndFrame; a frame: %.1f nodes (max %u), %.1f passes "
        "(plan: %.1f exec, %.1f empty, %.1f clear-only), %.1f draws, %.1f reads, %.1f clears (folded %.1f, standalone %.1f, dead %.1f), "
        "ops copy %.1f resolve %.1f compute %.1f barrier %.1f readback %.1f present %.2f clear %.1f; barriers kept in passes %.1f "
        "(passes ended by an attachment read after one %.1f), passes with a barrier before them %.1f; load clear/dc/load %.1f/%.1f/%.1f, "
        "store dc/store %.1f/%.1f (kept STORE for lost records %llu in %llu parts), transient %.1f, xread new %llu; plan %.1f us (max %u us), bad %llu; overflow %llu, stray %llu; "
        "NVN a frame: draws %.0f dispatch %.1f program %.0f state %.0f vbuf %.0f ubo %.0f ssbo %.1f counter %.0f; unregistered ids %llu, "
        "other cb %llu, other submits %llu, other thread %llu; NB records %.1f KB a frame (max %.1f KB a part)",
        (unsigned long long)g_ir.finals, (unsigned long long)g_ir.mids, (unsigned long long)g_ir.no_submit, g_ir.nodes / fr, g_ir.nodes_max,
        g_ir.passes / fr, g_ir.exec / fr, g_ir.empty / fr, g_ir.clear_only / fr, g_ir.draws / fr, g_ir.refs / fr, g_ir.clears / fr,
        g_ir.folded / fr, g_ir.standalone / fr, g_ir.dead / fr, g_ir.ops[NB_OP_COPY] / fr, g_ir.ops[NB_OP_RESOLVE] / fr,
        g_ir.ops[NB_OP_COMPUTE] / fr, g_ir.ops[NB_OP_BARRIER] / fr, g_ir.ops[NB_OP_READBACK] / fr, g_ir.ops[NB_OP_PRESENT] / fr,
        g_ir.ops[NB_OP_CLEAR] / fr, g_ir.bar_kept / fr, g_ir.bar_split / fr, g_ir.prebar / fr, g_ir.ld_clear / fr, g_ir.ld_dc / fr, g_ir.ld_load / fr, g_ir.st_dc / fr, g_ir.st_store / fr,
        (unsigned long long)g_ir.st_lost, (unsigned long long)g_ir.lost_parts, g_ir.transient / fr, (unsigned long long)g_ir.xread_new, g_ir.parts ? g_ir.plan_ns / 1e3 / (double)g_ir.parts : 0., g_ir.plan_ns_max / 1000u,
        (unsigned long long)g_ir.plan_bad, (unsigned long long)g_ir.overflow, (unsigned long long)g_ir.stray,
        g_ir.note[NBN_DRAW] / fr, g_ir.note[NBN_DISPATCH] / fr, g_ir.note[NBN_PROGRAM] / fr, g_ir.note[NBN_STATE] / fr, g_ir.note[NBN_VBUF] / fr,
        g_ir.note[NBN_UBO] / fr, g_ir.note[NBN_SSBO] / fr, g_ir.note[NBN_COUNTER] / fr, (unsigned long long)g_ir.note[NBN_UNREG],
        (unsigned long long)g_ir.note[NBN_OTHER_CB], (unsigned long long)g_ir.note[NBN_OTHER_SUBMIT], (unsigned long long)other_thread,
        g_ir.nb_bytes / fr / 1024., g_ir.nb_bytes_max / 1024.);
    if (g_irf.rs_mode) {   // [NR-CPU] NR-TAPSET
        LOG("[nr] tapset (DM_NR_TAPSET=%d, last report): %.1f reads a frame through the set, %.1f of them already held; passes over %d refs (scan) %llu%s; set vs scan differ %llu%s",
            g_irf.rs_mode, (double)g_irf.rs_lookups / fr, (double)g_irf.rs_hits / fr, NB_RS_MAX, (unsigned long long)g_irf.rs_big, g_irf.rs_mode == 2 ? " (=2: the scan decided)" : "",
            (unsigned long long)g_irf.rs_bad, "");
        if (g_irf.rs_bad) LOG("【红】[nr] tapset: the set and the scan of the pass's refs answered differently %llu times", (unsigned long long)g_irf.rs_bad);
        g_irf.rs_lookups = g_irf.rs_hits = g_irf.rs_big = g_irf.rs_bad = 0;
    }
    {   // NB-07: reads by the backend's slot masks (DM_NB_SLOTMASK); table = programs the backend published (xbg_nb.inc nbsm)
        const nb_sm_table *t = xbg_nb_slotmask_table ? xbg_nb_slotmask_table() : NULL;
        const uint64_t k = g_ir.note[NBN_SM_KNOWN], u = g_ir.note[NBN_SM_UNKNOWN];
        LOG("[nb] slotmask (DM_NB_SLOTMASK=%d): draws + dispatches a frame with masks %.1f, unknown program %.1f (%.1f%%), bound slots not "
            "read %.1f a frame; backend table %u programs, %u dropped (full)", g_nb_slotmask, k / fr, u / fr, k + u ? 100. * (double)u / (double)(k + u) : 0.,
            g_ir.note[NBN_SM_SKIP] / fr, t ? __atomic_load_n(&t->n, __ATOMIC_RELAXED) : 0u, t ? __atomic_load_n(&t->full, __ATOMIC_RELAXED) : 0u);
    }
    dm_nb_post_report(fr);   // NB-18b-4a
    nr_oc_report(fr);        // NR-ONCHIP
    memset(&g_ir, 0, sizeof g_ir);
    if (g_nb_dump) g_dump_due = 1;
}

// ---- NB-09: DM_NB_OPS, the draw bracket ----
void nvn_nb_draw_check(uint64_t mask);   // NB-10: DM_NB_OPS_CHECK opcodes (dm/gfx/nvn/nvn_nb_draw.inc)
static uint64_t g_nb_ops;   // DM_NB_OPS as taken (only bits 36 / 37 so far)
// ---- NB-13a: the state bits ----
#define NB_STATE_OPS (1ull << 6 | 1ull << 7 | 1ull << 8 | 1ull << 9 | 1ull << 10 | 1ull << 11 | 1ull << 18 | 1ull << 19 | 1ull << 21 | 1ull << 27 |                       1ull << 48 | 1ull << 63)
static uint64_t g_nbs_ops;  // DM_NB_OPS & NB_STATE_OPS as taken
// ---- NB-15: DM_NB_FOREIGN ----
enum { NB_UI_LO = 0x1bea840, NB_UI_HI = 0x1bf4290 };   // the UI / Iggy-like block of main (NATIVE_PLAN 1.2: its own draw / state wrappers)
static int g_nbf;           // DM_NB_FOREIGN=1
static nbf_stats g_nbf_v;   // the last window (report only)
static void nb_foreign_report(double fr) {
    if (!g_nbf) return;
    nbf_stats *v = &g_nbf_v; nvn_nb_foreign_stats(v);
    uint64_t r = 0, d = 0, by = 0;
    for (int k = 0; k < NBF_N; k++) { r += v->rec[k]; d += v->drw[k]; by += v->bytes[k]; }
    const uint64_t fr_r = r - v->rec[NBF_NB], fr_d = d - v->drw[NBF_NB];
#define NBF_P(k) v->rec[k] / fr, v->drw[k] / fr
    LOG("[nb] foreign (DM_NB_FOREIGN=1, DM_NB_OPS taken %#llx): a frame %.0f records (%.1f KB), %.1f draws; NB path %.1f records (%.1f KB), "
        "%.1f draws (%.1f%% of draws); fallback (NVN records) %.1f records (%.1f%%), %.1f draws (%.1f%%) - records/draws a frame by caller class: "
        "foreign in taken draws %.1f/%.1f, taken packets not captured %.1f/%.1f, opcodes off the mask %.1f/%.1f, callbacks 1/2 %.1f/%.1f, "
        "UI block %.1f/%.1f, direct from main %.1f/%.1f, caller unknown %.1f/%.1f (another module %llu, walk stopped %llu), other threads %.1f/%.1f",
        (unsigned long long)(g_nb_ops | g_nbs_ops), r / fr, by / fr / 1024., d / fr, v->rec[NBF_NB] / fr, v->bytes[NBF_NB] / fr / 1024., v->drw[NBF_NB] / fr,
        d ? 100. * (double)v->drw[NBF_NB] / (double)d : 0., fr_r / fr, r ? 100. * (double)fr_r / (double)r : 0., fr_d / fr, d ? 100. * (double)fr_d / (double)d : 0.,
        NBF_P(NBF_DRAW_FOREIGN), NBF_P(NBF_TAKEN_PKT), NBF_P(NBF_OFF_MASK), NBF_P(NBF_CALLBACK), NBF_P(NBF_UI), NBF_P(NBF_DIRECT), NBF_P(NBF_OTHER),
        (unsigned long long)v->other_module, (unsigned long long)v->walk_failed, NBF_P(NBF_THREAD));
#undef NBF_P
    // opcodes: the NB-15 candidates always, then the 12 with the most records (* = taken)
    char b[1400]; int n = 0; uint8_t used[NBF_OPS] = {0};
    static const uint8_t cand[] = {35, 40, 42, 61, 1};
    for (unsigned i = 0; i < sizeof cand; i++) {
        const int o = cand[i]; used[o] = 1;
        n += snprintf(b + n, sizeof b - (size_t)n, "%s%d%s %.1f/%.1f", i ? " " : "", o, ((g_nb_ops | g_nbs_ops) >> o & 1) ? "*" : "", v->op_rec[o] / fr, v->op_drw[o] / fr);
    }
    n += snprintf(b + n, sizeof b - (size_t)n, "; top:");
    for (int t = 0; t < 12 && n < (int)sizeof b - 48; t++) {
        int best = -1;
        for (int o = 0; o < NBF_OPS; o++) if (!used[o] && v->op_rec[o] && (best < 0 || v->op_rec[o] > v->op_rec[best])) best = o;
        if (best < 0) break;
        used[best] = 1;
        n += snprintf(b + n, sizeof b - (size_t)n, " %d%s %.1f/%.1f (%.1f KB)", best, ((g_nb_ops | g_nbs_ops) >> best & 1) ? "*" : "", v->op_rec[best] / fr,
                      v->op_drw[best] / fr, v->op_bytes[best] / fr / 1024.);
    }
    LOG("[nb] foreign opcodes (records/draws a frame inside packets of the opcode, * = taken): candidates %s", b);
    // sites: the first caller in main of calls outside packets / in callbacks, the 10 with the most records
    n = 0; b[0] = 0; uint8_t su[NBF_SITES] = {0}; unsigned nsite = 0;
    for (int s = 0; s < NBF_SITES; s++) nsite += v->site[s].used;
    for (int t = 0; t < 10 && n < (int)sizeof b - 64; t++) {
        int best = -1;
        for (int s = 0; s < NBF_SITES; s++) if (v->site[s].used && !su[s] && (best < 0 || v->site[s].rec > v->site[best].rec)) best = s;
        if (best < 0) break;
        su[best] = 1; const nbf_site *e = &v->site[best];
        const int ui = e->off >= NB_UI_LO && e->off < NB_UI_HI;
        char tag[16] = ""; if (e->op != NBF_OP_NONE) snprintf(tag, sizeof tag, " op%u", e->op);
        n += snprintf(b + n, sizeof b - (size_t)n, "%smain+%#x%s%s %.1f/%.1f", t ? ", " : "", e->off, ui ? " ui" : "", tag, e->rec / fr, e->drw / fr);
    }
    LOG("[nb] foreign sites (first caller in main outside packets / in callbacks 1/2, records/draws a frame): %u sites%s; %s",
        nsite, v->site_full ? " (table full: some dropped)" : "", n ? b : "none");
}
// ---- NR-13 / NR-26: DM_NR_LEAN, the front part (NRL_STATE; rules in dm/gfx/nr/nr_lean.h, docs/dos2/nr/NR-13.md) ----
// 1 + NRL_STATE eligible (no consumer of the NB-13a blocks: DM_NB_RELOC / DM_NB_ORDER off): the state bits of DM_NB_OPS are not taken -
// nvn_nb_state_enable is never called, the state packets write plain NVN state records (the NB-09 / NB-10 stream). With DM_NR_LEAN_AB=N
// the capture is enabled and paused / resumed at EndFrame in the ABBA phases (nvn_nb_state_pause), and the recording time
// BeginFrame -> EndFrame is kept per phase. 2: everything as at 0, the line says what 1 would not capture. The backend part (NRL_UBO)
// follows the phase through xbg_nr_lean_phase.
static nr_lean_cfg g_nrl;
static int g_nrl_state, g_nrl_state_why;   // NRL_STATE armed (mode 1 and eligible) / nr_lean_state_why
static int g_nrl_on;                        // lean is on in the frame being recorded (mode 1: always, or the A/B phase)
static uint64_t g_nrl_frame;
static struct { uint64_t frames[2], rec_ns[2], toggles, refused, cap_calls, cap_bytes, out_bytes; } g_nrlw;
void xbg_nr_lean_phase(int on) __attribute__((weak));   // dm/gfx/vulkan/engine/xbg_nr_lean.inc
int nvn_nb_state_pause(int off);                        // dm/gfx/nvn/nvn_nb_state.inc
// EndFrame (render thread): this frame's recording time goes to its phase; the next frame's phase is set.
static int g_nrl_want;
static void nrl_phase(void) {   // EndFrame, and BeginFrame when EndFrame could not (render thread)
    if (g_nrl_want == g_nrl_on) return;
    if (g_nrl_state && !nvn_nb_state_pause(g_nrl_want)) { g_nrlw.refused++; return; }   // a packet is open / another thread owns the shadow: next hook
    g_nrl_on = g_nrl_want; g_nrlw.toggles++;
    if (xbg_nr_lean_phase) xbg_nr_lean_phase(g_nrl_on);
}
static void nrl_frame(uint64_t rec_ns) {
    g_nrlw.frames[g_nrl_on]++; g_nrlw.rec_ns[g_nrl_on] += rec_ns;
    if (!g_nrl.ab) return;
    g_nrl_want = nr_lean_ab_on(++g_nrl_frame, g_nrl.ab);
    nrl_phase();
}
static void nrl_report(double fr) {
    if (!g_nrl.mode) return;
    const double f0 = g_nrlw.frames[0] ? (double)g_nrlw.frames[0] : 1., f1 = g_nrlw.frames[1] ? (double)g_nrlw.frames[1] : 1.;
    char ab[320]; ab[0] = 0;
    if (g_nrl.ab) snprintf(ab, sizeof ab, "; A/B (DM_NR_LEAN_AB=%u, off N / on 2N / off N): recording BeginFrame -> EndFrame %.3f ms a frame lean "
                           "(%llu frames) vs %.3f ms full (%llu frames), %llu phase changes (%llu put off: a packet was open)", g_nrl.ab,
                           g_nrlw.rec_ns[1] / f1 / 1e6, (unsigned long long)g_nrlw.frames[1], g_nrlw.rec_ns[0] / f0 / 1e6, (unsigned long long)g_nrlw.frames[0],
                           (unsigned long long)g_nrlw.toggles, (unsigned long long)g_nrlw.refused);
    else { const uint64_t n = g_nrlw.frames[0] + g_nrlw.frames[1];
        snprintf(ab, sizeof ab, "; recording BeginFrame -> EndFrame %.3f ms a frame", n ? (double)(g_nrlw.rec_ns[0] + g_nrlw.rec_ns[1]) / (double)n / 1e6 : 0.); }
    LOG("[nr] lean (front, DM_NR_LEAN=%d parts %#x): state capture NB-13a %s (%s); %s %.1f state calls a frame (%.0f bytes as NVN records -> %.0f bytes "
        "of OP_X_NB_STATE; as plain records: %+.0f bytes of command memory a frame and no shadow / dictionary work on this thread)%s",
        g_nrl.mode, g_nrl.parts, g_nrl_state ? (g_nrl.ab ? "paused in the lean phases" : "not taken: plain NVN state records") : g_nrl.mode == 2 ? "taken as before" : "taken (part off or not eligible)",
        nr_lean_state_name(g_nrl_state_why), g_nrl_state && g_nrl.ab ? "captured in the full phases:" : g_nrl.mode == 2 ? "1 would not capture" : "captured",
        g_nrlw.cap_calls / fr, g_nrlw.cap_bytes / fr, g_nrlw.out_bytes / fr, ((double)g_nrlw.cap_bytes - (double)g_nrlw.out_bytes) / fr, ab);
    memset(g_nrlw.frames, 0, sizeof g_nrlw.frames); memset(g_nrlw.rec_ns, 0, sizeof g_nrlw.rec_ns);
    g_nrlw.toggles = g_nrlw.refused = g_nrlw.cap_calls = g_nrlw.cap_bytes = g_nrlw.out_bytes = 0;
}
static void nb_state_report(double fr) {
    if (!g_nbs_ops) return;
    uint64_t v[NBSS_N]; nvn_nb_state_stats(v);
    g_nrlw.cap_calls += v[3]; g_nrlw.cap_bytes += v[4]; g_nrlw.out_bytes += v[6];   // NR-13: for the "[nr] lean" line
    // order = nvn_nb_state.inc NBSS_*: brackets, taken, other thread, captured, captured bytes, records, bytes, def, ref, noid, same, raw,
    // xraw, unknown, fallback, epochs
    LOG("[nb] state (DM_NB_OPS state bits %#llx): %.1f taken packets a frame (%llu on another thread, not taken); %.1f state calls captured "
        "a frame (%.0f bytes as NVN records) -> %.1f OP_X_NB_STATE a frame (%.0f bytes); blocks a frame: %.1f new (DEF) %.1f repeated (REF) "
        "%.2f without id, %.1f unchanged (skipped); plain state records outside taken packets %.1f a frame (+%llu on other threads), "
        "sub-states made unknown %.1f a frame; %llu captured calls written back as plain records; %.1f epochs a frame",
        (unsigned long long)g_nbs_ops, v[1] / fr, (unsigned long long)v[2], v[3] / fr, v[4] / fr, v[5] / fr, v[6] / fr, v[7] / fr, v[8] / fr,
        v[9] / fr, v[10] / fr, v[11] / fr, (unsigned long long)v[12], v[13] / fr, (unsigned long long)v[14], v[15] / fr);
}
// ---- NB-14: DM_NB_RELOC, the key frame of every IR pass ----
static int g_nbk;           // DM_NB_RELOC 1 (the backend sets its state from the key at every pass) / 2 (compares only); 0 off
static void nb_key_report(double fr) {
    if (!g_nbk) return;
    uint64_t v[NBKS_N]; nvn_nb_key_stats(v);
    // order = nvn_nb_key.inc NBKS_*: keys, bytes, units, def, ref, walked, nested, blocks, nbdraw, noanchor, lost end / bad / guard, partial,
    // unresolved, over, bad record, between, no record, timed, ns, frames
    const double k = v[0] ? (double)v[0] : 1.;
    LOG("[nb] key (DM_NB_RELOC=%d): %.1f OP_X_NB_KEY a frame (%.0f bytes a frame, %.0f a key; %.1f FRAME anchors a frame); units a key %.1f "
        "(%.1f new DEF, %.1f REF); records walked a frame %.0f (nested handles / native draws %.1f, OP_X_NB_STATE blocks %.1f, OP_X_NB_DRAW %.1f); "
        "%.1f us a pass (walk + pack + write, 1 in 16 timed); void keys: no FRAME anchor %llu, recording ended before the pass %llu, bad "
        "record %llu, guard %llu; keys with lost units %llu (list full %llu, OP_X_NB_STATE REF unresolved %llu, state records not keyed %llu); "
        "records between PASS and KEY %llu, KEY not written %llu",
        g_nbk, v[0] / fr, v[1] / fr, v[1] / k, v[21] / fr, v[2] / k, v[3] / k, v[4] / k, v[5] / fr, v[6] / fr, v[7] / fr, v[8] / fr,
        v[19] ? v[20] / 1e3 / (double)v[19] : 0., (unsigned long long)v[9], (unsigned long long)v[10], (unsigned long long)v[11],
        (unsigned long long)v[12], (unsigned long long)v[13], (unsigned long long)v[15], (unsigned long long)v[14], (unsigned long long)v[16],
        (unsigned long long)v[17], (unsigned long long)v[18]);
}
static void nb_draw_pre(uint64_t r, unsigned op) { (void)r; if (op < 64 && (g_nb_ops >> op & 1)) nvn_nb_draw_begin(op); }
static void nb_draw_post(uint64_t r, unsigned op) { (void)r; if (op < 64 && (g_nb_ops >> op & 1)) nvn_nb_draw_end(); }
static const dn_draw_handler k_nb_draw = {nb_draw_pre, nb_draw_post};
static void nb_draw_report(double s, double fr) {
    if (!g_nb_ops) return;
    uint64_t v[NBDS_N]; nvn_nb_draw_stats(v);
    // order = nvn_nb_draw.inc NBDS_*: brackets, draws, binds, bytes, foreign, foreign flush, flush binds, orphan, full, cb, nodraw
    LOG("[nb] draw (DM_NB_OPS=%#llx, last %.1f s): %.1f engine draws a frame, %.1f OP_X_NB_DRAW a frame (%.1f binds, %.0f bytes a record); "
        "foreign records in the draws %.1f a frame (%.2f flushed the shadow); binds written back as NVN records %.1f a frame "
        "(foreign %llu, full %llu, other command buffer %llu, after the draw %llu flushes); draws without a modelled draw call %llu",
        (unsigned long long)g_nb_ops, s, v[0] / fr, v[1] / fr, v[1] ? (double)v[2] / v[1] : 0., v[1] ? (double)v[3] / v[1] : 0., v[4] / fr, v[5] / fr,
        v[6] / fr, (unsigned long long)v[5], (unsigned long long)v[8], (unsigned long long)v[9], (unsigned long long)v[7], (unsigned long long)v[10]);
}

static void nb_tid(int kind) {
    static __thread uint8_t seen;
    if (seen & kind) return;
    seen |= (uint8_t)kind;
    const int me = (int)gettid(), n = __atomic_load_n(&g_nb.ntid, __ATOMIC_RELAXED);
    for (int i = 0; i < n && i < 8; i++) if (g_nb.tid[i] == me) { __atomic_fetch_or(&g_nb.kind[i], (uint8_t)kind, __ATOMIC_RELAXED); return; }
    if (n < 8) { g_nb.tid[n] = me; g_nb.kind[n] = (uint8_t)kind; __atomic_store_n(&g_nb.ntid, n + 1, __ATOMIC_RELAXED); }
}

static void nb_report(uint64_t now) {
    const double s = (now - g_nb.t0) / 1e9, f = g_nb.frames ? (double)g_nb.frames : 1.;
    char tids[160]; int k = 0; tids[0] = 0;
    for (int i = 0; i < g_nb.ntid && k < (int)sizeof tids - 24; i++)
        k += snprintf(tids + k, sizeof tids - (size_t)k, "%s%d:%s%s%s", i ? " " : "", g_nb.tid[i], g_nb.kind[i] & NB_TID_LIST ? "L" : "",
                      g_nb.kind[i] & NB_TID_BEGIN ? "B" : "", g_nb.kind[i] & NB_TID_END ? "E" : "");
    LOG("[nb] frame (DM_NB=%d, last %.1f s): %llu frames (%.1f fps); a frame: %.1f lists (max %u), %.0f packets (max %u), %.2f BeginFrame, "
        "recording %.2f ms (BeginFrame -> EndFrame); %llu lists outside BeginFrame..EndFrame, %llu packets outside a list, %llu nested lists "
        "(depth max %u); threads [%s] (L lists, B BeginFrame, E EndFrame)",
        g_dm_nb, s, (unsigned long long)g_nb.frames, s > 0 ? g_nb.frames / s : 0., g_nb.w_lists / f, g_nb.lists_max, g_nb.w_packets / f,
        g_nb.packets_max, g_nb.w_begins / f, g_nb.w_rec_ns / f / 1e6, (unsigned long long)g_nb.lists_out, (unsigned long long)g_nb.packets_out,
        (unsigned long long)g_nb.nested, g_nb.depth_max, tids);
    g_nb.frames = g_nb.w_lists = g_nb.w_packets = g_nb.w_begins = g_nb.w_rec_ns = g_nb.lists_out = g_nb.packets_out = g_nb.nested = 0;
    g_nb.lists_max = g_nb.packets_max = g_nb.depth_max = 0;
    g_nb.t0 = now;
    nb_draw_report(s, f);   // NB-09 (before nb_ir_report resets nothing of it: own counters)
    nb_state_report(f);     // NB-13a
    nrl_report(f);          // NR-13 DM_NR_LEAN (after the state line: it takes its counters)
    nb_foreign_report(f);   // NB-15
    nb_key_report(f);       // NB-14
    nb_order_report(f);     // NB-17a
    nb_own_report(f);       // NB-21
    nb_ir_report();
}

static void nb_rc_pre(uint64_t list, unsigned op, uint64_t pkt) {
    (void)list; (void)op; (void)pkt;
    g_nb.packets++;
    if (!nb_depth) g_nb.packets_out++;
    if (g_nbs_ops) nvn_nb_state_begin(op < 64 && (g_nbs_ops >> op & 1));   // NB-13a: every packet nests (a non-taken one inside a taken one is not taken)
    if (g_nbf) nvn_nb_foreign_pkt((int)op, op < 64 && ((g_nb_ops | g_nbs_ops) >> op & 1));   // NB-15
}
static void nb_rc_post(uint64_t list, unsigned op, uint64_t pkt, uint64_t len) {
    (void)list; (void)op; (void)pkt; (void)len;
    if (g_nbs_ops) nvn_nb_state_end();
    if (g_nbf) nvn_nb_foreign_pkt(-1, 0);   // NB-15
}
static const dn_rc_handler k_nb_rc = {nb_rc_pre, nb_rc_post};

static uint64_t nb_ecl_hook(uint64_t list, uint64_t flags, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    nb_tid(NB_TID_LIST); t_nb_rt = 1;
    g_nb.lists++;
    if (!g_nb.in_frame) { g_nb.lists_out++; if (g_nbw) { if (g_nb_in_end) g_nbwc.lists_end++; else g_nbwc.lists_gap++; } }   // NB-21: by place
    if (nb_depth) g_nb.nested++;
    if (++nb_depth > g_nb.depth_max) g_nb.depth_max = nb_depth;
    uint64_t v = g_ecl_orig(list, flags, a2, a3, a4, a5);
    nb_depth--;
    return v;
}
static uint64_t nb_begin_hook(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    nb_tid(NB_TID_BEGIN); t_nb_rt = 1;
    if (g_nrl.ab && !g_nb.begins) nrl_phase();   // NR-13 DM_NR_LEAN_AB: a phase change EndFrame had to put off (before the time below)
    if (!g_nb.begins) g_nb.t_begin = nb_now();   // the recording time counts from the first BeginFrame of the frame
    if (g_nbw && !g_nb.begins) g_nbwc.nodes_gap += (uint64_t)g_irf.nnode;   // NB-21: IR nodes recorded since the last EndFrame returned
    g_nb.begins++; g_nb.in_frame = 1;
    return g_begin_orig(r, a1, a2, a3, a4, a5);
}
static uint64_t nb_end_hook(uint64_t w, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    nb_tid(NB_TID_END); t_nb_rt = 1;
    const uint64_t now = nb_now();   // the frame closes before the original records its submit / present
    g_nb.frames++;
    g_nb.w_lists += g_nb.lists; g_nb.w_packets += g_nb.packets; g_nb.w_begins += g_nb.begins;
    if (g_nb.begins && now > g_nb.t_begin) g_nb.w_rec_ns += now - g_nb.t_begin;
    if (g_nrl.mode && g_nb.begins && now > g_nb.t_begin) nrl_frame(now - g_nb.t_begin);   // NR-13 DM_NR_LEAN: per-phase recording time, next phase
    if (g_nb.lists > g_nb.lists_max) g_nb.lists_max = g_nb.lists;
    if (g_nb.packets > g_nb.packets_max) g_nb.packets_max = g_nb.packets;
    g_nb.lists = g_nb.packets = g_nb.begins = 0; g_nb.in_frame = 0;
    g_nb_in_end = 1;
    if (g_nbw) {   // NB-21: w1 bit 0 = the present path (main+0x93cd14 / +0x93ce24); without it EndFrame submits and waits (FenceSync + Flush + SyncWait)
        g_nbw_n0 = g_irf.nnode; g_nbwc.ends++;
        if (!(a1 & 1)) g_nbwc.ends_nopresent++;
        nvn_nb_own_arm((int)(a1 & 1));
    }
    const uint64_t v = g_end_orig(w, a1, a2, a3, a4, a5);   // EndRecording, SubmitCommands (-> dm_nb_submit, final), PresentTexture
    g_nb_in_end = 0;
    if (g_nbw) nvn_nb_own_end();   // NB-21: an open job gets no present from another EndFrame
    if (!g_ir_final_pending && g_irf.nnode) { g_ir.no_submit++; nb_submit(&g_irf, 1); nb_ir_plan(); g_ir_final_pending = 1; }
    if (g_ir_final_pending) { g_ir_final_pending = 0; nb_ir_finish(); }
    if (!g_nb.t0) g_nb.t0 = now;
    else if (now - g_nb.t0 >= 10000000000ull) nb_report(now);
    return v;
}

void dm_dos2_nb_install(void);   // main.c host_preload, after dm_dos2_native_install (before any guest code runs)
void dm_dos2_nb_install(void) {
    const char *e = getenv("DM_NB");
    const int mode = e ? atoi(e) : 0;
    if (mode != 1 && mode != 2) return;
    module_t *m = dn_main();
    if (!m) { LOG("【红】[nb] no main module: DM_NB off"); return; }
    if (!dn_words_ok(m, NB_ECL, k_nb_ecl) || !dn_words_ok(m, NB_BEGIN, k_nb_frame) || !dn_words_ok(m, NB_END, k_nb_frame)) {
        LOG("【红】[nb] main+%#x / +%#x / +%#x differ: not DOS2 update v786432 main (build 4979b200): DM_NB off", NB_ECL, NB_BEGIN, NB_END); return; }
    if (dn_install_rc(&k_nb_rc)) { LOG("【红】[nb] no ExecuteRenderCommand handler (see above): DM_NB off"); return; }   // checks 0x93e5f0 itself
    void *o = NULL;
    if (dn_hook(m, NB_ECL, k_nb_ecl, (void *)nb_ecl_hook, &o)) { LOG("【红】[nb] ExecuteCommandList: mmap / mprotect failed: DM_NB counts packets only"); return; }
    g_ecl_orig = (nb_fn)o;
    if (dn_hook(m, NB_BEGIN, k_nb_frame, (void *)nb_begin_hook, &o)) { LOG("【红】[nb] BeginFrame: mmap / mprotect failed: no frame line"); return; }
    g_begin_orig = (nb_fn)o;
    if (dn_hook(m, NB_END, k_nb_frame, (void *)nb_end_hook, &o)) { LOG("【红】[nb] EndFrame: mmap / mprotect failed: no frame line"); return; }
    g_end_orig = (nb_fn)o;
    nb_frame_reset(&g_irf, g_ir_frame); nb_persist_reset(&g_irp);
    { const char *d = getenv("DM_NR_TAPSET"); const int v = d && *d ? atoi(d) : 0; g_irf.rs_mode = (uint8_t)(v == 1 || v == 2 ? v : 0);   // [NR-CPU] NR-TAPSET
      if (v) LOG("[nr] DM_NR_TAPSET=%d (NR-CPU): %s", g_irf.rs_mode, g_irf.rs_mode == 1 ? "the IR's per-pass read test through a hash set of the pass's refs (no scan)" : g_irf.rs_mode == 2 ? "the set and the scan both, the scan decides, differences counted" : "only 0 / 1 / 2: off"); }
    { const char *d = getenv("DM_NB_DUMP"); g_nb_dump = d && atoi(d) == 1; }
    { const char *d = getenv("DM_NB_SLOTMASK"); g_nb_slotmask = !(d && *d && atoi(d) == 0); nvn_nb_tap_slotmask(g_nb_slotmask); }   // NB-07
    dm_nb_post_install();   // NB-18b-4a: DM_NB_POST (before the tap is live)
    g_dm_nb = mode;   // the NVN tap is live from here
    {   // NB-09: DM_NB_OPS; NB-10: DM_NB_OPS_CHECK (same bits: both the old records and a check-only OP_X_NB_DRAW, nvn_nb_draw.inc); NB-13a: state bits
        const char *o = getenv("DM_NB_OPS"), *oc = getenv("DM_NB_OPS_CHECK");
        const uint64_t chk = (oc && *oc ? strtoull(oc, NULL, 0) : 0) & (1ull << 36 | 1ull << 37);
        const uint64_t want = (o && *o ? strtoull(o, NULL, 0) : 0) | chk, draw = want & (1ull << 36 | 1ull << 37), st = want & NB_STATE_OPS;
        if (want & ~draw & ~st) LOG("[nb] DM_NB_OPS=%#llx: opcodes %#llx are not taken by NB yet (36 / 37 NB-09, state bits %#llx NB-13a): left "
                                    "on the NVN path", (unsigned long long)want, (unsigned long long)(want & ~draw & ~st), (unsigned long long)NB_STATE_OPS);
        {   // NR-13 / NR-26 DM_NR_LEAN: NRL_STATE (before the state bits are taken; the backend reads the same switches)
            const char *r = getenv("DM_NB_RELOC"), *od = getenv("DM_NB_ORDER");
            const unsigned bad = nr_lean_parse(getenv("DM_NR_LEAN"), getenv("DM_NR_LEAN_PARTS"), getenv("DM_NR_LEAN_AB"), &g_nrl);
            if (bad) LOG("【红】[nr] DM_NR_LEAN%s%s%s: out of range (DM_NR_LEAN 0 / 1 / 2, DM_NR_LEAN_PARTS a mask of %#x, DM_NR_LEAN_AB 0..100000) - that one is ignored",
                         bad & NRL_BAD_MODE ? " (mode)" : "", bad & NRL_BAD_PARTS ? " _PARTS" : "", bad & NRL_BAD_AB ? " _AB" : "", NRL_ALL);
            g_nrl_state_why = nr_lean_state_why(st != 0, r && atoi(r) != 0, od && atoi(od) != 0);
            g_nrl_state = g_nrl.mode == 1 && (g_nrl.parts & NRL_STATE) && g_nrl_state_why == NRL_ST_OK;
            g_nrl_on = g_nrl.mode == 1 && !g_nrl.ab;   // the A/B starts in an "off" phase (nr_lean_ab_on(0, n) == 0)
            if (g_nrl.mode) LOG("[nr] DM_NR_LEAN=%d parts %#x (NR-13 / NR-26): state capture NB-13a %s (%s)%s", g_nrl.mode, g_nrl.parts,
                g_nrl_state ? (g_nrl.ab ? "paused in the lean phases of the A/B" : "NOT taken - the state packets write plain NVN state records, every draw sees the state they set in call order") :
                g_nrl.mode == 2 ? "taken as before, counted" : "taken as before", nr_lean_state_name(g_nrl_state_why),
                g_nrl.ab ? "; DM_NR_LEAN_AB: lean alternates off N / on 2N / off N frames, the lines '[nr] lean' give both phases" : "");
        }
        if (st && g_nrl_state && !g_nrl.ab) {
            LOG("[nb] DM_NB_OPS state bits %#llx: not taken (DM_NR_LEAN=1, NR-13): the state calls of these engine opcodes stay plain NVN state "
                "records; nothing reads the NB-13a block ids (DM_NB_RELOC / DM_NB_ORDER are off)", (unsigned long long)st);
        } else if (st) {   // NB-13a: the ExecuteRenderCommand handler above brackets every packet from here on
            nvn_nb_state_enable(); g_nbs_ops = st;
            LOG("[nb] DM_NB_OPS state bits %#llx: the state calls of these engine opcodes go to the state shadow and are written as deduplicated "
                "OP_X_NB_STATE blocks before the next other record; plain state records elsewhere mark their sub-state unknown (NB-13a); "
                "the backend executes the blocks' records as before", (unsigned long long)st);
        }
        if (draw) {
            if (dn_install_draw(&k_nb_draw)) LOG("【红】[nb] DM_NB_OPS: no draw hook (see above): draws stay NVN records");
            else { g_nb_ops = draw; nvn_nb_draw_check(chk); nvn_nb_draw_enable();
                if (chk) LOG("[nb] DM_NB_OPS_CHECK=%#llx: those draws write the old NVN records AND a check-only OP_X_NB_DRAW; the backend "
                             "executes the old ones and compares (lines '[nb] draw check', NB-10)", (unsigned long long)chk);
                const uint64_t nat = draw & ~chk;
                if (nat) LOG("[nb] DM_NB_OPS=%#llx: engine draws%s%s written as one OP_X_NB_DRAW each (binds + draw; other NVN calls inside flush them "
                    "as NVN records first); the backend applies them to its state and draws (NB-10)", (unsigned long long)nat,
                    nat >> 36 & 1 ? " 36 (main+0x928e80)" : "", nat >> 37 & 1 ? " 37 (main+0x928fb0)" : ""); }
        }
    }
    {   // NB-15: DM_NB_FOREIGN (after DM_NB_OPS: the taken mask is known; count only)
        const char *e = getenv("DM_NB_FOREIGN");
        if (e && atoi(e) == 1) {
            nvn_nb_foreign_enable(m->base, m->size, NB_UI_LO, NB_UI_HI); g_nbf = 1;
            LOG("[nb] DM_NB_FOREIGN=1: every record / draw record counted by caller class (taken draws, taken packets, opcodes off the mask, "
                "callbacks 1/2, UI block main+%#x..%#x, direct, unknown, other threads); lines '[nb] foreign' every 10 s (NB-15, count only)", NB_UI_LO, NB_UI_HI);
        }
    }
    {   // NB-14: DM_NB_RELOC (1 / 2 only; the backend reads the same switch)
        const char *e = getenv("DM_NB_RELOC");
        const int r = e && *e ? atoi(e) : 0;
        if (r == 1 || r == 2) {
            nvn_nb_key_enable(); g_nbk = r;
            LOG("[nb] DM_NB_RELOC=%d: every OP_X_NB_PASS is followed by OP_X_NB_KEY, the state the pass starts from (the backend's state at the "
                "part's OP_X_NB_FRAME + the state records since, NB-14); the backend %s; lines '[nb] key' every 10 s", r,
                r == 1 ? "sets its state from it at every pass" : "compares it with its state at every pass ('[nb] key check')");
        } else if (e && *e && r) LOG("[nb] DM_NB_RELOC=%s: only 1 / 2 - off", e);
    }
    {   // NB-17a / NB-17b: DM_NB_ORDER (1 / 2 / 3; the backend reads the same switch)
        const char *e = getenv("DM_NB_ORDER"), *a = getenv("DM_NB_ORDER_ASSUME");
        const int r = e && *e ? atoi(e) : 0;
        if (r >= 1 && r <= 3) {
            g_nbo = r; g_nbo_assume = r >= 2 && a && atoi(a) == 1; nvn_nb_order_enable(r);
            LOG("[nb] DM_NB_ORDER=%d: OP_X_NB_FRAME carries the order tail (execution order + address of every OP_X_NB_PASS, NB-17a); %s; "
                "lines '[nb] order' every 10 s%s%s", r,
                r == 1 ? "the stream order, the backend executes it segment by segment" :
                r == 2 ? "the merge planner's order (NB-16), the backend verifies it and executes as recorded" :
                         "the merge planner's order (NB-16), the backend executes it",
                g_nbo_assume ? "; DM_NB_ORDER_ASSUME=1 (debug): aa merges planned as if every hazard were tracked" : "",
                r == 3 && !g_nbk ? "; DM_NB_RELOC is off: without key frames the backend runs only the stream order" : "");
        } else if (e && *e && r) LOG("[nb] DM_NB_ORDER=%s: only 1 / 2 / 3 - off", e);
    }
    {   // NB-21: DM_NB_OWN (1 / 2; the backend reads the same switch)
        const char *e = getenv("DM_NB_OWN");
        const int r = e && *e ? atoi(e) : 0;
        if (r == 1 || r == 2) {
            g_nbw = r; nvn_nb_own_enable(r);
            LOG("[nb] DM_NB_OWN=%d: every submission of the frame IR is handed to the backend with its frame job (segments, order; nothing "
                "written into the stream); %s; lines '[nb] own' every 10 s (NB-21)", r,
                r == 1 ? "the present and the frame number of a frame join the queue job of its final submit (one job a frame)" :
                         "check: the queue works as before and counts what 1 would do");
        } else if (e && *e && r) LOG("[nb] DM_NB_OWN=%s: only 1 / 2 - off", e);
    }
    LOG("[nb] DM_NB=%d (%s): ExecuteCommandList main+%#x, BeginFrame main+%#x, EndFrame main+%#x hooked + an ExecuteRenderCommand handler; "
        "frame IR from the NVN tap, planned per submission, nothing executed differently (NB-04); DM_NB_DUMP=%d", mode, mode == 2 ? "check" : "native",
        NB_ECL, NB_BEGIN, NB_END, g_nb_dump);
}
#endif
