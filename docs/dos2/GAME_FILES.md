# Game files: what setup does, and how to do it by hand

Players only need `setup.bat` / `setup.sh` ([INSTALL.md](../INSTALL.md)). This page describes what `tools/setup_dos2.py`
does with your dumps, for developers and for troubleshooting.

## Inputs

| Title | Id | Version | Needed for |
|---|---|---|---|
| Divinity: Original Sin 2 – Definitive Edition | `010027400CDC6000` | v0 | RomFS (base of the BKTR patch) |
| Update | `010027400CDC6800` | **v786432** | ExeFS (`main` build id `4979b200d53bb282e0d11871613286da`, `sdk` `230994acc1eb0f13878dd155361beff3`, SDK 9.3.0) + the BKTR RomFS patch |
| `prod.keys` | — | — | decrypting the packages (your own console's) |
| iPad / iPhone `.ipa` (optional) | `com.larian.dos2ipad` | 3.6.72 checked | its `Data/Game.pak`: the `_t` interface movies whose button art goes into the touch HUD |

Packages may be NSP, NSZ, XCI or XCZ, also split dumps. They are identified by their content meta (CNMT), not by the file
name. The game's languages and the *Gift Bag* content (`CommunityMods.pak`, `SharedDLC*.pak`) are in the base RomFS; the
game imports no add-on content functions, so there is nothing else to install.

## Output (`work/DivinityOriginalSin2Mobile`, copied to `Download/DivinityOriginalSin2Mobile` on the phone)

```text
DivinityOriginalSin2Mobile/
  romfs/Data/*.pak, Data/Localization/...       the merged RomFS (47 files, ~11.4 GB): Larian LSPK v13 packages
  romfs/Data/TouchUI.pak                        the touch interface movies (made by setup, ~2-21 MB, see below)
  dm/out/{main,sdk}.{patched.bin,fixups.tsv,gates.tsv,sites.tsv,...}   the patched modules (15 files)
  dm/shaders/shaders.xgspack                    12 329 SPIR-V bundles in one pack (~271 MB)
  dm/shaders/native-post.xgspack                7 fused post-processing shaders (112 KB, the post-tail fusion DM_NB_POST)
  dm/shaders/native-nr.xgspack                  13 engine shaders by name (180 KB, DM_NR_SH; own GLSL rewrites in your bundles' heads)
  astc/dos2-astc.pack + manifest.json, quality.tsv   ASTC textures for GPUs without BC (~1.5 GB; not needed on Adreno)
  drivers/README.txt                            where your own Vulkan driver packages go (empty otherwise)
```

The app also reads `dm/env` (KEY=VALUE lines, debugging switches) there, and writes `logs/` and `saves/` (save export).
Its own storage (`Android/data/org.dos2.port/files`) holds the Vulkan pipeline cache, the lazy ASTC cache (textures the
pack lacks, encoded on the phone) and the saves.

## Steps

| # | Step | Tool | Result |
|---|---|---|---|
| 3 | ExeFS of the update | `content_reader.py` (plain NSP / XCI) or `extract_exefs_nsz.py` (NSZ / XCZ); by hand: `tools/exefs_nsp.py <update> work/exefs-update` | `work/exefs-update/{main, sdk, subsdk0, rtld, main.npdm}` + `main.raw` (decompressed, `tools/nso_symbols.py`); the build ids are checked |
| 4 | Offline patch | `DM_TARGET=android PYTHONUTF8=1 python dm/tools/patch_nso.py work/exefs-update/<mod> <out> <mod>` for `main`, `sdk` | `dm/out/*` (main: 12 335 sites, sdk: 11 242 sites + 93 host gates). `subsdk0` is a byte-for-byte copy of `sdk` and is not loaded |
| 5 | Patch check | `python dm/verify/equiv.py <out> <mod> work/exefs-update/<mod> --jobs N` | every rewritten site against the original instruction under random states, plus a 300-mutation self-check; must print `EQUIV <mod> OK`. Needs `unicorn` |
| 6 | RomFS | `tools/romfs_d3.py extract <base> <update> <dest>` (the generic base + BKTR reader; NSZ / XCZ: program expanded first with `tools/ncz2nsp.py`) | IVFC hashes verified, BKTR applied, files + `work/romfs-manifest.json` (sha256 of every file) |
| 7 | Shaders | `DOS2_MAIN_RAW=work/exefs-update/main.raw python tools/dos2_shader_jobs.py <romfs>/Data work/shaders-jobs` → `tools/build_dmsh_xlate_d3.py` (.NET 10 SDK, Ryujinx 1.3.2 from `dm/third_party/ryujinx`) → `XLATE=<dmsh_xlate> python dm/gfx/tools/dmsh_vk_xc2.py work/shaders-jobs work/shaders-vk` → `python dm/gfx/tools/xgs_pack.py work/shaders-vk <dm/shaders>/shaders.xgspack` | 6158 `.bshd` programs (`EngineShaders`, `Engine`, `Materials`, `CommunityMods.pak`) + 75 stages built into `main` → 12 329 unique stages (6132 vertex, 6192 pixel, 5 compute) → one pack the engine opens once |
| 8 | Fused post-processing shaders | the five translated bundles PPApplyLUT `921728a5…`, EoCSight `91eb2d97…`, PPVignette `43e8e8ed…`, CombineUI_NoFetch `98186c22…`, PPFXAA `691768aa…` read out of `shaders.xgspack` into `work/native-post/bundles` → `DOS2_SHADER_TOOLS=<dir> python tools/dos2_post_fuse.py --bundles work/native-post/bundles --out work/native-post --n 20` → `work/native-post/fused.xgspack` copied to `<dm/shaders>/native-post.xgspack`; stamp `work/native-post.json` (pack SHA-256, tools hash, input bundles hash) | 7 bundles (F1, F1A, F1AS, F1AR, F2, F3, F3V; XGF1 format, `dm/gfx/nb/nb_fused.h`) spliced from the translated SPIR-V with `tools/spv_splice.py`, each `spirv-val`-clean, interface = the host's, checked against the chain of translated shaders in `tools/spv_interp.py` (`work/native-post/equivalence.txt`). `--n 20` instead of the tool's 60 only takes fewer random cases (same pack bytes, ~45 s instead of ~2 min). Optional: on failure setup goes on, and without the pack the engine draws the chain as recorded. `--skip-post` leaves it out |
| 8 | Engine shaders by name (NR-04) | the ten translated bundles of `tools/dos2_nr_sh_pack.py` ENTRIES (FadeBlending, EoCFog, EoCOutlineCombine, LinearizeDepth, PPDownSampleLog2, DeferredSky, ClusteredLightingWithSSAO, CascadedPCF_3x768 / 2x1024 / 3x512) read out of `shaders.xgspack` → `DOS2_SHADER_TOOLS=<dir> python tools/dos2_nr_sh_pack.py --pack <shaders.xgspack> --out work/native-nr --n 40 --programs work/shaders-jobs/programs.tsv` → `work/native-nr/native-nr.xgspack` copied to `<dm/shaders>/native-nr.xgspack`; stamp `work/native-nr.json` | 13 bundles keyed `<variant>.<engine name>` (x = exact, m = mobile shadows): the translated head byte for byte + this port's GLSL rewrite (`tools/dos2_post`, `tools/dos2_pcf`) compiled with glslc, `spirv-val`-clean, interface = the translated module's; post / lighting bit for bit against the translated shader in `tools/spv_interp.py`, shadows compared and reported (`work/native-nr/equivalence.txt`, `nr_sh.tsv`). Optional (`DM_NR_SH`, off by default); `--skip-post` leaves it out |
| 9 | Touch interface | `python tools/dos2_touchui_pak.py touch --sw <romfs>/Data/Game.pak --out <dir> [--src <iPad Game.pak> | --no-vis --no-panels]` | `romfs/Data/TouchUI.pak`: an extra LSPK package the game mounts by itself (it loads every `*.pak` in `Data`). Console movies with touch handlers (`*_d.swf`, patched from the Switch's own `*_c.swf`), and with the iPad movies the HUD column / panel buttons grafted in. Read back and compared byte for byte |
| 10 | ASTC textures | `python tools/dos2_astc_cache.py <romfs> <astc dir> [--jobs N]` (needs `astc-encoder-py`, `numpy`) | every BC1–BC3 2D texture of the packages (25 145 unique) → ASTC 4x4 / 5x5 / 6x6 at ≥ 42 dB PSNR of mip 0 against the decoded BC, else left BC; `D3AC` pack, two index entries per texture (the file's UNORM format and its sRGB twin, which the renderer may pick at run time) |

Every step is skipped when its output is complete; delete an output to redo it. `setup --steps exefs,patch,verify` (etc.)
runs a subset (steps `exefs,patch,verify,romfs,shaders,post,touchui,astc`), `--skip-verify` / `--skip-post` / `--skip-astc` leave those out, `--keep-nsp` keeps the expanded NSPs, `--ipa FILE` names
the iPad version explicitly, `--work DIR` moves the whole work folder. The results are deterministic: the same dumps give
byte-identical patched modules, RomFS files, shader pack and touch package (checked: a fresh setup run reproduces the
files of the development phone byte for byte; the fused pack too: `native-post.xgspack` = the development `…fusedA`, md5
`f357693f1eb47118414a236b7221bc67`).

SPIRV-Tools are optional: without `spirv-opt` the capability trim is done in Python (same output), without `spirv-val`
the per-shader validation is skipped. Setup finds them on `PATH`, through `SPIRV_OPT` / `SPIRV_VAL`, or in an Android NDK
(`ANDROID_NDK_HOME`, `ANDROID_HOME/ndk/*`).

Step 8 needs `glslc`, `spirv-as`, `spirv-dis`, `spirv-opt` and `spirv-val`, pinned to the Android NDK r29 (29.0.14206865)
`shader-tools` build (shaderc v2022.3, SPIRV-Tools v2022.4 `ndk-r28b`). They live in `tools/.runtime/spirv-tools`: copied
from a local NDK r29 when its files have the pinned SHA-256s, else (after asking) read out of
`https://dl.google.com/android/repository/android-ndk-r29-{windows,linux,darwin}.zip` with HTTP range requests (the zip
directory + those members, ~6.4 / 6.6 / 10.3 MB instead of 0.83 / 0.78 / 1.05 GB), each checked against the SHA-256 in
`SPV_FILES` of `tools/setup_dos2.py`. Linux x86-64 and macOS (universal) also get the NDK's `libc++.so` / `libc++.dylib`
next to the tools. Other machines (Linux on ARM): set `DOS2_SHADER_TOOLS` to a folder with your own builds of the five tools
(the pack is then checked the same way, but may differ in bytes).

## The ASTC pack on the phone

The engine reads `Download/DivinityOriginalSin2Mobile/astc/dos2-astc.pack` only where the GPU does not sample BC
(`textureCompressionBC = 0`: Mali, PowerVR, most Xclipse). A texture is found by its storage when the game creates it
(CRC-32 + Adler-32 of the bytes, size, NVN format, width, height, levels — `dm/gfx/vulkan/engine/xbg_astc_cache.h`), then
created in ASTC and uploaded from the pack; anything the pack lacks takes the GPU BC decoder and is encoded in the
background into the lazy cache in the app's own storage. boot.log: `NVN astc cache: using setup pack … (N entries, … MB) +
lazy …` once, then `lookups …: hits …, misses …` at 64, 128, 256 … lookups. On Adreno the line is
`NVN astc cache: not used (the device samples BC …)` and the pack is never opened. `--match-journal <.journal-lazy>`
compares the pack's keys with a lazy-cache journal pulled from a phone.

## Copying to the phone

`setup push [serial]` copies only missing or size-mismatched files over adb (an interrupted copy resumes; files in
`drivers/` on the phone are never overwritten) and grants the app *All files access*. By hand:

```sh
adb shell mkdir -p /sdcard/Download/DivinityOriginalSin2Mobile/dm
adb push work/DivinityOriginalSin2Mobile/romfs /sdcard/Download/DivinityOriginalSin2Mobile/
adb push work/DivinityOriginalSin2Mobile/dm/out /sdcard/Download/DivinityOriginalSin2Mobile/dm/
adb push work/DivinityOriginalSin2Mobile/dm/shaders /sdcard/Download/DivinityOriginalSin2Mobile/dm/
adb push work/DivinityOriginalSin2Mobile/astc /sdcard/Download/DivinityOriginalSin2Mobile/     # only without BC
adb shell appops set org.dos2.port MANAGE_EXTERNAL_STORAGE allow          # after installing the APK
```

Large files over a flaky USB / wireless adb link: `tools/push_phone_dos2.sh` (and `tools/push_big.sh`) resume in chunks.

## Developer tools (not needed to play)

| Tool | What |
|---|---|
| `tools/lspk.py list / extract <pak>` | Larian LSPK v13 package reader |
| `tools/nso_symbols.py <exefs dir> main sdk` | decompressed images (`*.raw`) and import / export tables (`*.symbols.json`) |
| `tools/dos2_touchui_check.py <main.raw> <TouchUI.pak>`, `dos2_ui_audit.py`, `dos2_swf_abc.py` | touch package checks against the program, SWF / ABC analysis |
| `tools/dos2_native_shaders.py`, `dos2_post_mobile.py`, `dos2_pcf_mobile.py`, `dos2_light_mobile.py` | experimental native / mobile shader packs (`DM_GFX_NATIVE_SHADERS`, off by default) |
| `tools/dos2_nr_sh_pack.py --pack <shaders.xgspack> --out DIR [--programs programs.tsv]` | the engine shaders by name `native-nr.xgspack` (setup stage 8; `DM_NR_SH`, off by default; `docs/dos2/nr/NR-04.md`) |
| `tools/symbt.py <log> [exefs dir]`, `tools/plt.py`, `tools/xref_str.py`, `tools/ipc_reach.py` | backtrace symbolisation, PLT names, string xrefs, SDK IPC reachability |
| `tools/run_dos2.sh`, `hangcheck.sh`, `shots.sh`, `keep_awake.sh`, `rebuild.sh` | device test runs (`ADB_SERIAL`, `ADB`), logs to `work/logs/` |
