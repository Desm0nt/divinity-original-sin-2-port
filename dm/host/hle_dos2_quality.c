// [DOS2] Render quality knobs (DM_DOS2_DRS / _TARGET_MS / DM_DOS2_SHADOWS / DM_DOS2_AA / DM_DOS2_THERMAL / DM_DOS2_PARTICLES_FULL; docs/dos2/DOS2_PORT.md "Качество рендера и DRS",
// plan docs/dos2/NATIVE_PLAN.md section 6). main = main.raw of update v786432, build 4979b200 (offsets = main.raw, as hle_dos2_wide.c).
//
// Dynamic resolution (DRS, a Switch-build addition; no strings name it). One object, 0x50 bytes, vtable main+0x299b938, pointer at
// main+0x2bbb198 (made by 0x8a9a40 -> ctor 0xb21800, defaults 1920x1080 / min 1440x810):
//   +0x08/+0x0c int  max w/h = the window      +0x10/+0x14 int  min w/h          +0x18/+0x1c float  step up (w, h x 0.0125)
//   +0x20/+0x24 float step down (x 0.025)      +0x28/+0x2c float current w/h    +0x30/+0x34 float  scale = current / max (what the
//   renderer reads: 0xab2118, 0xab322c, 0x911074)  +0x38 int frames since a change (evaluates from 4)  +0x3c int downscale count
//   +0x40/+0x41 byte handheld / CPU-boost flags (apm config 0x92220008 <-> 0x20004)   +0x44 float ms of headroom gathered for a step up
//   +0x48 byte off (set on an operation-mode change 0x8a7718 and by 0x158cde8 while loading; cleared 0x8a774c)
// SetSize 0xb21880 (drs, w, h; from 0xa6a770 with the window size): max = w x h, min = (int)(w x 0.75) x (int)(h x 0.75)
//   (fmov s2,#0.75 at 0xb21890), current = max, scale 1.
// Controller, per frame in 0xc05c80 (gate [[main+0x2bb8a60]+0x6a0] == 2, then +0x48 == 0, +0x38 >= 4) at 0xc05da8..0xc06620:
//   GPU ms = [[main+0x2bbdc68]+0x18f0] (written 0x921780: two nvnCommandBufferReportCounter TIMESTAMPs of the frame, / 1e6); target
//   31.5 ms (main+0x1f1237c). GPU > 31.0 ms -> step down by +0x20/+0x24 until the model (cost ~ 0.5 + 0.5 x area / max area) fits,
//   not below min; predicted cost of one step up < 30.0 ms (fmov at 0xc05e48) for 1000 ms in a row -> one step up, clamped to
//   s7 / s2 = max w / h (0xc05e18..0xc05e24 fcmp/fcsel, stored by 0xc06458..0xc06480). The host's NVN reports real GPU timestamps.
// Fixed quality: graphicSettings.lsx (0xa687e0) only loads gamma / HDR; 0xa6d400 then writes the console's fixed values into the
//   GraphicSettings object (0x88 bytes, ctor 0xa46d00, vtable main+0x29946e8, pointer main+0x2bc0278); names from the PC key table
//   0x8b3d00 and the readers:
//   +0x08 Fullscreen 1  +0x20 VSync 1  +0x24 VSyncDivider 2 (30 fps)  +0x28 TripleBuffering 1  +0x34 TextureDetail 0  +0x38 TextureFiltering 1
//   +0x3c TextureStreaming 1  +0x40 ModelDetail 2  +0x48 LightingDetail 2  +0x4d 1 / +0x4e 0  +0x50 ShadowQuality 1  +0x64 AntiAliasing 1
//   (0xab6d4c: FXAA, PPFXAA.bshd vs PPDefault.bshd)  +0x68 Bloom 1  +0x69 DOF 0  +0x6a GodRays 0  +0x6b LensFlare 0  +0x6c MotionBlur 0
//   +0x6d SSAO 0  +0x71 0  +0x72 u16 30. Ctor (PC) defaults: ShadowQuality 0, +0x6e 0, +0x6f 1, +0x70 1, +0x71 0.
// ShadowQuality (+0x50, PROVEN): the directional ShadowStage (0xb15960 init / 0xb16a58 re-init) takes q = min(+0x50, 2) and the tables
//   main+0x1f58d92 u16 {1024, 768, 512} = the cascade map size (atlas allocated at size x 2: 0xb16738 lsl) and main+0x1f58d98 u32 {2, 3, 3}
//   (stage +0x340 / +0x2e0). Every frame 0xb16604..0xb16614 compares the stage's q (+0x2f0) with +0x50 and rebuilds the shadow maps when
//   they differ (0xb16714): a run-time change is the game's own path (the PC options menu).
// Bloom (+0x68): read every frame by the post step's enable (0xab4680: step +0x30 = camera +0x2a9 && Bloom && step +0x80; also 0xab50b0),
//   so flipping it at run time only skips / resumes the pass. +0x6f (PC default 1): read every frame by 0xa69410 (called from 0xc072a0)
//   and by 0xa4cee0 / 0xa4d1b0 (with a light's +0x50 >= 2); its order in the key table says ShadowsEnabled but no reader names shadows,
//   so it is NOT exposed. +0x70 has no reader found; +0x71 has many (0xcb8f4c.., 0xd031d0..), meaning unknown.
// AntiAliasing (+0x64, PROVEN statically, NB-18b-1): the post step "FXAA" (0x50 bytes, vtable main+0x2998280; built by 0xab13e4 in the
//   post chain 0xab1xxx: [.., LUT 0xab54e0, FXAA, Vignette vtable main+0x29982f8], linked by vt+0x48 next / vt+0x40 prev; EoCSight
//   0x1424494 sits between LUT and FXAA in every P2 frame). Load vt+0x10 0xab6d30: +0x38 = PPFXAA.bshd (0xab6d48), +0x40 = PPDefault.bshd
//   (0xab6d68), +0x48 = both loaded. Enable vt+0x20 0xab6e10 (per frame): on (+0x30 = +0x48) when the float [[[ctx+0x10]+0x50]+0x2c] != 1.0
//   (render scale; DRS 0.9 in P2) OR +0x64 == 1 (0xab6e34), else off; then the next step's enable. Execute vt+0x38 0xab6e90: off -> the
//   next step's execute; on and +0x64 != 1 (0xab6ec8) and the next step "renders" (vt+0x50 0xab4130: its +0x30 or any later one) -> tail
//   call of the next step's execute with the same arguments = NO pass of its own (Vignette reads Sight's output); otherwise its own
//   full-screen pass with +0x64 ? PPFXAA : PPDefault (0xab6f80). So at 0: Vignette frames -> the FXAA pass is gone; no Vignette (menu,
//   light frames) at scale != 1 -> a PPDefault copy pass (fs 7b3210191abd); at scale 1 -> gone. Other writers of +0x64: 0xc099c8 (an
//   options-map apply, vtable slot main+0x29a6600 -> 0xc08630: value = render config [[main+0x2bb8a60]+0x628] or the map's key
//   [main+0x32be528]; also stores cfg+0x628) and 0x113f848 (UI sync: writes when its byte +0x1d9 != cfg+0x628). Both can bring 1 back.
//   DM_DOS2_AA=0 patches 0xa6d434 str w9,[x8,#0x64] -> str wzr and the monitor thread clears +0x64 if it reads non-zero (logged);
//   DM_DOS2_AA=1 / unset = the console's FXAA. cfg+0x628 is left alone (keeps the UI sync quiet).
//
//   DM_DOS2_DRS unset / 0      the game's DRS (min 75 %, max 100 % of the window)
//   DM_DOS2_DRS=off / 100      no DRS: the controller's evaluation skipped (0xc05db4 cbnz -> b 0xc05ef8): always the full window
//   DM_DOS2_DRS=<min>:<max>    min in % of the window (50..99; the fmov immediate, nearest 1/32), max = a ceiling in % (50..100); min >= 100 = off
//   DM_DOS2_DRS_TARGET_MS      GPU target of the controller. Unset = the host's frame budget: present period (hle_nvn.c
//                              g_nvn_present_period_ns) x 0.95, followed at 4 Hz (~15.8 ms at 60 Hz); <ms> = fixed (4..100); 0 = the
//                              game's 31.5 ms. The step-up threshold = target x 30 / 31.5. The loads point at a data page of ours
//                              (0xc05e04 adrp + 0xc05e08 ldr s18 -> [page+0]; 0xc05e48 fmov s5,#30 -> ldr s5,[x11,#4]); main+0x1f1237c untouched.
//   Ceiling (DM_DOS2_DRS max, the thermal ladder): a code cave. 0xc05e18..0xc05e24 (fcmp / fcsel / fcmp / fcsel, no branch lands inside) ->
//                              ldr x16,#8; br x16; .quad cave. The cave scales the clamp limits s7 / s2 (max w / h) by page[2]
//                              (ldr s22: x16 / v22 are unused in the whole controller), runs the four instructions and jumps back to
//                              0xc05e28. The step up then stops at the ceiling, and when the ceiling drops below the current size the next
//                              "step up" (predicted cheaper) lands on it. No writes into the game's object.
//   DM_DOS2_AA=0/1             AntiAliasing at boot: 0 = no FXAA (see above: pass gone or a PPDefault copy), 1 = the console's FXAA
//                              (0xa6d434 str w9 -> str wzr; readers 0xab6e34 / 0xab6ec8 / 0xab6f80 word-checked); unset = the console's
//   DM_DOS2_SHADOWS=0/1/2      ShadowQuality at boot: 0 = 1024 x 2 (PC default), 1 = 768 x 3 (the console's), 2 = 512 x 3 (0xa6d444
//                              str w9,[x8,#0x50] -> str wzr / str w10; unset = the console's)
//   DM_DOS2_THERMAL=1          thermal ladder (NATIVE_PLAN.md section 6): AThermal headroom (perf_android.c dm_thermal_headroom, 10 s
//                              forecast) at 1 Hz, EMA of 3; >= 0.7 -> ceiling 85 %; >= 0.8 -> 75 % + ShadowQuality 2; >= 0.9 -> 65 % + Bloom
//                              off (the ceiling not below the DRS min). A stage up after 2 polls over its threshold, down after 20 s under it
//                              - 0.1, one change per 10 s at most; each change logged; going down restores the values it found. Default 0.
//   DM_DOS2_DRS_LOG=1          one line every 10 s with the current size, scale and GPU ms (the first sighting is always logged)
// Offscreen particles (docs/dos2/DOS2_PORT.md "Частицы: полуразрешённая offscreen-цепочка"): the byte [[main+0x2bb8a60]+0xc7] (the
//   render config global that also holds the DRS gate +0x6a0; NOT in GraphicSettings) is the engine's own switch. Written only at level
//   load 0x104cd4c (strb w8 = level != [[client+0x70]+8], "ClientLoadLevel") and cleared on the menu 0xd3e268 ("LoadMenu"). Readers:
//   the cull 0xb0c9b4 (an alpha object with flags & 0xa00 = particles -> CullStage::m_OffscreenAlphaObjects / ..PostRefraction.. lists
//   instead of m_AlphaObjects), 4 more visitors with the same 0xa00 test (0xb7e0dc, 0xc6cd04, 0xc7f700, 0xc82e5c), and the alpha stage
//   (vtable main+0x299b6c0, id 13, Init 0xb1d290, Execute 0xb1d6a0): at 0xb1ddc4 it skips, when 0, the whole chain = DownsampleDepthMax
//   (stage +0xd0) into HalfResDepth (+0xc0, 0.5x), LinearizeDepth (+0xe0 / +0xe8) into HalfResLinearDepth (+0xc8, 0.5x), clear + the
//   offscreen lists (renderer +0x24c0 / +0x2c80) into OffscreenParticlesBuffer (+0xb8, 0.5x) and CrossBilateralUpsample (+0xd8) back
//   into the HDR target. With it 0 the particles are drawn with the other alpha objects, full resolution, in the main pass (the iPad
//   build has no offscreen lists at all). The half-res targets are still allocated by Init (memory only).
//   DM_DOS2_PARTICLES_FULL=1   0x104cd4c strb w8 -> strb wzr (the flag stays 0 in every level); the monitor thread logs the flag and
//                              clears it if it ever reads non-zero. Default 0 (the console's chain) until phone-verified.
// dm_dos2_drs_set_ceiling(pct): the run-time ceiling; the effective one = min(DM_DOS2_DRS max, it).
// All code sites are checked word for word: another build changes nothing. The monitor thread only reads the DRS object (log, ladder floor).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { DQ_SETSIZE = 0xb21880, DQ_GATE = 0xc05db0, DQ_FIXED = 0xa6d400, DQ_DRS_PTR = 0x2bbb198, DQ_DRS_VT = 0x299b938,
       DQ_RENDERER_PTR = 0x2bbdc68, DQ_GPU_MS = 0x18f0, DQ_GS_PTR = 0x2bc0278, DQ_GS_VT = 0x29946e8,
       DQ_TGT_ADRP = 0xc05e04, DQ_THR_FMOV = 0xc05e48, DQ_CLAMP = 0xc05e18, DQ_SHQ_STR = 0xa6d444,
       DQ_AA_STR = 0xa6d434, DQ_AA_EN = 0xab6e34, DQ_AA_EX = 0xab6ec8, DQ_AA_SH = 0xab6f80, GS_AA = 0x64,
       GS_SHADOWQ = 0x50, GS_BLOOM = 0x68,
       DQ_CFG_PTR = 0x2bb8a60, CFG_OFFSCREEN_PT = 0xc7, DQ_PT_STORE = 0x104cd3c, DQ_PT_EXEC = 0xb1ddbc, DQ_PT_CULL = 0xb0c9b4,
       DQ_PT_MENU = 0xd3e268 };
