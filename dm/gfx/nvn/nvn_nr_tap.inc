// [DOS2] NR-01: the NVN tap of the engine object registry (DM_NR, dm/host/hle_dos2_nr.c, dm/gfx/nr/nr_reg.h). Included by hle_nvn.c.
// Four entry points call one NRT(...) line each, after the object is initialised: nvnTextureInitialize, nvnProgramSetShaders,
// nvnSamplerInitialize, nvnBufferInitialize. With DM_NR unset (g_dm_nr == 0) that line is a load and a not-taken branch; with DM_NR=1 / 2
// it only reads the host object and hands plain values over - it never writes an NVN object, a record or a return value.
#ifdef DM_GAME_DOS2
#include "nvn_nr_tap.h"
#define NRT(x) do { if (__builtin_expect(g_dm_nr != 0, 0)) { x; } } while (0)
static void nrt_texture(const tex_t *o) {
    dm_nr_tap_texture(o, o->d.target, o->d.format, o->d.flags, o->d.samples, o->d.width, o->d.height, o->d.depth, o->d.levels, o->has_layout, o->L.size);
}
static void nrt_program(const prog_t *o) { dm_nr_tap_program(o, (int)o->nstage, o->nstage ? o->sd[0].control : NULL); }
static void nrt_sampler(const samp_t *o) { dm_nr_tap_sampler(o, o->p.minf, o->p.magf, o->p.ws, o->p.wt, o->p.wr, o->p.cmp_mode, o->p.cmp_func, o->p.aniso); }
// nvnBufferInitialize calls dm_nr_tap_buffer directly (buf_t is defined further down in hle_nvn.c)
#else
#define NRT(x) do { } while (0)
#endif
