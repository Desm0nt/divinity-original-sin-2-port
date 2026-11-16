# DOS2: проходы/шейдеры Switch, дорогие на тайлерах, и замены из iPad (14.11.2026)

Вопрос хозяина: какие проходы/шейдеры Switch (Maxwell, immediate-mode) неэффективны на тайловых GPU (Adreno / Mali / PowerVR) и
чем их заменила iPad-сборка (Metal, тайлер); выдрать iPad-версии как SPIR-V. Только ПК, телефон не трогался, файлы порта не менялись
(кроме этого документа и `work/ipad-shaders/`). Основа: `IPA_RECON.md` §3–4, `METAL_SPIRV_SPIKE.md`.

## 0. Коротко
- Реальный кадр кампании на телефоне (q2_boot, 3D 1800x806, выход 2000x896) **не содержит** шахматки и compute вообще:
  `dispatches=0.0` в 604 из 606 окон PERF (`q1.log`, `q2_boot.log`); единичный dispatch — пересчёт LUT. `CB_Resolve`,
  `BlurH/VBilateral`, SSAO (`PPSAO*`) в кадре не видны. Значит, «выкинуть шахматку» сейчас ничего не даёт.
- **Поправки к `IPA_RECON.md` §3:** у Switch `DeferredDirectionalWithSSAO`, `BilateralUpsample`, `CalculateBilateralOffsets` — пустые
  `.bshd` без программы (`shaders-jobs/empty.txt`), т.е. Switch тоже освещает только `ClusteredLightingWithSSAO`; `PPFXAA` у iPad
  **есть** (`Core.metallib` + `Shaders/Metal/PPFXAA.bshd`). Реальные отличия набора: у iPad нет `CB_Resolve`, `DownsampleDepth*`,
  `CrossBilateralUpsample`, `CombineUI_NoFetch/_BicubicFilter`, каскадов 3x/512/768; есть `CombinePPUI`, деинтерливинг SAO,
  `CompressRG11B10F/BC4`, PCF на аппаратном сравнении глубины.
- Что реально стоит денег на тайлере в текущем кадре (по убыванию): (1) цепочка **полуразрешённых частиц** (DownsampleDepthMax →
  LinearizeDepth ½ → частицы RGBA16F ½ с depth-feedback → CrossBilateralUpsample), (2) **«Light Shadows» `CascadedPCF_3x768`** — 45
  невекторизованных выборок на пиксель полного кадра, (3) **хвост LDR-поста** из 6 полноэкранных проходов, (4) чтения UI-фильтрами Iggy
  своего же RT (частые разрывы прохода 2000x896). iPad-замена шейдером есть только для (2) и частично (3); (1) и (4) — перестройка
  проходов, не подмена модуля.
- Извлечено: 32 модуля (17 конвейеров) в `work/ipad-shaders/` + `manifest.tsv`; **все 234 модуля** 119 движковых конвейеров iPad
  собраны с настоящими парами VS/PS и проходят `spirv-val --target-env vulkan1.2 --scalar-block-layout` (без scalar — 27/234).

## 1. Метод
- Пары VS/PS iPad — из `MSLShaders.pak` `Shaders/Metal/*.bshd` (144 текстовых дескриптора `[Vertex shader] X [Fragment shader] Y`,
  compute с `numthreads`), а не эвристика спайка (там 58 отказов `UnmatchedVarying` были неверными парами). Перевод — `metal2vulkan`
  7ef17497 + `M2V_SPIKE_IGNORE_OPTS=1`, профиль `adreno_nofma.json` = профиль спайка без `VK_KHR_shader_fma` (иначе 13 модулей валятся
  на cap 6030 в старом spirv-val). 120 конвейеров → 119 собраны (нет `AIGridVS` в metallib). Форматы RT — заглушка `R16Float` ×8.
- Switch: 128 программ `EngineShaders` + 75 стадий из `main` (`programs.tsv`/`jobs.tsv`), SPIR-V из `work/shaders-vk/*.xgs`.
- Признаки из SPIR-V: тело после `spirv-opt --freeze-spec-const -O` (без типов/констант/декораций), статические выборки (Sample/Fetch/
  Gather), циклы, `OpKill`/demote, `FragDepth`, число выходов (MRT), сэмпл-рейт. Статический счёт: у iPad PCF/SAO выборки в циклах.
