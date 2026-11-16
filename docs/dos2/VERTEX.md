# DOS2: «вершинная» цена 3D-меню. Это не вершины, а фиксированная цена на каждый draw и WFI от штампов отчётов

Ветка `dos2-vtx` (worktree `wt-vtx`). Данные: `src/work/logs/m4.log`…`m9.log` (Mi 14 Ultra, Adreno 750, Turnip, `DM_GFX_PASS_PROF=300,180,0`,
3D-меню; m7 = 2160x972, m9 = 1440x648, m5 = `TU_DEBUG=sysmem,nolrz`, m6 = GMEM). Разбор:
`python tools/dos2_gpu_breakdown.py <log> --gpass` (теперь печатает и `us/draw`, и `rep=`, если в логе есть).

## 0. Итог
- **Вершинный путь чистый.** Атрибуты идут через настоящий Vulkan vertex input, форматы 1:1 без CPU-конверсии, буферы вершин и индексов
  привязаны zero-copy к памяти игры, индексы исходные (firstIndex 0 от смещённого адреса, base vertex игры), VS крошечные (28–140 ALU-операций).
- **Цена не от вершин и не от пикселей, а от количества draw.** Регрессия по проходам m9 (кадр 841):
  **≈1.47 мкс на любой draw + ≈0.26 нс на индекс**. G-buffer (529 draw, 1.86 М индексов) = 0.78 + 0.48 мс → 1.26 мс, совпадает с замером.
  Тени (1116 draw, 5.65 М) = 1.64 + 1.46 → 3.10 мс, тоже совпадает. Проход i=14 (61 draw, 13 тыс. индексов) — 2.8 мкс/draw почти без геометрии.
- **В pre-pass сверх модели +1.7 мс** (модель 1.25 мс, замер 2.99 мс; на 2160x972 — 3.07 мс, т.е. от разрешения не зависит).
  Это **~1050 отчётов samples-passed за кадр** (`NVN PERF d3perf inpass_reports=1050`, примерно 2 на draw pre-pass): каждый пишет внутри
  прохода timestamp **BOTTOM_OF_PIPE**, а Turnip перед таким штампом ставит **WFI** (CP ждёт, пока весь конвейер опустеет). 1.7 мс / 1050 =
  **1.6 мкс на отчёт**, как в ETR (GPUCOST: ≈1.35 мкс на штамп, PERF-E03). При `DM_GFX_COND_RENDER=0` (по умолчанию) хост сэмплы вообще не
  считает (игра не делает ResetCounter), т.е. эти 1050 WFI покупают только поле timestamp, которое игра не читает.
- **Сделано (выкл.):** `DM_GFX_SAMPLES_TS` 2 = штамп TOP_OF_PIPE (без WFI) / 4 = один BOTTOM_OF_PIPE-штамп на проход для всех его samples-отчётов.
  Плюс диагностика: `rep=` в `NVN GPASS` (отчёты в проходе), `DM_GFX_VTX_STATS=1` (строка `NVN PERF vtxstats`: страйды и типы draw).
- **Ожидание:** pre-pass 3.0 → ~1.3–1.6 мс, GPU кадра в меню ≈12.3 → ≈10.7–11 мс (**−1.3…−1.7 мс**). Картинка не меняется (значения
  сэмплов те же; меняется только поле timestamp отчёта). На телефоне **не проверено**.

## 1. Как идут вершины (пункт 1 задачи)
`dm/gfx/vulkan/engine/xbg_game.cpp`, `draw(prim, first, count, indexed, …)`:
- **Vertex input настоящий:** `VkVertexInputAttributeDescription` на каждый вход VS (`state.shader[0]->inputs`), `binding = stream`,
  `stride = state.stream[].stride`, `inputRate` по divisor (divisor > 1 — fatal, в DOS2 не встречается). Эмуляции выборки в VS
  (storage / texel buffer) нет; во входах VS — обычные `OpVariable Input`.