static const uint32_t k_pt_store[5] = {0x9000db69 /* adrp x9,0x2bb8000 */, 0xf9453129 /* ldr x9,[x9,#0xa60] */, 0x1a9f07e8 /* cset w8,ne */,
                                       0x32000fe2 /* mov w2,#0xf */, 0x39031d28 /* +0x10 strb w8,[x9,#0xc7] */};
static const uint32_t k_pt_exec[4] = {0xf00104c8 /* adrp x8,0x2bb8000 */, 0xf9453108 /* ldr x8,[x8,#0xa60] */, 0x39431d08 /* ldrb w8,[x8,#0xc7] */,
                                      0x34002fc8 /* cbz w8,0xb1e3c0 */};
static const uint32_t k_pt_cull[4] = {0x90010568 /* adrp x8,0x2bb8000 */, 0xf9453108, 0x39431d08, 0x34000268 /* cbz w8,0xb0ca0c */};
static const uint32_t k_pt_menu = 0x39031d1f;   // strb wzr,[x8,#0xc7]
static const uint32_t k_setsize[6] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x1e220020 /* scvtf s0,w1 */, 0x1e2d1002 /* +0x10 fmov s2,#0.75 */,
                                      0x1e220041};
static const uint32_t k_gate[8] = {0x39412288 /* ldrb w8,[x20,#0x48] */, 0x35000a28 /* +4 cbnz w8,0xc05ef8 */, 0xb9403a88, 0x7100111f,
                                   0x540008a3, 0x9000fdc9, 0xf9463529, 0xbd58f130 /* ldr s16,[x9,#0x18f0] */};
