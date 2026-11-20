# NR-ONCHIP: deferred G-buffer to lighting in tile memory. Analysis, shader groundwork and plan (18.11.2026)

Branch `dos2-nrtile` (worktree `wt-nrtile`, base `dos2-nr-all` 4b06e3d). No phone tonight (night rules: no adb). **No change to the native
code, the APK or the defaults.** What this step contains: the analysis with numbers, a dump scanner, input-attachment variants of the native
lighting shaders proven bit-exact, and the plan for the execution step. `DM_NR_ONCHIP` is *reserved* (semantics in §5). It is not in the code
yet, because nothing executes on chip yet.

## 0. Summary

| Question | Answer |
|---|---|
| Mi 14 Ultra (Adreno 750, Turnip) | **0 ms from any on-chip scheme.** The port sets `TU_DEBUG=sysmem` (boot.log line 28, `android/vk_driver.c`), so Turnip renders every pass straight to memory, and subpasses, input attachments and transient attachments move the same bytes. The only route on the Mi 14 is a Turnip change: GMEM for this one merged pass, sysmem for the rest (§4, ~1.5 ms modelled, unproven). |
| Tile GPUs (Mali, PowerVR, stock-driver Adreno in GMEM) | Real. The whole chain (G): **121–133 MB/frame** less DRAM traffic at 2160x972. The small chain S3: **27–29 MB/frame**. ms per class: §4. |
| Can the G-buffer pass and the lighting be one render pass as the frame stands? | **No, not without reordering.** In 11 of 34 dumped 3D frames (all with shadows), **3 shadow-cascade passes (944–1116 draws)** run between LinearizeDepth and CascadedPCF. They have to be moved before the G-buffer pass first (NB-17 order planner, `DM_NB_ORDER=3`). Also, every G-buffer material pipeline must be rebuilt for subpass 0 of the merged pass, because render-pass compatibility includes the subpass count. |
| What is possible without touching the G-buffer | **S3 = CascadedPCF → DeferredSky → ClusteredLighting** as 3 subpasses (S2 = Sky → Light when there are no shadows). These passes are back to back in every frame. Only 3 single-draw native pipelines get variants, and only the lighting stage reads input attachments. Its variants are done and proven (§3). |
| SSAO inside ClusteredLightingWithSSAO | **There is none.** All 9 fetches of the stage are `texelFetch` at the fragment's own pixel (G-buffer ×5, linear depth, DeferredSky out, shadow mask, 1×1 exposure). No neighbourhood taps, and no AO texture is bound (the translated interface: `t_E t_A t_12 t_8 t_C t_10 t_32 t_14 t_18`, NR-04). The Switch build has SSAO off and kept the name. The 1.76 ms of the shader is lights + G-buffer bandwidth. **Half-res SSAO + bilateral upsample has nothing to replace.** Adding it would be a new effect that costs GPU time, so it is not offered. |

## 1. The frame (NB IR dumps ra1 / rb1 / rc1, 41 parts, 34 with the lighting pass; deck and 3D menu, 2160x972)

```
G-buffer  c0 R11G11B10F  c1 RG16F  c2..c4 RGBA8  ds D24S8   (395 draws deck / 268 a frame in Fort Joy, plan: colours CLEAR, depth LOAD from the pre-pass)
LinearizeDepth  ds -> 0x1181000000 (RGBA8 in the IR image table)                                  1 draw, texture(tDepth, TexCoord)
[shadow cascades: 3 passes into the 1536² D16 atlas 0x10be140000]                               only in frames with shadows
CascadedPCF_3x768  linear depth (texelFetch, own pixel) + atlas -> S 0x10c4fef000 R8             1 draw
DeferredSky  E A C + linear depth -> Sp 0x10cd7cf000, Df 0x10ccf5f000 (both cleared, folded)       1 draw, 2 MRT
ClusteredLightingWithSSAO  E A C t_8 t_10 + linear depth + Sp + S + exposure -> Df (LOAD)         1 draw
FadeBlending / DeferredFog / forward surfaces into Df with ds ...
```
Facts that matter for a merge (from the dumps, `tools/dos2_onchip_scan.py --frames`):
- Every chain read of an earlier chain output is at the fragment's own pixel: the native shaders' fast path, render scale 1, which the host
  always writes (`xbg_support_defaults`). LinearizeDepth samples with a texcoord, so its input-attachment form needs a run-time proof
  (nearest sampler, or bilinear with the texcoord within 1/512 texel of the centre). It is only in variant G.
- After the lighting, G-buffer c0 is cleared (dead). c1 has no later reference in the part (open). c2 / c3 are next used as attachments with
  LOAD at the same addresses. These are aliases from the engine's RT pool (NR-10 "fresh"), so they count as live in the conservative
  column. Depth is live (forward passes). Df is live. Sp is next an attachment with LOAD (the bloom alias at 1080x486): counted as live. S has no
  later reference (open).
- Nothing else (copies, clears outside a pass) sits between PCF, Sky and Light. Between the G-buffer pass and LinearizeDepth there is only an
  empty IR pass.

