# NR-33A — тяжёлые движковые шейдеры по смыслу (продолжение NR-04) + слитый AA маршрута A в кампании

Ветка `dos2-nr33a` (worktree `wt-nr33a`, база `dos2-nr-all` debf665). Телефон не трогался. Часть B (материалы Base_PBR / Tile /
TR_Layer / ATM_Wind / персонажи) — ветка `dos2-nr33b`; все добавления этой части в общих списках лежат отдельными блоками
`NR-33A (engine post)` в конце (`ENTRIES` в `tools/dos2_nr_sh_pack.py`, `nr_sh_table` в `dm/gfx/nr/nr_sh.c`, `SHADERS` / `case()` в
`tools/dos2_post_mobile.py`). Единственный общий счётчик — `CHECK(nr_sh_count == 10 + 3 …)` в `tests/nr_sh_test.cpp`: при слиянии
= 10 + (наши) + (B).

Данные: лог хозяина 18.11 `src/work/logs/play-1118` (палуба + 23 мин Форта Радости), `tools/dos2_shader_heat.py … --level FJ_FortJoy_Main`.

## 1. Слитый AA (F1A) работал только в меню — две причины и исправление

### Симптом
`[nb] post plan aa DM_NB_POST_AA=1: F1A a frame 0.00 … left as F1 + the console's FXAA 1.00; refusals, another FXAA pass reads what
Sight's target feeds N, its target was read by an operation M` во всех окнах кампании; в меню `F1A a frame 0.12`. Проход PPFXAA
(2160x972 RGBA8) в Форте — 0.39 мс выборочного кадра (`dos2_shader_heat.py`).

### Причина 1 — «its target was read by an operation» (NB_PAW_BAD): одно сохранение выключало маршрут A до конца сессии
- Строка 7516 `boot.log`: `aliased targets tracked 1 (read by an operation, never aliased again: 1; such reads now 1)` — сразу после
  `swkbd … «Трюм - 0h16m»` (сохранение игры). Операция (копия миниатюры сейва) прочла цель FXAA (`0x1181000000`), а
  `nb_post_aa_plan` помечал образ `bad` **навсегда**: с этого кадра каждый кадр с туманом (`Sight -> EoCFog (carry) -> FXAA`) получал отказ
  (диагностика кадров 5090–5092: `first reader … PPFXAA …; the FXAA pass that refused it = none`).
- Исправление (`dm/gfx/nb/nb_post_aa.h`): операция, читающая отслеживаемый образ **после его настоящей записи в этой же подаче**
  (исполняемый не выброшенный проход рисует в него — в т. ч. сам проход FXAA, который тогда не выбрасывается — или операция пишет в
  него), читает настоящий образ: в этой подаче AA-проход просто исполняется (`t.now`, новая причина `NB_PAW_OPREAD` «an operation reads
  its target in this submission (the AA pass runs)»), со следующей подачи слияние продолжается. Чтение **до** любой записи в подаче
  (устаревший образ) — по-прежнему `bad` навсегда. BE не менялся: настоящий draw FXAA в O2 и так снимает псевдоним (`pre()`).
- Строка лога 10 с: `… such reads now N; read by an operation after its real write - AA pass run there, fusion goes on: K)`.

### Причина 2 — «another FXAA pass reads what Sight's target feeds»: обводка между Sight и FXAA
- Кадр 5240: `Sight (S) -> EoCOutlineCombine (S -> A) -> EoCFog (A -> S) -> PPFXAA (S -> B) -> combine`. Обводка (выделение / наведение)
  не класс «carry» → она «первый читатель», FXAA после неё читает производный образ → отказ. Кадры 2811 / 5239 — та же обводка с `fs 0
  NOT exact` (программа ещё не опубликована BE в первые кадры её появления) — те отказываются при любой настройке, это 1–2 кадра.
- Исправление под переключателем **`DM_NB_POST_AA_OUTLINE`** (0 по умолчанию = как было; 1 = EoCOutlineCombine — класс carry, как
  EoCFog): `nb_post_aa_carry2(fs, outline)` во FE (`nb_post_aa_state.outline`, `hle_dos2_nb_post.c` читает env) и в BE
  (`xbg_nb_post.inc` `aa_outline`, `carry()` уже следует выходу F1A через проход). Цепочка Форта = 2 carry (предел `NBPA_CARRY = 2`),
  FXAA выброшен, combine читает Sight-цель. **Вид меняется:** обводка рисуется поверх уже сглаженного кадра — её край не сглаживается
  FXAA (как туман в NB-18b-10). Нужен взгляд хозяина: персонаж с обводкой (наведение / выбор), FXAA вкл.
