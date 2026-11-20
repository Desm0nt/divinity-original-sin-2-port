// [dos2-nb] NB-02: frame IR of the native render backend and the tiler load/store planner (docs/dos2/NATIVE_BACKEND_PLAN.md §4).
// Pure C: no Vulkan, no game, no allocation. The front end (render thread) builds one nb_frame per submission with the nb_* calls
// below, nb_plan() turns it into per-pass load/store decisions, clear folding and transient candidates; nb_dump()/nb_parse() give a
// text form that round-trips (golden frames from the phone are the input of tests/nb_plan_test.cpp).
//
// Model. A frame is a sequence of nodes in submission order: passes (one render-target set, draws, in-pass clears, texture reads)
// and operations outside passes (copy / blit / resolve / compute / barrier / readback / present / clear of a texture). Images are
// opaque 64-bit ids (the FE uses the pool registration key = texture memory address, so aliases share an id).
// Load / store values equal VkAttachmentLoadOp / VkAttachmentStoreOp (LOAD 0, CLEAR 1, DONT_CARE 2; STORE 0, DONT_CARE 1).
#ifndef DM_GFX_NB_IR_H
#define DM_GFX_NB_IR_H
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NB_MAX_ATT = 9, NB_SLOT_DS = 8 };          // colour slots 0..7, slot 8 = depth/stencil (both aspects decided together, FN-14/17)
enum { NB_MAX_NODES = 512, NB_MAX_REFS = 8192, NB_MAX_CLEARS = 512, NB_MAX_FRAME_IMAGES = 2048, NB_PERSIST_CAP = 4096 };

// NB_OP_FENCE (NB-16): a hazard outside passes (nb_hazard with no pass open: counter / conditional render / inline UBO update / CPU-visible
// work between passes); no images, nothing is moved across it.
enum { NB_NODE_PASS = 0, NB_OP_COPY, NB_OP_BLIT, NB_OP_RESOLVE, NB_OP_COMPUTE, NB_OP_BARRIER, NB_OP_READBACK, NB_OP_PRESENT,
       NB_OP_CLEAR, NB_OP_FENCE, NB_OP_KINDS };
enum { NB_OPF_DST_FULL = 0x100 };                 // nb_op kind flag: every dst is overwritten entirely (else dst keeps old texels)
enum { NB_END_OPEN = 0, NB_END_RT, NB_END_CLEAR, NB_END_OP, NB_END_COMPUTE, NB_END_BARRIER, NB_END_SUBMIT, NB_END_KINDS };
enum { NB_ASP_COLOR = 1, NB_ASP_DEPTH = 2, NB_ASP_STENCIL = 4 };
// NB_CLEAR_FULL: whole extent and every aspect the image has (the FE knows the format; a depth-only clear of D24S8 is NOT full).
// NB_CLEAR_RT: a clear of a bound render target (ClearColor / ClearDepthStencil); without it the clear is a texture clear
// (ClearTexture), which is an operation outside passes and closes the open pass.
enum { NB_CLEAR_FULL = 1, NB_CLEAR_RT = 2 };
enum { NB_LOAD = 0, NB_LOAD_CLEAR = 1, NB_LOAD_DONT_CARE = 2 };
enum { NB_STORE = 0, NB_STORE_DONT_CARE = 1 };
enum { NB_REF_READ = 0, NB_REF_SRC = 1, NB_REF_DST = 2 };

