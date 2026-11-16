// [DOS2] NR-01: interface between the NVN tap (nvn_nr_tap.inc, inside hle_nvn.c) and the engine object registry front end
// (dm/host/hle_dos2_nr.c). The tap hands over what an NVN object was initialised with; the front end attributes it to the engine call
// that is open on the calling thread (a thread-local bracket), or counts it as made outside every engine call. Any thread; no lock.
#ifndef DM_NVN_NR_TAP_H
#define DM_NVN_NR_TAP_H
#include <stdint.h>
extern int g_dm_nr;   // hle_dos2_nr.c: DM_NR 0 off (default), 1 registry + census, 2 + checks
void dm_nr_tap_texture(const void *tex, int target, int format, int flags, int samples, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int packaged, uint64_t size);
void dm_nr_tap_program(const void *prog, int nstage, const void *control0);   // control0: control section of stage 0 (the host's own copy)
void dm_nr_tap_sampler(const void *smp, unsigned minf, unsigned magf, unsigned ws, unsigned wt, unsigned wr, unsigned cmp_mode, unsigned cmp_func, float aniso);
void dm_nr_tap_buffer(const void *buf, uint64_t size);
#endif
