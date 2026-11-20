# NR-33 часть B `DM_NR_MAT` — массовые материальные шейдеры (depth / shadow / G-buffer)

Ветка `dos2-nr33b` (worktree `wt-nr33b`, база `dos2-nr-all` debf665). Телефон не трогался. **По умолчанию 0 — поведение байт в байт
прежнее** (одна проверка целого на загрузку шейдера и на draw). Часть A (пост / освещение / поверхностные FX, ветка `dos2-nr33a`) не
затронута: свои файлы, свои блоки в `setup_dos2.py` / `source_structure_test.py` / `xbg_vendor_policy.h`.

## 1. Что показал разбор (play-1118, Fort Joy, `tools/dos2_shader_heat.py --level FJ_FortJoy_Main`)

Материалы — это не 30 шейдеров, а 2001 программа `*_DEP_NX` + 2001 `*_DEF_NX` (свой шейдер на материал). По SPIR-V транслятора
(`work/shaders-vk/shaders.xgspack`):

| | программ | разных VS | разных FS |
|---|---|---|---|
| `*_DEP_NX` (pre-pass 2160x972 D24S8, тени 1536² D16) | 2001 | 702 | 436 |
| `*_DEF_NX` (G-buffer 2160x972, 5 вложений) | 2001 | 828 | 578 |

Классы depth-FS (436 разных, доля **вершин depth-only проходов** Форта, 7 кадров heat):

| класс | программ (разных SPIR-V) | доля вершин | что делает |
|---|---|---|---|
| **без kill, пишет цвет 0** — семья «fade» | 45 (**1**: `35d08d898762d082` = Base_PBR_ST_DEP, Base_Tile_PBR_ST_DEP, ATM_Wind_Tree_RedChannelOnly, Base_PBR_SK_DEP …) | **64 %** | `.w = (c3[0].x == 1.0) ? clamp(шум + дистанция …) : 1.0`; NVN alpha test GREATER 0.5 включён (лог: `NVN alpha test GREATER ref=0.5 emulated … 35d08d898762d082`) → kill-вариант на **каждый** фрагмент |
| kill (alpha-текстура, слои террейна, шахматка фейда) | 1680 (434), из них 39 с kill только по uniform (гизмо / движок) | 35 % | выходов цвета нет вовсе, kill от текстуры / входов |
| пустой main | 276 (1) | 1.4 % | уже пустой |

Итого: **одна** транслированная FS покрывает 2/3 вершин теней и pre-pass. В проходе без цветных вложений её цвет никуда не идёт; он
кормит только alpha test, а в нём `.w` — константа 1.0, пока материал не «растворяется» у камеры. Но из-за alpha test в шейдере
стоит kill → late-Z, нет записи LRZ (Adreno), нет FPK / late ZS (Mali), 2 vec4 варьинга (uv, мир) + 1 мёртвый (копия clip) на вершину.

G-buffer FS после `spirv-opt -O` (то, что видит драйвер): Base_PBR_ST_DEF 168 инструкций (3 tex, 1 IsNan+Select, 0 bitcast),
TR_Layer 195 (4 tex), Base_Tile 262 (6 tex), GrassFlowers 172, Characters_PBR_Base 299 (5 tex). Эмуляции транслятора в них почти нет
(1–2 NaN-защиты координат выборки — это семантика Maxwell); «чистый GLSL по смыслу» дал бы те же FMA / выборки. Точная перепись без
выигрыша не нужна, fp16 точным не будет → **G-buffer FS не переписывал** (решение по цифрам; см. §6 о том, что даёт им NR-33B).

## 2. Что сделано

