# NR-SHCACHE `DM_NR_SHCACHE`: shadow cascade layers (18.11.2026)

> **shc4 (18.11, branch `dos2-shc4`, §12):** the cache works on the phone (`=1` standing: +2.4 fps, ABBA) but the `=2` verify found cascade-2
> tiles that differ in ~1000 bytes at a fixed spot while every draw hashed as known: the hash took vertex / index buffers and textures by
> address / handle only. Each draw now also hashes a **content version** of every byte range and image it reads (write journal of the backend's
> own writes, the write-protect tracker for CPU-writable pools, image upload numbers); member probes name what moved. Read §12 before the runs.

> **shc3 (18.11, branch `dos2-shc3`, §11):** the first phone runs with armed cascades switched the cache off at the first BUILD
> (`execution 2 met 0 draws, execution 1 had 206`): execution 2 ran with the draw() hook closed (`live` already cleared). Fixed, plus an undo of
> the range's uniform updates before execution 2, a second reasons line (`[nr] shcache why2`) and word-level uniform diffs (`[nr] shcache words`).
> The -9 fps of the `=2` run was the phone's thermal 60 Hz cap, not the cache. Read §11 before any phone run.
>
> **shc2 (18.11, branch `dos2-shc2`, §10):** the first phone runs cached nothing (`armed 0`, `refused` = `ranges`): the shape test asked for
> `state.rt.n == 0`, DOS2 binds its atlas as `SetRenderTargets(1, {NULL}, atlas)`. Fixed (only a bound colour refuses), plus a reasons line
> (`[nr] shcache why`) and a sampled uniform diff (`[nr] shcache uniforms`). Read §10 before the phone runs of §7.

Branch `dos2-nrshadow` (worktree `wt-nrshadow`, base `dos2-nr-all` 2bfc010). No phone tonight (night rules: no adb). **Default 0 on every
family: byte-for-byte the old command stream** (one load in `draw()`, one load per record in `execute()`, one compare at FRAME / PASS /
PASS_END / CLEAR_DS records).

## 0. Summary

| Question | Answer |
|---|---|
| What is cached | The **static casters of each sun-shadow cascade**. A layer image S holds `min(clear, depth of the static casters)` for each cascade tile; a frame whose static set is unchanged copies S into the atlas tile and draws only the dynamic casters (skinned characters, wind trees, moving objects, anything new). |
| Exact? | **Yes, bit for bit**, by the order independence of depth-only `LESS` / `LEQUAL` draws into a fixed-point attachment (the NR-21 proof, `nr_sort.h`): the tile is `min(c, all fragments)`, and `min` is associative and commutative, so `min(min(c, M), R) = min(c, M ∪ R)`. Tested on a depth-test model in every scenario (§4); on the phone `DM_NR_SHCACHE=2` compares the cached tile with the drawn one on the GPU (byte compare at retire). |
| Where the skip happens | **In the backend, at the top of `draw()`**, before create_pass / pipeline / descriptors / NB-MT packets: a skipped draw costs the translator only its hash (~0.3-1 us), the NB-MT recorder and the GPU nothing. **Not on the main thread** (§6 says what that would need). |
| Expected gain, Mi 14, Fort Joy, still camera (combat, dialogue, inventory, standing) | Translator **-2 … -6 ms**, NB-MT recorder **-3.5 … -11 ms**, GPU **-0.5 … -1.0 ms** per frame (static share 60-85 % of 1000-2200 shadow draws; model, §5). Moving camera: **+0.1 … +0.3 ms** translator (probing), no gain. |
| Weak devices | CPU gains about x2 in ms (slower cores, same draw counts); GPU -1 … -2.5 ms on Mali / PowerVR class (shadow pass 2-4 ms there, the tile copies cost 0.4-0.7 ms). |
| Is it proven on the device | No (no phone). The check mode is built for one run that answers everything: how often each cascade would be reused, how many draws it would skip, the hash cost, and a GPU verify of the cached tile (§7). |

## 1. The frame (NB IR dumps ra1, deck; NR-ONCHIP section 1)

```
pass ds=atlas  clear d=1 full            (clear-only IR pass, folded into the next pass's load: CLEAR)
pass ds=atlas  draws 148  load clear     cascade 0 (near)   viewport = its 768x768 tile
pass ds=atlas  draws 187  load load      cascade 1
pass ds=atlas  draws 609  load load      cascade 2 (far)
pass c0=S(R8)  CascadedPCF_3x768 reads linear depth + atlas
```
The atlas is 1536x1536 D16 (`fmt=0x32`), three tiles of a 2x2 grid. Fort Joy (play-1118): up to 3371 shadow draws / 2.9 M vertices a frame,
1.2-1.5 ms GPU; CPU per draw 3.8 us main thread + ~4 us translator + ~6 us NB-MT recorder. 64 % of the shadow vertices are the "fade" family
(`Base_PBR_ST_DEP` & co., NR-33B), and wind trees / skinned characters are in the same passes.

## 2. Design

### 2.1 A range and its draws
A **range** = the records of one IR pass whose only attachment is a depth image (OP_X_NB_PASS `natt==1`, `slot 8`, plan LOAD or CLEAR,
STORE, no cover DONT_CARE, >= 8 IR draws) up to its OP_X_NB_PASS_END. At the range's first draw the backend **arms** it: the atlas must be
depth-only fixed point (D16 / X8_D24, no stencil aspect), 1 level / layer / sample, >= 256 px; the tile = the first draw's viewport (whole
pixels, inside the image); cascade k = the ordinal of the armed ranges of this frame (`p_nb_frame.frame_no`).

Every draw of a range gets a **64-bit hash of everything its depth output depends on**, computed from the decoded state at the top of
`draw()`: draw arguments, index buffer, shaders, the vertex attributes the vertex shader reads with their streams / buffers (stale binds of
unused slots do not count), raster / depth / stencil / bias / clamp / bounds / alpha test / multisample state, viewport, scissor, the
texture / sampler pools, and for every binding the stages declare: the **content** of the uniform range (up to the declared size;
memoized per range by address + size, invalidated by OP_UPDATE_UB), the texture / sampler handles and the registration revision + serial
of each texture. Each draw is also checked **order-free** (`nrs_why`: depth only, no stencil / colour / storage / query / bounds / A2C,
depth effect none or min, no conditional render, no OCC pair), targets the atlas, keeps the tile as viewport, is not indirect, and its
bindings are only uniform / texture / support / c1 (storage, images, bindless sources taint it). A range with any other record than the
allowed state / bind / draw / NB records (`nrshc::op`) or a clear of the atlas inside it is **tainted**: never cached.

### 2.2 The state machine (`dm/gfx/nr/nr_shc.h`, per cascade slot)
- history: hash -> streak (consecutive ranges the hash was present in);
- **BUILD** (no valid layer; the previous range had >= `MIN` draws that would be members): execution 1 draws only the members (streak >=
  `STABLE`) and defers the rest; at the end the tile is copied to S (layer = the members, with atlas, tile and clear value) and the range is
  **executed again from its start state** drawing only the deferred draws (by ordinal, each hash checked against execution 1);
- **HIT** (valid layer, same atlas / tile / clear value, younger than `AGE` ranges, previous range clean): at the first draw S's tile is
  copied into the atlas (OpCommands: the pending atlas clear is flushed first); a draw whose hash is an unconsumed member is skipped;
- **RECOVER** (a HIT or BUILD whose members did not all come, or a tainted draw): the tile is cleared to the frame's clear value (copy from
  Z) and the range executed again drawing everything - the frame stays exact; the layer is dropped;
- **probing**: a cascade with no static set for 4 ranges (camera moving) is hashed only in 2 ranges of every `PROBE` (8): two consecutive
  hashed ranges are enough to see a static set again (hits resume within ~10 ranges of standing still, §4).

Conditions a HIT / BUILD needs besides the layer: a full clear of the atlas earlier in this frame (value recorded), no partial clear /
stray viewport on the atlas this frame, tiles of the frame disjoint, the previous range of this cascade clean and executable again.