- Строка: `[nb] DM_NB_POST_AA_OUTLINE=1: EoCOutlineCombine … carries F1A's output`; в строке 10 с `fog / outline steps …`.

### Доля кадров (по окнам 10 с лога, Форт)
Отказы «outline» : «op» ≈ 351:239, 601:0, 0:601, 527:65, 450:114, 176:183 — обводка есть примерно в половине кадров. После
исправления 1 сливаются все кадры без обводки (≈ 40–60 %), с `DM_NB_POST_AA_OUTLINE=1` — практически все.

### Тесты
`nb_post_test.c`: «op after the write» (копия цели FXAA в конце кадра после слитых кадров → в этой подаче `NB_PAW_OPREAD`, FXAA
исполняется, образ не `bad`; следующая подача снова F1A), «sticky» теперь — копия **до** записи (`op_first`) → `bad` навсегда, как
прежде; «Fort Joy» — цепочка Sight → outline → fog (в Sight-цель) → FXAA: без `outline` — отказ `OTHER_FXAA` с узлами обводки и FXAA,
с `outline` — F1A, `carried 2`, флаги Sight FUSE|AA, outline / fog AA, FXAA ELIDE|AA, combine AA, отслеживается цель FXAA. PASS.

## 2. Новые шейдеры по имени (DM_NR_SH, пак `native-nr.xgspack`, стадия 8 setup)
Метод NR-04: GLSL по смыслу транслированного SPIR-V с тем же порядком операций (fma, обратные, `nanz`, поправка масштаба), голова бандла
игрока байт в байт, `spirv-val`, интерфейс = транслированному, дескрипторы ⊂ таблицы, **бит в бит в `tools/spv_interp.py`** (400
случайных вызовов: 300 при масштабе 1, 100 со случайными, в т. ч. отрицательными; NaN / inf / 0 в текстурах и делителях). Все новые —
класс post, вариант x, `proven = 1`, т. е. включаются тем же `DM_NR_SH=1` (умолчание DOS2), допуск («уровень 2») не понадобился.

| Имя (ключ FS) | Проход | Бит в бит | Статич. инстр. (spirv-opt) | Исполнено / пикс. | Выборок / пикс. | мс в Форте (выб. кадр, Mi 14) |
|---|---|---|---|---|---|---|
| DeferredFog (d8aeec640d465275) | 2160x972 R11G11B10F | 400 / 400 | 193 → 188 | 129 → 87 (−33 %) | 2 → 2 | 0.15 |
| PPBloomUpsampleAndBlur (4e290c9cf1162fe9) | 2160x972 R11G11B10F | 400 / 400 | 96 → 47 | 96 → 47 (−51 %) | 6 → 6 | 0.17 |
| CombineUI_NoFetch (98186c22a7209861) | 2400x1080 RGBA8 | 400 / 400 | 178 → 118 | 89.6 (макс 105) → 48.0 (55) (−46 %) | 2.43 → 1.77 (−27 %) | 0.54 |
| PPBloomScaleBiasDownsampleAndBlur (74fe9ae2066d53da) | 1080x486 R11G11B10F | 400 / 400 | 113 → 95 | 113 → 95 (−16 %) | 4 → 4 | 0.07 |
| PPBloomDownsampleAndBlur (0e3fc07b320db1e9) | 540x243 и мипы | 400 / 400 | 86 → 42 | 86 → 42 (−51 %) | 5 → 5 | 0.02 |
| PPFXAA (691768aa08545d95) | 2160x972 RGBA8 (+ портрет 80x100) | 400 / 400 | 398 → 349 | 127 (318) → 100.5 (259) (−21 %) | 5.42 → 5.42 | 0.39 (там, где не слит) |

