// [DOS2] NB-18b-4: interface of the post-tail fusion (DM_NB_POST; docs/dos2/NATIVE_BACKEND_PLAN.md "NB-18b-4 findings").
//   BE -> FE fs-key table: NB-07's table (dm/gfx/nb/nb_slotmask.h, xbg_nb.inc nbsm) gives the sampled slots per program serial; this one
//   adds only what NB-07 lacks - the fragment shader's key (first 16 hex digits as a number), keyed by the same prog_t serial, published
//   by the backend at OP_BIND_PROGRAM (xbg_nb_post.inc nbpost_publish) while DM_NB_POST is on. The NVN tap reads both at each draw
//   (nvn_nb_tap.inc nbt_prog_draw -> nb_draw_prog: fs key + exact sampled images); the front end (dm/host/hle_dos2_nb_post.c) runs
//   nb_post_plan after nb_plan and the PASS headers carry NB_NP_ELIDE / _FUSE.
#ifndef DM_GFX_NB_POST_H
#define DM_GFX_NB_POST_H
#include <stdint.h>
#include "nb_ir.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct nb_post_prog { uint64_t serial, fs; } nb_post_prog;   // prog_t serial; fragment shader key (0: no fragment shader)
extern int g_dm_nb_post;                              // DM_NB_POST as the front end took it (0 off; set at NB install, before guest code)
void dm_nb_post_publish(const nb_post_prog *p);       // backend (translation thread); first publication of a serial wins
const nb_post_prog *dm_nb_post_prog(uint64_t serial); // front end (render thread); NULL = not published (yet)
void dm_nb_post_install(void);                        // hle_dos2_nb.c dm_dos2_nb_install
void dm_nb_post_plan(const nb_frame *f, nb_plan_out *po);   // hle_dos2_nb.c nb_ir_plan, after nb_plan, before the headers are patched
void dm_nb_post_report(double frames);                // hle_dos2_nb.c nb_ir_report (every 10 s)
#ifdef __cplusplus
}
#endif
#endif