static const uint32_t k_fixed[22] = {0xf0010a88, 0xf9413d08, 0xb26003eb, 0x320003e9, 0x321f03ea /* mov w10,#2 */, 0xf803410b, 0x321f0feb,
                                     0x79001109, 0x39008109, 0xb900250a, 0x3900a109, 0x7900d109, 0xb806a11f, 0xb9006509 /* +0x34 str w9,[x8,#0x64] */, 0x3901c51f,
                                     0x7804d109, 0x3900f109, 0xb9005109 /* +0x44 str w9,[x8,#0x50] */, 0x7900e50b, 0xb900410a, 0xb900490a,
                                     0xd65f03c0};
static const uint32_t k_tgt[3] = {0x1e220165 /* scvtf s5,w11 */, 0xb000986b /* adrp x11,0x1f12000 */, 0xbd437d72 /* ldr s18,[x11,#0x37c] */};
static const uint32_t k_thr[2] = {0x1e2328a3 /* fadd s3,s5,s3 */, 0x1e27d005 /* fmov s5,#30.0 */};
static const uint32_t k_clamp[4] = {0x1e272220 /* fcmp s17,s7 */, 0x1e274e33 /* fcsel s19,s17,s7,mi */, 0x1e222080 /* fcmp s4,s2 */,
                                    0x1e224c94 /* fcsel s20,s4,s2,mi */};

static const uint32_t k_aa_rd = 0xb9406508;   // ldr w8,[x8,#0x64] at each FXAA-step reader (0xab6e34 / 0xab6ec8 / 0xab6f80)

static module_t *g_main;
static int g_user_pct = 100;           // DM_DOS2_DRS max
static _Atomic int g_ceil_pct = 100;   // run-time ceiling (the ladder's), 50..100, 100 = no ceiling
static int g_drs_log, g_tgt_mode, g_thermal, g_cave;   // g_tgt_mode: 0 the game's, 1 the host's frame budget, 2 fixed
static int g_pt_full;                  // DM_DOS2_PARTICLES_FULL=1 and the store patch is in
static int g_aa_off;                   // DM_DOS2_AA=0 and the store patch is in
static volatile float *g_q;            // our data page: [0] target ms, [1] step-up threshold ms, [2] ceiling factor (1 = none)
extern _Atomic uint64_t g_nvn_present_period_ns;
float dm_thermal_headroom(void) __attribute__((weak));

static int dq_poke(module_t *m, uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    size_t len = (size_t)(((uintptr_t)(at + n) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, w, 4 * (size_t)n);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[quality] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + n));
    return 0;
}
static int dq_poke1(module_t *m, uint32_t off, uint32_t w) { return dq_poke(m, off, &w, 1); }
// FMOV (scalar, immediate) single: value of imm8 (VFPExpandImm)
static float dq_imm8(unsigned i) {
    uint32_t a = i >> 7 & 1, b = i >> 6 & 1, cd = i >> 4 & 3, f = i & 15;
    uint32_t bits = a << 31 | (b ^ 1) << 30 | (b ? 0x1fu << 25 : 0) | cd << 23 | f << 19; float v; memcpy(&v, &bits, 4); return v;
}
static unsigned dq_imm8_near(float want, float *got) {
    unsigned best = 0x68; float bd = 1e9f;
    for (unsigned i = 0; i < 256; i++) { float v = dq_imm8(i); if (v > 0 && fabsf(v - want) < bd) { bd = fabsf(v - want); best = i; } }
    *got = dq_imm8(best); return best;
}

