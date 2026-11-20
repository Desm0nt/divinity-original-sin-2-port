// [DOS2] NR-09S (DM_NR_STATE, docs/dos2/nr/NR-09S.md): interface between the NVN layer (nvn_nr_state.inc, inside hle_nvn.c) and the
// front end (dm/host/hle_dos2_nr_state.inc). The calls are nrst_op of dm/gfx/nr/nr_state.h (the compact native state record of one
// opcode-21 packet). Everything here is per thread or set once at install.
#ifndef DM_NVN_NR_STATE_H
#define DM_NVN_NR_STATE_H
#include <stdint.h>
#include "../nr/nr_state.h"
// mode 0: nothing. 1: on. 2: the capture of the engine's own calls (the NST lines) is armed. Install time only.
void nvn_nrs_enable(int mode);
// 1 = the 19 entry points the packet calls end in the bodies nvn_nrs_exec calls (as nvn_nrd_ready of NR-09R); thunk[k] = the address
// GetProcAddress gave the engine for NRST_SLOTS row k (fn k + 1).
int nvn_nrs_ready(const void *thunk[NRST_F_N - 1]);
// The calls of one packet in order, on the big stack (nvn_on_bigstack), through the bodies of the entry points: every effect of a call
// (the NB tap, the NB-13a shadow, the record) is the body's own. The default-blend path's two stack objects (NRST_LOCAL_*) are this
// call's own, zeroed.
void nvn_nrs_exec(const nrst_op *op, int n);
// [NR-09S3] DM_NR_STATE=4 / 5 (nvn_nr_state3.inc): the packet's calls as ONE OP_X_NR_STATE record into command buffer cb (on the big stack,
// like nvn_nrs_exec). mode 4: the record + the side effects of the bodies that are not records (the engine's state objects, the IR tap's
// program per stage, the NB-13a shadow's kinds made unknown, SetRenderEnable* through their bodies). mode 5: a CHECK record (the backend
// applies it to a copy of its state), the calls through the bodies (nvn_nrs_exec), a CHECK_END record (the backend compares; seq pairs).
// Returns the record's payload bytes (> 0), 0 = not representable (nothing done: the caller makes the calls through nvn_nrs_exec).
uint32_t nvn_nrs3_exec(const nrst_op *op, int n, uint64_t cb, int mode, uint32_t seq);
// DM_NR_STATE=2: the calls the entry points see on this thread between begin and end (the engine's own handler running), as nrst_op.
void nvn_nrs_cap_begin(void);
int nvn_nrs_cap_end(const nrst_op **ops);
#endif