typedef struct nb_att { uint64_t image; uint8_t slot; } nb_att;
typedef struct nb_ref { uint64_t image; uint8_t role; } nb_ref;
typedef struct nb_clear_rec {
    uint64_t image; uint32_t color[4]; float depth; uint32_t stencil;
    uint8_t aspects, flags;        // NB_ASP_*, NB_CLEAR_*
    uint8_t after_draw;            // in-pass clear issued after the pass's first draw (stays vkCmdClearAttachments)
    int16_t node;
} nb_clear_rec;
typedef struct nb_node {
    uint16_t kind;                 // NB_NODE_PASS or NB_OP_* (ops: | NB_OPF_DST_FULL)
    uint8_t natt, end;             // pass: attachments; end reason NB_END_*
    nb_att att[NB_MAX_ATT];
    uint32_t draws, cover;         // cover: bit per slot fully covered by the first draw (proof from the host, e.g. first_draw_covers)
    int16_t clear_first, nclear;   // range in nb_frame.clr (in-pass clears, or the clear of an NB_OP_CLEAR)
    int ref_first, nref;           // range in nb_frame.ref (texture reads of a pass; src / dst of an op)
    uint16_t nbar;                 // pass: NVN barriers kept inside it (nb_barrier, NB-05b): no attachment of the pass was read after them
    uint8_t hz;                    // NB-16: NB_HZ_* seen inside the pass (or the hazards of an NB_OP_FENCE node)
    // NB-18b-4a (DM_NB_POST): program facts of the pass's draws (nb_draw_prog; dump line "fs=<key> [mixed] [exact]" + "sread" lines).
    // fs = the first 16 hex digits of the fragment shader's translated key as a number (0 = unknown); sread = the images the bound
    // programs really sample (their texture / image slots from the backend's program table), exact only with NB_FSX_EXACT.
    uint8_t fsx, nsr;              // NB_FSX_*; entries in sread
    uint64_t fs;
    uint64_t sread[12];
} nb_node;
enum { NB_MAX_SREAD = 12 };
enum { NB_FSX_ANY = 1, NB_FSX_MIXED = 2, NB_FSX_EXACT = 4 };   // a draw reported its program / draws with different fs / sread exact
// NB-16 hazards (plan §7 risk 8, §4 "Слияния"): a pass carrying one is never moved or merged, and nothing moves across it.
// COUNTER: ReportCounter / queries (occlusion, timestamps: op61 is the DRS GPU marker); COND: SetRenderEnable* / conditional render;
// SSBO: storage buffer or storage image writes by draws; CPU: CPU-visible work ordered with the pass (host memmove of a buffer copy,
// semaphore / event); UBO: inline UpdateUniformBuffer (a buffer write ordered in the command stream).
enum { NB_HZ_COUNTER = 1, NB_HZ_COND = 2, NB_HZ_SSBO = 4, NB_HZ_CPU = 8, NB_HZ_UBO = 16, NB_HZ_ALL = 31 };
// nb_frame.track: what the front end reports. The merge planner needs both: HAZARDS = every NB_HZ_* source above is tapped (until then
// the IR does not see them, NB-04 findings); VIEWS = attachment ids differ for different mips / layers / views of one memory (today a
// view of the same memory shares the id: two passes on mip 0 and mip 1 would look like the same set).
enum { NB_TRACK_HAZARDS = 1, NB_TRACK_VIEWS = 2, NB_TRACK_MERGE = 3 };
typedef struct nb_frame {
    uint32_t frame_no;             // game frame counter (submissions of one frame share it); written-this-frame is tracked per frame
    uint8_t final;                 // last submission of the frame (set by nb_submit); mid-frame submissions keep unresolved stores
    uint8_t open;                  // a pass is open (node[nnode-1])
    uint8_t last_natt;
    nb_att last_att[NB_MAX_ATT];   // RT set still bound after a pass was closed by an operation: the next draw reopens it
    int nnode, nref, nclear;
    uint32_t overflow, stray;      // records lost to full tables; draws / reads with no RT set
    uint8_t bar_pending;           // NB-05b: a barrier was kept inside the open pass; a later read of one of its attachments ends it there
    uint32_t bar_kept, bar_split;  // barriers kept inside passes / passes ended by an attachment read after a kept barrier
    uint8_t track;                 // NB-16: NB_TRACK_* (dump line "track hazards views"); 0 in every P2 dump
    nb_node node[NB_MAX_NODES];
    nb_ref ref[NB_MAX_REFS];
    nb_clear_rec clr[NB_MAX_CLEARS];
    // [DOS2] NR-CPU NR-TAPSET (docs/dos2/nr/NR-CPU.md): nb_read's "does the open pass already read this image" through a hash set of the
    // pass's refs instead of a scan of all of them (a G-buffer / forward pass of Fort Joy holds hundreds: the scan was quadratic in the
    // pass). rs_mode (set by the front end, kept by nb_frame_reset): 0 the scan (default), 1 the set, 2 both, the scan's answer used and
    // a different answer counted in rs_bad. The set mirrors refs [ref_first, ref_first + nref) of node rs_node and catches up with refs
    // appended by anyone (only nb_read / nb_op / nb_clear append; nothing removes or rewrites a ref); entries are valid while their tag
    // is rs_tag (a new pass = a new tag, no clearing). Over NB_RS_MAX refs in one pass: the scan (counted in rs_big).
    uint8_t rs_mode;
    int32_t rs_node, rs_first, rs_n;
    uint32_t rs_tag;
    uint64_t rs_bad, rs_big, rs_hits, rs_lookups;
    struct { uint64_t image; uint32_t tag, _pad; } rs[2048];
} nb_frame;
enum { NB_RS_SLOTS = 2048, NB_RS_MAX = 1024 };

