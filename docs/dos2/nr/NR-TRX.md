# NR-TRX: translator + NB-MT per-draw cost (night 18.11.2026, branch `dos2-nrtrx`)

Worktree `wt-nrtrx`, branch `dos2-nrtrx` from `dos2-nr-all` 12961a5. No phone (night rules: no adb): everything below is PC tests,
micro-benchmarks and the Turnip source at the exact driver build of play-1118 (`Mesa 27.0.0-devel (git-1c3a936490)` =
`<tools>/src/mesa-full`, `src/freedreno/vulkan/tu_cmd_buffer.cc`). Not one line ran on a device. Host / hle_dos2_nr* / nvn front end
untouched (the engine UBO allocator agent works there).

## 0. Summary

| Item | Switch (default) | Proof | PC ns | Expected, Mi 14 Fort Joy heavy windows |
|---|---|---|---|---|
| Sticky pages refined to written bytes (vertex / index views) | `DM_NR_TRX_STICKY` **0** (0 / 1 / 2 count) | `nr_trx_test` B: written-range set == byte bitmap on 58 920 queries, invariants, coarse cap | add 165 ns, query 41 ns | **unknown until =2 runs**: play-1118 has **1 392 sticky misses / frame -> 2.26 MB vertex copies / frame** (456 stream copies + front / map / vcache hits). If most are false sharing: translator **-0.5 ... -1.4 ms**, 2.3 MB/frame less copy traffic; if the bytes really are written by the replay: 0 |
| Zero-copy chunk lookup by sorted index | `DM_NR_TRX_ZC` **0** (0 / 1 / 2 check) | `nr_trx_test` A: index == scan on 4.5 M lookups / 3 000 tables (adjacent chunks, holes, slot reuse, overlaps -> scan) | 19-22 -> 12-15 ns a lookup | small: -15 ns a draw PC, **-0.03 ... -0.08 ms / frame** phone |
| Group refresh: recent ids before the hash (MRU 4) | - | `nr_trx_test` C (ids equal in all variants) | 79-129 -> **88-153** ns a draw (slower) | **rejected** |
| Group refresh: 4-lane hash | - | same | 79-129 -> 73-118 ns a draw (-7 ... -9 %) | rejected (6-11 ns a draw not worth a switch) |
| NB-MT packet encoding | - | `nr_trx_test` D (mock Vulkan) | ~5-10 ns a packet on the producer | not a lever (3.8 packets = 20-40 ns a draw of 1.85 us) |
| `VK_EXT_descriptor_buffer` / `VK_KHR_push_descriptor` (both on the device) | - | Turnip source §3 | - | **rejected** for Adreno: no Turnip work saved, moves it onto the translator, all pipelines re-made |
| Vertex fold on zero-copy Adreno costs Turnip a VS-params emit per draw | existing `DM_NR_DRAW_FOLD` (3) | Turnip source §3.3 | - | phone A/B `DM_NR_DRAW_FOLD=1` vs `3`: worker -0.07 ... -0.15 ms / submission [m] |

[m] = model / source reading, not a measurement.

## 1. Where the per-draw time is (play-1118, Fort Joy, 1 630-2 100 draws)

From the 10 s lines of the heavy window (boot.log 86000-88500): served draw = `nrdesc::serve` **1 913 ns** + `nrdraw::lane` **1 854 ns**
+ `nbdraw::apply` binds 295 ns + NB step / group refresh (`[nb] pipe`: 3.71 groups re-read a draw, 0.95 unchanged, 0 new ids, NB step
635 ns on the old path). `vkCmd* a served draw 3.79` (pipeline 0.56, set 0.79, vertex 0.91, index 0.53, viewport / scissor 0.001).
NB-MT: 9 037 packets / submission, worker replay 5.97 ms (heavier windows 13.3 ms) = 0.66-1.4 us a packet.
`NVN PERF zerocopy hits=11 340 sticky_miss=1 392 / frame`, `snap_mb_per_frame vertex=2.26`, `snapfront front_hits=935`, `vcache hits=128
volatile=7126`. NR-CPU's `nrdesc` / `nrlane` / `nbstate` / `nbdraw` / `mtend` split and `[nr] cpu mt` have not run on the phone yet.

What the PC benchmarks say about the hot paths themselves (hot caches, x86): the pieces that look like data-layout problems are cheap on
PC - the 34-slot chunk scan is 19-22 ns, the group refresh 80-130 ns a draw, the packet layer 5-10 ns a packet, a written-range query 41 ns.
The phone's 1.9 + 1.85 us per served draw is therefore not one slow loop: it is many small reads of large engine structures (State,
Image, Ent vectors, counters in separate cache lines) on a core HyperOS keeps moving to the A520s (NR-BN 2.4 / NR-CPU 1). Micro-rewrites
of single loops give 10-40 ns each; the levers with real size are the ones that remove work: the sticky-page copies (§2.2), the NB-MT
worker's core and Turnip-side extra emits (§3), and the per-draw state work NR-09S3 already moves (DM_NR_STATE=4).