- Граф кадра — из строк `NVN read break … ps=<ключ> … fmt WxH` (`q2_boot.log` 10000–16540, кампания), ключ → имя по `jobs.tsv`.
  Это не весь граф (только проходы, закрытые чтением), но порядок и размеры видны. `DM_GFX_PASS_GRAPH` в этих логах не включался.
- Скрипты и сырьё (вне дерева): `scratchpad/tile/{conv,feat,sw,ip,cmp,pack,rb,seq}.py`, `sw_feats.json`, `ip_feats.json`, `cmp.txt`.

## 2. Кадр кампании на телефоне (900p, Adreno 750; 60 fps, gpu 9–12 мс, 65–150 проходов, 400–1000 draw)
Цели: 3D 1800x806 (0.9 окна), ½ 900x403, выход 2000x896. Порядок восстановлен по зависимостям (кто что читает) из выборочных строк
read break, не по таймстемпам (форматы NVN: 0x35 D24S8, 0x3f R11G11B10F,
0x25 RGBA8, 0x29 RGBA16F, 0x6 R16):
`CascadedPCF_3x768` (1800, читает R16) → `ClusteredLightingWithSSAO` (1800, R11G11B10) → `DownsampleDepthMax` (D24S8 1800 → ½,
пишет `FragDepth`) → `LinearizeDepth` (1800 и ½) → частицы в RGBA16F ½ (`depth-feedback break` на ½-глубине #171) →
`CrossBilateralUpsample` (½ RGBA16F → полный HDR) → `EoCFog` → bloom: `PPDownSampleLog2` 1800 → `PPDefault` 256…1 (9 крошечных
проходов) → `PPLuminanceAccum` 1x1 → `PPBloomDownsampleAndBlur` 900/450 → `PPBloomUpsampleAndBlur` 225/450/900 → `PPApplyLUT` 1800
(HDR → RGBA8) → `EoCSight` → `PPFXAA` → `EoCOutlineBlurH` 1800 / `BlurV`, `Combine` 900 → `FadeBlending` 1800 → `CombineUI_NoFetch`
(1800 → 2000x896) → UI Iggy (ключи `main+0x2ace788`, `0x2a507c8`, `0x2a3cae8` читают 2000x896 RGBA8 — 9040 из 24308 строк read break).
Объёмы: 1800x806 RGBA8/R11G11B10/D24S8 = 5.8 МБ, RGBA16F = 11.6; ½ = 1.45 / 2.9; 2000x896 RGBA8 = 7.2 МБ.

## 3. Рейтинг кандидатов
| # | Проход Switch (ключ) | Признаки на тайлере | iPad | Ожидаемый выигрыш | Что нужно |
|---|---|---|---|---|---|
| 1 | Полуразрешённые частицы: `DownsampleDepthMax` facf8294 + `LinearizeDepth` ½ + `CrossBilateralUpsample` 34348860 | +4 границы прохода, проход с `FragDepth` (без early-Z/HSR/FPK), depth-feedback на ½-глубине = ещё разрыв, апскейл грузит и пишет полный HDR | цепочки нет (нет шейдеров и строк `Downsample Depth`/`Resolve Offscreen Particles`) — частицы в полном разрешении в основном HDR-проходе | ~36 МБ DRAM/кадр (≈2.2 ГБ/с при 60), −4…5 проходов; цена — overdraw частиц ×4 пикселя | **не шейдер**: флаг движка «offscreen particles» (найти по строке `Resolve Offscreen Particles` в main, Ghidra headless) или NB-план, рисующий draw'ы частиц в основной HDR-проход |
| 2 | «Light Shadows» `CascadedPCF_3x768` 1372db84 | **поправка 14.11 (ветка `dos2-shadows`): 45 — статический счёт (5 путей × 9); пиксель делает 9 точечных выборок, 18 в полосе смешения каскадов, ≈13 млн/кадр, не 65; замена — `DOS2_PORT.md` «Мобильный резолв теней»** · полный кадр 1800x806, **45 развёрнутых выборок** глубины без аппаратного сравнения (`SampleExplLod`, Dref нет ни в одном движковом шейдере Switch — это дизайн Switch, Ryujinx-транслятор Dref умеет) ≈ 65 млн выборок/кадр; 1065 инстр. | `CascadedPCF_{2,4}x{1024,2048}`: цикл каскадов + **16 выборок Poisson с `SampleDref`** (32 в зоне смешения), 267–279 инстр. | ≈2.8× меньше выборок, ALU ≈3–4× меньше; полоса та же (1 вход + 1 выход) | iPad-набора 3x768 нет → нужен режим теней 2/4 каскада × 1024 (настройка) **или** свой PCF с Dref по образцу iPad в раскладке констант Switch (проще: без трансляции cbuffer). Сэмплер атласа с `compareEnable` |

| 1 | Полуразрешённые частицы: `DownsampleDepthMax` facf8294 + `LinearizeDepth` ½ + `CrossBilateralUpsample` 34348860 | +4 границы прохода, проход с `FragDepth` (без early-Z/HSR/FPK), depth-feedback на ½-глубине = ещё разрыв, апскейл грузит и пишет полный HDR | цепочки нет (нет шейдеров и строк `Downsample Depth`/`Resolve Offscreen Particles`) — частицы в полном разрешении в основном HDR-проходе | ~36 МБ DRAM/кадр (≈2.2 ГБ/с при 60), −4…5 проходов; цена — overdraw частиц ×4 пикселя | **не шейдер**: флаг движка «offscreen particles» (найти по строке `Resolve Offscreen Particles` в main, Ghidra headless) или NB-план, рисующий draw'ы частиц в основной HDR-проход. **Найден** (14.11): байт `[[main+0x2bb8a60]+0xc7]`, ставится при загрузке уровня 0x104cd4c; `DM_DOS2_PARTICLES_FULL=1` держит его 0 (DOS2_PORT.md «Частицы: полуразрешённая offscreen-цепочка») |
| 2 | «Light Shadows» `CascadedPCF_3x768` 1372db84 | полный кадр 1800x806, **45 развёрнутых выборок** глубины без аппаратного сравнения (`SampleExplLod`, Dref нет ни в одном движковом шейдере Switch — это дизайн Switch, Ryujinx-транслятор Dref умеет) ≈ 65 млн выборок/кадр; 1065 инстр. | `CascadedPCF_{2,4}x{1024,2048}`: цикл каскадов + **16 выборок Poisson с `SampleDref`** (32 в зоне смешения), 267–279 инстр. | ≈2.8× меньше выборок, ALU ≈3–4× меньше; полоса та же (1 вход + 1 выход) | iPad-набора 3x768 нет → нужен режим теней 2/4 каскада × 1024 (настройка) **или** свой PCF с Dref по образцу iPad в раскладке констант Switch (проще: без трансляции cbuffer). Сэмплер атласа с `compareEnable` |
| 3 | Хвост LDR-поста: `PPApplyLUT` 921728a5 → `EoCSight` → `PPFXAA` → `EoCOutline*` → `FadeBlending` → `CombineUI_NoFetch` 98186c22 | 6 полноэкранных проходов, каждый ≈ 5.8 чтение + 5.8 запись ≈ 70 МБ/кадр | `CombinePPUI` (VS+PS): 3D-LUT + 3 2D-текстуры (кадр, UI, +1) за один проход; вариантов `_NoFetch/_Bicubic` нет | −1 полный проход (≈12 МБ) при слиянии LUT+UI; ещё −1, если FXAA не нужен | у Switch есть свой HDR-путь (`EoCSight_HDR`, `EoCOutlineCombine_HDR`, `EoCFog_HDR`, `CombineUI_HDR` с тонмапом, 414 инстр.) — **сначала опыт**: AA выкл. в меню игры → исчезают ли `PPApplyLUT`/`PPFXAA`. Только потом NB-18 с `CombinePPUI` |
| 4 | UI-фильтры Iggy (main+0x2ace788 d99f1fdb, 0x2a507c8, 0x2a3cae8; 2–4 выборки) | читают RT, в который рисует открытый проход → разрыв + store/load 7.2 МБ на каждый | те же шейдеры в main обеих платформ — замены нет | зависит от числа фильтров на кадр (меню!) | политика хоста: копия в временную картинку / `sub`-кандидат вместо разрыва (NB-17, граф проходов) |
| 5 | `FadeBlending` 62d683cc **(14.11, ветка `dos2-postsh`: заменён своим SPIR-V бит в бит, исполнено на пиксель 174 → 83, выборок 4/7 → 3/6; DOS2_PORT.md «Мобильные полноэкранные пост-шейдеры»)** | полный кадр R11G11B10 каждый кадр, 15 выборок, 973 инстр. | `FadeBlending_PS`: 14 выборок, **292** инстр. | только ALU (≈3.3×), полоса та же | подмена модуля в том же проходе: t0,t1 + b12 (4 Б push) — сверить константы |
| 6 | SSAO: `PPSAO` 61dce847 (13 выборок, 1115), `PPSAO_Blur` (635), `BlurH/VBilateral` cs (7 выборок, 1460) | в текущем кадре **не работает** | деинтерливинг: `PPSAO_Deinterleave` (4 MRT) → `PPSAO_PS` 4 выборки/345 → `Reinterleave` → `Blur` 147; bilateral cs: 1 выборка в цикле, 836 | только если SSAO включат (настройка качества): ALU 3–4× меньше, кэш-дружелюбный доступ | весь набор лежит в `ipad-shaders/`; новые проходы → NB-план |
| 7 | `CB_Resolve` d83b8d5c (cs, 9 выборок, 1690) | **не исполняется** (dispatch 0) | нет шахматки | 0 сейчас | держать шахматку выключенной; если какой-то пресет её включит — выключать настройкой |
| 8 | Мелочь ALU **(14.11, `dos2-postsh`: все четыре заменены бит в бит, исполнено на пиксель Fog 113 → 71, OutlineCombine 93 → 53, LinearizeDepth 40 → 22, DownSampleLog2 93 → 38)**: `EoCFog` 211→127, `LinearizeDepth` 55→34, `PPDownSampleLog2` 137→78, `EoCOutlineCombine` 168→59 | те же проходы | те же имена | только ALU, полоса без изменений | низкий приоритет |
| — | Не брать: `ClusteredLightingWithSSAO` (iPad 2634 > Switch 2157; **14.11 `dos2-lightsh`: заменён своим SPIR-V по Switch бит в бит, 906 → 508 исполнено на пиксель**), `PPFXAA` (614 > 401), bloom/luminance-цепочка (те же шейдеры у iPad) | | | | |

Счёт iPad раздут эмуляцией семантики Metal (спайк: PS ×1.45), поэтому там, где iPad меньше, разница реальна. VS iPad везде в 2–3 раза
меньше, но VS полноэкранных проходов — 3 вершины, это не важно.

## 4. Извлечённое: `work/ipad-shaders/` (manifest.tsv)
`CombinePPUI`, `FadeBlending`, `EoCFog`, `PPDownSampleLog2`, `LinearizeDepth`, `EoCOutlineCombine`, `CascadedPCF_2x1024`,
`CascadedPCF_4x1024`, `PPSAO_DownsampleDepth`, `PPSAO_Deinterleave`, `PPSAO`, `PPSAO_Reinterleave`, `PPSAO_Blur` — по `.vert.spv` +
`.frag.spv`; `BlurHBilateral`, `BlurVBilateral` — `.comp.spv`; справочно `ClusteredLightingWithSSAO`, `CompressRG11B10F`.
Столбцы manifest: iPad-функция, ключ/имя Switch, стадия, интерфейс (индексы Metal: b — буфер, t — текстура, s — сэмплер; push-байты,
число спец-констант), тела после opt iPad/Switch, выборки/циклы, итог spirv-val. Пример: `CombinePPUI.frag` — b0 (16 Б), b5 (32 Б с
смещения 192), t0, t1, t3 (3D), t12, один сэмплер s1, push 16 Б, 8 спец-констант; `CascadedPCF_4x1024.frag` — b0, b1, t0, t1 (атлас,
Dref), s0, push 8 Б, 5 спец-констант.
Vulkan-сторона везде одна: set 0, UBO как `uint[]` (scalar layout), смещение буфера через push, спец-константы = состояние сэмплера,
`ConstantLanes`, диапазоны UBO (`*.meta.json` в `scratchpad/tile/out/`). **Перед боем пересобрать** с настоящими форматами RT
(из `ShaderCache.pak` `.pso`; сейчас `R16Float` ×8) и с `vulkaninfo --json` телефона вместо имитации профиля. Пиксели не сравнивались.

## 5. Что нужно нативному бэкенду
- **NB-18 (пост + UI):** сначала опыт п.3 (AA выкл.) — если движок сам уходит на `_HDR`-варианты и `CombineUI_HDR`, слияние LUT+UI
  уже сделано движком Switch, `CombinePPUI` не нужен. Иначе: заменить пару `PPApplyLUT` + `CombineUI_NoFetch` одним проходом
  `CombinePPUI` можно только если между ними нет LDR-проходов (Sight/FXAA/Outline/Fade должны уйти в HDR или после) — это изменение
  порядка проходов, не подмена модуля; плюс трансляция констант (b0/b5) и биндингов (t0,1,3,12) из NVN-банков. UI 2000x896 остаётся
  отдельным RT и у iPad (CombinePPUI его сэмплит) — «UI прямо в финальный проход» iPad не делает.
- **NB-17 (слияния) / граф проходов:** цепочка частиц (п.1) и разрывы UI-фильтров (п.4) — главные источники лишних границ; для п.1 —
  флаг движка (поиск в main) либо перенос draw'ов частиц в основной HDR-проход с полным depth (`aba exact` не подходит: апскейл читает
  записанное). Проход с `FragDepth` (`DownsampleDepthMax`) — кандидат «не на тайлере, а как есть» до решения по флагу.
