// [DOS2] NR-NATIVE step D, half b (DM_NRN_NOBODY, default 0; docs/dos2/nr/NR-NATIVE-D.md section 12): interface between the NVN layer
// (nvn_nrn_b.inc, inside hle_nvn.c) and the front end (dm/host/hle_dos2_nrn_b.inc). Everything here is per thread or set at install.
#ifndef DM_NVN_NRN_B_H
#define DM_NVN_NRN_B_H
#include <stdint.h>
#include "../nr/nr_ubo.h"
// One texture bind of the engine's flush: = nr_nocall.h nrnc_bind (the front end asserts the layout).
typedef struct { uint8_t stage, slot; int32_t tex, smp; } nvn_nrnb_tex;
// mode 0: nothing. 1: the binds of a card draw are folded without the bodies of the NVN entry points. 2: the bodies run, the fold runs
// next to them on copies of the state and the two are compared word for word. Install time only.
void nvn_nrnb_enable(int mode);
// =1, on the caller's stack (no big stack): the uniform-buffer binds ub[0..nub) and the texture binds tb[0..ntb) of one direct draw,
// in that order, as nvnCommandBufferBindUniformBuffer / nvnDeviceGetTextureHandle + nvnCommandBufferBindTexture would leave them: the
// two mirrors of NR-09R / NR-UBD, the tap's binding tables and their signature, the texture-set generation, the use marks, the
// left-out mask of a short draw and the shadow of the draw. 1 = folded (the draw follows: nvn_nrd_draw, in place); 0 = refused -
// NOTHING was touched, the caller runs the bodies (the reason is counted).
int nvn_nrnb_binds(uint64_t cb, const nru_call *ub, int nub, const nvn_nrnb_tex *tb, int ntb);
// =2, around the bodies of the same binds (on the big stack, as they run today): snap before the first body (what the fold would
// start from; whether it would refuse), check after the last one and before the draw (the fold on the copies against what the
// bodies left - red line and BAD at the first different word).
void nvn_nrnb_snap(uint64_t cb, const nru_call *ub, int nub, int ntb);
void nvn_nrnb_check(uint64_t cb, const nru_call *ub, int nub, const nvn_nrnb_tex *tb, int ntb);
// Counters since the last call (any thread).
enum { NVN_NRNB_FOLDS, NVN_NRNB_UBO, NVN_NRNB_TEX, NVN_NRNB_CMP, NVN_NRNB_BAD, NVN_NRNB_TEXGEN_MOVED,
       NVN_NRNB_NO_MODE, NVN_NRNB_NO_CAP, NVN_NRNB_NO_TAP, NVN_NRNB_NO_SHADOW, NVN_NRNB_NO_MIRROR, NVN_NRNB_NO_ARGS, NVN_NRNB_NO_ROOM, NVN_NRNB_S_N };
void nvn_nrnb_stats(uint64_t out[NVN_NRNB_S_N]);
#endif