### 2.3 Execution 2
`execute(range start, depth)` with `nbord::stop` = the PASS_END record (as NB-17 runs its segments); the state at the PASS record is kept and
put back the way NB-17's `cb_put` does it (restart / sample counters kept, `tex_gen` / `buf_gen` bumped, NB-11 memo invalid,
`nbpipe::reset`), the end state is put back after it. The IR span of the pass is still open (the hook runs before `nbchk::pass_end`), so
the second host pass of the span begins by the old heuristics: LOAD / STORE. A range that crosses a command handle (OP_END at its depth)
cannot be executed again: HIT / BUILD are only started after a range of the same cascade that did not; if it happens anyway the switch goes
off with a red line (counted `unrepairable`).

### 2.4 Images
Per atlas (one at a time; another depth-only image takes over only after 60 frames without the current one): S (layer), Z (cleared to the
frame's clear value when needed), X (check mode: the truth saved). Same format / size as the atlas, tile coordinates identical, transfer
usage only, 4.7 MB each at 1536x1536 D16. All copies go through `OpCommands` (full barriers, layouts tracked, NB-MT packets).

## 3. Switches
| | |
|---|---|
| `DM_NR_SHCACHE` | **0** default (`xbg_nr_shcache_default` = 0 on every family). **1** on. **2** check: decisions made and counted, every draw drawn as before; every `VERIFY` frames per cascade a would-BUILD makes the layer for real next to the frame (truth -> X, Z -> tile, members drawn, tile -> S, X -> tile) and a would-HIT makes the cached tile next to the real one (truth -> X, S -> tile, only the dynamic draws, both read back, X -> tile) and compares them byte for byte at retire. **3** poison: as 1, but a HIT keeps the cleared tile - the static casters' shadows vanish on screen (proves the skip path is live). |
| `DM_NR_SHCACHE_STABLE` | 3: ranges a draw must be present in (with this one) to be a member |
| `DM_NR_SHCACHE_MIN` | 64: members a layer needs (below it no layer is built) |
| `DM_NR_SHCACHE_AGE` | 120: ranges a layer lives, then rebuilt (the one thing the hash cannot see: bytes the engine rewrites in place under a known address / registration) |
| `DM_NR_SHCACHE_PROBE` | 8: probing period of a quiet cascade (0 = hash always) |
| `DM_NR_SHCACHE_VERIFY` | 30: frames between GPU verifies per cascade in mode 2 (0 = none) |
Off by itself when `DM_NB_ORDER`, `DM_NB_RELOC` or `DM_NB_MX` != 0 (they execute the stream out of order / from key frames).

Lines: `[nr] DM_NR_SHCACHE=… stable=… min=… age=… probe=… verify=…` once; every 10 s `[nr] shcache (DM_NR_SHCACHE=m, last 10 s, F frames):
ranges R armed A (not hashed: quiet Q) refused X tainted T; hit H (ok O, recover V), build B (layer L, rest S), normal N [first … clear …
dirty … rect … prev … few … layer … age … backend …]; draws D a frame, skipped K a frame (P %; c0 k/d c1 k/d c2 k/d skipped/drawn by cascade);
execution 2: … draws a frame drawn …; hash U us a draw (ubo … KB a frame, memo …); copies … a frame; verify equal E differ F, side builds G;
unrepairable … desync …; red …` (mode 2 prints `would hit` / `would skip`). Red lines `【红】[nr] shcache: …` (first 16): verify differs,
execution-2 desync, unrepairable range.

## 4. Proof (PC)
- `tests/nr_shc_test.c` (clang 20): **9958 checks PASS**. The backend's call sequence (arm at the first draw, `nrshc_draw` per draw,
  `nrshc_end`, execution 2 by ordinal) on a 64x64 D16 depth-test model (sloped rectangles, LESS / LEQUAL, random order every frame): the
  cached tile equals the tile of drawing everything, **every frame of every scenario**: static only; static + 40 dynamic; a static caster that
  vanishes (exactly 1 recover); a camera move (1 recover); a tainted draw (1 recover); a changed clear value; duplicate draws; few static
  (never cached); all dynamic (never cached, mostly not hashed); walking 60 frames then standing (46 ranges not hashed, hits 33 of the
  37 standing frames). Plus: hash table (multiset take = count, generation clear, half-full refusal), content hash (every byte, length,
  seed), every arm reason, poison (HIT without the copy differs from the truth), execution-2 desync detection.
- `source_structure_test.py` block `NR-SHCACHE`: every hook gated by `nrshc::live` / `nrshc::mode`, the draw() hook is its first statement,
  the PASS_END hook before `nbchk::pass_end`, execution 2 from the kept start state stopped at the end record, check mode never skips, only
  mode 1 copies the layer, default 0, `nr_shc.h` engine-free.
