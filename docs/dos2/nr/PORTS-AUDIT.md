# Аудит переноса приёмов других портов в DOS2 (19.11, ветка `dos2-ndrift`, раунд 2 NR-DRIFT)

Источник: все строки `docs/port-kb/MATRIX.md` (основная таблица D16 / ETR / D3 / XC2 / TL2 / XC3; у DOS2 своей колонки нет — DOS2 =
клон TL2 от 14.11 + своё в разделе DOS2), где в другом порте ✅, а в колонке TL2 не ✅ и не «—»; каждая проверена grep'ом переключателя /
символа по дереву `wt-ndrift` и числами логов DOS2 (O_ab_a1 19.11 Форт стоя, play-1118, C_*). Плюс `port-common/tools/pc_check.py`.
Деревья других портов (ETR `src-cmp16`, D16 `doom-port/src`, D3, TL2) — только чтение.

## 1. Перенесено (отдельные коммиты)
| ID | Что | Переключатель, умолчание DOS2 | Коммит | Тесты |
|---|---|---|---|---|
| NR-DRIFT §8 | фильтр одиночных всплесков GPU в контроллере DRS (пещера `main+0xc05dcc`) | `DM_DOS2_DRS_SPIKE` 0 / 2 счёт / 1 | `3d98963` | `nr_drift_test` 80, `nr_drift_cave_test.py` (Unicorn) 12 208 PASS |
| SH-20 | Adreno 8xx: переделки quad-производных и warp32 выкл. как на 740–799 (D16 #39/#47) | `DM_GFX_ADRENO8_POLICY`: не задан = 0 (сегодня), 1 / auto, force | `87ab61c` | `adreno_policy_test` +12 PASS |
| PERF-E01 (часть) | `nvnSyncWait` с таймаутом на ещё не транслированном fence (TL2 #12 / ETR) | `DM_NVN_SYNC_XWAIT`: **2 = счёт + лог с цепочкой вызовов игры** (поведение как сегодня), 1 = ждать трансляцию, 0 = ничего | `87ab61c` | `source_structure ports1119_check`; сборка |
| PERF-X09 | частота GPU / шины: источники D3 (kgsl, MediaTek GED, gpufreq v1/v2, Mali, все devfreq) в строке `[nr] growth`, МГц | вместе с `DM_NR_GROWTH` | `2c1f042` | `nr_drift_test` +7 (разбор строк D3) |

Почему `SYNC_XWAIT` по умолчанию 2, а не 1 (как у TL2): в сохранённых логах DOS2 нет признака таких ожиданий (Mi 14 трансляцию не
отстаёт на 2 сабмита — `port-common/migration/sync-xwait.md`). `=2` ничего не меняет, а первые 4 случая и каждый 512-й пишет с
адресами игры: один прогон на слабом телефоне (или `DM_GFX_SIM_GPU=mali` с нагрузкой) решит, включать ли 1.

## 2. Уже есть в DOS2 (проверено) — клетка ✅ / ≈
| ID | В DOS2 |
|---|---|
| PERF-D01 / D02 / D03 / D04 / D09 / D10 | как TL2 ✅ (`DM_GFX_SUBMIT_CHUNK_US` есть, у DOS2 1 сабмит / кадр) |
| PERF-D07 present-thread | ✅ (`NVN PERF present_thread mode=1` в O_ab; pc_check «diverged» = форма NR-11) |
| PERF-D11 DIRTY_BLOCKS | ✅ `DM_DIRTY_BLOCKS` + NR-ZERO эпохи |
| PERF-D12 DESWZ_DIRECT | ✅ `DM_GFX_DESWZ_DIRECT` |
| PERF-D13 PIPE_MEMO_N | ≈ `DM_GFX_DRAW_FAST` + pipememo (хиты 100 %, `NVN PERF pipememo misses=0`) |
| PERF-D15 DRS по нашим счётчикам | ≈ DOS2 читает настоящие метки (`the game reads 9.05, the backend measured 9.02`) + NR-DRIFT удержание / фильтр |
| PERF-X01 IDLE_RETIRE | ✅ (`NVN PERF idle mode=2`) |
| PERF-E03 SAMPLES_TS | ✅ NR-14 (`DM_GFX_SAMPLES_TS`); Turnip occ3 — по KB только опция (хозяин 11-08) |
| PERF-E05 COND_RENDER | ✅ `DM_GFX_COND_RENDER` + NR-03 |
| PERF-E08 DRAW_FAST / DYN_STATE | ✅ (`drawfast mode=1 dyn=2`) |
| PERF-E10 прогрев | ≈ NR-07 (по событиям движка) + NR-31 seed + pipe-marker-mmap (`warm_built=2309`) |
| PERF-E11 TEXSTREAM | ≈ NR-22 (`DM_NR_TEXSTREAM`, поток заливки) |
| PERF-E14 ADPF, PERF-X08 подсказки | ✅ pc_check `adpf-boost`: present, in sync |
| PERF-E19 DESC_RECYCLE | ✅ (`descrecycle mode=1`) |
| PERF-X11 -O3, PERF-X14 маркеры mmap | ✅ pc_check in sync |
| PERF-X12 readback cached | ✅ код есть (`DM_GFX_READBACK_CACHED`), умолч. 0 — writeback у DOS2 0,0 МБ / кадр |
| APP-05 / APP-14 ядра | ✅ pc_check `cpu-place` in sync + `DM_CPU_SELF` |
| HLE-06 / 09 / 17 / 38 | ✅ pc_check present (vma-guard «diverged» = DOS2-эпохи NR-ZERO в `nvn_dirty.c`, всё нужное на месте: `g_pressure`, `nvn_dirty_relieve`, `GEN_STICKY`) |
| XCL-07 vtx-scaled | ✅ (`has no VERTEX_BUFFER support`, `vtx_conv`, `noscaled` есть; «diverged» = формы строк DOS2) |
| VK-UBO-MEMO (desc-memo-keys) | ✅ (NR-19: «manual merge» у pc_check ложный, проверено исполняемым тестом) |

## 3. Не переносится — причина одной строкой (цифры DOS2)
| ID | Причина |
|---|---|
| PERF-D05 CHUNK_MID + 3 слота | 1 сабмит / кадр и Mi 14 упирается в GPU (GPU простаивает 0,44 мс / кадр: 12,30 мс кадра при 11,86 мс GPU); резка рвёт проходы на тайлере |
| PERF-D08 / X10 поток записи Vulkan | анализ PERF-X10: −0,6…+2,4 мс CPU и 0 fps у D3 / TL2 / DOS2 |
| PERF-D11 STATEGEN / UBO_VIEWCACHE / INDEX_TAIL_CLAMP / PROTECT_BGEN | путь draw у DOS2 свой (NR-09 прямые draw, NR-08 дескрипторы, NB-12 кольцо UBO на месте, NR-ZERO эпохи); GPU-indirect draw у DOS2 нет (хвост индексов не к чему) |
| PERF-D12 SHADER_RAWCACHE | шейдеры из пака `shaders.xgspack` в памяти: открытий бандлов на FUSE нет |
| PERF-D13 DESC_HAZARD_EPOCH / DRVBLK_CACHE | NR-08 уже держит опасности только у GPU-записанных образов; блоки драйвера — NR-09U / кольцо UBO |
| PERF-E02 REPORT_FAST, E19 плоские контейнеры | отчётов 2–4 на кадр (`events reports=2`), `cleanup_ms queries 0.00` — ускорять нечего (у ETR ~4290) |
| PERF-E04 DISPATCH_RUNS / RP_DEPS / CLEAR_OUTSIDE | `dispatches=0.0` за кадр; барьеры — NR-10 (65 → ~0 полных); CLEAR_OUTSIDE в ETR без выигрыша и с подозрением на мерцание |
| PERF-E06 MEMO_GEN / DESC_FAST / MEMO_BINDLESS | bindless — только ETR; у DOS2 NR-08 (дескрипторы по поколению текстур, `desc_ns` 0) |
| PERF-E09 CPU_FIX / FAST_LOOKUPS | `snapfront invalidations` 2–24 / кадр (у ETR ~3100), отчётов 2–4; индекс чанков zero-copy = NR-TRX `DM_NR_TRX_ZC` |
| PERF-E12 ZC_TYPE / ZC_MIN_KB / ZC_STICKY / ZC_WRITE | zero-copy DOS2 свой (липкие промахи считаются, NR-TRX STICKY), writeback 0 МБ; `snap_mb` ≈ 0,2 МБ / кадр |
| PERF-E13 COMPLETION_THREAD / PARTS / FRAME_PACING / DIAG | writeback 0,0 МБ; 1 сабмит, GPU 96 % занят; `pace-fractional` «not relevant» (pc_check); выборка перф-разделов ~0,07 мс |
| PERF-X16 FENCE_FLUSH / PRESENT_SIDE / POOLRESET_SPLIT | Mi 14: кадр = GPU + 0,44 мс (у TL2 Mali было +4,4 мс) — верхняя граница +3,7 %; кандидат только после замера на Mali (`NVN PERF block_ms legacy_wait` / `poolreset wait_ms` на слабом телефоне) |
| MALI-10 / 15 zero-copy на Mali | решено с цифрой: WC dma-buf zero-copy −26 % fps на G720 → DOS2 держит на Mali зеркало NR-06 |
| MALI-11 / PVR-10 ZC_MAP / ZC3 | то же: zero-copy на тайлерах у DOS2 выкл. по политике |
| PVR-01 FENCE_GUARD / RECOVER | ≈ безопасный режим DOS2 (откат fence trylock, MALI-13 ✅ 17.11) |
| SYNC-06 / 07 / 11 FEEDBACK_COPY / MERGE_DISJOINT / GPU_INDEX_STRICT | исправления корректности ETR под его симптомы; у DOS2 таких симптомов нет, GPU-indirect нет |
| SYNC-17 / 21 апскейлы FSR / SGSR | делает агент «хаков» этой ночью (PERF-X23, FSR 1 EASU) |
| SH-12 / 13 / 17 COMPARE_BY_USE / loop guard / CLIP_FIX | правки шейдеров DOOM / ETR под их баги; у DOS2 свои паки (NR-04 / 33) и нет этих симптомов |
| PERF-E16 / E20 NVN как драйвер / descriptor buffer | у DOS2 свой путь к тому же (NR / NB); ETR-варианты не проверены даже у себя |

## 4. pc_check (`python port-common/tools/pc_check.py dos2-port/wt-ndrift --nso dos2-port/work/exefs-update`)
present, in sync: adpf-boost, audioout-keeper, cpu-place, nvn-ctlmem, pipe-marker-mmap, pipeguard-v2, thread-exit-hoststack, xbg-o3.
present, diverged (формы DOS2, функции на месте, проверено): ldn-lan (свой `hle_ldn.c`), lse-select (`hle_nvn.c` NR-28 lean entry),
pc-core (свой `pc.h`), present-thread (NR-11 present), vma-guard (NR-ZERO), vtx-scaled. MISSING: desc-memo-keys (ложно, NR-19),
dirty-order (ручной модуль из DOS2 же — ветка `dos2-nmem` этой ночи). Not relevant: pace-fractional.

## 5. Что прогнать на телефоне
- `DM_NVN_SYNC_XWAIT` (умолч. 2): в любом прогоне `grep "SyncWait on an untranslated fence" <run>.dm.log` — 0 строк = DOS2 не ждёт так,
  `=1` не нужен; строки есть = включать `=1` (сначала на слабом / `DM_GFX_SIM_GPU=mali`).
- `DM_GFX_ADRENO8_POLICY=1` — только у игрока на 8 Elite с артефактами частиц.
- `[nr] growth: … nodes readable:` — на MediaTek / Exynos у игроков (на Mi 14 ожидаем «none»).