| Часть | Где | Суть |
|---|---|---|
| Ядро (чистый C) | `dm/gfx/nr/nr_mat.{h,c}` | `nr_mat_scan` (kill / demote, встроенные выходы FS: глубина / маска / stencil, побочные эффекты: image / storage / atomics, маски Location-входов / выходов), пустая FS (32 слова, `spirv-val` OK), разбор / вычисление gate-записей, сравнения SPIR-V на сырых словах (FOrd / FUnord / NaN / ±0 / знаковые), `nr_mat_alpha_pass` (как `xbg_spirv_alpha.cpp`: упорядоченные сравнения), `nr_mat_prune` (удаление `OpStore` в Location-выходы, которые FS не читает; отказ при чтении / копировании / передаче выхода, struct / матрица / 64 бит) |
| Gate-записи (setup) | `tools/dos2_nr_mat.py`, `setup_dos2.py` стадия 8 `step_matpack` | символический обход каждой depth-FS игрока: входы / текстуры / прочие uniform — неизвестны, слово uniform, которое только сравнивают с константой, — символ. «Gate-путь»: OpReturn, ветвления только по таким сравнениям, без kill; если `.w` цвета 0 там константа — пишется. Любые входы, удовлетворяющие сравнениям пути, идут ровно этим путём ⇒ доказательство для всех входов. Плюс проверка на `spv_interp.py` (24 сидированных прогона на запись, слова gate берутся из констант сравнений, соседей, 0, 1, NaN): когда путь выполняется, kill нет и `.w` = записанному. Пак `<shaders>/native-nr33b.xgspack`, ключи `dg.<FS key>`, формат XGM1 (в докстринге), хэш FNV-1a 64 SPIR-V бандла |
| Хост | `dm/gfx/vulkan/engine/xbg_nr_mat.inc` (namespace `nrmat`), 9 строк в `xbg_game.cpp` | `note` в `load_shader`; `pre` в `draw()` сразу после pre-pass: подмена `state.shader[4]` на близнеца `<key>#dz` (пустая FS, те же привязки / c1 / raster-факты) на один draw, RAII `Guard` возвращает шейдер игры (как `GsPassthru`); `vs_choose` в сборке пайплайна (после retype): VS `<key>#dp<keep>`; `warm_module` в `warm::load_module` (записи прогрева с `#dz` / `#dp` строятся так же); сводка `[nr] mat` раз в 10 с |
| Политика | `xbg_vendor_policy.h` | `xbg_nr_mat_default` = 0 на всех семействах |
| Интерпретатор | `tools/spv_interp.py` | + вершинные шейдеры: встроенные входы `inputs[('bi', N)]`, блок `gl_PerVertex` как `out[('var', id)]` (FS-поведение прежнее) |
| Доказательство VS | `tools/dos2_nr_mat_proof.py` | C-обрезка (через CLI `nr_mat_test.exe`) на **каждой** паре (VS, маска входов FS) всех DEP / DEF программ пака + маска 0 (пустая FS): `spirv-val` + интерпретатор `gl_Position` и оставленные выходы бит в бит |

### Когда близнец точен (без допуска)
Все условия сразу: (a) у прохода нет цветных вложений, нет tess / GS, нет rasterizer discard, нет alpha-to-coverage; (b) у FS нет
встроенных выходов (глубина / sample mask / stencil) и побочных эффектов; (c) ни один фрагмент не может быть убит: kill-инструкций нет и
alpha test выключен **или** не применяется к этой FS (у неё нет `.w` цвета 0 — так рисует и транслятор); либо uniform этого draw
удовлетворяют gate-пути записи (kill нет; при alpha test — `func(w, ref)` истинно). Тогда глубина / stencil / occlusion — от растеризации,
выходы FS по Vulkan отбрасываются. Слово uniform читается как у `GsPassthru` (`state.ub[nvn_stage[4]][bank-3]`); не привязан / вне
диапазона / под ожидающим writeback → путь не выполнен → рисуется шейдер игры.
VS: удаляются только записи в выходы, которых FS не объявляет входами; позиционная математика не трогается (`Invariant` остаётся).
Пиксели на GPU не сравнивались (на скалярной семантике — бит в бит; драйвер компилирует оба модуля сам).

