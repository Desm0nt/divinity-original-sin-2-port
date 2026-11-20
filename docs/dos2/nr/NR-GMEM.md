# NR-GMEM `DM_GFX_PASS_GMEM`: our own GMEM / sysmem choice per render pass on Adreno (19.11.2026)

Tiler plan step 5 = KB PERF-X19 **T8** (owner-approved 18.11). Branch `dos2-gmem` (worktree `wt-gmem`, base `dos2-nr-all` 85d2cfa).
No phone this session. **Default 0: byte-for-byte the old command stream and the old `TU_DEBUG=sysmem`** (one compare per pass begin / end
and per draw).

## 0. Summary

| Question | Answer |
|---|---|
| Driver | The bundled default `turnip27` = Banners-Turnip v27.0.0-20261029 = **Mesa main 1c3a936** (stock, unpatched; the same commit as DOOM Eternal's `tools/turnip_build`, read from the gitlab tarball of that commit). Loaded through adrenotools (`dm/android/vk_driver.c`), `TU_DEBUG=sysmem` set there when dm/env has no `TU_DEBUG`. |
| Can stock Turnip take a per-pass choice? | **Yes, no rebuild.** `use_sysmem_rendering()` (tu_cmd_buffer.cc) is evaluated at `vkCmdEndRenderPass` and checks `rp.disable_gmem` **before** `TU_DEBUG(GMEM)`; a pipeline barrier inside the pass with a non-framebuffer stage (VERTEX -> VERTEX) sets `disable_gmem` (tu_barrier, reason "Non-framebuffer-space barrier"). So `TU_DEBUG=gmem` + that barrier on every pass we want in sysmem = exact per-pass control. |
| Eternal's mechanism | The same barrier (`xbg_gsync.h` `DM_GFX_PASS_GMEM`), but with `TU_DEBUG=` + `TU_AUTOTUNE_ALGO=prefer_gmem/profiled`: the autotuner still sends every pass with **< 5 draws to sysmem** ("Too few draws to tune", tu_autotune.cc), i.e. exactly the full-screen passes this rule wants in GMEM. That is why Eternal's 11.11 try left GMEM to 14 big passes out of 77 and gained 0. Here `TU_DEBUG=gmem` bypasses the autotuner. |
| What must go on the phone | Nothing but the APK. No driver file (the stock bundled `turnip27`). |
| Expected gain (Mi 14, ~12 ms GPU-bound) | **≈ 0 (-0.3 … +0.5 ms GPU)**, honestly uncertain. Turnip's own bandwidth model says a one-draw opaque full-screen pass is ~1.2x the bytes in GMEM (store + 10 % tile overhead vs direct writes); GMEM wins only with overdraw / blending, cleared or DONT_CARE-stored depth (NB-RP plans those on Adreno too), or depth test traffic. `DM_GFX_PASS_GMEM_BW=1` keeps only those. The ABBA decides. |
| Weak devices | Adreno only (Turnip). Mali / PowerVR are tilers anyway; Qualcomm's stock driver chooses itself (=1 does nothing there: driverID check). |

## 1. Mechanism (code)

- `dm/gfx/nr/nr_gmem.h` (pure C): the rule `nrgm_decide`, Turnip's bandwidth model `nrgm_bytes`, `nrgm_cpp`, `nrgm_draw_bytes`.
- `dm/gfx/vulkan/engine/xbg_pass_gmem.inc` (namespace `pgm`), included by `xbg_game.cpp` right after `xbg_nb_mt.inc`: the macros
  `vkCreateRenderPass` / `vkCmdBeginRenderPass` / `vkCmdEndRenderPass` of the translation unit go through `pgm` (then `nbmt`), so every pass
  the unit makes (create_pass, lazy-clear / NB twins, noload twins, NR-ONCHIP's M, the clear / copy passes) is seen: create records each
  render pass's load / store / clear bytes and colour / depth formats, begin the render area and pass, `draw()` / `nr_draw` count draws and
  their bytes per sample (`note_draw`, blend / depth / stencil state), `mark_storage_images` / storage writebacks and the depth-feedback path
  of `prepare_read` set the vetoes. At the end the rule decides; under `=1` a sysmem pass gets
  `vkCmdPipelineBarrier(VERTEX, VERTEX)` through `nbmt` right before `vkCmdEndRenderPass` (NR-SORT's queued draws are cut in before it).
- `dm/android/vk_driver.c`: `DM_GFX_PASS_GMEM=1` and no `TU_DEBUG` in dm/env -> `TU_DEBUG=gmem` (instead of sysmem); the trampoline slot
  of `vkCreateRenderPass` becomes `pgm_create_rp`, which appends one VERTEX -> VERTEX self-dependency per subpass (the barrier is then valid
  Vulkan; `tu_render_pass_add_subpass_dep` ignores self-dependencies, so nothing else changes). `dm_vk_pass_gmem_state()` = 1 only with a
  verified custom Adreno package and `TU_DEBUG` without `sysmem`. `dm_vk_pass_end_sysmem(cmd)` keeps the passes made outside xbg_game.cpp
  in sysmem: the present pass (`vulkan_present.cpp draw_pass`) and the 2D readback pass (`xbg_vulkan.cpp`).
- The engine applies only with `driverID == VK_DRIVER_ID_MESA_TURNIP` and the driver side ready; otherwise `=1` counts only (red line).
- Render-pass keys (pipelines, warm list, NR-31 seed: `pass_deps`) are unchanged: the self-dependencies are added below the key, at the
  driver boundary, for every pass of the process alike, so pipelines stay compatible with the passes they are used in.

## 2. The rule (`DM_GFX_PASS_GMEM` 0 / 2 count / 1 apply; boot only, `TU_DEBUG` is read when the driver loads)

First reason that applies keeps the pass in sysmem; none = GMEM.

| Reason | Test | Env (default) |
|---|---|---|
| other | more than one subpass (NR-ONCHIP M), layered framebuffer, or a render pass whose creation `pgm` did not see | — |
| storage | a draw in the pass writes storage buffers / images (vertex-side effects run once per bin in GMEM) | — |
| feedback | the pass's depth was sampled inside the pass (`DM_GFX_DEPTH_FEEDBACK`, on by default on Adreno) **and** the pass writes depth (GMEM samples the DRAM copy; exact only while depth is untouched) | — |
| nodraw | fewer draws than the minimum (clear-only passes, copy-as-pass) | `DM_GFX_PASS_GMEM_MIN_DRAWS` (1) |
| draws | more draws than the maximum: G-buffer, depth pre-pass, shadows, surfaces, the Iggy UI layer | `DM_GFX_PASS_GMEM_DRAWS` (5) |
| atts | more attachments than the maximum | `DM_GFX_PASS_GMEM_ATTS` (3) |
| small | render area below the minimum ("full-screen" only: 3D at ≥ ~0.6 of 1080p-class; half-res chains of 1080p, bloom, portrait, brightness chain stay) | `DM_GFX_PASS_GMEM_PIXELS` (600000) |
| bw | only with `DM_GFX_PASS_GMEM_BW=1`: Turnip's bandwidth model (below) does not favour GMEM | `DM_GFX_PASS_GMEM_BW` (0) |

Turnip's model (tu_autotune.cc `bandwidth_algo`, tu_pass.cc `tu_render_pass_bandwidth_config`, tu_cmd_buffer.cc draw stats), per pixel of
the render area: sysmem = clear bytes + Σ draws (colour bytes x2 with blending + depth cpp for test + for write + stencil x2); gmem =
1.1 x (load + store bytes) + 0.1 x the draw traffic. Turnip weights draw traffic by measured samples-passed; we count every draw as
full-screen (exact for the passes the rule picks).

Expected picks on the campaign frame (GPU_BREAKDOWN.md f=2509): DeferredSky (2 MRT), ClusteredLightingWithSSAO, DeferredFog, FadeBlending,
the post tail (PPApplyLUT, EoCSight, PPFXAA, PPVignette, CombineUI_NoFetch), the 5-draw forward particle pass, Iggy filter passes with
≤ 5 draws at window size; stays sysmem: pre-pass (526 draws), G-buffer (5 attachments, 231 draws), shadow atlas (252 draws), Iggy UI
layer, portrait 80x100 and its brightness chain, bloom, half-res particles, clears. `=2` gives the real list (`pass-gmem-top`).

## 3. Log lines

- Boot: `[vk] TU_DEBUG=gmem (...); DM_GFX_PASS_GMEM=1: render passes get VERTEX self-dependencies ...` (vk_driver) and
  `NVN DM_GFX_PASS_GMEM=1 (apply: ...) draws 1..5 atts<=3 pixels>=600000 bw=0; driverID=18 TU_DEBUG=gmem driver side=1`.
  A red `NVN DM_GFX_PASS_GMEM=1 (count only)` = not applied (dm/env has `TU_DEBUG=sysmem`, system driver, not Turnip).
- Per PERF window: `NVN PERF pass-gmem mode= apply= passes= gmem= (in GMEM | would be GMEM) barriers= gmem_draws= sysmem: other= storage=
  feedback= nodraw= draws= atts= small= bw=; turnip-model MB picked sys/gmem= all-sys=` (per frame).
- Every `DM_GFX_PASS_GMEM_TOP` (10) windows: up to 16 `NVN PERF pass-gmem-top WxH att= ld/st/clr= B/px draws= (max) ps=<first fragment
  shader key, 8> n=/frame -> <gmem | reason> turnip-model KB sys/gmem=` lines, GMEM picks first. Match `ps=` with the `first=...;4:<ps>`
  keys of an `NVN GPASS` run to name the passes.

## 4. Verification on the PC

- `dm/gfx/vulkan/engine/tests/nr_gmem_test.c` (clang 20): **14004 checks PASS** — the campaign frame model (19 passes, expected reason
  each), every veto, thresholds, Turnip's model by hand (plain post pass -> bw; transient depth, overdraw, clears), format sizes, per-draw
  bytes, and a 200 000-pass random sweep (a GMEM pick satisfies every condition; looser thresholds never drop a pick; every reason occurs).
- `tests/source_structure_test.py`: new block NR-GMEM (funnel right after nbmt, barrier only when applied, =0 records nothing, hooks,
  driver side, present pass) + all old blocks PASS.
- Turnip source read at the bundled commit (Mesa 1c3a936): `use_sysmem_rendering` order, `tu_barrier` non-framebuffer barrier,
  `tu_render_pass_add_subpass_dep` (self-dependencies ignored), autotuner small-pass rule, bandwidth model.
- APK: see §6.

## 5. Phone plan (not run: no phone this session)

Preconditions: dm/env **without** a `TU_DEBUG` line (else `=1` cannot set gmem), `DM_DOS2_FPS=0`, back up `files/save` inside the phone,
check the keyguard and `thermal_limit_refresh_rate` before reading fps; 60 s from level load; 2 min cooling after every 3 runs.

| # | Run | Env | Purpose | Duration |
|---|---|---|---|---|
| 1 | G_cnt | `DM_GFX_PASS_GMEM=2` | which passes switch, reasons, model MB (standing, Fort Joy) | 60 s |
| 2-5 | G_st A B B A | `=0` / `=1` | standing ABBA: fps, gpu_ms; `=1` log must say `apply=1`, barriers ≈ passes − gmem | 4 x 60 s |
| 6-9 | G_wk A B B A | `=0` / `=1` | walking ABBA | 4 x 60 s |
| 10 | G_bw | `=1 DM_GFX_PASS_GMEM_BW=1` | only the passes Turnip's model favours | 60 s |
| 11 | G_shot | `=1` | screenshot vs `=0`: post / lighting / UI identical (no garbage from DONT_CARE loads in GMEM) | 30 s |

Total ≈ 12 min of runs + 6 min cooling. Reading: `=1` vs `=0` gpu_ms; if `=1` loses, try `DM_GFX_PASS_GMEM_ATTS=1` (one-attachment post
passes only) or `_BW=1`. If `=1` shows garbage in a picked pass, that pass's load op is DONT_CARE while its content is live (NB-RP plan on
Adreno was only checked under SIM mali) -> report the `pass-gmem-top` line.

## 6. Build

APK `wt-gmem/work/DOS2-port-debug.apk`, 56 162 493 bytes, SHA-256 `ba60eadb63ab2c4f85062368cde40bdc0f700827268c96c0911ce3d7576d770e` (NDK r29, `tools/build_apk.ps1`;
no new warnings from the NR-GMEM files). Driver: none to push (stock `turnip27` bundled in the APK).

## 7. Risks

- GMEM honours load / store ops: a pass whose NB-RP / NR-RP plan has a DONT_CARE load on live content shows garbage only under `=1`
  (on sysmem DONT_CARE is a no-op). The plans were verified with poison under `DM_GFX_SIM_GPU=mali` (batch D: 0 magenta) — screenshot run 11.
- Passes created before `pgm::init` (none known: init runs in `xbg_game_initialize` before any pass) would count as "other" and stay sysmem.
- `TU_DEBUG_FILE` (Turnip watches the file with inotify; `sysmem` / `gmem` are runtime flags there) could switch modes live for an in-process
  A/B without restarts; not used here (the file's flags replace TU_DEBUG's runtime flags, and the port sets TU_DEBUG itself).
