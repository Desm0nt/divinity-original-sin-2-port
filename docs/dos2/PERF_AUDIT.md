# DOS2: аудит перф-переключателей по измеренным данным (14.11.2026)

Дерево `wt-perf` (ветка `dos2-perf`) = клон TL2 = линия D3 (`xbg_game.cpp` 7.3 тыс. строк, не 12.7 тыс. ETR). Сборка `dos2` определяет
`DM_GAME_DOOM + DM_GAME_D3 + DM_GAME_TL2 + DM_GAME_DOS2`, поэтому умолчания в `jni_android.c` — умолчания D3. Список ниже проверен grep'ом
строковых литералов `"DM_*"` по `dm/` этого дерева (139 переключателей) против ETR `src-cmp16` и D16 `doom-port/src`.

## Что показывают логи (Mi 14 Ultra, Adreno 750, Turnip 27; `src/work/logs/o2/boot.log`, `r5.log`, `r2.log`)
| Окно | fps | draws | passes | submit_cpu | gpu | что видно |
|---|---|---|---|---|---|---|
| заставка / загрузка | 60 | 10–21 | 4–13 | 0.3–0.7 мс | 0.8–2 мс | `present_cpu` 1–16 мс = ожидание vsync |
| главное меню | 58–60 | 2100–2260 | 35 | 10.8–11.7 мс (≈ 5 мкс/draw) | 11.4–12.2 мс | 1 сабмит/кадр; **~1028 счётчиков (samples) внутри прохода на кадр** (`inpass_reports=1028`, `reports=2058` в одной эпохе), `cleanup queries` 0.05 мс; `dispatches` 0 |
| вход в меню, первая сборка | 17.7 | 1778 | 31 | 49 мс | 11.1 | `pipelines_new=100 created=99 create_ms=1891 max=102` |
| загрузки | 5.7–8.5 | — | — | — | — | 15 отчётов stall watch 2–6.6 с; потоки игры в `getdents64` / `openat` → `fuse_simple_request`, процесс иначе спит (CPU 0.00 с) |

Аудио: audren `effects 0`, catch-up = 251 только на старте и дальше не растёт (38 строк подряд) → «20-fps lock» D16 (PERF-D06) здесь не
проявляется. Счётчиков много (~1028 на кадр, код 61 `ExecuteRenderCommand`, NATIVE_PLAN §1.4), но в **одной** эпохе (`epochs=1`, нет ResetCounter
перед каждым запросом, как у ETR): CPU-учёт дешёвый (0.05 мс) → `REPORT_FAST`/XLAT не нужны; цена на GPU (a7xx: ZPASS_DONE + WFI на query,
ETR GPUCOST ≈ 1.4 мкс/draw) — кандидат PERF-E03 (только опциональный Turnip occ3) или нативная замена, мерить `DM_GFX_PASS_PROF`. Compute нет.

## Переключатели: есть / умолчание для DM_GAME_DOS2 / нет
**Есть (наследие D3/TL2), умолчание DOS2:** `DM_NVN_SYNC=0` + `DM_HANDOFF_PINNED` (PERF-D04), `DM_GFX_DEFER_EVENTS=2` + `IDLE_RETIRE=2` +
`SUBMIT_CHUNK_US=0` (PERF-X01, 1 сабмит/кадр, 2 слота), `D3PERF=1` → `REPORT_INPASS LAZY_CLEAR NAN_SAT LDC_CANON F2I_NOTRUNC` (PERF-D10,
5 из 8; самотест не на Adreno, MALI-06), `EARLY_Z`, `GPU_INDIRECT`, `STORAGE_CHAIN`, `GUEST_BARRIER=0`, `LAZY_STORAGE`, `PARTIAL_TEX`,
`PRO_COPY` (PERF-D01), `MEMO_SIMG`, `GS_PASSTHRU` (PERF-D09), `ZEROCOPY` (Adreno), `ZC_UBO_MIN`, `DIRTY_BLOCKS`, `VCACHE`, `TEX_DIRTY`,
`WB_SKIP`, `PIPE_MEMO`, `UPLOAD_PROLOGUE`, `SHADER_REUSE`, `ASYNC_POOL_RESET`, `PERF_SAMPLE=0`, `DEPTH_FEEDBACK` по вендору (SYNC-05),
`DM_HLE_SPIN_US=30`, warm-лист `vk-pipeline-list.bin` (2 потока при старте) + `vk-pipeline-cache.bin` (сохраняется потоком каждые 300 кадров
с новыми сборками; в o2 вырос 100 → 698 КБ), `DM_GFX_ASYNC_PIPELINES` (код есть, было 0). Не-Adreno: `VTX_RETYPE` (MALI-01), фолбэки +
`SIM_GPU` (MALI-02), `PIPE_SERIAL`/`PIPE_GUARD` (PVR-02), модули под замком (PVR-05 как TL2), `ANISO_FALLBACK` (PVR-04), `UBO_DYNAMIC`
(0/1, не режим 2), `NOOUT_MASK`, `PASS_PROF`/`PASS_VALUES` (MALI-07), `SELFTEST` (MALI-06), `BC_SELFTEST` (TEX-02). `DM_GFX_NOLOAD` — старый
d2r-маркер (не MALI-03).

