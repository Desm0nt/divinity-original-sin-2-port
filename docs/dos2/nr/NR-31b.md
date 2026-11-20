# NR-31b — сид прогрева под текущие умолчания DOS2 (формы записей NB / NR-04 / NR-08 / NR-08b / safe mode)

Ветка `dos2-nr31b` (worktree `wt-nr31b`, база `dos2-nr-all` 795a5fb). Продолжение NR-31 (`docs/dos2/nr/NR-31.md`). Телефон не трогался.

## 0. Что было не так
`build` на списке телефона (`work/seed/vk-pipeline-list.bin`, 1923 записи, 17.11 22:50) давал `929 records of 1923; left out: dependencies 819,
layout 164, key 11` и `limit is 15..?`. Разбор по записям (политика каждой — по её зависимостям прохода, раскладке UBO против таблиц бандлов,
логам прогонов из `src/work/logs/*.env`):

| Форма | Записей | Откуда | Чего не понимал сид |
|---|---|---|---|
| зависимости тайлера (`dst_stage` 0x780 / 0x1780) | 825 | прогоны `DM_GFX_SIM_GPU=mali` / `powervr` на том же Mi 14 (nr9 / nr10 / nr15 / nr12 / nr14) | одна политика источника на весь список |
| раскладка: все UBO статические, Adreno-зависимости | 118 | safe-mode лестница `nr14_ladder` (SIM powervr), шаги 2 / 3: `TILE_BARRIER=0 UBO_DYNAMIC=0` | то же |
| раскладка: 8 из N динамических | 42 + 5 | `ubdyn8` (nr7_c5*, nr11_dir2m): лимит 8, NR-08 split (mode 3) | лимит / split |
| раскладка NR-08b (read-only: динамические только читаемые per-draw) | 100+ | SIM powervr с `DM_NR_UBODYN=1` (теперь умолчание PowerVR) | правило ro |
| ключ `F1.91eb2d97a304709a`, `F1A.…` | 11 | NB-18b `DM_NB_POST` / `_AA` (fused post) | ключ не hex, бандл — в `native-post.xgspack` |

Плюс в рантайме: `nrseed_policy()` ставил `unmodelled=1` при `DM_NR_DESC=1`, а это теперь умолчание DOS2 из `jni_android.c` — **с сидом в
APK любое устройство писало бы `this device runs NR-08 … not used`**, т.е. сид бы не работал нигде. NR-04 `DM_NR_SH=1` форм записей не
меняет (ключи те же; `xpack::read` отдаёт заменённый бандл с той же таблицей привязок — NR-04 сам отказывает при расхождении дескрипторов).

## 1. Что сделано
- **`tools/dos2_pipe_seed.py build`**: `--log` принимает несколько dm.log; первый — политика источника сида (Mi 14, все текущие умолчания:
  `nr13_f6a`), остальные — политики, под которыми ещё писался список; лог с несколькими загрузками (safe-mode лестница) даёт политику на
  каждую загрузку. Каждая запись сопоставляется политике по её зависимостям и **переводится в политику источника**: зависимости, раскладка
  (выводится заново из бандлов по модели — какие UBO были динамическими, решает правило устройства, а не игра; проверяется, что записанная
  раскладка = привязки бандлов в их порядке с UBO static/dynamic), вариант VS (`#vi`), маски NOOUT для FS с выходами (выходы — из SPIR-V
  бандла, как `xbg_spirv_raster_facts`), ids модулей; чего устройство-запись не могло записать (нет `fillModeNonSolid` / `depthBounds`) —
  как записано (0; ни одна родная запись Mi 14 их не использует). Дедуп по результату: одна отрисовка, записанная под двумя политиками, —
  одна запись (`duplicate (re-targeted)`). NOOUT-запись FS без выходов (маски игры потеряны) остаётся только если та же отрисовка есть с
  масками (тогда дубль); иначе `noout` (в этом списке таких 0). `--post-pack` — fused бандлы (`F1.` / `F1A.` / `F1AS.` / `F1AR.` / `F2.`)
  с таблицей привязок, переписанной как `compose()`. В конце самопроверка: каждая запись сида имеет форму, зависимости и раскладку, которыми
  рантайм проверяет сид. `policy --log X [--boot N] [--words]`.