## 2. What was done

### 2.1 `DM_NR_TRX_ZC` - chunk lookup without the table scan (`xbg_game.cpp` namespace zc, `dm/gfx/nr/nr_trx.h`)
`zc::holder()` scanned every slot of `zc::table` (live and retired, 88-byte entries; Fort Joy: 22 live of ~34 slots) on every
`zc::view` / `find` / `tail` (vertex, index, uniform views; NR-06 ring re-find). `=1`: a sorted index of the live chunks (`nrt_ix`,
rebuilt when the table changes - `map_pool` installs / `retire` bump `trx::ver`) + the chunk found last; live chunks that overlap ->
`ok = 0` -> the scan (counted). Exact: zc keeps at most one live chunk per address, so with no overlap the one holder IS the scan's
answer (the newest). `=2`: the scan's answer is used, the index's compared (`【红】[nr] trx zc: the chunk index found ...`, count).
Test A: 4.5 M lookups on 3 000 random tables, every boundary (lo, hi-1, hi, lo-1, outside), 48 tables with overlaps -> 0 differences.
Bench A (22 live / 34 slots, two lookups a draw, ~6 KB of other data touched between draws): random mesh order 44.3 -> 30.1 ns a draw,
material runs 38.6 -> 23.3 ns. Phone x2-3: -30 ... -50 ns a draw = **-0.03 ... -0.08 ms / frame** - real but small.

### 2.2 `DM_NR_TRX_STICKY` - sticky pages refined to the bytes the replay wrote
`zc::mark()` (replay-time CPU writes: `OP_UPDATE_UB`, CPU buffer copies, fills, the 16-byte zeroing) marks whole 4 KB pages sticky
forever; every later view touching such a page takes the snapshot path. With `DM_NR_TRX_STICKY` > 0, `mark()` also records the written
byte range in the chunk's `nrt_wrset` (sorted, merged, last-write submission serial kept; > 16 384 ranges -> that chunk goes coarse =
pages only, counted). In `zc::view()`, a **vertex / index** view (`snap_site` 1 / 2: the old tail's and `nrdraw::lane`'s stream and index
snapshots) that hits a sticky page:
- `=1`: is served in place when none of its bytes was ever written by the replay (`nrt_wr_meets` = 0);
- `=2`: counted only (would serve; and how many more if written ranges older than 8 submissions were let go - NOT done, a later write
  would reach draws recorded before it), the snapshot path runs as before.