## 2. Driver features (Mi 14 log play-1118, `--log`)
Turnip 27.0-devel advertises `VK_KHR_dynamic_rendering_local_read`, `VK_EXT_` and `VK_ARM_rasterization_order_attachment_access`,
`VK_EXT_attachment_feedback_loop_layout` and `load_store_op_none`, and has a lazily allocated memory type (t3 = 0x11). None of them helps while
`TU_DEBUG=sysmem` is set. Other families (general knowledge, to be confirmed from the `Device extensions` lines of player logs, which the scanner
reads):
- Input attachments in a multi-subpass `VkRenderPass`: core 1.0, every driver. This is Arm's and Imagination's recommended deferred path, and
  the one to build, because the backend uses `VkRenderPass` objects everywhere (no `vkCmdBeginRendering` in `xbg_game.cpp`). Local read would mean
  moving all pipelines to dynamic rendering first.
- Mali (Valhall r3x+): `VK_ARM_rasterization_order_attachment_access`, lazily allocated memory, 8 colour attachments. Tile budget: G57-class
  keeps the full 16×16 tile up to 128 bit/px of colour. S3 needs 8 + 32 + 32 = 72 (fits). G needs 5×32 + 32 + 8 + 2×32 = 264, so a
  smaller tile (some per-tile overhead, still a large net win).
- PowerVR: subpasses + lazily allocated memory, yes. Rasterization-order extensions: not expected.
- Xclipse (RDNA, no tile memory): about 0. Maleoon: unknown (decide by its log).

## 3. Shader variants (done, proven)
`tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag` and `DeferredSky_mobile.frag` take `-DOC=<mask>`. Texture n with bit n set becomes
`subpassInput` (`input_attachment_index` n) read by `subpassLoad`, and the render-scale slow path is compiled out. **Without `OC` both files
compile to the NR-04 SPIR-V byte for byte** (34 516 / 10 988 bytes, `cmp` equal), so setup stage 8 and `native-nr.xgspack` are unchanged.
| Variant | OC | Inputs (binding: index) | For |
|---|---|---|---|
| light.s2 | 0x40 | 52: 6 (tAmbient = Sky out 0) | S2 |
| light.s3 | 0xC0 | 52: 6, 58: 7 (tShadow) | S3 |
| light.g | 0xFF | 10:0 16:1 22:2 28:3 34:4 40:5 52:6 58:7 | G |
| sky.g | 0x0F | 10:0 16:1 22:2 34:3 | G |

Proof: `tools/dos2_onchip_shaders.py --bundles <player's shaders-vk>`. `tools/spv_interp.py` now models `OpImageRead` of a subpass input as
Vulkan defines it (the attachment's texel at the fragment's own pixel). On the random invocations of `dos2_light_mobile.py` at render scale 1
(all shading models, sky, 48 lights + clusters, shadow on / off), **variant == translated (spirv-opt -O) == NR-04 native, bit for bit: 400 / 400
for each of the 4 variants**. On the 8 fixed light scenarios × 20: 0 mismatches. spirv-val passes. The entry-point interface equals the
translated one, and the subpass bindings are exactly the mask. Mutation (modelled read one pixel off): caught in 44 / 44 / 60 / 55 of 60.
Since §8 the pack carries light.s2 / light.s3 as `2.` / `3.ClusteredLightingWithSSAO` (`dos2_nr_sh_pack.py`, setup stage 8).

## 4. Expected gain (model, `tools/dos2_onchip_scan.py`, no compression; framebuffer compression makes it roughly ×0.5–0.7)
DRAM traffic of the chain's passes, separate passes vs. one render pass (open images stored / dropped), averaged over the dumped frames:
| Variant | Frames | Separate | Merged | Saved MB/frame |
|---|---|---|---|---|
| S3 (PCF → Sky → Light) | 11 (shadow frames) | 48.3 | 21.0 / 19.1 | **27.3–29.2** |
| S2 (Sky → Light) | 23 | 42.0 | 16.8 | **25.2** |
| G (G-buffer … Light, shadow passes moved) | 34 | 189.5 | 68.5 / 56.8 | **121–133** |

ms = MB / effective bandwidth (assumed: G57 MC2 LPDDR4X ×32 11 GB/s, G615/G715 35, PowerVR GE8320/BXM 8, PowerVR DXT 40, stock Adreno GMEM 20,
Adreno 750 GMEM 50):
| Class | S3 | G |
|---|---|---|
| Mi 14 (Turnip sysmem, as shipped) | 0 | 0 |
| Mi 14 if Turnip ran the merged pass in GMEM | 0.55 | 2.4–2.65, minus a binning pass of the 268 G-buffer draws (~0.3–0.6) |
| Mali-G57 MC2 | 2.5–2.65 | 11–12 |
| Mali-G615/G715 | 0.8 | 3.5–3.8 |
| PowerVR GE8320/BXM | 3.4–3.65 | 15–16.6 |
| PowerVR DXT | 0.7 | 3.0–3.3 |
| Snapdragon 7, stock driver (GMEM) | 1.4 | 6–6.6 |
Weak devices run lower resolution (DRS 75 %: ×0.56). These are bandwidth bounds, not measured fps. Measuring them needs a tile device
(a tester prerelease).

