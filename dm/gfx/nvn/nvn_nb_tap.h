// [DOS2] NB-04: interface between the NVN tap (nvn_nb_tap.inc, inside hle_nvn.c) and the NB front end (dm/host/hle_dos2_nb.c).
// docs/dos2/NATIVE_BACKEND_PLAN.md §1 / §2. Everything runs on the game's render thread (dm_nb_frame returns NULL anywhere else).
#ifndef DM_NVN_NB_TAP_H
#define DM_NVN_NB_TAP_H
#include "../nb/nb_ir.h"
extern int g_dm_nb;                    // hle_dos2_nb.c: DM_NB 0 off, 1 native, 2 check (0 when a hook site did not match)
nb_frame *dm_nb_frame(void);           // front end: the IR to append to, NULL off the render thread (counted)
void dm_nb_submit(void);               // front end: the recording of the IR's command buffer is being submitted
void dm_nb_note(int what, unsigned n); // front end: counters, NBN_*
void nvn_nb_tap_reset(void);           // tap: the front end reset the IR (after a submission was planned)
int  nvn_nb_tap_image(uint64_t image, uint32_t *w, uint32_t *h, int *fmt);   // tap: size / NVN format of an RT image it saw (dump comments)
// NR-ONCHIP (dm/gfx/nb/nb_onchip.h): the size of attachment `att` (index in nb_node.att) of IR node `node` as bound when that pass opened.
// nvn_nb_tap_image is the LAST size seen for a storage address (an aliased RT of another size later in the frame overwrites it), so a size
// rule must use this. Kept only while on (the front end turns it on with DM_NR_ONCHIP > 0); 0 = off / unknown.
void nvn_nb_tap_pass_sizes(int on);
int  nvn_nb_tap_pass_size(int node, int att, uint32_t *w, uint32_t *h);
// tap (NB-05): back-patch the OP_X_NB_* records of this IR with its pass facts and plan (po NULL: no plan); before the submit is queued.
// Returns the NB record bytes written into the command buffer for this IR.
uint32_t nvn_nb_tap_patch(const nb_frame *f, const nb_plan_out *po, uint32_t part);
// NB-09 (nvn_nb_draw.inc): DM_NB_OPS bits 36 / 37. enable once at install; begin / end bracket one engine draw of opcode op on this thread
// (hle_dos2_native.c draw hook); stats = counters since the last call, indexed by NBDS_* (same order as in nvn_nb_draw.inc).
void nvn_nb_draw_enable(void);
void nvn_nb_draw_begin(unsigned op);
void nvn_nb_draw_end(void);
enum { NBDS_N = 11 };
void nvn_nb_draw_stats(uint64_t out[NBDS_N]);
// NB-13a (nvn_nb_state.inc): DM_NB_OPS state bits. enable once at install; begin / end around every engine packet while on (taken = the
// packet's opcode is a taken state bit); stats = counters since the last call, indexed by NBSS_* (order in nvn_nb_state.inc).
void nvn_nb_state_enable(void);
void nvn_nb_state_begin(int taken);
void nvn_nb_state_end(void);
enum { NBSS_N = 16 };
void nvn_nb_state_stats(uint64_t out[NBSS_N]);
// NB-14 (nvn_nb_key.inc): DM_NB_RELOC 1 / 2. enable once at install (OP_X_NB_KEY after every OP_X_NB_PASS); stats = counters since the last
// call, indexed by NBKS_* (order in nvn_nb_key.inc).
void nvn_nb_key_enable(void);
enum { NBKS_N = 22 };
void nvn_nb_key_stats(uint64_t out[NBKS_N]);
// NB-17a / NB-17b (nvn_nb_order.inc): DM_NB_ORDER 1 / 2 / 3. enable once at install (OP_X_NB_FRAME gets the order tail of nb_order.h);
// patch fills it for the IR just planned (after nvn_nb_tap_patch, before the submit is queued): m NULL = the stream order, planned 0 = no
// plan, tracked = m was planned with every hazard tracked; stats = counters since the last call, indexed by NBOS_* (order in nvn_nb_order.inc).
void nvn_nb_order_enable(int mode);
void nvn_nb_order_patch(const nb_frame *f, const nb_merge_out *m, int planned, int tracked);
enum { NBOS_N = 12 };
void nvn_nb_order_stats(uint64_t out[NBOS_N]);
// NB-21 (nvn_nb_own.inc + the queue in hle_nvn.c): DM_NB_OWN 1 / 2. enable once at install. build: the frame job of the IR just planned
// (after the patches, before its submit is queued; planned 0 = no plan). arm: the render thread is inside EndFrame with the present bit
// (1) / left it (0); end: EndFrame returned (an open job gets no present any more). stats = counters since the last call: NBWS_* of
// nvn_nb_own.inc, then nb_own.h NBW_H_* (the hand-off), then the queue jobs since the last call.
void nvn_nb_own_enable(int mode);
void nvn_nb_own_build(const nb_frame *f, int planned, uint32_t part);
void nvn_nb_own_arm(int on);
void nvn_nb_own_end(void);
enum { NBWS_N = 12, NBWH_N = 10, NBW_STATS = NBWS_N + NBWH_N + 1 };
void nvn_nb_own_stats(uint64_t out[NBW_STATS]);
// NBN_OTHER_CB: a tracked call into a command buffer other than the IR's; NBN_OTHER_SUBMIT: a submit without the IR's recording;
// NBN_UNREG: a bound texture / image id with no registered storage at draw time (the read is lost). The rest count NVN calls.
enum { NBN_OTHER_CB = 0, NBN_OTHER_SUBMIT, NBN_DRAW, NBN_DISPATCH, NBN_PROGRAM, NBN_STATE, NBN_VBUF, NBN_UBO, NBN_SSBO, NBN_COUNTER,
       NBN_UNREG, NBN_SM_KNOWN, NBN_SM_UNKNOWN, NBN_SM_SKIP, NBN_KINDS };