Uniform / storage / texel / other views stay page-sticky (a uniform block the game updates in parts keeps its whole page: its other
fields may be the next update's target). Why exact: the page rule protects the bytes a replay write changes; bytes of a sticky page that
no replay write ever touched are read in place exactly like the bytes of a page never written - the same model the zero-copy path uses for
every other page (game CPU writes follow NVN ordering, as on the Switch). The only changed decision is "copy" -> "in place" for views
that meet no written byte.
Line every 10 s: `[nr] trx zc (DM_NR_TRX_ZC=.. DM_NR_TRX_STICKY=.., 10 s): ... sticky-page vertex / index views V: bytes never written by
the replay W (served / would serve), + written ranges older than 8 submissions D; CPU writes marked M, most ranges in a chunk R, chunks
gone coarse C`, then `[nr] trx zc: replay CPU writes by op (10 s): op N count (KB) ...` (which op makes the pages sticky).
Test B: 16 random write histories (small UB-like writes and 70 KB copies, page-aligned and not), 58 920 view queries against a byte bitmap:
"meets" equal on every query, newest serial >= the bytes' own, ranges sorted / disjoint / not touching after every 97th add, coarse past
16 384 ranges. Cost: add 165 ns (2 000 ranges; ~76 writes / frame in play-1118 = 0.01 ms), query 41 ns (only on a sticky-page view).
Gain: decided by one `=2` run. play-1118: 1 392 sticky misses / frame, `vcache volatile=7126` (the game itself rewrites those vertex
ranges often = dynamic vertex data), i.e. the copied data are largely CPU-written vertices that share pages with replay writes. If W / V
is high: -456 stream copies (2.26 MB) + -935 front hits + map / vcache work a frame = **-0.5 ... -1.4 ms translator** [m] (Mi 14), x2-3
on weak cores; GPU unchanged (it reads the same bytes from the guest memory instead of the stream copy). If W ~ 0: no gain, and the op
histogram says which write to move to the GPU (`gpu_buffer_copy` exists for copies).

### 2.3 Tried and rejected (numbers in `nr_trx_test` C / D)
- **Recent-id compare before the group hash** (MRU of 4 ids, exact): 4.42 / 2.47 changed groups a draw, 74-80 % hits, but refresh
  129.4 -> 153.4 ns (random order) / 78.8 -> 87.7 ns (material runs): slower - the memcmp of up to 4 candidates costs more than the hash.
- **4-lane hash** in the group dictionary (exact: ids are first-seen order, the hash only picks probe slots): 129.4 -> 118.2 / 78.8 ->
  73.0 ns a draw (-6 ... -11 ns). Not worth a switch. The group refresh itself is 80-130 ns a draw on PC (~0.2-0.4 us on the phone):
  real, but it is the price of exact pipeline ids; S3 (`DM_NR_STATE=4`) lowers how often groups are dirty.
- **NB-MT packet layer**: served-draw mix (3.8 packets a draw) against the mock: direct 40.3 ns a call, packets replayed inline 47.5,
  worker (producer side) 37.2 (the mock's own logging, ~30 ns, is in all three) -> the packet encode is ~5-10 ns a packet, 20-40 ns a draw.
  Packet-level dedup would remove nothing (NR-CPU: lane already drops unchanged binds).
- **Fewer descriptor-set binds**: 0.79 a draw are offset changes of the per-draw ring blocks (3.9 a draw); a second physical set
  would add calls (NR-08 §2.1). Content-dedup of a moved block (same bytes at a new ring address) would need a memcmp per block and only
  helps draws whose every changed block is byte-equal - per-object blocks are not; not done.

## 3. Turnip (driver of play-1118, source at the same commit)

### 3.1 What each per-draw call costs Turnip (CPU, from `tu_cmd_buffer.cc`)
| Call (a served draw) | Turnip work |
|---|---|
| `vkCmdBindDescriptorSets` (0.79) | per dynamic UBO: memcpy of its descriptor + address patch; then `tu_cs_alloc` (sub_cs) of the whole dynamic block and a memcpy of all dynamic descriptors; dirty DESC_SETS -> at the draw `tu6_emit_descriptor_sets` (draw-state alloc, `SP_BINDLESS_BASE` of every bound set, `SP_UPDATE_CNTL` = bindless cache invalidate on the GPU) + the pipeline's DESC_SETS_LOAD state again (the CP re-loads UBO-promoted constants) |
| `vkCmdBindPipeline` (0.56) | `vk_cmd_set_dynamic_graphics_state` (field-by-field compare of the pipeline's state), CP_SET_DRAW_STATE of ~10 + N groups, and it dirties **DESC_SETS, SHADER_CONSTS, VS_PARAMS, PROGRAM**: every pipeline change re-emits the descriptor sets and the shader constants (`tu_emit_consts`) at the next draw even when the set did not change |
| `vkCmdBindVertexBuffers` (0.91) | `tu_cs_draw_state` of 4 x `max_vbs_bound` dwords and ALL vertex buffer registers up to the highest slot bound so far in the command buffer (not only the changed ones) |
| `vkCmdBindIndexBuffer` (0.53) | a few stores, no emit: cheap |
| `vkCmdDrawIndexed` (1) | `tu6_emit_vs_params` (§3.3), `tu6_draw_common`: `tu_emit_draw_state` + CP_SET_DRAW_STATE of every dirty group; early exit only when nothing is dirty |

### 3.2 Descriptor buffers / push descriptors: rejected for Adreno
Both are in the device list (`VK_EXT_descriptor_buffer`, `VK_KHR_push_descriptor`, play-1118 boot.log line 48-51).
- **Push descriptors** (`tu_push_descriptor_set`): a fresh `tu_cs_alloc` of the whole set, `tu_update_descriptor_sets` of every write
  (image descriptors too), a memcpy, then the ordinary `CmdBindDescriptorSets` path. Strictly more work than today's dynamic offsets.
- **Descriptor buffers** (`tu_set_descriptor_buffer_offsets`): the bind itself is only `set_iova` - but descriptor-buffer layouts have
  no dynamic UBOs, so the 3.9 per-draw ring blocks a draw would have to be written into descriptor memory by the app (`vkGetDescriptorEXT`
  + copy): the same bytes Turnip copies now in `tu_bind_descriptor_sets`, moved from the NB-MT worker to the translator (the busier
  thread). The GPU side is identical (`SP_BINDLESS_BASE` + `SP_UPDATE_CNTL`). It also needs every pipeline and layout re-made with
  `VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT` (the warm-up / pipeline cache). On Mali (PanVK) / PowerVR the picture differs (descriptor
  updates are the slow part there - NR-08 class "no dynamic UBO"), but those drivers are not in the device lists we have. No gain on Turnip.

### 3.3 Avoidable Turnip work found: the vertex fold re-emits VS params on almost every draw (Adreno, zero-copy)
`tu_CmdDrawIndexed` -> `tu6_emit_vs_params(cmd, NULL, 0, vertexOffset, firstInstance)` returns early only when `vertexOffset` and
`firstInstance` equal the last draw's; otherwise it opens a sub-stream, writes `VFD_INDEX_OFFSET` / `VFD_INSTANCE_START_OFFSET` (plus driver
params for shaders that read them), and dirties VS_PARAMS (one more draw-state group at the draw, and the draw can never take the
"nothing to emit" exit). NR-09's vertex fold (`DM_NR_DRAW_FOLD` bit 1) moves each stream back by k vertices and draws with
`vertexOffset + k`: every mesh gets its own vertexOffset (play-1118: `folds: vertex 685669` of 685669 served). On zero-copy Adreno the
fold saves almost no binds (NR-09 model: VB binds 0.85 -> 0.84 a draw; the chunks are 64 MB and offsets are arbitrary), so it is pure cost
there: ~40-80 ns of worker time a draw [m] = 0.07-0.15 ms a submission. The index fold (bit 0) only changes `firstIndex` (a packet field,
free). Without zero-copy (Mali / PowerVR / Xclipse: NR-06 copies placed on the stride) the vertex fold removes 0.3-0.85 binds a draw and
stays. Proposal for the policy after the A/B: `xbg_nr_draw_fold_default` = 1 on zero-copy devices, 3 otherwise (not changed tonight).

### 3.4 The worker's core and the end-of-submission wait
The NB-MT worker (`dm-nbmt`) has no pin (`DM_NB_MT_CPUS=0` = every CPU) and is not in `DM_CPU_SELF`; HyperOS parks / moves threads onto
the A520s (NR-BN 2.4); where the worker ran in play-1118 is not logged (`[cpu] where` lists translate / main only). Turnip's per-call
code (§3.1) is a few hundred instructions, so 0.66-1.4 us a packet suggests a slow or busy core [m]. Existing switches for one A/B, no code: `DM_NB_MT_CPUS=7c`
(the A720 cluster, the translator stays on cpu7) and `DM_NB_MT_FLUSH=64` (smaller hand-over chunks shorten the tail the translator waits
for in `nbmt::end`: today up to 256 packets).

## 4. Files
- new: `dm/gfx/nr/nr_trx.h` (pure C: `nrt_ix`, `nrt_wrset`), `dm/gfx/vulkan/engine/tests/nr_trx_test.cpp`, this document;
- changed: `dm/gfx/vulkan/engine/xbg_game.cpp` (namespace zc only: `#include`, `Chunk::wr`, `trx` switches / line, `holder_scan` /
  `holder_ix` / `holder`, `++trx::ver` in `map_pool` / `retire`, `mark()` ranges, `trx_sticky_ok` in `view()`).

## 5. Checks (PC)
- `nr_trx_test` (clang++ 21 -O2, mock Vulkan): **PASS, 4 785 189 checks** (A 4.5 M index == scan; B 58 920 queries; C ids equal in
  every variant on 2 x 600 000 draws; D the three NB-MT modes).
- suite (`run_tests.sh` of the shared doc pointed at this worktree + `nr_trx_test`): see §7.
- NDK 29: `cmake --build work/build-android --target dm`: `libdm.so` linked; `xbg_game.cpp -fsyntax-only` with the build's command: 0
  errors (warnings are the base's: unsequenced / unused).

## 6. Phone runs that confirm it (main agent, with the owner's permission; back up `files/save`, keyguard off, Fort Joy at the gate,
>= 60 s from the load). Files for the phone: **only the APK** (no packs).
1. **R-TRX1, sticky census:** `DM_NR_TRX_STICKY=2 DM_NR_TRX_ZC=2 DM_NR_CPU=1`. Expect `[nr] DM_NR_TRX_ZC=2 DM_NR_TRX_STICKY=2 (...)`; every
   10 s `[nr] trx zc (...)`: **index vs scan differ 0**, `sticky-page vertex / index views V: bytes never written by the replay W (would
   serve)` - **W / V is the answer** (and `+ written ranges older than 8 submissions D`); `replay CPU writes by op` (which op: 10 =
   buffer copy?, update UB, fill). Compare V with `NVN PERF zerocopy sticky_miss` x 10 s x fps.
2. **R-TRX2, sticky on (only if W / V >= 0.3):** ABBA 60 s `DM_NR_TRX_STICKY=0` vs `=1` (+ `DM_NR_TRX_ZC=1` in both): `NVN PERF zerocopy
   sticky_miss` drops by ~W / 10 s, `snap_mb_per_frame vertex` drops, `snapfront front_hits` drops, `submit_cpu_ms` / `[cpuprof] threads`
   translate; pictures equal (water, grass, characters, fire / particles, UI), no flicker over 60 s of camera movement.
3. **R-TRX3, Turnip VS params:** ABBA 60 s `DM_NR_DRAW_FOLD=3` vs `=1`, `DM_NR_CPU=1`: `[nr] cpu mt ... DrawIndexed x ns` and `replay X
   ms` lower with `=1`; `[nr] draw: vkCmd* a served draw ... vertex` about the same (0.91); pictures equal.
4. **R-TRX4, worker core / tail:** `DM_NB_MT_CPUS=7c` vs default, then `DM_NB_MT_FLUSH=64` vs 256: `[nb] mt ... worker replay X ms`,
   `waited W ms` (the `mtend` part of `parts_ms_per_frame` with `DM_NR_CPU=1`), `submit_cpu_ms`.
Defaults stay 0 until (1) shows differ 0 and (2) a gain.

## 7. Suite, build, APK
- Suite: the shared doc's `run_tests.sh` pointed at this worktree + `nr_trx_test` (scratchpad `nrtrx/run_tests.sh`): **85 of 85 PASS**
  (the first pass caught a wrong check in the new `nrtrx_check` block; after the fix, and after the rename to `trx::zcm` / `trx::stm`,
  `source_structure_test`, `nr_trx_test`, `dos2_nr_check --so`, `nr_leancall_test`, `nb_mt_test` and `nb_pipe_test` were re-run on the
  final libdm.so: PASS).
- NDK 29: `cmake --build work/build-android --target dm`: linked.
- APK: `tools/build_apk.ps1` (offline Gradle, driver zips copied from `wt-nrall/work/drivers`, nothing downloaded): rc 0,
  **`wt-nrtrx/work/DOS2-port-debug.apk`** 55 875 773 B, SHA-256 `147f2aaadb7e6513d71eef8ad8826058b719b81b94f3d7702c8787abf8df9dd5`
  (version unchanged). Not installed anywhere.

## 8. Step 2 (night 18.11, branch `dos2-nrtrx2` from `dos2-nr-all` 11e440d): stable written ranges served in place, exact

### 8.1 What the R-TRX1 run said (Mi 14, Fort Joy walking, `DM_NR_TRX_STICKY=2 DM_NR_TRX_ZC=2`, `work/logs/nrB_tex.dm.log`)
- `index vs scan differ 0` in every window (2.5-3.9 M lookups / 10 s): `DM_NR_TRX_ZC` is exact on the device.
- Sticky-page vertex / index views, 10 walking windows of 215-280 k / 10 s (~300 a frame at 72-80 fps; `NVN PERF zerocopy sticky_miss`
  ~316 a frame agrees): bytes never written **20-27 %**, written ranges older than 8 submissions **66-74 %**, recent **4-9 %**.
  Step 1 alone (never written) stays under the 30 % threshold; step 1 + the old ranges = **91-95 %** of the sticky misses.
- **op 6 = `OP_COPY_B2B`** = `nvnCommandBufferCopyBufferToBuffer` (hle_nvn.c), replayed as a CPU `memmove` because its source is not a
  pending GPU result (`gpu_buffer_copy` only takes those). It is the only op that marks pages in DOS2 (no `OP_UPDATE_UB`, fill or counter
  zeroing in any window): 6-20 k copies / 10 s, 37-194 MB, ~5-10 KB each, all coalescing into at most 70 ranges a chunk. These are the
  game's vertex / index uploads (staging -> the GPU pool): **the copies write the VB / IB bytes themselves** - 73-80 % of the sticky-page
  views overlap copied bytes; only the never-written 20-27 % are neighbours on the same pages. Most uploads happen once (the 66-74 % "old"
  views), a small set is rewritten often (the 4-9 % recent = dynamic vertex data).

### 8.2 What was done (`DM_NR_TRX_STICKY`, `dm/gfx/nr/nr_trx.h` (3), `xbg_game.cpp` namespace zc only)
- **Class of a sticky-page vertex / index view** (`nrt_sticky_class`): NEVER (no written byte, step 1), **STABLE** (every written range
  it meets was last written >= `DM_NR_TRX_STICKY_AGE` (default 8) submissions ago and none of it is hot), RECENT, HOT, COARSE (chunk past
  16 384 ranges: pages only). NEVER and STABLE are served in place; the others copy as before. Merged ranges keep the newest serial of what
  they merged, so a view can only look younger than its bytes (copy = safe side).
- **Every in-place serve is noted** (`nrt_svring`): per submission (ring of 4, keyed by `slot_serial`), a list of (chunk slot, chunk gen,
  [lo, hi)) and per chunk slot a bitmap of 64 KB blocks. Noted in `trx_sticky_ok` before `view()` returns, i.e. before the draw records.
- **The guard** (`trx_guard`, first thing in `zc::mark`, i.e. before every replay CPU write lands): does the write meet a view served for
  a submission >= `trx_floor()` (the oldest submission not retired: `slot_serial - 2` if `slots[slot_index]` is pending, else
  `slot_serial - 1` if the other slot is, else `slot_serial`)? Bitmap test, then the exact list scan (same slot + gen, overlap). If yes:
  - the view's submission is an earlier one -> `retire_slot(slots[slot_index])` (it holds `slot_serial - 2`) and, if needed,
    `retire_slot(slots[slot_index ^ 1])` (`slot_serial - 1`), in order: a fence wait only while that fence is still busy;
  - it is the submission being recorded (a draw earlier in this replay reads the bytes) -> `gpu_sync` (submit + wait for both slots: the
    same thing `cpu_access` does for a pending GPU result at the same call sites);
  - the written bytes go **hot**: never STABLE again in this chunk (bounds the waits to about one per rewritten range).
  Then the write lands, marks the pages and the range (serial = now, so the next views of those bytes are RECENT = copy at once).
- Values: **0** (default) the old code, byte-for-byte (the new code sits behind the same `trx::stm` branches; `trx_init` reads the env once);
  **1** on (NEVER + STABLE served, guard acts); **2** count: classes, notes and the guard run, nothing is served and nothing waits
  (what 1 would do: "would serve", "would submit + wait", "would wait", hot ranges as 1 would make them); also notes the base path's
  in-place vertex / index views (pages never written) and counts writes onto them in flight (see 8.4); **3** = 1 + the base path's
  in-place vertex / index views noted and guarded too.
- Lines every 10 s:
  `[nr] trx zc (DM_NR_TRX_ZC=.. DM_NR_TRX_STICKY=.., 10 s): ... sticky-page vertex / index views V: bytes never written by the replay W
  (served | would serve), stable written ranges (last write >= 8 submissions ago, none hot) S (served | would serve), recent writes R,
  hot H, chunk coarse C; ...`;
  `[nr] trx sticky guard (DM_NR_TRX_STICKY=.., 10 s): replay CPU writes onto views served in place for an unfinished submission G: of the
  recording one Gs (submitted + waited | would ...), of an earlier one Gw (waited | would wait; its fence still busy Gb); hot ranges added;
  serves noted (repeats), most in a submission, lists full; base path (pages never written): writes onto its in-place vertex / index
  views in flight B (guarded | counted, not guarded | not noted), views noted`;
  `[nr] trx zc: replay CPU writes by op (10 s): op 6 N (KB; onto bytes written before X, onto stable ranges Y, onto views in flight Z)`.
- `DM_NR_TRX_STICKY_AGE` (1 ... 2^20, default 8): a tuning knob for the phone runs only (players get no flags).

### 8.3 Why it is exact (the proof; `nr_trx_test` E checks it)
Model (the same one the zero-copy path uses for never-written pages):
- M1. A replay CPU write (the four `zc::mark` sites: `OP_COPY_B2B` CPU path, `OP_UPDATE_UB`, `OP_X_CLEAR_BUF`, the 16-byte counter
  zeroing; the structure test pins them and that `mark` precedes each `memmove` / `memcpy` / fill) stands for a write on the Switch GPU
  timeline: draws recorded before it read the old bytes, draws after it the new ones. It is executed synchronously at replay time.
- M2. An in-place view makes the GPU read guest memory while it executes the submission the draw was recorded in, i.e. between
  `vkQueueSubmit` and that slot's fence. At serve time guest memory holds exactly what the Switch GPU would read (every earlier replay write
  has landed; game CPU writes follow NVN fences - the guest fence completes only after the Vulkan fence, unchanged base model). GPU results
  written back by the translator (`resolve_pending`, `resolve_slot_writebacks`, `desc_abandon`) land only after the submission that made
  them retired, and a range with a pending writeback is never viewed in place (`overlaps_pending`), so they cannot hit an in-flight view.
- M3. Two frame slots; a slot is retired (fence waited) before it records again, fences signal in submission order (a fence signal covers
  every earlier batch on the queue), so every submission below `trx_floor()` has finished.
- M4. A draw's views and its commands are in the same submission: `preflight_cpu_reads()` at the draw start resolves pending results, so
  `snapshot()` cannot sync between a draw's views and its recording; a sync inside `descriptors()` abandons the draw (`desc_abandon`), so its
  views are never read. The per-slot stream-arena copies of the snapshot path rely on the same invariant.

Claim: for a view V of chunk (slot, gen) served in place on a sticky page for submission s, and a replay CPU write W onto bytes B with
B ∩ V ≠ ∅ that runs after the serve, W's bytes land only after the GPU finished s.
1. The serve is in `g[s % 4]` before `view()` returns (before the draw is recorded, M4).
2. W calls `mark` -> `trx_guard` before its bytes land (M1). Let now = `slot_serial` at W (>= s).
3. If s < floor: s is finished (M3). Otherwise s ∈ [floor, now], now - s <= 2 < 4: `g[s % 4]` still holds s (it is cleared only when a
   serial ≡ s mod 4 is noted, i.e. >= s + 4 > now); V's 64 KB blocks are set in its bitmap and the exact scan finds V (same slot + gen,
   overlap) -> a conflict with v >= s. A list that could not grow (`over`) makes every write onto a touched block a conflict.