## 5. Execution step (not done; why, and the plan)
Not done tonight. The Mi 14 gains 0 from it. Nothing could measure it without a tile device. The weekly limit is at 84 %. It touches the pass
machinery of `xbg_game.cpp` (lazy clear / CLEAR_FOLD twins, NB-06 plan begin, NR-10 visibility points, NR-10b area, store-hist, NB-MT packet
replay, NR-31 seed keys, pass timestamps), and doing that blind would break things. Plan, in order:
1. **S3 / S2 (≈ 5–7 agent h).** The front-end planner (`nb_plan.c`, NB-06) marks a chain in the PASS headers, using flag bits plus member index:
   consecutive passes PCF? / Sky / Light by FS key, same extent, Light reads Sky out 0 (and S), nothing between them. The backend verifies at the
   head pass that the native Sky and Light are loaded and that the `oc` variant is in the pack. It then creates a render pass with 2–3 subpasses
   (attachments S, Sp, Df; by-region dependencies COLOR_WRITE → INPUT_ATTACHMENT_READ / COLOR_READ; S preserved through the Sky subpass) and a
   framebuffer for it. It runs `vkCmdNextSubpass` (a new NB-MT packet) instead of end / begin, builds the 3 pipelines for (merged pass,
   subpass k), and writes bindings 52 / 58 as `INPUT_ATTACHMENT` with the attachment's own view (GENERAL). First version: every store kept
   (STORE), so a broken chain only ends the pass early (empty subpasses, everything stored) and stays correct. That gives 27.3 of the
   29.2 MB. Then DONT_CARE for S / Sp once `=2` shows that no chain broke.
2. **G (≈ 10–15 agent h + phone).** Move the shadow-cascade passes before the G-buffer pass (`DM_NB_ORDER=3` schedule; they read no G-buffer
   image, and nothing before PCF reads the atlas). Build G-buffer pipelines for subpass 0 of the merged pass as a second key, warmed together
   with the normal one so Mali does not stutter (MALI-17). Use a LinearizeDepth input variant with the run-time sampler / texcoord proof, and
   `light.g` / `sky.g`. G-buffer c0 / c1 (+ c2..c4 with NR-10 fresh) go DONT_CARE / transient (lazily allocated).
3. **Mi 14 only:** a Turnip build where `TU_DEBUG=sysmem` spares render passes with input attachments, plus A/B. Before that, a cheap
   env-only experiment: `TU_DEBUG=` (autotune) without any merge, to see what GMEM costs DOS2's other 76 passes.

`DM_NR_ONCHIP` (reserved): 0 off byte for byte (default on every family); 1 = S3/S2 merged; 2 = check (the planner's chain marks verified and
counted, old passes executed, `[nr] onchip` line every 10 s with chains / breaks / MB); 3 = 1 + every DONT_CARE attachment filled with magenta
in its first subpass. Defaults by `xbg_tile_family` only after `=3` with `DM_GFX_SIM_GPU=mali` on the Mi 14 (which exercises correctness
even in sysmem) and a tester build on Mali.

## 6. Files and checks
- `tools/dos2_onchip_scan.py` (new): IR dump chains + traffic model + driver features from a dm log.
  `python tools/dos2_onchip_scan.py <dumps…> [--frames] [--log boot.log]`.
- `tools/dos2_onchip_shaders.py` (new): variants + proof (PASS, 22 s). `tools/spv_interp.py`: `OpImageRead` of a subpass input (`_subpass_read`,
  `subpass_reads`).
- `tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag`, `DeferredSky_mobile.frag`: `OC` switch (OC undefined = identical SPIR-V).
- No native file, pack, policy or APK changed, so there is no APK to build and nothing to push to the phone.

## 7. Phone runs (when the execution step exists; none needed for this commit)
Mi 14, Fort Joy, 60 s from load: `DM_NR_ONCHIP=2` (`[nr] onchip` chains ≈ 1 / 3D frame, breaks 0); `=3 DM_GFX_SIM_GPU=mali` (no magenta,
picture = `=0` by `DM_GFX_PASS_VALUES`); `=1` vs `0` ABBA (`gpu_ms` equal within noise; it is sysmem). Real gain: a Mali / PowerVR tester
prerelease. Separately, an env-only A/B now: `TU_DEBUG=sysmem` vs `TU_DEBUG=` (autotune) in Fort Joy (`NVN PERF gpu_ms`).

## 8. Execution step S2 (18.11.2026, branch `dos2-nronchip` from `dos2-nr-all` a87ad39; no phone: night rules)
**What runs.** The front end marks the chains; the backend executes **Sky -> Light as one VkRenderPass with two subpasses**. An S3 mark
(PCF -> Sky -> Light) executes as PCF (its own pass, as before) + the same S2 merge. Reason: the merged pass would have to begin at PCF,
and there the backend has not reached Sky's SetRenderTargets yet, so it does not know Sky's attachments (images + views). The PASS headers
only carry memory addresses (aliases share them), and last frame's images could be the wrong ones. S3 adds only S (R8: 2.1 MB written +
2.1 MB read a frame at 2160x972) to S2's 25.2 MB. It waits for a header that carries the chain's attachments (see "Not done").
- **Planner** (`dm/gfx/nb/nb_onchip.h` `nb_onchip_plan`, pure C, after `nb_plan` / `nb_post_plan` in `dm/host/hle_dos2_nb.c`): the mark
  `oc = 0x80 | n << 2 | k` in `nb_node_plan.oc` -> `p_nb_pass.oc` (byte 111, the former padding: header layout and size unchanged,
  `static_assert` in the backend). Rules (header comment): one draw, exact fs, no hazard / elide / fuse / AA / feedback; Sky = 2 colours,
  Light = 1 = Sky out 1; Light samples Sky out 0 and not Df; no clear in Light, none after a draw in Sky / PCF; sizes equal; only barrier ops
  / empty passes between Sky and Light (between PCF and Sky also the ClearTexture ops folded into Sky). Line
  `[nr] onchip plan DM_NR_ONCHIP=N: ... chains marked a frame S3 x, S2 y; not marked / downgraded: ...`.