- **Ядро `nr_seed.{h,c}`**: `nsd_key_parse` — fused ключи (`NSD_TAG_FUSED`, base = весь ключ; только FS); `nsd_layout` моделирует оба
  правила `dyn_mode_for` приёмника: NR-08 split (mode 3, динамические блоки выбираются **по счётчикам изменений во время игры**) → новая
  причина отказа `NSD_E_SPLIT` (`split`, никогда не догадка) и NR-08b read-only (mode ≤ 2, динамический — только блок, который стадия читает;
  флаги чтения — `NSD_W_READS`, без них `facts`). Поля `split` / `ubo_ro` — только у приёмника, в заголовок сида не входят (слова те же 21).
- **Рантайм**: `shader_cb` берёт fused бандл через `nbpost::warm_bundle` (как прогрев; где устройство его не рисует — отказ `shader`),
  копирует `slot`, считает чтения UBO `nr_spv_binding_reads` по финальным словам (= `nrubd::mark`); `nrseed_policy()`: `ubo_ro=nrubd::ro`,
  `split` — предсказание `nrdesc::init` из тех же переключателей (`DM_NR_DESC=1`, `DM_NR_DESC_SPLIT`, `ubo_dynamic_on`), `unmodelled=0`;
  отказ сида целиком теперь только при `unmodelled` в заголовке источника. Строка запуска показывает `NR-08 split` / `NR-08b read-only`.
- Сторожа `source_structure_test.py` (блок NR-31): строки split / ro в `dyn_mode_for`, тело `split_pick` = `split_eligible`, предсказание
  `split_on` = `nrdesc::init`, скан `nrubd::mark` = `shader_cb`, `fused_name` = `nsd_key_parse`.

## 2. Сид (`work/seed/dos2-pipe-seed.bin`, копия `dm/android/dos2/dos2-pipe-seed.bin`)
```
python tools/dos2_pipe_seed.py build --list work/seed/vk-pipeline-list.bin --log src/work/logs/{nr13_f6a,nr15_g4b,nr12_pvr,nr12_pvr_nr08b,nr11_dir2m,nr7_c5a,nr14_ladder}.dm.log \
    --bundles work/shaders-vk --post-pack work/shaders-native-post-a/fused.xgspack --out work/seed/dos2-pipe-seed.bin
-> 931 records of 1923 (785530 bytes, id 9d2381cd2e92b72d); left out: duplicate (re-targeted) 992
   re-targeted: layouts re-derived 681, fields the recording device could not record 825, NOOUT masks unknown 118 (all resolved as duplicates)
   self-check: 931 of 931 kept records have the shape, dependencies and layout the runtime checks a seed with
audit -> header 116 + 931 records: state 628112, counts 74504, keys 29838, formats 19444, framing 18620, module ids 14896; every byte accounted for: True
```
sha256 `c050a328ede5a84731009d1b6b8c6d1ae2cae00a011527e4d58b93f46ecd8747`. 929 уникальных состояний палубы + 2 fused (F1 / F1A); все SIM-записи
оказались дублями родных (та же сцена). **Содержимое**: ключи шейдеров (16 hex хэша; `#at` вариант; префикс fused `F1A.`), id модулей (FNV
ключей), числа фиксированного конвейера Vulkan, заголовок с 21 числом политики — ни кода шейдеров, ни SPIR-V, ни байтов игры (`audit`
разбирает каждое слово). По §4 NR-31 это данные порта → положен в APK-дерево.

## 3. Проверки (ПК)
- `nr_seed_test.c` (clang, `-Werror`, + `-DNSD_TEST_VK` с `vulkan_core.h`): PASS. Модель: 720 000 пар (источник 4 политики × приёмник 6:
  + `mali+split`, `powervr+ro`), 0 неверных; split-состояния 5 472 — все отказаны; seed Mi 14 → `powervr+ro` 4880/5000 + split 120 = все;
  fused ключи (разбор, перенос с ключом, отказ на VS); ro без флагов чтения → `facts` 300/300; фазз, файл сида — как в NR-31.
