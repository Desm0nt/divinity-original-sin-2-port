# GPU safe mode for Mali / PowerVR (DOS2) — port of DOOM 2016 MALI-13 / PVR-08 (17.11.2026)

Branch `dos2-nrsafe` (worktree `dos2-port/wt-nrsafe`, base `dos2-nr-all` 8c6e8f5). Owner request 17.11: DOS2 gets the safe mode DOOM 2016 has,
adapted to what DOS2 turns on by default now (NB on since 852c6ec, NR-06 / 08 / 09 / 09R / 04 / 10 / 20 / 27 / 30 defaults), plus NR-08b on by
default on PowerVR. Players never need a flag: the ladder runs by itself after graphics crashes; dm/env still wins for the player's own values.

## What was ported, from where
| piece | DOOM 2016 (`doom-port/src`, read only) | DOS2 |
|---|---|---|
| pure rules | `gfx/vulkan/engine/xbg_safe_mode.h` | `xbg_safe_mode.h`: same step / marker / classifier / fake-parser code; **DOS2 switch lists**; DOS2 thread names; GPU-hang text (DOS2 has no `fence_fatal`); `xbg_safe_should_set` (the player rule, below); no `DM_PVR_LADDER` alias (DOS2 never had the v0.5.0 ladder) |
| engine side | `xbg_game.cpp` namespace `safemode`, `xbg_safe_mode_boot/record/status/event`, `xbg_crash_classify`, `gpguard::live_pipe` | `xbg_safe_mode.inc` (included after `gpguard`): same functions; boot sets over the app's default table (`setenv(..,1)`) unless the player set the key; live fallback = `live_pipe` only (DOOM's zero-copy self-test and fence trylock / guard fallbacks have no DOS2 counterpart); fake triggers |
| crash handler | `host/crash.c` "crash thread" line + classify hook | `host/crash.c`: thread line, pc / lr library via `dladdr`, `xbg_crash_classify` (weak); **not for a signal another process sent** (run scripts' thread dumps, DOS2's `g_ext_pid`) |
| presenter | `vulkan_present.cpp` boot call before `vkCreateDevice` | same place (right after `physical=devices[0]`); `DM_NR_ROTATE` is now read after it (it was read at the top of `dm_android_vk_initialize`) |
| JNI | `nativeSafeModeStatus` / `nativeSafeModeEvent` | same + `dm_env_player` (which keys are the player's) + `DM_SAFE_DIR` = files dir (DOS2 does not set `DM_FILES_DIR`: it would move gpguard's files) |
| Java | frontend-common `SafeMode.java`, `DmGuard.java`, `GearPages.safeMode`; DOOM `DmActivity` opt-in, manifest service | already in the DOS2 tree (= frontend-common); new: DmActivity opt-in (`if (DOS2)`), `Tl2Gear` row, manifest `DmGuard` `:guard` + `POST_NOTIFICATIONS` |
| texts | DOOM's own res (it sets `gearUsesPortResources`) | frontend-common `gear_text.tsv` / `GearText.java` (13 languages): `gear_safe_note`, `safe_step_1`, `safe_step_2` lost "version 0.5" (and step 1 its "zero-copy memory and others"); DOOM unaffected (own res) |

Why not a port-common module: DOS2's engine differs where every hook sits (no dm-record thread, no `crashinfo`, no `fguard`, NB / NR
switches, the jni default table), so a shared patch would be a rewrite per port (as `pipeguard-v2`). Pointer in
`port-common/migration/dos2.md` ("safe-mode").