- **Форматы:** таблица `format()` 1:1 NVN → Vulkan (включая 16-бит и normalized). `vertex_format_fallback()` срабатывает только если у
  формата нет `VERTEX_BUFFER_BIT` — в логах m4–m9 ни одной строки об этом. Константный (не привязанный) вход — нулевой буфер 16 байт,
  страйд 0. `DM_GFX_VTX_RETYPE` — только не-Adreno.
  **Scaled-форматы (ветка `dos2-vtxscaled`, 15.11, KB XCL-07, port-common `vtx-scaled`):** `format()` теперь знает NVN `*_UI2F` / `*_I2F`
  (0x51–0x68 без 32-битных, 0x6b / 0x6c RGB10A2) = Vulkan `*_USCALED` / `*_SSCALED` (раньше — `FATAL: Unsupported NVN format`; DOS2 их
  в наших логах не ставил ни разу). Если у формата нет `VERTEX_BUFFER_BIT` (AMD-подобные GPU: стоковый Xclipse, RADV; симуляция
  `DM_GFX_SIM_GPU=noscaled`, входит в `xclipse`), тот же буфер привязывается как `*_UINT` / `*_SINT` той же раскладки, а вариант VS
  (`vtx_retype_module`, ключ `<shader>#vi<код>c<маска>`) объявляет вход `uvec` / `ivec` и после каждого чтения делает
  `OpConvertUToF` / `OpConvertSToF` — на любом вендоре, независимо от `DM_GFX_VTX_RETYPE`. Лог: `NVN vertex format N (USCALED) has no
  VERTEX_BUFFER support: bound as M, the vertex shader converts uint -> float`. Тест: `tests/vertex_scaled_test.py` (spirv-val, 38 модулей).
  Замечание к nx2android #404 (Xclipse 940): там формат NVN 0x1d = `R16G16B16_SFLOAT` (vk 90), а не scaled — у нас это уже закрыто
  расширением до 4 компонент (`xbg_vertex_widen`, симуляция `novertex3`).
- **Буферы:** `snapshot()` → `zc::view()`: при `DM_GFX_ZEROCOPY=1` (в DOS2 включено, лог `zero-copy guest memory=1`) VB и IB — это
  сама память игры (AHB-чанки по 64 МБ), без копий. `NVN PERF zerocopy hits=4341/кадр, sticky_miss=164` (sticky = страницы, которые
  игра пишет CPU во время кадра, — копия в stream-арену, ~0.5 МБ). Снимки всего 32 МБ/с.
- **Индексы:** u16/u32 — как есть (zero-copy), `vkCmdDrawIndexed(count, instances, firstIndex=0, base_vertex игры, base_instance)`;
  адрес IB уже со смещением first — **повторное использование post-transform кэша не ломается** (индексы не переписываются, не
  разворачиваются). **u8** разворачиваются в u16 на CPU (`std::vector` + stream buffer на draw) — хотя `VK_KHR_index_type_uint8` у Turnip
  есть; сколько таких draw в DOS2 — покажет `vtxstats u8_index=` (ожидаем 0). Quad-примитивы — CPU-перепись (`quad_rewrites=0` в меню).
- **Primitive restart** только для strip/fan с индексом «все единицы» (у треугольных списков DOS2 его нет).
- **Instancing:** обычный `instances` в draw; вершинные шейдеры меню — без инстансинга (единичные draw).

## 2. Цена VS (пункт 2)
Ключи из `NVN GPASS first=/last=` (m9, кадр 841), SPIR-V из `work/shaders-vk/<key>.xgs`, `spirv-opt -O` (NDK shader-tools):

| VS | проход | инструкций SPIR-V (сырые → -O) | арифметика после -O |
|---|---|---|---|
| `414ed80a860bb7e9` | pre-pass (FadePattern, 1-й draw) | 144 → 100 | 2 FMA, 8 загрузок |
| `32c6099a8c607c18` | pre-pass (последний, TR_Layer DEP) | 387 → 218 | 20 FMA + 12 mul/add (две матрицы 4x4) |
| `2c41ecfc1c0f1e77` | G-buffer (1-й) | 1063 → 372 | 71 ExtInst (FMA) + 60 FMul + 4 FAdd, isnan-страж |
| `acae47a9ec95ab1c` | тени (1-й) | 752 → 318 | 50 FMA + 27 FMul + 15 FAdd |