- **Тени (п.2):** проще всего — свой SPIR-V PCF с Dref (16 Poisson по образцу iPad) в интерфейсе `1372db84` (те же 2 текстуры и
  cbuffer Switch) — без трансляции раскладки iPad; сэмплер атласа с compare (на Adreno/Mali аппаратный 2x2). Подмена модуля по ключу
  в `xbg_game.cpp` (shader reuse), за переключателем.
- **`FadeBlending` (п.5):** подмена модуля в том же проходе, если раскладка b12/t0/t1 сопоставима с NVN-банками ключа 62d683cc.

## 6. Что мерить на телефоне (с разрешения хозяина; один агент — телефон)
1. `DM_GFX_PASS_GRAPH=1` (+ `DM_GFX_PASS_PROF`, если есть в сборке) в кампании, 2–3 выборки: полный граф с `att=` load/store,
   `end=` (сколько разрывов дают `read#…` UI Iggy и depth-feedback частиц), `summary` (aba/exact).
2. Время проходов: доля `CascadedPCF_3x768`, цепочки частиц (4 прохода), хвоста поста (6 проходов) от gpu_ms 9–12.
3. Опыт настройками игры: AA выкл. (п.3), качество теней (какие каскады выбирает движок: 2x/4x или только 3x768), SSAO вкл.
   (появятся ли `PPSAO*`/compute, цена). Сравнить passes/draws/gpu_ms с q2.
4. На Mali/PowerVR (если есть устройство) — те же цифры: там границы прохода дороже, чем на Adreno 750, где кадр и так 60 fps.

## 7. Риски
- Все выводы о «тяжести» — статика (инструкции/выборки) + косвенный граф; времени проходов нет. На Adreno 750 кадр упирается в vsync
  (gpu 9–12 мс), выигрыш виден только на слабых тайлерах или как запас по нагреву.
- iPad-модули — другой конвейер (константы, сэмплеры, G-buffer для освещения): «valid ≠ считает то же самое», пиксели не сравнивались.
  Локальная правка опций AIR (`M2V_SPIKE_IGNORE_OPTS`) — непроверенное допущение. Лицензия metal2vulkan (GPL/LGPL) — только офлайн.