// Persistent per-image attributes (between frames; nb_persist_forget on re-creation of the image).
typedef struct nb_img_state {
    uint64_t image; uint32_t wframe; uint8_t used, wset;
    uint8_t xread;                 // in some frame read or loaded before its first write in that frame (cross-frame reader)
    uint8_t ever_read;             // ever sampled / copied from / presented
    uint16_t trans_streak;         // consecutive frames that met the transient conditions
} nb_img_state;
typedef struct nb_persist { nb_img_state e[NB_PERSIST_CAP]; int count; uint32_t full; } nb_persist;

enum { NB_RULES_EXACT = 1, NB_RULES_IPAD = 2 };    // DM_NB_RP=1 / DM_NB_RP_IPAD=1
// NB_RULEF_FRESH: exact rule's "not written this frame and not xread -> DONT_CARE" (plan §4). Off by default: unsafe for targets the
// game updates partially (UI cache, shadow atlas tiles) - see "NB-02 findings" in the plan. Needs a poison run before it is turned on.
enum { NB_RULEF_FRESH = 1 };
typedef struct nb_rules { int mode; unsigned flags; int transient_frames; } nb_rules;   // transient_frames 0 = 8

enum { NB_CLR_PENDING = 0, NB_CLR_FOLDED, NB_CLR_STANDALONE, NB_CLR_DEAD, NB_CLR_INPASS };
// NB_NP_PREBAR (NB-05c): the pass samples an image written in this submission (attachment of an earlier pass, op dst, standalone clear)
// with no barrier point since: Vulkan needs a memory barrier between that write and the read, outside the render pass. The FE knows the
// whole pass's reads, so the barrier goes right before the pass begins (the BE's job from NB-06 on; one barrier point covers every
// write before it). The host without it decides at the read: a read in draw 2+ of a pass it has begun ends that pass (prepare_read
// why 2 "written") - P2's whole "split" class. The pass itself never needs to end there.
// NB_NP_ELIDE / NB_NP_FUSE (NB-18b-4a, nb_post_plan, DM_NB_POST): a post-tail pass whose only output is read by the next pass alone is
// not executed (its draw's bindings are stashed by the BE), and that next pass draws the fused XGF1 shader (dm/gfx/nb/nb_fused.h) fed
// from the stash. Set only by nb_post_plan, after nb_plan.
// NB_NP_AA (NB-18b-8, route A, nb_post_aa.h, DM_NB_POST_AA): with ELIDE / FUSE on an F1 pair = the F1A bundle (FXAA inside); with ELIDE
// on the console's FXAA pass (or its PPDefault copy) = that pass is elided and its readers sample Sight's target; alone = the pass
// samples such an image.
enum { NB_NP_EXEC = 1, NB_NP_EMPTY = 2, NB_NP_CLEAR_ONLY = 4, NB_NP_FEEDBACK = 8, NB_NP_PREBAR = 16, NB_NP_ELIDE = 32, NB_NP_FUSE = 64, NB_NP_AA = 128 };
typedef struct nb_node_plan {
    uint8_t load[NB_MAX_ATT], store[NB_MAX_ATT];   // by attachment index (same order as nb_node.att)
    int16_t clear[NB_MAX_ATT];     // load == CLEAR: index of the clear record giving the values, else -1
    uint8_t flags;                 // NB_NP_*
    uint8_t oc;                    // NR-ONCHIP (nb_onchip.h nb_onchip_plan, DM_NR_ONCHIP): chain mark, 0 = none; set after nb_plan
    uint16_t cover_dc;             // NB-05: colour attachments (bit = index) whose LOAD would be DONT_CARE with a coverage proof of the
                                   // first draw (exact rules, no feedback): the BE proves it at the draw (first_draw_covers), the FE cannot
} nb_node_plan;
typedef struct nb_clear_plan { uint8_t fate; int16_t into; } nb_clear_plan;   // into: target pass of a FOLDED clear
typedef struct nb_plan_stats {
    int passes, exec, empty, clear_only, folded, standalone, dead, load_clear, load_dc, load_load, store_dc, store_store;
    int transient, images, xread_new;
    int prebar;                    // NB-05c: passes with NB_NP_PREBAR
    int store_lost;                // store DONT_CARE kept as STORE: the submission lost records (overflow / stray), a reader may be missing
} nb_plan_stats;
typedef struct nb_plan_scratch {
    uint64_t image; nb_img_state *ps; nb_img_state loc; int16_t pend; uint8_t w, next, rd, att_ok, used_att, other, persist;
    uint32_t wseq;                 // NB-05c: write clock of its last write in this submission (0 = none): > barrier point = needs NB_NP_PREBAR
} nb_plan_scratch;
typedef struct nb_plan_out {
    int nnode, ntransient;
    nb_node_plan node[NB_MAX_NODES];
    nb_clear_plan clr[NB_MAX_CLEARS];
    uint64_t transient[64];
    nb_plan_stats st;
    nb_plan_scratch img[NB_MAX_FRAME_IMAGES];   // planner scratch (per-frame image table)
    int nimg;
} nb_plan_out;