- Накладные Maxwell-трансляции малы: cbuffer как массив 4096 vec4 с постоянными индексами (ir3 «проталкивает» диапазоны в const-файл),
  по-скалярные `OpLoad` — компилятор их векторизует. Нет эмуляции rcp, нет flip/clip-конверсии глубины в VS (viewport-flip делается
  `vkCmdSetViewport`, `DM_GFX_TAA_FIX`). `Invariant` на Position обязателен (pre-pass + G-buffer с EQUAL). PointSize=1 и объявленные, но
  не записанные ClipDistance[8]/CullDistance[1] (без capability) — копейки.
- Порядок: 100 ALU-операций × ~1.5 М вершин теней = 150 М операций; 750 это ≈0.1 мс. **ALU VS не узкое место**, нативные мобильные VS
  (метод `dos2_post_mobile.py`) здесь ничего не дадут — поэтому не делались.

## 3. Post-transform кэш (пункт 3)
Индексы исходные, draw индексированные, без перебазирования и primitive restart в списках → кэш работает как у игры. Остаточная
«вершинная» цена (0.26 нс на индекс, ≈4 индекса за такт) — это предел фиксированных блоков (VFD/PC/setup) и чтение вершин из
interleaved-потока; в тенях это ≈1.46 мс из 3.1 (см. §6, следующий шаг).

## 4. Цена на draw (пункт 4)
| проход (m9, кадр 841) | draw | индексов | мкс | мкс/draw | отчётов |
|---|---|---|---|---|---|
| i=4 pre-pass 1440x648 | 526 | 1 854 249 | 2986 | **5.7** | ~1050 (оценка, `rep=` даст точно) |
| i=5 G-buffer 5 MRT | 529 | 1 858 380 | 1258 | 2.4 | ? |
| i=8 тени 1536² | 1116 | 5 650 290 | 3102 | 2.8 | ? |
| i=14 (декали/прозрачное) | 61 | 12 846 | 172 | 2.8 | ? |

- Подстановки: m7 (2160x972) pre-pass 3.07, G-buffer 1.52 (у G-buffer пиксельная часть ≈0.25 мс, у pre-pass — ноль).
- `NVN PERF binds`: 1910 привязок descriptor set на кадр (на 2264 draw), 6.8 dynamic offset на привязку, 852 — «только смещения».
  Пайплайны — из мемо (`pipememo hits=2263.8 misses=0`, `drawfast` 100 % hit), новых пайплайнов в меню 0.
- **Скачки ≈1.2 мс:** во всех прогонах (m4–m9, и nolrz, и GMEM) время то pre-pass, то теней прыгает ровно на ~1.2 мс (3.05 ↔ 4.25,
  3.10 ↔ 4.33), всегда в одном из двух самых длинных проходов. Похоже на внешнее вытеснение GPU (другой контекст, композиция) —
  **гипотеза**, не геометрия. Сравнивать A/B по медиане, не по максимуму.
- Фиксированная часть ≈1.47 мкс на любой draw (≈3.3 мс/кадр на 2264 draw) — отдельная тема (§6).

## 5. Что сделано
### 5.1 `DM_GFX_SAMPLES_TS` (boot env, по умолчанию 1 = как было) — `xbg_game.cpp` namespace `samplets`
Меняется только путь `DM_GFX_REPORT_INPASS` для samples-passed (`OP_REPORT_COUNTER`); timestamp-отчёты игры и путь вне прохода — без изменений.
- **1** — BOTTOM_OF_PIPE на каждый отчёт (как раньше).
- **2** — TOP_OF_PIPE на каждый отчёт (Turnip пишет always-on счётчик без WFI; так же как ETR g1 mode 2). При ретайре слота такой штамп
  поднимается до штампа предыдущего отчёта слота (`clamped=`), поток отчётов остаётся монотонным.