- **Backend** (`dm/gfx/vulkan/engine/xbg_nr_onchip.inc`, namespace `nroc`; the stepping decisions `nb_oc_step` are in the header and
  host-tested). At Sky's first host pass (draw or in-pass clear) M begins: the open pass ends; Sky's pending ClearTexture clears that the plan
  folded become M's **CLEAR load ops** (other attachments LOAD); one full barrier (it replaces the PREBAR of Sky and of Light: everything
  written before M is visible to both subpasses); every **store STORE**. M = attachments {Sp, Df}; subpass 0 colours [Sp, Df] (Sky's slots);
  subpass 1 colour [Df], input attachment index 6 = Sp; dependencies EXTERNAL->0 and 0/1->EXTERNAL as every pass (`pass_deps`), 0->1
  COLOR_ATTACHMENT_WRITE -> INPUT_ATTACHMENT_READ | COLOR_ATTACHMENT_READ / WRITE, BY_REGION.
  Light's draw: `pre()` (before the descriptors) swaps its fragment shader for the twin `<key>#oc2` = light.s2 (bundle
  `2.ClusteredLightingWithSSAO` of native-nr.xgspack through `xpack::nrsh::oc_apply`: translated head byte for byte, interface and
  descriptors checked, the load-time rewrites of every bundle; binding 52 becomes INPUT_ATTACHMENT; the module must have exactly {52: 6} as
  subpass input). `adjust()` (after create_pass) records `vkCmdNextSubpass` (new NB-MT packet `OP_NextSubpass`; `dm/android/vk_tramp.S` /
  `vk_tramp_names.h` regenerated with doom-port's `tools/gen_vk_tramp.py`, which reproduces the old file byte for byte without the new name)
  and gives the pass M / subpass 1 (`Pass::subpass` -> `VkGraphicsPipelineCreateInfo::subpass`; one PipelineDefinition per subpass, words
  `{0, 0x4f43, ...}`, so a warm-up / seed record of it fails `make_pass` instead of building a wrong pass). `descriptors()` writes binding 52
  with M's view of Sp (no `prepare_read`, no memo for that draw). Anything else that ends the open pass ends M: `end_open_pass` steps the
  subpasses left (`before_end`), contents are kept, every store is STORE, so the picture is the same and the rest of the chain runs as
  separate passes.
- Colour-attachable 2D images get `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT` (and their attachment views) only at =1 / =3; an unsupported
  combination drops that bit first.
- **Pack**: `tools/dos2_nr_sh_pack.py` (setup stage 8) also builds `2.ClusteredLightingWithSSAO` (light.s2, OC 0x40) and
  `3.ClusteredLightingWithSSAO` (light.s3, OC 0xC0, for the later S3 step), checks the subpass bindings and proves them with
  `tools/dos2_onchip_shaders.py check` (interpreter: variant == translated == NR-04 native: 40 / 40 each in the local build). Setup's NR tool
  hash now includes `dos2_onchip_shaders.py`, so a player's setup rebuilds native-nr.xgspack once. Local pack:
  `dos2-port/work/native-onchip/native-nr.xgspack` (21 bundles, from the player pack `work/shaders-vk/shaders.xgspack`;
  game data: never into git / the APK).

| DM_NR_ONCHIP | What |
|---|---|
| 0 (default, every family) | nothing: the front end plans nothing (the backend publishes 0), images keep their usage, one integer test per host pass |
| 1 | S2 merge (S3 marks: PCF separate + S2); colour images with input-attachment usage |
| 2 | check: the same decisions on a shadow run; the light.s2 module and M's render pass are built once; old passes executed; counts "would" |
| 3 | 1 + magenta: every attachment M begins with CLEAR is first cleared to magenta in memory (vkCmdClearColorImage before M). On a tiler a read that misses the subpass dependency or a load from memory shows magenta; on the Mi 14 (sysmem) magenta means a wrong load op or a wrong begin in the backend |

Line every 10 s: `[nr] onchip DM_NR_ONCHIP=N: a frame: marked heads h, M begun b (S3-marked s: PCF kept as its own pass), Sky -> Light chains c;
DRAM traffic not moved (model, own pixels): X MB a frame; begin refused: ...; ended early: ...; input-attachment descriptors n (another
image bound m); magenta poison clears p; red lines r`. Safe mode step 1 (tilers) sets DM_NR_ONCHIP=0. Family default: **0 everywhere** - not
proven on any device tonight (no phone, so no SIM-mali check either).

**Proof without a phone.** `tests/nb_onchip_test.cpp` (62 checks): synthetic S2 / S3 frames and 9 refusals; the phone's IR dumps (ra1 / rb1
/ rc1, 41 parts): **34 Light passes, S3 11, S2 23, none unmarked** - the counts of §1's analysis; the stepping state machine (begin / same /
next / break, steps to the end, roles of every mark, malformed marks); the subpass inputs of the built pack (light.s2 {52:6}, light.s3
{52:6, 58:7}, the NR-04 bundle none). `source_structure_test.py` `nronchip_check` (every hook gated, the order in draw(), one begin path
through lrz_touch_before / nbtr::decide, every store STORE, default 0, safe mode, the pack variants), plus NB-20a (`twin_draw` = S), NR-09
(the hook sits between create_pass and the lane), NR-21 (36 wrappers). NB-MT / NB-MX / NR-21 mock tests with the new packet, safe_mode_test,
vendor_policy_test and the NB planner tests pass. NDK 29 build of libdm.so links; APK in the final report.