- NDK 29 `-fsyntax-only` of `xbg_game.cpp` (the DOS2 build's defines): 0 errors, no new warning.
- Full host run (copy of `scratchpad/nr/merged/run_tests.sh` pointed at this worktree + `nr_shc_test`): 81 PASS after the APK build
  (the libdm-based checks `dos2_nr_check`, `nr_leancall_test`, `nr_nocall_oracle_obj` need the built `libdm.so`; `t_cpuprof_rt` is the
  known WSL timing flake and passed on the rerun); `nr_state_test`, `nb_onchip_test`, `nr_state_oracle` are not in this base (newer tree).
- APK: `tools/build_apk.ps1` (own `work/build-android`, drivers copied from `wt-nrall/work/drivers`, gradle offline) ->
  `work/DOS2-port-debug.apk` 55 720 125 B, SHA-256 `04449d219d87b136c4a35ec2ce9dfa99364df0df8cf96348a867c5458163764b`;
  `libdm.so` carries `DM_NR_SHCACHE=%d`.

## 5. Expected gain (model; the check run measures the inputs)
Inputs: D shadow draws a frame (deck dumps 944-1116; Fort Joy 1000-2200 typical, 3371 peak), static share s (unknown; trees with wind
and every animated character are dynamic; guess 0.6-0.85 with the camera still), costs per draw: translator 4 us, recorder 6 us, main 3.8
us; hash 0.3-1 us (measured by the line).

| Mi 14, Fort Joy, per frame | camera still (HIT) | camera moving |
|---|---|---|
| translate thread (15-22 ms, the bottleneck) | -(4 x sD) + 0.5 x D = **-1.9 … -6.4 ms** | +0.5 x D x 2/8 = **+0.1 … +0.3 ms** |
| NB-MT recorder (13 ms) | -6 x sD = **-3.6 … -11 ms** (the translator's waits for it, 2.6 ms, mostly go) | 0 |
| main thread | 0 | 0 |
| GPU (shadow 1.2-1.5 ms) | -s x 1.2-1.5 + 3 tile copies (7 MB, ~0.1 ms) + 6 full barriers (~0.2 ms) = **-0.5 … -1.0 ms** | 0 |
| build frame (after every stop, every 120 ranges) | + state records of one range again (~1-2 us x its draws) + 1 copy | - |

Weak devices (Snapdragon 7 / Dimensity / Mali / PowerVR): the per-draw CPU costs are ~2x, so the CPU gains in ms double; the shadow pass is
2-4 ms of GPU there, the copies 0.4-0.7 ms (no UBWC-style fast copy) -> **-1 … -2.5 ms GPU** in still scenes. On tilers the full-atlas
LOAD of cascades 1-2 stays as it is (unchanged by this step). Which share of play is "camera still" in DOS2 (turn-based combat, dialogue,
trading, inventory, looking around) is measured by the check run's `hit` / `normal [few]` counts.

## 6. Why not on the main thread, and what it would need
The main thread spends 3.8 us per shadow draw (NR direct: uniform blocks 0.77, identity 0.45, flush / record 2.56) plus the engine's own
culling and command-list building for the cascade. The backend can skip only because it sees the full draw (decoded state + constant
bytes) and can **recover**: if a member does not come, it executes the range again from the records. The main thread cannot do either:
1. its decision for frame N would come from the backend's frame N-1 / N-2 (the backend runs behind), and a skipped record cannot be
   recovered later (it was never written), so a wrong prediction would be a wrong frame;
2. a proof before recording needs engine-level facts: the cascade's view-projection (the per-view constant block the shadow renderer fills
   at the start of each cascade), and per caster an unchanged world transform, no skinning / wind / dissolve animation, no streaming of its
   mesh / textures - i.e. RE of the shadow cascade renderer (culling + command list generation in `main`), its render-object structures
   (transform dirty flags, skinning state, wind flags) and a hook that drops the static members' commands while keeping the dynamic ones.
A possible middle step later: the NR-09R direct path (`nrd_draw_take`) computing the same hash on the main thread from the engine's state
cache + the constant bytes it has just written, and writing a "member" flag instead of the full record for draws the backend's *current*
layer contains (the backend then still verifies and RECOVERs with records it would have to request again: needs a record ring that keeps
the skipped records, i.e. the saving is only the NVN-call part, 2.56 us). Not done: it needs the phone numbers of this step first.

## 7. Phone runs (Mi 14 Ultra, Fort Joy, 60 s from load; one with the camera still, one walking)
Files: only the APK (no pack). Before: back up `files/save`, check the keyguard (feedback rules).
1. **Check** `DM_NR_SHCACHE=2`: expect the `[nr] DM_NR_SHCACHE=2 …` line, then every 10 s `[nr] shcache (…)`. Read: `would hit` vs `normal`
   and the reasons, `would skip … a frame (… %)` and the per-cascade `c0 k/d c1 k/d c2 k/d`, `hash … us a draw`, **`verify equal E differ 0`**
   with E > 0, `desync 0`, `unrepairable 0`, no `【红】[nr] shcache`. Standing still for 20 s should show hits on c1 / c2 at least.
2. **On** `DM_NR_SHCACHE=1` vs `0` (ABBA, same spot, camera still, 60 s each): `NVN PERF … submit_cpu_ms` (translator) and `[nb] mt`
   (recorder) should drop by about the skipped draws x 4 / 6 us, `gpu_ms` by ~0.5-1 ms; `skipped … a frame` in the line. Look at the shadows
   while walking and standing (no popping, no missing / doubled shadows).
3. **Poison** `DM_NR_SHCACHE=3`, standing still: static shadows (buildings, walls, rocks) vanish while characters / trees keep theirs. Back
   to 0 afterwards.
Weak device (tester): run 1 on a Mali phone (`verify` must be equal there too).

## 8. Files
- `dm/gfx/nr/nr_shc.h` (new): hash, multiset table, cascade state machine, arm / draw / end / execution-2 decisions.
- `dm/gfx/vulkan/engine/xbg_nr_shc.inc` (new): the backend (ranges, hash, order-free check, images and copies, execution 2, check-mode
  verify, the line).
- `dm/gfx/vulkan/engine/xbg_game.cpp`: include + 8 hook lines (draw() first line, execute() record filter, OP_END, OP_CLEAR_DS, FRAME, PASS,
  PASS_END).
- `dm/gfx/vulkan/engine/xbg_vendor_policy.h`: `xbg_nr_shcache_default` = 0.
- `dm/gfx/vulkan/engine/tests/nr_shc_test.c` (new), `tests/source_structure_test.py` (block NR-SHCACHE).

## 9. Risks
- **In-place rewrites the hash cannot see** (vertex / index / texture bytes changed under the same address and registration without any
  record): a stale static shadow for at most `AGE` ranges (default 120 = 2-4 s). The engine's static meshes are created once (NR-06 static
  buffer table); texture streaming registers new textures (revision in the hash). Not seen, not excluded. **[shc4] Seen on the phone (the verify,
  §12) and closed: the hash now carries a content version of those bytes and images.**
- 64-bit hash collisions (as NR-02 / NR-21).
- The GPU verify reads back 2 x 1.2 MB per verify (mode 2 only).
- Execution 2 re-runs state records and OP_UPDATE_UB of the range (same bytes, same order); NB check mode (`DM_NB=2`) would count the
  second host pass of a span as a split and a fully skipped cascade as "no host pass" - informational only.
- A HIT on cascade 0 (plan load CLEAR) relies on the host's rule "plan CLEAR without a pending clear = LOAD" (nb_rp.h) after the copy flushed
  the pending clear; verified by the check mode on the phone, not on the PC.

## 10. shc2: why the phone cached nothing, and the fix (18.11.2026, branch `dos2-shc2`)

### 10.1 Evidence (Mi 14 Ultra, Adreno 750, Turnip, Fort Joy)
- `C_st_shc` (`DM_NR_SHCACHE=1`, camera still 60 s) and `nrB_shc` (`=2`, walking): every 10 s line says
  `ranges 3152 armed 0 (not hashed: quiet 0) refused 3152 tainted 394788; hit 0 ... normal 0 [first 0 ... backend 0]; draws 0 a frame` -
  **every range was refused at its first draw** (`refused` = `ranges`, 4 a frame), so nothing was hashed, decided or skipped. `normal 0 [...]`
  shows the state machine was never reached: the problem sat before it, in `arm()`'s shape test.
- **Cause 1 (all refusals): `state.rt.n == 0`.** The shape test refused any range whose `SetRenderTargets` had a colour count above 0. DOS2
  binds the shadow atlas with **one NULL colour slot**: every `NVN read break` line of the atlas in the logs reads
  `colors=0 ... rt_n=1 ... fmt=0x32 1536x1536` (1016 of 1016 such lines in `ab7rp3`, `c15rp1`, `d2profrp` `.dm.log`: the open pass has 0 colour
  attachments, `state.rt.n` = 1). The rest of the tree already counts only bound colours (`prepass::pre`, `nrmat`, `nrsort`, and NR-SHCACHE's
  own hash: `for i < rt.n: if(color[i])`). The host test never saw it: its model has no `SetRenderTargets`.
- **Cause 2 (the "tainted" number is not about shadows): `OP_REPORT_COUNTER` of the depth pre-pass.** The 4th range a frame is the scene
  depth pre-pass (D24S8 2160x972, one attachment, >= 8 draws; IR dump `p2b-nb/nb_ir_05_f3110_p0.txt`: `pass ds=0x10c944f000 draws 111`), which
  holds the game's occlusion pairs (code 61 / 62 `ReportCounter(SAMPLES)`, NR-03). `tainted 394788 / 788 frames = 501 a frame` against the
  IR line's `NVN a frame ... counter 502` (500 samples reports + 2 timestamps, `samplets reports=500.0`) in the same windows: the old `op()`
  counted every unknown op of a *refused* range as "tainted". That range is now refused for its **stencil aspect** and its ops are counted as
  "ops in refused ranges (no taint)". The occlusion queries do not reach the cascades: `state.count_samples` stays false (the game never resets
  the counter: NVN PERF `occlusion=0.0 ops=0.0/2.0/500.0` = 0 resets, 2 timestamps, 500 samples reports a frame; `DM_GFX_SAMPLES_TS=0` drops them).

### 10.2 The change
- `nr_shc.h`: `nrshc_shape_why()` (pure, tested): no depth / not the PASS's image / stencil aspect / not fixed point / levels-layers-samples /
  < 256 px / **a bound (non-NULL) colour** / another atlas / > 4 cascades -> `NRSHC_RF_*`, counted per reason. `arm()` counts bound colours only.
- `op()` classes the ops outside the allowed list: inside an armed range = taint (counted by opcode); before the first draw = refusal
  `op before draw` (counted by opcode); inside a refused range = nothing (counted as `ops in refused ranges`). **`OP_BARRIER` is allowed** in a
  range (DOS2 keeps one barrier inside a cascade pass, IR `barriers 1`; a barrier changes no byte and execution 2 repeats it harmlessly), and
  `OP_X_NR_STATE` (NR-09S3: State fields only, all read by the hash).
