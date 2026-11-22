// [DOS2] VK-DRS-FSR (docs/dos2/nr/VK-DRS-FSR.md, 22.11): anti-aliasing at the render resolution BEFORE FSR 1 EASU (vkn_fsr.c).
// DM_HACK_FSR swaps the console's PPFXAA step for tools/dos2_post/PPFXAA_fsr.frag (vkn_shlib_named.cpp): at a 1:1 stretch the console's
// FXAA bit for bit, below 100 % EASU instead of FXAA + the bilinear stretch - so with a lower 3D the picture had NO anti-aliasing at all
// and EASU reconstructed the stair steps of the point-sampled geometry sharp (the owner's "jaggies and shimmering at 75 %"; AMD's FSR 1
// guide: EASU wants an anti-aliased input). This piece (one draw-effect function, vkn_fx.h, order 450 after the post fusion) puts the
// console's FXAA back where it belongs, at the render resolution: for the PPFXAA draw of the swapped module at a stretch s < 1 it first
// draws the pre pass (ppfxaa_fsr_pre.frag.spv.inc = the same fxaa(), TexCoord0 = gl_FragCoord / size: 1:1 on the drawn part, proven bit
// for bit against the translated PPFXAA at 1:1, tools/dos2_post_fsr.py) into a copy of the source (a private colour target of the
// source's size and format; the draw's own vertex shader, viewport and constants, its scissor = the drawn part + 1 texel), then the
// engine's draw reads that copy instead of the source (a twin of the engine shader: the source binding fed by the effect texture, C1 as an
// effect block). The EASU module itself is unchanged (it clamps its taps to the drawn part, so the copy's undefined rest is never read).
// The stretch s is the vertex constant c3[0].xy of the draw (its TexCoord0 = uv x c3[0].xy + c3[0].zw, SPIR-V 28b24f7288e2abe4); at 1:1
// (and for the HUD portraits) nothing is done: the module's FXAA runs as the console's.
// Cost: one extra pass over the drawn part (s^2 of the target: 56 % at 75 %, 25 % at 50 %), the console's FXAA ALU there; the stretch pass
// stays EASU only.
// Switch DM_VK_NATIVE_FSR_AA (read once at install): 1 (default) = the pre pass; 0 = today's EASU of the raw image; 2 = count only (every
// decision made and counted "would", nothing drawn differently). Only installed with DM_HACK_FSR=1 / 3 and DM_VK_NATIVE_SH=1 (the
// library calls vkn_fsr_install when it applies the FSR module).
// Lines: "[vkn] fsr-aa: ..." at install and the first time each thing happens (red for a refusal); "[vkn] fsr-aa (10 s): ..." with the
// counters (riding on the shlib named census). No game addresses. Host test: vkn_stage3_test test_fsr.
#ifndef DM_VKN_FSR_H
#define DM_VKN_FSR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_FSR_AA_OFF = 0, VKN_FSR_AA_ON = 1, VKN_FSR_AA_COUNT = 2 };
// mode < 0: DM_VK_NATIVE_FSR_AA (default 1). Registers the function (once; any thread). The mode of the first call stays.
void vkn_fsr_install(int mode);
int vkn_fsr_mode(void);           // in effect (-1 = not installed)
void vkn_fsr_census(void);        // the 10-s line (nothing before the first PPFXAA draw)

enum { VKN_FSR_R_SWAPPED, VKN_FSR_R_TABLE, VKN_FSR_R_C3, VKN_FSR_R_SCALE, VKN_FSR_R_SRC, VKN_FSR_R_VIEWPORT, VKN_FSR_R_SCRATCH, VKN_FSR_R_SHADER,
       VKN_FSR_R_DROPPED, VKN_FSR_R_N };
typedef struct vkn_fsr_stats {
    uint64_t draws;               // PPFXAA draws of the swapped module seen
    uint64_t one_to_one;          // of them at a 1:1 stretch (the module's FXAA, nothing done)
    uint64_t pre;                 // pre passes drawn (the engine draw then read the copy)
    uint64_t would;               // mode 2: pre passes that would be drawn
    uint64_t pre_f1a;             // [VK-LOWRES] pre passes left out: route A drew F1A (FXAA on the part) into the source (vkn_fsr_pre_skip)
    uint64_t refused[VKN_FSR_R_N];   // drawn as recorded (today's EASU), by reason
    uint64_t scratch_made;        // copies made (one per source size / format)
    uint64_t scratch_freed;       // [VK-LOWRES] copies let go after 600 frames without a pre pass (route A)
    uint32_t last_w, last_h;      // the last pre pass's scissor (the drawn part + 1)
    float last_s[2];              // its stretch
} vkn_fsr_stats;
void vkn_fsr_stats_get(vkn_fsr_stats *s);
const char *vkn_fsr_reason_name(int r);
// [VK-LOWRES] route A at a part (docs/dos2/nr/VK-FRAME-WORK.md section 13; vkn_post.c at_host / at_step). At a lower 3D scale the AA step is
// the upscale and can't be skipped; with this module and its pre pass the step's FXAA is the pre pass, and F1A (FXAA inside Sight's draw,
// the same approximation as route A at 100 %) draws it on the part instead: the post module asks
//   vkn_fsr_route_ok()                  1 = installed, DM_VK_NATIVE_FSR_AA=1, the FSR module swapped in (else route A stays off at a part)
//   vkn_fsr_stretch_part(in, ctx, lim)  1 = this draw gets a pre pass (stretch < 1, part at the origin, source = target size, full
//                                       viewport); lim = the part EASU reads (texels [0, lim)); no side effects but the shader objects
//   vkn_fsr_pre_skip(ctx)               this draw (frame, draw number) gets no pre pass: EASU reads the source (F1A's output) as recorded
struct vkn_draw_in; struct vkn_fx_ctx;
int vkn_fsr_route_ok(void);
int vkn_fsr_stretch_part(const struct vkn_draw_in *in, const struct vkn_fx_ctx *ctx, uint32_t lim[2]);
void vkn_fsr_pre_skip(const struct vkn_fx_ctx *ctx);
// Host tests: the fragment module of the pre pass is made from these words (vkn_fsr.c's own copy of ppfxaa_fsr_pre.frag.spv.inc).
const uint32_t *vkn_fsr_pre_words(uint32_t *n);

#ifdef __cplusplus
}
#endif
#endif
