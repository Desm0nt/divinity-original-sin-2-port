# NR-05 fix — сторона движка NR-05 / NR-22 не включалась ни разу (гонка инициализации), 17.11.2026, ветка `dos2-nr05fix`

База `dos2-nr-all` (795a5fb). Телефон не трогался.

## Симптом (Mi 14 Ultra, zero-copy, палуба, загрузка из меню; `src/work/logs/nr16_tex1` / `nr16_ts1` / `nr16_ts2`, база `nr16_base`)
- Фронт: `[nr] textures by events (DM_NR_TEX=1): events 2406 …, prepared 2406 (174.7 MB, worker 176.7 ms), keys 0, taken 0 (0.0 MB) …
  blobs held … (peak 107.0 of 128 MB); texstream: prepared images 0 deswizzled + 0 ASTC, … taken 0` — рабочий поток готовит всё,
  транслятор не берёт ничего.
- Ни в одном из трёх прогонов **нет ни одной строки движка `NVN nr-tex …` / `NVN nr-texstream …`** (ни стартовой `NVN nr-tex DM_NR_TEX=1:`,
  ни отчёта раз в 10 окон PERF) — т.е. `nrtex::mode == 0` весь прогон.
- Длинные кадры (`[长帧] … 上传贴图`, сумма за прогон): base 1107 первых + 852 повторных, 340.6 МБ / **160.7 мс**; tex1 976 + 866,
  349.8 МБ / 142.5 мс; ts1 321.6 МБ / 145.0 мс; ts2 328.5 МБ / 134.0 мс — разницы нет (вход в уровень 211 + 188 / 91.6 МБ: 70.6 мс base,
  61.2 мс ts1).

## Причина (по логам, не догадка)
- `nrtex::init()` движка вызывается при создании устройства Vulkan (`xbg_game.cpp`, сразу после строки `NVN texture checks through the
  dirty tracker=…`) на потоке Vulkan; он читает `g_dm_nr_tex` **один раз**.
- `g_dm_nr_tex` ставит фронт в `nrt_start` из `dm_dos2_nr_install` (`host_preload`, другой поток), параллельно с созданием устройства.
- Во всех трёх прогонах движок успевал раньше на ~190 мс:

  | прогон | `dirty tracker=1` (поток Vulkan, `nrtex::init`) | `[nr] DM_NR_TEX=1` (`nrt_start`) |
  |---|---|---|
  | nr16_tex1 | 22:57:02.807 (tid 30322) | 22:57:03.002 (tid 30319) |
  | nr16_ts2 | 22:59:32.192 (31186) | 22:59:32.389 (31182) |
  | nr16_ts1 | 23:03:56.218 (32226) | 23:03:56.403 (32223) |

  → `init` прочитал 0, `nrtex::mode = 0` навсегда, `nrts::init` (NR-22) тоже 0. Отсюда всё остальное: `on_create` не зовётся →
  `dm_nr_tex_find` / `take` / `verdict` движок не вызывает (taken 0), вердиктов форматов нет → рабочий NR-22 ни разу не спрашивает
  движок (`prepared images 0`), blob-ы только истекают (`expired 1144`), бюджет держится зря (пик 107 МБ), 177 мс CPU рабочего — впустую.
- Остальные гипотезы проверены и **не** причина: ключ (серийник) у события и у транслятора — одно поле `tex_t::serial` одного NVN-объекта
  (тап `nvnTextureInitialize` и `get_image_slow`); чтение хранилища — обычные загрузки (рабочий их сделал 2406 раз), `process_vm_readv`
  (баг NR-01) в NR-05 не используется; путь claim с NR-06/08/09R/NB не обходится — до него просто не доходило из-за `mode 0`.
  Тайминг «blob готов позже первой заливки» был бы виден как `no blob: pending` на стороне движка — этой строки не было, т.к. движок
  выключен. Других слабых глобалов фронта, читаемых при создании устройства, в движке нет (NR-06 / NR-08 читают env сами).