- `nr_seed_tool_test.py --tl2 --dos2`: PASS. Синтетика, TL2 (как в NR-31: 293/293), **DOS2**: build оставляет только дубли, audit, затем
  `--real-seed`: сид → 10 политик прогонов (Mi 14, SIM mali, powervr, powervr NR-08b, ubdyn8 ×2, 4 шага лестницы):
  Mi 14 — `expanded 931, of them a record of the list 931`; **из 1923 записей списка 1918 возвращаются байт в байт** под своей политикой,
  5 — NR-08 split ubdyn8 (`refused split 13` у приёмника ubdyn8: раскладку выбирают счётчики во время игры), `other words 0`.
- `source_structure_test.py`: PASS (включая новые сторожа). Набор `run_tests.sh` (копия, на worktree, с `nr_ubodyn.c` и `--dos2`).
- `run_tests.sh` (копия на worktree, `nr_seed_test` с `nr_ubodyn.c`, `nr_seed_tool_test --tl2 --dos2`): **pass=69 fail=0**.
- Сборка: `cmake -S dm/android -B work/build-android … -DDM_GAME=dos2` (libadrenotools и `work/drivers` скопированы из `wt-nrall`, ничего не
  скачивалось) + `tools/build_apk.ps1`: rc 0, `work/DOS2-port-debug.apk` 55 224 509 Б, sha256 `2a3f550a04f40b3be47ae4c2be0bec9269750242e5c20f671a614b929fe8b651`,
  ассет `assets/dos2-pipe-seed.bin` 785 530 Б внутри. Предупреждений в изменённых файлах нет (NDK 29, флаги проекта). Не ставился.

## 4. Риски
- Сид теперь в APK, а `DM_NR_SEED` по умолчанию 1: первый запуск каждого устройства будет строить записи сида в фоне. Подтверждение — §5.
- Предсказание `split` при `DM_NR_DESC=1` без кольца констант (nrdesc сам выключится) лишь отказывает лишние ~1–2 % записей на лимите 8.
- Чтения UBO для NR-08b рантайм считает по финальным словам (как `nrubd::mark`); ПК-тест — по SPIR-V бандла: совпало на всех 258 записях
  NR-08b из списка.
- Маски NOOUT-записей без выходов разрешены дублями; если SIM-прогон видел отрисовку, которой нет в родных, она бы ушла в `noout` (здесь 0).
- Прогоны, которых нет в `--log`, дадут `dependencies`/неверную раскладку — инструмент назовёт: «add the dm.log of the run that recorded it».

## 5. Прогоны на телефоне (бэкап `files/save`, keyguard, force-stop после; 60 с от загрузки уровня)
- **G5b, тождество Mi 14**: APK этой ветки (сид внутри), `DM_NR_SEED=2`. Ждём `[nr] DM_NR_SEED=2 (NR-31: check …): seed 9d2381cd2e92b72d,
  931 records, from a device with tile 0 … dynamic UBO 1 (limit 16) … features 0x1f; this device: tile 0 … (limit 16, NR-08 split) … features 0x1f`,
  `[nr] seed (done …): 931 records: expanded 931, refused none`, к концу: `the seed had P` ≈ все сборки в кадре, `a different record for the
  same shaders 0`, ни одной `【红】[nr] seed check`.
- **G5c, Mali-класс**: `DM_GFX_SIM_GPU=mali,ubdyn8 DM_NR_SEED=2`. Ждём `this device: tile 1 … dynamic UBO 1 (limit 8, NR-08 split), NOOUT 1 …
  features 0x16`, `refused split N` (N ≈ 13, только split), `a different record for the same shaders 0`.
- **G5d, первый запуск, медленный компилятор**: `DM_GFX_SIM_GPU=mali,ubdyn8 DM_GFX_PIPE_CACHE_LOAD=0 DM_NR_PIPE_THREADS=1`, список и маркер
  отодвинуть (`mv vk-pipeline-list.bin{,.bak}`, `.seed`; вернуть!), A `DM_NR_SEED=0` / B `DM_NR_SEED=1`, ABBA. Решают `NVN PERF … created /
  create_ms / create_max_ms` у входа в игру (B ≪ A), `[nr] seed (done …): built B, taken by draws K`, `[nr] seed 9d2381cd2e92b72d is in this
  device's list now`; второй запуск B — `already expanded … nothing to do`.