// Building (render thread, single-threaded).
void nb_frame_reset(nb_frame *f, uint32_t frame_no);
int  nb_pass_begin(nb_frame *f, const nb_att *rt, int n);       // SetRenderTargets; closes an open pass (NB_END_RT); returns node or -1
void nb_clear(nb_frame *f, uint64_t image, unsigned aspects, unsigned flags, const uint32_t color[4], float depth, uint32_t stencil);
void nb_draw(nb_frame *f, uint32_t cover_slots);                // reopens the last RT set if an operation closed the pass
int  nb_read(nb_frame *f, int pass, uint64_t image);            // texture read at draw time; pass < 0 = the open pass (only that one)
int  nb_op(nb_frame *f, int kind, const uint64_t *src, int nsrc, const uint64_t *dst, int ndst);   // closes the open pass
// NB-05b: an NVN barrier (nvnCommandBufferBarrier names no image). Inside an open pass it is kept there (node.nbar, bar_pending): the
// barrier orders the pass's own attachment writes only against a later read of those attachments, so the pass ends only when such a
// read comes (nb_barrier_hazard -> NB_END_BARRIER + an "op barrier" node, the read's draw reopens the set). With no pass open it is an
// "op barrier" node as before. The host (xbg_game.cpp OP_BARRIER) skips guest barriers and ends its pass on that read (prepare_read).
int  nb_barrier(nb_frame *f);                                   // returns the node it went to, or -1
int  nb_barrier_hazard(nb_frame *f, uint64_t image);            // call before a draw for each image it reads; 1 = the pass was ended
void nb_pass_end(nb_frame *f, int reason);
// NB-18b-4a: the program facts of the draw just counted (nb_draw) for the open pass: fs key (0 unknown), exact = the sampled list below
// is complete for this draw (the backend's program table knew every bound stage), sampled = images of the slots its programs read.
void nb_draw_prog(nb_frame *f, uint64_t fs, int exact, const uint64_t *sampled, int n);
// NB-16: a hazard (NB_HZ_*): marks the open pass, or with no pass open adds / extends an NB_OP_FENCE node (does not close anything).
void nb_hazard(nb_frame *f, unsigned hz);
void nb_track(nb_frame *f, unsigned track);                     // NB_TRACK_* the front end guarantees for this frame
void nb_submit(nb_frame *f, int final_in_frame);                // closes the open pass (NB_END_SUBMIT)