- `hash()` returns the first taint reason of a draw (`NRSHC_T_*`: indirect, target, colour, viewport, vertex slot, storage, binding, order
  with the `nrs_why` code) and a **structure hash** (everything but the uniform bytes): `h = mix(hs, chained uniform content hashes)` - the
  same inputs as before. `nrshc_draw` takes `hs` and counts per range: draws known from the previous range / new by uniform bytes only / new
  otherwise / not compared. No decision reads these counters.
- **Cascade slot by tile** (`nrshc_pick_slot`): a range takes the free slot that last held the same viewport rectangle, so a frame where an
  earlier cascade has < 8 draws (no range) does not shift the later cascades into other slots (that drops their layers, `layer other`).
- Sampled uniform diff (`DM_NR_SHCACHE_DIAG`, default 64, 0 = off): per cascade, range r (r % 64 == 0) keeps each draw's uniform ranges
  (stage, slot, size, content hash, bytes, <= 4 MB) under structure hash + occurrence; range r + 1 compares: where the structure matches but a
  uniform range's bytes differ it counts (stage, slot, size) and ORs the 16-byte rows that moved. A per-frame constant (time, TAA jitter, a
  cascade matrix that follows the camera by sub-texels) shows as one row mask in nearly every draw of a cascade.

### 10.3 New lines (every 10 s, right after the `[nr] shcache (...)` line)
```
[nr] shcache why (last 10 s): refused R [stencil aspect a, ...] (before the first draw: (op N x n ...)); tainted ranges T: by ops X (op N x n),
  by draws D [viewport .., order .. (stencil .., ...)]; ops in refused ranges I (no taint); draws a frame known / new by uniform bytes /
  new other / not compared: c0 k/u/o/n c1 ... c2 ...
[nr] shcache uniforms (sampled: range pairs 1 of every 64 per cascade, last 10 s): ranges compared C, draws N: same uniforms E, uniform bytes
  changed U, no structure match M (bytes not kept B); changed by (stage slot size): vs u3 256 B: 412 draws, rows 0x30 (bit i = bytes
  16i..16i+15 of the first 1 KB), 0 past 1 KB, max 2 rows a draw; ...
```
Reading them: `refused` should be ~1 a frame (`stencil aspect`: the pre-pass) and `armed` ~3 a frame (the cascades). Standing still,
`known` should be most of each cascade's draws (static casters) and `new other` the animated ones; a large `new by uniform bytes` with one
row mask in the uniforms line = a constant that moves every frame in a block the static casters read -> §10.5.

### 10.4 Expected gain (Mi 14 Ultra, Fort Joy standing; the run measures it)
Inputs: ~680 shadow draws a frame at this spot (`nrB_still2-heat.tsv`, FJ, the 1536x1536 rows; `C_st_shc`: 1617 draws a frame in all),
static share s = 0.6-0.85 (unknown until the why line), per skipped draw ~3-4 us translator and ~1 us NB-MT recorder (`[nb] mt` worker
replay 1.66 ms for ~1617 draws), hash 0.3-1 us a draw.

| per frame | standing (hits) | walking |
|---|---|---|
| translator (submit_cpu 12.2 ms) | -(3.5 us x s x 680) + hash = **-1.0 ... -1.9 ms** | probing 2 of 8 ranges: **+0.1 ... +0.2 ms** |
| NB-MT recorder (worker thread) | **-0.4 ... -0.6 ms** | 0 |
| GPU (12.3 ms; shadow 0.8-1.2 ms, GPU_BREAKDOWN) | -s x shadow + 3 tile copies / barriers (~0.15 ms) = **-0.35 ... -0.85 ms** | 0 |

The frame is co-limited (submit_cpu 12.22 ms vs gpu_ms 12.27 ms at 78.8 fps in `C_st_shc`), so standing ~+5-10 fps if both drop as
modelled; walking ~0. `DM_NR_SHCACHE_DIAG` copies ~0.2-0.5 MB of uniform bytes in 2 of every 64 ranges per cascade: `=0` for the timed A/B.

### 10.5 If the cascades are not static in DOS2
If the why line shows `known` ~0 and `new by uniform bytes` ~ all draws with the camera still, a constant in a uniform block the casters
read changes every frame. The uniforms line names the block (stage, slot, size) and the rows. Smallest exact variants, in order:
(a) the rows are a time / frame constant the static casters' shaders do not read -> hash only the bytes the SPIR-V can read (a per-shader
read mask of each uniform block from its access chains; a dynamic index = the whole block) - exact, needs the SPIR-V scan;
(b) the rows are the cascade view-projection (it follows the camera by sub-texels while standing) -> nothing in that cascade is reusable bit
for bit; the far cascades still are if their matrix holds (the per-cascade `known` numbers tell which), and the slot machine already
decides per cascade. Neither is implemented before the run says which.

### 10.6 Phone runs (Mi 14 Ultra, Fort Joy, 60 s from load each; APK only; save backup + keyguard check first)

| # | env (dm/env, besides `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_FPS=0`) | scene | what to read |
|---|---|---|---|
| 1 | `DM_NR_SHCACHE=2` | standing 60 s | `armed` ~3 a frame, `would hit` > 0, `would skip N a frame`, `verify equal E differ 0` (E > 0), `desync 0`, `unrepairable 0`, no `【红】[nr] shcache`; the why and uniforms lines |
| 2 | `DM_NR_SHCACHE=2` | walking 60 s | `normal [few ...]`, `not hashed: quiet` > 0, no red line |
| 3 | `DM_NR_SHCACHE=1 DM_NR_SHCACHE_DIAG=0` vs `DM_NR_SHCACHE=0` (ABBA, 4 runs) | standing, same spot | `NVN PERF submit_cpu_ms` / `gpu_ms`, fps; `hit H (ok H, recover ~0)`, `skipped N a frame` |
| 4 | `DM_NR_SHCACHE=3` | standing | screenshot: building / wall / rock shadows gone, characters' and trees' shadows kept; back to 0 after |

The line that shows hits (run 3, mode 1): `[nr] shcache (DM_NR_SHCACHE=1, last 10 s, F frames): ranges R armed A ... refused ~F ...; hit H
(ok H, recover 0), build B (layer B, rest 0) ...; draws D a frame, skipped K a frame (P %; c0 k/d c1 k/d c2 k/d skipped/drawn by cascade)`
with H > 0 and K > 0 (mode 2 prints `would hit` / `would skip`).

### 10.7 Tests / build
- `nr_shc_test`: 10528 checks PASS (new: the refusal reasons incl. DOS2's `rt.n 1 / NULL` atlas and the D24S8 pre-pass, the row diff, the
  slot by tile, the stability counters: still = all known, moved matrix = all "new by uniform bytes", animated = "new other", exact every frame).
- `source_structure_test` block NR-SHCACHE extended (shape by bound colours, op classes, reasons line); NDK `-fsyntax-only` of `xbg_game.cpp`:
  0 errors, no new warning.