4. v = now -> `gpu_sync`: the recording slot is submitted and both fences are waited -> everything <= now is finished. v < now -> v is now - 1
   or now - 2: `slots[slot_index]` (now - 2, if pending) is retired, then `slots[slot_index ^ 1]` (now - 1) when v >= now - 1 -> s <= v is
   finished (M3).
5. Only then does W land. So no GPU read of V for s sees W, and V's bytes at serve time were the right ones (M2). ∎
A retired chunk's views keep reading its old AHB, which no longer backs the guest range (writes go to the new anonymous pages); a chunk
mapped later into the same table slot has a new gen and never matches the old views. The proof needs neither the age nor hot: age = 1 and
no hot are still exact (E3), they only lower how often the guard has to wait. NEVER-class views (step 1) are now guarded the same way
(step 1 alone had the first-write case of 8.4).

### 8.4 Found on the way: the base zero-copy path has the same first-write case (not changed at 0 / 1)
A vertex / index view on a page that was never written by the replay is served in place without any record (the base model). If a
replay write then lands on its bytes while that submission is in flight, the GPU may read the new bytes (on the Switch the copy is
ordered after the draw). It can happen once per page (afterwards the page is sticky); in DOS2 that is an upload `OP_COPY_B2B` into a
pool range a draw in flight still reads (a recycled mesh slot). The E3 model hits it ~220 times in 250 000 random steps (it writes
anywhere); on the phone it is unknown. `=2` counts it (`base path ...: writes onto its in-place vertex / index views in flight B`), `=3`
closes it (all in-place vertex / index views noted: ~12-16 ns PC a view, ~11 k zero-copy hits a frame of which the vertex / index part is
noted -> est. 0.1-0.3 ms a frame on the Mi 14 [m], so it is not part of 1). If B > 0 on the phone, the picture glitch it would cause is a
one-frame wrong vertex / index read after a mesh upload.