## The steps (Mali / PowerVR, real or `DM_GFX_SIM_GPU`); Adreno / Xclipse / others: one log line, nothing changes
`<files>/gfx-safe-mode` counts graphics failures; the next start runs step min(count, 3). `DM_SAFE_MODE`: unset / `auto` = counted,
`0` / `off` = none, `on` = at least 1 (the gear's "On"), `1`..`3` = that step.

| step | switch = value | why here (evidence) |
|---|---|---|
| 1 | `DM_GFX_ZEROCOPY=0` | DOOM step 1. DOS2 default is already off on tilers (zero-copy only on Adreno); only a forced 1 would be undone (and the player's dm/env wins) - kept for parity |
| 1 | PowerVR `DM_GFX_UBO_DYNAMIC=0` | DOOM step 1 (PVR-06 crash class: page fault in `vkCmdDrawIndexed`); already the DOS2 PowerVR default |
| 1 | PowerVR `DM_NR_UBODYN=0` | NR-08b, **default 1 on PowerVR from this branch** (below): it is the per-draw form of dynamic UBOs, the same crash class, never run on a real PowerVR |
| 1 | `DM_NR_RP=0` | NR-10 (P0 default 1 on Mali / PowerVR): barriers by render-pass visibility instead of full ones - a wrong barrier on a tiler is corruption / device lost |
| 1 | `DM_DOS2_PREPASS=0` | NR-30 (default 1 on tilers): depth pre-pass draws skipped by last frame's verdict - new, never on a real tiler |
| 1 | `DM_NR_UBOVEC=0` | NR-27 (default 1 on Mali): SPIR-V uniform-load rewrite - a driver-compiler input it never saw |
| 2 | `DM_GFX_NOLOAD=0`, `DM_GFX_STORE_HIST=0`, `DM_GFX_CLEAR_FOLD=0`, `DM_GFX_TILE_BARRIER=0`, `DM_GFX_LAZY_CLEAR=0` | DOOM step 2 (the tile pass policies). DOS2 names: `STORE_HIST` = its store-DONT_CARE heuristic (DOOM `STORE_DEAD` / `STORE_NONE`), `CLEAR_FOLD` (MALI-DOS2-01: clear-only loadOp CLEAR breaks on Mali r14 / r26). `LEAN_USAGE`, `TILE_AREA` do not exist in DOS2 |
| 2 | `DM_NB_RP=0`, `DM_NB_OPBARRIER=0`, `DM_NB_UIAREA=0` | the native backend's versions of the same policies (all 1 in the jni default table): NB-06 planner load / store / clear folding + pre-pass barrier (MALI-DOS2-02: its DONT_CARE stores), narrow copy barriers, UI render area. Without them step 2 would still discard stores |
| 2 | `DM_NR_DIRECT=0`, `DM_NR_DRAW=0`, `DM_NR_ROTATE=0`, `DM_NR_SH=0` | owner's list: the NR paths that change what is recorded (NR-09R direct recording, NR-09 short draw path, NR-20 pre-rotated swapchain -> identity + blit, NR-04 engine shaders by name) |
| 3 | `DM_GFX_PIPE_SERIAL=1` | DOOM `COMPILE_LOCK=2` in DOS2 terms: DOS2 has no `COMPILE_LOCK`; `PIPE_SERIAL` puts modules + pipelines under one lock (PVR-05) - already 1 on PowerVR, new on Mali |
| 3 | `DM_NB_MT=0` | DOOM's `COMPILE_LOCK=2` is about its record thread; DOS2's record thread is the NB-20b worker `dm-nbmt` (vkCmd* packets) - off = recording back on the translation thread |
| 3 | PowerVR `DM_GFX_NOOUT_MASK=0` | DOOM step 3 (PVR-07 ladder: changes the end-of-pass program the driver compiles). **Not on Mali**: there the mask is the d2r #179 picture fix (Mali writes 0 to colour of output-less shaders), no crash class it would fix |
| 3 | `DM_GFX_GS_PASSTHRU=0` | DOOM step 3 (culling GS kept). DOS2 has the code; only draws whose GS is the proven pass-through are affected |
| 3 | `DM_NR=0` + `DM_NR_BUF/DESC/HEAT/SEED=0`, `DM_NB=0` + `DM_NB_PIPE/DESC/UBO/OWN/POST/POST_AA=0`, `DM_GFX_PRESENT_THREAD=0` | "all NR off + NB conservative": the renderer of the released v0.1.1 (before 852c6ec), the last version players ran (owner rule: fall back to what worked). `source_structure_test` SAFE block fails if a new NB / NR / present entry is added to the jni table without a step undoing it |

Not ported (no DOS2 counterpart): `DM_GFX_ZC3_SELFTEST`, `DM_GFX_FENCE_TRYLOCK` / `FENCE_GUARD` live fallback, zero-copy self-test
fallback, DOOM's `crash pass:` lines (no dm-record packet table).

**[VK-DRV 21.11] Mode 1 (`DM_VK_NATIVE=1`, the default build): every family.** Steps 1-2 = the backend rows, step 3 = `DM_VK_NATIVE=0`; the old path's rows stay Mali / PowerVR only (Adreno / Xclipse / others: step 3 = the old path with its own defaults). Decided early (`xbg_safe_mode_pre`, jni `nativeStart`), counted step 3 only after the backend crashed at its step 2 (marker key `native=`). Details, log lines and the phone run list: `VK-DRIVERS.md` section 6 / 8. The paragraph below is the mode-0 rule.

**Adreno:** logs only (`NVN safe mode: not used on adreno ...`), as DOOM. The NB / NR defaults also run on Adreno, but no Adreno crash class
applies to the ladder today (the Mi 14 ran them, NB checks bad 0); an Adreno ladder would need its own evidence - a risk, below.

## Counting (which crash is a graphics failure)
Only on Mali / PowerVR. Counted once per process (`count=`, `step=`, `pid=`, `time=`, `family=`, `why=` in `<files>/gfx-safe-mode`, the file
SafeMode.java / DmGuard read): a native crash on `nvn-translate` (DOS2's translation thread: every vkCmd* without NB-MT, pipeline builds, the
warm-up / seed threads it starts inherit the name), `dm-nbmt` / `dm-nbx*` (NB-20b), `dm-nrp*`, `dm-present`, `dm-gfx`, `dm-crumbs`, a GPU
driver thread (`mali-*`, `pvr*`, `IMG*`, `Vk*` / `vk*`, `tu_*`, `pan*`); pc or lr in a GPU driver library; a fault in zero-copy memory; an
engine fatal naming `VK_ERROR_DEVICE_LOST` / result -4, or the GPU-hang fatal ("... (fence wait timed out), so the device is hung"). Not
counted: other threads (guest threads, `xbg-bcdec`, app / log threads) outside driver code, and any signal another process sent.

## In-process fallback
A graphics pipeline the driver fails to build (draws skipped, as before): on Mali / PowerVR `DM_GFX_PIPE_SERIAL` -> 1 for the rest of the run,
toast `toast_safe_pipe` (SafeMode.events). Once per run.

## The player rule (dm/env wins)
`jni_android.c` records the player's keys: every `DM_*` already in the environment when `nativeStart` begins (a launch environment, the gear's
`Os.setenv`) and every key `files/dm/env` sets. The boot keeps those (`The player's own (dm/env) values kept: ...` in the step line) and
replaces everything else, i.e. the app's own default table (`k_nb_on`, set with `setenv(..., 0)` before the Vulkan start), with
`setenv(key, value, 1)`; a replaced app default is shown as `KEY=0 (was 1)`. Host builds without the JNI: DOOM's rule (any set value is kept).

## Switches
| switch | values |
|---|---|
| `DM_SAFE_MODE` | unset / `auto` (default): counted step; `0` / `off`; `on`; `1` / `2` / `3` |
| gear "Safe graphics mode" (Mali / PowerVR only) | Off / Auto / On -> `DM_SAFE_MODE` 0 / unset / on; a change starts the count again |
| `DM_GFX_FAKE_GPU_CRASH=<s>[:rec\|lost\|hang]` (test) | rec: SIGBUS inside the first `vkCmdEndRenderPass` made after s seconds (on `dm-nbmt` with NB-MT 3, else `nvn-translate`); lost: a `vkQueueSubmit ... failed: -4` fatal; hang: the GPU-hang fatal; s counted from the engine start; Mali / PowerVR only |
| `DM_GFX_FAKE_FALLBACK=pipe[:<s>]` (test) | the pipeline-build fallback after s seconds |
| `DM_NR_UBODYN` | default now **1 on PowerVR** (`xbg_nr_ubodyn_default`), 0 elsewhere; step 1 sets 0 |

## NR-08b on PowerVR
`xbg_vendor_policy.h xbg_nr_ubodyn_default` returns 1 for `XBG_GPU_POWERVR` (owner 17.11; phone SIM batch 12 in `nr-phone-results.md`:
without it NR-08 writes ~1000 descriptor sets a frame under the PowerVR policy, with it 3 a minute, picture equal, check bad 0). Tests:
`vendor_policy_test` (PowerVR 1, others 0, the rule active under SIM powervr with DM_NR_UBODYN unset, off with 0), `source_structure_test`
NR-08b assert updated. Not run on a real PowerVR: safe-mode step 1 turns it off after a graphics crash there.

## Log lines
- Boot (dm-vulkan / boot.log): `NVN safe mode: step N of 3 on <family> (<how>; K graphics failure(s) counted in <files>/gfx-safe-mode, last:
  <why>; DM_SAFE_MODE=<v>): <step text>. Set: KEY=V (was W) ... The player's own (dm/env) values kept: ...` / `NVN safe mode: not used on adreno ...`.
- Test arm: `NVN safe mode test: DM_GFX_FAKE_GPU_CRASH=90:rec: a fake SIGBUS inside vkCmdEndRenderPass 90 s after the engine starts`.
- Crash: `[dm] CRASH sig=7 ...`, `[dm] crash thread dm-nbmt tid=...`, `[dm] safe mode: graphics failure on powervr (crash on the graphics thread
  dm-nbmt, signal 7): gfx-safe-mode count -> 1 (this run had step 0; the next start, counted, turns on step 1)`.
- Java crumbs: `safe mode: native decision: eligible 1, step N (counted), family powervr, count K, set ...`, `safe mode: crash guard started ...`,
  `safe mode: notice shown: step N of 3 ...`, `safe mode guard: ...` (DmGuard's decisions, files/guard.log -> boot.log).

## Phone run that proves the ladder (Mi 14 Ultra, SIM PowerVR; one agent drives the phone; back up files/save first)
dm/env:
```
DM_GFX_SIM_GPU=powervr
DM_GFX_FAKE_GPU_CRASH=90:rec
```
1. Start (gfx-safe-mode absent): `NVN safe mode: step 0 of 3 on powervr (counted; 0 ...)`, the arm line; ~90 s later the crash lines above
   (thread `dm-nbmt`), count -> 1. DmGuard relaunches (or its notification) - `safe mode guard: ...` in the next boot.log.
2. Run 2: `step 1 of 3 ... Set: DM_GFX_ZEROCOPY=0 DM_GFX_UBO_DYNAMIC=0 DM_NR_UBODYN=0 DM_NR_RP=0 DM_DOS2_PREPASS=0 DM_NR_UBOVEC=0`; the notice
   dialog (EN / RU step 1 text); `[nr] DM_NR_UBODYN=0`, `[nr] rp` line absent / 0, UBOVEC 0 in the engine lines. Crash at 90 s -> count 2.
3. Run 3: `step 2 of 3 ... DM_NR_DIRECT=0 (was 1) DM_NR_DRAW=0 (was 1) DM_NB_RP=0 (was 1) DM_NB_OPBARRIER=0 (was 1) DM_NB_UIAREA=0 (was 1) ...
   DM_NR_ROTATE=0 DM_NR_SH=0`; `[nr] DM_NR_ROTATE` line absent (identity swapchain); tile pass policy line all 0. Crash -> count 3.
4. Run 4: `step 3 of 3 ... DM_NB_MT=0 (was 3) ... DM_NR=0 (was 1) ... DM_NB=0 (was 1) ... DM_GFX_PRESENT_THREAD=0 (was 1)`; no `[nb]` / `[nr]`
   lines; the crash now on `nvn-translate` (no worker), still counted (count 4, step stays 3; the guard stops after 3 relaunches in 10 min).
5. Recovery: delete `DM_GFX_FAKE_GPU_CRASH` from dm/env, then gear Safe graphics mode Auto -> On -> Auto (each change deletes the count) or
   `run-as org.dos2.port rm files/gfx-safe-mode`: next start `step 0 of 3`; with `DM_SAFE_MODE=0` in dm/env the count stays but step 0.
6. Also: `DM_GFX_FAKE_GPU_CRASH=90:lost` and `=90:hang` (one run each: `FATAL: ...` then `safe mode: graphics failure ... (VK_ERROR_DEVICE_LOST
   fatal: ...)` / `(GPU hang fatal: ...)`), `DM_GFX_FAKE_FALLBACK=pipe:60` (`NVN safe mode (live): ... DM_GFX_PIPE_SERIAL 0 -> 1`, toast).
   On the real Adreno without SIM: `NVN safe mode: not used on adreno`, the fake switches inert.
Scene: ship deck after loading a save is enough (60-90 s); no heavy scene needed (the mechanism, not perf). Force-stop when done.

## Tests (no phone)
- `tests/safe_mode_test.cpp` (new, from DOOM's): steps, DOS2 switch lists per family and step (exact step 1, inclusion order, PowerVR-only keys,
  no DOOM-only names), the player rule, marker parse, DOS2 thread / library / device-lost / GPU-hang classifiers, fake parsers - all passed.
- `vendor_policy_test` (NR-08b PowerVR default), `source_structure_test` (new SAFE block: every step key is read by the tree, step 3 undoes
  every NB / NR / present default of the jni table, boot before `DM_NR_ROTATE` / `vkCreateDevice`, engine init after the policy family, fatal /
  crash / pipe recording, the player rule, rec hook in the EndRenderPass call, Java opt-in, gear row, manifest, texts) - pass.
- The whole NR / NB host suite (runner copy pointed at this worktree), NDK build of the changed units (no new warnings), `tools/build_apk.ps1`.

## Risks
- Crash classification by thread: on Mali / PowerVR any native crash on `nvn-translate` / `dm-nbmt` counts, also a bug of our own code that is
  not the driver's (a step then turns things off that may not be the cause; the next crash climbs further, step 3 = v0.1.1 renderer).
- Guest-thread crashes inside our NR-09R direct recorder (NVN level, not Vulkan) are not counted unless pc / lr is in the driver.
- Step 3 turns NB off entirely: a big CPU / perf step back on weak phones - by design the last resort.
- Adreno has no ladder: an NB / NR crash class on Adreno would still just crash (none known).
- `DM_NR_ROTATE` is now read after the physical-device pick instead of the top of `dm_android_vk_initialize` (nothing in between uses it).
- The shared texts in frontend-common changed (no "version 0.5"): DOOM 2016 shows its own resources, so it is unaffected; DOOM / Eternal sessions
  should know the TSV rows changed.
- Not run on a phone.

## KB-ready entry (for docs/port-kb/gpu-mali.md; the main session adds it after the phone run)
```
### MALI-DOS2-03 Безопасный режим Mali / PowerVR в DOS2 (порт MALI-13 / PVR-08 с DOOM 2016; 17.11.2026)
- **Симптом:** риск: умолчания DOS2 на тайлерах (NB с 852c6ec, NR-10 / 27 / 30, NR-08b на PowerVR) ни разу не шли на настоящем Mali / PowerVR;
  после падения драйвера — просто вылет, игроки флаги не ставят.
- **Устройства/GPU:** Mali / PowerVR (реальные и `DM_GFX_SIM_GPU=mali|powervr`); на Adreno — только строка лога.
- **Фикс:** `<files>/gfx-safe-mode` (формат DOOM) считает графические сбои (поток `nvn-translate` / `dm-nbmt` / `dm-nrp*` / драйвера, pc/lr в
  библиотеке драйвера, zero-copy, fatal DEVICE_LOST / зависание GPU; внешние сигналы — нет) → следующий старт: ступень min(count, 3):
  1 — `ZEROCOPY=0`, PowerVR `UBO_DYNAMIC=0` `NR_UBODYN=0`, `NR_RP=0`, `DOS2_PREPASS=0`, `NR_UBOVEC=0`; 2 — + `NOLOAD`, `STORE_HIST`, `CLEAR_FOLD`,
  `TILE_BARRIER`, `LAZY_CLEAR`, `NB_RP`, `NB_OPBARRIER`, `NB_UIAREA`, `NR_DIRECT`, `NR_DRAW`, `NR_ROTATE`, `NR_SH` = 0; 3 — + `PIPE_SERIAL=1`,
  `NB_MT=0`, PowerVR `NOOUT_MASK=0`, `GS_PASSTHRU=0`, все NR / NB и `PRESENT_THREAD` = 0 (рендер v0.1.1). `DM_SAFE_MODE` auto / 0 / on / 1..3;
  ключи игрока (dm/env, окружение старта) важнее, таблица умолчаний приложения — нет. На лету: несобранный пайплайн → `PIPE_SERIAL=1` + тост.
  Сторож DmGuard, уведомление, ⚙ «Безопасный режим графики» (frontend-common). Тест `DM_GFX_FAKE_GPU_CRASH=<с>[:rec|lost|hang]`,
  `DM_GFX_FAKE_FALLBACK=pipe:<с>`. NR-08b по умолчанию 1 на PowerVR (SIM batch 12: ~1000 → 3 записи наборов в минуту), ступень 1 его снимает.
- **Эталон:** dos2-port ветка `dos2-nrsafe` → `xbg_safe_mode.h` / `.inc`, `host/crash.c`, `vulkan_present.cpp`, `jni_android.c` (`dm_env_player`),
  DmActivity / Tl2Gear; docs/dos2/nr/SAFE-MODE.md. Источник — D16 MALI-13.
- **Проверка:** хост-тесты (`safe_mode_test`, `vendor_policy_test`, `source_structure_test` SAFE), APK собран; телефон — лестница под SIM powervr
  (план в SAFE-MODE.md) не прогнана.
```