// NB-07 (DM_NB_SLOTMASK, default 1): NBN_SM_KNOWN / _UNKNOWN = draws + dispatches whose stages with bound slots all had / some lacked the
// backend's slot masks (unknown: every bound slot is a read); NBN_SM_SKIP = bound slots not taken as reads (first draw of a pass, dispatch).
void nvn_nb_tap_slotmask(int on);      // tap: DM_NB_SLOTMASK (install time)
// NB-15 (DM_NB_FOREIGN=1, default 0; nvn_nb_tap.inc): every record written into a command buffer (cmd_alloc) and every draw record
// (rec: OP_DRAW / OP_DRAW_IDX / OP_X_DRAW_CALL / _INDIRECT / OP_X_MULTI_DRAW_INDIRECT / OP_X_DRAW_TEX, and OP_X_NB_DRAW as an NB draw),
// counted by caller class. Count only: the command stream is the same with it on or off.
//   NBF_NB          records written by NB itself (NBD_QUIET: OP_X_NB_*, the state / draw shadows' own and written-back records, wrappers)
//   NBF_DRAW_FOREIGN inside a taken engine draw (DM_NB_OPS 36 / 37) but not modelled (NB-09 "foreign")
//   NBF_TAKEN_PKT   inside a packet whose opcode is taken (DM_NB_OPS) and not captured by its shadow
//   NBF_OFF_MASK    inside a packet whose opcode is not taken (callbacks 1 / 2 apart)
//   NBF_CALLBACK    inside packet 1 / 2 (the engine calls a function pointer of the packet), first caller in main outside the UI block
//   NBF_UI          outside a packet or in a callback, first caller in main inside the UI block (ui_lo..ui_hi)
//   NBF_DIRECT      outside a packet, first caller in main elsewhere (passes that call NVN themselves)
//   NBF_OTHER       outside a packet, no caller in main found (another module, or the frame walk stopped)
//   NBF_THREAD      on a thread that never ran an engine packet
enum { NBF_NB = 0, NBF_DRAW_FOREIGN, NBF_TAKEN_PKT, NBF_OFF_MASK, NBF_CALLBACK, NBF_UI, NBF_DIRECT, NBF_OTHER, NBF_THREAD, NBF_N };
enum { NBF_SITES = 256, NBF_OPS = 64, NBF_OP_NONE = 0xff };
typedef struct { uint32_t off, rec, drw; uint8_t op, used, pad[2]; } nbf_site;   // first caller in main (offset) per packet opcode (0xff = none)
typedef struct {
    uint64_t rec[NBF_N], drw[NBF_N], bytes[NBF_N];
    uint64_t op_rec[NBF_OPS], op_drw[NBF_OPS], op_bytes[NBF_OPS];   // every record / draw inside a packet, by its opcode (taken or not)
    nbf_site site[NBF_SITES];
    uint64_t site_full, other_module, walk_failed;
} nbf_stats;
void nvn_nb_foreign_enable(const void *main_base, size_t main_size, uint32_t ui_lo, uint32_t ui_hi);   // install time
void nvn_nb_foreign_pkt(int op, int taken);   // front end, around every engine packet: op >= 0 = it begins (taken: in DM_NB_OPS), -1 = it ended
void nvn_nb_foreign_stats(nbf_stats *out);    // counters since the last call, then zeroed (render thread, at the 10 s line)
#endif