### 8.5 Tests (PC) and build
- `nr_trx_test` (clang++ 21 -O2): **PASS, 4 860 091 checks** (A-D unchanged, + E):
  - E1 `nrt_sv_conflict` == the list of every serve: 18 008 write queries (1 779 meet a served view) with ring wrap, slot reuse, offsets
    past 64 MB, `over`: flag and latest submission equal.
  - E2 `nrt_sticky_class` == byte bitmaps: 48 000 views (never 3 317 / stable 7 097 / recent 32 155 / hot 5 431): STABLE only when every
    met written byte is >= age old and not hot; never-written views always NEVER.
  - E3 replay model with the engine's two slots (serves before / after `recording_slot`, replay writes onto meshes / neighbours / anywhere,
    submits, in-order GPU ends, `wait_fence`, `gpu_sync`, chunk retire + slot reuse), 250 000 steps a run, every in-place read checked by
    byte versions at each write and at the GPU end: age 8 / 2 / 1: **0 stale reads** (54-67 k STABLE serves, 840-980 guard actions, of
    which ~40 % on a busy fence); every write demotes its bytes at once (0 servable after a write); `=3` model: 0 stale reads including the
    base path. Mutations, each caught: no guard (717 stale), floor one too high (300), the recording submission only waited (499), serves
    not noted (720). No hot + age 1: 0 stale (hot is a cost limit).
  - E4 bench (1 300 serves + 76 writes a frame, 22 chunks, 3 submissions in flight): note **12-16 ns** a serve, conflict check **0.21-0.29 us**
    a write (most writes meet a block that has serves in this bench; a filtered miss is a few ns).