// Planning. persist may be NULL (no history, nothing learned). Returns 0, or -1 on bad input.
void nb_persist_reset(nb_persist *p);
void nb_persist_forget(nb_persist *p, uint64_t image);
const nb_img_state *nb_persist_find(const nb_persist *p, uint64_t image);
int  nb_plan(const nb_frame *f, nb_persist *p, const nb_rules *r, nb_plan_out *out);

// Text form. nb_dump_str returns the length it needed (output truncated if > cap). plan may be NULL; plan lines start with '#'.
size_t nb_dump_str(const nb_frame *f, const nb_plan_out *plan, char *buf, size_t cap);
void nb_dump(const nb_frame *f, const nb_plan_out *plan, FILE *out);
int  nb_parse(const char *text, nb_frame *f);                   // 0, or the 1-based line of the first error
const char *nb_kind_name(int kind);
const char *nb_end_name(int reason);

/* ---------------- NB-18b-4a: post-tail fusion plan (DM_NB_POST; docs/dos2/NATIVE_BACKEND_PLAN.md "NB-18b-3 / 18b-4 findings") ----------------
   After nb_plan: finds the chains F1 = PPApplyLUT -> EoCSight and F2 = PPVignette -> CombineUI_NoFetch (classes by fs key prefix,
   nb_post_class) and sets NB_NP_ELIDE on the first pass (E) and NB_NP_FUSE on the second (H). Exact only: H and E executed, one draw each,
   one fs each, exact sampled reads, no NB_HZ_*; between them only barrier ops / empty passes; E has one colour attachment (slot 0), no
   clear, does not sample its own target and does not depend on its old texels (load CLEAR / DONT_CARE / cover_dc, or a feedback flag that
   the exact reads disprove); H samples E's target O; nobody else reads O before its next full overwrite (passes by sread when exact, else
   by IR reads; ops by src; an attachment load of O that keeps old texels; a partial op dst), and if O survives the submission its first
   access in each of the last hist_frames submissions was a write (history h); sizes via r->size when given (F1: O == H's target 0;
   both: every RT image E samples has O's size). One reason per candidate host pass in o->why. */
enum { NB_PSH_OTHER = 0, NB_PSH_LUT, NB_PSH_SIGHT, NB_PSH_VIG, NB_PSH_COMB, NB_PSH_KINDS };
enum { NB_FXK_NONE = 0, NB_FXK_F1 = 1, NB_FXK_F2 = 2, NB_FXK_KINDS };
enum { NB_PW_FUSED = 0, NB_PW_HOST, NB_PW_NO_ELIDED, NB_PW_ELIDED_SHAPE, NB_PW_ELIDED_LOADS, NB_PW_INEXACT, NB_PW_NO_EDGE, NB_PW_OTHER_READER,
       NB_PW_LATER_LOAD, NB_PW_PERSIST, NB_PW_SIZE, NB_PW_KINDS };
typedef struct nb_post_rules {
    unsigned fx;                   // bit per NB_FXK_* allowed (0 = both)
    int hist_frames;               // 0 = 8
    int (*size)(void *ctx, uint64_t image, uint32_t *w, uint32_t *h);   // NULL = no size checks
    void *ctx;
} nb_post_rules;
typedef struct nb_post_hist { struct { uint64_t image; uint32_t seen; uint8_t xread; } e[32]; int n; } nb_post_hist;   // zero = empty
typedef struct nb_post_out {
    int nnode, fused[NB_FXK_KINDS], later_cover, assumed_cover;   // later_cover: a later LOAD of O by cover_dc; assumed: by a disproved feedback
    uint8_t fx[NB_MAX_NODES];      // by node: NB_FXK_* of an elided / fused pass
    int16_t peer[NB_MAX_NODES];    // by node: the other pass of its pair (-1)
    int why[NB_FXK_KINDS][NB_PW_KINDS];
} nb_post_out;
int nb_post_class(uint64_t fs);
const char *nb_post_class_name(int cls);
const char *nb_post_why_name(int why);
int nb_post_plan(const nb_frame *f, nb_plan_out *plan, const nb_post_rules *r, nb_post_hist *h, nb_post_out *o);   // pairs fused, -1 bad input