## 3. Переключатели
- `DM_NR_MAT` **0** (по умолчанию) — ничего. **1** — близнец + обрезка VS. **2** — проверка: все решения считаются («would swap»),
  рисуются шейдеры игры, VS не трогается. **3** — яд: близнец ставится и там, где gate НЕ выполнен (растворение Base_PBR-объектов у камеры
  пропадает в pre-pass / тенях — видно глазами, что решает именно gate).
- `DM_NR_MAT_VS` = 1 (по умолчанию при 1 / 3), 0 — только близнец FS (для A/B вклада VS).
- Строки: `[nr] mat DM_NR_MAT=…` (старт), `[nr] mat <shaders>/native-nr33b.xgspack: N gate records` (или `no …: only shaders without a
  kill are swapped`), первые 4 `gate record, K paths`, первые 4 `<key> -> <key>#dz`, первые 4 `vertex <key>#dp<keep>: R stores … removed`;
  сводка раз в 10 с: `depth-only material draws S: swapped N (no kill + alpha off, gate + alpha off, gate + alpha passes w, alpha test not
  applied), kept: gate fails, alpha fails w, no record, kill without gate, alpha to coverage, already empty; twins; vertex variants (refused,
  retyped kept, pipelines); red lines`. Красные `【红】[nr] mat …` (первые 16): запись для другого SPIR-V / битая, пак повреждён.

## 4. Проверки (ПК)
- `tests/nr_mat_test.c` (clang 20, Windows): **PASS 47 / 47** — scan (пустая FS, VS 3 выхода, FS с kill / FragDepth / imageStore+SSBO / чистая),
  prune (keep uv → 2 записи удалены, интерфейс тот же; keep 0 → 3; ничего / всё → -4; VS, читающий свои выходы → -2; битый модуль → -1),
  все 22 сравнения на NaN / ±0 / inf / знаковых, alpha test (NEVER / ALWAYS / NaN / равенства), gate-запись формы Base_PBR (c3.x = 0 / 2 /
  1 / NaN / не привязан), 7 испорченных записей отвергнуты, FNV. Тестовые SPIR-V — `nr_mat_test_spv.h` из `nr_mat_test.glsl.txt`
  (glslc NDK r29 -O0). Добавлен в прогон хост-тестов (`run_tests.sh` копия: строка `nr_mat_test`).
- `dos2_nr_mat.py` на паке игрока (`--programs`, все 2001 DEP-программы, 436 разных SPIR-V, 8 с): **84 записи** (45 семьи fade + 39 kill
  по uniform), **2016 прогонов интерпретатора, gate выполнен в 1362, 0 ошибок**; 27 kill-FS — предел путей (512), без записи.
- `dos2_nr_mat_proof.py` (вся C-обрезка по паку игрока, `--n 3`): **2094 пары, все обрезаны, все `spirv-val`, 6282 / 6282 прогонов
  бит в бит** (gl_Position + оставленные выходы). `work/native-nr33b/proof.txt`, `proof.tsv`.
- `source_structure_test.py`: PASS, новый блок `NR-33B DM_NR_MAT wiring` (ядро без Vulkan / getenv; хук после pre-pass и до стадий /
  memo, только DOS2; у близнеца нет alpha-варианта; VS после retype; прогрев строит `#dz` / `#dp` до alpha-тега; `note` с сырым SPIR-V;
  Guard возвращает шейдер; retyped VS не трогается; слово gate под writeback не читается; стадия 8 строит пак; формат / ключ записи
  совпадают в Python / C / хосте; умолчание 0).
- NDK 29 `-fsyntax-only` `xbg_game.cpp` (все 4 игры) — 0 ошибок, новых предупреждений нет; `nr_mat.c` — 0 диагностик.
- Полный прогон хост-тестов (копия `run_tests.sh` + `nr_mat_test`): **pass=69 fail=2** — `t_cpuprof_rt` и `t_nr24_arb` (WSL, тайминги потоков
  `dm/host/hle_thread.c`, «T11 threads did not finish» под нагрузкой параллельной сборки APK); этих файлов ветка не трогает.