typedef struct { void *vt; int32_t max_w, max_h, min_w, min_h; float up_w, up_h, dn_w, dn_h, cur_w, cur_h, sc_w, sc_h;
                 int32_t frames, downs; uint8_t handheld, boost, pad[2]; float up_ms; uint8_t off; } dq_drs_t;   // 0x50 bytes in the game
_Static_assert(__builtin_offsetof(dq_drs_t, cur_w) == 0x28 && __builtin_offsetof(dq_drs_t, up_ms) == 0x44 &&
               __builtin_offsetof(dq_drs_t, off) == 0x48, "DRS layout");

static const dq_drs_t *dq_drs(void) {
    const dq_drs_t *d = *(const dq_drs_t *volatile *)(g_main->base + DQ_DRS_PTR);
    if (!d || (const uint8_t *)d->vt != g_main->base + DQ_DRS_VT) return NULL;
    return d;
}
static volatile uint8_t *dq_gs(void) {   // GraphicSettings, checked by its vtable
    volatile uint8_t *g = *(volatile uint8_t *volatile *)(g_main->base + DQ_GS_PTR);
    if (!g || *(uint8_t *volatile *)g != g_main->base + DQ_GS_VT) return NULL;
    return g;
}
__attribute__((weak)) double xbg_recent_gpu_ms(void);   // the Vulkan backend's GPU ms a frame over its last report window (-1: none yet)
static float dq_gpu_ms(void) {
    uint8_t *r = *(uint8_t *volatile *)(g_main->base + DQ_RENDERER_PTR);
    return r ? *(volatile float *)(r + DQ_GPU_MS) : NAN;
}

void dm_dos2_drs_set_ceiling(int pct);   // the run-time ceiling (the thermal ladder's): 50..100 % of the window height
void dm_dos2_drs_set_ceiling(int pct) { atomic_store(&g_ceil_pct, pct < 50 ? 50 : pct > 100 ? 100 : pct); }
static int dq_eff_pct(void) { int p = atomic_load(&g_ceil_pct); return g_user_pct < p ? g_user_pct : p; }

static void dq_set_target(float ms) { g_q[1] = ms * 30.0f / 31.5f; g_q[0] = ms; }

// Thermal ladder (1 Hz): stage 1 DRS ceiling 85 %, 2 + 75 % and ShadowQuality 2, 3 + 65 % and Bloom off
static void dq_ladder(const dq_drs_t *d) {
    static const float thr[4] = {0, 0.7f, 0.8f, 0.9f}; static const int ceil_of[4] = {100, 85, 75, 65};
    static int stage, over, idle_logged, saved_shq = -1, saved_bloom = -1; static float ema = NAN; static uint64_t last_change, under_since;
    if (!dm_thermal_headroom) { if (!idle_logged) { idle_logged = 1; LOG("[quality] DM_DOS2_THERMAL: no thermal headroom on this host: ladder idle"); } return; }
    float h = dm_thermal_headroom();
    if (h != h) return;   // NaN: unknown or asked too often
    ema = ema == ema ? ema + (h - ema) / 3 : h;
    uint64_t t = now_ns(); int want = stage;
    if (stage < 3 && ema >= thr[stage + 1]) { if (++over >= 2) want = stage + 1; } else over = 0;
    if (stage > 0 && ema < thr[stage] - 0.1f) { if (!under_since) under_since = t; if (t - under_since >= 20000000000ull) want = stage - 1; }
    else under_since = 0;
    if (want == stage || (last_change && t - last_change < 10000000000ull)) return;
    volatile uint8_t *gs = dq_gs();
    int floor_pct = d->max_h > 0 ? (d->min_h * 100 + d->max_h - 1) / d->max_h : 50, c = ceil_of[want] < floor_pct ? floor_pct : ceil_of[want];
    char extra[96] = "";
    if (gs) {
        volatile int32_t *shq = (volatile int32_t *)(gs + GS_SHADOWQ); volatile uint8_t *bloom = gs + GS_BLOOM;
        if (want >= 2 && saved_shq < 0) { saved_shq = *shq; if (*shq < 2) *shq = 2; }
        if (want < 2 && saved_shq >= 0) { *shq = saved_shq; saved_shq = -1; }
        if (want >= 3 && saved_bloom < 0) { saved_bloom = *bloom; *bloom = 0; }
        if (want < 3 && saved_bloom >= 0) { *bloom = (uint8_t)saved_bloom; saved_bloom = -1; }
        snprintf(extra, sizeof extra, ", ShadowQuality %d, Bloom %d", (int)*shq, (int)*bloom);
    } else if (want >= 2) snprintf(extra, sizeof extra, ", GraphicSettings not found: shadows / bloom untouched");
    LOG("[quality] thermal ladder stage %d -> %d: headroom %.2f (EMA %.2f), DRS ceiling %d %% (min %d %%)%s, current %.0fx%.0f, GPU %.2f ms",
        stage, want, (double)h, (double)ema, c, floor_pct, extra, d->cur_w, d->cur_h, (double)dq_gpu_ms());
    stage = want; over = 0; under_since = 0; last_change = t; dm_dos2_drs_set_ceiling(c);
}

// DM_DOS2_PARTICLES_FULL: the offscreen-particles flag, logged on a change; cleared if anything still sets it (a missed writer)
static void dq_pt_watch(void) {
    static int last = -1, cleared;
    volatile uint8_t *cfg = *(volatile uint8_t *volatile *)(g_main->base + DQ_CFG_PTR);
    if (!cfg) return;
    int v = cfg[CFG_OFFSCREEN_PT];
    if (v && cleared < 8) {
        cfg[CFG_OFFSCREEN_PT] = 0; cleared++;
        LOG("【红】[quality] offscreen-particles flag [main+%#x]+%#x read %d although the level-load store is patched: cleared (%d)", DQ_CFG_PTR,
            CFG_OFFSCREEN_PT, v, cleared);
        v = 0;
    }
    if (v != last) {
        last = v;
        LOG("[quality] offscreen-particles flag [main+%#x]+%#x = %d: particles %s", DQ_CFG_PTR, CFG_OFFSCREEN_PT, v,
            v ? "in the half-resolution offscreen chain" : "full resolution in the main alpha pass");
    }
}