/* ---------------- NB-16: merge planner (plan §4 "Слияния", §8 Этап E) ----------------
   Input: the frame and its nb_plan() output. Output: an execution order of all nodes and "pass j continues pass i" links.
   aa:  the nearest earlier executed pass with the same attachment set (images + slots) has only non-pass nodes (ops, empty /
        clear-only passes) between it and j; aba: there are executed passes ("B") between. Either way the nodes between are split
        into those that must stay after the merged unit (they depend on A1, transitively) and the rest, which go before it; the unit
        A1..A2 becomes contiguous. x before y depend on each other when x writes what y reads or writes, x reads what y writes, or
        either is a fence (compute, buffer copy / clear = copy without images, readback, present, NB_OP_FENCE, a pass with NB_HZ_*).
        Barrier ops and empty passes have no dependencies (the host skips guest barriers; every image write is tracked).
   Bans (blocked reason = the first that applies): frame not NB_TRACK_MERGE; kind disabled; feedback on A1 or A2; A2 loads with CLEAR
        (an overwrite, not a continuation); NB_HZ_* on A1 / A2; a fence / hazard node between; a node between that touches A's
        attachments (reads A1's output or writes them: it would have to be both before and after); a transitive chain of the same;
        A2 reads the output of an executed pass B between (strict "aba exact"; NB_MERGEF_HOIST lifts it: B and what it needs move
        before A1). Clear-only / empty passes (NB-06) are never A1 or A2. A merged unit is atomic for later candidates (A A A = 1 unit).
   PREBAR (NB-05c) is recomputed for the execution order (per unit, at its head); the identity order gives nb_plan's flags.
   A unit executes with the load ops of its first pass's header and the store ops of its last; in-pass clears after a draw stay
   vkCmdClearAttachments. NB_MERGE_OFF (DM_NB_MERGE=0, and the NB-17a identity run) = identity order, no links; the analysis
   (candidates / blocked) is still filled with what NB_MERGE_ON would do. Deterministic: one pass over the nodes in order. */
enum { NB_MERGE_OFF = 0, NB_MERGE_ON = 1 };
enum { NB_MERGEF_AA = 1, NB_MERGEF_ABA = 2, NB_MERGEF_HOIST = 4,
       NB_MERGEF_ASSUME_TRACKED = 8 };            // analysis of old dumps only: treat the frame as NB_TRACK_MERGE (an upper bound)
typedef struct nb_merge_rules { int mode; unsigned flags; } nb_merge_rules;   // flags without AA / ABA = both
enum { NB_MB_UNTRACKED = 0, NB_MB_KIND_OFF, NB_MB_FEEDBACK, NB_MB_A2_CLEAR, NB_MB_COUNTER, NB_MB_COND, NB_MB_SSBO, NB_MB_CPU, NB_MB_UBO,
       NB_MB_FENCE, NB_MB_TOUCHES_A, NB_MB_CHAIN, NB_MB_A2_READS_B, NB_MB_KINDS, NB_MB_MERGED = 0xff };
