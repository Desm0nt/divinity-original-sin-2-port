// [DOS2] NR-01: interface between the NVN tap (nvn_nr_tap.inc, inside hle_nvn.c) and the engine object registry front end
// (dm/host/hle_dos2_nr.c). The tap hands over what an NVN object was initialised with; the front end attributes it to the engine call
// that is open on the calling thread (a thread-local bracket), or counts it as made outside every engine call. Any thread; no lock.
#ifndef DM_NVN_NR_TAP_H
#define DM_NVN_NR_TAP_H
#include <stdint.h>
#include "nvn_tex.h"
extern int g_dm_nr;   // hle_dos2_nr.c: DM_NR 0 off (default), 1 registry + census, 2 + checks
void dm_nr_tap_texture(const void *tex, int target, int format, int flags, int samples, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int packaged, uint64_t size,
                       uint64_t serial, const void *mem, const nvn_texdesc *desc, const nvn_texlayout *layout);   // NR-05: + serial, storage, description, layout
void dm_nr_tap_texture_gone(uint64_t serial);      // NR-05: nvnTextureFinalize of an initialised texture (DM_NR_TEX: its entry dies)
void dm_nr_tap_texture_written(uint64_t serial);   // NR-05: nvnTextureWriteTexels(Strided) finished writing into this texture
// control0 / code0: control section (the host's own copy) and code of stage 0; the front end keys the program as the host's shader pack
// does (DOS2 builds with DM_GAME_D3: xbg_shader_program_key = SHA-256 of the control + the code, not the control alone)
void dm_nr_tap_program(const void *prog, int nstage, const void *control0, const void *code0);
void dm_nr_tap_sampler(const void *smp, unsigned minf, unsigned magf, unsigned ws, unsigned wt, unsigned wr, unsigned cmp_mode, unsigned cmp_func, float aniso);
void dm_nr_tap_buffer(const void *buf, uint64_t size, const void *mem);   // mem: the buffer's storage = its GPU address (NR-06)
// NR-02 (DM_NR_KEY, default 0): the programs a command buffer binds and every NVN draw; both return at once unless DM_NR_KEY is on.
void dm_nr_tap_bind(const void *cb, const void *prog, int stages);
void dm_nr_tap_draw(const void *cb);
// nvn_nr_tap.inc: the pack key of an NVN program's first stage as it is now (control copy + code); 0 = keyed, -1 = no stage / no key.
int dm_nr_prog_key(const void *prog, char out_hex17[17]);
#endif
