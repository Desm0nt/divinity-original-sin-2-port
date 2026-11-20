// [DOS2] NR-09U (DM_NR_UBO, docs/dos2/nr/NR-09U.md): interface between the NVN layer (nvn_nr_ubo.inc, inside hle_nvn.c) and the front end
// (dm/host/hle_dos2_nr_ubo.inc). The calls are nru_call of dm/gfx/nr/nr_ubo.h (BindUniformBuffer with its five arguments). Everything here
// is per thread or set once at install.
#ifndef DM_NVN_NR_UBO_H
#define DM_NVN_NR_UBO_H
#include <stdint.h>
#include "../nr/nr_ubo.h"
// mode 0: nothing. 1: on. 2: the capture of the engine's own BindUniformBuffer calls (the NUB line) is armed. Install time only.
void nvn_nru_enable(int mode);
// 1 = the engine's BindUniformBuffer entry point ends in the body nvn_nru_exec calls; *thunk = the address GetProcAddress gave the engine.
int nvn_nru_ready(const void **thunk);
// The calls of one draw in order, through the body of nvnCommandBufferBindUniformBuffer (call it on the big stack): every effect of a call
// (the NB tap note, the NB-09 draw shadow or the OP_BIND_UBO record) is the body's own.
void nvn_nru_exec(const nru_call *c, int n);
// DM_NR_UBO=2: the BindUniformBuffer calls the entry point sees on this thread between begin and end (the engine's two functions running).
void nvn_nru_cap_begin(void);
int nvn_nru_cap_end(const nru_call **c);
#endif
