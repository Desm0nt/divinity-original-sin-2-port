# Vulkan drivers in the DOS2 port (16.11.2026, branch `dos2-drv`)

Same driver choice and driver manager as DOOM 2016 / Diablo III 0.2.14 (KB `docs/port-kb/drivers-adrenotools.md` DRV-03/04/07/08/09/10),
for Mali / PowerVR / Xclipse phones. Built, **not run on a phone**.

- **Java:** the shared gear files synced from `frontend-common/java` (GearHost, GearSettings, GearPages, GearDisplay, GearText, VkDrivers,
  DriverManager, UpdateNotice, + SafeMode / DmGuard, inert: no `safeMode()`, no manifest entry). Pad / menu files (DoomPadView, FrontMenu,
  PauseMenu, Ui) not synced. `DmActivity`: `driverMultiPick()` true + `DriverManager.picked(host, Intent)` (several zips at once),
  `logsDir()` = `CrashLog.dir` (Download/DivinityOriginalSin2Mobile/logs), `showBootFailure`: `drivers.graphicsFailed()` on the Vulkan start
  error (-6) -> `RestartActivity` (new process with the system driver, the dialog says why). Tl2Gear unchanged (it already lists
  `GearPages.driver`: list, hidden packages with the reason, "Manage drivers…").
- **Native:** `vk_driver.c` = frontend-common copy (MESA_GRALLOC_LINEAR + PANVK_MAPPER_NAMES for Mali packages, `dm_vk_driver_reset_to_system`,
  fallback status); `jni_android.c vk_fallback_to_system` (in-process retry with the system driver when init fails with a custom one before
  the engine started, else `init-failed-restart`); `vulkan_present.cpp dm_android_vk_retry_safe`, per-init reset of `dm_vk_format_list /
  custom_border / descriptor_indexing / cond_render`, test switch `DM_VK_FAKE_INIT_FAIL=1|2`.
- **APK:** `tools/fetch_drivers.py` (= frontend-common, replaces `fetch_turnip.py`) bundles turnip27, turnip27-a710, turnip27-a8xx,
  turnip27-8g2, radv-xclipse-1.8.0, panvk-jim-1.6.0, panvk-zenith-26.3.1 (pinned SHA-256) + `assets/driver-sources.json`; APK 23.5 -> 53 MB.
  Only packages that fit the phone (family, glibc, minApi) are copied to `files/drivers`, again after every APK update (`.dm_origin`).
- **Defaults:** Adreno 6xx/7xx `turnip27` (unchanged); Adreno 710/720/722 -> `turnip27-a710`, 8xx -> `turnip27-a8xx` (before: the same,
  but only once the background manifest download had fetched them, else the system driver); Mali / PowerVR / Xclipse / Adreno < 600 -> system.
- **Delete:** downloaded, installed-from-file and imported packages have "Delete" in the manager (an imported one comes back while its source
  is in Download/DivinityOriginalSin2Mobile/drivers); bundled ones have none (the next start would copy them again).
- **To check on a phone:** Adreno: list + default as before; Mali: PanVK listed, system default, `[vk] MESA_GRALLOC_LINEAR=1` in boot.log
  with PanVK; fallback: `DM_VK_FAKE_INIT_FAIL=1` in dm/env with a custom driver -> dialog, saved choice = system.
- **Issue #7 (23.11, branch `issue7-panvk`):** PanVK JimVulkan (panvk-jim-1.6.0 = r10) emulates BC on phone Malis without a BC decoder
  (`panvk_bc_emu.c`: BC1/2/3/7 -> ASTC 4x4, BC4/5 -> EAC, BC6H -> RGBA16F in a hidden plane, transcoded after every copy) and reports
  `textureCompressionBC=1`, so the engine handed it raw BC; the reporter (Mali-G77, Dimensity 8050) saw wrong colours that the stock driver
  does not show. `vk_driver.c` now sets `PANVK_NO_BC_EMU=1` for Mali packages (`DM_VK_PANVK_BC_EMU=1` keeps the emulation; a dm/env value
  wins): the engine takes the stock-driver BC route. boot.log: `[vk] PANVK_NO_BC_EMU=1` and `[vk] gpu 0 features: ... bc 0`. Not run on a Mali.