Пояснения: DeferredFog — векторная арифметика и быстрый путь масштаба 1 (иначе — код перевода буквально), выборки те же;
блум — те же отводы в том же порядке fma (5 билинейных отводов не сливаются без потери бит в бит); CombineUI_NoFetch — перевод всегда
выбирал сцену (в (0,0), когда результат не нужен) и ещё раз на полупрозрачных пикселях UI, переписанный выбирает только там, где
результат используется, не больше раза; PPFXAA — операция в операцию (формы сравнений с NaN перевода сохранены), выигрыш только ALU.
Не зависят от SPIR-V: маршрут A опознаёт FXAA по ключу FS (`nb_post_aa_class`), F1A собирается из транслированного бандла; F2
(Vignette ∘ CombineUI) рисует свой сплавленный модуль, замена работает только в кадрах без F2.
Ключ пака: имя длиннее 29 символов (`PPBloomScaleBiasDownsampleAndBlur`) → `x.<ключ FS>` (`nr_sh_pack_key`, `dos2_nr_sh_pack.py`,
`setup_dos2.py step_nrpack` — одно правило; предел ключа xgspack 31 символ).
Проверки: пак `--pack shaders.xgspack --n 40 --programs` — 19 бандлов, имена ↔ ключи 16 / 16, post / light 13 / 13 бит в бит 40 / 40;
`nr_sh_test` 454 / 0 синтетики, 703 / 0 с паком; `source_structure_test.py` PASS (NR-04: 16 движковых шейдеров).

Setup: `ENTRIES` → стадия 8 (`step_nrpack`) собирает новые бандлы сама (хэш инструментов включает `tools/dos2_post/*_mobile.frag`, т. е.
у игрока пак пересоберётся при следующем setup). Пак для телефона: `dos2-port/work/native-nr33a/native-nr.xgspack`
(→ `files/dm/shaders/native-nr.xgspack`).

## 3. Что не сделано и почему
- **ClusteredLightingWithSSAO «мобильный алгоритм» (fp16, меньше итераций)** — не делал. NR-04 уже снял −44 % исполненных инструкций
  при 0 мс на Adreno 750 (замер `gp1`/`gp2`): проход упирается не в ALU. Без замера проходов (`DM_GFX_PASS_PROF`) с разбивкой
  выборки / ALU переписывание с допуском — гадание; следующий шаг — прогон с `DM_GFX_PASS_PROF=300,180,0` и вариантом без SSAO-выборки
  (A/B), потом решать. DeferredSky / FadeBlending уже нативные (NR-04).
- **Материалы поверхностей** (FlowingWater 2611 инстр., Blood 1458, Fire_Ground 1437, Poison 1500, Oil 1631, CloudShadow 685,
  Rain 949, Ocean_infinite 1712; в Форте вместе ≈ 1.5–2 мс) — не делал: исходников нет, разбор по смыслу 700–2600-инструкционных
  модулей — часы агента на каждый, а механизм NR-04 знает только `EngineShaders:Shaders/NX` (сверка имён / пути); путь материалов
  (`Materials.pak`) строит часть B — после её слияния эти восемь — отдельный шаг на её механизме.

## 4. Ожидаемый выигрыш
- **AA в кампании (исправление 1, по умолчанию):** в кадрах без обводки PPFXAA-проход (0.39 мс выборочного кадра на Mi 14, полноэкранный
  RGBA8 load + store ≈ 16 МБ трафика) исчезает, F1A дороже F1 на ≈ 50 инстр. / пикс. → **≈ −0.25…−0.35 мс GPU** в этих кадрах; с
  `DM_NB_POST_AA_OUTLINE=1` — во всех. Mali / PowerVR (тайлер, проход = загрузка + выгрузка тайлов, слабее полоса): ×3–4 → ≈ −1…−1.5 мс.
- **Шейдеры §2:** на 750 по опыту NR-04 — десятые доли мс в сумме (≈ 0…−0.1); на слабом ALU (S7, Mali G57/G68, PowerVR BXM) —
  пропорционально снятым инструкциям этих проходов (см. таблицу), ориентир −0.2…−0.6 мс.

## 5. Прогон на телефоне, который это подтвердит
Бэкап `files/save`; APK этой ветки; пак `work/native-nr33a/native-nr.xgspack` → `files/dm/shaders/`. Без лимита `DM_DOS2_FPS=0`,
`DM_GFX_PASS_PROF=300,180,0`; Форт, 60 с от загрузки, одно сохранение во время прогона.
1. Умолчания: ждём `[nb] post plan aa … F1A a frame > 0` в кадрах без обводки и **после сохранения** (`read by an operation after its real
   write … fusion goes on: ≥ 1`, `never aliased again: 0`); `[nr] sh` — `entries replaced 13 of 16`, строки `check/->` для новых имён,
   ни одной `not the verified pass`; в `dos2_gpu_breakdown.py --gpass` PPFXAA пропадает в слитых кадрах.
2. `DM_NB_POST_AA_OUTLINE=1`: `F1A a frame ≈ 1`, отказов `another FXAA pass …` почти нет; снимок экрана персонажа с обводкой (вид края).