- Сборка: `tools/build_apk.ps1` (свой `work/build-android`, драйверы из `wt-nrall/work/drivers`, gradle offline) →
  `work/DOS2-port-debug.apk` 55 318 717 Б, SHA-256 `f150b99e56683b57267311db79d4ac7e83c174bac15eec5e706c72c7879bbfcc`; в `libdm.so` есть
  строки `DM_NR_MAT=%d` и `native-nr33b.xgspack`.

## 5. Цифры на шейдер / семью

| семья | вариантов | было → стало (на вершину / фрагмент) |
|---|---|---|
| **DEP FS fade** (Base_PBR / Tile / RedChannelOnly / SK `_DEP_NX`) | 45 программ, 1 SPIR-V | FS: 20 исп. инструкций + alpha-kill на каждый фрагмент → **нет FS** (пустая), early-Z + LRZ-write / FPK; при fade (c3.x == 1) — как было (100 инстр., 1 tex) |
| **DEP VS под пустой FS** | 702 разных VS (все DEP) | после `spirv-opt -O` 424.7 → **320.2 (−24.6 %)**, Location-выходы **4.44 → 0** vec4 на вершину |
| DEP VS под kill-FS (выходы, что FS не читает) | 606 пар | 424.2 → 407.9 (−3.8 %), 4.48 → 3.31 vec4 |
| DEF VS (G-buffer) | 786 пар | 484.0 → 471.4 (−2.6 %), 5.21 → 4.13 vec4 (мёртвая копия clip-позиции у каждой программы) |
| DEF FS / kill-DEP FS | 578 / 434 | не переписаны (§1: после оптимизатора эмуляции нет; kill-DEP уже без цветовых выходов) |

## 6. Ожидаемый выигрыш (Fort Joy, тяжёлые окна: G-buffer 1.8, тени 1.5, pre-pass 0.9 мс на кадр выборки)
Доля семьи fade: тени ≈ 72 % вершин прохода (Base_PBR 942k + Tile 300k + Red 198k + SK 22k из 2.04 M), pre-pass ≈ 68 %.
- **Mi 14 / Adreno 750 [оценка]:** binning уже позиционный (драйвер), так что VS выигрывает только в проходе рендера; главное — kill
  уходит у 2/3 depth-геометрии (late-Z → early-Z, LRZ снова пишется; PREPASS.md: pre-pass с kill в меню 2.8–3.9 мс против 1.43 мс
  G-buffer той же геометрии). Модель «половина прохода — геометрия, половина — фрагменты»: тени −0.25…0.45, pre-pass −0.12…0.25,
  G-buffer −0.02…0.05 ⇒ **−0.4…0.7 мс GPU** из 17–21 в тяжёлых окнах. CPU: +1 чтение uniform и пара сравнений на depth-draw (~600 / кадр,
  < 0.05 мс), первый draw каждой пары (материал, проход) — сборка пайплайна близнеца (прогрев её запоминает).
- **Mali (Valhall IDVS) [оценка]:** варьинги пишутся в память: семья fade в depth-проходах ≈ 1.6 M вершин × 4.44 vec4 × 16 Б ≈ 110 МБ
  записи + чтение на кадр → 0; плюс FPK / early ZS вместо discard. **−1.5…4 мс** на классе G610 / G710 в тех же окнах; G-buffer / kill-DEP
  обрезка ещё ≈ 20 МБ / кадр (−0.3…1 мс).
- **PowerVR (BXM) [оценка]:** все варьинги пост-клиппинга идут в parameter buffer — тот же порядок, что Mali, плюс меньше риска переполнения
  PB в тенях (2.9 M вершин в пике); discard на PowerVR особенно дорог (HSR выключается для объекта) → вероятно верх диапазона Mali.
- **Xclipse / RDNA:** меньше экспорта параметров и PS-волн в depth-проходах, −0.2…0.6 мс.
- **Слабый CPU:** ≈ 0 (одна проверка на draw).