- `source_structure_test.py` (nrtrx_check extended: classes served, note before serve, 2 never serves, guard before the marking loop, guard
  acts at 1 / 3 only, the floor, the four write sites marked, base notes only at 2 / 3, env read in `trx_init` only): PASS.
- Suite, NDK, APK: section 8.7.

### 8.6 Expected gain
Translator thread only (the GPU reads the same bytes from the guest pool instead of the stream copy; zero-copy is Adreno-only, so Mali /
PowerVR / Xclipse: no change).
- Walking Fort Joy (nrB_tex, 72-80 fps): ~300 sticky misses a frame, 91-95 % servable -> ~275-285 snapshot-path copies a frame avoided.
  The snapshot part there is 0.6-1.0 ms a frame over 3 000-4 600 calls (`parts_ms_per_frame snapshot=`); a sticky miss takes the copy path
  (map, vcache, stream alloc + ~1.2 KB copy): est. **0.5-1.3 us** each on the phone [m] -> **-0.14 ... -0.36 ms a frame** (submit_cpu 11-12
  ms: 1.5-3 %), 0.2 MB a frame less stream traffic.
- play-1118 heavy windows (1 392 sticky misses, 2.26 MB vertex copies a frame): if the split is the same, ~1 280 avoided ->
  **-0.6 ... -1.7 ms** a frame [m] and ~2 MB a frame less copying. Weak Adreno (Snapdragon 7 class): x2-3 the time.
