// [DOS2] NR-01: the NVN tap of the engine object registry (DM_NR, dm/host/hle_dos2_nr.c, dm/gfx/nr/nr_reg.h). Included by hle_nvn.c.
// Four entry points call one NRT(...) line each, after the object is initialised: nvnTextureInitialize, nvnProgramSetShaders,
// nvnSamplerInitialize, nvnBufferInitialize; NR-02 adds one at the top of nvnCommandBufferBindProgram and of the seven draw entry
// points; NR-05 (DM_NR_TEX) adds the first statement of nvnTextureFinalize and one line after the write in nvnTextureWriteTexels /
// nvnTextureWriteTexelsStrided. With DM_NR unset (g_dm_nr == 0) that line is a load and a not-taken branch; with DM_NR=1 / 2 it only
// reads the host object and hands plain values over - it never writes an NVN object, a record or a return value.
#ifdef DM_GAME_DOS2
#include "nvn_nr_tap.h"
#define NRT(x) do { if (__builtin_expect(g_dm_nr != 0, 0)) { x; } } while (0)
static void nrt_texture(const tex_t *o) {
    dm_nr_tap_texture(o, o->d.target, o->d.format, o->d.flags, o->d.samples, o->d.width, o->d.height, o->d.depth, o->d.levels, o->has_layout, o->L.size,
                      o->serial, o->mem, &o->d, &o->L);
}
static void nrt_program(const prog_t *o) {
    dm_nr_tap_program(o, (int)o->nstage, o->nstage ? o->sd[0].control : NULL, o->nstage ? (const void *)(uintptr_t)o->sd[0].data : NULL);
}
static void nrt_texture_written(const tex_t *o) { dm_nr_tap_texture_written(o->serial); }   // NR-05: after the memcpy (inside the texel lock)
static void nrt_texture_gone(const tex_t *o) { if (o->magic == MAGIC('T', 'E', 'X', 0)) dm_nr_tap_texture_gone(o->serial); }   // NR-05: before Finalize clears magic
static void nrt_sampler(const samp_t *o) { dm_nr_tap_sampler(o, o->p.minf, o->p.magf, o->p.ws, o->p.wt, o->p.wr, o->p.cmp_mode, o->p.cmp_func, o->p.aniso); }
// nvnBufferInitialize calls dm_nr_tap_buffer directly (buf_t is defined further down in hle_nvn.c)
// NR-02: nvnCommandBufferBindProgram and the NVN draw entry points call dm_nr_tap_bind / dm_nr_tap_draw (one NRT line each, first
// statement); dm_nr_prog_key below is the DM_NR_KEY=2 check that a program's pack key at a draw is the one keyed at SetShaders (the host
// reads the stage's code at the first draw, xbg_game.cpp load_shader: the same two pointers).
int xbg_shader_program_key(const void *control, const void *code, char out_hex17[17]) __attribute__((weak));
int dm_nr_prog_key(const void *prog, char out[17]) {
    const prog_t *o = (const prog_t *)prog;
    out[0] = 0;
    if (!o || !o->nstage || !o->sd[0].control || !xbg_shader_program_key) return -1;
    return xbg_shader_program_key(o->sd[0].control, (const void *)(uintptr_t)o->sd[0].data, out) ? -1 : 0;
}
#else
#define NRT(x) do { } while (0)
#endif