## 7. Риски и чего нет
- Пиксели на GPU не сравнивались; доказательство — семантика SPIR-V (символический обход + интерпретатор) и правила Vulkan про выходы без
  вложений. Загрузочные переписывания хоста (invariant / early tests / quad …) делаются на итоговых словах, gate доказан на сыром SPIR-V
  бандла: они не меняют поток управления и сравнения uniform.
- Если драйвер сам уже выкидывал мёртвые варьинги / FS — выигрыш только от снятого kill (Adreno). Если uniform c3 этого draw пишется GPU
  (writeback в ожидании) — шейдер игры (не ошибка, просто без выигрыша).
- Новые ключи пайплайнов (`#dz` / `#dp`): у игроков со старым `vk-pipeline-list.bin` первый запуск с `DM_NR_MAT=1` собирает эти
  пайплайны в кадре (по разу), дальше прогрев их знает; при `DM_NR_MAT=0` записи с `#dz` / `#dp` не строятся (как alpha-записи без alpha).
- NB-13b / pipe memo / NR-09 видят близнеца как другой шейдер (ключ — указатель) — свои записи, лишняя сборка на пару.
- Не сделано: G-buffer FS (обосновано §1), fp16, kill-DEP FS, 27 kill-FS с пределом путей, сортировка depth-draw (опак первыми), APK-проверка
  на телефоне. `tools/spv_interp.py` изменён → хэш инструментов NR-04 / NB-18 паков другой: setup один раз пересоберёт их (байты те же).
- Общие файлы: `xbg_game.cpp` (+17 строк: поля Shader, 2 объявления, 6 хуков), `setup_dos2.py` (блоки `NR-33B`), `source_structure_test.py`
  (функция `nr33b_check`), `xbg_vendor_policy.h` (+1 функция).

## 8. Файлы для телефона и прогоны, которые это подтвердят
Пак: `dos2-port/work/native-nr33b/native-nr33b.xgspack` (12 248 Б, 84 записи,
SHA-256 `82f24ac8edef87dd01ec4a6056ec2e332f4fbc7c059141fb38e3f5f6fe0e58f5`) → `dm/shaders/` на телефоне (или стадия 8 setup). Без пака
работает часть «без kill + alpha off» и обрезка VS; семья fade (alpha test) — только с паком.
Перед любым прогоном: бэкап `files/save`, экран разблокирован; после — force-stop.
1. **Проверка:** `DM_NR_MAT=2`, Fort Joy (сейв форта) 60 с от загрузки. Ждём: `[nr] mat …native-nr33b.xgspack: 84 gate records`, `gate record`
   строки, сводка `would swap N` с `gate + alpha passes w` ≈ 2/3 `depth-only material draws`, `gate fails` — единицы (растворение у камеры),
   `red lines 0`.
2. **A/B GPU:** база ↔ `DM_NR_MAT=1` (ABBA, `DM_GFX_PASS_PROF=300,180,0`, тот же ракурс форта): `python tools/dos2_gpu_breakdown.py <log> --gpass`
   — проходы 1536x1536 fmt 124 и 2160x972 fmt 129 (ожидание −0.25…0.45 / −0.12…0.25 мс), G-buffer ≈ без изменений; `shader-heat.tsv`
   покажет ключи `…#dz`. Скриншоты = база (тени, листва, фейд у камеры). Затем `DM_NR_MAT=1 DM_NR_MAT_VS=0` — вклад VS.
3. **Яд (глазами):** `DM_NR_MAT=3`, подвести камеру к дому / стене, которые растворяются: в тенях / pre-pass дыры растворения пропадают
   (G-buffer рисует объект сквозь EQUAL) — доказательство, что gate работает; `POISONED N` в сводке.
4. **Слабые (Mali / PowerVR тестеры):** то же, что 2; из лога — сводка `[nr] mat …` и `[quality] GPU frame time`.
После 1–2 без красных строк кандидат в умолчание — `DM_NR_MAT=1` на всех семействах (`xbg_nr_mat_default`).

## 9. NRVS (ветка `dos2-nrvs`, база `dos2-nr-all` 75e2179): обрезка VS — общая с NR-32, умолчания