- Cost: notes 300-1 300 a frame x ~30-50 ns phone = 0.01-0.06 ms; guard check ~25-76 replay writes a frame x < 1 us = < 0.05 ms; plus the
  waits themselves - **unknown until the `=2` run** (`trx sticky guard`: "would wait ... its fence still busy" is the number that costs:
  each busy wait blocks the translator until the GPU finishes the previous frame). Rule for the A/B: if Gw busy + Gs is under ~1 a second,
  turn 1 on; if not, raise `DM_NR_TRX_STICKY_AGE` (16 / 32) in a second `=2` run and compare.

### 8.7 Suite, build, APK
- Suite: the shared `run_tests.sh` pointed at this worktree (scratchpad `nrtrx2/suite/run_tests.sh`): **87 of 87 PASS** (incl.
  `nr_trx_test`, `source_structure_test`, `dos2_nr_check --so` 0 problems and `nr_leancall_test` on the final `libdm.so`).
- NDK 29: `cmake --build work/build-android --target dm` (configured like `wt-nrall`; `third_party/libadrenotools` copied from it):
  linked, 0 errors, no warnings in the changed lines.
- APK: `tools/build_apk.ps1` (offline Gradle, driver zips copied from `wt-nrall/work/drivers`, nothing downloaded): rc 0,
  **`wt-nrtrx2/work/DOS2-port-debug.apk`** 55 924 925 B, SHA-256 `0a137b9253aa96c4c2e95c50439e2be5b2aa3b0a5513565f90b2b45b94ac75fb`
  (version unchanged, defaults unchanged: every `DM_NR_TRX_*` is 0 unless set in `dm/env`). Not installed anywhere.

