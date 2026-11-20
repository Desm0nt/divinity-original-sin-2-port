# NR-04 / NR-07 fix — две красные строки прогона `nr4_b2` (имя LinearizeDepth и «дыра подсказки» сплавленных post-конвейеров)

Ветка `dos2-nr0407` (база `dos2-nr-all` 617f4af). Телефон не трогался.

Прогон B2 (Mi 14 Ultra, `DM_NR=2 DM_NR_DUMP=1 DM_NR_KEY=2 DM_NB_FOREIGN=1 DM_NR_TEX=2 DM_NR_PIPE=2 DM_NR_SH=2`, палуба, 60 с;
`src/work/logs/nr4_b2.dm.log`, дампы `nr4_b2-dumps/`): всё чисто, кроме двух красных строк.

## 1. NR-04: `sh name check` на `Orthographic/LinearizeDepth`

### Симптом
`【红】[nr] sh name check: engine shader 'rom:/Data/Shaders/NX/Orthographic/LinearizeDepth.bshd' (path ...) fragment key a2cca3720ade8cc2,
table entry LinearizeDepth (f23c09d198b55dc5): the table's name with another code (that code is not replaced; ours is untouched)`

### Причина
- `nr_sh_find_name` (dm/gfx/nr/nr_sh.c) сравнивал только базовое имя (всё после последнего `/`, `\` или `:`, без `.bshd`).
- У движка два разных шейдера с базовым именем LinearizeDepth: `Shaders/NX/LinearizeDepth.bshd` (фрагментный ключ `f23c09d198b55dc5`,
  он в таблице) и `Shaders/NX/Orthographic/LinearizeDepth.bshd` (`a2cca3720ade8cc2`). Доказательство: `work/shaders-jobs/programs.tsv`
  (обе строки, общий VS `06b17b62ec233b75`) и `nr4_b2-dumps/nr_shaders.tsv` (два объекта `0x...07` / `0x...08` с этими путями и ключами).
- Ортографический вариант — законно другой шейдер, а не «имя таблицы с чужим кодом». Других совпадений базовых имён таблицы в
  подпапках в programs.tsv нет (проверены все 10 записей).

### Что сделано
- `nr_sh_find_name` стал путевым: все записи таблицы — программы EngineShaders прямо в `Shaders/NX`, поэтому имя с папкой
  принадлежит таблице, только если эта папка — `Shaders/NX` (любой регистр, `/` или `\`, перед ней начало или разделитель; то, что до
  последнего `:` — пак / `rom:` — не смотрится). Голое имя (`FadeBlending`, `x.FadeBlending` после `x.`) — как раньше.
- `nr_sh_name_note` не менялся: для ортографического LinearizeDepth теперь `bn = bp = bk = -1` → `NR_SH_NAME_OTHER` (не наш, нечего
  сравнивать) — красной строки нет. Настоящие противоречия остаются красными: `Shaders/NX/LinearizeDepth.bshd` с ключом `a2cc..` —
  `KEY_DIFFERS`; наш ключ `f23c..` под ортографическим путём — `NAME_DIFFERS` (сообщается); обмен ключей двух записей — `SWAPPED`.

## 2. NR-07: `pipe check` — тёплые записи 1183 / 1188 «failed», «hint hole»

### Симптом
`【红】[nr] pipe check: a draw needs warm record 1188 (d52cf8500f1e95d3+F1A.91eb2d97a304709a, failed) but no program the game set up
named all its shaders (a hint hole: DM_NR_PIPE=3 would build it in the frame)` и то же для 1183 (`+F1.91eb2d97a304709a`).

### Причина (a) — запись действительно не строилась прогревом, на каждой загрузке
- `vk-pipeline-list.bin` (`tools/dos2_nr_pso.py`-ридер `warm_records`): 1241 запись, из них 4 со сплавленным post-шейдером:
  1183 и 1240 `[d52cf8500f1e95d3, F1.91eb2d97a304709a]`, 1188 и 1223 `[d52cf8500f1e95d3, F1A.91eb2d97a304709a]` (по две на
  ключ — разные create-info); остальные не-шестнадцатеричные — только `#at5_...` (alpha-test, строятся).
- Лог: `[nr] pipe (list done, ... 0.1 s after start): ... built ... 1237 in list order, failed 3` → потом `failed 4`;
  `NVN PERF ... warm_built=1237 warm_failed=4` весь прогон. 1241 − 1237 = 4 = ровно сплавленные записи.
- Механизм: `warm::build` → `load_module` → `bundle_words` читал каждый шейдер стадии через `xpack::read` (пак игры / свой файл).
  Сплавленного бандла там нет: `F1.` / `F1A.<host>` живут в `native-post.xgspack` в формате XGF1, а рабочий бандл получается в
  `nbpost::prepare` (разбор XGF1, сверка головы с переведённым хостом, расстановка extra-привязок на запасные слоты) — только на
  потоке рендера, при первом сплавленном draw. Поэтому на каждой загрузке все 4 записи `failed`, и draw строил конвейер в кадре
  (`created=1 create_ms=0.4` в 13:11:11 — Adreno 750 с тёплым кэшем драйвера; на холодном кэше / Mali дороже).
- Ключ записи при этом составлен правильно (тот же текст, что `register_module` даёт при runtime-загрузке, `fnv` id совпадает) — из
  списка её можно собрать, не хватало только чтения бандла.

### Причина (b) — «дыра подсказки»
- `nrp_key_parse("F1A.91eb...")` = 0 («не ключ»): у записи стадия без ключа → её никогда нельзя подсказать (`rec_hinted` = 0), а
  `need()` при `!t_hint` пишет красную строку. Программы игры называют только свои ключи; сплавленный шейдер — наш.

### Что сделано
- **`nbpost::compose`** (xbg_nb_post.inc): тело `prepare()` от чтения пака до расстановки слотов вынесено в функцию без общего
  состояния (локальный `Fx`); `prepare()` вызывает её так же, как раньше (поведение рендер-потока байт в байт то же).
  Открытие `native-post.xgspack` (`pack_get`) — под `std::call_once` (раньше флаг `popen` без замка: теперь читают и воркеры прогрева).
- **`nbpost::warm_bundle(key)`**: бандл сплавленного ключа для прогрева, из одного ключа (хост = часть после `.`), только тех, что
  нарисует текущая настройка (`DM_NB_POST` 1..3; `F1A*` только при `DM_NB_POST_AA=1` и своём `DM_NB_POST_LUMA` — иначе запись не строится,
  как alpha-test-запись при выключенном alpha test). Настройки читаются из окружения (`settings()`, тот же разбор, что `on()`), без
  состояния потока рендера. Первые 4 успеха — строка `[nb] post: warm-up record stage F1A.<host> composed (...)`, первые 8 отказов —
  `[nb] post: warm-up record stage ... not built: <причина>`.
- **`warm::bundle_words`** (xbg_game.cpp): стадия с именем сплавленного бандла (`nbpost::fused_name`) читается через `warm_bundle`, все
  прочие — `xpack::read`, как раньше. Дальше те же переписывания, что у runtime-загрузки (`gfx-shader-reuse=2` сверяет), модуль тот же.
- **`nrp_stage_key`** (nr_pipe.{h,c}): ключ стадии записи = `nrp_key_parse`, а для `F1.` / `F2.` / `F1A.` / `F1AS.` / `F1AR.` + 16 hex —
  ключ хоста. Сплавленный шейдер рисуется вместо фрагментной стадии хоста (EoCSight: VS `d52cf8500f1e95d3`, FS `91eb2d97a304709a`,
  `nr_shaders.tsv` строка 90), значит программа, которую игра ставит с хостом, и есть его подсказка. Подсказки (`hint()`) остаются
  простыми ключами игры. `xbg_nr_pipe.inc start()` берёт ключи записей через `nrp_stage_key` и пишет в строке старта
  `(%u stages are our fused post bundles: hinted by the program of the host shader they replace)`.
- Итог: `DM_NR_PIPE=0 / 2` (список по порядку) — сплавленные записи строятся при загрузке, draw берёт готовый (`take`) → `need()` не
  зовётся. `=1` — подсказаны вместе с программой EoCSight и строятся первыми. `=3` (lazy) — раньше никогда (ключ 0), теперь по подсказке.
  Красная строка `pipe check` для них больше не возникает; настоящая дыра (запись игры без подсказки) остаётся красной.
- `DM_NB_POST=0`: записи по-прежнему не строятся (отказ с причиной в логе, `warm_failed` их считает) — они и не нужны.

## Затронутые файлы
- `dm/gfx/nr/nr_sh.c`, `nr_sh.h` — путевое `nr_sh_find_name`.
- `dm/gfx/nr/nr_pipe.c`, `nr_pipe.h` — `nrp_stage_key`.
- `dm/gfx/vulkan/engine/xbg_nr_pipe.inc` — ключи записей через `nrp_stage_key`, счётчик в строке старта.
- `dm/gfx/vulkan/engine/xbg_nb_post.inc` — `settings`, `compose`, `warm_bundle`, `fused_name`, `pack_get` под `call_once`.
- `dm/gfx/vulkan/engine/xbg_game.cpp` — `bundle_words` читает сплавленные стадии через `warm_bundle`; объявления `nbpost`.
- Тесты: `tests/nr_sh_test.cpp`, `tests/nr_pipe_test.c`, `tests/source_structure_test.py` (NR-07 + NR-0407).

## Проверка
- `nr_sh_test`: `PASS: 316 checks, 0 failures` (+15: ортографический путь → -1 и `NAME_OTHER`, `KEY_DIFFERS` под путём таблицы,
  `NAME_DIFFERS` нашего ключа в чужой папке, `AGREE` по `rom:/Data/Shaders/NX/...`, `\`-пути, любой регистр, `Shaders/NXOrtho`, `MyShaders/NX`).
- `nr_pipe_test`: `all passed` (+ `nrp_stage_key`: F1 / F1A / F2 / F1AS / F1AR (+`#at`), отказы F3 / 3 буквы / строчные / 15 цифр /
  длиннее; запись `(d52c.., F1A.91eb..)` в lazy-планировщике подсказывается программой EoCSight и строится, со старым разбором — никогда).
- `source_structure_test.py`: 33 PASS (NR-07 дополнен: `nrp_stage_key` в `start()`, `hint()` на `nrp_key_parse`, `bundle_words` →
  `warm_bundle`, `pack_get` под `call_once`, `prepare` и `warm_bundle` через один `compose`, `warm_bundle` без состояния потока рендера).
- Полный прогон хост-тестов как на `dos2-nr-all` (+ `nr_peek_test`): `pass=66 fail=0`, `dos2_nr_check` 0 problems.
- NDK 29 `-fsyntax-only` `xbg_game.cpp` (с `.inc`, флаги `dm_vulkan_engine`), `nr_pipe.c`, `nr_sh.c` — без новых предупреждений.

## Сборка
`tools/build_apk.ps1` в этой ветке (свой `work/build-android`, драйверы скопированы из `wt-nrall/work/drivers`, gradle offline):
`work/DOS2-port-debug.apk`, 54 896 758 байт, SHA-256 `50afd75c8b7d449cc161409c7ae2308b27bd366b76e1157db14f42f9ef2b5ccb`;
в `libdm.so` есть строки `warm-up record stage` и `stages are our fused post bundles`.

## Прогон на телефоне, который это подтвердит
Бэкап `files/save`; APK этой ветки; env как B2: `DM_NR=2 DM_NR_DUMP=1 DM_NR_KEY=2 DM_NB_FOREIGN=1 DM_NR_TEX=2 DM_NR_PIPE=2 DM_NR_SH=2`;
палуба, 60 с после загрузки; force-stop. Ждём:
- 0 строк `【红】[nr] sh name check` (ортографический LinearizeDepth больше не противоречие); `[nr] sh LinearizeDepth check: ... proven bit for bit`
  и `sh LinearizeDepth pass ... the verified pass` как в B2.
- в начале `[nb] post: <shaders>/native-post.xgspack, 7 fused bundles` (теперь из потока прогрева) и
  `[nb] post: warm-up record stage F1.91eb2d97a304709a composed (...)`, `... F1A.91eb2d97a304709a composed (...)`;
  `[nr] DM_NR_PIPE=2 (...): 1241 warm records, ... (4 stages are our fused post bundles: ...)`.
- `[nr] pipe (list done, ...)`: `built ... 1241 in list order, failed 0` (было 1237 / 3..4); `NVN PERF ... warm_failed=0`; на первом
  сплавленном кадре `created=0` (было 1) — draw берёт готовый конвейер.
- 0 строк `【红】[nr] pipe check`; в `[nr] pipe (10 s ...)` `missed ... failed 0`.
- По желанию отдельно `DM_NR_PIPE=1` / `3`: строки `hinted before` растут на 4 записи после загрузки палубы (EoCSight).

## Риски
- `warm_bundle` повторяет разбор `prepare()` — теперь это одна функция `compose`, так что разойтись они не могут; бандл F1A прогрева
  собирается при тех же env, что и у рендера (обе стороны читают `DM_NB_POST*` из окружения, задаваемого до старта движка).
- `pack_get` раньше открывался лениво на первом сплавленном draw, теперь — на прогреве (лишний `open` + `pread` индекса, ~0 мс), а строка
  `[nb] post: ... fused bundles` появляется раньше и с потока прогрева.
- Выигрыш: на Mi 14 / Adreno 750 с тёплым кэшем драйвера сплавленный конвейер в кадре стоил 0.4 мс (`create_ms=0.4`) — главное здесь
  правильность (прогрев перестаёт падать на 4 записях, нет ложной красной строки, lazy `=3` больше не оставляет их в кадре). На слабых
  телефонах / Mali / PowerVR с холодным кэшем драйвера (первый запуск, обновление драйвера) — один-два конвейера полноэкранного post
  меньше в кадре при первом выходе в мир (десятки мс одного кадра).