**Что поменялось в NR-33B.** Обрезку VS делает теперь только NR-32 (`vsout_module`, `xbg_spirv_vsout`; подробно — NR-32.md §11): у близнеца
`#dz` `vo_read = vo_decl = 0`, его пайплайн получает VS `<vs>[#vi…]#vo0000000000000000_0000000000000000` (все записи в Location-выходы сняты,
выходы без ссылок ушли из интерфейса; `gl_Position` не тронут). Убраны `dp_module`, `vs_choose`, `#dp<keep>`, `Shader::nm_dp / nm_in /
nm_out`; `note` разбирает только FS. Часть «пустой FS в depth-only проходе» (близнец, gate, сводка, яд) — без изменений.
- `DM_NR_MAT_VS=1` (умолчание при 1 / 3) → `xlq_twin`: пайплайны близнеца триммируются в режиме 1 при любом `DM_NR_XLQ`; `0` — NR-33B
  трим не добавляет (при `DM_NR_XLQ` 1 / 3 близнец триммируется всё равно — им владеет NR-32). Пайплайны без близнеца (G-buffer DEF,
  kill-DEP) NR-33B больше не триммирует: это `DM_NR_XLQ` (Mali / PowerVR / Maleoon по умолчанию 1; на Adreno / Xclipse компилятор линкует
  стадии сам — потерь нет).
- Лучше прежнего: (а) трим составляется с retype `#vi` / scaled-конверсией (раньше «retyped kept» — без обрезки); (б) одна схема ключей и
  один кеш модулей — двойных сборок VS при обоих переключателях нет; (в) каждая обрезка проверяется `xbg_spirv_vsout_check` перед модулем.
- Не хуже на маске 0: `tests/nrvs_equiv_test.cpp` — на **всех 6 130 VS пака** у трима NR-32 записей в Location-выходы остаётся столько же,
  сколько у `nr_mat_prune(keep 0)`: **0 из 147 985** (до правки литералов в NR-32 оставалось 21 751 в 3 017 VS — тест это и поймал);
  для масок FS из `nr_mat_test` — не больше, чем у `nr_mat_prune`. Доказательство трима на маске 0 по паку (6 132 VS × пустой FS) вошло в
  прогон NR-32.md §11: 0 отказов проверки, spirv-val 0, интерпретатор 0 расхождений.
- Сводка: `… twins N; twin pipelines with the NR-32 vertex trim M (NVN PERF xlq); red lines R`; в `NVN PERF xlq` — `twin=1 twin_pipelines=M`.
- Деструктор `Shader` после слияния проверен: `nm_dp` больше нет; близнец удаляется; `vo`-модуль освобождается, если это не базовый и не
  retype-модуль (тот освобождается своим циклом) — двойного освобождения нет.

**Умолчание `DM_NR_MAT` — остаётся 0 на всех семействах** (`xbg_nr_mat_default`). Почему риск реален, хоть доказательства шейдерные есть:
1. Ни одного прогона ни на одном GPU, даже `=2` (счёт) на Mi 14. Точность близнеца опирается на состояние draw, которого не видит ни один
   хост-тест: набор вложений прохода (`state.rt`), слово gate-uniform этого draw (`state.ub`, отложенные writeback), включённый alpha test.
   Ошибка там — дыры в тенях / pre-pass, а не красная строка.
2. Близнец — новый пайплайн на каждую пару (материал, depth-проход): у игрока без записей прогрева первая сессия собирает их в кадре
   (хитчи); прогрев их запоминает только со второй.
3. Семья fade (64 % вершин depth-проходов, главный выигрыш) работает только с паком `native-nr33b.xgspack` (setup стадия 8) — у игроков
   с прошлым setup его нет до повторного setup; без пака остаётся «без kill + alpha off» (почти ничего в DOS2).
