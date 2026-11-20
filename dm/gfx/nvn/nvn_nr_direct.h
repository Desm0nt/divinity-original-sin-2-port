// [DOS2] NR-09R (DM_NR_DIRECT, docs/dos2/nr/NR-09R.md): interface between the NVN layer (nvn_nr_direct.inc, inside hle_nvn.c) and the
// front end (dm/host/hle_dos2_nr_direct.inc). Everything here is per thread or set once at install.
#ifndef DM_NVN_NR_DIRECT_H
#define DM_NVN_NR_DIRECT_H
#include <stdint.h>
// mode 0: nothing. 1: on. 2: everything the old way + what 1 would do, compared. parts: NRDP_* of nr_direct.h. Install time only.
void nvn_nrd_enable(int mode, int parts);
// 1 = the five entry points the draw code of the engine calls end in the bodies the direct recorder stands for (as nvn_nc_ready of
// NR-23); thunk[i] = the address GetProcAddress gave the engine (i = NVN_NC_* of nvn_nr_nocall.h).
int nvn_nrd_ready(const void *thunk[5]);
// 1 = a direct draw may be recorded into cb by this thread now: DM_NR_DIRECT=1, inside the NB-09 bracket of an engine draw (DM_NB_OPS
// bits 36 / 37), no check / capture mode of another step on this draw, cb is a command buffer that is recording. Reads only.
int nvn_nrd_can(uint64_t cb);
// The recorder, on the big stack (nvn_on_bigstack) and only after nvn_nrd_can said 1:
//   bind  = nvnDeviceGetTextureHandle(tex, smp) + nvnCommandBufferBindTexture(cb, stage, slot, handle), without the calls;
//   draw  = nvnCommandBufferDrawElements (instances < 0) / DrawElementsInstanced(.., 0, 0, instances), without the call. The NR-09
//           identity is taken as before (dm_nr_draw_ident): the front end hands it over first (dm_nr_tap_draw). timed: this draw's
//           tap / record split goes to the counters (1 in 64 draws).
void nvn_nrd_bind(uint64_t cb, int stage, int slot, int32_t tex, int32_t smp);
void nvn_nrd_draw(uint64_t cb, int32_t prim, uint32_t itype, int32_t count, uint64_t ib, int32_t instances, int timed);
// Counters since the last call (any thread).
enum { NVN_NRD_DRAWS, NVN_NRD_BINDS, NVN_NRD_FILT, NVN_NRD_WOULD, NVN_NRD_WOULD_FLAG, NVN_NRD_WOULD_PAST, NVN_NRD_BIND_SEEN, NVN_NRD_FORGET,
       NVN_NRD_TAP_DRAWS, NVN_NRD_TAP_HIT, NVN_NRD_TAP_LEARN, NVN_NRD_TAP_UNKNOWN, NVN_NRD_TAP_BAD, NVN_NRD_TIMED, NVN_NRD_NS_TAP, NVN_NRD_NS_REC,
       NVN_NRD_PLAIN, NVN_NRD_NS_STATE,
       // NR-UBD (DM_NR_UBD): UBO binds of the owner thread into the tracked recording / of them inside a served direct draw / not sent (=1) /
       // =1 would not send (=2) / flagged for the backend / past bind NRD_X_BITS of their record / tables forgotten / adopted
       NVN_NRD_UB_SEEN, NVN_NRD_UB_SCOPE, NVN_NRD_UB_SKIP, NVN_NRD_UB_WOULD, NVN_NRD_UB_FLAG, NVN_NRD_UB_PAST, NVN_NRD_UB_FORGET, NVN_NRD_UB_ADOPT,
       NVN_NRD_S_N };   // NS_STATE [NR-CPU]: the state block's share of NS_REC (the OP_X_NB_STATE written before the draw record)
void nvn_nrd_stats(uint64_t out[NVN_NRD_S_N]);
// [DOS2] NR-UBD (DM_NR_UBD, docs/dos2/nr/NR-UBD.md): the mirror of the backend's uniform-buffer table (dm/gfx/nr/nr_ubd.h). mode 0 nothing,
// 1 a BindUniformBuffer of a served direct draw whose slot already holds that range is not sent, 2 it is sent and flagged for the
// backend's check. Install time only. nvn_nrub_scope(1 / 0): this thread starts / ends serving a direct draw (the front end, around it).
void nvn_nrub_enable(int mode);
void nvn_nrub_scope(int on);
#endif