### 8.8 Phone runs (main agent, with the owner's permission; back up `files/save`, keyguard off, Fort Joy, 60 s from the load). Only the APK.
1. **R-TRX5, census of the new class + guard:** `DM_NR_TRX_STICKY=2 DM_NR_TRX_ZC=1`. Expect `[nr] DM_NR_TRX_ZC=1 DM_NR_TRX_STICKY=2
   DM_NR_TRX_STICKY_AGE=8 (...)`; every 10 s `stable written ranges ... S (would serve)` ~ 66-74 % of V and `bytes never written W` ~ 20-27 %;
   `[nr] trx sticky guard`: G / Gs / Gw / Gb (the waits 1 would do) and `base path ... B`; op line `op 6 ... onto stable ranges Y, onto views
   in flight Z`.
2. **R-TRX6, on (only if Gs + Gb are small, see 8.6):** ABBA 60 s `DM_NR_TRX_STICKY=0` vs `=1` (`DM_NR_TRX_ZC=1` in both):
   `NVN PERF zerocopy sticky_miss` drops ~90 %, `snap_mb_per_frame vertex` / `snapfront front_hits` drop, `parts_ms_per_frame snapshot=` and
   `submit_cpu_ms` lower; guard line `submitted + waited` / `waited` as predicted by R-TRX5; pictures equal (characters, water, grass, fire,
   UI, camera turns 60 s: no flicker after uploads).
3. **R-TRX7 (only if B > 0 in R-TRX5):** `=3` vs `=1`: `base path ... guarded` > 0, same pictures, the extra cost in `snapshot=`.