- **4** — первый samples-отчёт в проходе резервирует timestamp-запрос, остальные отчёты прохода получают **тот же** запрос; один
  BOTTOM_OF_PIPE пишется при конце прохода (`end_open_pass`), перед timestamp-отчётом игры или перед сабмитом. У каждого отчёта время ≥
  настоящего и ≤ следующего отчёта; один WFI на проход вместо ~1050. Ретайр читает общий запрос один раз (`reads_saved=`; −~1000
  `vkGetQueryPoolResults` на кадр на CPU).
- Строка `NVN PERF samplets mode= reports= stamps= top= shared= flushes= clamped= reads_saved= lost=` (на кадр). `lost` должен быть 0.
- Счётчик сэмплов не трогается: при `DM_GFX_COND_RENDER=0` их и так нет (0), при `≠0` сэмплы идут из occlusion-запросов, а их конец на
  стоковом Turnip — свой WFI (поэтому `COND_RENDER≠0` вернёт часть цены, PERF-X06).

### 5.2 Диагностика
- `NVN GPASS … rep=N` — сколько counter-отчётов записано внутри прохода (подтверждает, что ~1050 в pre-pass). `dos2_gpu_breakdown.py --gpass`
  печатает `us/draw` и `rep=`.
- `DM_GFX_VTX_STATS=1` — `NVN PERF vtxstats depth-only|colour draws= idx= stride_avg= bytes_mb= hist<=12/16/24/32/48/more= multi_stream=
  u8_index= nonindexed= instanced=` (на кадр). `stride_avg` взвешен по индексам; `bytes_mb` — верхняя оценка байт выборки вершин без кэша.
  Нужен для решения §6.2.

### 5.3 Сборка
NDK: `cmake -S dm/android -B work/build-android -G Ninja … -DDM_GAME=dos2` + `cmake --build work/build-android --target dm` rc=0;
APK `tools/build_apk.ps1` (GRADLE 9.8.1, отладочный ключ), версия не менялась; `tests/source_structure_test.py` PASS
(новые глобальные на пути draw классифицированы: `cmd index` — S, счётчики vtxstats — K, `on` — D).

## 6. Дальше (не сделано)
1. **Фиксированные ≈1.47 мкс на draw** (≈3.3 мс/кадр). Кандидаты: 1910 привязок set с 6.8 dynamic offset (Turnip перезаливает
   дескрипторы / UBO-диапазоны const-файла на draw), смена стейта. Сначала разложить KGSL-счётчиками (ETR `DM_GFX_GPUCTR`,
   `xbg_gpuctr.h`) — без них это гадание. Пробник ETR `DM_GFX_PROBE_SCISSOR1` (scissor 1x1) отделяет front end от заливки.
2. **Тени, вершинная часть ≈1.46 мс** (5.65 М индексов). Если `vtxstats depth-only stride_avg` ≥ 32 (interleaved-вершина целиком ради
   позиции) — плотный поток позиций для depth-only draw (кэш по адресу VB + трекер страниц, как `vcache`) может срезать чтение памяти
   в 2–4 раза; если страйд уже ≤ 16 — смысла нет. Дешёвая альтернатива уже есть: `DM_DOS2_SHADOWS=0` (2 каскада, −1/3 draw и индексов).
3. Патченный Turnip `occ3` (ETR) снимает и WFI конца occlusion-запроса — нужен только вместе с `DM_GFX_COND_RENDER≠0`; по правилу хозяина
   — только опция в менеджере драйверов.