- `keys` в сводке фронта — это ключи **ASTC-кэша**, посчитанные рабочим (`nr_tex.c` `st.keys`, только при `want_key`); на Adreno
  кэш не используется (`NVN astc cache: not used (the device samples BC …)`), так что `keys 0` там верно и после починки. Ожидать
  `keys ≈ events` можно только с ASTC-кэшем (Mali / PowerVR / Xclipse или `DM_GFX_SIM_GPU=mali`).

## Что сделано (только при `DM_NR_TEX` ≠ 0; при 0 — поведение байт в байт, ни одной новой строки лога)
1. **Фронт `dm/host/hle_dos2_nr.c`:** `g_dm_nr_tex_set` — 1, когда `dm_dos2_nr_install` вернулась (любой путь, через
   `__attribute__((cleanup))` первым оператором функции; `__ATOMIC_RELEASE`, после записи `g_dm_nr_tex` / `g_dm_nr_texstream(_cap)`).
   Счётчик `engine asks N` (вызовы `dm_nr_tex_find` = `on_create` каждого нового образа) в конце строки `[nr] textures by events …` и одна
   красная строка `【红】[nr] textures by events: N events but the engine never asked for one …`, если событий ≥ 64, а движок не спросил
   ни разу (эта же поломка впредь видна одной строкой).
2. **Ядро `dm/gfx/nr/nr_tex.h`:** `nrt_engine_decide(host_mode, host_set, &mode)` (static inline, чистый C): нет фронта → 0; флаг
   опубликован и 0 → «ещё не решено»; иначе режим 0..3 (вне диапазона — 0). Старый фронт без флага → значение как есть (старое поведение).
3. **Движок `xbg_game.cpp namespace nrtex`:** `init()` (создание устройства) решает через `nrt_engine_decide`; если установка фронта ещё
   идёт — `pending`. `poll()` — на потоке трансляции, только пока `pending`: в начале `get_image_slow` (до поиска в ASTC-кэше и
   `on_create` нового образа) и в `perf_frame` (до `nrts::frame`, так что NR-22 открывается тем же present). Установка фронта
   заканчивается до любого кода гостя, поэтому первая же материальная текстура гостя видит решение. `start(m)` — единственное место,
   где ставится `mode` (+ строка `NVN nr-tex DM_NR_TEX=N: … (started after the front end's install, which came after the device
   init)`), затем `nrts::init()` (NR-22) и, если ASTC-кэш уже поднялся раньше, `dm_nr_tex_want_key(1)` (его `init` тогда ничего не
   попросил). При `DM_NR_TEX=0` первый `poll` после установки читает 0 и снимает `pending` — больше ничего.

Сверки байтов уже есть и не менялись: `DM_NR_TEX=2` — blob ↔ staging старого пути побайтно (`blob compares / mismatches`), хэш события ↔
хранилище, ключ ↔ ключ; `DM_NR_TEXSTREAM=2` — `VkImageCreateInfo` / требования памяти / staging побайтно (`staged byte compares /
mismatches`). До этой починки они не исполнялись ни разу (та же гонка).

## Слабые устройства / Mali
Починка общая (порядок потоков от GPU не зависит). Zero-copy выкл. (Mali, политика): хранилище — обычная память, рабочий читает её
загрузками как и раньше. ASTC-кэш (NR-18, `astccache::init` ленивый, на первом поиске транслятора): поиск идёт после `poll`, так что
`want_key` просит его `init`; если кэш поднялся раньше старта — `start()` просит сам. На медленных CPU разрыв «создание устройства →
установка фронта» скорее больше, т.е. без этой починки NR-05 там тоже не включался бы никогда.