- Full host run (copy of `scratchpad/nr/merged/run_tests.sh` pointed at this worktree, libdm built): **87 PASS, 0 FAIL**.
- APK: `tools/build_apk.ps1` (own `work/build-android` configured like wt-nrall's, drivers / libadrenotools copied from wt-nrall) ->
  `work/DOS2-port-debug.apk` 56 002 749 B, SHA-256 `c8c5d8c6ac1911c2b07d1429e330c1a0bbbb4945210b6fc755a912dd5b39e0a7`; `libdm.so` carries
  `[nr] shcache why` and `DM_NR_SHCACHE_DIAG`. Not run on the phone.

## 11. shc3: the first BUILD switched the cache off; what the four phone questions were (18.11.2026, branch `dos2-shc3`)

### 11.1 Evidence (Mi 14 Ultra, Adreno 750, Turnip, Fort Joy beach, standing; APK `c8c5d8c6` = 97aa5f5; `src/work/logs/C_st_*`)
| run | env | red line (first and only) | fps (steady windows, median) |
|---|---|---|---|
| `C_st_base2` | `DM_NR_SHCACHE=0` | - | 78.7 (141 windows; submit 12.10, present 0.41, gpu 12.28) |
| `C_st_shc2` | `=2` check | 20:14:03 `execution 2 met 0 draws, execution 1 had 206 (range seq 751 node 45) - switch off` | 79.2 in the 93 windows without the cap, 59.5 in the 50 windows after 20:16:19 (thermal 60 Hz, §11.5) |
| `C_st_shc1` | `=1` | 20:19:19 `... had 206 (range seq 721 node 45)` | 78.8 (125; 12.13 / 0.37 / 12.29) |
| `C_st_shc3` | `=3` poison | 20:22:20 `... had 257 (range seq 906 node 46)` | 79.2 (115; 12.00 / 0.45 / 12.21) |

"Steady" = the `NVN PERF v=1` windows with >= 1550 draws (the standing scene: 1620-1640 draws, 13 passes). The only `[nr] shcache` summary
with cascades (`=1`, 20:19:17) covers a 10-s window of 528 FRAME records of which ~17 were in the level (`ranges 69 = armed 51 + refused 18`):
everything "a frame" in it is diluted ~30x.

### 11.2 Point 1: "execution 1 / execution 2" - our own re-execution, and its draw hook was closed
- **Execution 1** is the normal, in-stream execution of a cascade pass's records (OP_X_NB_PASS .. OP_X_NB_PASS_END). **Execution 2** is not a
  second submit, a command-buffer reuse or another path: it is NR-SHCACHE's own `again_run()` - `execute(R.start, depth)` from the state kept at
  the PASS record, stopped at the PASS_END record - which a BUILD (draw the deferred draws), a RECOVER (draw all) and the check mode's side build /
  verify (`=2`) run at PASS_END.
- **Root cause:** the draw() hook is `if(nrshc::live && ... && nrshc::draw(...))` and `pass_end()` sets `live = false` *before* it calls
  `again_run()`. So in execution 2 no draw reached `nrshc::draw()`: none hashed, none counted (`ord` stayed 0), none selected (the hook never
  says "skip", so execution 2 drew **every** draw). `ord (0) != ndraw (206)` -> desync red line -> mode 0. It fires at the **first BUILD** of the
  run: `=1` had `build 0` in its only summary and switched off 1.8 s later; `=2` / `=3` the same 3-5 s after the level appeared. The picture of
  that one frame was still exact (members drawn twice = `min` idempotent), so no visible glitch, but the cache could never work.
- The host test never saw it: it drives `nr_shc.h` directly, the `live` gate is in the backend.
- **A second hazard on the same path (found while fixing it): `OP_UPDATE_UB` inside a range.** The backend writes an update's bytes into memory
  when the record runs. Execution 2 starts with memory as execution 1 *left* it, so a draw that came before an update of its block would read
  the later bytes: hash desync and, worse, a draw with the wrong constants. Now every update inside an open range has its old bytes noted
  before the record runs (`nrshc_undo`, 256 KB / 1024 entries per range) and `again_run()` writes them back in reverse order first (the same
  `cpu_access` / `zc::mark` / `invalidate_cpu_range` / `memcpy` sequence as OP_UPDATE_UB): execution 2 sees the range's start memory and replays
  the same updates in order. A full log makes the range "not executable again" (like one that crosses a command handle). Whether DOS2 has updates
  in its cascades is unknown: `why2` counts them.

### 11.3 Point 2: the stencil-aspect refusals are the scene pre-pass, not the cascades - the refusal is right
- `refused 18 [stencil aspect 18]` against `armed 51` = 1 refused + 3 armed ranges a frame for 17 frames. The armed ones are the cascades of the
  1536x1536 atlas, **NVN `DEPTH16` (0x32, `nvn.h`), no stencil aspect** (§10.1: every `NVN read break` of the atlas reads `fmt=0x32 1536x1536`).
  The refused one is the D24S8 2160x972 scene depth pre-pass (§10.1): `ops in refused ranges 18666 / 18 = 1037` records a range - its ~111 draws
  plus the 500 `ReportCounter(SAMPLES)` of the occlusion pairs. It writes the scene's stencil and holds queries: not a shadow pass, never order-free.
- The cascades were not "tiny": `draws 13 a frame` is 6864 hashed draws / 528 FRAME records; per armed range it is c0 ~62, c1 ~93, c2 ~248
  (`c0 0/2 c1 0/3 c2 0/8` x 528 / 17, rounded), ~400 shadow draws a frame at this spot. The same window says `few 48`: 48 of 51 ranges had
  fewer than 64 repeat draws - they were the first ~17 frames after the level load (the camera settles).
- No change to the refusal. `why2` now names the refused images (`2160x972 vk 129 asp 6 x18 (111 IR draws)` is the expected text): if the
  D16 / X8_D24 atlas (`vk 124` / `125`, `asp 2`) ever appears there, that is a bug.

### 11.4 Point 3: the uniform bytes - measured on the first two frames after the load; caching is possible
- The sampled diff compares range pair (0, 1) of each cascade, then (64, 65), ... In `=1` the only pairs compared (`ranges compared 3`) were
  **ranges 0 and 1 of each cascade, the first two frames after the level appeared** - the cache switched off before range 64. They show the
  start-of-level camera / cascade fit moving, not a static scene.
- What the blocks are (structure, no shader RE): `vs u12 192 B` = three 4x4 float matrices (rows 0-2, 4-6, 8-10 changed, rows 3 / 7 / 11
  never: an affine matrix's constant `0 0 0 1` row) per caster; `vs u12 208 B` the same + one vec4 (row 12: a per-object parameter); `vs u0 64 B`
  one affine matrix (rows 0-2) in 96 draws - the cascade's view-projection or a per-view block; `vs u13 128 B` row 7 only. Rows 0-2 of an affine
  matrix change for a camera-relative translation *and* for a re-snapped orthographic cascade (both move the translation column, stored as the
  `.w` of rows 0-2), so the row mask alone cannot tell them apart - the new word diff can (§11.7).
- **The decisive fact is the red line itself:** a BUILD is only armed when the previous range had >= `MIN` (64) draws whose **full** hash (incl.
  every uniform byte) was also in the range before it, and its members are draws present in 3 consecutive ranges. All three runs reached a BUILD
  1.8-5 s after the load (206 / 206 / 257 draws in that range): at least 64 casters of one cascade repeated **bit for bit** for 3 frames once the
  camera stopped. So the light / camera matrices do not change every frame while standing still, at least for that cascade; a cache in "exact
  bytes" is possible. No transformed-space compare is needed (and none would be exact: a camera-relative world matrix times a compensated
  light matrix is equal in math, not in float bits).
- What it cannot settle yet: the static share per cascade while standing for 60 s (c0 follows the camera most closely). The next `=2` run
  prints it (`why2` per armed range `known / new by uniform bytes / new other`).

### 11.5 Point 4: "switched off" was already free; the -9 fps was the phone's thermal 60 Hz cap
- After the red line every hook is one load (`live` false, `mode` 0; FRAME / PASS / CLEAR_DS gated by `mode`): nothing of NR-SHCACHE runs.
- `C_st_shc2` ran 11 s after `C_st_base2` ended (skin 44.8 C at its start vs 41.3 at base2's start), and was the longest run (3.5 min). At
  **20:16:19.212** logcat has `SettingsProvider: refresh rate settings changed, name:thermal_limit_refresh_rate,value:60,pkg:com.miui.powerkeeper`
  and `DisplayModeDirectorImpl ... render: (0.0 60.0)`; from the next window on fps sits at 58.8-60.7 with `present_cpu` 2.4-9.5 ms (the vsync
  wait), 50 of 143 steady windows. The windows before it, with the cache already off (20:15:39-20:16:17), read 79.0-79.5 fps, submit 12.0,
  present 0.4-0.5, gpu 12.2 = base2. The `.sum.txt` medians mix both. `=1` and `=3` (no cap) equal the base: 78.8 / 79.2 vs 78.7.
- The load-in windows of `=2` (20:14:04-20:15:36, 1443 / 2250 draws, cache already off) show present 3-4 ms where base2 had 0.3; the cache
  code was not running there either (hot phone, power policy: not attributable to NR-SHCACHE).
- Change: a switch-off now goes to mode -3, and the next FRAME frees the layer images (S, Z, X: 3 x 4.7 MB) through the recording slot's retire
  (no use in flight) and sets 0. The fps cost was already 0.
- **Rule for the phone runs (feedback "2 min cooling after every 3 runs"):** one ~60 s run each, a 2 min pause before an A/B pair, and look for
  `thermal_limit_refresh_rate` in the logcat before reading fps.

### 11.6 The change (behind `DM_NR_SHCACHE`, default 0 unchanged)
- `again_run()`: `live = true` around `execute()` (the draw() hook sees execution 2), a new uniform-memo generation, the undo of the range's
  updates first; `draw()` keeps counting in execution 2 after a desync (`mode -2`); the red line names cascade, action, how and updates.
- `op(o, p, n)` (hook passes the payload): OP_UPDATE_UB bumps the memo in both executions and notes its old bytes in execution 1 (`upd()`,
  `nrshc_undo_note`); `simple` also needs `!ud->over`.
- `nr_shc.h`: `nrshc_undo` (note / reverse restore), `nrshc_words` / `nrshc_same_delta` (diagnostics).
- `switch_off()` (mode -3) instead of `mode = 0`; `release()` at the next FRAME.
- Diagnostics: `why2` line, `words` lines, frames with an armed range, per-cascade armed ranges, the diff skips range 0 (records at 64, 128, ...).

### 11.7 New lines (every 10 s, after `[nr] shcache why`; the old lines are unchanged)
```
[nr] shcache why2 (last 10 s): refused images: 2160x972 vk 129 asp 6 x527 (111 IR draws); frames with an armed range F of N; per armed range
  known / new by uniform bytes / new other / not compared: c0 (R ranges) k/u/o/n c1 ... c2 ...; uniform updates in ranges U (B KB), undone for
  execution 2 X, log full Y; executions 2: rest a all b drawn c, D draws seen
[nr] shcache words (vs u12 192 B, 666 draws): words 0x88808880888 (bit i = bytes 4i..4i+3), deltas as the first draw's S / other O (e.g. word 3 12.5 -> 12.75)
```
Reading them: `words` with only bits 3 / 7 / 11 of each matrix (`0x888`, `0x888 << 16`, `0x888 << 32`) and `deltas as the first ~ all` = one
shift common to every caster (camera-relative positions, or a re-snapped cascade); whole rows (`0xfff...`) = a rotation (the camera turned);
`other` large = per-object changes (animation / wind) - those casters are dynamic and simply stay out of the layer.

### 11.8 Expected gain (Mi 14 Ultra, Fort Joy beach standing; the `=1` run measures it)
Inputs (these logs): ~400 shadow draws a frame here (c0 ~62, c1 ~93, c2 ~248), hash **1.29 us a draw** measured (more than the 0.3-1 us model),
~3.5 us translator per skipped draw (§10.4), shadow GPU 0.8-1.2 ms; static share s unknown (BUILD evidence: >= 64 of 206 in one cascade).
| per frame, standing | s = 0.5 | s = 0.8 |
|---|---|---|
| translator: -3.5 us x s x 400 + 1.29 us x 400 | -0.2 ms | -0.6 ms |
| GPU: -s x 1.0 ms + copies / barriers 0.15 ms | -0.35 ms | -0.65 ms |
The frame is co-limited (submit 12.1 / gpu 12.3 ms at 78.7 fps), so the fps gain is bounded by the smaller drop: **~+1 ... +4 fps standing**
(79 -> 80-83), 0 walking (probing +0.1 ms). Honest: small at this spot; the hash cost eats ~40 % of the translator saving. Larger in heavy
scenes (3371 shadow draws peak) and on weak devices (x2 per-draw cost). If the `=2` run shows `known` ~0 per armed range while standing for
60 s, the result is "infeasible on DOS2 in exact bytes" and the switch stays 0.

### 11.9 Phone runs (Mi 14 Ultra, Fort Joy beach, the same spot, 60 s from level load each; APK only; save backup + keyguard check first)
| # | env (dm/env, besides `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_FPS=0`) | scene | decisive lines |
|---|---|---|---|
| 1 | `DM_NR_SHCACHE=2` | standing 60 s | **no `【红】[nr] shcache`**; main line `would hit H` > 0, `would skip K a frame`, `verify equal E differ 0` (E > 0), `side builds` > 0, `desync 0`, `unrepairable 0`; `why2`: `executions 2: drawn c` > 0 with `D draws seen` = the ranges' draws, per armed range `known` (static share by cascade), `uniform updates in ranges` (0 or undone); `words` lines (what moves) |
| 2 | `DM_NR_SHCACHE=1 DM_NR_SHCACHE_DIAG=0` vs `=0`, ABBA (4 runs, 2 min cooling before) | standing | `hit H (ok ~H, recover ~0)`, `skipped K a frame`; `NVN PERF submit_cpu_ms` / `gpu_ms` / fps over the steady windows; logcat has no `thermal_limit_refresh_rate` |
| 3 | `DM_NR_SHCACHE=3` | standing | screenshot: building / wall / rock shadows gone, characters' kept (poison works); back to 0 |
| 4 | `DM_NR_SHCACHE=2` | walking 60 s | `normal [few ...]`, `not hashed: quiet` > 0, no red line |
Run 1 decides: `verify differ 0` with `E > 0` = exact on the device; `known` per range = the static share (gain). Run 2 only if run 1 shows hits.

### 11.10 Tests / build
- `nr_shc_test`: **10628 checks PASS** (new: uniform updates inside a range - without the undo execution 2 desyncs, with it every hash
  matches, memory after execution 2 = after execution 1, the tile exact every frame, 1 build + 37 hits; reverse restore of repeated
  addresses; full log by bytes and by entries; the word diff and "same shift" test).
- `source_structure_test` NR-SHCACHE block extended (execution 2 opens the hook, updates undone, memo new, the switch goes off only through
  `switch_off()`, counting to the end) PASS; NDK `-fsyntax-only` of `xbg_game.cpp`: 0 errors, the same 13 warnings as wt-nrall.
- Full host run (copy of the shc2 runner pointed at this worktree, libdm built): **87 PASS** (86 + `t_cpuprof_rt`, the known WSL timing flake,
  PASS on its rerun).
- APK: `tools/build_apk.ps1` (own `work/build-android` configured like wt-nrall's, drivers / libadrenotools copied from wt-nrall) ->
  `work/DOS2-port-debug.apk` 56 027 325 B, SHA-256 `44d7132c67fd34146f4ae04fbf227ac7f21218b232f94978e3df8e863040b4bd`; `libdm.so` carries
  `[nr] shcache why2`. Not run on the phone.

## 12. shc4: the verify differs - the hash did not see vertex / index / texture contents (18.11.2026, branch `dos2-shc4`)

### 12.1 Evidence (Mi 14 Ultra, Adreno 750, Turnip, APK `44d7132c` = 26b1307; `src/work/logs/C_s4_*`, `C_m4_*`, `C_ab_*`)
- **The cache works.** `C_s4_shc2` (`=2`, standing on the Fort Joy beach): `would skip 388 of 828 shadow draws a frame (46.9 %; c0 0/4 c1 71/142
  c2 317/682)`, `would hit 1554 (ok 1554, recover 0)` per 10 s, hash 1.04 us a draw, `desync 0`, `unrepairable 0`.
- **ABBA standing** (`=1 DM_NR_SHCACHE_DIAG=0` vs `=0`, `C_ab_base1 / C_ab_shc1a / C_ab_shc1b / C_ab_base2`, no thermal cap): base **78.5 / 78.1**
  fps, `=1` **80.6 / 80.8** fps -> **+2.4 fps**; GPU median **12.34 -> 11.94 ms**, submit (translator) **12.25 -> 11.60 ms**. Poison `C_s4_shc3`
  (`=3`): 82.6 fps (the static shadows are not drawn at all). `=1` itself: `skipped 388 a frame (46.9 %)`, `hit 1600 (ok 1600, recover 0)`.
- **The verify differs, and where.** `C_s4_shc2`: 9 red lines, all at 21:36:25-21:36:33, i.e. in the **3D main menu** before the save was loaded
  (`[touchui] ... mainMenu_d.swf`, 786 shadow draws a frame, `c0 109/146` - the menu camera is fixed): every one cascade 2, range node 47, first
  pixel **(513, 133)** (+-1 / +-2), **815-1025 bytes** (~400-500 D16 pixels), the count changing from verify to verify. After the load (from 21:37:10,
  Fort Joy standing) **verify equal 24, 32, 10, 14, 28, 24, 24, 28, 28 and differ 0 in every window**: the standing scene was exact all along.
  `C_m4_shc2` (walking): the same 10 menu differs (node 47, (513, 133)), then in Fort Joy cascade 2 at node 125 (490, 214, 1016-1166 B) and nodes
  194 / 190 / 136 (456, 350, 1168-1611 B) - 30 red lines in all, 16 printed.
- **What that pattern says.** A verify compares `min(S, the dynamic draws now)` with the truth; S holds the members as they were at the side
  build. Equal hash => equal fragments only if every input of the draw is in the hash. A fixed spot whose differing area changes size over
  seconds is a **shape that keeps moving under an unchanged hash**: a caster with bit-equal state and uniforms (a static transform - so it was a
  member) whose vertex data moves (a flag / sail / cloth, or an animated mesh written into the same buffer). Ruled out on the evidence:
  - texture streaming: `NVN PERF hash_sites ... upload=0.00/0` and `texdirty changed=0.0` in the menu windows 21:36:22-21:36:28, which hold two of
    the differs - no image changed in the backend there;
  - an atlas pass outside a range: the IR dumps of the same menu frames (`ra1-dumps`, `rb1-dumps`, `rc1-dumps`, 206 / 257 / 653 draws) show
    exactly one clear + three passes on the atlas;
  - uniform bytes past 1 KB: a uniform range is hashed to its declared size (6208 B for `vs u0`), the 1 KB is only the row-mask diagnostic.
  The backend writes ~23 ranges into guest memory every frame both in the menu and standing (`NVN PERF snapfront invalidations=23.0`): uploads
  happen every frame. Which channel moved the menu's flag - an in-stream upload (`OP_COPY_B2B`, NR-TRX §8.1: DOS2's vertex uploads) or the game's
  CPU writing a mapped pool - is not in these logs; the probes of §12.4 name it in the next `=2` run.
- **What the old hash took by address / handle only** (`hash()` of shc2/shc3): vertex streams (`state.vb[k].address`, size), the index range
  (`ix->ibuf`), texel buffers and images (handle + registration revision + texture serial), sampler parameters (by handle). Any byte rewritten
  under them left the hash unchanged; AGE (120 ranges) was the only bound (§9).

### 12.2 The fix (behind `DM_NR_SHCACHE` > 0, default 0 unchanged)
Every hashed draw of execution 1 mixes a **content version** of everything it reads by address into its hash (`Ver`: J, P, T):
- **J - the write journal** (`nr_shc.h nrshc_wj`): `snapshot_invalidate()` - the one function every write of the backend into guest memory goes
  through (replay CPU writes `OP_COPY_B2B` / `OP_UPDATE_UB` / `OP_X_CLEAR_BUF` / counter zeroing via `invalidate_cpu_range`, GPU writebacks at retire
  and at record time, storage writebacks, reports, events) - now calls `nrshc::wrote(address, bytes)` first; pool add / del (`xbg_legacy_pool_*_now`)
  too. The version of a range = the max serial of the page (4 KB) / block (64 KB) slots it covers. Every vertex stream the vertex shader reads, the
  index range (`count << itype` bytes from `ibuf`) and every texel buffer (its texture storage) is versioned.
- **P - the game's CPU** (`DM_NR_SHCACHE_CPUW`, default 1): a range in a pool that is not `NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS` (`nvn_pool_flags`,
  memoized per range) may be written by the game without any record. It is versioned by the HLE's write-protect tracker: new
  `nvn_dirty_version()` (nvn_dirty.c) = 1 when every page is protected now, with the pages' newest write generation and the pool's registration
  number. A range not protected is protected (`nvn_dirty_protect`) and is **no member this time** (salted hash); one found written again within 8
  frames of its protect is left alone for 600 frames (no fault storm) and never a member meanwhile; untracked = never a member. `=0`: CPU-writable
  ranges are never members; `=2`: trusted (journal only, NOT exact - an A/B of what the tracker costs).
- **T - images**: for each sampled texture `texver()` calls `get_image(texture, true)` - the check / re-upload the draw's own descriptors would make
  at this point (once a submission), so a skipped member re-uploads exactly when the drawn draw would - then mixes the image's **upload number**
  (`Image::nrshc_up`, a new number at every full upload and every partial re-upload); a GPU-written image = no member. Sampler registrations:
  `memo_bump[1]`. The texture map is the one of the draw's current pool (`cur_tmap`), not the range's first draw's.
- **Execution 2** never versions again: it takes execution 1's version per ordinal (`nrshc_range.v[]`, `nrshc_again_v`) - its own protects / the
  undo's journal writes cannot desync it.
- **An atlas pass that is no range** (< 8 draws, another shape) with draws now dirties the frame (`dirty`, counted `atlas passes outside a range`):
  a layer copied over a tile would erase what such a pass drew. Not seen in DOS2's dumps; it was a hole of the same family.
- A draw whose inputs moved is simply "new" for a few ranges: it is drawn (dynamic) while it changes and becomes a member again `STABLE`
  ranges after it stops. A HIT whose member did not come recovers, as before.

### 12.3 Why it is exact (tests in §12.6)
- **Journal:** a write with serial s sets the slot of every page and block it touches to s, larger than every slot before; any later query of a
  range it touched reads one of those slots (pages of a partly covered block by page slot, a fully covered block by block slot), so its version
  is >= s > every earlier version of that range. A version changes only when a slot it reads changes (a false change = the draw is drawn).
  The journal sees every byte the backend writes into guest memory because `snapshot_invalidate` is the common point of those writes
  (`source_structure_test` pins the hook as its first statement).
- **Tracker:** a protected page cannot change; a write faults, which raises its generation above every earlier one and leaves it writable; it
  is protected again only by a later protect, so "every page protected, newest generation g" always stands for one content. A pool registered
  again at the same address starts at generation 0: the registration number tells it apart. Game writes follow NVN fences (the guest fence of
  a frame completes only after our Vulkan fence), so a buffer the frame being translated reads is not written meanwhile (the zero-copy model).
- **Images:** the backend's image content changes only at an upload / partial re-upload (new number) or by the GPU (no member). The check runs
  at the same point of the stream the truth would run it (the first draw of the submission that reads the image), so the cached frame and the
  truth see the same image contents.

### 12.4 New diagnostics
- Line every 10 s after `why2`: `[nr] shcache inputs (last 10 s): byte ranges versioned R: CPU_NO_ACCESS pool a, CPU pool protected and
  unchanged b, protected now c, volatile d, untracked e, trusted f; images I (GPU-written G), texel ranges X; draws without a version (no member)
  D; new by content per armed range (J backend writes / P CPU pages / T images): c0 x (j/p/t) ...; atlas passes outside a range O; journal writes W
  (KB), pools P; verify probes: members noted N, compared C, changed vertex v index i texture t, not noted m`.
  "New by content" = a draw whose structure and uniform bytes were in the previous range but whose content version moved (per channel) - the
  class of the shc3 differs, counted instead of drawn wrong.
- **Member probes (`=2`):** at a side build every member drawn into the layer has its inputs noted (the bytes of its vertex streams and index
  range up to 1 MB each; per texture the image's upload number / GPU-written mark and a 16 KB sample of its guest storage); at a verified HIT
  every member it would skip is probed again. On a differ: `[nr] shcache verify detail: cascade k seq s: members probed N, inputs changed since the
  build: vertex v index i texture t [ord o: vertex vb 0x...+n stride s ib 0x...+n tex n at 0x...]` (first 16), or `no member input changed: the
  difference is not in the members' bytes`. With the fix a changed member is never taken, so `changed` should read 0 everywhere.
- Boot line: `... cpuw=1 (... shc4: vertex / index / texel / image contents versioned (CPU-writable pools by the write-protect tracker), ...)`.

### 12.5 Expected effect (Mi 14 Ultra)
- **Skip rate standing on the beach** (where the verify was already equal): ~unchanged, **45-47 %** (388 of 828 now). Members lose only what moves:
  a CPU-writable range is no member for its first range (protect), volatile ranges and GPU-written images never. In the menu / walking past
  flags: the moving casters (a few draws, ~1000 bytes of cascade 2) become dynamic: -0.1 ... -1 % there.
- **Hash cost:** + ~2-3 journal queries, a memo lookup, `get_image` per texture and (CPU pools) one page walk per range: **1.05 -> ~1.2-1.4 us a
  draw** (+0.1 ... +0.3 ms translator a frame at ~830 hashed draws). The ABBA gain of tonight (+2.4 fps, submit -0.65 ms, GPU -0.4 ms) shrinks on the
  translator side by that; the GPU part stays. Expected `=1` vs `=0` standing: **+1.5 ... +2.4 fps** - so the ABBA has to be repeated with this APK.
- `=2` costs more than before in verify frames (probes hash ~10-20 MB of vertex / index / texture bytes per verified hit, every 30 frames per
  cascade): check mode only, never timed.

### 12.6 Tests / build
- `nr_shc_test` (clang 20): **12 698 checks PASS**. New: `test_journal` (60 000 random writes x 64 ranges against an exact byte model: a written
  range always gets a new version - missed 0, below 0; a sparse phone-like case: 99.8 % of untouched ranges keep their version; page / block /
  straddle / n = 0 edges); `test_protect_model` (the tracker's argument on a page model: one version = one content, 0 bad of every repeat);
  **`content rewrite` - the phone bug class**: 180 casters whose shapes live in memory known to the hash by address, 3 "cloth" casters rewritten
  every frame for 20 frames, one upload into a static caster, one texture re-upload: the address-only hash is wrong in **19 of 90 frames**
  (reproduced), the versioned hash **exact in all 90** with 84 hits, the rewrites counted as new by content (J 82, T 1), execution 2 without
  desync.
- `source_structure_test` block NR-SHCACHE extended (journal hook first in `snapshot_invalidate`, pool events, upload numbers, texver checks the
  image first, streams / index / textures versioned, execution 2 reuses the version, CPU pools by the tracker, outside-range atlas passes) PASS.
- NDK `-fsyntax-only` of `xbg_game.cpp`: 0 errors, the same 13 warnings as wt-nrall.
- Full host run (shc3's runner pointed at this worktree, libdm built): **87 PASS, 0 FAIL**; after the last APK build the libdm-based checks
  (`dos2_nr_check`, `nr_leancall_test`, `nr_nocall_oracle_obj`) + `nr_shc_test` + `source_structure_test` again: 5 PASS.
- APK: see 12.8.

### 12.7 Phone runs (Mi 14 Ultra; APK only; save backup + keyguard check first; ~60 s each from the level, **include the 3D main menu** - that is
where the standing run's differs were - then the Fort Joy beach; 2 min cooling after every 3 runs; look for `thermal_limit_refresh_rate`)
| # | env (dm/env, besides `DM_DBG=1 DM_DOS2_TOUCHUI=1 DM_DOS2_FPS=0`) | scene | decisive lines |
|---|---|---|---|
| 1 | `DM_NR_SHCACHE=2` | menu ~20 s, then standing 60 s | **no `【红】[nr] shcache`**; `verify equal E differ 0` with E > 0 in the menu windows too; `inputs`: `new by content` > 0 in the menu (the moving caster, by channel J or P), `verify probes ... changed vertex 0 index 0 texture 0`; `would skip` ~45-47 % standing; `hash` us a draw |
| 2 | `DM_NR_SHCACHE=2` | menu, then walking 60 s past the Fort Joy flags | `differ 0`; `new by content` per cascade; `draws without a version` small |
| 3 | `DM_NR_SHCACHE=1 DM_NR_SHCACHE_DIAG=0` vs `=0`, ABBA (4 runs, 2 min cooling before) | standing | fps / `submit_cpu_ms` / `gpu_ms` over the steady windows vs tonight's +2.4 fps; `hit H (ok ~H, recover ~0)`; `skipped` ~388 a frame |
Run 3 is needed: the hash got dearer. If run 1 shows a differ with `verify detail ... no member input changed`, the cause is outside the
members (send the line); if it shows `changed vertex / index / texture > 0` with a red line, that channel's version is incomplete (send the
`ord ... vb 0x...` part). Optional A/B of the tracker's cost: run 3 with `DM_NR_SHCACHE_CPUW=2` (not exact; only if `CPU pool` ranges dominate the
inputs line and the hash cost rose > 0.3 us).

### 12.8 APK
`tools/build_apk.ps1` (own `work/build-android` configured like wt-nrall's, drivers / libadrenotools copied from wt-nrall, Gradle 9.8.1 offline)
-> `work/DOS2-port-debug.apk` **56 076 477 B, SHA-256 `1858e49e05f0ed2a9a5204983b6f0907ecf332184b4b3611899b9b955c0b1c5f`**; `libdm.so` carries
`[nr] shcache inputs` and `nvn_dirty_version`. Not run on the phone.

### 12.x Телефон shc4 (18.11 22:50, Mi 14 Ultra, APK `cc478c3`, 50 с, без теплового капа)
| Прогон | env | FPS | GPU мед., мс | submit мед., мс | кэш |
|---|---|---|---|---|---|
| C_s5_shc2 | `=2`, меню → стоя | 77.8 | 12.18 | 12.20 | differ 0 везде; меню 78–83 %, стоя 46.5 %, хэш 1.4–1.5 мкс |
| C_m5_shc2 | `=2`, меню → ходьба | 73.7 | 13.01 | 12.63 | differ 0; ходьба 34–43 %, recover 2–18 / 10 с |
| C_ab5_base1 | — | 78.5 | 12.31 | 12.27 | |
| C_ab5_shc1a | `=1 DIAG=0` | 80.9 | 11.91 | 11.59 | skipped 46.9 %, хэш 1.53 мкс |
| C_ab5_shc1b | `=1 DIAG=0` | 80.8 | 11.94 | 11.66 | skipped 46.6 %, хэш 1.81 мкс |
| C_ab5_base2 | — | 78.9 | 12.24 | 12.20 | |

Итог: бит в бит на Adreno доказан (меню, стоя, ходьба), стоя **+2.15 fps** (GPU −0.35, submit −0.6 мс). Логи `src/work/logs/C_s5_* C_m5_* C_ab5_*`.
Открыто: умолчание (Adreno `=1`?), тайлеры (копия слоя на Mali / PowerVR — партия D в SIM).

Умолчание (хозяин 18.11): `=1` на Adreno, `0` на остальных (и с `DM_GFX_SIM_GPU=mali`) до прогона на тайлере (партия D); `DM_NR_SHCACHE=0` выключает.
