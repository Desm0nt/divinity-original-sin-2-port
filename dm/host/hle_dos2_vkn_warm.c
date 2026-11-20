// [DOS2] VK-NATIVE part "pipeline warm-up" (dm_vkn_warm_register, called by dm_vkn_buf_register: hle_dos2_vkn.c has no weak slot for
// it and is not this part's file): starts the warm-up worker of dm/gfx/vkn/vkn_warm.c in mode 1 and adds its census line. No hooked
// sites and no game addresses: the warm-up works on the backend's own tables (vkn_state / vkn_shader), the engine never sees it.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_warm.h"
#include "hle_dos2_vkn.h"

void dm_vkn_warm_register(void);
void dm_vkn_warm_register(void) {
    if (vkn_mode() != VKN_NATIVE) return;   // mode 2 builds no pipelines (the old path draws)
    if (vkn_warm_start()) dm_vkn_census(vkn_warm_census);
}
#endif
