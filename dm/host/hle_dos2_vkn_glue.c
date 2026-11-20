// [DOS2] VK-NATIVE stage 3: the few calls between the host parts that were written apart (each part declares the other side weak).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdint.h>
#include "hle_dos2_vkn_exec.h"
// The pass part reset NVN's viewport and scissor to a target's full size (0x9279a0 / 0x928be0 write the viewport cache but not the
// scissor cache): the exec part's "scissor in effect" must see it as a viewport event (hle_dos2_vkn_pass.h declares this weak).
void dm_vkn_on_nvn_viewport_scissor(int32_t x, int32_t y, int32_t w, int32_t h);
void dm_vkn_on_nvn_viewport_scissor(int32_t x, int32_t y, int32_t w, int32_t h) { (void)x; (void)y; (void)w; (void)h; dm_vkn_exec_viewport_note(); }
#endif