// DM_DOS2_AA=0: GraphicSettings +0x64, cleared if a later writer (0xc099c8 options apply / 0x113f848 UI sync) brings FXAA back
static void dq_aa_watch(void) {
    static int last = -1, cleared;
    volatile uint8_t *gs = dq_gs();
    if (!gs) return;
    volatile int32_t *aa = (volatile int32_t *)(gs + GS_AA);
    int v = *aa;
    if (v && cleared < 8) {
        *aa = 0; cleared++;
        volatile uint8_t *cfg = *(volatile uint8_t *volatile *)(g_main->base + DQ_CFG_PTR);
        LOG("【红】[quality] AntiAliasing (GraphicSettings +%#x) read %d although the boot store is patched (render config +0x628 = %d): "
            "cleared (%d)", GS_AA, v, cfg ? (int)cfg[0x628] : -1, cleared);
        v = 0;
    }
    if (v != last) { last = v; LOG("[quality] AntiAliasing = %d: %s", v, v ? "FXAA" : "no FXAA (pass gone, or a PPDefault copy when nothing follows it)"); }
}

static void *dq_monitor(void *arg) {
    (void)arg;
    int seen = 0, tick = 0, pct_logged = 100; uint64_t next_log = 0; float tgt_logged = 0;
    int size_w = 0, size_h = 0, insane = 0; uint64_t size_at = 0;   // NB-18b-11: the size the game set (SetSize), reported once it settled
    // NB-18b-12: every 10 s the game's GPU frame time (renderer +0x18f0, sampled at 4 Hz: what its dynamic-resolution controller reads)
    // next to the backend's own (xbg_recent_gpu_ms = the "NVN PERF gpu_ms" of its last 60 frames) - one run proves they agree.
    double gs_sum = 0, gs_min = 1e30, gs_max = 0, hs_sum = 0; int gs_n = 0, hs_n = 0; uint64_t gs_next = 0;
    for (;;) {
        usleep(250 * 1000);
        if (g_tgt_mode == 1) {   // the host's frame budget
            uint64_t p = atomic_load(&g_nvn_present_period_ns);
            if (p) {
                float ms = (float)(p / 1e6 * 0.95); if (ms < 4) ms = 4; if (ms > 100) ms = 100;
                if (fabsf(ms - g_q[0]) > 0.01f) dq_set_target(ms);
                if (fabsf(ms - tgt_logged) > 0.2f) {
                    tgt_logged = ms;
                    LOG("[quality] DRS target %.2f ms GPU (present period %.2f ms x 0.95), step up under %.2f ms", (double)ms, p / 1e6, (double)g_q[1]);
                }
            }
        }
        if (g_pt_full) dq_pt_watch();
        if (g_aa_off) dq_aa_watch();
        const dq_drs_t *d = dq_drs(); if (!d) continue;
        if (!seen) {
            seen = 1;
            LOG("[quality] DRS object up (main+%#x): max %dx%d, min %dx%d, step up %.1fx%.1f / down %.1fx%.1f px, current %.0fx%.0f, "
                "%s; target %.2f ms GPU, step up after 1 s under %.2f ms; GPU now %.2f ms", DQ_DRS_PTR, d->max_w, d->max_h, d->min_w, d->min_h,
                d->up_w, d->up_h, d->dn_w, d->dn_h, d->cur_w, d->cur_h, d->off ? "off" : "on", g_q ? (double)g_q[0] : 31.5,
                g_q ? (double)g_q[1] : 30.0, (double)dq_gpu_ms());
        }
        // NB-18b-11: always in the log (not only with DM_DOS2_DRS_LOG): the maximum the game set for the window (the first sighting above is
        // the constructor's 1920x1080), 8 s later what the controller made of it, and a red line when the GPU frame time it reads is
        // not a frame time (then it sits at the minimum: the TIMESTAMP_TOP bug, xbg_game.cpp OP_REPORT_COUNTER).
        if (d->max_w != size_w || d->max_h != size_h) { size_w = d->max_w; size_h = d->max_h; size_at = now_ns() + 8000000000ull; }
        else if (size_at && now_ns() >= size_at) {
            size_at = 0;
            LOG("[quality] DRS size: max %dx%d (the 3D targets), min %dx%d, current %.0fx%.0f = %.1f %% of the max, GPU %.2f ms%s", d->max_w, d->max_h,
                d->min_w, d->min_h, d->cur_w, d->cur_h, d->max_h ? 100.0 * d->cur_h / d->max_h : 0.0, (double)dq_gpu_ms(), d->off ? ", off" : "");
        }
        { const float g = dq_gpu_ms();
          if (g == g && (g > 2000.f || g < 0.f) && insane < 3 && seen) { insane++;
              LOG("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91[quality] the game's GPU frame time reads %.2f ms: not a frame time - its dynamic resolution will sit at the minimum "
                  "(current %.0fx%.0f of %dx%d)", (double)g, d->cur_w, d->cur_h, d->max_w, d->max_h); } }
        { const float g = dq_gpu_ms(); const double h = xbg_recent_gpu_ms ? xbg_recent_gpu_ms() : -1; const uint64_t t = now_ns();
          if (g == g && g >= 0.f && g < 2000.f) { gs_sum += g; gs_n++; if (g < gs_min) gs_min = g; if (g > gs_max) gs_max = g; }
          if (h >= 0) { hs_sum += h; hs_n++; }
          if (!gs_next) gs_next = t + 10000000000ull;
          else if (t >= gs_next) {
              gs_next = t + 10000000000ull;
              if (gs_n) LOG("[quality] GPU frame time (10 s): the game reads %.2f ms (min %.2f, max %.2f, %d samples), the backend measured %.2f ms; DRS current %.0fx%.0f = "
                            "%.1f %% of %dx%d, target %.2f ms, step up under %.2f ms%s", gs_sum / gs_n, gs_min, gs_max, gs_n, hs_n ? hs_sum / hs_n : -1.0, d->cur_w, d->cur_h,
                            d->max_h ? 100.0 * d->cur_h / d->max_h : 0.0, d->max_w, d->max_h, g_q ? (double)g_q[0] : 31.5, g_q ? (double)g_q[1] : 30.0, d->off ? ", off" : "");
              gs_sum = hs_sum = 0; gs_n = hs_n = 0; gs_min = 1e30; gs_max = 0;
          } }
        if (g_thermal && ++tick % 4 == 0) dq_ladder(d);
        int pct = dq_eff_pct();
        if (g_cave) {
            float f = pct / 100.0f; if (g_q[2] != f) g_q[2] = f;
            if (pct != pct_logged && g_drs_log) { pct_logged = pct; LOG("[quality] DRS ceiling %d %% (cave factor %.2f)", pct, (double)f); }
        }
        if (g_drs_log) {
            uint64_t t = now_ns();
            if (t >= next_log) {
                next_log = t + 10000000000ull;
                LOG("[quality] DRS %.0fx%.0f of %dx%d (scale %.3f), min %dx%d, ceiling %d %%, GPU %.2f ms%s", d->cur_w, d->cur_h, d->max_w,
                    d->max_h, (double)d->sc_h, d->min_w, d->min_h, pct, (double)dq_gpu_ms(), d->off ? ", off" : "");
            }
        }
    }
    return NULL;
}

