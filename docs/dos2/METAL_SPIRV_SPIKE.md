# DOS2: шейдеры iPad (Metal AIR) → Vulkan SPIR-V — спайк metal2vulkan (14.11.2026)

Вопрос: можно ли взять мобильные Metal-шейдеры iPad (`Materials.metallib`, `Core.metallib`, IPA 3.6.72) и превратить их в SPIR-V для
нативного бэкенда (`NATIVE_BACKEND_PLAN.md`) вместо переведённых шейдеров Maxwell (`work/shaders-vk`). Время: ~70 мин, всё на ПК, телефон
не трогался. IPA — только чтение. `IPA_RECON.md` §4 («готового инструмента нет») **устарел**: инструмент есть и работает.

## 1. Инструмент
- `steelbrain/metal2vulkan` (GPL-2.0+/LGPL-3.0+), ветка **`development`, коммит `7ef17497d3980a846d71f314feeb976fa7e0d242`** (12.11.2026;
  `master` 219f0136 отстаёт на 3605 коммитов; форки Mirone/tqmane старше `development` — не брали). Клон и сборка:
  `work/tools-ext/metal2vulkan`, `cargo build --release -p metal2vulkan --features cli` с `RUSTC_WRAPPER=` (их `.cargo/config.toml` зовёт
  sh-обёртку, на Windows не запускается). Тулчейн из `rust-toolchain.toml` (`nightly-2026-11-03`) поставлен rustup'ом в профиль пользователя.
- **Локальная правка** (`crates/metal2vulkan-air/src/abi/module.rs`, `compile_options`): iOS 13.5-биткод несёт опции
  `air.compile.native_long_long_disable / native_double_disable / native_wide_vectors_disable`, которых нет в закрытом списке → `UnknownKey`
  на **каждой** функции. Первая пропускается всегда, остальные — при `M2V_SPIKE_IGNORE_OPTS=1`. Это опции эмуляции int64/double/широких
  векторов на старом GPU Apple; на семантику материалов влиять не должны (не проверено).
- Профиль устройства: адренового в репо нет; взят `amd-rdna2-apu-radv.json` минус `VK_KHR_shader_float_controls2` и
  `VK_KHR_shader_maximal_reconvergence` (на стоковом Adreno их скорее всего нет). Остался `SPV_KHR_fma` — тоже убрать в настоящем профиле
  (нужен `vulkaninfo --json` с телефона → их `scripts/regen-device-profiles`).
- Свои скрипты (вне дерева порта): `scratchpad/m2v/{mtlb_list.py,batch.py,analyze.py}`; результаты там же (`results_*.jsonl`, `val_*.json`,
  `compare.json`, `out_mat/` 401 МБ SPIR-V + meta JSON).

## 2. Цифры
**Materials.metallib** (15 693 функции = 7 980 vertex + 7 713 fragment): 7 980 пайплайнов (vertex + одноимённый fragment; 267 без fragment,
теневые/глубинные).
| | число |
|---|---|
| собрано (`PASS` внутренней проверки) | **7 869 / 7 980 (98.6 %)**, 15 471 модуль |
| отказы | 111, **все** `Lowering(Arith) NotImplemented` во fragment варианта `_FOR` (Preview_*, FX_MAT_Overlay/Burning/Acid…, GlowColor, MagicWall…) |
| `spirv-val --target-env vulkan1.1` | **0** по построению: выход SPIR-V 1.5 (инструмент требует Vulkan 1.2) |
| `spirv-val vulkan1.2` (NDK, SPIRV-Tools 2022.4) | 537 |
| `spirv-val vulkan1.2 --scalar-block-layout` | **13 263 / 15 471 (85.7 %)**; остальные 2 208 — только «Invalid capability 6030» = `SPV_KHR_fma`, которого старый валидатор не знает (артефакт профиля, см. §1) |

`--scalar-block-layout` обязателен: UBO лежат массивами `uint` со stride 4 (`UniformWords`) — нужен `scalarBlockLayout` (Vulkan 1.2, Adreno 7xx есть).

**Core.metallib** (173 функции): 5 compute-ядер (`PPComputeLUT`, `BlurH/VBilateral`, `ClearRT`, `bgra_to_rgba`) — 5/5 собраны и валидны.
Пары VS/PS по именам — моя эвристика: 75 из 134 собраны, 58 отказов `UnmatchedVarying` = неверная пара (PS подставлен к чужому VS),
1 — мой формат RT (`R16Float` под целочисленный выход `CompressBC4`). Отказов инструмента на Core не видно; настоящие пары брать из кода.

