# Game-derived information in this repository

Divinity Original Sin 2 Mobile contains **no game code and no game data**: no executables or parts of them, no RomFS
files (LSPK `.pak` packages, `.bshd` shaders, `.swf` interface movies, textures, audio, video, texts), no translated
`.xgs` bundles or shader packs, no textures, art, audio, video, text or fonts, nothing of the iPad / iPhone version, and
no keys. The player supplies the game ([GAME_FILES.md](GAME_FILES.md)) and every game-derived output (ExeFS, patched
modules, extracted RomFS, shader jobs, translated `.xgs` bundles and their pack, the touch interface package, the ASTC
texture pack) is produced on the player's machine into the git-ignored `work/` directory. The app's launcher icon is not
in the repository (`dm/android/dos2/icon.png` is git-ignored; without it the build uses d2r-mobile's icon).

To make the original code run on Android, the repository does contain *facts about* the game, found by analysing the
author's own copy: addresses, short instruction encodings, structure offsets, identifiers and format descriptions. This
page lists them so their extent is clear. They apply only to Divinity: Original Sin 2 – Definitive Edition for Switch,
update v786432 (`main` build id `4979b200d53bb282…`, `sdk` build id `230994acc1eb0f13…`, SDK 9.3.0); the host checks the
original instruction words before it changes anything, and setup checks the build ids.

| What | Where | Extent |
|---|---|---|
| **SDK services** the game needs beyond the sibling ports (save data size / extension, error applet, vibration value, keyboard code map, profanity filter, local network mode, profile image) | `dm/host/hle_dos2.c` | Symbol names (from the exports of the Switch modules) and constants |
| **Widescreen sites**: offsets and original instruction words of the window / render-target sizes, the camera aspect and the video letterbox, rewritten at load time | `dm/host/hle_dos2_wide.c` | Instruction words, each checked before it is replaced |
| **Render quality / dynamic resolution**: the DRS controller, its targets and the render-size fields | `dm/host/hle_dos2_quality.c` | Offsets and constants |
| **Input for touch**: the input manager's `InjectInput` record, the pad poll, raw input ids, the UI cursor, the controller-mode byte, the event filter, the world picking and the item placement (move) mode | `dm/host/hle_dos2_touch.c`, `hle_dos2_touch_world.c` | Offsets, a hook over the pad poll's prologue, record layouts |
| **Console UI for touch**: the UI object creators and type table, movie paths, ExternalInterface entry points, HUD element rects, the button column state | `dm/host/hle_dos2_touchui*.c` | Offsets, type ids and entry points; the movies are read from the player's RomFS |
| **Interface movie patches**: which ActionScript classes / methods of the console `*_c.swf` movies (and, with the player's own `.ipa`, which `*_t.swf` sprites) are changed or grafted, as transformations applied on the PC | `tools/dos2_touchui_pak.py`, `tools/dos2_*_touch.py`, `tools/dos2_swf_graft.py`, `tools/dos2_swf_abc.py` | Bytecode / tag transformations and class / method names; no movie bytes are stored |
| **Renderer facts** for the native-backend / profiling paths (engine draw-command opcodes, pass names) | `dm/host/hle_dos2_nb*.c`, `hle_dos2_native.c`, `docs/dos2/nb_opcodes.tsv` | Offsets, opcode numbers and descriptive names |
| **Shader container layout** (`.bshd`: zlib + GLSLCoutput stages, the 75 stages inside `main`), NVN depth mode set at a quoted address | `tools/dos2_shader_jobs.py` | Parser; shader identifiers (16-hex-digit keys) appear in notes |
| **Package and texture formats**: Larian LSPK v13, the NVN texture-packager container (XTX) and its formats | `tools/lspk.py`, `tools/dos2_astc_cache.py` | Format description and readers; no game files |
| **Reconstructed shader headers and mobile shader rewrites**: the uniform blocks recovered from the GLSLC reflection of the shaders, G-buffer encoding recovered from the translated code, and hand-written mobile versions of a few full-screen passes (same inputs and outputs as the translated ones) | `tools/dos2_nx_headers/`, `tools/dos2_post/`, `tools/dos2_pcf/` | Written for this port from the analysis (block layouts, constants); used only by the optional `DM_GFX_NATIVE_SHADERS` packs, off by default |
| **Analysis notes**: function names, addresses, call chains, measurements, plans | `docs/dos2/*.md` | Text analyses |

Function and type names come from the symbols and RTTI the Switch modules carry (or are descriptive names given by the
author).

What the repository does **not** contain, and the tooling never writes into the source tree: `*.nsp / *.nsz / *.nca /
*.xci`, ExeFS (`main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`), `*.patched.bin`, RomFS files (`*.pak`, `*.bshd`, `*.dds`,
`*.swf`…), shader jobs (`*.rec`, `jobs.tsv`, `shader_map.tsv`…), `.xgs` bundles and `.xgspack` packs, `TouchUI.pak`, the
ASTC pack, `.ipa` files or anything from them, saves, console keys.

The tree also carries the records of the sibling ports built on the same host: Torchlight II (`DM_GAME_TL2`, described in
[../tl2/DERIVED.md](../tl2/DERIVED.md); its address-bound code is behind `DM_TL2_MAIN` and not built for this game),
Diablo III (`DM_GAME_D3`, [../d3/DERIVED.md](../d3/DERIVED.md)), DOOM (2016) patch sites and gates (`DM_GAME_DOOM` code)
and the DOOM Eternal / Xenoblade 2 code paths, each bound to its own game's build id. The base d2r-mobile tree carries its
own D2R-derived tables; they are described in [`docs/DERIVED.md`](../DERIVED.md) and are not used by the Divinity:
Original Sin 2 build.