enum { NB_MK_AA = 0, NB_MK_ABA = 1 };
typedef struct nb_merge_rec {      // one candidate (pass j with an earlier executed pass of the same set), in examination order
    int16_t node, prev, head;      // j, the unit's last pass before j, the unit's first pass
    uint8_t kind, why;             // NB_MK_*, NB_MB_* or NB_MB_MERGED
    int16_t between, before, after, culprit;   // nodes between; of them moved before / kept after the unit; node that blocked (-1)
} nb_merge_rec;
typedef struct nb_merge_stats {
    int cand_aa, cand_aba, aa, exact, merged, moved, units, prebar, blocked[NB_MB_KINDS];
} nb_merge_stats;
typedef struct nb_merge_out {
    int nnode, nrec;
    int16_t order[NB_MAX_NODES];   // execution order (node indices)
    int16_t pos[NB_MAX_NODES];     // by node: position in order
    int16_t cont[NB_MAX_NODES];    // by node: previous pass of its unit (-1 = begins a unit / not a pass)
    int16_t head[NB_MAX_NODES];    // by node: first pass of its unit (itself for everything else)
    uint8_t prebar[NB_MAX_NODES];  // by node: barrier before the unit begins (unit heads only), for this order
    nb_merge_rec rec[NB_MAX_NODES];
    nb_merge_stats st;
    // scratch: per-node access lists (writes then reads) and class; items of the range between a candidate pair
    uint64_t acc[NB_MAX_REFS + NB_MAX_NODES * 2 * NB_MAX_ATT + NB_MAX_CLEARS];
    int32_t acc_first[NB_MAX_NODES]; uint16_t acc_nw[NB_MAX_NODES], acc_nr[NB_MAX_NODES]; uint8_t cls[NB_MAX_NODES];
    int16_t item[NB_MAX_NODES + 1], tmp[NB_MAX_NODES]; uint8_t after[NB_MAX_NODES], need[NB_MAX_NODES];
} nb_merge_out;
int nb_merge(const nb_frame *f, const nb_plan_out *plan, const nb_merge_rules *r, nb_merge_out *out);   // 0, or -1 on bad input
const char *nb_merge_why_name(int why);
size_t nb_merge_summary(const nb_merge_out *m, char *buf, size_t cap);   // one line "aa=.. aba=.. exact=.. blocked(reason n ...)"

/* Order tail of OP_X_NB_FRAME, layout v1 (NB-16). NOT the layout in the stream: NB-17a writes the segment tail of nb_order.h (execution
   order + address of every PASS record); this pack / parse stays for the planner's tests only.
   v1, little endian, 4-byte aligned, appended right after p_nb_frame:
     nb_merge_tail_hdr { version = 1, flags (NB_MTF_*), bytes = whole tail, nnode, ncont = entries with NB_MT_CONT, seq = p_nb_frame.seq }
     uint16_t entry[nnode] in execution order: node index (bits 0..9) | NB_MT_CONT (bit 15: this pass continues the previous pass
       entry: no end / begin between) | NB_MT_PREBAR (bit 14: full barrier before this unit begins; replaces the headers' NB_NP_PREBAR
       when a tail is present); padded to 4 bytes with 0xffff
     NB_MTF_OFFSETS: then uint32_t offset[nnode] by node: byte offset of the node's first record from the OP_X_NB_FRAME record
       (OP_X_NB_PASS of a pass; first NVN record of an op), filled by the stream side; 0xffffffff = none.
   Identity (DM_NB_MERGE=0, NB-17a =2): entries 0..nnode-1, no CONT, PREBAR = the headers' flags. Size: 12 + 2n (padded to 4) (+ 4n). */
enum { NB_MERGE_TAIL_VERSION = 1, NB_MT_NODE = 0x3ff, NB_MT_PREBAR = 0x4000, NB_MT_CONT = 0x8000, NB_MTF_OFFSETS = 1, NB_MTF_MERGED = 2 };
typedef struct nb_merge_tail_hdr { uint8_t version, flags; uint16_t bytes; uint16_t nnode, ncont; uint32_t seq; } nb_merge_tail_hdr;
size_t nb_merge_tail_size(int nnode, int with_offsets);
// Packs m (offsets may be NULL = no offset table). Returns the bytes written, 0 if cap is too small.
size_t nb_merge_tail_pack(const nb_merge_out *m, uint32_t seq, const uint32_t *offsets, void *buf, size_t cap);
// Validates (version, size, permutation, CONT only on a pass right after a pass entry) and unpacks; returns nnode or -1.
int nb_merge_tail_parse(const void *buf, size_t size, uint16_t *entry, int cap, uint32_t *offsets, nb_merge_tail_hdr *hdr);

#ifdef __cplusplus
}
#endif
#endif