## Тесты (ПК)
- `tests/nr_tex_start_test.cpp` (новый; clang++ 21): `nrt_engine_decide` на трёх потоках (создание устройства / установка фронта / транслятор,
  который опрашивает до первой текстуры гостя): порядок 17.11 (устройство на 19 мс раньше) — старое правило читает 0 (воспроизводит
  поломку), новое стартует поздно с режимом фронта (1..3); установка раньше — решено в `init` без опроса; `DM_NR_TEX=0` поздно — решено 0;
  600 случайных интерлейвингов (167 поздних) — всегда режим фронта к первой текстуре; без фронта / вне диапазона — 0; фронт без флага —
  как есть. **PASS, 626 проверок.**
- `source_structure_test.py`: новый блок «NR-05 fix» (флаг без инициализатора, ставится только в `nr_install_done`, `cleanup` — первый
  оператор установки; `nrtex::mode` ставится только в `start`; `init` / `poll` только через `nrt_engine_decide`, `pending` только там;
  `poll` ровно два раза — в `get_image_slow` до `astccache::lookup` / `on_create` и в `perf_frame` до `nrts::frame`); NR-05 / NR-22 и
  остальные — PASS.
- Полный прогон хост-тестов (копия `run_tests.sh` на этот worktree + `nr_tex_start_test`) и сборка — см. «Сборка».

## Прогоны на телефоне, которые это подтвердят (главный агент; бэкап `files/save`, keyguard, после — force-stop)
Сцена как в `nr16_*`: меню → загрузка сейва на палубе, 60 с после загрузки уровня. Все четыре — APK этой ветки.
1. **`DM_NR_TEX=2`** (сверка): `NVN nr-tex DM_NR_TEX=2: … (started after the front end's install …)` один раз; раз в 10 окон PERF
   `NVN nr-tex verify: first uploads N (≈ число первых заливок материальных), storage changed since the create event A (≈ 0), …
   blob compares F (> 0), mismatches 0`; `[nr] textures by events … engine asks K` (K > 0, ≈ число новых образов); красной строки
   `engine never asked` нет.
2. **`DM_NR_TEX=1`**: `NVN nr-tex DM_NR_TEX=1: images static S …; first uploads from the worker's blob B (… MB, … ms on this thread),
   no blob: …` (B > 0; `pending` — сколько рабочий не успел); во фронте `taken > 0 (… MB)`, `expired` ≪ 1144; `engine asks` > 0;
   `[长帧] … 上传贴图` за прогон < base (160.7 мс в nr16_base; вход в уровень 211 + 188 / 91.6 МБ = 70.6 мс).
3. **`DM_NR_TEX=1 DM_NR_TEXSTREAM=2`**: `NVN nr-texstream DM_NR_TEXSTREAM=2: …` и `NVN nr-texstream verify: claims C (> 0), create info / kind
   mismatches 0, memory requirement mismatches 0, staged byte compares M (> 0), mismatches 0`; во фронте `texstream: prepared images P
   deswizzled (> 0) …`.
4. **`DM_NR_TEXSTREAM=1`**: `worker made A deswizzled …; translator: taken …, adopted C (> 0, доли мс), staging used D …; images made here H`
   ≪ строки `NVN nr-texstream off … images made here` прогона 2; `[长帧]` первых заливок ещё меньше, чем в прогоне 2.
- (по желанию) `DM_GFX_SIM_GPU=mali DM_NR_TEX=1`: только здесь `keys ≈` событий кэшируемых текстур и `ASTC keys from the worker N` у движка.

## Риски
- Образ, который дошёл бы до `get_image_slow` раньше конца `host_preload` (не проверял, бывает ли такое у заставки), просто идёт
  старым путём (`pending`); материальные текстуры гостя создаются кодом гостя, т.е. всегда после решения.
- Поведение NR-05 / NR-22 при `=1` на телефоне по-прежнему **не проверено** (раньше не исполнялось вовсе): прогоны 1 и 3 — первые
  настоящие сверки этих путей; включать `=1` по умолчанию только после `mismatches 0`.