Обвязка CLI, которую пришлось дать: вершинный дескриптор (по атрибуту на буфер 16+i, `Float4`; атрибут 8 у `_SK` = индексы костей, нужен
целый формат `UShort4` — иначе `Lowering(StageIo)`; 2 623 пайплайна потребовали этот повтор), форматы RT (8 × `R16Float`), глубина `Depth32Float`.
Настоящие значения есть в `ShaderCache.pak` (`.pso`: `vertexAttribs` format/offset/bufferIndex, `bufferLayouts` stride/step, `pixelFormat`,
blend, depth/stencil) — формат TLV, **не разобран** в спайке.

## 3. Сравнение 20 материалов (iPad → metal2vulkan vs Switch `shaders-vk`)
Инструкции после `spirv-opt --freeze-spec-const -O` (NDK); «тело» = без типов/констант/декораций. Совпадение по пути `.bshd`
(в `shader_map.tsv` один файл на ключ, поэтому обе стадии нашлись у 3 555 из 6 003 Switch-материалов).
| материал | VS iPad / Switch | PS iPad / Switch | текстуры iPad / Switch | вх. PS | RT |
|---|---|---|---|---|---|
| Engine_Default_Mesh_Lit_Opaque_ST_DEF | 268 / 374 | 288 / 249 | 2 / 2 | 4 / 4 | 4 / 4 |
| Characters_PBR_Base_SK_DEF | 558 / 1099 | 639 / 402 | 5 / 5 | 7 / 7 | 4 / 4 |
| Base_PBR_Base_PBR_ST_DEF | 272 / 374 | 435 / 253 | **4 / 3** | **5 / 4** | 4 / 4 |
| NAT_Source_Lake_PBR_SK_DEF | 804 / 1202 | 960 / 610 | 5 / 5 | 7 / 7 | 4 / 4 |
| TR_BloodForest_PBR_SK_DEF | 537 / 1055 | 544 / 370 | 5 / 5 | 4 / 4 | 4 / 4 |
| ATM_Water_Plant_AlphaTest_2S_SK_DEF | 907 / 1246 | 439 / 257 | **4 / 3** | **5 / 4** | 4 / 4 |
| RS3_FX_Lavafall_01_SK_DEF | 790 / 1211 | 691 / 600 | 5+1 VS / 5+1 VS | 9 / 9 | 4 / 4 |
| Surface_Water_Cursed_02_STI_DEF | 319 / 463 | 797 / 558 | 6 / 6 | 6 / 6 | 4 / 4 |
| Cloud_Blood_Electrified_SK_DEF | 463 / 760 | 872 / 545 | 5 / 5 | 3 / 3 | 4 / 4 |
| ATM_Skydome_NoFog_SK_DEF | 360 / 709 | 137 / 114 | 1 / 1 | 1 / 1 | 4 / 4 |

Итог по 20 (VS) / 18 (PS; у 2 фрагментов spirv-opt 2022.4 не понимает `SPV_KHR_fma`): **VS iPad = 0.60× Switch** (тело 0.54×),
**PS iPad = 1.45× Switch** (тело 1.41×). VS Switch раздут переводом Maxwell (выборка из банков c[], скиннинг); PS iPad раздут эмуляцией
семантики Metal (точные NaN/деление/min-max, sRGB-варианты, сэмплер через спец-константы). Сумма по паре примерно равна → **выигрыша по
ALU «мобильные шейдеры» не дают**. **fp16: ноль** во всех модулях выборки (нет `OpTypeFloat 16`, нет заметок `*Half*`) — материалы iPad
посчитаны во float, «half-шейдеров» нет. Заметки точности у всех: DeviceTranscendentals/Denormals, FastMathRewrites, NativeDivision/Dot/MinMax.

Интерфейс (что кормить): iPad — VS константы в buffer 5, 6 (+7), PS buffer 0…2, текстуры 0…N, **1–3 сэмплера на все текстуры** (Switch —
пара tex+sampler на каждую), атрибуты — `[[attribute(0..8)]]` (число = Switch `inputs`), 4 RT во всех DEF. Vulkan-сторона от инструмента:
один set, UBO как `uint[]` со смещением через push (8–24 байта), 8–15 спец-констант (состояние сэмплера, `ConstantLanes`, диапазоны UBO),
варианты пайплайна по фактам (sRGB-текстура, cube-сэмплер…). Switch: `ubo` = банки NVN, `c1` = банк драйвера, `support` = наш буфер.