**Expected gain** (S2, the model of §4 with CLEAR loads: Sky's Sp + Df stores stay; Light's Df load, its Sp texture read and one Df store
go): 25.2 MB a frame at 2160x972, x0.5-0.7 with framebuffer compression. Mi 14 (Turnip sysmem): **0** (same bytes; +1 vkCmdNextSubpass,
-1 barrier, -1 begin / end). Mali-G57 MC2 ~2.3, G615 / G715 ~0.7, PowerVR GE8320 / BXM ~3.1, DXT ~0.6, stock-driver Adreno GMEM ~1.3 ms
(bandwidth bounds, not measured; DRS 75 %: x0.56). Cost: the first lit frame builds 2 pipelines in the frame (Sky in subpass 0, the
light.s2 twin, which is not in the warm-up), once per session.

**Risks.** (1) A driver that splits a render pass at a subpass with input attachments gains nothing (Mali / PowerVR merge them: it is their
documented deferred path). (2) Input-attachment usage on every colour target at =1: no known driver drops compression for it, but it is a
usage change on all render targets - measure =0 vs =1 on a tiler. (3) A Sky draw skipped by an async build (first frames only) leaves Sp /
Df with their clears, as the separate passes would. (4) A Light draw prepared for subpass 1 whose M ended inside its own descriptor build
(not expected: Light has no storage bindings) is skipped and counted (red line).

**Not done.** S3 execution (needs the chain's attachments at PCF: NB-17a offsets or a header extension; +4 MB a frame); DONT_CARE stores
for Sp / S after a clean =2 run; G (the G-buffer in the same pass, §5 step 2); the Turnip GMEM build for the Mi 14.

**Phone runs (for the owner; none tonight - no adb).** Push `native-nr.xgspack` from `dos2-port/work/native-onchip/` to the
phone's `<shaders>/native-nr.xgspack` (0.27 MB; or run setup once with the new tools). Mi 14, Fort Joy, 60 s from load each:
1. `DM_NR_ONCHIP=2`: `[nr] onchip plan ...: chains marked a frame S3 ~0.3, S2 ~0.7` and `[nr] onchip DM_NR_ONCHIP=2 (check: would): ...
   chains ~1.00`, `begin refused: none`, `ended early: none` (any reason here is the next fix).
2. `DM_NR_ONCHIP=3 DM_GFX_SIM_GPU=mali`: no magenta on screen; `chains ~1.00`, `magenta poison clears ~2.00`, `input-attachment
   descriptors` = chains, `another image bound 0`, no `【红】[nr] onchip` line.
3. `DM_NR_ONCHIP=1` vs `0` (ABBA): screenshots equal (same spot), `NVN PERF gpu_ms` equal within noise (sysmem).
Then a Mali / PowerVR tester prerelease at =1 to measure; the family default goes to 1 on `xbg_tile_family` only after that.

## 9. Zero chains on the phone: the size rule read the wrong size (19.11.2026, branch `dos2-onchip2` from `dos2-nr-all` 85d2cfa; no phone)
**Symptom.** Mi 14 Ultra, 2400x1080 window, Fort Joy walking, APK 8427667e (dos2-nr-all 55c455a), logs `src/work/logs/D_onchip2.dm.log`
(`DM_NR_ONCHIP=2`, Adreno) and `D_onchip3.dm.log` (`=3`, `DM_GFX_SIM_GPU=mali`): every 10 s `[nr] onchip plan ...: 774 parts planned (bad 0);
chains marked a frame S3 0.00, S2 0.00; not marked / downgraded: sizes differ 774`. "sizes differ" = parts planned in every 3D window of both
runs (744 ... 1134): **100 % of the Light passes**, and nothing else refused them (the size rule is the last check before the PCF downgrade, so
every Light pass had already passed shape / Sky-before-it / edge checks).

**Root cause.** The front end's size callback was `nvn_nb_tap_image(image)`: the tap's table of RT sizes keyed by **storage address**, which
keeps the **last** size seen for that address (`nbt_remember` overwrites on every SetRenderTargets). The engine's RT pool binds Sky's out 0
(Sp, RGBA16F) storage again as the half-size bloom target later in the same part. The plan runs at submit, after the whole part is recorded,
so the table said Sp = 1080x486 against Df = 2160x972 for every chain. Evidence:
- The phone's own log (D_onchip2.log, `[nr] sh ... the verified pass` lines, the host's real pass areas): `DeferredSky ... colours
  [0x3f,0x3f] ... 2160x972`, `ClusteredLightingWithSSAO ... [0x3f] ... 2160x972`, `PPBloomScaleBiasDownsampleAndBlur ... [0x3f] ... 1080x486`.
  Sky's pass area is min(Sp, Df) = 2160x972 and Light's (Df alone) is 2160x972: the passes match; only the table did not.