**Нет** (кроме перенесённого в `dos2-cpu`, раздел ниже): весь ETR-стек (PERF-E01…E20: `AHEAD/SYNC_XWAIT/WAIT_EVENT`, `REPORT_FAST`, occ3/`SAMPLES_TS`, `DISPATCH_RUNS/RP_DEPS`,
`COND_RENDER`, `MEMO_GEN/DESC_FAST/MEMO_BINDLESS`, `DYN_STATE/DRAW_FAST`, `CPU_FIX`, `WARM_SHADERS/PREP_SHADERS`, `TEXSTREAM/ASYNC_TEX`,
`ZC_TYPE/MIN_KB/STICKY/WRITE`, `COMPLETION_THREAD`, `PARTS`, `FRAME_PACING`, `PIN_AUTO`, `DM_CTL`, `GPUCLK`, `DESC_RECYCLE`, `NREC/RECXL`),
раунды D16 4–5 и mt (`STATEGEN`, `UBO_VIEWCACHE`, `INDEX_TAIL_CLAMP`, `SHADER_RAWCACHE`, `DESWZ_DIRECT`, `PIPE_MEMO_N`,
`DESC_HAZARD_EPOCH`, `DRVBLK_CACHE`), `CHUNK_MID/SLOTS`, `PRESENT_THREAD`, `RECORD_THREAD`, `AUDREN_CATCHUP`, тайловые политики D16
(MALI-03 `NOLOAD/TILE_AREA/TILE_BARRIER/STORE_*/LEAN_USAGE/AFBC`), MALI-10…14 (zero-copy режимы 2/3, `UBO_DYNAMIC=2`), PVR-01/06
(`FENCE_GUARD/TRYLOCK`), MALI-13 безопасный режим.

## Решение по переносу (что окупается на DOS2)
| Приём | Есть ли выигрыш на DOS2 по логам | Решение |
|---|---|---|
| PERF-D12 `SHADER_RAWCACHE` (шейдеры с FUSE) | да: 12 332 файла `dm/shaders` на `/sdcard`, каждая новая стадия = fopen бандла + неудачный fopen `spv-override` на FUSE на потоке трансляции | **сделано нативнее**: один файл-пак `shaders.xgspack` + `spv-override` листается один раз (шаг 3, `DM_GFX_SHADER_PACK`, `DM_GFX_SPV_OVERRIDE_PROBE`) |
| ФС-путь (нет ни в одном порту) | да: все 15 стопов загрузки — метаданные FUSE | **сделано**: индекс RomFS (шаг 2, `DM_FS_ROM_INDEX`) |
| `ASYNC_PIPELINES` (D2R/D3-код, ETR `ASYNC_PIPE` предложен 3) | да: 99 сборок = 1.9 с в одном окне | **включено для DOS2** (`=1`, шаг 3) |
| PERF-D03 `SUBMIT_CHUNK_US` | меню: CPU 11 мс и GPU 11.7 мс уже перекрываются кольцом 2 слотов (60 fps) | без изменений; A/B в кампании: `DM_GFX_SUBMIT_CHUNK_US=2000` |
| PERF-D06 `AUDREN_CATCHUP` | нет: catch-up только на старте, `effects 0` | не нужно |
| PERF-E02/E04/E19 | нет: 1 эпоха на кадр (учёт 0.05 мс), 0 dispatch | не нужно |
| PERF-E03 / E05 (цена query на GPU, `COND_RENDER`) | возможно: ~1028 samples-счётчиков на кадр, SetRenderEnableConditional в коде 36 | сначала замер `DM_GFX_PASS_PROF`; occ3 — только опция в менеджере драйверов |
| PERF-E06/E08, D11/D13 (мемо, CPU/draw) | 5 мкс/draw уже ниже ETR (8.7); кадр в меню упирается в vsync, кампания тяжелее | **частично сделано** (ветка `dos2-cpu`): `DRAW_FAST`, `DYN_STATE` 0x2, `DESC_RECYCLE` + замер `draw()` по секциям; `MEMO_GEN`/`DESC_FAST` — по `desc_ns` с телефона (ниже) |
| MALI-03, MALI-10/12/14, PVR-01/06, MALI-13 | для Mali/PowerVR — да (модели D16: 33–35 → 55–60 fps), на Adreno 0 | в `NATIVE_PLAN.md` (шаг 8): средний объём, высокий риск, без устройства не проверить |
| port-common | `pc_check`: audioout-keeper, lse-select, thread-exit-hoststack, vma-guard в синхроне; pipeguard-v2 MISSING (ручной); ldn-lan DIVERGED (сеть) | перф не меняет; pipeguard-v2 — отдельной задачей |