## 4. Что нужно, чтобы использовать в нативном бэкенде
1. **Связка ресурсов.** Движок Switch шлёт `BindUniformBuffer(stage, bank)` / `BindTexture(stage, slot)` в раскладке NX-шейдеров. Нужна
   таблица на материал: банк NVN → Metal buffer index, слот текстуры → texture index, выбор сэмплера (у iPad свой, меньше). Источник — свой
   разбор отражения AIR (`function.reflection`) + `.bshd/.lsb` обеих платформ. **Главный риск — раскладка cbuffer:** движок Switch пишет
   константы со смещениями NX-шейдера, Metal-шейдер ждёт свои (выравнивание `float3` 16 Б и т.п.). Нужна проверка смещений по полям или
   перепаковка на draw → это не «подменить модуль», а слой трансляции констант. Оценка 8–16 агент-ч + сверка на 5–10 материалах.
2. **Состояние на пайплайн.** Разобрать `.pso` (3 970 сочетаний) → вершинный дескриптор, форматы RT, blend, depth/stencil; это и есть
   прогревочный список. 2–4 агент-ч. Сочетаний только 3 970 против 7 980 пар — остальные у Apple в ODR/рантайме.
3. **Проходы/G-buffer.** RT = 4/4 совпадает, но кодировка G-buffer и набор входов PS местами разные (Base_PBR, Water_Plant: +1 текстура,
   +1 varying) — iPad собран под свой конвейер (clustered lighting, `_SHA` вместо `_DEP`, `CombinePPUI`). Смешивать материалы iPad с
   освещением/постом Switch без сверки нельзя; Core-шейдеры iPad тянут за собой проходы iPad (см. `IPA_RECON.md` §3).
4. **Рантайм-API инструмента.** Каждый пайплайн требует plan/instantiate (push-смещения, спец-константы по реальным сэмплерам и форматам) —
   либо линковать crate (Rust → .so, лицензия GPL/LGPL), либо офлайн-компилировать все варианты и повторить логику привязки у себя.
5. 111 `_FOR` (Preview/FX overlay) — оставить Switch-переводы.

## 5. Риски
- **«valid ≠ считает то же самое».** Проверены только структура и валидатор; ни одного пикселя не сравнено. Инструмент обещает
  «типизированный отказ вместо другого результата», но это для Metal-семантики AIR, а не для совместимости с NX-движком.
- Правка опций компиляции (§1) — непроверенное допущение. Профиль Adreno сымитирован; на реальном драйвере возможны другие отказы.
- SPIR-V 1.5 + scalar block layout + множество спец-констант и вариантов → больше пайплайнов и компиляций на телефоне, чем сейчас.
- Лицензия: GPL-2.0+/LGPL-3.0+ — для офлайн-инструмента на ПК проблем нет, для встраивания в APK нужно решение хозяина.

## 6. Рекомендация
**No-go как замена переведённых шейдеров** в нативном бэкенде: перевод работает (98.6 % пайплайнов, 85.7 % модулей валидны старым
валидатором, остальное — артефакт профиля), но выгоды нет — fp16 нет, суммарный ALU не меньше (VS −40 %, PS +45 %), а цена — слой
перепаковки констант и связки ресурсов под чужой интерфейс плюс риск расхождения G-buffer. **Go как справочник/донор:** (а) VS iPad вдвое
компактнее — если VS окажется узким местом на тайлерах, брать отдельные VS (их интерфейс к движку проще — 2–3 буфера + атрибуты); (б) Core:
`CombinePPUI`, деинтерливинг SAO, `CompressRG11B10F` — переводятся без отказов, годны как образцы слитых проходов (`IPA_RECON.md` §6 п.4);
(в) `.pso` — каталог состояний для прогрева. Дальнейший шаг, если хозяин захочет: настоящий профиль `vulkaninfo` с Mi 14 Ultra +
разбор `.pso` + один материал (Engine_Default_Mesh_Lit_Opaque_ST_DEF) вживую с перепаковкой констант — 6–10 агент-ч, 1–2 прогона на телефоне.