// The data page within adrp range of the controller (just past the module, else anywhere in range; NULL = none)
static void *dq_data_page(module_t *m, int64_t *dp_out) {
    uint8_t *pc = m->base + DQ_TGT_ADRP;
    // [DOS2] a 16 KB page within adrp reach (+-4 GB) of the site: right after main first, then a scan of hints 64 MB apart on both sides
    // (one plain hint failed now and then, 14.11: the kernel put the page elsewhere and the target / live DRS stayed the game's)
    uintptr_t base = ((uintptr_t)m->base + m->size + 0x3FFF) & ~(uintptr_t)0x3FFF;
    for (int i = 0; i < 128; i++) {
        intptr_t step = (intptr_t)(i + 1) / 2 * (intptr_t)0x4000000;   // 0, +64 MB, -64 MB, +128 MB, ...
        uintptr_t h = i == 0 ? base : (i & 1) ? base + (uintptr_t)step : base - (uintptr_t)step;
        void *pg = mmap((void *)h, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (pg == MAP_FAILED) continue;
        int64_t dp = ((int64_t)(uintptr_t)pg >> 12) - ((int64_t)(uintptr_t)pc >> 12);
        if (dp >= -(1 << 20) && dp < (1 << 20)) { *dp_out = dp; if (i) LOG("[quality] data page at %p (hint %d)", pg, i); return pg; }
        munmap(pg, 0x4000);
    }
    return NULL;
}
// The ceiling cave: s7 / s2 x page[2], the four clamp instructions, back to 0xc05e28
static int dq_cave_install(module_t *m) {
    if (memcmp(m->base + DQ_CLAMP, k_clamp, sizeof k_clamp)) { LOG("【红】[quality] main+%#x differs: no DRS ceiling", DQ_CLAMP); return -1; }
    uint32_t *c = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (c == MAP_FAILED) { LOG("【红】[quality] mmap failed: no DRS ceiling"); return -1; }
    uint64_t q = (uint64_t)(uintptr_t)g_q, back = (uint64_t)(uintptr_t)(m->base + DQ_CLAMP + 16);
    const uint32_t w[14] = {0x58000150 /* ldr x16,#40 (q) */, 0xbd400a16 /* ldr s22,[x16,#8] */, 0x1e3608e7 /* fmul s7,s7,s22 */,
                            0x1e360842 /* fmul s2,s2,s22 */, k_clamp[0], k_clamp[1], k_clamp[2], k_clamp[3],
                            0x58000090 /* ldr x16,#16 (back) */, 0xd61f0200 /* br x16 */,
                            (uint32_t)q, (uint32_t)(q >> 32), (uint32_t)back, (uint32_t)(back >> 32)};
    memcpy(c, w, sizeof w);
    if (mprotect(c, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[quality] mprotect RX failed: no DRS ceiling"); munmap(c, 0x4000); return -1; }
    __builtin___clear_cache((char *)c, (char *)(c + 14));
    uint64_t to = (uint64_t)(uintptr_t)c;
    const uint32_t j[4] = {0x58000050 /* ldr x16,#8 */, 0xd61f0200 /* br x16 */, (uint32_t)to, (uint32_t)(to >> 32)};
    if (dq_poke(m, DQ_CLAMP, j, 4)) { LOG("【红】[quality] mprotect RW failed: no DRS ceiling"); return -1; }
    LOG("[quality] DRS ceiling: code cave at %p (main+%#x..+%#x fcmp/fcsel -> jump; clamp limits x the factor at %p)", (void *)c, DQ_CLAMP,
        DQ_CLAMP + 12, (void *)(g_q + 2));
    return 0;
}

void dm_dos2_quality_install(void);   // main.c host_preload, right after load_modules (before any guest code runs)
void dm_dos2_quality_install(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    if (!g_main) { LOG("【红】[quality] no main module: render settings untouched"); return; }
    module_t *m = g_main;
    int ok_size = m->text_end >= DQ_GATE + sizeof k_gate && !memcmp(m->base + DQ_SETSIZE, k_setsize, sizeof k_setsize);
    int ok_gate = ok_size && !memcmp(m->base + DQ_GATE, k_gate, sizeof k_gate);
    int ok_fixed = m->text_end >= DQ_FIXED + sizeof k_fixed && !memcmp(m->base + DQ_FIXED, k_fixed, sizeof k_fixed);
    if (!ok_size || !ok_gate) { LOG("【红】[quality] main+%#x / +%#x differ: not DOS2 build 4979b200: render settings untouched", DQ_SETSIZE, DQ_GATE); g_main = NULL; return; }
    if (ok_fixed)
        LOG("[quality] console fixed settings (main+%#x -> GraphicSettings at [main+%#x]): VSync 1, VSyncDivider 2 (30 fps), TripleBuffering 1, "
            "TextureDetail 0, TextureFiltering 1, TextureStreaming 1, ModelDetail 2, LightingDetail 2, ShadowQuality 1 (768 x 3), AntiAliasing 1 "
            "(FXAA), Bloom 1, DOF 0, GodRays 0, LensFlare 0, MotionBlur 0, SSAO 0, +0x72 = 30", DQ_FIXED, DQ_GS_PTR);
    else LOG("[quality] main+%#x differs: console fixed settings not decoded, DM_DOS2_SHADOWS unavailable", DQ_FIXED);
    LOG("[quality] game DRS: min 75 %% of the window (main+%#x), max 100 %%, steps 1.25 %% up / 2.5 %% down, target 31.5 ms GPU",
        DQ_SETSIZE + 0x10);

    // Shadows at boot
    const char *se = getenv("DM_DOS2_SHADOWS");
    if (se && *se && ok_fixed) {
        int v = atoi(se);
        static const char *nm[3] = {"1024 x 2 (PC default)", "768 x 3 (the console's)", "512 x 3"};
        if (v < 0 || v > 2 || se[1]) LOG("[quality] DM_DOS2_SHADOWS=%s not understood (0 / 1 / 2): the console's ShadowQuality 1", se);
        else if (v != 1 && dq_poke1(m, DQ_SHQ_STR, v == 0 ? 0xb900511f /* str wzr,[x8,#0x50] */ : 0xb900510a /* str w10(=2),[x8,#0x50] */))
            LOG("【红】[quality] mprotect RW failed: ShadowQuality stays 1");
        else LOG("[quality] DM_DOS2_SHADOWS=%d: ShadowQuality %d = cascade maps %s (main+%#x)", v, v, nm[v], DQ_SHQ_STR);
    }

    // Anti-aliasing at boot
    const char *ae = getenv("DM_DOS2_AA");
    if (ae && *ae && ok_fixed) {
        int ok_rd = m->text_end >= DQ_AA_SH + 4 && !memcmp(m->base + DQ_AA_EN, &k_aa_rd, 4) && !memcmp(m->base + DQ_AA_EX, &k_aa_rd, 4) &&
                    !memcmp(m->base + DQ_AA_SH, &k_aa_rd, 4);
        if ((ae[0] != '0' && ae[0] != '1') || ae[1]) LOG("[quality] DM_DOS2_AA=%s not understood (0 / 1): the console's FXAA", ae);
        else if (ae[0] == '1') LOG("[quality] DM_DOS2_AA=1: the console's FXAA (PPFXAA.bshd)");
        else if (!ok_rd) LOG("【红】[quality] DM_DOS2_AA=0: main+%#x / +%#x / +%#x differ: FXAA stays", DQ_AA_EN, DQ_AA_EX, DQ_AA_SH);
        else if (dq_poke1(m, DQ_AA_STR, 0xb900651f /* str wzr,[x8,#0x64] */)) LOG("【红】[quality] mprotect RW failed: FXAA stays");
        else {
            g_aa_off = 1;
            LOG("[quality] DM_DOS2_AA=0: AntiAliasing 0 (main+%#x str w9 -> wzr): no FXAA pass when Vignette follows, a PPDefault copy "
                "when nothing follows and the render scale != 1", DQ_AA_STR);
        }
    } else if (ae && *ae) LOG("[quality] DM_DOS2_AA=%s: console fixed settings not decoded: FXAA stays", ae);

    // Particles: the half-resolution offscreen chain (the console's) or full resolution in the main pass (the iPad's way)
    const char *pe = getenv("DM_DOS2_PARTICLES_FULL");
    if (pe && *pe == '1') {
        int ok_pt = m->text_end >= DQ_PT_STORE + sizeof k_pt_store && !memcmp(m->base + DQ_PT_STORE, k_pt_store, sizeof k_pt_store) &&
                    !memcmp(m->base + DQ_PT_EXEC, k_pt_exec, sizeof k_pt_exec) && !memcmp(m->base + DQ_PT_CULL, k_pt_cull, sizeof k_pt_cull) &&
                    !memcmp(m->base + DQ_PT_MENU, &k_pt_menu, 4);
        if (!ok_pt) LOG("【红】[quality] DM_DOS2_PARTICLES_FULL=1: main+%#x / +%#x / +%#x / +%#x differ: offscreen particles stay", DQ_PT_STORE,
                        DQ_PT_EXEC, DQ_PT_CULL, DQ_PT_MENU);
        else if (dq_poke1(m, DQ_PT_STORE + 0x10, 0x39031d3f /* strb wzr,[x9,#0xc7] */))
            LOG("【红】[quality] mprotect RW failed: offscreen particles stay");
        else {
            g_pt_full = 1;
            LOG("[quality] DM_DOS2_PARTICLES_FULL=1: particles at full resolution in the main alpha pass (main+%#x strb w8 -> wzr: "
                "[[main+%#x]+%#x] stays 0; no DownsampleDepthMax / LinearizeDepth / offscreen draws / CrossBilateralUpsample)",
                DQ_PT_STORE + 0x10, DQ_CFG_PTR, CFG_OFFSCREEN_PT);
        }
    } else LOG("[quality] particles: the console's half-resolution offscreen chain (DM_DOS2_PARTICLES_FULL=1 = full resolution, main pass)");

    const char *e = getenv("DM_DOS2_DRS"); const char *l = getenv("DM_DOS2_DRS_LOG"); g_drs_log = l && *l == '1';
    unsigned mn = 0, mx = 100; int mode = 0;   // 0 game, 1 off, 2 limits
    if (!e || !*e || (e[0] == '0' && !e[1])) mode = 0;
    else if (!strcmp(e, "skip")) mode = 1;   // the old "off": the controller skipped (its current size stays where it was: 1920x1080 from the
                                             // constructor when the window is wider -> the 3D rendered at a fraction of the window, blurred, 14.11)
    else if (!strcmp(e, "off") || !strcmp(e, "100")) mode = 3;   // [DOS2] off = the controller runs with a 100 ms target: it never steps down and
                                                                  // steps the current size up to the full window (the game's own path, 14.11)
    else if (sscanf(e, "%u:%u", &mn, &mx) == 2 || sscanf(e, "%u", &mn) == 1) {
        if (mx > 100) mx = 100;
        if (mx < 50) mx = 50;
        if (mn >= 100) mode = 1; else { if (mn < 50) mn = 50; if (mn > mx) mn = mx; mode = mn >= 100 ? 1 : 2; }
    } else LOG("[quality] DM_DOS2_DRS=%s not understood (0 / off / <min>:<max> in %%): the game's DRS", e);
    const char *th = getenv("DM_DOS2_THERMAL"); g_thermal = th && *th == '1';

    if (mode == 1) {
        if (dq_poke1(m, DQ_GATE + 4, 0x14000000u | ((0xc05ef8 - (DQ_GATE + 4)) >> 2) /* b 0xc05ef8 */)) LOG("【红】[quality] mprotect RW failed: DRS stays");
        else LOG("[quality] DM_DOS2_DRS=%s: DRS off (main+%#x cbnz -> b): the full window every frame", e, DQ_GATE + 4);
    } else if (mode == 2) {
        float got; unsigned i8 = dq_imm8_near(mn / 100.0f, &got);
        if (i8 != 0x68 && dq_poke1(m, DQ_SETSIZE + 0x10, 0x1e201002u | i8 << 13 /* fmov s2,#min */)) LOG("【红】[quality] mprotect RW failed: DRS min stays 75 %%");
        else LOG("[quality] DM_DOS2_DRS=%s: DRS min %.2f %% of the window (main+%#x fmov), ceiling %u %%", e, got * 100.0, DQ_SETSIZE + 0x10, mx);
        g_user_pct = (int)mx;
    } else if (mode == 3) LOG("[quality] DM_DOS2_DRS=%s: DRS off = the game's controller with a 100 ms GPU target (never lowers the resolution; "
                              "steps up to the full window)", e);
    else LOG("[quality] DM_DOS2_DRS unset: the game's DRS");

    if (mode != 1) {
        int64_t dp = 0; void *pg = dq_data_page(m, &dp);
        if (!pg) pg = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0), dp = INT64_MAX;   // the cave needs no range
        if (pg == MAP_FAILED || !pg) { LOG("【红】[quality] mmap failed: DRS target / ceiling stay the game's"); pg = NULL; }
        if (pg) { g_q = pg; g_q[0] = 31.5f; g_q[1] = 30.0f; g_q[2] = dq_eff_pct() / 100.0f; }
        // the GPU target: our own copy
        const char *te = getenv("DM_DOS2_DRS_TARGET_MS"); double fixed = te && *te ? atof(te) : -1;
        if (mode == 3) { fixed = 100; te = "100 (DRS off)"; }
        g_tgt_mode = !pg ? 0 : fixed == 0 ? 0 : fixed > 0 ? 2 : 1;
        if (g_tgt_mode && (memcmp(m->base + DQ_TGT_ADRP - 4, k_tgt, sizeof k_tgt) || memcmp(m->base + DQ_THR_FMOV - 4, k_thr, sizeof k_thr))) {
            LOG("【红】[quality] main+%#x / +%#x differ: the game's DRS target 31.5 ms stays", DQ_TGT_ADRP, DQ_THR_FMOV); g_tgt_mode = 0; }
        if (g_tgt_mode && dp == INT64_MAX) { LOG("【红】[quality] no data page within 4 GB of main: the game's DRS target 31.5 ms stays"); g_tgt_mode = 0; }
        if (g_tgt_mode) {
            if (g_tgt_mode == 2) { float ms = (float)fixed; if (ms < 4) ms = 4; if (ms > 100) ms = 100; dq_set_target(ms); }
            uint32_t imm = (uint32_t)dp & 0x1fffff, adrp = 0x9000000bu /* adrp x11 */ | (imm & 3) << 29 | (imm >> 2) << 5;
            const uint32_t two[2] = {adrp, 0xbd400172 /* ldr s18,[x11] */};
            if (dq_poke(m, DQ_TGT_ADRP, two, 2) || dq_poke1(m, DQ_THR_FMOV, 0xbd400565 /* ldr s5,[x11,#4] */))
                LOG("【红】[quality] mprotect RW failed part-way: the DRS target may be mixed");
            else LOG("[quality] DRS target: our copy at %p (main+%#x adrp/ldr, main+%#x fmov -> ldr): %s", pg, DQ_TGT_ADRP, DQ_THR_FMOV,
                     g_tgt_mode == 2 ? "fixed" : "the host's frame budget x 0.95 from the first present (31.5 ms until then)");
            if (g_tgt_mode == 2) LOG("[quality] DM_DOS2_DRS_TARGET_MS=%s: target %.2f ms, step up under %.2f ms", te, (double)g_q[0], (double)g_q[1]);
        } else LOG("[quality] the game's DRS target 31.5 ms (built for 30 fps)%s", fixed == 0 ? ": DM_DOS2_DRS_TARGET_MS=0" : "");
        // the ceiling cave, when a ceiling can be asked for
        if (pg && (g_user_pct < 100 || g_thermal)) g_cave = !dq_cave_install(m);
        if (g_thermal) LOG("[quality] DM_DOS2_THERMAL=1: thermal ladder on (1 Hz; headroom 0.7 / 0.8 / 0.9 -> DRS ceiling 85 / 75 / 65 %%, "
                           "+ ShadowQuality 2 at 0.8, + Bloom off at 0.9)%s", g_cave ? "" : "; no DRS ceiling (cave not in)");
    } else if (g_thermal) LOG("[quality] DM_DOS2_THERMAL=1 with DRS off: ladder without the DRS ceiling");
    const char *mon = getenv("DM_DOS2_DRS_MON");
    if (mon && *mon == '0') {
        if (g_tgt_mode == 1 || g_thermal || g_cave) LOG("[quality] DM_DOS2_DRS_MON=0: no monitor thread: no target updates, no ladder");
        return;
    }
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, dq_monitor, NULL)) LOG("[quality] monitor thread refused");
    pthread_attr_destroy(&a);
}
// [DOS2] live DRS from the gear (JNI DmActivity.nativeSetDos2Drs, no restart): on = the game's controller with the target of `fps`
// (1000 / fps x 0.95; fps <= 0 = the host's frame budget), off = the 100 ms target (never lowers, steps back up to the full window).
// Works whenever our target copy is in (g_tgt_mode != 0: every DM_DOS2_DRS mode but "skip"); returns 0 when it cannot.
int dm_dos2_drs_live(int on, int fps);
int dm_dos2_drs_live(int on, int fps) {
    if (!g_q || !g_tgt_mode) { LOG("[quality] live DRS change not possible (no target copy: DM_DOS2_DRS=skip or the patch failed)"); return 0; }
    if (!on) { g_tgt_mode = 2; dq_set_target(100.0f); }
    else if (fps > 0) { g_tgt_mode = 2; dq_set_target(1000.0f / (float)fps * 0.95f); }
    else g_tgt_mode = 1;   // the monitor thread follows the present period again
    LOG("[quality] live DRS %s: target %.2f ms%s", on ? "on" : "off", (double)g_q[0], on && fps <= 0 ? " (the host's frame budget from now on)" : "");
    return 1;
}
#endif