4. Оценки — модели, не замеры: Mi 14 −0.4…0.7 мс GPU (тени −0.25…0.45, pre-pass −0.12…0.25) из 17–21 мс окон Форта; Mali (Valhall IDVS)
   −1.5…4 мс, PowerVR — верх диапазона. NR-30 (`DM_DOS2_PREPASS`, тайловые по умолчанию) не пересекается: он пропускает только
   пустые opaque DEP (1.4 % вершин), семья fade с alpha test остаётся в pre-pass — выигрыш NR-33B на Mali / PowerVR от NR-30 не уменьшается.

**Предложение по семействам (включать после прогонов ниже):**
| семейство | после прогона 1–2 на Mi 14 без красных строк и с картинкой = база | почему |
|---|---|---|
| Mali, PowerVR, Maleoon | **1** | выигрыш — varyings в память (IDVS / parameter buffer) + discard (FPK / HSR); точность от вендора не зависит (правила Vulkan + семантика SPIR-V) |
| Xclipse (RDNA) | **1** | −0.2…0.6 мс [оценка]: меньше экспорта параметров и PS-волн |
| Adreno | **1 только если** A/B (прогон 2) даёт ≥ 0.2 мс на тенях + pre-pass, иначе 0 | Turnip и так линкует varyings; выигрыш — только снятый kill (LRZ / early-Z); цена — пайплайны близнецов |

**Сборка / проверки:** полный прогон хост-тестов дерева (копия `run_tests.sh` + `nr_mat_test`, `vsout_test`, `vsout_test_asan`,
`nrvs_equiv_test`): итог ниже.

**Прогоны на телефоне** (Mi 14 Ultra, сейв Форт-Джоя, 60 с от загрузки уровня; перед — бэкап `files/save`, экран разблокирован; после —
force-stop; пак `native-nr33b.xgspack` в `dm/shaders/`):
1. `DM_NR_MAT=2` — `[nr] mat …: 84 gate records`; сводка `would swap N` ≈ 2/3 `depth-only material draws`, `red lines 0`; нет `NVN PERF xlq`
   (`twin=0`: режим 2 не триммирует).
2. ABBA база ↔ `DM_NR_MAT=1` (`DM_GFX_PASS_PROF=300,180,0`, тот же ракурс): `python tools/dos2_gpu_breakdown.py <log> --gpass` — проходы
   1536x1536 fmt 124 и 2160x972 fmt 129; сводка `swapped N`, `twin pipelines with the NR-32 vertex trim M` (M > 0), `NVN PERF xlq mode=0
   twin=1 twin_pipelines=M check_bad=0`; скриншоты = база (тени, листва, фейд у камеры).
3. `DM_NR_MAT=1 DM_NR_MAT_VS=0` — `twin=0`, нет `NVN PERF xlq`: вклад одного FS-близнеца.
4. `DM_NR_MAT=1 DM_GFX_SIM_GPU=family-mali` — путь Mali (`DM_NR_XLQ=1` по семейству + близнецы): `check_bad=0`, картинка = база.
5. (глазами) `DM_NR_MAT=3` — дыры растворения в тенях / pre-pass у камеры, `POISONED N`.

**Итог проверок и сборки (NRVS):** хост-тесты дерева (копия `run_tests.sh` из `nr/merged` + `nr_mat_test`, `vsout_test`, `vsout_test_asan`, `nrvs_equiv_test` по паку): **74 PASS, 0 FAIL** (`nr_tex_start_test` в полном прогоне один раз FAIL «decided late» — тайминг под нагрузкой, повтор отдельно PASS; ветка его не трогает). `source_structure_test.py` PASS. `tools/build_apk.ps1` (свой `work/build-android`, драйверы из `wt-nrall/work/drivers`, gradle offline): `libdm.so` собран, APK подписан — `wt-nrvs/work/DOS2-port-debug.apk` (= `wt-nrvs/dist/DOS2-Mobile-v0.1.1-arm64.apk`), 55 552 189 Б, SHA-256 `0558a0517838c31a4d6a52a625fc89d91c30840d6dad34ecdd882bcd441af2ea`. Новых файлов для телефона нет (пак NR-33B — прежний, §8).