- The phone dumps (rc1 / rb1 / ra1, same build family): the image table lists `0x10cd7cf000 1080x486` = Sky's c0, which is also
  `c0` of the bloom passes after Light. `nb_onchip_test` now plans every dump with the old rule (sizes from the `# image` lines): **marked 0,
  "sizes differ" 34 of 34 Light passes, Sp of every chain 1080x486** - the phone's result reproduced on the host. With per-pass sizes: 34 / 34.
- The 3D frame is not at the window size (2160x972 = 90 % of 2400x1080, DOS2's DRS / hires target) but that is the same for every 3D target
  and is not what differed.

**Fix.**
- Tap (`dm/gfx/nvn/nvn_nb_tap.inc`): a per-pass size table `g_nbt.pw / ph[node][att]`, filled (`nbt_psz`) from the RT set bound when each
  IR pass opened (in `nbt_sync`, which every pass-adding tap call runs, and at the top of `nbt_set_rt` before the set changes; reset with the
  IR in `nvn_nb_tap_reset`). Accessor `nvn_nb_tap_pass_size(node, att, &w, &h)`. Kept only while `DM_NR_ONCHIP > 0`
  (`nvn_nb_tap_pass_sizes(mode > 0)` from `dm_nr_onchip_publish`); at 0 nothing is filled (one byte test per `nbt_sync`). No record changes.
- Planner (`dm/gfx/nb/nb_onchip.h`): `nb_oc_rules.att_size` (per pass, used when set; the image-keyed `size` stays for host tests and the
  diagnostic). Rule `nb_oc_sizes`: **Df at Sky == Df at Light, Sp at Sky >= Df in both dimensions** (S at PCF >= Df for S3). Vulkan needs every
  attachment of the merged framebuffer >= the framebuffer (= Df's size = Sky's own pass area min(Sp, Df) = Light's area); Light reads Sp at its
  own pixel. Equal sizes stay the expected case; "Sp larger" is allowed and counted. New reason `a size not known` (tap had no entry).
- Backend (`xbg_nr_onchip.inc` `sky_ok`): the same rule on the real images (Df == pass area, Sp >= it), its own refusal `Sky image sizes`
  (was folded into "Sky pass shape") with the first example `(first: Sp WxH, Df WxH, pass WxH)` in the 10 s line.
- Log line (front end): `[nr] onchip plan DM_NR_ONCHIP=N: P parts planned (bad 0); chains marked a frame S3 x, S2 y (by per-pass sizes; Sp
  larger than Df n; the old last-size-per-storage table would have refused m of them); not marked / downgraded: ...[; first size refusal: Sky
  Sp WxH, Sky Df WxH, Light Df WxH]`. `m` ≈ chains proves the diagnosis on the phone itself; any size refusal prints the real sizes.
- IR dumps (`DM_NB_DUMP`) get `# passsize <node> <att>=WxH ...` comment lines (per pass, as bound); `nb_onchip_test` plans such dumps with them.

**Tests.** `nb_onchip_test` 72 checks (was 62): the alias case (image table Sp 1080x486, passes 2160x972: old rule refuses, per-pass rule
marks, `table_differ` 1), Sp smaller (refused, example kept), Sp larger (marked, counted), Sp smaller in one dimension, Df at Light != Df at
Sky, unknown size, S smaller than Df (S3 -> S2); the dumps as above. `source_structure_test.py`, `nb_tap_test.py` and the full host suite
(87 tests): all pass except `t_cpuprof_rt`, which fails the same way on the base `dos2-nr-all` 85d2cfa (WSL real-time test, unrelated).
Code commit 118c858. APK (NDK 29, debug key) `work/DOS2-port-debug.apk` sha256 `bde6c3fd92e304553b57345f09e1d6c6048a5dec5dcc41b7c58659021e4d9e00`.

**Phone runs (owner; none here).** Same APK for both, Mi 14 Ultra, Fort Joy walking, 60 s from load each (2 runs,
~3 min with load). Push nothing else (the onchip pack is already on the phone from the 18.11 runs).
| # | env (dm/env) | Purpose | Proof lines |
|---|---|---|---|
| 1 | `DM_NR_ONCHIP=2` | chains formed (Adreno, check) | `[nr] onchip plan DM_NR_ONCHIP=2: ... chains marked a frame S3 ~0.3, S2 ~0.7` (sum ≈ 1.00 in 3D), `the old ... table would have refused m` with m ≈ chains, no `sizes differ` / `a size not known`; backend `[nr] onchip DM_NR_ONCHIP=2 (check: would): ... marked heads ~1.00, M begun ~1.00, Sky -> Light chains ~1.00; begin refused: none; ended early: none` |
| 2 | `DM_NR_ONCHIP=3 DM_GFX_SIM_GPU=mali` | merged pass executed + magenta poison | the same plan line; backend `[nr] onchip DM_NR_ONCHIP=3: ... chains ~1.00 ... input-attachment descriptors` = chains `(another image bound 0); magenta poison clears ~2.00 / frame; red lines 0`, `[nr] onchip render pass M:` and `light.s2 module` lines once; no magenta on a screenshot |
If run 1 still shows a size refusal, its `first size refusal: ...` (front end) or `Sky image sizes ... (first: ...)` (backend) is the next
fix; any `begin refused` / `ended early` reason is the next step of §8.

## 10. M ended before Light, and the Light draw was skipped (19.11.2026, branch `dos2-ocfix` from `dos2-night1119` b99e4ba; no phone)
**Symptom.** Mi 14, `DM_NR_ONCHIP=3 DM_GFX_SIM_GPU=mali` (run N_oc3mr, all chains S2F): every frame `M begun 1.00, Sky -> Light chains 0.00;
ended early: ended after Sky (Light did not follow) N, M ended between Light's checks and its draw (draw skipped) N` - no sunlight / shadows
(the clustered Light result missing). `=2` on Adreno (N_oc2r) was clean, because there the old passes run.

**Where M ended (evidence from the logs, matched in-game seconds `pass_ends op25=47/48`).** N_oc3mr vs N_simm (SIM mali, ONCHIP 0):
`pass_end_lines L2535=25 L8511=23 L218=1` vs `L2535=26 L8511=24`, read breaks "combined reasons" 25 vs 26, draw-op pass ends 47 vs 48, one
more fragment barrier a frame. L218 = M's begin (xbg_nr_onchip.inc), L2535 = `prepare_read`'s `end_open_pass`. Per chain the old path ends
Sky's pass by Light's sampled read of Sp (2535) and Light's pass by Fog's read of Df (2535); at =3 there is the begin (218) and exactly one
2535 more. The red line says pre() had checked the Light draw and swapped in the twin, and at adjust() M was gone: so M was ended by a
read break **inside the Light draw's own descriptor build** (between pre() and create_pass), a break with "written" and / or "attachment of
the open pass" set. The twin reads Sp / c0 as input attachments (input descriptors 2 a chain, "another image bound 0"), so the read is one
of the other sampled bindings (Light's table: 10 16 22 34 40 46 58 = texture slots 3 1 5 2 4 21 8). The 1-in-50 `NVN read break` sample
aliases with the 25 breaks a frame, so these logs cannot name the image; the new lines below do. =2 cannot see it: there the old Sky pass
is open and Light's read of Sp is supposed to end it.

**Why the draw was skipped.** adjust() returned false ("M is gone under a prepared Light draw") and draw() returned: the descriptors were
built for the twin (binding 52 an INPUT_ATTACHMENT with M's view), so the draw could not run outside M, and nothing rebuilt it.

**Fix.**
- Never skip: adjust() false now means redo - `nroc::redo` puts the game's fragment shader back, draw() rebuilds the modules / stages, the
  descriptors and the pass (`dp=&descriptors_stable(c,false);p=Pass{};create_pass(p,c);`) and the draw continues on the normal path: the
  old pass, exactly as =2 / =0 draw it. Counted: `twin draws drawn again as their own pass n`.
- The cause handled before the twin: pre() checks the member draw's sampled texture bindings (all but the variant's inputs, resolved as
  descriptors() resolves them, nothing made) with prepare_read's own test against M (attachment of M, not GENERAL, written since the
  reader's barrier point - fragment-only under DM_GFX_TILE_BARRIER, NB-08 / NR-10 seen points - or a pending clear that the read would land).
  If its descriptor build would end M, M ends right there in pre() (reason `the member's descriptor build would end M`), before any
  descriptor, and the draw is the old pass. At =2 the same check runs for "the binding is Sp / Df / c0" (counted as would). A member
  refused by its checks (targets, shader, clears) now also ends M in pre() (it used to end only at adjust(), after its descriptors had
  already ended M under another name).
- Attribution: every early end of M (not decided by a member check) is counted by site - end_open_pass call line, `cur_op`, member k and,
  when prepare_read ended it, the read image (role Sp / Df / c0 / other, serial, format, size, why bits, last_write vs the barrier point,
  inside the twin draw or not). One line per new site: `[nr] onchip M ended early (after Sky, subpass 0 of 3): end_open_pass line L, op O,
  by prepare_read of ...`; the pre() check prints its first example: `[nr] onchip the Light draw's descriptor build would end M: fragment
  binding B (texture slot S) reads ... why W (...)`. The 10 s line gains `early-end sites (total): L.. op.. k.. read .. n`.
- =0 unchanged (every hook behind `nroc::mode>0` / `nroc::active`), =2 still runs the old passes, default 0.

**Tests.** `source_structure_test.py`: all PASS (67 lines, as before; nronchip / NR-09 asserts updated: redo instead of return, the line
argument, note_read, pre() ends M). `nb_onchip_test` 108 checks 0 failed (dumps + pack), `safe_mode_test`, `vendor_policy_test` pass. NDK 29
`-fsyntax-only` of xbg_game.cpp with the APK's defines: no error, no warning in the new code. No APK built.

**Phone runs (owner / main session).** Mi 14, Fort Joy walking, 50 s standing from load each: (1) `DM_NR_ONCHIP=3 DM_GFX_SIM_GPU=mali`;
(2) `DM_NR_ONCHIP=2 DM_GFX_SIM_GPU=mali` (the check under the same policy). Proof: (1) sunlight / shadows back (screenshot = N_simm),
`twin draws drawn again ... 0` or the `descriptor build would end M` line naming the binding / image (that is the next step: the image's
barrier before M or a planner rule), or an `M ended early ... end_open_pass line L` line naming the site if the cause is not a read.

## 11. A pending clear that Light samples ended M every frame (19.11.2026, branch `dos2-occlr` from `dos2-night1119` 2282e2b; no phone)
**Symptom.** Mi 14, `DM_NR_ONCHIP=3 DM_GFX_SIM_GPU=mali` (run P_oc3m, log `src/work/logs/P_oc3m.dm.log`): picture correct after §10, but
every frame `ended early: the member's descriptor build would end M ... 945`, `Sky -> Light chains 0.00`, `DRAM traffic not moved 0.0 MB`;
first example `fragment binding 40 (texture slot 4) reads another image serial 29 fmt 37 2160x972, why 8 (pending clear)`. `=2` on Adreno
(N_oc2r) was "would" 1.00: at `=2` only "the binding is Sp / Df / c0" is checked, the old passes run.

**Where the clear is deferred.** The tile family policy (`xbg_vendor_policy.h`, family mali / SIM mali: `DM_GFX_CLEAR_FOLD=1`,
`DM_NB_RP=1`, `DM_NB_OPBARRIER=1`) makes a ClearTexture of a whole one-level render target a pending clear (`clear_texture` ->
`pclear_put`, xbg_game.cpp): it lands when the image is used (`pclear_flush_image` from `image_read_hook` in `prepare_read`, an attachment
-> CLEAR load or land, a copy / blit, a submit). Serial 29 is the G-buffer's fifth target (RGBA8, 2160x972): the dumps (ra1 / rb1 / rc1,
`# image ... fmt=0x25`) show its ClearTexture right before the G-buffer pass (`clear 12 standalone`), then an RT set with it as c4 and no
draw (`plan 31 empty`), and no other use until Light samples it. So it stays pending until Light's descriptor build, whose read lands it
with `vkCmdClearColorImage` outside a render pass (`pclear_land` -> `end_open_pass`): that would end M, so pre() ended it (§10).

**Fix (`xbg_nr_onchip.inc`, `nb_onchip.h`).**
- Learned set (`nb_oc_learned`, pure, in `nb_onchip.h`): `reads_ok` (pre(), every Light / Fog check, all modes) now walks every sampled
  binding of the member (the first one that would end M still decides, as before) and records the texture serial of each image that is not
  Sp / Df / c0 (inputs excluded) - per member, replaced on every check. The bindings of the next Light are not bound when M begins, and
  NB_ORDER's look-ahead (`nbord::unit_member`) only exists inside a schedule run, so the previous check's set is what is known.
- `pre_flush` at M's begin (`begin()`): after `end_open_pass`, after M's render pass exists (a refused M moves nothing), before M's full
  barrier: every pending clear whose image is in the set (Light's; Fog's only when M has the Fog subpass; never Sp / Df / c0, which keep
  their rules) lands through the same `pclear_flush_image(im, 0)` the member's read would run. Line once:
  `[nr] onchip flushed N pending clear(s) before M (image serial ...)`. `=2`: the same pick at the shadow begin, nothing landed
  (`would flush`). 10 s line: `pending clears flushed before M[ (would)] x a frame (in y of the Ms)`.
- The `would end M` example line now prints up to 4 distinct examples (binding / image / why / member), so a blocker that appears after
  the first frame (whose learned set is still empty and ends M as before) is named too.

**Why it is exact.** A pending clear is landed by any use of its image (sampled / storage read, attachment, copy / blit / resolve,
submit), so between its ClearTexture and Light's read nothing used the image: it lands after every earlier writer and before Light's read
either way, with the same barriers (`pclear_land`; NB-08 mode 1 the image's own transitions) and then M's full barrier, which makes it
visible to Light (prepare_read finds nothing pending, written or to transition: no read break). Only an image that already has a pending
clear moves, by one clear; nothing else changes order. The dumps prove the order: in all 34 chains the image Light samples whose last
write is a ClearTexture has it **before Sky** (34), **none between Sky and Light** (0; Fog: 0) - the host test checks it. A clear issued
after M began would still end M through pre() (named), not moved. A wrong guess (the set from the previous check, Light sampling another
image this time) lands a clear early that the image's next use would have landed or folded: same contents, at most one clear instead of a
CLEAR load op.

**Tests.** `source_structure_test.py`: all 67 PASS (new asserts: flush after M's render pass, before its full barrier; only learned images,
via `pclear_flush_image`; `=2` counts; learning covers every binding). `nb_onchip_test` 115 checks 0 failed (+6 learned-set checks, + the
dump order check above). NDK 29 `-fsyntax-only` of xbg_game.cpp with the APK's flags: 0 errors, the same 15 warnings as the base. No APK.

**Phone run (main session).** Mi 14, Fort Joy, `DM_NR_ONCHIP=3 DM_GFX_SIM_GPU=mali`, 50 s standing from load. Proof: `[nr] onchip flushed
1 pending clear(s) before M (image serial 29)` once; 10 s lines `Sky -> Light chains 1.00`, `Fog drawn in M 1.00`, `DRAM traffic not
moved > 0` (~42 MB modelled), `pending clears flushed before M 1.00 a frame`, `ended early` only in the first window (the first frame), a
clean screenshot (= N_simm). Any other `would end M` example names the next blocker.