## 7. A/B на телефоне (3D-меню, по одному прогону, сравнивать медиану pre-pass и `gpu_ms`)
```
tools/run_dos2.sh v0 120 DM_GFX_PASS_PROF=300,180,0 DM_GFX_PASS_VALUES=0 DM_DOS2_DRS=off DM_GFX_VTX_STATS=1
tools/run_dos2.sh v4 120 DM_GFX_PASS_PROF=300,180,0 DM_GFX_PASS_VALUES=0 DM_DOS2_DRS=off DM_GFX_SAMPLES_TS=4
tools/run_dos2.sh v2 120 DM_GFX_PASS_PROF=300,180,0 DM_GFX_PASS_VALUES=0 DM_DOS2_DRS=off DM_GFX_SAMPLES_TS=2
tools/run_dos2.sh v4c 120 DM_GFX_PASS_PROF=300,180,0 DM_GFX_PASS_VALUES=0 DM_DOS2_DRS=off DM_GFX_SAMPLES_TS=4 DM_GFX_COND_RENDER=2
```
Смотреть: `python tools/dos2_gpu_breakdown.py work/logs/v4.log --gpass` (pre-pass мкс/draw: ждём 5.7 → ~2.5–3, `rep=` у i=4),
`NVN PERF samplets` (`lost=0`, `stamps`≈число проходов с отчётами при 4), `NVN PERF v=1 gpu_ms`, `vtxstats` из v0. Глазами: меню без
изменений. Принять 4 (или 2) по умолчанию, если pre-pass −1 мс и больше и `lost=0`; v4c — проверка, что с условным рендером пропуск
draw окупает WFI конца запросов.

## 8. Кросс-портовость
Транслятор и хост общие (`dm/`): тот же `OP_REPORT_COUNTER` / `DM_GFX_REPORT_INPASS` в D3 / TL2 / XC2 / DOOM (линия d3perf), в ETR —
свой `DM_GFX_SAMPLES_TS` 0–3 (g1; 0 отвергнут из-за мерцания при подсчёте сэмплов с AHEAD=4, 3 = штамп у 1 из 8). Режим 4 («один
штамп на проход») — новый, его можно перенести в ETR как вариант без пропажи настоящего GPU-времени. Проверка в других портах — одна
строка лога: `NVN PERF d3perf inpass_reports=` (D3 ≈16/кадр — выигрыша нет; если у порта сотни — переносить). Регрессию «мкс/draw +
нс/индекс» по `GPASS` стоит делать в любом порте, прежде чем лечить «вершины».

## 9. Черновик записи для KB (`docs/port-kb/performance.md`, не вносил)
```
### PERF-X08 «Вершинно-bound» проход = цена на draw + WFI штампов samples-отчётов (DOS2, 14.11.2026) — проверять в любом порте
- **Симптом:** DOS2 3D-меню (Adreno 750, Turnip): depth pre-pass 3.0 мс на 526 draw / 1.85 М индексов, не дешевеет при 2.25× меньше
  пикселей; G-buffer той же геометрии 1.3 мс; LRZ off и GMEM не влияют.
- **Причина:** не вершины (VS 28–140 ALU, vertex input настоящий, zero-copy, индексы исходные). Регрессия по GPASS: ≈1.47 мкс на draw +
  ≈0.26 нс на индекс; pre-pass сверх модели +1.7 мс = ~1050 samples-passed отчётов в проходе (`inpass_reports`), каждый — BOTTOM_OF_PIPE
  timestamp = WFI на Turnip (≈1.6 мкс). При COND_RENDER=0 сэмплы не считаются вовсе — WFI покупал только поле timestamp.
- **Фикс (выкл.):** `DM_GFX_SAMPLES_TS` 2 = TOP_OF_PIPE (зажим монотонности при ретайре), 4 = один штамп на проход (пишется в конце
  прохода / перед timestamp-отчётом / сабмитом). Диагностика `NVN GPASS rep=`, `DM_GFX_VTX_STATS=1`. dos2-port ветка `dos2-vtx`,
  `xbg_game.cpp` namespace `samplets` / `vtxstats`, `docs/dos2/VERTEX.md`.
- **Ожидание:** −1.3…−1.7 мс GPU в меню. На телефоне не проверено.
- **Другие порты:** D3/TL2/XC2/DOOM — тот же путь REPORT_INPASS, смотреть `inpass_reports`; ETR — PERF-E03 (`SAMPLES_TS` 0–3), режим 4 — кандидат.
```
MATRIX: строка «samples-отчёт без WFI» — DOS2 ✅off, ETR ✅ (свой SAMPLES_TS), остальные ? (по `inpass_reports`).