## Что мерить на телефоне (общая методика KB: ABBA ×2, медиана, кадры > 33 мс)
- Загрузка кампании и смена локаций: число строк `[stall] no frame presented`, их длительность; `[fs] rom index …` (одна строка при монтировании:
  `storage FUSE`, время построения) и строки счётчиков (`lookups found / not found / opens by dup`). A/B: `DM_FS_ROM_INDEX=0` против умолчания,
  `DM_FS_STATS=1` в обоих (средняя / макс. задержка pread).
- Проверка индекса: один прогон `DM_FS_ROM_INDEX=2` → в логе 0 строк `rom index mismatch`.
- Шейдеры: `NVN shader pack …: 12329 bundles`, `NVN PERF shader-pack hits=…`, в тех же окнах `create_ms` и fps против `DM_GFX_SHADER_PACK=0`;
  один прогон `=2` → 0 строк `differs from its file`.
- Первые сборки пайплайнов: окна с `pipelines_new>0`: `async_skips/async_built` > 0, `create_ms` ниже; глазами — эффекты появляются на
  несколько кадров позже, без мусора. Второй запуск: `NVN warm list=` ≈ число пайплайнов первого, окна без `created`.
- Поток записи: строки `[nvn] recording thread …` (`DM_NVN_THREADS`, раз в 10 с) — ожидаем один поток с BeginRecording/submit.

## Для KB (вставить в `docs/port-kb`, правка вне worktree не делалась)
- `hle-os-fs-saves.md`: «HLE-35 RomFS на FUSE: индекс в памяти вместо readdir на каждый компонент пути (`DM_FS_ROM_INDEX`, DOS2 1 / 2 check),
  dup удержанного дескриптора, `DM_ROMFS_APPDATA`; симптом — стопы 2–10 с в `fuse_simple_request`; эталон dos2-perf `hle_fs.c romi_*`; собрано, не проверено».
- `performance.md`: «PERF-X04 пак шейдеров `shaders.xgspack` (`DM_GFX_SHADER_PACK`, `xgs_pack.py`) — нативный вариант PERF-D12; собрано, не проверено».
- `MATRIX.md`: строки HLE-35 / PERF-X04 (DOS2 ✅ не подтв., остальные ❌; D16 ≈ `SHADER_RAWCACHE`), `ASYNC_PIPELINES` для DOS2 = 1.

## Перенос CPU-стека ETR в `draw()` (ветка `dos2-cpu`, 14.11.2026; собрано, на телефоне не проверено)
Подробности, умолчания и порядок A/B — `DOS2_PORT.md`, раздел «CPU draw()». Коротко (умолчания только для `DM_GAME_DOS2`, остальные игры как были):
| Переключатель | DOS2 | Эталон ETR | Ожидание | Проверка |
|---|---|---|---|---|
| `DM_GFX_PERF_SAMPLE` не 0 (1/32 → 1/64 по `DM_GFX_PERF_BUDGET_US`) + `draw_sections_ms_per_frame` | вкл. | cmp16 A0, perf4 `DRAW_MARK` | замер: куда уходят ~5 мкс/draw | `est_cost_ms` < 0.2 |
| `DM_GFX_DRAW_FAST` | 1 | cmp16 «fast block» | −(сборка CreateInfo) на попадание мемо | `=2` → `drawfast verify bad=0` |
| `DM_GFX_DYN_STATE` (только 0x2, POD-карта) | 2 | nx2b `20bc37b` | попадания для A-B-A-B | под `DRAW_FAST=2` тоже |
| `DM_GFX_DESC_RECYCLE` | 1 | xlat X5 `906cc4d` (−0.27 мкс/draw) | ≈ −0.6 мс/кадр в меню | `=2` → `descrecycle verify bad=0` |

Не перенесено: `MEMO_GEN`, `DESC_FAST` (+`DESC_REUSE`), `CPU_FIX`, `DYN_STATE` 0x1, `VB_ONECALL`, `DIAG`, `PERF_SCOPES` — причины в `DOS2_PORT.md`.
Решение по ним — по первой строке телефона: `parts_ms_per_frame` (descriptor / pipeline / snapshot), `draw_sections_ms_per_frame`,
`desc_ns_per_frame memo_key=`, `setcache memo_hits / memo_misses why=`.

Для KB (правка вне worktree не делалась): `performance.md` PERF-E06/E08/E19 — строка «DOS2 (dos2-cpu): DRAW_FAST=1, DYN_STATE=2 (только POD), DESC_RECYCLE=1;
собрано, не проверено»; `MATRIX.md` — колонка DOS2 для `DRAW_FAST`/`DYN_STATE`/`DESC_RECYCLE` = ✅ не подтв., `MEMO_GEN`/`DESC_FAST`/`CPU_FIX` = ❌ (ждёт замера).
